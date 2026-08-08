// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "doctest.h"

#include "ExyokiOffice/OpenXmlPackageValidator.hpp"
#include "ExyokiOffice/Word/WordDocument.hpp"
#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/Wordprocessing.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace
{

using ExyokiOffice::Word::BreakType;
using ExyokiOffice::Word::WordDocumentEditor;
namespace Wordprocessing = ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing;

std::shared_ptr<Wordprocessing::Break> FirstBreak(const std::shared_ptr<ExyokiOffice::Word::Paragraph>& paragraph)
{
    if (!paragraph || !paragraph->GetLowLevelApi())
    {
        return nullptr;
    }
    for (const auto& element : paragraph->GetLowLevelApi()->Descendants<Wordprocessing::Break>())
    {
        if (element)
        {
            return element;
        }
    }
    return nullptr;
}

WordDocumentEditor::Ptr RoundTrip(const WordDocumentEditor::Ptr& editor)
{
    if (!editor)
    {
        return nullptr;
    }
    const auto bytes = editor->SaveToMemory();
    if (bytes.empty())
    {
        return nullptr;
    }
    return WordDocumentEditor::Open(bytes);
}

} // namespace

TEST_SUITE("WordEditorAuthoringTests")
{

    TEST_CASE("AddBreak inserts line, page, and column breaks that round-trip [unit] [word] [word-authoring]")
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);

        auto linePara = editor->AddParagraph("before");
        REQUIRE(linePara != nullptr);
        REQUIRE(linePara->AddBreak() != nullptr);
        linePara->AddText("after");

        auto pagePara = editor->AddParagraph();
        REQUIRE(pagePara != nullptr);
        REQUIRE(pagePara->AddBreak(BreakType::Page) != nullptr);

        auto columnPara = editor->AddParagraph();
        REQUIRE(columnPara != nullptr);
        REQUIRE(columnPara->AddBreak(BreakType::Column) != nullptr);

        auto reopened = RoundTrip(editor);
        REQUIRE(reopened != nullptr);
        auto paragraphs = reopened->Paragraphs();
        REQUIRE(paragraphs.size() == 3);

        auto lineBreak = FirstBreak(paragraphs[0]);
        REQUIRE(lineBreak != nullptr);
        CHECK_FALSE(lineBreak->GetType().IsDefined());
        CHECK(paragraphs[0]->PlainText() == "beforeafter");

        auto pageBreak = FirstBreak(paragraphs[1]);
        REQUIRE(pageBreak != nullptr);
        REQUIRE(pageBreak->GetType().IsDefined());
        CHECK(pageBreak->GetType().Value().GetValue() == Wordprocessing::BreakValues::Page);

        auto columnBreak = FirstBreak(paragraphs[2]);
        REQUIRE(columnBreak != nullptr);
        REQUIRE(columnBreak->GetType().IsDefined());
        CHECK(columnBreak->GetType().Value().GetValue() == Wordprocessing::BreakValues::Column);
    }

    TEST_CASE("Run::AddBreak appends the break after existing run content [unit] [word] [word-authoring]")
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);

        auto paragraph = editor->AddParagraph();
        REQUIRE(paragraph != nullptr);
        auto run = paragraph->AddRun();
        REQUIRE(run != nullptr);
        run->AddText("first line");
        run->AddBreak().AddText("second line");

        auto lowLevelRun = run->GetLowLevelApi();
        REQUIRE(lowLevelRun != nullptr);

        bool sawTextBeforeBreak = false;
        bool sawBreak = false;
        bool sawTextAfterBreak = false;
        for (const auto& child : lowLevelRun->Children())
        {
            if (std::dynamic_pointer_cast<Wordprocessing::Break>(child))
            {
                sawBreak = true;
            }
            else if (std::dynamic_pointer_cast<Wordprocessing::Text>(child))
            {
                (sawBreak ? sawTextAfterBreak : sawTextBeforeBreak) = true;
            }
        }
        CHECK(sawTextBeforeBreak);
        CHECK(sawBreak);
        CHECK(sawTextAfterBreak);
    }

    TEST_CASE("AddPageBreak appends a dedicated page-break paragraph [unit] [word] [word-authoring]")
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);
        REQUIRE(editor->AddParagraph("page one") != nullptr);
        REQUIRE(editor->AddPageBreak() != nullptr);
        REQUIRE(editor->AddParagraph("page two") != nullptr);

        auto reopened = RoundTrip(editor);
        REQUIRE(reopened != nullptr);
        auto paragraphs = reopened->Paragraphs();
        REQUIRE(paragraphs.size() == 3);

        auto pageBreak = FirstBreak(paragraphs[1]);
        REQUIRE(pageBreak != nullptr);
        REQUIRE(pageBreak->GetType().IsDefined());
        CHECK(pageBreak->GetType().Value().GetValue() == Wordprocessing::BreakValues::Page);
    }

    TEST_CASE("AddHeading applies HeadingN styles and creates them only once [unit] [word] [word-authoring]")
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);

        auto heading = editor->AddHeading("Chapter");
        REQUIRE(heading != nullptr);
        CHECK(heading->GetStyleId() == "Heading1");

        auto subHeading = editor->AddHeading("Section", 2);
        REQUIRE(subHeading != nullptr);
        CHECK(subHeading->GetStyleId() == "Heading2");

        auto styles = editor->Styles();
        CHECK(styles.HasStyle("Normal"));
        CHECK(styles.HasStyle("Heading1"));
        CHECK(styles.HasStyle("Heading2"));

        auto definition = styles.GetStyle("Heading1");
        REQUIRE(definition.has_value());
        CHECK(definition->BasedOnStyleId == "Normal");
        CHECK(definition->NextStyleId == "Normal");
        CHECK(definition->IsPrimary);
        CHECK_FALSE(definition->IsCustom);

        // Note: the DOM factory types re-read children by qualified name, so a
        // style's w:rPr/w:pPr cannot be fetched via GetFirstChildOfType with the
        // Style* wrapper types; assert the created formatting through the part XML.
        auto stylesPart = styles.GetStylesPart();
        REQUIRE(stylesPart != nullptr);
        const auto stylesXml = stylesPart->GetXmlString();
        CHECK(stylesXml.find("<w:keepNext") != std::string::npos);
        CHECK(stylesXml.find("<w:outlineLvl w:val=\"0\"") != std::string::npos);
        CHECK(stylesXml.find("<w:outlineLvl w:val=\"1\"") != std::string::npos);
        CHECK(stylesXml.find("w:val=\"2F5496\"") != std::string::npos);
        CHECK(stylesXml.find("<w:sz w:val=\"32\"") != std::string::npos);
        CHECK(stylesXml.find("<w:sz w:val=\"26\"") != std::string::npos);

        SUBCASE("Repeated headings reuse the existing style definition")
        {
            const auto styleCountBefore = styles.Styles().size();
            REQUIRE(editor->AddHeading("Another chapter") != nullptr);
            CHECK(editor->Styles().Styles().size() == styleCountBefore);
        }

        SUBCASE("Existing style definitions are left untouched")
        {
            auto custom = styles.GetStyle("Heading2");
            REQUIRE(custom.has_value());
            custom->UiPriority = 42;
            REQUIRE(styles.UpdateStyle(*custom));

            REQUIRE(editor->AddHeading("Reused", 2) != nullptr);
            auto after = editor->Styles().GetStyle("Heading2");
            REQUIRE(after.has_value());
            CHECK(after->UiPriority == 42);
        }

        SUBCASE("Levels are clamped to 1-9 and survive a round trip")
        {
            auto tooSmall = editor->AddHeading("Clamped low", 0);
            REQUIRE(tooSmall != nullptr);
            CHECK(tooSmall->GetStyleId() == "Heading1");

            auto tooLarge = editor->AddHeading("Clamped high", 42);
            REQUIRE(tooLarge != nullptr);
            CHECK(tooLarge->GetStyleId() == "Heading9");

            auto reopened = RoundTrip(editor);
            REQUIRE(reopened != nullptr);
            CHECK(reopened->Styles().HasStyle("Heading9"));
            auto paragraphs = reopened->Paragraphs();
            REQUIRE(!paragraphs.empty());
            CHECK(paragraphs.back()->GetStyleId() == "Heading9");
        }
    }

    TEST_CASE("AddTableOfContents inserts a dirty TOC field with the requested levels [unit] [word] [word-authoring]")
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);
        REQUIRE(editor->AddHeading("First chapter") != nullptr);
        REQUIRE(editor->AddTableOfContents(1, 5) != nullptr);

        auto fields = editor->Fields();
        REQUIRE(fields.size() == 1);
        CHECK(fields.front()->GetInstruction().find("TOC \\o \"1-5\"") != std::string::npos);
        CHECK(fields.front()->IsDirty());

        SUBCASE("Levels are clamped and ordered")
        {
            auto second = WordDocumentEditor::CreateNew();
            REQUIRE(second != nullptr);
            REQUIRE(second->AddTableOfContents(7, 2) != nullptr);
            auto secondFields = second->Fields();
            REQUIRE(secondFields.size() == 1);
            CHECK(secondFields.front()->GetInstruction().find("TOC \\o \"7-7\"") != std::string::npos);
        }

        SUBCASE("The field round-trips including the dirty flag")
        {
            auto reopened = RoundTrip(editor);
            REQUIRE(reopened != nullptr);
            auto reopenedFields = reopened->Fields();
            REQUIRE(reopenedFields.size() == 1);
            CHECK(reopenedFields.front()->GetInstruction().find("TOC \\o \"1-5\"") != std::string::npos);
            CHECK(reopenedFields.front()->IsDirty());
        }
    }

    TEST_CASE("Extended document properties round-trip through save and reopen [unit] [word] [word-authoring]")
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);
        auto document = editor->GetDocument();
        REQUIRE(document != nullptr);

        // Deliberately scrambled order to exercise ordered insertion into core.xml.
        document->SetContentStatus("Draft");
        document->SetTitle("Property round trip");
        document->SetCategory("Testing");
        document->SetSubject("Extended properties");
        document->SetCreator("ExyokiOffice tests");
        document->SetKeywords("alpha, beta");
        document->SetDescription("Round-trip description");
        document->SetLastModifiedBy("Unit test");
        document->SetCompany("ExyokiOffice");

        CHECK(document->GetSubject() == "Extended properties");
        CHECK(document->GetKeywords() == "alpha, beta");
        CHECK(document->GetDescription() == "Round-trip description");
        CHECK(document->GetCategory() == "Testing");
        CHECK(document->GetContentStatus() == "Draft");
        CHECK(document->GetCompany() == "ExyokiOffice");

        auto reopened = RoundTrip(editor);
        REQUIRE(reopened != nullptr);
        auto reopenedDocument = reopened->GetDocument();
        REQUIRE(reopenedDocument != nullptr);
        CHECK(reopenedDocument->GetTitle() == "Property round trip");
        CHECK(reopenedDocument->GetCreator() == "ExyokiOffice tests");
        CHECK(reopenedDocument->GetLastModifiedBy() == "Unit test");
        CHECK(reopenedDocument->GetSubject() == "Extended properties");
        CHECK(reopenedDocument->GetKeywords() == "alpha, beta");
        CHECK(reopenedDocument->GetDescription() == "Round-trip description");
        CHECK(reopenedDocument->GetCategory() == "Testing");
        CHECK(reopenedDocument->GetContentStatus() == "Draft");
        CHECK(reopenedDocument->GetCompany() == "ExyokiOffice");

        SUBCASE("core.xml children follow the ECMA-376 canonical sequence")
        {
            auto corePart = reopenedDocument->GetCoreFilePropertiesPart();
            REQUIRE(corePart != nullptr);
            const auto xml = corePart->GetXmlString();

            const std::vector<std::string_view> canonicalOrder = {
                "<cp:category",
                "<cp:contentStatus",
                "<dcterms:created",
                "<dc:creator",
                "<dc:description",
                "<cp:keywords",
                "<cp:lastModifiedBy",
                "<dcterms:modified",
                "<dc:subject",
                "<dc:title",
            };

            ExyokiOffice::Size previous = 0;
            for (const auto& elementName : canonicalOrder)
            {
                CAPTURE(elementName);
                const auto position = xml.find(elementName);
                REQUIRE(position != std::string::npos);
                CHECK(position >= previous);
                previous = position;
            }
        }
    }

    TEST_CASE("Editor-produced packages pass OPC validation [unit] [word] [word-authoring] [opc-validation]")
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);

        editor->GetDocument()->SetTitle("Validation document");
        editor->GetDocument()->SetSubject("Kitchen sink");
        REQUIRE(editor->AddHeading("Heading") != nullptr);
        REQUIRE(editor->AddTableOfContents() != nullptr);
        REQUIRE(editor->AddParagraph("Body text") != nullptr);
        REQUIRE(editor->AddPageBreak() != nullptr);
        auto table = editor->AddTable(2, 2);
        REQUIRE(table != nullptr);
        table->SetCellText(0, 0, "Cell");
        auto listStyle = editor->EnsureBulletedListStyle();
        auto listItem = editor->AddParagraph("List entry");
        REQUIRE(listItem != nullptr);
        listItem->SetListStyle(listStyle);
        auto linkParagraph = editor->AddParagraph();
        REQUIRE(linkParagraph != nullptr);
        REQUIRE(linkParagraph->AddHyperlink("link", "https://example.com") != nullptr);
        auto section = editor->EnsureFinalSection();
        REQUIRE(section != nullptr);
        section->SetHeaderText(ExyokiOffice::Word::HeaderFooterType::Default, "Header");
        section->SetFooterText(ExyokiOffice::Word::HeaderFooterType::Default, "Footer");

        const auto bytes = editor->SaveToMemory();
        REQUIRE(!bytes.empty());

        ExyokiOffice::OpenXmlPackage package;
        REQUIRE(package.LoadFromMemory(bytes));

        const auto result = ExyokiOffice::OpenXmlPackageValidator().Validate(package);
        for (const auto& issue : result.Issues())
        {
            CAPTURE(issue.Message);
            CHECK(issue.Severity != ExyokiOffice::ValidationSeverity::Error);
        }
        CHECK_FALSE(result.HasErrors());
    }

} // TEST_SUITE("WordEditorAuthoringTests")
