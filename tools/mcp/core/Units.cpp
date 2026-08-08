// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "Units.hpp"

#include <cmath>
#include <cstdlib>

namespace ExyokiOffice::Mcp
{

/// File-local parsing helpers for the length grammar.
class UnitsParser
{
public:
    /// Screen pixels are converted at the 96 DPI Office assumes for `px`.
    static constexpr Real PixelsPerInch = 96.0;

    static std::string_view Trim(std::string_view text)
    {
        while (!text.empty() && IsSpace(text.front()))
        {
            text.remove_prefix(1);
        }

        while (!text.empty() && IsSpace(text.back()))
        {
            text.remove_suffix(1);
        }

        return text;
    }

    static bool IsSpace(char value) noexcept
    {
        return value == ' ' || value == '\t' || value == '\n' || value == '\r';
    }

    static char ToLower(char value) noexcept
    {
        if (value >= 'A' && value <= 'Z')
        {
            return static_cast<char>(value - 'A' + 'a');
        }

        return value;
    }

    static std::optional<MeasurementUnit> ParseUnit(std::string_view suffix)
    {
        std::string lowered;
        lowered.reserve(suffix.size());
        for (const char character : suffix)
        {
            lowered.push_back(ToLower(character));
        }

        if (lowered == "pt")
        {
            return MeasurementUnit::Point;
        }

        if (lowered == "cm")
        {
            return MeasurementUnit::Centimeter;
        }

        if (lowered == "mm")
        {
            return MeasurementUnit::Millimeter;
        }

        if (lowered == "in")
        {
            return MeasurementUnit::Inch;
        }

        if (lowered == "emu")
        {
            return MeasurementUnit::Emu;
        }

        return std::nullopt;
    }
};

std::optional<MeasuringUnits> ParseLength(const nlohmann::json& value)
{
    if (value.is_number())
    {
        return MeasuringUnits(value.get<Real>(), MeasurementUnit::Point);
    }

    if (value.is_string())
    {
        return ParseLengthText(value.get<std::string>());
    }

    return std::nullopt;
}

std::optional<MeasuringUnits> ParseLengthText(std::string_view text)
{
    const auto trimmed = UnitsParser::Trim(text);
    if (trimmed.empty())
    {
        return std::nullopt;
    }

    const std::string buffer(trimmed);
    char* suffix = nullptr;
    const Real amount = std::strtod(buffer.c_str(), &suffix);
    if (suffix == buffer.c_str())
    {
        return std::nullopt;
    }

    const auto unitText = UnitsParser::Trim(std::string_view(suffix));
    if (unitText.empty())
    {
        return MeasuringUnits(amount, MeasurementUnit::Point);
    }

    // "px" has no MeasurementUnit of its own; it is a device unit converted at
    // the fixed 96 DPI used by every Office renderer.
    if (unitText.size() == 2 && UnitsParser::ToLower(unitText[0]) == 'p' && UnitsParser::ToLower(unitText[1]) == 'x')
    {
        return MeasuringUnits(amount / UnitsParser::PixelsPerInch, MeasurementUnit::Inch);
    }

    const auto unit = UnitsParser::ParseUnit(unitText);
    if (!unit.has_value())
    {
        return std::nullopt;
    }

    return MeasuringUnits(amount, *unit);
}

std::optional<Color> ParseColor(std::string_view text)
{
    const auto trimmed = UnitsParser::Trim(text);
    if (trimmed.empty())
    {
        return std::nullopt;
    }

    std::string value(trimmed);
    if (value.front() != '#')
    {
        value.insert(value.begin(), '#');
    }

    if (value.size() != 7)
    {
        return std::nullopt;
    }

    return Color::FromHexString(value);
}

nlohmann::json LengthToJson(const MeasuringUnits& length)
{
    nlohmann::json result = nlohmann::json::object();
    result["pt"] = ToPointValue(length);
    result["emu"] = ToEmuValue(length);
    return result;
}

Int64 ToEmuValue(const MeasuringUnits& length)
{
    return static_cast<Int64>(std::llround(length.ToEmu().GetValue()));
}

Real ToPointValue(const MeasuringUnits& length)
{
    return length.ToPt().GetValue();
}

} // namespace ExyokiOffice::Mcp
