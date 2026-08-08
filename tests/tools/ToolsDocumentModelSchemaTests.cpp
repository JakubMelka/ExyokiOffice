// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "doctest.h"

#include "ExyokiOffice/Tools/DocumentModelIO.hpp"
#include "ExyokiOffice/Tools/DocumentModelSchema.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace ExyokiOffice::Tools;

/**
 * @brief Fixtures that reach every branch the serializer can take.
 *
 * The schema only detects drift over content that actually gets serialized, so
 * these models are deliberately maximal: every tagged variant, every optional
 * field, every nesting level. A serializer field that no fixture produces is a
 * field the conformance test cannot police.
 */
class SchemaTestModels
{
public:
    static DocumentModel MakeMaximalWordModel()
    {
        DocumentModel model;
        model.Family = DocumentFamily::Word;
        model.Properties.Title = "Schema coverage";
        model.Properties.Subject = "Testing";
        model.Properties.Creator = "JM";
        model.Properties.Keywords = "schema; test";
        model.Properties.Description = "Every branch of the Word serializer.";
        model.Properties.LastModifiedBy = "JM";
        model.Properties.Category = "Test";
        model.Properties.Created = "2026-08-01T10:00:00Z";
        model.Properties.Modified = "2026-08-01T11:00:00Z";
        model.Properties.Application = "ExyokiOffice";
        model.Properties.Company = "Exyoki";

        MediaReference media;
        media.Id = "media1";
        media.FileName = "doc_media/image1.png";
        media.ContentType = "image/png";
        media.Data = {0x89, 0x50, 0x4E, 0x47};
        model.Media.push_back(std::move(media));

        auto& word = model.Word.emplace();
        word.Body.push_back(MakeHeadingBlock());
        word.Body.push_back(MakeRichParagraphBlock());
        word.Body.push_back(MakeTableBlock());
        word.Body.push_back(MakeSectionBreakBlock());

        WordListLevel level;
        level.Format = "decimal";
        level.LevelText = "%1.";
        level.Start = 3;
        WordListDefinition list;
        list.NumberingId = 3;
        list.Levels.push_back(std::move(level));
        word.Lists.push_back(std::move(list));

        WordNote footnote;
        footnote.Id = 1;
        footnote.Blocks.push_back(MakeTextParagraphBlock("A footnote."));
        word.Footnotes.push_back(std::move(footnote));

        WordNote endnote;
        endnote.Id = 2;
        endnote.Blocks.push_back(MakeTextParagraphBlock("An endnote."));
        word.Endnotes.push_back(std::move(endnote));

        WordComment comment;
        comment.Id = 0;
        comment.Author = "JM";
        comment.Initials = "JM";
        comment.Date = "2026-08-01T10:30:00Z";
        comment.Blocks.push_back(MakeTextParagraphBlock("Check this."));
        word.Comments.push_back(std::move(comment));

        WordHeaderFooter header;
        header.Kind = "default";
        header.Blocks.push_back(MakeTextParagraphBlock("Header"));
        word.Headers.push_back(std::move(header));

        WordHeaderFooter footer;
        footer.Kind = "even";
        footer.Blocks.push_back(MakeTextParagraphBlock("Footer"));
        word.Footers.push_back(std::move(footer));
        return model;
    }

    static DocumentModel MakeMaximalExcelModel()
    {
        DocumentModel model;
        model.Family = DocumentFamily::Excel;
        model.Properties.Title = "Schema coverage";
        auto& workbook = model.Excel.emplace();

        ExcelSheetModel sheet;
        sheet.Name = "Data";
        sheet.Cells.push_back(MakeCell("A1", "string", "Name"));
        sheet.Cells.push_back(MakeCell("B1", "number", "42.5"));
        sheet.Cells.push_back(MakeCell("C1", "bool", "true"));
        sheet.Cells.push_back(MakeCell("D1", "error", "#DIV/0!"));
        sheet.Cells.push_back(MakeCell("E1", "datetime", "2026-01-01T00:00:00"));

        ExcelCellModel formula;
        formula.Address = "F1";
        formula.Type = "formula";
        formula.Formula = "SUM(B1:B1)";
        formula.CachedType = "number";
        formula.CachedValue = "42.5";
        sheet.Cells.push_back(std::move(formula));

        ExcelCellModel bareFormula;
        bareFormula.Address = "G1";
        bareFormula.Type = "formula";
        bareFormula.Formula = "TODAY()";
        sheet.Cells.push_back(std::move(bareFormula));

        sheet.Merges.emplace_back("A4:B4");
        sheet.Tables.push_back(ExcelTableModel{"Table1", "A1:F1"});
        sheet.Hyperlinks.push_back(ExcelHyperlinkModel{"A1", "https://example.com", "Example"});
        workbook.Sheets.push_back(std::move(sheet));

        ExcelSheetModel empty;
        empty.Name = "Empty";
        workbook.Sheets.push_back(std::move(empty));
        return model;
    }

