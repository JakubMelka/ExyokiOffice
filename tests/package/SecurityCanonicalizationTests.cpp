// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "doctest.h"

#include "Security/RelationshipTransform.hpp"
#include "Security/XmlCanonicalization.hpp"

#include <optional>
#include <string>
#include <vector>

namespace
{
using ExyokiOffice::OpenXmlRelationship;
using ExyokiOffice::Security::CanonicalizationMethod;
using ExyokiOffice::Security::RelationshipSelection;
using ExyokiOffice::Security::RelationshipTransform;
using ExyokiOffice::Security::XmlCanonicalization;

std::optional<std::string> Canonicalize(const std::string& xml,
                                        CanonicalizationMethod method = CanonicalizationMethod::InclusiveC14N)
{
    ExyokiOffice::Pugi::xml_document document;
    REQUIRE(document.load_string(xml.c_str(), XmlCanonicalization::ParseOptions));
    return XmlCanonicalization::CanonicalizeDocument(document, method);
}

/// The canonical form of XML the test knows canonicalizes, so the assertions
/// below stay about the canonical form rather than about the optional.
std::string CanonicalText(const std::string& xml,
                          CanonicalizationMethod method = CanonicalizationMethod::InclusiveC14N)
{
    auto canonical = Canonicalize(xml, method);
    REQUIRE(canonical.has_value());
    return *canonical;
}

std::string CanonicalizeElement(const std::string& xml, const char* xpath)
{
    ExyokiOffice::Pugi::xml_document document;
    REQUIRE(document.load_string(xml.c_str(), XmlCanonicalization::ParseOptions));
    const auto node = document.select_node(xpath).node();
    REQUIRE(node);
    auto canonical = XmlCanonicalization::CanonicalizeSubtree(node);
    REQUIRE(canonical.has_value());
    return *canonical;
}

/// The transform output of relationships the test knows canonicalize.
std::string TransformText(const std::vector<OpenXmlRelationship>& relationships,
                          const RelationshipSelection& selection)
{
    auto output = RelationshipTransform::Apply(relationships, selection);
    REQUIRE(output.has_value());
    return *output;
}

OpenXmlRelationship MakeRelationship(std::string id, std::string type, std::string target)
{
    OpenXmlRelationship relationship;
    relationship.Id = std::move(id);
    relationship.Type = std::move(type);
    relationship.Target = std::move(target);
    return relationship;
}
} // namespace

