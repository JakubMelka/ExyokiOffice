# Excel slicers

A slicer is the button panel that filters a pivot table or a worksheet table.
ExyokiOffice creates the slicer definition, its cache, the workbook and
worksheet registry entries, and the visible shape in the worksheet drawing, so
a slicer created through this API is immediately usable in Excel.

```cpp
#include "ExyokiOffice/Excel/ExcelDocument.hpp"
```

The full API reference is the Doxygen documentation in
[ExcelSlicer.hpp](../../include/ExyokiOffice/Excel/ExcelSlicer.hpp).

## Hello world

```cpp
auto slicer = SlicerBuilder(reportSheet)
                  .SetName("RegionSlicer")
                  .SetPivotSource("SalesByRegion", "Region")
                  .SetAnchor(*CellAddress::ParseA1("F2"), *CellAddress::ParseA1("H12"))
                  .SetColumnCount(2)
                  .Build();
```

The equivalent explicit form gives access to the structured failure reason:

```cpp
ExcelSlicerDefinition definition;
definition.PivotTableName = "SalesByRegion";
definition.SourceField    = "Region";
definition.From           = *CellAddress::ParseA1("F2");
definition.To             = *CellAddress::ParseA1("H12");

auto created = reportSheet->CreateSlicer(definition);
if (!created)
{
    // created.Status.Error carries the code, created.Slicer is nullptr
    std::cerr << created.Status.Message << '\n';
}
auto slicer = created.Slicer;
```

## What gets created

| Artifact | Location |
|---|---|
| Slicer cache definition part | `xl/slicerCaches/slicerCache1.xml`, attached to the workbook part |
| Slicers part | `xl/slicers/slicer1.xml`, attached to the hosting worksheet part |
| Workbook registry | `x:workbook/x:extLst/x:ext` → `x14:slicerCaches` (pivot) or `x15:slicerCaches` (table) |
| Worksheet registry | `x:worksheet/x:extLst/x:ext` → `x14:slicerList/x14:slicer` with the slicers part's relationship id |
| Visible shape | `xdr:twoCellAnchor/xdr:graphicFrame` in the worksheet drawing, carrying `sle:slicer` |

One slicers part holds every slicer of a worksheet, and the worksheet registry
carries one entry per slicers part rather than per slicer.

The operation is atomic. On any validation or write failure the workbook is
left byte-for-byte as it was.

## Pivot slicers

A pivot slicer filters one field of a pivot table's cache. The pivot table may
live on any worksheet of the same workbook; only the slicer shape is placed on
the worksheet you call `CreateSlicer` on.

```cpp
ExcelSlicerDefinition definition;
definition.SourceKind     = SlicerSourceKind::PivotTable;   // the default
definition.PivotTableName = "SalesByRegion";
definition.SourceField    = "Region";                       // a pivot cache field
```

`SourceField` names a pivot **cache** field, which is the header of the
corresponding source column. Because the cache holds every source column, a
field that is not placed on any axis can still be sliced.

The cache stores the item list and the selection:

```xml
<x14:slicerCacheDefinition name="Slicer_Region" sourceName="Region">
  <x14:pivotTables><x14:pivotTable tabId="2" name="SalesByRegion"/></x14:pivotTables>
  <x14:data>
    <x14:tabular pivotCacheId="1" sortOrder="ascending" crossFilter="showItemsWithDataAtTop">
      <x14:items count="2"><x14:i x="0"/><x14:i x="1" s="1"/></x14:items>
    </x14:tabular>
  </x14:data>
</x14:slicerCacheDefinition>
```

## Table slicers

A table slicer filters a column of an `ExcelTable`. The Excel 2010 tabular
cache cannot describe it, because that markup requires a pivot cache
identifier, so a table slicer is expressed by an Excel 2013 extension inside
the cache and is registered through the `x15` workbook extension.

```cpp
ExcelSlicerDefinition definition;
definition.SourceKind  = SlicerSourceKind::Table;
definition.TableName   = "SalesTable";
definition.SourceField = "Quarter";                 // a table column name
```

