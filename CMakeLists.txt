# Copyright (c) 2026 Jakub Melka and Collaborators
# SPDX-License-Identifier: MIT
# See LICENSE file in the project root for full license text.

# 3.25 is what CMakePresets.json declares and what README.md and CONTRIBUTING.md
# tell a consumer to install, so the build states the same number rather than a
# lower one it is never exercised with.
cmake_minimum_required(VERSION 3.25)

# VERSION.txt in the repository root is the single source of truth for the
# release number. The build, the installed Version.hpp, the Windows resource and
# the documentation workflows all read it, so a release is bumped in one place.
file(STRINGS ${CMAKE_CURRENT_SOURCE_DIR}/VERSION.txt ExyokiOfficeVersion LIMIT_COUNT 1)
string(STRIP "${ExyokiOfficeVersion}" ExyokiOfficeVersion)
if(NOT ExyokiOfficeVersion MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
    message(FATAL_ERROR
        "VERSION.txt must contain a MAJOR.MINOR.PATCH number, got '${ExyokiOfficeVersion}'")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/VERSION.txt)

message(STATUS "Build type - CMAKE_BUILD_TYPE: ${CMAKE_BUILD_TYPE}")

project(ExyokiOffice VERSION ${ExyokiOfficeVersion} LANGUAGES CXX)

# Only patch releases promise an unchanged ABI (docs/ABI.md), so the ABI
# identity is exactly MAJOR.MINOR. It is derived rather than maintained by hand:
# a separate number could only ever drift from the policy it encodes.
set(ExyokiOfficeAbiVersion ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR})

if(WIN32)
    enable_language(RC)
endif()

include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(EXYOKIOFFICE_DUMP_EXPORTS "Generate demangled export list (Windows only)" OFF)
option(EXYOKIOFFICE_BUILD_EXAMPLE_WORD "Build the ExampleWord sample" ON)
option(EXYOKIOFFICE_BUILD_EXAMPLE_WORD_EDITOR "Build the ExampleWordEditor sample" ON)
option(EXYOKIOFFICE_BUILD_EXAMPLE_WORD_EDIT "Build the ExampleWordEdit sample" ON)
option(EXYOKIOFFICE_BUILD_EXAMPLE_EXCEL_EDITOR "Build the ExampleExcelEditor sample" ON)
option(EXYOKIOFFICE_BUILD_EXAMPLE_POWERPOINT_EDITOR "Build the ExamplePowerPointEditor sample" ON)
option(EXYOKIOFFICE_BUILD_UNIT_TESTS "Build doctest-based unit tests" OFF)
option(EXYOKIOFFICE_BUILD_TOOL "Build the exyoki command-line utility" ON)
option(EXYOKIOFFICE_BUILD_MCP "Build the exyoki MCP servers" ON)
# Off by default because it is the only test that needs something the C++ build
# cannot produce: a Python environment carrying the official MCP SDK. See
# tests/mcp_python/README.md for the setup, and EXYOKIOFFICE_MCP_PYTHON to point
# the entry at an interpreter of your own.
option(EXYOKIOFFICE_BUILD_MCP_PYTHON_TESTS "Register tests/mcp_python as the Mcp.Python CTest entry" OFF)
option(EXYOKIOFFICE_BUILD_FUZZERS "Build libFuzzer targets (needs Clang or MSVC plus EXYOKIOFFICE_SANITIZER=address)" OFF)

include(cmake/CompilerWarnings.cmake)
include(cmake/Sanitizers.cmake)
include(cmake/Fuzzing.cmake)
include(cmake/Examples.cmake)

# Must run before any target is declared: the sanitizer flags are directory
# scoped and have to reach the generator, the library, the examples and the
# tests alike.
exyokioffice_apply_sanitizers()

# Fuzzing instrumentation is applied per target, not directory wide, so the
# check only validates the configuration here.
exyokioffice_validate_fuzzing_configuration()