TEST_SUITE("XML canonicalization")
{

    TEST_CASE("Nesting deeper than the cap is refused rather than recursed into [unit] [security] [c14n]")
    {
        // The writer recurses once per level with a namespace map on each frame,
        // and signature verification runs it on XML the package supplied, so the
        // depth has to be bounded by something other than the call stack.
        const auto build = [](unsigned int depth)
        {
            std::string xml = "<r xmlns=\"urn:x\">";
            for (unsigned int level = 0; level < depth; ++level)
            {
                xml += "<a>";
            }
            for (unsigned int level = 0; level < depth; ++level)
            {
                xml += "</a>";
            }
            return xml + "</r>";
        };

        // Just inside the cap: the root plus MaximumDepth - 1 nested elements.
        const auto shallow = Canonicalize(build(XmlCanonicalization::MaximumDepth - 1));
        REQUIRE(shallow.has_value());
        CHECK_FALSE(shallow->empty());

        // One level too far. No result rather than a truncated one, and no
        // result rather than an empty string: a caller that digests what it
        // gets back must not be handed something that hashes to anything.
        const auto deep = Canonicalize(build(XmlCanonicalization::MaximumDepth));
        CHECK_FALSE(deep.has_value());
    }

    TEST_CASE("Processing instructions and comments follow the specification [unit] [security] [c14n]")
    {
        // Example 3.1 of the canonicalization specification.
        const std::string input = "<?xml version=\"1.0\"?>\n"
                                  "\n"
                                  "<?pi-without-body ?>\n"
                                  "<doc>Hello, world!<!-- Comment 1 --></doc>\n"
                                  "\n"
                                  "<?pi-without-body ?>\n";

        CHECK(CanonicalText(input) == "<?pi-without-body?>\n<doc>Hello, world!</doc>\n<?pi-without-body?>");
        CHECK(CanonicalText(input, CanonicalizationMethod::InclusiveC14NWithComments) ==
              "<?pi-without-body?>\n<doc>Hello, world!<!-- Comment 1 --></doc>\n<?pi-without-body?>");
    }

    TEST_CASE("Character escaping follows the specification [unit] [security] [c14n]")
    {
        // Example 3.3: text keeps > escaped, attribute values keep tabs as entities.
        const std::string input = "<doc>\n"
                                  "   <text>First line&#x0d;&#10;Second line</text>\n"
                                  "   <value>&#x32;</value>\n"
                                  "   <compute expr='value&gt;\"0\" &amp;&amp; value&lt;\"10\" ?\"valid\":\"error\"'>"
                                  "valid</compute>\n"
                                  "   <norm attr=' &apos;   &#x20;&#13;&#xa;&#9;   &apos; '/>\n"
                                  "</doc>";

        const auto canonical = CanonicalText(input);
        CHECK(canonical.find("First line&#xD;\nSecond line") != std::string::npos);
        CHECK(canonical.find("<value>2</value>") != std::string::npos);
        // A greater-than sign stays literal inside an attribute value.
        CHECK(canonical.find("expr=\"value>&quot;0&quot; &amp;&amp; value&lt;&quot;10&quot; ?&quot;valid&quot;:"
                             "&quot;error&quot;\"") != std::string::npos);
        CHECK(canonical.find("attr=\" '    &#xD;&#xA;&#x9;   ' \"") != std::string::npos);
    }

    TEST_CASE("Empty elements become a start and an end tag [unit] [security] [c14n]")
    {
        CHECK(CanonicalText("<doc><e/></doc>") == "<doc><e></e></doc>");
    }

    TEST_CASE("Attributes sort by namespace URI and then local name [unit] [security] [c14n]")
    {
        const std::string input = "<doc xmlns:b=\"http://b.example\" xmlns:a=\"http://a.example\" "
                                  "zeta=\"1\" alpha=\"2\" b:second=\"3\" a:first=\"4\"/>";

        CHECK(CanonicalText(input) == "<doc xmlns:a=\"http://a.example\" xmlns:b=\"http://b.example\" "
                                      "alpha=\"2\" zeta=\"1\" a:first=\"4\" b:second=\"3\"></doc>");
    }

    TEST_CASE("Namespace declarations sort by prefix and drop redundant ones [unit] [security] [c14n]")
    {
        const std::string input = "<doc xmlns=\"http://default.example\" xmlns:z=\"http://z.example\" "
                                  "xmlns:a=\"http://a.example\">"
                                  "<child xmlns:a=\"http://a.example\" xmlns:m=\"http://m.example\"/>"
                                  "</doc>";

        CHECK(CanonicalText(input) == "<doc xmlns=\"http://default.example\" xmlns:a=\"http://a.example\" "
                                      "xmlns:z=\"http://z.example\">"
                                      "<child xmlns:m=\"http://m.example\"></child></doc>");
    }

    TEST_CASE("An inherited default namespace is undeclared explicitly [unit] [security] [c14n]")
    {
        const std::string input = "<doc xmlns=\"http://default.example\"><child xmlns=\"\"/></doc>";

        CHECK(CanonicalText(input) == "<doc xmlns=\"http://default.example\"><child xmlns=\"\"></child></doc>");
    }

    TEST_CASE("A subtree renders the namespaces it inherits [unit] [security] [c14n]")
    {
        const std::string input = "<root xmlns:a=\"http://a.example\" xmlns=\"http://default.example\">"
                                  "<middle xmlns:m=\"http://m.example\">"
                                  "<object a:mark=\"1\">text</object>"
                                  "</middle></root>";

        CHECK(CanonicalizeElement(input, "//object") ==
              "<object xmlns=\"http://default.example\" xmlns:a=\"http://a.example\" xmlns:m=\"http://m.example\" "
              "a:mark=\"1\">text</object>");
    }

    TEST_CASE("A subtree inherits xml attributes from its ancestors [unit] [security] [c14n]")
    {
        const std::string input = "<root xml:lang=\"cs\" xml:space=\"preserve\">"
                                  "<middle xml:lang=\"en\"><object/></middle></root>";

        // The nearest ancestor wins for xml:lang, and xml:space is inherited too.
        CHECK(CanonicalizeElement(input, "//object") ==
              "<object xml:lang=\"en\" xml:space=\"preserve\"></object>");
    }

    TEST_CASE("An own xml attribute overrides the inherited one [unit] [security] [c14n]")
    {
        const std::string input = "<root xml:lang=\"cs\"><object xml:lang=\"de\"/></root>";

        CHECK(CanonicalizeElement(input, "//object") == "<object xml:lang=\"de\"></object>");
    }

    TEST_CASE("CDATA sections become escaped text [unit] [security] [c14n]")
    {
        CHECK(CanonicalText("<doc><![CDATA[a < b & c]]></doc>") == "<doc>a &lt; b &amp; c</doc>");
    }

} // TEST_SUITE