```xml
<x14:slicerCacheDefinition name="Slicer_Quarter" sourceName="Quarter">
  <x14:extLst><ext uri="{2F2917AC-EB37-4324-AD4E-5DD8C200BD13}">
    <x15:tableSlicerCache tableId="1" column="2" sortOrder="ascending"
                          crossFilter="showItemsWithDataAtTop"/>
  </ext></x14:extLst>
</x14:slicerCacheDefinition>
```

`ExcelSlicerDefinition::showMissing` exists only in the Excel 2010 markup and
is therefore ignored for a table slicer.

## Items and selection

`ExcelSlicer::Items()` returns every button with its state, and
`SelectItems()` replaces the selection. Selection is expressed by **caption**,
never by index, because cache item indexes are renumbered whenever the source
is refreshed while captions survive.

```cpp
slicer->SelectItems({"East", "West"});   // filter to two values
slicer->SelectItems({});                 // select everything again
```

An empty selection is the unfiltered state, and Excel represents it by leaving
the selection attribute off every item rather than by marking each one
selected. As soon as a selection is explicit, both the selected (`s="1"`) and
the cleared (`s="0"`) items are written.

> **The two source kinds store the selection in different places.** A pivot
> slicer keeps it in the slicer cache. A table slicer keeps it in the table's
> auto-filter, which is where Excel puts it, so `SelectItems` on a table slicer
> creates or replaces the value filter of that table column and clearing the
> selection removes it. This asymmetry is Excel's design, not ExyokiOffice's.

Distinct table values are collected from the table's data rows, excluding the
header row, the totals row, and blank cells, and are ordered numbers-first then
case-insensitively, matching how pivot cache items are ordered.

## The visible shape

Unless `ExcelSlicerDefinition::writeDrawing` is disabled, `CreateSlicer` writes
a two-cell anchor into the worksheet drawing, creating the drawing part on
first use. The frame carries an `sle:slicer` element that refers to the slicer
by **name**, so renaming through `ExcelSlicer::SetName` rewrites the shape and
the frame's non-visual name together.

```cpp
slicer->SetAnchor(*CellAddress::ParseA1("J2"), *CellAddress::ParseA1("L14"));
auto placement = slicer->Anchor();   // std::optional<std::pair<CellAddress, CellAddress>>
```

**The shape requires Excel 2010 or newer.** Excel wraps slicer anchors in
`mc:AlternateContent` with an `mc:Fallback` picture so that Excel 2007 shows a
placeholder; ExyokiOffice writes the plain frame instead. Pre-2010
applications therefore ignore the shape. The filter itself is unaffected.

## Reading a slicer back

```cpp
for (const auto& slicer : sheet->Slicers())
{
    slicer->Name();               // "RegionSlicer"
    slicer->SourceKind();         // PivotTable or Table
    slicer->SourceObjectName();   // "SalesByRegion" or "SalesTable"
    slicer->SourceField();        // "Region"
    slicer->Items();              // captions with selection state
}

auto found = sheet->SlicerByName("RegionSlicer");   // case-insensitive
```

`Definition()` reconstructs the full `ExcelSlicerDefinition`, which can be
modified and passed back to `Update()`.

Slicers written by another producer are enumerated too. When their cache part
cannot be resolved the getters degrade to empty results instead of failing, and
`RemoveSlicer` still works.

## Updating

```cpp
auto definition = *slicer->Definition();
definition.ColumnCount = 3;
definition.CrossFilter = SlicerCrossFilter::None;
slicer->Update(definition);
```

`Update` keeps the package parts, the cache, and the relationships. Changing
`SourceKind`, `PivotTableName`, `TableName`, or `SourceField` is rejected with
`SlicerError::UnknownSource`; remove the slicer and create a new one instead.

