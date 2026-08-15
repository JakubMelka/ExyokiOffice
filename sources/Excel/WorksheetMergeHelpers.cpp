// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "Excel/WorksheetMergeHelpers.hpp"

#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/Spreadsheet.hpp"
#include "ExyokiOffice/DOM/Namespaces.hpp"
#include "ExyokiOffice/MetadataBuilder.hpp"
#include "OpenXmlDomInternal.hpp"
#include "XmlNamespaceResolver.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ExyokiOffice::Excel
{

namespace Spreadsheet = ExyokiOffice::DocumentFormat::OpenXml::Spreadsheet;

class WorksheetMergeImplementation final
{
public:
    WorksheetMergeImplementation() = delete;

    static std::vector<CellRange> MergedRanges(const Worksheet& worksheet)
    {
        std::vector<CellRange> ranges;
        const auto root = worksheet.GetLowLevelApi();
        const auto mergeCells = FindMergeCells(root);
        for (const auto& mergeCell : FindChildren<Spreadsheet::MergeCell>(mergeCells))
        {
            const auto range = CellRange::ParseA1(mergeCell->GetReference().ToString());
            if (range)
            {
                ranges.push_back(*range);
            }
        }
        return ranges;
    }

    static RangeOperationResult MergeRange(Worksheet& worksheet, CellRange range)
    {
        if (!IsMergeable(range))
        {
            return Error(RangeOperationError::InvalidAddress,
                         "A merged range must be valid and contain at least two cells.");
        }
        const auto part = worksheet.GetPart();
        const auto root = worksheet.GetLowLevelApi();
        if (!part || !root)
        {
            return Error(RangeOperationError::InvalidWorksheet,
                         "The worksheet is not attached to a usable worksheet part.");
        }

        const auto mergeCells = FindMergeCells(root);
        for (const auto& mergeCell : FindChildren<Spreadsheet::MergeCell>(mergeCells))
        {
            const auto existing = CellRange::ParseA1(mergeCell->GetReference().ToString());
            if (!existing)
            {
                return Error(RangeOperationError::ReferenceUpdateFailed,
                             "The worksheet contains a malformed merged-cell reference.");
            }
            if (Overlaps(*existing, range))
            {
                return Error(RangeOperationError::OverlappingRange,
                             "The requested range overlaps an existing merged-cell range.");
            }
        }

        const auto originalXml = part->GetXmlString();
        Size removedCells = 0;
        for (const auto address : worksheet.StoredCellAddresses())
        {
            if (address.ToA1() == range.First().ToA1() || !Contains(range, address))
            {
                continue;
            }
            if (!worksheet.RemoveCell(address))
            {
                part->SetXmlString(originalXml);
                return Error(RangeOperationError::WriteFailed,
                             "A covered cell could not be removed; the original worksheet was restored.");
            }
            ++removedCells;
        }

        auto registry = FindMergeCells(root);
        if (!registry)
        {
            registry = CreateMergeCells(root);
        }
        auto mergeCell = AppendMergeCell(registry);
        if (!registry || !mergeCell)
        {
            part->SetXmlString(originalXml);
            return Error(RangeOperationError::WriteFailed,
                         "The merged-cell registry could not be created; the original worksheet was restored.");
        }
        mergeCell->SetReference(StringValue(range.ToA1()));
        UpdateCount(registry);
        return RangeOperationResult{RangeOperationError::None, {}, removedCells};
    }

    static RangeOperationResult UnmergeRange(Worksheet& worksheet, CellRange range)
    {
        if (!IsMergeable(range))
        {
            return Error(RangeOperationError::InvalidAddress,
                         "An unmerge range must be valid and contain at least two cells.");
        }
        const auto part = worksheet.GetPart();
        const auto root = worksheet.GetLowLevelApi();
        if (!part || !root)
        {
            return Error(RangeOperationError::InvalidWorksheet,
                         "The worksheet is not attached to a usable worksheet part.");
        }
        const auto registry = FindMergeCells(root);
        if (!registry)
        {
            return Error(RangeOperationError::RangeNotFound,
                         "No merged-cell registry entry matches the requested range.");
        }

        std::shared_ptr<Spreadsheet::MergeCell> match;
        for (const auto& mergeCell : FindChildren<Spreadsheet::MergeCell>(registry))
        {
            const auto existing = CellRange::ParseA1(mergeCell->GetReference().ToString());
            if (!existing)
            {
                return Error(RangeOperationError::ReferenceUpdateFailed,
                             "The worksheet contains a malformed merged-cell reference.");
            }
            if (existing->ToA1() == range.ToA1())
            {
                match = mergeCell;
                break;
            }
        }
        if (!match)
        {
            return Error(RangeOperationError::RangeNotFound,
                         "No merged-cell registry entry exactly matches the requested range.");
        }

        const auto originalXml = part->GetXmlString();
        if (!registry->RemoveChild(match))
        {
            return Error(RangeOperationError::WriteFailed, "The merged-cell registry entry could not be removed.");
        }
        const auto remaining = FindChildren<Spreadsheet::MergeCell>(registry);
        if (remaining.empty())
        {
            if (!root->RemoveChild(registry))
            {
                part->SetXmlString(originalXml);
                return Error(RangeOperationError::WriteFailed,
                             "The empty merged-cell registry could not be removed; the worksheet was restored.");
            }
        }
        else
        {
            UpdateCount(registry);
        }
        return RangeOperationResult{RangeOperationError::None, {}, 1};
    }

private:
    static RangeOperationResult Error(RangeOperationError error, std::string message)
    {
        return RangeOperationResult{error, std::move(message), 0};
    }

    static bool IsMergeable(CellRange range) noexcept
    {
        return range.IsValid() && (range.RowCount() > 1 || range.ColumnCount() > 1);
    }

    static bool Contains(CellRange range, CellAddress address) noexcept
    {
        return address.IsValid() && address.Row().Value() >= range.First().Row().Value() &&
               address.Row().Value() <= range.Last().Row().Value() &&
               address.Column().Value() >= range.First().Column().Value() &&
               address.Column().Value() <= range.Last().Column().Value();
    }

    static bool Overlaps(CellRange left, CellRange right) noexcept
    {
        return left.First().Row().Value() <= right.Last().Row().Value() &&
               right.First().Row().Value() <= left.Last().Row().Value() &&
               left.First().Column().Value() <= right.Last().Column().Value() &&
               right.First().Column().Value() <= left.Last().Column().Value();
    }

    static bool NodeHasName(const Pugi::xml_node& node, const OpenXmlQualifiedName& expected)
    {
        const std::string_view qualifiedName(node.name());
        const auto separator = qualifiedName.find(':');
        const auto prefix = separator == std::string_view::npos ? std::string_view{} : qualifiedName.substr(0, separator);
        const auto localName =
            separator == std::string_view::npos ? qualifiedName : qualifiedName.substr(separator + 1);
        if (localName != expected.localName())
        {
            return false;
        }
        const auto namespaceUri = Xml::NamespaceResolver::LookupUriForPrefix(node, prefix);
        return namespaceUri && *namespaceUri == expected.namespaceUri();
    }

    template <typename T>
    static std::vector<std::shared_ptr<T>> FindChildren(const std::shared_ptr<OpenXMLElement>& parent)
    {
        std::vector<std::shared_ptr<T>> result;
        const auto parentNode = Detail::NodeOf(parent);
        for (auto child = parentNode.first_child(); child; child = child.next_sibling())
        {
            if (child.type() == Pugi::node_element && NodeHasName(child, T::StaticMetaClass()->QualifiedName()))
            {
                result.push_back(Detail::WrapNode<T>(child));
            }
        }
        return result;
    }

    static std::shared_ptr<Spreadsheet::MergeCells> FindMergeCells(
        const std::shared_ptr<Spreadsheet::Worksheet>& root)
    {
        const auto children = FindChildren<Spreadsheet::MergeCells>(root);
        return children.empty() ? nullptr : children.front();
    }

    static std::string QualifiedElementName(Pugi::xml_node& context, const OpenXmlQualifiedName& name)
    {
        std::optional<std::string_view> suggestedPrefix;
        if (const auto suggested = OpenXml::Features::OpenXmlNamespaceResolver::getPrefixForUrl(name.namespaceUri()))
        {
            suggestedPrefix = *suggested;
        }
        const auto prefix = Xml::NamespaceResolver::EnsurePrefix(context, name.namespaceUri(), suggestedPrefix);
        return prefix.empty() ? std::string(name.localName())
                              : prefix + ":" + std::string(name.localName());
    }

    static void FlattenParticle(const MetadataParticlePtr& particle, std::vector<OpenXmlQualifiedName>& order)
    {
        if (!particle)
        {
            return;
        }
        if (particle->Kind() == MetadataParticleKind::Element)
        {
            order.push_back(static_cast<const MetadataElementParticle&>(*particle).Element());
            return;
        }
        const auto composite = std::dynamic_pointer_cast<MetadataCompositeParticle>(particle);
        if (!composite)
        {
            return;
        }
        for (const auto& child : composite->Children())
        {
            FlattenParticle(child, order);
        }
    }

    static std::shared_ptr<Spreadsheet::MergeCells> CreateMergeCells(
        const std::shared_ptr<Spreadsheet::Worksheet>& root)
    {
        auto rootNode = Detail::NodeOf(root);
        if (!rootNode)
        {
            return nullptr;
        }
        std::vector<OpenXmlQualifiedName> order;
        FlattenParticle(Spreadsheet::Worksheet::StaticMetaClass()->GetMetadata()->ParticleTree(), order);
        const auto targetName = Spreadsheet::MergeCells::StaticMetaClass()->QualifiedName();
        const auto target = std::find(order.begin(), order.end(), targetName);
        if (target == order.end())
        {
            return nullptr;
        }
        const auto targetIndex = static_cast<Size>(std::distance(order.begin(), target));
        Pugi::xml_node before;
        for (auto child = rootNode.first_child(); child; child = child.next_sibling())
        {
            for (Size index = targetIndex + 1; index < order.size(); ++index)
            {
                if (NodeHasName(child, order[index]))
                {
                    before = child;
                    break;
                }
            }
            if (before)
            {
                break;
            }
        }
        const auto qualifiedName = QualifiedElementName(rootNode, targetName);
        const auto node = before ? rootNode.insert_child_before(qualifiedName.c_str(), before)
                                 : rootNode.append_child(qualifiedName.c_str());
        return Detail::WrapNode<Spreadsheet::MergeCells>(node);
    }

    static std::shared_ptr<Spreadsheet::MergeCell> AppendMergeCell(
        const std::shared_ptr<Spreadsheet::MergeCells>& registry)
    {
        auto registryNode = Detail::NodeOf(registry);
        if (!registryNode)
        {
            return nullptr;
        }
        const auto name =
            QualifiedElementName(registryNode, Spreadsheet::MergeCell::StaticMetaClass()->QualifiedName());
        return Detail::WrapNode<Spreadsheet::MergeCell>(registryNode.append_child(name.c_str()));
    }

    static void UpdateCount(const std::shared_ptr<Spreadsheet::MergeCells>& registry)
    {
        registry->SetCount(
            UInt32Value(static_cast<UInt32>(FindChildren<Spreadsheet::MergeCell>(registry).size())));
    }
};

std::vector<CellRange> WorksheetMergeHelpers::MergedRanges(const Worksheet& worksheet)
{
    return WorksheetMergeImplementation::MergedRanges(worksheet);
}

RangeOperationResult WorksheetMergeHelpers::MergeRange(Worksheet& worksheet, CellRange range)
{
    return WorksheetMergeImplementation::MergeRange(worksheet, range);
}

RangeOperationResult WorksheetMergeHelpers::UnmergeRange(Worksheet& worksheet, CellRange range)
{
    return WorksheetMergeImplementation::UnmergeRange(worksheet, range);
}

} // namespace ExyokiOffice::Excel
