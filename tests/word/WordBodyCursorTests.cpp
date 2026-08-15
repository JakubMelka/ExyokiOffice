// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "doctest.h"

#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/Wordprocessing.hpp"
#include "ExyokiOffice/Word/WordDocument.hpp"

#include <memory>
#include <string>
#include <vector>

namespace
{
using ExyokiOffice::OpenXMLElement;
using ExyokiOffice::OpenXmlQualifiedName;
using ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Body;
using ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Paragraph;
using ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::SectionProperties;
using ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Table;
using ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::Text;
using ExyokiOffice::Word::WordDocumentEditor;

constexpr std::string_view kWordNamespace = "http://schemas.openxmlformats.org/wordprocessingml/2006/main";

std::shared_ptr<Body> GetBody(const WordDocumentEditor::Ptr& editor)
{
    if (!editor || !editor->GetDocument())
    {
        return nullptr;
    }

    auto mainPart = editor->GetDocument()->GetMainDocumentPart();
    if (!mainPart)
    {
        return nullptr;
    }

    auto document = mainPart->GetTypedRootElement();
    if (!document)
    {
        return nullptr;
    }

    return document->GetFirstChildOfType<Body>();
}

std::string ParagraphText(const std::shared_ptr<OpenXMLElement>& element)
{
    auto paragraph = std::dynamic_pointer_cast<Paragraph>(element);
    if (!paragraph)
    {
        return {};
    }

    std::string result;
    for (const auto& text : paragraph->Descendants<Text>())
    {
        if (text)
        {
            result += std::string(text->GetText());
        }
    }
    return result;
}

std::vector<std::string> BodyShape(const WordDocumentEditor::Ptr& editor)
{
    std::vector<std::string> result;
    auto body = GetBody(editor);
    if (!body)
    {
        return result;
    }

    for (const auto& child : body->Children())
    {
        if (std::dynamic_pointer_cast<Paragraph>(child))
        {
            result.push_back("p:" + ParagraphText(child));
        }
        else if (std::dynamic_pointer_cast<Table>(child))
        {
            result.push_back("tbl");
        }
        else if (child && child->QualifiedName() == OpenXmlQualifiedName(kWordNamespace, "sectPr"))
        {
            result.push_back("sectPr");
        }
        else
        {
            result.push_back("other");
        }
    }
    return result;
}

void AddTrailingSectionProperties(const WordDocumentEditor::Ptr& editor)
{
    auto body = GetBody(editor);
    REQUIRE(body != nullptr);
    REQUIRE(body->AppendChild<SectionProperties>() != nullptr);
}

} // namespace

