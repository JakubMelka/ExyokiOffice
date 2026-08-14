# Copyright (c) 2026 Jakub Melka and Collaborators
# SPDX-License-Identifier: MIT
# See LICENSE file in the project root for full license text.

# The MCP servers are three thin binaries over one shared static library. The
# library implements the protocol, the session model, the workspace sandbox and
# every tool that does not depend on the document family; a binary only fills in
# its family adapter and toolset.
#
# json-schema-validator is compiled into the static library rather than reused
# from ExyokiOffice: the library links it privately and exports none of its
# symbols. It is already vendored under 3rdparty, so this adds no dependency.
add_library(exyoki-mcp-core STATIC
    core/JsonRpc.hpp
    core/JsonRpc.cpp
    core/StdioTransport.hpp
    core/StdioTransport.cpp
    core/McpServer.hpp
    core/McpServer.cpp
    core/ToolRegistry.hpp
    core/ToolRegistry.cpp
    core/ToolContext.hpp
    core/SessionStore.hpp
    core/SessionStore.cpp
    core/Workspace.hpp
    core/Workspace.cpp
    core/Results.hpp
    core/Results.cpp
    core/Units.hpp
    core/Units.cpp
    core/SchemaCheck.hpp
    core/SchemaCheck.cpp
    core/SharedToolset.hpp
    core/SharedToolset.cpp
    core/ServerRunner.hpp
    core/ServerRunner.cpp
    core/FamilyAdapter.hpp
    ${PROJECT_SOURCE_DIR}/3rdparty/json-schema-validator/json-patch.cpp
    ${PROJECT_SOURCE_DIR}/3rdparty/json-schema-validator/json-schema-draft7.json.cpp
    ${PROJECT_SOURCE_DIR}/3rdparty/json-schema-validator/json-uri.cpp
    ${PROJECT_SOURCE_DIR}/3rdparty/json-schema-validator/json-validator.cpp
    ${PROJECT_SOURCE_DIR}/3rdparty/json-schema-validator/smtp-address-validator.cpp
    ${PROJECT_SOURCE_DIR}/3rdparty/json-schema-validator/string-format-check.cpp)

target_link_libraries(exyoki-mcp-core PUBLIC ExyokiOffice)

target_include_directories(exyoki-mcp-core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/core)
# The library's own private headers, for the small building blocks a front end
# would otherwise copy - `AsciiText.hpp` above all. `exyoki` reaches for the
# same directory; document behavior still belongs in the library.
target_include_directories(exyoki-mcp-core PUBLIC ${PROJECT_SOURCE_DIR}/sources)
target_include_directories(exyoki-mcp-core SYSTEM PUBLIC
    ${PROJECT_SOURCE_DIR}/3rdparty
    ${PROJECT_SOURCE_DIR}/3rdparty/json-schema-validator
    ${PROJECT_SOURCE_DIR}/3rdparty/CLI11)

exyokioffice_enable_strict_warnings(exyoki-mcp-core)

# The mcprpc fuzz target drives this library, so it needs coverage feedback
# inside it - including the vendored schema validator, which is the busiest code
# on that path. A no-op unless EXYOKIOFFICE_BUILD_FUZZERS is on, and it sets
# target options, so the source-level warning exemption below is unaffected.
exyokioffice_enable_fuzzing_instrumentation(exyoki-mcp-core)

# The vendored validator is not held to the repository's warning policy; the
# sources are compiled here only because the shared library does not export them.
set_source_files_properties(
    ${PROJECT_SOURCE_DIR}/3rdparty/json-schema-validator/json-patch.cpp
    ${PROJECT_SOURCE_DIR}/3rdparty/json-schema-validator/json-schema-draft7.json.cpp
    ${PROJECT_SOURCE_DIR}/3rdparty/json-schema-validator/json-uri.cpp
    ${PROJECT_SOURCE_DIR}/3rdparty/json-schema-validator/json-validator.cpp
    ${PROJECT_SOURCE_DIR}/3rdparty/json-schema-validator/smtp-address-validator.cpp
    ${PROJECT_SOURCE_DIR}/3rdparty/json-schema-validator/string-format-check.cpp
    PROPERTIES COMPILE_OPTIONS "")

