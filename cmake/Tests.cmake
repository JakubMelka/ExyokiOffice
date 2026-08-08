# Copyright (c) 2026 Jakub Melka and Collaborators
# SPDX-License-Identifier: MIT
# See LICENSE file in the project root for full license text.

include_guard(GLOBAL)

# Test layers and CTest labels.
#
# The suite is split into layers (tests/unit, tests/word, tests/spreadsheet, …),
# each of which is one doctest executable. Inside a layer, tests are grouped
# into areas by the bracket tags in their TEST_CASE names, and every area is
# registered as its own CTest entry that runs the layer's executable with a
# doctest filter. The CTest LABELS of that entry are the layer's labels plus the
# area label, which is what `ctest -L word` and `ctest -L word-tables` select and
# what the rows of docs/Compatibility.md point at.
#
# The areas of one layer must use disjoint tags: each area entry runs the cases
# matching its tags, and the layer's trailing ".Other" entry runs everything the
# areas did not claim. Together they cover every test case exactly once, so a
# plain `ctest` still runs the whole suite with no test executed twice.
#
# An area whose cases sweep a set of fixtures can be registered as several
# entries instead of one - see exyokioffice_add_sharded_test_area. The entries
# run the same cases and are told which fixture to take through an environment
# variable, which is what lets `ctest --parallel` spread one long sweep across
# cores. The test presets ask for a parallel run; WinBuild.ps1 raises the level
# to the machine's core count.

# Declares one test layer executable.
#
# exyokioffice_add_test_executable(<target>
#     LAYER   <layer-label>       # unit | package | word | …
#     PREFIX  <CTest name prefix> # Word  → the layer's residual entry is Word.Other
#     SOURCES <files…>
#     [LABELS <extra layer labels…>]   # spreadsheet also answers to excel
#     [LINK   <extra link targets…>])
#
# TestMain.cpp is added to every executable rather than to the support library,
# because a main() that lives in a static archive is only pulled in as a side
# effect of the CRT referencing it.
function(exyokioffice_add_test_executable target)
    cmake_parse_arguments(arg "" "LAYER;PREFIX" "SOURCES;LABELS;LINK" ${ARGN})

    if(NOT arg_LAYER OR NOT arg_PREFIX OR NOT arg_SOURCES)
        message(FATAL_ERROR "exyokioffice_add_test_executable(${target}) needs LAYER, PREFIX and SOURCES")
    endif()

    add_executable(${target}
        ${PROJECT_SOURCE_DIR}/tests/support/TestMain.cpp
        ${arg_SOURCES})

    target_link_libraries(${target} PRIVATE
        ExyokiOffice ExyokiOfficeTestSupport ${arg_LINK})

    # GeneratorSchematronTests reads the metadata under data/ back from the
    # source tree; the definition is kept on every layer so a test can move
    # between layers without a CMake change.
    target_compile_definitions(${target} PRIVATE
        EXYOKIOFFICE_SOURCE_DIR="${PROJECT_SOURCE_DIR}")

    target_include_directories(${target} PRIVATE
        ${PROJECT_SOURCE_DIR}/3rdparty
        ${PROJECT_SOURCE_DIR}/3rdparty/doctest
        ${PROJECT_SOURCE_DIR}/sources
        ${PROJECT_SOURCE_DIR}/sources/zip)

    # The working directory of a CTest entry is not on the DLL search path.
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:ExyokiOffice>
                $<TARGET_FILE_DIR:${target}>)

    set_property(GLOBAL PROPERTY EXYOKIOFFICE_TEST_LAYER_${arg_LAYER}_TARGET ${target})
    set_property(GLOBAL PROPERTY EXYOKIOFFICE_TEST_LAYER_${arg_LAYER}_PREFIX ${arg_PREFIX})
    set_property(GLOBAL PROPERTY EXYOKIOFFICE_TEST_LAYER_${arg_LAYER}_LABELS "${arg_LAYER};${arg_LABELS}")
    set_property(GLOBAL PROPERTY EXYOKIOFFICE_TEST_LAYER_${arg_LAYER}_TAGS "")
    set_property(GLOBAL APPEND PROPERTY EXYOKIOFFICE_TEST_LAYERS ${arg_LAYER})
endfunction()