    static DocumentModel MakeMaximalPowerPointModel()
    {
        DocumentModel model;
        model.Family = DocumentFamily::PowerPoint;
        model.Properties.Title = "Schema coverage";

        MediaReference media;
        media.Id = "media1";
        media.ContentType = "image/png";
        media.Data = {0x89, 0x50, 0x4E, 0x47};
        model.Media.push_back(std::move(media));

        auto& deck = model.PowerPoint.emplace();

        PptSlide slide;
        slide.LayoutName = "Title Slide";
        slide.Hidden = true;
        slide.NotesText = "Speaker notes";
        slide.Comments.push_back(PptCommentModel{"JM", "Check numbers"});
        slide.Shapes.push_back(MakePlaceholderShape());
        slide.Shapes.push_back(MakeTextBoxShape());
        slide.Shapes.push_back(MakePictureShape());
        slide.Shapes.push_back(MakeTableShape());
        slide.Shapes.push_back(MakeGroupShape());
        slide.Shapes.push_back(MakeOtherShape());
        deck.Slides.push_back(std::move(slide));

        PptSlide bare;
        deck.Slides.push_back(std::move(bare));
        return model;
    }

    /// The published schema artifact next to the manual.
    static std::filesystem::path SchemaArtifactPath()
    {
        return std::filesystem::path(EXYOKIOFFICE_SOURCE_DIR) / "docs" / "schemas" /
               GetDocumentModelJsonSchemaFileName();
    }

    static std::string ReadFile(const std::filesystem::path& path)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file)
        {
            return {};
        }
        std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        content.erase(std::remove(content.begin(), content.end(), '\r'), content.end());
        return content;
    }

