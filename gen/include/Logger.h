// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>

namespace exyoki::generator
{
struct WarningDiagnostic
{
    std::string code;
    std::string category;
    std::string message;
    std::string sourceFile;
    std::string ownerKind;
    std::string ownerName;
    std::string property;
};

class Logger
{
public:
    static void Info(std::string_view message);
    static void Warn(WarningDiagnostic diagnostic);
    static void Error(std::string_view message);

    static void ResetWarnings();
    static std::size_t WarningCount();
    static void WriteWarningReport(const std::filesystem::path& path);
};
} // namespace exyoki::generator
