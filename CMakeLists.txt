cmake_minimum_required(VERSION 3.25)
project(ProcMon LANGUAGES CXX)

if (NOT WIN32)
    message(FATAL_ERROR "ProcMon targets Windows only (uses PDH, PSAPI, ToolHelp32 and DXGI).")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Third-party fetch (GLFW + Dear ImGui). CppLogicMake's schema doesn't
# model FetchContent yet (an explicit non-goal for now - see its README),
# so this stays hand-written CMake rather than Prolog facts.
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/ThirdParty.cmake)

# The procmon executable's own target/sources/include/link/define graph is
# authored as Prolog facts in ProcMon.lm and transpiled by CppLogicMake
# (https://github.com/cschladetsch/CppLogiMake) into generated/CMakeLists.txt.
# It's pulled in via add_subdirectory (not include()) so the rebased,
# "../src/..."-relative source paths CppLogicMake emits resolve correctly.
# generated/CMakeLists.txt is gitignored, not checked in - CppLogicMake
# is a required prerequisite (s.ps1 regenerates it automatically; to
# regenerate by hand:
#   logimake generate -Input ProcMon.lm -Output generated/CMakeLists.txt -WorkingDirectory .
# ProcMon.lm's compile_options/3 and install/2 facts cover the procmon
# target's warning flags and install rule directly in the generated
# output now.
add_subdirectory(generated)

# procmon is a GUI app (GLFW/Dear ImGui window), not a console tool, but
# main.cpp keeps an ordinary int main() rather than WinMain. Plain
# add_executable() defaults to the console subsystem, so without this,
# Windows allocates a console window for procmon.exe in addition to its
# real window every time it runs (visible as the "two console windows"
# s.ps1 -> Start-Process -> procmon.exe chain produces: the shell running
# s.ps1, plus procmon's own auto-allocated console). WIN32_EXECUTABLE
# switches it to the GUI subsystem; since that alone expects a WinMain
# entry point, /ENTRY:mainCRTStartup puts the entry back to the ordinary
# CRT startup that calls main() instead. CppLogicMake's schema has no
# predicate for either (target subsystem / linker entry point aren't
# build-graph facts in the sources/links/defines sense), so - like the
# FetchContent block above - this stays hand-written rather than forcing
# a schema addition for a single project-specific concern.
#
# The "LINKER:" prefix (not a bare /ENTRY:...) matters here: MSVC's
# cl.exe/link.exe accept a raw /ENTRY:... flag directly, but clang++
# (used with lld-link as the actual linker) is its own driver and takes
# that same flag on ITS OWN command line, where it just looks like a
# bogus input path ("no such file or directory: '/ENTRY:mainCRTStartup'")
# unless it's wrapped so clang forwards it to the linker instead of
# trying to interpret it itself. LINKER: is CMake's portable way to say
# "this is a linker-only flag" - it expands to -Xlinker/-Wl, wrapping for
# Clang/GCC-style drivers and passes through unwrapped for MSVC, so the
# same line is correct for both without a compiler-specific flag syntax.
set_target_properties(procmon PROPERTIES WIN32_EXECUTABLE ON)
if (MSVC OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_link_options(procmon PRIVATE "LINKER:/ENTRY:mainCRTStartup")
endif()
