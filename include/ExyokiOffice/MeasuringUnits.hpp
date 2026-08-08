// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

#include "ExyokiOffice/Export.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

namespace ExyokiOffice
{

/**
 * @brief Units supported by MeasuringUnits.
 */
enum class MeasurementUnit
{
    /// Millimeters.
    Millimeter,
    /// Centimeters.
    Centimeter,
    /// Inches.
    Inch,
    /// Points (1/72 inch).
    Point,
    /// Twips (1/20 point).
    Twip,
    /// English Metric Units (EMU).
    Emu
};

/**
 * @brief Stores a numeric value together with its measurement unit and provides conversions.
 *
 * The class is tailored for Open XML documents, where lengths are commonly expressed
 * in EMU, points, twips, or physical units (mm/cm/in). Conversions use the following
 * rules:
 *  - 1 inch = 25.4 mm
 *  - 1 inch = 2.54 cm
 *  - 1 inch = 72 points
 *  - 1 point = 20 twips
 *  - 1 inch = 914400 EMU
 *
 * @code
 * using namespace ExyokiOffice;
 *
 * MeasuringUnits margin(12.7, MeasurementUnit::Millimeter);
 * auto in = margin.ToIN();
 * auto emu = margin.ToEmu();
 *
 * Real inches = in.GetValue();
 * Real emuValue = emu.GetValue();
 * @endcode
 */
class EXYOKIOFFICE_EXPORT MeasuringUnits
{
public:
    /// Creates a zero EMU value.
    MeasuringUnits() = default;
    /// Creates a value expressed in native EMU; supports concise exact Open XML values.
    MeasuringUnits(Real emuValue) noexcept;
    /// Creates a value expressed in the specified unit.
    MeasuringUnits(Real value, MeasurementUnit unit) noexcept;

    /// Returns the stored numeric value.
    Real GetValue() const noexcept;
    /// Updates the stored numeric value (unit remains unchanged).
    void SetValue(Real value) noexcept;

    /// Returns the current unit.
    MeasurementUnit GetUnit() const noexcept;
    /// Updates the unit without changing the numeric value.
    void SetUnit(MeasurementUnit unit) noexcept;

    /// Returns a new instance converted to millimeters.
    MeasuringUnits ToMM() const;
    /// Returns a new instance converted to centimeters.
    MeasuringUnits ToCM() const;
    /// Returns a new instance converted to inches.
    MeasuringUnits ToIN() const;
    /// Returns a new instance converted to points.
    MeasuringUnits ToPt() const;
    /// Returns a new instance converted to twips.
    MeasuringUnits ToTw() const;
    /// Returns a new instance converted to EMU.
    MeasuringUnits ToEmu() const;

    /**
     * @brief Converts the stored value into the requested unit.
     * @param unit Target unit.
     * @return New instance expressed in the requested unit.
     */
    MeasuringUnits ToUnit(MeasurementUnit unit) const;

    /** @return True when both values round to the same integral EMU length. */
    bool operator==(const MeasuringUnits& other) const noexcept;

private:
    static Real ToEmuValue(Real value, MeasurementUnit unit) noexcept;
    static Real FromEmuValue(Real value, MeasurementUnit unit) noexcept;

    Real m_value = 0.0;
    MeasurementUnit m_unit = MeasurementUnit::Emu;
};

} // namespace ExyokiOffice
