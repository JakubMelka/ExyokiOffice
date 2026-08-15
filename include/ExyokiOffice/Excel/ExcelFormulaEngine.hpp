// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

#include "ExyokiOffice/Excel/ExcelDocument.hpp"
#include "ExyokiOffice/Export.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ExyokiOffice::Excel
{

class FormulaEvaluationSession;
class FormulaFunctionRegistry;

/**
 * @brief Excel worksheet error constants recognized and produced by the engine.
 *
 * The codes correspond to the classic worksheet error literals. `None`
 * represents "no error" and never appears inside an error value.
 */
enum class FormulaErrorCode
{
    /** No error. Never stored in an error @ref FormulaValue. */
    None,
    /** `#NULL!` - the intersection of two references is empty. */
    Null,
    /** `#DIV/0!` - a number was divided by zero. */
    Div0,
    /** `#VALUE!` - an operand or argument has the wrong type. */
    Value,
    /** `#REF!` - a reference is invalid or was deleted. */
    Ref,
    /** `#NAME?` - an unknown function or defined name was used. */
    Name,
    /** `#NUM!` - a numeric computation produced an invalid number. */
    Num,
    /** `#N/A` - a value is not available, for example a failed lookup. */
    NA
};

/**
 * @brief Returns the worksheet literal for an error code.
 *
 * @param code Error code to format.
 * @return Literal such as `#DIV/0!`, or an empty view for
 * @ref FormulaErrorCode::None.
 */
EXYOKIOFFICE_EXPORT std::string_view FormulaErrorText(FormulaErrorCode code) noexcept;

/**
 * @brief Parses a worksheet error literal.
 *
 * @param text Literal such as `#N/A`. Matching is case-sensitive because
 * SpreadsheetML stores error literals in canonical upper-case form.
 * @return The matching code, or std::nullopt when the text is not one of the
 * seven worksheet error literals.
 */
EXYOKIOFFICE_EXPORT std::optional<FormulaErrorCode> ParseFormulaErrorText(std::string_view text) noexcept;

/** @brief Kind of value produced by formula evaluation. */
enum class FormulaValueKind
{
    /** An empty result, produced by references to blank cells. */
    Blank,
    /** A double-precision number. Dates and times are serial numbers. */
    Number,
    /** A text string. */
    Text,
    /** A logical TRUE or FALSE value. */
    Boolean,
    /** A worksheet error such as `#DIV/0!`. */
    Error,
    /** A rectangular row-major matrix of scalar values (array formulas). */
    Array
};

/**
 * @brief Immutable result of evaluating a formula expression.
 *
 * A FormulaValue is either a scalar (blank, number, text, boolean, or
 * worksheet error) or a rectangular array of scalars produced by an array
 * formula. Worksheet errors are ordinary values: `=1/0` evaluates
 * successfully to `FormulaValue::Error(FormulaErrorCode::Div0)`.
 *
 * Scalars report RowCount() and ColumnCount() of 1 and return themselves from
 * At(0, 0), so uniform element access works for scalar and array results
 * alike.
 */
class EXYOKIOFFICE_EXPORT FormulaValue
{
public:
    /** @brief Creates a blank value. */
    FormulaValue() = default;

    /** @brief Creates a numeric value. */
    static FormulaValue Number(Real value);
    /** @brief Creates a text value. */
    static FormulaValue Text(std::string text);
    /** @brief Creates a logical value. */
    static FormulaValue Boolean(bool value);
    /**
     * @brief Creates a worksheet error value.
     *
     * @param code Concrete error code. Passing @ref FormulaErrorCode::None is
     * invalid and is normalized to `#VALUE!` so an error value always carries
     * a real worksheet error.
     */
    static FormulaValue Error(FormulaErrorCode code);
    /**
     * @brief Creates a rectangular array value.
     *
     * @param rowCount Number of matrix rows; must be at least 1.
     * @param columnCount Number of matrix columns; must be at least 1.
     * @param rowMajorValues Exactly rowCount * columnCount scalar elements in
     * row-major order. Array elements must not themselves be arrays.
     * @return The array value, or a `#VALUE!` error value when the dimensions
     * do not match the element count or an element is itself an array.
     */
    static FormulaValue Array(Size rowCount,
                              Size columnCount,
                              std::vector<FormulaValue> rowMajorValues);

    /** @brief Returns the kind of this value. */
    FormulaValueKind Kind() const noexcept;
    /** @brief Returns true when the value is a worksheet error. */
    [[nodiscard]] bool IsError() const noexcept;
    /**
     * @brief Returns the numeric payload.
     * @return The number when Kind() is Number, otherwise std::nullopt. No
     * type coercion is performed.
     */
    std::optional<Real> NumberValue() const noexcept;
    /**
     * @brief Returns the text payload.
     * @return The string when Kind() is Text, otherwise an empty string.
     */
    const std::string& TextValue() const noexcept;
    /**
     * @brief Returns the logical payload.
     * @return The boolean when Kind() is Boolean, otherwise std::nullopt.
     */
    std::optional<bool> BooleanValue() const noexcept;
    /**
     * @brief Returns the worksheet error code.
     * @return The code when Kind() is Error, otherwise
     * @ref FormulaErrorCode::None.
     */
    FormulaErrorCode ErrorCode() const noexcept;
    /** @brief Returns the number of rows; 1 for scalar values. */
    Size RowCount() const noexcept;
    /** @brief Returns the number of columns; 1 for scalar values. */
    Size ColumnCount() const noexcept;
    /**
     * @brief Returns one matrix element.
     *
     * Scalar values return themselves for (0, 0). Out-of-range coordinates
     * return a static `#REF!` error value.
     *
     * @param row Zero-based row inside the matrix.
     * @param column Zero-based column inside the matrix.
     */
    const FormulaValue& At(Size row, Size column) const noexcept;

    /**
     * @brief Converts to the worksheet cell-value model.
     *
     * Numbers use the same stable round-trip formatting as
     * @ref ExcelCellValue::Number, text becomes an inline string, and array
     * values convert their top-left element. The conversion is intended for
     * writing computed results into plain value cells.
     */
    ExcelCellValue ToCellValue() const;
    /**
     * @brief Formats the value the way a cell would display it.
     *
     * @return For example `42`, `TRUE`, `#DIV/0!`, or an empty string for a
     * blank value. Array values format their top-left element.
     */
    std::string ToDisplayText() const;

private:
    FormulaValueKind m_kind = FormulaValueKind::Blank;
    Real m_number = 0.0;
    std::string m_text;
    bool m_boolean = false;
    FormulaErrorCode m_error = FormulaErrorCode::None;
    Size m_rowCount = 1;
    Size m_columnCount = 1;
    std::shared_ptr<const std::vector<FormulaValue>> m_elements;
};

/**
 * @brief Engine-level error reported by @ref FormulaEngine operations.
 *
 * Engine-level errors describe why an operation could not run at all.
 * Worksheet errors such as `#DIV/0!` are not engine errors; they are ordinary
 * @ref FormulaValue results.
 */
enum class FormulaEngineError
{
    /** The operation completed successfully. */
    None,
    /** The engine has no attached workbook document. */
    InvalidDocument,
    /** The requested worksheet name does not exist in the workbook. */
    UnknownSheet,
    /** A supplied cell address is invalid. */
    InvalidAddress,
    /** The addressed cell exists but does not contain a formula. */
    NotAFormulaCell,
    /** The formula text could not be parsed; see the validation diagnostics. */
    ParseError,
    /** The formula or workbook uses R1C1 references, which the engine does not evaluate. */
    UnsupportedReferenceStyle,
    /** Evaluation or recalculation failed for an internal reason. */
    EvaluationFailed
};

/** @brief Structured status returned by formula-engine operations. */
struct EXYOKIOFFICE_EXPORT FormulaEngineStatus
{
    FormulaEngineError Error = FormulaEngineError::None;
    /** Human-readable English detail suitable for logs. */
    std::string Message;

    /** @brief Returns true when the operation completed successfully. */
    [[nodiscard]] bool Succeeded() const noexcept { return Error == FormulaEngineError::None; }
    /** @brief Provides convenient success testing in conditional statements. */
    [[nodiscard]] explicit operator bool() const noexcept { return Succeeded(); }
};

/**
 * @brief One parser or validator diagnostic with its source position.
 */
struct EXYOKIOFFICE_EXPORT FormulaDiagnostic
{
    /** Zero-based byte offset into the formula text, after any leading `=`. */
    Size Offset = 0;
    /** Length of the offending token in bytes; 0 marks the end of input. */
    Size Length = 0;
    /** Human-readable English explanation. */
    std::string Message;
};

/** @brief Result of validating formula text without evaluating it. */
struct EXYOKIOFFICE_EXPORT FormulaValidationResult
{
    /** Overall status; @ref FormulaEngineError::ParseError when diagnostics exist. */
    FormulaEngineStatus Status;
    /** Parser diagnostics in source order. Empty for a valid formula. */
    std::vector<FormulaDiagnostic> Diagnostics;

    /** @brief Returns true when the formula parsed without diagnostics. */
    [[nodiscard]] bool Succeeded() const noexcept { return Status.Succeeded(); }
    /** @brief Provides convenient success testing in conditional statements. */
    [[nodiscard]] explicit operator bool() const noexcept { return Succeeded(); }
};

/**
 * @brief Result of evaluating one formula expression or formula cell.
 *
 * The status reports engine-level failures only. A successfully evaluated
 * formula whose result is a worksheet error - for example `#DIV/0!` - has a
 * succeeded status and an error-kind @ref value.
 */
struct EXYOKIOFFICE_EXPORT FormulaEvaluationResult
{
    /** Engine-level status; parse failures also fill @ref diagnostics. */
    FormulaEngineStatus Status;
    /** Parser diagnostics when the status is ParseError. */
    std::vector<FormulaDiagnostic> Diagnostics;
    /** Computed value; meaningful only when the status succeeded. */
    FormulaValue Value;

    /** @brief Returns true when evaluation ran to completion. */
    [[nodiscard]] bool Succeeded() const noexcept { return Status.Succeeded(); }
    /** @brief Provides convenient success testing in conditional statements. */
    [[nodiscard]] explicit operator bool() const noexcept { return Succeeded(); }
};

/** @brief Worksheet-qualified cell address used in recalculation reports. */
struct EXYOKIOFFICE_EXPORT SheetCellAddress
{
    /** Worksheet display name, never quoted. */
    std::string Sheet;
    /** Cell address on that worksheet. */
    CellAddress Address;

    /**
     * @brief Formats as a sheet-qualified A1 reference such as `Sheet1!B2`.
     *
     * The sheet name is quoted with apostrophes when Excel requires quoting,
     * matching @ref SheetCellRange::ToFormula.
     */
    std::string ToFormula() const;
};

/** @brief Result of a workbook or worksheet recalculation. */
struct EXYOKIOFFICE_EXPORT RecalculationResult
{
    FormulaEngineStatus Status;
    /** Number of formula cells whose cached result was recomputed. */
    Size RecalculatedCellCount = 0;
    /**
     * Circular-reference cycles found during recalculation. Each inner vector
     * lists the members of one cycle. Cycle members keep their previous
     * cached values and are not counted in @ref recalculatedCellCount.
     */
    std::vector<std::vector<SheetCellAddress>> CircularReferenceCycles;

    /** @brief Returns true when recalculation ran to completion. */
    [[nodiscard]] bool Succeeded() const noexcept { return Status.Succeeded(); }
    /** @brief Provides convenient success testing in conditional statements. */
    [[nodiscard]] explicit operator bool() const noexcept { return Succeeded(); }
};

/**
 * @brief Invocation context passed to registered formula functions.
 *
 * The context describes where the formula is being evaluated. Ad-hoc
 * evaluation through @ref FormulaEngine::EvaluateFormula may run without an
 * anchor cell; Anchor() is invalid in that case.
 */
class EXYOKIOFFICE_EXPORT FormulaFunctionContext
{
public:
    /** @brief Returns the display name of the worksheet being evaluated. */
    const std::string& SheetName() const noexcept { return m_sheetName; }
    /**
     * @brief Returns the address of the cell whose formula is evaluated.
     *
     * @return The anchor address, or an invalid address for ad-hoc evaluation
     * without a cell context.
     */
    CellAddress Anchor() const noexcept { return m_anchor; }

private:
    friend class FormulaEvaluationSession;

    std::string m_sheetName;
    CellAddress m_anchor;
    const FormulaEvaluationSession* m_session = nullptr;
};

/**
 * @brief Callable implementing a worksheet function.
 *
 * Arguments arrive fully evaluated: references are dereferenced, and range
 * arguments arrive as @ref FormulaValueKind::Array values. The callable may
 * return any @ref FormulaValue, including worksheet errors. It must not throw;
 * error conditions are expressed by returning an error value.
 */
using FormulaFunction =
    std::function<FormulaValue(FormulaFunctionContext&, std::span<const FormulaValue>)>;

/** @brief Argument specification of a registered worksheet function. */
struct EXYOKIOFFICE_EXPORT FormulaFunctionSpec
{
    /** Minimum number of arguments accepted by the function. */
    Size MinimumArgumentCount = 0;
    /** Maximum number of arguments accepted; Excel's limit is 255. */
    Size MaximumArgumentCount = 255;
    /**
     * Marks the function volatile. Volatile functions such as NOW or RAND may
     * produce a different result on every evaluation, so recalculation always
     * recomputes cells that call them.
     */
    bool IsVolatile = false;
};

/**
 * @brief Workbook-scoped formula parser, evaluator, and recalculation service.
 *
 * FormulaEngine adds calculation semantics on top of the formula storage
 * already provided by @ref Worksheet::SetCellFormula and
 * @ref Worksheet::GetCellFormula. It parses and validates A1 formulas,
 * evaluates them against workbook data with Excel-compatible operator and
 * coercion semantics, detects circular references, and recalculates whole
 * worksheets or workbooks in dependency order, writing cached results so
 * spreadsheet applications display values immediately on open.
 *
 * The engine follows the workbook-service pattern of
 * @ref SharedStringTableService: it is a lightweight wrapper around a shared
 * @ref ExcelDocument and remains usable while that document is alive.
 *
 * Formula syntax is the canonical SpreadsheetML en-US form: `,` separates
 * arguments, `.` is the decimal point, and `;` separates array-literal rows.
 * Worksheet errors are values, not failures: `=1/0` evaluates successfully to
 * a `#DIV/0!` @ref FormulaValue. Statuses fail only for engine-level problems
 * such as an unknown worksheet or unparsable text.
 *
 * @code
 * auto editor = ExcelDocumentEditor::CreateNew();
 * auto sheet = editor->FirstWorksheet();
 * sheet->SetCellNumber(1, 1, 10.0);
 * sheet->SetCellNumber(2, 1, 32.0);
 * sheet->SetCellFormula(*CellAddress::ParseA1("A3"), "=SUM(A1:A2)");
 *
 * FormulaEngine engine(editor->GetDocument());
 * const auto result = engine.EvaluateCell("Sheet1", *CellAddress::ParseA1("A3"));
 * // result.Value.NumberValue() == 42.0
 *
 * engine.Recalculate();          // writes cached results into formula cells
 * editor->SaveToFile("report.xlsx");
 * @endcode
 *
 * Defined names created through @ref NamedRangeManager resolve during
 * evaluation - the sheet scope of the evaluating worksheet first, then the
 * workbook scope - and recalculation tracks dependencies through them.
 *
 * Known limitations, reported as documented error values rather than being
 * silently miscalculated: unknown defined names and structured table
 * references evaluate to `#NAME?`, external workbook references evaluate to
 * `#REF!`, and R1C1 formulas are rejected with
 * @ref FormulaEngineError::UnsupportedReferenceStyle.
 */
class EXYOKIOFFICE_EXPORT FormulaEngine
{
public:
    /**
     * @brief Creates a detached engine.
     *
     * Detached engines are invalid until a document is supplied through the
     * document constructor.
     */
    FormulaEngine();
    /**
     * @brief Creates an engine for the specified workbook document.
     *
     * The built-in function library is available immediately. Functions
     * registered through @ref RegisterFunction affect only this engine
     * instance.
     */
    explicit FormulaEngine(ExcelDocument::Ptr document);

    ~FormulaEngine();
    FormulaEngine(const FormulaEngine& other);
    FormulaEngine(FormulaEngine&& other) noexcept;
    FormulaEngine& operator=(const FormulaEngine& other);
    FormulaEngine& operator=(FormulaEngine&& other) noexcept;

    /** @brief Returns true when the engine is attached to a workbook document. */
    [[nodiscard]] bool IsValid() const noexcept;

    /**
     * @brief Parses formula text and reports syntax diagnostics.
     *
     * Validation is purely lexical and syntactic: no cells are read and no
     * functions are invoked. Unknown function names, and - when a document is
     * attached - names not defined in any scope, are reported because they
     * would evaluate to `#NAME?`. The formula may carry a leading `=`.
     *
     * @param formula Formula text to validate.
     * @return Success for a well-formed formula, or a ParseError status with
     * position diagnostics.
     */
    FormulaValidationResult ValidateFormula(std::string_view formula) const;

    /**
     * @brief Evaluates formula text against the workbook without modifying it.
     *
     * The formula is evaluated as if it were entered on the specified
     * worksheet, so unqualified references resolve there. Formula cells that
     * the expression references contribute their stored cached values.
     *
     * @param formula Formula text, with or without a leading `=`.
     * @param sheetName Worksheet used to resolve unqualified references. An
     * empty name selects the first worksheet.
     * @param anchor Optional cell context supplying ROW() and COLUMN(). An
     * invalid address means the formula has no cell context.
     * @return The computed value, or an engine-level failure status.
     */
    FormulaEvaluationResult EvaluateFormula(std::string_view formula,
                                            std::string_view sheetName = {},
                                            CellAddress anchor = {}) const;

    /**
     * @brief Evaluates one stored formula cell without modifying the worksheet.
     *
     * Shared formulas are expanded: a dependent cell of a shared group is
     * evaluated with its anchor's expression shifted to the cell's position.
     * The stored cached value is ignored; the expression is recomputed from
     * current workbook data.
     *
     * @param sheetName Worksheet display name; matched case-insensitively
     * like @ref ExcelDocumentEditor::GetWorksheet.
     * @param address Address of the formula cell.
     * @return The computed value, or a failure status such as
     * @ref FormulaEngineError::NotAFormulaCell.
     */
    FormulaEvaluationResult EvaluateCell(std::string_view sheetName, CellAddress address) const;

    /**
     * @brief Recalculates every formula cell in the workbook.
     *
     * Formula cells are evaluated in dependency order and their cached
     * results are rewritten through the storage API, so the workbook displays
     * up-to-date values when opened in a spreadsheet application. Cells that
     * participate in a circular reference keep their previous cached values
     * and are reported in the result; cells that depend on a cycle are
     * evaluated using the cycle members' previous cached values.
     *
     * A stale calculation-chain part, when present, is removed because
     * spreadsheet applications rebuild it automatically.
     *
     * @return Recalculation statistics and any circular-reference cycles.
     */
    RecalculationResult Recalculate();
    /**
     * @brief Recalculates the formula cells of one worksheet.
     *
     * Cross-sheet precedents are still read - only the named worksheet's
     * formula cells are recomputed and rewritten.
     *
     * @param sheetName Worksheet display name, matched case-insensitively.
     */
    RecalculationResult RecalculateSheet(std::string_view sheetName);

    /**
     * @brief Reports circular-reference cycles without modifying the workbook.
     *
     * @return One inner vector per cycle, listing the participating formula
     * cells. An empty result means the workbook has no circular references.
     */
    std::vector<std::vector<SheetCellAddress>> FindCircularReferences() const;

    /**
     * @brief Registers or replaces a custom worksheet function.
     *
     * Function names are case-insensitive; `MyFunc` and `MYFUNC` address the
     * same registration. Built-in functions may be overridden. The
     * registration affects only this engine instance and engines copied from
     * it afterwards.
     *
     * @param name Function name; ASCII letters, digits, `.`, and `_`, not
     * starting with a digit.
     * @param spec Argument-count limits and volatility.
     * @param function Callable evaluated for each invocation; must not be null.
     * @return True when the function was registered.
     */
    bool RegisterFunction(std::string_view name, FormulaFunctionSpec spec, FormulaFunction function);
    /** @brief Tests whether a function name is registered, case-insensitively. */
    [[nodiscard]] bool IsFunctionRegistered(std::string_view name) const;
    /**
     * @brief Lists registered function names.
     *
     * @return Upper-case names sorted alphabetically, including built-ins and
     * custom registrations.
     */
    std::vector<std::string> FunctionNames() const;

private:
    ExcelDocument::Ptr m_document;
    std::shared_ptr<FormulaFunctionRegistry> m_registry;
};

} // namespace ExyokiOffice::Excel
