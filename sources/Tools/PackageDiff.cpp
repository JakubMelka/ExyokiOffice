// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "ExyokiOffice/Tools/PackageDiff.hpp"
#include "ExyokiOffice/Tools/PackageLimits.hpp"

#include "ExyokiOffice/OpenXmlPackage.hpp"
#include "ExyokiOffice/Tools/PackageInspector.hpp"
#include "OpenXmlPackageUri.hpp"
#include "XmlParseOptions.hpp"
#include "pugixml/pugixml.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include "AsciiText.hpp"

#include <algorithm>
#include <map>
#include <optional>

namespace ExyokiOffice::Tools
{

/// File-local normalization helpers behind the package diff.
class PackageDiffHelper
{
public:
    static bool IsWhitespaceOnly(std::string_view text)
    {
        return std::all_of(text.begin(), text.end(),
                           [](char ch)
                           { return AsciiText::IsSpace(ch); });
    }

    static bool PreservesSpace(Pugi::xml_node node)
    {
        const auto attribute = node.attribute("xml:space");
        return attribute && std::string_view(attribute.value()) == "preserve";
    }

    static std::vector<Pugi::xml_node> SignificantChildren(Pugi::xml_node parent)
    {
        const bool preserve = PreservesSpace(parent);
        std::vector<Pugi::xml_node> result;
        for (auto child : parent.children())
        {
            if (child.type() == Pugi::node_element)
            {
                result.push_back(child);
            }
            else if (child.type() == Pugi::node_pcdata || child.type() == Pugi::node_cdata)
            {
                if (preserve || !IsWhitespaceOnly(child.value()))
                {
                    result.push_back(child);
                }
            }
        }
        return result;
    }

    static std::map<std::string, std::string> AttributeMap(Pugi::xml_node node)
    {
        std::map<std::string, std::string> attributes;
        for (auto attribute : node.attributes())
        {
            attributes.emplace(attribute.name(), attribute.value());
        }
        return attributes;
    }

    static std::string SameTagPathSegment(Pugi::xml_node node)
    {
        Size index = 1;
        for (auto sibling = node.previous_sibling(node.name()); sibling; sibling = sibling.previous_sibling(node.name()))
        {
            ++index;
        }
        return std::string(node.name()) + "[" + std::to_string(index) + "]";
    }

    static std::optional<std::string> CompareNodes(Pugi::xml_node a, Pugi::xml_node b, const std::string& path)
    {
        if (std::string_view(a.name()) != std::string_view(b.name()))
        {
            return path;
        }

        if (AttributeMap(a) != AttributeMap(b))
        {
            return path + "/@" + a.name();
        }

        const auto childrenA = SignificantChildren(a);
        const auto childrenB = SignificantChildren(b);
        const auto commonCount = std::min(childrenA.size(), childrenB.size());

        for (Size i = 0; i < commonCount; ++i)
        {
            auto childA = childrenA[i];
            auto childB = childrenB[i];

            if (childA.type() != childB.type())
            {
                return path + "/*";
            }

            if (childA.type() == Pugi::node_pcdata || childA.type() == Pugi::node_cdata)
            {
                const bool preserve = PreservesSpace(a);
                const std::string_view textA = childA.value();
                const std::string_view textB = childB.value();
                const bool equal = preserve ? textA == textB : AsciiText::Trim(textA) == AsciiText::Trim(textB);
                if (!equal)
                {
                    return path + "/text()";
                }
                continue;
            }

            const auto childPath = path + "/" + SameTagPathSegment(childA);
            if (auto diff = CompareNodes(childA, childB, childPath))
            {
                return diff;
            }
        }

        if (childrenA.size() != childrenB.size())
        {
            return path;
        }

        return std::nullopt;
    }

    static bool LoadXmlDocument(Pugi::xml_document& doc, const std::string& xml)
    {
        // load_buffer, not load_string: c_str() ends the document at the first
        // embedded NUL, so two parts differing only after one would compare equal.
        return static_cast<bool>(doc.load_buffer(xml.data(), xml.size(), Xml::ParseOptions::Preserving));
    }

