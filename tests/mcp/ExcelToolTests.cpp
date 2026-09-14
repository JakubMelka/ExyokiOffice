// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "McpTestSupport.hpp"

#include <fstream>
#include <filesystem>
#include <iterator>

#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/Office2019/Excel/ThreadedComments.hpp"
#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/Spreadsheet.hpp"
#include "ExyokiOffice/Excel/ExcelDocument.hpp"
#include "ExyokiOffice/Packaging/GeneratedParts.hpp"
#include "ExyokiOffice/Tools/ValidationRunner.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

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
    // The cached result is typed like a plain cell would be.
    CHECK(cells["data"]["cells"][0]["value"] == 5);
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

TEST_CASE("a chart on a summary sheet plots another sheet as columns with a line on a secondary axis [mcp-excel]")
{
    auto server = MakeExcelServer();
    server->Initialize();

    const auto created = server->Call("create_document", nlohmann::json{{"path", "combo.xlsx"}});
    const auto documentId = created["data"]["documentId"].get<std::string>();
    REQUIRE(server->Call("add_sheet", nlohmann::json{{"documentId", documentId}, {"name", "My Data"}})["ok"] == true);
    REQUIRE(server->Call("write_range",
                         nlohmann::json{{"documentId", documentId},
                                        {"sheet", "My Data"},
                                        {"origin", "A1"},
                                        {"values", nlohmann::json::array({nlohmann::json::array({"Q1", 100, 0.1, 4}),
                                                                          nlohmann::json::array({"Q2", 150, 0.2, 6}),
                                                                          nlohmann::json::array({"Q3", 120, 0.15, 5})})}})
                ["ok"] == true);

    const auto chart = [&](nlohmann::json arguments)
    {
        arguments["documentId"] = documentId;
        arguments["sheet"] = 1;
        if (!arguments.contains("anchor_cell"))
        {
            arguments["anchor_cell"] = "A1";
        }
        return server->Call("add_chart", arguments);
    };

    const auto combination =
        chart(nlohmann::json{{"type", "column"},
                             {"data_range", "'My Data'!B1:C3"},
                             {"categories_range", "'My Data'!A1:A3"},
                             {"series_names", nlohmann::json::array({"Revenue", "Margin"})},
                             {"series_options", nlohmann::json::array({nlohmann::json::object(),
                                                                       nlohmann::json{{"type", "line"},
                                                                                      {"secondary_axis", true}}})},
                             {"value_axis_title", "USD"},
                             {"secondary_axis_title", "Margin"},
                             {"legend", "bottom"}});
    REQUIRE(combination["ok"] == true);
    CHECK(combination["data"]["seriesCount"] == 2);
    CHECK(combination["data"]["sourceSheet"] == "My Data");

    const auto bubbles = chart(nlohmann::json{{"type", "bubble"},
                                              {"data_range", "'My Data'!B1:B3"},
                                              {"categories_range", "'My Data'!C1:C3"},
                                              {"sizes_range", "'My Data'!D1:D3"},
                                              {"anchor_cell", "A30"}});
    REQUIRE(bubbles["ok"] == true);

    // Every refusal says what is wrong and adds nothing.
    const auto expectError = [&](const nlohmann::json& arguments, const std::string& code)
    {
        const auto result = chart(arguments);
        INFO(arguments.dump());
        CHECK(result["ok"] == false);
        CHECK(result["error"]["code"] == code);
        CHECK_FALSE(result["error"]["hint"].get<std::string>().empty());
    };
    expectError(nlohmann::json{{"type", "column"}, {"data_range", "Missing!B1:B3"}}, "sheet_not_found");
    expectError(nlohmann::json{{"type", "column"}, {"data_range", "'My Data'!B1:nonsense"}}, "range_invalid");
    expectError(nlohmann::json{{"type", "column"}, {"data_range", "'My Data'!B1:B3"}, {"categories_range", "A1:A3"}},
                "input_invalid");
    expectError(nlohmann::json{{"type", "pie"},
                               {"data_range", "'My Data'!B1:B3"},
                               {"series_options", nlohmann::json::array({nlohmann::json{{"type", "column"}}})}},
                "input_invalid");
    expectError(nlohmann::json{{"type", "bar"},
                               {"data_range", "'My Data'!B1:C3"},
                               {"series_options", nlohmann::json::array({nlohmann::json::object(),
                                                                         nlohmann::json{{"type", "line"}}})}},
                "input_invalid");
    expectError(nlohmann::json{{"type", "column"},
                               {"data_range", "'My Data'!B1:B3"},
                               {"series_options", nlohmann::json::array({nlohmann::json{{"secondary_axis", true}}})}},
                "input_invalid");
    expectError(nlohmann::json{{"type", "column"},
                               {"data_range", "'My Data'!B1:B3"},
                               {"series_options", nlohmann::json::array({nlohmann::json::object(),
                                                                         nlohmann::json::object()})}},
                "input_invalid");
    expectError(nlohmann::json{{"type", "bubble"}, {"data_range", "'My Data'!B1:B3"}}, "input_invalid");
    expectError(nlohmann::json{{"type", "column"}, {"data_range", "'My Data'!B1:B3"}, {"sizes_range", "'My Data'!D1:D3"}},
                "input_invalid");
    expectError(nlohmann::json{{"type", "bubble"},
                               {"data_range", "'My Data'!B1:C3"},
                               {"sizes_range", "'My Data'!D1:D3"}},
                "input_invalid");
    expectError(nlohmann::json{{"type", "column"},
                               {"data_range", "'My Data'!B1:B3"},
                               {"series_options", nlohmann::json::array({nlohmann::json{{"type", "radar"}}})}},
                "input_invalid");
    expectError(nlohmann::json{{"type", "column"}, {"data_range", "'My Data'!B1:B3"}, {"legend", "middle"}},
                "input_invalid");

    REQUIRE(server->Call("save_document", nlohmann::json{{"documentId", documentId}})["ok"] == true);
    const auto report = ExyokiOffice::Tools::Run(server->Path("combo.xlsx"));
    CHECK(report.Loaded);
    CHECK_MESSAGE(report.ErrorCount == 0, DescribeValidationErrors(report));

    auto editor = ExyokiOffice::Excel::ExcelDocumentEditor::Open(server->Path("combo.xlsx"));
    REQUIRE(editor != nullptr);
    const auto charts = editor->FirstWorksheet()->Charts();
    REQUIRE(charts.size() == 2);
    REQUIRE(charts[0].Series.size() == 2);
    CHECK(charts[0].Series[0].SourceSheet == std::optional<std::string>("My Data"));
    REQUIRE(charts[0].Series[1].Type.has_value());
    CHECK(*charts[0].Series[1].Type == ExyokiOffice::Excel::ExcelChartType::Line);
    CHECK(charts[0].Series[1].SecondaryAxis);
    CHECK(charts[1].Type == ExyokiOffice::Excel::ExcelChartType::Bubble);
    REQUIRE(charts[1].Series.size() == 1);
    CHECK(charts[1].Series[0].BubbleSizes.has_value());
}

TEST_CASE("a table's header row carries its column names, as Excel requires [mcp-excel]")
{
    auto server = MakeExcelServer();
    server->Initialize();

    const auto created = server->Call("create_document", nlohmann::json{{"path", "headers.xlsx"}});
    const auto documentId = created["data"]["documentId"].get<std::string>();

    // An empty range: Excel would not open the workbook if the header cells
    // stayed empty while the columns are called Column1 and Column2.
    const auto empty = server->Call(
        "add_table", nlohmann::json{{"documentId", documentId}, {"range", "A1:B4"}, {"name", "Empty"}});
    REQUIRE(empty["ok"] == true);
    CHECK(empty["data"]["columns"] == nlohmann::json::array({"Column1", "Column2"}));
    CHECK(empty["warnings"].empty());

    // Without a header row the first row is replaced, and the caller is told.
    REQUIRE(server->Call("write_range", nlohmann::json{{"documentId", documentId},
                                                       {"origin", "D1"},
                                                       {"values", nlohmann::json::array({nlohmann::json::array({1, 2}),
                                                                                         nlohmann::json::array({3, 4})})}})
                ["ok"] == true);
    const auto numbers = server->Call("add_table", nlohmann::json{{"documentId", documentId},
                                                                  {"range", "D1:E2"},
                                                                  {"name", "Numbers"},
                                                                  {"header_row", false}});
    REQUIRE(numbers["ok"] == true);
    REQUIRE(numbers["warnings"].size() == 1);
    CHECK(numbers["warnings"][0]["code"] == "table_header_rewritten");
    CHECK(numbers["warnings"][0]["message"].get<std::string>().find("D1, E1") != std::string::npos);

    REQUIRE(server->Call("save_document", nlohmann::json{{"documentId", documentId}})["ok"] == true);
    auto editor = ExyokiOffice::Excel::ExcelDocumentEditor::Open(server->Path("headers.xlsx"));
    REQUIRE(editor != nullptr);
    const auto sheet = editor->FirstWorksheet();
    ExyokiOffice::Excel::SharedStringTableService strings(editor->GetDocument());
    for (const auto& [cell, expected] : std::vector<std::pair<std::string, std::string>>{
             {"A1", "Column1"}, {"B1", "Column2"}, {"D1", "Column1"}, {"E1", "Column2"}})
    {
        INFO(cell);
        const auto value = sheet->GetCellValue(*ExyokiOffice::Excel::CellAddress::ParseA1(cell));
        REQUIRE(value.has_value());
        REQUIRE(value->SharedStringIndex().has_value());
        CHECK(strings.Lookup(*value->SharedStringIndex()) == std::optional<std::string>(expected));
    }
}

TEST_CASE("a table refuses merged cells and merged cells refuse a table [mcp-excel]")
{
    auto server = MakeExcelServer();
    server->Initialize();

    const auto created = server->Call("create_document", nlohmann::json{{"path", "merged.xlsx"}});
    const auto documentId = created["data"]["documentId"].get<std::string>();

    REQUIRE(server->Call("merge_cells", nlohmann::json{{"documentId", documentId}, {"range", "A1:C1"}})["ok"] == true);
    const auto overMerge = server->Call(
        "add_table", nlohmann::json{{"documentId", documentId}, {"range", "A1:C10"}, {"name", "Sales"}});
    CHECK(overMerge["ok"] == false);
    CHECK(overMerge["error"]["code"] == "input_invalid");
    CHECK(overMerge["error"]["target"] == "A1:C1");
    CHECK(overMerge["error"]["hint"].get<std::string>().find("unmerge") != std::string::npos);

    REQUIRE(server->Call("add_table", nlohmann::json{{"documentId", documentId}, {"range", "E1:F5"}, {"name", "Side"}})
                ["ok"] == true);
    const auto intoTable =
        server->Call("merge_cells", nlohmann::json{{"documentId", documentId}, {"range", "F4:G4"}});
    CHECK(intoTable["ok"] == false);
    CHECK(intoTable["error"]["code"] == "input_invalid");
    CHECK(intoTable["error"]["message"].get<std::string>().find("Side") != std::string::npos);

    // Unmerging is always allowed, and afterwards the table fits.
    REQUIRE(server->Call("merge_cells", nlohmann::json{{"documentId", documentId}, {"range", "A1:C1"}, {"unmerge", true}})
                ["ok"] == true);
    CHECK(server->Call("add_table", nlohmann::json{{"documentId", documentId}, {"range", "A1:C10"}, {"name", "Sales"}})
              ["ok"] == true);
}

TEST_CASE("the print setup is written and reaches the saved worksheet [mcp-excel]")
{
    auto server = MakeExcelServer();
    server->Initialize();

    const auto created = server->Call("create_document", nlohmann::json{{"path", "printing.xlsx"}});
    const auto documentId = created["data"]["documentId"].get<std::string>();

    const auto applied = server->Call(
        "set_print_setup",
        nlohmann::json{{"documentId", documentId},
                       {"orientation", "landscape"},
                       {"paper_size", "a4"},
                       {"fit_to_width", 1},
                       {"fit_to_height", 0},
                       {"margins", nlohmann::json{{"left", "1cm"}, {"right", "1cm"}, {"header", "0.5cm"}}},
                       {"print_area", nlohmann::json::array({"A1:D20"})},
                       {"repeat_rows", "1:2"},
                       {"repeat_columns", "A"},
                       {"header_footer", nlohmann::json{{"odd_header", "&CQuarterly report"},
                                                        {"odd_footer", "&RPage &P of &N"}}},
                       {"options", nlohmann::json{{"grid_lines", true}, {"horizontal_centered", true}}}});
    REQUIRE(applied["ok"] == true);
    CHECK(applied["data"]["orientation"] == "landscape");
    CHECK(applied["data"]["paperSize"] == "a4");
    REQUIRE(applied["data"]["printArea"].size() == 1);
    CHECK(applied["data"]["printArea"][0] == "A1:D20");

    // Omitted members keep what the previous call set, which is what makes the
    // tool usable as a series of small adjustments.
    const auto adjusted =
        server->Call("set_print_setup", nlohmann::json{{"documentId", documentId}, {"scale", 80}});
    REQUIRE(adjusted["ok"] == true);
    CHECK(adjusted["data"]["orientation"] == "landscape");
    CHECK(adjusted["data"]["paperSize"] == "a4");

    REQUIRE(server->Call("save_document", nlohmann::json{{"documentId", documentId}})["ok"] == true);

    auto editor = ExyokiOffice::Excel::ExcelDocumentEditor::Open(server->Path("printing.xlsx"));
    REQUIRE(editor != nullptr);
    auto sheet = editor->FirstWorksheet();
    REQUIRE(sheet != nullptr);

    const auto setup = sheet->GetPageSetup();
    CHECK(setup.Orientation == ExyokiOffice::Excel::PageOrientation::Landscape);
    REQUIRE(setup.PaperSize.has_value());
    CHECK(*setup.PaperSize == ExyokiOffice::Excel::PaperSize::A4);
    REQUIRE(setup.FitToWidth.has_value());
    CHECK(*setup.FitToWidth == 1);

    const auto titles = sheet->GetPrintTitles();
    REQUIRE(titles.Rows.has_value());
    CHECK(titles.Rows->first == 1);
    CHECK(titles.Rows->second == 2);
    REQUIRE(titles.Columns.has_value());
    CHECK(titles.Columns->first == 1);
    CHECK(titles.Columns->second == 1);

    CHECK(sheet->GetPrintArea().size() == 1);
    CHECK(sheet->GetHeaderFooter().OddHeader == "&CQuarterly report");
    CHECK(sheet->GetPrintOptions().GridLines);

    const auto report = ExyokiOffice::Tools::Run(server->Path("printing.xlsx"));
    CHECK_MESSAGE(report.ErrorCount == 0, DescribeValidationErrors(report));
}

