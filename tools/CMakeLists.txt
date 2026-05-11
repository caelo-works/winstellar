add_executable(xisf_bench xisf_bench.cpp)
target_link_libraries(xisf_bench PRIVATE fits_core)
set_target_properties(xisf_bench PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
)
