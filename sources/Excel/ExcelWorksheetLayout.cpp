// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "ExyokiOffice/Excel/ExcelDocument.hpp"

#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/Spreadsheet.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <algorithm>
#include <cmath>

namespace ExyokiOffice::Excel
{
namespace Spreadsheet = ExyokiOffice::DocumentFormat::OpenXml::Spreadsheet;

class WorksheetLayoutHelpers final
{
public:
    static Spreadsheet::Row::Ptr FindRow(const Spreadsheet::Worksheet::Ptr& root, UInt32 index)
    {
        const auto data = root ? root->GetFirstChildOfType<Spreadsheet::SheetData>() : nullptr;
        if (!data)
        {
            return nullptr;
        }

        for (const auto& row : data->Elements<Spreadsheet::Row>())
        {
            if (row->GetRowIndex().ValueOr(0) == index)
            {
                return row;
            }
        }

        return nullptr;
    }

    static Spreadsheet::Row::Ptr EnsureRow(const Spreadsheet::Worksheet::Ptr& root, UInt32 index)
    {
        auto data = root->GetFirstChildOfType<Spreadsheet::SheetData>();
        if (!data)
        {
            data = root->AppendChild<Spreadsheet::SheetData>();
        }

        for (const auto& row : data->Elements<Spreadsheet::Row>())
        {
            const auto current = row->GetRowIndex().ValueOr(0);
            if (current == index)
            {
                return row;
            }

            if (current > index)
            {
                auto result = data->InsertChild<Spreadsheet::Row>(row);
                result->SetRowIndex(UInt32Value(index));
                return result;
            }
        }

        auto result = data->AppendChild<Spreadsheet::Row>();
        result->SetRowIndex(UInt32Value(index));
        return result;
    }

    static void CopyColumn(const Spreadsheet::Column::Ptr& from, const Spreadsheet::Column::Ptr& to)
    {
        to->SetWidth(from->GetWidth());
        to->SetStyle(from->GetStyle());
        to->SetHidden(from->GetHidden());
        to->SetBestFit(from->GetBestFit());
        to->SetCustomWidth(from->GetCustomWidth());
        to->SetPhonetic(from->GetPhonetic());
        to->SetOutlineLevel(from->GetOutlineLevel());
        to->SetCollapsed(from->GetCollapsed());
    }

    static bool HasNonDimensionColumnMetadata(const Spreadsheet::Column::Ptr& column)
    {
        return column->GetStyle().IsDefined() || column->GetBestFit().IsDefined() || column->GetPhonetic().IsDefined();
    }

