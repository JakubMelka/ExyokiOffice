// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "McpTestSupport.hpp"

#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/Wordprocessing.hpp"
#include "ExyokiOffice/Tools/ValidationRunner.hpp"
#include "ExyokiOffice/Word/WordDocument.hpp"

#include <doctest/doctest.h>

#include <filesystem>
#include <string>

using namespace ExyokiOfficeTests;
using namespace ExyokiOffice::Mcp;

TEST_CASE("paragraphs are inserted at every anchor position [mcp-word]")
{
    auto server = MakeWordServer();
    server->Initialize();

    const auto created = server->Call("create_document", nlohmann::json::object());
    const auto documentId = created["data"]["documentId"].get<std::string>();

    const auto atEnd = server->Call("insert_paragraph", nlohmann::json{{"documentId", documentId},
                                                                       {"anchor", nlohmann::json{{"position", "end"}}},
                                                                       {"text", "Second"}});
    REQUIRE(atEnd["ok"] == true);
    CHECK(atEnd["data"]["block"] == 1);

    const auto atStart = server->Call("insert_paragraph",
                                      nlohmann::json{{"documentId", documentId},
                                                     {"anchor", nlohmann::json{{"position", "start"}}},
                                                     {"text", "First"}});
    REQUIRE(atStart["ok"] == true);
    CHECK(atStart["data"]["block"] == 1);

    const auto after = server->Call("insert_paragraph",
                                    nlohmann::json{{"documentId", documentId},
                                                   {"anchor", nlohmann::json{{"position", "after"}, {"block", 2}}},
                                                   {"text", "Third"}});
    REQUIRE(after["ok"] == true);
    CHECK(after["data"]["block"] == 3);

    const auto blocks = server->Call("read_blocks", nlohmann::json{{"documentId", documentId}});
    REQUIRE(blocks["data"]["blocks"].size() == 3);
    CHECK(blocks["data"]["blocks"][0]["text"] == "First");
    CHECK(blocks["data"]["blocks"][1]["text"] == "Second");
    CHECK(blocks["data"]["blocks"][2]["text"] == "Third");
}

TEST_CASE("an anchor that names no block is rejected [mcp-word]")
{
    auto server = MakeWordServer();
    server->Initialize();

    const auto created = server->Call("create_document", nlohmann::json::object());
    const auto documentId = created["data"]["documentId"].get<std::string>();

    const auto missingBlock =
        server->Call("insert_paragraph", nlohmann::json{{"documentId", documentId},
                                                        {"anchor", nlohmann::json{{"position", "after"}}},
                                                        {"text", "Nowhere"}});
    CHECK(missingBlock["ok"] == false);
    CHECK(missingBlock["error"]["code"] == "anchor_invalid");

    const auto outOfRange =
        server->Call("insert_paragraph", nlohmann::json{{"documentId", documentId},
                                                        {"anchor", nlohmann::json{{"position", "after"}, {"block", 9}}},
                                                        {"text", "Nowhere"}});
    CHECK(outOfRange["ok"] == false);
    CHECK(outOfRange["error"]["code"] == "block_not_found");
}

TEST_CASE("formatted inline runs survive the round trip [mcp-word]")
{
    auto server = MakeWordServer();
    server->Initialize();

    const auto created = server->Call("create_document", nlohmann::json{{"path", "inlines.docx"}});
    const auto documentId = created["data"]["documentId"].get<std::string>();

    const auto inserted = server->Call(
        "insert_paragraph",
        nlohmann::json{{"documentId", documentId},
                       {"anchor", nlohmann::json{{"position", "end"}}},
                       {"inlines", nlohmann::json::array({nlohmann::json{{"text", "Bold "}, {"bold", true}},
                                                          nlohmann::json{{"text", "and red"}, {"color", "#FF0000"}},
                                                          nlohmann::json{{"break", "line"}},
                                                          nlohmann::json{{"text", "after a break"}}})}});
    REQUIRE(inserted["ok"] == true);

    static_cast<void>(server->Call("save_document", nlohmann::json{{"documentId", documentId}}));

    auto editor = ExyokiOffice::Word::WordDocumentEditor::Open(server->Path("inlines.docx"));
    REQUIRE(editor != nullptr);
    const auto paragraphs = editor->Paragraphs();
    REQUIRE_FALSE(paragraphs.empty());

    const auto runs = paragraphs.front()->Runs();
    REQUIRE(runs.size() >= 3);
    CHECK(runs[0]->GetBold().value_or(false));
    CHECK(runs[1]->GetColor() == "FF0000");
    CHECK(paragraphs.front()->PlainText().find("after a break") != std::string::npos);
}

TEST_CASE("a paragraph is rewritten with formatting and survives the round trip [mcp-word]")
{
    auto server = MakeWordServer();
    server->Initialize();

    const auto created = server->Call("create_document", nlohmann::json{{"path", "edited.docx"}});
    const auto documentId = created["data"]["documentId"].get<std::string>();
    REQUIRE(server->Call("insert_paragraph", nlohmann::json{{"documentId", documentId},
                                                            {"anchor", {{"position", "end"}}},
                                                            {"text", "Original"}})["ok"] == true);

    const auto edited = server->Call(
        "edit_paragraph",
        nlohmann::json{{"documentId", documentId},
                       {"block", 1},
                       {"alignment", "center"},
                       {"inlines", nlohmann::json::array({nlohmann::json{{"text", "Rewritten "}, {"bold", true}},
                                                          nlohmann::json{{"text", "content"},
                                                                         {"color", "#336699"},
                                                                         {"italic", true}}})}});
    REQUIRE(edited["ok"] == true);
    CHECK(edited["revision"] == 2);

    const auto blocks = server->Call("read_blocks", nlohmann::json{{"documentId", documentId}});
    REQUIRE(blocks["ok"] == true);
    REQUIRE(blocks["data"]["blocks"].size() == 1);
    CHECK(blocks["data"]["blocks"][0]["text"] == "Rewritten content");

    const auto notParagraph = server->Call("edit_paragraph", nlohmann::json{{"documentId", documentId},
                                                                            {"block", 99},
                                                                            {"text", "Missing"}});
    CHECK(notParagraph["ok"] == false);
    CHECK(notParagraph["error"]["code"] == "block_not_found");

    REQUIRE(server->Call("save_document", nlohmann::json{{"documentId", documentId}})["ok"] == true);
    auto editor = ExyokiOffice::Word::WordDocumentEditor::Open(server->Path("edited.docx"));
    REQUIRE(editor != nullptr);
    REQUIRE(editor->Paragraphs().size() == 1);
    CHECK(editor->Paragraphs()[0]->PlainText() == "Rewritten content");
    const auto report = ExyokiOffice::Tools::Run(server->Path("edited.docx"));
    CHECK(report.Loaded);
    CHECK(report.ErrorCount == 0);
}

