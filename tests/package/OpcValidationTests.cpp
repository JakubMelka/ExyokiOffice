// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "doctest.h"

#include "TestSupport.hpp"

#include "ExyokiOffice/OpenXmlPackage.hpp"
#include "ExyokiOffice/OpenXmlPackageValidator.hpp"
#include "ExyokiOffice/Packaging/WordprocessingDocument.hpp"
#include "ExyokiOffice/Packaging/SpreadsheetDocument.hpp"
#include "ExyokiOffice/Packaging/PowerPointDocument.hpp"
#include "zip/zip.h"
#include "ExyokiOffice/StandardTypes.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <limits>
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
constexpr std::string_view kStylesRelationship =
    "http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles";
constexpr std::string_view kImageRelationship =
    "http://schemas.openxmlformats.org/officeDocument/2006/relationships/image";
constexpr std::string_view kFootnotesRelationship =
    "http://schemas.openxmlformats.org/officeDocument/2006/relationships/footnotes";
constexpr std::string_view kWorksheetRelationship =
    "http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet";
constexpr std::string_view kStylesRelationshipSpreadsheet =
    "http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles";

class AlwaysCancelledToken final : public ExyokiOffice::ICancellationToken
{
public:
    bool IsCancelled() const override
    {
        return true;
    }
};

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

void AddWordContentTypes(zip_t* archive)
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
  <Override PartName="/word/styles.xml" ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.styles+xml"/>
</Types>)");
}

void AddWordParts(zip_t* archive)
{
    AddZipEntry(archive,
                "word/document.xml",
                R"(<?xml version="1.0" encoding="UTF-8"?>
<w:document xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main"><w:body/></w:document>)");
    AddZipEntry(archive,
                "word/settings.xml",
                R"(<?xml version="1.0" encoding="UTF-8"?>
<w:settings xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main"/>)");
    AddZipEntry(archive,
                "word/styles.xml",
                R"(<?xml version="1.0" encoding="UTF-8"?>
<w:styles xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main"/>)");
}

std::vector<ExyokiOffice::Byte> BuildValidPackage()
{
    auto* archive = zip_stream_open(nullptr, 0, ZIP_DEFAULT_COMPRESSION_LEVEL, 'w');
    REQUIRE(archive != nullptr);

    AddWordContentTypes(archive);
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
    AddWordParts(archive);
    return FinishZip(archive);
}

/// Two relationships in one .rels part, one in the other: the package-wide
/// total is three, so only a per-part count leaves MaxRelationships == 2
/// meaningful here.
std::vector<ExyokiOffice::Byte> BuildPackageWithTwoRelationshipsInOnePart()
{
    auto* archive = zip_stream_open(nullptr, 0, ZIP_DEFAULT_COMPRESSION_LEVEL, 'w');
    REQUIRE(archive != nullptr);

    AddWordContentTypes(archive);
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
  <Relationship Id="rId2" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles" Target="styles.xml"/>
</Relationships>)");
    AddWordParts(archive);
    return FinishZip(archive);
}

std::vector<ExyokiOffice::Byte> BuildPackageWithDuplicateRelationshipIdsInOneContainer()
{
    auto* archive = zip_stream_open(nullptr, 0, ZIP_DEFAULT_COMPRESSION_LEVEL, 'w');
    REQUIRE(archive != nullptr);

    AddWordContentTypes(archive);
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
  <Relationship Id="rId7" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/settings" Target="settings.xml"/>
  <Relationship Id="rId7" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles" Target="styles.xml"/>
</Relationships>)");
    AddWordParts(archive);
    return FinishZip(archive);
}

std::vector<ExyokiOffice::Byte> BuildPackageWithDanglingRelationshipTarget()
{
    auto* archive = zip_stream_open(nullptr, 0, ZIP_DEFAULT_COMPRESSION_LEVEL, 'w');
    REQUIRE(archive != nullptr);

    AddWordContentTypes(archive);
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
  <Relationship Id="rId2" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/settings" Target="missing/settings.xml"/>
</Relationships>)");
    AddZipEntry(archive,
                "word/document.xml",
                R"(<?xml version="1.0" encoding="UTF-8"?>
<w:document xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main"><w:body/></w:document>)");
    return FinishZip(archive);
}

/// A presentation whose slide master relationship points at nothing.
///
/// Hand-built rather than saved: the library does not produce a dangling
/// relationship, and a package written and read back cannot be given one
/// through the typed API.
std::vector<ExyokiOffice::Byte> BuildPresentationWithDanglingRelationshipTarget()
{
    auto* archive = zip_stream_open(nullptr, 0, ZIP_DEFAULT_COMPRESSION_LEVEL, 'w');
    REQUIRE(archive != nullptr);

    AddZipEntry(archive,
                "[Content_Types].xml",
                R"(<?xml version="1.0" encoding="UTF-8"?>
<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">
  <Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>
  <Default Extension="xml" ContentType="application/xml"/>
  <Override PartName="/ppt/presentation.xml" ContentType="application/vnd.openxmlformats-officedocument.presentationml.presentation.main+xml"/>
</Types>)");
    AddZipEntry(archive,
                "_rels/.rels",
                R"(<?xml version="1.0" encoding="UTF-8"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">
  <Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="ppt/presentation.xml"/>
</Relationships>)");
    AddZipEntry(archive,
                "ppt/_rels/presentation.xml.rels",
                R"(<?xml version="1.0" encoding="UTF-8"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">
  <Relationship Id="rId2" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/slideMaster" Target="missing/slideMaster1.xml"/>
</Relationships>)");
    AddZipEntry(archive,
                "ppt/presentation.xml",
                R"(<?xml version="1.0" encoding="UTF-8"?>
<p:presentation xmlns:p="http://schemas.openxmlformats.org/presentationml/2006/main"/>)");
    return FinishZip(archive);
}

std::vector<ExyokiOffice::Byte> BuildPackageWithInvalidTargetMode()
{
    auto* archive = zip_stream_open(nullptr, 0, ZIP_DEFAULT_COMPRESSION_LEVEL, 'w');
    REQUIRE(archive != nullptr);

    AddWordContentTypes(archive);
    AddZipEntry(archive,
                "_rels/.rels",
                R"(<?xml version="1.0" encoding="UTF-8"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">
  <Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="word/document.xml" TargetMode="Internal"/>
</Relationships>)");
    AddWordParts(archive);
    return FinishZip(archive);
}

std::vector<ExyokiOffice::Byte> BuildPackageWithMissingRelationshipFields()
{
    auto* archive = zip_stream_open(nullptr, 0, ZIP_DEFAULT_COMPRESSION_LEVEL, 'w');
    REQUIRE(archive != nullptr);

    AddWordContentTypes(archive);
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
  <Relationship Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/settings" Target="settings.xml"/>
  <Relationship Id="rId8" Target="styles.xml"/>
  <Relationship Id="rId9" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles"/>
</Relationships>)");
    AddWordParts(archive);
    return FinishZip(archive);
}

std::vector<ExyokiOffice::Byte> BuildPackageWithSharedInternalTarget()
{
    auto* archive = zip_stream_open(nullptr, 0, ZIP_DEFAULT_COMPRESSION_LEVEL, 'w');
    REQUIRE(archive != nullptr);

    AddWordContentTypes(archive);
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
  <Relationship Id="rId2" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles" Target="styles.xml"/>
  <Relationship Id="rId3" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/image" Target="../media/shared.png"/>
</Relationships>)");
    AddZipEntry(archive,
                "word/_rels/styles.xml.rels",
                R"(<?xml version="1.0" encoding="UTF-8"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">
  <Relationship Id="rId4" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/image" Target="../media/shared.png"/>
</Relationships>)");
    AddWordParts(archive);
    AddZipEntry(archive, "media/shared.png", "png");
    return FinishZip(archive);
}