# Turns a list of doctest tags into a doctest filter: "[a]" "[b]" → "*[a]*,*[b]*".
function(exyokioffice_test_filter outputVariable)
    set(filter "")
    foreach(tag IN LISTS ARGN)
        if(filter)
            string(APPEND filter ",")
        endif()
        string(APPEND filter "*${tag}*")
    endforeach()
    set(${outputVariable} "${filter}" PARENT_SCOPE)
endfunction()

# Registers one area of a layer as a CTest entry.
#
# exyokioffice_add_test_area(<layer>
#     NAME   Word.Tables      # CTest entry name
#     LABELS word-tables      # the label(s) docs/Compatibility.md refers to
#     TAGS   "[word-table-model]" […])
#
# LABELS takes more than one value where a single group of tests answers two
# rows of the matrix — the Word protection tests, for instance, are both the
# "Document protection" row and part of the cross-cutting "Protection" row.
#
# doctest exits successfully when a filter matches nothing, so a typo in TAGS
# would silently produce an empty label. FAIL_REGULAR_EXPRESSION catches the
# "test cases: 0" summary line and turns that into a failure instead. The
# pattern deliberately ends at the space after the zero rather than matching the
# following "|": CMake's regex engine has no escape for it, and "\\|" degenerates
# into an empty alternative that matches every line.
function(exyokioffice_add_test_area layer)
    cmake_parse_arguments(arg "" "NAME" "LABELS;TAGS" ${ARGN})

    if(NOT arg_NAME OR NOT arg_LABELS OR NOT arg_TAGS)
        message(FATAL_ERROR "exyokioffice_add_test_area(${layer}) needs NAME, LABELS and TAGS")
    endif()

    get_property(target GLOBAL PROPERTY EXYOKIOFFICE_TEST_LAYER_${layer}_TARGET)
    get_property(layerLabels GLOBAL PROPERTY EXYOKIOFFICE_TEST_LAYER_${layer}_LABELS)
    if(NOT target)
        message(FATAL_ERROR "exyokioffice_add_test_area: unknown test layer '${layer}'")
    endif()

    exyokioffice_test_filter(filter ${arg_TAGS})
    add_test(NAME ${arg_NAME} COMMAND ${target} --test-case=${filter})
    set_tests_properties(${arg_NAME} PROPERTIES
        LABELS "${layerLabels};${arg_LABELS}"
        FAIL_REGULAR_EXPRESSION "test cases: +0 ")

    set_property(GLOBAL APPEND PROPERTY EXYOKIOFFICE_TEST_LAYER_${layer}_TAGS ${arg_TAGS})
    # Kept separately from the tags: the partition check needs the per-area
    # filters, and joining them with "|" survives being passed through -D.
    set_property(GLOBAL APPEND_STRING PROPERTY
        EXYOKIOFFICE_TEST_LAYER_${layer}_FILTERS "${filter}|")
endfunction()

