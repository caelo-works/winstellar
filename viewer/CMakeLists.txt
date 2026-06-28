add_executable(WinStellar WIN32
    src/main.cpp
    src/ViewerWindow.cpp
    src/HeaderView.cpp
    src/AnalysisView.cpp
    src/Toolbar.cpp
    src/Histogram.cpp
    src/AberrationView.cpp
    src/TiltView.cpp
    src/BackgroundView.cpp
    src/viewer.rc
)

# Make Ninja re-build viewer.rc whenever the manifest XML changes — by default
# CMake/Ninja only depend on the .rc file itself, not on referenced assets.
set_property(SOURCE src/viewer.rc APPEND PROPERTY OBJECT_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/viewer.manifest")

target_include_directories(WinStellar PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(WinStellar PRIVATE
    fits_core
    d2d1
    dwrite
    dwmapi
    uxtheme
    windowscodecs
    comctl32
    shlwapi
    ole32
    uuid
    user32
    gdi32
)

set_target_properties(WinStellar PROPERTIES
    OUTPUT_NAME "WinStellar"
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
)
if(WINSTELLAR_IPO_OK)
    set_property(TARGET WinStellar
                 PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
endif()

# Our viewer.rc embeds a manifest (DPI-aware, common-controls v6). Stop the
# linker from also auto-generating one, which would fail with CVT1100 on
# duplicate MANIFEST resource.
target_link_options(WinStellar PRIVATE "/MANIFEST:NO")
