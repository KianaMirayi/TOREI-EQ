#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "SpectrumAnalyzer.h"

#include <atomic>
#include <cmath>

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

ToreiEQAudioProcessor::~ToreiEQAudioProcessor() {}

void ToreiEQAudioProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    juce::ignoreUnused (sampleRate, samplesPerBlock);
    prepareSpectrum (sampleRate, samplesPerBlock);
}

void ToreiEQAudioProcessor::releaseResources()
{
    resetSpectrum();
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

    // Passthrough.
}

juce::AudioProcessorEditor* ToreiEQAudioProcessor::createEditor()
{
    return new ToreiEQAudioProcessorEditor(*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new ToreiEQAudioProcessor();
}
