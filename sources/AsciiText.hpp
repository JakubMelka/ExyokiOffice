// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

#include <string>
#include <string_view>

namespace ExyokiOffice::AsciiText
{

/**
 * @file
 * @brief Case folding and trimming that answer for ASCII and nothing else.
 *
 * Everything this library folds case on is defined by a specification as an
 * ASCII token: XML local names and prefixes, content types, relationship
 * types, file extensions, number format codes, formula function names. None of
 * them means anything different in another alphabet.
 *
 * `<cctype>` cannot express that. `std::tolower` answers according to the
 * global C locale, which belongs to the hosting application - a library that
 * calls it lets `setlocale` decide whether `TITLE` is still recognized as a
 * field name, because Turkish locales fold `I` to a dotless `i`. These
 * functions decide it here instead, and stay `constexpr` while they are at it.
 *
 * Bytes outside the ASCII range pass through untouched, so UTF-8 sequences
 * survive a fold that only ever meant to normalize a token.
 */

/// @brief `A`-`Z` folded down; every other byte unchanged.
[[nodiscard]] constexpr char ToLower(char value) noexcept
{
    return (value >= 'A' && value <= 'Z') ? static_cast<char>(value - 'A' + 'a') : value;
}

/// @brief `a`-`z` folded up; every other byte unchanged.
[[nodiscard]] constexpr char ToUpper(char value) noexcept
{
    return (value >= 'a' && value <= 'z') ? static_cast<char>(value - 'a' + 'A') : value;
}

/// @brief A copy of @p text with every ASCII letter folded down.
[[nodiscard]] inline std::string ToLower(std::string_view text)
{
    std::string result(text);
    for (char& character : result)
    {
        character = ToLower(character);
    }
    return result;
}

/// @brief A copy of @p text with every ASCII letter folded up.
[[nodiscard]] inline std::string ToUpper(std::string_view text)
{
    std::string result(text);
    for (char& character : result)
    {
        character = ToUpper(character);
    }
    return result;
}

/// @brief Compares @p left and @p right folding ASCII case, without allocating.
[[nodiscard]] constexpr bool EqualsIgnoreCase(std::string_view left, std::string_view right) noexcept
{
    if (left.size() != right.size())
    {
        return false;
    }

    for (std::size_t index = 0; index < left.size(); ++index)
    {
        if (ToLower(left[index]) != ToLower(right[index]))
        {
            return false;
        }
    }

    return true;
}

/// @brief True when @p text begins with @p prefix, folding ASCII case.
[[nodiscard]] constexpr bool StartsWithIgnoreCase(std::string_view text, std::string_view prefix) noexcept
{
    return text.size() >= prefix.size() && EqualsIgnoreCase(text.substr(0, prefix.size()), prefix);
}

/// @brief True for the six characters XML and C both call whitespace.
[[nodiscard]] constexpr bool IsSpace(char value) noexcept
{
    return value == ' ' || value == '\t' || value == '\n' || value == '\v' || value == '\f' || value == '\r';
}

/**
 * @brief @p text without leading and trailing ASCII whitespace.
 *
 * The result points into @p text, so it lives exactly as long as the argument
 * does; construct a `std::string` from it when it has to outlive the call.
 */
[[nodiscard]] constexpr std::string_view Trim(std::string_view text) noexcept
{
    std::size_t first = 0;
    while (first < text.size() && IsSpace(text[first]))
    {
        ++first;
    }

    std::size_t last = text.size();
    while (last > first && IsSpace(text[last - 1]))
    {
        --last;
    }

    return text.substr(first, last - first);
}

} // namespace ExyokiOffice::AsciiText
