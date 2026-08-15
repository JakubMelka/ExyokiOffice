# Copyright (c) 2026 Jakub Melka and Contributors
# SPDX-License-Identifier: MIT
# See LICENSE file in the project root for full license text.

# The fuzz entry points live in a plain library rather than inside the libFuzzer
# executables. That way the same code has two consumers: the fuzz binaries built
# only under EXYOKIOFFICE_BUILD_FUZZERS, and the corpus replay test in
# tests/unit, which runs every committed corpus and crash input in an ordinary
# MSVC build with no libFuzzer and no Clang involved. A crash artifact committed
# under crashes/<target>/ therefore becomes a regression test automatically.
add_library(ExyokiOfficeFuzzTargets STATIC
    FuzzTargets.cpp
    Targets/FlatOpcFuzzer.cpp
    Targets/PackageLoadFuzzer.cpp
    Targets/XmlPartFuzzer.cpp
    Targets/SimpleTypesFuzzer.cpp
    Targets/FormulaFuzzer.cpp
    Targets/CopyOpsFuzzer.cpp)

target_link_libraries(ExyokiOfficeFuzzTargets PUBLIC ExyokiOffice)
target_include_directories(ExyokiOfficeFuzzTargets PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
exyokioffice_enable_strict_warnings(ExyokiOfficeFuzzTargets)
exyokioffice_enable_fuzzing_instrumentation(ExyokiOfficeFuzzTargets)

# Consumers locate the committed corpus through this path.
target_compile_definitions(ExyokiOfficeFuzzTargets PUBLIC
    EXYOKIOFFICE_FUZZ_DIR="${CMAKE_CURRENT_SOURCE_DIR}")

# The MCP protocol surface is the only code an untrusted client reaches
# directly, so it gets a target of its own. exyoki-mcp-core is declared later in
# the root CMakeLists.txt; target_link_libraries resolves a target name at
# generate time, so the option is what has to be tested here - if(TARGET ...)
# would still be false at this point. The Word toolset lives outside the core
# library, exactly as tests/mcp compiles it in.
if(EXYOKIOFFICE_BUILD_MCP)
    target_sources(ExyokiOfficeFuzzTargets PRIVATE
        Targets/McpRpcFuzzer.cpp
        ${PROJECT_SOURCE_DIR}/tools/mcp/word/WordToolset.cpp
        ${PROJECT_SOURCE_DIR}/tools/mcp/word/WordAddressing.cpp)
    target_link_libraries(ExyokiOfficeFuzzTargets PUBLIC exyoki-mcp-core)
    target_include_directories(ExyokiOfficeFuzzTargets PRIVATE
        ${PROJECT_SOURCE_DIR}/tools/mcp/word)
    target_compile_definitions(ExyokiOfficeFuzzTargets PUBLIC EXYOKIOFFICE_FUZZ_HAS_MCP=1)
endif()

if(EXYOKIOFFICE_BUILD_FUZZERS)
    exyokioffice_add_fuzz_target(flatopc Entry/FlatOpcEntry.cpp)
    exyokioffice_add_fuzz_target(packageload Entry/PackageLoadEntry.cpp)
    exyokioffice_add_fuzz_target(xmlpart Entry/XmlPartEntry.cpp)
    exyokioffice_add_fuzz_target(simpletypes Entry/SimpleTypesEntry.cpp)
    exyokioffice_add_fuzz_target(formula Entry/FormulaEntry.cpp)
    exyokioffice_add_fuzz_target(copyops Entry/CopyOpsEntry.cpp)

    if(EXYOKIOFFICE_BUILD_MCP)
        exyokioffice_add_fuzz_target(mcprpc Entry/McpRpcEntry.cpp)
    endif()
endif()
