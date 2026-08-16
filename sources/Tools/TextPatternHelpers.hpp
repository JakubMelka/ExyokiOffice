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
     * @brief Compiles @p needle into a pattern, or std::nullopt when none is wanted.
     *
     * `std::nullopt` carries two different outcomes on purpose. With neither
     * @p useRegex nor @p ignoreCase there is nothing to build and the caller
     * takes its plain substring path; with either of them it means the regular
     * expression did not compile, and a diagnostic has been appended. The call
     * site already knows which flags it passed, so it can tell them apart.
     *
     * The expression is compiled here once and matched against every text unit
     * of the document, which is why the tools hold a compiled expression rather
     * than a `RegexPattern`: building the automaton is the expensive half of a
     * short match, and doing it per paragraph or per cell would scale the cost
     * of a search with the size of the document rather than with its text.
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

        RegexPattern pattern;
        pattern.Expression = useRegex ? std::string(needle) : Detail::EscapeRegexLiteral(needle);
        pattern.IgnoreCase = ignoreCase;

        std::string error;
        auto compiled = Detail::CompileRegex(pattern, &error);
        if (!compiled)
        {
            diagnostics.push_back(ToolDiagnostic{ToolSeverity::Error, "Invalid regular expression", std::move(error)});
        }
        return compiled;
    }
};

} // namespace ExyokiOffice::Tools
