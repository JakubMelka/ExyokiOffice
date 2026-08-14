// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "CorePropertiesXml.hpp"

#include "XmlNamespaceResolver.hpp"

#include <array>
#include <utility>

namespace ExyokiOffice::Xml
{

/// File-local helpers for core property XML.
class CorePropertiesXmlHelper
{
public:
    /// Canonical child order of `coreProperties` per ECMA-376 Part 2. The elements
    /// are optional, but when present they must appear in this sequence. Local
    /// names are unique across the three namespaces involved, so ordering can be
    /// keyed by local name alone.
    static constexpr std::array<std::string_view, 15> kCorePropertyOrder{
        "category",
        "contentStatus",
        "created",
        "creator",
        "description",
        "identifier",
        "keywords",
        "language",
        "lastModifiedBy",
        "lastPrinted",
        "modified",
        "revision",
        "subject",
        "title",
        "version",
    };

    /// The conventional prefix for each namespace, used only when a document does
    /// not bind the namespace at all and one has to be declared.
    struct CanonicalNamespace
    {
        std::string_view Prefix;
        std::string_view Uri;
    };

    static constexpr std::array<CanonicalNamespace, 3> kCanonicalNamespaces{{
        {"cp", CorePropertiesXml::CoreNamespace},
        {"dc", CorePropertiesXml::DublinCoreNamespace},
        {"dcterms", CorePropertiesXml::DublinCoreTermsNamespace},
    }};

    static std::string_view PrefixOf(std::string_view qualifiedName)
    {
        const auto colon = qualifiedName.find(':');
        return colon == std::string_view::npos ? std::string_view{} : qualifiedName.substr(0, colon);
    }

    /// Composes a qualified name, omitting the colon for the default namespace.
    static std::string QualifiedName(std::string_view prefix, std::string_view localName)
    {
        if (prefix.empty())
        {
            return std::string(localName);
        }
        return std::string(prefix) + ":" + std::string(localName);
    }
};

std::string_view CorePropertiesXml::LocalName(std::string_view qualifiedName)
{
    const auto colon = qualifiedName.find(':');
    return colon == std::string_view::npos ? qualifiedName : qualifiedName.substr(colon + 1);
}

bool CorePropertiesXml::ResolveCanonicalName(std::string_view canonicalName,
                                             std::string_view& namespaceUri,
                                             std::string_view& localName)
{
    const auto prefix = CorePropertiesXmlHelper::PrefixOf(canonicalName);
    for (const auto& entry : CorePropertiesXmlHelper::kCanonicalNamespaces)
    {
        if (entry.Prefix == prefix)
        {
            namespaceUri = entry.Uri;
            localName = LocalName(canonicalName);
            return true;
        }
    }
    return false;
}

std::string_view CorePropertiesXml::NamespaceUriOf(const Pugi::xml_node& node)
{
    const auto uri = NamespaceResolver::LookupUriForPrefix(node, CorePropertiesXmlHelper::PrefixOf(node.name()));
    return uri ? *uri : std::string_view{};
}

Size CorePropertiesXml::OrderIndex(std::string_view localName)
{
    for (Size i = 0; i < CorePropertiesXmlHelper::kCorePropertyOrder.size(); ++i)
    {
        if (CorePropertiesXmlHelper::kCorePropertyOrder[i] == localName)
        {
            return i;
        }
    }
    return CorePropertiesXmlHelper::kCorePropertyOrder.size();
}

Pugi::xml_node CorePropertiesXml::FindRoot(Pugi::xml_document& document)
{
    for (auto child = document.first_child(); child; child = child.next_sibling())
    {
        if (child.type() != Pugi::node_element)
        {
            continue;
        }
        if (LocalName(child.name()) == "coreProperties" && NamespaceUriOf(child) == CoreNamespace)
        {
            return child;
        }
    }
    return {};
}

Pugi::xml_node CorePropertiesXml::EnsureRoot(Pugi::xml_document& document)
{
    if (auto existing = FindRoot(document))
    {
        return existing;
    }

    auto root = document.append_child("cp:coreProperties");
    const auto declare = [&root](const char* attribute, std::string_view value)
    {
        root.append_attribute(attribute) = std::string(value).c_str();
    };
    declare("xmlns:cp", CoreNamespace);
    declare("xmlns:dc", DublinCoreNamespace);
    declare("xmlns:dcterms", DublinCoreTermsNamespace);
    declare("xmlns:dcmitype", DublinCoreTypeNamespace);
    declare("xmlns:xsi", XmlSchemaInstanceNamespace);
    return root;
}

Pugi::xml_node CorePropertiesXml::FindChild(Pugi::xml_node root, std::string_view canonicalName)
{
    std::string_view namespaceUri;
    std::string_view localName;
    if (!root || !ResolveCanonicalName(canonicalName, namespaceUri, localName))
    {
        return {};
    }

    for (auto child = root.first_child(); child; child = child.next_sibling())
    {
        if (child.type() == Pugi::node_element && LocalName(child.name()) == localName &&
            NamespaceUriOf(child) == namespaceUri)
        {
            return child;
        }
    }
    return {};
}

Pugi::xml_node CorePropertiesXml::EnsureChild(Pugi::xml_node root, std::string_view canonicalName)
{
    std::string_view namespaceUri;
    std::string_view localName;
    if (!root || !ResolveCanonicalName(canonicalName, namespaceUri, localName))
    {
        return {};
    }

    if (auto existing = FindChild(root, canonicalName))
    {
        return existing;
    }

    const auto prefix = NamespaceResolver::EnsurePrefix(root, namespaceUri, CorePropertiesXmlHelper::PrefixOf(canonicalName));
    const auto name = CorePropertiesXmlHelper::QualifiedName(prefix, localName);

    const auto index = OrderIndex(localName);
    for (auto child = root.first_child(); child; child = child.next_sibling())
    {
        if (child.type() == Pugi::node_element && OrderIndex(LocalName(child.name())) > index)
        {
            return root.insert_child_before(name.c_str(), child);
        }
    }
    return root.append_child(name.c_str());
}

bool CorePropertiesXml::RemoveChild(Pugi::xml_node root, std::string_view canonicalName)
{
    if (auto node = FindChild(root, canonicalName))
    {
        return root.remove_child(node);
    }
    return false;
}

bool CorePropertiesXml::IsDateProperty(std::string_view canonicalName)
{
    const auto localName = LocalName(canonicalName);
    return localName == "created" || localName == "modified";
}

void CorePropertiesXml::EnsureDateTypeAttribute(Pugi::xml_node node)
{
    if (!node)
    {
        return;
    }

    for (const auto& attribute : node.attributes())
    {
        if (LocalName(attribute.name()) == "type" &&
            NamespaceResolver::LookupUriForPrefix(node, CorePropertiesXmlHelper::PrefixOf(attribute.name())) ==
                XmlSchemaInstanceNamespace)
        {
            return;
        }
    }

    const auto schemaPrefix = NamespaceResolver::EnsurePrefix(node, XmlSchemaInstanceNamespace, "xsi");
    const auto termsPrefix = NamespaceResolver::EnsurePrefix(node, DublinCoreTermsNamespace, "dcterms");
    node.append_attribute(CorePropertiesXmlHelper::QualifiedName(schemaPrefix, "type").c_str()) =
        CorePropertiesXmlHelper::QualifiedName(termsPrefix, "W3CDTF").c_str();
}

} // namespace ExyokiOffice::Xml