TEST_CASE("headings appear in the outline [mcp-word]")
{
    auto server = MakeWordServer();
    server->Initialize();

    const auto created = server->Call("create_document", nlohmann::json::object());
    const auto documentId = created["data"]["documentId"].get<std::string>();

    static_cast<void>(server->Call("insert_paragraph", nlohmann::json{{"documentId", documentId},
                                                                      {"anchor", nlohmann::json{{"position", "end"}}},
                                                                      {"text", "Introduction"},
                                                                      {"heading_level", 1}}));
    static_cast<void>(server->Call("insert_paragraph", nlohmann::json{{"documentId", documentId},
                                                                      {"anchor", nlohmann::json{{"position", "end"}}},
                                                                      {"text", "Body text"}}));

    const auto outline = server->Call("get_outline", nlohmann::json{{"documentId", documentId}});
    REQUIRE(outline["ok"] == true);
    REQUIRE(outline["data"]["headings"].size() == 1);
    CHECK(outline["data"]["headings"][0]["text"] == "Introduction");
    CHECK(outline["data"]["headings"][0]["level"] == 1);
    CHECK(outline["data"]["headings"][0]["block"] == 1);
}

TEST_CASE("an unknown style is reported as style_not_found [mcp-word]")
{
    auto server = MakeWordServer();
    server->Initialize();

    const auto created = server->Call("create_document", nlohmann::json::object());
    const auto documentId = created["data"]["documentId"].get<std::string>();
    static_cast<void>(server->Call("insert_paragraph", nlohmann::json{{"documentId", documentId},
                                                                      {"anchor", nlohmann::json{{"position", "end"}}},
                                                                      {"text", "Body"}}));

    const auto refused = server->Call("apply_style", nlohmann::json{{"documentId", documentId},
                                                                    {"blocks", nlohmann::json::array({1})},
                                                                    {"style_id", "NoSuchStyle"}});
    CHECK(refused["ok"] == false);
    CHECK(refused["error"]["code"] == "style_not_found");
    CHECK(refused["error"]["hint"] == "Call list_styles to see the available style identifiers.");

    // A document created from scratch carries no style catalog of its own, so
    // the listing is empty here; the tool still has to answer successfully.
    const auto styles = server->Call("list_styles", nlohmann::json{{"documentId", documentId}});
    CHECK(styles["ok"] == true);
    CHECK(styles["data"]["styles"].is_array());
}

TEST_CASE("lists are inserted as consecutive numbered paragraphs [mcp-word]")
{
    auto server = MakeWordServer();
    server->Initialize();

    const auto created = server->Call("create_document", nlohmann::json::object());
    const auto documentId = created["data"]["documentId"].get<std::string>();

    const auto list = server->Call(
        "insert_list", nlohmann::json{{"documentId", documentId},
                                      {"anchor", nlohmann::json{{"position", "end"}}},
                                      {"kind", "numbered"},
                                      {"items", nlohmann::json::array({nlohmann::json{{"text", "First"}},
                                                                       nlohmann::json{{"text", "Second"}},
                                                                       nlohmann::json{{"text", "Third"}}})}});
    REQUIRE(list["ok"] == true);
    CHECK(list["data"]["firstBlock"] == 1);
    CHECK(list["data"]["lastBlock"] == 3);
    CHECK(list["data"]["numberingId"].get<int>() > 0);

    const auto blocks = server->Call("read_blocks", nlohmann::json{{"documentId", documentId}});
    REQUIRE(blocks["data"]["blocks"].size() == 3);
    CHECK(blocks["data"]["blocks"][0]["text"] == "First");
    CHECK(blocks["data"]["blocks"][2]["text"] == "Third");
}

TEST_CASE("tables are inserted, filled, and restructured [mcp-word]")
{
    auto server = MakeWordServer();
    server->Initialize();

    const auto created = server->Call("create_document", nlohmann::json{{"path", "tables.docx"}});
    const auto documentId = created["data"]["documentId"].get<std::string>();

    const auto table = server->Call(
        "insert_table", nlohmann::json{{"documentId", documentId},
                                       {"anchor", nlohmann::json{{"position", "end"}}},
                                       {"rows", 2},
                                       {"cols", 2},
                                       {"header_row", true},
                                       {"data", nlohmann::json::array({nlohmann::json::array({"Region", "Revenue"}),
                                                                       nlohmann::json::array({"North", "1200"})})}});
    REQUIRE(table["ok"] == true);
    const auto block = table["data"]["block"].get<int>();

    const auto edited = server->Call("edit_table_cell", nlohmann::json{{"documentId", documentId},
                                                                       {"block", block},
                                                                       {"row", 2},
                                                                       {"col", 2},
                                                                       {"text", "1500"}});
    REQUIRE(edited["ok"] == true);

    const auto grown = server->Call("modify_table", nlohmann::json{{"documentId", documentId},
                                                                   {"block", block},
                                                                   {"operation", "add_row"},
                                                                   {"count", 2}});
    REQUIRE(grown["ok"] == true);
    CHECK(grown["data"]["rows"] == 4);

    const auto outOfRange = server->Call("edit_table_cell", nlohmann::json{{"documentId", documentId},
                                                                           {"block", block},
                                                                           {"row", 99},
                                                                           {"col", 1},
                                                                           {"text", "x"}});
    CHECK(outOfRange["ok"] == false);
    CHECK(outOfRange["error"]["code"] == "anchor_invalid");

    const auto notATable = server->Call("modify_table", nlohmann::json{{"documentId", documentId},
                                                                       {"block", 99},
                                                                       {"operation", "add_row"}});
    CHECK(notATable["ok"] == false);
    CHECK(notATable["error"]["code"] == "block_not_found");

    static_cast<void>(server->Call("save_document", nlohmann::json{{"documentId", documentId}}));

    auto editor = ExyokiOffice::Word::WordDocumentEditor::Open(server->Path("tables.docx"));
    REQUIRE(editor != nullptr);
    const auto tables = editor->Tables();
    REQUIRE(tables.size() == 1);
    CHECK(tables.front()->GetRowCount() == 4);

    const auto report = ExyokiOffice::Tools::Run(server->Path("tables.docx"));
    CHECK(report.ErrorCount == 0);
}

