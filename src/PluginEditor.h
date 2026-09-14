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

    // Pushes one curve group (Mid/Side/L/R). Shared by all EQ_Curve_Data* events.
    void pushCurve (const char* eventName, EqEngine::CurveGroup group);

    // M/S runtime diagnostics (MID_SIDE_HANDOFF.md §13). All of it runs on the message
    // thread; the audio thread only publishes atomics. Compiled only when
    // TOREI_EQ_DEBUG_LOG is 1 -- the M/S verification is complete (§18), so a normal
    // build produces none of this traffic (§19).
#if TOREI_EQ_DEBUG_LOG
    void logMsDiagnostics();
#endif

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

    // Curve log throttle. A member rather than a function-local static (a static would
    // be shared by every editor instance). This one-shot line is kept in normal builds.
    bool curveLoggedOnce = false;

#if TOREI_EQ_DEBUG_LOG
    // --- Diagnostic-only state (MID_SIDE_HANDOFF.md §13; gated per §19) ------------
    // Everything below exists solely to feed the temporary diagnostics, so it is
    // compiled out of a normal build along with the passes that fill it.
    int  curveDiagTick   = 0;

    // Peak dB / peak frequency of each curve group, indexed by EqEngine::CurveGroup.
    static constexpr int kNumCurveGroups = 5;
    float        curvePeakDb[kNumCurveGroups]   = {};
    float        curvePeakFreq[kNumCurveGroups] = {};

    int          pendingLanesBand = -1;          // band awaiting its post-change LANES log
    juce::uint32 pendingLanesAtMs = 0;
    juce::uint32 lastMsProbeMs    = 0;

    // `ENGINE after setParam` is deferred to the timer and throttled (§15.2). The flag
    // stays set until it has been emitted, so one final line always lands once a drag
    // stops -- the settled state is never lost.
    bool         pendingEngineLog = false;
    juce::uint32 lastEngineLogMs  = 0;
    static constexpr juce::uint32 kEngineLogIntervalMs = 500;
#endif

    bool pageLoaded = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ToreiEQAudioProcessorEditor)
};
