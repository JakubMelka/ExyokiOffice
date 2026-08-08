# Copyright (c) 2026 Jakub Melka and Collaborators
# SPDX-License-Identifier: MIT
# See LICENSE file in the project root for full license text.

include_guard(GLOBAL)

# Declares one example executable.
#
# Every example is a single main.cpp linked against the shared library, and
# every example also doubles as a smoke test: each one writes a document, opens
# it again, and returns non-zero when anything fails. Registering them with
# CTest keeps a broken example from rotting unnoticed, because nothing else in
# the build compiles them into an assertion.
#
# The test runs in its own directory under ${CMAKE_BINARY_DIR}/example-output so
# the generated documents never land in the source tree and two examples cannot
# overwrite each other's files. On Windows the shared library is copied next to
# the executable, because the working directory is not on the DLL search path.
#
# LABELS names the families the example demonstrates, so `ctest -L word` picks
# up the Word examples along with the Word test layer. Every example also
# carries the "example" label.
function(exyokioffice_add_example target)
    cmake_parse_arguments(arg "" "" "LABELS" ${ARGN})

    add_executable(${target} main.cpp)
    target_link_libraries(${target} PRIVATE ExyokiOffice)

    # Shared example helpers are included as "Common/SampleImage.hpp".
    target_include_directories(${target} PRIVATE ${PROJECT_SOURCE_DIR}/examples)

    if(WIN32)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    $<TARGET_FILE:ExyokiOffice>
                    $<TARGET_FILE_DIR:${target}>)
    endif()

    if(EXYOKIOFFICE_BUILD_UNIT_TESTS)
        set(workingDirectory "${CMAKE_BINARY_DIR}/example-output/${target}")
        file(MAKE_DIRECTORY "${workingDirectory}")
        add_test(NAME Example.${target} COMMAND ${target})
        set_tests_properties(Example.${target} PROPERTIES
            WORKING_DIRECTORY "${workingDirectory}"
            LABELS "example;${arg_LABELS}")
    endif()
endfunction()
