#include "PluginProcessor.h"
#include "PluginEditor.h"

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
}

ToreiEQAudioProcessor::~ToreiEQAudioProcessor() {}

void ToreiEQAudioProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    juce::ignoreUnused (sampleRate, samplesPerBlock);
}

void ToreiEQAudioProcessor::releaseResources() {}

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
