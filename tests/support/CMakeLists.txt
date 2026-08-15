# Copyright (c) 2026 Jakub Melka and Contributors
# SPDX-License-Identifier: MIT
# See LICENSE file in the project root for full license text.

# Everything the test layers share.
#
# The library exports only what the export header marks - a DLL always did, and
# an ELF shared object does too since the visibility default was flipped. zip
# and pugixml are private to it, so the test binaries carry their own copy of
# those two translation units. They are compiled here rather than into each
# executable because there are nine of them. Strict warnings stay off — two of
# the three translation units are vendored third-party code.
#
# The duplicate copy is only sound because the library's own is hidden: nothing
# interposes, so the two never resolve to each other. It is also what
# AddressSanitizer's odr-violation check keys on, which is why this used to be
# Windows-only, back when an ELF build exported everything it had.
set(supportSources
    TestSupport.cpp
    ImageFixtures.cpp
    ${PROJECT_SOURCE_DIR}/sources/zip/zip.cpp
    ${PROJECT_SOURCE_DIR}/sources/pugixml/pugixml.cpp)

add_library(ExyokiOfficeTestSupport STATIC ${supportSources})

target_link_libraries(ExyokiOfficeTestSupport PUBLIC ExyokiOffice)

# PUBLIC: the test layers include "TestSupport.hpp" directly.
target_include_directories(ExyokiOfficeTestSupport PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(ExyokiOfficeTestSupport PRIVATE
    ${PROJECT_SOURCE_DIR}/sources
    ${PROJECT_SOURCE_DIR}/sources/zip)
