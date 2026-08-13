// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "doctest.h"

#include "ImageFixtures.hpp"
#include "TestSupport.hpp"

#include "ExyokiOffice/Packaging/GeneratedParts.hpp"
#include "ExyokiOffice/Word/WordDocument.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace
{
namespace Wp = ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing;

using ExyokiOffice::MeasurementUnit;
using ExyokiOffice::MeasuringUnits;
using ExyokiOffice::Word::Image;
using ExyokiOffice::Word::ImageCrop;
using ExyokiOffice::Word::ImageDistanceFromText;
using ExyokiOffice::Word::ImageFormatInfo;
using ExyokiOffice::Word::ImageHyperlink;
using ExyokiOffice::Word::ImageLayout;
using ExyokiOffice::Word::ImageWrap;
using ExyokiOffice::Word::WordDocumentEditor;

// The payload builders live in tests/support/ImageFixtures.hpp, shared with
// the unit layer's DetectImageFormat tests.
using ExyokiOfficeTests::BuildBmp;
using ExyokiOfficeTests::BuildGif;
using ExyokiOfficeTests::BuildJpeg;
using ExyokiOfficeTests::BuildPng;

bool ApproximatelyEqual(ExyokiOffice::Real lhs, ExyokiOffice::Real rhs, ExyokiOffice::Real tolerance)
{
    return std::fabs(lhs - rhs) <= tolerance;
}

} // namespace

