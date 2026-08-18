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

#include <algorithm>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

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

    static std::vector<char> ReadAllBytes(const std::filesystem::path& path)
    {
        std::ifstream file(path, std::ios::binary);
        REQUIRE(file.is_open());
        return std::vector<char>(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    }

    /**
     * @brief Flips one byte of the stored data of @p entryName, breaking its CRC.
     *
     * Written against the archive rather than through the library, because what
     * is being reproduced is a file the library cannot produce: an entry whose
     * central directory is intact and whose bytes do not match the checksum
     * beside them. That is what a truncated download, a bit-rotted disk and a
     * crafted package all look like from inside `zip_entry_read`.
     */
    static void CorruptEntryPayload(const std::filesystem::path& path, std::string_view entryName)
    {
        auto bytes = ReadAllBytes(path);
        const std::string_view all(bytes.data(), bytes.size());
        const auto readLe16 = [&bytes](ExyokiOffice::Size at)
        {
            return static_cast<ExyokiOffice::Size>(static_cast<unsigned char>(bytes[at])) |
                   (static_cast<ExyokiOffice::Size>(static_cast<unsigned char>(bytes[at + 1])) << 8);
        };

        // Local file header: the signature, 26 bytes of fixed fields, the name
        // length at 26, the extra length at 28, then the name and the data. The
        // signature can also occur inside compressed data, so the name decides.
        for (ExyokiOffice::Size at = 0;;)
        {
            const auto found = all.find("PK\x03\x04", at, 4);
            REQUIRE(found != std::string_view::npos);
            const auto nameLength = readLe16(found + 26);
            const auto extraLength = readLe16(found + 28);
            if (found + 30 + nameLength <= bytes.size() &&
                all.substr(found + 30, nameLength) == entryName)
            {
                const auto data = found + 30 + nameLength + extraLength;
                REQUIRE(data < bytes.size());
                bytes[data] = static_cast<char>(bytes[data] ^ 0xFF);
                std::ofstream out(path, std::ios::binary | std::ios::trunc);
                REQUIRE(out.is_open());
                out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
                REQUIRE(out.good());
                return;
            }
            at = found + 4;
        }
    }

    static bool HasDiagnostic(const std::vector<ToolDiagnostic>& diagnostics, std::string_view text)
    {
        return std::any_of(diagnostics.begin(), diagnostics.end(), [text](const auto& diagnostic)
                           { return diagnostic.Message.find(text) != std::string::npos ||
                                    diagnostic.Context.find(text) != std::string::npos; });
    }
};

