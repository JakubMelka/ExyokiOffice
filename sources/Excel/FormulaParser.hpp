// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

#include "ExyokiOffice/Excel/ExcelFormulaEngine.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ExyokiOffice::Excel
{

/**
 * @brief One endpoint coordinate of a formula reference.
 *
 * Coordinates are one-based like the worksheet grid. The absolute flag
 * records a `$` marker; relative coordinates shift when a shared formula is
 * expanded to a dependent cell.
 */
struct FormulaCoordinate
{
    UInt32 value = 0;
    bool absolute = false;
};

/**
 * @brief Rectangular reference area recognized in formula text.
 *
 * Whole-column references (`A:A`) span every row and whole-row references
 * (`1:3`) span every column; their unbounded coordinates hold the grid
 * limits. An external area preserves its original token text for diagnostics
 * and always evaluates to `#REF!`.
 */
struct FormulaReferenceArea
{
    FormulaCoordinate firstRow;
    FormulaCoordinate lastRow;
    FormulaCoordinate firstColumn;
    FormulaCoordinate lastColumn;
    /** True for `A:A` style references spanning all rows. */
    bool wholeColumns = false;
    /** True for `1:3` style references spanning all columns. */
    bool wholeRows = false;
    /** Sheet qualifier with quoting removed; empty for local references. */
    std::string sheet;
    /** True when a sheet qualifier is present. */
    bool hasSheet = false;
    /** True for `[Book]Sheet!A1` external or `Sheet1:Sheet2!A1` 3-D references. */
    bool external = false;
};

/** @brief Kind of a parsed formula expression node. */
enum class FormulaExpressionKind
{
    NumberLiteral,
    StringLiteral,
    BooleanLiteral,
    ErrorLiteral,
    /** Array constant `{1,2;3,4}`; children hold elements in row-major order. */
    ArrayLiteral,
    /** An omitted function argument such as the second argument of `IF(A1,,2)`. */
    EmptyArgument,
    /** Cell, range, whole-row, or whole-column reference. */
    Reference,
    /** Defined name or structured reference; evaluates to `#NAME?`. */
    NameReference,
    /** Unary `+`, `-`, or postfix `%`; one child. */
    Unary,
    /** Binary operator; two children. */
    Binary,
    /** Reference union `(A1,B2)`; children hold the operands. */
    Union,
    /** Function call; children hold the arguments in order. */
    FunctionCall
};

/** @brief Binary operators recognized by the parser. */
enum class FormulaBinaryOperator
{
    Add,
    Subtract,
    Multiply,
    Divide,
    Power,
    Concatenate,
    Equal,
    NotEqual,
    Less,
    LessEqual,
    Greater,
    GreaterEqual,
    /** Reference intersection written as a space between references. */
    Intersect
};

/** @brief Unary operators recognized by the parser. */
enum class FormulaUnaryOperator
{
    Plus,
    Minus,
    /** Postfix percent; divides the operand by 100. */
    Percent
};

/**
 * @brief Node of a parsed formula expression tree.
 *
 * A single node type keeps the tree cheap to allocate and walk. Which fields
 * are meaningful depends on @ref kind. Source positions are byte offsets into
 * the formula text after any leading equals sign.
 */
struct FormulaExpression
{
    FormulaExpressionKind kind = FormulaExpressionKind::NumberLiteral;

    Real number = 0.0;
    /** String literal text, function name, or defined name. */
    std::string text;
    bool boolean = false;
    FormulaErrorCode error = FormulaErrorCode::None;
    FormulaReferenceArea area;
    Size arrayRowCount = 0;
    Size arrayColumnCount = 0;
    FormulaBinaryOperator binaryOperator = FormulaBinaryOperator::Add;
    FormulaUnaryOperator unaryOperator = FormulaUnaryOperator::Plus;
    std::vector<std::unique_ptr<FormulaExpression>> children;

    Size offset = 0;
    Size length = 0;

    FormulaExpression() = default;

    /**
     * @brief Releases the subtree without recursing once per level.
     *
     * The compiler-written destructor would descend one stack frame per tree
     * level. Nesting depth is capped while parsing, but tree depth is not the
     * same thing: a left-associative chain such as `1+1+1+...` is built by a
     * loop and is as deep as the formula is long. Tearing such a tree down
     * recursively exhausts the stack on formula text the parser accepted, so
     * the nodes are collected into an explicit stack instead.
     */
    ~FormulaExpression();

    FormulaExpression(const FormulaExpression&) = delete;
    FormulaExpression& operator=(const FormulaExpression&) = delete;
};

/** @brief Result of parsing formula text into an expression tree. */
struct FormulaParseResult
{
    /** Root expression; null when parsing failed. */
    std::unique_ptr<FormulaExpression> root;
    /** Diagnostics in source order; empty for a valid formula. */
    std::vector<FormulaDiagnostic> diagnostics;

    bool Succeeded() const noexcept { return root != nullptr && diagnostics.empty(); }
    explicit operator bool() const noexcept { return Succeeded(); }
};

/**
 * @brief Recursive-descent parser for canonical en-US A1 formula text.
 *
 * The parser accepts the SpreadsheetML storage syntax: `,` separates
 * arguments, `.` is the decimal point, `;` separates array-literal rows, and
 * string literals double embedded quotes. A leading `=` is skipped. The
 * parser performs no workbook access and no function-name resolution; the
 * engine validates names and argument counts against its registry afterwards.
 */
class FormulaParser final
{
public:
    FormulaParser() = delete;

    /** @brief Parses formula text into an expression tree. */
    static FormulaParseResult Parse(std::string_view formula);
};

} // namespace ExyokiOffice::Excel
