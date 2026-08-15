// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "ExyokiOffice/MeasuringAngle.hpp"

#include <doctest/doctest.h>

#include <numbers>

using namespace ExyokiOffice;

TEST_CASE("MeasuringAngle converts between degrees and radians")
{
    const MeasuringAngle degrees(180.0, AngleUnit::Degree);
    const MeasuringAngle radians(std::numbers::pi, AngleUnit::Radian);

    CHECK(degrees.ToRadians().GetValue() == doctest::Approx(std::numbers::pi));
    CHECK(radians.ToDegrees().GetValue() == doctest::Approx(180.0));
    CHECK(degrees == radians);
}

TEST_CASE("MeasuringAngle setters retain explicit unit semantics")
{
    MeasuringAngle angle;
    angle.SetValue(0.5);
    angle.SetUnit(AngleUnit::Radian);

    CHECK(angle.GetValue() == doctest::Approx(0.5));
    CHECK(angle.GetUnit() == AngleUnit::Radian);
    CHECK(angle.ToDegrees().GetValue() == doctest::Approx(28.6478897565));
}
