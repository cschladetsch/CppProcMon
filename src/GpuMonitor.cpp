#include "GpuMonitor.h"

#include <cwchar>
#include <pdhmsg.h>
#include <vector>

namespace {

// Expands a wildcard PDH counter path (e.g. "\GPU Process Memory(*)\Dedicated Usage")
// into the full list of currently-live counter paths.
std::vector<std::wstring> ExpandWildcard(const wchar_t* wildcardPath) {
    std::vector<std::wstring> result;

    DWORD pathListLen = 0;
    PDH_STATUS status = PdhExpandWildCardPathW(nullptr, wildcardPath, nullptr, &pathListLen, 0);
    // PDH_MORE_DATA is an unsigned constant but PDH_STATUS is signed; cast
    // to avoid a sign-compare warning on the comparison below.
    if (status != static_cast<PDH_STATUS>(PDH_MORE_DATA) || pathListLen == 0) {
        return result; // Counter object not present on this system/driver.
    }

    std::vector<wchar_t> buffer(pathListLen);
    status = PdhExpandWildCardPathW(nullptr, wildcardPath, buffer.data(), &pathListLen, 0);
    if (status != ERROR_SUCCESS) {
        return result;
    }

    const wchar_t* p = buffer.data();
    while (*p) {
        result.emplace_back(p);
        p += wcslen(p) + 1;
    }
    return result;
}

} // namespace

GpuMonitor::GpuMonitor() {
    PdhOpenQueryW(nullptr, 0, &query_);
}

GpuMonitor::~GpuMonitor() {
    if (query_) {
        PdhCloseQuery(query_);
        query_ = nullptr;
    }
}

bool GpuMonitor::ExtractInstance(const std::wstring& fullPath, std::wstring& instance) {
    auto open = fullPath.find(L'(');
    auto close = fullPath.rfind(L')');
    if (open == std::wstring::npos || close == std::wstring::npos || close <= open) {
        return false;
    }
    instance = fullPath.substr(open + 1, close - open - 1);
    return true;
}

bool GpuMonitor::ExtractPid(const std::wstring& instance, uint32_t& pid) {
    // Instance names look like:
    //   pid_1234_luid_0x00000000_0x0000ABCD_phys_0
    //   pid_1234_luid_0x00000000_0x0000ABCD_phys_0_eng_0_engtype_3D
    unsigned value = 0;
    if (swscanf_s(instance.c_str(), L"pid_%u_", &value) == 1) {
        pid = value;
        return true;
    }
    return false;
}

void GpuMonitor::SyncWildcardCounters(const wchar_t* wildcardPath,
                                       std::unordered_map<std::wstring, PDH_HCOUNTER>& counters) {
    auto paths = ExpandWildcard(wildcardPath);

    std::unordered_map<std::wstring, PDH_HCOUNTER> updated;
    updated.reserve(paths.size());

    for (auto& path : paths) {
        auto it = counters.find(path);
        if (it != counters.end()) {
            updated.emplace(path, it->second);
            counters.erase(it);
        } else {
            PDH_HCOUNTER counter = nullptr;
            if (PdhAddEnglishCounterW(query_, path.c_str(), 0, &counter) == ERROR_SUCCESS) {
                updated.emplace(path, counter);
            }
        }
    }

    // Anything left behind belongs to an instance that vanished this cycle
    // (process exited / GPU context torn down) - drop it.
    for (auto& [path, counter] : counters) {
        PdhRemoveCounter(counter);
    }

    counters = std::move(updated);
}

std::unordered_map<uint32_t, GpuMonitor::Stats> GpuMonitor::Poll() {
    std::unordered_map<uint32_t, Stats> result;
    if (!query_) {
        return result;
    }

    SyncWildcardCounters(L"\\GPU Process Memory(*)\\Dedicated Usage", dedicatedCounters_);
    SyncWildcardCounters(L"\\GPU Process Memory(*)\\Shared Usage", sharedCounters_);
    SyncWildcardCounters(L"\\GPU Engine(*)\\Utilization Percentage", engineCounters_);

    if (PdhCollectQueryData(query_) != ERROR_SUCCESS) {
        return result;
    }

    PDH_FMT_COUNTERVALUE value{};

    for (auto& [path, counter] : dedicatedCounters_) {
        if (PdhGetFormattedCounterValue(counter, PDH_FMT_LARGE, nullptr, &value) != ERROR_SUCCESS) {
            continue; // Freshly-added counter, or momentarily unavailable.
        }
        std::wstring instance;
        uint32_t pid = 0;
        if (ExtractInstance(path, instance) && ExtractPid(instance, pid)) {
            result[pid].dedicatedBytes += static_cast<uint64_t>(value.largeValue);
        }
    }

    for (auto& [path, counter] : sharedCounters_) {
        if (PdhGetFormattedCounterValue(counter, PDH_FMT_LARGE, nullptr, &value) != ERROR_SUCCESS) {
            continue;
        }
        std::wstring instance;
        uint32_t pid = 0;
        if (ExtractInstance(path, instance) && ExtractPid(instance, pid)) {
            result[pid].sharedBytes += static_cast<uint64_t>(value.largeValue);
        }
    }

    for (auto& [path, counter] : engineCounters_) {
        // Utilization Percentage is a rate counter: it needs two samples
        // before it reports a meaningful value, which naturally happens
        // on the poll after a given engine instance first appears.
        if (PdhGetFormattedCounterValue(counter, PDH_FMT_DOUBLE, nullptr, &value) != ERROR_SUCCESS) {
            continue;
        }
        std::wstring instance;
        uint32_t pid = 0;
        if (ExtractInstance(path, instance) && ExtractPid(instance, pid)) {
            result[pid].utilizationPercent += value.doubleValue;
        }
    }

    return result;
}
