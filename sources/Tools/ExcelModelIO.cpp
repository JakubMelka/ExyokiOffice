// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "ExyokiOffice/Tools/DocumentModelIO.hpp"

#include "ExyokiOffice/Excel/ExcelDocument.hpp"
#include "ExyokiOffice/Tools/PackageInspector.hpp"
#include "ExyokiOffice/Tools/PackageLimits.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <algorithm>
#include <cstdlib>
#include <map>

namespace ExyokiOffice::Tools
{

/// File-local helpers for the workbook document model round trip.
class ExcelModelIoHelper
{
public:
    static void Warn(std::vector<ToolDiagnostic>& diagnostics, std::string message, std::string context = {})
    {
        diagnostics.push_back(ToolDiagnostic{ToolSeverity::Warning, std::move(message), std::move(context)});
    }

    static std::string_view CachedKindToken(Excel::FormulaCachedValueKind kind)
    {
        switch (kind)
        {
            case Excel::FormulaCachedValueKind::SharedString:
            case Excel::FormulaCachedValueKind::String:
                return "string";
            case Excel::FormulaCachedValueKind::Number:
                return "number";
            case Excel::FormulaCachedValueKind::Boolean:
                return "bool";
            case Excel::FormulaCachedValueKind::Error:
                return "error";
            case Excel::FormulaCachedValueKind::DateTime:
                return "datetime";
            case Excel::FormulaCachedValueKind::None:
                break;
        }
        return "";
    }

    static Excel::FormulaCachedValueKind CachedKindFromToken(const std::string& token)
    {
        if (token == "string")
        {
            return Excel::FormulaCachedValueKind::String;
        }
        if (token == "number")
        {
            return Excel::FormulaCachedValueKind::Number;
        }
        if (token == "bool")
        {
            return Excel::FormulaCachedValueKind::Boolean;
        }
        if (token == "error")
        {
            return Excel::FormulaCachedValueKind::Error;
        }
        if (token == "datetime")
        {
            return Excel::FormulaCachedValueKind::DateTime;
        }
        return Excel::FormulaCachedValueKind::None;
    }

    static bool ParseNumber(const std::string& text, Real& value)
    {
        if (text.empty())
        {
            return false;
        }
        char* end = nullptr;
        value = std::strtod(text.c_str(), &end);
        return end != nullptr && *end == '\0';
    }

    static void ReadSheet(const Excel::Worksheet::Ptr& worksheet, const Excel::SharedStringTableService& sharedStrings,
                          ExcelSheetModel& sheet, std::vector<ToolDiagnostic>& diagnostics)
    {
        sheet.Name = worksheet->Name();

        auto addresses = worksheet->StoredCellAddresses();
        std::sort(addresses.begin(), addresses.end(),
                  [](const Excel::CellAddress& left, const Excel::CellAddress& right)
                  {
                      if (left.Row().Value() != right.Row().Value())
                      {
                          return left.Row().Value() < right.Row().Value();
                      }
                      return left.Column().Value() < right.Column().Value();
                  });

        for (const auto& address : addresses)
        {
            const auto value = worksheet->GetCellValue(address);
            if (!value || value->IsBlank())
            {
                continue;
            }

            ExcelCellModel cell;
            cell.Address = address.ToA1();
            switch (value->Kind())
            {
                case Excel::CellValueKind::SharedString:
                {
                    cell.Type = "string";
                    if (const auto index = value->SharedStringIndex())
                    {
                        cell.Value = sharedStrings.Lookup(*index).value_or(std::string());
                    }
                    break;
                }
                case Excel::CellValueKind::InlineString:
                    cell.Type = "string";
                    cell.Value = value->Text();
                    break;
                case Excel::CellValueKind::Number:
                    cell.Type = "number";
                    cell.Value = value->Text();
                    break;
                case Excel::CellValueKind::Boolean:
                    cell.Type = "bool";
                    cell.Value = value->BooleanValue().value_or(false) ? "true" : "false";
                    break;
                case Excel::CellValueKind::Error:
                    cell.Type = "error";
                    cell.Value = value->Text();
                    break;
                case Excel::CellValueKind::DateTime:
                    cell.Type = "datetime";
                    cell.Value = value->Text();
                    break;
                case Excel::CellValueKind::Formula:
                {
                    cell.Type = "formula";
                    const auto formula = worksheet->GetCellFormula(address);
                    if (formula)
                    {
                        cell.Formula = formula->Formula;
                        cell.CachedType = std::string(CachedKindToken(formula->CachedKind));
                        cell.CachedValue = formula->CachedText;
                        if (formula->Kind != Excel::CellFormulaKind::Normal)
                        {
                            Warn(diagnostics, "Shared/array formula exported as a normal formula",
                                 sheet.Name + "!" + cell.Address);
                        }
                    }
                    if (cell.Formula.empty())
                    {
                        Warn(diagnostics, "Formula text unavailable; cached value kept",
                             sheet.Name + "!" + cell.Address);
                        cell.Type = "string";
                        cell.Value = value->Text();
                        cell.CachedType.clear();
                        cell.CachedValue.clear();
                    }
                    break;
                }
                case Excel::CellValueKind::Blank:
                    continue;
            }
            sheet.Cells.push_back(std::move(cell));
        }

        for (const auto& range : worksheet->MergedRanges())
        {
            sheet.Merges.push_back(range.ToA1());
        }
        for (const auto& table : worksheet->Tables())
        {
            if (!table)
            {
                continue;
            }
            ExcelTableModel modelTable;
            modelTable.Name = table->Name();
            if (const auto range = table->Range())
            {
                modelTable.Range = range->ToA1();
            }
            sheet.Tables.push_back(std::move(modelTable));
        }
        for (const auto& hyperlink : worksheet->Hyperlinks())
        {
            ExcelHyperlinkModel modelLink;
            modelLink.Cell = hyperlink.Address.ToA1();
            modelLink.Target = !hyperlink.Target.empty() ? hyperlink.Target : hyperlink.Location;
            modelLink.Tooltip = hyperlink.Tooltip;
            sheet.Hyperlinks.push_back(std::move(modelLink));
        }
    }

