// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "McpTestSupport.hpp"

#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/Spreadsheet.hpp"
#include "ExyokiOffice/Excel/ExcelDocument.hpp"
#include "ExyokiOffice/Packaging/GeneratedParts.hpp"
#include "ExyokiOffice/Tools/ValidationRunner.hpp"

#include <doctest/doctest.h>

#include <string>

using namespace ExyokiOfficeTests;
using namespace ExyokiOffice::Mcp;

/// Round-trip helpers the Excel tool tests share.
class ExcelToolTestSupport
{
public:
    /**
     * @brief Plain text of one cell of a reopened workbook.
     *
     * Saving may move a string into the shared-string table, so a round-trip
     * assertion has to resolve the index rather than read the raw cell text.
     */
    [[nodiscard]] static std::string CellText(ExyokiOffice::Excel::ExcelDocumentEditor& editor,
                                              const ExyokiOffice::Excel::Worksheet& sheet, const char* address)
    {
        const auto parsed = ExyokiOffice::Excel::CellAddress::ParseA1(address);
        if (!parsed.has_value())
        {
            return {};
        }

        const auto stored = sheet.GetCellValue(*parsed);
        if (!stored.has_value())
        {
            return {};
        }

        if (stored->Kind() == ExyokiOffice::Excel::CellValueKind::SharedString)
        {
            const auto index = stored->SharedStringIndex();
            if (!index.has_value())
            {
                return {};
            }

            return editor.SharedStrings().Lookup(*index).value_or(std::string());
        }

        return stored->Text();
    }

    /// Marks the worksheet at @p index hidden, which no high-level API exposes.
    static bool HideWorksheet(ExyokiOffice::Excel::ExcelDocumentEditor& editor, ExyokiOffice::Size index)
    {
        namespace Spreadsheet = ExyokiOffice::DocumentFormat::OpenXml::Spreadsheet;

        const auto document = editor.GetDocument();
        const auto part = document != nullptr ? document->GetWorkbookPart() : nullptr;
        const auto workbook = part != nullptr ? part->GetWorkbook() : nullptr;
        const auto sheets = workbook != nullptr ? workbook->GetFirstChildOfType<Spreadsheet::Sheets>() : nullptr;
        if (sheets == nullptr)
        {
            return false;
        }

        const auto elements = sheets->Elements<Spreadsheet::Sheet>();
        if (index >= elements.size() || elements[index] == nullptr)
        {
            return false;
        }

        elements[index]->SetState(ExyokiOffice::EnumValue<Spreadsheet::SheetStateValues>(
            Spreadsheet::SheetStateValues::Hidden));
        return true;
    }
};

TEST_CASE("worksheets are listed, added, renamed, and deleted [mcp-excel]")
{
    auto server = MakeExcelServer();
    server->Initialize();

    const auto created = server->Call("create_document", nlohmann::json{{"path", "book.xlsx"}});
    const auto documentId = created["data"]["documentId"].get<std::string>();

    const auto listed = server->Call("list_sheets", nlohmann::json{{"documentId", documentId}});
    REQUIRE(listed["ok"] == true);
    REQUIRE(listed["data"]["sheets"].size() == 1);

    const auto added = server->Call("add_sheet", nlohmann::json{{"documentId", documentId}, {"name", "Data"}});
    REQUIRE(added["ok"] == true);
    CHECK(added["data"]["index"] == 2);

    const auto duplicate = server->Call("add_sheet", nlohmann::json{{"documentId", documentId}, {"name", "Data"}});
    CHECK(duplicate["ok"] == false);

    const auto invalid = server->Call("add_sheet", nlohmann::json{{"documentId", documentId}, {"name", "Bad:Name"}});
    CHECK(invalid["ok"] == false);
    CHECK(invalid["error"]["code"] == "input_invalid");

    const auto renamed = server->Call(
        "rename_sheet", nlohmann::json{{"documentId", documentId}, {"sheet", "Data"}, {"new_name", "Numbers"}});
    REQUIRE(renamed["ok"] == true);

    const auto missing = server->Call("delete_sheet", nlohmann::json{{"documentId", documentId}, {"sheet", "Ghost"}});
    CHECK(missing["ok"] == false);
    CHECK(missing["error"]["code"] == "sheet_not_found");

    const auto deleted =
        server->Call("delete_sheet", nlohmann::json{{"documentId", documentId}, {"sheet", "Numbers"}});
    CHECK(deleted["ok"] == true);
    CHECK(deleted["data"]["sheetCount"] == 1);

    const auto last = server->Call("delete_sheet", nlohmann::json{{"documentId", documentId}, {"sheet", 1}});
    CHECK(last["ok"] == false);
    CHECK(last["error"]["code"] == "operation_failed");
}

