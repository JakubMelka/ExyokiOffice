// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "doctest.h"

#include "ExyokiOffice/Packaging/GeneratedParts.hpp"
#include "ExyokiOffice/Packaging/PowerPointDocument.hpp"
#include "ExyokiOffice/Packaging/SpreadsheetDocument.hpp"
#include "ExyokiOffice/Packaging/WordprocessingDocument.hpp"

#include "OpenXmlPackageUri.hpp"

#include <string>

namespace
{
using ExyokiOffice::Detail::CombineChildPartFolder;
} // namespace

TEST_SUITE("Child part folders")
{

    TEST_CASE("A repeated folder name is not nested twice [unit] [package] [uri]")
    {
        // The descriptor of a part names the folder relative to whatever parent
        // it is attached to, and a few descriptors repeat the folder their only
        // sensible parent already sits in.
        CHECK(CombineChildPartFolder("/_xmlsignatures", "_xmlsignatures") == "/_xmlsignatures");
        CHECK(CombineChildPartFolder("/ppt/slides", "slides") == "/ppt/slides");
        CHECK(CombineChildPartFolder("/customData", "customData") == "/customData");
    }

    TEST_CASE("Ordinary paths are still combined [unit] [package] [uri]")
    {
        CHECK(CombineChildPartFolder("/", "_xmlsignatures") == "/_xmlsignatures");
        CHECK(CombineChildPartFolder("/ppt", "slides") == "/ppt/slides");
        CHECK(CombineChildPartFolder("/xl", "worksheets") == "/xl/worksheets");
        CHECK(CombineChildPartFolder("/word", ".") == "/word");
    }

    TEST_CASE("Navigating paths keep their meaning [unit] [package] [uri]")
    {
        // "../media" has to climb out of the parent's folder even when that
        // folder happens to be called media.
        CHECK(CombineChildPartFolder("/word/media", "../media") == "/word/media");
        CHECK(CombineChildPartFolder("/xl/worksheets", "../threadedcomments") == "/xl/threadedcomments");
        CHECK(CombineChildPartFolder("/ppt/slides", "../media") == "/ppt/media");
    }

    TEST_CASE("A part with an absolute family folder ignores its parent [unit] [package] [uri]")
    {
        // Images, themes and charts live in one place per document family
        // whatever they hang off, so their descriptors carry an absolute path.
        CHECK(CombineChildPartFolder("/word", "/word/media") == "/word/media");
        CHECK(CombineChildPartFolder("/ppt/slides", "/ppt/media") == "/ppt/media");
        CHECK(CombineChildPartFolder("/ppt/slideMasters", "/ppt/theme") == "/ppt/theme");
        CHECK(CombineChildPartFolder("/xl/worksheets", "/xl/charts") == "/xl/charts");
    }

    TEST_CASE("Parts Office keeps in one place follow the document family [unit] [package] [uri]")
    {
        auto word = ExyokiOffice::Packaging::WordDocument::Create(
            ExyokiOffice::Packaging::WordprocessingDocumentType::Document);
        REQUIRE(word);
        REQUIRE(word->InitDocument());
        auto mainPart = word->GetMainDocumentPart();
        REQUIRE(mainPart);
        auto wordImage = mainPart->AddImagePart();
        REQUIRE(wordImage);
        CHECK(wordImage->Uri() == "/word/media/image1.bin");

        // A part attached with its content type already set is named after it.
        // Names are unique per URI, so the .png does not collide with the .bin.
        auto png = std::make_shared<ExyokiOffice::Packaging::ImagePart>();
        png->SetContentType("image/png");
        REQUIRE(mainPart->AddImagePart(png));
        CHECK(png->Uri() == "/word/media/image1.png");
        auto wordTheme = mainPart->AddThemePart();
        REQUIRE(wordTheme);
        CHECK(wordTheme->Uri() == "/word/theme/theme.xml");

        // The custom XML store stays at the package root, where Word puts it.
        // Its payload type is only known to the caller, so the name keeps the
        // `.bin` placeholder until one is supplied.
        auto customXml = mainPart->AddCustomXmlPart();
        REQUIRE(customXml);
        CHECK(customXml->Uri() == "/customXml/item1.bin");

        auto powerPoint = ExyokiOffice::Packaging::PowerPointDocument::Create(
            ExyokiOffice::Packaging::PowerPointDocumentType::Presentation);
        REQUIRE(powerPoint);
        REQUIRE(powerPoint->InitDocument());
        auto presentationPart = powerPoint->GetPresentationPart();
        REQUIRE(presentationPart);
        auto master = presentationPart->AddSlideMasterPart();
        REQUIRE(master);
        auto masterTheme = master->AddThemePart();
        REQUIRE(masterTheme);
        CHECK(masterTheme->Uri() == "/ppt/theme/theme.xml");
        auto slide = presentationPart->AddSlidePart();
        REQUIRE(slide);
        auto slideImage = slide->AddImagePart();
        REQUIRE(slideImage);
        CHECK(slideImage->Uri() == "/ppt/media/image1.bin");
    }

    TEST_CASE("A partial name match still nests [unit] [package] [uri]")
    {
        // Only a whole segment counts; "slide" is not "slides".
        CHECK(CombineChildPartFolder("/ppt/slide", "slides") == "/ppt/slide/slides");
        CHECK(CombineChildPartFolder("/ppt/slidesx", "slides") == "/ppt/slidesx/slides");
    }

    TEST_CASE("A signature part lands next to the origin part [unit] [package] [uri]")
    {
        auto document = ExyokiOffice::Packaging::WordDocument::Create(
            ExyokiOffice::Packaging::WordprocessingDocumentType::Document);
        REQUIRE(document);

        auto origin = document->AddDigitalSignatureOriginPart();
        REQUIRE(origin);
        CHECK(origin->Uri() == "/_xmlsignatures/origin.sigs");

        auto signature = origin->AddXmlSignaturePart();
        REQUIRE(signature);
        CHECK(signature->Uri() == "/_xmlsignatures/sig1.xml");

        auto second = origin->AddXmlSignaturePart();
        REQUIRE(second);
        CHECK(second->Uri() == "/_xmlsignatures/sig2.xml");
    }

    TEST_CASE("A part that moves with the application follows the family [unit] [package] [uri]")
    {
        // The task panes part is the one part whose folder depends on which
        // application owns the package.
        const auto& descriptor = ExyokiOffice::Packaging::WebExTaskpanesPart::Descriptor();
        CHECK(descriptor.PathFor(ExyokiOffice::OpenXmlDocumentFamily::Word) == "word/webextensions");
        CHECK(descriptor.PathFor(ExyokiOffice::OpenXmlDocumentFamily::Excel) == "xl/webextensions");
        CHECK(descriptor.PathFor(ExyokiOffice::OpenXmlDocumentFamily::PowerPoint) == "ppt/webextensions");
        CHECK(descriptor.PathFor(ExyokiOffice::OpenXmlDocumentFamily::Unknown) == descriptor.DefaultPath);

        auto word = ExyokiOffice::Packaging::WordDocument::Create(
            ExyokiOffice::Packaging::WordprocessingDocumentType::Document);
        REQUIRE(word);
        CHECK(word->DocumentFamily() == ExyokiOffice::OpenXmlDocumentFamily::Word);
        auto wordTaskpanes = word->AddWebExTaskpanesPart();
        REQUIRE(wordTaskpanes);
        CHECK(wordTaskpanes->Uri() == "/word/webextensions/taskpanes.xml");

        auto excel = ExyokiOffice::Packaging::ExcelDocument::Create(
            ExyokiOffice::Packaging::SpreadsheetDocumentType::Workbook);
        REQUIRE(excel);
        CHECK(excel->DocumentFamily() == ExyokiOffice::OpenXmlDocumentFamily::Excel);
        auto excelTaskpanes = excel->AddWebExTaskpanesPart();
        REQUIRE(excelTaskpanes);
        CHECK(excelTaskpanes->Uri() == "/xl/webextensions/taskpanes.xml");

        auto powerPoint = ExyokiOffice::Packaging::PowerPointDocument::Create(
            ExyokiOffice::Packaging::PowerPointDocumentType::Presentation);
        REQUIRE(powerPoint);
        CHECK(powerPoint->DocumentFamily() == ExyokiOffice::OpenXmlDocumentFamily::PowerPoint);
        auto powerPointTaskpanes = powerPoint->AddWebExTaskpanesPart();
        REQUIRE(powerPointTaskpanes);
        CHECK(powerPointTaskpanes->Uri() == "/ppt/webextensions/taskpanes.xml");
    }

    TEST_CASE("A part with no family folder is unaffected [unit] [package] [uri]")
    {
        // Nearly every part has one folder whatever package it belongs to.
        const auto& descriptor = ExyokiOffice::Packaging::XmlSignaturePart::Descriptor();
        CHECK(descriptor.PathFor(ExyokiOffice::OpenXmlDocumentFamily::Word) == descriptor.DefaultPath);
        CHECK(descriptor.PathFor(ExyokiOffice::OpenXmlDocumentFamily::Excel) == descriptor.DefaultPath);
        CHECK(descriptor.PathFor(ExyokiOffice::OpenXmlDocumentFamily::PowerPoint) == descriptor.DefaultPath);
    }

    TEST_CASE("An attached part keeps every descriptor field [unit] [package] [uri]")
    {
        // A part copies the descriptor it was created from, because the source
        // may be a temporary; the copy must not lose fields.
        auto word = ExyokiOffice::Packaging::WordDocument::Create(
            ExyokiOffice::Packaging::WordprocessingDocumentType::Document);
        REQUIRE(word);

        auto taskpanes = word->AddWebExTaskpanesPart();
        REQUIRE(taskpanes);

        const auto& stored = taskpanes->Descriptor();
        const auto& original = ExyokiOffice::Packaging::WebExTaskpanesPart::Descriptor();
        CHECK(stored.Name == original.Name);
        CHECK(stored.DefaultPath == original.DefaultPath);
        CHECK(stored.WordPath == original.WordPath);
        CHECK(stored.ExcelPath == original.ExcelPath);
        CHECK(stored.PowerPointPath == original.PowerPointPath);
        CHECK(stored.PathFor(ExyokiOffice::OpenXmlDocumentFamily::Excel) == "xl/webextensions");
    }

    TEST_CASE("A plain package reports no family [unit] [package] [uri]")
    {
        ExyokiOffice::OpenXmlPackage package;
        CHECK(package.DocumentFamily() == ExyokiOffice::OpenXmlDocumentFamily::Unknown);
    }

    TEST_CASE("A slide added to a slide stays in the slide folder [unit] [package] [uri]")
    {
        auto document = ExyokiOffice::Packaging::PowerPointDocument::Create(
            ExyokiOffice::Packaging::PowerPointDocumentType::Presentation);
        REQUIRE(document);

        // The package class deliberately creates no slide structure, so the
        // presentation part is added here.
        auto presentation = document->AddPresentationPart();
        REQUIRE(presentation);

        auto slide = presentation->AddSlidePart();
        REQUIRE(slide);
        CHECK(slide->Uri() == "/ppt/slides/slide1.xml");

        auto nested = slide->AddSlidePart();
        REQUIRE(nested);
        CHECK(nested->Uri() == "/ppt/slides/slide2.xml");
    }

} // TEST_SUITE
