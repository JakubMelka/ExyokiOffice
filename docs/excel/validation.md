# Data validation and conditional formatting

Data validation constrains what a user may *enter* into a cell; conditional
formatting changes how a cell *looks* based on its value. Both are
range-scoped worksheet features with typed definitions.

```cpp
#include "ExyokiOffice/Excel/ExcelDocument.hpp"
using namespace ExyokiOffice::Excel;
```

## Data validation

```cpp
ExcelDataValidationDefinition rule;
rule.Type = DataValidationType::Whole;
rule.Operation = DataValidationOperator::GreaterThan;
rule.Formula1 = "0";
rule.Ranges = {*CellRange::ParseA1("B2:B10")};
rule.ShowErrorMessage = true;
rule.ErrorTitle = "Invalid quantity";
rule.Error = "Enter a whole number greater than zero.";
sheet->CreateDataValidation(rule);
```

`DataValidationType` covers Excel's rule kinds (whole number, decimal, list,
date, time, text length, custom formula); `DataValidationOperator` supplies
the comparison, with `Formula1`/`Formula2` as the operands. A dropdown list
is a `List` rule whose `Formula1` is either a literal
(`"\"Red,Green,Blue\""`) or a range reference. Input-message fields
(`ShowInputMessage`, `PromptTitle`, `Prompt`) mirror the error fields.

The full lifecycle is available: `DataValidations()` enumerates existing
rules as `ExcelDataValidation` wrappers, `UpdateDataValidation` replaces a
rule's definition, and `RemoveDataValidation` deletes it.

Validation is advisory: it is enforced by Excel's UI at entry time, not by
the file format — values written by this library (or any tool) bypass it.

## Conditional formatting

```cpp
sheet->CreateConditionalFormatting(
    ExcelConditionalFormattingDefinition::CellIs(
        {*CellRange::ParseA1("D2:D10")}, ConditionalFormattingOperator::GreaterThan, "100"));
```

`ExcelConditionalFormattingDefinition` has static factories for the common
rule shapes (`Expression`, `CellIs`, `Between`, `ContainsText`,
`UniqueValues`, `Top`, `AboveAverage`, and more).
`ConditionalFormattings()`, `UpdateConditionalFormatting`,
`MoveConditionalFormatting` (rule priority), and
`RemoveConditionalFormatting` complete the lifecycle.

A rule's `DifferentialFormatId` references an existing workbook `dxfs`
(differential formats) entry — the "what to apply" half of the rule. This
API preserves that reference but does not create differential-format styles;
authoring a new `dxfs` entry currently requires the low-level DOM.
