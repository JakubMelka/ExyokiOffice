// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "ExcelToolset.hpp"
#include "ServerRunner.hpp"

#include <memory>

int main(int argc, char** argv)
{
    using namespace ExyokiOffice::Mcp;

    ServerDescriptor descriptor;
    descriptor.Name = "exyoki-mcp-excel";
    descriptor.Title = "ExyokiOffice Excel MCP Server";
    descriptor.Adapter = std::make_shared<ExcelFamilyAdapter>();
    descriptor.AddressingLines = {
        "Worksheets are addressed by name (case-insensitive) or by 1-based index; list_sheets reports both.",
        "Cells and ranges are always A1 notation, such as \"B2\" or \"A1:C10\"; column and row bands are \"B:D\" "
        "and \"2:5\".",
        "A cell value is a string, number, boolean, {\"formula\": \"SUM(A1:A9)\"}, or "
        "{\"value\": \"2026-08-02T00:00:00\", \"type\": \"datetime\"}; null skips a cell in write_range."};
    descriptor.Toolsets = {[](ToolRegistry& registry)
                           { RegisterExcelToolset(registry); }};

    return RunMcpServer(descriptor, argc, argv);
}
