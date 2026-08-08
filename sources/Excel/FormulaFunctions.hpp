// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

#include "FormulaEvaluator.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <functional>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ExyokiOffice::Excel
{

/**
 * @brief Functions the evaluator must handle with lazy argument evaluation.
 *
 * Special forms receive unevaluated argument expressions so, for example,
 * `IF(TRUE,1,1/0)` never evaluates the division.
 */
enum class FormulaSpecialForm
{
    None,
    If,
    IfError,
    IfNa,
    Choose,
    Ifs,
    Switch,
    And,
    Or
};

/**
 * @brief Callable implementing a built-in function.
 *
 * Arguments arrive as raw evaluation values: references are preserved so
 * aggregate and reference functions can iterate stored cells. Implementations
 * use @ref FormulaFunctionHelpers to coerce arguments.
 */
using InternalFormulaFunction =
    std::function<FormulaValue(FormulaEvaluationSession&, std::span<EvalValue>)>;

/** @brief One registry entry: either a built-in or a user registration. */
struct RegisteredFormulaFunction
{
    FormulaFunctionSpec spec;
    FormulaSpecialForm specialForm = FormulaSpecialForm::None;
    /** Built-in implementation; empty for custom functions and special forms. */
    InternalFormulaFunction internalFunction;
    /** User-registered implementation with the public signature. */
    FormulaFunction customFunction;
};

/**
 * @brief Case-insensitive function registry backing one @ref FormulaEngine.
 *
 * Construction copies the shared built-in table, so user registrations never
 * affect other engine instances. Keys are stored upper-cased.
 */
class FormulaFunctionRegistry
{
public:
    FormulaFunctionRegistry();

    /** @brief Finds a registration by name, case-insensitively. */
    const RegisteredFormulaFunction* Find(std::string_view name) const;
    /**
     * @brief Registers or replaces a custom function.
     *
     * @return False for an invalid name or a null callable.
     */
    bool RegisterCustom(std::string_view name, FormulaFunctionSpec spec, FormulaFunction function);
    /** @brief Tests whether a name is registered. */
    bool IsRegistered(std::string_view name) const;
    /** @brief Returns sorted upper-case names of all registrations. */
    std::vector<std::string> Names() const;
    /** @brief Returns true when the named function is volatile. */
    bool IsVolatile(std::string_view name) const;

    /** @brief Upper-cases an ASCII function name. */
    static std::string NormalizeName(std::string_view name);
    /** @brief Validates a function name for registration. */
    static bool IsValidName(std::string_view name);

private:
    using FunctionMap = std::map<std::string, RegisteredFormulaFunction, std::less<>>;

    static const FunctionMap& BuiltIns();

    FunctionMap m_functions;
};

/**
 * @brief Installer helpers splitting the built-in library across sources.
 *
 * FormulaFunctions.cpp installs the registry core plus math, logical,
 * information, statistical, and conditional-aggregation functions.
 * FormulaFunctionsLibrary.cpp installs text, lookup, date/time, and financial
 * functions.
 */
class FormulaFunctionLibrary final
{
public:
    FormulaFunctionLibrary() = delete;

    using FunctionMap = std::map<std::string, RegisteredFormulaFunction, std::less<>>;

    static void RegisterMathFunctions(FunctionMap& functions);
    static void RegisterLogicalFunctions(FunctionMap& functions);
    static void RegisterInformationFunctions(FunctionMap& functions);
    static void RegisterStatisticalFunctions(FunctionMap& functions);
    static void RegisterConditionalAggregationFunctions(FunctionMap& functions);
    static void RegisterTextFunctions(FunctionMap& functions);
    static void RegisterLookupFunctions(FunctionMap& functions);
    static void RegisterDateTimeFunctions(FunctionMap& functions);
    static void RegisterFinancialFunctions(FunctionMap& functions);
};

/** @brief Shared coercion and iteration helpers for function implementations. */
class FormulaFunctionHelpers final
{
public:
    FormulaFunctionHelpers() = delete;

    /** @brief Dereferences an argument to a scalar with implicit intersection. */
    static FormulaValue Scalar(FormulaEvaluationSession& session, const EvalValue& value);
    /** @brief Scalar argument coerced to a number; errors pass through. */
    static FormulaValue ScalarNumber(FormulaEvaluationSession& session, const EvalValue& value);
    /** @brief Scalar argument coerced to text; errors pass through. */
    static FormulaValue ScalarText(FormulaEvaluationSession& session, const EvalValue& value);
    /** @brief Scalar argument coerced to a logical value; errors pass through. */
    static FormulaValue ScalarBoolean(FormulaEvaluationSession& session, const EvalValue& value);

    /**
     * @brief Streams the numeric contents of aggregate arguments.
     *
     * Excel aggregation rules: scalar arguments are coerced to numbers, while
     * numbers inside references and arrays are used directly and text or
     * logical cells are skipped. The first error encountered is returned;
     * otherwise std::nullopt.
     *
     * @param includeBooleansInRanges COUNTA-style helpers may include
     * non-numeric range members; aggregation of numbers keeps them excluded.
     */
    static std::optional<FormulaValue> ForEachNumber(
        FormulaEvaluationSession& session,
        std::span<EvalValue> arguments,
        const std::function<void(Real)>& callback);

    /**
     * @brief Streams every scalar value of one argument (reference, array, or
     * scalar), including blanks for array elements but not for missing cells.
     *
     * @return An error value when the sheet of a reference is unknown.
     */
    static std::optional<FormulaValue> ForEachValue(
        FormulaEvaluationSession& session,
        const EvalValue& argument,
        const std::function<void(const FormulaValue&)>& callback);

    /**
     * @brief Counts cells in reference arguments, visiting stored cells only.
     */
    static std::optional<FormulaValue> ForEachCell(
        FormulaEvaluationSession& session,
        const EvalValue& argument,
        const std::function<void(UInt32 row, UInt32 column, const FormulaValue& value)>& callback);

    /**
     * @brief Materializes an argument as a matrix value for lookup functions.
     *
     * References are clipped to the stored worksheet extent, but never below
     * the size needed to keep lookup semantics: missing trailing cells read
     * as blanks on demand through the returned matrix.
     */
    static FormulaValue Materialize(FormulaEvaluationSession& session, const EvalValue& value);

    /**
     * @brief Excel criteria matcher for COUNTIF/SUMIF-style arguments.
     *
     * Supports comparison prefixes (`>`, `>=`, `<`, `<=`, `<>`, `=`) and the
     * wildcards `*` and `?` with `~` escaping.
     */
    class Criterion
    {
    public:
        explicit Criterion(const FormulaValue& criterion);

        /** @brief Tests one cell value against the criterion. */
        bool Matches(const FormulaValue& value) const;

    private:
        enum class Comparison
        {
            Equal,
            NotEqual,
            Less,
            LessEqual,
            Greater,
            GreaterEqual
        };

        Comparison m_comparison = Comparison::Equal;
        FormulaValue m_operand;
        bool m_hasWildcards = false;
        std::string m_pattern;
    };

    /** @brief Case-insensitive wildcard match with `~` escaping. */
    static bool WildcardMatch(std::string_view pattern, std::string_view text);
};

} // namespace ExyokiOffice::Excel
