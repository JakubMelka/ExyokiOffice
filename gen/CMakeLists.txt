# Copyright (c) 2026 Jakub Melka and Collaborators
# SPDX-License-Identifier: MIT
# See LICENSE file in the project root for full license text.

cmake_minimum_required(VERSION 3.25)

project(ExyokiOfficeGenerator LANGUAGES CXX)

add_library(generator_support STATIC
    src/Json.cpp
    src/FileSystem.cpp
    src/Logger.cpp
    src/Schematron.cpp
)

target_include_directories(generator_support
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_include_directories(generator_support SYSTEM PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty
)

set_target_properties(generator_support PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

add_executable(openxml_generator
    src/main.cpp
    src/Generator.cpp
)

target_link_libraries(openxml_generator PRIVATE generator_support)

# The generator is a standalone program and does not link the library. The one
# thing it borrows from it is sources/AsciiText.hpp, a header with no
# dependencies of its own: case folding decided by the machine's locale has no
# place in a code generator either, and a second copy of the same twenty lines
# would be exactly the duplication that header was introduced to remove.
target_include_directories(openxml_generator PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../sources)

set_target_properties(openxml_generator PROPERTIES
    OUTPUT_NAME "OpenXmlGenerator"
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)
