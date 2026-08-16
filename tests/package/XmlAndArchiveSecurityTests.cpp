// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "doctest.h"

#include "TestSupport.hpp"

#include "ExyokiOffice/OpenXmlPackage.hpp"
#include "ExyokiOffice/Tools/PackageArchiver.hpp"
#include "zip/zip.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{

class XmlAndArchiveSecurityTestHelper final
{
public:
    struct LoadResult
    {
        bool Loaded = false;
        std::string SerializedDocument;
    };

    static std::vector<ExyokiOffice::Byte> BuildWordPackage(std::string_view documentXml,
                                                            int compressionLevel = 0)
    {
        return BuildArchive(
            {{"[Content_Types].xml",
              R"(<?xml version="1.0" encoding="UTF-8"?>
<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">
  <Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>
  <Default Extension="xml" ContentType="application/xml"/>
  <Override PartName="/word/document.xml" ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml"/>
</Types>)"},
             {"_rels/.rels",
              R"(<?xml version="1.0" encoding="UTF-8"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">
  <Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="word/document.xml"/>
</Relationships>)"},
             {"word/document.xml", std::string(documentXml)}},
            compressionLevel);
    }

    static std::vector<ExyokiOffice::Byte>
    BuildArchive(std::initializer_list<std::pair<std::string, std::string>> entries,
                 int compressionLevel = ZIP_DEFAULT_COMPRESSION_LEVEL)
    {
        auto* archive = zip_stream_open(nullptr, 0, compressionLevel, 'w');
        REQUIRE(archive != nullptr);

        for (const auto& [name, content] : entries)
        {
            REQUIRE(zip_entry_open(archive, name.c_str()) == 0);
            CHECK(zip_entry_write(archive, content.data(), content.size()) == 0);
            CHECK(zip_entry_close(archive) == 0);
        }

        void* rawBuffer = nullptr;
        ExyokiOffice::Size rawSize = 0;
        REQUIRE(zip_stream_copy(archive, &rawBuffer, &rawSize) > 0);
        zip_stream_close(archive);
        REQUIRE(rawBuffer != nullptr);

        const auto* begin = static_cast<const ExyokiOffice::Byte*>(rawBuffer);
        std::vector<ExyokiOffice::Byte> result(begin, begin + rawSize);
        std::free(rawBuffer);
        return result;
    }

    static LoadResult LoadDocument(std::string_view documentXml)
    {
        ExyokiOffice::OpenXmlPackage package;
        package.SetPackageLimits(ExyokiOffice::OpenXmlPackageLimits::Recommended());

        LoadResult result;
        result.Loaded = package.LoadFromMemory(BuildWordPackage(documentXml));
        if (result.Loaded)
        {
            const auto part = package.GetPartByUri("/word/document.xml");
            REQUIRE(part != nullptr);
            result.SerializedDocument = part->GetXmlString();
        }
        return result;
    }

    static void WriteTextFile(const std::filesystem::path& path, std::string_view text)
    {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        REQUIRE(file.is_open());
        file.write(text.data(), static_cast<std::streamsize>(text.size()));
        REQUIRE(file.good());
    }

    static void WriteBinaryFile(const std::filesystem::path& path,
                                const std::vector<ExyokiOffice::Byte>& bytes)
    {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        REQUIRE(file.is_open());
        file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        REQUIRE(file.good());
    }

    static std::string ToFileUri(const std::filesystem::path& path)
    {
        const auto absolute = std::filesystem::absolute(path).generic_string();
        std::string encoded;
        encoded.reserve(absolute.size());
        for (const unsigned char ch : absolute)
        {
            const bool unreserved = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                                    (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.' ||
                                    ch == '~' || ch == '/' || ch == ':';
            if (unreserved)
            {
                encoded.push_back(static_cast<char>(ch));
            }
            else
            {
                static constexpr char kHex[] = "0123456789ABCDEF";
                encoded.push_back('%');
                encoded.push_back(kHex[ch >> 4]);
                encoded.push_back(kHex[ch & 0x0f]);
            }
        }

        if (encoded.size() >= 2 && encoded[1] == ':')
        {
            return "file:///" + encoded;
        }
        return "file://" + encoded;
    }

    static bool HasValidationIssue(const ExyokiOffice::ValidationResult& result,
                                   ExyokiOffice::ValidationErrorId id)
    {
        return std::any_of(result.Issues().begin(), result.Issues().end(),
                           [id](const auto& issue)
                           { return issue.Id == id; });
    }

    static bool HasDiagnostic(const std::vector<ExyokiOffice::Tools::ToolDiagnostic>& diagnostics,
                              std::string_view message)
    {
        return std::any_of(diagnostics.begin(), diagnostics.end(), [message](const auto& diagnostic)
                           { return diagnostic.Message.find(message) != std::string::npos; });
    }
};

constexpr std::string_view kWordNamespace =
    "http://schemas.openxmlformats.org/wordprocessingml/2006/main";
constexpr std::string_view kSecret = "EXYOKI_XXE_SECRET_7D0A6C3E";

} // namespace

