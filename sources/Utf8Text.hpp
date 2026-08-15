// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

#include <cstddef>
#include <string_view>

namespace ExyokiOffice::Utf8Text
{

/**
 * @file
 * @brief Decoding UTF-8 and the character classes XML defines over code points.
 *
 * Most of this library can classify text one byte at a time, because the tokens
 * it recognizes are ASCII and UTF-8 never spells a non-ASCII character with an
 * ASCII byte - that is what `AsciiText` is for, and what `AsciiText::IsNonAscii`
 * expresses when a rule has to let everything else through untouched.
 *
 * Two jobs cannot be done that way. Deciding whether a name is a legal XML name
 * is a rule about code points, not bytes: `U+00D7` (the multiplication sign) is
 * forbidden while its neighbours are allowed, so a byte-wise test either rejects
 * accented letters or writes a document no conforming parser will read back.
 * Both of those are worse than decoding the sequence.
 */

/// @brief True for the second and further bytes of a multi-byte sequence.
[[nodiscard]] constexpr bool IsContinuationByte(char value) noexcept
{
    return (static_cast<unsigned char>(value) & 0xC0U) == 0x80U;
}

/**
 * @brief Bytes the sequence starting with @p lead claims to occupy, 1 to 4.
 *
 * The answer comes from the lead byte alone and is therefore a claim rather
 * than a fact; a byte that cannot start a sequence answers 1 so that a caller
 * walking a string always makes progress. Use @ref Decode when the difference
 * between a well-formed sequence and a plausible-looking one matters.
 */
[[nodiscard]] constexpr std::size_t SequenceLength(char lead) noexcept
{
    const auto byte = static_cast<unsigned char>(lead);
    if (byte < 0x80U)
    {
        return 1;
    }
    if ((byte & 0xE0U) == 0xC0U)
    {
        return 2;
    }
    if ((byte & 0xF0U) == 0xE0U)
    {
        return 3;
    }
    if ((byte & 0xF8U) == 0xF0U)
    {
        return 4;
    }
    return 1;
}

/// @brief One decoded character, or the single byte that failed to start one.
struct DecodedCharacter
{
    /// @brief The code point, meaningful only when @ref Valid is true.
    char32_t Value = 0;
    /// @brief Bytes to advance by; always at least one unless the input ended.
    std::size_t Length = 0;
    /// @brief False for a truncated, overlong, surrogate, or out-of-range sequence.
    bool Valid = false;
};

/**
 * @brief Decodes the character at @p offset, rejecting the ill-formed spellings.
 *
 * Overlong encodings, the surrogate range, and anything above `U+10FFFF` are
 * reported as invalid rather than decoded: they are the sequences that let a
 * string smuggle one meaning past a check and a different one past a parser.
 * An invalid sequence still reports `Length == 1`, so a loop that ignores
 * @ref DecodedCharacter::Valid degrades to walking bytes instead of hanging.
 */
[[nodiscard]] constexpr DecodedCharacter Decode(std::string_view text, std::size_t offset) noexcept
{
    if (offset >= text.size())
    {
        return {};
    }

    const auto lead = static_cast<unsigned char>(text[offset]);
    if (lead < 0x80U)
    {
        return {static_cast<char32_t>(lead), 1, true};
    }

    const std::size_t length = SequenceLength(text[offset]);
    if (length == 1 || offset + length > text.size())
    {
        return {0, 1, false};
    }

    constexpr unsigned char leadMask[5] = {0, 0, 0x1FU, 0x0FU, 0x07U};
    auto value = static_cast<char32_t>(lead & leadMask[length]);
    for (std::size_t index = 1; index < length; ++index)
    {
        const char continuation = text[offset + index];
        if (!IsContinuationByte(continuation))
        {
            return {0, 1, false};
        }
        value = (value << 6) | static_cast<char32_t>(static_cast<unsigned char>(continuation) & 0x3FU);
    }

    constexpr char32_t minimum[5] = {0, 0, 0x80U, 0x800U, 0x10000U};
    if (value < minimum[length] || value > 0x10FFFFU || (value >= 0xD800U && value <= 0xDFFFU))
    {
        return {0, 1, false};
    }

    return {value, length, true};
}

/**
 * @brief True for XML 1.0 `NameStartChar`, colon included.
 *
 * Callers validating an `NCName` reject the colon themselves; it is part of the
 * production this function names, and leaving it out here would make the
 * function a different one than its name promises.
 */
[[nodiscard]] constexpr bool IsXmlNameStartChar(char32_t value) noexcept
{
    return value == U':' || value == U'_' ||
           (value >= U'A' && value <= U'Z') ||
           (value >= U'a' && value <= U'z') ||
           (value >= 0x00C0U && value <= 0x00D6U) ||
           (value >= 0x00D8U && value <= 0x00F6U) ||
           (value >= 0x00F8U && value <= 0x02FFU) ||
           (value >= 0x0370U && value <= 0x037DU) ||
           (value >= 0x037FU && value <= 0x1FFFU) ||
           (value >= 0x200CU && value <= 0x200DU) ||
           (value >= 0x2070U && value <= 0x218FU) ||
           (value >= 0x2C00U && value <= 0x2FEFU) ||
           (value >= 0x3001U && value <= 0xD7FFU) ||
           (value >= 0xF900U && value <= 0xFDCFU) ||
           (value >= 0xFDF0U && value <= 0xFFFDU) ||
           (value >= 0x10000U && value <= 0xEFFFFU);
}

/// @brief True for XML 1.0 `NameChar`: a name start character, or what may follow one.
[[nodiscard]] constexpr bool IsXmlNameChar(char32_t value) noexcept
{
    return IsXmlNameStartChar(value) || value == U'-' || value == U'.' ||
           (value >= U'0' && value <= U'9') || value == 0x00B7U ||
           (value >= 0x0300U && value <= 0x036FU) ||
           (value >= 0x203FU && value <= 0x2040U);
}

/**
 * @brief True when @p text is a well-formed XML `NCName`.
 *
 * That is an XML `Name` with no colon anywhere in it, which is what every
 * position in this library that takes a bare element, attribute, or prefix
 * name actually requires.
 */
[[nodiscard]] constexpr bool IsNcName(std::string_view text) noexcept
{
    if (text.empty())
    {
        return false;
    }

    std::size_t offset = 0;
    bool first = true;
    while (offset < text.size())
    {
        const DecodedCharacter character = Decode(text, offset);
        if (!character.Valid || character.Value == U':')
        {
            return false;
        }
        if (first ? !IsXmlNameStartChar(character.Value) : !IsXmlNameChar(character.Value))
        {
            return false;
        }
        offset += character.Length;
        first = false;
    }

    return true;
}

} // namespace ExyokiOffice::Utf8Text
