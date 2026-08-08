// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "JsonRpc.hpp"

namespace ExyokiOffice::Mcp
{

std::optional<JsonRpcRequest> ParseJsonRpcRequest(const nlohmann::json& message, std::string& error)
{
    if (message.is_array())
    {
        // JSON-RPC batching was removed from MCP in revision 2025-06-18 and no
        // revision this server offers permits it.
        error = "JSON-RPC batches are not supported; send one message per line.";
        return std::nullopt;
    }

    if (!message.is_object())
    {
        error = "A JSON-RPC message must be a JSON object.";
        return std::nullopt;
    }

    const auto version = message.find("jsonrpc");
    if (version != message.end() && (!version->is_string() || version->get<std::string>() != "2.0"))
    {
        error = "The 'jsonrpc' member must be the string \"2.0\".";
        return std::nullopt;
    }

    const auto method = message.find("method");
    if (method == message.end() || !method->is_string())
    {
        error = "A JSON-RPC message must carry a string 'method' member.";
        return std::nullopt;
    }

    JsonRpcRequest request;
    request.Method = method->get<std::string>();

    const auto id = message.find("id");
    if (id != message.end())
    {
        if (id->is_null())
        {
            error = "A JSON-RPC 'id' must not be null; omit the member to send a notification.";
            return std::nullopt;
        }

        if (!id->is_string() && !id->is_number())
        {
            error = "A JSON-RPC 'id' must be a string or a number.";
            return std::nullopt;
        }

        request.Id = *id;
        request.HasId = true;
    }

    const auto params = message.find("params");
    if (params != message.end() && !params->is_null())
    {
        if (!params->is_object())
        {
            // Every MCP method passes its arguments by name; positional
            // parameters have no meaning anywhere in the protocol.
            error = "A JSON-RPC 'params' member must be an object.";
            return std::nullopt;
        }

        request.Params = *params;
    }

    return request;
}

nlohmann::json MakeJsonRpcResponse(const nlohmann::json& id, nlohmann::json result)
{
    nlohmann::json response = nlohmann::json::object();
    response["jsonrpc"] = "2.0";
    response["id"] = id;
    response["result"] = std::move(result);
    return response;
}

nlohmann::json MakeJsonRpcError(const nlohmann::json& id, JsonRpcErrorCode code, std::string_view message,
                                nlohmann::json data)
{
    nlohmann::json error = nlohmann::json::object();
    error["code"] = static_cast<int>(code);
    error["message"] = std::string(message);
    if (!data.is_null())
    {
        error["data"] = std::move(data);
    }

    nlohmann::json response = nlohmann::json::object();
    response["jsonrpc"] = "2.0";
    response["id"] = id;
    response["error"] = std::move(error);
    return response;
}

} // namespace ExyokiOffice::Mcp
