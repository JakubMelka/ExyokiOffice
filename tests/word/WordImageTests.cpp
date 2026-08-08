// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "doctest.h"

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
using ExyokiOffice::MeasurementUnit;
using ExyokiOffice::MeasuringUnits;
using ExyokiOffice::Word::Image;
using ExyokiOffice::Word::ImageCrop;
using ExyokiOffice::Word::ImageFormatInfo;
using ExyokiOffice::Word::ImageHyperlink;
using ExyokiOffice::Word::ImageLayout;
using ExyokiOffice::Word::ImageWrap;
using ExyokiOffice::Word::WordDocumentEditor;

void PushU16BE(std::vector<ExyokiOffice::Byte>& data, ExyokiOffice::UInt16 value)
{
    data.push_back(static_cast<ExyokiOffice::UInt8>((value >> 8) & 0xFF));
    data.push_back(static_cast<ExyokiOffice::UInt8>(value & 0xFF));
}

void PushU32BE(std::vector<ExyokiOffice::Byte>& data, ExyokiOffice::UInt32 value)
{
    data.push_back(static_cast<ExyokiOffice::UInt8>((value >> 24) & 0xFF));
    data.push_back(static_cast<ExyokiOffice::UInt8>((value >> 16) & 0xFF));
    data.push_back(static_cast<ExyokiOffice::UInt8>((value >> 8) & 0xFF));
    data.push_back(static_cast<ExyokiOffice::UInt8>(value & 0xFF));
}

void PushFourCC(std::vector<ExyokiOffice::Byte>& data, const char (&fourCc)[5])
{
    for (int i = 0; i < 4; ++i)
    {
        data.push_back(static_cast<ExyokiOffice::UInt8>(fourCc[i]));
    }
}

/// Builds a minimal, syntactically valid PNG payload (fake CRCs; DetectImageFormat
/// does not validate them). An optional pHYs chunk carries physical pixel density.
std::vector<ExyokiOffice::Byte> BuildPng(ExyokiOffice::UInt32 width,
                                         ExyokiOffice::UInt32 height,
                                         std::optional<std::pair<ExyokiOffice::UInt32, ExyokiOffice::UInt32>> pixelsPerMeter = std::nullopt)
{
    std::vector<ExyokiOffice::Byte> data;
    static constexpr ExyokiOffice::UInt8 kSignature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    data.insert(data.end(), std::begin(kSignature), std::end(kSignature));

    // IHDR
    PushU32BE(data, 13);
    PushFourCC(data, "IHDR");
    PushU32BE(data, width);
    PushU32BE(data, height);
    data.push_back(8);  // bit depth
    data.push_back(6);  // color type (RGBA)
    data.push_back(0);  // compression
    data.push_back(0);  // filter
    data.push_back(0);  // interlace
    PushU32BE(data, 0); // fake CRC

    if (pixelsPerMeter)
    {
        PushU32BE(data, 9);
        PushFourCC(data, "pHYs");
        PushU32BE(data, pixelsPerMeter->first);
        PushU32BE(data, pixelsPerMeter->second);
        data.push_back(1);  // unit: meters
        PushU32BE(data, 0); // fake CRC
    }

    // IEND
    PushU32BE(data, 0);
    PushFourCC(data, "IEND");
    PushU32BE(data, 0); // fake CRC
    return data;
}

/// Builds a minimal JPEG payload: SOI, optional JFIF APP0 (for DPI), SOF0, EOI.
/// No entropy-coded scan data is included since DetectImageFormat only reads headers.
std::vector<ExyokiOffice::Byte> BuildJpeg(ExyokiOffice::UInt16 width, ExyokiOffice::UInt16 height, ExyokiOffice::UInt16 dpi = 0)
{
    std::vector<ExyokiOffice::Byte> data;
    data.push_back(0xFF);
    data.push_back(0xD8); // SOI

    data.push_back(0xFF);
    data.push_back(0xE0); // APP0
    PushU16BE(data, 16);  // segment length
    data.push_back('J');
    data.push_back('F');
    data.push_back('I');
    data.push_back('F');
    data.push_back(0x00);
    data.push_back(1);               // version major
    data.push_back(2);               // version minor
    data.push_back(dpi > 0 ? 1 : 0); // units: 1 = dots per inch
    PushU16BE(data, dpi);
    PushU16BE(data, dpi);
    data.push_back(0); // thumbnail width
    data.push_back(0); // thumbnail height

    data.push_back(0xFF);
    data.push_back(0xC0); // SOF0
    PushU16BE(data, 11);  // segment length
    data.push_back(8);    // sample precision
    PushU16BE(data, height);
    PushU16BE(data, width);
    data.push_back(1);    // number of components
    data.push_back(1);    // component id
    data.push_back(0x11); // sampling factors
    data.push_back(0);    // quantization table id

    data.push_back(0xFF);
    data.push_back(0xD9); // EOI
    return data;
}

std::vector<ExyokiOffice::Byte> BuildGif(ExyokiOffice::UInt16 width, ExyokiOffice::UInt16 height)
{
    std::vector<ExyokiOffice::Byte> data = {'G', 'I', 'F', '8', '9', 'a'};
    data.push_back(static_cast<ExyokiOffice::UInt8>(width & 0xFF));
    data.push_back(static_cast<ExyokiOffice::UInt8>((width >> 8) & 0xFF));
    data.push_back(static_cast<ExyokiOffice::UInt8>(height & 0xFF));
    data.push_back(static_cast<ExyokiOffice::UInt8>((height >> 8) & 0xFF));
    data.push_back(0); // packed fields
    data.push_back(0); // background color index
    data.push_back(0); // pixel aspect ratio
    return data;
}

std::vector<ExyokiOffice::Byte> BuildBmp(ExyokiOffice::Int32 width, ExyokiOffice::Int32 height, ExyokiOffice::Int32 pixelsPerMeterX = 0, ExyokiOffice::Int32 pixelsPerMeterY = 0)
{
    std::vector<ExyokiOffice::Byte> data(54, 0);
    data[0] = 'B';
    data[1] = 'M';

    const auto writeS32LE = [&data](ExyokiOffice::Size offset, ExyokiOffice::Int32 value)
    {
        const auto unsignedValue = static_cast<ExyokiOffice::UInt32>(value);
        data[offset] = static_cast<ExyokiOffice::UInt8>(unsignedValue & 0xFF);
        data[offset + 1] = static_cast<ExyokiOffice::UInt8>((unsignedValue >> 8) & 0xFF);
        data[offset + 2] = static_cast<ExyokiOffice::UInt8>((unsignedValue >> 16) & 0xFF);
        data[offset + 3] = static_cast<ExyokiOffice::UInt8>((unsignedValue >> 24) & 0xFF);
    };
    writeS32LE(18, width);
    writeS32LE(22, height);
    writeS32LE(38, pixelsPerMeterX);
    writeS32LE(42, pixelsPerMeterY);
    return data;
}

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

} // TEST_SUITE("WordImageTests")
