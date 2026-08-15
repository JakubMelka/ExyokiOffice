// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "ExyokiOffice/OpenXmlSimpleTypes.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <doctest/doctest.h>

#include <chrono>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

using namespace ExyokiOffice;

namespace
{

template <typename TValue>
void CheckIntegralBoundaries(typename TValue::value_type minimum,
                             typename TValue::value_type maximum)
{
    const auto minimumText = std::to_string(minimum);
    const auto maximumText = std::to_string(maximum);

    TValue minimumValue{std::string_view(minimumText)};
    TValue maximumValue{std::string_view(maximumText)};

    REQUIRE(minimumValue.IsDefined());
    REQUIRE(maximumValue.IsDefined());
    CHECK(minimumValue.Value() == minimum);
    CHECK(maximumValue.Value() == maximum);
    CHECK(minimumValue.ToString() == minimumText);
    CHECK(maximumValue.ToString() == maximumText);
}

/**
 * @brief Checks that every spelling parses to @p expected and normalizes to @p canonical.
 *
 * The canonical form is also fed back through the same type, because a value the
 * library writes has to be one the library can read.
 */
template <typename TValue>
void CheckAcceptedTexts(std::initializer_list<std::string_view> texts, bool expected, std::string_view canonical)
{
    for (const auto text : texts)
    {
        CAPTURE(text);
        TValue value{std::string_view(text)};
        REQUIRE(value.IsDefined());
        CHECK(value.Value() == expected);
        CHECK(value.ToString() == canonical);

        TValue reparsed{std::string_view(value.ToString())};
        REQUIRE(reparsed.IsDefined());
        CHECK(reparsed.Value() == expected);
    }
}

template <typename TValue>
void CheckRejectedTexts(std::initializer_list<std::string_view> texts)
{
    for (const auto text : texts)
    {
        CAPTURE(text);
        TValue value(typename TValue::value_type{});
        REQUIRE(value.IsDefined());
        CHECK_FALSE(value.AssignFromString(text));
        CHECK_FALSE(value.IsDefined());
        CHECK(value.ToString().empty());
    }
}

struct CaseInsensitiveTraits
{
    static bool TryParse(std::string_view text, std::string& value) noexcept
    {
        value.assign(text);
        return !text.empty();
    }

    static bool Validate(const std::string& value) noexcept
    {
        return value.size() <= 4;
    }

    static bool Equals(const std::string& left, const std::string& right) noexcept
    {
        if (left.size() != right.size())
        {
            return false;
        }

        for (Size index = 0; index < left.size(); ++index)
        {
            const auto normalize = [](char character)
            {
                return character >= 'A' && character <= 'Z'
                           ? static_cast<char>(character - 'A' + 'a')
                           : character;
            };
            if (normalize(left[index]) != normalize(right[index]))
            {
                return false;
            }
        }
        return true;
    }

    static std::string Format(const std::string& value)
    {
        return value;
    }
};

using ValidatedValue = detail::SimpleValue<std::string, CaseInsensitiveTraits>;

class TestEnum : public OpenXmlEnum
{
public:
    enum class Value : UInt32
    {
        Alpha,
        Beta,
        Invalid
    };

    struct Meta
    {
        UInt32 FromString(std::string_view text) const
        {
            if (text == "alpha")
            {
                return static_cast<UInt32>(Value::Alpha);
            }
            if (text == "beta")
            {
                return static_cast<UInt32>(Value::Beta);
            }
            return static_cast<UInt32>(Value::Invalid);
        }

        std::string_view ToString(UInt32 rawValue) const
        {
            switch (static_cast<Value>(rawValue))
            {
                case Value::Alpha:
                    return "alpha";
                case Value::Beta:
                    return "beta";
                default:
                    return {};
            }
        }
    };

    TestEnum() = default;
    explicit TestEnum(Value value)
        : m_value(value)
    {
    }

    static const Meta* GetMetaEnum()
    {
        static const Meta meta;
        return &meta;
    }

    bool IsValid() const
    {
        return m_value != Value::Invalid;
    }

    Value GetValue() const
    {
        return m_value;
    }

