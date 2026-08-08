// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

#include "ExyokiOffice/OpenXmlQualifiedName.hpp"

#include <string>
#include <vector>

namespace ExyokiOffice
{

class OpenXMLElement;
class OpenXMLElementClass;

/**
 * @internal
 * @brief Resolves a markup compatibility element name to its hand-written class.
 *
 * The generated element factory is a closed lookup table built from the schema
 * metadata, and `mc:` has no schema there to be generated from. The DOM therefore
 * consults this resolver before falling back to GenericOpenXmlElement, so that
 * `mc:AlternateContent` and its branches materialize as typed elements.
 *
 * @return The meta-class for the name, or nullptr when the name is not one of the
 * markup compatibility elements.
 */
const OpenXMLElementClass* ResolveMarkupCompatibilityClass(const OpenXmlQualifiedName& name) noexcept;

/** @internal @brief Reports whether @p value is one of the four XML whitespace characters. */
bool IsXmlWhitespace(char value) noexcept;

/**
 * @internal
 * @brief Collects the namespaces declared ignorable at @p element or above it.
 *
 * `mc:Ignorable` applies to the element that carries it and to its whole
 * subtree, so the answer depends on every ancestor. Each prefix is resolved
 * against the namespace declarations in scope where it appears, because the same
 * prefix may be bound to different namespaces at different depths; a prefix that
 * resolves to nothing is skipped rather than reported, since a reader that only
 * needs to know what it may ignore loses nothing by ignoring less.
 */
std::vector<std::string> CollectIgnorableNamespaces(const OpenXMLElement& element);

} // namespace ExyokiOffice
