// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "McpTestSupport.hpp"

#include "StdioTransport.hpp"

#include <doctest/doctest.h>

#include <filesystem>
#include <sstream>
#include <string>

using namespace ExyokiOfficeTests;
using namespace ExyokiOffice::Mcp;

/// Sends one `initialize` request with @p params and returns the response.
static nlohmann::json SendInitialize(McpTestServer& server, const nlohmann::json& params)
{
    nlohmann::json initialize = nlohmann::json::object();
    initialize["jsonrpc"] = "2.0";
    initialize["id"] = 1;
    initialize["method"] = "initialize";
    initialize["params"] = params;
    return server.Send(initialize);
}

TEST_CASE("the transport drops a line longer than the accepted maximum [mcp-protocol]")
{
    // A line is read before anything can tell whether it is a message, so an
    // unbounded one is an out-of-memory condition rather than a protocol error.
    std::string input;
    input.append(StdioTransport::MaximumLineBytes + 16, 'x');
    input.push_back('\n');
    input.append("{\"jsonrpc\":\"2.0\"}\n");

    std::istringstream in(input);
    std::ostringstream out;
    StdioTransport transport(in, out);

    std::string line;
    bool oversized = false;

    REQUIRE(transport.ReadLine(line, oversized));
    CHECK(oversized);
    CHECK(line.empty());

    // The rest of the oversized line was consumed, so the next read starts on a
    // message boundary instead of in the middle of the one that was dropped.
    REQUIRE(transport.ReadLine(line, oversized));
    CHECK_FALSE(oversized);
    CHECK(line == "{\"jsonrpc\":\"2.0\"}");

    CHECK_FALSE(transport.ReadLine(line, oversized));
}

TEST_CASE("the transport accepts a line right at the maximum [mcp-protocol]")
{
    std::string payload(StdioTransport::MaximumLineBytes, 'y');
    std::istringstream in(payload + "\n");
    std::ostringstream out;
    StdioTransport transport(in, out);

    std::string line;
    bool oversized = false;
    REQUIRE(transport.ReadLine(line, oversized));
    CHECK_FALSE(oversized);
    CHECK(line.size() == StdioTransport::MaximumLineBytes);
}

TEST_CASE("initialize reports the server identity and capabilities [mcp-protocol]")
{
    auto server = MakeWordServer();

    const auto response = SendInitialize(*server, MakeInitializeParams("2025-06-18"));
    REQUIRE(response.is_object());
    REQUIRE(response.contains("result"));

    const auto& result = response["result"];
    CHECK(result["protocolVersion"] == "2025-06-18");
    CHECK(result["capabilities"]["tools"]["listChanged"] == false);
    CHECK(result["serverInfo"]["name"] == "exyoki-mcp-test");
    CHECK_FALSE(result["serverInfo"]["version"].get<std::string>().empty());
    CHECK(result["instructions"].get<std::string>().find("create_document") != std::string::npos);

    CHECK(server->Server().IsInitialized());
    CHECK(server->Server().NegotiatedProtocolVersion() == "2025-06-18");
}

TEST_CASE("an unknown protocol version is answered with the newest supported one [mcp-protocol]")
{
    auto server = MakeWordServer();

    const auto response = SendInitialize(*server, MakeInitializeParams("1999-01-01"));
    CHECK(response["result"]["protocolVersion"] == std::string(McpServer::LatestProtocolVersion()));
    CHECK(server->Server().NegotiatedProtocolVersion() == std::string(McpServer::LatestProtocolVersion()));
}

TEST_CASE("initialize without a member the specification requires is refused [mcp-protocol]")
{
    for (const auto& member : McpServer::RequiredInitializeMembers())
    {
        CAPTURE(member);

        auto server = MakeWordServer();
        auto params = MakeInitializeParams();
        params.erase(member);

        const auto response = SendInitialize(*server, params);
        REQUIRE(response.contains("error"));
        CHECK(response["error"]["code"] == -32602);
        CHECK(response["error"]["data"]["required"].size() == McpServer::RequiredInitializeMembers().size());
        CHECK_FALSE(server->Server().IsInitialized());
    }
}

