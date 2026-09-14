// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "doctest.h"

#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/Office2019/Excel/ThreadedComments.hpp"
#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/Spreadsheet.hpp"
#include "ExyokiOffice/Excel/ExcelDocument.hpp"
#include "ExyokiOffice/Packaging/GeneratedParts.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <algorithm>
#include <string>
#include <vector>

using namespace ExyokiOffice::Excel;

/// Helpers shared by the worksheet copy regressions.
class WorksheetCopyTestSupport final
{
public:
    WorksheetCopyTestSupport() = delete;

    static CellAddress Address(std::string_view text)
    {
        const auto value = CellAddress::ParseA1(text);
        REQUIRE(value);
        return *value;
    }

    static CellRange Range(std::string_view text)
    {
        const auto value = CellRange::ParseA1(text);
        REQUIRE(value);
        return *value;
    }

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

    /// The ids of the persons a workbook lists.
    static std::vector<std::string> PersonIds(const ExcelDocumentEditor& editor)
    {
        namespace Xltc = ExyokiOffice::DocumentFormat::OpenXml::Office2019::Excel::ThreadedComments;
        std::vector<std::string> ids;
        for (const auto& part : editor.GetDocument()->GetWorkbookPart()->GetWorkbookPersonParts())
        {
            const auto root = part ? part->GetTypedRootElement() : nullptr;
            if (!root)
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

    /// A worksheet carrying every kind of related part a copy has to reproduce.
    static void FillWithRelatedParts(const ExcelDocumentEditor::Ptr& editor, const Worksheet::Ptr& sheet)
    {
        REQUIRE(sheet->SetCellText(1, 1, "Region"));
        REQUIRE(sheet->SetCellText(1, 2, "Revenue"));
        REQUIRE(sheet->SetCellText(2, 1, "North"));
        REQUIRE(sheet->SetCellNumber(2, 2, 120.0));
        REQUIRE(sheet->SetCellText(3, 1, "South"));
        REQUIRE(sheet->SetCellNumber(3, 2, 90.0));

        ExcelWorksheetImage image;
        image.From = Address("D2");
        image.To = Address("F6");
        image.Data = {1, 2, 3, 4};
        REQUIRE(sheet->AddImage(image));

        REQUIRE(sheet->SetHyperlink({Address("A1"), "https://example.com", {}, "Example", {}}));
        REQUIRE(sheet->SetComment({Address("B2"), "Reviewer", "A plain note."}));

        ExcelThreadedComment thread;
        thread.Address = Address("B3");
        thread.PersonName = "Jakub";
        thread.Text = "A threaded one.";
        REQUIRE(sheet->AddThreadedComment(thread));

        REQUIRE(sheet->CreateTable("Sales", Range("A1:B3"), {{0, "Region"}, {0, "Revenue"}}));

        ExcelChartDefinition chart;
        chart.From = Address("D10");
        chart.To = Address("K24");
        ExcelChartSeries series;
        series.Name = "Revenue";
        series.Values = Range("B2:B3");
        series.Categories = Range("A2:A3");
        chart.Series.push_back(series);
        REQUIRE(sheet->AddChart(chart));

        REQUIRE(sheet->SetPrintArea({Range("A1:B3")}));
        static_cast<void>(editor);
    }
};

TEST_SUITE("ExcelWorksheetCopyTests")
{
    TEST_CASE("X-1: CopyWorksheet clones the sheet's part graph with fresh identities [unit] [excel] [excel-worksheets]")
    {
        auto editor = ExcelDocumentEditor::CreateNew();
        REQUIRE(editor);
        auto original = editor->FirstWorksheet();
        REQUIRE(original);
        WorksheetCopyTestSupport::FillWithRelatedParts(editor, original);

        auto copy = editor->CopyWorksheet(0, "Copy");
        REQUIRE(copy);

        auto reopened = ExcelDocumentEditor::Open(editor->SaveToMemory());
        REQUIRE(reopened);
        REQUIRE(reopened->Worksheets().size() == 2);
        const auto source = reopened->Worksheets()[0];
        const auto copied = reopened->Worksheets()[1];
        REQUIRE(copied->Name() == "Copy");

        // Every relationship id the copied sheet and its drawing refer to resolves.
        const auto copiedPart = copied->GetPart();
        const auto sheetIds = WorksheetCopyTestSupport::ReferencedRelationshipIds(copiedPart->GetXmlString());
        CHECK_MESSAGE(sheetIds.size() >= 4, copiedPart->GetXmlString().substr(0, 800));
        for (const auto& id : sheetIds)
        {
            CHECK_MESSAGE(WorksheetCopyTestSupport::HasRelationship(*copiedPart, id), "dangling sheet relationship " << id);
        }
        const auto drawing = copiedPart->GetDrawingsPart();
        REQUIRE(drawing);
        CHECK(drawing != source->GetPart()->GetDrawingsPart());
        for (const auto& id : WorksheetCopyTestSupport::ReferencedRelationshipIds(drawing->GetXmlString()))
        {
            CHECK_MESSAGE(WorksheetCopyTestSupport::HasRelationship(*drawing, id), "dangling drawing relationship " << id);
        }

        // Media is shared rather than duplicated; everything else is a part of its own.
        REQUIRE(copied->Images().size() == 1);
        CHECK(copied->Images()[0].Data == source->Images()[0].Data);
        CHECK(drawing->GetImageParts().size() == 1);
        CHECK(drawing->GetImageParts()[0] == source->GetPart()->GetDrawingsPart()->GetImageParts()[0]);
        CHECK(copied->Charts().size() == 1);
        CHECK(copied->Comments().size() == 1);
        REQUIRE(copied->Hyperlinks().size() == 1);
        CHECK(copied->Hyperlinks()[0].Target == "https://example.com");

        // Tables and threads are identified workbook-wide, so the copies get new ids.
        REQUIRE(copied->Tables().size() == 1);
        CHECK(copied->Tables()[0]->Id() != source->Tables()[0]->Id());
        CHECK(copied->Tables()[0]->Name() != source->Tables()[0]->Name());
        CHECK(copied->Tables()[0]->Range()->ToA1() == "A1:B3");
        REQUIRE(copied->ThreadedComments().size() == 1);
        CHECK(copied->ThreadedComments()[0].Id != source->ThreadedComments()[0].Id);
        CHECK(copied->ThreadedComments()[0].PersonId == source->ThreadedComments()[0].PersonId);
        CHECK(copied->ThreadedComments()[0].Text == "A threaded one.");

        // The sheet-scoped print area follows the copy under its own name.
        REQUIRE(copied->GetPrintArea().size() == 1);
        CHECK(copied->GetPrintArea()[0].ToA1() == "A1:B3");
        REQUIRE(source->GetPrintArea().size() == 1);

        // Editing the copy leaves the original alone.
        REQUIRE(copied->SetCellText(2, 1, "West"));
        CHECK(source->GetCellValue(WorksheetCopyTestSupport::Address("A2")).has_value());
        CHECK(reopened->SharedStrings().Lookup(*source->GetCellValue(WorksheetCopyTestSupport::Address("A2"))->SharedStringIndex()) ==
              "North");
    }

    TEST_CASE("X-2: CreateTable refuses a range that overlaps an existing table [unit] [excel] [excel-table]")
    {
        auto editor = ExcelDocumentEditor::CreateNew();
        auto sheet = editor->FirstWorksheet();
        REQUIRE(sheet);
        REQUIRE(sheet->SetCellNumber(2, 2, 5.0));
        REQUIRE(sheet->CreateTable("TblOne", WorksheetCopyTestSupport::Range("A1:B3"), {{0, "a"}, {0, "b"}}));

        CHECK(sheet->CreateTable("TblTwo", WorksheetCopyTestSupport::Range("B2:C4"), {{0, "x"}, {0, "y"}}) ==
              nullptr);
        CHECK(sheet->CreateTable("TblTwo", WorksheetCopyTestSupport::Range("A1:B3"), {{0, "x"}, {0, "y"}}) ==
              nullptr);
        // Nothing was written: one table and B2 still holds its number.
        CHECK(sheet->Tables().size() == 1);
        CHECK(sheet->GetCellValue(WorksheetCopyTestSupport::Address("B2"))->Text() == "5");

        // A range next to the table is still fine.
        CHECK(sheet->CreateTable("TblTwo", WorksheetCopyTestSupport::Range("D1:E3"), {{0, "x"}, {0, "y"}}));
    }

    TEST_CASE("X-3: CopyWorksheetFrom brings the persons of imported threads along [unit] [excel] [excel-worksheets]")
    {
        auto source = ExcelDocumentEditor::CreateNew();
        auto target = ExcelDocumentEditor::CreateNew();
        REQUIRE(source);
        REQUIRE(target);
        ExcelThreadedComment thread;
        thread.Address = WorksheetCopyTestSupport::Address("B2");
        thread.PersonName = "Reviewer";
        thread.PersonEmail = "reviewer@example.com";
        thread.Text = "Check this.";
        REQUIRE(source->FirstWorksheet()->AddThreadedComment(thread));
        const auto personId = source->FirstWorksheet()->ThreadedComments()[0].PersonId;

        auto imported = target->CopyWorksheetFrom(*source, 0, "Imported");
        REQUIRE(imported);

        auto reopened = ExcelDocumentEditor::Open(target->SaveToMemory());
        REQUIRE(reopened);
        const auto sheet = reopened->GetWorksheet("Imported");
        REQUIRE(sheet);
        REQUIRE(sheet->ThreadedComments().size() == 1);
        CHECK(sheet->ThreadedComments()[0].PersonId == personId);
        CHECK(sheet->ThreadedComments()[0].PersonName == "Reviewer");
        CHECK(sheet->ThreadedComments()[0].PersonEmail == "reviewer@example.com");
        const auto persons = WorksheetCopyTestSupport::PersonIds(*reopened);
        CHECK(std::find(persons.begin(), persons.end(), personId) != persons.end());
        CHECK(reopened->GetDocument()->GetWorkbookPart()->GetWorkbookPersonParts().size() == 1);

        // A second import of the same person does not list them twice.
        REQUIRE(target->CopyWorksheetFrom(*source, 0, "Again"));
        CHECK(WorksheetCopyTestSupport::PersonIds(*target).size() == 1);
    }

    TEST_CASE("X-4: CopyWorksheetFrom remaps styles into a different style catalog [unit] [excel] [excel-worksheets]")
    {
        auto source = ExcelDocumentEditor::CreateNew();
        auto target = ExcelDocumentEditor::CreateNew();
        REQUIRE(source);
        REQUIRE(target);

        ExcelStyle bold;
        bold.Font = ExcelFont{};
        bold.Font->Bold = true;
        ExcelStyle red;
        red.Fill = ExcelFill{};
        red.Fill->Pattern = ExcelFillPattern::Solid;
        red.Fill->Foreground = ExcelColor::Rgb("FFFF0000");
        ExcelStyle italic;
        italic.Font = ExcelFont{};
        italic.Font->Italic = true;

        // The target already has a style at index 1, so the source's index 1
        // (bold) would land on italic if it were carried over unchanged.
        REQUIRE(target->Styles().GetOrAdd(italic));
        const auto boldIndex = source->Styles().GetOrAdd(bold);
        const auto redIndex = source->Styles().GetOrAdd(red);
        REQUIRE(boldIndex);
        REQUIRE(redIndex);
        auto sourceSheet = source->FirstWorksheet();
        REQUIRE(source->Styles().ApplyToCell(*sourceSheet, WorksheetCopyTestSupport::Address("A1"), boldIndex.StyleIndex));
        REQUIRE(source->Styles().ApplyToCell(*sourceSheet, WorksheetCopyTestSupport::Address("B1"), redIndex.StyleIndex));
        REQUIRE(sourceSheet->SetCellText(1, 1, "Bold"));

        ExcelStyle highlight;
        highlight.Fill = ExcelFill{};
        highlight.Fill->Pattern = ExcelFillPattern::Solid;
        highlight.Fill->Foreground = ExcelColor::Rgb("FF00FF00");
        const auto dxf = source->Styles().GetOrAddDifferentialFormat(highlight);
        REQUIRE(dxf);
        auto rule = ExcelConditionalFormattingDefinition::Expression({WorksheetCopyTestSupport::Range("A1:B1")},
                                                                     "TRUE");
        rule.DifferentialFormatId = dxf.StyleIndex;
        REQUIRE(sourceSheet->CreateConditionalFormatting(rule));

        auto imported = target->CopyWorksheetFrom(*source, 0, "Imported");
        REQUIRE(imported);

        auto reopened = ExcelDocumentEditor::Open(target->SaveToMemory());
        REQUIRE(reopened);
        const auto sheet = reopened->GetWorksheet("Imported");
        REQUIRE(sheet);
        const auto a1 = reopened->Styles().GetCellStyle(*sheet, WorksheetCopyTestSupport::Address("A1"));
        REQUIRE(a1);
        REQUIRE(a1->Font);
        CHECK(a1->Font->Bold);
        CHECK_FALSE(a1->Font->Italic);
        const auto b1 = reopened->Styles().GetCellStyle(*sheet, WorksheetCopyTestSupport::Address("B1"));
        REQUIRE(b1);
        REQUIRE(b1->Fill);
        REQUIRE(b1->Fill->Foreground);
        CHECK(b1->Fill->Foreground->Argb == "FFFF0000");

        const auto rules = sheet->ConditionalFormattings();
        REQUIRE(rules.size() == 1);
        const auto definition = rules[0]->Definition();
        REQUIRE(definition.DifferentialFormatId);
        const auto painted = reopened->Styles().GetDifferentialFormat(*definition.DifferentialFormatId);
        REQUIRE(painted);
        REQUIRE(painted->Fill);
        REQUIRE((painted->Fill->Foreground.has_value() || painted->Fill->Background.has_value()));
        const auto colour = painted->Fill->Foreground ? painted->Fill->Foreground : painted->Fill->Background;
        CHECK(colour->Argb == "FF00FF00");
    }

    TEST_CASE("X-5: DrawingAnchorForSize walks the real column widths and row heights [unit] [excel] [layout]")
    {
        auto editor = ExcelDocumentEditor::CreateNew();
        auto sheet = editor->FirstWorksheet();
        REQUIRE(sheet);
        const auto from = WorksheetCopyTestSupport::Address("B2");

        // Two default columns (64 px each) and ten pixels more; three default rows exactly.
        constexpr ExyokiOffice::Int64 pixel = 9525;
        constexpr ExyokiOffice::Int64 point = 12700;
        auto anchor = sheet->DrawingAnchorForSize(from, ExyokiOffice::MeasuringUnits(static_cast<ExyokiOffice::Real>(138 * pixel)),
                                                  ExyokiOffice::MeasuringUnits(45.0, ExyokiOffice::MeasurementUnit::Point));
        REQUIRE(anchor);
        CHECK(anchor->From.ToA1() == "B2");
        CHECK(anchor->To.ToA1() == "D5");
        CHECK(anchor->ToOffset.Column == 10 * pixel);
        CHECK(anchor->ToOffset.Row == 0);
        REQUIRE(anchor->Extent);
        CHECK(anchor->Extent->Width == 138 * pixel);
        CHECK(anchor->Extent->Height == 45 * point);

        // A wider column B and a taller row 2 change where the far edge lands.
        REQUIRE(sheet->SetColumnDimension(2, ColumnDimension{.Width = 20.0}));
        REQUIRE(sheet->SetRowDimension(2, RowDimension{.Height = 30.0}));
        anchor = sheet->DrawingAnchorForSize(from, ExyokiOffice::MeasuringUnits(static_cast<ExyokiOffice::Real>(150 * pixel)),
                                             ExyokiOffice::MeasuringUnits(40.0, ExyokiOffice::MeasurementUnit::Point));
        REQUIRE(anchor);
        // 20 characters are 140 px, so 10 px spill into column C; 30 + 15 = 45 pt cover 40 pt within row 3.
        CHECK(anchor->To.ToA1() == "C3");
        CHECK(anchor->ToOffset.Column == 10 * pixel);
        CHECK(anchor->ToOffset.Row == 10 * point);

        CHECK_FALSE(sheet->DrawingAnchorForSize(from, ExyokiOffice::MeasuringUnits(-1.0, ExyokiOffice::MeasurementUnit::Point),
                                                ExyokiOffice::MeasuringUnits(1.0, ExyokiOffice::MeasurementUnit::Point)));
    }

    TEST_CASE("X-5: an image or chart with an extent is written as a one-cell anchor of that size [unit] [excel] [layout]")
    {
        auto editor = ExcelDocumentEditor::CreateNew();
        auto sheet = editor->FirstWorksheet();
        REQUIRE(sheet);
        REQUIRE(sheet->SetCellNumber(1, 1, 1.0));
        REQUIRE(sheet->SetCellNumber(2, 1, 2.0));

        ExcelWorksheetImage image;
        image.From = WorksheetCopyTestSupport::Address("D2");
        image.Extent = DrawingExtent{1440000, 1080000};
        image.Data = {1, 2, 3};
        REQUIRE(sheet->AddImage(image));

        ExcelChartDefinition chart;
        chart.From = WorksheetCopyTestSupport::Address("H2");
        chart.Extent = DrawingExtent{5400000, 2880000};
        ExcelChartSeries series;
        series.Values = WorksheetCopyTestSupport::Range("A1:A2");
        chart.Series.push_back(series);
        const auto chartId = sheet->AddChart(chart);
        REQUIRE(chartId);

        auto reopened = ExcelDocumentEditor::Open(editor->SaveToMemory());
        REQUIRE(reopened);
        sheet = reopened->FirstWorksheet();
        REQUIRE(sheet->Images().size() == 1);
        REQUIRE(sheet->Images()[0].Extent);
        CHECK(sheet->Images()[0].Extent->Width == 1440000);
        CHECK(sheet->Images()[0].Extent->Height == 1080000);
        CHECK(sheet->Images()[0].From.ToA1() == "D2");
        REQUIRE(sheet->Charts().size() == 1);
        REQUIRE(sheet->Charts()[0].Extent);
        CHECK(sheet->Charts()[0].Extent->Width == 5400000);
        CHECK(sheet->Charts()[0].From.ToA1() == "H2");

        const auto xml = sheet->GetPart()->GetDrawingsPart()->GetXmlString();
        CHECK(xml.find("oneCellAnchor") != std::string::npos);
        CHECK(xml.find("twoCellAnchor") == std::string::npos);

        // The chart can still be updated and removed through its one-cell anchor.
        auto updated = sheet->Charts()[0];
        updated.Title = "Renamed";
        updated.Extent = DrawingExtent{2000000, 1000000};
        CHECK(sheet->UpdateChart(updated));
        CHECK(sheet->Charts()[0].Extent->Width == 2000000);
        CHECK(sheet->RemoveChart(*chartId));
        CHECK(sheet->Charts().empty());
        CHECK(sheet->RemoveImage(sheet->Images()[0].Id));
        CHECK(sheet->Images().empty());
    }

    TEST_CASE("X-6: fit-to-page page setup switches sheetPr/pageSetUpPr on and off [unit] [excel] [print]")
    {
        namespace Spreadsheet = ExyokiOffice::DocumentFormat::OpenXml::Spreadsheet;
        auto editor = ExcelDocumentEditor::CreateNew();
        auto sheet = editor->FirstWorksheet();
        REQUIRE(sheet);
        REQUIRE(sheet->SetCellText(1, 1, "data"));

        PageSetup setup;
        setup.FitToWidth = 1;
        setup.FitToHeight = 0;
        REQUIRE(sheet->SetPageSetup(setup));

        const auto fitToPage = [&sheet]()
        {
            const auto root = sheet->GetLowLevelApi();
            const auto properties = root ? root->GetFirstChildOfType<Spreadsheet::SheetProperties>() : nullptr;
            const auto pageSetUp = properties ? properties->GetFirstChildOfType<Spreadsheet::PageSetupProperties>()
                                              : nullptr;
            return pageSetUp && pageSetUp->GetFitToPage().ValueOr(false);
        };
        CHECK(fitToPage());
        // sheetPr is the worksheet's first child.
        const auto children = sheet->GetLowLevelApi()->ChildrenInContentModel();
        REQUIRE_FALSE(children.empty());
        CHECK(std::dynamic_pointer_cast<Spreadsheet::SheetProperties>(children.front()) != nullptr);

        auto reopened = ExcelDocumentEditor::Open(editor->SaveToMemory());
        REQUIRE(reopened);
        sheet = reopened->FirstWorksheet();
        CHECK(fitToPage());
        CHECK(sheet->GetPageSetup().FitToWidth == 1);

        // Going back to a plain scale turns the switch off again.
        PageSetup scaled;
        scaled.Scale = 80;
        REQUIRE(sheet->SetPageSetup(scaled));
        CHECK_FALSE(fitToPage());
    }
}