# Registers one area of a layer as several CTest entries, one per shard.
#
# exyokioffice_add_sharded_test_area(<layer>
#     NAME         Corpus.RoundTrip     # entries are <NAME>.<shard name>
#     LABELS       corpus-roundtrip slow
#     TAGS         "[corpus-roundtrip]"
#     ENVIRONMENT  EXYOKIOFFICE_CORPUS_DOCUMENT  # variable naming the shard
#     SHARD_NAMES  Word.Open_Source_Software …
#     SHARD_VALUES word/Open_Source_Software.docx …)
#
# Every entry runs the same test cases with the same doctest filter; what
# differs is the value of ENVIRONMENT, which the cases read to decide which
# fixture they sweep. An area is worth sharding when its cases loop over a set
# of files and the loop is the cost - the corpus sweeps take tens of seconds
# each as one entry, and one entry per document turns that into a run bounded by
# the slowest single file rather than by their sum.
#
# TAGS and the area's doctest filter are recorded once, not once per shard: the
# partition check in CheckTestPartition.cmake counts the cases each area filter
# selects, and recording the same filter N times would inflate the sum by N and
# fail a layer that is in fact partitioned. Nothing checks that the shard values
# cover the fixtures exactly once, because nothing has to - they are generated
# from the same manifest the cases read, and a value that manifest does not list
# makes the entry fail rather than pass having swept nothing.
function(exyokioffice_add_sharded_test_area layer)
    cmake_parse_arguments(arg "" "NAME;ENVIRONMENT" "LABELS;TAGS;SHARD_NAMES;SHARD_VALUES" ${ARGN})

    if(NOT arg_NAME OR NOT arg_LABELS OR NOT arg_TAGS OR NOT arg_ENVIRONMENT)
        message(FATAL_ERROR
            "exyokioffice_add_sharded_test_area(${layer}) needs NAME, LABELS, TAGS and ENVIRONMENT")
    endif()

    get_property(target GLOBAL PROPERTY EXYOKIOFFICE_TEST_LAYER_${layer}_TARGET)
    get_property(layerLabels GLOBAL PROPERTY EXYOKIOFFICE_TEST_LAYER_${layer}_LABELS)
    if(NOT target)
        message(FATAL_ERROR "exyokioffice_add_sharded_test_area: unknown test layer '${layer}'")
    endif()

    list(LENGTH arg_SHARD_NAMES shardCount)
    list(LENGTH arg_SHARD_VALUES shardValueCount)
    if(NOT shardCount EQUAL shardValueCount)
        message(FATAL_ERROR
            "exyokioffice_add_sharded_test_area(${arg_NAME}): ${shardCount} shard name(s) "
            "but ${shardValueCount} shard value(s)")
    endif()
    if(shardCount EQUAL 0)
        message(FATAL_ERROR "exyokioffice_add_sharded_test_area(${arg_NAME}) was given no shards")
    endif()

    exyokioffice_test_filter(filter ${arg_TAGS})

    math(EXPR lastShard "${shardCount} - 1")
    foreach(index RANGE ${lastShard})
        list(GET arg_SHARD_NAMES ${index} shardName)
        list(GET arg_SHARD_VALUES ${index} shardValue)

        add_test(NAME ${arg_NAME}.${shardName} COMMAND ${target} --test-case=${filter})
        set_tests_properties(${arg_NAME}.${shardName} PROPERTIES
            LABELS "${layerLabels};${arg_LABELS}"
            ENVIRONMENT "${arg_ENVIRONMENT}=${shardValue}"
            FAIL_REGULAR_EXPRESSION "test cases: +0 ")
    endforeach()

    set_property(GLOBAL APPEND PROPERTY EXYOKIOFFICE_TEST_LAYER_${layer}_TAGS ${arg_TAGS})
    set_property(GLOBAL APPEND_STRING PROPERTY
        EXYOKIOFFICE_TEST_LAYER_${layer}_FILTERS "${filter}|")
endfunction()

# Registers the layer's residual entry, which runs every test case that no area
# of that layer claimed. It carries only the layer labels, and unlike an area it
# is allowed to end up empty — that just means every case has an area.
#
# Also registers <Prefix>.Partition, which proves the areas and the residual
# entry together cover the layer exactly once. See cmake/CheckTestPartition.cmake
# for why that needs checking rather than assuming.
function(exyokioffice_finalize_test_layer layer)
    get_property(target GLOBAL PROPERTY EXYOKIOFFICE_TEST_LAYER_${layer}_TARGET)
    get_property(prefix GLOBAL PROPERTY EXYOKIOFFICE_TEST_LAYER_${layer}_PREFIX)
    get_property(layerLabels GLOBAL PROPERTY EXYOKIOFFICE_TEST_LAYER_${layer}_LABELS)
    get_property(tags GLOBAL PROPERTY EXYOKIOFFICE_TEST_LAYER_${layer}_TAGS)
    get_property(areaFilters GLOBAL PROPERTY EXYOKIOFFICE_TEST_LAYER_${layer}_FILTERS)
    if(NOT target)
        message(FATAL_ERROR "exyokioffice_finalize_test_layer: unknown test layer '${layer}'")
    endif()

    set(excludeFilter "")
    if(tags)
        exyokioffice_test_filter(excludeFilter ${tags})
        add_test(NAME ${prefix}.Other COMMAND ${target} --test-case-exclude=${excludeFilter})
    else()
        add_test(NAME ${prefix}.Other COMMAND ${target})
    endif()

    set_tests_properties(${prefix}.Other PROPERTIES LABELS "${layerLabels}")

    add_test(NAME ${prefix}.Partition COMMAND ${CMAKE_COMMAND}
        -DEXECUTABLE=$<TARGET_FILE:${target}>
        -DAREA_FILTERS=${areaFilters}
        -DEXCLUDE_FILTER=${excludeFilter}
        -P ${PROJECT_SOURCE_DIR}/cmake/CheckTestPartition.cmake)
    set_tests_properties(${prefix}.Partition PROPERTIES LABELS "${layerLabels};partition")
endfunction()