private:
    static WordBlock MakeTextParagraphBlock(const std::string& text)
    {
        WordInline run;
        run.Kind = WordInline::Type::Text;
        run.Text = text;

        WordParagraph paragraph;
        paragraph.Inlines.push_back(std::move(run));

        WordBlock block;
        block.Kind = WordBlock::Type::Paragraph;
        block.Paragraph = std::move(paragraph);
        return block;
    }

    static WordBlock MakeHeadingBlock()
    {
        WordInline run;
        run.Kind = WordInline::Type::Text;
        run.Text = "Chapter";

        WordParagraph paragraph;
        paragraph.StyleId = "Heading1";
        paragraph.HeadingLevel = 1;
        paragraph.Alignment = "center";
        paragraph.Inlines.push_back(std::move(run));

        WordBlock block;
        block.Kind = WordBlock::Type::Paragraph;
        block.Paragraph = std::move(paragraph);
        return block;
    }

    /// One paragraph carrying every inline variant the serializer knows.
    static WordBlock MakeRichParagraphBlock()
    {
        WordParagraph paragraph;
        paragraph.StyleId = "ListParagraph";
        paragraph.List = WordListRef{3, 1};

        WordInline formatted;
        formatted.Kind = WordInline::Type::Text;
        formatted.Text = "formatted";
        formatted.Props.Bold = true;
        formatted.Props.Italic = true;
        formatted.Props.Underline = true;
        formatted.Props.Strike = true;
        formatted.Props.Caps = true;
        formatted.Props.SmallCaps = true;
        formatted.Props.Color = "FF0000";
        formatted.Props.Highlight = "yellow";
        formatted.Props.Font = "Calibri";
        formatted.Props.StyleId = "Emphasis";
        formatted.Props.FontSizePt = 14.0;
        paragraph.Inlines.push_back(std::move(formatted));

        WordInline externalLink;
        externalLink.Kind = WordInline::Type::Hyperlink;
        externalLink.Target = "https://example.com";
        externalLink.Tooltip = "Example";
        WordInline linkText;
        linkText.Kind = WordInline::Type::Text;
        linkText.Text = "site";
        externalLink.Children.push_back(std::move(linkText));
        paragraph.Inlines.push_back(std::move(externalLink));

        WordInline internalLink;
        internalLink.Kind = WordInline::Type::Hyperlink;
        internalLink.Anchor = "bookmark1";
        paragraph.Inlines.push_back(std::move(internalLink));

        WordInline image;
        image.Kind = WordInline::Type::Image;
        image.MediaId = "media1";
        image.AltText = "A picture";
        image.WidthEmu = 914400;
        image.HeightEmu = 457200;
        paragraph.Inlines.push_back(std::move(image));

        WordInline footnoteRef;
        footnoteRef.Kind = WordInline::Type::FootnoteRef;
        footnoteRef.NoteId = 1;
        paragraph.Inlines.push_back(std::move(footnoteRef));

        WordInline endnoteRef;
        endnoteRef.Kind = WordInline::Type::EndnoteRef;
        endnoteRef.NoteId = 2;
        paragraph.Inlines.push_back(std::move(endnoteRef));

        WordInline commentRef;
        commentRef.Kind = WordInline::Type::CommentRef;
        commentRef.NoteId = 0;
        paragraph.Inlines.push_back(std::move(commentRef));

        WordInline field;
        field.Kind = WordInline::Type::Field;
        field.FieldCode = "PAGE";
        field.Text = "1";
        paragraph.Inlines.push_back(std::move(field));

        for (const char* kind : {"line", "page", "column"})
        {
            WordInline lineBreak;
            lineBreak.Kind = WordInline::Type::Break;
            lineBreak.BreakKind = kind;
            paragraph.Inlines.push_back(std::move(lineBreak));
        }

        WordBlock block;
        block.Kind = WordBlock::Type::Paragraph;
        block.Paragraph = std::move(paragraph);
        return block;
    }

    /// A table with a span origin, a covered position and a nested table.
    static WordBlock MakeTableBlock()
    {
        WordTableCell spanning;
        spanning.ColSpan = 2;
        spanning.RowSpan = 2;
        spanning.Blocks.push_back(MakeTextParagraphBlock("Spanning"));

        WordTableCell covered;
        covered.Covered = true;

        WordTableRow firstRow;
        firstRow.Cells.push_back(std::move(spanning));
        firstRow.Cells.push_back(std::move(covered));

        WordTable nested;
        WordTableCell nestedCell;
        nestedCell.Blocks.push_back(MakeTextParagraphBlock("Nested"));
        WordTableRow nestedRow;
        nestedRow.Cells.push_back(std::move(nestedCell));
        nested.Rows.push_back(std::move(nestedRow));

        WordBlock nestedBlock;
        nestedBlock.Kind = WordBlock::Type::Table;
        nestedBlock.Table = std::move(nested);

        WordTableCell host;
        host.Blocks.push_back(std::move(nestedBlock));

        WordTableRow secondRow;
        secondRow.Cells.push_back(std::move(host));

        WordTable table;
        table.StyleId = "TableGrid";
        table.Rows.push_back(std::move(firstRow));
        table.Rows.push_back(std::move(secondRow));

        WordBlock block;
        block.Kind = WordBlock::Type::Table;
        block.Table = std::move(table);
        return block;
    }

    static WordBlock MakeSectionBreakBlock()
    {
        WordBlock block;
        block.Kind = WordBlock::Type::SectionBreak;
        return block;
    }

    static ExcelCellModel MakeCell(std::string address, std::string type, std::string value)
    {
        ExcelCellModel cell;
        cell.Address = std::move(address);
        cell.Type = std::move(type);
        cell.Value = std::move(value);
        return cell;
    }

    static PptTextFrame MakeTextFrame(const std::string& text, int level)
    {
        PptRun run;
        run.Text = text;
        run.Bold = true;
        run.Italic = true;
        run.Underline = true;
        run.FontSizePt = 24.0;
        run.Color = "0000FF";
        run.Hyperlink = "https://example.com";

        PptParagraph paragraph;
        paragraph.Level = level;
        paragraph.Runs.push_back(std::move(run));

        PptTextFrame frame;
        frame.Paragraphs.push_back(std::move(paragraph));
        return frame;
    }

    static PptShape MakePlaceholderShape()
    {
        PptShape shape;
        shape.Kind = PptShape::Type::Placeholder;
        shape.Name = "Title 1";
        shape.PlaceholderType = "title";
        shape.Text = MakeTextFrame("Q3 Results", 0);
        return shape;
    }

    static PptShape MakeTextBoxShape()
    {
        PptShape shape;
        shape.Kind = PptShape::Type::TextBox;
        shape.Name = "TextBox 2";
        shape.Transform.Present = true;
        shape.Transform.X = 457200;
        shape.Transform.Y = 1600200;
        shape.Transform.Cx = 8229600;
        shape.Transform.Cy = 2000000;
        shape.Text = MakeTextFrame("Point A", 1);
        return shape;
    }

    static PptShape MakePictureShape()
    {
        PptShape shape;
        shape.Kind = PptShape::Type::Picture;
        shape.Name = "Picture 3";
        shape.MediaId = "media1";
        shape.AltText = "Chart";
        return shape;
    }

    static PptShape MakeTableShape()
    {
        PptTableCell anchor;
        anchor.RowSpan = 1;
        anchor.ColSpan = 2;
        anchor.Text = MakeTextFrame("Header", 0);

        PptTableCell covered;
        covered.Covered = true;

        PptTableModel table;
        table.Rows.push_back({std::move(anchor), std::move(covered)});

        PptShape shape;
        shape.Kind = PptShape::Type::Table;
        shape.Name = "Table 4";
        shape.Table = std::move(table);
        return shape;
    }

    static PptShape MakeGroupShape()
    {
        PptShape child;
        child.Kind = PptShape::Type::TextBox;
        child.Text = MakeTextFrame("Inside group", 0);

        PptShape shape;
        shape.Kind = PptShape::Type::Group;
        shape.Name = "Group 5";
        shape.Children.push_back(std::move(child));
        return shape;
    }

    static PptShape MakeOtherShape()
    {
        PptShape shape;
        shape.Kind = PptShape::Type::Other;
        shape.Name = "Chart 6";
        return shape;
    }
};

