# Copyright (c) 2026 Jakub Melka and Contributors
# SPDX-License-Identifier: MIT
# See LICENSE file in the project root for full license text.

# Consumer project for the exyokioffice vcpkg port. It is deliberately outside
# the library build: it configures against an installed vcpkg tree and uses only
# find_package, so it fails exactly when the port would fail a real consumer.

cmake_minimum_required(VERSION 3.25)

project(ExyokiOfficeVcpkgTest LANGUAGES CXX)

# vcpkg's consumer toolchain deliberately leaves the CRT to the project: it sets
# CMAKE_MSVC_RUNTIME_LIBRARY only while building ports. A `-static` triplet ships
# packages linked against the static runtime, so a consumer that does not match
# it fails at link time with LNK2038. `-static-md` keeps the dynamic runtime and
# is therefore excluded.
if(MSVC AND VCPKG_TARGET_TRIPLET MATCHES "-static$")
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

find_package(ExyokiOffice 1.1 CONFIG REQUIRED)

add_executable(ExyokiOfficeVcpkgSmoke main.cpp)
target_compile_features(ExyokiOfficeVcpkgSmoke PRIVATE cxx_std_20)
target_link_libraries(ExyokiOfficeVcpkgSmoke PRIVATE ExyokiOffice::ExyokiOffice)

enable_testing()

add_test(NAME Vcpkg.Library COMMAND ExyokiOfficeVcpkgSmoke)

# The tools and mcp features are optional, so their entries are registered only
# when the port actually installed them. A missing executable is not a failure
# here - it means the package was installed without the feature.
set(EXYOKIOFFICE_TOOLS_DIR "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/tools/exyokioffice")

function(exyokioffice_add_tool_test name executable)
    find_program(${name}_EXECUTABLE
        NAMES ${executable}
        PATHS "${EXYOKIOFFICE_TOOLS_DIR}"
        NO_DEFAULT_PATH)

    if(${name}_EXECUTABLE)
        # Each executable answers --version with "<name> <version>", so the
        # entry also proves the tool was linked against the package it shipped in.
        add_test(NAME ${name} COMMAND "${${name}_EXECUTABLE}" --version)
        set_tests_properties(${name} PROPERTIES
            PASS_REGULAR_EXPRESSION "${executable} ${ExyokiOffice_VERSION}")
        message(STATUS "vcpkg test - registered ${name} for ${${name}_EXECUTABLE}")
    else()
        message(STATUS "vcpkg test - ${executable} is not installed, skipping ${name}")
    endif()
endfunction()

exyokioffice_add_tool_test(Vcpkg.Tool.Version exyoki)
exyokioffice_add_tool_test(Vcpkg.Mcp.Word exyoki-mcp-word)
exyokioffice_add_tool_test(Vcpkg.Mcp.Excel exyoki-mcp-excel)
exyokioffice_add_tool_test(Vcpkg.Mcp.PowerPoint exyoki-mcp-power-point)
