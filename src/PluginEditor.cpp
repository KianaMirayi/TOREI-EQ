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
    auto winOpts = juce::WebBrowserComponent::Options::WinWebView2{};
    winOpts = winOpts.withUserDataFolder(
        juce::File::getSpecialLocation(juce::File::tempDirectory)
            .getChildFile("TOREI_EQ_WebView2"));
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

    webView = std::make_unique<juce::WebBrowserComponent>(opts);
    webView->setBounds(getLocalBounds());
    addAndMakeVisible(webView.get());
    webView->goToURL("about:blank");

    setSize(1500, 700);
    setResizable(true, true);
    setResizeLimits(800, 500, 2200, 1500);
}

ToreiEQAudioProcessorEditor::~ToreiEQAudioProcessorEditor() {}

void ToreiEQAudioProcessorEditor::resized()
{
    if (webView)
        webView->setBounds(getLocalBounds());
}