TEST_CASE("cells are written and read back through the library [mcp-excel]")
{
    auto server = MakeExcelServer();
    server->Initialize();

    const auto created = server->Call("create_document", nlohmann::json{{"path", "cells.xlsx"}});
    const auto documentId = created["data"]["documentId"].get<std::string>();

    const auto written = server->Call(
        "write_cells",
        nlohmann::json{{"documentId", documentId},
                       {"cells", nlohmann::json::array({nlohmann::json{{"address", "A1"}, {"value", "Region"}},
                                                        nlohmann::json{{"address", "B1"}, {"value", "Revenue"}},
                                                        nlohmann::json{{"address", "A2"}, {"value", "North"}},
                                                        nlohmann::json{{"address", "B2"}, {"value", 1200}}})}});
    REQUIRE(written["ok"] == true);
    CHECK(written["data"]["written"] == 4);

    const auto block = server->Call(
        "write_range",
        nlohmann::json{{"documentId", documentId},
                       {"origin", "A3"},
                       {"values", nlohmann::json::array({nlohmann::json::array({"South", 900}),
                                                         nlohmann::json::array({nullptr, 700})})}});
    REQUIRE(block["ok"] == true);
    CHECK(block["data"]["written"] == 3);

    const auto values = server->Call(
        "read_range", nlohmann::json{{"documentId", documentId}, {"sheet", 1}, {"range", "A1:B4"}});
    REQUIRE(values["ok"] == true);
    REQUIRE(values["data"]["values"].size() == 4);
    CHECK(values["data"]["values"][0][0] == "Region");
    CHECK(values["data"]["values"][1][1] == 1200.0);
    CHECK(values["data"]["values"][3][0].is_null());

    static_cast<void>(server->Call("save_document", nlohmann::json{{"documentId", documentId}}));

    auto editor = ExyokiOffice::Excel::ExcelDocumentEditor::Open(server->Path("cells.xlsx"));
    REQUIRE(editor != nullptr);
    auto sheet = editor->FirstWorksheet();
    REQUIRE(sheet != nullptr);
    const auto address = ExyokiOffice::Excel::CellAddress::ParseA1("B2");
    REQUIRE(address.has_value());
    const auto stored = sheet->GetCellValue(*address);
    REQUIRE(stored.has_value());
    if (stored.has_value())
    {
        CHECK(stored->Text() == "1200");
    }

    const auto report = ExyokiOffice::Tools::Run(server->Path("cells.xlsx"));
    CHECK_MESSAGE(report.ErrorCount == 0, DescribeValidationErrors(report));
}

TEST_CASE("a malformed A1 reference is reported as range_invalid [mcp-excel]")
{
    auto server = MakeExcelServer();
    server->Initialize();

    const auto created = server->Call("create_document", nlohmann::json::object());
    const auto documentId = created["data"]["documentId"].get<std::string>();

    const auto refused = server->Call("read_range", nlohmann::json{{"documentId", documentId}, {"range", "not a range"}});
    CHECK(refused["ok"] == false);
    CHECK(refused["error"]["code"] == "range_invalid");

    const auto missingSheet =
        server->Call("read_range", nlohmann::json{{"documentId", documentId}, {"sheet", "Ghost"}});
    CHECK(missingSheet["ok"] == false);
    CHECK(missingSheet["error"]["code"] == "sheet_not_found");
}

TEST_CASE("formulas are stored and recalculated [mcp-excel]")
{
    auto server = MakeExcelServer();
    server->Initialize();

    const auto created = server->Call("create_document", nlohmann::json{{"path", "formulas.xlsx"}});
    const auto documentId = created["data"]["documentId"].get<std::string>();

    static_cast<void>(server->Call(
        "write_cells",
        nlohmann::json{{"documentId", documentId},
                       {"cells", nlohmann::json::array({nlohmann::json{{"address", "A1"}, {"value", 2}},
                                                        nlohmann::json{{"address", "A2"}, {"value", 3}},
                                                        nlohmann::json{{"address", "A3"},
                                                                       {"value", nlohmann::json{{"formula",
                                                                                                 "SUM(A1:A2)"}}}}})}}));

    const auto recalculated = server->Call("recalculate", nlohmann::json{{"documentId", documentId}});
    REQUIRE(recalculated["ok"] == true);
    CHECK(recalculated["data"]["recalculatedCells"] == 1);
    CHECK(recalculated["data"]["circularReferences"].empty());

    const auto cells = server->Call(
        "read_range",
        nlohmann::json{{"documentId", documentId}, {"range", "A3"}, {"mode", "cells"}, {"include_formulas", true}});
    REQUIRE(cells["data"]["cells"].size() == 1);
    CHECK(cells["data"]["cells"][0]["type"] == "formula");
    CHECK(cells["data"]["cells"][0]["formula"] == "SUM(A1:A2)");
    CHECK(cells["data"]["cells"][0]["value"] == "5");
}

TEST_CASE("ranges are cleared, merged, and formatted [mcp-excel]")
{
    auto server = MakeExcelServer();
    server->Initialize();

    const auto created = server->Call("create_document", nlohmann::json{{"path", "format.xlsx"}});
    const auto documentId = created["data"]["documentId"].get<std::string>();

    static_cast<void>(server->Call(
        "write_range", nlohmann::json{{"documentId", documentId},
                                      {"origin", "A1"},
                                      {"values", nlohmann::json::array({nlohmann::json::array({"A", "B", "C"})})}}));

    const auto formatted = server->Call("format_range", nlohmann::json{{"documentId", documentId},
                                                                       {"range", "A1:C1"},
                                                                       {"font", nlohmann::json{{"bold", true}}},
                                                                       {"fill", nlohmann::json{{"color", "#FFFF00"}}},
                                                                       {"number_format", "#,##0.00"}});
    REQUIRE(formatted["ok"] == true);

    const auto merged = server->Call("merge_cells", nlohmann::json{{"documentId", documentId}, {"range", "A3:C3"}});
    REQUIRE(merged["ok"] == true);

    const auto unmerged =
        server->Call("merge_cells", nlohmann::json{{"documentId", documentId}, {"range", "A3:C3"}, {"unmerge", true}});
    CHECK(unmerged["ok"] == true);

    const auto cleared =
        server->Call("clear_range", nlohmann::json{{"documentId", documentId}, {"range", "A1:C1"}, {"what", "all"}});
    REQUIRE(cleared["ok"] == true);

    static_cast<void>(server->Call("save_document", nlohmann::json{{"documentId", documentId}}));
    const auto report = ExyokiOffice::Tools::Run(server->Path("format.xlsx"));
    CHECK_MESSAGE(report.ErrorCount == 0, DescribeValidationErrors(report));
}

