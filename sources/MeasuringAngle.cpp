// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "ExyokiOffice/MeasuringAngle.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <cmath>
#include <limits>
#include <numbers>

namespace ExyokiOffice
{

MeasuringAngle::MeasuringAngle(Real value, AngleUnit unit) noexcept
    : m_value(value), m_unit(unit)
{
}

Real MeasuringAngle::GetValue() const noexcept
{
    return m_value;
}

void MeasuringAngle::SetValue(Real value) noexcept
{
    m_value = value;
}

AngleUnit MeasuringAngle::GetUnit() const noexcept
{
    return m_unit;
}

void MeasuringAngle::SetUnit(AngleUnit unit) noexcept
{
    m_unit = unit;
}

MeasuringAngle MeasuringAngle::ToDegrees() const noexcept
{
    return ToUnit(AngleUnit::Degree);
}

MeasuringAngle MeasuringAngle::ToRadians() const noexcept
{
    return ToUnit(AngleUnit::Radian);
}

MeasuringAngle MeasuringAngle::ToUnit(AngleUnit unit) const noexcept
{
    const Real degreeValue = ToDegreeValue(m_value, m_unit);
    return MeasuringAngle(FromDegreeValue(degreeValue, unit), unit);
}

bool MeasuringAngle::operator==(const MeasuringAngle& other) const noexcept
{
    constexpr Real drawingMlUnitsPerDegree = 60000.0;
    const Real left = ToDegreeValue(m_value, m_unit) * drawingMlUnitsPerDegree;
    const Real right = ToDegreeValue(other.m_value, other.m_unit) * drawingMlUnitsPerDegree;
    const auto canRound = [](Real value)
    {
        const auto extended = static_cast<RealExtended>(value);
        return std::isfinite(value) && extended >= static_cast<RealExtended>(std::numeric_limits<Int64>::min()) &&
               extended <= static_cast<RealExtended>(std::numeric_limits<Int64>::max());
    };
    return canRound(left) && canRound(right) && std::llround(left) == std::llround(right);
}

Real MeasuringAngle::ToDegreeValue(Real value, AngleUnit unit) noexcept
{
    switch (unit)
    {
        case AngleUnit::Degree:
            return value;
        case AngleUnit::Radian:
            return value * 180.0 / std::numbers::pi;
    }
    return value;
}

Real MeasuringAngle::FromDegreeValue(Real value, AngleUnit unit) noexcept
{
    switch (unit)
    {
        case AngleUnit::Degree:
            return value;
        case AngleUnit::Radian:
            return value * std::numbers::pi / 180.0;
    }
    return value;
}

} // namespace ExyokiOffice
