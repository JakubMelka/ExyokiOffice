// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "doctest.h"

#include "TestSupport.hpp"

#include "ExyokiOffice/Excel/ExcelDocument.hpp"
#include "ExyokiOffice/Tools/DocumentConverter.hpp"
#include "ExyokiOffice/Tools/DocumentModelIO.hpp"

#include <chrono>
#include <fstream>

using namespace ExyokiOffice::Tools;
using ExyokiOffice::Excel::ExcelDocumentEditor;

namespace
{

using ExyokiOfficeTests::MakeTemporaryPath;

std::filesystem::path SaveSampleWorkbook()
{
    auto editor = ExcelDocumentEditor::CreateNew();
    REQUIRE(editor);
    auto sheet = editor->FirstWorksheet();
    REQUIRE(sheet);
    REQUIRE(editor->RenameWorksheet(0, "Data"));

    REQUIRE(sheet->SetCellText(1, 1, "Name"));
    REQUIRE(sheet->SetCellText(1, 2, "Value"));
    REQUIRE(sheet->SetCellText(2, 1, "Answer"));
    REQUIRE(sheet->SetCellNumber(2, 2, 42.5));

    const auto boolAddress = ExyokiOffice::Excel::CellAddress::ParseA1("C2");
    REQUIRE(boolAddress);
    REQUIRE(sheet->SetCellBoolean(*boolAddress, true));

    const auto formulaAddress = ExyokiOffice::Excel::CellAddress::ParseA1("D2");
    REQUIRE(formulaAddress);
    REQUIRE(sheet->SetCellFormula(*formulaAddress, "SUM(B2:B2)"));

    const auto mergeRange = ExyokiOffice::Excel::CellRange::ParseA1("A4:B4");
    REQUIRE(mergeRange);
    sheet->MergeRange(*mergeRange);

    auto second = editor->AddWorksheet("Second");
    REQUIRE(second);
    REQUIRE(second->SetCellText(1, 1, "Other sheet"));

    const auto path = MakeTemporaryPath("exyoki_convert_excel", ".xlsx");
    REQUIRE(editor->SaveToFile(path));
    return path;
}

std::string ReadFileText(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    REQUIRE(file);
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

const ExcelCellModel* FindCell(const ExcelSheetModel& sheet, std::string_view address)
{
    for (const auto& cell : sheet.Cells)
    {
        if (cell.Address == address)
        {
            return &cell;
        }
    }
    return nullptr;
}

} // namespace

TEST_CASE("Excel workbook converts to JSON and back with values and formulas [unit] [tools] [conversion]")
{
    const auto xlsxPath = SaveSampleWorkbook();
    const auto jsonPath = MakeTemporaryPath("exyoki_convert_excel", ".json");
    const auto backPath = MakeTemporaryPath("exyoki_convert_excel_back", ".xlsx");

    const auto toJson = ConvertDocument(xlsxPath, jsonPath);
    CHECK(toJson.Ok);
    CHECK(toJson.Family == DocumentFamily::Excel);
    CHECK(toJson.SheetCount == 2);
    CHECK(toJson.CellCount >= 6);

    const auto toXlsx = ConvertDocument(jsonPath, backPath);
    CHECK(toXlsx.Ok);

    std::vector<ToolDiagnostic> diagnostics;
    const auto model = ReadExcelModel(backPath, {}, diagnostics);
    REQUIRE(model.Excel);
    REQUIRE(model.Excel->Sheets.size() == 2);

    const auto& data = model.Excel->Sheets[0];
    CHECK(data.Name == "Data");
    const auto* name = FindCell(data, "A1");
    REQUIRE(name != nullptr);
    CHECK(name->Type == "string");
    CHECK(name->Value == "Name");
    const auto* number = FindCell(data, "B2");
    REQUIRE(number != nullptr);
    CHECK(number->Type == "number");
    CHECK(number->Value.find("42.5") != std::string::npos);
    const auto* boolean = FindCell(data, "C2");
    REQUIRE(boolean != nullptr);
    CHECK(boolean->Type == "bool");
    CHECK(boolean->Value == "true");
    const auto* formula = FindCell(data, "D2");
    REQUIRE(formula != nullptr);
    CHECK(formula->Type == "formula");
    CHECK(formula->Formula == "SUM(B2:B2)");
    REQUIRE(data.Merges.size() == 1);
    CHECK(data.Merges[0] == "A4:B4");

    CHECK(model.Excel->Sheets[1].Name == "Second");

    std::filesystem::remove(xlsxPath);
    std::filesystem::remove(jsonPath);
    std::filesystem::remove(backPath);
}

TEST_CASE("Excel workbook converts to Markdown with absolute cell positions [unit] [tools] [conversion]")
{
    const auto xlsxPath = SaveSampleWorkbook();
    const auto markdownPath = MakeTemporaryPath("exyoki_convert_excel", ".md");
    const auto backPath = MakeTemporaryPath("exyoki_convert_excel_md_back", ".xlsx");

    const auto toMarkdown = ConvertDocument(xlsxPath, markdownPath);
    CHECK(toMarkdown.Ok);

    const auto markdown = ReadFileText(markdownPath);
    CHECK(markdown.find("## Data") != std::string::npos);
    CHECK(markdown.find("## Second") != std::string::npos);
    CHECK(markdown.find("Answer") != std::string::npos);
    CHECK(markdown.find("=SUM(B2:B2)") != std::string::npos);

    const auto toXlsx = ConvertDocument(markdownPath, backPath);
    CHECK(toXlsx.Ok);

    std::vector<ToolDiagnostic> diagnostics;
    const auto model = ReadExcelModel(backPath, {}, diagnostics);
    REQUIRE(model.Excel);
    REQUIRE(model.Excel->Sheets.size() == 2);
    const auto& data = model.Excel->Sheets[0];

    // Markdown tables are padded from A1, so positions stay absolute.
    const auto* answer = FindCell(data, "A2");
    REQUIRE(answer != nullptr);
    CHECK(answer->Value == "Answer");
    const auto* number = FindCell(data, "B2");
    REQUIRE(number != nullptr);
    CHECK(number->Type == "number");
    const auto* boolean = FindCell(data, "C2");
    REQUIRE(boolean != nullptr);
    CHECK(boolean->Type == "bool");
    const auto* formula = FindCell(data, "D2");
    REQUIRE(formula != nullptr);
    CHECK(formula->Type == "formula");
    CHECK(formula->Formula == "SUM(B2:B2)");

    std::filesystem::remove(xlsxPath);
    std::filesystem::remove(markdownPath);
    std::filesystem::remove(backPath);
}
