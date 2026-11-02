cmake_minimum_required(VERSION 3.16)

if(APPLE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "10.13" CACHE STRING "Minimum macOS version")
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "macOS architectures (universal by default)")
endif()

project(reaper_drop_to_sampler LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Static MSVC runtime so clean PCs do not need VCRUNTIME140.dll
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Plugin version: release CI passes the git tag without "v" (-DDTS_VERSION=0.6.3)
set(DTS_VERSION "0.6.3" CACHE STRING "Plugin version (x.y.z)")
if(NOT DTS_VERSION MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)(-[0-9A-Za-z.-]+)?$")
    message(FATAL_ERROR "DTS_VERSION must look like x.y.z or x.y.z-suffix, got '${DTS_VERSION}'")
endif()
set(DTS_VERSION_MAJOR ${CMAKE_MATCH_1})
set(DTS_VERSION_MINOR ${CMAKE_MATCH_2})
set(DTS_VERSION_PATCH ${CMAKE_MATCH_3})
configure_file(src/version.h.in ${CMAKE_CURRENT_BINARY_DIR}/version.h @ONLY)

add_library(reaper_drop_to_sampler SHARED
    src/main.cpp
    src/drop.cpp
    src/prompts.cpp
    src/sampler.cpp
    src/settings.cpp
    src/settings_page.cpp
    src/tracks.cpp
)

target_include_directories(reaper_drop_to_sampler PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_BINARY_DIR}
)

set_target_properties(reaper_drop_to_sampler PROPERTIES
    PREFIX ""
    OUTPUT_NAME "reaper_drop_to_sampler"
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
)

if(WIN32)
    enable_language(RC)
    set(CMAKE_RC_FLAGS "${CMAKE_RC_FLAGS} -I${CMAKE_CURRENT_SOURCE_DIR}/src -I${CMAKE_CURRENT_BINARY_DIR}")

    target_sources(reaper_drop_to_sampler PRIVATE
        src/resource.rc
        src/platform/win/drop_target_win.cpp
        src/platform/win/overlay_win.cpp
        src/platform/win/platform_win.cpp
    )
    # Same name as the release asset, so a local deploy replaces the ReaPack install
    set_target_properties(reaper_drop_to_sampler PROPERTIES OUTPUT_NAME "reaper_drop_to_sampler-x64" SUFFIX ".dll")

    if(MSVC)
        target_compile_options(reaper_drop_to_sampler PRIVATE /utf-8)
        target_compile_definitions(reaper_drop_to_sampler PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()

    set(WIN_LIBS user32 gdi32 shell32 ole32 gdiplus msimg32)
    if(MINGW)
        target_compile_options(reaper_drop_to_sampler PRIVATE -pthread)
        # ReaperPluginEntry is already __declspec(dllexport) — no .def file needed.
        # Static-link MinGW runtime so clean PCs do not need libwinpthread-1.dll etc.
        target_link_libraries(reaper_drop_to_sampler PRIVATE
            -static-libgcc
            -static-libstdc++
            "-Wl,-Bstatic"
            -lstdc++
            -lpthread
            -lwinpthread
            "-Wl,-Bdynamic"
            ${WIN_LIBS}
        )
    else()
        target_link_libraries(reaper_drop_to_sampler PRIVATE ${WIN_LIBS})
    endif()

else()
    if(NOT APPLE AND NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
        message(FATAL_ERROR "Only Windows, macOS and Linux are supported")
    endif()

    # SWELL (Win32 API emulation provided by REAPER) from the WDL submodule
    set(WDL_DIR ${CMAKE_CURRENT_SOURCE_DIR}/vendor/WDL/WDL)
    if(NOT EXISTS ${WDL_DIR}/swell/swell.h)
        message(FATAL_ERROR "WDL not found. Run: git submodule update --init")
    endif()

    # Settings dialog from resource.rc, converted to SWELL format
    find_package(Perl REQUIRED)
    set(RESGEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/resgen)
    add_custom_command(
        OUTPUT ${RESGEN_DIR}/resource.rc_mac_dlg ${RESGEN_DIR}/resource.rc_mac_menu
        COMMAND ${CMAKE_COMMAND} -E make_directory ${RESGEN_DIR}
        COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/src/resource.rc ${RESGEN_DIR}/resource.rc
        COMMAND ${PERL_EXECUTABLE} ${WDL_DIR}/swell/swell_resgen.pl --quiet ${RESGEN_DIR}/resource.rc
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/resource.rc
        COMMENT "Generating SWELL dialog resources"
    )
    target_sources(reaper_drop_to_sampler PRIVATE
        ${RESGEN_DIR}/resource.rc_mac_dlg
        src/platform/posix/files_posix.cpp
    )
    set_source_files_properties(${RESGEN_DIR}/resource.rc_mac_dlg PROPERTIES HEADER_FILE_ONLY ON)

    # "../WDL/swell/swell.h" in reaper_plugin.h resolves against ${WDL_DIR}
    target_include_directories(reaper_drop_to_sampler PRIVATE ${WDL_DIR} ${RESGEN_DIR})
    target_compile_definitions(reaper_drop_to_sampler PRIVATE SWELL_PROVIDED_BY_APP NOMINMAX)

    # Overlay PNGs embedded as byte arrays (Windows uses RCDATA instead)
    set(EMBED_HEADER ${CMAKE_CURRENT_BINARY_DIR}/overlay_images.h)
    file(WRITE ${EMBED_HEADER} "#pragma once\n// Generated from src/res/*.png\n")
    foreach(name sampler_insert sampler_replace)
        set(png ${CMAKE_CURRENT_SOURCE_DIR}/src/res/${name}.png)
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${png})
        file(READ ${png} hex HEX)
        string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
        file(APPEND ${EMBED_HEADER} "static const unsigned char k_${name}_png[] = {${bytes}};\n")
    endforeach()

    if(APPLE)
        enable_language(OBJCXX)

        target_sources(reaper_drop_to_sampler PRIVATE
            src/platform/mac/drop_target_mac.mm
            src/platform/mac/overlay_mac.mm
            src/platform/mac/platform_mac.mm
            ${WDL_DIR}/swell/swell-modstub.mm
        )
        target_link_libraries(reaper_drop_to_sampler PRIVATE "-framework AppKit")
        set_target_properties(reaper_drop_to_sampler PROPERTIES SUFFIX ".dylib")
    else()
        # Same name as the release asset, so a local deploy replaces the ReaPack install
        set(DTS_LINUX_ARCH ${CMAKE_SYSTEM_PROCESSOR})
        if(DTS_LINUX_ARCH STREQUAL "amd64")
            set(DTS_LINUX_ARCH x86_64)
        endif()

        target_sources(reaper_drop_to_sampler PRIVATE
            src/platform/linux/drop_target_linux.cpp
            src/platform/linux/overlay_linux.cpp
            src/platform/linux/platform_linux.cpp
            ${WDL_DIR}/swell/swell-modstub-generic.cpp
        )
        target_link_libraries(reaper_drop_to_sampler PRIVATE ${CMAKE_DL_LIBS})
        # Static runtime so it loads on distros with an older libstdc++; keep its symbols private
        target_link_options(reaper_drop_to_sampler PRIVATE -static-libstdc++ -static-libgcc -Wl,--exclude-libs,ALL)
        set_target_properties(reaper_drop_to_sampler PROPERTIES
            OUTPUT_NAME "reaper_drop_to_sampler-${DTS_LINUX_ARCH}"
            SUFFIX ".so"
        )
    endif()
endif()
