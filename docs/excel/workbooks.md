# Excel workbooks: lifecycle and document-level services

This chapter covers everything that concerns a workbook as a whole:
creating, opening, and saving; undo snapshots; document properties and
themes; VBA projects; and workbook-structure protection. Worksheet content
starts in [Worksheets](worksheets.md).

Everything shown here lives in one header:

```cpp
#include "ExyokiOffice/Excel/ExcelDocument.hpp"
using namespace ExyokiOffice::Excel;
```

## Creating, opening, and saving

```cpp
auto fresh    = ExcelDocumentEditor::CreateNew();             // new .xlsx
auto macro    = ExcelDocumentEditor::CreateNew(
                    SpreadsheetDocumentType::MacroEnabledWorkbook);
auto fromDisk = ExcelDocumentEditor::Open("existing.xlsx");   // nullptr on failure
auto fromBytes= ExcelDocumentEditor::Open(bytes);             // std::vector<uint8_t> or std::span

editor->SaveToFile("out.xlsx");             // atomic save by default
std::vector<Byte> blob = editor->SaveToMemory();
```

`Open` accepts an `OpenSettings` argument to control ZIP/XML safety limits and
validation behavior. The core default is unlimited unless the application
installs a policy; use `OpenXmlPackageLimits::Recommended()` for untrusted
input as shown in
[Opening untrusted packages safely](../introduction.md#opening-untrusted-packages-safely).
All factories return `nullptr` when the source cannot be read or parsed —
always check the result.
`SpreadsheetDocumentType` selects the package flavor: `Workbook` (`.xlsx`),
`MacroEnabledWorkbook` (`.xlsm`), `Template` (`.xltx`), and
`MacroEnabledTemplate` (`.xltm`).

Saving matches the Word editor: `SaveToFile(path, atomicSave,
cancellationToken)` publishes atomically by default, `SaveToMemory` returns
the complete package bytes, and both accept an optional cancellation token.

## Undo snapshots: mementos and transactions

The workbook editor offers the same snapshot mechanism as the Word editor:
`CreateMemento`/`RestoreMemento` capture and restore the complete package,
and `BeginTransaction` wraps them in a scope guard that rolls back unless
`Commit()` is called. A successful restore invalidates all previously
obtained worksheets and wrappers. See the
[Word documents chapter](../word/documents.md) for the pattern; the API is
identical.

## Document properties and themes

`Properties()` returns the unified core/extended/custom properties editor.
The theme API also mirrors Word: `ThemeSettings`/`SetThemeSettings` for the
typed essentials (scheme colors, fonts), `ThemeXml`/`SetThemeXml` for the
lossless DrawingML theme, and `EnsureTheme`/`RemoveTheme` for the part
lifecycle. Theme colors matter in Excel because `ExcelColor::Theme(...)`
styles resolve against them.

## VBA projects and macros

VBA projects are handled as opaque binary payloads. ExyokiOffice can
detect, extract, preserve, replace, and remove `vbaProject.bin`, but
deliberately does not parse VBA source or execute macros.

```cpp
auto editor = ExcelDocumentEditor::Open("input.xlsm");
if (editor && editor->HasVbaProject())
{
    auto originalProject = editor->GetVbaProjectData();
    editor->SetVbaProjectData(replacementProject);
    editor->SaveToFile("updated.xlsm");
}

editor->RemoveVbaProject();
editor->SaveToFile("macro-free.xlsx");
```

Adding a project automatically changes a workbook or template to its
macro-enabled package type. Removing it changes `.xlsm` back to `.xlsx` and
`.xltm` back to `.xltx`. The same methods are available directly on the
low-level `ExcelDocument`.

## Workbook protection

Workbook protection locks the workbook's *structure* (adding, removing,
renaming, or reordering sheets) and window layout — distinct from per-sheet
cell protection, which is covered in [Worksheets](worksheets.md):

```cpp
WorkbookProtectionOptions options;
options.LockStructure = true;

auto applied = editor->ProtectWorkbook(options, "admin");
auto state = editor->GetWorkbookProtection();
auto removed = editor->UnprotectWorkbook("admin");
```

Like every Office protection mechanism in this library, this is a
user-interface restriction with a password verifier, not encryption; the
result objects carry structured `WorkbookProtectionError` values.

## Escape hatch to the low-level API

`editor->GetLowLevelApi()` (an alias of `GetDocument()`) returns the
packaging-level `ExcelDocument` for direct part and relationship access, and
`Worksheet::GetLowLevelApi()` returns the generated SpreadsheetML
`Worksheet` element. `Worksheet::GetPart()` exposes the worksheet part
itself.

## The chapters

| Chapter | Covers |
| --- | --- |
| [Worksheets](worksheets.md) | Sheet management, copying between workbooks, sheet protection. |
| [Cells and ranges](cells.md) | Values, shared strings, ranges, structural edits, merged cells. |
| [Styles and number formats](styles.md) | `StyleRepository`, fonts, fills, borders, number formats. |
| [Named ranges](named-ranges.md) | `NamedRangeManager`, scopes, name resolution. |
| [Formulas](formulas.md) | The formula engine, supported functions, recalculation. |
| [Tables](tables.md) | Worksheet tables and auto-filters. |
| [Charts](charts.md) | `ChartBuilder`, series, cross-sheet data sources. |
| [Pivot tables](pivot-tables.md) | Caches, definitions, aggregation, refresh. |
| [Slicers](slicers.md) | Slicers over pivot tables and worksheet tables. |
| [Data validation and conditional formatting](validation.md) | Rules, operators, ranges. |
| [Layout and annotations](layout.md) | Row/column dimensions, views, hyperlinks, comments, images. |
| [Printing](printing.md) | Page setup, margins, print areas, headers and footers. |
