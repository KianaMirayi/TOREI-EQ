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

    // Push the EQ magnitude response (dB, log-spaced 20 Hz..20 kHz) to the UI.
    if (curveScratch == nullptr)
        curveScratch.calloc (EqEngine::kCurvePoints);

    const int curvePoints = processorRef.getEqEngine().getCurveGains (curveScratch.getData(),
                                                                      EqEngine::kCurvePoints);

    curvePayload.clearQuick();
    curvePayload.ensureStorageAllocated (curvePoints);

    for (int i = 0; i < curvePoints; ++i)
        curvePayload.add (juce::var ((double) std::round (curveScratch[i] * 10.0f) * 0.1f));

    webView->emitEventIfBrowserIsVisible ("EQ_Curve_Data", curvePayload);

    {
        static bool curveLoggedOnce = false;
        if (! curveLoggedOnce)
        {
            curveLoggedOnce = true;
            logEq ("SEND EQ_Curve_Data  points=" + juce::String (curvePoints)
                   + "  firstGain=" + juce::String (curveScratch[0]));
        }
    }

    // Periodic curve diagnostic: report the computed curve's min/max dB so we can
    // confirm getCurveGains actually reflects the band boosts (every ~5 s).
    {
        static int curveDiagTick = 0;
        if ((++curveDiagTick % 200) == 0 && curvePoints > 0)
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
        logEq ("ENGINE after setParam  " + processorRef.getEqEngine().describe());
    }
    else
    {
        logEq ("RECV Parameter_Change  (not an object)  raw=" + object.toString());
    }
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
            logEq ("ENGINE after addBand  " + processorRef.getEqEngine().describe());
        }
        else if (cmd == "RemoveBand")
        {
            const int index = (int) (double) d->getProperty ("index");
            logEq ("RECV Command RemoveBand  index=" + juce::String (index));
            processorRef.getEqEngine().removeBand (index);
            logEq ("ENGINE after removeBand  " + processorRef.getEqEngine().describe());
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
