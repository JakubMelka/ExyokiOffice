# Copyright (c) 2026 Jakub Melka and Contributors
# SPDX-License-Identifier: MIT
# See LICENSE file in the project root for full license text.

include_guard(GLOBAL)

# Off by default, and deliberately so. A warning that is an error is a useful
# rule for the people who develop this library and a trap for everyone who only
# wants to build it: every new compiler generation invents diagnostics, so a
# default of ON means a toolchain newer than the ones CI covers turns a warning
# nobody has seen yet into a failed build half an hour in. CI (.github/workflows
# /ci.yml) and WinBuild.ps1 pass ON explicitly, so the strictness stays where it
# belongs - on the builds that are meant to police it.
option(EXYOKIOFFICE_WARNINGS_AS_ERRORS
    "Treat compiler warnings in ExyokiOffice handwritten code as errors"
    OFF)

function(exyokioffice_enable_strict_warnings target)
    set(msvc_warnings
        /W4
        /permissive-
        /w14242
        /w14254
        /w14263
        /w14265
        /w14287
        /w14296
        /w14311
        /w14545
        /w14546
        /w14547
        /w14549
        /w14555
        /w14619
        /w14640
        /w14826
        /w14905
        /w14906
        /w14928
        # Exported API intentionally uses STL value types. C4251 would require
        # a separate ABI/PImpl redesign and has no GCC/Clang equivalent.
        /wd4251
        # Generated OpenXML declarations contain class/struct tag mismatches.
        /wd4099)

    set(gcc_clang_warnings
        -Wall
        -Wextra
        -Wpedantic
        -Wcast-align
        -Wcast-qual
        -Wconversion
        -Wformat=2
        -Wnon-virtual-dtor
        -Wold-style-cast
        -Woverloaded-virtual
        -Wshadow
        -Wsign-conversion
        -Wundef)

    get_target_property(target_sources ${target} SOURCES)
    set(handwritten_sources)
    foreach(source IN LISTS target_sources)
        string(REPLACE "\\" "/" normalized_source "${source}")
        if(normalized_source MATCHES "\\.cpp$"
           AND NOT normalized_source MATCHES "sources/DOM/"
           AND NOT normalized_source MATCHES "sources/pugixml/"
           AND NOT normalized_source MATCHES "sources/zip/"
           AND NOT normalized_source MATCHES "3rdparty/"
           AND NOT normalized_source MATCHES "sources/Packaging/Generated.*\\.cpp$"
           AND NOT normalized_source MATCHES "sources/Packaging/OpenXmlPackageFactory\\.cpp$")
            list(APPEND handwritten_sources "${source}")
        endif()
    endforeach()

    if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
        set(strict_warnings ${msvc_warnings})
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang"
           AND CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC")
        set(strict_warnings
            /W4
            /permissive-
            /clang:-Wcast-align
            /clang:-Wcast-qual
            /clang:-Wconversion
            /clang:-Wformat=2
            /clang:-Wnon-virtual-dtor
            /clang:-Wold-style-cast
            /clang:-Woverloaded-virtual
            /clang:-Wshadow
            /clang:-Wsign-conversion
            /clang:-Wundef)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
        set(strict_warnings ${gcc_clang_warnings})
    else()
        message(WARNING
            "No strict warning profile is defined for ${CMAKE_CXX_COMPILER_ID}; using the compiler defaults")
    endif()

    if(EXYOKIOFFICE_WARNINGS_AS_ERRORS)
        if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC"
           OR CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC")
            list(APPEND strict_warnings /WX)
        elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
            list(APPEND strict_warnings -Werror)
        endif()
    endif()

    set_source_files_properties(${handwritten_sources} PROPERTIES
        COMPILE_OPTIONS "${strict_warnings}")
endfunction()
