// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "doctest.h"

#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/Wordprocessing.hpp"
#include "ExyokiOffice/Word/WordDocument.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <cmath>
#include <memory>
#include <vector>

namespace
{
using ExyokiOffice::MeasurementUnit;
using ExyokiOffice::MeasuringUnits;
using ExyokiOffice::Word::BodyBlockType;
using ExyokiOffice::Word::PageOrientation;
using ExyokiOffice::Word::SectionColumns;
using ExyokiOffice::Word::SectionMargins;
using ExyokiOffice::Word::SectionPageSize;
using ExyokiOffice::Word::SectionStartType;
using ExyokiOffice::Word::WordDocumentEditor;

constexpr std::string_view kWordNamespace =
    "http://schemas.openxmlformats.org/wordprocessingml/2006/main";

int Twips(const MeasuringUnits& value)
{
    return static_cast<int>(std::lround(value.ToTw().GetValue()));
}

MeasuringUnits Inches(ExyokiOffice::Real value)
{
    return MeasuringUnits(value, MeasurementUnit::Inch);
}

MeasuringUnits Points(ExyokiOffice::Real value)
{
    return MeasuringUnits(value, MeasurementUnit::Point);
}

std::vector<BodyBlockType> BodyTypes(const WordDocumentEditor::Ptr& editor)
{
    std::vector<BodyBlockType> result;
    for (const auto& block : editor->BodyBlocks())
    {
        result.push_back(block.Type());
    }
    return result;
}

void CheckPageSize(const ExyokiOffice::Word::Section::Ptr& section,
                   int widthTwips,
                   int heightTwips,
                   PageOrientation orientation)
{
    REQUIRE(section != nullptr);
    auto pageSize = section->GetPageSize();
    REQUIRE(pageSize.has_value());
    CHECK(Twips(pageSize->Width) == widthTwips);
    CHECK(Twips(pageSize->Height) == heightTwips);
    CHECK(pageSize->Orientation == orientation);
}

void CheckMargins(const ExyokiOffice::Word::Section::Ptr& section,
                  int top,
                  int right,
                  int bottom,
                  int left,
                  int header,
                  int footer,
                  int gutter)
{
    REQUIRE(section != nullptr);
    auto margins = section->GetMargins();
    REQUIRE(margins.has_value());
    CHECK(Twips(margins->Top) == top);
    CHECK(Twips(margins->Right) == right);
    CHECK(Twips(margins->Bottom) == bottom);
    CHECK(Twips(margins->Left) == left);
    CHECK(Twips(margins->Header) == header);
    CHECK(Twips(margins->Footer) == footer);
    CHECK(Twips(margins->Gutter) == gutter);
}

ExyokiOffice::Size CountDirectSectionTypeChildren(const ExyokiOffice::Word::Section::Ptr& section)
{
    if (!section || !section->GetLowLevelApi())
    {
        return 0;
    }

    ExyokiOffice::Size count = 0;
    const auto typeName = ExyokiOffice::OpenXmlQualifiedName(kWordNamespace, "type");
    for (const auto& child : section->GetLowLevelApi()->Children())
    {
        if (child && child->QualifiedName() == typeName)
        {
            ++count;
        }
    }
    return count;
}

} // namespace

