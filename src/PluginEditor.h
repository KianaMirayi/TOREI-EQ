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
    void handleParameterChange (const juce::var& object);
    void handleCommand (const juce::var& object);

    // Pushes the full band list to the UI on request (state mirroring: C++ owns the
    // state, the UI renders it).
    void pushBandState();

    // Pushes the persisted output (trim) gain so the UI can restore it on window
    // reopen. Sent alongside Band_State, but as an independent event.
    void pushOutputState();

    // Tells the host the project is modified. Must pass
    // nonParameterStateChanged = true: a bare updateHostDisplay() sends an all-false
    // ChangeDetails, which the VST3 wrapper collapses to a no-op (no setDirty).
    void notifyHostStateChanged();

    ToreiEQAudioProcessor& processorRef;
    std::unique_ptr<ToreiWebView> webView;

    juce::HeapBlock<float> spectrumScratch;   // allocated lazily (kSpectrumBinCount)
    juce::Array<juce::var> spectrumPayload;   // reused across frames

    juce::HeapBlock<float> spectrumPostScratch;  // POST (output) spectrum
    juce::Array<juce::var> spectrumPostPayload;  // reused across frames

    juce::HeapBlock<float> curveScratch;      // EqEngine::kCurvePoints floats
    juce::Array<juce::var> curvePayload;      // reused across frames

    // Listen state pushed back to the UI (so it can re-sync when the DSP clears
    // listen on its own, e.g. the listened band was removed). Deliberately a
    // MEMBER, not a function-local static: a static would be shared by every editor
    // instance and re-introduce exactly the multi-instance cross-talk we just fixed.
    juce::DynamicObject::Ptr listenStateObj;   // reused across frames
    int lastPushedListenIndex = -2;            // -2 = nothing pushed yet

    juce::DynamicObject::Ptr outputStateObj;   // reused across frames (Output_State)

    bool pageLoaded = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ToreiEQAudioProcessorEditor)
};
