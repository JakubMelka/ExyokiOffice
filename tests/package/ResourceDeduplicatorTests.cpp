// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "doctest.h"

#include "TestSupport.hpp"

#include "ExyokiOffice/Tools/ResourceDeduplicator.hpp"
#include "ExyokiOffice/Word/WordDocument.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace
{

using ExyokiOffice::Tools::DeduplicateSharedResources;
using ExyokiOffice::Tools::ResourceDeduplicationOptions;
using ExyokiOffice::Word::ImageLayout;
using ExyokiOffice::Word::ImageWrap;
using ExyokiOffice::Word::WordDocumentEditor;

void PushU32BE(std::vector<ExyokiOffice::Byte>& data, ExyokiOffice::UInt32 value)
{
    data.push_back(static_cast<ExyokiOffice::UInt8>((value >> 24) & 0xFF));
    data.push_back(static_cast<ExyokiOffice::UInt8>((value >> 16) & 0xFF));
    data.push_back(static_cast<ExyokiOffice::UInt8>((value >> 8) & 0xFF));
    data.push_back(static_cast<ExyokiOffice::UInt8>(value & 0xFF));
}

void PushFourCC(std::vector<ExyokiOffice::Byte>& data, const char (&fourCc)[5])
{
    for (int i = 0; i < 4; ++i)
    {
        data.push_back(static_cast<ExyokiOffice::UInt8>(fourCc[i]));
    }
}

/// Minimal syntactically valid PNG; CRCs are fake, which image detection tolerates.
std::vector<ExyokiOffice::Byte> BuildPngPayload(ExyokiOffice::UInt32 width, ExyokiOffice::UInt32 height)
{
    std::vector<ExyokiOffice::Byte> data;
    static constexpr ExyokiOffice::UInt8 kSignature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    data.insert(data.end(), std::begin(kSignature), std::end(kSignature));
    PushU32BE(data, 13);
    PushFourCC(data, "IHDR");
    PushU32BE(data, width);
    PushU32BE(data, height);
    data.push_back(8);
    data.push_back(6);
    data.push_back(0);
    data.push_back(0);
    data.push_back(0);
    PushU32BE(data, 0);
    PushU32BE(data, 0);
    PushFourCC(data, "IEND");
    PushU32BE(data, 0);
    return data;
}

std::filesystem::path MakeTemporaryPath(std::string_view stem)
{
    return ExyokiOfficeTests::MakeTemporaryPath(stem, ".docx");
}

/// Creates a Word document whose body embeds the same PNG payload twice plus
/// one different PNG, producing three separate image parts.
WordDocumentEditor::Ptr BuildDocumentWithDuplicateImages()
{
    auto editor = WordDocumentEditor::CreateNew();
    REQUIRE(editor != nullptr);

    const auto shared = BuildPngPayload(64, 64);
    REQUIRE(editor->AddImageFromData(shared, ImageLayout::Inline, ImageWrap::Square) != nullptr);
    REQUIRE(editor->AddImageFromData(shared, ImageLayout::Inline, ImageWrap::Square) != nullptr);
    REQUIRE(editor->AddImageFromData(BuildPngPayload(32, 32), ImageLayout::Inline, ImageWrap::Square) !=
            nullptr);
    return editor;
}

/// Counts distinct image parts. Parts() holds one entry per relationship edge,
/// so a part referenced by two relationships of the same container appears
/// twice in GetImageParts(); deduplication must be measured per part.
ExyokiOffice::Size CountImageParts(const WordDocumentEditor::Ptr& editor)
{
    auto mainPart = editor->GetDocument()->GetMainDocumentPart();
    REQUIRE(mainPart != nullptr);
    auto parts = mainPart->GetImageParts();
    std::sort(parts.begin(), parts.end());
    parts.erase(std::unique(parts.begin(), parts.end()), parts.end());
    return parts.size();
}

} // namespace