TEST_CASE("set_print_setup refuses a setting it cannot write [mcp-excel]")
{
    auto server = MakeExcelServer();
    server->Initialize();

    const auto created = server->Call("create_document", nlohmann::json::object());
    const auto documentId = created["data"]["documentId"].get<std::string>();

    const auto badRows =
        server->Call("set_print_setup", nlohmann::json{{"documentId", documentId}, {"repeat_rows", "top:two"}});
    CHECK(badRows["ok"] == false);
    CHECK(badRows["error"]["code"] == "range_invalid");

    const auto badColumns =
        server->Call("set_print_setup", nlohmann::json{{"documentId", documentId}, {"repeat_columns", "1:2"}});
    CHECK(badColumns["ok"] == false);
    CHECK(badColumns["error"]["code"] == "range_invalid");

    const auto badArea = server->Call(
        "set_print_setup",
        nlohmann::json{{"documentId", documentId}, {"print_area", nlohmann::json::array({"A1:D20", "nonsense"})}});
    CHECK(badArea["ok"] == false);
    CHECK(badArea["error"]["code"] == "range_invalid");

    const auto negative = server->Call(
        "set_print_setup",
        nlohmann::json{{"documentId", documentId}, {"margins", nlohmann::json{{"left", "-2cm"}}}});
    CHECK(negative["ok"] == false);
    CHECK(negative["error"]["code"] == "input_invalid");

    const auto notALength = server->Call(
        "set_print_setup",
        nlohmann::json{{"documentId", documentId}, {"margins", nlohmann::json{{"top", "wide"}}}});
    CHECK(notALength["ok"] == false);
    CHECK(notALength["error"]["code"] == "input_invalid");

    // The schema bounds the scale, so an out-of-range value never reaches the
    // handler at all.
    const auto badScale =
        server->Call("set_print_setup", nlohmann::json{{"documentId", documentId}, {"scale", 5}});
    CHECK(badScale["ok"] == false);
    CHECK(badScale["error"]["code"] == "input_invalid");

    const auto missingSheet = server->Call(
        "set_print_setup", nlohmann::json{{"documentId", documentId}, {"sheet", "Nowhere"}, {"scale", 90}});
    CHECK(missingSheet["ok"] == false);
    CHECK(missingSheet["error"]["code"] == "sheet_not_found");

    // Nothing above may have been written, so the document is still clean.
    const auto documents = server->Call("list_documents", nlohmann::json::object());
    REQUIRE(documents["ok"] == true);
    REQUIRE(documents["data"]["documents"].size() == 1);
    CHECK(documents["data"]["documents"][0]["dirty"] == false);
}

TEST_CASE("an image is anchored on the worksheet [mcp-excel]")
{
    auto server = MakeExcelServer();
    server->Initialize();

    const auto created = server->Call("create_document", nlohmann::json{{"path", "picture.xlsx"}});
    const auto documentId = created["data"]["documentId"].get<std::string>();

    const std::string png =
        "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAEhQGAhKmMIQAAAABJRU5ErkJggg==";

    const auto placed = server->Call("add_image", nlohmann::json{{"documentId", documentId},
                                                                 {"anchor_cell", "D2"},
                                                                 {"dataBase64", png},
                                                                 {"alt", "A dot."},
                                                                 {"width", "4cm"},
                                                                 {"height", "3cm"}});
    REQUIRE(placed["ok"] == true);
    CHECK(placed["data"]["contentType"] == "image/png");
    CHECK(placed["data"]["anchor"] == "D2");

    const auto broken = server->Call("add_image", nlohmann::json{{"documentId", documentId},
                                                                 {"anchor_cell", "D2"},
                                                                 {"dataBase64", "not base64 at all!!"}});
    CHECK(broken["ok"] == false);

    // A payload SpreadsheetML has no enumeration value for is refused rather
    // than stored under a format it is not.
    const auto unsupported = server->Call("add_image", nlohmann::json{{"documentId", documentId},
                                                                      {"anchor_cell", "D2"},
                                                                      {"dataBase64", png},
                                                                      {"contentType", "image/svg+xml"}});
    CHECK(unsupported["ok"] == false);
    CHECK(unsupported["error"]["code"] == "unsupported");

    const auto badAnchor = server->Call("add_image", nlohmann::json{{"documentId", documentId},
                                                                    {"anchor_cell", "not-a-cell"},
                                                                    {"dataBase64", png}});
    CHECK(badAnchor["ok"] == false);
    CHECK(badAnchor["error"]["code"] == "range_invalid");

    const auto missingSheet = server->Call("add_image", nlohmann::json{{"documentId", documentId},
                                                                       {"sheet", "Nowhere"},
                                                                       {"anchor_cell", "D2"},
                                                                       {"dataBase64", png}});
    CHECK(missingSheet["ok"] == false);
    CHECK(missingSheet["error"]["code"] == "sheet_not_found");

    REQUIRE(server->Call("save_document", nlohmann::json{{"documentId", documentId}})["ok"] == true);

    auto editor = ExyokiOffice::Excel::ExcelDocumentEditor::Open(server->Path("picture.xlsx"));
    REQUIRE(editor != nullptr);
    auto sheet = editor->FirstWorksheet();
    REQUIRE(sheet != nullptr);
    // Only the one accepted call may have reached the worksheet.
    REQUIRE(sheet->Images().size() == 1);
    CHECK(sheet->Images()[0].Description == "A dot.");

    const auto report = ExyokiOffice::Tools::Run(server->Path("picture.xlsx"));
    CHECK_MESSAGE(report.ErrorCount == 0, DescribeValidationErrors(report));
}

TEST_CASE("cell comments survive a round trip in both models [mcp-excel]")
{
    auto server = MakeExcelServer();
    server->Initialize();

    const auto created = server->Call("create_document", nlohmann::json{{"path", "comments.xlsx"}});
    const auto documentId = created["data"]["documentId"].get<std::string>();

    const auto threaded = server->Call("add_comment", nlohmann::json{{"documentId", documentId},
                                                                     {"cell", "B4"},
                                                                     {"text", "Check this number."},
                                                                     {"author", "Jakub"},
                                                                     {"threaded", true}});
    REQUIRE(threaded["ok"] == true);
    CHECK(threaded["data"]["threaded"] == true);
    CHECK(threaded["data"]["cell"] == "B4");
    // A threaded entry is addressed by its identifier afterwards, so the tool
    // has to hand one back; a plain note has nothing to hand back.
    CHECK_FALSE(threaded["data"]["commentId"].get<std::string>().empty());

    // The default is the threaded model, so the older plain note has to be
    // asked for by name.
    const auto plain = server->Call("add_comment", nlohmann::json{{"documentId", documentId},
                                                                  {"cell", "C5"},
                                                                  {"text", "A plain note."},
                                                                  {"threaded", false}});
    REQUIRE(plain["ok"] == true);
    CHECK(plain["data"]["threaded"] == false);
    CHECK(plain["data"]["commentId"] == "");

    const auto listed = server->Call("list_comments", nlohmann::json{{"documentId", documentId}});
    REQUIRE(listed["ok"] == true);
    REQUIRE(listed["data"]["comments"].size() == 2);

    bool sawPlain = false;
    bool sawThreaded = false;
    for (const auto& comment : listed["data"]["comments"])
    {
        if (comment["threaded"] == true)
        {
            sawThreaded = true;
            CHECK(comment["cell"] == "B4");
            CHECK(comment["author"] == "Jakub");
            CHECK(comment["text"] == "Check this number.");
        }
        else
        {
            sawPlain = true;
            CHECK(comment["cell"] == "C5");
            CHECK(comment["text"] == "A plain note.");
        }
    }

    CHECK(sawThreaded);
    CHECK(sawPlain);

    REQUIRE(server->Call("save_document", nlohmann::json{{"documentId", documentId}})["ok"] == true);

    auto editor = ExyokiOffice::Excel::ExcelDocumentEditor::Open(server->Path("comments.xlsx"));
    REQUIRE(editor != nullptr);
    auto sheet = editor->FirstWorksheet();
    REQUIRE(sheet != nullptr);
    // The note backing the threaded comment is not reported as a note of its own.
    REQUIRE(sheet->Comments().size() == 1);
    CHECK(sheet->Comments()[0].Text == "A plain note.");
    REQUIRE(sheet->ThreadedComments().size() == 1);
    CHECK(sheet->ThreadedComments()[0].Text == "Check this number.");

    const auto report = ExyokiOffice::Tools::Run(server->Path("comments.xlsx"));
    CHECK_MESSAGE(report.ErrorCount == 0, DescribeValidationErrors(report));
}

TEST_CASE("a threaded reply names the entry it answers [mcp-excel]")
{
    auto server = MakeExcelServer();
    server->Initialize();

    const auto created = server->Call("create_document", nlohmann::json::object());
    const auto documentId = created["data"]["documentId"].get<std::string>();

    const auto root = server->Call(
        "add_comment",
        nlohmann::json{{"documentId", documentId}, {"cell", "A1"}, {"text", "Why?"}, {"threaded", true}});
    REQUIRE(root["ok"] == true);
    const auto rootId = root["data"]["commentId"].get<std::string>();

    const auto reply = server->Call("add_comment", nlohmann::json{{"documentId", documentId},
                                                                  {"cell", "A1"},
                                                                  {"text", "Because."},
                                                                  {"reply_to", rootId}});
    REQUIRE(reply["ok"] == true);
    // reply_to implies a threaded comment even though `threaded` was not given:
    // a plain note has no parent to point at.
    CHECK(reply["data"]["threaded"] == true);

    const auto listed = server->Call("list_comments", nlohmann::json{{"documentId", documentId}});
    REQUIRE(listed["ok"] == true);
    REQUIRE(listed["data"]["comments"].size() == 2);

    ExyokiOffice::Size replies = 0;
    for (const auto& comment : listed["data"]["comments"])
    {
        if (comment["parentId"] == rootId)
        {
            ++replies;
            CHECK(comment["text"] == "Because.");
        }
    }

    CHECK(replies == 1);

    // Removing the reply leaves the root, so the identifier really addresses
    // one entry rather than the whole thread.
    const auto replyId = server->Call("list_comments", nlohmann::json{{"documentId", documentId}});
    std::string toRemove;
    for (const auto& comment : replyId["data"]["comments"])
    {
        if (comment["parentId"] == rootId)
        {
            toRemove = comment["id"].get<std::string>();
        }
    }

    REQUIRE_FALSE(toRemove.empty());
    const auto removed =
        server->Call("delete_comment", nlohmann::json{{"documentId", documentId}, {"comment_id", toRemove}});
    REQUIRE(removed["ok"] == true);
    CHECK(removed["data"]["removed"] == true);

    const auto remaining = server->Call("list_comments", nlohmann::json{{"documentId", documentId}});
    CHECK(remaining["data"]["comments"].size() == 1);
}

