# Copyright (c) 2026 Jakub Melka and Collaborators
# SPDX-License-Identifier: MIT
# See LICENSE file in the project root for full license text.

# The ExyokiOffice::Tools module backing exyoki: inspection, validation,
# archiving, Flat OPC and semantic conversion, diffing, text extraction and the
# XML query tool. It spans all three families, which is why it is a layer of its
# own rather than part of any one of them.
exyokioffice_add_test_executable(ExyokiOfficeToolsTests
    LAYER tools
    PREFIX Tools
    SOURCES
        ToolsPackageInspectorTests.cpp
        ToolsValidationRunnerTests.cpp
        ToolsReportRendererTests.cpp
        ToolsPackageArchiverTests.cpp
        ToolsPackageDiffTests.cpp
        ToolsFlatOpcConverterTests.cpp
        ToolsMediaExporterTests.cpp
        ToolsOutputNamingTests.cpp
        ToolsTextExtractorTests.cpp
        ToolsMarkdownDocumentTests.cpp
        ToolsDocumentModelJsonTests.cpp
        ToolsDocumentModelSchemaTests.cpp
        ToolsConvertWordTests.cpp
        ToolsConvertExcelTests.cpp
        ToolsConvertPowerPointTests.cpp
        ToolsDocumentToolsTests.cpp
        ToolsDocumentAutomationTests.cpp
        ToolsCorePropertiesNamespaceTests.cpp
        ToolsWordTextToolsTests.cpp
        ToolsWordDocumentToolsTests.cpp
        ToolsXmlQueryTests.cpp)

# The "Other formats" rows of docs/Compatibility.md are all implemented here.
exyokioffice_add_test_area(tools NAME Tools.FlatOpc LABELS formats-flatopc
    TAGS "[flat-opc]")
exyokioffice_add_test_area(tools NAME Tools.Conversion LABELS formats-conversion
    TAGS "[conversion]")
exyokioffice_add_test_area(tools NAME Tools.Validation LABELS validation
    TAGS "[validation-runner]")

exyokioffice_finalize_test_layer(tools)
