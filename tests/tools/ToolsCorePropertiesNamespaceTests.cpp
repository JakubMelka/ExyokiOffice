// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

// The core-properties part binds three namespaces, and the cp:/dc:/dcterms:
// prefixes Office uses for them are a convention, not a rule. These tests pin
// the behaviour for a document that binds the same namespaces to other
// prefixes: reads must still find the values, and writes must go into the
// existing root instead of appending a second one.
//
// The companion checks cover the validator rule that catches such a second
// root, and the explicit report for ISO 29500 Strict packages.

#include "doctest.h"

#include "TestSupport.hpp"

#include "ExyokiOffice/OpenXmlPackage.hpp"
#include "ExyokiOffice/Packaging/DocumentProperties.hpp"
#include "ExyokiOffice/Tools/DocumentRedactor.hpp"
#include "ExyokiOffice/Tools/DocumentTextTools.hpp"
#include "ExyokiOffice/Tools/PackageArchiver.hpp"
#include "ExyokiOffice/Tools/PackageInspector.hpp"
#include "ExyokiOffice/Tools/TextExtractor.hpp"
#include "ExyokiOffice/Tools/ValidationRunner.hpp"
#include "ExyokiOffice/Word/WordDocument.hpp"

#include <algorithm>
#include <fstream>
#include <string>

using namespace ExyokiOffice::Tools;
using ExyokiOffice::OpenXmlPackage;
using ExyokiOffice::Word::WordDocumentEditor;
using ExyokiOfficeTests::MakeTemporaryPath;

namespace
{

/// A core.xml that is semantically identical to what the library writes, but
/// binds every core-property namespace to a different prefix.
constexpr const char* kAlternativePrefixCoreXml =
    "<?xml version=\"1.0\"?>"
    "<coreProps:coreProperties"
    " xmlns:coreProps=\"http://schemas.openxmlformats.org/package/2006/metadata/core-properties\""
    " xmlns:dcx=\"http://purl.org/dc/elements/1.1/\""
    " xmlns:dct=\"http://purl.org/dc/terms/\""
    " xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\">"
    "<dct:created xsi:type=\"dct:W3CDTF\">2026-01-02T03:04:05Z</dct:created>"
    "<dcx:creator>Original Author</dcx:creator>"
    "<dcx:title>Original Title</dcx:title>"
    "</coreProps:coreProperties>";

/// Builds a one-paragraph document whose core.xml uses the alternative prefixes.
std::filesystem::path BuildAlternativePrefixDocument(std::string_view stem)
{
    auto editor = WordDocumentEditor::CreateNew();
    REQUIRE(editor);
    editor->AddParagraph("Body text.");
    const auto path = MakeTemporaryPath(stem, ".docx");
    REQUIRE(editor->SaveToFile(path));

    OpenXmlPackage package;
    REQUIRE(package.LoadFromFile(path));
    // Locate the part by content type, so the fixture does not hard-code a path.
    std::string coreUri;
    for (const auto& record : ListParts(package))
    {
        if (record.ContentType.find("core-properties") != std::string::npos)
        {
            coreUri = record.Uri;
            break;
        }
    }
    REQUIRE_FALSE(coreUri.empty());
    auto corePart = package.GetPartByUri(coreUri);
    REQUIRE(corePart);
    corePart->SetXmlString(kAlternativePrefixCoreXml);
    REQUIRE(package.SaveToFile(path));
    return path;
}

/// Returns the XML of the package's core-properties part.
std::string ReadCorePropertiesXml(const std::filesystem::path& path)
{
    OpenXmlPackage package;
    REQUIRE(package.LoadFromFile(path));
    for (const auto& record : ListParts(package))
    {
        if (record.ContentType.find("core-properties") == std::string::npos)
        {
            continue;
        }
        auto part = package.GetPartByUri(record.Uri);
        REQUIRE(part);
        return part->GetXmlString();
    }
    return {};
}

/**
 * @brief Counts `coreProperties` element openings, whatever prefix they carry.
 *
 * pugixml is a private dependency of the library, so the test cannot parse the
 * part; it scans for element starts instead. A well-formed core-properties part
 * has exactly one opening. Two mean a second root element was appended, which
 * is the corruption this fixture is written to catch.
 */
ExyokiOffice::Size CountCorePropertiesRoots(const std::filesystem::path& path)
{
    const auto xml = ReadCorePropertiesXml(path);

    ExyokiOffice::Size count = 0;
    for (std::string::size_type position = xml.find('<'); position != std::string::npos;
         position = xml.find('<', position + 1))
    {
        auto nameBegin = position + 1;
        if (nameBegin >= xml.size() || xml[nameBegin] == '/' || xml[nameBegin] == '?' ||
            xml[nameBegin] == '!')
        {
            continue;
        }
        auto nameEnd = nameBegin;
        while (nameEnd < xml.size() && xml[nameEnd] != ' ' && xml[nameEnd] != '>' &&
               xml[nameEnd] != '/' && xml[nameEnd] != '\t' && xml[nameEnd] != '\n' &&
               xml[nameEnd] != '\r')
        {
            ++nameEnd;
        }
        const auto qualifiedName = xml.substr(nameBegin, nameEnd - nameBegin);
        const auto colon = qualifiedName.find(':');
        const auto localName =
            colon == std::string::npos ? qualifiedName : qualifiedName.substr(colon + 1);
        if (localName == "coreProperties")
        {
            ++count;
        }
    }
    return count;
}

} // namespace