TEST_CASE("the comment tools refuse input that names nothing [mcp-excel]")
{
    auto server = MakeExcelServer();
    server->Initialize();

    const auto created = server->Call("create_document", nlohmann::json::object());
    const auto documentId = created["data"]["documentId"].get<std::string>();

    // A comment without text would be written out as an empty annotation, which
    // no reader can distinguish from a defect.
    const auto empty =
        server->Call("add_comment", nlohmann::json{{"documentId", documentId}, {"cell", "A1"}, {"text", ""}});
    CHECK(empty["ok"] == false);
    CHECK(empty["error"]["code"] == "input_invalid");

    const auto malformed = server->Call(
        "add_comment", nlohmann::json{{"documentId", documentId}, {"cell", "A0"}, {"text", "Hello."}});
    CHECK(malformed["ok"] == false);
    CHECK(malformed["error"]["code"] == "range_invalid");

    const auto missingSheet = server->Call("add_comment", nlohmann::json{{"documentId", documentId},
                                                                         {"sheet", "Nowhere"},
                                                                         {"cell", "A1"},
                                                                         {"text", "Hello."}});
    CHECK(missingSheet["ok"] == false);
    CHECK(missingSheet["error"]["code"] == "sheet_not_found");

    const auto orphan = server->Call("add_comment", nlohmann::json{{"documentId", documentId},
                                                                   {"cell", "A1"},
                                                                   {"text", "Answer."},
                                                                   {"reply_to", "no-such-entry"}});
    CHECK(orphan["ok"] == false);
    CHECK(orphan["error"]["code"] == "comment_not_found");

    // delete_comment takes one of two addressing modes, and both the empty and
    // the doubled form have to be refused rather than silently resolved.
    const auto neither = server->Call("delete_comment", nlohmann::json{{"documentId", documentId}});
    CHECK(neither["ok"] == false);
    CHECK(neither["error"]["code"] == "input_invalid");

    const auto both = server->Call("delete_comment", nlohmann::json{{"documentId", documentId},
                                                                    {"cell", "A1"},
                                                                    {"comment_id", "something"}});
    CHECK(both["ok"] == false);
    CHECK(both["error"]["code"] == "input_invalid");

    const auto unknownId =
        server->Call("delete_comment", nlohmann::json{{"documentId", documentId}, {"comment_id", "no-such-entry"}});
    CHECK(unknownId["ok"] == false);
    CHECK(unknownId["error"]["code"] == "comment_not_found");

    const auto bareCell =
        server->Call("delete_comment", nlohmann::json{{"documentId", documentId}, {"cell", "Z9"}});
    CHECK(bareCell["ok"] == false);
    CHECK(bareCell["error"]["code"] == "comment_not_found");

    // A failed delete must not count as a mutation; the document is still clean.
    const auto documents = server->Call("list_documents", nlohmann::json::object());
    REQUIRE(documents["ok"] == true);
    REQUIRE(documents["data"]["documents"].size() == 1);
    CHECK(documents["data"]["documents"][0]["dirty"] == false);

    const auto listedElsewhere =
        server->Call("list_comments", nlohmann::json{{"documentId", documentId}, {"sheet", "Nowhere"}});
    CHECK(listedElsewhere["ok"] == false);
    CHECK(listedElsewhere["error"]["code"] == "sheet_not_found");
}

TEST_CASE("worksheets are moved and copied, within a workbook and across two [mcp-excel]")
{
    auto server = MakeExcelServer();
    server->Initialize();

    // A separate workbook to copy from, written through the server so the
    // cross-workbook path goes through the workspace like any other file.
    const auto sourceDoc = server->Call("create_document", nlohmann::json{{"path", "source.xlsx"}});
    const auto sourceId = sourceDoc["data"]["documentId"].get<std::string>();
    REQUIRE(server->Call("rename_sheet", nlohmann::json{{"documentId", sourceId},
                                                        {"sheet", 1},
                                                        {"new_name", "Imported"}})["ok"] == true);
    REQUIRE(server->Call("write_cells",
                         nlohmann::json{{"documentId", sourceId},
                                        {"cells", nlohmann::json::array({nlohmann::json{
                                            {"address", "A1"}, {"value", "from elsewhere"}}})}})["ok"] == true);
    REQUIRE(server->Call("save_document", nlohmann::json{{"documentId", sourceId}})["ok"] == true);
    REQUIRE(server->Call("close_document", nlohmann::json{{"documentId", sourceId}})["ok"] == true);

    const auto created = server->Call("create_document", nlohmann::json{{"path", "book.xlsx"}});
    const auto documentId = created["data"]["documentId"].get<std::string>();
    REQUIRE(server->Call("add_sheet", nlohmann::json{{"documentId", documentId}, {"name", "Data"}})["ok"] == true);
    REQUIRE(server->Call("add_sheet", nlohmann::json{{"documentId", documentId}, {"name", "Notes"}})["ok"] == true);

    const auto moved = server->Call(
        "move_sheet", nlohmann::json{{"documentId", documentId}, {"sheet", "Notes"}, {"to_index", 1}});
    REQUIRE(moved["ok"] == true);
    CHECK(moved["data"]["name"] == "Notes");
    CHECK(moved["data"]["index"] == 1);

    const auto order = server->Call("list_sheets", nlohmann::json{{"documentId", documentId}});
    REQUIRE(order["data"]["sheets"].size() == 3);
    CHECK(order["data"]["sheets"][0]["name"] == "Notes");

    const auto copied = server->Call(
        "copy_sheet",
        nlohmann::json{{"documentId", documentId}, {"sheet", "Data"}, {"name", "Data backup"}});
    REQUIRE(copied["ok"] == true);
    CHECK(copied["data"]["name"] == "Data backup");
    CHECK(copied["data"]["index"] == 4);

    const auto imported = server->Call("copy_sheet", nlohmann::json{{"documentId", documentId},
                                                                    {"source_path", "source.xlsx"},
                                                                    {"sheet", "Imported"},
                                                                    {"name", "From source"}});
    REQUIRE(imported["ok"] == true);
    CHECK(imported["data"]["index"] == 5);

    const auto value = server->Call("read_range", nlohmann::json{{"documentId", documentId},
                                                                 {"sheet", "From source"},
                                                                 {"range", "A1:A1"}});
    REQUIRE(value["ok"] == true);
    CHECK(value["data"]["values"][0][0] == "from elsewhere");

    REQUIRE(server->Call("save_document", nlohmann::json{{"documentId", documentId}})["ok"] == true);

    const auto report = ExyokiOffice::Tools::Run(server->Path("book.xlsx"));
    CHECK_MESSAGE(report.ErrorCount == 0, DescribeValidationErrors(report));
}

TEST_CASE("a range is copied and moved on the sheet [mcp-excel]")
{
    auto server = MakeExcelServer();
    server->Initialize();

    const auto created = server->Call("create_document", nlohmann::json{{"path", "ranges.xlsx"}});
    const auto documentId = created["data"]["documentId"].get<std::string>();

    REQUIRE(server->Call("write_range",
                         nlohmann::json{{"documentId", documentId},
                                        {"origin", "A1"},
                                        {"values", nlohmann::json::array({nlohmann::json::array({"a", 1}),
                                                                          nlohmann::json::array({"b", 2})})}})
                ["ok"] == true);

    const auto copied = server->Call(
        "copy_range", nlohmann::json{{"documentId", documentId}, {"source", "A1:B2"}, {"destination", "D1"}});
    REQUIRE(copied["ok"] == true);
    CHECK(copied["data"]["destination"] == "D1:E2");
    CHECK(copied["data"]["moved"] == false);

    const auto afterCopy = server->Call(
        "read_range", nlohmann::json{{"documentId", documentId}, {"range", "A1:E2"}});
    CHECK(afterCopy["data"]["values"][0][0] == "a");
    CHECK(afterCopy["data"]["values"][0][3] == "a");

    const auto moved = server->Call("copy_range", nlohmann::json{{"documentId", documentId},
                                                                 {"source", "D1:E2"},
                                                                 {"destination", "G1"},
                                                                 {"move", true}});
    REQUIRE(moved["ok"] == true);
    CHECK(moved["data"]["moved"] == true);

    // Moving clears the source, so what stood at D1 is gone.
    const auto afterMove = server->Call(
        "read_range", nlohmann::json{{"documentId", documentId}, {"range", "D1:H2"}});
    CHECK(afterMove["data"]["values"][0][0].is_null());
    CHECK(afterMove["data"]["values"][0][3] == "a");

    const auto badSource = server->Call(
        "copy_range", nlohmann::json{{"documentId", documentId}, {"source", "nonsense"}, {"destination", "A9"}});
    CHECK(badSource["ok"] == false);
    CHECK(badSource["error"]["code"] == "range_invalid");

    const auto badDestination = server->Call(
        "copy_range", nlohmann::json{{"documentId", documentId}, {"source", "A1:B2"}, {"destination", "A0"}});
    CHECK(badDestination["ok"] == false);
    CHECK(badDestination["error"]["code"] == "range_invalid");

    // Overlap is supported rather than refused: the source is snapshotted
    // before anything is written, so the result is well defined.
    const auto overlapping = server->Call(
        "copy_range", nlohmann::json{{"documentId", documentId}, {"source", "A1:B2"}, {"destination", "B2"}});
    REQUIRE(overlapping["ok"] == true);
    const auto afterOverlap = server->Call(
        "read_range", nlohmann::json{{"documentId", documentId}, {"range", "B2:C3"}});
    CHECK(afterOverlap["data"]["values"][0][0] == "a");
}

TEST_CASE("sheet and workbook protection are applied and lifted [mcp-excel]")
{
    auto server = MakeExcelServer();
    server->Initialize();

    const auto created = server->Call("create_document", nlohmann::json{{"path", "locked.xlsx"}});
    const auto documentId = created["data"]["documentId"].get<std::string>();

    const auto sheetProtected = server->Call(
        "set_protection",
        nlohmann::json{{"documentId", documentId},
                       {"scope", "sheet"},
                       {"password", "secret"},
                       {"allow", nlohmann::json{{"sort", true}, {"format_cells", true}}}});
    REQUIRE(sheetProtected["ok"] == true);
    CHECK(sheetProtected["data"]["protected"] == true);
    CHECK(sheetProtected["data"]["scope"] == "sheet");

    const auto workbookProtected = server->Call(
        "set_protection",
        nlohmann::json{{"documentId", documentId}, {"scope", "workbook"}, {"lock_structure", true}});
    REQUIRE(workbookProtected["ok"] == true);

    REQUIRE(server->Call("save_document", nlohmann::json{{"documentId", documentId}})["ok"] == true);

    auto editor = ExyokiOffice::Excel::ExcelDocumentEditor::Open(server->Path("locked.xlsx"));
    REQUIRE(editor != nullptr);
    auto sheet = editor->FirstWorksheet();
    REQUIRE(sheet != nullptr);
    const auto info = sheet->GetProtection();
    CHECK(info.has_value());
    if (info.has_value())
    {
        CHECK(info->Options.AllowSort);
        CHECK(info->Options.AllowFormatCells);
        CHECK(info->HasPassword);
    }

    // Removing protection needs the password it was created with.
    const auto wrongPassword = server->Call(
        "set_protection",
        nlohmann::json{{"documentId", documentId}, {"protect", false}, {"password", "guess"}});
    CHECK(wrongPassword["ok"] == false);
    CHECK(wrongPassword["error"]["code"] == "operation_failed");

    const auto lifted = server->Call(
        "set_protection",
        nlohmann::json{{"documentId", documentId}, {"protect", false}, {"password", "secret"}});
    REQUIRE(lifted["ok"] == true);
    CHECK(lifted["data"]["protected"] == false);

    const auto missingSheet = server->Call(
        "set_protection", nlohmann::json{{"documentId", documentId}, {"sheet", "Nowhere"}});
    CHECK(missingSheet["ok"] == false);
    CHECK(missingSheet["error"]["code"] == "sheet_not_found");
}

TEST_CASE("the overview reports what a workbook restricts [mcp-excel]")
{
    auto server = MakeExcelServer();
    server->Initialize();

    const auto created = server->Call("create_document", nlohmann::json{{"path", "guarded.xlsx"}});
    const auto documentId = created["data"]["documentId"].get<std::string>();
    REQUIRE(server->Call("add_sheet", nlohmann::json{{"documentId", documentId},
                                                      {"name", "Open"}})["ok"] == true);

    // A workbook nobody restricted says so by leaving the field out.
    const auto before = server->Call("get_document_info", nlohmann::json{{"documentId", documentId}});
    CHECK_FALSE(before["data"].contains("protection"));

    REQUIRE(server->Call("set_protection", nlohmann::json{{"documentId", documentId},
                                                           {"scope", "workbook"},
                                                           {"lock_windows", true}})["ok"] == true);
    REQUIRE(server->Call("set_protection", nlohmann::json{{"documentId", documentId},
                                                           {"scope", "sheet"},
                                                           {"sheet", "Sheet1"},
                                                           {"password", "secret"}})["ok"] == true);

    const auto reported = server->Call("get_document_info", nlohmann::json{{"documentId", documentId}});
    REQUIRE(reported["data"].contains("protection"));
    const auto& protection = reported["data"]["protection"];
    CHECK(protection["workbook"]["lockStructure"] == true);
    CHECK(protection["workbook"]["lockWindows"] == true);
    CHECK(protection["workbook"]["hasPassword"] == false);

    // Only the protected sheet is listed; the other one is not restricted and
    // has nothing to report.
    REQUIRE(protection["sheets"].size() == 1);
    CHECK(protection["sheets"][0]["sheet"] == "Sheet1");
    CHECK(protection["sheets"][0]["hasPassword"] == true);
}

