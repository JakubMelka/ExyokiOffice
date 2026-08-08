// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

#include "Results.hpp"

#include "ExyokiOffice/Excel/ExcelDocument.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <vector>

namespace ExyokiOffice::Mcp
{

/**
 * @brief A1 addressing and cell-value marshalling for the Excel toolset.
 *
 * Worksheets are addressed by name (case-insensitive) or by 1-based index, and
 * cells and ranges exclusively in A1 notation. Keeping that translation in one
 * place is what lets every Excel tool report the same `sheet_not_found` and
 * `range_invalid` errors with the same hints.
 */
class ExcelAddressing
{
public:
    /// Resolves the `sheet` argument; fills @p failure with `sheet_not_found`.
    [[nodiscard]] static Excel::Worksheet::Ptr FindSheet(Excel::ExcelDocumentEditor& editor,
                                                         const nlohmann::json& arguments, ToolOutcome& failure);

    /**
     * @brief Resolves any worksheet-naming argument; fills @p failure on miss.
     *
     * Used for `sheet` and for the pivot table's `source_sheet` and
     * `target_sheet`, so every one of them accepts the same two forms.
     * A missing member falls back to the first worksheet.
     */
    [[nodiscard]] static Excel::Worksheet::Ptr FindSheetMember(Excel::ExcelDocumentEditor& editor,
                                                               const nlohmann::json& arguments,
                                                               const std::string& member, ToolOutcome& failure);

    /**
     * @brief Resolves a worksheet from a raw argument value.
     *
     * A JSON integer is a 1-based position and never a name; a JSON string is
     * a name first and a position second. Collapsing both to text would make
     * the integer 2 find a worksheet called "2" instead of the second one.
     */
    [[nodiscard]] static Excel::Worksheet::Ptr FindSheetValue(Excel::ExcelDocumentEditor& editor,
                                                              const nlohmann::json& value);

    /// Resolves a worksheet by name or 1-based index text.
    [[nodiscard]] static Excel::Worksheet::Ptr FindSheetByToken(Excel::ExcelDocumentEditor& editor,
                                                                const std::string& token);

    /// Resolves a worksheet by its 1-based position; nullptr when out of range.
    [[nodiscard]] static Excel::Worksheet::Ptr FindSheetByIndex(Excel::ExcelDocumentEditor& editor, Size index);

    /**
     * @brief Resolves a worksheet by name only, case-insensitively as Excel does.
     *
     * Use this wherever a name is a name and never an index — a uniqueness
     * check, for instance, which must not reject a sheet literally called "2"
     * because a second worksheet exists.
     */
    [[nodiscard]] static Excel::Worksheet::Ptr FindSheetByName(Excel::ExcelDocumentEditor& editor,
                                                               const std::string& name);

    /**
     * @brief Reads a worksheet-naming argument that may be a name or an index.
     *
     * Every sheet-naming parameter accepts both a string and an integer, so the
     * conversion to the token FindSheetByToken() takes lives in one place.
     * A missing member, or one of any other type, yields an empty token.
     */
    [[nodiscard]] static std::string SheetToken(const nlohmann::json& arguments, const std::string& name);

    /// Parses an A1 cell address; fills @p failure with `range_invalid`.
    [[nodiscard]] static std::optional<Excel::CellAddress> ParseCell(const std::string& text, ToolOutcome& failure);

    /// Parses an A1 range; a single cell is accepted as a one-cell range.
    [[nodiscard]] static std::optional<Excel::CellRange> ParseRange(const std::string& text, ToolOutcome& failure);

    /// The range actually holding data, or std::nullopt for an empty sheet.
    [[nodiscard]] static std::optional<Excel::CellRange> UsedRange(const Excel::Worksheet& sheet);

    /// Converts one JSON cell value into the editor's cell value type.
    [[nodiscard]] static bool ParseCellValue(const nlohmann::json& value, Excel::ExcelCellValue& result,
                                             ToolOutcome& failure);

    /// Renders a stored cell value as JSON, resolving shared strings.
    [[nodiscard]] static nlohmann::json CellValueToJson(const Excel::ExcelCellValue& value,
                                                        const Excel::SharedStringTableService& sharedStrings);

    /// Plain text of a stored cell value, resolving shared strings.
    [[nodiscard]] static std::string CellValueToText(const Excel::ExcelCellValue& value,
                                                     const Excel::SharedStringTableService& sharedStrings);

    /// Renders a cell value kind as its tool-facing token.
    [[nodiscard]] static std::string CellKindToken(Excel::CellValueKind kind);

    /// Schema of one cell value accepted by the writing tools.
    [[nodiscard]] static nlohmann::json CellValueSchema();

    /// Parses a column band such as "B" or "B:D" into inclusive 1-based indices.
    [[nodiscard]] static bool ParseColumnBand(const std::string& text, UInt32& first, UInt32& last);

    /// Parses a row band such as "2" or "2:5" into inclusive 1-based indices.
    [[nodiscard]] static bool ParseRowBand(const std::string& text, UInt32& first, UInt32& last);

    /// Case-insensitive comparison used for sheet names.
    [[nodiscard]] static bool EqualsIgnoringCase(std::string_view left, std::string_view right);
};

} // namespace ExyokiOffice::Mcp
