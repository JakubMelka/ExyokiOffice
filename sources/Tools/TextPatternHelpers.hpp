// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

#include "ExyokiOffice/Tools/PackageModel.hpp"

#include <optional>
#include <regex>
#include <string>
#include <string_view>
#include <vector>

namespace ExyokiOffice::Tools
{

/**
 * @file
 * @brief The search-and-replace grammar the text tools share.
 *
 * Both `DocumentTextTools` and `WordTextTools` expose the same needle: a plain
 * substring, or an ECMAScript regular expression, either of them optionally
 * case-insensitive. The two escaping rules that go with it are easy to mix up,
 * so they are named for the side they belong to rather than for the character
 * they touch.
 */
class TextPattern
{
public:
    /// @brief Escapes ECMAScript metacharacters so a literal needle matches as itself.
    static std::string EscapeRegexLiteral(std::string_view text)
    {
        static constexpr std::string_view special = "\\^$.|?*+()[]{}";
        std::string escaped;
        escaped.reserve(text.size());
        for (const char character : text)
        {
            if (special.find(character) != std::string_view::npos)
            {
                escaped += '\\';
            }
            escaped += character;
        }
        return escaped;
    }

    /**
     * @brief Escapes `$` so a literal replacement survives format expansion.
     *
     * `std::regex_replace` reads `$1`, `$&` and friends out of the replacement
     * string, so a replacement the caller meant literally has to double every
     * `$` before it gets there. This is the replacement side of the grammar,
     * not the pattern side - @ref EscapeRegexLiteral is what the needle needs.
     */
    static std::string EscapeFormatLiteral(std::string_view text)
    {
        std::string escaped;
        escaped.reserve(text.size());
        for (const char character : text)
        {
            if (character == '$')
            {
                escaped += '$';
            }
            escaped += character;
        }
        return escaped;
    }

    /**
     * @brief Compiles @p needle, or `std::nullopt` when no pattern is wanted.
     *
     * `std::nullopt` carries two different outcomes on purpose. With neither
     * @p useRegex nor @p ignoreCase there is nothing to compile and the caller
     * takes its plain substring path; with either of them it means the regular
     * expression did not compile, and a diagnostic has been appended. The call
     * site already knows which flags it passed, so it can tell them apart.
     */
    static std::optional<std::regex> BuildPattern(std::string_view needle,
                                                  bool useRegex,
                                                  bool ignoreCase,
                                                  std::vector<ToolDiagnostic>& diagnostics)
    {
        if (!useRegex && !ignoreCase)
        {
            return std::nullopt;
        }

        const std::string patternText = useRegex ? std::string(needle) : EscapeRegexLiteral(needle);
        auto flags = std::regex::ECMAScript;
        if (ignoreCase)
        {
            flags |= std::regex::icase;
        }

        try
        {
            return std::regex(patternText, flags);
        }
        catch (const std::regex_error& error)
        {
            diagnostics.push_back(ToolDiagnostic{ToolSeverity::Error, "Invalid regular expression", error.what()});
            return std::nullopt;
        }
    }
};

} // namespace ExyokiOffice::Tools
