// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <string_view>

namespace ExyokiOffice::Mcp
{

/**
 * @brief JSON-RPC 2.0 error codes the server emits.
 *
 * Only protocol-level failures travel as JSON-RPC errors. A tool that fails
 * for a document reason answers with a successful response whose result
 * carries `isError: true`; see Results.hpp.
 */
enum class JsonRpcErrorCode
{
    ParseError = -32700,
    InvalidRequest = -32600,
    MethodNotFound = -32601,
    InvalidParams = -32602,
    InternalError = -32603,
    /// MCP-specific: a `tools/*` call arrived before `initialize` completed.
    ServerNotInitialized = -32002
};

/** @brief One decoded JSON-RPC request or notification. */
struct JsonRpcRequest
{
    /// Request identifier; meaningful only when HasId is set.
    nlohmann::json Id = nullptr;
    std::string Method;
    nlohmann::json Params = nlohmann::json::object();
    /// False for a notification, which must never be answered.
    bool HasId = false;

    /// A message without an `id` expects no response.
    [[nodiscard]] bool IsNotification() const noexcept { return !HasId; }
};

/**
 * @brief Parses one JSON-RPC 2.0 message object into a request.
 *
 * Accepts a missing `params` member as an empty object and tolerates a missing
 * `jsonrpc` version string, because several clients omit it on notifications.
 *
 * An explicit null `id` is rejected rather than quietly demoted to a
 * notification: MCP forbids a null id, and answering nothing would leave the
 * client waiting for a response that never comes.
 *
 * Returns std::nullopt and fills @p error when the message is not a request.
 */
[[nodiscard]] std::optional<JsonRpcRequest> ParseJsonRpcRequest(const nlohmann::json& message, std::string& error);

/** @brief Builds a successful JSON-RPC response envelope. */
[[nodiscard]] nlohmann::json MakeJsonRpcResponse(const nlohmann::json& id, nlohmann::json result);

/** @brief Builds a JSON-RPC error response envelope. */
[[nodiscard]] nlohmann::json MakeJsonRpcError(const nlohmann::json& id, JsonRpcErrorCode code,
                                              std::string_view message, nlohmann::json data = nullptr);

} // namespace ExyokiOffice::Mcp
