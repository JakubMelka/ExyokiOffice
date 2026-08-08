// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "OpenXmlContentModel.hpp"

#include "ExyokiOffice/MarkupCompatibility.hpp"
#include "ExyokiOffice/OpenXMLElement.hpp"
#include "OpenXmlDiagnosticNames.hpp"
#include "OpenXmlVersionSupport.hpp"

#include <algorithm>
#include <bit>
#include <iterator>
#include <limits>
#include <utility>

namespace ExyokiOffice
{

using Detail::SupportsOfficeVersion;

bool ContentModelWildcardMatches(std::string_view wildcard,
                                 std::string_view namespaceUri,
                                 std::string_view targetNamespace) noexcept
{
    if (wildcard.empty() || wildcard == "##any")
    {
        return true;
    }

    Size start = 0;
    while (start < wildcard.size())
    {
        while (start < wildcard.size() && wildcard[start] == ' ')
        {
            ++start;
        }
        const auto end = wildcard.find(' ', start);
        const auto token = wildcard.substr(start, end == std::string_view::npos ? wildcard.size() - start
                                                                                : end - start);
        if ((token == "##local" && namespaceUri.empty()) ||
            (token == "##targetNamespace" && namespaceUri == targetNamespace) ||
            (token == "##other" && !namespaceUri.empty() && namespaceUri != targetNamespace) ||
            token == namespaceUri)
        {
            return true;
        }
        if (end == std::string_view::npos)
        {
            break;
        }
        start = end + 1;
    }
    return false;
}

/**
 * @internal
 * @brief Fixed-width bit sets over `std::vector<UInt64>`, as the simulation uses them.
 *
 * A named helper rather than a class template: there is one bit-set shape here -
 * a word vector the caller sized once - and spelling the four operations out
 * keeps the simulation readable.
 */
class ContentModelBits
{
public:
    [[nodiscard]] static Size WordCount(Size bitCount) noexcept { return (bitCount + 63) / 64; }

    static void Set(std::vector<UInt64>& bits, Size index) noexcept
    {
        bits[index / 64] |= UInt64{1} << (index % 64);
    }

    [[nodiscard]] static bool Test(const std::vector<UInt64>& bits, Size index) noexcept
    {
        return (bits[index / 64] & (UInt64{1} << (index % 64))) != 0;
    }

    [[nodiscard]] static bool Any(const std::vector<UInt64>& bits) noexcept
    {
        return std::any_of(bits.begin(), bits.end(), [](UInt64 word)
                           { return word != 0; });
    }

    [[nodiscard]] static bool Intersects(const std::vector<UInt64>& left,
                                         const std::vector<UInt64>& right) noexcept
    {
        for (Size index = 0; index < left.size() && index < right.size(); ++index)
        {
            if ((left[index] & right[index]) != 0)
            {
                return true;
            }
        }
        return false;
    }

    /** Calls @p visitor with the index of every set bit, in increasing order. */
    template <typename Visitor>
    static void ForEach(const std::vector<UInt64>& bits, Visitor visitor)
    {
        for (Size word = 0; word < bits.size(); ++word)
        {
            for (auto remaining = bits[word]; remaining != 0; remaining &= remaining - 1)
            {
                visitor(word * 64 + static_cast<Size>(std::countr_zero(remaining)));
            }
        }
    }
};

/**
 * @internal
 * @brief One state of the simulation: where it can be, and which `xs:all` members it used.
 *
 * The position set is a plain bit set because positions merge freely - two ways
 * of reaching the same position are the same state. The `xs:all` masks do not
 * merge: reaching a position having already taken member 3 is a different state
 * from reaching it not having taken member 3. Configurations are therefore kept
 * apart by their masks and merged by their positions, which for a model without
 * an `xs:all` leaves exactly one configuration and reduces the whole thing to a
 * bit set.
 */
struct ContentModelConfiguration
{
    std::vector<UInt64> Positions;
    std::vector<UInt64> Masks;
};

/**
 * @internal
 * @brief Compiles a particle tree into positions and transitions.
 *
 * Every method returns a @ref Fragment, which is what Glushkov's construction
 * needs to know about a sub-expression: whether it accepts the empty sequence,
 * which positions can start it, and which can end it. The transitions
 * themselves never appear in a fragment - they are written straight into the
 * automaton's position table as the fragments are combined.
 */
class OpenXmlContentModelAutomaton::Builder
{
public:
    Builder(OpenXmlContentModelAutomaton& automaton, Size occurrenceBound) noexcept
        : automaton_(automaton), occurrenceBound_(occurrenceBound)
    {
    }

