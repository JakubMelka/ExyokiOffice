// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "doctest.h"

#include "ExyokiOffice/Word/WordDocument.hpp"
#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/Wordprocessing.hpp"

namespace
{
namespace W = ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing;

using ExyokiOffice::Word::FieldKind;
using ExyokiOffice::Word::WordDocumentEditor;

struct ReopenedField
{
    std::shared_ptr<WordDocumentEditor> Editor;
    std::shared_ptr<ExyokiOffice::Word::Field> Field;
};

ReopenedField ReopenSingleField(const std::shared_ptr<WordDocumentEditor>& editor)
{
    ReopenedField result;
    result.Editor = WordDocumentEditor::Open(editor->SaveToMemory());
    REQUIRE(result.Editor != nullptr);
    auto fields = result.Editor->Fields();
    REQUIRE(fields.size() == 1);
    result.Field = fields.front();
    return result;
}
} // namespace

TEST_SUITE("WordFieldTests")
{

    TEST_CASE("Paragraph::AddField creates a complex field with instruction result and round-trip support [unit] [word] [word-field]")
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);

        auto paragraph = editor->AddParagraph();
        REQUIRE(paragraph != nullptr);

        auto field = paragraph->AddField("DATE \\\\@ \"yyyy-MM-dd\"", "2026-07-04");
        REQUIRE(field != nullptr);

        CHECK(field->Kind() == FieldKind::Complex);
        CHECK_FALSE(field->IsLayoutDependent());
        CHECK_FALSE(field->IsDirty());
        CHECK(field->GetInstruction() == "DATE \\\\@ \"yyyy-MM-dd\"");
        CHECK(field->GetResult() == "2026-07-04");
        CHECK(paragraph->PlainText() == "2026-07-04");

        auto lowParagraph = paragraph->GetLowLevelApi();
        REQUIRE(lowParagraph != nullptr);
        CHECK(lowParagraph->Descendants<W::FieldChar>().size() == 3);
        CHECK(lowParagraph->Descendants<W::FieldCode>().size() == 1);
        CHECK(lowParagraph->Descendants<W::Text>().size() == 1);

        auto reopened = ReopenSingleField(editor);
        CHECK(reopened.Field->Kind() == FieldKind::Complex);
        CHECK(reopened.Field->GetInstruction() == "DATE \\\\@ \"yyyy-MM-dd\"");
        CHECK(reopened.Field->GetResult() == "2026-07-04");
        CHECK_FALSE(reopened.Field->IsDirty());
    }

    TEST_CASE("Field::SetInstruction invalidates the result without deleting the cached text [unit] [word] [word-field]")
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);

        auto paragraph = editor->AddParagraph();
        REQUIRE(paragraph != nullptr);

        auto field = paragraph->AddField("MERGEFIELD OldName", "Old value");
        REQUIRE(field != nullptr);

        field->SetInstruction("MERGEFIELD CustomerName");

        CHECK(field->GetInstruction() == "MERGEFIELD CustomerName");
        CHECK(field->GetResult() == "Old value");
        CHECK(field->IsDirty());
        CHECK(field->GetBeginFieldCharElement() != nullptr);
        CHECK(field->GetBeginFieldCharElement()->GetDirty().ValueOr(false));

        auto reopened = ReopenSingleField(editor);
        CHECK(reopened.Field->GetInstruction() == "MERGEFIELD CustomerName");
        CHECK(reopened.Field->GetResult() == "Old value");
        CHECK(reopened.Field->IsDirty());
    }

    TEST_CASE("Field::SetResult replaces previous complex field result runs [unit] [word] [word-field]")
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);

        auto paragraph = editor->AddParagraph();
        REQUIRE(paragraph != nullptr);

        auto field = paragraph->AddField("MERGEFIELD CustomerName", "Old");
        REQUIRE(field != nullptr);
        REQUIRE(field->SetResult("Alice"));
        REQUIRE(field->SetResult("Bob"));

        CHECK(field->GetResult() == "Bob");
        CHECK(paragraph->PlainText() == "Bob");

        auto lowParagraph = paragraph->GetLowLevelApi();
        REQUIRE(lowParagraph != nullptr);
        auto textNodes = lowParagraph->Descendants<W::Text>();
        REQUIRE(textNodes.size() == 1);
        CHECK(std::string(textNodes.front()->GetText()) == "Bob");

        auto reopened = ReopenSingleField(editor);
        CHECK(reopened.Field->GetResult() == "Bob");
    }

    TEST_CASE("Paragraph::AddSimpleField creates and edits fldSimple without changing representation [unit] [word] [word-field]")
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);

        auto paragraph = editor->AddParagraph();
        REQUIRE(paragraph != nullptr);

        auto field = paragraph->AddSimpleField("REF BookmarkA \\\\h", "Original");
        REQUIRE(field != nullptr);

        CHECK(field->Kind() == FieldKind::Simple);
        CHECK(field->GetSimpleFieldElement() != nullptr);
        CHECK(field->GetInstruction() == "REF BookmarkA \\\\h");
        CHECK(field->GetResult() == "Original");

        field->SetInstruction("REF BookmarkB \\\\h");
        CHECK(field->IsDirty());
        CHECK(field->SetResult("Updated"));
        CHECK(field->GetInstruction() == "REF BookmarkB \\\\h");
        CHECK(field->GetResult() == "Updated");
        CHECK(paragraph->PlainText() == "Updated");

        auto reopened = ReopenSingleField(editor);
        CHECK(reopened.Field->Kind() == FieldKind::Simple);
        CHECK(reopened.Field->GetInstruction() == "REF BookmarkB \\\\h");
        CHECK(reopened.Field->GetResult() == "Updated");
        CHECK(reopened.Field->IsDirty());
    }

    TEST_CASE("Layout-dependent complex fields are marked dirty and their cached result is not fabricated [unit] [word] [word-field]")
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);

        auto paragraph = editor->AddParagraph();
        REQUIRE(paragraph != nullptr);

        auto field = paragraph->AddField(" PAGE \\\\* Arabic ", "7");
        REQUIRE(field != nullptr);

        CHECK(field->IsLayoutDependent());
        CHECK(field->IsDirty());
        CHECK(field->GetResult() == "7");

        CHECK_FALSE(field->SetResult("999"));
        CHECK(field->GetResult() == "7");
        CHECK(paragraph->PlainText() == "7");
        CHECK(field->GetBeginFieldCharElement()->GetDirty().ValueOr(false));

        auto reopened = ReopenSingleField(editor);
        CHECK(reopened.Field->IsLayoutDependent());
        CHECK(reopened.Field->IsDirty());
        CHECK(reopened.Field->GetResult() == "7");
    }

    TEST_CASE("Layout-dependent simple fields are marked dirty and preserve their stored result [unit] [word] [word-field]")
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);

        auto paragraph = editor->AddParagraph();
        REQUIRE(paragraph != nullptr);

        auto field = paragraph->AddSimpleField("NUMPAGES", "12");
        REQUIRE(field != nullptr);

        CHECK(field->IsLayoutDependent());
        CHECK(field->IsDirty());
        CHECK_FALSE(field->SetResult("99"));
        CHECK(field->GetResult() == "12");
        CHECK(field->GetSimpleFieldElement()->GetDirty().ValueOr(false));
    }

    TEST_CASE("WordDocumentEditor::Fields returns simple and complex fields in body order [unit] [word] [word-field]")
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);

        auto first = editor->AddParagraph();
        REQUIRE(first != nullptr);
        REQUIRE(first->AddSimpleField("REF First", "A") != nullptr);

        auto second = editor->AddParagraph("plain");
        REQUIRE(second != nullptr);
        REQUIRE(second->AddField("MERGEFIELD Second", "B") != nullptr);

        auto third = editor->AddParagraph();
        REQUIRE(third != nullptr);
        REQUIRE(third->AddField("SECTIONPAGES", "3") != nullptr);

        auto fields = editor->Fields();
        REQUIRE(fields.size() == 3);
        CHECK(fields[0]->Kind() == FieldKind::Simple);
        CHECK(fields[0]->GetInstruction() == "REF First");
        CHECK(fields[1]->Kind() == FieldKind::Complex);
        CHECK(fields[1]->GetInstruction() == "MERGEFIELD Second");
        CHECK(fields[2]->IsLayoutDependent());
    }

    TEST_CASE("Paragraph::Fields ignores malformed complex fields without an end marker [unit] [word] [word-field]")
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);

        auto paragraph = editor->AddParagraph();
        REQUIRE(paragraph != nullptr);
        auto lowParagraph = paragraph->GetLowLevelApi();
        REQUIRE(lowParagraph != nullptr);

        auto run = lowParagraph->AppendChild<W::Run>();
        REQUIRE(run != nullptr);
        auto begin = run->AppendChild<W::FieldChar>();
        REQUIRE(begin != nullptr);
        begin->SetFieldCharType(ExyokiOffice::EnumValue<W::FieldCharValues>(W::FieldCharValues::Begin));

        CHECK(paragraph->Fields().empty());
        CHECK(editor->Fields().empty());
    }

} // TEST_SUITE("WordFieldTests")
