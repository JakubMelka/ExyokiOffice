// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

#include "ExyokiOffice/Export.hpp"
#include "FileFormatVersions.h"

#include <string_view>

namespace ExyokiOffice
{

/**
 * @brief Non-owning Open XML qualified name.
 *
 * OpenXmlQualifiedName stores the namespace URI and local name as
 * `std::string_view` values. The object does not allocate or copy either
 * string; callers are responsible for ensuring that both character ranges
 * remain valid for at least as long as the OpenXmlQualifiedName object, and
 * for as long as any object that stores a copy of it.
 *
 * This type is intended primarily for schema metadata and other static
 * descriptors, where the namespace URI and local name are normally string
 * literals or other static-storage strings. Passing a temporary `std::string`,
 * a view into a short-lived buffer, or a substring of a string that may be
 * destroyed or reallocated will leave the qualified name dangling.
 */
class EXYOKIOFFICE_EXPORT OpenXmlQualifiedName
{
public:
    /**
     * @brief Creates an empty qualified name.
     */
    constexpr OpenXmlQualifiedName() noexcept = default;

    /**
     * @brief Creates a qualified name from non-owning namespace and local-name views.
     *
     * @param nsUri Namespace URI. The referenced characters must outlive this object.
     * @param localName Local XML name. The referenced characters must outlive this object.
     *
     * No string data is copied. Prefer string literals or other static-storage strings
     * when constructing metadata. Use caution with `std::string`, because modifying or
     * destroying the source string invalidates the stored views.
     */
    constexpr OpenXmlQualifiedName(std::string_view nsUri, std::string_view localName) noexcept
        : m_namespaceUri(nsUri), m_localName(localName)
    {
    }

    /**
     * @brief Returns the namespace URI view.
     *
     * The returned view has the same lifetime constraints as the value passed to
     * the constructor.
     */
    std::string_view namespaceUri() const noexcept;

    /**
     * @brief Returns the local-name view.
     *
     * The returned view has the same lifetime constraints as the value passed to
     * the constructor.
     */
    std::string_view localName() const noexcept;

    bool operator==(const OpenXmlQualifiedName& other) const noexcept;
    bool operator!=(const OpenXmlQualifiedName& other) const noexcept;
    bool operator<(const OpenXmlQualifiedName& other) const noexcept;

private:
    std::string_view m_namespaceUri;
    std::string_view m_localName;
};

/**
 * @brief Non-owning schema metadata for an Open XML attribute.
 *
 * OpenXmlAttribute is a lightweight descriptor used by generated and
 * hand-written metadata. Its textual fields are `std::string_view` values and
 * are not owned by the descriptor. The qualified name, type name, and comment
 * must therefore point to strings that remain valid for the lifetime of this
 * descriptor and for the lifetime of any metadata table that stores it.
 *
 * In normal schema metadata these values should be string literals or other
 * static-storage strings. Do not initialize this type from temporary
 * `std::string` objects or short-lived buffers.
 */
struct OpenXmlAttribute
{
    /**
     * @brief Creates an empty attribute descriptor.
     */
    constexpr OpenXmlAttribute() noexcept = default;

    /**
     * @brief Creates an attribute descriptor from non-owning metadata views.
     *
     * @param qName Qualified attribute name. Its stored views must outlive this descriptor.
     * @param typeName Attribute value type name. The referenced characters must outlive this descriptor.
     * @param version First Office file format version that supports this attribute.
     * @param comment Documentation comment. The referenced characters must outlive this descriptor.
     */
    constexpr OpenXmlAttribute(OpenXmlQualifiedName qName,
                               std::string_view typeName,
                               OpenXml::FileFormatVersions version,
                               std::string_view comment) noexcept
        : Name(qName), TypeName(typeName), Version(version), Comment(comment)
    {
    }

    /// Qualified attribute name. The nested string views are non-owning.
    OpenXmlQualifiedName Name;
    /// Attribute value type name. Non-owning; must refer to long-lived storage.
    std::string_view TypeName;
    /// First Office file format version that supports this attribute.
    OpenXml::FileFormatVersions Version{OpenXml::FileFormatVersions::Office2007};
    /// Attribute documentation text. Non-owning; must refer to long-lived storage.
    std::string_view Comment;
};

} // namespace ExyokiOffice
