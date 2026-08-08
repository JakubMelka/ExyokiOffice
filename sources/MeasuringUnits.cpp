// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "ExyokiOffice/MeasuringUnits.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <cmath>
#include <limits>

namespace ExyokiOffice
{

namespace
{
constexpr Real kEmuPerInch = 914400.0;
constexpr Real kPointsPerInch = 72.0;
constexpr Real kTwipsPerPoint = 20.0;
constexpr Real kMillimetersPerInch = 25.4;
constexpr Real kCentimetersPerInch = 2.54;
} // namespace

MeasuringUnits::MeasuringUnits(Real emuValue) noexcept
    : m_value(emuValue), m_unit(MeasurementUnit::Emu)
{
}

MeasuringUnits::MeasuringUnits(Real value, MeasurementUnit unit) noexcept
    : m_value(value), m_unit(unit)
{
}

Real MeasuringUnits::GetValue() const noexcept
{
    return m_value;
}

void MeasuringUnits::SetValue(Real value) noexcept
{
    m_value = value;
}

MeasurementUnit MeasuringUnits::GetUnit() const noexcept
{
    return m_unit;
}

void MeasuringUnits::SetUnit(MeasurementUnit unit) noexcept
{
    m_unit = unit;
}

MeasuringUnits MeasuringUnits::ToMM() const
{
    return ToUnit(MeasurementUnit::Millimeter);
}

MeasuringUnits MeasuringUnits::ToCM() const
{
    return ToUnit(MeasurementUnit::Centimeter);
}

MeasuringUnits MeasuringUnits::ToIN() const
{
    return ToUnit(MeasurementUnit::Inch);
}

MeasuringUnits MeasuringUnits::ToPt() const
{
    return ToUnit(MeasurementUnit::Point);
}

MeasuringUnits MeasuringUnits::ToTw() const
{
    return ToUnit(MeasurementUnit::Twip);
}

MeasuringUnits MeasuringUnits::ToEmu() const
{
    return ToUnit(MeasurementUnit::Emu);
}

MeasuringUnits MeasuringUnits::ToUnit(MeasurementUnit unit) const
{
    const Real emuValue = ToEmuValue(m_value, m_unit);
    const Real converted = FromEmuValue(emuValue, unit);
    return MeasuringUnits(converted, unit);
}

bool MeasuringUnits::operator==(const MeasuringUnits& other) const noexcept
{
    const auto left = ToEmuValue(m_value, m_unit);
    const auto right = ToEmuValue(other.m_value, other.m_unit);
    const auto inRange = [](Real value)
    {
        const auto extended = static_cast<RealExtended>(value);
        return std::isfinite(value) && extended >= static_cast<RealExtended>(std::numeric_limits<Int64>::min()) &&
               extended <= static_cast<RealExtended>(std::numeric_limits<Int64>::max());
    };
    return inRange(left) && inRange(right) && std::llround(left) == std::llround(right);
}

Real MeasuringUnits::ToEmuValue(Real value, MeasurementUnit unit) noexcept
{
    switch (unit)
    {
        case MeasurementUnit::Millimeter:
            return (value / kMillimetersPerInch) * kEmuPerInch;
        case MeasurementUnit::Centimeter:
            return (value / kCentimetersPerInch) * kEmuPerInch;
        case MeasurementUnit::Inch:
            return value * kEmuPerInch;
        case MeasurementUnit::Point:
            return (value / kPointsPerInch) * kEmuPerInch;
        case MeasurementUnit::Twip:
            return (value / (kPointsPerInch * kTwipsPerPoint)) * kEmuPerInch;
        case MeasurementUnit::Emu:
            return value;
    }
    return value;
}

Real MeasuringUnits::FromEmuValue(Real value, MeasurementUnit unit) noexcept
{
    switch (unit)
    {
        case MeasurementUnit::Millimeter:
            return (value / kEmuPerInch) * kMillimetersPerInch;
        case MeasurementUnit::Centimeter:
            return (value / kEmuPerInch) * kCentimetersPerInch;
        case MeasurementUnit::Inch:
            return value / kEmuPerInch;
        case MeasurementUnit::Point:
            return (value / kEmuPerInch) * kPointsPerInch;
        case MeasurementUnit::Twip:
            return (value / kEmuPerInch) * kPointsPerInch * kTwipsPerPoint;
        case MeasurementUnit::Emu:
            return value;
    }
    return value;
}

} // namespace ExyokiOffice