TEST_CASE("Document model schema is published under docs/schemas [unit] [tools] [conversion]")
{
    const auto path = SchemaTestModels::SchemaArtifactPath();
    REQUIRE_MESSAGE(std::filesystem::exists(path),
                    "Missing published schema; regenerate with "
                    "'exyoki schema --output docs/schemas/<name>.schema.json'");

    auto expected = GetDocumentModelJsonSchema();
    expected.erase(std::remove(expected.begin(), expected.end(), '\r'), expected.end());

    const auto published = SchemaTestModels::ReadFile(path);
    CHECK_MESSAGE(published == expected,
                  "The published schema is stale; regenerate it with "
                  "'exyoki schema --output docs/schemas/<name>.schema.json'");
}

TEST_CASE("Serialized Word envelopes conform to the schema [unit] [tools] [conversion]")
{
    const auto model = SchemaTestModels::MakeMaximalWordModel();

    SUBCASE("with external media")
    {
        const auto json = SerializeModelJson(model, false);
        std::vector<ToolDiagnostic> diagnostics;
        CHECK(ValidateModelJson(json, diagnostics));
        CHECK(diagnostics.empty());
        for (const auto& diagnostic : diagnostics)
        {
            MESSAGE(diagnostic.Context << ": " << diagnostic.Message);
        }
    }

    SUBCASE("with embedded media")
    {
        const auto json = SerializeModelJson(model, true);
        std::vector<ToolDiagnostic> diagnostics;
        CHECK(ValidateModelJson(json, diagnostics));
        for (const auto& diagnostic : diagnostics)
        {
            MESSAGE(diagnostic.Context << ": " << diagnostic.Message);
        }
    }

    SUBCASE("covers every inline and block variant")
    {
        // Guards the guard: a fixture that stops producing a variant would make
        // the conformance check above pass without exercising that schema branch.
        const auto json = SerializeModelJson(model, false);
        for (const char* token : {"\"paragraph\"", "\"table\"", "\"sectionBreak\"", "\"text\"", "\"link\"",
                                  "\"image\"", "\"footnoteRef\"", "\"endnoteRef\"", "\"commentRef\"",
                                  "\"field\"", "\"break\"", "\"covered\"", "\"heading\"", "\"list\""})
        {
            CAPTURE(token);
            CHECK(json.find(token) != std::string::npos);
        }
    }
}