TEST_CASE("move_sheet and copy_sheet refuse what they cannot do [mcp-excel]")
{
    auto server = MakeExcelServer();
    server->Initialize();

    const auto created = server->Call("create_document", nlohmann::json::object());
    const auto documentId = created["data"]["documentId"].get<std::string>();
    REQUIRE(server->Call("add_sheet", nlohmann::json{{"documentId", documentId}, {"name", "Data"}})["ok"] == true);

    const auto pastEnd = server->Call(
        "move_sheet", nlohmann::json{{"documentId", documentId}, {"sheet", "Data"}, {"to_index", 9}});
    CHECK(pastEnd["ok"] == false);
    CHECK(pastEnd["error"]["code"] == "input_invalid");

    const auto missing = server->Call(
        "move_sheet", nlohmann::json{{"documentId", documentId}, {"sheet", "Nowhere"}, {"to_index", 1}});
    CHECK(missing["ok"] == false);
    CHECK(missing["error"]["code"] == "sheet_not_found");

    // A copy cannot take a name another sheet already holds.
    const auto duplicate = server->Call(
        "copy_sheet", nlohmann::json{{"documentId", documentId}, {"sheet", "Data"}, {"name", "Data"}});
    CHECK(duplicate["ok"] == false);

    const auto noSource = server->Call("copy_sheet", nlohmann::json{{"documentId", documentId},
                                                                    {"source_path", "absent.xlsx"},
                                                                    {"sheet", 1}});
    CHECK(noSource["ok"] == false);
    CHECK(noSource["error"]["code"] == "file_not_found");

    const auto outsideWorkspace = server->Call(
        "copy_sheet",
        nlohmann::json{{"documentId", documentId}, {"source_path", "../escape.xlsx"}, {"sheet", 1}});
    CHECK(outsideWorkspace["ok"] == false);
    CHECK(outsideWorkspace["error"]["code"] == "path_outside_workspace");

    // Nothing above may have changed the workbook.
    const auto sheets = server->Call("list_sheets", nlohmann::json{{"documentId", documentId}});
    CHECK(sheets["data"]["sheets"].size() == 2);
}

/// A workbook with a source range and a table over it, which slicers filter.
static std::string MakeSlicerWorkbook(McpTestServer& server, const std::string& path)
{
    const auto created = server.Call("create_document", nlohmann::json{{"path", path}});
    const auto documentId = created["data"]["documentId"].get<std::string>();

    REQUIRE(server.Call("write_range",
                        nlohmann::json{{"documentId", documentId},
                                       {"origin", "A1"},
                                       {"values", nlohmann::json::array({
                                           nlohmann::json::array({"Region", "Revenue"}),
                                           nlohmann::json::array({"North", 1200}),
                                           nlohmann::json::array({"South", 900}),
                                           nlohmann::json::array({"East", 700}),
                                           nlohmann::json::array({"North", 300})})}})["ok"] == true);

    REQUIRE(server.Call("add_table", nlohmann::json{{"documentId", documentId},
                                                    {"range", "A1:B5"},
                                                    {"name", "Sales"}})["ok"] == true);
    return documentId;
}

TEST_CASE("a slicer filters a worksheet table and reports its buttons [mcp-excel]")
{
    auto server = MakeExcelServer();
    server->Initialize();

    const auto documentId = MakeSlicerWorkbook(*server, "sliced.xlsx");

    const auto added = server->Call("add_slicer", nlohmann::json{{"documentId", documentId},
                                                                 {"source_kind", "table"},
                                                                 {"source", "Sales"},
                                                                 {"field", "Region"},
                                                                 {"anchor_cell", "D2"},
                                                                 {"name", "RegionSlicer"},
                                                                 {"caption", "Pick a region"},
                                                                 {"columns", 2}});
    REQUIRE(added["ok"] == true);
    CHECK(added["data"]["name"] == "RegionSlicer");
    CHECK(added["data"]["anchor"] == "D2");
    // North appears twice in the source but is one button.
    CHECK(added["data"]["itemCount"] == 3);

    const auto listed = server->Call("list_slicers", nlohmann::json{{"documentId", documentId}});
    REQUIRE(listed["ok"] == true);
    REQUIRE(listed["data"]["slicers"].size() == 1);

    const auto& slicer = listed["data"]["slicers"][0];
    CHECK(slicer["name"] == "RegionSlicer");
    CHECK(slicer["caption"] == "Pick a region");
    CHECK(slicer["sourceKind"] == "table");
    CHECK(slicer["source"] == "Sales");
    CHECK(slicer["field"] == "Region");
    CHECK(slicer["columns"] == 2);
    REQUIRE(slicer["items"].size() == 3);
    for (const auto& item : slicer["items"])
    {
        CHECK(item["selected"] == true);
    }

    const auto selected = server->Call(
        "set_slicer_selection",
        nlohmann::json{{"documentId", documentId},
                       {"slicer", "RegionSlicer"},
                       {"selected", nlohmann::json::array({"North", "South"})}});
    REQUIRE(selected["ok"] == true);
    CHECK(selected["data"]["selectedCount"] == 2);

    const auto afterSelect = server->Call("list_slicers", nlohmann::json{{"documentId", documentId}});
    ExyokiOffice::Size chosen = 0;
    for (const auto& item : afterSelect["data"]["slicers"][0]["items"])
    {
        if (item["selected"] == true)
        {
            ++chosen;
            CHECK(item["caption"] != "East");
        }
    }

    CHECK(chosen == 2);

    // An empty list is how a filter is cleared, not an error.
    const auto cleared = server->Call("set_slicer_selection",
                                      nlohmann::json{{"documentId", documentId},
                                                     {"slicer", "RegionSlicer"},
                                                     {"selected", nlohmann::json::array()}});
    REQUIRE(cleared["ok"] == true);

    REQUIRE(server->Call("save_document", nlohmann::json{{"documentId", documentId}})["ok"] == true);

    auto editor = ExyokiOffice::Excel::ExcelDocumentEditor::Open(server->Path("sliced.xlsx"));
    REQUIRE(editor != nullptr);
    auto sheet = editor->FirstWorksheet();
    REQUIRE(sheet != nullptr);
    CHECK(sheet->Slicers().size() == 1);

    const auto report = ExyokiOffice::Tools::Run(server->Path("sliced.xlsx"));
    CHECK_MESSAGE(report.ErrorCount == 0, DescribeValidationErrors(report));
}

TEST_CASE("the slicer tools refuse a source or a caption that does not exist [mcp-excel]")
{
    auto server = MakeExcelServer();
    server->Initialize();

    const auto documentId = MakeSlicerWorkbook(*server, "sliced2.xlsx");

    const auto unknownTable = server->Call("add_slicer", nlohmann::json{{"documentId", documentId},
                                                                        {"source_kind", "table"},
                                                                        {"source", "NoSuchTable"},
                                                                        {"field", "Region"},
                                                                        {"anchor_cell", "D2"}});
    CHECK(unknownTable["ok"] == false);
    CHECK(unknownTable["error"]["code"] == "operation_failed");

    const auto unknownField = server->Call("add_slicer", nlohmann::json{{"documentId", documentId},
                                                                        {"source_kind", "table"},
                                                                        {"source", "Sales"},
                                                                        {"field", "NoSuchColumn"},
                                                                        {"anchor_cell", "D2"}});
    CHECK(unknownField["ok"] == false);
    CHECK(unknownField["error"]["code"] == "operation_failed");

    const auto badAnchor = server->Call("add_slicer", nlohmann::json{{"documentId", documentId},
                                                                     {"source_kind", "table"},
                                                                     {"source", "Sales"},
                                                                     {"field", "Region"},
                                                                     {"anchor_cell", "A0"}});
    CHECK(badAnchor["ok"] == false);
    CHECK(badAnchor["error"]["code"] == "range_invalid");

    // A pivot slicer over a table name finds no pivot table of that name.
    const auto wrongKind = server->Call("add_slicer", nlohmann::json{{"documentId", documentId},
                                                                     {"source_kind", "pivot_table"},
                                                                     {"source", "Sales"},
                                                                     {"field", "Region"},
                                                                     {"anchor_cell", "D2"}});
    CHECK(wrongKind["ok"] == false);

    const auto noSlicers = server->Call("list_slicers", nlohmann::json{{"documentId", documentId}});
    CHECK(noSlicers["data"]["slicers"].empty());

    const auto unknownSlicer = server->Call(
        "set_slicer_selection",
        nlohmann::json{{"documentId", documentId},
                       {"slicer", "Nothing"},
                       {"selected", nlohmann::json::array({"North"})}});
    CHECK(unknownSlicer["ok"] == false);
    CHECK(unknownSlicer["error"]["code"] == "shape_not_found");

    REQUIRE(server->Call("add_slicer", nlohmann::json{{"documentId", documentId},
                                                      {"source_kind", "table"},
                                                      {"source", "Sales"},
                                                      {"field", "Region"},
                                                      {"anchor_cell", "D2"},
                                                      {"name", "Region"}})["ok"] == true);

    // A caption the slicer does not offer is refused rather than ignored, so a
    // typo cannot quietly produce a filter nobody asked for.
    const auto unknownCaption = server->Call(
        "set_slicer_selection",
        nlohmann::json{{"documentId", documentId},
                       {"slicer", "Region"},
                       {"selected", nlohmann::json::array({"West"})}});
    CHECK(unknownCaption["ok"] == false);
    CHECK(unknownCaption["error"]["code"] == "input_invalid");
}

/// A workbook with a table over four data rows, which the table tools filter.
static std::string MakeTableWorkbook(McpTestServer& server, const std::string& path)
{
    const auto created = server.Call("create_document", nlohmann::json{{"path", path}});
    const auto documentId = created["data"]["documentId"].get<std::string>();

    REQUIRE(server.Call("write_range",
                        nlohmann::json{{"documentId", documentId},
                                       {"origin", "A1"},
                                       {"values", nlohmann::json::array({
                                           nlohmann::json::array({"Region", "Quarter", "Amount"}),
                                           nlohmann::json::array({"North", "Q1", 100}),
                                           nlohmann::json::array({"South", "Q2", 200}),
                                           nlohmann::json::array({"East", "Q1", 300}),
                                           nlohmann::json::array({"North", "Q2", 400})})}})["ok"] == true);

    REQUIRE(server.Call("add_table", nlohmann::json{{"documentId", documentId},
                                                    {"range", "A1:C5"},
                                                    {"name", "Sales"}})["ok"] == true);
    return documentId;
}

TEST_CASE("tables are listed with their columns and the filters in force [mcp-excel]")
{
    auto server = MakeExcelServer();
    server->Initialize();

    const auto documentId = MakeTableWorkbook(*server, "tables.xlsx");

    const auto listed = server->Call("list_tables", nlohmann::json{{"documentId", documentId}});
    REQUIRE(listed["ok"] == true);
    REQUIRE(listed["data"]["tables"].size() == 1);

    const auto& table = listed["data"]["tables"][0];
    CHECK(table["name"] == "Sales");
    CHECK(table["sheet"] == "Sheet1");
    CHECK(table["range"] == "A1:C5");
    CHECK(table["autoFilter"] == true);
    CHECK(table["totalsRow"] == false);
    REQUIRE(table["columns"].size() == 3);
    CHECK(table["columns"][0]["name"] == "Region");
    CHECK(table["filters"].empty());

    const auto filtered = server->Call(
        "update_table",
        nlohmann::json{{"documentId", documentId},
                       {"table", "sales"},
                       {"filters", nlohmann::json::array({nlohmann::json{
                           {"column", "Region"},
                           {"values", nlohmann::json::array({"North", "South"})}}})}});
    REQUIRE(filtered["ok"] == true);
    CHECK(filtered["data"]["filterCount"] == 1);

    const auto afterFilter = server->Call("list_tables", nlohmann::json{{"documentId", documentId}});
    const auto& active = afterFilter["data"]["tables"][0]["filters"];
    REQUIRE(active.size() == 1);
    CHECK(active[0]["column"] == "Region");
    CHECK(active[0]["columnIndex"] == 1);
    CHECK(active[0]["values"].size() == 2);

    REQUIRE(server->Call("save_document", nlohmann::json{{"documentId", documentId}})["ok"] == true);

    auto editor = ExyokiOffice::Excel::ExcelDocumentEditor::Open(server->Path("tables.xlsx"));
    REQUIRE(editor != nullptr);
    auto sheet = editor->FirstWorksheet();
    REQUIRE(sheet != nullptr);

    // The criteria alone leave every row on screen: a reader applies no filter
    // on open, so the excluded rows have to be hidden in the file as well.
    const auto hidden = [&](ExyokiOffice::UInt32 row)
    {
        const auto dimension = sheet->GetRowDimension(row);
        return dimension.has_value() ? dimension->Hidden : false;
    };
    CHECK_FALSE(hidden(2));
    CHECK_FALSE(hidden(3));
    CHECK(hidden(4));
    CHECK_FALSE(hidden(5));

    const auto report = ExyokiOffice::Tools::Run(server->Path("tables.xlsx"));
    CHECK_MESSAGE(report.ErrorCount == 0, DescribeValidationErrors(report));
}

TEST_CASE("clearing a table filter brings the hidden rows back [mcp-excel]")
{
    auto server = MakeExcelServer();
    server->Initialize();

    const auto documentId = MakeTableWorkbook(*server, "unfiltered.xlsx");

    REQUIRE(server->Call("update_table",
                         nlohmann::json{{"documentId", documentId},
                                        {"table", "Sales"},
                                        {"filters", nlohmann::json::array({nlohmann::json{
                                            {"column", 1},
                                            {"values", nlohmann::json::array({"North"})}}})}})["ok"] == true);

    const auto cleared = server->Call("update_table", nlohmann::json{{"documentId", documentId},
                                                                     {"table", "Sales"},
                                                                     {"clear_filters", true}});
    REQUIRE(cleared["ok"] == true);
    CHECK(cleared["data"]["filterCount"] == 0);

    REQUIRE(server->Call("save_document", nlohmann::json{{"documentId", documentId}})["ok"] == true);

    auto editor = ExyokiOffice::Excel::ExcelDocumentEditor::Open(server->Path("unfiltered.xlsx"));
    REQUIRE(editor != nullptr);
    auto sheet = editor->FirstWorksheet();
    for (ExyokiOffice::UInt32 row = 2; row <= 5; ++row)
    {
        const auto dimension = sheet->GetRowDimension(row);
        const bool isHidden = dimension.has_value() ? dimension->Hidden : false;
        CHECK_FALSE(isHidden);
    }
}

