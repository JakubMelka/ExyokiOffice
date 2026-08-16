// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

// Tests for the CLI-facing automation additions: family-aware search/replace,
// CSV conversion, redaction, template filling, workbook recalculation,
// semantic Word comparison, and batch input expansion.

#include "doctest.h"

#include "TestSupport.hpp"

#include "ExyokiOffice/Excel/ExcelDocument.hpp"
#include "ExyokiOffice/OpenXmlPackage.hpp"
#include "ExyokiOffice/PowerPoint/PowerPointDocument.hpp"
#include "ExyokiOffice/Tools/DocumentConverter.hpp"
#include "ExyokiOffice/Tools/DocumentModelIO.hpp"
#include "ExyokiOffice/Tools/DocumentRedactor.hpp"
#include "ExyokiOffice/Tools/DocumentTextTools.hpp"
#include "ExyokiOffice/Tools/PackageInspector.hpp"
#include "ExyokiOffice/Tools/Report.hpp"
#include "ExyokiOffice/Tools/SpreadsheetTools.hpp"
#include "ExyokiOffice/Tools/TextExtractor.hpp"
#include "ExyokiOffice/Tools/WordAutomationTools.hpp"
#include "ExyokiOffice/TextPattern.hpp"
#include "ExyokiOffice/Word/WordDocument.hpp"

#include <algorithm>
#include <fstream>
#include <string>

using namespace ExyokiOffice::Tools;
using ExyokiOffice::Excel::ExcelDocumentEditor;
using ExyokiOffice::PowerPoint::PowerPointDocumentEditor;
using ExyokiOffice::Word::WordDocumentEditor;
using ExyokiOfficeTests::MakeTemporaryPath;

