# Copyright (c) 2026 Jakub Melka and Contributors
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

# Fail here, with the version that is needed, rather than several hundred
# translation units later with a page of errors from <format> or <charconv>.
# The numbers are the oldest toolchains the library is actually built and
# tested with; README.md states the same set.
set(ExyokiOfficeMinimumCompiler "")
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    set(ExyokiOfficeMinimumCompiler "13")
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    set(ExyokiOfficeMinimumCompiler "17")
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    # 19.30 is Visual Studio 2022 17.0.
    set(ExyokiOfficeMinimumCompiler "19.30")
endif()

if(ExyokiOfficeMinimumCompiler AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS ExyokiOfficeMinimumCompiler)
    message(FATAL_ERROR
        "ExyokiOffice needs ${CMAKE_CXX_COMPILER_ID} ${ExyokiOfficeMinimumCompiler} or newer for its C++20 "
        "library features (<format>, floating-point <charconv>); this is "
        "${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}. See the platform table in README.md.")
endif()

if(APPLE)
    # No macOS build is exercised by CI or by the presets. The known obstacle is
    # libc++: floating-point std::from_chars arrived in LLVM 20, so an older
    # Apple toolchain fails to compile sources/OpenXmlSimpleTypes.cpp. Reported
    # as a warning rather than an error because a recent enough libc++, or
    # libstdc++ from Homebrew GCC, does compile it.
    message(WARNING
        "macOS is not a tested platform for ExyokiOffice. Building needs a libc++ with "
        "floating-point std::from_chars (LLVM 20 or newer) or Homebrew GCC 13+. "
        "Please report what you find.")
endif()

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
option(EXYOKIOFFICE_BUILD_EXAMPLE_WORD_DEMO "Build the ExampleWordDemo sample" ON)
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
# One executable holding the library and every test layer, for coverage
# measurement without module seams; see exyokioffice_add_test_monolith in
# cmake/Tests.cmake. Off by default because it roughly doubles what a full
# build compiles.
option(EXYOKIOFFICE_TEST_MONOLITH
    "Build ExyokiOfficeMonolithTests, the single-executable coverage target" OFF)
# The generated sources are committed, so a build does not need the generator to
# produce them - it reruns it to keep them in step with data/ and gen/. That is
# what someone working on this repository wants and what nobody else does: the
# generator writes into the *source* tree, so a build that is not a developer
# build would either fail on a read-only checkout (Nix, distro packaging) or
# leave regenerated files in a clone the user only meant to compile.
#
# The default therefore has to be derived rather than fixed. It is on when this
# is the top-level project and the source tree can be written to, which is
# precisely the developer case; consuming ExyokiOffice through add_subdirectory
# or FetchContent, packaging it, or cross compiling all land on off, and each of
# those uses the committed files as they are. Setting the option explicitly
# always wins.
#
# Cross compiling is off for a second reason: the generator is built for the
# target, and a build that runs it would be running a target binary on the host.
if(PROJECT_IS_TOP_LEVEL AND NOT CMAKE_CROSSCOMPILING)
    # Tested rather than assumed: a checkout can be read-only for reasons no
    # path tells CMake about, and the answer decides whether the build works at
    # all. The probe runs as a child process because file(WRITE) on a read-only
    # tree is a fatal error - it would abort the configure this is trying to
    # keep working.
    set(_exyokioffice_write_probe "${CMAKE_CURRENT_SOURCE_DIR}/sources/DOM/.exyokioffice-write-probe")
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E touch "${_exyokioffice_write_probe}"
        RESULT_VARIABLE _exyokioffice_write_probe_result
        OUTPUT_QUIET
        ERROR_QUIET)
    if(_exyokioffice_write_probe_result EQUAL 0)
        set(_exyokioffice_run_generator_default ON)
        file(REMOVE "${_exyokioffice_write_probe}")
    else()
        set(_exyokioffice_run_generator_default OFF)
        message(STATUS
            "ExyokiOffice: the source tree is not writable, so the OpenXML generator will not run; "
            "the committed generated sources are used as they are.")
    endif()
    unset(_exyokioffice_write_probe)
    unset(_exyokioffice_write_probe_result)
else()
    set(_exyokioffice_run_generator_default OFF)
endif()

option(EXYOKIOFFICE_RUN_GENERATOR
    "Run OpenXmlGenerator during the build and refresh the committed generated sources"
    ${_exyokioffice_run_generator_default})
unset(_exyokioffice_run_generator_default)

