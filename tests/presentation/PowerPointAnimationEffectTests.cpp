// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "doctest.h"

#include "ExyokiOffice/OpenXmlPackageValidator.hpp"
#include "ExyokiOffice/PowerPoint/PowerPointDocument.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <string>
#include <vector>

using namespace ExyokiOffice;
using namespace ExyokiOffice::PowerPoint;

class PowerPointAnimationEffectTestHelpers
{
public:
    /// Adds `count` plain shapes and returns their non-visual identifiers.
    static std::vector<UInt32> Shapes(const PresentationSlide::Ptr& slide, Size count)
    {
        std::vector<UInt32> result;
        auto tree = slide->ShapeTree();
        for (Size index = 0; index < count; ++index)
        {
            auto shape = tree->AddShape("Shape " + std::to_string(index));
            result.push_back(shape ? shape->Id() : 0);
        }
        return result;
    }

    static PresentationAnimationEffectData Entrance(UInt32 shapeId, PresentationAnimationEffect effect,
                                                    std::optional<PresentationAnimationDirection> direction = {})
    {
        PresentationAnimationEffectData data;
        data.TargetShapeId = shapeId;
        data.Class = PresentationAnimationEffectClass::Entrance;
        data.Effect = effect;
        data.Direction = direction;
        return data;
    }

    static PresentationAnimationEffectData Exit(UInt32 shapeId, PresentationAnimationEffect effect,
                                                std::optional<PresentationAnimationDirection> direction = {})
    {
        auto data = Entrance(shapeId, effect, direction);
        data.Class = PresentationAnimationEffectClass::Exit;
        return data;
    }

    static PresentationAnimationEffectData Grow(UInt32 shapeId, Int32 percent)
    {
        PresentationAnimationEffectData data;
        data.TargetShapeId = shapeId;
        data.Class = PresentationAnimationEffectClass::Emphasis;
        data.Effect = PresentationAnimationEffect::GrowShrink;
        data.ScalePercent = percent;
        return data;
    }

    static PresentationAnimationEffectData Spin(UInt32 shapeId, Int32 degrees)
    {
        PresentationAnimationEffectData data;
        data.TargetShapeId = shapeId;
        data.Class = PresentationAnimationEffectClass::Emphasis;
        data.Effect = PresentationAnimationEffect::Spin;
        data.RotationDegrees = degrees;
        return data;
    }

    static PresentationAnimationEffectData Recolor(UInt32 shapeId, const std::string& color)
    {
        PresentationAnimationEffectData data;
        data.TargetShapeId = shapeId;
        data.Class = PresentationAnimationEffectClass::Emphasis;
        data.Effect = PresentationAnimationEffect::ChangeFillColor;
        data.Color = color;
        return data;
    }

    static PresentationAnimationEffectData Path(UInt32 shapeId, const std::string& path)
    {
        PresentationAnimationEffectData data;
        data.TargetShapeId = shapeId;
        data.Class = PresentationAnimationEffectClass::MotionPath;
        data.Effect = PresentationAnimationEffect::MotionPath;
        data.MotionPath = path;
        return data;
    }

    static Size Occurrences(const std::string& text, const std::string& needle)
    {
        Size count = 0;
        for (auto position = text.find(needle); position != std::string::npos;
             position = text.find(needle, position + needle.size()))
        {
            ++count;
        }
        return count;
    }

    static std::string Xml(const PresentationSlide::Ptr& slide)
    {
        return slide->GetPart()->GetXmlString();
    }

    /// Rewrites one preset identifier so the effect stops being recognized by the API.
    static bool Obfuscate(const PresentationSlide::Ptr& slide)
    {
        auto part = slide->GetPart();
        auto xml = part->GetXmlString();
        const auto position = xml.find("presetID=\"10\"");
        if (position == std::string::npos)
        {
            return false;
        }
        xml.replace(position, std::string("presetID=\"10\"").size(), "presetID=\"99\"");
        part->SetXmlString(xml);
        return true;
    }
};

