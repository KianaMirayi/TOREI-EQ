#include "PluginProcessor.h"
#include "PluginEditor.h"

ToreiEQAudioProcessor::ToreiEQAudioProcessor()
    : juce::AudioProcessor(BusesProperties()
        .withInput("Input", juce::AudioChannelSet::stereo(), true)
        .withOutput("Output", juce::AudioChannelSet::stereo(), true))
{
}

ToreiEQAudioProcessor::~ToreiEQAudioProcessor() {}

void ToreiEQAudioProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    juce::ignoreUnused(sampleRate, samplesPerBlock);
}

void ToreiEQAudioProcessor::releaseResources() {}

void ToreiEQAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    // Passthrough
}

juce::AudioProcessorEditor* ToreiEQAudioProcessor::createEditor()
{
    return new ToreiEQAudioProcessorEditor(*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new ToreiEQAudioProcessor();
}
