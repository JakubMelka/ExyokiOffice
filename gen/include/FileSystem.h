// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace exyoki::generator
{
std::string ReadFileText(const std::filesystem::path& path);
std::vector<std::filesystem::path> EnumerateFiles(const std::filesystem::path& root, const std::string& extension);
void EnsureDirectory(const std::filesystem::path& path);
void WriteFileText(const std::filesystem::path& path, const std::string& content);
} // namespace exyoki::generator
