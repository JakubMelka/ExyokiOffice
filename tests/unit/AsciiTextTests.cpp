// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "AsciiText.hpp"
#include "Base64.hpp"

#include <doctest/doctest.h>

#include <clocale>
// doctest prints a failing comparison, and Trim returns a std::string_view,
// whose operator<< needs the complete std::ostream.
#include <ostream>
#include <string>
#include <vector>

using namespace ExyokiOffice;

namespace
{

/// Sets a locale for the body of a test and puts "C" back afterwards.
class ScopedLocale
{
public:
    explicit ScopedLocale(const char* name) : m_applied(std::setlocale(LC_ALL, name) != nullptr) {}

    ~ScopedLocale() { std::setlocale(LC_ALL, "C"); }

    ScopedLocale(const ScopedLocale&) = delete;
    ScopedLocale& operator=(const ScopedLocale&) = delete;

    /// False when the platform does not ship the locale, so the test can skip.
    [[nodiscard]] bool Applied() const noexcept { return m_applied; }

private:
    bool m_applied;
};

} // namespace

TEST_CASE("ASCII case folding covers the letters and leaves everything else alone [unit] [ascii-text]")
{
    CHECK(AsciiText::ToLower('A') == 'a');
    CHECK(AsciiText::ToLower('z') == 'z');
    CHECK(AsciiText::ToUpper('a') == 'A');
    CHECK(AsciiText::ToUpper('Z') == 'Z');

    // Digits, punctuation and the bytes of a UTF-8 sequence are not letters
    // this function knows, so they survive unchanged.
    CHECK(AsciiText::ToLower('7') == '7');
    CHECK(AsciiText::ToUpper('_') == '_');
    CHECK(AsciiText::ToLower('\x80') == '\x80');
    CHECK(AsciiText::ToUpper(static_cast<char>(0xC3)) == static_cast<char>(0xC3));

    CHECK(AsciiText::ToLower(std::string_view("Wordprocessing ML")) == "wordprocessing ml");
    CHECK(AsciiText::ToUpper(std::string_view("docProps/core.xml")) == "DOCPROPS/CORE.XML");

    // A two-byte 'č' passes through both directions untouched.
    const std::string accented = "\xC4\x8D";
    CHECK(AsciiText::ToLower(std::string_view(accented)) == accented);
    CHECK(AsciiText::ToUpper(std::string_view(accented)) == accented);
}

TEST_CASE("Case-insensitive comparison matches on ASCII only [unit] [ascii-text]")
{
    CHECK(AsciiText::EqualsIgnoreCase("Relationship", "RELATIONSHIP"));
    CHECK(AsciiText::EqualsIgnoreCase("", ""));
    CHECK_FALSE(AsciiText::EqualsIgnoreCase("sheet1", "sheet10"));
    CHECK_FALSE(AsciiText::EqualsIgnoreCase("alpha", "beta"));

    CHECK(AsciiText::StartsWithIgnoreCase("IMAGE/PNG", "image/"));
    CHECK_FALSE(AsciiText::StartsWithIgnoreCase("image", "image/png"));

    // constexpr, so a mistake here is a compile error rather than a test failure
    static_assert(AsciiText::EqualsIgnoreCase("Xml", "xml"));
    static_assert(!AsciiText::EqualsIgnoreCase("Xml", "xm"));
}

TEST_CASE("Case folding does not follow the global C locale [unit] [ascii-text]")
{
    // The Turkish locale folds 'I' to a dotless 'i', which would stop a field
    // instruction such as TITLE from being recognized. A hosting application
    // may set it at any time, so the library must not consult it.
    const ScopedLocale turkish("tr_TR.UTF-8");
    if (!turkish.Applied())
    {
        MESSAGE("tr_TR.UTF-8 is not installed; the locale independence check did not run");
        return;
    }

    CHECK(AsciiText::ToLower('I') == 'i');
    CHECK(AsciiText::ToUpper('i') == 'I');
    CHECK(AsciiText::EqualsIgnoreCase("TITLE", "title"));
    CHECK(AsciiText::ToLower(std::string_view("INCLUDEPICTURE")) == "includepicture");
}