if(MSVC)
    set(EXYOKIOFFICE_BUILD_JOBS "" CACHE STRING
        "Maximum number of parallel MSVC compiler processes; empty uses the compiler default")

    add_compile_options(/bigobj)

    # /MP is a cl.exe feature. clang-cl accepts the spelling but ignores it, and
    # since the build is warnings-as-errors the resulting
    # -Wunused-command-line-argument fails every translation unit. Ninja already
    # parallelizes across files there anyway.
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        if(EXYOKIOFFICE_BUILD_JOBS)
            add_compile_options("/MP${EXYOKIOFFICE_BUILD_JOBS}")
        else()
            add_compile_options(/MP)
        endif()
    endif()
endif()

add_subdirectory(gen)

set(GENERATED_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/include/ExyokiOffice/DOM)
set(GENERATED_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/sources/DOM)
file(GLOB_RECURSE GENERATED_HEADERS CONFIGURE_DEPENDS "${GENERATED_INCLUDE_DIR}/*.hpp")
file(GLOB_RECURSE GENERATED_SOURCES CONFIGURE_DEPENDS "${GENERATED_SOURCE_DIR}/*.cpp")
set(GENERATED_PACKAGING_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/include/ExyokiOffice/Packaging)
set(GENERATED_PACKAGING_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/sources/Packaging)
file(GLOB GENERATED_PACKAGING_HEADERS CONFIGURE_DEPENDS "${GENERATED_PACKAGING_INCLUDE_DIR}/*.hpp")
file(GLOB GENERATED_PACKAGING_SOURCES CONFIGURE_DEPENDS "${GENERATED_PACKAGING_SOURCE_DIR}/*.cpp")

message(STATUS "Generated headers: " ${GENERATED_HEADERS})
message(STATUS "Generated sources: " ${GENERATED_SOURCES})

set(GENERATED_STAMP ${CMAKE_CURRENT_BINARY_DIR}/OpenXmlGenerated.stamp)
set(GENERATOR_WARNING_REPORT ${CMAKE_CURRENT_BINARY_DIR}/OpenXmlGeneratorWarnings.json)
add_custom_command(
    OUTPUT ${GENERATED_STAMP}
    BYPRODUCTS ${GENERATOR_WARNING_REPORT}
    COMMAND $<TARGET_FILE:openxml_generator>
            --data "${CMAKE_CURRENT_SOURCE_DIR}/data"
            --out-include "${CMAKE_CURRENT_SOURCE_DIR}/include"
            --out-source "${CMAKE_CURRENT_SOURCE_DIR}/sources"
            --out-tests "${CMAKE_CURRENT_SOURCE_DIR}/tests/dom"
            --warning-report "${GENERATOR_WARNING_REPORT}"
    COMMAND ${CMAKE_COMMAND} -E touch ${GENERATED_STAMP}
    DEPENDS openxml_generator
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Generating ExyokiOffice OpenXML sources"
)

add_custom_target(generate_openxml ALL DEPENDS ${GENERATED_STAMP})

# A fuzz build links the library statically. libFuzzer keeps its coverage
# counters and its runtime state in one instance per process; with the library
# in a DLL the instrumented code and the runtime end up on opposite sides of a
# module boundary, and on Windows the sanitizer coverage symbols then fail to
# resolve at all. Linking statically puts the whole fuzzed surface into the fuzz
# executable, which is also how the coverage feedback is meant to work.
if(EXYOKIOFFICE_BUILD_FUZZERS)
    set(EXYOKIOFFICE_LIBRARY_KIND STATIC)
else()
    set(EXYOKIOFFICE_LIBRARY_KIND SHARED)
endif()

