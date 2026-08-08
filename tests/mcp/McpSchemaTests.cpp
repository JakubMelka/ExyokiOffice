// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "McpTestSupport.hpp"

#include "SchemaCheck.hpp"
#include "WordToolset.hpp"

#include <doctest/doctest.h>

#include <memory>
#include <set>
#include <string>
#include <vector>

using namespace ExyokiOfficeTests;
using namespace ExyokiOffice::Mcp;

/// Fixtures shared by the catalog-wide schema checks.
class McpSchemaFixture
{
public:
    /// Every server, so a schema defect in one family is not missed by the others.
    static std::vector<std::unique_ptr<McpTestServer>> AllServers()
    {
        std::vector<std::unique_ptr<McpTestServer>> servers;
        servers.push_back(MakeWordServer());
        servers.push_back(MakeExcelServer());
        servers.push_back(MakePowerPointServer());
        return servers;
    }
};

/// Shorthand used by every case below.
static std::vector<std::unique_ptr<McpTestServer>> AllServers()
{
    return McpSchemaFixture::AllServers();
}

TEST_CASE("every tool descriptor is complete [mcp-schema]")
{
    for (const auto& server : AllServers())
    {
        std::set<std::string> names;
        for (const auto& tool : server->Registry().Tools())
        {
            const auto& definition = tool.Definition;
            CAPTURE(definition.Name);

            CHECK_FALSE(definition.Name.empty());
            CHECK(definition.Name.size() <= 64);
            CHECK(definition.Name.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_") == std::string::npos);
            CHECK_FALSE(definition.Title.empty());
            CHECK_FALSE(definition.Description.empty());
            CHECK_FALSE(definition.Group.empty());
            CHECK(names.insert(definition.Name).second);

            REQUIRE(definition.InputSchema.is_object());
            CHECK(definition.InputSchema["type"] == "object");
            CHECK(definition.InputSchema["additionalProperties"] == false);
            REQUIRE(definition.OutputSchema.is_object());
            CHECK(definition.OutputSchema["type"] == "object");
        }
    }
}

TEST_CASE("every parameter carries a description [mcp-schema]")
{
    for (const auto& server : AllServers())
    {
        for (const auto& tool : server->Registry().Tools())
        {
            CAPTURE(tool.Definition.Name);
            const auto& properties = tool.Definition.InputSchema["properties"];
            REQUIRE(properties.is_object());
            for (const auto& [name, schema] : properties.items())
            {
                CAPTURE(name);
                REQUIRE(schema.is_object());
                CHECK(schema.contains("description"));
                CHECK_FALSE(schema["description"].get<std::string>().empty());
            }
        }
    }
}

TEST_CASE("no schema uses a $ref [mcp-schema]")
{
    for (const auto& server : AllServers())
    {
        for (const auto& tool : server->Registry().Tools())
        {
            CAPTURE(tool.Definition.Name);
            CHECK(tool.Definition.InputSchema.dump().find("\"$ref\"") == std::string::npos);
            CHECK(tool.Definition.OutputSchema.dump().find("\"$ref\"") == std::string::npos);
        }
    }
}

TEST_CASE("output schemas compile as JSON Schema [mcp-schema]")
{
    for (const auto& server : AllServers())
    {
        for (const auto& tool : server->Registry().Tools())
        {
            CAPTURE(tool.Definition.Name);
            SchemaCheck check;
            std::string error;
            CHECK_MESSAGE(check.SetSchema(tool.Definition.OutputSchema, error), error);
        }
    }
}

TEST_CASE("every documented example validates against its input schema [mcp-schema]")
{
    for (const auto& server : AllServers())
    {
        for (const auto& tool : server->Registry().Tools())
        {
            CAPTURE(tool.Definition.Name);
            REQUIRE(tool.Definition.Example.is_object());

            std::vector<SchemaViolation> violations;
            const bool valid = tool.InputCheck->Validate(tool.Definition.Example, violations);
            if (!valid && !violations.empty())
            {
                CAPTURE(violations.front().Pointer);
                CAPTURE(violations.front().Message);
            }

            CHECK(valid);
        }
    }
}

TEST_CASE("a tool without required parameters accepts an empty object [mcp-schema]")
{
    auto server = MakeWordServer();
    for (const auto& tool : server->Registry().Tools())
    {
        CAPTURE(tool.Definition.Name);
        const auto& required = tool.Definition.InputSchema["required"];
        std::vector<SchemaViolation> violations;
        const bool accepted = tool.InputCheck->Validate(nlohmann::json::object(), violations);
        CHECK(accepted == required.empty());
    }
}

TEST_CASE("a foreign argument is always rejected [mcp-schema]")
{
    auto server = MakeExcelServer();
    for (const auto& tool : server->Registry().Tools())
    {
        CAPTURE(tool.Definition.Name);
        auto arguments = tool.Definition.Example;
        arguments["definitely_not_a_parameter"] = 1;

        std::vector<SchemaViolation> violations;
        CHECK_FALSE(tool.InputCheck->Validate(arguments, violations));
    }
}

TEST_CASE("the shared toolset is offered by every family server [mcp-schema]")
{
    static const std::vector<std::string> shared{
        "create_document", "open_document", "save_document", "close_document", "list_documents",
        "undo", "get_document_info", "get_document_model", "get_document_markdown",
        "get_document_text", "search_text", "validate_document", "query_xml", "get_properties",
        "list_media", "get_media", "replace_text", "set_properties", "batch",
        "list_workspace", "convert_document", "diff_documents", "merge_documents", "split_document",
        "redact_document", "export_media"};

    for (const auto& server : AllServers())
    {
        for (const auto& name : shared)
        {
            CAPTURE(name);
            CHECK(server->Registry().Find(name) != nullptr);
        }
    }
}

TEST_CASE("the toolset filter narrows the catalog to the requested groups [mcp-schema]")
{
    McpTestServer server(std::make_shared<WordFamilyAdapter>(), {}, false, {"lifecycle"});
    for (const auto& tool : server.Registry().Tools())
    {
        CHECK(tool.Definition.Group == "lifecycle");
    }

    CHECK(server.Registry().Find("create_document") != nullptr);
    CHECK(server.Registry().Find("list_workspace") == nullptr);
}

TEST_CASE("every registered tool has an explicit behavioral test commitment [mcp-schema]")
{
    // This list is intentionally independent of registration and the published
    // catalogs. Adding a tool must update this commitment and add a real call
    // in SharedToolTests, SharedFamilyToolTests, or the family test file.
    const std::set<std::string> shared{
        "create_document", "open_document", "save_document", "close_document", "list_documents", "undo",
        "get_document_info", "get_document_model", "get_document_markdown", "get_document_text", "search_text",
        "validate_document", "query_xml", "get_properties", "list_media", "get_media", "replace_text",
        "set_properties", "batch", "list_workspace", "convert_document", "diff_documents", "merge_documents",
        "split_document", "redact_document", "export_media"};
    const std::set<std::string> word{
        "get_outline", "read_blocks", "list_styles", "list_revisions", "list_comments", "insert_paragraph",
        "insert_list", "edit_paragraph", "delete_blocks", "apply_style", "insert_image", "add_bookmark",
        "insert_table", "edit_table_cell", "modify_table", "set_header_footer", "set_section",
        "set_tracked_changes", "resolve_revisions", "add_comment", "delete_comment", "add_note", "fill_template",
        "compare_documents"};
    const std::set<std::string> excel{
        "list_sheets", "read_range", "add_sheet", "rename_sheet", "delete_sheet", "write_cells", "write_range",
        "clear_range", "modify_sheet_structure", "set_hyperlink", "recalculate", "merge_cells", "format_range",
        "set_column_width", "set_row_height", "freeze_panes", "add_table", "add_named_range",
        "add_data_validation", "add_conditional_formatting", "add_chart", "add_pivot_table"};
    const std::set<std::string> powerPoint{
        "list_slides", "get_slide", "list_layouts", "list_comments", "add_slide", "delete_slide", "move_slide",
        "duplicate_slide", "copy_slide_from", "set_slide_hidden", "set_placeholder_text", "add_text_box",
        "edit_text_frame", "delete_shape", "set_shape_transform", "add_image", "add_table", "edit_table_cell",
        "add_chart", "set_notes", "add_comment", "set_transition", "add_section", "set_slide_size"};

    const auto check = [&shared](const McpTestServer& server, const std::set<std::string>& family)
    {
        auto committed = shared;
        committed.insert(family.begin(), family.end());
        std::set<std::string> registered;
        for (const auto& tool : server.Registry().Tools())
        {
            registered.insert(tool.Definition.Name);
        }
        CHECK(registered == committed);
    };

    check(*MakeWordServer(), word);
    check(*MakeExcelServer(), excel);
    check(*MakePowerPointServer(), powerPoint);
}
