// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "doctest.h"

#include "TestSupport.hpp"

#include "ExyokiOffice/Excel/ExcelDocument.hpp"
#include "ExyokiOffice/OpenXmlPackage.hpp"
#include "ExyokiOffice/Tools/DocumentRedactor.hpp"
#include "ExyokiOffice/Tools/PackageInspector.hpp"
#include "ExyokiOffice/Word/WordDocument.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <string>

using namespace ExyokiOffice::Tools;
using ExyokiOffice::Word::WordDocumentEditor;

/// Building the packages these cases redact, at the XML level.
///
/// A tracked change in a header, a paragraph mark marked as deleted, a run
/// hidden by its style: none of them can be produced through the typed editor,
/// and all of them are what a document arriving from a real review looks like.
class RedactorTestHelpers
{
public:
    /// Writes @p xml into the part @p uri of the package saved at @p path.
    static void SetPartXml(const std::filesystem::path& path, const std::string& uri, const std::string& xml)
    {
        ExyokiOffice::OpenXmlPackage package;
        package.SetPackageLimits(ExyokiOffice::OpenXmlPackageLimits::Unlimited());
        REQUIRE(package.LoadFromFile(path));
        auto part = package.GetPartByUri(uri);
        REQUIRE(part);
        part->SetXmlString(xml);
        REQUIRE(package.SaveToFile(path));
    }

    static std::string GetPartXml(const std::filesystem::path& path, const std::string& uri)
    {
        ExyokiOffice::OpenXmlPackage package;
        package.SetPackageLimits(ExyokiOffice::OpenXmlPackageLimits::Unlimited());
        REQUIRE(package.LoadFromFile(path));
        auto part = package.GetPartByUri(uri);
        REQUIRE(part);
        return part->GetXmlString();
    }

    static bool HasPart(const std::filesystem::path& path, std::string_view uriPrefix)
    {
        ExyokiOffice::OpenXmlPackage package;
        package.SetPackageLimits(ExyokiOffice::OpenXmlPackageLimits::Unlimited());
        REQUIRE(package.LoadFromFile(path));
        for (const auto& record : ListParts(package))
        {
            if (record.Uri.starts_with(uriPrefix))
            {
                return true;
            }
        }
        return false;
    }

    static constexpr std::string_view WordNamespace =
        "http://schemas.openxmlformats.org/wordprocessingml/2006/main";

    /// A document whose main part is exactly @p body, wrapped in the usual root.
    static std::filesystem::path DocumentWithBody(const std::string& body)
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor);
        editor->AddParagraph("placeholder");
        const auto path = ExyokiOfficeTests::MakeTemporaryPath("exyoki_redact", ".docx");
        REQUIRE(editor->SaveToFile(path));

        std::string xml = "<w:document xmlns:w=\"";
        xml += WordNamespace;
        xml += "\"><w:body>";
        xml += body;
        xml += "</w:body></w:document>";
        SetPartXml(path, "/word/document.xml", xml);
        return path;
    }
};

