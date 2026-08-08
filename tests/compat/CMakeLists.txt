# Copyright (c) 2026 Jakub Melka and Collaborators
# SPDX-License-Identifier: MIT
# See LICENSE file in the project root for full license text.

# The compatibility matrix layer: one test case per row of docs/Compatibility.md,
# exercising exactly the three capabilities that row is graded on — Create, Edit
# and Preserve — through the public high-level API.
#
# This layer is what makes each row of the matrix falsifiable, so its area tags
# are literally the labels the matrix cites. Everything here is also labelled
# with its family, which is why `ctest -L word` runs the Word rows along with the
# Word test layer.
exyokioffice_add_test_executable(ExyokiOfficeCompatibilityTests
    LAYER compat
    PREFIX Compat
    SOURCES
        FormatMatrixTests.cpp
        WordMatrixTests.cpp
        ExcelMatrixTests.cpp
        PowerPointMatrixTests.cpp
        CrossCuttingMatrixTests.cpp)

# --- File formats and Office versions -------------------------------------
exyokioffice_add_test_area(compat NAME Compat.Formats.Word LABELS formats-word word
    TAGS "[formats-word]")
exyokioffice_add_test_area(compat NAME Compat.Formats.Excel LABELS formats-excel excel
    TAGS "[formats-excel]")
exyokioffice_add_test_area(compat NAME Compat.Formats.PowerPoint LABELS formats-powerpoint powerpoint
    TAGS "[formats-powerpoint]")
exyokioffice_add_test_area(compat NAME Compat.Formats.FlatOpc LABELS formats-flatopc
    TAGS "[formats-flatopc]")
exyokioffice_add_test_area(compat NAME Compat.Formats.Conversion LABELS formats-conversion
    TAGS "[formats-conversion]")
exyokioffice_add_test_area(compat NAME Compat.OfficeVersions LABELS office-versions
    TAGS "[office-versions]")

# --- Word ------------------------------------------------------------------
exyokioffice_add_test_area(compat NAME Compat.Word.Documents LABELS word-documents word
    TAGS "[word-documents]")
exyokioffice_add_test_area(compat NAME Compat.Word.Text LABELS word-text word
    TAGS "[word-text]")
exyokioffice_add_test_area(compat NAME Compat.Word.Styles LABELS word-styles word
    TAGS "[word-styles]")
exyokioffice_add_test_area(compat NAME Compat.Word.Tables LABELS word-tables word
    TAGS "[word-tables]")
exyokioffice_add_test_area(compat NAME Compat.Word.Images LABELS word-images word
    TAGS "[word-images]")
exyokioffice_add_test_area(compat NAME Compat.Word.Hyperlinks LABELS word-hyperlinks word
    TAGS "[word-hyperlinks]")
exyokioffice_add_test_area(compat NAME Compat.Word.Fields LABELS word-fields word
    TAGS "[word-fields]")
exyokioffice_add_test_area(compat NAME Compat.Word.Charts LABELS word-charts word
    TAGS "[word-charts]")
exyokioffice_add_test_area(compat NAME Compat.Word.Sections LABELS word-sections word
    TAGS "[word-sections]")
exyokioffice_add_test_area(compat NAME Compat.Word.Notes LABELS word-notes word
    TAGS "[word-notes]")
exyokioffice_add_test_area(compat NAME Compat.Word.ContentControls LABELS word-content-controls word
    TAGS "[word-content-controls]")
exyokioffice_add_test_area(compat NAME Compat.Word.Revisions LABELS word-revisions word
    TAGS "[word-revisions]")
exyokioffice_add_test_area(compat NAME Compat.Word.Merge LABELS word-merge word
    TAGS "[word-merge]")
exyokioffice_add_test_area(compat NAME Compat.Word.Protection LABELS word-protection word
    TAGS "[word-protection]")
exyokioffice_add_test_area(compat NAME Compat.Word.Preserved LABELS word-preserved word
    TAGS "[word-preserved]")

# --- Excel -----------------------------------------------------------------
exyokioffice_add_test_area(compat NAME Compat.Excel.Workbooks LABELS excel-workbooks excel
    TAGS "[excel-workbooks]")
exyokioffice_add_test_area(compat NAME Compat.Excel.Worksheets LABELS excel-worksheets excel
    TAGS "[excel-worksheets]")
exyokioffice_add_test_area(compat NAME Compat.Excel.Cells LABELS excel-cells excel
    TAGS "[excel-cells]")
exyokioffice_add_test_area(compat NAME Compat.Excel.RichText LABELS excel-rich-text excel
    TAGS "[excel-rich-text]")
exyokioffice_add_test_area(compat NAME Compat.Excel.Styles LABELS excel-styles excel
    TAGS "[excel-styles]")
exyokioffice_add_test_area(compat NAME Compat.Excel.NamedRanges LABELS excel-named-ranges excel
    TAGS "[excel-named-ranges]")
exyokioffice_add_test_area(compat NAME Compat.Excel.Formulas LABELS excel-formulas excel
    TAGS "[excel-formulas]")
