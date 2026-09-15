// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "McpTestSupport.hpp"

#include "Results.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>

using namespace ExyokiOfficeTests;
using namespace ExyokiOffice::Mcp;

/**
 * @brief Regressions for defects found while auditing the MCP layer.
 *
 * Each case pins one behavior that was wrong before: the protocol revisions the
 * server may honestly claim, the shapes a JSON-RPC message may take, the
 * bookkeeping undo and batch rollback restore, and the payload limits the
 * reading tools promise to respect.
 */

/// File-local helpers for the regression cases.
class McpRegressionSupport
{
public:
    /// Reads one session's `list_documents` entry, where dirty and revision live.
    [[nodiscard]] static nlohmann::json SessionEntry(McpTestServer& server, const std::string& documentId)
    {
        const auto listed = server.Call("list_documents", nlohmann::json::object());
        if (!listed.is_object() || !listed.contains("data"))
        {
            return nlohmann::json::object();
        }

        for (const auto& entry : listed["data"]["documents"])
        {
            if (entry.value("documentId", std::string()) == documentId)
            {
                return entry;
            }
        }

        return nlohmann::json::object();
    }

    /// Creates a document at @p path and returns its session id.
    [[nodiscard]] static std::string CreateDocument(McpTestServer& server, const std::string& path)
    {
        const auto created = server.Call("create_document", nlohmann::json{{"path", path}});
        return created["data"]["documentId"].get<std::string>();
    }

    /// Appends one paragraph at the end of the body.
    [[nodiscard]] static nlohmann::json AppendParagraph(McpTestServer& server, const std::string& documentId,
                                                        const std::string& text)
    {
        return server.Call("insert_paragraph", nlohmann::json{{"documentId", documentId},
                                                              {"anchor", nlohmann::json{{"position", "end"}}},
                                                              {"text", text}});
    }
};

// ---------------------------------------------------------------------------
// Protocol
// ---------------------------------------------------------------------------

TEST_CASE("the newest offered revision is the stable one and batching is never promised [mcp-protocol]")
{
    const auto& supported = McpServer::SupportedProtocolVersions();

    CHECK(McpServer::LatestProtocolVersion() == "2025-11-25");
    CHECK(std::find(supported.begin(), supported.end(), "2025-11-25") != supported.end());

    // 2025-03-26 is the one revision that requires servers to accept JSON-RPC
    // batches. The transport reads one message per line, so offering it would
    // be a promise the server cannot keep.
    CHECK(std::find(supported.begin(), supported.end(), "2025-03-26") == supported.end());

    for (const auto& version : supported)
    {
        CHECK(version <= std::string(McpServer::LatestProtocolVersion()));
    }
}

TEST_CASE("a requested revision the server implements is echoed unchanged [mcp-protocol]")
{
    for (const auto& version : McpServer::SupportedProtocolVersions())
    {
        auto server = MakeWordServer();

        nlohmann::json initialize = nlohmann::json::object();
        initialize["jsonrpc"] = "2.0";
        initialize["id"] = 1;
        initialize["method"] = "initialize";
        initialize["params"] = MakeInitializeParams(version);

        const auto response = server->Send(initialize);
        REQUIRE(response.contains("result"));
        CHECK(response["result"]["protocolVersion"] == version);
    }
}

TEST_CASE("a JSON-RPC batch is rejected as an invalid request [mcp-protocol]")
{
    auto server = MakeWordServer();
    server->Initialize();

    const auto response = server->Server().HandleMessage(
        R"([{"jsonrpc":"2.0","id":1,"method":"ping"},{"jsonrpc":"2.0","id":2,"method":"ping"}])");
    REQUIRE(response.has_value());

    const auto parsed = nlohmann::json::parse(*response);
    REQUIRE(parsed.contains("error"));
    CHECK(parsed["error"]["code"] == -32600);
    CHECK(parsed["error"]["message"].get<std::string>().find("batch") != std::string::npos);
}

TEST_CASE("an explicit null id is rejected instead of being treated as a notification [mcp-protocol]")
{
    auto server = MakeWordServer();
    server->Initialize();

    // Answering nothing would leave the client waiting for a response forever,
    // so the malformed id is reported rather than silently demoted.
    const auto response = server->Server().HandleMessage(R"({"jsonrpc":"2.0","id":null,"method":"ping"})");
    REQUIRE(response.has_value());

    const auto parsed = nlohmann::json::parse(*response);
    REQUIRE(parsed.contains("error"));
    CHECK(parsed["error"]["code"] == -32600);
}

