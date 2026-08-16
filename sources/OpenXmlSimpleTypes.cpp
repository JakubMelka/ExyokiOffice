// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "ExyokiOffice/OpenXmlSimpleTypes.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include "Base64.hpp"
#include "AsciiText.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <format>
#include <limits>
#include <string>
#include <string_view>

namespace ExyokiOffice::detail
{
/// File-local parsing and formatting helpers for simple types.
class OpenXmlSimpleTypesHelper
{
public:
    static bool parseUnsigned(std::string_view text, Size pos, Size length, unsigned& value) noexcept
    {
        if (pos + length > text.size())
        {
            return false;
        }

        unsigned parsed = 0;
        for (Size i = 0; i < length; ++i)
        {
            char ch = text[pos + i];
            if (ch < '0' || ch > '9')
            {
                return false;
            }

            parsed = static_cast<unsigned>(parsed * 10 + static_cast<unsigned>(ch - '0'));
        }

        value = parsed;
        return true;
    }

    static bool parseSigned(std::string_view text, Size pos, Size length, int& value) noexcept
    {
        unsigned unsignedValue = 0;
        if (!parseUnsigned(text, pos, length, unsignedValue))
        {
            return false;
        }

        value = static_cast<int>(unsignedValue);
        return true;
    }

    static constexpr Int64 secondsPerDay = 24LL * 60LL * 60LL;
    static constexpr Int64 nanosecondsPerSecond = 1000000000LL;

    // The instants a parsed xsd:dateTime can be handed back as.
    //
    // The clock's duration counts nanoseconds on libstdc++ and libc++, which spans
    // only about the years 1678 to 2262, and 100 ns ticks on the Microsoft standard
    // library, which covers the whole of xsd:dateTime. xsd:dateTime itself runs from
    // year 1 to year 9999, so on the narrow representations a perfectly legal date -
    // `0001-01-01T00:00:00Z`, the conventional null-date sentinel in Office
    // documents, among them - scales to a count no signed 64-bit integer holds. That
    // is undefined behaviour, and a silently wrong instant wherever it does not trap,
    // so the whole-second count is checked against these bounds first and a date the
    // clock cannot carry is rejected rather than mangled. The Format side of the
    // round-trip has its own guard against the same overflow.
    //
    // The second of headroom at each end leaves room for the sub-second remainder
    // that is added after the conversion.
    static constexpr Int64 minRepresentableSecond =
        std::chrono::ceil<std::chrono::seconds>(std::chrono::system_clock::duration::min()).count() + 1;
    static constexpr Int64 maxRepresentableSecond =
        std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::duration::max()).count() - 1;

    static bool isLeapYear(int year) noexcept
    {
        return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
    }

