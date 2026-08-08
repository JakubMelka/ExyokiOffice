# Copyright (c) 2026 Jakub Melka and Collaborators
# SPDX-License-Identifier: MIT
# See LICENSE file in the project root for full license text.

cmake_minimum_required(VERSION 3.25)

project(ExyokiOfficeInstallSmoke LANGUAGES CXX)

find_package(ExyokiOffice 1.0 CONFIG REQUIRED)

add_executable(ExyokiOfficeInstallSmoke main.cpp)
target_compile_features(ExyokiOfficeInstallSmoke PRIVATE cxx_std_20)
target_link_libraries(ExyokiOfficeInstallSmoke PRIVATE ExyokiOffice::ExyokiOffice)
