// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

#include "FamilyAdapter.hpp"
#include "ToolRegistry.hpp"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace ExyokiOffice::Mcp
{

/// Registers one toolset into a server's catalog.
using ToolsetRegistrar = std::function<void(ToolRegistry&)>;

/**
 * @brief Everything that distinguishes one server binary from the other two.
 *
 * A binary's `main` fills this in and hands it to RunMcpServer; the option
 * parsing, the catalog assembly, the stdio loop, and the shutdown are all
 * shared, so a family binary stays under a hundred lines.
 */
struct ServerDescriptor
{
    /// Program name published as `serverInfo.name`, for example `exyoki-mcp-word`.
    std::string Name;
    /// Human-readable server title.
    std::string Title;
    /// Family-specific addressing guidance appended to the shared instructions.
    std::vector<std::string> AddressingLines;
    /// Family operations the shared tools are parameterized by.
    std::shared_ptr<FamilyAdapter> Adapter;
    /// Registrars run in order; the shared toolset is registered first automatically.
    std::vector<ToolsetRegistrar> Toolsets;
};

/**
 * @brief Parses the command line, assembles the server, and runs it to EOF.
 *
 * @return Process exit code: zero for a clean shutdown, non-zero for a usage
 *         or start-up failure.
 */
[[nodiscard]] int RunMcpServer(const ServerDescriptor& descriptor, int argc, char** argv);

} // namespace ExyokiOffice::Mcp