add_library(ExyokiOffice ${EXYOKIOFFICE_LIBRARY_KIND}
    ${CMAKE_CURRENT_BINARY_DIR}/ExyokiOffice/Version.hpp
    include/ExyokiOffice/StandardTypes.hpp
    include/ExyokiOffice/Guid.hpp
    include/ExyokiOffice/OpenXmlSimpleTypes.hpp
    include/ExyokiOffice/ICancellationToken.hpp
    include/ExyokiOffice/OpenXMLElement.hpp
    include/ExyokiOffice/OpenXmlPackage.hpp
    include/ExyokiOffice/OpenXmlPackagePart.hpp
    include/ExyokiOffice/OpenXmlPackageValidator.hpp
    include/ExyokiOffice/OpenXmlDomValidator.hpp
    include/ExyokiOffice/MarkupCompatibility.hpp
    include/ExyokiOffice/MetadataBuilder.hpp
    include/ExyokiOffice/ValidationResult.hpp
    include/ExyokiOffice/OpenXmlQualifiedName.hpp
    include/ExyokiOffice/Color.hpp
    include/ExyokiOffice/MeasuringAngle.hpp
    include/ExyokiOffice/MeasuringUnits.hpp
    include/ExyokiOffice/ImageFormat.hpp
    include/ExyokiOffice/Packaging/PackageUtilities.hpp
    include/ExyokiOffice/Packaging/WordprocessingDocument.hpp
    include/ExyokiOffice/Packaging/SpreadsheetDocument.hpp
    include/ExyokiOffice/Word/WordDocument.hpp
    include/ExyokiOffice/Excel/ExcelAddress.hpp
    include/ExyokiOffice/Excel/ExcelReference.hpp
    include/ExyokiOffice/Excel/ExcelFormulaEngine.hpp
    include/ExyokiOffice/Excel/ExcelNamedRange.hpp
    include/ExyokiOffice/Excel/ExcelTable.hpp
    include/ExyokiOffice/Excel/ExcelPivotTable.hpp
    include/ExyokiOffice/Excel/ExcelSlicer.hpp
    include/ExyokiOffice/Excel/ExcelDataValidation.hpp
    include/ExyokiOffice/Excel/ExcelConditionalFormatting.hpp
    include/ExyokiOffice/Excel/ExcelWorksheetLayout.hpp
    include/ExyokiOffice/Excel/ExcelCellValue.hpp
    include/ExyokiOffice/Excel/ExcelDocument.hpp
    include/ExyokiOffice/PowerPoint/PowerPointDocument.hpp
    include/ExyokiOffice/Packaging/PowerPointDocument.hpp
    include/ExyokiOffice/Tools/PackageModel.hpp
    include/ExyokiOffice/Tools/PackageLimits.hpp
    include/ExyokiOffice/Tools/OutputNaming.hpp
    include/ExyokiOffice/Tools/Report.hpp
    include/ExyokiOffice/Tools/PackageInspector.hpp
    include/ExyokiOffice/Tools/ValidationRunner.hpp
    include/ExyokiOffice/Tools/SignatureInspector.hpp
    include/ExyokiOffice/Tools/ExternalResourceInspector.hpp
    include/ExyokiOffice/Tools/PackageArchiver.hpp
    include/ExyokiOffice/Tools/FlatOpcConverter.hpp
    include/ExyokiOffice/Tools/MediaExporter.hpp
    include/ExyokiOffice/Tools/WordTextTools.hpp
    include/ExyokiOffice/Tools/WordDocumentTools.hpp
    include/ExyokiOffice/Tools/WordAutomationTools.hpp
    include/ExyokiOffice/Tools/DocumentTools.hpp
    include/ExyokiOffice/Tools/DocumentTextTools.hpp
    include/ExyokiOffice/Tools/DocumentRedactor.hpp
    include/ExyokiOffice/Tools/SpreadsheetTools.hpp
    include/ExyokiOffice/Tools/TextExtractor.hpp
    include/ExyokiOffice/Tools/PackageDiff.hpp
    include/ExyokiOffice/Tools/DocumentEditors.hpp
    include/ExyokiOffice/Tools/DocumentStats.hpp
    include/ExyokiOffice/Tools/DocumentModel.hpp
    include/ExyokiOffice/Tools/DocumentModelIO.hpp
    include/ExyokiOffice/Tools/DocumentModelSchema.hpp
    include/ExyokiOffice/Tools/MarkdownDocument.hpp
    include/ExyokiOffice/Tools/DocumentConverter.hpp
    include/ExyokiOffice/Tools/XmlQueryTool.hpp
    include/ExyokiOffice/Xml/XmlQuery.hpp
    sources/OpenXmlSimpleTypes.cpp
    sources/DocumentEditTransaction.cpp
    sources/OpenXMLElement.cpp
    sources/OpenXmlPackage.cpp
    sources/OpenXmlPackagePart.cpp
    sources/OpenXmlPackageValidator.cpp
    sources/OpenXmlDomValidator.cpp
    sources/OpenXmlContentModelAutomaton.cpp
    sources/OpenXmlContentModelReference.cpp
    sources/Color.cpp
    sources/Guid.cpp
    sources/MeasuringAngle.cpp
    sources/MeasuringUnits.cpp
    sources/ImageFormat.cpp
    sources/Packaging/PackageUtilities.cpp
    sources/Packaging/DocumentProperties.cpp
    sources/ThemeService.cpp
    sources/Packaging/WordprocessingDocument.cpp
    sources/Packaging/SpreadsheetDocument.cpp
    sources/Packaging/PowerPointDocument.cpp
    sources/Charts/ChartXml.cpp
    sources/Word/WordDocument.cpp
    sources/Word/WordProtection.cpp
    sources/Word/WordChart.cpp
    sources/Excel/ExcelAddress.cpp
    sources/Excel/ExcelReference.cpp
    sources/Excel/FormulaParser.cpp
    sources/Excel/FormulaEvaluator.cpp
    sources/Excel/FormulaFunctions.cpp
    sources/Excel/FormulaFunctionsLibrary.cpp
    sources/Excel/NumberFormatText.cpp
    sources/Excel/ExcelFormulaEngine.cpp
    sources/Excel/ExcelNamedRange.cpp
      sources/Excel/ExcelTable.cpp
      sources/Excel/ExcelPivotTable.cpp
      sources/Excel/ExcelSlicer.cpp
      sources/Excel/ExcelDataValidation.cpp
      sources/Excel/ExcelConditionalFormatting.cpp
    sources/Excel/ExcelWorksheetLayout.cpp
    sources/Excel/ExcelWorksheetPrint.cpp
    sources/Excel/ExcelWorksheetProtection.cpp
    sources/Excel/WorksheetDrawingHelpers.cpp
    sources/Excel/ExcelWorksheetContent.cpp
    sources/Excel/ExcelChart.cpp
    sources/Excel/ExcelCellValue.cpp
    sources/Excel/ExcelStyle.cpp
    sources/Excel/ExcelFormatting.cpp
    sources/Excel/StyleRepository.cpp
    sources/Excel/ExcelDocument.cpp
    sources/PowerPoint/PowerPointDocument.cpp
    sources/PowerPoint/PowerPointProtection.cpp
    sources/PowerPoint/PowerPointChart.cpp
    sources/Excel/WorksheetStructureHelpers.cpp
    sources/Excel/WorksheetMergeHelpers.cpp
    sources/MarkupCompatibility.cpp
    sources/MetadataBuilder.cpp
    sources/ValidationResult.cpp
    sources/Version.cpp
    sources/Tools/PackageModel.cpp
    sources/Tools/PackageLimits.cpp
    sources/Tools/OutputNaming.cpp
    sources/Tools/Report.cpp
    sources/Tools/PackageInspector.cpp
    sources/Tools/ValidationRunner.cpp
    sources/Tools/SignatureInspector.cpp
    sources/Tools/ExternalResourceInspector.cpp
    sources/Tools/PackageArchiver.cpp
    sources/Tools/FlatOpcConverter.cpp
    sources/Tools/MediaExporter.cpp
    sources/Tools/WordTextTools.cpp
    sources/Tools/WordDocumentTools.cpp
    sources/Tools/WordAutomationTools.cpp
    sources/Tools/DocumentTools.cpp
    sources/Tools/DocumentTextTools.cpp
    sources/Tools/DocumentRedactor.cpp
    sources/Tools/SpreadsheetTools.cpp
    sources/Tools/TextExtractor.cpp
    sources/Tools/PackageDiff.cpp
    sources/Tools/DocumentStats.cpp
    sources/Tools/DocumentModelJsonInternal.hpp
    sources/Tools/DocumentModelJson.cpp
    sources/Tools/DocumentModelSchema.cpp
    sources/Tools/DocumentModelXml.cpp
    sources/Tools/DocumentModelText.cpp
    sources/Tools/DocumentModelCsv.cpp
    sources/Tools/DocumentModelMarkdown.cpp
    sources/Tools/MarkdownDocument.cpp
    sources/Tools/WordModelIO.cpp
    sources/Tools/ExcelModelIO.cpp
    sources/Tools/PowerPointModelIO.cpp
    sources/Tools/DocumentConverter.cpp
    sources/Tools/XmlQueryTool.cpp
    sources/Tools/ResourceDeduplicator.cpp
    sources/Xml/XmlQuery.cpp
    sources/pugixml/pugiconfig.hpp
    sources/pugixml/pugixml.cpp
    sources/pugixml/pugixml.hpp
    sources/zip/miniz.h
    sources/zip/zip.cpp
    sources/zip/zip.h
    3rdparty/json-schema-validator/nlohmann/json-schema.hpp
    3rdparty/json-schema-validator/json-patch.hpp
    3rdparty/json-schema-validator/json-patch.cpp
    3rdparty/json-schema-validator/json-schema-draft7.json.cpp
    3rdparty/json-schema-validator/json-uri.cpp
    3rdparty/json-schema-validator/json-validator.cpp
    3rdparty/json-schema-validator/smtp-address-validator.hpp
    3rdparty/json-schema-validator/smtp-address-validator.cpp
    3rdparty/json-schema-validator/string-format-check.cpp
    sources/XmlNamespaceResolver.cpp
    sources/XmlNamespaceResolver.hpp
    sources/XmlParseOptions.hpp
    sources/CorePropertiesXml.cpp
    sources/CorePropertiesXml.hpp
    sources/ConformanceClass.hpp
    sources/OfficePasswordVerifier.cpp
    sources/OfficePasswordVerifier.hpp
    include/ExyokiOffice/Security/CryptoProvider.hpp
    include/ExyokiOffice/Security/ExternalResources.hpp
    include/ExyokiOffice/Security/PackageSignatures.hpp
    include/ExyokiOffice/Security/ResourceResolver.hpp
    sources/Security/CryptoProvider.cpp
    sources/Security/ExternalResourceGateway.cpp
    sources/Security/ExternalResourceGateway.hpp
    sources/Security/ExternalResources.cpp
    sources/Security/ExternalUri.cpp
    sources/Security/ExternalUri.hpp
    sources/Security/MessageDigest.cpp
    sources/Security/MessageDigest.hpp
    sources/Security/PackageSignatures.cpp
    sources/Security/ResourceResolver.cpp
    sources/Security/SignatureXml.cpp
    sources/Security/SignatureXml.hpp
    sources/Security/RelationshipTransform.cpp
    sources/Security/RelationshipTransform.hpp
    sources/Security/SignatureNames.hpp
    sources/Security/XmlCanonicalization.cpp
    sources/Security/XmlCanonicalization.hpp)

