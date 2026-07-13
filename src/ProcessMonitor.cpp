#include "ProcessMonitor.h"

// WIN32_LEAN_AND_MEAN/UNICODE/_UNICODE/NOMINMAX come from the procmon
// target's compile definitions (ProcMon.lm -> generated/CMakeLists.txt),
// not a local #define, to avoid a macro-redefined warning against that.
#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>

namespace {

uint64_t FileTimeToU64(const FILETIME& ft) {
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return u.QuadPart;
}

} // namespace

ProcessMonitor::ProcessMonitor() {
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    numCores_ = si.dwNumberOfProcessors > 0 ? si.dwNumberOfProcessors : 1;
}

std::vector<ProcessSample> ProcessMonitor::Poll(double elapsedSeconds) {
    std::vector<ProcessSample> out;

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        return out;
    }

    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);

    std::unordered_map<uint32_t, uint64_t> currentTicks;
    currentTicks.reserve(prevCpuTicks_.size());

    if (Process32FirstW(snap, &pe)) {
        do {
            ProcessSample s;
            s.pid = pe.th32ProcessID;
            s.name = pe.szExeFile;

            // PID 0 is the "System Idle Process" pseudo-entry; nothing to query.
            if (s.pid == 0) {
                out.push_back(std::move(s));
                continue;
            }

            HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE, s.pid);
            if (h) {
                FILETIME creationTime{}, exitTime{}, kernelTime{}, userTime{};
                if (GetProcessTimes(h, &creationTime, &exitTime, &kernelTime, &userTime)) {
                    uint64_t total = FileTimeToU64(kernelTime) + FileTimeToU64(userTime);
                    currentTicks[s.pid] = total;

                    auto it = prevCpuTicks_.find(s.pid);
                    if (it != prevCpuTicks_.end() && elapsedSeconds > 0.0) {
                        uint64_t deltaTicks = (total > it->second) ? (total - it->second) : 0;
                        double deltaSeconds = static_cast<double>(deltaTicks) / 1.0e7; // 100ns units
                        s.cpuPercent = (deltaSeconds / elapsedSeconds / numCores_) * 100.0;
                    }
                }

                PROCESS_MEMORY_COUNTERS_EX pmc{};
                pmc.cb = sizeof(pmc);
                if (GetProcessMemoryInfo(h, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc))) {
                    s.workingSetBytes = pmc.WorkingSetSize;
                    s.privateBytes = pmc.PrivateUsage;
                }

                wchar_t pathBuf[MAX_PATH];
                DWORD pathLen = MAX_PATH;
                if (QueryFullProcessImageNameW(h, 0, pathBuf, &pathLen)) {
                    s.imagePath.assign(pathBuf, pathLen);
                }

                CloseHandle(h);
            }
            // If OpenProcess failed (protected/elevated process), the row still
            // shows pid + name with zeroed metrics rather than being dropped.

            out.push_back(std::move(s));
        } while (Process32NextW(snap, &pe));
    }

    CloseHandle(snap);
    prevCpuTicks_ = std::move(currentTicks);
    return out;
}
