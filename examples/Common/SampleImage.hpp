// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

// Synthetic image bytes shared by the samples. The examples generate their
// pictures instead of shipping asset files, so every sample can be built and
// run straight from the repository. Nothing here is part of the library API.
#pragma once

#include "ExyokiOffice/StandardTypes.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace ExyokiOfficeExamples
{

/**
 * @brief Builds an uncompressed 24bpp BMP filled with a two-axis color gradient.
 *
 * The result is accepted by every ExyokiOffice image entry point that sniffs
 * the format from the bytes (`AddImageFromData`, `AddPictureFromData`,
 * `Worksheet::AddImage`).
 */
inline std::vector<ExyokiOffice::Byte> BuildGradientBmp(ExyokiOffice::Int32 width, ExyokiOffice::Int32 height)
{
    const ExyokiOffice::Int32 rowStride = ((width * 3 + 3) / 4) * 4;
    const ExyokiOffice::UInt32 pixelBytes = static_cast<ExyokiOffice::UInt32>(rowStride * height);
    const ExyokiOffice::UInt32 fileSize = 54 + pixelBytes;

    std::vector<ExyokiOffice::Byte> data(fileSize, 0);
    const auto writeU32 = [&data](ExyokiOffice::Size offset, ExyokiOffice::UInt32 value)
    {
        data[offset] = static_cast<ExyokiOffice::UInt8>(value & 0xFF);
        data[offset + 1] = static_cast<ExyokiOffice::UInt8>((value >> 8) & 0xFF);
        data[offset + 2] = static_cast<ExyokiOffice::UInt8>((value >> 16) & 0xFF);
        data[offset + 3] = static_cast<ExyokiOffice::UInt8>((value >> 24) & 0xFF);
    };

    data[0] = 'B';
    data[1] = 'M';
    writeU32(2, fileSize);
    writeU32(10, 54); // pixel data offset
    writeU32(14, 40); // BITMAPINFOHEADER size
    writeU32(18, static_cast<ExyokiOffice::UInt32>(width));
    writeU32(22, static_cast<ExyokiOffice::UInt32>(height));
    data[26] = 1;  // planes
    data[28] = 24; // bits per pixel
    writeU32(34, pixelBytes);

    for (ExyokiOffice::Int32 y = 0; y < height; ++y)
    {
        for (ExyokiOffice::Int32 x = 0; x < width; ++x)
        {
            const ExyokiOffice::Size offset = 54 + static_cast<ExyokiOffice::Size>(y) * rowStride + static_cast<ExyokiOffice::Size>(x) * 3;
            data[offset] = static_cast<ExyokiOffice::UInt8>(255 - (x * 255) / width); // blue
            data[offset + 1] = static_cast<ExyokiOffice::UInt8>((y * 255) / height);  // green
            data[offset + 2] = static_cast<ExyokiOffice::UInt8>((x * 255) / width);   // red
        }
    }
    return data;
}

} // namespace ExyokiOfficeExamples
