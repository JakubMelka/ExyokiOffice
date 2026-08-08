// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

#include "pugixml/pugixml.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ExyokiOffice::Xml
{

class NamespaceResolver
{
public:
    static std::optional<std::string_view> LookupPrefixForUri(const ExyokiOffice::Pugi::xml_node& node,
                                                              std::string_view uri);

    static std::optional<std::string_view> LookupUriForPrefix(const ExyokiOffice::Pugi::xml_node& node,
                                                              std::string_view prefix);

    static std::string EnsurePrefix(Pugi::xml_node& node,
                                    std::string_view uri,
                                    std::optional<std::string_view> suggestedPrefix = std::nullopt);

private:
    static bool IsDefaultXmlPrefix(std::string_view prefix, std::string_view uri) noexcept;

    static std::string GenerateUniquePrefix(const ExyokiOffice::Pugi::xml_node& node,
                                            std::string_view base);

    static bool IsPrefixInScope(const ExyokiOffice::Pugi::xml_node& node, std::string_view prefix);
};

} // namespace ExyokiOffice::Xml
