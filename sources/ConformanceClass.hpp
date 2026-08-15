// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

#include <string_view>

namespace ExyokiOffice
{

/**
 * @brief The two conformance classes of ECMA-376 / ISO 29500.
 *
 * Transitional is what Office writes by default: it describes the format
 * including the constructs inherited from the legacy binary formats. Strict is
 * the ISO-cleaned class, and it spells every namespace and relationship type
 * under `purl.oclc.org` instead of `schemas.openxmlformats.org`. The two are
 * therefore disjoint to a parser - a name from one class never matches the
 * other.
 *
 * ExyokiOffice implements Transitional only; the generated DOM is built from
 * the Transitional schemas. Strict is recognized here so the library can say
 * so instead of behaving as if the document were unreadable for an unknown
 * reason. See docs/Compatibility.md.
 */
class ConformanceClass
{
public:
    /// Root relationship naming the main document part, Transitional spelling.
    static constexpr std::string_view TransitionalOfficeDocumentRelationship =
        "http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument";

    /// The same relationship as spelled by ISO 29500 Strict packages.
    static constexpr std::string_view StrictOfficeDocumentRelationship =
        "http://purl.oclc.org/ooxml/officeDocument/relationships/officeDocument";

    /// Namespace prefix every Strict markup namespace and relationship shares.
    static constexpr std::string_view StrictNamespacePrefix = "http://purl.oclc.org/ooxml/";

    [[nodiscard]] static constexpr bool IsTransitionalOfficeDocument(std::string_view relationshipType) noexcept
    {
        return relationshipType == TransitionalOfficeDocumentRelationship;
    }

    [[nodiscard]] static constexpr bool IsStrictOfficeDocument(std::string_view relationshipType) noexcept
    {
        return relationshipType == StrictOfficeDocumentRelationship;
    }
};

} // namespace ExyokiOffice
