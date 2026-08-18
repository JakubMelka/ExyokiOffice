// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "McpTestSupport.hpp"

#include "ExcelToolset.hpp"
#include "PowerPointToolset.hpp"
#include "SchemaCheck.hpp"
#include "WordToolset.hpp"

#include "TestSupport.hpp"

#include "ExyokiOffice/Version.hpp"

#include <system_error>
#include <stdexcept>
#include <utility>

namespace ExyokiOfficeTests
{

using namespace ExyokiOffice::Mcp;

McpTestServer::McpTestServer(std::shared_ptr<FamilyAdapter> adapter, std::vector<ToolsetRegistrar> toolsets,
                             bool readOnly, std::vector<std::string> toolsetFilter)
    : McpTestServer(std::move(adapter), std::move(toolsets),
                    ServerOptions{.ReadOnly = readOnly, .Toolsets = std::move(toolsetFilter)})
{
}

McpTestServer::McpTestServer(std::shared_ptr<FamilyAdapter> adapter, std::vector<ToolsetRegistrar> toolsets,
                             ServerOptions options)
    : m_adapter(std::move(adapter))
{
    m_root = MakeTemporaryPath("mcp-workspace", "");

    std::error_code error;
    std::filesystem::create_directories(m_root, error);

    m_options = std::move(options);
    if (m_options.WorkspaceRoots.empty())
    {
        m_options.WorkspaceRoots.push_back(m_root);
    }

    // As RunMcpServer does before serving the first request: the adapter is
    // what actually opens documents, so a fixture that skipped this would test
    // a server configured differently from the real one.
    m_adapter->SetPackageLimits(m_options.PackageLimits);

    m_workspace = std::make_unique<Workspace>(m_options.WorkspaceRoots);
    m_sessions = std::make_unique<SessionStore>(m_options.MaxDocuments, m_options.SnapshotDepth);
    m_context = std::make_unique<ToolContext>(m_options, *m_workspace, *m_sessions, *m_adapter);

    m_registry = std::make_unique<ToolRegistry>(m_options.ReadOnly, m_options.Toolsets);
    RegisterSharedToolset(*m_registry, *m_adapter);
    for (const auto& registrar : toolsets)
    {
        registrar(*m_registry);
    }

    m_context->SetRegistry(m_registry.get());

    ServerInfo info;
    info.Name = "exyoki-mcp-test";
    info.Title = "ExyokiOffice MCP Test Server";
    info.Version = std::string(ExyokiOffice::GetVersion());
    info.Instructions = BuildInstructions(m_adapter->FamilyName(), m_adapter->FileExtension(), {});

    m_server = std::make_unique<McpServer>(std::move(info), *m_registry, *m_context);
}

McpTestServer::~McpTestServer()
{
    // The sessions hold open documents that may still hold the workspace files
    // open; they are released before the directory is removed.
    m_server.reset();
    m_registry.reset();
    m_context.reset();
    m_sessions.reset();

    std::error_code error;
    std::filesystem::remove_all(m_root, error);
}

void McpTestServer::Initialize()
{
    nlohmann::json initialize = nlohmann::json::object();
    initialize["jsonrpc"] = "2.0";
    initialize["id"] = m_nextId++;
    initialize["method"] = "initialize";
    initialize["params"] = MakeInitializeParams();
    static_cast<void>(Send(initialize));

    nlohmann::json initialized = nlohmann::json::object();
    initialized["jsonrpc"] = "2.0";
    initialized["method"] = "notifications/initialized";
    static_cast<void>(Send(initialized));
}

nlohmann::json McpTestServer::Send(const nlohmann::json& message)
{
    auto response = m_server->HandleMessage(message.dump());
    if (!response.has_value())
    {
        return nullptr;
    }

    return nlohmann::json::parse(*response, nullptr, false);
}

nlohmann::json McpTestServer::CallRaw(const std::string& tool, const nlohmann::json& arguments)
{
    nlohmann::json message = nlohmann::json::object();
    message["jsonrpc"] = "2.0";
    message["id"] = m_nextId++;
    message["method"] = "tools/call";
    message["params"] = nlohmann::json{{"name", tool}, {"arguments", arguments}};

    auto response = Send(message);
    if (!response.is_object() || !response.contains("result"))
    {
        return response;
    }

    auto result = response["result"];
    const auto* registered = m_registry->Find(tool);
    if (registered != nullptr && result.is_object() && result.contains("structuredContent"))
    {
        SchemaCheck outputCheck;
        std::string schemaError;
        if (!outputCheck.SetSchema(registered->Definition.OutputSchema, schemaError))
        {
            throw std::runtime_error("Invalid output schema of " + tool + ": " + schemaError);
        }

        std::vector<SchemaViolation> violations;
        if (!outputCheck.Validate(result["structuredContent"], violations))
        {
            const auto detail = violations.empty() ? std::string("unknown violation")
                                                   : violations.front().Pointer + ": " + violations.front().Message;
            throw std::runtime_error("Output of " + tool + " violates its schema: " + detail);
        }

        if (!result.contains("content") || !result["content"].is_array() || result["content"].empty() ||
            result["content"][0].value("type", std::string()) != "text")
        {
            throw std::runtime_error("Output of " + tool + " does not carry the structured envelope as text.");
        }

        const auto mirrored = nlohmann::json::parse(result["content"][0].value("text", std::string()), nullptr, false);
        if (mirrored != result["structuredContent"])
        {
            throw std::runtime_error("Text and structured output of " + tool + " differ.");
        }
    }

    return result;
}

nlohmann::json McpTestServer::Call(const std::string& tool, const nlohmann::json& arguments)
{
    auto result = CallRaw(tool, arguments);
    if (result.is_object() && result.contains("structuredContent"))
    {
        return result["structuredContent"];
    }

    return result;
}

nlohmann::json MakeInitializeParams(const std::string& version)
{
    nlohmann::json clientInfo = nlohmann::json::object();
    clientInfo["name"] = "exyoki-mcp-tests";
    clientInfo["version"] = std::string(ExyokiOffice::GetVersion());

    nlohmann::json params = nlohmann::json::object();
    params["protocolVersion"] = version.empty() ? std::string(McpServer::LatestProtocolVersion()) : version;
    params["capabilities"] = nlohmann::json::object();
    params["clientInfo"] = std::move(clientInfo);
    return params;
}

std::unique_ptr<McpTestServer> MakeWordServer(bool readOnly)
{
    return std::make_unique<McpTestServer>(std::make_shared<WordFamilyAdapter>(),
                                           std::vector<ToolsetRegistrar>{[](ToolRegistry& registry)
                                                                         { RegisterWordToolset(registry); }},
                                           readOnly);
}

std::unique_ptr<McpTestServer> MakeExcelServer(bool readOnly)
{
    return std::make_unique<McpTestServer>(std::make_shared<ExcelFamilyAdapter>(),
                                           std::vector<ToolsetRegistrar>{[](ToolRegistry& registry)
                                                                         { RegisterExcelToolset(registry); }},
                                           readOnly);
}

std::unique_ptr<McpTestServer> MakePowerPointServer(bool readOnly)
{
    return std::make_unique<McpTestServer>(std::make_shared<PowerPointFamilyAdapter>(),
                                           std::vector<ToolsetRegistrar>{[](ToolRegistry& registry)
                                                                         { RegisterPowerPointToolset(registry); }},
                                           readOnly);
}

std::string DescribeValidationErrors(const ExyokiOffice::Tools::ValidationReport& report)
{
    std::string text;
    for (const auto* issues : {&report.LoadIssues, &report.ValidationIssues})
    {
        for (const auto& issue : *issues)
        {
            if (issue.Severity != ExyokiOffice::ValidationSeverity::Error)
            {
                continue;
            }
            if (!text.empty())
            {
                text += " | ";
            }
            text += issue.Message + " in " + issue.PartUri + " at " + issue.Location.Path;
        }
    }
    return text;
}

} // namespace ExyokiOfficeTests
