#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "SpectrumAnalyzer.h"
#include "EqLog.h"

#include <atomic>
#include <cmath>
#include <algorithm>

namespace
{
    std::atomic<float> gPeakDb { -90.0f };
    std::atomic<float> gRmsDb  { -90.0f };
}

float getAudioPeakDb() { return gPeakDb.load (std::memory_order_relaxed); }
float getAudioRmsDb()  { return gRmsDb.load  (std::memory_order_relaxed); }

ToreiEQAudioProcessor::ToreiEQAudioProcessor()
    : juce::AudioProcessor(BusesProperties()
        .withInput("Input", juce::AudioChannelSet::stereo(), true)
        .withOutput("Output", juce::AudioChannelSet::stereo(), true))
{
    // Work around a JUCE startup race: forces the JUCE TimerThread to be created on
    // the main thread NOW, before any WASAPI device-change notification can fire
    // from a COM thread. Otherwise the WASAPI handler calls Timer::startTimer and
    // races the TimerThread singleton creation, hitting a null lock and crashing
    // (0xC0000005 in CriticalSection::enter). This is a plugin-level fix (no JUCE
    // change). The callback is a no-op.
    juce::Timer::callAfterDelay (0, [] {});

    // Spectrum smoothing tuning parameters.
    spectrumAttack  = new juce::AudioParameterFloat ("spectrumAttack",  "Spectrum Attack",  0.05f,  0.95f, 0.50f);
    spectrumRelease = new juce::AudioParameterFloat ("spectrumRelease", "Spectrum Release", 0.10f,  0.98f, 0.96f);
    spectrumBlur    = new juce::AudioParameterFloat ("spectrumBlur",    "Spectrum Blur",    0.0f,   5.0f,  0.0f);
    spectrumDilate  = new juce::AudioParameterFloat ("spectrumDilate",  "Spectrum Dilate",  0.0f,   3.0f,  0.0f);
    spectrumBand    = new juce::AudioParameterFloat ("spectrumBand",    "Spectrum Band",    0.002f, 0.10f, 0.02f);

    addParameter (spectrumAttack);
    addParameter (spectrumRelease);
    addParameter (spectrumBlur);
    addParameter (spectrumDilate);
    addParameter (spectrumBand);
}

ToreiEQAudioProcessor::~ToreiEQAudioProcessor()
{
    // Tear down the spectrum analyser and its dsp::FFT during normal shutdown,
    // before static teardown. Standalone's deletePlugin() reaches here via
    // `processor = nullptr` without guaranteeing that releaseResources() is
    // called, so without this the FFT (held by the process-lifetime global
    // gAnalyzer) survives until exit and trips JUCE's "leaked FFT" assertion.
    resetSpectrum();
}

void ToreiEQAudioProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    juce::ignoreUnused (sampleRate, samplesPerBlock);
    prepareSpectrum (sampleRate, samplesPerBlock);
    eqEngine.prepare (sampleRate, samplesPerBlock);
}

void ToreiEQAudioProcessor::releaseResources()
{
    resetSpectrum();
    eqEngine.reset();
}

void ToreiEQAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    // Measure the incoming signal on the left channel. This is intentionally the
    // only thing the audio thread does: no allocation, no `this` member access.
    if (buffer.getNumChannels() > 0 && buffer.getNumSamples() > 0)
    {
        const auto* data = buffer.getReadPointer (0);
        const auto n = buffer.getNumSamples();

        float peak = 0.0f;
        float sumSquares = 0.0f;

        for (int i = 0; i < n; ++i)
        {
            const float s = data[i];
            const float a = std::fabs (s);
            if (a > peak)
                peak = a;

            sumSquares += s * s;
        }

        constexpr float floorDb = -90.0f;
        const float peakDb = std::max (floorDb, 20.0f * std::log10 (std::max (1e-9f, peak)));
        const float rmsDb  = std::max (floorDb, 20.0f * std::log10 (std::max (1e-9f, std::sqrt (sumSquares / (float) n))));

        // Simple attack/release smoothing so the meter is readable.
        const auto update = [] (std::atomic<float>& v, float target)
        {
            const float cur = v.load (std::memory_order_relaxed);
            const float next = target > cur ? cur + (target - cur) * 0.4f
                                            : cur + (target - cur) * 0.08f;
            v.store (next, std::memory_order_relaxed);
        };

        update (gPeakDb, peakDb);
        update (gRmsDb,  rmsDb);
    }

    // Feed the spectrum analyser (lock-free downmix + ring-buffer write).
    pushAudioToSpectrum (buffer.getArrayOfReadPointers(),
                         buffer.getNumChannels(),
                         buffer.getNumSamples());

    // --- Temporary Phase-1 diagnostic: prove the EQ actually changes the audio.
    // Captures the pre-EQ sum, applies the EQ, then logs pre/post the FIRST time a
    // non-unity band is present AND real signal is flowing (so we catch audio, not
    // the silent startup). If a band stays boosting but the buffer stays silent for
    // a while, log PROC-SILENT (meaning the host feeds no signal to the plugin). ---
    static bool procLogged = false;
    static int  procBlocks  = 0;
    static int  silentBlocks = 0;
    const auto blockSum = [] (const juce::AudioBuffer<float>& buf)
    {
        float s = 0.0f;
        for (int ch = 0; ch < buf.getNumChannels(); ++ch)
            for (int n = 0; n < buf.getNumSamples(); ++n)
                s += std::fabs (buf.getSample (ch, n));
        return s;
    };

    float preSum = 0.0f, preS0 = 0.0f;
    if (! procLogged && buffer.getNumChannels() > 0 && buffer.getNumSamples() > 0)
    {
        preSum = blockSum (buffer);
        preS0  = buffer.getSample (0, 0);
    }

    // Apply the static EQ (biquad bands).
    eqEngine.process (buffer);

    ++procBlocks;
    if (! procLogged && eqEngine.hasNonUnityBand())
    {
        if (preSum > 0.1f)
        {
            procLogged = true;
            const float postSum = blockSum (buffer);

            logEq ("PROC  block=" + juce::String (procBlocks)
                   + "  preSum=" + juce::String (preSum)
                   + "  postSum=" + juce::String (postSum)
                   + "  dSum=" + juce::String (postSum - preSum)
                   + "  preS0=" + juce::String (preS0)
                   + "  postS0=" + juce::String (buffer.getSample (0, 0))
                   + "   [engine] " + eqEngine.describe());
        }
        else if (++silentBlocks == 200)
        {
            // A band is boosting but the plugin keeps receiving silence.
            logEq (juce::String ("PROC-SILENT  hasNonUnityBand but preSum=0 for 200 blocks")
                   + "  -> the host is NOT feeding audio to the plugin"
                   + "   [engine] " + eqEngine.describe());
        }
    }
}

juce::AudioProcessorEditor* ToreiEQAudioProcessor::createEditor()
{
    return new ToreiEQAudioProcessorEditor(*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new ToreiEQAudioProcessor();
}
