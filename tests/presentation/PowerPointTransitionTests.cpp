// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "doctest.h"

#include "ExyokiOffice/OpenXmlPackageValidator.hpp"
#include "ExyokiOffice/PowerPoint/PowerPointDocument.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

using namespace ExyokiOffice;
using namespace ExyokiOffice::PowerPoint;
namespace Presentation = ExyokiOffice::DocumentFormat::OpenXml::Presentation;

class PowerPointTransitionTestHelpers
{
public:
    static PresentationTransitionData FadeWithSound()
    {
        PresentationTransitionData value;
        value.Kind = PresentationTransitionKind::Fade;
        value.Speed = PresentationTransitionSpeed::Slow;
        value.Duration = 875;
        value.AdvanceOnClick = false;
        value.AdvanceAfter = 4200;
        value.Sound = PresentationTransitionSound{
            {{'R', 'I', 'F', 'F', 1, 2, 3, 4}, "audio/wav"}, "Chime", false, true};
        return value;
    }

    /// Injects an effect element from a namespace the library does not model.
    static bool InjectForeignEffect(const PresentationSlide::Ptr& slide)
    {
        auto part = slide->GetPart();
        auto xml = part->GetXmlString();
        const auto marker = xml.rfind("</p:sld>");
        if (marker == std::string::npos)
        {
            return false;
        }
        xml.insert(marker,
                   R"(<p:transition spd="med"><x:mystery xmlns:x="urn:example:transition" token="keep-me"/></p:transition>)");
        part->SetXmlString(xml);
        return true;
    }

    static std::string ForeignToken(const PresentationSlide::Ptr& slide)
    {
        const auto xml = slide->GetPart()->GetXmlString();
        return xml.find("keep-me") == std::string::npos ? std::string{} : std::string("keep-me");
    }

    static PresentationTransitionData Effect(PresentationTransitionKind kind,
                                             const PresentationTransitionOptions& options)
    {
        PresentationTransitionData value;
        value.Kind = kind;
        value.Duration = 700;
        value.Options = options;
        return value;
    }

    static PresentationTransitionOptions Direction(PresentationTransitionDirection direction)
    {
        PresentationTransitionOptions options;
        options.Direction = direction;
        return options;
    }

    static PresentationTransitionOptions Orientation(PresentationTransitionOrientation orientation)
    {
        PresentationTransitionOptions options;
        options.Orientation = orientation;
        return options;
    }
};