# Declares one family server: sources, Windows version resource, install rules,
# the loader-relative RPATH the relocatable install tree needs, and the DLL copy
# that lets the binary run straight from the build directory.
function(exyokioffice_add_mcp_server target)
    cmake_parse_arguments(arg "" "DESCRIPTION" "SOURCES" ${ARGN})

    if(WIN32)
        set(RC_FILE_DESCRIPTION "${arg_DESCRIPTION}")
        set(RC_ORIGINAL_FILENAME "${target}.exe")
        set(RC_INTERNAL_NAME "${target}")
        set(RC_FILE_TYPE "VFT_APP")
        configure_file(
            ${PROJECT_SOURCE_DIR}/cmake/VersionInfo.rc.in
            ${CMAKE_CURRENT_BINARY_DIR}/${target}.rc
            @ONLY)
        set(MCP_RC_FILE ${CMAKE_CURRENT_BINARY_DIR}/${target}.rc)
    endif()

    add_executable(${target} ${arg_SOURCES} ${MCP_RC_FILE})
    target_link_libraries(${target} PRIVATE exyoki-mcp-core)
    exyokioffice_enable_strict_warnings(${target})

    install(TARGETS ${target} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

    if(NOT WIN32)
        file(RELATIVE_PATH MCP_INSTALLED_LIBRARY_PATH
            ${CMAKE_INSTALL_FULL_BINDIR} ${CMAKE_INSTALL_FULL_LIBDIR})
        if(APPLE)
            set(MCP_LOADER_ORIGIN "@loader_path")
        else()
            set(MCP_LOADER_ORIGIN "$ORIGIN")
        endif()
        set_target_properties(${target} PROPERTIES
            INSTALL_RPATH "${MCP_LOADER_ORIGIN}/${MCP_INSTALLED_LIBRARY_PATH}")
    endif()

    exyokioffice_copy_runtime_library(${target})
endfunction()

# A fuzz build wants the core library for the mcprpc target, not the servers:
# they carry a main() the fuzzer replaces, and their post-build DLL copy means
# nothing in the static build a libFuzzer configuration produces.
if(NOT EXYOKIOFFICE_BUILD_FUZZERS)
    exyokioffice_add_mcp_server(exyoki-mcp-word
        DESCRIPTION "exyoki-mcp-word - ExyokiOffice Word MCP server"
        SOURCES
            word/main.cpp
            word/WordToolset.hpp
            word/WordToolset.cpp
            word/WordAddressing.hpp
            word/WordAddressing.cpp)

    exyokioffice_add_mcp_server(exyoki-mcp-excel
        DESCRIPTION "exyoki-mcp-excel - ExyokiOffice Excel MCP server"
        SOURCES
            excel/main.cpp
            excel/ExcelToolset.hpp
            excel/ExcelToolset.cpp
            excel/ExcelAddressing.hpp
            excel/ExcelAddressing.cpp)

    exyokioffice_add_mcp_server(exyoki-mcp-power-point
        DESCRIPTION "exyoki-mcp-power-point - ExyokiOffice PowerPoint MCP server"
        SOURCES
            powerpoint/main.cpp
            powerpoint/PowerPointToolset.hpp
            powerpoint/PowerPointToolset.cpp
            powerpoint/PptAddressing.hpp
            powerpoint/PptAddressing.cpp)
endif()

# The published tool catalogs under docs/schemas are the machine-readable
# description of these interfaces, exactly like `exyoki commands`. A catalog
# that silently stops matching the servers would mislead every client that
# reads it, and no compiler or review step catches that, so CTest does. Every
# entry here runs a server binary, so they exist only when one was built.
if(EXYOKIOFFICE_BUILD_UNIT_TESTS AND TARGET exyoki-mcp-word)
    foreach(MCP_SERVER word excel power-point)
        add_test(NAME Mcp.Catalog.${MCP_SERVER} COMMAND ${CMAKE_COMMAND}
            -DEXECUTABLE=$<TARGET_FILE:exyoki-mcp-${MCP_SERVER}>
            -DCATALOG=${PROJECT_SOURCE_DIR}/docs/schemas/mcp-${MCP_SERVER}-tools.json
            -P ${PROJECT_SOURCE_DIR}/cmake/CheckMcpCatalog.cmake)
        set_tests_properties(Mcp.Catalog.${MCP_SERVER} PROPERTIES LABELS "mcp")
    endforeach()

    # The replay conversations exercise the real stdio framing, which the
    # in-process protocol tests deliberately bypass.
    foreach(MCP_SERVER word excel power-point)
        add_test(NAME Mcp.Replay.${MCP_SERVER}
            COMMAND exyoki-mcp-${MCP_SERVER}
                    --workspace ${CMAKE_CURRENT_BINARY_DIR}/replay-${MCP_SERVER}
                    --replay ${PROJECT_SOURCE_DIR}/tests/mcp/replays/${MCP_SERVER}.jsonl)
        # Responses are written as compact JSON, one per line, so the expected
        # text carries no space after the colon.
        set_tests_properties(Mcp.Replay.${MCP_SERVER} PROPERTIES
            LABELS "mcp"
            PASS_REGULAR_EXPRESSION "\"errorCount\":0"
            FAIL_REGULAR_EXPRESSION "\"isError\":true")
    endforeach()

    file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/replay-word)
    file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/replay-excel)
    file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/replay-power-point)

    add_test(NAME Mcp.Options.CliOverridesEnvironment
        COMMAND exyoki-mcp-word --toolsets lifecycle --print-tools)
    set_tests_properties(Mcp.Options.CliOverridesEnvironment PROPERTIES
        LABELS "mcp"
        ENVIRONMENT "EXYOKI_MCP_TOOLSETS=content"
        PASS_REGULAR_EXPRESSION "create_document"
        FAIL_REGULAR_EXPRESSION "insert_paragraph")

    add_test(NAME Mcp.Options.InvalidLogLevel
        COMMAND exyoki-mcp-word --log-level invalid)
    set_tests_properties(Mcp.Options.InvalidLogLevel PROPERTIES
        LABELS "mcp"
        WILL_FAIL TRUE)

    add_test(NAME Mcp.Options.Version
        COMMAND exyoki-mcp-word --version)
    set_tests_properties(Mcp.Options.Version PROPERTIES
        LABELS "mcp"
        PASS_REGULAR_EXPRESSION "exyoki-mcp-word")
