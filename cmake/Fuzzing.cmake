# Copyright (c) 2026 Jakub Melka and Contributors
# SPDX-License-Identifier: MIT
# See LICENSE file in the project root for full license text.

include_guard(GLOBAL)

# Coverage instrumentation for libFuzzer.
#
# Two rules shape everything below.
#
# First, instrumentation is applied per target rather than to the whole build.
# Coverage counters emit calls to __sanitizer_cov_*, which only the libFuzzer
# runtime defines. Instrumenting the source generator - a build-time tool that
# is never fuzzed - would therefore break its link for no benefit. Only the
# library and the fuzz entry points, both of which end up inside a fuzz
# executable, get instrumented.
#
# Second, the runtime that provides those symbols is linked into the fuzz
# executables alone, because it carries main(). Instrumentation without the
# runtime is spelled -fsanitize=fuzzer-no-link for Clang, and for cl.exe as the
# individual /fsanitize-coverage= flags, since its /fsanitize=fuzzer would pull
# the runtime in through #pragma comment(lib).

# Validates the fuzzing configuration. Reports the resulting mode.
macro(exyokioffice_validate_fuzzing_configuration)
    if(EXYOKIOFFICE_BUILD_FUZZERS)
        string(TOLOWER "${EXYOKIOFFICE_SANITIZER}" _eo_fuzz_san)
        if(NOT _eo_fuzz_san MATCHES "address")
            message(FATAL_ERROR
                "EXYOKIOFFICE_BUILD_FUZZERS=ON requires EXYOKIOFFICE_SANITIZER to include "
                "'address'; a fuzzer without AddressSanitizer detects almost nothing. "
                "Requested '${EXYOKIOFFICE_SANITIZER}'.")
        endif()
        unset(_eo_fuzz_san)

        if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
            message(FATAL_ERROR
                "EXYOKIOFFICE_BUILD_FUZZERS=ON needs Clang or MSVC; GCC has no libFuzzer driver")
        elseif(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND NOT MSVC)
            message(FATAL_ERROR
                "No fuzzing profile is defined for ${CMAKE_CXX_COMPILER_ID}")
        endif()
    endif()

    message(STATUS "Fuzzing - EXYOKIOFFICE_BUILD_FUZZERS: ${EXYOKIOFFICE_BUILD_FUZZERS}")
endmacro()

# Adds coverage instrumentation to one target. A no-op unless fuzzing is on.
function(exyokioffice_enable_fuzzing_instrumentation target)
    if(NOT EXYOKIOFFICE_BUILD_FUZZERS)
        return()
    endif()

    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        # Accepted by clang-cl as well as by clang.
        target_compile_options(${target} PRIVATE -fsanitize=fuzzer-no-link)
    elseif(MSVC)
        target_compile_options(${target} PRIVATE
            /fsanitize-coverage=inline-8bit-counters
            /fsanitize-coverage=edge
            /fsanitize-coverage=trace-cmp
            /fsanitize-coverage=trace-div)
    endif()
endfunction()

# Declares one libFuzzer executable driving a single entry point.
#
# name        short target name used on the command line and for the corpus,
#             dictionary and crash directories (for example "flatopc")
# entrySource translation unit providing LLVMFuzzerTestOneInput
function(exyokioffice_add_fuzz_target name entrySource)
    set(_target ExyokiOfficeFuzz_${name})

    add_executable(${_target} ${entrySource})
    target_link_libraries(${_target} PRIVATE ExyokiOfficeFuzzTargets)
    exyokioffice_enable_strict_warnings(${_target})
    exyokioffice_enable_fuzzing_instrumentation(${_target})

    if(MSVC AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        # CMake drives lld-link directly for clang-cl, and lld-link would not
        # understand -fsanitize=fuzzer, so the runtime carrying main() has to be
        # named outright. Same reasoning as the ASan runtime in Sanitizers.cmake.
        #
        # The library deliberately comes from the MSVC toolset rather than from
        # the LLVM tools: LLVM ships clang_rt.fuzzer-x86_64.lib built against
        # the static CRT, which /failifmismatch rejects against this build's
        # dynamic CRT. Microsoft ships the same compiler-rt code in per-CRT
        # flavors, and the MSVC lib directory is already on the linker search
        # path inside a developer environment.
        target_link_options(${_target} PRIVATE
            "$<IF:$<CONFIG:Debug>,clang_rt.fuzzer_MDd-x86_64.lib,clang_rt.fuzzer_MD-x86_64.lib>")
    elseif(MSVC)
        # cl.exe pulls clang_rt.fuzzer_MD*.lib in through #pragma comment(lib).
        target_compile_options(${_target} PRIVATE /fsanitize=fuzzer)
    else()
        target_link_options(${_target} PRIVATE -fsanitize=fuzzer)
    endif()

    set_target_properties(${_target} PROPERTIES OUTPUT_NAME fuzz_${name})
endfunction()