TEST_SUITE("Tools.Redactor")
{
    TEST_CASE("A part the loader could not read is still reported after the open [security-regression]")
    {
        // The warning is recorded by the package loader, and everything above it
        // used to start by clearing the collection it lives in. Opening through
        // an editor - which is how the tools and the MCP servers open documents,
        // and how almost every application does - therefore threw away the one
        // statement that the document is not all of the file.
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor);
        editor->AddParagraph("Body text");
        const auto path = ExyokiOfficeTests::MakeTemporaryPath("exyoki_unreadable", ".docx");
        REQUIRE(editor->SaveToFile(path));
        editor.reset();

        // Any part but the main one: what is being reproduced is a document
        // that still opens with a piece of it missing, so which piece it is does
        // not matter and pinning a particular name would only tie the case to
        // what CreateNew happens to write today.
        std::string victimUri;
        {
            ExyokiOffice::OpenXmlPackage intact;
            REQUIRE(intact.LoadFromFile(path));
            for (const auto& record : ListParts(intact))
            {
                if (record.Uri != "/word/document.xml")
                {
                    victimUri = record.Uri;
                    break;
                }
            }
        }
        REQUIRE_FALSE(victimUri.empty());
        REQUIRE(victimUri.front() == '/');

        RedactorTestHelpers::CorruptEntryPayload(path, std::string_view(victimUri).substr(1));

        SUBCASE("the editor keeps what the loader said")
        {
            auto reopened = WordDocumentEditor::Open(path);
            REQUIRE(reopened);
            auto document = reopened->GetDocument();
            REQUIRE(document);

            // The document opens: the main part is intact, and one part that is
            // not is no reason to refuse the rest of the file.
            CHECK(document->GetPartByUri(victimUri) == nullptr);

            const auto& issues = document->LastValidationResult().Issues();
            CHECK(std::any_of(issues.begin(), issues.end(), [](const auto& issue)
                              { return issue.Id == ExyokiOffice::ValidationErrorId::OpcEntryUnreadable; }));
        }

        SUBCASE("a redaction says what it never got to look at")
        {
            const auto output = ExyokiOfficeTests::MakeTemporaryPath("exyoki_unreadable_out", ".docx");
            const auto result = RedactDocument(path, output);

            // Ok, because everything present was redacted. The diagnostic is
            // what separates that from "this document is now clean".
            CHECK(result.Ok);
            CHECK(RedactorTestHelpers::HasDiagnostic(result.Diagnostics, "could not be read"));
            CHECK(RedactorTestHelpers::HasDiagnostic(result.Diagnostics, victimUri.substr(1)));
        }
    }

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

    TEST_CASE("The revision-save id registry is stripped from the settings part [unit] [tools] [redact]")
    {
        // The per-story pass reaches the `w:rsid*` attributes scattered through
        // the body, but the `w:rsids` registry - the list of every editing
        // session that ever touched the file - lives in word/settings.xml, which
        // is not a story part, so a body-only pass would leave it behind.
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor);
        editor->AddParagraph("text");
        const auto path = ExyokiOfficeTests::MakeTemporaryPath("exyoki_redact_settings", ".docx");
        REQUIRE(editor->SaveToFile(path));

        {
            ExyokiOffice::Packaging::WordDocument package;
            REQUIRE(package.LoadFromFile(path));
            REQUIRE(package.EnsureDocumentSettingsPart());
            REQUIRE(package.SaveToFile(path));
        }

        std::string settings = "<w:settings xmlns:w=\"";
        settings += RedactorTestHelpers::WordNamespace;
        settings += "\"><w:rsids><w:rsidRoot w:val=\"00AB12CD\"/><w:rsid w:val=\"00AB12CD\"/>"
                    "<w:rsid w:val=\"00EF3456\"/></w:rsids></w:settings>";
        RedactorTestHelpers::SetPartXml(path, "/word/settings.xml", settings);

        const auto output = ExyokiOfficeTests::MakeTemporaryPath("exyoki_redact_settings_out", ".docx");
        REQUIRE(RedactDocument(path, output).Ok);

        const auto redacted = RedactorTestHelpers::GetPartXml(output, "/word/settings.xml");
        CHECK(redacted.find("rsid") == std::string::npos);
        CHECK(redacted.find("00AB12CD") == std::string::npos);
        CHECK(redacted.find("00EF3456") == std::string::npos);
    }

    TEST_CASE("Every revision-save id registry is stripped, even a duplicate one [unit] [tools] [redact]")
    {
        // The loader does not run strict schema validation, so a non-conformant
        // settings part can carry more than one `w:rsids`. Dropping only the first
        // would republish the editing-session ids the second one still lists.
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor);
        editor->AddParagraph("text");
        const auto path = ExyokiOfficeTests::MakeTemporaryPath("exyoki_redact_settings_dup", ".docx");
        REQUIRE(editor->SaveToFile(path));

        {
            ExyokiOffice::Packaging::WordDocument package;
            REQUIRE(package.LoadFromFile(path));
            REQUIRE(package.EnsureDocumentSettingsPart());
            REQUIRE(package.SaveToFile(path));
        }

        std::string settings = "<w:settings xmlns:w=\"";
        settings += RedactorTestHelpers::WordNamespace;
        settings += "\"><w:rsids><w:rsid w:val=\"00AB12CD\"/></w:rsids>"
                    "<w:rsids><w:rsid w:val=\"00EF3456\"/></w:rsids></w:settings>";
        RedactorTestHelpers::SetPartXml(path, "/word/settings.xml", settings);

        const auto output = ExyokiOfficeTests::MakeTemporaryPath("exyoki_redact_settings_dup_out", ".docx");
        REQUIRE(RedactDocument(path, output).Ok);

        const auto redacted = RedactorTestHelpers::GetPartXml(output, "/word/settings.xml");
        CHECK(redacted.find("rsids") == std::string::npos);
        CHECK(redacted.find("00AB12CD") == std::string::npos);
        CHECK(redacted.find("00EF3456") == std::string::npos);
    }

    TEST_CASE("A hidden glossary style does not delete a main run that shares its id [unit] [tools] [redact]")
    {
        // The main document and the glossary each carry their own styles part, and
        // the same styleId in each is a different style. A style that hides text in
        // the glossary must not reach a visible main-document run that merely shares
        // the id, or redaction would silently delete text the document shows.
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor);
        editor->AddParagraph("placeholder");
        const auto path = ExyokiOfficeTests::MakeTemporaryPath("exyoki_redact_glossary", ".docx");
        REQUIRE(editor->SaveToFile(path));

        std::string mainStylesUri;
        std::string glossaryDocUri;
        std::string glossaryStylesUri;
        {
            ExyokiOffice::Packaging::WordDocument package;
            REQUIRE(package.LoadFromFile(path));
            auto mainPart = package.GetMainDocumentPart();
            REQUIRE(mainPart);

            auto mainStyles = mainPart->GetStyleDefinitionsPart();
            if (!mainStyles)
            {
                mainStyles = mainPart->AddStyleDefinitionsPart();
            }
            REQUIRE(mainStyles);
            mainStylesUri = mainStyles->Uri();

            auto glossary = mainPart->AddGlossaryDocumentPart();
            REQUIRE(glossary);
            glossaryDocUri = glossary->Uri();
            auto glossaryStyles = glossary->AddStyleDefinitionsPart();
            REQUIRE(glossaryStyles);
            glossaryStylesUri = glossaryStyles->Uri();

            REQUIRE(package.SaveToFile(path));
        }

        std::string mainBody = "<w:document xmlns:w=\"";
        mainBody += RedactorTestHelpers::WordNamespace;
        mainBody += "\"><w:body><w:p><w:r><w:rPr><w:rStyle w:val=\"Shared\"/></w:rPr>"
                    "<w:t>main shared text</w:t></w:r></w:p></w:body></w:document>";
        RedactorTestHelpers::SetPartXml(path, "/word/document.xml", mainBody);

        std::string mainStyles = "<w:styles xmlns:w=\"";
        mainStyles += RedactorTestHelpers::WordNamespace;
        mainStyles += "\"><w:style w:type=\"character\" w:styleId=\"Shared\"><w:rPr/></w:style></w:styles>";
        RedactorTestHelpers::SetPartXml(path, mainStylesUri, mainStyles);

        std::string glossaryDoc = "<w:glossaryDocument xmlns:w=\"";
        glossaryDoc += RedactorTestHelpers::WordNamespace;
        glossaryDoc += "\"><w:docParts><w:docPart><w:docPartBody>"
                       "<w:p><w:r><w:rPr><w:rStyle w:val=\"Shared\"/></w:rPr>"
                       "<w:t>glossary shared text</w:t></w:r></w:p>"
                       "</w:docPartBody></w:docPart></w:docParts></w:glossaryDocument>";
        RedactorTestHelpers::SetPartXml(path, glossaryDocUri, glossaryDoc);

        std::string glossaryStyles = "<w:styles xmlns:w=\"";
        glossaryStyles += RedactorTestHelpers::WordNamespace;
        glossaryStyles += "\"><w:style w:type=\"character\" w:styleId=\"Shared\">"
                          "<w:rPr><w:vanish/></w:rPr></w:style></w:styles>";
        RedactorTestHelpers::SetPartXml(path, glossaryStylesUri, glossaryStyles);

        const auto output = ExyokiOfficeTests::MakeTemporaryPath("exyoki_redact_glossary_out", ".docx");
        REQUIRE(RedactDocument(path, output).Ok);

        // The main run keeps its text: the main styles part does not hide "Shared".
        const auto mainOut = RedactorTestHelpers::GetPartXml(output, "/word/document.xml");
        CHECK(mainOut.find("main shared text") != std::string::npos);

        // The glossary run is removed: the glossary styles part does hide "Shared".
        const auto glossaryOut = RedactorTestHelpers::GetPartXml(output, glossaryDocUri);
        CHECK(glossaryOut.find("glossary shared text") == std::string::npos);
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
