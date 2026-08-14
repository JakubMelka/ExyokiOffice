// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "ExyokiOffice/Excel/ExcelDocument.hpp"
#include "ExyokiOffice/Excel/ExcelReference.hpp"

#include "ExyokiOffice/DOM/Namespaces.hpp"
#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/Spreadsheet.hpp"
#include "OpenXmlDomInternal.hpp"
#include "XmlNamespaceResolver.hpp"
#include "Excel/ExcelSlicerInternal.hpp"
#include "Excel/WorksheetStructureHelpers.hpp"
#include "Excel/WorksheetMergeHelpers.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include "AsciiText.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <optional>
#include <unordered_map>
#include <utility>

namespace ExyokiOffice::Excel
{
namespace Spreadsheet = ExyokiOffice::DocumentFormat::OpenXml::Spreadsheet;

/// File-local helpers for schema-aware SpreadsheetML navigation.
class ExcelDocumentXmlHelper
{
public:
    /**
     * @brief File-local helpers for schema-aware SpreadsheetML XML navigation.
     *
     * The generated DOM factory cannot distinguish every colliding schema type in
     * all contexts, so sparse worksheet storage performs this narrow raw-XML
     * navigation. Every comparison and insertion uses a generated schema QName
     * and the namespace resolver instead of relying on a literal prefix.
     */
    class SpreadsheetXmlHelpers final
    {
    public:
        SpreadsheetXmlHelpers() = delete;

        struct XmlName
        {
            std::string_view prefix;
            std::string_view localName;
        };

        static XmlName SplitXmlName(std::string_view qualifiedName)
        {
            const auto separator = qualifiedName.find(':');
            return separator == std::string_view::npos
                       ? XmlName{{}, qualifiedName}
                       : XmlName{qualifiedName.substr(0, separator), qualifiedName.substr(separator + 1)};
        }

        static bool NodeHasName(const Pugi::xml_node& node, const OpenXmlQualifiedName& expected)
        {
            const auto actual = SplitXmlName(node.name());
            if (actual.localName != expected.localName())
            {
                return false;
            }
            const auto namespaceUri = Xml::NamespaceResolver::LookupUriForPrefix(node, actual.prefix);
            return namespaceUri && *namespaceUri == expected.namespaceUri();
        }

        template <typename T>
        static bool NodeHasName(const Pugi::xml_node& node)
        {
            return NodeHasName(node, T::StaticMetaClass()->QualifiedName());
        }

        static std::string QualifiedElementName(Pugi::xml_node& context, const OpenXmlQualifiedName& name)
        {
            std::optional<std::string_view> suggestedPrefix;
            if (const auto suggested = OpenXml::Features::OpenXmlNamespaceResolver::getPrefixForUrl(name.namespaceUri()))
            {
                suggestedPrefix = *suggested;
            }
            const auto prefix = Xml::NamespaceResolver::EnsurePrefix(context, name.namespaceUri(), suggestedPrefix);
            return prefix.empty() ? std::string(name.localName())
                                  : prefix + ":" + std::string(name.localName());
        }

        template <typename T>
        static std::shared_ptr<T> WrapNode(const Pugi::xml_node& node)
        {
            return ExyokiOffice::openxmlelement_cast<T>(ExyokiOffice::Detail::CreateOpenXmlElementFromNode(node));
        }
    };

    static bool IsValidWorksheetName(std::string_view name)
    {
        if (name.empty() || name.size() > 31)
        {
            return false;
        }
        return name.find_first_of(":\\/?*[]") == std::string_view::npos;
    }

    static Pugi::xml_node EnsureSheetData(const std::shared_ptr<Spreadsheet::Worksheet>& worksheet)
    {
        if (!worksheet)
        {
            return {};
        }
        auto worksheetNode = Detail::NodeOf(worksheet);
        for (auto child = worksheetNode.first_child(); child; child = child.next_sibling())
        {
            if (SpreadsheetXmlHelpers::NodeHasName<Spreadsheet::SheetData>(child))
            {
                return child;
            }
        }
        if (!worksheetNode)
        {
            return {};
        }
        const auto name = SpreadsheetXmlHelpers::QualifiedElementName(
            worksheetNode, Spreadsheet::SheetData::StaticMetaClass()->QualifiedName());
        return worksheetNode.append_child(name.c_str());
    }

    static Pugi::xml_node EnsureRow(Pugi::xml_node sheetData,
                                    UInt32 rowIndex)
    {
        if (!sheetData)
        {
            return {};
        }
        Pugi::xml_node insertBefore;
        for (auto row = sheetData.first_child(); row; row = row.next_sibling())
        {
            if (!SpreadsheetXmlHelpers::NodeHasName<Spreadsheet::Row>(row))
            {
                continue;
            }
            const auto rowElement = SpreadsheetXmlHelpers::WrapNode<Spreadsheet::Row>(row);
            const auto existingIndex = rowElement ? rowElement->GetRowIndex() : UInt32Value{};
            if (existingIndex.IsDefined() && existingIndex.Value() == rowIndex)
            {
                return row;
            }
            if (!insertBefore && existingIndex.IsDefined() && existingIndex.Value() > rowIndex)
            {
                insertBefore = row;
            }
        }
        const auto name = SpreadsheetXmlHelpers::QualifiedElementName(sheetData, Spreadsheet::Row::StaticMetaClass()->QualifiedName());
        auto row = insertBefore ? sheetData.insert_child_before(name.c_str(), insertBefore) : sheetData.append_child(name.c_str());
        if (row)
        {
            if (const auto rowElement = SpreadsheetXmlHelpers::WrapNode<Spreadsheet::Row>(row))
            {
                rowElement->SetRowIndex(UInt32Value(rowIndex));
            }
        }
        return row;
    }

    static std::string RawCellReference(const std::shared_ptr<Spreadsheet::Cell>& cell)
    {
        return cell ? cell->GetCellReference().ToString() : std::string{};
    }

    static std::shared_ptr<Spreadsheet::Cell> WrapCellNode(const Pugi::xml_node& node)
    {
        return SpreadsheetXmlHelpers::WrapNode<Spreadsheet::Cell>(node);
    }

    static std::shared_ptr<Spreadsheet::Cell> EnsureCell(Pugi::xml_node row,
                                                         UInt32 rowIndex,
                                                         UInt32 columnIndex)
    {
        if (!row)
        {
            return nullptr;
        }
        const auto address = CellAddress::TryCreate(rowIndex, columnIndex);
        if (!address)
        {
            return nullptr;
        }
        const auto reference = address->ToA1();
        Pugi::xml_node insertBefore;
        for (auto child = row.first_child(); child; child = child.next_sibling())
        {
            if (!SpreadsheetXmlHelpers::NodeHasName<Spreadsheet::Cell>(child))
            {
                continue;
            }
            const auto existingCell = WrapCellNode(child);
            if (RawCellReference(existingCell) == reference)
            {
                return existingCell;
            }
            const auto existing = CellAddress::ParseA1(RawCellReference(existingCell));
            if (!insertBefore && existing && existing->Row().Value() == rowIndex &&
                existing->Column().Value() > columnIndex)
            {
                insertBefore = child;
            }
        }
        const auto name = SpreadsheetXmlHelpers::QualifiedElementName(row, Spreadsheet::Cell::StaticMetaClass()->QualifiedName());
        auto cellNode = insertBefore ? row.insert_child_before(name.c_str(), insertBefore) : row.append_child(name.c_str());
        auto cell = WrapCellNode(cellNode);
        if (cell)
        {
            cell->SetCellReference(StringValue(reference));
        }
        return cell;
    }

    static std::shared_ptr<Spreadsheet::Cell> FindCell(const std::shared_ptr<Spreadsheet::Worksheet>& worksheet,
                                                       CellAddress address)
    {
        if (!worksheet || !address.IsValid())
        {
            return nullptr;
        }

        Pugi::xml_node sheetData;
        for (auto child = Detail::NodeOf(worksheet).first_child(); child; child = child.next_sibling())
        {
            if (SpreadsheetXmlHelpers::NodeHasName<Spreadsheet::SheetData>(child))
            {
                sheetData = child;
                break;
            }
        }
        if (!sheetData)
        {
            return nullptr;
        }
        const auto reference = address.ToA1();
        for (auto rowNode = sheetData.first_child(); rowNode; rowNode = rowNode.next_sibling())
        {
            if (!SpreadsheetXmlHelpers::NodeHasName<Spreadsheet::Row>(rowNode))
            {
                continue;
            }
            const auto row = SpreadsheetXmlHelpers::WrapNode<Spreadsheet::Row>(rowNode);
            if (!row || row->GetRowIndex().ValueOr(0) != address.Row().Value())
            {
                continue;
            }
            for (auto cellNode = rowNode.first_child(); cellNode; cellNode = cellNode.next_sibling())
            {
                if (SpreadsheetXmlHelpers::NodeHasName<Spreadsheet::Cell>(cellNode))
                {
                    const auto cell = WrapCellNode(cellNode);
                    if (RawCellReference(cell) == reference)
                    {
                        return cell;
                    }
                }
            }
        }
        return nullptr;
    }

    static void ClearCellChildren(const std::shared_ptr<Spreadsheet::Cell>& cell)
    {
        if (!cell)
        {
            return;
        }
        for (const auto& child : cell->Children())
        {
            cell->RemoveChild(child);
        }
    }

    static std::shared_ptr<Spreadsheet::CellValue> AppendCellValue(const std::shared_ptr<Spreadsheet::Cell>& cell,
                                                                   std::string_view text)
    {
        auto cellValue = cell ? cell->AppendChild<Spreadsheet::CellValue>() : nullptr;
        if (cellValue)
        {
            cellValue->SetText(text);
        }
        return cellValue;
    }

    static Pugi::xml_node FirstCellValueNode(const std::shared_ptr<Spreadsheet::Cell>& cell)
    {
        for (auto child = Detail::NodeOf(cell).first_child(); child; child = child.next_sibling())
        {
            const std::string_view name(child.name());
            const auto localStart = name.find(':');
            const auto localName = localStart == std::string_view::npos ? name : name.substr(localStart + 1);
            if (localName == "v")
            {
                return child;
            }
        }
        return {};
    }

    static std::string CellValueText(const std::shared_ptr<Spreadsheet::Cell>& cell)
    {
        auto cellValue = FirstCellValueNode(cell);
        return cellValue ? cellValue.text().as_string() : std::string{};
    }

    static std::string InlineStringText(const std::shared_ptr<Spreadsheet::Cell>& cell)
    {
        for (auto child = Detail::NodeOf(cell).first_child(); child; child = child.next_sibling())
        {
            const std::string_view name(child.name());
            const auto localStart = name.find(':');
            const auto localName = localStart == std::string_view::npos ? name : name.substr(localStart + 1);
            if (localName != "is")
            {
                continue;
            }
            for (auto inlineChild = child.first_child(); inlineChild; inlineChild = inlineChild.next_sibling())
            {
                const std::string_view inlineName(inlineChild.name());
                const auto inlineLocalStart = inlineName.find(':');
                const auto inlineLocalName = inlineLocalStart == std::string_view::npos
                                                 ? inlineName
                                                 : inlineName.substr(inlineLocalStart + 1);
                if (inlineLocalName == "t")
                {
                    return inlineChild.text().as_string();
                }
            }
        }
        return {};
    }

    static std::optional<UInt32> ParseUInt32(std::string_view text)
    {
        UInt32 value = 0;
        auto* first = text.data();
        auto* last = first + text.size();
        auto [ptr, ec] = std::from_chars(first, last, value);
        if (ec != std::errc() || ptr != last)
        {
            return std::nullopt;
        }
        return value;
    }

    static FormulaCachedValueKind CachedKindFromCellType(Spreadsheet::CellValues::Value type)
    {
        switch (type)
        {
            case Spreadsheet::CellValues::SharedString:
                return FormulaCachedValueKind::SharedString;
            case Spreadsheet::CellValues::String:
            case Spreadsheet::CellValues::InlineString:
                return FormulaCachedValueKind::String;
            case Spreadsheet::CellValues::Boolean:
                return FormulaCachedValueKind::Boolean;
            case Spreadsheet::CellValues::Error:
                return FormulaCachedValueKind::Error;
            case Spreadsheet::CellValues::Date:
                return FormulaCachedValueKind::DateTime;
            case Spreadsheet::CellValues::Number:
            case Spreadsheet::CellValues::NotDefinedEnumValue:
                return FormulaCachedValueKind::Number;
            default:
                return FormulaCachedValueKind::None;
        }
    }

