find_package(GTest CONFIG REQUIRED)
include(GoogleTest)

# Shared test helpers (synthetic FITS/XISF generation, no test cases of their
# own) — built once and linked into each test binary.
add_library(winstellar_test_helpers STATIC
    helpers/synth_fits.cpp
    helpers/synth_xisf.cpp
)
target_include_directories(winstellar_test_helpers PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(winstellar_test_helpers PUBLIC
    fits_core
    CFITSIO::cfitsio
)
set_target_properties(winstellar_test_helpers PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
)

# Convenience: one executable per logical group, linked against fits_core and
# gtest. gtest_discover_tests registers each TEST() with CTest so failures
# show file:line and `ctest -R` can target individual tests.
function(winstellar_add_test_exe name)
    add_executable(${name} ${ARGN})
    target_link_libraries(${name} PRIVATE
        winstellar_test_helpers
        fits_core
        GTest::gtest
        GTest::gtest_main
    )
    set_target_properties(${name} PROPERTIES
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
    )
    gtest_discover_tests(${name}
        PROPERTIES LABELS "winstellar"
        DISCOVERY_TIMEOUT 30
    )
endfunction()

# ---------- Phase 1: unit tests (pure math/parsing, no I/O) ----------------
winstellar_add_test_exe(unit_stretch    unit_stretch.cpp)
winstellar_add_test_exe(unit_headers    unit_headers.cpp)
winstellar_add_test_exe(unit_xisf_magic unit_xisf_magic.cpp)
winstellar_add_test_exe(unit_cache_key  unit_cache_key.cpp)
winstellar_add_test_exe(unit_debayer    unit_debayer.cpp)
winstellar_add_test_exe(unit_raw        unit_raw.cpp)

# Header-only inspection-rotation helpers live in viewer/src; give this one test
# access to that include dir (no link against the viewer needed).
winstellar_add_test_exe(unit_rotation   unit_rotation.cpp)
target_include_directories(unit_rotation PRIVATE ${CMAKE_SOURCE_DIR}/viewer/src)

# ---------- Phase 2: integration tests (file/buffer I/O) -------------------
winstellar_add_test_exe(integration_fits_loader integration_fits_loader.cpp)
winstellar_add_test_exe(integration_xisf        integration_xisf.cpp)
winstellar_add_test_exe(integration_raw         integration_raw.cpp)
winstellar_add_test_exe(integration_analysis    integration_analysis.cpp)
winstellar_add_test_exe(integration_cache       integration_cache.cpp)

# ---------- Phase 3: shell-extension DLL smoke ----------------------------
# Runs after the DLL is built; locates it via $<TARGET_FILE:WinStellarShellExt>
# at test time. The test only verifies the DLL exposes the COM entry points;
# it never calls regsvr32 (would require admin).
add_executable(smoke_shell_ext smoke_shell_ext.cpp)
target_link_libraries(smoke_shell_ext PRIVATE
    GTest::gtest
    GTest::gtest_main
)
set_target_properties(smoke_shell_ext PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
)
add_dependencies(smoke_shell_ext WinStellarShellExt)
target_compile_definitions(smoke_shell_ext PRIVATE
    WINSTELLAR_SHELL_EXT_PATH="$<TARGET_FILE:WinStellarShellExt>"
)
gtest_discover_tests(smoke_shell_ext
    PROPERTIES LABELS "winstellar;smoke"
    DISCOVERY_TIMEOUT 30
)

# ---------- Phase 4: viewer --check smoke ---------------------------------
# Launches WinStellar.exe --check <synthetic FITS> as a child process and
# verifies the exit code. The path is baked in at compile time via a generator
# expression so it stays correct under multi-config (Debug/Release).
add_executable(smoke_viewer smoke_viewer.cpp)
target_link_libraries(smoke_viewer PRIVATE
    winstellar_test_helpers
    GTest::gtest
    GTest::gtest_main
)
set_target_properties(smoke_viewer PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
)
add_dependencies(smoke_viewer WinStellar)
target_compile_definitions(smoke_viewer PRIVATE
    WINSTELLAR_EXE_PATH="$<TARGET_FILE:WinStellar>"
)
gtest_discover_tests(smoke_viewer
    PROPERTIES LABELS "winstellar;smoke"
    DISCOVERY_TIMEOUT 60
)
