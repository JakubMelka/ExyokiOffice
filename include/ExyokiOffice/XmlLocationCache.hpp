// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

#include "ExyokiOffice/Export.hpp"
#include "ExyokiOffice/OpenXmlQualifiedName.hpp"
#include "ExyokiOffice/StandardTypes.hpp"
#include "ExyokiOffice/ValidationResult.hpp"

#include <memory>

namespace ExyokiOffice
{

class OpenXMLElement;

/**
 * @brief Memoizes the diagnostic locations of the elements of one XML tree.
 *
 * A location carries a positional path (`/w:document/w:body/w:p[3]`), and
 * building one walks every ancestor and counts the same-name siblings at each
 * level - a scan that resolves a namespace prefix per sibling. Repeating that
 * per diagnostic is quadratic in the size of the part, which a document with a
 * few hundred paragraphs already feels.
 *
 * The cache turns it linear. A miss has to scan all children of a parent anyway
 * to learn a position, so it records the path of every one of them, and the
 * ancestors it passes through are recorded on the way back down. A whole tree
 * therefore costs one pass over it, no matter how many locations are asked for.
 *
 * A cache is bound to the tree it was filled from and assumes that tree does not
 * change while it lives; a path recorded before an insertion is not the path
 * after it. That is why validation creates one per run instead of keeping one
 * alive: nothing edits a document in the middle of validating it.
 */
class EXYOKIOFFICE_EXPORT XmlLocationCache
{
public:
    XmlLocationCache();
    ~XmlLocationCache();

    XmlLocationCache(const XmlLocationCache&) = delete;
    XmlLocationCache& operator=(const XmlLocationCache&) = delete;
    XmlLocationCache(XmlLocationCache&&) noexcept;
    XmlLocationCache& operator=(XmlLocationCache&&) noexcept;

    /**
     * @brief Returns the location of @p element, computing it only the first time.
     *
     * Equivalent to OpenXMLElement::GetXmlLocation() in what it produces.
     */
    XmlLocation Location(const OpenXMLElement& element);

    /**
     * @brief Returns the location of @p element pointing at @p attribute.
     *
     * The attribute part is not memoized - naming it reads only the element's own
     * attributes - so only the element's path comes from the cache.
     */
    XmlLocation Location(const OpenXMLElement& element, const OpenXmlQualifiedName& attribute);

    /** Forgets everything recorded so far. Needed after the tree changes. */
    void Clear() noexcept;

    /** How many elements have a recorded location. Exposed so tests can see the batching. */
    [[nodiscard]] Size MemoizedElementCount() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ExyokiOffice