TEST_CASE("blocks are deleted and the remaining indices shift [mcp-word]")
{
    auto server = MakeWordServer();
    server->Initialize();

    const auto created = server->Call("create_document", nlohmann::json::object());
    const auto documentId = created["data"]["documentId"].get<std::string>();

    for (const auto* text : {"One", "Two", "Three", "Four"})
    {
        static_cast<void>(server->Call("insert_paragraph",
                                       nlohmann::json{{"documentId", documentId},
                                                      {"anchor", nlohmann::json{{"position", "end"}}},
                                                      {"text", text}}));
    }

    const auto deleted =
        server->Call("delete_blocks", nlohmann::json{{"documentId", documentId}, {"from", 2}, {"to", 3}});
    REQUIRE(deleted["ok"] == true);
    CHECK(deleted["data"]["deleted"] == 2);
    CHECK(deleted["data"]["blockCount"] == 2);

    const auto blocks = server->Call("read_blocks", nlohmann::json{{"documentId", documentId}});
    CHECK(blocks["data"]["blocks"][0]["text"] == "One");
    CHECK(blocks["data"]["blocks"][1]["text"] == "Four");
}

TEST_CASE("headers, footers, and the page setup are written [mcp-word]")
{
    auto server = MakeWordServer();
    server->Initialize();

    const auto created = server->Call("create_document", nlohmann::json{{"path", "layout.docx"}});
    const auto documentId = created["data"]["documentId"].get<std::string>();
    static_cast<void>(server->Call("insert_paragraph", nlohmann::json{{"documentId", documentId},
                                                                      {"anchor", nlohmann::json{{"position", "end"}}},
                                                                      {"text", "Body"}}));

    const auto footer = server->Call(
        "set_header_footer",
        nlohmann::json{{"documentId", documentId}, {"target", "footer"}, {"text", "Confidential"}});
    REQUIRE(footer["ok"] == true);

    const auto section = server->Call("set_section", nlohmann::json{{"documentId", documentId},
                                                                    {"page_size", nlohmann::json{{"preset", "A4"}}},
                                                                    {"orientation", "landscape"}});
    REQUIRE(section["ok"] == true);
    CHECK(section["data"]["orientation"] == "landscape");
    CHECK(section["data"]["widthPt"].get<double>() > section["data"]["heightPt"].get<double>());

    static_cast<void>(server->Call("save_document", nlohmann::json{{"documentId", documentId}}));
    const auto report = ExyokiOffice::Tools::Run(server->Path("layout.docx"));
    CHECK(report.ErrorCount == 0);
}

TEST_CASE("comments and notes are added and removed [mcp-word]")
{
    auto server = MakeWordServer();
    server->Initialize();

    const auto created = server->Call("create_document", nlohmann::json::object());
    const auto documentId = created["data"]["documentId"].get<std::string>();
    static_cast<void>(server->Call("insert_paragraph", nlohmann::json{{"documentId", documentId},
                                                                      {"anchor", nlohmann::json{{"position", "end"}}},
                                                                      {"text", "Reviewed text"}}));

    const auto comment = server->Call("add_comment", nlohmann::json{{"documentId", documentId},
                                                                    {"block", 1},
                                                                    {"text", "Please verify"},
                                                                    {"author", "Reviewer"}});
    REQUIRE(comment["ok"] == true);
    const auto commentId = comment["data"]["commentId"].get<int>();

    const auto listed = server->Call("list_comments", nlohmann::json{{"documentId", documentId}});
    REQUIRE(listed["data"]["comments"].size() == 1);
    CHECK(listed["data"]["comments"][0]["author"] == "Reviewer");

    const auto note = server->Call("add_note", nlohmann::json{{"documentId", documentId},
                                                              {"block", 1},
                                                              {"kind", "footnote"},
                                                              {"text", "Source: internal"}});
    CHECK(note["ok"] == true);

    const auto removed =
        server->Call("delete_comment", nlohmann::json{{"documentId", documentId}, {"comment_id", commentId}});
    CHECK(removed["ok"] == true);

    const auto missing =
        server->Call("delete_comment", nlohmann::json{{"documentId", documentId}, {"comment_id", 999}});
    CHECK(missing["ok"] == false);
    CHECK(missing["error"]["code"] == "comment_not_found");
}

TEST_CASE("revision tracking is toggled and revisions resolved [mcp-word]")
{
    auto server = MakeWordServer();
    server->Initialize();

    const auto created = server->Call("create_document", nlohmann::json::object());
    const auto documentId = created["data"]["documentId"].get<std::string>();

    const auto enabled =
        server->Call("set_tracked_changes", nlohmann::json{{"documentId", documentId}, {"enabled", true}});
    REQUIRE(enabled["ok"] == true);
    CHECK(enabled["data"]["enabled"] == true);

    const auto withAuthor = server->Call(
        "set_tracked_changes",
        nlohmann::json{{"documentId", documentId}, {"enabled", true}, {"author", "Reviewer"}});
    REQUIRE(withAuthor["ok"] == true);
    CHECK_FALSE(withAuthor["warnings"].empty());

    const auto disabled =
        server->Call("set_tracked_changes", nlohmann::json{{"documentId", documentId}, {"enabled", false}});
    CHECK(disabled["data"]["enabled"] == false);

    const auto revisions = server->Call("list_revisions", nlohmann::json{{"documentId", documentId}});
    REQUIRE(revisions["ok"] == true);
    CHECK(revisions["data"]["revisions"].empty());

    const auto resolved =
        server->Call("resolve_revisions", nlohmann::json{{"documentId", documentId}, {"mode", "accept"}});
    CHECK(resolved["ok"] == true);
    CHECK(resolved["data"]["resolved"] == 0);
}