    /**
     * @brief Compiles @p root and leaves the automaton ready to match.
     *
     * The virtual start position is created first, so that it is position 0 and
     * `Follow[0]` is the set of positions a match may begin at.
     */
    void Build(const MetadataParticlePtr& root)
    {
        automaton_.positions_.emplace_back();
        automaton_.positions_.front().Kind = MetadataParticleKind::Sequence; // neither Element nor Any: matches nothing

        const auto model = root ? BuildParticle(root) : EmptyFragment();
        automaton_.positions_.front().Follow = model.First;

        automaton_.accepting_.assign(ContentModelBits::WordCount(automaton_.positions_.size()), 0);
        // A void model matches nothing at all, not even the empty sequence, so
        // the start position stays unmarked and every document fails against it.
        if (model.Nullable)
        {
            ContentModelBits::Set(automaton_.accepting_, 0);
        }
        for (const auto position : model.Last)
        {
            ContentModelBits::Set(automaton_.accepting_, position);
        }

        for (auto& position : automaton_.positions_)
        {
            std::sort(position.Follow.begin(), position.Follow.end());
            position.Follow.erase(std::unique(position.Follow.begin(), position.Follow.end()),
                                  position.Follow.end());
        }
    }

private:
    /**
     * @brief What Glushkov's construction knows about one sub-expression.
     *
     * @ref Void distinguishes "matches nothing at all" from "matches the empty
     * sequence": an `xs:choice` with no branches left after version filtering is
     * the former, and concatenating anything onto it still matches nothing.
     * Without the distinction the two collapse, and a model that should reject
     * every document would start accepting one.
     */
    struct Fragment
    {
        bool Nullable = false;
        bool Void = false;
        std::vector<Size> First;
        std::vector<Size> Last;
    };

    static Fragment EmptyFragment() { return Fragment{true, false, {}, {}}; }
    static Fragment VoidFragment() { return Fragment{false, true, {}, {}}; }

    /** `A B`: whatever can end A may be followed by whatever can start B. */
    Fragment Concatenate(Fragment left, Fragment right)
    {
        if (left.Void || right.Void)
        {
            // The positions the surviving side already created stay in the
            // table with nothing leading to them, which is the one thing that
            // makes the reachability check in SelfCheck() not hold.
            automaton_.hasVoidBranch_ =
                automaton_.hasVoidBranch_ || !left.First.empty() || !right.First.empty();
            return VoidFragment();
        }

        AddTransitions(left.Last, right.First);

        Fragment result;
        result.Nullable = left.Nullable && right.Nullable;
        result.First = std::move(left.First);
        if (left.Nullable)
        {
            Append(result.First, right.First);
        }
        result.Last = std::move(right.Last);
        if (right.Nullable)
        {
            Append(result.Last, left.Last);
        }
        return result;
    }

    /** `A | B`: the branches share a start and an end and never see each other. */
    static Fragment Alternate(Fragment left, Fragment right)
    {
        if (left.Void)
        {
            return right;
        }
        if (right.Void)
        {
            return left;
        }

        Fragment result;
        result.Nullable = left.Nullable || right.Nullable;
        result.First = std::move(left.First);
        Append(result.First, right.First);
        result.Last = std::move(left.Last);
        Append(result.Last, right.Last);
        return result;
    }

    /** `A?`: the same positions, now also satisfied by consuming nothing. */
    static Fragment Optional(Fragment fragment)
    {
        if (fragment.Void)
        {
            return EmptyFragment();
        }
        fragment.Nullable = true;
        return fragment;
    }

    /** `A+`: whatever can end A may be followed by whatever can start A. */
    void Loop(const Fragment& fragment) { AddTransitions(fragment.Last, fragment.First); }

    /**
     * @brief Merges the sorted @p source into the sorted @p target.
     *
     * Every fragment keeps its position sets sorted, so this is a merge rather
     * than a sort. It matters: combining the copies of a repeated body calls
     * this once per copy, and re-sorting a set that grows with the document
     * would put a quadratic back into the compilation.
     */
    static void Append(std::vector<Size>& target, const std::vector<Size>& source)
    {
        if (source.empty())
        {
            return;
        }
        if (target.empty())
        {
            target = source;
            return;
        }

        std::vector<Size> merged;
        merged.reserve(target.size() + source.size());
        std::set_union(target.begin(), target.end(), source.begin(), source.end(),
                       std::back_inserter(merged));
        target.swap(merged);
    }

    void AddTransitions(const std::vector<Size>& from, const std::vector<Size>& to)
    {
        if (to.empty())
        {
            return;
        }

        transitions_ += from.size() * to.size();
        if (transitions_ > TransitionBudget)
        {
            Decline("the compiled content model would exceed " + std::to_string(TransitionBudget) +
                    " transitions");
            return;
        }

        for (const auto position : from)
        {
            auto& follow = automaton_.positions_[position].Follow;
            follow.insert(follow.end(), to.begin(), to.end());
        }
    }

    /** Fails the whole compilation; @ref OpenXmlContentModelAutomaton::IsSupported then says so. */
    void Decline(std::string reason)
    {
        if (automaton_.unsupportedReason_.empty())
        {
            automaton_.unsupportedReason_ = std::move(reason);
        }
    }

    Size NewPosition()
    {
        if (automaton_.positions_.size() >= PositionBudget)
        {
            Decline("the compiled content model would exceed " + std::to_string(PositionBudget) +
                    " positions");
            // Reusing the start position keeps the build from growing further
            // while the recursion unwinds; nothing will ever match with it.
            return 0;
        }
        automaton_.positions_.emplace_back();
        return automaton_.positions_.size() - 1;
    }