TEST_CASE("a totals row grows the table and keeps the workbook valid [mcp-excel]")
{
    auto server = MakeExcelServer();
    server->Initialize();

    const auto documentId = MakeTableWorkbook(*server, "totals.xlsx");

    const auto shown = server->Call(
        "update_table", nlohmann::json{{"documentId", documentId}, {"table", "Sales"}, {"totals_row", true}});
    REQUIRE(shown["ok"] == true);
    CHECK(shown["data"]["totalsRow"] == true);
    CHECK(shown["data"]["range"] == "A1:C6");

    REQUIRE(server->Call("save_document", nlohmann::json{{"documentId", documentId}})["ok"] == true);

    auto editor = ExyokiOffice::Excel::ExcelDocumentEditor::Open(server->Path("totals.xlsx"));
    REQUIRE(editor != nullptr);
    auto sheet = editor->FirstWorksheet();
    REQUIRE(sheet != nullptr);
    REQUIRE(sheet->Tables().size() == 1);

    // Excel refuses the workbook when the auto-filter reaches into the totals
    // row, so the two rectangles have to differ.
    const auto xml = sheet->Tables()[0]->GetPart()->GetXmlString();
    CHECK(xml.find("ref=\"A1:C6\"") != std::string::npos);
    CHECK(xml.find("autoFilter ref=\"A1:C5\"") != std::string::npos);

    const auto report = ExyokiOffice::Tools::Run(server->Path("totals.xlsx"));
    CHECK_MESSAGE(report.ErrorCount == 0, DescribeValidationErrors(report));
}

TEST_CASE("the table tools refuse a table, a column or a filter that makes no sense [mcp-excel]")
{
    auto server = MakeExcelServer();
    server->Initialize();

    const auto documentId = MakeTableWorkbook(*server, "refused.xlsx");

    const auto unknownTable = server->Call(
        "update_table", nlohmann::json{{"documentId", documentId}, {"table", "Nowhere"}, {"auto_filter", false}});
    CHECK(unknownTable["ok"] == false);
    CHECK(unknownTable["error"]["code"] == "block_not_found");

    const auto unknownColumn = server->Call(
        "update_table",
        nlohmann::json{{"documentId", documentId},
                       {"table", "Sales"},
                       {"filters", nlohmann::json::array({nlohmann::json{
                           {"column", "Nope"}, {"values", nlohmann::json::array({"North"})}}})}});
    CHECK(unknownColumn["ok"] == false);
    CHECK(unknownColumn["error"]["code"] == "input_invalid");

    const auto pastEnd = server->Call(
        "update_table",
        nlohmann::json{{"documentId", documentId},
                       {"table", "Sales"},
                       {"filters", nlohmann::json::array({nlohmann::json{
                           {"column", 9}, {"values", nlohmann::json::array({"North"})}}})}});
    CHECK(pastEnd["ok"] == false);
    CHECK(pastEnd["error"]["code"] == "input_invalid");

    // An empty filter would hide every row, which is never what was meant.
    const auto empty = server->Call(
        "update_table", nlohmann::json{{"documentId", documentId},
                                       {"table", "Sales"},
                                       {"filters", nlohmann::json::array({nlohmann::json{
                                           {"column", "Region"},
                                           {"values", nlohmann::json::array()}}})}});
    CHECK(empty["ok"] == false);
    CHECK(empty["error"]["code"] == "input_invalid");

    const auto badName = server->Call(
        "update_table", nlohmann::json{{"documentId", documentId}, {"table", "Sales"}, {"name", "not a name"}});
    CHECK(badName["ok"] == false);
    CHECK(badName["error"]["code"] == "operation_failed");

    const auto missingSheet = server->Call(
        "list_tables", nlohmann::json{{"documentId", documentId}, {"sheet", "Nowhere"}});
    CHECK(missingSheet["ok"] == false);
    CHECK(missingSheet["error"]["code"] == "sheet_not_found");

    // Nothing above may have touched the table.
    const auto listed = server->Call("list_tables", nlohmann::json{{"documentId", documentId}});
    REQUIRE(listed["data"]["tables"].size() == 1);
    CHECK(listed["data"]["tables"][0]["name"] == "Sales");
    CHECK(listed["data"]["tables"][0]["filters"].empty());
}

TEST_CASE("conditional formatting covers the ranking and average rules [mcp-excel]")
{
    auto server = MakeExcelServer();
    server->Initialize();

    const auto created = server->Call("create_document", nlohmann::json{{"path", "rules.xlsx"}});
    const auto documentId = created["data"]["documentId"].get<std::string>();
    REQUIRE(server->Call("write_range",
                         nlohmann::json{{"documentId", documentId},
                                        {"origin", "A1"},
                                        {"values", nlohmann::json::array({nlohmann::json::array({10}),
                                                                          nlohmann::json::array({20}),
                                                                          nlohmann::json::array({30}),
                                                                          nlohmann::json::array({40})})}})
                ["ok"] == true);

    const auto top = server->Call("add_conditional_formatting",
                                  nlohmann::json{{"documentId", documentId},
                                                 {"range", "A1:A4"},
                                                 {"rule", nlohmann::json{{"type", "top"}, {"rank", 2}}}});
    REQUIRE(top["ok"] == true);
    CHECK(top["data"]["type"] == "top");

    REQUIRE(server->Call("add_conditional_formatting",
                         nlohmann::json{{"documentId", documentId},
                                        {"range", "A1:A4"},
                                        {"rule", nlohmann::json{{"type", "bottom"},
                                                                {"rank", 25},
                                                                {"percent", true}}}})["ok"] == true);

    REQUIRE(server->Call("add_conditional_formatting",
                         nlohmann::json{{"documentId", documentId},
                                        {"range", "A1:A4"},
                                        {"rule", nlohmann::json{{"type", "aboveAverage"},
                                                                {"equal_average", true},
                                                                {"standard_deviation", 1}}}})["ok"] == true);

    REQUIRE(server->Call("add_conditional_formatting",
                         nlohmann::json{{"documentId", documentId},
                                        {"range", "A1:A4"},
                                        {"rule", nlohmann::json{{"type", "containsErrors"}}}})["ok"] == true);

    // Several rectangles are one rule over one population, not a rule each.
    const auto several = server->Call(
        "add_conditional_formatting",
        nlohmann::json{{"documentId", documentId},
                       {"range", nlohmann::json::array({"A1:A2", "C1:C2"})},
                       {"rule", nlohmann::json{{"type", "belowAverage"}}}});
    REQUIRE(several["ok"] == true);
    CHECK(several["data"]["range"] == "A1:A2 C1:C2");

    REQUIRE(server->Call("save_document", nlohmann::json{{"documentId", documentId}})["ok"] == true);

    auto editor = ExyokiOffice::Excel::ExcelDocumentEditor::Open(server->Path("rules.xlsx"));
    REQUIRE(editor != nullptr);
    CHECK(editor->FirstWorksheet()->ConditionalFormattings().size() == 5);

    const auto report = ExyokiOffice::Tools::Run(server->Path("rules.xlsx"));
    CHECK_MESSAGE(report.ErrorCount == 0, DescribeValidationErrors(report));
}

TEST_CASE("a conditional formatting rule is refused when it cannot be built [mcp-excel]")
{
    auto server = MakeExcelServer();
    server->Initialize();

    const auto created = server->Call("create_document", nlohmann::json{{"path", "badrules.xlsx"}});
    const auto documentId = created["data"]["documentId"].get<std::string>();

    // Color scales and data bars are not written by this version, and the
    // schema says so, so the call never reaches the handler.
    const auto colorScale = server->Call(
        "add_conditional_formatting",
        nlohmann::json{{"documentId", documentId}, {"range", "A1:A4"}, {"rule", nlohmann::json{{"type",
                                                                                               "colorScale"}}}});
    CHECK(colorScale["ok"] == false);
    CHECK(colorScale["error"]["code"] == "input_invalid");

    const auto missingOperator = server->Call(
        "add_conditional_formatting",
        nlohmann::json{{"documentId", documentId},
                       {"range", "A1:A4"},
                       {"rule", nlohmann::json{{"type", "cellIs"}, {"formula1", "1"}}}});
    CHECK(missingOperator["ok"] == false);
    CHECK(missingOperator["error"]["code"] == "input_invalid");

    // A text rule without its text builds nothing.
    const auto missingText = server->Call(
        "add_conditional_formatting",
        nlohmann::json{{"documentId", documentId},
                       {"range", "A1:A4"},
                       {"rule", nlohmann::json{{"type", "containsText"}}}});
    CHECK(missingText["ok"] == false);
    CHECK(missingText["error"]["code"] == "input_invalid");

    const auto badRange = server->Call(
        "add_conditional_formatting",
        nlohmann::json{{"documentId", documentId},
                       {"range", nlohmann::json::array({"A1:A4", "nonsense"})},
                       {"rule", nlohmann::json{{"type", "duplicateValues"}}}});
    CHECK(badRange["ok"] == false);
    CHECK(badRange["error"]["code"] == "range_invalid");

    // A refused rule writes nothing, so the new document is still clean.
    const auto documents = server->Call("list_documents", nlohmann::json::object());
    REQUIRE(documents["data"]["documents"].size() == 1);
    CHECK(documents["data"]["documents"][0]["dirty"] == false);
}

TEST_CASE("a conditional formatting rule paints with the format it is given [mcp-excel]")
{
    auto server = MakeExcelServer();
    server->Initialize();

    const auto created = server->Call("create_document", nlohmann::json{{"path", "painted.xlsx"}});
    const auto documentId = created["data"]["documentId"].get<std::string>();

    const auto painted = server->Call(
        "add_conditional_formatting",
        nlohmann::json{{"documentId", documentId},
                       {"range", "A1:A10"},
                       {"rule",
                        nlohmann::json{
                            {"type", "cellIs"}, {"operator", "greaterThan"}, {"formula1", "100"}}},
                       {"format", nlohmann::json{{"fill", nlohmann::json{{"color", "#FF0000"}}},
                                                 {"font", nlohmann::json{{"bold", true},
                                                                         {"color", "#FFFFFF"}}}}}});
    REQUIRE(painted["ok"] == true);
    CHECK(painted["data"]["differentialFormatId"] == 0);

    // A second rule asking for the same appearance reuses the same format.
    const auto reused = server->Call(
        "add_conditional_formatting",
        nlohmann::json{{"documentId", documentId},
                       {"range", "C1:C10"},
                       {"rule", nlohmann::json{{"type", "duplicateValues"}}},
                       {"format", nlohmann::json{{"fill", nlohmann::json{{"color", "#FF0000"}}},
                                                 {"font", nlohmann::json{{"bold", true},
                                                                         {"color", "#FFFFFF"}}}}}});
    REQUIRE(reused["ok"] == true);
    CHECK(reused["data"]["differentialFormatId"] == 0);

    // A rule without a format matches cells and changes nothing, which the
    // answer says by leaving the field out rather than reporting a format.
    const auto plain = server->Call(
        "add_conditional_formatting",
        nlohmann::json{{"documentId", documentId},
                       {"range", "E1:E10"},
                       {"rule", nlohmann::json{{"type", "containsBlanks"}}}});
    REQUIRE(plain["ok"] == true);
    CHECK_FALSE(plain["data"].contains("differentialFormatId"));

    const auto different = server->Call(
        "add_conditional_formatting",
        nlohmann::json{{"documentId", documentId},
                       {"range", "G1:G10"},
                       {"rule", nlohmann::json{{"type", "containsErrors"}}},
                       {"format", nlohmann::json{{"number_format", "0.00"},
                                                 {"alignment", nlohmann::json{{"horizontal", "center"}}}}}});
    REQUIRE(different["ok"] == true);
    CHECK(different["data"]["differentialFormatId"] == 1);

    REQUIRE(server->Call("save_document", nlohmann::json{{"documentId", documentId}})["ok"] == true);

    auto editor = ExyokiOffice::Excel::ExcelDocumentEditor::Open(server->Path("painted.xlsx"));
    REQUIRE(editor != nullptr);
    CHECK(editor->Styles().DifferentialFormatCount() == 2);

    const auto rules = editor->FirstWorksheet()->ConditionalFormattings();
    REQUIRE(rules.size() == 4);
    REQUIRE(rules[0]->Definition().DifferentialFormatId.has_value());
    CHECK(*rules[0]->Definition().DifferentialFormatId == 0);
    CHECK_FALSE(rules[2]->Definition().DifferentialFormatId.has_value());

    // A dxf solid fill carries only bgColor; the same colour written as a cell
    // fill validates and Excel then paints nothing.
    const auto styles = editor->GetDocument()->GetWorkbookPart()->GetWorkbookStylesPart()->GetXmlString();
    const auto dxfs = styles.substr(styles.find("<x:dxfs"));
    CHECK(dxfs.find("<x:bgColor rgb=\"FFFF0000\"") != std::string::npos);
    CHECK(dxfs.find("fgColor") == std::string::npos);

    const auto report = ExyokiOffice::Tools::Run(server->Path("painted.xlsx"));
    CHECK_MESSAGE(report.ErrorCount == 0, DescribeValidationErrors(report));
}