TEST_SUITE("WordBodyCursorTests")
{

    TEST_CASE("WordDocumentEditor::BodyCursor inserts at body start and logical body end [unit] [word] [word-body-cursor]")
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);

        AddTrailingSectionProperties(editor);

        auto first = editor->Body().InsertParagraph("First");
        REQUIRE(first != nullptr);
        CHECK(BodyShape(editor) == std::vector<std::string>{"p:First", "sectPr"});

        auto last = editor->Body().InsertParagraph("Last");
        REQUIRE(last != nullptr);
        CHECK(BodyShape(editor) == std::vector<std::string>{"p:First", "p:Last", "sectPr"});

        auto start = editor->BodyStart().InsertParagraph("Start");
        REQUIRE(start != nullptr);
        CHECK(BodyShape(editor) == std::vector<std::string>{"p:Start", "p:First", "p:Last", "sectPr"});
    }

    TEST_CASE("WordDocumentEditor::BodyCursor inserts before and after paragraph anchors [unit] [word] [word-body-cursor]")
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);

        auto first = editor->Body().InsertParagraph("A");
        auto last = editor->Body().InsertParagraph("D");
        REQUIRE(first != nullptr);
        REQUIRE(last != nullptr);

        auto beforeLast = editor->Before(last).InsertParagraph("C");
        REQUIRE(beforeLast != nullptr);
        CHECK(BodyShape(editor) == std::vector<std::string>{"p:A", "p:C", "p:D"});

        auto afterFirst = editor->After(first).InsertParagraph("B");
        REQUIRE(afterFirst != nullptr);
        CHECK(BodyShape(editor) == std::vector<std::string>{"p:A", "p:B", "p:C", "p:D"});
    }

    TEST_CASE("WordDocumentEditor::BodyCursor inserts tables before final section properties [unit] [word] [word-body-cursor]")
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);

        auto paragraph = editor->Body().InsertParagraph("Before table");
        REQUIRE(paragraph != nullptr);
        AddTrailingSectionProperties(editor);

        auto table = editor->After(paragraph).InsertTable(2, 3);
        REQUIRE(table != nullptr);
        CHECK(table->GetRowCount() == 2);
        CHECK(table->GetColumnCount() == 3);
        CHECK(BodyShape(editor) == std::vector<std::string>{"p:Before table", "tbl", "sectPr"});

        auto heading = editor->Before(table).InsertParagraph("Table heading");
        REQUIRE(heading != nullptr);
        CHECK(BodyShape(editor) == std::vector<std::string>{"p:Before table", "p:Table heading", "tbl", "sectPr"});
    }

    TEST_CASE("WordDocumentEditor append helpers use the logical body end [unit] [word] [word-body-cursor]")
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);

        AddTrailingSectionProperties(editor);

        REQUIRE(editor->AddParagraph("Append paragraph") != nullptr);
        REQUIRE(editor->AddTable(1, 1) != nullptr);
        CHECK(BodyShape(editor) == std::vector<std::string>{"p:Append paragraph", "tbl", "sectPr"});
    }

    TEST_CASE("WordDocumentEditor::BodyCursor preserves trailing section properties after round trip [unit] [word] [word-body-cursor]")
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);

        REQUIRE(editor->Body().InsertParagraph("Existing") != nullptr);
        AddTrailingSectionProperties(editor);
        REQUIRE(editor->Body().InsertParagraph("Inserted") != nullptr);
        CHECK(BodyShape(editor) == std::vector<std::string>{"p:Existing", "p:Inserted", "sectPr"});

        auto packageBytes = editor->SaveToMemory();
        REQUIRE(!packageBytes.empty());

        auto reopened = WordDocumentEditor::Open(packageBytes);
        REQUIRE(reopened != nullptr);
        CHECK(BodyShape(reopened) == std::vector<std::string>{"p:Existing", "p:Inserted", "sectPr"});
    }

    TEST_CASE("WordDocumentEditor::BodyCursor edits an opened existing document [unit] [word] [word-body-cursor]")
    {
        auto created = WordDocumentEditor::CreateNew();
        REQUIRE(created != nullptr);

        REQUIRE(created->Body().InsertParagraph("First") != nullptr);
        REQUIRE(created->Body().InsertParagraph("Third") != nullptr);
        AddTrailingSectionProperties(created);

        auto packageBytes = created->SaveToMemory();
        REQUIRE(!packageBytes.empty());

        auto opened = WordDocumentEditor::Open(packageBytes);
        REQUIRE(opened != nullptr);
        CHECK(BodyShape(opened) == std::vector<std::string>{"p:First", "p:Third", "sectPr"});

        auto body = GetBody(opened);
        REQUIRE(body != nullptr);
        auto paragraphs = body->Elements<Paragraph>();
        REQUIRE(paragraphs.size() == 2);

        auto first = std::make_shared<ExyokiOffice::Word::Paragraph>(paragraphs[0]);
        auto third = std::make_shared<ExyokiOffice::Word::Paragraph>(paragraphs[1]);

        REQUIRE(opened->After(first).InsertParagraph("Second") != nullptr);
        REQUIRE(opened->Before(third).InsertTable(1, 2) != nullptr);
        REQUIRE(opened->Body().InsertParagraph("Fourth") != nullptr);

        CHECK(BodyShape(opened) == std::vector<std::string>{"p:First", "p:Second", "tbl", "p:Third", "p:Fourth", "sectPr"});

        auto editedBytes = opened->SaveToMemory();
        REQUIRE(!editedBytes.empty());

        auto reopened = WordDocumentEditor::Open(editedBytes);
        REQUIRE(reopened != nullptr);
        CHECK(BodyShape(reopened) == std::vector<std::string>{"p:First", "p:Second", "tbl", "p:Third", "p:Fourth", "sectPr"});
    }

    TEST_CASE("WordDocumentEditor::BodyCursor reports invalid cursors [unit] [word] [word-body-cursor]")
    {
        WordDocumentEditor emptyEditor;
        auto emptyBody = emptyEditor.Body();
        CHECK_FALSE(emptyBody.IsValid());
        CHECK(emptyBody.InsertParagraph("No document") == nullptr);
        CHECK(emptyBody.InsertTable(1, 1) == nullptr);

        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);

        std::shared_ptr<ExyokiOffice::Word::Paragraph> missingParagraph;
        auto beforeMissing = editor->Before(missingParagraph);
        CHECK_FALSE(beforeMissing.IsValid());
        CHECK(beforeMissing.InsertParagraph("No anchor") == nullptr);
    }

} // TEST_SUITE("WordBodyCursorTests")
