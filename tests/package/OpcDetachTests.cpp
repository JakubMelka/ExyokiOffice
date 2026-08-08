// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "doctest.h"

#include "ExyokiOffice/Packaging/GeneratedParts.hpp"
#include "ExyokiOffice/Packaging/WordprocessingDocument.hpp"
#include "zip/zip.h"
#include "ExyokiOffice/StandardTypes.hpp"

#include <algorithm>
#include <cstdlib>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace
{

constexpr std::string_view kImageRelationship =
    "http://schemas.openxmlformats.org/officeDocument/2006/relationships/image";

void AddZipEntry(zip_t* archive, const char* name, std::string_view content)
{
    REQUIRE(zip_entry_open(archive, name) == 0);
    CHECK(zip_entry_write(archive, content.data(), content.size()) == 0);
    zip_entry_close(archive);
}

std::vector<ExyokiOffice::Byte> FinishZip(zip_t* archive)
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

bool ZipEntryExists(std::span<const ExyokiOffice::Byte> packageBytes, const char* entryName)
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

std::vector<ExyokiOffice::Byte> BuildSharedImagePackage()
{
    auto* archive = zip_stream_open(nullptr, 0, ZIP_DEFAULT_COMPRESSION_LEVEL, 'w');
    REQUIRE(archive != nullptr);

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
  <Relationship Id="rId2" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/image" Target="../media/shared.png"/>
  <Relationship Id="rId3" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/settings" Target="settings.xml"/>
</Relationships>)");
    AddZipEntry(archive,
                "word/_rels/settings.xml.rels",
                R"(<?xml version="1.0" encoding="UTF-8"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">
  <Relationship Id="rId4" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/image" Target="../media/shared.png"/>
</Relationships>)");
    AddZipEntry(archive,
                "word/document.xml",
                R"(<?xml version="1.0" encoding="UTF-8"?>
<w:document xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main"><w:body/></w:document>)");
    AddZipEntry(archive,
                "word/settings.xml",
                R"(<?xml version="1.0" encoding="UTF-8"?>
<w:settings xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main"/>)");
    AddZipEntry(archive, "media/shared.png", "png");
    return FinishZip(archive);
}

std::vector<ExyokiOffice::OpenXmlRelationship> RelationshipsByType(
    const std::vector<ExyokiOffice::OpenXmlRelationship>& relationships,
    std::string_view relationshipType)
{
    std::vector<ExyokiOffice::OpenXmlRelationship> result;
    std::copy_if(relationships.begin(), relationships.end(), std::back_inserter(result), [relationshipType](const auto& relationship)
                 { return relationship.Type == relationshipType; });
    return result;
}

std::string EntryNameForPart(const std::shared_ptr<ExyokiOffice::OpenXmlPackagePart>& part)
{
    REQUIRE(part != nullptr);
    auto uri = part->Uri();
    if (!uri.empty() && uri.front() == '/')
    {
        uri.erase(uri.begin());
    }
    return uri;
}

} // namespace

