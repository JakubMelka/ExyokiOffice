// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "FileSystem.h"

#include <algorithm>
#include <fstream>
#include <stdexcept>

namespace exyoki::generator
{
std::string ReadFileText(const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::in | std::ios::binary);
    if (!stream)
    {
        throw std::runtime_error("Failed to open file: " + path.string());
    }

    std::string content;
    stream.seekg(0, std::ios::end);
    content.resize(static_cast<std::size_t>(stream.tellg()));
    stream.seekg(0, std::ios::beg);
    stream.read(content.data(), static_cast<std::streamsize>(content.size()));
    return content;
}

std::vector<std::filesystem::path> EnumerateFiles(const std::filesystem::path& root, const std::string& extension)
{
    std::vector<std::filesystem::path> results;
    if (!std::filesystem::exists(root))
    {
        return results;
    }

    for (const auto& entry : std::filesystem::recursive_directory_iterator(root))
    {
        if (entry.is_regular_file())
        {
            if (extension.empty() || entry.path().extension() == extension)
            {
                results.push_back(entry.path());
            }
        }
    }

    std::sort(results.begin(), results.end());
    return results;
}

void EnsureDirectory(const std::filesystem::path& path)
{
    if (path.empty())
    {
        return;
    }

    std::filesystem::create_directories(path);
}

void WriteFileText(const std::filesystem::path& path, const std::string& content)
{
    EnsureDirectory(path.parent_path());
    std::ofstream stream(path, std::ios::out | std::ios::binary | std::ios::trunc);
    if (!stream)
    {
        throw std::runtime_error("Failed to write file: " + path.string());
    }

    stream.write(content.data(), static_cast<std::streamsize>(content.size()));
}
} // namespace exyoki::generator
