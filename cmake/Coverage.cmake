# Copyright (c) 2026 Jakub Melka and Collaborators
# SPDX-License-Identifier: MIT
# See LICENSE file in the project root for full license text.

include_guard(GLOBAL)

# LLVM source-based code coverage.
#
# The instrumentation is Clang's own (-fprofile-instr-generate together with
# -fcoverage-mapping), not gcov and not a PDB-reading profiler: the counters are
# emitted by the front end alongside a mapping from counter to source range, so
# an optimized build still attributes hits to the region they belong to. That is
# what makes the RelWithDebInfo coverage preset usable at all - a -O0 build of
# this library costs roughly six times the running time, per WinBuild.ps1.
#
# Applied to the whole directory scope rather than per target, for the same
# reason the sanitizers are: the counters have to reach the library, the test
# executables and everything they link, and a module compiled without the
# mapping simply disappears from the report rather than showing up as uncovered.
#
# The library stays SHARED here on purpose. A static coverage build would let
# /OPT:REF drop every function no test calls, and those are exactly the functions
# a coverage report exists to show; the DLL keeps the whole library in the
# report. The cost is that the DLL and each test executable carry their own copy
# of the profile runtime and write their own raw profile, which is what the %m
# in the LLVM_PROFILE_FILE pattern of WinCoverage.ps1 is for.

option(EXYOKIOFFICE_COVERAGE
    "Instrument the build for LLVM source-based code coverage (Clang or clang-cl only)" OFF)

# Modified condition/decision coverage: for every boolean decision, whether the
# tests showed each condition independently affecting the outcome. It is the
# criterion DO-178C level A asks for, and it is strictly harder to satisfy than
# branch coverage - a decision can have all its branches taken and still no
# condition proven independent.
#
# Kept separate from EXYOKIOFFICE_COVERAGE, and off by default, because it is
# not free: the instrumentation adds a test-vector bitmap per decision on top of
# the counters, which costs binary size, run time and raw profile size that a
# plain line-coverage question does not need to pay.
option(EXYOKIOFFICE_COVERAGE_MCDC
    "Add MC/DC instrumentation to the coverage build; needs EXYOKIOFFICE_COVERAGE" OFF)

# Implemented as a macro for the same reason exyokioffice_apply_sanitizers is:
# the flags must land in the calling (root) directory scope.
macro(exyokioffice_apply_coverage)
    if(EXYOKIOFFICE_COVERAGE)
        if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
            message(FATAL_ERROR
                "EXYOKIOFFICE_COVERAGE=ON needs Clang or clang-cl; source-based coverage "
                "has no cl.exe equivalent. Configure with the "
                "windows-ninja-clang-coverage preset, or use OpenCppCoverage against an "
                "ordinary MSVC build instead. Found ${CMAKE_CXX_COMPILER_ID}.")
        endif()

        # Restricted to the compiled languages: add_compile_options also feeds
        # the Windows resource compiler, which does not understand these.
        add_compile_options(
            "$<$<COMPILE_LANGUAGE:C,CXX>:-fprofile-instr-generate>"
            "$<$<COMPILE_LANGUAGE:C,CXX>:-fcoverage-mapping>")

        if(EXYOKIOFFICE_COVERAGE_MCDC)
            add_compile_options("$<$<COMPILE_LANGUAGE:C,CXX>:-fcoverage-mcdc>")
        endif()

        if(MSVC)
            # Same story as the ASan runtime in Sanitizers.cmake: CMake drives
            # lld-link directly for clang-cl, so the compiler-rt library that
            # defines __llvm_profile_* and writes the raw profile at exit has to
            # be named outright.
            #
            # Unlike the libFuzzer runtime this one is taken from the LLVM tools
            # rather than from the MSVC toolset. It ships in a single flavor with
            # no CRT-specific variants, so the /failifmismatch that rules out
            # LLVM's clang_rt.fuzzer does not arise, and matching the runtime to
            # the llvm-profdata sitting next to it keeps the raw profile format
            # and the reader in step.
            exyokioffice_clang_runtime_dir(_eo_cov_rt_dir)
            add_link_options(
                "/LIBPATH:${_eo_cov_rt_dir}"
                clang_rt.profile-x86_64.lib)
            unset(_eo_cov_rt_dir)
        else()
            add_link_options(-fprofile-instr-generate)
        endif()
    elseif(EXYOKIOFFICE_COVERAGE_MCDC)
        message(FATAL_ERROR
            "EXYOKIOFFICE_COVERAGE_MCDC=ON needs EXYOKIOFFICE_COVERAGE=ON; MC/DC is an "
            "addition to the source-based coverage instrumentation, not a mode of its own.")
    endif()

    message(STATUS "Coverage - EXYOKIOFFICE_COVERAGE: ${EXYOKIOFFICE_COVERAGE}")
    message(STATUS "Coverage - EXYOKIOFFICE_COVERAGE_MCDC: ${EXYOKIOFFICE_COVERAGE_MCDC}")
endmacro()
