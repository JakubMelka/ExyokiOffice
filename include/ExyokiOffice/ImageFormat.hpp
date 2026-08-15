// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

#include "ExyokiOffice/Export.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace ExyokiOffice
{

/** @brief Detected raster-image format, dimensions, and physical resolution. */
struct ImageFormatInfo
{
    std::string ContentType;   ///< IANA media type such as `image/png`.
    std::string Extension;     ///< Conventional extension including the leading dot.
    UInt32 PixelWidth = 0;     ///< Native width in pixels.
    UInt32 PixelHeight = 0;    ///< Native height in pixels.
    Real HorizontalDpi = 96.0; ///< Horizontal resolution, defaulting to 96 DPI.
    Real VerticalDpi = 96.0;   ///< Vertical resolution, defaulting to 96 DPI.
    bool operator==(const ImageFormatInfo&) const = default;
};

/**
 * @brief Detects PNG, JPEG, GIF, or BMP metadata from the payload signature.
 * @param data Image bytes; the buffer is only inspected and is never retained.
 * @return Format metadata, or `std::nullopt` for malformed or unsupported data.
 */
EXYOKIOFFICE_EXPORT std::optional<ImageFormatInfo> DetectImageFormat(std::span<const Byte> data);

} // namespace ExyokiOffice
