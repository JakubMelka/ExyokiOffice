// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "doctest.h"

#include "ExyokiOffice/PowerPoint/PowerPointDocument.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

using namespace ExyokiOffice::PowerPoint;

namespace
{
PresentationAnimationNode AnimationFor(ExyokiOffice::UInt32 shapeId)
{
    PresentationAnimationNode value;
    value.TargetShapeId = shapeId;
    value.Duration = 650;
    value.From = "0";
    value.To = "1";
    value.By = "0.25";
    return value;
}

PresentationMediaData Video()
{
    PresentationMediaData value;
    value.Kind = PresentationMediaKind::Video;
    value.Embedded = PresentationEmbeddedMedia{{0, 0, 0, 24, 'f', 't', 'y', 'p'}, "video/mp4"};
    value.Transform.Size = {100, 100};
    return value;
}
} // namespace

TEST_SUITE("PowerPointAnimationTests")
{
    TEST_CASE("animation nodes retain stable IDs, targets, values, and order across a round trip [unit] [powerpoint] [animation]")
    {
        auto editor = PowerPointDocumentEditor::CreateNew();
        auto slide = editor->AddSlide();
        auto firstShape = slide->ShapeTree()->AddShape("First");
        auto secondShape = slide->ShapeTree()->AddShape("Second");
        REQUIRE(firstShape);
        REQUIRE(secondShape);

        auto first = AnimationFor(firstShape->Id());
        auto firstId = slide->AddAnimation(first);
        REQUIRE(firstId);
        first.Id = *firstId;
        auto second = AnimationFor(secondShape->Id());
        second.Id = 40;
        second.Duration.reset();
        second.From = "10";
        REQUIRE(slide->AddAnimation(second) == 40);
        CHECK(slide->Animations() == std::vector<PresentationAnimationNode>{first, second});

        auto reopened = PowerPointDocumentEditor::Open(editor->SaveToMemory());
        REQUIRE(reopened);
        CHECK(reopened->GetSlide(0)->Animations() == std::vector<PresentationAnimationNode>{first, second});
    }

    TEST_CASE("animation CRUD validates timing IDs and target shape IDs transactionally [unit] [powerpoint] [animation]")
    {
        auto editor = PowerPointDocumentEditor::CreateNew();
        auto slide = editor->AddSlide();
        auto shape = slide->ShapeTree()->AddShape();
        REQUIRE(shape);
        auto value = AnimationFor(shape->Id());
        value.Id = 7;
        REQUIRE(slide->AddAnimation(value) == 7);

        CHECK_FALSE(slide->AddAnimation(value));
        auto invalid = value;
        invalid.TargetShapeId = 999999;
        CHECK_FALSE(slide->UpdateAnimation(7, invalid));
        invalid = value;
        invalid.Id = 8;
        CHECK_FALSE(slide->UpdateAnimation(7, invalid));
        CHECK_FALSE(slide->UpdateAnimation(999, value));
        CHECK(slide->Animations() == std::vector<PresentationAnimationNode>{value});

        value.Duration = 1200;
        value.To = "2";
        REQUIRE(slide->UpdateAnimation(7, value));
        CHECK(slide->Animations() == std::vector<PresentationAnimationNode>{value});
        REQUIRE(slide->RemoveAnimation(7));
        CHECK(slide->Animations().empty());
        CHECK_FALSE(slide->RemoveAnimation(7));
    }

    TEST_CASE("shape removal warns or cascades according to animation dependency policy [unit] [powerpoint] [animation]")
    {
        auto editor = PowerPointDocumentEditor::CreateNew();
        auto slide = editor->AddSlide();
        auto shape = slide->ShapeTree()->AddShape("Animated");
        REQUIRE(shape);
        REQUIRE(slide->AddAnimation(AnimationFor(shape->Id())));

        CHECK(shape->RemoveWithAnimationPolicy(PresentationAnimationRemovalPolicy::Warn) ==
              PresentationShapeRemovalResult::AnimationDependencyWarning);
        CHECK(slide->ShapeTree()->Count() == 1);
        CHECK(slide->Animations().size() == 1);
        CHECK(shape->RemoveWithAnimationPolicy(PresentationAnimationRemovalPolicy::RemoveDependentAnimations) ==
              PresentationShapeRemovalResult::Removed);
        CHECK(slide->ShapeTree()->Count() == 0);
        CHECK(slide->Animations().empty());
        CHECK(shape->RemoveWithAnimationPolicy(PresentationAnimationRemovalPolicy::Warn) ==
              PresentationShapeRemovalResult::NotAttached);
    }

    TEST_CASE("animation CRUD does not expose or remove media timing nodes [unit] [powerpoint] [animation]")
    {
        auto editor = PowerPointDocumentEditor::CreateNew();
        auto slide = editor->AddSlide();
        auto media = slide->ShapeTree()->AddMedia(Video());
        auto shape = slide->ShapeTree()->AddShape();
        REQUIRE(media);
        REQUIRE(shape);
        CHECK(slide->Animations().empty());
        auto id = slide->AddAnimation(AnimationFor(shape->Id()));
        REQUIRE(id);
        CHECK(slide->Animations().size() == 1);
        REQUIRE(slide->RemoveAnimation(*id));
        CHECK(slide->Animations().empty());
        REQUIRE(media->GetMedia());
    }
} // TEST_SUITE("PowerPointAnimationTests")