TEST_CASE("Serialized Excel envelopes conform to the schema [unit] [tools] [conversion]")
{
    const auto model = SchemaTestModels::MakeMaximalExcelModel();
    const auto json = SerializeModelJson(model, false);

    std::vector<ToolDiagnostic> diagnostics;
    CHECK(ValidateModelJson(json, diagnostics));
    for (const auto& diagnostic : diagnostics)
    {
        MESSAGE(diagnostic.Context << ": " << diagnostic.Message);
    }

    for (const char* token : {"\"string\"", "\"number\"", "\"bool\"", "\"error\"", "\"datetime\"",
                              "\"formula\"", "\"cached\"", "\"merges\"", "\"tables\"", "\"hyperlinks\""})
    {
        CAPTURE(token);
        CHECK(json.find(token) != std::string::npos);
    }
}

TEST_CASE("Serialized PowerPoint envelopes conform to the schema [unit] [tools] [conversion]")
{
    const auto model = SchemaTestModels::MakeMaximalPowerPointModel();
    const auto json = SerializeModelJson(model, true);

    std::vector<ToolDiagnostic> diagnostics;
    CHECK(ValidateModelJson(json, diagnostics));
    for (const auto& diagnostic : diagnostics)
    {
        MESSAGE(diagnostic.Context << ": " << diagnostic.Message);
    }

    for (const char* token : {"\"placeholder\"", "\"textBox\"", "\"picture\"", "\"table\"", "\"group\"",
                              "\"other\"", "\"transform\"", "\"children\"", "\"notes\"", "\"comments\""})
    {
        CAPTURE(token);
        CHECK(json.find(token) != std::string::npos);
    }
}

TEST_CASE("Schema-valid envelopes survive a parse round trip [unit] [tools] [conversion]")
{
    const DocumentModel models[] = {SchemaTestModels::MakeMaximalWordModel(),
                                    SchemaTestModels::MakeMaximalExcelModel(),
                                    SchemaTestModels::MakeMaximalPowerPointModel()};
    for (const auto& model : models)
    {
        const auto json = SerializeModelJson(model, true);

        std::vector<ToolDiagnostic> parseDiagnostics;
        const auto parsed = ParseModelJson(json, parseDiagnostics);
        CHECK(parsed.Family == model.Family);

        // Re-serializing a parsed model must land back inside the schema too;
        // otherwise the parser and the serializer disagree about the contract.
        const auto reserialized = SerializeModelJson(parsed, true);
        std::vector<ToolDiagnostic> diagnostics;
        CHECK(ValidateModelJson(reserialized, diagnostics));
        for (const auto& diagnostic : diagnostics)
        {
            MESSAGE(diagnostic.Context << ": " << diagnostic.Message);
        }
    }
}

TEST_CASE("The schema rejects malformed envelopes [unit] [tools] [conversion]")
{
    SUBCASE("not JSON at all")
    {
        std::vector<ToolDiagnostic> diagnostics;
        CHECK_FALSE(ValidateModelJson("{ this is not json", diagnostics));
        CHECK_FALSE(diagnostics.empty());
    }

    SUBCASE("unknown envelope member")
    {
        // The closed objects are what turns the schema into a drift detector:
        // a key the serializer might start writing has to fail here.
        const auto json = R"({"format":"exyokioffice-document","version":1,"family":"word",
                              "document":{"body":[]},"surprise":true})";
        std::vector<ToolDiagnostic> diagnostics;
        CHECK_FALSE(ValidateModelJson(json, diagnostics));
        CHECK_FALSE(diagnostics.empty());
    }

    SUBCASE("unknown member inside a paragraph")
    {
        const auto json = R"({"format":"exyokioffice-document","version":1,"family":"word",
                              "document":{"body":[{"type":"paragraph","content":[],"weight":3}]}})";
        std::vector<ToolDiagnostic> diagnostics;
        CHECK_FALSE(ValidateModelJson(json, diagnostics));
    }

    SUBCASE("unknown inline type")
    {
        const auto json = R"({"format":"exyokioffice-document","version":1,"family":"word",
                              "document":{"body":[{"type":"paragraph","content":[{"type":"marquee"}]}]}})";
        std::vector<ToolDiagnostic> diagnostics;
        CHECK_FALSE(ValidateModelJson(json, diagnostics));
    }

    SUBCASE("wrong payload for the declared family")
    {
        const auto json = R"({"format":"exyokioffice-document","version":1,"family":"excel",
                              "document":{"body":[]}})";
        std::vector<ToolDiagnostic> diagnostics;
        CHECK_FALSE(ValidateModelJson(json, diagnostics));
    }

    SUBCASE("unknown family")
    {
        const auto json = R"({"format":"exyokioffice-document","version":1,"family":"visio",
                              "document":{}})";
        std::vector<ToolDiagnostic> diagnostics;
        CHECK_FALSE(ValidateModelJson(json, diagnostics));
    }

    SUBCASE("wrong format identifier")
    {
        const auto json = R"({"format":"something-else","version":1,"family":"word",
                              "document":{"body":[]}})";
        std::vector<ToolDiagnostic> diagnostics;
        CHECK_FALSE(ValidateModelJson(json, diagnostics));
    }

    SUBCASE("bad cell type token")
    {
        const auto json = R"({"format":"exyokioffice-document","version":1,"family":"excel",
                              "document":{"sheets":[{"name":"S","cells":[
                                  {"cell":"A1","type":"currency","value":"1"}]}]}})";
        std::vector<ToolDiagnostic> diagnostics;
        CHECK_FALSE(ValidateModelJson(json, diagnostics));
    }

    SUBCASE("diagnostics point at the offending value")
    {
        const auto json = R"({"format":"exyokioffice-document","version":1,"family":"word",
                              "document":{"body":[{"type":"paragraph","content":[],"heading":42}]}})";
        std::vector<ToolDiagnostic> diagnostics;
        CHECK_FALSE(ValidateModelJson(json, diagnostics));
        REQUIRE_FALSE(diagnostics.empty());
        bool located = false;
        for (const auto& diagnostic : diagnostics)
        {
            located = located || diagnostic.Context.find("/document/body/0") != std::string::npos;
        }
        CHECK(located);
    }
}

