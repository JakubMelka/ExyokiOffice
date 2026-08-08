# Copyright (c) 2026 Jakub Melka and Collaborators
# SPDX-License-Identifier: MIT
# See LICENSE file in the project root for full license text.

# Checks that `exyoki commands` describes the whole command line interface.
#
# Run as:
#   cmake -DEXECUTABLE=<path to exyoki> -P CheckCommandCatalog.cmake
#
# The catalog exists so that a program — an agent, a CI script, a wrapper — can
# discover exyoki's commands without parsing --help. That promise breaks in two
# quiet ways: the catalog stops listing something --help still offers, or its
# exit code table names a command that no longer exists. Neither shows up as a
# crash, so this script runs both views of the interface and compares them.

if(NOT EXECUTABLE)
    message(FATAL_ERROR "CheckCommandCatalog.cmake needs EXECUTABLE")
endif()

# Runs exyoki and returns its stdout, failing on any exit code but zero.
function(run_exyoki outputVariable)
    execute_process(
        COMMAND ${EXECUTABLE} ${ARGN}
        OUTPUT_VARIABLE output
        ERROR_VARIABLE errors
        RESULT_VARIABLE status)

    if(NOT status EQUAL 0)
        message(FATAL_ERROR "'${EXECUTABLE} ${ARGN}' failed: ${status}\n${errors}")
    endif()

    set(${outputVariable} "${output}" PARENT_SCOPE)
endfunction()

# A non-zero exit or a non-ok status here means the catalog found itself
# inconsistent, which it reports as a diagnostic rather than a crash.
run_exyoki(catalog commands --format json)

if(NOT catalog MATCHES "\"status\": \"ok\"")
    message(FATAL_ERROR "exyoki commands did not report status ok:\n${catalog}")
endif()

if(NOT catalog MATCHES "\"schemaVersion\": ([0-9]+)")
    message(FATAL_ERROR "exyoki commands emitted no schemaVersion:\n${catalog}")
endif()

run_exyoki(helpText --help)

# CLI11 lists every top-level command in a SUBCOMMANDS section, one per line,
# with the name in the first column and any wrapped description indented far
# past it.
string(REPLACE ";" "\\;" helpText "${helpText}")
string(REPLACE "\n" ";" helpLines "${helpText}")

set(inSubcommands FALSE)
set(commandNames "")
foreach(line IN LISTS helpLines)
    if(line MATCHES "^SUBCOMMANDS:")
        set(inSubcommands TRUE)
        continue()
    endif()
    if(NOT inSubcommands)
        continue()
    endif()
    if(line MATCHES "^  ([A-Za-z][A-Za-z0-9_-]*)([ ]|$)")
        list(APPEND commandNames ${CMAKE_MATCH_1})
    endif()
endforeach()

list(LENGTH commandNames commandCount)
if(commandCount LESS 2)
    message(FATAL_ERROR "no subcommands were found in exyoki --help:\n${helpText}")
endif()

set(missing "")
foreach(name IN LISTS commandNames)
    if(NOT catalog MATCHES "\"path\": \"${name}\"")
        list(APPEND missing ${name})
    endif()
endforeach()

if(missing)
    string(REPLACE ";" ", " missing "${missing}")
    message(FATAL_ERROR
        "exyoki commands does not describe every command --help offers.\n"
        "  missing: ${missing}\n"
        "The catalog is built from the live CLI11 parser, so a command can only "
        "go missing through the filtering in tools/exyoki/CommandCatalog.cpp.")
endif()

message(STATUS "exyoki commands describes all ${commandCount} top-level command(s).")