TEST_CASE("a bookmark can be added and read back from the outline [mcp-word]")
{
    auto server = MakeWordServer();
    server->Initialize();

    const auto created = server->Call("create_document", nlohmann::json::object());
    const auto documentId = created["data"]["documentId"].get<std::string>();
    static_cast<void>(server->Call("insert_paragraph", nlohmann::json{{"documentId", documentId},
                                                                      {"anchor", nlohmann::json{{"position", "end"}}},
                                                                      {"text", "Summary"}}));

    const auto bookmark =
        server->Call("add_bookmark", nlohmann::json{{"documentId", documentId}, {"name", "summary"}, {"block", 1}});
    REQUIRE(bookmark["ok"] == true);
    CHECK(bookmark["data"]["name"] == "summary");

    const auto outline = server->Call("get_outline", nlohmann::json{{"documentId", documentId}});
    REQUIRE(outline["data"]["bookmarks"].size() == 1);
    CHECK(outline["data"]["bookmarks"][0]["name"] == "summary");
}

TEST_CASE("a mail-merge template is filled from JSON data [mcp-word]")
{
    auto server = MakeWordServer();
    server->Initialize();

    const auto created = server->Call("create_document", nlohmann::json::object());
    const auto documentId = created["data"]["documentId"].get<std::string>();
    static_cast<void>(server->Call("insert_paragraph", nlohmann::json{{"documentId", documentId},
                                                                      {"anchor", nlohmann::json{{"position", "end"}}},
                                                                      {"text", "Dear "}}));
    static_cast<void>(server->Call("add_bookmark",
                                   nlohmann::json{{"documentId", documentId}, {"name", "Customer"}, {"block", 1}}));

    const auto filled = server->Call(
        "fill_template",
        nlohmann::json{{"documentId", documentId}, {"data", nlohmann::json{{"Customer", "Acme Corp."}}}});
    REQUIRE(filled["ok"] == true);
    CHECK(filled["data"]["bookmarksMerged"] == 1);

    const auto empty = server->Call(
        "fill_template",
        nlohmann::json{{"documentId", documentId}, {"data", nlohmann::json{{"Missing", "value"}}}});
    CHECK(empty["ok"] == false);
    CHECK(empty["error"]["code"] == "template_field_missing");
}

TEST_CASE("an unparsable heading style leaves the outline usable [mcp-word]")
{
    auto server = MakeWordServer();
    server->Initialize();

    const auto created = server->Call("create_document", nlohmann::json{{"path", "outline.docx"}});
    const auto documentId = created["data"]["documentId"].get<std::string>();
    static_cast<void>(server->Call("insert_paragraph", nlohmann::json{{"documentId", documentId},
                                                                      {"anchor", nlohmann::json{{"position", "end"}}},
                                                                      {"text", "Real heading"},
                                                                      {"heading_level", 1}}));
    static_cast<void>(server->Call("insert_paragraph", nlohmann::json{{"documentId", documentId},
                                                                      {"anchor", nlohmann::json{{"position", "end"}}},
                                                                      {"text", "Odd style"}}));
    REQUIRE(server->Call("save_document", nlohmann::json{{"documentId", documentId}})["ok"] == true);
    static_cast<void>(server->Call("close_document", nlohmann::json{{"documentId", documentId}}));

    // A style identifier is arbitrary document data; this one overflows every
    // integer type and used to abort get_outline with an internal error.
    {
        auto editor = ExyokiOffice::Word::WordDocumentEditor::Open(server->Path("outline.docx"));
        REQUIRE(editor != nullptr);
        REQUIRE(editor->Paragraphs().size() == 2);
        editor->Paragraphs()[1]->SetStyleId("Heading99999999999999");
        REQUIRE(editor->SaveToFile(server->Path("outline.docx")));
    }

    const auto outline = server->Call("get_outline", nlohmann::json{{"path", "outline.docx"}});
    REQUIRE(outline["ok"] == true);
    REQUIRE(outline["data"]["headings"].size() == 1);
    CHECK(outline["data"]["headings"][0]["text"] == "Real heading");
}

TEST_CASE("the rendered window matches the reported block indices [mcp-word]")
{
    auto server = MakeWordServer();
    server->Initialize();

    // A block-level content control expands to several document-model blocks,
    // so the model window and the body window drift apart unless the tool maps
    // one onto the other.
    {
        auto editor = ExyokiOffice::Word::WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);
        REQUIRE(editor->AddParagraph("One") != nullptr);
        auto control = editor->Body().InsertContentControl("wrapped", "Wrapped");
        REQUIRE(control != nullptr);
        REQUIRE(control->AddParagraph("Two") != nullptr);
        REQUIRE(control->AddParagraph("Three") != nullptr);
        REQUIRE(editor->AddParagraph("Four") != nullptr);
        REQUIRE(editor->SaveToFile(server->Path("controls.docx")));
    }

    const auto model = server->Call("read_blocks", nlohmann::json{{"path", "controls.docx"}});
    REQUIRE(model["ok"] == true);

    int lastBlock = 0;
    for (const auto& block : model["data"]["blocks"])
    {
        if (block["kind"] == "paragraph" && block["text"] == "Four")
        {
            lastBlock = block["block"].get<int>();
        }
    }

    REQUIRE(lastBlock > 0);

    const auto rendered = server->Call(
        "read_blocks",
        nlohmann::json{{"path", "controls.docx"}, {"from", lastBlock}, {"count", 1}, {"format", "text"}});
    REQUIRE(rendered["ok"] == true);
    CHECK(rendered["warnings"].empty());

    const auto text = rendered["data"]["text"].get<std::string>();
    CHECK(text.find("Four") != std::string::npos);
    CHECK(text.find("Three") == std::string::npos);
}