    // Not `= default`: OpenXmlEnum has no operator==, so a defaulted one is
    // implicitly deleted and comparing two TestEnum values would not compile.
    friend bool operator==(const TestEnum& left, const TestEnum& right)
    {
        return left.m_value == right.m_value;
    }

private:
    Value m_value{Value::Invalid};
};

} // namespace

TEST_CASE("SimpleValue manages undefined, assigned, reset, and comparison states")
{
    Int32Value undefined;
    Int32Value alsoUndefined;
    Int32 output = 99;

    CHECK_FALSE(undefined.IsDefined());
    CHECK(undefined.Value() == 0);
    CHECK(undefined.ValueOr(17) == 17);
    CHECK_FALSE(undefined.TryGet(output));
    CHECK(output == 99);
    CHECK(undefined.ToString().empty());
    CHECK(undefined == alsoUndefined);
    CHECK(undefined != Int32Value(0));

    REQUIRE(undefined.Assign(42));
    CHECK(undefined.IsDefined());
    CHECK(undefined.ValueOr(17) == 42);
    CHECK(undefined.TryGet(output));
    CHECK(output == 42);
    CHECK(undefined == 42);
    CHECK(42 == undefined);
    CHECK(undefined != 41);

    undefined.Reset();
    CHECK_FALSE(undefined.IsDefined());
    CHECK(undefined.Value() == 0);
}

TEST_CASE("SimpleValue supports validation, custom equality, and total ordering")
{
    ValidatedValue lower(std::string_view("test"));
    ValidatedValue upper(std::string_view("TEST"));
    ValidatedValue invalid(std::string_view("longer"));

    CHECK(lower == upper);
    CHECK_FALSE(invalid.IsDefined());
    CHECK(lower.ToString() == "test");

    Int32Value undefined;
    Int32Value one(1);
    Int32Value two(2);
    CHECK(undefined < one);
    CHECK(undefined <= one);
    CHECK(one > undefined);
    CHECK(one >= undefined);
    CHECK(one < two);
    CHECK(two > one);
    CHECK(one <= one);
    CHECK(two >= two);
}

TEST_CASE("StringValue distinguishes undefined, borrowed, and owned storage")
{
    StringValue undefined;
    CHECK_FALSE(undefined.IsDefined());
    CHECK_FALSE(undefined.IsView());
    CHECK_FALSE(undefined.IsOwned());
    CHECK(undefined.View().empty());
    CHECK(undefined.ToString().empty());

    StringValue nullValue(static_cast<const char*>(nullptr));
    CHECK_FALSE(nullValue.IsDefined());

    std::string source = "borrowed";
    StringValue borrowed{std::string_view(source)};
    CHECK(borrowed.IsDefined());
    CHECK(borrowed.IsView());
    CHECK_FALSE(borrowed.IsOwned());
    source[0] = 'B';
    CHECK(borrowed.View() == "Borrowed");

    StringValue owned(std::string("owned"));
    CHECK(owned.IsOwned());
    CHECK(owned.View() == "owned");
    CHECK(owned == StringValue("owned"));
    CHECK(owned != borrowed);

    REQUIRE(borrowed.AssignFromString("copied"));
    CHECK(borrowed.IsOwned());
    CHECK(borrowed.ToString() == "copied");
    borrowed.Reset();
    CHECK_FALSE(borrowed.IsDefined());

    StringValue empty(std::string(""));
    CHECK(empty.IsDefined());
    CHECK(empty.IsOwned());
    CHECK(empty.ToString().empty());
    CHECK(empty != undefined);
}

TEST_CASE("All integral simple types accept exact boundaries")
{
    CheckIntegralBoundaries<ByteValue>(0, std::numeric_limits<UInt8>::max());
    CheckIntegralBoundaries<SByteValue>(std::numeric_limits<Int8>::min(),
                                        std::numeric_limits<Int8>::max());
    CheckIntegralBoundaries<Int16Value>(std::numeric_limits<Int16>::min(),
                                        std::numeric_limits<Int16>::max());
    CheckIntegralBoundaries<UInt16Value>(0, std::numeric_limits<UInt16>::max());
    CheckIntegralBoundaries<Int32Value>(std::numeric_limits<Int32>::min(),
                                        std::numeric_limits<Int32>::max());
    CheckIntegralBoundaries<UInt32Value>(0, std::numeric_limits<UInt32>::max());
    CheckIntegralBoundaries<Int64Value>(std::numeric_limits<Int64>::min(),
                                        std::numeric_limits<Int64>::max());
    CheckIntegralBoundaries<UInt64Value>(0, std::numeric_limits<UInt64>::max());
    CheckIntegralBoundaries<IntegerValue>(std::numeric_limits<Int64>::min(),
                                          std::numeric_limits<Int64>::max());
}

