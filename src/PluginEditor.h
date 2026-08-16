#pragma once
#include <JuceHeader.h>
#include "PluginProcessor.h"
#include <memory>
#include <functional>

/** WebBrowserComponent subclass that exposes the page-finished-loading event. */
class ToreiWebView : public juce::WebBrowserComponent
{
public:
    using WebBrowserComponent::WebBrowserComponent;

    std::function<void()> onPageLoaded;

    void pageFinishedLoading(const juce::String&) override
    {
        if (onPageLoaded)
            onPageLoaded();
    }
};

class ToreiEQAudioProcessorEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit ToreiEQAudioProcessorEditor(ToreiEQAudioProcessor&);
    ~ToreiEQAudioProcessorEditor() override;

    void resized() override;
    void visibilityChanged() override;

private:
    void timerCallback() override;
    void ensureWebView();

    ToreiEQAudioProcessor& processorRef;
    std::unique_ptr<ToreiWebView> webView;

    juce::HeapBlock<float> spectrumScratch;   // allocated lazily (kSpectrumBinCount)
    juce::Array<juce::var> spectrumPayload;   // reused across frames

    bool pageLoaded = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ToreiEQAudioProcessorEditor)
};
