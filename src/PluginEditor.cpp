#include "PluginEditor.h"
#include "SpectrumAnalyzer.h"
#include "EqLog.h"

#include <algorithm>
#include <cmath>

static juce::File findWebUI()
{
    const auto exeDir = juce::File::getSpecialLocation(
        juce::File::invokedExecutableFile).getParentDirectory();

    auto dir = exeDir;
    for (int i = 0; i < 8; ++i)
    {
        for (auto& sub : { "webui/index.html", "../webui/index.html",
                           "../../webui/index.html" })
        {
            auto f = dir.getChildFile(sub);
            if (f.existsAsFile()) return f;
        }
        dir = dir.getParentDirectory();
    }

    auto srcDir = juce::File(__FILE__).getParentDirectory().getParentDirectory();
    auto f = srcDir.getChildFile("webui").getChildFile("index.html");
    if (f.existsAsFile()) return f;

    return {};
}

// 诊断用：%APPDATA%\TOREI-EQ\push_mode.txt 里的整数（0/1/2）覆盖编译期的
// TOREI_DIAG_PUSH_MODE，这样切换推送模式不需要重编译，也不需要重启 DAW
// （编辑器每秒重读一次）。返回 -1 表示没有有效覆盖值。
//
// 只在诊断构建（TOREI_EQ_DEBUG_LOG=1）里编译：产品构建不该每秒去碰一次文件系统，
// 而这个运行时实验通道本来就是定位问题用的（WEBVIEW2_MULTI_INSTANCE_FREEZE_HANDOFF.md §13.5）。
#if TOREI_EQ_DEBUG_LOG
static int readPushModeOverride()
{
    const auto f = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                     .getChildFile ("TOREI-EQ")
                     .getChildFile ("push_mode.txt");

    if (! f.existsAsFile())
        return -1;

    const int v = f.loadFileAsString().trim().getIntValue();

    return (v >= 0 && v <= 2) ? v : -1;
}
#endif

// 曲线安全刷新间隔（单位：tick）。实测 tick 率约 21 Hz，所以 10 tick ≈ 0.5 s：
// 万一将来有人给曲线加了新的输入却忘了 bump 版本号（见 EqEngine::getCurveRevision()），
// 显示的曲线最多滞后这么久就会被强制纠正回来 —— 去重是"优化"，这条兜底保证它不会
// 变成"陈旧显示"这类正确性问题。
static constexpr int kCurveSafetyRefreshTicks = 10;

// "慢 tick"告警阈值（毫秒）。设为 0 即关闭告警。
// 修复前实测每 tick 18.6 ms/实例（两个实例就把宿主消息线程吃到饱和），修复后空闲
// 7.2 ms，Release 下应远低于此。25 ms 是"明显不正常"的量级：它触发就意味着占用率
// 问题回来了（或是宿主自己在忙别的）。只在超过阈值时写盘，健康运行时零开销。
static constexpr double kSlowTickAlarmMs = 25.0;

// 同一条告警的最小间隔，避免"每个 tick 都超阈值"时把日志写爆。
static constexpr juce::uint32 kSlowTickLogIntervalMs = 5000;

// 每 tick 的最小计时器：产品构建只统计总耗时（喂上面的"慢 tick"告警），诊断构建额外
// 记录分阶段时刻（喂 TICK 心跳行里的分解）。这样调用点不用写 #if：产品构建里
// markXxx() 是空函数，只留下 5 次高精度计数器读取（约 60 ns）。
struct TickTimer
{
    juce::int64 t0 = juce::Time::getHighResolutionTicks();

    static double ms (juce::int64 a, juce::int64 b)
    {
        return juce::Time::highResolutionTicksToSeconds (b - a) * 1000.0;
    }

#if TOREI_EQ_DEBUG_LOG
    juce::int64 tLevel = 0, tPre = 0, tPost = 0;
    void markLevel() { tLevel = juce::Time::getHighResolutionTicks(); }
    void markPre()   { tPre   = juce::Time::getHighResolutionTicks(); }
    void markPost()  { tPost  = juce::Time::getHighResolutionTicks(); }
#else
    void markLevel() {}
    void markPre()   {}
    void markPost()  {}
#endif
};

// 窗口尺寸持久化：%APPDATA%\TOREI-EQ\editor_size.txt，内容形如 "1500x900"。
// 宿主创建编辑器后会按编辑器上报的尺寸开窗，所以只要在构造时 setSize 到上次的尺寸，
// 下次打开就还是那个尺寸 —— 不需要宿主配合保存窗口位置。
static juce::File editorSizeFile()
{
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
             .getChildFile ("TOREI-EQ")
             .getChildFile ("editor_size.txt");
}

// 尺寸持久化的三个阈值（"只在用户真的拖了窗口时才写"靠它们 + 左键判据）
static constexpr int    kSizeMinDeltaPx = 2;       // 小于 2px 的变化视为噪声（拖动时真有 ±1~3px 抖动）
static constexpr double kSizeGraceMs    = 500.0;   // 打开后这段时间内一律不写（避开宿主创建编辑器时的 setSize）
static constexpr double kSizeThrottleMs = 500.0;   // 两次写盘最小间隔

// 当前显示器的工作区。构造时编辑器还没有 peer，所以按"鼠标所在显示器"取，取不到退回主显示器。
// 恢复尺寸时用它做二次夹制：在大屏设的尺寸挪到小屏上就不会溢出屏幕。
static juce::Rectangle<int> currentDisplayWorkArea()
{
    auto& displays = juce::Desktop::getInstance().getDisplays();

    if (auto* d = displays.getDisplayForPoint (juce::Desktop::getInstance().getMousePosition(), false))
        return d->userArea;

    if (auto* d = displays.getPrimaryDisplay())
        return d->userArea;

    return { 0, 0, 1920, 1080 };
}

