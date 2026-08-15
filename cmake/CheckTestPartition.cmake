# Copyright (c) 2026 Jakub Melka and Contributors
# SPDX-License-Identifier: MIT
# See LICENSE file in the project root for full license text.

# Checks that a test layer's areas partition its test cases.
#
# Run as:
#   cmake -DEXECUTABLE=<path> -DAREA_FILTERS=<f1|f2|…> -DEXCLUDE_FILTER=<f>
#         -P CheckTestPartition.cmake
#
# Each area of a layer is a CTest entry that runs the layer executable with a
# doctest filter, and the layer's ".Other" entry runs everything the areas did
# not claim. That only adds up when the area tags are disjoint: a case carrying
# two area tags is run twice, which inflates the suite silently because both
# runs pass. This script counts the cases each filter selects — with
# --list-test-cases, so nothing is actually executed — and fails when the parts
# do not sum to the whole.
#
# It is cheap enough to be a normal CTest entry, and it fails at the moment a
# tag overlap is introduced rather than the next time somebody adds up the
# numbers by hand.

if(NOT EXECUTABLE OR NOT DEFINED AREA_FILTERS)
    message(FATAL_ERROR "CheckTestPartition.cmake needs EXECUTABLE and AREA_FILTERS")
endif()

# Runs the executable in listing mode and returns how many cases pass the filter.
function(count_cases outputVariable)
    execute_process(
        COMMAND ${EXECUTABLE} --list-test-cases ${ARGN}
        OUTPUT_VARIABLE listing
        ERROR_VARIABLE errors
        RESULT_VARIABLE status)

    if(NOT status EQUAL 0)
        message(FATAL_ERROR "'${EXECUTABLE} --list-test-cases ${ARGN}' failed: ${status}\n${errors}")
    endif()

    if(NOT listing MATCHES "unskipped test cases passing the current filters: ([0-9]+)")
        message(FATAL_ERROR "unexpected doctest listing output:\n${listing}")
    endif()

    set(${outputVariable} ${CMAKE_MATCH_1} PARENT_SCOPE)
endfunction()

count_cases(total)

set(claimed 0)
string(REPLACE "|" ";" areaFilters "${AREA_FILTERS}")
foreach(filter IN LISTS areaFilters)
    if(filter STREQUAL "")
        continue()
    endif()
    count_cases(count --test-case=${filter})
    math(EXPR claimed "${claimed} + ${count}")
endforeach()

if(EXCLUDE_FILTER STREQUAL "")
    set(other 0)
else()
    count_cases(other --test-case-exclude=${EXCLUDE_FILTER})
endif()

math(EXPR covered "${claimed} + ${other}")

if(NOT covered EQUAL total)
    math(EXPR difference "${covered} - ${total}")
    message(FATAL_ERROR
        "Test areas do not partition this layer.\n"
        "  areas claim   ${claimed} case(s)\n"
        "  .Other adds   ${other} case(s)\n"
        "  sum           ${covered}\n"
        "  layer has     ${total}\n"
        "A positive difference (${difference}) means at least one test case carries the tags "
        "of two areas and is therefore run twice; a negative one means cases are unreachable. "
        "Make the area tags disjoint.")
endif()

message(STATUS "${total} test case(s) partitioned across ${claimed} claimed and ${other} unclaimed.")
