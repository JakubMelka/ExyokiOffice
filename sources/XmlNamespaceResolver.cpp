// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "XmlNamespaceResolver.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <set>
#include <sstream>

namespace ExyokiOffice::Xml
{

/// File-local helpers for namespace prefix resolution.
class XmlNamespaceResolverHelper
{
public:
    static bool IsNamespaceDeclaration(const Pugi::xml_attribute& attr, std::string_view& outPrefix)
    {
        const std::string_view name(attr.name());
        if (name == "xmlns")
        {
            outPrefix = std::string_view{};
            return true;
        }

        constexpr std::string_view kXmlnsPrefix = "xmlns:";
        if (name.size() > kXmlnsPrefix.size() && name.substr(0, kXmlnsPrefix.size()) == kXmlnsPrefix)
        {
            outPrefix = name.substr(kXmlnsPrefix.size());
            return true;
        }

        return false;
    }
};

bool NamespaceResolver::IsDefaultXmlPrefix(std::string_view prefix, std::string_view uri) noexcept
{
    constexpr std::string_view kXmlNamespace = "http://www.w3.org/XML/1998/namespace";
    if (prefix == "xml")
    {
        return uri.empty() || uri == kXmlNamespace;
    }
    return false;
}

std::optional<std::string_view> NamespaceResolver::LookupPrefixForUri(const ExyokiOffice::Pugi::xml_node& node,
                                                                      std::string_view uri)
{
    if (uri.empty())
    {
        return std::string_view{};
    }

    if (IsDefaultXmlPrefix("xml", uri))
    {
        return std::string_view{"xml"};
    }

    for (auto current = node; current; current = current.parent())
    {
        for (const auto& attr : current.attributes())
        {
            std::string_view prefix;
            if (!XmlNamespaceResolverHelper::IsNamespaceDeclaration(attr, prefix))
            {
                continue;
            }

            if (uri == std::string_view(attr.value()))
            {
                return std::string_view(prefix);
            }
        }
    }

    return std::nullopt;
}

std::optional<std::string_view> NamespaceResolver::LookupUriForPrefix(const ExyokiOffice::Pugi::xml_node& node,
                                                                      std::string_view prefix)
{
    if (IsDefaultXmlPrefix(prefix, {}))
    {
        return std::string_view{"http://www.w3.org/XML/1998/namespace"};
    }

    for (auto current = node; current; current = current.parent())
    {
        for (const auto& attr : current.attributes())
        {
            std::string_view declaredPrefix;
            if (!XmlNamespaceResolverHelper::IsNamespaceDeclaration(attr, declaredPrefix))
            {
                continue;
            }

            if (declaredPrefix == prefix)
            {
                return std::string_view(attr.value());
            }
        }
    }

    return std::nullopt;
}

bool NamespaceResolver::IsPrefixInScope(const ExyokiOffice::Pugi::xml_node& node, std::string_view prefix)
{
    return LookupUriForPrefix(node, prefix).has_value();
}

std::string NamespaceResolver::GenerateUniquePrefix(const ExyokiOffice::Pugi::xml_node& node,
                                                    std::string_view base)
{
    std::string candidateBase = base.empty() ? "ns" : std::string(base);
    for (Size index = 0;; ++index)
    {
        std::ostringstream name;
        name << candidateBase;
        if (index != 0)
        {
            name << index;
        }

        const auto generated = name.str();
        if (!IsPrefixInScope(node, generated))
        {
            return generated;
        }
    }
}

std::string NamespaceResolver::EnsurePrefix(ExyokiOffice::Pugi::xml_node& node,
                                            std::string_view uri,
                                            std::optional<std::string_view> suggestedPrefix)
{
    if (uri.empty())
    {
        return {};
    }

    if (IsDefaultXmlPrefix("xml", uri))
    {
        return "xml";
    }

    if (auto existing = LookupPrefixForUri(node, uri))
    {
        return std::string(*existing);
    }

    ExyokiOffice::Pugi::xml_node target = node;
    for (auto parent = target.parent(); parent && parent.type() == Pugi::node_element; parent = target.parent())
    {
        target = parent;
    }

    std::string prefix;
    if (suggestedPrefix.has_value() && !suggestedPrefix->empty())
    {
        prefix = *suggestedPrefix;
        if (IsPrefixInScope(node, prefix))
        {
            prefix = GenerateUniquePrefix(node, prefix);
        }
    }
    else
    {
        prefix = GenerateUniquePrefix(node, "ns");
    }

    std::string attrName = "xmlns:";
    attrName.append(prefix);
    Pugi::xml_attribute decl = target.append_attribute(attrName.c_str());
    decl.set_value(std::string(uri).c_str());

    return prefix;
}

} // namespace ExyokiOffice::Xml
