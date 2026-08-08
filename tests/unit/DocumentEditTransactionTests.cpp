// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "ExyokiOffice/DocumentEditTransaction.hpp"
#include "ExyokiOffice/Excel/ExcelDocument.hpp"
#include "ExyokiOffice/PowerPoint/PowerPointDocument.hpp"
#include "ExyokiOffice/Word/WordDocument.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <doctest/doctest.h>

#include <cstddef>
#include <utility>

using namespace ExyokiOffice;

TEST_CASE("Editors retain their existing lifecycle when transactions are not used")
{
    auto word = Word::WordDocumentEditor::CreateNew();
    REQUIRE(word);
    REQUIRE(word->AddParagraph("ordinary Word edit"));
    auto reopenedWord = Word::WordDocumentEditor::Open(word->SaveToMemory());
    REQUIRE(reopenedWord);
    REQUIRE(reopenedWord->Paragraphs().size() == 1);
    CHECK(reopenedWord->Paragraphs().front()->PlainText() == "ordinary Word edit");

    auto excel = Excel::ExcelDocumentEditor::CreateNew();
    REQUIRE(excel);
    REQUIRE(excel->FirstWorksheet()->SetCellNumber(1, 1, 42.0));
    auto reopenedExcel = Excel::ExcelDocumentEditor::Open(excel->SaveToMemory());
    REQUIRE(reopenedExcel);
    auto number = reopenedExcel->FirstWorksheet()->GetCellValue(1, 1);
    REQUIRE(number);
    CHECK(number->Text() == "42");

    auto powerPoint = PowerPoint::PowerPointDocumentEditor::CreateNew();
    REQUIRE(powerPoint);
    REQUIRE(powerPoint->AddSlide());
    auto reopenedPowerPoint =
        PowerPoint::PowerPointDocumentEditor::Open(powerPoint->SaveToMemory());
    REQUIRE(reopenedPowerPoint);
    CHECK(reopenedPowerPoint->SlideCount() == 1);
}

TEST_CASE("Word edit transaction commits or restores the complete package")
{
    auto editor = Word::WordDocumentEditor::CreateNew();
    REQUIRE(editor);
    REQUIRE(editor->AddParagraph("original"));

    SUBCASE("explicit rollback restores DOM and relationships")
    {
        auto transaction = editor->BeginTransaction();
        REQUIRE(transaction.IsActive());

        auto changed = editor->AddParagraph("changed");
        REQUIRE(changed);
        REQUIRE(changed->AddHyperlink("website", "https://example.com"));
        REQUIRE(editor->Paragraphs().size() == 2);

        CHECK(transaction.Rollback());
        CHECK_FALSE(transaction.IsActive());
        REQUIRE(editor->Paragraphs().size() == 1);
        CHECK(editor->Paragraphs().front()->PlainText() == "original");

        auto reopened = Word::WordDocumentEditor::Open(editor->SaveToMemory());
        REQUIRE(reopened);
        REQUIRE(reopened->Paragraphs().size() == 1);
        CHECK(reopened->Paragraphs().front()->Hyperlinks().empty());
    }

    SUBCASE("commit retains all edits")
    {
        auto transaction = editor->BeginTransaction();
        REQUIRE(transaction.IsActive());
        REQUIRE(editor->AddParagraph("committed"));
        CHECK(transaction.Commit());
        CHECK_FALSE(transaction.IsActive());
        CHECK_FALSE(transaction.Commit());
        REQUIRE(editor->Paragraphs().size() == 2);
        CHECK(editor->Paragraphs().back()->PlainText() == "committed");
    }

    SUBCASE("scope exit rolls back automatically")
    {
        {
            auto transaction = editor->BeginTransaction();
            REQUIRE(transaction.IsActive());
            REQUIRE(editor->AddParagraph("temporary"));
        }

        REQUIRE(editor->Paragraphs().size() == 1);
        CHECK(editor->Paragraphs().front()->PlainText() == "original");
    }
}

