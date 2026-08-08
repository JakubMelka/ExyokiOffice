# Excel formula engine

The formula engine adds calculation semantics on top of the formula *storage*
layer (`Worksheet::SetCellFormula` / `GetCellFormula`, `CellFormulaValue`). It
parses and validates A1 formulas, evaluates them against workbook data with
Excel-compatible operator and coercion semantics, detects circular references,
and recalculates worksheets or whole workbooks in dependency order, writing
cached results so spreadsheet applications display up-to-date values on open.

The public API is one header:

```cpp
#include "ExyokiOffice/Excel/ExcelFormulaEngine.hpp"
```

## Quick start

```cpp
using namespace ExyokiOffice::Excel;

auto editor = ExcelDocumentEditor::CreateNew();
auto sheet = editor->FirstWorksheet();
sheet->SetCellNumber(1, 1, 10.0);                               // A1
sheet->SetCellNumber(2, 1, 32.0);                               // A2
sheet->SetCellFormula(*CellAddress::ParseA1("A3"), "=SUM(A1:A2)");

FormulaEngine engine(editor->GetDocument());

// Ad-hoc evaluation without touching the workbook:
auto adHoc = engine.EvaluateFormula("=AVERAGE(A1:A2)*2");
// adHoc.Value.NumberValue() == 42.0

// Evaluate one stored formula cell (ignores the stale cache):
auto cell = engine.EvaluateCell("Sheet1", *CellAddress::ParseA1("A3"));

// Recalculate everything and persist cached results:
engine.Recalculate();
editor->SaveToFile("report.xlsx");
```

`FormulaEngine` follows the workbook-service pattern of
`SharedStringTableService`: it is a lightweight wrapper around a shared
`ExcelDocument` and stays usable while that document is alive.

## Core concepts

### Worksheet errors are values, not failures

`=1/0` evaluates *successfully* to a `#DIV/0!` value. Statuses
(`FormulaEngineStatus`) fail only for engine-level problems: no document,
unknown worksheet, invalid address, unparsable text, or an R1C1 workbook.

| Error literal | `FormulaErrorCode` | Typical cause |
|---|---|---|
| `#NULL!`  | `Null`  | empty intersection `A1:A2 B5:B6` |
| `#DIV/0!` | `Div0`  | division by zero |
| `#VALUE!` | `Value` | wrong operand/argument type |
| `#REF!`   | `Ref`   | invalid or deleted reference, unknown sheet, external workbook |
| `#NAME?`  | `Name`  | unknown function or defined name |
| `#NUM!`   | `Num`   | invalid numeric result (`SQRT(-1)`, overflow) |
| `#N/A`    | `NA`    | failed lookup, `NA()` |

`FormulaErrorText` and `ParseFormulaErrorText` convert between codes and
literals.

### FormulaValue

Evaluation produces an immutable `FormulaValue`: Blank, Number, Text, Boolean,
Error, or Array (a rectangular row-major matrix of scalars produced by array
formulas). Scalars answer `RowCount()`/`ColumnCount()` of 1 and return
themselves from `At(0, 0)`, so element access is uniform.

### Formula syntax

Formulas use the canonical SpreadsheetML en-US storage form: `,` separates
arguments, `.` is the decimal point, `;` separates array-literal rows, string
literals double embedded quotes, and sheet names quote with apostrophes
(`'My Data'!B2`). A leading `=` is accepted and ignored. `_xlfn.` prefixes
(as stored by Excel for newer functions) are stripped.

Operator precedence matches Excel, including its two famous quirks: unary
minus binds tighter than the power operator (`-2^2 = 4`) and `^` is
left-associative (`2^3^2 = 64`). The full ladder, loosest to tightest:
comparisons, `&`, `+ -`, `* /`, `^`, unary `+ -`, postfix `%`, then the
reference operators `:` (range), space (intersection), and `,` (union inside
parentheses).

Coercion follows Excel: blank is 0 / `""` / FALSE depending on context,
booleans are 1/0 in arithmetic, numeric text converts (`"3"+4 = 7`), text
comparison is case-insensitive, and cross-type ordering is
number < text < boolean.

## API overview

