// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "ExyokiOffice/Excel/ExcelFormulaEngine.hpp"

#include "FormulaEvaluator.hpp"
#include "FormulaFunctions.hpp"
#include "FormulaParser.hpp"

#include "ExyokiOffice/Excel/ExcelNamedRange.hpp"
#include "ExyokiOffice/Packaging/GeneratedParts.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include "AsciiText.hpp"

#include <algorithm>
#include <array>
#include <map>
#include <set>
#include <unordered_map>
#include <utility>

namespace ExyokiOffice::Excel
{

// ---------------------------------------------------------------------------
// Error literals
// ---------------------------------------------------------------------------

std::string_view FormulaErrorText(FormulaErrorCode code) noexcept
{
    switch (code)
    {
        case FormulaErrorCode::None:
            return {};
        case FormulaErrorCode::Null:
            return "#NULL!";
        case FormulaErrorCode::Div0:
            return "#DIV/0!";
        case FormulaErrorCode::Value:
            return "#VALUE!";
        case FormulaErrorCode::Ref:
            return "#REF!";
        case FormulaErrorCode::Name:
            return "#NAME?";
        case FormulaErrorCode::Num:
            return "#NUM!";
        case FormulaErrorCode::NA:
            return "#N/A";
    }
    return {};
}

std::optional<FormulaErrorCode> ParseFormulaErrorText(std::string_view text) noexcept
{
    static constexpr std::array<std::pair<std::string_view, FormulaErrorCode>, 7> literals = {{
        {"#NULL!", FormulaErrorCode::Null},
        {"#DIV/0!", FormulaErrorCode::Div0},
        {"#VALUE!", FormulaErrorCode::Value},
        {"#REF!", FormulaErrorCode::Ref},
        {"#NAME?", FormulaErrorCode::Name},
        {"#NUM!", FormulaErrorCode::Num},
        {"#N/A", FormulaErrorCode::NA},
    }};
    for (const auto& [literal, code] : literals)
    {
        if (text == literal)
        {
            return code;
        }
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// FormulaValue
// ---------------------------------------------------------------------------

FormulaValue FormulaValue::Number(Real value)
{
    FormulaValue result;
    result.m_kind = FormulaValueKind::Number;
    result.m_number = value;
    return result;
}

FormulaValue FormulaValue::Text(std::string text)
{
    FormulaValue result;
    result.m_kind = FormulaValueKind::Text;
    result.m_text = std::move(text);
    return result;
}

FormulaValue FormulaValue::Boolean(bool value)
{
    FormulaValue result;
    result.m_kind = FormulaValueKind::Boolean;
    result.m_boolean = value;
    return result;
}

FormulaValue FormulaValue::Error(FormulaErrorCode code)
{
    FormulaValue result;
    result.m_kind = FormulaValueKind::Error;
    result.m_error = code == FormulaErrorCode::None ? FormulaErrorCode::Value : code;
    return result;
}

FormulaValue FormulaValue::Array(Size rowCount,
                                 Size columnCount,
                                 std::vector<FormulaValue> rowMajorValues)
{
    if (rowCount == 0 || columnCount == 0 || rowMajorValues.size() != rowCount * columnCount)
    {
        return Error(FormulaErrorCode::Value);
    }
    for (const FormulaValue& element : rowMajorValues)
    {
        if (element.Kind() == FormulaValueKind::Array)
        {
            return Error(FormulaErrorCode::Value);
        }
    }
    FormulaValue result;
    result.m_kind = FormulaValueKind::Array;
    result.m_rowCount = rowCount;
    result.m_columnCount = columnCount;
    result.m_elements = std::make_shared<const std::vector<FormulaValue>>(std::move(rowMajorValues));
    return result;
}

FormulaValueKind FormulaValue::Kind() const noexcept
{
    return m_kind;
}

bool FormulaValue::IsError() const noexcept
{
    return m_kind == FormulaValueKind::Error;
}

std::optional<Real> FormulaValue::NumberValue() const noexcept
{
    if (m_kind != FormulaValueKind::Number)
    {
        return std::nullopt;
    }
    return m_number;
}

const std::string& FormulaValue::TextValue() const noexcept
{
    static const std::string empty;
    return m_kind == FormulaValueKind::Text ? m_text : empty;
}

std::optional<bool> FormulaValue::BooleanValue() const noexcept
{
    if (m_kind != FormulaValueKind::Boolean)
    {
        return std::nullopt;
    }
    return m_boolean;
}

FormulaErrorCode FormulaValue::ErrorCode() const noexcept
{
    return m_kind == FormulaValueKind::Error ? m_error : FormulaErrorCode::None;
}

Size FormulaValue::RowCount() const noexcept
{
    return m_kind == FormulaValueKind::Array ? m_rowCount : 1;
}

Size FormulaValue::ColumnCount() const noexcept
{
    return m_kind == FormulaValueKind::Array ? m_columnCount : 1;
}

const FormulaValue& FormulaValue::At(Size row, Size column) const noexcept
{
    static const FormulaValue outOfRange = FormulaValue::Error(FormulaErrorCode::Ref);
    if (m_kind != FormulaValueKind::Array)
    {
        return (row == 0 && column == 0) ? *this : outOfRange;
    }
    if (row >= m_rowCount || column >= m_columnCount || !m_elements)
    {
        return outOfRange;
    }
    return (*m_elements)[row * m_columnCount + column];
}

ExcelCellValue FormulaValue::ToCellValue() const
{
    switch (m_kind)
    {
        case FormulaValueKind::Blank:
            return ExcelCellValue::Blank();
        case FormulaValueKind::Number:
            return ExcelCellValue::Number(m_number);
        case FormulaValueKind::Text:
            return ExcelCellValue::InlineString(m_text);
        case FormulaValueKind::Boolean:
            return ExcelCellValue::Boolean(m_boolean);
        case FormulaValueKind::Error:
            return ExcelCellValue::Error(std::string(FormulaErrorText(m_error)));
        case FormulaValueKind::Array:
            return At(0, 0).ToCellValue();
    }
    return ExcelCellValue::Blank();
}

std::string FormulaValue::ToDisplayText() const
{
    switch (m_kind)
    {
        case FormulaValueKind::Blank:
            return {};
        case FormulaValueKind::Number:
            return FormulaCoercion::FormatNumber(m_number);
        case FormulaValueKind::Text:
            return m_text;
        case FormulaValueKind::Boolean:
            return m_boolean ? "TRUE" : "FALSE";
        case FormulaValueKind::Error:
            return std::string(FormulaErrorText(m_error));
        case FormulaValueKind::Array:
            return At(0, 0).ToDisplayText();
    }
    return {};
}

// ---------------------------------------------------------------------------
// SheetCellAddress
// ---------------------------------------------------------------------------

namespace FormulaEngineDetail
{

bool SheetNameNeedsQuoting(std::string_view name)
{
    if (name.empty())
    {
        return true;
    }
    const char first = name.front();
    if ((first >= '0' && first <= '9') || first == '.')
    {
        return true;
    }
    for (const char c : name)
    {
        const bool safe = AsciiText::IsAlnum(c) || AsciiText::IsNonAscii(c) || c == '_' || c == '.';
        if (!safe)
        {
            return true;
        }
    }
    // Names that parse as cell references (like "A1") must be quoted.
    if (CellAddress::ParseA1(name))
    {
        return true;
    }
    return false;
}

std::string QuoteSheetName(const std::string& name)
{
    if (!SheetNameNeedsQuoting(name))
    {
        return name;
    }
    std::string quoted = "'";
    for (const char c : name)
    {
        if (c == '\'')
        {
            quoted += "''";
        }
        else
        {
            quoted.push_back(c);
        }
    }
    quoted.push_back('\'');
    return quoted;
}

/** Converts a computed value to the stored cached-result representation. */
void ToCachedResult(const FormulaValue& value, FormulaCachedValueKind& kind, std::string& text)
{
    switch (value.Kind())
    {
        case FormulaValueKind::Blank:
            kind = FormulaCachedValueKind::Number;
            text = "0";
            break;
        case FormulaValueKind::Number:
            kind = FormulaCachedValueKind::Number;
            text = FormulaCoercion::FormatNumber(*value.NumberValue());
            break;
        case FormulaValueKind::Text:
            kind = FormulaCachedValueKind::String;
            text = value.TextValue();
            break;
        case FormulaValueKind::Boolean:
            kind = FormulaCachedValueKind::Boolean;
            text = *value.BooleanValue() ? "1" : "0";
            break;
        case FormulaValueKind::Error:
            kind = FormulaCachedValueKind::Error;
            text = std::string(FormulaErrorText(value.ErrorCode()));
            break;
        case FormulaValueKind::Array:
            ToCachedResult(value.At(0, 0), kind, text);
            break;
    }
}

/** One formula cell prepared for dependency analysis and evaluation. */
struct FormulaCellNode
{
    std::string sheetLower;
    std::string sheetDisplay;
    CellAddress address;
    CellFormulaValue model;
    /** Effective expression tree; null when parsing failed. */
    const FormulaExpression* ast = nullptr;
    Int64 rowOffset = 0;
    Int64 columnOffset = 0;
    bool isArrayFormula = false;
    CellRange arrayRange;
    /** Contains a volatile function or a dynamic reference (OFFSET/INDIRECT). */
    bool dynamic = false;
    bool skip = false;
};

/** Per-sheet index of formula cells sorted by row and column. */
struct SheetNodeIndex
{
    /** (row, column, nodeIndex) sorted ascending. */
    std::vector<std::tuple<UInt32, UInt32, Size>> cells;
};

struct DependencyGraph
{
    std::vector<FormulaCellNode> nodes;
    /** adjacency[i] lists nodes that depend on node i. */
    std::vector<std::vector<Size>> dependents;
    std::vector<Size> indegree;
    /** Nodes that participate in a cycle. */
    std::vector<bool> cyclic;
    std::vector<std::vector<Size>> cycles;
    /** Evaluation order of acyclic nodes. */
    std::vector<Size> order;
    bool anyDynamic = false;
};

/** Applies a shared-formula offset to a reference coordinate. */
std::optional<UInt32> OffsetCoordinate(const FormulaCoordinate& coordinate,
                                       Int64 offset,
                                       UInt32 maximum)
{
    Int64 value = coordinate.value;
    if (!coordinate.absolute)
    {
        value += offset;
    }
    if (value < 1 || value > maximum)
    {
        return std::nullopt;
    }
    return static_cast<UInt32>(value);
}

/** Sheet context and shared-formula offsets used while collecting precedents. */
struct PrecedentContext
{
    std::string sheetDisplay;
    Int64 rowOffset = 0;
    Int64 columnOffset = 0;
};

/** (lower-case scope sheet or empty, lower-case name) to parsed definition. */
using NameAstMap = std::map<std::pair<std::string, std::string>, const FormulaExpression*>;

/**
 * @brief Largest chain of defined names that is expanded.
 *
 * Name expansion is the one axis here that recurses, and a workbook can chain
 * as many names as it likes (`n1` refers to `n2` refers to `n3`...). The cycle
 * guard stops a loop but not a long chain, so the depth is capped as well.
 */
constexpr Size MaxNameExpansionDepth = 64;

/** Collects the statically known precedent areas of an expression tree. */
void CollectPrecedents(const FormulaExpression& root,
                       const PrecedentContext& context,
                       const FormulaFunctionRegistry& registry,
                       const NameAstMap& names,
                       std::vector<std::string>& nameStack,
                       std::vector<ResolvedReferenceArea>& areas,
                       bool& dynamic)
{
    // Walk the tree with an explicit stack. Tree depth is not bounded by the
    // parser's nesting limit: chained operators such as `A1+A2+...+A400` build
    // a tree as deep as the formula is long, and recursing per node would put
    // the call stack at the mercy of the workbook. Children are pushed in
    // reverse so that areas are still collected in document order.
    std::vector<const FormulaExpression*> pending;
    pending.push_back(&root);

    while (!pending.empty())
    {
        const FormulaExpression& node = *pending.back();
        pending.pop_back();

        switch (node.kind)
        {
            case FormulaExpressionKind::Reference:
            {
                if (node.area.external)
                {
                    break;
                }
                const auto firstRow = OffsetCoordinate(node.area.firstRow, context.rowOffset, MaxRowIndex);
                const auto lastRow = OffsetCoordinate(node.area.lastRow, context.rowOffset, MaxRowIndex);
                const auto firstColumn =
                    OffsetCoordinate(node.area.firstColumn, context.columnOffset, MaxColumnIndex);
                const auto lastColumn =
                    OffsetCoordinate(node.area.lastColumn, context.columnOffset, MaxColumnIndex);
                if (!firstRow || !lastRow || !firstColumn || !lastColumn)
                {
                    break;
                }
                ResolvedReferenceArea area;
                area.sheet = node.area.hasSheet ? node.area.sheet : context.sheetDisplay;
                area.firstRow = std::min(*firstRow, *lastRow);
                area.lastRow = std::max(*firstRow, *lastRow);
                area.firstColumn = std::min(*firstColumn, *lastColumn);
                area.lastColumn = std::max(*firstColumn, *lastColumn);
                areas.push_back(std::move(area));
                break;
            }
            case FormulaExpressionKind::NameReference:
            {
                // Expand the name's definition so dependencies flow through
                // defined names. The stack guards against name-to-name cycles.
                const std::string nameLower = AsciiText::ToLower(node.text);
                const std::string scopeLower =
                    AsciiText::ToLower(node.area.hasSheet ? std::string_view(node.area.sheet)
                                                          : std::string_view(context.sheetDisplay));
                auto it = names.find({scopeLower, nameLower});
                if (it == names.end())
                {
                    it = names.find({std::string(), nameLower});
                }
                if (nameStack.size() >= MaxNameExpansionDepth)
                {
                    // The chain is longer than anything a workbook needs. Stop
                    // expanding, and report the formula as dynamic so that it is
                    // recalculated rather than trusted to a truncated precedent set.
                    dynamic = true;
                    break;
                }
                if (it != names.end() && it->second != nullptr &&
                    std::find(nameStack.begin(), nameStack.end(), nameLower) == nameStack.end())
                {
                    nameStack.push_back(nameLower);
                    // Name definitions are self-contained; shared offsets reset.
                    PrecedentContext nameContext{context.sheetDisplay, 0, 0};
                    CollectPrecedents(*it->second, nameContext, registry, names, nameStack, areas, dynamic);
                    nameStack.pop_back();
                }
                break;
            }
            case FormulaExpressionKind::FunctionCall:
            {
                const RegisteredFormulaFunction* function = registry.Find(node.text);
                if (function && function->spec.IsVolatile)
                {
                    dynamic = true;
                }
                if (node.text == "INDIRECT" || node.text == "OFFSET")
                {
                    dynamic = true;
                }
                break;
            }
            default:
                break;
        }

        for (auto child = node.children.rbegin(); child != node.children.rend(); ++child)
        {
            if (*child)
            {
                pending.push_back(child->get());
            }
        }
    }
}

} // namespace FormulaEngineDetail

std::string SheetCellAddress::ToFormula() const
{
    if (!Address.IsValid())
    {
        return {};
    }
    return FormulaEngineDetail::QuoteSheetName(Sheet) + "!" + Address.ToA1();
}

// ---------------------------------------------------------------------------
// FormulaEngine
// ---------------------------------------------------------------------------

FormulaEngine::FormulaEngine() : m_registry(std::make_shared<FormulaFunctionRegistry>())
{
}

FormulaEngine::FormulaEngine(ExcelDocument::Ptr document)
    : m_document(std::move(document)), m_registry(std::make_shared<FormulaFunctionRegistry>())
{
}

FormulaEngine::~FormulaEngine() = default;

FormulaEngine::FormulaEngine(const FormulaEngine& other)
    : m_document(other.m_document),
      m_registry(std::make_shared<FormulaFunctionRegistry>(*other.m_registry))
{
}

FormulaEngine::FormulaEngine(FormulaEngine&& other) noexcept = default;

FormulaEngine& FormulaEngine::operator=(const FormulaEngine& other)
{
    if (this != &other)
    {
        m_document = other.m_document;
        m_registry = std::make_shared<FormulaFunctionRegistry>(*other.m_registry);
    }
    return *this;
}

FormulaEngine& FormulaEngine::operator=(FormulaEngine&& other) noexcept = default;

bool FormulaEngine::IsValid() const noexcept
{
    return m_document != nullptr;
}

FormulaValidationResult FormulaEngine::ValidateFormula(std::string_view formula) const
{
    FormulaValidationResult result;
    FormulaParseResult parsed = FormulaParser::Parse(formula);
    result.Diagnostics = std::move(parsed.diagnostics);

    if (parsed.root && result.Diagnostics.empty())
    {
        // Known defined names, in any scope, for name diagnostics.
        std::set<std::string> knownNames;
        if (m_document)
        {
            NamedRangeManager names(m_document);
            for (const NamedRange& entry : names.List())
            {
                knownNames.insert(AsciiText::ToLower(entry.Name));
            }
        }

        // Validate function names, argument counts, and defined names. The walk
        // uses an explicit stack because a chain of operators makes the tree as
        // deep as the formula is long; children are pushed in reverse so that
        // diagnostics still come out in document order.
        const auto validate = [this, &result, &knownNames](const FormulaExpression& node)
        {
            if (node.kind == FormulaExpressionKind::FunctionCall)
            {
                const RegisteredFormulaFunction* function = m_registry->Find(node.text);
                if (!function)
                {
                    result.Diagnostics.push_back(
                        {node.offset, node.length, "Unknown function '" + node.text + "'."});
                }
                else if (node.children.size() < function->spec.MinimumArgumentCount ||
                         node.children.size() > function->spec.MaximumArgumentCount)
                {
                    result.Diagnostics.push_back(
                        {node.offset, node.length,
                         "Function '" + node.text + "' does not accept " +
                             std::to_string(node.children.size()) + " argument(s)."});
                }
            }
            else if (node.kind == FormulaExpressionKind::NameReference && m_document &&
                     knownNames.find(AsciiText::ToLower(node.text)) == knownNames.end())
            {
                result.Diagnostics.push_back(
                    {node.offset, node.length, "Unknown name '" + node.text + "'."});
            }
        };

        std::vector<const FormulaExpression*> pending;
        pending.push_back(parsed.root.get());
        while (!pending.empty())
        {
            const FormulaExpression& node = *pending.back();
            pending.pop_back();
            validate(node);
            for (auto child = node.children.rbegin(); child != node.children.rend(); ++child)
            {
                if (*child)
                {
                    pending.push_back(child->get());
                }
            }
        }
    }

    if (!result.Diagnostics.empty())
    {
        result.Status.Error = FormulaEngineError::ParseError;
        result.Status.Message = result.Diagnostics.front().Message;
    }
    return result;
}

FormulaEvaluationResult FormulaEngine::EvaluateFormula(std::string_view formula,
                                                       std::string_view sheetName,
                                                       CellAddress anchor) const
{
    FormulaEvaluationResult result;
    if (!IsValid())
    {
        result.Status.Error = FormulaEngineError::InvalidDocument;
        result.Status.Message = "The formula engine has no attached workbook document.";
        return result;
    }

    FormulaParseResult parsed = FormulaParser::Parse(formula);
    if (!parsed.Succeeded())
    {
        result.Status.Error = FormulaEngineError::ParseError;
        result.Diagnostics = std::move(parsed.diagnostics);
        result.Status.Message = result.Diagnostics.empty() ? std::string("The formula could not be parsed.")
                                                           : result.Diagnostics.front().Message;
        return result;
    }

    FormulaEvaluationSession session(m_document, *m_registry);
    std::string resolvedSheet = std::string(sheetName);
    if (resolvedSheet.empty())
    {
        resolvedSheet = session.FirstSheetName();
    }
    if (resolvedSheet.empty() || !session.SheetExists(resolvedSheet))
    {
        result.Status.Error = FormulaEngineError::UnknownSheet;
        result.Status.Message = "Worksheet '" + resolvedSheet + "' does not exist in the workbook.";
        return result;
    }
    session.SetCurrentSheet(std::move(resolvedSheet));
    session.SetAnchor(anchor);
    result.Value = session.EvaluateToValue(*parsed.root);
    return result;
}

FormulaEvaluationResult FormulaEngine::EvaluateCell(std::string_view sheetName, CellAddress address) const
{
    FormulaEvaluationResult result;
    if (!IsValid())
    {
        result.Status.Error = FormulaEngineError::InvalidDocument;
        result.Status.Message = "The formula engine has no attached workbook document.";
        return result;
    }
    if (!address.IsValid())
    {
        result.Status.Error = FormulaEngineError::InvalidAddress;
        result.Status.Message = "The supplied cell address is invalid.";
        return result;
    }

    ExcelDocumentEditor editor(m_document);
    Worksheet::Ptr worksheet = editor.GetWorksheet(sheetName);
    if (!worksheet)
    {
        result.Status.Error = FormulaEngineError::UnknownSheet;
        result.Status.Message = "Worksheet '" + std::string(sheetName) + "' does not exist in the workbook.";
        return result;
    }

    const auto model = worksheet->GetCellFormula(address);
    if (!model)
    {
        result.Status.Error = FormulaEngineError::NotAFormulaCell;
        result.Status.Message = "Cell " + address.ToA1() + " does not contain a formula.";
        return result;
    }
    if (model->ReferenceStyle == FormulaReferenceStyle::R1C1)
    {
        result.Status.Error = FormulaEngineError::UnsupportedReferenceStyle;
        result.Status.Message = "R1C1 formulas are not supported by the formula engine.";
        return result;
    }

    // Resolve the effective expression: shared dependents evaluate their
    // anchor's expression shifted to this cell.
    std::string effectiveFormula = model->Formula;
    Int64 rowOffset = 0;
    Int64 columnOffset = 0;
    if (model->Kind == CellFormulaKind::Shared && effectiveFormula.empty() && model->SharedIndex)
    {
        bool anchorFound = false;
        for (const CellAddress& candidate : worksheet->StoredCellAddresses())
        {
            const auto candidateModel = worksheet->GetCellFormula(candidate);
            if (!candidateModel || candidateModel->Kind != CellFormulaKind::Shared ||
                candidateModel->SharedIndex != model->SharedIndex || candidateModel->Formula.empty())
            {
                continue;
            }
            effectiveFormula = candidateModel->Formula;
            rowOffset = static_cast<Int64>(address.Row().Value()) - candidate.Row().Value();
            columnOffset = static_cast<Int64>(address.Column().Value()) - candidate.Column().Value();
            anchorFound = true;
            break;
        }
        if (!anchorFound)
        {
            result.Status.Error = FormulaEngineError::EvaluationFailed;
            result.Status.Message = "The shared-formula anchor for cell " + address.ToA1() + " is missing.";
            return result;
        }
    }

    FormulaParseResult parsed = FormulaParser::Parse(effectiveFormula);
    if (!parsed.Succeeded())
    {
        result.Status.Error = FormulaEngineError::ParseError;
        result.Diagnostics = std::move(parsed.diagnostics);
        result.Status.Message = result.Diagnostics.empty() ? std::string("The formula could not be parsed.")
                                                           : result.Diagnostics.front().Message;
        return result;
    }

    FormulaEvaluationSession session(m_document, *m_registry);
    session.SetCurrentSheet(worksheet->Name());
    session.SetAnchor(address);
    session.SetReferenceOffset(rowOffset, columnOffset);
    session.SetArrayContext(model->Kind == CellFormulaKind::Array);
    result.Value = session.EvaluateToValue(*parsed.root);
    return result;
}

// ---------------------------------------------------------------------------
// Dependency graph construction and recalculation
// ---------------------------------------------------------------------------

namespace FormulaEngineDetail
{

/** Builds the workbook-wide formula dependency graph. */
class GraphBuilder final
{
public:
    GraphBuilder() = delete;

    static DependencyGraph Build(const ExcelDocument::Ptr& document,
                                 const FormulaFunctionRegistry& registry,
                                 std::vector<FormulaParseResult>& parseStorage)
    {
        DependencyGraph graph;
        ExcelDocumentEditor editor(document);

        // Enumerate formula cells per worksheet, resolving shared groups.
        std::map<std::string, SheetNodeIndex> sheetIndex;
        std::map<std::pair<std::string, UInt32>, std::pair<CellAddress, std::string>> sharedAnchors;

        struct PendingCell
        {
            std::string sheetLower;
            std::string sheetDisplay;
            CellAddress address;
            CellFormulaValue model;
        };
        std::vector<PendingCell> pending;

        for (const auto& worksheet : editor.Worksheets())
        {
            if (!worksheet)
            {
                continue;
            }
            const std::string displayName = worksheet->Name();
            const std::string lowerName = AsciiText::ToLower(displayName);
            for (const CellAddress& address : worksheet->StoredCellAddresses())
            {
                auto model = worksheet->GetCellFormula(address);
                if (!model)
                {
                    continue;
                }
                if (model->Kind == CellFormulaKind::Shared && !model->Formula.empty() && model->SharedIndex)
                {
                    sharedAnchors[{lowerName, *model->SharedIndex}] = {address, model->Formula};
                }
                pending.push_back({lowerName, displayName, address, std::move(*model)});
            }
        }

        // Parse each distinct formula text once.
        std::map<std::string, const FormulaExpression*> parsedByText;
        const auto parseFormula = [&](const std::string& text) -> const FormulaExpression*
        {
            const auto it = parsedByText.find(text);
            if (it != parsedByText.end())
            {
                return it->second;
            }
            FormulaParseResult parsed = FormulaParser::Parse(text);
            const FormulaExpression* root = parsed.Succeeded() ? parsed.root.get() : nullptr;
            parseStorage.push_back(std::move(parsed));
            parsedByText.emplace(text, root);
            return root;
        };

        // Defined names, parsed once, so dependencies flow through them.
        NameAstMap nameAsts;
        {
            NamedRangeManager names(document);
            for (const NamedRange& entry : names.List())
            {
                if (entry.Scope == NamedRangeScope::Sheet && entry.ScopeSheet.empty())
                {
                    continue;
                }
                const std::string scopeLower =
                    entry.Scope == NamedRangeScope::Sheet ? AsciiText::ToLower(entry.ScopeSheet) : std::string();
                nameAsts[{scopeLower, AsciiText::ToLower(entry.Name)}] = parseFormula(entry.Formula);
            }
        }

        graph.nodes.reserve(pending.size());
        for (PendingCell& cell : pending)
        {
            FormulaCellNode node;
            node.sheetLower = cell.sheetLower;
            node.sheetDisplay = cell.sheetDisplay;
            node.address = cell.address;
            node.model = std::move(cell.model);

            if (node.model.ReferenceStyle == FormulaReferenceStyle::R1C1)
            {
                node.skip = true;
            }

            std::string effectiveFormula = node.model.Formula;
            if (node.model.Kind == CellFormulaKind::Shared && effectiveFormula.empty() &&
                node.model.SharedIndex)
            {
                const auto anchor = sharedAnchors.find({node.sheetLower, *node.model.SharedIndex});
                if (anchor == sharedAnchors.end())
                {
                    node.skip = true;
                }
                else
                {
                    effectiveFormula = anchor->second.second;
                    node.rowOffset = static_cast<Int64>(node.address.Row().Value()) -
                                     anchor->second.first.Row().Value();
                    node.columnOffset = static_cast<Int64>(node.address.Column().Value()) -
                                        anchor->second.first.Column().Value();
                }
            }
            if (node.model.Kind == CellFormulaKind::Array)
            {
                node.isArrayFormula = true;
                node.arrayRange = CellRange(node.address, node.address);
                if (node.model.Reference)
                {
                    if (const auto range = CellRange::ParseA1(*node.model.Reference))
                    {
                        node.arrayRange = *range;
                    }
                }
            }

            if (!node.skip)
            {
                node.ast = parseFormula(effectiveFormula);
                if (!node.ast)
                {
                    node.skip = true;
                }
            }
            graph.nodes.push_back(std::move(node));
        }

        // Per-sheet node index for precedent matching.
        for (Size i = 0; i < graph.nodes.size(); ++i)
        {
            const FormulaCellNode& node = graph.nodes[i];
            sheetIndex[node.sheetLower].cells.emplace_back(node.address.Row().Value(),
                                                           node.address.Column().Value(), i);
        }
        for (auto& [sheet, index] : sheetIndex)
        {
            std::sort(index.cells.begin(), index.cells.end());
        }

        // Edges: precedent formula cell -> dependent formula cell.
        graph.dependents.assign(graph.nodes.size(), {});
        graph.indegree.assign(graph.nodes.size(), 0);
        for (Size i = 0; i < graph.nodes.size(); ++i)
        {
            FormulaCellNode& node = graph.nodes[i];
            if (node.skip || !node.ast)
            {
                continue;
            }
            std::vector<ResolvedReferenceArea> precedents;
            bool dynamic = false;
            PrecedentContext context{node.sheetDisplay, node.rowOffset, node.columnOffset};
            std::vector<std::string> nameStack;
            CollectPrecedents(*node.ast, context, registry, nameAsts, nameStack, precedents, dynamic);
            node.dynamic = dynamic;
            graph.anyDynamic = graph.anyDynamic || dynamic;

            for (const ResolvedReferenceArea& area : precedents)
            {
                const auto sheetIt = sheetIndex.find(AsciiText::ToLower(area.sheet));
                if (sheetIt == sheetIndex.end())
                {
                    continue;
                }
                const auto& cells = sheetIt->second.cells;
                auto it = std::lower_bound(cells.begin(), cells.end(),
                                           std::make_tuple(area.firstRow, UInt32(0), Size(0)));
                for (; it != cells.end(); ++it)
                {
                    const auto [row, column, precedentIndex] = *it;
                    if (row > area.lastRow)
                    {
                        break;
                    }
                    if (column < area.firstColumn || column > area.lastColumn)
                    {
                        continue;
                    }
                    graph.dependents[precedentIndex].push_back(i);
                    ++graph.indegree[i];
                }
            }
        }

        DetectCycles(graph);
        BuildOrder(graph);
        return graph;
    }

private:
    /** Iterative Tarjan strongly-connected components. */
    static void DetectCycles(DependencyGraph& graph)
    {
        const Size count = graph.nodes.size();
        graph.cyclic.assign(count, false);

        std::vector<Int64> index(count, -1);
        std::vector<Int64> lowLink(count, 0);
        std::vector<bool> onStack(count, false);
        std::vector<Size> stack;
        Int64 nextIndex = 0;

        struct Frame
        {
            Size node;
            Size childPosition;
        };
        std::vector<Frame> callStack;

        for (Size start = 0; start < count; ++start)
        {
            if (index[start] != -1)
            {
                continue;
            }
            callStack.push_back({start, 0});
            while (!callStack.empty())
            {
                Frame& frame = callStack.back();
                const Size node = frame.node;
                if (frame.childPosition == 0)
                {
                    index[node] = lowLink[node] = nextIndex++;
                    stack.push_back(node);
                    onStack[node] = true;
                }
                bool descended = false;
                while (frame.childPosition < graph.dependents[node].size())
                {
                    const Size child = graph.dependents[node][frame.childPosition];
                    ++frame.childPosition;
                    if (index[child] == -1)
                    {
                        callStack.push_back({child, 0});
                        descended = true;
                        break;
                    }
                    if (onStack[child])
                    {
                        lowLink[node] = std::min(lowLink[node], index[child]);
                    }
                }
                if (descended)
                {
                    continue;
                }
                if (lowLink[node] == index[node])
                {
                    std::vector<Size> component;
                    while (true)
                    {
                        const Size member = stack.back();
                        stack.pop_back();
                        onStack[member] = false;
                        component.push_back(member);
                        if (member == node)
                        {
                            break;
                        }
                    }
                    bool isCycle = component.size() > 1;
                    if (!isCycle)
                    {
                        // A single node forms a cycle when it references itself.
                        const Size single = component.front();
                        for (const Size dependent : graph.dependents[single])
                        {
                            if (dependent == single)
                            {
                                isCycle = true;
                                break;
                            }
                        }
                    }
                    if (isCycle)
                    {
                        std::sort(component.begin(), component.end());
                        for (const Size member : component)
                        {
                            graph.cyclic[member] = true;
                        }
                        graph.cycles.push_back(std::move(component));
                    }
                }
                callStack.pop_back();
                if (!callStack.empty())
                {
                    Frame& parent = callStack.back();
                    lowLink[parent.node] = std::min(lowLink[parent.node], lowLink[node]);
                }
            }
        }
        std::sort(graph.cycles.begin(), graph.cycles.end());
    }

    /** Kahn topological order over the acyclic part of the graph. */
    static void BuildOrder(DependencyGraph& graph)
    {
        const Size count = graph.nodes.size();
        std::vector<Size> indegree = graph.indegree;
        std::vector<Size> queue;

        // Cyclic and skipped nodes are treated as already processed: they
        // release their dependents without being evaluated themselves.
        std::vector<bool> released(count, false);
        for (Size i = 0; i < count; ++i)
        {
            if (graph.cyclic[i] || graph.nodes[i].skip)
            {
                released[i] = true;
            }
        }
        for (Size i = 0; i < count; ++i)
        {
            if (released[i])
            {
                for (const Size dependent : graph.dependents[i])
                {
                    if (indegree[dependent] > 0)
                    {
                        --indegree[dependent];
                    }
                }
            }
        }
        for (Size i = 0; i < count; ++i)
        {
            if (!released[i] && indegree[i] == 0)
            {
                queue.push_back(i);
            }
        }
        Size head = 0;
        while (head < queue.size())
        {
            const Size node = queue[head++];
            graph.order.push_back(node);
            for (const Size dependent : graph.dependents[node])
            {
                if (released[dependent] || graph.cyclic[dependent])
                {
                    continue;
                }
                if (indegree[dependent] == 0)
                {
                    continue;
                }
                if (--indegree[dependent] == 0)
                {
                    queue.push_back(dependent);
                }
            }
        }
        // Nodes that remain (blocked behind cycles through multiple edges)
        // are appended so dependents of cycles still evaluate, using the
        // cycle members' previous cached values.
        std::vector<bool> ordered(count, false);
        for (const Size node : graph.order)
        {
            ordered[node] = true;
        }
        for (Size i = 0; i < count; ++i)
        {
            if (!ordered[i] && !released[i])
            {
                graph.order.push_back(i);
            }
        }
    }
};

} // namespace FormulaEngineDetail

std::vector<std::vector<SheetCellAddress>> FormulaEngine::FindCircularReferences() const
{
    std::vector<std::vector<SheetCellAddress>> result;
    if (!IsValid())
    {
        return result;
    }
    std::vector<FormulaParseResult> parseStorage;
    const auto graph = FormulaEngineDetail::GraphBuilder::Build(m_document, *m_registry, parseStorage);
    result.reserve(graph.cycles.size());
    for (const auto& cycle : graph.cycles)
    {
        std::vector<SheetCellAddress> members;
        members.reserve(cycle.size());
        for (const Size nodeIndex : cycle)
        {
            const auto& node = graph.nodes[nodeIndex];
            members.push_back({node.sheetDisplay, node.address});
        }
        result.push_back(std::move(members));
    }
    return result;
}

RecalculationResult FormulaEngine::Recalculate()
{
    return RecalculateSheet({});
}

RecalculationResult FormulaEngine::RecalculateSheet(std::string_view sheetName)
{
    using namespace FormulaEngineDetail;

    RecalculationResult result;
    if (!IsValid())
    {
        result.Status.Error = FormulaEngineError::InvalidDocument;
        result.Status.Message = "The formula engine has no attached workbook document.";
        return result;
    }

    ExcelDocumentEditor editor(m_document);
    std::string targetSheetLower;
    if (!sheetName.empty())
    {
        Worksheet::Ptr worksheet = editor.GetWorksheet(sheetName);
        if (!worksheet)
        {
            result.Status.Error = FormulaEngineError::UnknownSheet;
            result.Status.Message =
                "Worksheet '" + std::string(sheetName) + "' does not exist in the workbook.";
            return result;
        }
        targetSheetLower = AsciiText::ToLower(worksheet->Name());
    }

    std::vector<FormulaParseResult> parseStorage;
    auto graph = GraphBuilder::Build(m_document, *m_registry, parseStorage);

    for (const auto& cycle : graph.cycles)
    {
        std::vector<SheetCellAddress> members;
        members.reserve(cycle.size());
        for (const Size nodeIndex : cycle)
        {
            const auto& node = graph.nodes[nodeIndex];
            members.push_back({node.sheetDisplay, node.address});
        }
        result.CircularReferenceCycles.push_back(std::move(members));
    }

    // Evaluate in dependency order, keeping results in an overlay so cell
    // reads see fresh values before anything is written back.
    FormulaEvaluationSession session(m_document, *m_registry);
    FormulaEvaluationSession::OverlayMap overlay;
    session.SetOverlay(&overlay);

    /** Distributed array results: sheetLower/cell -> plain value to store. */
    std::map<FormulaEvaluationSession::CellKey, FormulaValue> arraySpill;
    std::vector<std::pair<Size, FormulaValue>> computed;

    const auto evaluateNode = [&](Size nodeIndex)
    {
        const FormulaCellNode& node = graph.nodes[nodeIndex];
        session.SetCurrentSheet(node.sheetDisplay);
        session.SetAnchor(node.address);
        session.SetReferenceOffset(node.rowOffset, node.columnOffset);
        session.SetArrayContext(node.isArrayFormula);
        const FormulaValue value = session.EvaluateToValue(*node.ast);

        // Publish the result in the overlay for later nodes.
        if (node.isArrayFormula)
        {
            const UInt32 firstRow = node.arrayRange.First().Row().Value();
            const UInt32 firstColumn = node.arrayRange.First().Column().Value();
            for (UInt32 row = firstRow; row <= node.arrayRange.Last().Row().Value(); ++row)
            {
                for (UInt32 column = firstColumn;
                     column <= node.arrayRange.Last().Column().Value(); ++column)
                {
                    FormulaValue element;
                    const Size rowIndex = row - firstRow;
                    const Size columnIndex = column - firstColumn;
                    if (value.Kind() == FormulaValueKind::Array)
                    {
                        const Size effectiveRow = value.RowCount() == 1 ? 0 : rowIndex;
                        const Size effectiveColumn = value.ColumnCount() == 1 ? 0 : columnIndex;
                        element = (effectiveRow < value.RowCount() && effectiveColumn < value.ColumnCount())
                                      ? value.At(effectiveRow, effectiveColumn)
                                      : FormulaValue::Error(FormulaErrorCode::NA);
                    }
                    else
                    {
                        element = value;
                    }
                    FormulaEvaluationSession::CellKey key{node.sheetLower, row, column};
                    overlay[key] = element;
                    if (row != node.address.Row().Value() || column != node.address.Column().Value())
                    {
                        arraySpill[key] = element;
                    }
                }
            }
        }
        else
        {
            overlay[{node.sheetLower, node.address.Row().Value(), node.address.Column().Value()}] = value;
        }
        return value;
    };

    for (const Size nodeIndex : graph.order)
    {
        computed.emplace_back(nodeIndex, evaluateNode(nodeIndex));
    }
    // Dynamic references (OFFSET, INDIRECT) and volatile functions may read
    // cells that were recalculated after them; one bounded extra pass
    // stabilizes those results.
    if (graph.anyDynamic)
    {
        computed.clear();
        for (const Size nodeIndex : graph.order)
        {
            computed.emplace_back(nodeIndex, evaluateNode(nodeIndex));
        }
    }

    // Write results back through the storage API.
    for (auto& [nodeIndex, value] : computed)
    {
        const FormulaCellNode& node = graph.nodes[nodeIndex];
        if (!targetSheetLower.empty() && node.sheetLower != targetSheetLower)
        {
            continue;
        }
        Worksheet::Ptr worksheet = editor.GetWorksheet(node.sheetDisplay);
        if (!worksheet)
        {
            continue;
        }
        CellFormulaValue model = node.model;
        const FormulaValue& cellValue =
            node.isArrayFormula
                ? overlay[{node.sheetLower, node.address.Row().Value(), node.address.Column().Value()}]
                : value;
        ToCachedResult(cellValue, model.CachedKind, model.CachedText);
        if (worksheet->SetCellFormula(node.address, model))
        {
            ++result.RecalculatedCellCount;
        }
    }
    // Distribute array-formula results over their ranges as plain values.
    for (const auto& [key, value] : arraySpill)
    {
        if (!targetSheetLower.empty() && key.sheet != targetSheetLower)
        {
            continue;
        }
        Worksheet::Ptr worksheet = editor.GetWorksheet(key.sheet);
        if (!worksheet)
        {
            continue;
        }
        const auto address = CellAddress::TryCreate(key.row, key.column);
        if (!address)
        {
            continue;
        }
        // Cells of the array range that hold their own formula model keep it.
        if (worksheet->GetCellFormula(*address))
        {
            continue;
        }
        worksheet->SetCellValue(*address, value.ToCellValue());
    }

    // A stale calculation chain can trigger repair prompts; spreadsheet
    // applications rebuild it automatically when it is absent.
    if (const auto workbookPart = m_document->GetWorkbookPart())
    {
        workbookPart->RemoveCalculationChainPart();
    }
    return result;
}

bool FormulaEngine::RegisterFunction(std::string_view name,
                                     FormulaFunctionSpec spec,
                                     FormulaFunction function)
{
    if (!m_registry)
    {
        return false;
    }
    return m_registry->RegisterCustom(name, spec, std::move(function));
}

bool FormulaEngine::IsFunctionRegistered(std::string_view name) const
{
    return m_registry && m_registry->IsRegistered(name);
}

std::vector<std::string> FormulaEngine::FunctionNames() const
{
    return m_registry ? m_registry->Names() : std::vector<std::string>();
}

} // namespace ExyokiOffice::Excel
