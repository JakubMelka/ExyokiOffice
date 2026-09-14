// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

// Regressions for the PowerPoint text tools and the PowerPoint model writer
// found by the Office COM run (MCP_ERRORS.md, P-2 and P-6).

#include "doctest.h"

#include "TestSupport.hpp"

#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/Drawing.hpp"
#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/Presentation.hpp"
#include "ExyokiOffice/PowerPoint/PowerPointDocument.hpp"
#include "ExyokiOffice/StandardTypes.hpp"
#include "ExyokiOffice/Tools/DocumentConverter.hpp"
#include "ExyokiOffice/Tools/DocumentTextTools.hpp"
#include "ExyokiOffice/Tools/ValidationRunner.hpp"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace ExyokiOffice;
using namespace ExyokiOffice::Tools;
using namespace ExyokiOffice::PowerPoint;

namespace Drawing = ExyokiOffice::DocumentFormat::OpenXml::Drawing;
namespace Presentation = ExyokiOffice::DocumentFormat::OpenXml::Presentation;

class ToolsPowerPointTextToolsTestHelpers
{
public:
    static PresentationTextFrame Frame(const std::string& text)
    {
        PresentationTextFrame frame;
        PresentationTextParagraph paragraph;
        PresentationTextRun run;
        run.Text = text;
        paragraph.Runs.push_back(run);
        frame.Paragraphs.push_back(paragraph);
        return frame;
    }

    static PresentationShapeTransform Transform(Int64 y)
    {
        PresentationShapeTransform transform;
        transform.Position = PresentationPoint(Int64{457200}, y);
        transform.Size = PresentationSize(Int64{4000000}, Int64{800000});
        return transform;
    }

    /// Appends a `p:grpSp` holding one text shape, which no high-level API authors.
    static void AddGroupWithText(const PresentationSlide::Ptr& slide, const std::string& text)
    {
        auto root = slide->GetPart()->GetTypedRootElement();
        const auto trees = root->Descendants<Presentation::ShapeTree>();
        REQUIRE_FALSE(trees.empty());
        auto group = trees.front()->AppendChild<Presentation::GroupShape>();
        REQUIRE(group);
        auto nonVisual = group->AppendChild<Presentation::NonVisualGroupShapeProperties>();
        auto drawing = nonVisual->AppendChild<Presentation::NonVisualDrawingProperties>();
        REQUIRE(nonVisual->AppendChild<Presentation::NonVisualGroupShapeDrawingProperties>());
        REQUIRE(nonVisual->AppendChild<Presentation::ApplicationNonVisualDrawingProperties>());
        drawing->SetId(UInt32Value(90));
        drawing->SetName(StringValue("Group"));
        auto groupProperties = group->AppendChild<Presentation::GroupShapeProperties>();
        auto transform = groupProperties->AppendChild<Drawing::TransformGroup>();
        auto offset = transform->AppendChild<Drawing::Offset>();
        auto extents = transform->AppendChild<Drawing::Extents>();
        auto childOffset = transform->AppendChild<Drawing::ChildOffset>();
        auto childExtents = transform->AppendChild<Drawing::ChildExtents>();
        REQUIRE(offset);
        REQUIRE(extents);
        REQUIRE(childOffset);
        REQUIRE(childExtents);
        offset->SetX(Int64Value(0));
        offset->SetY(Int64Value(0));
        childOffset->SetX(Int64Value(0));
        childOffset->SetY(Int64Value(0));
        extents->SetCx(Int64Value(1000000));
        extents->SetCy(Int64Value(1000000));
        childExtents->SetCx(Int64Value(1000000));
        childExtents->SetCy(Int64Value(1000000));

        auto shape = group->AppendChild<Presentation::Shape>();
        auto shapeNonVisual = shape->AppendChild<Presentation::NonVisualShapeProperties>();
        auto shapeDrawing = shapeNonVisual->AppendChild<Presentation::NonVisualDrawingProperties>();
        REQUIRE(shapeNonVisual->AppendChild<Presentation::NonVisualShapeDrawingProperties>());
        REQUIRE(shapeNonVisual->AppendChild<Presentation::ApplicationNonVisualDrawingProperties>());
        shapeDrawing->SetId(UInt32Value(91));
        shapeDrawing->SetName(StringValue("Grouped"));
        REQUIRE(shape->AppendChild<Presentation::ShapeProperties>());
        auto body = shape->AppendChild<Presentation::TextBody>();
        REQUIRE(body->AppendChild<Drawing::BodyProperties>());
        REQUIRE(body->AppendChild<Drawing::ListStyle>());
        auto paragraph = body->AppendChild<Drawing::Paragraph>();
        auto run = paragraph->AppendChild<Drawing::Run>();
        auto textElement = run->AppendChild<Drawing::Text>();
        REQUIRE(textElement);
        textElement->SetText(text);
    }

