// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

// One test case per row of the Word table in docs/Compatibility.md.
//
// Each case follows the same shape, because that is what the three graded
// columns mean:
//
//   Create   author the construct from nothing with WordDocumentEditor;
//   Edit     reopen the saved package, find the construct through the reading
//            API rather than a kept pointer, and change it;
//   Preserve save, open and save again, and require the package to come back
//            byte-for-byte identical.
//
// Rows graded No or Partial are tested for exactly that: the restriction the
// Notes column states is asserted, so a row that quietly becomes stricter or
// looser fails here rather than misleading a reader.

#include "doctest.h"

#include "TestSupport.hpp"

#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/Wordprocessing.hpp"
#include "ExyokiOffice/OpenXMLElement.hpp"
#include "ExyokiOffice/Packaging/GeneratedParts.hpp"
#include "ExyokiOffice/Word/WordDocument.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <regex>
#include <string>
#include <string_view>
#include <vector>

namespace
{

using namespace ExyokiOffice::Word;
using ExyokiOffice::Byte;
using ExyokiOfficeTests::CheckPreservation;
using ExyokiOfficeTests::RoundTrip;
using ExyokiOfficeTests::ValidatePackage;

namespace Wordprocessing = ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing;

/// A 1x1 transparent PNG, which DetectImageFormat recognizes.
std::vector<Byte> MinimalPng()
{
    return {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52,
            0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x06, 0x00, 0x00, 0x00, 0x1F, 0x15, 0xC4,
            0x89, 0x00, 0x00, 0x00, 0x0D, 0x49, 0x44, 0x41, 0x54, 0x08, 0xD7, 0x63, 0xF8, 0xCF, 0xC0, 0xF0,
            0x1F, 0x00, 0x05, 0x00, 0x01, 0xFF, 0x89, 0x99, 0x3D, 0x1D, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45,
            0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82};
}

/// The first paragraph whose text contains @p needle, or nullptr.
std::shared_ptr<Paragraph> ParagraphContaining(const WordDocumentEditor::Ptr& editor, std::string_view needle)
{
    if (!editor)
    {
        return nullptr;
    }
    for (const auto& paragraph : editor->Paragraphs())
    {
        if (paragraph && paragraph->PlainText().find(needle) != std::string::npos)
        {
            return paragraph;
        }
    }
    return nullptr;
}

/// Saves, checks the package validates, and reports that the bytes round-trip
/// unchanged. Every row below ends with this.
void CheckSavesValidatesAndPreserves(const WordDocumentEditor::Ptr& editor)
{
    REQUIRE(editor != nullptr);
    const auto bytes = editor->SaveToMemory();
    REQUIRE_FALSE(bytes.empty());

    const auto validation = ValidatePackage(bytes);
    CAPTURE(validation.FirstError);
    CHECK_FALSE(validation.HasErrors);

    const auto preservation = CheckPreservation(bytes);
    REQUIRE(preservation.Ok);
    for (const auto& difference : preservation.Differences)
    {
        CAPTURE(difference);
        CHECK_MESSAGE(false, "package changed through open-save");
    }
    CHECK(preservation.Preserved);
}

} // namespace

