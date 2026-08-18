// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "McpTestSupport.hpp"

#include "WordToolset.hpp"

#include "ExyokiOffice/Packaging/GeneratedParts.hpp"
#include "ExyokiOffice/Tools/ValidationRunner.hpp"
#include "ExyokiOffice/Word/WordDocument.hpp"

#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>

using namespace ExyokiOfficeTests;
using namespace ExyokiOffice::Mcp;

TEST_CASE("a document is created, edited, saved, and reopened by the library [mcp-lifecycle]")
{
    auto server = MakeWordServer();
    server->Initialize();

    const auto created = server->Call("create_document", nlohmann::json{{"path", "report.docx"}});
    REQUIRE(created["ok"] == true);
    const auto documentId = created["data"]["documentId"].get<std::string>();
    CHECK(created["data"]["family"] == "word");
    CHECK(created["dirty"] == false);

    const auto inserted = server->Call("insert_paragraph",
                                       nlohmann::json{{"documentId", documentId},
                                                      {"anchor", nlohmann::json{{"position", "end"}}},
                                                      {"text", "Quarterly summary"}});
    REQUIRE(inserted["ok"] == true);
    CHECK(inserted["revision"] == 1);
    CHECK(inserted["dirty"] == true);

    const auto saved = server->Call("save_document", nlohmann::json{{"documentId", documentId}});
    REQUIRE(saved["ok"] == true);
    CHECK(saved["dirty"] == false);
    CHECK(saved["data"]["bytesWritten"].get<ExyokiOffice::UInt64>() > 0);

    // Verified through the library rather than through the server, so a bug in
    // the reading tools cannot make a broken write look correct.
    const auto path = server->Path("report.docx");
    REQUIRE(std::filesystem::exists(path));

    auto editor = ExyokiOffice::Word::WordDocumentEditor::Open(path);
    REQUIRE(editor != nullptr);
    const auto paragraphs = editor->Paragraphs();
    REQUIRE_FALSE(paragraphs.empty());
    CHECK(paragraphs.front()->PlainText() == "Quarterly summary");

    const auto report = ExyokiOffice::Tools::Run(path);
    CHECK(report.Loaded);
    CHECK_MESSAGE(report.ErrorCount == 0, DescribeValidationErrors(report));
}

TEST_CASE("closing a document with unsaved changes needs discard [mcp-lifecycle]")
{
    auto server = MakeWordServer();
    server->Initialize();

    const auto created = server->Call("create_document", nlohmann::json::object());
    const auto documentId = created["data"]["documentId"].get<std::string>();

    static_cast<void>(server->Call("insert_paragraph", nlohmann::json{{"documentId", documentId},
                                                                      {"anchor", nlohmann::json{{"position", "end"}}},
                                                                      {"text", "Draft"}}));

    const auto refused = server->Call("close_document", nlohmann::json{{"documentId", documentId}});
    CHECK(refused["ok"] == false);
    CHECK(refused["error"]["code"] == "operation_failed");

    const auto closed =
        server->Call("close_document", nlohmann::json{{"documentId", documentId}, {"discard", true}});
    CHECK(closed["ok"] == true);

    const auto listed = server->Call("list_documents", nlohmann::json::object());
    CHECK(listed["data"]["documents"].empty());
}

TEST_CASE("a path outside the workspace is rejected [mcp-lifecycle]")
{
    auto server = MakeWordServer();
    server->Initialize();

    const auto envelope = server->Call("create_document", nlohmann::json{{"path", "../escaped.docx"}});
    CHECK(envelope["ok"] == false);
    CHECK(envelope["error"]["code"] == "path_outside_workspace");
}

TEST_CASE("an unknown document id is reported as document_not_found [mcp-lifecycle]")
{
    auto server = MakeWordServer();
    server->Initialize();

    const auto envelope = server->Call("save_document", nlohmann::json{{"documentId", "doc-999"}});
    CHECK(envelope["ok"] == false);
    CHECK(envelope["error"]["code"] == "document_not_found");
    CHECK(envelope["error"]["hint"] == "Call list_documents to see the open sessions.");
}

