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

namespace
{

/// Appends one 12-byte IFD entry whose value fits in the entry itself.
void AppendInlineTiffEntry(ImageBytes& tiff,
                           ExyokiOffice::UInt32 tag,
                           ExyokiOffice::UInt32 type,
                           ExyokiOffice::UInt32 value)
{
    tiff.Le16(tag).Le16(type).Le32(1);
    if (type == 3) // SHORT: the value sits in the low half, the rest is padding.
    {
        tiff.Le16(value).Le16(0);
    }
    else
    {
        tiff.Le32(value);
    }
}

/// Appends a RATIONAL entry, whose value is always addressed by offset.
void AppendRationalTiffEntry(ImageBytes& tiff, ExyokiOffice::UInt32 tag, ExyokiOffice::UInt32 valueOffset)
{
    tiff.Le16(tag).Le16(5).Le32(1).Le32(valueOffset);
}

/**
 * A little-endian TIFF header plus one directory, as a standalone block.
 *
 * The layout is fixed so every offset can be written as a constant: header (8
 * bytes), entry count (2), five entries (60), the next-directory pointer (4),
 * and then the two rationals the resolution tags point at.
 */
std::vector<ExyokiOffice::Byte> BuildTiffBlock(ExyokiOffice::UInt32 width,
                                               ExyokiOffice::UInt32 height,
                                               ExyokiOffice::UInt32 dpi)
{
    constexpr ExyokiOffice::UInt32 kDirectory = 8;
    constexpr ExyokiOffice::UInt32 kEntryCount = 5;
    constexpr ExyokiOffice::UInt32 kAfterDirectory = kDirectory + 2 + kEntryCount * 12 + 4;
    constexpr ExyokiOffice::UInt32 kHorizontalValue = kAfterDirectory;
    constexpr ExyokiOffice::UInt32 kVerticalValue = kAfterDirectory + 8;

    ImageBytes tiff;
    tiff.Ascii("II").Le16(42).Le32(kDirectory);
    tiff.Le16(kEntryCount);
    AppendInlineTiffEntry(tiff, 0x0100, 4, width);  // ImageWidth, LONG
    AppendInlineTiffEntry(tiff, 0x0101, 4, height); // ImageLength, LONG
    AppendRationalTiffEntry(tiff, 0x011A, kHorizontalValue);
    AppendRationalTiffEntry(tiff, 0x011B, kVerticalValue);
    AppendInlineTiffEntry(tiff, 0x0128, 3, 2); // ResolutionUnit: inch
    tiff.Le32(0);                              // no next directory
    tiff.Le32(dpi > 0 ? dpi : 1).Le32(1);
    tiff.Le32(dpi > 0 ? dpi : 1).Le32(1);
    return tiff.Bytes();
}

} // namespace

std::vector<ExyokiOffice::Byte> BuildJpegWithExifDpi(ExyokiOffice::UInt16 width,
                                                     ExyokiOffice::UInt16 height,
                                                     ExyokiOffice::UInt32 dpi)
{
    const auto tiff = BuildTiffBlock(width, height, dpi);

    ImageBytes jpeg;
    jpeg.U8({0xFF, 0xD8});         // SOI
    AppendJfifApp0(jpeg, 0, 0, 0); // JFIF present but claiming no units, as cameras write it

    // APP1: length covers itself, the Exif marker, and the TIFF block.
    jpeg.U8({0xFF, 0xE1});
    jpeg.Be16(static_cast<ExyokiOffice::UInt32>(2 + 6 + tiff.size()));
    jpeg.Ascii("Exif").U8({0, 0});
    for (const auto byte : tiff)
    {
        jpeg.U8({static_cast<int>(byte)});
    }

    AppendSof(jpeg, 0xC0, width, height);
    jpeg.U8({0xFF, 0xD9}); // EOI
    return jpeg.Bytes();
}

std::vector<ExyokiOffice::Byte> BuildTiff(ExyokiOffice::UInt32 width,
                                          ExyokiOffice::UInt32 height,
                                          ExyokiOffice::UInt32 dpi)
{
    return BuildTiffBlock(width, height, dpi);
}

std::vector<ExyokiOffice::Byte> BuildEmfFrame(ExyokiOffice::Int32 left,
                                              ExyokiOffice::Int32 top,
                                              ExyokiOffice::Int32 right,
                                              ExyokiOffice::Int32 bottom)
{
    ImageBytes emf;
    emf.Le32(1);                               // iType: EMR_HEADER
    emf.Le32(88);                              // nSize
    emf.Le32(0).Le32(0).Le32(1000).Le32(1000); // rclBounds, device units
    emf.Le32(static_cast<ExyokiOffice::UInt32>(left));
    emf.Le32(static_cast<ExyokiOffice::UInt32>(top));
    emf.Le32(static_cast<ExyokiOffice::UInt32>(right));
    emf.Le32(static_cast<ExyokiOffice::UInt32>(bottom));
    emf.Ascii(" EMF");    // dSignature, at offset 40
    emf.Le32(0x00010000); // nVersion
    emf.Zeros(88 - 48);
    return emf.Bytes();
}

std::vector<ExyokiOffice::Byte> BuildEmf(ExyokiOffice::Int32 frameWidth001mm,
                                         ExyokiOffice::Int32 frameHeight001mm)
{
    return BuildEmfFrame(0, 0, frameWidth001mm, frameHeight001mm);
}

std::vector<ExyokiOffice::Byte> BuildPlaceableWmf(ExyokiOffice::Int32 width,
                                                  ExyokiOffice::Int32 height,
                                                  ExyokiOffice::UInt16 unitsPerInch)
{
    ImageBytes wmf;
    wmf.Le32(0x9AC6CDD7); // Aldus placeable key
    wmf.Le16(0);          // hmf, unused
    wmf.Le16(0).Le16(0);  // bounding box left, top
    wmf.Le16(static_cast<ExyokiOffice::UInt32>(width));
    wmf.Le16(static_cast<ExyokiOffice::UInt32>(height));
    wmf.Le16(unitsPerInch);
    wmf.Le32(0); // reserved
    wmf.Le16(0); // checksum, not verified by the detector
    return wmf.Bytes();
}

} // namespace ExyokiOfficeTests
