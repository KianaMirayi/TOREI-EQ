#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "SpectrumAnalyzer.h"
#include "EqLog.h"

#include <atomic>
#include <cmath>
#include <algorithm>

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
    // Tear down the per-instance spectrum analysers and their dsp::FFT during
    // normal shutdown, before static teardown. Standalone's deletePlugin() reaches
    // here via `processor = nullptr` without guaranteeing that releaseResources()
    // is called, so without this the FFT (held by the per-instance analyser)
    // survives until exit and trips JUCE's "leaked FFT" assertion.
    analyzerPre.reset();
    analyzerPost.reset();
}

void ToreiEQAudioProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    juce::ignoreUnused (sampleRate, samplesPerBlock);
    analyzerPre.prepare (sampleRate, samplesPerBlock);
    analyzerPost.prepare (sampleRate, samplesPerBlock);
    eqEngine.prepare (sampleRate, samplesPerBlock);
}

void ToreiEQAudioProcessor::releaseResources()
{
    analyzerPre.reset();
    analyzerPost.reset();
    eqEngine.reset();
}

void ToreiEQAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    // NOTE: the level meter is measured at the END of the chain (see below), not
    // here, so it reports the true OUTPUT level and follows the Output trim.

    // Feed the PRE (input) spectrum analyser (lock-free downmix + ring-buffer
    // write) BEFORE the EQ mutates the buffer, so it captures the incoming signal.
    analyzerPre.push (buffer.getArrayOfReadPointers(),
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

    // Feed the POST (output) spectrum analyser AFTER the EQ. The buffer is
    // processed in place, so this captures the EQ'd (post) signal. pre/post are
    // kept aligned by the same downmix and smoothing inside each analyser.
    analyzerPost.push (buffer.getArrayOfReadPointers(),
                       buffer.getNumChannels(),
                       buffer.getNumSamples());

    // Measure the level meter on the OUTPUT (channel 0), i.e. after the whole chain
    // including the Output trim. This makes the meter report what actually leaves the
    // plugin, so it tracks both the EQ and the Output control -- and it matches what
    // LISTEN_FEATURE_HANDOFF.md §7 already documented ("电平表 ... 在 EQ 之后采集").
    // Still allocation-free and lock-free, so it stays audio-thread safe.
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
        const float outPeakDb = std::max (floorDb, 20.0f * std::log10 (std::max (1e-9f, peak)));
        const float outRmsDb  = std::max (floorDb, 20.0f * std::log10 (std::max (1e-9f, std::sqrt (sumSquares / (float) n))));

        // Simple attack/release smoothing so the meter is readable.
        const auto update = [] (std::atomic<float>& v, float target)
        {
            const float cur = v.load (std::memory_order_relaxed);
            const float next = target > cur ? cur + (target - cur) * 0.4f
                                            : cur + (target - cur) * 0.08f;
            v.store (next, std::memory_order_relaxed);
        };

        update (this->peakDb, outPeakDb);
        update (this->rmsDb,  outRmsDb);
    }

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

void ToreiEQAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    // Serialise only the PERSISTED band parameters. `listen` and `soloLevel` are
    // transient audition state and are intentionally excluded (see
    // LISTEN_FEATURE_HANDOFF.md §7).
    juce::ValueTree root ("TOREI_EQ_STATE");
    root.setProperty ("version", 1, nullptr);

    // Output (trim) gain is a persisted user setting -- unlike listen/soloLevel.
    root.setProperty ("outputGain", (double) eqEngine.getOutputGainDb(), nullptr);

    EqEngine::BandInfo info[EqEngine::kMaxBands];
    const int n = eqEngine.getBandSnapshot (info, EqEngine::kMaxBands);

    for (int i = 0; i < n; ++i)
    {
        juce::ValueTree band ("BAND");
        band.setProperty ("index",  info[i].index, nullptr);
        band.setProperty ("type",   juce::String (EqEngine::typeToString (info[i].type)), nullptr);
        band.setProperty ("freq",   (double) info[i].freq, nullptr);
        band.setProperty ("gain",   (double) info[i].gain, nullptr);
        band.setProperty ("q",      (double) info[i].q, nullptr);
        band.setProperty ("bypass", info[i].bypass, nullptr);
        root.appendChild (band, nullptr);
    }

    if (auto xml = root.createXml())
        copyXmlToBinary (*xml, destData);
}

void ToreiEQAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    auto xml = getXmlFromBinary (data, sizeInBytes);

    if (xml == nullptr || ! xml->hasTagName ("TOREI_EQ_STATE"))
        return;

    auto root = juce::ValueTree::fromXml (*xml);

    if (! root.isValid())
        return;

    // Clear FIRST: slots absent from the incoming state would otherwise stay
    // occupied and blend with the restored bands.
    eqEngine.clearAllBands();

    for (int i = 0; i < root.getNumChildren(); ++i)
    {
        auto band = root.getChild (i);

        if (! band.hasType ("BAND"))
            continue;

        const int index = (int) band.getProperty ("index", -1);

        if (index < 0 || index >= EqEngine::kMaxBands)
            continue;

        eqEngine.addBand (index,
                          EqEngine::typeFromString (band.getProperty ("type").toString()),
                          (float) (double) band.getProperty ("freq", 1000.0),
                          (float) (double) band.getProperty ("gain", 0.0),
                          (float) (double) band.getProperty ("q", 1.0));

        if ((bool) band.getProperty ("bypass", false))
            eqEngine.setParam (index, "bypass", true);
    }

    // Output (trim) gain. Defaults to 0 dB for projects saved before this existed.
    eqEngine.setOutputGainDb ((float) (double) root.getProperty ("outputGain", 0.0));

    // Let the host refresh anything it derives from our state. Note the explicit
    // nonParameterStateChanged flag: the VST3 wrapper only marks the project dirty
    // when that is set.
    updateHostDisplay (juce::AudioProcessor::ChangeDetails().withNonParameterStateChanged (true));
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new ToreiEQAudioProcessor();
}
