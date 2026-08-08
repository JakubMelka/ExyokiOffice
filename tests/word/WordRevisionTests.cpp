// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "doctest.h"

#include "ExyokiOffice/Word/WordDocument.hpp"
#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/Wordprocessing.hpp"

namespace
{
namespace W = ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing;

using ExyokiOffice::Word::RevisionType;
using ExyokiOffice::Word::WordDocumentEditor;

std::shared_ptr<WordDocumentEditor> Reopen(const std::shared_ptr<WordDocumentEditor>& editor)
{
    auto reopened = WordDocumentEditor::Open(editor->SaveToMemory());
    REQUIRE(reopened != nullptr);
    return reopened;
}

std::shared_ptr<ExyokiOffice::Word::Paragraph> AddRevisionParagraph(
    const std::shared_ptr<WordDocumentEditor>& editor)
{
    auto paragraph = editor->AddParagraph();
    REQUIRE(paragraph != nullptr);
    auto lowParagraph = paragraph->GetLowLevelApi();
    REQUIRE(lowParagraph != nullptr);

    auto insertion = lowParagraph->AppendChild<W::InsertedRun>();
    REQUIRE(insertion != nullptr);
    insertion->SetId(ExyokiOffice::StringValue("1"));
    insertion->SetAuthor(ExyokiOffice::StringValue("Alice"));
    auto insertedRun = insertion->AppendChild<W::Run>();
    REQUIRE(insertedRun != nullptr);
    auto insertedText = insertedRun->AppendChild<W::Text>();
    REQUIRE(insertedText != nullptr);
    insertedText->SetText("new");

    auto deletion = lowParagraph->AppendChild<W::DeletedRun>();
    REQUIRE(deletion != nullptr);
    deletion->SetId(ExyokiOffice::StringValue("2"));
    deletion->SetAuthor(ExyokiOffice::StringValue("Bob"));
    auto deletedRun = deletion->AppendChild<W::Run>();
    REQUIRE(deletedRun != nullptr);
    auto deletedText = deletedRun->AppendChild<W::DeletedText>();
    REQUIRE(deletedText != nullptr);
    deletedText->SetText("old");

    return paragraph;
}
} // namespace

