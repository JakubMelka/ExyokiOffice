// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

#include "ExyokiOffice/Excel/ExcelAddress.hpp"
#include "ExyokiOffice/DocumentEditTransaction.hpp"
#include "ExyokiOffice/Excel/ExcelCellValue.hpp"
#include "ExyokiOffice/Excel/ExcelDataValidation.hpp"
#include "ExyokiOffice/Excel/ExcelConditionalFormatting.hpp"
#include "ExyokiOffice/Excel/ExcelWorksheetLayout.hpp"
#include "ExyokiOffice/Excel/ExcelWorksheetContent.hpp"
#include "ExyokiOffice/Excel/ExcelChart.hpp"
#include "ExyokiOffice/Excel/ExcelPivotTable.hpp"
#include "ExyokiOffice/Excel/ExcelSlicer.hpp"
#include "ExyokiOffice/Excel/ExcelTable.hpp"
#include "ExyokiOffice/Excel/ExcelStyle.hpp"
#include "ExyokiOffice/MeasuringUnits.hpp"
#include "ExyokiOffice/Packaging/SpreadsheetDocument.hpp"
#include "ExyokiOffice/Security/ExternalResources.hpp"
#include "ExyokiOffice/ThemeService.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <cstdint>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ExyokiOffice::Excel
{

/**
 * @brief Low-level Excel package type used by the high-level Excel API.
 *
 * This alias mirrors the Word namespace pattern and lets callers work inside
 * ExyokiOffice::Excel without spelling the Packaging namespace.
 */
using ExcelDocument = ExyokiOffice::Packaging::ExcelDocument;
/**
 * @brief Spreadsheet package flavor used when creating or converting workbooks.
 */
using SpreadsheetDocumentType = ExyokiOffice::Packaging::SpreadsheetDocumentType;

class ExcelDocumentEditor;
class SharedStringTableService;
class StyleRepository;

/**
 * @brief Rectangular matrix of typed Excel cell values.
 *
 * The outer vector contains rows and each inner vector contains columns. Range
 * APIs require a non-jagged matrix whose dimensions exactly match the target
 * CellRange. A blank value clears the corresponding sparse worksheet cell.
 */
using ExcelCellMatrix = std::vector<std::vector<ExcelCellValue>>;

/**
 * @brief Error code reported by a worksheet range operation.
 *
 * Range APIs validate every address and matrix dimension before mutating the
 * worksheet. Callers can use this code for programmatic error handling and the
 * accompanying message for diagnostics.
 */
enum class RangeOperationError
{
    /** The operation completed successfully. */
    None,
    /** The worksheet wrapper is detached or has no usable worksheet root. */
    InvalidWorksheet,
    /** A supplied CellRange or CellAddress is invalid. */
    InvalidAddress,
    /** Matrix rows are jagged or do not exactly match the target range. */
    DimensionMismatch,
    /** The destination rectangle would extend beyond Excel's worksheet grid. */
    DestinationOutOfBounds,
    /** A requested insertion/deletion count is zero or exceeds the grid. */
    InvalidCount,
    /** A stored cell or range reference could not be rewritten safely. */
    ReferenceUpdateFailed,
    /** The requested merge intersects an existing merged-cell range. */
    OverlappingRange,
    /** No merged-cell registry entry exactly matches the requested range. */
    RangeNotFound,
    /** A style definition violates SpreadsheetML value constraints. */
    InvalidStyle,
    /** A requested cell-XF style index does not exist. */
    StyleNotFound,
    /** A low-level cell mutation failed; any completed mutations were rolled back. */
    WriteFailed
};

/**
 * @brief Controls formula rewriting during structural worksheet edits.
 *
 * Structural edits always update cell addresses and worksheet-owned range
 * metadata. This policy controls only formula text. ExyokiOffice does not
 * evaluate formulas.
 */
enum class FormulaReferenceUpdatePolicy
{
    /** Preserve formula text byte-for-byte. */
    PreserveFormulaText,
    /**
     * Rewrite unqualified local A1 cell and range references.
     *
     * Absolute markers (`$`) are preserved. References inside string literals,
     * sheet-qualified references, external references, structured references,
     * names, and R1C1 expressions are left unchanged. A single-cell reference
     * deleted by the operation becomes `#REF!`; partially deleted ranges shrink
     * and fully deleted ranges become `#REF!`.
     */
    UpdateUnqualifiedA1References
};

/**
 * @brief Structured status returned by worksheet range mutations.
 */
struct EXYOKIOFFICE_EXPORT RangeOperationResult
{
    RangeOperationError Error = RangeOperationError::None;
    std::string Message;
    Size AffectedCellCount = 0;

    /** @brief Returns true when the operation completed successfully. */
    [[nodiscard]] bool Succeeded() const noexcept { return Error == RangeOperationError::None; }
    /** @brief Provides convenient success testing in conditional statements. */
    [[nodiscard]] explicit operator bool() const noexcept { return Succeeded(); }
};

/**
 * @brief Result of reading a rectangular worksheet range.
 */
struct EXYOKIOFFICE_EXPORT RangeReadResult
{
    RangeOperationResult Status;
    ExcelCellMatrix Values;

    /** @brief Returns true when the complete matrix was read successfully. */
    [[nodiscard]] bool Succeeded() const noexcept { return Status.Succeeded(); }
    /** @brief Provides convenient success testing in conditional statements. */
    [[nodiscard]] explicit operator bool() const noexcept { return Succeeded(); }
};

/** @brief Result of registering or deduplicating a cell style. */
struct EXYOKIOFFICE_EXPORT StyleRegistrationResult
{
    RangeOperationResult Status;
    UInt32 StyleIndex = 0;

    /** @brief Returns true when a valid style index was returned. */
    [[nodiscard]] bool Succeeded() const noexcept { return Status.Succeeded(); }
    /** @brief Provides convenient success testing in conditional statements. */
    [[nodiscard]] explicit operator bool() const noexcept { return Succeeded(); }
};

/** @brief Page orientation used when a worksheet is printed. */
enum class PageOrientation
{
    Portrait,
    Landscape
};

/** @brief Standard Excel paper-size codes used by `x:pageSetup/@paperSize`. */
enum class PaperSize : UInt32
{
    Letter = 1,
    LetterSmall = 2,
    Tabloid = 3,
    Ledger = 4,
    Legal = 5,
    Statement = 6,
    Executive = 7,
    A3 = 8,
    A4 = 9,
    A4Small = 10,
    A5 = 11,
    B4 = 12,
    B5 = 13,
    Folio = 14
};

/**
 * @brief Physical page margins for a worksheet.
 *
 * Values may use any @ref ExyokiOffice::MeasurementUnit. SpreadsheetML
 * stores margins in inches, so values are converted to inches on write and
 * returned in inches when read.
 */
struct EXYOKIOFFICE_EXPORT PageMargins
{
    ExyokiOffice::MeasuringUnits Left{0.7, ExyokiOffice::MeasurementUnit::Inch};
    ExyokiOffice::MeasuringUnits Right{0.7, ExyokiOffice::MeasurementUnit::Inch};
    ExyokiOffice::MeasuringUnits Top{0.75, ExyokiOffice::MeasurementUnit::Inch};
    ExyokiOffice::MeasuringUnits Bottom{0.75, ExyokiOffice::MeasurementUnit::Inch};
    ExyokiOffice::MeasuringUnits Header{0.3, ExyokiOffice::MeasurementUnit::Inch};
    ExyokiOffice::MeasuringUnits Footer{0.3, ExyokiOffice::MeasurementUnit::Inch};
};

/** @brief Worksheet printer and scaling settings. */
struct EXYOKIOFFICE_EXPORT PageSetup
{
    PageOrientation Orientation = PageOrientation::Portrait;
    // Qualified because the member and the enumeration share a name: inside the
    // class an unqualified `PaperSize` would first mean the type and then the
    // member, which GCC rejects under -Wchanges-meaning.
    std::optional<ExyokiOffice::Excel::PaperSize> PaperSize;
    std::optional<UInt32> Scale;
    std::optional<UInt32> FitToWidth;
    std::optional<UInt32> FitToHeight;
};

/** @brief Optional worksheet print decorations and alignment settings. */
struct EXYOKIOFFICE_EXPORT PrintOptions
{
    bool HorizontalCentered = false;
    bool VerticalCentered = false;
    bool Headings = false;
    bool GridLines = false;
};

/** @brief Header and footer strings using Excel's header/footer formatting codes. */
struct EXYOKIOFFICE_EXPORT HeaderFooter
{
    std::string OddHeader;
    std::string OddFooter;
    std::string EvenHeader;
    std::string EvenFooter;
    std::string FirstHeader;
    std::string FirstFooter;
    bool DifferentOddEven = false;
    bool DifferentFirst = false;
};

/** @brief Rows and columns repeated at the top and left of every printed page. */
struct EXYOKIOFFICE_EXPORT PrintTitles
{
    std::optional<std::pair<UInt32, UInt32>> Rows;
    std::optional<std::pair<UInt32, UInt32>> Columns;
};

/**
 * @brief Operations that remain available while a worksheet is protected.
 *
 * SpreadsheetML stores inverse "locked" flags for these capabilities. This
 * high-level model deliberately uses positive `allow...` names so `true`
 * consistently means that the workbook user may perform the operation.
 * Selecting locked and unlocked cells is allowed by default, matching Excel.
 */
struct EXYOKIOFFICE_EXPORT SheetProtectionOptions
{
    /** Permit changing cell-format records. */
    bool AllowFormatCells = false;
    /** Permit changing column widths and column formatting. */
    bool AllowFormatColumns = false;
    /** Permit changing row heights and row formatting. */
    bool AllowFormatRows = false;
    /** Permit inserting worksheet columns. */
    bool AllowInsertColumns = false;
    /** Permit inserting worksheet rows. */
    bool AllowInsertRows = false;
    /** Permit inserting hyperlinks. */
    bool AllowInsertHyperlinks = false;
    /** Permit deleting columns whose cells are not locked. */
    bool AllowDeleteColumns = false;
    /** Permit deleting rows whose cells are not locked. */
    bool AllowDeleteRows = false;
    /** Permit selecting cells whose effective style is locked. */
    bool AllowSelectLockedCells = true;
    /** Permit sorting ranges whose cells are not locked. */
    bool AllowSort = false;
    /** Permit changing existing auto-filter criteria. */
    bool AllowAutoFilter = false;
    /** Permit interacting with existing pivot tables. */
    bool AllowPivotTables = false;
    /** Permit selecting cells whose effective style is unlocked. */
    bool AllowSelectUnlockedCells = true;
    /** Protect drawing objects and controls from modification. */
    bool ProtectObjects = true;
    /** Protect worksheet scenarios from modification. */
    bool ProtectScenarios = true;
    bool operator==(const SheetProtectionOptions&) const = default;
};

/**
 * @brief Current worksheet-protection state.
 *
 * The optional password verifier is intentionally not exposed. `hasPassword`
 * only reports whether removal should require the same password.
 */
struct EXYOKIOFFICE_EXPORT SheetProtectionInfo
{
    /** Operations permitted while protection is active. */
    SheetProtectionOptions Options;
    /** True when a password verifier is stored on the worksheet. */
    bool HasPassword = false;
    bool operator==(const SheetProtectionInfo&) const = default;
};

/** @brief Error code returned by worksheet protection operations. */
enum class SheetProtectionError
{
    None,
    InvalidWorksheet,
    InvalidPassword,
    PasswordMismatch,
    WriteFailed
};

/** @brief Structured result of protecting or unprotecting a worksheet. */
struct EXYOKIOFFICE_EXPORT SheetProtectionResult
{
    SheetProtectionError Error = SheetProtectionError::None;
    std::string Message;

    /** @brief Returns true when the requested operation completed. */
    [[nodiscard]] bool Succeeded() const noexcept { return Error == SheetProtectionError::None; }
    /** @brief Provides convenient success testing in conditional statements. */
    [[nodiscard]] explicit operator bool() const noexcept { return Succeeded(); }
};

/**
 * @brief Workbook-level structural locks applied by workbook protection.
 *
 * Workbook protection is independent of worksheet protection: it restricts
 * changes to the workbook itself (adding, deleting, renaming, hiding, or
 * reordering sheets) rather than changes to cell content.
 */
struct EXYOKIOFFICE_EXPORT WorkbookProtectionOptions
{
    /** Prevent adding, deleting, renaming, hiding, moving, or copying sheets. */
    bool LockStructure = true;
    /** Prevent moving, resizing, and closing the workbook windows. */
    bool LockWindows = false;
    bool operator==(const WorkbookProtectionOptions&) const = default;
};

/**
 * @brief Current workbook-protection state.
 *
 * As with worksheet protection, the stored password verifier is deliberately
 * not exposed; `hasPassword` only reports whether removing protection requires
 * the same password.
 */
struct EXYOKIOFFICE_EXPORT WorkbookProtectionInfo
{
    /** Structural locks that are currently active. */
    WorkbookProtectionOptions Options;
    /** True when a workbook password verifier is stored. */
    bool HasPassword = false;
    bool operator==(const WorkbookProtectionInfo&) const = default;
};

/** @brief Error code returned by workbook protection operations. */
enum class WorkbookProtectionError
{
    None,
    InvalidWorkbook,
    InvalidPassword,
    PasswordMismatch,
    WriteFailed
};

/** @brief Structured result of protecting or unprotecting a workbook. */
struct EXYOKIOFFICE_EXPORT WorkbookProtectionResult
{
    WorkbookProtectionError Error = WorkbookProtectionError::None;
    std::string Message;

    /** @brief Returns true when the requested operation completed. */
    [[nodiscard]] bool Succeeded() const noexcept { return Error == WorkbookProtectionError::None; }
    /** @brief Provides convenient success testing in conditional statements. */
    [[nodiscard]] explicit operator bool() const noexcept { return Succeeded(); }
};

/**
 * @brief High-level wrapper for one worksheet inside an Excel workbook.
 *
 * Worksheet provides convenient cell-writing helpers over a generated
 * WorksheetPart and SpreadsheetML worksheet DOM. Rows and columns use Excel's
 * one-based coordinate system: row 1 is the first row and column 1 is column A.
 *
 * The wrapper is lightweight. It references the underlying package part, so
 * changes made through the wrapper immediately affect the owning document.
 */
class EXYOKIOFFICE_EXPORT Worksheet
{
public:
    using Ptr = std::shared_ptr<Worksheet>;

    /**
     * @brief Constructs an empty worksheet wrapper.
     *
     * The wrapper is invalid until it is associated with a WorksheetPart. Normal
     * callers obtain worksheet wrappers from ExcelDocumentEditor instead.
     */
    Worksheet() = default;
    /**
     * @brief Constructs a wrapper around an existing worksheet part.
     *
     * @param name Display name stored in the workbook's sheet list.
     * @param part Low-level worksheet part that owns the worksheet XML.
     */
    Worksheet(std::string name,
              std::shared_ptr<ExyokiOffice::Packaging::WorksheetPart> part,
              ExcelDocument::Ptr document = nullptr);

    /**
     * @brief Returns the worksheet display name.
     *
     * This is the name from the workbook sheet entry, such as "Sheet1".
     */
    std::string Name() const;
    /**
     * @brief Writes a shared string value to a worksheet cell.
     *
     * Missing row and cell elements are created automatically. The text is
     * inserted into the workbook shared string table with deduplication and
     * the cell stores the resulting shared-string index. Use SetCellValue()
     * with ExcelCellValue::InlineString() when an inline string is required.
     *
     * @param row One-based row index, valid range 1..1,048,576.
     * @param column One-based column index, valid range 1..16,384 (A..XFD).
     * @param value Text to store in the cell.
     * @return True when the cell was updated, false when the address or wrapper is invalid.
     */
    bool SetCellText(UInt32 row, UInt32 column, std::string_view value);
    /**
     * @brief Writes a shared string value to a worksheet cell.
     *
     * This overload accepts a validated CellAddress produced by the address
     * parser or by CellAddress::TryCreate().
     *
     * @param address Validated worksheet cell address.
     * @param value Text to store in the cell.
     * @return True when the cell was updated, false when the address or wrapper is invalid.
     */
    bool SetCellText(CellAddress address, std::string_view value);
    /**
     * @brief Writes a numeric value to a worksheet cell.
     *
     * Missing row and cell elements are created automatically. Existing cell
     * children are replaced with a numeric cell value.
     *
     * @param row One-based row index, valid range 1..1,048,576.
     * @param column One-based column index, valid range 1..16,384 (A..XFD).
     * @param value Numeric value to store in the cell.
     * @return True when the cell was updated, false when the address or wrapper is invalid.
     */
    bool SetCellNumber(UInt32 row, UInt32 column, Real value);
    /**
     * @brief Writes a numeric value to a worksheet cell.
     *
     * This overload accepts a validated CellAddress produced by the address
     * parser or by CellAddress::TryCreate().
     *
     * @param address Validated worksheet cell address.
     * @param value Numeric value to store in the cell.
     * @return True when the cell was updated, false when the address or wrapper is invalid.
     */
    bool SetCellNumber(CellAddress address, Real value);
    /**
     * @brief Writes a typed value to a worksheet cell.
     *
     * Missing row and cell elements are created automatically. Existing cell
     * children that represent value content (`f`, `v`, and `is`) are replaced.
     * Address, style, and metadata attributes are preserved.
     *
     * @param address Validated worksheet cell address.
     * @param value Typed value to write.
     * @return True when the cell was updated, false when the address or wrapper is invalid.
     */
    bool SetCellValue(CellAddress address, const ExcelCellValue& value);
    /**
     * @brief Writes a typed value to a worksheet cell.
     *
     * @param row One-based row index, valid range 1..1,048,576.
     * @param column One-based column index, valid range 1..16,384 (A..XFD).
     * @param value Typed value to write.
     * @return True when the cell was updated, false when the address or wrapper is invalid.
     */
    bool SetCellValue(UInt32 row, UInt32 column, const ExcelCellValue& value);
    /**
     * @brief Reads a typed value from a worksheet cell without modifying the worksheet.
     *
     * Missing cells are reported as blank. Existing cells are classified from
     * their formula, data type, inline string, and value elements. Shared-string
     * cells return the shared string table index; resolving the actual text is
     * handled by the shared strings service in a later roadmap item.
     *
     * @param address Validated worksheet cell address.
     * @return Parsed cell value, or std::nullopt when the address or wrapper is invalid.
     */
    std::optional<ExcelCellValue> GetCellValue(CellAddress address) const;
    /**
     * @brief Reads a typed value from a worksheet cell without modifying the worksheet.
     */
    std::optional<ExcelCellValue> GetCellValue(UInt32 row, UInt32 column) const;
    /**
     * @brief Tests whether a cell is physically stored in the worksheet XML.
     *
     * A false result does not mean that Excel cannot display a value at the
     * address; it means that no `c` element exists for the address. The lookup
     * examines only the requested sparse row and never materializes missing
     * rows or cells.
     *
     * @param address Validated worksheet cell address.
     * @return True when a cell element exists at the address.
     */
    [[nodiscard]] bool ContainsCell(CellAddress address) const;
    /**
     * @brief Tests whether a cell is physically stored in the worksheet XML.
     *
     * @return False when either coordinate is outside the Excel grid.
     */
    [[nodiscard]] bool ContainsCell(UInt32 row, UInt32 column) const;
    /**
     * @brief Removes a physically stored cell from the sparse worksheet.
     *
     * When the removed cell is the last cell in its row, the now-empty row is
     * removed as well. Other row metadata prevents removal of the row. Shared
     * string table entries are not compacted automatically; call
     * SharedStringTableService::Cleanup() when compaction is desired.
     *
     * @return True when a cell was removed, false for an invalid address or a
     * missing cell.
     */
    bool RemoveCell(CellAddress address);
    /** @brief Removes a stored cell addressed by one-based coordinates. */
    bool RemoveCell(UInt32 row, UInt32 column);
    /**
     * @brief Returns the number of physically stored cells.
     *
     * Runtime and memory use are proportional to stored XML cells, not to the
     * 1,048,576 by 16,384 worksheet grid.
     */
    Size StoredCellCount() const;
    /**
     * @brief Enumerates physically stored cell addresses in worksheet order.
     *
     * Invalid or unreferenced `c` elements from malformed input are excluded.
     * The returned vector contains one entry per valid stored cell and does not
     * include implicit blank grid positions.
     */
    std::vector<CellAddress> StoredCellAddresses() const;
    /**
     * @brief Reads a rectangular range as a dense row-major value matrix.
     *
     * Missing sparse cells are returned as ExcelCellValue::Blank(). Reading
     * never materializes rows or cells in the worksheet.
     *
     * @param range Valid, normalized rectangle to read.
     * @return Structured status and a matrix matching the range dimensions.
     */
    RangeReadResult GetRangeValues(CellRange range) const;
    /**
     * @brief Replaces all values in a rectangular range atomically.
     *
     * The matrix must be rectangular and exactly match the target range. All
     * dimensions are validated before mutation, so a mismatch returns
     * RangeOperationError::DimensionMismatch without changing any cell. Blank
     * matrix entries remove corresponding sparse cells. If a later low-level
     * write fails, earlier writes from this operation are rolled back.
     *
     * @param range Target rectangle.
     * @param values Row-major values with exactly range.RowCount() rows and
     * range.ColumnCount() columns.
     */
    RangeOperationResult SetRangeValues(CellRange range, const ExcelCellMatrix& values);
    /**
     * @brief Removes every physically stored cell in a range.
     *
     * Cells outside the rectangle are untouched. Empty rows are pruned using
     * the same metadata-preserving rules as RemoveCell().
     */
    RangeOperationResult ClearRange(CellRange range);
    /**
     * @brief Assigns one typed value to every cell in a range.
     *
     * A blank fill clears the range and retains sparse storage semantics.
     */
    RangeOperationResult FillRange(CellRange range, const ExcelCellValue& value);
    /**
     * @brief Copies range values to a destination rectangle on this worksheet.
     *
     * The destination has the same dimensions as source and is identified by
     * its top-left address. Source and destination may overlap because all
     * source values are snapshotted before any destination cell is changed.
     * Formulas are copied verbatim and are not reference-adjusted.
     */
    RangeOperationResult CopyRange(CellRange source, CellAddress destinationTopLeft);
    /**
     * @brief Moves range values to a destination rectangle on this worksheet.
     *
     * Source values are snapshotted, the source is cleared, and the snapshot is
     * written to the destination. Overlapping rectangles are supported. Any
     * destination area not covered by the overlap receives the source values;
     * source cells outside the destination are cleared. Local A1 references
     * that point into the source rectangle are retargeted to the corresponding
     * destination cells throughout the worksheet. Absolute and mixed markers
     * are preserved; external and other-sheet references remain unchanged.
     * R1C1 formula text is preserved because it requires formula-cell-relative
     * interpretation. Formula rewriting and value movement are atomic.
     */
    RangeOperationResult MoveRange(CellRange source, CellAddress destinationTopLeft);
    /**
     * @brief Inserts sparse worksheet rows before a one-based row index.
     *
     * Existing rows at or below `beforeRow` move down by `count`. Cell
     * references, worksheet dimensions, merged cells, filters, hyperlinks,
     * validation/conditional-format ranges, selections, protected ranges, and
     * formula-owned ranges are updated atomically. No empty row elements are
     * materialized. The operation fails without mutation if any shifted
     * reference would exceed row 1,048,576.
     *
     * @param beforeRow One-based insertion position.
     * @param count Number of rows to insert; must be greater than zero.
     * @param formulaPolicy Formula text rewrite policy.
     */
    RangeOperationResult InsertRows(UInt32 beforeRow,
                                    UInt32 count = 1,
                                    FormulaReferenceUpdatePolicy formulaPolicy =
                                        FormulaReferenceUpdatePolicy::UpdateUnqualifiedA1References);
    /**
     * @brief Deletes consecutive worksheet rows.
     *
     * Rows in `[firstRow, firstRow + count)` are removed and later rows move
     * upward. Range metadata is contracted or removed when fully deleted.
     * Formula updates follow `formulaPolicy`. The complete worksheet XML is
     * restored if any reference cannot be updated safely.
     */
    RangeOperationResult DeleteRows(UInt32 firstRow,
                                    UInt32 count = 1,
                                    FormulaReferenceUpdatePolicy formulaPolicy =
                                        FormulaReferenceUpdatePolicy::UpdateUnqualifiedA1References);
    /**
     * @brief Inserts sparse worksheet columns before a one-based column index.
     *
     * Existing cells at or to the right of `beforeColumn` move right. No cells
     * are created for the inserted columns. Cell/range/formula references are
     * updated atomically and overflow beyond XFD is rejected.
     */
    RangeOperationResult InsertColumns(UInt32 beforeColumn,
                                       UInt32 count = 1,
                                       FormulaReferenceUpdatePolicy formulaPolicy =
                                           FormulaReferenceUpdatePolicy::UpdateUnqualifiedA1References);
    /**
     * @brief Deletes consecutive worksheet columns.
     *
     * Cells in the deleted interval are removed, cells to its right move left,
     * and affected worksheet ranges are contracted or removed. Formula text is
     * updated according to `formulaPolicy`.
     */
    RangeOperationResult DeleteColumns(UInt32 firstColumn,
                                       UInt32 count = 1,
                                       FormulaReferenceUpdatePolicy formulaPolicy =
                                           FormulaReferenceUpdatePolicy::UpdateUnqualifiedA1References);
    /**
     * @brief Returns all valid merged-cell ranges registered by the worksheet.
     *
     * Ranges are returned in worksheet XML order. Malformed merge references
     * from externally produced packages are ignored by this convenience query;
     * merge mutations reject malformed registries with
     * RangeOperationError::ReferenceUpdateFailed.
     */
    std::vector<CellRange> MergedRanges() const;
    /**
     * @brief Finds the merged range containing a cell.
     *
     * @return The containing range, or std::nullopt when the cell is not merged
     * or the address is invalid.
     */
    std::optional<CellRange> MergedRangeAt(CellAddress address) const;
    /**
     * @brief Returns the active worksheet-protection settings.
     *
     * @return Protection information, or std::nullopt when the worksheet is
     * unprotected or this wrapper is detached.
     */
    std::optional<SheetProtectionInfo> GetProtection() const;
    /**
     * @brief Protects the worksheet with optional legacy Excel password verification.
     *
     * Protection controls editing in spreadsheet applications; it does not
     * encrypt worksheet data and is not a security boundary. The optional
     * password uses the interoperable legacy worksheet verifier and must
     * contain at most 15 bytes. Passing an empty password creates protection
     * that can be removed without a password.
     *
     * That verifier stores a 16-bit hash, which is the whole of what
     * SpreadsheetML records for a legacy sheet password: there are only 65 536
     * distinct values, so short colliding passwords are found by trying them
     * and the original is never recoverable from the file either. Unprotect()
     * accepts any password that hashes the same. Use this to keep a user from
     * editing a sheet by accident, never to keep data from someone who has the
     * file.
     *
     * Existing protection is replaced atomically.
     *
     * @param options Operations users may perform while protection is active.
     * @param password Optional case-sensitive password.
     * @return Structured success or validation/write error information.
     */
    SheetProtectionResult Protect(const SheetProtectionOptions& options = {},
                                  std::string_view password = {});
    /**
     * @brief Removes worksheet protection after verifying its password.
     *
     * An unprotected worksheet is treated as a successful no-op. A protected
     * worksheet without a password accepts only an empty password.
     *
     * @param password Password supplied when protection was created.
     * @return Structured success, password mismatch, or write error information.
     */
    SheetProtectionResult Unprotect(std::string_view password = {});
    /**
     * @brief Merges a rectangular range while preserving its top-left value.
     *
     * The range must contain at least two cells and must not intersect any
     * existing merged range. Every physically stored cell except the top-left
     * cell is removed, because SpreadsheetML stores the value of a merged area
     * only in its top-left cell. The merge registry and its count are updated
     * in schema order. On any failure the complete original worksheet XML is
     * restored.
     *
     * @return Success with the number of non-top-left cells physically removed,
     * or a structured error such as OverlappingRange.
     */
    RangeOperationResult MergeRange(CellRange range);
    /**
     * @brief Removes one exact merged-cell registry entry.
     *
     * Unmerging does not synthesize values for the formerly covered cells; the
     * preserved top-left value remains unchanged. A containing or partially
     * overlapping range is not sufficient—the registered range must match
     * exactly.
     */
    RangeOperationResult UnmergeRange(CellRange range);
    /**
     * @brief Writes a boolean value to a worksheet cell.
     */
    bool SetCellBoolean(CellAddress address, bool value);
    /**
     * @brief Writes an error literal to a worksheet cell.
     */
    bool SetCellError(CellAddress address, std::string_view errorText);
    /**
     * @brief Writes a date/time lexical value to a worksheet cell.
     *
     * The text is stored with SpreadsheetML cell type `d`.
     */
    bool SetCellDateTimeText(CellAddress address, std::string_view value);
    /**
     * @brief Writes a formula and optional cached result to a worksheet cell.
     *
     * The formula may be supplied with or without a leading `=`.
     */
    bool SetCellFormula(CellAddress address,
                        std::string_view formula,
                        FormulaCachedValueKind cachedKind = FormulaCachedValueKind::None,
                        std::string_view cachedText = {});
    /**
     * @brief Writes a complete normal, shared, or array formula model.
     *
     * The method stores the expression without evaluating it and preserves the
     * supplied cached result verbatim. Shared formulas require a shared index;
     * their anchor additionally requires a valid A1 group range containing the
     * target cell. Array formulas require a valid A1 result range containing
     * the target cell. Invalid models leave the worksheet and workbook
     * unchanged.
     *
     * Formula reference syntax is a workbook-wide SpreadsheetML property.
     * Consequently, writing a model whose referenceStyle differs from the
     * current setting updates the workbook calculation properties for every
     * formula in the workbook.
     *
     * @return true when the complete formula model was written.
     */
    bool SetCellFormula(CellAddress address, const CellFormulaValue& formula);
    /**
     * @brief Reads a complete formula model from a worksheet cell.
     *
     * Formula text, normal/shared/array kind, group index, range, array
     * recalculation flag, workbook A1/R1C1 mode, and cached result are retained.
     * ExyokiOffice never calculates or validates the expression language.
     *
     * @return The formula model, or std::nullopt when the cell does not exist,
     * is not a formula, or the worksheet is detached.
     */
    std::optional<CellFormulaValue> GetCellFormula(CellAddress address) const;

    /**
     * @brief Creates and attaches a worksheet table.
     *
     * The range includes the header row and must contain at least one row.
     * `columns.size()` must equal the range width. The display name is checked
     * case-insensitively across every table in the workbook. A stable table ID,
     * package relationship, worksheet `tableParts` entry, table columns, and an
     * auto-filter are created atomically.
     *
     * @return The attached table, or nullptr when validation or package
     * mutation fails.
     */
    ExcelTable::Ptr CreateTable(std::string_view name,
                                CellRange range,
                                const std::vector<ExcelTableColumn>& columns);
    /** @brief Returns all tables attached to this worksheet in relationship order. */
    std::vector<ExcelTable::Ptr> Tables() const;
    /**
     * @brief Finds a worksheet table by case-insensitive display name.
     * @return The matching table, or nullptr when this worksheet has no table with that name.
     */
    ExcelTable::Ptr TableByName(std::string_view name) const;
    /**
     * @brief Renames an attached table with workbook-wide uniqueness checking.
     * @return true when the table belongs to this worksheet and was renamed.
     */
    bool RenameTable(const ExcelTable::Ptr& table, std::string_view newName);
    /**
     * @brief Removes a table definition, worksheet registry entry, and relationship.
     *
     * Worksheet cell values are intentionally retained. The operation returns
     * false when the table is null or does not belong to this worksheet.
     */
    bool RemoveTable(const ExcelTable::Ptr& table);

    /**
     * @brief Creates a pivot table that reports on a worksheet range.
     *
     * The call creates three package parts and links them: a pivot table
     * definition attached to this worksheet, a pivot cache definition attached
     * to the workbook, and a pivot cache records part below the cache
     * definition. The workbook's `pivotCaches` registry receives an entry with a
     * freshly allocated, workbook-unique cache identifier.
     *
     * The source range is read once. Every source column becomes a pivot cache
     * field; the columns named in @ref ExcelPivotTableDefinition::fields are
     * placed on the requested axis and all remaining columns stay unplaced. The
     * distinct values of every placed column become that field's shared items,
     * sorted ascending.
     *
     * Unless @ref ExcelPivotTableDefinition::writeCachedReport is disabled, the
     * complete report is also computed and written into this worksheet's cells,
     * so the result is readable without recalculation. See
     * [docs/excel/pivot-tables.md](../../../docs/excel/pivot-tables.md) for the
     * exact cell layout.
     *
     * The operation is atomic: on any validation or write failure the workbook
     * is left exactly as it was.
     *
     * @param definition Complete pivot table configuration.
     * @return The attached pivot table together with the structured status. On
     * failure @ref PivotTableCreationResult::PivotTable is nullptr and
     * @ref PivotTableCreationResult::Status carries the reason.
     *
     * @code
     * ExcelPivotTableDefinition definition;
     * definition.SourceSheet = "Data";
     * definition.SourceRange = *CellRange::ParseA1("A1:C13");
     * definition.TargetCell  = *CellAddress::ParseA1("A1");
     * definition.Fields      = {{"Region", PivotAxis::Row}};
     * definition.DataFields  = {{"Amount"}};
     * auto created = reportSheet->CreatePivotTable(definition);
     * if (!created)
     * {
     *     std::cerr << created.Status.Message;
     * }
     * @endcode
     */
    PivotTableCreationResult CreatePivotTable(const ExcelPivotTableDefinition& definition);
    /** @brief Returns all pivot tables hosted by this worksheet, in relationship order. */
    std::vector<ExcelPivotTable::Ptr> PivotTables() const;
    /**
     * @brief Finds a hosted pivot table by case-insensitive name.
     * @return The matching pivot table, or nullptr when this worksheet hosts none with that name.
     */
    ExcelPivotTable::Ptr PivotTableByName(std::string_view name) const;
    /**
     * @brief Removes a pivot table, its cache parts, and its cached report cells.
     *
     * The workbook `pivotCaches` entry, the pivot table part, and the cache
     * definition and records parts are removed together. Cache parts shared with
     * another pivot table are retained. Worksheet cells previously written by
     * the report are cleared.
     *
     * @param pivotTable Pivot table hosted by this worksheet.
     * @return true when the pivot table belonged to this worksheet and was removed.
     */
    bool RemovePivotTable(const ExcelPivotTable::Ptr& pivotTable);

    /**
     * @brief Creates a slicer over a pivot table field or a table column.
     *
     * The call creates a slicer cache definition part below the workbook and,
     * the first time this worksheet receives a slicer, a slicers part below this
     * worksheet. The workbook's extension list receives an `x14:slicerCaches`
     * entry for a pivot slicer or an `x15:slicerCaches` entry for a table
     * slicer, and this worksheet's extension list receives an `x14:slicerList`
     * entry pointing at the slicers part.
     *
     * A slicer that filters the same source column as an existing slicer reuses
     * that slicer's cache, which is what keeps two slicers synchronized.
     *
     * Unless @ref ExcelSlicerDefinition::writeDrawing is disabled, a two-cell
     * drawing anchor carrying an `sle:slicer` graphic frame is written as well,
     * so the slicer is visible in the worksheet. See
     * [docs/excel/slicers.md](../../../docs/excel/slicers.md) for the complete
     * guide, including the Excel 2010 minimum version for the visible shape.
     *
     * The operation is atomic: on any validation or write failure the workbook
     * is left exactly as it was.
     *
     * @param definition Complete slicer configuration.
     * @return The attached slicer together with the structured status. On
     * failure @ref SlicerCreationResult::Slicer is nullptr and
     * @ref SlicerCreationResult::Status carries the reason.
     *
     * @code
     * ExcelSlicerDefinition definition;
     * definition.PivotTableName = "SalesByRegion";
     * definition.SourceField    = "Region";
     * definition.From           = *CellAddress::ParseA1("F2");
     * definition.To             = *CellAddress::ParseA1("H12");
     * auto created = reportSheet->CreateSlicer(definition);
     * if (!created)
     * {
     *     std::cerr << created.Status.Message;
     * }
     * @endcode
     */
    SlicerCreationResult CreateSlicer(const ExcelSlicerDefinition& definition);
    /** @brief Returns all slicers hosted by this worksheet, in XML order. */
    std::vector<ExcelSlicer::Ptr> Slicers() const;
    /**
     * @brief Finds a hosted slicer by case-insensitive name.
     * @return The matching slicer, or nullptr when this worksheet hosts none with that name.
     */
    ExcelSlicer::Ptr SlicerByName(std::string_view name) const;
    /**
     * @brief Removes a slicer, its visible shape, and its unshared cache.
     *
     * The slicer cache part and its workbook registry entry are retained when
     * another slicer still uses them. The slicers part, this worksheet's
     * `x14:slicerList` entry, and the containing extension are removed once the
     * last slicer of this worksheet is gone. A table slicer also drops the
     * table auto-filter it owned.
     *
     * @param slicer Slicer hosted by this worksheet.
     * @return true when the slicer belonged to this worksheet and was removed.
     */
    bool RemoveSlicer(const ExcelSlicer::Ptr& slicer);

    /**
     * @brief Creates and appends an atomically validated data validation rule.
     * @return Attached rule, or nullptr when the definition or worksheet is invalid.
     */
    ExcelDataValidation::Ptr CreateDataValidation(const ExcelDataValidationDefinition& definition);
    /** @brief Enumerates worksheet data validation rules in XML order. */
    std::vector<ExcelDataValidation::Ptr> DataValidations() const;
    /**
     * @brief Replaces an attached rule's complete definition atomically.
     * @return False for invalid input or a rule owned by another worksheet.
     */
    bool UpdateDataValidation(const ExcelDataValidation::Ptr& validation,
                              const ExcelDataValidationDefinition& definition);
    /**
     * @brief Removes an attached rule and updates or removes its container.
     * @return False when the handle is null or belongs to another worksheet.
     */
    bool RemoveDataValidation(const ExcelDataValidation::Ptr& validation);

    /**
     * @brief Creates a validated rule at the lowest evaluation priority.
     * @return The attached rule, or nullptr when the definition or the worksheet is invalid.
     */
    ExcelConditionalFormatting::Ptr CreateConditionalFormatting(
        const ExcelConditionalFormattingDefinition& definition);
    /** @brief Enumerates rules in stable priority order. */
    std::vector<ExcelConditionalFormatting::Ptr> ConditionalFormattings() const;
    /** @brief Atomically replaces a rule definition while preserving its priority. */
    bool UpdateConditionalFormatting(const ExcelConditionalFormatting::Ptr& formatting,
                                     const ExcelConditionalFormattingDefinition& definition);
    /**
     * @brief Moves a rule to a one-based priority, shifting intervening rules.
     * @return False for a foreign rule or a priority outside [1, rule count].
     */
    bool MoveConditionalFormatting(const ExcelConditionalFormatting::Ptr& formatting,
                                   UInt32 priority);
    /** @brief Removes an owned rule and closes the priority gap. */
    bool RemoveConditionalFormatting(const ExcelConditionalFormatting::Ptr& formatting);

    /** @brief Reads explicit metadata for a one-based row, if present. */
    std::optional<RowDimension> GetRowDimension(UInt32 row) const;
    /**
     * @brief Sets or clears metadata for a one-based row without changing cells.
     * @param dimension Nullopt clears height/visibility/outline metadata.
     */
    bool SetRowDimension(UInt32 row, const std::optional<RowDimension>& dimension);
    /** @brief Reads effective explicit metadata for a one-based column. */
    std::optional<ColumnDimension> GetColumnDimension(UInt32 column) const;
    /**
     * @brief Sets or clears metadata for one column while preserving adjacent ranges.
     * @param dimension Nullopt removes the column's explicit metadata.
     */
    bool SetColumnDimension(UInt32 column, const std::optional<ColumnDimension>& dimension);
    /** @brief Reads the primary worksheet view; returns defaults if none is stored. */
    WorksheetView GetView() const;
    /** @brief Atomically writes active-cell and frozen-pane metadata. */
    bool SetView(const WorksheetView& view);

    /** @brief Returns the worksheet page setup, using Excel defaults for absent metadata. */
    PageSetup GetPageSetup() const;
    /** @brief Writes page orientation, paper size, scaling, and fit-to-page settings. */
    bool SetPageSetup(const PageSetup& setup);
    /** @brief Returns page margins in inches, using Excel defaults when absent. */
    PageMargins GetPageMargins() const;
    /** @brief Writes the six page margins. All values must be finite and non-negative. */
    bool SetPageMargins(const PageMargins& margins);
    /** @brief Returns optional print decorations and alignment flags. */
    PrintOptions GetPrintOptions() const;
    /** @brief Writes worksheet print decorations and alignment flags. */
    bool SetPrintOptions(const PrintOptions& options);
    /** @brief Returns Excel header/footer strings, including formatting codes such as `&P`. */
    HeaderFooter GetHeaderFooter() const;
    /** @brief Writes header/footer strings and their first/even-page flags. */
    bool SetHeaderFooter(const HeaderFooter& headerFooter);
    /** @brief Returns the rectangular areas printed from this worksheet. */
    std::vector<CellRange> GetPrintArea() const;
    /** @brief Sets or clears rectangular print areas. An empty vector clears the print area. */
    bool SetPrintArea(const std::vector<CellRange>& areas);
    /** @brief Returns rows and columns repeated on each printed page. */
    PrintTitles GetPrintTitles() const;
    /** @brief Sets or clears repeated print titles. Each range must be ordered and inside the Excel grid. */
    bool SetPrintTitles(const PrintTitles& titles);

    /** @brief Creates or replaces the hyperlink at a cell. */
    bool SetHyperlink(const ExcelHyperlink& hyperlink);

    /** @brief Returns the hyperlink at a cell, if present. */
    std::optional<ExcelHyperlink> GetHyperlink(CellAddress address) const;

    /** @brief Enumerates valid worksheet hyperlinks in document order. */
    std::vector<ExcelHyperlink> Hyperlinks() const;

    /** @brief Removes a hyperlink and its now-unused external relationship. */
    bool RemoveHyperlink(CellAddress address);

    /** @brief Creates or replaces a traditional comment at a cell. */
    bool SetComment(const ExcelComment& comment);

    /** @brief Returns the traditional comment at a cell, if present. */
    std::optional<ExcelComment> GetComment(CellAddress address) const;

    /** @brief Enumerates traditional comments in part order. */
    std::vector<ExcelComment> Comments() const;

    /** @brief Removes a comment and deletes an empty comments part. */
    bool RemoveComment(CellAddress address);

    /** @brief Adds a modern threaded comment or reply. */
    std::optional<std::string> AddThreadedComment(ExcelThreadedComment comment);

    /** @brief Enumerates modern threaded comments in part order. */
    std::vector<ExcelThreadedComment> ThreadedComments() const;

    /** @brief Removes one threaded comment and unused supporting parts. */
    bool RemoveThreadedComment(std::string_view id);

    /** @brief Adds an image and a standards-compliant two-cell drawing anchor. */
    std::optional<UInt32> AddImage(ExcelWorksheetImage image);

    /** @brief Enumerates images managed through worksheet drawing anchors. */
    std::vector<ExcelWorksheetImage> Images() const;

    /** @brief Removes an image and cleans empty drawing and media parts. */
    bool RemoveImage(UInt32 id);

    /**
     * @brief Builds a chart from cell ranges and anchors it into the worksheet.
     *
     * The chart XML is written to a new chart part and referenced by a two-cell
     * graphic-frame anchor in the worksheet drawing. Series value and category
     * ranges are emitted as sheet-qualified absolute formula references, and the
     * current cell values are embedded as numeric and string caches so the chart
     * renders without recalculation. The drawing object identifier shares a
     * numbering space with worksheet images.
     *
     * @param chart Chart definition. At least one series and valid anchors are required.
     * @return The allocated drawing object identifier, or std::nullopt on invalid input.
     */
    std::optional<UInt32> AddChart(ExcelChartDefinition chart);

    /**
     * @brief Updates an existing chart's type, title, axes, legend, anchor,
     * and series in place, refreshing the embedded value/label caches from
     * the worksheet's current cell contents.
     *
     * The chart identified by `chart.Id` keeps its chart part identity and
     * relationship id; only its content is rewritten, so anything that
     * already references the chart (for example a Word document that
     * embeds it) is unaffected. The anchor position is replaced with
     * `chart.From`/`chart.To`, and the `xdr:cNvPr` display name is replaced
     * when @ref ExcelChartDefinition::name is non-empty. Series ranges are
     * re-resolved exactly like @ref AddChart: sheet-qualified absolute
     * formulas, with numeric/string caches embedded for ranges that live on
     * this worksheet.
     *
     * @param chart Replacement chart definition. `chart.Id` selects the
     * chart to update and must match an existing chart anchor's drawing
     * object id; at least one series and valid anchors are required.
     * @return True when a chart with `chart.Id` was found and updated.
     *
     * @code
     * auto charts = sheet->Charts();
     * auto definition = charts.front();
     * definition.Title = "Updated title";
     * definition.Series[0].Values = *CellRange::ParseA1("B1:B4");
     * sheet->UpdateChart(definition);
     * @endcode
     */
    bool UpdateChart(const ExcelChartDefinition& chart);

    /**
     * @brief Enumerates charts managed through worksheet drawing anchors.
     *
     * Each definition is reconstructed from its chart part XML, so type,
     * title, and series formula references round-trip; the embedded value
     * caches themselves are not read back, since they are write-only
     * presentation data for external viewers rather than a second source of
     * truth for the ranges already stored in the workbook.
     *
     * @return One definition per chart anchor, in drawing part order. Empty
     * when the worksheet has no drawing part.
     */
    std::vector<ExcelChartDefinition> Charts() const;

    /**
     * @brief Removes a chart and cleans empty drawing and chart parts.
     *
     * When the removed chart was the last anchor (picture or chart) in the
     * worksheet drawing, the drawing part and its worksheet link are also
     * removed, matching @ref RemoveImage.
     *
     * @param id Drawing object identifier returned by @ref AddChart.
     * @return True when a chart with @p id was found and removed.
     */
    bool RemoveChart(UInt32 id);

    /**
     * @brief Returns the low-level worksheet package part.
     *
     * Advanced callers can use the part to access relationships or raw XML.
     * @return The worksheet part, or nullptr when this wrapper is not attached to one.
     */
    std::shared_ptr<ExyokiOffice::Packaging::WorksheetPart> GetPart() const;
    /**
     * @brief Returns the generated SpreadsheetML worksheet root element.
     *
     * This gives advanced callers direct access to the typed DOM used by the
     * wrapper. The result is nullptr when the wrapper is not attached to a part.
     */
    std::shared_ptr<ExyokiOffice::DocumentFormat::OpenXml::Spreadsheet::Worksheet> GetLowLevelApi() const;

private:
    friend class ExcelDocumentEditor;
    friend class ExcelPivotTable;
    friend class ExcelSlicer;

    /**
     * @brief Rebuilds a pivot table's cache, definition, and cached report cells.
     *
     * Shared by @ref CreatePivotTable and @ref ExcelPivotTable::Update so that a
     * newly created and an updated pivot table are byte-for-byte equivalent. The
     * caller owns the package parts and is responsible for rolling them back.
     *
     * @param host Worksheet the report is written to.
     * @param part Pivot table definition part to fill.
     * @param cachePart Pivot cache definition part to fill.
     * @param recordsPart Pivot cache records part to fill.
     * @param document Workbook that owns every part.
     * @param definition Requested pivot table configuration.
     * @param cacheId Workbook-wide cache identifier already registered in `workbook.xml`.
     */
    static PivotTableResult WritePivotTable(
        Worksheet& host,
        const std::shared_ptr<ExyokiOffice::Packaging::PivotTablePart>& part,
        const std::shared_ptr<ExyokiOffice::Packaging::PivotTableCacheDefinitionPart>& cachePart,
        const std::shared_ptr<ExyokiOffice::Packaging::PivotTableCacheRecordsPart>& recordsPart,
        const ExcelDocument::Ptr& document,
        const ExcelPivotTableDefinition& definition,
        UInt32 cacheId);

    std::string m_name;
    std::shared_ptr<ExyokiOffice::Packaging::WorksheetPart> m_part;
    ExcelDocument::Ptr m_document;
};

/**
 * @brief Workbook-level service for SpreadsheetML shared strings.
 *
 * The shared string table stores distinct text values once and lets cells
 * reference them by zero-based index. This service provides lookup,
 * deduplicated insertion, reference counting, and cleanup/remapping for the
 * workbook edited by ExcelDocumentEditor. It handles plain text shared string
 * items; rich-text runs are preserved as existing table entries but are not
 * synthesized by the high-level API yet.
 */
class EXYOKIOFFICE_EXPORT SharedStringTableService
{
public:
    /**
     * @brief Creates a detached service.
     *
     * Detached services are invalid until a document is supplied through the
     * document constructor or by obtaining the service from ExcelDocumentEditor.
     */
    SharedStringTableService() = default;
    /**
     * @brief Creates a service for the specified workbook document.
     */
    explicit SharedStringTableService(ExcelDocument::Ptr document);

    /**
     * @brief Returns true when the service is attached to a workbook document.
     */
    [[nodiscard]] bool IsValid() const noexcept;
    /**
     * @brief Returns the number of entries in the shared string table.
     *
     * Missing shared string parts are reported as zero entries.
     */
    UInt32 Count() const;
    /**
     * @brief Returns the unique string count stored in the table.
     *
     * For normalized tables this is the same as Count().
     */
    UInt32 UniqueCount() const;
    /**
     * @brief Resolves a shared string index to text.
     *
     * @return Text for the index, or std::nullopt when the index is out of
     * range or the service is invalid.
     */
    std::optional<std::string> Lookup(UInt32 index) const;
    /**
     * @brief Finds an existing plain-text shared string.
     *
     * @return Zero-based index, or std::nullopt when the text is not present.
     */
    std::optional<UInt32> Find(std::string_view text) const;
    /**
     * @brief Inserts text into the shared string table with deduplication.
     *
     * Existing equal plain-text entries are reused. A missing shared string
     * table part is created automatically.
     *
     * @return Zero-based shared string index, or std::nullopt on failure.
     */
    std::optional<UInt32> GetOrAdd(std::string_view text);
    /**
     * @brief Counts worksheet references to a shared string index.
     *
     * The count includes normal shared-string cells and formula cached results
     * whose cached type is shared string.
     */
    UInt32 ReferenceCount(UInt32 index) const;
    /**
     * @brief Removes unused and duplicate entries, then remaps worksheet cells.
     *
     * Only shared string indexes referenced by worksheets are retained. Equal
     * plain-text values collapse to one table entry and all cells are updated
     * to the new indexes. When no shared strings remain, the shared string part
     * is removed from the workbook.
     *
     * @return True when cleanup completed or there was nothing to clean.
     */
    bool Cleanup();

private:
    ExcelDocument::Ptr m_document;
};

/**
 * @brief Workbook-level repository for reusable, deduplicated cell styles.
 *
 * The repository owns no independent cache: it inspects the actual
 * WorkbookStylesPart on every registration, so deduplication remains correct
 * across multiple service objects and after package round trips. Missing style
 * parts are initialized with the mandatory default font, fills, border, base
 * style XF, Normal cell style, and default cell XF.
 */
class EXYOKIOFFICE_EXPORT StyleRepository
{
public:
    /** @brief Creates a detached, invalid repository. */
    StyleRepository() = default;
    /** @brief Creates a repository attached to an Excel document. */
    explicit StyleRepository(ExcelDocument::Ptr document);

    /** @brief Returns true when a workbook document is attached. */
    [[nodiscard]] bool IsValid() const noexcept;
    /** @brief Returns the number of registered cell-XF records. */
    UInt32 Count() const;
    /**
     * @brief Registers a style or reuses an equivalent existing style.
     *
     * Fonts, fills, borders, custom number formats, and the final XF are each
     * deduplicated. Repeating an equal definition therefore does not increase
     * any corresponding stylesheet collection count.
     */
    StyleRegistrationResult GetOrAdd(const ExcelStyle& style);
    /**
     * @brief Applies an existing style index to one cell.
     *
     * A missing sparse cell is materialized as a blank styled cell. Existing
     * values, formulas, and metadata remain unchanged.
     */
    RangeOperationResult ApplyToCell(Worksheet& worksheet, CellAddress address, UInt32 styleIndex);
    /**
     * @brief Applies one style index to every cell in a rectangular range.
     *
     * The operation validates the style and complete range before mutation and
     * restores the original worksheet XML if any write fails.
     */
    RangeOperationResult ApplyToRange(Worksheet& worksheet, CellRange range, UInt32 styleIndex);
    /**
     * @brief Reads the style index stored on a cell.
     *
     * Missing cells and cells without an explicit style return index zero, the
     * workbook's default cell XF.
     */
    std::optional<UInt32> CellStyleIndex(const Worksheet& worksheet, CellAddress address) const;
    /**
     * @brief Reads a registered cell XF back into a high-level style definition.
     *
     * This is the inverse of GetOrAdd() and makes formatting of existing
     * workbooks inspectable and editable. Number format, font, fill, and border
     * components are reported when the XF either sets the matching `apply...`
     * flag or references a non-default component record, which keeps documents
     * written by Excel (where `apply...` flags are frequently omitted) readable.
     * Alignment and protection are reported whenever the corresponding child
     * element exists.
     *
     * Registering the returned definition again through GetOrAdd() yields the
     * same style index, so read-modify-write cycles do not grow the stylesheet.
     * A workbook that has no stylesheet yet still reports an empty default
     * definition for index zero.
     *
     * @param styleIndex Zero-based index into the workbook cell XF collection.
     * @return Style definition, or std::nullopt when the index does not exist.
     */
    std::optional<ExcelStyle> GetStyle(UInt32 styleIndex) const;
    /**
     * @brief Reads the style definition currently applied to one cell.
     *
     * Missing cells and cells without an explicit style resolve to the
     * workbook's default cell XF at index zero.
     *
     * @param worksheet Worksheet that owns the cell.
     * @param address Validated worksheet cell address.
     * @return Style definition, or std::nullopt when the address, worksheet, or
     *         referenced style index is invalid.
     */
    std::optional<ExcelStyle> GetCellStyle(const Worksheet& worksheet, CellAddress address) const;

private:
    ExcelDocument::Ptr m_document;
};

/**
 * @brief High-level editor for creating and modifying Excel workbooks.
 *
 * ExcelDocumentEditor holds a `std::shared_ptr<ExcelDocument>` and keeps the
 * package in memory until it is saved explicitly.
 *
 * Workbook-level operations live on this class: package lifecycle, worksheet
 * creation, renaming, reordering, copying (including CopyWorksheetFrom()
 * across workbooks) and removal, the shared string and style tables, theme
 * settings, the VBA project payload, workbook protection, document properties,
 * and edit transactions and mementos.
 *
 * Cell- and sheet-level editing lives on ExcelWorksheet, obtained from
 * GetWorksheet(), Worksheets(), or FirstWorksheet(): typed cell reads and
 * writes, ranges, formulas, cell and range styling, conditional formatting,
 * tables, data validation, pivot tables, slicers, charts, images, hyperlinks,
 * comments, page setup, and structural row/column inserts and deletes that
 * rewrite the affected references. Lower-level package and DOM APIs remain
 * available through GetDocument() and GetLowLevelApi().
 *
 * @par Thread safety
 * An editor, its worksheets, and every wrapper they hand out are **not**
 * thread-safe. All mutating calls on one workbook, and any read that races
 * with such a call, must be externally serialized. Distinct editors over
 * distinct workbooks may be used concurrently; schema metadata is immutable
 * and shared safely.
 *
 * Example:
 * @code
 * auto editor = ExyokiOffice::Excel::ExcelDocumentEditor::CreateNew();
 * auto sheet = editor->FirstWorksheet();
 * sheet->SetCellText(1, 1, "Name");
 * sheet->SetCellNumber(2, 1, 42);
 * editor->SaveToFile("report.xlsx");
 * @endcode
 */
class EXYOKIOFFICE_EXPORT ExcelDocumentEditor
{
public:
    using Ptr = std::shared_ptr<ExcelDocumentEditor>;

    /**
     * @brief Constructs an editor without an attached document.
     *
     * Use SetDocument(), CreateNew(), Open(), or AddWorksheet() before editing.
     */
    ExcelDocumentEditor() = default;
    ~ExcelDocumentEditor();
    /**
     * @brief Constructs an editor around an existing low-level Excel document.
     *
     * @param document Document instance to edit. The editor keeps it alive.
     */
    explicit ExcelDocumentEditor(const ExcelDocument::Ptr& document);

    /**
     * @brief Creates an editor for an existing document pointer.
     *
     * The document may be null; in that case the editor is created but cannot
     * save or enumerate worksheets until a document is assigned or created.
     */
    static Ptr Create(const ExcelDocument::Ptr& document = nullptr);
    /**
     * @brief Creates a new workbook with one default worksheet named Sheet1.
     *
     * @param type Desired package flavor for the new workbook.
     * @return Editor with an initialized document, or nullptr on failure.
     */
    static Ptr CreateNew(SpreadsheetDocumentType type = SpreadsheetDocumentType::Workbook);
    /**
     * @brief Opens an Excel workbook from disk and wraps it in an editor.
     *
     * @param path Source package path.
     * @param settings Open settings controlling limits and validation behavior.
     * @param cancellationToken Optional non-owning token used to cancel loading.
     * @param error Optional out-parameter; when not null and the call fails, it
     *        receives why. See Packaging::OpenError.
     * @return Editor for the loaded document, or nullptr on failure.
     */
    static Ptr Open(const std::filesystem::path& path,
                    const ExyokiOffice::Packaging::OpenSettings& settings = {},
                    const ICancellationToken* cancellationToken = nullptr,
                    ExyokiOffice::Packaging::OpenError* error = nullptr);
    /**
     * @brief Opens an Excel workbook from an owned byte buffer.
     *
     * @param packageBuffer Bytes containing an OPC SpreadsheetML package.
     * @param settings Open settings controlling limits and validation behavior.
     * @param cancellationToken Optional non-owning token used to cancel loading.
     * @param error Optional out-parameter; when not null and the call fails, it
     *        receives why. See Packaging::OpenError.
     * @return Editor for the loaded document, or nullptr on failure.
     */
    static Ptr Open(const std::vector<Byte>& packageBuffer,
                    const ExyokiOffice::Packaging::OpenSettings& settings = {},
                    const ICancellationToken* cancellationToken = nullptr,
                    ExyokiOffice::Packaging::OpenError* error = nullptr);
    /**
     * @brief Opens an Excel workbook from a contiguous byte range.
     *
     * @param packageBuffer Byte span containing an OPC SpreadsheetML package.
     * @param settings Open settings controlling limits and validation behavior.
     * @param cancellationToken Optional non-owning token used to cancel loading.
     * @param error Optional out-parameter; when not null and the call fails, it
     *        receives why. See Packaging::OpenError.
     * @return Editor for the loaded document, or nullptr on failure.
     */
    static Ptr Open(std::span<const Byte> packageBuffer,
                    const ExyokiOffice::Packaging::OpenSettings& settings = {},
                    const ICancellationToken* cancellationToken = nullptr,
                    ExyokiOffice::Packaging::OpenError* error = nullptr);

    /**
     * @brief Saves the current workbook to a file.
     *
     * @param path Destination path.
     * @param atomicSave When true, write through a temporary file and replace the destination.
     * @param cancellationToken Optional non-owning token used to cancel saving.
     * @return True when the workbook was written successfully.
     */
    bool SaveToFile(const std::filesystem::path& path,
                    bool atomicSave = true,
                    const ICancellationToken* cancellationToken = nullptr);
    /**
     * @brief Serializes the current workbook into memory.
     *
     * @param cancellationToken Optional non-owning token used to cancel saving.
     * @return ZIP package bytes, or an empty vector when no document is attached or saving fails.
     */
    std::vector<Byte> SaveToMemory(const ICancellationToken* cancellationToken = nullptr);

    /**
     * @brief Captures the complete workbook package as an immutable edit memento.
     *
     * The snapshot includes worksheets, shared strings, styles, relationships,
     * drawings, embedded packages, VBA payloads, and every other OPC part.
     *
     * @param cancellationToken Optional non-owning token used to cancel serialization.
     * @return Complete package memento, or `std::nullopt` on failure.
     */
    std::optional<DocumentEditMemento> CreateMemento(
        const ICancellationToken* cancellationToken = nullptr);

    /**
     * @brief Restores a complete workbook from an Excel-family memento.
     *
     * Successful restoration replaces the low-level document graph. Worksheet,
     * cell, part, and DOM wrappers obtained before this call must be reacquired.
     *
     * @param memento Snapshot to restore.
     * @param cancellationToken Optional non-owning token used to cancel package loading.
     * @return True when the complete workbook was reopened and installed.
     */
    bool RestoreMemento(const DocumentEditMemento& memento,
                        const ICancellationToken* cancellationToken = nullptr);

    /**
     * @brief Starts an all-or-nothing workbook edit transaction.
     *
     * Destruction rolls back automatically unless Commit() has accepted the
     * changes. An inactive result indicates that the initial snapshot failed.
     * The transaction must not outlive this editor.
     */
    DocumentEditTransaction BeginTransaction(
        const ICancellationToken* cancellationToken = nullptr);

    /**
     * @brief Replaces the document edited by this instance.
     *
     * @param document New low-level document pointer. May be nullptr.
     */
    void SetDocument(const ExcelDocument::Ptr& document);
    /**
     * @brief Returns the current low-level Excel document.
     * @return The document, or nullptr when no document is attached to this editor.
     */
    ExcelDocument::Ptr GetDocument() const;
    /**
     * @brief Returns the low-level Excel package API.
     *
     * This is equivalent to GetDocument() and mirrors the Word editor naming.
     * @return The document, or nullptr when no document is attached to this editor.
     */
    ExcelDocument::Ptr GetLowLevelApi() const;

    /**
     * @brief Returns the unified core/extended/custom document-properties editor.
     *
     * The returned editor is a lightweight view over the attached document;
     * it stays valid while this editor keeps the document alive. Calling this
     * without an attached document is undefined; check GetDocument() first
     * when the editor may be empty.
     */
    Packaging::DocumentProperties Properties() const;

    /**
     * @brief Lists the workbooks this workbook links to.
     *
     * External links are stored as external OPC relationships on the external
     * link parts, so the targets are enumerable without opening anything. The
     * formula engine evaluates references into these workbooks as `#REF!`
     * because their values are not part of this package.
     *
     * @return One entry per external workbook target; empty when there are none.
     */
    [[nodiscard]] std::vector<Security::ExternalReference> ExternalWorkbookLinks() const;

    /**
     * @brief Reads a linked workbook through the resolver installed on the package.
     *
     * The bytes are returned to the caller; nothing is imported into this
     * workbook and no cached value is updated. Without a resolver the result is
     * Security::ExternalResourceStatus::NoResolver, and with the default policy
     * it is AccessDenied.
     *
     * @param reference Entry obtained from ExternalWorkbookLinks().
     * @param cancellationToken Optional non-owning token handed to the resolver.
     * @return The workbook bytes, or the status explaining why there are none.
     */
    [[nodiscard]] Security::ExternalResourceResponse ResolveExternalWorkbook(
        const Security::ExternalReference& reference,
        const ICancellationToken* cancellationToken = nullptr);

    /**
     * @return Essential typed theme settings of the workbook theme part, or
     *         `std::nullopt` when the workbook has no theme or the theme does
     *         not contain a complete color/font/format scheme.
     */
    std::optional<ExyokiOffice::ThemeSettings> ThemeSettings() const;

    /**
     * @brief Applies essential theme colors, fonts, and scheme names.
     *
     * The workbook must already have a theme part; call EnsureTheme() first
     * for workbooks created without one. Existing fill, line, effect, and
     * extension XML is preserved.
     *
     * @return `false` when the theme is missing or the settings are invalid.
     */
    bool SetThemeSettings(const ExyokiOffice::ThemeSettings& settings);

    /**
     * @return The complete lossless DrawingML theme XML, or `std::nullopt`
     *         when the workbook part has no theme relationship.
     */
    std::optional<std::string> ThemeXml() const;

    /**
     * @brief Creates or replaces the workbook theme from a complete `a:theme` document.
     * @return `true` when the XML was parsed and attached; invalid XML leaves
     *         the current theme unchanged.
     */
    bool SetThemeXml(std::string xml);

    /**
     * @brief Ensures the workbook has a theme part.
     *
     * When the workbook part has no theme relationship, a theme part with the
     * standard Office default theme is created. An existing theme is left
     * untouched.
     *
     * @return `true` when a theme part exists after the call.
     */
    bool EnsureTheme();

    /**
     * @brief Removes the workbook's theme relationship and part.
     * @return `true` when a theme was present and removed.
     */
    bool RemoveTheme();

    /**
     * @brief Reports whether the workbook contains an embedded VBA project.
     *
     * VBA is handled only as opaque binary data; macros are never executed.
     */
    [[nodiscard]] bool HasVbaProject() const noexcept;
    /** @brief Returns an exact copy of `vbaProject.bin`, or an empty vector when absent. */
    std::vector<Byte> GetVbaProjectData() const;
    /**
     * @brief Adds or replaces the opaque VBA project and enables macros for the package type.
     *
     * Empty data is rejected. This API does not parse, edit, validate, or
     * execute the contained VBA code.
     */
    bool SetVbaProjectData(std::span<const Byte> data);
    /**
     * @brief Removes the VBA project and changes the package back to its macro-free counterpart.
     */
    bool RemoveVbaProject();

    /**
     * @brief Adds a worksheet to the workbook and registers it in the workbook sheet list.
     *
     * If this editor has no document yet, a default workbook is created first.
     * An empty name produces a generated SheetN name.
     *
     * @param name Desired worksheet display name.
     * @return Wrapper for the new worksheet, or nullptr on failure.
     */
    Worksheet::Ptr AddWorksheet(std::string_view name);
    /**
     * @brief Returns the worksheet at the specified zero-based workbook position.
     *
     * @param index Zero-based index in workbook sheet-list order.
     * @return Worksheet wrapper, or nullptr when the index is out of range.
     */
    Worksheet::Ptr GetWorksheet(Size index) const;
    /**
     * @brief Finds a worksheet by display name.
     *
     * Excel worksheet names are matched case-insensitively because workbook
     * sheet names are unique without regard to case.
     *
     * @param name Worksheet display name to find.
     * @return Worksheet wrapper, or nullptr when no sheet has that name.
     */
    Worksheet::Ptr GetWorksheet(std::string_view name) const;
    /**
     * @brief Renames a worksheet while preserving workbook invariants.
     *
     * The new name must satisfy Excel's worksheet-name rules: non-empty,
     * at most 31 characters, no `:`, `\`, `/`, `?`, `*`, `[`, or `]`, and
     * unique among worksheets case-insensitively.
     *
     * @param index Zero-based worksheet index.
     * @param newName New display name.
     * @return True when the worksheet was renamed.
     */
    bool RenameWorksheet(Size index, std::string_view newName);
    /**
     * @brief Moves a worksheet to another zero-based workbook position.
     *
     * The operation updates only the workbook sheet list order. Worksheet parts
     * and relationships are preserved.
     *
     * @param fromIndex Current zero-based worksheet index.
     * @param toIndex Destination zero-based worksheet index.
     * @return True when the worksheet order was updated.
     */
    bool MoveWorksheet(Size fromIndex, Size toIndex);
    /**
     * @brief Copies a worksheet within the same workbook.
     *
     * A new worksheet part and workbook relationship are created. The worksheet
     * XML is deep-copied into the new part, so later edits do not mutate the
     * source worksheet. Child relationships of the source worksheet are not
     * copied yet; media, drawings, tables, and comments are handled by later
     * roadmap items.
     *
     * @param sourceIndex Zero-based worksheet index to copy.
     * @param name Desired name for the new sheet. When empty, a unique copy
     * name is generated from the source name.
     * @return Wrapper for the copied worksheet, or nullptr on failure.
     */
    Worksheet::Ptr CopyWorksheet(Size sourceIndex, std::string_view name = {});

    /**
     * @brief Imports one worksheet from a different workbook.
     *
     * The worksheet XML and its complete outgoing OPC part graph are copied into
     * this workbook. This preserves worksheet-owned drawings, images, tables,
     * comments, printer settings, hyperlinks, and other related parts without
     * sharing mutable package objects with the source workbook. Shared-string
     * cell indices are rewritten against this workbook's shared-string table.
     *
     * Cell style indices are workbook-global. Consequently, an imported sheet
     * that contains styled cells is accepted only when the source and destination
     * workbook style parts are byte-for-byte equivalent. Rejecting incompatible
     * style catalogs is deliberate: silently retaining source indices would make
     * cells reference unrelated destination formats. Unstyled worksheets can be
     * imported regardless of the style catalogs.
     *
     * Worksheet names remain case-insensitively unique. When @p name is empty,
     * the source name is retained if available; otherwise a valid `Copy` suffix
     * is generated. An explicit invalid or conflicting name causes failure.
     * The source editor and all its package parts remain unchanged.
     *
     * @param sourceEditor Workbook that owns the worksheet to import. It must be
     *        a different document from this editor's workbook.
     * @param sourceIndex Zero-based worksheet index in source workbook order.
     * @param name Optional destination worksheet name.
     * @return Wrapper for the imported worksheet, or `nullptr` if either editor
     *         is invalid, the index/name is invalid, styles are incompatible,
     *         shared strings cannot be remapped, or the part graph import fails.
     * @note The operation is transactional with respect to the new worksheet:
     *       a failed import removes the partially imported root and does not add
     *       a workbook `<sheet>` entry.
     */
    Worksheet::Ptr CopyWorksheetFrom(const ExcelDocumentEditor& sourceEditor,
                                     Size sourceIndex,
                                     std::string_view name = {});
    /**
     * @brief Removes a worksheet from the workbook.
     *
     * The corresponding workbook `sheet` entry, relationship, worksheet part,
     * and content type are removed together. Removing the last worksheet is
     * rejected so the workbook remains structurally usable.
     *
     * @param index Zero-based worksheet index.
     * @return True when the worksheet was removed.
     */
    bool RemoveWorksheet(Size index);
    /**
     * @brief Enumerates worksheets in workbook sheet-list order.
     *
     * Each returned wrapper references the corresponding WorksheetPart.
     */
    std::vector<Worksheet::Ptr> Worksheets() const;
    /**
     * @brief Returns the first worksheet in workbook sheet-list order.
     *
     * @return First worksheet wrapper, or nullptr when the workbook has no worksheets.
     */
    Worksheet::Ptr FirstWorksheet() const;
    /**
     * @brief Returns a workbook-level shared string table service.
     */
    SharedStringTableService SharedStrings() const;
    /**
     * @brief Returns the workbook cell-style repository.
     *
     * The returned lightweight service references this editor's document and
     * remains usable while that shared document remains alive.
     */
    StyleRepository Styles() const;
    /**
     * @brief Reads the workbook-level structural protection state.
     *
     * @return Protection information, or std::nullopt when the workbook is not
     *         protected or no workbook part is attached.
     */
    std::optional<WorkbookProtectionInfo> GetWorkbookProtection() const;
    /**
     * @brief Protects the workbook structure with optional password verification.
     *
     * Workbook protection prevents structural edits (sheet insertion, deletion,
     * renaming, hiding, reordering) in spreadsheet applications. Like worksheet
     * protection it is an interoperability convention, not encryption, and it
     * does not restrict this library's own editing API.
     *
     * Any existing workbook protection element is replaced. The password uses
     * Excel's legacy 16-bit verifier, which is limited to 15 bytes; longer
     * passwords are rejected and leave the workbook unchanged.
     *
     * @param options Structural locks to activate.
     * @param password Optional legacy password of at most 15 bytes.
     * @return Structured result describing success or the failure reason.
     */
    WorkbookProtectionResult ProtectWorkbook(const WorkbookProtectionOptions& options = {},
                                             std::string_view password = {});
    /**
     * @brief Removes workbook protection after verifying the stored password.
     *
     * Removing protection from an unprotected workbook succeeds and does not
     * modify the package.
     *
     * @param password Password used when protection was applied, or empty when
     *        protection was applied without a password.
     * @return Structured result describing success or the failure reason.
     */
    WorkbookProtectionResult UnprotectWorkbook(std::string_view password = {});

private:
    bool CreateDefaultDocument(SpreadsheetDocumentType type);

    ExcelDocument::Ptr m_document;
    std::shared_ptr<detail::DocumentEditTransactionOwner> m_transactionOwner;
};

} // namespace ExyokiOffice::Excel
