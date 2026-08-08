# Copyright (c) 2026 Jakub Melka and Collaborators
# SPDX-License-Identifier: MIT
# See LICENSE file in the project root for full license text.

# The generator's own output contract: the schematron rules it emits and the
# element metadata the rest of the library reads back at runtime. These are the
# only tests that link generator_support and the only ones that read data/ from
# the source tree through EXYOKIOFFICE_SOURCE_DIR.
exyokioffice_add_test_executable(ExyokiOfficeGeneratorTests
    LAYER generator
    PREFIX Generator
    LINK generator_support
    SOURCES
        GeneratorSchematronTests.cpp
        MetadataConstraintTests.cpp)

exyokioffice_add_test_area(generator NAME Generator.Schematron LABELS generator-schematron
    TAGS "[generator-schematron]")
exyokioffice_add_test_area(generator NAME Generator.Metadata LABELS generator-metadata
    TAGS "[metadata-constraints]")

exyokioffice_finalize_test_layer(generator)