TEST_CASE("rows and columns are inserted and sized [mcp-excel]")
{
    auto server = MakeExcelServer();
    server->Initialize();

    const auto created = server->Call("create_document", nlohmann::json::object());
    const auto documentId = created["data"]["documentId"].get<std::string>();

    static_cast<void>(server->Call(
        "write_cells",
        nlohmann::json{{"documentId", documentId},
                       {"cells", nlohmann::json::array({nlohmann::json{{"address", "A1"}, {"value", "First"}}})}}));

    const auto inserted = server->Call("modify_sheet_structure",
                                       nlohmann::json{{"documentId", documentId}, {"operation", "insert_rows"}, {"at", 1}});
    REQUIRE(inserted["ok"] == true);

    const auto moved = server->Call("read_range", nlohmann::json{{"documentId", documentId}, {"range", "A2"}});
    CHECK(moved["data"]["values"][0][0] == "First");

    const auto width = server->Call(
        "set_column_width", nlohmann::json{{"documentId", documentId}, {"columns", "A:C"}, {"width", 18.0}});
    REQUIRE(width["ok"] == true);
    CHECK(width["data"]["columns"] == 3);

    const auto height =
        server->Call("set_row_height", nlohmann::json{{"documentId", documentId}, {"rows", "1:2"}, {"height", 24.0}});
    REQUIRE(height["ok"] == true);
    CHECK(height["data"]["rows"] == 2);

    const auto badBand =
        server->Call("set_column_width", nlohmann::json{{"documentId", documentId}, {"columns", "1:2"}, {"width", 10.0}});
    CHECK(badBand["ok"] == false);
    CHECK(badBand["error"]["code"] == "range_invalid");
}

TEST_CASE("panes are frozen and released [mcp-excel]")
{
    auto server = MakeExcelServer();
    server->Initialize();

    const auto created = server->Call("create_document", nlohmann::json::object());
    const auto documentId = created["data"]["documentId"].get<std::string>();

    const auto frozen = server->Call("freeze_panes", nlohmann::json{{"documentId", documentId}, {"cell", "B2"}});
    REQUIRE(frozen["ok"] == true);
    CHECK(frozen["data"]["frozenRows"] == 1);
    CHECK(frozen["data"]["frozenColumns"] == 1);

    const auto released = server->Call("freeze_panes", nlohmann::json{{"documentId", documentId}, {"cell", "A1"}});
    CHECK(released["data"]["frozenRows"] == 0);
}

TEST_CASE("tables, names, validation, and formatting rules are added [mcp-excel]")
{
    auto server = MakeExcelServer();
    server->Initialize();

    const auto created = server->Call("create_document", nlohmann::json{{"path", "analysis.xlsx"}});
    const auto documentId = created["data"]["documentId"].get<std::string>();

    static_cast<void>(server->Call(
        "write_range",
        nlohmann::json{{"documentId", documentId},
                       {"origin", "A1"},
                       {"values", nlohmann::json::array({nlohmann::json::array({"Region", "Revenue"}),
                                                         nlohmann::json::array({"North", 1200}),
                                                         nlohmann::json::array({"South", 900})})}}));

    const auto table =
        server->Call("add_table", nlohmann::json{{"documentId", documentId}, {"range", "A1:B3"}, {"name", "Sales"}});
    REQUIRE(table["ok"] == true);
    CHECK(table["data"]["columns"][0] == "Region");

    const auto named = server->Call(
        "add_named_range", nlohmann::json{{"documentId", documentId}, {"name", "SalesData"}, {"range", "A1:B3"}});
    REQUIRE(named["ok"] == true);

    const auto validation = server->Call(
        "add_data_validation",
        nlohmann::json{{"documentId", documentId},
                       {"range", "C2:C3"},
                       {"rule", nlohmann::json{{"type", "list"},
                                               {"values", nlohmann::json::array({"Yes", "No"})}}}});
    REQUIRE(validation["ok"] == true);

    const auto formatting = server->Call(
        "add_conditional_formatting",
        nlohmann::json{{"documentId", documentId},
                       {"range", "B2:B3"},
                       {"rule", nlohmann::json{{"type", "cellIs"}, {"operator", "greaterThan"}, {"formula1", "1000"}}}});
    REQUIRE(formatting["ok"] == true);

    const auto unsupported = server->Call(
        "add_conditional_formatting",
        nlohmann::json{{"documentId", documentId}, {"range", "B2:B3"}, {"rule", nlohmann::json{{"type", "cellIs"}}}});
    CHECK(unsupported["ok"] == false);

    static_cast<void>(server->Call("save_document", nlohmann::json{{"documentId", documentId}}));
    const auto report = ExyokiOffice::Tools::Run(server->Path("analysis.xlsx"));
    CHECK_MESSAGE(report.ErrorCount == 0, DescribeValidationErrors(report));
}