TEST_CASE("a conditional format that cannot be built is refused whole [mcp-excel]")
{
    auto server = MakeExcelServer();
    server->Initialize();

    const auto created = server->Call("create_document", nlohmann::json{{"path", "badformat.xlsx"}});
    const auto documentId = created["data"]["documentId"].get<std::string>();

    const auto empty = server->Call(
        "add_conditional_formatting",
        nlohmann::json{{"documentId", documentId},
                       {"range", "A1:A4"},
                       {"rule", nlohmann::json{{"type", "duplicateValues"}}},
                       {"format", nlohmann::json::object()}});
    CHECK(empty["ok"] == false);
    CHECK(empty["error"]["code"] == "input_invalid");

    const auto badColor = server->Call(
        "add_conditional_formatting",
        nlohmann::json{{"documentId", documentId},
                       {"range", "A1:A4"},
                       {"rule", nlohmann::json{{"type", "duplicateValues"}}},
                       {"format", nlohmann::json{{"fill", nlohmann::json{{"color", "crimson"}}}}}});
    CHECK(badColor["ok"] == false);
    CHECK(badColor["error"]["code"] == "input_invalid");

    // The format publishes a closed vocabulary, so a member outside it is
    // rejected against the schema rather than quietly dropped.
    const auto unknownMember = server->Call(
        "add_conditional_formatting",
        nlohmann::json{{"documentId", documentId},
                       {"range", "A1:A4"},
                       {"rule", nlohmann::json{{"type", "duplicateValues"}}},
                       {"format", nlohmann::json{{"gradient", "rainbow"}}}});
    CHECK(unknownMember["ok"] == false);
    CHECK(unknownMember["error"]["code"] == "input_invalid");

    const auto notAnObject = server->Call(
        "add_conditional_formatting",
        nlohmann::json{{"documentId", documentId},
                       {"range", "A1:A4"},
                       {"rule", nlohmann::json{{"type", "duplicateValues"}}},
                       {"format", "red"}});
    CHECK(notAnObject["ok"] == false);

    // A rule the format cannot be built for is not written half way: neither
    // the rule nor a differential format survives, and the document is clean.
    const auto documents = server->Call("list_documents", nlohmann::json::object());
    REQUIRE(documents["data"]["documents"].size() == 1);
    CHECK(documents["data"]["documents"][0]["dirty"] == false);
}

/// Stand-in for a VBA project: an OLE compound-file signature and filler.
///
/// The server treats the payload as opaque throughout, so the round trip is
/// exactly as meaningful with these bytes as with a real project. What no test
/// here can show is whether Excel accepts a project the server embedded: that
/// needs a genuine `vbaProject.bin`, and obtaining one means turning on trusted
/// access to the VBA object model, which is not a setting a test may change.
static std::vector<ExyokiOffice::Byte> MakeVbaPayload(ExyokiOffice::Byte marker)
{
    std::vector<ExyokiOffice::Byte> payload{0xD0, 0xCF, 0x11, 0xE0, 0xA1, 0xB1, 0x1A, 0xE1};
    payload.resize(64, marker);
    return payload;
}

static void WriteWorkspaceFile(const std::filesystem::path& path,
                               const std::vector<ExyokiOffice::Byte>& payload)
{
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    REQUIRE(stream.good());
    stream.write(reinterpret_cast<const char*>(payload.data()),
                 static_cast<std::streamsize>(payload.size()));
    REQUIRE(stream.good());
}

TEST_CASE("a VBA project is embedded, extracted unchanged, and removed [mcp-excel]")
{
    auto server = MakeExcelServer();
    server->Initialize();

    const auto payload = MakeVbaPayload(0x11);
    WriteWorkspaceFile(server->Path("macros.bin"), payload);

    const auto created = server->Call("create_document", nlohmann::json{{"path", "book.xlsm"}});
    const auto documentId = created["data"]["documentId"].get<std::string>();

    const auto before = server->Call("get_vba_project", nlohmann::json{{"documentId", documentId}});
    REQUIRE(before["ok"] == true);
    CHECK(before["data"]["present"] == false);

    const auto embedded = server->Call(
        "set_vba_project", nlohmann::json{{"documentId", documentId}, {"source_path", "macros.bin"}});
    REQUIRE(embedded["ok"] == true);
    CHECK(embedded["data"]["bytes"] == payload.size());
    CHECK(embedded["data"]["macroEnabled"] == true);

    const auto extracted = server->Call(
        "get_vba_project", nlohmann::json{{"documentId", documentId}, {"output_path", "extracted.bin"}});
    REQUIRE(extracted["ok"] == true);
    CHECK(extracted["data"]["present"] == true);
    CHECK(extracted["data"]["path"] == "extracted.bin");

    // The payload is opaque, so what comes back has to be what went in.
    std::ifstream stream(server->Path("extracted.bin"), std::ios::binary);
    REQUIRE(stream.good());
    const std::vector<ExyokiOffice::Byte> roundTripped((std::istreambuf_iterator<char>(stream)),
                                                       std::istreambuf_iterator<char>());
    CHECK(roundTripped == payload);

    // Embedding again replaces rather than appends.
    const auto replacement = MakeVbaPayload(0x22);
    WriteWorkspaceFile(server->Path("other.bin"), replacement);
    REQUIRE(server->Call("set_vba_project",
                         nlohmann::json{{"documentId", documentId},
                                        {"source_path", "other.bin"}})["ok"] == true);

    REQUIRE(server->Call("save_document", nlohmann::json{{"documentId", documentId}})["ok"] == true);

    auto editor = ExyokiOffice::Excel::ExcelDocumentEditor::Open(server->Path("book.xlsm"));
    REQUIRE(editor != nullptr);
    REQUIRE(editor->GetDocument() != nullptr);
    CHECK(editor->GetDocument()->HasVbaProject());
    CHECK(editor->GetDocument()->GetVbaProjectData() == replacement);

    const auto removed = server->Call("remove_vba_project", nlohmann::json{{"documentId", documentId}});
    REQUIRE(removed["ok"] == true);
    CHECK(removed["data"]["removed"] == true);

    const auto after = server->Call("get_vba_project", nlohmann::json{{"documentId", documentId}});
    CHECK(after["data"]["present"] == false);

    const auto report = ExyokiOffice::Tools::Run(server->Path("book.xlsm"));
    CHECK_MESSAGE(report.ErrorCount == 0, DescribeValidationErrors(report));
}

TEST_CASE("the VBA tools refuse a payload they cannot take [mcp-excel]")
{
    auto server = MakeExcelServer();
    server->Initialize();

    const auto created = server->Call("create_document", nlohmann::json{{"path", "plain.xlsm"}});
    const auto documentId = created["data"]["documentId"].get<std::string>();

    // Removing what is not there is the state that was asked for, so it is
    // reported as done with nothing removed rather than as an error.
    const auto nothingToRemove = server->Call("remove_vba_project", nlohmann::json{{"documentId", documentId}});
    CHECK(nothingToRemove["ok"] == true);
    CHECK(nothingToRemove["data"]["removed"] == false);

    const auto missing = server->Call(
        "set_vba_project", nlohmann::json{{"documentId", documentId}, {"source_path", "absent.bin"}});
    CHECK(missing["ok"] == false);
    CHECK(missing["error"]["code"] == "file_not_found");

    const auto outside = server->Call(
        "set_vba_project", nlohmann::json{{"documentId", documentId}, {"source_path", "../escape.bin"}});
    CHECK(outside["ok"] == false);
    CHECK(outside["error"]["code"] == "path_outside_workspace");

    WriteWorkspaceFile(server->Path("empty.bin"), {});
    const auto empty = server->Call(
        "set_vba_project", nlohmann::json{{"documentId", documentId}, {"source_path", "empty.bin"}});
    CHECK(empty["ok"] == false);
    CHECK(empty["error"]["code"] == "input_invalid");

    // A workspace file that already exists is not overwritten by accident.
    const auto payload = MakeVbaPayload(0x33);
    WriteWorkspaceFile(server->Path("macros.bin"), payload);
    REQUIRE(server->Call("set_vba_project",
                         nlohmann::json{{"documentId", documentId},
                                        {"source_path", "macros.bin"}})["ok"] == true);
    WriteWorkspaceFile(server->Path("taken.bin"), MakeVbaPayload(0x44));
    const auto clash = server->Call(
        "get_vba_project", nlohmann::json{{"documentId", documentId}, {"output_path", "taken.bin"}});
    CHECK(clash["ok"] == false);

    const auto forced = server->Call("get_vba_project", nlohmann::json{{"documentId", documentId},
                                                                       {"output_path", "taken.bin"},
                                                                       {"overwrite", true}});
    CHECK(forced["ok"] == true);

    // Nothing above may have written the workbook to disk.
    const auto documents = server->Call("list_documents", nlohmann::json::object());
    REQUIRE(documents["data"]["documents"].size() == 1);
    CHECK(documents["data"]["documents"][0]["dirty"] == true);
}

// ---------------------------------------------------------------------------
// Regressions found by the Office COM run (MCP_ERRORS.md, exyoki-mcp-excel)
// ---------------------------------------------------------------------------

/// Whether @p part carries a relationship with the id @p id.
static bool HasRelationship(const ExyokiOffice::OpenXmlPackagePart& part, const std::string& id)
{
    const auto& relationships = part.Relationships();
    return std::any_of(relationships.begin(), relationships.end(),
                       [&id](const ExyokiOffice::OpenXmlRelationship& relationship) { return relationship.Id == id; });
}

/// Every relationship id an XML part refers to (`r:id`, `r:embed`, `r:link`).
static std::vector<std::string> ReferencedRelationshipIds(const std::string& xml)
{
    std::vector<std::string> ids;
    for (const char* attribute : {" r:id=\"", " r:embed=\"", " r:link=\""})
    {
        const std::string marker(attribute);
        for (auto position = xml.find(marker); position != std::string::npos; position = xml.find(marker, position))
        {
            position += marker.size();
            const auto end = xml.find('"', position);
            if (end == std::string::npos)
            {
                break;
            }
            ids.push_back(xml.substr(position, end - position));
            position = end;
        }
    }
    return ids;
}

/// The 1x1 PNG the image tests embed.
static const std::string OnePixelPng =
    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAEhQGAhKmMIQAAAABJRU5ErkJggg==";

