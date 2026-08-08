// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "doctest.h"

#include "TestSupport.hpp"

#include "ExyokiOffice/OpenXmlPackageValidator.hpp"
#include "ExyokiOffice/PowerPoint/PowerPointDocument.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <fstream>

using namespace ExyokiOffice::PowerPoint;

namespace
{
PresentationEmbeddedPicture Poster()
{
    return {{0x89, 'P', 'N', 'G', 13, 10, 26, 10}, "image/png"};
}

PresentationMediaData EmbeddedVideo()
{
    PresentationMediaData value;
    value.Kind = PresentationMediaKind::Video;
    value.Embedded = PresentationEmbeddedMedia{{0, 0, 0, 24, 'f', 't', 'y', 'p'}, "video/mp4"};
    value.PosterFrame = Poster();
    value.Playback = {42000, true, false, true, true};
    value.Name = "Quarterly video";
    value.AltText = "A product demonstration";
    value.Transform.Position = {1200, 3400};
    value.Transform.Size = {5000000, 2812500};
    return value;
}
} // namespace

TEST_SUITE("PowerPointMediaTests")
{
    TEST_CASE("embedded video, poster frame, and playback metadata survive a round trip [unit] [powerpoint] [media]")
    {
        auto editor = PowerPointDocumentEditor::CreateNew();
        auto shape = editor->AddSlide()->ShapeTree()->AddMedia(EmbeddedVideo());
        REQUIRE(shape);
        REQUIRE(shape->GetMedia());
        CHECK(*shape->GetMedia() == EmbeddedVideo());

        auto reopened = PowerPointDocumentEditor::Open(editor->SaveToMemory());
        REQUIRE(reopened);
        auto persisted = reopened->GetSlide(0)->ShapeTree()->Get(0)->GetMedia();
        REQUIRE(persisted);
        CHECK(*persisted == EmbeddedVideo());
    }

    TEST_CASE("linked audio can replace embedded video without resolving the URI [unit] [powerpoint] [media]")
    {
        auto editor = PowerPointDocumentEditor::CreateNew();
        auto slide = editor->AddSlide();
        auto shape = slide->ShapeTree()->AddMedia(EmbeddedVideo());
        REQUIRE(shape);

        PresentationMediaData audio;
        audio.Kind = PresentationMediaKind::Audio;
        audio.LinkedUri = "https://media.example.test/podcast.mp3";
        audio.Playback.Volume = 75000;
        audio.Playback.ShowWhenStopped = false;
        audio.Name = "Linked podcast";
        audio.Transform.Size = {100, 100};
        REQUIRE(shape->SetMedia(audio));
        REQUIRE(shape->GetMedia());
        CHECK(*shape->GetMedia() == audio);

        const auto relationships = slide->GetPart()->Relationships();
        CHECK(std::count_if(relationships.begin(), relationships.end(), [](const auto& relationship)
                            { return relationship.IsExternal && relationship.Target == "https://media.example.test/podcast.mp3"; }) == 1);
    }

    TEST_CASE("invalid media changes are rejected without modifying the current payload [unit] [powerpoint] [media]")
    {
        auto editor = PowerPointDocumentEditor::CreateNew();
        auto tree = editor->AddSlide()->ShapeTree();
        auto shape = tree->AddMedia(EmbeddedVideo());
        REQUIRE(shape);
        const auto original = *shape->GetMedia();

        auto invalid = original;
        invalid.LinkedUri = "https://example.test/video.mp4";
        CHECK_FALSE(shape->SetMedia(invalid));
        invalid = original;
        invalid.Playback.Volume = 100001;
        CHECK_FALSE(shape->SetMedia(invalid));
        invalid = original;
        invalid.Embedded->Data.clear();
        CHECK_FALSE(shape->SetMedia(invalid));
        CHECK(*shape->GetMedia() == original);
        CHECK_FALSE(tree->AddShape()->GetMedia());
    }

    TEST_CASE("removing media removes source, poster, and timing relationships [unit] [powerpoint] [media]")
    {
        auto editor = PowerPointDocumentEditor::CreateNew();
        auto slide = editor->AddSlide();
        auto shape = slide->ShapeTree()->AddMedia(EmbeddedVideo());
        REQUIRE(shape);
        CHECK(slide->GetPart()->Relationships().size() >= 2);
        REQUIRE(shape->Remove());
        CHECK(slide->ShapeTree()->Count() == 0);
        CHECK(slide->GetPart()->Relationships().empty());
    }

    TEST_CASE("media pictures support shape outlines and effects without changing playback [unit] [powerpoint] [media]")
    {
        auto editor = PowerPointDocumentEditor::CreateNew();
        auto shape = editor->AddSlide()->ShapeTree()->AddMedia(EmbeddedVideo());
        REQUIRE(shape != nullptr);
        const auto media = shape->GetMedia();
        REQUIRE(media.has_value());

        PresentationShapeOutline outline;
        outline.Fill = PresentationFillKind::Solid;
        outline.ColorValue = ExyokiOffice::Color(1, 2, 3);
        outline.Width = ExyokiOffice::MeasuringUnits(1.5, ExyokiOffice::MeasurementUnit::Point);
        REQUIRE(shape->SetOutline(outline));
        PresentationShapeEffects effects;
        effects.Glow = PresentationTextGlow{
            ExyokiOffice::MeasuringUnits(2.0, ExyokiOffice::MeasurementUnit::Point),
            ExyokiOffice::Color(4, 5, 6)};
        REQUIRE(shape->SetEffects(effects));
        CHECK(shape->GetMedia() == media);
        CHECK(ExyokiOffice::OpenXmlPackageValidator().Validate(*editor->GetDocument()).IsValid());

        auto reopened = PowerPointDocumentEditor::Open(editor->SaveToMemory());
        REQUIRE(reopened != nullptr);
        auto persisted = reopened->GetSlide(0)->ShapeTree()->Get(0);
        REQUIRE(persisted != nullptr);
        CHECK(persisted->GetMedia() == media);
        CHECK(persisted->GetOutline() == outline);
        CHECK(persisted->GetEffects() == effects);
    }

    TEST_CASE("media files can be embedded and replaced while retaining presentation metadata [unit] [powerpoint] [media]")
    {
        const auto path = ExyokiOfficeTests::MakeTemporaryPath("exyokioffice-powerpoint-media", ".bin");
        {
            std::ofstream file(path, std::ios::binary | std::ios::trunc);
            const std::vector<ExyokiOffice::Byte> bytes{1, 2, 3, 4};
            file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        }

        auto editor = PowerPointDocumentEditor::CreateNew();
        auto slide = editor->AddSlide();
        PresentationShapeTransform transform;
        transform.Position = {100, 200};
        transform.Size = {300, 400};
        PresentationMediaPlayback playback;
        playback.Loop = true;
        auto media = slide->ShapeTree()->AddMediaFromFile(
            PresentationMediaKind::Audio, path, "audio/mpeg", transform, std::nullopt, playback, "Narration");
        REQUIRE(media != nullptr);
        auto inserted = media->GetMedia();
        REQUIRE(inserted.has_value());
        REQUIRE(inserted->Embedded.has_value());
        CHECK(inserted->Embedded->Data == std::vector<ExyokiOffice::Byte>{1, 2, 3, 4});
        CHECK(inserted->Playback == playback);
        CHECK(inserted->Transform == transform);
        CHECK(inserted->Name == "Narration");

        {
            std::ofstream file(path, std::ios::binary | std::ios::trunc);
            const std::vector<ExyokiOffice::Byte> bytes{9, 8, 7};
            file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        }
        REQUIRE(media->ReplaceMediaFromFile(path, "audio/wav"));
        auto replaced = media->GetMedia();
        REQUIRE(replaced.has_value());
        REQUIRE(replaced->Embedded.has_value());
        CHECK(replaced->Embedded->Data == std::vector<ExyokiOffice::Byte>{9, 8, 7});
        CHECK(replaced->Embedded->ContentType == "audio/wav");
        CHECK(replaced->Playback == playback);
        CHECK(replaced->Transform == transform);

        std::error_code error;
        std::filesystem::remove(path, error);
        CHECK_FALSE(error);
    }
} // TEST_SUITE("PowerPointMediaTests")
