// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

#include <type_traits>

namespace ExyokiOffice
{

template <typename Enum>
class Flags
{
    static_assert(std::is_enum_v<Enum>, "Flags only supports enum types");

public:
    using EnumType = Enum;
    using StorageType = std::underlying_type_t<Enum>;

    constexpr Flags() noexcept = default;
    constexpr Flags(Enum value) noexcept
        : m_value(static_cast<StorageType>(value))
    {
    }
    constexpr explicit Flags(StorageType rawValue) noexcept
        : m_value(rawValue)
    {
    }

    constexpr StorageType Value() const noexcept { return m_value; }
    constexpr bool IsEmpty() const noexcept { return m_value == 0; }
    constexpr explicit operator bool() const noexcept { return m_value != 0; }

    constexpr bool TestFlag(Enum flag) const noexcept
    {
        const auto bits = static_cast<StorageType>(flag);
        return bits != 0 && (m_value & bits) == bits;
    }

    constexpr Flags operator|(Flags other) const noexcept { return Flags(m_value | other.m_value); }
    constexpr Flags operator&(Flags other) const noexcept { return Flags(m_value & other.m_value); }
    constexpr Flags operator^(Flags other) const noexcept { return Flags(m_value ^ other.m_value); }
    constexpr Flags operator~() const noexcept { return Flags(static_cast<StorageType>(~m_value)); }

    Flags& operator|=(Flags other) noexcept
    {
        m_value |= other.m_value;
        return *this;
    }

    Flags& operator&=(Flags other) noexcept
    {
        m_value &= other.m_value;
        return *this;
    }

    Flags& operator^=(Flags other) noexcept
    {
        m_value ^= other.m_value;
        return *this;
    }

    Flags& operator|=(Enum flag) noexcept
    {
        m_value |= static_cast<StorageType>(flag);
        return *this;
    }

    Flags& operator&=(Enum flag) noexcept
    {
        m_value &= static_cast<StorageType>(flag);
        return *this;
    }

    Flags& operator^=(Enum flag) noexcept
    {
        m_value ^= static_cast<StorageType>(flag);
        return *this;
    }

private:
    StorageType m_value = 0;
};

template <typename Enum>
constexpr Flags<Enum> operator|(Enum lhs, Enum rhs) noexcept
{
    return Flags<Enum>(lhs) | Flags<Enum>(rhs);
}

template <typename Enum>
constexpr Flags<Enum> operator&(Enum lhs, Enum rhs) noexcept
{
    return Flags<Enum>(lhs) & Flags<Enum>(rhs);
}

template <typename Enum>
constexpr Flags<Enum> operator^(Enum lhs, Enum rhs) noexcept
{
    return Flags<Enum>(lhs) ^ Flags<Enum>(rhs);
}

template <typename Enum>
constexpr Flags<Enum> operator|(Flags<Enum> lhs, Enum rhs) noexcept
{
    lhs |= rhs;
    return lhs;
}

template <typename Enum>
constexpr Flags<Enum> operator&(Flags<Enum> lhs, Enum rhs) noexcept
{
    lhs &= rhs;
    return lhs;
}

template <typename Enum>
constexpr Flags<Enum> operator^(Flags<Enum> lhs, Enum rhs) noexcept
{
    lhs ^= rhs;
    return lhs;
}

template <typename Enum>
constexpr Flags<Enum> operator|(Enum lhs, Flags<Enum> rhs) noexcept
{
    rhs |= lhs;
    return rhs;
}

template <typename Enum>
constexpr Flags<Enum> operator&(Enum lhs, Flags<Enum> rhs) noexcept
{
    rhs &= lhs;
    return rhs;
}

template <typename Enum>
constexpr Flags<Enum> operator^(Enum lhs, Flags<Enum> rhs) noexcept
{
    rhs ^= lhs;
    return rhs;
}

} // namespace ExyokiOffice