TEST_CASE("a file changed on disk blocks an in-place save until forced [mcp-lifecycle]")
{
    auto server = MakeWordServer();
    server->Initialize();

    const auto created = server->Call("create_document", nlohmann::json{{"path", "conflict.docx"}});
    const auto documentId = created["data"]["documentId"].get<std::string>();
    static_cast<void>(server->Call("save_document", nlohmann::json{{"documentId", documentId}}));

    const auto path = server->Path("conflict.docx");
    const auto original = std::filesystem::last_write_time(path);
    std::filesystem::last_write_time(path, original + std::chrono::seconds(10));

    const auto blocked = server->Call("save_document", nlohmann::json{{"documentId", documentId}});
    CHECK(blocked["ok"] == false);
    CHECK(blocked["error"]["code"] == "file_changed_on_disk");

    const auto forced = server->Call("save_document", nlohmann::json{{"documentId", documentId}, {"force", true}});
    CHECK(forced["ok"] == true);
}

TEST_CASE("the document limit is enforced [mcp-lifecycle]")
{
    McpTestServer server(std::make_shared<WordFamilyAdapter>(), {});
    server.Initialize();

    // The default limit is 16 documents; the seventeenth must be refused.
    for (int index = 0; index < 16; ++index)
    {
        const auto envelope = server.Call("create_document", nlohmann::json::object());
        REQUIRE(envelope["ok"] == true);
    }

    const auto refused = server.Call("create_document", nlohmann::json::object());
    CHECK(refused["ok"] == false);
    CHECK(refused["error"]["code"] == "document_limit_reached");
}

TEST_CASE("undo restores the state before the last mutation [mcp-lifecycle]")
{
    auto server = MakeWordServer();
    server->Initialize();

    const auto created = server->Call("create_document", nlohmann::json::object());
    const auto documentId = created["data"]["documentId"].get<std::string>();

    const auto empty = server->Call("undo", nlohmann::json{{"documentId", documentId}});
    CHECK(empty["ok"] == false);
    CHECK(empty["error"]["code"] == "snapshot_unavailable");

    static_cast<void>(server->Call("insert_paragraph", nlohmann::json{{"documentId", documentId},
                                                                      {"anchor", nlohmann::json{{"position", "end"}}},
                                                                      {"text", "First"}}));
    static_cast<void>(server->Call("insert_paragraph", nlohmann::json{{"documentId", documentId},
                                                                      {"anchor", nlohmann::json{{"position", "end"}}},
                                                                      {"text", "Second"}}));

    const auto before = server->Call("read_blocks", nlohmann::json{{"documentId", documentId}});
    CHECK(before["data"]["blockCount"] == 2);

    const auto undone = server->Call("undo", nlohmann::json{{"documentId", documentId}});
    REQUIRE(undone["ok"] == true);
    CHECK(undone["data"]["restoredRevision"] == 1);

    const auto after = server->Call("read_blocks", nlohmann::json{{"documentId", documentId}});
    CHECK(after["data"]["blockCount"] == 1);
}

TEST_CASE("reading tools accept a path and reject both identifiers at once [mcp-lifecycle]")
{
    auto server = MakeWordServer();
    server->Initialize();

    const auto created = server->Call("create_document", nlohmann::json{{"path", "text.docx"}});
    const auto documentId = created["data"]["documentId"].get<std::string>();
    static_cast<void>(server->Call("insert_paragraph", nlohmann::json{{"documentId", documentId},
                                                                      {"anchor", nlohmann::json{{"position", "end"}}},
                                                                      {"text", "Readable"}}));
    static_cast<void>(server->Call("save_document", nlohmann::json{{"documentId", documentId}}));

    const auto byPath = server->Call("get_document_text", nlohmann::json{{"path", "text.docx"}});
    REQUIRE(byPath["ok"] == true);
    CHECK(byPath["data"]["text"].get<std::string>().find("Readable") != std::string::npos);

    const auto both =
        server->Call("get_document_text", nlohmann::json{{"documentId", documentId}, {"path", "text.docx"}});
    CHECK(both["ok"] == false);
    CHECK(both["error"]["code"] == "input_invalid");

    const auto neither = server->Call("get_document_text", nlohmann::json::object());
    CHECK(neither["ok"] == false);
}