Rebuilding a pivot table renumbers its cache items, so `ExcelPivotTable::Update`,
`Refresh`, and `SetSourceRange` re-derive every slicer built on that pivot
table, preserving the selection by caption. Renaming a pivot table updates the
name recorded in its slicer caches.

## Naming

The slicer name is workbook-unique, compared case-insensitively, and defaults
to a generated `SlicerN`. `IsValidSlicerName` applies the same rules the editor
enforces: non-empty, at most 255 characters, no leading or trailing space, and
no control characters.

The **cache** name is derived, not supplied: `Slicer_` plus the source column
with every character outside `[A-Za-z0-9_]` replaced by an underscore, and a
numeric suffix on collision. Excel exposes it in its object model, so it has to
remain a legal identifier.

## Cache sharing

Two slicers over the same column of the same source share one cache part, which
is what keeps them synchronized in Excel:

```cpp
sheet->CreateSlicer(first);    // creates Slicer_Region
sheet->CreateSlicer(second);   // reuses it
```

Sharing is keyed on the source kind, the identity of the backing object, and
the column — not on the column name alone, so a `Region` slicer over one pivot
table never shares with a `Region` slicer over another.

## Removing

```cpp
sheet->RemoveSlicer(slicer);
```

This removes the slicer element and its shape, and drops the drawing part when
it held nothing else. The cache part and its workbook registry entry are
retained while another slicer still uses them. Once the worksheet's last slicer
is gone, the slicers part, the `x14:slicerList` entry, and the containing
extension are removed as well. A table slicer also drops the auto-filter it
owned.

Removing the underlying object cleans up after itself:
`Worksheet::RemovePivotTable` and `Worksheet::RemoveTable` detach every slicer
that depended on it, rather than leaving a dangling reference that would make
Excel repair the package.

## Errors

`SlicerResult` pairs a `SlicerError` code with a diagnostic message. It is
returned directly by the mutating methods, and as the `Status` member of the
`SlicerCreationResult` that `Worksheet::CreateSlicer` returns.

| `SlicerError` | Meaning |
|---|---|
| `None` | The operation completed successfully. |
| `InvalidWorksheet` | The worksheet wrapper, workbook, or slicer part is detached. |
| `InvalidName` | The name is invalid or already used in this workbook. |
| `UnknownSource` | The named pivot table or table does not exist, or `Update` tried to change the source. |
| `UnknownField` | The source column is not a pivot cache field or a table column. |
| `InvalidAnchor` | The anchor cells are missing or the rectangle is inverted. |
| `InvalidPresentation` | `ColumnCount` is outside 1..20000, or `RowHeight` is zero. |
| `UnknownItem` | A selected caption does not occur in the source column. |
| `WriteFailed` | A package part, relationship, or XML write failed. |

## Escape hatch to the low-level DOM

```cpp
slicer->GetPart();          // Packaging::SlicersPart
slicer->GetCachePart();     // Packaging::SlicerCachePart
slicer->GetLowLevelApi();   // Office2010::Excel::Slicer

auto root = slicer->GetCachePart()->GetSlicerCacheDefinition();
```

Note that the qualified name `x14:slicer` denotes two different generated
classes: `Slicer` inside `x14:slicers` and `SlicerRef` inside `x14:slicerList`.
A typed lookup by either class matches the same node, so ask for the class that
matches the container you are reading.

## Known limitations

- The shape is written without an `mc:AlternateContent` fallback, so Excel 2007
  does not render it.
- OLAP slicer caches (`x14:olap`) are preserved but not created; only tabular
  pivot caches and table caches are authored.
- Slicer styles are referenced by name (`SlicerStyleLight1` by default). The
  `x14:slicerStyles` table itself is not created, so custom style definitions
  have to be supplied through the typed DOM.
- Multi-pivot "report connections" — one cache driving several pivot tables —
  are preserved but a new slicer always connects to a single pivot table.
- A slicer over a table column that a later `ExcelTable::Resize` drops is left
  in place with an empty item list rather than being removed.

The generated parts validate cleanly against `OpenXmlDomValidator`.
