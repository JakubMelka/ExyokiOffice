// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "doctest.h"

#include "ExyokiOffice/Word/WordDocument.hpp"

namespace
{
using ExyokiOffice::Word::TemplateMergeData;
using ExyokiOffice::Word::WordDocumentEditor;
} // namespace

TEST_SUITE("WordTemplateMergeTests")
{

    TEST_CASE("MergeTemplate replaces MERGEFIELD results from literal scalar values [unit] [word] [word-template-merge]")
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);

        auto paragraph = editor->AddParagraph("Dear ");
        REQUIRE(paragraph != nullptr);
        REQUIRE(paragraph->AddField("MERGEFIELD FirstName \\\\* MERGEFORMAT", "First") != nullptr);
        REQUIRE(paragraph->AddSimpleField("MERGEFIELD \"Last Name\"", "Last") != nullptr);

        TemplateMergeData data;
        data.Values.emplace("FirstName", "Ada");
        data.Values.emplace("Last Name", "Lovelace");

        auto result = editor->MergeTemplate(data);

        CHECK(result.FieldsMerged == 2);
        CHECK(result.BookmarksMerged == 0);
        CHECK(result.RegionsMerged == 0);
        CHECK(paragraph->PlainText() == "Dear AdaLovelace");

        auto fields = editor->Fields();
        REQUIRE(fields.size() == 2);
        CHECK(fields[0]->GetInstruction() == "MERGEFIELD FirstName \\\\* MERGEFORMAT");
        CHECK(fields[0]->GetResult() == "Ada");
        CHECK(fields[1]->GetResult() == "Lovelace");
    }

    TEST_CASE("MergeTemplate leaves missing fields unchanged and never evaluates instruction text [unit] [word] [word-template-merge]")
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);

        auto paragraph = editor->AddParagraph();
        REQUIRE(paragraph != nullptr);
        auto field = paragraph->AddField("MERGEFIELD Total + DangerousCall()", "unchanged");
        REQUIRE(field != nullptr);

        TemplateMergeData data;
        data.Values.emplace("Total", "42");

        auto result = editor->MergeTemplate(data);

        CHECK(result.FieldsMerged == 0);
        CHECK(field->GetResult() == "unchanged");
        CHECK(paragraph->PlainText() == "unchanged");
    }

    TEST_CASE("MergeTemplate replaces same-paragraph bookmarks by name [unit] [word] [word-template-merge]")
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);

        auto paragraph = editor->AddParagraph("Customer: ");
        REQUIRE(paragraph != nullptr);
        REQUIRE(paragraph->AddBookmark("CustomerName") != nullptr);

        TemplateMergeData data;
        data.Values.emplace("CustomerName", "Contoso Ltd.");

        auto result = editor->MergeTemplate(data, true);

        CHECK(result.FieldsMerged == 0);
        CHECK(result.BookmarksMerged == 1);
        CHECK(paragraph->PlainText() == "Customer: Contoso Ltd.");
        REQUIRE(editor->FindBookmark("CustomerName") != nullptr);
    }

    TEST_CASE("MergeTemplate expands TableStart and TableEnd repeating regions [unit] [word] [word-template-merge]")
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);

        auto start = editor->AddParagraph();
        REQUIRE(start != nullptr);
        REQUIRE(start->AddField("MERGEFIELD TableStart:Orders", "") != nullptr);

        auto row = editor->AddParagraph("Item: ");
        REQUIRE(row != nullptr);
        REQUIRE(row->AddField("MERGEFIELD Name", "Template") != nullptr);

        auto end = editor->AddParagraph();
        REQUIRE(end != nullptr);
        REQUIRE(end->AddField("MERGEFIELD TableEnd:Orders", "") != nullptr);

        TemplateMergeData data;
        data.Regions["Orders"].push_back({{"Name", "Pencil"}});
        data.Regions["Orders"].push_back({{"Name", "Paper"}});

        auto result = editor->MergeTemplate(data);

        CHECK(result.RegionsMerged == 1);
        CHECK(result.RegionRowsInserted == 2);
        CHECK(result.FieldsMerged == 2);

        auto paragraphs = editor->Paragraphs();
        REQUIRE(paragraphs.size() == 2);
        CHECK(paragraphs[0]->PlainText() == "Item: Pencil");
        CHECK(paragraphs[1]->PlainText() == "Item: Paper");
        CHECK(editor->Fields().size() == 2);
    }

    TEST_CASE("MergeTemplate removes repeating regions with missing row data [unit] [word] [word-template-merge]")
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);

        REQUIRE(editor->AddParagraph()->AddField("MERGEFIELD TableStart:Lines", "") != nullptr);
        auto row = editor->AddParagraph("Template row");
        REQUIRE(row != nullptr);
        REQUIRE(editor->AddParagraph()->AddField("MERGEFIELD TableEnd:Lines", "") != nullptr);
        auto after = editor->AddParagraph("After");
        REQUIRE(after != nullptr);

        TemplateMergeData data;
        auto result = editor->MergeTemplate(data);

        CHECK(result.RegionsMerged == 1);
        CHECK(result.RegionRowsInserted == 0);
        auto paragraphs = editor->Paragraphs();
        REQUIRE(paragraphs.size() == 1);
        CHECK(paragraphs.front()->PlainText() == "After");
    }

    TEST_CASE("MergeTemplate output round-trips after fields bookmarks and regions are merged [unit] [word] [word-template-merge]")
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);

        auto title = editor->AddParagraph("Report for ");
        REQUIRE(title != nullptr);
        REQUIRE(title->AddSimpleField("MERGEFIELD Customer", "Customer") != nullptr);
        REQUIRE(title->AddBookmark("Suffix") != nullptr);

        REQUIRE(editor->AddParagraph()->AddField("MERGEFIELD TableStart:Rows", "") != nullptr);
        auto row = editor->AddParagraph("Value: ");
        REQUIRE(row != nullptr);
        REQUIRE(row->AddField("MERGEFIELD Value", "x") != nullptr);
        REQUIRE(editor->AddParagraph()->AddField("MERGEFIELD TableEnd:Rows", "") != nullptr);

        TemplateMergeData data;
        data.Values.emplace("Customer", "Northwind");
        data.Values.emplace("Suffix", " Inc.");
        data.Regions["Rows"].push_back({{"Value", "A"}});
        data.Regions["Rows"].push_back({{"Value", "B"}});

        auto result = editor->MergeTemplate(data);
        CHECK(result.FieldsMerged == 3);
        CHECK(result.BookmarksMerged == 1);
        CHECK(result.RegionsMerged == 1);

        auto reopened = WordDocumentEditor::Open(editor->SaveToMemory());
        REQUIRE(reopened != nullptr);
        auto paragraphs = reopened->Paragraphs();
        REQUIRE(paragraphs.size() == 3);
        CHECK(paragraphs[0]->PlainText() == "Report for Northwind Inc.");
        CHECK(paragraphs[1]->PlainText() == "Value: A");
        CHECK(paragraphs[2]->PlainText() == "Value: B");
        CHECK(reopened->FindBookmark("Suffix") != nullptr);
    }

} // TEST_SUITE("WordTemplateMergeTests")