TEST_CASE("Envelopes read from the document corpus conform to the schema [unit] [tools] [conversion]")
{
    // The synthetic fixtures above prove the schema covers what the serializer
    // can write; this proves it covers what real Office documents make it
    // write. The corpus is optional, so a checkout without it stays green.
    struct Family
    {
        const char* Directory;
        const char* Extension;
        DocumentModel (*Read)(const std::filesystem::path&, const ModelReadOptions&,
                              std::vector<ToolDiagnostic>&);
    };
    const Family families[] = {{"word", ".docx", &ReadWordModel},
                               {"excel", ".xlsx", &ReadExcelModel},
                               {"powerpoint", ".pptx", &ReadPowerPointModel}};

    ExyokiOffice::Size checked = 0;
    for (const auto& family : families)
    {
        const auto directory = std::filesystem::path(EXYOKIOFFICE_SOURCE_DIR) / "corpus" / family.Directory;
        if (!std::filesystem::is_directory(directory))
        {
            continue;
        }

        for (const auto& entry : std::filesystem::directory_iterator(directory))
        {
            // Word leaves "~$name.docx" owner files behind; they are not documents.
            if (!entry.is_regular_file() || entry.path().extension() != family.Extension ||
                entry.path().filename().string().rfind("~$", 0) == 0)
            {
                continue;
            }

            CAPTURE(entry.path().filename().string());
            std::vector<ToolDiagnostic> readDiagnostics;
            const auto model = family.Read(entry.path(), ModelReadOptions{}, readDiagnostics);
            REQUIRE(model.Family != DocumentFamily::Unknown);

            const auto json = SerializeModelJson(model, true);
            std::vector<ToolDiagnostic> diagnostics;
            CHECK(ValidateModelJson(json, diagnostics));
            for (const auto& diagnostic : diagnostics)
            {
                MESSAGE(diagnostic.Context << ": " << diagnostic.Message);
            }
            ++checked;
        }
    }
    MESSAGE("Corpus documents checked against the schema: " << checked);
}

TEST_CASE("A minimal envelope is accepted for every family [unit] [tools] [conversion]")
{
    const char* envelopes[] = {
        R"({"format":"exyokioffice-document","version":1,"family":"word","document":{"body":[]}})",
        R"({"format":"exyokioffice-document","version":1,"family":"excel","document":{"sheets":[]}})",
        R"({"format":"exyokioffice-document","version":1,"family":"powerpoint","document":{"slides":[]}})"};
    for (const char* envelope : envelopes)
    {
        CAPTURE(envelope);
        std::vector<ToolDiagnostic> diagnostics;
        CHECK(ValidateModelJson(envelope, diagnostics));
        for (const auto& diagnostic : diagnostics)
        {
            MESSAGE(diagnostic.Context << ": " << diagnostic.Message);
        }
    }
}
