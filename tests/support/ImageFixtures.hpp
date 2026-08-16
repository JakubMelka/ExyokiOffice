// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

// Hand-assembled raster-image fixtures shared by the layers that exercise
// DetectImageFormat and the image editors. The builders emit minimal headers
// byte by byte - fake CRCs, no scan data - because what the tests pin is
// signature parsing, and a real codec payload would only obscure which field
// a case exercises. One definition serves every layer, so a change to the
// detector's expectations is made here once.

#include "ExyokiOffice/StandardTypes.hpp"

#include <functional>
#include <initializer_list>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace ExyokiOfficeTests
{

/** Incremental byte-buffer builder for hand-assembled image payloads. */
class ImageBytes final
{
public:
    ImageBytes() = default;

    ImageBytes& U8(std::initializer_list<int> values)
    {
        for (const int value : values)
        {
            m_data.push_back(static_cast<ExyokiOffice::Byte>(value));
        }
        return *this;
    }

    ImageBytes& Ascii(std::string_view text)
    {
        for (const char c : text)
        {
            m_data.push_back(static_cast<ExyokiOffice::Byte>(c));
        }
        return *this;
    }

    ImageBytes& Be16(ExyokiOffice::UInt32 value)
    {
        return U8({static_cast<int>((value >> 8) & 0xFF), static_cast<int>(value & 0xFF)});
    }

    ImageBytes& Be32(ExyokiOffice::UInt32 value)
    {
        return U8({static_cast<int>((value >> 24) & 0xFF), static_cast<int>((value >> 16) & 0xFF),
                   static_cast<int>((value >> 8) & 0xFF), static_cast<int>(value & 0xFF)});
    }

    ImageBytes& Le16(ExyokiOffice::UInt32 value)
    {
        return U8({static_cast<int>(value & 0xFF), static_cast<int>((value >> 8) & 0xFF)});
    }

    ImageBytes& Le32(ExyokiOffice::UInt32 value)
    {
        return U8({static_cast<int>(value & 0xFF), static_cast<int>((value >> 8) & 0xFF),
                   static_cast<int>((value >> 16) & 0xFF), static_cast<int>((value >> 24) & 0xFF)});
    }

    ImageBytes& Zeros(ExyokiOffice::Size count)
    {
        m_data.insert(m_data.end(), count, ExyokiOffice::Byte{0});
        return *this;
    }

    [[nodiscard]] std::span<const ExyokiOffice::Byte> Span() const noexcept { return m_data; }

    [[nodiscard]] std::vector<ExyokiOffice::Byte> Bytes() const { return m_data; }

private:
    std::vector<ExyokiOffice::Byte> m_data;
};

/** PNG signature plus an IHDR chunk carrying the given dimensions. */
ImageBytes PngHeader(ExyokiOffice::UInt32 width, ExyokiOffice::UInt32 height);

/** Appends one PNG chunk with an arbitrary payload written by @p payload. */
void AppendPngChunk(ImageBytes& png,
                    std::string_view type,
                    ExyokiOffice::UInt32 length,
                    const std::function<void(ImageBytes&)>& payload);

/** JFIF APP0 segment with the given density fields. */
void AppendJfifApp0(ImageBytes& jpeg,
                    int units,
                    ExyokiOffice::UInt32 xDensity,
                    ExyokiOffice::UInt32 yDensity);

/** SOF segment for @p marker carrying the given dimensions. */
void AppendSof(ImageBytes& jpeg,
               int marker,
               ExyokiOffice::UInt32 width,
               ExyokiOffice::UInt32 height);

/** BMP file header plus BITMAPINFOHEADER; height may be negative (top-down). */
ImageBytes BmpHeader(ExyokiOffice::UInt32 width,
                     ExyokiOffice::Int32 height,
                     ExyokiOffice::Int32 pixelsPerMeterX,
                     ExyokiOffice::Int32 pixelsPerMeterY);

/**
 * Complete minimal PNG payload: signature, IHDR, optional pHYs density in
 * pixels per meter, and IEND.
 */
std::vector<ExyokiOffice::Byte> BuildPng(
    ExyokiOffice::UInt32 width,
    ExyokiOffice::UInt32 height,
    std::optional<std::pair<ExyokiOffice::UInt32, ExyokiOffice::UInt32>> pixelsPerMeter =
        std::nullopt);

/**
 * Complete minimal JPEG payload: SOI, JFIF APP0 (with the given DPI when
 * nonzero), SOF0, and EOI. No entropy-coded scan data.
 */
std::vector<ExyokiOffice::Byte> BuildJpeg(ExyokiOffice::UInt16 width,
                                          ExyokiOffice::UInt16 height,
                                          ExyokiOffice::UInt16 dpi = 0);

/** Complete minimal GIF89a payload. */
std::vector<ExyokiOffice::Byte> BuildGif(ExyokiOffice::UInt16 width, ExyokiOffice::UInt16 height);

/** Complete minimal BMP payload; densities are pixels per meter. */
std::vector<ExyokiOffice::Byte> BuildBmp(ExyokiOffice::Int32 width,
                                         ExyokiOffice::Int32 height,
                                         ExyokiOffice::Int32 pixelsPerMeterX = 0,
                                         ExyokiOffice::Int32 pixelsPerMeterY = 0);

/**
 * Complete minimal JPEG payload whose resolution is stated by an Exif APP1
 * segment rather than by JFIF - which is how every camera and phone writes it.
 * The JFIF segment is present and says "no units", exactly as those files do.
 */
std::vector<ExyokiOffice::Byte> BuildJpegWithExifDpi(ExyokiOffice::UInt16 width,
                                                     ExyokiOffice::UInt16 height,
                                                     ExyokiOffice::UInt32 dpi);

/** Minimal little-endian TIFF: header, one IFD with dimensions and resolution. */
std::vector<ExyokiOffice::Byte> BuildTiff(ExyokiOffice::UInt32 width,
                                          ExyokiOffice::UInt32 height,
                                          ExyokiOffice::UInt32 dpi = 0);

/** Minimal EMR_HEADER; the frame is given in hundredths of a millimetre. */
std::vector<ExyokiOffice::Byte> BuildEmf(ExyokiOffice::Int32 frameWidth001mm,
                                         ExyokiOffice::Int32 frameHeight001mm);

/** Placeable (Aldus) WMF header; the bounding box is in @p unitsPerInch units. */
std::vector<ExyokiOffice::Byte> BuildPlaceableWmf(ExyokiOffice::Int32 width,
                                                  ExyokiOffice::Int32 height,
                                                  ExyokiOffice::UInt16 unitsPerInch);

} // namespace ExyokiOfficeTests
