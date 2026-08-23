#pragma once

// Shared debug log used by BOTH the UI bridge (PluginEditor) and the audio path
// (PluginProcessor), so we can trace the whole DSP<->UI loop from file.
//
// Concurrent access from the message thread (editor timer), the WebView2 callback
// thread (incoming UI events) and the audio thread (processBlock) is serialised
// with a function-local mutex, so fopen/fprintf/fclose on the shared path is safe.
#include <JuceHeader.h>
#include <cstdio>
#include <mutex>

inline void logEq (const juce::String& msg)
{
    static std::mutex logMutex;
    const std::scoped_lock lock { logMutex };

    // Write to the user's AppData folder (a location a DAW-hosted plugin process
    // can always write to). The project dir is NOT writable from some DAW/plugin
    // containers, which is why eq_debug.txt never appeared there when hosted.
    const auto dir = juce::File::getSpecialLocation(
        juce::File::userApplicationDataDirectory).getChildFile ("TOREI-EQ");
    dir.createDirectory();
    const auto path = dir.getChildFile ("eq_debug.txt");

    FILE* f = fopen (path.getFullPathName().toRawUTF8(), "a");
    if (f)
    {
        const juce::Time now = juce::Time::getCurrentTime();
        fprintf (f, "[%02d:%02d:%02d.%03d] %s\n",
                 (int) now.getHours(),
                 (int) now.getMinutes(),
                 (int) now.getSeconds(),
                 (int) (now.getMilliseconds() % 1000),
                 msg.toRawUTF8());
        fclose (f);
    }
}
