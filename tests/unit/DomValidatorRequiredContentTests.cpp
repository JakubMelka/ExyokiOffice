// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

// ---------------------------------------------------------------------------
// Negative suite: documents that are deliberately invalid must be reported.
//
// The imported schema snapshot omits every default, and a particle without an
// `Occurs` property occurs exactly once. The generator once read that as
// "optional", which turned every required child in every content model into an
// optional one: a `p:presentation` without `p:notesSz` or a workbook without
// `x:sheets` validated clean while PowerPoint and Excel refused to open them.
// These cases pin the strict reading on markup small enough to read, next to
// the positive cases that show the same models accept complete content, so a
// regression in the generator or the matcher fails here rather than in an
// Office repair dialog.
// ---------------------------------------------------------------------------

#include "DomValidationTestSupport.hpp"

#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/Presentation.hpp"
#include "ExyokiOffice/OpenXmlPackageValidator.hpp"
#include "ExyokiOffice/PowerPoint/PowerPointDocument.hpp"

#include "doctest.h"

#include <string>

namespace
{

std::string PackageErrors(const ExyokiOffice::PowerPoint::PowerPointDocumentEditor& editor)
{
    const auto result = ExyokiOffice::OpenXmlPackageValidator(ExyokiOffice::OpenXmlDomValidationSettings{}).Validate(*editor.GetDocument());
    std::string errors;
    for (const auto& issue : result.Issues())
    {
        if (issue.Severity == ExyokiOffice::ValidationSeverity::Error)
        {
            if (!errors.empty())
            {
                errors += " | ";
            }
            errors += issue.Message + " in " + issue.PartUri + " at " + issue.Location.Path;
        }
    }
    return errors;
}

} // namespace

