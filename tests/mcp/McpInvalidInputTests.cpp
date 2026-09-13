// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

// Invalid and boundary input for the tools the rest of the suite calls only
// with arguments that work. Each refusal is checked for the error code an agent
// acts on, and every mutating refusal for leaving the document as it was.

#include "McpTestSupport.hpp"

#include <doctest/doctest.h>

#include <fstream>
#include <functional>
#include <memory>
#include <string>

using namespace ExyokiOfficeTests;
using namespace ExyokiOffice::Mcp;

namespace
{

/// Checks that @p result is a refusal carrying @p code.
void CheckRefused(const nlohmann::json& result, const std::string& code)
{
    INFO(result.dump());
    CHECK(result["ok"] == false);
    CHECK(result["error"]["code"] == code);
}

/// Creates a document of the server's family at @p path and returns its session identifier.
std::string CreateSaved(McpTestServer& server, const std::string& path)
{
    const auto created = server.Call("create_document", nlohmann::json{{"path", path}});
    REQUIRE(created["ok"] == true);
    const auto documentId = created["data"]["documentId"].get<std::string>();
    REQUIRE(server.Call("save_document", nlohmann::json{{"documentId", documentId}})["ok"] == true);
    return documentId;
}

std::unique_ptr<McpTestServer> MakeServer(const std::string& extension)
{
    if (extension == ".docx")
    {
        return MakeWordServer();
    }

    return extension == ".xlsx" ? MakeExcelServer() : MakePowerPointServer();
}

} // namespace

TEST_CASE("delete_blocks refuses blocks past the end and leaves the body intact [mcp-word]")
{
    auto server = MakeWordServer();
    server->Initialize();
    const auto documentId = server->Call("create_document", nlohmann::json::object())["data"]["documentId"];
    for (const auto* text : {"One", "Two", "Three"})
    {
        REQUIRE(server->Call("insert_paragraph", nlohmann::json{{"documentId", documentId},
                                                                {"anchor", nlohmann::json{{"position", "end"}}},
                                                                {"text", text}})["ok"] == true);
    }

    const auto blockCount = [&]()
    { return server->Call("read_blocks", nlohmann::json{{"documentId", documentId}})["data"]["blocks"].size(); };

    CheckRefused(server->Call("delete_blocks", nlohmann::json{{"documentId", documentId}, {"from", 99}}),
                 "block_not_found");
    CheckRefused(server->Call("delete_blocks", nlohmann::json{{"documentId", documentId}, {"from", 2}, {"to", 99}}),
                 "block_not_found");
    CheckRefused(server->Call("delete_blocks", nlohmann::json{{"documentId", documentId}, {"from", 0}}),
                 "input_invalid");
    const auto reversed =
        server->Call("delete_blocks", nlohmann::json{{"documentId", documentId}, {"from", 3}, {"to", 2}});
    CHECK(reversed["ok"] == false);
    CHECK(blockCount() == 3);

    // The last block is a boundary, not an error.
    const auto last = server->Call("delete_blocks", nlohmann::json{{"documentId", documentId}, {"from", 3}});
    REQUIRE(last["ok"] == true);
    CHECK(blockCount() == 2);
}

TEST_CASE("add_note and resolve_revisions refuse what does not exist [mcp-word]")
{
    auto server = MakeWordServer();
    server->Initialize();
    const auto documentId = server->Call("create_document", nlohmann::json::object())["data"]["documentId"];
    REQUIRE(server->Call("insert_paragraph", nlohmann::json{{"documentId", documentId},
                                                            {"anchor", nlohmann::json{{"position", "end"}}},
                                                            {"text", "Body"}})["ok"] == true);

    CheckRefused(server->Call("add_note", nlohmann::json{{"documentId", documentId}, {"block", 99}, {"text", "x"}}),
                 "block_not_found");
    CheckRefused(server->Call("add_note", nlohmann::json{{"documentId", documentId},
                                                         {"block", 1},
                                                         {"text", "x"},
                                                         {"kind", "sidenote"}}),
                 "input_invalid");

    CheckRefused(server->Call("resolve_revisions", nlohmann::json{{"documentId", documentId}, {"mode", "maybe"}}),
                 "input_invalid");

    // An identifier that names no revision resolves nothing, and says so.
    const auto unknown = server->Call(
        "resolve_revisions",
        nlohmann::json{{"documentId", documentId}, {"mode", "accept"}, {"ids", nlohmann::json::array({"999"})}});
    REQUIRE(unknown["ok"] == true);
    CHECK(unknown["data"]["resolved"] == 0);
    REQUIRE(unknown["warnings"].size() == 1);
    CHECK(unknown["warnings"][0]["code"] == "revision_not_found");
}