TEST_SUITE("PowerPointAnimationEffectTests")
{
    using Helpers = PowerPointAnimationEffectTestHelpers;

    TEST_CASE("entrance, emphasis, exit, and motion-path effects round trip [unit] [powerpoint] [animation]")
    {
        auto editor = PowerPointDocumentEditor::CreateNew();
        auto slide = editor->AddSlide();
        const auto shapes = Helpers::Shapes(slide, 3);
        REQUIRE(shapes.size() == 3);

        std::vector<PresentationAnimationEffectData> effects{
            Helpers::Entrance(shapes[0], PresentationAnimationEffect::Appear),
            Helpers::Entrance(shapes[0], PresentationAnimationEffect::Fade),
            Helpers::Entrance(shapes[1], PresentationAnimationEffect::Fly, PresentationAnimationDirection::Left),
            Helpers::Entrance(shapes[1], PresentationAnimationEffect::Wipe, PresentationAnimationDirection::Down),
            Helpers::Entrance(shapes[2], PresentationAnimationEffect::Zoom, PresentationAnimationDirection::In),
            Helpers::Grow(shapes[0], 150),
            Helpers::Spin(shapes[1], -90),
            Helpers::Recolor(shapes[2], "FF8800"),
            Helpers::Path(shapes[0], "M 0 0 L 0.25 0.5 E"),
            Helpers::Exit(shapes[1], PresentationAnimationEffect::Fade),
            Helpers::Exit(shapes[2], PresentationAnimationEffect::Fly, PresentationAnimationDirection::Right),
            Helpers::Exit(shapes[0], PresentationAnimationEffect::Appear),
            Helpers::Exit(shapes[0], PresentationAnimationEffect::Zoom, PresentationAnimationDirection::Out),
            Helpers::Exit(shapes[1], PresentationAnimationEffect::Wipe, PresentationAnimationDirection::Up),
        };
        REQUIRE(slide->SetAnimationEffects(effects));

        auto stored = slide->AnimationEffects();
        REQUIRE(stored.size() == effects.size());
        for (Size index = 0; index < effects.size(); ++index)
        {
            auto expected = effects[index];
            expected.Id = stored[index].Id;
            CHECK(stored[index] == expected);
            CHECK(stored[index].Id != 0);
        }

        auto reopened = PowerPointDocumentEditor::Open(editor->SaveToMemory());
        REQUIRE(reopened);
        CHECK(reopened->GetSlide(0)->AnimationEffects() == stored);
        CHECK(ExyokiOffice::OpenXmlPackageValidator().Validate(*reopened->GetDocument()).IsValid());
    }

    TEST_CASE("effect timing is stored on the effect node and round trips [unit] [powerpoint] [animation]")
    {
        auto editor = PowerPointDocumentEditor::CreateNew();
        auto slide = editor->AddSlide();
        const auto shapes = Helpers::Shapes(slide, 1);

        auto effect = Helpers::Entrance(shapes[0], PresentationAnimationEffect::Fade);
        effect.Timing.Delay = 250;
        effect.Timing.Duration = 1750;
        effect.Timing.RepeatCount = 3;
        effect.Timing.AutoReverse = true;
        effect.Timing.Acceleration = 20000;
        effect.Timing.Deceleration = 30000;
        const auto id = slide->AddAnimationEffect(effect);
        REQUIRE(id);

        auto stored = slide->AnimationEffects();
        REQUIRE(stored.size() == 1);
        CHECK(stored[0].Timing == effect.Timing);

        const auto xml = Helpers::Xml(slide);
        CHECK(xml.find("dur=\"1750\"") != std::string::npos);
        CHECK(xml.find("repeatCount=\"3000\"") != std::string::npos);
        CHECK(xml.find("accel=\"20000\"") != std::string::npos);
        CHECK(xml.find("decel=\"30000\"") != std::string::npos);
        CHECK(xml.find("delay=\"250\"") != std::string::npos);

        auto indefinite = effect;
        indefinite.Timing.RepeatCount.reset();
        indefinite.Timing.RepeatIndefinitely = true;
        REQUIRE(slide->UpdateAnimationEffect(*id, indefinite));
        CHECK(slide->AnimationEffects().at(0).Timing == indefinite.Timing);

        auto reopened = PowerPointDocumentEditor::Open(editor->SaveToMemory());
        REQUIRE(reopened);
        CHECK(reopened->GetSlide(0)->AnimationEffects().at(0).Timing == indefinite.Timing);
    }

    TEST_CASE("triggers build click, with-previous, and after-previous groups [unit] [powerpoint] [animation]")
    {
        auto editor = PowerPointDocumentEditor::CreateNew();
        auto slide = editor->AddSlide();
        const auto shapes = Helpers::Shapes(slide, 4);

        std::vector<PresentationAnimationEffectData> effects;
        for (Size index = 0; index < 4; ++index)
        {
            effects.push_back(Helpers::Entrance(shapes[index], PresentationAnimationEffect::Fade));
        }
        effects[1].Trigger = PresentationAnimationTrigger::WithPrevious;
        effects[2].Trigger = PresentationAnimationTrigger::AfterPrevious;
        effects[3].Trigger = PresentationAnimationTrigger::OnClick;
        REQUIRE(slide->SetAnimationEffects(effects));

        auto stored = slide->AnimationEffects();
        REQUIRE(stored.size() == 4);
        CHECK(stored[0].Trigger == PresentationAnimationTrigger::OnClick);
        CHECK(stored[1].Trigger == PresentationAnimationTrigger::WithPrevious);
        CHECK(stored[2].Trigger == PresentationAnimationTrigger::AfterPrevious);
        CHECK(stored[3].Trigger == PresentationAnimationTrigger::OnClick);

        const auto xml = Helpers::Xml(slide);
        CHECK(Helpers::Occurrences(xml, "nodeType=\"clickEffect\"") == 2);
        CHECK(Helpers::Occurrences(xml, "nodeType=\"withEffect\"") == 1);
        CHECK(Helpers::Occurrences(xml, "nodeType=\"afterEffect\"") == 1);
        CHECK(Helpers::Occurrences(xml, "nodeType=\"mainSeq\"") == 1);
        CHECK(Helpers::Occurrences(xml, "nodeType=\"tmRoot\"") == 1);
        // Two click groups, each holding a timing group; the after-previous effect adds a third.
        CHECK(Helpers::Occurrences(xml, "delay=\"indefinite\"") == 2);
    }

    TEST_CASE("shape-triggered effects live in their own interactive sequence [unit] [powerpoint] [animation]")
    {
        auto editor = PowerPointDocumentEditor::CreateNew();
        auto slide = editor->AddSlide();
        const auto shapes = Helpers::Shapes(slide, 3);

        auto main = Helpers::Entrance(shapes[0], PresentationAnimationEffect::Fade);
        auto triggered = Helpers::Grow(shapes[1], 120);
        triggered.TriggerShapeId = shapes[2];
        auto alsoTriggered = Helpers::Spin(shapes[1], 360);
        alsoTriggered.TriggerShapeId = shapes[2];
        alsoTriggered.Trigger = PresentationAnimationTrigger::WithPrevious;
        REQUIRE(slide->SetAnimationEffects({main, triggered, alsoTriggered}));

        auto stored = slide->AnimationEffects();
        REQUIRE(stored.size() == 3);
        CHECK(stored[0].TriggerShapeId == 0);
        CHECK(stored[1].TriggerShapeId == shapes[2]);
        CHECK(stored[2].TriggerShapeId == shapes[2]);

        const auto xml = Helpers::Xml(slide);
        CHECK(Helpers::Occurrences(xml, "nodeType=\"mainSeq\"") == 1);
        CHECK(Helpers::Occurrences(xml, "nodeType=\"interactiveSeq\"") == 1);
        CHECK(xml.find("evt=\"onClick\"") != std::string::npos);

        auto reopened = PowerPointDocumentEditor::Open(editor->SaveToMemory());
        REQUIRE(reopened);
        CHECK(reopened->GetSlide(0)->AnimationEffects() == stored);
    }

    TEST_CASE("effect list supports add, update, move, remove, and clear [unit] [powerpoint] [animation]")
    {
        auto editor = PowerPointDocumentEditor::CreateNew();
        auto slide = editor->AddSlide();
        const auto shapes = Helpers::Shapes(slide, 2);

        const auto first = slide->AddAnimationEffect(Helpers::Entrance(shapes[0], PresentationAnimationEffect::Fade));
        const auto second = slide->AddAnimationEffect(Helpers::Grow(shapes[1], 200));
        REQUIRE(first);
        REQUIRE(second);
        CHECK(*first != *second);
        REQUIRE(slide->AnimationEffects().size() == 2);
        CHECK(slide->AnimationEffects().at(0).Id == *first);

        auto replacement = Helpers::Exit(shapes[0], PresentationAnimationEffect::Wipe, PresentationAnimationDirection::Right);
        replacement.Timing.Duration = 900;
        REQUIRE(slide->UpdateAnimationEffect(*first, replacement));
        auto stored = slide->AnimationEffects();
        REQUIRE(stored.size() == 2);
        CHECK(stored[0].Id == *first);
        CHECK(stored[0].Effect == PresentationAnimationEffect::Wipe);
        CHECK(stored[0].Class == PresentationAnimationEffectClass::Exit);
        CHECK(stored[0].Direction == PresentationAnimationDirection::Right);
        CHECK(stored[0].Timing.Duration == 900);

        REQUIRE(slide->MoveAnimationEffect(*first, 1));
        CHECK(slide->AnimationEffects().at(0).Id == *second);
        CHECK(slide->AnimationEffects().at(1).Id == *first);
        CHECK_FALSE(slide->MoveAnimationEffect(*first, 2));
        CHECK_FALSE(slide->MoveAnimationEffect(9999, 0));

        REQUIRE(slide->RemoveAnimationEffect(*second));
        REQUIRE(slide->AnimationEffects().size() == 1);
        CHECK(slide->AnimationEffects().at(0).Id == *first);
        CHECK_FALSE(slide->RemoveAnimationEffect(*second));

        REQUIRE(slide->ClearAnimationEffects());
        CHECK(slide->AnimationEffects().empty());
        CHECK(Helpers::Xml(slide).find("nodeType=\"mainSeq\"") == std::string::npos);
        CHECK(slide->ClearAnimationEffects());
    }

    TEST_CASE("invalid effects are rejected without modifying the slide [unit] [powerpoint] [animation]")
    {
        auto editor = PowerPointDocumentEditor::CreateNew();
        auto slide = editor->AddSlide();
        const auto shapes = Helpers::Shapes(slide, 2);

        const auto valid = Helpers::Entrance(shapes[0], PresentationAnimationEffect::Fade);
        REQUIRE(slide->AddAnimationEffect(valid));
        const auto baseline = slide->AnimationEffects();
        REQUIRE(baseline.size() == 1);

        SUBCASE("unknown target and trigger shapes")
        {
            CHECK_FALSE(slide->AddAnimationEffect(Helpers::Entrance(999999, PresentationAnimationEffect::Fade)));
            auto trigger = Helpers::Entrance(shapes[1], PresentationAnimationEffect::Fade);
            trigger.TriggerShapeId = 999999;
            CHECK_FALSE(slide->AddAnimationEffect(trigger));
        }
        SUBCASE("effects outside their gallery")
        {
            CHECK_FALSE(slide->AddAnimationEffect(Helpers::Entrance(shapes[1], PresentationAnimationEffect::Spin)));
            auto emphasis = Helpers::Grow(shapes[1], 120);
            emphasis.Class = PresentationAnimationEffectClass::Entrance;
            CHECK_FALSE(slide->AddAnimationEffect(emphasis));
            auto path = Helpers::Path(shapes[1], "M 0 0 L 1 1 E");
            path.Class = PresentationAnimationEffectClass::Emphasis;
            CHECK_FALSE(slide->AddAnimationEffect(path));
        }
        SUBCASE("mismatched effect parameters")
        {
            CHECK_FALSE(slide->AddAnimationEffect(Helpers::Entrance(shapes[1], PresentationAnimationEffect::Fly)));
            CHECK_FALSE(slide->AddAnimationEffect(
                Helpers::Entrance(shapes[1], PresentationAnimationEffect::Fade, PresentationAnimationDirection::Left)));
            CHECK_FALSE(slide->AddAnimationEffect(
                Helpers::Entrance(shapes[1], PresentationAnimationEffect::Fly, PresentationAnimationDirection::In)));
            CHECK_FALSE(slide->AddAnimationEffect(
                Helpers::Entrance(shapes[1], PresentationAnimationEffect::Zoom, PresentationAnimationDirection::Up)));
            CHECK_FALSE(slide->AddAnimationEffect(Helpers::Grow(shapes[1], 0)));
            CHECK_FALSE(slide->AddAnimationEffect(Helpers::Recolor(shapes[1], "not-hex")));
            CHECK_FALSE(slide->AddAnimationEffect(Helpers::Recolor(shapes[1], "FFF")));
            CHECK_FALSE(slide->AddAnimationEffect(Helpers::Path(shapes[1], "")));
        }
        SUBCASE("out-of-range timing")
        {
            auto zeroDuration = Helpers::Entrance(shapes[1], PresentationAnimationEffect::Fade);
            zeroDuration.Timing.Duration = 0;
            CHECK_FALSE(slide->AddAnimationEffect(zeroDuration));
            auto singleRepeat = Helpers::Entrance(shapes[1], PresentationAnimationEffect::Fade);
            singleRepeat.Timing.RepeatCount = 1;
            CHECK_FALSE(slide->AddAnimationEffect(singleRepeat));
            auto conflicting = Helpers::Entrance(shapes[1], PresentationAnimationEffect::Fade);
            conflicting.Timing.RepeatCount = 2;
            conflicting.Timing.RepeatIndefinitely = true;
            CHECK_FALSE(slide->AddAnimationEffect(conflicting));
            auto easing = Helpers::Entrance(shapes[1], PresentationAnimationEffect::Fade);
            easing.Timing.Acceleration = 60000;
            easing.Timing.Deceleration = 60000;
            CHECK_FALSE(slide->AddAnimationEffect(easing));
        }
        SUBCASE("duplicate and mismatched identifiers")
        {
            auto duplicate = Helpers::Entrance(shapes[1], PresentationAnimationEffect::Fade);
            duplicate.Id = baseline[0].Id;
            CHECK_FALSE(slide->SetAnimationEffects({baseline[0], duplicate}));
            CHECK_FALSE(slide->UpdateAnimationEffect(baseline[0].Id + 1000, valid));
            auto renamed = valid;
            renamed.Id = baseline[0].Id + 1;
            CHECK_FALSE(slide->UpdateAnimationEffect(baseline[0].Id, renamed));
        }

        CHECK(slide->AnimationEffects() == baseline);
    }

    TEST_CASE("removing a shape warns about or cascades its animation effects [unit] [powerpoint] [animation]")
    {
        auto editor = PowerPointDocumentEditor::CreateNew();
        auto slide = editor->AddSlide();
        const auto shapes = Helpers::Shapes(slide, 2);
        auto animated = slide->ShapeTree()->Get(0);
        REQUIRE(animated);
        REQUIRE(slide->AddAnimationEffect(Helpers::Entrance(shapes[0], PresentationAnimationEffect::Fade)));
        REQUIRE(slide->AddAnimationEffect(Helpers::Grow(shapes[1], 130)));

        CHECK(animated->RemoveWithAnimationPolicy(PresentationAnimationRemovalPolicy::Warn) ==
              PresentationShapeRemovalResult::AnimationDependencyWarning);
        CHECK(slide->AnimationEffects().size() == 2);
        CHECK(animated->RemoveWithAnimationPolicy(PresentationAnimationRemovalPolicy::RemoveDependentAnimations) ==
              PresentationShapeRemovalResult::Removed);

        auto remaining = slide->AnimationEffects();
        REQUIRE(remaining.size() == 1);
        CHECK(remaining[0].TargetShapeId == shapes[1]);
        CHECK(slide->ShapeTree()->Count() == 1);
    }

    TEST_CASE("rebuilding preserves free-standing behaviors, media, and opaque effects [unit] [powerpoint] [animation]")
    {
        auto editor = PowerPointDocumentEditor::CreateNew();
        auto slide = editor->AddSlide();

        PresentationMediaData video;
        video.Kind = PresentationMediaKind::Video;
        video.Embedded = PresentationEmbeddedMedia{{0, 0, 0, 24, 'f', 't', 'y', 'p'}, "video/mp4"};
        video.Transform.Size = {100, 100};
        auto mediaShape = slide->ShapeTree()->AddMedia(video);
        REQUIRE(mediaShape);
        const auto shapes = Helpers::Shapes(slide, 2);

        PresentationAnimationNode raw;
        raw.TargetShapeId = shapes[0];
        raw.Duration = 400;
        raw.To = "1";
        const auto rawId = slide->AddAnimation(raw);
        REQUIRE(rawId);

        REQUIRE(slide->AddAnimationEffect(Helpers::Entrance(shapes[0], PresentationAnimationEffect::Fade)));
        REQUIRE(Helpers::Obfuscate(slide));

        auto opaque = slide->AnimationEffects();
        REQUIRE(opaque.size() == 1);
        CHECK(opaque[0].Effect == PresentationAnimationEffect::Unsupported);
        CHECK(opaque[0].Id != 0);

        // Appending next to an opaque effect keeps the original subtree byte-for-byte.
        REQUIRE(slide->AddAnimationEffect(Helpers::Grow(shapes[1], 175)));
        auto stored = slide->AnimationEffects();
        REQUIRE(stored.size() == 2);
        CHECK(stored[0].Effect == PresentationAnimationEffect::Unsupported);
        CHECK(stored[0].Id == opaque[0].Id);
        CHECK(stored[1].Effect == PresentationAnimationEffect::GrowShrink);
        CHECK(Helpers::Xml(slide).find("presetID=\"99\"") != std::string::npos);

        // Opaque effects cannot be replaced in place but can be removed.
        CHECK_FALSE(slide->UpdateAnimationEffect(opaque[0].Id,
                                                 Helpers::Entrance(shapes[0], PresentationAnimationEffect::Fade)));
        CHECK(slide->Animations().size() == 1);
        CHECK(slide->Animations().at(0).Id == *rawId);
        // The XML rewrite above replaced the DOM, so the media shape is re-read here.
        REQUIRE(slide->ShapeTree()->Get(0)->GetMedia());

        auto reopened = PowerPointDocumentEditor::Open(editor->SaveToMemory());
        REQUIRE(reopened);
        auto persisted = reopened->GetSlide(0);
        CHECK(persisted->AnimationEffects() == stored);
        CHECK(persisted->Animations().size() == 1);
        REQUIRE(persisted->ShapeTree()->Get(0)->GetMedia());

        REQUIRE(persisted->RemoveAnimationEffect(stored[0].Id));
        CHECK(persisted->AnimationEffects().size() == 1);
        CHECK(Helpers::Xml(persisted).find("presetID=\"99\"") == std::string::npos);
    }
} // TEST_SUITE("PowerPointAnimationEffectTests")
