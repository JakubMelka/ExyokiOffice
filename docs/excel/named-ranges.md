# Excel named ranges

`NamedRangeManager` creates and manages SpreadsheetML defined names — the
workbook's `definedNames` collection — and the formula engine resolves them
during evaluation and recalculation.

The public API is one header:

```cpp
#include "ExyokiOffice/Excel/ExcelNamedRange.hpp"
```

## Quick start

```cpp
using namespace ExyokiOffice::Excel;

auto editor = ExcelDocumentEditor::CreateNew();
auto sheet = editor->FirstWorksheet();
sheet->SetCellNumber(1, 1, 10.0);                   // A1
sheet->SetCellNumber(2, 1, 32.0);                   // A2

NamedRangeManager names(editor->GetDocument());
names.Create("SalesData", SheetCellRange("Sheet1", *CellRange::ParseA1("A1:A2")));
names.CreateFromFormula("TaxRate", "=0.21");

sheet->SetCellFormula(*CellAddress::ParseA1("B1"), "=SUM(SalesData)*(1+TaxRate)");

FormulaEngine engine(editor->GetDocument());
engine.Recalculate();                               // B1 caches 50.82
editor->SaveToFile("report.xlsx");
```

`NamedRangeManager` follows the workbook-service pattern of
`SharedStringTableService`: a lightweight wrapper around a shared
`ExcelDocument`, valid while the document is alive.

## Scopes

A defined name is either **workbook-scoped** (visible everywhere) or
**sheet-scoped** (owned by one worksheet, stored with `localSheetId`):

- A formula resolves an unqualified name against the evaluating worksheet's
  sheet scope first, then the workbook scope — so a sheet-scoped name shadows
  a workbook-scoped name of the same spelling on its own sheet.
- A qualified reference such as `Report!LocalName` selects the sheet scope of
  `Report` explicitly (falling back to the workbook scope).
- Names are unique case-insensitively *within a scope*; the same spelling may
  exist once per sheet scope plus once at workbook scope.

`Find(name, scopeSheet)` addresses one exact scope (empty = workbook);
`Resolve(name, sheetName)` performs the formula-style two-step lookup.

## Naming rules

`NamedRangeManager::IsValidName` enforces Excel's syntax: non-empty, at most
255 characters, first character a letter, `_`, or `\`, remaining characters
letters, digits, `.`, `_`, or `\`, no spaces, not a cell-reference spelling
(`A1`, `R1C1`), and not the reserved single letters `C` or `R`. Non-ASCII
(UTF-8) letters are accepted.

## API overview

| Method | Purpose |
|---|---|
| `Create(name, range, scope, scopeSheet)` | Stores an absolute sheet-qualified range (`Sheet1!$A$1:$B$4`). Validates the name, scope sheet, target sheet, and per-scope uniqueness. |
| `CreateFromFormula(name, formula, ...)` | Stores arbitrary `refersTo` text — constants (`=0.21`), computed definitions (`=SUM(...)`), or references to other names. |
| `Find` / `Resolve` / `GetRange` / `List` / `Count` | Queries; `NamedRange::Range()` parses the definition back into a `SheetCellRange` when it is a plain range. |
| `SetRange` / `SetFormula` | Replace an existing definition. |
| `Rename` | Renames within a scope (formulas referencing the old spelling are not rewritten). |
| `Remove` | Deletes the entry; the last removal also prunes the empty `definedNames` element. |

All mutations return `NamedRangeResult` (`NamedRangeError` + message) in the
project's structured-result convention.

## Formula engine integration

- Evaluation (`EvaluateFormula`, `EvaluateCell`) resolves `NameReference`
  nodes through the two-step scope lookup. Range-valued names stay
  references, so `SUM(SalesData)` or `VLOOKUP(x, Table, 2)` behave exactly
  like inline ranges; constant and computed names evaluate their definition.
- Unqualified cell references inside a name's definition resolve against the
  worksheet the *formula* is evaluated on (definitions created by this API
  are always sheet-qualified and unaffected).
- `Recalculate()` tracks dependencies **through** names: a formula using
  `Doubled` (defined as `Sheet1!$B$1`) is ordered after the formula in `B1`.
  Cycles closed through names are detected and reported like direct ones.
- Cyclic name definitions (`Ping = Pong+1`, `Pong = Ping+1`) terminate with a
  `#VALUE!` error value instead of recursing.
- `ValidateFormula` reports names that are not defined in any scope.
- Unknown names still evaluate to `#NAME?`.

## Limitations

- Structural edits (row/column insertion or deletion, worksheet removal or
  reordering) do not rewrite defined-name formulas or stored sheet indexes.
- Hidden names round-trip (`NamedRange::hidden`) but the API does not create
  them.
- Function-type names (VBA/XLM `function` flags) are preserved as opaque
  entries.

## Testing

`tests/spreadsheet/ExcelNamedRangeTests.cpp` (label `excel-named-ranges`, tags
`[unit] [excel] [excel-named-range]`) covers naming-rule validation, per-scope uniqueness and
shadowing, CRUD round-trips through `SaveToMemory`/`Open`, formula evaluation
through names (including name-to-name chains and qualified scope selection),
`ValidateFormula` diagnostics, dependency-ordered recalculation through
names, and cycle detection both through cells and between name definitions.
