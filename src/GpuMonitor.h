#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>

// WIN32_LEAN_AND_MEAN comes from the procmon target's compile definitions
// (ProcMon.lm -> generated/CMakeLists.txt), not a local #define.
#include <windows.h>
#include <pdh.h>

#include "Types.h"

// Reads per-process VRAM usage and GPU engine utilization via the
// "GPU Process Memory" and "GPU Engine" PDH performance counter objects
// (the same source Task Manager's per-process GPU columns use on
// Windows 10 1803+). Counter instances churn constantly as processes
// create/destroy GPU contexts, so the counter set is re-synced every poll.
class GpuMonitor {
public:
    struct Stats {
        uint64_t dedicatedBytes = 0;
        uint64_t sharedBytes = 0;
        double utilizationPercent = 0.0; // Sum of engines, i.e. Task Manager's GPU%.
        GpuEngineBreakdown engines;      // Same total, split by engine type.
    };

    GpuMonitor();
    ~GpuMonitor();

    GpuMonitor(const GpuMonitor&) = delete;
    GpuMonitor& operator=(const GpuMonitor&) = delete;

    // Returns per-PID aggregated GPU stats (summed across adapters/engines).
    // Intended to be called roughly once per second; newly-appeared
    // instances may read as zero for one cycle until PDH has two samples.
    std::unordered_map<uint32_t, Stats> Poll();

private:
    PDH_HQUERY query_ = nullptr;
    std::unordered_map<std::wstring, PDH_HCOUNTER> dedicatedCounters_;
    std::unordered_map<std::wstring, PDH_HCOUNTER> sharedCounters_;
    std::unordered_map<std::wstring, PDH_HCOUNTER> engineCounters_;

    void SyncWildcardCounters(const wchar_t* wildcardPath,
                               std::unordered_map<std::wstring, PDH_HCOUNTER>& counters);

    static bool ExtractInstance(const std::wstring& fullPath, std::wstring& instance);
    static bool ExtractPid(const std::wstring& instance, uint32_t& pid);

    // Parses the "engtype_<Type>" suffix off a GPU Engine instance name
    // (e.g. "...eng_0_engtype_VideoDecode" -> "VideoDecode") and adds
    // `percent` into the matching GpuEngineBreakdown field, falling back to
    // engineOther for types not broken out individually.
    static void AddEngineUtilization(const std::wstring& instance, double percent, GpuEngineBreakdown& out);
};
