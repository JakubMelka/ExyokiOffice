// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "doctest.h"

#include "ExyokiOffice/PowerPoint/PowerPointDocument.hpp"

using ExyokiOffice::MeasurementUnit;
using ExyokiOffice::MeasuringUnits;
using ExyokiOffice::PowerPoint::PowerPointDocumentEditor;
using ExyokiOffice::PowerPoint::PresentationSize;
using ExyokiOffice::PowerPoint::PresentationSlideSize;
namespace Presentation = ExyokiOffice::DocumentFormat::OpenXml::Presentation;

TEST_SUITE("PowerPointSlideSizeTests")
{
    TEST_CASE("a new presentation has no explicit slide size [unit] [powerpoint] [powerpoint-slide-size]")
    {
        auto editor = PowerPointDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);
        CHECK_FALSE(editor->GetSlideSize().has_value());
        CHECK_FALSE(editor->RemoveSlideSize());
    }

    TEST_CASE("slide size survives a round trip [unit] [powerpoint] [powerpoint-slide-size]")
    {
        auto editor = PowerPointDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);
        REQUIRE(editor->SetSlideSize(PresentationSlideSize::Widescreen16x9()));

        const auto bytes = editor->SaveToMemory();
        REQUIRE_FALSE(bytes.empty());
        auto reopened = PowerPointDocumentEditor::Open(bytes);
        REQUIRE(reopened != nullptr);

        const auto size = reopened->GetSlideSize();
        REQUIRE(size.has_value());
        REQUIRE(size->Type.has_value());
        CHECK(*size->Type == Presentation::SlideSizeValues::Screen16x9);
        CHECK(size->Size.Width.ToEmu().GetValue() == doctest::Approx(12192000.0));
        CHECK(size->Size.Height.ToEmu().GetValue() == doctest::Approx(6858000.0));
    }

    TEST_CASE("every named slide size is representable [unit] [powerpoint] [powerpoint-slide-size]")
    {
        auto editor = PowerPointDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);
        for (const auto& size : {PresentationSlideSize::Widescreen16x9(),
                                 PresentationSlideSize::Widescreen16x10(),
                                 PresentationSlideSize::Standard4x3(),
                                 PresentationSlideSize::A4Landscape()})
        {
            REQUIRE(editor->SetSlideSize(size));
            const auto stored = editor->GetSlideSize();
            REQUIRE(stored.has_value());
            CHECK(stored->Type == size.Type);
            CHECK(stored->Size.Width.ToEmu().GetValue() ==
                  doctest::Approx(size.Size.Width.ToEmu().GetValue()).epsilon(0.000001));
            CHECK(stored->Size.Height.ToEmu().GetValue() ==
                  doctest::Approx(size.Size.Height.ToEmu().GetValue()).epsilon(0.000001));
        }
    }

    TEST_CASE("a custom slide size writes no type attribute [unit] [powerpoint] [powerpoint-slide-size]")
    {
        auto editor = PowerPointDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);

        PresentationSlideSize custom;
        custom.Size = PresentationSize(MeasuringUnits(200.0, MeasurementUnit::Millimeter),
                                       MeasuringUnits(150.0, MeasurementUnit::Millimeter));
        REQUIRE(editor->SetSlideSize(custom));

        const auto stored = editor->GetSlideSize();
        REQUIRE(stored.has_value());
        CHECK_FALSE(stored->Type.has_value());
        CHECK(stored->Size.Width.ToUnit(MeasurementUnit::Millimeter).GetValue() == doctest::Approx(200.0));
    }

    TEST_CASE("out-of-range and degenerate sizes are rejected [unit] [powerpoint] [powerpoint-slide-size]")
    {
        auto editor = PowerPointDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);
        REQUIRE(editor->SetSlideSize(PresentationSlideSize::Standard4x3()));

        PresentationSlideSize tooSmall;
        tooSmall.Size = PresentationSize(MeasuringUnits(1.0, MeasurementUnit::Millimeter),
                                         MeasuringUnits(1.0, MeasurementUnit::Millimeter));
        CHECK_FALSE(editor->SetSlideSize(tooSmall));

        PresentationSlideSize tooLarge;
        tooLarge.Size = PresentationSize(MeasuringUnits(100.0, MeasurementUnit::Inch),
                                         MeasuringUnits(10.0, MeasurementUnit::Inch));
        CHECK_FALSE(editor->SetSlideSize(tooLarge));

        CHECK_FALSE(editor->SetSlideSize(PresentationSlideSize{}));

        // The rejected writes left the previously stored size untouched.
        const auto stored = editor->GetSlideSize();
        REQUIRE(stored.has_value());
        CHECK(stored->Size.Width.ToUnit(MeasurementUnit::Inch).GetValue() == doctest::Approx(10.0));
    }

    TEST_CASE("removing the slide size restores the default [unit] [powerpoint] [powerpoint-slide-size]")
    {
        auto editor = PowerPointDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);
        REQUIRE(editor->SetSlideSize(PresentationSlideSize::A4Landscape()));
        REQUIRE(editor->RemoveSlideSize());
        CHECK_FALSE(editor->GetSlideSize().has_value());

        const auto bytes = editor->SaveToMemory();
        REQUIRE_FALSE(bytes.empty());
        auto reopened = PowerPointDocumentEditor::Open(bytes);
        REQUIRE(reopened != nullptr);
        CHECK_FALSE(reopened->GetSlideSize().has_value());
    }
}