    /// Appends a legacy (pre-2016) `p:cm` comment part with one comment.
    static void AddLegacyComment(const PresentationSlide::Ptr& slide, const std::string& text)
    {
        auto part = slide->GetPart()->AddSlideCommentsPart();
        REQUIRE(part);
        auto list = part->GetTypedRootElement();
        REQUIRE(list);
        auto comment = list->AppendChild<Presentation::Comment>();
        REQUIRE(comment);
        comment->SetAuthorId(UInt32Value(0));
        comment->SetIndex(UInt32Value(1));
        auto textElement = comment->AppendChild<Presentation::Text>();
        REQUIRE(textElement);
        textElement->SetText(text);
    }

    static std::string LegacyCommentText(const PresentationSlide::Ptr& slide)
    {
        auto part = slide->GetPart()->GetSlideCommentsPart();
        const auto texts = part ? part->GetTypedRootElement()->Descendants<Presentation::Text>()
                                : std::vector<Presentation::Text::Ptr>{};
        return texts.empty() ? std::string() : std::string(texts.front()->GetText());
    }

    static std::string GroupedText(const PresentationSlide::Ptr& slide)
    {
        for (const auto& shape : slide->ShapeTree()->Shapes())
        {
            if (shape->IsGroup())
            {
                const auto children = shape->Children();
                REQUIRE_FALSE(children.empty());
                const auto frame = children.front()->GetTextFrame();
                REQUIRE(frame);
                return frame->Paragraphs.front().Runs.front().Text;
            }
        }
        return {};
    }

    static std::string CellText(const PresentationSlide::Ptr& slide)
    {
        for (const auto& shape : slide->ShapeTree()->Shapes())
        {
            if (const auto table = shape->GetTable())
            {
                return table->Rows.front().Cells.front().Text;
            }
        }
        return {};
    }

    static bool HasMatch(const DocumentSearchResult& result, const std::string& label)
    {
        for (const auto& match : result.Matches)
        {
            if (match.Label == label)
            {
                return true;
            }
        }
        return false;
    }
};

using Helpers = ToolsPowerPointTextToolsTestHelpers;