target_sources(ExyokiOffice PRIVATE
    ${GENERATED_SOURCES}
    ${GENERATED_HEADERS}
    ${GENERATED_PACKAGING_SOURCES}
    ${GENERATED_PACKAGING_HEADERS})
add_dependencies(ExyokiOffice generate_openxml)

exyokioffice_enable_strict_warnings(ExyokiOffice)

target_include_directories(ExyokiOffice PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_include_directories(ExyokiOffice PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_include_directories(ExyokiOffice PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/pugixml)
target_include_directories(ExyokiOffice PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/sources)
target_include_directories(ExyokiOffice SYSTEM PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty)
# json-schema-validator ships its public header as <nlohmann/json-schema.hpp>, so
# its own root joins the search path next to the one holding <nlohmann/json.hpp>.
target_include_directories(ExyokiOffice SYSTEM PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/json-schema-validator)

# Generate export header instead of exporting every symbol
include(GenerateExportHeader)
set(EXPORT_HEADER_PATH ${CMAKE_CURRENT_BINARY_DIR}/ExyokiOffice/Export.hpp)
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/Version.hpp.in
    ${CMAKE_CURRENT_BINARY_DIR}/ExyokiOffice/Version.hpp
    @ONLY)
generate_export_header(ExyokiOffice
    BASE_NAME ExyokiOffice
    EXPORT_FILE_NAME ${EXPORT_HEADER_PATH})

if(EXYOKIOFFICE_LIBRARY_KIND STREQUAL "STATIC")
    # Makes the generated header expand EXYOKIOFFICE_EXPORT to nothing;
    # __declspec(dllimport) on a static library is an error.
    target_compile_definitions(ExyokiOffice PUBLIC EXYOKIOFFICE_STATIC_DEFINE)
endif()

exyokioffice_enable_fuzzing_instrumentation(ExyokiOffice)

if(WIN32)
    set(RC_FILE_DESCRIPTION "ExyokiOffice OpenXML library")
    set(RC_ORIGINAL_FILENAME "ExyokiOffice.dll")
    set(RC_INTERNAL_NAME "ExyokiOffice")
    set(RC_FILE_TYPE "VFT_DLL")
    configure_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/cmake/VersionInfo.rc.in
        ${CMAKE_CURRENT_BINARY_DIR}/ExyokiOffice/ExyokiOffice.rc
        @ONLY)
    target_sources(ExyokiOffice PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/ExyokiOffice/ExyokiOffice.rc)
