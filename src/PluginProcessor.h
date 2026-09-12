#pragma once

#include <JuceHeader.h>

#include "EqEngine.h"
#include "SpectrumAnalyzer.h"

#include <atomic>

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

    // Band state is serialised with a ValueTree/XML payload (see the .cpp).
    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    // Spectrum smoothing tuning parameters (exposed to the host for live tweaking).
    juce::AudioParameterFloat* spectrumAttack  = nullptr;  // 0.05..0.95
    juce::AudioParameterFloat* spectrumRelease = nullptr;  // 0.10..0.98
    juce::AudioParameterFloat* spectrumBlur    = nullptr;  // 0..5
    juce::AudioParameterFloat* spectrumDilate  = nullptr;  // 0..3
    juce::AudioParameterFloat* spectrumBand    = nullptr;  // 0.002..0.10 octave (band half-width)

    // Static EQ engine (biquad bands), shared with the editor.
    EqEngine& getEqEngine() { return eqEngine; }

    // Per-instance level meters (no cross-instance sharing).
    float getPeakDb() const { return peakDb.load (std::memory_order_relaxed); }
    float getRmsDb()  const { return rmsDb.load  (std::memory_order_relaxed); }

    // Per-instance spectrum analysis: two analysers owned by THIS processor, so
    // multiple plugin instances in one host never share state (no cross-talk).
    int readSpectrumPre  (float* dest, int destSize) { return analyzerPre.read  (dest, destSize); }
    int readSpectrumPost (float* dest, int destSize) { return analyzerPost.read (dest, destSize); }
    void setSpectrumSmoothing (float attack, float release, int blurRadius, int dilateRadius, float bandHalfOct)
    {
        analyzerPre.setSmoothing  (attack, release, blurRadius, dilateRadius, bandHalfOct);
        analyzerPost.setSmoothing (attack, release, blurRadius, dilateRadius, bandHalfOct);
    }

private:
    EqEngine eqEngine;

    // Per-instance spectrum analysers (pre = EQ input, post = EQ output).
    SpectrumAnalyzer analyzerPre;
    SpectrumAnalyzer analyzerPost;

    // Per-instance level meters (written on the audio thread, read by the editor timer).
    std::atomic<float> peakDb { -90.0f };
    std::atomic<float> rmsDb  { -90.0f };

public:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ToreiEQAudioProcessor)
};
