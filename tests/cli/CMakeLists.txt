# Copyright (c) 2026 Jakub Melka and Contributors
# SPDX-License-Identifier: MIT
# See LICENSE file in the project root for full license text.

# The exyoki command line. The cases link exyoki_core and call the tool's own
# code in process: BuildCommandLine for what a command line means, and one
# Run() per command for what it does. That reaches everything the binary does
# except main() itself, which is one call to RunCommandLine.
#
# The exit codes are the reason this layer exists. They are a published
# contract - `exyoki commands` reports the table, and scripts branch on it - so
# "matched nothing" against "could not search", or "different" against "failed
# to compare", are checked per command rather than left to the reviewer.
exyokioffice_add_test_executable(ExyokiOfficeCliTests
    LAYER cli
    PREFIX Cli
    SOURCES
        CliTestSupport.hpp
        CliTestSupport.cpp
        CliParserTests.cpp
        CliInspectionCommandTests.cpp
        CliDocumentCommandTests.cpp
        CliReportTests.cpp
        CliAdapterTests.cpp
    LINK exyoki_core)

exyokioffice_add_test_area(cli
    NAME Cli.Parser
    LABELS cli-parser
    TAGS "[cli-parser]")

exyokioffice_add_test_area(cli
    NAME Cli.Inspection
    LABELS cli-inspection
    TAGS "[cli-inspect]")

exyokioffice_add_test_area(cli
    NAME Cli.Documents
    LABELS cli-documents
    TAGS "[cli-document]")

exyokioffice_add_test_area(cli
    NAME Cli.Report
    LABELS cli-report
    TAGS "[cli-report]")

exyokioffice_finalize_test_layer(cli)

# What the in-process cases cannot reach: that the built executable starts, that
# main() hands its arguments to RunCommandLine, and that the process exit code
# is the one the command returned. Everything below the entry point is covered
# by the cases above, so this stays a handful of entries rather than a second
# suite. Tool.CommandCatalog and Tool.Completions in tools/exyoki/CMakeLists.txt
# check the same binary from the same angle.
function(exyoki_add_binary_case name expectedCode)
    add_test(NAME Cli.Binary.${name} COMMAND ${CMAKE_COMMAND}
        -DEXECUTABLE=$<TARGET_FILE:exyoki>
        "-DARGUMENTS=${ARGN}"
        -DEXPECTED_CODE=${expectedCode}
        -P ${PROJECT_SOURCE_DIR}/cmake/CheckToolExitCode.cmake)
    set_tests_properties(Cli.Binary.${name} PROPERTIES LABELS "cli;cli-binary")
endfunction()

exyoki_add_binary_case(Version 0 --version)
exyoki_add_binary_case(Help 0 --help)
exyoki_add_binary_case(NoCommand 2)
exyoki_add_binary_case(UnknownCommand 2 no-such-command)
exyoki_add_binary_case(MissingArgument 2 parts)
exyoki_add_binary_case(MissingPackage 1 parts no-such-package-cli-test.docx)