ToreiEQAudioProcessorEditor::ToreiEQAudioProcessorEditor(ToreiEQAudioProcessor& p)
    : AudioProcessorEditor(&p)
    , processorRef(p)
{
    logEq ("=== editor session start ===");

    // Create the browser lazily on first visibility to avoid first-load crashes
    // in hosts whose message loop is not yet stable during editor creation.
    startTimerHz(40);

    setSize(1500, 1000);
    setResizable(true, true);
    setResizeLimits(900, 600, 3000, 2000);

    // 读回上次用户调整后的尺寸（文件缺失/非法就保留上面的默认值）。解析出的原始值必须先
    // 判正再夹到 setResizeLimits 的上下界里 —— 否则 "abc" 会被 getIntValue() 读成 0，
    // 再夹一下就变成一个合法的 900x600 把窗口改小。
    editorCreatedMs = juce::Time::getMillisecondCounterHiRes();

    {
        const auto txt = editorSizeFile().loadFileAsString().trim();
        const int xp = txt.indexOfChar ('x');

        if (xp > 0)
        {
            const int rw = txt.substring (0, xp).getIntValue();
            const int rh = txt.substring (xp + 1).getIntValue();

            if (rw > 0 && rh > 0)
            {
                // 第一层：setResizeLimits 的上下界；第二层：当前显示器工作区（大屏设的尺寸挪到小屏
                // 不会溢出屏幕），但不会低于我们的最小尺寸。
                const auto wa = currentDisplayWorkArea();

                const int w = juce::jlimit (900, juce::jmax (900, wa.getWidth()),  juce::jlimit (900, 3000, rw));
                const int h = juce::jlimit (600, juce::jmax (600, wa.getHeight()), juce::jlimit (600, 2000, rh));

                setSize (w, h);
            }
        }

        lastSavedEditorW = getWidth();
        lastSavedEditorH = getHeight();
        prevTickW = lastSavedEditorW;
        prevTickH = lastSavedEditorH;
    }
}

// 把当前窗口尺寸落盘。调用点已经做了判据（左键/稳定/≥2px/开场窗口），这里只管写。
void ToreiEQAudioProcessorEditor::writeEditorSize()
{
    lastEditorSizeWriteMs = juce::Time::getMillisecondCounterHiRes();
    lastSavedEditorW = getWidth();
    lastSavedEditorH = getHeight();

    auto f = editorSizeFile();
    f.getParentDirectory().createDirectory();
    f.replaceWithText (juce::String (lastSavedEditorW) + "x" + juce::String (lastSavedEditorH));
}

// 与上次写入（或构造时应用）的尺寸相差 ≥2px。拖动窗口时实测有 ±1~3px 的连续抖动，
// 用 int 直接比较会把噪声全部当成"变化"⇒ 每 500ms 一次无效写盘（还会污染常开的 SLOWTICK 判据）。
bool ToreiEQAudioProcessorEditor::editorSizeDiffersEnough() const
{
    const int dw = getWidth()  - lastSavedEditorW;
    const int dh = getHeight() - lastSavedEditorH;

    return (dw >= kSizeMinDeltaPx || dw <= -kSizeMinDeltaPx
         || dh >= kSizeMinDeltaPx || dh <= -kSizeMinDeltaPx);
}

void ToreiEQAudioProcessorEditor::ensureWebView()
{
    if (webView)
        return;

    auto winOpts = juce::WebBrowserComponent::Options::WinWebView2{};

    // Persistent user-data folder so a cleaned temp directory does not reset the
    // WebView2 runtime to a slow first-initialisation path.
    const auto userDataDir = juce::File::getSpecialLocation(
        juce::File::userApplicationDataDirectory).getChildFile("TOREI-EQ");
    userDataDir.createDirectory();
    winOpts = winOpts.withUserDataFolder(userDataDir.getChildFile("WebView2"));

    winOpts = winOpts.withStatusBarDisabled();
    winOpts = winOpts.withBuiltInErrorPageDisabled();
    winOpts = winOpts.withBackgroundColour(juce::Colour(0xff0b0c10));

    auto opts = juce::WebBrowserComponent::Options{};
    opts = opts.withBackend(juce::WebBrowserComponent::Options::Backend::webview2);
    opts = opts.withWinWebView2Options(winOpts);
    opts = opts.withNativeIntegrationEnabled();
    opts = opts.withKeepPageLoadedWhenBrowserIsHidden();

    // UI -> C++ events (emitted by the page via __JUCE__.backend.emitEvent).
    // These are invoked on the WebView2 callback thread, which can still be
    // delivering a message while the editor is being torn down. Guard against a
    // use-after-free with a SafePointer to the editor.
    auto safeSelf = juce::Component::SafePointer<ToreiEQAudioProcessorEditor>{this};
    opts = opts.withEventListener ("Parameter_Change",
                                  [safeSelf] (const juce::var& o)
                                  { if (safeSelf) safeSelf->handleParameterChange (o); });
    opts = opts.withEventListener ("Command",
                                  [safeSelf] (const juce::var& o)
                                  { if (safeSelf) safeSelf->handleCommand (o); });

    // Diagnostics for the UI->C++ bridge (temporary, for Phase 1 debugging).
    opts = opts.withEventListener ("UI_Ready", [safeSelf] (const juce::var& o)
                                  { logEq ("UI: " + (o.isObject() ? o["msg"].toString() : o.toString())); });
    opts = opts.withEventListener ("UI_Log",   [safeSelf] (const juce::var& o)
                                  {
#if TOREI_EQ_DEBUG_LOG
                                      // 只有诊断构建才需要"页面自己的启动探针"这个就绪判据。
                                      if (safeSelf) safeSelf->uiBootSeen = true;
#else
                                      juce::ignoreUnused (safeSelf);
#endif
                                      logEq ("UI: " + (o.isObject() ? o["msg"].toString() : o.toString()));
                                  });

    // Load the packaged UI via a custom-scheme ResourceProvider (torei://). This
    // serves the self-contained webui/index.html as a NORMAL page, so the app
    // script and the WebView2 bridge (window.__JUCE__ / chrome.webview) share the
    // same window context -- which is what makes the UI->C++ command queue reliable.
    // (The previous document.write injection isolated the app's window from the
    // bridge, which is exactly why the UI->C++ transport was unreliable.)
    // Load the packaged UI as a normal file:// page. A NORMAL page load (unlike
    // document.write) keeps the app script and the WebView2 bridge
    // (window.__JUCE__ / chrome.webview) in the same window context, so the
    // UI->C++ emitEvent path is reliable. (The torei:// custom-scheme approach did
    // NOT wire up chrome.webview, so the whole bridge was dead -- both directions.)
    auto htmlFile = findWebUI();
    juce::String uiUrl;
    if (htmlFile.existsAsFile())
        uiUrl = "file:///" + htmlFile.getFullPathName().replace ("\\", "/");
    else
        uiUrl = "about:blank";

    webView = std::make_unique<ToreiWebView>(opts);
    webView->onPageLoaded = [this] (const juce::String& url)
    {
#if TOREI_EQ_DEBUG_LOG
        // 诊断：JUCE 对"任何一次导航完成"都回调这里 —— 包括 WebView2 控制器创建时的初始
        // about:blank 文档，以及我们自己的 goToURL 打断它时以 OPERATION_CANCELED 上报的
        // 那一次（JUCE 把该错误码当作成功，照样回调）。所以这一行能直接看出 pageLoaded
        // 是否在我们真正的 UI 页面之前就被置位了。
        logEq ("PAGE finished  url=" + url
               + (pageLoaded ? "  (pageLoaded already true)" : "  -> pageLoaded=true"));
#else
        juce::ignoreUnused (url);
#endif
        pageLoaded = true;

        // A freshly loaded page has empty curve buffers of its own, so whatever we
        // think the UI already knows is worthless now: force a full curve re-push on
        // the next tick. Same reasoning as the !isVisible() case in timerCallback().
        curvePushPending = true;
    };
    webView->setBounds(getLocalBounds());
    addAndMakeVisible(webView.get());
    webView->goToURL (uiUrl);

    logEq ("webview created, listeners registered  load=file://");
}

