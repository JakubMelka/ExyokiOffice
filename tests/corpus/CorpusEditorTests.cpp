// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

// The family editors over the corpus.
//
// CorpusRoundTripTests goes through OpenXmlPackage, which knows nothing about
// Word, Excel or PowerPoint and therefore cannot drop a part because it does
// not model it. The family editors can: they build a typed view of the package
// on open and write it back on save, and every part outside that view depends
// on being carried along untouched.
//
// These cases open each fixture through its editor and check that saving keeps
// the part graph the manifest describes. They deliberately do not validate the
// markup again - CorpusValidationTests already does, and a second full
// validation of fifteen real documents is the most expensive thing this suite
// could add.

#include "doctest.h"

#include "CorpusManifest.hpp"

#include "ExyokiOffice/Excel/ExcelDocument.hpp"
#include "ExyokiOffice/OpenXmlPackage.hpp"
#include "ExyokiOffice/PowerPoint/PowerPointDocument.hpp"
#include "ExyokiOffice/Tools/PackageInspector.hpp"
#include "ExyokiOffice/Word/WordDocument.hpp"

#include <memory>
#include <set>
#include <string>
#include <vector>

namespace
{
using ExyokiOffice::Byte;
using ExyokiOfficeTests::CorpusDocument;
using ExyokiOfficeTests::CorpusManifest;

/** Opens a fixture through the editor of its family and saves it to memory. */
std::vector<Byte> EditorRoundTrip(const CorpusDocument& document)
{
    if (document.Family == "word")
    {
        const auto editor = ExyokiOffice::Word::WordDocumentEditor::Open(document.Path());
        return editor ? editor->SaveToMemory() : std::vector<Byte>{};
    }
    if (document.Family == "excel")
    {
        const auto editor = ExyokiOffice::Excel::ExcelDocumentEditor::Open(document.Path());
        return editor ? editor->SaveToMemory() : std::vector<Byte>{};
    }
    if (document.Family == "powerpoint")
    {
        const auto editor = ExyokiOffice::PowerPoint::PowerPointDocumentEditor::Open(document.Path());
        return editor ? editor->SaveToMemory() : std::vector<Byte>{};
    }
    return {};
}
} // namespace

TEST_SUITE("Corpus family editors")
{

    TEST_CASE("each family editor opens its fixtures and keeps the part graph [corpus] [corpus-editors]")
    {
        for (const auto& document : CorpusManifest())
        {
            CAPTURE(document.File);

            const auto saved = EditorRoundTrip(document);
            REQUIRE_FALSE(saved.empty());

            ExyokiOffice::OpenXmlPackage package;
            REQUIRE(package.LoadFromMemory(saved));

            const auto parts = ExyokiOffice::Tools::CollectAllParts(package);
            std::set<std::string> uris;
            std::set<std::string> contentTypes;
            for (const auto& part : parts)
            {
                uris.insert(part->Uri());
                contentTypes.insert(std::string(part->ContentType()));
            }

            CHECK(parts.size() == document.PartCount);
            CHECK(contentTypes.size() == document.ContentTypeCount);
            CHECK(ExyokiOffice::Tools::ListRelationships(package).size() == document.RelationshipCount);

            for (const auto& required : document.RequiredParts)
            {
                CHECK_MESSAGE(uris.count(required) == 1, "the editor dropped part '", required, "'");
            }
        }
    }

    TEST_CASE("the family detected on open matches the manifest [corpus] [corpus-editors]")
    {
        for (const auto& document : CorpusManifest())
        {
            CAPTURE(document.File);

            ExyokiOffice::OpenXmlPackage package;
            REQUIRE(package.LoadFromFile(document.Path()));

            const auto info = ExyokiOffice::Tools::GetInfo(package);
            CHECK(info.DocumentTypeName == document.DocumentType);
            CHECK(info.MainPartContentType.empty() == false);
            CHECK_FALSE(info.IsStrictConformance);
        }
    }
}