TEST_CASE("a malformed message without an id is dropped without a response [mcp-protocol]")
{
    auto server = MakeWordServer();
    server->Initialize();

    CHECK_FALSE(server->Server().HandleMessage(R"({"jsonrpc":"2.0","method":42})").has_value());
    CHECK_FALSE(server->Server().HandleMessage(R"({"jsonrpc":"2.0","method":"ping","params":[1,2]})").has_value());
}

TEST_CASE("a wrong jsonrpc version and positional parameters are invalid requests [mcp-protocol]")
{
    auto server = MakeWordServer();
    server->Initialize();

    const auto wrongVersion = server->Server().HandleMessage(R"({"jsonrpc":"1.0","id":1,"method":"ping"})");
    REQUIRE(wrongVersion.has_value());
    CHECK(nlohmann::json::parse(*wrongVersion)["error"]["code"] == -32600);

    const auto positional = server->Server().HandleMessage(R"({"jsonrpc":"2.0","id":2,"method":"ping","params":[]})");
    REQUIRE(positional.has_value());
    CHECK(nlohmann::json::parse(*positional)["error"]["code"] == -32600);
}

TEST_CASE("a request method sent without an id is never answered [mcp-protocol]")
{
    auto server = MakeWordServer();

    CHECK_FALSE(server->Server()
                    .HandleMessage(R"({"jsonrpc":"2.0","method":"initialize","params":{}})")
                    .has_value());
    CHECK_FALSE(server->Server().HandleMessage(R"({"jsonrpc":"2.0","method":"tools/list"})").has_value());
}

TEST_CASE("ping is answered before initialization [mcp-protocol]")
{
    auto server = MakeWordServer();

    const auto response = server->Server().HandleMessage(R"({"jsonrpc":"2.0","id":1,"method":"ping"})");
    REQUIRE(response.has_value());

    const auto parsed = nlohmann::json::parse(*response);
    REQUIRE(parsed.contains("result"));
    CHECK(parsed["result"].empty());
}

TEST_CASE("non-object tool arguments are a protocol error, not a tool error [mcp-protocol]")
{
    auto server = MakeWordServer();
    server->Initialize();

    nlohmann::json message = nlohmann::json::object();
    message["jsonrpc"] = "2.0";
    message["id"] = 5;
    message["method"] = "tools/call";
    message["params"] = nlohmann::json{{"name", "list_documents"}, {"arguments", "not an object"}};

    const auto response = server->Send(message);
    REQUIRE(response.contains("error"));
    CHECK(response["error"]["code"] == -32602);
}

// ---------------------------------------------------------------------------
// Serialization and budgets
// ---------------------------------------------------------------------------

TEST_CASE("serialization replaces invalid UTF-8 instead of throwing [mcp-protocol]")
{
    // A document may carry a malformed sequence, and the default dump() throws
    // on it, which would end the process in the middle of a response.
    nlohmann::json value = nlohmann::json::object();
    value["text"] = std::string("valid\xC3\x28invalid");

    std::string serialized;
    CHECK_NOTHROW(serialized = SerializeJson(value));
    CHECK_FALSE(serialized.empty());
    CHECK(serialized.find("valid") != std::string::npos);

    const auto parsed = nlohmann::json::parse(serialized, nullptr, false);
    CHECK_FALSE(parsed.is_discarded());
}

TEST_CASE("array truncation keeps a prefix and reports what it dropped [mcp-protocol]")
{
    nlohmann::json items = nlohmann::json::array();
    for (int index = 0; index < 200; ++index)
    {
        items.push_back(std::string(100, 'x'));
    }

    const auto original = items.size();
    CHECK(TruncateArrayToBudget(items, 1024));
    CHECK(items.size() < original);
    CHECK(SerializeJson(items).size() <= 1024);

    nlohmann::json small = nlohmann::json::array({1, 2, 3});
    CHECK_FALSE(TruncateArrayToBudget(small, 1024));
    CHECK(small.size() == 3);
}

// ---------------------------------------------------------------------------
// Sessions
// ---------------------------------------------------------------------------