void ToreiEQAudioProcessorEditor::visibilityChanged()
{
    if (isVisible())
        ensureWebView();

    Component::visibilityChanged();
}

ToreiEQAudioProcessorEditor::~ToreiEQAudioProcessorEditor()
{
    if (webView)
    {
        webView->onPageLoaded = nullptr;

        // Unload the page before the WebView2 is destroyed. The juce Windows
        // WebView2 teardown calls ICoreWebView2Controller::Close() (async) and
        // immediately releases the COM pointers; if a live React page is still being
        // torn down at that moment the heap gets corrupted (the editor's delete then
        // reports "wrote to memory after end of heap buffer"). Navigating to a blank
        // page first gives the browser a trivial page to destroy.
        webView->goToURL ("about:blank");
    }

    // 关窗前把最后尺寸落盘（拖完立刻关窗时，上面那个 tick 还没轮到）。判据同 timerCallback：
    // 只有"用户拖动引起的、还没写过的尺寸"才写 —— 写完会清掉 userResizeArmed，所以宿主在关窗
    // 时的程序化改动不会被误记。
    if (userResizeArmed
        && juce::Time::getMillisecondCounterHiRes() - editorCreatedMs > kSizeGraceMs
        && editorSizeDiffersEnough())
        writeEditorSize();

    stopTimer();
}