TEST_CASE("Trimming removes the ASCII whitespace on both ends [unit] [ascii-text]")
{
    CHECK(AsciiText::Trim("  padded  ") == "padded");
    CHECK(AsciiText::Trim("\t\r\n\v\f value \f\v\n\r\t") == "value");
    CHECK(AsciiText::Trim("inner space") == "inner space");
    CHECK(AsciiText::Trim("").empty());
    CHECK(AsciiText::Trim("   ").empty());
    CHECK(AsciiText::Trim("nothing-to-do") == "nothing-to-do");
}

TEST_CASE("Base64 encoding follows the standard alphabet and padding [unit] [ascii-text]")
{
    const auto encode = [](std::string_view text)
    {
        const std::vector<Byte> bytes(text.begin(), text.end());
        return Base64::Encode(bytes);
    };

    CHECK(encode("") == "");
    CHECK(encode("f") == "Zg==");
    CHECK(encode("fo") == "Zm8=");
    CHECK(encode("foo") == "Zm9v");
    CHECK(encode("foob") == "Zm9vYg==");
    CHECK(encode("fooba") == "Zm9vYmE=");
    CHECK(encode("foobar") == "Zm9vYmFy");
}

TEST_CASE("Strict base64 decoding accepts only well-formed padding [unit] [ascii-text]")
{
    std::vector<Byte> value;
    const auto decode = [&value](std::string_view text)
    { return Base64::Decode(text, value, Base64::Padding::Required); };
    const auto text = [&value]
    { return std::string(value.begin(), value.end()); };

    CHECK(decode("Zm9vYmFy"));
    CHECK(text() == "foobar");
    CHECK(decode("Zg=="));
    CHECK(text() == "f");

    // Whitespace is how an XML producer wraps a long value across lines.
    CHECK(decode("Zm9v\n  YmFy"));
    CHECK(text() == "foobar");

    CHECK(decode(""));
    CHECK(value.empty());

    // Rejected: a partial quad, padding away from the end, a lone '=' in the
    // third position, and a character outside the alphabet.
    CHECK_FALSE(decode("Zm9vYg"));
    CHECK_FALSE(decode("Zg==Zg=="));
    CHECK_FALSE(decode("Zm9=YmFy"));
    CHECK_FALSE(decode("Zm9v*mFy"));

    // A rejected value leaves nothing behind that could pass for a decode.
    CHECK(value.empty());
}

TEST_CASE("Lenient base64 decoding accepts a missing final pad [unit] [ascii-text]")
{
    std::vector<Byte> value;
    const auto decode = [&value](std::string_view text)
    { return Base64::Decode(text, value, Base64::Padding::Optional); };
    const auto text = [&value]
    { return std::string(value.begin(), value.end()); };

    CHECK(decode("Zm9vYg"));
    CHECK(text() == "foob");
    CHECK(decode("Zm9vYg=="));
    CHECK(text() == "foob");

    // Still not anything: content after the padding, or a foreign character.
    CHECK_FALSE(decode("Zg==Zg=="));
    CHECK_FALSE(decode("Zm9v*mFy"));
    CHECK(value.empty());
}

TEST_CASE("Base64 survives a round trip over every byte value [unit] [ascii-text]")
{
    std::vector<Byte> original;
    original.reserve(256);
    for (int index = 0; index < 256; ++index)
    {
        original.push_back(static_cast<Byte>(index));
    }

    for (Size length = 0; length <= original.size(); ++length)
    {
        const std::vector<Byte> input(original.begin(), original.begin() + static_cast<std::ptrdiff_t>(length));
        std::vector<Byte> decoded;
        REQUIRE(Base64::Decode(Base64::Encode(input), decoded, Base64::Padding::Required));
        CHECK(decoded == input);
    }
}
