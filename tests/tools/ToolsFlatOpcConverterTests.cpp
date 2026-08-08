// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "doctest.h"

#include "TestSupport.hpp"

#include "ExyokiOffice/Tools/FlatOpcConverter.hpp"
#include "ExyokiOffice/Tools/PackageDiff.hpp"
#include "ExyokiOffice/Word/WordDocument.hpp"
#include "zip/zip.h"
#include "ExyokiOffice/StandardTypes.hpp"

#include <chrono>
#include <cstdlib>
#include <span>

using namespace ExyokiOffice::Tools;
using ExyokiOffice::Word::WordDocumentEditor;

class FlatOpcTestHelpers
{
public:
    using Bytes = std::vector<ExyokiOffice::Byte>;

    static std::filesystem::path Temporary(std::string_view stem, std::string_view extension)
    {
        return ExyokiOfficeTests::MakeTemporaryPath(stem, extension);
    }

    static std::shared_ptr<WordDocumentEditor> Document()
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor);
        editor->AddParagraph("Flat OPC round-trip.");
        const std::vector<ExyokiOffice::Byte> png = {
            0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52,
            0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x06, 0x00, 0x00, 0x00, 0x1f, 0x15, 0xc4,
            0x89, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x44, 0x41, 0x54, 0x08, 0xd7, 0x63, 0xf8, 0xcf, 0xc0, 0xf0,
            0x1f, 0x00, 0x05, 0x00, 0x01, 0xff, 0x89, 0x99, 0x3d, 0x1d, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45,
            0x4e, 0x44, 0xae, 0x42, 0x60, 0x82};
        REQUIRE(editor->AddImageFromData(png));
        return editor;
    }

    static void AddZipEntry(zip_t* archive, std::string_view name, std::span<const ExyokiOffice::Byte> bytes)
    {
        REQUIRE(zip_entry_open(archive, std::string(name).c_str()) == 0);
        REQUIRE(zip_entry_write(archive, bytes.data(), bytes.size()) == 0);
        REQUIRE(zip_entry_close(archive) == 0);
    }

    static void AddZipEntry(zip_t* archive, std::string_view name, std::string_view text)
    {
        AddZipEntry(archive, name,
                    std::span(reinterpret_cast<const ExyokiOffice::UInt8*>(text.data()), text.size()));
    }

    static Bytes FinishZip(zip_t* archive)
    {
        void* raw = nullptr;
        ExyokiOffice::Size size = 0;
        REQUIRE(zip_stream_copy(archive, &raw, &size) > 0);
        zip_stream_close(archive);
        const auto* first = static_cast<const ExyokiOffice::UInt8*>(raw);
        Bytes bytes(first, first + size);
        std::free(raw);
        return bytes;
    }

    static Bytes BuildContentTypeFixture()
    {
        auto* archive = zip_stream_open(nullptr, 0, ZIP_DEFAULT_COMPRESSION_LEVEL, 'w');
        REQUIRE(archive != nullptr);
        AddZipEntry(archive, "[Content_Types].xml",
                    R"(<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types"><Default Extension="XML" ContentType="application/default+xml"/><Default Extension="bin" ContentType="application/example-binary"/><Override PartName="/special.xml" ContentType="application/override+xml"/></Types>)");
        AddZipEntry(archive, "ordinary.XML", "<ordinary value=\"yes\"/>");
        AddZipEntry(archive, "special.xml", "<special/>");
        AddZipEntry(archive, "opaque.bin", Bytes{0x00, 0xFF, 0x10, 0x7F});
        AddZipEntry(archive, "unknown.dat", Bytes{0x01, 0x02, 0x03});
        return FinishZip(archive);
    }

    /// A minimal WordprocessingML package whose paragraph spells "one two" as
    /// three runs, the middle one carrying only a space.
    static Bytes BuildWhitespaceRunFixture()
    {
        auto* archive = zip_stream_open(nullptr, 0, ZIP_DEFAULT_COMPRESSION_LEVEL, 'w');
        REQUIRE(archive != nullptr);
        AddZipEntry(archive, "[Content_Types].xml",
                    R"(<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types"><Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/><Override PartName="/word/document.xml" ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml"/></Types>)");
        AddZipEntry(archive, "_rels/.rels",
                    R"(<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships"><Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="word/document.xml"/></Relationships>)");
        AddZipEntry(archive, "word/document.xml",
                    R"(<w:document xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main"><w:body><w:p><w:r><w:t>one</w:t></w:r><w:r><w:t xml:space="preserve"> </w:t></w:r><w:r><w:t>two</w:t></w:r></w:p></w:body></w:document>)");
        return FinishZip(archive);
    }

    /// A VML drawing shaped the way Excel writes one: no XML declaration, and
    /// the start tag broken over three lines. The bytes are well-formed XML,
    /// but the content type makes this a binary part, so the two ways of
    /// classifying it disagree.
    static constexpr std::string_view VmlDrawing =
        "<xml xmlns:v=\"urn:schemas-microsoft-com:vml\"\n"
        " xmlns:o=\"urn:schemas-microsoft-com:office:office\"\n"
        " xmlns:x=\"urn:schemas-microsoft-com:office:excel\">\n"
        " <o:shapelayout v:ext=\"edit\">\n"
        "  <o:idmap v:ext=\"edit\" data=\"1\"/>\n"
        " </o:shapelayout>\n"
        "</xml>\n";

    static Bytes BuildVmlDrawingFixture()
    {
        auto* archive = zip_stream_open(nullptr, 0, ZIP_DEFAULT_COMPRESSION_LEVEL, 'w');
        REQUIRE(archive != nullptr);
        AddZipEntry(archive, "[Content_Types].xml",
                    R"(<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types"><Default Extension="vml" ContentType="application/vnd.openxmlformats-officedocument.vmlDrawing"/></Types>)");
        AddZipEntry(archive, "xl/drawings/vmlDrawing1.vml", VmlDrawing);
        return FinishZip(archive);
    }

    static Bytes ReadZipEntry(std::span<const ExyokiOffice::Byte> package, std::string_view name)
    {
        int error = 0;
        auto* archive = zip_stream_openwitherror(reinterpret_cast<const char*>(package.data()), package.size(), 0, 'r', &error);
        REQUIRE(archive != nullptr);
        REQUIRE(zip_entry_open(archive, std::string(name).c_str()) == 0);
        void* raw = nullptr;
        ExyokiOffice::Size size = 0;
        REQUIRE(zip_entry_read(archive, &raw, &size) >= 0);
        REQUIRE(zip_entry_close(archive) == 0);
        zip_stream_close(archive);
        const auto* first = static_cast<const ExyokiOffice::UInt8*>(raw);
        Bytes bytes(first, first + size);
        std::free(raw);
        return bytes;
    }
};