std::vector<ExyokiOffice::Byte> BuildPackageWithExternalRelationship()
{
    auto* archive = zip_stream_open(nullptr, 0, ZIP_DEFAULT_COMPRESSION_LEVEL, 'w');
    REQUIRE(archive != nullptr);

    AddWordContentTypes(archive);
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
  <Relationship Id="rId2" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/image" Target="https://example.test/image.png" TargetMode="External"/>
</Relationships>)");
    AddZipEntry(archive,
                "word/document.xml",
                R"(<?xml version="1.0" encoding="UTF-8"?>
<w:document xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main"><w:body/></w:document>)");
    return FinishZip(archive);
}

std::vector<ExyokiOffice::Byte> BuildPackageWithInvalidSchematronRelationshipType()
{
    auto* archive = zip_stream_open(nullptr, 0, ZIP_DEFAULT_COMPRESSION_LEVEL, 'w');
    REQUIRE(archive != nullptr);

    AddWordContentTypes(archive);
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
  <Relationship Id="rIdImage" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/settings" Target="settings.xml"/>
</Relationships>)");
    AddZipEntry(archive,
                "word/document.xml",
                R"(<?xml version="1.0" encoding="UTF-8"?>
<w:document xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main"
            xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships"
            xmlns:v="urn:schemas-microsoft-com:vml">
  <w:body>
    <w:p><w:r><w:pict><v:fill r:id="rIdImage"/></w:pict></w:r></w:p>
  </w:body>
</w:document>)");
    AddZipEntry(archive,
                "word/settings.xml",
                R"(<?xml version="1.0" encoding="UTF-8"?>
<w:settings xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main"/>)");
    return FinishZip(archive);
}

std::vector<ExyokiOffice::Byte> BuildPackageWithDuplicateNormalizedPartUri()
{
    auto* archive = zip_stream_open(nullptr, 0, ZIP_DEFAULT_COMPRESSION_LEVEL, 'w');
    REQUIRE(archive != nullptr);

    AddWordContentTypes(archive);
    AddZipEntry(archive,
                "_rels/.rels",
                R"(<?xml version="1.0" encoding="UTF-8"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">
  <Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="word/document.xml"/>
</Relationships>)");
    AddZipEntry(archive,
                "word/document.xml",
                R"(<?xml version="1.0" encoding="UTF-8"?>
<w:document xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main"><w:body/></w:document>)");
    AddZipEntry(archive,
                "word/./document.xml",
                R"(<?xml version="1.0" encoding="UTF-8"?>
<w:document xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main"><w:body><w:p/></w:body></w:document>)");
    return FinishZip(archive);
}

std::vector<ExyokiOffice::Byte> BuildPackageWithDocumentXml(std::string_view documentXml)
{
    auto* archive = zip_stream_open(nullptr, 0, ZIP_DEFAULT_COMPRESSION_LEVEL, 'w');
    REQUIRE(archive != nullptr);

    AddWordContentTypes(archive);
    AddZipEntry(archive,
                "_rels/.rels",
                R"(<?xml version="1.0" encoding="UTF-8"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">
  <Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="word/document.xml"/>
</Relationships>)");
    AddZipEntry(archive, "word/document.xml", documentXml);
    return FinishZip(archive);
}

std::vector<ExyokiOffice::Byte> BuildPackageWithInvalidFootnoteReference()
{
    auto* archive = zip_stream_open(nullptr, 0, ZIP_DEFAULT_COMPRESSION_LEVEL, 'w');
    REQUIRE(archive != nullptr);

    AddWordContentTypes(archive);
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
  <Relationship Id="rIdFootnotes" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/footnotes" Target="footnotes.xml"/>
</Relationships>)");
    AddZipEntry(archive,
                "word/document.xml",
                R"(<?xml version="1.0" encoding="UTF-8"?>
<w:document xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main">
  <w:body><w:p><w:r><w:footnoteReference w:id="42"/></w:r></w:p></w:body>
</w:document>)");
    AddZipEntry(archive,
                "word/footnotes.xml",
                R"(<?xml version="1.0" encoding="UTF-8"?>
<w:footnotes xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main">
  <w:footnote w:id="1"/>
  <w:footnote w:id="2"/>
</w:footnotes>)");
    return FinishZip(archive);
}

std::vector<ExyokiOffice::Byte> BuildSpreadsheetPackageWithInvalidStyleIndex()
{
    auto* archive = zip_stream_open(nullptr, 0, ZIP_DEFAULT_COMPRESSION_LEVEL, 'w');
    REQUIRE(archive != nullptr);

    AddZipEntry(archive,
                "[Content_Types].xml",
                R"(<?xml version="1.0" encoding="UTF-8"?>
<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">
  <Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>
  <Default Extension="xml" ContentType="application/xml"/>
</Types>)");
    AddZipEntry(archive,
                "_rels/.rels",
                R"(<?xml version="1.0" encoding="UTF-8"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">
  <Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="xl/workbook.xml"/>
</Relationships>)");
    AddZipEntry(archive,
                "xl/_rels/workbook.xml.rels",
                R"(<?xml version="1.0" encoding="UTF-8"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">
  <Relationship Id="rIdStyles" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles" Target="styles.xml"/>
  <Relationship Id="rIdSheet" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet" Target="worksheets/sheet1.xml"/>
</Relationships>)");
    AddZipEntry(archive,
                "xl/workbook.xml",
                R"(<?xml version="1.0" encoding="UTF-8"?>
<x:workbook xmlns:x="http://schemas.openxmlformats.org/spreadsheetml/2006/main">
  <x:sheets><x:sheet x:name="Sheet1" x:sheetId="1"/></x:sheets>
</x:workbook>)");
    AddZipEntry(archive,
                "xl/styles.xml",
                R"(<?xml version="1.0" encoding="UTF-8"?>
<x:styleSheet xmlns:x="http://schemas.openxmlformats.org/spreadsheetml/2006/main">
  <x:cellXfs><x:xf/></x:cellXfs>
</x:styleSheet>)");
    AddZipEntry(archive,
                "xl/worksheets/sheet1.xml",
                R"(<?xml version="1.0" encoding="UTF-8"?>
<x:worksheet xmlns:x="http://schemas.openxmlformats.org/spreadsheetml/2006/main">
  <x:sheetData><x:row><x:c x:r="A1" x:s="2"/></x:row></x:sheetData>
</x:worksheet>)");
    return FinishZip(archive);
}

std::vector<ExyokiOffice::Byte> BuildPackageWithInvalidSamePartCount()
{
    return BuildPackageWithDocumentXml(
        R"(<?xml version="1.0" encoding="UTF-8"?>
<x:comments xmlns:x="http://schemas.openxmlformats.org/spreadsheetml/2006/main">
  <x:authors><x:author>Ada</x:author></x:authors>
  <x:commentList><x:comment x:ref="A1" x:authorId="2"/></x:commentList>
</x:comments>)");
}

