// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

#include "JsonRpc.hpp"
#include "ToolRegistry.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ExyokiOffice::Mcp
{

class ToolContext;

/** @brief Identity and guidance one server publishes at `initialize`. */
struct ServerInfo
{
    /// Program name, for example `exyoki-mcp-word`.
    std::string Name;
    /// Human-readable name, for example "ExyokiOffice Word MCP Server".
    std::string Title;
    /// Library version; never hard-coded, always ExyokiOffice::GetVersion().
    std::string Version;
    /// Free-form guidance handed to the model at connection time.
    std::string Instructions;
};

/**
 * @brief The MCP endpoint: one JSON-RPC message in, one response out.
 *
 * The server is single-threaded and handles requests strictly in arrival
 * order. That matches the concurrency contract in docs/Threading.md — one
 * document graph belongs to one worker — and it is what makes session state
 * consistent without a lock anywhere in the tool layer.
 *
 * Message handling is separated from the transport so the test suite can drive
 * a whole conversation in-process, without pipes or child processes.
 */
class McpServer
{
public:
    McpServer(ServerInfo info, const ToolRegistry& registry, ToolContext& context);

    /**
     * @brief Handles one JSON-RPC message line.
     *
     * @return The response line to write back, or std::nullopt for a
     *         notification and for input that cannot be answered at all.
     */
    [[nodiscard]] std::optional<std::string> HandleMessage(const std::string& line);

    /// True once an `initialize` request has been answered successfully.
    [[nodiscard]] bool IsInitialized() const noexcept { return m_initialized; }

    /// True once the client confirmed the handshake with `notifications/initialized`.
    [[nodiscard]] bool IsInitializationConfirmed() const noexcept { return m_initializationConfirmed; }

    /// Revision agreed at `initialize`; empty before a successful handshake.
    [[nodiscard]] const std::string& NegotiatedProtocolVersion() const noexcept { return m_negotiatedVersion; }

    /// Highest MCP protocol revision this implementation targets.
    [[nodiscard]] static std::string_view LatestProtocolVersion() noexcept;

    /// Protocol revisions the server echoes back unchanged.
    [[nodiscard]] static const std::vector<std::string>& SupportedProtocolVersions();

    /// Members `initialize` params must carry, in the order they are checked.
    [[nodiscard]] static const std::vector<std::string>& RequiredInitializeMembers();

private:
    [[nodiscard]] nlohmann::json HandleInitialize(const nlohmann::json& id, const nlohmann::json& params);
    [[nodiscard]] std::optional<nlohmann::json> HandleRequest(const JsonRpcRequest& request);
    [[nodiscard]] nlohmann::json HandleToolsCall(const nlohmann::json& id, const nlohmann::json& params);
    void HandleNotification(const JsonRpcRequest& request);

    ServerInfo m_info;
    const ToolRegistry* m_registry = nullptr;
    ToolContext* m_context = nullptr;
    /// Set by a successful `initialize` exchange only; see HandleNotification.
    bool m_initialized = false;
    bool m_initializationConfirmed = false;
    std::string m_negotiatedVersion;
};

/**
 * @brief Builds the `instructions` text sent at `initialize`.
 *
 * The workflow, the units, and the document-identification rules are identical
 * across the three servers, so the template lives here and each binary adds
 * only its family's addressing sentence.
 *
 * @param familyName Family label, for example "Word".
 * @param extension Default document extension including the dot.
 * @param addressingLines Family-specific addressing guidance, one line each.
 */
[[nodiscard]] std::string BuildInstructions(std::string_view familyName, std::string_view extension,
                                            const std::vector<std::string>& addressingLines);

} // namespace ExyokiOffice::Mcp
