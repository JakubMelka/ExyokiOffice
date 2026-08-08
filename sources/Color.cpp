// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "ExyokiOffice/Color.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>

namespace ExyokiOffice
{

namespace
{
UInt8 ClampByte(int value) noexcept
{
    if (value < 0)
    {
        return 0;
    }
    if (value > 255)
    {
        return 255;
    }
    return static_cast<UInt8>(value);
}

UInt8 ClampNormalized(Real value) noexcept
{
    if (value < 0.0)
    {
        value = 0.0;
    }
    if (value > 1.0)
    {
        value = 1.0;
    }
    return static_cast<UInt8>(std::lround(value * 255.0));
}

struct PresetColor
{
    ColorPreset preset;
    UInt8 r;
    UInt8 g;
    UInt8 b;
};

constexpr std::array<PresetColor, 13> kPresetColors = {{{ColorPreset::Black, 0, 0, 0},
                                                        {ColorPreset::White, 255, 255, 255},
                                                        {ColorPreset::Red, 255, 0, 0},
                                                        {ColorPreset::Green, 0, 128, 0},
                                                        {ColorPreset::Blue, 0, 0, 255},
                                                        {ColorPreset::Yellow, 255, 255, 0},
                                                        {ColorPreset::Cyan, 0, 255, 255},
                                                        {ColorPreset::Magenta, 255, 0, 255},
                                                        {ColorPreset::Gray, 128, 128, 128},
                                                        {ColorPreset::DarkGray, 64, 64, 64},
                                                        {ColorPreset::LightGray, 192, 192, 192},
                                                        {ColorPreset::Orange, 255, 165, 0},
                                                        {ColorPreset::Brown, 165, 42, 42}}};
} // namespace

Color::Color() noexcept = default;

Color::Color(ColorPreset preset) noexcept
{
    if (preset == ColorPreset::Auto)
    {
        m_isAuto = true;
        return;
    }

    auto it = std::find_if(kPresetColors.begin(), kPresetColors.end(),
                           [preset](const PresetColor& item)
                           { return item.preset == preset; });
    if (it != kPresetColors.end())
    {
        m_isAuto = false;
        m_red = it->r;
        m_green = it->g;
        m_blue = it->b;
    }
}

Color::Color(Real red, Real green, Real blue) noexcept
    : m_isAuto(false),
      m_red(ClampNormalized(red)),
      m_green(ClampNormalized(green)),
      m_blue(ClampNormalized(blue))
{
}

Color::Color(int red, int green, int blue) noexcept
    : m_isAuto(false),
      m_red(ClampByte(red)),
      m_green(ClampByte(green)),
      m_blue(ClampByte(blue))
{
}

std::optional<Color> Color::FromHexString(std::string_view value) noexcept
{
    if (!value.empty() && value.front() == '#')
    {
        value.remove_prefix(1);
    }
    if (value.size() != 6)
    {
        return std::nullopt;
    }
    const auto nibble = [](char digit) -> std::optional<int>
    {
        if (digit >= '0' && digit <= '9')
        {
            return digit - '0';
        }
        if (digit >= 'a' && digit <= 'f')
        {
            return digit - 'a' + 10;
        }
        if (digit >= 'A' && digit <= 'F')
        {
            return digit - 'A' + 10;
        }
        return std::nullopt;
    };
    int channels[3]{};
    for (Size index = 0; index < 3; ++index)
    {
        const auto high = nibble(value[index * 2]);
        const auto low = nibble(value[index * 2 + 1]);
        if (!high || !low)
        {
            return std::nullopt;
        }
        channels[index] = (*high << 4) | *low;
    }
    return Color(channels[0], channels[1], channels[2]);
}

bool Color::IsAuto() const noexcept
{
    return m_isAuto;
}

std::string Color::ToHexString() const
{
    if (m_isAuto)
    {
        return "auto";
    }

    char buffer[7] = {};
    std::snprintf(buffer, sizeof(buffer), "%02X%02X%02X", m_red, m_green, m_blue);
    return std::string(buffer);
}

} // namespace ExyokiOffice
