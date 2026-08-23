# Copyright (c) 2026 Jakub Melka and Contributors
# SPDX-License-Identifier: MIT
# See LICENSE file in the project root for full license text.

cmake_minimum_required(VERSION 3.25)

project(ExyokiOfficeInstallSmoke LANGUAGES CXX)

find_package(ExyokiOffice 1.1 CONFIG REQUIRED)

add_executable(ExyokiOfficeInstallSmoke main.cpp)
# Deliberately no target_compile_features here: the C++20 the public headers
# need has to arrive from the imported target, and asking for it again would
# hide the case where it does not.
target_link_libraries(ExyokiOfficeInstallSmoke PRIVATE ExyokiOffice::ExyokiOffice)