void ToreiEQAudioProcessorEditor::timerCallback()
{
    TickTimer tickTimer;

    ensureWebView();

    // 尺寸持久化：只认"用户在拖窗口"引起的尺寸变化（宿主自己摆窗口 / 恢复工程布局不算）——
    //   ① 尺寸**发生变化的那一刻**左键必须按下（或刚松开 400ms 内）⇒ 判定为用户在拖。
    //      注意：判定必须挂在"变化"上，不能只要求"左键按下过 + 尺寸与文件不同" ——
    //      否则"宿主程序化改了尺寸 + 用户随手点一下"也会被误记（实测抓到过）。
    //   ② 尺寸要连续两个 tick 不变（拖动过程中不写，避免每 500ms 一次无效写盘）
    //   ③ 与上次写入相差 ≥2px（实测拖动时真有 ±1~3px 抖动，int 直接比较会把噪声当变化）
    //   ④ 打开后 ≥500ms（避开宿主创建编辑器那一轮 setSize）
    {
        const int w = getWidth(), h = getHeight();
        const double now = juce::Time::getMillisecondCounterHiRes();

        if (juce::ModifierKeys::getCurrentModifiersRealtime().isLeftButtonDown())
            lastMouseDownSeenMs = now;

        if (w != prevTickW || h != prevTickH)
        {
            if (now - lastMouseDownSeenMs < 400.0)   // ① 变化发生在左键按下期间 / 刚松开
                userResizeArmed = true;

            prevTickW = w;
            prevTickH = h;
        }
        else if (userResizeArmed                        // 用户在拖，且尺寸已稳定
                 && editorSizeDiffersEnough()           // ③ ≥2px
                 && now - editorCreatedMs > kSizeGraceMs      // ④ 不在开场窗口内
                 && now - lastEditorSizeWriteMs > kSizeThrottleMs)
        {
            writeEditorSize();
            userResizeArmed = false;    // 写过了：之后宿主的程序化改动不会被误记
        }
    }

#if TOREI_EQ_DEBUG_LOG
    // --- 心跳（诊断，只在 TOREI_EQ_DEBUG_LOG=1 的构建里存在）------------------------
    // 每 40 tick 一行，带实例短标识，两个实例的行不会混。**注意它证明不了什么"卡死"**：
    // 实测冻结时两个实例的频谱都在继续变化，所以"心跳停了"只可能是进程被杀 —— 心跳的
    // 真正用途是给出"每 tick 耗时 / 事件数 / 阶段计数"这几个量（§13.1 的成本模型就是
    // 从它反推出来的）。占用率告警另见下面的 kSlowTickAlarmMs（那条是常开的）。
    if (++diagTick >= 40)
    {
        diagTick = 0;

        // 每秒重读一次推送模式覆盖文件（诊断用）
        const int ov = readPushModeOverride();
        if (ov >= 0)
            pushMode = ov;

        // 每 tick 耗时（上一次心跳窗口累计值）。avg 是"我们每 tick 占消息线程多少毫秒"，
        // sum 是窗口内合计 —— 与窗口时长（约 1850 ms）一比就是我们的占用率。
        juce::String msInfo;
        if (tickMsCount > 0)
        {
            const double n = (double) tickMsCount;

            msInfo = "  ms avg=" + juce::String (tickMsTotal / n, 2)
                   + " max="     + juce::String (tickMsMax, 2)
                   + " sum="     + juce::String (tickMsTotal, 0)
                   + " n="       + juce::String (tickMsCount)
                   + " | lvl="   + juce::String (tickMsLevel / n, 2)
                   + " pre="     + juce::String (tickMsPre / n, 2)
                   + " post="    + juce::String (tickMsPost / n, 2)
                   + " curve="   + juce::String (tickMsCurve / n, 2);

            tickMsTotal = tickMsLevel = tickMsPre = tickMsPost = tickMsCurve = 0.0;
            tickMsMax   = 0.0;
            tickMsCount = 0;
        }

        logEq ("TICK " + juce::String::toHexString ((juce::int64) this).substring (0, 6)
               + "  webView=" + juce::String (webView != nullptr ? 1 : 0)
               + " pageLoaded=" + juce::String (pageLoaded ? 1 : 0)
               + " uiBoot=" + juce::String (uiBootSeen.load() ? 1 : 0)
               + " push=" + juce::String (pushMode)
               + "  stages: level=" + juce::String (stageLevel)
               + " pre=" + juce::String (stageSpecPre)
               + " post=" + juce::String (stageSpecPost)
               + " curve=" + juce::String (stageCurves)
               + " listen=" + juce::String (stageListen)
               + " done=" + juce::String (stageDone)
               // 真正发出去的事件数 / 被去重跳过的事件数。空闲时 skip 应该接近
               // 5 x tick 数（5 条曲线全被去重），evts 只剩电平 + 两条频谱。
               + "  evts=" + juce::String (diagEventsSent)
               + " skip=" + juce::String (diagEventsSkipped)
               + msInfo);

        diagEventsSent    = 0;
        diagEventsSkipped = 0;
    }
#endif

    if (!webView || !pageLoaded)
        return;

    // 推送节流 / 推送量实验，模式来自 pushMode（编译期默认 + 运行时覆盖文件）：
    //   1 = 停掉"重"事件（两条频谱 + 5 条曲线），只留电平与监听这两条极小的事件；
    //   2 = 轻量推送（频谱每 2 tick，并去掉重复的旧曲线事件）；
    //   0 = 原样。
    // 曲线不再按 tick 节流：曲线去重（见下面的曲线段）已经把"没变化时的重复推送"降为
    // 零，比按 4 tick 抽样更好 —— 抽样只会把用户拖节点时的曲线更新额外延迟最多 4 个
    // tick（约 200 ms），而那正是用户最需要曲线跟手的时候。
    const bool pushSpectrum = (pushMode == 0) || (pushMode == 2 && (pushTick & 1) == 0);
    const bool pushCurves   = (pushMode == 0) || (pushMode == 2);
    ++pushTick;

    // 诊断（一次性）：闸门第一次放行推送。把它与上面的 "PAGE finished url=..." 行对时间戳
    // 比较即可定性 —— 若它出现在 url=about:blank 之后、而 file:/// 页面那行还在后面，
    // 就证明推送确实发生在我们真正的 UI 页面加载完成之前。
#if TOREI_EQ_DEBUG_LOG
    if (! firstPushLogged)
    {
        firstPushLogged = true;
        logEq ("GATE first push  uiBootSeen=" + juce::String (uiBootSeen.load() ? 1 : 0));
    }
#endif

    // Push the real incoming audio level to the frontend meter.
    juce::DynamicObject::Ptr obj = new juce::DynamicObject();
    obj->setProperty ("peak", processorRef.getPeakDb());
    obj->setProperty ("rms",  processorRef.getRmsDb());
    webView->emitEventIfBrowserIsVisible ("Audio_Level", obj.get());
#if TOREI_EQ_DEBUG_LOG
    ++diagEventsSent;
    ++stageLevel;
#endif
    tickTimer.markLevel();

    // Apply the host-tunable smoothing parameters before computing the spectrum.
    processorRef.setSpectrumSmoothing (processorRef.spectrumAttack->get(),
                                       processorRef.spectrumRelease->get(),
                                       (int) std::lround (processorRef.spectrumBlur->get()),
                                       (int) std::lround (processorRef.spectrumDilate->get()),
                                       processorRef.spectrumBand->get());

    // Push the smoothed log-spaced spectrum to the frontend (dBFS points).
    if (spectrumScratch == nullptr)
        spectrumScratch.calloc (kSpectrumPointCount);

    const int points = processorRef.readSpectrumPre (spectrumScratch.getData(), kSpectrumPointCount);

    if (pushSpectrum && points > 0)
    {
        spectrumPayload.clearQuick();
        spectrumPayload.ensureStorageAllocated (points);

        for (int i = 0; i < points; ++i)
        {
            // Round to 0.1 dB to keep the JSON payload compact.
            const float v = std::round (spectrumScratch[i] * 10.0f) * 0.1f;
            spectrumPayload.add (juce::var ((double) v));
        }

        webView->emitEventIfBrowserIsVisible ("Spectrum_Data", spectrumPayload);
#if TOREI_EQ_DEBUG_LOG
        ++diagEventsSent;
        ++stageSpecPre;
#endif
    }

    tickTimer.markPre();

    // Push the POST (output) spectrum on its own channel so the UI can overlay the
    // pre/post curves Pro-Q style. Same log-spaced layout and smoothing as pre.
    if (spectrumPostScratch == nullptr)
        spectrumPostScratch.calloc (kSpectrumPointCount);

    const int postPoints = processorRef.readSpectrumPost (spectrumPostScratch.getData(), kSpectrumPointCount);

    if (pushSpectrum && postPoints > 0)
    {
        spectrumPostPayload.clearQuick();
        spectrumPostPayload.ensureStorageAllocated (postPoints);

        for (int i = 0; i < postPoints; ++i)
        {
            const float v = std::round (spectrumPostScratch[i] * 10.0f) * 0.1f;
            spectrumPostPayload.add (juce::var ((double) v));
        }

        webView->emitEventIfBrowserIsVisible ("Spectrum_Data_Post", spectrumPostPayload);
#if TOREI_EQ_DEBUG_LOG
        ++diagEventsSent;
        ++stageSpecPost;
#endif
    }

    tickTimer.markPost();

    // Push the EQ magnitude response curves (dB, log-spaced 20 Hz..20 kHz).
    //
    // Four per-channel-mode groups (MID_SIDE_HANDOFF.md §5), each = stereo bands plus
    // the bands routed to that channel. `EQ_Curve_Data` is the legacy name and is sent
    // with the SAME data as `EQ_Curve_Data_Mid`: the UI binds both names to its Mid
    // slot, so keeping them identical makes the displayed result independent of which
    // arrives last (agreed as option 2-1).
    //
    // When no band is routed to mid/side the Mid and Side arrays are identical, and the
    // UI's own merge logic collapses them into a single line (acceptance §8.1).
    if (pushCurves)
    {
        // --- 曲线去重（WEBVIEW2_MULTI_INSTANCE_FREEZE_HANDOFF.md）------------------
        // 在此之前，每个 tick 都无条件重算 5 组 512 点曲线、构造 5 个 payload、发 5 个
        // 事件 —— 而曲线只在用户改参数时才会变。UI 空闲时这就是 100% 的纯冗余工作，
        // 两个实例同时可见时正好是灌爆 host 消息线程 / WebView2 IPC 的那部分流量。
        //
        // 现在：版本号没变就整段跳过（不重算、不构造、不发）。曲线一变（拖节点、
        // 切类型/M/S 模式、bypass、增删频段、宿主采样率变化）版本号就会变，立刻照旧推。
        const juce::uint32 curveRev = processorRef.getEqEngine().getCurveRevision();

        const bool refreshDue = (++curveRefreshTick >= kCurveSafetyRefreshTicks);
        if (refreshDue)
            curveRefreshTick = 0;

        if (! webView->isVisible())
        {
            // emitEventIfBrowserIsVisible 此时是空操作（JUCE 的可见性门控），所以现在
            // 算出来的曲线 UI 根本收不到。只标记"下次可见要整组重推"，绝不更新版本号。
            curvePushPending = true;
#if TOREI_EQ_DEBUG_LOG
            diagEventsSkipped += 5;
#endif
        }
        else if (curveRev != lastCurveRevision || curvePushPending.load() || refreshDue)
        {
            curvePushPending = false;
            lastCurveRevision = curveRev;

            // 模式 2 起：旧事件 EQ_Curve_Data 与 _Mid 数据完全相同（UI 把两个名字绑到
            // 同一个缓冲），去掉它能省掉 1/5 的曲线流量，UI 侧不需要改动。
            if (pushMode != 2)
                pushCurve ("EQ_Curve_Data",  EqEngine::curveMid);

            pushCurve ("EQ_Curve_Data_Mid",  EqEngine::curveMid);
            pushCurve ("EQ_Curve_Data_Side", EqEngine::curveSide);
            pushCurve ("EQ_Curve_Data_L",    EqEngine::curveLeft);
            pushCurve ("EQ_Curve_Data_R",    EqEngine::curveRight);

#if TOREI_EQ_DEBUG_LOG
            diagEventsSent += (pushMode != 2 ? 5 : 4);
#endif
        }
        else
        {
            // 曲线的唯一输入没变：整组跳过。这是空闲时省下的全部工作。
#if TOREI_EQ_DEBUG_LOG
            diagEventsSkipped += 5;
#endif
        }
    }
#if TOREI_EQ_DEBUG_LOG
    ++stageCurves;
#endif

    // Push the listen state (hold-to-listen target band, -1 = none) whenever it
    // changes. Change-detection rather than every frame: it keeps the bridge quiet
    // while still guaranteeing the "DSP cleared listen by itself" path is pushed
    // (that IS a change), which is the case the UI needs to stay in sync with.
    {
        const int listen = processorRef.getEqEngine().getListenIndex();

        if (listen != lastPushedListenIndex)
        {
            lastPushedListenIndex = listen;

            if (listenStateObj == nullptr)
                listenStateObj = new juce::DynamicObject();

            listenStateObj->setProperty ("index", listen);
            webView->emitEventIfBrowserIsVisible ("EQ_Listen_State", listenStateObj.get());
#if TOREI_EQ_DEBUG_LOG
            ++diagEventsSent;
#endif

            logEq ("SEND EQ_Listen_State  index=" + juce::String (listen));
        }
    }

#if TOREI_EQ_DEBUG_LOG
    ++stageListen;
#endif

    // Curve logging / diagnostics live in pushCurve() (it owns the scratch buffer).
#if TOREI_EQ_DEBUG_LOG
    ++stageDone;
#endif

    // --- 每 tick 耗时累计 + "慢 tick"告警 -------------------------------------------
    // 总耗时**永远**统计（喂告警）；分阶段耗时只在诊断构建里统计（喂心跳行的分解）。
    {
        const juce::int64 tEnd = juce::Time::getHighResolutionTicks();
        const double total = TickTimer::ms (tickTimer.t0, tEnd);

#if TOREI_EQ_DEBUG_LOG
        tickMsTotal += total;
        tickMsMax    = juce::jmax (tickMsMax, total);
        ++tickMsCount;

        tickMsLevel += TickTimer::ms (tickTimer.t0,      tickTimer.tLevel);   // 含 ensureWebView + 心跳行 + 闸门
        tickMsPre   += TickTimer::ms (tickTimer.tLevel,  tickTimer.tPre);     // pre：FFT + 平滑 + payload
        tickMsPost  += TickTimer::ms (tickTimer.tPre,    tickTimer.tPost);    // post：同上
        tickMsCurve += TickTimer::ms (tickTimer.tPost,   tEnd);              // 曲线段 + 监听事件
#endif

        // 常开的唯一告警：只在"某次 tick 明显超时"时写一行（带节流）。这个 bug 的性质是
        // 消息线程占用率，保留这条最小判据，复发时立刻能拿到"我们占了多少毫秒"。
        if (kSlowTickAlarmMs > 0.0 && total >= kSlowTickAlarmMs)
        {
            const juce::uint32 now = juce::Time::getMillisecondCounter();

            if (now - lastSlowTickLogMs >= kSlowTickLogIntervalMs)
            {
                lastSlowTickLogMs = now;

                logEq ("SLOWTICK " + juce::String (total, 2) + " ms"
                       + "  (threshold " + juce::String (kSlowTickAlarmMs, 1) + " ms)"
                       + "  push=" + juce::String (pushMode)
                       + "  webView=" + juce::String (webView != nullptr ? 1 : 0)
                       + " pageLoaded=" + juce::String (pageLoaded ? 1 : 0)
                       + "  -> 参见 WEBVIEW2_MULTI_INSTANCE_FREEZE_HANDOFF.md §13.1"
                       + " (成本 ≈ 0.09 ms/事件 + ≈4.5 µs/每个推送的数值)");
            }
        }
    }

#if TOREI_EQ_DEBUG_LOG
    logMsDiagnostics();
#endif
}