TEST_SUITE("DOM validation of required content")
{
    using ExyokiOfficeTests::DomValidation::ValidationErrors;

    TEST_CASE("a presentation without p:notesSz is incomplete [unit] [dom-required]")
    {
        // p:notesSz is the one mandatory child of CT_Presentation; PowerPoint
        // reports the file as damaged without it.
        const auto errors = ValidationErrors(
            R"(<p:presentation xmlns:p="http://schemas.openxmlformats.org/presentationml/2006/main")"
            R"( xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships">)"
            R"(<p:sldIdLst/><p:sldSz cx="12192000" cy="6858000"/></p:presentation>)");
        CHECK_MESSAGE(errors.find("Content of element 'p:presentation'") != std::string::npos, errors);
        CHECK_MESSAGE(errors.find("'p:notesSz'") != std::string::npos, errors);

        CHECK(ValidationErrors(
                  R"(<p:presentation xmlns:p="http://schemas.openxmlformats.org/presentationml/2006/main">)"
                  R"(<p:sldSz cx="12192000" cy="6858000"/><p:notesSz cx="6858000" cy="9144000"/></p:presentation>)") == "");
    }

    TEST_CASE("a modern comment's anchor is optional to the schema but written by the library [unit] [dom-required]")
    {
        // p188:cm opens with a choice of anchors (pc:sldMkLst, ...,
        // p188:unknownAnchor). Several members of that choice are declared
        // 0..unbounded, so the choice is satisfiable by nothing and a comment
        // without an anchor is schema-valid - the Open XML SDK validator agrees.
        // PowerPoint nevertheless refuses to load such a comment part, which is
        // why PresentationSlide::AddComment always writes an anchor (covered by
        // the presentation tests). This case pins the schema reading, so that a
        // future report about the anchor is looked for in the writer, not here.
        constexpr std::string_view head =
            R"(<p188:cmLst xmlns:p188="http://schemas.microsoft.com/office/powerpoint/2018/8/main")"
            R"( xmlns:pc="http://schemas.microsoft.com/office/powerpoint/2013/main/command")"
            R"( xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main">)"
            R"(<p188:cm id="{0B9E4F21-6D3C-4A8B-8F1E-2C3D4E5F6A7B}" authorId="{7C0F1D8E-2B7A-4E5B-9C6D-1A2B3C4D5E6F}" created="2026-01-01T00:00:00Z">)";
        constexpr std::string_view tail =
            R"(<p188:pos x="0" y="0"/><p188:txBody><a:bodyPr/><a:lstStyle/><a:p><a:r><a:t>x</a:t></a:r></a:p></p188:txBody>)"
            R"(</p188:cm></p188:cmLst>)";

        CHECK(ValidationErrors(std::string(head) + std::string(tail)) == "");
        CHECK(ValidationErrors(std::string(head) +
                               R"(<pc:sldMkLst><pc:docMk/><pc:sldMk cId="0" sldId="256"/></pc:sldMkLst>)" +
                               std::string(tail)) == "");
        CHECK(ValidationErrors(std::string(head) + R"(<p188:unknownAnchor/>)" + std::string(tail)) == "");

        // What is required: pc:sldMkLst must not appear after p188:pos.
        const auto misplaced = ValidationErrors(std::string(head) + R"(<p188:pos x="0" y="0"/>)" +
                                                R"(<pc:sldMkLst><pc:docMk/><pc:sldMk cId="0" sldId="256"/></pc:sldMkLst>)" +
                                                R"(<p188:txBody><a:bodyPr/><a:lstStyle/><a:p/></p188:txBody></p188:cm></p188:cmLst>)");
        CHECK_MESSAGE(misplaced.find("Content of element 'p188:cm'") != std::string::npos, misplaced);
    }

    TEST_CASE("a workbook without x:sheets is incomplete [unit] [dom-required]")
    {
        const auto errors = ValidationErrors(
            R"(<x:workbook xmlns:x="http://schemas.openxmlformats.org/spreadsheetml/2006/main">)"
            R"(<x:workbookPr/></x:workbook>)");
        CHECK_MESSAGE(errors.find("Content of element 'x:workbook'") != std::string::npos, errors);
        CHECK_MESSAGE(errors.find("'x:sheets'") != std::string::npos, errors);

        // x:sheets itself needs at least one x:sheet (1..unbounded).
        const auto emptySheets = ValidationErrors(
            R"(<x:workbook xmlns:x="http://schemas.openxmlformats.org/spreadsheetml/2006/main">)"
            R"(<x:sheets/></x:workbook>)");
        CHECK_MESSAGE(emptySheets.find("Content of element 'x:sheets'") != std::string::npos, emptySheets);
        CHECK_MESSAGE(emptySheets.find("'x:sheet'") != std::string::npos, emptySheets);

        CHECK(ValidationErrors(
                  R"(<x:workbook xmlns:x="http://schemas.openxmlformats.org/spreadsheetml/2006/main")"
                  R"( xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships">)"
                  R"(<x:sheets><x:sheet name="Sheet1" sheetId="1" r:id="rId1"/></x:sheets></x:workbook>)") == "");
    }

    TEST_CASE("a table without w:tblGrid and a text body without a paragraph are incomplete [unit] [dom-required]")
    {
        const auto table = ValidationErrors(
            R"(<w:tbl xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main"><w:tblPr/></w:tbl>)");
        CHECK_MESSAGE(table.find("Content of element 'w:tbl'") != std::string::npos, table);
        CHECK_MESSAGE(table.find("'w:tblGrid'") != std::string::npos, table);

        // Optional children stay optional: a paragraph may be empty.
        CHECK(ValidationErrors(
                  R"(<w:p xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main"/>)") == "");

        // a:txBody requires a:bodyPr and at least one a:p; a:lstStyle is optional.
        const auto body = ValidationErrors(
            R"(<a:txBody xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main"><a:bodyPr/></a:txBody>)");
        CHECK_MESSAGE(body.find("Content of element 'a:txBody'") != std::string::npos, body);
        CHECK_MESSAGE(body.find("'a:p'") != std::string::npos, body);
        CHECK(ValidationErrors(
                  R"(<a:txBody xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main"><a:bodyPr/><a:p/></a:txBody>)") == "");
    }

    TEST_CASE("a chart space without c:chart and a plot area without a chart type are incomplete [unit] [dom-required]")
    {
        const auto space = ValidationErrors(
            R"(<c:chartSpace xmlns:c="http://schemas.openxmlformats.org/drawingml/2006/chart"><c:date1904 val="0"/></c:chartSpace>)");
        CHECK_MESSAGE(space.find("Content of element 'c:chartSpace'") != std::string::npos, space);
        CHECK_MESSAGE(space.find("content ends after 1 child element") != std::string::npos, space);

        const auto plotArea = ValidationErrors(
            R"(<c:chartSpace xmlns:c="http://schemas.openxmlformats.org/drawingml/2006/chart">)"
            R"(<c:chart><c:plotArea><c:layout/></c:plotArea></c:chart></c:chartSpace>)");
        CHECK_MESSAGE(plotArea.find("Content of element 'c:plotArea'") != std::string::npos, plotArea);
        CHECK_MESSAGE(plotArea.find("'c:areaChart'") != std::string::npos, plotArea);

        CHECK(ValidationErrors(
                  R"(<c:chartSpace xmlns:c="http://schemas.openxmlformats.org/drawingml/2006/chart">)"
                  R"(<c:chart><c:plotArea><c:pieChart/></c:plotArea></c:chart></c:chartSpace>)") == "");
    }

    TEST_CASE("the reference matcher agrees with the automaton on incomplete content [unit] [dom-required]")
    {
        // The compiled automaton and the recursive matcher read the same particle
        // metadata; cross-checking makes a divergence a reported error rather than
        // a silently different verdict.
        ExyokiOffice::OpenXmlDomValidationSettings settings;
        settings.CrossCheckContentModel = true;
        ExyokiOffice::OpenXmlPackage package;
        REQUIRE(package.LoadFromMemory(ExyokiOfficeTests::DomValidation::BuildSingleXmlPartPackage(
            R"(<p:presentation xmlns:p="http://schemas.openxmlformats.org/presentationml/2006/main"><p:sldIdLst/></p:presentation>)")));
        auto root = package.GetPartByUri("/custom.xml")->GetRootElement();
        REQUIRE(root != nullptr);
        const auto result = ExyokiOffice::OpenXmlDomValidator(settings).Validate(*root);
        bool particleError = false;
        for (const auto& issue : result.Issues())
        {
            CHECK(issue.Id != ExyokiOffice::ValidationErrorId::ContentModelCrossCheckMismatch);
            particleError = particleError || issue.Id == ExyokiOffice::ValidationErrorId::ParticleConstraintViolation;
        }
        CHECK(particleError);
    }

    TEST_CASE("a package loses its clean bill when a required element is removed [unit] [dom-required]")
    {
        // End to end: what the editor writes validates, and the package validator
        // reports the presentation part once p:notesSz is taken out of the DOM.
        namespace Presentation = ExyokiOffice::DocumentFormat::OpenXml::Presentation;
        auto editor = ExyokiOffice::PowerPoint::PowerPointDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);
        auto master = editor->AddSlideMaster("Master");
        auto layout = editor->AddSlideLayout(master, "Blank", Presentation::SlideLayoutValues::Blank);
        REQUIRE(layout != nullptr);
        REQUIRE(editor->AddSlide() != nullptr);
        REQUIRE(editor->SetSlideLayout(0, layout));
        CHECK(PackageErrors(*editor) == "");

        auto presentation = editor->GetDocument()->GetPresentationPart()->GetTypedRootElement();
        auto notesSize = presentation->GetFirstChildOfType<Presentation::NotesSize>();
        REQUIRE(notesSize != nullptr);
        REQUIRE(presentation->RemoveChild(notesSize));

        // Through a save and a reopen, so the check covers what a reader of the
        // package (exyoki validate included) sees rather than only the live DOM.
        auto reopened = ExyokiOffice::PowerPoint::PowerPointDocumentEditor::Open(editor->SaveToMemory());
        REQUIRE(reopened != nullptr);
        const auto errors = PackageErrors(*reopened);
        CHECK_MESSAGE(errors.find("'p:notesSz'") != std::string::npos, errors);
        CHECK_MESSAGE(errors.find("/ppt/presentation.xml") != std::string::npos, errors);
    }
}
