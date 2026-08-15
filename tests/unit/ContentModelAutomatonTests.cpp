// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

// ---------------------------------------------------------------------------
// The content-model automaton decides whether an element's children satisfy its
// schema particle. It replaced a recursive matcher that was slow enough to make
// a 200-paragraph document take two and a half minutes, and the recursive
// matcher is still in the build, because a compiled automaton is the kind of
// code whose wrong answers look exactly like its right ones.
//
// The load-bearing test here is the differential one: over randomly generated
// content models, every child sequence up to a small length is put to both
// implementations, and they have to agree. "Small length" is the point - a
// construction bug that only shows up on long inputs would be a strange bug,
// while one that shows up on three children is the normal kind. A failure
// prints the model and the sequence that broke it.
//
// That test is only worth as much as its ability to fail, so one case
// deliberately mismatches the two implementations and requires the harness to
// notice. The rest of the file pins the parts the differential test cannot
// reach: the wildcard predicate both implementations share, the diagnostics
// only the automaton produces, and the models it declines to compile.
// ---------------------------------------------------------------------------

#include "doctest.h"

#include "ExyokiOffice/MetadataBuilder.hpp"
#include "ExyokiOffice/OpenXMLElement.hpp"
#include "ExyokiOffice/OpenXmlPackage.hpp"
#include "ExyokiOffice/OpenXmlPackagePart.hpp"
#include "ExyokiOffice/StandardTypes.hpp"
#include "OpenXmlContentModel.hpp"
#include "zip/zip.h"

#include <algorithm>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ContentModelTestHelpers
{

using namespace ExyokiOffice;

/** A real Open XML namespace, so that diagnostics render as `w:a` rather than a URI. */
constexpr std::string_view kModelNs = "http://schemas.openxmlformats.org/wordprocessingml/2006/main";
constexpr std::string_view kForeignNs = "urn:exyokioffice:foreign";
constexpr auto kVersion = OpenXml::FileFormatVersions::Office2007;

void AddZipEntry(zip_t* archive, const char* name, std::string_view content)
{
    REQUIRE(zip_entry_open(archive, name) == 0);
    CHECK(zip_entry_write(archive, content.data(), content.size()) == 0);
    zip_entry_close(archive);
}

std::vector<Byte> BuildSingleXmlPartPackage(std::string_view xml)
{
    auto* archive = zip_stream_open(nullptr, 0, ZIP_DEFAULT_COMPRESSION_LEVEL, 'w');
    REQUIRE(archive != nullptr);
    AddZipEntry(archive,
                "[Content_Types].xml",
                R"(<?xml version="1.0" encoding="UTF-8"?>
<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">
  <Default Extension="xml" ContentType="application/xml"/>
</Types>)");
    AddZipEntry(archive, "custom.xml", xml);

    void* rawBuffer = nullptr;
    Size rawSize = 0;
    REQUIRE(zip_stream_copy(archive, &rawBuffer, &rawSize) > 0);
    zip_stream_close(archive);
    REQUIRE(rawBuffer != nullptr);

    const auto* bytes = static_cast<const UInt8*>(rawBuffer);
    std::vector<Byte> result(bytes, bytes + rawSize);
    std::free(rawBuffer);
    return result;
}

/**
 * @brief The elements the tests compose child sequences out of, parsed once.
 *
 * Matching only ever reads a child's qualified name, so one element per letter
 * of the alphabet is enough and the same object can stand at several positions
 * of a sequence. Parsing a document per sequence instead would make the
 * exhaustive enumeration below far too slow to run on every build.
 */
class ElementPool
{
public:
    ElementPool()
    {
        const std::string xml = std::string(R"(<?xml version="1.0" encoding="UTF-8"?><w:root xmlns:w=")") +
                                std::string(kModelNs) + R"(" xmlns:f=")" + std::string(kForeignNs) +
                                R"(" xmlns:mc="http://schemas.openxmlformats.org/markup-compatibility/2006">)"
                                R"(<w:a/><w:b/><w:c/><f:z/><mc:AlternateContent/></w:root>)";
        REQUIRE(package_.LoadFromMemory(BuildSingleXmlPartPackage(xml)));
        auto part = package_.GetPartByUri("/custom.xml");
        REQUIRE(part != nullptr);
        auto root = part->GetRootElement();
        REQUIRE(root != nullptr);
        letters_ = root->ChildrenInContentModel();
        REQUIRE(letters_.size() == 5);
    }

    /** The child for letter @p index, in the order a, b, c, f:z, mc:AlternateContent. */
    [[nodiscard]] const std::shared_ptr<OpenXMLElement>& Letter(Size index) const { return letters_[index]; }

    [[nodiscard]] Size LetterCount() const noexcept { return letters_.size(); }

    /** The one-character name of letter @p index, for a failure message. */
    [[nodiscard]] static char LetterName(Size index) noexcept
    {
        constexpr char names[] = {'a', 'b', 'c', 'z', 'M'};
        return names[index];
    }

private:
    OpenXmlPackage package_;
    std::vector<std::shared_ptr<OpenXMLElement>> letters_;
};