TEST_CASE("Integral simple types reject malformed and out-of-range lexical values")
{
    CheckRejectedTexts<Int32Value>({"", " ", "+1", "1 ", " 1", "1.0", "1x", "--1",
                                    "2147483648", "-2147483649"});
    CheckRejectedTexts<UInt32Value>({"-1", "4294967296"});
    CheckRejectedTexts<ByteValue>({"-1", "256"});
    CheckRejectedTexts<SByteValue>({"-129", "128"});

    Int32Value leadingZero(std::string_view("0012"));
    REQUIRE(leadingZero.IsDefined());
    CHECK(leadingZero.Value() == 12);
    CHECK(leadingZero.ToString() == "12");
}

TEST_CASE("Floating and decimal values parse, format, and reject partial input")
{
    DoubleValue doubleValue(std::string_view("-1.25e2"));
    SingleValue singleValue(std::string_view("0.5"));
    DecimalValue decimalValue(std::string_view("123.125"));

    REQUIRE(doubleValue.IsDefined());
    REQUIRE(singleValue.IsDefined());
    REQUIRE(decimalValue.IsDefined());
    CHECK(doubleValue.Value() == doctest::Approx(-125.0));
    CHECK(singleValue.Value() == doctest::Approx(0.5f));
    CHECK(static_cast<Real>(decimalValue.Value()) == doctest::Approx(123.125));
    CHECK(DoubleValue(std::string_view(doubleValue.ToString())).Value() ==
          doctest::Approx(doubleValue.Value()));
    CHECK(DecimalValue(std::string_view(decimalValue.ToString())).Value() ==
          doctest::Approx(static_cast<Real>(decimalValue.Value())));

    CheckRejectedTexts<DoubleValue>({"", " ", "1.2x", "1,2"});
    CheckRejectedTexts<SingleValue>({"", "1f", "1.0 "});
    CheckRejectedTexts<DecimalValue>({"", " ", "1.2x", "1,2"});
}

TEST_CASE("Boolean lexical families accept only their documented spellings")
{
    // Each family admits exactly the lexical space of its schema type and nothing
    // else. The four spaces genuinely differ, so a spelling valid for one is
    // deliberately invalid for another: `1` is a boolean but not an ST_TrueFalse,
    // `on` is an ST_OnOff but not a boolean, the blank belongs to ST_TrueFalseBlank
    // alone. Case matters everywhere - none of these types is case-insensitive.

    SUBCASE("xsd:boolean is true/false/1/0")
    {
        CheckAcceptedTexts<BooleanValue>({"1", "true"}, true, "1");
        CheckAcceptedTexts<BooleanValue>({"0", "false"}, false, "0");

        // `True`/`False` are .NET's bool.ToString(), not xsd:boolean literals.
        CheckRejectedTexts<BooleanValue>({"True", "False", "TRUE", "FALSE"});
        CheckRejectedTexts<BooleanValue>({"", " ", "yes", "no", "t", "f", "2", "-1", "01"});
        CheckRejectedTexts<BooleanValue>({" true", "true ", "tr ue"});
        // The on/off branch belongs to ST_OnOff, which is a different type.
        CheckRejectedTexts<BooleanValue>({"on", "off"});
    }

    SUBCASE("ST_OnOff adds on/off to xsd:boolean and nothing more")
    {
        CheckAcceptedTexts<OnOffValue>({"true", "1", "on"}, true, "true");
        CheckAcceptedTexts<OnOffValue>({"false", "0", "off"}, false, "false");

        // Neither branch of the union has an empty member: `val=""` is malformed,
        // not a definite false.
        CheckRejectedTexts<OnOffValue>({""});
        CheckRejectedTexts<OnOffValue>({"True", "False", "On", "Off", "ON", "OFF"});
        CheckRejectedTexts<OnOffValue>({" ", "yes", "no", "t", "f", "2", " on", "on "});
    }

    SUBCASE("ST_TrueFalse is t/f/true/false")
    {
        CheckAcceptedTexts<TrueFalseValue>({"true", "t"}, true, "true");
        CheckAcceptedTexts<TrueFalseValue>({"false", "f"}, false, "false");

        CheckRejectedTexts<TrueFalseValue>({"", "1", "0", "on", "off"});
        CheckRejectedTexts<TrueFalseValue>({"True", "False", "T", "F", "yes"});
    }

    SUBCASE("ST_TrueFalseBlank is ST_TrueFalse plus the blank")
    {
        CheckAcceptedTexts<TrueFalseBlankValue>({"true", "t"}, true, "true");
        // The blank is a member of this type - it is what the type name says.
        CheckAcceptedTexts<TrueFalseBlankValue>({"false", "f", ""}, false, "false");

        CheckRejectedTexts<TrueFalseBlankValue>({"1", "0", "on", "off"});
        CheckRejectedTexts<TrueFalseBlankValue>({"True", "False", "T", "F", " "});
    }
}

