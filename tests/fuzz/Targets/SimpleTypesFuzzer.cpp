// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "FuzzHarness.hpp"
#include "FuzzTargets.hpp"

#include "ExyokiOffice/OpenXmlSimpleTypes.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <string>
#include <string_view>

namespace ExyokiOffice::Fuzz
{

/**
 * @brief Round-trip driver shared by every simple type.
 *
 * The parsers are all declared noexcept, so an escaping exception terminates
 * the process and the fuzzer reports it without any help from the harness.
 * What still needs an explicit check is textual stability: once a value parses,
 * formatting it and parsing the result again must produce the same text.
 * A parser and a formatter that disagree silently corrupt documents on save,
 * which no crash would ever reveal.
 */
class SimpleTypeFuzzHelpers
{
public:
    template <typename TValue>
    static void RoundTrip(std::string_view text)
    {
        TValue value;
        if (!value.AssignFromString(text) || !value.IsDefined())
        {
            return;
        }

        const std::string formatted = value.ToString();

        TValue reparsed;
        EXYOKIOFFICE_FUZZ_CHECK(reparsed.AssignFromString(formatted),
                                "ToString() produced text that AssignFromString() rejects");
        EXYOKIOFFICE_FUZZ_CHECK(reparsed.IsDefined(),
                                "re-parsing ToString() output produced an undefined value");
        EXYOKIOFFICE_FUZZ_CHECK(reparsed.ToString() == formatted,
                                "parse/format round-trip is not stable");
    }
};

int RunSimpleTypes(const UInt8* data, Size size)
{
    if (size > kMaxInputSize)
    {
        return 0;
    }

    ByteTape tape(data, size);
    const UInt8 selector = tape.NextByte();
    const std::string_view text = tape.RestAsText();

    switch (selector % 18)
    {
        case 0:
            SimpleTypeFuzzHelpers::RoundTrip<BooleanValue>(text);
            break;
        case 1:
            SimpleTypeFuzzHelpers::RoundTrip<OnOffValue>(text);
            break;
        case 2:
            SimpleTypeFuzzHelpers::RoundTrip<TrueFalseValue>(text);
            break;
        case 3:
            SimpleTypeFuzzHelpers::RoundTrip<TrueFalseBlankValue>(text);
            break;
        case 4:
            SimpleTypeFuzzHelpers::RoundTrip<ByteValue>(text);
            break;
        case 5:
            SimpleTypeFuzzHelpers::RoundTrip<SByteValue>(text);
            break;
        case 6:
            SimpleTypeFuzzHelpers::RoundTrip<Int16Value>(text);
            break;
        case 7:
            SimpleTypeFuzzHelpers::RoundTrip<Int32Value>(text);
            break;
        case 8:
            SimpleTypeFuzzHelpers::RoundTrip<Int64Value>(text);
            break;
        case 9:
            SimpleTypeFuzzHelpers::RoundTrip<UInt16Value>(text);
            break;
        case 10:
            SimpleTypeFuzzHelpers::RoundTrip<UInt32Value>(text);
            break;
        case 11:
            SimpleTypeFuzzHelpers::RoundTrip<UInt64Value>(text);
            break;
        case 12:
            SimpleTypeFuzzHelpers::RoundTrip<DoubleValue>(text);
            break;
        case 13:
            SimpleTypeFuzzHelpers::RoundTrip<DecimalValue>(text);
            break;
        case 14:
            SimpleTypeFuzzHelpers::RoundTrip<DateTimeValue>(text);
            break;
        case 15:
            SimpleTypeFuzzHelpers::RoundTrip<IntegerValue>(text);
            break;
        case 16:
            SimpleTypeFuzzHelpers::RoundTrip<Base64BinaryValue>(text);
            break;
        default:
            SimpleTypeFuzzHelpers::RoundTrip<HexBinaryValue>(text);
            break;
    }

    // Whitespace-separated lists reach SplitWhitespace and the per-item parse
    // loop, which single values never touch.
    ListValue<Int32Value> intList;
    (void)intList.AssignFromString(text);

    ListValue<StringValue> stringList;
    (void)stringList.AssignFromString(text);

    StringValue string;
    (void)string.AssignFromString(text);

    return 0;
}

} // namespace ExyokiOffice::Fuzz
