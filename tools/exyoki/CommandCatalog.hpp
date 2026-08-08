// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

#include "ExyokiOffice/Tools/Report.hpp"

#include <CLI11.hpp>

namespace exyoki
{

/**
 * @brief Describes the whole command line interface as a report document.
 *
 * The catalog is derived from the live CLI11 parser rather than from a
 * hand-maintained table, so `exyoki commands --format json` cannot drift away
 * from what the tool actually accepts. It lists the global options, the exit
 * code table, and every command with its positionals, options, value
 * constraints, defaults, and the exit codes that command can return.
 */
ExyokiOffice::Tools::ReportDocument AdaptCommands(const CLI::App& app);

} // namespace exyoki
