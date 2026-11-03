// NativeEventLoop: Briefly pump the native event loop from headless tests that wait on native menus and peers.

#pragma once

#include <JuceHeader.h>

#if JUCE_MAC
#include <CoreFoundation/CoreFoundation.h>
#endif

namespace SpatialEchoTests::NativeEventLoop
{
enum class PumpResult
{
    handledSource,
    timedOut,
    other
};

// macOS runs the CoreFoundation loop for 10 ms. Other platforms have no
// equivalent without JUCE modal loops; the native-menu tests that depend on it
// run on the macOS headless runner only, so elsewhere this just yields.
inline PumpResult pumpBriefly()
{
#if JUCE_MAC
    switch (CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.01, true))
    {
        case kCFRunLoopRunHandledSource:
            return PumpResult::handledSource;
        case kCFRunLoopRunTimedOut:
            return PumpResult::timedOut;
        case kCFRunLoopRunFinished:
        case kCFRunLoopRunStopped:
            return PumpResult::other;
    }
    return PumpResult::other;
#else
    juce::Thread::sleep(10);
    return PumpResult::timedOut;
#endif
}

inline bool isMainLoop()
{
#if JUCE_MAC
    return CFRunLoopGetCurrent() == CFRunLoopGetMain();
#else
    return juce::MessageManager::existsAndIsCurrentThread();
#endif
}
}  // namespace SpatialEchoTests::NativeEventLoop
