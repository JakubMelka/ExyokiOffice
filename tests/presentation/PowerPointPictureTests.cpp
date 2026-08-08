// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "doctest.h"

#include "ExyokiOffice/OpenXmlPackageValidator.hpp"
#include "ExyokiOffice/PowerPoint/PowerPointDocument.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

using namespace ExyokiOffice::PowerPoint;

namespace
{
PresentationPictureData EmbeddedPicture()
{
    PresentationPictureData picture;
    picture.Embedded = PresentationEmbeddedPicture{{0x89, 'P', 'N', 'G', 1, 2, 3}, "image/png"};
    picture.Crop = {1000, 2000, 3000, 4000};
    picture.Name = "Quarterly chart";
    picture.AltText = "Revenue increased in every quarter";
    picture.Title = "Revenue chart";
    picture.Hyperlink = "https://example.test/report";
    picture.HyperlinkTooltip = "Open the report";
    picture.Transform.Position = {914400, 1828800};
    picture.Transform.Size = {2743200, 1828800};
    picture.Transform.Rotation = 900000;
    picture.Transform.FlipHorizontal = true;
    return picture;
}

std::vector<ExyokiOffice::Byte> PngHeader(ExyokiOffice::UInt32 width, ExyokiOffice::UInt32 height)
{
    return {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n',
            0, 0, 0, 13, 'I', 'H', 'D', 'R',
            static_cast<ExyokiOffice::UInt8>(width >> 24),
            static_cast<ExyokiOffice::UInt8>(width >> 16),
            static_cast<ExyokiOffice::UInt8>(width >> 8),
            static_cast<ExyokiOffice::UInt8>(width),
            static_cast<ExyokiOffice::UInt8>(height >> 24),
            static_cast<ExyokiOffice::UInt8>(height >> 16),
            static_cast<ExyokiOffice::UInt8>(height >> 8),
            static_cast<ExyokiOffice::UInt8>(height)};
}
} // namespace