    static EnumValue<Spreadsheet::CellValues> CellTypeForFormulaCache(FormulaCachedValueKind kind)
    {
        switch (kind)
        {
            case FormulaCachedValueKind::SharedString:
                return EnumValue<Spreadsheet::CellValues>(Spreadsheet::CellValues::SharedString);
            case FormulaCachedValueKind::String:
                return EnumValue<Spreadsheet::CellValues>(Spreadsheet::CellValues::String);
            case FormulaCachedValueKind::Boolean:
                return EnumValue<Spreadsheet::CellValues>(Spreadsheet::CellValues::Boolean);
            case FormulaCachedValueKind::Error:
                return EnumValue<Spreadsheet::CellValues>(Spreadsheet::CellValues::Error);
            case FormulaCachedValueKind::DateTime:
                return EnumValue<Spreadsheet::CellValues>(Spreadsheet::CellValues::Date);
            case FormulaCachedValueKind::Number:
            case FormulaCachedValueKind::None:
            default:
                return {};
        }
    }

    static bool WriteCellValue(const std::shared_ptr<Spreadsheet::Cell>& cell, const ExcelCellValue& value)
    {
        if (!cell)
        {
            return false;
        }

        ClearCellChildren(cell);
        cell->SetDataType({});

        switch (value.Kind())
        {
            case CellValueKind::Blank:
                return true;
            case CellValueKind::InlineString:
            {
                cell->SetDataType(EnumValue<Spreadsheet::CellValues>(Spreadsheet::CellValues::InlineString));
                auto inlineString = cell->AppendChild<Spreadsheet::InlineString>();
                auto text = inlineString ? inlineString->AppendChild<Spreadsheet::Text>() : nullptr;
                if (!text)
                {
                    return false;
                }
                text->SetText(value.Text());
                return true;
            }
            case CellValueKind::SharedString:
                cell->SetDataType(EnumValue<Spreadsheet::CellValues>(Spreadsheet::CellValues::SharedString));
                return AppendCellValue(cell, value.Text()) != nullptr;
            case CellValueKind::Number:
                cell->SetDataType(EnumValue<Spreadsheet::CellValues>(Spreadsheet::CellValues::Number));
                return AppendCellValue(cell, value.Text()) != nullptr;
            case CellValueKind::Boolean:
                cell->SetDataType(EnumValue<Spreadsheet::CellValues>(Spreadsheet::CellValues::Boolean));
                return AppendCellValue(cell, value.Text()) != nullptr;
            case CellValueKind::Error:
                cell->SetDataType(EnumValue<Spreadsheet::CellValues>(Spreadsheet::CellValues::Error));
                return AppendCellValue(cell, value.Text()) != nullptr;
            case CellValueKind::DateTime:
                cell->SetDataType(EnumValue<Spreadsheet::CellValues>(Spreadsheet::CellValues::Date));
                return AppendCellValue(cell, value.Text()) != nullptr;
            case CellValueKind::Formula:
            {
                const auto& formulaValue = value.FormulaValue();
                cell->SetDataType(CellTypeForFormulaCache(formulaValue.CachedKind));
                auto formula = cell->AppendChild<Spreadsheet::CellFormula>();
                if (!formula)
                {
                    return false;
                }
                formula->SetText(formulaValue.Formula);
                if (formulaValue.Kind == CellFormulaKind::Shared)
                {
                    formula->SetFormulaType(EnumValue<Spreadsheet::CellFormulaValues>(Spreadsheet::CellFormulaValues::Shared));
                    formula->SetSharedIndex(UInt32Value(*formulaValue.SharedIndex));
                    if (formulaValue.Reference)
                    {
                        formula->SetReference(StringValue(*formulaValue.Reference));
                    }
                }
                else if (formulaValue.Kind == CellFormulaKind::Array)
                {
                    formula->SetFormulaType(EnumValue<Spreadsheet::CellFormulaValues>(Spreadsheet::CellFormulaValues::Array));
                    formula->SetReference(StringValue(*formulaValue.Reference));
                    if (formulaValue.AlwaysCalculateArray)
                    {
                        formula->SetAlwaysCalculateArray(BooleanValue(true));
                    }
                }
                if (formulaValue.CachedKind == FormulaCachedValueKind::None)
                {
                    return true;
                }
                return AppendCellValue(cell, formulaValue.CachedText) != nullptr;
            }
        }
        return false;
    }

    static std::string SharedStringItemText(const std::shared_ptr<Spreadsheet::SharedStringItem>& item)
    {
        std::string text;
        for (auto child = Detail::NodeOf(item).first_child(); child; child = child.next_sibling())
        {
            const std::string_view name(child.name());
            const auto localStart = name.find(':');
            const auto localName = localStart == std::string_view::npos ? name : name.substr(localStart + 1);
            if (localName == "t")
            {
                text += child.text().as_string();
            }
            else if (localName == "r")
            {
                for (auto runChild = child.first_child(); runChild; runChild = runChild.next_sibling())
                {
                    const std::string_view runName(runChild.name());
                    const auto runLocalStart = runName.find(':');
                    const auto runLocalName = runLocalStart == std::string_view::npos
                                                  ? runName
                                                  : runName.substr(runLocalStart + 1);
                    if (runLocalName == "t")
                    {
                        text += runChild.text().as_string();
                    }
                }
            }
        }
        return text;
    }

    static std::shared_ptr<Spreadsheet::SharedStringTable> SharedStringTableRoot(const ExcelDocument::Ptr& document,
                                                                                 bool create)
    {
        auto workbookPart = document ? document->GetWorkbookPart() : nullptr;
        if (!workbookPart)
        {
            return nullptr;
        }
        auto part = workbookPart->GetSharedStringTablePart();
        if (!part && create)
        {
            part = workbookPart->AddSharedStringTablePart();
        }
        return part ? part->GetTypedRootElement() : nullptr;
    }

    static std::vector<std::shared_ptr<Spreadsheet::SharedStringItem>> SharedStringItems(
        const std::shared_ptr<Spreadsheet::SharedStringTable>& table)
    {
        return table ? table->Elements<Spreadsheet::SharedStringItem>()
                     : std::vector<std::shared_ptr<Spreadsheet::SharedStringItem>>{};
    }

    static void UpdateSharedStringCounts(const std::shared_ptr<Spreadsheet::SharedStringTable>& table)
    {
        if (!table)
        {
            return;
        }
        const auto count = static_cast<UInt32>(SharedStringItems(table).size());
        table->SetCount(UInt32Value(count));
        table->SetUniqueCount(UInt32Value(count));
    }

    static std::shared_ptr<Spreadsheet::SharedStringItem> AppendPlainSharedString(
        const std::shared_ptr<Spreadsheet::SharedStringTable>& table,
        std::string_view text)
    {
        auto item = table ? table->AppendChild<Spreadsheet::SharedStringItem>() : nullptr;
        auto textElement = item ? item->AppendChild<Spreadsheet::Text>() : nullptr;
        if (!textElement)
        {
            return nullptr;
        }
        textElement->SetText(text);
        return item;
    }

    static std::vector<std::shared_ptr<Spreadsheet::Cell>> WorkbookCells(const ExcelDocument::Ptr& document)
    {
        std::vector<std::shared_ptr<Spreadsheet::Cell>> cells;
        auto workbookPart = document ? document->GetWorkbookPart() : nullptr;
        if (!workbookPart)
        {
            return cells;
        }
        for (const auto& worksheetPart : workbookPart->GetWorksheetParts())
        {
            auto worksheet = worksheetPart ? worksheetPart->GetTypedRootElement() : nullptr;
            if (!worksheet)
            {
                continue;
            }
            for (auto& cell : worksheet->Descendants<Spreadsheet::Cell>())
            {
                if (cell)
                {
                    cells.push_back(cell);
                }
            }
        }
        return cells;
    }

    static bool IsSharedStringCell(const std::shared_ptr<Spreadsheet::Cell>& cell)
    {
        return cell && cell->GetDataType().ValueOr(Spreadsheet::CellValues()).GetValue() ==
                           Spreadsheet::CellValues::SharedString;
    }

    static std::optional<UInt32> SharedStringIndexFromCell(const std::shared_ptr<Spreadsheet::Cell>& cell)
    {
        if (!IsSharedStringCell(cell))
        {
            return std::nullopt;
        }
        return ParseUInt32(CellValueText(cell));
    }

    static bool SetSharedStringIndexOnCell(const std::shared_ptr<Spreadsheet::Cell>& cell, UInt32 index)
    {
        if (!cell)
        {
            return false;
        }
        auto cellValue = FirstCellValueNode(cell);
        if (cellValue)
        {
            cellValue.text().set(std::to_string(index).c_str());
            return true;
        }
        return AppendCellValue(cell, std::to_string(index)) != nullptr;
    }

    static ExcelCellValue ReadCellValue(const std::shared_ptr<Spreadsheet::Cell>& cell,
                                        FormulaReferenceStyle referenceStyle = FormulaReferenceStyle::A1)
    {
        if (!cell)
        {
            return ExcelCellValue::Blank();
        }

        const auto dataType = cell->GetDataType().ValueOr(Spreadsheet::CellValues());
        auto formula = cell->GetFirstChildOfType<Spreadsheet::CellFormula>();
        if (formula)
        {
            const auto cachedText = CellValueText(cell);
            const auto cachedKind = cachedText.empty() ? FormulaCachedValueKind::None : CachedKindFromCellType(dataType.GetValue());
            auto model = CellFormulaValue::Normal(std::string(formula->GetText()), cachedKind, cachedText);
            model.ReferenceStyle = referenceStyle;
            const auto formulaType = formula->GetFormulaType().ValueOr(Spreadsheet::CellFormulaValues()).GetValue();
            if (formulaType == Spreadsheet::CellFormulaValues::Shared)
            {
                model.Kind = CellFormulaKind::Shared;
                const auto sharedIndex = formula->GetSharedIndex();
                if (sharedIndex.IsDefined())
                {
                    model.SharedIndex = sharedIndex.Value();
                }
            }
            else if (formulaType == Spreadsheet::CellFormulaValues::Array)
            {
                model.Kind = CellFormulaKind::Array;
                model.AlwaysCalculateArray = formula->GetAlwaysCalculateArray().ValueOr(false);
            }
            const auto reference = formula->GetReference();
            if (reference.IsDefined())
            {
                model.Reference = reference.ToString();
            }
            return ExcelCellValue::Formula(std::move(model));
        }

        switch (dataType.GetValue())
        {
            case Spreadsheet::CellValues::InlineString:
                return ExcelCellValue::InlineString(InlineStringText(cell));
            case Spreadsheet::CellValues::SharedString:
            {
                const auto text = CellValueText(cell);
                auto index = ParseUInt32(text);
                return index ? ExcelCellValue::SharedString(*index) : ExcelCellValue::SharedString(0);
            }
            case Spreadsheet::CellValues::Boolean:
            {
                const auto text = CellValueText(cell);
                return ExcelCellValue::Boolean(text == "1" || text == "true" || text == "TRUE");
            }
            case Spreadsheet::CellValues::Error:
                return ExcelCellValue::Error(CellValueText(cell));
            case Spreadsheet::CellValues::Date:
                return ExcelCellValue::DateTimeText(CellValueText(cell));
            case Spreadsheet::CellValues::String:
                return ExcelCellValue::InlineString(CellValueText(cell));
            case Spreadsheet::CellValues::Number:
            case Spreadsheet::CellValues::NotDefinedEnumValue:
            default:
            {
                const auto text = CellValueText(cell);
                return text.empty() ? ExcelCellValue::Blank() : ExcelCellValue::NumberText(text);
            }
        }
    }

    static std::shared_ptr<Spreadsheet::Sheets> EnsureWorkbookSheets(const ExcelDocument::Ptr& document)
    {
        if (!document)
        {
            return nullptr;
        }
        auto workbookPart = document->GetWorkbookPart();
        if (!workbookPart)
        {
            return nullptr;
        }
        auto workbook = workbookPart->GetTypedRootElement();
        if (!workbook)
        {
            return nullptr;
        }
        auto sheets = workbook->GetFirstChildOfType<Spreadsheet::Sheets>();
        if (!sheets)
        {
            sheets = workbook->AppendChild<Spreadsheet::Sheets>();
        }
        return sheets;
    }