| Method | Purpose |
|---|---|
| `ValidateFormula(text)` | Parse-only check; returns `FormulaDiagnostic`s with byte offsets (relative to the text after a leading `=`). Unknown function names and wrong argument counts are reported. |
| `EvaluateFormula(text, sheet, anchor)` | Ad-hoc evaluation; unqualified references resolve on `sheet` (default: first worksheet). The optional anchor supplies `ROW()`/`COLUMN()` context and implicit intersection. |
| `EvaluateCell(sheet, address)` | Recomputes one stored formula cell from current data (the stored cache is ignored). Shared-group dependents evaluate their anchor's expression shifted to the cell. Array formulas return the whole matrix. |
| `Recalculate()` / `RecalculateSheet(name)` | Dependency-ordered recalculation; writes cached results through the storage API. The sheet variant still reads cross-sheet precedents but writes only the named sheet. |
| `FindCircularReferences()` | Reports cycles without modifying anything. |
| `RegisterFunction(name, spec, fn)` | Registers or overrides a case-insensitive custom function on this engine instance. |

## Recalculation

`Recalculate()` builds a workbook-wide dependency graph (formula cell →
formula cell), detects strongly connected components with an iterative Tarjan
algorithm, and evaluates the acyclic part in topological order. Results are
kept in an overlay during the pass — so every formula sees fresh precedent
values — and written back at the end as cached results
(`CellFormulaValue::cachedKind` / `CachedText`), preserving the formula text,
shared/array metadata, and reference style.

- **Circular references:** cells in a cycle are *not* rewritten — they keep
  their previous cached values — and every cycle is reported in
  `RecalculationResult::circularReferenceCycles`. Cells that depend on a cycle
  are evaluated using the members' previous cached values. (Excel writes 0 and
  shows a warning; keeping the cache is a deliberate, non-destructive
  divergence.)
- **Shared formulas:** dependent cells of a shared group evaluate the anchor's
  parsed expression with a row/column offset applied to relative references —
  no text rewriting.
- **Array (CSE) formulas:** the anchor evaluates its matrix with broadcasting
  semantics; the result is distributed over the stored `ref` range top-left
  anchored (scalar results broadcast, uncovered cells receive `#N/A`).
  Non-anchor range cells receive plain cached values.
- **Dynamic references:** `OFFSET`, `INDIRECT`, and volatile functions hide
  dependencies from static analysis. When any are present, one bounded extra
  pass re-evaluates the workbook so results stabilize.
- **calcChain:** the engine never writes `calcChain.xml` and removes an
  existing part during recalculation — a stale chain can trigger repair
  prompts, and spreadsheet applications rebuild it automatically.
- **calcPr** is left untouched.

String results are cached as `str` (plain formula strings), never as shared
strings, mirroring Excel's own behavior for freshly calculated results.

## Built-in function library (~105 functions)

- **Math/trig:** SUM, PRODUCT, ABS, SIGN, INT, TRUNC, ROUND, ROUNDUP,
  ROUNDDOWN, MOD, POWER, SQRT, EXP, LN, LOG, LOG10, PI, SIN, COS, TAN, ASIN,
  ACOS, ATAN, ATAN2, DEGREES, RADIANS, CEILING, FLOOR, EVEN, ODD, RAND,
  RANDBETWEEN, SUMPRODUCT
- **Conditional aggregation:** SUMIF, SUMIFS, COUNTIF, COUNTIFS, AVERAGEIF,
  AVERAGEIFS — criteria support `>`, `>=`, `<`, `<=`, `<>`, `=` prefixes and
  the wildcards `*`/`?` with `~` escaping
- **Statistical:** AVERAGE, AVERAGEA, COUNT, COUNTA, COUNTBLANK, MAX, MIN,
  MEDIAN, MODE, LARGE, SMALL, STDEV, STDEVP, VAR, VARP
- **Logical:** IF, IFERROR, IFNA, IFS, SWITCH, CHOOSE (lazy special forms —
  `IF(TRUE,1,1/0)` never divides), AND, OR, XOR, NOT, TRUE, FALSE
- **Text:** CONCATENATE, CONCAT, TEXTJOIN, LEFT, RIGHT, MID, LEN, LOWER,
  UPPER, PROPER, TRIM, CLEAN, SUBSTITUTE, REPLACE, FIND, SEARCH, REPT, VALUE,
  EXACT, CHAR, CODE, TEXT — positions and lengths count UTF-8 characters
