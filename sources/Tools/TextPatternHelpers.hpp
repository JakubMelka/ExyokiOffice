// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

#include "ExyokiOffice/TextPattern.hpp"
#include "ExyokiOffice/Tools/PackageModel.hpp"

#include "TextPatternCompiler.hpp"

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
 * case-insensitive. The escaping rules and the compiler itself live in
 * `TextPatternCompiler.hpp`, next to the public `RegexPattern` they implement;
 * what is here is only the tools' convention for turning three arguments plus a
 * diagnostics list into one of them.
 */
class TextPattern
{
public:
    /// @brief Escapes ECMAScript metacharacters so a literal needle matches as itself.
    static std::string EscapeRegexLiteral(std::string_view text) { return Detail::EscapeRegexLiteral(text); }

    /// @brief Escapes `$` so a literal replacement survives format expansion.
    static std::string EscapeFormatLiteral(std::string_view text) { return Detail::EscapeFormatLiteral(text); }

    /**
     * @brief Describes @p needle as a pattern, or std::nullopt when none is wanted.
     *
     * `std::nullopt` carries two different outcomes on purpose. With neither
     * @p useRegex nor @p ignoreCase there is nothing to build and the caller
     * takes its plain substring path; with either of them it means the regular
     * expression did not compile, and a diagnostic has been appended. The call
     * site already knows which flags it passed, so it can tell them apart.
     */
    static std::optional<RegexPattern> Describe(std::string_view needle,
                                                bool useRegex,
                                                bool ignoreCase,
                                                std::vector<ToolDiagnostic>& diagnostics)
    {
        if (!useRegex && !ignoreCase)
        {
            return std::nullopt;
        }

        RegexPattern pattern;
        pattern.Expression = useRegex ? std::string(needle) : Detail::EscapeRegexLiteral(needle);
        pattern.IgnoreCase = ignoreCase;

        std::string error;
        if (!pattern.IsValid(&error))
        {
            diagnostics.push_back(ToolDiagnostic{ToolSeverity::Error, "Invalid regular expression", std::move(error)});
            return std::nullopt;
        }
        return pattern;
    }

    /// @brief As @ref Describe, compiled for a caller that matches text itself.
    static std::optional<std::regex> BuildPattern(std::string_view needle,
                                                  bool useRegex,
                                                  bool ignoreCase,
                                                  std::vector<ToolDiagnostic>& diagnostics)
    {
        const auto described = Describe(needle, useRegex, ignoreCase, diagnostics);
        if (!described)
        {
            return std::nullopt;
        }
        return Detail::CompileRegex(*described);
    }
};

} // namespace ExyokiOffice::Tools
