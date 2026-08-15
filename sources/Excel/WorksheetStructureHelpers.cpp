// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "Excel/WorksheetStructureHelpers.hpp"

#include "ExyokiOffice/Excel/ExcelReference.hpp"
#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/Spreadsheet.hpp"
#include "ExyokiOffice/DOM/Namespaces.hpp"
#include "OpenXmlDomInternal.hpp"
#include "XmlNamespaceResolver.hpp"
#include "ExyokiOffice/StandardTypes.hpp"
#include "AsciiText.hpp"

#include <algorithm>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ExyokiOffice::Excel
{

namespace Spreadsheet = ExyokiOffice::DocumentFormat::OpenXml::Spreadsheet;

class WorksheetStructureImplementation final
{
public:
    WorksheetStructureImplementation() = delete;

    enum class Axis
    {
        Row,
        Column
    };

    enum class EditKind
    {
        Insert,
        Delete
    };

    struct Edit
    {
        Axis axis;
        EditKind kind;
        UInt32 index;
        UInt32 count;
    };

    struct ParsedReference
    {
        UInt32 row = 0;
        UInt32 column = 0;
        bool absoluteRow = false;
        bool absoluteColumn = false;
        Size length = 0;
    };

    struct RewriteResult
    {
        bool succeeded = true;
        bool removed = false;
        std::string text;
    };

    static RangeOperationResult Apply(Worksheet& worksheet,
                                      Edit edit,
                                      FormulaReferenceUpdatePolicy formulaPolicy)
    {
        if (edit.count == 0)
        {
            return Error(RangeOperationError::InvalidCount, "The structural edit count must be greater than zero.");
        }
        const auto maximum = edit.axis == Axis::Row ? MaxRowIndex : MaxColumnIndex;
        if (edit.index == 0 || edit.index > maximum ||
            (edit.kind == EditKind::Delete &&
             static_cast<UInt64>(edit.index) + edit.count - 1 > maximum))
        {
            return Error(RangeOperationError::InvalidAddress,
                         "The structural edit interval is outside the Excel worksheet grid.");
        }

        const auto part = worksheet.GetPart();
        const auto root = worksheet.GetLowLevelApi();
        if (!part || !root)
        {
            return Error(RangeOperationError::InvalidWorksheet,
                         "The worksheet is not attached to a usable worksheet part.");
        }

        const auto originalXml = part->GetXmlString();
        Size affectedCells = 0;
        if (!RewriteStoredCells(root, edit, affectedCells) ||
            !RewriteWorksheetRanges(root, edit) ||
            (formulaPolicy == FormulaReferenceUpdatePolicy::UpdateUnqualifiedA1References &&
             !RewriteFormulas(root, worksheet.Name(), edit)))
        {
            part->SetXmlString(originalXml);
            return Error(RangeOperationError::ReferenceUpdateFailed,
                         "A worksheet reference could not be rewritten safely; the original worksheet was restored.");
        }

        return RangeOperationResult{RangeOperationError::None, {}, affectedCells};
    }

private:
    static RangeOperationResult Error(RangeOperationError error, std::string message)
    {
        return RangeOperationResult{error, std::move(message), 0};
    }

    static bool NodeHasName(const Pugi::xml_node& node, const OpenXmlQualifiedName& expected)
    {
        const std::string_view qualifiedName(node.name());
        const auto separator = qualifiedName.find(':');
        const auto prefix = separator == std::string_view::npos ? std::string_view{} : qualifiedName.substr(0, separator);
        const auto localName =
            separator == std::string_view::npos ? qualifiedName : qualifiedName.substr(separator + 1);
        if (localName != expected.localName())
        {
            return false;
        }
        const auto namespaceUri = Xml::NamespaceResolver::LookupUriForPrefix(node, prefix);
        return namespaceUri && *namespaceUri == expected.namespaceUri();
    }

    template <typename T>
    static std::vector<std::shared_ptr<T>> FindElements(const std::shared_ptr<OpenXMLElement>& root)
    {
        std::vector<std::shared_ptr<T>> result;
        const auto rootNode = Detail::NodeOf(root);
        if (!rootNode)
        {
            return result;
        }
        std::vector<Pugi::xml_node> stack;
        for (auto child = rootNode.last_child(); child; child = child.previous_sibling())
        {
            if (child.type() == Pugi::node_element)
            {
                stack.push_back(child);
            }
        }
        while (!stack.empty())
        {
            const auto node = stack.back();
            stack.pop_back();
            if (NodeHasName(node, T::StaticMetaClass()->QualifiedName()))
            {
                result.push_back(Detail::WrapNode<T>(node));
            }
            for (auto child = node.last_child(); child; child = child.previous_sibling())
            {
                if (child.type() == Pugi::node_element)
                {
                    stack.push_back(child);
                }
            }
        }
        return result;
    }

    template <typename T>
    static std::vector<std::shared_ptr<T>> FindChildren(const std::shared_ptr<OpenXMLElement>& parent)
    {
        std::vector<std::shared_ptr<T>> result;
        const auto parentNode = Detail::NodeOf(parent);
        for (auto child = parentNode.first_child(); child; child = child.next_sibling())
        {
            if (child.type() == Pugi::node_element && NodeHasName(child, T::StaticMetaClass()->QualifiedName()))
            {
                result.push_back(Detail::WrapNode<T>(child));
            }
        }
        return result;
    }

    static bool RewriteStoredCells(const std::shared_ptr<Spreadsheet::Worksheet>& root,
                                   const Edit& edit,
                                   Size& affectedCells)
    {
        for (const auto& row : FindElements<Spreadsheet::Row>(root))
        {
            if (!row)
            {
                continue;
            }
            const auto oldRowIndex = row->GetRowIndex();
            if (!oldRowIndex.IsDefined())
            {
                return false;
            }

            if (edit.axis == Axis::Row)
            {
                const auto rewrittenRow = RewriteCoordinate(oldRowIndex.Value(), edit);
                if (!rewrittenRow.succeeded)
                {
                    return false;
                }
                if (rewrittenRow.removed)
                {
                    affectedCells += FindChildren<Spreadsheet::Cell>(row).size();
                    if (const auto parent = row->Parent())
                    {
                        parent->RemoveChild(row);
                    }
                    continue;
                }
                if (rewrittenRow.value != oldRowIndex.Value())
                {
                    row->SetRowIndex(UInt32Value(rewrittenRow.value));
                }
            }

            for (const auto& cell : FindChildren<Spreadsheet::Cell>(row))
            {
                if (!cell)
                {
                    continue;
                }
                const auto address = CellAddress::ParseA1(cell->GetCellReference().ToString());
                if (!address)
                {
                    return false;
                }
                const auto coordinate = edit.axis == Axis::Row ? address->Row().Value() : address->Column().Value();
                const auto rewritten = RewriteCoordinate(coordinate, edit);
                if (!rewritten.succeeded)
                {
                    return false;
                }
                if (rewritten.removed)
                {
                    ++affectedCells;
                    row->RemoveChild(cell);
                    continue;
                }
                if (rewritten.value != coordinate)
                {
                    const auto newRow = edit.axis == Axis::Row ? rewritten.value : address->Row().Value();
                    const auto newColumn = edit.axis == Axis::Column ? rewritten.value : address->Column().Value();
                    const auto newAddress = CellAddress::TryCreate(newRow, newColumn);
                    if (!newAddress)
                    {
                        return false;
                    }
                    cell->SetCellReference(StringValue(newAddress->ToA1()));
                    ++affectedCells;
                }
            }
        }
        return true;
    }

    struct CoordinateRewrite
    {
        bool succeeded = true;
        bool removed = false;
        UInt32 value = 0;
    };

    static CoordinateRewrite RewriteCoordinate(UInt32 value, const Edit& edit)
    {
        if (edit.kind == EditKind::Insert)
        {
            if (value < edit.index)
            {
                return {true, false, value};
            }
            const auto shifted = static_cast<UInt64>(value) + edit.count;
            const auto maximum = edit.axis == Axis::Row ? MaxRowIndex : MaxColumnIndex;
            return shifted <= maximum ? CoordinateRewrite{true, false, static_cast<UInt32>(shifted)}
                                      : CoordinateRewrite{false, false, value};
        }

        const auto lastDeleted = edit.index + edit.count - 1;
        if (value < edit.index)
        {
            return {true, false, value};
        }
        if (value <= lastDeleted)
        {
            return {true, true, value};
        }
        return {true, false, value - edit.count};
    }

    static std::optional<ParsedReference> ParseReference(std::string_view text, Size offset = 0)
    {
        ParsedReference result;
        Size cursor = offset;
        if (cursor < text.size() && text[cursor] == '$')
        {
            result.absoluteColumn = true;
            ++cursor;
        }
        const auto columnStart = cursor;
        while (cursor < text.size() && AsciiText::IsAlpha(text[cursor]))
        {
            ++cursor;
        }
        if (cursor == columnStart || cursor - columnStart > 3)
        {
            return std::nullopt;
        }
        const auto column = ColumnIndex::ParseName(text.substr(columnStart, cursor - columnStart));
        if (!column)
        {
            return std::nullopt;
        }
        if (cursor < text.size() && text[cursor] == '$')
        {
            result.absoluteRow = true;
            ++cursor;
        }
        const auto rowStart = cursor;
        UInt64 row = 0;
        while (cursor < text.size() && AsciiText::IsDigit(text[cursor]))
        {
            row = row * 10 + static_cast<unsigned char>(text[cursor] - '0');
            if (row > MaxRowIndex)
            {
                return std::nullopt;
            }
            ++cursor;
        }
        if (cursor == rowStart || row == 0)
        {
            return std::nullopt;
        }
        result.row = static_cast<UInt32>(row);
        result.column = column->Value();
        result.length = cursor - offset;
        return result;
    }

    static std::string FormatReference(const ParsedReference& reference)
    {
        std::string result;
        if (reference.absoluteColumn)
        {
            result.push_back('$');
        }
        result += ColumnIndex(reference.column).ToName();
        if (reference.absoluteRow)
        {
            result.push_back('$');
        }
        result += std::to_string(reference.row);
        return result;
    }

    static RewriteResult RewriteReference(std::string_view text, const Edit& edit)
    {
        const auto first = ParseReference(text);
        if (!first)
        {
            return {false, false, std::string(text)};
        }
        if (first->length == text.size())
        {
            auto rewritten = *first;
            const auto coordinate = edit.axis == Axis::Row ? rewritten.row : rewritten.column;
            const auto changed = RewriteCoordinate(coordinate, edit);
            if (!changed.succeeded)
            {
                return {false, false, std::string(text)};
            }
            if (changed.removed)
            {
                return {true, true, {}};
            }
            if (edit.axis == Axis::Row)
            {
                rewritten.row = changed.value;
            }
            else
            {
                rewritten.column = changed.value;
            }
            return {true, false, FormatReference(rewritten)};
        }
        if (first->length >= text.size() || text[first->length] != ':')
        {
            return {false, false, std::string(text)};
        }
        const auto second = ParseReference(text, first->length + 1);
        if (!second || first->length + 1 + second->length != text.size())
        {
            return {false, false, std::string(text)};
        }

        auto rewrittenFirst = *first;
        auto rewrittenSecond = *second;
        if (!RewriteRangeAxis(rewrittenFirst, rewrittenSecond, edit))
        {
            const auto failedStart = edit.axis == Axis::Row ? rewrittenFirst.row : rewrittenFirst.column;
            return failedStart == 0 ? RewriteResult{false, false, std::string(text)}
                                    : RewriteResult{true, true, {}};
        }
        return {true, false, FormatReference(rewrittenFirst) + ":" + FormatReference(rewrittenSecond)};
    }

    static bool RewriteRangeAxis(ParsedReference& first, ParsedReference& last, const Edit& edit)
    {
        auto& start = edit.axis == Axis::Row ? first.row : first.column;
        auto& finish = edit.axis == Axis::Row ? last.row : last.column;
        if (edit.kind == EditKind::Insert)
        {
            const auto rewrittenStart = RewriteCoordinate(start, edit);
            const auto rewrittenFinish = RewriteCoordinate(finish, edit);
            if (!rewrittenStart.succeeded || !rewrittenFinish.succeeded)
            {
                start = 0;
                finish = 0;
                return false;
            }
            start = rewrittenStart.value;
            finish = rewrittenFinish.value;
            return true;
        }

        const auto deletedFirst = edit.index;
        const auto deletedLast = edit.index + edit.count - 1;
        if (finish < deletedFirst)
        {
            return true;
        }
        if (start > deletedLast)
        {
            start -= edit.count;
            finish -= edit.count;
            return true;
        }
        const auto newStart = start < deletedFirst ? start : deletedFirst;
        const auto newFinish = finish > deletedLast ? finish - edit.count : deletedFirst - 1;
        if (newStart == 0 || newStart > newFinish)
        {
            return false;
        }
        start = newStart;
        finish = newFinish;
        return true;
    }

    static bool IsReferenceBoundary(std::string_view formula, Size offset, Size length)
    {
        // Bytes outside ASCII belong to the identifier they sit in, so a name
        // written in another script keeps a reference-looking substring of its
        // own from being rewritten as a reference.
        if (offset > 0)
        {
            const auto previous = formula[offset - 1];
            if (AsciiText::IsAlnum(previous) || AsciiText::IsNonAscii(previous) ||
                previous == '_' || previous == '.' || previous == '!' || previous == '[')
            {
                return false;
            }
        }
        const auto end = offset + length;
        if (end < formula.size())
        {
            const auto next = formula[end];
            if (AsciiText::IsAlnum(next) || AsciiText::IsNonAscii(next) ||
                next == '_' || next == '.' || next == '(' || next == ']')
            {
                return false;
            }
        }
        return true;
    }

    static RewriteResult RewriteFormulaText(std::string_view formula, const Edit& edit)
    {
        std::string output;
        output.reserve(formula.size());
        for (Size cursor = 0; cursor < formula.size();)
        {
            if (formula[cursor] == '"')
            {
                const auto literalStart = cursor++;
                while (cursor < formula.size())
                {
                    if (formula[cursor] != '"')
                    {
                        ++cursor;
                        continue;
                    }
                    ++cursor;
                    if (cursor < formula.size() && formula[cursor] == '"')
                    {
                        ++cursor;
                        continue;
                    }
                    break;
                }
                output.append(formula.substr(literalStart, cursor - literalStart));
                continue;
            }

            const auto first = ParseReference(formula, cursor);
            if (!first || !IsReferenceBoundary(formula, cursor, first->length))
            {
                output.push_back(formula[cursor++]);
                continue;
            }
            Size tokenLength = first->length;
            if (cursor + tokenLength < formula.size() && formula[cursor + tokenLength] == ':')
            {
                const auto second = ParseReference(formula, cursor + tokenLength + 1);
                if (second)
                {
                    tokenLength += 1 + second->length;
                }
            }
            if (!IsReferenceBoundary(formula, cursor, tokenLength))
            {
                output.push_back(formula[cursor++]);
                continue;
            }
            const auto rewritten = RewriteReference(formula.substr(cursor, tokenLength), edit);
            if (!rewritten.succeeded)
            {
                return {false, false, {}};
            }
            output += rewritten.removed ? "#REF!" : rewritten.text;
            cursor += tokenLength;
        }
        return {true, false, std::move(output)};
    }

    template <typename TElement>
    static bool RewriteSingleReferences(const std::shared_ptr<Spreadsheet::Worksheet>& root, const Edit& edit)
    {
        for (const auto& element : FindElements<TElement>(root))
        {
            const auto reference = element->GetReference();
            if (!reference.IsDefined())
            {
                continue;
            }
            const auto rewritten = RewriteReference(reference.ToString(), edit);
            if (!rewritten.succeeded)
            {
                return false;
            }
            if (rewritten.removed)
            {
                if (const auto parent = element->Parent())
                {
                    parent->RemoveChild(element);
                }
            }
            else
            {
                element->SetReference(StringValue(rewritten.text));
            }
        }
        return true;
    }

    template <typename TElement>
    static bool RewriteReferenceLists(const std::shared_ptr<Spreadsheet::Worksheet>& root, const Edit& edit)
    {
        std::optional<OpenXmlQualifiedName> referenceAttribute;
        for (const auto& attribute : TElement::StaticMetaClass()->GetAttributes())
        {
            if (attribute.TypeName == "ListValue<StringValue>")
            {
                referenceAttribute = attribute.Name;
                break;
            }
        }
        if (!referenceAttribute)
        {
            return false;
        }
        for (const auto& element : FindElements<TElement>(root))
        {
            std::string_view referenceText;
            if (!element->TryGetAttribute(*referenceAttribute, referenceText))
            {
                continue;
            }
            std::vector<std::string> rewrittenValues;
            Size cursor = 0;
            while (cursor < referenceText.size())
            {
                while (cursor < referenceText.size() &&
                       AsciiText::IsSpace(referenceText[cursor]))
                {
                    ++cursor;
                }
                const auto start = cursor;
                while (cursor < referenceText.size() &&
                       !AsciiText::IsSpace(referenceText[cursor]))
                {
                    ++cursor;
                }
                if (start == cursor)
                {
                    continue;
                }
                const auto rewritten = RewriteReference(referenceText.substr(start, cursor - start), edit);
                if (!rewritten.succeeded)
                {
                    return false;
                }
                if (!rewritten.removed)
                {
                    rewrittenValues.push_back(rewritten.text);
                }
            }
            if (rewrittenValues.empty())
            {
                if (const auto parent = element->Parent())
                {
                    parent->RemoveChild(element);
                }
            }
            else
            {
                std::string joined;
                for (const auto& value : rewrittenValues)
                {
                    if (!joined.empty())
                    {
                        joined.push_back(' ');
                    }
                    joined += value;
                }
                element->SetAttribute(*referenceAttribute, joined);
            }
        }
        return true;
    }

    static bool RewriteWorksheetRanges(const std::shared_ptr<Spreadsheet::Worksheet>& root, const Edit& edit)
    {
        const bool singles = RewriteSingleReferences<Spreadsheet::SheetDimension>(root, edit) &&
                             RewriteSingleReferences<Spreadsheet::MergeCell>(root, edit) &&
                             RewriteSingleReferences<Spreadsheet::AutoFilter>(root, edit) &&
                             RewriteSingleReferences<Spreadsheet::Hyperlink>(root, edit);
        const bool lists = RewriteReferenceLists<Spreadsheet::ConditionalFormatting>(root, edit) &&
                           RewriteReferenceLists<Spreadsheet::DataValidation>(root, edit) &&
                           RewriteReferenceLists<Spreadsheet::IgnoredError>(root, edit) &&
                           RewriteReferenceLists<Spreadsheet::ProtectedRange>(root, edit) &&
                           RewriteReferenceLists<Spreadsheet::Selection>(root, edit) &&
                           RewriteReferenceLists<Spreadsheet::Scenarios>(root, edit);
        if (!singles || !lists)
        {
            return false;
        }

        for (const auto& watch : FindElements<Spreadsheet::CellWatch>(root))
        {
            const auto rewritten = RewriteReference(watch->GetCellReference().ToString(), edit);
            if (!rewritten.succeeded)
            {
                return false;
            }
            if (rewritten.removed)
            {
                if (const auto parent = watch->Parent())
                {
                    parent->RemoveChild(watch);
                }
            }
            else
            {
                watch->SetCellReference(StringValue(rewritten.text));
            }
        }

        for (const auto& mergeCells : FindElements<Spreadsheet::MergeCells>(root))
        {
            mergeCells->SetCount(
                UInt32Value(static_cast<UInt32>(FindChildren<Spreadsheet::MergeCell>(mergeCells).size())));
        }
        return true;
    }

    static bool RewriteFormulas(const std::shared_ptr<Spreadsheet::Worksheet>& root,
                                std::string_view sheetName,
                                const Edit& edit)
    {
        FormulaReferenceTransform transform;
        if (edit.axis == Axis::Row && edit.kind == EditKind::Insert)
        {
            transform = FormulaReferenceTransform::InsertRows(edit.index, edit.count);
        }
        else if (edit.axis == Axis::Row)
        {
            transform = FormulaReferenceTransform::DeleteRows(edit.index, edit.count);
        }
        else if (edit.kind == EditKind::Insert)
        {
            transform = FormulaReferenceTransform::InsertColumns(edit.index, edit.count);
        }
        else
        {
            transform = FormulaReferenceTransform::DeleteColumns(edit.index, edit.count);
        }

        for (const auto& formula : FindElements<Spreadsheet::CellFormula>(root))
        {
            const auto rewrittenText = FormulaReferenceRewriter::RewriteA1(formula->GetText(), sheetName, transform);
            if (!rewrittenText)
            {
                return false;
            }
            formula->SetText(rewrittenText.Formula);

            const auto reference = formula->GetReference();
            if (reference.IsDefined())
            {
                const auto rewritten = RewriteReference(reference.ToString(), edit);
                if (!rewritten.succeeded)
                {
                    return false;
                }
                formula->SetReference(rewritten.removed ? StringValue{} : StringValue(rewritten.text));
            }
        }
        return true;
    }
};

RangeOperationResult WorksheetStructureHelpers::InsertRows(Worksheet& worksheet,
                                                           UInt32 beforeRow,
                                                           UInt32 count,
                                                           FormulaReferenceUpdatePolicy formulaPolicy)
{
    return WorksheetStructureImplementation::Apply(
        worksheet,
        {WorksheetStructureImplementation::Axis::Row,
         WorksheetStructureImplementation::EditKind::Insert,
         beforeRow,
         count},
        formulaPolicy);
}

RangeOperationResult WorksheetStructureHelpers::DeleteRows(Worksheet& worksheet,
                                                           UInt32 firstRow,
                                                           UInt32 count,
                                                           FormulaReferenceUpdatePolicy formulaPolicy)
{
    return WorksheetStructureImplementation::Apply(
        worksheet,
        {WorksheetStructureImplementation::Axis::Row,
         WorksheetStructureImplementation::EditKind::Delete,
         firstRow,
         count},
        formulaPolicy);
}

RangeOperationResult WorksheetStructureHelpers::InsertColumns(Worksheet& worksheet,
                                                              UInt32 beforeColumn,
                                                              UInt32 count,
                                                              FormulaReferenceUpdatePolicy formulaPolicy)
{
    return WorksheetStructureImplementation::Apply(
        worksheet,
        {WorksheetStructureImplementation::Axis::Column,
         WorksheetStructureImplementation::EditKind::Insert,
         beforeColumn,
         count},
        formulaPolicy);
}

RangeOperationResult WorksheetStructureHelpers::DeleteColumns(Worksheet& worksheet,
                                                              UInt32 firstColumn,
                                                              UInt32 count,
                                                              FormulaReferenceUpdatePolicy formulaPolicy)
{
    return WorksheetStructureImplementation::Apply(
        worksheet,
        {WorksheetStructureImplementation::Axis::Column,
         WorksheetStructureImplementation::EditKind::Delete,
         firstColumn,
         count},
        formulaPolicy);
}

} // namespace ExyokiOffice::Excel
