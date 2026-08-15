// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "ExyokiOffice/Excel/ExcelDocument.hpp"
#include "doctest.h"
#include "zip/zip.h"
#include "ExyokiOffice/StandardTypes.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <map>
#include <span>
#include <string_view>

namespace
{
using Bytes = std::vector<ExyokiOffice::Byte>;
struct ZipEntry
{
    const char* name;
    std::string_view data;
};

/// Stand-in VBA project; the payload carries embedded NULs, so the length cannot
/// be derived from the string_view constructor and comes from the array instead.
constexpr char kVbaProject[] = "\x00\xD0\xCF\x11\xE0\xA1\xB1\x1A\x00VBA\xFFpayload";

constexpr std::array<ZipEntry, 8> kAdvancedParts = {{{"xl/pivotTables/pivotTable1.xml",
                                                      R"(<pivotTableDefinition xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main" name="SalesPivot" cacheId="1"><location ref="A1:C9" firstHeaderRow="1" firstDataRow="1" firstDataCol="1"/></pivotTableDefinition>)"},
                                                     {"xl/pivotCache/pivotCacheDefinition1.xml",
                                                      R"(<pivotCacheDefinition xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main" saveData="1" recordCount="2"><cacheSource type="worksheet"><worksheetSource ref="A1:C9" sheet="Sheet1"/></cacheSource><cacheFields count="1"><cacheField name="Region"><sharedItems count="2"><s v="West"/><s v="East"/></sharedItems></cacheField></cacheFields></pivotCacheDefinition>)"},
                                                     {"xl/pivotCache/pivotCacheRecords1.xml",
                                                      R"(<pivotCacheRecords xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main" count="2"><r><x v="0"/></r><r><x v="1"/></r></pivotCacheRecords>)"},
                                                     {"xl/charts/chart1.xml",
                                                      R"(<c:chartSpace xmlns:c="http://schemas.openxmlformats.org/drawingml/2006/chart"><c:chart><c:autoTitleDeleted val="0"/><c:plotArea/></c:chart></c:chartSpace>)"},
                                                     {"xl/slicers/slicer1.xml",
                                                      R"(<x14:slicers xmlns:x14="http://schemas.microsoft.com/office/spreadsheetml/2009/9/main"><x14:slicer name="RegionSlicer" cache="Slicer_Region"/></x14:slicers>)"},
                                                     {"xl/externalLinks/externalLink1.xml",
                                                      R"(<externalLink xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main"><externalBook xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships" r:id="rId1"><sheetNames><sheetName val="Remote"/></sheetNames></externalBook></externalLink>)"},
                                                     {"xl/connections.xml",
                                                      R"(<connections xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main" count="1"><connection id="1" name="Warehouse" type="5" refreshedVersion="8"><dbPr connection="Provider=Example" command="SELECT 1" commandType="2"/></connection></connections>)"},
                                                     {"xl/vbaProject.bin", std::string_view(kVbaProject, sizeof(kVbaProject) - 1)}}};

void Put(zip_t* zip, const char* name, std::string_view data)
{
    REQUIRE(zip_entry_open(zip, name) == 0);
    REQUIRE(zip_entry_write(zip, data.data(), data.size()) == 0);
    REQUIRE(zip_entry_close(zip) == 0);
}

Bytes Finish(zip_t* zip)
{
    void* raw = nullptr;
    ExyokiOffice::Size size = 0;
    REQUIRE(zip_stream_copy(zip, &raw, &size) > 0);
    zip_stream_close(zip);
    const auto* begin = static_cast<const ExyokiOffice::UInt8*>(raw);
    Bytes result(begin, begin + size);
    std::free(raw);
    return result;
}

Bytes Fixture()
{
    auto* zip = zip_stream_open(nullptr, 0, ZIP_DEFAULT_COMPRESSION_LEVEL, 'w');
    REQUIRE(zip);
    Put(zip,
        "[Content_Types].xml", R"(<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types"><Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/><Default Extension="xml" ContentType="application/xml"/><Default Extension="bin" ContentType="application/vnd.ms-office.vbaProject"/><Override PartName="/xl/workbook.xml" ContentType="application/vnd.ms-excel.sheet.macroEnabled.main+xml"/><Override PartName="/xl/worksheets/sheet1.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml"/><Override PartName="/xl/pivotTables/pivotTable1.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.pivotTable+xml"/><Override PartName="/xl/pivotCache/pivotCacheDefinition1.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.pivotCacheDefinition+xml"/><Override PartName="/xl/pivotCache/pivotCacheRecords1.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.pivotCacheRecords+xml"/><Override PartName="/xl/charts/chart1.xml" ContentType="application/vnd.openxmlformats-officedocument.drawingml.chart+xml"/><Override PartName="/xl/slicers/slicer1.xml" ContentType="application/vnd.ms-excel.slicer+xml"/><Override PartName="/xl/externalLinks/externalLink1.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.externalLink+xml"/><Override PartName="/xl/connections.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.connections+xml"/></Types>)");
    Put(zip, "_rels/.rels",
        R"(<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships"><Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="xl/workbook.xml"/></Relationships>)");
    Put(zip, "xl/workbook.xml",
        R"(<workbook xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main" xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships"><sheets><sheet name="Sheet1" sheetId="1" r:id="rId1"/></sheets><pivotCaches><pivotCache cacheId="1" r:id="rId2"/></pivotCaches></workbook>)");
    Put(zip, "xl/_rels/workbook.xml.rels",
        R"(<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships"><Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet" Target="worksheets/sheet1.xml"/><Relationship Id="rId2" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/pivotCacheDefinition" Target="pivotCache/pivotCacheDefinition1.xml"/><Relationship Id="rId3" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/externalLink" Target="externalLinks/externalLink1.xml"/><Relationship Id="rId4" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/connections" Target="connections.xml"/><Relationship Id="rId5" Type="http://schemas.microsoft.com/office/2006/relationships/vbaProject" Target="vbaProject.bin"/></Relationships>)");
    Put(zip, "xl/worksheets/sheet1.xml",
        R"(<worksheet xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main"><sheetData><row r="1"><c r="A1"><v>7</v></c></row></sheetData></worksheet>)");
    Put(zip, "xl/worksheets/_rels/sheet1.xml.rels",
        R"(<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships"><Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/pivotTable" Target="../pivotTables/pivotTable1.xml"/><Relationship Id="rId2" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/chart" Target="../charts/chart1.xml"/><Relationship Id="rId3" Type="http://schemas.microsoft.com/office/2007/relationships/slicer" Target="../slicers/slicer1.xml"/></Relationships>)");
    Put(zip, "xl/pivotTables/_rels/pivotTable1.xml.rels",
        R"(<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships"><Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/pivotCacheDefinition" Target="../pivotCache/pivotCacheDefinition1.xml"/></Relationships>)");
    Put(zip, "xl/pivotCache/_rels/pivotCacheDefinition1.xml.rels",
        R"(<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships"><Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/pivotCacheRecords" Target="pivotCacheRecords1.xml"/></Relationships>)");
    Put(zip, "xl/externalLinks/_rels/externalLink1.xml.rels",
        R"(<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships"><Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/externalLinkPath" Target="file:///C:/data/source.xlsx" TargetMode="External"/></Relationships>)");
    for (const auto& part : kAdvancedParts)
    {
        Put(zip, part.name, part.data);
    }
    return Finish(zip);
}

Bytes Read(std::span<const ExyokiOffice::Byte> bytes, const char* name)
{
    int error = 0;
    auto* zip = zip_stream_openwitherror(reinterpret_cast<const char*>(bytes.data()), bytes.size(), 0, 'r', &error);
    REQUIRE(zip);
    REQUIRE(zip_entry_open(zip, name) == 0);
    void* raw = nullptr;
    ExyokiOffice::Size size = 0;
    REQUIRE(zip_entry_read(zip, &raw, &size) >= 0);
    zip_entry_close(zip);
    zip_stream_close(zip);
    const auto* begin = static_cast<const ExyokiOffice::UInt8*>(raw);
    Bytes result(begin, begin + size);
    std::free(raw);
    return result;
}

using Manifest = std::map<std::string, std::vector<std::string>>;
void Capture(const ExyokiOffice::OpenXmlPartContainer& container, Manifest& out)
{
    auto& relationships = out[container.ContainerUri()];
    for (const auto& relationship : container.Relationships())
    {
        relationships.push_back(relationship.Id + "|" + relationship.Type + "|" + relationship.Target + "|" +
                                relationship.TargetMode);
    }
    std::sort(relationships.begin(), relationships.end());
    for (const auto& part : container.Parts())
    {
        Capture(*part, out);
    }
}
Manifest Capture(const ExyokiOffice::OpenXmlPackage& package)
{
    Manifest result;
    Capture(package, result);
    return result;
}
bool ContainsManifest(const Manifest& actual, const Manifest& expected)
{
    for (const auto& [source, edges] : expected)
    {
        const auto found = actual.find(source);
        if (found == actual.end())
        {
            return false;
        }
        for (const auto& edge : edges)
        {
            if (std::find(found->second.begin(), found->second.end(), edge) == found->second.end())
            {
                return false;
            }
        }
    }
    return true;
}

std::map<std::string, Bytes> SerializedAdvancedPayloads(const ExyokiOffice::OpenXmlPackage& package)
{
    std::map<std::string, Bytes> result;
    for (const auto& entry : kAdvancedParts)
    {
        auto part = package.GetPartByUri(std::string("/") + entry.name);
        REQUIRE(part != nullptr);
        if (part->IsBinaryPart())
        {
            result[entry.name] = part->GetBinaryData();
        }
        else
        {
            const auto xml = part->GetXmlString();
            result[entry.name] = Bytes(xml.begin(), xml.end());
        }
    }
    return result;
}
} // namespace

TEST_SUITE("ExcelAdvancedPreservationTests")
{
    TEST_CASE("ordinary cell edit preserves advanced payload and relationship manifest [unit] [excel] [preservation]")
    {
        const auto fixture = Fixture();
        auto editor = ExyokiOffice::Excel::ExcelDocumentEditor::Open(fixture);
        REQUIRE(editor);
        REQUIRE(editor->FirstWorksheet());
        const auto manifest = Capture(*editor->GetDocument());
        const auto payloads = SerializedAdvancedPayloads(*editor->GetDocument());
        CHECK(editor->FirstWorksheet()->SetCellText(2, 2, "edited"));
        const auto saved = editor->SaveToMemory();
        REQUIRE_FALSE(saved.empty());
        for (const auto& part : kAdvancedParts)
        {
            CAPTURE(part.name);
            CHECK(Read(saved, part.name) == payloads.at(part.name));
        }
        auto reopened = ExyokiOffice::Excel::ExcelDocumentEditor::Open(saved);
        REQUIRE(reopened);
        CHECK(ContainsManifest(Capture(*reopened->GetDocument()), manifest));
    }

    TEST_CASE("advanced parts remain stable through repeated edit-save-open cycles [unit] [excel] [preservation]")
    {
        auto initial = ExyokiOffice::Excel::ExcelDocumentEditor::Open(Fixture());
        REQUIRE(initial);
        REQUIRE(initial->FirstWorksheet());
        CHECK(initial->FirstWorksheet()->SetCellText(2, 2, "baseline"));
        auto bytes = initial->SaveToMemory();
        REQUIRE_FALSE(bytes.empty());
        auto baseline = ExyokiOffice::Excel::ExcelDocumentEditor::Open(bytes);
        REQUIRE(baseline);
        const auto manifest = Capture(*baseline->GetDocument());
        const auto payloads = SerializedAdvancedPayloads(*baseline->GetDocument());
        for (ExyokiOffice::UInt32 pass = 1; pass <= 3; ++pass)
        {
            auto editor = ExyokiOffice::Excel::ExcelDocumentEditor::Open(bytes);
            REQUIRE(editor);
            REQUIRE(editor->FirstWorksheet());
            CHECK(editor->FirstWorksheet()->SetCellNumber(pass + 2, pass + 2, pass));
            bytes = editor->SaveToMemory();
            REQUIRE_FALSE(bytes.empty());
            auto reopened = ExyokiOffice::Excel::ExcelDocumentEditor::Open(bytes);
            REQUIRE(reopened);
            CHECK(Capture(*reopened->GetDocument()) == manifest);
            for (const auto& part : kAdvancedParts)
            {
                CAPTURE(pass);
                CAPTURE(part.name);
                CHECK(Read(bytes, part.name) == payloads.at(part.name));
            }
        }
    }
}