include(cmake/CompilerWarnings.cmake)
include(cmake/RuntimeLibraryCopy.cmake)
include(cmake/Sanitizers.cmake)
# After Sanitizers.cmake: the coverage runtime is located with the
# exyokioffice_clang_runtime_dir helper defined there.
include(cmake/Coverage.cmake)
include(cmake/Fuzzing.cmake)
include(cmake/Examples.cmake)

# Must run before any target is declared: the sanitizer flags are directory
# scoped and have to reach the generator, the library, the examples and the
# tests alike.
exyokioffice_apply_sanitizers()

# Directory scoped for the same reason, and for the additional one that a
# translation unit compiled without the coverage mapping is missing from the
# report rather than reported as uncovered.
exyokioffice_apply_coverage()

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

# tests/generator links generator_support, so the directory is still needed when
# the generator itself is not run.
if(EXYOKIOFFICE_RUN_GENERATOR OR EXYOKIOFFICE_BUILD_UNIT_TESTS)
    add_subdirectory(gen)
endif()

set(GENERATED_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/include/ExyokiOffice/DOM)
set(GENERATED_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/sources/DOM)
file(GLOB_RECURSE GENERATED_HEADERS CONFIGURE_DEPENDS "${GENERATED_INCLUDE_DIR}/*.hpp")
file(GLOB_RECURSE GENERATED_SOURCES CONFIGURE_DEPENDS "${GENERATED_SOURCE_DIR}/*.cpp")
set(GENERATED_PACKAGING_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/include/ExyokiOffice/Packaging)
set(GENERATED_PACKAGING_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/sources/Packaging)
file(GLOB GENERATED_PACKAGING_HEADERS CONFIGURE_DEPENDS "${GENERATED_PACKAGING_INCLUDE_DIR}/*.hpp")
file(GLOB GENERATED_PACKAGING_SOURCES CONFIGURE_DEPENDS "${GENERATED_PACKAGING_SOURCE_DIR}/*.cpp")

# Counted rather than listed. There are hundreds of each, and a configure step
# that prints them all buries every other message it emits - including the
# FATAL_ERROR below, which is the one a reader actually has to see.
list(LENGTH GENERATED_HEADERS GENERATED_HEADER_COUNT)
list(LENGTH GENERATED_SOURCES GENERATED_SOURCE_COUNT)
message(STATUS "Generated DOM: ${GENERATED_HEADER_COUNT} header(s), ${GENERATED_SOURCE_COUNT} source(s)")

if(EXYOKIOFFICE_RUN_GENERATOR)
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
elseif(NOT GENERATED_SOURCES OR NOT GENERATED_PACKAGING_SOURCES)
    # The library is assembled from a glob, so an absent generated tree would
    # otherwise produce a library that links and is missing most of its API.
    message(FATAL_ERROR
        "EXYOKIOFFICE_RUN_GENERATOR is OFF but the committed generated sources are "
        "missing from ${GENERATED_SOURCE_DIR} and ${GENERATED_PACKAGING_SOURCE_DIR}. "
        "Build from a complete checkout, or turn the option on to generate them.")
endif()

# A fuzz build links the library statically. libFuzzer keeps its coverage
# counters and its runtime state in one instance per process; with the library
# in a DLL the instrumented code and the runtime end up on opposite sides of a
# module boundary, and on Windows the sanitizer coverage symbols then fail to
# resolve at all. Linking statically puts the whole fuzzed surface into the fuzz
# executable, which is also how the coverage feedback is meant to work.
#
# Otherwise BUILD_SHARED_LIBS decides, which is what package managers set. It is
# read rather than passed to add_library() directly so that leaving it undefined
# keeps the shared library the presets, WinBuild.ps1 and docs/ABI.md all describe.
if(EXYOKIOFFICE_BUILD_FUZZERS)
    set(EXYOKIOFFICE_LIBRARY_KIND STATIC)
elseif(DEFINED BUILD_SHARED_LIBS AND NOT BUILD_SHARED_LIBS)
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
    include/ExyokiOffice/TextPattern.hpp
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
    sources/TextPattern.cpp
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
if(EXYOKIOFFICE_RUN_GENERATOR)
    add_dependencies(ExyokiOffice generate_openxml)
endif()

exyokioffice_enable_strict_warnings(ExyokiOffice)

# CMAKE_CXX_STANDARD is a directory variable and does not travel with the
# exported target; the compile feature is what a consumer inherits.
target_compile_features(ExyokiOffice PUBLIC cxx_std_20)

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