    static Spreadsheet::SheetViews::Ptr EnsureViews(const Spreadsheet::Worksheet::Ptr& root)
    {
        auto views = root->GetFirstChildOfType<Spreadsheet::SheetViews>();
        if (views)
        {
            return views;
        }

        std::shared_ptr<OpenXMLElement> before = root->GetFirstChildOfType<Spreadsheet::SheetFormatProperties>();
        if (!before)
        {
            before = root->GetFirstChildOfType<Spreadsheet::Columns>();
        }
        if (!before)
        {
            before = root->GetFirstChildOfType<Spreadsheet::SheetData>();
        }

        return before ? root->InsertChild<Spreadsheet::SheetViews>(before)
                      : root->AppendChild<Spreadsheet::SheetViews>();
    }
};

bool IsValidRowDimension(const RowDimension& dimension) noexcept
{
    return dimension.OutlineLevel <= 7 && (!dimension.Height || (std::isfinite(*dimension.Height) &&
                                                                 *dimension.Height > 0 && *dimension.Height <= 409.5));
}

bool IsValidColumnDimension(const ColumnDimension& dimension) noexcept
{
    return dimension.OutlineLevel <= 7 &&
           (!dimension.Width || (std::isfinite(*dimension.Width) && *dimension.Width >= 0 && *dimension.Width <= 255));
}

bool IsValidWorksheetView(const WorksheetView& view) noexcept
{
    return (!view.ActiveCell || view.ActiveCell->IsValid()) && view.FrozenRows < MaxRowIndex &&
           view.FrozenColumns < MaxColumnIndex;
}

std::optional<RowDimension> Worksheet::GetRowDimension(UInt32 index) const
{
    if (index == 0 || index > MaxRowIndex)
    {
        return std::nullopt;
    }

    const auto row = WorksheetLayoutHelpers::FindRow(GetLowLevelApi(), index);
    if (!row)
    {
        return std::nullopt;
    }

    if (!row->GetHeight().IsDefined() && !row->GetHidden().IsDefined() && !row->GetOutlineLevel().IsDefined() &&
        !row->GetCollapsed().IsDefined())
    {
        return std::nullopt;
    }

    RowDimension result;
    if (row->GetHeight().IsDefined())
    {
        result.Height = row->GetHeight().Value();
    }

    result.Hidden = row->GetHidden().ValueOr(false);
    result.OutlineLevel = row->GetOutlineLevel().ValueOr(0);
    result.Collapsed = row->GetCollapsed().ValueOr(false);
    return result;
}

bool Worksheet::SetRowDimension(UInt32 index, const std::optional<RowDimension>& dimension)
{
    if (index == 0 || index > MaxRowIndex || (dimension && !IsValidRowDimension(*dimension)))
    {
        return false;
    }

    const auto root = GetLowLevelApi();
    if (!root)
    {
        return false;
    }

    auto row =
        dimension ? WorksheetLayoutHelpers::EnsureRow(root, index) : WorksheetLayoutHelpers::FindRow(root, index);
    if (!row)
    {
        return !dimension;
    }

    row->SetHeight(dimension && dimension->Height ? DoubleValue(*dimension->Height) : DoubleValue());
    row->SetCustomHeight(dimension && dimension->Height ? BooleanValue(true) : BooleanValue());
    row->SetHidden(dimension ? BooleanValue(dimension->Hidden) : BooleanValue());
    row->SetOutlineLevel(dimension ? ByteValue(dimension->OutlineLevel) : ByteValue());
    row->SetCollapsed(dimension ? BooleanValue(dimension->Collapsed) : BooleanValue());
    return true;
}

std::optional<ColumnDimension> Worksheet::GetColumnDimension(UInt32 index) const
{
    if (index == 0 || index > MaxColumnIndex)
    {
        return std::nullopt;
    }

    const auto root = GetLowLevelApi();
    const auto columns = root ? root->GetFirstChildOfType<Spreadsheet::Columns>() : nullptr;
    if (!columns)
    {
        return std::nullopt;
    }

    for (const auto& column : columns->Elements<Spreadsheet::Column>())
    {
        if (column->GetMin().ValueOr(0) > index || column->GetMax().ValueOr(0) < index)
        {
            continue;
        }

        if (!column->GetWidth().IsDefined() && !column->GetHidden().IsDefined() &&
            !column->GetOutlineLevel().IsDefined() && !column->GetCollapsed().IsDefined())
        {
            return std::nullopt;
        }

        ColumnDimension result;
        if (column->GetWidth().IsDefined())
        {
            result.Width = column->GetWidth().Value();
        }

        result.Hidden = column->GetHidden().ValueOr(false);
        result.OutlineLevel = column->GetOutlineLevel().ValueOr(0);
        result.Collapsed = column->GetCollapsed().ValueOr(false);
        return result;
    }

    return std::nullopt;
}

bool Worksheet::SetColumnDimension(UInt32 index, const std::optional<ColumnDimension>& dimension)
{
    if (index == 0 || index > MaxColumnIndex || (dimension && !IsValidColumnDimension(*dimension)))
    {
        return false;
    }

    const auto root = GetLowLevelApi();
    if (!root)
    {
        return false;
    }

    auto columns = root->GetFirstChildOfType<Spreadsheet::Columns>();
    if (!columns)
    {
        if (!dimension)
        {
            return true;
        }

        const auto data = root->GetFirstChildOfType<Spreadsheet::SheetData>();
        columns = data ? root->InsertChild<Spreadsheet::Columns>(data) : root->AppendChild<Spreadsheet::Columns>();
    }

    Spreadsheet::Column::Ptr target;
    for (const auto& column : columns->Elements<Spreadsheet::Column>())
    {
        if (column->GetMin().ValueOr(0) > index || column->GetMax().ValueOr(0) < index)
        {
            continue;
        }

        const auto first = column->GetMin().Value();
        const auto last = column->GetMax().Value();

        if (first < index)
        {
            auto left = columns->InsertChild<Spreadsheet::Column>(column);
            WorksheetLayoutHelpers::CopyColumn(column, left);
            left->SetMin(UInt32Value(first));
            left->SetMax(UInt32Value(index - 1));
        }

        if (last > index)
        {
            auto right = columns->InsertChild<Spreadsheet::Column>(column);
            WorksheetLayoutHelpers::CopyColumn(column, right);
            right->SetMin(UInt32Value(index + 1));
            right->SetMax(UInt32Value(last));
        }

        column->SetMin(UInt32Value(index));
        column->SetMax(UInt32Value(index));
        target = column;
        break;
    }

    if (!target && dimension)
    {
        target = columns->AppendChild<Spreadsheet::Column>();
        target->SetMin(UInt32Value(index));
        target->SetMax(UInt32Value(index));
    }

    if (target)
    {
        target->SetWidth(dimension && dimension->Width ? DoubleValue(*dimension->Width) : DoubleValue());
        target->SetCustomWidth(dimension && dimension->Width ? BooleanValue(true) : BooleanValue());
        target->SetHidden(dimension ? BooleanValue(dimension->Hidden) : BooleanValue());
        target->SetOutlineLevel(dimension ? ByteValue(dimension->OutlineLevel) : ByteValue());
        target->SetCollapsed(dimension ? BooleanValue(dimension->Collapsed) : BooleanValue());

        if (!dimension && !WorksheetLayoutHelpers::HasNonDimensionColumnMetadata(target))
        {
            columns->RemoveChild(target);
        }
    }

    if (columns->Elements<Spreadsheet::Column>().empty())
    {
        root->RemoveChild(columns);
    }

    return true;
}

WorksheetView Worksheet::GetView() const
{
    WorksheetView result;
    const auto root = GetLowLevelApi();
    const auto views = root ? root->GetFirstChildOfType<Spreadsheet::SheetViews>() : nullptr;
    const auto view = views ? views->GetFirstChildOfType<Spreadsheet::SheetView>() : nullptr;
    if (!view)
    {
        return result;
    }

    const auto selection = view->GetFirstChildOfType<Spreadsheet::Selection>();
    if (selection && selection->GetActiveCell().IsDefined())
    {
        result.ActiveCell = CellAddress::ParseA1(selection->GetActiveCell().ToString());
    }

    const auto pane = view->GetFirstChildOfType<Spreadsheet::Pane>();
    if (pane &&
        pane->GetState().ValueOr(Spreadsheet::PaneStateValues()).GetValue() == Spreadsheet::PaneStateValues::Frozen)
    {
        result.FrozenColumns = static_cast<UInt32>(pane->GetHorizontalSplit().ValueOr(0));
        result.FrozenRows = static_cast<UInt32>(pane->GetVerticalSplit().ValueOr(0));
    }

    return result;
}

bool Worksheet::SetView(const WorksheetView& definition)
{
    if (!IsValidWorksheetView(definition))
    {
        return false;
    }

    const auto root = GetLowLevelApi();
    if (!root)
    {
        return false;
    }

    const auto views = WorksheetLayoutHelpers::EnsureViews(root);
    auto view = views->GetFirstChildOfType<Spreadsheet::SheetView>();
    if (!view)
    {
        view = views->AppendChild<Spreadsheet::SheetView>();
        view->SetWorkbookViewId(UInt32Value(0));
    }

    auto selection = view->GetFirstChildOfType<Spreadsheet::Selection>();
    if (!selection)
    {
        selection = view->AppendChild<Spreadsheet::Selection>();
    }

    const auto active = definition.ActiveCell.value_or(*CellAddress::ParseA1("A1"));
    selection->SetActiveCell(StringValue(active.ToA1()));
    selection->SetSequenceOfReferences(ListValue<StringValue>(std::vector<StringValue>{StringValue(active.ToA1())}));

    if (const auto oldPane = view->GetFirstChildOfType<Spreadsheet::Pane>())
    {
        view->RemoveChild(oldPane);
    }

    if (definition.FrozenRows || definition.FrozenColumns)
    {
        auto pane = view->InsertChild<Spreadsheet::Pane>(selection);
        pane->SetState(EnumValue<Spreadsheet::PaneStateValues>(
            Spreadsheet::PaneStateValues(Spreadsheet::PaneStateValues::Frozen)));

        if (definition.FrozenColumns)
        {
            pane->SetHorizontalSplit(DoubleValue(definition.FrozenColumns));
        }
        if (definition.FrozenRows)
        {
            pane->SetVerticalSplit(DoubleValue(definition.FrozenRows));
        }

        // frozenRows/frozenColumns were bounds-checked by IsValidWorksheetView above.
        const CellAddress topLeft(RowIndex(definition.FrozenRows + 1), ColumnIndex(definition.FrozenColumns + 1));
        pane->SetTopLeftCell(StringValue(topLeft.ToA1()));

        using Pane = Spreadsheet::PaneValues;
        const auto activePane =
            definition.FrozenRows ? (definition.FrozenColumns ? Pane::BottomRight : Pane::BottomLeft) : Pane::TopRight;
        pane->SetActivePane(EnumValue<Pane>(Pane(activePane)));
        selection->SetPane(EnumValue<Pane>(Pane(activePane)));
    }
    else
    {
        selection->SetPane(EnumValue<Spreadsheet::PaneValues>());
    }

    return true;
}

} // namespace ExyokiOffice::Excel
