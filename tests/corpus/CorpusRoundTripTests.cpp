// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

// The round-trip matrix: every corpus fixture through open-save-open.
//
// The in-memory round-trip tests in the other layers can only lose what the
// library itself put there. These start from packages the library never
// produced, which is the only way to notice that a part it does not model, a
// relationship it does not follow, or a content type it does not know is
// dropped on the way out.
//
// What each case measures:
//
//   - parts: the set of part URIs, their content types, and the SHA-256 of the
//     payload each part would be written with - a serialized XML tree or a
//     binary buffer - so a re-serialized part differing by one byte is visible;
//   - relationships: every edge, by source, id, type, target and target mode;
//   - the [Content_Types].xml overrides and defaults, through the content type
//     each part reports back;
//   - stability: saving the reopened package again must produce the same
//     package, so the format the library writes is a fixed point.

#include "doctest.h"

#include "CorpusManifest.hpp"
#include "TestSupport.hpp"

#include "ExyokiOffice/OpenXmlPackage.hpp"
#include "ExyokiOffice/OpenXmlPackageValidator.hpp"
#include "ExyokiOffice/Tools/PackageInspector.hpp"
#include "XmlParseOptions.hpp"
#include "pugixml/pugixml.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace
{
using ExyokiOffice::Byte;
using ExyokiOfficeTests::CorpusDocument;
// CorpusShard, not CorpusManifest: every case in this file is registered as one
// CTest entry per corpus document, and sweeps the document that entry owns.
using ExyokiOfficeTests::CorpusShard;

/** The content of a package, reduced to what a lossless round trip must keep. */
struct PackageFingerprint
{
    /** part URI -> content type. */
    std::map<std::string, std::string> ContentTypes;
    /** part URI -> SHA-256 of the payload bytes. */
    std::map<std::string, std::string> PayloadDigests;
    /** One line per relationship edge, sorted so the comparison ignores order. */
    std::vector<std::string> Relationships;
};

PackageFingerprint Fingerprint(std::span<const Byte> packageBytes, bool& loaded)
{
    PackageFingerprint fingerprint;
    ExyokiOffice::OpenXmlPackage package;
    loaded = package.LoadFromMemory(packageBytes);
    if (!loaded)
    {
        return fingerprint;
    }

    for (const auto& part : ExyokiOffice::Tools::CollectAllParts(package))
    {
        fingerprint.ContentTypes.emplace(part->Uri(), std::string(part->ContentType()));

        // An XML part is digested from its serialized tree and a binary part
        // from its buffer, which together are exactly what the next save writes.
        // GetOriginalBytes() would be the wrong source: it returns the bytes
        // that were read, so the comparison would hold even if the writer
        // mangled the tree on the way out.
        std::vector<Byte> payload;
        if (part->IsXmlPart())
        {
            const auto xml = part->GetXmlString();
            payload.assign(xml.begin(), xml.end());
        }
        else
        {
            payload = part->GetBinaryData();
        }
        // Deliberately no "payload is not empty" assertion: a part legitimately
        // carries nothing, and Word writes exactly such a part - the zero-byte
        // /_xmlsignatures/origin.sigs that only exists to anchor a relationship.
        // A part that lost its content still fails, because the digest below
        // stops matching the one taken before the round trip.
        fingerprint.PayloadDigests.emplace(part->Uri(), ExyokiOfficeTests::Sha256Hex(payload));
    }

    for (const auto& relationship : ExyokiOffice::Tools::ListRelationships(package))
    {
        fingerprint.Relationships.push_back(
            relationship.SourceUri + " " + relationship.Relationship.Id + " " + relationship.Relationship.Type +
            " -> " + relationship.Relationship.Target +
            (relationship.Relationship.IsExternal ? " (external)" : ""));
    }
    std::sort(fingerprint.Relationships.begin(), fingerprint.Relationships.end());
    return fingerprint;
}

/** Renders the differences between two fingerprints, empty when they agree. */
std::vector<std::string> Compare(const PackageFingerprint& before, const PackageFingerprint& after)
{
    std::vector<std::string> differences;

    for (const auto& [uri, contentType] : before.ContentTypes)
    {
        const auto found = after.ContentTypes.find(uri);
        if (found == after.ContentTypes.end())
        {
            differences.push_back("part removed: " + uri);
        }
        else if (found->second != contentType)
        {
            differences.push_back("content type of " + uri + " changed: " + contentType + " -> " + found->second);
        }
    }
    for (const auto& [uri, contentType] : after.ContentTypes)
    {
        if (before.ContentTypes.find(uri) == before.ContentTypes.end())
        {
            differences.push_back("part added: " + uri + " (" + contentType + ")");
        }
    }

    for (const auto& [uri, digest] : before.PayloadDigests)
    {
        const auto found = after.PayloadDigests.find(uri);
        if (found != after.PayloadDigests.end() && found->second != digest)
        {
            differences.push_back("payload of " + uri + " changed: " + digest + " -> " + found->second);
        }
    }

    std::set<std::string> beforeEdges(before.Relationships.begin(), before.Relationships.end());
    std::set<std::string> afterEdges(after.Relationships.begin(), after.Relationships.end());
    for (const auto& edge : beforeEdges)
    {
        if (afterEdges.find(edge) == afterEdges.end())
        {
            differences.push_back("relationship removed: " + edge);
        }
    }
    for (const auto& edge : afterEdges)
    {
        if (beforeEdges.find(edge) == beforeEdges.end())
        {
            differences.push_back("relationship added: " + edge);
        }
    }

    return differences;
}

/**
 * Every text node of an XML part, in document order, one entry per node.
 *
 * Whitespace-only nodes count: a run holding nothing but a space is how
 * WordprocessingML separates two words, so losing one runs the document
 * together. The indentation a pretty-printing writer inserts between elements
 * does not count, which is what Xml::ParseOptions::Preserving separates - it
 * keeps a whitespace-only node only where it is the whole content of its
 * element.
 */
std::vector<std::string> TextNodes(std::string_view xml)
{
    std::vector<std::string> nodes;
    ExyokiOffice::Pugi::xml_document document;
    if (!document.load_buffer(xml.data(), xml.size(), ExyokiOffice::Xml::ParseOptions::Preserving))
    {
        return nodes;
    }
    const auto collect = [&nodes](auto&& self, const ExyokiOffice::Pugi::xml_node& node) -> void
    {
        for (auto child : node.children())
        {
            if (child.type() == ExyokiOffice::Pugi::node_pcdata || child.type() == ExyokiOffice::Pugi::node_cdata)
            {
                nodes.emplace_back(child.value());
            }
            else
            {
                self(self, child);
            }
        }
    };
    collect(collect, document);
    return nodes;
}

/** Loads a corpus file and saves it straight back out. */
std::vector<Byte> OpenAndSave(std::span<const Byte> packageBytes, bool& ok)
{
    ExyokiOffice::OpenXmlPackage package;
    ok = package.LoadFromMemory(packageBytes);
    if (!ok)
    {
        return {};
    }

    auto saved = package.SaveToMemory();
    ok = !saved.empty();
    return saved;
}
} // namespace

