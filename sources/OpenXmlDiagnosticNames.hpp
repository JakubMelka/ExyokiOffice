// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

#include "ExyokiOffice/DOM/Namespaces.hpp"
#include "ExyokiOffice/OpenXmlQualifiedName.hpp"

#include <string>

namespace ExyokiOffice::Detail
{

/**
 * @brief Formats a qualified name for a diagnostic message.
 *
 * Known Open XML namespaces are rendered with their canonical prefix, so a
 * message reads `w:val` rather than repeating the whole namespace URI. An unknown
 * namespace falls back to the URI, which is all there is to identify it by.
 */
inline std::string DescribeQualifiedName(const OpenXmlQualifiedName& name)
{
    if (name.namespaceUri().empty())
    {
        return std::string(name.localName());
    }

    std::string_view qualifier = name.namespaceUri();
    if (const auto prefix = OpenXml::Features::OpenXmlNamespaceResolver::getPrefixForUrl(name.namespaceUri()))
    {
        qualifier = *prefix;
    }

    std::string result;
    result.reserve(qualifier.size() + 1 + name.localName().size());
    result.append(qualifier);
    result.push_back(':');
    result.append(name.localName());
    return result;
}

} // namespace ExyokiOffice::Detail