exyokioffice_add_test_area(compat NAME Compat.Excel.ExternalRefs LABELS excel-external-refs excel
    TAGS "[excel-external-refs]")
exyokioffice_add_test_area(compat NAME Compat.Excel.Tables LABELS excel-tables excel
    TAGS "[excel-tables]")
exyokioffice_add_test_area(compat NAME Compat.Excel.Charts LABELS excel-charts excel
    TAGS "[excel-charts]")
exyokioffice_add_test_area(compat NAME Compat.Excel.PivotTables LABELS excel-pivot excel
    TAGS "[excel-pivot]")
exyokioffice_add_test_area(compat NAME Compat.Excel.Slicers LABELS excel-slicers excel
    TAGS "[excel-slicers]")
exyokioffice_add_test_area(compat NAME Compat.Excel.Validation LABELS excel-validation excel
    TAGS "[excel-validation]")
exyokioffice_add_test_area(compat NAME Compat.Excel.ConditionalFormatting
    LABELS excel-conditional-formatting excel
    TAGS "[excel-conditional-formatting]")
exyokioffice_add_test_area(compat NAME Compat.Excel.Layout LABELS excel-layout excel
    TAGS "[excel-layout]")
exyokioffice_add_test_area(compat NAME Compat.Excel.Printing LABELS excel-printing excel
    TAGS "[excel-printing]")

# --- PowerPoint ------------------------------------------------------------
exyokioffice_add_test_area(compat NAME Compat.Ppt.Presentations LABELS ppt-presentations powerpoint
    TAGS "[ppt-presentations]")
exyokioffice_add_test_area(compat NAME Compat.Ppt.Slides LABELS ppt-slides powerpoint
    TAGS "[ppt-slides]")
exyokioffice_add_test_area(compat NAME Compat.Ppt.Sections LABELS ppt-sections powerpoint
    TAGS "[ppt-sections]")
exyokioffice_add_test_area(compat NAME Compat.Ppt.Masters LABELS ppt-masters powerpoint
    TAGS "[ppt-masters]")
exyokioffice_add_test_area(compat NAME Compat.Ppt.Shapes LABELS ppt-shapes powerpoint
    TAGS "[ppt-shapes]")
exyokioffice_add_test_area(compat NAME Compat.Ppt.Text LABELS ppt-text powerpoint
    TAGS "[ppt-text]")
exyokioffice_add_test_area(compat NAME Compat.Ppt.Media LABELS ppt-media powerpoint
    TAGS "[ppt-media]")
exyokioffice_add_test_area(compat NAME Compat.Ppt.Tables LABELS ppt-tables powerpoint
    TAGS "[ppt-tables]")
exyokioffice_add_test_area(compat NAME Compat.Ppt.Charts LABELS ppt-charts powerpoint
    TAGS "[ppt-charts]")
exyokioffice_add_test_area(compat NAME Compat.Ppt.Transitions LABELS ppt-transitions powerpoint
    TAGS "[ppt-transitions]")
exyokioffice_add_test_area(compat NAME Compat.Ppt.Animations LABELS ppt-animations powerpoint
    TAGS "[ppt-animations]")
exyokioffice_add_test_area(compat NAME Compat.Ppt.Notes LABELS ppt-notes powerpoint
    TAGS "[ppt-notes]")
exyokioffice_add_test_area(compat NAME Compat.Ppt.Preserved LABELS ppt-preserved powerpoint
    TAGS "[ppt-preserved]")

# --- Cross-cutting ---------------------------------------------------------
exyokioffice_add_test_area(compat NAME Compat.Cross.OpcRoundtrip LABELS opc-roundtrip
    TAGS "[opc-roundtrip]")
exyokioffice_add_test_area(compat NAME Compat.Cross.UnknownParts LABELS opc-unknown-parts
    TAGS "[opc-unknown-parts]")
exyokioffice_add_test_area(compat NAME Compat.Cross.MarkupCompatibility LABELS markup-compatibility
    TAGS "[markup-compatibility]")
exyokioffice_add_test_area(compat NAME Compat.Cross.Validation LABELS validation
    TAGS "[validation]")
exyokioffice_add_test_area(compat NAME Compat.Cross.Properties LABELS properties
    TAGS "[properties]")
exyokioffice_add_test_area(compat NAME Compat.Cross.Themes LABELS themes
    TAGS "[themes]")
exyokioffice_add_test_area(compat NAME Compat.Cross.Vba LABELS vba
    TAGS "[vba]")
exyokioffice_add_test_area(compat NAME Compat.Cross.Protection LABELS protection
    TAGS "[protection]")
exyokioffice_add_test_area(compat NAME Compat.Cross.Signatures LABELS signatures
    TAGS "[signatures]")
exyokioffice_add_test_area(compat NAME Compat.Cross.Encryption LABELS encryption
    TAGS "[encryption]")
exyokioffice_add_test_area(compat NAME Compat.Cross.ExternalResources LABELS external-resources
    TAGS "[external-resources]")

exyokioffice_finalize_test_layer(compat)