TEST_CASE("initialize with a malformed member is refused [mcp-protocol]")
{
    const auto refused = [](const nlohmann::json& params)
    {
        auto server = MakeWordServer();
        const auto response = SendInitialize(*server, params);
        CHECK_FALSE(server->Server().IsInitialized());
        REQUIRE(response.contains("error"));
        return response["error"]["code"] == -32602;
    };

    auto emptyVersion = MakeInitializeParams();
    emptyVersion["protocolVersion"] = "";
    CHECK(refused(emptyVersion));

    auto numericVersion = MakeInitializeParams();
    numericVersion["protocolVersion"] = 2025;
    CHECK(refused(numericVersion));

    auto listedCapabilities = MakeInitializeParams();
    listedCapabilities["capabilities"] = nlohmann::json::array();
    CHECK(refused(listedCapabilities));

    auto namelessClient = MakeInitializeParams();
    namelessClient["clientInfo"] = nlohmann::json{{"version", "1"}};
    CHECK(refused(namelessClient));

    auto unversionedClient = MakeInitializeParams();
    unversionedClient["clientInfo"] = nlohmann::json{{"name", "probe"}};
    CHECK(refused(unversionedClient));

    // Params that are not an object at all never reach the handshake: the
    // JSON-RPC layer refuses positional parameters for every method.
    auto server = MakeWordServer();
    const auto positional = SendInitialize(*server, nlohmann::json::array());
    REQUIRE(positional.contains("error"));
    CHECK(positional["error"]["code"] == -32600);
    CHECK_FALSE(server->Server().IsInitialized());
}

TEST_CASE("initialize is answered once per connection [mcp-protocol]")
{
    auto server = MakeWordServer();
    server->Initialize();

    const auto response = SendInitialize(*server, MakeInitializeParams("2024-11-05"));
    REQUIRE(response.contains("error"));
    CHECK(response["error"]["code"] == -32600);

    // The refused second handshake leaves the negotiated revision alone.
    CHECK(server->Server().NegotiatedProtocolVersion() == std::string(McpServer::LatestProtocolVersion()));
}

TEST_CASE("tool calls before initialize are rejected [mcp-protocol]")
{
    auto server = MakeWordServer();

    nlohmann::json call = nlohmann::json::object();
    call["jsonrpc"] = "2.0";
    call["id"] = 7;
    call["method"] = "tools/list";

    const auto response = server->Send(call);
    REQUIRE(response.contains("error"));
    CHECK(response["error"]["code"] == -32002);
}

TEST_CASE("the initialized notification cannot stand in for initialize [mcp-protocol]")
{
    auto server = MakeWordServer();

    nlohmann::json initialized = nlohmann::json::object();
    initialized["jsonrpc"] = "2.0";
    initialized["method"] = "notifications/initialized";
    CHECK(server->Send(initialized).is_null());

    // Nothing was negotiated, so the catalog stays locked.
    CHECK_FALSE(server->Server().IsInitialized());
    CHECK_FALSE(server->Server().IsInitializationConfirmed());

    nlohmann::json list = nlohmann::json::object();
    list["jsonrpc"] = "2.0";
    list["id"] = 2;
    list["method"] = "tools/list";

    const auto response = server->Send(list);
    REQUIRE(response.contains("error"));
    CHECK(response["error"]["code"] == -32002);

    // The proper handshake still works afterwards.
    server->Initialize();
    CHECK(server->Server().IsInitialized());
    CHECK(server->Server().IsInitializationConfirmed());
    CHECK(server->Send(list)["result"]["tools"].is_array());
}

TEST_CASE("an unknown method is a JSON-RPC method-not-found error [mcp-protocol]")
{
    auto server = MakeWordServer();
    server->Initialize();

    nlohmann::json call = nlohmann::json::object();
    call["jsonrpc"] = "2.0";
    call["id"] = 3;
    call["method"] = "resources/list";

    const auto response = server->Send(call);
    REQUIRE(response.contains("error"));
    CHECK(response["error"]["code"] == -32601);
}

TEST_CASE("an unparsable line is a parse error, an empty line is ignored [mcp-protocol]")
{
    auto server = MakeWordServer();

    const auto broken = server->Server().HandleMessage("{not json");
    REQUIRE(broken.has_value());
    const auto parsed = nlohmann::json::parse(*broken);
    CHECK(parsed["error"]["code"] == -32700);

    CHECK_FALSE(server->Server().HandleMessage("   ").has_value());
}

TEST_CASE("ping answers with an empty result [mcp-protocol]")
{
    auto server = MakeWordServer();
    server->Initialize();

    nlohmann::json ping = nlohmann::json::object();
    ping["jsonrpc"] = "2.0";
    ping["id"] = 9;
    ping["method"] = "ping";

    const auto response = server->Send(ping);
    REQUIRE(response.contains("result"));
    CHECK(response["result"].is_object());
    CHECK(response["result"].empty());
}

