// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "doctest.h"

#include "TestSupport.hpp"

#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/Drawing.hpp"
#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/Drawing/Spreadsheet.hpp"
#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/Office2010/Drawing/Slicer.hpp"
#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/Office2010/Excel.hpp"
#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/Office2013/Excel.hpp"
#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/Spreadsheet.hpp"
#include "ExyokiOffice/Excel/ExcelDocument.hpp"
#include "ExyokiOffice/OpenXmlDomValidator.hpp"
#include "ExyokiOffice/Packaging/GeneratedParts.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <algorithm>
#include <string>

using namespace ExyokiOffice::Excel;

namespace ExcelSlicerTestHelpers
{

namespace S = ExyokiOffice::DocumentFormat::OpenXml::Spreadsheet;
namespace X14 = ExyokiOffice::DocumentFormat::OpenXml::Office2010::Excel;
namespace X15 = ExyokiOffice::DocumentFormat::OpenXml::Office2013::Excel;
namespace A = ExyokiOffice::DocumentFormat::OpenXml::Drawing;
namespace XDR = ExyokiOffice::DocumentFormat::OpenXml::Drawing::Spreadsheet;
namespace SLE = ExyokiOffice::DocumentFormat::OpenXml::Office2010::Drawing::Slicer;

/** The four well-known extension URIs, asserted literally by the tests. */
constexpr const char* kWorkbookSlicerCachesUri = "{BBE1A952-AA13-448e-AADC-164F8A28A991}";
constexpr const char* kWorkbookSlicerCachesX15Uri = "{46BE6895-7355-4a93-B00E-2C351335B9C9}";
constexpr const char* kWorksheetSlicerListUri = "{A8765BA9-456A-4dab-B4F3-ACF1056F45CF}";
constexpr const char* kTableSlicerCacheUri = "{2F2917AC-EB37-4324-AD4E-5DD8C200BD13}";
constexpr const char* kSlicerGraphicUri = "http://schemas.microsoft.com/office/drawing/2010/slicer";

CellAddress Address(std::string_view text)
{
    const auto value = CellAddress::ParseA1(text);
    REQUIRE(value);
    return *value;
}

CellRange Range(std::string_view text)
{
    const auto value = CellRange::ParseA1(text);
    REQUIRE(value);
    return *value;
}

/**
 * Builds a workbook with a "Data" sheet holding the sample sales table and an
 * empty "Report" sheet:
 *
 *     Region | Quarter | Amount | Units
 *     East   | Q1      |     10 |     1
 *     East   | Q2      |     20 |     2
 *     West   | Q1      |     30 |     3
 *     West   | Q2      |     40 |     4
 *     East   | Q1      |      5 |     5
 */
ExcelDocumentEditor::Ptr MakeWorkbook()
{
    auto editor = ExcelDocumentEditor::CreateNew();
    REQUIRE(editor);
    REQUIRE(editor->RenameWorksheet(0, "Data"));
    auto data = editor->FirstWorksheet();
    REQUIRE(data);

    const char* headers[] = {"Region", "Quarter", "Amount", "Units"};
    for (ExyokiOffice::UInt32 column = 0; column < 4; ++column)
    {
        REQUIRE(data->SetCellText(1, column + 1, headers[column]));
    }
    struct Row
    {
        const char* region;
        const char* quarter;
        ExyokiOffice::Real amount;
        ExyokiOffice::Real units;
    };
    const Row rows[] = {{"East", "Q1", 10, 1},
                        {"East", "Q2", 20, 2},
                        {"West", "Q1", 30, 3},
                        {"West", "Q2", 40, 4},
                        {"East", "Q1", 5, 5}};
    ExyokiOffice::UInt32 row = 2;
    for (const auto& entry : rows)
    {
        REQUIRE(data->SetCellText(row, 1, entry.region));
        REQUIRE(data->SetCellText(row, 2, entry.quarter));
        REQUIRE(data->SetCellNumber(row, 3, entry.amount));
        REQUIRE(data->SetCellNumber(row, 4, entry.units));
        ++row;
    }
    REQUIRE(editor->AddWorksheet("Report"));
    return editor;
}

/** Adds the "SalesByRegion" pivot table to the "Report" sheet. */
ExcelPivotTable::Ptr AddPivotTable(const ExcelDocumentEditor::Ptr& editor)
{
    auto report = editor->GetWorksheet("Report");
    REQUIRE(report);
    ExcelPivotTableDefinition definition;
    definition.Name = "SalesByRegion";
    definition.SourceSheet = "Data";
    definition.SourceRange = Range("A1:D6");
    definition.TargetCell = Address("A1");
    definition.Fields = {{"Region", PivotAxis::Row}, {"Quarter", PivotAxis::Column}};
    definition.DataFields.push_back(ExcelPivotDataField{"Amount"});
    auto created = report->CreatePivotTable(definition);
    INFO("pivot: ", created.Status.Message);
    REQUIRE(created.PivotTable);
    return created.PivotTable;
}

/** Adds the "SalesTable" worksheet table over the sample data. */
ExcelTable::Ptr AddTable(const ExcelDocumentEditor::Ptr& editor)
{
    auto data = editor->GetWorksheet("Data");
    REQUIRE(data);
    const std::vector<ExcelTableColumn> columns = {
        {0, "Region", {}, {}, {}, TableTotalsFunction::None},
        {0, "Quarter", {}, {}, {}, TableTotalsFunction::None},
        {0, "Amount", {}, {}, {}, TableTotalsFunction::None},
        {0, "Units", {}, {}, {}, TableTotalsFunction::None}};
    auto table = data->CreateTable("SalesTable", Range("A1:D6"), columns);
    REQUIRE(table);
    return table;
}

ExcelSlicerDefinition BasicPivotSlicer()
{
    ExcelSlicerDefinition definition;
    definition.Name = "RegionSlicer";
    definition.SourceKind = SlicerSourceKind::PivotTable;
    definition.PivotTableName = "SalesByRegion";
    definition.SourceField = "Region";
    definition.From = Address("F2");
    definition.To = Address("H12");
    return definition;
}

ExcelSlicerDefinition BasicTableSlicer()
{
    ExcelSlicerDefinition definition;
    definition.Name = "QuarterSlicer";
    definition.SourceKind = SlicerSourceKind::Table;
    definition.TableName = "SalesTable";
    definition.SourceField = "Quarter";
    definition.From = Address("F2");
    definition.To = Address("H12");
    return definition;
}

std::string Describe(const ExyokiOffice::ValidationResult& result)
{
    std::string text;
    for (const auto& issue : result.Issues())
    {
        text += issue.Message;
        text += '\n';
    }
    return text;
}

/** Returns the `x:ext` with @p uri from a workbook or worksheet root. */
template <typename TList, typename TExtension>
std::shared_ptr<TExtension> FindExtension(const std::shared_ptr<ExyokiOffice::OpenXMLElement>& owner,
                                          std::string_view uri)
{
    const auto list = owner ? owner->GetFirstChildOfType<TList>() : nullptr;
    if (!list)
    {
        return nullptr;
    }
    for (const auto& extension : list->template Elements<TExtension>())
    {
        if (extension && extension->GetUri().ToString() == uri)
        {
            return extension;
        }
    }
    return nullptr;
}

std::shared_ptr<XDR::WorksheetDrawing> DrawingOf(const Worksheet::Ptr& sheet)
{
    const auto part = sheet ? sheet->GetPart() : nullptr;
    const auto drawing = part ? part->GetDrawingsPart() : nullptr;
    return drawing ? drawing->GetWorksheetDrawing() : nullptr;
}

/** Walks an anchor down to the `sle:slicer` element it carries. */
std::shared_ptr<SLE::Slicer> AnchorSlicer(const std::shared_ptr<XDR::TwoCellAnchor>& anchor)
{
    const auto frame = anchor ? anchor->GetFirstChildOfType<XDR::GraphicFrame>() : nullptr;
    const auto graphic = frame ? frame->GetFirstChildOfType<A::Graphic>() : nullptr;
    const auto data = graphic ? graphic->GetFirstChildOfType<A::GraphicData>() : nullptr;
    return data ? data->GetFirstChildOfType<SLE::Slicer>() : nullptr;
}

} // namespace ExcelSlicerTestHelpers

