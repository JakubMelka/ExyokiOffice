// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "ExyokiOffice/Tools/OutputNaming.hpp"

#include "ExyokiOffice/StandardTypes.hpp"

#include <vector>

namespace ExyokiOffice::Tools
{

/// File-local helpers behind the output-name rules.
class OutputNamingHelper
{
public:
    static bool EqualsIgnoringCase(std::string_view left, std::string_view right)
    {
        if (left.size() != right.size())
        {
            return false;
        }

        for (Size index = 0; index < left.size(); ++index)
        {
            if (ToLower(left[index]) != ToLower(right[index]))
            {
                return false;
            }
        }

        return true;
    }

    static char ToLower(char value)
    {
        if (value >= 'A' && value <= 'Z')
        {
            return static_cast<char>(value - 'A' + 'a');
        }

        return value;
    }

    /**
     * @brief True when @p name reaches a DOS device rather than a file.
     *
     * Windows cuts the name at the first dot and ignores trailing dots and
     * spaces, so `NUL`, `nul.png` and `aux .` all reach the same device.
     */
    static bool IsReservedDeviceName(std::string_view name)
    {
        auto stem = name.substr(0, name.find('.'));
        while (!stem.empty() && (stem.back() == ' ' || stem.back() == '.'))
        {
            stem.remove_suffix(1);
        }

        static constexpr std::string_view devices[] = {"CON", "PRN", "AUX", "NUL", "CONIN$", "CONOUT$"};
        for (const auto& device : devices)
        {
            if (EqualsIgnoringCase(stem, device))
            {
                return true;
            }
        }

        // COM0..COM9 and LPT0..LPT9.
        return stem.size() == 4 &&
               (EqualsIgnoringCase(stem.substr(0, 3), "COM") || EqualsIgnoringCase(stem.substr(0, 3), "LPT")) &&
               stem[3] >= '0' && stem[3] <= '9';
    }

    /// True for a character that must not appear anywhere in an output name.
    static bool IsRefusedCharacter(char value)
    {
        return static_cast<unsigned char>(value) < 0x20 || value == '/' || value == '\\' || value == ':';
    }

    /// The non-empty components of @p path, normalized.
    static std::vector<std::filesystem::path> Components(const std::filesystem::path& path)
    {
        std::vector<std::filesystem::path> components;
        for (const auto& component : path.lexically_normal())
        {
            // A trailing separator produces an empty final component ("a/b/"
            // iterates as a, b, ""), which would otherwise never match.
            if (!component.empty())
            {
                components.push_back(component);
            }
        }

        return components;
    }
};

bool IsPlainOutputName(std::string_view name) noexcept
{
    if (name.empty() || name == "." || name == "..")
    {
        return false;
    }

    for (const char character : name)
    {
        if (OutputNamingHelper::IsRefusedCharacter(character))
        {
            return false;
        }
    }

    if (name.back() == '.' || name.back() == ' ')
    {
        return false;
    }

    return !OutputNamingHelper::IsReservedDeviceName(name);
}

std::string MakePlainOutputName(std::string_view name, std::string_view fallback)
{
    std::string sanitized;
    sanitized.reserve(name.size());
    for (const char character : name)
    {
        sanitized.push_back(OutputNamingHelper::IsRefusedCharacter(character) ? '_' : character);
    }

    while (!sanitized.empty() && (sanitized.back() == '.' || sanitized.back() == ' '))
    {
        sanitized.pop_back();
    }

    // What is left can still be unusable — `..` loses its dots and empties, a
    // device name survives character replacement untouched — so the rewritten
    // name is held to the same rule as any other.
    return IsPlainOutputName(sanitized) ? sanitized : std::string(fallback);
}

bool IsInsideDirectory(const std::filesystem::path& directory, const std::filesystem::path& path)
{
    const auto directoryComponents = OutputNamingHelper::Components(directory);
    const auto pathComponents = OutputNamingHelper::Components(path);
    if (pathComponents.size() < directoryComponents.size())
    {
        return false;
    }

    for (Size index = 0; index < directoryComponents.size(); ++index)
    {
        if (pathComponents[index] != directoryComponents[index])
        {
            return false;
        }
    }

    return true;
}

} // namespace ExyokiOffice::Tools