TEST_CASE("Excel edit transaction restores worksheets, cells, and package graph")
{
    auto editor = Excel::ExcelDocumentEditor::CreateNew();
    REQUIRE(editor);
    REQUIRE(editor->FirstWorksheet());
    REQUIRE(editor->FirstWorksheet()->SetCellText(1, 1, "original"));

    auto transaction = editor->BeginTransaction();
    REQUIRE(transaction.IsActive());
    REQUIRE(editor->FirstWorksheet()->SetCellText(1, 1, "changed"));
    REQUIRE(editor->AddWorksheet("Temporary"));
    REQUIRE(editor->Worksheets().size() == 2);

    CHECK(transaction.Rollback());
    REQUIRE(editor->Worksheets().size() == 1);

    auto reopened = Excel::ExcelDocumentEditor::Open(editor->SaveToMemory());
    REQUIRE(reopened);
    REQUIRE(reopened->Worksheets().size() == 1);
    auto value = reopened->FirstWorksheet()->GetCellValue(1, 1);
    REQUIRE(value);
    REQUIRE(value->SharedStringIndex());
    CHECK(reopened->SharedStrings().Lookup(*value->SharedStringIndex()) == "original");
}

TEST_CASE("PowerPoint edit transaction restores slides and their relationships")
{
    auto editor = PowerPoint::PowerPointDocumentEditor::CreateNew();
    REQUIRE(editor);
    REQUIRE(editor->AddSlide());
    REQUIRE(editor->SlideCount() == 1);

    auto transaction = editor->BeginTransaction();
    REQUIRE(transaction.IsActive());
    REQUIRE(editor->AddSlide());
    REQUIRE(editor->AddSlide());
    REQUIRE(editor->SlideCount() == 3);

    CHECK(transaction.Rollback());
    CHECK(editor->SlideCount() == 1);

    auto reopened = PowerPoint::PowerPointDocumentEditor::Open(editor->SaveToMemory());
    REQUIRE(reopened);
    CHECK(reopened->SlideCount() == 1);
}

TEST_CASE("Document edit mementos are reusable and family safe")
{
    auto word = Word::WordDocumentEditor::CreateNew();
    auto excel = Excel::ExcelDocumentEditor::CreateNew();
    auto powerPoint = PowerPoint::PowerPointDocumentEditor::CreateNew();
    REQUIRE(word);
    REQUIRE(excel);
    REQUIRE(powerPoint);
    REQUIRE(word->AddParagraph("snapshot"));

    auto memento = word->CreateMemento();
    REQUIRE(memento);
    CHECK(memento->Family() == DocumentFamily::Word);
    CHECK(memento->Size() > 0);
    CHECK(memento->Bytes().size() == memento->Size());
    auto reconstructed =
        DocumentEditMemento::FromBytes(DocumentFamily::Word, memento->Bytes());
    REQUIRE(reconstructed);
    CHECK(reconstructed->Bytes().size() == memento->Bytes().size());
    CHECK_FALSE(DocumentEditMemento::FromBytes(DocumentFamily::Word, {}).has_value());

    REQUIRE(word->AddParagraph("discarded"));
    REQUIRE(word->RestoreMemento(*reconstructed));
    REQUIRE(word->Paragraphs().size() == 1);
    CHECK(word->Paragraphs().front()->PlainText() == "snapshot");

    REQUIRE(word->AddParagraph("discarded again"));
    REQUIRE(word->RestoreMemento(*memento));
    REQUIRE(word->Paragraphs().size() == 1);

    CHECK_FALSE(excel->RestoreMemento(*memento));
    CHECK_FALSE(powerPoint->RestoreMemento(*memento));
}

