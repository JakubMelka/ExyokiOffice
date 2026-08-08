// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "doctest.h"

#include "ExyokiOffice/OpenXmlPackage.hpp"
#include "ExyokiOffice/OpenXmlPackagePart.hpp"
#include "ExyokiOffice/StandardTypes.hpp"
#include "zip/zip.h"

#include <cstdlib>
#include <memory>
#include <string_view>
#include <vector>

// The Open XML relationship graph is not a tree. A slide layout points back at
// its slide master, a notes slide back at the slide it annotates, and every
// relationship edge is an owning shared_ptr. Such a component cannot free
// itself, so a package that stops tracking its parts has to cut the edges. The
// packages built here reproduce the shape with two parts pointing at each other,
// which is all the ownership layer needs to see.

namespace OpcPartLifetime
{
/// Detaching a root part is how a whole component becomes unreachable, and the
/// operation is protected on the package.
class DetachablePackage : public ExyokiOffice::OpenXmlPackage
{
public:
    using ExyokiOffice::OpenXmlPackage::DetachRootPart;
};
} // namespace OpcPartLifetime

static void AddLifetimeZipEntry(zip_t* archive, const char* name, std::string_view content)
{
    REQUIRE(zip_entry_open(archive, name) == 0);
    CHECK(zip_entry_write(archive, content.data(), content.size()) == 0);
    zip_entry_close(archive);
}

static std::vector<ExyokiOffice::Byte> BuildPackageWithCyclicRelationships()
{
    auto* archive = zip_stream_open(nullptr, 0, ZIP_DEFAULT_COMPRESSION_LEVEL, 'w');
    REQUIRE(archive != nullptr);

    AddLifetimeZipEntry(archive,
                        "[Content_Types].xml",
                        R"(<?xml version="1.0" encoding="UTF-8"?>
<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">
  <Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>
  <Default Extension="xml" ContentType="application/xml"/>
  <Override PartName="/word/document.xml" ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml"/>
  <Override PartName="/word/settings.xml" ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.settings+xml"/>
</Types>)");

    AddLifetimeZipEntry(archive,
                        "_rels/.rels",
                        R"(<?xml version="1.0" encoding="UTF-8"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">
  <Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="word/document.xml"/>
</Relationships>)");

    AddLifetimeZipEntry(archive,
                        "word/_rels/document.xml.rels",
                        R"(<?xml version="1.0" encoding="UTF-8"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">
  <Relationship Id="rId2" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/settings" Target="settings.xml"/>
</Relationships>)");

    // The edge that closes the cycle.
    AddLifetimeZipEntry(archive,
                        "word/_rels/settings.xml.rels",
                        R"(<?xml version="1.0" encoding="UTF-8"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">
  <Relationship Id="rId3" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="document.xml"/>
</Relationships>)");

    AddLifetimeZipEntry(archive,
                        "word/document.xml",
                        R"(<?xml version="1.0" encoding="UTF-8"?>
<w:document xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main"><w:body/></w:document>)");
    AddLifetimeZipEntry(archive,
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

/// Confirms the loaded package really contains the cycle the test is about, so
/// that an expired weak_ptr later means the cycle was cut and not that the
/// package never had one.
static void RequireCycle(const std::shared_ptr<ExyokiOffice::OpenXmlPackagePart>& documentPart,
                         const std::shared_ptr<ExyokiOffice::OpenXmlPackagePart>& settingsPart)
{
    REQUIRE(documentPart != nullptr);
    REQUIRE(settingsPart != nullptr);

    const auto& documentChildren = documentPart->Parts();
    REQUIRE(documentChildren.size() == 1);
    REQUIRE(documentChildren.front() == settingsPart);

    const auto& settingsChildren = settingsPart->Parts();
    REQUIRE(settingsChildren.size() == 1);
    REQUIRE(settingsChildren.front() == documentPart);
}

TEST_SUITE("OpcPartLifetimeTests")
{

    TEST_CASE("cyclic relationships do not outlive the package [opc][lifetime][cycle][leak] [unit] [opc-part-lifetime]")
    {
        std::weak_ptr<ExyokiOffice::OpenXmlPackagePart> documentWatch;
        std::weak_ptr<ExyokiOffice::OpenXmlPackagePart> settingsWatch;

        {
            ExyokiOffice::OpenXmlPackage package;
            REQUIRE(package.LoadFromMemory(BuildPackageWithCyclicRelationships()));

            auto documentPart = package.GetPartByUri("/word/document.xml");
            auto settingsPart = package.GetPartByUri("/word/settings.xml");
            RequireCycle(documentPart, settingsPart);

            documentWatch = documentPart;
            settingsWatch = settingsPart;
        }

        CHECK(documentWatch.expired());
        CHECK(settingsWatch.expired());
    }

    TEST_CASE("clearing a package frees the parts it held [opc][lifetime][cycle][leak][clear] [unit] [opc-part-lifetime]")
    {
        ExyokiOffice::OpenXmlPackage package;
        REQUIRE(package.LoadFromMemory(BuildPackageWithCyclicRelationships()));

        std::weak_ptr<ExyokiOffice::OpenXmlPackagePart> documentWatch;
        std::weak_ptr<ExyokiOffice::OpenXmlPackagePart> settingsWatch;
        {
            auto documentPart = package.GetPartByUri("/word/document.xml");
            auto settingsPart = package.GetPartByUri("/word/settings.xml");
            RequireCycle(documentPart, settingsPart);

            documentWatch = documentPart;
            settingsWatch = settingsPart;
        }

        package.Clear();

        CHECK(documentWatch.expired());
        CHECK(settingsWatch.expired());
    }

    TEST_CASE("reloading a package frees the previous content [opc][lifetime][cycle][leak][reload] [unit] [opc-part-lifetime]")
    {
        ExyokiOffice::OpenXmlPackage package;
        REQUIRE(package.LoadFromMemory(BuildPackageWithCyclicRelationships()));

        std::weak_ptr<ExyokiOffice::OpenXmlPackagePart> documentWatch;
        {
            auto documentPart = package.GetPartByUri("/word/document.xml");
            REQUIRE(documentPart != nullptr);
            documentWatch = documentPart;
        }

        REQUIRE(package.LoadFromMemory(BuildPackageWithCyclicRelationships()));

        CHECK(documentWatch.expired());
        CHECK(package.GetPartByUri("/word/document.xml") != nullptr);
    }

    TEST_CASE("a component that becomes unreachable is freed with the package still alive [opc][lifetime][cycle][leak][detach] [unit] [opc-part-lifetime]")
    {
        OpcPartLifetime::DetachablePackage package;
        REQUIRE(package.LoadFromMemory(BuildPackageWithCyclicRelationships()));

        std::weak_ptr<ExyokiOffice::OpenXmlPackagePart> documentWatch;
        std::weak_ptr<ExyokiOffice::OpenXmlPackagePart> settingsWatch;
        {
            auto documentPart = package.GetPartByUri("/word/document.xml");
            auto settingsPart = package.GetPartByUri("/word/settings.xml");
            RequireCycle(documentPart, settingsPart);

            documentWatch = documentPart;
            settingsWatch = settingsPart;

            // The only edge from the package root. Both parts are unreachable
            // afterwards, and they hold each other.
            REQUIRE(package.DetachRootPart(documentPart));
        }

        CHECK(documentWatch.expired());
        CHECK(settingsWatch.expired());
        CHECK(package.GetPartByUri("/word/document.xml") == nullptr);
    }
}