#if TOREI_EQ_DEBUG_LOG
void ToreiEQAudioProcessorEditor::logMsDiagnostics()
{
    EqEngine& engine = processorRef.getEqEngine();

    const juce::uint32 now = juce::Time::getMillisecondCounter();

    // (2) LANES -- once, ~300 ms after a mode change, so we report the CONVERGED ramp
    // values rather than a mid-transition snapshot.
    //
    // Deliberately NOT gated on "is anything still routed": the case that matters most
    // is the LAST band going back to stereo, which by definition leaves nothing routed.
    // Gating here would silently swallow exactly that line (§16.4). This is a one-shot
    // response to a mode-change event, not periodic traffic, so it needs no gate.
    if (pendingLanesBand >= 0 && now >= pendingLanesAtMs)
    {
        const int b = pendingLanesBand;
        pendingLanesBand = -1;

        const auto fmtMix = [] (float v)
        {
            return juce::String (v, 3);
        };

        logEq ("LANES band=" + juce::String (b)
               + " mode=" + juce::String (EqEngine::modeToString (engine.getBandMode (b)))
               + " cL=" + fmtMix (engine.getLaneMix (b, EqEngine::laneL))
               + " cR=" + fmtMix (engine.getLaneMix (b, EqEngine::laneR))
               + " cM=" + fmtMix (engine.getLaneMix (b, EqEngine::laneM))
               + " cS=" + fmtMix (engine.getLaneMix (b, EqEngine::laneS)));
    }

    // `ENGINE after setParam` is throttled (~500 ms) instead of being logged per change:
    // a node drag fires it at ~40 Hz and each line dumps every band's full state, which
    // is what took the log to 21 MB (§15.2). Throttling still reports the evolving values
    // and, because the pending flag survives, also lands one final line after the drag
    // stops -- so the settled state is never lost.
    if (pendingEngineLog && now - lastEngineLogMs >= kEngineLogIntervalMs)
    {
        pendingEngineLog = false;
        lastEngineLogMs  = now;

        logEq ("ENGINE after setParam  " + engine.describe());
    }

    // (2b) SOLO -- the solo stage's OWN routing ramps and audition output (§26).
    // Deliberately placed BEFORE the "is anything routed away from stereo" gate below:
    // the solo stage has its own ramps, and soloing a plain STEREO band must still be
    // reportable. Emitted only while actually listening, so it is silent otherwise.
    {
        const int listenBand = engine.getListenIndex();

        if (listenBand >= 0 && now - lastSoloLogMs >= kEngineLogIntervalMs)
        {
            lastSoloLogMs = now;

            const auto fmtW = [&engine] (int lane)
            {
                return juce::String (engine.getSoloWeight (lane), 3);   // converged value
            };

            logEq ("SOLO listen=b" + juce::String (listenBand)
                   + " mode=" + juce::String (EqEngine::modeToString (engine.getBandMode (listenBand)))
                   + "  w: L=" + fmtW (EqEngine::laneL)
                   + " R=" + fmtW (EqEngine::laneR)
                   + " M=" + fmtW (EqEngine::laneM)
                   + " S=" + fmtW (EqEngine::laneS)
                   + " | soloOut: L=" + juce::String (engine.getSoloOutDb (false), 1)
                   + " R=" + juce::String (engine.getSoloOutDb (true), 1)
                   + " corr=" + juce::String (engine.getSoloOutCorr(), 2));
        }
    }

    // Everything below is periodic M/S probing, which only means anything while a band
    // is actually routed away from stereo -- so the common all-stereo case stays silent
    // (§13.4).
    if (engine.getFirstNonStereoBand() < 0)
        return;

    if (now - lastMsProbeMs < 500)
        return;

    lastMsProbeMs = now;

    const auto fmtDb = [] (float v)
    {
        return juce::String (v, 1);
    };

    // (3) MSPROBE -- the numeric substitute for "the ear cannot tell".
    // NOTE: in/out are measured across the WHOLE EQ chain, not per band (measuring per
    // band would mean a second RMS pass for every band). With a single routed band --
    // the test case in §13.5 -- the chain in/out IS that band's effect.
    //
    // ALL routed bands are listed, not just the first: reporting only the first meant a
    // second routed band (e.g. a `side` band behind a `mid` one) never appeared at all,
    // and `side` is precisely the case that most needs numeric evidence (§16.3).
    int routed[EqEngine::kMaxBands];
    const int routedCount = engine.getRoutedBands (routed, EqEngine::kMaxBands);

    juce::String routedDesc;

    for (int i = 0; i < routedCount; ++i)
        routedDesc << (i > 0 ? " " : "")
                   << "band" << routed[i] << ":"
                   << EqEngine::modeToString (engine.getBandMode (routed[i]));

    logEq ("MSPROBE routed=[" + routedDesc + "]"
           + "  (whole chain)"
           + "  in: L=" + fmtDb (engine.getProbeDb (false, EqEngine::probeL))
           + " R="      + fmtDb (engine.getProbeDb (false, EqEngine::probeR))
           + " M="      + fmtDb (engine.getProbeDb (false, EqEngine::probeM))
           + " S="      + fmtDb (engine.getProbeDb (false, EqEngine::probeS))
           + " | out: L=" + fmtDb (engine.getProbeDb (true, EqEngine::probeL))
           + " R="        + fmtDb (engine.getProbeDb (true, EqEngine::probeR))
           + " M="        + fmtDb (engine.getProbeDb (true, EqEngine::probeM))
           + " S="        + fmtDb (engine.getProbeDb (true, EqEngine::probeS)));

    // (4) curve-group confirmation: the four groups must NOT all be the same array.
    const auto fmtPeak = [this] (EqEngine::CurveGroup g)
    {
        return "peak=" + juce::String (curvePeakDb[g] >= 0.0f ? "+" : "")
                        + juce::String (curvePeakDb[g], 1) + "dB@"
                        + juce::String (juce::roundToInt (curvePeakFreq[g])) + "Hz";
    };

    logEq (juce::String ("CURVE mid: ") + fmtPeak (EqEngine::curveMid)
           + "   side: " + fmtPeak (EqEngine::curveSide));
    logEq (juce::String ("CURVE L:   ") + fmtPeak (EqEngine::curveLeft)
           + "   R:    " + fmtPeak (EqEngine::curveRight));
}
#endif   // TOREI_EQ_DEBUG_LOG

