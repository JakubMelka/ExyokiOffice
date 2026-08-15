// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "Utf8Text.hpp"

#include <doctest/doctest.h>

#include <cstddef>
#include <string_view>

using namespace ExyokiOffice;

// The compiler is not told the source encoding, so every character above ASCII
// is written as an escape or a code point here rather than spelled out. The
// name in the comment beside it says which one is meant.

namespace
{

constexpr char32_t kCCaron = 0x010DU;         // c with caron
constexpr char32_t kEuroSign = 0x20ACU;       // euro sign
constexpr char32_t kPageEmoji = 0x1F4C4U;     // page facing up
constexpr char32_t kCjkIdeograph = 0x4F60U;   // CJK ideograph
constexpr char32_t kMultiplication = 0x00D7U; // multiplication sign
constexpr char32_t kDivision = 0x00F7U;       // division sign
constexpr char32_t kODiaeresis = 0x00D6U;     // O with diaeresis
constexpr char32_t kOStroke = 0x00D8U;        // O with stroke
constexpr char32_t kMiddleDot = 0x00B7U;      // middle dot
constexpr char32_t kCombiningAcute = 0x0301U; // combining acute accent

} // namespace

TEST_CASE("A well-formed sequence decodes to its code point [unit] [utf8-text]")
{
    struct Sample
    {
        std::string_view Bytes;
        char32_t Value;
        std::size_t Length;
    };

    const Sample samples[] = {
        {"A", U'A', 1},
        {"\x7F", 0x7FU, 1},
        {"\xC2\x80", 0x80U, 2}, // the lowest code point needing two bytes
        {"\xC4\x8D", kCCaron, 2},
        {"\xE2\x82\xAC", kEuroSign, 3},
        {"\xEF\xBF\xBD", 0xFFFDU, 3}, // replacement character
        {"\xF0\x9F\x93\x84", kPageEmoji, 4},
        {"\xF4\x8F\xBF\xBF", 0x10FFFFU, 4}, // the last code point there is
    };

    for (const Sample& sample : samples)
    {
        CAPTURE(sample.Value);
        const Utf8Text::DecodedCharacter decoded = Utf8Text::Decode(sample.Bytes, 0);
        CHECK(decoded.Valid);
        CHECK(decoded.Value == sample.Value);
        CHECK(decoded.Length == sample.Length);
    }
}

TEST_CASE("An ill-formed sequence is reported rather than decoded [unit] [utf8-text]")
{
    // Each of these is a way of writing something that is not a character: a
    // lone continuation byte, a lead byte whose sequence was cut short, an
    // overlong spelling of '/', a surrogate half, and a value past U+10FFFF.
    // Accepting any of them would let a name pass a check in one spelling and
    // reach a parser in another.
    const std::string_view malformed[] = {
        "\x80",
        "\xBF",
        "\xC4",
        "\xE2\x82",
        "\xC0\xAF",
        "\xE0\x80\xAF",
        "\xED\xA0\x80",
        "\xF5\x80\x80\x80",
        "\xFF",
        "\xC4"
        "A", // a lead byte followed by something that cannot continue it
    };

    for (const std::string_view sequence : malformed)
    {
        CAPTURE(static_cast<int>(static_cast<unsigned char>(sequence.front())));
        const Utf8Text::DecodedCharacter decoded = Utf8Text::Decode(sequence, 0);
        CHECK_FALSE(decoded.Valid);
        // One byte of progress, so a loop over the string always terminates.
        CHECK(decoded.Length == 1);
    }

    CHECK(Utf8Text::Decode("", 0).Length == 0);
    CHECK(Utf8Text::Decode("abc", 3).Length == 0);
}

TEST_CASE("Walking a string by sequence length visits every character once [unit] [utf8-text]")
{
    const std::string_view text = "a\xC4\x8D\xE2\x82\xAC\xF0\x9F\x93\x84z";
    std::size_t offset = 0;
    std::size_t characters = 0;
    while (offset < text.size())
    {
        const Utf8Text::DecodedCharacter decoded = Utf8Text::Decode(text, offset);
        CHECK(decoded.Valid);
        CHECK(decoded.Length == Utf8Text::SequenceLength(text[offset]));
        offset += decoded.Length;
        ++characters;
    }
    CHECK(characters == 5);
    CHECK(offset == text.size());
}

