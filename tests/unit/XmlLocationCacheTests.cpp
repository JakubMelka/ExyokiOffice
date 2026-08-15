// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

// ---------------------------------------------------------------------------
// XmlLocationCache is what makes a diagnostic's location affordable to build.
// Uncached, a path costs a walk over the element's ancestors plus a scan of the
// same-name siblings at every level, so a part with a few hundred children of
// one parent pays that scan again for every issue reported against it.
//
// What has to hold: the cached path is the path the uncached call produces -
// otherwise the cache would quietly change what diagnostics say - and one miss
// records the whole generation it had to scan, which is where the linear
// behaviour comes from.
// ---------------------------------------------------------------------------

#include "doctest.h"

#include "ExyokiOffice/OpenXMLElement.hpp"
#include "ExyokiOffice/OpenXmlPackage.hpp"
#include "ExyokiOffice/OpenXmlPackagePart.hpp"
#include "ExyokiOffice/StandardTypes.hpp"
#include "ExyokiOffice/XmlLocationCache.hpp"
#include "zip/zip.h"

#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

namespace XmlLocationCacheHelpers
{

void AddZipEntry(zip_t* archive, const char* name, std::string_view content)
{
    REQUIRE(zip_entry_open(archive, name) == 0);
    CHECK(zip_entry_write(archive, content.data(), content.size()) == 0);
    zip_entry_close(archive);
}

/** Wraps @p xml in a package, because only a package gives the tree its namespace scopes. */
std::vector<ExyokiOffice::Byte> BuildSingleXmlPartPackage(std::string_view xml)
{
    auto* archive = zip_stream_open(nullptr, 0, ZIP_DEFAULT_COMPRESSION_LEVEL, 'w');
    REQUIRE(archive != nullptr);

    AddZipEntry(archive,
                "[Content_Types].xml",
                R"(<?xml version="1.0" encoding="UTF-8"?>
<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">
  <Default Extension="xml" ContentType="application/xml"/>
</Types>)");
    AddZipEntry(archive, "custom.xml", xml);

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

/** A body of @p paragraphs paragraphs, each holding one run and one text node. */
std::string DocumentWithParagraphs(ExyokiOffice::Size paragraphs)
{
    std::string xml =
        R"(<?xml version="1.0" encoding="UTF-8"?>
<w:document xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main"><w:body>)";
    for (ExyokiOffice::Size index = 0; index < paragraphs; ++index)
    {
        xml += "<w:p><w:r><w:t>Paragraph ";
        xml += std::to_string(index);
        xml += "</w:t></w:r></w:p>";
    }
    xml += "<w:sectPr/></w:body></w:document>";
    return xml;
}

/** Depth-first list of every element of the tree rooted at @p element, @p element first. */
void CollectElements(const std::shared_ptr<ExyokiOffice::OpenXMLElement>& element,
                     std::vector<std::shared_ptr<ExyokiOffice::OpenXMLElement>>& elements)
{
    if (!element)
    {
        return;
    }

    elements.push_back(element);
    for (const auto& child : element->ChildrenInContentModel())
    {
        CollectElements(child, elements);
    }
}

} // namespace XmlLocationCacheHelpers

TEST_SUITE("XML location cache")
{
    using namespace XmlLocationCacheHelpers;

    TEST_CASE("a cached location is the location the element reports itself [unit] [xml-location-cache]")
    {
        ExyokiOffice::OpenXmlPackage package;
        REQUIRE(package.LoadFromMemory(BuildSingleXmlPartPackage(DocumentWithParagraphs(6))));

        auto part = package.GetPartByUri("/custom.xml");
        REQUIRE(part != nullptr);
        auto root = part->GetRootElement();
        REQUIRE(root != nullptr);

        std::vector<std::shared_ptr<ExyokiOffice::OpenXMLElement>> elements;
        CollectElements(root, elements);
        REQUIRE(elements.size() > 6);

        // Filled in document order here, which is the order validation walks the
        // tree in; the reverse order below starts from the deepest element, so
        // both directions of the ancestor chain get exercised.
        ExyokiOffice::XmlLocationCache forward;
        ExyokiOffice::XmlLocationCache backward;
        for (const auto& element : elements)
        {
            const auto expected = element->GetXmlLocation();
            CHECK(forward.Location(*element).Path == expected.Path);
            CHECK(forward.Location(*element).ElementName == expected.ElementName);
        }
        for (auto element = elements.rbegin(); element != elements.rend(); ++element)
        {
            CHECK(backward.Location(**element).Path == (*element)->GetXmlLocation().Path);
        }

        // The positional predicate is the part a cache could plausibly get wrong,
        // so one is spelled out rather than only compared against itself.
        const auto body = root->ChildrenInContentModel().front();
        REQUIRE(body != nullptr);
        const auto paragraphs = body->ChildrenInContentModel();
        REQUIRE(paragraphs.size() > 2);
        CHECK(forward.Location(*paragraphs[1]).Path == "/w:document/w:body/w:p[2]");
    }

    TEST_CASE("an attribute location keeps the element path of the cache [unit] [xml-location-cache]")
    {
        ExyokiOffice::OpenXmlPackage package;
        REQUIRE(package.LoadFromMemory(BuildSingleXmlPartPackage(
            R"(<?xml version="1.0" encoding="UTF-8"?>
<w:document xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main"><w:body>
  <w:p><w:pPr><w:pStyle w:val="Normal"/></w:pPr></w:p>
  <w:p><w:pPr><w:pStyle w:val="Heading1"/></w:pPr></w:p>
  <w:sectPr/></w:body></w:document>)")));

        auto root = package.GetPartByUri("/custom.xml")->GetRootElement();
        REQUIRE(root != nullptr);

        std::vector<std::shared_ptr<ExyokiOffice::OpenXMLElement>> elements;
        CollectElements(root, elements);

        const ExyokiOffice::OpenXmlQualifiedName value(
            "http://schemas.openxmlformats.org/wordprocessingml/2006/main", "val");

        ExyokiOffice::XmlLocationCache cache;
        for (const auto& element : elements)
        {
            const auto expected = element->GetXmlLocation(value);
            const auto actual = cache.Location(*element, value);
            CHECK(actual.Path == expected.Path);
            CHECK(actual.AttributeName == expected.AttributeName);
        }
    }

    TEST_CASE("one lookup records the whole generation it had to scan [unit] [xml-location-cache]")
    {
        ExyokiOffice::OpenXmlPackage package;
        REQUIRE(package.LoadFromMemory(BuildSingleXmlPartPackage(DocumentWithParagraphs(10))));

        auto root = package.GetPartByUri("/custom.xml")->GetRootElement();
        REQUIRE(root != nullptr);

        const auto body = root->ChildrenInContentModel().front();
        REQUIRE(body != nullptr);
        const auto paragraphs = body->ChildrenInContentModel();
        REQUIRE(paragraphs.size() == 11); // ten paragraphs and the section properties

        ExyokiOffice::XmlLocationCache cache;
        CHECK(cache.MemoizedElementCount() == 0);

        // Learning one paragraph's position means counting all of them, so the
        // rest of the generation - plus the two ancestors walked through - are
        // recorded by the same pass. Without that, every diagnostic against a
        // sibling would repeat the scan.
        cache.Location(*paragraphs.front());
        const auto afterFirst = cache.MemoizedElementCount();
        CHECK(afterFirst == paragraphs.size() + 2);

        for (const auto& paragraph : paragraphs)
        {
            cache.Location(*paragraph);
        }
        CHECK(cache.MemoizedElementCount() == afterFirst);

        cache.Clear();
        CHECK(cache.MemoizedElementCount() == 0);
        CHECK(cache.Location(*paragraphs.front()).Path == "/w:document/w:body/w:p[1]");
    }
}
