// Copyright (c) 2026 Jakub Melka and Contributors
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
 * @brief True for '0' to '9'.
 *
 * The C library answers this one the same way in every locale, so this exists
 * for uniformity rather than to fix anything: a file that classifies some of
 * its characters here and the rest through `<cctype>` invites the next reader
 * to copy whichever line is nearer.
 */
[[nodiscard]] constexpr bool IsDigit(char value) noexcept
{
    return value >= '0' && value <= '9';
}

/// @brief True for the sixteen hexadecimal digits, in either case.
[[nodiscard]] constexpr bool IsHexDigit(char value) noexcept
{
    return IsDigit(value) || (value >= 'a' && value <= 'f') || (value >= 'A' && value <= 'F');
}

/**
 * @brief True for 'A' to 'Z' and 'a' to 'z', and for nothing else.
 *
 * In particular false for every byte of a UTF-8 multi-byte sequence, which is
 * the answer that keeps such a sequence whole: those bytes all have the high
 * bit set, so they never collide with an ASCII character, whereas a
 * single-byte locale would call some of them letters and split the character
 * in half.
 */
[[nodiscard]] constexpr bool IsAlpha(char value) noexcept
{
    return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z');
}

/// @brief True for an ASCII letter or an ASCII digit.
[[nodiscard]] constexpr bool IsAlnum(char value) noexcept
{
    return IsAlpha(value) || IsDigit(value);
}

/// @brief True for the printable ASCII that is neither a letter, a digit, nor a space.
[[nodiscard]] constexpr bool IsPunct(char value) noexcept
{
    const auto byte = static_cast<unsigned char>(value);
    return byte > 0x20U && byte < 0x7FU && !IsAlnum(value);
}

/**
 * @brief True for the ASCII control characters: 0x00 to 0x1F, and DEL.
 *
 * A name rejected for holding a control character must not be rejected for
 * holding a non-ASCII letter, and a locale that classifies the continuation
 * bytes of a UTF-8 sequence would do exactly that.
 */
[[nodiscard]] constexpr bool IsControl(char value) noexcept
{
    const auto byte = static_cast<unsigned char>(value);
    return byte < 0x20U || byte == 0x7FU;
}

/**
 * @brief True for a byte that belongs to a UTF-8 multi-byte sequence.
 *
 * Every rule in this library that spells out which ASCII characters it accepts
 * has to say something about the rest, and for a rule over UTF-8 text the
 * answer is almost always "let them through": a name may be written in any
 * script, and a tokenizer that treated `0xC4` as a separator would cut a
 * character in half. Spelling that as a named test rather than a bare
 * comparison against `0x80` keeps the decision greppable.
 *
 * This is a test on one byte, so it cannot tell a well-formed sequence from a
 * malformed one. Where that distinction matters - deciding whether a name is a
 * legal XML name, for instance - decode instead, with `Utf8Text`.
 */
[[nodiscard]] constexpr bool IsNonAscii(char value) noexcept
{
    return static_cast<unsigned char>(value) >= 0x80U;
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
