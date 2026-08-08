// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "doctest.h"

#include "ExyokiOffice/Packaging/GeneratedParts.hpp"
#include "ExyokiOffice/Word/WordDocument.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <string>
#include <vector>

namespace
{
using ExyokiOffice::OpenXmlQualifiedName;
using ExyokiOffice::Word::HeaderFooterType;
using ExyokiOffice::Word::Section;
using ExyokiOffice::Word::SectionStartType;
using ExyokiOffice::Word::WordDocumentEditor;

constexpr std::string_view kWordNamespace =
    "http://schemas.openxmlformats.org/wordprocessingml/2006/main";

ExyokiOffice::Size CountReferences(const Section::Ptr& section, std::string_view localName)
{
    if (!section || !section->GetLowLevelApi())
    {
        return 0;
    }

    const OpenXmlQualifiedName name(kWordNamespace, localName);
    ExyokiOffice::Size count = 0;
    for (const auto& child : section->GetLowLevelApi()->Children())
    {
        if (child && child->QualifiedName() == name)
        {
            ++count;
        }
    }
    return count;
}

std::vector<std::string> HeaderTexts(const Section::Ptr& section)
{
    std::vector<std::string> texts;
    for (const auto type : {HeaderFooterType::Default, HeaderFooterType::Even, HeaderFooterType::First})
    {
        auto content = section->GetHeader(type);
        texts.push_back(content ? content->PlainText() : std::string{});
    }
    return texts;
}

std::vector<std::string> FooterTexts(const Section::Ptr& section)
{
    std::vector<std::string> texts;
    for (const auto type : {HeaderFooterType::Default, HeaderFooterType::Even, HeaderFooterType::First})
    {
        auto content = section->GetFooter(type);
        texts.push_back(content ? content->PlainText() : std::string{});
    }
    return texts;
}

} // namespace

