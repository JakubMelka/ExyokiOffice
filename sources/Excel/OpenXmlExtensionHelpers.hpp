// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

#include "ExyokiOffice/OpenXMLElement.hpp"
#include "ExyokiOffice/OpenXmlSimpleTypes.hpp"
#include "AsciiText.hpp"

#include <algorithm>
#include <memory>
#include <string>
#include <string_view>

namespace ExyokiOffice::Excel::Detail
{

/**
 * @brief Well-known OOXML extension URIs used by the hand-written Excel editors.
 *
 * Every `x:ext` inside an extension list is identified by a GUID URI that the
 * consuming application matches literally. An application silently ignores an
 * extension whose URI it does not know, so a typo here produces a file that
 * opens without complaint and simply lacks the feature. The values are
 * therefore defined exactly once and asserted by the unit tests.
 */
struct ExtensionUris
{
    /** @brief `x14:slicerCaches` inside `x:workbook/x:extLst`. */
    static constexpr std::string_view WorkbookSlicerCaches = "{BBE1A952-AA13-448e-AADC-164F8A28A991}";
    /** @brief `x15:slicerCaches` inside `x:workbook/x:extLst`, used by table slicers. */
    static constexpr std::string_view WorkbookSlicerCachesX15 = "{46BE6895-7355-4a93-B00E-2C351335B9C9}";
    /** @brief `x14:slicerList` inside `x:worksheet/x:extLst`. */
    static constexpr std::string_view WorksheetSlicerList = "{A8765BA9-456A-4dab-B4F3-ACF1056F45CF}";
    /** @brief `x15:tableSlicerCache` inside `x14:slicerCacheDefinition/x14:extLst`. */
    static constexpr std::string_view TableSlicerCache = "{2F2917AC-EB37-4324-AD4E-5DD8C200BD13}";
};

/**
 * @brief Compares two extension URIs the way consuming applications do.
 *
 * Excel emits these GUIDs with inconsistent letter case (`{BBE1A952-AA13-448e-...}`
 * mixes both), so a case-sensitive comparison against a hand-typed constant
 * would fail to recognize an extension written by Excel itself.
 */
inline bool ExtensionUriEquals(std::string_view left, std::string_view right)
{
    return AsciiText::EqualsIgnoreCase(left, right);
}

/**
 * @brief Finds the `x:ext` carrying @p uri inside @p owner's extension list.
 *
 * @tparam TList Generated extension list class, for example WorkbookExtensionList.
 * @tparam TExtension Generated extension class, for example WorkbookExtension.
 * @param owner Element that may own the extension list.
 * @param uri Well-known extension URI, braces included.
 * @return The extension, or nullptr when the list or the extension is absent.
 */
template <typename TList, typename TExtension>
std::shared_ptr<TExtension> FindExtension(const std::shared_ptr<OpenXMLElement>& owner, std::string_view uri)
{
    const auto list = owner ? owner->GetFirstChildOfType<TList>() : nullptr;
    if (!list)
    {
        return nullptr;
    }
    for (const auto& extension : list->template Elements<TExtension>())
    {
        if (extension && ExtensionUriEquals(extension->GetUri().ToString(), uri))
        {
            return extension;
        }
    }
    return nullptr;
}

/**
 * @brief Finds or creates the `x:ext` carrying @p uri inside @p owner.
 *
 * OOXML extension lists hold one `x:ext` per feature, keyed by a well-known
 * GUID URI, and each `x:ext` admits exactly one feature child. This implements
 * the get-or-create half of that contract for any of the generated extension
 * list and extension class pairs, of which SpreadsheetML has several distinct
 * ones (workbook, worksheet, slicer cache definition).
 *
 * @tparam TList Generated extension list class.
 * @tparam TExtension Generated extension class.
 * @param owner Element that owns, or should own, the extension list.
 * @param uri Well-known extension URI, braces included.
 * @param create Creates the list and the extension when they are missing.
 * @return The extension, nullptr when it is absent and @p create is false, or
 * nullptr when the content model refused an insertion.
 */
template <typename TList, typename TExtension>
std::shared_ptr<TExtension> GetOrCreateExtension(const std::shared_ptr<OpenXMLElement>& owner,
                                                 std::string_view uri,
                                                 bool create)
{
    if (!owner)
    {
        return nullptr;
    }
    if (auto existing = FindExtension<TList, TExtension>(owner, uri))
    {
        return existing;
    }
    if (!create)
    {
        return nullptr;
    }
    auto list = owner->GetFirstChildOfType<TList>();
    if (!list)
    {
        list = owner->AppendChild<TList>();
    }
    if (!list)
    {
        return nullptr;
    }
    const auto extension = list->template AppendChild<TExtension>();
    if (!extension)
    {
        return nullptr;
    }
    extension->SetUri(StringValue(uri));
    return extension;
}

/**
 * @brief Finds or creates the single feature child of an extension.
 *
 * The feature child is what actually carries the extension payload, for
 * example `x14:slicerCaches`. A null result means the extension already holds
 * a different feature child, because the generated particles model the child
 * as a choice with a maximum of one occurrence.
 *
 * @tparam TFeature Generated feature element class.
 * @param extension Extension returned by @ref GetOrCreateExtension.
 * @param create Creates the feature child when it is missing.
 */
template <typename TFeature, typename TExtension>
std::shared_ptr<TFeature> GetOrCreateExtensionFeature(const std::shared_ptr<TExtension>& extension, bool create)
{
    if (!extension)
    {
        return nullptr;
    }
    auto feature = extension->template GetFirstChildOfType<TFeature>();
    if (!feature && create)
    {
        feature = extension->template AppendChild<TFeature>();
    }
    return feature;
}

/**
 * @brief Removes the `x:ext` carrying @p uri and, when it was the last one,
 * the extension list itself.
 *
 * Leaving an empty `x:extLst` behind is legal but is not what Excel writes, and
 * an empty list makes round-trip comparisons noisy.
 *
 * @return True when the extension existed and was removed.
 */
template <typename TList, typename TExtension>
bool RemoveExtension(const std::shared_ptr<OpenXMLElement>& owner, std::string_view uri)
{
    const auto list = owner ? owner->GetFirstChildOfType<TList>() : nullptr;
    const auto extension = list ? FindExtension<TList, TExtension>(owner, uri) : nullptr;
    if (!extension || !list->RemoveChild(extension))
    {
        return false;
    }
    if (list->template Elements<TExtension>().empty())
    {
        owner->RemoveChild(list);
    }
    return true;
}

} // namespace ExyokiOffice::Excel::Detail
