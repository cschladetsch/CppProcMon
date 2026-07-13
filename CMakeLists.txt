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
# A checked-in copy of generated/CMakeLists.txt ships in this archive so the
# project builds without CppLogicMake installed; s.ps1 regenerates it
# automatically when `logimake` is on PATH. To regenerate by hand:
#   logimake generate -Input ProcMon.lm -Output generated/CMakeLists.txt -WorkingDirectory .
add_subdirectory(generated)

# CppLogicMake's schema has no compile-options predicate, so warning flags
# for the procmon target are layered on here instead, after the target
# exists.
if (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(procmon PRIVATE -Wall -Wextra -Wpedantic)
elseif (MSVC)
    target_compile_options(procmon PRIVATE /W4 /utf-8)
endif()

install(TARGETS procmon RUNTIME DESTINATION bin)