TEST_CASE("Flat OPC file conversion round-trips semantically [unit] [tools] [flat-opc]")
{
    const auto original = FlatOpcTestHelpers::Temporary("flat_opc_original", ".docx");
    const auto flat = FlatOpcTestHelpers::Temporary("flat_opc", ".xml");
    const auto rebuilt = FlatOpcTestHelpers::Temporary("flat_opc_rebuilt", ".docx");
    REQUIRE(FlatOpcTestHelpers::Document()->SaveToFile(original));
    const auto to = ConvertToFlatOpc(original, flat);
    REQUIRE(to.Ok);
    CHECK(to.PartCount > 0);
    const auto from = ConvertFromFlatOpc(flat, rebuilt);
    REQUIRE(from.Ok);
    const auto diff = Compare(original, rebuilt);
    CHECK(diff.Ok);
    CHECK(diff.Identical);
    std::filesystem::remove(original);
    std::filesystem::remove(flat);
    std::filesystem::remove(rebuilt);
}

TEST_CASE("Flat OPC in-memory conversion preserves content types and binary parts [unit] [tools] [flat-opc]")
{
    const auto bytes = FlatOpcTestHelpers::Document()->SaveToMemory();
    const auto to = ConvertToFlatOpc(bytes);
    REQUIRE(to.Ok);
    CHECK(to.FlatOpcXml.find("pkg:package") != std::string::npos);
    const auto documentPart = to.FlatOpcXml.find("pkg:name=\"/word/document.xml\"");
    REQUIRE(documentPart != std::string::npos);
    const auto partEnd = to.FlatOpcXml.find('>', documentPart);
    REQUIRE(partEnd != std::string::npos);
    CHECK(to.FlatOpcXml.substr(documentPart, partEnd - documentPart).find("pkg:contentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml\"") !=
          std::string::npos);
    const auto from = ConvertFromFlatOpc(to.FlatOpcXml);
    REQUIRE(from.Ok);
    CHECK(WordDocumentEditor::Open(from.PackageBytes) != nullptr);
}

