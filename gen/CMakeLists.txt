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

set_target_properties(openxml_generator PROPERTIES
    OUTPUT_NAME "OpenXmlGenerator"
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)
