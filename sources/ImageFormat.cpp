// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "ExyokiOffice/ImageFormat.hpp"

#include "ExyokiOffice/Word/WordDocument.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

namespace ExyokiOffice
{

std::optional<ImageFormatInfo> DetectImageFormat(std::span<const Byte> data)
{
    return Word::DetectImageFormat(data);
}

} // namespace ExyokiOffice
