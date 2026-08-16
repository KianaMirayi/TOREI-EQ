#pragma once
#include <JuceHeader.h>

// Minimal audio-receive confirmation: the audio thread only updates global atomic
// level meters (no heap allocation, no member access through `this`, so it stays
// safe even if the host calls processBlock after the processor is destroyed).
float getAudioPeakDb();
float getAudioRmsDb();

class ToreiEQAudioProcessor : public juce::AudioProcessor
{
public:
    ToreiEQAudioProcessor();
    ~ToreiEQAudioProcessor() override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "TOREI-EQ"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}

    void getStateInformation(juce::MemoryBlock&) override {}
    void setStateInformation(const void*, int) override {}

    // Spectrum smoothing tuning parameters (exposed to the host for live tweaking).
    juce::AudioParameterFloat* spectrumAttack  = nullptr;  // 0.05..0.95
    juce::AudioParameterFloat* spectrumRelease = nullptr;  // 0.10..0.98
    juce::AudioParameterFloat* spectrumBlur    = nullptr;  // 0..5
    juce::AudioParameterFloat* spectrumDilate  = nullptr;  // 0..3
    juce::AudioParameterFloat* spectrumBand    = nullptr;  // 0.002..0.10 octave (band half-width)

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ToreiEQAudioProcessor)
};