- **Lookup/reference:** VLOOKUP, HLOOKUP, LOOKUP, INDEX, MATCH, OFFSET,
  INDIRECT (A1 only), ROW, COLUMN, ROWS, COLUMNS, TRANSPOSE
- **Date/time:** DATE, TIME, DATEVALUE, TIMEVALUE, YEAR, MONTH, DAY, HOUR,
  MINUTE, SECOND, WEEKDAY, WEEKNUM, EDATE, EOMONTH, DAYS, TODAY, NOW
- **Financial:** PMT, IPMT, PPMT, FV, PV, NPER, RATE, NPV, IRR (RATE and IRR
  use Newton iteration with a bisection fallback)
- **Information:** ISBLANK, ISERROR, ISERR, ISNA, ISNUMBER, ISTEXT, ISLOGICAL,
  ISEVEN, ISODD, N, NA

Volatile functions (always recomputed): NOW, TODAY, RAND, RANDBETWEEN,
OFFSET, INDIRECT.

### Custom functions

```cpp
FormulaFunctionSpec spec;
spec.MinimumArgumentCount = 1;
spec.MaximumArgumentCount = 1;
engine.RegisterFunction("DOUBLEIT", spec,
    [](FormulaFunctionContext& context, std::span<const FormulaValue> args) {
        const auto number = args[0].NumberValue();
        return number ? FormulaValue::Number(*number * 2.0)
                      : FormulaValue::Error(FormulaErrorCode::Value);
    });
// =DOUBLEIT(A1) now works on this engine instance.
```

Arguments arrive fully evaluated (ranges as Array values). Registrations are
per-engine: other `FormulaEngine` instances keep the default library.
Built-ins may be overridden.

## Dates, times, and TEXT formats

Dates are 1900-system serial numbers (serial 1 = 1900-01-01). The historical
Lotus leap-year bug is preserved for compatibility: 1900-03-01 is serial 61,
and the 1900 system counts serial 1 as a Sunday, so weekday results match
Excel for all modern dates. Cells stored with SpreadsheetML type `d` (ISO 8601
text) coerce to serial numbers automatically.

`TEXT()` implements a documented subset of number-format codes: digit
placeholders (`0`, `#`, `?`), decimal point, thousands separator and
trailing-comma scaling, `%`, scientific `E+00`, literal text, `@`, up to four
`;` sections, and the date/time tokens `yyyy`, `yy`, `mmmm`, `mmm`, `mm`, `m`,
`dddd`, `ddd`, `dd`, `d`, `hh`, `h`, `mm`, `ss`, `AM/PM`, `[h]`. Unsupported
codes (colors, conditions, fractions, fill tokens) return `#VALUE!` instead of
silently misformatting.

## Known limitations (documented behavior, not silent miscalculation)

- Defined names created through `NamedRangeManager` resolve during
  evaluation and recalculation (see `docs/excel/named-ranges.md`); names not
  defined in any scope, and structured table references, evaluate to
  `#NAME?`.
- External workbook references (`[Book.xlsx]Sheet1!A1`) and 3-D sheet ranges
  (`Sheet1:Sheet3!A1`) evaluate to `#REF!`.
- R1C1 formulas are rejected with
  `FormulaEngineError::UnsupportedReferenceStyle`; recalculation skips R1C1
  cells.
- No iterative calculation: circular references keep their previous cached
  values.
- Legacy CSE array formulas only; dynamic-array spilling is not modeled.
- The engine is en-US canonical only; UI-locale formula text (`;` argument
  separators, `,` decimals) is out of scope.
- Text case mapping (UPPER/LOWER/PROPER, case-insensitive comparison) is
  ASCII-only; non-ASCII characters pass through unchanged.
- Dates before 1900-03-01 may differ from Excel by one day around the
  fictitious 1900-02-29; serial 60 maps to 1900-02-28.

## Testing

`tests/spreadsheet/ExcelFormulaEngineTests.cpp` (label `excel-formulas`, tags
`[unit] [excel] [excel-formula-engine]`) covers parsing diagnostics, operator semantics and
coercion, all seven error values with propagation and containment, every
function category, cross-sheet references, shared and array formulas,
circular-reference detection, dependency-ordered recalculation, package
round-trips of cached results, and custom function registration.
