// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

#include "ExyokiOffice/Excel/ExcelAddress.hpp"
#include "ExyokiOffice/Export.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ExyokiOffice::Excel
{

/** @brief Describes a hyperlink attached to one worksheet cell. */
struct EXYOKIOFFICE_EXPORT ExcelHyperlink
{
    /** @brief Cell containing the hyperlink. */
    CellAddress Address;

    /** @brief External URI. Empty for an internal workbook location. */
    std::string Target;

    /** @brief Workbook location such as `Sheet2!A1`. */
    std::string Location;

    /** @brief Optional text shown instead of the cell value. */
    std::string Display;

    /** @brief Optional screen tip displayed by spreadsheet applications. */
    std::string Tooltip;
};

/** @brief Describes a traditional SpreadsheetML cell comment. */
struct EXYOKIOFFICE_EXPORT ExcelComment
{
    /** @brief Cell annotated by the comment. */
    CellAddress Address;

    /** @brief Comment author stored in the worksheet author table. */
    std::string Author;

    /** @brief Plain comment text. */
    std::string Text;
};

/** @brief Describes a modern threaded cell comment. */
struct EXYOKIOFFICE_EXPORT ExcelThreadedComment
{
    /** @brief Stable comment identifier in UUID form. Generated when empty. */
    std::string Id;

    /** @brief Cell annotated by the thread entry. */
    CellAddress Address;

    /** @brief Stable person identifier in UUID form. Generated when empty. */
    std::string PersonId;

    /** @brief Human-readable person name. */
    std::string PersonName;

    /** @brief Optional e-mail address associated with the person. */
    std::string PersonEmail;

    /** @brief Comment text. */
    std::string Text;

    /** @brief UTC ISO-8601 timestamp. The current time is used when empty. */
    std::string DateTime;

    /** @brief Parent comment identifier for a reply, or empty for a root comment. */
    std::string ParentId;
};

/** @brief Supported image payload formats for worksheet drawings. */
enum class ExcelImageFormat
{
    Png,
    Jpeg,
    Gif,
    Bmp,
    Tiff
};

/** @brief Describes an image anchored to a worksheet cell rectangle. */
struct EXYOKIOFFICE_EXPORT ExcelWorksheetImage
{
    /** @brief Stable drawing object identifier. Zero requests automatic allocation. */
    UInt32 Id = 0;

    /** @brief Human-readable non-visual drawing name. */
    std::string Name;

    /** @brief Alternative text for accessibility. */
    std::string Description;

    /** @brief Top-left anchor cell. */
    CellAddress From;

    /** @brief Bottom-right anchor cell, exclusive in drawing coordinates. */
    CellAddress To;

    /** @brief Encoded image bytes. */
    std::vector<Byte> Data;

    /** @brief Image encoding and package content type. */
    ExcelImageFormat Format = ExcelImageFormat::Png;
};

} // namespace ExyokiOffice::Excel