TEST_CASE("a chart is anchored on the worksheet [mcp-excel]")
{
    auto server = MakeExcelServer();
    server->Initialize();

    const auto created = server->Call("create_document", nlohmann::json{{"path", "chart.xlsx"}});
    const auto documentId = created["data"]["documentId"].get<std::string>();

    static_cast<void>(server->Call(
        "write_range", nlohmann::json{{"documentId", documentId},
                                      {"origin", "A1"},
                                      {"values", nlohmann::json::array({nlohmann::json::array({10}),
                                                                        nlohmann::json::array({20}),
                                                                        nlohmann::json::array({30})})}}));

    const auto chart = server->Call("add_chart", nlohmann::json{{"documentId", documentId},
                                                                {"type", "column"},
                                                                {"data_range", "A1:A3"},
                                                                {"anchor_cell", "C2"},
                                                                {"title", "Revenue"}});
    REQUIRE(chart["ok"] == true);
    CHECK(chart["data"]["anchor"] == "C2");

    static_cast<void>(server->Call("save_document", nlohmann::json{{"documentId", documentId}}));

    auto editor = ExyokiOffice::Excel::ExcelDocumentEditor::Open(server->Path("chart.xlsx"));
    REQUIRE(editor != nullptr);
    auto sheet = editor->FirstWorksheet();
    REQUIRE(sheet != nullptr);
    CHECK(sheet->Charts().size() == 1);
}

TEST_CASE("a hyperlink is set and removed [mcp-excel]")
{
    auto server = MakeExcelServer();
    server->Initialize();

    const auto created = server->Call("create_document", nlohmann::json::object());
    const auto documentId = created["data"]["documentId"].get<std::string>();

    const auto linked = server->Call("set_hyperlink", nlohmann::json{{"documentId", documentId},
                                                                     {"cell", "A1"},
                                                                     {"target", "https://example.com"},
                                                                     {"tooltip", "Example"}});
    REQUIRE(linked["ok"] == true);
    CHECK(linked["data"]["removed"] == false);

    const auto removed = server->Call("set_hyperlink", nlohmann::json{{"documentId", documentId}, {"cell", "A1"}});
    CHECK(removed["ok"] == true);
    CHECK(removed["data"]["removed"] == true);
}

TEST_CASE("a pivot table is built from a source range [mcp-excel]")
{
    auto server = MakeExcelServer();
    server->Initialize();

    const auto created = server->Call("create_document", nlohmann::json{{"path", "pivot.xlsx"}});
    const auto documentId = created["data"]["documentId"].get<std::string>();

    static_cast<void>(server->Call(
        "write_range",
        nlohmann::json{{"documentId", documentId},
                       {"origin", "A1"},
                       {"values", nlohmann::json::array({nlohmann::json::array({"Region", "Revenue"}),
                                                         nlohmann::json::array({"North", 1200}),
                                                         nlohmann::json::array({"South", 900}),
                                                         nlohmann::json::array({"North", 300})})}}));
    static_cast<void>(server->Call("add_sheet", nlohmann::json{{"documentId", documentId}, {"name", "Report"}}));

    const auto pivot = server->Call(
        "add_pivot_table",
        nlohmann::json{{"documentId", documentId},
                       {"source_sheet", "Sheet1"},
                       {"source_range", "A1:B4"},
                       {"target_sheet", "Report"},
                       {"target_cell", "A1"},
                       {"rows", nlohmann::json::array({"Region"})},
                       {"values", nlohmann::json::array({nlohmann::json{{"field", "Revenue"}, {"aggregate", "sum"}}})}});

    REQUIRE(pivot["ok"] == true);
    CHECK_FALSE(pivot["data"]["name"].get<std::string>().empty());
    CHECK(pivot["data"]["targetCell"] == "A1");

    REQUIRE(server->Call("save_document", nlohmann::json{{"documentId", documentId}})["ok"] == true);
    const auto report = ExyokiOffice::Tools::Run(server->Path("pivot.xlsx"));
    CHECK(report.Loaded);
    CHECK_MESSAGE(report.ErrorCount == 0, DescribeValidationErrors(report));

    const auto unknownSheet = server->Call("add_pivot_table", nlohmann::json{{"documentId", documentId},
                                                                             {"source_range", "A1:B4"},
                                                                             {"target_sheet", "Ghost"},
                                                                             {"target_cell", "A1"}});
    CHECK(unknownSheet["ok"] == false);
    CHECK(unknownSheet["error"]["code"] == "sheet_not_found");
}

