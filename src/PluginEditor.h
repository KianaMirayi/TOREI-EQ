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
    juce::uint32 lastSoloLogMs    = 0;           // SOLO line throttle (§26)

    // `ENGINE after setParam` is deferred to the timer and throttled (§15.2). The flag
    // stays set until it has been emitted, so one final line always lands once a drag
    // stops -- the settled state is never lost.
    bool         pendingEngineLog = false;
    juce::uint32 lastEngineLogMs  = 0;
    static constexpr juce::uint32 kEngineLogIntervalMs = 500;
#endif

    bool pageLoaded = false;

    // --- 曲线推送去重（修复，非诊断；WEBVIEW2_MULTI_INSTANCE_FREEZE_HANDOFF.md §10）---
    // 曲线的唯一输入是频段参数 + 采样率，引擎把它们的变化汇总成一个版本号
    // （EqEngine::getCurveRevision()）。版本号没变 = 曲线没变，于是整组 512 点曲线
    // 既不重算也不重推。这是 40 Hz 定时器里最贵的一段，也是两个实例同时可见时灌进
    // WebView2 IPC 的主要流量 —— 而冻结恰恰发生在 UI 空闲、无人操作的时候，
    // 也就是"每个 tick 都在重推 5 条一模一样的曲线"的时候。
    //
    // 不变量：只有事件确实发出去之后，才把版本号记成"已推送"。JUCE 的
    // emitEventIfBrowserIsVisible 在组件不可见时一个字节都不发，若此时就记成已推送，
    // 窗口重新可见后 UI 会永远停在旧曲线上。
    juce::uint32      lastCurveRevision = 0;
    std::atomic<bool> curvePushPending  { true };   // 下一个可见 tick 必须整组重推
    int               curveRefreshTick  = 0;        // 安全刷新计时（漏 bump 的兜底）

    // 推送模式（诊断实验的开关，但门控逻辑本身是产品的一部分）：
    //   0 = 全推（默认）  1 = 只推电平/监听  2 = 频谱半速
    // 编译期默认值在 EqConfig.h；运行时用 push_mode.txt 覆盖的那条路径**只在诊断构建
    // （TOREI_EQ_DEBUG_LOG=1）里编译**，产品构建永远用编译期默认值。
    int pushMode = TOREI_DIAG_PUSH_MODE;
    int pushTick = 0;                     // pushMode==2 的频谱节流计数

    // --- "慢 tick"告警（唯一常开的诊断测量）-----------------------------------------
    // 这个 bug 的性质是"消息线程占用率"，把可观测性整段删掉的话，下次复发又只能靠猜。
    // 所以保留一个极轻的常开判据：只有某次 tick 超过阈值才写一行（带节流）。
    // 健康运行时几乎永不触发；触发时给出的正是"我们占了多少毫秒"这个关键量。
    // 关掉它只需把 PluginEditor.cpp 里的 kSlowTickAlarmMs 设为 0。
    juce::uint32 lastSlowTickLogMs = 0;

#if TOREI_EQ_DEBUG_LOG
    // --- 以下全是诊断（默认编译掉；WEBVIEW2_MULTI_INSTANCE_FREEZE_HANDOFF.md §13.5）--
    //   uiBootSeen       —— 页面自己的启动探针（UI_Log "ui-boot"）是否已收到。它比
    //                       "导航完成"更可靠；监听器可能在 WebView2 回调线程上跑，故用 atomic。
    //   firstPushLogged  —— 闸门第一次放行推送时打一行（一次性）。
    //   diagTick/阶段计数 —— 喂 TICK 心跳行；成员而非函数内 static，否则两个实例互相污染。
    //   tickMsTotal/...  —— 心跳窗口（40 tick）内的耗时统计与分阶段分解。
    std::atomic<bool> uiBootSeen { false };
    bool firstPushLogged = false;

    double tickMsTotal = 0.0;
    double tickMsMax   = 0.0;
    int    tickMsCount = 0;

    int diagTick      = 0;
    int stageLevel    = 0;
    int stageSpecPre  = 0;
    int stageSpecPost = 0;
    int stageCurves   = 0;
    int stageListen   = 0;
    int stageDone     = 0;

    // 上次心跳以来真正发出去的事件数 / 被去重跳过的事件数（写在 TICK 行里，
    // 是"曲线去重是否生效"的当场验收指标）。
    int diagEventsSent    = 0;
    int diagEventsSkipped = 0;

    double tickMsLevel = 0.0;   // 电平事件
    double tickMsPre   = 0.0;   // pre 频谱：FFT + 平滑 + payload + executeScript
    double tickMsPost  = 0.0;   // post 频谱
    double tickMsCurve = 0.0;   // 曲线段 + 监听事件（曲线被去重时这段≈0）
#endif

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ToreiEQAudioProcessorEditor)
};