TEST_CASE("a boolean outside its lexical space reads as unset, never as false")
{
    // This is the whole point of rejecting a malformed spelling rather than
    // charitably mapping it onto false: a caller asking "is this on?" must be able
    // to tell "the document said off" from "the document said something invalid".
    // ValueOr(true) surfaces the difference - a silent false would return false.
    for (const auto text : {"", "True", "yes"})
    {
        CAPTURE(text);
        OnOffValue value{std::string_view(text)};
        CHECK_FALSE(value.IsDefined());
        CHECK(value.ValueOr(true));
        CHECK_FALSE(value.ValueOr(false));

        bool target = true;
        CHECK_FALSE(value.TryGet(target));
        CHECK(target);
    }

    // Assigning a malformed value over a good one clears the holder rather than
    // leaving the previous value in place, so a failed parse cannot be mistaken
    // for a successful one by a caller that ignores the return value.
    BooleanValue value{true};
    REQUIRE(value.IsDefined());
    CHECK_FALSE(value.AssignFromString("True"));
    CHECK_FALSE(value.IsDefined());
    CHECK(value.ValueOr(false) == false);
}

TEST_CASE("DateTimeValue normalizes UTC offsets and fractional seconds")
{
    DateTimeValue utc(std::string_view("2024-02-29T12:34:56Z"));
    DateTimeValue positiveOffset(std::string_view("2024-02-29T14:34:56+02:00"));
    DateTimeValue negativeOffset(std::string_view("2024-02-29T07:04:56-05:30"));
    DateTimeValue noZone(std::string_view("2024-02-29T12:34:56"));
    DateTimeValue lowercaseZulu(std::string_view("2024-02-29T12:34:56z"));

    REQUIRE(utc.IsDefined());
    CHECK(positiveOffset == utc);
    CHECK(negativeOffset == utc);
    CHECK(noZone == utc);
    CHECK(lowercaseZulu == utc);
    CHECK(utc.ToString() == "2024-02-29T12:34:56Z");

    DateTimeValue fraction(std::string_view("1970-01-01T00:00:00.123400000Z"));
    REQUIRE(fraction.IsDefined());
    CHECK(fraction.ToString() == "1970-01-01T00:00:00.1234Z");

    DateTimeValue truncated(std::string_view("1970-01-01T00:00:00.1234567899Z"));
    DateTimeValue platformPrecision(std::string_view("1970-01-01T00:00:00.123456789Z"));
    REQUIRE(truncated.IsDefined());
    CHECK(truncated == platformPrecision);
    CHECK(DateTimeValue(std::string_view(truncated.ToString())) == truncated);

    const DateTimeValue beforeEpoch(
        std::chrono::system_clock::time_point(std::chrono::milliseconds(-1)));
    CHECK(beforeEpoch.ToString() == "1969-12-31T23:59:59.999Z");
}

