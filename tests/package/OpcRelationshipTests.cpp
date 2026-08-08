// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "doctest.h"

#include "ExyokiOffice/Packaging/GeneratedParts.hpp"
#include "ExyokiOffice/Packaging/WordprocessingDocument.hpp"
#include "OpenXmlPackageUri.hpp"
#include "zip/zip.h"
#include "ExyokiOffice/StandardTypes.hpp"

#include <algorithm>
#include <cstdlib>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace
{
constexpr std::string_view kOfficeDocumentRelationship =
    "http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument";
constexpr std::string_view kSettingsRelationship =
    "http://schemas.openxmlformats.org/officeDocument/2006/relationships/settings";
constexpr std::string_view kWorksheetRelationship =
    "http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet";
constexpr std::string_view kSlideLayoutRelationship =
    "http://schemas.openxmlformats.org/officeDocument/2006/relationships/slideLayout";
constexpr std::string_view kHyperlinkRelationship =
    "http://schemas.openxmlformats.org/officeDocument/2006/relationships/hyperlink";
constexpr std::string_view kAttachedTemplateRelationship =
    "http://schemas.openxmlformats.org/officeDocument/2006/relationships/attachedTemplate";
constexpr std::string_view kPackageMetadataRelationship =
    "http://example.com/relationships/packageMetadata";

void AddZipEntry(zip_t* archive, const char* name, std::string_view content)
{
    REQUIRE(zip_entry_open(archive, name) == 0);
    CHECK(zip_entry_write(archive, content.data(), content.size()) == 0);
    zip_entry_close(archive);
}

std::vector<ExyokiOffice::Byte> BuildMinimalWordPackageWithSettingsRelationship()
{
    auto* archive = zip_stream_open(nullptr, 0, ZIP_DEFAULT_COMPRESSION_LEVEL, 'w');
    REQUIRE(archive != nullptr);

    AddZipEntry(archive,
                "[Content_Types].xml",
                R"(<?xml version="1.0" encoding="UTF-8"?>
<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">
  <Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>
  <Default Extension="xml" ContentType="application/xml"/>
  <Override PartName="/word/document.xml" ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml"/>
  <Override PartName="/word/settings.xml" ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.settings+xml"/>
</Types>)");
    AddZipEntry(archive,
                "_rels/.rels",
                R"(<?xml version="1.0" encoding="UTF-8"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">
  <Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="word/document.xml"/>
</Relationships>)");
    AddZipEntry(archive,
                "word/_rels/document.xml.rels",
                R"(<?xml version="1.0" encoding="UTF-8"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">
  <Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/settings" Target="settings.xml"/>
</Relationships>)");
    AddZipEntry(archive,
                "word/document.xml",
                R"(<?xml version="1.0" encoding="UTF-8"?>
<w:document xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main">
  <w:body/>
</w:document>)");
    AddZipEntry(archive,
                "word/settings.xml",
                R"(<?xml version="1.0" encoding="UTF-8"?>
<w:settings xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main"/>)");

    void* rawBuffer = nullptr;
    ExyokiOffice::Size rawSize = 0;
    REQUIRE(zip_stream_copy(archive, &rawBuffer, &rawSize) > 0);
    zip_stream_close(archive);
    REQUIRE(rawBuffer != nullptr);

    const auto* bytes = static_cast<const ExyokiOffice::UInt8*>(rawBuffer);
    std::vector<ExyokiOffice::Byte> result(bytes, bytes + rawSize);
    std::free(rawBuffer);
    return result;
}

std::vector<ExyokiOffice::Byte> BuildMinimalWordPackageWithExternalRelationships()
{
    auto* archive = zip_stream_open(nullptr, 0, ZIP_DEFAULT_COMPRESSION_LEVEL, 'w');
    REQUIRE(archive != nullptr);

    AddZipEntry(archive,
                "[Content_Types].xml",
                R"(<?xml version="1.0" encoding="UTF-8"?>
<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">
  <Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>
  <Default Extension="xml" ContentType="application/xml"/>
  <Override PartName="/word/document.xml" ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml"/>
  <Override PartName="/word/settings.xml" ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.settings+xml"/>
</Types>)");
    AddZipEntry(archive,
                "_rels/.rels",
                R"(<?xml version="1.0" encoding="UTF-8"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">
  <Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="word/document.xml"/>
  <Relationship Id="rId9" Type="http://example.com/relationships/packageMetadata" Target="https://example.test/package-metadata.json" TargetMode="External"/>
</Relationships>)");
    AddZipEntry(archive,
                "word/_rels/document.xml.rels",
                R"(<?xml version="1.0" encoding="UTF-8"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">
  <Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/hyperlink" Target="https://example.test/report?id=42#section" TargetMode="External"/>
  <Relationship Id="rId2" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/attachedTemplate" Target="../Templates/Base.dotx" TargetMode="External"/>
  <Relationship Id="rId3" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/settings" Target="settings.xml"/>
</Relationships>)");
    AddZipEntry(archive,
                "word/document.xml",
                R"(<?xml version="1.0" encoding="UTF-8"?>
<w:document xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main"
            xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships">
  <w:body>
    <w:p>
      <w:hyperlink r:id="rId1">
        <w:r><w:t>external</w:t></w:r>
      </w:hyperlink>
    </w:p>
  </w:body>
</w:document>)");
    AddZipEntry(archive,
                "word/settings.xml",
                R"(<?xml version="1.0" encoding="UTF-8"?>
<w:settings xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main">
  <w:attachedTemplate r:id="rId2" xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships"/>
</w:settings>)");

    void* rawBuffer = nullptr;
    ExyokiOffice::Size rawSize = 0;
    REQUIRE(zip_stream_copy(archive, &rawBuffer, &rawSize) > 0);
    zip_stream_close(archive);
    REQUIRE(rawBuffer != nullptr);

    const auto* bytes = static_cast<const ExyokiOffice::UInt8*>(rawBuffer);
    std::vector<ExyokiOffice::Byte> result(bytes, bytes + rawSize);
    std::free(rawBuffer);
    return result;
}

std::vector<ExyokiOffice::Byte> BuildPackageWithCustomContentTypes()
{
    auto* archive = zip_stream_open(nullptr, 0, ZIP_DEFAULT_COMPRESSION_LEVEL, 'w');
    REQUIRE(archive != nullptr);

    AddZipEntry(archive,
                "[Content_Types].xml",
                R"(<?xml version="1.0" encoding="UTF-8"?>
<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">
  <Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>
  <Default Extension="xml" ContentType="application/vnd.example.default+xml"/>
  <Default Extension="bin" ContentType="application/vnd.example.binary"/>
  <Default Extension="legacy" ContentType="application/vnd.example.legacy"/>
  <Override PartName="/custom/item1.xml" ContentType="application/vnd.example.item+xml"/>
  <Override PartName="/future/missing.xml" ContentType="application/vnd.example.future+xml"/>
</Types>)");
    AddZipEntry(archive,
                "custom/item1.xml",
                R"(<?xml version="1.0" encoding="UTF-8"?>
<ex:item xmlns:ex="urn:example:item"><ex:value>42</ex:value></ex:item>)");
    AddZipEntry(archive, "custom/from-default.xml", R"(<ex:default xmlns:ex="urn:example:default"/>)");
    AddZipEntry(archive, "media/blob.bin", "opaque-binary-payload");

    void* rawBuffer = nullptr;
    ExyokiOffice::Size rawSize = 0;
    REQUIRE(zip_stream_copy(archive, &rawBuffer, &rawSize) > 0);
    zip_stream_close(archive);
    REQUIRE(rawBuffer != nullptr);

    const auto* bytes = static_cast<const ExyokiOffice::UInt8*>(rawBuffer);
    std::vector<ExyokiOffice::Byte> result(bytes, bytes + rawSize);
    std::free(rawBuffer);
    return result;
}

std::string ReadZipEntry(std::span<const ExyokiOffice::Byte> packageBytes, const char* entryName)
{
    int error = 0;
    auto* archive = zip_stream_openwitherror(reinterpret_cast<const char*>(packageBytes.data()),
                                             packageBytes.size(),
                                             0,
                                             'r',
                                             &error);
    REQUIRE(archive != nullptr);
    REQUIRE(zip_entry_open(archive, entryName) == 0);

    void* rawBuffer = nullptr;
    ExyokiOffice::Size rawSize = 0;
    REQUIRE(zip_entry_read(archive, &rawBuffer, &rawSize) >= 0);
    zip_entry_close(archive);
    zip_stream_close(archive);
    REQUIRE(rawBuffer != nullptr);

    std::string result(static_cast<const char*>(rawBuffer), rawSize);
    std::free(rawBuffer);
    return result;
}

ExyokiOffice::Size CountOccurrences(std::string_view value, std::string_view needle)
{
    ExyokiOffice::Size count = 0;
    ExyokiOffice::Size position = 0;
    while ((position = value.find(needle, position)) != std::string_view::npos)
    {
        ++count;
        position += needle.size();
    }
    return count;
}

std::optional<ExyokiOffice::OpenXmlRelationship> FindRelationshipById(
    const std::vector<ExyokiOffice::OpenXmlRelationship>& relationships,
    std::string_view id)
{
    const auto it = std::find_if(relationships.begin(), relationships.end(), [id](const auto& relationship)
                                 { return relationship.Id == id; });
    if (it == relationships.end())
    {
        return std::nullopt;
    }
    return *it;
}

std::vector<ExyokiOffice::OpenXmlRelationship> FindRelationshipsByExternalMode(
    const std::vector<ExyokiOffice::OpenXmlRelationship>& relationships,
    bool isExternal)
{
    std::vector<ExyokiOffice::OpenXmlRelationship> result;
    std::copy_if(relationships.begin(), relationships.end(), std::back_inserter(result), [isExternal](const auto& rel)
                 { return rel.IsExternal == isExternal; });
    return result;
}
} // namespace