TEST_CASE("X-1: copy_sheet within a workbook keeps every related part reachable [mcp-excel]")
{
    auto server = MakeExcelServer();
    server->Initialize();

    const auto created = server->Call("create_document", nlohmann::json{{"path", "copied.xlsx"}});
    const auto documentId = created["data"]["documentId"].get<std::string>();

    REQUIRE(server->Call("write_range",
                         nlohmann::json{{"documentId", documentId},
                                        {"origin", "A1"},
                                        {"values", nlohmann::json::array({
                                            nlohmann::json::array({"Region", "Revenue"}),
                                            nlohmann::json::array({"North", 120}),
                                            nlohmann::json::array({"South", 90})})}})["ok"] == true);
    REQUIRE(server->Call("add_image", nlohmann::json{{"documentId", documentId},
                                                     {"anchor_cell", "D2"},
                                                     {"dataBase64", OnePixelPng},
                                                     {"width", "4cm"},
                                                     {"height", "3cm"}})["ok"] == true);
    REQUIRE(server->Call("set_hyperlink", nlohmann::json{{"documentId", documentId},
                                                         {"cell", "A1"},
                                                         {"target", "https://example.com"}})["ok"] == true);
    REQUIRE(server->Call("add_comment", nlohmann::json{{"documentId", documentId},
                                                       {"cell", "B2"},
                                                       {"text", "A plain note."},
                                                       {"threaded", false}})["ok"] == true);
    const auto thread = server->Call("add_comment", nlohmann::json{{"documentId", documentId},
                                                                   {"cell", "B3"},
                                                                   {"text", "A threaded one."},
                                                                   {"author", "Jakub"},
                                                                   {"threaded", true}});
    REQUIRE(thread["ok"] == true);
    REQUIRE(server->Call("add_table", nlohmann::json{{"documentId", documentId},
                                                     {"range", "A1:B3"},
                                                     {"name", "Sales"}})["ok"] == true);
    REQUIRE(server->Call("add_chart", nlohmann::json{{"documentId", documentId},
                                                     {"type", "column"},
                                                     {"data_range", "A1:B3"},
                                                     {"anchor_cell", "D10"}})["ok"] == true);

    const auto copied =
        server->Call("copy_sheet", nlohmann::json{{"documentId", documentId}, {"sheet", 1}, {"name", "Copy"}});
    REQUIRE(copied["ok"] == true);
    CHECK(copied["data"]["name"] == "Copy");

    // The copy holds the same data, and its table is a table of its own.
    const auto values = server->Call(
        "read_range", nlohmann::json{{"documentId", documentId}, {"sheet", "Copy"}, {"range", "A2:B2"}});
    REQUIRE(values["ok"] == true);
    CHECK(values["data"]["values"][0][0] == "North");
    const auto tables = server->Call("list_tables", nlohmann::json{{"documentId", documentId}});
    REQUIRE(tables["ok"] == true);
    REQUIRE(tables["data"]["tables"].size() == 2);
    CHECK(tables["data"]["tables"][0]["name"] != tables["data"]["tables"][1]["name"]);

    REQUIRE(server->Call("save_document", nlohmann::json{{"documentId", documentId}})["ok"] == true);

    auto editor = ExyokiOffice::Excel::ExcelDocumentEditor::Open(server->Path("copied.xlsx"));
    REQUIRE(editor != nullptr);
    const auto sheets = editor->Worksheets();
    REQUIRE(sheets.size() == 2);
    const auto original = sheets[0];
    const auto copy = sheets[1];
    REQUIRE(copy->Name() == "Copy");

    // Every relationship id the copied sheet and its drawing name has to exist,
    // which is exactly what Excel checks before it opens the workbook.
    const auto copyPart = copy->GetPart();
    REQUIRE(copyPart != nullptr);
    const auto sheetIds = ReferencedRelationshipIds(copyPart->GetXmlString());
    CHECK_MESSAGE(sheetIds.size() >= 4, copyPart->GetXmlString().substr(0, 800));
    for (const auto& id : sheetIds)
    {
        CHECK_MESSAGE(HasRelationship(*copyPart, id), "dangling sheet relationship " << id);
    }
    const auto drawing = copyPart->GetDrawingsPart();
    REQUIRE(drawing != nullptr);
    const auto drawingIds = ReferencedRelationshipIds(drawing->GetXmlString());
    CHECK(drawingIds.size() == 2);
    for (const auto& id : drawingIds)
    {
        CHECK_MESSAGE(HasRelationship(*drawing, id), "dangling drawing relationship " << id);
    }
    CHECK(copyPart->GetDrawingsPart() != original->GetPart()->GetDrawingsPart());

    // The copied parts are independent objects with identities of their own.
    CHECK(copy->Images().size() == 1);
    CHECK(copy->Charts().size() == 1);
    CHECK(copy->Comments().size() == 1);
    REQUIRE(copy->ThreadedComments().size() == 1);
    REQUIRE(original->ThreadedComments().size() == 1);
    CHECK(copy->ThreadedComments()[0].Id != original->ThreadedComments()[0].Id);
    CHECK(copy->ThreadedComments()[0].PersonName == "Jakub");
    REQUIRE(copy->Hyperlinks().size() == 1);
    CHECK(copy->Hyperlinks()[0].Target == "https://example.com");
    REQUIRE(copy->Tables().size() == 1);
    REQUIRE(original->Tables().size() == 1);
    CHECK(copy->Tables()[0]->Id() != original->Tables()[0]->Id());
    CHECK(copy->Tables()[0]->Name() != original->Tables()[0]->Name());

    const auto report = ExyokiOffice::Tools::Run(server->Path("copied.xlsx"));
    CHECK_MESSAGE(report.ErrorCount == 0, DescribeValidationErrors(report));
}

TEST_CASE("X-2: add_table refuses a range that overlaps another table [mcp-excel]")
{
    auto server = MakeExcelServer();
    server->Initialize();

    const auto created = server->Call("create_document", nlohmann::json{{"path", "overlap.xlsx"}});
    const auto documentId = created["data"]["documentId"].get<std::string>();

    REQUIRE(server->Call("write_range",
                         nlohmann::json{{"documentId", documentId},
                                        {"origin", "A1"},
                                        {"values", nlohmann::json::array({
                                            nlohmann::json::array({"a", "b", "c"}),
                                            nlohmann::json::array({1, 2, 3}),
                                            nlohmann::json::array({4, 5, 6}),
                                            nlohmann::json::array({7, 8, 9})})}})["ok"] == true);
    REQUIRE(server->Call("add_table", nlohmann::json{{"documentId", documentId},
                                                     {"range", "A1:B3"},
                                                     {"name", "TblOne"}})["ok"] == true);

    const auto overlapping = server->Call(
        "add_table", nlohmann::json{{"documentId", documentId}, {"range", "B2:C4"}, {"name", "TblTwo"}});
    CHECK(overlapping["ok"] == false);
    CHECK(overlapping["error"]["code"] == "range_invalid");
    CHECK(overlapping["error"]["message"].get<std::string>().find("TblOne") != std::string::npos);

    // Nothing was written before the refusal: one table, and TblOne's data intact.
    const auto tables = server->Call("list_tables", nlohmann::json{{"documentId", documentId}});
    REQUIRE(tables["data"]["tables"].size() == 1);
    const auto values = server->Call("read_range", nlohmann::json{{"documentId", documentId}, {"range", "B2:C2"}});
    CHECK(values["data"]["values"][0][0] == 2);
    CHECK(values["data"]["values"][0][1] == 3);
}

/// Writes a workbook holding one threaded comment and closes it.
static void MakeThreadedCommentWorkbook(McpTestServer& server, const std::string& path)
{
    const auto created = server.Call("create_document", nlohmann::json{{"path", path}});
    const auto documentId = created["data"]["documentId"].get<std::string>();
    const auto thread = server.Call("add_comment", nlohmann::json{{"documentId", documentId},
                                                                  {"cell", "B2"},
                                                                  {"text", "Check this."},
                                                                  {"author", "Reviewer"},
                                                                  {"threaded", true}});
    REQUIRE(thread["ok"] == true);
    REQUIRE(server.Call("save_document", nlohmann::json{{"documentId", documentId}})["ok"] == true);
    REQUIRE(server.Call("close_document", nlohmann::json{{"documentId", documentId}})["ok"] == true);
}

/// The ids of the persons a workbook lists.
static std::vector<std::string> PersonIds(ExyokiOffice::Excel::ExcelDocumentEditor& editor)
{
    namespace Xltc = ExyokiOffice::DocumentFormat::OpenXml::Office2019::Excel::ThreadedComments;
    std::vector<std::string> ids;
    for (const auto& part : editor.GetDocument()->GetWorkbookPart()->GetWorkbookPersonParts())
    {
        const auto root = part != nullptr ? part->GetTypedRootElement() : nullptr;
        if (root == nullptr)
        {
            continue;
        }
        for (const auto& person : root->Elements<Xltc::Person>())
        {
            ids.push_back(person->GetId().ToString());
        }
    }
    return ids;
}

TEST_CASE("X-3: importing a sheet with threaded comments brings the person list along [mcp-excel]")
{
    auto server = MakeExcelServer();
    server->Initialize();

    const auto plain = server->Call("create_document", nlohmann::json{{"path", "m_plain.xlsx"}});
    const auto plainId = plain["data"]["documentId"].get<std::string>();
    REQUIRE(server->Call("save_document", nlohmann::json{{"documentId", plainId}})["ok"] == true);
    REQUIRE(server->Call("close_document", nlohmann::json{{"documentId", plainId}})["ok"] == true);
    MakeThreadedCommentWorkbook(*server, "m_thread.xlsx");

    // The plain workbook is the base, so the thread's person has to be carried
    // into a workbook that has no person list yet.
    const auto merged = server->Call(
        "merge_documents", nlohmann::json{{"input_paths", nlohmann::json::array({"m_plain.xlsx", "m_thread.xlsx"})},
                                          {"output_path", "merged.xlsx"}});
    REQUIRE(merged["ok"] == true);

    auto mergedEditor = ExyokiOffice::Excel::ExcelDocumentEditor::Open(server->Path("merged.xlsx"));
    REQUIRE(mergedEditor != nullptr);
    REQUIRE(mergedEditor->Worksheets().size() == 2);
    const auto imported = mergedEditor->Worksheets()[1];
    REQUIRE(imported->ThreadedComments().size() == 1);
    const auto personId = imported->ThreadedComments()[0].PersonId;
    CHECK_FALSE(personId.empty());
    const auto persons = PersonIds(*mergedEditor);
    CHECK(std::find(persons.begin(), persons.end(), personId) != persons.end());
    CHECK(imported->ThreadedComments()[0].PersonName == "Reviewer");
    CHECK(mergedEditor->GetDocument()->GetWorkbookPart()->GetWorkbookPersonParts().size() == 1);

    const auto mergedReport = ExyokiOffice::Tools::Run(server->Path("merged.xlsx"));
    CHECK_MESSAGE(mergedReport.ErrorCount == 0, DescribeValidationErrors(mergedReport));

    // The same path serves copy_sheet with a source workbook.
    const auto target = server->Call("create_document", nlohmann::json{{"path", "target.xlsx"}});
    const auto targetId = target["data"]["documentId"].get<std::string>();
    REQUIRE(server->Call("copy_sheet", nlohmann::json{{"documentId", targetId},
                                                      {"source_path", "m_thread.xlsx"},
                                                      {"sheet", 1},
                                                      {"name", "Imported"}})["ok"] == true);
    REQUIRE(server->Call("save_document", nlohmann::json{{"documentId", targetId}})["ok"] == true);

    auto targetEditor = ExyokiOffice::Excel::ExcelDocumentEditor::Open(server->Path("target.xlsx"));
    REQUIRE(targetEditor != nullptr);
    const auto copied = targetEditor->GetWorksheet("Imported");
    REQUIRE(copied != nullptr);
    REQUIRE(copied->ThreadedComments().size() == 1);
    const auto copiedPersons = PersonIds(*targetEditor);
    CHECK(std::find(copiedPersons.begin(), copiedPersons.end(), copied->ThreadedComments()[0].PersonId) !=
          copiedPersons.end());

    const auto targetReport = ExyokiOffice::Tools::Run(server->Path("target.xlsx"));
    CHECK_MESSAGE(targetReport.ErrorCount == 0, DescribeValidationErrors(targetReport));
}

TEST_CASE("X-4: copy_sheet from another workbook keeps the formatting of a styled sheet [mcp-excel]")
{
    auto server = MakeExcelServer();
    server->Initialize();

    const auto source = server->Call("create_document", nlohmann::json{{"path", "src.xlsx"}});
    const auto sourceId = source["data"]["documentId"].get<std::string>();
    REQUIRE(server->Call("rename_sheet", nlohmann::json{{"documentId", sourceId},
                                                        {"sheet", 1},
                                                        {"new_name", "Imported"}})["ok"] == true);
    REQUIRE(server->Call("write_range",
                         nlohmann::json{{"documentId", sourceId},
                                        {"origin", "A1"},
                                        {"values", nlohmann::json::array({nlohmann::json::array({"Bold", "Red"})})}})
                ["ok"] == true);
    REQUIRE(server->Call("format_range", nlohmann::json{{"documentId", sourceId},
                                                        {"range", "A1"},
                                                        {"font", nlohmann::json{{"bold", true}}}})["ok"] == true);
    REQUIRE(server->Call("format_range", nlohmann::json{{"documentId", sourceId},
                                                        {"range", "B1"},
                                                        {"fill", nlohmann::json{{"color", "#FF0000"}}}})["ok"] ==
            true);
    REQUIRE(server->Call("save_document", nlohmann::json{{"documentId", sourceId}})["ok"] == true);
    REQUIRE(server->Call("close_document", nlohmann::json{{"documentId", sourceId}})["ok"] == true);

    // The destination has an unrelated style of its own, so the two catalogs
    // differ and every imported index has to be translated.
    const auto created = server->Call("create_document", nlohmann::json{{"path", "dest.xlsx"}});
    const auto documentId = created["data"]["documentId"].get<std::string>();
    REQUIRE(server->Call("format_range", nlohmann::json{{"documentId", documentId},
                                                        {"range", "A1"},
                                                        {"font", nlohmann::json{{"italic", true}}}})["ok"] == true);

    const auto imported = server->Call("copy_sheet", nlohmann::json{{"documentId", documentId},
                                                                    {"source_path", "src.xlsx"},
                                                                    {"sheet", "Imported"},
                                                                    {"name", "FromFile"}});
    REQUIRE(imported["ok"] == true);
    REQUIRE(server->Call("save_document", nlohmann::json{{"documentId", documentId}})["ok"] == true);

    auto editor = ExyokiOffice::Excel::ExcelDocumentEditor::Open(server->Path("dest.xlsx"));
    REQUIRE(editor != nullptr);
    const auto sheet = editor->GetWorksheet("FromFile");
    REQUIRE(sheet != nullptr);
    const auto bold = editor->Styles().GetCellStyle(*sheet, *ExyokiOffice::Excel::CellAddress::ParseA1("A1"));
    REQUIRE(bold.has_value());
    REQUIRE(bold->Font.has_value());
    CHECK(bold->Font->Bold);
    CHECK_FALSE(bold->Font->Italic);
    const auto red = editor->Styles().GetCellStyle(*sheet, *ExyokiOffice::Excel::CellAddress::ParseA1("B1"));
    REQUIRE(red.has_value());
    REQUIRE(red->Fill.has_value());
    REQUIRE(red->Fill->Foreground.has_value());
    CHECK(red->Fill->Foreground->Argb == "FFFF0000");

    const auto report = ExyokiOffice::Tools::Run(server->Path("dest.xlsx"));
    CHECK_MESSAGE(report.ErrorCount == 0, DescribeValidationErrors(report));

    // A name that is taken is reported as such, not as a vague failure.
    const auto taken = server->Call("copy_sheet", nlohmann::json{{"documentId", documentId},
                                                                 {"source_path", "src.xlsx"},
                                                                 {"sheet", "Imported"},
                                                                 {"name", "FromFile"}});
    CHECK(taken["ok"] == false);
    CHECK(taken["error"]["message"].get<std::string>().find("already exists") != std::string::npos);
}