TEST_SUITE("WordImageTests")
{

    TEST_CASE("DetectImageFormat recognizes PNG, JPEG, GIF, and BMP signatures [unit] [word] [word-image]")
    {
        SUBCASE("PNG without resolution metadata defaults to 96 DPI")
        {
            const auto data = BuildPng(64, 32);
            const auto format = ExyokiOffice::Word::DetectImageFormat(data);
            REQUIRE(format.has_value());
            CHECK(format->ContentType == "image/png");
            CHECK(format->PixelWidth == 64);
            CHECK(format->PixelHeight == 32);
            CHECK(ApproximatelyEqual(format->HorizontalDpi, 96.0, 0.01));
            CHECK(ApproximatelyEqual(format->VerticalDpi, 96.0, 0.01));
        }

        SUBCASE("PNG with pHYs chunk reports physical resolution")
        {
            // 11811 pixels per meter is approximately 300 DPI (11811 * 0.0254 = 300.0).
            const auto data = BuildPng(100, 50, std::make_pair<ExyokiOffice::UInt32, ExyokiOffice::UInt32>(11811, 11811));
            const auto format = ExyokiOffice::Word::DetectImageFormat(data);
            REQUIRE(format.has_value());
            CHECK(format->PixelWidth == 100);
            CHECK(format->PixelHeight == 50);
            CHECK(ApproximatelyEqual(format->HorizontalDpi, 300.0, 0.5));
            CHECK(ApproximatelyEqual(format->VerticalDpi, 300.0, 0.5));
        }

        SUBCASE("JPEG reports SOF0 dimensions and JFIF resolution")
        {
            const auto data = BuildJpeg(200, 100, 150);
            const auto format = ExyokiOffice::Word::DetectImageFormat(data);
            REQUIRE(format.has_value());
            CHECK(format->ContentType == "image/jpeg");
            CHECK(format->PixelWidth == 200);
            CHECK(format->PixelHeight == 100);
            CHECK(ApproximatelyEqual(format->HorizontalDpi, 150.0, 0.01));
            CHECK(ApproximatelyEqual(format->VerticalDpi, 150.0, 0.01));
        }

        SUBCASE("GIF reports logical screen dimensions and defaults to 96 DPI")
        {
            const auto data = BuildGif(320, 240);
            const auto format = ExyokiOffice::Word::DetectImageFormat(data);
            REQUIRE(format.has_value());
            CHECK(format->ContentType == "image/gif");
            CHECK(format->PixelWidth == 320);
            CHECK(format->PixelHeight == 240);
            CHECK(ApproximatelyEqual(format->HorizontalDpi, 96.0, 0.01));
        }

        SUBCASE("BMP reports DIB header dimensions and pixels-per-meter resolution")
        {
            // 3780 pixels per meter is approximately 96 DPI (3780 * 0.0254 = 96.012).
            const auto data = BuildBmp(48, 24, 3780, 3780);
            const auto format = ExyokiOffice::Word::DetectImageFormat(data);
            REQUIRE(format.has_value());
            CHECK(format->ContentType == "image/bmp");
            CHECK(format->PixelWidth == 48);
            CHECK(format->PixelHeight == 24);
            CHECK(ApproximatelyEqual(format->HorizontalDpi, 96.0, 0.1));
        }

        SUBCASE("BMP with a negative (top-down) height reports a positive pixel height")
        {
            const auto data = BuildBmp(10, -20);
            const auto format = ExyokiOffice::Word::DetectImageFormat(data);
            REQUIRE(format.has_value());
            CHECK(format->PixelHeight == 20);
        }

        SUBCASE("Unrecognized or too-short payloads return nullopt")
        {
            CHECK_FALSE(ExyokiOffice::Word::DetectImageFormat({}).has_value());
            const std::vector<ExyokiOffice::Byte> garbage = {0x01, 0x02, 0x03, 0x04};
            CHECK_FALSE(ExyokiOffice::Word::DetectImageFormat(garbage).has_value());
        }
    }

    TEST_CASE("WordDocumentEditor::AddImageFromData auto-detects content type and natural size [unit] [word] [word-image]")
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);

        const auto png = BuildPng(96, 48);
        auto image = editor->AddImageFromData(png, ImageLayout::Inline, ImageWrap::Square);
        REQUIRE(image != nullptr);

        ExyokiOffice::Word::ImageSize size;
        REQUIRE(image->TryGetSize(size));
        // 96 pixels at the default 96 DPI is exactly one inch: 914400 EMU.
        CHECK(ApproximatelyEqual(size.Width.ToEmu().GetValue(), 914400.0, 1.0));
        CHECK(ApproximatelyEqual(size.Height.ToEmu().GetValue(), 457200.0, 1.0));

        auto mainPart = editor->GetDocument()->GetMainDocumentPart();
        REQUIRE(mainPart != nullptr);
        const auto imageParts = mainPart->GetImageParts();
        REQUIRE(imageParts.size() == 1);
        CHECK(imageParts.front()->ContentType() == "image/png");
    }

    TEST_CASE("WordDocumentEditor::AddImageFromFile auto-detects content type and natural size [unit] [word] [word-image]")
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);

        const auto jpeg = BuildJpeg(150, 75, 96);
        const auto tempPath = ExyokiOfficeTests::MakeTemporaryPath("exyokioffice_word_image_test", ".jpg");
        {
            std::ofstream file(tempPath, std::ios::binary);
            REQUIRE(file.is_open());
            file.write(reinterpret_cast<const char*>(jpeg.data()), static_cast<std::streamsize>(jpeg.size()));
        }

        auto image = editor->AddImageFromFile(tempPath);
        std::filesystem::remove(tempPath);
        REQUIRE(image != nullptr);

        ExyokiOffice::Word::ImageSize size;
        REQUIRE(image->TryGetSize(size));
        CHECK(ApproximatelyEqual(size.Width.ToEmu().GetValue(), 150.0 / 96.0 * 914400.0, 1.0));
        CHECK(ApproximatelyEqual(size.Height.ToEmu().GetValue(), 75.0 / 96.0 * 914400.0, 1.0));

        auto mainPart = editor->GetDocument()->GetMainDocumentPart();
        REQUIRE(mainPart != nullptr);
        const auto imageParts = mainPart->GetImageParts();
        REQUIRE(imageParts.size() == 1);
        CHECK(imageParts.front()->ContentType() == "image/jpeg");
    }

    TEST_CASE("Image crop metadata round-trips through save and open [unit] [word] [word-image]")
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);

        auto image = editor->AddImageFromData(BuildPng(64, 64),
                                              "image/png",
                                              MeasuringUnits(2.0, MeasurementUnit::Centimeter),
                                              MeasuringUnits(2.0, MeasurementUnit::Centimeter));
        REQUIRE(image != nullptr);

        image->SetCrop(0.1, 0.2, 0.3, 0.4);

        ImageCrop crop;
        REQUIRE(image->TryGetCrop(crop));
        CHECK(ApproximatelyEqual(crop.Left, 0.1, 0.0001));
        CHECK(ApproximatelyEqual(crop.Top, 0.2, 0.0001));
        CHECK(ApproximatelyEqual(crop.Right, 0.3, 0.0001));
        CHECK(ApproximatelyEqual(crop.Bottom, 0.4, 0.0001));

        auto reopened = WordDocumentEditor::Open(editor->SaveToMemory());
        REQUIRE(reopened != nullptr);
        auto paragraphs = reopened->Paragraphs();
        REQUIRE(paragraphs.size() == 1);
        auto images = paragraphs.front()->Images();
        REQUIRE(images.size() == 1);

        ImageCrop reopenedCrop;
        REQUIRE(images.front()->TryGetCrop(reopenedCrop));
        CHECK(ApproximatelyEqual(reopenedCrop.Left, 0.1, 0.0001));
        CHECK(ApproximatelyEqual(reopenedCrop.Top, 0.2, 0.0001));
        CHECK(ApproximatelyEqual(reopenedCrop.Right, 0.3, 0.0001));
        CHECK(ApproximatelyEqual(reopenedCrop.Bottom, 0.4, 0.0001));

        image->ClearCrop();
        CHECK_FALSE(image->TryGetCrop(crop));
    }

    TEST_CASE("Image alt text round-trips through save and open [unit] [word] [word-image]")
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);

        auto image = editor->AddImageFromData(BuildPng(32, 32),
                                              "image/png",
                                              MeasuringUnits(1.0, MeasurementUnit::Centimeter),
                                              MeasuringUnits(1.0, MeasurementUnit::Centimeter));
        REQUIRE(image != nullptr);

        image->SetAltText("Company logo", "A stylized letter E used as the company logo");
        CHECK(image->GetTitle() == "Company logo");
        CHECK(image->GetDescription() == "A stylized letter E used as the company logo");

        auto reopened = WordDocumentEditor::Open(editor->SaveToMemory());
        REQUIRE(reopened != nullptr);
        auto images = reopened->Paragraphs().front()->Images();
        REQUIRE(images.size() == 1);
        CHECK(images.front()->GetTitle() == "Company logo");
        CHECK(images.front()->GetDescription() == "A stylized letter E used as the company logo");
    }

    TEST_CASE("Image rotation and flip transform metadata round-trips through save and open [unit] [word] [word-image]")
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);

        auto image = editor->AddImageFromData(BuildPng(32, 32),
                                              "image/png",
                                              MeasuringUnits(1.0, MeasurementUnit::Centimeter),
                                              MeasuringUnits(1.0, MeasurementUnit::Centimeter));
        REQUIRE(image != nullptr);

        image->SetRotation(90.0);
        image->SetFlip(true, false);

        CHECK(ApproximatelyEqual(image->GetRotation(), 90.0, 0.001));
        bool horizontal = false;
        bool vertical = false;
        REQUIRE(image->TryGetFlip(horizontal, vertical));
        CHECK(horizontal);
        CHECK_FALSE(vertical);

        auto reopened = WordDocumentEditor::Open(editor->SaveToMemory());
        REQUIRE(reopened != nullptr);
        auto images = reopened->Paragraphs().front()->Images();
        REQUIRE(images.size() == 1);
        CHECK(ApproximatelyEqual(images.front()->GetRotation(), 90.0, 0.001));
        REQUIRE(images.front()->TryGetFlip(horizontal, vertical));
        CHECK(horizontal);
        CHECK_FALSE(vertical);
    }

    TEST_CASE("Image negative rotation normalizes into the 0-360 degree range [unit] [word] [word-image]")
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);

        auto image = editor->AddImageFromData(BuildPng(16, 16),
                                              "image/png",
                                              MeasuringUnits(1.0, MeasurementUnit::Centimeter),
                                              MeasuringUnits(1.0, MeasurementUnit::Centimeter));
        REQUIRE(image != nullptr);

        image->SetRotation(-90.0);
        CHECK(ApproximatelyEqual(image->GetRotation(), 270.0, 0.001));
    }

    TEST_CASE("Image hyperlink creates a relationship and round-trips through save and open [unit] [word] [word-image]")
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);

        auto image = editor->AddImageFromData(BuildPng(32, 32),
                                              "image/png",
                                              MeasuringUnits(1.0, MeasurementUnit::Centimeter),
                                              MeasuringUnits(1.0, MeasurementUnit::Centimeter));
        REQUIRE(image != nullptr);

        image->SetHyperlink("https://example.com/product", true, "Visit our website");

        ImageHyperlink hyperlink;
        REQUIRE(image->TryGetHyperlink(hyperlink));
        CHECK(hyperlink.Url == "https://example.com/product");
        CHECK(hyperlink.Tooltip == "Visit our website");
        CHECK(hyperlink.NewWindow);

        auto reopened = WordDocumentEditor::Open(editor->SaveToMemory());
        REQUIRE(reopened != nullptr);
        auto images = reopened->Paragraphs().front()->Images();
        REQUIRE(images.size() == 1);

        auto reopenedMainPart = reopened->GetDocument()->GetMainDocumentPart();
        REQUIRE(reopenedMainPart != nullptr);
        images.front()->AttachMainDocumentPart(reopenedMainPart);

        ImageHyperlink reopenedHyperlink;
        REQUIRE(images.front()->TryGetHyperlink(reopenedHyperlink));
        CHECK(reopenedHyperlink.Url == "https://example.com/product");
        CHECK(reopenedHyperlink.Tooltip == "Visit our website");
        CHECK(reopenedHyperlink.NewWindow);

        images.front()->RemoveHyperlink();
        CHECK_FALSE(images.front()->TryGetHyperlink(reopenedHyperlink));
    }

    TEST_CASE("Image obtained via Paragraph::Images does not resolve a hyperlink URL without an attached part [unit] [word] [word-image]")
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);

        auto image = editor->AddImageFromData(BuildPng(16, 16),
                                              "image/png",
                                              MeasuringUnits(1.0, MeasurementUnit::Centimeter),
                                              MeasuringUnits(1.0, MeasurementUnit::Centimeter));
        REQUIRE(image != nullptr);
        image->SetHyperlink("https://example.com", false, "Example");

        auto detached = editor->Paragraphs().front()->Images();
        REQUIRE(detached.size() == 1);

        ImageHyperlink hyperlink;
        REQUIRE(detached.front()->TryGetHyperlink(hyperlink));
        CHECK(hyperlink.Tooltip == "Example");
        CHECK(hyperlink.Url.empty());
    }

    TEST_CASE("Setting a hyperlink without an attached main document part is a no-op [unit] [word] [word-image]")
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);

        auto image = editor->AddImageFromData(BuildPng(16, 16),
                                              "image/png",
                                              MeasuringUnits(1.0, MeasurementUnit::Centimeter),
                                              MeasuringUnits(1.0, MeasurementUnit::Centimeter));
        REQUIRE(image != nullptr);

        auto detached = editor->Paragraphs().front()->Images();
        REQUIRE(detached.size() == 1);
        detached.front()->SetHyperlink("https://example.com");

        ImageHyperlink hyperlink;
        CHECK_FALSE(detached.front()->TryGetHyperlink(hyperlink));
    }

    // wp:inline and wp:anchor both carry distT/distB/distL/distR - the gap Word
    // keeps between the image edge and the surrounding text. Losing them on a
    // round trip or a layout switch reflows the document the next time Word
    // opens it, which is exactly the kind of silent damage a library must not do.
    TEST_CASE("Image distance from text survives round trip and layout conversion [unit] [word] [word-image]")
    {
        constexpr ExyokiOffice::Real kEmuPerMillimeter = 36000.0;
        const auto emu = [](const MeasuringUnits& value)
        { return value.ToEmu().GetValue(); };

        SUBCASE("distances set on an inline image round-trip through the package")
        {
            auto editor = WordDocumentEditor::CreateNew();
            REQUIRE(editor != nullptr);
            auto image = editor->AddImageFromData(BuildPng(32, 32), ImageLayout::Inline,
                                                  ImageWrap::Square);
            REQUIRE(image != nullptr);

            image->SetDistanceFromText(MeasuringUnits(2.0, MeasurementUnit::Millimeter),
                                       MeasuringUnits(1.0, MeasurementUnit::Millimeter),
                                       MeasuringUnits(3.0, MeasurementUnit::Millimeter),
                                       MeasuringUnits(4.0, MeasurementUnit::Millimeter));

            auto reopened = WordDocumentEditor::Open(editor->SaveToMemory());
            REQUIRE(reopened != nullptr);
            const auto paragraphs = reopened->Paragraphs();
            REQUIRE_FALSE(paragraphs.empty());
            auto images = paragraphs.front()->Images();
            REQUIRE(images.size() == 1);

            ImageDistanceFromText distances;
            REQUIRE(images.front()->TryGetDistanceFromText(distances));
            CHECK(emu(distances.Left) == doctest::Approx(2.0 * kEmuPerMillimeter));
            CHECK(emu(distances.Top) == doctest::Approx(1.0 * kEmuPerMillimeter));
            CHECK(emu(distances.Right) == doctest::Approx(3.0 * kEmuPerMillimeter));
            CHECK(emu(distances.Bottom) == doctest::Approx(4.0 * kEmuPerMillimeter));
        }

        SUBCASE("switching to floating layout carries the distances onto the anchor")
        {
            auto editor = WordDocumentEditor::CreateNew();
            REQUIRE(editor != nullptr);
            auto image = editor->AddImageFromData(BuildPng(32, 32), ImageLayout::Inline,
                                                  ImageWrap::Square);
            REQUIRE(image != nullptr);
            image->SetDistanceFromText(MeasuringUnits(2.0, MeasurementUnit::Millimeter),
                                       MeasuringUnits(0.0, MeasurementUnit::Millimeter),
                                       MeasuringUnits(0.0, MeasurementUnit::Millimeter),
                                       MeasuringUnits(0.0, MeasurementUnit::Millimeter));

            image->SetLayout(ImageLayout::Floating);
            CHECK(image->GetLayout() == ImageLayout::Floating);

            ImageDistanceFromText distances;
            REQUIRE(image->TryGetDistanceFromText(distances));
            CHECK(emu(distances.Left) == doctest::Approx(2.0 * kEmuPerMillimeter));
            CHECK(emu(distances.Top) == doctest::Approx(0.0));

            // And back: the anchor's distances return to the inline element.
            image->SetLayout(ImageLayout::Inline);
            CHECK(image->GetLayout() == ImageLayout::Inline);
            REQUIRE(image->TryGetDistanceFromText(distances));
            CHECK(emu(distances.Left) == doctest::Approx(2.0 * kEmuPerMillimeter));
            CHECK(emu(distances.Right) == doctest::Approx(0.0));
        }

        SUBCASE("a floating image without distances converts without inventing them")
        {
            auto editor = WordDocumentEditor::CreateNew();
            REQUIRE(editor != nullptr);
            auto image = editor->AddImageFromData(BuildPng(16, 16), ImageLayout::Floating,
                                                  ImageWrap::Square);
            REQUIRE(image != nullptr);

            image->SetLayout(ImageLayout::Inline);

            ImageDistanceFromText distances;
            REQUIRE(image->TryGetDistanceFromText(distances));
            CHECK(emu(distances.Left) == doctest::Approx(0.0));
            CHECK(emu(distances.Top) == doctest::Approx(0.0));
            CHECK(emu(distances.Right) == doctest::Approx(0.0));
            CHECK(emu(distances.Bottom) == doctest::Approx(0.0));
        }
    }

    TEST_CASE("A fully configured floating anchor round-trips and stays schema-valid [unit] [word] [word-image]")
    {
        // The anchoring API writes the whole `<wp:anchor>` shape - wrap element,
        // positionH/positionV, and the anchor attributes. What makes the document
        // usable is not only that the values read back: `wp:CT_Anchor` fixes the
        // order of those children, so an element appended in the wrong place
        // produces a file Word refuses to open while every getter here still
        // answers correctly. The package is therefore validated as well.
        constexpr ExyokiOffice::Real kEmuPerCentimeter = 360000.0;
        const auto emu = [](const MeasuringUnits& value)
        { return value.ToEmu().GetValue(); };

        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);
        auto image = editor->AddImageFromData(BuildPng(64, 64), ImageLayout::Inline, ImageWrap::Square);
        REQUIRE(image != nullptr);

        // Every one of these switches an inline image to floating on its own; the
        // first call has to do the conversion and the rest must not undo it.
        image->SetWrap(ImageWrap::Tight, Wp::WrapTextValues::Left)
            .SetPosition(Wp::HorizontalRelativePositionValues::Page,
                         MeasuringUnits(2.5, MeasurementUnit::Centimeter),
                         Wp::VerticalRelativePositionValues::Paragraph,
                         MeasuringUnits(1.25, MeasurementUnit::Centimeter))
            .SetBehindText(true)
            .SetAllowOverlap(false)
            .SetAnchorLocked(true)
            .SetLayoutInCell(false)
            .SetRelativeHeight(251658240);
        CHECK(image->GetLayout() == ImageLayout::Floating);

        const auto bytes = editor->SaveToMemory();
        REQUIRE_FALSE(bytes.empty());

        const auto validation = ExyokiOfficeTests::ValidatePackage(bytes);
        REQUIRE(validation.Loaded);
        CAPTURE(validation.FirstError);
        CHECK_FALSE(validation.HasErrors);

        auto reopened = WordDocumentEditor::Open(bytes);
        REQUIRE(reopened != nullptr);
        const auto paragraphs = reopened->Paragraphs();
        REQUIRE_FALSE(paragraphs.empty());
        auto images = paragraphs.front()->Images();
        REQUIRE(images.size() == 1);
        auto reloaded = images.front();
        CHECK(reloaded->GetLayout() == ImageLayout::Floating);

        ExyokiOffice::Word::ImageWrapSettings wrap;
        REQUIRE(reloaded->TryGetWrap(wrap));
        CHECK(wrap.Wrap == ImageWrap::Tight);
        CHECK(wrap.WrapText == Wp::WrapTextValues::Left);

        ExyokiOffice::Word::ImagePosition position;
        REQUIRE(reloaded->TryGetPosition(position));
        CHECK(position.HorizontalFrom == Wp::HorizontalRelativePositionValues::Page);
        CHECK(position.VerticalFrom == Wp::VerticalRelativePositionValues::Paragraph);
        REQUIRE(position.HorizontalOffset.has_value());
        REQUIRE(position.VerticalOffset.has_value());
        CHECK(emu(*position.HorizontalOffset) == doctest::Approx(2.5 * kEmuPerCentimeter));
        CHECK(emu(*position.VerticalOffset) == doctest::Approx(1.25 * kEmuPerCentimeter));
        // Offsets and alignments are alternatives, not companions.
        CHECK_FALSE(position.HorizontalAlignment.has_value());
        CHECK_FALSE(position.VerticalAlignment.has_value());

        ExyokiOffice::Word::ImageAnchorOptions options;
        REQUIRE(reloaded->TryGetAnchorOptions(options));
        CHECK(options.BehindText);
        CHECK_FALSE(options.AllowOverlap);
        CHECK(options.AnchorLocked);
        CHECK_FALSE(options.LayoutInCell);
        CHECK(options.RelativeHeight == 251658240u);
        CHECK_FALSE(options.SimplePositionEnabled);
    }

    TEST_CASE("Aligned positioning and offset positioning replace one another [unit] [word] [word-image]")
    {
        // `wp:CT_PosH` is a choice: exactly one of `wp:align` and `wp:posOffset`.
        // Leaving the previous child behind would produce a document that carries
        // two contradictory positions and no longer validates, which is why each
        // setter removes the other kind rather than only writing its own.
        constexpr ExyokiOffice::Real kEmuPerCentimeter = 360000.0;
        const auto emu = [](const MeasuringUnits& value)
        { return value.ToEmu().GetValue(); };

        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);
        auto image = editor->AddImageFromData(BuildPng(32, 32), ImageLayout::Floating, ImageWrap::Square);
        REQUIRE(image != nullptr);

        image->SetPosition(Wp::HorizontalRelativePositionValues::Margin,
                           MeasuringUnits(1.0, MeasurementUnit::Centimeter),
                           Wp::VerticalRelativePositionValues::Line,
                           MeasuringUnits(2.0, MeasurementUnit::Centimeter));

        ExyokiOffice::Word::ImagePosition position;
        REQUIRE(image->TryGetPosition(position));
        REQUIRE(position.HorizontalOffset.has_value());
        CHECK(emu(*position.HorizontalOffset) == doctest::Approx(1.0 * kEmuPerCentimeter));

        image->SetPositionAligned(Wp::HorizontalRelativePositionValues::Page,
                                  Wp::HorizontalAlignmentValues::Center,
                                  Wp::VerticalRelativePositionValues::Page,
                                  Wp::VerticalAlignmentValues::Top);

        position = ExyokiOffice::Word::ImagePosition{};
        REQUIRE(image->TryGetPosition(position));
        CHECK(position.HorizontalFrom == Wp::HorizontalRelativePositionValues::Page);
        CHECK(position.VerticalFrom == Wp::VerticalRelativePositionValues::Page);
        REQUIRE(position.HorizontalAlignment.has_value());
        REQUIRE(position.VerticalAlignment.has_value());
        CHECK(*position.HorizontalAlignment == Wp::HorizontalAlignmentValues::Center);
        CHECK(*position.VerticalAlignment == Wp::VerticalAlignmentValues::Top);
        CHECK_FALSE(position.HorizontalOffset.has_value());
        CHECK_FALSE(position.VerticalOffset.has_value());

        const auto aligned = editor->SaveToMemory();
        const auto alignedValidation = ExyokiOfficeTests::ValidatePackage(aligned);
        REQUIRE(alignedValidation.Loaded);
        CAPTURE(alignedValidation.FirstError);
        CHECK_FALSE(alignedValidation.HasErrors);

        // Back to offsets: the alignment children have to disappear again.
        image->SetPosition(Wp::HorizontalRelativePositionValues::Column,
                           MeasuringUnits(0.5, MeasurementUnit::Centimeter),
                           Wp::VerticalRelativePositionValues::Paragraph,
                           MeasuringUnits(0.75, MeasurementUnit::Centimeter));

        position = ExyokiOffice::Word::ImagePosition{};
        REQUIRE(image->TryGetPosition(position));
        CHECK_FALSE(position.HorizontalAlignment.has_value());
        CHECK_FALSE(position.VerticalAlignment.has_value());
        REQUIRE(position.HorizontalOffset.has_value());
        CHECK(emu(*position.HorizontalOffset) == doctest::Approx(0.5 * kEmuPerCentimeter));
    }

    TEST_CASE("Simple positioning round-trips with its coordinates [unit] [word] [word-image]")
    {
        // `wp:simplePos` is the anchor's escape hatch for absolute page
        // coordinates. Writing the coordinates implies enabling the flag - a
        // `<wp:simplePos>` child that the `simplePos` attribute does not switch on
        // is ignored by Word, so the two must not be settable apart.
        constexpr ExyokiOffice::Real kEmuPerCentimeter = 360000.0;
        const auto emu = [](const MeasuringUnits& value)
        { return value.ToEmu().GetValue(); };

        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);
        auto image = editor->AddImageFromData(BuildPng(32, 32), ImageLayout::Inline, ImageWrap::Square);
        REQUIRE(image != nullptr);

        image->SetSimplePosition(MeasuringUnits(3.0, MeasurementUnit::Centimeter),
                                 MeasuringUnits(4.0, MeasurementUnit::Centimeter));
        CHECK(image->GetLayout() == ImageLayout::Floating);

        auto reopened = WordDocumentEditor::Open(editor->SaveToMemory());
        REQUIRE(reopened != nullptr);
        const auto paragraphs = reopened->Paragraphs();
        REQUIRE_FALSE(paragraphs.empty());
        auto images = paragraphs.front()->Images();
        REQUIRE(images.size() == 1);

        ExyokiOffice::Word::ImageAnchorOptions options;
        REQUIRE(images.front()->TryGetAnchorOptions(options));
        CHECK(options.SimplePositionEnabled);
        REQUIRE(options.SimplePosition.has_value());
        CHECK(emu(options.SimplePosition->X) == doctest::Approx(3.0 * kEmuPerCentimeter));
        CHECK(emu(options.SimplePosition->Y) == doctest::Approx(4.0 * kEmuPerCentimeter));

        // Turning the flag off leaves the coordinates in place but stops Word
        // honoring them, which is what the separate setter is for.
        images.front()->SetSimplePositionEnabled(false);
        options = ExyokiOffice::Word::ImageAnchorOptions{};
        REQUIRE(images.front()->TryGetAnchorOptions(options));
        CHECK_FALSE(options.SimplePositionEnabled);
        REQUIRE(options.SimplePosition.has_value());
    }

    TEST_CASE("Every wrap mode is written and read back as itself [unit] [word] [word-image]")
    {
        // One wrap element at a time: setting a new mode has to remove the
        // previous one, or the anchor ends up with two mutually exclusive wrap
        // children and the first one found wins silently.
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);
        auto image = editor->AddImageFromData(BuildPng(32, 32), ImageLayout::Inline, ImageWrap::Square);
        REQUIRE(image != nullptr);

        const ImageWrap modes[] = {ImageWrap::Square, ImageWrap::Tight, ImageWrap::Through,
                                   ImageWrap::TopAndBottom, ImageWrap::None};
        for (const ImageWrap mode : modes)
        {
            CAPTURE(static_cast<int>(mode));
            image->SetWrap(mode, Wp::WrapTextValues::Largest);

            ExyokiOffice::Word::ImageWrapSettings wrap;
            REQUIRE(image->TryGetWrap(wrap));
            CHECK(wrap.Wrap == mode);
            // Only the side-wrapping modes carry a wrapText preference; the other
            // two report the schema default rather than the value passed in.
            const bool carriesWrapText =
                mode == ImageWrap::Square || mode == ImageWrap::Tight || mode == ImageWrap::Through;
            CHECK(wrap.WrapText ==
                  (carriesWrapText ? Wp::WrapTextValues::Largest : Wp::WrapTextValues::BothSides));
        }

        const auto bytes = editor->SaveToMemory();
        const auto validation = ExyokiOfficeTests::ValidatePackage(bytes);
        REQUIRE(validation.Loaded);
        CAPTURE(validation.FirstError);
        CHECK_FALSE(validation.HasErrors);

        auto reopened = WordDocumentEditor::Open(bytes);
        REQUIRE(reopened != nullptr);
        auto images = reopened->Paragraphs().front()->Images();
        REQUIRE(images.size() == 1);
        ExyokiOffice::Word::ImageWrapSettings wrap;
        REQUIRE(images.front()->TryGetWrap(wrap));
        CHECK(wrap.Wrap == ImageWrap::None);
    }

} // TEST_SUITE("WordImageTests")