TEST_CASE("a limited workspace listing is the first page of a stable order [mcp-lifecycle]")
{
    auto server = MakeWordServer();
    server->Initialize();

    for (const auto* name : {"delta.docx", "alpha.docx", "charlie.docx", "bravo.docx"})
    {
        const auto created = server->Call("create_document", nlohmann::json{{"path", name}});
        REQUIRE(created["ok"] == true);
        const auto id = created["data"]["documentId"].get<std::string>();
        REQUIRE(server->Call("save_document", nlohmann::json{{"documentId", id}})["ok"] == true);
        REQUIRE(server->Call("close_document", nlohmann::json{{"documentId", id}})["ok"] == true);
    }

    const auto listed = server->Call("list_workspace", nlohmann::json{{"limit", 2}});
    REQUIRE(listed["ok"] == true);

    const auto& files = listed["data"]["files"];
    REQUIRE(files.size() == 2);
    CHECK(files[0]["path"] == "alpha.docx");
    CHECK(files[1]["path"] == "bravo.docx");

    // Hitting the limit is a shortened answer and must be reported as one.
    CHECK(listed["truncated"] == true);

    const auto all = server->Call("list_workspace", nlohmann::json::object());
    CHECK(all["truncated"] == false);
    CHECK(all["data"]["files"].size() == 4);
}

TEST_CASE("consecutive undo steps report the revision they restored [mcp-lifecycle]")
{
    auto server = MakeWordServer();
    server->Initialize();

    const auto id = McpRegressionSupport::CreateDocument(*server, "undo.docx");
    REQUIRE(McpRegressionSupport::AppendParagraph(*server, id, "First")["ok"] == true);
    REQUIRE(McpRegressionSupport::AppendParagraph(*server, id, "Second")["ok"] == true);

    const auto first = server->Call("undo", nlohmann::json{{"documentId", id}});
    REQUIRE(first["ok"] == true);
    CHECK(first["data"]["restoredRevision"] == 1);

    // The counter keeps rising while the content walks backwards, so the second
    // undo restores revision 0 rather than whatever the counter now reads.
    const auto second = server->Call("undo", nlohmann::json{{"documentId", id}});
    REQUIRE(second["ok"] == true);
    CHECK(second["data"]["restoredRevision"] == 0);

    const auto exhausted = server->Call("undo", nlohmann::json{{"documentId", id}});
    CHECK(exhausted["ok"] == false);
    CHECK(exhausted["error"]["code"] == "snapshot_unavailable");
}

TEST_CASE("a failed batch restores the unsaved-changes flag as well as the content [mcp-lifecycle]")
{
    auto server = MakeWordServer();
    server->Initialize();

    const auto id = McpRegressionSupport::CreateDocument(*server, "batch.docx");
    REQUIRE(server->Call("save_document", nlohmann::json{{"documentId", id}})["ok"] == true);

    const auto saved = McpRegressionSupport::SessionEntry(*server, id);
    REQUIRE(saved["dirty"] == false);

    nlohmann::json operations = nlohmann::json::array();
    operations.push_back(nlohmann::json{{"tool", "insert_paragraph"},
                                        {"arguments", nlohmann::json{{"anchor", nlohmann::json{{"position", "end"}}},
                                                                     {"text", "Applied"}}}});
    operations.push_back(
        nlohmann::json{{"tool", "apply_style"},
                       {"arguments", nlohmann::json{{"blocks", nlohmann::json::array({1})},
                                                    {"style_id", "NoSuchStyle"}}}});

    const auto failed = server->Call("batch", nlohmann::json{{"documentId", id}, {"operations", operations}});
    REQUIRE(failed["ok"] == false);
    CHECK(failed["error"]["code"] == "batch_aborted");
    CHECK(failed["error"]["details"][0]["failedIndex"] == 1);
    CHECK(failed["error"]["details"][0]["restored"] == true);

    // The document was saved before the batch, so rolling the batch back must
    // leave it clean; reporting unsaved changes would send the agent to save a
    // file that is already on disk.
    const auto after = McpRegressionSupport::SessionEntry(*server, id);
    CHECK(after["dirty"] == false);
    CHECK(after["revision"] == saved["revision"]);
}