const ElementPool& Pool()
{
    static const ElementPool pool;
    return pool;
}

std::vector<std::shared_ptr<OpenXMLElement>> Children(std::string_view letters)
{
    std::vector<std::shared_ptr<OpenXMLElement>> children;
    for (const auto letter : letters)
    {
        switch (letter)
        {
            case 'a':
                children.push_back(Pool().Letter(0));
                break;
            case 'b':
                children.push_back(Pool().Letter(1));
                break;
            case 'c':
                children.push_back(Pool().Letter(2));
                break;
            case 'z':
                children.push_back(Pool().Letter(3));
                break;
            case 'M':
                children.push_back(Pool().Letter(4));
                break;
            default:
                FAIL("unknown letter in a test sequence");
        }
    }
    return children;
}

MetadataParticlePtr Element(std::string_view localName,
                            UInt32 minOccurs = 1,
                            std::optional<UInt32> maxOccurs = 1,
                            OpenXml::FileFormatVersions version = kVersion)
{
    return std::make_shared<MetadataElementParticle>(OpenXmlQualifiedName(kModelNs, localName),
                                                     std::string(localName), std::string(localName),
                                                     minOccurs, maxOccurs, version);
}

MetadataParticlePtr Any(std::string_view wildcard,
                        UInt32 minOccurs = 1,
                        std::optional<UInt32> maxOccurs = 1)
{
    return std::make_shared<MetadataAnyParticle>(std::string(wildcard), minOccurs, maxOccurs, kVersion);
}

template <typename Particle>
MetadataParticlePtr Composite(UInt32 minOccurs,
                              std::optional<UInt32> maxOccurs,
                              std::vector<MetadataParticlePtr> children)
{
    auto particle = std::make_shared<Particle>(minOccurs, maxOccurs, kVersion, false);
    for (auto& child : children)
    {
        particle->AddChild(child);
    }
    return particle;
}

MetadataParticlePtr Sequence(UInt32 minOccurs,
                             std::optional<UInt32> maxOccurs,
                             std::vector<MetadataParticlePtr> children)
{
    return Composite<MetadataSequenceParticle>(minOccurs, maxOccurs, std::move(children));
}

MetadataParticlePtr Choice(UInt32 minOccurs,
                           std::optional<UInt32> maxOccurs,
                           std::vector<MetadataParticlePtr> children)
{
    return Composite<MetadataChoiceParticle>(minOccurs, maxOccurs, std::move(children));
}

MetadataParticlePtr Group(UInt32 minOccurs,
                          std::optional<UInt32> maxOccurs,
                          std::vector<MetadataParticlePtr> children)
{
    return Composite<MetadataGroupParticle>(minOccurs, maxOccurs, std::move(children));
}

MetadataParticlePtr All(UInt32 minOccurs,
                        std::optional<UInt32> maxOccurs,
                        std::vector<MetadataParticlePtr> children)
{
    return Composite<MetadataAllParticle>(minOccurs, maxOccurs, std::move(children));
}

/** Renders a particle tree as a regular expression, so a counterexample names itself. */
std::string Describe(const MetadataParticlePtr& particle)
{
    if (!particle)
    {
        return "<none>";
    }

    std::string text;
    switch (particle->Kind())
    {
        case MetadataParticleKind::Element:
            text = std::string(static_cast<const MetadataElementParticle&>(*particle).Element().localName());
            break;
        case MetadataParticleKind::Any:
            text = "any(" + static_cast<const MetadataAnyParticle&>(*particle).Wildcard() + ")";
            break;
        default:
        {
            const auto& composite = static_cast<const MetadataCompositeParticle&>(*particle);
            const char* separator = particle->Kind() == MetadataParticleKind::Choice ? "|"
                                    : particle->Kind() == MetadataParticleKind::All  ? "&"
                                                                                     : " ";
            text = "(";
            for (Size index = 0; index < composite.Children().size(); ++index)
            {
                if (index != 0)
                {
                    text += separator;
                }
                text += Describe(composite.Children()[index]);
            }
            text += ")";
            break;
        }
    }

    const auto minimum = particle->MinOccurs();
    const auto maximum = particle->MaxOccurs();
    if (minimum == 1 && maximum && *maximum == 1)
    {
        return text;
    }
    return text + "{" + std::to_string(minimum) + "," + (maximum ? std::to_string(*maximum) : "*") + "}";
}

/**
 * @brief Reports whether every particle of @p model allows at least as many
 *        occurrences as it demands.
 *
 * XSD forbids `minOccurs > maxOccurs`, and no imported Open XML model has it,
 * but the generator below produces it anyway because both matchers have to
 * answer for it. They agree that such a model matches nothing; they differ on
 * which child to blame, because the reference walks into the branch that cannot
 * be satisfied and reports how far it got, while the automaton never compiled
 * any positions for it. That difference is about where to point a message, not
 * about the verdict, so the blame index is only compared for models without it.
 */
