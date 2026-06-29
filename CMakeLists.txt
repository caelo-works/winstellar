cmake_minimum_required(VERSION 3.21)

# Required for MSVC_RUNTIME_LIBRARY target property to take effect.
cmake_policy(SET CMP0091 NEW)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

project(WinStellar LANGUAGES CXX VERSION 0.6.1)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Single source of truth for the version. Generates a header used by both
# .rc files (FILEVERSION) and is read by installer/build_installer.ps1.
# Build provenance: capture the git SHA + dirty-tree flag at configure time
# so the VERSIONINFO carries traceable build metadata. `scripts/build.ps1`
# re-runs configure on every build, so the values stay fresh.
find_package(Git QUIET)
set(FITS_GIT_SHA "unknown")
set(FITS_GIT_DIRTY 0)
if(GIT_FOUND)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" rev-parse --short=8 HEAD
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        OUTPUT_VARIABLE _git_sha
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE _git_rc)
    if(_git_rc EQUAL 0 AND _git_sha)
        set(FITS_GIT_SHA "${_git_sha}")
    endif()
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" status --porcelain
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        OUTPUT_VARIABLE _git_status
        ERROR_QUIET)
    if(_git_status)
        set(FITS_GIT_DIRTY 1)
    endif()
endif()
string(TIMESTAMP FITS_BUILD_DATE "%Y-%m-%d")
if(FITS_GIT_DIRTY)
    set(FITS_VERSION_FULL "${PROJECT_VERSION} (${FITS_GIT_SHA}-dirty, ${FITS_BUILD_DATE})")
else()
    set(FITS_VERSION_FULL "${PROJECT_VERSION} (${FITS_GIT_SHA}, ${FITS_BUILD_DATE})")
endif()
message(STATUS "Build version: ${FITS_VERSION_FULL}")

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/fits_version.h.in"
    "${CMAKE_BINARY_DIR}/generated/fits_version.h"
    @ONLY
)
include_directories(BEFORE "${CMAKE_BINARY_DIR}/generated")

if(NOT WIN32)
    message(FATAL_ERROR "WinStellar targets Windows only (MSVC). Configure with the x64-windows-msvc CMake preset.")
endif()

if(MSVC)
    add_compile_options(/W4 /permissive- /utf-8 /Zc:__cplusplus /MP /EHsc)
    add_compile_definitions(
        UNICODE _UNICODE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        _CRT_SECURE_NO_WARNINGS
    )
    # Release-only optimizations: AVX2 (Windows 11 minimum CPU has it),
    # function-level linking (/Gy default-on under /O2 -- safe to keep
    # explicit), data COMDAT folding (/Gw, helps the linker eliminate dead
    # globals / strings).
    add_compile_options($<$<CONFIG:Release>:/arch:AVX2>)
    add_compile_options($<$<CONFIG:Release>:/Gw>)
endif()

# Link-time code generation for Release. cfitsio / sqlite3 ship as static
# libs from vcpkg without LTCG bitcode, so we restrict IPO to our own
# targets (it's per-target rather than global). Skip if the compiler
# doesn't report IPO support.
include(CheckIPOSupported)
check_ipo_supported(RESULT WINSTELLAR_IPO_OK OUTPUT _ipo_err)
if(NOT WINSTELLAR_IPO_OK)
    message(STATUS "IPO/LTCG unavailable: ${_ipo_err}")
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

find_package(cfitsio CONFIG REQUIRED)

add_subdirectory(fits_core)
add_subdirectory(shell_ext)
add_subdirectory(viewer)
add_subdirectory(tools)

# Tests live in tests/ and are opt-in via the cache variable so external
# consumers can disable them. CTest is enabled here so gtest_discover_tests
# can register each TEST individually with the test runner.
option(WINSTELLAR_BUILD_TESTS "Build the WinStellar test suite" ON)
if(WINSTELLAR_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
