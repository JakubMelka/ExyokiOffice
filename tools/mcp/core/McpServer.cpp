// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "McpServer.hpp"

#include "ToolContext.hpp"

#include <algorithm>
#include <string_view>
#include <utility>

namespace ExyokiOffice::Mcp
{

/// Wraps a message in the optional a request handler answers with.
///
/// `nlohmann::json` carries a greedy templated conversion operator, so letting
/// a json convert implicitly to `std::optional<nlohmann::json>` leaves GCC's
/// -Wconversion reporting which of the two candidates it picked. Constructing
/// the optional in place converts nothing and reads the same.
static std::optional<nlohmann::json> Answer(nlohmann::json message)
{
    return std::optional<nlohmann::json>(std::in_place, std::move(message));
}

/// True when @p line nests arrays or objects deeper than @p limit.
///
/// Deliberately a scan of the text rather than a check on the parsed value:
/// once the value exists the damage is done, because freeing it is what
/// recurses. Brackets inside a string literal are text, so the scan has to know
/// where strings start and end - the one piece of JSON syntax it cannot ignore.
/// Everything else, valid or not, is left to the parser to reject.
static bool NestsTooDeeply(std::string_view line, Size limit)
{
    Size depth = 0;
    bool inString = false;
    bool escaped = false;
    for (const char character : line)
    {
        if (inString)
        {
            if (escaped)
            {
                escaped = false;
            }
            else if (character == '\\')
            {
                escaped = true;
            }
            else if (character == '"')
            {
                inString = false;
            }
            continue;
        }

        switch (character)
        {
            case '"':
                inString = true;
                break;
            case '[':
            case '{':
                if (++depth > limit)
                {
                    return true;
                }
                break;
            case ']':
            case '}':
                if (depth > 0)
                {
                    --depth;
                }
                break;
            default:
                break;
        }
    }
    return false;
}

McpServer::McpServer(ServerInfo info, const ToolRegistry& registry, ToolContext& context)
    : m_info(std::move(info)), m_registry(&registry), m_context(&context)
{
}

std::string_view McpServer::LatestProtocolVersion() noexcept
{
    return "2025-11-25";
}

const std::vector<std::string>& McpServer::SupportedProtocolVersions()
{
    // 2025-03-26 is deliberately absent: it is the one revision that requires
    // servers to accept JSON-RPC batches, which this server does not implement
    // (batching was removed again in 2025-06-18). Offering it would be a
    // promise the transport cannot keep.
    static const std::vector<std::string> versions{"2024-11-05", "2025-06-18", "2025-11-25"};
    return versions;
}

const std::vector<std::string>& McpServer::RequiredInitializeMembers()
{
    static const std::vector<std::string> members{"protocolVersion", "capabilities", "clientInfo"};
    return members;
}

std::optional<std::string> McpServer::HandleMessage(const std::string& line)
{
    if (line.find_first_not_of(" \t\r\n") == std::string::npos)
    {
        return std::nullopt;
    }

    if (NestsTooDeeply(line, MaximumMessageDepth))
    {
        // Refused before the parser builds the tree, because the tree is what
        // costs: freeing a value nested a million deep recurses a million times
        // and takes the process with it. A parse error is also the honest
        // answer - this is a message the server will not read, and there is no
        // id to answer under until it has been read.
        Log::Warn("Discarding a JSON-RPC line nested past the depth limit.");
        return SerializeJson(MakeJsonRpcError(nullptr, JsonRpcErrorCode::ParseError,
                                              "The message nests arrays or objects too deeply."));
    }

    const auto message = nlohmann::json::parse(line, nullptr, false);
    if (message.is_discarded())
    {
        // Without a parsable message there is no id to answer under; the spec
        // allows a null id for a parse error, which is what a client expects.
        Log::Warn("Discarding an unparsable JSON-RPC line.");
        return SerializeJson(
            MakeJsonRpcError(nullptr, JsonRpcErrorCode::ParseError, "The message is not valid JSON."));
    }

    std::string error;
    const auto request = ParseJsonRpcRequest(message, error);
    if (!request.has_value())
    {
        Log::Warn("Rejecting an invalid JSON-RPC message: " + error);
        if (message.is_object() && !message.contains("id"))
        {
            // A message without an id is a notification even when it is
            // malformed, and a notification is never answered.
            return std::nullopt;
        }

        const auto id = message.is_object() && message.contains("id") ? message["id"] : nlohmann::json(nullptr);
        return SerializeJson(MakeJsonRpcError(id, JsonRpcErrorCode::InvalidRequest, error));
    }

    std::optional<nlohmann::json> response;
    try
    {
        response = HandleRequest(*request);
    }
    catch (const std::exception& failure)
    {
        // Dispatch itself failing is a defect rather than a client error, but
        // taking the process down would strand every open session.
        Log::Error("Handling '" + request->Method + "' threw: " + failure.what());
        if (request->IsNotification())
        {
            return std::nullopt;
        }

        return SerializeJson(MakeJsonRpcError(request->Id, JsonRpcErrorCode::InternalError,
                                              "The server failed to handle the request."));
    }

    if (!response.has_value())
    {
        return std::nullopt;
    }

    return SerializeJson(*response);
}

std::optional<nlohmann::json> McpServer::HandleRequest(const JsonRpcRequest& request)
{
    Log::Debug("Handling method '" + request.Method + "'.");

    if (request.IsNotification())
    {
        HandleNotification(request);
        return std::nullopt;
    }

    if (request.Method == "initialize")
    {
        return Answer(HandleInitialize(request.Id, request.Params));
    }

    if (request.Method == "ping")
    {
        // Answered at any time, including before initialization: the spec makes
        // ping the one request a client may always issue.
        return Answer(MakeJsonRpcResponse(request.Id, nlohmann::json::object()));
    }

    const bool isToolMethod = request.Method.rfind("tools/", 0) == 0;
    if (isToolMethod && !m_initialized)
    {
        // Gated on the initialize exchange rather than on the `initialized`
        // notification: a tool call before initialize has no negotiated
        // protocol version to honour, while the window between the initialize
        // response and the notification is one the specification leaves open
        // to clients.
        return Answer(MakeJsonRpcError(request.Id, JsonRpcErrorCode::ServerNotInitialized,
                                       "The server has not been initialized; call 'initialize' first."));
    }

    if (request.Method == "tools/list")
    {
        nlohmann::json result = nlohmann::json::object();
        result["tools"] = m_registry->ListJson();
        // The catalog fits into a single page; no cursor is ever handed back,
        // and a cursor supplied by the client is accepted and ignored.
        return Answer(MakeJsonRpcResponse(request.Id, std::move(result)));
    }

    if (request.Method == "tools/call")
    {
        return Answer(HandleToolsCall(request.Id, request.Params));
    }

    return Answer(MakeJsonRpcError(request.Id, JsonRpcErrorCode::MethodNotFound,
                                   "Unknown method '" + request.Method + "'."));
}

void McpServer::HandleNotification(const JsonRpcRequest& request)
{
    if (request.Method == "notifications/initialized")
    {
        if (!m_initialized)
        {
            // The notification only confirms a completed initialize exchange. On
            // its own it agrees on no protocol version and carries no client
            // capabilities, so treating it as initialization would let a client
            // reach the tools without ever having handshaken.
            Log::Warn("Ignoring 'notifications/initialized' sent before a successful 'initialize'.");
            return;
        }

        if (m_initializationConfirmed)
        {
            Log::Info("Ignoring a repeated 'notifications/initialized'.");
            return;
        }

        m_initializationConfirmed = true;
        Log::Debug("The client confirmed initialization.");
        return;
    }

    if (request.Method == "notifications/cancelled")
    {
        // Requests are served one at a time and to completion, so by the time a
        // cancellation arrives its target has already been answered.
        Log::Info("Ignoring a cancellation notification; requests are handled synchronously.");
        return;
    }

    Log::Info("Ignoring an unknown notification '" + request.Method + "'.");
}

nlohmann::json McpServer::HandleInitialize(const nlohmann::json& id, const nlohmann::json& params)
{
    // Everything after this exchange depends on its outcome: the agreed
    // revision, what the client can do, and who the client is. A request that
    // omits any of it is refused rather than guessed at, because a guessed
    // handshake leaves a session neither side has actually agreed on.
    if (m_initialized)
    {
        // Initialization is the first interaction and happens once. Silently
        // renegotiating would replace the terms the already open sessions run
        // under.
        Log::Warn("Rejecting a repeated 'initialize' request.");
        return MakeJsonRpcError(id, JsonRpcErrorCode::InvalidRequest,
                                "The connection is already initialized; 'initialize' is sent once per connection.",
                                nlohmann::json{{"protocolVersion", m_negotiatedVersion}});
    }

    const auto reject = [&id](std::string_view message, nlohmann::json data)
    {
        Log::Warn("Rejecting a non-conformant 'initialize': " + std::string(message));
        return MakeJsonRpcError(id, JsonRpcErrorCode::InvalidParams, message, std::move(data));
    };

    const nlohmann::json required = RequiredInitializeMembers();
    if (!params.is_object())
    {
        return reject("The 'initialize' parameters must be an object.", nlohmann::json{{"required", required}});
    }

    const auto version = params.find("protocolVersion");
    if (version == params.end() || !version->is_string() || version->get<std::string>().empty())
    {
        return reject("The 'initialize' parameters must carry a non-empty string 'protocolVersion'.",
                      nlohmann::json{{"required", required}, {"supported", SupportedProtocolVersions()}});
    }

    const auto clientCapabilities = params.find("capabilities");
    if (clientCapabilities == params.end() || !clientCapabilities->is_object())
    {
        return reject("The 'initialize' parameters must carry an object 'capabilities'; send an empty object when "
                      "the client declares none.",
                      nlohmann::json{{"required", required}});
    }

    const auto clientInfo = params.find("clientInfo");
    if (clientInfo == params.end() || !clientInfo->is_object())
    {
        return reject("The 'initialize' parameters must carry an object 'clientInfo'.",
                      nlohmann::json{{"required", required}});
    }

    const auto clientName = clientInfo->find("name");
    const auto clientVersion = clientInfo->find("version");
    if (clientName == clientInfo->end() || !clientName->is_string() || clientName->get<std::string>().empty() ||
        clientVersion == clientInfo->end() || !clientVersion->is_string())
    {
        return reject("The 'clientInfo' member must carry a non-empty string 'name' and a string 'version'.",
                      nlohmann::json{{"required", nlohmann::json::array({"clientInfo.name", "clientInfo.version"})}});
    }

    const auto requested = version->get<std::string>();
    const auto& supported = SupportedProtocolVersions();
    const bool known = std::find(supported.begin(), supported.end(), requested) != supported.end();
    if (!known)
    {
        // Answering with a revision the server does support is what the
        // specification asks for here; refusing the connection is the client's
        // decision to make once it sees the answer.
        Log::Info("Client requested unsupported protocol version '" + requested + "'; offering " +
                  std::string(LatestProtocolVersion()) + ".");
    }

    m_negotiatedVersion = known ? requested : std::string(LatestProtocolVersion());
    m_initialized = true;
    Log::Info("Initialized for '" + clientName->get<std::string>() + " " + clientVersion->get<std::string>() +
              "' on protocol " + m_negotiatedVersion + ".");

    nlohmann::json tools = nlohmann::json::object();
    tools["listChanged"] = false;

    nlohmann::json capabilities = nlohmann::json::object();
    capabilities["tools"] = std::move(tools);

    nlohmann::json serverInfo = nlohmann::json::object();
    serverInfo["name"] = m_info.Name;
    serverInfo["title"] = m_info.Title;
    serverInfo["version"] = m_info.Version;

    nlohmann::json result = nlohmann::json::object();
    result["protocolVersion"] = m_negotiatedVersion;
    result["capabilities"] = std::move(capabilities);
    result["serverInfo"] = std::move(serverInfo);
    result["instructions"] = m_info.Instructions;
    return MakeJsonRpcResponse(id, std::move(result));
}

nlohmann::json McpServer::HandleToolsCall(const nlohmann::json& id, const nlohmann::json& params)
{
    if (!params.is_object())
    {
        return MakeJsonRpcError(id, JsonRpcErrorCode::InvalidParams,
                                "The 'tools/call' parameters must be an object.");
    }

    const auto name = params.find("name");
    if (name == params.end() || !name->is_string())
    {
        return MakeJsonRpcError(id, JsonRpcErrorCode::InvalidParams,
                                "The 'tools/call' parameters must carry a string 'name'.");
    }

    const auto toolName = name->get<std::string>();
    const auto* tool = m_registry->Find(toolName);
    if (tool == nullptr)
    {
        return MakeJsonRpcError(id, JsonRpcErrorCode::InvalidParams, "Unknown tool '" + toolName + "'.");
    }

    nlohmann::json arguments = nlohmann::json::object();
    const auto suppliedArguments = params.find("arguments");
    if (suppliedArguments != params.end() && !suppliedArguments->is_null())
    {
        if (!suppliedArguments->is_object())
        {
            // A malformed CallToolRequest is a protocol error; only arguments
            // that fail the tool's own input schema come back as isError.
            return MakeJsonRpcError(id, JsonRpcErrorCode::InvalidParams,
                                    "The 'arguments' member of 'tools/call' must be an object.");
        }

        arguments = *suppliedArguments;
    }

    auto outcome = m_registry->Call(*m_context, *tool, arguments);

    nlohmann::json content = nlohmann::json::array();
    nlohmann::json textBlock = nlohmann::json::object();
    textBlock["type"] = "text";
    textBlock["text"] = SerializeJson(outcome.Structured, 2);
    content.push_back(std::move(textBlock));

    if (outcome.ExtraContent.is_array())
    {
        for (auto& block : outcome.ExtraContent)
        {
            content.push_back(std::move(block));
        }
    }

    nlohmann::json result = nlohmann::json::object();
    result["content"] = std::move(content);
    result["structuredContent"] = std::move(outcome.Structured);
    result["isError"] = outcome.IsError;
    return MakeJsonRpcResponse(id, std::move(result));
}

std::string BuildInstructions(std::string_view familyName, std::string_view extension,
                              const std::vector<std::string>& addressingLines)
{
    std::string text;
    text.append("This server edits ").append(familyName).append(" documents (").append(extension);
    text.append(") through the ExyokiOffice library.\n\n");
    text.append("Workflow: call create_document or open_document to obtain a documentId, apply editing tools to "
                "that id, then call save_document. Sessions keep the document in memory between calls, so nothing "
                "reaches disk until you save. Call close_document when you are done.\n");
    text.append("Reading tools accept either documentId or path (exactly one); mutating tools require "
                "documentId. Path-based utilities work file to file and open no session.\n");

    for (const auto& line : addressingLines)
    {
        text.append(line).append("\n");
    }

    text.append("Lengths are a number in points or a string with a unit, such as 12, \"2.5cm\", \"1in\", or "
                "\"96px\". Colors are \"#RRGGBB\".\n");
    text.append("Every result is the same envelope: ok, summary, data, warnings, and revision/dirty for session "
                "tools. A failure sets isError and carries error.code plus a hint naming the tool that resolves "
                "it.\n");
    text.append("Indices shift after structural edits, so read them back from the mutation result or re-read the "
                "document before addressing content again.\n");
    text.append("Verify your work cheaply with get_document_markdown for content and validate_document for "
                "package correctness before you save the final result.\n");
    text.append("All paths must stay inside the configured workspace; absolute paths outside it are rejected.\n");
    return text;
}

} // namespace ExyokiOffice::Mcp
