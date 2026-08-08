# Copyright (c) 2026 Jakub Melka and Collaborators
# SPDX-License-Identifier: MIT
# See LICENSE file in the project root for full license text.

# SpreadsheetML layer: everything reachable from ExcelDocumentEditor. The layer
# answers to both "spreadsheet" (its directory) and "excel" (the tag its tests
# have always used and the name the matrix uses).
exyokioffice_add_test_executable(ExyokiOfficeSpreadsheetTests
    LAYER spreadsheet
    PREFIX Spreadsheet
    LABELS excel
    SOURCES
        ExcelLifecycleTests.cpp
        ExcelAddressTests.cpp
        ExcelReferenceTests.cpp
        ExcelCellValueTests.cpp
        ExcelSharedStringTests.cpp
        ExcelSparseWorksheetTests.cpp
        ExcelRangeBulkTests.cpp
        ExcelWorksheetContentTests.cpp
        ExcelStructuralEditTests.cpp
        ExcelMergeTests.cpp
        ExcelStyleRepositoryTests.cpp
        ExcelAdvancedFormattingTests.cpp
        ExcelConditionalFormattingTests.cpp
        ExcelFormulaTests.cpp
        ExcelFormulaEngineTests.cpp
        ExcelNamedRangeTests.cpp
        ExcelTableTests.cpp
        ExcelPivotTableTests.cpp
        ExcelSlicerTests.cpp
        ExcelDataValidationTests.cpp
        ExcelChartTests.cpp
        ExcelWorksheetLayoutTests.cpp
        ExcelWorksheetPrintTests.cpp
        ExcelAdvancedPreservationTests.cpp
        ExcelVbaTests.cpp)

# Areas follow the rows of the Excel table in docs/Compatibility.md. Their tags
# must stay disjoint: a case matched by two areas would run twice.
exyokioffice_add_test_area(spreadsheet NAME Spreadsheet.Workbooks LABELS excel-workbooks
    TAGS "[excel-lifecycle]" "[preservation]")
exyokioffice_add_test_area(spreadsheet NAME Spreadsheet.Worksheets LABELS excel-worksheets
    TAGS "[excel-worksheets]" "[excel-structure]")
exyokioffice_add_test_area(spreadsheet NAME Spreadsheet.Cells LABELS excel-cells
    TAGS "[excel-cell-value]" "[excel-shared-strings]" "[excel-sparse]" "[excel-range]" "[excel-merge]")
exyokioffice_add_test_area(spreadsheet NAME Spreadsheet.Addresses LABELS excel-addresses
    TAGS "[excel-address]")
exyokioffice_add_test_area(spreadsheet NAME Spreadsheet.Styles LABELS excel-styles
    TAGS "[excel-style]" "[excel-advanced-formatting]" "[excel-style-readback]" "[excel-style-delta]"
         "[excel-cell-formatter]" "[excel-range-border]")
exyokioffice_add_test_area(spreadsheet NAME Spreadsheet.NamedRanges LABELS excel-named-ranges
    TAGS "[excel-named-range]")
exyokioffice_add_test_area(spreadsheet NAME Spreadsheet.Formulas LABELS excel-formulas
    TAGS "[excel-formula]" "[excel-formula-engine]")
exyokioffice_add_test_area(spreadsheet NAME Spreadsheet.Tables LABELS excel-tables
    TAGS "[excel-table]")
exyokioffice_add_test_area(spreadsheet NAME Spreadsheet.Charts LABELS excel-charts
    TAGS "[excel-chart]")
exyokioffice_add_test_area(spreadsheet NAME Spreadsheet.PivotTables LABELS excel-pivot
    TAGS "[excel-pivot]")
exyokioffice_add_test_area(spreadsheet NAME Spreadsheet.Slicers LABELS excel-slicers
    TAGS "[excel-slicer]")
exyokioffice_add_test_area(spreadsheet NAME Spreadsheet.Validation LABELS excel-validation
    TAGS "[excel-validation]")
exyokioffice_add_test_area(spreadsheet NAME Spreadsheet.ConditionalFormatting
    LABELS excel-conditional-formatting
    TAGS "[conditional-formatting]")
exyokioffice_add_test_area(spreadsheet NAME Spreadsheet.Layout LABELS excel-layout
    TAGS "[layout]")
exyokioffice_add_test_area(spreadsheet NAME Spreadsheet.Printing LABELS excel-printing
    TAGS "[print]")
exyokioffice_add_test_area(spreadsheet NAME Spreadsheet.Vba LABELS vba
    TAGS "[vba]")
exyokioffice_add_test_area(spreadsheet NAME Spreadsheet.Protection LABELS protection
    TAGS "[excel-protection]")

exyokioffice_finalize_test_layer(spreadsheet)
