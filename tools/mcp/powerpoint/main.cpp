// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "PowerPointToolset.hpp"
#include "ServerRunner.hpp"

#include <memory>

int main(int argc, char** argv)
{
    using namespace ExyokiOffice::Mcp;

    ServerDescriptor descriptor;
    descriptor.Name = "exyoki-mcp-power-point";
    descriptor.Title = "ExyokiOffice PowerPoint MCP Server";
    descriptor.Adapter = std::make_shared<PowerPointFamilyAdapter>();
    descriptor.AddressingLines = {
        "Slides are 1-based indices; list_slides reports them together with the layout and title of each slide.",
        "Shapes are addressed by their path in the shape tree: \"2\" is the second top-level shape, \"2/1\" the "
        "first child of a group; get_slide reports the path of every shape.",
        "Placeholders are addressed by type (\"title\", \"body\", \"subtitle\", ...) or by 1-based index; prefer "
        "set_placeholder_text over add_text_box so the layout's formatting applies.",
        "Lengths are points as a number or a string with a unit such as \"4cm\"; rotation is degrees, as a number "
        "such as 45 or a string such as \"45deg\"."};
    descriptor.Toolsets = {[](ToolRegistry& registry)
                           { RegisterPowerPointToolset(registry); }};

    return RunMcpServer(descriptor, argc, argv);
}
