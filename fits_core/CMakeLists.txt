find_package(unofficial-sqlite3 CONFIG REQUIRED)
find_package(pugixml CONFIG REQUIRED)
find_package(libraw CONFIG REQUIRED)

add_library(fits_core STATIC
    src/fits_loader.cpp
    src/fits_stretch.cpp
    src/fits_render.cpp
    src/fits_debayer.cpp
    src/fits_headers.cpp
    src/xisf_loader.cpp
    src/raw_loader.cpp
    src/analysis.cpp
    src/cache.cpp
)

target_include_directories(fits_core
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(fits_core
    PUBLIC CFITSIO::cfitsio
    PRIVATE
        unofficial::sqlite3::sqlite3
        pugixml::pugixml
        libraw::raw_r
)

set_target_properties(fits_core PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
)
if(WINSTELLAR_IPO_OK)
    set_property(TARGET fits_core
                 PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
endif()
