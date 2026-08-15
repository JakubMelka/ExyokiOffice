// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "ExyokiOffice/Excel/ExcelDocument.hpp"
#include <doctest.h>

using namespace ExyokiOffice::Excel;

namespace
{
CellRange R(std::string_view value)
{
    return *CellRange::ParseA1(value);
}
ExcelConditionalFormattingDefinition Expression(std::string formula, std::string range = "A1:A5")
{
    return ExcelConditionalFormattingDefinition::Expression({R(range)}, std::move(formula));
}
} // namespace

TEST_CASE("conditional formatting factories create valid common rules [unit] [excel] [conditional-formatting]")
{
    using D = ExcelConditionalFormattingDefinition;
    const std::vector<CellRange> ranges{R("A1:A10")};
    std::vector<D> definitions{D::Expression(ranges, "A1>0"),
                               D::CellIs(ranges, ConditionalFormattingOperator::Equal, "1"),
                               D::Between(ranges, "1", "10"),
                               D::NotBetween(ranges, "1", "10"),
                               D::ContainsText(ranges, "red"),
                               D::NotContainsText(ranges, "red"),
                               D::BeginsWith(ranges, "A"),
                               D::EndsWith(ranges, "Z"),
                               D::UniqueValues(ranges),
                               D::DuplicateValues(ranges),
                               D::ContainsBlanks(ranges),
                               D::NotContainsBlanks(ranges),
                               D::ContainsErrors(ranges),
                               D::NotContainsErrors(ranges),
                               D::Top(ranges, 10),
                               D::Bottom(ranges, 5, true),
                               D::AboveAverage(ranges, true, 2),
                               D::BelowAverage(ranges, false, 1)};
    for (const auto& definition : definitions)
    {
        CAPTURE(static_cast<int>(definition.Type));
    }
    for (const auto& definition : definitions)
    {
        CHECK(IsValidExcelConditionalFormatting(definition));
    }
    CHECK(definitions[2].Operation == ConditionalFormattingOperator::Between);
    CHECK(definitions[2].Formulas.size() == 2);
    CHECK(definitions[15].IsBottom);
    CHECK(definitions[15].Percent);
    CHECK(definitions[15].Rank == 5);
    CHECK_FALSE(definitions[17].IsAboveAverage);
    CHECK(definitions[16].EqualAverage);
    CHECK(definitions[16].StandardDeviation == 2);
    CHECK_FALSE(IsValidExcelConditionalFormatting(D::CellIs(ranges, ConditionalFormattingOperator::Between, "1")));
    CHECK_FALSE(IsValidExcelConditionalFormatting(D::Top(ranges, 0)));
}

TEST_CASE("conditional formatting validates rule-specific fields [unit] [excel] [conditional-formatting]")
{
    auto d = Expression("A1>0");
    CHECK(IsValidExcelConditionalFormatting(d));
    d.Formulas.clear();
    CHECK_FALSE(IsValidExcelConditionalFormatting(d));
    d = Expression("A1>0");
    d.Ranges.push_back(R("A1:A5"));
    CHECK_FALSE(IsValidExcelConditionalFormatting(d));
    d = {};
    d.Type = ConditionalFormattingType::CellIs;
    d.Ranges = {R("B1")};
    d.Operation = ConditionalFormattingOperator::Between;
    d.Formulas = {"1"};
    CHECK_FALSE(IsValidExcelConditionalFormatting(d));
    d.Formulas.push_back("10");
    CHECK(IsValidExcelConditionalFormatting(d));
    d = {};
    d.Type = ConditionalFormattingType::ContainsText;
    d.Ranges = {R("C1:C9")};
    d.Text = "urgent";
    CHECK(IsValidExcelConditionalFormatting(d));
    d.Text = "";
    CHECK_FALSE(IsValidExcelConditionalFormatting(d));
    d = {};
    d.Type = ConditionalFormattingType::Top;
    d.Ranges = {R("D1:D9")};
    d.Rank = 0;
    CHECK_FALSE(IsValidExcelConditionalFormatting(d));
    d.Rank = 5;
    d.Percent = true;
    d.IsBottom = true;
    CHECK(IsValidExcelConditionalFormatting(d));
    d = {};
    d.Type = ConditionalFormattingType::AboveAverage;
    d.Ranges = {R("E1:E9")};
    d.StandardDeviation = -1;
    CHECK_FALSE(IsValidExcelConditionalFormatting(d));
}