TEST_CASE("an out-of-range section is refused instead of clamped [mcp-word]")
{
    auto server = MakeWordServer();
    server->Initialize();

    const auto created = server->Call("create_document", nlohmann::json::object());
    const auto documentId = created["data"]["documentId"].get<std::string>();
    static_cast<void>(server->Call("insert_paragraph", nlohmann::json{{"documentId", documentId},
                                                                      {"anchor", nlohmann::json{{"position", "end"}}},
                                                                      {"text", "Body"}}));

    const auto missing = server->Call("set_section", nlohmann::json{{"documentId", documentId},
                                                                    {"section", 9},
                                                                    {"orientation", "landscape"}});
    CHECK(missing["ok"] == false);
    CHECK(missing["error"]["code"] == "anchor_invalid");

    const auto missingRunning = server->Call(
        "set_header_footer",
        nlohmann::json{{"documentId", documentId}, {"target", "footer"}, {"section", 9}, {"text", "x"}});
    CHECK(missingRunning["ok"] == false);
    CHECK(missingRunning["error"]["code"] == "anchor_invalid");

    const auto lastSection =
        server->Call("set_section", nlohmann::json{{"documentId", documentId}, {"orientation", "landscape"}});
    CHECK(lastSection["ok"] == true);
}

TEST_CASE("table deletions stop at the last row and merges are validated [mcp-word]")
{
    auto server = MakeWordServer();
    server->Initialize();

    const auto created = server->Call("create_document", nlohmann::json{{"path", "structure.docx"}});
    const auto documentId = created["data"]["documentId"].get<std::string>();

    const auto table = server->Call("insert_table", nlohmann::json{{"documentId", documentId},
                                                                   {"anchor", nlohmann::json{{"position", "end"}}},
                                                                   {"rows", 3},
                                                                   {"cols", 3}});
    REQUIRE(table["ok"] == true);
    const auto block = table["data"]["block"].get<int>();

    const auto tooWide = server->Call("modify_table",
                                      nlohmann::json{{"documentId", documentId},
                                                     {"block", block},
                                                     {"operation", "merge_cells"},
                                                     {"range", nlohmann::json{{"row", 2},
                                                                              {"col", 2},
                                                                              {"rowSpan", 5},
                                                                              {"colSpan", 5}}}});
    CHECK(tooWide["ok"] == false);
    CHECK(tooWide["error"]["code"] == "range_invalid");

    const auto outsideTable = server->Call("modify_table",
                                           nlohmann::json{{"documentId", documentId},
                                                          {"block", block},
                                                          {"operation", "merge_cells"},
                                                          {"range", nlohmann::json{{"row", 9},
                                                                                   {"col", 1},
                                                                                   {"rowSpan", 1},
                                                                                   {"colSpan", 1}}}});
    CHECK(outsideTable["ok"] == false);
    CHECK(outsideTable["error"]["code"] == "anchor_invalid");

    const auto merged = server->Call("modify_table",
                                     nlohmann::json{{"documentId", documentId},
                                                    {"block", block},
                                                    {"operation", "merge_cells"},
                                                    {"range", nlohmann::json{{"row", 1},
                                                                             {"col", 1},
                                                                             {"rowSpan", 1},
                                                                             {"colSpan", 2}}}});
    REQUIRE(merged["ok"] == true);

    const auto trimmed = server->Call("modify_table", nlohmann::json{{"documentId", documentId},
                                                                     {"block", block},
                                                                     {"operation", "delete_row"},
                                                                     {"at", 2},
                                                                     {"count", 10}});
    REQUIRE(trimmed["ok"] == true);
    CHECK(trimmed["data"]["removed"] == 2);
    CHECK(trimmed["data"]["rows"] == 1);
    CHECK_FALSE(trimmed["warnings"].empty());

    REQUIRE(server->Call("save_document", nlohmann::json{{"documentId", documentId}})["ok"] == true);

    auto editor = ExyokiOffice::Word::WordDocumentEditor::Open(server->Path("structure.docx"));
    REQUIRE(editor != nullptr);
    REQUIRE(editor->Tables().size() == 1);
    CHECK(editor->Tables().front()->GetRowCount() == 1);

    const auto report = ExyokiOffice::Tools::Run(server->Path("structure.docx"));
    CHECK(report.Loaded);
    CHECK(report.ErrorCount == 0);
}

TEST_CASE("a paragraph only joins a numbering instance that exists [mcp-word]")
{
    auto server = MakeWordServer();
    server->Initialize();

    const auto created = server->Call("create_document", nlohmann::json{{"path", "numbering.docx"}});
    const auto documentId = created["data"]["documentId"].get<std::string>();

    const auto list = server->Call("insert_list",
                                   nlohmann::json{{"documentId", documentId},
                                                  {"anchor", nlohmann::json{{"position", "end"}}},
                                                  {"kind", "numbered"},
                                                  {"items", nlohmann::json::array({nlohmann::json{{"text", "One"}}})}});
    REQUIRE(list["ok"] == true);
    const auto numberingId = list["data"]["numberingId"].get<int>();

    const auto joined = server->Call("insert_paragraph",
                                     nlohmann::json{{"documentId", documentId},
                                                    {"anchor", nlohmann::json{{"position", "end"}}},
                                                    {"text", "Two"},
                                                    {"list", nlohmann::json{{"numberingId", numberingId}}}});
    REQUIRE(joined["ok"] == true);

    const auto dangling = server->Call("insert_paragraph",
                                       nlohmann::json{{"documentId", documentId},
                                                      {"anchor", nlohmann::json{{"position", "end"}}},
                                                      {"text", "Nowhere"},
                                                      {"list", nlohmann::json{{"numberingId", 9999}}}});
    CHECK(dangling["ok"] == false);
    CHECK(dangling["error"]["code"] == "input_invalid");

    REQUIRE(server->Call("save_document", nlohmann::json{{"documentId", documentId}})["ok"] == true);

    auto editor = ExyokiOffice::Word::WordDocumentEditor::Open(server->Path("numbering.docx"));
    REQUIRE(editor != nullptr);
    REQUIRE(editor->Paragraphs().size() == 2);
    ExyokiOffice::Word::ParagraphNumbering numbering;
    REQUIRE(editor->Paragraphs()[1]->TryGetNumbering(numbering));
    REQUIRE(numbering.NumberingId.has_value());
    CHECK(*numbering.NumberingId == numberingId);

    const auto report = ExyokiOffice::Tools::Run(server->Path("numbering.docx"));
    CHECK(report.ErrorCount == 0);
}