    static unsigned daysInMonth(int year, unsigned month) noexcept
    {
        static constexpr std::array<unsigned, 12> days{
            31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
        if (month == 0 || month > days.size())
        {
            return 0;
        }

        if (month == 2 && isLeapYear(year))
        {
            return 29;
        }

        return days[month - 1];
    }

    static Int64 daysFromCivil(int year, unsigned month, unsigned day) noexcept
    {
        year -= (month <= 2) ? 1 : 0;
        const auto era = (year >= 0 ? year : year - 399) / 400;
        const auto yoe = static_cast<unsigned>(year - era * 400);
        const auto adjustedMonth = static_cast<unsigned>(static_cast<int>(month) + (month > 2 ? -3 : 9));
        const auto doy = (153 * adjustedMonth + 2) / 5 + day - 1;
        const auto doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
        return static_cast<Int64>(era) * 146097 + static_cast<Int64>(doe) - 719468;
    }

    struct CivilDate
    {
        int year = 0;
        unsigned month = 1;
        unsigned day = 1;
    };

    static CivilDate civilFromDays(Int64 days) noexcept
    {
        days += 719468;
        const auto era = (days >= 0 ? days : days - 146096) / 146097;
        const auto doe = static_cast<unsigned>(days - era * 146097);
        const auto yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
        auto year = static_cast<int>(yoe) + era * 400;
        const auto doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
        const auto mp = static_cast<int>((5 * doy + 2) / 153);
        const auto day = doy - static_cast<unsigned>((153 * mp + 2) / 5) + 1;
        const auto month = mp + (mp < 10 ? 3 : -9);

        year += (month <= 2) ? 1 : 0;

        CivilDate date{};
        date.year = static_cast<int>(year);
        date.month = static_cast<unsigned>(month);
        date.day = static_cast<unsigned>(day);
        return date;
    }

    static inline unsigned hexValue(char ch) noexcept
    {
        if (ch >= '0' && ch <= '9')
        {
            return static_cast<unsigned>(ch - '0');
        }

        if (ch >= 'A' && ch <= 'F')
        {
            return static_cast<unsigned>(ch - 'A' + 10);
        }

        if (ch >= 'a' && ch <= 'f')
        {
            return static_cast<unsigned>(ch - 'a' + 10);
        }

        return std::numeric_limits<unsigned>::max();
    }
};

template <typename TInt>
bool IntegralValueTraits<TInt>::TryParse(std::string_view text, TInt& value) noexcept
{
    if (text.empty())
    {
        return false;
    }

    TInt parsed{};
    const auto begin = text.data();
    const auto end = begin + text.size();
    auto result = std::from_chars(begin, end, parsed);
    if (result.ec != std::errc{} || result.ptr != end)
    {
        return false;
    }

    value = parsed;
    return true;
}

template <typename TInt>
std::string IntegralValueTraits<TInt>::Format(TInt value)
{
    char buffer[64]{};
    auto result = std::to_chars(std::begin(buffer), std::end(buffer), value);
    if (result.ec != std::errc{})
    {
        return {};
    }

    return std::string(buffer, static_cast<Size>(result.ptr - buffer));
}

template <typename TFloat>
bool FloatingValueTraits<TFloat>::TryParse(std::string_view text, TFloat& value) noexcept
{
    if (text.empty())
    {
        return false;
    }

    TFloat parsed{};
    const auto begin = text.data();
    const auto end = begin + text.size();
    auto result = std::from_chars(begin, end, parsed, std::chars_format::general);
    if (result.ec != std::errc{} || result.ptr != end)
    {
        return false;
    }

    value = parsed;
    return true;
}

template <typename TFloat>
std::string FloatingValueTraits<TFloat>::Format(TFloat value)
{
    char buffer[128]{};
    auto result =
        std::to_chars(std::begin(buffer), std::end(buffer), value, std::chars_format::general);
    if (result.ec != std::errc{})
    {
        return {};
    }

    return std::string(buffer, static_cast<Size>(result.ptr - buffer));
}

bool IsWhitespace(char ch) noexcept
{
    return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
}

std::vector<std::string_view> SplitWhitespace(std::string_view text)
{
    std::vector<std::string_view> tokens;
    Size start = 0;

    while (start < text.size())
    {
        while (start < text.size() && IsWhitespace(text[start]))
        {
            ++start;
        }

        if (start >= text.size())
        {
            break;
        }

        Size end = start;
        while (end < text.size() && !IsWhitespace(text[end]))
        {
            ++end;
        }

        tokens.emplace_back(text.substr(start, end - start));
        start = end;
    }

    return tokens;
}

bool BooleanTextTraits::TryParse(std::string_view text, bool& value) noexcept
{
    // xsd:boolean has exactly four literals. `True`/`False` are what .NET's
    // bool.ToString() emits, not what the schema admits, so they are rejected here
    // and the attribute reads as unset - the raw text still round-trips untouched.
    if (text == "1" || text == "true")
    {
        value = true;
        return true;
    }

    if (text == "0" || text == "false")
    {
        value = false;
        return true;
    }

    return false;
}

std::string BooleanTextTraits::Format(bool value)
{
    return value ? "1" : "0";
}

bool OnOffTextTraits::TryParse(std::string_view text, bool& value) noexcept
{
    // ST_OnOff is the union of xsd:boolean with ST_OnOff1 (`on`/`off`). Neither
    // branch has an empty member, so `val=""` is not a value of the type: it reads
    // as unset rather than as a definite `false`. Contrast ST_TrueFalseBlank, whose
    // lexical space really does include the blank.
    if (text == "true" || text == "1" || text == "on")
    {
        value = true;
        return true;
    }

    if (text == "false" || text == "0" || text == "off")
    {
        value = false;
        return true;
    }

    return false;
}

std::string OnOffTextTraits::Format(bool value)
{
    return value ? "true" : "false";
}

bool TrueFalseTextTraits::TryParse(std::string_view text, bool& value) noexcept
{
    if (text == "true" || text == "t")
    {
        value = true;
        return true;
    }

    if (text == "false" || text == "f")
    {
        value = false;
        return true;
    }

    return false;
}

std::string TrueFalseTextTraits::Format(bool value)
{
    return value ? "true" : "false";
}

bool TrueFalseBlankTextTraits::TryParse(std::string_view text, bool& value) noexcept
{
    if (text.empty())
    {
        value = false;
        return true;
    }

    if (text == "true" || text == "t")
    {
        value = true;
        return true;
    }

    if (text == "false" || text == "f")
    {
        value = false;
        return true;
    }

    return false;
}

std::string TrueFalseBlankTextTraits::Format(bool value)
{
    return value ? "true" : "false";
}

bool DecimalValueTraits::TryParse(std::string_view text, RealExtended& value) noexcept
{
    if (text.empty())
    {
        return false;
    }

    // std::from_chars rather than std::strtold: the C conversions read the
    // decimal separator from the global C locale, so under de-DE the same
    // document would parse "1.5" as 1 and write 1.5 back as "1,5" - text no
    // conforming reader accepts. from_chars is defined to be locale
    // independent, which is the only correct answer for a serialization format,
    // and it is what every other numeric trait in this file already uses.
    RealExtended parsed{};
    const auto begin = text.data();
    const auto end = begin + text.size();
    const auto result = std::from_chars(begin, end, parsed, std::chars_format::general);
    if (result.ec != std::errc{} || result.ptr != end)
    {
        value = 0.0L;
        return false;
    }

    value = parsed;
    return true;
}

std::string DecimalValueTraits::Format(RealExtended value)
{
    char buffer[128]{};
    const auto result = std::to_chars(std::begin(buffer), std::end(buffer), value, std::chars_format::general);
    if (result.ec != std::errc{})
    {
        return {};
    }

    return std::string(buffer, static_cast<Size>(result.ptr - buffer));
}

bool DateTimeValueTraits::TryParse(std::string_view text, value_type& value) noexcept
{
    if (text.size() < 19)
    {
        return false;
    }

    int year = 0;
    unsigned month = 0;
    unsigned day = 0;
    unsigned hour = 0;
    unsigned minute = 0;
    unsigned second = 0;

    if (!OpenXmlSimpleTypesHelper::parseSigned(text, 0, 4, year) ||
        text[4] != '-' ||
        !OpenXmlSimpleTypesHelper::parseUnsigned(text, 5, 2, month) ||
        text[7] != '-' ||
        !OpenXmlSimpleTypesHelper::parseUnsigned(text, 8, 2, day) ||
        text[10] != 'T' ||
        !OpenXmlSimpleTypesHelper::parseUnsigned(text, 11, 2, hour) ||
        text[13] != ':' ||
        !OpenXmlSimpleTypesHelper::parseUnsigned(text, 14, 2, minute) ||
        text[16] != ':' ||
        !OpenXmlSimpleTypesHelper::parseUnsigned(text, 17, 2, second))
    {
        return false;
    }

    Size index = 19;
    Int64 nanoseconds = 0;

    if (index < text.size() && text[index] == '.')
    {
        ++index;
        Size digits = 0;
        Int64 fraction = 0;

        while (index < text.size() && AsciiText::IsDigit(text[index]))
        {
            if (digits < 9)
            {
                fraction = fraction * 10 + static_cast<Int64>(text[index] - '0');
                ++digits;
            }
            else
            {
                ++digits;
            }

            ++index;
        }

        if (digits == 0)
        {
            return false;
        }

        if (digits > 9)
        {
            digits = 9;
        }

        while (digits < 9)
        {
            fraction *= 10;
            ++digits;
        }

        nanoseconds = fraction;
    }

    int offsetMinutes = 0;
    if (index == text.size())
    {
        offsetMinutes = 0;
    }
    else if (text[index] == 'Z' || text[index] == 'z')
    {
        offsetMinutes = 0;
        ++index;
    }
    else if (text[index] == '+' || text[index] == '-')
    {
        const bool negative = text[index] == '-';
        ++index;

        unsigned offsetHours = 0;
        unsigned offsetMins = 0;
        if (!OpenXmlSimpleTypesHelper::parseUnsigned(text, index, 2, offsetHours))
        {
            return false;
        }

        index += 2;

        if (index >= text.size() || text[index] != ':')
        {
            return false;
        }

        ++index;

        if (!OpenXmlSimpleTypesHelper::parseUnsigned(text, index, 2, offsetMins))
        {
            return false;
        }

        index += 2;
        if (offsetHours > 14 || offsetMins > 59 || (offsetHours == 14 && offsetMins != 0))
        {
            return false;
        }

        offsetMinutes = static_cast<int>(offsetHours * 60 + offsetMins);
        if (negative)
        {
            offsetMinutes = -offsetMinutes;
        }
    }
    else
    {
        return false;
    }

    if (index != text.size())
    {
        return false;
    }

    if (day == 0 || day > OpenXmlSimpleTypesHelper::daysInMonth(year, month) || hour > 23 || minute > 59 ||
        second > 59)
    {
        return false;
    }

    const auto days = OpenXmlSimpleTypesHelper::daysFromCivil(year, month, day);
    auto seconds = static_cast<Int64>(days) * OpenXmlSimpleTypesHelper::secondsPerDay;
    seconds += static_cast<Int64>(hour) * 3600;
    seconds += static_cast<Int64>(minute) * 60;
    seconds += static_cast<Int64>(second);
    seconds -= static_cast<Int64>(offsetMinutes) * 60;

    if (seconds < OpenXmlSimpleTypesHelper::minRepresentableSecond || seconds > OpenXmlSimpleTypesHelper::maxRepresentableSecond)
    {
        return false;
    }

    const auto totalSeconds = std::chrono::seconds(seconds);
    const auto fractional = std::chrono::nanoseconds(nanoseconds);
    const auto duration =
        std::chrono::duration_cast<std::chrono::system_clock::duration>(totalSeconds) +
        std::chrono::duration_cast<std::chrono::system_clock::duration>(fractional);

    value = value_type(duration);
    return true;
}

std::string DateTimeValueTraits::Format(const value_type& value)
{
    // The whole duration must never be converted to nanoseconds: that count is
    // a 64-bit integer, so it only spans roughly the years 1678 to 2262, and
    // anything outside silently overflows. xsd:dateTime reaches from year 1 to
    // year 9999, and `0001-01-01T00:00:00Z` in particular is a common null-date
    // sentinel in Office documents.
    //
    // Flooring to whole seconds first keeps the large value in the clock's own
    // representation, and only the sub-second remainder - always less than one
    // second - is converted to nanoseconds. floor rather than duration_cast so
    // that pre-epoch times truncate downwards and the remainder stays positive.
    const auto sinceEpoch = value.time_since_epoch();
    const auto wholeSeconds = std::chrono::floor<std::chrono::seconds>(sinceEpoch);

    Int64 seconds = wholeSeconds.count();
    Int64 nanos =
        std::chrono::duration_cast<std::chrono::nanoseconds>(sinceEpoch - wholeSeconds).count();

    const auto days =
        seconds >= 0 ? seconds / OpenXmlSimpleTypesHelper::secondsPerDay : (seconds - (OpenXmlSimpleTypesHelper::secondsPerDay - 1)) / OpenXmlSimpleTypesHelper::secondsPerDay;
    Int64 daySeconds = seconds - days * OpenXmlSimpleTypesHelper::secondsPerDay;

    auto date = OpenXmlSimpleTypesHelper::civilFromDays(days);

    const unsigned hour = static_cast<unsigned>(daySeconds / 3600);
    daySeconds %= 3600;
    const unsigned minute = static_cast<unsigned>(daySeconds / 60);
    const unsigned second = static_cast<unsigned>(daySeconds % 60);

    // std::format, not snprintf: the format string is checked at compile time
    // against the argument types, and no part of the result depends on a locale
    // the hosting application may have installed.
    std::string result = std::format("{:04}-{:02}-{:02}T{:02}:{:02}:{:02}",
                                     date.year,
                                     date.month,
                                     date.day,
                                     hour,
                                     minute,
                                     second);

    if (nanos > 0)
    {
        // The remainder of flooring to whole seconds is always below one
        // second, but nothing in the type says so: the modulo is what tells the
        // compiler that nine digits are the most this can print, instead of the
        // twenty a 64-bit count would need.
        const auto nanosOfSecond = static_cast<unsigned>(nanos % OpenXmlSimpleTypesHelper::nanosecondsPerSecond);

        std::string fractional = std::format("{:09}", nanosOfSecond);
        while (!fractional.empty() && fractional.back() == '0')
        {
            fractional.pop_back();
        }

        result.push_back('.');
        result.append(fractional);
    }

    result.push_back('Z');
    return result;
}

bool HexBinaryTraits::TryParse(std::string_view text, std::vector<Byte>& value) noexcept
{
    value.clear();

    if (text.empty())
    {
        return true;
    }

    if (text.size() % 2 != 0)
    {
        return false;
    }

    value.reserve(text.size() / 2);

    for (Size i = 0; i < text.size(); i += 2)
    {
        const unsigned high = OpenXmlSimpleTypesHelper::hexValue(text[i]);
        const unsigned low = OpenXmlSimpleTypesHelper::hexValue(text[i + 1]);

        if (high == std::numeric_limits<unsigned>::max() ||
            low == std::numeric_limits<unsigned>::max())
        {
            value.clear();
            return false;
        }

        value.push_back(static_cast<UInt8>((high << 4) | low));
    }

    return true;
}

std::string HexBinaryTraits::Format(const std::vector<Byte>& value)
{
    if (value.empty())
    {
        return {};
    }

    static constexpr char digits[] = "0123456789ABCDEF";
    std::string text;
    text.resize(value.size() * 2);

    for (Size i = 0; i < value.size(); ++i)
    {
        text[2 * i] = digits[(value[i] >> 4) & 0x0F];
        text[2 * i + 1] = digits[value[i] & 0x0F];
    }

    return text;
}

bool Base64BinaryTraits::TryParse(std::string_view text, std::vector<Byte>& value) noexcept
{
    // `base64Binary` is defined by the schema, so the lexical space is the
    // strict one: whole quads, padding only at the end.
    return Base64::Decode(text, value, Base64::Padding::Required);
}

std::string Base64BinaryTraits::Format(const std::vector<Byte>& value)
{
    return Base64::Encode(value);
}

template struct IntegralValueTraits<Int8>;
template struct IntegralValueTraits<UInt8>;
template struct IntegralValueTraits<Int16>;
template struct IntegralValueTraits<UInt16>;
template struct IntegralValueTraits<Int32>;
template struct IntegralValueTraits<UInt32>;
template struct IntegralValueTraits<Int64>;
template struct IntegralValueTraits<UInt64>;

template struct FloatingValueTraits<Single>;
template struct FloatingValueTraits<Real>;

} // namespace ExyokiOffice::detail