TEST_CASE("the reading tools of Word and PowerPoint want exactly one source [mcp-word]")
{
    auto word = MakeWordServer();
    word->Initialize();
    const auto wordDocument = CreateSaved(*word, "styles.docx");
    for (const std::string tool : {"list_styles", "list_revisions"})
    {
        CheckRefused(word->Call(tool, nlohmann::json::object()), "input_invalid");
        CheckRefused(word->Call(tool, nlohmann::json{{"documentId", wordDocument}, {"path", "styles.docx"}}),
                     "input_invalid");
        CheckRefused(word->Call(tool, nlohmann::json{{"path", "missing.docx"}}), "file_not_found");
    }

    auto powerPoint = MakePowerPointServer();
    powerPoint->Initialize();
    CheckRefused(powerPoint->Call("list_layouts", nlohmann::json::object()), "input_invalid");
    CheckRefused(powerPoint->Call("list_layouts", nlohmann::json{{"path", "missing.pptx"}}), "file_not_found");
}

TEST_CASE("compare_documents refuses missing inputs and destinations it must not write [mcp-word]")
{
    auto server = MakeWordServer();
    server->Initialize();
    static_cast<void>(CreateSaved(*server, "v1.docx"));

    const auto compare = [&](const std::string& original, const std::string& output)
    {
        return server->Call("compare_documents", nlohmann::json{{"original_path", original},
                                                                {"revised_path", "v1.docx"},
                                                                {"output_path", output}});
    };

    CheckRefused(compare("missing.docx", "out.docx"), "file_not_found");
    CheckRefused(compare("v1.docx", "v1.docx"), "file_exists");
    CheckRefused(compare("v1.docx", "../out.docx"), "path_outside_workspace");
    // A Word package under a name Excel is expected to open.
    CheckRefused(compare("v1.docx", "out.xlsx"), "family_mismatch");
    CHECK_FALSE(std::filesystem::exists(server->Path("out.xlsx")));

    const auto identical = compare("v1.docx", "same.docx");
    REQUIRE(identical["ok"] == true);
    CHECK(identical["data"]["identical"] == true);
}

TEST_CASE("modify_sheet_structure refuses intervals off the grid [mcp-excel]")
{
    auto server = MakeExcelServer();
    server->Initialize();
    const auto documentId = server->Call("create_document", nlohmann::json::object())["data"]["documentId"];

    const auto modify = [&](const std::string& operation, int at, nlohmann::json extra = nlohmann::json::object())
    {
        extra["documentId"] = documentId;
        extra["operation"] = operation;
        extra["at"] = at;
        return server->Call("modify_sheet_structure", extra);
    };

    const auto pastRows = modify("delete_rows", 1048577);
    CheckRefused(pastRows, "range_invalid");
    CHECK(pastRows["error"]["message"].get<std::string>().find("1048576") != std::string::npos);
    CheckRefused(modify("insert_columns", 16385), "range_invalid");
    CheckRefused(modify("insert_rows", 1, nlohmann::json{{"count", 0}}), "input_invalid");
    CheckRefused(modify("insert_rows", 1, nlohmann::json{{"count", 10001}}), "input_invalid");
    CheckRefused(modify("insert_rows", 1, nlohmann::json{{"sheet", "Missing"}}), "sheet_not_found");
    CheckRefused(modify("insert_cells", 1), "input_invalid");

    // The last column and the last row are boundaries, not errors.
    CHECK(modify("delete_columns", 16384)["ok"] == true);
    CHECK(modify("delete_rows", 1048576)["ok"] == true);
}

