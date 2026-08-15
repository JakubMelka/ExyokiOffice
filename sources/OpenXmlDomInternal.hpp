// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

#include "ExyokiOffice/OpenXMLElement.hpp"

#include "pugixml/pugixml.hpp"
#include <memory>
#include <type_traits>

namespace ExyokiOffice
{

class OpenXMLElementClass;
class OpenXmlPackagePart;

namespace Detail
{

/** Turns an opaque element node handle into a live pugixml node. */
inline ExyokiOffice::Pugi::xml_node ToNode(OpenXmlNodeHandle handle) noexcept
{
    return ExyokiOffice::Pugi::xml_node(handle.Get());
}

/** Turns a live pugixml node into the opaque handle stored by elements. */
inline OpenXmlNodeHandle ToHandle(const ExyokiOffice::Pugi::xml_node& node) noexcept
{
    return OpenXmlNodeHandle(node.internal_object());
}

/** Returns the node behind an element; an empty node for a null element. */
inline ExyokiOffice::Pugi::xml_node NodeOf(const OpenXMLElement* element) noexcept
{
    return element ? ToNode(element->GetNodeHandle()) : ExyokiOffice::Pugi::xml_node();
}

/** Returns the node behind an element; an empty node for a null element. */
template <typename TElement>
ExyokiOffice::Pugi::xml_node NodeOf(const std::shared_ptr<TElement>& element) noexcept
{
    static_assert(std::is_base_of_v<OpenXMLElement, TElement>, "TElement must derive from OpenXMLElement.");
    return NodeOf(static_cast<const OpenXMLElement*>(element.get()));
}

/**
 * @brief Creates a strongly typed wrapper over an existing node.
 *
 * The type is chosen by the caller; no element-name lookup takes place. Use
 * CreateOpenXmlElementFromNode() when the node's own name must decide the type.
 */
template <typename T>
std::shared_ptr<T> WrapNode(const ExyokiOffice::Pugi::xml_node& node)
{
    if (!node)
    {
        return nullptr;
    }

    auto element = std::make_shared<T>();
    element->SetNodeHandle(ToHandle(node));
    return element;
}

std::shared_ptr<OpenXMLElement> CreateOpenXmlElementFromNode(const ExyokiOffice::Pugi::xml_node& node);
std::shared_ptr<OpenXMLElement> CreateOpenXmlElementFromNode(
    const ExyokiOffice::Pugi::xml_node& node,
    const ExyokiOffice::OpenXMLElementClass* preferredClass);

} // namespace Detail

} // namespace ExyokiOffice