    static void WriteSheet(const Excel::Worksheet::Ptr& worksheet, const ExcelSheetModel& sheet,
                           std::vector<ToolDiagnostic>& diagnostics)
    {
        // Cell text by (row, column), used to derive table column names.
        std::map<std::pair<UInt32, UInt32>, std::string> textByPosition;

        for (const auto& cell : sheet.Cells)
        {
            const auto address = Excel::CellAddress::ParseA1(cell.Address);
            if (!address)
            {
                Warn(diagnostics, "Invalid cell address skipped", sheet.Name + "!" + cell.Address);
                continue;
            }

            const auto displayText = cell.Type == "formula" ? cell.CachedValue : cell.Value;
            textByPosition[{address->Row().Value(), address->Column().Value()}] = displayText;

            if (cell.Type == "number")
            {
                Real number = 0.0;
                if (ParseNumber(cell.Value, number))
                {
                    worksheet->SetCellNumber(*address, number);
                }
                else
                {
                    Warn(diagnostics, "Invalid number stored as text", sheet.Name + "!" + cell.Address);
                    worksheet->SetCellText(*address, cell.Value);
                }
            }
            else if (cell.Type == "bool")
            {
                worksheet->SetCellBoolean(*address, cell.Value == "true" || cell.Value == "1");
            }
            else if (cell.Type == "error")
            {
                worksheet->SetCellError(*address, cell.Value);
            }
            else if (cell.Type == "datetime")
            {
                worksheet->SetCellDateTimeText(*address, cell.Value);
            }
            else if (cell.Type == "formula")
            {
                Excel::CellFormulaValue formula;
                formula.Formula = cell.Formula;
                formula.CachedKind = CachedKindFromToken(cell.CachedType);
                formula.CachedText = cell.CachedValue;
                if (!worksheet->SetCellFormula(*address, formula))
                {
                    Warn(diagnostics, "Formula could not be written", sheet.Name + "!" + cell.Address);
                    worksheet->SetCellText(*address, "=" + cell.Formula);
                }
            }
            else
            {
                worksheet->SetCellText(*address, cell.Value);
            }
        }

        for (const auto& merge : sheet.Merges)
        {
            const auto range = Excel::CellRange::ParseA1(merge);
            if (!range)
            {
                Warn(diagnostics, "Invalid merge range skipped", sheet.Name + "!" + merge);
                continue;
            }
            worksheet->MergeRange(*range);
        }

        for (const auto& table : sheet.Tables)
        {
            const auto range = Excel::CellRange::ParseA1(table.Range);
            if (!range || table.Name.empty())
            {
                Warn(diagnostics, "Invalid table definition skipped", sheet.Name + "!" + table.Range);
                continue;
            }
            std::vector<Excel::ExcelTableColumn> columns;
            const auto headerRow = range->First().Row().Value();
            for (UInt32 column = range->First().Column().Value();
                 column <= range->Last().Column().Value(); ++column)
            {
                Excel::ExcelTableColumn tableColumn;
                tableColumn.Id = column - range->First().Column().Value() + 1;
                const auto text = textByPosition.find({headerRow, column});
                tableColumn.Name = text != textByPosition.end() && !text->second.empty()
                                       ? text->second
                                       : "Column" + std::to_string(tableColumn.Id);
                columns.push_back(std::move(tableColumn));
            }
            if (!worksheet->CreateTable(table.Name, *range, columns))
            {
                Warn(diagnostics, "Table could not be created", sheet.Name + "!" + table.Name);
            }
        }

        for (const auto& hyperlink : sheet.Hyperlinks)
        {
            const auto address = Excel::CellAddress::ParseA1(hyperlink.Cell);
            if (!address)
            {
                Warn(diagnostics, "Invalid hyperlink cell skipped", sheet.Name + "!" + hyperlink.Cell);
                continue;
            }
            Excel::ExcelHyperlink link;
            link.Address = *address;
            // Workbook-internal locations look like "Sheet2!A1"; anything else is external.
            if (hyperlink.Target.find("://") == std::string::npos &&
                hyperlink.Target.find('!') != std::string::npos)
            {
                link.Location = hyperlink.Target;
            }
            else
            {
                link.Target = hyperlink.Target;
            }
            link.Tooltip = hyperlink.Tooltip;
            worksheet->SetHyperlink(link);
        }
    }
};

DocumentModel ReadExcelModel(const std::filesystem::path& path, const ModelReadOptions& options,
                             std::vector<ToolDiagnostic>& diagnostics)
{
    auto opened = Excel::ExcelDocumentEditor::Open(path, UntrustedOpenSettings());
    if (!opened)
    {
        diagnostics.push_back(
            ToolDiagnostic{ToolSeverity::Error, "Failed to open Excel document", path.string()});
        return {};
    }

    return ReadExcelModel(*opened, options, diagnostics);
}

DocumentModel ReadExcelModel(Excel::ExcelDocumentEditor& editor, const ModelReadOptions& options,
                             std::vector<ToolDiagnostic>& diagnostics)
{
    (void)options;
    DocumentModel model;
    model.Family = DocumentFamily::Excel;
    if (auto document = editor.GetDocument())
    {
        model.Properties = ReadCoreProperties(*document);
    }

    auto& workbook = model.Excel.emplace();
    const auto sharedStrings = editor.SharedStrings();
    for (const auto& worksheet : editor.Worksheets())
    {
        if (!worksheet)
        {
            continue;
        }
        ExcelSheetModel sheet;
        ExcelModelIoHelper::ReadSheet(worksheet, sharedStrings, sheet, diagnostics);
        workbook.Sheets.push_back(std::move(sheet));
    }
    return model;
}

bool WriteExcelModel(const DocumentModel& model, const std::filesystem::path& path,
                     std::vector<ToolDiagnostic>& diagnostics)
{
    if (!model.Excel)
    {
        diagnostics.push_back(ToolDiagnostic{ToolSeverity::Error, "Model carries no Excel workbook"});
        return false;
    }

    auto editor = Excel::ExcelDocumentEditor::CreateNew();
    if (!editor)
    {
        diagnostics.push_back(ToolDiagnostic{ToolSeverity::Error, "Cannot create Excel document"});
        return false;
    }

    bool first = true;
    for (const auto& sheet : model.Excel->Sheets)
    {
        Excel::Worksheet::Ptr worksheet;
        const auto sheetName = sheet.Name.empty() ? "Sheet" : sheet.Name;
        if (first && editor->FirstWorksheet())
        {
            worksheet = editor->FirstWorksheet();
            editor->RenameWorksheet(0, sheetName);
        }
        else
        {
            worksheet = editor->AddWorksheet(sheetName);
        }
        first = false;
        if (!worksheet)
        {
            ExcelModelIoHelper::Warn(diagnostics, "Worksheet could not be created", sheetName);
            continue;
        }
        ExcelModelIoHelper::WriteSheet(worksheet, sheet, diagnostics);
    }

    if (auto document = editor->GetDocument())
    {
        const auto writeProperty = [&](std::string_view name, const std::string& value)
        {
            if (!value.empty())
            {
                WriteCoreProperty(*document, name, value);
            }
        };
        writeProperty("Title", model.Properties.Title);
        writeProperty("Subject", model.Properties.Subject);
        writeProperty("Creator", model.Properties.Creator);
        writeProperty("Keywords", model.Properties.Keywords);
        writeProperty("Description", model.Properties.Description);
        writeProperty("Category", model.Properties.Category);
    }

    if (!editor->SaveToFile(path))
    {
        diagnostics.push_back(
            ToolDiagnostic{ToolSeverity::Error, "Failed to save Excel document", path.string()});
        return false;
    }
    return true;
}

} // namespace ExyokiOffice::Tools
