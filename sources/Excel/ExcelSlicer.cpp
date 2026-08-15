// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "ExyokiOffice/Excel/ExcelDocument.hpp"

#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/Drawing.hpp"
#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/Drawing/Spreadsheet.hpp"
#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/Office2010/Drawing/Slicer.hpp"
#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/Office2010/Excel.hpp"
#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/Office2013/Excel.hpp"
#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/Spreadsheet.hpp"
#include "ExyokiOffice/Packaging/GeneratedParts.hpp"
#include "Excel/ExcelSlicerInternal.hpp"
#include "Excel/OpenXmlExtensionHelpers.hpp"
#include "Excel/WorksheetDrawingHelpers.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include "AsciiText.hpp"

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ExyokiOffice::Excel
{
namespace SlicerDetail
{

namespace S = ExyokiOffice::DocumentFormat::OpenXml::Spreadsheet;
namespace X14 = ExyokiOffice::DocumentFormat::OpenXml::Office2010::Excel;
namespace X15 = ExyokiOffice::DocumentFormat::OpenXml::Office2013::Excel;
namespace A = ExyokiOffice::DocumentFormat::OpenXml::Drawing;
namespace XDR = ExyokiOffice::DocumentFormat::OpenXml::Drawing::Spreadsheet;
namespace SLE = ExyokiOffice::DocumentFormat::OpenXml::Office2010::Drawing::Slicer;

/** Graphic data URI that marks a drawing frame as a slicer shape. */
constexpr std::string_view kSlicerGraphicUri = "http://schemas.microsoft.com/office/drawing/2010/slicer";
/** Prefix Excel uses when it derives a slicer cache name from a source column. */
constexpr std::string_view kCacheNamePrefix = "Slicer_";
/** Upper bound Excel accepts for the slicer button column count. */
constexpr UInt32 kMaxColumnCount = 20000;
/** Upper bound shared by all Excel object names. */
constexpr Size kMaxNameLength = 255;
/** Guard for the unique-name probe loops. */
constexpr UInt32 kMaxNameProbe = 100000;

// ---------------------------------------------------------------------------
// Small text helpers
// ---------------------------------------------------------------------------

SlicerResult Failure(SlicerError error, std::string message)
{
    return SlicerResult{error, std::move(message)};
}

SlicerResult Success()
{
    return SlicerResult{};
}

/**
 * Derives the token Excel uses inside a slicer cache name.
 *
 * The cache name is exposed in the spreadsheet application's object model as a
 * defined-name-like identifier, so the punctuation and spaces that a defined
 * name cannot hold collapse to an underscore and a leading digit is pushed
 * behind one. Letters outside ASCII are kept: a defined name may be written in
 * any script, and replacing those bytes would both mangle the name and leave a
 * half-decoded character behind.
 */
std::string SanitizeCacheToken(std::string_view sourceName)
{
    std::string token;
    token.reserve(sourceName.size());
    for (const char character : sourceName)
    {
        const bool keep = AsciiText::IsAlnum(character) || AsciiText::IsNonAscii(character) || character == '_';
        token.push_back(keep ? character : '_');
    }
    if (token.empty())
    {
        token = "Field";
    }
    if (AsciiText::IsDigit(token.front()))
    {
        token.insert(token.begin(), '_');
    }
    return token;
}

// ---------------------------------------------------------------------------
// Enum mapping between the public POD enums and the generated x14 enums
// ---------------------------------------------------------------------------

EnumValue<X14::TabularSlicerCacheSortOrderValues> ToSortOrder(SlicerSortOrder order)
{
    using Values = X14::TabularSlicerCacheSortOrderValues;
    return EnumValue<Values>(order == SlicerSortOrder::Descending ? Values::Descending : Values::Ascending);
}

SlicerSortOrder FromSortOrder(const EnumValue<X14::TabularSlicerCacheSortOrderValues>& value)
{
    using Values = X14::TabularSlicerCacheSortOrderValues;
    return value.ValueOr(Values::Ascending) == Values::Descending ? SlicerSortOrder::Descending
                                                                  : SlicerSortOrder::Ascending;
}

EnumValue<X14::SlicerCacheCrossFilterValues> ToCrossFilter(SlicerCrossFilter crossFilter)
{
    using Values = X14::SlicerCacheCrossFilterValues;
    switch (crossFilter)
    {
        case SlicerCrossFilter::None:
            return EnumValue<Values>(Values::None);
        case SlicerCrossFilter::ShowItemsWithNoData:
            return EnumValue<Values>(Values::ShowItemsWithNoData);
        case SlicerCrossFilter::ShowItemsWithDataAtTop:
            break;
    }
    return EnumValue<Values>(Values::ShowItemsWithDataAtTop);
}

SlicerCrossFilter FromCrossFilter(const EnumValue<X14::SlicerCacheCrossFilterValues>& value)
{
    using Values = X14::SlicerCacheCrossFilterValues;
    switch (value.ValueOr(Values::ShowItemsWithDataAtTop))
    {
        case Values::None:
            return SlicerCrossFilter::None;
        case Values::ShowItemsWithNoData:
            return SlicerCrossFilter::ShowItemsWithNoData;
        default:
            break;
    }
    return SlicerCrossFilter::ShowItemsWithDataAtTop;
}

// ---------------------------------------------------------------------------
// Package and DOM navigation
// ---------------------------------------------------------------------------

/** Returns the relationship identifier that links @p source to @p target. */
std::string RelationshipIdBetween(const OpenXmlPackagePart& source, const OpenXmlPackagePart& target)
{
    for (const auto& incoming : target.IncomingRelationships())
    {
        if (incoming.SourceUri == source.Uri())
        {
            return incoming.Id;
        }
    }
    return {};
}

std::vector<std::shared_ptr<Packaging::SlicersPart>> AllSlicersParts(const ExcelDocument::Ptr& document)
{
    std::vector<std::shared_ptr<Packaging::SlicersPart>> result;
    const auto workbookPart = document ? document->GetWorkbookPart() : nullptr;
    if (!workbookPart)
    {
        return result;
    }
    for (const auto& worksheetPart : workbookPart->GetWorksheetParts())
    {
        if (!worksheetPart)
        {
            continue;
        }
        for (const auto& part : worksheetPart->GetSlicersParts())
        {
            result.push_back(part);
        }
    }
    return result;
}

/**
 * Returns the slicer elements of one part.
 *
 * `Elements<Slicer>()` resolves the `{x14, slicer}` element name through the
 * parent's content model. The generic `Children()` must never be used here,
 * because the same qualified name also denotes `SlicerRef` inside a slicer list.
 */
std::vector<std::shared_ptr<X14::Slicer>> SlicerElements(const std::shared_ptr<Packaging::SlicersPart>& part)
{
    const auto root = part ? part->GetSlicers() : nullptr;
    return root ? root->Elements<X14::Slicer>() : std::vector<std::shared_ptr<X14::Slicer>>{};
}

std::shared_ptr<X14::Slicer> FindSlicerElement(const std::shared_ptr<Packaging::SlicersPart>& part,
                                               std::string_view name)
{
    for (const auto& slicer : SlicerElements(part))
    {
        if (slicer && AsciiText::EqualsIgnoreCase(slicer->GetName().ToString(), name))
        {
            return slicer;
        }
    }
    return nullptr;
}

bool SlicerNameExists(const ExcelDocument::Ptr& document,
                      std::string_view name,
                      const std::shared_ptr<Packaging::SlicersPart>& exceptPart,
                      std::string_view exceptName)
{
    for (const auto& part : AllSlicersParts(document))
    {
        for (const auto& slicer : SlicerElements(part))
        {
            const auto candidate = slicer ? slicer->GetName().ToString() : std::string{};
            if (part == exceptPart && AsciiText::EqualsIgnoreCase(candidate, exceptName))
            {
                continue;
            }
            if (AsciiText::EqualsIgnoreCase(candidate, name))
            {
                return true;
            }
        }
    }
    return false;
}

std::string MakeUniqueSlicerName(const ExcelDocument::Ptr& document)
{
    for (UInt32 index = 1; index < kMaxNameProbe; ++index)
    {
        auto candidate = "Slicer" + std::to_string(index);
        if (!SlicerNameExists(document, candidate, nullptr, {}))
        {
            return candidate;
        }
    }
    return {};
}

std::shared_ptr<Packaging::SlicerCachePart> SlicerCachePartByName(const ExcelDocument::Ptr& document,
                                                                  std::string_view cacheName)
{
    const auto workbookPart = document ? document->GetWorkbookPart() : nullptr;
    if (!workbookPart || cacheName.empty())
    {
        return nullptr;
    }
    for (const auto& part : workbookPart->GetSlicerCacheParts())
    {
        const auto root = part ? part->GetSlicerCacheDefinition() : nullptr;
        if (root && AsciiText::EqualsIgnoreCase(root->GetName().ToString(), cacheName))
        {
            return part;
        }
    }
    return nullptr;
}

/** Returns the `x14:tabular` cache data of a pivot slicer cache, when present. */
std::shared_ptr<X14::TabularSlicerCache> TabularCacheOf(const std::shared_ptr<X14::SlicerCacheDefinition>& root)
{
    const auto data = root ? root->GetFirstChildOfType<X14::SlicerCacheData>() : nullptr;
    return data ? data->GetFirstChildOfType<X14::TabularSlicerCache>() : nullptr;
}

/**
 * Returns the `x15:tableSlicerCache` extension of a table slicer cache.
 *
 * The presence of this extension is what distinguishes a table slicer from a
 * pivot slicer, because the two never coexist in one cache.
 */
std::shared_ptr<X15::TableSlicerCache> TableSlicerCacheOf(const std::shared_ptr<X14::SlicerCacheDefinition>& root)
{
    const auto extension =
        Detail::FindExtension<X14::SlicerCacheDefinitionExtensionList, S::SlicerCacheDefinitionExtension>(
            root, Detail::ExtensionUris::TableSlicerCache);
    return extension ? extension->GetFirstChildOfType<X15::TableSlicerCache>() : nullptr;
}

std::string MakeUniqueCacheName(const ExcelDocument::Ptr& document, std::string_view sourceName)
{
    const auto base = std::string(kCacheNamePrefix) + SanitizeCacheToken(sourceName);
    if (!SlicerCachePartByName(document, base))
    {
        return base;
    }
    for (UInt32 index = 1; index < kMaxNameProbe; ++index)
    {
        auto candidate = base + std::to_string(index);
        if (!SlicerCachePartByName(document, candidate))
        {
            return candidate;
        }
    }
    return {};
}

/** Returns true when any slicer in the workbook still references @p cacheName. */
bool CacheIsInUse(const ExcelDocument::Ptr& document,
                  std::string_view cacheName,
                  const std::shared_ptr<X14::Slicer>& except)
{
    for (const auto& part : AllSlicersParts(document))
    {
        for (const auto& slicer : SlicerElements(part))
        {
            if (!slicer || slicer == except)
            {
                continue;
            }
            if (AsciiText::EqualsIgnoreCase(slicer->GetCache().ToString(), cacheName))
            {
                return true;
            }
        }
    }
    return false;
}

std::shared_ptr<Packaging::WorksheetPart> HostWorksheetPart(const ExcelDocument::Ptr& document,
                                                            const std::shared_ptr<Packaging::SlicersPart>& part)
{
    const auto workbookPart = document ? document->GetWorkbookPart() : nullptr;
    if (!workbookPart || !part)
    {
        return nullptr;
    }
    for (const auto& worksheetPart : workbookPart->GetWorksheetParts())
    {
        if (!worksheetPart)
        {
            continue;
        }
        const auto parts = worksheetPart->GetSlicersParts();
        if (std::find(parts.begin(), parts.end(), part) != parts.end())
        {
            return worksheetPart;
        }
    }
    return nullptr;
}

Worksheet::Ptr WorksheetForPart(const ExcelDocument::Ptr& document,
                                const std::shared_ptr<Packaging::WorksheetPart>& worksheetPart)
{
    if (!worksheetPart)
    {
        return nullptr;
    }
    ExcelDocumentEditor editor(document);
    for (const auto& sheet : editor.Worksheets())
    {
        if (sheet && sheet->GetPart() == worksheetPart)
        {
            return sheet;
        }
    }
    return nullptr;
}

/** Returns the one-based tab index of @p worksheetPart, or zero when unknown. */
UInt32 SheetTabIndex(const ExcelDocument::Ptr& document,
                     const std::shared_ptr<Packaging::WorksheetPart>& worksheetPart)
{
    const auto workbookPart = document ? document->GetWorkbookPart() : nullptr;
    if (!workbookPart || !worksheetPart)
    {
        return 0;
    }
    UInt32 index = 1;
    for (const auto& candidate : workbookPart->GetWorksheetParts())
    {
        if (candidate == worksheetPart)
        {
            return index;
        }
        ++index;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Registries in the workbook and worksheet extension lists
// ---------------------------------------------------------------------------

/**
 * Adds the workbook-level registry entry that points at a slicer cache part.
 *
 * A pivot slicer is registered through the Excel 2010 extension and a table
 * slicer through the Excel 2013 one, so that Excel 2010 — which cannot
 * interpret a table slicer cache — simply ignores the entry.
 */
bool RegisterWorkbookCache(const ExcelDocument::Ptr& document,
                           const std::shared_ptr<Packaging::SlicerCachePart>& cachePart,
                           SlicerSourceKind kind)
{
    const auto workbookPart = document ? document->GetWorkbookPart() : nullptr;
    const auto workbook = workbookPart ? workbookPart->GetTypedRootElement() : nullptr;
    if (!workbook || !cachePart)
    {
        return false;
    }
    const auto relationshipId = RelationshipIdBetween(*workbookPart, *cachePart);
    if (relationshipId.empty())
    {
        return false;
    }

    std::shared_ptr<X14::SlicerCache> entry;
    if (kind == SlicerSourceKind::PivotTable)
    {
        const auto extension = Detail::GetOrCreateExtension<S::WorkbookExtensionList, S::WorkbookExtension>(
            workbook, Detail::ExtensionUris::WorkbookSlicerCaches, true);
        const auto caches = Detail::GetOrCreateExtensionFeature<X14::SlicerCaches>(extension, true);
        entry = caches ? caches->AppendChild<X14::SlicerCache>() : nullptr;
    }
    else
    {
        const auto extension = Detail::GetOrCreateExtension<S::WorkbookExtensionList, S::WorkbookExtension>(
            workbook, Detail::ExtensionUris::WorkbookSlicerCachesX15, true);
        const auto caches = Detail::GetOrCreateExtensionFeature<X15::SlicerCaches>(extension, true);
        entry = caches ? caches->AppendChild<X14::SlicerCache>() : nullptr;
    }
    if (!entry)
    {
        return false;
    }
    entry->SetId(StringValue(relationshipId));
    return true;
}

/** Removes the workbook registry entry whose relationship resolves to @p cachePart. */
void UnregisterWorkbookCache(const ExcelDocument::Ptr& document,
                             const std::shared_ptr<Packaging::SlicerCachePart>& cachePart)
{
    const auto workbookPart = document ? document->GetWorkbookPart() : nullptr;
    const auto workbook = workbookPart ? workbookPart->GetTypedRootElement() : nullptr;
    if (!workbook || !cachePart)
    {
        return;
    }
    const auto relationshipId = RelationshipIdBetween(*workbookPart, *cachePart);

    const auto prune = [&]<typename TCaches>(std::string_view uri)
    {
        const auto extension = Detail::FindExtension<S::WorkbookExtensionList, S::WorkbookExtension>(workbook, uri);
        const auto caches = extension ? extension->GetFirstChildOfType<TCaches>() : nullptr;
        if (!caches)
        {
            return;
        }
        for (const auto& entry : caches->template Elements<X14::SlicerCache>())
        {
            if (entry && entry->GetId().ToString() == relationshipId)
            {
                caches->RemoveChild(entry);
                break;
            }
        }
        if (caches->template Elements<X14::SlicerCache>().empty())
        {
            Detail::RemoveExtension<S::WorkbookExtensionList, S::WorkbookExtension>(workbook, uri);
        }
    };
    prune.template operator()<X14::SlicerCaches>(Detail::ExtensionUris::WorkbookSlicerCaches);
    prune.template operator()<X15::SlicerCaches>(Detail::ExtensionUris::WorkbookSlicerCachesX15);
}

/**
 * Adds this worksheet's `x14:slicerList` entry for a slicers part.
 *
 * There is one entry per slicers part rather than per slicer, so the entry is
 * written once and reused by every later slicer on the same worksheet.
 */
bool RegisterWorksheetSlicerList(const std::shared_ptr<S::Worksheet>& worksheet,
                                 const std::shared_ptr<Packaging::SlicersPart>& slicersPart,
                                 bool& added)
{
    added = false;
    if (!worksheet || !slicersPart)
    {
        return false;
    }
    const auto relationshipId = slicersPart->RelationshipId();
    if (relationshipId.empty())
    {
        return false;
    }
    const auto extension = Detail::GetOrCreateExtension<S::WorksheetExtensionList, S::WorksheetExtension>(
        worksheet, Detail::ExtensionUris::WorksheetSlicerList, true);
    const auto list = Detail::GetOrCreateExtensionFeature<X14::SlicerList>(extension, true);
    if (!list)
    {
        return false;
    }
    for (const auto& reference : list->Elements<X14::SlicerRef>())
    {
        if (reference && reference->GetId().ToString() == relationshipId)
        {
            return true;
        }
    }
    const auto reference = list->AppendChild<X14::SlicerRef>();
    if (!reference)
    {
        return false;
    }
    reference->SetId(StringValue(relationshipId));
    added = true;
    return true;
}

/** Removes this worksheet's `x14:slicerList` entry for a slicers part. */
void UnregisterWorksheetSlicerList(const std::shared_ptr<S::Worksheet>& worksheet,
                                   const std::string& relationshipId)
{
    if (!worksheet || relationshipId.empty())
    {
        return;
    }
    const auto extension = Detail::FindExtension<S::WorksheetExtensionList, S::WorksheetExtension>(
        worksheet, Detail::ExtensionUris::WorksheetSlicerList);
    const auto list = extension ? extension->GetFirstChildOfType<X14::SlicerList>() : nullptr;
    if (!list)
    {
        return;
    }
    for (const auto& reference : list->Elements<X14::SlicerRef>())
    {
        if (reference && reference->GetId().ToString() == relationshipId)
        {
            list->RemoveChild(reference);
            break;
        }
    }
    if (list->Elements<X14::SlicerRef>().empty())
    {
        Detail::RemoveExtension<S::WorksheetExtensionList, S::WorksheetExtension>(
            worksheet, Detail::ExtensionUris::WorksheetSlicerList);
    }
}

// ---------------------------------------------------------------------------
// Drawing shape
// ---------------------------------------------------------------------------

void SetMarker(const std::shared_ptr<XDR::MarkerType>& marker, const CellAddress& address)
{
    if (!marker)
    {
        return;
    }
    const auto set = [&]<typename T>(std::string value)
    {
        auto node = marker->GetFirstChildOfType<T>();
        if (!node)
        {
            node = marker->AppendChild<T>();
        }
        if (node)
        {
            node->SetText(value);
        }
    };
    set.template operator()<XDR::ColumnId>(std::to_string(address.Column().Value() - 1));
    set.template operator()<XDR::ColumnOffset>("0");
    set.template operator()<XDR::RowId>(std::to_string(address.Row().Value() - 1));
    set.template operator()<XDR::RowOffset>("0");
}

std::optional<CellAddress> ReadMarker(const std::shared_ptr<XDR::MarkerType>& marker)
{
    if (!marker)
    {
        return std::nullopt;
    }
    const auto column = marker->GetFirstChildOfType<XDR::ColumnId>();
    const auto row = marker->GetFirstChildOfType<XDR::RowId>();
    if (!column || !row)
    {
        return std::nullopt;
    }
    const auto columnValue = OpenXmlSimpleValueConvertor::GetUInt32ValueFromString(column->GetText());
    const auto rowValue = OpenXmlSimpleValueConvertor::GetUInt32ValueFromString(row->GetText());
    if (!columnValue.IsDefined() || !rowValue.IsDefined())
    {
        return std::nullopt;
    }
    return CellAddress::TryCreate(rowValue.Value() + 1, columnValue.Value() + 1);
}

/** Returns the `sle:slicer` element carried by an anchor, when the anchor is a slicer. */
std::shared_ptr<SLE::Slicer> AnchorSlicerElement(const std::shared_ptr<XDR::TwoCellAnchor>& anchor)
{
    const auto frame = anchor ? anchor->GetFirstChildOfType<XDR::GraphicFrame>() : nullptr;
    const auto graphic = frame ? frame->GetFirstChildOfType<A::Graphic>() : nullptr;
    const auto data = graphic ? graphic->GetFirstChildOfType<A::GraphicData>() : nullptr;
    if (!data || data->GetUri().ToString() != kSlicerGraphicUri)
    {
        return nullptr;
    }
    return data->GetFirstChildOfType<SLE::Slicer>();
}

std::shared_ptr<XDR::TwoCellAnchor> FindSlicerAnchor(const std::shared_ptr<XDR::WorksheetDrawing>& root,
                                                     std::string_view slicerName)
{
    if (!root)
    {
        return nullptr;
    }
    for (const auto& anchor : root->Elements<XDR::TwoCellAnchor>())
    {
        const auto slicer = AnchorSlicerElement(anchor);
        if (slicer && AsciiText::EqualsIgnoreCase(slicer->GetName().ToString(), slicerName))
        {
            return anchor;
        }
    }
    return nullptr;
}

UInt32 MaxDrawingId(const std::shared_ptr<XDR::WorksheetDrawing>& root)
{
    UInt32 result = 0;
    if (!root)
    {
        return result;
    }
    for (const auto& properties : root->Descendants<XDR::NonVisualDrawingProperties>())
    {
        result = std::max(result, properties->GetId().ValueOr(0));
    }
    return result;
}

/**
 * Appends the visible slicer shape.
 *
 * The structure mirrors the chart anchor written by Worksheet::AddChart; only
 * the graphic data URI and its single child differ. The `sle:slicer` element
 * refers to the slicer by name rather than by relationship identifier, which is
 * why renaming a slicer has to rewrite this element as well.
 *
 * Excel additionally wraps the anchor in `mc:AlternateContent` so that Excel
 * 2007 can show a placeholder picture. ExyokiOffice writes the plain frame,
 * so the shape needs Excel 2010 or newer to be rendered.
 */
bool AppendSlicerAnchor(const std::shared_ptr<XDR::WorksheetDrawing>& root,
                        const ExcelSlicerDefinition& definition,
                        std::string_view slicerName,
                        UInt32 drawingId)
{
    if (!root)
    {
        return false;
    }
    const auto anchor = root->AppendChild<XDR::TwoCellAnchor>();
    if (!anchor)
    {
        return false;
    }
    SetMarker(anchor->AppendChild<XDR::FromMarker>(), definition.From);
    SetMarker(anchor->AppendChild<XDR::ToMarker>(), definition.To);

    const auto frame = anchor->AppendChild<XDR::GraphicFrame>();
    const auto nonVisual = frame ? frame->AppendChild<XDR::NonVisualGraphicFrameProperties>() : nullptr;
    const auto properties = nonVisual ? nonVisual->AppendChild<XDR::NonVisualDrawingProperties>() : nullptr;
    if (!properties)
    {
        root->RemoveChild(anchor);
        return false;
    }
    properties->SetId(UInt32Value(drawingId));
    properties->SetName(StringValue(slicerName));
    nonVisual->AppendChild<XDR::NonVisualGraphicFrameDrawingProperties>();

    if (const auto transform = frame->AppendChild<XDR::Transform>())
    {
        if (const auto offset = transform->AppendChild<A::Offset>())
        {
            offset->SetAttribute(OpenXmlQualifiedName({}, "x"), "0");
            offset->SetAttribute(OpenXmlQualifiedName({}, "y"), "0");
        }
        if (const auto extents = transform->AppendChild<A::Extents>())
        {
            extents->SetAttribute(OpenXmlQualifiedName({}, "cx"), "0");
            extents->SetAttribute(OpenXmlQualifiedName({}, "cy"), "0");
        }
    }

    const auto graphic = frame->AppendChild<A::Graphic>();
    const auto data = graphic ? graphic->AppendChild<A::GraphicData>() : nullptr;
    const auto slicer = data ? data->AppendChild<SLE::Slicer>() : nullptr;
    if (!slicer)
    {
        root->RemoveChild(anchor);
        return false;
    }
    data->SetUri(StringValue(kSlicerGraphicUri));
    slicer->SetName(StringValue(slicerName));
    anchor->AppendChild<XDR::ClientData>();
    return true;
}

/** Removes the anchor of @p slicerName and, when the drawing is empty, the part. */
void RemoveSlicerAnchor(const std::shared_ptr<Packaging::WorksheetPart>& worksheetPart, std::string_view slicerName)
{
    const auto drawing = worksheetPart ? worksheetPart->GetDrawingsPart() : nullptr;
    const auto root = drawing ? drawing->GetWorksheetDrawing() : nullptr;
    const auto anchor = FindSlicerAnchor(root, slicerName);
    if (!anchor)
    {
        return;
    }
    root->RemoveChild(anchor);
    if (root->Children().empty())
    {
        Detail::UnlinkWorksheetDrawing(worksheetPart);
        worksheetPart->RemoveDrawingsPart();
    }
}

// ---------------------------------------------------------------------------
// Source resolution
// ---------------------------------------------------------------------------

/** Everything the writers need after a definition's source has been resolved. */
struct ResolvedSource
{
    SlicerSourceKind kind = SlicerSourceKind::PivotTable;
    /** Canonical source column name, using the workbook's own letter case. */
    std::string sourceName;
    /** Item captions in button order. */
    std::vector<std::string> captions;
    /** Pivot table name, for the cache's `x14:pivotTables` entry. */
    std::string pivotTableName;
    UInt32 pivotCacheId = 0;
    UInt32 pivotTabId = 0;
    UInt32 tableId = 0;
    UInt32 tableColumnId = 0;
    UInt32 tableColumnOrdinal = 0;
    ExcelTable::Ptr table;
};

std::vector<ExcelPivotTable::Ptr> AllPivotTables(const ExcelDocument::Ptr& document)
{
    std::vector<ExcelPivotTable::Ptr> result;
    const auto workbookPart = document ? document->GetWorkbookPart() : nullptr;
    if (!workbookPart)
    {
        return result;
    }
    for (const auto& worksheetPart : workbookPart->GetWorksheetParts())
    {
        if (!worksheetPart)
        {
            continue;
        }
        for (const auto& part : worksheetPart->GetPivotTableParts())
        {
            result.push_back(std::make_shared<ExcelPivotTable>(part, document));
        }
    }
    return result;
}

ExcelPivotTable::Ptr PivotTableByName(const ExcelDocument::Ptr& document, std::string_view name)
{
    for (const auto& pivotTable : AllPivotTables(document))
    {
        if (pivotTable && AsciiText::EqualsIgnoreCase(pivotTable->Name(), name))
        {
            return pivotTable;
        }
    }
    return nullptr;
}

std::vector<ExcelTable::Ptr> AllTables(const ExcelDocument::Ptr& document)
{
    std::vector<ExcelTable::Ptr> result;
    const auto workbookPart = document ? document->GetWorkbookPart() : nullptr;
    if (!workbookPart)
    {
        return result;
    }
    for (const auto& worksheetPart : workbookPart->GetWorksheetParts())
    {
        if (!worksheetPart)
        {
            continue;
        }
        for (const auto& part : worksheetPart->GetTableDefinitionParts())
        {
            result.push_back(std::make_shared<ExcelTable>(part));
        }
    }
    return result;
}

std::shared_ptr<Packaging::WorksheetPart> HostWorksheetPartOfPivotTable(const ExcelDocument::Ptr& document,
                                                                        const ExcelPivotTable::Ptr& pivotTable)
{
    const auto workbookPart = document ? document->GetWorkbookPart() : nullptr;
    const auto pivotPart = pivotTable ? pivotTable->GetPart() : nullptr;
    if (!workbookPart || !pivotPart)
    {
        return nullptr;
    }
    for (const auto& worksheetPart : workbookPart->GetWorksheetParts())
    {
        if (!worksheetPart)
        {
            continue;
        }
        const auto parts = worksheetPart->GetPivotTableParts();
        if (std::find(parts.begin(), parts.end(), pivotPart) != parts.end())
        {
            return worksheetPart;
        }
    }
    return nullptr;
}

std::shared_ptr<Packaging::WorksheetPart> HostWorksheetPartOfTable(const ExcelDocument::Ptr& document,
                                                                   const ExcelTable::Ptr& table)
{
    const auto workbookPart = document ? document->GetWorkbookPart() : nullptr;
    const auto tablePart = table ? table->GetPart() : nullptr;
    if (!workbookPart || !tablePart)
    {
        return nullptr;
    }
    for (const auto& worksheetPart : workbookPart->GetWorksheetParts())
    {
        if (!worksheetPart)
        {
            continue;
        }
        const auto parts = worksheetPart->GetTableDefinitionParts();
        if (std::find(parts.begin(), parts.end(), tablePart) != parts.end())
        {
            return worksheetPart;
        }
    }
    return nullptr;
}

/** One resolved table cell, ordered the same way pivot cache items are ordered. */
struct ColumnValue
{
    bool numeric = false;
    Real number = 0.0;
    std::string caption;
};

bool ColumnValueLess(const ColumnValue& left, const ColumnValue& right)
{
    if (left.numeric != right.numeric)
    {
        return left.numeric;
    }
    if (left.numeric)
    {
        return left.number < right.number;
    }
    const auto lowerLeft = AsciiText::ToLower(left.caption);
    const auto lowerRight = AsciiText::ToLower(right.caption);
    if (lowerLeft != lowerRight)
    {
        return lowerLeft < lowerRight;
    }
    return left.caption < right.caption;
}

/** Reads one worksheet cell and resolves shared strings and cached formula results. */
std::optional<ColumnValue> ReadColumnValue(const Worksheet& worksheet,
                                           const SharedStringTableService& strings,
                                           CellAddress address)
{
    const auto cell = worksheet.GetCellValue(address);
    if (!cell || cell->Kind() == CellValueKind::Blank)
    {
        return std::nullopt;
    }
    ColumnValue value;
    switch (cell->Kind())
    {
        case CellValueKind::SharedString:
        {
            const auto index = cell->SharedStringIndex();
            const auto text = index ? strings.Lookup(*index) : std::nullopt;
            value.caption = text.value_or(std::string{});
            break;
        }
        case CellValueKind::Boolean:
            value.caption = cell->BooleanValue().value_or(false) ? "TRUE" : "FALSE";
            break;
        case CellValueKind::Formula:
            value.caption = cell->FormulaValue().CachedText;
            break;
        case CellValueKind::Number:
        {
            const auto parsed = OpenXmlSimpleValueConvertor::GetDoubleValueFromString(cell->Text());
            if (parsed.IsDefined())
            {
                value.numeric = true;
                value.number = parsed.Value();
            }
            value.caption = cell->Text();
            break;
        }
        default:
            value.caption = cell->Text();
            break;
    }
    if (value.caption.empty())
    {
        return std::nullopt;
    }
    return value;
}

/** Reads the distinct values of one table column, in slicer button order. */
std::vector<std::string> ReadTableColumnCaptions(const ExcelDocument::Ptr& document,
                                                 const ExcelTable::Ptr& table,
                                                 UInt32 columnOrdinal)
{
    std::vector<std::string> result;
    const auto range = table ? table->Range() : std::nullopt;
    const auto worksheetPart = HostWorksheetPartOfTable(document, table);
    const auto worksheet = WorksheetForPart(document, worksheetPart);
    if (!range || !worksheet)
    {
        return result;
    }

    const auto firstRow = range->First().Row().Value() + 1;
    auto lastRow = range->Last().Row().Value();
    if (table->TotalsRowShown() && lastRow > firstRow)
    {
        --lastRow;
    }
    const auto column = range->First().Column().Value() + columnOrdinal;

    const SharedStringTableService strings(document);
    std::vector<ColumnValue> values;
    for (UInt32 row = firstRow; row <= lastRow; ++row)
    {
        const auto address = CellAddress::TryCreate(row, column);
        const auto value = address ? ReadColumnValue(*worksheet, strings, *address) : std::nullopt;
        if (!value)
        {
            continue;
        }
        const auto duplicate = std::ranges::any_of(values, [&](const ColumnValue& existing)
                                                   { return AsciiText::EqualsIgnoreCase(existing.caption, value->caption); });
        if (!duplicate)
        {
            values.push_back(*value);
        }
    }
    std::stable_sort(values.begin(), values.end(), ColumnValueLess);
    result.reserve(values.size());
    for (const auto& value : values)
    {
        result.push_back(value.caption);
    }
    return result;
}

std::optional<ResolvedSource> ResolvePivotSource(const ExcelDocument::Ptr& document,
                                                 const ExcelSlicerDefinition& definition,
                                                 SlicerResult& status)
{
    const auto pivotTable = PivotTableByName(document, definition.PivotTableName);
    if (!pivotTable)
    {
        status = Failure(SlicerError::UnknownSource,
                         "The pivot table '" + definition.PivotTableName + "' does not exist in this workbook.");
        return std::nullopt;
    }

    ResolvedSource resolved;
    resolved.kind = SlicerSourceKind::PivotTable;
    for (const auto& fieldName : pivotTable->SourceFieldNames())
    {
        if (AsciiText::EqualsIgnoreCase(fieldName, definition.SourceField))
        {
            resolved.sourceName = fieldName;
            break;
        }
    }
    if (resolved.sourceName.empty())
    {
        status = Failure(SlicerError::UnknownField,
                         "The pivot cache of '" + pivotTable->Name() + "' has no field named '" +
                             definition.SourceField + "'.");
        return std::nullopt;
    }

    for (const auto& item : pivotTable->FieldItems(resolved.sourceName))
    {
        resolved.captions.push_back(item.Caption);
    }
    resolved.pivotTableName = pivotTable->Name();
    resolved.pivotCacheId = pivotTable->CacheId();
    resolved.pivotTabId = SheetTabIndex(document, HostWorksheetPartOfPivotTable(document, pivotTable));
    return resolved;
}

std::optional<ResolvedSource> ResolveTableSource(const ExcelDocument::Ptr& document,
                                                 const ExcelSlicerDefinition& definition,
                                                 SlicerResult& status)
{
    ExcelTable::Ptr table;
    for (const auto& candidate : AllTables(document))
    {
        if (candidate && AsciiText::EqualsIgnoreCase(candidate->Name(), definition.TableName))
        {
            table = candidate;
            break;
        }
    }
    if (!table)
    {
        status = Failure(SlicerError::UnknownSource,
                         "The table '" + definition.TableName + "' does not exist in this workbook.");
        return std::nullopt;
    }

    ResolvedSource resolved;
    resolved.kind = SlicerSourceKind::Table;
    resolved.table = table;
    resolved.tableId = table->Id();

    const auto columns = table->Columns();
    bool found = false;
    for (Size index = 0; index < columns.size(); ++index)
    {
        if (AsciiText::EqualsIgnoreCase(columns[index].Name, definition.SourceField))
        {
            resolved.sourceName = columns[index].Name;
            resolved.tableColumnId = columns[index].Id;
            resolved.tableColumnOrdinal = static_cast<UInt32>(index);
            found = true;
            break;
        }
    }
    if (!found)
    {
        status = Failure(SlicerError::UnknownField,
                         "The table '" + table->Name() + "' has no column named '" + definition.SourceField + "'.");
        return std::nullopt;
    }

    resolved.captions = ReadTableColumnCaptions(document, table, resolved.tableColumnOrdinal);
    return resolved;
}

std::optional<ResolvedSource> ResolveSource(const ExcelDocument::Ptr& document,
                                            const ExcelSlicerDefinition& definition,
                                            SlicerResult& status)
{
    if (definition.SourceField.empty())
    {
        status = Failure(SlicerError::UnknownField, "The slicer source column name must not be empty.");
        return std::nullopt;
    }
    return definition.SourceKind == SlicerSourceKind::Table ? ResolveTableSource(document, definition, status)
                                                            : ResolvePivotSource(document, definition, status);
}

/**
 * Finds a cache another slicer already built for exactly the same source.
 *
 * Sharing is keyed on the source kind, the identity of the backing object, and
 * the source column. Keying on the column alone would wrongly make a `Region`
 * slicer over one pivot table share a cache with a `Region` slicer over
 * another one, which would silently link two unrelated reports.
 */
std::shared_ptr<Packaging::SlicerCachePart> FindSharedCachePart(const ExcelDocument::Ptr& document,
                                                                const ResolvedSource& resolved)
{
    const auto workbookPart = document ? document->GetWorkbookPart() : nullptr;
    if (!workbookPart)
    {
        return nullptr;
    }
    for (const auto& cachePart : workbookPart->GetSlicerCacheParts())
    {
        const auto root = cachePart ? cachePart->GetSlicerCacheDefinition() : nullptr;
        if (!root || !AsciiText::EqualsIgnoreCase(root->GetSourceName().ToString(), resolved.sourceName))
        {
            continue;
        }
        const auto tableCache = TableSlicerCacheOf(root);
        if (resolved.kind == SlicerSourceKind::Table)
        {
            if (tableCache && tableCache->GetTableId().ValueOr(0) == resolved.tableId &&
                tableCache->GetColumn().ValueOr(0) == resolved.tableColumnId)
            {
                return cachePart;
            }
            continue;
        }
        if (tableCache)
        {
            continue;
        }
        const auto pivotTables = root->GetFirstChildOfType<X14::SlicerCachePivotTables>();
        if (!pivotTables)
        {
            continue;
        }
        for (const auto& entry : pivotTables->Elements<X14::SlicerCachePivotTable>())
        {
            if (entry && AsciiText::EqualsIgnoreCase(entry->GetName().ToString(), resolved.pivotTableName))
            {
                return cachePart;
            }
        }
    }
    return nullptr;
}

/**
 * Maps the requested captions onto the resolved item list.
 *
 * An empty request selects everything, which Excel represents by omitting the
 * selection attribute altogether rather than by marking every item selected.
 */
bool ResolveSelection(const ResolvedSource& resolved,
                      const std::vector<std::string>& requested,
                      std::vector<bool>& selection,
                      SlicerResult& status)
{
    selection.assign(resolved.captions.size(), true);
    if (requested.empty())
    {
        return true;
    }
    selection.assign(resolved.captions.size(), false);
    for (const auto& caption : requested)
    {
        bool matched = false;
        for (Size index = 0; index < resolved.captions.size(); ++index)
        {
            if (AsciiText::EqualsIgnoreCase(resolved.captions[index], caption))
            {
                selection[index] = true;
                matched = true;
                break;
            }
        }
        if (!matched)
        {
            status = Failure(SlicerError::UnknownItem,
                             "The item '" + caption + "' does not occur in the slicer source column.");
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Writers
// ---------------------------------------------------------------------------

void ClearChildren(const std::shared_ptr<OpenXMLElement>& element)
{
    if (!element)
    {
        return;
    }
    for (const auto& child : element->Children())
    {
        element->RemoveChild(child);
    }
}

bool WriteCacheDefinition(const std::shared_ptr<Packaging::SlicerCachePart>& cachePart,
                          const ExcelSlicerDefinition& definition,
                          const ResolvedSource& resolved,
                          const std::vector<bool>& selection,
                          bool hasExplicitSelection,
                          std::string_view cacheName)
{
    const auto root = cachePart ? cachePart->GetSlicerCacheDefinition() : nullptr;
    if (!root)
    {
        return false;
    }
    ClearChildren(root);
    root->SetName(StringValue(cacheName));
    root->SetSourceName(StringValue(resolved.sourceName));

    if (resolved.kind == SlicerSourceKind::PivotTable)
    {
        const auto pivotTables = root->AppendChild<X14::SlicerCachePivotTables>();
        const auto entry = pivotTables ? pivotTables->AppendChild<X14::SlicerCachePivotTable>() : nullptr;
        if (!entry)
        {
            return false;
        }
        entry->SetTabId(UInt32Value(resolved.pivotTabId));
        entry->SetName(StringValue(resolved.pivotTableName));

        const auto data = root->AppendChild<X14::SlicerCacheData>();
        const auto tabular = data ? data->AppendChild<X14::TabularSlicerCache>() : nullptr;
        if (!tabular)
        {
            return false;
        }
        tabular->SetPivotCacheId(UInt32Value(resolved.pivotCacheId));
        tabular->SetSortOrder(ToSortOrder(definition.SortOrder));
        tabular->SetCustomListSort(BooleanValue(definition.CustomListSort));
        tabular->SetShowMissing(BooleanValue(definition.ShowMissing));
        tabular->SetCrossFilter(ToCrossFilter(definition.CrossFilter));

        const auto items = tabular->AppendChild<X14::TabularSlicerCacheItems>();
        if (!items)
        {
            return false;
        }
        items->SetCount(UInt32Value(static_cast<UInt32>(resolved.captions.size())));
        for (Size index = 0; index < resolved.captions.size(); ++index)
        {
            const auto item = items->AppendChild<X14::TabularSlicerCacheItem>();
            if (!item)
            {
                return false;
            }
            item->SetAtom(UInt32Value(static_cast<UInt32>(index)));
            if (hasExplicitSelection)
            {
                item->SetIsSelected(BooleanValue(selection[index]));
            }
        }
        return true;
    }

    // A table slicer cannot use x14:data, because the tabular cache requires a
    // pivot cache identifier. Everything it needs lives in the x15 extension.
    const auto extension =
        Detail::GetOrCreateExtension<X14::SlicerCacheDefinitionExtensionList, S::SlicerCacheDefinitionExtension>(
            root, Detail::ExtensionUris::TableSlicerCache, true);
    const auto tableCache = Detail::GetOrCreateExtensionFeature<X15::TableSlicerCache>(extension, true);
    if (!tableCache)
    {
        return false;
    }
    tableCache->SetTableId(UInt32Value(resolved.tableId));
    tableCache->SetColumn(UInt32Value(resolved.tableColumnId));
    tableCache->SetSortOrder(ToSortOrder(definition.SortOrder));
    tableCache->SetCustomListSort(BooleanValue(definition.CustomListSort));
    tableCache->SetCrossFilter(ToCrossFilter(definition.CrossFilter));
    return true;
}

/** Applies a table slicer's selection to the table auto-filter that stores it. */
bool ApplyTableSelection(const ResolvedSource& resolved,
                         const std::vector<bool>& selection,
                         bool hasExplicitSelection)
{
    if (resolved.kind != SlicerSourceKind::Table || !resolved.table)
    {
        return true;
    }
    if (!hasExplicitSelection)
    {
        resolved.table->RemoveValueFilter(resolved.tableColumnOrdinal);
        return true;
    }
    ExcelTableValueFilter filter;
    filter.ColumnIndex = resolved.tableColumnOrdinal;
    for (Size index = 0; index < resolved.captions.size(); ++index)
    {
        if (selection[index])
        {
            filter.Values.push_back(resolved.captions[index]);
        }
    }
    if (filter.Values.empty())
    {
        return resolved.table->RemoveValueFilter(resolved.tableColumnOrdinal);
    }
    resolved.table->SetAutoFilterEnabled(true);
    return resolved.table->SetValueFilter(filter);
}

void WriteSlicerElement(const std::shared_ptr<X14::Slicer>& slicer,
                        const ExcelSlicerDefinition& definition,
                        std::string_view slicerName,
                        std::string_view cacheName,
                        std::string_view caption)
{
    slicer->SetName(StringValue(slicerName));
    slicer->SetCache(StringValue(cacheName));
    slicer->SetCaption(StringValue(caption));
    slicer->SetColumnCount(UInt32Value(definition.ColumnCount));
    slicer->SetRowHeight(UInt32Value(definition.RowHeight));
    slicer->SetShowCaption(BooleanValue(definition.ShowCaption));
    if (!definition.Style.empty())
    {
        slicer->SetStyle(StringValue(definition.Style));
    }
    if (definition.LockedPosition)
    {
        slicer->SetLockedPosition(BooleanValue(true));
    }
    if (definition.StartItem != 0)
    {
        slicer->SetStartItem(UInt32Value(definition.StartItem));
    }
    if (definition.Level != 0)
    {
        slicer->SetLevel(UInt32Value(definition.Level));
    }
}

SlicerResult ValidatePresentation(const ExcelSlicerDefinition& definition)
{
    if (definition.ColumnCount == 0 || definition.ColumnCount > kMaxColumnCount)
    {
        return Failure(SlicerError::InvalidPresentation, "The slicer column count must be between 1 and 20000.");
    }
    if (definition.RowHeight == 0)
    {
        return Failure(SlicerError::InvalidPresentation, "The slicer row height must be greater than zero.");
    }
    if (definition.WriteDrawing)
    {
        if (!definition.From.IsValid() || !definition.To.IsValid() ||
            definition.To.Row().Value() < definition.From.Row().Value() ||
            definition.To.Column().Value() < definition.From.Column().Value())
        {
            return Failure(SlicerError::InvalidAnchor,
                           "The slicer anchor must be a valid, non-inverted two-cell rectangle.");
        }
    }
    return Success();
}

} // namespace SlicerDetail

// ---------------------------------------------------------------------------
// Free functions
// ---------------------------------------------------------------------------

bool IsValidSlicerName(std::string_view name)
{
    if (name.empty() || name.size() > SlicerDetail::kMaxNameLength)
    {
        return false;
    }
    if (name.front() == ' ' || name.back() == ' ')
    {
        return false;
    }
    return std::ranges::none_of(name, [](char character)
                                { return AsciiText::IsControl(character); });
}

// ---------------------------------------------------------------------------
// ExcelSlicer
// ---------------------------------------------------------------------------

ExcelSlicer::ExcelSlicer(std::shared_ptr<Packaging::SlicersPart> part,
                         std::string name,
                         std::shared_ptr<Packaging::ExcelDocument> document)
    : m_part(std::move(part)), m_name(std::move(name)), m_document(std::move(document))
{
}

bool ExcelSlicer::IsValid() const noexcept
{
    return m_part != nullptr && m_document != nullptr && SlicerDetail::FindSlicerElement(m_part, m_name) != nullptr;
}

std::shared_ptr<Packaging::SlicersPart> ExcelSlicer::GetPart() const
{
    return m_part;
}

std::shared_ptr<DocumentFormat::OpenXml::Office2010::Excel::Slicer> ExcelSlicer::GetLowLevelApi() const
{
    return SlicerDetail::FindSlicerElement(m_part, m_name);
}

std::shared_ptr<Packaging::SlicerCachePart> ExcelSlicer::GetCachePart() const
{
    const auto slicer = GetLowLevelApi();
    return slicer ? SlicerDetail::SlicerCachePartByName(m_document, slicer->GetCache().ToString()) : nullptr;
}

std::string ExcelSlicer::Name() const
{
    const auto slicer = GetLowLevelApi();
    return slicer ? slicer->GetName().ToString() : std::string{};
}

std::string ExcelSlicer::Caption() const
{
    const auto slicer = GetLowLevelApi();
    return slicer ? slicer->GetCaption().ToString() : std::string{};
}

UInt32 ExcelSlicer::ColumnCount() const
{
    const auto slicer = GetLowLevelApi();
    return slicer ? slicer->GetColumnCount().ValueOr(1) : 0;
}

std::string ExcelSlicer::Style() const
{
    const auto slicer = GetLowLevelApi();
    return slicer ? slicer->GetStyle().ToString() : std::string{};
}

bool ExcelSlicer::ShowCaption() const
{
    const auto slicer = GetLowLevelApi();
    return slicer && slicer->GetShowCaption().ValueOr(true);
}

bool ExcelSlicer::LockedPosition() const
{
    const auto slicer = GetLowLevelApi();
    return slicer && slicer->GetLockedPosition().ValueOr(false);
}

SlicerSourceKind ExcelSlicer::SourceKind() const
{
    namespace X14 = DocumentFormat::OpenXml::Office2010::Excel;
    const auto cachePart = GetCachePart();
    const auto root = cachePart ? cachePart->GetSlicerCacheDefinition() : nullptr;
    if (root && SlicerDetail::TableSlicerCacheOf(root))
    {
        return SlicerSourceKind::Table;
    }
    return SlicerSourceKind::PivotTable;
}

std::string ExcelSlicer::SourceField() const
{
    const auto cachePart = GetCachePart();
    const auto root = cachePart ? cachePart->GetSlicerCacheDefinition() : nullptr;
    return root ? root->GetSourceName().ToString() : std::string{};
}

std::string ExcelSlicer::SourceObjectName() const
{
    namespace X14 = DocumentFormat::OpenXml::Office2010::Excel;
    const auto cachePart = GetCachePart();
    const auto root = cachePart ? cachePart->GetSlicerCacheDefinition() : nullptr;
    if (!root)
    {
        return {};
    }
    if (const auto tableCache = SlicerDetail::TableSlicerCacheOf(root))
    {
        for (const auto& table : SlicerDetail::AllTables(m_document))
        {
            if (table && table->Id() == tableCache->GetTableId().ValueOr(0))
            {
                return table->Name();
            }
        }
        return {};
    }
    const auto pivotTables = root->GetFirstChildOfType<X14::SlicerCachePivotTables>();
    const auto entry = pivotTables ? pivotTables->GetFirstChildOfType<X14::SlicerCachePivotTable>() : nullptr;
    return entry ? entry->GetName().ToString() : std::string{};
}

SlicerSortOrder ExcelSlicer::SortOrder() const
{
    const auto cachePart = GetCachePart();
    const auto root = cachePart ? cachePart->GetSlicerCacheDefinition() : nullptr;
    if (!root)
    {
        return SlicerSortOrder::Ascending;
    }
    if (const auto tableCache = SlicerDetail::TableSlicerCacheOf(root))
    {
        return SlicerDetail::FromSortOrder(tableCache->GetSortOrder());
    }
    const auto tabular = SlicerDetail::TabularCacheOf(root);
    return tabular ? SlicerDetail::FromSortOrder(tabular->GetSortOrder()) : SlicerSortOrder::Ascending;
}

SlicerCrossFilter ExcelSlicer::CrossFilter() const
{
    const auto cachePart = GetCachePart();
    const auto root = cachePart ? cachePart->GetSlicerCacheDefinition() : nullptr;
    if (!root)
    {
        return SlicerCrossFilter::ShowItemsWithDataAtTop;
    }
    if (const auto tableCache = SlicerDetail::TableSlicerCacheOf(root))
    {
        return SlicerDetail::FromCrossFilter(tableCache->GetCrossFilter());
    }
    const auto tabular = SlicerDetail::TabularCacheOf(root);
    return tabular ? SlicerDetail::FromCrossFilter(tabular->GetCrossFilter())
                   : SlicerCrossFilter::ShowItemsWithDataAtTop;
}

std::vector<ExcelSlicerItem> ExcelSlicer::Items() const
{
    namespace X14 = DocumentFormat::OpenXml::Office2010::Excel;
    std::vector<ExcelSlicerItem> result;
    const auto cachePart = GetCachePart();
    const auto root = cachePart ? cachePart->GetSlicerCacheDefinition() : nullptr;
    if (!root)
    {
        return result;
    }

    if (const auto tableCache = SlicerDetail::TableSlicerCacheOf(root))
    {
        // A table slicer keeps no item list of its own; the buttons come from
        // the table cells and the selection from the table auto-filter.
        ExcelTable::Ptr table;
        for (const auto& candidate : SlicerDetail::AllTables(m_document))
        {
            if (candidate && candidate->Id() == tableCache->GetTableId().ValueOr(0))
            {
                table = candidate;
                break;
            }
        }
        if (!table)
        {
            return result;
        }
        const auto columns = table->Columns();
        UInt32 ordinal = 0;
        for (Size index = 0; index < columns.size(); ++index)
        {
            if (columns[index].Id == tableCache->GetColumn().ValueOr(0))
            {
                ordinal = static_cast<UInt32>(index);
                break;
            }
        }
        std::vector<std::string> selected;
        bool filtered = false;
        for (const auto& filter : table->ValueFilters())
        {
            if (filter.ColumnIndex == ordinal)
            {
                selected = filter.Values;
                filtered = true;
                break;
            }
        }
        for (auto& caption : SlicerDetail::ReadTableColumnCaptions(m_document, table, ordinal))
        {
            ExcelSlicerItem item;
            item.Selected = !filtered || std::ranges::any_of(selected, [&](const std::string& value)
                                                             { return AsciiText::EqualsIgnoreCase(value, caption); });
            item.Caption = std::move(caption);
            result.push_back(std::move(item));
        }
        return result;
    }

    const auto tabular = SlicerDetail::TabularCacheOf(root);
    const auto items = tabular ? tabular->GetFirstChildOfType<X14::TabularSlicerCacheItems>() : nullptr;
    if (!items)
    {
        return result;
    }
    const auto pivotTable = SlicerDetail::PivotTableByName(m_document, SourceObjectName());
    const auto captions = pivotTable ? pivotTable->FieldItems(root->GetSourceName().ToString())
                                     : std::vector<ExcelPivotItem>{};
    for (const auto& entry : items->Elements<X14::TabularSlicerCacheItem>())
    {
        if (!entry)
        {
            continue;
        }
        const auto index = entry->GetAtom().ValueOr(0);
        ExcelSlicerItem item;
        item.Caption = index < captions.size() ? captions[index].Caption : std::string{};
        item.Selected = entry->GetIsSelected().ValueOr(true);
        item.HasNoData = entry->GetNonDisplay().ValueOr(false);
        result.push_back(std::move(item));
    }
    return result;
}

std::optional<std::pair<CellAddress, CellAddress>> ExcelSlicer::Anchor() const
{
    const auto worksheetPart = SlicerDetail::HostWorksheetPart(m_document, m_part);
    const auto drawing = worksheetPart ? worksheetPart->GetDrawingsPart() : nullptr;
    const auto root = drawing ? drawing->GetWorksheetDrawing() : nullptr;
    const auto anchor = SlicerDetail::FindSlicerAnchor(root, m_name);
    if (!anchor)
    {
        return std::nullopt;
    }
    namespace XDR = DocumentFormat::OpenXml::Drawing::Spreadsheet;
    const auto from = SlicerDetail::ReadMarker(anchor->GetFirstChildOfType<XDR::FromMarker>());
    const auto to = SlicerDetail::ReadMarker(anchor->GetFirstChildOfType<XDR::ToMarker>());
    if (!from || !to)
    {
        return std::nullopt;
    }
    return std::make_pair(*from, *to);
}

SlicerResult ExcelSlicer::SetAnchor(CellAddress from, CellAddress to)
{
    namespace XDR = DocumentFormat::OpenXml::Drawing::Spreadsheet;
    if (!IsValid())
    {
        return SlicerDetail::Failure(SlicerError::InvalidWorksheet, "The slicer is detached.");
    }
    if (!from.IsValid() || !to.IsValid() || to.Row().Value() < from.Row().Value() ||
        to.Column().Value() < from.Column().Value())
    {
        return SlicerDetail::Failure(SlicerError::InvalidAnchor,
                                     "The slicer anchor must be a valid, non-inverted two-cell rectangle.");
    }
    const auto worksheetPart = SlicerDetail::HostWorksheetPart(m_document, m_part);
    const auto drawing = worksheetPart ? worksheetPart->GetDrawingsPart() : nullptr;
    const auto root = drawing ? drawing->GetWorksheetDrawing() : nullptr;
    const auto anchor = SlicerDetail::FindSlicerAnchor(root, m_name);
    if (!anchor)
    {
        return SlicerDetail::Failure(SlicerError::InvalidAnchor, "The slicer has no visible shape to move.");
    }
    SlicerDetail::SetMarker(anchor->GetFirstChildOfType<XDR::FromMarker>(), from);
    SlicerDetail::SetMarker(anchor->GetFirstChildOfType<XDR::ToMarker>(), to);
    return SlicerDetail::Success();
}

SlicerResult ExcelSlicer::SetName(std::string_view name)
{
    namespace XDR = DocumentFormat::OpenXml::Drawing::Spreadsheet;
    const auto slicer = GetLowLevelApi();
    if (!slicer || !m_document)
    {
        return SlicerDetail::Failure(SlicerError::InvalidWorksheet, "The slicer is detached.");
    }
    if (!IsValidSlicerName(name))
    {
        return SlicerDetail::Failure(SlicerError::InvalidName, "The slicer name is not a valid Excel object name.");
    }
    if (SlicerDetail::SlicerNameExists(m_document, name, m_part, m_name))
    {
        return SlicerDetail::Failure(SlicerError::InvalidName,
                                     "The slicer name '" + std::string(name) + "' is already used in this workbook.");
    }

    // The drawing refers to the slicer by name, so the shape has to follow.
    const auto worksheetPart = SlicerDetail::HostWorksheetPart(m_document, m_part);
    const auto drawing = worksheetPart ? worksheetPart->GetDrawingsPart() : nullptr;
    const auto root = drawing ? drawing->GetWorksheetDrawing() : nullptr;
    if (const auto anchor = SlicerDetail::FindSlicerAnchor(root, m_name))
    {
        if (const auto shape = SlicerDetail::AnchorSlicerElement(anchor))
        {
            shape->SetName(StringValue(name));
        }
        const auto frame = anchor->GetFirstChildOfType<XDR::GraphicFrame>();
        const auto nonVisual = frame ? frame->GetFirstChildOfType<XDR::NonVisualGraphicFrameProperties>() : nullptr;
        if (const auto properties =
                nonVisual ? nonVisual->GetFirstChildOfType<XDR::NonVisualDrawingProperties>() : nullptr)
        {
            properties->SetName(StringValue(name));
        }
    }
    slicer->SetName(StringValue(name));
    m_name = std::string(name);
    return SlicerDetail::Success();
}

SlicerResult ExcelSlicer::SetCaption(std::string_view caption)
{
    const auto slicer = GetLowLevelApi();
    if (!slicer)
    {
        return SlicerDetail::Failure(SlicerError::InvalidWorksheet, "The slicer is detached.");
    }
    slicer->SetCaption(StringValue(caption));
    return SlicerDetail::Success();
}

std::optional<ExcelSlicerDefinition> ExcelSlicer::Definition() const
{
    const auto slicer = GetLowLevelApi();
    if (!slicer)
    {
        return std::nullopt;
    }
    ExcelSlicerDefinition definition;
    definition.Name = slicer->GetName().ToString();
    definition.Caption = slicer->GetCaption().ToString();
    definition.ColumnCount = slicer->GetColumnCount().ValueOr(1);
    definition.RowHeight = slicer->GetRowHeight().ValueOr(0);
    definition.Style = slicer->GetStyle().ToString();
    definition.ShowCaption = slicer->GetShowCaption().ValueOr(true);
    definition.LockedPosition = slicer->GetLockedPosition().ValueOr(false);
    definition.StartItem = slicer->GetStartItem().ValueOr(0);
    definition.Level = slicer->GetLevel().ValueOr(0);
    definition.SourceKind = SourceKind();
    definition.SourceField = SourceField();
    if (definition.SourceKind == SlicerSourceKind::Table)
    {
        definition.TableName = SourceObjectName();
    }
    else
    {
        definition.PivotTableName = SourceObjectName();
    }
    definition.SortOrder = SortOrder();
    definition.CrossFilter = CrossFilter();

    const auto cachePart = GetCachePart();
    const auto root = cachePart ? cachePart->GetSlicerCacheDefinition() : nullptr;
    if (root)
    {
        if (const auto tableCache = SlicerDetail::TableSlicerCacheOf(root))
        {
            definition.CustomListSort = tableCache->GetCustomListSort().ValueOr(true);
        }
        else if (const auto tabular = SlicerDetail::TabularCacheOf(root))
        {
            definition.CustomListSort = tabular->GetCustomListSort().ValueOr(true);
            definition.ShowMissing = tabular->GetShowMissing().ValueOr(true);
        }
    }

    const auto items = Items();
    const bool allSelected = std::ranges::all_of(items, [](const ExcelSlicerItem& item)
                                                 { return item.Selected; });
    if (!allSelected)
    {
        for (const auto& item : items)
        {
            if (item.Selected)
            {
                definition.SelectedItems.push_back(item.Caption);
            }
        }
    }

    if (const auto anchor = Anchor())
    {
        definition.From = anchor->first;
        definition.To = anchor->second;
        definition.WriteDrawing = true;
    }
    else
    {
        definition.WriteDrawing = false;
    }
    return definition;
}

SlicerResult ExcelSlicer::SelectItems(const std::vector<std::string>& captions)
{
    auto definition = Definition();
    if (!definition)
    {
        return SlicerDetail::Failure(SlicerError::InvalidWorksheet, "The slicer is detached.");
    }
    definition->SelectedItems = captions;
    return Update(*definition);
}

SlicerResult ExcelSlicer::Update(const ExcelSlicerDefinition& definition)
{
    const auto slicer = GetLowLevelApi();
    const auto cachePart = GetCachePart();
    if (!slicer || !m_document || !cachePart)
    {
        return SlicerDetail::Failure(SlicerError::InvalidWorksheet, "The slicer is detached.");
    }
    if (auto status = SlicerDetail::ValidatePresentation(definition); !status)
    {
        return status;
    }
    if (definition.SourceKind != SourceKind() || !AsciiText::EqualsIgnoreCase(definition.SourceField, SourceField()))
    {
        return SlicerDetail::Failure(SlicerError::UnknownSource,
                                     "The slicer source cannot be changed; remove the slicer and create a new one.");
    }

    SlicerResult status;
    const auto resolved = SlicerDetail::ResolveSource(m_document, definition, status);
    if (!resolved)
    {
        return status;
    }
    std::vector<bool> selection;
    if (!SlicerDetail::ResolveSelection(*resolved, definition.SelectedItems, selection, status))
    {
        return status;
    }
    const bool hasExplicitSelection = !definition.SelectedItems.empty();

    const auto cacheName = slicer->GetCache().ToString();
    const auto originalCacheXml = cachePart->GetXmlString();
    if (!SlicerDetail::WriteCacheDefinition(cachePart, definition, *resolved, selection, hasExplicitSelection,
                                            cacheName))
    {
        cachePart->SetXmlString(originalCacheXml);
        return SlicerDetail::Failure(SlicerError::WriteFailed, "The slicer cache could not be written.");
    }
    if (!SlicerDetail::ApplyTableSelection(*resolved, selection, hasExplicitSelection))
    {
        cachePart->SetXmlString(originalCacheXml);
        return SlicerDetail::Failure(SlicerError::WriteFailed, "The table filter could not be updated.");
    }

    const auto caption = definition.Caption.empty() ? resolved->sourceName : definition.Caption;
    SlicerDetail::WriteSlicerElement(slicer, definition, m_name, cacheName, caption);
    if (definition.WriteDrawing && !definition.Name.empty() &&
        !AsciiText::EqualsIgnoreCase(definition.Name, m_name))
    {
        if (auto renamed = SetName(definition.Name); !renamed)
        {
            return renamed;
        }
    }
    if (definition.WriteDrawing && definition.From.IsValid() && definition.To.IsValid())
    {
        SetAnchor(definition.From, definition.To);
    }
    return SlicerDetail::Success();
}

// ---------------------------------------------------------------------------
// SlicerBuilder
// ---------------------------------------------------------------------------

SlicerBuilder::SlicerBuilder(std::shared_ptr<Worksheet> sheet)
    : m_sheet(std::move(sheet))
{
}

SlicerBuilder& SlicerBuilder::SetName(std::string name)
{
    m_definition.Name = std::move(name);
    return *this;
}

SlicerBuilder& SlicerBuilder::SetCaption(std::string caption)
{
    m_definition.Caption = std::move(caption);
    return *this;
}

SlicerBuilder& SlicerBuilder::SetPivotSource(std::string pivotTableName, std::string fieldName)
{
    m_definition.SourceKind = SlicerSourceKind::PivotTable;
    m_definition.PivotTableName = std::move(pivotTableName);
    m_definition.TableName.clear();
    m_definition.SourceField = std::move(fieldName);
    return *this;
}

SlicerBuilder& SlicerBuilder::SetTableSource(std::string tableName, std::string columnName)
{
    m_definition.SourceKind = SlicerSourceKind::Table;
    m_definition.TableName = std::move(tableName);
    m_definition.PivotTableName.clear();
    m_definition.SourceField = std::move(columnName);
    return *this;
}

SlicerBuilder& SlicerBuilder::SetAnchor(CellAddress from, CellAddress to)
{
    m_definition.From = from;
    m_definition.To = to;
    return *this;
}

SlicerBuilder& SlicerBuilder::SetColumnCount(UInt32 columns)
{
    m_definition.ColumnCount = columns;
    return *this;
}

SlicerBuilder& SlicerBuilder::SetStyle(std::string style)
{
    m_definition.Style = std::move(style);
    return *this;
}

SlicerBuilder& SlicerBuilder::ShowCaption(bool show)
{
    m_definition.ShowCaption = show;
    return *this;
}

SlicerBuilder& SlicerBuilder::SetSortOrder(SlicerSortOrder order)
{
    m_definition.SortOrder = order;
    return *this;
}

SlicerBuilder& SlicerBuilder::SetCrossFilter(SlicerCrossFilter crossFilter)
{
    m_definition.CrossFilter = crossFilter;
    return *this;
}

SlicerBuilder& SlicerBuilder::SelectItems(std::vector<std::string> captions)
{
    m_definition.SelectedItems = std::move(captions);
    return *this;
}

SlicerBuilder& SlicerBuilder::WriteDrawing(bool write)
{
    m_definition.WriteDrawing = write;
    return *this;
}

ExcelSlicer::Ptr SlicerBuilder::Build()
{
    return m_sheet ? m_sheet->CreateSlicer(m_definition).Slicer : nullptr;
}

// ---------------------------------------------------------------------------
// Worksheet integration
// ---------------------------------------------------------------------------

SlicerCreationResult Worksheet::CreateSlicer(const ExcelSlicerDefinition& definition)
{
    namespace Detail = SlicerDetail;
    namespace X14 = DocumentFormat::OpenXml::Office2010::Excel;
    const auto report = [](SlicerResult status) -> SlicerCreationResult
    {
        return SlicerCreationResult{std::move(status), nullptr};
    };

    if (!m_part || !m_document)
    {
        return report(Detail::Failure(SlicerError::InvalidWorksheet, "The worksheet is detached."));
    }
    const auto workbookPart = m_document->GetWorkbookPart();
    const auto worksheetRoot = GetLowLevelApi();
    if (!workbookPart || !worksheetRoot)
    {
        return report(Detail::Failure(SlicerError::InvalidWorksheet, "The workbook part is missing."));
    }
    if (auto status = Detail::ValidatePresentation(definition); !status)
    {
        return report(std::move(status));
    }

    SlicerResult status;
    const auto resolved = Detail::ResolveSource(m_document, definition, status);
    if (!resolved)
    {
        return report(std::move(status));
    }
    std::vector<bool> selection;
    if (!Detail::ResolveSelection(*resolved, definition.SelectedItems, selection, status))
    {
        return report(std::move(status));
    }
    const bool hasExplicitSelection = !definition.SelectedItems.empty();

    const auto slicerName = definition.Name.empty() ? Detail::MakeUniqueSlicerName(m_document) : definition.Name;
    if (!IsValidSlicerName(slicerName))
    {
        return report(Detail::Failure(SlicerError::InvalidName, "The slicer name is not a valid Excel object name."));
    }
    if (Detail::SlicerNameExists(m_document, slicerName, nullptr, {}))
    {
        return report(Detail::Failure(SlicerError::InvalidName,
                                      "The slicer name '" + slicerName + "' is already used in this workbook."));
    }

    // Two slicers over the same source column share one cache, which is what
    // keeps them synchronized in Excel.
    auto cachePart = Detail::FindSharedCachePart(m_document, *resolved);
    const bool createdCache = cachePart == nullptr;
    std::string cacheName;
    if (createdCache)
    {
        cacheName = Detail::MakeUniqueCacheName(m_document, resolved->sourceName);
        cachePart = workbookPart->AddSlicerCachePart();
    }
    else
    {
        const auto root = cachePart->GetSlicerCacheDefinition();
        cacheName = root ? root->GetName().ToString() : std::string{};
    }

    const auto existingSlicersParts = m_part->GetSlicersParts();
    auto slicersPart = existingSlicersParts.empty() ? m_part->AddSlicersPart() : existingSlicersParts.front();
    const bool createdSlicersPart = existingSlicersParts.empty();

    bool addedWorkbookEntry = false;
    bool addedWorksheetReference = false;
    bool createdDrawingPart = false;
    std::shared_ptr<X14::Slicer> slicerElement;
    const auto originalWorkbookXml = workbookPart->GetXmlString();
    const auto originalWorksheetXml = m_part->GetXmlString();

    const auto rollback = [&]()
    {
        if (slicerElement)
        {
            if (const auto root = slicersPart->GetSlicers())
            {
                root->RemoveChild(slicerElement);
            }
        }
        if (createdDrawingPart)
        {
            Excel::Detail::UnlinkWorksheetDrawing(m_part);
            m_part->RemoveDrawingsPart();
        }
        else
        {
            Detail::RemoveSlicerAnchor(m_part, slicerName);
        }
        if (addedWorksheetReference)
        {
            m_part->SetXmlString(originalWorksheetXml);
        }
        if (createdSlicersPart && slicersPart)
        {
            m_part->RemoveSlicersPart(slicersPart);
        }
        if (addedWorkbookEntry)
        {
            workbookPart->SetXmlString(originalWorkbookXml);
        }
        if (createdCache && cachePart)
        {
            workbookPart->RemoveSlicerCachePart(cachePart);
        }
    };

    if (!cachePart || !slicersPart || cacheName.empty())
    {
        rollback();
        return report(Detail::Failure(SlicerError::WriteFailed, "The slicer parts could not be created."));
    }

    if (createdCache)
    {
        if (!Detail::WriteCacheDefinition(cachePart, definition, *resolved, selection, hasExplicitSelection, cacheName))
        {
            rollback();
            return report(Detail::Failure(SlicerError::WriteFailed, "The slicer cache could not be written."));
        }
        if (!Detail::RegisterWorkbookCache(m_document, cachePart, resolved->kind))
        {
            rollback();
            return report(
                Detail::Failure(SlicerError::WriteFailed, "The workbook slicer cache registry could not be updated."));
        }
        addedWorkbookEntry = true;
    }
    if (!Detail::ApplyTableSelection(*resolved, selection, hasExplicitSelection))
    {
        rollback();
        return report(Detail::Failure(SlicerError::WriteFailed, "The table filter could not be updated."));
    }

    if (!Detail::RegisterWorksheetSlicerList(worksheetRoot, slicersPart, addedWorksheetReference))
    {
        rollback();
        return report(Detail::Failure(SlicerError::WriteFailed, "The worksheet slicer list could not be updated."));
    }

    const auto slicersRoot = slicersPart->GetSlicers();
    slicerElement = slicersRoot ? slicersRoot->AppendChild<X14::Slicer>() : nullptr;
    if (!slicerElement)
    {
        rollback();
        return report(Detail::Failure(SlicerError::WriteFailed, "The slicer element could not be created."));
    }
    const auto caption = definition.Caption.empty() ? resolved->sourceName : definition.Caption;
    Detail::WriteSlicerElement(slicerElement, definition, slicerName, cacheName, caption);

    if (definition.WriteDrawing)
    {
        auto drawing = m_part->GetDrawingsPart();
        if (!drawing)
        {
            drawing = m_part->AddDrawingsPart();
            if (!drawing || !Excel::Detail::LinkWorksheetDrawing(m_part, drawing->RelationshipId()))
            {
                rollback();
                return report(Detail::Failure(SlicerError::WriteFailed, "The worksheet drawing could not be created."));
            }
            createdDrawingPart = true;
        }
        const auto drawingRoot = drawing->GetWorksheetDrawing();
        const auto drawingId = definition.Id != 0 ? definition.Id : Detail::MaxDrawingId(drawingRoot) + 1;
        if (!Detail::AppendSlicerAnchor(drawingRoot, definition, slicerName, drawingId))
        {
            rollback();
            return report(Detail::Failure(SlicerError::WriteFailed, "The slicer shape could not be written."));
        }
    }

    return SlicerCreationResult{Detail::Success(), std::make_shared<ExcelSlicer>(slicersPart, slicerName, m_document)};
}

std::vector<ExcelSlicer::Ptr> Worksheet::Slicers() const
{
    std::vector<ExcelSlicer::Ptr> result;
    if (!m_part || !m_document)
    {
        return result;
    }
    for (const auto& part : m_part->GetSlicersParts())
    {
        for (const auto& slicer : SlicerDetail::SlicerElements(part))
        {
            if (slicer)
            {
                result.push_back(std::make_shared<ExcelSlicer>(part, slicer->GetName().ToString(), m_document));
            }
        }
    }
    return result;
}

ExcelSlicer::Ptr Worksheet::SlicerByName(std::string_view name) const
{
    for (const auto& slicer : Slicers())
    {
        if (slicer && AsciiText::EqualsIgnoreCase(slicer->Name(), name))
        {
            return slicer;
        }
    }
    return nullptr;
}

bool Worksheet::RemoveSlicer(const ExcelSlicer::Ptr& slicer)
{
    namespace Detail = SlicerDetail;
    if (!slicer || !m_part || !m_document)
    {
        return false;
    }
    const auto slicersPart = slicer->GetPart();
    const auto parts = m_part->GetSlicersParts();
    if (!slicersPart || std::find(parts.begin(), parts.end(), slicersPart) == parts.end())
    {
        return false;
    }
    const auto element = slicer->GetLowLevelApi();
    const auto slicersRoot = slicersPart->GetSlicers();
    if (!element || !slicersRoot)
    {
        return false;
    }

    const auto name = element->GetName().ToString();
    const auto cacheName = element->GetCache().ToString();
    const auto cachePart = slicer->GetCachePart();
    const auto tableCache = cachePart && cachePart->GetSlicerCacheDefinition()
                                ? Detail::TableSlicerCacheOf(cachePart->GetSlicerCacheDefinition())
                                : nullptr;
    const auto tableId = tableCache ? tableCache->GetTableId().ValueOr(0) : 0;
    const auto tableColumnId = tableCache ? tableCache->GetColumn().ValueOr(0) : 0;

    Detail::RemoveSlicerAnchor(m_part, name);
    if (!slicersRoot->RemoveChild(element))
    {
        return false;
    }

    if (Detail::SlicerElements(slicersPart).empty())
    {
        Detail::UnregisterWorksheetSlicerList(GetLowLevelApi(), slicersPart->RelationshipId());
        m_part->RemoveSlicersPart(slicersPart);
    }

    if (!Detail::CacheIsInUse(m_document, cacheName, nullptr))
    {
        if (tableCache)
        {
            // Drop the auto-filter this slicer owned, so the table is not left
            // filtered by a slicer that no longer exists.
            for (const auto& table : Detail::AllTables(m_document))
            {
                if (!table || table->Id() != tableId)
                {
                    continue;
                }
                const auto columns = table->Columns();
                for (Size index = 0; index < columns.size(); ++index)
                {
                    if (columns[index].Id == tableColumnId)
                    {
                        table->RemoveValueFilter(static_cast<UInt32>(index));
                        break;
                    }
                }
                break;
            }
        }
        Detail::UnregisterWorkbookCache(m_document, cachePart);
        if (const auto workbookPart = m_document->GetWorkbookPart(); workbookPart && cachePart)
        {
            workbookPart->RemoveSlicerCachePart(cachePart);
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Cross-feature maintenance called by the pivot table and table editors
// ---------------------------------------------------------------------------

namespace SlicerDetail
{

/** Removes every slicer built on @p cachePart, together with the cache itself. */
void RemoveSlicerChain(const ExcelDocument::Ptr& document, const std::shared_ptr<Packaging::SlicerCachePart>& cachePart)
{
    const auto root = cachePart ? cachePart->GetSlicerCacheDefinition() : nullptr;
    const auto workbookPart = document ? document->GetWorkbookPart() : nullptr;
    if (!root || !workbookPart)
    {
        return;
    }
    const auto cacheName = root->GetName().ToString();

    ExcelDocumentEditor editor(document);
    for (const auto& sheet : editor.Worksheets())
    {
        if (!sheet)
        {
            continue;
        }
        for (const auto& slicer : sheet->Slicers())
        {
            const auto element = slicer ? slicer->GetLowLevelApi() : nullptr;
            if (element && AsciiText::EqualsIgnoreCase(element->GetCache().ToString(), cacheName))
            {
                sheet->RemoveSlicer(slicer);
            }
        }
    }
    // RemoveSlicer already drops an unused cache; this covers a cache that no
    // slicer referenced in the first place.
    const auto remaining = workbookPart->GetSlicerCacheParts();
    if (std::find(remaining.begin(), remaining.end(), cachePart) != remaining.end())
    {
        UnregisterWorkbookCache(document, cachePart);
        workbookPart->RemoveSlicerCachePart(cachePart);
    }
}

void DetachPivotTableFromSlicers(const ExcelDocument::Ptr& document, std::string_view pivotTableName)
{
    namespace X14 = DocumentFormat::OpenXml::Office2010::Excel;
    const auto workbookPart = document ? document->GetWorkbookPart() : nullptr;
    if (!workbookPart || pivotTableName.empty())
    {
        return;
    }
    std::vector<std::shared_ptr<Packaging::SlicerCachePart>> orphaned;
    for (const auto& cachePart : workbookPart->GetSlicerCacheParts())
    {
        const auto root = cachePart ? cachePart->GetSlicerCacheDefinition() : nullptr;
        const auto pivotTables = root ? root->GetFirstChildOfType<X14::SlicerCachePivotTables>() : nullptr;
        if (!pivotTables)
        {
            continue;
        }
        for (const auto& entry : pivotTables->Elements<X14::SlicerCachePivotTable>())
        {
            if (entry && AsciiText::EqualsIgnoreCase(entry->GetName().ToString(), pivotTableName))
            {
                pivotTables->RemoveChild(entry);
            }
        }
        if (pivotTables->Elements<X14::SlicerCachePivotTable>().empty())
        {
            orphaned.push_back(cachePart);
        }
    }
    for (const auto& cachePart : orphaned)
    {
        RemoveSlicerChain(document, cachePart);
    }
}

void DetachTableFromSlicers(const ExcelDocument::Ptr& document, UInt32 tableId)
{
    const auto workbookPart = document ? document->GetWorkbookPart() : nullptr;
    if (!workbookPart || tableId == 0)
    {
        return;
    }
    std::vector<std::shared_ptr<Packaging::SlicerCachePart>> orphaned;
    for (const auto& cachePart : workbookPart->GetSlicerCacheParts())
    {
        const auto root = cachePart ? cachePart->GetSlicerCacheDefinition() : nullptr;
        const auto tableCache = root ? TableSlicerCacheOf(root) : nullptr;
        if (tableCache && tableCache->GetTableId().ValueOr(0) == tableId)
        {
            orphaned.push_back(cachePart);
        }
    }
    for (const auto& cachePart : orphaned)
    {
        RemoveSlicerChain(document, cachePart);
    }
}

void RenamePivotTableInSlicers(const ExcelDocument::Ptr& document,
                               std::string_view oldName,
                               std::string_view newName)
{
    namespace X14 = DocumentFormat::OpenXml::Office2010::Excel;
    const auto workbookPart = document ? document->GetWorkbookPart() : nullptr;
    if (!workbookPart || oldName.empty() || newName.empty())
    {
        return;
    }
    for (const auto& cachePart : workbookPart->GetSlicerCacheParts())
    {
        const auto root = cachePart ? cachePart->GetSlicerCacheDefinition() : nullptr;
        const auto pivotTables = root ? root->GetFirstChildOfType<X14::SlicerCachePivotTables>() : nullptr;
        if (!pivotTables)
        {
            continue;
        }
        for (const auto& entry : pivotTables->Elements<X14::SlicerCachePivotTable>())
        {
            if (entry && AsciiText::EqualsIgnoreCase(entry->GetName().ToString(), oldName))
            {
                entry->SetName(StringValue(newName));
            }
        }
    }
}

void RefreshSlicersForPivotTable(const ExcelDocument::Ptr& document, std::string_view pivotTableName)
{
    namespace X14 = DocumentFormat::OpenXml::Office2010::Excel;
    const auto workbookPart = document ? document->GetWorkbookPart() : nullptr;
    if (!workbookPart || pivotTableName.empty())
    {
        return;
    }
    ExcelDocumentEditor editor(document);
    for (const auto& sheet : editor.Worksheets())
    {
        if (!sheet)
        {
            continue;
        }
        for (const auto& slicer : sheet->Slicers())
        {
            if (!slicer || slicer->SourceKind() != SlicerSourceKind::PivotTable ||
                !AsciiText::EqualsIgnoreCase(slicer->SourceObjectName(), pivotTableName))
            {
                continue;
            }
            // Definition() reads the selection back as captions, so passing it
            // straight into Update() renumbers the item indexes while keeping
            // the very same values selected.
            if (const auto definition = slicer->Definition())
            {
                slicer->Update(*definition);
            }
        }
    }
}

} // namespace SlicerDetail

} // namespace ExyokiOffice::Excel