    static UInt32 NextSheetId(const std::shared_ptr<Spreadsheet::Sheets>& sheets)
    {
        UInt32 nextId = 1;
        if (!sheets)
        {
            return nextId;
        }
        for (const auto& sheet : sheets->Elements<Spreadsheet::Sheet>())
        {
            if (sheet)
            {
                nextId = std::max(nextId, sheet->GetSheetId().ValueOr(0) + 1);
            }
        }
        return nextId;
    }

    static std::vector<std::shared_ptr<Spreadsheet::Sheet>> SheetElements(const ExcelDocument::Ptr& document)
    {
        std::vector<std::shared_ptr<Spreadsheet::Sheet>> result;
        auto sheets = EnsureWorkbookSheets(document);
        if (!sheets)
        {
            return result;
        }
        for (const auto& sheet : sheets->Elements<Spreadsheet::Sheet>())
        {
            if (sheet)
            {
                result.push_back(sheet);
            }
        }
        return result;
    }

    static std::shared_ptr<Spreadsheet::Sheet> GetSheetElement(const ExcelDocument::Ptr& document, Size index)
    {
        auto sheets = SheetElements(document);
        return index < sheets.size() ? sheets[index] : nullptr;
    }

    static bool WorksheetNameExists(const ExcelDocument::Ptr& document,
                                    std::string_view name,
                                    const std::shared_ptr<Spreadsheet::Sheet>& except = nullptr)
    {
        const auto desired = AsciiText::ToLower(name);
        for (const auto& sheet : SheetElements(document))
        {
            if (sheet != except && AsciiText::ToLower(sheet->GetName().ToString()) == desired)
            {
                return true;
            }
        }
        return false;
    }

    static std::string MakeUniqueWorksheetName(const ExcelDocument::Ptr& document, std::string_view baseName)
    {
        std::string base = baseName.empty() ? "Sheet" : std::string(baseName);
        if (base.size() > 31)
        {
            base.resize(31);
        }
        if (!IsValidWorksheetName(base))
        {
            base = "Sheet";
        }
        if (!WorksheetNameExists(document, base))
        {
            return base;
        }

        for (UInt32 suffix = 2; suffix < 100000; ++suffix)
        {
            const auto suffixText = " (" + std::to_string(suffix) + ")";
            auto prefix = base;
            if (prefix.size() + suffixText.size() > 31)
            {
                prefix.resize(31 - suffixText.size());
            }
            auto candidate = prefix + suffixText;
            if (!WorksheetNameExists(document, candidate))
            {
                return candidate;
            }
        }
        return {};
    }

    static std::shared_ptr<Packaging::WorksheetPart> FindWorksheetPartByRelationshipId(const ExcelDocument::Ptr& document,
                                                                                       std::string_view relationshipId)
    {
        auto workbookPart = document ? document->GetWorkbookPart() : nullptr;
        if (!workbookPart)
        {
            return nullptr;
        }
        for (const auto& worksheetPart : workbookPart->GetWorksheetParts())
        {
            if (!worksheetPart)
            {
                continue;
            }
            if (worksheetPart->RelationshipId() == relationshipId)
            {
                return worksheetPart;
            }
        }
        return nullptr;
    }

    static Worksheet::Ptr WrapWorksheet(const ExcelDocument::Ptr& document, const std::shared_ptr<Spreadsheet::Sheet>& sheet)
    {
        if (!sheet)
        {
            return nullptr;
        }
        auto part = FindWorksheetPartByRelationshipId(document, sheet->GetId().ToString());
        return part ? std::make_shared<Worksheet>(sheet->GetName().ToString(), part, document) : nullptr;
    }

    class RangeBulkHelpers final
    {
    public:
        RangeBulkHelpers() = delete;

        struct PlannedCell
        {
            CellAddress address;
            ExcelCellValue value;
        };

        static RangeOperationResult Error(RangeOperationError error, std::string message)
        {
            return RangeOperationResult{error, std::move(message), 0};
        }

        static RangeOperationResult Success(Size affectedCellCount)
        {
            return RangeOperationResult{RangeOperationError::None, {}, affectedCellCount};
        }

        static bool MatrixMatches(CellRange range, const ExcelCellMatrix& values)
        {
            if (!range.IsValid() || values.size() != range.RowCount())
            {
                return false;
            }
            return std::all_of(values.begin(), values.end(), [&](const auto& row)
                               { return row.size() == range.ColumnCount(); });
        }

        static std::optional<CellRange> DestinationRange(CellRange source, CellAddress destinationTopLeft)
        {
            if (!source.IsValid() || !destinationTopLeft.IsValid())
            {
                return std::nullopt;
            }
            const auto lastRow = static_cast<UInt64>(destinationTopLeft.Row().Value()) + source.RowCount() - 1;
            const auto lastColumn =
                static_cast<UInt64>(destinationTopLeft.Column().Value()) + source.ColumnCount() - 1;
            if (lastRow > MaxRowIndex || lastColumn > MaxColumnIndex)
            {
                return std::nullopt;
            }
            const auto last = CellAddress::TryCreate(static_cast<UInt32>(lastRow),
                                                     static_cast<UInt32>(lastColumn));
            return last ? CellRange::TryCreate(destinationTopLeft, *last) : std::nullopt;
        }

        static UInt64 AddressKey(CellAddress address) noexcept
        {
            return (static_cast<UInt64>(address.Row().Value()) << 16) | address.Column().Value();
        }

        static void AddOrReplace(std::vector<PlannedCell>& plan,
                                 std::unordered_map<UInt64, Size>& indexes,
                                 CellAddress address,
                                 const ExcelCellValue& value)
        {
            const auto key = AddressKey(address);
            if (const auto existing = indexes.find(key); existing != indexes.end())
            {
                plan[existing->second].value = value;
                return;
            }
            indexes.emplace(key, plan.size());
            plan.push_back(PlannedCell{address, value});
        }

        static std::vector<PlannedCell> MatrixPlan(CellRange range, const ExcelCellMatrix& values)
        {
            std::vector<PlannedCell> plan;
            plan.reserve(static_cast<Size>(range.RowCount()) * range.ColumnCount());
            for (UInt32 rowOffset = 0; rowOffset < range.RowCount(); ++rowOffset)
            {
                for (UInt32 columnOffset = 0; columnOffset < range.ColumnCount(); ++columnOffset)
                {
                    const auto address = CellAddress::TryCreate(range.First().Row().Value() + rowOffset,
                                                                range.First().Column().Value() + columnOffset);
                    if (address)
                    {
                        plan.push_back(PlannedCell{*address, values[rowOffset][columnOffset]});
                    }
                }
            }
            return plan;
        }

        static bool ApplyValue(Worksheet& worksheet, CellAddress address, const ExcelCellValue& value)
        {
            if (value.IsBlank())
            {
                return !worksheet.ContainsCell(address) || worksheet.RemoveCell(address);
            }
            return worksheet.SetCellValue(address, value);
        }

        static RangeOperationResult ApplyAtomically(Worksheet& worksheet, const std::vector<PlannedCell>& plan)
        {
            const auto part = worksheet.GetPart();
            if (!part)
            {
                return Error(RangeOperationError::InvalidWorksheet, "The worksheet is not attached to a usable part.");
            }
            const auto worksheetXml = part->GetXmlString();

            for (const auto& cell : plan)
            {
                if (ApplyValue(worksheet, cell.address, cell.value))
                {
                    continue;
                }
                part->SetXmlString(worksheetXml);
                return Error(RangeOperationError::WriteFailed,
                             "A cell write failed and all changes made by the range operation were rolled back.");
            }
            return Success(plan.size());
        }
    };

    /** @brief File-local validation and workbook metadata support for formula models. */
    class FormulaModelHelpers final
    {
    public:
        FormulaModelHelpers() = delete;

        static bool RangeContains(std::string_view text, CellAddress address)
        {
            const auto range = CellRange::ParseA1(text);
            return range && address.Row().Value() >= range->First().Row().Value() &&
                   address.Row().Value() <= range->Last().Row().Value() &&
                   address.Column().Value() >= range->First().Column().Value() &&
                   address.Column().Value() <= range->Last().Column().Value();
        }

        static bool IsValid(const CellFormulaValue& formula, CellAddress address)
        {
            if (formula.CachedKind == FormulaCachedValueKind::None && !formula.CachedText.empty())
            {
                return false;
            }
            switch (formula.Kind)
            {
                case CellFormulaKind::Normal:
                    return !formula.Formula.empty() && !formula.Reference && !formula.SharedIndex &&
                           !formula.AlwaysCalculateArray;
                case CellFormulaKind::Shared:
                    if (!formula.SharedIndex || formula.AlwaysCalculateArray)
                    {
                        return false;
                    }
                    return formula.Reference ? !formula.Formula.empty() && RangeContains(*formula.Reference, address)
                                             : formula.Formula.empty();
                case CellFormulaKind::Array:
                    return !formula.Formula.empty() && !formula.SharedIndex && formula.Reference &&
                           RangeContains(*formula.Reference, address);
            }
            return false;
        }

        static std::shared_ptr<Spreadsheet::CalculationProperties> CalculationProperties(
            const ExcelDocument::Ptr& document, bool create)
        {
            auto workbookPart = document ? document->GetWorkbookPart() : nullptr;
            auto workbook = workbookPart ? workbookPart->GetTypedRootElement() : nullptr;
            if (!workbook)
            {
                return nullptr;
            }
            auto properties = workbook->GetFirstChildOfType<Spreadsheet::CalculationProperties>();
            return properties || !create ? properties : workbook->AppendChild<Spreadsheet::CalculationProperties>();
        }

        static FormulaReferenceStyle ReferenceStyle(const ExcelDocument::Ptr& document)
        {
            const auto properties = CalculationProperties(document, false);
            const auto value = properties
                                   ? properties->GetReferenceMode().ValueOr(Spreadsheet::ReferenceModeValues()).GetValue()
                                   : Spreadsheet::ReferenceModeValues::NotDefinedEnumValue;
            return value == Spreadsheet::ReferenceModeValues::R1C1 ? FormulaReferenceStyle::R1C1
                                                                   : FormulaReferenceStyle::A1;
        }