TEST_CASE("search and replace work over the session document [mcp-lifecycle]")
{
    auto server = MakeWordServer();
    server->Initialize();

    const auto created = server->Call("create_document", nlohmann::json::object());
    const auto documentId = created["data"]["documentId"].get<std::string>();
    static_cast<void>(server->Call("insert_paragraph", nlohmann::json{{"documentId", documentId},
                                                                      {"anchor", nlohmann::json{{"position", "end"}}},
                                                                      {"text", "Results for 2025"}}));

    const auto found = server->Call("search_text", nlohmann::json{{"documentId", documentId}, {"needle", "2025"}});
    REQUIRE(found["ok"] == true);
    CHECK(found["data"]["matchCount"] == 1);

    const auto dryRun = server->Call(
        "replace_text",
        nlohmann::json{{"documentId", documentId}, {"needle", "2025"}, {"replacement", "2026"}, {"dry_run", true}});
    REQUIRE(dryRun["ok"] == true);
    CHECK(dryRun["data"]["replacementCount"] == 1);
    CHECK(dryRun["revision"] == 1);

    const auto applied = server->Call(
        "replace_text", nlohmann::json{{"documentId", documentId}, {"needle", "2025"}, {"replacement", "2026"}});
    REQUIRE(applied["ok"] == true);
    CHECK(applied["revision"] == 2);

    const auto text = server->Call("get_document_text", nlohmann::json{{"documentId", documentId}});
    CHECK(text["data"]["text"].get<std::string>().find("2026") != std::string::npos);
}

TEST_CASE("properties round-trip through the session [mcp-lifecycle]")
{
    auto server = MakeWordServer();
    server->Initialize();

    const auto created = server->Call("create_document", nlohmann::json::object());
    const auto documentId = created["data"]["documentId"].get<std::string>();

    const auto written = server->Call("set_properties", nlohmann::json{{"documentId", documentId},
                                                                       {"title", "Quarterly report"},
                                                                       {"creator", "ExyokiOffice"}});
    REQUIRE(written["ok"] == true);
    CHECK(written["data"]["written"].size() == 2);

    const auto read = server->Call("get_properties", nlohmann::json{{"documentId", documentId}});
    CHECK(read["data"]["core"]["title"] == "Quarterly report");
    CHECK(read["data"]["core"]["creator"] == "ExyokiOffice");
}

TEST_CASE("validation and the model reader agree the document is sound [mcp-lifecycle]")
{
    auto server = MakeWordServer();
    server->Initialize();

    const auto created = server->Call("create_document", nlohmann::json::object());
    const auto documentId = created["data"]["documentId"].get<std::string>();
    static_cast<void>(server->Call("insert_paragraph", nlohmann::json{{"documentId", documentId},
                                                                      {"anchor", nlohmann::json{{"position", "end"}}},
                                                                      {"text", "Body"}}));

    const auto validated = server->Call("validate_document", nlohmann::json{{"documentId", documentId}});
    REQUIRE(validated["ok"] == true);
    CHECK(validated["data"]["errorCount"] == 0);

    const auto model = server->Call("get_document_model", nlohmann::json{{"documentId", documentId}});
    REQUIRE(model["ok"] == true);
    CHECK(model["data"]["document"]["format"] == "exyokioffice-document");

    const auto markdown = server->Call("get_document_markdown", nlohmann::json{{"documentId", documentId}});
    CHECK(markdown["data"]["markdown"].get<std::string>().find("Body") != std::string::npos);
}

TEST_CASE("query_xml reaches the markup the typed tools do not expose [mcp-lifecycle]")
{
    auto server = MakeWordServer();
    server->Initialize();

    const auto created = server->Call("create_document", nlohmann::json::object());
    const auto documentId = created["data"]["documentId"].get<std::string>();
    static_cast<void>(server->Call("insert_paragraph", nlohmann::json{{"documentId", documentId},
                                                                      {"anchor", nlohmann::json{{"position", "end"}}},
                                                                      {"text", "Queried"}}));

    const auto matched = server->Call("query_xml", nlohmann::json{{"documentId", documentId}, {"xpath", "//w:p"}});
    REQUIRE(matched["ok"] == true);
    CHECK_FALSE(matched["data"]["matches"].empty());

    const auto broken = server->Call("query_xml", nlohmann::json{{"documentId", documentId}, {"xpath", "//["}});
    CHECK(broken["ok"] == false);
}

