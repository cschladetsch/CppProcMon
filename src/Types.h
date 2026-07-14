#pragma once
#include <cstdint>
#include <string>

// Per-process GPU utilization, split by engine type instead of collapsed
// into one number. The "GPU Engine(*)\Utilization Percentage" PDH counter
// exposes one instance per pid+engine (3D, Copy, VideoDecode, ...) - Task
// Manager sums these into a single per-process GPU% column and only shows
// which single engine is busiest via an extra column; this keeps every
// engine's share visible at once, which matters for telling "this process
// is GPU-compute-bound" apart from "this process is just doing video
// decode" at a glance. engineOther catches engine types not broken out
// individually (Overlay, SceneAssembly, Security, vendor-specific, etc.)
// so the sum of all fields always equals the process's total gpuPercent.
struct GpuEngineBreakdown {
    double engine3D = 0.0;
    double engineCopy = 0.0;
    double engineVideoDecode = 0.0;
    double engineVideoEncode = 0.0;
    double engineVideoProcessing = 0.0;
    double engineCompute = 0.0;
    double engineOther = 0.0;
};

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
    GpuEngineBreakdown gpuEngines;  // Same total, split out by engine type.
};