TEST_CASE("ReadCoreProperties resolves namespaces, not prefixes [unit] [tools]")
{
    const auto path = BuildAlternativePrefixDocument("exyoki_ns_read");

    OpenXmlPackage package;
    REQUIRE(package.LoadFromFile(path));
    const auto properties = ReadCoreProperties(package);
    CHECK(properties.Title == "Original Title");
    CHECK(properties.Creator == "Original Author");
    CHECK(properties.Created == "2026-01-02T03:04:05Z");

    std::filesystem::remove(path);
}

TEST_CASE("WriteCoreProperty updates the existing root instead of adding one [unit] [tools]")
{
    const auto path = BuildAlternativePrefixDocument("exyoki_ns_write");

    {
        OpenXmlPackage package;
        REQUIRE(package.LoadFromFile(path));
        CHECK(WriteCoreProperty(package, "Title", "New Title"));
        REQUIRE(package.SaveToFile(path));
    }

    // One root element: the write reused the document's own prefixes rather
    // than appending a second cp:coreProperties, which is not well-formed XML.
    CHECK(CountCorePropertiesRoots(path) == 1);

    OpenXmlPackage package;
    REQUIRE(package.LoadFromFile(path));
    const auto properties = ReadCoreProperties(package);
    CHECK(properties.Title == "New Title");
    // The untouched properties survive the edit.
    CHECK(properties.Creator == "Original Author");
    CHECK(properties.Created == "2026-01-02T03:04:05Z");

    std::filesystem::remove(path);
}

TEST_CASE("Packaging DocumentProperties agree with the Tools reader [unit] [tools]")
{
    const auto path = BuildAlternativePrefixDocument("exyoki_ns_packaging");

    auto editor = WordDocumentEditor::Open(path);
    REQUIRE(editor);
    auto document = editor->GetDocument();
    REQUIRE(document);

    ExyokiOffice::Packaging::DocumentProperties properties(*document);
    CHECK(properties.GetTitle() == "Original Title");
    CHECK(properties.GetCreator() == "Original Author");

    CHECK(properties.SetCreator("Replacement Author"));
    REQUIRE(editor->SaveToFile(path));
    CHECK(CountCorePropertiesRoots(path) == 1);

    OpenXmlPackage package;
    REQUIRE(package.LoadFromFile(path));
    CHECK(ReadCoreProperties(package).Creator == "Replacement Author");

    std::filesystem::remove(path);
}

TEST_CASE("RedactDocument really clears identity metadata under any prefix [unit] [tools]")
{
    const auto path = BuildAlternativePrefixDocument("exyoki_ns_redact");

    const auto result = RedactDocument(path);
    CHECK(result.Ok);
    CHECK(result.MetadataFieldsCleared > 0);

    // The whole point of redaction: the author name must be gone from the file,
    // not merely absent from a freshly appended second root.
    CHECK(CountCorePropertiesRoots(path) == 1);
    OpenXmlPackage package;
    REQUIRE(package.LoadFromFile(path));
    CHECK(ReadCoreProperties(package).Creator.empty());

    for (const auto& record : ListParts(package))
    {
        if (record.ContentType.find("core-properties") == std::string::npos)
        {
            continue;
        }
        auto part = package.GetPartByUri(record.Uri);
        REQUIRE(part);
        CHECK(part->GetXmlString().find("Original Author") == std::string::npos);
    }

    std::filesystem::remove(path);
}

TEST_CASE("Validation reports an XML part with two root elements [unit] [tools] [validation-runner]")
{
    auto editor = WordDocumentEditor::CreateNew();
    REQUIRE(editor);
    editor->AddParagraph("Body text.");
    const auto path = MakeTemporaryPath("exyoki_wellformed", ".docx");
    REQUIRE(editor->SaveToFile(path));

    {
        OpenXmlPackage package;
        REQUIRE(package.LoadFromFile(path));
        std::string coreUri;
        for (const auto& record : ListParts(package))
        {
            if (record.ContentType.find("core-properties") != std::string::npos)
            {
                coreUri = record.Uri;
                break;
            }
        }
        REQUIRE_FALSE(coreUri.empty());
        auto corePart = package.GetPartByUri(coreUri);
        REQUIRE(corePart);
        corePart->SetXmlString(std::string(kAlternativePrefixCoreXml) +
                               "<cp:coreProperties"
                               " xmlns:cp=\"http://schemas.openxmlformats.org/package/2006/"
                               "metadata/core-properties\"/>");
        REQUIRE(package.SaveToFile(path));
    }

    const auto report = Run(path);
    CHECK(report.Loaded);
    CHECK(report.ErrorCount > 0);
    const auto malformed =
        std::any_of(report.ValidationIssues.begin(), report.ValidationIssues.end(),
                    [](const ExyokiOffice::ValidationIssue& issue)
                    { return issue.Id == ExyokiOffice::ValidationErrorId::OpcMalformedPartXml; });
    CHECK(malformed);

    std::filesystem::remove(path);
}