void ToreiEQAudioProcessorEditor::pushCurve (const char* eventName, EqEngine::CurveGroup group)
{
    if (webView == nullptr)
        return;

    if (curveScratch == nullptr)
        curveScratch.calloc (EqEngine::kCurvePoints);

    const int curvePoints = processorRef.getEqEngine().getCurveGains (curveScratch.getData(),
                                                                      EqEngine::kCurvePoints,
                                                                      group);

    if (curvePoints <= 0)
        return;

    // Reuse the one payload buffer: the events are emitted back to back.
    curvePayload.clearQuick();
    curvePayload.ensureStorageAllocated (curvePoints);

    for (int i = 0; i < curvePoints; ++i)
    {
        // NOTE: 0.01 dB, not 0.1 dB. The curve is drawn through g2y(g) = 0.5 - g/60, so
        // 1 dB is H/60 px and a 0.1 dB quantum is H/600 px -- about 1.37 px on a ~820 px
        // tall display. That is a WHOLE PIXEL of vertical banding per data step, and it is
        // the real cause of the "staircase / sawtooth" curve (CURVE_STAIRSTEP_HANDOFF.md):
        // a low-gain bell spans only a handful of 0.1 dB levels, so consecutive samples
        // repeat, Catmull-Rom then has p0==p1==p2==p3 and produces LITERALLY flat runs --
        // which is why whole columns came out byte-identical. 0.01 dB is ~0.14 px, i.e.
        // sub-pixel and invisible, while keeping the JSON payload compact.
        curvePayload.add (juce::var ((double) std::round (curveScratch[i] * 100.0f) * 0.01f));
    }

    webView->emitEventIfBrowserIsVisible (eventName, curvePayload);

#if TOREI_EQ_DEBUG_LOG
    // Record this group's peak for the §13.3(4) curve-group log. All five events share
    // the same band set, so one frame's worth of peaks is logged together below.
    {
        int peakIndex = 0;

        for (int i = 1; i < curvePoints; ++i)
            if (curveScratch[i] > curveScratch[peakIndex])
                peakIndex = i;

        if (group >= 0 && group < kNumCurveGroups)
        {
            curvePeakDb[group]   = curveScratch[peakIndex];
            curvePeakFreq[group] = (float) (20.0 * std::pow (10.0,
                                       (double) peakIndex / (double) (curvePoints - 1) * 3.0));
        }
    }
#endif

    // Diagnostics run once per frame, on the Mid group only (all five events share the
    // same band set, so reporting one is enough). Members, not statics: a function-local
    // static would be shared by every editor instance.
    if (group == EqEngine::curveMid)
    {
        if (! curveLoggedOnce)
        {
            curveLoggedOnce = true;
            logEq ("SEND EQ_Curve_Data  points=" + juce::String (curvePoints)
                   + "  firstGain=" + juce::String (curveScratch[0]));
        }

#if TOREI_EQ_DEBUG_LOG
        if ((++curveDiagTick % 200) == 0)
        {
            float mx = curveScratch[0], mn = curveScratch[0];
            for (int i = 1; i < curvePoints; ++i)
            {
                mx = std::max (mx, curveScratch[i]);
                mn = std::min (mn, curveScratch[i]);
            }
            logEq ("CURVE  maxDb=" + juce::String (mx)
                   + "  minDb=" + juce::String (mn)
                   + "   [engine] " + processorRef.getEqEngine().describe());
        }
#endif
    }
}