    /** Applies the occurrence bounds of @p particle to repeated copies of its body. */
    Fragment BuildParticle(const MetadataParticlePtr& particle)
    {
        if (!particle || !SupportsOfficeVersion(automaton_.targetVersion_, particle->Version()))
        {
            // A particle the target Office generation does not have is not a
            // particle that matches nothing - it is one the schema does not
            // declare at all, which is the empty sequence.
            return EmptyFragment();
        }
        if (!automaton_.unsupportedReason_.empty())
        {
            return VoidFragment();
        }

        // A schema that asks for more occurrences than it allows asks for
        // something no document can supply. XSD forbids it, but the metadata is
        // imported rather than written here, so it is answered rather than
        // assumed away.
        if (particle->MaxOccurs() && *particle->MaxOccurs() < particle->MinOccurs())
        {
            return VoidFragment();
        }

        // A minimum larger than the number of children can never be met, and
        // one copy more than there are children says exactly that; a maximum
        // larger than the number of children can never be reached, so it says
        // nothing the child count does not already say. Clamping both is what
        // keeps `maxOccurs="65430"` from compiling into 65430 copies.
        Size minimum = std::min<Size>(particle->MinOccurs(), SaturatingIncrement(occurrenceBound_));
        bool loops = !particle->MaxOccurs().has_value();
        Size maximum = loops ? std::max<Size>(minimum, 1)
                             : std::max(minimum, std::min<Size>(*particle->MaxOccurs(), occurrenceBound_));

        // A body that accepts the empty sequence can always be occurred one more
        // time without consuming anything, so the minimum never binds: whatever
        // the schema asks for can be padded out with occurrences that match
        // nothing.
        const bool bodyMatchesEmpty = BodyMatchesEmpty(particle);
        if (bodyMatchesEmpty)
        {
            minimum = 0;
        }

        // Below, every occurrence of the body consumes at least one child, so a
        // document of N children can never use more than N of them. A maximum at
        // least as large as the child count therefore forbids nothing the
        // document could have done anyway, and the repetition is the same
        // language as an unbounded one over any document this automaton will be
        // asked about. Saying so is what keeps two nested numeric bounds - the
        // custom UI schemas put a `maxOccurs="1000"` group inside a
        // `maxOccurs="1000"` choice - from compiling into their product.
        //
        // Only where there is something to save, and never over an `xs:all`.
        // Turning a particle that occurs once into a loop saves no positions at
        // all, and a loop is the one place an `xs:all` cannot go - laying the
        // copies out one after another keeps each of them a block of its own,
        // with its own record of which members it has taken.
        if (!loops && maximum > 1 && *particle->MaxOccurs() >= occurrenceBound_ &&
            !ContainsAll(particle))
        {
            loops = true;
            maximum = std::max<Size>(minimum, 1);
        }

        if (maximum == 0)
        {
            return EmptyFragment();
        }

        // Copies are built left to right, so position numbers follow document
        // order and a diagnostic lists the names in the order the schema does.
        //
        // An `xs:all` anywhere below a repetition that loops cannot be
        // represented, and "below" means at any depth: the loop is what revisits
        // its positions, however many particles sit in between.
        loopDepth_ += loops ? 1 : 0;
        std::vector<Fragment> copies;
        copies.reserve(maximum);
        for (Size index = 0; index < maximum; ++index)
        {
            copies.push_back(BuildBody(particle));
        }
        loopDepth_ -= loops ? 1 : 0;
        if (loops)
        {
            // Only the last copy loops; the ones before it are the mandatory
            // minimum, which a loop would let the document skip.
            Loop(copies.back());
        }

        // The nullability of the body was needed before it was compiled, so it
        // was worked out from the particle tree. Now that the fragment exists,
        // the two are compared: a disagreement means the collapse above may have
        // been applied to a body it does not hold for, which is a reason to
        // decline the model rather than to compile a language nobody chose.
        if (copies.front().Nullable != bodyMatchesEmpty)
        {
            Decline("a body whose emptiness the compiler and the particle tree disagree about");
            return VoidFragment();
        }

        // With the minimum gone, `B{0,n}` for an empty-matching B accepts
        // exactly the concatenations of at most n *non-empty* words of B: the
        // empty occurrences contribute nothing and drop out. Saying so - a
        // fragment stops matching the empty sequence while its positions and
        // transitions stay as they were - turns every repetition into a chain of
        // copies, each handing on to the next. The alternative is to let a copy
        // be skipped, which needs a transition from every copy to every later
        // one, and `maxOccurs="1000"` on an optional group then compiles into a
        // million of them.
        if (bodyMatchesEmpty)
        {
            for (auto& copy : copies)
            {
                copy.Nullable = false;
            }
        }

        return Combine(std::move(copies), minimum);
    }

