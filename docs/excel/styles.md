# Styles and number formats

Excel styles are workbook-global: fonts, fills, borders, number formats, and
alignment records live in the stylesheet, and each cell references one
combined record (an "XF") by index. `StyleRepository` manages that catalog
so you work with descriptive values instead of raw indices.

```cpp
#include "ExyokiOffice/Excel/ExcelDocument.hpp"
using namespace ExyokiOffice::Excel;
```

## Registering and applying styles

`StyleRepository` (via `editor->Styles()`) deduplicates fonts, fills,
borders, number formats, and the resulting cell XF against the workbook's
actual stylesheet, so registering an equal style twice never grows the file:

```cpp
auto styles = editor->Styles();

ExcelStyle headerStyle;
headerStyle.Font = ExcelFont{.Color = ExcelColor::Rgb("FFFFFFFF"), .Bold = true};
headerStyle.Fill = ExcelFill{.Kind = ExcelFillKind::Pattern,
                             .Pattern = ExcelFillPattern::Solid,
                             .Foreground = ExcelColor::Rgb("FF4472C4")};
auto result = styles.GetOrAdd(headerStyle);         // StyleRegistrationResult
styles.ApplyToRange(*sheet, *CellRange::ParseA1("A1:D1"), result.StyleIndex);
styles.ApplyToCell(*sheet, *CellAddress::ParseA1("F1"), result.StyleIndex);
```

`ExcelStyle` covers:

- **Fonts** — name, size, bold/italic/underline/strike, color, theme font
  schemes.
- **Colors** — explicit sRGB (`ExcelColor::Rgb`, ARGB hex), theme slots with
  tint, and legacy indexed colors.
- **Fills** — pattern fills (every SpreadsheetML pattern) and gradient
  fills.
- **Borders** — every edge (including diagonals) with every line style.
- **Alignment** — horizontal, vertical, wrap, rotation, indent.
- **Protection** — the per-cell locked/hidden flags worksheet protection
  respects (see [Worksheets](worksheets.md)).

## Number formats

Number-format factories avoid magic built-in IDs, and custom codes remain
available for locale- or domain-specific display:

```cpp
ExcelStyle amount;
amount.NumberFormat = *ExcelNumberFormat::Accounting("EUR", 2);
amount.Protection = ExcelProtection{false, false}; // editable on a protected sheet
auto amountStyle = styles.GetOrAdd(amount);
styles.ApplyToRange(*sheet, *CellRange::ParseA1("D2:D100"), amountStyle.StyleIndex);

ExcelStyle probability;
probability.NumberFormat = ExcelNumberFormat::PercentDecimal();

ExcelStyle custom;
custom.NumberFormat = *ExcelNumberFormat::Custom("0.00\" kg\"");
```

Remember that a number format is *display only*: the stored cell value stays
a plain number, and dates are numbers with a date format applied.

## Reading styles back

`GetStyle(styleIndex)` is the inverse of `GetOrAdd`: it reconstructs an
`ExcelStyle` from the stylesheet, and `GetCellStyle(worksheet, address)`
does the same for the style a particular cell uses. Re-registering the
returned definition yields the same index, which makes "format this cell
like that one" a two-liner:

```cpp
auto model = styles.GetCellStyle(*sheet, *CellAddress::ParseA1("A1"));
if (model)
{
    model->Font->Italic = true;
    auto derived = styles.GetOrAdd(*model);
    styles.ApplyToCell(*sheet, *CellAddress::ParseA1("A2"), derived.StyleIndex);
}
```

## Related topics

Conditional formatting references *differential* formats (`dxfs`), a
separate catalog this API preserves but does not author — see
[Data validation and conditional formatting](validation.md). Row heights and
column widths are dimensions, not styles — see
[Layout and annotations](layout.md).
