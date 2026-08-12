// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "FormulaFunctions.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <random>
#include <utility>

namespace ExyokiOffice::Excel
{

namespace FormulaFunctionDetail
{

char AsciiUpper(char c)
{
    return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
}

int CompareTextIgnoreCase(std::string_view left, std::string_view right)
{
    const Size common = std::min(left.size(), right.size());
    for (Size i = 0; i < common; ++i)
    {
        const char l = AsciiUpper(left[i]);
        const char r = AsciiUpper(right[i]);
        if (l != r)
        {
            return l < r ? -1 : 1;
        }
    }
    if (left.size() == right.size())
    {
        return 0;
    }
    return left.size() < right.size() ? -1 : 1;
}

void Add(FormulaFunctionLibrary::FunctionMap& functions,
         std::string name,
         Size minimumArguments,
         Size maximumArguments,
         InternalFormulaFunction function,
         bool isVolatile = false)
{
    RegisteredFormulaFunction entry;
    entry.spec.MinimumArgumentCount = minimumArguments;
    entry.spec.MaximumArgumentCount = maximumArguments;
    entry.spec.IsVolatile = isVolatile;
    entry.internalFunction = std::move(function);
    functions.insert_or_assign(std::move(name), std::move(entry));
}

void AddSpecialForm(FormulaFunctionLibrary::FunctionMap& functions,
                    std::string name,
                    Size minimumArguments,
                    Size maximumArguments,
                    FormulaSpecialForm form)
{
    RegisteredFormulaFunction entry;
    entry.spec.MinimumArgumentCount = minimumArguments;
    entry.spec.MaximumArgumentCount = maximumArguments;
    entry.specialForm = form;
    functions.insert_or_assign(std::move(name), std::move(entry));
}

/** Wraps a numeric function of one argument. */
InternalFormulaFunction Unary(std::function<FormulaValue(Real)> body)
{
    return [body = std::move(body)](FormulaEvaluationSession& session,
                                    std::span<EvalValue> arguments) -> FormulaValue
    {
        const FormulaValue value = FormulaFunctionHelpers::ScalarNumber(session, arguments[0]);
        if (value.IsError())
        {
            return value;
        }
        return body(*value.NumberValue());
    };
}

/** Wraps a numeric function of two arguments. */
InternalFormulaFunction BinaryNumeric(std::function<FormulaValue(Real, Real)> body)
{
    return [body = std::move(body)](FormulaEvaluationSession& session,
                                    std::span<EvalValue> arguments) -> FormulaValue
    {
        const FormulaValue left = FormulaFunctionHelpers::ScalarNumber(session, arguments[0]);
        if (left.IsError())
        {
            return left;
        }
        const FormulaValue right = FormulaFunctionHelpers::ScalarNumber(session, arguments[1]);
        if (right.IsError())
        {
            return right;
        }
        return body(*left.NumberValue(), *right.NumberValue());
    };
}

FormulaValue FiniteNumber(Real value)
{
    if (!std::isfinite(value))
    {
        return FormulaValue::Error(FormulaErrorCode::Num);
    }
    return FormulaValue::Number(value);
}

/** The direction the ROUND family carries a scaled value to an integer. */
enum class RoundingMode
{
    HalfAwayFromZero, // ROUND
    TowardZero,       // ROUNDDOWN, TRUNC
    AwayFromZero,     // ROUNDUP
};

/**
 * Rounds @p value at 10^digits the way Excel's ROUND family does, including
 * the digit counts whose scale factor leaves the double range:
 *
 * - 10^digits overflows to infinity, or value * 10^digits overflows: the
 *   rounding unit 10^-digits is below one ulp of the value, so rounding
 *   cannot change it and the value is returned unchanged rather than turned
 *   into #NUM! by way of inf/scale.
 * - 10^digits underflows to zero (digits <= about -324): the rounding unit
 *   exceeds the double range. Rounding toward zero or to nearest therefore
 *   reaches 0, while rounding a nonzero value away from zero would need a
 *   magnitude of at least 10^324 and reports overflow (#NUM! through
 *   FiniteNumber), continuing what the digit counts just above -324 produce.
 */
Real RoundAtScale(Real value, Real digits, RoundingMode mode)
{
    const Real scale = std::pow(10.0, std::floor(digits));
    if (scale == 0.0)
    {
        if (mode == RoundingMode::AwayFromZero && value != 0.0)
        {
            return std::copysign(std::numeric_limits<Real>::infinity(), value);
        }
        return 0.0;
    }
    const Real scaled = value * scale;
    if (!std::isfinite(scaled))
    {
        return value;
    }
    Real rounded = 0.0;
    switch (mode)
    {
        case RoundingMode::HalfAwayFromZero:
            rounded = scaled >= 0.0 ? std::floor(scaled + 0.5) : std::ceil(scaled - 0.5);
            break;
        case RoundingMode::TowardZero:
            rounded = scaled >= 0.0 ? std::floor(scaled) : std::ceil(scaled);
            break;
        case RoundingMode::AwayFromZero:
            rounded = scaled >= 0.0 ? std::ceil(scaled) : std::floor(scaled);
            break;
    }
    return rounded / scale;
}

/** Excel ROUND: half away from zero at the given decimal digit count. */
Real RoundHalfAwayFromZero(Real value, Real digits)
{
    return RoundAtScale(value, digits, RoundingMode::HalfAwayFromZero);
}

Real RoundTowardZero(Real value, Real digits)
{
    return RoundAtScale(value, digits, RoundingMode::TowardZero);
}

Real RoundAwayFromZero(Real value, Real digits)
{
    return RoundAtScale(value, digits, RoundingMode::AwayFromZero);
}

/** Collects every aggregated number into a vector. */
std::optional<FormulaValue> CollectNumbers(FormulaEvaluationSession& session,
                                           std::span<EvalValue> arguments,
                                           std::vector<Real>& numbers)
{
    return FormulaFunctionHelpers::ForEachNumber(session, arguments,
                                                 [&numbers](Real value)
                                                 { numbers.push_back(value); });
}

/**
 * Clips whole-row/whole-column areas to the stored worksheet extent so the
 * criteria loops stay proportional to worksheet content.
 */
ResolvedReferenceArea ClipToExtent(FormulaEvaluationSession& session, const ResolvedReferenceArea& area)
{
    ResolvedReferenceArea clipped = area;
    if (clipped.lastRow == MaxRowIndex || clipped.lastColumn == MaxColumnIndex)
    {
        const auto [maxRow, maxColumn] = session.SheetExtent(area.sheet);
        if (clipped.lastRow == MaxRowIndex)
        {
            clipped.lastRow = std::max(clipped.firstRow, maxRow);
        }
        if (clipped.lastColumn == MaxColumnIndex)
        {
            clipped.lastColumn = std::max(clipped.firstColumn, maxColumn);
        }
    }
    return clipped;
}

/**
 * Iterates every cell of an area including blanks when the area is small
 * enough; enormous areas fall back to stored cells only, which differs from
 * Excel only for criteria that match blank cells.
 */
constexpr UInt64 MaxGeometricIterationCells = 1u << 20;

bool ForEachCriteriaCell(FormulaEvaluationSession& session,
                         const ResolvedReferenceArea& area,
                         const std::function<void(UInt32 row, UInt32 column,
                                                  const FormulaValue& value)>& callback)
{
    const UInt64 cellCount = static_cast<UInt64>(area.RowCount()) * area.ColumnCount();
    if (cellCount > MaxGeometricIterationCells)
    {
        return session.ForEachStoredCell(area, callback);
    }
    for (UInt32 row = area.firstRow; row <= area.lastRow; ++row)
    {
        for (UInt32 column = area.firstColumn; column <= area.lastColumn; ++column)
        {
            callback(row, column, session.ReadCell(area.sheet, row, column));
        }
    }
    return true;
}

/** Shared implementation for the *IF single-criteria aggregates. */
enum class SingleCriteriaMode
{
    Count,
    Sum,
    Average
};

FormulaValue EvaluateSingleCriteria(FormulaEvaluationSession& session,
                                    std::span<EvalValue> arguments,
                                    SingleCriteriaMode mode)
{
    if (!arguments[0].isReference || arguments[0].areas.size() != 1)
    {
        return FormulaValue::Error(FormulaErrorCode::Value);
    }
    if (!session.SheetExists(arguments[0].areas.front().sheet))
    {
        return FormulaValue::Error(FormulaErrorCode::Ref);
    }
    const ResolvedReferenceArea criteriaArea = ClipToExtent(session, arguments[0].areas.front());

    const FormulaValue criterionValue = FormulaFunctionHelpers::Scalar(session, arguments[1]);
    if (criterionValue.IsError() && criterionValue.ErrorCode() != FormulaErrorCode::NA)
    {
        return criterionValue;
    }
    const FormulaFunctionHelpers::Criterion criterion(criterionValue);

    const ResolvedReferenceArea* valueArea = &criteriaArea;
    ResolvedReferenceArea shiftedArea;
    if (mode != SingleCriteriaMode::Count && arguments.size() >= 3)
    {
        if (!arguments[2].isReference || arguments[2].areas.size() != 1)
        {
            return FormulaValue::Error(FormulaErrorCode::Value);
        }
        // Excel aligns the value range to the criteria range's shape using its
        // top-left cell.
        shiftedArea = arguments[2].areas.front();
        shiftedArea.lastRow = shiftedArea.firstRow + criteriaArea.RowCount() - 1;
        shiftedArea.lastColumn = shiftedArea.firstColumn + criteriaArea.ColumnCount() - 1;
        valueArea = &shiftedArea;
    }

    Real sum = 0.0;
    Size matchCount = 0;
    FormulaValue error;
    bool hasError = false;

    const bool visited = ForEachCriteriaCell(
        session, criteriaArea, [&](UInt32 row, UInt32 column, const FormulaValue& value)
        {
            if (hasError || !criterion.Matches(value))
            {
                return;
            }
            if (mode == SingleCriteriaMode::Count)
            {
                ++matchCount;
                return;
            }
            const UInt32 valueRow = valueArea->firstRow + (row - criteriaArea.firstRow);
            const UInt32 valueColumn = valueArea->firstColumn + (column - criteriaArea.firstColumn);
            const FormulaValue cell = session.ReadCell(valueArea->sheet, valueRow, valueColumn);
            if (cell.IsError())
            {
                error = cell;
                hasError = true;
                return;
            }
            if (cell.Kind() == FormulaValueKind::Number)
            {
                sum += *cell.NumberValue();
                ++matchCount;
            } });
    if (!visited)
    {
        return FormulaValue::Error(FormulaErrorCode::Ref);
    }
    if (hasError)
    {
        return error;
    }

    switch (mode)
    {
        case SingleCriteriaMode::Count:
            return FormulaValue::Number(static_cast<Real>(matchCount));
        case SingleCriteriaMode::Sum:
            return FiniteNumber(sum);
        case SingleCriteriaMode::Average:
            if (matchCount == 0)
            {
                return FormulaValue::Error(FormulaErrorCode::Div0);
            }
            return FiniteNumber(sum / static_cast<Real>(matchCount));
    }
    return FormulaValue::Error(FormulaErrorCode::Value);
}

/** Shared implementation for the *IFS multi-criteria aggregates. */
enum class MultiCriteriaMode
{
    Count,
    Sum,
    Average
};

FormulaValue EvaluateMultiCriteria(FormulaEvaluationSession& session,
                                   std::span<EvalValue> arguments,
                                   MultiCriteriaMode mode)
{
    Size pairStart = 0;
    const ResolvedReferenceArea* valueArea = nullptr;
    ResolvedReferenceArea clippedValueArea;
    if (mode != MultiCriteriaMode::Count)
    {
        if (!arguments[0].isReference || arguments[0].areas.size() != 1)
        {
            return FormulaValue::Error(FormulaErrorCode::Value);
        }
        if (!session.SheetExists(arguments[0].areas.front().sheet))
        {
            return FormulaValue::Error(FormulaErrorCode::Ref);
        }
        clippedValueArea = ClipToExtent(session, arguments[0].areas.front());
        valueArea = &clippedValueArea;
        pairStart = 1;
    }
    if ((arguments.size() - pairStart) < 2 || (arguments.size() - pairStart) % 2 != 0)
    {
        return FormulaValue::Error(FormulaErrorCode::Value);
    }

    struct CriteriaPair
    {
        ResolvedReferenceArea area;
        std::optional<FormulaFunctionHelpers::Criterion> criterion;
    };
    std::vector<CriteriaPair> pairs;
    for (Size i = pairStart; i + 1 < arguments.size(); i += 2)
    {
        if (!arguments[i].isReference || arguments[i].areas.size() != 1)
        {
            return FormulaValue::Error(FormulaErrorCode::Value);
        }
        if (!session.SheetExists(arguments[i].areas.front().sheet))
        {
            return FormulaValue::Error(FormulaErrorCode::Ref);
        }
        CriteriaPair pair;
        pair.area = ClipToExtent(session, arguments[i].areas.front());
        const FormulaValue criterionValue = FormulaFunctionHelpers::Scalar(session, arguments[i + 1]);
        if (criterionValue.IsError() && criterionValue.ErrorCode() != FormulaErrorCode::NA)
        {
            return criterionValue;
        }
        pair.criterion.emplace(criterionValue);
        pairs.push_back(std::move(pair));
    }

    const ResolvedReferenceArea& shape = pairs.front().area;
    const UInt32 rowCount = shape.RowCount();
    const UInt32 columnCount = shape.ColumnCount();
    for (const auto& pair : pairs)
    {
        if (pair.area.RowCount() != rowCount || pair.area.ColumnCount() != columnCount)
        {
            return FormulaValue::Error(FormulaErrorCode::Value);
        }
    }
    if (valueArea && (valueArea->RowCount() != rowCount || valueArea->ColumnCount() != columnCount))
    {
        return FormulaValue::Error(FormulaErrorCode::Value);
    }

    Real sum = 0.0;
    Size matchCount = 0;
    FormulaValue cellError;
    bool hasCellError = false;
    const auto processCell = [&](UInt32 rowDelta, UInt32 columnDelta)
    {
        if (hasCellError)
        {
            return;
        }
        for (const auto& pair : pairs)
        {
            const FormulaValue value = session.ReadCell(pair.area.sheet,
                                                        pair.area.firstRow + rowDelta,
                                                        pair.area.firstColumn + columnDelta);
            if (!pair.criterion->Matches(value))
            {
                return;
            }
        }
        if (mode == MultiCriteriaMode::Count)
        {
            ++matchCount;
            return;
        }
        const FormulaValue cell = session.ReadCell(valueArea->sheet,
                                                   valueArea->firstRow + rowDelta,
                                                   valueArea->firstColumn + columnDelta);
        if (cell.IsError())
        {
            cellError = cell;
            hasCellError = true;
            return;
        }
        if (cell.Kind() == FormulaValueKind::Number)
        {
            sum += *cell.NumberValue();
            ++matchCount;
        }
    };

    const UInt64 cellCount = static_cast<UInt64>(rowCount) * columnCount;
    if (cellCount <= MaxGeometricIterationCells)
    {
        for (UInt32 rowDelta = 0; rowDelta < rowCount && !hasCellError; ++rowDelta)
        {
            for (UInt32 columnDelta = 0; columnDelta < columnCount; ++columnDelta)
            {
                processCell(rowDelta, columnDelta);
            }
        }
    }
    else
    {
        // Enormous ranges fall back to stored cells of the first criteria
        // range; this differs from Excel only for criteria matching blanks.
        const ResolvedReferenceArea& probeArea = pairs.front().area;
        const bool visited = session.ForEachStoredCell(
            probeArea, [&](UInt32 row, UInt32 column, const FormulaValue&)
            { processCell(row - probeArea.firstRow, column - probeArea.firstColumn); });
        if (!visited)
        {
            return FormulaValue::Error(FormulaErrorCode::Ref);
        }
    }
    if (hasCellError)
    {
        return cellError;
    }

    switch (mode)
    {
        case MultiCriteriaMode::Count:
            return FormulaValue::Number(static_cast<Real>(matchCount));
        case MultiCriteriaMode::Sum:
            return FiniteNumber(sum);
        case MultiCriteriaMode::Average:
            if (matchCount == 0)
            {
                return FormulaValue::Error(FormulaErrorCode::Div0);
            }
            return FiniteNumber(sum / static_cast<Real>(matchCount));
    }
    return FormulaValue::Error(FormulaErrorCode::Value);
}

std::mt19937& RandomEngine()
{
    thread_local std::mt19937 engine{std::random_device{}()};
    return engine;
}

} // namespace FormulaFunctionDetail

// ---------------------------------------------------------------------------
// FormulaFunctionRegistry
// ---------------------------------------------------------------------------

FormulaFunctionRegistry::FormulaFunctionRegistry() : m_functions(BuiltIns())
{
}

const FormulaFunctionRegistry::FunctionMap& FormulaFunctionRegistry::BuiltIns()
{
    static const FunctionMap builtIns = []
    {
        FunctionMap functions;
        FormulaFunctionLibrary::RegisterMathFunctions(functions);
        FormulaFunctionLibrary::RegisterLogicalFunctions(functions);
        FormulaFunctionLibrary::RegisterInformationFunctions(functions);
        FormulaFunctionLibrary::RegisterStatisticalFunctions(functions);
        FormulaFunctionLibrary::RegisterConditionalAggregationFunctions(functions);
        FormulaFunctionLibrary::RegisterTextFunctions(functions);
        FormulaFunctionLibrary::RegisterLookupFunctions(functions);
        FormulaFunctionLibrary::RegisterDateTimeFunctions(functions);
        FormulaFunctionLibrary::RegisterFinancialFunctions(functions);
        return functions;
    }();
    return builtIns;
}

std::string FormulaFunctionRegistry::NormalizeName(std::string_view name)
{
    std::string result(name);
    for (char& c : result)
    {
        c = FormulaFunctionDetail::AsciiUpper(c);
    }
    return result;
}

bool FormulaFunctionRegistry::IsValidName(std::string_view name)
{
    if (name.empty())
    {
        return false;
    }
    const char first = name.front();
    if (!((first >= 'A' && first <= 'Z') || (first >= 'a' && first <= 'z') || first == '_'))
    {
        return false;
    }
    for (const char c : name)
    {
        const bool valid = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                           (c >= '0' && c <= '9') || c == '_' || c == '.';
        if (!valid)
        {
            return false;
        }
    }
    return true;
}

const RegisteredFormulaFunction* FormulaFunctionRegistry::Find(std::string_view name) const
{
    const auto it = m_functions.find(NormalizeName(name));
    return it != m_functions.end() ? &it->second : nullptr;
}

bool FormulaFunctionRegistry::RegisterCustom(std::string_view name,
                                             FormulaFunctionSpec spec,
                                             FormulaFunction function)
{
    if (!IsValidName(name) || !function ||
        spec.MinimumArgumentCount > spec.MaximumArgumentCount)
    {
        return false;
    }
    RegisteredFormulaFunction entry;
    entry.spec = spec;
    entry.customFunction = std::move(function);
    m_functions.insert_or_assign(NormalizeName(name), std::move(entry));
    return true;
}

bool FormulaFunctionRegistry::IsRegistered(std::string_view name) const
{
    return Find(name) != nullptr;
}

std::vector<std::string> FormulaFunctionRegistry::Names() const
{
    std::vector<std::string> names;
    names.reserve(m_functions.size());
    for (const auto& [name, entry] : m_functions)
    {
        names.push_back(name);
    }
    return names;
}

bool FormulaFunctionRegistry::IsVolatile(std::string_view name) const
{
    const RegisteredFormulaFunction* entry = Find(name);
    return entry != nullptr && entry->spec.IsVolatile;
}

// ---------------------------------------------------------------------------
// FormulaFunctionHelpers
// ---------------------------------------------------------------------------

FormulaValue FormulaFunctionHelpers::Scalar(FormulaEvaluationSession& session, const EvalValue& value)
{
    return session.DereferenceScalar(value);
}

FormulaValue FormulaFunctionHelpers::ScalarNumber(FormulaEvaluationSession& session, const EvalValue& value)
{
    return FormulaCoercion::ToNumber(Scalar(session, value));
}

FormulaValue FormulaFunctionHelpers::ScalarText(FormulaEvaluationSession& session, const EvalValue& value)
{
    return FormulaCoercion::ToText(Scalar(session, value));
}

FormulaValue FormulaFunctionHelpers::ScalarBoolean(FormulaEvaluationSession& session, const EvalValue& value)
{
    return FormulaCoercion::ToBoolean(Scalar(session, value));
}

std::optional<FormulaValue> FormulaFunctionHelpers::ForEachNumber(
    FormulaEvaluationSession& session,
    std::span<EvalValue> arguments,
    const std::function<void(Real)>& callback)
{
    for (const EvalValue& argument : arguments)
    {
        if (argument.isReference)
        {
            FormulaValue error;
            bool hasError = false;
            for (const ResolvedReferenceArea& area : argument.areas)
            {
                const bool visited = session.ForEachStoredCell(
                    area, [&](UInt32, UInt32, const FormulaValue& value)
                    {
                        if (hasError)
                        {
                            return;
                        }
                        if (value.IsError())
                        {
                            error = value;
                            hasError = true;
                            return;
                        }
                        if (value.Kind() == FormulaValueKind::Number)
                        {
                            callback(*value.NumberValue());
                        } });
                if (!visited)
                {
                    return FormulaValue::Error(FormulaErrorCode::Ref);
                }
                if (hasError)
                {
                    return error;
                }
            }
            continue;
        }

        const FormulaValue& value = argument.value;
        if (value.Kind() == FormulaValueKind::Array)
        {
            for (Size row = 0; row < value.RowCount(); ++row)
            {
                for (Size column = 0; column < value.ColumnCount(); ++column)
                {
                    const FormulaValue& element = value.At(row, column);
                    if (element.IsError())
                    {
                        return element;
                    }
                    if (element.Kind() == FormulaValueKind::Number)
                    {
                        callback(*element.NumberValue());
                    }
                }
            }
            continue;
        }

        // Direct scalar arguments are coerced like Excel does.
        if (value.Kind() == FormulaValueKind::Blank)
        {
            callback(0.0);
            continue;
        }
        const FormulaValue number = FormulaCoercion::ToNumber(value);
        if (number.IsError())
        {
            return number;
        }
        callback(*number.NumberValue());
    }
    return std::nullopt;
}

std::optional<FormulaValue> FormulaFunctionHelpers::ForEachValue(
    FormulaEvaluationSession& session,
    const EvalValue& argument,
    const std::function<void(const FormulaValue&)>& callback)
{
    if (argument.isReference)
    {
        for (const ResolvedReferenceArea& area : argument.areas)
        {
            const bool visited = session.ForEachStoredCell(
                area, [&](UInt32, UInt32, const FormulaValue& value)
                { callback(value); });
            if (!visited)
            {
                return FormulaValue::Error(FormulaErrorCode::Ref);
            }
        }
        return std::nullopt;
    }
    const FormulaValue& value = argument.value;
    if (value.Kind() == FormulaValueKind::Array)
    {
        for (Size row = 0; row < value.RowCount(); ++row)
        {
            for (Size column = 0; column < value.ColumnCount(); ++column)
            {
                callback(value.At(row, column));
            }
        }
        return std::nullopt;
    }
    callback(value);
    return std::nullopt;
}

std::optional<FormulaValue> FormulaFunctionHelpers::ForEachCell(
    FormulaEvaluationSession& session,
    const EvalValue& argument,
    const std::function<void(UInt32, UInt32, const FormulaValue&)>& callback)
{
    if (!argument.isReference)
    {
        return FormulaValue::Error(FormulaErrorCode::Value);
    }
    for (const ResolvedReferenceArea& area : argument.areas)
    {
        const bool visited = session.ForEachStoredCell(area, callback);
        if (!visited)
        {
            return FormulaValue::Error(FormulaErrorCode::Ref);
        }
    }
    return std::nullopt;
}

FormulaValue FormulaFunctionHelpers::Materialize(FormulaEvaluationSession& session, const EvalValue& value)
{
    return session.DereferenceToValue(value);
}

bool FormulaFunctionHelpers::WildcardMatch(std::string_view pattern, std::string_view text)
{
    // Iterative wildcard match with star backtracking; `~` escapes the next
    // pattern character.
    Size patternIndex = 0;
    Size textIndex = 0;
    Size starPattern = std::string_view::npos;
    Size starText = 0;

    const auto upper = FormulaFunctionDetail::AsciiUpper;
    while (textIndex < text.size())
    {
        bool literal = false;
        char patternChar = '\0';
        Size nextPatternIndex = patternIndex;
        if (patternIndex < pattern.size())
        {
            patternChar = pattern[patternIndex];
            nextPatternIndex = patternIndex + 1;
            if (patternChar == '~' && patternIndex + 1 < pattern.size())
            {
                literal = true;
                patternChar = pattern[patternIndex + 1];
                nextPatternIndex = patternIndex + 2;
            }
        }

        if (patternIndex < pattern.size() && !literal && patternChar == '*')
        {
            starPattern = patternIndex;
            starText = textIndex;
            patternIndex = nextPatternIndex;
            continue;
        }
        if (patternIndex < pattern.size() &&
            ((!literal && patternChar == '?') || upper(patternChar) == upper(text[textIndex])))
        {
            patternIndex = nextPatternIndex;
            ++textIndex;
            continue;
        }
        if (starPattern != std::string_view::npos)
        {
            patternIndex = starPattern + 1;
            ++starText;
            textIndex = starText;
            continue;
        }
        return false;
    }
    while (patternIndex < pattern.size() && pattern[patternIndex] == '*')
    {
        ++patternIndex;
    }
    return patternIndex == pattern.size();
}

FormulaFunctionHelpers::Criterion::Criterion(const FormulaValue& criterion)
{
    if (criterion.Kind() != FormulaValueKind::Text)
    {
        m_operand = criterion;
        return;
    }

    std::string_view text = criterion.TextValue();
    if (text.starts_with("<>"))
    {
        m_comparison = Comparison::NotEqual;
        text.remove_prefix(2);
    }
    else if (text.starts_with(">="))
    {
        m_comparison = Comparison::GreaterEqual;
        text.remove_prefix(2);
    }
    else if (text.starts_with("<="))
    {
        m_comparison = Comparison::LessEqual;
        text.remove_prefix(2);
    }
    else if (text.starts_with(">"))
    {
        m_comparison = Comparison::Greater;
        text.remove_prefix(1);
    }
    else if (text.starts_with("<"))
    {
        m_comparison = Comparison::Less;
        text.remove_prefix(1);
    }
    else if (text.starts_with("="))
    {
        m_comparison = Comparison::Equal;
        text.remove_prefix(1);
    }

    if (const auto number = FormulaCoercion::ParseNumberText(text))
    {
        m_operand = FormulaValue::Number(*number);
        return;
    }
    if (FormulaFunctionDetail::CompareTextIgnoreCase(text, "TRUE") == 0)
    {
        m_operand = FormulaValue::Boolean(true);
        return;
    }
    if (FormulaFunctionDetail::CompareTextIgnoreCase(text, "FALSE") == 0)
    {
        m_operand = FormulaValue::Boolean(false);
        return;
    }
    m_operand = FormulaValue::Text(std::string(text));
    if ((m_comparison == Comparison::Equal || m_comparison == Comparison::NotEqual) &&
        (text.find('*') != std::string_view::npos || text.find('?') != std::string_view::npos))
    {
        m_hasWildcards = true;
        m_pattern = std::string(text);
    }
}

bool FormulaFunctionHelpers::Criterion::Matches(const FormulaValue& value) const
{
    // Empty criterion matches blank cells and empty text.
    if (m_operand.Kind() == FormulaValueKind::Blank ||
        (m_operand.Kind() == FormulaValueKind::Text && m_operand.TextValue().empty()))
    {
        const bool isEmpty = value.Kind() == FormulaValueKind::Blank ||
                             (value.Kind() == FormulaValueKind::Text && value.TextValue().empty());
        return m_comparison == Comparison::NotEqual ? !isEmpty : isEmpty;
    }

    if (m_hasWildcards)
    {
        if (value.Kind() != FormulaValueKind::Text)
        {
            return m_comparison == Comparison::NotEqual;
        }
        const bool matched = WildcardMatch(m_pattern, value.TextValue());
        return m_comparison == Comparison::NotEqual ? !matched : matched;
    }

    // Criteria compare only within the same type family.
    if (value.Kind() != m_operand.Kind())
    {
        return m_comparison == Comparison::NotEqual;
    }
    const auto comparison = FormulaCoercion::Compare(value, m_operand);
    if (!comparison)
    {
        return false;
    }
    switch (m_comparison)
    {
        case Comparison::Equal:
            return *comparison == 0;
        case Comparison::NotEqual:
            return *comparison != 0;
        case Comparison::Less:
            return *comparison < 0;
        case Comparison::LessEqual:
            return *comparison <= 0;
        case Comparison::Greater:
            return *comparison > 0;
        case Comparison::GreaterEqual:
            return *comparison >= 0;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Math and trigonometry
// ---------------------------------------------------------------------------

void FormulaFunctionLibrary::RegisterMathFunctions(FunctionMap& functions)
{
    using namespace FormulaFunctionDetail;
    using Helpers = FormulaFunctionHelpers;

    Add(functions, "SUM", 1, 255,
        [](FormulaEvaluationSession& session, std::span<EvalValue> arguments) -> FormulaValue
        {
            Real sum = 0.0;
            if (const auto error = Helpers::ForEachNumber(session, arguments,
                                                          [&sum](Real value)
                                                          { sum += value; }))
            {
                return *error;
            }
            return FiniteNumber(sum);
        });

    Add(functions, "PRODUCT", 1, 255,
        [](FormulaEvaluationSession& session, std::span<EvalValue> arguments) -> FormulaValue
        {
            Real product = 1.0;
            bool any = false;
            if (const auto error = Helpers::ForEachNumber(session, arguments, [&](Real value)
                                                          {
                    product *= value;
                    any = true; }))
            {
                return *error;
            }
            return FiniteNumber(any ? product : 0.0);
        });

    Add(functions, "ABS", 1, 1, Unary([](Real x)
                                      { return FormulaValue::Number(std::fabs(x)); }));
    Add(functions, "SIGN", 1, 1,
        Unary([](Real x)
              { return FormulaValue::Number(x > 0.0 ? 1.0 : (x < 0.0 ? -1.0 : 0.0)); }));
    Add(functions, "INT", 1, 1, Unary([](Real x)
                                      { return FormulaValue::Number(std::floor(x)); }));
    Add(functions, "SQRT", 1, 1, Unary([](Real x)
                                       {
            if (x < 0.0)
            {
                return FormulaValue::Error(FormulaErrorCode::Num);
            }
            return FormulaValue::Number(std::sqrt(x)); }));
    Add(functions, "EXP", 1, 1, Unary([](Real x)
                                      { return FiniteNumber(std::exp(x)); }));
    Add(functions, "LN", 1, 1, Unary([](Real x)
                                     {
            if (x <= 0.0)
            {
                return FormulaValue::Error(FormulaErrorCode::Num);
            }
            return FormulaValue::Number(std::log(x)); }));
    Add(functions, "LOG10", 1, 1, Unary([](Real x)
                                        {
            if (x <= 0.0)
            {
                return FormulaValue::Error(FormulaErrorCode::Num);
            }
            return FormulaValue::Number(std::log10(x)); }));
    Add(functions, "LOG", 1, 2,
        [](FormulaEvaluationSession& session, std::span<EvalValue> arguments) -> FormulaValue
        {
            const FormulaValue number = Helpers::ScalarNumber(session, arguments[0]);
            if (number.IsError())
            {
                return number;
            }
            Real base = 10.0;
            if (arguments.size() >= 2)
            {
                const FormulaValue baseValue = Helpers::ScalarNumber(session, arguments[1]);
                if (baseValue.IsError())
                {
                    return baseValue;
                }
                base = *baseValue.NumberValue();
            }
            const Real x = *number.NumberValue();
            if (x <= 0.0 || base <= 0.0)
            {
                return FormulaValue::Error(FormulaErrorCode::Num);
            }
            // Base 1 is Excel's division by ln(1) = 0, not a domain error.
            if (base == 1.0)
            {
                return FormulaValue::Error(FormulaErrorCode::Div0);
            }
            return FormulaValue::Number(std::log(x) / std::log(base));
        });
    Add(functions, "PI", 0, 0,
        [](FormulaEvaluationSession&, std::span<EvalValue>) -> FormulaValue
        {
            return FormulaValue::Number(3.14159265358979323846);
        });
    // Excel refuses trigonometric arguments of magnitude 2^27 and above with
    // #NUM!: beyond that the double's ulp exceeds the period and the result
    // would be numerically meaningless.
    constexpr Real kTrigonometricArgumentLimit = 134217728.0; // 2^27
    Add(functions, "SIN", 1, 1, Unary([](Real x)
                                      {
            if (std::fabs(x) >= kTrigonometricArgumentLimit)
            {
                return FormulaValue::Error(FormulaErrorCode::Num);
            }
            return FiniteNumber(std::sin(x)); }));
    Add(functions, "COS", 1, 1, Unary([](Real x)
                                      {
            if (std::fabs(x) >= kTrigonometricArgumentLimit)
            {
                return FormulaValue::Error(FormulaErrorCode::Num);
            }
            return FiniteNumber(std::cos(x)); }));
    Add(functions, "TAN", 1, 1, Unary([](Real x)
                                      {
            if (std::fabs(x) >= kTrigonometricArgumentLimit)
            {
                return FormulaValue::Error(FormulaErrorCode::Num);
            }
            return FiniteNumber(std::tan(x)); }));
    Add(functions, "ASIN", 1, 1, Unary([](Real x)
                                       {
            if (x < -1.0 || x > 1.0)
            {
                return FormulaValue::Error(FormulaErrorCode::Num);
            }
            return FormulaValue::Number(std::asin(x)); }));
    Add(functions, "ACOS", 1, 1, Unary([](Real x)
                                       {
            if (x < -1.0 || x > 1.0)
            {
                return FormulaValue::Error(FormulaErrorCode::Num);
            }
            return FormulaValue::Number(std::acos(x)); }));
    Add(functions, "ATAN", 1, 1, Unary([](Real x)
                                       { return FormulaValue::Number(std::atan(x)); }));
    Add(functions, "ATAN2", 2, 2, BinaryNumeric([](Real x, Real y)
                                                {
            if (x == 0.0 && y == 0.0)
            {
                return FormulaValue::Error(FormulaErrorCode::Div0);
            }
            return FormulaValue::Number(std::atan2(y, x)); }));
    Add(functions, "DEGREES", 1, 1,
        Unary([](Real x)
              { return FiniteNumber(x * 180.0 / 3.14159265358979323846); }));
    Add(functions, "RADIANS", 1, 1,
        Unary([](Real x)
              { return FormulaValue::Number(x * 3.14159265358979323846 / 180.0); }));
    Add(functions, "POWER", 2, 2, BinaryNumeric([](Real base, Real exponent)
                                                {
            if (base == 0.0 && exponent == 0.0)
            {
                return FormulaValue::Error(FormulaErrorCode::Num);
            }
            if (base == 0.0 && exponent < 0.0)
            {
                return FormulaValue::Error(FormulaErrorCode::Div0);
            }
            if (base < 0.0 && exponent != std::floor(exponent))
            {
                return FormulaValue::Error(FormulaErrorCode::Num);
            }
            return FiniteNumber(std::pow(base, exponent)); }));
    Add(functions, "MOD", 2, 2, BinaryNumeric([](Real number, Real divisor)
                                              {
            if (divisor == 0.0)
            {
                return FormulaValue::Error(FormulaErrorCode::Div0);
            }
            // Excel MOD takes the sign of the divisor.
            return FiniteNumber(number - divisor * std::floor(number / divisor)); }));
    Add(functions, "ROUND", 2, 2, BinaryNumeric([](Real number, Real digits)
                                                { return FiniteNumber(RoundHalfAwayFromZero(number, digits)); }));
    Add(functions, "ROUNDDOWN", 2, 2, BinaryNumeric([](Real number, Real digits)
                                                    { return FiniteNumber(RoundTowardZero(number, digits)); }));
    Add(functions, "ROUNDUP", 2, 2, BinaryNumeric([](Real number, Real digits)
                                                  { return FiniteNumber(RoundAwayFromZero(number, digits)); }));
    Add(functions, "TRUNC", 1, 2,
        [](FormulaEvaluationSession& session, std::span<EvalValue> arguments) -> FormulaValue
        {
            const FormulaValue number = Helpers::ScalarNumber(session, arguments[0]);
            if (number.IsError())
            {
                return number;
            }
            Real digits = 0.0;
            if (arguments.size() >= 2)
            {
                const FormulaValue digitsValue = Helpers::ScalarNumber(session, arguments[1]);
                if (digitsValue.IsError())
                {
                    return digitsValue;
                }
                digits = *digitsValue.NumberValue();
            }
            return FiniteNumber(RoundTowardZero(*number.NumberValue(), digits));
        });
    Add(functions, "CEILING", 2, 2, BinaryNumeric([](Real number, Real significance)
                                                  {
            if (significance == 0.0)
            {
                return FormulaValue::Number(0.0);
            }
            if (number > 0.0 && significance < 0.0)
            {
                return FormulaValue::Error(FormulaErrorCode::Num);
            }
            return FiniteNumber(std::ceil(number / significance) * significance); }));
    Add(functions, "FLOOR", 2, 2, BinaryNumeric([](Real number, Real significance)
                                                {
            if (significance == 0.0)
            {
                return FormulaValue::Error(FormulaErrorCode::Div0);
            }
            if (number > 0.0 && significance < 0.0)
            {
                return FormulaValue::Error(FormulaErrorCode::Num);
            }
            return FiniteNumber(std::floor(number / significance) * significance); }));
    Add(functions, "EVEN", 1, 1, Unary([](Real x)
                                       {
            const Real rounded = std::ceil(std::fabs(x) / 2.0) * 2.0;
            return FormulaValue::Number(x >= 0.0 ? rounded : -rounded); }));
    Add(functions, "ODD", 1, 1, Unary([](Real x)
                                      {
            Real magnitude = std::fabs(x);
            Real rounded = std::ceil((magnitude + 1.0) / 2.0) * 2.0 - 1.0;
            if (magnitude == 0.0)
            {
                rounded = 1.0;
            }
            return FormulaValue::Number(x >= 0.0 ? rounded : -rounded); }));
    Add(functions, "RAND", 0, 0, [](FormulaEvaluationSession&, std::span<EvalValue>) -> FormulaValue
        {
            std::uniform_real_distribution<Real> distribution(0.0, 1.0);
            return FormulaValue::Number(distribution(RandomEngine())); }, true);
    Add(functions, "RANDBETWEEN", 2, 2, [](FormulaEvaluationSession& session, std::span<EvalValue> arguments) -> FormulaValue
        {
            const FormulaValue lowValue = Helpers::ScalarNumber(session, arguments[0]);
            if (lowValue.IsError())
            {
                return lowValue;
            }
            const FormulaValue highValue = Helpers::ScalarNumber(session, arguments[1]);
            if (highValue.IsError())
            {
                return highValue;
            }
            const Real low = std::ceil(*lowValue.NumberValue());
            const Real high = std::floor(*highValue.NumberValue());
            if (low > high)
            {
                return FormulaValue::Error(FormulaErrorCode::Num);
            }
            // Bounds beyond the exactly representable integer range must not
            // reach the Int64 cast (undefined behavior); Excel's own integer
            // precision ends at 2^53, so such ranges draw in the real domain.
            constexpr Real kExactIntegerLimit = 9007199254740992.0; // 2^53
            if (low < -kExactIntegerLimit || high > kExactIntegerLimit)
            {
                std::uniform_real_distribution<Real> unit(0.0, 1.0);
                const Real drawn = low + std::floor(unit(RandomEngine()) * (high - low + 1.0));
                return FormulaValue::Number(std::min(drawn, high));
            }
            std::uniform_int_distribution<Int64> distribution(static_cast<Int64>(low),
                                                                  static_cast<Int64>(high));
            return FormulaValue::Number(static_cast<Real>(distribution(RandomEngine()))); }, true);

    Add(functions, "SUMPRODUCT", 1, 255,
        [](FormulaEvaluationSession& session, std::span<EvalValue> arguments) -> FormulaValue
        {
            std::vector<FormulaValue> matrices;
            matrices.reserve(arguments.size());
            for (const EvalValue& argument : arguments)
            {
                FormulaValue matrix = Helpers::Materialize(session, argument);
                if (matrix.IsError())
                {
                    return matrix;
                }
                matrices.push_back(std::move(matrix));
            }
            const Size rows = matrices.front().RowCount();
            const Size columns = matrices.front().ColumnCount();
            for (const FormulaValue& matrix : matrices)
            {
                if (matrix.RowCount() != rows || matrix.ColumnCount() != columns)
                {
                    return FormulaValue::Error(FormulaErrorCode::Value);
                }
            }
            Real sum = 0.0;
            for (Size row = 0; row < rows; ++row)
            {
                for (Size column = 0; column < columns; ++column)
                {
                    Real product = 1.0;
                    for (const FormulaValue& matrix : matrices)
                    {
                        const FormulaValue& element = matrix.At(row, column);
                        if (element.IsError())
                        {
                            return element;
                        }
                        product *= element.Kind() == FormulaValueKind::Number ? *element.NumberValue() : 0.0;
                    }
                    sum += product;
                }
            }
            return FiniteNumber(sum);
        });
}

// ---------------------------------------------------------------------------
// Logical functions
// ---------------------------------------------------------------------------

void FormulaFunctionLibrary::RegisterLogicalFunctions(FunctionMap& functions)
{
    using namespace FormulaFunctionDetail;
    using Helpers = FormulaFunctionHelpers;

    AddSpecialForm(functions, "IF", 2, 3, FormulaSpecialForm::If);
    AddSpecialForm(functions, "IFERROR", 2, 2, FormulaSpecialForm::IfError);
    AddSpecialForm(functions, "IFNA", 2, 2, FormulaSpecialForm::IfNa);
    AddSpecialForm(functions, "CHOOSE", 2, 255, FormulaSpecialForm::Choose);
    AddSpecialForm(functions, "IFS", 2, 255, FormulaSpecialForm::Ifs);
    AddSpecialForm(functions, "SWITCH", 3, 255, FormulaSpecialForm::Switch);

    enum class LogicalMode
    {
        And,
        Or,
        Xor
    };
    const auto logicalAggregate = [](LogicalMode mode)
    {
        return [mode](FormulaEvaluationSession& session, std::span<EvalValue> arguments) -> FormulaValue
        {
            bool andResult = true;
            bool orResult = false;
            Size trueCount = 0;
            bool any = false;
            FormulaValue error;
            bool hasError = false;
            for (const EvalValue& argument : arguments)
            {
                const auto failure = Helpers::ForEachValue(session, argument, [&](const FormulaValue& value)
                                                           {
                    if (hasError)
                    {
                        return;
                    }
                    if (value.IsError())
                    {
                        error = value;
                        hasError = true;
                        return;
                    }
                    // Text inside ranges is ignored; logical and numeric
                    // values participate.
                    if (value.Kind() == FormulaValueKind::Boolean || value.Kind() == FormulaValueKind::Number)
                    {
                        const bool truth = value.Kind() == FormulaValueKind::Boolean
                                               ? *value.BooleanValue()
                                               : *value.NumberValue() != 0.0;
                        andResult = andResult && truth;
                        orResult = orResult || truth;
                        trueCount += truth ? 1 : 0;
                        any = true;
                    }
                    // Text inside arrays is ignored like text inside ranges;
                    // only a direct scalar text argument is coerced.
                    else if (!argument.isReference &&
                             argument.value.Kind() != FormulaValueKind::Array &&
                             value.Kind() == FormulaValueKind::Text)
                    {
                        const FormulaValue coerced = FormulaCoercion::ToBoolean(value);
                        if (coerced.IsError())
                        {
                            error = coerced;
                            hasError = true;
                            return;
                        }
                        const bool truth = *coerced.BooleanValue();
                        andResult = andResult && truth;
                        orResult = orResult || truth;
                        trueCount += truth ? 1 : 0;
                        any = true;
                    } });
                if (failure)
                {
                    return *failure;
                }
                if (hasError)
                {
                    return error;
                }
            }
            if (!any)
            {
                return FormulaValue::Error(FormulaErrorCode::Value);
            }
            switch (mode)
            {
                case LogicalMode::And:
                    return FormulaValue::Boolean(andResult);
                case LogicalMode::Or:
                    return FormulaValue::Boolean(orResult);
                case LogicalMode::Xor:
                    return FormulaValue::Boolean(trueCount % 2 == 1);
            }
            return FormulaValue::Error(FormulaErrorCode::Value);
        };
    };
    Add(functions, "AND", 1, 255, logicalAggregate(LogicalMode::And));
    Add(functions, "OR", 1, 255, logicalAggregate(LogicalMode::Or));
    Add(functions, "XOR", 1, 255, logicalAggregate(LogicalMode::Xor));

    Add(functions, "NOT", 1, 1,
        [](FormulaEvaluationSession& session, std::span<EvalValue> arguments) -> FormulaValue
        {
            const FormulaValue value = Helpers::ScalarBoolean(session, arguments[0]);
            if (value.IsError())
            {
                return value;
            }
            return FormulaValue::Boolean(!*value.BooleanValue());
        });
    Add(functions, "TRUE", 0, 0,
        [](FormulaEvaluationSession&, std::span<EvalValue>) -> FormulaValue
        {
            return FormulaValue::Boolean(true);
        });
    Add(functions, "FALSE", 0, 0,
        [](FormulaEvaluationSession&, std::span<EvalValue>) -> FormulaValue
        {
            return FormulaValue::Boolean(false);
        });
}

// ---------------------------------------------------------------------------
// Information functions
// ---------------------------------------------------------------------------

void FormulaFunctionLibrary::RegisterInformationFunctions(FunctionMap& functions)
{
    using namespace FormulaFunctionDetail;
    using Helpers = FormulaFunctionHelpers;

    const auto classify = [](std::function<FormulaValue(const FormulaValue&)> body)
    {
        return [body = std::move(body)](FormulaEvaluationSession& session,
                                        std::span<EvalValue> arguments) -> FormulaValue
        {
            return body(Helpers::Scalar(session, arguments[0]));
        };
    };

    Add(functions, "ISBLANK", 1, 1, classify([](const FormulaValue& value)
                                             { return FormulaValue::Boolean(value.Kind() == FormulaValueKind::Blank); }));
    Add(functions, "ISERROR", 1, 1, classify([](const FormulaValue& value)
                                             { return FormulaValue::Boolean(value.IsError()); }));
    Add(functions, "ISERR", 1, 1, classify([](const FormulaValue& value)
                                           { return FormulaValue::Boolean(value.IsError() && value.ErrorCode() != FormulaErrorCode::NA); }));
    Add(functions, "ISNA", 1, 1, classify([](const FormulaValue& value)
                                          { return FormulaValue::Boolean(value.IsError() && value.ErrorCode() == FormulaErrorCode::NA); }));
    Add(functions, "ISNUMBER", 1, 1, classify([](const FormulaValue& value)
                                              { return FormulaValue::Boolean(value.Kind() == FormulaValueKind::Number); }));
    Add(functions, "ISTEXT", 1, 1, classify([](const FormulaValue& value)
                                            { return FormulaValue::Boolean(value.Kind() == FormulaValueKind::Text); }));
    Add(functions, "ISLOGICAL", 1, 1, classify([](const FormulaValue& value)
                                               { return FormulaValue::Boolean(value.Kind() == FormulaValueKind::Boolean); }));
    Add(functions, "ISEVEN", 1, 1,
        [](FormulaEvaluationSession& session, std::span<EvalValue> arguments) -> FormulaValue
        {
            const FormulaValue value = Helpers::ScalarNumber(session, arguments[0]);
            if (value.IsError())
            {
                return value;
            }
            const Real truncated = std::trunc(*value.NumberValue());
            return FormulaValue::Boolean(std::fmod(std::fabs(truncated), 2.0) == 0.0);
        });
    Add(functions, "ISODD", 1, 1,
        [](FormulaEvaluationSession& session, std::span<EvalValue> arguments) -> FormulaValue
        {
            const FormulaValue value = Helpers::ScalarNumber(session, arguments[0]);
            if (value.IsError())
            {
                return value;
            }
            const Real truncated = std::trunc(*value.NumberValue());
            return FormulaValue::Boolean(std::fmod(std::fabs(truncated), 2.0) == 1.0);
        });
    Add(functions, "N", 1, 1, classify([](const FormulaValue& value) -> FormulaValue
                                       {
            switch (value.Kind())
            {
                case FormulaValueKind::Number: return value;
                case FormulaValueKind::Boolean: return FormulaValue::Number(*value.BooleanValue() ? 1.0 : 0.0);
                case FormulaValueKind::Error: return value;
                default: return FormulaValue::Number(0.0);
            } }));
    Add(functions, "NA", 0, 0,
        [](FormulaEvaluationSession&, std::span<EvalValue>) -> FormulaValue
        {
            return FormulaValue::Error(FormulaErrorCode::NA);
        });
}

// ---------------------------------------------------------------------------
// Statistical functions
// ---------------------------------------------------------------------------

void FormulaFunctionLibrary::RegisterStatisticalFunctions(FunctionMap& functions)
{
    using namespace FormulaFunctionDetail;
    using Helpers = FormulaFunctionHelpers;

    Add(functions, "AVERAGE", 1, 255,
        [](FormulaEvaluationSession& session, std::span<EvalValue> arguments) -> FormulaValue
        {
            Real sum = 0.0;
            Size count = 0;
            if (const auto error = Helpers::ForEachNumber(session, arguments, [&](Real value)
                                                          {
                    sum += value;
                    ++count; }))
            {
                return *error;
            }
            if (count == 0)
            {
                return FormulaValue::Error(FormulaErrorCode::Div0);
            }
            return FiniteNumber(sum / static_cast<Real>(count));
        });

    Add(functions, "AVERAGEA", 1, 255,
        [](FormulaEvaluationSession& session, std::span<EvalValue> arguments) -> FormulaValue
        {
            Real sum = 0.0;
            Size count = 0;
            FormulaValue error;
            bool hasError = false;
            for (const EvalValue& argument : arguments)
            {
                const auto failure = Helpers::ForEachValue(session, argument, [&](const FormulaValue& value)
                                                           {
                    if (hasError)
                    {
                        return;
                    }
                    switch (value.Kind())
                    {
                        case FormulaValueKind::Error:
                            error = value;
                            hasError = true;
                            break;
                        case FormulaValueKind::Number:
                            sum += *value.NumberValue();
                            ++count;
                            break;
                        case FormulaValueKind::Boolean:
                            sum += *value.BooleanValue() ? 1.0 : 0.0;
                            ++count;
                            break;
                        case FormulaValueKind::Text:
                            // Text in a reference or array counts as zero; a
                            // direct text argument follows the scalar rules -
                            // numeric text contributes its value, anything
                            // else is #VALUE!.
                            if (!argument.isReference &&
                                argument.value.Kind() != FormulaValueKind::Array)
                            {
                                if (const auto parsed =
                                        FormulaCoercion::ParseNumberText(value.TextValue()))
                                {
                                    sum += *parsed;
                                    ++count;
                                }
                                else
                                {
                                    error = FormulaValue::Error(FormulaErrorCode::Value);
                                    hasError = true;
                                }
                                break;
                            }
                            ++count;
                            break;
                        default: break;
                    } });
                if (failure)
                {
                    return *failure;
                }
                if (hasError)
                {
                    return error;
                }
            }
            if (count == 0)
            {
                return FormulaValue::Error(FormulaErrorCode::Div0);
            }
            return FiniteNumber(sum / static_cast<Real>(count));
        });

    Add(functions, "COUNT", 1, 255,
        [](FormulaEvaluationSession& session, std::span<EvalValue> arguments) -> FormulaValue
        {
            Size count = 0;
            for (const EvalValue& argument : arguments)
            {
                if (argument.isReference || argument.value.Kind() == FormulaValueKind::Array)
                {
                    const auto failure =
                        Helpers::ForEachValue(session, argument, [&](const FormulaValue& value)
                                              {
                            if (value.Kind() == FormulaValueKind::Number)
                            {
                                ++count;
                            } });
                    if (failure)
                    {
                        return *failure;
                    }
                }
                else
                {
                    // Direct arguments count when they are number-convertible.
                    const FormulaValue& value = argument.value;
                    if (value.Kind() == FormulaValueKind::Number ||
                        value.Kind() == FormulaValueKind::Boolean ||
                        (value.Kind() == FormulaValueKind::Text &&
                         FormulaCoercion::ParseNumberText(value.TextValue())))
                    {
                        ++count;
                    }
                }
            }
            return FormulaValue::Number(static_cast<Real>(count));
        });

    Add(functions, "COUNTA", 1, 255,
        [](FormulaEvaluationSession& session, std::span<EvalValue> arguments) -> FormulaValue
        {
            Size count = 0;
            for (const EvalValue& argument : arguments)
            {
                const auto failure = Helpers::ForEachValue(session, argument, [&](const FormulaValue& value)
                                                           {
                    if (value.Kind() != FormulaValueKind::Blank)
                    {
                        ++count;
                    } });
                if (failure)
                {
                    return *failure;
                }
            }
            return FormulaValue::Number(static_cast<Real>(count));
        });

    Add(functions, "COUNTBLANK", 1, 1,
        [](FormulaEvaluationSession& session, std::span<EvalValue> arguments) -> FormulaValue
        {
            if (!arguments[0].isReference)
            {
                return FormulaValue::Error(FormulaErrorCode::Value);
            }
            UInt64 totalCells = 0;
            UInt64 nonBlank = 0;
            for (const ResolvedReferenceArea& area : arguments[0].areas)
            {
                totalCells += static_cast<UInt64>(area.RowCount()) * area.ColumnCount();
            }
            const auto failure =
                Helpers::ForEachCell(session, arguments[0],
                                     [&](UInt32, UInt32, const FormulaValue& value)
                                     {
                                         const bool isBlank =
                                             value.Kind() == FormulaValueKind::Blank ||
                                             (value.Kind() == FormulaValueKind::Text && value.TextValue().empty());
                                         if (!isBlank)
                                         {
                                             ++nonBlank;
                                         }
                                     });
            if (failure)
            {
                return *failure;
            }
            return FormulaValue::Number(static_cast<Real>(totalCells - nonBlank));
        });

    const auto extremum = [](bool isMax)
    {
        return [isMax](FormulaEvaluationSession& session, std::span<EvalValue> arguments) -> FormulaValue
        {
            std::optional<Real> best;
            if (const auto error = FormulaFunctionHelpers::ForEachNumber(session, arguments, [&](Real value)
                                                                         {
                    if (!best || (isMax ? value > *best : value < *best))
                    {
                        best = value;
                    } }))
            {
                return *error;
            }
            return FormulaValue::Number(best.value_or(0.0));
        };
    };
    Add(functions, "MAX", 1, 255, extremum(true));
    Add(functions, "MIN", 1, 255, extremum(false));

    Add(functions, "MEDIAN", 1, 255,
        [](FormulaEvaluationSession& session, std::span<EvalValue> arguments) -> FormulaValue
        {
            std::vector<Real> numbers;
            if (const auto error = CollectNumbers(session, arguments, numbers))
            {
                return *error;
            }
            if (numbers.empty())
            {
                return FormulaValue::Error(FormulaErrorCode::Num);
            }
            std::sort(numbers.begin(), numbers.end());
            const Size middle = numbers.size() / 2;
            if (numbers.size() % 2 == 1)
            {
                return FormulaValue::Number(numbers[middle]);
            }
            return FiniteNumber((numbers[middle - 1] + numbers[middle]) / 2.0);
        });

    Add(functions, "MODE", 1, 255,
        [](FormulaEvaluationSession& session, std::span<EvalValue> arguments) -> FormulaValue
        {
            std::vector<Real> numbers;
            if (const auto error = CollectNumbers(session, arguments, numbers))
            {
                return *error;
            }
            std::sort(numbers.begin(), numbers.end());
            std::optional<Real> best;
            Size bestCount = 1;
            Size index = 0;
            while (index < numbers.size())
            {
                Size runEnd = index;
                while (runEnd < numbers.size() && numbers[runEnd] == numbers[index])
                {
                    ++runEnd;
                }
                const Size runLength = runEnd - index;
                if (runLength > bestCount)
                {
                    bestCount = runLength;
                    best = numbers[index];
                }
                index = runEnd;
            }
            if (!best)
            {
                return FormulaValue::Error(FormulaErrorCode::NA);
            }
            return FormulaValue::Number(*best);
        });

    const auto ranked = [](bool isLarge)
    {
        return [isLarge](FormulaEvaluationSession& session, std::span<EvalValue> arguments) -> FormulaValue
        {
            std::vector<Real> numbers;
            std::span<EvalValue> dataArguments = arguments.subspan(0, 1);
            if (const auto error = CollectNumbers(session, dataArguments, numbers))
            {
                return *error;
            }
            const FormulaValue rank = FormulaFunctionHelpers::ScalarNumber(session, arguments[1]);
            if (rank.IsError())
            {
                return rank;
            }
            const Real k = std::floor(*rank.NumberValue());
            if (numbers.empty() || k < 1.0 || k > static_cast<Real>(numbers.size()))
            {
                return FormulaValue::Error(FormulaErrorCode::Num);
            }
            std::sort(numbers.begin(), numbers.end());
            const Size index = static_cast<Size>(k) - 1;
            return FormulaValue::Number(isLarge ? numbers[numbers.size() - 1 - index] : numbers[index]);
        };
    };
    Add(functions, "LARGE", 2, 2, ranked(true));
    Add(functions, "SMALL", 2, 2, ranked(false));

    enum class SpreadMode
    {
        SampleStdDev,
        PopulationStdDev,
        SampleVariance,
        PopulationVariance
    };
    const auto spread = [](SpreadMode mode)
    {
        return [mode](FormulaEvaluationSession& session, std::span<EvalValue> arguments) -> FormulaValue
        {
            std::vector<Real> numbers;
            if (const auto error = CollectNumbers(session, arguments, numbers))
            {
                return *error;
            }
            const bool sample = mode == SpreadMode::SampleStdDev || mode == SpreadMode::SampleVariance;
            if (numbers.size() < (sample ? 2u : 1u))
            {
                return FormulaValue::Error(FormulaErrorCode::Div0);
            }
            Real mean = 0.0;
            for (const Real value : numbers)
            {
                mean += value;
            }
            mean /= static_cast<Real>(numbers.size());
            Real sumOfSquares = 0.0;
            for (const Real value : numbers)
            {
                sumOfSquares += (value - mean) * (value - mean);
            }
            const Real divisor = static_cast<Real>(sample ? numbers.size() - 1 : numbers.size());
            const Real variance = sumOfSquares / divisor;
            const bool stdDev = mode == SpreadMode::SampleStdDev || mode == SpreadMode::PopulationStdDev;
            return FiniteNumber(stdDev ? std::sqrt(variance) : variance);
        };
    };
    Add(functions, "STDEV", 1, 255, spread(SpreadMode::SampleStdDev));
    Add(functions, "STDEVP", 1, 255, spread(SpreadMode::PopulationStdDev));
    Add(functions, "VAR", 1, 255, spread(SpreadMode::SampleVariance));
    Add(functions, "VARP", 1, 255, spread(SpreadMode::PopulationVariance));
}

// ---------------------------------------------------------------------------
// Conditional aggregation
// ---------------------------------------------------------------------------

void FormulaFunctionLibrary::RegisterConditionalAggregationFunctions(FunctionMap& functions)
{
    using namespace FormulaFunctionDetail;

    Add(functions, "COUNTIF", 2, 2,
        [](FormulaEvaluationSession& session, std::span<EvalValue> arguments) -> FormulaValue
        {
            return EvaluateSingleCriteria(session, arguments, SingleCriteriaMode::Count);
        });
    Add(functions, "SUMIF", 2, 3,
        [](FormulaEvaluationSession& session, std::span<EvalValue> arguments) -> FormulaValue
        {
            return EvaluateSingleCriteria(session, arguments, SingleCriteriaMode::Sum);
        });
    Add(functions, "AVERAGEIF", 2, 3,
        [](FormulaEvaluationSession& session, std::span<EvalValue> arguments) -> FormulaValue
        {
            return EvaluateSingleCriteria(session, arguments, SingleCriteriaMode::Average);
        });
    Add(functions, "COUNTIFS", 2, 255,
        [](FormulaEvaluationSession& session, std::span<EvalValue> arguments) -> FormulaValue
        {
            return EvaluateMultiCriteria(session, arguments, MultiCriteriaMode::Count);
        });
    Add(functions, "SUMIFS", 3, 255,
        [](FormulaEvaluationSession& session, std::span<EvalValue> arguments) -> FormulaValue
        {
            return EvaluateMultiCriteria(session, arguments, MultiCriteriaMode::Sum);
        });
    Add(functions, "AVERAGEIFS", 3, 255,
        [](FormulaEvaluationSession& session, std::span<EvalValue> arguments) -> FormulaValue
        {
            return EvaluateMultiCriteria(session, arguments, MultiCriteriaMode::Average);
        });
}

} // namespace ExyokiOffice::Excel
