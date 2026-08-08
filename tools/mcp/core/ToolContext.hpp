// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

#include "FamilyAdapter.hpp"
#include "SessionStore.hpp"
#include "Workspace.hpp"

#include "ExyokiOffice/StandardTypes.hpp"

#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace ExyokiOffice::Mcp
{

class ToolRegistry;

/** @brief Verbosity of the stderr log. */
enum class LogLevel
{
    Error,
    Warn,
    Info,
    Debug
};

/**
 * @brief Diagnostic logging for the servers.
 *
 * Everything goes to stderr. Standard output belongs exclusively to the
 * JSON-RPC stream: one stray line there desynchronizes the client's parser and
 * takes the whole session down with it.
 */
class Log
{
public:
    static void SetLevel(LogLevel level) noexcept { s_level = level; }
    [[nodiscard]] static LogLevel Level() noexcept { return s_level; }

    static void Write(LogLevel level, std::string_view message)
    {
        if (static_cast<int>(level) > static_cast<int>(s_level))
        {
            return;
        }

        std::cerr << Prefix(level) << message << '\n';
    }

    static void Error(std::string_view message) { Write(LogLevel::Error, message); }
    static void Warn(std::string_view message) { Write(LogLevel::Warn, message); }
    static void Info(std::string_view message) { Write(LogLevel::Info, message); }
    static void Debug(std::string_view message) { Write(LogLevel::Debug, message); }

private:
    static std::string_view Prefix(LogLevel level) noexcept
    {
        switch (level)
        {
            case LogLevel::Error:
                return "[error] ";
            case LogLevel::Warn:
                return "[warn] ";
            case LogLevel::Info:
                return "[info] ";
            case LogLevel::Debug:
                return "[debug] ";
        }

        return "[info] ";
    }

    inline static LogLevel s_level = LogLevel::Warn;
};

/** @brief Command-line and environment configuration shared by all three binaries. */
struct ServerOptions
{
    /// Roots of the permitted file space; empty means the working directory.
    std::vector<std::filesystem::path> WorkspaceRoots;
    /// Register only tools annotated `readOnlyHint`.
    bool ReadOnly = false;
    /// Tool groups to register; empty registers every group.
    std::vector<std::string> Toolsets;
    Size MaxDocuments = 16;
    Size SnapshotDepth = 8;
    Size MaxMediaSizeMiB = 8;
    LogLevel Level = LogLevel::Warn;
    /// ZIP/XML safety limits every document is opened with; see `--package-limits`.
    OpenXmlPackageLimits PackageLimits = OpenXmlPackageLimits::Recommended();
};

/**
 * @brief Everything a tool handler is allowed to reach.
 *
 * Handlers get a mutable context because opening, closing, and mutating
 * documents all change session state. The registry pointer exists for `batch`,
 * which dispatches other tools of the same server.
 */
class ToolContext
{
public:
    ToolContext(const ServerOptions& options, Workspace& workspace, SessionStore& sessions, FamilyAdapter& adapter)
        : m_options(&options), m_workspace(&workspace), m_sessions(&sessions), m_adapter(&adapter)
    {
    }

    [[nodiscard]] const ServerOptions& Options() const noexcept { return *m_options; }
    [[nodiscard]] Workspace& GetWorkspace() const noexcept { return *m_workspace; }
    [[nodiscard]] SessionStore& Sessions() const noexcept { return *m_sessions; }
    [[nodiscard]] FamilyAdapter& Adapter() const noexcept { return *m_adapter; }

    void SetRegistry(const ToolRegistry* registry) noexcept { m_registry = registry; }
    [[nodiscard]] const ToolRegistry* Registry() const noexcept { return m_registry; }

    /// Media payload limit in bytes, derived from `--max-media-size`.
    [[nodiscard]] Size MaxMediaBytes() const noexcept { return m_options->MaxMediaSizeMiB * 1024u * 1024u; }

private:
    const ServerOptions* m_options = nullptr;
    Workspace* m_workspace = nullptr;
    SessionStore* m_sessions = nullptr;
    FamilyAdapter* m_adapter = nullptr;
    const ToolRegistry* m_registry = nullptr;
};

} // namespace ExyokiOffice::Mcp
