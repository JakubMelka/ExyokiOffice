// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "doctest.h"

#include "TestSupport.hpp"

#include "ExyokiOffice/PowerPoint/PowerPointDocument.hpp"
#include "ExyokiOffice/Tools/DocumentConverter.hpp"
#include "ExyokiOffice/Tools/DocumentModelIO.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <chrono>
#include <fstream>

using namespace ExyokiOffice::Tools;
using namespace ExyokiOffice::PowerPoint;
using ExyokiOffice::MeasurementUnit;
using ExyokiOffice::MeasuringUnits;

namespace
{

using ExyokiOfficeTests::MakeTemporaryPath;

std::filesystem::path SaveSamplePresentation()
{
    auto editor = PowerPointDocumentEditor::CreateNew();
    REQUIRE(editor);

    auto slide = editor->CreateSlideBuilder()
                     .SetTitle("Quarterly report",
                               {{MeasuringUnits(0.5, MeasurementUnit::Inch),
                                 MeasuringUnits(0.3, MeasurementUnit::Inch)},
                                {MeasuringUnits(9.0, MeasurementUnit::Inch),
                                 MeasuringUnits(1.0, MeasurementUnit::Inch)}})
                     .Build();
    REQUIRE(slide);

    auto tree = slide->ShapeTree();
    REQUIRE(tree);
    auto shape = tree->AddShape("Content 1");
    REQUIRE(shape);
    PresentationShapeTransform transform;
    transform.Position = PresentationPoint(ExyokiOffice::Int64{457200}, ExyokiOffice::Int64{1600200});
    transform.Size = PresentationSize(ExyokiOffice::Int64{8229600}, ExyokiOffice::Int64{2000000});
    shape->SetTransform(transform);

    PresentationTextFrame frame;
    PresentationTextParagraph first;
    PresentationTextRun bold;
    bold.Text = "Bold point";
    bold.Bold = true;
    first.Runs.push_back(bold);
    frame.Paragraphs.push_back(first);
    PresentationTextParagraph nested;
    nested.Level = 1;
    PresentationTextRun nestedRun;
    nestedRun.Text = "Nested detail";
    nested.Runs.push_back(nestedRun);
    frame.Paragraphs.push_back(nested);
    REQUIRE(shape->SetTextFrame(frame));

    slide->SetNotesText("Remember the numbers");

    const auto path = MakeTemporaryPath("exyoki_convert_pptx", ".pptx");
    REQUIRE(editor->SaveToFile(path));
    return path;
}

std::string ReadFileText(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    REQUIRE(file);
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

bool DeckContainsText(const PowerPointDeckModel& deck, std::string_view text)
{
    for (const auto& slide : deck.Slides)
    {
        for (const auto& shape : slide.Shapes)
        {
            if (!shape.Text)
            {
                continue;
            }
            for (const auto& paragraph : shape.Text->Paragraphs)
            {
                for (const auto& run : paragraph.Runs)
                {
                    if (run.Text.find(text) != std::string::npos)
                    {
                        return true;
                    }
                }
            }
        }
    }
    return false;
}

} // namespace

TEST_CASE("PowerPoint presentation converts to JSON and back [unit] [tools] [conversion]")
{
    const auto pptxPath = SaveSamplePresentation();
    const auto jsonPath = MakeTemporaryPath("exyoki_convert_pptx", ".json");
    const auto backPath = MakeTemporaryPath("exyoki_convert_pptx_back", ".pptx");

    const auto toJson = ConvertDocument(pptxPath, jsonPath);
    CHECK(toJson.Ok);
    CHECK(toJson.Family == DocumentFamily::PowerPoint);
    CHECK(toJson.SlideCount == 1);

    const auto json = ReadFileText(jsonPath);
    CHECK(json.find("Quarterly report") != std::string::npos);
    CHECK(json.find("Bold point") != std::string::npos);
    CHECK(json.find("Remember the numbers") != std::string::npos);

    const auto toPptx = ConvertDocument(jsonPath, backPath);
    CHECK(toPptx.Ok);

    std::vector<ToolDiagnostic> diagnostics;
    const auto model = ReadPowerPointModel(backPath, {}, diagnostics);
    REQUIRE(model.PowerPoint);
    REQUIRE(model.PowerPoint->Slides.size() == 1);
    const auto& slide = model.PowerPoint->Slides[0];
    CHECK(slide.NotesText == "Remember the numbers");
    CHECK(DeckContainsText(*model.PowerPoint, "Quarterly report"));
    CHECK(DeckContainsText(*model.PowerPoint, "Bold point"));
    CHECK(DeckContainsText(*model.PowerPoint, "Nested detail"));

    std::filesystem::remove(pptxPath);
    std::filesystem::remove(jsonPath);
    std::filesystem::remove(backPath);
}

TEST_CASE("PowerPoint presentation converts to Markdown and back [unit] [tools] [conversion]")
{
    const auto pptxPath = SaveSamplePresentation();
    const auto markdownPath = MakeTemporaryPath("exyoki_convert_pptx", ".md");
    const auto backPath = MakeTemporaryPath("exyoki_convert_pptx_md_back", ".pptx");

    const auto toMarkdown = ConvertDocument(pptxPath, markdownPath);
    CHECK(toMarkdown.Ok);

    const auto markdown = ReadFileText(markdownPath);
    CHECK(markdown.find("# Quarterly report") != std::string::npos);
    CHECK(markdown.find("**Bold point**") != std::string::npos);
    CHECK(markdown.find("> Notes: Remember the numbers") != std::string::npos);

    const auto toPptx = ConvertDocument(markdownPath, backPath);
    CHECK(toPptx.Ok);

    std::vector<ToolDiagnostic> diagnostics;
    const auto model = ReadPowerPointModel(backPath, {}, diagnostics);
    REQUIRE(model.PowerPoint);
    REQUIRE(model.PowerPoint->Slides.size() == 1);
    CHECK(model.PowerPoint->Slides[0].NotesText == "Remember the numbers");
    CHECK(DeckContainsText(*model.PowerPoint, "Quarterly report"));
    CHECK(DeckContainsText(*model.PowerPoint, "Bold point"));

    std::filesystem::remove(pptxPath);
    std::filesystem::remove(markdownPath);
    std::filesystem::remove(backPath);
}

TEST_CASE("Multi-slide Markdown builds a deck with tables [unit] [tools] [conversion]")
{
    const auto markdownPath = MakeTemporaryPath("exyoki_convert_pptx_multi", ".md");
    {
        std::ofstream file(markdownPath, std::ios::binary);
        file << "# First slide\n\n- point one\n- point two\n\n---\n\n"
                "# Second slide\n\n| A | B |\n| --- | --- |\n| 1 | 2 |\n\n> Notes: second notes\n";
    }
    const auto pptxPath = MakeTemporaryPath("exyoki_convert_pptx_multi", ".pptx");

    const auto toPptx = ConvertDocument(markdownPath, pptxPath);
    CHECK(toPptx.Ok);

    std::vector<ToolDiagnostic> diagnostics;
    const auto model = ReadPowerPointModel(pptxPath, {}, diagnostics);
    REQUIRE(model.PowerPoint);
    REQUIRE(model.PowerPoint->Slides.size() == 2);
    CHECK(DeckContainsText(*model.PowerPoint, "First slide"));
    CHECK(DeckContainsText(*model.PowerPoint, "point one"));
    CHECK(model.PowerPoint->Slides[1].NotesText == "second notes");

    bool tableFound = false;
    for (const auto& shape : model.PowerPoint->Slides[1].Shapes)
    {
        if (shape.Kind == PptShape::Type::Table && shape.Table && shape.Table->Rows.size() == 2)
        {
            tableFound = true;
        }
    }
    CHECK(tableFound);

    std::filesystem::remove(markdownPath);
    std::filesystem::remove(pptxPath);
}