TEST_CASE("add_data_validation refuses incomplete and misaddressed rules [mcp-excel]")
{
    auto server = MakeExcelServer();
    server->Initialize();
    const auto documentId = server->Call("create_document", nlohmann::json::object())["data"]["documentId"];

    const auto validate = [&](const std::string& range, const nlohmann::json& rule,
                              nlohmann::json extra = nlohmann::json::object())
    {
        extra["documentId"] = documentId;
        extra["range"] = range;
        extra["rule"] = rule;
        return server->Call("add_data_validation", extra);
    };

    const nlohmann::json list{{"type", "list"}, {"values", nlohmann::json::array({"Yes", "No"})}};
    CheckRefused(validate("B2:", list), "range_invalid");
    CheckRefused(validate("B2", nlohmann::json{{"values", nlohmann::json::array({"a"})}}), "input_invalid");
    CheckRefused(validate("B2", nlohmann::json{{"type", "regex"}}), "input_invalid");
    CheckRefused(validate("B2", nlohmann::json{{"type", "list"}}), "input_invalid");
    CheckRefused(validate("B2", nlohmann::json{{"type", "list"}, {"values", nlohmann::json::array()}}),
                 "input_invalid");
    CheckRefused(validate("B2", "list"), "input_invalid");
    CheckRefused(validate("B2", list, nlohmann::json{{"sheet", "Missing"}}), "sheet_not_found");

    const auto whole = validate("C1:C9", nlohmann::json{{"type", "whole"},
                                                        {"operator", "between"},
                                                        {"formula1", "1"},
                                                        {"formula2", "5"}});
    CHECK(whole["ok"] == true);
}

TEST_CASE("slide addressing refuses slides and shapes past the end [mcp-powerpoint]")
{
    auto server = MakePowerPointServer();
    server->Initialize();
    const auto documentId = server->Call("create_document", nlohmann::json::object())["data"]["documentId"];
    for (int index = 0; index < 2; ++index)
    {
        REQUIRE(server->Call("add_slide", nlohmann::json{{"documentId", documentId}})["ok"] == true);
    }

    CheckRefused(server->Call("move_slide", nlohmann::json{{"documentId", documentId}, {"from", 9}, {"to", 1}}),
                 "slide_not_found");
    CheckRefused(server->Call("move_slide", nlohmann::json{{"documentId", documentId}, {"from", 1}, {"to", 9}}),
                 "slide_not_found");
    CHECK(server->Call("move_slide", nlohmann::json{{"documentId", documentId}, {"from", 2}, {"to", 2}})["ok"] ==
          true);

    CheckRefused(
        server->Call("set_slide_hidden", nlohmann::json{{"documentId", documentId}, {"slide", 9}, {"hidden", true}}),
        "slide_not_found");
    CheckRefused(server->Call("set_notes", nlohmann::json{{"documentId", documentId}, {"slide", 9}, {"text", "x"}}),
                 "slide_not_found");

    for (const std::string shape : {"99", "abc", "0"})
    {
        CheckRefused(
            server->Call("delete_shape", nlohmann::json{{"documentId", documentId}, {"slide", 1}, {"shape", shape}}),
            "shape_not_found");
    }
    CheckRefused(server->Call("delete_shape", nlohmann::json{{"documentId", documentId}, {"slide", 9}, {"shape", "1"}}),
                 "slide_not_found");

    CHECK(server->Call("list_slides", nlohmann::json{{"documentId", documentId}})["data"]["slides"].size() == 2);
}

TEST_CASE("set_transition wants exactly one of slide and all [mcp-powerpoint]")
{
    auto server = MakePowerPointServer();
    server->Initialize();
    const auto documentId = server->Call("create_document", nlohmann::json::object())["data"]["documentId"];
    REQUIRE(server->Call("add_slide", nlohmann::json{{"documentId", documentId}})["ok"] == true);

    const auto transition = [&](nlohmann::json arguments)
    {
        arguments["documentId"] = documentId;
        if (!arguments.contains("type"))
        {
            arguments["type"] = "fade";
        }
        return server->Call("set_transition", arguments);
    };

    // Neither used to report a slide 0; both used to apply to every slide.
    CheckRefused(transition(nlohmann::json::object()), "input_invalid");
    CheckRefused(transition(nlohmann::json{{"slide", 1}, {"all", true}}), "input_invalid");
    CheckRefused(transition(nlohmann::json{{"slide", 9}}), "slide_not_found");
    CheckRefused(transition(nlohmann::json{{"slide", 1}, {"duration_ms", -1}}), "input_invalid");
    CheckRefused(transition(nlohmann::json{{"slide", 1}, {"type", "morph"}}), "input_invalid");

    CHECK(transition(nlohmann::json{{"slide", 1}, {"all", false}})["ok"] == true);
}

