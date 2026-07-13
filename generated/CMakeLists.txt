# Pre-generated snapshot of CppLogicMake's output from ../ProcMon.lm,
# checked in so this archive builds without CppLogicMake installed.
# s.ps1 regenerates this file automatically when `logimake` is on PATH
# (or $env:LOGICMAKE_ROOT is set) - if you plan to regenerate, make your
# changes in ../ProcMon.lm instead of editing this file by hand.
cmake_minimum_required(VERSION 3.25)
project(generated LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --- procmon ---
add_executable(procmon ../src/GpuMonitor.cpp ../src/ProcessMonitor.cpp ../src/SystemInfo.cpp ../src/main.cpp)
target_include_directories(procmon PUBLIC ../src)
target_link_libraries(procmon PRIVATE imgui)
target_link_libraries(procmon PRIVATE pdh psapi dxgi)
# NOTE: resolved closure for procmon is tracked in the semantic model, not flattened into CMake.
target_compile_definitions(procmon PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