TEST_SUITE("WordHeaderFooterTests")
{

    TEST_CASE("Word section headers and footers support default even and first references [unit] [word] [word-header-footer]")
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);

        auto section = editor->EnsureFinalSection();
        REQUIRE(section != nullptr);

        section->SetHeaderText(HeaderFooterType::Default, "Default header")
            .SetHeaderText(HeaderFooterType::Even, "Even header")
            .SetHeaderText(HeaderFooterType::First, "First header")
            .SetFooterText(HeaderFooterType::Default, "Default footer")
            .SetFooterText(HeaderFooterType::Even, "Even footer")
            .SetFooterText(HeaderFooterType::First, "First footer");

        CHECK(section->HasHeader(HeaderFooterType::Default));
        CHECK(section->HasHeader(HeaderFooterType::Even));
        CHECK(section->HasHeader(HeaderFooterType::First));
        CHECK(section->HasFooter(HeaderFooterType::Default));
        CHECK(section->HasFooter(HeaderFooterType::Even));
        CHECK(section->HasFooter(HeaderFooterType::First));
        CHECK(CountReferences(section, "headerReference") == 3);
        CHECK(CountReferences(section, "footerReference") == 3);

        auto mainPart = editor->GetDocument()->GetMainDocumentPart();
        REQUIRE(mainPart != nullptr);
        CHECK(mainPart->GetHeaderParts().size() == 3);
        CHECK(mainPart->GetFooterParts().size() == 3);

        auto reopened = WordDocumentEditor::Open(editor->SaveToMemory());
        REQUIRE(reopened != nullptr);
        auto sections = reopened->Sections();
        REQUIRE(sections.size() == 1);

        CHECK(HeaderTexts(sections[0]) == std::vector<std::string>{
                                              "Default header", "Even header", "First header"});
        CHECK(FooterTexts(sections[0]) == std::vector<std::string>{
                                              "Default footer", "Even footer", "First footer"});
        CHECK(CountReferences(sections[0], "headerReference") == 3);
        CHECK(CountReferences(sections[0], "footerReference") == 3);

        auto reopenedMainPart = reopened->GetDocument()->GetMainDocumentPart();
        REQUIRE(reopenedMainPart != nullptr);
        CHECK(reopenedMainPart->GetHeaderParts().size() == 3);
        CHECK(reopenedMainPart->GetFooterParts().size() == 3);
    }

    TEST_CASE("Header footer content wrapper edits paragraphs without creating duplicate references [unit] [word] [word-header-footer]")
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);

        auto section = editor->EnsureFinalSection();
        REQUIRE(section != nullptr);

        auto header = section->EnsureHeader(HeaderFooterType::Default);
        REQUIRE(header != nullptr);
        CHECK(header->IsHeader());
        CHECK_FALSE(header->IsFooter());
        auto relationshipId = header->RelationshipId();
        REQUIRE(!relationshipId.empty());

        REQUIRE(header->AddParagraph("Line one") != nullptr);
        REQUIRE(header->AddParagraph("Line two") != nullptr);
        CHECK(header->Paragraphs().size() == 2);
        CHECK(header->PlainText() == "Line oneLine two");

        auto sameHeader = section->EnsureHeader(HeaderFooterType::Default);
        REQUIRE(sameHeader != nullptr);
        CHECK(sameHeader->RelationshipId() == relationshipId);
        sameHeader->SetText("Replacement");

        CHECK(CountReferences(section, "headerReference") == 1);
        auto mainPart = editor->GetDocument()->GetMainDocumentPart();
        REQUIRE(mainPart != nullptr);
        CHECK(mainPart->GetHeaderParts().size() == 1);

        auto reopened = WordDocumentEditor::Open(editor->SaveToMemory());
        REQUIRE(reopened != nullptr);
        auto sections = reopened->Sections();
        REQUIRE(sections.size() == 1);
        auto reopenedHeader = sections[0]->GetHeader(HeaderFooterType::Default);
        REQUIRE(reopenedHeader != nullptr);
        CHECK(reopenedHeader->RelationshipId() == relationshipId);
        CHECK(reopenedHeader->Paragraphs().size() == 1);
        CHECK(reopenedHeader->PlainText() == "Replacement");
    }

    TEST_CASE("Link to previous removes section references and unused parts [unit] [word] [word-header-footer]")
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);

        auto first = editor->Body().InsertSectionBreak(SectionStartType::NextPage);
        REQUIRE(first != nullptr);
        first->SetHeaderText(HeaderFooterType::Default, "Section one header")
            .SetFooterText(HeaderFooterType::Default, "Section one footer");

        auto second = editor->EnsureFinalSection();
        REQUIRE(second != nullptr);
        second->SetHeaderText(HeaderFooterType::Default, "Section two header")
            .SetFooterText(HeaderFooterType::Default, "Section two footer");

        auto mainPart = editor->GetDocument()->GetMainDocumentPart();
        REQUIRE(mainPart != nullptr);
        CHECK(mainPart->GetHeaderParts().size() == 2);
        CHECK(mainPart->GetFooterParts().size() == 2);

        CHECK(second->LinkHeaderToPrevious(HeaderFooterType::Default));
        CHECK(second->LinkFooterToPrevious(HeaderFooterType::Default));
        CHECK_FALSE(second->HasHeader(HeaderFooterType::Default));
        CHECK_FALSE(second->HasFooter(HeaderFooterType::Default));
        CHECK(second->IsHeaderLinkedToPrevious(HeaderFooterType::Default));
        CHECK(second->IsFooterLinkedToPrevious(HeaderFooterType::Default));
        CHECK(mainPart->GetHeaderParts().size() == 1);
        CHECK(mainPart->GetFooterParts().size() == 1);

        auto reopened = WordDocumentEditor::Open(editor->SaveToMemory());
        REQUIRE(reopened != nullptr);
        auto sections = reopened->Sections();
        REQUIRE(sections.size() == 2);

        CHECK(sections[0]->GetHeader(HeaderFooterType::Default)->PlainText() == "Section one header");
        CHECK(sections[0]->GetFooter(HeaderFooterType::Default)->PlainText() == "Section one footer");
        CHECK_FALSE(sections[1]->HasHeader(HeaderFooterType::Default));
        CHECK_FALSE(sections[1]->HasFooter(HeaderFooterType::Default));

        auto reopenedMainPart = reopened->GetDocument()->GetMainDocumentPart();
        REQUIRE(reopenedMainPart != nullptr);
        CHECK(reopenedMainPart->GetHeaderParts().size() == 1);
        CHECK(reopenedMainPart->GetFooterParts().size() == 1);
    }

    TEST_CASE("Removing one header type preserves the other section header references [unit] [word] [word-header-footer]")
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);

        auto section = editor->EnsureFinalSection();
        REQUIRE(section != nullptr);
        section->SetHeaderText(HeaderFooterType::Default, "Default")
            .SetHeaderText(HeaderFooterType::Even, "Even")
            .SetFooterText(HeaderFooterType::First, "First footer");

        auto mainPart = editor->GetDocument()->GetMainDocumentPart();
        REQUIRE(mainPart != nullptr);
        CHECK(mainPart->GetHeaderParts().size() == 2);
        CHECK(mainPart->GetFooterParts().size() == 1);

        CHECK(section->RemoveHeader(HeaderFooterType::Even));
        CHECK_FALSE(section->RemoveHeader(HeaderFooterType::Even));
        CHECK(section->HasHeader(HeaderFooterType::Default));
        CHECK_FALSE(section->HasHeader(HeaderFooterType::Even));
        CHECK(section->HasFooter(HeaderFooterType::First));
        CHECK(mainPart->GetHeaderParts().size() == 1);
        CHECK(mainPart->GetFooterParts().size() == 1);

        auto reopened = WordDocumentEditor::Open(editor->SaveToMemory());
        REQUIRE(reopened != nullptr);
        auto sections = reopened->Sections();
        REQUIRE(sections.size() == 1);
        CHECK(sections[0]->GetHeader(HeaderFooterType::Default)->PlainText() == "Default");
        CHECK(sections[0]->GetHeader(HeaderFooterType::Even) == nullptr);
        CHECK(sections[0]->GetFooter(HeaderFooterType::First)->PlainText() == "First footer");
    }

    TEST_CASE("Header and footer references stay well formed XML [unit] [word] [header-footer]")
    {
        // The reference carries r:id once. Writing it a second time as a raw
        // prefixed attribute produced a duplicate that made Word reject the
        // whole part.
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);
        auto section = editor->EnsureFinalSection();
        REQUIRE(section != nullptr);
        section->SetHeaderText(HeaderFooterType::Default, "Header");
        section->SetFooterText(HeaderFooterType::Default, "Footer");

        const auto xml = editor->GetDocument()->GetMainDocumentPart()->GetXmlString();
        const auto headerStart = xml.find("<w:headerReference");
        REQUIRE(headerStart != std::string::npos);
        const auto headerEnd = xml.find('>', headerStart);
        REQUIRE(headerEnd != std::string::npos);
        const auto headerReference = xml.substr(headerStart, headerEnd - headerStart);
        CHECK(headerReference.find("r:id") != std::string::npos);
        CHECK(headerReference.find("r:id") == headerReference.rfind("r:id"));
        CHECK(headerReference.find("xmlns:r") == std::string::npos);

        // Word re-reads what it wrote, and so does the round trip.
        auto reopened = WordDocumentEditor::Open(editor->SaveToMemory());
        REQUIRE(reopened != nullptr);
        auto sections = reopened->Sections();
        REQUIRE(sections.size() == 1);
        REQUIRE(sections[0]->GetHeader(HeaderFooterType::Default) != nullptr);
        CHECK(sections[0]->GetHeader(HeaderFooterType::Default)->PlainText() == "Header");
    }

} // TEST_SUITE("WordHeaderFooterTests")