TEST_CASE("an Excel batch is atomic and is one undo step [mcp-excel]")
{
    auto server = MakeExcelServer();
    server->Initialize();
    const auto created = server->Call("create_document", nlohmann::json::object());
    const auto documentId = created["data"]["documentId"].get<std::string>();

    const auto applied = server->Call(
        "batch", nlohmann::json{{"documentId", documentId},
                                {"operations", nlohmann::json::array(
                                                   {nlohmann::json{{"tool", "write_range"},
                                                                   {"arguments", {{"origin", "A1"}, {"values", {{"one"}}}}}},
                                                    nlohmann::json{{"tool", "format_range"},
                                                                   {"arguments", {{"range", "A1"}, {"font", {{"bold", true}}}}}}})}});
    REQUIRE(applied["ok"] == true);
    CHECK(applied["revision"] == 1);
    CHECK(server->Call("read_range", nlohmann::json{{"documentId", documentId}, {"range", "A1"}})
              ["data"]["values"][0][0] == "one");

    const auto failed = server->Call(
        "batch", nlohmann::json{{"documentId", documentId},
                                {"operations", nlohmann::json::array(
                                                   {nlohmann::json{{"tool", "write_range"},
                                                                   {"arguments", {{"origin", "A1"}, {"values", {{"changed"}}}}}},
                                                    nlohmann::json{{"tool", "format_range"},
                                                                   {"arguments", {{"range", "invalid"}}}}})}});
    CHECK(failed["ok"] == false);
    CHECK(failed["error"]["code"] == "batch_aborted");
    CHECK(server->Call("read_range", nlohmann::json{{"documentId", documentId}, {"range", "A1"}})
              ["data"]["values"][0][0] == "one");

    REQUIRE(server->Call("undo", nlohmann::json{{"documentId", documentId}})["ok"] == true);
    CHECK(server->Call("read_range", nlohmann::json{{"documentId", documentId}, {"range", "A1"}})
              ["data"]["values"][0][0]
                  .is_null());
}

TEST_CASE("write_cells leaves a null value untouched [mcp-excel]")
{
    auto server = MakeExcelServer();
    server->Initialize();

    const auto created = server->Call("create_document", nlohmann::json{{"path", "nulls.xlsx"}});
    const auto documentId = created["data"]["documentId"].get<std::string>();

    REQUIRE(server->Call("write_cells",
                         nlohmann::json{{"documentId", documentId},
                                        {"cells", nlohmann::json::array(
                                                      {nlohmann::json{{"address", "A1"}, {"value", "keep"}}})}})
                ["ok"] == true);

    const auto written = server->Call(
        "write_cells",
        nlohmann::json{{"documentId", documentId},
                       {"cells", nlohmann::json::array({nlohmann::json{{"address", "A1"}, {"value", nullptr}},
                                                        nlohmann::json{{"address", "B1"}, {"value", "added"}}})}});
    REQUIRE(written["ok"] == true);
    CHECK(written["data"]["written"] == 1);
    CHECK(written["data"]["skipped"] == 1);

    const auto values = server->Call("read_range", nlohmann::json{{"documentId", documentId}, {"range", "A1:B1"}});
    CHECK(values["data"]["values"][0][0] == "keep");
    CHECK(values["data"]["values"][0][1] == "added");

    // The address is still validated even when the value is skipped.
    const auto refused = server->Call(
        "write_cells", nlohmann::json{{"documentId", documentId},
                                      {"cells", nlohmann::json::array(
                                                    {nlohmann::json{{"address", "nope"}, {"value", nullptr}}})}});
    CHECK(refused["ok"] == false);
    CHECK(refused["error"]["code"] == "range_invalid");

    REQUIRE(server->Call("save_document", nlohmann::json{{"documentId", documentId}})["ok"] == true);

    auto editor = ExyokiOffice::Excel::ExcelDocumentEditor::Open(server->Path("nulls.xlsx"));
    REQUIRE(editor != nullptr);
    auto sheet = editor->FirstWorksheet();
    REQUIRE(sheet != nullptr);
    CHECK(ExcelToolTestSupport::CellText(*editor, *sheet, "A1") == "keep");
    CHECK(ExcelToolTestSupport::CellText(*editor, *sheet, "B1") == "added");
}

TEST_CASE("a worksheet named \"2\" is not mistaken for an index [mcp-excel]")
{
    auto server = MakeExcelServer();
    server->Initialize();

    const auto created = server->Call("create_document", nlohmann::json{{"path", "numeric.xlsx"}});
    const auto documentId = created["data"]["documentId"].get<std::string>();

    REQUIRE(server->Call("add_sheet", nlohmann::json{{"documentId", documentId}, {"name", "Data"}})["ok"] == true);

    // The workbook already holds two sheets, so a duplicate check that also
    // resolved "2" as an index would wrongly refuse this name.
    const auto numeric = server->Call("add_sheet", nlohmann::json{{"documentId", documentId}, {"name", "2"}});
    REQUIRE(numeric["ok"] == true);
    CHECK(numeric["data"]["index"] == 3);

    // A genuine duplicate is still refused.
    CHECK(server->Call("add_sheet", nlohmann::json{{"documentId", documentId}, {"name", "2"}})["ok"] == false);

    REQUIRE(server->Call("write_cells",
                         nlohmann::json{{"documentId", documentId},
                                        {"sheet", "2"},
                                        {"cells", nlohmann::json::array(
                                                      {nlohmann::json{{"address", "A1"}, {"value", "named"}}})}})
                ["ok"] == true);

    // The string "2" names the third sheet; the integer 2 is still an index.
    const auto byName =
        server->Call("read_range", nlohmann::json{{"documentId", documentId}, {"sheet", "2"}, {"range", "A1"}});
    CHECK(byName["data"]["values"][0][0] == "named");

    const auto byIndex =
        server->Call("read_range", nlohmann::json{{"documentId", documentId}, {"sheet", 2}, {"range", "A1"}});
    CHECK(byIndex["data"]["values"][0][0].is_null());

    REQUIRE(server->Call("save_document", nlohmann::json{{"documentId", documentId}})["ok"] == true);

    auto editor = ExyokiOffice::Excel::ExcelDocumentEditor::Open(server->Path("numeric.xlsx"));
    REQUIRE(editor != nullptr);
    auto sheet = editor->GetWorksheet("2");
    REQUIRE(sheet != nullptr);
    CHECK(ExcelToolTestSupport::CellText(*editor, *sheet, "A1") == "named");
}

