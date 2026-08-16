// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "ExyokiOffice/Excel/ExcelCellValue.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <charconv>

namespace ExyokiOffice::Excel
{
/// File-local parsing helpers for cell values.
class ExcelCellValueHelper
{
public:
    static std::string FormatDouble(Real value)
    {
        // 64 bytes is past the 24 the shortest round-trip form of a double ever
        // needs, so the conversion cannot report value_too_large; an empty
        // string on a failure that cannot happen beats a locale-dependent
        // fallback that would quietly write `1,5` into the file.
        char buffer[64]{};
        const auto [ptr, ec] = std::to_chars(buffer, buffer + sizeof(buffer), value);
        return ec == std::errc() ? std::string(buffer, ptr) : std::string();
    }

    static std::string StripFormulaPrefix(std::string formula)
    {
        if (!formula.empty() && formula.front() == '=')
        {
            formula.erase(formula.begin());
        }
        return formula;
    }
};

CellFormulaValue CellFormulaValue::Normal(std::string formula,
                                          FormulaCachedValueKind cachedKind,
                                          std::string cachedText,
                                          FormulaReferenceStyle referenceStyle)
{
    CellFormulaValue value;
    value.Formula = ExcelCellValueHelper::StripFormulaPrefix(std::move(formula));
    value.ReferenceStyle = referenceStyle;
    value.CachedKind = cachedKind;
    value.CachedText = std::move(cachedText);
    return value;
}

CellFormulaValue CellFormulaValue::Shared(std::string formula,
                                          UInt32 sharedIndex,
                                          std::string reference,
                                          FormulaCachedValueKind cachedKind,
                                          std::string cachedText,
                                          FormulaReferenceStyle referenceStyle)
{
    auto value = Normal(std::move(formula), cachedKind, std::move(cachedText), referenceStyle);
    value.Kind = CellFormulaKind::Shared;
    value.Reference = std::move(reference);
    value.SharedIndex = sharedIndex;
    return value;
}

CellFormulaValue CellFormulaValue::SharedDependent(UInt32 sharedIndex,
                                                   FormulaCachedValueKind cachedKind,
                                                   std::string cachedText,
                                                   FormulaReferenceStyle referenceStyle)
{
    auto value = Normal({}, cachedKind, std::move(cachedText), referenceStyle);
    value.Kind = CellFormulaKind::Shared;
    value.SharedIndex = sharedIndex;
    return value;
}

CellFormulaValue CellFormulaValue::Array(std::string formula,
                                         std::string reference,
                                         FormulaCachedValueKind cachedKind,
                                         std::string cachedText,
                                         bool alwaysCalculate,
                                         FormulaReferenceStyle referenceStyle)
{
    auto value = Normal(std::move(formula), cachedKind, std::move(cachedText), referenceStyle);
    value.Kind = CellFormulaKind::Array;
    value.Reference = std::move(reference);
    value.AlwaysCalculateArray = alwaysCalculate;
    return value;
}

ExcelCellValue ExcelCellValue::Blank()
{
    return {};
}

ExcelCellValue ExcelCellValue::InlineString(std::string text)
{
    ExcelCellValue value;
    value.m_kind = CellValueKind::InlineString;
    value.m_text = std::move(text);
    return value;
}

ExcelCellValue ExcelCellValue::SharedString(UInt32 sharedStringIndex)
{
    ExcelCellValue value;
    value.m_kind = CellValueKind::SharedString;
    value.m_sharedStringIndex = sharedStringIndex;
    value.m_text = std::to_string(sharedStringIndex);
    return value;
}

ExcelCellValue ExcelCellValue::NumberText(std::string text)
{
    ExcelCellValue value;
    value.m_kind = CellValueKind::Number;
    value.m_text = std::move(text);
    return value;
}

ExcelCellValue ExcelCellValue::Number(Real value)
{
    return NumberText(ExcelCellValueHelper::FormatDouble(value));
}

ExcelCellValue ExcelCellValue::Boolean(bool value)
{
    ExcelCellValue cellValue;
    cellValue.m_kind = CellValueKind::Boolean;
    cellValue.m_booleanValue = value;
    cellValue.m_text = value ? "1" : "0";
    return cellValue;
}

ExcelCellValue ExcelCellValue::Error(std::string errorText)
{
    ExcelCellValue value;
    value.m_kind = CellValueKind::Error;
    value.m_text = std::move(errorText);
    return value;
}

ExcelCellValue ExcelCellValue::DateTimeText(std::string text)
{
    ExcelCellValue value;
    value.m_kind = CellValueKind::DateTime;
    value.m_text = std::move(text);
    return value;
}

ExcelCellValue ExcelCellValue::Formula(std::string formula,
                                       FormulaCachedValueKind cachedKind,
                                       std::string cachedText)
{
    return Formula(CellFormulaValue::Normal(std::move(formula), cachedKind, std::move(cachedText)));
}

ExcelCellValue ExcelCellValue::Formula(CellFormulaValue formula)
{
    ExcelCellValue value;
    value.m_kind = CellValueKind::Formula;
    formula.Formula = ExcelCellValueHelper::StripFormulaPrefix(std::move(formula.Formula));
    value.m_formula = std::move(formula);
    value.m_text = value.m_formula.CachedText;
    return value;
}

CellValueKind ExcelCellValue::Kind() const noexcept
{
    return m_kind;
}

bool ExcelCellValue::IsBlank() const noexcept
{
    return m_kind == CellValueKind::Blank;
}

const std::string& ExcelCellValue::Text() const noexcept
{
    return m_text;
}

std::optional<UInt32> ExcelCellValue::SharedStringIndex() const noexcept
{
    return m_sharedStringIndex;
}

std::optional<bool> ExcelCellValue::BooleanValue() const noexcept
{
    return m_booleanValue;
}

const CellFormulaValue& ExcelCellValue::FormulaValue() const noexcept
{
    return m_formula;
}

} // namespace ExyokiOffice::Excel
