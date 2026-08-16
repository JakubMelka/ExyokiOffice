// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "doctest.h"

#include "TestSupport.hpp"

#include "ExyokiOffice/OpenXmlPackage.hpp"
#include "ExyokiOffice/Word/WordDocument.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <string>

using ExyokiOffice::Word::WordDocumentEditor;

/// Documents whose paragraph structure the typed authoring API cannot produce.
class WordTextModelHelpers
{
public:
    static constexpr std::string_view WordNamespace =
        "http://schemas.openxmlformats.org/wordprocessingml/2006/main";

    /// Opens an editor over a document whose body is exactly @p body.
    static std::shared_ptr<WordDocumentEditor> EditorWithBody(const std::string& body)
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor);
        editor->AddParagraph("placeholder");
        const auto path = ExyokiOfficeTests::MakeTemporaryPath("exyoki_text_model", ".docx");
        REQUIRE(editor->SaveToFile(path));

        {
            ExyokiOffice::OpenXmlPackage package;
            REQUIRE(package.LoadFromFile(path));
            auto part = package.GetPartByUri("/word/document.xml");
            REQUIRE(part);
            std::string xml = "<w:document xmlns:w=\"";
            xml += WordNamespace;
            xml += "\"><w:body>";
            xml += body;
            xml += "</w:body></w:document>";
            part->SetXmlString(xml);
            REQUIRE(package.SaveToFile(path));
        }

        auto reopened = WordDocumentEditor::Open(path);
        REQUIRE(reopened);
        return reopened;
    }

    static std::string MainPartXml(const std::shared_ptr<WordDocumentEditor>& editor)
    {
        const auto path = ExyokiOfficeTests::MakeTemporaryPath("exyoki_text_model_out", ".docx");
        REQUIRE(editor->SaveToFile(path));
        ExyokiOffice::OpenXmlPackage package;
        REQUIRE(package.LoadFromFile(path));
        auto part = package.GetPartByUri("/word/document.xml");
        REQUIRE(part);
        return part->GetXmlString();
    }
};