namespace
{

std::filesystem::path BuildWorkbookFixture()
{
    auto editor = ExcelDocumentEditor::CreateNew();
    REQUIRE(editor);
    auto sheet = editor->FirstWorksheet();
    REQUIRE(sheet);
    sheet->SetCellText(1, 1, "Quarterly report");
    sheet->SetCellText(2, 1, "quarterly summary");
    sheet->SetCellNumber(3, 1, 2026.0);

    const auto path = MakeTemporaryPath("exyoki_auto_book", ".xlsx");
    REQUIRE(editor->SaveToFile(path));
    return path;
}

std::filesystem::path BuildPresentationFixture()
{
    using namespace ExyokiOffice::PowerPoint;

    auto editor = PowerPointDocumentEditor::CreateNew();
    REQUIRE(editor);
    auto slide = editor->AddSlide();
    REQUIRE(slide);
    auto tree = slide->ShapeTree();
    REQUIRE(tree);
    auto shape = tree->AddShape("Title 1");
    REQUIRE(shape);

    PresentationTextFrame frame;
    PresentationTextParagraph paragraph;
    PresentationTextRun bold;
    bold.Text = "Quarterly ";
    bold.Bold = true;
    PresentationTextRun plain;
    plain.Text = "report";
    paragraph.Runs.push_back(bold);
    paragraph.Runs.push_back(plain);
    frame.Paragraphs.push_back(paragraph);
    REQUIRE(shape->SetTextFrame(frame));

    slide->SetNotesText("Mention the quarterly targets.");

    const auto path = MakeTemporaryPath("exyoki_auto_deck", ".pptx");
    REQUIRE(editor->SaveToFile(path));
    return path;
}

[[nodiscard]] bool AnyMatchLabel(const DocumentSearchResult& result, std::string_view needle)
{
    return std::any_of(result.Matches.begin(), result.Matches.end(),
                       [needle](const DocumentSearchMatch& match)
                       { return match.Label.find(needle) != std::string::npos; });
}

std::string ReadTextFile(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    REQUIRE(file);
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

} // namespace

// --- Family-aware search and replace ---------------------------------------

TEST_CASE("SearchDocumentText finds Excel cell matches with sheet labels [unit] [tools]")
{
    const auto path = BuildWorkbookFixture();
    const auto result = SearchDocumentText(path, "quarterly", 10, false, true);
    CHECK(result.Ok);
    CHECK(result.Family == DocumentFamily::Excel);
    CHECK(result.Matches.size() == 2);
    CHECK(AnyMatchLabel(result, "!A1"));
    CHECK(AnyMatchLabel(result, "!A2"));
    std::filesystem::remove(path);
}

TEST_CASE("ReplaceDocumentText rewrites Excel text cells and skips numbers [unit] [tools]")
{
    const auto path = BuildWorkbookFixture();

    // "2026" only occurs in a number cell; the match is counted as skipped,
    // both as a structured field and as a human-readable warning.
    const auto skipped = ReplaceDocumentText(path, "2026", "2027", true);
    CHECK(skipped.Ok);
    CHECK(skipped.ReplacementCount == 0);
    CHECK(skipped.SkippedMatches == 1);
    CHECK(std::any_of(skipped.Diagnostics.begin(), skipped.Diagnostics.end(),
                      [](const ToolDiagnostic& diagnostic)
                      { return diagnostic.Severity == ToolSeverity::Warning; }));

    const auto output = MakeTemporaryPath("exyoki_auto_book_out", ".xlsx");
    const auto replaced = ReplaceDocumentText(path, "Quarterly", "Annual", false, output);
    CHECK(replaced.Ok);
    CHECK(replaced.ReplacementCount == 1);
    CHECK(replaced.Saved);

    const auto verify = SearchDocumentText(output, "Annual report");
    CHECK(verify.Matches.size() == 1);

    std::filesystem::remove(path);
    std::filesystem::remove(output);
}

TEST_CASE("SearchDocumentText finds PowerPoint shape and notes matches [unit] [tools]")
{
    const auto path = BuildPresentationFixture();
    const auto result = SearchDocumentText(path, "quarterly", 40, false, true);
    CHECK(result.Ok);
    CHECK(result.Family == DocumentFamily::PowerPoint);
    CHECK(result.Matches.size() == 2);
    CHECK(AnyMatchLabel(result, "slide 1 shape 1 paragraph 1"));
    CHECK(AnyMatchLabel(result, "slide 1 notes"));
    std::filesystem::remove(path);
}

TEST_CASE("ReplaceDocumentText replaces across PowerPoint runs and notes [unit] [tools]")
{
    const auto path = BuildPresentationFixture();

    // "Quarterly report" spans two runs (bold "Quarterly " + plain "report").
    const auto result = ReplaceDocumentText(path, "Quarterly report", "Annual overview", false);
    CHECK(result.Ok);
    CHECK(result.ReplacementCount == 1);
    CHECK(result.Saved);

    const auto notes = ReplaceDocumentText(path, "quarterly", "yearly", false, {}, false, true);
    CHECK(notes.Ok);
    CHECK(notes.ReplacementCount == 1);

    const auto extracted = Extract(path);
    REQUIRE(extracted.Ok);
    bool sawShape = false;
    bool sawNotes = false;
    for (const auto& block : extracted.Blocks)
    {
        sawShape = sawShape || block.Text.find("Annual overview") != std::string::npos;
        sawNotes = sawNotes || block.Text.find("yearly targets") != std::string::npos;
        CHECK(block.Text.find("Quarterly report") == std::string::npos);
    }
    CHECK(sawShape);
    CHECK(sawNotes);
    std::filesystem::remove(path);
}

TEST_CASE("ReplaceDocumentText expands regex capture groups in Excel cells [unit] [tools]")
{
    const auto path = BuildWorkbookFixture();
    const auto result = ReplaceDocumentText(path, "(Quarterly) (report)", "$2 ($1)", false, {}, true);
    CHECK(result.Ok);
    CHECK(result.ReplacementCount == 1);

    const auto verify = SearchDocumentText(path, "report (Quarterly)");
    CHECK(verify.Matches.size() == 1);
    std::filesystem::remove(path);
}

TEST_CASE("SearchDocumentText reports an invalid regex instead of matching [unit] [tools]")
{
    const auto path = BuildWorkbookFixture();
    const auto result = SearchDocumentText(path, "([", 40, true);
    CHECK_FALSE(result.Ok);
    CHECK(std::any_of(result.Diagnostics.begin(), result.Diagnostics.end(),
                      [](const ToolDiagnostic& diagnostic)
                      { return diagnostic.Severity == ToolSeverity::Error; }));
    std::filesystem::remove(path);
}

TEST_CASE("A cell past the regex subject limit is not searched [unit] [tools] [security]")
{
    // A backtracking regex engine recurses per input character, and the
    // Microsoft implementation exhausts the thread stack on a long enough
    // subject. That is not an exception a caller can catch - the process is
    // gone - so `RegexPattern::MaximumSubjectLength` promises such text is never
    // handed to the engine. The promise was kept by the Word paragraph API and
    // by nothing else, which left every Excel cell, PowerPoint shape, table cell
    // and notes page open to it: a hostile document only had to pick the other
    // frontend.
    auto editor = ExcelDocumentEditor::CreateNew();
    REQUIRE(editor);
    auto sheet = editor->FirstWorksheet();
    REQUIRE(sheet);

    const auto limit = ExyokiOffice::RegexPattern::MaximumSubjectLength;
    sheet->SetCellText(1, 1, "needle " + std::string(limit, 'x'));
    sheet->SetCellText(2, 1, "needle in a cell of ordinary length");

    const auto matched = SearchDocumentText(*editor, "needle", 10, true);
    CHECK(matched.Ok);
    // The short cell matches; the over-long one is skipped rather than searched.
    REQUIRE(matched.Matches.size() == 1U);
    CHECK(AnyMatchLabel(matched, "!A2"));

    // Exactly at the limit is still searched - the bound is inclusive.
    sheet->SetCellText(1, 1, "needle" + std::string(limit - 6, 'x'));
    const auto atLimit = SearchDocumentText(*editor, "needle", 10, true);
    CHECK(atLimit.Matches.size() == 2U);

    // A plain substring search does not go through the engine and is unaffected.
    sheet->SetCellText(1, 1, "needle " + std::string(limit, 'x'));
    const auto plain = SearchDocumentText(*editor, "needle");
    CHECK(plain.Matches.size() == 2U);
}

TEST_CASE("SearchDocumentText rejects an empty needle [unit] [tools]")
{
    const auto result = SearchDocumentText("nonexistent.docx", "");
    CHECK_FALSE(result.Ok);
    CHECK_FALSE(result.Diagnostics.empty());
}

TEST_CASE("SearchDocumentText still dispatches Word documents [unit] [tools]")
{
    auto editor = WordDocumentEditor::CreateNew();
    REQUIRE(editor);
    editor->AddParagraph("Alpha appears in the body.");
    const auto path = MakeTemporaryPath("exyoki_auto_word", ".docx");
    REQUIRE(editor->SaveToFile(path));

    const auto result = SearchDocumentText(path, "Alpha");
    CHECK(result.Ok);
    CHECK(result.Family == DocumentFamily::Word);
    REQUIRE(result.Matches.size() == 1);
    CHECK(result.Matches.front().Label.find("body") != std::string::npos);
    std::filesystem::remove(path);
}

// --- CSV -------------------------------------------------------------------

TEST_CASE("SerializeModelCsv quotes and orders the worksheet grid [unit] [tools] [conversion]")
{
    DocumentModel model;
    model.Family = DocumentFamily::Excel;
    model.Excel.emplace();
    ExcelSheetModel sheet;
    sheet.Name = "Sheet1";
    sheet.Cells.push_back(ExcelCellModel{"A1", "string", "Name", "", "", ""});
    sheet.Cells.push_back(ExcelCellModel{"B1", "string", "Value", "", "", ""});
    sheet.Cells.push_back(ExcelCellModel{"A2", "string", "Widget, \"Large\"", "", "", ""});
    sheet.Cells.push_back(ExcelCellModel{"B2", "number", "42", "", "", ""});
    sheet.Cells.push_back(ExcelCellModel{"B3", "formula", "", "A1+A2", "number", "52"});
    model.Excel->Sheets.push_back(std::move(sheet));

    std::vector<ToolDiagnostic> diagnostics;
    const auto csv = SerializeModelCsv(model, CsvOptions{}, diagnostics);
    CHECK(csv == "Name,Value\r\n\"Widget, \"\"Large\"\"\",42\r\n,52\r\n");
    CHECK(diagnostics.empty());
}

TEST_CASE("ParseModelCsv infers cell types conservatively [unit] [tools] [conversion]")
{
    std::vector<ToolDiagnostic> diagnostics;
    const auto model =
        ParseModelCsv("Name,Count,Flag,Code\r\n\"Bolt, M4\",42,TRUE,007\n=SUM(A1),3.5e2,,end",
                      CsvOptions{}, diagnostics);
    REQUIRE(model.Family == DocumentFamily::Excel);
    REQUIRE(model.Excel);
    REQUIRE(model.Excel->Sheets.size() == 1);
    const auto& cells = model.Excel->Sheets.front().Cells;

    const auto find = [&](std::string_view address) -> const ExcelCellModel*
    {
        for (const auto& cell : cells)
        {
            if (cell.Address == address)
            {
                return &cell;
            }
        }
        return nullptr;
    };

    REQUIRE(find("A2"));
    CHECK(find("A2")->Type == "string");
    CHECK(find("A2")->Value == "Bolt, M4");
    CHECK(find("B2")->Type == "number");
    CHECK(find("C2")->Type == "bool");
    CHECK(find("C2")->Value == "true");
    // Leading zeros survive as text, and '=' never creates a formula.
    CHECK(find("D2")->Type == "string");
    CHECK(find("D2")->Value == "007");
    CHECK(find("A3")->Type == "string");
    CHECK(find("A3")->Value == "=SUM(A1)");
    CHECK(find("B3")->Type == "number");
    CHECK(find("C3") == nullptr);
    CHECK(find("D3")->Value == "end");
}

TEST_CASE("CSV honors a custom separator in both directions [unit] [tools] [conversion]")
{
    DocumentModel model;
    model.Family = DocumentFamily::Excel;
    model.Excel.emplace();
    ExcelSheetModel sheet;
    sheet.Name = "Sheet1";
    sheet.Cells.push_back(ExcelCellModel{"A1", "string", "a,b", "", "", ""});
    sheet.Cells.push_back(ExcelCellModel{"B1", "string", "c;d", "", "", ""});
    model.Excel->Sheets.push_back(std::move(sheet));

    CsvOptions options;
    options.Separator = ";";
    std::vector<ToolDiagnostic> diagnostics;
    const auto csv = SerializeModelCsv(model, options, diagnostics);
    // With ';' as separator the comma needs no quoting, the semicolon does.
    CHECK(csv == "a,b;\"c;d\"\r\n");

    const auto parsed = ParseModelCsv(csv, options, diagnostics);
    REQUIRE(parsed.Excel);
    REQUIRE(parsed.Excel->Sheets.front().Cells.size() == 2);
    CHECK(parsed.Excel->Sheets.front().Cells[0].Value == "a,b");
    CHECK(parsed.Excel->Sheets.front().Cells[1].Value == "c;d");
}

TEST_CASE("SerializeModelCsv selects worksheets by name [unit] [tools] [conversion]")
{
    DocumentModel model;
    model.Family = DocumentFamily::Excel;
    model.Excel.emplace();
    for (const auto* name : {"First", "Second"})
    {
        ExcelSheetModel sheet;
        sheet.Name = name;
        sheet.Cells.push_back(ExcelCellModel{"A1", "string", name, "", "", ""});
        model.Excel->Sheets.push_back(std::move(sheet));
    }

    // Default: first sheet, with a multi-sheet warning.
    std::vector<ToolDiagnostic> defaultDiagnostics;
    CHECK(SerializeModelCsv(model, CsvOptions{}, defaultDiagnostics) == "First\r\n");
    CHECK(std::any_of(defaultDiagnostics.begin(), defaultDiagnostics.end(),
                      [](const ToolDiagnostic& diagnostic)
                      { return diagnostic.Severity == ToolSeverity::Warning; }));

    // Named selection is case-insensitive.
    CsvOptions named;
    named.SheetName = "second";
    std::vector<ToolDiagnostic> namedDiagnostics;
    CHECK(SerializeModelCsv(model, named, namedDiagnostics) == "Second\r\n");
    CHECK(namedDiagnostics.empty());

    CsvOptions missing;
    missing.SheetName = "Third";
    std::vector<ToolDiagnostic> missingDiagnostics;
    CHECK(SerializeModelCsv(model, missing, missingDiagnostics).empty());
    CHECK(std::any_of(missingDiagnostics.begin(), missingDiagnostics.end(),
                      [](const ToolDiagnostic& diagnostic)
                      { return diagnostic.Severity == ToolSeverity::Error; }));
}

TEST_CASE("ParseModelCsv keeps line breaks inside quoted fields [unit] [tools] [conversion]")
{
    std::vector<ToolDiagnostic> diagnostics;
    const auto model = ParseModelCsv("\"line1\r\nline2\",tail\r\nnext", CsvOptions{}, diagnostics);
    REQUIRE(model.Excel);
    const auto& cells = model.Excel->Sheets.front().Cells;
    REQUIRE(cells.size() == 3);
    CHECK(cells[0].Address == "A1");
    CHECK(cells[0].Value == "line1\r\nline2");
    CHECK(cells[1].Address == "B1");
    CHECK(cells[1].Value == "tail");
    CHECK(cells[2].Address == "A2");
    CHECK(cells[2].Value == "next");
}

TEST_CASE("ConvertDocument round-trips xlsx through CSV [unit] [tools] [conversion]")
{
    const auto workbook = BuildWorkbookFixture();
    const auto csvPath = MakeTemporaryPath("exyoki_auto_csv", ".csv");
    const auto rebuilt = MakeTemporaryPath("exyoki_auto_rebuilt", ".xlsx");

    const auto exported = ConvertDocument(workbook, csvPath);
    CHECK(exported.Ok);
    CHECK(exported.To == ConvertFormat::Csv);
    const auto csv = ReadTextFile(csvPath);
    CHECK(csv.find("Quarterly report\r\n") != std::string::npos);
    CHECK(csv.find("2026") != std::string::npos);

    const auto imported = ConvertDocument(csvPath, rebuilt);
    CHECK(imported.Ok);
    const auto verify = SearchDocumentText(rebuilt, "Quarterly report");
    CHECK(verify.Ok);
    CHECK(verify.Matches.size() == 1);

    std::filesystem::remove(workbook);
    std::filesystem::remove(csvPath);
    std::filesystem::remove(rebuilt);
}

TEST_CASE("ConvertDocument rejects CSV output for a Word document [unit] [tools] [conversion]")
{
    auto editor = WordDocumentEditor::CreateNew();
    REQUIRE(editor);
    editor->AddParagraph("Text");
    const auto path = MakeTemporaryPath("exyoki_auto_word_csv", ".docx");
    REQUIRE(editor->SaveToFile(path));

    const auto csvPath = MakeTemporaryPath("exyoki_auto_word_out", ".csv");
    const auto result = ConvertDocument(path, csvPath);
    CHECK_FALSE(result.Ok);
    CHECK_FALSE(result.UsageOk);
    std::filesystem::remove(path);
}

TEST_CASE("ConvertDocument rejects CSV input to a text format [unit] [tools] [conversion]")
{
    // Like Markdown and plain text, CSV input must target an Office format.
    const auto result = ConvertDocument("data.csv", "data.md");
    CHECK_FALSE(result.Ok);
    CHECK_FALSE(result.UsageOk);
}

// --- Redact ----------------------------------------------------------------

TEST_CASE("RedactDocument removes Word comments and hidden text [unit] [tools]")
{
    auto editor = WordDocumentEditor::CreateNew();
    REQUIRE(editor);
    auto paragraph = editor->AddParagraph("Reviewed content.");
    REQUIRE(paragraph);
    ExyokiOffice::Word::CommentAuthor author;
    author.Name = "Reviewer";
    author.Initials = "RV";
    REQUIRE(paragraph->AddCommentOnParagraph("Please rephrase.", author));
    const auto path = MakeTemporaryPath("exyoki_auto_redact", ".docx");
    REQUIRE(editor->SaveToFile(path));

    // Inject a hidden run the way editing applications store it, plus a
    // same-named element in a foreign namespace that redaction must not touch.
    {
        ExyokiOffice::OpenXmlPackage package;
        REQUIRE(package.LoadFromFile(path));
        auto part = package.GetPartByUri(GetInfo(package).MainPartUri);
        REQUIRE(part);
        auto xml = part->GetXmlString();
        const std::string hidden =
            "<w:p><w:r><w:rPr><w:vanish/></w:rPr><w:t>SECRET</w:t></w:r>"
            "<w:r><w:t>visible tail</w:t></w:r>"
            "<foo:commentRangeStart xmlns:foo=\"urn:example-foreign\"/></w:p>";
        const auto bodyEnd = xml.find("</w:body>");
        REQUIRE(bodyEnd != std::string::npos);
        xml.insert(bodyEnd, hidden);
        part->SetXmlString(xml);
        REQUIRE(package.SaveToFile(path));
    }

    const auto output = MakeTemporaryPath("exyoki_auto_redacted", ".docx");
    const auto result = RedactDocument(path, output);
    CHECK(result.Ok);
    CHECK(result.Family == DocumentFamily::Word);
    CHECK(result.CommentsRemoved == 1);
    CHECK(result.HiddenRunsRemoved == 1);
    CHECK(result.Saved);

    auto redacted = WordDocumentEditor::Open(output);
    REQUIRE(redacted);
    CHECK(redacted->Comments().empty());

    const auto text = Extract(output);
    REQUIRE(text.Ok);
    for (const auto& block : text.Blocks)
    {
        CHECK(block.Text.find("SECRET") == std::string::npos);
        CHECK(block.Text.find("Please rephrase") == std::string::npos);
    }
    bool sawTail = false;
    for (const auto& block : text.Blocks)
    {
        sawTail = sawTail || block.Text.find("visible tail") != std::string::npos;
    }
    CHECK(sawTail);

    // The foreign-namespace element with a comment-marker local name survives:
    // the scrub matches by namespace URI, not by element name alone.
    {
        ExyokiOffice::OpenXmlPackage package;
        REQUIRE(package.LoadFromFile(output));
        auto part = package.GetPartByUri(GetInfo(package).MainPartUri);
        REQUIRE(part);
        CHECK(part->GetXmlString().find("urn:example-foreign") != std::string::npos);
    }

    std::filesystem::remove(path);
    std::filesystem::remove(output);
}

TEST_CASE("RedactDocument removes Excel comments [unit] [tools]")
{
    auto editor = ExcelDocumentEditor::CreateNew();
    REQUIRE(editor);
    auto sheet = editor->FirstWorksheet();
    REQUIRE(sheet);
    sheet->SetCellText(1, 1, "Value");
    ExyokiOffice::Excel::ExcelComment comment;
    comment.Address = *ExyokiOffice::Excel::CellAddress::ParseA1("A1");
    comment.Author = "Reviewer";
    comment.Text = "Check this figure.";
    REQUIRE(sheet->SetComment(comment));
    const auto path = MakeTemporaryPath("exyoki_auto_redact_xlsx", ".xlsx");
    REQUIRE(editor->SaveToFile(path));

    const auto result = RedactDocument(path);
    CHECK(result.Ok);
    CHECK(result.CommentsRemoved == 1);

    auto redacted = ExcelDocumentEditor::Open(path);
    REQUIRE(redacted);
    CHECK(redacted->FirstWorksheet()->Comments().empty());
    std::filesystem::remove(path);
}

TEST_CASE("RedactDocument keeps categories that were opted out [unit] [tools]")
{
    auto editor = WordDocumentEditor::CreateNew();
    REQUIRE(editor);
    auto paragraph = editor->AddParagraph("Reviewed content.");
    REQUIRE(paragraph);
    REQUIRE(paragraph->AddCommentOnParagraph("Keep me."));
    const auto path = MakeTemporaryPath("exyoki_auto_redact_keep", ".docx");
    REQUIRE(editor->SaveToFile(path));

    RedactOptions options;
    options.RemoveComments = false;
    const auto result = RedactDocument(path, {}, options);
    CHECK(result.Ok);
    CHECK(result.CommentsRemoved == 0);

    auto kept = WordDocumentEditor::Open(path);
    REQUIRE(kept);
    CHECK(kept->Comments().size() == 1);
    std::filesystem::remove(path);
}

TEST_CASE("RedactDocument removes PowerPoint comments and their authors [unit] [tools]")
{
    using namespace ExyokiOffice::PowerPoint;

    auto editor = PowerPointDocumentEditor::CreateNew();
    REQUIRE(editor);
    auto slide = editor->AddSlide();
    REQUIRE(slide);

    PresentationCommentAuthor author;
    author.Id = "author-1";
    author.Name = "Reviewer";
    author.Initials = "RV";
    REQUIRE(editor->AddCommentAuthor(author));

    PresentationComment comment;
    comment.Id = "comment-1";
    comment.AuthorId = "author-1";
    comment.Text = "Please reword this slide.";
    REQUIRE(slide->AddComment(comment));

    const auto path = MakeTemporaryPath("exyoki_auto_redact_pptx", ".pptx");
    REQUIRE(editor->SaveToFile(path));

    const auto result = RedactDocument(path);
    CHECK(result.Ok);
    CHECK(result.Family == DocumentFamily::PowerPoint);
    CHECK(result.CommentsRemoved == 1);

    auto redacted = PowerPointDocumentEditor::Open(path);
    REQUIRE(redacted);
    CHECK(redacted->Slides().front()->Comments().empty());
    CHECK(redacted->CommentAuthors().empty());
    std::filesystem::remove(path);
}

// --- Fill, compare, recalc -------------------------------------------------

TEST_CASE("FillWordTemplate merges JSON scalars into MERGEFIELDs [unit] [tools]")
{
    auto editor = WordDocumentEditor::CreateNew();
    REQUIRE(editor);
    auto paragraph = editor->AddParagraph("Dear ");
    REQUIRE(paragraph);
    REQUIRE(paragraph->AddField("MERGEFIELD FirstName", "First"));
    const auto templatePath = MakeTemporaryPath("exyoki_auto_template", ".docx");
    REQUIRE(editor->SaveToFile(templatePath));

    const auto dataPath = MakeTemporaryPath("exyoki_auto_data", ".json");
    {
        std::ofstream file(dataPath, std::ios::binary);
        file << R"({"FirstName": "Ada", "Skipped": {"nested": true}})";
    }

    const auto output = MakeTemporaryPath("exyoki_auto_filled", ".docx");
    const auto result = FillWordTemplate(templatePath, dataPath, output);
    CHECK(result.Ok);
    CHECK(result.FieldsMerged == 1);
    CHECK(result.Saved);
    // The non-scalar member is skipped with a warning, not silently.
    CHECK(std::any_of(result.Diagnostics.begin(), result.Diagnostics.end(),
                      [](const ToolDiagnostic& diagnostic)
                      { return diagnostic.Severity == ToolSeverity::Warning; }));

    auto filled = WordDocumentEditor::Open(output);
    REQUIRE(filled);
    const auto text = Extract(output);
    bool sawAda = false;
    for (const auto& block : text.Blocks)
    {
        sawAda = sawAda || block.Text.find("Ada") != std::string::npos;
    }
    CHECK(sawAda);

    std::filesystem::remove(templatePath);
    std::filesystem::remove(dataPath);
    std::filesystem::remove(output);
}

TEST_CASE("FillWordTemplate expands repeating regions from arrays of objects [unit] [tools]")
{
    auto editor = WordDocumentEditor::CreateNew();
    REQUIRE(editor);
    auto start = editor->AddParagraph();
    REQUIRE(start);
    REQUIRE(start->AddField("MERGEFIELD TableStart:Orders", ""));
    auto row = editor->AddParagraph("Item: ");
    REQUIRE(row);
    REQUIRE(row->AddField("MERGEFIELD Item", "item"));
    auto end = editor->AddParagraph();
    REQUIRE(end);
    REQUIRE(end->AddField("MERGEFIELD TableEnd:Orders", ""));
    const auto templatePath = MakeTemporaryPath("exyoki_auto_region", ".docx");
    REQUIRE(editor->SaveToFile(templatePath));

    const auto dataPath = MakeTemporaryPath("exyoki_auto_region_data", ".json");
    {
        std::ofstream file(dataPath, std::ios::binary);
        file << R"({"Orders": [{"Item": "Widget"}, {"Item": "Gadget"}]})";
    }

    const auto output = MakeTemporaryPath("exyoki_auto_region_out", ".docx");
    const auto result = FillWordTemplate(templatePath, dataPath, output);
    CHECK(result.Ok);
    CHECK(result.RegionsMerged == 1);
    CHECK(result.RegionRowsInserted == 2);

    const auto text = Extract(output);
    REQUIRE(text.Ok);
    bool sawWidget = false;
    bool sawGadget = false;
    for (const auto& block : text.Blocks)
    {
        sawWidget = sawWidget || block.Text.find("Widget") != std::string::npos;
        sawGadget = sawGadget || block.Text.find("Gadget") != std::string::npos;
    }
    CHECK(sawWidget);
    CHECK(sawGadget);

    std::filesystem::remove(templatePath);
    std::filesystem::remove(dataPath);
    std::filesystem::remove(output);
}

TEST_CASE("FillWordTemplate rejects malformed data files [unit] [tools]")
{
    auto editor = WordDocumentEditor::CreateNew();
    REQUIRE(editor);
    editor->AddParagraph("Body");
    const auto templatePath = MakeTemporaryPath("exyoki_auto_fill_bad", ".docx");
    REQUIRE(editor->SaveToFile(templatePath));

    const auto invalidJson = MakeTemporaryPath("exyoki_auto_fill_bad_data", ".json");
    {
        std::ofstream file(invalidJson, std::ios::binary);
        file << "{not json";
    }
    CHECK_FALSE(FillWordTemplate(templatePath, invalidJson).Ok);

    const auto arrayRoot = MakeTemporaryPath("exyoki_auto_fill_array", ".json");
    {
        std::ofstream file(arrayRoot, std::ios::binary);
        file << "[1, 2, 3]";
    }
    CHECK_FALSE(FillWordTemplate(templatePath, arrayRoot).Ok);

    CHECK_FALSE(FillWordTemplate(templatePath, "does-not-exist.json").Ok);

    std::filesystem::remove(templatePath);
    std::filesystem::remove(invalidJson);
    std::filesystem::remove(arrayRoot);
}

TEST_CASE("CompareWordDocuments writes tracked revisions and redact resolves them [unit] [tools]")
{
    auto original = WordDocumentEditor::CreateNew();
    REQUIRE(original);
    original->AddParagraph("Shared paragraph.");
    original->AddParagraph("Removed paragraph.");
    const auto originalPath = MakeTemporaryPath("exyoki_auto_cmp_a", ".docx");
    REQUIRE(original->SaveToFile(originalPath));

    auto revised = WordDocumentEditor::CreateNew();
    REQUIRE(revised);
    revised->AddParagraph("Shared paragraph.");
    revised->AddParagraph("Added paragraph.");
    const auto revisedPath = MakeTemporaryPath("exyoki_auto_cmp_b", ".docx");
    REQUIRE(revised->SaveToFile(revisedPath));

    const auto outputPath = MakeTemporaryPath("exyoki_auto_cmp_out", ".docx");
    const auto compared = CompareWordDocuments(originalPath, revisedPath, outputPath, "Reviewer");
    CHECK(compared.Ok);
    CHECK_FALSE(compared.Identical);
    CHECK(compared.RevisionsCreated > 0);

    // The comparison output carries live revisions; redaction resolves them.
    const auto redacted = RedactDocument(outputPath);
    CHECK(redacted.Ok);
    CHECK(redacted.RevisionsResolved > 0);

    const auto same = CompareWordDocuments(originalPath, originalPath, outputPath);
    CHECK(same.Ok);
    CHECK(same.Identical);

    std::filesystem::remove(originalPath);
    std::filesystem::remove(revisedPath);
    std::filesystem::remove(outputPath);
}

TEST_CASE("RecalculateWorkbook rewrites cached formula results [unit] [tools]")
{
    auto editor = ExcelDocumentEditor::CreateNew();
    REQUIRE(editor);
    auto sheet = editor->FirstWorksheet();
    REQUIRE(sheet);
    sheet->SetCellNumber(1, 1, 10.0);
    sheet->SetCellNumber(2, 1, 32.0);
    REQUIRE(sheet->SetCellFormula(*ExyokiOffice::Excel::CellAddress::ParseA1("A3"), "=SUM(A1:A2)"));
    const auto path = MakeTemporaryPath("exyoki_auto_recalc", ".xlsx");
    REQUIRE(editor->SaveToFile(path));

    const auto result = RecalculateWorkbook(path);
    CHECK(result.Ok);
    CHECK(result.RecalculatedCellCount >= 1);
    CHECK(result.CircularReferenceCycles.empty());
    CHECK(result.Saved);

    auto recalculated = ExcelDocumentEditor::Open(path);
    REQUIRE(recalculated);
    const auto formula = recalculated->FirstWorksheet()->GetCellFormula(
        *ExyokiOffice::Excel::CellAddress::ParseA1("A3"));
    REQUIRE(formula);
    CHECK(formula->CachedText == "42");
    std::filesystem::remove(path);
}

TEST_CASE("RecalculateWorkbook honors dry runs and sheet selection [unit] [tools]")
{
    auto editor = ExcelDocumentEditor::CreateNew();
    REQUIRE(editor);
    auto sheet = editor->FirstWorksheet();
    REQUIRE(sheet);
    sheet->SetCellNumber(1, 1, 5.0);
    REQUIRE(sheet->SetCellFormula(*ExyokiOffice::Excel::CellAddress::ParseA1("A2"), "=A1*2"));
    const auto sheetName = sheet->Name();
    const auto path = MakeTemporaryPath("exyoki_auto_recalc_dry", ".xlsx");
    REQUIRE(editor->SaveToFile(path));

    const auto dryRun = RecalculateWorkbook(path, {}, {}, true);
    CHECK(dryRun.Ok);
    CHECK(dryRun.RecalculatedCellCount >= 1);
    CHECK_FALSE(dryRun.Saved);

    const auto scoped = RecalculateWorkbook(path, sheetName);
    CHECK(scoped.Ok);
    CHECK(scoped.RecalculatedCellCount >= 1);
    CHECK(scoped.Saved);

    const auto unknownSheet = RecalculateWorkbook(path, "NoSuchSheet");
    CHECK_FALSE(unknownSheet.Ok);
    CHECK_FALSE(unknownSheet.Diagnostics.empty());

    std::filesystem::remove(path);
}

TEST_CASE("RecalculateWorkbook reports circular references [unit] [tools]")
{
    auto editor = ExcelDocumentEditor::CreateNew();
    REQUIRE(editor);
    auto sheet = editor->FirstWorksheet();
    REQUIRE(sheet);
    REQUIRE(sheet->SetCellFormula(*ExyokiOffice::Excel::CellAddress::ParseA1("A1"), "=B1"));
    REQUIRE(sheet->SetCellFormula(*ExyokiOffice::Excel::CellAddress::ParseA1("B1"), "=A1"));
    const auto path = MakeTemporaryPath("exyoki_auto_recalc_cycle", ".xlsx");
    REQUIRE(editor->SaveToFile(path));

    const auto result = RecalculateWorkbook(path, {}, {}, true);
    CHECK(result.Ok);
    REQUIRE(result.CircularReferenceCycles.size() == 1);
    CHECK(result.CircularReferenceCycles.front().find(" -> ") != std::string::npos);
    std::filesystem::remove(path);
}

// --- Batch expansion and report rendering ----------------------------------

TEST_CASE("ExpandInputPaths expands filename wildcards deterministically [unit] [tools]")
{
    const auto directory = MakeTemporaryPath("exyoki_auto_expand", "");
    std::filesystem::create_directories(directory);
    for (const auto* name : {"b.docx", "a.docx", "c.txt"})
    {
        std::ofstream file(directory / name, std::ios::binary);
        file << "x";
    }

    std::vector<ToolDiagnostic> diagnostics;
    const auto expanded = ExpandInputPaths({(directory / "*.docx").string()}, diagnostics);
    REQUIRE(expanded.size() == 2);
    CHECK(expanded[0].filename() == "a.docx");
    CHECK(expanded[1].filename() == "b.docx");
    CHECK(diagnostics.empty());

    const auto passthrough = ExpandInputPaths({(directory / "missing.docx").string()}, diagnostics);
    CHECK(passthrough.size() == 1);
    CHECK(diagnostics.empty());

    const auto unmatched = ExpandInputPaths({(directory / "*.pptx").string()}, diagnostics);
    CHECK(unmatched.empty());
    CHECK(diagnostics.size() == 1);

    std::filesystem::remove_all(directory);
}

TEST_CASE("RenderJson ends with a newline for clean piping [unit] [tools]")
{
    ReportDocument document;
    document.Command = "info";
    const auto rendered = RenderJson(document);
    REQUIRE_FALSE(rendered.empty());
    CHECK(rendered.back() == '\n');
}
