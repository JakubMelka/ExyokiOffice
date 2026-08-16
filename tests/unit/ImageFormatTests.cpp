// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

// DetectImageFormat decides the content type, the part extension and - through
// the detected DPI - the rendered size of every raster image placed into a
// document, so each branch here is a visible property of a saved package.
// The fixtures are minimal headers assembled byte by byte: what is being
// tested is signature parsing, not codec correctness, and a real image would
// only obscure which field a case exercises.
//
// The happy path of each format is pinned by WordImageTests.cpp; this file
// owns the variants - the other density units, the markers that look like a
// start-of-frame but are not, the spec ordering rules, and the rejections.

#include "ExyokiOffice/ImageFormat.hpp"

#include "ImageFixtures.hpp"

#include <doctest/doctest.h>

#include <limits>
#include <string_view>

using namespace ExyokiOffice;

using ExyokiOfficeTests::AppendJfifApp0;
using ExyokiOfficeTests::AppendPngChunk;
using ExyokiOfficeTests::AppendSof;
using ExyokiOfficeTests::BmpHeader;
using ExyokiOfficeTests::ImageBytes;
using ExyokiOfficeTests::PngHeader;

TEST_CASE("PNG detection honors only a valid pHYs chunk [image-format]")
{
    SUBCASE("pHYs in meters converts pixels per meter to DPI")
    {
        auto png = PngHeader(10, 10);
        // 5000 px/m * 0.0254 m/in = 127 DPI exactly.
        AppendPngChunk(png, "pHYs", 9,
                       [](ImageBytes& b)
                       { b.Be32(5000).Be32(5000).U8({1}); });
        const auto info = DetectImageFormat(png.Span());
        REQUIRE(info);
        CHECK(info->HorizontalDpi == doctest::Approx(127.0));
        CHECK(info->VerticalDpi == doctest::Approx(127.0));
    }

    SUBCASE("pHYs with unit 0 is an aspect ratio, not a physical density")
    {
        auto png = PngHeader(10, 10);
        AppendPngChunk(png, "pHYs", 9,
                       [](ImageBytes& b)
                       { b.Be32(5000).Be32(2500).U8({0}); });
        const auto info = DetectImageFormat(png.Span());
        REQUIRE(info);
        CHECK(info->HorizontalDpi == doctest::Approx(96.0));
        CHECK(info->VerticalDpi == doctest::Approx(96.0));
    }

    SUBCASE("pHYs with a zero density falls back to the default")
    {
        auto png = PngHeader(10, 10);
        AppendPngChunk(png, "pHYs", 9,
                       [](ImageBytes& b)
                       { b.Be32(0).Be32(5000).U8({1}); });
        const auto info = DetectImageFormat(png.Span());
        REQUIRE(info);
        CHECK(info->HorizontalDpi == doctest::Approx(96.0));
    }

    SUBCASE("a pHYs chunk after IDAT is ignored, matching the spec ordering")
    {
        auto png = PngHeader(10, 10);
        AppendPngChunk(png, "IDAT", 2, [](ImageBytes& b)
                       { b.U8({0, 0}); });
        AppendPngChunk(png, "pHYs", 9,
                       [](ImageBytes& b)
                       { b.Be32(5000).Be32(5000).U8({1}); });
        const auto info = DetectImageFormat(png.Span());
        REQUIRE(info);
        CHECK(info->HorizontalDpi == doctest::Approx(96.0));
    }

    SUBCASE("a truncated chunk stops the scan without invalidating the image")
    {
        auto png = PngHeader(10, 10);
        // Declares nine payload bytes but the file ends after the type tag.
        png.Be32(9).Ascii("pHYs");
        const auto info = DetectImageFormat(png.Span());
        REQUIRE(info);
        CHECK(info->PixelWidth == 10);
        CHECK(info->HorizontalDpi == doctest::Approx(96.0));
    }

    SUBCASE("a corrupted IHDR tag is rejected")
    {
        auto png = PngHeader(10, 10);
        ImageBytes corrupted;
        corrupted.U8({0x89}).Ascii("PNG").U8({0x0D, 0x0A, 0x1A, 0x0A});
        corrupted.Be32(13).Ascii("XHDR").Be32(10).Be32(10).U8({8, 6, 0, 0, 0}).Be32(0);
        CHECK_FALSE(DetectImageFormat(corrupted.Span()));
    }

    SUBCASE("a file shorter than signature plus IHDR is rejected")
    {
        ImageBytes stub;
        stub.U8({0x89}).Ascii("PNG").U8({0x0D, 0x0A, 0x1A, 0x0A}).Zeros(8);
        CHECK_FALSE(DetectImageFormat(stub.Span()));
    }
}

