// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "doctest.h"

#include "TestSupport.hpp"

#include "ExyokiOffice/OpenXMLElement.hpp"
#include "ExyokiOffice/OpenXmlPackage.hpp"
#include "ExyokiOffice/OpenXmlPackagePart.hpp"
#include "ExyokiOffice/OpenXmlQualifiedName.hpp"
#include "ExyokiOffice/Word/WordDocument.hpp"
#include "ExyokiOffice/Xml/XmlQuery.hpp"

#include <chrono>
#include <string>

using ExyokiOffice::OpenXMLElement;
using ExyokiOffice::OpenXmlPackage;
using ExyokiOffice::OpenXmlQualifiedName;
using ExyokiOffice::Word::WordDocumentEditor;
namespace Xml = ExyokiOffice::Xml;

namespace
{

constexpr std::string_view kWordNs = "http://schemas.openxmlformats.org/wordprocessingml/2006/main";

// A WordprocessingML document that deliberately declares the "w" namespace under
// a different prefix (ns0), so queries written with the conventional "w:" prefix
// exercise namespace-precise (not prefix-literal) matching.
constexpr const char* kDocumentXml =
    "<ns0:document xmlns:ns0=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\">"
    "<ns0:body>"
    "<ns0:p ns0:rsidR=\"00AA11\"><ns0:r><ns0:t>Alpha</ns0:t></ns0:r></ns0:p>"
    "<ns0:p><ns0:r><ns0:t>Beta</ns0:t></ns0:r></ns0:p>"
    "</ns0:body>"
    "</ns0:document>";

std::filesystem::path MakeTemporaryPath(std::string_view stem)
{
    return ExyokiOfficeTests::MakeTemporaryPath(stem, ".docx");
}

std::filesystem::path BuildBaseDocx()
{
    auto editor = WordDocumentEditor::CreateNew();
    REQUIRE(editor);
    editor->AddParagraph("placeholder");
    const auto path = MakeTemporaryPath("exyoki_xmlquery");
    REQUIRE(editor->SaveToFile(path));
    return path;
}

// An element whose local name is not spelled in ASCII: "Prehled" with the r
// carrying a caron, written as escapes because the compiler is not told the
// source encoding.
constexpr const char* kAccentedNameXml =
    "<ns0:document xmlns:ns0=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\">"
    "<ns0:body>"
    "<ns0:P\xC5\x99"
    "ehled>Alpha</ns0:P\xC5\x99"
    "ehled>"
    "<ns0:P\xC5\x99"
    "ehled>Beta</ns0:P\xC5\x99"
    "ehled>"
    "</ns0:body>"
    "</ns0:document>";

// Loads a base .docx and replaces the main document part's XML with @p xml,
// returning its root element. The package must outlive the returned root, so the
// caller passes one in.
std::shared_ptr<OpenXMLElement> LoadRootWithXml(OpenXmlPackage& package, const std::filesystem::path& path,
                                                const char* xml)
{
    REQUIRE(package.LoadFromFile(path));
    auto part = package.GetPartByUri("/word/document.xml");
    REQUIRE(part);
    part->SetXmlString(xml);
    auto root = part->GetRootElement();
    REQUIRE(root);
    return root;
}

std::shared_ptr<OpenXMLElement> LoadCustomRoot(OpenXmlPackage& package, const std::filesystem::path& path)
{
    return LoadRootWithXml(package, path, kDocumentXml);
}

} // namespace

TEST_CASE("SelectNodes matches by namespace regardless of document prefix [unit] [xml]")
{
    const auto path = BuildBaseDocx();
    OpenXmlPackage package;
    auto root = LoadCustomRoot(package, path);

    std::string error;
    const auto paragraphs = Xml::SelectNodes(root, "//w:p", {}, &error);
    CHECK(error.empty());
    REQUIRE(paragraphs.size() == 2);
    for (const auto& paragraph : paragraphs)
    {
        const auto name = paragraph->QualifiedName();
        CHECK(name.localName() == "p");
        CHECK(name.namespaceUri() == kWordNs);
    }

    std::filesystem::remove(path);
}

TEST_CASE("SelectNodes matches an element name written outside ASCII [unit] [xml]")
{
    // The name test in a query is scanned byte by byte, so every byte of a
    // UTF-8 sequence has to count as part of the name. A rule that admitted
    // ASCII letters only would end the name at the first accented letter and
    // read the rest of it as syntax, leaving the element unreachable.
    const auto path = BuildBaseDocx();
    OpenXmlPackage package;
    auto root = LoadRootWithXml(package, path, kAccentedNameXml);

    std::string error;
    const auto matches = Xml::SelectNodes(root, "//w:P\xC5\x99"
                                                "ehled",
                                          {}, &error);
    CHECK(error.empty());
    REQUIRE(matches.size() == 2);
    CHECK(matches.front()->QualifiedName().localName() == "P\xC5\x99"
                                                          "ehled");
    CHECK(matches.front()->QualifiedName().namespaceUri() == kWordNs);

    std::filesystem::remove(path);
}