    /**
     * @brief Reports whether one occurrence of @p particle's body can consume no children.
     *
     * Read off the particle tree rather than off a compiled fragment, because
     * @ref BuildParticle needs the answer before it decides how many copies of
     * the body to compile. The answer is checked against the fragment as soon as
     * there is one.
     */
    bool BodyMatchesEmpty(const MetadataParticlePtr& particle) const
    {
        switch (particle->Kind())
        {
            case MetadataParticleKind::Element:
            case MetadataParticleKind::Any:
                return false;

            case MetadataParticleKind::Choice:
            {
                const auto& composite = static_cast<const MetadataCompositeParticle&>(*particle);
                return std::any_of(composite.Children().begin(), composite.Children().end(),
                                   [this](const MetadataParticlePtr& child)
                                   { return ParticleMatchesEmpty(child); });
            }

            case MetadataParticleKind::Sequence:
            case MetadataParticleKind::Group:
            case MetadataParticleKind::All:
            {
                const auto& composite = static_cast<const MetadataCompositeParticle&>(*particle);
                return std::all_of(composite.Children().begin(), composite.Children().end(),
                                   [this](const MetadataParticlePtr& child)
                                   { return ParticleMatchesEmpty(child); });
            }
        }
        return false;
    }

    /** Reports whether @p particle or anything below it is an `xs:all`. */
    static bool ContainsAll(const MetadataParticlePtr& particle)
    {
        if (!particle)
        {
            return false;
        }
        if (particle->Kind() == MetadataParticleKind::All)
        {
            return true;
        }
        if (particle->Kind() == MetadataParticleKind::Element ||
            particle->Kind() == MetadataParticleKind::Any)
        {
            return false;
        }

        const auto& composite = static_cast<const MetadataCompositeParticle&>(*particle);
        return std::any_of(composite.Children().begin(), composite.Children().end(), ContainsAll);
    }

    /** Reports whether @p particle, occurrence bounds and all, can consume no children. */
    bool ParticleMatchesEmpty(const MetadataParticlePtr& particle) const
    {
        if (!particle || !SupportsOfficeVersion(automaton_.targetVersion_, particle->Version()))
        {
            return true;
        }
        if (particle->MaxOccurs() && *particle->MaxOccurs() < particle->MinOccurs())
        {
            // Matches nothing whatsoever, which is not the same as matching the
            // empty sequence.
            return false;
        }
        return particle->MinOccurs() == 0 || BodyMatchesEmpty(particle);
    }

    /**
     * @brief Chains @p copies of one repeated body into a single fragment.
     *
     * No copy matches the empty sequence by the time it gets here, so copy
     * `i + 1` can only begin where copy `i` ended: one transition per junction,
     * and the run may stop after any copy from the minimum onwards. That is what
     * keeps `maxOccurs="65430"` on an element affordable - a chain of positions,
     * not a graph where every position leads to every later one.
     */
    Fragment Combine(std::vector<Fragment> copies, Size minimum)
    {
        if (copies.front().Void)
        {
            automaton_.hasVoidBranch_ = true;
            return minimum == 0 ? EmptyFragment() : VoidFragment();
        }

        for (Size index = 0; index + 1 < copies.size(); ++index)
        {
            AddTransitions(copies[index].Last, copies[index + 1].First);
        }

        Fragment result;
        result.Nullable = minimum == 0;
        result.First = std::move(copies.front().First);
        for (Size index = std::max<Size>(minimum, 1); index <= copies.size(); ++index)
        {
            result.Last.insert(result.Last.end(), copies[index - 1].Last.begin(),
                               copies[index - 1].Last.end());
        }
        std::sort(result.Last.begin(), result.Last.end());
        result.Last.erase(std::unique(result.Last.begin(), result.Last.end()), result.Last.end());
        return result;
    }

    /** Compiles one occurrence of @p particle, ignoring its occurrence bounds. */
    Fragment BuildBody(const MetadataParticlePtr& particle)
    {
        switch (particle->Kind())
        {
            case MetadataParticleKind::Element:
            case MetadataParticleKind::Any:
            {
                const auto position = NewPosition();
                if (position == 0)
                {
                    return VoidFragment();
                }
                auto& leaf = automaton_.positions_[position];
                leaf.Kind = particle->Kind();
                if (particle->Kind() == MetadataParticleKind::Element)
                {
                    leaf.Name = static_cast<const MetadataElementParticle&>(*particle).Element();
                }
                else
                {
                    leaf.Wildcard = static_cast<const MetadataAnyParticle&>(*particle).Wildcard();
                }
                return Fragment{false, false, {position}, {position}};
            }

            case MetadataParticleKind::Sequence:
            case MetadataParticleKind::Group:
            {
                const auto& composite = static_cast<const MetadataCompositeParticle&>(*particle);
                Fragment result = EmptyFragment();
                for (const auto& child : composite.Children())
                {
                    result = Concatenate(std::move(result), BuildParticle(child));
                }
                return result;
            }

            case MetadataParticleKind::Choice:
            {
                const auto& composite = static_cast<const MetadataCompositeParticle&>(*particle);
                Fragment result = VoidFragment();
                for (const auto& child : composite.Children())
                {
                    result = Alternate(std::move(result), BuildParticle(child));
                }
                return result;
            }

            case MetadataParticleKind::All:
                return BuildAll(particle);
        }
        return VoidFragment();
    }