TEST_SUITE("Tools.Redactor")
{
    TEST_CASE("Revisions are accepted in every story part, not only the body [unit] [tools] [redact]")
    {
        // The typed editor's AcceptAllRevisions walks the main document. A
        // deletion in a header survived it, so a document published after
        // "accept all" still carried the text the reviewer had removed.
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor);
        editor->AddParagraph("Body text");
        auto section = editor->EnsureFinalSection();
        REQUIRE(section);
        section->SetHeaderText(ExyokiOffice::Word::HeaderFooterType::Default, "header");

        const auto path = ExyokiOfficeTests::MakeTemporaryPath("exyoki_redact_header", ".docx");
        REQUIRE(editor->SaveToFile(path));

        std::string headerUri;
        {
            ExyokiOffice::OpenXmlPackage package;
            REQUIRE(package.LoadFromFile(path));
            for (const auto& record : ListParts(package))
            {
                if (record.ContentType.ends_with("header+xml"))
                {
                    headerUri = record.Uri;
                }
            }
        }
        REQUIRE_FALSE(headerUri.empty());

        std::string headerXml = "<w:hdr xmlns:w=\"";
        headerXml += RedactorTestHelpers::WordNamespace;
        headerXml +=
            "\"><w:p>"
            "<w:del w:id=\"1\" w:author=\"Reviewer\"><w:r><w:delText>secret</w:delText></w:r></w:del>"
            "<w:ins w:id=\"2\" w:author=\"Reviewer\"><w:r><w:t>public</w:t></w:r></w:ins>"
            "</w:p></w:hdr>";
        RedactorTestHelpers::SetPartXml(path, headerUri, headerXml);

        const auto output = ExyokiOfficeTests::MakeTemporaryPath("exyoki_redact_header_out", ".docx");
        const auto result = RedactDocument(path, output);
        REQUIRE(result.Ok);
        CHECK(result.RevisionsResolved >= 2);

        const auto redacted = RedactorTestHelpers::GetPartXml(output, headerUri);
        CHECK(redacted.find("secret") == std::string::npos);
        CHECK(redacted.find("public") != std::string::npos);
        CHECK(redacted.find("Reviewer") == std::string::npos);
        CHECK(redacted.find("w:ins") == std::string::npos);
    }

    TEST_CASE("A deleted paragraph mark merges its paragraph into the next one [unit] [tools] [redact]")
    {
        // Accepting the deletion of a paragraph mark joins the two paragraphs.
        // Removing only the marker would leave them apart, which is what
        // rejecting the change means - the opposite of what was asked for.
        const auto path = RedactorTestHelpers::DocumentWithBody(
            "<w:p><w:pPr><w:rPr><w:del w:id=\"3\" w:author=\"Reviewer\"/></w:rPr></w:pPr>"
            "<w:r><w:t>first</w:t></w:r></w:p>"
            "<w:p><w:r><w:t>second</w:t></w:r></w:p>");

        const auto output = ExyokiOfficeTests::MakeTemporaryPath("exyoki_redact_mark_out", ".docx");
        REQUIRE(RedactDocument(path, output).Ok);

        auto reopened = WordDocumentEditor::Open(output);
        REQUIRE(reopened);
        const auto paragraphs = reopened->Paragraphs();
        REQUIRE(paragraphs.size() == 1);
        CHECK(paragraphs[0]->PlainText() == "firstsecond");
    }

    TEST_CASE("A row marked as deleted is removed with its cells [unit] [tools] [redact]")
    {
        const auto path = RedactorTestHelpers::DocumentWithBody(
            "<w:tbl>"
            "<w:tr><w:tc><w:p><w:r><w:t>kept</w:t></w:r></w:p></w:tc></w:tr>"
            "<w:tr><w:trPr><w:del w:id=\"4\" w:author=\"Reviewer\"/></w:trPr>"
            "<w:tc><w:p><w:r><w:t>dropped</w:t></w:r></w:p></w:tc></w:tr>"
            "</w:tbl>");

        const auto output = ExyokiOfficeTests::MakeTemporaryPath("exyoki_redact_row_out", ".docx");
        REQUIRE(RedactDocument(path, output).Ok);

        const auto redacted = RedactorTestHelpers::GetPartXml(output, "/word/document.xml");
        CHECK(redacted.find("kept") != std::string::npos);
        CHECK(redacted.find("dropped") == std::string::npos);
    }

    TEST_CASE("Records of former formatting do not survive the accept [unit] [tools] [redact]")
    {
        const auto path = RedactorTestHelpers::DocumentWithBody(
            "<w:p><w:pPr><w:pPrChange w:id=\"5\" w:author=\"Reviewer\"><w:pPr/></w:pPrChange></w:pPr>"
            "<w:r><w:rPr><w:rPrChange w:id=\"6\" w:author=\"Reviewer\"><w:rPr/></w:rPrChange></w:rPr>"
            "<w:t>text</w:t></w:r></w:p>");

        const auto output = ExyokiOfficeTests::MakeTemporaryPath("exyoki_redact_change_out", ".docx");
        REQUIRE(RedactDocument(path, output).Ok);

        const auto redacted = RedactorTestHelpers::GetPartXml(output, "/word/document.xml");
        CHECK(redacted.find("pPrChange") == std::string::npos);
        CHECK(redacted.find("rPrChange") == std::string::npos);
        CHECK(redacted.find("Reviewer") == std::string::npos);
        CHECK(redacted.find("text") != std::string::npos);
    }

    TEST_CASE("Text hidden by its style is removed [unit] [tools] [redact]")
    {
        // The run says nothing about being hidden; the style does. A redactor
        // that reads only run properties publishes exactly the text the author
        // took the trouble to hide.
        const auto path = RedactorTestHelpers::DocumentWithBody(
            "<w:p><w:r><w:rPr><w:rStyle w:val=\"HiddenNote\"/></w:rPr><w:t>internal note</w:t></w:r>"
            "<w:r><w:t>public text</w:t></w:r></w:p>");

        {
            // A fresh document has no styles part; the style manager creates it.
            auto editor = WordDocumentEditor::Open(path);
            REQUIRE(editor);
            ExyokiOffice::Word::StyleDefinition placeholder;
            placeholder.StyleId = "HiddenBase";
            placeholder.Name = "Hidden base";
            placeholder.Type = ExyokiOffice::Word::StyleType::Character;
            REQUIRE(editor->Styles().CreateStyle(placeholder));
            REQUIRE(editor->SaveToFile(path));
        }

        std::string styles = "<w:styles xmlns:w=\"";
        styles += RedactorTestHelpers::WordNamespace;
        styles +=
            "\"><w:style w:type=\"character\" w:styleId=\"HiddenBase\"><w:rPr><w:vanish/></w:rPr></w:style>"
            "<w:style w:type=\"character\" w:styleId=\"HiddenNote\">"
            "<w:basedOn w:val=\"HiddenBase\"/><w:rPr/></w:style></w:styles>";
        RedactorTestHelpers::SetPartXml(path, "/word/styles.xml", styles);

        const auto output = ExyokiOfficeTests::MakeTemporaryPath("exyoki_redact_style_out", ".docx");
        const auto result = RedactDocument(path, output);
        REQUIRE(result.Ok);
        CHECK(result.HiddenRunsRemoved == 1);

        const auto redacted = RedactorTestHelpers::GetPartXml(output, "/word/document.xml");
        CHECK(redacted.find("internal note") == std::string::npos);
        CHECK(redacted.find("public text") != std::string::npos);
    }

    TEST_CASE("The thumbnail and the custom XML store are removed [unit] [tools] [redact]")
    {
        // The thumbnail is a rendering of the first page made before any of
        // this ran, and it is kept in the package as an image.
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor);
        editor->AddParagraph("Confidential heading");
        const auto path = ExyokiOfficeTests::MakeTemporaryPath("exyoki_redact_thumb", ".docx");
        REQUIRE(editor->SaveToFile(path));

        {
            ExyokiOffice::Packaging::WordprocessingDocument package;
            REQUIRE(package.LoadFromFile(path));
            const std::vector<ExyokiOffice::Byte> jpeg{0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10, 0x4A, 0x46};
            auto thumbnail = package.AddThumbnailPart();
            REQUIRE(thumbnail);
            thumbnail->SetBinaryData(jpeg);
            REQUIRE(package.SaveToFile(path));
        }
        REQUIRE(RedactorTestHelpers::HasPart(path, "/docProps/thumbnail"));

        const auto output = ExyokiOfficeTests::MakeTemporaryPath("exyoki_redact_thumb_out", ".docx");
        const auto result = RedactDocument(path, output);
        REQUIRE(result.Ok);
        CHECK(result.PartsRemoved >= 1);
        CHECK_FALSE(RedactorTestHelpers::HasPart(output, "/docProps/thumbnail"));
    }

    TEST_CASE("Editing-session identifiers are stripped from the story parts [unit] [tools] [redact]")
    {
        const auto path = RedactorTestHelpers::DocumentWithBody(
            "<w:p w:rsidR=\"00AB12CD\" w:rsidRDefault=\"00AB12CD\"><w:r><w:t>text</w:t></w:r></w:p>");

        const auto output = ExyokiOfficeTests::MakeTemporaryPath("exyoki_redact_rsid_out", ".docx");
        REQUIRE(RedactDocument(path, output).Ok);

        const auto redacted = RedactorTestHelpers::GetPartXml(output, "/word/document.xml");
        CHECK(redacted.find("rsid") == std::string::npos);
        CHECK(redacted.find("00AB12CD") == std::string::npos);
    }

    TEST_CASE("Hiding switched off with w:val=\"false\" is not hiding [unit] [tools] [redact]")
    {
        // `w:vanish` and `w:specVanish` are both CT_OnOff, so both can be turned
        // off - which is how one run opts out of a style that hides text. Reading
        // the element's presence as the answer deleted text the document shows.
        const auto path = RedactorTestHelpers::DocumentWithBody(
            "<w:p>"
            "<w:r><w:rPr><w:specVanish w:val=\"false\"/></w:rPr><w:t>visible one</w:t></w:r>"
            "<w:r><w:rPr><w:vanish w:val=\"false\"/></w:rPr><w:t>visible two</w:t></w:r>"
            "<w:r><w:rPr><w:specVanish/></w:rPr><w:t>hidden one</w:t></w:r>"
            "<w:r><w:rPr><w:vanish/></w:rPr><w:t>hidden two</w:t></w:r>"
            "</w:p>");

        const auto output = ExyokiOfficeTests::MakeTemporaryPath("exyoki_redact_vanish_out", ".docx");
        const auto result = RedactDocument(path, output);
        REQUIRE(result.Ok);
        CHECK(result.HiddenRunsRemoved == 2);

        const auto redacted = RedactorTestHelpers::GetPartXml(output, "/word/document.xml");
        CHECK(redacted.find("visible one") != std::string::npos);
        CHECK(redacted.find("visible two") != std::string::npos);
        CHECK(redacted.find("hidden one") == std::string::npos);
        CHECK(redacted.find("hidden two") == std::string::npos);
    }

    TEST_CASE("Property-level revision markers go with the revisions [unit] [tools] [redact]")
    {
        // `w:trPr/w:ins` and `w:pPr/w:rPr/w:ins` are records, not wrappers: there
        // is no content to unwrap, so a pass that only knew about wrappers left
        // them in place - together with the author and the date they carry.
        const auto path = RedactorTestHelpers::DocumentWithBody(
            "<w:p><w:pPr><w:rPr><w:ins w:id=\"7\" w:author=\"Reviewer\"/></w:rPr></w:pPr>"
            "<w:r><w:t>paragraph</w:t></w:r></w:p>"
            "<w:tbl><w:tr><w:trPr><w:ins w:id=\"8\" w:author=\"Reviewer\"/></w:trPr>"
            "<w:tc><w:p><w:r><w:t>inserted row</w:t></w:r></w:p></w:tc></w:tr></w:tbl>");

        const auto output = ExyokiOfficeTests::MakeTemporaryPath("exyoki_redact_marker_out", ".docx");
        REQUIRE(RedactDocument(path, output).Ok);

        const auto redacted = RedactorTestHelpers::GetPartXml(output, "/word/document.xml");
        // The insertions are accepted, so their content stays and the record of
        // who inserted it does not.
        CHECK(redacted.find("paragraph") != std::string::npos);
        CHECK(redacted.find("inserted row") != std::string::npos);
        CHECK(redacted.find("Reviewer") == std::string::npos);
        CHECK(redacted.find("w:ins") == std::string::npos);
    }

    TEST_CASE("A paragraph mark that moved away merges like a deleted one [unit] [tools] [redact]")
    {
        const auto path = RedactorTestHelpers::DocumentWithBody(
            "<w:p><w:pPr><w:rPr><w:moveFrom w:id=\"9\" w:author=\"Reviewer\"/></w:rPr></w:pPr>"
            "<w:r><w:t>first</w:t></w:r></w:p>"
            "<w:p><w:r><w:t>second</w:t></w:r></w:p>");

        const auto output = ExyokiOfficeTests::MakeTemporaryPath("exyoki_redact_movefrom_out", ".docx");
        REQUIRE(RedactDocument(path, output).Ok);

        auto reopened = WordDocumentEditor::Open(output);
        REQUIRE(reopened);
        const auto paragraphs = reopened->Paragraphs();
        REQUIRE(paragraphs.size() == 1);
        CHECK(paragraphs[0]->PlainText() == "firstsecond");
    }

    TEST_CASE("Revisions inside kept comments are accepted too [unit] [tools] [redact]")
    {
        // --keep-comments is a supported combination, and a comment is a story
        // like any other: a tracked deletion in one hides text behind an author
        // name and a date that "revisions accepted" says are gone.
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor);
        auto paragraph = editor->AddParagraph("Body text");
        REQUIRE(paragraph);
        REQUIRE(paragraph->AddCommentOnParagraph("placeholder"));

        const auto path = ExyokiOfficeTests::MakeTemporaryPath("exyoki_redact_comment", ".docx");
        REQUIRE(editor->SaveToFile(path));

        std::string commentsXml = "<w:comments xmlns:w=\"";
        commentsXml += RedactorTestHelpers::WordNamespace;
        commentsXml +=
            "\"><w:comment w:id=\"1\" w:author=\"Reviewer\"><w:p>"
            "<w:del w:id=\"10\" w:author=\"Second Reviewer\"><w:r><w:delText>withdrawn</w:delText></w:r></w:del>"
            "<w:ins w:id=\"11\" w:author=\"Second Reviewer\"><w:r><w:t>kept remark</w:t></w:r></w:ins>"
            "</w:p></w:comment></w:comments>";
        RedactorTestHelpers::SetPartXml(path, "/word/comments.xml", commentsXml);

        RedactOptions options;
        options.RemoveComments = false;

        const auto output = ExyokiOfficeTests::MakeTemporaryPath("exyoki_redact_comment_out", ".docx");
        const auto result = RedactDocument(path, output, options);
        REQUIRE(result.Ok);

        const auto redacted = RedactorTestHelpers::GetPartXml(output, "/word/comments.xml");
        CHECK(redacted.find("kept remark") != std::string::npos);
        CHECK(redacted.find("withdrawn") == std::string::npos);
        CHECK(redacted.find("Second Reviewer") == std::string::npos);
    }

    TEST_CASE("The author registry stays as long as the comments do [unit] [tools] [redact]")
    {
        // A threaded comment names its author through the workbook person
        // registry. Removing the registry from under a comment that was kept
        // leaves every `personId` pointing at nothing: a broken workbook rather
        // than a redacted one.
        auto editor = ExyokiOffice::Excel::ExcelDocumentEditor::CreateNew();
        REQUIRE(editor);
        auto worksheet = editor->Worksheets().front();
        REQUIRE(worksheet);

        ExyokiOffice::Excel::ExcelThreadedComment comment;
        comment.Address = ExyokiOffice::Excel::CellAddress::ParseA1("A1").value();
        comment.PersonName = "Jane Reviewer";
        comment.Text = "Check this number";
        REQUIRE(worksheet->AddThreadedComment(comment));

        const auto path = ExyokiOfficeTests::MakeTemporaryPath("exyoki_redact_persons", ".xlsx");
        REQUIRE(editor->SaveToFile(path));
        REQUIRE(RedactorTestHelpers::HasPart(path, "/xl/persons/"));

        SUBCASE("kept comments keep it")
        {
            RedactOptions options;
            options.RemoveComments = false;

            const auto output = ExyokiOfficeTests::MakeTemporaryPath("exyoki_redact_persons_kept", ".xlsx");
            REQUIRE(RedactDocument(path, output, options).Ok);
            CHECK(RedactorTestHelpers::HasPart(output, "/xl/persons/"));
        }

        SUBCASE("removed comments take it with them")
        {
            const auto output = ExyokiOfficeTests::MakeTemporaryPath("exyoki_redact_persons_gone", ".xlsx");
            REQUIRE(RedactDocument(path, output).Ok);
            CHECK_FALSE(RedactorTestHelpers::HasPart(output, "/xl/persons/"));
        }
    }

    TEST_CASE("Descriptive metadata is cleared, not only the author [unit] [tools] [redact]")
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor);
        editor->AddParagraph("Body");
        auto document = editor->GetDocument();
        REQUIRE(document);
        document->SetCreator("Jane Reviewer");
        document->SetTitle("Internal draft - do not circulate");
        document->SetSubject("Acquisition of Contoso");
        document->SetKeywords("confidential, project falcon");

        const auto path = ExyokiOfficeTests::MakeTemporaryPath("exyoki_redact_props", ".docx");
        REQUIRE(editor->SaveToFile(path));

        const auto output = ExyokiOfficeTests::MakeTemporaryPath("exyoki_redact_props_out", ".docx");
        const auto result = RedactDocument(path, output);
        REQUIRE(result.Ok);

        ExyokiOffice::OpenXmlPackage package;
        REQUIRE(package.LoadFromFile(output));
        const auto properties = ReadCoreProperties(package);
        CHECK(properties.Creator.empty());
        CHECK(properties.Title.empty());
        CHECK(properties.Subject.empty());
        CHECK(properties.Keywords.empty());
    }
}