TEST_CASE("validate_document never reports more issues than max_issues [mcp-lifecycle]")
{
    auto server = MakeWordServer();
    server->Initialize();

    const auto created = server->Call("create_document", nlohmann::json{{"path", "validate.docx"}});
    const auto id = created["data"]["documentId"].get<std::string>();
    REQUIRE(server->Call("save_document", nlohmann::json{{"documentId", id}})["ok"] == true);

    for (const int cap : {1, 2, 5})
    {
        const auto report = server->Call("validate_document", nlohmann::json{{"documentId", id}, {"max_issues", cap}});
        REQUIRE(report["ok"] == true);
        CHECK(report["data"]["issues"].size() <= static_cast<std::size_t>(cap));
    }
}

// ---------------------------------------------------------------------------
// Defects found by the Office COM test (MCP_ERRORS.md, shared entries)
// ---------------------------------------------------------------------------

/// Helpers for the shared-code regressions of the COM test.
class ComRegressionSupport
{
public:
    /// Appends @p count paragraphs one call at a time, so each is one undo step.
    static void FillHistory(McpTestServer& server, const std::string& documentId, int count)
    {
        for (int index = 1; index <= count; ++index)
        {
            REQUIRE(McpRegressionSupport::AppendParagraph(server, documentId, "Step " + std::to_string(index))["ok"] ==
                    true);
        }
    }

    [[nodiscard]] static nlohmann::json InsertOperation(const std::string& text)
    {
        return nlohmann::json{{"tool", "insert_paragraph"},
                              {"arguments", nlohmann::json{{"anchor", nlohmann::json{{"position", "end"}}},
                                                           {"text", text}}}};
    }

    [[nodiscard]] static std::size_t BlockCount(McpTestServer& server, const std::string& documentId)
    {
        const auto blocks = server.Call("read_blocks", nlohmann::json{{"documentId", documentId}});
        return blocks["data"]["blockCount"].get<std::size_t>();
    }

    /// A 1x1 PNG, so the payload passes format detection.
    [[nodiscard]] static std::string Png()
    {
        return "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAEhQGAhKmMIQAAAABJRU5ErkJggg==";
    }
};

TEST_CASE("S-1: a failed batch over a full undo history leaves the history intact [mcp-lifecycle]")
{
    // The default depth is eight snapshots. Eight mutations fill the history;
    // the batch's own per-operation snapshots then used to evict the oldest
    // real steps, and the rollback trimmed nothing because the size never
    // exceeded the count recorded at the start.
    auto server = MakeWordServer();
    server->Initialize();

    const auto id = McpRegressionSupport::CreateDocument(*server, "history.docx");
    ComRegressionSupport::FillHistory(*server, id, 8);
    REQUIRE(ComRegressionSupport::BlockCount(*server, id) == 8);

    nlohmann::json operations = nlohmann::json::array();
    operations.push_back(ComRegressionSupport::InsertOperation("Nine"));
    operations.push_back(
        nlohmann::json{{"tool", "apply_style"},
                       {"arguments", nlohmann::json{{"blocks", nlohmann::json::array({1})},
                                                    {"style_id", "NoSuchStyle"}}}});
    const auto failed = server->Call("batch", nlohmann::json{{"documentId", id}, {"operations", operations}});
    REQUIRE(failed["ok"] == false);
    CHECK(failed["error"]["code"] == "batch_aborted");
    CHECK(ComRegressionSupport::BlockCount(*server, id) == 8);

    // The step before the batch is the eighth paragraph; undoing it must
    // restore seven paragraphs, not the eight the batch was rolled back to.
    const auto undone = server->Call("undo", nlohmann::json{{"documentId", id}});
    REQUIRE(undone["ok"] == true);
    CHECK(undone["data"]["restoredRevision"] == 7);
    CHECK(ComRegressionSupport::BlockCount(*server, id) == 7);
}

