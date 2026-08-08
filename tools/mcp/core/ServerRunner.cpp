// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "ServerRunner.hpp"

#include "McpServer.hpp"
#include "SessionStore.hpp"
#include "SharedToolset.hpp"
#include "StdioTransport.hpp"
#include "ToolContext.hpp"
#include "Workspace.hpp"

#include "ExyokiOffice/Version.hpp"

#include <CLI11.hpp>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <system_error>

namespace ExyokiOffice::Mcp
{

/// File-local helpers for the shared start-up path.
class ServerRunnerSupport
{
public:
    static LogLevel ParseLogLevel(const std::string& text)
    {
        if (text == "error")
        {
            return LogLevel::Error;
        }

        if (text == "info")
        {
            return LogLevel::Info;
        }

        if (text == "debug")
        {
            return LogLevel::Debug;
        }

        return LogLevel::Warn;
    }

    /**
     * @brief Resolves `--package-limits` to the limits every open is given.
     *
     * `recommended` is the default because this server opens whatever a client
     * names, and a client's documents come from wherever the agent driving it
     * has been. `unlimited` restores the library's own default and exists for
     * the case the limits are what stands in the way — forensics on a damaged
     * package, a legitimately enormous workbook — where the operator is the one
     * who gets to decide.
     */
    static bool ParsePackageLimits(const std::string& text, OpenXmlPackageLimits& limits)
    {
        if (text == "recommended")
        {
            limits = OpenXmlPackageLimits::Recommended();
            return true;
        }

        if (text == "unlimited")
        {
            limits = OpenXmlPackageLimits::Unlimited();
            return true;
        }

        return false;
    }
};

int RunMcpServer(const ServerDescriptor& descriptor, int argc, char** argv)
{
    CLI::App app{descriptor.Title, descriptor.Name};
    // Kept consistent with exyoki: a leading '/' is a path here, never an option.
    app.allow_windows_style_options(false);

    std::vector<std::string> workspaceRoots;
    bool readOnly = false;
    std::vector<std::string> toolsets;
    Size maxDocuments = 16;
    Size snapshotDepth = 8;
    Size maxMediaSize = 8;
    std::string logLevel = "warn";
    std::string packageLimits = "recommended";
    bool printTools = false;
    std::string replayPath;

    app.add_option("--workspace", workspaceRoots,
                   "Root of the permitted file space; repeat for several roots (default: working directory)")
        ->envname("EXYOKI_MCP_WORKSPACE");
    app.add_flag("--read-only", readOnly, "Register only tools that never modify a document or the file system")
        ->envname("EXYOKI_MCP_READ_ONLY");
    app.add_option("--toolsets", toolsets, "Comma-separated tool groups to register (default: all groups)")
        ->delimiter(',')
        ->envname("EXYOKI_MCP_TOOLSETS");
    app.add_option("--max-documents", maxDocuments, "Maximum number of simultaneously open documents")
        ->envname("EXYOKI_MCP_MAX_DOCUMENTS");
    app.add_option("--snapshot-depth", snapshotDepth,
                   "Undo snapshots kept per document; 0 disables undo and leaves a failed edit unrolled back")
        ->envname("EXYOKI_MCP_SNAPSHOT_DEPTH");
    app.add_option("--max-media-size", maxMediaSize, "Maximum base64 media payload in MiB")
        ->envname("EXYOKI_MCP_MAX_MEDIA_SIZE");
    app.add_option("--log-level", logLevel, "Verbosity of the stderr log")
        ->check(CLI::IsMember({"error", "warn", "info", "debug"}))
        ->envname("EXYOKI_MCP_LOG_LEVEL");
    app.add_option("--package-limits", packageLimits,
                   "ZIP/XML safety limits applied to every document opened: 'recommended' bounds entry counts, "
                   "sizes, compression ratio and XML nesting; 'unlimited' switches the guard off")
        ->check(CLI::IsMember({"recommended", "unlimited"}))
        ->envname("EXYOKI_MCP_PACKAGE_LIMITS");
    app.add_flag("--print-tools", printTools, "Print the tool catalog as JSON and exit");
    app.add_option("--replay", replayPath, "Read requests from a JSON Lines file instead of standard input");
    app.set_version_flag("--version", std::string(descriptor.Name) + " " + std::string(ExyokiOffice::GetVersion()));

    try
    {
        app.parse(argc, argv);
    }
    catch (const CLI::ParseError& error)
    {
        return app.exit(error);
    }

    ServerOptions options;
    options.ReadOnly = readOnly;
    options.Toolsets = toolsets;
    options.MaxDocuments = maxDocuments;
    options.SnapshotDepth = snapshotDepth;
    options.MaxMediaSizeMiB = maxMediaSize;
    options.Level = ServerRunnerSupport::ParseLogLevel(logLevel);
    for (const auto& root : workspaceRoots)
    {
        options.WorkspaceRoots.emplace_back(root);
    }

    Log::SetLevel(options.Level);

    if (!ServerRunnerSupport::ParsePackageLimits(packageLimits, options.PackageLimits))
    {
        Log::Error("Unknown --package-limits value '" + packageLimits + "'.");
        return 1;
    }

    // Two mechanisms for one rule, because there are two kinds of open. The
    // adapter carries the limits into every document the session tools load,
    // where the settings are visible at the call. The process-wide default
    // covers the rest: `validate_document`, `diff_documents`, `get_stats` and
    // the conversion tools reach the loader through `ExyokiOffice::Tools` entry
    // points that construct their own packages and take no settings, and those
    // read the very same untrusted files.
    OpenXmlPackage::SetDefaultPackageLimits(options.PackageLimits);
    if (packageLimits == "unlimited")
    {
        Log::Warn("Package safety limits are switched off; a decompression bomb or deeply nested XML in an opened "
                  "document can exhaust memory or the stack.");
    }

    if (descriptor.Adapter == nullptr)
    {
        Log::Error("The server descriptor carries no family adapter.");
        return 1;
    }

    descriptor.Adapter->SetPackageLimits(options.PackageLimits);

    Workspace workspace(options.WorkspaceRoots);
    if (workspace.Roots().empty())
    {
        Log::Error("No usable workspace root; pass --workspace with an existing directory.");
        return 1;
    }

    SessionStore sessions(options.MaxDocuments, options.SnapshotDepth);

    ToolContext context(options, workspace, sessions, *descriptor.Adapter);

    ToolRegistry registry(options.ReadOnly, options.Toolsets);
    RegisterSharedToolset(registry, *descriptor.Adapter);
    for (const auto& registrar : descriptor.Toolsets)
    {
        registrar(registry);
    }

    context.SetRegistry(&registry);

    if (printTools)
    {
        std::cout << SerializeJson(registry.CatalogJson(descriptor.Name, descriptor.Title), 2) << '\n';
        return 0;
    }

    ServerInfo info;
    info.Name = descriptor.Name;
    info.Title = descriptor.Title;
    info.Version = std::string(ExyokiOffice::GetVersion());
    info.Instructions = BuildInstructions(descriptor.Adapter->FamilyName(), descriptor.Adapter->FileExtension(),
                                          descriptor.AddressingLines);

    McpServer server(std::move(info), registry, context);

    std::ifstream replayInput;
    if (!replayPath.empty())
    {
        replayInput.open(replayPath);
        if (!replayInput)
        {
            Log::Error("Cannot open the replay file '" + replayPath + "'.");
            return 1;
        }
    }

    StdioTransport transport = replayPath.empty() ? StdioTransport() : StdioTransport(replayInput, std::cout);

    Log::Info(descriptor.Name + " " + std::string(ExyokiOffice::GetVersion()) + " ready with " +
              std::to_string(registry.Tools().size()) + " tools.");

    std::string line;
    bool oversized = false;
    while (transport.ReadLine(line, oversized))
    {
        if (oversized)
        {
            // The message was dropped before it was ever parsed, so there is no
            // id to answer under - the same situation as unparsable JSON, which
            // the specification answers with a null id.
            Log::Warn("Discarding a JSON-RPC line longer than the accepted maximum.");
            transport.WriteLine(SerializeJson(MakeJsonRpcError(
                nullptr, JsonRpcErrorCode::ParseError, "The message exceeds the maximum accepted line length.")));
            continue;
        }

        auto response = server.HandleMessage(line);
        if (response.has_value())
        {
            transport.WriteLine(*response);
        }
    }

    Log::Info("Input closed; shutting down.");
    return 0;
}

} // namespace ExyokiOffice::Mcp