TEST_CASE("one image dimension keeps the natural aspect ratio [mcp-word]")
{
    auto server = MakeWordServer();
    server->Initialize();

    const auto created = server->Call("create_document", nlohmann::json{{"path", "picture.docx"}});
    const auto documentId = created["data"]["documentId"].get<std::string>();

    // A 1x1 PNG: the natural ratio is square, so the height must match the width.
    const std::string png =
        "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAEhQGAhKmMIQAAAABJRU5ErkJggg==";

    const auto inserted = server->Call("insert_image", nlohmann::json{{"documentId", documentId},
                                                                      {"anchor", nlohmann::json{{"position", "end"}}},
                                                                      {"dataBase64", png},
                                                                      {"width", "4cm"}});
    REQUIRE(inserted["ok"] == true);

    const auto badLength = server->Call("insert_image", nlohmann::json{{"documentId", documentId},
                                                                       {"anchor", nlohmann::json{{"position", "end"}}},
                                                                       {"dataBase64", png},
                                                                       {"width", "four furlongs"}});
    CHECK(badLength["ok"] == false);
    CHECK(badLength["error"]["code"] == "input_invalid");

    REQUIRE(server->Call("save_document", nlohmann::json{{"documentId", documentId}})["ok"] == true);

    auto editor = ExyokiOffice::Word::WordDocumentEditor::Open(server->Path("picture.docx"));
    REQUIRE(editor != nullptr);

    bool sawImage = false;
    for (const auto& paragraph : editor->Paragraphs())
    {
        for (const auto& image : paragraph->Images())
        {
            ExyokiOffice::Word::ImageSize size;
            if (image == nullptr || !image->TryGetSize(size))
            {
                continue;
            }

            sawImage = true;
            CHECK(size.Width.ToEmu().GetValue() ==
                  doctest::Approx(size.Height.ToEmu().GetValue()).epsilon(0.01));
        }
    }

    CHECK(sawImage);

    const auto report = ExyokiOffice::Tools::Run(server->Path("picture.docx"));
    CHECK(report.ErrorCount == 0);
}

TEST_CASE("a hyperlink carries formatted runs [mcp-word]")
{
    auto server = MakeWordServer();
    server->Initialize();

    const auto created = server->Call("create_document", nlohmann::json{{"path", "links.docx"}});
    const auto documentId = created["data"]["documentId"].get<std::string>();

    const auto inserted = server->Call(
        "insert_paragraph",
        nlohmann::json{{"documentId", documentId},
                       {"anchor", nlohmann::json{{"position", "end"}}},
                       {"inlines", nlohmann::json::array({nlohmann::json{
                                       {"link", nlohmann::json{{"target", "https://example.com/docs"},
                                                               {"runs", nlohmann::json::array(
                                                                            {nlohmann::json{{"text", "Exyoki"}, {"bold", true}},
                                                                             nlohmann::json{{"text", " manual"}}})}}}}})}});
    REQUIRE(inserted["ok"] == true);

    const auto targetless = server->Call(
        "insert_paragraph",
        nlohmann::json{{"documentId", documentId},
                       {"anchor", nlohmann::json{{"position", "end"}}},
                       {"inlines", nlohmann::json::array({nlohmann::json{{"link", nlohmann::json::object()}}})}});
    CHECK(targetless["ok"] == false);
    CHECK(targetless["error"]["code"] == "input_invalid");

    REQUIRE(server->Call("save_document", nlohmann::json{{"documentId", documentId}})["ok"] == true);

    auto editor = ExyokiOffice::Word::WordDocumentEditor::Open(server->Path("links.docx"));
    REQUIRE(editor != nullptr);
    REQUIRE_FALSE(editor->Paragraphs().empty());

    const auto hyperlinks = editor->Paragraphs().front()->Hyperlinks();
    REQUIRE(hyperlinks.size() == 1);
    const auto runs = hyperlinks.front()->Runs();
    REQUIRE(runs.size() == 2);
    CHECK(runs[0]->GetBold().value_or(false));
    CHECK_FALSE(runs[1]->GetBold().value_or(false));
    CHECK(hyperlinks.front()->PlainText() == "Exyoki manual");

    const auto report = ExyokiOffice::Tools::Run(server->Path("links.docx"));
    CHECK(report.ErrorCount == 0);
}

