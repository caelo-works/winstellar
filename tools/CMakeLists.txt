add_executable(xisf_bench xisf_bench.cpp)
target_link_libraries(xisf_bench PRIVATE fits_core)
set_target_properties(xisf_bench PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
)

# Camera-RAW decode benchmark. Links LibRaw directly (fits_core keeps it
# PRIVATE) so it can probe per-stage timings under different demosaic settings.
find_package(libraw CONFIG REQUIRED)
add_executable(raw_bench raw_bench.cpp)
target_link_libraries(raw_bench PRIVATE libraw::raw_r)
set_target_properties(raw_bench PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
)