using namespace ExcelSlicerTestHelpers;

TEST_SUITE("ExcelSlicerTests")
{

    TEST_CASE("CreateSlicer over a pivot field builds cache, slicers part, and both registries "
              "[unit] [excel] [excel-slicer]")
    {
        auto editor = MakeWorkbook();
        AddPivotTable(editor);
        auto report = editor->GetWorksheet("Report");

        auto created = report->CreateSlicer(BasicPivotSlicer());
        INFO("slicer: ", created.Status.Message);
        auto slicer = created.Slicer;
        REQUIRE(slicer);
        CHECK(created.Status.Succeeded());

        const auto workbookPart = editor->GetDocument()->GetWorkbookPart();
        REQUIRE(workbookPart);
        CHECK(workbookPart->GetSlicerCacheParts().size() == 1);
        CHECK(report->GetPart()->GetSlicersParts().size() == 1);

        const auto workbook = workbookPart->GetTypedRootElement();
        const auto workbookExt =
            FindExtension<S::WorkbookExtensionList, S::WorkbookExtension>(workbook, kWorkbookSlicerCachesUri);
        REQUIRE(workbookExt);
        CHECK(workbookExt->GetFirstChildOfType<X14::SlicerCaches>() != nullptr);

        const auto worksheetExt = FindExtension<S::WorksheetExtensionList, S::WorksheetExtension>(
            report->GetLowLevelApi(), kWorksheetSlicerListUri);
        REQUIRE(worksheetExt);
        CHECK(worksheetExt->GetFirstChildOfType<X14::SlicerList>() != nullptr);
    }

    TEST_CASE("Pivot slicer cache carries name, sourceName, pivot table, and tabular items "
              "[unit] [excel] [excel-slicer]")
    {
        auto editor = MakeWorkbook();
        AddPivotTable(editor);
        auto report = editor->GetWorksheet("Report");
        auto slicer = report->CreateSlicer(BasicPivotSlicer()).Slicer;
        REQUIRE(slicer);

        const auto cachePart = slicer->GetCachePart();
        REQUIRE(cachePart);
        const auto root = cachePart->GetSlicerCacheDefinition();
        REQUIRE(root);
        CHECK(root->GetName().ToString() == "Slicer_Region");
        CHECK(root->GetSourceName().ToString() == "Region");

        const auto pivotTables = root->GetFirstChildOfType<X14::SlicerCachePivotTables>();
        REQUIRE(pivotTables);
        const auto entry = pivotTables->GetFirstChildOfType<X14::SlicerCachePivotTable>();
        REQUIRE(entry);
        CHECK(entry->GetName().ToString() == "SalesByRegion");
        CHECK(entry->GetTabId().ValueOr(0) == 2);

        const auto data = root->GetFirstChildOfType<X14::SlicerCacheData>();
        REQUIRE(data);
        const auto tabular = data->GetFirstChildOfType<X14::TabularSlicerCache>();
        REQUIRE(tabular);
        CHECK(tabular->GetPivotCacheId().ValueOr(0) == 1);

        const auto items = tabular->GetFirstChildOfType<X14::TabularSlicerCacheItems>();
        REQUIRE(items);
        const auto entries = items->Elements<X14::TabularSlicerCacheItem>();
        CHECK(entries.size() == 2);
        CHECK(items->GetCount().ValueOr(0) == 2);
        CHECK(entries[0]->GetAtom().ValueOr(99) == 0);
        CHECK(entries[1]->GetAtom().ValueOr(99) == 1);
    }

    TEST_CASE("Slicer element carries the required name, cache, and rowHeight attributes "
              "[unit] [excel] [excel-slicer]")
    {
        auto editor = MakeWorkbook();
        AddPivotTable(editor);
        auto report = editor->GetWorksheet("Report");
        auto slicer = report->CreateSlicer(BasicPivotSlicer()).Slicer;
        REQUIRE(slicer);

        const auto element = slicer->GetLowLevelApi();
        REQUIRE(element);
        CHECK(element->GetName().ToString() == "RegionSlicer");
        CHECK(element->GetCache().ToString() == "Slicer_Region");
        CHECK(element->GetCaption().ToString() == "Region");
        // rowHeight is a required attribute; omitting it makes the part invalid.
        CHECK(element->GetRowHeight().IsDefined());
        CHECK(element->GetRowHeight().ValueOr(0) == 241300);

        const auto xml = slicer->GetPart()->GetXmlString();
        CHECK(xml.find("rowHeight=") != std::string::npos);
        CHECK(xml.find("cache=\"Slicer_Region\"") != std::string::npos);
    }

    TEST_CASE("Slicer elements resolve as Slicer and slicer list entries as SlicerRef "
              "[unit] [excel] [excel-slicer]")
    {
        auto editor = MakeWorkbook();
        AddPivotTable(editor);
        auto report = editor->GetWorksheet("Report");
        auto slicer = report->CreateSlicer(BasicPivotSlicer()).Slicer;
        REQUIRE(slicer);

        // The qualified name {x14, slicer} denotes two different classes, and a
        // typed lookup by either class matches the same node. What distinguishes
        // them is the content the code writes, so assert on that: a slicers part
        // holds definitions with a name and a cache and no relationship, while a
        // slicer list holds references that carry only a relationship id.
        const auto slicersRoot = slicer->GetPart()->GetSlicers();
        REQUIRE(slicersRoot);
        const auto definitions = slicersRoot->Elements<X14::Slicer>();
        REQUIRE(definitions.size() == 1);
        CHECK(definitions[0]->GetName().ToString() == "RegionSlicer");
        CHECK(definitions[0]->GetCache().ToString() == "Slicer_Region");
        CHECK_FALSE(slicersRoot->Elements<X14::SlicerRef>()[0]->GetId().IsDefined());

        const auto worksheetExt = FindExtension<S::WorksheetExtensionList, S::WorksheetExtension>(
            report->GetLowLevelApi(), kWorksheetSlicerListUri);
        const auto list = worksheetExt ? worksheetExt->GetFirstChildOfType<X14::SlicerList>() : nullptr;
        REQUIRE(list);
        const auto references = list->Elements<X14::SlicerRef>();
        REQUIRE(references.size() == 1);
        CHECK(references[0]->GetId().ToString() == slicer->GetPart()->RelationshipId());
        CHECK_FALSE(list->Elements<X14::Slicer>()[0]->GetName().IsDefined());
    }

    TEST_CASE("CreateSlicer writes a two-cell anchor carrying an sle:slicer graphic frame "
              "[unit] [excel] [excel-slicer]")
    {
        auto editor = MakeWorkbook();
        AddPivotTable(editor);
        auto report = editor->GetWorksheet("Report");
        auto slicer = report->CreateSlicer(BasicPivotSlicer()).Slicer;
        REQUIRE(slicer);

        const auto drawing = DrawingOf(report);
        REQUIRE(drawing);
        const auto anchors = drawing->Elements<XDR::TwoCellAnchor>();
        REQUIRE(anchors.size() == 1);

        const auto frame = anchors[0]->GetFirstChildOfType<XDR::GraphicFrame>();
        const auto graphic = frame ? frame->GetFirstChildOfType<A::Graphic>() : nullptr;
        const auto data = graphic ? graphic->GetFirstChildOfType<A::GraphicData>() : nullptr;
        REQUIRE(data);
        CHECK(data->GetUri().ToString() == kSlicerGraphicUri);

        const auto shape = data->GetFirstChildOfType<SLE::Slicer>();
        REQUIRE(shape);
        CHECK(shape->GetName().ToString() == "RegionSlicer");

        const auto anchor = slicer->Anchor();
        REQUIRE(anchor);
        CHECK(anchor->first.ToA1() == "F2");
        CHECK(anchor->second.ToA1() == "H12");
    }

    TEST_CASE("CreateSlicer with writeDrawing disabled creates no drawing part "
              "[unit] [excel] [excel-slicer]")
    {
        auto editor = MakeWorkbook();
        AddPivotTable(editor);
        auto report = editor->GetWorksheet("Report");

        auto definition = BasicPivotSlicer();
        definition.WriteDrawing = false;
        auto slicer = report->CreateSlicer(definition).Slicer;
        REQUIRE(slicer);

        CHECK(report->GetPart()->GetDrawingsPart() == nullptr);
        CHECK_FALSE(slicer->Anchor().has_value());
        CHECK(report->GetPart()->GetSlicersParts().size() == 1);
    }

    TEST_CASE("Table slicer writes an x15 tableSlicerCache extension and no tabular data "
              "[unit] [excel] [excel-slicer]")
    {
        auto editor = MakeWorkbook();
        auto table = AddTable(editor);
        auto data = editor->GetWorksheet("Data");

        auto created = data->CreateSlicer(BasicTableSlicer());
        INFO("slicer: ", created.Status.Message);
        auto slicer = created.Slicer;
        REQUIRE(slicer);

        const auto root = slicer->GetCachePart()->GetSlicerCacheDefinition();
        REQUIRE(root);
        CHECK(root->GetName().ToString() == "Slicer_Quarter");
        CHECK(root->GetSourceName().ToString() == "Quarter");
        // The tabular cache requires a pivot cache id, so a table slicer must
        // not have one at all.
        CHECK(root->GetFirstChildOfType<X14::SlicerCacheData>() == nullptr);

        const auto extension =
            FindExtension<X14::SlicerCacheDefinitionExtensionList, S::SlicerCacheDefinitionExtension>(
                root, kTableSlicerCacheUri);
        REQUIRE(extension);
        const auto tableCache = extension->GetFirstChildOfType<X15::TableSlicerCache>();
        REQUIRE(tableCache);
        CHECK(tableCache->GetTableId().ValueOr(0) == table->Id());
        CHECK(tableCache->GetColumn().ValueOr(0) == table->Columns()[1].Id);

        CHECK(slicer->SourceKind() == SlicerSourceKind::Table);
        CHECK(slicer->SourceObjectName() == "SalesTable");
    }

    TEST_CASE("Table slicer registers its cache in the x15 workbook extension "
              "[unit] [excel] [excel-slicer]")
    {
        auto editor = MakeWorkbook();
        AddTable(editor);
        auto data = editor->GetWorksheet("Data");
        REQUIRE(data->CreateSlicer(BasicTableSlicer()).Slicer);

        const auto workbook = editor->GetDocument()->GetWorkbookPart()->GetTypedRootElement();
        const auto x15Ext =
            FindExtension<S::WorkbookExtensionList, S::WorkbookExtension>(workbook, kWorkbookSlicerCachesX15Uri);
        REQUIRE(x15Ext);
        const auto caches = x15Ext->GetFirstChildOfType<X15::SlicerCaches>();
        REQUIRE(caches);
        CHECK(caches->Elements<X14::SlicerCache>().size() == 1);

        // A table slicer must not appear in the Excel 2010 registry.
        CHECK(FindExtension<S::WorkbookExtensionList, S::WorkbookExtension>(workbook, kWorkbookSlicerCachesUri) ==
              nullptr);
    }

    TEST_CASE("Table slicer selection is stored as a table value filter [unit] [excel] [excel-slicer]")
    {
        auto editor = MakeWorkbook();
        auto table = AddTable(editor);
        auto data = editor->GetWorksheet("Data");
        auto slicer = data->CreateSlicer(BasicTableSlicer()).Slicer;
        REQUIRE(slicer);

        CHECK(table->ValueFilters().empty());
        const auto status = slicer->SelectItems({"Q1"});
        INFO("select: ", status.Message);
        CHECK(status.Succeeded());

        const auto filters = table->ValueFilters();
        REQUIRE(filters.size() == 1);
        CHECK(filters[0].ColumnIndex == 1);
        REQUIRE(filters[0].Values.size() == 1);
        CHECK(filters[0].Values[0] == "Q1");

        const auto items = slicer->Items();
        REQUIRE(items.size() == 2);
        CHECK(items[0].Caption == "Q1");
        CHECK(items[0].Selected);
        CHECK(items[1].Caption == "Q2");
        CHECK_FALSE(items[1].Selected);
    }

    TEST_CASE("SelectItems marks the chosen pivot items and clears the rest "
              "[unit] [excel] [excel-slicer]")
    {
        auto editor = MakeWorkbook();
        AddPivotTable(editor);
        auto report = editor->GetWorksheet("Report");
        auto slicer = report->CreateSlicer(BasicPivotSlicer()).Slicer;
        REQUIRE(slicer);

        SUBCASE("an empty selection omits the attribute entirely")
        {
            // "Select all" is the absence of @s, not @s="1" on every item.
            const auto xml = slicer->GetCachePart()->GetXmlString();
            CHECK(xml.find(" s=") == std::string::npos);
            const auto items = slicer->Items();
            REQUIRE(items.size() == 2);
            CHECK(items[0].Selected);
            CHECK(items[1].Selected);
        }

        SUBCASE("an explicit selection writes both the selected and the cleared items")
        {
            const auto status = slicer->SelectItems({"East"});
            INFO("select: ", status.Message);
            REQUIRE(status.Succeeded());

            const auto root = slicer->GetCachePart()->GetSlicerCacheDefinition();
            const auto data = root->GetFirstChildOfType<X14::SlicerCacheData>();
            const auto tabular = data->GetFirstChildOfType<X14::TabularSlicerCache>();
            const auto items = tabular->GetFirstChildOfType<X14::TabularSlicerCacheItems>();
            REQUIRE(items);
            const auto entries = items->Elements<X14::TabularSlicerCacheItem>();
            REQUIRE(entries.size() == 2);
            CHECK(entries[0]->GetIsSelected().IsDefined());
            CHECK(entries[0]->GetIsSelected().ValueOr(false));
            CHECK(entries[1]->GetIsSelected().IsDefined());
            CHECK_FALSE(entries[1]->GetIsSelected().ValueOr(true));
        }

        SUBCASE("an unknown caption is rejected")
        {
            const auto status = slicer->SelectItems({"North"});
            CHECK_FALSE(status.Succeeded());
            CHECK(status.Error == SlicerError::UnknownItem);
        }
    }

    TEST_CASE("SetName renames the slicer, the shape, and the graphic frame "
              "[unit] [excel] [excel-slicer]")
    {
        auto editor = MakeWorkbook();
        AddPivotTable(editor);
        auto report = editor->GetWorksheet("Report");
        auto slicer = report->CreateSlicer(BasicPivotSlicer()).Slicer;
        REQUIRE(slicer);

        const auto status = slicer->SetName("AreaSlicer");
        INFO("rename: ", status.Message);
        REQUIRE(status.Succeeded());
        CHECK(slicer->Name() == "AreaSlicer");
        CHECK(report->SlicerByName("AreaSlicer") != nullptr);
        CHECK(report->SlicerByName("RegionSlicer") == nullptr);

        const auto drawing = DrawingOf(report);
        const auto anchors = drawing->Elements<XDR::TwoCellAnchor>();
        REQUIRE(anchors.size() == 1);
        const auto shape = AnchorSlicer(anchors[0]);
        REQUIRE(shape);
        CHECK(shape->GetName().ToString() == "AreaSlicer");

        const auto frame = anchors[0]->GetFirstChildOfType<XDR::GraphicFrame>();
        const auto nonVisual = frame->GetFirstChildOfType<XDR::NonVisualGraphicFrameProperties>();
        const auto properties = nonVisual->GetFirstChildOfType<XDR::NonVisualDrawingProperties>();
        REQUIRE(properties);
        CHECK(properties->GetName().ToString() == "AreaSlicer");
    }

    TEST_CASE("Update replaces presentation without touching the parts or the cache name "
              "[unit] [excel] [excel-slicer]")
    {
        auto editor = MakeWorkbook();
        AddPivotTable(editor);
        auto report = editor->GetWorksheet("Report");
        auto slicer = report->CreateSlicer(BasicPivotSlicer()).Slicer;
        REQUIRE(slicer);

        const auto part = slicer->GetPart();
        const auto cachePart = slicer->GetCachePart();

        auto definition = slicer->Definition();
        REQUIRE(definition);
        definition->ColumnCount = 3;
        definition->Caption = "Sales region";
        definition->CrossFilter = SlicerCrossFilter::None;
        definition->SortOrder = SlicerSortOrder::Descending;

        const auto status = slicer->Update(*definition);
        INFO("update: ", status.Message);
        REQUIRE(status.Succeeded());

        CHECK(slicer->GetPart() == part);
        CHECK(slicer->GetCachePart() == cachePart);
        CHECK(cachePart->GetSlicerCacheDefinition()->GetName().ToString() == "Slicer_Region");
        CHECK(slicer->ColumnCount() == 3);
        CHECK(slicer->Caption() == "Sales region");
        CHECK(slicer->CrossFilter() == SlicerCrossFilter::None);
        CHECK(slicer->SortOrder() == SlicerSortOrder::Descending);
    }

    TEST_CASE("Update rejects a change of the slicer source [unit] [excel] [excel-slicer]")
    {
        auto editor = MakeWorkbook();
        AddPivotTable(editor);
        auto report = editor->GetWorksheet("Report");
        auto slicer = report->CreateSlicer(BasicPivotSlicer()).Slicer;
        REQUIRE(slicer);

        auto definition = slicer->Definition();
        REQUIRE(definition);
        definition->SourceField = "Quarter";
        const auto status = slicer->Update(*definition);
        CHECK_FALSE(status.Succeeded());
        CHECK(status.Error == SlicerError::UnknownSource);
        CHECK(slicer->SourceField() == "Region");
    }

    TEST_CASE("Definition round-trips every field of a slicer [unit] [excel] [excel-slicer]")
    {
        SUBCASE("pivot slicer")
        {
            auto editor = MakeWorkbook();
            AddPivotTable(editor);
            auto report = editor->GetWorksheet("Report");

            auto original = BasicPivotSlicer();
            original.Caption = "Region filter";
            original.ColumnCount = 2;
            original.Style = "SlicerStyleDark2";
            original.SortOrder = SlicerSortOrder::Descending;
            original.CrossFilter = SlicerCrossFilter::ShowItemsWithNoData;
            original.SelectedItems = {"West"};
            auto slicer = report->CreateSlicer(original).Slicer;
            REQUIRE(slicer);

            const auto definition = slicer->Definition();
            REQUIRE(definition);
            CHECK(definition->Name == "RegionSlicer");
            CHECK(definition->Caption == "Region filter");
            CHECK(definition->SourceKind == SlicerSourceKind::PivotTable);
            CHECK(definition->PivotTableName == "SalesByRegion");
            CHECK(definition->SourceField == "Region");
            CHECK(definition->ColumnCount == 2);
            CHECK(definition->RowHeight == 241300);
            CHECK(definition->Style == "SlicerStyleDark2");
            CHECK(definition->SortOrder == SlicerSortOrder::Descending);
            CHECK(definition->CrossFilter == SlicerCrossFilter::ShowItemsWithNoData);
            CHECK(definition->SelectedItems == std::vector<std::string>{"West"});
            CHECK(definition->WriteDrawing);
            CHECK(definition->From.ToA1() == "F2");
            CHECK(definition->To.ToA1() == "H12");
        }

        SUBCASE("table slicer")
        {
            auto editor = MakeWorkbook();
            AddTable(editor);
            auto data = editor->GetWorksheet("Data");

            auto original = BasicTableSlicer();
            original.ColumnCount = 2;
            original.SelectedItems = {"Q2"};
            auto slicer = data->CreateSlicer(original).Slicer;
            REQUIRE(slicer);

            const auto definition = slicer->Definition();
            REQUIRE(definition);
            CHECK(definition->Name == "QuarterSlicer");
            CHECK(definition->SourceKind == SlicerSourceKind::Table);
            CHECK(definition->TableName == "SalesTable");
            CHECK(definition->SourceField == "Quarter");
            CHECK(definition->ColumnCount == 2);
            CHECK(definition->SelectedItems == std::vector<std::string>{"Q2"});
        }
    }

    TEST_CASE("Two slicers over the same pivot field share one slicer cache "
              "[unit] [excel] [excel-slicer]")
    {
        auto editor = MakeWorkbook();
        AddPivotTable(editor);
        auto report = editor->GetWorksheet("Report");

        auto first = report->CreateSlicer(BasicPivotSlicer()).Slicer;
        REQUIRE(first);
        auto other = BasicPivotSlicer();
        other.Name = "RegionSlicer2";
        other.From = Address("J2");
        other.To = Address("L12");
        auto secondSlicer = report->CreateSlicer(other).Slicer;
        REQUIRE(secondSlicer);

        CHECK(editor->GetDocument()->GetWorkbookPart()->GetSlicerCacheParts().size() == 1);
        CHECK(first->GetLowLevelApi()->GetCache().ToString() ==
              secondSlicer->GetLowLevelApi()->GetCache().ToString());
        CHECK(report->GetPart()->GetSlicersParts().size() == 1);
        CHECK(report->Slicers().size() == 2);
    }

    TEST_CASE("Slicers over different pivot fields get separate caches [unit] [excel] [excel-slicer]")
    {
        auto editor = MakeWorkbook();
        AddPivotTable(editor);
        auto report = editor->GetWorksheet("Report");

        REQUIRE(report->CreateSlicer(BasicPivotSlicer()).Slicer);
        auto other = BasicPivotSlicer();
        other.Name = "QuarterSlicer";
        other.SourceField = "Quarter";
        REQUIRE(report->CreateSlicer(other).Slicer);

        CHECK(editor->GetDocument()->GetWorkbookPart()->GetSlicerCacheParts().size() == 2);
    }

    TEST_CASE("RemoveSlicer keeps a cache that another slicer still uses "
              "[unit] [excel] [excel-slicer]")
    {
        auto editor = MakeWorkbook();
        AddPivotTable(editor);
        auto report = editor->GetWorksheet("Report");

        auto first = report->CreateSlicer(BasicPivotSlicer()).Slicer;
        auto other = BasicPivotSlicer();
        other.Name = "RegionSlicer2";
        auto second = report->CreateSlicer(other).Slicer;
        REQUIRE(first);
        REQUIRE(second);

        CHECK(report->RemoveSlicer(first));
        CHECK(editor->GetDocument()->GetWorkbookPart()->GetSlicerCacheParts().size() == 1);
        CHECK(report->Slicers().size() == 1);
        CHECK(report->GetPart()->GetSlicersParts().size() == 1);
    }

    TEST_CASE("RemoveSlicer removes the cache, both registries, the part, and the anchor "
              "[unit] [excel] [excel-slicer]")
    {
        auto editor = MakeWorkbook();
        AddPivotTable(editor);
        auto report = editor->GetWorksheet("Report");
        auto slicer = report->CreateSlicer(BasicPivotSlicer()).Slicer;
        REQUIRE(slicer);

        CHECK(report->RemoveSlicer(slicer));

        const auto workbookPart = editor->GetDocument()->GetWorkbookPart();
        CHECK(workbookPart->GetSlicerCacheParts().empty());
        CHECK(report->GetPart()->GetSlicersParts().empty());
        CHECK(report->Slicers().empty());

        // The extension lists must disappear completely, not merely be emptied.
        const auto workbook = workbookPart->GetTypedRootElement();
        CHECK(workbook->GetFirstChildOfType<S::WorkbookExtensionList>() == nullptr);
        CHECK(report->GetLowLevelApi()->GetFirstChildOfType<S::WorksheetExtensionList>() == nullptr);
        CHECK(report->GetPart()->GetDrawingsPart() == nullptr);
    }

    TEST_CASE("RemoveSlicer rejects a slicer owned by another worksheet [unit] [excel] [excel-slicer]")
    {
        auto editor = MakeWorkbook();
        AddPivotTable(editor);
        auto report = editor->GetWorksheet("Report");
        auto data = editor->GetWorksheet("Data");
        auto slicer = report->CreateSlicer(BasicPivotSlicer()).Slicer;
        REQUIRE(slicer);

        CHECK_FALSE(data->RemoveSlicer(slicer));
        CHECK(report->Slicers().size() == 1);
    }

    TEST_CASE("Invalid definitions are rejected without changing the workbook "
              "[unit] [excel] [excel-slicer]")
    {
        auto editor = MakeWorkbook();
        AddPivotTable(editor);
        AddTable(editor);
        auto report = editor->GetWorksheet("Report");

        auto definition = BasicPivotSlicer();
        auto expected = SlicerError::None;

        SUBCASE("unknown pivot table")
        {
            definition.PivotTableName = "Missing";
            expected = SlicerError::UnknownSource;
        }
        SUBCASE("unknown table")
        {
            definition = BasicTableSlicer();
            definition.TableName = "Missing";
            expected = SlicerError::UnknownSource;
        }
        SUBCASE("unknown pivot field")
        {
            definition.SourceField = "Missing";
            expected = SlicerError::UnknownField;
        }
        SUBCASE("unknown table column")
        {
            definition = BasicTableSlicer();
            definition.SourceField = "Missing";
            expected = SlicerError::UnknownField;
        }
        SUBCASE("empty source field")
        {
            definition.SourceField.clear();
            expected = SlicerError::UnknownField;
        }
        SUBCASE("inverted anchor")
        {
            definition.From = Address("H12");
            definition.To = Address("F2");
            expected = SlicerError::InvalidAnchor;
        }
        SUBCASE("zero column count")
        {
            definition.ColumnCount = 0;
            expected = SlicerError::InvalidPresentation;
        }
        SUBCASE("excessive column count")
        {
            definition.ColumnCount = 20001;
            expected = SlicerError::InvalidPresentation;
        }
        SUBCASE("zero row height")
        {
            definition.RowHeight = 0;
            expected = SlicerError::InvalidPresentation;
        }
        SUBCASE("unknown selected item")
        {
            definition.SelectedItems = {"North"};
            expected = SlicerError::UnknownItem;
        }
        SUBCASE("blank name")
        {
            definition.Name = " ";
            expected = SlicerError::InvalidName;
        }

        // Saving normally refreshes dcterms:modified, so two saves a second
        // apart differ in docProps/core.xml no matter what the slicer did. The
        // one-shot suppression has to be armed before each of them for the
        // comparison below to be about the slicer and nothing else.
        editor->GetDocument()->SuppressSaveTimePropertyUpdateOnce();
        const auto before = editor->SaveToMemory();
        REQUIRE_FALSE(before.empty());

        const auto created = report->CreateSlicer(definition);
        CAPTURE(created.Status.Message);
        CHECK(created.Slicer == nullptr);
        CHECK(created.Status.Error == expected);

        // The failure has to leave the workbook unchanged. The two buffers are
        // compared part-by-part rather than byte-by-byte: every ZIP entry
        // carries a wall-clock DOS timestamp, so raw buffers differ whenever
        // the two saves straddle a two-second tick, however little the slicer
        // touched.
        editor->GetDocument()->SuppressSaveTimePropertyUpdateOnce();
        const auto after = editor->SaveToMemory();
        const auto comparison = ExyokiOfficeTests::ComparePackages(before, after);
        CHECK(comparison.Ok);
        for (const auto& difference : comparison.Differences)
        {
            CAPTURE(difference);
        }
        CHECK(comparison.Preserved);
    }

    TEST_CASE("A duplicate slicer name is rejected [unit] [excel] [excel-slicer]")
    {
        auto editor = MakeWorkbook();
        AddPivotTable(editor);
        auto report = editor->GetWorksheet("Report");
        REQUIRE(report->CreateSlicer(BasicPivotSlicer()).Slicer);

        auto duplicate = BasicPivotSlicer();
        duplicate.SourceField = "Quarter";
        const auto created = report->CreateSlicer(duplicate);
        CHECK(created.Slicer == nullptr);
        CHECK(created.Status.Error == SlicerError::InvalidName);
    }

    TEST_CASE("An empty slicer name is generated [unit] [excel] [excel-slicer]")
    {
        auto editor = MakeWorkbook();
        AddPivotTable(editor);
        auto report = editor->GetWorksheet("Report");

        auto definition = BasicPivotSlicer();
        definition.Name.clear();
        auto slicer = report->CreateSlicer(definition).Slicer;
        REQUIRE(slicer);
        CHECK(slicer->Name() == "Slicer1");
    }

    TEST_CASE("Slicer parts validate against the SpreadsheetML schema [unit] [excel] [excel-slicer]")
    {
        auto editor = MakeWorkbook();
        AddPivotTable(editor);
        AddTable(editor);
        auto report = editor->GetWorksheet("Report");
        auto data = editor->GetWorksheet("Data");
        REQUIRE(report->CreateSlicer(BasicPivotSlicer()).Slicer);
        REQUIRE(data->CreateSlicer(BasicTableSlicer()).Slicer);

        ExyokiOffice::OpenXmlDomValidator validator;
        const auto check = [&](const std::shared_ptr<ExyokiOffice::OpenXMLElement>& root, const char* label)
        {
            REQUIRE(root);
            const auto result = validator.Validate(*root);
            INFO(label, ": ", Describe(result));
            CHECK(result.IsValid());
        };

        const auto workbookPart = editor->GetDocument()->GetWorkbookPart();
        check(workbookPart->GetTypedRootElement(), "workbook");
        check(report->GetLowLevelApi(), "worksheet");
        check(data->GetLowLevelApi(), "data worksheet");
        for (const auto& cachePart : workbookPart->GetSlicerCacheParts())
        {
            check(cachePart->GetSlicerCacheDefinition(), "slicerCacheDefinition");
        }
        for (const auto& slicersPart : report->GetPart()->GetSlicersParts())
        {
            check(slicersPart->GetSlicers(), "slicers");
        }
        check(DrawingOf(report), "worksheetDrawing");
    }

    TEST_CASE("Slicers survive a save and reopen round trip [unit] [excel] [excel-slicer]")
    {
        auto editor = MakeWorkbook();
        AddPivotTable(editor);
        AddTable(editor);
        auto report = editor->GetWorksheet("Report");
        auto data = editor->GetWorksheet("Data");

        auto pivotDefinition = BasicPivotSlicer();
        pivotDefinition.SelectedItems = {"East"};
        REQUIRE(report->CreateSlicer(pivotDefinition).Slicer);
        REQUIRE(data->CreateSlicer(BasicTableSlicer()).Slicer);

        const auto bytes = editor->SaveToMemory();
        REQUIRE_FALSE(bytes.empty());
        auto reopened = ExcelDocumentEditor::Open(bytes);
        REQUIRE(reopened);

        auto reopenedReport = reopened->GetWorksheet("Report");
        REQUIRE(reopenedReport);
        auto pivotSlicer = reopenedReport->SlicerByName("RegionSlicer");
        REQUIRE(pivotSlicer);
        // Each worksheet must own a distinct drawing part; sharing one would
        // silently overwrite the first sheet's shapes.
        const auto reportDrawing = reopenedReport->GetPart()->GetDrawingsPart();
        const auto dataDrawing = reopened->GetWorksheet("Data")->GetPart()->GetDrawingsPart();
        REQUIRE(reportDrawing);
        REQUIRE(dataDrawing);
        CHECK(reportDrawing->Uri() != dataDrawing->Uri());
        const auto pivotRead = pivotSlicer->Definition();
        REQUIRE(pivotRead);
        CHECK(pivotRead->SourceKind == SlicerSourceKind::PivotTable);
        CHECK(pivotRead->PivotTableName == "SalesByRegion");
        CHECK(pivotRead->SourceField == "Region");
        CHECK(pivotRead->SelectedItems == std::vector<std::string>{"East"});
        CHECK(pivotRead->From.ToA1() == "F2");

        auto reopenedData = reopened->GetWorksheet("Data");
        REQUIRE(reopenedData);
        auto tableSlicer = reopenedData->SlicerByName("QuarterSlicer");
        REQUIRE(tableSlicer);
        const auto tableRead = tableSlicer->Definition();
        REQUIRE(tableRead);
        CHECK(tableRead->SourceKind == SlicerSourceKind::Table);
        CHECK(tableRead->TableName == "SalesTable");
        CHECK(tableRead->SourceField == "Quarter");
    }

    TEST_CASE("RemovePivotTable detaches the slicers built on it [unit] [excel] [excel-slicer]")
    {
        auto editor = MakeWorkbook();
        auto pivot = AddPivotTable(editor);
        auto report = editor->GetWorksheet("Report");
        REQUIRE(report->CreateSlicer(BasicPivotSlicer()).Slicer);
        REQUIRE(editor->GetDocument()->GetWorkbookPart()->GetSlicerCacheParts().size() == 1);

        CHECK(report->RemovePivotTable(pivot));
        CHECK(editor->GetDocument()->GetWorkbookPart()->GetSlicerCacheParts().empty());
        CHECK(report->Slicers().empty());
        CHECK(report->GetPart()->GetSlicersParts().empty());
    }

    TEST_CASE("RemoveTable detaches the slicers built on it [unit] [excel] [excel-slicer]")
    {
        auto editor = MakeWorkbook();
        auto table = AddTable(editor);
        auto data = editor->GetWorksheet("Data");
        REQUIRE(data->CreateSlicer(BasicTableSlicer()).Slicer);
        REQUIRE(editor->GetDocument()->GetWorkbookPart()->GetSlicerCacheParts().size() == 1);

        CHECK(data->RemoveTable(table));
        CHECK(editor->GetDocument()->GetWorkbookPart()->GetSlicerCacheParts().empty());
        CHECK(data->Slicers().empty());
    }

    TEST_CASE("Renaming a pivot table follows through to its slicer caches "
              "[unit] [excel] [excel-slicer]")
    {
        auto editor = MakeWorkbook();
        auto pivot = AddPivotTable(editor);
        auto report = editor->GetWorksheet("Report");
        auto slicer = report->CreateSlicer(BasicPivotSlicer()).Slicer;
        REQUIRE(slicer);

        REQUIRE(pivot->SetName("SalesReport").Succeeded());
        CHECK(slicer->SourceObjectName() == "SalesReport");
    }

    TEST_CASE("SlicerBuilder inserts a configured slicer [unit] [excel] [excel-slicer]")
    {
        auto editor = MakeWorkbook();
        AddPivotTable(editor);
        auto report = editor->GetWorksheet("Report");

        auto slicer = SlicerBuilder(report)
                          .SetName("BuiltSlicer")
                          .SetPivotSource("SalesByRegion", "Region")
                          .SetAnchor(Address("F2"), Address("H12"))
                          .SetColumnCount(2)
                          .SetCaption("Region")
                          .SelectItems({"East"})
                          .Build();
        REQUIRE(slicer);
        CHECK(slicer->Name() == "BuiltSlicer");
        CHECK(slicer->ColumnCount() == 2);
        const auto items = slicer->Items();
        REQUIRE(items.size() == 2);
        CHECK(items[0].Selected);
        CHECK_FALSE(items[1].Selected);
    }

    TEST_CASE("An imported slicer without a cache part is enumerated and removable "
              "[unit] [excel] [excel-slicer]")
    {
        auto editor = MakeWorkbook();
        auto report = editor->GetWorksheet("Report");

        // Mimics a slicer authored by another producer: a slicers part with no
        // cache part and no worksheet registry entry.
        auto slicersPart = report->GetPart()->AddSlicersPart();
        REQUIRE(slicersPart);
        auto root = slicersPart->GetSlicers();
        REQUIRE(root);
        auto element = root->AppendChild<X14::Slicer>();
        REQUIRE(element);
        element->SetName(ExyokiOffice::StringValue("ForeignSlicer"));
        element->SetCache(ExyokiOffice::StringValue("Slicer_Unknown"));
        element->SetRowHeight(ExyokiOffice::UInt32Value(241300));

        const auto slicers = report->Slicers();
        REQUIRE(slicers.size() == 1);
        CHECK(slicers[0]->Name() == "ForeignSlicer");
        CHECK(slicers[0]->GetCachePart() == nullptr);
        CHECK(slicers[0]->Items().empty());
        CHECK(slicers[0]->SourceObjectName().empty());
        CHECK(slicers[0]->SourceKind() == SlicerSourceKind::PivotTable);

        CHECK(report->RemoveSlicer(slicers[0]));
        CHECK(report->Slicers().empty());
    }

    TEST_CASE("IsValidSlicerName enforces Excel's naming rules [unit] [excel] [excel-slicer]")
    {
        CHECK(IsValidSlicerName("RegionSlicer"));
        CHECK(IsValidSlicerName("Region Slicer"));
        CHECK_FALSE(IsValidSlicerName(""));
        CHECK_FALSE(IsValidSlicerName(" Leading"));
        CHECK_FALSE(IsValidSlicerName("Trailing "));
        CHECK_FALSE(IsValidSlicerName(std::string(256, 'a')));
        CHECK_FALSE(IsValidSlicerName(std::string("With\tTab")));
    }
}