void ToreiEQAudioProcessorEditor::handleParameterChange (const juce::var& object)
{
    if (auto* d = object.getDynamicObject())
    {
        const int index = (int) (double) d->getProperty ("index");
        const juce::String param = d->getProperty ("param").toString();
        const juce::var value = d->getProperty ("value");

        logEq ("RECV Parameter_Change  index=" + juce::String (index)
               + "  param=" + param + "  value=" + value.toString());

        processorRef.getEqEngine().setParam (index, param, value);

#if TOREI_EQ_DEBUG_LOG
        // NOT logged here: a node drag fires this at ~40 Hz and describe() dumps every
        // band, which is what inflated the log to 21 MB (§15.2). The throttled logger in
        // logMsDiagnostics() emits it at most every 500 ms, plus once after the last
        // change, so the settled state still gets recorded.
        pendingEngineLog = true;

        // §13.3(1): explicit acknowledgement that C++ actually received the mode (as
        // opposed to silently dropping it), plus a sample of the lane ramps ~300 ms
        // later once they have converged onto the new target.
        if (param == "mode")
        {
            const auto newMode = processorRef.getEqEngine().getBandMode (index);

            logEq ("ENGINE setParam band=" + juce::String (index)
                   + " param=mode value=" + value.toString()
                   + " -> " + juce::String (EqEngine::modeToString (newMode)));

            pendingLanesBand = index;
            pendingLanesAtMs = juce::Time::getMillisecondCounter() + 300;
        }
#endif

        // `listen` / `soloLevel` are transient audition state and are deliberately
        // NOT saved, so they must not mark the project as modified either.
        if (param != "listen" && param != "soloLevel")
            notifyHostStateChanged();
    }
    else
    {
        logEq ("RECV Parameter_Change  (not an object)  raw=" + object.toString());
    }
}

