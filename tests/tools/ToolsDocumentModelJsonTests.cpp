// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "doctest.h"

#include "ExyokiOffice/Tools/DocumentModelIO.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

using namespace ExyokiOffice::Tools;

namespace
{

DocumentModel MakeWordModel()
{
    DocumentModel model;
    model.Family = DocumentFamily::Word;
    model.Properties.Title = "Round trip";
    auto& word = model.Word.emplace();

    WordParagraph heading;
    heading.StyleId = "Heading1";
    heading.HeadingLevel = 1;
    WordInline headingText;
    headingText.Kind = WordInline::Type::Text;
    headingText.Text = "Chapter";
    heading.Inlines.push_back(headingText);
    WordBlock headingBlock;
    headingBlock.Kind = WordBlock::Type::Paragraph;
    headingBlock.Paragraph = heading;
    word.Body.push_back(headingBlock);

    WordParagraph body;
    body.Alignment = "center";
    body.List = WordListRef{3, 1};
    WordInline bold;
    bold.Kind = WordInline::Type::Text;
    bold.Text = "bold";
    bold.Props.Bold = true;
    bold.Props.Color = "FF0000";
    bold.Props.FontSizePt = 14.0;
    body.Inlines.push_back(bold);
    WordInline link;
    link.Kind = WordInline::Type::Hyperlink;
    link.Target = "https://example.com";
    WordInline linkText;
    linkText.Kind = WordInline::Type::Text;
    linkText.Text = "site";
    link.Children.push_back(linkText);
    body.Inlines.push_back(link);
    WordInline note;
    note.Kind = WordInline::Type::FootnoteRef;
    note.NoteId = 1;
    body.Inlines.push_back(note);
    WordBlock bodyBlock;
    bodyBlock.Kind = WordBlock::Type::Paragraph;
    bodyBlock.Paragraph = body;
    word.Body.push_back(bodyBlock);

    WordTable table;
    WordTableRow row;
    WordTableCell cell;
    cell.ColSpan = 2;
    WordParagraph cellParagraph;
    WordInline cellText;
    cellText.Kind = WordInline::Type::Text;
    cellText.Text = "cell";
    cellParagraph.Inlines.push_back(cellText);
    WordBlock cellBlock;
    cellBlock.Kind = WordBlock::Type::Paragraph;
    cellBlock.Paragraph = cellParagraph;
    cell.Blocks.push_back(cellBlock);
    row.Cells.push_back(cell);
    WordTableCell covered;
    covered.Covered = true;
    row.Cells.push_back(covered);
    table.Rows.push_back(row);
    WordBlock tableBlock;
    tableBlock.Kind = WordBlock::Type::Table;
    tableBlock.Table = table;
    word.Body.push_back(tableBlock);

    WordListDefinition list;
    list.NumberingId = 3;
    WordListLevel level;
    level.Format = "decimal";
    level.LevelText = "%1.";
    list.Levels.push_back(level);
    word.Lists.push_back(list);

    WordNote footnote;
    footnote.Id = 1;
    WordParagraph noteParagraph;
    WordInline noteText;
    noteText.Kind = WordInline::Type::Text;
    noteText.Text = "note body";
    noteParagraph.Inlines.push_back(noteText);
    WordBlock noteBlock;
    noteBlock.Kind = WordBlock::Type::Paragraph;
    noteBlock.Paragraph = noteParagraph;
    footnote.Blocks.push_back(noteBlock);
    word.Footnotes.push_back(footnote);

    return model;
}

DocumentModel MakeExcelModel()
{
    DocumentModel model;
    model.Family = DocumentFamily::Excel;
    auto& workbook = model.Excel.emplace();
    ExcelSheetModel sheet;
    sheet.Name = "Data";
    sheet.Cells.push_back(ExcelCellModel{"A1", "string", "Name", "", "", ""});
    sheet.Cells.push_back(ExcelCellModel{"B1", "number", "42.5", "", "", ""});
    sheet.Cells.push_back(ExcelCellModel{"C1", "bool", "true", "", "", ""});
    sheet.Cells.push_back(ExcelCellModel{"D1", "formula", "", "SUM(A1:B1)", "number", "42"});
    sheet.Merges.push_back("A2:B3");
    sheet.Tables.push_back(ExcelTableModel{"Table1", "A1:C4"});
    sheet.Hyperlinks.push_back(ExcelHyperlinkModel{"A1", "https://example.com", "tip"});
    workbook.Sheets.push_back(sheet);
    return model;
}

DocumentModel MakePowerPointModel()
{
    DocumentModel model;
    model.Family = DocumentFamily::PowerPoint;
    auto& deck = model.PowerPoint.emplace();

    PptSlide slide;
    slide.Hidden = true;
    slide.NotesText = "speaker notes";

    PptShape title;
    title.Kind = PptShape::Type::Placeholder;
    title.PlaceholderType = "title";
    PptTextFrame frame;
    PptParagraph paragraph;
    PptRun run;
    run.Text = "Q3";
    run.Bold = true;
    paragraph.Runs.push_back(run);
    frame.Paragraphs.push_back(paragraph);
    title.Text = frame;
    slide.Shapes.push_back(title);

    PptShape box;
    box.Kind = PptShape::Type::TextBox;
    box.Transform.Present = true;
    box.Transform.X = 914400;
    box.Transform.Cx = 914400;
    box.Transform.Cy = 457200;
    PptTextFrame boxFrame;
    PptParagraph bullet;
    bullet.Level = 1;
    PptRun bulletRun;
    bulletRun.Text = "Point";
    bullet.Runs.push_back(bulletRun);
    boxFrame.Paragraphs.push_back(bullet);
    box.Text = boxFrame;
    slide.Shapes.push_back(box);

    slide.Comments.push_back(PptCommentModel{"JM", "check numbers"});
    deck.Slides.push_back(slide);

    MediaReference media;
    media.Id = "media1";
    media.ContentType = "image/png";
    media.Data = {0x89, 0x50, 0x4E, 0x47};
    model.Media.push_back(media);
    return model;
}

} // namespace