TEST_CASE("Continuation bytes are the ones carrying no character of their own [unit] [utf8-text]")
{
    CHECK_FALSE(Utf8Text::IsContinuationByte('a'));
    CHECK_FALSE(Utf8Text::IsContinuationByte('\x7F'));
    CHECK(Utf8Text::IsContinuationByte('\x80'));
    CHECK(Utf8Text::IsContinuationByte('\xBF'));
    CHECK_FALSE(Utf8Text::IsContinuationByte('\xC4')); // a lead byte
}

TEST_CASE("XML name characters follow the production, not the byte [unit] [utf8-text]")
{
    CHECK(Utf8Text::IsXmlNameStartChar(U'a'));
    CHECK(Utf8Text::IsXmlNameStartChar(U'_'));
    CHECK(Utf8Text::IsXmlNameStartChar(U':'));
    CHECK(Utf8Text::IsXmlNameStartChar(kCCaron));
    CHECK(Utf8Text::IsXmlNameStartChar(kCjkIdeograph));
    CHECK_FALSE(Utf8Text::IsXmlNameStartChar(U'1'));
    CHECK_FALSE(Utf8Text::IsXmlNameStartChar(U'-'));
    CHECK_FALSE(Utf8Text::IsXmlNameStartChar(U' '));

    // The two gaps inside Latin-1 that no byte-wise test can see, and their
    // neighbours on either side, which are allowed.
    CHECK_FALSE(Utf8Text::IsXmlNameStartChar(kMultiplication));
    CHECK_FALSE(Utf8Text::IsXmlNameStartChar(kDivision));
    CHECK(Utf8Text::IsXmlNameStartChar(kODiaeresis));
    CHECK(Utf8Text::IsXmlNameStartChar(kOStroke));

    // NameChar adds what may follow a start character but not open a name.
    CHECK(Utf8Text::IsXmlNameChar(U'1'));
    CHECK(Utf8Text::IsXmlNameChar(U'-'));
    CHECK(Utf8Text::IsXmlNameChar(U'.'));
    CHECK(Utf8Text::IsXmlNameChar(kMiddleDot));
    CHECK(Utf8Text::IsXmlNameChar(kCombiningAcute));
    CHECK_FALSE(Utf8Text::IsXmlNameChar(U' '));
    CHECK_FALSE(Utf8Text::IsXmlNameChar(kMultiplication));
}

TEST_CASE("An NCName may be written in any script but still has a rule [unit] [utf8-text]")
{
    CHECK(Utf8Text::IsNcName("name"));
    CHECK(Utf8Text::IsNcName("_name-1.2"));
    CHECK(Utf8Text::IsNcName("P\xC5\x99"
                             "ehled")); // r with caron inside
    CHECK(Utf8Text::IsNcName("\xC4\x8D"
                             "islo")); // c with caron at the front
    CHECK(Utf8Text::IsNcName("\xE4\xBD\xA0\xE5\xA5\xBD"));

    CHECK_FALSE(Utf8Text::IsNcName(""));
    CHECK_FALSE(Utf8Text::IsNcName("1name")); // a digit cannot open a name
    CHECK_FALSE(Utf8Text::IsNcName("-name"));
    CHECK_FALSE(Utf8Text::IsNcName("has space"));
    CHECK_FALSE(Utf8Text::IsNcName("a:b")); // the colon is what NC excludes
    CHECK_FALSE(Utf8Text::IsNcName(":name"));
    CHECK_FALSE(Utf8Text::IsNcName("a\xC3\x97"
                                   "b")); // multiplication sign
    CHECK_FALSE(Utf8Text::IsNcName("a\xC4"
                                   "b")); // truncated sequence
    CHECK_FALSE(Utf8Text::IsNcName("\x80"
                                   "name")); // lone continuation byte
}

TEST_CASE("The XML name rule is decided at compile time [unit] [utf8-text]")
{
    // Less a behavioural claim than a guarantee that none of this reaches for a
    // locale or a table while it runs: it would not compile if it did.
    static_assert(Utf8Text::IsNcName("Prehled"));
    static_assert(!Utf8Text::IsNcName("1"));
    static_assert(Utf8Text::Decode("\xC4\x8D", 0).Value == 0x010DU);
    static_assert(!Utf8Text::Decode("\xED\xA0\x80", 0).Valid);
    static_assert(Utf8Text::IsXmlNameStartChar(0x010DU));
    static_assert(!Utf8Text::IsXmlNameStartChar(0x00D7U));
    CHECK(true);
}