TEST_CASE("S-1: a committed batch over a full undo history is one step and the steps before it survive [mcp-lifecycle]")
{
    auto server = MakeWordServer();
    server->Initialize();

    const auto id = McpRegressionSupport::CreateDocument(*server, "history.docx");
    ComRegressionSupport::FillHistory(*server, id, 8);

    nlohmann::json operations = nlohmann::json::array();
    operations.push_back(ComRegressionSupport::InsertOperation("Nine"));
    operations.push_back(ComRegressionSupport::InsertOperation("Ten"));
    const auto applied = server->Call("batch", nlohmann::json{{"documentId", id}, {"operations", operations}});
    REQUIRE(applied["ok"] == true);
    CHECK(applied["revision"] == 9);
    CHECK(ComRegressionSupport::BlockCount(*server, id) == 10);

    // One undo removes both paragraphs of the batch; the next restores the
    // step before it. Before the fix the second undo brought back the batch's
    // intermediate state - nine paragraphs, a state the agent never saw - and
    // reported a revision higher than the one the first undo restored.
    const auto first = server->Call("undo", nlohmann::json{{"documentId", id}});
    REQUIRE(first["ok"] == true);
    CHECK(first["data"]["restoredRevision"] == 8);
    CHECK(ComRegressionSupport::BlockCount(*server, id) == 8);

    const auto second = server->Call("undo", nlohmann::json{{"documentId", id}});
    REQUIRE(second["ok"] == true);
    CHECK(second["data"]["restoredRevision"] == 7);
    CHECK(ComRegressionSupport::BlockCount(*server, id) == 7);

    // The history is still bounded by the depth: eight steps in total, so
    // six more undo calls succeed and the ninth is refused.
    for (int index = 0; index < 6; ++index)
    {
        REQUIRE(server->Call("undo", nlohmann::json{{"documentId", id}})["ok"] == true);
    }
    CHECK(ComRegressionSupport::BlockCount(*server, id) == 1);
    const auto exhausted = server->Call("undo", nlohmann::json{{"documentId", id}});
    CHECK(exhausted["ok"] == false);
    CHECK(exhausted["error"]["code"] == "snapshot_unavailable");
}

TEST_CASE("S-2: export_media onto existing files answers file_exists instead of exporting nothing [mcp-lifecycle]")
{
    auto server = MakeWordServer();
    server->Initialize();

    const auto id = McpRegressionSupport::CreateDocument(*server, "media.docx");
    REQUIRE(server->Call("insert_image", nlohmann::json{{"documentId", id},
                                                        {"anchor", nlohmann::json{{"position", "end"}}},
                                                        {"dataBase64", ComRegressionSupport::Png()}})["ok"] == true);

    const auto exported = server->Call("export_media", nlohmann::json{{"documentId", id}, {"output_dir", "exp"}});
    REQUIRE(exported["ok"] == true);
    REQUIRE(exported["data"]["files"].size() == 1);

    const auto again = server->Call("export_media", nlohmann::json{{"documentId", id}, {"output_dir", "exp"}});
    REQUIRE(again["ok"] == false);
    CHECK(again["error"]["code"] == "file_exists");

    const auto forced =
        server->Call("export_media", nlohmann::json{{"documentId", id}, {"output_dir", "exp"}, {"overwrite", true}});
    REQUIRE(forced["ok"] == true);
    CHECK(forced["data"]["files"].size() == 1);
}

TEST_CASE("S-3: a document of another family is refused with family_mismatch, not package_load_failed [mcp-lifecycle]")
{
    auto excel = MakeExcelServer();
    excel->Initialize();
    const auto book = McpRegressionSupport::CreateDocument(*excel, "book.xlsx");
    REQUIRE(excel->Call("save_document", nlohmann::json{{"documentId", book}})["ok"] == true);

    auto word = MakeWordServer();
    word->Initialize();
    std::filesystem::copy_file(excel->Path("book.xlsx"), word->Path("book.xlsx"));
    std::filesystem::copy_file(excel->Path("book.xlsx"), word->Path("renamed.docx"));

    for (const auto* name : {"book.xlsx", "renamed.docx"})
    {
        const auto opened = word->Call("open_document", nlohmann::json{{"path", name}});
        REQUIRE(opened["ok"] == false);
        CHECK(opened["error"]["code"] == "family_mismatch");

        // The path form of the reading tools goes through the same door.
        const auto info = word->Call("get_document_info", nlohmann::json{{"path", name}});
        REQUIRE(info["ok"] == false);
        CHECK(info["error"]["code"] == "family_mismatch");
    }

    // A file that is no package at all still says so.
    std::ofstream(word->Path("junk.docx"), std::ios::binary) << "not a zip";
    const auto junk = word->Call("open_document", nlohmann::json{{"path", "junk.docx"}});
    REQUIRE(junk["ok"] == false);
    CHECK(junk["error"]["code"] == "package_load_failed");
}