endif()
# Define version and installation directories
include(CMakePackageConfigHelpers)
set_target_properties(ExyokiOffice PROPERTIES
    VERSION ${ExyokiOfficeVersion}
    SOVERSION ${ExyokiOfficeAbiVersion}
    EXPORT_NAME ExyokiOffice)
add_library(ExyokiOffice::ExyokiOffice ALIAS ExyokiOffice)

install(TARGETS ExyokiOffice
        EXPORT ExyokiOfficeTargets
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/ExyokiOffice
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
        FILES_MATCHING
        PATTERN "*.hpp"
        PATTERN "*.h")
install(FILES
        ${EXPORT_HEADER_PATH}
        ${CMAKE_CURRENT_BINARY_DIR}/ExyokiOffice/Version.hpp
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/ExyokiOffice)

configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/ExyokiOfficeConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/ExyokiOfficeConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ExyokiOffice)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/ExyokiOfficeConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMinorVersion)
install(EXPORT ExyokiOfficeTargets
        FILE ExyokiOfficeTargets.cmake
        NAMESPACE ExyokiOffice::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ExyokiOffice)
install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/ExyokiOfficeConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/ExyokiOfficeConfigVersion.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ExyokiOffice)
install(FILES
        LICENSE
        THIRD-PARTY-LICENSES.md
        README.md
        CHANGELOG.md
        SECURITY.md
        AGENTS.md
        CONTRIBUTING.md
        llms.txt
        llms-full.txt
        DESTINATION ${CMAKE_INSTALL_DOCDIR})