TEST_CASE("the workspace listing and conversion work file to file [mcp-lifecycle]")
{
    auto server = MakeWordServer();
    server->Initialize();

    const auto created = server->Call("create_document", nlohmann::json{{"path", "source.docx"}});
    const auto documentId = created["data"]["documentId"].get<std::string>();
    static_cast<void>(server->Call("insert_paragraph", nlohmann::json{{"documentId", documentId},
                                                                      {"anchor", nlohmann::json{{"position", "end"}}},
                                                                      {"text", "Converted"}}));
    static_cast<void>(server->Call("save_document", nlohmann::json{{"documentId", documentId}}));

    const auto listed = server->Call("list_workspace", nlohmann::json::object());
    REQUIRE(listed["ok"] == true);
    REQUIRE(listed["data"]["files"].size() == 1);
    CHECK(listed["data"]["files"][0]["path"] == "source.docx");

    const auto converted = server->Call(
        "convert_document", nlohmann::json{{"input_path", "source.docx"}, {"output_path", "source.md"}});
    REQUIRE(converted["ok"] == true);
    CHECK(std::filesystem::exists(server->Path("source.md")));

    const auto diffed = server->Call(
        "diff_documents", nlohmann::json{{"left_path", "source.docx"}, {"right_path", "source.docx"}});
    REQUIRE(diffed["ok"] == true);
    CHECK(diffed["data"]["identical"] == true);
}

TEST_CASE("an existing destination is protected unless overwrite is passed [mcp-lifecycle]")
{
    auto server = MakeWordServer();
    server->Initialize();

    std::ofstream(server->Path("taken.md")) << "existing";

    const auto created = server->Call("create_document", nlohmann::json{{"path", "input.docx"}});
    const auto documentId = created["data"]["documentId"].get<std::string>();
    static_cast<void>(server->Call("save_document", nlohmann::json{{"documentId", documentId}}));

    const auto refused = server->Call(
        "convert_document", nlohmann::json{{"input_path", "input.docx"}, {"output_path", "taken.md"}});
    CHECK(refused["ok"] == false);
    CHECK(refused["error"]["code"] == "file_exists");

    const auto allowed = server->Call("convert_document", nlohmann::json{{"input_path", "input.docx"},
                                                                         {"output_path", "taken.md"},
                                                                         {"overwrite", true}});
    CHECK(allowed["ok"] == true);
}

TEST_CASE("batch applies every operation or none of them [mcp-lifecycle]")
{
    auto server = MakeWordServer();
    server->Initialize();

    const auto created = server->Call("create_document", nlohmann::json::object());
    const auto documentId = created["data"]["documentId"].get<std::string>();

    const auto anchor = nlohmann::json{{"position", "end"}};
    const auto applied =
        server->Call("batch", nlohmann::json{{"documentId", documentId},
                                             {"operations", nlohmann::json::array({
                                                                nlohmann::json{{"tool", "insert_paragraph"},
                                                                               {"arguments", nlohmann::json{{"anchor", anchor},
                                                                                                            {"text", "One"}}}},
                                                                nlohmann::json{{"tool", "insert_paragraph"},
                                                                               {"arguments", nlohmann::json{{"anchor", anchor},
                                                                                                            {"text", "Two"}}}},
                                                            })}});
    REQUIRE(applied["ok"] == true);
    CHECK(applied["data"]["applied"] == 2);
    // The whole batch is one undo unit, so it counts as a single revision.
    CHECK(applied["revision"] == 1);

    const auto rolledBack =
        server->Call("batch", nlohmann::json{{"documentId", documentId},
                                             {"operations", nlohmann::json::array({
                                                                nlohmann::json{{"tool", "insert_paragraph"},
                                                                               {"arguments", nlohmann::json{{"anchor", anchor},
                                                                                                            {"text", "Three"}}}},
                                                                nlohmann::json{{"tool", "apply_style"},
                                                                               {"arguments", nlohmann::json{{"blocks", nlohmann::json::array({1})},
                                                                                                            {"style_id", "NoSuchStyle"}}}},
                                                            })}});
    CHECK(rolledBack["ok"] == false);
    CHECK(rolledBack["error"]["code"] == "batch_aborted");
    CHECK(rolledBack["error"]["details"][0]["failedIndex"] == 1);

    const auto blocks = server->Call("read_blocks", nlohmann::json{{"documentId", documentId}});
    CHECK(blocks["data"]["blockCount"] == 2);
}