TEST_SUITE("ResourceDeduplicatorTests")
{

    TEST_CASE("Identical embedded images are merged into one shared part [unit] [shared] [dedup]")
    {
        auto editor = BuildDocumentWithDuplicateImages();
        REQUIRE(CountImageParts(editor) == 3);

        const auto result = DeduplicateSharedResources(*editor->GetDocument());
        CHECK(result.Ok);
        CHECK(result.RemovedParts == 1);
        CHECK(result.RewrittenRelationships == 1);
        CHECK(result.BytesSaved > 0);
        REQUIRE(result.Groups.size() == 1);
        CHECK(result.Groups.front().ContentType == "image/png");
        CHECK(result.Groups.front().DuplicatePartUris.size() == 1);
        CHECK(CountImageParts(editor) == 2);

        // Every r:embed reference in the document body must still resolve.
        auto mainPart = editor->GetDocument()->GetMainDocumentPart();
        const auto documentXml = mainPart->GetXmlString();
        ExyokiOffice::Size embeds = 0;
        for (const auto& relationship : mainPart->Relationships())
        {
            if (documentXml.find("r:embed=\"" + relationship.Id + "\"") != std::string::npos)
            {
                ++embeds;
            }
        }
        CHECK(embeds == 3);

        SUBCASE("the deduplicated package survives open-save-open")
        {
            const auto bytes = editor->SaveToMemory();
            REQUIRE_FALSE(bytes.empty());
            auto reopened = WordDocumentEditor::Open(bytes);
            REQUIRE(reopened != nullptr);
            CHECK(CountImageParts(reopened) == 2);
        }

        SUBCASE("a second run finds nothing to merge")
        {
            const auto again = DeduplicateSharedResources(*editor->GetDocument());
            CHECK(again.Ok);
            CHECK(again.RemovedParts == 0);
            CHECK(again.Groups.empty());
        }
    }

    TEST_CASE("Dry run reports duplicate groups without modifying the package [unit] [shared] [dedup]")
    {
        auto editor = BuildDocumentWithDuplicateImages();

        ResourceDeduplicationOptions options;
        options.DryRun = true;
        const auto result = DeduplicateSharedResources(*editor->GetDocument(), options);
        CHECK(result.Ok);
        CHECK(result.RemovedParts == 0);
        CHECK(result.RewrittenRelationships == 0);
        CHECK(result.BytesSaved > 0);
        REQUIRE(result.Groups.size() == 1);
        CHECK(result.Groups.front().DuplicatePartUris.size() == 1);
        CHECK(CountImageParts(editor) == 3);
    }

    TEST_CASE("Distinct payloads and disabled categories are left alone [unit] [shared] [dedup]")
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);
        REQUIRE(editor->AddImageFromData(BuildPngPayload(64, 64), ImageLayout::Inline, ImageWrap::Square) !=
                nullptr);
        REQUIRE(editor->AddImageFromData(BuildPngPayload(32, 32), ImageLayout::Inline, ImageWrap::Square) !=
                nullptr);

        SUBCASE("different payloads never merge")
        {
            const auto result = DeduplicateSharedResources(*editor->GetDocument());
            CHECK(result.Ok);
            CHECK(result.RemovedParts == 0);
            CHECK(result.Groups.empty());
            CHECK(CountImageParts(editor) == 2);
        }

        SUBCASE("disabling images skips image parts entirely")
        {
            REQUIRE(editor->AddImageFromData(BuildPngPayload(64, 64), ImageLayout::Inline,
                                             ImageWrap::Square) != nullptr);
            ResourceDeduplicationOptions options;
            options.IncludeImages = false;
            const auto result = DeduplicateSharedResources(*editor->GetDocument(), options);
            CHECK(result.Ok);
            CHECK(result.RemovedParts == 0);
            CHECK(CountImageParts(editor) == 3);
        }
    }

    TEST_CASE("File overload deduplicates and protects existing outputs [unit] [shared] [dedup]")
    {
        auto editor = BuildDocumentWithDuplicateImages();
        const auto input = MakeTemporaryPath("dedup_input");
        const auto output = MakeTemporaryPath("dedup_output");
        REQUIRE(editor->SaveToFile(input));

        const auto result = DeduplicateSharedResources(input, output);
        CHECK(result.Ok);
        CHECK(result.RemovedParts == 1);
        REQUIRE(std::filesystem::exists(output));

        auto reopened = WordDocumentEditor::Open(output);
        REQUIRE(reopened != nullptr);
        CHECK(CountImageParts(reopened) == 2);

        SUBCASE("an existing output is protected unless Overwrite is set")
        {
            const auto blocked = DeduplicateSharedResources(input, output);
            CHECK_FALSE(blocked.Ok);

            ResourceDeduplicationOptions options;
            options.Overwrite = true;
            const auto forced = DeduplicateSharedResources(input, output, options);
            CHECK(forced.Ok);
        }

        std::filesystem::remove(input);
        std::filesystem::remove(output);
    }

    TEST_CASE("RetargetRelationship keeps ids stable and cleans up orphans [unit] [shared] [dedup] [opc]")
    {
        auto editor = BuildDocumentWithDuplicateImages();
        auto document = editor->GetDocument();
        auto mainPart = document->GetMainDocumentPart();
        REQUIRE(mainPart != nullptr);
        auto imageParts = mainPart->GetImageParts();
        REQUIRE(imageParts.size() == 3);

        auto first = imageParts[0];
        auto second = imageParts[1];
        REQUIRE(second->IncomingRelationships().size() == 1);
        const auto edge = second->IncomingRelationships().front();

        SUBCASE("successful retarget preserves the relationship id")
        {
            REQUIRE(mainPart->RetargetRelationship(edge.Id, first));
            const auto relationship = [&]() -> std::optional<ExyokiOffice::OpenXmlRelationship>
            {
                for (const auto& candidate : mainPart->Relationships())
                {
                    if (candidate.Id == edge.Id)
                    {
                        return candidate;
                    }
                }
                return std::nullopt;
            }();
            REQUIRE(relationship.has_value());
            CHECK_FALSE(relationship->IsExternal);

            // The abandoned image part is unreachable and must be gone.
            CHECK(document->GetPartByUri(second->Uri()) == nullptr);
            // The kept part now has two incoming edges.
            CHECK(first->IncomingRelationships().size() >= 2);
        }

        SUBCASE("unknown ids and foreign parts are rejected")
        {
            CHECK_FALSE(mainPart->RetargetRelationship("rId999", first));
            CHECK_FALSE(mainPart->RetargetRelationship(edge.Id, nullptr));

            auto foreignEditor = WordDocumentEditor::CreateNew();
            REQUIRE(foreignEditor != nullptr);
            REQUIRE(foreignEditor->AddImageFromData(BuildPngPayload(8, 8), ImageLayout::Inline,
                                                    ImageWrap::Square) != nullptr);
            auto foreignImage =
                foreignEditor->GetDocument()->GetMainDocumentPart()->GetImageParts().front();
            CHECK_FALSE(mainPart->RetargetRelationship(edge.Id, foreignImage));

            // Failed retargets leave the original edge intact.
            CHECK(document->GetPartByUri(second->Uri()) == second);
        }

        SUBCASE("retargeting to the current target is a no-op success")
        {
            CHECK(mainPart->RetargetRelationship(edge.Id, second));
            CHECK(document->GetPartByUri(second->Uri()) == second);
        }
    }

} // TEST_SUITE("ResourceDeduplicatorTests")
