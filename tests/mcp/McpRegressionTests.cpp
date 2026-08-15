// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "McpTestSupport.hpp"

#include "Results.hpp"

#include <doctest/doctest.h>

#include <algorithm>
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