TEST_CASE("P-6: PowerPoint search and replace cover table cells, groups and comments [unit] [tools] [text-tools]")
{
    auto editor = PowerPointDocumentEditor::CreateNew();
    REQUIRE(editor);
    auto slide = editor->AddSlide();
    REQUIRE(slide);
    auto tree = slide->ShapeTree();
    REQUIRE(tree);

    auto box = tree->AddShape("Box");
    REQUIRE(box);
    REQUIRE(box->SetTransform(Helpers::Transform(500000)));
    REQUIRE(box->SetTextFrame(Helpers::Frame("box 2025")));

    PresentationTableData table;
    table.Transform = Helpers::Transform(1500000);
    table.ColumnWidths = {MeasuringUnits(3000000.0, MeasurementUnit::Emu)};
    PresentationTableRow row;
    row.Height = MeasuringUnits(400000.0, MeasurementUnit::Emu);
    row.Cells.push_back(PresentationTableCell{"cell 2025"});
    table.Rows.push_back(row);
    REQUIRE(tree->AddTable(table));

    Helpers::AddGroupWithText(slide, "group 2025");
    REQUIRE(slide->SetNotesText("notes 2025"));

    PresentationCommentAuthor author;
    author.Id = "{11111111-2222-3333-4444-555555555555}";
    author.Name = "Reviewer";
    author.Initials = "R";
    REQUIRE(editor->AddCommentAuthor(author));
    PresentationComment comment;
    comment.Id = "{66666666-7777-8888-9999-000000000000}";
    comment.AuthorId = author.Id;
    comment.Text = "comment 2025";
    comment.Replies.push_back(PresentationCommentReply{"{AAAAAAAA-BBBB-CCCC-DDDD-EEEEEEEEEEEE}", author.Id, "reply 2025"});
    REQUIRE(slide->AddComment(comment));
    Helpers::AddLegacyComment(slide, "legacy 2025");

    const auto found = SearchDocumentText(*editor, "2025");
    REQUIRE(found.Ok);
    CHECK(found.Matches.size() == 7);
    CHECK(Helpers::HasMatch(found, "slide 1 shape 1 paragraph 1"));
    CHECK(Helpers::HasMatch(found, "slide 1 shape 2 cell 1,1 paragraph 1"));
    CHECK(Helpers::HasMatch(found, "slide 1 shape 3/1 paragraph 1"));
    CHECK(Helpers::HasMatch(found, "slide 1 notes"));
    CHECK(Helpers::HasMatch(found, "slide 1 comment 1"));
    CHECK(Helpers::HasMatch(found, "slide 1 comment 1 reply 1"));
    CHECK(Helpers::HasMatch(found, "slide 1 comment 2"));

    const auto dryRun = ReplaceDocumentText(*editor, "2025", "2026", true);
    REQUIRE(dryRun.Ok);
    CHECK(dryRun.ReplacementCount == 7);
    CHECK(Helpers::CellText(slide) == "cell 2025");

    const auto replaced = ReplaceDocumentText(*editor, "2025", "2026", false);
    REQUIRE(replaced.Ok);
    CHECK(replaced.ReplacementCount == 7);
    CHECK(Helpers::CellText(slide) == "cell 2026");
    CHECK(Helpers::GroupedText(slide) == "group 2026");
    CHECK(slide->NotesText() == "notes 2026");
    const auto comments = slide->Comments();
    REQUIRE(comments.size() == 1);
    CHECK(comments.front().Text == "comment 2026");
    REQUIRE(comments.front().Replies.size() == 1);
    CHECK(comments.front().Replies.front().Text == "reply 2026");
    CHECK(Helpers::LegacyCommentText(slide) == "legacy 2026");
    CHECK(SearchDocumentText(*editor, "2025").Matches.empty());
    CHECK(SearchDocumentText(*editor, "2026").Matches.size() == 7);

    // Replacing inside a table cell keeps the cell's runs and formatting node.
    const auto reopened = PowerPointDocumentEditor::Open(editor->SaveToMemory());
    REQUIRE(reopened);
    CHECK(Helpers::CellText(reopened->GetSlide(0)) == "cell 2026");
}

TEST_CASE("P-2: a presentation written from the document model has a master, layout and theme [unit] [tools] [conversion]")
{
    const auto markdownPath = ExyokiOfficeTests::MakeTemporaryPath("exyoki_hand_outline", ".md");
    {
        std::ofstream markdown(markdownPath, std::ios::binary);
        REQUIRE(markdown);
        markdown << "# Hand title\n\n- bullet one\n";
    }
    const auto pptxPath = ExyokiOfficeTests::MakeTemporaryPath("exyoki_hand_outline", ".pptx");
    const auto converted = ConvertDocument(markdownPath, pptxPath);
    REQUIRE(converted.Ok);

    auto editor = PowerPointDocumentEditor::Open(pptxPath);
    REQUIRE(editor);
    REQUIRE(editor->SlideCount() == 1);
    REQUIRE_FALSE(editor->SlideMasters().empty());
    REQUIRE_FALSE(editor->SlideLayouts().empty());
    CHECK(editor->SlideMasters().front()->ThemeXml().has_value());
    auto slide = editor->GetSlide(0);
    REQUIRE(slide);
    REQUIRE(slide->Layout());
    CHECK(slide->Layout()->Name() == "Title and Content");
    CHECK_FALSE(slide->Placeholders(false).empty());

    const auto report = Tools::Run(pptxPath);
    CHECK(report.Loaded);
    CHECK(report.ErrorCount == 0);

    std::filesystem::remove(markdownPath);
    std::filesystem::remove(pptxPath);
}