TEST_CASE("notifications produce no response [mcp-protocol]")
{
    auto server = MakeWordServer();
    server->Initialize();

    nlohmann::json cancelled = nlohmann::json::object();
    cancelled["jsonrpc"] = "2.0";
    cancelled["method"] = "notifications/cancelled";
    cancelled["params"] = nlohmann::json{{"requestId", 1}};

    CHECK(server->Send(cancelled).is_null());
}

TEST_CASE("tools/list is stable and starts with the lifecycle group [mcp-protocol]")
{
    auto server = MakeWordServer();
    server->Initialize();

    nlohmann::json list = nlohmann::json::object();
    list["jsonrpc"] = "2.0";
    list["id"] = 2;
    list["method"] = "tools/list";
    list["params"] = nlohmann::json{{"cursor", "ignored"}};

    const auto response = server->Send(list);
    REQUIRE(response.contains("result"));

    const auto& tools = response["result"]["tools"];
    REQUIRE(tools.is_array());
    CHECK(tools.size() > 20);
    CHECK_FALSE(response["result"].contains("nextCursor"));

    // Lifecycle first, then reads, then mutations, then the file utilities.
    CHECK(tools.front()["name"] == "create_document");
    CHECK(tools.back()["name"] == "export_media");

    const auto second = server->Send(list);
    CHECK(second["result"]["tools"] == tools);
}

TEST_CASE("an unknown tool is an invalid-params error, not a tool failure [mcp-protocol]")
{
    auto server = MakeWordServer();
    server->Initialize();

    nlohmann::json call = nlohmann::json::object();
    call["jsonrpc"] = "2.0";
    call["id"] = 4;
    call["method"] = "tools/call";
    call["params"] = nlohmann::json{{"name", "no_such_tool"}, {"arguments", nlohmann::json::object()}};

    const auto response = server->Send(call);
    REQUIRE(response.contains("error"));
    CHECK(response["error"]["code"] == -32602);
}

TEST_CASE("a tool failure is a successful response with isError [mcp-protocol]")
{
    auto server = MakeWordServer();
    server->Initialize();

    const auto result = server->CallRaw("open_document", nlohmann::json{{"path", "missing.docx"}});
    REQUIRE(result.is_object());
    CHECK(result["isError"] == true);
    CHECK(result["structuredContent"]["ok"] == false);
    CHECK(result["structuredContent"]["error"]["code"] == "file_not_found");
    CHECK_FALSE(result["structuredContent"]["error"]["hint"].get<std::string>().empty());

    // The text block mirrors the structured envelope for older clients.
    REQUIRE(result["content"].is_array());
    CHECK(result["content"][0]["type"] == "text");
}

TEST_CASE("input that violates the schema is reported with JSON pointers [mcp-protocol]")
{
    auto server = MakeWordServer();
    server->Initialize();

    const auto envelope = server->Call("open_document", nlohmann::json{{"path", 42}});
    CHECK(envelope["ok"] == false);
    CHECK(envelope["error"]["code"] == "input_invalid");
    REQUIRE(envelope["error"]["details"].is_array());
    CHECK_FALSE(envelope["error"]["details"].empty());
    CHECK(envelope["error"]["details"][0].contains("pointer"));
}

TEST_CASE("unknown arguments are rejected by additionalProperties [mcp-protocol]")
{
    auto server = MakeWordServer();
    server->Initialize();

    const auto envelope =
        server->Call("list_documents", nlohmann::json{{"unexpected", "value"}});
    CHECK(envelope["ok"] == false);
    CHECK(envelope["error"]["code"] == "input_invalid");
}

TEST_CASE("read-only mode publishes only read-only tools [mcp-protocol]")
{
    const auto checkServer = [](std::unique_ptr<McpTestServer> server)
    {
        server->Initialize();
        for (const auto& tool : server->Registry().Tools())
        {
            CAPTURE(tool.Definition.Name);
            CHECK(tool.Definition.Annotations.ReadOnly);
        }

        CHECK(server->Registry().Find("create_document") == nullptr);
        CHECK(server->Registry().Find("replace_text") == nullptr);
        CHECK(server->Registry().Find("get_document_info") != nullptr);
    };

    SUBCASE("Word")
    {
        auto server = MakeWordServer(true);
        CHECK(server->Registry().Find("insert_paragraph") == nullptr);
        checkServer(std::move(server));
    }
    SUBCASE("Excel")
    {
        auto server = MakeExcelServer(true);
        CHECK(server->Registry().Find("write_range") == nullptr);
        checkServer(std::move(server));
    }
    SUBCASE("PowerPoint")
    {
        auto server = MakePowerPointServer(true);
        CHECK(server->Registry().Find("add_slide") == nullptr);
        checkServer(std::move(server));
    }
}