TEST_CASE("batch refuses tools that are not mutating session tools [mcp-lifecycle]")
{
    auto server = MakeWordServer();
    server->Initialize();

    const auto created = server->Call("create_document", nlohmann::json::object());
    const auto documentId = created["data"]["documentId"].get<std::string>();

    const auto refused = server->Call(
        "batch", nlohmann::json{{"documentId", documentId},
                                {"operations", nlohmann::json::array({nlohmann::json{
                                                   {"tool", "save_document"}, {"arguments", nlohmann::json::object()}}})}});
    CHECK(refused["ok"] == false);
    CHECK(refused["error"]["code"] == "input_invalid");

    const auto nested = server->Call(
        "batch", nlohmann::json{{"documentId", documentId},
                                {"operations", nlohmann::json::array({nlohmann::json{
                                                   {"tool", "batch"}, {"arguments", nlohmann::json::object()}}})}});
    CHECK(nested["ok"] == false);
}

TEST_CASE("media is listed and exported [mcp-lifecycle]")
{
    auto server = MakeWordServer();
    server->Initialize();

    const auto created = server->Call("create_document", nlohmann::json::object());
    const auto documentId = created["data"]["documentId"].get<std::string>();

    // A minimal 1x1 PNG, so the payload passes format detection.
    const std::string png =
        "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAEhQGAhKmMIQAAAABJRU5ErkJggg==";

    const auto image = server->Call("insert_image", nlohmann::json{{"documentId", documentId},
                                                                   {"anchor", nlohmann::json{{"position", "end"}}},
                                                                   {"dataBase64", png}});
    REQUIRE(image["ok"] == true);
    CHECK(image["data"]["contentType"] == "image/png");

    const auto listed = server->Call("list_media", nlohmann::json{{"documentId", documentId}});
    REQUIRE(listed["ok"] == true);
    REQUIRE(listed["data"]["media"].size() == 1);
    const auto mediaId = listed["data"]["media"][0]["id"].get<std::string>();

    const auto fetched =
        server->CallRaw("get_media", nlohmann::json{{"documentId", documentId}, {"media_id", mediaId}});
    REQUIRE(fetched["isError"] == false);
    REQUIRE(fetched["content"].size() == 2);
    CHECK(fetched["content"][1]["type"] == "image");
    CHECK(fetched["content"][1]["mimeType"] == "image/png");

    const auto missing =
        server->Call("get_media", nlohmann::json{{"documentId", documentId}, {"media_id", "media99"}});
    CHECK(missing["ok"] == false);
    CHECK(missing["error"]["code"] == "media_not_found");

    const auto exported =
        server->Call("export_media", nlohmann::json{{"documentId", documentId}, {"output_dir", "media"}});
    REQUIRE(exported["ok"] == true);
    CHECK(exported["data"]["files"].size() == 1);
}

TEST_CASE("media without a typed content block is refused in favour of export_media [mcp-lifecycle]")
{
    auto server = MakeWordServer();
    server->Initialize();

    // Only image and audio have a typed MCP content block. Video, OLE objects
    // and embedded packages would need an embedded resource, which the
    // specification pairs with a `resources` capability this server does not
    // declare and could not honour.
    {
        auto editor = ExyokiOffice::Word::WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);
        auto mainPart = editor->GetDocument()->GetMainDocumentPart();
        REQUIRE(mainPart != nullptr);

        auto videoPart = mainPart->AddEmbeddedObjectPart();
        REQUIRE(videoPart != nullptr);
        videoPart->SetContentType("video/mp4");
        videoPart->SetBinaryData({0, 0, 0, 24, 'f', 't', 'y', 'p'});
        REQUIRE(editor->SaveToFile(server->Path("video.docx")));
    }

    const auto listed = server->Call("list_media", nlohmann::json{{"path", "video.docx"}});
    REQUIRE(listed["ok"] == true);
    REQUIRE(listed["data"]["media"].size() == 1);
    CHECK(listed["data"]["media"][0]["contentType"] == "video/mp4");
    const auto mediaId = listed["data"]["media"][0]["id"].get<std::string>();

    const auto refused = server->Call("get_media", nlohmann::json{{"path", "video.docx"}, {"media_id", mediaId}});
    CHECK(refused["ok"] == false);
    CHECK(refused["error"]["code"] == "unsupported");
    CHECK(refused["error"]["hint"].get<std::string>().find("export_media") != std::string::npos);

    // The payload itself is still reachable; it just goes to a file.
    const auto exported =
        server->Call("export_media", nlohmann::json{{"path", "video.docx"}, {"output_dir", "media"}});
    REQUIRE(exported["ok"] == true);
    CHECK(exported["data"]["files"].size() == 1);
}
