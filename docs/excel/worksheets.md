# Worksheets

A workbook is a list of worksheets. This chapter covers managing that list —
adding, finding, renaming, moving, copying (including across workbooks), and
removing sheets — plus per-sheet protection.

```cpp
#include "ExyokiOffice/Excel/ExcelDocument.hpp"
using namespace ExyokiOffice::Excel;
```

## Managing the sheet list

```cpp
auto sheet   = editor->AddWorksheet("Sales");
auto first   = editor->FirstWorksheet();
auto byName  = editor->GetWorksheet("Sales");
auto byIndex = editor->GetWorksheet(0);

editor->RenameWorksheet(0, "Q1 Sales");
editor->MoveWorksheet(0, 1);
auto copy = editor->CopyWorksheet(0, "Q1 Sales (copy)");
editor->RemoveWorksheet(1);

for (const auto& worksheet : editor->Worksheets())
{
    // ...
}
```

Worksheet names follow Excel's rules: non-empty, at most 31 characters, no
`: \ / ? * [ ]`, and unique case-insensitively. Removing the last worksheet
is rejected — a workbook always keeps at least one. `CopyWorksheet` derives
a unique name automatically when none is supplied.

`CopyWorksheet` copies the sheet's whole part graph, not just its XML:
drawings with their charts, images (shared, not duplicated), notes with their
VML boxes, threaded comments, tables, printer settings and external
hyperlinks all come along with valid relationships. What is identified
workbook-wide is renamed on the way — the copied tables get new ids and
names, the copied threads new ids — and the sheet-scoped defined names (print
area, print titles) are duplicated for the copy. Pivot tables on the copy share
the original's cache. A slicer on the copied sheet keeps its name, which
Excel treats as a duplicate; remove or rename it after the copy.

## Copying a sheet from another workbook

```cpp
auto source = ExcelDocumentEditor::Open("template.xlsx");
auto copied = editor->CopyWorksheetFrom(*source, 0, "Imported");
```

`CopyWorksheetFrom` imports the worksheet's complete graph — cells and
worksheet-owned parts — allocating fresh identifiers in the destination
workbook. Everything that is numbered per workbook is translated: shared
strings are re-indexed, cell and differential style indices are registered in
the destination stylesheet (so a bold cell stays bold whatever the two
catalogs look like), table ids and names are made unique, and the persons a
threaded comment names are merged into the destination's person list. For
appending whole workbooks to one another from the command line, see
[exyoki](../tools/exyoki.md) `merge`.

## Worksheet protection

Worksheet protection controls editing in Excel-compatible applications; it
does not encrypt cell data. Permission names are positive, even though OOXML
stores the corresponding attributes as inverse "locked" flags:

```cpp
SheetProtectionOptions permissions;
permissions.AllowAutoFilter = true;
permissions.AllowSort = true;
permissions.AllowSelectUnlockedCells = true;

auto protectedResult = sheet->Protect(permissions, "review");
auto state = sheet->GetProtection();       // options plus hasPassword
auto unprotectedResult = sheet->Unprotect("review");
```

Passwords use Excel's interoperable legacy worksheet verifier and are
limited to 15 bytes. The API validates a password before removing protection
and reports structured `SheetProtectionError` values. This verifier is an
accidental-editing safeguard, not a cryptographic security mechanism.

Which cells a protected sheet still allows editing is a *style* property:
set `ExcelProtection{false, false}` (unlocked, not hidden) on the styles of
the editable cells — see [Styles and number formats](styles.md). Protecting
the workbook's sheet *list* rather than cell content is workbook protection,
covered in [Workbooks](workbooks.md).
