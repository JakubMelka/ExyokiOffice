// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "OpenXmlContentModel.hpp"

#include "ExyokiOffice/MarkupCompatibility.hpp"
#include "ExyokiOffice/OpenXMLElement.hpp"
#include "OpenXmlVersionSupport.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <utility>

namespace ExyokiOffice
{

using Detail::SupportsOfficeVersion;

/**
 * @internal
 * @brief One run of the recursive matcher, holding the state that run accumulates.
 *
 * Split out of @ref OpenXmlContentModelReference so that the matcher itself
 * stays a value with no per-match state: the memo table and the failure
 * bookkeeping below only make sense for one particular sequence of children.
 */
class OpenXmlContentModelReferenceSession
{
public:
    OpenXmlContentModelReferenceSession(const std::vector<std::shared_ptr<OpenXMLElement>>& children,
                                        std::string_view targetNamespace,
                                        OpenXml::FileFormatVersions targetVersion)
        : children_(children), targetNamespace_(targetNamespace), targetVersion_(targetVersion)
    {
        // Answered once here rather than per match attempt: the search visits a
        // position many times, and almost no element has an alternative content
        // child at all.
        for (Size index = 0; index < children_.size(); ++index)
        {
            if (children_[index] &&
                children_[index]->QualifiedName() == MarkupCompatibilityNames::AlternateContent())
            {
                alternatives_.push_back(index);
            }
        }
    }

    ContentModelMatch Run(const MetadataParticlePtr& particle)
    {
        ContentModelMatch match;
        if (!particle)
        {
            match.Accepted = children_.empty();
        }
        else
        {
            const auto ends = Match(particle, 0);
            match.Accepted = std::find(ends.begin(), ends.end(), children_.size()) != ends.end();
        }

        if (match.Accepted)
        {
            return match;
        }

        // The furthest position any branch of the search consumed is the most
        // useful place to blame: everything before it fits the content model
        // under at least one reading.
        match.ChildIndex = std::min(furthestPosition_, children_.size());
        if (const auto expected = expectedNames_.find(match.ChildIndex); expected != expectedNames_.end())
        {
            match.Expected = expected->second;
        }
        match.ExpectedWildcard = expectedWildcard_.count(match.ChildIndex) != 0;
        return match;
    }

private:
    using Positions = std::vector<Size>;

    /** Notes that @p name was admissible at @p position, whether or not it was there. */
    void RecordExpectation(Size position, const OpenXmlQualifiedName& name)
    {
        auto& names = expectedNames_[position];
        if (std::find(names.begin(), names.end(), name) == names.end())
        {
            names.push_back(name);
        }
    }

    /** Notes that some branch of the search consumed children up to @p position. */
    void RecordProgress(Size position) { furthestPosition_ = std::max(furthestPosition_, position); }

    static void AddUnique(Positions& positions, Size value)
    {
        if (std::find(positions.begin(), positions.end(), value) == positions.end())
        {
            positions.push_back(value);
        }
    }

    Positions Match(const MetadataParticlePtr& particle, Size position)
    {
        if (!SupportsOfficeVersion(targetVersion_, particle->Version()))
        {
            return {position};
        }
        const auto key = std::make_pair(particle.get(), position);
        if (const auto cached = matchCache_.find(key); cached != matchCache_.end())
        {
            return cached->second;
        }

        Positions accepted;
        Positions frontier{position};
        if (particle->MinOccurs() == 0)
        {
            accepted.push_back(position);
        }

        const Size documentBound = children_.size() + particle->MinOccurs() + 1;
        const Size maximum = particle->MaxOccurs()
                                 ? std::min<Size>(*particle->MaxOccurs(), documentBound)
                                 : documentBound;

        for (Size occurrence = 1; occurrence <= maximum && !frontier.empty(); ++occurrence)
        {
            Positions next;
            for (const auto current : frontier)
            {
                for (const auto end : MatchSingle(particle, current))
                {
                    if (end != current || occurrence <= particle->MinOccurs())
                    {
                        AddUnique(next, end);
                    }
                }
            }
            frontier = std::move(next);
            if (occurrence >= particle->MinOccurs())
            {
                for (const auto end : frontier)
                {
                    AddUnique(accepted, end);
                }
            }
        }
        matchCache_.emplace(key, accepted);
        return accepted;
    }

    /**
     * @brief Reports whether the child at @p position stands in for a model element.
     *
     * ECMA-376 Part 3 lets `mc:AlternateContent` appear wherever an element of
     * the surrounding vocabulary may appear: it wraps several renderings of that
     * one element, of which a consumer takes the branch it understands. It
     * therefore occupies exactly one position in the content model, and which
     * name the model wanted there depends on a branch selection the schema
     * cannot express - so any position it may legally stand in is accepted.
     */
    bool IsTransparentAlternative(Size position) const
    {
        return !alternatives_.empty() &&
               std::find(alternatives_.begin(), alternatives_.end(), position) != alternatives_.end();
    }