TEST_CASE("read_range past the end of the range returns an empty page [mcp-excel]")
{
    auto server = MakeExcelServer();
    server->Initialize();

    const auto created = server->Call("create_document", nlohmann::json::object());
    const auto documentId = created["data"]["documentId"].get<std::string>();

    REQUIRE(server->Call("write_range",
                         nlohmann::json{{"documentId", documentId},
                                        {"origin", "A1"},
                                        {"values", nlohmann::json::array({nlohmann::json::array({"first"}),
                                                                          nlohmann::json::array({"second"})})}})
                ["ok"] == true);

    const auto page = server->Call(
        "read_range", nlohmann::json{{"documentId", documentId}, {"range", "A1:A2"}, {"offset", 5}});
    REQUIRE(page["ok"] == true);
    CHECK(page["data"]["values"].empty());
    CHECK(page["data"]["cells"].empty());
    CHECK(page["data"]["csv"] == "");
    CHECK(page["data"]["nextOffset"] == 0);
    CHECK(page["truncated"] == false);

    // The offset that lands exactly on the end behaves the same way.
    const auto edge = server->Call(
        "read_range", nlohmann::json{{"documentId", documentId}, {"range", "A1:A2"}, {"offset", 2}});
    CHECK(edge["ok"] == true);
    CHECK(edge["data"]["values"].empty());
    CHECK(edge["truncated"] == false);

    // The last readable page still returns its row.
    const auto tail = server->Call(
        "read_range", nlohmann::json{{"documentId", documentId}, {"range", "A1:A2"}, {"offset", 1}});
    REQUIRE(tail["data"]["values"].size() == 1);
    CHECK(tail["data"]["values"][0][0] == "second");
    CHECK(tail["data"]["nextOffset"] == 0);
    CHECK(tail["truncated"] == false);
}

TEST_CASE("row and column bands reject trailing garbage [mcp-excel]")
{
    auto server = MakeExcelServer();
    server->Initialize();

    const auto created = server->Call("create_document", nlohmann::json::object());
    const auto documentId = created["data"]["documentId"].get<std::string>();

    for (const auto& band : {"2x:5", "2:5x", "2:", ":5", "", "2 5", "0:5"})
    {
        INFO("rows=" << band);
        const auto refused = server->Call(
            "set_row_height", nlohmann::json{{"documentId", documentId}, {"rows", band}, {"height", 20.0}});
        CHECK(refused["ok"] == false);
        CHECK(refused["error"]["code"] == "range_invalid");
    }

    for (const auto& band : {"B3:D", "B:D3", "B:", "1:2"})
    {
        INFO("columns=" << band);
        const auto refused = server->Call(
            "set_column_width", nlohmann::json{{"documentId", documentId}, {"columns", band}, {"width", 12.0}});
        CHECK(refused["ok"] == false);
        CHECK(refused["error"]["code"] == "range_invalid");
    }

    const auto accepted =
        server->Call("set_row_height", nlohmann::json{{"documentId", documentId}, {"rows", "2:5"}, {"height", 20.0}});
    REQUIRE(accepted["ok"] == true);
    CHECK(accepted["data"]["rows"] == 4);
}

TEST_CASE("list_sheets reports the hidden state [mcp-excel]")
{
    auto server = MakeExcelServer();
    server->Initialize();

    auto editor = ExyokiOffice::Excel::ExcelDocumentEditor::CreateNew();
    REQUIRE(editor != nullptr);
    REQUIRE(editor->AddWorksheet("Backstage") != nullptr);
    REQUIRE(ExcelToolTestSupport::HideWorksheet(*editor, 1));
    REQUIRE(editor->SaveToFile(server->Path("hidden.xlsx")));

    const auto listed = server->Call("list_sheets", nlohmann::json{{"path", "hidden.xlsx"}});
    REQUIRE(listed["ok"] == true);
    REQUIRE(listed["data"]["sheets"].size() == 2);
    CHECK(listed["data"]["sheets"][0]["hidden"] == false);
    CHECK(listed["data"]["sheets"][1]["name"] == "Backstage");
    CHECK(listed["data"]["sheets"][1]["hidden"] == true);

    // A workbook the server itself created has no hidden sheet.
    const auto created = server->Call("create_document", nlohmann::json::object());
    const auto documentId = created["data"]["documentId"].get<std::string>();
    const auto fresh = server->Call("list_sheets", nlohmann::json{{"documentId", documentId}});
    REQUIRE(fresh["data"]["sheets"].size() == 1);
    CHECK(fresh["data"]["sheets"][0]["hidden"] == false);
}

