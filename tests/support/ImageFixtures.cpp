// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "ImageFixtures.hpp"

namespace ExyokiOfficeTests
{

ImageBytes PngHeader(ExyokiOffice::UInt32 width, ExyokiOffice::UInt32 height)
{
    ImageBytes png;
    png.U8({0x89}).Ascii("PNG").U8({0x0D, 0x0A, 0x1A, 0x0A});
    png.Be32(13).Ascii("IHDR").Be32(width).Be32(height);
    png.U8({8, 6, 0, 0, 0}); // bit depth, color type, compression, filter, interlace
    png.Be32(0);             // CRC is never validated by the detector
    return png;
}

void AppendPngChunk(ImageBytes& png,
                    std::string_view type,
                    ExyokiOffice::UInt32 length,
                    const std::function<void(ImageBytes&)>& payload)
{
    png.Be32(length).Ascii(type);
    payload(png);
    png.Be32(0); // CRC
}

void AppendJfifApp0(ImageBytes& jpeg,
                    int units,
                    ExyokiOffice::UInt32 xDensity,
                    ExyokiOffice::UInt32 yDensity)
{
    jpeg.U8({0xFF, 0xE0}).Be16(16).Ascii("JFIF").U8({0x00, 0x01, 0x02});
    jpeg.U8({units}).Be16(xDensity).Be16(yDensity).U8({0, 0});
}

void AppendSof(ImageBytes& jpeg, int marker, ExyokiOffice::UInt32 width, ExyokiOffice::UInt32 height)
{
    // Length 11 = the length field itself, precision, height, width, component
    // count, and one component entry (id, sampling factors, quantization table).
    jpeg.U8({0xFF, marker}).Be16(11).U8({8}).Be16(height).Be16(width).U8({1, 1, 0x11, 0});
}

ImageBytes BmpHeader(ExyokiOffice::UInt32 width,
                     ExyokiOffice::Int32 height,
                     ExyokiOffice::Int32 pixelsPerMeterX,
                     ExyokiOffice::Int32 pixelsPerMeterY)
{
    ImageBytes bmp;
    bmp.Ascii("BM").Le32(54).Le16(0).Le16(0).Le32(54);
    bmp.Le32(40).Le32(width).Le32(static_cast<ExyokiOffice::UInt32>(height));
    bmp.Le16(1).Le16(24).Le32(0).Le32(0);
    bmp.Le32(static_cast<ExyokiOffice::UInt32>(pixelsPerMeterX))
        .Le32(static_cast<ExyokiOffice::UInt32>(pixelsPerMeterY));
    bmp.Le32(0).Le32(0);
    return bmp;
}

std::vector<ExyokiOffice::Byte> BuildPng(
    ExyokiOffice::UInt32 width,
    ExyokiOffice::UInt32 height,
    std::optional<std::pair<ExyokiOffice::UInt32, ExyokiOffice::UInt32>> pixelsPerMeter)
{
    ImageBytes png = PngHeader(width, height);
    if (pixelsPerMeter)
    {
        AppendPngChunk(png, "pHYs", 9,
                       [&pixelsPerMeter](ImageBytes& bytes)
                       {
                           bytes.Be32(pixelsPerMeter->first).Be32(pixelsPerMeter->second);
                           bytes.U8({1}); // unit: meters
                       });
    }
    AppendPngChunk(png, "IEND", 0, [](ImageBytes&) {});
    return png.Bytes();
}

std::vector<ExyokiOffice::Byte> BuildJpeg(ExyokiOffice::UInt16 width,
                                          ExyokiOffice::UInt16 height,
                                          ExyokiOffice::UInt16 dpi)
{
    ImageBytes jpeg;
    jpeg.U8({0xFF, 0xD8}); // SOI
    AppendJfifApp0(jpeg, dpi > 0 ? 1 : 0, dpi, dpi);
    AppendSof(jpeg, 0xC0, width, height);
    jpeg.U8({0xFF, 0xD9}); // EOI
    return jpeg.Bytes();
}

std::vector<ExyokiOffice::Byte> BuildGif(ExyokiOffice::UInt16 width, ExyokiOffice::UInt16 height)
{
    ImageBytes gif;
    gif.Ascii("GIF89a").Le16(width).Le16(height);
    gif.U8({0, 0, 0}); // packed fields, background color index, pixel aspect ratio
    return gif.Bytes();
}

std::vector<ExyokiOffice::Byte> BuildBmp(ExyokiOffice::Int32 width,
                                         ExyokiOffice::Int32 height,
                                         ExyokiOffice::Int32 pixelsPerMeterX,
                                         ExyokiOffice::Int32 pixelsPerMeterY)
{
    return BmpHeader(static_cast<ExyokiOffice::UInt32>(width), height, pixelsPerMeterX,
                     pixelsPerMeterY)
        .Bytes();
}

} // namespace ExyokiOfficeTests
