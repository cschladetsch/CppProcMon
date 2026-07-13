#include "SystemInfo.h"

// WIN32_LEAN_AND_MEAN comes from the procmon target's compile definitions
// (ProcMon.lm -> generated/CMakeLists.txt), not a local #define.
#include <windows.h>

#include <dxgi1_4.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

SystemMemoryInfo GetSystemMemoryInfo() {
    SystemMemoryInfo info;
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof(ms);
    if (GlobalMemoryStatusEx(&ms)) {
        info.totalPhysicalBytes = ms.ullTotalPhys;
        info.availablePhysicalBytes = ms.ullAvailPhys;
    }
    return info;
}

std::vector<GpuAdapterInfo> GetGpuAdapters() {
    std::vector<GpuAdapterInfo> adapters;

    ComPtr<IDXGIFactory4> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        return adapters;
    }

    for (UINT i = 0;; ++i) {
        ComPtr<IDXGIAdapter1> adapter1;
        if (factory->EnumAdapters1(i, &adapter1) == DXGI_ERROR_NOT_FOUND) {
            break;
        }

        DXGI_ADAPTER_DESC1 desc{};
        if (FAILED(adapter1->GetDesc1(&desc))) {
            continue;
        }
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) {
            continue; // Skip the WARP/basic-render software adapter.
        }

        GpuAdapterInfo info;
        info.description = desc.Description;
        info.dedicatedVideoMemoryBytes = desc.DedicatedVideoMemory;
        info.sharedSystemMemoryBytes = desc.SharedSystemMemory;
        adapters.push_back(std::move(info));
    }

    return adapters;
}