TEST_SUITE("Word.TextModel")
{
    TEST_CASE("Text inside a hyperlink belongs to the paragraph [unit] [word] [word-text-model]")
    {
        // PlainText() saw this text and Find() did not, so a search reported no
        // match on a string the reader can see, and any offset the two produced
        // addressed different characters.
        auto editor = WordTextModelHelpers::EditorWithBody(
            "<w:p><w:r><w:t>see </w:t></w:r>"
            "<w:hyperlink w:anchor=\"top\"><w:r><w:t>the manual</w:t></w:r></w:hyperlink>"
            "<w:r><w:t> for details</w:t></w:r></w:p>");

        const auto paragraphs = editor->Paragraphs();
        REQUIRE(paragraphs.size() == 1);
        const auto& paragraph = paragraphs.front();

        CHECK(paragraph->PlainText() == "see the manual for details");
        const auto found = paragraph->Find("the manual");
        REQUIRE(found);
        CHECK(paragraph->GetText(*found) == "the manual");
    }

    TEST_CASE("Replacing text inside a hyperlink actually replaces it [unit] [word] [word-text-model]")
    {
        // The run lives under the hyperlink, not under the paragraph. Removing
        // it from the paragraph does nothing, and the replacement used to be
        // reported as done while the document was unchanged.
        auto editor = WordTextModelHelpers::EditorWithBody(
            "<w:p><w:r><w:t>go to </w:t></w:r>"
            "<w:hyperlink w:anchor=\"top\"><w:r><w:t>OLDNAME</w:t></w:r></w:hyperlink></w:p>");

        const auto paragraph = editor->Paragraphs().front();
        REQUIRE(paragraph->ReplaceAll("OLDNAME", "NEWNAME") == 1);
        CHECK(paragraph->PlainText() == "go to NEWNAME");

        const auto xml = WordTextModelHelpers::MainPartXml(editor);
        CHECK(xml.find("OLDNAME") == std::string::npos);
        CHECK(xml.find("NEWNAME") != std::string::npos);
    }

    TEST_CASE("Deleted text is not part of the paragraph's text [unit] [word] [word-text-model]")
    {
        // Text inside w:del is not displayed. Including it would let a search
        // match text the reader cannot see and let a replacement write into a
        // tracked deletion.
        auto editor = WordTextModelHelpers::EditorWithBody(
            "<w:p><w:del w:id=\"1\" w:author=\"A\"><w:r><w:delText>removed</w:delText></w:r></w:del>"
            "<w:ins w:id=\"2\" w:author=\"A\"><w:r><w:t>added</w:t></w:r></w:ins></w:p>");

        const auto paragraph = editor->Paragraphs().front();
        CHECK(paragraph->PlainText() == "added");
        CHECK_FALSE(paragraph->Find("removed"));
    }

    TEST_CASE("Tabs and breaks are characters of the paragraph's text [unit] [word] [word-text-model]")
    {
        auto editor = WordTextModelHelpers::EditorWithBody(
            "<w:p><w:r><w:t>Name</w:t><w:tab/><w:t>Value</w:t><w:br/><w:t>next</w:t></w:r></w:p>");

        const auto paragraph = editor->Paragraphs().front();
        CHECK(paragraph->PlainText() == "Name\tValue\nnext");

        const auto found = paragraph->Find("Name\tValue");
        REQUIRE(found);
        CHECK(found->Start == 0);
    }

    TEST_CASE("A replacement written back keeps tabs and breaks as elements [unit] [word] [word-text-model]")
    {
        auto editor = WordTextModelHelpers::EditorWithBody(
            "<w:p><w:r><w:t>a</w:t><w:tab/><w:t>b</w:t></w:r></w:p>");

        const auto paragraph = editor->Paragraphs().front();
        REQUIRE(paragraph->ReplaceAll("a\tb", "x\ty") == 1);
        CHECK(paragraph->PlainText() == "x\ty");

        // A literal tab character inside `w:t` is whitespace Word collapses; the
        // tab has to come back as a `w:tab` element or the replacement lost it.
        const auto xml = WordTextModelHelpers::MainPartXml(editor);
        CHECK(xml.find("<w:tab") != std::string::npos);
        // Not as a character inside the text element, which is where a naive
        // write-back would have put it.
        CHECK(xml.find("\t</w:t>") == std::string::npos);
        CHECK(xml.find(">x\ty<") == std::string::npos);

        const auto path = ExyokiOfficeTests::MakeTemporaryPath("exyoki_tab_roundtrip", ".docx");
        REQUIRE(editor->SaveToFile(path));
        auto reopened = WordDocumentEditor::Open(path);
        REQUIRE(reopened);
        CHECK(reopened->Paragraphs().front()->PlainText() == "x\ty");
    }

    TEST_CASE("Text boxes anchored in a paragraph are not its text [unit] [word] [word-text-model]")
    {
        // The text box holds its own paragraphs. Counting them here would make
        // one paragraph's offsets address another paragraph's characters.
        auto editor = WordTextModelHelpers::EditorWithBody(
            "<w:p><w:r><w:t>visible</w:t>"
            "<w:pict><v:shape xmlns:v=\"urn:schemas-microsoft-com:vml\"><v:textbox><w:txbxContent>"
            "<w:p><w:r><w:t>inside the box</w:t></w:r></w:p>"
            "</w:txbxContent></v:textbox></v:shape></w:pict></w:r></w:p>");

        const auto paragraph = editor->Paragraphs().front();
        CHECK(paragraph->PlainText() == "visible");
        CHECK_FALSE(paragraph->Find("inside the box"));
    }

    TEST_CASE("Replacing text leaves the rest of the run alone [unit] [word] [word-text-model]")
    {
        // A `w:br` is a line break in one document and a page break in the next,
        // and a `w:noBreakHyphen` is a hyphen that must not be broken across
        // lines. Both read as one character, and rebuilding the run from its text
        // turned each into its plain counterpart - so replacing one word moved
        // where the page ended.
        auto editor = WordTextModelHelpers::EditorWithBody(
            "<w:p><w:r><w:t>alpha</w:t><w:br w:type=\"page\"/><w:t>beta</w:t>"
            "<w:noBreakHyphen/><w:t>gamma</w:t></w:r></w:p>");

        const auto paragraph = editor->Paragraphs().front();
        REQUIRE(paragraph->PlainText() == "alpha\nbeta-gamma");

        const auto found = paragraph->Find("beta");
        REQUIRE(found);
        REQUIRE(paragraph->ReplaceText(*found, "delta"));
        CHECK(paragraph->PlainText() == "alpha\ndelta-gamma");

        const auto xml = WordTextModelHelpers::MainPartXml(editor);
        CHECK(xml.find("w:type=\"page\"") != std::string::npos);
        CHECK(xml.find("noBreakHyphen") != std::string::npos);
    }

    TEST_CASE("A replacement covering a special character removes it [unit] [word] [word-text-model]")
    {
        // The other half of the same contract: inside the replaced range the
        // characters are gone, and so are the elements that stood for them.
        auto editor = WordTextModelHelpers::EditorWithBody(
            "<w:p><w:r><w:t>alpha</w:t><w:tab/><w:t>beta</w:t></w:r></w:p>");

        const auto paragraph = editor->Paragraphs().front();
        REQUIRE(paragraph->PlainText() == "alpha\tbeta");

        const auto found = paragraph->Find("a\tb");
        REQUIRE(found);
        REQUIRE(paragraph->ReplaceText(*found, "A B"));
        CHECK(paragraph->PlainText() == "alphA Beta");

        const auto xml = WordTextModelHelpers::MainPartXml(editor);
        CHECK(xml.find("<w:tab") == std::string::npos);
    }

    TEST_CASE("Added text keeps its spaces by default [unit] [word] [word-text-model]")
    {
        // Without xml:space="preserve" an XML reader may collapse the trailing
        // space, and `AddParagraph("Hello ") + AddText("world")` came back as
        // "Helloworld" - text the caller wrote, silently lost.
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor);
        auto paragraph = editor->AddParagraph("Hello ");
        REQUIRE(paragraph);
        REQUIRE(paragraph->AddText("world"));

        const auto xml = WordTextModelHelpers::MainPartXml(editor);
        CHECK(xml.find("xml:space=\"preserve\"") != std::string::npos);

        const auto path = ExyokiOfficeTests::MakeTemporaryPath("exyoki_preserve", ".docx");
        REQUIRE(editor->SaveToFile(path));
        auto reopened = WordDocumentEditor::Open(path);
        REQUIRE(reopened);
        CHECK(reopened->Paragraphs().front()->PlainText() == "Hello world");
    }
}