TEST_CASE("Word model survives a JSON round trip [unit] [tools] [conversion]")
{
    const auto model = MakeWordModel();
    const auto json = SerializeModelJson(model, false);
    CHECK(json.find("\"exyokioffice-document\"") != std::string::npos);
    CHECK(json.find("\"family\": \"word\"") != std::string::npos);

    std::vector<ToolDiagnostic> diagnostics;
    const auto parsed = ParseModelJson(json, diagnostics);
    CHECK(diagnostics.empty());
    REQUIRE(parsed.Family == DocumentFamily::Word);
    REQUIRE(parsed.Word);
    CHECK(parsed.Properties.Title == "Round trip");

    REQUIRE(parsed.Word->Body.size() == 3);
    const auto& heading = parsed.Word->Body[0];
    REQUIRE(heading.Paragraph);
    CHECK(heading.Paragraph->StyleId == "Heading1");
    CHECK(heading.Paragraph->HeadingLevel == 1);

    const auto& body = parsed.Word->Body[1];
    REQUIRE(body.Paragraph);
    CHECK(body.Paragraph->Alignment == "center");
    REQUIRE(body.Paragraph->List);
    CHECK(body.Paragraph->List->NumberingId == 3);
    CHECK(body.Paragraph->List->Level == 1);
    REQUIRE(body.Paragraph->Inlines.size() == 3);
    CHECK(body.Paragraph->Inlines[0].Props.Bold);
    CHECK(body.Paragraph->Inlines[0].Props.Color == "FF0000");
    CHECK(body.Paragraph->Inlines[0].Props.FontSizePt == doctest::Approx(14.0));
    CHECK(body.Paragraph->Inlines[1].Kind == WordInline::Type::Hyperlink);
    CHECK(body.Paragraph->Inlines[1].Target == "https://example.com");
    CHECK(body.Paragraph->Inlines[2].Kind == WordInline::Type::FootnoteRef);

    const auto& table = parsed.Word->Body[2];
    REQUIRE(table.Table);
    REQUIRE(table.Table->Rows.size() == 1);
    REQUIRE(table.Table->Rows[0].Cells.size() == 2);
    CHECK(table.Table->Rows[0].Cells[0].ColSpan == 2);
    CHECK(table.Table->Rows[0].Cells[1].Covered);

    REQUIRE(parsed.Word->Lists.size() == 1);
    CHECK(parsed.Word->Lists[0].NumberingId == 3);
    REQUIRE(parsed.Word->Footnotes.size() == 1);
    CHECK(parsed.Word->Footnotes[0].Id == 1);
}