TEST_CASE("Document editors reject concurrent transactions and preserve move ownership")
{
    auto editor = Word::WordDocumentEditor::CreateNew();
    REQUIRE(editor);
    REQUIRE(editor->AddParagraph("root"));

    auto first = editor->BeginTransaction();
    REQUIRE(first.IsActive());
    CHECK_FALSE(editor->BeginTransaction().IsActive());

    DocumentEditTransaction moved = std::move(first);
    CHECK_FALSE(first.IsActive());
    REQUIRE(moved.IsActive());
    CHECK_FALSE(editor->BeginTransaction().IsActive());
    CHECK(moved.Commit());

    auto afterCommit = editor->BeginTransaction();
    REQUIRE(afterCommit.IsActive());
    CHECK(afterCommit.Rollback());

    {
        auto scoped = editor->BeginTransaction();
        REQUIRE(scoped.IsActive());
    }

    auto afterDestruction = editor->BeginTransaction();
    REQUIRE(afterDestruction.IsActive());
    CHECK(afterDestruction.Commit());
}

TEST_CASE("Every document editor permits only one active transaction")
{
    auto word = Word::WordDocumentEditor::CreateNew();
    auto excel = Excel::ExcelDocumentEditor::CreateNew();
    auto powerPoint = PowerPoint::PowerPointDocumentEditor::CreateNew();
    REQUIRE(word);
    REQUIRE(excel);
    REQUIRE(powerPoint);

    auto wordTransaction = word->BeginTransaction();
    auto excelTransaction = excel->BeginTransaction();
    auto powerPointTransaction = powerPoint->BeginTransaction();
    REQUIRE(wordTransaction.IsActive());
    REQUIRE(excelTransaction.IsActive());
    REQUIRE(powerPointTransaction.IsActive());

    CHECK_FALSE(word->BeginTransaction().IsActive());
    CHECK_FALSE(excel->BeginTransaction().IsActive());
    CHECK_FALSE(powerPoint->BeginTransaction().IsActive());

    CHECK(wordTransaction.Rollback());
    CHECK(excelTransaction.Rollback());
    CHECK(powerPointTransaction.Rollback());

    CHECK(word->BeginTransaction().IsActive());
    CHECK(excel->BeginTransaction().IsActive());
    CHECK(powerPoint->BeginTransaction().IsActive());
}

TEST_CASE("Inactive edit transactions report snapshot precondition failures")
{
    Word::WordDocumentEditor word;
    Excel::ExcelDocumentEditor excel;
    PowerPoint::PowerPointDocumentEditor powerPoint;

    auto wordTransaction = word.BeginTransaction();
    auto excelTransaction = excel.BeginTransaction();
    auto powerPointTransaction = powerPoint.BeginTransaction();

    CHECK_FALSE(wordTransaction.IsActive());
    CHECK_FALSE(excelTransaction.IsActive());
    CHECK_FALSE(powerPointTransaction.IsActive());
    CHECK_FALSE(wordTransaction.Commit());
    CHECK_FALSE(excelTransaction.Rollback());
}

TEST_CASE("Transactions become inactive when their editor is destroyed")
{
    DocumentEditTransaction wordTransaction;
    {
        auto editor = Word::WordDocumentEditor::CreateNew();
        REQUIRE(editor);
        wordTransaction = editor->BeginTransaction();
        REQUIRE(wordTransaction.IsActive());
    }
    CHECK_FALSE(wordTransaction.IsActive());
    CHECK_FALSE(wordTransaction.Commit());
    CHECK_FALSE(wordTransaction.Rollback());

    DocumentEditTransaction excelTransaction;
    {
        auto editor = Excel::ExcelDocumentEditor::CreateNew();
        REQUIRE(editor);
        excelTransaction = editor->BeginTransaction();
        REQUIRE(excelTransaction.IsActive());
    }
    CHECK_FALSE(excelTransaction.IsActive());
    CHECK_FALSE(excelTransaction.Rollback());

    DocumentEditTransaction powerPointTransaction;
    {
        auto editor = PowerPoint::PowerPointDocumentEditor::CreateNew();
        REQUIRE(editor);
        powerPointTransaction = editor->BeginTransaction();
        REQUIRE(powerPointTransaction.IsActive());
    }
    CHECK_FALSE(powerPointTransaction.IsActive());
    CHECK_FALSE(powerPointTransaction.Rollback());
}