        static bool SetReferenceStyle(const ExcelDocument::Ptr& document, FormulaReferenceStyle style)
        {
            auto properties = CalculationProperties(document, true);
            if (!properties)
            {
                return false;
            }
            const auto value = style == FormulaReferenceStyle::R1C1 ? Spreadsheet::ReferenceModeValues::R1C1
                                                                    : Spreadsheet::ReferenceModeValues::A1;
            properties->SetReferenceMode(EnumValue<Spreadsheet::ReferenceModeValues>(value));
            return true;
        }
    };
};

Worksheet::Worksheet(std::string name,
                     std::shared_ptr<ExyokiOffice::Packaging::WorksheetPart> part,
                     ExcelDocument::Ptr document)
    : m_name(std::move(name)),
      m_part(std::move(part)),
      m_document(std::move(document))
{
}

std::string Worksheet::Name() const
{
    return m_name;
}

bool Worksheet::SetCellText(UInt32 row, UInt32 column, std::string_view value)
{
    auto address = CellAddress::TryCreate(row, column);
    return address ? SetCellText(*address, value) : false;
}

bool Worksheet::SetCellText(CellAddress address, std::string_view value)
{
    SharedStringTableService sharedStrings(m_document);
    auto index = sharedStrings.GetOrAdd(value);
    return index ? SetCellValue(address, ExcelCellValue::SharedString(*index)) : false;
}

bool Worksheet::SetCellNumber(UInt32 row, UInt32 column, Real value)
{
    auto address = CellAddress::TryCreate(row, column);
    return address ? SetCellNumber(*address, value) : false;
}

bool Worksheet::SetCellNumber(CellAddress address, Real value)
{
    return SetCellValue(address, ExcelCellValue::Number(value));
}

bool Worksheet::SetCellValue(CellAddress address, const ExcelCellValue& value)
{
    if (!address.IsValid())
    {
        return false;
    }
    if (value.Kind() == CellValueKind::Formula &&
        (!ExcelDocumentXmlHelper::FormulaModelHelpers::IsValid(value.FormulaValue(), address) ||
         !ExcelDocumentXmlHelper::FormulaModelHelpers::SetReferenceStyle(m_document, value.FormulaValue().ReferenceStyle)))
    {
        return false;
    }
    auto worksheet = GetLowLevelApi();
    auto cell = ExcelDocumentXmlHelper::FindCell(worksheet, address);
    if (!cell)
    {
        cell = ExcelDocumentXmlHelper::EnsureCell(ExcelDocumentXmlHelper::EnsureRow(ExcelDocumentXmlHelper::EnsureSheetData(worksheet), address.Row().Value()),
                                                  address.Row().Value(),
                                                  address.Column().Value());
    }
    if (!cell)
    {
        return false;
    }

    return ExcelDocumentXmlHelper::WriteCellValue(cell, value);
}

bool Worksheet::SetCellValue(UInt32 row, UInt32 column, const ExcelCellValue& value)
{
    auto address = CellAddress::TryCreate(row, column);
    return address ? SetCellValue(*address, value) : false;
}

std::optional<ExcelCellValue> Worksheet::GetCellValue(CellAddress address) const
{
    if (!address.IsValid())
    {
        return std::nullopt;
    }
    auto worksheet = GetLowLevelApi();
    return worksheet ? std::optional<ExcelCellValue>(
                           ExcelDocumentXmlHelper::ReadCellValue(ExcelDocumentXmlHelper::FindCell(worksheet, address), ExcelDocumentXmlHelper::FormulaModelHelpers::ReferenceStyle(m_document)))
                     : std::nullopt;
}

std::optional<ExcelCellValue> Worksheet::GetCellValue(UInt32 row, UInt32 column) const
{
    auto address = CellAddress::TryCreate(row, column);
    return address ? GetCellValue(*address) : std::nullopt;
}

bool Worksheet::ContainsCell(CellAddress address) const
{
    return address.IsValid() && ExcelDocumentXmlHelper::FindCell(GetLowLevelApi(), address) != nullptr;
}

bool Worksheet::ContainsCell(UInt32 row, UInt32 column) const
{
    const auto address = CellAddress::TryCreate(row, column);
    return address && ContainsCell(*address);
}

bool Worksheet::RemoveCell(CellAddress address)
{
    if (!address.IsValid())
    {
        return false;
    }
    auto worksheet = GetLowLevelApi();
    auto cell = ExcelDocumentXmlHelper::FindCell(worksheet, address);
    auto row = cell ? openxmlelement_cast<Spreadsheet::Row>(cell->Parent()) : nullptr;
    if (!row || !row->RemoveChild(cell))
    {
        return false;
    }

    const auto rowNode = Detail::NodeOf(row);
    bool hasContent = false;
    if (rowNode)
    {
        for (auto child = rowNode.first_child(); child; child = child.next_sibling())
        {
            if (child.type() == Pugi::node_element)
            {
                hasContent = true;
                break;
            }
        }
        Size attributeCount = 0;
        for (auto attribute = rowNode.first_attribute(); attribute; attribute = attribute.next_attribute())
        {
            ++attributeCount;
        }
        hasContent = hasContent || attributeCount > (row->GetRowIndex().IsDefined() ? 1u : 0u);
    }
    auto parent = row->Parent();
    if (!hasContent && parent)
    {
        parent->RemoveChild(row);
    }
    return true;
}

bool Worksheet::RemoveCell(UInt32 row, UInt32 column)
{
    const auto address = CellAddress::TryCreate(row, column);
    return address && RemoveCell(*address);
}

Size Worksheet::StoredCellCount() const
{
    Size count = 0;
    auto worksheet = GetLowLevelApi();
    const auto worksheetNode = Detail::NodeOf(worksheet);
    if (!worksheetNode)
    {
        return count;
    }
    for (auto child = worksheetNode.first_child(); child; child = child.next_sibling())
    {
        if (!ExcelDocumentXmlHelper::SpreadsheetXmlHelpers::NodeHasName<Spreadsheet::SheetData>(child))
        {
            continue;
        }
        for (auto row = child.first_child(); row; row = row.next_sibling())
        {
            if (!ExcelDocumentXmlHelper::SpreadsheetXmlHelpers::NodeHasName<Spreadsheet::Row>(row))
            {
                continue;
            }
            for (auto cell = row.first_child(); cell; cell = cell.next_sibling())
            {
                if (ExcelDocumentXmlHelper::SpreadsheetXmlHelpers::NodeHasName<Spreadsheet::Cell>(cell) &&
                    CellAddress::ParseA1(ExcelDocumentXmlHelper::RawCellReference(ExcelDocumentXmlHelper::WrapCellNode(cell))))
                {
                    ++count;
                }
            }
        }
        break;
    }
    return count;
}

std::vector<CellAddress> Worksheet::StoredCellAddresses() const
{
    std::vector<CellAddress> result;
    auto worksheet = GetLowLevelApi();
    const auto worksheetNode = Detail::NodeOf(worksheet);
    if (!worksheetNode)
    {
        return result;
    }
    Pugi::xml_node sheetData;
    for (auto child = worksheetNode.first_child(); child; child = child.next_sibling())
    {
        if (ExcelDocumentXmlHelper::SpreadsheetXmlHelpers::NodeHasName<Spreadsheet::SheetData>(child))
        {
            sheetData = child;
            break;
        }
    }
    for (auto row = sheetData ? sheetData.first_child() : Pugi::xml_node{}; row; row = row.next_sibling())
    {
        if (!ExcelDocumentXmlHelper::SpreadsheetXmlHelpers::NodeHasName<Spreadsheet::Row>(row))
        {
            continue;
        }
        for (auto cell = row.first_child(); cell; cell = cell.next_sibling())
        {
            if (!ExcelDocumentXmlHelper::SpreadsheetXmlHelpers::NodeHasName<Spreadsheet::Cell>(cell))
            {
                continue;
            }
            const auto address = CellAddress::ParseA1(ExcelDocumentXmlHelper::RawCellReference(ExcelDocumentXmlHelper::WrapCellNode(cell)));
            if (address)
            {
                result.push_back(*address);
            }
        }
    }
    return result;
}

RangeReadResult Worksheet::GetRangeValues(CellRange range) const
{
    RangeReadResult result;
    if (!range.IsValid())
    {
        result.Status = ExcelDocumentXmlHelper::RangeBulkHelpers::Error(RangeOperationError::InvalidAddress,
                                                                        "The source range is invalid or not normalized.");
        return result;
    }
    if (!GetLowLevelApi())
    {
        result.Status = ExcelDocumentXmlHelper::RangeBulkHelpers::Error(RangeOperationError::InvalidWorksheet,
                                                                        "The worksheet is not attached to a usable part.");
        return result;
    }

    result.Values.reserve(range.RowCount());
    for (UInt32 rowOffset = 0; rowOffset < range.RowCount(); ++rowOffset)
    {
        std::vector<ExcelCellValue> row;
        row.reserve(range.ColumnCount());
        for (UInt32 columnOffset = 0; columnOffset < range.ColumnCount(); ++columnOffset)
        {
            const auto address = CellAddress::TryCreate(range.First().Row().Value() + rowOffset,
                                                        range.First().Column().Value() + columnOffset);
            const auto value = address ? GetCellValue(*address) : std::nullopt;
            if (!value)
            {
                result.Values.clear();
                result.Status = ExcelDocumentXmlHelper::RangeBulkHelpers::Error(RangeOperationError::InvalidWorksheet,
                                                                                "A cell could not be read from the worksheet.");
                return result;
            }
            row.push_back(*value);
        }
        result.Values.push_back(std::move(row));
    }
    result.Status = ExcelDocumentXmlHelper::RangeBulkHelpers::Success(
        static_cast<Size>(range.RowCount()) * range.ColumnCount());
    return result;
}

RangeOperationResult Worksheet::SetRangeValues(CellRange range, const ExcelCellMatrix& values)
{
    if (!range.IsValid())
    {
        return ExcelDocumentXmlHelper::RangeBulkHelpers::Error(RangeOperationError::InvalidAddress,
                                                               "The target range is invalid or not normalized.");
    }
    if (!ExcelDocumentXmlHelper::RangeBulkHelpers::MatrixMatches(range, values))
    {
        return ExcelDocumentXmlHelper::RangeBulkHelpers::Error(
            RangeOperationError::DimensionMismatch,
            "The value matrix must be rectangular and exactly match the target range dimensions.");
    }
    if (!GetLowLevelApi())
    {
        return ExcelDocumentXmlHelper::RangeBulkHelpers::Error(RangeOperationError::InvalidWorksheet,
                                                               "The worksheet is not attached to a usable part.");
    }
    return ExcelDocumentXmlHelper::RangeBulkHelpers::ApplyAtomically(*this, ExcelDocumentXmlHelper::RangeBulkHelpers::MatrixPlan(range, values));
}

RangeOperationResult Worksheet::ClearRange(CellRange range)
{
    if (!range.IsValid())
    {
        return ExcelDocumentXmlHelper::RangeBulkHelpers::Error(RangeOperationError::InvalidAddress,
                                                               "The target range is invalid or not normalized.");
    }
    if (!GetLowLevelApi())
    {
        return ExcelDocumentXmlHelper::RangeBulkHelpers::Error(RangeOperationError::InvalidWorksheet,
                                                               "The worksheet is not attached to a usable part.");
    }

    std::vector<ExcelDocumentXmlHelper::RangeBulkHelpers::PlannedCell> plan;
    for (const auto address : StoredCellAddresses())
    {
        if (address.Row().Value() >= range.First().Row().Value() &&
            address.Row().Value() <= range.Last().Row().Value() &&
            address.Column().Value() >= range.First().Column().Value() &&
            address.Column().Value() <= range.Last().Column().Value())
        {
            plan.push_back({address, ExcelCellValue::Blank()});
        }
    }
    return ExcelDocumentXmlHelper::RangeBulkHelpers::ApplyAtomically(*this, plan);
}

RangeOperationResult Worksheet::FillRange(CellRange range, const ExcelCellValue& value)
{
    if (!range.IsValid())
    {
        return ExcelDocumentXmlHelper::RangeBulkHelpers::Error(RangeOperationError::InvalidAddress,
                                                               "The target range is invalid or not normalized.");
    }
    if (value.IsBlank())
    {
        return ClearRange(range);
    }
    ExcelCellMatrix values(range.RowCount(), std::vector<ExcelCellValue>(range.ColumnCount(), value));
    return SetRangeValues(range, values);
}

RangeOperationResult Worksheet::CopyRange(CellRange source, CellAddress destinationTopLeft)
{
    if (!source.IsValid() || !destinationTopLeft.IsValid())
    {
        return ExcelDocumentXmlHelper::RangeBulkHelpers::Error(RangeOperationError::InvalidAddress,
                                                               "The source range and destination address must be valid.");
    }
    const auto destination = ExcelDocumentXmlHelper::RangeBulkHelpers::DestinationRange(source, destinationTopLeft);
    if (!destination)
    {
        return ExcelDocumentXmlHelper::RangeBulkHelpers::Error(RangeOperationError::DestinationOutOfBounds,
                                                               "The destination rectangle extends beyond the Excel worksheet grid.");
    }
    auto sourceValues = GetRangeValues(source);
    if (!sourceValues)
    {
        return sourceValues.Status;
    }
    return SetRangeValues(*destination, sourceValues.Values);
}

RangeOperationResult Worksheet::MoveRange(CellRange source, CellAddress destinationTopLeft)
{
    if (!source.IsValid() || !destinationTopLeft.IsValid())
    {
        return ExcelDocumentXmlHelper::RangeBulkHelpers::Error(RangeOperationError::InvalidAddress,
                                                               "The source range and destination address must be valid.");
    }
    const auto destination = ExcelDocumentXmlHelper::RangeBulkHelpers::DestinationRange(source, destinationTopLeft);
    if (!destination)
    {
        return ExcelDocumentXmlHelper::RangeBulkHelpers::Error(RangeOperationError::DestinationOutOfBounds,
                                                               "The destination rectangle extends beyond the Excel worksheet grid.");
    }
    auto sourceValues = GetRangeValues(source);
    if (!sourceValues)
    {
        return sourceValues.Status;
    }
    const auto originalXml = m_part ? m_part->GetXmlString() : std::string{};

    std::vector<ExcelDocumentXmlHelper::RangeBulkHelpers::PlannedCell> plan;
    std::unordered_map<UInt64, Size> indexes;
    const auto sourceBlank = ExcelCellValue::Blank();
    for (UInt32 rowOffset = 0; rowOffset < source.RowCount(); ++rowOffset)
    {
        for (UInt32 columnOffset = 0; columnOffset < source.ColumnCount(); ++columnOffset)
        {
            const auto sourceAddress = CellAddress::TryCreate(source.First().Row().Value() + rowOffset,
                                                              source.First().Column().Value() + columnOffset);
            if (sourceAddress)
            {
                ExcelDocumentXmlHelper::RangeBulkHelpers::AddOrReplace(plan, indexes, *sourceAddress, sourceBlank);
            }
        }
    }
    for (UInt32 rowOffset = 0; rowOffset < source.RowCount(); ++rowOffset)
    {
        for (UInt32 columnOffset = 0; columnOffset < source.ColumnCount(); ++columnOffset)
        {
            const auto destinationAddress = CellAddress::TryCreate(destination->First().Row().Value() + rowOffset,
                                                                   destination->First().Column().Value() + columnOffset);
            if (destinationAddress)
            {
                ExcelDocumentXmlHelper::RangeBulkHelpers::AddOrReplace(
                    plan, indexes, *destinationAddress, sourceValues.Values[rowOffset][columnOffset]);
            }
        }
    }
    auto status = ExcelDocumentXmlHelper::RangeBulkHelpers::ApplyAtomically(*this, plan);
    if (!status)
    {
        return status;
    }

    const auto transform = FormulaReferenceTransform::MoveRange(source, destinationTopLeft);
    for (const auto address : StoredCellAddresses())
    {
        auto formula = GetCellFormula(address);
        if (!formula || formula->ReferenceStyle != FormulaReferenceStyle::A1)
        {
            continue;
        }
        const auto rewritten = FormulaReferenceRewriter::RewriteA1(formula->Formula, Name(), transform);
        if (!rewritten)
        {
            m_part->SetXmlString(originalXml);
            return ExcelDocumentXmlHelper::RangeBulkHelpers::Error(RangeOperationError::ReferenceUpdateFailed, rewritten.ErrorMessage);
        }
        formula->Formula = rewritten.Formula;
        if (formula->Reference)
        {
            const auto rewrittenRange = FormulaReferenceRewriter::RewriteA1(*formula->Reference, Name(), transform);
            if (!rewrittenRange || rewrittenRange.Formula == "#REF!")
            {
                m_part->SetXmlString(originalXml);
                return ExcelDocumentXmlHelper::RangeBulkHelpers::Error(RangeOperationError::ReferenceUpdateFailed,
                                                                       "A formula-owned range could not be moved safely.");
            }
            formula->Reference = rewrittenRange.Formula;
        }
        if (!SetCellFormula(address, *formula))
        {
            m_part->SetXmlString(originalXml);
            return ExcelDocumentXmlHelper::RangeBulkHelpers::Error(RangeOperationError::WriteFailed,
                                                                   "A moved formula could not be written; the worksheet was restored.");
        }
    }
    return status;
}

RangeOperationResult Worksheet::InsertRows(UInt32 beforeRow,
                                           UInt32 count,
                                           FormulaReferenceUpdatePolicy formulaPolicy)
{
    return WorksheetStructureHelpers::InsertRows(*this, beforeRow, count, formulaPolicy);
}

RangeOperationResult Worksheet::DeleteRows(UInt32 firstRow,
                                           UInt32 count,
                                           FormulaReferenceUpdatePolicy formulaPolicy)
{
    return WorksheetStructureHelpers::DeleteRows(*this, firstRow, count, formulaPolicy);
}

RangeOperationResult Worksheet::InsertColumns(UInt32 beforeColumn,
                                              UInt32 count,
                                              FormulaReferenceUpdatePolicy formulaPolicy)
{
    return WorksheetStructureHelpers::InsertColumns(*this, beforeColumn, count, formulaPolicy);
}

RangeOperationResult Worksheet::DeleteColumns(UInt32 firstColumn,
                                              UInt32 count,
                                              FormulaReferenceUpdatePolicy formulaPolicy)
{
    return WorksheetStructureHelpers::DeleteColumns(*this, firstColumn, count, formulaPolicy);
}

std::vector<CellRange> Worksheet::MergedRanges() const
{
    return WorksheetMergeHelpers::MergedRanges(*this);
}

std::optional<CellRange> Worksheet::MergedRangeAt(CellAddress address) const
{
    if (!address.IsValid())
    {
        return std::nullopt;
    }
    for (const auto range : MergedRanges())
    {
        if (address.Row().Value() >= range.First().Row().Value() &&
            address.Row().Value() <= range.Last().Row().Value() &&
            address.Column().Value() >= range.First().Column().Value() &&
            address.Column().Value() <= range.Last().Column().Value())
        {
            return range;
        }
    }
    return std::nullopt;
}

RangeOperationResult Worksheet::MergeRange(CellRange range)
{
    return WorksheetMergeHelpers::MergeRange(*this, range);
}

RangeOperationResult Worksheet::UnmergeRange(CellRange range)
{
    return WorksheetMergeHelpers::UnmergeRange(*this, range);
}

bool Worksheet::SetCellBoolean(CellAddress address, bool value)
{
    return SetCellValue(address, ExcelCellValue::Boolean(value));
}

bool Worksheet::SetCellError(CellAddress address, std::string_view errorText)
{
    return SetCellValue(address, ExcelCellValue::Error(std::string(errorText)));
}

bool Worksheet::SetCellDateTimeText(CellAddress address, std::string_view value)
{
    return SetCellValue(address, ExcelCellValue::DateTimeText(std::string(value)));
}

bool Worksheet::SetCellFormula(CellAddress address,
                               std::string_view formula,
                               FormulaCachedValueKind cachedKind,
                               std::string_view cachedText)
{
    return SetCellFormula(address,
                          CellFormulaValue::Normal(std::string(formula), cachedKind, std::string(cachedText)));
}

bool Worksheet::SetCellFormula(CellAddress address, const CellFormulaValue& formula)
{
    return SetCellValue(address, ExcelCellValue::Formula(formula));
}

std::optional<CellFormulaValue> Worksheet::GetCellFormula(CellAddress address) const
{
    const auto value = GetCellValue(address);
    if (!value || value->Kind() != CellValueKind::Formula)
    {
        return std::nullopt;
    }
    auto formula = value->FormulaValue();
    formula.ReferenceStyle = ExcelDocumentXmlHelper::FormulaModelHelpers::ReferenceStyle(m_document);
    return formula;
}

ExcelTable::Ptr Worksheet::CreateTable(std::string_view name,
                                       CellRange range,
                                       const std::vector<ExcelTableColumn>& columns)
{
    if (!m_part || !m_document || !range.IsValid() || columns.size() != range.ColumnCount() ||
        !IsValidExcelTableName(name))
    {
        return nullptr;
    }
    const auto workbookPart = m_document->GetWorkbookPart();
    if (!workbookPart)
    {
        return nullptr;
    }

    UInt32 nextId = 1;
    for (const auto& worksheetPart : workbookPart->GetWorksheetParts())
    {
        for (const auto& part : worksheetPart->GetTableDefinitionParts())
        {
            const auto existing = std::make_shared<ExcelTable>(part);
            nextId = std::max(nextId, existing->Id() + 1);
            if (AsciiText::ToLower(existing->Name()) == AsciiText::ToLower(name))
            {
                return nullptr;
            }
        }
    }

    const auto part = m_part->AddTableDefinitionPart();
    const auto table = part ? part->GetTable() : nullptr;
    if (!part || !table)
    {
        return nullptr;
    }
    table->SetId(UInt32Value(nextId));
    table->SetReference(StringValue(range.ToA1()));
    table->SetHeaderRowCount(UInt32Value(1));
    table->SetTotalsRowCount(UInt32Value(0));
    table->SetTotalsRowShown(BooleanValue(false));
    auto result = std::make_shared<ExcelTable>(part);
    if (!result->SetName(name) || !result->SetColumns(columns) || !result->SetAutoFilterEnabled(true))
    {
        m_part->RemoveTableDefinitionPart(part);
        return nullptr;
    }

    const auto root = GetLowLevelApi();
    auto tableParts = root ? root->GetFirstChildOfType<Spreadsheet::TableParts>() : nullptr;
    if (!tableParts)
    {
        tableParts = root ? root->AppendChild<Spreadsheet::TableParts>() : nullptr;
    }
    const auto registryEntry = tableParts ? tableParts->AppendChild<Spreadsheet::TablePart>() : nullptr;
    if (!registryEntry)
    {
        m_part->RemoveTableDefinitionPart(part);
        return nullptr;
    }
    registryEntry->SetId(StringValue(part->RelationshipId()));
    tableParts->SetCount(UInt32Value(static_cast<UInt32>(tableParts->Elements<Spreadsheet::TablePart>().size())));
    return result;
}

std::vector<ExcelTable::Ptr> Worksheet::Tables() const
{
    std::vector<ExcelTable::Ptr> result;
    if (!m_part)
    {
        return result;
    }
    for (const auto& part : m_part->GetTableDefinitionParts())
    {
        result.push_back(std::make_shared<ExcelTable>(part));
    }
    return result;
}

ExcelTable::Ptr Worksheet::TableByName(std::string_view name) const
{
    for (const auto& table : Tables())
    {
        if (AsciiText::ToLower(table->Name()) == AsciiText::ToLower(name))
        {
            return table;
        }
    }
    return nullptr;
}

bool Worksheet::RenameTable(const ExcelTable::Ptr& table, std::string_view newName)
{
    if (!table || !m_part || !m_document || !IsValidExcelTableName(newName))
    {
        return false;
    }
    const auto ownedPart = table->GetPart();
    const auto parts = m_part->GetTableDefinitionParts();
    if (std::find(parts.begin(), parts.end(), ownedPart) == parts.end())
    {
        return false;
    }
    const auto workbookPart = m_document->GetWorkbookPart();
    if (!workbookPart)
    {
        return false;
    }
    for (const auto& worksheetPart : workbookPart->GetWorksheetParts())
    {
        for (const auto& candidatePart : worksheetPart->GetTableDefinitionParts())
        {
            if (candidatePart != ownedPart &&
                AsciiText::ToLower(std::make_shared<ExcelTable>(candidatePart)->Name()) == AsciiText::ToLower(newName))
            {
                return false;
            }
        }
    }
    return table->SetName(newName);
}

bool Worksheet::RemoveTable(const ExcelTable::Ptr& table)
{
    if (!table || !m_part)
    {
        return false;
    }
    const auto part = table->GetPart();
    const auto parts = m_part->GetTableDefinitionParts();
    if (std::find(parts.begin(), parts.end(), part) == parts.end())
    {
        return false;
    }
    const auto root = GetLowLevelApi();
    const auto tableParts = root ? root->GetFirstChildOfType<Spreadsheet::TableParts>() : nullptr;
    if (!tableParts)
    {
        return false;
    }
    const auto tableId = table->Id();
    const auto originalWorksheetXml = m_part->GetXmlString();
    std::shared_ptr<Spreadsheet::TablePart> registryEntry;
    for (const auto& candidate : tableParts->Elements<Spreadsheet::TablePart>())
    {
        if (candidate->GetId().ToString() == part->RelationshipId())
        {
            registryEntry = candidate;
        }
    }
    if (!registryEntry || !tableParts->RemoveChild(registryEntry))
    {
        return false;
    }
    if (!m_part->RemoveTableDefinitionPart(part))
    {
        m_part->SetXmlString(originalWorksheetXml);
        return false;
    }
    const auto remaining = tableParts->Elements<Spreadsheet::TablePart>();
    if (remaining.empty())
    {
        root->RemoveChild(tableParts);
    }
    else
    {
        tableParts->SetCount(UInt32Value(static_cast<UInt32>(remaining.size())));
    }
    // A table slicer's cache points at the table by identifier, so removing the
    // table would otherwise orphan the whole slicer chain built on it.
    SlicerDetail::DetachTableFromSlicers(m_document, tableId);
    return true;
}

ExcelDataValidation::Ptr Worksheet::CreateDataValidation(const ExcelDataValidationDefinition& definition)
{
    if (!IsValidExcelDataValidation(definition))
    {
        return nullptr;
    }
    const auto root = GetLowLevelApi();
    if (!root)
    {
        return nullptr;
    }
    auto container = root->GetFirstChildOfType<Spreadsheet::DataValidations>();
    if (!container)
    {
        // New workbooks already contain page setup metadata, which follows
        // dataValidations in the worksheet sequence.
        const auto before = root->GetFirstChildOfType<Spreadsheet::PageMargins>();
        container = before ? root->InsertChild<Spreadsheet::DataValidations>(before)
                           : root->AppendChild<Spreadsheet::DataValidations>();
    }
    const auto element = container ? container->AppendChild<Spreadsheet::DataValidation>() : nullptr;
    if (!element)
    {
        return nullptr;
    }
    auto result = ExcelDataValidation::Ptr(new ExcelDataValidation(element, m_part));
    if (!UpdateDataValidation(result, definition))
    {
        container->RemoveChild(element);
        if (container->Elements<Spreadsheet::DataValidation>().empty())
        {
            root->RemoveChild(container);
        }
        return nullptr;
    }
    return result;
}

std::vector<ExcelDataValidation::Ptr> Worksheet::DataValidations() const
{
    std::vector<ExcelDataValidation::Ptr> result;
    const auto root = GetLowLevelApi();
    const auto container = root ? root->GetFirstChildOfType<Spreadsheet::DataValidations>() : nullptr;
    if (container)
    {
        for (const auto& item : container->Elements<Spreadsheet::DataValidation>())
        {
            result.push_back(ExcelDataValidation::Ptr(new ExcelDataValidation(item, m_part)));
        }
    }
    return result;
}

bool Worksheet::UpdateDataValidation(const ExcelDataValidation::Ptr& validation,
                                     const ExcelDataValidationDefinition& d)
{
    if (!validation || !IsValidExcelDataValidation(d))
    {
        return false;
    }
    const auto root = GetLowLevelApi();
    const auto container = root ? root->GetFirstChildOfType<Spreadsheet::DataValidations>() : nullptr;
    const auto element = validation->GetLowLevelApi();
    if (!container || !element || validation->m_owner.lock() != m_part)
    {
        return false;
    }
    const auto owned = container->Elements<Spreadsheet::DataValidation>();

    using Type = Spreadsheet::DataValidationValues;
    using Op = Spreadsheet::DataValidationOperatorValues;
    using Style = Spreadsheet::DataValidationErrorStyleValues;
    static constexpr Type::Value types[] = {Type::None, Type::Whole, Type::Decimal, Type::List,
                                            Type::Date, Type::Time, Type::TextLength, Type::Custom};
    static constexpr Op::Value ops[] = {Op::Between, Op::NotBetween, Op::Equal, Op::NotEqual,
                                        Op::LessThan, Op::LessThanOrEqual, Op::GreaterThan, Op::GreaterThanOrEqual};
    static constexpr Style::Value styles[] = {Style::Stop, Style::Warning, Style::Information};
    element->SetType(EnumValue<Type>(Type(types[static_cast<Size>(d.Type)])));
    element->SetOperator(d.Operation ? EnumValue<Op>(Op(ops[static_cast<Size>(*d.Operation)])) : EnumValue<Op>());
    element->SetAllowBlank(BooleanValue(d.AllowBlank));
    element->SetShowDropDown(BooleanValue(!d.ShowDropDown));
    element->SetShowInputMessage(BooleanValue(d.ShowInputMessage));
    element->SetShowErrorMessage(BooleanValue(d.ShowErrorMessage));
    element->SetErrorStyle(EnumValue<Style>(Style(styles[static_cast<Size>(d.ErrorStyle)])));
    element->SetPromptTitle(d.PromptTitle ? StringValue(*d.PromptTitle) : StringValue());
    element->SetPrompt(d.Prompt ? StringValue(*d.Prompt) : StringValue());
    element->SetErrorTitle(d.ErrorTitle ? StringValue(*d.ErrorTitle) : StringValue());
    element->SetError(d.Error ? StringValue(*d.Error) : StringValue());
    std::vector<StringValue> refs;
    refs.reserve(d.Ranges.size());
    for (const auto& range : d.Ranges)
    {
        refs.emplace_back(range.ToA1());
    }
    element->SetSequenceOfReferences(ListValue<StringValue>(std::move(refs)));
    if (const auto old = element->GetFirstChildOfType<Spreadsheet::Formula1>())
    {
        element->RemoveChild(old);
    }
    if (const auto old = element->GetFirstChildOfType<Spreadsheet::Formula2>())
    {
        element->RemoveChild(old);
    }
    if (d.Formula1)
    {
        element->AppendChild<Spreadsheet::Formula1>()->SetText(*d.Formula1);
    }
    if (d.Formula2)
    {
        element->AppendChild<Spreadsheet::Formula2>()->SetText(*d.Formula2);
    }
    container->SetCount(UInt32Value(static_cast<UInt32>(owned.size())));
    return true;
}

bool Worksheet::RemoveDataValidation(const ExcelDataValidation::Ptr& validation)
{
    if (!validation)
    {
        return false;
    }
    const auto root = GetLowLevelApi();
    const auto container = root ? root->GetFirstChildOfType<Spreadsheet::DataValidations>() : nullptr;
    const auto element = validation->GetLowLevelApi();
    if (!container || !element || validation->m_owner.lock() != m_part)
    {
        return false;
    }
    const auto owned = container->Elements<Spreadsheet::DataValidation>();
    if (!container->RemoveChild(element))
    {
        return false;
    }
    const auto remaining = container->Elements<Spreadsheet::DataValidation>();
    if (remaining.empty())
    {
        return root->RemoveChild(container);
    }
    container->SetCount(UInt32Value(static_cast<UInt32>(remaining.size())));
    return true;
}

ExcelConditionalFormatting::Ptr Worksheet::CreateConditionalFormatting(
    const ExcelConditionalFormattingDefinition& definition)
{
    if (!IsValidExcelConditionalFormatting(definition))
    {
        return nullptr;
    }
    const auto root = GetLowLevelApi();
    if (!root)
    {
        return nullptr;
    }
    const auto before = root->GetFirstChildOfType<Spreadsheet::DataValidations>() ? std::static_pointer_cast<OpenXMLElement>(root->GetFirstChildOfType<Spreadsheet::DataValidations>()) : std::static_pointer_cast<OpenXMLElement>(root->GetFirstChildOfType<Spreadsheet::PageMargins>());
    const auto container = before ? root->InsertChild<Spreadsheet::ConditionalFormatting>(before) : root->AppendChild<Spreadsheet::ConditionalFormatting>();
    const auto rule = container ? container->AppendChild<Spreadsheet::ConditionalFormattingRule>() : nullptr;
    if (!rule)
    {
        if (container)
        {
            root->RemoveChild(container);
        }
        return nullptr;
    }
    auto result = ExcelConditionalFormatting::Ptr(new ExcelConditionalFormatting(container, rule, m_part));
    if (!UpdateConditionalFormatting(result, definition))
    {
        root->RemoveChild(container);
        return nullptr;
    }
    auto rules = ConditionalFormattings();
    Int32 nextPriority = 1;
    for (const auto& current : rules)
    {
        if (!rule->IsSameNode(current->m_rule))
        {
            nextPriority = std::max(nextPriority, current->m_rule->GetPriority().ValueOr(0) + 1);
        }
    }
    rule->SetPriority(Int32Value(nextPriority));
    return result;
}

std::vector<ExcelConditionalFormatting::Ptr> Worksheet::ConditionalFormattings() const
{
    std::vector<ExcelConditionalFormatting::Ptr> result;
    const auto root = GetLowLevelApi();
    if (!root)
    {
        return result;
    }
    for (const auto& container : root->Elements<Spreadsheet::ConditionalFormatting>())
    {
        for (const auto& rule : container->Elements<Spreadsheet::ConditionalFormattingRule>())
        {
            result.push_back(ExcelConditionalFormatting::Ptr(new ExcelConditionalFormatting(container, rule, m_part)));
        }
    }
    std::stable_sort(result.begin(), result.end(), [](const auto& a, const auto& b)
                     { return a->Priority() < b->Priority(); });
    return result;
}

bool Worksheet::UpdateConditionalFormatting(const ExcelConditionalFormatting::Ptr& formatting,
                                            const ExcelConditionalFormattingDefinition& d)
{
    if (!formatting || !IsValidExcelConditionalFormatting(d) || formatting->m_owner.lock() != m_part)
    {
        return false;
    }
    const auto root = GetLowLevelApi();
    const auto rule = formatting->m_rule;
    const auto container = formatting->m_container;
    if (!root || !rule || !container)
    {
        return false;
    }
    const auto containers = root->Elements<Spreadsheet::ConditionalFormatting>();
    if (std::none_of(containers.begin(), containers.end(),
                     [&](const auto& item)
                     { return container->IsSameNode(item); }))
    {
        return false;
    }
    const auto rules = container->Elements<Spreadsheet::ConditionalFormattingRule>();
    if (std::none_of(rules.begin(), rules.end(),
                     [&](const auto& item)
                     { return rule->IsSameNode(item); }))
    {
        return false;
    }
    using T = Spreadsheet::ConditionalFormatValues;
    using O = Spreadsheet::ConditionalFormattingOperatorValues;
    static constexpr T::Value types[] = {T::Expression, T::CellIs, T::UniqueValues, T::DuplicateValues, T::ContainsText, T::NotContainsText,
                                         T::BeginsWith, T::EndsWith, T::ContainsBlanks, T::NotContainsBlanks, T::ContainsErrors, T::NotContainsErrors, T::Top10, T::AboveAverage};
    static constexpr O::Value ops[] = {O::LessThan, O::LessThanOrEqual, O::Equal, O::NotEqual, O::GreaterThanOrEqual, O::GreaterThan, O::Between, O::NotBetween};
    rule->SetType(EnumValue<T>(T(types[static_cast<Size>(d.Type)])));
    rule->SetOperator(d.Operation ? EnumValue<O>(O(ops[static_cast<Size>(*d.Operation)])) : EnumValue<O>());
    rule->SetText(d.Text ? StringValue(*d.Text) : StringValue());
    rule->SetFormatId(d.DifferentialFormatId ? UInt32Value(*d.DifferentialFormatId) : UInt32Value());
    rule->SetStopIfTrue(BooleanValue(d.StopIfTrue));
    rule->SetRank(d.Type == ConditionalFormattingType::Top ? UInt32Value(d.Rank) : UInt32Value());
    rule->SetPercent(d.Type == ConditionalFormattingType::Top ? BooleanValue(d.Percent) : BooleanValue());
    rule->SetBottom(d.Type == ConditionalFormattingType::Top ? BooleanValue(d.IsBottom) : BooleanValue());
    rule->SetAboveAverage(d.Type == ConditionalFormattingType::AboveAverage ? BooleanValue(d.IsAboveAverage) : BooleanValue());
    rule->SetEqualAverage(d.Type == ConditionalFormattingType::AboveAverage ? BooleanValue(d.EqualAverage) : BooleanValue());
    rule->SetStdDev(d.StandardDeviation ? Int32Value(*d.StandardDeviation) : Int32Value());
    for (const auto& old : rule->Elements<Spreadsheet::Formula>())
    {
        rule->RemoveChild(old);
    }
    for (const auto& formula : d.Formulas)
    {
        rule->AppendChild<Spreadsheet::Formula>()->SetText(formula);
    }
    std::vector<StringValue> refs;
    for (const auto& range : d.Ranges)
    {
        refs.emplace_back(range.ToA1());
    }
    container->SetSequenceOfReferences(ListValue<StringValue>(std::move(refs)));
    return true;
}

bool Worksheet::MoveConditionalFormatting(const ExcelConditionalFormatting::Ptr& formatting, UInt32 priority)
{
    if (!formatting)
    {
        return false;
    }
    auto rules = ConditionalFormattings();
    const auto it = std::find_if(rules.begin(), rules.end(), [&](const auto& item)
                                 { return formatting->m_rule->IsSameNode(item->m_rule) && formatting->m_owner.lock() == m_part; });
    if (it == rules.end() || priority == 0 || priority > rules.size())
    {
        return false;
    }
    auto moved = *it;
    rules.erase(it);
    rules.insert(rules.begin() + static_cast<PtrDiff>(priority - 1), moved);
    for (Size i = 0; i < rules.size(); ++i)
    {
        rules[i]->m_rule->SetPriority(Int32Value(static_cast<Int32>(i + 1)));
    }
    return true;
}

bool Worksheet::RemoveConditionalFormatting(const ExcelConditionalFormatting::Ptr& formatting)
{
    if (!formatting || formatting->m_owner.lock() != m_part)
    {
        return false;
    }
    const auto root = GetLowLevelApi();
    if (!root)
    {
        return false;
    }
    const auto containers = root->Elements<Spreadsheet::ConditionalFormatting>();
    if (std::none_of(containers.begin(), containers.end(), [&](const auto& item)
                     { return formatting->m_container->IsSameNode(item); }))
    {
        return false;
    }
    if (!formatting->m_container->RemoveChild(formatting->m_rule))
    {
        return false;
    }
    if (formatting->m_container->Elements<Spreadsheet::ConditionalFormattingRule>().empty())
    {
        root->RemoveChild(formatting->m_container);
    }
    auto rules = ConditionalFormattings();
    for (Size i = 0; i < rules.size(); ++i)
    {
        rules[i]->m_rule->SetPriority(Int32Value(static_cast<Int32>(i + 1)));
    }
    return true;
}

std::shared_ptr<ExyokiOffice::Packaging::WorksheetPart> Worksheet::GetPart() const
{
    return m_part;
}

std::shared_ptr<Spreadsheet::Worksheet> Worksheet::GetLowLevelApi() const
{
    return m_part ? m_part->GetTypedRootElement() : nullptr;
}

SharedStringTableService::SharedStringTableService(ExcelDocument::Ptr document)
    : m_document(std::move(document))
{
}

bool SharedStringTableService::IsValid() const noexcept
{
    return m_document && m_document->GetWorkbookPart();
}

UInt32 SharedStringTableService::Count() const
{
    auto table = ExcelDocumentXmlHelper::SharedStringTableRoot(m_document, false);
    return static_cast<UInt32>(ExcelDocumentXmlHelper::SharedStringItems(table).size());
}

UInt32 SharedStringTableService::UniqueCount() const
{
    auto table = ExcelDocumentXmlHelper::SharedStringTableRoot(m_document, false);
    return table ? table->GetUniqueCount().ValueOr(Count()) : 0;
}

std::optional<std::string> SharedStringTableService::Lookup(UInt32 index) const
{
    auto items = ExcelDocumentXmlHelper::SharedStringItems(ExcelDocumentXmlHelper::SharedStringTableRoot(m_document, false));
    if (index >= items.size())
    {
        return std::nullopt;
    }
    return ExcelDocumentXmlHelper::SharedStringItemText(items[index]);
}

std::optional<UInt32> SharedStringTableService::Find(std::string_view text) const
{
    auto items = ExcelDocumentXmlHelper::SharedStringItems(ExcelDocumentXmlHelper::SharedStringTableRoot(m_document, false));
    for (UInt32 index = 0; index < items.size(); ++index)
    {
        if (ExcelDocumentXmlHelper::SharedStringItemText(items[index]) == text)
        {
            return index;
        }
    }
    return std::nullopt;
}

std::optional<UInt32> SharedStringTableService::GetOrAdd(std::string_view text)
{
    if (!IsValid())
    {
        return std::nullopt;
    }
    if (auto existing = Find(text))
    {
        return existing;
    }
    auto table = ExcelDocumentXmlHelper::SharedStringTableRoot(m_document, true);
    if (!ExcelDocumentXmlHelper::AppendPlainSharedString(table, text))
    {
        return std::nullopt;
    }
    ExcelDocumentXmlHelper::UpdateSharedStringCounts(table);
    return Count() - 1;
}

UInt32 SharedStringTableService::ReferenceCount(UInt32 index) const
{
    UInt32 count = 0;
    for (const auto& cell : ExcelDocumentXmlHelper::WorkbookCells(m_document))
    {
        auto referenced = ExcelDocumentXmlHelper::SharedStringIndexFromCell(cell);
        if (referenced && *referenced == index)
        {
            ++count;
        }
    }
    return count;
}

bool SharedStringTableService::Cleanup()
{
    auto workbookPart = m_document ? m_document->GetWorkbookPart() : nullptr;
    auto tablePart = workbookPart ? workbookPart->GetSharedStringTablePart() : nullptr;
    auto table = tablePart ? tablePart->GetTypedRootElement() : nullptr;
    if (!workbookPart || !table)
    {
        return IsValid();
    }

    std::unordered_map<std::string, UInt32> textToNew;
    std::vector<std::string> newTexts;
    const auto items = ExcelDocumentXmlHelper::SharedStringItems(table);
    for (const auto& cell : ExcelDocumentXmlHelper::WorkbookCells(m_document))
    {
        auto oldIndex = ExcelDocumentXmlHelper::SharedStringIndexFromCell(cell);
        if (!oldIndex || *oldIndex >= items.size())
        {
            continue;
        }
        const auto text = ExcelDocumentXmlHelper::SharedStringItemText(items[*oldIndex]);
        auto found = textToNew.find(text);
        if (found == textToNew.end())
        {
            const auto newIndex = static_cast<UInt32>(newTexts.size());
            found = textToNew.emplace(text, newIndex).first;
            newTexts.push_back(text);
        }
        ExcelDocumentXmlHelper::SetSharedStringIndexOnCell(cell, found->second);
    }

    if (newTexts.empty())
    {
        return workbookPart->RemoveSharedStringTablePart();
    }

    for (const auto& item : ExcelDocumentXmlHelper::SharedStringItems(table))
    {
        table->RemoveChild(item);
    }
    for (const auto& text : newTexts)
    {
        if (!ExcelDocumentXmlHelper::AppendPlainSharedString(table, text))
        {
            return false;
        }
    }
    ExcelDocumentXmlHelper::UpdateSharedStringCounts(table);
    return true;
}

ExcelDocumentEditor::ExcelDocumentEditor(const ExcelDocument::Ptr& document)
    : m_document(document)
{
}

ExcelDocumentEditor::~ExcelDocumentEditor()
{
    if (m_transactionOwner)
    {
        m_transactionOwner->Invalidate(this);
    }
}

ExcelDocumentEditor::Ptr ExcelDocumentEditor::Create(const ExcelDocument::Ptr& document)
{
    return std::make_shared<ExcelDocumentEditor>(document);
}

ExcelDocumentEditor::Ptr ExcelDocumentEditor::CreateNew(SpreadsheetDocumentType type)
{
    auto editor = std::make_shared<ExcelDocumentEditor>();
    if (!editor || !editor->CreateDefaultDocument(type))
    {
        return nullptr;
    }
    return editor;
}

ExcelDocumentEditor::Ptr ExcelDocumentEditor::Open(const std::filesystem::path& path,
                                                   const ExyokiOffice::Packaging::OpenSettings& settings,
                                                   const ICancellationToken* cancellationToken)
{
    auto document = ExcelDocument::Open(path, settings, cancellationToken);
    if (!document)
    {
        return nullptr;
    }
    return std::make_shared<ExcelDocumentEditor>(document);
}

ExcelDocumentEditor::Ptr ExcelDocumentEditor::Open(const std::vector<Byte>& packageBuffer,
                                                   const ExyokiOffice::Packaging::OpenSettings& settings,
                                                   const ICancellationToken* cancellationToken)
{
    auto document = ExcelDocument::Open(packageBuffer, settings, cancellationToken);
    if (!document)
    {
        return nullptr;
    }
    return std::make_shared<ExcelDocumentEditor>(document);
}

ExcelDocumentEditor::Ptr ExcelDocumentEditor::Open(std::span<const Byte> packageBuffer,
                                                   const ExyokiOffice::Packaging::OpenSettings& settings,
                                                   const ICancellationToken* cancellationToken)
{
    auto document = ExcelDocument::Open(packageBuffer, settings, cancellationToken);
    if (!document)
    {
        return nullptr;
    }
    return std::make_shared<ExcelDocumentEditor>(document);
}

bool ExcelDocumentEditor::SaveToFile(const std::filesystem::path& path,
                                     bool atomicSave,
                                     const ICancellationToken* cancellationToken)
{
    return m_document && m_document->SaveToFile(path, atomicSave, cancellationToken);
}

std::vector<Byte> ExcelDocumentEditor::SaveToMemory(const ICancellationToken* cancellationToken)
{
    return m_document ? m_document->SaveToMemory(cancellationToken) : std::vector<Byte>{};
}

std::optional<DocumentEditMemento> ExcelDocumentEditor::CreateMemento(
    const ICancellationToken* cancellationToken)
{
    if (!m_document)
    {
        return std::nullopt;
    }

    auto bytes = m_document->SaveToMemory(cancellationToken);
    if (bytes.empty())
    {
        return std::nullopt;
    }

    return DocumentEditMemento(DocumentFamily::Excel, std::move(bytes));
}

bool ExcelDocumentEditor::RestoreMemento(const DocumentEditMemento& memento,
                                         const ICancellationToken* cancellationToken)
{
    if (memento.Family() != DocumentFamily::Excel || memento.Bytes().empty())
    {
        return false;
    }

    auto document = ExcelDocument::Open(memento.Bytes(), {}, cancellationToken);
    if (!document)
    {
        return false;
    }

    m_document = std::move(document);
    return true;
}

DocumentEditTransaction ExcelDocumentEditor::BeginTransaction(
    const ICancellationToken* cancellationToken)
{
    if (!m_transactionOwner || !m_transactionOwner->IsAlive(this))
    {
        m_transactionOwner = std::make_shared<detail::DocumentEditTransactionOwner>(this);
    }

    if (!m_transactionOwner->TryBeginTransaction(this))
    {
        return {};
    }

    try
    {
        auto memento = CreateMemento(cancellationToken);
        if (!memento)
        {
            m_transactionOwner->EndTransaction(this);
            return {};
        }

        return DocumentEditTransaction(
            std::move(*memento),
            [this](const DocumentEditMemento& value)
            { return RestoreMemento(value); },
            m_transactionOwner,
            this);
    }
    catch (...)
    {
        m_transactionOwner->EndTransaction(this);
        throw;
    }
}

void ExcelDocumentEditor::SetDocument(const ExcelDocument::Ptr& document)
{
    m_document = document;
}

ExcelDocument::Ptr ExcelDocumentEditor::GetDocument() const
{
    return m_document;
}

ExcelDocument::Ptr ExcelDocumentEditor::GetLowLevelApi() const
{
    return m_document;
}

Packaging::DocumentProperties ExcelDocumentEditor::Properties() const
{
    return Packaging::DocumentProperties(*m_document);
}

std::vector<Security::ExternalReference> ExcelDocumentEditor::ExternalWorkbookLinks() const
{
    if (!m_document)
    {
        return {};
    }

    std::vector<Security::ExternalReference> links;
    for (auto& reference : Security::CollectExternalReferences(*m_document))
    {
        if (reference.Kind == Security::ExternalResourceKind::ExternalWorkbook)
        {
            links.push_back(std::move(reference));
        }
    }
    return links;
}

Security::ExternalResourceResponse ExcelDocumentEditor::ResolveExternalWorkbook(
    const Security::ExternalReference& reference,
    const ICancellationToken* cancellationToken)
{
    if (!m_document)
    {
        Security::ExternalResourceResponse response;
        response.Status = Security::ExternalResourceStatus::Failed;
        response.Message = "The editor has no attached document.";
        return response;
    }

    return Security::ResolveExternalResource(*m_document, reference, cancellationToken);
}

std::optional<ExyokiOffice::ThemeSettings> ExcelDocumentEditor::ThemeSettings() const
{
    auto workbookPart = m_document ? m_document->GetWorkbookPart() : nullptr;
    return ThemeService::ReadSettings(workbookPart ? workbookPart->GetThemePart() : nullptr);
}

bool ExcelDocumentEditor::SetThemeSettings(const ExyokiOffice::ThemeSettings& settings)
{
    auto workbookPart = m_document ? m_document->GetWorkbookPart() : nullptr;
    return ThemeService::WriteSettings(workbookPart ? workbookPart->GetThemePart() : nullptr, settings);
}

std::optional<std::string> ExcelDocumentEditor::ThemeXml() const
{
    auto workbookPart = m_document ? m_document->GetWorkbookPart() : nullptr;
    return ThemeService::ReadXml(workbookPart ? workbookPart->GetThemePart() : nullptr);
}

bool ExcelDocumentEditor::SetThemeXml(std::string xml)
{
    auto workbookPart = m_document ? m_document->GetWorkbookPart() : nullptr;
    if (!workbookPart || !ThemeService::IsValidThemeXml(xml))
    {
        return false;
    }
    auto theme = workbookPart->GetThemePart();
    if (!theme)
    {
        theme = workbookPart->AddThemePart();
    }
    return ThemeService::WriteXml(theme, std::move(xml));
}

bool ExcelDocumentEditor::EnsureTheme()
{
    auto workbookPart = m_document ? m_document->GetWorkbookPart() : nullptr;
    if (!workbookPart)
    {
        return false;
    }
    if (workbookPart->GetThemePart())
    {
        return true;
    }
    auto theme = workbookPart->AddThemePart();
    if (!ThemeService::WriteDefaultTheme(theme))
    {
        workbookPart->RemoveThemePart();
        return false;
    }
    return true;
}

bool ExcelDocumentEditor::RemoveTheme()
{
    auto workbookPart = m_document ? m_document->GetWorkbookPart() : nullptr;
    return workbookPart && workbookPart->RemoveThemePart();
}

bool ExcelDocumentEditor::HasVbaProject() const noexcept
{
    return m_document && m_document->HasVbaProject();
}

std::vector<Byte> ExcelDocumentEditor::GetVbaProjectData() const
{
    return m_document ? m_document->GetVbaProjectData() : std::vector<Byte>{};
}

bool ExcelDocumentEditor::SetVbaProjectData(std::span<const Byte> data)
{
    return m_document && m_document->SetVbaProjectData(data);
}

bool ExcelDocumentEditor::RemoveVbaProject()
{
    return m_document && m_document->RemoveVbaProject();
}

Worksheet::Ptr ExcelDocumentEditor::AddWorksheet(std::string_view name)
{
    if (!m_document && !CreateDefaultDocument(SpreadsheetDocumentType::Workbook))
    {
        return nullptr;
    }
    auto workbookPart = m_document->GetWorkbookPart();
    auto sheets = ExcelDocumentXmlHelper::EnsureWorkbookSheets(m_document);
    if (!workbookPart || !sheets)
    {
        return nullptr;
    }

    auto worksheetPart = workbookPart->AddWorksheetPart();
    if (!worksheetPart)
    {
        return nullptr;
    }
    auto worksheet = worksheetPart->GetTypedRootElement();
    if (!worksheet)
    {
        return nullptr;
    }
    ExcelDocumentXmlHelper::EnsureSheetData(worksheet);

    const auto requestedName = name.empty() ? std::string("Sheet") + std::to_string(ExcelDocumentXmlHelper::NextSheetId(sheets)) : std::string(name);
    if (!ExcelDocumentXmlHelper::IsValidWorksheetName(requestedName) || ExcelDocumentXmlHelper::WorksheetNameExists(m_document, requestedName))
    {
        workbookPart->RemoveWorksheetPart(worksheetPart);
        return nullptr;
    }
    const auto sheetName = requestedName;
    auto sheet = sheets->AppendChild<Spreadsheet::Sheet>();
    if (!sheet)
    {
        workbookPart->RemoveWorksheetPart(worksheetPart);
        return nullptr;
    }
    sheet->SetName(StringValue(sheetName));
    sheet->SetSheetId(UInt32Value(ExcelDocumentXmlHelper::NextSheetId(sheets)));
    sheet->SetId(StringValue(worksheetPart->RelationshipId()));

    return std::make_shared<Worksheet>(sheetName, worksheetPart, m_document);
}

Worksheet::Ptr ExcelDocumentEditor::GetWorksheet(Size index) const
{
    return ExcelDocumentXmlHelper::WrapWorksheet(m_document, ExcelDocumentXmlHelper::GetSheetElement(m_document, index));
}

Worksheet::Ptr ExcelDocumentEditor::GetWorksheet(std::string_view name) const
{
    const auto desired = AsciiText::ToLower(name);
    for (const auto& sheet : ExcelDocumentXmlHelper::SheetElements(m_document))
    {
        if (AsciiText::ToLower(sheet->GetName().ToString()) == desired)
        {
            return ExcelDocumentXmlHelper::WrapWorksheet(m_document, sheet);
        }
    }
    return nullptr;
}

bool ExcelDocumentEditor::RenameWorksheet(Size index, std::string_view newName)
{
    auto sheet = ExcelDocumentXmlHelper::GetSheetElement(m_document, index);
    if (!sheet || !ExcelDocumentXmlHelper::IsValidWorksheetName(newName) || ExcelDocumentXmlHelper::WorksheetNameExists(m_document, newName, sheet))
    {
        return false;
    }
    sheet->SetName(StringValue(std::string(newName)));
    return true;
}

bool ExcelDocumentEditor::MoveWorksheet(Size fromIndex, Size toIndex)
{
    auto sheets = ExcelDocumentXmlHelper::EnsureWorkbookSheets(m_document);
    auto currentSheets = ExcelDocumentXmlHelper::SheetElements(m_document);
    if (!sheets || fromIndex >= currentSheets.size() || toIndex >= currentSheets.size())
    {
        return false;
    }
    if (fromIndex == toIndex)
    {
        return true;
    }

    auto source = currentSheets[fromIndex];
    auto before = currentSheets[toIndex];
    if (fromIndex < toIndex)
    {
        before = (toIndex + 1 < currentSheets.size()) ? currentSheets[toIndex + 1] : nullptr;
    }

    auto moved = sheets->InsertChild<Spreadsheet::Sheet>(before);
    if (!moved)
    {
        return false;
    }
    moved->SetName(source->GetName());
    moved->SetSheetId(source->GetSheetId());
    moved->SetId(source->GetId());
    return sheets->RemoveChild(source);
}

Worksheet::Ptr ExcelDocumentEditor::CopyWorksheet(Size sourceIndex, std::string_view name)
{
    if (!m_document)
    {
        return nullptr;
    }
    auto sourceSheet = ExcelDocumentXmlHelper::GetSheetElement(m_document, sourceIndex);
    auto workbookPart = m_document->GetWorkbookPart();
    auto sheets = ExcelDocumentXmlHelper::EnsureWorkbookSheets(m_document);
    auto sourcePart = sourceSheet ? ExcelDocumentXmlHelper::FindWorksheetPartByRelationshipId(m_document, sourceSheet->GetId().ToString()) : nullptr;
    if (!sourceSheet || !workbookPart || !sheets || !sourcePart)
    {
        return nullptr;
    }

    const auto sheetName = name.empty() ? ExcelDocumentXmlHelper::MakeUniqueWorksheetName(m_document, sourceSheet->GetName().ToString())
                                        : std::string(name);
    if (!ExcelDocumentXmlHelper::IsValidWorksheetName(sheetName) || ExcelDocumentXmlHelper::WorksheetNameExists(m_document, sheetName))
    {
        return nullptr;
    }

    auto worksheetPart = workbookPart->AddWorksheetPart();
    if (!worksheetPart)
    {
        return nullptr;
    }
    worksheetPart->SetXmlString(sourcePart->GetXmlString());

    auto sheet = sheets->AppendChild<Spreadsheet::Sheet>();
    if (!sheet)
    {
        workbookPart->RemoveWorksheetPart(worksheetPart);
        return nullptr;
    }
    sheet->SetName(StringValue(sheetName));
    sheet->SetSheetId(UInt32Value(ExcelDocumentXmlHelper::NextSheetId(sheets)));
    sheet->SetId(StringValue(worksheetPart->RelationshipId()));
    return std::make_shared<Worksheet>(sheetName, worksheetPart, m_document);
}

Worksheet::Ptr ExcelDocumentEditor::CopyWorksheetFrom(const ExcelDocumentEditor& sourceEditor,
                                                      Size sourceIndex,
                                                      std::string_view name)
{
    if (!m_document || !sourceEditor.m_document || m_document == sourceEditor.m_document)
    {
        return nullptr;
    }
    auto sourceSheet = ExcelDocumentXmlHelper::GetSheetElement(sourceEditor.m_document, sourceIndex);
    auto sourcePart = sourceSheet
                          ? ExcelDocumentXmlHelper::FindWorksheetPartByRelationshipId(sourceEditor.m_document, sourceSheet->GetId().ToString())
                          : nullptr;
    auto workbookPart = m_document->GetWorkbookPart();
    auto sheets = ExcelDocumentXmlHelper::EnsureWorkbookSheets(m_document);
    if (!sourceSheet || !sourcePart || !workbookPart || !sheets)
    {
        return nullptr;
    }

    const auto sheetName = name.empty()
                               ? (ExcelDocumentXmlHelper::WorksheetNameExists(m_document, sourceSheet->GetName().ToString())
                                      ? ExcelDocumentXmlHelper::MakeUniqueWorksheetName(m_document, sourceSheet->GetName().ToString())
                                      : sourceSheet->GetName().ToString())
                               : std::string(name);
    if (!ExcelDocumentXmlHelper::IsValidWorksheetName(sheetName) || ExcelDocumentXmlHelper::WorksheetNameExists(m_document, sheetName))
    {
        return nullptr;
    }

    auto sourceRoot = sourcePart->GetTypedRootElement();
    bool usesStyles = false;
    if (sourceRoot)
    {
        for (const auto& cell : sourceRoot->Descendants<Spreadsheet::Cell>())
        {
            if (cell && cell->GetStyleIndex().IsDefined() && cell->GetStyleIndex().ValueOr(0) != 0)
            {
                usesStyles = true;
                break;
            }
        }
    }
    if (usesStyles)
    {
        auto sourceStyles = sourceEditor.m_document->GetWorkbookPart()->GetWorkbookStylesPart();
        auto targetStyles = workbookPart->GetWorkbookStylesPart();
        if (!sourceStyles || !targetStyles || sourceStyles->GetXmlString() != targetStyles->GetXmlString())
        {
            return nullptr;
        }
    }

    auto importedBase = workbookPart->ImportPartGraph(sourcePart);
    auto importedPart = std::dynamic_pointer_cast<Packaging::WorksheetPart>(importedBase);
    if (!importedPart)
    {
        return nullptr;
    }

    auto importedRoot = importedPart->GetTypedRootElement();
    auto sourceStrings = sourceEditor.SharedStrings();
    auto targetStrings = SharedStrings();
    for (const auto& cell : importedRoot ? importedRoot->Descendants<Spreadsheet::Cell>()
                                         : std::vector<Spreadsheet::Cell::Ptr>{})
    {
        auto oldIndex = ExcelDocumentXmlHelper::SharedStringIndexFromCell(cell);
        if (!oldIndex)
        {
            continue;
        }
        auto text = sourceStrings.Lookup(*oldIndex);
        auto newIndex = text ? targetStrings.GetOrAdd(*text) : std::nullopt;
        if (!newIndex || !ExcelDocumentXmlHelper::SetSharedStringIndexOnCell(cell, *newIndex))
        {
            workbookPart->RemoveWorksheetPart(importedPart);
            return nullptr;
        }
    }

    auto sheet = sheets->AppendChild<Spreadsheet::Sheet>();
    if (!sheet)
    {
        workbookPart->RemoveWorksheetPart(importedPart);
        return nullptr;
    }
    sheet->SetName(StringValue(sheetName));
    sheet->SetSheetId(UInt32Value(ExcelDocumentXmlHelper::NextSheetId(sheets)));
    sheet->SetId(StringValue(importedPart->RelationshipId()));
    return std::make_shared<Worksheet>(sheetName, importedPart, m_document);
}

bool ExcelDocumentEditor::RemoveWorksheet(Size index)
{
    auto workbookPart = m_document ? m_document->GetWorkbookPart() : nullptr;
    auto sheets = ExcelDocumentXmlHelper::EnsureWorkbookSheets(m_document);
    auto currentSheets = ExcelDocumentXmlHelper::SheetElements(m_document);
    if (!workbookPart || !sheets || index >= currentSheets.size() || currentSheets.size() <= 1)
    {
        return false;
    }

    auto sheet = currentSheets[index];
    auto part = ExcelDocumentXmlHelper::FindWorksheetPartByRelationshipId(m_document, sheet->GetId().ToString());
    if (!part || !sheets->RemoveChild(sheet))
    {
        return false;
    }
    return workbookPart->RemoveWorksheetPart(part);
}

std::vector<Worksheet::Ptr> ExcelDocumentEditor::Worksheets() const
{
    std::vector<Worksheet::Ptr> result;
    for (const auto& sheet : ExcelDocumentXmlHelper::SheetElements(m_document))
    {
        auto worksheet = ExcelDocumentXmlHelper::WrapWorksheet(m_document, sheet);
        if (worksheet)
        {
            result.push_back(worksheet);
        }
    }
    return result;
}

Worksheet::Ptr ExcelDocumentEditor::FirstWorksheet() const
{
    auto worksheets = Worksheets();
    return worksheets.empty() ? nullptr : worksheets.front();
}

SharedStringTableService ExcelDocumentEditor::SharedStrings() const
{
    return SharedStringTableService(m_document);
}

StyleRepository ExcelDocumentEditor::Styles() const
{
    return StyleRepository(m_document);
}

bool ExcelDocumentEditor::CreateDefaultDocument(SpreadsheetDocumentType type)
{
    auto document = ExcelDocument::Create(type);
    if (!document || !document->InitDocument())
    {
        return false;
    }
    m_document = document;
    return AddWorksheet("Sheet1") != nullptr;
}

} // namespace ExyokiOffice::Excel
