// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

#include <CLI11.hpp>

#include <string>
#include <string_view>

namespace exyoki
{

/**
 * @brief Generates a shell completion script for the live CLI11 parser.
 *
 * Like the `commands` catalog, the script is derived from the parser the tool
 * actually runs on, so it cannot offer a command or option that does not
 * exist. Supported shells: "bash", "zsh", and "powershell" (alias "pwsh").
 * The script completes command names in the first position, that command's
 * options (plus the global ones) after it, and falls back to filename
 * completion everywhere else. Returns an empty string for an unknown shell.
 */
std::string GenerateCompletionScript(const CLI::App& app, std::string_view shell);

} // namespace exyoki
