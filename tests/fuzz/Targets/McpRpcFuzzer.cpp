// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "FuzzHarness.hpp"
#include "FuzzTargets.hpp"

#if defined(EXYOKIOFFICE_FUZZ_HAS_MCP)

#include "McpServer.hpp"
#include "SessionStore.hpp"
#include "SharedToolset.hpp"
#include "ToolContext.hpp"
#include "ToolRegistry.hpp"
#include "WordToolset.hpp"
#include "Workspace.hpp"

#include "ExyokiOffice/StandardTypes.hpp"
#include "ExyokiOffice/Version.hpp"

#include <exception>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace ExyokiOffice::Fuzz
{

/// File-local helpers that keep one MCP server available to the entry point.
class McpRpcFuzzHelpers
{
public:
    /// The pieces one connection owns; the server points into the other two.
    struct Connection
    {
        std::unique_ptr<Mcp::SessionStore> Sessions;
        std::unique_ptr<Mcp::ToolContext> Context;
        std::unique_ptr<Mcp::McpServer> Server;
    };

    /**
     * @brief Builds one connection over the shared, already-registered catalog.
     *
     * The catalog costs far more to build than everything else here — fifty
     * tools with their schemas — so it is built once, while the session store
     * and the server are new for every input. That is what keeps one input from
     * inheriting sessions, an open document, or a negotiated protocol version
     * from the one before it: a crash artifact has to reproduce on its own.
     */
    static Connection MakeConnection()
    {
        Connection connection;
        connection.Sessions =
            std::make_unique<Mcp::SessionStore>(SharedOptions().MaxDocuments, SharedOptions().SnapshotDepth);
        connection.Context = std::make_unique<Mcp::ToolContext>(SharedOptions(), SharedWorkspace(),
                                                                *connection.Sessions, SharedAdapter());
        connection.Context->SetRegistry(&SharedRegistry());

        Mcp::ServerInfo info;
        info.Name = "exyoki-mcp-fuzz";
        info.Title = "ExyokiOffice MCP Fuzz Target";
        info.Version = std::string(GetVersion());
        info.Instructions = Mcp::BuildInstructions(SharedAdapter().FamilyName(), SharedAdapter().FileExtension(), {});

        connection.Server = std::make_unique<Mcp::McpServer>(std::move(info), SharedRegistry(), *connection.Context);
        return connection;
    }

    /// A conformant handshake, so mutants can reach the tools at all.
    static void Handshake(Mcp::McpServer& server)
    {
        static const std::string initialize =
            R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-11-25",)"
            R"("capabilities":{},"clientInfo":{"name":"fuzz","version":"1"}}})";
        static const std::string initialized = R"({"jsonrpc":"2.0","method":"notifications/initialized"})";

        static_cast<void>(server.HandleMessage(initialize));
        static_cast<void>(server.HandleMessage(initialized));
    }

    /**
     * @brief Checks everything a response line must satisfy, whatever went in.
     *
     * The transport frames one message per line, so a response that is not a
     * single line of valid JSON desynchronizes the client's parser for the rest
     * of the connection — a failure no client can recover from and no schema
     * catches.
     */
    static void CheckResponse(const std::string& response)
    {
        EXYOKIOFFICE_FUZZ_CHECK(response.find('\n') == std::string::npos,
                                "A response line carries an embedded newline");

        const auto parsed = nlohmann::json::parse(response, nullptr, false);
        EXYOKIOFFICE_FUZZ_CHECK(!parsed.is_discarded(), "A response line is not valid JSON");
        EXYOKIOFFICE_FUZZ_CHECK(parsed.is_object(), "A response is not a JSON object");
        EXYOKIOFFICE_FUZZ_CHECK(parsed.value("jsonrpc", std::string()) == "2.0",
                                "A response does not carry jsonrpc 2.0");
        EXYOKIOFFICE_FUZZ_CHECK(parsed.contains("result") != parsed.contains("error"),
                                "A response carries both result and error, or neither");
        EXYOKIOFFICE_FUZZ_CHECK(parsed.contains("id"), "A response carries no id");
    }

private:
    static const Mcp::ServerOptions& SharedOptions()
    {
        static const Mcp::ServerOptions options = MakeOptions();
        return options;
    }

    static Mcp::ServerOptions MakeOptions()
    {
        Mcp::ServerOptions options;
        options.WorkspaceRoots = {WorkspacePath()};
        options.Level = Mcp::LogLevel::Error;
        return options;
    }

    /**
     * @brief Workspace root that deliberately does not exist.
     *
     * Handlers run in full — arguments are validated, documents are created and
     * edited in memory, `batch` recurses — and only a write reaches the file
     * system, where it fails because the directory is not there. Letting inputs
     * write would make each one depend on what earlier inputs left behind, and
     * a crash artifact that only reproduces after its predecessors is not a
     * regression test.
     */
    static const std::filesystem::path& WorkspacePath()
    {
        static const std::filesystem::path path =
            std::filesystem::temp_directory_path() / "exyoki-mcp-fuzz-workspace-absent";
        return path;
    }

    static Mcp::Workspace& SharedWorkspace()
    {
        static Mcp::Workspace workspace(std::vector<std::filesystem::path>{WorkspacePath()});
        return workspace;
    }

    static Mcp::FamilyAdapter& SharedAdapter()
    {
        static Mcp::WordFamilyAdapter adapter;
        return adapter;
    }

    static const Mcp::ToolRegistry& SharedRegistry()
    {
        static const Mcp::ToolRegistry& registry = MakeRegistry();
        return registry;
    }

    static Mcp::ToolRegistry& MakeRegistry()
    {
        static Mcp::ToolRegistry registry(false, {});
        Mcp::RegisterSharedToolset(registry, SharedAdapter());
        Mcp::RegisterWordToolset(registry);
        return registry;
    }
};

int RunMcpRpc(const UInt8* data, Size size)
{
    if (data == nullptr || size > kMaxInputSize)
    {
        return 0;
    }

    // One byte of selector, then the conversation. Without the handshake option
    // almost every mutant would stop at -32002 and the run would never reach a
    // tool: the initialize request now needs three members the fuzzer cannot
    // stumble into.
    ByteTape tape(data, size);
    const bool handshake = (tape.NextByte() & 1u) != 0;
    const auto conversation = tape.RestAsText();

    auto connection = McpRpcFuzzHelpers::MakeConnection();
    if (handshake)
    {
        McpRpcFuzzHelpers::Handshake(*connection.Server);
    }

    // A cap on lines, not on bytes: one huge line is a valid case, thousands of
    // tiny ones only slow the run down.
    constexpr Size MaximumLines = 64;
    Size lines = 0;
    Size start = 0;
    while (start <= conversation.size() && lines < MaximumLines)
    {
        const auto end = conversation.find('\n', start);
        const std::string line(
            conversation.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start));
        ++lines;

        try
        {
            if (auto response = connection.Server->HandleMessage(line))
            {
                McpRpcFuzzHelpers::CheckResponse(*response);
            }
        }
        catch (const std::exception& failure)
        {
            EXYOKIOFFICE_FUZZ_CHECK(false, failure.what());
        }
        catch (...)
        {
            EXYOKIOFFICE_FUZZ_CHECK(false, "An unknown exception escaped HandleMessage");
        }

        if (end == std::string_view::npos)
        {
            break;
        }

        start = end + 1;
    }

    return 0;
}

} // namespace ExyokiOffice::Fuzz

#endif // EXYOKIOFFICE_FUZZ_HAS_MCP
