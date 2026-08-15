# Copyright (c) 2026 Jakub Melka and Contributors
# SPDX-License-Identifier: MIT
# See LICENSE file in the project root for full license text.

if(WIN32)
    set(RC_FILE_DESCRIPTION "exyoki - ExyokiOffice command-line utility")
    set(RC_ORIGINAL_FILENAME "exyoki.exe")
    set(RC_INTERNAL_NAME "exyoki")
    set(RC_FILE_TYPE "VFT_APP")
    configure_file(
        ${PROJECT_SOURCE_DIR}/cmake/VersionInfo.rc.in
        ${CMAKE_CURRENT_BINARY_DIR}/exyoki.rc
        @ONLY)
    set(EXYOKI_RC_FILE ${CMAKE_CURRENT_BINARY_DIR}/exyoki.rc)
endif()

# Everything the tool does, with main() left out: the command implementations,
# the report adapters, the catalog and the completion generator.
#
# It is a library rather than a set of sources compiled straight into the
# executable so that tests/cli can call the commands in process - a command is
# a structure with its options as fields and one Run(), which asserts far more
# directly than spawning exyoki and matching its output would. What only the
# executable has is argument parsing, and tests/cli covers that by running the
# binary; see the CTest entries at the end of this file.
add_library(exyoki_core STATIC
    Adapters.hpp
    Adapters.cpp
    CommandCatalog.hpp
    CommandCatalog.cpp
    CommandLine.hpp
    CommandLine.cpp
    Commands.hpp
    Commands.cpp
    Completions.hpp
    Completions.cpp
    ExitCodes.hpp)

target_link_libraries(exyoki_core PUBLIC ExyokiOffice)

# PUBLIC: a consumer includes "Commands.hpp", which includes CLI11.
target_include_directories(exyoki_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(exyoki_core PRIVATE ${PROJECT_SOURCE_DIR}/sources)
target_include_directories(exyoki_core SYSTEM PUBLIC
    ${PROJECT_SOURCE_DIR}/3rdparty/CLI11
    ${PROJECT_SOURCE_DIR}/3rdparty)

exyokioffice_enable_strict_warnings(exyoki_core)

add_executable(exyoki
    main.cpp
    ${EXYOKI_RC_FILE})

target_link_libraries(exyoki PRIVATE exyoki_core)

exyokioffice_enable_strict_warnings(exyoki)

install(TARGETS exyoki RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# The installed tree is relocatable - the create_install workflow zips a prefix
# that is extracted anywhere. On Windows the shared library sits next to the
# executable and is found by the default search; elsewhere it lands in the
# library directory, so the tool needs a loader-relative RPATH to reach it
# without LD_LIBRARY_PATH.
if(NOT WIN32)
    file(RELATIVE_PATH EXYOKI_INSTALLED_LIBRARY_PATH
        ${CMAKE_INSTALL_FULL_BINDIR} ${CMAKE_INSTALL_FULL_LIBDIR})
    if(APPLE)
        set(EXYOKI_LOADER_ORIGIN "@loader_path")
    else()
        set(EXYOKI_LOADER_ORIGIN "$ORIGIN")
    endif()
    set_target_properties(exyoki PROPERTIES
        INSTALL_RPATH "${EXYOKI_LOADER_ORIGIN}/${EXYOKI_INSTALLED_LIBRARY_PATH}")
endif()

# Only a shared build has a runtime file to place next to the executable; a
# static one links it in, and the helper is a no-op there.
exyokioffice_copy_runtime_library(exyoki)

# `exyoki commands` is the machine-readable description of this interface, so it
# has to stay complete on its own. See cmake/CheckCommandCatalog.cmake for what
# the entry compares and why that cannot be left to review.
if(EXYOKIOFFICE_BUILD_UNIT_TESTS)
    add_test(NAME Tool.CommandCatalog COMMAND ${CMAKE_COMMAND}
        -DEXECUTABLE=$<TARGET_FILE:exyoki>
        -P ${PROJECT_SOURCE_DIR}/cmake/CheckCommandCatalog.cmake)
    set_tests_properties(Tool.CommandCatalog PROPERTIES LABELS "tools")

    # The completion scripts are generated from the live parser; a smoke test
    # per shell catches a generator that crashes or emits nothing.
    foreach(EXYOKI_COMPLETION_SHELL bash zsh powershell)
        add_test(NAME Tool.Completions.${EXYOKI_COMPLETION_SHELL}
            COMMAND exyoki completions ${EXYOKI_COMPLETION_SHELL})
        set_tests_properties(Tool.Completions.${EXYOKI_COMPLETION_SHELL} PROPERTIES
            LABELS "tools"
            PASS_REGULAR_EXPRESSION "exyoki")
    endforeach()
endif()