TEST_CASE("JPEG detection walks markers to SOF and reads JFIF densities [image-format]")
{
    SUBCASE("baseline SOF0 with JFIF DPI units")
    {
        ImageBytes jpeg;
        jpeg.U8({0xFF, 0xD8});
        AppendJfifApp0(jpeg, 1, 300, 200);
        AppendSof(jpeg, 0xC0, 800, 600);
        const auto info = DetectImageFormat(jpeg.Span());
        REQUIRE(info);
        CHECK(info->ContentType == "image/jpeg");
        CHECK(info->Extension == ".jpg");
        CHECK(info->PixelWidth == 800);
        CHECK(info->PixelHeight == 600);
        CHECK(info->HorizontalDpi == doctest::Approx(300.0));
        CHECK(info->VerticalDpi == doctest::Approx(200.0));
    }

    SUBCASE("JFIF dots per centimeter are converted to DPI")
    {
        ImageBytes jpeg;
        jpeg.U8({0xFF, 0xD8});
        AppendJfifApp0(jpeg, 2, 100, 100); // 100 dots/cm = 254 DPI
        AppendSof(jpeg, 0xC0, 4, 4);
        const auto info = DetectImageFormat(jpeg.Span());
        REQUIRE(info);
        CHECK(info->HorizontalDpi == doctest::Approx(254.0));
        CHECK(info->VerticalDpi == doctest::Approx(254.0));
    }

    SUBCASE("JFIF unit 0 is an aspect ratio and keeps the default DPI")
    {
        ImageBytes jpeg;
        jpeg.U8({0xFF, 0xD8});
        AppendJfifApp0(jpeg, 0, 4, 3);
        AppendSof(jpeg, 0xC0, 4, 4);
        const auto info = DetectImageFormat(jpeg.Span());
        REQUIRE(info);
        CHECK(info->HorizontalDpi == doctest::Approx(96.0));
    }

    SUBCASE("a zero JFIF density keeps the default DPI")
    {
        ImageBytes jpeg;
        jpeg.U8({0xFF, 0xD8});
        AppendJfifApp0(jpeg, 1, 0, 300);
        AppendSof(jpeg, 0xC0, 4, 4);
        const auto info = DetectImageFormat(jpeg.Span());
        REQUIRE(info);
        CHECK(info->HorizontalDpi == doctest::Approx(96.0));
    }

    SUBCASE("progressive SOF2 carries the dimensions like SOF0")
    {
        ImageBytes jpeg;
        jpeg.U8({0xFF, 0xD8});
        AppendSof(jpeg, 0xC2, 1024, 768);
        const auto info = DetectImageFormat(jpeg.Span());
        REQUIRE(info);
        CHECK(info->PixelWidth == 1024);
        CHECK(info->PixelHeight == 768);
    }

    SUBCASE("DHT, JPG and DAC markers sit in the SOF range but carry no frame")
    {
        // 0xC4, 0xC8 and 0xCC would each be misread as a start-of-frame by a
        // plain range check; the dimensions must come from the SOF0 after them.
        ImageBytes jpeg;
        jpeg.U8({0xFF, 0xD8});
        jpeg.U8({0xFF, 0xC4}).Be16(4).U8({0, 0});
        jpeg.U8({0xFF, 0xC8}).Be16(4).U8({0, 0});
        jpeg.U8({0xFF, 0xCC}).Be16(4).U8({0, 0});
        AppendSof(jpeg, 0xC0, 32, 16);
        const auto info = DetectImageFormat(jpeg.Span());
        REQUIRE(info);
        CHECK(info->PixelWidth == 32);
        CHECK(info->PixelHeight == 16);
    }

    SUBCASE("restart markers, TEM and fill bytes are skipped without a length")
    {
        ImageBytes jpeg;
        jpeg.U8({0xFF, 0xD8});
        jpeg.U8({0xFF, 0xD0}).U8({0xFF, 0xD7}); // RST0, RST7
        jpeg.U8({0xFF, 0x01});                  // TEM
        jpeg.U8({0xFF, 0xFF});                  // fill byte before a marker
        AppendSof(jpeg, 0xC0, 8, 8);
        const auto info = DetectImageFormat(jpeg.Span());
        REQUIRE(info);
        CHECK(info->PixelWidth == 8);
    }

    SUBCASE("EOI before any SOF yields no dimensions and no detection")
    {
        // Trailing bytes keep the buffer long enough for the marker loop to
        // actually reach the EOI branch; a bare 4-byte SOI+EOI never enters
        // the loop and would pass for the same reason as "SOI alone".
        ImageBytes jpeg;
        jpeg.U8({0xFF, 0xD8, 0xFF, 0xD9}).Zeros(8);
        CHECK_FALSE(DetectImageFormat(jpeg.Span()));
    }

    SUBCASE("a segment length overrunning the file stops the scan")
    {
        ImageBytes jpeg;
        jpeg.U8({0xFF, 0xD8});
        jpeg.U8({0xFF, 0xE1}).Be16(1000).U8({0, 0});
        CHECK_FALSE(DetectImageFormat(jpeg.Span()));
    }

    SUBCASE("SOI alone is not an image")
    {
        ImageBytes jpeg;
        jpeg.U8({0xFF, 0xD8});
        CHECK_FALSE(DetectImageFormat(jpeg.Span()));
    }
}