TEST_SUITE("WordSectionApiTests")
{

    TEST_CASE("WordDocumentEditor section API creates section breaks with page metadata [unit] [word] [word-section-api]")
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);

        REQUIRE(editor->Body().InsertParagraph("Portrait intro") != nullptr);

        auto landscape = editor->Body().InsertSectionBreak(SectionStartType::NextPage);
        REQUIRE(landscape != nullptr);
        landscape->SetPageSize({Inches(11.0), Inches(8.5), PageOrientation::Landscape})
            .SetMargins({Inches(0.7),
                         Inches(0.8),
                         Inches(0.9),
                         Inches(1.0),
                         Inches(0.3),
                         Inches(0.4),
                         Inches(0.2)})
            .SetColumns({2, Inches(0.25), true});

        REQUIRE(editor->Body().InsertParagraph("Landscape body") != nullptr);

        auto finalSection = editor->EnsureFinalSection();
        REQUIRE(finalSection != nullptr);
        finalSection->SetStartType(SectionStartType::Continuous)
            .SetPageSize({Inches(8.5), Inches(11.0), PageOrientation::Portrait})
            .SetMargins({Inches(1.0),
                         Inches(1.0),
                         Inches(1.0),
                         Inches(1.0),
                         Inches(0.5),
                         Inches(0.5),
                         Inches(0.0)});

        CHECK(BodyTypes(editor) == std::vector<BodyBlockType>{
                                       BodyBlockType::Paragraph,
                                       BodyBlockType::Paragraph,
                                       BodyBlockType::Paragraph,
                                       BodyBlockType::Section});

        auto bytes = editor->SaveToMemory();
        REQUIRE(!bytes.empty());

        auto opened = WordDocumentEditor::Open(bytes);
        REQUIRE(opened != nullptr);

        auto sections = opened->Sections();
        REQUIRE(sections.size() == 2);
        CHECK_FALSE(sections[0]->IsFinalBodySection());
        CHECK(sections[0]->GetStartType() == SectionStartType::NextPage);
        CheckPageSize(sections[0], 15840, 12240, PageOrientation::Landscape);
        CheckMargins(sections[0], 1008, 1152, 1296, 1440, 432, 576, 288);

        auto columns = sections[0]->GetColumns();
        REQUIRE(columns.has_value());
        CHECK(columns->Count == 2);
        CHECK(Twips(columns->Spacing) == 360);
        CHECK(columns->Separator);

        CHECK(sections[1]->IsFinalBodySection());
        CHECK(sections[1]->GetStartType() == SectionStartType::Continuous);
        CheckPageSize(sections[1], 12240, 15840, PageOrientation::Portrait);
    }

    TEST_CASE("Section API updates metadata on an opened document and survives round trip [unit] [word] [word-section-api]")
    {
        auto created = WordDocumentEditor::CreateNew();
        REQUIRE(created != nullptr);

        REQUIRE(created->Body().InsertParagraph("Before break") != nullptr);
        auto sectionBreak = created->Body().InsertSectionBreak(SectionStartType::OddPage);
        REQUIRE(sectionBreak != nullptr);
        sectionBreak->SetPageOrientation(PageOrientation::Portrait);
        REQUIRE(created->Body().InsertParagraph("After break") != nullptr);
        REQUIRE(created->EnsureFinalSection() != nullptr);

        auto opened = WordDocumentEditor::Open(created->SaveToMemory());
        REQUIRE(opened != nullptr);
        auto sections = opened->Sections();
        REQUIRE(sections.size() == 2);

        sections[0]->SetStartType(SectionStartType::EvenPage).SetPageSize({Inches(6.0), Inches(9.0), PageOrientation::Portrait}).SetColumns({3, Points(18.0), false});
        sections[1]->SetPageOrientation(PageOrientation::Landscape);

        auto reopened = WordDocumentEditor::Open(opened->SaveToMemory());
        REQUIRE(reopened != nullptr);
        auto reopenedSections = reopened->Sections();
        REQUIRE(reopenedSections.size() == 2);

        CHECK(reopenedSections[0]->GetStartType() == SectionStartType::EvenPage);
        CheckPageSize(reopenedSections[0], 8640, 12960, PageOrientation::Portrait);
        auto columns = reopenedSections[0]->GetColumns();
        REQUIRE(columns.has_value());
        CHECK(columns->Count == 3);
        CHECK(Twips(columns->Spacing) == 360);
        CHECK_FALSE(columns->Separator);

        CHECK(reopenedSections[1]->IsFinalBodySection());
        auto finalPageSize = reopenedSections[1]->GetPageSize();
        CHECK_FALSE(finalPageSize.has_value());
        CHECK(reopenedSections[1]->GetMargins() == std::nullopt);
    }

    TEST_CASE("Section API handles invalid wrappers and creates a missing final section [unit] [word] [word-section-api]")
    {
        ExyokiOffice::Word::Section invalid(nullptr);
        CHECK(invalid.GetStartType() == std::nullopt);
        CHECK(invalid.GetPageSize() == std::nullopt);
        CHECK(invalid.GetMargins() == std::nullopt);
        CHECK(invalid.GetColumns() == std::nullopt);

        invalid.SetStartType(SectionStartType::NextColumn)
            .SetPageOrientation(PageOrientation::Landscape)
            .SetPageSize({Inches(1.0), Inches(2.0), PageOrientation::Landscape})
            .SetMargins({})
            .SetColumns({});

        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);
        REQUIRE(editor->Body().InsertParagraph("Only visible body paragraph") != nullptr);
        CHECK(editor->Sections().empty());

        auto finalSection = editor->EnsureFinalSection();
        REQUIRE(finalSection != nullptr);
        CHECK(finalSection->IsFinalBodySection());
        finalSection->SetMargins({Inches(1.0),
                                  Inches(1.0),
                                  Inches(1.0),
                                  Inches(1.0),
                                  Inches(0.5),
                                  Inches(0.5),
                                  Inches(0.0)});

        auto reopened = WordDocumentEditor::Open(editor->SaveToMemory());
        REQUIRE(reopened != nullptr);
        auto sections = reopened->Sections();
        REQUIRE(sections.size() == 1);
        CHECK(sections[0]->IsFinalBodySection());
        CheckMargins(sections[0], 1440, 1440, 1440, 1440, 720, 720, 0);
    }

    TEST_CASE("Section API round trips every supported section start type [unit] [word] [word-section-api]")
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);

        const std::vector<SectionStartType> expected{
            SectionStartType::NextPage,
            SectionStartType::NextColumn,
            SectionStartType::Continuous,
            SectionStartType::EvenPage,
            SectionStartType::OddPage};

        for (const auto startType : expected)
        {
            REQUIRE(editor->Body().InsertParagraph("Section body") != nullptr);
            auto section = editor->Body().InsertSectionBreak(startType);
            REQUIRE(section != nullptr);
            CHECK(section->GetStartType() == startType);
        }

        auto reopened = WordDocumentEditor::Open(editor->SaveToMemory());
        REQUIRE(reopened != nullptr);
        auto sections = reopened->Sections();
        REQUIRE(sections.size() == expected.size());
        for (ExyokiOffice::Size i = 0; i < expected.size(); ++i)
        {
            CHECK(sections[i]->GetStartType() == expected[i]);
            CHECK(CountDirectSectionTypeChildren(sections[i]) == 1);
        }
    }

    TEST_CASE("Section API preserves significant margin edge values [unit] [word] [word-section-api]")
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);

        auto section = editor->EnsureFinalSection();
        REQUIRE(section != nullptr);
        section->SetMargins({MeasuringUnits(-12.0, MeasurementUnit::Point),
                             MeasuringUnits(-1.0, MeasurementUnit::Inch),
                             MeasuringUnits(-18.0, MeasurementUnit::Point),
                             MeasuringUnits(-2.0, MeasurementUnit::Inch),
                             MeasuringUnits(-3.0, MeasurementUnit::Point),
                             MeasuringUnits(-4.0, MeasurementUnit::Point),
                             MeasuringUnits(-5.0, MeasurementUnit::Point)});

        auto reopened = WordDocumentEditor::Open(editor->SaveToMemory());
        REQUIRE(reopened != nullptr);
        auto sections = reopened->Sections();
        REQUIRE(sections.size() == 1);

        // WordprocessingML allows signed top/bottom margins. Unsigned margins are clamped to zero.
        CheckMargins(sections[0], -240, 0, -360, 0, 0, 0, 0);
    }

    TEST_CASE("Section API clamps column counts to values WordprocessingML can store [unit] [word] [word-section-api]")
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);

        auto section = editor->EnsureFinalSection();
        REQUIRE(section != nullptr);
        section->SetColumns({0, Points(12.0), false});
        auto columns = section->GetColumns();
        REQUIRE(columns.has_value());
        CHECK(columns->Count == 1);
        CHECK(Twips(columns->Spacing) == 240);

        section->SetColumns({40000, Points(6.0), true});
        auto reopened = WordDocumentEditor::Open(editor->SaveToMemory());
        REQUIRE(reopened != nullptr);
        auto sections = reopened->Sections();
        REQUIRE(sections.size() == 1);
        auto reopenedColumns = sections[0]->GetColumns();
        REQUIRE(reopenedColumns.has_value());
        CHECK(reopenedColumns->Count == 32767);
        CHECK(Twips(reopenedColumns->Spacing) == 120);
        CHECK(reopenedColumns->Separator);
    }

    TEST_CASE("Section API updates existing section type without duplicating w:type [unit] [word] [word-section-api]")
    {
        auto created = WordDocumentEditor::CreateNew();
        REQUIRE(created != nullptr);

        auto section = created->Body().InsertSectionBreak(SectionStartType::OddPage);
        REQUIRE(section != nullptr);

        auto opened = WordDocumentEditor::Open(created->SaveToMemory());
        REQUIRE(opened != nullptr);
        auto sections = opened->Sections();
        REQUIRE(sections.size() == 1);
        CHECK(sections[0]->GetStartType() == SectionStartType::OddPage);
        CHECK(CountDirectSectionTypeChildren(sections[0]) == 1);

        sections[0]->SetStartType(SectionStartType::NextColumn);
        CHECK(sections[0]->GetStartType() == SectionStartType::NextColumn);
        CHECK(CountDirectSectionTypeChildren(sections[0]) == 1);

        auto reopened = WordDocumentEditor::Open(opened->SaveToMemory());
        REQUIRE(reopened != nullptr);
        auto reopenedSections = reopened->Sections();
        REQUIRE(reopenedSections.size() == 1);
        CHECK(reopenedSections[0]->GetStartType() == SectionStartType::NextColumn);
        CHECK(CountDirectSectionTypeChildren(reopenedSections[0]) == 1);
    }

} // TEST_SUITE("WordSectionApiTests")