    /// Returns nullopt when the two XML documents are structurally equivalent, otherwise the first difference's path.
    static std::optional<std::string> CompareXmlNormalized(const std::string& xmlA, const std::string& xmlB)
    {
        Pugi::xml_document docA;
        Pugi::xml_document docB;
        const bool loadedA = !xmlA.empty() && LoadXmlDocument(docA, xmlA);
        const bool loadedB = !xmlB.empty() && LoadXmlDocument(docB, xmlB);
        if (!loadedA || !loadedB)
        {
            return xmlA != xmlB ? std::make_optional(std::string("/")) : std::nullopt;
        }

        auto rootA = docA.document_element();
        auto rootB = docB.document_element();
        if (!rootA || !rootB)
        {
            return xmlA != xmlB ? std::make_optional(std::string("/")) : std::nullopt;
        }

        return CompareNodes(rootA, rootB, "/" + SameTagPathSegment(rootA));
    }
};

DiffResult Compare(const std::filesystem::path& left, const std::filesystem::path& right, bool normalizeXml)
{
    DiffResult result;

    OpenXmlPackage leftPackage;
    OpenXmlPackage rightPackage;
    ApplyDefaultPackageLimits(leftPackage);
    ApplyDefaultPackageLimits(rightPackage);
    if (!leftPackage.LoadFromFile(left))
    {
        result.Diagnostics.push_back(ToolDiagnostic{ToolSeverity::Error, "Failed to open package", left.string()});
        return result;
    }
    if (!rightPackage.LoadFromFile(right))
    {
        result.Diagnostics.push_back(ToolDiagnostic{ToolSeverity::Error, "Failed to open package", right.string()});
        return result;
    }

    const auto leftParts = ListParts(leftPackage);
    const auto rightParts = ListParts(rightPackage);

    std::map<std::string, const PartRecord*> leftByUri;
    for (const auto& part : leftParts)
    {
        leftByUri[part.Uri] = &part;
    }
    std::map<std::string, const PartRecord*> rightByUri;
    for (const auto& part : rightParts)
    {
        rightByUri[part.Uri] = &part;
    }

    for (const auto& [uri, leftPart] : leftByUri)
    {
        const auto rightIt = rightByUri.find(uri);
        if (rightIt == rightByUri.end())
        {
            PartDiffEntry entry;
            entry.Uri = uri;
            entry.Kind = PartChangeKind::Removed;
            entry.LeftContentType = leftPart->ContentType;
            result.PartChanges.push_back(std::move(entry));
            continue;
        }

        const auto* rightPart = rightIt->second;
        if (leftPart->ContentType != rightPart->ContentType)
        {
            PartDiffEntry entry;
            entry.Uri = uri;
            entry.Kind = PartChangeKind::ContentTypeChanged;
            entry.LeftContentType = leftPart->ContentType;
            entry.RightContentType = rightPart->ContentType;
            result.PartChanges.push_back(std::move(entry));
            continue;
        }

        auto leftDoc = leftPackage.GetPartByUri(uri);
        auto rightDoc = rightPackage.GetPartByUri(uri);
        if (!leftDoc || !rightDoc)
        {
            continue;
        }

        if (leftPart->Kind == PartPayloadKind::Binary)
        {
            if (leftDoc->GetBinaryData() != rightDoc->GetBinaryData())
            {
                PartDiffEntry entry;
                entry.Uri = uri;
                entry.Kind = PartChangeKind::ChangedBinary;
                entry.LeftContentType = leftPart->ContentType;
                entry.RightContentType = rightPart->ContentType;
                result.PartChanges.push_back(std::move(entry));
            }
            continue;
        }

        const auto leftXml = leftDoc->GetXmlString();
        const auto rightXml = rightDoc->GetXmlString();
        if (!normalizeXml)
        {
            if (leftXml != rightXml)
            {
                PartDiffEntry entry;
                entry.Uri = uri;
                entry.Kind = PartChangeKind::ChangedXml;
                entry.LeftContentType = leftPart->ContentType;
                entry.RightContentType = rightPart->ContentType;
                result.PartChanges.push_back(std::move(entry));
            }
            continue;
        }

        if (auto diffPath = PackageDiffHelper::CompareXmlNormalized(leftXml, rightXml))
        {
            PartDiffEntry entry;
            entry.Uri = uri;
            entry.Kind = PartChangeKind::ChangedXml;
            entry.LeftContentType = leftPart->ContentType;
            entry.RightContentType = rightPart->ContentType;
            entry.FirstDifferencePath = std::move(*diffPath);
            result.PartChanges.push_back(std::move(entry));
        }
    }

    for (const auto& [uri, rightPart] : rightByUri)
    {
        if (leftByUri.find(uri) == leftByUri.end())
        {
            PartDiffEntry entry;
            entry.Uri = uri;
            entry.Kind = PartChangeKind::Added;
            entry.RightContentType = rightPart->ContentType;
            result.PartChanges.push_back(std::move(entry));
        }
    }

    std::sort(result.PartChanges.begin(), result.PartChanges.end(),
              [](const auto& lhs, const auto& rhs)
              { return lhs.Uri < rhs.Uri; });

    auto collectRelationships = [](const OpenXmlPackage& package)
    {
        std::map<std::pair<std::string, std::string>, OpenXmlRelationship> relationships;
        relationships.clear();
        for (auto relationship : package.Relationships())
        {
            relationships.emplace(std::make_pair(std::string("/"), relationship.Id), relationship);
        }
        for (const auto& part : CollectAllParts(package))
        {
            for (const auto& relationship : part->Relationships())
            {
                relationships.emplace(std::make_pair(part->Uri(), relationship.Id), relationship);
            }
        }
        return relationships;
    };

    const auto leftRelationships = collectRelationships(leftPackage);
    const auto rightRelationships = collectRelationships(rightPackage);

    for (const auto& [key, leftRelationship] : leftRelationships)
    {
        const auto rightIt = rightRelationships.find(key);
        if (rightIt == rightRelationships.end())
        {
            result.RelationshipChanges.push_back(
                RelationshipDiffEntry{key.first, key.second, RelationshipChangeKind::Removed, leftRelationship, {}});
            continue;
        }
        const auto& rightRelationship = rightIt->second;
        if (leftRelationship.Type != rightRelationship.Type || leftRelationship.Target != rightRelationship.Target ||
            leftRelationship.TargetMode != rightRelationship.TargetMode ||
            leftRelationship.IsExternal != rightRelationship.IsExternal)
        {
            result.RelationshipChanges.push_back(RelationshipDiffEntry{
                key.first, key.second, RelationshipChangeKind::Changed, leftRelationship, rightRelationship});
        }
    }
    for (const auto& [key, rightRelationship] : rightRelationships)
    {
        if (leftRelationships.find(key) == leftRelationships.end())
        {
            result.RelationshipChanges.push_back(
                RelationshipDiffEntry{key.first, key.second, RelationshipChangeKind::Added, {}, rightRelationship});
        }
    }

    result.Ok = true;
    result.Identical = result.PartChanges.empty() && result.RelationshipChanges.empty();
    return result;
}

} // namespace ExyokiOffice::Tools
