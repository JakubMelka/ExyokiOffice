// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include <doctest.h>

#include "TestSupport.hpp"

#include "ExyokiOffice/Excel/ExcelDocument.hpp"
#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/Spreadsheet.hpp"

#include <filesystem>

using namespace ExyokiOffice::Excel;

static CellRange DvRange(std::string_view text)
{
    return *CellRange::ParseA1(text);
}

TEST_CASE("Excel data validation creates and reads complete rules [unit] [excel] [excel-validation]")
{
    auto editor = ExcelDocumentEditor::CreateNew();
    auto sheet = editor->FirstWorksheet();
    ExcelDataValidationDefinition d;
    d.Type = DataValidationType::Whole;
    d.Operation = DataValidationOperator::Between;
    d.Formula1 = "1";
    d.Formula2 = "100";
    d.Ranges = {DvRange("A2:A20"), DvRange("C2:C20")};
    d.AllowBlank = true;
    d.ShowInputMessage = true;
    d.PromptTitle = "Quantity";
    d.Prompt = "Enter 1 through 100";
    d.ShowErrorMessage = true;
    d.ErrorStyle = DataValidationErrorStyle::Warning;
    d.ErrorTitle = "Invalid quantity";
    d.Error = "The value is outside the allowed range.";
    auto rule = sheet->CreateDataValidation(d);
    REQUIRE(rule);
    auto actual = rule->Definition();
    CHECK(actual.Type == d.Type);
    CHECK(actual.Operation == d.Operation);
    CHECK(actual.Formula1 == d.Formula1);
    CHECK(actual.Formula2 == d.Formula2);
    CHECK(actual.Ranges.size() == 2);
    CHECK(actual.AllowBlank);
    CHECK(actual.PromptTitle == d.PromptTitle);
    CHECK(actual.Error == d.Error);
    auto xml = sheet->GetPart()->GetXmlString();
    CHECK(xml.find("count=\"1\"") != std::string::npos);
    CHECK(xml.find("sqref=\"A2:A20 C2:C20\"") != std::string::npos);
}

TEST_CASE("Excel data validation rejects incompatible definitions atomically [unit] [excel] [excel-validation]")
{
    auto sheet = ExcelDocumentEditor::CreateNew()->FirstWorksheet();
    ExcelDataValidationDefinition valid;
    valid.Type = DataValidationType::List;
    valid.Formula1 = "\"Yes,No\"";
    valid.Ranges = {DvRange("B2:B10")};
    auto rule = sheet->CreateDataValidation(valid);
    REQUIRE(rule);
    const auto before = sheet->GetPart()->GetXmlString();
    auto invalid = valid;
    invalid.Operation = DataValidationOperator::Equal;
    CHECK_FALSE(sheet->UpdateDataValidation(rule, invalid));
    CHECK(sheet->GetPart()->GetXmlString() == before);
    invalid = valid;
    invalid.Ranges.push_back(invalid.Ranges.front());
    CHECK_FALSE(IsValidExcelDataValidation(invalid));
    invalid = valid;
    invalid.PromptTitle = std::string(33, 'x');
    CHECK_FALSE(sheet->CreateDataValidation(invalid));
}

TEST_CASE("Excel data validation update clears obsolete metadata [unit] [excel] [excel-validation]")
{
    auto sheet = ExcelDocumentEditor::CreateNew()->FirstWorksheet();
    ExcelDataValidationDefinition d;
    d.Type = DataValidationType::Decimal;
    d.Operation = DataValidationOperator::GreaterThan;
    d.Formula1 = "0";
    d.Ranges = {DvRange("D2:D9")};
    d.Prompt = "Positive";
    auto rule = sheet->CreateDataValidation(d);
    REQUIRE(rule);
    ExcelDataValidationDefinition custom;
    custom.Type = DataValidationType::Custom;
    custom.Formula1 = "MOD(D2,2)=0";
    custom.Ranges = {DvRange("D2:D20")};
    REQUIRE(sheet->UpdateDataValidation(rule, custom));
    const auto actual = rule->Definition();
    CHECK(actual.Type == DataValidationType::Custom);
    CHECK_FALSE(actual.Operation);
    CHECK_FALSE(actual.Prompt);
    CHECK_FALSE(actual.Formula2);
    REQUIRE(actual.Ranges.size() == 1);
    CHECK(actual.Ranges.front().ToA1() == "D2:D20");
}

TEST_CASE("Excel data validation enforces worksheet ownership and removes container [unit] [excel] [excel-validation]")
{
    auto editor = ExcelDocumentEditor::CreateNew();
    auto first = editor->FirstWorksheet();
    auto second = editor->AddWorksheet("Other");
    ExcelDataValidationDefinition d;
    d.Type = DataValidationType::TextLength;
    d.Operation = DataValidationOperator::LessThanOrEqual;
    d.Formula1 = "12";
    d.Ranges = {DvRange("A1")};
    auto rule = first->CreateDataValidation(d);
    REQUIRE(rule);
    CHECK_FALSE(second->UpdateDataValidation(rule, d));
    CHECK_FALSE(second->RemoveDataValidation(rule));
    CHECK(first->RemoveDataValidation(rule));
    CHECK(first->DataValidations().empty());
    CHECK_FALSE(first->GetLowLevelApi()->GetFirstChildOfType<ExyokiOffice::DocumentFormat::OpenXml::Spreadsheet::DataValidations>());
}

TEST_CASE("Excel data validation survives package round trip [unit] [excel] [excel-validation]")
{
    auto editor = ExcelDocumentEditor::CreateNew();
    ExcelDataValidationDefinition d;
    d.Type = DataValidationType::Date;
    d.Operation = DataValidationOperator::Between;
    d.Formula1 = "DATE(2025,1,1)";
    d.Formula2 = "DATE(2025,12,31)";
    d.Ranges = {DvRange("A2:A100")};
    REQUIRE(editor->FirstWorksheet()->CreateDataValidation(d));
    const auto path = ExyokiOfficeTests::MakeTemporaryPath("exyokioffice-data-validation", ".xlsx");
    REQUIRE(editor->SaveToFile(path));
    auto reopened = ExcelDocumentEditor::Open(path);
    REQUIRE(reopened);
    auto rules = reopened->FirstWorksheet()->DataValidations();
    REQUIRE(rules.size() == 1);
    CHECK(rules.front()->Definition().Formula2 == d.Formula2);
    std::filesystem::remove(path);
}