TEST_CASE("Copied editors cannot keep another editor transaction owner alive")
{
    auto original = Word::WordDocumentEditor::CreateNew();
    REQUIRE(original);
    auto originalTransaction = original->BeginTransaction();
    REQUIRE(originalTransaction.IsActive());

    auto copy = std::make_shared<Word::WordDocumentEditor>(*original);
    original.reset();
    CHECK_FALSE(originalTransaction.IsActive());

    auto copyTransaction = copy->BeginTransaction();
    REQUIRE(copyTransaction.IsActive());
    CHECK(copyTransaction.Commit());
}

TEST_CASE("Fault injection after each Word edit step leaves no partial change")
{
    constexpr Size stepCount = 3;
    for (Size failingStep = 0; failingStep < stepCount; ++failingStep)
    {
        CAPTURE(failingStep);
        auto editor = Word::WordDocumentEditor::CreateNew();
        REQUIRE(editor);
        REQUIRE(editor->AddParagraph("baseline"));

        {
            auto transaction = editor->BeginTransaction();
            REQUIRE(transaction.IsActive());

            auto first = editor->AddParagraph("step one");
            REQUIRE(first);
            if (failingStep == 0)
            {
                continue;
            }

            REQUIRE(first->AddHyperlink("step two", "https://example.com"));
            if (failingStep == 1)
            {
                continue;
            }

            REQUIRE(editor->AddTable(2, 2));
        }

        REQUIRE(editor->Paragraphs().size() == 1);
        CHECK(editor->Paragraphs().front()->PlainText() == "baseline");
        CHECK(editor->Tables().empty());
    }
}

TEST_CASE("Fault injection after each Excel edit step leaves no partial change")
{
    constexpr Size stepCount = 3;
    for (Size failingStep = 0; failingStep < stepCount; ++failingStep)
    {
        CAPTURE(failingStep);
        auto editor = Excel::ExcelDocumentEditor::CreateNew();
        REQUIRE(editor);
        REQUIRE(editor->FirstWorksheet()->SetCellText(1, 1, "baseline"));

        {
            auto transaction = editor->BeginTransaction();
            REQUIRE(transaction.IsActive());

            REQUIRE(editor->FirstWorksheet()->SetCellText(1, 1, "step one"));
            if (failingStep == 0)
            {
                continue;
            }

            REQUIRE(editor->AddWorksheet("StepTwo"));
            if (failingStep == 1)
            {
                continue;
            }

            REQUIRE(editor->Worksheets().back()->SetCellNumber(2, 2, 3.0));
        }

        REQUIRE(editor->Worksheets().size() == 1);
        auto value = editor->FirstWorksheet()->GetCellValue(1, 1);
        REQUIRE(value);
        REQUIRE(value->SharedStringIndex());
        CHECK(editor->SharedStrings().Lookup(*value->SharedStringIndex()) == "baseline");
    }
}

TEST_CASE("Fault injection after each PowerPoint edit step leaves no partial change")
{
    constexpr Size stepCount = 3;
    for (Size failingStep = 0; failingStep < stepCount; ++failingStep)
    {
        CAPTURE(failingStep);
        auto editor = PowerPoint::PowerPointDocumentEditor::CreateNew();
        REQUIRE(editor);
        REQUIRE(editor->AddSlide());

        {
            auto transaction = editor->BeginTransaction();
            REQUIRE(transaction.IsActive());

            REQUIRE(editor->AddSlide());
            if (failingStep == 0)
            {
                continue;
            }

            REQUIRE(editor->AddSlide());
            if (failingStep == 1)
            {
                continue;
            }

            REQUIRE(editor->MoveSlide(2, 0));
        }

        CHECK(editor->SlideCount() == 1);
    }
}
