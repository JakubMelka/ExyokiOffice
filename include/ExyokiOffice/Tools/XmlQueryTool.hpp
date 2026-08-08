// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

#include "ExyokiOffice/Export.hpp"
#include "ExyokiOffice/Tools/PackageModel.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ExyokiOffice::Tools
{

/// One element matched by a dynamic XML query.
struct EXYOKIOFFICE_EXPORT QueryMatch
{
    /// Best-effort XPath-like location of the element within its part.
    std::string Location;
    /// Prefixed element name exactly as written in the document (for example "w:p").
    std::string Name;
    /// Element attributes as "name=value" pairs (xmlns declarations excluded).
    std::vector<std::pair<std::string, std::string>> Attributes;
    /// Aggregated text of the element's subtree, in document order.
    std::string Text;
};

struct EXYOKIOFFICE_EXPORT QueryResult
{
    bool Ok = false;
    /// URI of the part that was queried (for example "/word/document.xml").
    std::string PartName;
    std::vector<QueryMatch> Matches;
    std::vector<ToolDiagnostic> Diagnostics;
};

struct EXYOKIOFFICE_EXPORT QueryOptions
{
    /// Part URI to query; empty selects the package's main document part.
    std::string PartName;
    /// Extra prefix -> namespace-URI bindings layered over the well-known/document ones.
    std::vector<std::pair<std::string, std::string>> NamespaceBindings;
    /// Maximum matches to return; 0 means unlimited.
    Size MaxMatches = 0;
};

/**
 * @brief Runs a dynamic XPath query over one XML part of any OPC package.
 *
 * Opens @p packagePath (Word, Excel, or PowerPoint), selects the requested XML
 * part (or the main document part by default), and evaluates @p xpath through
 * the namespace-precise engine in ExyokiOffice::Xml. Prefixed name tests
 * resolve against the supplied bindings, the part's own namespace declarations,
 * and the built-in table of well-known Open XML prefixes.
 *
 * Returns Ok = false with a diagnostic when the package cannot be opened, the
 * requested part is missing or is not XML, or the query is malformed (for
 * example an unbound prefix or invalid XPath). Never throws.
 */
EXYOKIOFFICE_EXPORT QueryResult Query(const std::filesystem::path& packagePath, std::string_view xpath,
                                      const QueryOptions& options = {});

/**
 * @brief Runs the same query over a package that is already open.
 *
 * The overload above is a thin wrapper over this one. Prefer this when you
 * hold the package — a document open in an editor, for instance — because it
 * skips serializing and reparsing the package just to read from it, and
 * because the query then sees the document as it currently stands rather than
 * as it was last written.
 */
EXYOKIOFFICE_EXPORT QueryResult Query(const OpenXmlPackage& package, std::string_view xpath,
                                      const QueryOptions& options = {});

} // namespace ExyokiOffice::Tools
