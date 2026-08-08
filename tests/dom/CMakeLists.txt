# Copyright (c) 2026 Jakub Melka and Collaborators
# SPDX-License-Identifier: MIT
# See LICENSE file in the project root for full license text.

# The generator-emitted per-namespace element suites, one translation unit per
# API namespace, plus the hand-written breadth sweep over the element factory.
# The generator always (re)writes this directory; CONFIGURE_DEPENDS re-globs it
# when the set of namespaces changes.
#
# This is by far the largest test layer (over four thousand cases in more than a
# hundred translation units), which is why it is a separate executable: a change
# to a hand-written test no longer relinks it.
file(GLOB EXYOKIOFFICE_DOM_TESTS CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

exyokioffice_add_test_executable(ExyokiOfficeDomTests
    LAYER dom
    PREFIX Dom
    SOURCES ${EXYOKIOFFICE_DOM_TESTS})

exyokioffice_add_test_area(dom
    NAME Dom.Corpus
    LABELS dom-corpus
    TAGS "[dom-corpus]")

exyokioffice_finalize_test_layer(dom)
