// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

#include "pugixml/pugixml.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <string>
#include <string_view>

namespace ExyokiOffice::Xml
{

/**
 * @brief Namespace-aware access to the OPC core-properties part.
 *
 * `docProps/core.xml` mixes three namespaces, and the prefixes Office happens
 * to use for them (`cp:`, `dc:`, `dcterms:`) are a convention, not a rule: any
 * producer may bind the same namespaces to any prefix and the document stays
 * valid and semantically identical. Addressing the elements by their literal
 * prefixed names therefore reads nothing from such a file, and - worse -
 * writing one appends a second `cp:coreProperties` root, which is not
 * well-formed XML.
 *
 * Every accessor here resolves namespace URIs instead. Reads match on
 * (namespace URI, local name); writes reuse whatever prefix the document
 * already binds to the target namespace and only declare the conventional one
 * when the namespace is absent altogether.
 *
 * Properties are addressed by their canonical name (`dc:title`,
 * `cp:keywords`, `dcterms:created`). The prefix in that argument names the
 * namespace and never has to match the document.
 *
 * This helper is shared by the Packaging document-property API and the Tools
 * package inspector, which used to carry separate copies of the same logic.
 */
class CorePropertiesXml
{
public:
    static constexpr std::string_view CoreNamespace =
        "http://schemas.openxmlformats.org/package/2006/metadata/core-properties";
    static constexpr std::string_view DublinCoreNamespace = "http://purl.org/dc/elements/1.1/";
    static constexpr std::string_view DublinCoreTermsNamespace = "http://purl.org/dc/terms/";
    static constexpr std::string_view DublinCoreTypeNamespace = "http://purl.org/dc/dcmitype/";
    static constexpr std::string_view XmlSchemaInstanceNamespace =
        "http://www.w3.org/2001/XMLSchema-instance";

    /// Returns the part after the first colon, or the whole name when unprefixed.
    static std::string_view LocalName(std::string_view qualifiedName);

    /**
     * @brief Splits a canonical property name into its namespace URI and local name.
     *
     * @param canonicalName Name such as `dc:title`, using the conventional prefix.
     * @return False when the prefix is not one of `cp`, `dc`, or `dcterms`.
     */
    static bool ResolveCanonicalName(std::string_view canonicalName,
                                     std::string_view& namespaceUri,
                                     std::string_view& localName);

    /// Returns the namespace URI in effect for an element, or an empty view when unbound.
    static std::string_view NamespaceUriOf(const Pugi::xml_node& node);

    /**
     * @brief Finds the `coreProperties` root element under any prefix.
     * @return The root, or an empty node when the document has none.
     */
    static Pugi::xml_node FindRoot(Pugi::xml_document& document);

    /**
     * @brief Finds the root or creates one carrying the conventional prefixes.
     *
     * An existing root is reused as it stands - its prefix is never rewritten,
     * because the children already reference it.
     */
    static Pugi::xml_node EnsureRoot(Pugi::xml_document& document);

    /// Finds a property element by canonical name; empty node when absent.
    static Pugi::xml_node FindChild(Pugi::xml_node root, std::string_view canonicalName);

    /**
     * @brief Finds or creates a property element.
     *
     * A created element is inserted at its position in the canonical child
     * order of ECMA-376 Part 2 (strict validators reject out-of-order
     * children) and named with the prefix already bound to its namespace.
     *
     * @return The element, or an empty node when @p canonicalName is unknown.
     */
    static Pugi::xml_node EnsureChild(Pugi::xml_node root, std::string_view canonicalName);

    /// Removes a property element when present; returns true when one was removed.
    static bool RemoveChild(Pugi::xml_node root, std::string_view canonicalName);

    /// True for the two date properties, which carry an `xsi:type` marker.
    static bool IsDateProperty(std::string_view canonicalName);

    /**
     * @brief Adds the `xsi:type="dcterms:W3CDTF"` marker to a date element.
     *
     * Both the attribute name and its value are prefixed names, so both are
     * built from the prefixes actually in scope rather than assumed.
     */
    static void EnsureDateTypeAttribute(Pugi::xml_node node);

private:
    /// Position of a local name in the canonical child order; size() when unknown.
    static Size OrderIndex(std::string_view localName);
};

} // namespace ExyokiOffice::Xml
