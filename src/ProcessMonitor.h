#pragma once
#include "Types.h"

#include <cstdint>
#include <unordered_map>
#include <vector>

// Enumerates running processes and computes per-process CPU% and RAM usage.
// CPU% is derived from the delta of kernel+user time between successive
// Poll() calls, normalized by wall-clock elapsed time and core count -
// the same convention Task Manager's "CPU" column uses (sums to ~100%
// system-wide, not per-core).
class ProcessMonitor {
public:
    ProcessMonitor();

    std::vector<ProcessSample> Poll(double elapsedSeconds);

private:
    unsigned numCores_ = 1;
    std::unordered_map<uint32_t, uint64_t> prevCpuTicks_; // pid -> 100ns kernel+user ticks
};