TEST_SUITE("OpcRelationshipTests")
{

    TEST_CASE("part URI normalization is independent from platform path rules [opc][uri][normalization] [unit] [opc-relationship]")
    {
        CHECK(ExyokiOffice::Detail::NormalizePartUri(R"(word\media\..\document.xml)") == "/word/document.xml");
        CHECK(ExyokiOffice::Detail::NormalizePartUri("xl/./worksheets//sheet 1.xml") == "/xl/worksheets/sheet%201.xml");
        CHECK(ExyokiOffice::Detail::NormalizePartUri("/custom/%7e/%2f/item#.xml") == "/custom/%7E/%2F/item%23.xml");
        CHECK(ExyokiOffice::Detail::NormalizePartUri("../../word/document.xml") == "/word/document.xml");
    }

    TEST_CASE("relationship target resolution uses OPC URI directories [opc][uri][relationships] [unit] [opc-relationship]")
    {
        CHECK(ExyokiOffice::Detail::ResolveRelationshipTarget("/word/document.xml", "settings.xml") == "/word/settings.xml");
        CHECK(ExyokiOffice::Detail::ResolveRelationshipTarget("/ppt/slides/slide1.xml",
                                                              "../slideLayouts/slideLayout 1.xml") == "/ppt/slideLayouts/slideLayout%201.xml");
        CHECK(ExyokiOffice::Detail::BuildRelationshipTarget("/ppt/slides",
                                                            "/ppt/slideLayouts/slideLayout 1.xml") == "../slideLayouts/slideLayout%201.xml");
        CHECK(ExyokiOffice::Detail::BuildRelationshipTarget("/", "/word/document.xml") == "word/document.xml");
    }

    TEST_CASE("relationship part entry names are built as package URIs [opc][uri][relationships] [unit] [opc-relationship]")
    {
        CHECK(ExyokiOffice::Detail::RelationshipPartEntryName("/word/document.xml") == "word/_rels/document.xml.rels");
        CHECK(ExyokiOffice::Detail::RelationshipPartEntryName(R"(\ppt\slides\slide 1.xml)") == "ppt/slides/_rels/slide%201.xml.rels");
    }

    TEST_CASE("part relationships are written relative to the source part folder [opc][relationships][word] [unit] [opc-relationship]")
    {
        ExyokiOffice::Packaging::WordprocessingDocument package;
        auto mainPart = package.AddMainDocumentPart();
        REQUIRE(mainPart != nullptr);

        auto settingsPart = mainPart->AddDocumentSettingsPart();
        REQUIRE(settingsPart != nullptr);
        CHECK(settingsPart->Uri() == "/word/settings.xml");

        const auto relationships = mainPart->Relationships();
        REQUIRE(relationships.size() == 1);
        CHECK(relationships.front().Type == kSettingsRelationship);
        CHECK(relationships.front().Target == "settings.xml");
        CHECK_FALSE(relationships.front().IsExternal);
    }

    TEST_CASE("spreadsheet child relationships are written relative to the workbook folder [opc][relationships][spreadsheet] [unit] [opc-relationship]")
    {
        ExyokiOffice::Packaging::SpreadsheetDocument package;
        auto workbookPart = package.AddWorkbookPart();
        REQUIRE(workbookPart != nullptr);

        auto worksheetPart = workbookPart->AddWorksheetPart();
        REQUIRE(worksheetPart != nullptr);
        CHECK(worksheetPart->Uri() == "/xl/worksheets/sheet1.xml");

        const auto relationships = workbookPart->Relationships();
        REQUIRE(relationships.size() == 1);
        CHECK(relationships.front().Type == kWorksheetRelationship);
        CHECK(relationships.front().Target == "worksheets/sheet1.xml");
        CHECK_FALSE(relationships.front().IsExternal);
    }

    TEST_CASE("presentation child relationships use parent directory segments when needed [opc][relationships][presentation] [unit] [opc-relationship]")
    {
        ExyokiOffice::Packaging::PresentationDocument package;
        auto presentationPart = package.AddPresentationPart();
        REQUIRE(presentationPart != nullptr);

        auto slidePart = presentationPart->AddSlidePart();
        REQUIRE(slidePart != nullptr);
        CHECK(slidePart->Uri() == "/ppt/slides/slide1.xml");

        auto slideLayoutPart = slidePart->AddSlideLayoutPart();
        REQUIRE(slideLayoutPart != nullptr);
        CHECK(slideLayoutPart->Uri() == "/ppt/slideLayouts/slideLayout.xml");

        const auto relationships = slidePart->Relationships();
        REQUIRE(relationships.size() == 1);
        CHECK(relationships.front().Type == kSlideLayoutRelationship);
        CHECK(relationships.front().Target == "../slideLayouts/slideLayout.xml");
        CHECK_FALSE(relationships.front().IsExternal);
    }

    TEST_CASE("part relationships are resolved against the source part folder when loading [opc][relationships][load][word] [unit] [opc-relationship]")
    {
        auto packageBytes = BuildMinimalWordPackageWithSettingsRelationship();

        ExyokiOffice::Packaging::WordprocessingDocument package;
        REQUIRE(package.LoadFromMemory(packageBytes));

        auto mainPart = package.GetMainDocumentPart();
        REQUIRE(mainPart != nullptr);
        CHECK(mainPart->Uri() == "/word/document.xml");

        auto settingsPart = mainPart->GetDocumentSettingsPart();
        REQUIRE(settingsPart != nullptr);
        CHECK(settingsPart->Uri() == "/word/settings.xml");

        const auto packageRelationships = package.Relationships();
        REQUIRE(packageRelationships.size() == 1);
        CHECK(packageRelationships.front().Type == kOfficeDocumentRelationship);
        CHECK(packageRelationships.front().Target == "word/document.xml");

        const auto mainRelationships = mainPart->Relationships();
        REQUIRE(mainRelationships.size() == 1);
        CHECK(mainRelationships.front().Type == kSettingsRelationship);
        CHECK(mainRelationships.front().Target == "settings.xml");
    }

    TEST_CASE("external package and part relationships are loaded without creating target parts [opc][relationships][external][load][word][roundtrip] [unit] [opc-relationship]")
    {
        auto packageBytes = BuildMinimalWordPackageWithExternalRelationships();

        ExyokiOffice::Packaging::WordprocessingDocument package;
        REQUIRE(package.LoadFromMemory(packageBytes));

        auto mainPart = package.GetMainDocumentPart();
        REQUIRE(mainPart != nullptr);
        CHECK(package.GetPartByUri("/word/settings.xml") != nullptr);
        CHECK(package.GetPartByUri("/word/https:/example.test/report") == nullptr);
        CHECK(package.GetPartByUri("/Templates/Base.dotx") == nullptr);

        const auto packageRelationships = package.Relationships();
        REQUIRE(packageRelationships.size() == 2);

        auto officeDocument = FindRelationshipById(packageRelationships, "rId1");
        REQUIRE(officeDocument.has_value());
        CHECK(officeDocument->Type == kOfficeDocumentRelationship);
        CHECK(officeDocument->Target == "word/document.xml");
        CHECK_FALSE(officeDocument->IsExternal);

        auto packageExternal = FindRelationshipById(packageRelationships, "rId9");
        REQUIRE(packageExternal.has_value());
        CHECK(packageExternal->Type == kPackageMetadataRelationship);
        CHECK(packageExternal->Target == "https://example.test/package-metadata.json");
        CHECK(packageExternal->IsExternal);

        const auto mainRelationships = mainPart->Relationships();
        REQUIRE(mainRelationships.size() == 3);
        CHECK(FindRelationshipsByExternalMode(mainRelationships, true).size() == 2);
        CHECK(FindRelationshipsByExternalMode(mainRelationships, false).size() == 1);

        auto hyperlink = FindRelationshipById(mainRelationships, "rId1");
        REQUIRE(hyperlink.has_value());
        CHECK(hyperlink->Type == kHyperlinkRelationship);
        CHECK(hyperlink->Target == "https://example.test/report?id=42#section");
        CHECK(hyperlink->IsExternal);

        auto attachedTemplate = FindRelationshipById(mainRelationships, "rId2");
        REQUIRE(attachedTemplate.has_value());
        CHECK(attachedTemplate->Type == kAttachedTemplateRelationship);
        CHECK(attachedTemplate->Target == "../Templates/Base.dotx");
        CHECK(attachedTemplate->IsExternal);

        auto settings = FindRelationshipById(mainRelationships, "rId3");
        REQUIRE(settings.has_value());
        CHECK(settings->Type == kSettingsRelationship);
        CHECK(settings->Target == "settings.xml");
        CHECK_FALSE(settings->IsExternal);
    }

    TEST_CASE("external relationships survive save to memory and reopen [opc][relationships][external][save][roundtrip][word] [unit] [opc-relationship]")
    {
        auto packageBytes = BuildMinimalWordPackageWithExternalRelationships();

        ExyokiOffice::Packaging::WordprocessingDocument package;
        REQUIRE(package.LoadFromMemory(packageBytes));
        auto savedBytes = package.SaveToMemory();
        REQUIRE_FALSE(savedBytes.empty());

        const auto rootRelationshipsXml = ReadZipEntry(savedBytes, "_rels/.rels");
        CHECK(rootRelationshipsXml.find(R"(Id="rId9")") != std::string::npos);
        CHECK(rootRelationshipsXml.find(R"(Type="http://example.com/relationships/packageMetadata")") != std::string::npos);
        CHECK(rootRelationshipsXml.find(R"(Target="https://example.test/package-metadata.json")") != std::string::npos);
        CHECK(rootRelationshipsXml.find(R"(TargetMode="External")") != std::string::npos);

        const auto documentRelationshipsXml = ReadZipEntry(savedBytes, "word/_rels/document.xml.rels");
        CHECK(documentRelationshipsXml.find(R"(Id="rId1")") != std::string::npos);
        CHECK(documentRelationshipsXml.find(R"(Target="https://example.test/report?id=42#section")") != std::string::npos);
        CHECK(documentRelationshipsXml.find(R"(Id="rId2")") != std::string::npos);
        CHECK(documentRelationshipsXml.find(R"(Target="../Templates/Base.dotx")") != std::string::npos);
        CHECK(documentRelationshipsXml.find(R"(Id="rId3")") != std::string::npos);
        CHECK(documentRelationshipsXml.find(R"(Target="settings.xml")") != std::string::npos);
        CHECK(documentRelationshipsXml.find(R"(TargetMode="External")") != std::string::npos);

        ExyokiOffice::Packaging::WordprocessingDocument reopened;
        REQUIRE(reopened.LoadFromMemory(savedBytes));
        auto mainPart = reopened.GetMainDocumentPart();
        REQUIRE(mainPart != nullptr);

        const auto reopenedMainRelationships = mainPart->Relationships();
        auto hyperlink = FindRelationshipById(reopenedMainRelationships, "rId1");
        REQUIRE(hyperlink.has_value());
        CHECK(hyperlink->Type == kHyperlinkRelationship);
        CHECK(hyperlink->Target == "https://example.test/report?id=42#section");
        CHECK(hyperlink->IsExternal);

        auto attachedTemplate = FindRelationshipById(reopenedMainRelationships, "rId2");
        REQUIRE(attachedTemplate.has_value());
        CHECK(attachedTemplate->Type == kAttachedTemplateRelationship);
        CHECK(attachedTemplate->Target == "../Templates/Base.dotx");
        CHECK(attachedTemplate->IsExternal);

        const auto reopenedPackageRelationships = reopened.Relationships();
        auto packageExternal = FindRelationshipById(reopenedPackageRelationships, "rId9");
        REQUIRE(packageExternal.has_value());
        CHECK(packageExternal->Type == kPackageMetadataRelationship);
        CHECK(packageExternal->Target == "https://example.test/package-metadata.json");
        CHECK(packageExternal->IsExternal);
    }

    TEST_CASE("adding external relationships after load does not reuse loaded relationship ids [opc][relationships][external][allocation][word] [unit] [opc-relationship]")
    {
        auto packageBytes = BuildMinimalWordPackageWithExternalRelationships();

        ExyokiOffice::Packaging::WordprocessingDocument package;
        REQUIRE(package.LoadFromMemory(packageBytes));

        auto mainPart = package.GetMainDocumentPart();
        REQUIRE(mainPart != nullptr);

        const auto newMainId =
            mainPart->AddExternalRelationship(std::string(kHyperlinkRelationship), "https://example.test/new-link");
        CHECK(newMainId == "rId4");
        CHECK(mainPart->Relationships().size() == 4);
        CHECK(FindRelationshipById(mainPart->Relationships(), newMainId).has_value());

        const auto newPackageId =
            package.AddExternalRelationship(std::string(kPackageMetadataRelationship), "https://example.test/another.json");
        CHECK(newPackageId == "rId10");
        CHECK(package.Relationships().size() == 3);
        CHECK(FindRelationshipById(package.Relationships(), newPackageId).has_value());
    }

    TEST_CASE("loaded content type manifest defaults and overrides survive save [opc][content-types][manifest][roundtrip][opaque] [unit] [opc-relationship]")
    {
        auto packageBytes = BuildPackageWithCustomContentTypes();

        ExyokiOffice::OpenXmlPackage package;
        REQUIRE(package.LoadFromMemory(packageBytes));

        auto overriddenXmlPart = package.GetPartByUri("/custom/item1.xml");
        REQUIRE(overriddenXmlPart != nullptr);
        CHECK(overriddenXmlPart->ContentType() == "application/vnd.example.item+xml");

        auto defaultXmlPart = package.GetPartByUri("/custom/from-default.xml");
        REQUIRE(defaultXmlPart != nullptr);
        CHECK(defaultXmlPart->ContentType() == "application/vnd.example.default+xml");

        auto defaultBinaryPart = package.GetPartByUri("/media/blob.bin");
        REQUIRE(defaultBinaryPart != nullptr);
        CHECK(defaultBinaryPart->ContentType() == "application/vnd.example.binary");
        CHECK(defaultBinaryPart->GetBinaryData() == std::vector<ExyokiOffice::Byte>({'o', 'p', 'a', 'q', 'u', 'e', '-', 'b', 'i', 'n', 'a', 'r', 'y', '-', 'p', 'a', 'y', 'l', 'o', 'a', 'd'}));

        auto savedBytes = package.SaveToMemory();
        REQUIRE_FALSE(savedBytes.empty());

        const auto contentTypesXml = ReadZipEntry(savedBytes, "[Content_Types].xml");
        CHECK(contentTypesXml.find(R"(Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml")") != std::string::npos);
        CHECK(contentTypesXml.find(R"(Extension="xml" ContentType="application/vnd.example.default+xml")") != std::string::npos);
        CHECK(contentTypesXml.find(R"(Extension="bin" ContentType="application/vnd.example.binary")") != std::string::npos);
        CHECK(contentTypesXml.find(R"(Extension="legacy" ContentType="application/vnd.example.legacy")") != std::string::npos);
        CHECK(contentTypesXml.find(R"(PartName="/custom/item1.xml" ContentType="application/vnd.example.item+xml")") != std::string::npos);
        CHECK(contentTypesXml.find(R"(PartName="/future/missing.xml" ContentType="application/vnd.example.future+xml")") != std::string::npos);
        CHECK(contentTypesXml.find(R"(PartName="/custom/from-default.xml")") == std::string::npos);
        CHECK(contentTypesXml.find(R"(PartName="/media/blob.bin")") == std::string::npos);
        CHECK(CountOccurrences(contentTypesXml, R"(Extension="xml")") == 1);
    }

    TEST_CASE("existing part content type changes update preserved override [opc][content-types][manifest][override][opaque][save] [unit] [opc-relationship]")
    {
        auto packageBytes = BuildPackageWithCustomContentTypes();

        ExyokiOffice::OpenXmlPackage package;
        REQUIRE(package.LoadFromMemory(packageBytes));

        auto overriddenXmlPart = package.GetPartByUri("/custom/item1.xml");
        REQUIRE(overriddenXmlPart != nullptr);
        overriddenXmlPart->SetContentType("application/vnd.example.changed+xml");

        auto savedBytes = package.SaveToMemory();
        REQUIRE_FALSE(savedBytes.empty());

        const auto contentTypesXml = ReadZipEntry(savedBytes, "[Content_Types].xml");
        CHECK(contentTypesXml.find(R"(PartName="/custom/item1.xml" ContentType="application/vnd.example.changed+xml")") != std::string::npos);
        CHECK(contentTypesXml.find(R"(PartName="/custom/item1.xml" ContentType="application/vnd.example.item+xml")") == std::string::npos);
        CHECK(contentTypesXml.find(R"(Extension="xml" ContentType="application/vnd.example.default+xml")") != std::string::npos);
    }

    TEST_CASE("new packages still write required defaults and part overrides [opc][content-types][manifest][create][word] [unit] [opc-relationship]")
    {
        ExyokiOffice::Packaging::WordprocessingDocument package;
        auto mainPart = package.AddMainDocumentPart();
        REQUIRE(mainPart != nullptr);
        mainPart->SetContentType("application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml");

        auto savedBytes = package.SaveToMemory();
        REQUIRE_FALSE(savedBytes.empty());

        const auto contentTypesXml = ReadZipEntry(savedBytes, "[Content_Types].xml");
        CHECK(contentTypesXml.find(R"(Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml")") != std::string::npos);
        CHECK(contentTypesXml.find(R"(Extension="xml" ContentType="application/xml")") != std::string::npos);
        CHECK(contentTypesXml.find(R"(PartName="/word/document.xml" ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml")") != std::string::npos);
    }

} // TEST_SUITE("OpcRelationshipTests")
