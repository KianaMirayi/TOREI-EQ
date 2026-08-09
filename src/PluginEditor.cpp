#include "PluginEditor.h"

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

static juce::String loadBase64(const juce::File& f)
{
    juce::MemoryBlock mb;
    return f.loadFileAsData(mb)
        ? juce::Base64::toBase64(mb.getData(), mb.getSize())
        : juce::String{};
}

ToreiEQAudioProcessorEditor::ToreiEQAudioProcessorEditor(ToreiEQAudioProcessor& p)
    : AudioProcessorEditor(&p)
    , processorRef(p)
{
    // Create the browser lazily on first visibility to avoid first-load crashes
    // in hosts whose message loop is not yet stable during editor creation.
    startTimerHz(21);

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

    auto htmlFile = findWebUI();
    if (htmlFile.existsAsFile())
    {
        auto b64 = loadBase64(htmlFile);
        if (b64.isNotEmpty())
            opts = opts.withUserScript(
                "document.write(atob('" + b64 + "')); document.close();");
    }

    webView = std::make_unique<ToreiWebView>(opts);
    webView->onPageLoaded = [this] { pageLoaded = true; };
    webView->setBounds(getLocalBounds());
    addAndMakeVisible(webView.get());
    webView->goToURL("about:blank");
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
        webView->onPageLoaded = nullptr;

    stopTimer();
}

void ToreiEQAudioProcessorEditor::timerCallback()
{
    ensureWebView();

    if (!webView || !pageLoaded)
        return;

    // Push the real incoming audio level to the frontend meter.
    juce::DynamicObject::Ptr obj = new juce::DynamicObject();
    obj->setProperty ("peak", getAudioPeakDb());
    obj->setProperty ("rms",  getAudioRmsDb());
    webView->emitEventIfBrowserIsVisible ("Audio_Level", obj.get());
}

void ToreiEQAudioProcessorEditor::resized()
{
    if (webView)
        webView->setBounds(getLocalBounds());
}