TEST_SUITE("PowerPointPictureTests")
{
    TEST_CASE("embedded picture payload and all metadata round trip [unit] [powerpoint] [picture]")
    {
        auto editor = PowerPointDocumentEditor::CreateNew();
        auto picture = editor->AddSlide()->ShapeTree()->AddPicture(EmbeddedPicture());
        REQUIRE(picture);
        CHECK(picture->GetPicture() == EmbeddedPicture());

        auto reopened = PowerPointDocumentEditor::Open(editor->SaveToMemory());
        REQUIRE(reopened);
        REQUIRE(reopened->GetSlide(0));
        CHECK(reopened->GetSlide(0)->ShapeTree()->Get(0)->GetPicture() == EmbeddedPicture());
    }

    TEST_CASE("linked pictures retain URI without resolving any resource [unit] [powerpoint] [picture]")
    {
        auto editor = PowerPointDocumentEditor::CreateNew();
        auto slide = editor->AddSlide();
        PresentationPictureData linked;
        linked.LinkedUri = "https://unreachable.invalid/image.png";
        linked.Name = "Remote image";
        linked.Transform.Size = {100, 200};
        auto picture = slide->ShapeTree()->AddPicture(linked);
        REQUIRE(picture);
        REQUIRE(picture->GetPicture());
        CHECK(picture->GetPicture()->LinkedUri == linked.LinkedUri);
        CHECK_FALSE(picture->GetPicture()->Embedded.has_value());
        CHECK(slide->GetPart()->GetImageParts().empty());

        auto reopened = PowerPointDocumentEditor::Open(editor->SaveToMemory());
        REQUIRE(reopened);
        CHECK(reopened->GetSlide(0)->ShapeTree()->Get(0)->GetPicture() == linked);
    }

    TEST_CASE("replacing picture source removes obsolete relationships [unit] [powerpoint] [picture]")
    {
        auto editor = PowerPointDocumentEditor::CreateNew();
        auto slide = editor->AddSlide();
        auto tree = slide->ShapeTree();
        auto picture = tree->AddPicture(EmbeddedPicture());
        REQUIRE(picture);
        REQUIRE(slide->GetPart()->GetImageParts().size() == 1);

        PresentationPictureData linked;
        linked.LinkedUri = "file:///images/current.png";
        linked.Transform.Size = {10, 10};
        REQUIRE(picture->SetPicture(linked));
        CHECK(slide->GetPart()->GetImageParts().empty());
        REQUIRE(picture->GetPicture());
        CHECK(picture->GetPicture()->LinkedUri == linked.LinkedUri);
        CHECK_FALSE(picture->GetPicture()->Hyperlink.has_value());

        REQUIRE(tree->Remove(0));
        CHECK(slide->GetPart()->Relationships().empty());
    }

    TEST_CASE("invalid picture inputs are rejected without changing existing content [unit] [powerpoint] [picture]")
    {
        auto editor = PowerPointDocumentEditor::CreateNew();
        auto tree = editor->AddSlide()->ShapeTree();
        auto picture = tree->AddPicture(EmbeddedPicture());
        REQUIRE(picture);
        const auto original = picture->GetPicture();

        auto invalid = EmbeddedPicture();
        invalid.LinkedUri = "https://example.test/image.png";
        CHECK_FALSE(picture->SetPicture(invalid));
        invalid = EmbeddedPicture();
        invalid.Crop.Left = 100001;
        CHECK_FALSE(picture->SetPicture(invalid));
        invalid = EmbeddedPicture();
        invalid.Transform.Size.Width = -1;
        CHECK_FALSE(picture->SetPicture(invalid));
        CHECK(picture->GetPicture() == original);
        CHECK_FALSE(tree->AddShape()->SetPicture(EmbeddedPicture()));
    }

    TEST_CASE("authored embedded and linked pictures pass package validation [unit] [powerpoint] [picture]")
    {
        auto editor = PowerPointDocumentEditor::CreateNew();
        auto tree = editor->AddSlide()->ShapeTree();
        REQUIRE(tree->AddPicture(EmbeddedPicture()));
        PresentationPictureData linked;
        linked.LinkedUri = "https://example.test/image.jpg";
        linked.Transform.Position = {100, 200};
        linked.Transform.Size = {300, 400};
        REQUIRE(tree->AddPicture(linked));
        CHECK(ExyokiOffice::OpenXmlPackageValidator().Validate(*editor->GetDocument()).IsValid());
    }

    TEST_CASE("detected picture data receives intrinsic size and supports payload-only replacement [unit] [powerpoint] [picture]")
    {
        auto editor = PowerPointDocumentEditor::CreateNew();
        auto slide = editor->AddSlide();
        PresentationShapeTransform transform;
        transform.Position = {1234, 5678};
        auto picture = slide->ShapeTree()->AddPictureFromData(PngHeader(2, 1), transform, "Detected PNG");
        REQUIRE(picture != nullptr);

        auto original = picture->GetPicture();
        REQUIRE(original.has_value());
        REQUIRE(original->Embedded.has_value());
        CHECK(original->Embedded->ContentType == "image/png");
        CHECK(original->Name == "Detected PNG");
        CHECK(original->Transform.Position == transform.Position);
        CHECK(original->Transform.Size.Width.ToEmu().GetValue() == doctest::Approx(19050.0));
        CHECK(original->Transform.Size.Height.ToEmu().GetValue() == doctest::Approx(9525.0));

        original->Crop = {1000, 2000, 3000, 4000};
        original->AltText = "Preserved description";
        REQUIRE(picture->SetPicture(*original));
        PresentationShapeOutline outline;
        outline.Fill = PresentationFillKind::Solid;
        outline.ColorValue = ExyokiOffice::Color(10, 20, 30);
        outline.Width = ExyokiOffice::MeasuringUnits(2.0, ExyokiOffice::MeasurementUnit::Point);
        REQUIRE(picture->SetOutline(outline));
        PresentationShapeEffects effects;
        effects.Glow = PresentationTextGlow{
            ExyokiOffice::MeasuringUnits(3.0, ExyokiOffice::MeasurementUnit::Point),
            ExyokiOffice::Color(40, 50, 60)};
        REQUIRE(picture->SetEffects(effects));

        REQUIRE(picture->ReplacePictureFromData(PngHeader(4, 3)));
        auto replaced = picture->GetPicture();
        REQUIRE(replaced.has_value());
        CHECK(replaced->Crop == original->Crop);
        CHECK(replaced->AltText == original->AltText);
        CHECK(replaced->Transform == original->Transform);
        CHECK(picture->GetOutline() == outline);
        CHECK(picture->GetEffects() == effects);
        CHECK_FALSE(picture->ReplacePictureFromData({1, 2, 3}));

        auto reopened = PowerPointDocumentEditor::Open(editor->SaveToMemory());
        REQUIRE(reopened != nullptr);
        auto persisted = reopened->GetSlide(0)->ShapeTree()->Get(0);
        REQUIRE(persisted != nullptr);
        CHECK(persisted->GetPicture()->Crop == original->Crop);
        CHECK(persisted->GetOutline() == outline);
        CHECK(persisted->GetEffects() == effects);
    }
} // TEST_SUITE("PowerPointPictureTests")