TEST_CASE("Validation accepts an ordinary single-root package [unit] [tools] [validation-runner]")
{
    auto editor = WordDocumentEditor::CreateNew();
    REQUIRE(editor);
    editor->AddParagraph("Body text.");
    const auto path = MakeTemporaryPath("exyoki_wellformed_ok", ".docx");
    REQUIRE(editor->SaveToFile(path));

    const auto report = Run(path);
    CHECK(report.Loaded);
    const auto malformed =
        std::any_of(report.ValidationIssues.begin(), report.ValidationIssues.end(),
                    [](const ExyokiOffice::ValidationIssue& issue)
                    { return issue.Id == ExyokiOffice::ValidationErrorId::OpcMalformedPartXml; });
    CHECK_FALSE(malformed);

    std::filesystem::remove(path);
}

TEST_CASE("A Strict conformance package is reported as such [unit] [tools]")
{
    auto editor = WordDocumentEditor::CreateNew();
    REQUIRE(editor);
    editor->AddParagraph("Body text.");
    const auto path = MakeTemporaryPath("exyoki_strict", ".docx");
    REQUIRE(editor->SaveToFile(path));

    // Re-declare the root relationship with the ISO 29500 Strict type. The
    // package API offers no way to rewrite a relationship type, so the rels
    // entry is edited in an unpacked tree and packed again.
    const auto treeDirectory = MakeTemporaryPath("exyoki_strict_tree", "");
    {
        REQUIRE(Unpack(path, treeDirectory).Ok);
        const auto relsPath = treeDirectory / "_rels" / ".rels";
        std::string rels;
        {
            std::ifstream input(relsPath, std::ios::binary);
            REQUIRE(input);
            rels.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
        }
        const std::string transitional =
            "http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument";
        const std::string strict =
            "http://purl.oclc.org/ooxml/officeDocument/relationships/officeDocument";
        const auto position = rels.find(transitional);
        REQUIRE(position != std::string::npos);
        rels.replace(position, transitional.size(), strict);
        {
            std::ofstream output(relsPath, std::ios::binary | std::ios::trunc);
            REQUIRE(output);
            output << rels;
        }
        REQUIRE(Pack(treeDirectory, path, PackOptions{6, false, false}).Ok);
    }

    OpenXmlPackage package;
    REQUIRE(package.LoadFromFile(path));
    const auto info = GetInfo(package);
    CHECK(info.IsStrictConformance);
    CHECK(info.Family == DocumentFamily::Unknown);
    CHECK(DescribeUnknownFamily(info).find("Strict") != std::string::npos);

    // The family-aware tools say why, instead of "unrecognized document family".
    const auto extracted = Extract(path);
    CHECK_FALSE(extracted.Ok);
    REQUIRE_FALSE(extracted.Diagnostics.empty());
    CHECK(extracted.Diagnostics.front().Message.find("Strict") != std::string::npos);

    const auto searched = SearchDocumentText(path, "Body");
    CHECK_FALSE(searched.Ok);
    REQUIRE_FALSE(searched.Diagnostics.empty());
    CHECK(searched.Diagnostics.front().Message.find("Strict") != std::string::npos);

    // Validation rejects the package outright. "No errors" would read as "this
    // file is supported" to every caller, which is the one thing it is not.
    const auto report = Run(path);
    CHECK(report.Loaded);
    CHECK(report.ErrorCount > 0);
    const auto reportedAsStrict =
        std::any_of(report.ValidationIssues.begin(), report.ValidationIssues.end(),
                    [](const ExyokiOffice::ValidationIssue& issue)
                    {
                        return issue.Id ==
                                   ExyokiOffice::ValidationErrorId::PackageStrictConformanceUnsupported &&
                               issue.Severity == ExyokiOffice::ValidationSeverity::Error;
                    });
    CHECK(reportedAsStrict);

    // The OPC container itself is class-independent, so the package still loads
    // and the package-level tools still work on it. docs/Compatibility.md
    // promises exactly that, and the promise is worth pinning down.
    CHECK_FALSE(ListParts(package).empty());
    CHECK_FALSE(ListRelationships(package).empty());

    std::filesystem::remove(path);
    std::error_code errorCode;
    std::filesystem::remove_all(treeDirectory, errorCode);
}
