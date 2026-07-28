#pragma once
#include <JuceHeader.h>
#include "PluginProcessor.h"
#include <memory>

class ToreiEQAudioProcessorEditor : public juce::AudioProcessorEditor
{
public:
    explicit ToreiEQAudioProcessorEditor(ToreiEQAudioProcessor&);
    ~ToreiEQAudioProcessorEditor() override;

    void resized() override;

private:
    ToreiEQAudioProcessor& processorRef;
    std::unique_ptr<juce::WebBrowserComponent> webView;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ToreiEQAudioProcessorEditor)
};