TEST_CASE("Flat OPC reader accepts a foreign namespace prefix [unit] [tools] [flat-opc]")
{
    constexpr std::string_view xml = R"(<x:package xmlns:x="http://schemas.microsoft.com/office/2006/xmlPackage"><x:part x:name="/[Content_Types].xml" x:contentType="application/vnd.openxmlformats-package.content-types+xml"><x:xmlData><Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types"/></x:xmlData></x:part></x:package>)";
    const auto result = ConvertFromFlatOpc(xml);
    CHECK(result.Ok);
    CHECK(result.PartCount == 1);
    CHECK(!result.PackageBytes.empty());
}

TEST_CASE("Flat OPC derives content types from overrides, defaults, and fallbacks [unit] [tools] [flat-opc]")
{
    const auto result = ConvertToFlatOpc(FlatOpcTestHelpers::BuildContentTypeFixture());
    REQUIRE(result.Ok);
    CHECK(result.PartCount == 5);
    CHECK(result.FlatOpcXml.find("pkg:name=\"/ordinary.XML\" pkg:contentType=\"application/default+xml\"") !=
          std::string::npos);
    CHECK(result.FlatOpcXml.find("pkg:name=\"/special.xml\" pkg:contentType=\"application/override+xml\"") !=
          std::string::npos);
    CHECK(result.FlatOpcXml.find("pkg:name=\"/opaque.bin\" pkg:contentType=\"application/example-binary\"") !=
          std::string::npos);
    CHECK(result.FlatOpcXml.find("pkg:name=\"/unknown.dat\" pkg:contentType=\"application/octet-stream\"") !=
          std::string::npos);
}

TEST_CASE("Flat OPC preserves arbitrary binary bytes through base64 [unit] [tools] [flat-opc]")
{
    const auto source = FlatOpcTestHelpers::BuildContentTypeFixture();
    const auto flat = ConvertToFlatOpc(source);
    REQUIRE(flat.Ok);
    CHECK(flat.FlatOpcXml.find("AP8Qfw==") != std::string::npos);
    const auto rebuilt = ConvertFromFlatOpc(flat.FlatOpcXml);
    REQUIRE(rebuilt.Ok);
    CHECK(FlatOpcTestHelpers::ReadZipEntry(rebuilt.PackageBytes, "opaque.bin") ==
          FlatOpcTestHelpers::Bytes({0x00, 0xFF, 0x10, 0x7F}));
    CHECK(FlatOpcTestHelpers::ReadZipEntry(rebuilt.PackageBytes, "unknown.dat") ==
          FlatOpcTestHelpers::Bytes({0x01, 0x02, 0x03}));
}

TEST_CASE("Flat OPC keeps a binary part that parses as XML byte-for-byte [unit] [tools] [flat-opc]")
{
    // Deciding by the bytes rather than by the content type used to send a VML
    // drawing through the XML DOM, and the trip back returned it reserialized:
    // an XML declaration it never had, and the newlines inside its start tag
    // gone. Flat OPC is documented as a byte-level round trip.
    const auto source = FlatOpcTestHelpers::BuildVmlDrawingFixture();
    const auto flat = ConvertToFlatOpc(source);
    REQUIRE(flat.Ok);

    const auto part = flat.FlatOpcXml.find("pkg:name=\"/xl/drawings/vmlDrawing1.vml\"");
    REQUIRE(part != std::string::npos);
    const auto partEnd = flat.FlatOpcXml.find("</pkg:part>", part);
    REQUIRE(partEnd != std::string::npos);
    const auto element = flat.FlatOpcXml.substr(part, partEnd - part);
    CHECK(element.find("pkg:binaryData") != std::string::npos);
    CHECK(element.find("pkg:xmlData") == std::string::npos);

    const auto rebuilt = ConvertFromFlatOpc(flat.FlatOpcXml);
    REQUIRE(rebuilt.Ok);
    CHECK(FlatOpcTestHelpers::ReadZipEntry(rebuilt.PackageBytes, "xl/drawings/vmlDrawing1.vml") ==
          FlatOpcTestHelpers::ReadZipEntry(source, "xl/drawings/vmlDrawing1.vml"));
}