# Every vendored component compiled into an installed binary, with the notice
# its license requires a binary distribution to carry — BSD-3-Clause says so in
# as many words for CLI11, and MIT asks for it in every substantial portion.
# THIRD-PARTY-LICENSES.md above explains what is in which binary; these are the
# upstream texts themselves, one file per component because they all happen to
# be called LICENSE. doctest is absent on purpose: it is linked into the test
# executables only, and none of those are installed.
install(FILES 3rdparty/CLI11/LICENSE
        RENAME CLI11-LICENSE
        DESTINATION ${CMAKE_INSTALL_DOCDIR}/licenses)
install(FILES 3rdparty/nlohmann/LICENSE.MIT
        RENAME nlohmann-json-LICENSE
        DESTINATION ${CMAKE_INSTALL_DOCDIR}/licenses)
install(FILES 3rdparty/json-schema-validator/LICENSE
        RENAME json-schema-validator-LICENSE
        DESTINATION ${CMAKE_INSTALL_DOCDIR}/licenses)
install(FILES sources/pugixml/LICENSE
        RENAME pugixml-LICENSE
        DESTINATION ${CMAKE_INSTALL_DOCDIR}/licenses)
install(FILES sources/zip/LICENSE
        RENAME zip-miniz-LICENSE
        DESTINATION ${CMAKE_INSTALL_DOCDIR}/licenses)
install(FILES data/LICENSE
        RENAME Open-XML-SDK-data-LICENSE
        DESTINATION ${CMAKE_INSTALL_DOCDIR}/licenses)
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/docs
        DESTINATION ${CMAKE_INSTALL_DOCDIR}
        FILES_MATCHING
        PATTERN "*.md"
        PATTERN "_*" EXCLUDE)