TEST_SUITE("OpcDetachTests")
{

    TEST_CASE("detaching a child part removes its unreachable descendant parts [opc][detach][gc][subgraph][word] [unit] [opc-detach]")
    {
        ExyokiOffice::Packaging::WordprocessingDocument package;
        auto mainPart = package.AddMainDocumentPart();
        REQUIRE(mainPart != nullptr);
        mainPart->SetContentType("application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml");

        auto settingsPart = mainPart->AddDocumentSettingsPart();
        REQUIRE(settingsPart != nullptr);
        const auto settingsUri = settingsPart->Uri();
        const auto settingsEntry = EntryNameForPart(settingsPart);

        auto imagePart = settingsPart->AddImagePart();
        REQUIRE(imagePart != nullptr);
        imagePart->SetContentType("image/png");
        imagePart->SetBinaryData({'p', 'n', 'g'});
        const auto imageUri = imagePart->Uri();
        const auto imageEntry = EntryNameForPart(imagePart);

        REQUIRE(package.GetPartByUri(settingsUri) == settingsPart);
        REQUIRE(package.GetPartByUri(imageUri) == imagePart);

        CHECK(mainPart->RemoveDocumentSettingsPart());

        CHECK(package.GetPartByUri(settingsUri) == nullptr);
        CHECK(package.GetPartByUri(imageUri) == nullptr);
        CHECK(mainPart->GetDocumentSettingsPart() == nullptr);
        CHECK(RelationshipsByType(mainPart->Relationships(),
                                  "http://schemas.openxmlformats.org/officeDocument/2006/relationships/settings")
                  .empty());

        const auto savedBytes = package.SaveToMemory();
        REQUIRE_FALSE(savedBytes.empty());
        CHECK_FALSE(ZipEntryExists(savedBytes, settingsEntry.c_str()));
        CHECK_FALSE(ZipEntryExists(savedBytes, imageEntry.c_str()));
    }

    TEST_CASE("detaching the root part removes the whole package subgraph [opc][detach][gc][root][word] [unit] [opc-detach]")
    {
        ExyokiOffice::Packaging::WordprocessingDocument package;
        auto mainPart = package.AddMainDocumentPart();
        REQUIRE(mainPart != nullptr);
        mainPart->SetContentType("application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml");
        const auto mainUri = mainPart->Uri();
        const auto mainEntry = EntryNameForPart(mainPart);

        auto settingsPart = mainPart->AddDocumentSettingsPart();
        REQUIRE(settingsPart != nullptr);
        const auto settingsUri = settingsPart->Uri();

        auto imagePart = settingsPart->AddImagePart();
        REQUIRE(imagePart != nullptr);
        imagePart->SetContentType("image/png");
        imagePart->SetBinaryData({'p', 'n', 'g'});
        const auto imageUri = imagePart->Uri();

        CHECK(package.RemoveMainDocumentPart());

        CHECK(package.GetMainDocumentPart() == nullptr);
        CHECK(package.GetPartByUri(mainUri) == nullptr);
        CHECK(package.GetPartByUri(settingsUri) == nullptr);
        CHECK(package.GetPartByUri(imageUri) == nullptr);
        CHECK(package.Relationships().empty());

        const auto savedBytes = package.SaveToMemory();
        REQUIRE_FALSE(savedBytes.empty());
        CHECK_FALSE(ZipEntryExists(savedBytes, mainEntry.c_str()));
    }

    TEST_CASE("detaching one relationship to a shared target keeps the target while another source references it [opc][detach][gc][shared-target][relationships][word] [unit] [opc-detach]")
    {
        ExyokiOffice::Packaging::WordprocessingDocument package;
        REQUIRE(package.LoadFromMemory(BuildSharedImagePackage()));

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

        CHECK(mainPart->RemoveImagePart(sharedImage));

        CHECK(package.GetPartByUri("/media/shared.png") == sharedImage);
        CHECK(mainPart->GetImageParts().empty());
        CHECK(RelationshipsByType(mainPart->Relationships(), kImageRelationship).empty());
        REQUIRE(settingsPart->GetImageParts().size() == 1);
        CHECK(settingsPart->GetImageParts().front() == sharedImage);
        REQUIRE(RelationshipsByType(settingsPart->Relationships(), kImageRelationship).size() == 1);

        const auto savedWithSharedTarget = package.SaveToMemory();
        REQUIRE_FALSE(savedWithSharedTarget.empty());
        CHECK(ZipEntryExists(savedWithSharedTarget, "media/shared.png"));

        CHECK(settingsPart->RemoveImagePart(sharedImage));

        CHECK(package.GetPartByUri("/media/shared.png") == nullptr);
        CHECK(settingsPart->GetImageParts().empty());
        CHECK(RelationshipsByType(settingsPart->Relationships(), kImageRelationship).empty());

        const auto savedWithoutSharedTarget = package.SaveToMemory();
        REQUIRE_FALSE(savedWithoutSharedTarget.empty());
        CHECK_FALSE(ZipEntryExists(savedWithoutSharedTarget, "media/shared.png"));
    }

    TEST_CASE("detaching a missing child part is a no-op and leaves package graph intact [opc][detach][gc][missing-child][word] [unit] [opc-detach]")
    {
        ExyokiOffice::Packaging::WordprocessingDocument package;
        auto mainPart = package.AddMainDocumentPart();
        REQUIRE(mainPart != nullptr);

        auto unattachedImage = std::make_shared<ExyokiOffice::Packaging::ImagePart>();
        CHECK_FALSE(mainPart->RemoveImagePart(unattachedImage));
        CHECK(package.GetMainDocumentPart() == mainPart);
        CHECK(package.GetPartByUri(mainPart->Uri()) == mainPart);
        CHECK(mainPart->Parts().empty());
    }

} // TEST_SUITE("OpcDetachTests")