TEST_SUITE("Corpus round trip")
{

    TEST_CASE("open-save-open keeps every part, relationship and payload [corpus] [corpus-roundtrip]")
    {
        for (const auto& document : CorpusShard())
        {
            CAPTURE(document.File);

            const auto original = ExyokiOfficeTests::ReadAllBytes(document.Path());
            REQUIRE_FALSE(original.empty());

            bool loaded = false;
            const auto before = Fingerprint(original, loaded);
            REQUIRE(loaded);
            REQUIRE(before.ContentTypes.size() == document.PartCount);

            bool saved = false;
            const auto roundTripped = OpenAndSave(original, saved);
            REQUIRE(saved);

            const auto after = Fingerprint(roundTripped, loaded);
            REQUIRE(loaded);

            const auto differences = Compare(before, after);
            for (const auto& difference : differences)
            {
                CAPTURE(difference);
            }
            CHECK(differences.empty());
        }
    }

    TEST_CASE("a second save reproduces the first [corpus] [corpus-roundtrip]")
    {
        // Open-save is allowed to normalize - the library reserializes every XML
        // part from its tree - but it has to reach a fixed point immediately.
        // A package that keeps changing every time it is opened and saved makes
        // any diff of two edits meaningless.
        for (const auto& document : CorpusShard())
        {
            CAPTURE(document.File);

            const auto original = ExyokiOfficeTests::ReadAllBytes(document.Path());
            REQUIRE_FALSE(original.empty());

            bool ok = false;
            const auto first = OpenAndSave(original, ok);
            REQUIRE(ok);
            const auto second = OpenAndSave(first, ok);
            REQUIRE(ok);

            const auto summary = ExyokiOfficeTests::ComparePackages(first, second);
            for (const auto& difference : summary.Differences)
            {
                CAPTURE(difference);
            }
            CHECK(summary.Ok);
            CHECK(summary.Preserved);
        }
    }

    TEST_CASE("the reopened package still validates [corpus] [corpus-roundtrip]")
    {
        // Preserving the part graph is not the same as preserving the markup:
        // a writer that reorders children keeps every part and still produces a
        // document Office rejects.
        for (const auto& document : CorpusShard())
        {
            CAPTURE(document.File);

            const auto original = ExyokiOfficeTests::ReadAllBytes(document.Path());
            REQUIRE_FALSE(original.empty());

            bool ok = false;
            const auto roundTripped = OpenAndSave(original, ok);
            REQUIRE(ok);

            ExyokiOffice::OpenXmlPackage reopened;
            REQUIRE(reopened.LoadFromMemory(roundTripped));

            // The settings constructor, not the default one: only it validates
            // the markup inside the parts, which is the point of this case.
            const ExyokiOffice::OpenXmlPackageValidator validator{ExyokiOffice::OpenXmlDomValidationSettings{}};
            const auto result = validator.Validate(reopened);

            std::vector<std::string> issues;
            for (const auto& issue : result.Issues())
            {
                issues.push_back(issue.Message + (issue.PartUri.empty() ? "" : " in " + issue.PartUri) +
                                 (issue.Location.Path.empty() ? "" : " at " + issue.Location.Path));
            }
            for (const auto& issue : issues)
            {
                CAPTURE(issue);
            }
            CHECK(issues.empty());
        }
    }

    TEST_CASE("open-save keeps the text of every XML part [corpus] [corpus-roundtrip]")
    {
        // The fingerprint cases compare the library against itself: they digest
        // the tree the loader built, so a loader that drops content and a writer
        // that never writes it back agree with each other. This one reads the
        // bytes Office wrote - the part as it was stored - and compares the text
        // in them against the text the round trip produces.
        for (const auto& document : CorpusShard())
        {
            CAPTURE(document.File);

            const auto original = ExyokiOfficeTests::ReadAllBytes(document.Path());
            REQUIRE_FALSE(original.empty());

            ExyokiOffice::OpenXmlPackage source;
            source.SetPartByteRetention(ExyokiOffice::PartByteRetention::Always);
            REQUIRE(source.LoadFromMemory(original));

            bool ok = false;
            const auto roundTripped = OpenAndSave(original, ok);
            REQUIRE(ok);

            ExyokiOffice::OpenXmlPackage reopened;
            REQUIRE(reopened.LoadFromMemory(roundTripped));

            for (const auto& part : ExyokiOffice::Tools::CollectAllParts(source))
            {
                if (!part->IsXmlPart())
                {
                    continue;
                }
                CAPTURE(part->Uri());
                REQUIRE(part->HasOriginalBytes());
                const auto stored = part->GetOriginalBytes();

                const auto reopenedPart = reopened.GetPartByUri(part->Uri());
                REQUIRE(reopenedPart);
                const auto written = reopenedPart->GetXmlString();

                CHECK(TextNodes(std::string_view(reinterpret_cast<const char*>(stored.data()), stored.size())) ==
                      TextNodes(written));
            }
        }
    }

    TEST_CASE("the part graph survives a round trip unchanged [corpus] [corpus-roundtrip]")
    {
        // The manifest counts are the baseline the other layers cannot provide:
        // they were read off files Office wrote, so a regression that silently
        // drops a pivot cache or a notes master fails here with a number.
        for (const auto& document : CorpusShard())
        {
            CAPTURE(document.File);

            const auto original = ExyokiOfficeTests::ReadAllBytes(document.Path());
            REQUIRE_FALSE(original.empty());

            bool ok = false;
            const auto roundTripped = OpenAndSave(original, ok);
            REQUIRE(ok);

            ExyokiOffice::OpenXmlPackage package;
            REQUIRE(package.LoadFromMemory(roundTripped));

            const auto parts = ExyokiOffice::Tools::CollectAllParts(package);
            std::set<std::string> contentTypes;
            std::set<std::string> uris;
            for (const auto& part : parts)
            {
                uris.insert(part->Uri());
                contentTypes.insert(std::string(part->ContentType()));
            }

            CHECK(parts.size() == document.PartCount);
            CHECK(contentTypes.size() == document.ContentTypeCount);
            CHECK(ExyokiOffice::Tools::ListRelationships(package).size() == document.RelationshipCount);
            CHECK(ExyokiOffice::Tools::GetInfo(package).MainPartUri == document.MainPart);

            for (const auto& required : document.RequiredParts)
            {
                CHECK_MESSAGE(uris.count(required) == 1, "round trip lost part '", required, "'");
            }
        }
    }
}
