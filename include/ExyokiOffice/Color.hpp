// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

#include "ExyokiOffice/Export.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace ExyokiOffice
{

/**
 * @brief Predefined color presets for ExyokiOffice::Color.
 */
enum class ColorPreset
{
    Auto,
    Black,
    White,
    Red,
    Green,
    Blue,
    Yellow,
    Cyan,
    Magenta,
    Gray,
    DarkGray,
    LightGray,
    Orange,
    Brown
};

/**
 * @brief Represents a color value used in Open XML documents.
 *
 * The color can be automatic ("auto") or an explicit RGB value.
 * RGB values are stored as 8-bit channels and serialized as hex
 * strings without the leading '#'.
 *
 * Supported constructions:
 *  - Default (auto).
 *  - Predefined preset.
 *  - Normalized RGB (double 0.0 - 1.0).
 *  - Byte RGB (int 0 - 255).
 */
class EXYOKIOFFICE_EXPORT Color
{
public:
    /// Creates an automatic color.
    Color() noexcept;
    /// Creates a color from a predefined preset.
    explicit Color(ColorPreset preset) noexcept;
    /// Creates a color from normalized RGB (0.0 - 1.0).
    Color(Real red, Real green, Real blue) noexcept;
    /// Creates a color from byte RGB (0 - 255).
    Color(int red, int green, int blue) noexcept;

    /**
     * @brief Parses exactly six hexadecimal RGB digits, optionally prefixed by `#`.
     * @return An explicit color, or `std::nullopt` for malformed input and `auto`.
     */
    static std::optional<Color> FromHexString(std::string_view value) noexcept;

    /// Returns true when the color is automatic ("auto").
    [[nodiscard]] bool IsAuto() const noexcept;
    /// Returns the RGB hex string or "auto".
    std::string ToHexString() const;
    /** @return True when both colors have the same automatic state and RGB channels. */
    bool operator==(const Color&) const noexcept = default;

private:
    bool m_isAuto = true;
    UInt8 m_red = 0;
    UInt8 m_green = 0;
    UInt8 m_blue = 0;
};

} // namespace ExyokiOffice