TEST_CASE("Flat OPC keeps a run that holds nothing but a space [unit] [tools] [flat-opc]")
{
    // A space between two words is stored as a run of its own, so dropping the
    // whitespace-only text node runs the whole document together.
    const auto source = FlatOpcTestHelpers::BuildWhitespaceRunFixture();
    for (const bool prettyPrint : {true, false})
    {
        ToFlatOpcOptions options;
        options.PrettyPrint = prettyPrint;
        const auto flat = ConvertToFlatOpc(source, options);
        REQUIRE(flat.Ok);
        CHECK(flat.FlatOpcXml.find(R"(<w:t xml:space="preserve"> </w:t>)") != std::string::npos);
        const auto rebuilt = ConvertFromFlatOpc(flat.FlatOpcXml);
        REQUIRE(rebuilt.Ok);
        const auto document = FlatOpcTestHelpers::ReadZipEntry(rebuilt.PackageBytes, "word/document.xml");
        const std::string text(document.begin(), document.end());
        CHECK(text.find(R"(<w:t xml:space="preserve"> </w:t>)") != std::string::npos);
        CHECK(text.find("<w:t xml:space=\"preserve\" />") == std::string::npos);
    }
}

TEST_CASE("Flat OPC compact output can be converted back [unit] [tools] [flat-opc]")
{
    ToFlatOpcOptions options;
    options.PrettyPrint = false;
    const auto flat = ConvertToFlatOpc(FlatOpcTestHelpers::BuildContentTypeFixture(), options);
    REQUIRE(flat.Ok);
    CHECK(flat.FlatOpcXml.find("\n  <pkg:part") == std::string::npos);
    const auto rebuilt = ConvertFromFlatOpc(flat.FlatOpcXml);
    CHECK(rebuilt.Ok);
    CHECK(rebuilt.PartCount == flat.PartCount);
}

TEST_CASE("Flat OPC reader accepts whitespace in base64 and clamps compression [unit] [tools] [flat-opc]")
{
    constexpr std::string_view xml =
        R"(<x:package xmlns:x="http://schemas.microsoft.com/office/2006/xmlPackage"><x:part x:name="/payload.bin" x:contentType="application/octet-stream"><x:binaryData>AAE C /4A=</x:binaryData></x:part></x:package>)";
    FromFlatOpcOptions options;
    options.CompressionLevel = 999;
    const auto result = ConvertFromFlatOpc(xml, options);
    REQUIRE(result.Ok);
    CHECK(result.PartCount == 1);
    CHECK(FlatOpcTestHelpers::ReadZipEntry(result.PackageBytes, "payload.bin") ==
          FlatOpcTestHelpers::Bytes({0x00, 0x01, 0x02, 0xFF, 0x80}));
}

TEST_CASE("Flat OPC reader diagnoses bad parts but preserves valid parts [unit] [tools] [flat-opc]")
{
    constexpr std::string_view xml =
        R"(<pkg:package xmlns:pkg="http://schemas.microsoft.com/office/2006/xmlPackage"><pkg:part pkg:name="/valid.bin" pkg:contentType="application/octet-stream"><pkg:binaryData>AQID</pkg:binaryData></pkg:part><pkg:part pkg:name="/invalid-base64.bin" pkg:contentType="application/octet-stream"><pkg:binaryData>@@@</pkg:binaryData></pkg:part><pkg:part pkg:name="/../escape.bin" pkg:contentType="application/octet-stream"><pkg:binaryData>AQ==</pkg:binaryData></pkg:part><pkg:part pkg:name="/missing.bin" pkg:contentType="application/octet-stream"/></pkg:package>)";
    const auto result = ConvertFromFlatOpc(xml);
    REQUIRE(result.Ok);
    CHECK(result.PartCount == 1);
    CHECK(result.Diagnostics.size() == 3);
    CHECK(FlatOpcTestHelpers::ReadZipEntry(result.PackageBytes, "valid.bin") ==
          FlatOpcTestHelpers::Bytes({0x01, 0x02, 0x03}));
}

TEST_CASE("Flat OPC conversion reports invalid inputs [unit] [tools] [flat-opc]")
{
    const auto missing = ConvertToFlatOpc(FlatOpcTestHelpers::Temporary("missing", ".docx"),
                                          FlatOpcTestHelpers::Temporary("unused", ".xml"));
    CHECK_FALSE(missing.Ok);
    CHECK(!missing.Diagnostics.empty());
    const auto invalid = ConvertFromFlatOpc("<not-a-package/>");
    CHECK_FALSE(invalid.Ok);
    CHECK(!invalid.Diagnostics.empty());
    const auto corruptPackage = ConvertToFlatOpc(FlatOpcTestHelpers::Bytes{0x50, 0x4B, 0x03, 0x04});
    CHECK_FALSE(corruptPackage.Ok);
    CHECK(!corruptPackage.Diagnostics.empty());
    const auto malformedXml = ConvertFromFlatOpc("<pkg:package");
    CHECK_FALSE(malformedXml.Ok);
    CHECK(!malformedXml.Diagnostics.empty());
}