TEST_CASE("Excel model survives a JSON round trip [unit] [tools] [conversion]")
{
    const auto model = MakeExcelModel();
    const auto json = SerializeModelJson(model, false);

    std::vector<ToolDiagnostic> diagnostics;
    const auto parsed = ParseModelJson(json, diagnostics);
    REQUIRE(parsed.Family == DocumentFamily::Excel);
    REQUIRE(parsed.Excel);
    REQUIRE(parsed.Excel->Sheets.size() == 1);

    const auto& sheet = parsed.Excel->Sheets[0];
    CHECK(sheet.Name == "Data");
    REQUIRE(sheet.Cells.size() == 4);
    CHECK(sheet.Cells[0].Type == "string");
    CHECK(sheet.Cells[1].Value == "42.5");
    CHECK(sheet.Cells[2].Type == "bool");
    CHECK(sheet.Cells[3].Type == "formula");
    CHECK(sheet.Cells[3].Formula == "SUM(A1:B1)");
    CHECK(sheet.Cells[3].CachedType == "number");
    CHECK(sheet.Cells[3].CachedValue == "42");
    REQUIRE(sheet.Merges.size() == 1);
    CHECK(sheet.Merges[0] == "A2:B3");
    REQUIRE(sheet.Tables.size() == 1);
    CHECK(sheet.Tables[0].Name == "Table1");
    REQUIRE(sheet.Hyperlinks.size() == 1);
    CHECK(sheet.Hyperlinks[0].Target == "https://example.com");
}

TEST_CASE("PowerPoint model survives a JSON round trip with embedded media [unit] [tools] [conversion]")
{
    const auto model = MakePowerPointModel();
    const auto json = SerializeModelJson(model, true);
    CHECK(json.find("\"data\"") != std::string::npos); // base64 media payload

    std::vector<ToolDiagnostic> diagnostics;
    const auto parsed = ParseModelJson(json, diagnostics);
    REQUIRE(parsed.Family == DocumentFamily::PowerPoint);
    REQUIRE(parsed.PowerPoint);
    REQUIRE(parsed.PowerPoint->Slides.size() == 1);

    const auto& slide = parsed.PowerPoint->Slides[0];
    CHECK(slide.Hidden);
    CHECK(slide.NotesText == "speaker notes");
    REQUIRE(slide.Shapes.size() == 2);
    CHECK(slide.Shapes[0].Kind == PptShape::Type::Placeholder);
    CHECK(slide.Shapes[0].PlaceholderType == "title");
    REQUIRE(slide.Shapes[0].Text);
    CHECK(slide.Shapes[0].Text->Paragraphs[0].Runs[0].Bold);
    CHECK(slide.Shapes[1].Kind == PptShape::Type::TextBox);
    CHECK(slide.Shapes[1].Transform.Present);
    CHECK(slide.Shapes[1].Transform.X == 914400);
    REQUIRE(slide.Comments.size() == 1);
    CHECK(slide.Comments[0].Text == "check numbers");

    REQUIRE(parsed.Media.size() == 1);
    CHECK(parsed.Media[0].ContentType == "image/png");
    CHECK(parsed.Media[0].Data == std::vector<ExyokiOffice::Byte>{0x89, 0x50, 0x4E, 0x47});
}

TEST_CASE("JSON parser tolerates unknown keys and reports bad envelopes [unit] [tools] [conversion]")
{
    std::vector<ToolDiagnostic> diagnostics;
    const auto parsed = ParseModelJson(
        R"({"format":"exyokioffice-document","version":1,"family":"word","futureKey":true,
            "document":{"body":[{"type":"paragraph","content":[{"type":"text","text":"hi","fancy":1}]}]}})",
        diagnostics);
    REQUIRE(parsed.Family == DocumentFamily::Word);
    REQUIRE(parsed.Word);
    REQUIRE(parsed.Word->Body.size() == 1);
    CHECK(parsed.Word->Body[0].Paragraph->Inlines[0].Text == "hi");

    std::vector<ToolDiagnostic> badDiagnostics;
    const auto bad = ParseModelJson("{\"family\":\"sheet-music\"}", badDiagnostics);
    CHECK(bad.Family == DocumentFamily::Unknown);
    CHECK(!badDiagnostics.empty());

    std::vector<ToolDiagnostic> invalidDiagnostics;
    const auto invalid = ParseModelJson("this is not json", invalidDiagnostics);
    CHECK(invalid.Family == DocumentFamily::Unknown);
    CHECK(!invalidDiagnostics.empty());
}

TEST_CASE("Semantic XML mirrors the JSON envelope for every family [unit] [tools] [conversion]")
{
    for (const auto& model : {MakeWordModel(), MakeExcelModel(), MakePowerPointModel()})
    {
        const auto xml = SerializeModelXml(model, true);
        CHECK(xml.find("<eoDocument") != std::string::npos);

        std::vector<ToolDiagnostic> diagnostics;
        const auto parsed = ParseModelXml(xml, diagnostics);
        REQUIRE(parsed.Family == model.Family);

        // The XML projection must agree with the canonical JSON serialization.
        CHECK(SerializeModelJson(parsed, true) == SerializeModelJson(model, true));
    }
}
