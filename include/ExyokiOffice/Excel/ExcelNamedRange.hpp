// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

#include "ExyokiOffice/Excel/ExcelDocument.hpp"
#include "ExyokiOffice/Export.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ExyokiOffice::Excel
{

/**
 * @brief Visibility scope of a defined name.
 *
 * Workbook-scoped names are visible from every worksheet. Sheet-scoped names
 * belong to one worksheet; a formula on that worksheet resolves the
 * sheet-scoped name before any workbook-scoped name of the same spelling, and
 * other worksheets reach it only through an explicit qualifier such as
 * `Sheet1!LocalName`.
 */
enum class NamedRangeScope
{
    /** The name is visible workbook-wide (no `localSheetId`). */
    Workbook,
    /** The name belongs to one worksheet (`localSheetId` set). */
    Sheet
};

/** @brief Error code reported by named-range operations. */
enum class NamedRangeError
{
    /** The operation completed successfully. */
    None,
    /** The manager has no attached workbook document. */
    InvalidDocument,
    /**
     * The name violates Excel's naming rules: it must be non-empty, at most
     * 255 characters, start with a letter, `_`, or `\`, continue with
     * letters, digits, `.`, `_`, or `\`, must not contain spaces, must not
     * spell a cell reference such as `A1` or `R1C1`, and must not be the
     * single letter `C` or `R`.
     */
    InvalidName,
    /** A name with the same case-insensitive spelling exists in the scope. */
    DuplicateName,
    /** The requested scope worksheet does not exist. */
    UnknownSheet,
    /** The supplied range or formula text is empty or invalid. */
    InvalidRange,
    /** No defined name with this spelling exists in the scope. */
    NameNotFound
};

/** @brief Structured status returned by named-range operations. */
struct EXYOKIOFFICE_EXPORT NamedRangeResult
{
    NamedRangeError Error = NamedRangeError::None;
    /** Human-readable English detail suitable for logs. */
    std::string Message;

    /** @brief Returns true when the operation completed successfully. */
    [[nodiscard]] bool Succeeded() const noexcept { return Error == NamedRangeError::None; }
    /** @brief Provides convenient success testing in conditional statements. */
    [[nodiscard]] explicit operator bool() const noexcept { return Succeeded(); }
};

/**
 * @brief One defined name read from the workbook.
 *
 * The stored @ref formula is the SpreadsheetML `refersTo` text without a
 * leading `=`. For names created by @ref NamedRangeManager::Create it is an
 * absolute sheet-qualified range such as `Sheet1!$A$1:$B$4`, but SpreadsheetML
 * allows any formula - constants, function calls, or references to other
 * names - which round-trip unchanged.
 */
struct EXYOKIOFFICE_EXPORT NamedRange
{
    /** Defined name exactly as stored. */
    std::string Name;
    /** Visibility scope. */
    NamedRangeScope Scope = NamedRangeScope::Workbook;
    /**
     * Owning worksheet display name when @ref scope is Sheet. Empty for
     * workbook-scoped names, and for sheet-scoped entries whose stored sheet
     * index does not match any worksheet (a package inconsistency).
     */
    std::string ScopeSheet;
    /** `refersTo` formula text without a leading `=`. */
    std::string Formula;
    /** True when the name is hidden from name-manager user interfaces. */
    bool Hidden = false;
    /** Optional comment stored with the name; empty when absent. */
    std::string Comment;

    /**
     * @brief Parses the formula as a single sheet-qualified range.
     *
     * @return The range when the formula has the shape `Sheet!A1:B4` (with or
     * without `$` markers and quoting), otherwise std::nullopt - for example
     * for constant or computed names.
     */
    std::optional<SheetCellRange> Range() const;
};

/**
 * @brief Workbook-level service for creating and managing defined names.
 *
 * NamedRangeManager edits the workbook's `definedNames` collection. It
 * follows the workbook-service pattern of @ref SharedStringTableService: a
 * lightweight wrapper around a shared @ref ExcelDocument that stays usable
 * while the document is alive.
 *
 * Names created here are visible to the formula engine: a formula such as
 * `=SUM(SalesData)` resolves the name - sheet scope first, then workbook
 * scope - and @ref FormulaEngine recalculation tracks dependencies through
 * names.
 *
 * @code
 * auto editor = ExcelDocumentEditor::CreateNew();
 * auto sheet = editor->FirstWorksheet();
 * sheet->SetCellNumber(1, 1, 10.0);
 * sheet->SetCellNumber(2, 1, 32.0);
 *
 * NamedRangeManager names(editor->GetDocument());
 * names.Create("SalesData", SheetCellRange("Sheet1", *CellRange::ParseA1("A1:A2")));
 * sheet->SetCellFormula(*CellAddress::ParseA1("B1"), "=SUM(SalesData)");
 *
 * FormulaEngine engine(editor->GetDocument());
 * engine.Recalculate();          // B1 caches 42
 * @endcode
 *
 * Known limitation: structural worksheet edits (row/column insertion or
 * deletion, sheet removal or reordering) do not rewrite defined-name
 * formulas or stored sheet indexes yet.
 */
class EXYOKIOFFICE_EXPORT NamedRangeManager
{
public:
    /**
     * @brief Creates a detached manager.
     *
     * Detached managers are invalid until a document is supplied through the
     * document constructor.
     */
    NamedRangeManager() = default;
    /** @brief Creates a manager for the specified workbook document. */
    explicit NamedRangeManager(ExcelDocument::Ptr document);

    /** @brief Returns true when the manager is attached to a workbook document. */
    [[nodiscard]] bool IsValid() const noexcept;

    /**
     * @brief Creates a defined name referring to a worksheet range.
     *
     * The range is stored as an absolute sheet-qualified reference
     * (`Sheet1!$A$1:$B$4`). The range's worksheet must exist. Names are
     * unique case-insensitively within their scope; the same spelling may
     * exist once per worksheet scope and once at workbook scope.
     *
     * @param name New defined name; see @ref NamedRangeError::InvalidName for
     * the accepted syntax.
     * @param range Worksheet-qualified target range.
     * @param scope Workbook-wide or sheet-local visibility.
     * @param scopeSheet Owning worksheet display name; required when @p scope
     * is Sheet and ignored otherwise.
     */
    NamedRangeResult Create(std::string_view name,
                            const SheetCellRange& range,
                            NamedRangeScope scope = NamedRangeScope::Workbook,
                            std::string_view scopeSheet = {});
    /**
     * @brief Creates a defined name from raw formula text.
     *
     * The text is stored as the name's `refersTo` formula without validation
     * of its semantics, so constants (`=1.05`), computed definitions
     * (`=SUM(Sheet1!$A$1:$A$9)`), and references to other names are all
     * possible. A leading `=` is accepted and stripped.
     *
     * @param name New defined name.
     * @param formula Definition text; must not be empty.
     * @param scope Workbook-wide or sheet-local visibility.
     * @param scopeSheet Owning worksheet display name for sheet scope.
     */
    NamedRangeResult CreateFromFormula(std::string_view name,
                                       std::string_view formula,
                                       NamedRangeScope scope = NamedRangeScope::Workbook,
                                       std::string_view scopeSheet = {});

    /**
     * @brief Finds a defined name in one exact scope.
     *
     * @param name Name to find; matched case-insensitively.
     * @param scopeSheet Empty selects the workbook scope; a worksheet display
     * name selects that sheet's scope.
     * @return The stored definition, or std::nullopt when the scope has no
     * such name.
     */
    std::optional<NamedRange> Find(std::string_view name, std::string_view scopeSheet = {}) const;
    /**
     * @brief Resolves a name the way a formula on a worksheet would.
     *
     * The worksheet's sheet scope is searched first, then the workbook scope.
     *
     * @param name Name to resolve; matched case-insensitively.
     * @param sheetName Worksheet the formula is evaluated on.
     * @return The visible definition, or std::nullopt when neither scope
     * defines the name.
     */
    std::optional<NamedRange> Resolve(std::string_view name, std::string_view sheetName) const;
    /**
     * @brief Returns the range a defined name refers to.
     *
     * Convenience for `Find(...)->Range()`.
     *
     * @return The range, or std::nullopt when the name does not exist in the
     * scope or its formula is not a single range reference.
     */
    std::optional<SheetCellRange> GetRange(std::string_view name, std::string_view scopeSheet = {}) const;

    /** @brief Returns every defined name in workbook storage order. */
    std::vector<NamedRange> List() const;
    /** @brief Returns the defined names of one scope in storage order. */
    std::vector<NamedRange> List(NamedRangeScope scope) const;
    /** @brief Returns the number of defined names in the workbook. */
    Size Count() const;

    /**
     * @brief Replaces the range an existing defined name refers to.
     *
     * @param name Existing name; matched case-insensitively in the scope
     * selected by @p scopeSheet.
     * @param range New worksheet-qualified target range.
     * @param scopeSheet Empty selects the workbook scope.
     */
    NamedRangeResult SetRange(std::string_view name,
                              const SheetCellRange& range,
                              std::string_view scopeSheet = {});
    /**
     * @brief Replaces the formula of an existing defined name.
     *
     * A leading `=` is accepted and stripped; the text must not be empty.
     */
    NamedRangeResult SetFormula(std::string_view name,
                                std::string_view formula,
                                std::string_view scopeSheet = {});
    /**
     * @brief Renames a defined name within its scope.
     *
     * The new name must satisfy the naming rules and must not collide with
     * another name in the same scope. Formulas that reference the old name
     * are not rewritten.
     *
     * @param name Existing name.
     * @param newName Replacement name.
     * @param scopeSheet Empty selects the workbook scope.
     */
    NamedRangeResult Rename(std::string_view name,
                            std::string_view newName,
                            std::string_view scopeSheet = {});
    /**
     * @brief Removes a defined name from one exact scope.
     *
     * Formulas that reference the removed name are not modified; they
     * evaluate to `#NAME?` afterwards. Removing the last defined name also
     * removes the empty `definedNames` collection element.
     */
    NamedRangeResult Remove(std::string_view name, std::string_view scopeSheet = {});

    /**
     * @brief Tests a candidate against Excel's defined-name syntax rules.
     *
     * The check covers syntax only; scope uniqueness is verified by the
     * mutation methods.
     */
    [[nodiscard]] static bool IsValidName(std::string_view name);

private:
    ExcelDocument::Ptr m_document;
};

} // namespace ExyokiOffice::Excel
