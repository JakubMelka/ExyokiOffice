# Copyright (c) 2026 Jakub Melka and Contributors
# SPDX-License-Identifier: MIT
# See LICENSE file in the project root for full license text.

include_guard(GLOBAL)

# Locates the compiler-rt library directory shipping with the active Clang.
#
# clang-cl knows where its own runtime lives, but only the driver does; the
# path never reaches CMake on its own. -print-resource-dir returns the resource
# root, and the Windows runtime libraries sit in lib/windows below it. This is
# needed by both the sanitizer and the fuzzer link steps, hence a shared helper.
function(exyokioffice_clang_runtime_dir outVariable)
    execute_process(
        COMMAND "${CMAKE_CXX_COMPILER}" -print-resource-dir
        OUTPUT_VARIABLE _eo_resource_dir
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE _eo_resource_result)

    if(NOT _eo_resource_result EQUAL 0 OR _eo_resource_dir STREQUAL "")
        message(FATAL_ERROR
            "Could not determine the Clang resource directory by running "
            "'${CMAKE_CXX_COMPILER} -print-resource-dir'")
    endif()

    file(TO_CMAKE_PATH "${_eo_resource_dir}/lib/windows" _eo_runtime_dir)
    if(NOT IS_DIRECTORY "${_eo_runtime_dir}")
        message(FATAL_ERROR
            "Clang runtime directory '${_eo_runtime_dir}' does not exist; the "
            "installation appears to be missing its compiler-rt libraries")
    endif()

    set(${outVariable} "${_eo_runtime_dir}" PARENT_SCOPE)
endfunction()

set(EXYOKIOFFICE_SANITIZER "none" CACHE STRING
    "Sanitizer instrumentation applied to the whole build: none, address, undefined, address+undefined, thread")
set_property(CACHE EXYOKIOFFICE_SANITIZER PROPERTY STRINGS
    none address undefined "address+undefined" thread)

# Implemented as a macro on purpose: the sanitizer flags must land in the
# calling (root) directory scope and the MSVC-specific edits below rewrite the
# CMAKE_<LANG>_FLAGS_<CONFIG> variables of that scope.
macro(exyokioffice_apply_sanitizers)
    string(TOLOWER "${EXYOKIOFFICE_SANITIZER}" _eo_san)

    if(_eo_san STREQUAL "" OR _eo_san STREQUAL "none" OR _eo_san STREQUAL "off")
        set(_eo_san "none")
    else()
        set(_eo_san_address OFF)
        set(_eo_san_undefined OFF)
        set(_eo_san_thread OFF)

        if(_eo_san STREQUAL "address")
            set(_eo_san_address ON)
        elseif(_eo_san STREQUAL "undefined")
            set(_eo_san_undefined ON)
        elseif(_eo_san STREQUAL "address+undefined")
            set(_eo_san_address ON)
            set(_eo_san_undefined ON)
        elseif(_eo_san STREQUAL "thread")
            set(_eo_san_thread ON)
        else()
            message(FATAL_ERROR
                "EXYOKIOFFICE_SANITIZER='${EXYOKIOFFICE_SANITIZER}' is not supported; "
                "use none, address, undefined, address+undefined or thread")
        endif()

        if(MSVC)
            # Covers both cl.exe and clang-cl: only AddressSanitizer has a
            # working MSVC-style driver and runtime on Windows.
            if(_eo_san_undefined OR _eo_san_thread)
                message(FATAL_ERROR
                    "Only EXYOKIOFFICE_SANITIZER=address is supported with an MSVC-style "
                    "compiler; requested '${EXYOKIOFFICE_SANITIZER}'")
            endif()

            add_compile_options(/fsanitize=address)

            if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
                # cl.exe embeds #pragma comment(lib) references to the ASan
                # runtime, so its linker resolves them on its own. clang-cl does
                # not: it expects the runtime to be added by the clang driver at
                # link time, and CMake invokes lld-link directly instead. The
                # libraries therefore have to be named explicitly, otherwise
                # every target fails with undefined __asan_* symbols.
                exyokioffice_clang_runtime_dir(_eo_san_rt_dir)
                add_link_options(
                    "/LIBPATH:${_eo_san_rt_dir}"
                    clang_rt.asan_dynamic-x86_64.lib
                    clang_rt.asan_dynamic_runtime_thunk-x86_64.lib)
                unset(_eo_san_rt_dir)
            endif()

            # /RTC is rejected together with /fsanitize=address and ASan cannot
            # be combined with incremental linking.
            foreach(_eo_san_flags
                    CMAKE_C_FLAGS_DEBUG
                    CMAKE_CXX_FLAGS_DEBUG
                    CMAKE_C_FLAGS_RELWITHDEBINFO
                    CMAKE_CXX_FLAGS_RELWITHDEBINFO)
                string(REGEX REPLACE "/RTC[1csu]+" "" ${_eo_san_flags} "${${_eo_san_flags}}")
            endforeach()
            unset(_eo_san_flags)

            add_link_options(/INCREMENTAL:NO)
        elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
            set(_eo_san_flags -fno-omit-frame-pointer -g)

            if(_eo_san_address)
                list(APPEND _eo_san_flags -fsanitize=address)
            endif()

            if(_eo_san_undefined)
                # vptr is excluded: it needs full RTTI visibility across the
                # shared library boundary and reports on the generated DOM.
                list(APPEND _eo_san_flags
                    -fsanitize=undefined
                    -fno-sanitize=vptr
                    -fno-sanitize-recover=all)
            endif()

            if(_eo_san_thread)
                list(APPEND _eo_san_flags -fsanitize=thread)
            endif()

            add_compile_options(${_eo_san_flags})
            add_link_options(${_eo_san_flags})
            unset(_eo_san_flags)
        else()
            message(FATAL_ERROR
                "No sanitizer profile is defined for ${CMAKE_CXX_COMPILER_ID}")
        endif()

        unset(_eo_san_address)
        unset(_eo_san_undefined)
        unset(_eo_san_thread)
    endif()

    message(STATUS "Sanitizer - EXYOKIOFFICE_SANITIZER: ${_eo_san}")
    unset(_eo_san)
endmacro()