TEST_CASE("S-4: split_document reports path_invalid for a bad prefix and file_exists for taken outputs [mcp-lifecycle]")
{
    auto server = MakeWordServer();
    server->Initialize();

    const auto id = McpRegressionSupport::CreateDocument(*server, "split.docx");
    ComRegressionSupport::FillHistory(*server, id, 3);
    REQUIRE(server->Call("save_document", nlohmann::json{{"documentId", id}})["ok"] == true);

    for (const auto* prefix : {"a/b", "NUL", "..", "part."})
    {
        const auto refused = server->Call("split_document", nlohmann::json{{"input_path", "split.docx"},
                                                                           {"output_dir", "parts"},
                                                                           {"by", "paragraphs"},
                                                                           {"count", 1},
                                                                           {"prefix", prefix}});
        REQUIRE(refused["ok"] == false);
        CHECK(refused["error"]["code"] == "path_invalid");
    }

    const auto arguments = nlohmann::json{
        {"input_path", "split.docx"}, {"output_dir", "parts"}, {"by", "paragraphs"}, {"count", 1}};
    const auto written = server->Call("split_document", arguments);
    REQUIRE(written["ok"] == true);
    REQUIRE(written["data"]["outputFiles"].size() >= 3);

    const auto taken = server->Call("split_document", arguments);
    REQUIRE(taken["ok"] == false);
    CHECK(taken["error"]["code"] == "file_exists");

    auto forced = arguments;
    forced["overwrite"] = true;
    CHECK(server->Call("split_document", forced)["ok"] == true);
}

TEST_CASE("X-9 and P-10e: a model scope naming a missing sheet or slide is an error, not an empty model [mcp-lifecycle]")
{
    auto excel = MakeExcelServer();
    excel->Initialize();
    const auto book = McpRegressionSupport::CreateDocument(*excel, "scope.xlsx");
    for (const auto* tool : {"get_document_model", "get_document_markdown"})
    {
        const auto missing =
            excel->Call(tool, nlohmann::json{{"documentId", book}, {"scope", nlohmann::json{{"sheet", "Nope"}}}});
        REQUIRE(missing["ok"] == false);
        CHECK(missing["error"]["code"] == "sheet_not_found");
    }
    const auto sheets = excel->Call("list_sheets", nlohmann::json{{"documentId", book}});
    REQUIRE(sheets["ok"] == true);
    const auto sheetName = sheets["data"]["sheets"][0]["name"].get<std::string>();
    const auto present = excel->Call(
        "get_document_model", nlohmann::json{{"documentId", book}, {"scope", nlohmann::json{{"sheet", sheetName}}}});
    REQUIRE(present["ok"] == true);
    CHECK(present["data"]["itemCount"] == 1);

    auto powerPoint = MakePowerPointServer();
    powerPoint->Initialize();
    const auto deck = McpRegressionSupport::CreateDocument(*powerPoint, "scope.pptx");
    REQUIRE(powerPoint->Call("add_slide", nlohmann::json{{"documentId", deck}})["ok"] == true);
    for (const auto* tool : {"get_document_model", "get_document_markdown"})
    {
        const auto missing =
            powerPoint->Call(tool, nlohmann::json{{"documentId", deck}, {"scope", nlohmann::json{{"slide", 99}}}});
        REQUIRE(missing["ok"] == false);
        CHECK(missing["error"]["code"] == "slide_not_found");
    }
    const auto first = powerPoint->Call(
        "get_document_model", nlohmann::json{{"documentId", deck}, {"scope", nlohmann::json{{"slide", 1}}}});
    REQUIRE(first["ok"] == true);
    CHECK(first["data"]["itemCount"] == 1);
}

TEST_CASE("P-10b: a malformed regex, XPath or part name is input_invalid, not operation_failed [mcp-lifecycle]")
{
    auto server = MakeWordServer();
    server->Initialize();

    const auto id = McpRegressionSupport::CreateDocument(*server, "malformed.docx");
    REQUIRE(McpRegressionSupport::AppendParagraph(*server, id, "Some text")["ok"] == true);

    const auto search =
        server->Call("search_text", nlohmann::json{{"documentId", id}, {"needle", "("}, {"regex", true}});
    REQUIRE(search["ok"] == false);
    CHECK(search["error"]["code"] == "input_invalid");

    const auto replace = server->Call(
        "replace_text", nlohmann::json{{"documentId", id}, {"needle", "("}, {"replacement", "x"}, {"regex", true}});
    REQUIRE(replace["ok"] == false);
    CHECK(replace["error"]["code"] == "input_invalid");

    const auto xpath = server->Call("query_xml", nlohmann::json{{"documentId", id}, {"xpath", "//w:["}});
    REQUIRE(xpath["ok"] == false);
    CHECK(xpath["error"]["code"] == "input_invalid");

    const auto part = server->Call(
        "query_xml", nlohmann::json{{"documentId", id}, {"xpath", "//w:p"}, {"part", "/word/nope.xml"}});
    REQUIRE(part["ok"] == false);
    CHECK(part["error"]["code"] == "input_invalid");

    // An empty needle is a caller error as well.
    const auto empty = server->Call("search_text", nlohmann::json{{"documentId", id}, {"needle", ""}});
    REQUIRE(empty["ok"] == false);
    CHECK(empty["error"]["code"] == "input_invalid");
}

