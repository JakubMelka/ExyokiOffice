// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "NumberFormatText.hpp"

#include "FormulaEvaluator.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace ExyokiOffice::Excel
{

namespace NumberFormatDetail
{

char AsciiLower(char c)
{
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

bool EqualsIgnoreCase(std::string_view left, std::string_view right)
{
    if (left.size() != right.size())
    {
        return false;
    }
    for (Size i = 0; i < left.size(); ++i)
    {
        if (AsciiLower(left[i]) != AsciiLower(right[i]))
        {
            return false;
        }
    }
    return true;
}

/** One lexical token of a format section. */
struct FormatToken
{
    enum class Kind
    {
        Digit,        // 0, #, ?
        DecimalPoint, // .
        Percent,      // %
        Exponent,     // E+00 / E-00
        Literal,      // quoted or escaped text, safe punctuation
        TextValue,    // @
        DateTime,     // yyyy, mm, d, hh, s, ...
        AmPm,         // AM/PM
        ElapsedHours, // [h]
        ThousandsScale
    };

    Kind kind = Kind::Literal;
    char digit = '#';
    std::string text;
    /** Date-time token letter and repetition count, e.g. ('m', 2) for `mm`. */
    char dateTimeUnit = '\0';
    int repeat = 0;
    bool exponentPlus = true;
    int exponentDigits = 2;
};

struct FormatSection
{
    std::vector<FormatToken> tokens;
    bool hasDigits = false;
    bool hasDateTime = false;
    bool hasTextPlaceholder = false;
    bool hasPercent = false;
    bool hasThousandsSeparator = false;
    int thousandsScaleCount = 0;
    bool hasExponent = false;
    bool isGeneral = false;
    int integerPlaceholders = 0;
    int decimalPlaceholders = 0;
    int exponentDigits = 2;
    bool exponentPlus = true;
};

const std::string_view MonthNames[12] = {"January", "February", "March", "April", "May", "June",
                                         "July", "August", "September", "October", "November", "December"};
const std::string_view DayNames[7] = {"Sunday", "Monday", "Tuesday", "Wednesday",
                                      "Thursday", "Friday", "Saturday"};

/** Parses one section; returns false for unsupported codes. */
bool ParseSection(std::string_view code, FormatSection& section)
{
    if (code.empty() || EqualsIgnoreCase(code, "General"))
    {
        section.isGeneral = true;
        return true;
    }

    // First pass: raw tokens.
    Size i = 0;
    bool afterDecimal = false;
    bool sawDigitSinceSeparator = false;
    while (i < code.size())
    {
        const char c = code[i];
        switch (c)
        {
            case '0':
            case '#':
            case '?':
            {
                FormatToken token;
                token.kind = FormatToken::Kind::Digit;
                token.digit = c;
                section.tokens.push_back(token);
                section.hasDigits = true;
                if (afterDecimal)
                {
                    ++section.decimalPlaceholders;
                }
                else
                {
                    ++section.integerPlaceholders;
                }
                sawDigitSinceSeparator = true;
                ++i;
                continue;
            }
            case '.':
            {
                if (afterDecimal)
                {
                    // Later dots are literal characters (common in date codes).
                    FormatToken token;
                    token.kind = FormatToken::Kind::Literal;
                    token.text = ".";
                    section.tokens.push_back(token);
                    ++i;
                    continue;
                }
                afterDecimal = true;
                FormatToken token;
                token.kind = FormatToken::Kind::DecimalPoint;
                section.tokens.push_back(token);
                ++i;
                continue;
            }
            case ',':
            {
                // A comma between digit placeholders enables thousands
                // grouping; trailing commas scale by 1000 each.
                Size j = i;
                while (j < code.size() && code[j] == ',')
                {
                    ++j;
                }
                const bool digitsFollow = j < code.size() && (code[j] == '0' || code[j] == '#' || code[j] == '?');
                if (!afterDecimal && sawDigitSinceSeparator && digitsFollow)
                {
                    section.hasThousandsSeparator = true;
                }
                else if (sawDigitSinceSeparator && !digitsFollow)
                {
                    section.thousandsScaleCount += static_cast<int>(j - i);
                }
                else
                {
                    FormatToken token;
                    token.kind = FormatToken::Kind::Literal;
                    token.text = std::string(j - i, ',');
                    section.tokens.push_back(token);
                }
                i = j;
                continue;
            }
            case '%':
            {
                FormatToken token;
                token.kind = FormatToken::Kind::Percent;
                section.tokens.push_back(token);
                section.hasPercent = true;
                ++i;
                continue;
            }
            case 'E':
            case 'e':
            {
                if (i + 1 < code.size() && (code[i + 1] == '+' || code[i + 1] == '-'))
                {
                    FormatToken token;
                    token.kind = FormatToken::Kind::Exponent;
                    token.exponentPlus = code[i + 1] == '+';
                    Size j = i + 2;
                    int digits = 0;
                    while (j < code.size() && code[j] == '0')
                    {
                        ++digits;
                        ++j;
                    }
                    token.exponentDigits = std::max(digits, 1);
                    section.tokens.push_back(token);
                    section.hasExponent = true;
                    section.exponentDigits = token.exponentDigits;
                    section.exponentPlus = token.exponentPlus;
                    i = j;
                    continue;
                }
                return false;
            }
            case '@':
            {
                FormatToken token;
                token.kind = FormatToken::Kind::TextValue;
                section.tokens.push_back(token);
                section.hasTextPlaceholder = true;
                ++i;
                continue;
            }
            case '"':
            {
                Size j = i + 1;
                std::string literal;
                while (j < code.size() && code[j] != '"')
                {
                    literal.push_back(code[j]);
                    ++j;
                }
                if (j >= code.size())
                {
                    return false;
                }
                FormatToken token;
                token.kind = FormatToken::Kind::Literal;
                token.text = std::move(literal);
                section.tokens.push_back(token);
                i = j + 1;
                continue;
            }
            case '\\':
            {
                if (i + 1 >= code.size())
                {
                    return false;
                }
                FormatToken token;
                token.kind = FormatToken::Kind::Literal;
                token.text.assign(1, code[i + 1]);
                section.tokens.push_back(token);
                i += 2;
                continue;
            }
            case '_':
            {
                // Width-of-character padding renders as a single space.
                if (i + 1 >= code.size())
                {
                    return false;
                }
                FormatToken token;
                token.kind = FormatToken::Kind::Literal;
                token.text = " ";
                section.tokens.push_back(token);
                i += 2;
                continue;
            }
            case '[':
            {
                // Only the elapsed-hours token is supported; colors and
                // conditions are not.
                if (i + 2 < code.size() && AsciiLower(code[i + 1]) == 'h' && code[i + 2] == ']')
                {
                    FormatToken token;
                    token.kind = FormatToken::Kind::ElapsedHours;
                    section.tokens.push_back(token);
                    section.hasDateTime = true;
                    i += 3;
                    continue;
                }
                return false;
            }
            case '*':
            case '/':
                // Fill tokens and fraction formats are unsupported.
                return false;
            case 'A':
            case 'a':
            {
                if (code.substr(i).size() >= 5 && EqualsIgnoreCase(code.substr(i, 5), "AM/PM"))
                {
                    FormatToken token;
                    token.kind = FormatToken::Kind::AmPm;
                    section.tokens.push_back(token);
                    section.hasDateTime = true;
                    i += 5;
                    continue;
                }
                if (code.substr(i).size() >= 3 && EqualsIgnoreCase(code.substr(i, 3), "A/P"))
                {
                    FormatToken token;
                    token.kind = FormatToken::Kind::AmPm;
                    token.repeat = 1; // short form
                    section.tokens.push_back(token);
                    section.hasDateTime = true;
                    i += 3;
                    continue;
                }
                return false;
            }
            case 'y':
            case 'Y':
            case 'm':
            case 'M':
            case 'd':
            case 'D':
            case 'h':
            case 'H':
            case 's':
            case 'S':
            {
                const char unit = AsciiLower(c);
                Size j = i;
                while (j < code.size() && AsciiLower(code[j]) == unit)
                {
                    ++j;
                }
                FormatToken token;
                token.kind = FormatToken::Kind::DateTime;
                token.dateTimeUnit = unit;
                token.repeat = static_cast<int>(j - i);
                section.tokens.push_back(token);
                section.hasDateTime = true;
                i = j;
                continue;
            }
            case ' ':
            case '-':
            case '+':
            case '(':
            case ')':
            case ':':
            case '$':
            case '!':
            case '^':
            case '&':
            case '\'':
            case '~':
            case '{':
            case '}':
            case '<':
            case '>':
            case '=':
            {
                FormatToken token;
                token.kind = FormatToken::Kind::Literal;
                token.text.assign(1, c);
                section.tokens.push_back(token);
                ++i;
                continue;
            }
            default:
                return false;
        }
    }

    // Disambiguate months and minutes: an `m` token directly after an hour
    // token or directly before a second token means minutes.
    for (Size index = 0; index < section.tokens.size(); ++index)
    {
        FormatToken& token = section.tokens[index];
        if (token.kind != FormatToken::Kind::DateTime || token.dateTimeUnit != 'm')
        {
            continue;
        }
        bool isMinutes = false;
        for (Size back = index; back-- > 0;)
        {
            const FormatToken& previous = section.tokens[back];
            if (previous.kind == FormatToken::Kind::DateTime || previous.kind == FormatToken::Kind::ElapsedHours)
            {
                isMinutes = previous.dateTimeUnit == 'h' ||
                            previous.kind == FormatToken::Kind::ElapsedHours;
                break;
            }
        }
        if (!isMinutes)
        {
            for (Size forward = index + 1; forward < section.tokens.size(); ++forward)
            {
                const FormatToken& next = section.tokens[forward];
                if (next.kind == FormatToken::Kind::DateTime)
                {
                    isMinutes = next.dateTimeUnit == 's';
                    break;
                }
            }
        }
        if (isMinutes)
        {
            token.dateTimeUnit = 'n'; // internal marker for minutes
        }
    }
    return true;
}

std::string PadNumber(UInt64 value, int width)
{
    std::string digits = std::to_string(value);
    while (static_cast<int>(digits.size()) < width)
    {
        digits.insert(digits.begin(), '0');
    }
    return digits;
}

/** Renders the numeric digit placeholders of a section. */
void RenderNumericTokens(const FormatSection& section, Real value, std::string& output)
{
    bool negative = std::signbit(value) && value != 0.0;
    Real magnitude = std::fabs(value);

    if (section.hasPercent)
    {
        magnitude *= 100.0;
    }
    for (int scale = 0; scale < section.thousandsScaleCount; ++scale)
    {
        magnitude /= 1000.0;
    }

    int exponent = 0;
    if (section.hasExponent)
    {
        const int mantissaDigits = std::max(section.integerPlaceholders, 1);
        if (magnitude != 0.0)
        {
            exponent = static_cast<int>(std::floor(std::log10(magnitude)));
            // Normalize the mantissa to the requested integer digit count.
            exponent = exponent - (mantissaDigits - 1);
            magnitude /= std::pow(10.0, exponent);
        }
    }

    // Round to the requested number of decimals.
    const Real scale = std::pow(10.0, section.decimalPlaceholders);
    Real rounded = std::floor(magnitude * scale + 0.5) / scale;
    if (section.hasExponent && rounded >= std::pow(10.0, std::max(section.integerPlaceholders, 1)))
    {
        rounded /= 10.0;
        ++exponent;
    }
    if (rounded == 0.0)
    {
        negative = false;
    }

    char buffer[64]{};
    std::snprintf(buffer, sizeof(buffer), "%.*f", section.decimalPlaceholders, rounded);
    std::string text(buffer);
    std::string integerPart = text;
    std::string decimalPart;
    if (const Size dot = text.find('.'); dot != std::string::npos)
    {
        integerPart = text.substr(0, dot);
        decimalPart = text.substr(dot + 1);
    }
    if (integerPart == "0" && section.integerPlaceholders == 0)
    {
        integerPart.clear();
    }

    if (section.hasThousandsSeparator && integerPart.size() > 3)
    {
        std::string grouped;
        int count = 0;
        for (Size index = integerPart.size(); index-- > 0;)
        {
            grouped.insert(grouped.begin(), integerPart[index]);
            if (++count == 3 && index > 0)
            {
                grouped.insert(grouped.begin(), ',');
                count = 0;
            }
        }
        integerPart = std::move(grouped);
    }

    // Pad the integer part to the number of '0'/'?' placeholders.
    int mandatoryInteger = 0;
    bool afterDecimal = false;
    for (const FormatToken& token : section.tokens)
    {
        if (token.kind == FormatToken::Kind::DecimalPoint)
        {
            afterDecimal = true;
        }
        if (token.kind == FormatToken::Kind::Digit && !afterDecimal && token.digit != '#')
        {
            ++mandatoryInteger;
        }
    }
    while (static_cast<int>(integerPart.size()) -
               static_cast<int>(std::count(integerPart.begin(), integerPart.end(), ',')) <
           mandatoryInteger)
    {
        integerPart.insert(integerPart.begin(), '0');
    }

    // Trim optional trailing decimal placeholders.
    int mandatoryDecimals = 0;
    afterDecimal = false;
    int decimalIndex = 0;
    std::vector<char> decimalKinds;
    for (const FormatToken& token : section.tokens)
    {
        if (token.kind == FormatToken::Kind::DecimalPoint)
        {
            afterDecimal = true;
            continue;
        }
        if (token.kind == FormatToken::Kind::Digit && afterDecimal)
        {
            decimalKinds.push_back(token.digit);
            if (token.digit != '#')
            {
                mandatoryDecimals = decimalIndex + 1;
            }
            ++decimalIndex;
        }
    }
    while (static_cast<int>(decimalPart.size()) > mandatoryDecimals && !decimalPart.empty() &&
           decimalPart.back() == '0')
    {
        decimalPart.pop_back();
    }

    if (negative)
    {
        output.push_back('-');
    }

    bool wroteInteger = false;
    bool wroteDecimalPoint = false;
    for (const FormatToken& token : section.tokens)
    {
        switch (token.kind)
        {
            case FormatToken::Kind::Digit:
                if (!wroteDecimalPoint && !wroteInteger)
                {
                    output += integerPart;
                    wroteInteger = true;
                }
                break;
            case FormatToken::Kind::DecimalPoint:
                if (!wroteInteger)
                {
                    output += integerPart;
                    wroteInteger = true;
                }
                if (!decimalPart.empty())
                {
                    output.push_back('.');
                    output += decimalPart;
                }
                wroteDecimalPoint = true;
                break;
            case FormatToken::Kind::Percent:
                output.push_back('%');
                break;
            case FormatToken::Kind::Exponent:
            {
                if (!wroteInteger)
                {
                    output += integerPart;
                    wroteInteger = true;
                }
                output.push_back('E');
                if (exponent < 0)
                {
                    output.push_back('-');
                }
                else if (token.exponentPlus)
                {
                    output.push_back('+');
                }
                output += PadNumber(static_cast<UInt64>(std::abs(exponent)), token.exponentDigits);
                break;
            }
            case FormatToken::Kind::Literal:
                output += token.text;
                break;
            default:
                break;
        }
    }
    if (!wroteInteger)
    {
        output += integerPart;
    }
}

std::optional<std::string> RenderDateTimeTokens(const FormatSection& section, Real value)
{
    const auto parts = ExcelDateSerial::ToParts(value);
    if (!parts)
    {
        return std::nullopt;
    }

    bool twelveHour = false;
    for (const FormatToken& token : section.tokens)
    {
        if (token.kind == FormatToken::Kind::AmPm)
        {
            twelveHour = true;
        }
    }

    std::string output;
    for (const FormatToken& token : section.tokens)
    {
        switch (token.kind)
        {
            case FormatToken::Kind::Literal:
                output += token.text;
                break;
            case FormatToken::Kind::DecimalPoint:
                // Dots between date tokens are literal separators.
                output.push_back('.');
                break;
            case FormatToken::Kind::AmPm:
            {
                const bool pm = parts->hour >= 12;
                if (token.repeat == 1)
                {
                    output += pm ? "P" : "A";
                }
                else
                {
                    output += pm ? "PM" : "AM";
                }
                break;
            }
            case FormatToken::Kind::ElapsedHours:
            {
                const auto totalHours = static_cast<UInt64>(std::floor(value * 24.0));
                output += std::to_string(totalHours);
                break;
            }
            case FormatToken::Kind::DateTime:
            {
                switch (token.dateTimeUnit)
                {
                    case 'y':
                        if (token.repeat <= 2)
                        {
                            output += PadNumber(static_cast<UInt64>(parts->year % 100), 2);
                        }
                        else
                        {
                            output += std::to_string(parts->year);
                        }
                        break;
                    case 'm': // months
                        if (token.repeat >= 4)
                        {
                            output += MonthNames[parts->month - 1];
                        }
                        else if (token.repeat == 3)
                        {
                            output += MonthNames[parts->month - 1].substr(0, 3);
                        }
                        else
                        {
                            output += PadNumber(parts->month, token.repeat >= 2 ? 2 : 1);
                        }
                        break;
                    case 'n': // minutes
                        output += PadNumber(parts->minute, token.repeat >= 2 ? 2 : 1);
                        break;
                    case 'd':
                    {
                        if (token.repeat >= 3)
                        {
                            // The 1900 date system counts serial 1 as a Sunday.
                            const auto days = static_cast<Int64>(std::floor(value));
                            const Int64 weekdayIndex = ((days - 1) % 7 + 7) % 7;
                            const std::string_view name = DayNames[weekdayIndex];
                            output += token.repeat == 3 ? std::string(name.substr(0, 3)) : std::string(name);
                        }
                        else
                        {
                            output += PadNumber(parts->day, token.repeat >= 2 ? 2 : 1);
                        }
                        break;
                    }
                    case 'h':
                    {
                        UInt32 hour = parts->hour;
                        if (twelveHour)
                        {
                            hour = hour % 12;
                            if (hour == 0)
                            {
                                hour = 12;
                            }
                        }
                        output += PadNumber(hour, token.repeat >= 2 ? 2 : 1);
                        break;
                    }
                    case 's':
                        output += PadNumber(static_cast<UInt32>(parts->second),
                                            token.repeat >= 2 ? 2 : 1);
                        break;
                    default:
                        return std::nullopt;
                }
                break;
            }
            default:
                // Digit placeholders inside date sections are unsupported.
                return std::nullopt;
        }
    }
    return output;
}

std::vector<std::string_view> SplitSections(std::string_view code)
{
    std::vector<std::string_view> sections;
    Size start = 0;
    bool inQuotes = false;
    for (Size i = 0; i < code.size(); ++i)
    {
        const char c = code[i];
        if (c == '"')
        {
            inQuotes = !inQuotes;
        }
        else if (c == '\\' && !inQuotes)
        {
            ++i;
        }
        else if (c == ';' && !inQuotes)
        {
            sections.push_back(code.substr(start, i - start));
            start = i + 1;
        }
    }
    sections.push_back(code.substr(start));
    return sections;
}

} // namespace NumberFormatDetail

std::optional<std::string> NumberFormatText::FormatNumber(Real value, std::string_view formatCode)
{
    using namespace NumberFormatDetail;

    const auto sections = SplitSections(formatCode);
    if (sections.size() > 4)
    {
        return std::nullopt;
    }

    // Section selection: positive;negative;zero.
    std::string_view selected = sections[0];
    bool negativeHandledBySection = false;
    if (value < 0.0 && sections.size() >= 2)
    {
        selected = sections[1];
        negativeHandledBySection = true;
    }
    else if (value == 0.0 && sections.size() >= 3)
    {
        selected = sections[2];
    }

    FormatSection section;
    if (!ParseSection(selected, section))
    {
        return std::nullopt;
    }
    if (section.isGeneral)
    {
        return FormulaCoercion::FormatNumber(value);
    }
    if (section.hasTextPlaceholder)
    {
        return std::nullopt;
    }
    if (section.hasDateTime)
    {
        if (section.hasDigits || value < 0.0)
        {
            return std::nullopt;
        }
        return RenderDateTimeTokens(section, value);
    }

    std::string output;
    RenderNumericTokens(section, negativeHandledBySection ? std::fabs(value) : value, output);
    return output;
}

std::optional<std::string> NumberFormatText::FormatText(std::string_view text, std::string_view formatCode)
{
    using namespace NumberFormatDetail;

    const auto sections = SplitSections(formatCode);
    if (sections.size() > 4)
    {
        return std::nullopt;
    }
    // The fourth section formats text; otherwise a lone `@` section applies.
    std::string_view selected;
    if (sections.size() == 4)
    {
        selected = sections[3];
    }
    else
    {
        selected = sections[0];
    }

    FormatSection section;
    if (!ParseSection(selected, section))
    {
        return std::nullopt;
    }
    if (section.isGeneral)
    {
        return std::string(text);
    }
    if (!section.hasTextPlaceholder)
    {
        // Excel returns text values unchanged when the format has no
        // text section.
        return std::string(text);
    }

    std::string output;
    for (const FormatToken& token : section.tokens)
    {
        switch (token.kind)
        {
            case FormatToken::Kind::TextValue:
                output += text;
                break;
            case FormatToken::Kind::Literal:
                output += token.text;
                break;
            default:
                return std::nullopt;
        }
    }
    return output;
}

} // namespace ExyokiOffice::Excel
