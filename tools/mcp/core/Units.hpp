// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

#include "ExyokiOffice/Color.hpp"
#include "ExyokiOffice/MeasuringUnits.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <string_view>

namespace ExyokiOffice::Mcp
{

/**
 * @brief Parses a length from a tool argument.
 *
 * A bare number is points, matching how the schemas describe every length. A
 * string carries an explicit unit: `pt`, `cm`, `mm`, `in`, `emu`, or `px`
 * (screen pixels at 96 DPI, the conversion Office itself assumes). The value
 * is returned as a MeasuringUnits so callers keep working in the library's own
 * unit type instead of hand-rolled arithmetic.
 *
 * @return std::nullopt when the value is neither a number nor a parsable
 *         length string.
 */
[[nodiscard]] std::optional<MeasuringUnits> ParseLength(const nlohmann::json& value);

/// Parses a length written as a string, without the numeric shorthand.
[[nodiscard]] std::optional<MeasuringUnits> ParseLengthText(std::string_view text);

/// Parses an `#RRGGBB` color; also accepts the form without the leading `#`.
[[nodiscard]] std::optional<Color> ParseColor(std::string_view text);

/// Renders a length as `{"pt": <points>, "emu": <emu>}`, the shape every tool reports.
[[nodiscard]] nlohmann::json LengthToJson(const MeasuringUnits& length);

/// Rounds a length to whole EMU, the integral unit the drawing layer stores.
[[nodiscard]] Int64 ToEmuValue(const MeasuringUnits& length);

/// Rounds a length to points.
[[nodiscard]] Real ToPointValue(const MeasuringUnits& length);

} // namespace ExyokiOffice::Mcp
