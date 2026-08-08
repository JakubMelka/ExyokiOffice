// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "doctest.h"

#include "ExyokiOffice/OpenXmlPackage.hpp"
#include "ExyokiOffice/OpenXmlPackageValidator.hpp"
#include "ExyokiOffice/Packaging/WordprocessingDocument.hpp"
#include "zip/zip.h"
#include "ExyokiOffice/StandardTypes.hpp"

#include <algorithm>
#include <cstdlib>
#include <span>
#include <string>
#include <string_view>
#include <vector>

static constexpr std::string_view kImageRelationship =
    "http://schemas.openxmlformats.org/officeDocument/2006/relationships/image";

static void AddZipEntry(zip_t* archive, const char* name, std::string_view content)
{
    REQUIRE(zip_entry_open(archive, name) == 0);
    CHECK(zip_entry_write(archive, content.data(), content.size()) == 0);
    zip_entry_close(archive);
}

static std::vector<ExyokiOffice::Byte> FinishZip(zip_t* archive)
{
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

static std::string ReadZipEntry(std::span<const ExyokiOffice::Byte> packageBytes, const char* entryName)
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

    const auto* bytes = static_cast<const char*>(rawBuffer);
    std::string result(bytes, bytes + rawSize);
    std::free(rawBuffer);
    return result;
}

static bool ZipEntryExists(std::span<const ExyokiOffice::Byte> packageBytes, const char* entryName)
{
    int error = 0;
    auto* archive = zip_stream_openwitherror(reinterpret_cast<const char*>(packageBytes.data()),
                                             packageBytes.size(),
                                             0,
                                             'r',
                                             &error);
    REQUIRE(archive != nullptr);
    const bool exists = zip_entry_open(archive, entryName) == 0;
    if (exists)
    {
        zip_entry_close(archive);
    }
    zip_stream_close(archive);
    return exists;
}

static void AddSharedTargetContentTypes(zip_t* archive)
{
    AddZipEntry(archive,
                "[Content_Types].xml",
                R"(<?xml version="1.0" encoding="UTF-8"?>
<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">
  <Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>
  <Default Extension="xml" ContentType="application/xml"/>
  <Default Extension="png" ContentType="image/png"/>
  <Override PartName="/word/document.xml" ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml"/>
  <Override PartName="/word/settings.xml" ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.settings+xml"/>
</Types>)");
}

static void AddSharedTargetWordParts(zip_t* archive)
{
    AddZipEntry(archive,
                "word/document.xml",
                R"(<?xml version="1.0" encoding="UTF-8"?>
<w:document xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main"><w:body/></w:document>)");
    AddZipEntry(archive,
                "word/settings.xml",
                R"(<?xml version="1.0" encoding="UTF-8"?>
<w:settings xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main"/>)");
    AddZipEntry(archive, "media/shared.png", "png");
}

static std::vector<ExyokiOffice::Byte> BuildWordPackageWithSharedImage(std::string_view documentImageId = "rId2",
                                                                       std::string_view settingsImageId = "rId4")
{
    auto* archive = zip_stream_open(nullptr, 0, ZIP_DEFAULT_COMPRESSION_LEVEL, 'w');
    REQUIRE(archive != nullptr);

    AddSharedTargetContentTypes(archive);
    AddZipEntry(archive,
                "_rels/.rels",
                R"(<?xml version="1.0" encoding="UTF-8"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">
  <Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="word/document.xml"/>
</Relationships>)");

    const auto documentRelationships =
        std::string(R"(<?xml version="1.0" encoding="UTF-8"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">
  <Relationship Id=")") +
        std::string(documentImageId) + R"(" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/image" Target="../media/shared.png"/>
  <Relationship Id="rId3" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/settings" Target="settings.xml"/>
</Relationships>)";
    AddZipEntry(archive, "word/_rels/document.xml.rels", documentRelationships);

    const auto settingsRelationships =
        std::string(R"(<?xml version="1.0" encoding="UTF-8"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">
  <Relationship Id=")") +
        std::string(settingsImageId) + R"(" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/image" Target="../media/shared.png"/>
</Relationships>)";
    AddZipEntry(archive, "word/_rels/settings.xml.rels", settingsRelationships);

    AddSharedTargetWordParts(archive);
    return FinishZip(archive);
}

static std::vector<ExyokiOffice::Byte> BuildPackageAndPartRelationshipsToSameTarget()
{
    auto* archive = zip_stream_open(nullptr, 0, ZIP_DEFAULT_COMPRESSION_LEVEL, 'w');
    REQUIRE(archive != nullptr);

    AddSharedTargetContentTypes(archive);
    AddZipEntry(archive,
                "_rels/.rels",
                R"(<?xml version="1.0" encoding="UTF-8"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">
  <Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="word/document.xml"/>
  <Relationship Id="rId2" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/image" Target="media/shared.png"/>
</Relationships>)");
    AddZipEntry(archive,
                "word/_rels/document.xml.rels",
                R"(<?xml version="1.0" encoding="UTF-8"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">
  <Relationship Id="rId7" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/image" Target="../media/shared.png"/>
</Relationships>)");

    AddSharedTargetWordParts(archive);
    return FinishZip(archive);
}