TEST_CASE(
    "conditional formatting CRUD preserves definitions and stable priorities [unit] [excel] [conditional-formatting]")
{
    auto editor = ExcelDocumentEditor::CreateNew();
    auto sheet = editor->FirstWorksheet();
    auto first = sheet->CreateConditionalFormatting(Expression("A1>5"));
    REQUIRE(first);
    CHECK(first->Priority() == 1);
    ExcelConditionalFormattingDefinition cell;
    cell.Type = ConditionalFormattingType::CellIs;
    cell.Ranges = {R("B1:B8"), R("D1:D8")};
    cell.Operation = ConditionalFormattingOperator::Between;
    cell.Formulas = {"1", "8"};
    cell.DifferentialFormatId = 3;
    cell.StopIfTrue = true;
    auto second = sheet->CreateConditionalFormatting(cell);
    REQUIRE(second);
    CHECK(second->Priority() == 2);
    auto third = sheet->CreateConditionalFormatting(Expression("C1=0", "C1:C8"));
    REQUIRE(third);
    REQUIRE(sheet->MoveConditionalFormatting(third, 1));
    CHECK(third->Priority() == 1);
    CHECK(first->Priority() == 2);
    CHECK(second->Priority() == 3);
    auto changed = cell;
    changed.Type = ConditionalFormattingType::ContainsText;
    changed.Operation.reset();
    changed.Formulas.clear();
    changed.Text = "late";
    REQUIRE(sheet->UpdateConditionalFormatting(second, changed));
    CHECK(second->Priority() == 3);
    CHECK(second->Definition().Text == "late");
    auto invalid = changed;
    invalid.Text = "";
    CHECK_FALSE(sheet->UpdateConditionalFormatting(second, invalid));
    CHECK(second->Definition().Text == "late");
    REQUIRE(sheet->RemoveConditionalFormatting(first));
    CHECK(third->Priority() == 1);
    CHECK(second->Priority() == 2);
    CHECK_FALSE(sheet->MoveConditionalFormatting(second, 0));
    CHECK_FALSE(sheet->MoveConditionalFormatting(second, 3));
}

TEST_CASE("conditional formatting rejects foreign ownership and round trips package XML [unit] [excel] "
          "[conditional-formatting]")
{
    auto editor = ExcelDocumentEditor::CreateNew();
    auto first = editor->FirstWorksheet();
    auto other = editor->AddWorksheet("Other");
    auto rule = first->CreateConditionalFormatting(Expression("SUM(A1:A3)>2"));
    REQUIRE(rule);
    CHECK_FALSE(other->UpdateConditionalFormatting(rule, Expression("TRUE")));
    CHECK_FALSE(other->RemoveConditionalFormatting(rule));
    auto bytes = editor->SaveToMemory();
    REQUIRE_FALSE(bytes.empty());
    auto reopened = ExcelDocumentEditor::Open(bytes);
    REQUIRE(reopened);
    auto rules = reopened->FirstWorksheet()->ConditionalFormattings();
    REQUIRE(rules.size() == 1);
    const auto d = rules[0]->Definition();
    CHECK(d.Formulas == std::vector<std::string>{"SUM(A1:A3)>2"});
    REQUIRE(d.Ranges.size() == 1);
    CHECK(d.Ranges[0].ToA1() == "A1:A5");
    CHECK(rules[0]->Priority() == 1);
    REQUIRE(reopened->FirstWorksheet()->RemoveConditionalFormatting(rules[0]));
    auto again = reopened->SaveToMemory();
    REQUIRE_FALSE(again.empty());
    auto finalDoc = ExcelDocumentEditor::Open(again);
    REQUIRE(finalDoc);
    CHECK(finalDoc->FirstWorksheet()->ConditionalFormattings().empty());
}
