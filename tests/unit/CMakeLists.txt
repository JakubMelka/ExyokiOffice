# Copyright (c) 2026 Jakub Melka and Collaborators
# SPDX-License-Identifier: MIT
# See LICENSE file in the project root for full license text.

# Core layer: the DOM element model, simple types, markup compatibility and the
# query layer, all of it independent of any single Office family. The generated
# per-namespace element suites are a layer of their own in tests/dom.
exyokioffice_add_test_executable(ExyokiOfficeUnitTests
    LAYER unit
    PREFIX Unit
    SOURCES
        VersionTests.cpp
        GuidTests.cpp
        MeasuringAngleTests.cpp
        ImageFormatTests.cpp
        OpenXmlSimpleTypesTests.cpp
        GeneratedEnumMetadataTests.cpp
        OpenXmlElementNodeTests.cpp
        OpenXmlElementSiblingTests.cpp
        OpenXmlElementStructuralOpsTests.cpp
        MarkupCompatibilityTests.cpp
        DomContextResolutionTests.cpp
        DomValidatorExtensibilityTests.cpp
        DocumentEditTransactionTests.cpp
        XmlQueryTests.cpp
        XmlLocationCacheTests.cpp
        ContentModelAutomatonTests.cpp
        ContentModelMetadataTests.cpp)

exyokioffice_add_test_area(unit
    NAME Unit.MarkupCompatibility
    LABELS markup-compatibility
    TAGS "[markup-compatibility]")

exyokioffice_add_test_area(unit
    NAME Unit.Xml
    LABELS xml-query
    TAGS "[xml]")

exyokioffice_add_test_area(unit
    NAME Unit.DomContext
    LABELS dom-resolution
    TAGS "[dom-resolution]")

exyokioffice_add_test_area(unit
    NAME Unit.DomExtensibility
    LABELS dom-extensibility validation
    TAGS "[dom-extensibility]")

exyokioffice_add_test_area(unit
    NAME Unit.XmlLocationCache
    LABELS xml-location-cache
    TAGS "[xml-location-cache]")

# Both carry `slow`: between them they are the whole cost of the unit layer,
# which is otherwise instant. `ctest -LE slow` is the edit-test loop.
exyokioffice_add_test_area(unit
    NAME Unit.ContentModel
    LABELS content-model validation slow
    TAGS "[content-model]")

exyokioffice_add_test_area(unit
    NAME Unit.ContentModelMetadata
    LABELS content-model validation slow
    TAGS "[metadata-content-model]")

exyokioffice_add_test_area(unit
    NAME Unit.EnumMetadata
    LABELS enum-metadata
    TAGS "[enum-metadata]")

exyokioffice_add_test_area(unit
    NAME Unit.ImageFormat
    LABELS image-format
    TAGS "[image-format]")

exyokioffice_finalize_test_layer(unit)