bool AllowsWhatItDemands(const MetadataParticlePtr& particle)
{
    if (!particle)
    {
        return true;
    }
    if (particle->MaxOccurs() && *particle->MaxOccurs() < particle->MinOccurs())
    {
        return false;
    }
    if (particle->Kind() == MetadataParticleKind::Element || particle->Kind() == MetadataParticleKind::Any)
    {
        return true;
    }

    const auto& composite = static_cast<const MetadataCompositeParticle&>(*particle);
    return std::all_of(composite.Children().begin(), composite.Children().end(), AllowsWhatItDemands);
}

/** A reproducible source of small integers; the seed appears in every failure message. */
class Random
{
public:
    explicit Random(UInt64 seed) noexcept : state_(seed * 6364136223846793005ULL + 1442695040888963407ULL) {}

    UInt32 Next(UInt32 bound) noexcept
    {
        state_ = state_ * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<UInt32>((state_ >> 33) % bound);
    }

private:
    UInt64 state_;
};

/** Occurrence bounds a generated particle may carry, kept small enough to enumerate against. */
struct Occurrence
{
    UInt32 Minimum;
    std::optional<UInt32> Maximum;
};

const std::vector<Occurrence>& Occurrences()
{
    static const std::vector<Occurrence> occurrences{{1, 1}, {0, 1}, {0, std::nullopt}, {1, std::nullopt}, {0, 2}, {1, 2}, {2, 2}, {2, 3}, {2, std::nullopt}, {0, 3}};
    return occurrences;
}

/**
 * @brief Builds a random content model over the letters a, b, c and a wildcard.
 *
 * `xs:all` members are restricted to single optional elements, which is what
 * XSD 1.0 allows there and what the automaton represents; the models that break
 * that rule get their own test rather than being generated here.
 */
MetadataParticlePtr RandomParticle(Random& random, UInt32 depth)
{
    const auto occurrence = Occurrences()[random.Next(static_cast<UInt32>(Occurrences().size()))];
    const auto kind = depth == 0 ? random.Next(2) : random.Next(6);
    switch (kind)
    {
        case 0:
        case 1:
        {
            constexpr std::string_view names[] = {"a", "b", "c"};
            return Element(names[random.Next(3)], occurrence.Minimum, occurrence.Maximum);
        }
        case 2:
            return Any(random.Next(2) == 0 ? "##any" : "##other", occurrence.Minimum, occurrence.Maximum);
        case 5:
        {
            std::vector<MetadataParticlePtr> members;
            const auto count = 1 + random.Next(3);
            constexpr std::string_view names[] = {"a", "b", "c"};
            for (UInt32 index = 0; index < count; ++index)
            {
                members.push_back(Element(names[index], random.Next(2), 1));
            }
            return All(occurrence.Minimum, 1, std::move(members));
        }
        default:
        {
            std::vector<MetadataParticlePtr> children;
            const auto count = 1 + random.Next(3);
            for (UInt32 index = 0; index < count; ++index)
            {
                children.push_back(RandomParticle(random, depth - 1));
            }
            if (kind == 3)
            {
                return Sequence(occurrence.Minimum, occurrence.Maximum, std::move(children));
            }
            return random.Next(2) == 0 ? Choice(occurrence.Minimum, occurrence.Maximum, std::move(children))
                                       : Group(occurrence.Minimum, occurrence.Maximum, std::move(children));
        }
    }
}

/** Every sequence of at most @p maxLength letters, shortest first. */
std::vector<std::string> AllSequences(Size maxLength)
{
    std::vector<std::string> sequences{""};
    Size begin = 0;
    for (Size length = 0; length < maxLength; ++length)
    {
        const auto end = sequences.size();
        for (Size index = begin; index < end; ++index)
        {
            for (Size letter = 0; letter < Pool().LetterCount(); ++letter)
            {
                sequences.push_back(sequences[index] + ElementPool::LetterName(letter));
            }
        }
        begin = end;
    }
    return sequences;
}

} // namespace ContentModelTestHelpers

