# Copyright (c) 2026 Jakub Melka and Collaborators
# SPDX-License-Identifier: MIT
# See LICENSE file in the project root for full license text.

# The MCP layer: the JSON-RPC protocol, the tool catalog and its schemas, the
# session model, and one tool suite per family. The tests drive
# McpServer::HandleMessage in process, so a whole conversation runs without a
# child process or a pipe; the stdio framing itself is covered by the --replay
# CTest entries registered in tools/mcp.
exyokioffice_add_test_executable(ExyokiOfficeMcpTests
    LAYER mcp
    PREFIX Mcp
    SOURCES
        McpTestSupport.hpp
        McpTestSupport.cpp
        McpProtocolTests.cpp
        McpRegressionTests.cpp
        McpSchemaTests.cpp
        SharedToolTests.cpp
        SharedFamilyToolTests.cpp
        McpBoundaryTests.cpp
        WordToolTests.cpp
        ExcelToolTests.cpp
        PowerPointToolTests.cpp
    LINK exyoki-mcp-core)

target_sources(ExyokiOfficeMcpTests PRIVATE
    ${PROJECT_SOURCE_DIR}/tools/mcp/word/WordToolset.cpp
    ${PROJECT_SOURCE_DIR}/tools/mcp/word/WordAddressing.cpp
    ${PROJECT_SOURCE_DIR}/tools/mcp/excel/ExcelToolset.cpp
    ${PROJECT_SOURCE_DIR}/tools/mcp/excel/ExcelAddressing.cpp
    ${PROJECT_SOURCE_DIR}/tools/mcp/powerpoint/PowerPointToolset.cpp
    ${PROJECT_SOURCE_DIR}/tools/mcp/powerpoint/PptAddressing.cpp)

target_include_directories(ExyokiOfficeMcpTests PRIVATE
    ${PROJECT_SOURCE_DIR}/tools/mcp/word
    ${PROJECT_SOURCE_DIR}/tools/mcp/excel
    ${PROJECT_SOURCE_DIR}/tools/mcp/powerpoint)

exyokioffice_add_test_area(mcp NAME Mcp.Protocol LABELS mcp-protocol
    TAGS "[mcp-protocol]")
exyokioffice_add_test_area(mcp NAME Mcp.Shared LABELS mcp-shared
    TAGS "[mcp-lifecycle]")
exyokioffice_add_test_area(mcp NAME Mcp.Schemas LABELS mcp-schemas
    TAGS "[mcp-schema]")
exyokioffice_add_test_area(mcp NAME Mcp.Word LABELS mcp-word
    TAGS "[mcp-word]")
exyokioffice_add_test_area(mcp NAME Mcp.Excel LABELS mcp-excel
    TAGS "[mcp-excel]")
exyokioffice_add_test_area(mcp NAME Mcp.PowerPoint LABELS mcp-powerpoint
    TAGS "[mcp-powerpoint]")

exyokioffice_finalize_test_layer(mcp)
