// AllocationTracking: Audio-thread allocation counter for tests. Defined in PluginProcessorDebug.cpp, which replaces
//   the global operator new in Debug and headless-test builds only.

#pragma once

namespace SpatialEchoDebug
{
// Counts heap allocations made on the calling thread between begin and end.
void beginAudioAllocationTracking();
int endAudioAllocationTracking();
}  // namespace SpatialEchoDebug