TEST_CASE("GIF detection accepts both spec versions and nothing else [image-format]")
{
    SUBCASE("GIF87a and GIF89a both parse with little-endian dimensions")
    {
        for (const std::string_view header : {"GIF87a", "GIF89a"})
        {
            ImageBytes gif;
            gif.Ascii(header).Le16(320).Le16(200);
            const auto info = DetectImageFormat(gif.Span());
            REQUIRE(info);
            CHECK(info->ContentType == "image/gif");
            CHECK(info->Extension == ".gif");
            CHECK(info->PixelWidth == 320);
            CHECK(info->PixelHeight == 200);
        }
    }

    SUBCASE("an unknown version byte is rejected")
    {
        ImageBytes gif;
        gif.Ascii("GIF88a").Le16(320).Le16(200);
        CHECK_FALSE(DetectImageFormat(gif.Span()));
    }

    SUBCASE("a wrong trailing signature byte is rejected")
    {
        ImageBytes gif;
        gif.Ascii("GIF87b").Le16(320).Le16(200);
        CHECK_FALSE(DetectImageFormat(gif.Span()));
    }

    SUBCASE("a file cut inside the dimensions is rejected")
    {
        ImageBytes gif;
        gif.Ascii("GIF87a").Le16(320).U8({0});
        CHECK_FALSE(DetectImageFormat(gif.Span()));
    }
}

TEST_CASE("BMP detection guards the density fields and the header size [image-format]")
{
    SUBCASE("a zero or negative density keeps the default DPI")
    {
        const auto zeroX = BmpHeader(4, 4, 0, 5000);
        const auto negativeY = BmpHeader(4, 4, 5000, -5000);
        for (const auto* bmp : {&zeroX, &negativeY})
        {
            const auto info = DetectImageFormat(bmp->Span());
            REQUIRE(info);
            CHECK(info->HorizontalDpi == doctest::Approx(96.0));
            CHECK(info->VerticalDpi == doctest::Approx(96.0));
        }
    }

    SUBCASE("a file shorter than both headers is rejected")
    {
        ImageBytes bmp;
        bmp.Ascii("BM").Zeros(51); // 53 bytes, one short of the minimum
        CHECK_FALSE(DetectImageFormat(bmp.Span()));
    }
}

TEST_CASE("JPEG resolution comes from Exif when JFIF does not state one [image-format]")
{
    // Cameras and phones write their resolution into Exif and leave the JFIF
    // segment claiming "no units". Reading only JFIF made every such photo
    // nominally 96 DPI, and a 4000 pixel wide picture forty-one inches across.
    const auto jpeg = ExyokiOfficeTests::BuildJpegWithExifDpi(4000, 3000, 300);
    const auto info = DetectImageFormat(jpeg);
    REQUIRE(info);
    CHECK(info->ContentType == "image/jpeg");
    CHECK(info->PixelWidth == 4000);
    CHECK(info->PixelHeight == 3000);
    CHECK(info->HorizontalDpi == doctest::Approx(300.0));
    CHECK(info->VerticalDpi == doctest::Approx(300.0));

    // JFIF still wins when it states a real unit: it comes first in the file
    // and a reader that found one has no reason to look further.
    const auto jfif = ExyokiOfficeTests::BuildJpeg(100, 50, 200);
    const auto jfifInfo = DetectImageFormat(jfif);
    REQUIRE(jfifInfo);
    CHECK(jfifInfo->HorizontalDpi == doctest::Approx(200.0));
}