std::vector<ExyokiOffice::Byte> BuildPackageWithInvalidParentPartReference()
{
    auto* archive = zip_stream_open(nullptr, 0, ZIP_DEFAULT_COMPRESSION_LEVEL, 'w');
    REQUIRE(archive != nullptr);

    AddZipEntry(archive,
                "[Content_Types].xml",
                R"(<?xml version="1.0" encoding="UTF-8"?>
<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">
  <Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>
  <Default Extension="xml" ContentType="application/xml"/>
</Types>)");
    AddZipEntry(archive,
                "_rels/.rels",
                R"(<?xml version="1.0" encoding="UTF-8"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">
  <Relationship Id="rId1" Type="http://schemas.example.test/root" Target="custom/parent.xml"/>
</Relationships>)");
    AddZipEntry(archive,
                "custom/_rels/parent.xml.rels",
                R"(<?xml version="1.0" encoding="UTF-8"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">
  <Relationship Id="rIdChild" Type="http://schemas.example.test/child" Target="child.xml"/>
</Relationships>)");
    AddZipEntry(archive,
                "custom/parent.xml",
                R"(<?xml version="1.0" encoding="UTF-8"?>
<x14:root xmlns:x14="http://schemas.microsoft.com/office/spreadsheetml/2009/9/main">
  <x14:cfRule x14:priority="1"/>
</x14:root>)");
    AddZipEntry(archive,
                "custom/child.xml",
                R"(<?xml version="1.0" encoding="UTF-8"?>
<x14:root xmlns:x14="http://schemas.microsoft.com/office/spreadsheetml/2009/9/main">
  <x14:conditionalFormat x14:priority="2"/>
</x14:root>)");
    return FinishZip(archive);
}

std::vector<ExyokiOffice::ValidationIssue> FindIssues(
    const ExyokiOffice::ValidationResult& result,
    ExyokiOffice::ValidationErrorId id)
{
    std::vector<ExyokiOffice::ValidationIssue> issues;
    std::copy_if(result.Issues().begin(), result.Issues().end(), std::back_inserter(issues), [id](const auto& issue)
                 { return issue.Id == id; });
    return issues;
}

std::vector<ExyokiOffice::ValidationIssue> FindPackageUniqueValueIssues(
    const ExyokiOffice::ValidationResult& result)
{
    std::vector<ExyokiOffice::ValidationIssue> issues;
    std::copy_if(result.Issues().begin(), result.Issues().end(), std::back_inserter(issues), [](const auto& issue)
                 { return issue.Domain == ExyokiOffice::ValidationDomain::Packaging && issue.Id == ExyokiOffice::ValidationErrorId::Unknown && issue.Message.find("unique-values") != std::string::npos; });
    return issues;
}

std::vector<ExyokiOffice::ValidationIssue> FindPackageSchematronIssues(
    const ExyokiOffice::ValidationResult& result,
    std::string_view marker)
{
    std::vector<ExyokiOffice::ValidationIssue> issues;
    std::copy_if(result.Issues().begin(), result.Issues().end(), std::back_inserter(issues), [marker](const auto& issue)
                 { return issue.Domain == ExyokiOffice::ValidationDomain::Packaging && issue.Id == ExyokiOffice::ValidationErrorId::Unknown && issue.Message.find(marker) != std::string::npos; });
    return issues;
}

class CountingSink final : public ExyokiOffice::DiagnosticSink
{
public:
    void Report(ExyokiOffice::ValidationIssue issue) override
    {
        Issues.push_back(std::move(issue));
    }

    std::vector<ExyokiOffice::ValidationIssue> Issues;
};

} // namespace