TEST_SUITE("content model automaton")
{
    using namespace ContentModelTestHelpers;

    TEST_CASE("the automaton agrees with the reference matcher on every short sequence [unit] [content-model]")
    {
        // 600 models times 781 sequences, covering every particle kind, every
        // occurrence bound in the list above, and nesting three deep. Raising
        // kModels is the first thing to do when a content-model bug is
        // suspected and this case is silent; the cost is linear in it.
        constexpr UInt64 kModels = 600;
        constexpr Size kMaxLength = 4;

        const auto sequences = AllSequences(kMaxLength);
        const OpenXmlContentModelReference reference(kVersion);

        Size declined = 0;
        Size compared = 0;
        for (UInt64 seed = 0; seed < kModels; ++seed)
        {
            Random random(seed);
            const auto model = RandomParticle(random, 3);
            const auto wellFormed = AllowsWhatItDemands(model);

            // One automaton per length: the occurrence bound it is compiled with
            // has to cover the sequence being matched, which is what the cache
            // does for the validator.
            std::vector<OpenXmlContentModelAutomaton> automata;
            for (Size length = 0; length <= kMaxLength; ++length)
            {
                automata.emplace_back(model, kVersion, OpenXmlContentModelAutomaton::RoundOccurrenceBound(length));
            }
            // Supportedness is a property of the compiled automaton, not of the
            // model: the occurrence bound decides how a repetition is laid out,
            // and only some layouts can carry an xs:all. A model is only
            // compared where every bound compiled.
            const auto unsupported = std::find_if(automata.begin(), automata.end(),
                                                  [](const OpenXmlContentModelAutomaton& automaton)
                                                  { return !automaton.IsSupported(); });
            if (unsupported != automata.end())
            {
                CHECK(unsupported->UnsupportedReason() == "an xs:all inside an unbounded repetition");
                ++declined;
                continue;
            }
            CHECK(automata.front().SelfCheck().empty());

            for (const auto& letters : sequences)
            {
                const auto children = Children(letters);
                const auto expected = reference.Match(model, children, kModelNs);
                const auto actual = automata[letters.size()].Match(children, kModelNs);
                ++compared;
                if (expected.Accepted != actual.Accepted)
                {
                    FAIL("seed ", seed, " model ", Describe(model), " sequence \"", letters,
                         "\": reference says ", expected.Accepted, ", automaton says ", actual.Accepted);
                }
                if (!expected.Accepted && wellFormed && expected.ChildIndex != actual.ChildIndex)
                {
                    FAIL("seed ", seed, " model ", Describe(model), " sequence \"", letters,
                         "\": reference blames child ", expected.ChildIndex, ", automaton blames ",
                         actual.ChildIndex);
                }
            }
        }

        CHECK(compared > 300000);
        // The generator does produce models the automaton declines - an xs:all
        // under a repeating particle - and that is the only reason it can
        // produce. Anything else appearing here means the decline test below is
        // no longer describing what gets declined.
        CHECK(declined * 4 < kModels);
    }

    TEST_CASE("the automaton agrees with the reference matcher on longer sequences too [unit] [content-model]")
    {
        // The exhaustive case above stops at four children, which is short
        // enough that a bound like `{2,3}` is never made to bite twice over and
        // a repetition never turns over more than a few times. This one takes
        // sequences of up to twelve children instead, sampled rather than
        // enumerated - there are five million of them - and the sampling is
        // seeded, so a failure here reproduces exactly like one above.
        constexpr UInt64 kModels = 120;
        constexpr Size kSequencesPerModel = 400;
        constexpr Size kMaxLength = 12;

        const OpenXmlContentModelReference reference(kVersion);
        Size compared = 0;
        Size accepted = 0;
        for (UInt64 seed = 0; seed < kModels; ++seed)
        {
            Random random(seed + 1000);
            const auto model = RandomParticle(random, 3);
            const auto wellFormed = AllowsWhatItDemands(model);

            for (Size index = 0; index < kSequencesPerModel; ++index)
            {
                std::string letters;
                const auto length = random.Next(kMaxLength + 1);
                for (UInt32 position = 0; position < length; ++position)
                {
                    // Weighted towards the model's own alphabet: a sequence of
                    // mostly foreign names is rejected by the first child and
                    // tests almost nothing.
                    const auto letter = random.Next(8);
                    letters.push_back(ElementPool::LetterName(letter < 6 ? letter % 3 : letter - 5));
                }

                const auto children = Children(letters);
                const OpenXmlContentModelAutomaton automaton(
                    model, kVersion, OpenXmlContentModelAutomaton::RoundOccurrenceBound(children.size()));
                if (!automaton.IsSupported())
                {
                    break;
                }

                const auto expected = reference.Match(model, children, kModelNs);
                const auto actual = automaton.Match(children, kModelNs);
                ++compared;
                accepted += expected.Accepted ? 1 : 0;
                if (expected.Accepted != actual.Accepted)
                {
                    FAIL("seed ", seed, " model ", Describe(model), " sequence \"", letters,
                         "\": reference says ", expected.Accepted, ", automaton says ", actual.Accepted);
                }
                if (!expected.Accepted && wellFormed && expected.ChildIndex != actual.ChildIndex)
                {
                    FAIL("seed ", seed, " model ", Describe(model), " sequence \"", letters,
                         "\": reference blames child ", expected.ChildIndex, ", automaton blames ",
                         actual.ChildIndex);
                }
            }
        }

        CHECK(compared > 20000);
        // A sweep in which nothing is ever accepted would agree trivially.
        CHECK(accepted > 1000);
    }

    TEST_CASE("the differential comparison notices when the two are asked different questions [unit] [content-model]")
    {
        // A comparison that cannot fail proves nothing. Here the automaton is
        // built from one model and the reference is run on another, and the
        // enumeration above has to find the disagreement - if it does not, the
        // case above is not testing what it claims to.
        const auto compiled = Sequence(1, 1, {Element("a"), Element("b", 0, 1)});
        const auto interpreted = Sequence(1, 1, {Element("a"), Element("b")});

        const OpenXmlContentModelReference reference(kVersion);
        const OpenXmlContentModelAutomaton automaton(compiled, kVersion,
                                                     OpenXmlContentModelAutomaton::UnclampedBound);
        REQUIRE(automaton.IsSupported());

        bool found = false;
        for (const auto& letters : AllSequences(3))
        {
            const auto children = Children(letters);
            if (reference.Match(interpreted, children, kModelNs).Accepted !=
                automaton.Match(children, kModelNs).Accepted)
            {
                found = true;
            }
        }
        CHECK(found);
    }

    TEST_CASE("sequence choice group and nesting accept exactly what the schema says [unit] [content-model]")
    {
        const auto match = [](const MetadataParticlePtr& model, std::string_view letters)
        {
            const OpenXmlContentModelAutomaton automaton(
                model, kVersion, OpenXmlContentModelAutomaton::RoundOccurrenceBound(letters.size()));
            REQUIRE(automaton.IsSupported());
            REQUIRE(automaton.SelfCheck().empty());
            return automaton.Match(Children(letters), kModelNs).Accepted;
        };

        const auto sequence = Sequence(1, 1, {Element("a"), Element("b")});
        CHECK(match(sequence, "ab"));
        CHECK_FALSE(match(sequence, "ba"));
        CHECK_FALSE(match(sequence, "a"));
        CHECK_FALSE(match(sequence, "abc"));

        const auto choice = Choice(1, 1, {Element("a"), Element("b")});
        CHECK(match(choice, "a"));
        CHECK(match(choice, "b"));
        CHECK_FALSE(match(choice, ""));
        CHECK_FALSE(match(choice, "ab"));

        const auto optionalChoice = Choice(1, 1, {Element("a", 0, 1)});
        CHECK(match(optionalChoice, ""));

        // A choice with no branches matches nothing at all, which is not the
        // same as matching the empty sequence - the distinction the Void
        // fragment exists for.
        const auto emptyChoice = Choice(1, 1, {});
        CHECK_FALSE(match(emptyChoice, ""));
        CHECK(match(Choice(0, 1, {}), ""));

        const auto group = Group(1, 1, {sequence});
        CHECK(match(group, "ab"));

        // A repeated choice inside a sequence: the shape that made the old
        // matcher super-quadratic.
        const auto nested =
            Sequence(1, 1, {Choice(1, std::nullopt, {Element("a"), Element("b")}), Element("c")});
        CHECK(match(nested, "abac"));
        CHECK(match(nested, "ac"));
        CHECK_FALSE(match(nested, "acb"));
        CHECK_FALSE(match(nested, "c"));
    }

    TEST_CASE("occurrence bounds are exact, not approximated [unit] [content-model]")
    {
        const auto match = [](const MetadataParticlePtr& model, std::string_view letters)
        {
            const OpenXmlContentModelAutomaton automaton(
                model, kVersion, OpenXmlContentModelAutomaton::RoundOccurrenceBound(letters.size()));
            REQUIRE(automaton.IsSupported());
            return automaton.Match(Children(letters), kModelNs).Accepted;
        };

        CHECK_FALSE(match(Element("a", 2, 2), "a"));
        CHECK(match(Element("a", 2, 2), "aa"));
        CHECK_FALSE(match(Element("a", 2, 2), "aaa"));

        CHECK(match(Element("a", 0, 3), ""));
        CHECK(match(Element("a", 0, 3), "aaa"));
        CHECK_FALSE(match(Element("a", 0, 3), "aaaa"));

        CHECK(match(Element("a", 0, std::nullopt), ""));
        CHECK(match(Element("a", 0, std::nullopt), "aaaaa"));
        CHECK_FALSE(match(Element("a", 1, std::nullopt), ""));

        // A bound on a composite repeats the whole body, and the copies stay
        // independent: `(a b){1,2}` is `ab` or `abab`, never `aab`.
        const auto pair = Sequence(1, 2, {Element("a"), Element("b")});
        CHECK(match(pair, "ab"));
        CHECK(match(pair, "abab"));
        CHECK_FALSE(match(pair, "aab"));
        CHECK_FALSE(match(pair, "ababab"));

        // A minimum larger than the whole document can never be met.
        CHECK_FALSE(match(Element("a", 9, 9), "aa"));
    }

    TEST_CASE("a clamped occurrence bound decides the same as an unclamped one [unit] [content-model]")
    {
        // The clamp is the one step of the compilation that is an optimization
        // rather than a translation: a bound larger than the number of children
        // is unreachable, so it is lowered to keep `maxOccurs="65430"` from
        // compiling into 65430 copies. If that reasoning is wrong, the two
        // automata below disagree.
        const auto model = Sequence(1, 1, {Element("a", 1, 65430), Element("b", 0, 1000)});
        const OpenXmlContentModelAutomaton unclamped(model, kVersion,
                                                     OpenXmlContentModelAutomaton::UnclampedBound);
        REQUIRE(unclamped.IsSupported());
        // Unclamped, the schema's numbers are taken at face value and compile
        // into one position per allowed occurrence.
        CHECK(unclamped.PositionCount() > 66000);

        for (const auto& letters : AllSequences(4))
        {
            const auto children = Children(letters);
            const OpenXmlContentModelAutomaton clamped(
                model, kVersion, OpenXmlContentModelAutomaton::RoundOccurrenceBound(children.size()));
            REQUIRE(clamped.IsSupported());
            CHECK(clamped.PositionCount() < 20);
            const OpenXmlContentModelReference reference(kVersion);
            CHECK(clamped.Match(children, kModelNs).Accepted ==
                  reference.Match(model, children, kModelNs).Accepted);
            CHECK(clamped.Match(children, kModelNs).Accepted ==
                  unclamped.Match(children, kModelNs).Accepted);
        }

        CHECK(OpenXmlContentModelAutomaton::NeedsOccurrenceBound(model));
        CHECK_FALSE(OpenXmlContentModelAutomaton::NeedsOccurrenceBound(
            Sequence(1, 1, {Element("a", 0, std::nullopt), Element("b", 0, 1)})));

        CHECK(OpenXmlContentModelAutomaton::RoundOccurrenceBound(0) == 1);
        CHECK(OpenXmlContentModelAutomaton::RoundOccurrenceBound(1) == 1);
        CHECK(OpenXmlContentModelAutomaton::RoundOccurrenceBound(3) == 4);
        CHECK(OpenXmlContentModelAutomaton::RoundOccurrenceBound(1000) == 1024);
    }

    TEST_CASE("an xs:all takes its members in any order and each at most once [unit] [content-model]")
    {
        const auto match = [](const MetadataParticlePtr& model, std::string_view letters)
        {
            const OpenXmlContentModelAutomaton automaton(
                model, kVersion, OpenXmlContentModelAutomaton::RoundOccurrenceBound(letters.size()));
            REQUIRE(automaton.IsSupported());
            REQUIRE(automaton.SelfCheck().empty());
            return automaton.Match(Children(letters), kModelNs).Accepted;
        };

        const auto required = All(1, 1, {Element("a"), Element("b")});
        CHECK(match(required, "ab"));
        CHECK(match(required, "ba"));
        CHECK_FALSE(match(required, "a"));
        CHECK_FALSE(match(required, "aab"));
        CHECK_FALSE(match(required, "aba"));
        CHECK_FALSE(match(required, ""));

        const auto mixed = All(1, 1, {Element("a", 0, 1), Element("b")});
        CHECK(match(mixed, "b"));
        CHECK(match(mixed, "ba"));
        CHECK(match(mixed, "ab"));
        CHECK_FALSE(match(mixed, "a"));

        const auto optional = All(0, 1, {Element("a", 0, 1), Element("b", 0, 1)});
        CHECK(match(optional, ""));
        CHECK(match(optional, "b"));
        CHECK_FALSE(match(optional, "bb"));

        // A required member inside a branch the content never took makes no
        // demand: the block was not entered, so its bits stay clear.
        const auto branch = Choice(1, 1, {All(1, 1, {Element("a"), Element("b")}), Element("c")});
        CHECK(match(branch, "c"));
        CHECK(match(branch, "ba"));
        CHECK_FALSE(match(branch, "a"));
    }

    TEST_CASE("wildcards admit exactly the namespaces the constraint names [unit] [content-model]")
    {
        // Both matchers call this, so the differential test cannot see a bug in
        // it; it is pinned directly instead.
        CHECK(ContentModelWildcardMatches("##any", "urn:anything", kModelNs));
        CHECK(ContentModelWildcardMatches("", "urn:anything", kModelNs));
        CHECK(ContentModelWildcardMatches("##local", "", kModelNs));
        CHECK_FALSE(ContentModelWildcardMatches("##local", kModelNs, kModelNs));
        CHECK(ContentModelWildcardMatches("##targetNamespace", kModelNs, kModelNs));
        CHECK_FALSE(ContentModelWildcardMatches("##targetNamespace", kForeignNs, kModelNs));
        CHECK(ContentModelWildcardMatches("##other", kForeignNs, kModelNs));
        CHECK_FALSE(ContentModelWildcardMatches("##other", kModelNs, kModelNs));
        CHECK_FALSE(ContentModelWildcardMatches("##other", "", kModelNs));
        CHECK(ContentModelWildcardMatches("urn:exyokioffice:foreign ##local", kForeignNs, kModelNs));
        CHECK(ContentModelWildcardMatches("urn:exyokioffice:foreign ##local", "", kModelNs));
        CHECK_FALSE(ContentModelWildcardMatches("urn:exyokioffice:foreign", kModelNs, kModelNs));

        const auto match = [](const MetadataParticlePtr& model, std::string_view letters)
        {
            const OpenXmlContentModelAutomaton automaton(model, kVersion,
                                                         OpenXmlContentModelAutomaton::UnclampedBound);
            REQUIRE(automaton.IsSupported());
            return automaton.Match(Children(letters), kModelNs).Accepted;
        };
        CHECK(match(Any("##other"), "z"));
        CHECK_FALSE(match(Any("##other"), "a"));
        CHECK(match(Any("##any"), "a"));
    }

    TEST_CASE("mc:AlternateContent stands in for whatever the model wanted [unit] [content-model]")
    {
        const auto match = [](const MetadataParticlePtr& model, std::string_view letters)
        {
            const OpenXmlContentModelAutomaton automaton(
                model, kVersion, OpenXmlContentModelAutomaton::RoundOccurrenceBound(letters.size()));
            REQUIRE(automaton.IsSupported());
            return automaton.Match(Children(letters), kModelNs).Accepted;
        };

        const auto sequence = Sequence(1, 1, {Element("a"), Element("b")});
        CHECK(match(sequence, "Mb"));
        CHECK(match(sequence, "aM"));
        CHECK(match(sequence, "MM"));
        // It stands for one element, not for any number of them.
        CHECK_FALSE(match(sequence, "M"));
        CHECK_FALSE(match(sequence, "MMM"));

        // Standing in for a member of an xs:all still uses that member up.
        const auto all = All(1, 1, {Element("a"), Element("b")});
        CHECK(match(all, "Ma"));
        CHECK(match(all, "MM"));
        CHECK_FALSE(match(all, "M"));
    }

    TEST_CASE("a particle the target Office version does not have is not in the model [unit] [content-model]")
    {
        const auto model = Sequence(1, 1, {Element("a"), Element("b", 1, 1, OpenXml::FileFormatVersions::Office2019)});

        const OpenXmlContentModelAutomaton modern(model, OpenXml::FileFormatVersions::Office2021,
                                                  OpenXmlContentModelAutomaton::UnclampedBound);
        CHECK(modern.Match(Children("ab"), kModelNs).Accepted);
        CHECK_FALSE(modern.Match(Children("a"), kModelNs).Accepted);

        const OpenXmlContentModelAutomaton legacy(model, OpenXml::FileFormatVersions::Office2007,
                                                  OpenXmlContentModelAutomaton::UnclampedBound);
        CHECK(legacy.Match(Children("a"), kModelNs).Accepted);
        CHECK_FALSE(legacy.Match(Children("ab"), kModelNs).Accepted);
    }

    TEST_CASE("a failure says which child broke the model and what belonged there [unit] [content-model]")
    {
        const auto model =
            Sequence(1, 1, {Element("a"), Choice(0, 1, {Element("b"), Element("c")}), Element("a")});
        const OpenXmlContentModelAutomaton automaton(model, kVersion,
                                                     OpenXmlContentModelAutomaton::UnclampedBound);
        REQUIRE(automaton.IsSupported());

        const auto tooShort = automaton.Match(Children("a"), kModelNs);
        CHECK_FALSE(tooShort.Accepted);
        // A valid prefix that ends early is blamed on the end, not on a child.
        CHECK(tooShort.ChildIndex == 1);
        CHECK(tooShort.Expected.size() == 3);
        CHECK(tooShort.Expected[0] == OpenXmlQualifiedName(kModelNs, "b"));
        CHECK(tooShort.Expected[1] == OpenXmlQualifiedName(kModelNs, "c"));
        CHECK(tooShort.Expected[2] == OpenXmlQualifiedName(kModelNs, "a"));
        CHECK_FALSE(tooShort.ExpectedWildcard);

        const auto wrongChild = automaton.Match(Children("azb"), kModelNs);
        CHECK_FALSE(wrongChild.Accepted);
        CHECK(wrongChild.ChildIndex == 1);

        const auto trailing = automaton.Match(Children("abab"), kModelNs);
        CHECK_FALSE(trailing.Accepted);
        CHECK(trailing.ChildIndex == 3);
        CHECK(trailing.Expected.empty());

        const auto wildcard = OpenXmlContentModelAutomaton(Sequence(1, 1, {Element("a"), Any("##other")}),
                                                           kVersion,
                                                           OpenXmlContentModelAutomaton::UnclampedBound)
                                  .Match(Children("a"), kModelNs);
        CHECK_FALSE(wildcard.Accepted);
        CHECK(wildcard.Expected.empty());
        CHECK(wildcard.ExpectedWildcard);
    }

    TEST_CASE("the compiled automaton can be read back in full [unit] [content-model]")
    {
        // `a{0,2}` compiles to a chain: the first copy leads to the second, and
        // the run may stop having consumed none, one or both. Spelling the whole
        // table out is what makes a wrong transition visible by reading rather
        // than by guessing from a verdict.
        const OpenXmlContentModelAutomaton automaton(Element("a", 0, 2), kVersion,
                                                     OpenXmlContentModelAutomaton::UnclampedBound);
        REQUIRE(automaton.IsSupported());
        CHECK(automaton.PositionCount() == 3);
        CHECK(automaton.TransitionCount() == 2);
        CHECK(automaton.SelfCheck().empty());
        CHECK(automaton.Describe() ==
              "content model automaton: 3 positions, 2 transitions, occurrence bound none\n"
              "  0 <start> [accepting] -> 1\n"
              "  1 w:a [accepting] -> 2\n"
              "  2 w:a [accepting] ->\n");

        const OpenXmlContentModelAutomaton withAll(All(1, 1, {Element("a"), Element("b", 0, 1)}), kVersion,
                                                   OpenXmlContentModelAutomaton::UnclampedBound);
        REQUIRE(withAll.IsSupported());
        CHECK(withAll.Describe() ==
              "content model automaton: 3 positions, 4 transitions, occurrence bound none\n"
              "  0 <start> -> 1 2\n"
              "  1 w:a [all 0 member 0] [accepting] -> 2\n"
              "  2 w:b [all 0 member 1] [accepting] -> 1\n"
              "  all 0: 2 members, required 0\n");
    }

    TEST_CASE("a model the automaton cannot represent is declined, never approximated [unit] [content-model]")
    {
        // An xs:all inside an unbounded repetition would need its "already
        // taken" bits reset on every turn around the loop, and they have no way
        // to reset.
        const OpenXmlContentModelAutomaton repeated(
            Group(0, std::nullopt, {All(1, 1, {Element("a"), Element("b")})}), kVersion,
            OpenXmlContentModelAutomaton::UnclampedBound);
        CHECK_FALSE(repeated.IsSupported());
        CHECK(repeated.UnsupportedReason() == "an xs:all inside an unbounded repetition");
        CHECK(repeated.Describe().find("declined") != std::string::npos);

        // XSD 1.0 only allows single elements with maxOccurs="1" in an xs:all,
        // and one bit per member cannot count higher than that.
        const OpenXmlContentModelAutomaton repeatedMember(All(1, 1, {Element("a", 1, 3)}), kVersion,
                                                          OpenXmlContentModelAutomaton::UnclampedBound);
        CHECK_FALSE(repeatedMember.IsSupported());
        CHECK(repeatedMember.UnsupportedReason() == "an xs:all whose member may appear more than once");

        const OpenXmlContentModelAutomaton compositeMember(
            All(1, 1, {Sequence(1, 1, {Element("a"), Element("b")})}), kVersion,
            OpenXmlContentModelAutomaton::UnclampedBound);
        CHECK_FALSE(compositeMember.IsSupported());
        CHECK(compositeMember.UnsupportedReason() == "an xs:all whose member is not a single element");

        // The position budget is the other way out, and it names itself too.
        const OpenXmlContentModelAutomaton huge(Element("a", 1, 1000000), kVersion,
                                                OpenXmlContentModelAutomaton::UnclampedBound);
        CHECK_FALSE(huge.IsSupported());
        CHECK(huge.UnsupportedReason().find("positions") != std::string::npos);
    }

    TEST_CASE("the cache compiles a model once, and once per bound where the bound matters [unit] [content-model]")
    {
        const auto unbounded = Sequence(1, 1, {Element("a", 0, std::nullopt)});
        const auto numeric = Sequence(1, 1, {Element("a", 0, 500)});

        OpenXmlContentModelCache cache(kVersion);
        CHECK(cache.AutomatonCount() == 0);

        static_cast<void>(cache.Get(unbounded, 3));
        static_cast<void>(cache.Get(unbounded, 90));
        static_cast<void>(cache.Get(unbounded, 4000));
        // No numeric bound to clamp, so the child count changes nothing.
        CHECK(cache.AutomatonCount() == 1);

        static_cast<void>(cache.Get(numeric, 3));
        CHECK(cache.AutomatonCount() == 2);
        static_cast<void>(cache.Get(numeric, 4)); // the same power-of-two bucket as 3
        CHECK(cache.AutomatonCount() == 2);
        static_cast<void>(cache.Get(numeric, 9));
        CHECK(cache.AutomatonCount() == 3);

        // Whatever the bucket, the verdict is the one the model calls for.
        CHECK(cache.Get(numeric, 2).Match(Children("aa"), kModelNs).Accepted);
        CHECK_FALSE(cache.Get(numeric, 3).Match(Children("ab"), kModelNs).Accepted);

        cache.Clear();
        CHECK(cache.AutomatonCount() == 0);
    }
}