TEST_SUITE("WordMatrixTests")
{

    TEST_CASE("Documents and lifecycle: create, edit, preserve [compat] [word] [word-documents]")
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);
        editor->Properties().SetTitle("Lifecycle");
        editor->Properties().SetCreator("ExyokiOffice");
        REQUIRE(editor->AddParagraph("Body") != nullptr);
        REQUIRE(editor->EnsureTheme());

        auto reopened = RoundTrip(editor);
        REQUIRE(reopened != nullptr);
        CHECK(reopened->Properties().GetTitle() == "Lifecycle");
        CHECK(reopened->Properties().GetCreator() == "ExyokiOffice");
        CHECK(reopened->ThemeXml().has_value());

        reopened->Properties().SetTitle("Edited");
        auto edited = RoundTrip(reopened);
        REQUIRE(edited != nullptr);
        CHECK(edited->Properties().GetTitle() == "Edited");

        // A transaction that rolls back leaves the document as it was.
        const auto before = edited->Paragraphs().size();
        {
            auto transaction = edited->BeginTransaction();
            REQUIRE(edited->AddParagraph("Rolled back") != nullptr);
            transaction.Rollback();
        }
        CHECK(edited->Paragraphs().size() == before);

        CheckSavesValidatesAndPreserves(edited);
    }

    TEST_CASE("Text and paragraphs: create, edit, preserve [compat] [word] [word-text]")
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);

        auto first = editor->AddParagraph("Placeholder {{CUSTOMER}} follows.");
        REQUIRE(first != nullptr);
        first->SetAlignment(Wordprocessing::JustificationValues::Center);
        auto run = first->AddRun(" formatted", RunStyle{.Bold = true});
        REQUIRE(run != nullptr);

        // Body cursors are the documented insertion model.
        REQUIRE(editor->Before(first).InsertParagraph("Inserted before") != nullptr);
        REQUIRE(editor->After(first).InsertParagraph("Inserted after") != nullptr);

        auto reopened = RoundTrip(editor);
        REQUIRE(reopened != nullptr);
        REQUIRE(reopened->Paragraphs().size() >= 3);
        CHECK(reopened->Paragraphs().front()->PlainText() == "Inserted before");

        auto target = ParagraphContaining(reopened, "{{CUSTOMER}}");
        REQUIRE(target != nullptr);
        CHECK(target->GetAlignment() == Wordprocessing::JustificationValues::Center);

        // Find and replace, both plain and regular expression, across runs.
        CHECK(target->Find("{{CUSTOMER}}").has_value());
        CHECK(target->ReplaceAll("{{CUSTOMER}}", "Contoso") == 1);
        CHECK(target->PlainText().find("Contoso") != std::string::npos);

        const std::regex pattern("[Ff]ollows");
        CHECK(target->FindAllRegex(pattern).size() == 1);
        CHECK(target->ReplaceAllRegex(pattern, "trails") == 1);

        auto edited = RoundTrip(reopened);
        REQUIRE(edited != nullptr);
        CHECK(ParagraphContaining(edited, "Contoso") != nullptr);
        CHECK(ParagraphContaining(edited, "trails") != nullptr);
        CHECK(ParagraphContaining(edited, "{{CUSTOMER}}") == nullptr);

        CheckSavesValidatesAndPreserves(edited);
    }

    TEST_CASE("Styles, headings and lists: create, edit, preserve [compat] [word] [word-styles]")
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);

        auto styles = editor->Styles();
        StyleDefinition quote;
        quote.StyleId = "CompatQuote";
        quote.Name = "Compat Quote";
        quote.Type = StyleType::Paragraph;
        REQUIRE(styles.CreateStyle(quote));

        REQUIRE(editor->AddHeading("Heading one", 1) != nullptr);
        auto quoted = editor->AddParagraph("Quoted text");
        REQUIRE(quoted != nullptr);
        quoted->SetStyleId("CompatQuote");

        const auto bullets = editor->EnsureBulletedListStyle();
        auto item = editor->AddParagraph("Bullet item");
        REQUIRE(item != nullptr);
        item->SetListStyle(bullets);

        auto reopened = RoundTrip(editor);
        REQUIRE(reopened != nullptr);

        auto reopenedStyles = reopened->Styles();
        CHECK(reopenedStyles.GetStyle("CompatQuote").has_value());
        auto quotedAgain = ParagraphContaining(reopened, "Quoted text");
        REQUIRE(quotedAgain != nullptr);
        CHECK(quotedAgain->GetStyleId() == "CompatQuote");

        // Edit: rename the style in place.
        auto updated = quote;
        updated.Name = "Compat Quote Edited";
        CHECK(reopenedStyles.UpdateStyle(updated));

        auto edited = RoundTrip(reopened);
        REQUIRE(edited != nullptr);
        const auto readBack = edited->Styles().GetStyle("CompatQuote");
        REQUIRE(readBack.has_value());
        CHECK(readBack->Name == "Compat Quote Edited");

        CheckSavesValidatesAndPreserves(edited);
    }

    TEST_CASE("Tables: create, edit, preserve [compat] [word] [word-tables]")
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);

        auto table = editor->AddTable(3, 3);
        REQUIRE(table != nullptr);
        // The table mutators are a fluent chain returning Table&.
        table->SetCellText(0, 0, "corner").SetCellText(1, 1, "middle").SetRowHeader(0, true).MergeCells(2, 0, 1, 2);

        auto reopened = RoundTrip(editor);
        REQUIRE(reopened != nullptr);
        REQUIRE_FALSE(reopened->Tables().empty());

        auto readBack = reopened->Tables().front();
        REQUIRE(readBack != nullptr);
        CHECK(readBack->GetRowCount() == 3);
        CHECK(readBack->GetColumnCount() == 3);
        CHECK_FALSE(readBack->GetLogicalGrid().empty());

        // Edit: grow the table and change a cell.
        readBack->AddRow();
        readBack->SetCellText(0, 0, "changed");

        auto edited = RoundTrip(reopened);
        REQUIRE(edited != nullptr);
        REQUIRE_FALSE(edited->Tables().empty());
        CHECK(edited->Tables().front()->GetRowCount() == 4);

        CheckSavesValidatesAndPreserves(edited);
    }

    TEST_CASE("Images: create, edit, preserve [compat] [word] [word-images]")
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);

        auto image = editor->AddImageFromData(MinimalPng());
        REQUIRE(image != nullptr);
        image->SetAltText("Compat", "A one pixel image");
        image->SetCrop(0.1, 0.1, 0.1, 0.1);

        auto floating = editor->AddImageFromData(MinimalPng(), ImageLayout::Floating, ImageWrap::Square);
        REQUIRE(floating != nullptr);

        auto reopened = RoundTrip(editor);
        REQUIRE(reopened != nullptr);

        std::vector<std::shared_ptr<Image>> images;
        for (const auto& paragraph : reopened->Paragraphs())
        {
            for (const auto& found : paragraph->Images())
            {
                images.push_back(found);
            }
        }
        REQUIRE(images.size() == 2);
        CHECK(images.front()->GetTitle() == "Compat");
        CHECK(images.front()->GetDescription() == "A one pixel image");

        images.front()->SetAltText("Edited", "Edited description");
        auto edited = RoundTrip(reopened);
        REQUIRE(edited != nullptr);

        bool foundEdited = false;
        for (const auto& paragraph : edited->Paragraphs())
        {
            for (const auto& found : paragraph->Images())
            {
                if (found && found->GetTitle() == "Edited")
                {
                    foundEdited = true;
                }
            }
        }
        CHECK(foundEdited);

        CheckSavesValidatesAndPreserves(edited);
    }

    TEST_CASE("Hyperlinks and bookmarks: create, edit, preserve [compat] [word] [word-hyperlinks]")
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);

        auto target = editor->AddParagraph("Destination");
        REQUIRE(target != nullptr);
        REQUIRE(target->AddBookmark("Target") != nullptr);

        auto source = editor->AddParagraph();
        REQUIRE(source != nullptr);
        REQUIRE(source->AddHyperlink("External", "https://example.com") != nullptr);
        REQUIRE(source->AddInternalHyperlink("Internal", "Target") != nullptr);

        auto reopened = RoundTrip(editor);
        REQUIRE(reopened != nullptr);
        CHECK(reopened->FindBookmark("Target") != nullptr);

        auto linkParagraph = ParagraphContaining(reopened, "External");
        REQUIRE(linkParagraph != nullptr);
        auto links = linkParagraph->Hyperlinks();
        REQUIRE(links.size() == 2);
        CHECK(links.front()->GetUrl() == "https://example.com");

        links.front()->SetUrl("https://example.org");
        auto edited = RoundTrip(reopened);
        REQUIRE(edited != nullptr);
        auto editedParagraph = ParagraphContaining(edited, "External");
        REQUIRE(editedParagraph != nullptr);
        REQUIRE_FALSE(editedParagraph->Hyperlinks().empty());
        CHECK(editedParagraph->Hyperlinks().front()->GetUrl() == "https://example.org");

        CheckSavesValidatesAndPreserves(edited);
    }

    TEST_CASE("Fields and TOC: authored but never evaluated [compat] [word] [word-fields]")
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);

        REQUIRE(editor->AddTableOfContents(1, 3) != nullptr);
        auto paragraph = editor->AddParagraph();
        REQUIRE(paragraph != nullptr);
        REQUIRE(paragraph->AddField("PAGE", "1") != nullptr);
        REQUIRE(paragraph->AddField("MERGEFIELD Customer", "placeholder") != nullptr);

        auto reopened = RoundTrip(editor);
        REQUIRE(reopened != nullptr);

        auto fields = reopened->Fields();
        REQUIRE_FALSE(fields.empty());

        bool foundPage = false;
        bool foundMerge = false;
        for (const auto& field : fields)
        {
            if (!field)
            {
                continue;
            }

            if (field->GetInstruction().find("PAGE") != std::string::npos)
            {
                foundPage = true;
                // "Values are never computed" is not just an omission: a
                // layout-dependent field refuses to have a result written into
                // it at all, and is marked dirty so Word refreshes it.
                CHECK(field->IsLayoutDependent());
                CHECK(field->IsDirty());
                CHECK(field->GetResult() == "1");
                CHECK_FALSE(field->SetResult("7"));
                CHECK(field->GetResult() == "1");
            }
            else if (field->GetInstruction().find("MERGEFIELD") != std::string::npos)
            {
                foundMerge = true;
                // A field whose result does not depend on layout is editable.
                CHECK_FALSE(field->IsLayoutDependent());
                CHECK(field->SetResult("Contoso Ltd."));
            }
        }
        CHECK(foundPage);
        CHECK(foundMerge);

        auto edited = RoundTrip(reopened);
        REQUIRE(edited != nullptr);
        bool foundEdited = false;
        for (const auto& field : edited->Fields())
        {
            if (field && field->GetInstruction().find("MERGEFIELD") != std::string::npos &&
                field->GetResult() == "Contoso Ltd.")
            {
                foundEdited = true;
            }
        }
        CHECK(foundEdited);

        CheckSavesValidatesAndPreserves(edited);
    }

    TEST_CASE("Embedded charts: no create, but edit and preserve [compat] [word] [word-charts]")
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);
        REQUIRE(editor->AddParagraph("Chart carrier") != nullptr);

        // Create is graded No: no helper anchors a new chart, so the fixture is
        // attached through the package layer the way another tool would have
        // produced it.
        auto mainPart = editor->GetDocument()->GetMainDocumentPart();
        REQUIRE(mainPart != nullptr);
        auto chartPart = mainPart->AddChartPart();
        REQUIRE(chartPart != nullptr);
        chartPart->SetXmlString(
            R"(<c:chartSpace xmlns:c="http://schemas.openxmlformats.org/drawingml/2006/chart")"
            R"( xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main">)"
            R"(<c:chart><c:plotArea><c:layout/><c:barChart><c:barDir val="col"/>)"
            R"(<c:grouping val="clustered"/><c:varyColors val="0"/>)"
            R"(<c:ser><c:idx val="0"/><c:order val="0"/><c:tx><c:v>North</c:v></c:tx>)"
            R"(<c:val><c:numRef><c:f>Sheet1!$B$1:$B$2</c:f><c:numCache>)"
            R"(<c:formatCode>General</c:formatCode><c:ptCount val="2"/>)"
            R"(<c:pt idx="0"><c:v>1</c:v></c:pt><c:pt idx="1"><c:v>2</c:v></c:pt>)"
            R"(</c:numCache></c:numRef></c:val></c:ser>)"
            R"(<c:axId val="1"/><c:axId val="2"/></c:barChart>)"
            R"(<c:catAx><c:axId val="1"/><c:scaling><c:orientation val="minMax"/></c:scaling>)"
            R"(<c:delete val="0"/><c:axPos val="b"/><c:crossAx val="2"/></c:catAx>)"
            R"(<c:valAx><c:axId val="2"/><c:scaling><c:orientation val="minMax"/></c:scaling>)"
            R"(<c:delete val="0"/><c:axPos val="l"/><c:crossAx val="1"/></c:valAx>)"
            R"(</c:plotArea></c:chart></c:chartSpace>)");

        // A chart part on its own is not a chart in the document; the body has
        // to reference it through a drawing, which is the piece the missing
        // create helper would have written.
        auto documentXml = mainPart->GetXmlString();
        const auto bodyEnd = documentXml.find("</w:body>");
        REQUIRE(bodyEnd != std::string::npos);
        documentXml.insert(
            bodyEnd,
            R"(<w:p><w:r><w:drawing>)"
            R"(<wp:inline xmlns:wp="http://schemas.openxmlformats.org/drawingml/2006/wordprocessingDrawing">)"
            R"(<a:graphic xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main">)"
            R"(<a:graphicData uri="http://schemas.openxmlformats.org/drawingml/2006/chart">)"
            R"(<c:chart xmlns:c="http://schemas.openxmlformats.org/drawingml/2006/chart")"
            R"( xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships" r:id=")" +
                chartPart->RelationshipId() +
                R"("/></a:graphicData></a:graphic></wp:inline></w:drawing></w:r></w:p>)");
        mainPart->SetXmlString(documentXml);

        const auto charts = editor->Charts();
        REQUIRE(charts.size() == 1);
        CHECK(charts.front().Type == WordChartType::Column);
        REQUIRE(charts.front().Series.size() == 1);
        CHECK(charts.front().Series.front().Name == "North");

        // Edit is graded Yes: series and title are rewritten in place.
        WordChartSeries replacement;
        replacement.Name = "South";
        replacement.Values = {5.0, 6.0};
        CHECK(editor->UpdateChartData(charts.front().RelationshipId, {replacement}, std::string("Updated")));

        auto reopened = RoundTrip(editor);
        REQUIRE(reopened != nullptr);
        const auto reopenedCharts = reopened->Charts();
        REQUIRE(reopenedCharts.size() == 1);
        CHECK(reopenedCharts.front().Title == "Updated");
        REQUIRE(reopenedCharts.front().Series.size() == 1);
        CHECK(reopenedCharts.front().Series.front().Name == "South");

        CheckSavesValidatesAndPreserves(reopened);
    }

    TEST_CASE("Sections and page setup: create, edit, preserve [compat] [word] [word-sections]")
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);
        REQUIRE(editor->AddParagraph("Page one") != nullptr);

        auto section = editor->EnsureFinalSection();
        REQUIRE(section != nullptr);
        section->SetPageOrientation(PageOrientation::Landscape);
        section->SetHeaderText(HeaderFooterType::Default, "Header text");
        section->SetFooterText(HeaderFooterType::Default, "Footer text");

        auto reopened = RoundTrip(editor);
        REQUIRE(reopened != nullptr);
        REQUIRE_FALSE(reopened->Sections().empty());

        auto readBack = reopened->Sections().back();
        REQUIRE(readBack != nullptr);
        auto header = readBack->GetHeader(HeaderFooterType::Default);
        REQUIRE(header != nullptr);
        CHECK(header->PlainText().find("Header text") != std::string::npos);

        readBack->SetHeaderText(HeaderFooterType::Default, "Edited header");
        auto edited = RoundTrip(reopened);
        REQUIRE(edited != nullptr);
        REQUIRE_FALSE(edited->Sections().empty());
        auto editedHeader = edited->Sections().back()->GetHeader(HeaderFooterType::Default);
        REQUIRE(editedHeader != nullptr);
        CHECK(editedHeader->PlainText().find("Edited header") != std::string::npos);

        CheckSavesValidatesAndPreserves(edited);
    }

    TEST_CASE("Notes and comments: create, edit, preserve [compat] [word] [word-notes]")
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);

        auto paragraph = editor->AddParagraph("Annotated");
        REQUIRE(paragraph != nullptr);
        REQUIRE(paragraph->AddFootnote("A footnote") != nullptr);
        REQUIRE(paragraph->AddEndnote("An endnote") != nullptr);
        REQUIRE(paragraph->AddCommentOnParagraph("A comment", CommentAuthor{.Name = "Reviewer", .Initials = "R"}) !=
                nullptr);

        auto reopened = RoundTrip(editor);
        REQUIRE(reopened != nullptr);
        CHECK_FALSE(reopened->Footnotes().empty());
        CHECK_FALSE(reopened->Endnotes().empty());
        REQUIRE_FALSE(reopened->Comments().empty());

        auto comment = reopened->Comments().front();
        REQUIRE(comment != nullptr);
        CHECK(comment->GetAuthor() == "Reviewer");
        CHECK(comment->PlainText().find("A comment") != std::string::npos);

        comment->SetText("Edited comment");
        comment->SetAuthor("Editor");
        auto edited = RoundTrip(reopened);
        REQUIRE(edited != nullptr);
        REQUIRE_FALSE(edited->Comments().empty());
        CHECK(edited->Comments().front()->GetAuthor() == "Editor");
        CHECK(edited->Comments().front()->PlainText().find("Edited comment") != std::string::npos);

        CheckSavesValidatesAndPreserves(edited);
    }

    TEST_CASE("Content controls: partial create and edit, full preserve "
              "[compat] [word] [word-content-controls]")
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);

        auto paragraph = editor->AddParagraph();
        REQUIRE(paragraph != nullptr);
        auto control = paragraph->AddInlineContentControl("compat-tag", "Compat alias");
        REQUIRE(control != nullptr);
        control->SetText("Inside the control");

        auto reopened = RoundTrip(editor);
        REQUIRE(reopened != nullptr);
        auto controls = reopened->ContentControls();
        REQUIRE_FALSE(controls.empty());

        // Partial means the modelled properties — tag, alias, lock and text —
        // and nothing beyond them.
        bool foundInline = false;
        for (const auto& found : controls)
        {
            if (found && found->GetTag() == "compat-tag")
            {
                foundInline = true;
                CHECK(found->GetAlias() == "Compat alias");
                CHECK(found->Level() == ContentControlLevel::Inline);
                CHECK(found->PlainText() == "Inside the control");
                found->SetAlias("Edited alias");
            }
        }
        CHECK(foundInline);

        auto edited = RoundTrip(reopened);
        REQUIRE(edited != nullptr);
        bool foundEdited = false;
        for (const auto& found : edited->ContentControls())
        {
            if (found && found->GetTag() == "compat-tag" && found->GetAlias() == "Edited alias")
            {
                foundEdited = true;
            }
        }
        CHECK(foundEdited);

        CheckSavesValidatesAndPreserves(edited);
    }

    TEST_CASE("Revisions: produced by comparison, then accepted or rejected "
              "[compat] [word] [word-revisions]")
    {
        auto original = WordDocumentEditor::CreateNew();
        REQUIRE(original != nullptr);
        REQUIRE(original->AddParagraph("Unchanged paragraph") != nullptr);
        REQUIRE(original->AddParagraph("Original wording") != nullptr);

        auto revised = WordDocumentEditor::CreateNew();
        REQUIRE(revised != nullptr);
        REQUIRE(revised->AddParagraph("Unchanged paragraph") != nullptr);
        REQUIRE(revised->AddParagraph("Revised wording") != nullptr);

        // Create is graded Partial: revisions come from CompareWith, and there
        // is no "record my edits" tracking mode that would produce them.
        const auto produced = original->CompareWith(*revised, RevisionAuthor{});
        CHECK(produced > 0);

        auto reopened = RoundTrip(original);
        REQUIRE(reopened != nullptr);
        CHECK_FALSE(reopened->Revisions().empty());

        const auto accepted = reopened->AcceptAllRevisions();
        CHECK(accepted > 0);
        CHECK(reopened->Revisions().empty());

        auto edited = RoundTrip(reopened);
        REQUIRE(edited != nullptr);
        CHECK(edited->Revisions().empty());
        CHECK(ParagraphContaining(edited, "Revised wording") != nullptr);

        CheckSavesValidatesAndPreserves(edited);
    }

    TEST_CASE("Merging and mail merge: create, edit, preserve [compat] [word] [word-merge]")
    {
        auto target = WordDocumentEditor::CreateNew();
        REQUIRE(target != nullptr);
        auto merged = target->AddParagraph();
        REQUIRE(merged != nullptr);
        REQUIRE(merged->AddField("MERGEFIELD Customer", "Customer") != nullptr);

        auto source = WordDocumentEditor::CreateNew();
        REQUIRE(source != nullptr);
        REQUIRE(source->AddParagraph("Imported from another document") != nullptr);

        // InsertDocument brings another document's body in through a cursor.
        CHECK(target->Body().InsertDocument(*source));

        TemplateMergeData data;
        data.Values["Customer"] = "Contoso Ltd.";
        const auto result = target->MergeTemplate(data);
        CHECK(result.FieldsMerged == 1);

        auto reopened = RoundTrip(target);
        REQUIRE(reopened != nullptr);
        CHECK(ParagraphContaining(reopened, "Imported from another document") != nullptr);
        CHECK(ParagraphContaining(reopened, "Contoso Ltd.") != nullptr);

        REQUIRE(reopened->AddParagraph("Appended after merge") != nullptr);
        auto edited = RoundTrip(reopened);
        REQUIRE(edited != nullptr);
        CHECK(ParagraphContaining(edited, "Appended after merge") != nullptr);

        CheckSavesValidatesAndPreserves(edited);
    }

    TEST_CASE("Document protection: create, edit, preserve [compat] [word] [word-protection]")
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);
        REQUIRE(editor->AddParagraph("Protected") != nullptr);

        WordProtectionOptions options;
        options.Editing = WordProtectionType::ReadOnly;
        const auto protectResult = editor->ProtectDocument(options, "secret");
        CAPTURE(protectResult.Message);
        REQUIRE(protectResult.Succeeded());

        auto reopened = RoundTrip(editor);
        REQUIRE(reopened != nullptr);
        auto info = reopened->GetDocumentProtection();
        REQUIRE(info.has_value());
        CHECK(info->Options.Editing == WordProtectionType::ReadOnly);
        CHECK(info->HasPassword);

        // The wrong password is refused; the right one lifts the restriction.
        CHECK_FALSE(reopened->UnprotectDocument("wrong").Succeeded());
        CHECK(reopened->UnprotectDocument("secret").Succeeded());

        auto edited = RoundTrip(reopened);
        REQUIRE(edited != nullptr);
        CHECK_FALSE(edited->GetDocumentProtection().has_value());

        CheckSavesValidatesAndPreserves(edited);
    }

    TEST_CASE("SmartArt, text boxes, equations and OLE objects are preserved untouched "
              "[compat] [word] [word-preserved]")
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);
        REQUIRE(editor->AddParagraph("Carrier paragraph") != nullptr);

        // No typed API models these constructs, so the row is exercised the way
        // the matrix describes it: content outside the typed model, written by
        // some other producer and carried through verbatim. A foreign-namespace
        // element stands in for the real thing until BLD-004 supplies file
        // fixtures with genuine SmartArt and OLE payloads.
        auto mainPart = editor->GetDocument()->GetMainDocumentPart();
        REQUIRE(mainPart != nullptr);

        auto xml = mainPart->GetXmlString();
        const auto bodyEnd = xml.find("</w:body>");
        REQUIRE(bodyEnd != std::string::npos);
        xml.insert(bodyEnd,
                   R"(<vnd:customBlock xmlns:vnd="http://example.com/vendor" kind="smart-art-stand-in">)"
                   R"(<vnd:payload>opaque</vnd:payload></vnd:customBlock>)");
        mainPart->SetXmlString(xml);

        const auto bytes = editor->SaveToMemory();
        REQUIRE_FALSE(bytes.empty());

        auto reopened = WordDocumentEditor::Open(bytes);
        REQUIRE(reopened != nullptr);
        const auto reopenedXml = reopened->GetDocument()->GetMainDocumentPart()->GetXmlString();
        CHECK(reopenedXml.find("customBlock") != std::string::npos);
        CHECK(reopenedXml.find("smart-art-stand-in") != std::string::npos);
        CHECK(reopenedXml.find("opaque") != std::string::npos);

        // Create and Edit are graded No; Preserve is the whole promise.
        const auto preservation = CheckPreservation(bytes);
        REQUIRE(preservation.Ok);
        for (const auto& difference : preservation.Differences)
        {
            CAPTURE(difference);
            CHECK_MESSAGE(false, "package changed through open-save");
        }
        CHECK(preservation.Preserved);
    }

} // TEST_SUITE("WordMatrixTests")
