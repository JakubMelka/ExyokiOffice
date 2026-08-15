// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "doctest.h"

#include "ExyokiOffice/Excel/ExcelDocument.hpp"

#include <string>

using ExyokiOffice::Excel::ExcelDocumentEditor;

TEST_SUITE("ExcelLifecycleTests")
{

    TEST_CASE("ExcelDocumentEditor creates workbook and round trips worksheet cells [unit] [excel] [excel-lifecycle]")
    {
        auto editor = ExcelDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);
        REQUIRE(editor->GetDocument() != nullptr);

        auto sheet = editor->FirstWorksheet();
        REQUIRE(sheet != nullptr);
        CHECK(sheet->Name() == "Sheet1");
        CHECK(sheet->SetCellText(1, 1, "Hello"));
        CHECK(sheet->SetCellNumber(2, 2, 42.5));

        auto packageBytes = editor->SaveToMemory();
        REQUIRE(!packageBytes.empty());

        auto reopened = ExcelDocumentEditor::Open(packageBytes);
        REQUIRE(reopened != nullptr);

        auto reopenedSheets = reopened->Worksheets();
        REQUIRE(reopenedSheets.size() == 1);
        CHECK(reopenedSheets.front()->Name() == "Sheet1");

        const auto xml = reopenedSheets.front()->GetPart()->GetXmlString();
        CHECK(xml.find("A1") != std::string::npos);
        CHECK(reopened->SharedStrings().Lookup(0) == "Hello");
        CHECK(xml.find("B2") != std::string::npos);
        CHECK(xml.find("42.5") != std::string::npos);
    }

    TEST_CASE("ExcelDocumentEditor manages worksheet collection and preserves workbook links [unit] [excel] [excel-worksheets]")
    {
        auto editor = ExcelDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);

        auto data = editor->AddWorksheet("Data");
        auto summary = editor->AddWorksheet("Summary");
        REQUIRE(data != nullptr);
        REQUIRE(summary != nullptr);
        CHECK(data->SetCellText(1, 1, "source"));

        CHECK(editor->Worksheets().size() == 3);
        CHECK(editor->GetWorksheet(1)->Name() == "Data");
        CHECK(editor->GetWorksheet("summary")->Name() == "Summary");

        CHECK(editor->RenameWorksheet(1, "RawData"));
        CHECK(editor->GetWorksheet("rawdata") != nullptr);
        CHECK_FALSE(editor->RenameWorksheet(1, "SUMMARY"));
        CHECK_FALSE(editor->RenameWorksheet(1, "Bad/Name"));
        CHECK_FALSE(editor->RenameWorksheet(99, "Other"));

        CHECK(editor->MoveWorksheet(2, 0));
        auto moved = editor->Worksheets();
        REQUIRE(moved.size() == 3);
        CHECK(moved[0]->Name() == "Summary");
        CHECK(moved[1]->Name() == "Sheet1");
        CHECK(moved[2]->Name() == "RawData");

        auto copy = editor->CopyWorksheet(2, "RawData Copy");
        REQUIRE(copy != nullptr);
        CHECK(copy->Name() == "RawData Copy");
        CHECK(copy->GetPart() != data->GetPart());
        CHECK(editor->SharedStrings().Lookup(0) == "source");
        CHECK(copy->SetCellText(1, 1, "copy"));
        CHECK(editor->SharedStrings().Lookup(0) == "source");
        CHECK(data->GetPart()->GetXmlString().find("copy") == std::string::npos);
        CHECK_FALSE(editor->CopyWorksheet(0, "RawData Copy"));
        CHECK_FALSE(editor->CopyWorksheet(99, "Missing"));

        CHECK(editor->RemoveWorksheet(1));
        CHECK_FALSE(editor->RemoveWorksheet(99));

        auto packageBytes = editor->SaveToMemory();
        REQUIRE(!packageBytes.empty());

        auto reopened = ExcelDocumentEditor::Open(packageBytes);
        REQUIRE(reopened != nullptr);
        auto reopenedSheets = reopened->Worksheets();
        REQUIRE(reopenedSheets.size() == 3);
        CHECK(reopenedSheets[0]->Name() == "Summary");
        CHECK(reopenedSheets[1]->Name() == "RawData");
        CHECK(reopenedSheets[2]->Name() == "RawData Copy");

        auto workbookPart = reopened->GetDocument()->GetWorkbookPart();
        REQUIRE(workbookPart != nullptr);
        CHECK(workbookPart->GetWorksheetParts().size() == 3);
        for (const auto& worksheet : reopenedSheets)
        {
            REQUIRE(worksheet != nullptr);
            CHECK(!worksheet->GetPart()->RelationshipId().empty());
        }
    }

    TEST_CASE("ExcelDocumentEditor rejects duplicate and invalid worksheet additions [unit] [excel] [excel-worksheets]")
    {
        auto editor = ExcelDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);

        CHECK(editor->AddWorksheet("Sheet2") != nullptr);
        CHECK(editor->AddWorksheet("") != nullptr);
        CHECK_FALSE(editor->AddWorksheet("sheet1"));
        CHECK_FALSE(editor->AddWorksheet("Bad:Name"));
        CHECK_FALSE(editor->AddWorksheet("ThisWorksheetNameIsLongerThanThirtyOneCharacters"));

        auto workbookPart = editor->GetDocument()->GetWorkbookPart();
        REQUIRE(workbookPart != nullptr);
        CHECK(editor->Worksheets().size() == 3);
        CHECK(workbookPart->GetWorksheetParts().size() == 3);
    }

    TEST_CASE("ExcelDocumentEditor keeps at least one worksheet [unit] [excel] [excel-worksheets]")
    {
        auto editor = ExcelDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);

        CHECK_FALSE(editor->RemoveWorksheet(0));
        CHECK(editor->AddWorksheet("Second") != nullptr);
        CHECK(editor->RemoveWorksheet(0));
        CHECK_FALSE(editor->RemoveWorksheet(0));
        REQUIRE(editor->FirstWorksheet() != nullptr);
        CHECK(editor->FirstWorksheet()->Name() == "Second");
    }

} // namespace