TEST_CASE("a comment spans a block range and carries threaded replies [mcp-word]")
{
    auto server = MakeWordServer();
    server->Initialize();

    const auto created = server->Call("create_document", nlohmann::json{{"path", "ranges.docx"}});
    const auto documentId = created["data"]["documentId"].get<std::string>();
    for (const auto* text : {"First paragraph", "Second paragraph"})
    {
        static_cast<void>(server->Call("insert_paragraph",
                                       nlohmann::json{{"documentId", documentId},
                                                      {"anchor", nlohmann::json{{"position", "end"}}},
                                                      {"text", text}}));
    }

    const auto spanning = server->Call("add_comment", nlohmann::json{{"documentId", documentId},
                                                                     {"block", 1},
                                                                     {"end_block", 2},
                                                                     {"text", "Covers both"},
                                                                     {"author", "Reviewer"}});
    REQUIRE(spanning["ok"] == true);
    CHECK(spanning["data"]["endBlock"] == 2);

    // A reply shares the range its parent already marks, so it addresses a
    // comment instead of a block; passing both is ambiguous.
    const auto ambiguous = server->Call("add_comment", nlohmann::json{{"documentId", documentId},
                                                                      {"block", 1},
                                                                      {"text", "A reply"},
                                                                      {"parent_id", 0}});
    CHECK(ambiguous["ok"] == false);
    CHECK(ambiguous["error"]["code"] == "input_invalid");

    const auto parentId = spanning["data"]["commentId"].get<int>();
    const auto reply = server->Call(
        "add_comment",
        nlohmann::json{{"documentId", documentId}, {"parent_id", parentId}, {"text", "A reply"}});
    REQUIRE(reply["ok"] == true);
    CHECK(reply["data"]["parentId"] == parentId);
    CHECK(reply["data"]["commentId"] != parentId);

    const auto threaded = server->Call("list_comments", nlohmann::json{{"documentId", documentId}});
    REQUIRE(threaded["ok"] == true);
    bool sawReply = false;
    bool sawRoot = false;
    for (const auto& entry : threaded["data"]["comments"])
    {
        if (entry.contains("parentId"))
        {
            sawReply = true;
            CHECK(entry["parentId"] == parentId);
            CHECK(entry["text"] == "A reply");
            CHECK(entry["resolved"] == false);
        }
        else
        {
            // Identifiers start at 0, so a thread root omits the member
            // instead of reporting a sentinel that is also a valid id.
            sawRoot = true;
        }
    }
    CHECK(sawReply);
    CHECK(sawRoot);

    const auto orphan = server->Call(
        "add_comment", nlohmann::json{{"documentId", documentId}, {"parent_id", 9999}, {"text", "Nowhere"}});
    CHECK(orphan["ok"] == false);
    CHECK(orphan["error"]["code"] == "comment_not_found");

    const auto reversed = server->Call("add_comment", nlohmann::json{{"documentId", documentId},
                                                                     {"block", 2},
                                                                     {"end_block", 1},
                                                                     {"text", "Backwards"}});
    CHECK(reversed["ok"] == false);
    CHECK(reversed["error"]["code"] == "anchor_invalid");

    REQUIRE(server->Call("save_document", nlohmann::json{{"documentId", documentId}})["ok"] == true);

    const auto listed = server->Call("list_comments", nlohmann::json{{"path", "ranges.docx"}});
    REQUIRE(listed["ok"] == true);

    // The thread root and its reply are both comments in their own right, and
    // the reply covers the same blocks because it shares the parent's range.
    REQUIRE(listed["data"]["comments"].size() == 2);
    for (const auto& entry : listed["data"]["comments"])
    {
        CHECK(entry["blocks"] == nlohmann::json::array({1, 2}));
        CHECK_FALSE(entry["date"].get<std::string>().empty());
    }

    auto editor = ExyokiOffice::Word::WordDocumentEditor::Open(server->Path("ranges.docx"));
    REQUIRE(editor != nullptr);
    REQUIRE(editor->Comments().size() == 2);

    auto root = editor->Comments().front();
    REQUIRE(root != nullptr);
    CHECK(root->PlainText() == "Covers both");
    REQUIRE(root->Replies().size() == 1);
    CHECK(root->Replies().front()->PlainText() == "A reply");

    const auto report = ExyokiOffice::Tools::Run(server->Path("ranges.docx"));
    CHECK(report.ErrorCount == 0);
}

TEST_CASE("a bookmark spans a block range [mcp-word]")
{
    auto server = MakeWordServer();
    server->Initialize();

    const auto created = server->Call("create_document", nlohmann::json{{"path", "bookmarks.docx"}});
    const auto documentId = created["data"]["documentId"].get<std::string>();
    for (const auto* text : {"Opening", "Closing"})
    {
        static_cast<void>(server->Call("insert_paragraph",
                                       nlohmann::json{{"documentId", documentId},
                                                      {"anchor", nlohmann::json{{"position", "end"}}},
                                                      {"text", text}}));
    }

    const auto spanning = server->Call(
        "add_bookmark",
        nlohmann::json{{"documentId", documentId}, {"name", "section"}, {"block", 1}, {"end_block", 2}});
    REQUIRE(spanning["ok"] == true);
    CHECK(spanning["data"]["endBlock"] == 2);

    const auto reversed = server->Call(
        "add_bookmark",
        nlohmann::json{{"documentId", documentId}, {"name", "backwards"}, {"block", 2}, {"end_block", 1}});
    CHECK(reversed["ok"] == false);
    CHECK(reversed["error"]["code"] == "anchor_invalid");

    REQUIRE(server->Call("save_document", nlohmann::json{{"documentId", documentId}})["ok"] == true);

    auto editor = ExyokiOffice::Word::WordDocumentEditor::Open(server->Path("bookmarks.docx"));
    REQUIRE(editor != nullptr);
    REQUIRE(editor->Paragraphs().size() == 2);

    auto bookmark = editor->FindBookmark("section");
    REQUIRE(bookmark != nullptr);
    auto end = bookmark->GetEndElement();
    REQUIRE(end != nullptr);
    auto owner = end->Parent();
    REQUIRE(owner != nullptr);
    CHECK(owner->IsSameNode(*editor->Paragraphs()[1]->GetLowLevelApi()));

    const auto report = ExyokiOffice::Tools::Run(server->Path("bookmarks.docx"));
    CHECK(report.ErrorCount == 0);
}