TEST_CASE("every worksheet argument accepts a name or an index [mcp-excel]")
{
    auto server = MakeExcelServer();
    server->Initialize();

    for (const auto& tool : server->Registry().Tools())
    {
        // The path-based utilities hold no open workbook, so a sheet position
        // has nothing to resolve against; they take a name and say so.
        if (tool.Definition.Group == "files")
        {
            continue;
        }

        const auto properties = tool.Definition.InputSchema.find("properties");
        if (properties == tool.Definition.InputSchema.end())
        {
            continue;
        }

        for (const auto& name : {"sheet", "source_sheet", "target_sheet"})
        {
            const auto property = properties->find(name);
            if (property == properties->end())
            {
                continue;
            }

            INFO(tool.Definition.Name << "." << name);
            const auto type = property->find("type");
            REQUIRE(type != property->end());
            CHECK(*type == nlohmann::json::array({"string", "integer"}));
        }
    }

    const auto created = server->Call("create_document", nlohmann::json{{"path", "indexed.xlsx"}});
    const auto documentId = created["data"]["documentId"].get<std::string>();
    REQUIRE(server->Call("add_sheet", nlohmann::json{{"documentId", documentId}, {"name", "Report"}})["ok"] == true);
    CHECK(server->Call("rename_sheet",
                       nlohmann::json{{"documentId", documentId}, {"sheet", 2}, {"new_name", "Analysis"}})["ok"] ==
          true);

    REQUIRE(server->Call(
                "write_range",
                nlohmann::json{{"documentId", documentId},
                               {"sheet", 1},
                               {"origin", "A1"},
                               {"values", nlohmann::json::array({nlohmann::json::array({"Region", "Revenue"}),
                                                                 nlohmann::json::array({"North", 1200}),
                                                                 nlohmann::json::array({"South", 900})})}})
                ["ok"] == true);

    // The sheet-naming arguments of add_pivot_table take indices too.
    const auto pivot = server->Call(
        "add_pivot_table",
        nlohmann::json{{"documentId", documentId},
                       {"source_sheet", 1},
                       {"source_range", "A1:B3"},
                       {"target_sheet", 2},
                       {"target_cell", "A1"},
                       {"rows", nlohmann::json::array({"Region"})},
                       {"values", nlohmann::json::array({nlohmann::json{{"field", "Revenue"}, {"aggregate", "sum"}}})}});
    REQUIRE(pivot["ok"] == true);
    CHECK(pivot["data"]["targetCell"] == "A1");

    // A representative sweep of the sheet-scoped tools, addressed by index.
    CHECK(server->Call("format_range",
                       nlohmann::json{{"documentId", documentId}, {"sheet", 1}, {"range", "A1:B1"}, {"font", nlohmann::json{{"bold", true}}}})["ok"] == true);
    CHECK(server->Call("merge_cells",
                       nlohmann::json{{"documentId", documentId}, {"sheet", 1}, {"range", "D1:E1"}})["ok"] == true);
    CHECK(server->Call("set_column_width", nlohmann::json{{"documentId", documentId}, {"sheet", 1}, {"columns", "A:B"}, {"width", 16.0}})["ok"] == true);
    CHECK(server->Call("set_row_height", nlohmann::json{{"documentId", documentId}, {"sheet", 1}, {"rows", "1"}, {"height", 22.0}})["ok"] == true);
    CHECK(server->Call("freeze_panes",
                       nlohmann::json{{"documentId", documentId}, {"sheet", 1}, {"cell", "A2"}})["ok"] == true);
    CHECK(server->Call("set_hyperlink", nlohmann::json{{"documentId", documentId}, {"sheet", 1}, {"cell", "G1"}, {"target", "https://example.com"}})["ok"] == true);
    CHECK(server->Call("add_named_range", nlohmann::json{{"documentId", documentId}, {"sheet", 1}, {"name", "Sales"}, {"range", "A1:B3"}})["ok"] == true);
    CHECK(server->Call("recalculate", nlohmann::json{{"documentId", documentId}, {"sheet", 1}})["ok"] == true);
    CHECK(server->Call("clear_range",
                       nlohmann::json{{"documentId", documentId}, {"sheet", 1}, {"range", "G1"}})["ok"] == true);
}

TEST_CASE("an unreadable color is reported instead of dropped [mcp-excel]")
{
    auto server = MakeExcelServer();
    server->Initialize();

    const auto created = server->Call("create_document", nlohmann::json::object());
    const auto documentId = created["data"]["documentId"].get<std::string>();

    const auto font = server->Call("format_range",
                                   nlohmann::json{{"documentId", documentId},
                                                  {"range", "A1"},
                                                  {"font", nlohmann::json{{"color", "not a color"}}}});
    CHECK(font["ok"] == false);
    CHECK(font["error"]["code"] == "input_invalid");

    const auto fill = server->Call("format_range", nlohmann::json{{"documentId", documentId},
                                                                  {"range", "A1"},
                                                                  {"fill", nlohmann::json{{"color", "#GGGGGG"}}}});
    CHECK(fill["ok"] == false);
    CHECK(fill["error"]["code"] == "input_invalid");

    const auto border = server->Call(
        "format_range", nlohmann::json{{"documentId", documentId},
                                       {"range", "A1"},
                                       {"border", nlohmann::json{{"style", "thin"}, {"color", "blue"}}}});
    CHECK(border["ok"] == false);
    CHECK(border["error"]["code"] == "input_invalid");

    const auto accepted = server->Call(
        "format_range", nlohmann::json{{"documentId", documentId},
                                       {"range", "A1"},
                                       {"font", nlohmann::json{{"bold", true}, {"color", "#1F4E79"}}},
                                       {"fill", nlohmann::json{{"color", "#FFFF00"}}},
                                       {"border", nlohmann::json{{"style", "thin"}, {"color", "#000000"}}}});
    CHECK(accepted["ok"] == true);
}