TEST_CASE("DateTimeValue rejects structurally invalid lexical forms")
{
    CheckRejectedTexts<DateTimeValue>({"",
                                       "2024-01-01",
                                       "2024-01-01 00:00:00Z",
                                       "2024/01/01T00:00:00Z",
                                       "2024-01-01T00:00Z",
                                       "2024-01-01T00:00:00.",
                                       "2024-01-01T00:00:00+0100",
                                       "2024-01-01T00:00:00Zjunk",
                                       "2023-02-29T00:00:00Z",
                                       "2024-00-01T00:00:00Z",
                                       "2024-13-01T00:00:00Z",
                                       "2024-04-31T00:00:00Z",
                                       "2024-01-01T24:00:00Z",
                                       "2024-01-01T00:60:00Z",
                                       "2024-01-01T00:00:60Z",
                                       "2024-01-01T00:00:00+14:01",
                                       "2024-01-01T00:00:00+01:60"});
}

TEST_CASE("Base64BinaryValue handles RFC vectors, whitespace, padding, and failures")
{
    const std::vector<std::pair<std::string_view, std::string_view>> vectors{
        {"Zg==", "f"}, {"Zm8=", "fo"}, {"Zm9v", "foo"}, {"Zm9vYg==", "foob"}, {"Zm9vYmE=", "fooba"}, {"Zm9vYmFy", "foobar"}};

    for (const auto& [encoded, decoded] : vectors)
    {
        CAPTURE(encoded);
        Base64BinaryValue value(encoded);
        REQUIRE(value.IsDefined());
        CHECK(std::string(value.Value().begin(), value.Value().end()) == decoded);
        CHECK(value.ToString() == encoded);
    }

    Base64BinaryValue spaced(std::string_view(" Zm9v\r\nYmFy\t"));
    REQUIRE(spaced.IsDefined());
    CHECK(spaced.ToString() == "Zm9vYmFy");

    Base64BinaryValue empty(std::string_view(""));
    REQUIRE(empty.IsDefined());
    CHECK(empty.Value().empty());
    CHECK(empty.ToString().empty());

    CheckRejectedTexts<Base64BinaryValue>(
        {"A", "AAA", "====", "=AAA", "AA=A", "AA==AAAA", "AA?=", "AAA=="});
}

TEST_CASE("HexBinaryValue accepts both cases, emits uppercase, and rejects invalid input")
{
    HexBinaryValue value(std::string_view("00aBff10"));
    REQUIRE(value.IsDefined());
    CHECK(value.Value() == std::vector<Byte>({0x00, 0xAB, 0xFF, 0x10}));
    CHECK(value.ToString() == "00ABFF10");

    HexBinaryValue empty(std::string_view(""));
    REQUIRE(empty.IsDefined());
    CHECK(empty.Value().empty());
    CHECK(empty.ToString().empty());

    CheckRejectedTexts<HexBinaryValue>({"0", "ABC", "GG", "00 11", "0x12"});
}

TEST_CASE("EnumValue parses and formats valid metadata values")
{
    EnumValue<TestEnum> alpha(std::string_view("alpha"));
    EnumValue<TestEnum> beta{TestEnum(TestEnum::Value::Beta)};
    EnumValue<TestEnum> invalid(std::string_view("unknown"));

    REQUIRE(alpha.IsDefined());
    CHECK(alpha.Value().GetValue() == TestEnum::Value::Alpha);
    CHECK(alpha.ToString() == "alpha");
    CHECK(beta.ToString() == "beta");
    CHECK_FALSE(invalid.IsDefined());
}