void ToreiEQAudioProcessorEditor::notifyHostStateChanged()
{
    // ChangeDetails must carry nonParameterStateChanged: the VST3 wrapper only calls
    // setDirty() when that flag is set (it maps it to its internal
    // pluginShouldBeMarkedDirtyFlag). A default-constructed ChangeDetails therefore
    // does nothing at all.
    processorRef.updateHostDisplay (
        juce::AudioProcessor::ChangeDetails().withNonParameterStateChanged (true));
}

void ToreiEQAudioProcessorEditor::pushBandState()
{
    if (webView == nullptr)
        return;

    EqEngine::BandInfo info[EqEngine::kMaxBands];
    const int n = processorRef.getEqEngine().getBandSnapshot (info, EqEngine::kMaxBands);

    juce::Array<juce::var> payload;
    payload.ensureStorageAllocated (n);

    for (int i = 0; i < n; ++i)
    {
        juce::DynamicObject::Ptr o = new juce::DynamicObject();
        o->setProperty ("index",  info[i].index);
        o->setProperty ("type",   juce::String (EqEngine::typeToString (info[i].type)));
        o->setProperty ("freq",   (double) info[i].freq);
        o->setProperty ("gain",   (double) info[i].gain);
        o->setProperty ("q",      (double) info[i].q);
        o->setProperty ("slope",  info[i].slope);
        o->setProperty ("mode",   juce::String (EqEngine::modeToString (info[i].mode)));
        o->setProperty ("bypass", info[i].bypass);
        payload.add (juce::var (o.get()));
    }

    webView->emitEventIfBrowserIsVisible ("Band_State", payload);

    logEq ("SEND Band_State  bands=" + juce::String (n));
}

void ToreiEQAudioProcessorEditor::pushOutputState()
{
    if (webView == nullptr)
        return;

    const float gainDb = processorRef.getEqEngine().getOutputGainDb();

    if (outputStateObj == nullptr)
        outputStateObj = new juce::DynamicObject();

    outputStateObj->setProperty ("gain", (double) gainDb);
    webView->emitEventIfBrowserIsVisible ("Output_State", outputStateObj.get());

    logEq ("SEND Output_State  gain=" + juce::String (gainDb));
}

void ToreiEQAudioProcessorEditor::handleCommand (const juce::var& object)
{
    if (auto* d = object.getDynamicObject())
    {
        const juce::String cmd = d->getProperty ("command").toString();

        if (cmd == "AddBand")
        {
            const int index = (int) (double) d->getProperty ("index");
            const float freq = (float) (double) d->getProperty ("freq");
            const float gain = (float) (double) d->getProperty ("gain");
            const float q    = (float) (double) d->getProperty ("q");
            const juce::String type = d->getProperty ("type").toString();

            logEq ("RECV Command AddBand  index=" + juce::String (index)
                   + "  type=" + type + "  freq=" + juce::String (freq)
                   + "  gain=" + juce::String (gain) + "  q=" + juce::String (q));

            processorRef.getEqEngine().addBand (index, EqEngine::typeFromString (type), freq, gain, q);

#if TOREI_EQ_DEBUG_LOG
            // Same class as `ENGINE after setParam`: a full-band describe() line. Low
            // frequency (only when a band is created), but gated for consistency and to
            // avoid pointless long lines (§21.3).
            logEq ("ENGINE after addBand  " + processorRef.getEqEngine().describe());
#endif

            notifyHostStateChanged();
        }
        else if (cmd == "RemoveBand")
        {
            const int index = (int) (double) d->getProperty ("index");
            logEq ("RECV Command RemoveBand  index=" + juce::String (index));
            processorRef.getEqEngine().removeBand (index);

#if TOREI_EQ_DEBUG_LOG
            logEq ("ENGINE after removeBand  " + processorRef.getEqEngine().describe());
#endif

            notifyHostStateChanged();
        }
        else if (cmd == "Request_State")
        {
            // The UI asks for the authoritative state on mount. C++ owns the bands
            // (EqEngine outlives the editor, so closing/reopening the window or
            // switching plugins must NOT reset them), and the UI mirrors what it
            // gets back instead of pushing its own defaults.
            logEq ("RECV Command Request_State");
            pushBandState();
            pushOutputState();
        }
        else
        {
            logEq ("RECV Command (unknown)  " + object.toString());
        }
    }
    else
    {
        logEq ("RECV Command (not an object)  raw=" + object.toString());
    }
}

void ToreiEQAudioProcessorEditor::resized()
{
    if (webView)
        webView->setBounds(getLocalBounds());
}