TEST_SUITE("OpcValidationTests")
{

    TEST_CASE("valid relationship graph returns no diagnostics [opc][validation][relationships][valid][word] [unit] [opc-validation]")
    {
        ExyokiOffice::OpenXmlPackage package;
        REQUIRE(package.LoadFromMemory(BuildValidPackage()));

        const auto result = ExyokiOffice::OpenXmlPackageValidator().Validate(package);
        CHECK(result.IsValid());
        CHECK_FALSE(result.HasErrors());
        CHECK_FALSE(result.HasWarnings());
        CHECK(result.Issues().empty());
    }

    TEST_CASE("duplicate relationship ids are reported per source container [opc][validation][relationships][duplicate-id][word] [unit] [opc-validation]")
    {
        ExyokiOffice::OpenXmlPackage package;
        REQUIRE(package.LoadFromMemory(BuildPackageWithDuplicateRelationshipIdsInOneContainer()));

        const auto result = ExyokiOffice::OpenXmlPackageValidator().Validate(package);

        const auto duplicateIdIssues = FindIssues(result, ExyokiOffice::ValidationErrorId::OpcDuplicateRelationshipId);
        REQUIRE(duplicateIdIssues.size() == 1);
        CHECK_FALSE(result.IsValid());
        CHECK(duplicateIdIssues.front().Domain == ExyokiOffice::ValidationDomain::Opc);
        CHECK(duplicateIdIssues.front().Severity == ExyokiOffice::ValidationSeverity::Error);
        CHECK(duplicateIdIssues.front().RelationshipSourceUri == "/word/document.xml");
        CHECK(duplicateIdIssues.front().PartUri == "/word/document.xml");
        CHECK(duplicateIdIssues.front().RelationshipId == "rId7");
        CHECK(duplicateIdIssues.front().RelationshipType == kStylesRelationship);
        CHECK(duplicateIdIssues.front().TargetUri == "styles.xml");
    }

    TEST_CASE("same relationship id in different source containers is valid [opc][validation][relationships][duplicate-id][valid][word] [unit] [opc-validation]")
    {
        ExyokiOffice::OpenXmlPackage package;
        REQUIRE(package.LoadFromMemory(BuildValidPackage()));

        const auto result = ExyokiOffice::OpenXmlPackageValidator().Validate(package);

        CHECK(FindIssues(result, ExyokiOffice::ValidationErrorId::OpcDuplicateRelationshipId).empty());
        CHECK(result.IsValid());
    }

    TEST_CASE("dangling internal relationship target is reported with resolved URI [opc][validation][relationships][dangling-target][word] [unit] [opc-validation]")
    {
        ExyokiOffice::OpenXmlPackage package;
        REQUIRE(package.LoadFromMemory(BuildPackageWithDanglingRelationshipTarget()));

        const auto result = ExyokiOffice::OpenXmlPackageValidator().Validate(package);

        const auto danglingTargetIssues =
            FindIssues(result, ExyokiOffice::ValidationErrorId::OpcDanglingRelationshipTarget);
        REQUIRE(danglingTargetIssues.size() == 1);
        CHECK_FALSE(result.IsValid());
        CHECK(danglingTargetIssues.front().RelationshipSourceUri == "/word/document.xml");
        CHECK(danglingTargetIssues.front().RelationshipId == "rId2");
        CHECK(danglingTargetIssues.front().RelationshipType == kSettingsRelationship);
        CHECK(danglingTargetIssues.front().TargetUri == "/word/missing/settings.xml");
    }

    TEST_CASE("duplicate normalized part URIs are reported [opc][validation][duplicate-uri][word] [unit] [opc-validation]")
    {
        ExyokiOffice::OpenXmlPackage package;
        REQUIRE(package.LoadFromMemory(BuildPackageWithDuplicateNormalizedPartUri()));

        const auto result = ExyokiOffice::OpenXmlPackageValidator().Validate(package);

        const auto duplicateUriIssues = FindIssues(result, ExyokiOffice::ValidationErrorId::OpcDuplicatePartUri);
        REQUIRE(duplicateUriIssues.size() == 1);
        CHECK_FALSE(result.IsValid());
        CHECK(duplicateUriIssues.front().Domain == ExyokiOffice::ValidationDomain::Opc);
        CHECK(duplicateUriIssues.front().Severity == ExyokiOffice::ValidationSeverity::Error);
        CHECK(duplicateUriIssues.front().PartUri == "/word/document.xml");
        CHECK(duplicateUriIssues.front().TargetUri == "/word/document.xml");
    }

    TEST_CASE("external relationship target is not resolved as a package part [opc][validation][relationships][external][target-mode][word] [unit] [opc-validation]")
    {
        ExyokiOffice::OpenXmlPackage package;
        REQUIRE(package.LoadFromMemory(BuildPackageWithExternalRelationship()));

        const auto result = ExyokiOffice::OpenXmlPackageValidator().Validate(package);

        CHECK(result.IsValid());
        CHECK(FindIssues(result, ExyokiOffice::ValidationErrorId::OpcDanglingRelationshipTarget).empty());
        auto mainPart = package.GetPartByUri("/word/document.xml");
        REQUIRE(mainPart != nullptr);
        REQUIRE(mainPart->Relationships().size() == 1);
        CHECK(mainPart->Relationships().front().TargetMode == "External");
    }

    TEST_CASE("generated schematron relationship type rules are enforced [opc][validation][schematron][relationships][word] [unit] [opc-validation]")
    {
        ExyokiOffice::OpenXmlPackage package;
        REQUIRE(package.LoadFromMemory(BuildPackageWithInvalidSchematronRelationshipType()));

        const auto result = ExyokiOffice::OpenXmlPackageValidator().Validate(package);

        const auto issues = result.Issues();
        REQUIRE(issues.size() == 1);
        CHECK_FALSE(result.IsValid());
        CHECK(issues.front().Domain == ExyokiOffice::ValidationDomain::Packaging);
        CHECK(issues.front().Severity == ExyokiOffice::ValidationSeverity::Error);
        CHECK(issues.front().Id == ExyokiOffice::ValidationErrorId::Unknown);
        CHECK(issues.front().PartUri == "/word/document.xml");
        CHECK(issues.front().RelationshipSourceUri == "/word/document.xml");
        CHECK(issues.front().RelationshipId == "rIdImage");
        CHECK(issues.front().RelationshipType == kSettingsRelationship);
        CHECK(issues.front().TargetUri == "settings.xml");
        CHECK(issues.front().Message.find(std::string(kImageRelationship)) != std::string::npos);
    }

    TEST_CASE("generated schematron unique value rules are enforced for scoped paths [opc][validation][schematron][unique-values][word] [unit] [opc-validation]")
    {
        ExyokiOffice::OpenXmlPackage package;
        REQUIRE(package.LoadFromMemory(BuildPackageWithDocumentXml(
            R"(<?xml version="1.0" encoding="UTF-8"?>
<w:endnotes xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main">
  <w:endnote w:id="1"/>
  <w:endnote w:id="2"/>
  <w:endnote w:id="1"/>
</w:endnotes>)")));

        const auto result = ExyokiOffice::OpenXmlPackageValidator().Validate(package);

        const auto issues = FindPackageUniqueValueIssues(result);
        REQUIRE(issues.size() == 1);
        CHECK_FALSE(result.IsValid());
        CHECK(issues.front().PartUri == "/word/document.xml");
        CHECK(issues.front().Message.find("@w:id") != std::string::npos);
    }

    TEST_CASE("generated schematron unique value rules honor lower-case semantics [opc][validation][schematron][unique-values][case-insensitive][spreadsheet] [unit] [opc-validation]")
    {
        ExyokiOffice::OpenXmlPackage package;
        REQUIRE(package.LoadFromMemory(BuildPackageWithDocumentXml(
            R"(<?xml version="1.0" encoding="UTF-8"?>
<x:workbook xmlns:x="http://schemas.openxmlformats.org/spreadsheetml/2006/main">
  <x:sheets>
    <x:sheet x:name="Summary" x:sheetId="1"/>
    <x:sheet x:name="summary" x:sheetId="2"/>
  </x:sheets>
</x:workbook>)")));

        const auto result = ExyokiOffice::OpenXmlPackageValidator().Validate(package);

        const auto issues = FindPackageUniqueValueIssues(result);
        REQUIRE(issues.size() == 1);
        CHECK_FALSE(result.IsValid());
        CHECK(issues.front().PartUri == "/word/document.xml");
        CHECK(issues.front().Message.find("lower-case") != std::string::npos);
    }

    TEST_CASE("generated schematron ancestor unique value rules are scoped to the ancestor [opc][validation][schematron][unique-values][ancestor][spreadsheet] [unit] [opc-validation]")
    {
        ExyokiOffice::OpenXmlPackage package;
        REQUIRE(package.LoadFromMemory(BuildPackageWithDocumentXml(
            R"(<?xml version="1.0" encoding="UTF-8"?>
<x:tables xmlns:x="http://schemas.openxmlformats.org/spreadsheetml/2006/main">
  <x:table>
    <x:tableColumns>
      <x:tableColumn x:id="1" x:name="A"/>
      <x:tableColumn x:id="2" x:name="B"/>
      <x:tableColumn x:id="1" x:name="C"/>
    </x:tableColumns>
  </x:table>
  <x:table>
    <x:tableColumns>
      <x:tableColumn x:id="1" x:name="A"/>
    </x:tableColumns>
  </x:table>
</x:tables>)")));

        const auto result = ExyokiOffice::OpenXmlPackageValidator().Validate(package);

        const auto issues = FindPackageUniqueValueIssues(result);
        REQUIRE(issues.size() == 1);
        CHECK_FALSE(result.IsValid());
        CHECK(issues.front().PartUri == "/word/document.xml");
        CHECK(issues.front().Message.find("ancestor::x:table") != std::string::npos);
        CHECK(issues.front().Message.find("@x:id") != std::string::npos);
    }

    TEST_CASE("generated schematron part reference rules resolve relative child parts [opc][validation][schematron][part-reference][word] [unit] [opc-validation]")
    {
        ExyokiOffice::OpenXmlPackage package;
        REQUIRE(package.LoadFromMemory(BuildPackageWithInvalidFootnoteReference()));

        const auto result = ExyokiOffice::OpenXmlPackageValidator().Validate(package);

        const auto issues = FindPackageSchematronIssues(result, "part-reference");
        REQUIRE(issues.size() == 1);
        CHECK_FALSE(result.IsValid());
        CHECK(issues.front().PartUri == "/word/document.xml");
        CHECK(issues.front().Message.find("Part:FootnotesPart") != std::string::npos);
        CHECK(issues.front().Message.find("@w:id") != std::string::npos);
    }

    TEST_CASE("generated schematron part count rules resolve absolute package part paths [opc][validation][schematron][part-count][spreadsheet] [unit] [opc-validation]")
    {
        ExyokiOffice::OpenXmlPackage package;
        REQUIRE(package.LoadFromMemory(BuildSpreadsheetPackageWithInvalidStyleIndex()));

        const auto result = ExyokiOffice::OpenXmlPackageValidator().Validate(package);

        const auto issues = FindPackageSchematronIssues(result, "part-count");
        REQUIRE(issues.size() == 1);
        CHECK_FALSE(result.IsValid());
        CHECK(issues.front().PartUri == "/xl/worksheets/sheet1.xml");
        CHECK(issues.front().Message.find("WorkbookStylesPart") != std::string::npos);
        CHECK(issues.front().Message.find("@x:s") != std::string::npos);
    }

    TEST_CASE("generated schematron part count rules resolve the current part selector [opc][validation][schematron][part-count][current-part] [unit] [opc-validation]")
    {
        ExyokiOffice::OpenXmlPackage package;
        REQUIRE(package.LoadFromMemory(BuildPackageWithInvalidSamePartCount()));

        const auto result = ExyokiOffice::OpenXmlPackageValidator().Validate(package);

        const auto issues = FindPackageSchematronIssues(result, "part-count");
        REQUIRE(issues.size() == 1);
        CHECK_FALSE(result.IsValid());
        CHECK(issues.front().PartUri == "/word/document.xml");
        CHECK(issues.front().Message.find("Part:.") != std::string::npos);
        CHECK(issues.front().Message.find("@x:authorId") != std::string::npos);
    }

    TEST_CASE("generated schematron part reference rules resolve the parent part selector [opc][validation][schematron][part-reference][parent-part] [unit] [opc-validation]")
    {
        ExyokiOffice::OpenXmlPackage package;
        REQUIRE(package.LoadFromMemory(BuildPackageWithInvalidParentPartReference()));

        const auto result = ExyokiOffice::OpenXmlPackageValidator().Validate(package);

        const auto issues = FindPackageSchematronIssues(result, "part-reference");
        REQUIRE(issues.size() == 1);
        CHECK_FALSE(result.IsValid());
        CHECK(issues.front().PartUri == "/custom/child.xml");
        CHECK(issues.front().Message.find("Part:..") != std::string::npos);
        CHECK(issues.front().Message.find("@x14:priority") != std::string::npos);
    }

    TEST_CASE("invalid relationship target mode is reported while target is still resolved internally [opc][validation][relationships][target-mode][word] [unit] [opc-validation]")
    {
        ExyokiOffice::OpenXmlPackage package;
        REQUIRE(package.LoadFromMemory(BuildPackageWithInvalidTargetMode()));

        const auto result = ExyokiOffice::OpenXmlPackageValidator().Validate(package);

        const auto invalidModeIssues =
            FindIssues(result, ExyokiOffice::ValidationErrorId::OpcInvalidRelationshipTargetMode);
        REQUIRE(invalidModeIssues.size() == 1);
        CHECK_FALSE(result.IsValid());
        CHECK(invalidModeIssues.front().RelationshipSourceUri == "/");
        CHECK(invalidModeIssues.front().RelationshipId == "rId1");
        CHECK(invalidModeIssues.front().RelationshipType == kOfficeDocumentRelationship);
        CHECK(invalidModeIssues.front().TargetUri == "word/document.xml");
        CHECK(invalidModeIssues.front().TargetMode == "Internal");
        CHECK(FindIssues(result, ExyokiOffice::ValidationErrorId::OpcDanglingRelationshipTarget).empty());
    }

    TEST_CASE("missing relationship attributes are reported independently [opc][validation][relationships][missing-fields][word] [unit] [opc-validation]")
    {
        ExyokiOffice::OpenXmlPackage package;
        REQUIRE(package.LoadFromMemory(BuildPackageWithMissingRelationshipFields()));

        const auto result = ExyokiOffice::OpenXmlPackageValidator().Validate(package);

        const auto missingIdIssues = FindIssues(result, ExyokiOffice::ValidationErrorId::OpcEmptyRelationshipId);
        const auto missingTypeIssues = FindIssues(result, ExyokiOffice::ValidationErrorId::OpcEmptyRelationshipType);
        const auto missingTargetIssues = FindIssues(result, ExyokiOffice::ValidationErrorId::OpcEmptyRelationshipTarget);
        REQUIRE(missingIdIssues.size() == 1);
        REQUIRE(missingTypeIssues.size() == 1);
        REQUIRE(missingTargetIssues.size() == 1);
        CHECK_FALSE(result.IsValid());
        CHECK(missingIdIssues.front().RelationshipSourceUri == "/word/document.xml");
        CHECK(missingTypeIssues.front().RelationshipId == "rId8");
        CHECK(missingTypeIssues.front().TargetUri == "styles.xml");
        CHECK(missingTargetIssues.front().RelationshipId == "rId9");
        CHECK(missingTargetIssues.front().RelationshipType == kStylesRelationship);
    }

    TEST_CASE("shared internal targets from multiple source parts are accepted [opc][validation][relationships][shared-target][word] [unit] [opc-validation]")
    {
        ExyokiOffice::OpenXmlPackage package;
        REQUIRE(package.LoadFromMemory(BuildPackageWithSharedInternalTarget()));

        const auto result = ExyokiOffice::OpenXmlPackageValidator().Validate(package);

        CHECK(result.IsValid());
        CHECK(FindIssues(result, ExyokiOffice::ValidationErrorId::OpcDanglingRelationshipTarget).empty());
        CHECK(package.GetPartByUri("/media/shared.png") != nullptr);
    }

    TEST_CASE("validator can report through a diagnostic sink [opc][validation][diagnostics][sink][word] [unit] [opc-validation]")
    {
        ExyokiOffice::OpenXmlPackage package;
        REQUIRE(package.LoadFromMemory(BuildPackageWithDanglingRelationshipTarget()));

        CountingSink sink;
        ExyokiOffice::OpenXmlPackageValidator().Validate(package, sink);

        REQUIRE(sink.Issues.size() == 1);
        CHECK(sink.Issues.front().Id == ExyokiOffice::ValidationErrorId::OpcDanglingRelationshipTarget);
        CHECK(sink.Issues.front().Domain == ExyokiOffice::ValidationDomain::Opc);
        CHECK(sink.Issues.front().RelationshipSourceUri == "/word/document.xml");
        CHECK(sink.Issues.front().TargetUri == "/word/missing/settings.xml");
    }

    TEST_CASE("semantic validation reports a missing main part for Word Excel and PowerPoint [opc][validation][semantic] [unit] [opc-validation]")
    {
        ExyokiOffice::Packaging::WordDocument word;
        ExyokiOffice::Packaging::ExcelDocument excel;
        ExyokiOffice::Packaging::PowerPointDocument powerPoint;

        CHECK(FindIssues(ExyokiOffice::OpenXmlPackageValidator().Validate(word),
                         ExyokiOffice::ValidationErrorId::PackageMissingMainPart)
                  .size() == 1);
        CHECK(FindIssues(ExyokiOffice::OpenXmlPackageValidator().Validate(excel),
                         ExyokiOffice::ValidationErrorId::PackageMissingMainPart)
                  .size() == 1);
        CHECK(FindIssues(ExyokiOffice::OpenXmlPackageValidator().Validate(powerPoint),
                         ExyokiOffice::ValidationErrorId::PackageMissingMainPart)
                  .size() == 1);
    }

    TEST_CASE("semantic validation checks relationship and content types against part descriptors [opc][validation][semantic] [unit] [opc-validation]")
    {
        auto word = ExyokiOffice::Packaging::WordDocument::Create(
            ExyokiOffice::Packaging::WordprocessingDocumentType::Document);
        REQUIRE(word);
        auto main = word->AddMainDocumentPart();
        REQUIRE(main);
        auto settings = main->AddDocumentSettingsPart();
        REQUIRE(settings);

        main->SetContentType("application/x-wrong-main");
        settings->SetContentType("application/x-wrong-settings");
        REQUIRE_FALSE(main->AddPartReference(settings, "urn:wrong-relationship-type").empty());

        const auto result = ExyokiOffice::OpenXmlPackageValidator().Validate(*word);
        const auto contentIssues = FindIssues(result, ExyokiOffice::ValidationErrorId::PackageContentTypeMismatch);
        const auto relationshipIssues = FindIssues(result, ExyokiOffice::ValidationErrorId::PackageRelationshipTypeMismatch);
        REQUIRE(contentIssues.size() == 2);
        CHECK(std::any_of(contentIssues.begin(), contentIssues.end(), [&](const auto& issue)
                          { return issue.PartUri == main->Uri(); }));
        CHECK(std::any_of(contentIssues.begin(), contentIssues.end(), [&](const auto& issue)
                          { return issue.PartUri == settings->Uri(); }));
        REQUIRE(relationshipIssues.size() == 1);
        CHECK(relationshipIssues.front().RelationshipSourceUri == main->Uri());
        CHECK(relationshipIssues.front().TargetUri == settings->Uri());
    }

    TEST_CASE("semantic validation applies the target Office version to package parts [opc][validation][semantic][version] [unit] [opc-validation]")
    {
        ExyokiOffice::OpenXmlPackage package;
        auto control = std::make_shared<ExyokiOffice::Packaging::ControlPropertiesPart>();
        REQUIRE(package.AttachCustomPart(control,
                                         ExyokiOffice::Packaging::ControlPropertiesPart::Descriptor()));

        const auto office2007 = ExyokiOffice::OpenXmlPackageValidator(
                                    {ExyokiOffice::OpenXml::FileFormatVersions::Office2007})
                                    .Validate(package);
        CHECK(FindIssues(office2007, ExyokiOffice::ValidationErrorId::PartVersionViolation).size() == 1);

        const auto office2010 = ExyokiOffice::OpenXmlPackageValidator(
                                    {ExyokiOffice::OpenXml::FileFormatVersions::Office2010})
                                    .Validate(package);
        CHECK(FindIssues(office2010, ExyokiOffice::ValidationErrorId::PartVersionViolation).empty());
    }

    TEST_CASE("strict open policy rejects invalid relationship graph [opc][validation][strict][word] [unit] [opc-validation]")
    {
        auto packageBytes = BuildPackageWithDanglingRelationshipTarget();
        ExyokiOffice::Packaging::OpenSettings settings;
        settings.OpcValidation = ExyokiOffice::Packaging::OpcValidationMode::Strict;

        CountingSink sink;
        settings.ValidationDiagnostics = &sink;

        auto document = ExyokiOffice::Packaging::WordDocument::Open(packageBytes, settings);

        CHECK(document == nullptr);
        REQUIRE(sink.Issues.size() == 1);
        CHECK(sink.Issues.front().Severity == ExyokiOffice::ValidationSeverity::Error);
        CHECK(sink.Issues.front().Id == ExyokiOffice::ValidationErrorId::OpcDanglingRelationshipTarget);
    }

    TEST_CASE("the open policy reaches PowerPoint too [opc][validation][strict][powerpoint] [unit] [opc-validation]")
    {
        // PowerPoint's FinishOpen ran the markup compatibility pass and nothing
        // else, so OpcValidationMode meant nothing for a .pptx however it was
        // set - while OpenSettings promised the same behaviour for all three
        // families.
        const auto packageBytes = BuildPresentationWithDanglingRelationshipTarget();

        SUBCASE("strict refuses the package")
        {
            ExyokiOffice::Packaging::OpenSettings settings;
            settings.OpcValidation = ExyokiOffice::Packaging::OpcValidationMode::Strict;

            ExyokiOffice::Packaging::OpenError error;
            auto document = ExyokiOffice::Packaging::PowerPointDocument::Open(packageBytes, settings, nullptr, &error);
            CHECK(document == nullptr);
            CHECK(error.Code == ExyokiOffice::Packaging::OpenErrorCode::ValidationFailed);
            CHECK_FALSE(error.Diagnostics.Issues().empty());
        }

        SUBCASE("tolerant opens it and keeps the issue as a warning")
        {
            ExyokiOffice::Packaging::OpenSettings settings;
            settings.OpcValidation = ExyokiOffice::Packaging::OpcValidationMode::Tolerant;

            auto document = ExyokiOffice::Packaging::PowerPointDocument::Open(packageBytes, settings);
            REQUIRE(document != nullptr);
            REQUIRE(document->LastValidationResult().Issues().size() == 1);
            CHECK(document->LastValidationResult().Issues().front().Id ==
                  ExyokiOffice::ValidationErrorId::OpcDanglingRelationshipTarget);
            CHECK(document->LastValidationResult().Issues().front().Severity ==
                  ExyokiOffice::ValidationSeverity::Warning);
        }

        SUBCASE("a part over the character budget fails the open")
        {
            ExyokiOffice::Packaging::OpenSettings settings;
            settings.MaxCharactersInPart = 32;

            ExyokiOffice::Packaging::OpenError error;
            CHECK(ExyokiOffice::Packaging::PowerPointDocument::Open(packageBytes, settings, nullptr, &error) ==
                  nullptr);
            CHECK(error.Code == ExyokiOffice::Packaging::OpenErrorCode::PartTooLarge);
        }
    }

    TEST_CASE("tolerant open policy keeps document and exposes warnings [opc][validation][tolerant][word] [unit] [opc-validation]")
    {
        auto packageBytes = BuildPackageWithDanglingRelationshipTarget();
        ExyokiOffice::Packaging::OpenSettings settings;
        settings.OpcValidation = ExyokiOffice::Packaging::OpcValidationMode::Tolerant;

        CountingSink sink;
        settings.ValidationDiagnostics = &sink;

        auto document = ExyokiOffice::Packaging::WordDocument::Open(packageBytes, settings);

        REQUIRE(document != nullptr);
        REQUIRE(document->LastValidationResult().Issues().size() == 1);
        CHECK(document->LastValidationResult().Issues().front().Severity == ExyokiOffice::ValidationSeverity::Warning);
        CHECK(document->LastValidationResult().Issues().front().Id == ExyokiOffice::ValidationErrorId::OpcDanglingRelationshipTarget);
        REQUIRE(sink.Issues.size() == 1);
        CHECK(sink.Issues.front().Severity == ExyokiOffice::ValidationSeverity::Warning);
    }

    TEST_CASE("Unlimited() sets no limits at all [opc][limits][presets] [unit] [opc-validation]")
    {
        const auto limits = ExyokiOffice::OpenXmlPackageLimits::Unlimited();

        // Zero is the "no limit" sentinel, so the preset must be all zeros and
        // indistinguishable from a default-constructed value.
        CHECK(limits.MaxEntries == 0);
        CHECK(limits.MaxCompressedBytes == 0);
        CHECK(limits.MaxUncompressedBytes == 0);
        CHECK(limits.MaxPartBytes == 0);
        CHECK(limits.MaxRelationships == 0);
        CHECK(limits.MaxCompressionRatio == 0);
        CHECK(limits.MaxXmlDepth == 0);
        CHECK(limits.MaxXmlNodes == 0);
        CHECK(limits.MaxXmlAttributes == 0);
        CHECK(limits.MaxXmlTextCharacters == 0);

        ExyokiOffice::OpenXmlPackage package;
        package.SetPackageLimits(limits);
        CHECK(package.LoadFromMemory(BuildValidPackage()));
    }

    TEST_CASE("Recommended() bounds every limit and still opens ordinary packages [opc][limits][presets] [unit] [opc-validation]")
    {
        const auto limits = ExyokiOffice::OpenXmlPackageLimits::Recommended();

        // Every knob must be armed - one left at zero would be a silent hole.
        CHECK(limits.MaxEntries > 0);
        CHECK(limits.MaxCompressedBytes > 0);
        CHECK(limits.MaxUncompressedBytes > 0);
        CHECK(limits.MaxPartBytes > 0);
        CHECK(limits.MaxRelationships > 0);
        CHECK(limits.MaxCompressionRatio > 0);
        CHECK(limits.MaxXmlDepth > 0);
        CHECK(limits.MaxXmlNodes > 0);
        CHECK(limits.MaxXmlAttributes > 0);
        CHECK(limits.MaxXmlTextCharacters > 0);

        // A single part cannot be allowed to exceed the whole package.
        CHECK(limits.MaxPartBytes <= limits.MaxUncompressedBytes);

        ExyokiOffice::OpenXmlPackage package;
        package.SetPackageLimits(limits);
        CHECK(package.LoadFromMemory(BuildValidPackage()));
        CHECK(package.LastValidationResult().IsValid());
    }

    TEST_CASE("Recommended() limits reach the loader through OpenSettings [opc][limits][presets] [unit] [opc-validation]")
    {
        ExyokiOffice::Packaging::OpenSettings settings;
        settings.PackageLimits = ExyokiOffice::OpenXmlPackageLimits::Recommended();

        auto document = ExyokiOffice::Packaging::WordDocument::Open(BuildValidPackage(), settings);
        CHECK(document != nullptr);

        // Tightening one value from the preset is enough to reject the package,
        // which proves the preset is what the loader actually enforced.
        settings.PackageLimits.MaxEntries = 1;
        CHECK(ExyokiOffice::Packaging::WordDocument::Open(BuildValidPackage(), settings) == nullptr);
    }

    TEST_CASE("SetDefaultPackageLimits arms packages constructed afterwards [opc][limits][default] [unit] [opc-validation]")
    {
        // Process-wide state, so it is restored before leaving: every later test
        // in this binary would otherwise run under whatever this one set.
        // The configured value, not DefaultPackageLimits(): restoring the latter
        // would leave the process configured-as-Unlimited where it had been
        // unconfigured, and the Tools layer tells those two apart.
        const auto previous = ExyokiOffice::OpenXmlPackage::ConfiguredDefaultPackageLimits();
        struct Restore
        {
            std::optional<ExyokiOffice::OpenXmlPackageLimits> Value;
            ~Restore() { ExyokiOffice::OpenXmlPackage::SetDefaultPackageLimits(Value); }
        } const restore{previous};

        ExyokiOffice::OpenXmlPackageLimits limits;
        limits.MaxEntries = 1;
        ExyokiOffice::OpenXmlPackage::SetDefaultPackageLimits(limits);
        CHECK(ExyokiOffice::OpenXmlPackage::DefaultPackageLimits().MaxEntries == 1);

        // Constructed after the call, so it starts with the default and rejects
        // a package nobody passed it any limits for.
        ExyokiOffice::OpenXmlPackage constrained;
        CHECK(constrained.PackageLimits().MaxEntries == 1);
        CHECK_FALSE(constrained.LoadFromMemory(BuildValidPackage()));

        // And an explicit call still wins: the default is the fallback, not an
        // override of what the caller asked for.
        constrained.SetPackageLimits(ExyokiOffice::OpenXmlPackageLimits::Unlimited());
        CHECK(constrained.LoadFromMemory(BuildValidPackage()));
    }

    TEST_CASE("SetDefaultPackageLimits reaches the document factories [opc][limits][default] [unit] [opc-validation]")
    {
        // The regression this pins down: OpenSettings::PackageLimits used to be
        // all zeros by default, and every factory assigns it over whatever the
        // package constructor picked up. A configured policy therefore survived
        // OpenXmlPackage and died at WordDocument::Open, which is the door the
        // Tools entry points and the MCP model tools all go through.
        // Built before the policy is installed, so the packages themselves are
        // ordinary output of this library rather than anything crafted.
        auto workbook = ExyokiOffice::Packaging::ExcelDocument::Create();
        REQUIRE(workbook);
        REQUIRE(workbook->InitDocument());
        const auto spreadsheetBytes = workbook->SaveToMemory();
        REQUIRE(!spreadsheetBytes.empty());

        auto presentation = ExyokiOffice::Packaging::PowerPointDocument::Create();
        REQUIRE(presentation);
        REQUIRE(presentation->InitDocument());
        const auto presentationBytes = presentation->SaveToMemory();
        REQUIRE(!presentationBytes.empty());

        // Both open without a policy in force, so the refusals below are the
        // policy rather than anything about the packages.
        REQUIRE(ExyokiOffice::Packaging::ExcelDocument::Open(spreadsheetBytes) != nullptr);
        REQUIRE(ExyokiOffice::Packaging::PowerPointDocument::Open(presentationBytes) != nullptr);

        const auto previous = ExyokiOffice::OpenXmlPackage::ConfiguredDefaultPackageLimits();
        struct Restore
        {
            std::optional<ExyokiOffice::OpenXmlPackageLimits> Value;
            ~Restore() { ExyokiOffice::OpenXmlPackage::SetDefaultPackageLimits(Value); }
        } const restore{previous};

        ExyokiOffice::OpenXmlPackageLimits limits;
        limits.MaxEntries = 1;
        ExyokiOffice::OpenXmlPackage::SetDefaultPackageLimits(limits);

        CHECK(ExyokiOffice::Packaging::WordDocument::Open(BuildValidPackage()) == nullptr);
        CHECK(ExyokiOffice::Packaging::ExcelDocument::Open(spreadsheetBytes) == nullptr);
        CHECK(ExyokiOffice::Packaging::PowerPointDocument::Open(presentationBytes) == nullptr);

        // An explicit Unlimited() at the open site still wins, or the escape
        // hatch `--package-limits unlimited` relies on would not be one.
        ExyokiOffice::Packaging::OpenSettings unlimited;
        unlimited.PackageLimits = ExyokiOffice::OpenXmlPackageLimits::Unlimited();
        CHECK(ExyokiOffice::Packaging::WordDocument::Open(BuildValidPackage(), unlimited) != nullptr);
    }

    TEST_CASE("limit checks report the rule that was broken [opc][limits][checks] [unit] [opc-validation]")
    {
        // The loader and Tools::Unpack share these, so they are worth pinning
        // down on their own rather than only through a package that trips them.
        ExyokiOffice::OpenXmlPackageLimits limits;
        limits.MaxPartBytes = 100;
        limits.MaxCompressionRatio = 10;
        limits.MaxEntries = 3;
        limits.MaxCompressedBytes = 50;
        limits.MaxUncompressedBytes = 200;

        CHECK(limits.CheckEntry(100, 10).empty());
        CHECK(limits.CheckEntry(101, 50) == "ZIP entry exceeds configured part size limit");
        CHECK(limits.CheckEntry(100, 9) == "ZIP entry exceeds configured compression ratio limit");
        // Nothing to divide by, and no honest entry is stored that way.
        CHECK(limits.CheckEntry(1, 0) == "ZIP entry exceeds configured compression ratio limit");
        // An empty entry has no ratio to exceed.
        CHECK(limits.CheckEntry(0, 0).empty());
        // The ratio on its own, with no size limit to fire first.
        ExyokiOffice::OpenXmlPackageLimits ratioOnly;
        ratioOnly.MaxCompressionRatio = 10;
        CHECK(ratioOnly.CheckEntry(100, 10).empty());
        // A fractional excess is still an excess: integer division reads 101/10
        // as exactly 10 and lets it through, though the real ratio is 10.1.
        CHECK(ratioOnly.CheckEntry(101, 10) == "ZIP entry exceeds configured compression ratio limit");
        // An entry so large that compressed * ratio cannot be represented
        // cannot exceed the ratio, and must not be rejected as if it had.
        CHECK(ratioOnly.CheckEntry(std::numeric_limits<ExyokiOffice::UInt64>::max(),
                                   std::numeric_limits<ExyokiOffice::UInt64>::max() / 2)
                  .empty());

        CHECK(limits.CheckEntryCount(3).empty());
        CHECK(limits.CheckEntryCount(4) == "ZIP package exceeds configured entry count limit");

        CHECK(limits.CheckTotals(50, 200).empty());
        CHECK(limits.CheckTotals(51, 200) == "ZIP package exceeds configured compressed size limit");
        CHECK(limits.CheckTotals(50, 201) == "ZIP package exceeds configured uncompressed size limit");

        // A zero limit is the "unlimited" sentinel and must never refuse.
        const auto unlimited = ExyokiOffice::OpenXmlPackageLimits::Unlimited();
        CHECK(unlimited.CheckEntry(1ull << 40, 1).empty());
        CHECK(unlimited.CheckEntryCount(1ull << 40).empty());
        CHECK(unlimited.CheckTotals(1ull << 40, 1ull << 40).empty());
    }

    TEST_CASE("package entry count limit rejects oversized ZIP directory [opc][limits][entries] [unit] [opc-validation]")
    {
        ExyokiOffice::OpenXmlPackage package;
        ExyokiOffice::OpenXmlPackageLimits limits;
        limits.MaxEntries = 2;
        package.SetPackageLimits(limits);

        CHECK_FALSE(package.LoadFromMemory(BuildValidPackage()));

        const auto issues = FindIssues(package.LastValidationResult(), ExyokiOffice::ValidationErrorId::OpcLimitExceeded);
        REQUIRE(issues.size() == 1);
        CHECK(issues.front().Domain == ExyokiOffice::ValidationDomain::Opc);
    }

    TEST_CASE("package part byte limit rejects large entry before materialization [opc][limits][part-size] [unit] [opc-validation]")
    {
        ExyokiOffice::OpenXmlPackage package;
        ExyokiOffice::OpenXmlPackageLimits limits;
        limits.MaxPartBytes = 64;
        package.SetPackageLimits(limits);

        CHECK_FALSE(package.LoadFromMemory(BuildValidPackage()));

        const auto issues = FindIssues(package.LastValidationResult(), ExyokiOffice::ValidationErrorId::OpcLimitExceeded);
        REQUIRE(issues.size() == 1);
        CHECK(issues.front().Domain == ExyokiOffice::ValidationDomain::Opc);
    }

    TEST_CASE("relationship count limit rejects excessive relationship graph [opc][limits][relationships] [unit] [opc-validation]")
    {
        ExyokiOffice::OpenXmlPackage package;
        ExyokiOffice::OpenXmlPackageLimits limits;
        limits.MaxRelationships = 1;
        package.SetPackageLimits(limits);

        CHECK_FALSE(package.LoadFromMemory(BuildPackageWithTwoRelationshipsInOnePart()));

        const auto issues = FindIssues(package.LastValidationResult(), ExyokiOffice::ValidationErrorId::OpcLimitExceeded);
        REQUIRE(issues.size() == 1);
        CHECK(issues.front().Domain == ExyokiOffice::ValidationDomain::Opc);
    }

    TEST_CASE("relationship count limit applies per relationships part [opc][limits][relationships] [unit] [opc-validation]")
    {
        // Two .rels parts with one relationship each: the header promises a
        // limit on "any one relationships part", so a limit of one has to
        // accept this package rather than reject it on the package-wide total.
        ExyokiOffice::OpenXmlPackage package;
        ExyokiOffice::OpenXmlPackageLimits limits;
        limits.MaxRelationships = 1;
        package.SetPackageLimits(limits);

        CHECK(package.LoadFromMemory(BuildValidPackage()));
        CHECK(FindIssues(package.LastValidationResult(), ExyokiOffice::ValidationErrorId::OpcLimitExceeded).empty());
    }

    TEST_CASE("XML depth limit rejects deeply nested part [opc][limits][xml-depth] [unit] [opc-validation]")
    {
        ExyokiOffice::OpenXmlPackage package;
        ExyokiOffice::OpenXmlPackageLimits limits;
        limits.MaxXmlDepth = 2;
        package.SetPackageLimits(limits);

        const auto packageBytes = BuildPackageWithDocumentXml(
            R"(<?xml version="1.0" encoding="UTF-8"?>
<w:document xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main"><w:body><w:p/></w:body></w:document>)");

        CHECK_FALSE(package.LoadFromMemory(packageBytes));

        const auto issues = FindIssues(package.LastValidationResult(), ExyokiOffice::ValidationErrorId::XmlLimitExceeded);
        REQUIRE(issues.size() == 1);
        CHECK(issues.front().Domain == ExyokiOffice::ValidationDomain::Xml);
        CHECK(issues.front().PartUri == "/word/document.xml");
    }

    TEST_CASE("XML node count limit rejects dense part [opc][limits][xml-nodes] [unit] [opc-validation]")
    {
        std::string xml =
            R"(<?xml version="1.0" encoding="UTF-8"?><w:document xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main"><w:body>)";
        for (int i = 0; i < 25; ++i)
        {
            xml += "<w:p/>";
        }
        xml += "</w:body></w:document>";

        ExyokiOffice::OpenXmlPackage package;
        ExyokiOffice::OpenXmlPackageLimits limits;
        limits.MaxXmlNodes = 20;
        package.SetPackageLimits(limits);

        CHECK_FALSE(package.LoadFromMemory(BuildPackageWithDocumentXml(xml)));

        const auto issues = FindIssues(package.LastValidationResult(), ExyokiOffice::ValidationErrorId::XmlLimitExceeded);
        REQUIRE(issues.size() == 1);
        CHECK(issues.front().PartUri == "/word/document.xml");
    }

    TEST_CASE("XML attribute limit rejects attribute-heavy part [opc][limits][xml-attributes] [unit] [opc-validation]")
    {
        std::string xml =
            R"(<?xml version="1.0" encoding="UTF-8"?><w:document xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main"><w:body><w:p)";
        for (int i = 0; i < 25; ++i)
        {
            xml += " a";
            xml += std::to_string(i);
            xml += "=\"v\"";
        }
        xml += "/></w:body></w:document>";

        ExyokiOffice::OpenXmlPackage package;
        ExyokiOffice::OpenXmlPackageLimits limits;
        limits.MaxXmlAttributes = 20;
        package.SetPackageLimits(limits);

        CHECK_FALSE(package.LoadFromMemory(BuildPackageWithDocumentXml(xml)));

        const auto issues = FindIssues(package.LastValidationResult(), ExyokiOffice::ValidationErrorId::XmlLimitExceeded);
        REQUIRE(issues.size() == 1);
        CHECK(issues.front().PartUri == "/word/document.xml");
    }

    TEST_CASE("XML text limit rejects text-heavy part [opc][limits][xml-text] [unit] [opc-validation]")
    {
        ExyokiOffice::OpenXmlPackage package;
        ExyokiOffice::OpenXmlPackageLimits limits;
        limits.MaxXmlTextCharacters = 8;
        package.SetPackageLimits(limits);

        const auto packageBytes = BuildPackageWithDocumentXml(
            R"(<?xml version="1.0" encoding="UTF-8"?>
<w:document xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main"><w:body><w:p>too much text</w:p></w:body></w:document>)");

        CHECK_FALSE(package.LoadFromMemory(packageBytes));

        const auto issues = FindIssues(package.LastValidationResult(), ExyokiOffice::ValidationErrorId::XmlLimitExceeded);
        REQUIRE(issues.size() == 1);
        CHECK(issues.front().PartUri == "/word/document.xml");
    }

    TEST_CASE("cancellation token prevents open and save side effects [opc][cancellation] [unit] [opc-validation]")
    {
        const AlwaysCancelledToken token;
        const auto packageBytes = BuildValidPackage();

        ExyokiOffice::Packaging::OpenSettings settings;
        auto document = ExyokiOffice::Packaging::WordDocument::Open(packageBytes, settings, &token);
        CHECK(document == nullptr);

        ExyokiOffice::OpenXmlPackage package;
        REQUIRE(package.LoadFromMemory(packageBytes));
        CHECK(package.SaveToMemory(&token).empty());

        const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto targetPath = ExyokiOfficeTests::MakeTemporaryPath("ExyokiOfficeCancellationSave_" + std::to_string(suffix), ".docx");
        std::filesystem::remove(targetPath);

        CHECK_FALSE(package.SaveToFile(targetPath, false, &token));
        CHECK_FALSE(std::filesystem::exists(targetPath));
        std::filesystem::remove(targetPath);
    }

} // TEST_SUITE("OpcValidationTests")