static std::vector<ExyokiOffice::OpenXmlIncomingRelationship> IncomingFrom(
    const std::shared_ptr<ExyokiOffice::OpenXmlPackagePart>& part,
    std::string_view sourceUri)
{
    std::vector<ExyokiOffice::OpenXmlIncomingRelationship> result;
    const auto& incoming = part->IncomingRelationships();
    std::copy_if(incoming.begin(), incoming.end(), std::back_inserter(result), [sourceUri](const auto& edge)
                 { return edge.SourceUri == sourceUri; });
    return result;
}

TEST_SUITE("OpcRelationshipGraphTests")
{

    TEST_CASE("shared target records independent incoming relationship edges [opc][relationship-graph][shared-target][incoming-edges][word] [unit] [opc-relationship-graph]")
    {
        ExyokiOffice::Packaging::WordprocessingDocument package;
        REQUIRE(package.LoadFromMemory(BuildWordPackageWithSharedImage()));

        auto mainPart = package.GetMainDocumentPart();
        REQUIRE(mainPart != nullptr);
        auto settingsPart = mainPart->GetDocumentSettingsPart();
        REQUIRE(settingsPart != nullptr);

        auto mainImages = mainPart->GetImageParts();
        auto settingsImages = settingsPart->GetImageParts();
        REQUIRE(mainImages.size() == 1);
        REQUIRE(settingsImages.size() == 1);
        CHECK(mainImages.front() == settingsImages.front());

        const auto sharedImage = mainImages.front();
        REQUIRE(sharedImage != nullptr);
        CHECK(sharedImage->Uri() == "/media/shared.png");
        CHECK(sharedImage->RelationshipId().empty());

        const auto& incoming = sharedImage->IncomingRelationships();
        REQUIRE(incoming.size() == 2);

        const auto fromDocument = IncomingFrom(sharedImage, "/word/document.xml");
        const auto fromSettings = IncomingFrom(sharedImage, "/word/settings.xml");
        REQUIRE(fromDocument.size() == 1);
        REQUIRE(fromSettings.size() == 1);
        CHECK(fromDocument.front().Id == "rId2");
        CHECK(fromDocument.front().Type == kImageRelationship);
        CHECK(fromDocument.front().Target == "../media/shared.png");
        CHECK_FALSE(fromDocument.front().IsExternal);
        CHECK(fromSettings.front().Id == "rId4");
        CHECK(fromSettings.front().Type == kImageRelationship);
        CHECK(fromSettings.front().Target == "../media/shared.png");
        CHECK_FALSE(fromSettings.front().IsExternal);
    }

    TEST_CASE("shared target relationship graph survives save and reload [opc][relationship-graph][shared-target][roundtrip][relationships][word] [unit] [opc-relationship-graph]")
    {
        ExyokiOffice::Packaging::WordprocessingDocument package;
        REQUIRE(package.LoadFromMemory(BuildWordPackageWithSharedImage()));

        const auto savedBytes = package.SaveToMemory();
        REQUIRE_FALSE(savedBytes.empty());
        CHECK(ZipEntryExists(savedBytes, "media/shared.png"));

        const auto documentRels = ReadZipEntry(savedBytes, "word/_rels/document.xml.rels");
        const auto settingsRels = ReadZipEntry(savedBytes, "word/_rels/settings.xml.rels");
        CHECK(documentRels.find(R"(Id="rId2")") != std::string::npos);
        CHECK(documentRels.find(R"(Target="../media/shared.png")") != std::string::npos);
        CHECK(settingsRels.find(R"(Id="rId4")") != std::string::npos);
        CHECK(settingsRels.find(R"(Target="../media/shared.png")") != std::string::npos);

        ExyokiOffice::Packaging::WordprocessingDocument reopened;
        REQUIRE(reopened.LoadFromMemory(savedBytes));
        auto reopenedMain = reopened.GetMainDocumentPart();
        REQUIRE(reopenedMain != nullptr);
        auto reopenedSettings = reopenedMain->GetDocumentSettingsPart();
        REQUIRE(reopenedSettings != nullptr);
        REQUIRE(reopenedMain->GetImageParts().size() == 1);
        REQUIRE(reopenedSettings->GetImageParts().size() == 1);
        CHECK(reopenedMain->GetImageParts().front() == reopenedSettings->GetImageParts().front());
        CHECK(reopenedMain->GetImageParts().front()->IncomingRelationships().size() == 2);
    }

    TEST_CASE("detaching one source edge updates incoming graph without deleting shared target [opc][relationship-graph][detach][shared-target][gc][word] [unit] [opc-relationship-graph]")
    {
        ExyokiOffice::Packaging::WordprocessingDocument package;
        REQUIRE(package.LoadFromMemory(BuildWordPackageWithSharedImage()));

        auto mainPart = package.GetMainDocumentPart();
        REQUIRE(mainPart != nullptr);
        auto settingsPart = mainPart->GetDocumentSettingsPart();
        REQUIRE(settingsPart != nullptr);
        auto sharedImage = mainPart->GetImageParts().front();
        REQUIRE(sharedImage != nullptr);

        CHECK(mainPart->RemoveImagePart(sharedImage));
        CHECK(package.GetPartByUri("/media/shared.png") == sharedImage);
        REQUIRE(sharedImage->IncomingRelationships().size() == 1);
        CHECK(sharedImage->RelationshipId() == "rId4");
        CHECK(IncomingFrom(sharedImage, "/word/document.xml").empty());
        REQUIRE(IncomingFrom(sharedImage, "/word/settings.xml").size() == 1);
        CHECK(settingsPart->GetImageParts().front() == sharedImage);

        CHECK(settingsPart->RemoveImagePart(sharedImage));
        CHECK(package.GetPartByUri("/media/shared.png") == nullptr);

        const auto savedBytes = package.SaveToMemory();
        REQUIRE_FALSE(savedBytes.empty());
        CHECK_FALSE(ZipEntryExists(savedBytes, "media/shared.png"));
    }

    TEST_CASE("same relationship id in different source containers can target one shared part [opc][relationship-graph][shared-target][duplicate-id][valid][validation][word] [unit] [opc-relationship-graph]")
    {
        ExyokiOffice::Packaging::WordprocessingDocument package;
        REQUIRE(package.LoadFromMemory(BuildWordPackageWithSharedImage("rId9", "rId9")));

        auto mainPart = package.GetMainDocumentPart();
        REQUIRE(mainPart != nullptr);
        auto settingsPart = mainPart->GetDocumentSettingsPart();
        REQUIRE(settingsPart != nullptr);
        REQUIRE(mainPart->GetImageParts().size() == 1);
        REQUIRE(settingsPart->GetImageParts().size() == 1);

        const auto sharedImage = mainPart->GetImageParts().front();
        REQUIRE(sharedImage != nullptr);
        CHECK(sharedImage == settingsPart->GetImageParts().front());
        CHECK(sharedImage->RelationshipId().empty());
        REQUIRE(sharedImage->IncomingRelationships().size() == 2);
        CHECK(IncomingFrom(sharedImage, "/word/document.xml").front().Id == "rId9");
        CHECK(IncomingFrom(sharedImage, "/word/settings.xml").front().Id == "rId9");

        const auto result = ExyokiOffice::OpenXmlPackageValidator().Validate(package);
        CHECK(result.IsValid());
    }

    TEST_CASE("package root and part relationship containers can both target the same part [opc][relationship-graph][package-root][shared-target][incoming-edges] [unit] [opc-relationship-graph]")
    {
        ExyokiOffice::OpenXmlPackage package;
        REQUIRE(package.LoadFromMemory(BuildPackageAndPartRelationshipsToSameTarget()));

        auto document = package.GetPartByUri("/word/document.xml");
        auto sharedImage = package.GetPartByUri("/media/shared.png");
        REQUIRE(document != nullptr);
        REQUIRE(sharedImage != nullptr);

        REQUIRE(package.Parts().size() == 2);
        CHECK(std::find(package.Parts().begin(), package.Parts().end(), document) != package.Parts().end());
        CHECK(std::find(package.Parts().begin(), package.Parts().end(), sharedImage) != package.Parts().end());
        REQUIRE(document->Parts().size() == 1);
        CHECK(document->Parts().front() == sharedImage);

        CHECK(sharedImage->RelationshipId().empty());
        REQUIRE(sharedImage->IncomingRelationships().size() == 2);
        const auto fromPackage = IncomingFrom(sharedImage, "/");
        const auto fromDocument = IncomingFrom(sharedImage, "/word/document.xml");
        REQUIRE(fromPackage.size() == 1);
        REQUIRE(fromDocument.size() == 1);
        CHECK(fromPackage.front().Id == "rId2");
        CHECK(fromPackage.front().Target == "media/shared.png");
        CHECK(fromDocument.front().Id == "rId7");
        CHECK(fromDocument.front().Target == "../media/shared.png");

        const auto savedBytes = package.SaveToMemory();
        REQUIRE_FALSE(savedBytes.empty());
        CHECK(ReadZipEntry(savedBytes, "_rels/.rels").find(R"(Id="rId2")") != std::string::npos);
        CHECK(ReadZipEntry(savedBytes, "word/_rels/document.xml.rels").find(R"(Id="rId7")") != std::string::npos);
    }

} // TEST_SUITE("OpcRelationshipGraphTests")
