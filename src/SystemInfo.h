#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct SystemMemoryInfo {
    uint64_t totalPhysicalBytes = 0;
    uint64_t availablePhysicalBytes = 0;
};

struct GpuAdapterInfo {
    std::wstring description;
    uint64_t dedicatedVideoMemoryBytes = 0;
    uint64_t sharedSystemMemoryBytes = 0;
};

// System-wide RAM totals, via GlobalMemoryStatusEx.
SystemMemoryInfo GetSystemMemoryInfo();

// Enumerates hardware (non-software) GPU adapters and their VRAM capacity,
// via DXGI. Used only for the summary bar (total capacity), not per-process
// tracking - that comes from GpuMonitor's PDH counters instead.
std::vector<GpuAdapterInfo> GetGpuAdapters();