TEST_CASE("TIFF is detected with its dimensions and resolution [image-format]")
{
    const auto tiff = ExyokiOfficeTests::BuildTiff(640, 480, 150);
    const auto info = DetectImageFormat(tiff);
    REQUIRE(info);
    CHECK(info->ContentType == "image/tiff");
    CHECK(info->Extension == ".tiff");
    CHECK(info->PixelWidth == 640);
    CHECK(info->PixelHeight == 480);
    CHECK(info->HorizontalDpi == doctest::Approx(150.0));

    // A header that claims a directory the file does not contain is refused
    // rather than read past the end of the buffer.
    ImageBytes truncated;
    truncated.Ascii("II").Le16(42).Le32(4096);
    CHECK_FALSE(DetectImageFormat(truncated.Span()));
}

TEST_CASE("metafiles are detected from their own frame [image-format]")
{
    SUBCASE("EMF states its frame in hundredths of a millimetre")
    {
        // 2540 hundredths of a millimetre is exactly one inch, which is 96
        // pixels at the nominal density metafiles are expressed in.
        const auto emf = ExyokiOfficeTests::BuildEmf(2540, 5080);
        const auto info = DetectImageFormat(emf);
        REQUIRE(info);
        CHECK(info->ContentType == "image/x-emf");
        CHECK(info->Extension == ".emf");
        CHECK(info->PixelWidth == 96);
        CHECK(info->PixelHeight == 192);
    }

    SUBCASE("an EMF frame spanning the whole Int32 range is measured without overflowing")
    {
        // rclFrame is four untrusted 32-bit fields. `right > left` says nothing
        // about the difference fitting in an Int32, and INT32_MIN to INT32_MAX
        // passes it: subtracting there is undefined behavior, and where it
        // wrapped it produced -1, which PixelsAt96Dpi turned into a zero width
        // and the detector then rejected. Widened first, the frame is simply
        // enormous.
        const auto emf = ExyokiOfficeTests::BuildEmfFrame(std::numeric_limits<Int32>::min(),
                                                          std::numeric_limits<Int32>::min(),
                                                          std::numeric_limits<Int32>::max(),
                                                          std::numeric_limits<Int32>::max());
        const auto info = DetectImageFormat(emf);
        REQUIRE(info);
        // About 1.69 million inches across. Nothing here bounds an image's
        // dimensions - a PNG is believed about its IHDR too - so what this pins
        // is that the arithmetic is defined, not that the number is sensible.
        CHECK(info->PixelWidth > 100000000U);
        CHECK(info->PixelWidth == info->PixelHeight);
    }

    SUBCASE("a placeable WMF states its bounding box in its own units")
    {
        const auto wmf = ExyokiOfficeTests::BuildPlaceableWmf(1440, 720, 1440);
        const auto info = DetectImageFormat(wmf);
        REQUIRE(info);
        CHECK(info->ContentType == "image/x-wmf");
        CHECK(info->Extension == ".wmf");
        CHECK(info->PixelWidth == 96);
        CHECK(info->PixelHeight == 48);
    }

    SUBCASE("a WMF without the placeable header states no size and is not detected")
    {
        // A bare WMF carries no bounding box, so there is nothing to lay it out
        // from; it can still be added with an explicit content type and size.
        ImageBytes bare;
        bare.Le16(1).Le16(9).Le16(0x0300).Zeros(32);
        CHECK_FALSE(DetectImageFormat(bare.Span()));
    }
}

TEST_CASE("unrecognized payloads are rejected rather than guessed [image-format]")
{
    CHECK_FALSE(DetectImageFormat({}));

    // SVG is a vector format Word only renders with a raster fallback beside
    // it, so it is deliberately not auto-detected: a caller who has both passes
    // the content type explicitly.
    ImageBytes text;
    text.Ascii("<svg xmlns='http://www.w3.org/2000/svg'/>");
    CHECK_FALSE(DetectImageFormat(text.Span()));

    ImageBytes zeros;
    zeros.Zeros(64);
    CHECK_FALSE(DetectImageFormat(zeros.Span()));
}
