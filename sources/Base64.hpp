// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

#include "ExyokiOffice/StandardTypes.hpp"

#include "AsciiText.hpp"

#include <array>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ExyokiOffice::Base64
{

/**
 * @file
 * @brief The one base64 codec, for every layer that needs one.
 *
 * Header-only on purpose: the command line and the MCP servers are separate
 * targets, and a codec that lived in the library would have to join the
 * exported ABI just to reach them.
 *
 * Encoding is the same everywhere - standard alphabet, `=` padding to a
 * multiple of four. Decoding is not, and the difference is deliberate rather
 * than accidental, so it is a parameter instead of a second implementation.
 */
enum class Padding
{
    /**
     * @brief What the XML Schema `base64Binary` lexical space allows.
     *
     * The input must be a whole number of quads, `=` may only appear in the
     * final one, and a single `=` in the third position is malformed. This is
     * the rule for anything read out of a package: a producer that cannot pad
     * correctly has written something the format does not define, and silently
     * decoding it invents bytes nobody wrote.
     */
    Required,

    /**
     * @brief Also accepts a final quad that was left unpadded.
     *
     * Base64 arriving from outside a package - a tool argument, an agent's
     * JSON - is routinely written without padding, and its length alone
     * carries the intent. Bits that do not complete a byte are dropped; the
     * encoder would never have produced them in the first place.
     */
    Optional,
};

/// File-local alphabet, lookup and the two decoding passes.
class Base64Helper
{
public:
    static constexpr std::string_view Alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    /// @brief The 6-bit value @p character stands for, or -1 for anything else.
    static int IndexOf(char character) noexcept
    {
        static const std::array<int, 256> table = BuildTable();
        return table[static_cast<unsigned char>(character)];
    }

    /// @brief The quad-by-quad pass, so a padding error is caught as one.
    static bool DecodeStrict(std::string_view filtered, std::vector<Byte>& value)
    {
        if (filtered.size() % 4 != 0)
        {
            return false;
        }

        value.reserve(filtered.size() / 4 * 3);
        for (Size index = 0; index < filtered.size(); index += 4)
        {
            const int s0 = IndexOf(filtered[index]);
            const int s1 = IndexOf(filtered[index + 1]);
            if (s0 < 0 || s1 < 0)
            {
                return false;
            }

            const bool pad2 = filtered[index + 2] == '=';
            const bool pad3 = filtered[index + 3] == '=';
            // Padding closes the value: it belongs to the last quad, and a
            // lone '=' in the third position claims a byte that is not there.
            if (((pad2 || pad3) && index + 4 != filtered.size()) || (pad2 && !pad3))
            {
                return false;
            }

            const int s2 = pad2 ? 0 : IndexOf(filtered[index + 2]);
            const int s3 = pad3 ? 0 : IndexOf(filtered[index + 3]);
            if (s2 < 0 || s3 < 0)
            {
                return false;
            }

            const UInt32 triple = (static_cast<UInt32>(s0) << 18) | (static_cast<UInt32>(s1) << 12) |
                                  (static_cast<UInt32>(s2) << 6) | static_cast<UInt32>(s3);
            value.push_back(static_cast<UInt8>((triple >> 16) & 0xFFu));
            if (!pad2)
            {
                value.push_back(static_cast<UInt8>((triple >> 8) & 0xFFu));
            }
            if (!pad3)
            {
                value.push_back(static_cast<UInt8>(triple & 0xFFu));
            }
        }

        return true;
    }

    /// @brief The streaming pass, which stops reading once padding begins.
    static bool DecodeLenient(std::string_view filtered, std::vector<Byte>& value)
    {
        // Unsigned: the accumulator is a bit buffer, and mixing it with the
        // unsigned mask below would otherwise convert a signed value.
        UInt32 accumulator = 0;
        int bits = -8;
        bool padded = false;
        for (const char character : filtered)
        {
            if (character == '=')
            {
                padded = true;
                continue;
            }

            if (padded)
            {
                return false;
            }

            const int digit = IndexOf(character);
            if (digit < 0)
            {
                return false;
            }

            accumulator = (accumulator << 6) | static_cast<UInt32>(digit);
            bits += 6;
            if (bits >= 0)
            {
                value.push_back(static_cast<UInt8>((accumulator >> bits) & 0xFFu));
                bits -= 8;
            }
        }

        return true;
    }

private:
    static std::array<int, 256> BuildTable() noexcept
    {
        std::array<int, 256> table{};
        table.fill(-1);
        for (Size index = 0; index < Alphabet.size(); ++index)
        {
            table[static_cast<unsigned char>(Alphabet[index])] = static_cast<int>(index);
        }
        return table;
    }
};

/// @brief @p bytes as standard, padded base64.
[[nodiscard]] inline std::string Encode(std::span<const Byte> bytes)
{
    std::string text;
    text.reserve((bytes.size() + 2) / 3 * 4);

    Size index = 0;
    while (index + 2 < bytes.size())
    {
        const UInt32 triple = (static_cast<UInt32>(bytes[index]) << 16) |
                              (static_cast<UInt32>(bytes[index + 1]) << 8) | static_cast<UInt32>(bytes[index + 2]);
        text.push_back(Base64Helper::Alphabet[(triple >> 18) & 0x3Fu]);
        text.push_back(Base64Helper::Alphabet[(triple >> 12) & 0x3Fu]);
        text.push_back(Base64Helper::Alphabet[(triple >> 6) & 0x3Fu]);
        text.push_back(Base64Helper::Alphabet[triple & 0x3Fu]);
        index += 3;
    }

    const Size remaining = bytes.size() - index;
    if (remaining == 1)
    {
        const UInt32 triple = static_cast<UInt32>(bytes[index]) << 16;
        text.push_back(Base64Helper::Alphabet[(triple >> 18) & 0x3Fu]);
        text.push_back(Base64Helper::Alphabet[(triple >> 12) & 0x3Fu]);
        text.append("==");
    }
    else if (remaining == 2)
    {
        const UInt32 triple = (static_cast<UInt32>(bytes[index]) << 16) | (static_cast<UInt32>(bytes[index + 1]) << 8);
        text.push_back(Base64Helper::Alphabet[(triple >> 18) & 0x3Fu]);
        text.push_back(Base64Helper::Alphabet[(triple >> 12) & 0x3Fu]);
        text.push_back(Base64Helper::Alphabet[(triple >> 6) & 0x3Fu]);
        text.push_back('=');
    }

    return text;
}

/**
 * @brief Decodes @p text into @p value.
 *
 * ASCII whitespace is ignored wherever it appears, so text wrapped across
 * lines by an XML producer decodes unchanged. @p value is cleared first and
 * left empty when the input is rejected, so a caller that ignores the result
 * cannot mistake a partial decode for the real thing.
 */
[[nodiscard]] inline bool Decode(std::string_view text,
                                 std::vector<Byte>& value,
                                 Padding padding = Padding::Required)
{
    value.clear();

    std::string filtered;
    filtered.reserve(text.size());
    for (const char character : text)
    {
        if (!AsciiText::IsSpace(character))
        {
            filtered.push_back(character);
        }
    }

    if (filtered.empty())
    {
        return true;
    }

    const bool decoded = (padding == Padding::Required) ? Base64Helper::DecodeStrict(filtered, value)
                                                        : Base64Helper::DecodeLenient(filtered, value);
    if (!decoded)
    {
        value.clear();
    }

    return decoded;
}

} // namespace ExyokiOffice::Base64
