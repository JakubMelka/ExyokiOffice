// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "ServerRunner.hpp"
#include "WordToolset.hpp"

#include <memory>

int main(int argc, char** argv)
{
    using namespace ExyokiOffice::Mcp;

    ServerDescriptor descriptor;
    descriptor.Name = "exyoki-mcp-word";
    descriptor.Title = "ExyokiOffice Word MCP Server";
    descriptor.Adapter = std::make_shared<WordFamilyAdapter>();
    descriptor.AddressingLines = {
        "The document body is a flat list of blocks (paragraphs, tables, section breaks) numbered from 1; "
        "read_blocks and get_outline report those indices.",
        "Insertions take an anchor: {\"position\":\"end\"}, {\"position\":\"start\"}, or "
        "{\"position\":\"before\"|\"after\", \"block\": N}.",
        "Table cells are addressed as block, row, col with 1-based row and column indices over the logical grid."};
    descriptor.Toolsets = {[](ToolRegistry& registry)
                           { RegisterWordToolset(registry); }};

    return RunMcpServer(descriptor, argc, argv);
}