TEST_SUITE("XmlAndArchiveSecurityTests")
{
    TEST_CASE("external general entities cannot read local files [security-regression]")
    {
        const auto canaryPath = ExyokiOfficeTests::MakeTemporaryPath("xxe-canary", ".txt");
        XmlAndArchiveSecurityTestHelper::WriteTextFile(canaryPath, kSecret);

        const auto xml = std::string("<?xml version=\"1.0\"?>\n<!DOCTYPE w:document [<!ENTITY xxe SYSTEM \"") +
                         XmlAndArchiveSecurityTestHelper::ToFileUri(canaryPath) +
                         "\">]>\n<w:document xmlns:w=\"" + std::string(kWordNamespace) +
                         "\"><w:body><w:p>&xxe;</w:p></w:body></w:document>";

        const auto result = XmlAndArchiveSecurityTestHelper::LoadDocument(xml);
        REQUIRE(result.Loaded);
        CHECK(result.SerializedDocument.find(kSecret) == std::string::npos);
    }

    TEST_CASE("external DTDs and parameter entities cannot inject declarations [security-regression]")
    {
        const auto dtdPath = ExyokiOfficeTests::MakeTemporaryPath("xxe-external", ".dtd");
        XmlAndArchiveSecurityTestHelper::WriteTextFile(
            dtdPath, std::string("<!ENTITY stolen \"") + std::string(kSecret) + "\">");
        const auto dtdUri = XmlAndArchiveSecurityTestHelper::ToFileUri(dtdPath);

        SUBCASE("external subset")
        {
            const auto xml = std::string("<?xml version=\"1.0\"?>\n<!DOCTYPE w:document SYSTEM \"") + dtdUri +
                             "\">\n<w:document xmlns:w=\"" + std::string(kWordNamespace) +
                             "\"><w:body><w:p>&stolen;</w:p></w:body></w:document>";
            const auto result = XmlAndArchiveSecurityTestHelper::LoadDocument(xml);
            REQUIRE(result.Loaded);
            CHECK(result.SerializedDocument.find(kSecret) == std::string::npos);
        }

        SUBCASE("external parameter entity")
        {
            const auto xml = std::string("<?xml version=\"1.0\"?>\n<!DOCTYPE w:document [<!ENTITY % remote SYSTEM \"") +
                             dtdUri + "\">%remote;]>\n<w:document xmlns:w=\"" + std::string(kWordNamespace) +
                             "\"><w:body><w:p>&stolen;</w:p></w:body></w:document>";
            const auto result = XmlAndArchiveSecurityTestHelper::LoadDocument(xml);
            REQUIRE(result.Loaded);
            CHECK(result.SerializedDocument.find(kSecret) == std::string::npos);
        }
    }

    TEST_CASE("XInclude stylesheets and schema locations are never fetched [security-regression]")
    {
        const auto canaryPath = ExyokiOfficeTests::MakeTemporaryPath("xml-reference-canary", ".xml");
        XmlAndArchiveSecurityTestHelper::WriteTextFile(canaryPath, kSecret);
        const auto uri = XmlAndArchiveSecurityTestHelper::ToFileUri(canaryPath);

        const auto xml = std::string("<?xml version=\"1.0\"?>\n<?xml-stylesheet type=\"text/xsl\" href=\"") + uri +
                         "\"?>\n<w:document xmlns:w=\"" + std::string(kWordNamespace) +
                         "\" xmlns:xi=\"http://www.w3.org/2001/XInclude\" "
                         "xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" "
                         "xsi:schemaLocation=\"urn:exyoki:test " +
                         uri +
                         "\"><w:body><xi:include href=\"" + uri +
                         "\" parse=\"text\"/></w:body></w:document>";

        const auto result = XmlAndArchiveSecurityTestHelper::LoadDocument(xml);
        REQUIRE(result.Loaded);
        CHECK(result.SerializedDocument.find(kSecret) == std::string::npos);
        CHECK(result.SerializedDocument.find("xi:include") != std::string::npos);
    }

    TEST_CASE("Billion Laughs entity expansion remains inert [security-regression]")
    {
        std::string xml = "<?xml version=\"1.0\"?>\n<!DOCTYPE w:document [\n<!ENTITY laugh0 \"LAUGH_0\">\n";
        for (int level = 1; level <= 6; ++level)
        {
            xml += "<!ENTITY laugh" + std::to_string(level) + " \"";
            for (int repeat = 0; repeat < 10; ++repeat)
            {
                xml += "&laugh" + std::to_string(level - 1) + ";";
            }
            xml += "\">\n";
        }
        xml += "]>\n<w:document xmlns:w=\"" + std::string(kWordNamespace) +
               "\"><w:body><w:p>&laugh6;</w:p></w:body></w:document>";

        const auto result = XmlAndArchiveSecurityTestHelper::LoadDocument(xml);
        REQUIRE(result.Loaded);
        CHECK(result.SerializedDocument.size() < 64 * 1024);
        CHECK(result.SerializedDocument.find("LAUGH_0LAUGH_0") == std::string::npos);
    }

    TEST_CASE("quadratic entity expansion remains inert [security-regression]")
    {
        std::string xml = "<?xml version=\"1.0\"?>\n<!DOCTYPE w:document [<!ENTITY block \"";
        xml.append(2048, 'Q');
        xml += "\">]>\n<w:document xmlns:w=\"" + std::string(kWordNamespace) + "\"><w:body><w:p>";
        for (int repeat = 0; repeat < 2048; ++repeat)
        {
            xml += "&block;";
        }
        xml += "</w:p></w:body></w:document>";

        const auto result = XmlAndArchiveSecurityTestHelper::LoadDocument(xml);
        REQUIRE(result.Loaded);
        CHECK(result.SerializedDocument.size() < 128 * 1024);
        CHECK(result.SerializedDocument.find(std::string(4096, 'Q')) == std::string::npos);
    }

    TEST_CASE("recommended limits stop extreme XML recursion [security-regression]")
    {
        std::string xml = "<?xml version=\"1.0\"?><w:document xmlns:w=\"" + std::string(kWordNamespace) +
                          "\"><w:body>";
        for (int depth = 0; depth < 10'000; ++depth)
        {
            xml += "<n>";
        }
        for (int depth = 0; depth < 10'000; ++depth)
        {
            xml += "</n>";
        }
        xml += "</w:body></w:document>";

        ExyokiOffice::OpenXmlPackage package;
        package.SetPackageLimits(ExyokiOffice::OpenXmlPackageLimits::Recommended());
        CHECK_FALSE(package.LoadFromMemory(XmlAndArchiveSecurityTestHelper::BuildWordPackage(xml)));
        CHECK(XmlAndArchiveSecurityTestHelper::HasValidationIssue(
            package.LastValidationResult(), ExyokiOffice::ValidationErrorId::XmlLimitExceeded));
    }

    TEST_CASE("part and node limits bound XML DOM memory bombs [security-regression]")
    {
        SUBCASE("oversized XML is rejected from ZIP metadata before DOM construction")
        {
            std::string xml = "<?xml version=\"1.0\"?><w:document xmlns:w=\"" + std::string(kWordNamespace) +
                              "\"><w:body>";
            xml.append(2 * 1024 * 1024, 'X');
            xml += "</w:body></w:document>";

            auto limits = ExyokiOffice::OpenXmlPackageLimits::Recommended();
            limits.MaxPartBytes = 1024 * 1024;
            ExyokiOffice::OpenXmlPackage package;
            package.SetPackageLimits(limits);
            CHECK_FALSE(package.LoadFromMemory(XmlAndArchiveSecurityTestHelper::BuildWordPackage(xml)));
            CHECK(XmlAndArchiveSecurityTestHelper::HasValidationIssue(
                package.LastValidationResult(), ExyokiOffice::ValidationErrorId::OpcLimitExceeded));
        }

        SUBCASE("large sibling fan-out is rejected by the DOM node ceiling")
        {
            std::string xml = "<?xml version=\"1.0\"?><w:document xmlns:w=\"" + std::string(kWordNamespace) +
                              "\"><w:body>";
            for (int node = 0; node < 200'000; ++node)
            {
                xml += "<n/>";
            }
            xml += "</w:body></w:document>";

            auto limits = ExyokiOffice::OpenXmlPackageLimits::Recommended();
            limits.MaxXmlNodes = 100'000;
            ExyokiOffice::OpenXmlPackage package;
            package.SetPackageLimits(limits);
            CHECK_FALSE(package.LoadFromMemory(XmlAndArchiveSecurityTestHelper::BuildWordPackage(xml)));
            CHECK(XmlAndArchiveSecurityTestHelper::HasValidationIssue(
                package.LastValidationResult(), ExyokiOffice::ValidationErrorId::XmlLimitExceeded));
        }
    }

    TEST_CASE("recommended entry ceiling rejects million-entry ZIP metadata [security-regression]")
    {
        const auto limits = ExyokiOffice::OpenXmlPackageLimits::Recommended();
        CHECK(limits.CheckEntryCount(limits.MaxEntries).empty());
        CHECK(limits.CheckEntryCount(limits.MaxEntries + 1) ==
              "ZIP package exceeds configured entry count limit");
        CHECK(limits.CheckEntryCount(1'000'000) ==
              "ZIP package exceeds configured entry count limit");
    }

    TEST_CASE("recommended limits stop a compressed ZIP bomb before extraction [security-regression]")
    {
        std::string xml = "<?xml version=\"1.0\"?><w:document xmlns:w=\"" + std::string(kWordNamespace) +
                          "\"><w:body><w:p>";
        xml.append(2 * 1024 * 1024, 'A');
        xml += "</w:p></w:body></w:document>";
        const auto bytes = XmlAndArchiveSecurityTestHelper::BuildWordPackage(xml, ZIP_DEFAULT_COMPRESSION_LEVEL);

        ExyokiOffice::OpenXmlPackage package;
        package.SetPackageLimits(ExyokiOffice::OpenXmlPackageLimits::Recommended());
        CHECK_FALSE(package.LoadFromMemory(bytes));
        CHECK(XmlAndArchiveSecurityTestHelper::HasValidationIssue(
            package.LastValidationResult(), ExyokiOffice::ValidationErrorId::OpcLimitExceeded));

        const auto archivePath = ExyokiOfficeTests::MakeTemporaryPath("zip-bomb", ".docx");
        const auto outputPath = ExyokiOfficeTests::MakeTemporaryPath("zip-bomb-output", "");
        XmlAndArchiveSecurityTestHelper::WriteBinaryFile(archivePath, bytes);
        ExyokiOffice::Tools::UnpackOptions options;
        options.Limits = ExyokiOffice::OpenXmlPackageLimits::Recommended();
        const auto unpacked = ExyokiOffice::Tools::Unpack(archivePath, outputPath, options);
        CHECK_FALSE(unpacked.Ok);
        CHECK(XmlAndArchiveSecurityTestHelper::HasDiagnostic(unpacked.Diagnostics, "compression ratio"));
        CHECK_FALSE(std::filesystem::exists(outputPath / "word" / "document.xml"));
    }

    TEST_CASE("archive extraction rejects Zip Slip traversal entries [security-regression]")
    {
        const auto archivePath = ExyokiOfficeTests::MakeTemporaryPath("zip-slip", ".zip");
        const auto outputPath = ExyokiOfficeTests::MakeTemporaryPath("zip-slip-output", "");
        const auto outsidePath = ExyokiOfficeTests::MakeTemporaryPath("zip-slip-outside", ".txt");
        const auto traversalName = std::string("../") + outsidePath.filename().generic_string();
        const auto bytes = XmlAndArchiveSecurityTestHelper::BuildArchive(
            {{"safe.txt", "safe"}, {traversalName, "escaped"}}, 0);
        XmlAndArchiveSecurityTestHelper::WriteBinaryFile(archivePath, bytes);

        ExyokiOffice::Tools::UnpackOptions options;
        options.Limits = ExyokiOffice::OpenXmlPackageLimits::Recommended();
        const auto unpacked = ExyokiOffice::Tools::Unpack(archivePath, outputPath, options);

        CHECK(unpacked.Ok);
        CHECK(unpacked.EntryCount == 1);
        CHECK(XmlAndArchiveSecurityTestHelper::HasDiagnostic(unpacked.Diagnostics, "path traversal"));
        CHECK(std::filesystem::exists(outputPath / "safe.txt"));
        CHECK_FALSE(std::filesystem::exists(outsidePath));
    }
}