if(WIN32 AND EXYOKIOFFICE_DUMP_EXPORTS)
    set(EXPORTS_RAW ${CMAKE_CURRENT_BINARY_DIR}/exports.txt)
    set(EXPORTS_DEMANGLED ${CMAKE_CURRENT_BINARY_DIR}/exports_demangled.txt)

    add_custom_command(
        OUTPUT ${EXPORTS_RAW} ${EXPORTS_DEMANGLED}
        COMMAND ${CMAKE_COMMAND} -E echo "Generating export list for ExyokiOffice.dll"
        COMMAND "dumpbin" "/EXPORTS" "${CMAKE_CURRENT_BINARY_DIR}/$<TARGET_FILE_NAME:ExyokiOffice>" "/OUT:${EXPORTS_RAW}" 
        COMMAND ${CMAKE_COMMAND} -E echo "Undecorating export names"
        COMMAND undname "${EXPORTS_RAW}" > "${EXPORTS_DEMANGLED}"
        DEPENDS ExyokiOffice
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
        COMMENT "Dumping and demangling ExyokiOffice exports"
        VERBATIM
    )

    add_custom_target(generate_exports_list ALL DEPENDS ${EXPORTS_DEMANGLED})
endif()

# Enabled before the examples are added, because each example registers itself
# as a CTest smoke test and add_test() only takes effect after enable_testing().
# The Python suite builds nothing, so it registers its entry without the doctest
# layers and needs testing enabled on its own account.
if(EXYOKIOFFICE_BUILD_UNIT_TESTS OR EXYOKIOFFICE_BUILD_MCP_PYTHON_TESTS)
    enable_testing()
endif()

if(EXYOKIOFFICE_BUILD_EXAMPLE_WORD)
    add_subdirectory(examples/ExampleWord)
endif()

if(EXYOKIOFFICE_BUILD_EXAMPLE_WORD_EDITOR)
    add_subdirectory(examples/ExampleWordEditor)
endif()

if(EXYOKIOFFICE_BUILD_EXAMPLE_WORD_EDIT)
    add_subdirectory(examples/ExampleWordEdit)
endif()

if(EXYOKIOFFICE_BUILD_EXAMPLE_EXCEL_EDITOR)
    add_subdirectory(examples/ExampleExcelEditor)
endif()

if(EXYOKIOFFICE_BUILD_EXAMPLE_POWERPOINT_EDITOR)
    add_subdirectory(examples/ExamplePowerPointEditor)
endif()

# Added for both options: the fuzz entry points are a plain library that the
# unit tests replay over the committed corpus, so they are needed even when no
# libFuzzer executable is built.
if(EXYOKIOFFICE_BUILD_UNIT_TESTS OR EXYOKIOFFICE_BUILD_FUZZERS)
    add_subdirectory(tests/fuzz)
endif()

# The test suite is split into layers, one executable each, and every layer
# registers its CTest entries with labels through cmake/Tests.cmake. See
# docs/Compatibility.md for what the labels mean and how the matrix maps onto
# them.
if(EXYOKIOFFICE_BUILD_UNIT_TESTS)
    include(cmake/Tests.cmake)

    add_subdirectory(tests/support)

    add_subdirectory(tests/unit)
    add_subdirectory(tests/dom)
    add_subdirectory(tests/package)
    add_subdirectory(tests/word)
    add_subdirectory(tests/spreadsheet)
    add_subdirectory(tests/presentation)
    add_subdirectory(tests/tools)
    add_subdirectory(tests/generator)
    add_subdirectory(tests/compat)
    add_subdirectory(tests/corpus)
endif()

if(EXYOKIOFFICE_BUILD_TOOL)
    add_subdirectory(tools/exyoki)
endif()

# The MCP servers link the same public API as exyoki and are built after it, so
# the catalog and replay CTest entries can refer to the finished binaries.
if(EXYOKIOFFICE_BUILD_MCP)
    add_subdirectory(tools/mcp)

    if(EXYOKIOFFICE_BUILD_UNIT_TESTS)
        add_subdirectory(tests/mcp)
    endif()
endif()
