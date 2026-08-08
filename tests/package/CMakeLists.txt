# Copyright (c) 2026 Jakub Melka and Collaborators
# SPDX-License-Identifier: MIT
# See LICENSE file in the project root for full license text.

# OPC layer: the package, its relationship graph, the shared Office services
# built on top of it (properties, themes, deduplication) and the security
# subsystems that operate on a whole package.
#
# ExyokiOfficeFuzzTargets carries the fuzz entry points and the
# EXYOKIOFFICE_FUZZ_DIR definition; FuzzCorpusReplayTests replays the
# committed corpus and crash artifacts through them.
exyokioffice_add_test_executable(ExyokiOfficePackageTests
    LAYER package
    PREFIX Package
    LINK ExyokiOfficeFuzzTargets
    SOURCES
        OpcRelationshipTests.cpp
        OpcRelationshipGraphTests.cpp
        OpcPartLifetimeTests.cpp
        OpcDetachTests.cpp
        OpcValidationTests.cpp
        OpcChildPartFolderTests.cpp
        PartByteRetentionTests.cpp
        DocumentPropertiesTests.cpp
        ThemeServiceTests.cpp
        ResourceDeduplicatorTests.cpp
        OfficePasswordVerifierTests.cpp
        SecurityMessageDigestTests.cpp
        SecurityCanonicalizationTests.cpp
        PackageSignatureTests.cpp
        ExternalResourceTests.cpp
        FuzzCorpusReplayTests.cpp)

# Areas follow the cross-cutting table in docs/Compatibility.md. Their tags must
# stay disjoint: a case matched by two areas would run twice. That is why the
# external-resource tests carry their own [external-resources] tag — the older
# [external] also appears on relationship and validation cases.
exyokioffice_add_test_area(package NAME Package.Roundtrip LABELS opc-roundtrip
    TAGS "[opc-relationship]" "[opc-relationship-graph]" "[opc-part-lifetime]" "[opc-detach]" "[retention]" "[uri]")
exyokioffice_add_test_area(package NAME Package.Validation LABELS validation
    TAGS "[opc-validation]")
exyokioffice_add_test_area(package NAME Package.Properties LABELS properties
    TAGS "[properties]")
exyokioffice_add_test_area(package NAME Package.Themes LABELS themes
    TAGS "[theme]")
exyokioffice_add_test_area(package NAME Package.Signatures LABELS signatures
    TAGS "[signature]" "[digest]" "[c14n]")
exyokioffice_add_test_area(package NAME Package.Protection LABELS protection
    TAGS "[protection]")
exyokioffice_add_test_area(package NAME Package.ExternalResources LABELS external-resources
    TAGS "[external-resources]")
exyokioffice_add_test_area(package NAME Package.Fuzz LABELS fuzz
    TAGS "[fuzz]")

exyokioffice_finalize_test_layer(package)
