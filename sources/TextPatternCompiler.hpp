// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

#include "ExyokiOffice/TextPattern.hpp"

#include <optional>
#include <regex>
#include <string>
#include <string_view>

namespace ExyokiOffice::Detail
{

/**
 * @file
 * @brief The one place `<regex>` is turned on, and the rules that go with it.
 *
 * Everything the public API says about a @ref ExyokiOffice::RegexPattern is
 * enforced here rather than at each call site, so "an invalid expression finds
 * nothing" and "an over-long subject is not searched" cannot drift apart
 * between the Word paragraph API and the text tools.
 */

/// @brief Escapes ECMAScript metacharacters so a literal needle matches as itself.
[[nodiscard]] inline std::string EscapeRegexLiteral(std::string_view text)
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
 * `std::regex_replace` and `match_results::format` read `$1`, `$&` and friends
 * out of the replacement string, so a replacement the caller meant literally
 * has to double every `$` before it gets there. This is the replacement side of
 * the grammar, not the pattern side - @ref EscapeRegexLiteral is what the
 * needle needs.
 */
[[nodiscard]] inline std::string EscapeFormatLiteral(std::string_view text)
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
 * @brief Compiles @p pattern, or std::nullopt when it does not compile.
 *
 * @param error Optional destination for the message `std::regex_error` carries.
 */
[[nodiscard]] inline std::optional<std::regex> CompileRegex(const RegexPattern& pattern, std::string* error = nullptr)
{
    auto flags = std::regex::ECMAScript;
    if (pattern.IgnoreCase)
    {
        flags |= std::regex::icase;
    }

    try
    {
        return std::regex(pattern.Expression, flags);
    }
    catch (const std::regex_error& failure)
    {
        if (error)
        {
            *error = failure.what();
        }
        return std::nullopt;
    }
}

/**
 * @brief True when @p subject is short enough to hand to the regex engine.
 *
 * See RegexPattern::MaximumSubjectLength: past this length a recursive matcher
 * can overrun the stack, which is not a failure any caller can recover from.
 */
[[nodiscard]] inline bool IsSearchableSubject(std::string_view subject) noexcept
{
    return subject.size() <= RegexPattern::MaximumSubjectLength;
}

} // namespace ExyokiOffice::Detail