TEST_CASE("set_slide_size refuses sizes PresentationML does not allow [mcp-powerpoint]")
{
    auto server = MakePowerPointServer();
    server->Initialize();
    const auto documentId = server->Call("create_document", nlohmann::json::object())["data"]["documentId"];

    const auto resize = [&](nlohmann::json arguments)
    {
        arguments["documentId"] = documentId;
        return server->Call("set_slide_size", arguments);
    };

    CheckRefused(resize(nlohmann::json::object()), "input_invalid");
    CheckRefused(resize(nlohmann::json{{"width", "20cm"}}), "input_invalid");
    // A preset with explicit dimensions used to take the preset and drop the rest.
    CheckRefused(resize(nlohmann::json{{"preset", "4:3"}, {"width", "20cm"}, {"height", "10cm"}}), "input_invalid");
    CheckRefused(resize(nlohmann::json{{"width", "wide"}, {"height", "10cm"}}), "input_invalid");
    // Out of range used to be reported as a failure to write.
    CheckRefused(resize(nlohmann::json{{"width", "-20cm"}, {"height", "10cm"}}), "input_invalid");
    CheckRefused(resize(nlohmann::json{{"width", "20cm"}, {"height", 0}}), "input_invalid");
    CheckRefused(resize(nlohmann::json{{"width", "143cm"}, {"height", "10cm"}}), "input_invalid");

    // One inch and 56 inches are the limits themselves.
    const auto smallest = resize(nlohmann::json{{"width", "1in"}, {"height", "1in"}});
    REQUIRE(smallest["ok"] == true);
    CHECK(smallest["data"]["widthPt"] == 72.0);
    CHECK(resize(nlohmann::json{{"width", "56in"}, {"height", "56in"}})["ok"] == true);
}

TEST_CASE("the file-to-file tools refuse missing, foreign and misnamed files in every family [mcp-lifecycle]")
{
    for (const std::string extension : {".docx", ".xlsx", ".pptx"})
    {
        INFO(extension);
        auto server = MakeServer(extension);
        server->Initialize();
        const auto documentId = CreateSaved(*server, "base" + extension);
        {
            std::ofstream junk(server->Path("junk" + extension), std::ios::binary);
            junk << "not a package";
        }
        const std::string foreign = extension == ".docx" ? "out.xlsx" : "out.docx";

        CheckRefused(server->Call("diff_documents",
                                  nlohmann::json{{"left_path", "missing" + extension}, {"right_path", "base" + extension}}),
                     "file_not_found");
        CheckRefused(server->Call("diff_documents",
                                  nlohmann::json{{"left_path", "junk" + extension}, {"right_path", "base" + extension}}),
                     "package_load_failed");
        CheckRefused(server->Call("diff_documents",
                                  nlohmann::json{{"left_path", "../x" + extension}, {"right_path", "base" + extension}}),
                     "path_outside_workspace");

        CheckRefused(server->Call("merge_documents",
                                  nlohmann::json{{"input_paths", nlohmann::json::array()}, {"output_path", "m" + extension}}),
                     "input_invalid");
        CheckRefused(server->Call("merge_documents",
                                  nlohmann::json{{"input_paths", nlohmann::json::array({"base" + extension, "missing" + extension})},
                                                 {"output_path", "m" + extension}}),
                     "file_not_found");
        CheckRefused(server->Call("merge_documents",
                                  nlohmann::json{{"input_paths", nlohmann::json::array({"base" + extension})},
                                                 {"output_path", "base" + extension}}),
                     "file_exists");
        CheckRefused(server->Call("merge_documents",
                                  nlohmann::json{{"input_paths", nlohmann::json::array({"base" + extension})},
                                                 {"output_path", foreign}}),
                     "family_mismatch");

        CheckRefused(server->Call("redact_document", nlohmann::json{{"output_path", "r" + extension}}), "input_invalid");
        CheckRefused(server->Call("redact_document", nlohmann::json{{"documentId", documentId},
                                                                    {"input_path", "base" + extension}}),
                     "input_invalid");
        CheckRefused(server->Call("redact_document",
                                  nlohmann::json{{"input_path", "missing" + extension}, {"output_path", "r" + extension}}),
                     "file_not_found");
        CheckRefused(server->Call("redact_document", nlohmann::json{{"documentId", "doc-99"}}), "document_not_found");

        // Redacting the open session is a mutation like any other. Its answer
        // carries the session fields, which the output schema must allow; the
        // test harness validates every answer against it.
        const auto session = server->Call("redact_document", nlohmann::json{{"documentId", documentId}});
        REQUIRE(session["ok"] == true);
        CHECK(session["documentId"] == documentId);
        CHECK(session["dirty"] == true);
    }
}