    Positions MatchSingle(const MetadataParticlePtr& particle, Size position)
    {
        switch (particle->Kind())
        {
            case MetadataParticleKind::Element:
            {
                const auto& expected = static_cast<const MetadataElementParticle&>(*particle).Element();
                RecordExpectation(position, expected);
                if (position < children_.size() && children_[position] &&
                    (children_[position]->QualifiedName() == expected || IsTransparentAlternative(position)))
                {
                    RecordProgress(position + 1);
                    return {position + 1};
                }
                return {};
            }

            case MetadataParticleKind::Any:
                expectedWildcard_.insert(position);
                if (position < children_.size() && children_[position] &&
                    (IsTransparentAlternative(position) ||
                     MatchesWildcard(static_cast<const MetadataAnyParticle&>(*particle).Wildcard(),
                                     children_[position]->QualifiedName().namespaceUri())))
                {
                    RecordProgress(position + 1);
                    return {position + 1};
                }
                return {};

            case MetadataParticleKind::Choice:
            {
                Positions result;
                const auto& choice = static_cast<const MetadataCompositeParticle&>(*particle);
                for (const auto& child : choice.Children())
                {
                    for (const auto end : Match(child, position))
                    {
                        AddUnique(result, end);
                    }
                }
                return result;
            }

            case MetadataParticleKind::All:
            {
                const auto& all = static_cast<const MetadataCompositeParticle&>(*particle);
                if (all.Children().size() > 63)
                {
                    return MatchSequence(all.Children(), position);
                }
                Positions result;
                MatchAll(all.Children(), position, 0, result);
                return result;
            }

            case MetadataParticleKind::Sequence:
            case MetadataParticleKind::Group:
                return MatchSequence(static_cast<const MetadataCompositeParticle&>(*particle).Children(), position);
        }
        return {};
    }

    Positions MatchSequence(const std::vector<MetadataParticlePtr>& particles, Size position)
    {
        Positions positions{position};
        for (const auto& child : particles)
        {
            Positions next;
            for (const auto current : positions)
            {
                for (const auto end : Match(child, current))
                {
                    AddUnique(next, end);
                }
            }
            positions = std::move(next);
            if (positions.empty())
            {
                break;
            }
        }
        return positions;
    }

    void MatchAll(const std::vector<MetadataParticlePtr>& particles, Size position, UInt64 used, Positions& result)
    {
        bool allRemainingNullable = true;
        for (Size index = 0; index < particles.size(); ++index)
        {
            if ((used & (UInt64{1} << index)) == 0 && particles[index]->MinOccurs() != 0)
            {
                allRemainingNullable = false;
                break;
            }
        }
        if (allRemainingNullable)
        {
            AddUnique(result, position);
        }

        for (Size index = 0; index < particles.size(); ++index)
        {
            const auto bit = UInt64{1} << index;
            if ((used & bit) != 0)
            {
                continue;
            }
            for (const auto end : Match(particles[index], position))
            {
                if (end != position)
                {
                    MatchAll(particles, end, used | bit, result);
                }
            }
        }
    }

    bool MatchesWildcard(std::string_view wildcard, std::string_view namespaceUri) const
    {
        return ContentModelWildcardMatches(wildcard, namespaceUri, targetNamespace_);
    }

    const std::vector<std::shared_ptr<OpenXMLElement>>& children_;
    std::string_view targetNamespace_;
    OpenXml::FileFormatVersions targetVersion_;
    /** Child positions holding an `mc:AlternateContent`; almost always empty. */
    std::vector<Size> alternatives_;
    std::map<std::pair<const MetadataParticle*, Size>, Positions> matchCache_;
    std::map<Size, std::vector<OpenXmlQualifiedName>> expectedNames_;
    std::set<Size> expectedWildcard_;
    Size furthestPosition_ = 0;
};

OpenXmlContentModelReference::OpenXmlContentModelReference(OpenXml::FileFormatVersions targetVersion) noexcept
    : targetVersion_(targetVersion)
{
}

ContentModelMatch OpenXmlContentModelReference::Match(const MetadataParticlePtr& particle,
                                                      const std::vector<std::shared_ptr<OpenXMLElement>>& children,
                                                      std::string_view targetNamespace) const
{
    OpenXmlContentModelReferenceSession session(children, targetNamespace, targetVersion_);
    return session.Run(particle);
}

} // namespace ExyokiOffice
