# Copyright (c) 2026 Jakub Melka and Collaborators
# SPDX-License-Identifier: MIT
# See LICENSE file in the project root for full license text.

# Checks that running a tool with a given command line produces a given exit code.
#
# Run as:
#   cmake -DEXECUTABLE=<path> "-DARGUMENTS=<a;b;c>" -DEXPECTED_CODE=<n>
#         -P CheckToolExitCode.cmake
#
# CTest itself can only distinguish zero from non-zero, and the exit codes are a
# published contract with nine distinct values - "matched nothing" and "could
# not read the package" are both non-zero and mean opposite things to a script.
# The value has to be compared rather than merely observed to be non-zero.

if(NOT EXECUTABLE)
    message(FATAL_ERROR "CheckToolExitCode.cmake needs EXECUTABLE")
endif()

if(NOT DEFINED EXPECTED_CODE)
    message(FATAL_ERROR "CheckToolExitCode.cmake needs EXPECTED_CODE")
endif()

execute_process(
    COMMAND ${EXECUTABLE} ${ARGUMENTS}
    OUTPUT_VARIABLE output
    ERROR_VARIABLE errors
    RESULT_VARIABLE status)

if(NOT status EQUAL EXPECTED_CODE)
    message(FATAL_ERROR
        "'${EXECUTABLE} ${ARGUMENTS}' exited with ${status}, expected ${EXPECTED_CODE}.\n"
        "stdout:\n${output}\nstderr:\n${errors}")
endif()
