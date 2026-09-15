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
                                  { logEq ("UI: " + (o.isObject() ? o["msg"].toString() : o.toString())); });

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
    webView->onPageLoaded = [this]
    {
        pageLoaded = true;
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

    stopTimer();
}

void ToreiEQAudioProcessorEditor::timerCallback()
{
    ensureWebView();

    if (!webView || !pageLoaded)
        return;

    // Push the real incoming audio level to the frontend meter.
    juce::DynamicObject::Ptr obj = new juce::DynamicObject();
    obj->setProperty ("peak", processorRef.getPeakDb());
    obj->setProperty ("rms",  processorRef.getRmsDb());
    webView->emitEventIfBrowserIsVisible ("Audio_Level", obj.get());

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

    if (points > 0)
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
    }

    // Push the POST (output) spectrum on its own channel so the UI can overlay the
    // pre/post curves Pro-Q style. Same log-spaced layout and smoothing as pre.
    if (spectrumPostScratch == nullptr)
        spectrumPostScratch.calloc (kSpectrumPointCount);

    const int postPoints = processorRef.readSpectrumPost (spectrumPostScratch.getData(), kSpectrumPointCount);

    if (postPoints > 0)
    {
        spectrumPostPayload.clearQuick();
        spectrumPostPayload.ensureStorageAllocated (postPoints);

        for (int i = 0; i < postPoints; ++i)
        {
            const float v = std::round (spectrumPostScratch[i] * 10.0f) * 0.1f;
            spectrumPostPayload.add (juce::var ((double) v));
        }

        webView->emitEventIfBrowserIsVisible ("Spectrum_Data_Post", spectrumPostPayload);
    }

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
    pushCurve ("EQ_Curve_Data",      EqEngine::curveMid);
    pushCurve ("EQ_Curve_Data_Mid",  EqEngine::curveMid);
    pushCurve ("EQ_Curve_Data_Side", EqEngine::curveSide);
    pushCurve ("EQ_Curve_Data_L",    EqEngine::curveLeft);
    pushCurve ("EQ_Curve_Data_R",    EqEngine::curveRight);

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

            logEq ("SEND EQ_Listen_State  index=" + juce::String (listen));
        }
    }

    // Curve logging / diagnostics live in pushCurve() (it owns the scratch buffer).

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
        curvePayload.add (juce::var ((double) std::round (curveScratch[i] * 10.0f) * 0.1f));

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
