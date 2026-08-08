// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

#include "ExyokiOffice/Export.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

namespace ExyokiOffice
{

/** @brief Angular units supported by MeasuringAngle. */
enum class AngleUnit
{
    /** Degrees, where one complete revolution is 360 degrees. */
    Degree,
    /** Radians, where one complete revolution is 2 pi radians. */
    Radian
};

/**
 * @brief Stores an angle together with its unit and provides lossless-intent conversions.
 *
 * MeasuringAngle keeps the unit selected by the caller. Conversion to an OOXML
 * representation is performed by the consuming document API. DrawingML, for
 * example, serializes angles in 1/60000 of a degree.
 *
 * @code
 * using namespace ExyokiOffice;
 *
 * MeasuringAngle quarterTurn(90.0, AngleUnit::Degree);
 * auto radians = quarterTurn.ToRadians();
 * @endcode
 */
class EXYOKIOFFICE_EXPORT MeasuringAngle
{
public:
    /** Creates a zero-degree angle. */
    MeasuringAngle() = default;
    /** Creates an angle expressed in the specified unit. */
    MeasuringAngle(Real value, AngleUnit unit) noexcept;

    /** Returns the stored numeric value without converting it. */
    Real GetValue() const noexcept;
    /** Updates the stored numeric value while retaining the current unit. */
    void SetValue(Real value) noexcept;

    /** Returns the unit associated with the stored value. */
    AngleUnit GetUnit() const noexcept;
    /** Changes the unit label without converting the stored numeric value. */
    void SetUnit(AngleUnit unit) noexcept;

    /** Returns a new angle expressed in degrees. */
    MeasuringAngle ToDegrees() const noexcept;
    /** Returns a new angle expressed in radians. */
    MeasuringAngle ToRadians() const noexcept;
    /** Returns a new angle expressed in the requested unit. */
    MeasuringAngle ToUnit(AngleUnit unit) const noexcept;

    /**
     * @return True when both values serialize to the same DrawingML angular unit.
     *
     * DrawingML's resolution is 1/60000 of a degree, so differences below that
     * serialization precision do not make two angles unequal.
     */
    bool operator==(const MeasuringAngle& other) const noexcept;

private:
    static Real ToDegreeValue(Real value, AngleUnit unit) noexcept;
    static Real FromDegreeValue(Real value, AngleUnit unit) noexcept;

    Real m_value = 0.0;
    AngleUnit m_unit = AngleUnit::Degree;
};

} // namespace ExyokiOffice
