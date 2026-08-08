# Copyright (c) 2026 Jakub Melka and Collaborators
# SPDX-License-Identifier: MIT
# See LICENSE file in the project root for full license text.

# Everything the test layers share.
#
# On Windows a DLL exports only what the export header marks, and zip and
# pugixml are private to the library, so the test binaries have to carry their
# own copy of those two translation units. They are compiled here rather than
# into each executable because there are nine of them. Strict warnings stay off
# — two of the three translation units are vendored third-party code.
#
# An ELF shared library exports every symbol instead, so elsewhere the test
# binaries resolve zip and pugixml against the library's own copy. Compiling a
# second copy in would put two definitions of the same entities in one process,
# each with its own static state; AddressSanitizer reports that as an
# odr-violation and aborts every test binary that pulls the objects in.
set(supportSources TestSupport.cpp)
if(WIN32)
    list(APPEND supportSources
        ${PROJECT_SOURCE_DIR}/sources/zip/zip.cpp
        ${PROJECT_SOURCE_DIR}/sources/pugixml/pugixml.cpp)
endif()

add_library(ExyokiOfficeTestSupport STATIC ${supportSources})

target_link_libraries(ExyokiOfficeTestSupport PUBLIC ExyokiOffice)

# PUBLIC: the test layers include "TestSupport.hpp" directly.
target_include_directories(ExyokiOfficeTestSupport PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(ExyokiOfficeTestSupport PRIVATE
    ${PROJECT_SOURCE_DIR}/sources
    ${PROJECT_SOURCE_DIR}/sources/zip)
