// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

#include "ExyokiOffice/Excel/ExcelAddress.hpp"
#include "ExyokiOffice/Export.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ExyokiOffice::Excel
{

/** @brief Kind of worksheet-grid transformation applied to formula references.
 */
enum class ReferenceTransformKind
{
    InsertRows,
    DeleteRows,
    InsertColumns,
    DeleteColumns,
    MoveRange
};

/**
 * @brief Describes one coordinate transformation for A1 formula references.
 *
 * Insert and delete transformations use @ref index and @ref count. MoveRange
 * uses @ref source and @ref destinationTopLeft; its index and count fields are
 * ignored. All coordinates are one-based and must fit the Excel worksheet grid.
 */
struct EXYOKIOFFICE_EXPORT FormulaReferenceTransform
{
    ReferenceTransformKind Kind = ReferenceTransformKind::InsertRows;
    UInt32 Index = 0;
    UInt32 Count = 0;
    CellRange Source;
    CellAddress DestinationTopLeft;

    /** @brief Creates a row insertion transformation. */
    static FormulaReferenceTransform InsertRows(UInt32 beforeRow,
                                                UInt32 count = 1);
    /** @brief Creates a row deletion transformation. */
    static FormulaReferenceTransform DeleteRows(UInt32 firstRow,
                                                UInt32 count = 1);
    /** @brief Creates a column insertion transformation. */
    static FormulaReferenceTransform InsertColumns(UInt32 beforeColumn,
                                                   UInt32 count = 1);
    /** @brief Creates a column deletion transformation. */
    static FormulaReferenceTransform DeleteColumns(UInt32 firstColumn,
                                                   UInt32 count = 1);
    /** @brief Creates a rectangular move transformation. */
    static FormulaReferenceTransform MoveRange(CellRange source,
                                               CellAddress destinationTopLeft);
};

/** @brief Severity-independent diagnostic produced while examining a formula.
 */
struct EXYOKIOFFICE_EXPORT FormulaReferenceDiagnostic
{
    /** Zero-based byte offset of the preserved token in the input formula. */
    Size Offset = 0;
    /** Token text, including its workbook and sheet qualifier. */
    std::string Token;
    /** Human-readable English explanation suitable for logs. */
    std::string Message;
};

/** @brief Result of an A1 formula reference rewrite. */
struct EXYOKIOFFICE_EXPORT FormulaReferenceRewriteResult
{
    /** Rewritten formula text, without adding or removing a leading equals sign.
     */
    std::string Formula;
    /** Number of scalar or range tokens whose text changed. */
    Size RewrittenReferenceCount = 0;
    /** Diagnostics for recognized external workbook references left unchanged. */
    std::vector<FormulaReferenceDiagnostic> Diagnostics;
    /** False when the transform is invalid or would move a reference outside the
     * grid. */
    bool Succeeded = true;
    /** English error detail when @ref succeeded is false. */
    std::string ErrorMessage;

    /** @brief Provides convenient success testing in conditional statements. */
    [[nodiscard]] explicit operator bool() const noexcept { return Succeeded; }
};

/**
 * @brief Rewrites local A1 references after worksheet structural changes.
 *
 * The rewriter is lexical and does not evaluate formulas. It recognizes scalar
 * references and rectangular ranges with relative, mixed, or absolute markers.
 * Unqualified references and references qualified with `localSheetName` are
 * transformed. Other sheet-qualified references, defined names, structured
 * table references, R1C1 expressions, and text inside string literals remain
 * byte-for-byte unchanged. External workbook references such as
 * `[Book.xlsx]Sheet1!A1` are preserved and reported in @ref
 * FormulaReferenceRewriteResult::diagnostics.
 *
 * Sheet names may be quoted with apostrophes; doubled apostrophes inside quoted
 * names are decoded for local-sheet comparison and their original spelling is
 * retained. A deleted scalar reference becomes `#REF!`. A partially deleted
 * range contracts, while a fully deleted range becomes `#REF!`.
 */
class EXYOKIOFFICE_EXPORT FormulaReferenceRewriter final
{
public:
    FormulaReferenceRewriter() = delete;

    /**
     * @brief Applies a grid transformation to references in an A1 formula.
     * @param formula Formula text, with or without a leading equals sign.
     * @param localSheetName Name of the worksheet being transformed. An empty
     * value still permits unqualified references but matches no qualifier.
     * @param transform Valid insertion, deletion, or move transformation.
     * @return Rewritten text, reference count, diagnostics, and status.
     */
    static FormulaReferenceRewriteResult
    RewriteA1(std::string_view formula, std::string_view localSheetName,
              const FormulaReferenceTransform& transform);
};

} // namespace ExyokiOffice::Excel
