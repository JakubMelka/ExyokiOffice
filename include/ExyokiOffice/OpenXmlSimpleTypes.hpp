// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

#include "ExyokiOffice/Export.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <chrono>
#include <cstdint>
#include <cstddef>
#include <initializer_list>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace ExyokiOffice
{

class OpenXmlEnum
{
public:
    inline constexpr OpenXmlEnum() = default;
};

namespace detail
{

template <typename Traits, typename TValue, typename = void>
struct traits_has_validate : std::false_type
{
};

template <typename Traits, typename TValue>
struct traits_has_validate<Traits,
                           TValue,
                           std::void_t<decltype(Traits::Validate(std::declval<const TValue&>()))>>
    : std::true_type
{
};

template <typename Traits, typename TValue, typename = void>
struct traits_has_equals : std::false_type
{
};

template <typename Traits, typename TValue>
struct traits_has_equals<Traits,
                         TValue,
                         std::void_t<decltype(Traits::Equals(std::declval<const TValue&>(),
                                                             std::declval<const TValue&>()))>>
    : std::true_type
{
};

template <typename Traits, typename TValue, typename = void>
struct traits_has_format : std::false_type
{
};

template <typename Traits, typename TValue>
struct traits_has_format<Traits,
                         TValue,
                         std::void_t<decltype(Traits::Format(std::declval<const TValue&>()))>>
    : std::true_type
{
};

template <typename T, typename = void>
struct has_less_operator : std::false_type
{
};

template <typename T>
struct has_less_operator<T,
                         std::void_t<decltype(std::declval<const T&>() < std::declval<const T&>())>>
    : std::true_type
{
};

template <typename T, typename = void>
struct has_greater_operator : std::false_type
{
};

template <typename T>
struct has_greater_operator<
    T,
    std::void_t<decltype(std::declval<const T&>() > std::declval<const T&>())>>
    : std::true_type
{
};

template <typename T, typename = void>
struct has_to_string_method : std::false_type
{
};

template <typename T>
struct has_to_string_method<T,
                            std::void_t<decltype(std::declval<const T&>().ToString())>>
    : std::true_type
{
};

template <typename TValue, typename Traits>
class SimpleValue
{
public:
    using value_type = TValue;

    SimpleValue() = default;
    explicit SimpleValue(const TValue& value) noexcept
    {
        Assign(value);
    }
    explicit SimpleValue(std::string_view text) noexcept
    {
        AssignFromString(text);
    }

    [[nodiscard]] bool IsDefined() const noexcept
    {
        return m_defined;
    }

    const TValue& Value() const noexcept
    {
        return m_value;
    }

    TValue ValueOr(const TValue& fallback) const noexcept
    {
        return m_defined ? m_value : fallback;
    }

    [[nodiscard]] bool TryGet(TValue& output) const noexcept
    {
        if (!m_defined)
        {
            return false;
        }

        output = m_value;
        return true;
    }

    void Reset() noexcept
    {
        m_defined = false;
        m_value = TValue();
    }

    bool Assign(const TValue& value) noexcept
    {
        if constexpr (traits_has_validate<Traits, TValue>::value)
        {
            if (!Traits::Validate(value))
            {
                Reset();
                return false;
            }
        }

        m_value = value;
        m_defined = true;
        return true;
    }

    bool AssignFromString(std::string_view text) noexcept
    {
        TValue parsed{};
        if (!Traits::TryParse(text, parsed))
        {
            Reset();
            return false;
        }

        return Assign(parsed);
    }

    std::string ToString() const
    {
        if (!m_defined)
        {
            return {};
        }

        if constexpr (traits_has_format<Traits, TValue>::value)
        {
            return Traits::Format(m_value);
        }
        else
        {
            return {};
        }
    }

    friend bool operator==(const SimpleValue& left, const SimpleValue& right) noexcept
    {
        if (!left.m_defined && !right.m_defined)
        {
            return true;
        }

        if (left.m_defined != right.m_defined)
        {
            return false;
        }

        if constexpr (traits_has_equals<Traits, TValue>::value)
        {
            return Traits::Equals(left.m_value, right.m_value);
        }
        else
        {
            return left.m_value == right.m_value;
        }
    }

    friend bool operator!=(const SimpleValue& left, const SimpleValue& right) noexcept
    {
        return !(left == right);
    }

    friend bool operator==(const SimpleValue& left, const TValue& right) noexcept
    {
        return left.m_defined && left.m_value == right;
    }

    friend bool operator==(const TValue& left, const SimpleValue& right) noexcept
    {
        return right == left;
    }

    friend bool operator!=(const SimpleValue& left, const TValue& right) noexcept
    {
        return !(left == right);
    }

    friend bool operator!=(const TValue& left, const SimpleValue& right) noexcept
    {
        return !(right == left);
    }

    template <typename Q = TValue, typename = std::enable_if_t<has_less_operator<Q>::value>>
    friend bool operator<(const SimpleValue& left, const SimpleValue& right) noexcept
    {
        if (!left.m_defined)
        {
            return right.m_defined;
        }

        if (!right.m_defined)
        {
            return false;
        }

        return left.m_value < right.m_value;
    }

    template <typename Q = TValue, typename = std::enable_if_t<has_less_operator<Q>::value>>
    friend bool operator<=(const SimpleValue& left, const SimpleValue& right) noexcept
    {
        return !(right < left);
    }

    template <typename Q = TValue, typename = std::enable_if_t<has_greater_operator<Q>::value>>
    friend bool operator>(const SimpleValue& left, const SimpleValue& right) noexcept
    {
        return right < left;
    }

    template <typename Q = TValue, typename = std::enable_if_t<has_greater_operator<Q>::value>>
    friend bool operator>=(const SimpleValue& left, const SimpleValue& right) noexcept
    {
        return !(left < right);
    }

private:
    TValue m_value{};
    bool m_defined{false};
};

template <typename TInt>
struct EXYOKIOFFICE_EXPORT IntegralValueTraits
{
    [[nodiscard]] static bool TryParse(std::string_view text, TInt& value) noexcept;
    static std::string Format(TInt value);
};

template <typename TFloat>
struct EXYOKIOFFICE_EXPORT FloatingValueTraits
{
    [[nodiscard]] static bool TryParse(std::string_view text, TFloat& value) noexcept;
    static std::string Format(TFloat value);
};

/**
 * @brief `xsd:boolean`: exactly `true`, `false`, `1`, `0`; writes `1`/`0`.
 *
 * The four boolean families below differ only in which spellings they admit, and
 * each admits exactly what its schema type does - nothing is accepted "to be
 * helpful". A value outside the lexical space leaves the holder undefined, which
 * is how a caller tells a malformed attribute from an absent one; the original
 * text is still written back verbatim as long as nobody assigns over it.
 */
struct EXYOKIOFFICE_EXPORT BooleanTextTraits
{
    [[nodiscard]] static bool TryParse(std::string_view text, bool& value) noexcept;
    static std::string Format(bool value);
};

/** @brief `ST_OnOff`, the union of `xsd:boolean` with `on`/`off`. No empty member; writes `true`/`false`. */
struct EXYOKIOFFICE_EXPORT OnOffTextTraits
{
    [[nodiscard]] static bool TryParse(std::string_view text, bool& value) noexcept;
    static std::string Format(bool value);
};

/** @brief `ST_TrueFalse` (VML): exactly `t`, `f`, `true`, `false`; writes `true`/`false`. */
struct EXYOKIOFFICE_EXPORT TrueFalseTextTraits
{
    [[nodiscard]] static bool TryParse(std::string_view text, bool& value) noexcept;
    static std::string Format(bool value);
};

/**
 * @brief `ST_TrueFalseBlank` (VML): `t`, `f`, `true`, `false`, and the blank.
 *
 * The blank is a real member of this type - it is what the name says - and parses
 * as false. This is the one boolean family where `attr=""` is a defined value, and
 * `MetadataAttributeInfo::AllowsEmptyValue()` agrees.
 */
struct EXYOKIOFFICE_EXPORT TrueFalseBlankTextTraits
{
    [[nodiscard]] static bool TryParse(std::string_view text, bool& value) noexcept;
    static std::string Format(bool value);
};

struct EXYOKIOFFICE_EXPORT DecimalValueTraits
{
    [[nodiscard]] static bool TryParse(std::string_view text, RealExtended& value) noexcept;
    static std::string Format(RealExtended value);
};

struct EXYOKIOFFICE_EXPORT DateTimeValueTraits
{
    using value_type = std::chrono::system_clock::time_point;

    [[nodiscard]] static bool TryParse(std::string_view text, value_type& value) noexcept;
    static std::string Format(const value_type& value);
};

struct EXYOKIOFFICE_EXPORT Base64BinaryTraits
{
    [[nodiscard]] static bool TryParse(std::string_view text, std::vector<Byte>& value) noexcept;
    static std::string Format(const std::vector<Byte>& value);
};

struct EXYOKIOFFICE_EXPORT HexBinaryTraits
{
    [[nodiscard]] static bool TryParse(std::string_view text, std::vector<Byte>& value) noexcept;
    static std::string Format(const std::vector<Byte>& value);
};

[[nodiscard]] EXYOKIOFFICE_EXPORT bool IsWhitespace(char ch) noexcept;

EXYOKIOFFICE_EXPORT std::vector<std::string_view> SplitWhitespace(std::string_view text);

} // namespace detail

class EXYOKIOFFICE_EXPORT StringValue
{
public:
    enum class Storage
    {
        Undefined,
        View,
        Owned
    };

    StringValue();
    StringValue(const char* value);
    explicit StringValue(std::string value) noexcept;
    explicit StringValue(std::string_view value) noexcept;

    [[nodiscard]] bool IsDefined() const noexcept;
    [[nodiscard]] bool IsView() const noexcept;
    [[nodiscard]] bool IsOwned() const noexcept;

    void Reset() noexcept;

    bool Assign(std::string value) noexcept;
    bool AssignView(std::string_view view) noexcept;
    bool AssignFromString(std::string_view text) noexcept;

    std::string_view View() const noexcept;
    std::string ToString() const;

    friend EXYOKIOFFICE_EXPORT bool operator==(const StringValue& left,
                                               const StringValue& right) noexcept;
    friend EXYOKIOFFICE_EXPORT bool operator!=(const StringValue& left,
                                               const StringValue& right) noexcept;

private:
    Storage m_storage{Storage::Undefined};
    std::string m_owned{};
    std::string_view m_view{};
};

class BooleanValue : public detail::SimpleValue<bool, detail::BooleanTextTraits>
{
public:
    using detail::SimpleValue<bool, detail::BooleanTextTraits>::SimpleValue;
};

class ByteValue : public detail::SimpleValue<UInt8, detail::IntegralValueTraits<UInt8>>
{
public:
    using detail::SimpleValue<UInt8, detail::IntegralValueTraits<UInt8>>::SimpleValue;
};

class SByteValue : public detail::SimpleValue<Int8, detail::IntegralValueTraits<Int8>>
{
public:
    using detail::SimpleValue<Int8, detail::IntegralValueTraits<Int8>>::SimpleValue;
};

class Int16Value : public detail::SimpleValue<Int16, detail::IntegralValueTraits<Int16>>
{
public:
    using detail::SimpleValue<Int16, detail::IntegralValueTraits<Int16>>::SimpleValue;
};

class Int32Value : public detail::SimpleValue<Int32, detail::IntegralValueTraits<Int32>>
{
public:
    using detail::SimpleValue<Int32, detail::IntegralValueTraits<Int32>>::SimpleValue;
};

class Int64Value : public detail::SimpleValue<Int64, detail::IntegralValueTraits<Int64>>
{
public:
    using detail::SimpleValue<Int64, detail::IntegralValueTraits<Int64>>::SimpleValue;
};

class UInt16Value : public detail::SimpleValue<UInt16, detail::IntegralValueTraits<UInt16>>
{
public:
    using detail::SimpleValue<UInt16, detail::IntegralValueTraits<UInt16>>::SimpleValue;
};

class UInt32Value : public detail::SimpleValue<UInt32, detail::IntegralValueTraits<UInt32>>
{
public:
    using detail::SimpleValue<UInt32, detail::IntegralValueTraits<UInt32>>::SimpleValue;
};

class UInt64Value : public detail::SimpleValue<UInt64, detail::IntegralValueTraits<UInt64>>
{
public:
    using detail::SimpleValue<UInt64, detail::IntegralValueTraits<UInt64>>::SimpleValue;
};

class IntegerValue : public detail::SimpleValue<Int64, detail::IntegralValueTraits<Int64>>
{
public:
    using detail::SimpleValue<Int64, detail::IntegralValueTraits<Int64>>::SimpleValue;
};

class DoubleValue : public detail::SimpleValue<Real, detail::FloatingValueTraits<Real>>
{
public:
    using detail::SimpleValue<Real, detail::FloatingValueTraits<Real>>::SimpleValue;
};

class SingleValue : public detail::SimpleValue<Single, detail::FloatingValueTraits<Single>>
{
public:
    using detail::SimpleValue<Single, detail::FloatingValueTraits<Single>>::SimpleValue;
};

class DecimalValue : public detail::SimpleValue<RealExtended, detail::DecimalValueTraits>
{
public:
    using detail::SimpleValue<RealExtended, detail::DecimalValueTraits>::SimpleValue;
};

class DateTimeValue : public detail::SimpleValue<std::chrono::system_clock::time_point, detail::DateTimeValueTraits>
{
public:
    using detail::
        SimpleValue<std::chrono::system_clock::time_point, detail::DateTimeValueTraits>::SimpleValue;
};

class OnOffValue : public detail::SimpleValue<bool, detail::OnOffTextTraits>
{
public:
    using detail::SimpleValue<bool, detail::OnOffTextTraits>::SimpleValue;
};

class TrueFalseValue : public detail::SimpleValue<bool, detail::TrueFalseTextTraits>
{
public:
    using detail::SimpleValue<bool, detail::TrueFalseTextTraits>::SimpleValue;
};

class TrueFalseBlankValue : public detail::SimpleValue<bool, detail::TrueFalseBlankTextTraits>
{
public:
    using detail::SimpleValue<bool, detail::TrueFalseBlankTextTraits>::SimpleValue;
};

class Base64BinaryValue : public detail::SimpleValue<std::vector<Byte>, detail::Base64BinaryTraits>
{
public:
    using detail::SimpleValue<std::vector<Byte>, detail::Base64BinaryTraits>::SimpleValue;
};

class HexBinaryValue : public detail::SimpleValue<std::vector<Byte>, detail::HexBinaryTraits>
{
public:
    using detail::SimpleValue<std::vector<Byte>, detail::HexBinaryTraits>::SimpleValue;
};

template <typename TEnum>
struct OpenXmlEnumTraits
{
    static constexpr bool IsEnabled = std::is_base_of_v<OpenXmlEnum, TEnum>;

    [[nodiscard]] static bool TryParse(std::string_view text, TEnum& value) noexcept
    {
        if constexpr (IsEnabled)
        {
            const auto* meta = TEnum::GetMetaEnum();
            if (!meta)
            {
                return false;
            }

            auto raw = meta->FromString(text);
            TEnum parsed(static_cast<typename TEnum::Value>(raw));
            if (!parsed.IsValid())
            {
                return false;
            }

            value = parsed;
            return true;
        }
        else
        {
            return false;
        }
    }

    static std::string Format(const TEnum& value)
    {
        if constexpr (IsEnabled)
        {
            const auto* meta = TEnum::GetMetaEnum();
            if (!meta)
            {
                return {};
            }

            return std::string(meta->ToString(static_cast<UInt32>(value.GetValue())));
        }
        else
        {
            return {};
        }
    }
};

template <typename TEnum, typename Traits = OpenXmlEnumTraits<TEnum>>
class EnumValue : public detail::SimpleValue<TEnum, Traits>
{
    static_assert(Traits::IsEnabled, "OpenXmlEnumTraits specialization required");

public:
    using detail::SimpleValue<TEnum, Traits>::SimpleValue;
};

template <typename TValue>
class ListValue
{
public:
    using value_type = TValue;

    ListValue() = default;
    explicit ListValue(std::vector<TValue> values)
        : m_items(std::move(values))
    {
        m_defined = !m_items.empty();
    }
    ListValue(std::initializer_list<TValue> values)
        : m_items(values)
    {
        m_defined = !m_items.empty();
    }
    explicit ListValue(std::string_view text)
    {
        AssignFromString(text);
    }

    [[nodiscard]] bool IsDefined() const noexcept
    {
        return m_defined;
    }

    [[nodiscard]] bool Empty() const noexcept
    {
        return m_items.empty();
    }

    ExyokiOffice::Size Size() const noexcept
    {
        return m_items.size();
    }

    std::vector<TValue>& Items() noexcept
    {
        m_defined = !m_items.empty();
        return m_items;
    }

    const std::vector<TValue>& Items() const noexcept
    {
        return m_items;
    }

    void Clear() noexcept
    {
        m_items.clear();
        m_defined = false;
    }

    bool AssignFromString(std::string_view text) noexcept
    {
        Clear();
        auto tokens = detail::SplitWhitespace(text);
        std::vector<TValue> parsed;

        for (auto token : tokens)
        {
            TValue entry;
            if (!entry.AssignFromString(token))
            {
                return false;
            }

            if (!entry.IsDefined())
            {
                return false;
            }

            parsed.push_back(entry);
        }

        m_items = std::move(parsed);
        m_defined = !m_items.empty();
        return true;
    }

    std::string ToString() const
    {
        if (!m_defined)
        {
            return {};
        }

        std::string buffer;
        const char separator = ' ';
        bool first = true;

        for (const auto& item : m_items)
        {
            std::string token = item.ToString();
            if (token.empty())
            {
                return {};
            }

            if (!first)
            {
                buffer.push_back(separator);
            }

            buffer.append(token);
            first = false;
        }

        return buffer;
    }

    typename std::vector<TValue>::iterator begin() noexcept
    {
        return m_items.begin();
    }

    typename std::vector<TValue>::iterator end() noexcept
    {
        return m_items.end();
    }

    typename std::vector<TValue>::const_iterator begin() const noexcept
    {
        return m_items.begin();
    }

    typename std::vector<TValue>::const_iterator end() const noexcept
    {
        return m_items.end();
    }

    friend bool operator==(const ListValue& left, const ListValue& right) noexcept
    {
        return left.m_items == right.m_items;
    }

    friend bool operator!=(const ListValue& left, const ListValue& right) noexcept
    {
        return !(left == right);
    }

private:
    std::vector<TValue> m_items{};
    bool m_defined{false};
};

class EXYOKIOFFICE_EXPORT OpenXmlSimpleValueConvertor
{
public:
    template <typename TValue>
    static TValue FromString(std::string_view text) noexcept
    {
        TValue value;
        TryAssign(text, value);
        return value;
    }

    template <typename TValue>
    [[nodiscard]] static bool FromString(std::string_view text, TValue& value) noexcept
    {
        return TryAssign(text, value);
    }

    template <typename TValue>
    static std::string ToString(const TValue& value)
    {
        if constexpr (detail::has_to_string_method<TValue>::value)
        {
            return value.ToString();
        }
        else
        {
            return {};
        }
    }

    static BooleanValue GetBooleanValueFromString(std::string_view text) noexcept
    {
        return FromString<BooleanValue>(text);
    }

    static ByteValue GetByteValueFromString(std::string_view text) noexcept
    {
        return FromString<ByteValue>(text);
    }

    static SByteValue GetSByteValueFromString(std::string_view text) noexcept
    {
        return FromString<SByteValue>(text);
    }

    static Int16Value GetInt16ValueFromString(std::string_view text) noexcept
    {
        return FromString<Int16Value>(text);
    }

    static Int32Value GetInt32ValueFromString(std::string_view text) noexcept
    {
        return FromString<Int32Value>(text);
    }

    static Int64Value GetInt64ValueFromString(std::string_view text) noexcept
    {
        return FromString<Int64Value>(text);
    }

    static UInt16Value GetUInt16ValueFromString(std::string_view text) noexcept
    {
        return FromString<UInt16Value>(text);
    }

    static UInt32Value GetUInt32ValueFromString(std::string_view text) noexcept
    {
        return FromString<UInt32Value>(text);
    }

    static UInt64Value GetUInt64ValueFromString(std::string_view text) noexcept
    {
        return FromString<UInt64Value>(text);
    }

    static IntegerValue GetIntegerValueFromString(std::string_view text) noexcept
    {
        return FromString<IntegerValue>(text);
    }

    static DoubleValue GetDoubleValueFromString(std::string_view text) noexcept
    {
        return FromString<DoubleValue>(text);
    }

    static SingleValue GetSingleValueFromString(std::string_view text) noexcept
    {
        return FromString<SingleValue>(text);
    }

    static DecimalValue GetDecimalValueFromString(std::string_view text) noexcept
    {
        return FromString<DecimalValue>(text);
    }

    static DateTimeValue GetDateTimeValueFromString(std::string_view text) noexcept
    {
        return FromString<DateTimeValue>(text);
    }

    static OnOffValue GetOnOffValueFromString(std::string_view text) noexcept
    {
        return FromString<OnOffValue>(text);
    }

    static TrueFalseValue GetTrueFalseValueFromString(std::string_view text) noexcept
    {
        return FromString<TrueFalseValue>(text);
    }

    static TrueFalseBlankValue GetTrueFalseBlankValueFromString(std::string_view text) noexcept
    {
        return FromString<TrueFalseBlankValue>(text);
    }

    static Base64BinaryValue GetBase64BinaryValueFromString(std::string_view text) noexcept
    {
        return FromString<Base64BinaryValue>(text);
    }

    static HexBinaryValue GetHexBinaryValueFromString(std::string_view text) noexcept
    {
        return FromString<HexBinaryValue>(text);
    }

    static StringValue GetStringValueFromString(std::string_view text) noexcept
    {
        return FromString<StringValue>(text);
    }

private:
    template <typename TValue>
    static auto TryAssign(std::string_view text, TValue& value) noexcept
        -> decltype(value.AssignFromString(text), bool())
    {
        return value.AssignFromString(text);
    }
};

} // namespace ExyokiOffice
