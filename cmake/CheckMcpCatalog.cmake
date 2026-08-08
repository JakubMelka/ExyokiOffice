# Copyright (c) 2026 Jakub Melka and Collaborators
# SPDX-License-Identifier: MIT
# See LICENSE file in the project root for full license text.

# Checks that a published MCP tool catalog still matches its server.
#
# Run as:
#   cmake -DEXECUTABLE=<path to server> -DCATALOG=<docs/schemas/mcp-*.json> \
#         -P CheckMcpCatalog.cmake
#
# The catalogs under docs/schemas are the machine-readable description of what
# each MCP server offers, exactly like `exyoki commands` is for the CLI. A
# client that reads a stale catalog calls tools that no longer exist, or misses
# tools that do — and nothing about that shows up as a build failure, so it is
# checked here. Regenerate an intentionally changed catalog with:
#   <server> --print-tools > docs/schemas/mcp-<family>-tools.json

if(NOT EXECUTABLE)
    message(FATAL_ERROR "CheckMcpCatalog.cmake needs EXECUTABLE")
endif()

if(NOT CATALOG)
    message(FATAL_ERROR "CheckMcpCatalog.cmake needs CATALOG")
endif()

if(NOT EXISTS ${CATALOG})
    message(FATAL_ERROR
        "The published catalog ${CATALOG} does not exist.\n"
        "Create it with: ${EXECUTABLE} --print-tools > ${CATALOG}")
endif()

execute_process(
    COMMAND ${EXECUTABLE} --print-tools
    OUTPUT_VARIABLE liveCatalog
    ERROR_VARIABLE errors
    RESULT_VARIABLE status)

if(NOT status EQUAL 0)
    message(FATAL_ERROR "'${EXECUTABLE} --print-tools' failed: ${status}\n${errors}")
endif()

file(READ ${CATALOG} publishedCatalog)

# Both sides are compared with their trailing whitespace normalized, so a
# platform newline difference in the redirected file is not reported as drift.
string(STRIP "${liveCatalog}" liveCatalog)
string(STRIP "${publishedCatalog}" publishedCatalog)
string(REPLACE "\r\n" "\n" liveCatalog "${liveCatalog}")
string(REPLACE "\r\n" "\n" publishedCatalog "${publishedCatalog}")

if(NOT liveCatalog STREQUAL publishedCatalog)
    message(FATAL_ERROR
        "The tool catalog of ${EXECUTABLE} no longer matches ${CATALOG}.\n"
        "Regenerate it with: ${EXECUTABLE} --print-tools > ${CATALOG}")
endif()

string(REGEX MATCH "\"toolCount\": ([0-9]+)" toolCountMatch "${liveCatalog}")
message(STATUS "MCP catalog matches: ${CMAKE_MATCH_1} tool(s).")
