#pragma once

// Shared debug log used by BOTH the UI bridge (PluginEditor) and the audio path
// (PluginProcessor), so we can trace the whole DSP<->UI loop from file.
//
// Concurrent access from the message thread (editor timer), the WebView2 callback
// thread (incoming UI events) and the audio thread (processBlock) is serialised
// with a function-local mutex, so fopen/fprintf/fclose on the shared path is safe.
//
// NOTE: logEq() itself is always compiled -- the short RECV transport lines and the
// one-shot PROC diagnostics rely on it. What TOREI_EQ_DEBUG_LOG gates is the chatty
// diagnostic layer built on top of it; see EqConfig.h.
#include "EqConfig.h"

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

    // --- Size cap / rotation (MID_SIDE_HANDOFF.md §15.2) -----------------------
    // This log had reached 21 MB / 180k lines because a node drag produced one entry
    // per parameter change (~40 Hz). Callers now throttle their chatty lines, but the
    // guard belongs here too so the file can never grow without bound regardless of
    // what future call sites do.
    constexpr juce::int64 kMaxLogBytes = 8 * 1024 * 1024;   // 8 MB

    if (path.existsAsFile() && path.getSize() > kMaxLogBytes)
    {
        // Keep exactly one previous file: overwrite any older rotation.
        auto rotated = dir.getChildFile ("eq_debug.1.txt");
        rotated.deleteFile();
        path.moveFileTo (rotated);
    }

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
