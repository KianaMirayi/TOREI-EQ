#pragma once
#include <JuceHeader.h>
#include "PluginProcessor.h"
#include <memory>
#include <functional>
#include <atomic>

/** WebBrowserComponent subclass that exposes the page-finished-loading event. */
class ToreiWebView : public juce::WebBrowserComponent
{
public:
    using WebBrowserComponent::WebBrowserComponent;

    // NOTE: gets the finished navigation's URL. JUCE calls this for ANY completed
    // navigation -- including the WebView2 controller's initial about:blank document,
    // and the one that our own goToURL cancels (JUCE treats OPERATION_CANCELED as
    // success and calls it anyway). Keep the URL so a caller can tell them apart.
    std::function<void(const juce::String&)> onPageLoaded;

    void pageFinishedLoading(const juce::String& url) override
    {
        if (onPageLoaded)
            onPageLoaded (url);
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
    // 窗口尺寸持久化：把当前尺寸写回 editor_size.txt（构造时读回、用户拖动后写回）
    void writeEditorSize();
    bool editorSizeDiffersEnough() const;               // 与上次写入相差 ≥2px（排除拖动中 ±1~3px 抖动）
    int    lastSavedEditorW = 0, lastSavedEditorH = 0;  // 上次写入（或构造时应用）的尺寸
    int    prevTickW = 0, prevTickH = 0;                // 上一 tick 的尺寸（稳定性 + "变化"判据）
    bool   userResizeArmed = false;                     // 检测到"用户拖动引起的尺寸变化"，等它稳定后写
    double editorCreatedMs = 0.0;                       // 构造时刻（避开宿主在打开瞬间摆窗口）
    double lastMouseDownSeenMs = -1.0e9;                // 最近一次观察到左键按下的时刻
    double lastEditorSizeWriteMs = -1.0e9;
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
    //------------
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
    juce::uint32 lastSoloLogMs    = 0;           // 

    // 
    // stays set until it has been emitted, so one final line always lands once a drag
    // stops -- the settled state is never lost.
    bool         pendingEngineLog = false;
    juce::uint32 lastEngineLogMs  = 0;
    static constexpr juce::uint32 kEngineLogIntervalMs = 500;
#endif

    bool pageLoaded = false;

    // 曲线去重：版本号没变就整组跳过；只有事件真发出去了才记"已推送"。
    juce::uint32      lastCurveRevision = 0;
    std::atomic<bool> curvePushPending  { true };   // 下一个可见 tick 必须整组重推
    int               curveRefreshTick  = 0;        // 安全刷新计时（漏 bump 的兜底）

    // 推送模式：0 全推 / 1 只推电平监听 / 2 频谱半速（push_mode.txt 覆盖仅诊断构建）。
    int pushMode = TOREI_DIAG_PUSH_MODE;
    int pushTick = 0;                     // pushMode==2 的频谱节流计数

    // 慢 tick 告警：某次 tick 超过 kSlowTickAlarmMs 才写一行（节流 5s）。设为 0 即关闭。
    juce::uint32 lastSlowTickLogMs = 0;

#if TOREI_EQ_DEBUG_LOG
    // 以下全是诊断，默认编译掉。
    std::atomic<bool> uiBootSeen { false };   // 页面 ui-boot 探针（可能在 WebView2 线程回调）
    bool firstPushLogged = false;

    double tickMsTotal = 0.0;   // 心跳窗口（40 tick）内的耗时统计与分阶段分解
    double tickMsMax   = 0.0;
    int    tickMsCount = 0;

    int diagTick      = 0;
    int stageLevel    = 0;
    int stageSpecPre  = 0;
    int stageSpecPost = 0;
    int stageCurves   = 0;
    int stageListen   = 0;
    int stageDone     = 0;

    // 上次心跳以来真正发出 / 被去重跳过的事件数（写进 TICK 行，用来验收去重是否生效）。
    int diagEventsSent    = 0;
    int diagEventsSkipped = 0;

    double tickMsLevel = 0.0;   // 电平事件
    double tickMsPre   = 0.0;   // pre 频谱：FFT + 平滑 + payload + executeScript
    double tickMsPost  = 0.0;   // post 频谱
    double tickMsCurve = 0.0;   // 曲线段 + 监听事件（曲线被去重时这段≈0）
#endif

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ToreiEQAudioProcessorEditor)
};