TEST_CASE("a footer carries a page-number field [mcp-word]")
{
    auto server = MakeWordServer();
    server->Initialize();

    const auto created = server->Call("create_document", nlohmann::json{{"path", "running.docx"}});
    const auto documentId = created["data"]["documentId"].get<std::string>();
    static_cast<void>(server->Call("insert_paragraph", nlohmann::json{{"documentId", documentId},
                                                                      {"anchor", nlohmann::json{{"position", "end"}}},
                                                                      {"text", "Body"}}));

    const auto footer = server->Call(
        "set_header_footer",
        nlohmann::json{{"documentId", documentId},
                       {"target", "footer"},
                       {"inlines", nlohmann::json::array({nlohmann::json{{"text", "Page "}},
                                                          nlohmann::json{{"field", nlohmann::json{{"code", "PAGE"},
                                                                                                  {"text", "1"}}}}})}});
    REQUIRE(footer["ok"] == true);
    CHECK(footer["data"]["paragraphs"] == 1);

    const auto withPicture = server->Call(
        "set_header_footer",
        nlohmann::json{{"documentId", documentId},
                       {"target", "header"},
                       {"inlines", nlohmann::json::array({nlohmann::json{
                                       {"image", nlohmann::json{{"dataBase64", "iVBORw0KGgo="}}}}})}});
    CHECK(withPicture["ok"] == false);
    CHECK(withPicture["error"]["code"] == "unsupported");

    const auto multiple = server->Call(
        "set_header_footer",
        nlohmann::json{{"documentId", documentId},
                       {"target", "header"},
                       {"blocks", nlohmann::json::array({nlohmann::json{{"text", "Top line"}},
                                                         nlohmann::json{{"text", "Second line"}}})}});
    REQUIRE(multiple["ok"] == true);
    CHECK(multiple["data"]["paragraphs"] == 2);

    REQUIRE(server->Call("save_document", nlohmann::json{{"documentId", documentId}})["ok"] == true);

    auto editor = ExyokiOffice::Word::WordDocumentEditor::Open(server->Path("running.docx"));
    REQUIRE(editor != nullptr);
    REQUIRE_FALSE(editor->Sections().empty());

    auto content = editor->Sections().back()->GetFooter();
    REQUIRE(content != nullptr);
    REQUIRE(content->Paragraphs().size() == 1);
    const auto fields = content->Paragraphs().front()->Fields();
    REQUIRE(fields.size() == 1);
    CHECK(fields.front()->GetInstruction().find("PAGE") != std::string::npos);

    auto header = editor->Sections().back()->GetHeader();
    REQUIRE(header != nullptr);
    CHECK(header->Paragraphs().size() == 2);

    const auto report = ExyokiOffice::Tools::Run(server->Path("running.docx"));
    CHECK(report.ErrorCount == 0);
}

TEST_CASE("a template is filled from a path into a new file [mcp-word]")
{
    auto server = MakeWordServer();
    server->Initialize();

    const auto created = server->Call("create_document", nlohmann::json{{"path", "template.docx"}});
    const auto documentId = created["data"]["documentId"].get<std::string>();
    static_cast<void>(server->Call("insert_paragraph", nlohmann::json{{"documentId", documentId},
                                                                      {"anchor", nlohmann::json{{"position", "end"}}},
                                                                      {"text", "Dear "}}));
    static_cast<void>(server->Call("add_bookmark",
                                   nlohmann::json{{"documentId", documentId}, {"name", "Customer"}, {"block", 1}}));
    REQUIRE(server->Call("save_document", nlohmann::json{{"documentId", documentId}})["ok"] == true);
    static_cast<void>(server->Call("close_document", nlohmann::json{{"documentId", documentId}}));

    const auto filled = server->Call("fill_template",
                                     nlohmann::json{{"input_path", "template.docx"},
                                                    {"output_path", "filled.docx"},
                                                    {"data", nlohmann::json{{"Customer", "Acme Corp."}}}});
    REQUIRE(filled["ok"] == true);
    CHECK(filled["data"]["bookmarksMerged"] == 1);
    CHECK(filled["data"]["outputPath"] == "filled.docx");
    REQUIRE(std::filesystem::exists(server->Path("filled.docx")));

    const auto both = server->Call("fill_template",
                                   nlohmann::json{{"documentId", "doc-1"},
                                                  {"input_path", "template.docx"},
                                                  {"data", nlohmann::json{{"Customer", "Acme Corp."}}}});
    CHECK(both["ok"] == false);
    CHECK(both["error"]["code"] == "input_invalid");

    const auto neither =
        server->Call("fill_template", nlohmann::json{{"data", nlohmann::json{{"Customer", "Acme Corp."}}}});
    CHECK(neither["ok"] == false);
    CHECK(neither["error"]["code"] == "input_invalid");

    const auto existing = server->Call("fill_template",
                                       nlohmann::json{{"input_path", "template.docx"},
                                                      {"output_path", "filled.docx"},
                                                      {"data", nlohmann::json{{"Customer", "Acme Corp."}}}});
    CHECK(existing["ok"] == false);
    CHECK(existing["error"]["code"] == "file_exists");

    auto editor = ExyokiOffice::Word::WordDocumentEditor::Open(server->Path("filled.docx"));
    REQUIRE(editor != nullptr);
    REQUIRE_FALSE(editor->Paragraphs().empty());
    CHECK(editor->Paragraphs().front()->PlainText().find("Acme Corp.") != std::string::npos);

    const auto report = ExyokiOffice::Tools::Run(server->Path("filled.docx"));
    CHECK(report.ErrorCount == 0);
}

TEST_CASE("two documents are compared into a tracked-revision result [mcp-word]")
{
    auto server = MakeWordServer();
    server->Initialize();

    for (const auto& [name, text] :
         std::vector<std::pair<std::string, std::string>>{{"v1.docx", "Original"}, {"v2.docx", "Revised"}})
    {
        const auto created = server->Call("create_document", nlohmann::json{{"path", name}});
        const auto documentId = created["data"]["documentId"].get<std::string>();
        static_cast<void>(server->Call("insert_paragraph",
                                       nlohmann::json{{"documentId", documentId},
                                                      {"anchor", nlohmann::json{{"position", "end"}}},
                                                      {"text", text}}));
        static_cast<void>(server->Call("save_document", nlohmann::json{{"documentId", documentId}}));
        static_cast<void>(server->Call("close_document", nlohmann::json{{"documentId", documentId}}));
    }

    const auto compared = server->Call("compare_documents", nlohmann::json{{"original_path", "v1.docx"},
                                                                           {"revised_path", "v2.docx"},
                                                                           {"output_path", "compared.docx"}});
    REQUIRE(compared["ok"] == true);
    CHECK(compared["data"]["identical"] == false);
    CHECK(std::filesystem::exists(server->Path("compared.docx")));
}