TEST_CASE("X-5: images and charts keep the size they were given [mcp-excel]")
{
    auto server = MakeExcelServer();
    server->Initialize();

    const auto created = server->Call("create_document", nlohmann::json{{"path", "sized.xlsx"}});
    const auto documentId = created["data"]["documentId"].get<std::string>();
    REQUIRE(server->Call("write_range",
                         nlohmann::json{{"documentId", documentId},
                                        {"origin", "A1"},
                                        {"values", nlohmann::json::array({nlohmann::json::array({10}),
                                                                          nlohmann::json::array({20})})}})["ok"] ==
            true);

    REQUIRE(server->Call("add_image", nlohmann::json{{"documentId", documentId},
                                                     {"anchor_cell", "D2"},
                                                     {"dataBase64", OnePixelPng},
                                                     {"width", "4cm"},
                                                     {"height", "3cm"}})["ok"] == true);
    REQUIRE(server->Call("add_image", nlohmann::json{{"documentId", documentId},
                                                     {"anchor_cell", "D20"},
                                                     {"dataBase64", OnePixelPng},
                                                     {"width", 96},
                                                     {"height", "96px"}})["ok"] == true);
    REQUIRE(server->Call("add_chart", nlohmann::json{{"documentId", documentId},
                                                     {"type", "column"},
                                                     {"data_range", "A1:A2"},
                                                     {"anchor_cell", "H2"},
                                                     {"width", "15cm"},
                                                     {"height", "8cm"}})["ok"] == true);

    const auto negative = server->Call("add_image", nlohmann::json{{"documentId", documentId},
                                                                   {"anchor_cell", "D2"},
                                                                   {"dataBase64", OnePixelPng},
                                                                   {"width", -5}});
    CHECK(negative["ok"] == false);
    CHECK(negative["error"]["code"] == "input_invalid");

    REQUIRE(server->Call("save_document", nlohmann::json{{"documentId", documentId}})["ok"] == true);

    auto editor = ExyokiOffice::Excel::ExcelDocumentEditor::Open(server->Path("sized.xlsx"));
    REQUIRE(editor != nullptr);
    auto sheet = editor->FirstWorksheet();
    REQUIRE(sheet != nullptr);

    // The size travels as an exact extent, in EMU: 4 cm is 1440000, 96 pt is
    // 1219200 and 96 px is one inch. A two-cell anchor could only round it to
    // whatever the reader's default column width and row height happen to be.
    const auto images = sheet->Images();
    REQUIRE(images.size() == 2);
    REQUIRE(images[0].Extent.has_value());
    CHECK(images[0].Extent->Width == 1440000);
    CHECK(images[0].Extent->Height == 1080000);
    CHECK(images[0].From.ToA1() == "D2");
    REQUIRE(images[1].Extent.has_value());
    CHECK(images[1].Extent->Width == 1219200);
    CHECK(images[1].Extent->Height == 914400);

    const auto charts = sheet->Charts();
    REQUIRE(charts.size() == 1);
    REQUIRE(charts[0].Extent.has_value());
    CHECK(charts[0].Extent->Width == 5400000);
    CHECK(charts[0].Extent->Height == 2880000);
    CHECK(charts[0].From.ToA1() == "H2");

    const auto drawing = sheet->GetPart()->GetDrawingsPart();
    REQUIRE(drawing != nullptr);
    const auto xml = drawing->GetXmlString();
    CHECK(xml.find("oneCellAnchor") != std::string::npos);
    CHECK(xml.find("cx=\"1440000\"") != std::string::npos);
    CHECK(xml.find("cy=\"1080000\"") != std::string::npos);

    const auto report = ExyokiOffice::Tools::Run(server->Path("sized.xlsx"));
    CHECK_MESSAGE(report.ErrorCount == 0, DescribeValidationErrors(report));
}

TEST_CASE("X-6: set_print_setup fit-to-page switches the sheet's fitToPage on [mcp-excel]")
{
    namespace Spreadsheet = ExyokiOffice::DocumentFormat::OpenXml::Spreadsheet;

    auto server = MakeExcelServer();
    server->Initialize();

    const auto created = server->Call("create_document", nlohmann::json{{"path", "fit.xlsx"}});
    const auto documentId = created["data"]["documentId"].get<std::string>();

    REQUIRE(server->Call("set_print_setup", nlohmann::json{{"documentId", documentId},
                                                           {"sheet", 1},
                                                           {"fit_to_width", 1},
                                                           {"fit_to_height", 0}})["ok"] == true);
    REQUIRE(server->Call("save_document", nlohmann::json{{"documentId", documentId}})["ok"] == true);

    auto editor = ExyokiOffice::Excel::ExcelDocumentEditor::Open(server->Path("fit.xlsx"));
    REQUIRE(editor != nullptr);
    auto sheet = editor->FirstWorksheet();
    REQUIRE(sheet != nullptr);
    REQUIRE(sheet->GetPageSetup().FitToWidth == 1);

    // Excel scales to the page counts only when sheetPr says so; without the
    // switch it prints at 100% and drops the counts on its next save.
    const auto root = sheet->GetLowLevelApi();
    REQUIRE(root != nullptr);
    const auto properties = root->GetFirstChildOfType<Spreadsheet::SheetProperties>();
    REQUIRE(properties != nullptr);
    const auto pageSetUp = properties->GetFirstChildOfType<Spreadsheet::PageSetupProperties>();
    REQUIRE(pageSetUp != nullptr);
    CHECK(pageSetUp->GetFitToPage().ValueOr(false));
    // sheetPr has to be the first child for the worksheet to stay valid.
    const auto children = root->ChildrenInContentModel();
    REQUIRE_FALSE(children.empty());
    CHECK(std::dynamic_pointer_cast<Spreadsheet::SheetProperties>(children.front()) != nullptr);

    const auto report = ExyokiOffice::Tools::Run(server->Path("fit.xlsx"));
    CHECK_MESSAGE(report.ErrorCount == 0, DescribeValidationErrors(report));
}

TEST_CASE("X-7: a table slicer selection hides the rows it excludes [mcp-excel]")
{
    auto server = MakeExcelServer();
    server->Initialize();

    const auto documentId = MakeSlicerWorkbook(*server, "slicer_rows.xlsx");
    REQUIRE(server->Call("add_slicer", nlohmann::json{{"documentId", documentId},
                                                      {"source_kind", "table"},
                                                      {"source", "Sales"},
                                                      {"field", "Region"},
                                                      {"anchor_cell", "D2"},
                                                      {"name", "RegionSlicer"}})["ok"] == true);
    REQUIRE(server->Call("set_slicer_selection", nlohmann::json{{"documentId", documentId},
                                                                {"slicer", "RegionSlicer"},
                                                                {"selected", nlohmann::json::array({"North"})}})
                ["ok"] == true);
    REQUIRE(server->Call("save_document", nlohmann::json{{"documentId", documentId}})["ok"] == true);

    auto editor = ExyokiOffice::Excel::ExcelDocumentEditor::Open(server->Path("slicer_rows.xlsx"));
    REQUIRE(editor != nullptr);
    auto sheet = editor->FirstWorksheet();
    REQUIRE(sheet != nullptr);
    const auto hidden = [&sheet](ExyokiOffice::UInt32 row)
    {
        const auto dimension = sheet->GetRowDimension(row);
        return dimension.has_value() && dimension->Hidden;
    };
    // Rows 2 and 5 are North; 3 and 4 are South and East.
    CHECK_FALSE(hidden(2));
    CHECK(hidden(3));
    CHECK(hidden(4));
    CHECK_FALSE(hidden(5));

    // Selecting everything clears the filter and brings the rows back.
    REQUIRE(server->Call("set_slicer_selection", nlohmann::json{{"documentId", documentId},
                                                                {"slicer", "RegionSlicer"},
                                                                {"selected", nlohmann::json::array()}})["ok"] == true);
    REQUIRE(server->Call("save_document", nlohmann::json{{"documentId", documentId}})["ok"] == true);
    editor = ExyokiOffice::Excel::ExcelDocumentEditor::Open(server->Path("slicer_rows.xlsx"));
    REQUIRE(editor != nullptr);
    sheet = editor->FirstWorksheet();
    REQUIRE(sheet != nullptr);
    CHECK_FALSE(hidden(3));
    CHECK_FALSE(hidden(4));

    const auto report = ExyokiOffice::Tools::Run(server->Path("slicer_rows.xlsx"));
    CHECK_MESSAGE(report.ErrorCount == 0, DescribeValidationErrors(report));
}

TEST_CASE("X-8: read_range reports a formula's cached result typed like a plain cell [mcp-excel]")
{
    auto server = MakeExcelServer();
    server->Initialize();

    const auto created = server->Call("create_document", nlohmann::json::object());
    const auto documentId = created["data"]["documentId"].get<std::string>();
    REQUIRE(server->Call("write_cells",
                         nlohmann::json{{"documentId", documentId},
                                        {"cells", nlohmann::json::array({
                                            nlohmann::json{{"address", "A1"}, {"value", nlohmann::json{{"formula", "1+1"}}}},
                                            nlohmann::json{{"address", "A2"}, {"value", 2}},
                                            nlohmann::json{{"address", "A3"}, {"value", nlohmann::json{{"formula", "1>0"}}}},
                                            nlohmann::json{{"address", "A4"},
                                                           {"value", nlohmann::json{{"formula", "\"a\"&\"b\""}}}}})}})
                ["ok"] == true);
    REQUIRE(server->Call("recalculate", nlohmann::json{{"documentId", documentId}})["ok"] == true);

    const auto values = server->Call("read_range", nlohmann::json{{"documentId", documentId}, {"range", "A1:A4"}});
    REQUIRE(values["ok"] == true);
    // =1+1 and a plain 2 read the same way.
    CHECK(values["data"]["values"][0][0].is_number());
    CHECK(values["data"]["values"][0][0] == values["data"]["values"][1][0]);
    CHECK(values["data"]["values"][2][0] == true);
    CHECK(values["data"]["values"][3][0] == "ab");

    const auto cells = server->Call(
        "read_range", nlohmann::json{{"documentId", documentId}, {"range", "A1"}, {"mode", "cells"}});
    REQUIRE(cells["data"]["cells"].size() == 1);
    CHECK(cells["data"]["cells"][0]["type"] == "formula");
    CHECK(cells["data"]["cells"][0]["value"] == 2);
}

TEST_CASE("X-10: set_column_width and set_row_height refuse impossible sizes [mcp-excel]")
{
    auto server = MakeExcelServer();
    server->Initialize();

    const auto created = server->Call("create_document", nlohmann::json::object());
    const auto documentId = created["data"]["documentId"].get<std::string>();

    for (const auto width : {-5.0, 1e6})
    {
        const auto refused = server->Call(
            "set_column_width", nlohmann::json{{"documentId", documentId}, {"columns", "B"}, {"width", width}});
        CHECK(refused["ok"] == false);
        CHECK(refused["error"]["code"] == "input_invalid");
    }
    for (const auto height : {-1.0, 0.0, 500.0})
    {
        const auto refused = server->Call(
            "set_row_height", nlohmann::json{{"documentId", documentId}, {"rows", "2"}, {"height", height}});
        CHECK(refused["ok"] == false);
        CHECK(refused["error"]["code"] == "input_invalid");
    }

    // Nothing above changed the workbook.
    const auto documents = server->Call("list_documents", nlohmann::json::object());
    REQUIRE(documents["data"]["documents"].size() == 1);
    CHECK(documents["data"]["documents"][0]["dirty"] == false);

    const auto accepted = server->Call(
        "set_column_width", nlohmann::json{{"documentId", documentId}, {"columns", "B"}, {"width", 14}});
    CHECK(accepted["ok"] == true);
    CHECK(accepted["data"]["columns"] == 1);
}

TEST_CASE("X-11: the table, slicer and VBA tools answer with codes that mean what happened [mcp-excel]")
{
    auto server = MakeExcelServer();
    server->Initialize();

    const auto documentId = MakeTableWorkbook(*server, "codes.xlsx");

    const auto unknownTable = server->Call(
        "update_table", nlohmann::json{{"documentId", documentId}, {"table", "Nowhere"}, {"auto_filter", false}});
    CHECK(unknownTable["ok"] == false);
    CHECK(unknownTable["error"]["code"] == "block_not_found");

    const auto unknownSlicer = server->Call("set_slicer_selection",
                                            nlohmann::json{{"documentId", documentId},
                                                           {"slicer", "Nothing"},
                                                           {"selected", nlohmann::json::array({"North"})}});
    CHECK(unknownSlicer["ok"] == false);
    CHECK(unknownSlicer["error"]["code"] == "shape_not_found");

    const auto nothingToRemove = server->Call("remove_vba_project", nlohmann::json{{"documentId", documentId}});
    CHECK(nothingToRemove["ok"] == true);
    CHECK(nothingToRemove["data"]["removed"] == false);
}