    /**
     * @brief Compiles an `xs:all` into a member clique plus a "taken" bitmask.
     *
     * The transitions say "after any member, any other member may follow",
     * which on its own would accept a member twice. The mask the simulation
     * carries is what forbids that, and it can only do so because every member
     * is a single position: XSD 1.0 allows nothing but elements with
     * `maxOccurs="1"` inside an `xs:all`, so taking a member and reaching its
     * one position are the same event. A model that breaks that rule is
     * declined rather than approximated.
     *
     * The mask also has no way to reset, so an `xs:all` inside an unbounded
     * repetition - where the same positions are visited again on the next turn
     * around the loop - is declined for the same reason. Bounded repetition is
     * fine: each copy compiles to its own block with its own bits.
     */
    Fragment BuildAll(const MetadataParticlePtr& particle)
    {
        if (loopDepth_ != 0)
        {
            Decline("an xs:all inside an unbounded repetition");
            return VoidFragment();
        }

        const auto& composite = static_cast<const MetadataCompositeParticle&>(*particle);
        const Size blockIndex = automaton_.allBlocks_.size();
        automaton_.allBlocks_.emplace_back();

        std::vector<Size> members;
        std::vector<bool> required;
        for (const auto& child : composite.Children())
        {
            if (!child || !SupportsOfficeVersion(automaton_.targetVersion_, child->Version()))
            {
                continue;
            }
            if (child->Kind() != MetadataParticleKind::Element && child->Kind() != MetadataParticleKind::Any)
            {
                Decline("an xs:all whose member is not a single element");
                automaton_.hasVoidBranch_ = true;
                return VoidFragment();
            }
            if (!child->MaxOccurs() || *child->MaxOccurs() > 1 || child->MinOccurs() > 1)
            {
                Decline("an xs:all whose member may appear more than once");
                automaton_.hasVoidBranch_ = true;
                return VoidFragment();
            }
            if (*child->MaxOccurs() == 0)
            {
                continue;
            }

            const auto fragment = BuildBody(child);
            if (fragment.First.size() != 1)
            {
                Decline("an xs:all whose member did not compile to a single position");
                automaton_.hasVoidBranch_ = true;
                return VoidFragment();
            }

            const auto position = fragment.First.front();
            automaton_.positions_[position].AllBlock = blockIndex;
            automaton_.positions_[position].AllMember = members.size();
            members.push_back(position);
            required.push_back(child->MinOccurs() != 0);
        }

        auto& block = automaton_.allBlocks_[blockIndex];
        block.MemberCount = members.size();
        block.Required.assign(ContentModelBits::WordCount(std::max<Size>(members.size(), 1)), 0);
        for (Size index = 0; index < required.size(); ++index)
        {
            if (required[index])
            {
                ContentModelBits::Set(block.Required, index);
            }
        }

        if (members.empty())
        {
            return EmptyFragment();
        }

        // Every member may follow every other one; the mask stops it from
        // following itself, so no self-transition is needed.
        for (const auto from : members)
        {
            for (const auto to : members)
            {
                if (from != to)
                {
                    automaton_.positions_[from].Follow.push_back(to);
                }
            }
        }

        std::sort(members.begin(), members.end());
        return Fragment{!ContentModelBits::Any(block.Required), false, members, members};
    }

    [[nodiscard]] static Size SaturatingIncrement(Size value) noexcept
    {
        return value == std::numeric_limits<Size>::max() ? value : value + 1;
    }

    /**
     * The cap on how large a compiled model may grow. Only the repeat-the-body
     * compilation of a numeric occurrence bound can approach it, and only for an
     * element that really does hold that many children. Passing it declines the
     * model, which sends the caller to the reference matcher - and the flat
     * `a{1,65430}` shape that gets here is one the reference matcher handles in
     * linear time anyway.
     */
    static constexpr Size PositionBudget = 200000;

    /**
     * The same kind of ceiling on the transition table. Positions grow one per
     * repeated occurrence, but transitions can grow faster than that where the
     * repeated body is optional, so the two need separate limits.
     */
    static constexpr Size TransitionBudget = 2000000;