TEST_CASE("P-10d: set_properties refuses a custom value it cannot store instead of dropping it [mcp-lifecycle]")
{
    auto server = MakeWordServer();
    server->Initialize();

    const auto id = McpRegressionSupport::CreateDocument(*server, "properties.docx");

    const auto nested =
        server->Call("set_properties", nlohmann::json{{"documentId", id},
                                                      {"custom", nlohmann::json{{"Bad", nlohmann::json{{"nested", 1}}}}}});
    REQUIRE(nested["ok"] == false);
    CHECK(nested["error"]["code"] == "input_invalid");
    CHECK(nested["error"]["message"].get<std::string>().find("Bad") != std::string::npos);

    // Sent together with a valid member, the bad value used to be dropped
    // silently while the call reported success. Now nothing is written.
    const auto mixed = server->Call("set_properties",
                                    nlohmann::json{{"documentId", id},
                                                   {"title", "Kept?"},
                                                   {"custom", nlohmann::json{{"List", nlohmann::json::array({1, 2})}}}});
    REQUIRE(mixed["ok"] == false);
    CHECK(mixed["error"]["code"] == "input_invalid");
    const auto read = server->Call("get_properties", nlohmann::json{{"documentId", id}});
    REQUIRE(read["ok"] == true);
    CHECK(read["data"]["core"].value("title", std::string()).empty());

    const auto nothing = server->Call("set_properties", nlohmann::json{{"documentId", id}});
    REQUIRE(nothing["ok"] == false);
    CHECK(nothing["error"]["code"] == "input_invalid");

    const auto typed = server->Call("set_properties", nlohmann::json{{"documentId", id}, {"title", 42}});
    REQUIRE(typed["ok"] == false);
    CHECK(typed["error"]["code"] == "input_invalid");
}

TEST_CASE("W-11c: diff_documents reports part change kinds its schema enumerates [mcp-lifecycle]")
{
    auto server = MakeWordServer();
    server->Initialize();

    const auto id = McpRegressionSupport::CreateDocument(*server, "before.docx");
    REQUIRE(McpRegressionSupport::AppendParagraph(*server, id, "Before")["ok"] == true);
    REQUIRE(server->Call("save_document", nlohmann::json{{"documentId", id}})["ok"] == true);
    REQUIRE(McpRegressionSupport::AppendParagraph(*server, id, "After")["ok"] == true);
    REQUIRE(server->Call("save_document", nlohmann::json{{"documentId", id}, {"path", "after.docx"}})["ok"] == true);

    const auto diff =
        server->Call("diff_documents", nlohmann::json{{"left_path", "before.docx"}, {"right_path", "after.docx"}});
    REQUIRE(diff["ok"] == true);
    CHECK(diff["data"]["identical"] == false);
    REQUIRE(diff["data"]["partChanges"].size() >= 1);

    // The kinds the handler emits ("changedXml" among them) used to be absent
    // from the schema, whose description promised a plain "changed".
    const auto* tool = server->Registry().Find("diff_documents");
    REQUIRE(tool != nullptr);
    const auto allowed = tool->Definition.OutputSchema["properties"]["data"]["properties"]["partChanges"]["items"]
                                                      ["properties"]["kind"]["enum"];
    REQUIRE(allowed.is_array());
    for (const auto& change : diff["data"]["partChanges"])
    {
        const auto kind = change["kind"].get<std::string>();
        CHECK(std::find(allowed.begin(), allowed.end(), nlohmann::json(kind)) != allowed.end());
    }
    CHECK(std::find(allowed.begin(), allowed.end(), nlohmann::json("changedXml")) != allowed.end());
}