TEST_SUITE("PowerPointTransitionTests")
{
    using Helpers = PowerPointTransitionTestHelpers;

    TEST_CASE("transition timing, advance, and embedded sound survive a round trip [unit] [powerpoint] [transition]")
    {
        auto editor = PowerPointDocumentEditor::CreateNew();
        auto slide = editor->AddSlide();
        REQUIRE(slide->SetTransition(Helpers::FadeWithSound()));
        REQUIRE(slide->GetTransition());
        CHECK(*slide->GetTransition() == Helpers::FadeWithSound());

        auto reopened = PowerPointDocumentEditor::Open(editor->SaveToMemory());
        REQUIRE(reopened);
        auto persisted = reopened->GetSlide(0)->GetTransition();
        REQUIRE(persisted);
        CHECK(*persisted == Helpers::FadeWithSound());
    }

    TEST_CASE("supported transition type and common metadata can be replaced [unit] [powerpoint] [transition]")
    {
        auto editor = PowerPointDocumentEditor::CreateNew();
        auto slide = editor->AddSlide();
        REQUIRE(slide->SetTransition(Helpers::FadeWithSound()));

        PresentationTransitionData replacement;
        replacement.Kind = PresentationTransitionKind::Wipe;
        replacement.Speed = PresentationTransitionSpeed::Fast;
        replacement.AdvanceOnClick = true;
        REQUIRE(slide->SetTransition(replacement));
        REQUIRE(slide->GetTransition());
        CHECK(*slide->GetTransition() == replacement);
        CHECK(slide->GetPart()->Relationships().empty());
    }

    TEST_CASE("every transition effect round trips together with its own option family [unit] [powerpoint] [transition]")
    {
        struct Sample
        {
            PresentationTransitionKind Kind;
            PresentationTransitionOptions Options;
        };
        const std::vector<Sample> samples{
            {PresentationTransitionKind::Blinds, Helpers::Orientation(PresentationTransitionOrientation::Vertical)},
            {PresentationTransitionKind::Checker, Helpers::Orientation(PresentationTransitionOrientation::Horizontal)},
            {PresentationTransitionKind::Circle, {}},
            {PresentationTransitionKind::Comb, Helpers::Orientation(PresentationTransitionOrientation::Vertical)},
            {PresentationTransitionKind::Cover, Helpers::Direction(PresentationTransitionDirection::RightDown)},
            {PresentationTransitionKind::Cut, {}},
            {PresentationTransitionKind::Diamond, {}},
            {PresentationTransitionKind::Dissolve, {}},
            {PresentationTransitionKind::Fade, {}},
            {PresentationTransitionKind::Newsflash, {}},
            {PresentationTransitionKind::Plus, {}},
            {PresentationTransitionKind::Pull, Helpers::Direction(PresentationTransitionDirection::LeftUp)},
            {PresentationTransitionKind::Push, Helpers::Direction(PresentationTransitionDirection::Down)},
            {PresentationTransitionKind::Random, {}},
            {PresentationTransitionKind::RandomBar, Helpers::Orientation(PresentationTransitionOrientation::Horizontal)},
            {PresentationTransitionKind::Split, {}},
            {PresentationTransitionKind::Strips, Helpers::Direction(PresentationTransitionDirection::LeftDown)},
            {PresentationTransitionKind::Wedge, {}},
            {PresentationTransitionKind::Wheel, {}},
            {PresentationTransitionKind::Wipe, Helpers::Direction(PresentationTransitionDirection::Up)},
            {PresentationTransitionKind::Zoom, Helpers::Direction(PresentationTransitionDirection::In)},
        };

        auto editor = PowerPointDocumentEditor::CreateNew();
        std::vector<PresentationTransitionData> expected;
        for (const auto& sample : samples)
        {
            auto slide = editor->AddSlide();
            REQUIRE(slide);
            const auto value = Helpers::Effect(sample.Kind, sample.Options);
            REQUIRE(slide->SetTransition(value));
            REQUIRE(slide->GetTransition());
            CHECK(*slide->GetTransition() == value);
            expected.push_back(value);
        }

        auto reopened = PowerPointDocumentEditor::Open(editor->SaveToMemory());
        REQUIRE(reopened);
        for (Size index = 0; index < expected.size(); ++index)
        {
            auto persisted = reopened->GetSlide(index)->GetTransition();
            REQUIRE(persisted);
            CHECK(*persisted == expected[index]);
        }
        CHECK(ExyokiOffice::OpenXmlPackageValidator().Validate(*reopened->GetDocument()).IsValid());
    }

    TEST_CASE("transition options exclusive to other effects are refused [unit] [powerpoint] [transition]")
    {
        auto editor = PowerPointDocumentEditor::CreateNew();
        auto slide = editor->AddSlide();

        PresentationTransitionOptions throughBlack;
        throughBlack.ThroughBlack = true;
        CHECK(slide->SetTransition(Helpers::Effect(PresentationTransitionKind::Fade, throughBlack)));
        CHECK_FALSE(slide->SetTransition(Helpers::Effect(PresentationTransitionKind::Wipe, throughBlack)));

        // Wipe takes side directions only, Strips corner directions only, Zoom in/out only.
        CHECK_FALSE(slide->SetTransition(
            Helpers::Effect(PresentationTransitionKind::Wipe, Helpers::Direction(PresentationTransitionDirection::LeftUp))));
        CHECK_FALSE(slide->SetTransition(
            Helpers::Effect(PresentationTransitionKind::Strips, Helpers::Direction(PresentationTransitionDirection::Up))));
        CHECK_FALSE(slide->SetTransition(
            Helpers::Effect(PresentationTransitionKind::Zoom, Helpers::Direction(PresentationTransitionDirection::Left))));
        CHECK_FALSE(slide->SetTransition(Helpers::Effect(PresentationTransitionKind::Circle,
                                                         Helpers::Orientation(PresentationTransitionOrientation::Vertical))));

        PresentationTransitionOptions spokes;
        spokes.Spokes = 0;
        CHECK_FALSE(slide->SetTransition(Helpers::Effect(PresentationTransitionKind::Wheel, spokes)));
        spokes.Spokes = 6;
        CHECK(slide->SetTransition(Helpers::Effect(PresentationTransitionKind::Wheel, spokes)));
        CHECK_FALSE(slide->SetTransition(Helpers::Effect(PresentationTransitionKind::Blinds, spokes)));

        // The last accepted write is the one that survived every rejection.
        REQUIRE(slide->GetTransition());
        CHECK(slide->GetTransition()->Kind == PresentationTransitionKind::Wheel);
        CHECK(slide->GetTransition()->Options.Spokes == 6);
    }

    TEST_CASE("split transition carries both an orientation and an in/out direction [unit] [powerpoint] [transition]")
    {
        auto editor = PowerPointDocumentEditor::CreateNew();
        auto slide = editor->AddSlide();
        PresentationTransitionOptions options;
        options.Orientation = PresentationTransitionOrientation::Vertical;
        options.Direction = PresentationTransitionDirection::Out;
        const auto value = Helpers::Effect(PresentationTransitionKind::Split, options);
        REQUIRE(slide->SetTransition(value));
        CHECK(*slide->GetTransition() == value);

        auto invalid = options;
        invalid.Direction = PresentationTransitionDirection::Right;
        CHECK_FALSE(slide->SetTransition(Helpers::Effect(PresentationTransitionKind::Split, invalid)));
        CHECK(*slide->GetTransition() == value);
    }

    TEST_CASE("unsupported transition effect remains opaque while common metadata is edited [unit] [powerpoint] [transition]")
    {
        auto editor = PowerPointDocumentEditor::CreateNew();
        auto slide = editor->AddSlide();
        REQUIRE(Helpers::InjectForeignEffect(slide));

        auto value = slide->GetTransition();
        REQUIRE(value);
        CHECK(value->Kind == PresentationTransitionKind::Unsupported);
        value->Duration = 1234;
        value->AdvanceAfter = 9000;
        REQUIRE(slide->SetTransition(*value));
        CHECK(Helpers::ForeignToken(slide) == "keep-me");
        CHECK(slide->GetTransition()->Duration == 1234);
        CHECK(slide->GetTransition()->AdvanceAfter == 9000);

        auto reopened = PowerPointDocumentEditor::Open(editor->SaveToMemory());
        REQUIRE(reopened);
        CHECK(Helpers::ForeignToken(reopened->GetSlide(0)) == "keep-me");
        CHECK(reopened->GetSlide(0)->GetTransition()->Kind == PresentationTransitionKind::Unsupported);
        CHECK(reopened->GetSlide(0)->GetTransition()->Duration == 1234);
    }

    TEST_CASE("transition validation and removal are transactional [unit] [powerpoint] [transition]")
    {
        auto editor = PowerPointDocumentEditor::CreateNew();
        auto slide = editor->AddSlide();
        PresentationTransitionData unsupported;
        unsupported.Kind = PresentationTransitionKind::Unsupported;
        CHECK_FALSE(slide->SetTransition(unsupported));
        CHECK_FALSE(slide->GetTransition());

        REQUIRE(slide->SetTransition(Helpers::FadeWithSound()));
        const auto original = *slide->GetTransition();
        auto invalid = original;
        invalid.Sound->Audio.Data.clear();
        CHECK_FALSE(slide->SetTransition(invalid));
        CHECK(*slide->GetTransition() == original);

        REQUIRE(slide->RemoveTransition());
        CHECK_FALSE(slide->GetTransition());
        CHECK(slide->GetPart()->Relationships().empty());
        CHECK_FALSE(slide->RemoveTransition());
    }
} // TEST_SUITE("PowerPointTransitionTests")
