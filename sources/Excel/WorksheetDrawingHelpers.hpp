// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

#include "pugixml/pugixml.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace ExyokiOffice::Packaging
{
class WorksheetPart;
}

namespace ExyokiOffice::Excel::Detail
{

/**
 * @brief Resolves the canonical prefix for a namespace URI and ensures it is
 * declared in scope on @p node or one of its ancestors.
 *
 * This wraps the project-wide namespace registry
 * (OpenXml::Features::OpenXmlNamespaceResolver::getPrefixForUrl, sourced from
 * the generated OpenXML namespace table) together with
 * Xml::NamespaceResolver::EnsurePrefix, so hand-written drawing XML reuses the
 * same canonical prefixes (`xdr`, `a`, `r`, `c`, ...) as the typed DOM instead
 * of maintaining a second, ad hoc set of `xmlns:*` literals.
 *
 * @param node Node whose ancestor chain is searched for an existing binding,
 * and which receives the declaration when none is found.
 * @param uri Namespace URI to resolve.
 * @return The prefix now in scope for @p uri.
 */
std::string ResolveNamespace(Pugi::xml_node node, std::string_view uri);

/**
 * @brief Loads a worksheet drawing part's XML into @p document, or creates a
 * fresh `xdr:wsDr` root when the part has no content yet.
 *
 * A freshly created DrawingsPart already serializes a minimal `xdr:wsDr` root
 * that declares only the `xdr` namespace; this function also ensures the `a`
 * and `r` namespaces used by every anchor (picture or chart) are declared on
 * the returned root, so callers never need to repeat that bookkeeping.
 *
 * @param document Document that will own the parsed or newly created tree.
 * @param xml Existing drawing part XML, or an empty string for a new part.
 * @return The `xdr:wsDr` root node, or an empty node when @p xml is non-empty
 * but fails to parse.
 */
Pugi::xml_node LoadOrCreateWorksheetDrawing(Pugi::xml_document& document, const std::string& xml);

/**
 * @brief Serializes @p document without an XML declaration or indentation.
 *
 * Matches the format package parts already store: OpenXmlPackagePart::SetXmlString
 * re-parses the string and OpenXmlPackagePart::GetXmlString adds the
 * declaration back on save, so declaration-free, unindented output keeps part
 * content minimal and consistent with the rest of the hand-written Excel layer.
 */
std::string SerializeRaw(Pugi::xml_document& document);

/**
 * @brief Returns the highest drawing object id (`xdr:cNvPr/@id`) used by any
 * anchor under @p worksheetDrawing.
 *
 * Pictures and charts share one id space so that picture and chart anchors in
 * the same drawing part never collide; this is the shared allocation source
 * for both Worksheet::AddImage and Worksheet::AddChart.
 */
UInt32 MaxDrawingObjectId(Pugi::xml_node worksheetDrawing);

/** @brief True when some anchor under @p worksheetDrawing already uses @p id. */
bool DrawingObjectIdExists(Pugi::xml_node worksheetDrawing, UInt32 id);

/**
 * @brief Adds the worksheet-level `<drawing r:id="...">` link the first time a
 * drawing part is created for a worksheet.
 *
 * @return False when the worksheet part XML cannot be parsed.
 */
bool LinkWorksheetDrawing(const std::shared_ptr<Packaging::WorksheetPart>& worksheetPart,
                          const std::string& relationshipId);

/**
 * @brief Removes the worksheet-level `<drawing>` link.
 *
 * Called once the drawing part's last anchor (picture or chart) is removed,
 * matching the point where the caller also removes the now-empty DrawingsPart.
 */
void UnlinkWorksheetDrawing(const std::shared_ptr<Packaging::WorksheetPart>& worksheetPart);

} // namespace ExyokiOffice::Excel::Detail