TEST_CASE("ListValue parses XML whitespace, iterates, serializes, and clears atomically")
{
    ListValue<Int32Value> values(std::string_view(" \t1  -2\r\n3 "));
    REQUIRE(values.IsDefined());
    CHECK_FALSE(values.Empty());
    CHECK(values.Size() == 3);
    CHECK(values.ToString() == "1 -2 3");

    Int32 sum = 0;
    for (const auto& value : values)
    {
        sum += value.Value();
    }
    CHECK(sum == 2);

    const ListValue<Int32Value> copy{Int32Value(1), Int32Value(-2), Int32Value(3)};
    CHECK(values == copy);

    REQUIRE_FALSE(values.AssignFromString("1 invalid 3"));
    CHECK_FALSE(values.IsDefined());
    CHECK(values.Empty());
    CHECK(values.ToString().empty());

    REQUIRE(values.AssignFromString("   "));
    CHECK_FALSE(values.IsDefined());
    CHECK(values.Empty());

    ListValue<Int32Value> constructed(std::vector<Int32Value>{});
    CHECK_FALSE(constructed.IsDefined());
    constructed.Items().push_back(Int32Value(7));
    CHECK_FALSE(constructed.IsDefined());
    CHECK(constructed.Size() == 1);
    constructed.Items();
    CHECK(constructed.IsDefined());
    constructed.Clear();
    CHECK_FALSE(constructed.IsDefined());
}

TEST_CASE("OpenXmlSimpleValueConvertor covers generic and named conversion entry points")
{
    Int32Value output(7);
    CHECK(OpenXmlSimpleValueConvertor::FromString("42", output));
    CHECK(output == 42);
    CHECK_FALSE(OpenXmlSimpleValueConvertor::FromString("bad", output));
    CHECK_FALSE(output.IsDefined());

    CHECK(OpenXmlSimpleValueConvertor::FromString<Int32Value>("12") == 12);
    CHECK(OpenXmlSimpleValueConvertor::ToString(Int32Value(12)) == "12");
    CHECK(OpenXmlSimpleValueConvertor::ToString(12).empty());

    CHECK(OpenXmlSimpleValueConvertor::GetBooleanValueFromString("true").Value());
    CHECK(OpenXmlSimpleValueConvertor::GetByteValueFromString("255").Value() == 255);
    CHECK(OpenXmlSimpleValueConvertor::GetSByteValueFromString("-128").Value() == -128);
    CHECK(OpenXmlSimpleValueConvertor::GetInt16ValueFromString("-16").Value() == -16);
    CHECK(OpenXmlSimpleValueConvertor::GetInt32ValueFromString("-32").Value() == -32);
    CHECK(OpenXmlSimpleValueConvertor::GetInt64ValueFromString("-64").Value() == -64);
    CHECK(OpenXmlSimpleValueConvertor::GetUInt16ValueFromString("16").Value() == 16);
    CHECK(OpenXmlSimpleValueConvertor::GetUInt32ValueFromString("32").Value() == 32);
    CHECK(OpenXmlSimpleValueConvertor::GetUInt64ValueFromString("64").Value() == 64);
    CHECK(OpenXmlSimpleValueConvertor::GetIntegerValueFromString("-1").Value() == -1);
    CHECK(OpenXmlSimpleValueConvertor::GetDoubleValueFromString("1.5").Value() ==
          doctest::Approx(1.5));
    CHECK(OpenXmlSimpleValueConvertor::GetSingleValueFromString("2.5").Value() ==
          doctest::Approx(2.5f));
    CHECK(static_cast<Real>(
              OpenXmlSimpleValueConvertor::GetDecimalValueFromString("3.5").Value()) ==
          doctest::Approx(3.5));
    CHECK(OpenXmlSimpleValueConvertor::GetDateTimeValueFromString("1970-01-01T00:00:00Z")
              .ToString() == "1970-01-01T00:00:00Z");
    CHECK(OpenXmlSimpleValueConvertor::GetOnOffValueFromString("on").Value());
    CHECK(OpenXmlSimpleValueConvertor::GetTrueFalseValueFromString("t").Value());
    CHECK_FALSE(OpenXmlSimpleValueConvertor::GetTrueFalseBlankValueFromString("").Value());
    CHECK(OpenXmlSimpleValueConvertor::GetBase64BinaryValueFromString("Zg==").ToString() ==
          "Zg==");
    CHECK(OpenXmlSimpleValueConvertor::GetHexBinaryValueFromString("ab").ToString() == "AB");
    CHECK(OpenXmlSimpleValueConvertor::GetStringValueFromString("text").ToString() == "text");
}
