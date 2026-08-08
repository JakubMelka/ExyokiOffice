// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

#include "McpServer.hpp"
#include "ServerRunner.hpp"
#include "SessionStore.hpp"
#include "SharedToolset.hpp"
#include "ToolContext.hpp"
#include "ToolRegistry.hpp"
#include "Workspace.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace ExyokiOfficeTests
{

/**
 * @brief One MCP server wired up in process, over a temporary workspace.
 *
 * The tests drive the same McpServer the binaries drive, but through
 * HandleMessage rather than stdio: a conversation is a sequence of JSON lines
 * in and JSON values out, with no child process to start and no pipe to
 * synchronize. The workspace root is a unique temporary directory that the
 * fixture removes on destruction.
 */
class McpTestServer
{
public:
    McpTestServer(std::shared_ptr<ExyokiOffice::Mcp::FamilyAdapter> adapter,
                  std::vector<ExyokiOffice::Mcp::ToolsetRegistrar> toolsets, bool readOnly = false,
                  std::vector<std::string> toolsetFilter = {});
    McpTestServer(std::shared_ptr<ExyokiOffice::Mcp::FamilyAdapter> adapter,
                  std::vector<ExyokiOffice::Mcp::ToolsetRegistrar> toolsets,
                  ExyokiOffice::Mcp::ServerOptions options);
    ~McpTestServer();

    McpTestServer(const McpTestServer&) = delete;
    McpTestServer& operator=(const McpTestServer&) = delete;
    McpTestServer(McpTestServer&&) = delete;
    McpTestServer& operator=(McpTestServer&&) = delete;

    /// Sends `initialize` and `notifications/initialized`.
    void Initialize();

    /// Sends one raw JSON-RPC line and returns the parsed response.
    [[nodiscard]] nlohmann::json Send(const nlohmann::json& message);

    /// Calls one tool and returns the `structuredContent` envelope.
    [[nodiscard]] nlohmann::json Call(const std::string& tool, const nlohmann::json& arguments);

    /// Calls one tool and returns the whole `tools/call` result.
    [[nodiscard]] nlohmann::json CallRaw(const std::string& tool, const nlohmann::json& arguments);

    [[nodiscard]] const std::filesystem::path& Root() const noexcept { return m_root; }
    [[nodiscard]] const ExyokiOffice::Mcp::ToolRegistry& Registry() const noexcept { return *m_registry; }
    [[nodiscard]] ExyokiOffice::Mcp::McpServer& Server() const noexcept { return *m_server; }

    /// Absolute path of a workspace-relative name, for direct library checks.
    [[nodiscard]] std::filesystem::path Path(const std::string& relative) const { return m_root / relative; }

private:
    std::filesystem::path m_root;
    std::shared_ptr<ExyokiOffice::Mcp::FamilyAdapter> m_adapter;
    ExyokiOffice::Mcp::ServerOptions m_options;
    std::unique_ptr<ExyokiOffice::Mcp::Workspace> m_workspace;
    std::unique_ptr<ExyokiOffice::Mcp::SessionStore> m_sessions;
    std::unique_ptr<ExyokiOffice::Mcp::ToolContext> m_context;
    std::unique_ptr<ExyokiOffice::Mcp::ToolRegistry> m_registry;
    std::unique_ptr<ExyokiOffice::Mcp::McpServer> m_server;
    int m_nextId = 1;
};

/**
 * @brief Builds `initialize` params the server accepts.
 *
 * The handshake is validated member by member, so every test that opens a
 * connection by hand goes through here rather than assembling a partial
 * request that would now be refused.
 *
 * @param version Requested revision; empty means the newest supported one.
 */
[[nodiscard]] nlohmann::json MakeInitializeParams(const std::string& version = std::string());

/// Builds a Word test server with the shared and Word toolsets registered.
[[nodiscard]] std::unique_ptr<McpTestServer> MakeWordServer(bool readOnly = false);

/// Builds an Excel test server with the shared and Excel toolsets registered.
[[nodiscard]] std::unique_ptr<McpTestServer> MakeExcelServer(bool readOnly = false);

/// Builds a PowerPoint test server with the shared and PowerPoint toolsets registered.
[[nodiscard]] std::unique_ptr<McpTestServer> MakePowerPointServer(bool readOnly = false);

} // namespace ExyokiOfficeTests
