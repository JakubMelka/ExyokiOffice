// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

#include "ExyokiOffice/Export.hpp"
#include "ExyokiOffice/FileFormatVersions.h"
#include "ExyokiOffice/MetadataBuilder.hpp"
#include "ExyokiOffice/OpenXmlQualifiedName.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace ExyokiOffice
{

class OpenXMLElement;
struct ContentModelConfiguration;

/**
 * @internal
 * @file
 * @brief Deciding whether an element's children satisfy its schema content model.
 *
 * A content model is the `xs:sequence` / `xs:choice` / `xs:all` / `xs:group`
 * tree the generator imported from the Open XML schemas, held as a
 * @ref MetadataParticle tree. Matching a document against one is exactly the
 * question "is this word in the language of this regular expression", where the
 * letters are qualified element names.
 *
 * Two answers to that question live here, and they are meant to agree:
 *
 * - @ref OpenXmlContentModelReference walks the particle tree recursively and
 *   returns the set of child positions a particle can end at. It follows the
 *   shape of the schema so closely that it can be read as the specification,
 *   and it is the definition of "correct" for the other one. It is also
 *   super-quadratic in the number of children, which is why it is not what the
 *   validator runs.
 * - @ref OpenXmlContentModelAutomaton compiles the same tree into a position
 *   automaton once and then consumes the children in a single left-to-right
 *   pass. That is linear in the number of children, and it is what the
 *   validator runs.
 *
 * Keeping both is deliberate. The automaton is the kind of code where a wrong
 * answer looks exactly like a right one, so there has to be a way to find out
 * that it is wrong:
 *
 * - `tests/unit/ContentModelAutomatonTests.cpp` enumerates every child sequence
 *   up to a small length over randomly generated content models and requires
 *   the two to return the same verdict. A construction bug shows up as a
 *   concrete counterexample, printed with the model that produced it.
 * - @ref OpenXmlDomValidationSettings::CrossCheckContentModel makes a shipped
 *   validator run both on every element and report a
 *   @ref ValidationErrorId::ContentModelCrossCheckMismatch where they differ, so
 *   a suspicious document can be interrogated without a debug build.
 * - @ref OpenXmlContentModelAutomaton::Describe prints the compiled automaton
 *   in full, and @ref OpenXmlContentModelAutomaton::SelfCheck restates the
 *   structural invariants of the construction as assertions over the built
 *   object.
 */

/**
 * @internal
 * @brief Reports whether an `xs:any` namespace constraint admits @p namespaceUri.
 *
 * @param wildcard The `namespace` attribute of the wildcard: a space-separated
 *        list of namespace URIs and the tokens `##any`, `##other`, `##local`
 *        and `##targetNamespace`. An empty list means `##any`.
 * @param namespaceUri The namespace of the child being placed, empty for an
 *        unqualified name.
 * @param targetNamespace The schema's target namespace, which is what
 *        `##targetNamespace` names and what `##other` excludes.
 *
 * Shared by both matchers on purpose. The differential tests can only find a
 * disagreement between two implementations, so anything they both call has to
 * be pinned by its own tests instead - see the wildcard cases in
 * `tests/unit/ContentModelAutomatonTests.cpp`.
 */
[[nodiscard]] EXYOKIOFFICE_EXPORT bool ContentModelWildcardMatches(std::string_view wildcard,
                                                                   std::string_view namespaceUri,
                                                                   std::string_view targetNamespace) noexcept;

/**
 * @internal
 * @brief The verdict of one content-model match.
 *
 * On success only @ref Accepted matters. On failure the other members say where
 * to point the reader: @ref ChildIndex is the first child no reading of the
 * model could consume, and equals the child count when the children form a
 * valid prefix that simply ends too early. @ref Expected lists the element names
 * that would have been admissible at that point, in the order the content model
 * declares them.
 */
struct ContentModelMatch
{
    bool Accepted = false;
    Size ChildIndex = 0;
    std::vector<OpenXmlQualifiedName> Expected;
    /** Set when a wildcard (`xs:any`) was among the things admissible at @ref ChildIndex. */
    bool ExpectedWildcard = false;
};

/**
 * @internal
 * @brief The recursive content-model matcher, kept as the executable specification.
 *
 * `Match(particle, position)` answers "starting at child @p position, at which
 * child positions can @p particle stop", and every particle kind is one obvious
 * case of that question. Composition is the same in both directions: a sequence
 * threads the position set through its children, a choice unions its children's
 * answers, a repetition iterates the body over a growing frontier.
 *
 * That directness is the whole point of the class, and it is also why it is
 * slow: an unbounded particle re-runs its body over a frontier that the level
 * below has already grown to the size of the document, and the four levels of
 * nested unbounded groups in `w:CT_Body` turn that into roughly N^3.5. It stays
 * in the build as the reference the automaton is checked against, as the
 * fallback for the few content models the automaton declines to compile, and as
 * the second opinion `CrossCheckContentModel` asks for.
 */
class EXYOKIOFFICE_EXPORT OpenXmlContentModelReference
{
public:
    explicit OpenXmlContentModelReference(OpenXml::FileFormatVersions targetVersion) noexcept;

    /**
     * @brief Matches @p children against @p particle.
     *
     * @param particle The content model. A null particle admits no children at all.
     * @param children The children to match, in document order.
     * @param targetNamespace The namespace `##targetNamespace` and `##other`
     *        wildcards are resolved against, which is the namespace of the
     *        element whose content model this is.
     */
    [[nodiscard]] ContentModelMatch Match(const MetadataParticlePtr& particle,
                                          const std::vector<std::shared_ptr<OpenXMLElement>>& children,
                                          std::string_view targetNamespace) const;

private:
    OpenXml::FileFormatVersions targetVersion_;
};

/**
 * @internal
 * @brief The content model compiled into a position automaton.
 *
 * ## What is built
 *
 * Every leaf of the particle tree - an `xs:element` or an `xs:any` - becomes one
 * *position*. Positions are the states: being "at position p" means the last
 * child consumed was matched by that particular leaf, which is not the same as
 * "matched that element name", because one name may appear at several places in
 * a model and the places differ in what may follow. A virtual position 0 stands
 * for "nothing consumed yet", so that the start needs no special case in the
 * simulation.
 *
 * The construction is Glushkov's: each particle yields a fragment carrying
 * whether it accepts the empty sequence (`nullable`), which positions can begin
 * it (`first`), and which can end it (`last`), while the transitions
 * (`follow`) accumulate into one table shared by the whole model. A sequence
 * links the `last` of each part to the `first` of the next; a choice unions;
 * an unbounded repetition links the body's `last` back to its own `first`. No
 * epsilon transitions are ever created, which is what makes the simulation a
 * plain set of positions rather than a closure computation.
 *
 * ## Occurrence bounds
 *
 * `minOccurs` and `maxOccurs` of 0, 1 and unbounded are exactly what the loop
 * above expresses. A numeric bound - `maxOccurs="65430"` on `x:xf`, or the
 * `maxOccurs="1000"` choices in the custom UI schemas - is compiled by laying
 * the body out that many times over, which is the only construction here that
 * can grow the automaton. Three rules keep it from growing past the document it
 * is meant to match, and all three rest on the same fact: the number of
 * occurrences a document can use is bounded by the number of children it has.
 *
 * 1. **The bound is clamped to the child count.** A maximum larger than the
 *    number of children can never be reached, so `B{m,n}` restricted to
 *    sequences of at most `bound` children accepts the same ones as
 *    `B{m, max(m, min(n, bound))}`. The clamp is the @p occurrenceBound
 *    constructor argument.
 * 2. **A bound the child count reaches becomes no bound at all.** If the
 *    clamped maximum is the child count, the repetition forbids nothing the
 *    document could have done anyway, and it is compiled as a loop over one
 *    copy. This is what keeps the custom UI schemas' `maxOccurs="1000"` group
 *    inside a `maxOccurs="1000"` choice from compiling into their product.
 * 3. **An empty-matching body is repeated by its non-empty words.** Occurrences
 *    that consume nothing are free, so `B{m,n}` for such a B accepts exactly the
 *    concatenations of at most `n` *non-empty* words of B - and each of those
 *    consumes a child, which is what makes the first two rules apply to it too.
 *    Compiled the other way, letting a copy be skipped, `B{0,1000}` would need a
 *    transition from every copy to every later one.
 *
 * ## `xs:all`
 *
 * An `xs:all` is not a regular expression over positions: its members may come
 * in any order but at most once each. The transitions are built as "after any
 * member, any member may follow", and the "at most once" part is carried by a
 * bitmask alongside the position set - one bit per member, per `xs:all` in the
 * model. Because XSD 1.0 only allows single elements with `maxOccurs="1"` inside
 * an `xs:all`, one bit per member is enough to say which one was taken. A model
 * that breaks that rule is declined rather than approximated - see below.
 *
 * The bits have no way to reset, so an `xs:all` cannot sit inside a loop, where
 * the same positions come round again. Inside a repetition laid out copy by copy
 * it is fine - each copy is a block of its own - which is why the second rule
 * above never turns a repetition containing an `xs:all` into a loop.
 *
 * ## Markup compatibility
 *
 * `mc:AlternateContent` stands in for exactly one element of the surrounding
 * vocabulary, and which one it stands for is a branch selection the schema
 * cannot express. A child with that name therefore takes every transition out
 * of the current state set, whatever the labels say.
 *
 * ## Declining a model
 *
 * @ref IsSupported reports false for a model the construction above cannot
 * represent faithfully, and @ref UnsupportedReason says why. There is no
 * approximation anywhere: the caller falls back to
 * @ref OpenXmlContentModelReference, which is slower and always exact. This
 * keeps "the automaton cannot do it" and "the answer is wrong" from ever being
 * the same thing.
 */
class EXYOKIOFFICE_EXPORT OpenXmlContentModelAutomaton
{
public:
    /** The clamp value that means "no numeric occurrence bound needs clamping". */
    static constexpr Size UnclampedBound = static_cast<Size>(-1);

    /**
     * @brief Compiles @p particle.
     *
     * @param particle The content model. A null particle compiles to an
     *        automaton that accepts only the empty child sequence.
     * @param targetVersion The Office generation whose availability rules apply.
     *        Particles the target does not cover are compiled away entirely,
     *        which is the same thing as treating them as absent from the schema.
     * @param occurrenceBound An upper bound on the number of children this
     *        automaton will be asked to match, used to clamp numeric occurrence
     *        bounds. Passing a value that is too small is a correctness bug, not
     *        a performance one; pass @ref UnclampedBound when the model has no
     *        numeric bound to clamp, which @ref NeedsOccurrenceBound answers.
     */
    OpenXmlContentModelAutomaton(const MetadataParticlePtr& particle,
                                 OpenXml::FileFormatVersions targetVersion,
                                 Size occurrenceBound);

    /**
     * @brief Reports whether @p particle contains a bound the child count can clamp.
     *
     * False for the overwhelming majority of Open XML content models, whose
     * occurrence bounds are all 0, 1 or unbounded. Those compile to one
     * automaton that serves every child count; the rest need one per bound.
     */
    [[nodiscard]] static bool NeedsOccurrenceBound(const MetadataParticlePtr& particle);

    /** Rounds a child count up to the next power of two, so that bounds cache well. */
    [[nodiscard]] static Size RoundOccurrenceBound(Size childCount) noexcept;

    [[nodiscard]] bool IsSupported() const noexcept { return unsupportedReason_.empty(); }
    [[nodiscard]] const std::string& UnsupportedReason() const noexcept { return unsupportedReason_; }

    /**
     * @brief Matches @p children in one left-to-right pass.
     *
     * Must not be called on an automaton @ref IsSupported rejects, and
     * @p children must not be longer than the occurrence bound it was built
     * with.
     */
    [[nodiscard]] ContentModelMatch Match(const std::vector<std::shared_ptr<OpenXMLElement>>& children,
                                          std::string_view targetNamespace) const;

    [[nodiscard]] Size PositionCount() const noexcept { return positions_.size(); }
    [[nodiscard]] Size TransitionCount() const noexcept;
    [[nodiscard]] Size OccurrenceBound() const noexcept { return occurrenceBound_; }

    /**
     * @brief Prints the whole automaton: positions, labels, transitions, `xs:all` blocks.
     *
     * Stable enough to be compared against a golden text in a test, and complete
     * enough that a wrong transition table can be read off it by hand.
     */
    [[nodiscard]] std::string Describe() const;

    /**
     * @brief Restates the construction's invariants as checks over the built object.
     *
     * Returns one message per violation and an empty list for a healthy
     * automaton. This catches a builder that produced a table no simulation
     * could interpret - a transition to a position that does not exist, a
     * position nothing can reach, an `xs:all` member bit out of range - without
     * needing a document to match.
     */
    [[nodiscard]] std::vector<std::string> SelfCheck() const;

private:
    /** The @ref Position::AllBlock of a position that is not a member of any `xs:all`. */
    static constexpr Size NoAllBlock = static_cast<Size>(-1);

    /** One leaf of the content model, and the transitions leaving it. */
    struct Position
    {
        /** Element or Any; the virtual start position 0 is neither and matches nothing. */
        MetadataParticleKind Kind = MetadataParticleKind::Element;
        OpenXmlQualifiedName Name;
        std::string Wildcard;
        /** Index of the `xs:all` this position is a member of, or @ref NoAllBlock. */
        Size AllBlock = NoAllBlock;
        /** Index of the member inside that block. */
        Size AllMember = 0;
        std::vector<Size> Follow;
    };

    /** An `xs:all`, whose members may appear in any order but at most once each. */
    struct AllBlock
    {
        Size MemberCount = 0;
        /** Members whose `minOccurs` is not zero, as a bitmask over member indices. */
        std::vector<UInt64> Required;
    };

    class Builder;

    /** Advances one configuration over one child, appending what it reaches to @p next. */
    void Step(const ContentModelConfiguration& configuration,
              const OpenXMLElement* child,
              bool transparent,
              std::string_view targetNamespace,
              Size positionWords,
              std::vector<ContentModelConfiguration>& next) const;

    /** Reports whether @p position may match the child at hand. */
    [[nodiscard]] bool Admits(Size position,
                              const OpenXMLElement* child,
                              bool transparent,
                              std::string_view targetNamespace) const;

    [[nodiscard]] bool IsAccepting(const ContentModelConfiguration& configuration) const;

    /** Reports whether an `xs:all` block has taken every member it requires. */
    [[nodiscard]] bool IsAllBlockComplete(const std::vector<UInt64>& masks, Size block) const;

    /** Reports whether a step out of an `xs:all` is allowed yet. */
    [[nodiscard]] bool MayLeaveAllBlock(const std::vector<UInt64>& masks, Size from, Size to) const;

    [[nodiscard]] std::vector<OpenXmlQualifiedName> CollectExpected(
        const std::vector<ContentModelConfiguration>& configurations,
        bool& expectedWildcard) const;

    /** The index of @p position's `xs:all` member bit in the flat mask vector. */
    [[nodiscard]] Size MaskBit(const Position& position) const noexcept;

    /** Returns the position set of the configuration carrying @p masks, creating it if needed. */
    [[nodiscard]] static std::vector<UInt64>& FindConfiguration(
        std::vector<ContentModelConfiguration>& configurations,
        const std::vector<UInt64>& masks,
        Size positionWords);

    std::vector<Position> positions_;
    std::vector<AllBlock> allBlocks_;
    /** Positions the automaton may stop at, as a bitmask; bit 0 means the model is nullable. */
    std::vector<UInt64> accepting_;
    Size occurrenceBound_ = UnclampedBound;
    OpenXml::FileFormatVersions targetVersion_{};
    std::string unsupportedReason_;
    /**
     * Set when compiling threw away a branch that matches nothing, which leaves
     * the positions that branch had already created in the table with nothing
     * leading to them. @ref SelfCheck stops looking for unreachable positions
     * once it is set, because then they are expected rather than a defect.
     */
    bool hasVoidBranch_ = false;
};

/**
 * @internal
 * @brief The automata compiled during one validation run.
 *
 * Compiling a content model costs more than matching a handful of children
 * against it, and a document repeats a few dozen content models thousands of
 * times, so the automaton has to outlive the element it was built for. It must
 * not outlive the run: it is built for one target version, and for models with
 * numeric occurrence bounds also for one child-count clamp. Holding the cache
 * for the run rather than in a global keeps both of those facts local and keeps
 * the validator free of shared mutable state.
 */
class EXYOKIOFFICE_EXPORT OpenXmlContentModelCache
{
public:
    explicit OpenXmlContentModelCache(OpenXml::FileFormatVersions targetVersion) noexcept;

    /**
     * @brief Returns the automaton for @p particle able to match @p childCount children.
     *
     * The returned reference stays valid for the lifetime of the cache.
     */
    [[nodiscard]] const OpenXmlContentModelAutomaton& Get(const MetadataParticlePtr& particle, Size childCount);

    [[nodiscard]] Size AutomatonCount() const noexcept { return automata_.size(); }
    void Clear() noexcept;

private:
    /** The particle, and the clamp it was compiled for; UnclampedBound for models that need none. */
    using Key = std::pair<const MetadataParticle*, Size>;

    OpenXml::FileFormatVersions targetVersion_;
    /** `std::map` rather than a hash: the key is a pair, and a run holds a few dozen entries. */
    std::map<Key, OpenXmlContentModelAutomaton> automata_;
    /** Answers of @ref OpenXmlContentModelAutomaton::NeedsOccurrenceBound, which is a tree walk. */
    std::map<const MetadataParticle*, bool> needsBound_;
};

} // namespace ExyokiOffice
