// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

// ---------------------------------------------------------------------------
// The automaton in ContentModelAutomatonTests.cpp is checked against randomly
// generated content models, which is where a construction bug shows up as a
// counterexample small enough to read. This file asks a different question
// about the same code: does it hold up against the content models that actually
// exist?
//
// Every generated element class is compiled at several child counts and each
// result has to pass its own structural self-check. What the cases pin is not a
// verdict but two properties of the compilation: no imported model uses a
// construct the automaton cannot represent, and no imported model compiles into
// an automaton out of proportion to the document it is meant to match. Both are
// claims about the whole schema import, and both would go quietly wrong as the
// metadata is regenerated.
// ---------------------------------------------------------------------------

#include "doctest.h"

#include "ExyokiOffice/DOM/OpenXmlElementFactory.hpp"
#include "ExyokiOffice/MetadataBuilder.hpp"
#include "ExyokiOffice/OpenXMLElement.hpp"
#include "ExyokiOffice/StandardTypes.hpp"
#include "OpenXmlContentModel.hpp"

#include <algorithm>
#include <map>
#include <string>
#include <vector>

namespace ContentModelMetadataHelpers
{

using namespace ExyokiOffice;

/** Child counts a part written by Office plausibly reaches under one element. */
constexpr Size kRealisticChildCounts[] = {0, 1, 8, 256};

/** Far past anything Office writes, kept to see where the compilation gives out. */
constexpr Size kExtremeChildCount = 65536;

/** What one sweep over the metadata found. */
struct Survey
{
    Size Models = 0;
    Size Automata = 0;
    Size LargestPositionCount = 0;
    std::string LargestModel;
    /** Declines, keyed by the model and the reason, so a failure names both. */
    std::map<std::string, Size> Declined;
    std::vector<std::string> Problems;
};

Survey SurveyMetadata(OpenXml::FileFormatVersions version, const std::vector<Size>& childCounts)
{
    Survey survey;
    for (const auto* elementClass : Generated::OpenXmlElementFactory::AllElementClasses())
    {
        if (!elementClass)
        {
            continue;
        }
        const auto metadata = elementClass->GetMetadata();
        if (!metadata || !metadata->ParticleTree())
        {
            continue;
        }

        ++survey.Models;
        const auto typeName = std::string(elementClass->TypeQualifiedName().localName());
        for (const auto childCount : childCounts)
        {
            // The same choice of bound the validator's cache makes.
            const auto bound = OpenXmlContentModelAutomaton::NeedsOccurrenceBound(metadata->ParticleTree())
                                   ? OpenXmlContentModelAutomaton::RoundOccurrenceBound(childCount)
                                   : OpenXmlContentModelAutomaton::UnclampedBound;
            const OpenXmlContentModelAutomaton automaton(metadata->ParticleTree(), version, bound);
            ++survey.Automata;

            if (!automaton.IsSupported())
            {
                ++survey.Declined[typeName + " at " + std::to_string(childCount) + " children: " +
                                  automaton.UnsupportedReason()];
                continue;
            }
            for (const auto& problem : automaton.SelfCheck())
            {
                survey.Problems.push_back(typeName + ": " + problem);
            }
            if (automaton.PositionCount() > survey.LargestPositionCount)
            {
                survey.LargestPositionCount = automaton.PositionCount();
                survey.LargestModel = typeName + " at " + std::to_string(childCount) + " children";
            }
        }
    }
    return survey;
}

std::string Join(const std::vector<std::string>& lines, Size limit)
{
    std::string joined;
    for (Size index = 0; index < lines.size() && index < limit; ++index)
    {
        if (!joined.empty())
        {
            joined += " | ";
        }
        joined += lines[index];
    }
    return joined;
}

std::string Join(const std::map<std::string, Size>& counted)
{
    std::string joined;
    for (const auto& [reason, count] : counted)
    {
        if (!joined.empty())
        {
            joined += " | ";
        }
        joined += reason;
        if (count != 1)
        {
            joined += " (x" + std::to_string(count) + ")";
        }
    }
    return joined;
}

/** Declines that are the compilation running out of room rather than out of vocabulary. */
bool IsSizeLimit(const std::string& reason)
{
    return reason.find("would exceed") != std::string::npos;
}

std::vector<Size> RealisticChildCounts()
{
    return {std::begin(kRealisticChildCounts), std::end(kRealisticChildCounts)};
}

} // namespace ContentModelMetadataHelpers

TEST_SUITE("content model metadata")
{
    using namespace ContentModelMetadataHelpers;

    TEST_CASE("every imported content model compiles and passes its own self-check [unit] [metadata-content-model]")
    {
        const auto survey =
            SurveyMetadata(ExyokiOffice::OpenXml::FileFormatVersions::Microsoft365, RealisticChildCounts());

        // A few thousand types carry a content model; a number far below that
        // would mean the sweep stopped finding them rather than that they passed.
        CHECK(survey.Models > 1000);
        CHECK(survey.Automata == survey.Models * std::size(kRealisticChildCounts));

        // The self-check restates the invariants of the construction - every
        // transition lands on a position that exists, nothing points back at the
        // start, every position is an element or a wildcard - so a violation
        // here is a builder that produced a table the simulation cannot read.
        CHECK(Join(survey.Problems, 10) == "");

        // Nothing in the imported schemas is a construct the automaton cannot
        // represent. A decline here means the import has grown one, and the
        // validator would be quietly falling back to the matcher this whole
        // change exists to stop running.
        CHECK(Join(survey.Declined) == "");
    }

    TEST_CASE("an older target generation compiles the same models just as cleanly [unit] [metadata-content-model]")
    {
        // Compiling for an earlier Office removes the later particles from every
        // model, which is a different tree for the builder to walk - and one
        // that can leave a choice with no branches, or a sequence that has
        // become empty.
        const auto survey =
            SurveyMetadata(ExyokiOffice::OpenXml::FileFormatVersions::Office2007, RealisticChildCounts());
        CHECK(Join(survey.Problems, 10) == "");
        CHECK(Join(survey.Declined) == "");
    }

    TEST_CASE("a compiled model stays proportional to its document [unit] [metadata-content-model]")
    {
        // Repeating the body is the one compilation step that can grow with the
        // document, and it is clamped to the child count so that
        // `maxOccurs="65430"` on `x:xf` costs one position per cell format
        // actually present rather than 65430 whatever the document holds. The
        // largest model at 65536 children therefore has to stay near that
        // figure and not near the numbers the schemas write down.
        const auto survey = SurveyMetadata(ExyokiOffice::OpenXml::FileFormatVersions::Microsoft365,
                                           {kExtremeChildCount});
        CAPTURE(survey.LargestModel);
        CHECK(survey.LargestPositionCount < 4 * kExtremeChildCount);

        // Two levels of numeric bound multiply: the custom UI schemas nest a
        // `maxOccurs="1000"` group inside a `maxOccurs="1000"` choice, and an
        // element with tens of thousands of children clamps neither of them. The
        // compilation gives up rather than allocating the product, and the
        // caller falls back to the reference matcher, which is exact.
        //
        // What this case asks is that giving up is always about size and never
        // about vocabulary, and that it stays confined to documents far larger
        // than the ones the realistic sweep above covers.
        for (const auto& [reason, count] : survey.Declined)
        {
            CAPTURE(reason);
            CHECK(IsSizeLimit(reason));
        }
        CHECK(survey.Declined.size() < 20);
    }
}