namespace ExyokiOffice
{

StringValue::StringValue() = default;

StringValue::StringValue(const char* value)
{
    if (value != nullptr)
    {
        Assign(std::string(value));
    }
}

StringValue::StringValue(std::string value) noexcept
{
    Assign(std::move(value));
}

StringValue::StringValue(std::string_view value) noexcept
{
    AssignView(value);
}

bool StringValue::IsDefined() const noexcept
{
    return m_storage != Storage::Undefined;
}

bool StringValue::IsView() const noexcept
{
    return m_storage == Storage::View;
}

bool StringValue::IsOwned() const noexcept
{
    return m_storage == Storage::Owned;
}

void StringValue::Reset() noexcept
{
    m_storage = Storage::Undefined;
    m_owned.clear();
    m_view = std::string_view();
}

bool StringValue::Assign(std::string value) noexcept
{
    m_owned = std::move(value);
    m_view = m_owned;
    m_storage = Storage::Owned;
    return true;
}

bool StringValue::AssignView(std::string_view view) noexcept
{
    m_view = view;
    m_owned.clear();
    m_storage = Storage::View;
    return true;
}

bool StringValue::AssignFromString(std::string_view text) noexcept
{
    return Assign(std::string(text));
}

std::string_view StringValue::View() const noexcept
{
    if (!IsDefined())
    {
        return {};
    }

    if (IsOwned())
    {
        return m_owned;
    }

    return m_view;
}

std::string StringValue::ToString() const
{
    if (!IsDefined())
    {
        return {};
    }

    if (m_storage == Storage::Owned)
    {
        return m_owned;
    }

    return std::string(m_view);
}

bool operator==(const StringValue& left, const StringValue& right) noexcept
{
    if (!left.IsDefined() && !right.IsDefined())
    {
        return true;
    }

    if (left.IsDefined() != right.IsDefined())
    {
        return false;
    }

    return left.View() == right.View();
}

bool operator!=(const StringValue& left, const StringValue& right) noexcept
{
    return !(left == right);
}

} // namespace ExyokiOffice