    OpenXmlContentModelAutomaton& automaton_;
    Size occurrenceBound_;
    /** How many looping repetitions the recursion is currently inside. */
    Size loopDepth_ = 0;
    /** Transitions asked for so far, including any the budget refused. */
    Size transitions_ = 0;
};

OpenXmlContentModelAutomaton::OpenXmlContentModelAutomaton(const MetadataParticlePtr& particle,
                                                           OpenXml::FileFormatVersions targetVersion,
                                                           Size occurrenceBound)
    : occurrenceBound_(occurrenceBound), targetVersion_(targetVersion)
{
    Builder builder(*this, occurrenceBound);
    builder.Build(particle);
}

bool OpenXmlContentModelAutomaton::NeedsOccurrenceBound(const MetadataParticlePtr& particle)
{
    if (!particle)
    {
        return false;
    }
    if (particle->MinOccurs() > 1 || (particle->MaxOccurs() && *particle->MaxOccurs() > 1))
    {
        return true;
    }
    if (particle->Kind() == MetadataParticleKind::Element || particle->Kind() == MetadataParticleKind::Any)
    {
        return false;
    }

    const auto& composite = static_cast<const MetadataCompositeParticle&>(*particle);
    return std::any_of(composite.Children().begin(), composite.Children().end(),
                       [](const MetadataParticlePtr& child)
                       { return NeedsOccurrenceBound(child); });
}

Size OpenXmlContentModelAutomaton::RoundOccurrenceBound(Size childCount) noexcept
{
    Size rounded = 1;
    while (rounded < childCount)
    {
        rounded *= 2;
    }
    return rounded;
}

Size OpenXmlContentModelAutomaton::TransitionCount() const noexcept
{
    Size total = 0;
    for (const auto& position : positions_)
    {
        total += position.Follow.size();
    }
    return total;
}

ContentModelMatch OpenXmlContentModelAutomaton::Match(const std::vector<std::shared_ptr<OpenXMLElement>>& children,
                                                      std::string_view targetNamespace) const
{
    const auto positionWords = ContentModelBits::WordCount(positions_.size());
    Size maskWords = 0;
    for (const auto& block : allBlocks_)
    {
        maskWords += block.Required.size();
    }

    std::vector<ContentModelConfiguration> current;
    current.push_back(ContentModelConfiguration{std::vector<UInt64>(positionWords, 0),
                                                std::vector<UInt64>(maskWords, 0)});
    ContentModelBits::Set(current.front().Positions, 0);

    ContentModelMatch match;
    std::vector<ContentModelConfiguration> next;
    for (Size index = 0; index < children.size(); ++index)
    {
        const auto& child = children[index];
        // ECMA-376 Part 3 lets `mc:AlternateContent` stand wherever an element
        // of the surrounding vocabulary may stand, and which element it stands
        // for is a branch selection the schema cannot express. It therefore
        // takes every transition out of the current state, whatever the labels.
        const bool transparent =
            child && child->QualifiedName() == MarkupCompatibilityNames::AlternateContent();

        next.clear();
        for (const auto& configuration : current)
        {
            Step(configuration, child.get(), transparent, targetNamespace, positionWords, next);
        }

        if (next.empty())
        {
            match.ChildIndex = index;
            match.Expected = CollectExpected(current, match.ExpectedWildcard);
            return match;
        }
        current.swap(next);
    }

    for (const auto& configuration : current)
    {
        if (IsAccepting(configuration))
        {
            match.Accepted = true;
            return match;
        }
    }

    match.ChildIndex = children.size();
    match.Expected = CollectExpected(current, match.ExpectedWildcard);
    return match;
}

/** Advances one configuration over one child, appending the results to @p next. */
void OpenXmlContentModelAutomaton::Step(const ContentModelConfiguration& configuration,
                                        const OpenXMLElement* child,
                                        bool transparent,
                                        std::string_view targetNamespace,
                                        Size positionWords,
                                        std::vector<ContentModelConfiguration>& next) const
{
    ContentModelBits::ForEach(configuration.Positions,
                              [&](Size from)
                              {
                                  for (const auto to : positions_[from].Follow)
                                  {
                                      if (!Admits(to, child, transparent, targetNamespace) ||
                                          !MayLeaveAllBlock(configuration.Masks, from, to))
                                      {
                                          continue;
                                      }

                                      const auto& position = positions_[to];
                                      if (position.AllBlock == NoAllBlock)
                                      {
                                          ContentModelBits::Set(FindConfiguration(next, configuration.Masks,
                                                                                  positionWords),
                                                                to);
                                          continue;
                                      }

                                      // Taking an `xs:all` member is only legal once, and the
                                      // configuration it leads to is the one that remembers it.
                                      auto masks = configuration.Masks;
                                      const auto bit = MaskBit(position);
                                      if (ContentModelBits::Test(masks, bit))
                                      {
                                          continue;
                                      }
                                      ContentModelBits::Set(masks, bit);
                                      ContentModelBits::Set(FindConfiguration(next, masks, positionWords), to);
                                  }
                              });
}

/** The index of @p position's member bit in the flat mask vector. */
Size OpenXmlContentModelAutomaton::MaskBit(const Position& position) const noexcept
{
    Size offset = 0;
    for (Size index = 0; index < position.AllBlock; ++index)
    {
        offset += allBlocks_[index].Required.size() * 64;
    }
    return offset + position.AllMember;
}

/** Returns the position set of the configuration in @p configurations carrying @p masks. */
std::vector<UInt64>& OpenXmlContentModelAutomaton::FindConfiguration(
    std::vector<ContentModelConfiguration>& configurations,
    const std::vector<UInt64>& masks,
    Size positionWords)
{
    for (auto& configuration : configurations)
    {
        if (configuration.Masks == masks)
        {
            return configuration.Positions;
        }
    }
    configurations.push_back(ContentModelConfiguration{std::vector<UInt64>(positionWords, 0), masks});
    return configurations.back().Positions;
}

/** Reports whether the child at hand may be matched by @p position. */
bool OpenXmlContentModelAutomaton::Admits(Size position,
                                          const OpenXMLElement* child,
                                          bool transparent,
                                          std::string_view targetNamespace) const
{
    if (!child)
    {
        return false;
    }
    if (transparent)
    {
        return true;
    }

    const auto& candidate = positions_[position];
    if (candidate.Kind == MetadataParticleKind::Element)
    {
        return candidate.Name == child->QualifiedName();
    }
    return ContentModelWildcardMatches(candidate.Wildcard, child->QualifiedName().namespaceUri(),
                                       targetNamespace);
}

/**
 * @brief Reports whether an `xs:all` block has taken every member it requires.
 *
 * @param masks The configuration's member bits.
 * @param block The block to ask about.
 */
bool OpenXmlContentModelAutomaton::IsAllBlockComplete(const std::vector<UInt64>& masks, Size block) const
{
    Size offset = 0;
    for (Size index = 0; index < block; ++index)
    {
        offset += allBlocks_[index].Required.size();
    }
    for (Size word = 0; word < allBlocks_[block].Required.size(); ++word)
    {
        if ((allBlocks_[block].Required[word] & ~masks[offset + word]) != 0)
        {
            return false;
        }
    }
    return true;
}

/**
 * @brief Reports whether the step from @p from to @p to is allowed to leave an `xs:all`.
 *
 * The transitions built for an `xs:all` say "after any member, anything the
 * whole block could be followed by", because Glushkov's `last` set of the block
 * is all of its members. That over-states it: a block with a required member
 * cannot be finished until that member has been taken, so a step out of the
 * block is only legal once it is complete. Leaving that to the acceptance check
 * at the end of the document still reaches the right verdict - the member bits
 * are never cleared - but it lets the run walk on past the real mistake and
 * blames a later child for it.
 */
bool OpenXmlContentModelAutomaton::MayLeaveAllBlock(const std::vector<UInt64>& masks,
                                                    Size from,
                                                    Size to) const
{
    const auto block = positions_[from].AllBlock;
    return block == NoAllBlock || positions_[to].AllBlock == block || IsAllBlockComplete(masks, block);
}

/**
 * @brief Reports whether the run may stop in @p configuration.
 *
 * Being at a position the model can end at is not enough where an `xs:all` is
 * involved: a block that was entered has to have taken every member the schema
 * requires. A block whose mask is still empty was never entered on this path,
 * so it makes no demand - which is what makes a required member inside an
 * `xs:all` that sits in an untaken branch of a choice stay silent.
 */
bool OpenXmlContentModelAutomaton::IsAccepting(const ContentModelConfiguration& configuration) const
{
    if (!ContentModelBits::Intersects(configuration.Positions, accepting_))
    {
        return false;
    }

    Size offset = 0;
    for (Size index = 0; index < allBlocks_.size(); ++index)
    {
        bool entered = false;
        for (Size word = 0; word < allBlocks_[index].Required.size(); ++word)
        {
            entered = entered || configuration.Masks[offset + word] != 0;
        }
        offset += allBlocks_[index].Required.size();

        if (entered && !IsAllBlockComplete(configuration.Masks, index))
        {
            return false;
        }
    }
    return true;
}

/** Collects the element names the model would admit next, in content-model order. */
std::vector<OpenXmlQualifiedName> OpenXmlContentModelAutomaton::CollectExpected(
    const std::vector<ContentModelConfiguration>& configurations,
    bool& expectedWildcard) const
{
    std::vector<UInt64> reachable(ContentModelBits::WordCount(positions_.size()), 0);
    for (const auto& configuration : configurations)
    {
        ContentModelBits::ForEach(configuration.Positions,
                                  [&](Size from)
                                  {
                                      for (const auto to : positions_[from].Follow)
                                      {
                                          const auto& position = positions_[to];
                                          if (position.AllBlock != NoAllBlock &&
                                              ContentModelBits::Test(configuration.Masks, MaskBit(position)))
                                          {
                                              continue;
                                          }
                                          if (!MayLeaveAllBlock(configuration.Masks, from, to))
                                          {
                                              continue;
                                          }
                                          ContentModelBits::Set(reachable, to);
                                      }
                                  });
    }

    std::vector<OpenXmlQualifiedName> expected;
    expectedWildcard = false;
    ContentModelBits::ForEach(reachable,
                              [&](Size index)
                              {
                                  const auto& position = positions_[index];
                                  if (position.Kind == MetadataParticleKind::Any)
                                  {
                                      expectedWildcard = true;
                                      return;
                                  }
                                  if (std::find(expected.begin(), expected.end(), position.Name) ==
                                      expected.end())
                                  {
                                      expected.push_back(position.Name);
                                  }
                              });
    return expected;
}

std::string OpenXmlContentModelAutomaton::Describe() const
{
    std::string text = "content model automaton: " + std::to_string(positions_.size()) + " positions, " +
                       std::to_string(TransitionCount()) + " transitions, occurrence bound ";
    text += occurrenceBound_ == UnclampedBound ? "none" : std::to_string(occurrenceBound_);
    text.push_back('\n');
    if (!unsupportedReason_.empty())
    {
        text += "  declined: " + unsupportedReason_ + "\n";
        return text;
    }

    for (Size index = 0; index < positions_.size(); ++index)
    {
        const auto& position = positions_[index];
        text += "  " + std::to_string(index) + " ";
        if (index == 0)
        {
            text += "<start>";
        }
        else if (position.Kind == MetadataParticleKind::Any)
        {
            text += "any(" + (position.Wildcard.empty() ? std::string("##any") : position.Wildcard) + ")";
        }
        else
        {
            text += Detail::DescribeQualifiedName(position.Name);
        }
        if (position.AllBlock != NoAllBlock)
        {
            text += " [all " + std::to_string(position.AllBlock) + " member " +
                    std::to_string(position.AllMember) + "]";
        }
        if (ContentModelBits::Test(accepting_, index))
        {
            text += " [accepting]";
        }

        text += " ->";
        for (const auto follow : position.Follow)
        {
            text += " " + std::to_string(follow);
        }
        text.push_back('\n');
    }

    for (Size index = 0; index < allBlocks_.size(); ++index)
    {
        text += "  all " + std::to_string(index) + ": " + std::to_string(allBlocks_[index].MemberCount) +
                " members, required";
        bool any = false;
        for (Size member = 0; member < allBlocks_[index].MemberCount; ++member)
        {
            if (ContentModelBits::Test(allBlocks_[index].Required, member))
            {
                text += " " + std::to_string(member);
                any = true;
            }
        }
        if (!any)
        {
            text += " none";
        }
        text.push_back('\n');
    }
    return text;
}

std::vector<std::string> OpenXmlContentModelAutomaton::SelfCheck() const
{
    std::vector<std::string> problems;
    if (!IsSupported())
    {
        return problems;
    }

    if (positions_.empty())
    {
        problems.emplace_back("the automaton has no start position");
        return problems;
    }
    if (accepting_.size() != ContentModelBits::WordCount(positions_.size()))
    {
        problems.emplace_back("the accepting set is not sized for the position count");
    }

    std::vector<bool> reachable(positions_.size(), false);
    reachable[0] = true;
    for (bool changed = true; changed;)
    {
        changed = false;
        for (Size index = 0; index < positions_.size(); ++index)
        {
            if (!reachable[index])
            {
                continue;
            }
            for (const auto follow : positions_[index].Follow)
            {
                if (follow < positions_.size() && !reachable[follow])
                {
                    reachable[follow] = true;
                    changed = true;
                }
            }
        }
    }

    for (Size index = 0; index < positions_.size(); ++index)
    {
        const auto& position = positions_[index];
        for (const auto follow : position.Follow)
        {
            if (follow >= positions_.size())
            {
                problems.push_back("position " + std::to_string(index) + " has a transition to " +
                                   std::to_string(follow) + ", which does not exist");
            }
            else if (follow == 0)
            {
                problems.push_back("position " + std::to_string(index) +
                                   " has a transition back to the start position");
            }
        }
        if (!reachable[index] && !hasVoidBranch_)
        {
            problems.push_back("position " + std::to_string(index) + " cannot be reached from the start");
        }
        if (index != 0 && position.Kind != MetadataParticleKind::Element &&
            position.Kind != MetadataParticleKind::Any)
        {
            problems.push_back("position " + std::to_string(index) + " is neither an element nor a wildcard");
        }
        if (position.AllBlock != NoAllBlock)
        {
            if (position.AllBlock >= allBlocks_.size())
            {
                problems.push_back("position " + std::to_string(index) + " names xs:all block " +
                                   std::to_string(position.AllBlock) + ", which does not exist");
            }
            else if (position.AllMember >= allBlocks_[position.AllBlock].MemberCount)
            {
                problems.push_back("position " + std::to_string(index) + " is member " +
                                   std::to_string(position.AllMember) + " of an xs:all with only " +
                                   std::to_string(allBlocks_[position.AllBlock].MemberCount) + " members");
            }
        }
    }
    return problems;
}

OpenXmlContentModelCache::OpenXmlContentModelCache(OpenXml::FileFormatVersions targetVersion) noexcept
    : targetVersion_(targetVersion)
{
}

const OpenXmlContentModelAutomaton& OpenXmlContentModelCache::Get(const MetadataParticlePtr& particle,
                                                                  Size childCount)
{
    const auto* key = particle.get();
    auto needs = needsBound_.find(key);
    if (needs == needsBound_.end())
    {
        needs = needsBound_.emplace(key, OpenXmlContentModelAutomaton::NeedsOccurrenceBound(particle)).first;
    }

    // A model whose occurrence bounds are all 0, 1 or unbounded compiles the
    // same way whatever the child count, so it gets one entry rather than one
    // per document shape. The rest are rounded to a power of two, because a
    // bound larger than the child count is just as correct and keeps a part
    // with a thousand differently sized elements from compiling a thousand
    // automata.
    const Size bound = needs->second ? OpenXmlContentModelAutomaton::RoundOccurrenceBound(childCount)
                                     : OpenXmlContentModelAutomaton::UnclampedBound;

    const Key entry{key, bound};
    const auto found = automata_.find(entry);
    if (found != automata_.end())
    {
        return found->second;
    }
    return automata_.emplace(entry, OpenXmlContentModelAutomaton(particle, targetVersion_, bound))
        .first->second;
}

void OpenXmlContentModelCache::Clear() noexcept
{
    automata_.clear();
    needsBound_.clear();
}

} // namespace ExyokiOffice
