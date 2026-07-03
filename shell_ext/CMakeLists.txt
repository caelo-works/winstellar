add_library(WinStellarShellExt SHARED
    src/dllmain.cpp
    src/ClassFactory.cpp
    src/PreviewHandler.cpp
    src/PropertyHandler.cpp
    src/ThumbnailProvider.cpp
    src/Registration.cpp
    src/StreamBuffer.cpp
    src/shell_ext.def
    src/shell_ext.rc
)

target_include_directories(WinStellarShellExt PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(WinStellarShellExt PRIVATE
    fits_core
    propsys
    shlwapi
    shell32
    advapi32
    ole32
    oleaut32
    uuid
    user32
    gdi32
)

# Harden the DLL search order for the vendored dependency DLLs (#5). This DLL is
# loaded in-process inside explorer.exe; by default its implicit imports would be
# resolved via a search that includes the process CWD and PATH, so a planted
# cfitsio.dll / sqlite3.dll / pugixml.dll / raw_r.dll could be loaded into
# Explorer. Delay-load them and resolve each from this module's own directory via
# the __pfnDliNotifyHook2 hook in dllmain.cpp (which also protects their
# transitive deps: z.dll, pthreadVC3.dll, lcms2-2.dll).
if(MSVC)
    target_link_libraries(WinStellarShellExt PRIVATE delayimp)
    target_link_options(WinStellarShellExt PRIVATE
        /DELAYLOAD:cfitsio.dll
        /DELAYLOAD:sqlite3.dll
        /DELAYLOAD:pugixml.dll
        /DELAYLOAD:raw_r.dll
    )
endif()

set_target_properties(WinStellarShellExt PROPERTIES
    OUTPUT_NAME "WinStellarShellExt"
    PREFIX ""
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
)
if(WINSTELLAR_IPO_OK)
    set_property(TARGET WinStellarShellExt
                 PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
endif()

# Always-run custom target that stages FitsProps.propdesc next to the DLL.
# We use add_custom_target ALL (not add_custom_command POST_BUILD on the DLL)
# because the DLL doesn't get rebuilt when only the .propdesc changes - and
# OUTPUT in add_custom_command can't evaluate $<TARGET_FILE_DIR:...> at
# configure time. copy_if_different makes the always-run cheap when nothing
# changed.
add_custom_target(stage_propdesc ALL
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/src/FitsProps.propdesc"
        "$<TARGET_FILE_DIR:WinStellarShellExt>/FitsProps.propdesc"
    COMMENT "Staging FitsProps.propdesc next to the DLL"
    VERBATIM
)
add_dependencies(stage_propdesc WinStellarShellExt)