TEST_CASE("recalculate reports per-cell formula errors [mcp-excel]")
{
    auto server = MakeExcelServer();
    server->Initialize();

    const auto created = server->Call("create_document", nlohmann::json::object());
    const auto documentId = created["data"]["documentId"].get<std::string>();

    REQUIRE(server->Call(
                "write_cells",
                nlohmann::json{{"documentId", documentId},
                               {"cells", nlohmann::json::array(
                                             {nlohmann::json{{"address", "A1"}, {"value", 0}},
                                              nlohmann::json{{"address", "A2"}, {"value", 4}},
                                              nlohmann::json{{"address", "B1"},
                                                             {"value", nlohmann::json{{"formula", "A2/A1"}}}},
                                              nlohmann::json{{"address", "B2"},
                                                             {"value", nlohmann::json{{"formula", "A2+1"}}}}})}})
                ["ok"] == true);

    const auto recalculated = server->Call("recalculate", nlohmann::json{{"documentId", documentId}});
    REQUIRE(recalculated["ok"] == true);
    CHECK(recalculated["data"]["circularReferences"].empty());
    REQUIRE(recalculated["data"]["formulaErrors"].size() == 1);
    CHECK(recalculated["data"]["formulaErrors"][0]["address"] == "B1");
    CHECK(recalculated["data"]["formulaErrors"][0]["formula"] == "A2/A1");
    CHECK(recalculated["data"]["formulaErrors"][0]["error"] == "#DIV/0!");
    CHECK_FALSE(recalculated["data"]["formulaErrors"][0]["sheet"].get<std::string>().empty());

    // Repairing the divisor clears the report.
    REQUIRE(server->Call("write_cells",
                         nlohmann::json{{"documentId", documentId},
                                        {"cells", nlohmann::json::array(
                                                      {nlohmann::json{{"address", "A1"}, {"value", 2}}})}})
                ["ok"] == true);

    const auto repaired = server->Call("recalculate", nlohmann::json{{"documentId", documentId}});
    REQUIRE(repaired["ok"] == true);
    CHECK(repaired["data"]["formulaErrors"].empty());
}

TEST_CASE("a multi-column data range becomes one chart series per column [mcp-excel]")
{
    auto server = MakeExcelServer();
    server->Initialize();

    const auto created = server->Call("create_document", nlohmann::json{{"path", "series.xlsx"}});
    const auto documentId = created["data"]["documentId"].get<std::string>();

    REQUIRE(server->Call("write_range",
                         nlohmann::json{{"documentId", documentId},
                                        {"origin", "A1"},
                                        {"values", nlohmann::json::array({nlohmann::json::array({10, 1}),
                                                                          nlohmann::json::array({20, 2}),
                                                                          nlohmann::json::array({30, 3})})}})
                ["ok"] == true);

    const auto columns = server->Call("add_chart", nlohmann::json{{"documentId", documentId},
                                                                  {"type", "column"},
                                                                  {"data_range", "A1:B3"},
                                                                  {"anchor_cell", "D1"},
                                                                  {"series_names", nlohmann::json::array({"Left"})}});
    REQUIRE(columns["ok"] == true);
    CHECK(columns["data"]["seriesCount"] == 2);
    CHECK(columns["warnings"].empty());

    const auto rows = server->Call("add_chart", nlohmann::json{{"documentId", documentId},
                                                               {"type", "line"},
                                                               {"data_range", "A1:B3"},
                                                               {"series_in", "rows"},
                                                               {"anchor_cell", "D20"}});
    REQUIRE(rows["ok"] == true);
    CHECK(rows["data"]["seriesCount"] == 3);

    // A pie chart plots one series and says so rather than misleading the agent.
    const auto pie = server->Call("add_chart", nlohmann::json{{"documentId", documentId},
                                                              {"type", "pie"},
                                                              {"data_range", "A1:B3"},
                                                              {"anchor_cell", "D40"}});
    REQUIRE(pie["ok"] == true);
    CHECK(pie["data"]["seriesCount"] == 1);
    REQUIRE(pie["warnings"].size() == 1);
    CHECK(pie["warnings"][0]["code"] == "chart_series_dropped");

    // A single-column range keeps the one-series behavior.
    const auto single = server->Call("add_chart", nlohmann::json{{"documentId", documentId},
                                                                 {"type", "column"},
                                                                 {"data_range", "A1:A3"},
                                                                 {"anchor_cell", "D60"},
                                                                 {"title", "Revenue"}});
    REQUIRE(single["ok"] == true);
    CHECK(single["data"]["seriesCount"] == 1);

    REQUIRE(server->Call("save_document", nlohmann::json{{"documentId", documentId}})["ok"] == true);

    auto editor = ExyokiOffice::Excel::ExcelDocumentEditor::Open(server->Path("series.xlsx"));
    REQUIRE(editor != nullptr);
    auto sheet = editor->FirstWorksheet();
    REQUIRE(sheet != nullptr);
    CHECK(sheet->Charts().size() == 4);
}
