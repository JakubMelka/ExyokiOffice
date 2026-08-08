// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "RelationshipTransform.hpp"

#include "SignatureNames.hpp"
#include "XmlCanonicalization.hpp"

#include <algorithm>
#include <string_view>

namespace ExyokiOffice::Security
{

/// Element and attribute names of the relationship part vocabulary.
class RelationshipTransformNames final
{
public:
    RelationshipTransformNames() = delete;

    static constexpr const char* RelationshipsNamespace =
        "http://schemas.openxmlformats.org/package/2006/relationships";
    static constexpr const char* RelationshipsElement = "Relationships";
    static constexpr const char* RelationshipElement = "Relationship";
    static constexpr const char* ReferenceElement = "RelationshipReference";
    static constexpr const char* GroupReferenceElement = "RelationshipsGroupReference";
    static constexpr const char* SourceIdAttribute = "SourceId";
    static constexpr const char* SourceTypeAttribute = "SourceType";
    static constexpr const char* InternalTargetMode = "Internal";

    /// Returns the part of a qualified name after the prefix.
    static std::string_view LocalNameOf(std::string_view qualifiedName) noexcept
    {
        const auto colon = qualifiedName.find(':');
        return colon == std::string_view::npos ? qualifiedName : qualifiedName.substr(colon + 1);
    }
};

RelationshipSelection RelationshipTransform::ReadSelection(const Pugi::xml_node& transformNode)
{
    RelationshipSelection selection;
    if (!transformNode)
    {
        return selection;
    }

    for (const auto& child : transformNode.children())
    {
        if (child.type() != Pugi::node_element)
        {
            continue;
        }

        const auto localName = RelationshipTransformNames::LocalNameOf(child.name());
        if (localName == RelationshipTransformNames::ReferenceElement)
        {
            if (const auto attribute = child.attribute(RelationshipTransformNames::SourceIdAttribute))
            {
                selection.SourceIds.emplace_back(attribute.value());
            }
        }
        else if (localName == RelationshipTransformNames::GroupReferenceElement)
        {
            if (const auto attribute = child.attribute(RelationshipTransformNames::SourceTypeAttribute))
            {
                selection.SourceTypes.emplace_back(attribute.value());
            }
        }
    }

    return selection;
}

std::optional<std::string> RelationshipTransform::Apply(const OpenXmlPartContainer& container,
                                                        const RelationshipSelection& selection)
{
    return Apply(container.Relationships(), selection);
}

std::optional<std::string> RelationshipTransform::Apply(const std::vector<OpenXmlRelationship>& relationships,
                                                        const RelationshipSelection& selection)
{
    // An empty selection means "every relationship of this part".
    const bool selectAll = selection.SourceIds.empty() && selection.SourceTypes.empty();

    std::vector<const OpenXmlRelationship*> selected;
    selected.reserve(relationships.size());
    for (const auto& relationship : relationships)
    {
        const bool matchesId = std::find(selection.SourceIds.begin(), selection.SourceIds.end(), relationship.Id) !=
                               selection.SourceIds.end();
        const bool matchesType =
            std::find(selection.SourceTypes.begin(), selection.SourceTypes.end(), relationship.Type) !=
            selection.SourceTypes.end();
        if (selectAll || matchesId || matchesType)
        {
            selected.push_back(&relationship);
        }
    }

    // The transform sorts by identifier so that the digest does not depend on
    // the order in which relationships happen to be stored.
    std::sort(selected.begin(), selected.end(), [](const auto* left, const auto* right)
              { return left->Id < right->Id; });

    Pugi::xml_document document;
    auto root = document.append_child(RelationshipTransformNames::RelationshipsElement);
    root.append_attribute("xmlns") = RelationshipTransformNames::RelationshipsNamespace;

    for (const auto* relationship : selected)
    {
        auto node = root.append_child(RelationshipTransformNames::RelationshipElement);
        node.append_attribute("Id") = relationship->Id.c_str();
        node.append_attribute("Type") = relationship->Type.c_str();
        node.append_attribute("Target") = relationship->Target.c_str();
        // TargetMode is optional in storage but explicit in the transform output.
        node.append_attribute("TargetMode") = relationship->TargetMode.empty()
                                                  ? RelationshipTransformNames::InternalTargetMode
                                                  : relationship->TargetMode.c_str();
    }

    return XmlCanonicalization::CanonicalizeSubtree(root, CanonicalizationMethod::InclusiveC14N);
}

} // namespace ExyokiOffice::Security
