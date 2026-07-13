#pragma once
#include <cstdint>
#include <string>

// One row of the process table. Filled in two passes: ProcessMonitor
// supplies pid/name/cpu/ram, GpuMonitor supplies the gpu* fields.
struct ProcessSample {
    uint32_t pid = 0;
    std::wstring name;
    std::wstring imagePath;        // Full path to the executable, if resolvable.

    double cpuPercent = 0.0;       // Normalized 0-100 across all logical cores.

    uint64_t workingSetBytes = 0;  // "RAM" as Task Manager shows it.
    uint64_t privateBytes = 0;     // Private/commit bytes (closer to true footprint).

    uint64_t gpuDedicatedBytes = 0; // VRAM on the GPU itself.
    uint64_t gpuSharedBytes = 0;    // System RAM borrowed by the GPU.
    double gpuPercent = 0.0;        // Sum of GPU engine utilization for this pid.
};