endif()

# The pytest suite under tests/mcp_python is the only test that observes these
# servers the way a real host does: over stdio, through the official MCP Python
# SDK, with nothing of ExyokiOffice linked in. It builds nothing, so it is a
# CTest entry rather than a test layer, and it is opt-in because it needs a
# Python environment the C++ build cannot produce.
if(EXYOKIOFFICE_BUILD_MCP_PYTHON_TESTS AND TARGET exyoki-mcp-word)
    set(MCP_PYTHON_SUITE ${PROJECT_SOURCE_DIR}/tests/mcp_python)

    if(NOT EXYOKIOFFICE_MCP_PYTHON)
        # The suite's README puts a virtual environment next to it, and that is
        # the interpreter carrying the SDK — preferred over whatever Python is on
        # PATH, which almost certainly does not have it. CI passes
        # -DEXYOKIOFFICE_MCP_PYTHON explicitly instead of relying on this.
        foreach(candidate
                ${MCP_PYTHON_SUITE}/.venv/Scripts/python.exe
                ${MCP_PYTHON_SUITE}/.venv/bin/python)
            if(EXISTS ${candidate})
                set(EXYOKIOFFICE_MCP_PYTHON ${candidate} CACHE FILEPATH
                    "Python interpreter that runs the tests/mcp_python suite")
                break()
            endif()
        endforeach()
    endif()

    if(NOT EXYOKIOFFICE_MCP_PYTHON)
        message(FATAL_ERROR
            "EXYOKIOFFICE_BUILD_MCP_PYTHON_TESTS is ON but no Python interpreter was found.\n"
            "Prepare one with:\n"
            "  python -m venv ${MCP_PYTHON_SUITE}/.venv\n"
            "  <that venv>/python -m pip install -e ${MCP_PYTHON_SUITE}\n"
            "or set EXYOKIOFFICE_MCP_PYTHON to an interpreter that already has the suite's "
            "dependencies.")
    endif()

    # -p no:cacheprovider: the suite sits in the source tree and a test run must
    # not write into it.
    add_test(NAME Mcp.Python
        COMMAND ${EXYOKIOFFICE_MCP_PYTHON} -m pytest -q -p no:cacheprovider
        WORKING_DIRECTORY ${MCP_PYTHON_SUITE})

    # The binaries are named outright rather than left to the suite's build-tree
    # search, so this entry tests the configuration that registered it and not
    # whichever build directory happens to be newer.
    set_tests_properties(Mcp.Python PROPERTIES
        LABELS "mcp;mcp-python"
        ENVIRONMENT
            "EXYOKI_MCP_WORD_EXE=$<TARGET_FILE:exyoki-mcp-word>;EXYOKI_MCP_EXCEL_EXE=$<TARGET_FILE:exyoki-mcp-excel>;EXYOKI_MCP_POWERPOINT_EXE=$<TARGET_FILE:exyoki-mcp-power-point>")
endif()