# The export header only decides anything once the default is "not exported".
# That is already true for a Windows DLL, but an ELF shared object exports
# every symbol it has unless the visibility default is flipped, which would
# leave EXYOKIOFFICE_EXPORT decorative on those platforms and put every
# file-local helper into the dynamic symbol table. Setting it here makes the
# two platforms agree on what the public surface is.
#
# Position-independent code is set for both library kinds, not only the shared
# one. CMake turns it on for a SHARED target by itself; a STATIC target gets it
# only when asked, and a static archive without it cannot be linked into
# somebody else's shared object at all on ELF platforms. Consumers that vendor
# this library into a plugin are exactly the case a static build is for.
set_target_properties(ExyokiOffice PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    C_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    POSITION_INDEPENDENT_CODE ON)
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

if(EXYOKIOFFICE_BUILD_FUZZERS)
    # A mutated .docx fails the per-entry CRC32 before miniz hands over a single
    # byte, so the raw-package target spends its whole budget on archive framing
    # and the loader below it is never reached. Turning the check off in the fuzz
    # build alone lets the mutated bytes through to the code being fuzzed; the
    # bytes that arrive are ones a well-formed archive could have carried, so the
    # findings are real rather than artifacts of the define.
    #
    # Fuzz builds only. A CRC is how a shipped build tells damage from data.
    target_compile_definitions(ExyokiOffice PRIVATE MINIZ_DISABLE_ZIP_READER_CRC32_CHECKS)
endif()

if(WIN32)
    set(RC_FILE_DESCRIPTION "ExyokiOffice OpenXML library")
    set(RC_INTERNAL_NAME "ExyokiOffice")
    if(EXYOKIOFFICE_LIBRARY_KIND STREQUAL "SHARED")
        set(RC_ORIGINAL_FILENAME "ExyokiOffice.dll")
        set(RC_FILE_TYPE "VFT_DLL")
    else()
        set(RC_ORIGINAL_FILENAME "ExyokiOffice.lib")
        set(RC_FILE_TYPE "VFT_STATIC_LIB")
    endif()
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

# Not in a fuzz build, and not by each example saying so. The library carries
# coverage instrumentation there, and the __sanitizer_cov_* symbols it emits are
# defined only by the libFuzzer runtime - which carries main() and can therefore
# be linked into a fuzz executable and nowhere else. An example linking the
# library is not one, so it cannot link at all.
#
# Stated once, here, rather than as a switch per example in every fuzz preset:
# that list already missed ExampleWordEdit, and a preset that has to be edited
# whenever an example is added is a fuzz build that breaks without being touched.
if(EXYOKIOFFICE_BUILD_FUZZERS)
    message(STATUS "Fuzzing - examples are not built: instrumented code needs the libFuzzer runtime to link")
else()
    if(EXYOKIOFFICE_BUILD_EXAMPLE_WORD)
        add_subdirectory(examples/ExampleWord)
    endif()

    if(EXYOKIOFFICE_BUILD_EXAMPLE_WORD_EDITOR)
        add_subdirectory(examples/ExampleWordEditor)
    endif()

    if(EXYOKIOFFICE_BUILD_EXAMPLE_WORD_EDIT)
        add_subdirectory(examples/ExampleWordEdit)
    endif()

    if(EXYOKIOFFICE_BUILD_EXAMPLE_WORD_DEMO)
        add_subdirectory(examples/ExampleWordDemo)
    endif()

    if(EXYOKIOFFICE_BUILD_EXAMPLE_EXCEL_EDITOR)
        add_subdirectory(examples/ExampleExcelEditor)
    endif()

    if(EXYOKIOFFICE_BUILD_EXAMPLE_POWERPOINT_EDITOR)
        add_subdirectory(examples/ExamplePowerPointEditor)
    endif()
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

    # The CLI layer calls the command implementations in process and runs the
    # built binary, so it needs the tool to have been added first.
    if(EXYOKIOFFICE_BUILD_UNIT_TESTS)
        add_subdirectory(tests/cli)
    endif()
endif()

# The MCP servers link the same public API as exyoki and are built after it, so
# the catalog and replay CTest entries can refer to the finished binaries.
if(EXYOKIOFFICE_BUILD_MCP)
    add_subdirectory(tools/mcp)

    if(EXYOKIOFFICE_BUILD_UNIT_TESTS)
        add_subdirectory(tests/mcp)
    endif()
endif()

# Last on purpose: the monolith folds in every layer registered above,
# including tests/mcp.
if(EXYOKIOFFICE_TEST_MONOLITH)
    if(NOT EXYOKIOFFICE_BUILD_UNIT_TESTS)
        message(FATAL_ERROR
            "EXYOKIOFFICE_TEST_MONOLITH=ON needs EXYOKIOFFICE_BUILD_UNIT_TESTS=ON; "
            "the monolith is assembled from the test layers.")
    endif()
    exyokioffice_add_test_monolith()
endif()
