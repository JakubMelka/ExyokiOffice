// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

// ---------------------------------------------------------------------------
// Breadth backbone for the generated DOM corpus.
//
// This hand-written suite visits *every* generated element class through
// `OpenXmlElementFactory::AllElementClasses()` and asserts the invariants that
// must hold for all of them: the class is constructible, reports a consistent
// meta-class identity, and every declared attribute survives a real XML
// serialization round-trip. It deliberately works at the raw-string layer so it
// stays type-agnostic; the generator-emitted per-namespace suites exercise the
// typed `Get<Prop>()`/`Set<Prop>()` accessors and enum conversions with
// type-correct values on top of this.
// ---------------------------------------------------------------------------

#include "doctest.h"

#include "DomTestSupport.hpp"

#include "ExyokiOffice/DOM/OpenXmlElementFactory.hpp"
#include "ExyokiOffice/OpenXMLElement.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <string>
#include <string_view>

namespace DomGeneratedCorpusTestsHelpers
{

constexpr std::string_view kSampleValue = "eo-corpus-sample";

inline std::string DescribeClass(const ExyokiOffice::OpenXMLElementClass* metaClass)
{
    const auto type = metaClass->TypeQualifiedName();
    const auto name = metaClass->QualifiedName();
    std::string description(type.namespaceUri());
    description += " {";
    description += std::string(type.localName());
    description += "} <";
    description += std::string(name.localName());
    description += '>';
    return description;
}

} // namespace DomGeneratedCorpusTestsHelpers

TEST_SUITE("DomGeneratedCorpusTests")
{
    TEST_CASE("the generated element corpus is non-empty and unique [unit] [metadata] [dom-corpus]")
    {
        const auto classes = ExyokiOffice::Generated::OpenXmlElementFactory::AllElementClasses();
        REQUIRE(classes.size() > 0);

        // No meta-class pointer should appear twice - the enumeration is
        // deduplicated by class.
        for (ExyokiOffice::Size i = 0; i < classes.size(); ++i)
        {
            REQUIRE(classes[i] != nullptr);
            for (ExyokiOffice::Size j = i + 1; j < classes.size(); ++j)
            {
                CHECK(classes[i] != classes[j]);
            }
        }
    }

    TEST_CASE("every generated element is constructible with a consistent meta-class [unit] [metadata] [dom-corpus]")
    {
        for (const auto* metaClass : ExyokiOffice::Generated::OpenXmlElementFactory::AllElementClasses())
        {
            INFO("class: " << DomGeneratedCorpusTestsHelpers::DescribeClass(metaClass));

            auto instance = metaClass->Create();
            REQUIRE(instance != nullptr);
            CHECK(instance->ElementMetaClass() == metaClass);
            CHECK(instance->ElementMetaClass()->QualifiedName() == metaClass->QualifiedName());
            CHECK(instance->ElementMetaClass()->TypeQualifiedName() == metaClass->TypeQualifiedName());
        }
    }

    TEST_CASE("every generated element hosts and round-trips its declared attributes [unit] [metadata] [dom-corpus]")
    {
        using namespace DomGeneratedCorpusTestsHelpers;

        for (const auto* metaClass : ExyokiOffice::Generated::OpenXmlElementFactory::AllElementClasses())
        {
            INFO("class: " << DescribeClass(metaClass));

            // The factory enumeration only contains named elements, so hosting
            // each of them as a document root should always succeed.
            DomTest::HostedRoot hosted;
            REQUIRE(DomTest::TryHostMeta(metaClass, hosted));

            const auto attributes = metaClass->GetAttributes();
            for (const auto& attribute : attributes)
            {
                INFO("attribute: " << std::string(attribute.Name.namespaceUri()) << " {"
                                   << std::string(attribute.Name.localName()) << "}");
                hosted.root->SetAttribute(attribute.Name, kSampleValue);
                CHECK(hosted.root->GetAttribute(attribute.Name) == kSampleValue);
            }

            if (attributes.empty())
            {
                continue;
            }

            // Serialize the whole part and reload it: the attributes we wrote
            // must survive a real persistence cycle, including the namespace
            // declarations the DOM synthesizes for prefixed attribute names.
            DomTest::HostedRoot reloaded;
            REQUIRE(DomTest::TryHostRootXml(hosted.part->GetXmlString(), reloaded));
            for (const auto& attribute : attributes)
            {
                INFO("attribute: " << std::string(attribute.Name.namespaceUri()) << " {"
                                   << std::string(attribute.Name.localName()) << "}");
                CHECK(reloaded.root->GetAttribute(attribute.Name) == kSampleValue);
            }
        }
    }
}
