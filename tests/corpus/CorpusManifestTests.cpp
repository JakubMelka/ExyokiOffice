// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "doctest.h"

#include "CorpusManifest.hpp"

#include "ExyokiOffice/OpenXmlPackage.hpp"
#include "ExyokiOffice/Tools/PackageInspector.hpp"

#include <algorithm>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace
{
using ExyokiOfficeTests::CorpusDocument;
using ExyokiOfficeTests::CorpusManifest;

std::shared_ptr<ExyokiOffice::OpenXmlPackage> OpenCorpusPackage(const CorpusDocument& document)
{
    auto package = std::make_shared<ExyokiOffice::OpenXmlPackage>();
    return package->LoadFromFile(document.Path()) ? package : nullptr;
}
} // namespace

TEST_SUITE("Corpus manifest")
{

    TEST_CASE("every corpus file is described by the manifest [corpus] [corpus-manifest]")
    {
        std::set<std::string> described;
        for (const auto& document : CorpusManifest())
        {
            CHECK_MESSAGE(described.insert(document.File).second,
                          "manifest lists '", document.File, "' more than once");
        }

        // A fixture nobody described is a fixture nobody tests. Dropping a file
        // into corpus/ has to fail here until the manifest says what it is for.
        for (const auto& file : ExyokiOfficeTests::CorpusFilesOnDisk())
        {
            CHECK_MESSAGE(described.count(file) == 1,
                          "corpus file '", file, "' has no manifest entry");
        }
    }

    TEST_CASE("every manifest entry has a purpose and required parts [corpus] [corpus-manifest]")
    {
        for (const auto& document : CorpusManifest())
        {
            CAPTURE(document.File);
            CHECK_FALSE(document.Purpose.empty());
            CHECK_FALSE(document.Features.empty());
            CHECK_FALSE(document.RequiredParts.empty());
            CHECK(document.Sha256.size() == 64);
        }
    }

    TEST_CASE("corpus files match the manifest checksums [corpus] [corpus-manifest]")
    {
        for (const auto& document : CorpusManifest())
        {
            CAPTURE(document.File);
            const auto bytes = ExyokiOfficeTests::ReadAllBytes(document.Path());
            REQUIRE_FALSE(bytes.empty());
            CHECK(bytes.size() == document.FileSize);
            // Pins the fixture: a corpus file replaced or re-saved by hand
            // changes what every other case in this layer is measuring.
            CHECK(ExyokiOfficeTests::Sha256Hex(bytes) == document.Sha256);
        }
    }

    TEST_CASE("corpus packages carry the parts the manifest expects [corpus] [corpus-manifest]")
    {
        for (const auto& document : CorpusManifest())
        {
            CAPTURE(document.File);
            const auto package = OpenCorpusPackage(document);
            REQUIRE(package != nullptr);

            const auto info = ExyokiOffice::Tools::GetInfo(*package);
            CHECK(info.MainPartUri == document.MainPart);

            const auto parts = ExyokiOffice::Tools::CollectAllParts(*package);
            std::set<std::string> uris;
            std::set<std::string> contentTypes;
            for (const auto& part : parts)
            {
                uris.insert(part->Uri());
                contentTypes.insert(std::string(part->ContentType()));
            }

            CHECK(parts.size() == document.PartCount);
            CHECK(uris.size() == document.PartCount);
            CHECK(contentTypes.size() == document.ContentTypeCount);
            CHECK(ExyokiOffice::Tools::ListRelationships(*package).size() == document.RelationshipCount);

            for (const auto& required : document.RequiredParts)
            {
                CHECK_MESSAGE(uris.count(required) == 1, "missing part '", required, "'");
            }
        }
    }

    TEST_CASE("the corpus covers all three families [corpus] [corpus-manifest]")
    {
        std::set<std::string> families;
        for (const auto& document : CorpusManifest())
        {
            families.insert(document.Family);
        }

        CHECK(families.count("word") == 1);
        CHECK(families.count("excel") == 1);
        CHECK(families.count("powerpoint") == 1);
    }

    TEST_CASE("the corpus covers the feature areas it claims [corpus] [corpus-manifest]")
    {
        std::set<std::string> features;
        for (const auto& document : CorpusManifest())
        {
            features.insert(document.Features.begin(), document.Features.end());
        }

        // These are the areas the corpus exists to cover: markup a document
        // built in memory by the library's own API never produces. Losing one
        // means a fixture was removed without a replacement.
        for (const char* required : {"headers", "footers", "footnotes", "endnotes", "numbering",
                                     "charts", "pivot-tables", "comments-threaded", "vml",
                                     "extension-lists", "notes", "masters", "layouts"})
        {
            CHECK_MESSAGE(features.count(required) == 1, "no fixture covers '", required, "'");
        }
    }
}