TEST_SUITE("Relationship transform")
{

    TEST_CASE("Relationships are sorted and TargetMode is made explicit [unit] [security] [c14n]")
    {
        const std::vector<OpenXmlRelationship> relationships{
            MakeRelationship("rId10", "http://types.example/styles", "styles.xml"),
            MakeRelationship("rId2", "http://types.example/theme", "theme.xml")};

        const auto output = TransformText(relationships, {});
        CHECK(output == "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
                        "<Relationship Id=\"rId10\" Target=\"styles.xml\" TargetMode=\"Internal\" "
                        "Type=\"http://types.example/styles\"></Relationship>"
                        "<Relationship Id=\"rId2\" Target=\"theme.xml\" TargetMode=\"Internal\" "
                        "Type=\"http://types.example/theme\"></Relationship>"
                        "</Relationships>");
    }

    TEST_CASE("An external relationship keeps its target mode [unit] [security] [c14n]")
    {
        auto external = MakeRelationship("rId1", "http://types.example/hyperlink", "http://example.com");
        external.TargetMode = "External";
        external.IsExternal = true;

        const auto output = TransformText({external}, {});
        CHECK(output.find("TargetMode=\"External\"") != std::string::npos);
    }

    TEST_CASE("Selection by identifier keeps only the listed relationships [unit] [security] [c14n]")
    {
        const std::vector<OpenXmlRelationship> relationships{
            MakeRelationship("rId1", "http://types.example/styles", "styles.xml"),
            MakeRelationship("rId2", "http://types.example/theme", "theme.xml")};

        RelationshipSelection selection;
        selection.SourceIds.emplace_back("rId2");

        const auto output = TransformText(relationships, selection);
        CHECK(output.find("rId2") != std::string::npos);
        CHECK(output.find("rId1") == std::string::npos);
    }

    TEST_CASE("Selection by type keeps every relationship of that type [unit] [security] [c14n]")
    {
        const std::vector<OpenXmlRelationship> relationships{
            MakeRelationship("rId1", "http://types.example/image", "media/one.png"),
            MakeRelationship("rId2", "http://types.example/theme", "theme.xml"),
            MakeRelationship("rId3", "http://types.example/image", "media/two.png")};

        RelationshipSelection selection;
        selection.SourceTypes.emplace_back("http://types.example/image");

        const auto output = TransformText(relationships, selection);
        CHECK(output.find("media/one.png") != std::string::npos);
        CHECK(output.find("media/two.png") != std::string::npos);
        CHECK(output.find("theme.xml") == std::string::npos);
    }

    TEST_CASE("The selection is read from the transform element [unit] [security] [c14n]")
    {
        ExyokiOffice::Pugi::xml_document document;
        REQUIRE(document.load_string(
            "<Transform xmlns:mdssi=\"http://schemas.openxmlformats.org/package/2006/digital-signature\">"
            "<mdssi:RelationshipReference SourceId=\"rId1\"/>"
            "<mdssi:RelationshipReference SourceId=\"rId4\"/>"
            "<mdssi:RelationshipsGroupReference SourceType=\"http://types.example/image\"/>"
            "</Transform>"));

        const auto selection = RelationshipTransform::ReadSelection(document.document_element());
        REQUIRE(selection.SourceIds.size() == 2U);
        CHECK(selection.SourceIds[0] == "rId1");
        CHECK(selection.SourceIds[1] == "rId4");
        REQUIRE(selection.SourceTypes.size() == 1U);
        CHECK(selection.SourceTypes[0] == "http://types.example/image");
    }

} // TEST_SUITE