TEST_CASE("SelectNodes honors positional predicates and inner text [unit] [xml]")
{
    const auto path = BuildBaseDocx();
    OpenXmlPackage package;
    auto root = LoadCustomRoot(package, path);

    const auto first = Xml::SelectNodes(root, "//w:p[1]");
    REQUIRE(first.size() == 1);
    CHECK(Xml::InnerText(first.front()) == "Alpha");

    const auto runs = Xml::SelectNodes(root, "//w:t");
    REQUIRE(runs.size() == 2);
    CHECK(Xml::InnerText(root) == "AlphaBeta");

    std::filesystem::remove(path);
}

TEST_CASE("SelectNodes evaluates attribute existence and value predicates [unit] [xml]")
{
    const auto path = BuildBaseDocx();
    OpenXmlPackage package;
    auto root = LoadCustomRoot(package, path);

    CHECK(Xml::SelectNodes(root, "//w:p[@w:rsidR]").size() == 1);
    CHECK(Xml::SelectNodes(root, "//w:p[@w:rsidR='00AA11']").size() == 1);
    CHECK(Xml::SelectNodes(root, "//w:p[@w:rsidR='nope']").empty());

    std::filesystem::remove(path);
}

TEST_CASE("SelectNodes supports wildcards and prefixed wildcards [unit] [xml]")
{
    const auto path = BuildBaseDocx();
    OpenXmlPackage package;
    auto root = LoadCustomRoot(package, path);

    CHECK(Xml::SelectNodes(root, "//w:body/*").size() == 2);
    CHECK(Xml::SelectNodes(root, "//w:*").size() >= 6); // document, body, 2x p/r/t
    // Attribute results are not elements: no matches, but no error either.
    std::string error;
    CHECK(Xml::SelectNodes(root, "//w:p/@w:rsidR", {}, &error).empty());
    CHECK(error.empty());

    std::filesystem::remove(path);
}

TEST_CASE("SelectNodes reports malformed expressions and unbound prefixes [unit] [xml]")
{
    const auto path = BuildBaseDocx();
    OpenXmlPackage package;
    auto root = LoadCustomRoot(package, path);

    std::string error;
    CHECK(Xml::SelectNodes(root, "//[", {}, &error).empty());
    CHECK_FALSE(error.empty());

    error.clear();
    CHECK(Xml::SelectNodes(root, "//zz:p", {}, &error).empty());
    CHECK(error.find("zz") != std::string::npos);

    std::filesystem::remove(path);
}

TEST_CASE("SelectNodes accepts explicit namespace bindings [unit] [xml]")
{
    const auto path = BuildBaseDocx();
    OpenXmlPackage package;
    auto root = LoadCustomRoot(package, path);

    Xml::XmlQueryOptions options;
    options.NamespaceBindings.emplace_back("x", std::string(kWordNs));
    std::string error;
    CHECK(Xml::SelectNodes(root, "//x:t", options, &error).size() == 2);
    CHECK(error.empty());

    std::filesystem::remove(path);
}

TEST_CASE("Describe builds an owned, display-safe match [unit] [xml]")
{
    const auto path = BuildBaseDocx();
    OpenXmlPackage package;
    auto root = LoadCustomRoot(package, path);

    const auto first = Xml::SelectNodes(root, "//w:p[1]");
    REQUIRE(first.size() == 1);
    const auto match = Xml::Describe(first.front());
    CHECK(match.Name == "ns0:p"); // prefixed exactly as written in the document
    CHECK(match.LocalName == "p");
    CHECK(match.NamespaceUri == kWordNs);
    CHECK(match.Text == "Alpha");
    REQUIRE(match.Attributes.size() == 1);
    CHECK(match.Attributes.front().first == "ns0:rsidR");
    CHECK(match.Attributes.front().second == "00AA11");

    std::filesystem::remove(path);
}

TEST_CASE("XmlQuery fluent Where/FirstOrDefault refine a selection [unit] [xml]")
{
    const auto path = BuildBaseDocx();
    OpenXmlPackage package;
    auto root = LoadCustomRoot(package, path);

    auto query = Xml::XmlQuery::Select(root, "//w:p").Where([](const OpenXMLElement& paragraph)
                                                            { return paragraph.GetChild(OpenXmlQualifiedName{kWordNs, "r"}) != nullptr; });
    CHECK(query.Count() == 2);
    CHECK(query.FirstOrDefault() != nullptr);

    auto empty = Xml::XmlQuery::Select(root, "//w:p").Where([](const OpenXMLElement&)
                                                            { return false; });
    CHECK(empty.Count() == 0);
    CHECK(empty.FirstOrDefault() == nullptr);

    std::filesystem::remove(path);
}

TEST_CASE("XmlHelpers::FindByAttribute finds elements by attribute value [unit] [xml]")
{
    const auto path = BuildBaseDocx();
    OpenXmlPackage package;
    auto root = LoadCustomRoot(package, path);

    const auto found = Xml::XmlHelpers::FindByAttribute(root, OpenXmlQualifiedName{kWordNs, "rsidR"}, "00AA11");
    REQUIRE(found.size() == 1);
    CHECK(found.front()->QualifiedName().localName() == "p");

    const auto texts = Xml::XmlHelpers::ExtractAllText(root);
    REQUIRE(texts.size() == 2);
    CHECK(texts.front() == "Alpha");
    CHECK(texts.back() == "Beta");

    std::filesystem::remove(path);
}