TEST_SUITE("WordRevisionTests")
{

    TEST_CASE("WordDocumentEditor::Revisions reads insertion and deletion metadata in body order [unit] [word] [word-revision]")
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);
        AddRevisionParagraph(editor);

        auto revisions = editor->Revisions();
        REQUIRE(revisions.size() == 2);
        CHECK(revisions[0]->Type() == RevisionType::Insertion);
        CHECK(revisions[0]->GetId() == "1");
        CHECK(revisions[0]->GetAuthor() == "Alice");
        CHECK(revisions[0]->Text() == "new");
        CHECK(revisions[1]->Type() == RevisionType::Deletion);
        CHECK(revisions[1]->GetId() == "2");
        CHECK(revisions[1]->GetAuthor() == "Bob");
        CHECK(revisions[1]->Text() == "old");
    }

    TEST_CASE("AcceptAllRevisions keeps insertions and removes deleted text across round-trip [unit] [word] [word-revision]")
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);
        auto paragraph = AddRevisionParagraph(editor);
        CHECK(paragraph->PlainText() == "new");

        CHECK(editor->AcceptAllRevisions() == 2);
        CHECK(editor->Revisions().empty());
        REQUIRE(editor->Paragraphs().size() == 1);
        CHECK(editor->Paragraphs().front()->PlainText() == "new");

        auto reopened = Reopen(editor);
        CHECK(reopened->Revisions().empty());
        REQUIRE(reopened->Paragraphs().size() == 1);
        CHECK(reopened->Paragraphs().front()->PlainText() == "new");
    }

    TEST_CASE("Accepting an insertion promotes its content without recreating it [unit] [word] [word-revision]")
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);
        auto paragraph = AddRevisionParagraph(editor);

        auto lowParagraph = paragraph->GetLowLevelApi();
        REQUIRE(lowParagraph != nullptr);
        auto insertion = lowParagraph->GetFirstChildOfType<W::InsertedRun>();
        REQUIRE(insertion != nullptr);
        auto insertedRun = insertion->GetFirstChildOfType<W::Run>();
        REQUIRE(insertedRun != nullptr);

        auto revisions = editor->Revisions();
        REQUIRE(revisions.size() == 2);
        REQUIRE(revisions[0]->Type() == RevisionType::Insertion);
        CHECK(revisions[0]->Accept());

        // Unwrapping moves the run out of the w:ins rather than copying it, so a
        // wrapper taken beforehand still views the very same node, now reparented.
        CHECK_FALSE(insertedRun->IsNull());
        CHECK(insertedRun->Parent()->IsSameNode(*lowParagraph));
        CHECK(insertedRun->GetFirstChildOfType<W::Text>()->GetText() == "new");

        // The w:ins itself is destroyed. The `insertion` wrapper above is not the
        // one the operation was handed, so by contract it is stale from here on
        // and must not be touched - only the tree is queried.
        CHECK(lowParagraph->GetFirstChildOfType<W::InsertedRun>() == nullptr);
    }

    TEST_CASE("RejectAllRevisions removes insertions and restores deleted text nodes [unit] [word] [word-revision]")
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);
        AddRevisionParagraph(editor);

        CHECK(editor->RejectAllRevisions() == 2);
        CHECK(editor->Revisions().empty());
        REQUIRE(editor->Paragraphs().size() == 1);
        CHECK(editor->Paragraphs().front()->PlainText() == "old");

        auto reopened = Reopen(editor);
        CHECK(reopened->Revisions().empty());
        REQUIRE(reopened->Paragraphs().size() == 1);
        CHECK(reopened->Paragraphs().front()->PlainText() == "old");
    }

    TEST_CASE("CompareWith creates accept/rejectable tracked paragraph changes [unit] [word] [word-revision]")
    {
        auto original = WordDocumentEditor::CreateNew();
        auto revised = WordDocumentEditor::CreateNew();
        REQUIRE(original != nullptr);
        REQUIRE(revised != nullptr);

        original->AddParagraph("Alpha");
        original->AddParagraph("Beta");
        revised->AddParagraph("Alpha");
        revised->AddParagraph("Gamma");
        revised->AddParagraph("Delta");

        CHECK(original->CompareWith(*revised, {"Reviewer", {}, "2026-07-08T12:00:00Z"}) == 3);
        auto revisions = original->Revisions();
        REQUIRE(revisions.size() == 3);
        CHECK(revisions[0]->Type() == RevisionType::Deletion);
        CHECK(revisions[0]->Text() == "Beta");
        CHECK(revisions[1]->Type() == RevisionType::Insertion);
        CHECK(revisions[1]->Text() == "Gamma");
        CHECK(revisions[1]->GetAuthor() == "Reviewer");
        CHECK(revisions[2]->Type() == RevisionType::Insertion);
        CHECK(revisions[2]->Text() == "Delta");

        auto accepted = Reopen(original);
        CHECK(accepted->AcceptAllRevisions() == 3);
        auto acceptedParagraphs = accepted->Paragraphs();
        REQUIRE(acceptedParagraphs.size() == 3);
        CHECK(acceptedParagraphs[0]->PlainText() == "Alpha");
        CHECK(acceptedParagraphs[1]->PlainText() == "Gamma");
        CHECK(acceptedParagraphs[2]->PlainText() == "Delta");

        auto rejected = Reopen(original);
        CHECK(rejected->RejectAllRevisions() == 3);
        auto rejectedParagraphs = rejected->Paragraphs();
        REQUIRE(rejectedParagraphs.size() == 2);
        CHECK(rejectedParagraphs[0]->PlainText() == "Alpha");
        CHECK(rejectedParagraphs[1]->PlainText() == "Beta");
    }

} // TEST_SUITE("WordRevisionTests")
