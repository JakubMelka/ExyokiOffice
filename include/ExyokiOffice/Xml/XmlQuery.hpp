// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

#include "ExyokiOffice/Export.hpp"
#include "ExyokiOffice/OpenXmlQualifiedName.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ExyokiOffice
{
class OpenXMLElement;
}

namespace ExyokiOffice::Xml
{

/**
 * @brief Extra prefix -> namespace-URI bindings for a dynamic query.
 *
 * These are layered on top of (and take precedence over) the namespaces
 * declared in the queried document and the built-in table of well-known Open
 * XML prefixes. Use them to bind a prefix the document does not declare, or to
 * override an ambiguous one (for example `x` for SpreadsheetML).
 */
struct XmlQueryOptions
{
    std::vector<std::pair<std::string, std::string>> NamespaceBindings;
};

/**
 * @brief Owned, display-safe description of one matched element.
 *
 * Every textual field owns its storage, so a match outlives the query state and
 * can be rendered or serialized freely. @ref Element keeps the live typed handle
 * for callers that want to keep navigating the DOM.
 */
struct EXYOKIOFFICE_EXPORT XmlQueryMatch
{
    /// Best-effort XPath-like location of the element (from OpenXMLElement::GetXmlLocation).
    std::string Location;
    /// Prefixed element name exactly as written in the document (for example "w:p").
    std::string Name;
    /// Resolved namespace URI of the element.
    std::string NamespaceUri;
    /// Local (unprefixed) element name.
    std::string LocalName;
    /// Element attributes as (prefixed-name, value) pairs; xmlns declarations are omitted.
    std::vector<std::pair<std::string, std::string>> Attributes;
    /// Aggregated text of the whole subtree, in document order.
    std::string Text;
    /// Live typed handle to the matched element.
    std::shared_ptr<OpenXMLElement> Element;
};

/**
 * @brief Aggregates all descendant text of an element in document order.
 *
 * This is the generic "inner text" the typed DOM does not otherwise expose: it
 * concatenates every descendant character-data node, regardless of the element
 * types involved. Returns an empty string for a null element.
 */
EXYOKIOFFICE_EXPORT std::string InnerText(const std::shared_ptr<OpenXMLElement>& element);

/**
 * @brief Runs a namespace-precise dynamic XPath query rooted at @p root.
 *
 * The query is XPath 1.0 (evaluated by the vendored pugixml engine). Prefixed
 * name tests such as `w:p` or `@w:val` are resolved to their namespace URIs and
 * rewritten to namespace-aware predicates before evaluation, so a query matches
 * by namespace regardless of which prefix the document happens to use. Prefixes
 * resolve against, in order: @p options bindings, the document's own xmlns
 * declarations, then the built-in table of well-known Open XML prefixes.
 *
 * Only element nodes are returned (attribute/text results of an expression are
 * ignored). On an unresolvable prefix or a malformed expression the function
 * returns an empty vector and, when @p error is non-null, sets it to a
 * diagnostic message; it never throws.
 */
EXYOKIOFFICE_EXPORT std::vector<std::shared_ptr<OpenXMLElement>>
SelectNodes(const std::shared_ptr<OpenXMLElement>& root, std::string_view xpath,
            const XmlQueryOptions& options = {}, std::string* error = nullptr);

/**
 * @brief Builds an owned XmlQueryMatch describing @p element.
 */
EXYOKIOFFICE_EXPORT XmlQueryMatch Describe(const std::shared_ptr<OpenXMLElement>& element);

/**
 * @brief LINQ-style fluent query over a set of DOM elements.
 *
 * Start from a dynamic XPath selection, then refine with typed predicates:
 * @code
 * auto bold = Xml::XmlQuery::Select(body, "//w:r")
 *     .Where([](const OpenXMLElement& r) { return r.GetChild({wNs, "rPr"}) != nullptr; })
 *     .ToList();
 * @endcode
 */
class EXYOKIOFFICE_EXPORT XmlQuery
{
public:
    /// Seeds the query with the result of a dynamic XPath selection.
    static XmlQuery Select(const std::shared_ptr<OpenXMLElement>& root, std::string_view xpath,
                           const XmlQueryOptions& options = {});

    /// Keeps only the elements for which @p predicate returns true.
    XmlQuery& Where(const std::function<bool(const OpenXMLElement&)>& predicate);

    /// Replaces the current set with matching descendants of each current element.
    XmlQuery& SelectDescendants(std::string_view localName,
                                std::optional<std::string_view> namespaceUri = std::nullopt);

    /// Materializes the current elements.
    std::vector<std::shared_ptr<OpenXMLElement>> ToList() const;
    /// Returns the first element, or nullptr when the set is empty.
    std::shared_ptr<OpenXMLElement> FirstOrDefault() const;
    /// Number of elements currently in the set.
    Size Count() const;

private:
    std::vector<std::shared_ptr<OpenXMLElement>> elements_;
};

/// Free helpers for common dynamic queries.
struct EXYOKIOFFICE_EXPORT XmlHelpers
{
    /// Descendant-or-self elements that carry attribute @p attribute equal to @p value.
    static std::vector<std::shared_ptr<OpenXMLElement>>
    FindByAttribute(const std::shared_ptr<OpenXMLElement>& root,
                    const OpenXmlQualifiedName& attribute, std::string_view value);

    /// Every descendant character-data run, in document order, one entry per text node.
    static std::vector<std::string> ExtractAllText(const std::shared_ptr<OpenXMLElement>& root);
};

} // namespace ExyokiOffice::Xml
