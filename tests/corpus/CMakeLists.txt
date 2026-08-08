# Copyright (c) 2026 Jakub Melka and Collaborators
# SPDX-License-Identifier: MIT
# See LICENSE file in the project root for full license text.

# The corpus layer: real packages saved by Microsoft Office, read from
# corpus/ in the source tree and described by corpus/manifest.json.
#
# Every other layer builds its fixtures with the library's own API, so it can
# only lose what the library put there in the first place. This layer is the
# only one whose inputs the library never produced, which makes it the place
# where a validator rule that misreads its metadata, or a writer that drops a
# part it does not model, actually shows up.
#
# The layer reads the corpus through EXYOKIOFFICE_SOURCE_DIR (defined for every
# test target by cmake/Tests.cmake) rather than copying it next to the binary:
# the fixtures are megabytes, and a stale copy would test the wrong bytes.
exyokioffice_add_test_executable(ExyokiOfficeCorpusTests
    LAYER corpus
    PREFIX Corpus
    SOURCES
        CorpusManifest.hpp
        CorpusManifest.cpp
        CorpusManifestTests.cpp
        CorpusValidationTests.cpp
        CorpusRoundTripTests.cpp
        CorpusSignatureTests.cpp
        CorpusEditorTests.cpp)

target_include_directories(ExyokiOfficeCorpusTests PRIVATE
    ${PROJECT_SOURCE_DIR}/tests/support)

# One shard per corpus document, read from the manifest.
#
# The three areas below sweep every package end to end and are between them most
# of the suite's running time. As one entry each they are also its critical
# path: `ctest --parallel` can only run three things at once, and each of the
# three takes as long as all its documents together. Registering them per
# document instead turns that into a run bounded by the slowest single file.
#
# The manifest is the source of truth here rather than a glob over corpus/,
# because a glob at configure time does not notice a file added afterwards - and
# because a file with no manifest entry is already a failure of Corpus.Manifest,
# which is the check that keeps the two in step. Listing the manifest in
# CMAKE_CONFIGURE_DEPENDS is what makes describing a new fixture enough: the
# next build reconfigures and the entries appear.
set(corpusManifestFile ${PROJECT_SOURCE_DIR}/corpus/manifest.json)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${corpusManifestFile})

file(READ ${corpusManifestFile} corpusManifestJson)
string(JSON corpusDocumentCount ERROR_VARIABLE corpusManifestError
    LENGTH ${corpusManifestJson} Documents)
if(corpusManifestError OR corpusDocumentCount EQUAL 0)
    message(FATAL_ERROR "corpus/manifest.json lists no documents: ${corpusManifestError}")
endif()

set(corpusShardNames "")
set(corpusShardValues "")
math(EXPR corpusLastDocument "${corpusDocumentCount} - 1")
foreach(corpusIndex RANGE ${corpusLastDocument})
    string(JSON corpusFile GET ${corpusManifestJson} Documents ${corpusIndex} File)
    string(JSON corpusFamily GET ${corpusManifestJson} Documents ${corpusIndex} Family)

    # Spelled out rather than derived from the family string, so that the entry
    # names match the ones the compatibility layer already uses (Compat.Ppt.*).
    # A family nobody named here is a fixture nobody planned for: fail rather
    # than invent a prefix for it.
    if(corpusFamily STREQUAL "word")
        set(corpusFamilyPrefix Word)
    elseif(corpusFamily STREQUAL "excel")
        set(corpusFamilyPrefix Excel)
    elseif(corpusFamily STREQUAL "powerpoint")
        set(corpusFamilyPrefix Ppt)
    else()
        message(FATAL_ERROR
            "corpus/manifest.json: '${corpusFile}' has Family '${corpusFamily}', which has no "
            "CTest name prefix. Add one here.")
    endif()

    get_filename_component(corpusStem ${corpusFile} NAME_WLE)
    string(REGEX REPLACE "[^A-Za-z0-9_]" "_" corpusStem "${corpusStem}")

    # add_test() rejects a duplicate name, so two fixtures whose names collide
    # after this stop the configure instead of silently sharing an entry.
    list(APPEND corpusShardNames "${corpusFamilyPrefix}.${corpusStem}")
    list(APPEND corpusShardValues "${corpusFile}")
endforeach()

exyokioffice_add_test_area(corpus NAME Corpus.Manifest LABELS corpus-manifest
    TAGS "[corpus-manifest]")
# The sharded areas carry `slow`: between them they are most of the suite's
# running time, and `ctest -LE slow` is what keeps the edit-test loop in seconds.
exyokioffice_add_sharded_test_area(corpus NAME Corpus.Validation
    LABELS corpus-validation validation slow
    TAGS "[corpus-validation]"
    ENVIRONMENT EXYOKIOFFICE_CORPUS_DOCUMENT
    SHARD_NAMES ${corpusShardNames}
    SHARD_VALUES ${corpusShardValues})
exyokioffice_add_sharded_test_area(corpus NAME Corpus.ContentModel
    LABELS corpus-content-model validation slow
    TAGS "[corpus-content-model]"
    ENVIRONMENT EXYOKIOFFICE_CORPUS_DOCUMENT
    SHARD_NAMES ${corpusShardNames}
    SHARD_VALUES ${corpusShardValues})
exyokioffice_add_sharded_test_area(corpus NAME Corpus.RoundTrip
    LABELS corpus-roundtrip opc-roundtrip slow
    TAGS "[corpus-roundtrip]"
    ENVIRONMENT EXYOKIOFFICE_CORPUS_DOCUMENT
    SHARD_NAMES ${corpusShardNames}
    SHARD_VALUES ${corpusShardValues})
# Not sharded: this one names the three fixtures it needs instead of sweeping
# the corpus, so a per-document entry would run it once per document.
exyokioffice_add_test_area(corpus NAME Corpus.Validation.TargetVersion
    LABELS corpus-validation validation slow
    TAGS "[corpus-version-target]")
exyokioffice_add_test_area(corpus NAME Corpus.Editors LABELS corpus-editors
    TAGS "[corpus-editors]")
# Verification against a signature Word wrote: the only fixture that can show
# whether this library's canonical form matches a conforming implementation's.
exyokioffice_add_test_area(corpus NAME Corpus.Signatures LABELS corpus-signatures signatures security
    TAGS "[corpus-signatures]")

exyokioffice_finalize_test_layer(corpus)
