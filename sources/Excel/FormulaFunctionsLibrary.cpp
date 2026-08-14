// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "FormulaFunctions.hpp"
#include "FormulaParser.hpp"
#include "NumberFormatText.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include "AsciiText.hpp"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <utility>

namespace ExyokiOffice::Excel
{

namespace FormulaLibraryDetail
{

using Helpers = FormulaFunctionHelpers;

constexpr Size ExcelMaxTextLength = 32767;

void Add(FormulaFunctionLibrary::FunctionMap& functions,
         std::string name,
         Size minimumArguments,
         Size maximumArguments,
         InternalFormulaFunction function,
         bool isVolatile = false)
{
    RegisteredFormulaFunction entry;
    entry.spec.MinimumArgumentCount = minimumArguments;
    entry.spec.MaximumArgumentCount = maximumArguments;
    entry.spec.IsVolatile = isVolatile;
    entry.internalFunction = std::move(function);
    functions.insert_or_assign(std::move(name), std::move(entry));
}

// --- UTF-8 aware character helpers -----------------------------------------
// Excel text functions operate on characters. Strings are stored as UTF-8, so
// positions and lengths count code points.

Size Utf8SequenceLength(unsigned char lead)
{
    if (lead < 0x80)
    {
        return 1;
    }
    if ((lead & 0xE0) == 0xC0)
    {
        return 2;
    }
    if ((lead & 0xF0) == 0xE0)
    {
        return 3;
    }
    if ((lead & 0xF8) == 0xF0)
    {
        return 4;
    }
    return 1; // treat malformed bytes as single characters
}

Size Utf8Length(std::string_view text)
{
    Size count = 0;
    Size i = 0;
    while (i < text.size())
    {
        i += Utf8SequenceLength(static_cast<unsigned char>(text[i]));
        ++count;
    }
    return count;
}

/** Byte offset of the zero-based character index, clamped to text size. */
Size Utf8ByteOffset(std::string_view text, Size characterIndex)
{
    Size i = 0;
    Size count = 0;
    while (i < text.size() && count < characterIndex)
    {
        i += Utf8SequenceLength(static_cast<unsigned char>(text[i]));
        ++count;
    }
    return std::min(i, text.size());
}

std::string Utf8Substring(std::string_view text, Size startCharacter, Size characterCount)
{
    const Size begin = Utf8ByteOffset(text, startCharacter);
    const Size end = Utf8ByteOffset(text, startCharacter + characterCount);
    return std::string(text.substr(begin, end - begin));
}

// --- shared argument helpers ------------------------------------------------

struct NumberArg
{
    Real value = 0.0;
    FormulaValue error;
    bool failed = false;
};

NumberArg GetNumber(FormulaEvaluationSession& session, const EvalValue& argument)
{
    NumberArg result;
    const FormulaValue value = Helpers::ScalarNumber(session, argument);
    if (value.IsError())
    {
        result.error = value;
        result.failed = true;
        return result;
    }
    result.value = *value.NumberValue();
    return result;
}

struct TextArg
{
    std::string value;
    FormulaValue error;
    bool failed = false;
};

TextArg GetText(FormulaEvaluationSession& session, const EvalValue& argument)
{
    TextArg result;
    const FormulaValue value = Helpers::ScalarText(session, argument);
    if (value.IsError())
    {
        result.error = value;
        result.failed = true;
        return result;
    }
    result.value = value.TextValue();
    return result;
}

/** Collects numbers of one argument in row-major order (for NPV and IRR). */
std::optional<FormulaValue> CollectOrderedNumbers(FormulaEvaluationSession& session,
                                                  const EvalValue& argument,
                                                  std::vector<Real>& numbers)
{
    FormulaValue error;
    bool hasError = false;
    const auto failure = Helpers::ForEachValue(session, argument, [&](const FormulaValue& value)
                                               {
        if (hasError)
        {
            return;
        }
        if (value.IsError())
        {
            error = value;
            hasError = true;
            return;
        }
        if (value.Kind() == FormulaValueKind::Number)
        {
            numbers.push_back(*value.NumberValue());
        } });
    if (failure)
    {
        return failure;
    }
    if (hasError)
    {
        return error;
    }
    return std::nullopt;
}

// --- lookup helpers ---------------------------------------------------------

/**
 * Compares a lookup subject with a table candidate. Only same-type values
 * compare; mismatched types report std::nullopt.
 */
std::optional<int> LookupCompare(const FormulaValue& subject, const FormulaValue& candidate)
{
    if (subject.Kind() != candidate.Kind())
    {
        return std::nullopt;
    }
    return FormulaCoercion::Compare(candidate, subject);
}

bool LookupExactMatch(const FormulaValue& subject, const FormulaValue& candidate)
{
    if (subject.Kind() == FormulaValueKind::Text && candidate.Kind() == FormulaValueKind::Text)
    {
        const std::string& pattern = subject.TextValue();
        if (pattern.find('*') != std::string::npos || pattern.find('?') != std::string::npos)
        {
            return Helpers::WildcardMatch(pattern, candidate.TextValue());
        }
    }
    const auto comparison = LookupCompare(subject, candidate);
    return comparison && *comparison == 0;
}

/**
 * Approximate lookup over one row or column of a matrix: the largest value
 * less than or equal to the subject, assuming ascending order.
 */
std::optional<Size> ApproximateMatch(const FormulaValue& matrix,
                                     const FormulaValue& subject,
                                     bool byRow,
                                     Size lineIndex,
                                     Size count)
{
    std::optional<Size> best;
    for (Size i = 0; i < count; ++i)
    {
        const FormulaValue& candidate = byRow ? matrix.At(lineIndex, i) : matrix.At(i, lineIndex);
        const auto comparison = LookupCompare(subject, candidate);
        if (!comparison)
        {
            continue;
        }
        if (*comparison <= 0)
        {
            best = i;
        }
        else if (best)
        {
            break;
        }
    }
    return best;
}

// --- date helpers -----------------------------------------------------------

std::optional<Real> CurrentSerial(bool includeTime)
{
    std::time_t now = std::time(nullptr);
    std::tm local{};
#ifdef _WIN32
    if (localtime_s(&local, &now) != 0)
    {
        return std::nullopt;
    }
#else
    if (localtime_r(&now, &local) == nullptr)
    {
        return std::nullopt;
    }
#endif
    return ExcelDateSerial::FromParts(local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
                                      includeTime ? local.tm_hour : 0,
                                      includeTime ? local.tm_min : 0,
                                      includeTime ? local.tm_sec : 0);
}

/** Parses date text: ISO 8601 or the canonical en-US M/D/YYYY form. */
std::optional<Real> ParseDateText(std::string_view text)
{
    if (const auto iso = ExcelDateSerial::ParseIso(text))
    {
        return iso;
    }
    // M/D/YYYY with optional time after a space.
    std::string_view datePart = text;
    std::string_view timePart;
    if (const Size space = text.find(' '); space != std::string_view::npos)
    {
        datePart = text.substr(0, space);
        timePart = text.substr(space + 1);
    }
    const Size firstSlash = datePart.find('/');
    if (firstSlash == std::string_view::npos)
    {
        return std::nullopt;
    }
    const Size secondSlash = datePart.find('/', firstSlash + 1);
    if (secondSlash == std::string_view::npos)
    {
        return std::nullopt;
    }
    const auto parseNumber = [](std::string_view part) -> std::optional<UInt32>
    {
        if (part.empty() || part.size() > 4)
        {
            return std::nullopt;
        }
        UInt32 value = 0;
        for (const char c : part)
        {
            if (c < '0' || c > '9')
            {
                return std::nullopt;
            }
            value = value * 10 + static_cast<UInt32>(c - '0');
        }
        return value;
    };
    const auto month = parseNumber(datePart.substr(0, firstSlash));
    const auto day = parseNumber(datePart.substr(firstSlash + 1, secondSlash - firstSlash - 1));
    const auto year = parseNumber(datePart.substr(secondSlash + 1));
    if (!month || !day || !year || *month < 1 || *month > 12 || *day < 1 || *day > 31)
    {
        return std::nullopt;
    }
    UInt32 fullYear = *year;
    if (fullYear < 100)
    {
        fullYear += fullYear < 30 ? 2000 : 1900;
    }
    auto serial = ExcelDateSerial::FromParts(static_cast<Int32>(fullYear), *month, *day);
    if (!serial)
    {
        return std::nullopt;
    }
    if (!timePart.empty())
    {
        const auto timeSerial = ExcelDateSerial::ParseIso(timePart);
        if (!timeSerial)
        {
            return std::nullopt;
        }
        *serial += *timeSerial;
    }
    return serial;
}

// --- financial helpers ------------------------------------------------------

/** Future-value equation shared by PMT, FV, PV, NPER, and RATE. */
Real AnnuityFutureValue(Real rate, Real nper, Real pmt, Real pv, int type)
{
    if (rate == 0.0)
    {
        return pv + pmt * nper;
    }
    const Real growth = std::pow(1.0 + rate, nper);
    return pv * growth + pmt * (1.0 + rate * type) * (growth - 1.0) / rate;
}

/** Solves f(rate) = 0 by Newton iteration with a bisection fallback. */
std::optional<Real> SolveRate(const std::function<Real(Real)>& f, Real guess)
{
    // Newton with numeric derivative.
    Real rate = guess;
    for (int iteration = 0; iteration < 64; ++iteration)
    {
        const Real value = f(rate);
        if (std::fabs(value) < 1e-10)
        {
            return rate;
        }
        const Real h = std::max(1e-7, std::fabs(rate) * 1e-6);
        const Real derivative = (f(rate + h) - f(rate - h)) / (2.0 * h);
        if (derivative == 0.0 || !std::isfinite(derivative))
        {
            break;
        }
        const Real next = rate - value / derivative;
        if (!std::isfinite(next) || next <= -1.0)
        {
            break;
        }
        if (std::fabs(next - rate) < 1e-12)
        {
            return next;
        }
        rate = next;
    }

    // Bisection over an expanding bracket.
    Real low = -0.999999;
    Real high = 10.0;
    Real fLow = f(low);
    Real fHigh = f(high);
    int expansion = 0;
    while (fLow * fHigh > 0.0 && expansion < 16)
    {
        high *= 2.0;
        fHigh = f(high);
        ++expansion;
    }
    if (fLow * fHigh > 0.0 || !std::isfinite(fLow) || !std::isfinite(fHigh))
    {
        return std::nullopt;
    }
    for (int iteration = 0; iteration < 200; ++iteration)
    {
        const Real middle = (low + high) / 2.0;
        const Real fMiddle = f(middle);
        if (!std::isfinite(fMiddle))
        {
            return std::nullopt;
        }
        if (std::fabs(fMiddle) < 1e-10 || (high - low) < 1e-14)
        {
            return middle;
        }
        if (fLow * fMiddle <= 0.0)
        {
            high = middle;
        }
        else
        {
            low = middle;
            fLow = fMiddle;
        }
    }
    return (low + high) / 2.0;
}

} // namespace FormulaLibraryDetail

// ---------------------------------------------------------------------------
// Text functions
// ---------------------------------------------------------------------------

void FormulaFunctionLibrary::RegisterTextFunctions(FunctionMap& functions)
{
    using namespace FormulaLibraryDetail;

    Add(functions, "CONCATENATE", 1, 255,
        [](FormulaEvaluationSession& session, std::span<EvalValue> arguments) -> FormulaValue
        {
            std::string result;
            for (const EvalValue& argument : arguments)
            {
                TextArg text = GetText(session, argument);
                if (text.failed)
                {
                    return text.error;
                }
                result += text.value;
            }
            return FormulaValue::Text(std::move(result));
        });

    Add(functions, "CONCAT", 1, 255,
        [](FormulaEvaluationSession& session, std::span<EvalValue> arguments) -> FormulaValue
        {
            std::string result;
            FormulaValue error;
            bool hasError = false;
            for (const EvalValue& argument : arguments)
            {
                const auto failure = Helpers::ForEachValue(session, argument, [&](const FormulaValue& value)
                                                           {
                    if (hasError)
                    {
                        return;
                    }
                    const FormulaValue text = FormulaCoercion::ToText(value);
                    if (text.IsError())
                    {
                        error = text;
                        hasError = true;
                        return;
                    }
                    result += text.TextValue(); });
                if (failure)
                {
                    return *failure;
                }
                if (hasError)
                {
                    return error;
                }
            }
            return FormulaValue::Text(std::move(result));
        });

    Add(functions, "TEXTJOIN", 3, 255,
        [](FormulaEvaluationSession& session, std::span<EvalValue> arguments) -> FormulaValue
        {
            TextArg delimiter = GetText(session, arguments[0]);
            if (delimiter.failed)
            {
                return delimiter.error;
            }
            const FormulaValue ignoreEmptyValue = Helpers::ScalarBoolean(session, arguments[1]);
            if (ignoreEmptyValue.IsError())
            {
                return ignoreEmptyValue;
            }
            const bool ignoreEmpty = *ignoreEmptyValue.BooleanValue();

            std::string result;
            bool first = true;
            FormulaValue error;
            bool hasError = false;
            for (Size i = 2; i < arguments.size(); ++i)
            {
                const auto failure =
                    Helpers::ForEachValue(session, arguments[i], [&](const FormulaValue& value)
                                          {
                        if (hasError)
                        {
                            return;
                        }
                        const FormulaValue text = FormulaCoercion::ToText(value);
                        if (text.IsError())
                        {
                            error = text;
                            hasError = true;
                            return;
                        }
                        if (ignoreEmpty && text.TextValue().empty())
                        {
                            return;
                        }
                        if (!first)
                        {
                            result += delimiter.value;
                        }
                        result += text.TextValue();
                        first = false; });
                if (failure)
                {
                    return *failure;
                }
                if (hasError)
                {
                    return error;
                }
            }
            return FormulaValue::Text(std::move(result));
        });

    const auto sideText = [](bool fromLeft)
    {
        return [fromLeft](FormulaEvaluationSession& session, std::span<EvalValue> arguments) -> FormulaValue
        {
            TextArg text = GetText(session, arguments[0]);
            if (text.failed)
            {
                return text.error;
            }
            Real count = 1.0;
            if (arguments.size() >= 2)
            {
                const NumberArg countArg = GetNumber(session, arguments[1]);
                if (countArg.failed)
                {
                    return countArg.error;
                }
                count = std::floor(countArg.value);
            }
            if (count < 0.0)
            {
                return FormulaValue::Error(FormulaErrorCode::Value);
            }
            const Size length = Utf8Length(text.value);
            const Size take = std::min(static_cast<Size>(count), length);
            if (fromLeft)
            {
                return FormulaValue::Text(Utf8Substring(text.value, 0, take));
            }
            return FormulaValue::Text(Utf8Substring(text.value, length - take, take));
        };
    };
    Add(functions, "LEFT", 1, 2, sideText(true));
    Add(functions, "RIGHT", 1, 2, sideText(false));

    Add(functions, "MID", 3, 3,
        [](FormulaEvaluationSession& session, std::span<EvalValue> arguments) -> FormulaValue
        {
            TextArg text = GetText(session, arguments[0]);
            if (text.failed)
            {
                return text.error;
            }
            const NumberArg start = GetNumber(session, arguments[1]);
            if (start.failed)
            {
                return start.error;
            }
            const NumberArg count = GetNumber(session, arguments[2]);
            if (count.failed)
            {
                return count.error;
            }
            const Real startIndex = std::floor(start.value);
            const Real characterCount = std::floor(count.value);
            if (startIndex < 1.0 || characterCount < 0.0)
            {
                return FormulaValue::Error(FormulaErrorCode::Value);
            }
            return FormulaValue::Text(Utf8Substring(text.value,
                                                    static_cast<Size>(startIndex) - 1,
                                                    static_cast<Size>(characterCount)));
        });

    Add(functions, "LEN", 1, 1,
        [](FormulaEvaluationSession& session, std::span<EvalValue> arguments) -> FormulaValue
        {
            TextArg text = GetText(session, arguments[0]);
            if (text.failed)
            {
                return text.error;
            }
            return FormulaValue::Number(static_cast<Real>(Utf8Length(text.value)));
        });

    const auto mapText = [](std::function<std::string(std::string)> transform)
    {
        return [transform = std::move(transform)](FormulaEvaluationSession& session,
                                                  std::span<EvalValue> arguments) -> FormulaValue
        {
            TextArg text = GetText(session, arguments[0]);
            if (text.failed)
            {
                return text.error;
            }
            return FormulaValue::Text(transform(std::move(text.value)));
        };
    };
    // Case mapping is ASCII-only; non-ASCII characters pass through unchanged.
    Add(functions, "LOWER", 1, 1, mapText([](std::string text)
                                          {
            for (char& c : text)
            {
                c = AsciiText::ToLower(c);
            }
            return text; }));
    Add(functions, "UPPER", 1, 1, mapText([](std::string text)
                                          {
            for (char& c : text)
            {
                c = AsciiText::ToUpper(c);
            }
            return text; }));
    Add(functions, "PROPER", 1, 1, mapText([](std::string text)
                                           {
            bool startOfWord = true;
            for (char& c : text)
            {
                const bool isLetter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
                c = startOfWord ? AsciiText::ToUpper(c) : AsciiText::ToLower(c);
                startOfWord = !isLetter;
            }
            return text; }));
    Add(functions, "TRIM", 1, 1, mapText([](std::string text)
                                         {
            std::string result;
            bool pendingSpace = false;
            for (const char c : text)
            {
                if (c == ' ')
                {
                    pendingSpace = !result.empty();
                    continue;
                }
                if (pendingSpace)
                {
                    result.push_back(' ');
                    pendingSpace = false;
                }
                result.push_back(c);
            }
            return result; }));
    Add(functions, "CLEAN", 1, 1, mapText([](std::string text)
                                          {
            std::string result;
            for (const char c : text)
            {
                if (static_cast<unsigned char>(c) >= 32 || static_cast<unsigned char>(c) >= 0x80)
                {
                    result.push_back(c);
                }
            }
            return result; }));

    Add(functions, "SUBSTITUTE", 3, 4,
        [](FormulaEvaluationSession& session, std::span<EvalValue> arguments) -> FormulaValue
        {
            TextArg text = GetText(session, arguments[0]);
            if (text.failed)
            {
                return text.error;
            }
            TextArg oldText = GetText(session, arguments[1]);
            if (oldText.failed)
            {
                return oldText.error;
            }
            TextArg newText = GetText(session, arguments[2]);
            if (newText.failed)
            {
                return newText.error;
            }
            std::optional<Size> instance;
            if (arguments.size() >= 4)
            {
                const NumberArg instanceArg = GetNumber(session, arguments[3]);
                if (instanceArg.failed)
                {
                    return instanceArg.error;
                }
                const Real index = std::floor(instanceArg.value);
                if (index < 1.0)
                {
                    return FormulaValue::Error(FormulaErrorCode::Value);
                }
                instance = static_cast<Size>(index);
            }
            if (oldText.value.empty())
            {
                return FormulaValue::Text(std::move(text.value));
            }

            std::string result;
            Size position = 0;
            Size occurrence = 0;
            while (true)
            {
                const Size found = text.value.find(oldText.value, position);
                if (found == std::string::npos)
                {
                    result.append(text.value, position, std::string::npos);
                    break;
                }
                ++occurrence;
                result.append(text.value, position, found - position);
                if (!instance || occurrence == *instance)
                {
                    result += newText.value;
                }
                else
                {
                    result += oldText.value;
                }
                position = found + oldText.value.size();
            }
            return FormulaValue::Text(std::move(result));
        });

    Add(functions, "REPLACE", 4, 4,
        [](FormulaEvaluationSession& session, std::span<EvalValue> arguments) -> FormulaValue
        {
            TextArg text = GetText(session, arguments[0]);
            if (text.failed)
            {
                return text.error;
            }
            const NumberArg start = GetNumber(session, arguments[1]);
            if (start.failed)
            {
                return start.error;
            }
            const NumberArg count = GetNumber(session, arguments[2]);
            if (count.failed)
            {
                return count.error;
            }
            TextArg replacement = GetText(session, arguments[3]);
            if (replacement.failed)
            {
                return replacement.error;
            }
            const Real startIndex = std::floor(start.value);
            const Real characterCount = std::floor(count.value);
            if (startIndex < 1.0 || characterCount < 0.0)
            {
                return FormulaValue::Error(FormulaErrorCode::Value);
            }
            const Size begin = Utf8ByteOffset(text.value, static_cast<Size>(startIndex) - 1);
            const Size end = Utf8ByteOffset(text.value,
                                            static_cast<Size>(startIndex) - 1 +
                                                static_cast<Size>(characterCount));
            std::string result = text.value.substr(0, begin);
            result += replacement.value;
            result += text.value.substr(end);
            return FormulaValue::Text(std::move(result));
        });

    const auto findText = [](bool caseSensitive)
    {
        return [caseSensitive](FormulaEvaluationSession& session,
                               std::span<EvalValue> arguments) -> FormulaValue
        {
            TextArg needle = GetText(session, arguments[0]);
            if (needle.failed)
            {
                return needle.error;
            }
            TextArg haystack = GetText(session, arguments[1]);
            if (haystack.failed)
            {
                return haystack.error;
            }
            Size startCharacter = 0;
            if (arguments.size() >= 3)
            {
                const NumberArg start = GetNumber(session, arguments[2]);
                if (start.failed)
                {
                    return start.error;
                }
                const Real index = std::floor(start.value);
                if (index < 1.0)
                {
                    return FormulaValue::Error(FormulaErrorCode::Value);
                }
                startCharacter = static_cast<Size>(index) - 1;
            }

            const Size haystackLength = Utf8Length(haystack.value);
            if (startCharacter > haystackLength)
            {
                return FormulaValue::Error(FormulaErrorCode::Value);
            }

            const bool useWildcards =
                !caseSensitive && (needle.value.find('*') != std::string::npos ||
                                   needle.value.find('?') != std::string::npos);
            const std::string wildcardPattern = needle.value + "*";

            for (Size character = startCharacter; character <= haystackLength; ++character)
            {
                const Size byteOffset = Utf8ByteOffset(haystack.value, character);
                const std::string_view rest = std::string_view(haystack.value).substr(byteOffset);
                bool matches = false;
                if (useWildcards)
                {
                    matches = Helpers::WildcardMatch(wildcardPattern, rest);
                }
                else if (rest.size() >= needle.value.size())
                {
                    const std::string_view candidate = rest.substr(0, needle.value.size());
                    if (caseSensitive)
                    {
                        matches = candidate == needle.value;
                    }
                    else
                    {
                        matches = std::equal(candidate.begin(), candidate.end(), needle.value.begin(),
                                             [](char a, char b)
                                             { return AsciiText::ToUpper(a) == AsciiText::ToUpper(b); });
                    }
                }
                if (matches)
                {
                    return FormulaValue::Number(static_cast<Real>(character + 1));
                }
            }
            return FormulaValue::Error(FormulaErrorCode::Value);
        };
    };
    Add(functions, "FIND", 2, 3, findText(true));
    Add(functions, "SEARCH", 2, 3, findText(false));

    Add(functions, "REPT", 2, 2,
        [](FormulaEvaluationSession& session, std::span<EvalValue> arguments) -> FormulaValue
        {
            TextArg text = GetText(session, arguments[0]);
            if (text.failed)
            {
                return text.error;
            }
            const NumberArg count = GetNumber(session, arguments[1]);
            if (count.failed)
            {
                return count.error;
            }
            const Real repetitions = std::floor(count.value);
            if (repetitions < 0.0)
            {
                return FormulaValue::Error(FormulaErrorCode::Value);
            }
            if (static_cast<Real>(text.value.size()) * repetitions > static_cast<Real>(ExcelMaxTextLength))
            {
                return FormulaValue::Error(FormulaErrorCode::Value);
            }
            // Empty text passes the length check for any repetition count, so the
            // count itself is bounded too; iterating a count like 1e300 would
            // never terminate. A non-empty text is already capped by the check
            // above, which keeps the conversion below in range.
            const Size repeatCount =
                text.value.empty() ? 0 : static_cast<Size>(repetitions);
            std::string result;
            result.reserve(text.value.size() * repeatCount);
            for (Size i = 0; i < repeatCount; ++i)
            {
                result += text.value;
            }
            return FormulaValue::Text(std::move(result));
        });

    Add(functions, "VALUE", 1, 1,
        [](FormulaEvaluationSession& session, std::span<EvalValue> arguments) -> FormulaValue
        {
            const FormulaValue value = Helpers::Scalar(session, arguments[0]);
            if (value.IsError() || value.Kind() == FormulaValueKind::Number)
            {
                return value;
            }
            if (value.Kind() == FormulaValueKind::Text)
            {
                if (const auto parsed = FormulaCoercion::ParseNumberText(value.TextValue(), true))
                {
                    return FormulaValue::Number(*parsed);
                }
            }
            return FormulaValue::Error(FormulaErrorCode::Value);
        });

    Add(functions, "EXACT", 2, 2,
        [](FormulaEvaluationSession& session, std::span<EvalValue> arguments) -> FormulaValue
        {
            TextArg left = GetText(session, arguments[0]);
            if (left.failed)
            {
                return left.error;
            }
            TextArg right = GetText(session, arguments[1]);
            if (right.failed)
            {
                return right.error;
            }
            return FormulaValue::Boolean(left.value == right.value);
        });

    Add(functions, "CHAR", 1, 1,
        [](FormulaEvaluationSession& session, std::span<EvalValue> arguments) -> FormulaValue
        {
            const NumberArg code = GetNumber(session, arguments[0]);
            if (code.failed)
            {
                return code.error;
            }
            const Real value = std::floor(code.value);
            if (value < 1.0 || value > 255.0)
            {
                return FormulaValue::Error(FormulaErrorCode::Value);
            }
            const auto codePoint = static_cast<UInt32>(value);
            std::string text;
            if (codePoint < 0x80)
            {
                text.push_back(static_cast<char>(codePoint));
            }
            else
            {
                // Latin-1 code points encode as two UTF-8 bytes.
                text.push_back(static_cast<char>(0xC0 | (codePoint >> 6)));
                text.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
            }
            return FormulaValue::Text(std::move(text));
        });

    Add(functions, "CODE", 1, 1,
        [](FormulaEvaluationSession& session, std::span<EvalValue> arguments) -> FormulaValue
        {
            TextArg text = GetText(session, arguments[0]);
            if (text.failed)
            {
                return text.error;
            }
            if (text.value.empty())
            {
                return FormulaValue::Error(FormulaErrorCode::Value);
            }
            // Decode the first UTF-8 code point.
            const auto lead = static_cast<unsigned char>(text.value[0]);
            UInt32 codePoint = lead;
            const Size length = Utf8SequenceLength(lead);
            if (length > 1 && text.value.size() >= length)
            {
                codePoint = lead & (0xFF >> (length + 1));
                for (Size i = 1; i < length; ++i)
                {
                    codePoint = (codePoint << 6) | (static_cast<unsigned char>(text.value[i]) & 0x3F);
                }
            }
            return FormulaValue::Number(static_cast<Real>(codePoint));
        });

    Add(functions, "TEXT", 2, 2,
        [](FormulaEvaluationSession& session, std::span<EvalValue> arguments) -> FormulaValue
        {
            const FormulaValue value = Helpers::Scalar(session, arguments[0]);
            if (value.IsError())
            {
                return value;
            }
            TextArg format = GetText(session, arguments[1]);
            if (format.failed)
            {
                return format.error;
            }
            if (value.Kind() == FormulaValueKind::Text)
            {
                auto formatted = NumberFormatText::FormatText(value.TextValue(), format.value);
                if (!formatted)
                {
                    return FormulaValue::Error(FormulaErrorCode::Value);
                }
                return FormulaValue::Text(std::move(*formatted));
            }
            const FormulaValue number = FormulaCoercion::ToNumber(value);
            if (number.IsError())
            {
                return number;
            }
            auto formatted = NumberFormatText::FormatNumber(*number.NumberValue(), format.value);
            if (!formatted)
            {
                return FormulaValue::Error(FormulaErrorCode::Value);
            }
            return FormulaValue::Text(std::move(*formatted));
        });
}

// ---------------------------------------------------------------------------
// Lookup and reference functions
// ---------------------------------------------------------------------------

void FormulaFunctionLibrary::RegisterLookupFunctions(FunctionMap& functions)
{
    using namespace FormulaLibraryDetail;

    const auto tableLookup = [](bool vertical)
    {
        return [vertical](FormulaEvaluationSession& session, std::span<EvalValue> arguments) -> FormulaValue
        {
            const FormulaValue subject = Helpers::Scalar(session, arguments[0]);
            if (subject.IsError())
            {
                return subject;
            }
            const FormulaValue table = Helpers::Materialize(session, arguments[1]);
            if (table.IsError())
            {
                return table;
            }
            const NumberArg indexArg = GetNumber(session, arguments[2]);
            if (indexArg.failed)
            {
                return indexArg.error;
            }
            bool approximate = true;
            if (arguments.size() >= 4)
            {
                const FormulaValue flag = Helpers::ScalarBoolean(session, arguments[3]);
                if (flag.IsError())
                {
                    return flag;
                }
                approximate = *flag.BooleanValue();
            }
            const Real index = std::floor(indexArg.value);
            const Size resultLine = static_cast<Size>(index);
            if (index < 1.0)
            {
                return FormulaValue::Error(FormulaErrorCode::Value);
            }
            const Size lineCount = vertical ? table.ColumnCount() : table.RowCount();
            const Size searchCount = vertical ? table.RowCount() : table.ColumnCount();
            if (resultLine > lineCount)
            {
                return FormulaValue::Error(FormulaErrorCode::Ref);
            }

            std::optional<Size> matchIndex;
            if (approximate)
            {
                matchIndex = ApproximateMatch(table, subject, !vertical, 0, searchCount);
            }
            else
            {
                for (Size i = 0; i < searchCount; ++i)
                {
                    const FormulaValue& candidate = vertical ? table.At(i, 0) : table.At(0, i);
                    if (LookupExactMatch(subject, candidate))
                    {
                        matchIndex = i;
                        break;
                    }
                }
            }
            if (!matchIndex)
            {
                return FormulaValue::Error(FormulaErrorCode::NA);
            }
            return vertical ? table.At(*matchIndex, resultLine - 1) : table.At(resultLine - 1, *matchIndex);
        };
    };
    Add(functions, "VLOOKUP", 3, 4, tableLookup(true));
    Add(functions, "HLOOKUP", 3, 4, tableLookup(false));

    Add(functions, "LOOKUP", 2, 3,
        [](FormulaEvaluationSession& session, std::span<EvalValue> arguments) -> FormulaValue
        {
            const FormulaValue subject = Helpers::Scalar(session, arguments[0]);
            if (subject.IsError())
            {
                return subject;
            }
            const FormulaValue vector = Helpers::Materialize(session, arguments[1]);
            if (vector.IsError())
            {
                return vector;
            }

            if (arguments.size() >= 3)
            {
                // Vector form: search the first vector, return from the second.
                const FormulaValue resultVector = Helpers::Materialize(session, arguments[2]);
                if (resultVector.IsError())
                {
                    return resultVector;
                }
                const bool byRow = vector.RowCount() == 1;
                const Size count = byRow ? vector.ColumnCount() : vector.RowCount();
                const auto match = ApproximateMatch(vector, subject, byRow, 0, count);
                if (!match)
                {
                    return FormulaValue::Error(FormulaErrorCode::NA);
                }
                const bool resultByRow = resultVector.RowCount() == 1;
                const Size resultCount =
                    resultByRow ? resultVector.ColumnCount() : resultVector.RowCount();
                if (*match >= resultCount)
                {
                    return FormulaValue::Error(FormulaErrorCode::NA);
                }
                return resultByRow ? resultVector.At(0, *match) : resultVector.At(*match, 0);
            }

            // Array form: search the first column (or row when wider than
            // tall) and return from the last column (or row).
            const bool searchColumns = vector.RowCount() >= vector.ColumnCount();
            const Size count = searchColumns ? vector.RowCount() : vector.ColumnCount();
            const auto match = ApproximateMatch(vector, subject, !searchColumns, 0, count);
            if (!match)
            {
                return FormulaValue::Error(FormulaErrorCode::NA);
            }
            return searchColumns ? vector.At(*match, vector.ColumnCount() - 1)
                                 : vector.At(vector.RowCount() - 1, *match);
        });

    Add(functions, "INDEX", 2, 3,
        [](FormulaEvaluationSession& session, std::span<EvalValue> arguments) -> FormulaValue
        {
            const FormulaValue matrix = Helpers::Materialize(session, arguments[0]);
            if (matrix.IsError())
            {
                return matrix;
            }
            const NumberArg rowArg = GetNumber(session, arguments[1]);
            if (rowArg.failed)
            {
                return rowArg.error;
            }
            Real columnNumber = matrix.RowCount() == 1 && arguments.size() < 3 ? rowArg.value : 1.0;
            Real rowNumber = rowArg.value;
            if (matrix.RowCount() == 1 && arguments.size() < 3)
            {
                rowNumber = 1.0;
            }
            if (arguments.size() >= 3)
            {
                const NumberArg columnArg = GetNumber(session, arguments[2]);
                if (columnArg.failed)
                {
                    return columnArg.error;
                }
                columnNumber = columnArg.value;
            }
            const Real row = std::floor(rowNumber);
            const Real column = std::floor(columnNumber);
            if (row < 0.0 || column < 0.0)
            {
                return FormulaValue::Error(FormulaErrorCode::Value);
            }
            if (row > static_cast<Real>(matrix.RowCount()) ||
                column > static_cast<Real>(matrix.ColumnCount()))
            {
                return FormulaValue::Error(FormulaErrorCode::Ref);
            }

            // Row or column zero selects the whole line.
            if (row == 0.0 && column == 0.0)
            {
                return matrix;
            }
            if (row == 0.0)
            {
                std::vector<FormulaValue> elements;
                elements.reserve(matrix.RowCount());
                for (Size i = 0; i < matrix.RowCount(); ++i)
                {
                    elements.push_back(matrix.At(i, static_cast<Size>(column) - 1));
                }
                return FormulaValue::Array(matrix.RowCount(), 1, std::move(elements));
            }
            if (column == 0.0)
            {
                std::vector<FormulaValue> elements;
                elements.reserve(matrix.ColumnCount());
                for (Size i = 0; i < matrix.ColumnCount(); ++i)
                {
                    elements.push_back(matrix.At(static_cast<Size>(row) - 1, i));
                }
                return FormulaValue::Array(1, matrix.ColumnCount(), std::move(elements));
            }
            return matrix.At(static_cast<Size>(row) - 1, static_cast<Size>(column) - 1);
        });

    Add(functions, "MATCH", 2, 3,
        [](FormulaEvaluationSession& session, std::span<EvalValue> arguments) -> FormulaValue
        {
            const FormulaValue subject = Helpers::Scalar(session, arguments[0]);
            if (subject.IsError())
            {
                return subject;
            }
            const FormulaValue vector = Helpers::Materialize(session, arguments[1]);
            if (vector.IsError())
            {
                return vector;
            }
            Real matchType = 1.0;
            if (arguments.size() >= 3)
            {
                const NumberArg typeArg = GetNumber(session, arguments[2]);
                if (typeArg.failed)
                {
                    return typeArg.error;
                }
                matchType = typeArg.value;
            }
            const bool byRow = vector.RowCount() == 1;
            const Size count = byRow ? vector.ColumnCount() : vector.RowCount();
            if (vector.RowCount() != 1 && vector.ColumnCount() != 1)
            {
                return FormulaValue::Error(FormulaErrorCode::NA);
            }

            const auto candidateAt = [&](Size i) -> const FormulaValue&
            {
                return byRow ? vector.At(0, i) : vector.At(i, 0);
            };

            if (matchType == 0.0)
            {
                for (Size i = 0; i < count; ++i)
                {
                    if (LookupExactMatch(subject, candidateAt(i)))
                    {
                        return FormulaValue::Number(static_cast<Real>(i + 1));
                    }
                }
                return FormulaValue::Error(FormulaErrorCode::NA);
            }
            if (matchType > 0.0)
            {
                const auto match = ApproximateMatch(vector, subject, byRow, 0, count);
                if (!match)
                {
                    return FormulaValue::Error(FormulaErrorCode::NA);
                }
                return FormulaValue::Number(static_cast<Real>(*match + 1));
            }
            // matchType -1: smallest value >= subject, assuming descending order.
            std::optional<Size> best;
            for (Size i = 0; i < count; ++i)
            {
                const auto comparison = LookupCompare(subject, candidateAt(i));
                if (!comparison)
                {
                    continue;
                }
                if (*comparison >= 0)
                {
                    best = i;
                }
                else if (best)
                {
                    break;
                }
            }
            if (!best)
            {
                return FormulaValue::Error(FormulaErrorCode::NA);
            }
            return FormulaValue::Number(static_cast<Real>(*best + 1));
        });

    Add(functions, "ROW", 0, 1,
        [](FormulaEvaluationSession& session, std::span<EvalValue> arguments) -> FormulaValue
        {
            if (arguments.empty())
            {
                if (!session.Anchor().IsValid())
                {
                    return FormulaValue::Error(FormulaErrorCode::Value);
                }
                return FormulaValue::Number(static_cast<Real>(session.Anchor().Row().Value()));
            }
            if (!arguments[0].isReference || arguments[0].areas.empty())
            {
                return FormulaValue::Error(FormulaErrorCode::Value);
            }
            return FormulaValue::Number(static_cast<Real>(arguments[0].areas.front().firstRow));
        });
    Add(functions, "COLUMN", 0, 1,
        [](FormulaEvaluationSession& session, std::span<EvalValue> arguments) -> FormulaValue
        {
            if (arguments.empty())
            {
                if (!session.Anchor().IsValid())
                {
                    return FormulaValue::Error(FormulaErrorCode::Value);
                }
                return FormulaValue::Number(static_cast<Real>(session.Anchor().Column().Value()));
            }
            if (!arguments[0].isReference || arguments[0].areas.empty())
            {
                return FormulaValue::Error(FormulaErrorCode::Value);
            }
            return FormulaValue::Number(static_cast<Real>(arguments[0].areas.front().firstColumn));
        });
    Add(functions, "ROWS", 1, 1,
        [](FormulaEvaluationSession& session, std::span<EvalValue> arguments) -> FormulaValue
        {
            if (arguments[0].isReference && !arguments[0].areas.empty())
            {
                return FormulaValue::Number(static_cast<Real>(arguments[0].areas.front().RowCount()));
            }
            const FormulaValue value = session.DereferenceToValue(arguments[0]);
            if (value.IsError())
            {
                return value;
            }
            return FormulaValue::Number(static_cast<Real>(value.RowCount()));
        });
    Add(functions, "COLUMNS", 1, 1,
        [](FormulaEvaluationSession& session, std::span<EvalValue> arguments) -> FormulaValue
        {
            if (arguments[0].isReference && !arguments[0].areas.empty())
            {
                return FormulaValue::Number(static_cast<Real>(arguments[0].areas.front().ColumnCount()));
            }
            const FormulaValue value = session.DereferenceToValue(arguments[0]);
            if (value.IsError())
            {
                return value;
            }
            return FormulaValue::Number(static_cast<Real>(value.ColumnCount()));
        });

    Add(functions, "OFFSET", 3, 5, [](FormulaEvaluationSession& session, std::span<EvalValue> arguments) -> FormulaValue
        {
            if (!arguments[0].isReference || arguments[0].areas.size() != 1)
            {
                return FormulaValue::Error(FormulaErrorCode::Value);
            }
            const ResolvedReferenceArea& base = arguments[0].areas.front();
            const NumberArg rowsArg = GetNumber(session, arguments[1]);
            if (rowsArg.failed)
            {
                return rowsArg.error;
            }
            const NumberArg columnsArg = GetNumber(session, arguments[2]);
            if (columnsArg.failed)
            {
                return columnsArg.error;
            }
            Real height = static_cast<Real>(base.RowCount());
            Real width = static_cast<Real>(base.ColumnCount());
            if (arguments.size() >= 4 && arguments[3].value.Kind() != FormulaValueKind::Blank)
            {
                const NumberArg heightArg = GetNumber(session, arguments[3]);
                if (heightArg.failed)
                {
                    return heightArg.error;
                }
                height = std::floor(heightArg.value);
            }
            if (arguments.size() >= 5 && arguments[4].value.Kind() != FormulaValueKind::Blank)
            {
                const NumberArg widthArg = GetNumber(session, arguments[4]);
                if (widthArg.failed)
                {
                    return widthArg.error;
                }
                width = std::floor(widthArg.value);
            }
            if (height < 1.0 || width < 1.0)
            {
                return FormulaValue::Error(FormulaErrorCode::Ref);
            }

            const Real firstRow = static_cast<Real>(base.firstRow) + std::floor(rowsArg.value);
            const Real firstColumn = static_cast<Real>(base.firstColumn) + std::floor(columnsArg.value);
            const Real lastRow = firstRow + height - 1.0;
            const Real lastColumn = firstColumn + width - 1.0;
            if (firstRow < 1.0 || firstColumn < 1.0 || lastRow > static_cast<Real>(MaxRowIndex) ||
                lastColumn > static_cast<Real>(MaxColumnIndex))
            {
                return FormulaValue::Error(FormulaErrorCode::Ref);
            }

            ResolvedReferenceArea area;
            area.sheet = base.sheet;
            area.firstRow = static_cast<UInt32>(firstRow);
            area.lastRow = static_cast<UInt32>(lastRow);
            area.firstColumn = static_cast<UInt32>(firstColumn);
            area.lastColumn = static_cast<UInt32>(lastColumn);
            // The result is a value or matrix of the target area; reference
            // results are dereferenced here because internal functions return
            // values.
            EvalValue reference = EvalValue::Reference({area});
            if (area.RowCount() == 1 && area.ColumnCount() == 1)
            {
                return session.ReadCell(area.sheet, area.firstRow, area.firstColumn);
            }
            return session.DereferenceToValue(reference); }, true);

    Add(functions, "INDIRECT", 1, 2, [](FormulaEvaluationSession& session, std::span<EvalValue> arguments) -> FormulaValue
        {
            const FormulaValue text = Helpers::ScalarText(session, arguments[0]);
            if (text.IsError())
            {
                return text;
            }
            if (arguments.size() >= 2)
            {
                const FormulaValue a1 = Helpers::ScalarBoolean(session, arguments[1]);
                if (a1.IsError())
                {
                    return a1;
                }
                if (!*a1.BooleanValue())
                {
                    // R1C1 interpretation is not supported.
                    return FormulaValue::Error(FormulaErrorCode::Ref);
                }
            }
            FormulaParseResult parsed = FormulaParser::Parse(text.TextValue());
            if (!parsed.Succeeded() || parsed.root->kind != FormulaExpressionKind::Reference)
            {
                return FormulaValue::Error(FormulaErrorCode::Ref);
            }
            const EvalValue reference = session.Evaluate(*parsed.root);
            if (!reference.isReference)
            {
                return reference.value.IsError() ? reference.value
                                                 : FormulaValue::Error(FormulaErrorCode::Ref);
            }
            return session.DereferenceToValue(reference); }, true);

    Add(functions, "TRANSPOSE", 1, 1,
        [](FormulaEvaluationSession& session, std::span<EvalValue> arguments) -> FormulaValue
        {
            const FormulaValue matrix = Helpers::Materialize(session, arguments[0]);
            if (matrix.IsError())
            {
                return matrix;
            }
            std::vector<FormulaValue> elements;
            elements.reserve(matrix.RowCount() * matrix.ColumnCount());
            for (Size column = 0; column < matrix.ColumnCount(); ++column)
            {
                for (Size row = 0; row < matrix.RowCount(); ++row)
                {
                    elements.push_back(matrix.At(row, column));
                }
            }
            return FormulaValue::Array(matrix.ColumnCount(), matrix.RowCount(), std::move(elements));
        });
}

// ---------------------------------------------------------------------------
// Date and time functions
// ---------------------------------------------------------------------------

void FormulaFunctionLibrary::RegisterDateTimeFunctions(FunctionMap& functions)
{
    using namespace FormulaLibraryDetail;

    Add(functions, "DATE", 3, 3,
        [](FormulaEvaluationSession& session, std::span<EvalValue> arguments) -> FormulaValue
        {
            const NumberArg year = GetNumber(session, arguments[0]);
            if (year.failed)
            {
                return year.error;
            }
            const NumberArg month = GetNumber(session, arguments[1]);
            if (month.failed)
            {
                return month.error;
            }
            const NumberArg day = GetNumber(session, arguments[2]);
            if (day.failed)
            {
                return day.error;
            }
            Real yearValue = std::floor(year.value);
            // Excel adds 1900 to years below 1900.
            if (yearValue >= 0.0 && yearValue < 1900.0)
            {
                yearValue += 1900.0;
            }
            if (yearValue < 0.0 || yearValue > 9999.0)
            {
                return FormulaValue::Error(FormulaErrorCode::Num);
            }
            const auto serial = ExcelDateSerial::FromParts(static_cast<Int32>(yearValue),
                                                           static_cast<Int64>(std::floor(month.value)),
                                                           static_cast<Int64>(std::floor(day.value)));
            if (!serial)
            {
                return FormulaValue::Error(FormulaErrorCode::Num);
            }
            return FormulaValue::Number(*serial);
        });

    Add(functions, "TIME", 3, 3,
        [](FormulaEvaluationSession& session, std::span<EvalValue> arguments) -> FormulaValue
        {
            const NumberArg hour = GetNumber(session, arguments[0]);
            if (hour.failed)
            {
                return hour.error;
            }
            const NumberArg minute = GetNumber(session, arguments[1]);
            if (minute.failed)
            {
                return minute.error;
            }
            const NumberArg second = GetNumber(session, arguments[2]);
            if (second.failed)
            {
                return second.error;
            }
            const Real totalSeconds = std::floor(hour.value) * 3600.0 + std::floor(minute.value) * 60.0 +
                                      std::floor(second.value);
            if (totalSeconds < 0.0)
            {
                return FormulaValue::Error(FormulaErrorCode::Num);
            }
            const Real fraction = totalSeconds / 86400.0;
            return FormulaValue::Number(fraction - std::floor(fraction));
        });

    Add(functions, "DATEVALUE", 1, 1,
        [](FormulaEvaluationSession& session, std::span<EvalValue> arguments) -> FormulaValue
        {
            const FormulaValue value = Helpers::Scalar(session, arguments[0]);
            if (value.IsError())
            {
                return value;
            }
            if (value.Kind() != FormulaValueKind::Text)
            {
                return FormulaValue::Error(FormulaErrorCode::Value);
            }
            const auto serial = ParseDateText(value.TextValue());
            if (!serial)
            {
                return FormulaValue::Error(FormulaErrorCode::Value);
            }
            return FormulaValue::Number(std::floor(*serial));
        });

    Add(functions, "TIMEVALUE", 1, 1,
        [](FormulaEvaluationSession& session, std::span<EvalValue> arguments) -> FormulaValue
        {
            const FormulaValue value = Helpers::Scalar(session, arguments[0]);
            if (value.IsError())
            {
                return value;
            }
            if (value.Kind() != FormulaValueKind::Text)
            {
                return FormulaValue::Error(FormulaErrorCode::Value);
            }
            const auto serial = ParseDateText(value.TextValue());
            if (!serial)
            {
                return FormulaValue::Error(FormulaErrorCode::Value);
            }
            return FormulaValue::Number(*serial - std::floor(*serial));
        });

    const auto datePart = [](std::function<FormulaValue(const ExcelDateSerial::DateParts&)> body)
    {
        return [body = std::move(body)](FormulaEvaluationSession& session,
                                        std::span<EvalValue> arguments) -> FormulaValue
        {
            const NumberArg serial = FormulaLibraryDetail::GetNumber(session, arguments[0]);
            if (serial.failed)
            {
                return serial.error;
            }
            const auto parts = ExcelDateSerial::ToParts(serial.value);
            if (!parts)
            {
                return FormulaValue::Error(FormulaErrorCode::Num);
            }
            return body(*parts);
        };
    };
    Add(functions, "YEAR", 1, 1, datePart([](const ExcelDateSerial::DateParts& parts)
                                          { return FormulaValue::Number(static_cast<Real>(parts.year)); }));
    Add(functions, "MONTH", 1, 1, datePart([](const ExcelDateSerial::DateParts& parts)
                                           { return FormulaValue::Number(static_cast<Real>(parts.month)); }));
    Add(functions, "DAY", 1, 1, datePart([](const ExcelDateSerial::DateParts& parts)
                                         { return FormulaValue::Number(static_cast<Real>(parts.day)); }));
    Add(functions, "HOUR", 1, 1, datePart([](const ExcelDateSerial::DateParts& parts)
                                          { return FormulaValue::Number(static_cast<Real>(parts.hour)); }));
    Add(functions, "MINUTE", 1, 1, datePart([](const ExcelDateSerial::DateParts& parts)
                                            { return FormulaValue::Number(static_cast<Real>(parts.minute)); }));
    Add(functions, "SECOND", 1, 1, datePart([](const ExcelDateSerial::DateParts& parts)
                                            { return FormulaValue::Number(std::floor(parts.second)); }));

    Add(functions, "WEEKDAY", 1, 2,
        [](FormulaEvaluationSession& session, std::span<EvalValue> arguments) -> FormulaValue
        {
            const NumberArg serial = GetNumber(session, arguments[0]);
            if (serial.failed)
            {
                return serial.error;
            }
            if (serial.value < 0.0)
            {
                return FormulaValue::Error(FormulaErrorCode::Num);
            }
            Real type = 1.0;
            if (arguments.size() >= 2)
            {
                const NumberArg typeArg = GetNumber(session, arguments[1]);
                if (typeArg.failed)
                {
                    return typeArg.error;
                }
                type = std::floor(typeArg.value);
            }
            // The 1900 date system treats serial 1 as a Sunday (the fictitious
            // 1900-02-29 compensates, so modern dates come out correctly).
            const auto days = static_cast<Int64>(std::floor(serial.value));
            const Int64 sundayIndex = ((days - 1) % 7 + 7) % 7; // 0 = Sunday
            if (type == 1.0)
            {
                return FormulaValue::Number(static_cast<Real>(sundayIndex + 1));
            }
            if (type == 2.0)
            {
                return FormulaValue::Number(static_cast<Real>((sundayIndex + 6) % 7 + 1));
            }
            if (type == 3.0)
            {
                return FormulaValue::Number(static_cast<Real>((sundayIndex + 6) % 7));
            }
            return FormulaValue::Error(FormulaErrorCode::Num);
        });

    Add(functions, "WEEKNUM", 1, 2,
        [](FormulaEvaluationSession& session, std::span<EvalValue> arguments) -> FormulaValue
        {
            const NumberArg serial = GetNumber(session, arguments[0]);
            if (serial.failed)
            {
                return serial.error;
            }
            Real type = 1.0;
            if (arguments.size() >= 2)
            {
                const NumberArg typeArg = GetNumber(session, arguments[1]);
                if (typeArg.failed)
                {
                    return typeArg.error;
                }
                type = std::floor(typeArg.value);
            }
            if (type != 1.0 && type != 2.0)
            {
                return FormulaValue::Error(FormulaErrorCode::Num);
            }
            const auto parts = ExcelDateSerial::ToParts(serial.value);
            if (!parts)
            {
                return FormulaValue::Error(FormulaErrorCode::Num);
            }
            const auto january1 = ExcelDateSerial::FromParts(parts->year, 1, 1);
            if (!january1)
            {
                return FormulaValue::Error(FormulaErrorCode::Num);
            }
            const auto days = static_cast<Int64>(std::floor(serial.value)) -
                              static_cast<Int64>(*january1);
            // Weekday of January 1st with week start Sunday (type 1) or
            // Monday (type 2); serial 1 counts as a Sunday in the 1900 system.
            const Int64 january1Days = static_cast<Int64>(*january1);
            const Int64 sundayIndex = ((january1Days - 1) % 7 + 7) % 7; // 0 = Sunday
            const Int64 weekStartIndex = type == 1.0 ? sundayIndex : (sundayIndex + 6) % 7;
            // Week numbers are whole weeks, so the division is deliberately integral.
            const Int64 weekNumber = (days + weekStartIndex) / 7 + 1;
            return FormulaValue::Number(static_cast<Real>(weekNumber));
        });

    const auto monthShift = [](bool endOfMonth)
    {
        return [endOfMonth](FormulaEvaluationSession& session,
                            std::span<EvalValue> arguments) -> FormulaValue
        {
            const NumberArg serial = FormulaLibraryDetail::GetNumber(session, arguments[0]);
            if (serial.failed)
            {
                return serial.error;
            }
            const NumberArg months = FormulaLibraryDetail::GetNumber(session, arguments[1]);
            if (months.failed)
            {
                return months.error;
            }
            const auto parts = ExcelDateSerial::ToParts(serial.value);
            if (!parts)
            {
                return FormulaValue::Error(FormulaErrorCode::Num);
            }
            const auto shift = static_cast<Int64>(std::trunc(months.value));
            // First day of the shifted month, then clamp or move to the end.
            const auto firstOfMonth =
                ExcelDateSerial::FromParts(parts->year, static_cast<Int64>(parts->month) + shift, 1);
            if (!firstOfMonth)
            {
                return FormulaValue::Error(FormulaErrorCode::Num);
            }
            const auto nextMonth = ExcelDateSerial::FromParts(
                parts->year, static_cast<Int64>(parts->month) + shift + 1, 1);
            if (!nextMonth)
            {
                return FormulaValue::Error(FormulaErrorCode::Num);
            }
            const Real lastDay = *nextMonth - 1.0;
            if (endOfMonth)
            {
                return FormulaValue::Number(lastDay);
            }
            const Real dayCount = lastDay - *firstOfMonth + 1.0;
            const Real day = std::min(static_cast<Real>(parts->day), dayCount);
            return FormulaValue::Number(*firstOfMonth + day - 1.0);
        };
    };
    Add(functions, "EDATE", 2, 2, monthShift(false));
    Add(functions, "EOMONTH", 2, 2, monthShift(true));

    Add(functions, "DAYS", 2, 2,
        [](FormulaEvaluationSession& session, std::span<EvalValue> arguments) -> FormulaValue
        {
            const NumberArg end = GetNumber(session, arguments[0]);
            if (end.failed)
            {
                return end.error;
            }
            const NumberArg start = GetNumber(session, arguments[1]);
            if (start.failed)
            {
                return start.error;
            }
            return FormulaValue::Number(std::floor(end.value) - std::floor(start.value));
        });

    Add(functions, "TODAY", 0, 0, [](FormulaEvaluationSession&, std::span<EvalValue>) -> FormulaValue
        {
            const auto serial = CurrentSerial(false);
            if (!serial)
            {
                return FormulaValue::Error(FormulaErrorCode::Value);
            }
            return FormulaValue::Number(std::floor(*serial)); }, true);
    Add(functions, "NOW", 0, 0, [](FormulaEvaluationSession&, std::span<EvalValue>) -> FormulaValue
        {
            const auto serial = CurrentSerial(true);
            if (!serial)
            {
                return FormulaValue::Error(FormulaErrorCode::Value);
            }
            return FormulaValue::Number(*serial); }, true);
}

// ---------------------------------------------------------------------------
// Financial functions
// ---------------------------------------------------------------------------

void FormulaFunctionLibrary::RegisterFinancialFunctions(FunctionMap& functions)
{
    using namespace FormulaLibraryDetail;

    struct AnnuityArguments
    {
        Real values[5] = {0.0, 0.0, 0.0, 0.0, 0.0};
        FormulaValue error;
        bool failed = false;
    };
    // Reads up to five scalar numeric arguments with missing ones as zero.
    const auto readAnnuity = [](FormulaEvaluationSession& session,
                                std::span<EvalValue> arguments) -> AnnuityArguments
    {
        AnnuityArguments result;
        for (Size i = 0; i < arguments.size() && i < 5; ++i)
        {
            const NumberArg value = GetNumber(session, arguments[i]);
            if (value.failed)
            {
                result.error = value.error;
                result.failed = true;
                return result;
            }
            result.values[i] = value.value;
        }
        return result;
    };

    Add(functions, "PMT", 3, 5,
        [readAnnuity](FormulaEvaluationSession& session, std::span<EvalValue> arguments) -> FormulaValue
        {
            const AnnuityArguments a = readAnnuity(session, arguments);
            if (a.failed)
            {
                return a.error;
            }
            const Real rate = a.values[0];
            const Real nper = a.values[1];
            const Real pv = a.values[2];
            const Real fv = a.values[3];
            const int type = a.values[4] != 0.0 ? 1 : 0;
            if (nper == 0.0)
            {
                return FormulaValue::Error(FormulaErrorCode::Num);
            }
            if (rate == 0.0)
            {
                return FormulaValue::Number(-(pv + fv) / nper);
            }
            const Real growth = std::pow(1.0 + rate, nper);
            const Real payment = -(pv * growth + fv) * rate / ((1.0 + rate * type) * (growth - 1.0));
            if (!std::isfinite(payment))
            {
                return FormulaValue::Error(FormulaErrorCode::Num);
            }
            return FormulaValue::Number(payment);
        });

    const auto paymentPart = [](bool interestPart)
    {
        return [interestPart](FormulaEvaluationSession& session,
                              std::span<EvalValue> arguments) -> FormulaValue
        {
            // Arguments: rate, per, nper, pv, [fv], [type]
            const NumberArg rateArg = GetNumber(session, arguments[0]);
            if (rateArg.failed)
            {
                return rateArg.error;
            }
            const NumberArg perArg = GetNumber(session, arguments[1]);
            if (perArg.failed)
            {
                return perArg.error;
            }
            const NumberArg nperArg = GetNumber(session, arguments[2]);
            if (nperArg.failed)
            {
                return nperArg.error;
            }
            const NumberArg pvArg = GetNumber(session, arguments[3]);
            if (pvArg.failed)
            {
                return pvArg.error;
            }
            Real fv = 0.0;
            Real typeValue = 0.0;
            if (arguments.size() >= 5)
            {
                const NumberArg fvArg = GetNumber(session, arguments[4]);
                if (fvArg.failed)
                {
                    return fvArg.error;
                }
                fv = fvArg.value;
            }
            if (arguments.size() >= 6)
            {
                const NumberArg typeArg = GetNumber(session, arguments[5]);
                if (typeArg.failed)
                {
                    return typeArg.error;
                }
                typeValue = typeArg.value;
            }
            const Real rate = rateArg.value;
            const Real per = std::floor(perArg.value);
            const Real nper = nperArg.value;
            const Real pv = pvArg.value;
            const int type = typeValue != 0.0 ? 1 : 0;
            if (per < 1.0 || per > nper || nper <= 0.0 || nper > 1e6)
            {
                return FormulaValue::Error(FormulaErrorCode::Num);
            }

            Real payment;
            if (rate == 0.0)
            {
                payment = -(pv + fv) / nper;
            }
            else
            {
                const Real growth = std::pow(1.0 + rate, nper);
                payment = -(pv * growth + fv) * rate / ((1.0 + rate * type) * (growth - 1.0));
            }

            // Roll the balance forward period by period. With type 1 the
            // payment is applied at the beginning of each period and the
            // first period accrues no interest by definition.
            Real balance = pv;
            Real interest = 0.0;
            // 'per' is validated against nper above, so the whole periods it covers
            // fit an integer counter; a floating-point one would only risk drift.
            const Int64 periods = static_cast<Int64>(std::floor(per));
            for (Int64 k = 1; k <= periods; ++k)
            {
                if (type == 1)
                {
                    interest = k == 1 ? 0.0 : -((balance + payment) * rate);
                    balance = (balance + payment) * (1.0 + rate);
                }
                else
                {
                    interest = -(balance * rate);
                    balance = balance * (1.0 + rate) + payment;
                }
            }
            if (!std::isfinite(interest) || !std::isfinite(payment))
            {
                return FormulaValue::Error(FormulaErrorCode::Num);
            }
            return FormulaValue::Number(interestPart ? interest : payment - interest);
        };
    };
    Add(functions, "IPMT", 4, 6, paymentPart(true));
    Add(functions, "PPMT", 4, 6, paymentPart(false));

    Add(functions, "FV", 3, 5,
        [readAnnuity](FormulaEvaluationSession& session, std::span<EvalValue> arguments) -> FormulaValue
        {
            const AnnuityArguments a = readAnnuity(session, arguments);
            if (a.failed)
            {
                return a.error;
            }
            const Real rate = a.values[0];
            const Real nper = a.values[1];
            const Real pmt = a.values[2];
            const Real pv = a.values[3];
            const int type = a.values[4] != 0.0 ? 1 : 0;
            const Real result = -AnnuityFutureValue(rate, nper, pmt, pv, type);
            if (!std::isfinite(result))
            {
                return FormulaValue::Error(FormulaErrorCode::Num);
            }
            return FormulaValue::Number(result);
        });

    Add(functions, "PV", 3, 5,
        [readAnnuity](FormulaEvaluationSession& session, std::span<EvalValue> arguments) -> FormulaValue
        {
            const AnnuityArguments a = readAnnuity(session, arguments);
            if (a.failed)
            {
                return a.error;
            }
            const Real rate = a.values[0];
            const Real nper = a.values[1];
            const Real pmt = a.values[2];
            const Real fv = a.values[3];
            const int type = a.values[4] != 0.0 ? 1 : 0;
            Real result;
            if (rate == 0.0)
            {
                result = -fv - pmt * nper;
            }
            else
            {
                const Real growth = std::pow(1.0 + rate, nper);
                result = -(fv + pmt * (1.0 + rate * type) * (growth - 1.0) / rate) / growth;
            }
            if (!std::isfinite(result))
            {
                return FormulaValue::Error(FormulaErrorCode::Num);
            }
            return FormulaValue::Number(result);
        });

    Add(functions, "NPER", 3, 5,
        [readAnnuity](FormulaEvaluationSession& session, std::span<EvalValue> arguments) -> FormulaValue
        {
            const AnnuityArguments a = readAnnuity(session, arguments);
            if (a.failed)
            {
                return a.error;
            }
            const Real rate = a.values[0];
            const Real pmt = a.values[1];
            const Real pv = a.values[2];
            const Real fv = a.values[3];
            const int type = a.values[4] != 0.0 ? 1 : 0;
            if (rate == 0.0)
            {
                if (pmt == 0.0)
                {
                    return FormulaValue::Error(FormulaErrorCode::Num);
                }
                return FormulaValue::Number(-(pv + fv) / pmt);
            }
            const Real adjusted = pmt * (1.0 + rate * type) / rate;
            const Real numerator = adjusted - fv;
            const Real denominator = pv + adjusted;
            if (numerator == 0.0 || denominator == 0.0 || numerator / denominator <= 0.0)
            {
                return FormulaValue::Error(FormulaErrorCode::Num);
            }
            const Real result = std::log(numerator / denominator) / std::log(1.0 + rate);
            if (!std::isfinite(result))
            {
                return FormulaValue::Error(FormulaErrorCode::Num);
            }
            return FormulaValue::Number(result);
        });

    Add(functions, "RATE", 3, 6,
        [](FormulaEvaluationSession& session, std::span<EvalValue> arguments) -> FormulaValue
        {
            // rate is solved from: nper, pmt, pv, [fv], [type], [guess]
            const NumberArg nperArg = GetNumber(session, arguments[0]);
            if (nperArg.failed)
            {
                return nperArg.error;
            }
            const NumberArg pmtArg = GetNumber(session, arguments[1]);
            if (pmtArg.failed)
            {
                return pmtArg.error;
            }
            const NumberArg pvArg = GetNumber(session, arguments[2]);
            if (pvArg.failed)
            {
                return pvArg.error;
            }
            Real fv = 0.0;
            Real typeValue = 0.0;
            Real guess = 0.1;
            if (arguments.size() >= 4)
            {
                const NumberArg fvArg = GetNumber(session, arguments[3]);
                if (fvArg.failed)
                {
                    return fvArg.error;
                }
                fv = fvArg.value;
            }
            if (arguments.size() >= 5)
            {
                const NumberArg typeArg = GetNumber(session, arguments[4]);
                if (typeArg.failed)
                {
                    return typeArg.error;
                }
                typeValue = typeArg.value;
            }
            if (arguments.size() >= 6)
            {
                const NumberArg guessArg = GetNumber(session, arguments[5]);
                if (guessArg.failed)
                {
                    return guessArg.error;
                }
                guess = guessArg.value;
            }
            const Real nper = nperArg.value;
            const Real pmt = pmtArg.value;
            const Real pv = pvArg.value;
            const int type = typeValue != 0.0 ? 1 : 0;
            if (nper <= 0.0)
            {
                return FormulaValue::Error(FormulaErrorCode::Num);
            }
            const auto equation = [=](Real rate)
            {
                return AnnuityFutureValue(rate, nper, pmt, pv, type) + fv;
            };
            const auto rate = SolveRate(equation, guess);
            if (!rate)
            {
                return FormulaValue::Error(FormulaErrorCode::Num);
            }
            return FormulaValue::Number(*rate);
        });

    Add(functions, "NPV", 2, 255,
        [](FormulaEvaluationSession& session, std::span<EvalValue> arguments) -> FormulaValue
        {
            const NumberArg rateArg = GetNumber(session, arguments[0]);
            if (rateArg.failed)
            {
                return rateArg.error;
            }
            const Real rate = rateArg.value;
            if (rate == -1.0)
            {
                return FormulaValue::Error(FormulaErrorCode::Div0);
            }
            std::vector<Real> values;
            for (Size i = 1; i < arguments.size(); ++i)
            {
                if (const auto error = CollectOrderedNumbers(session, arguments[i], values))
                {
                    return *error;
                }
            }
            Real result = 0.0;
            Real discount = 1.0;
            for (const Real value : values)
            {
                discount *= 1.0 + rate;
                result += value / discount;
            }
            if (!std::isfinite(result))
            {
                return FormulaValue::Error(FormulaErrorCode::Num);
            }
            return FormulaValue::Number(result);
        });

    Add(functions, "IRR", 1, 2,
        [](FormulaEvaluationSession& session, std::span<EvalValue> arguments) -> FormulaValue
        {
            std::vector<Real> values;
            if (const auto error = CollectOrderedNumbers(session, arguments[0], values))
            {
                return *error;
            }
            Real guess = 0.1;
            if (arguments.size() >= 2)
            {
                const NumberArg guessArg = GetNumber(session, arguments[1]);
                if (guessArg.failed)
                {
                    return guessArg.error;
                }
                guess = guessArg.value;
            }
            bool hasPositive = false;
            bool hasNegative = false;
            for (const Real value : values)
            {
                hasPositive = hasPositive || value > 0.0;
                hasNegative = hasNegative || value < 0.0;
            }
            if (!hasPositive || !hasNegative)
            {
                return FormulaValue::Error(FormulaErrorCode::Num);
            }
            const auto netPresentValue = [&values](Real rate)
            {
                Real result = 0.0;
                Real discount = 1.0;
                for (const Real value : values)
                {
                    result += value / discount;
                    discount *= 1.0 + rate;
                }
                return result;
            };
            const auto rate = SolveRate(netPresentValue, guess);
            if (!rate)
            {
                return FormulaValue::Error(FormulaErrorCode::Num);
            }
            return FormulaValue::Number(*rate);
        });
}

} // namespace ExyokiOffice::Excel
