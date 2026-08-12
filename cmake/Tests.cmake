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
    exyokioffice_copy_runtime_library(${target})

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

# Builds the whole library and every registered test layer into one
# executable, ExyokiOfficeMonolithTests. Must be called from the root scope
# after every layer has been added.
#
# The target exists for coverage measurement. Coverage of a function belongs
# to the binary that compiled it, and the same inline function hashes
# differently across this project's modules, so a multi-binary report
# (llvm-cov with several -object arguments) drops a large share of records as
# mismatched. One binary has no seams: header code instantiated by tests is
# attributed exactly, and the whole suite produces a single raw profile. See
# docs/coverage.md for the workflow and WinCoverage.ps1 -Monolith for the
# driver.
#
# The library's translation units are compiled into the executable rather
# than linked as a library: a shared library would reintroduce the module
# seam, and whole-archiving the static one trips the linker over the .rc
# resource member. The version resource is left out for the same reason -
# a coverage harness needs no version stamp.
#
# No CTest entry is registered on purpose: the layers already cover the suite
# for a plain `ctest`, and a second, serial sweep of everything would double
# every run. The executable is meant to be run directly, once.
#
# The assembly mirrors targets by property (SOURCES, INCLUDE_DIRECTORIES,
# COMPILE_DEFINITIONS) rather than consuming a shared build definition, so
# target-level COMPILE_OPTIONS, per-source properties, and the SYSTEM marking
# of include directories deliberately do not transfer: the monolith compiles
# with the toolchain's default warnings, which is what lets the vendored
# sources build without the strict-warning exemptions the ordinary targets
# arrange. A property the build later starts to rely on for correctness has
# to be added to the mirror loop below.
function(exyokioffice_add_test_monolith)
    get_property(layers GLOBAL PROPERTY EXYOKIOFFICE_TEST_LAYERS)
    if(NOT layers)
        message(FATAL_ERROR "exyokioffice_add_test_monolith: no test layers are registered")
    endif()

    # A shared library cannot back the monolith: the import library's thunks
    # would collide with the same definitions compiled into the executable.
    if(EXYOKIOFFICE_LIBRARY_KIND STREQUAL "SHARED")
        message(FATAL_ERROR
            "EXYOKIOFFICE_TEST_MONOLITH needs a static library build; configure with "
            "BUILD_SHARED_LIBS=OFF (the windows-ninja-clang-coverage-monolith preset "
            "does).")
    endif()

    set(monolithSources)
    _exyokioffice_append_target_sources(ExyokiOffice TRUE monolithSources)

    set(layerTargets)
    set(candidateLibraries)
    foreach(layer IN LISTS layers)
        get_property(layerTarget GLOBAL PROPERTY EXYOKIOFFICE_TEST_LAYER_${layer}_TARGET)
        list(APPEND layerTargets ${layerTarget})
        _exyokioffice_append_target_sources(${layerTarget} FALSE monolithSources)
        get_target_property(layerLibraries ${layerTarget} LINK_LIBRARIES)
        if(layerLibraries)
            list(APPEND candidateLibraries ${layerLibraries})
        endif()
    endforeach()

    # The helper archives (test support, MCP core, fuzz targets) name
    # ExyokiOffice in their link interface. Linking them would drag the whole
    # static library onto the build and the link line - every translation
    # unit compiled a second time for an archive no member is ever pulled
    # from, since the executable already defines every symbol. Their sources
    # are folded into the executable instead, and only link entries free of
    # the library (system libraries, generator_support) stay on the line.
    set(foldedLibraries)
    set(keptLibraries)
    list(REMOVE_DUPLICATES candidateLibraries)
    while(candidateLibraries)
        list(POP_FRONT candidateLibraries candidate)
        if(candidate STREQUAL "ExyokiOffice"
           OR candidate IN_LIST foldedLibraries
           OR candidate IN_LIST keptLibraries)
            continue()
        endif()
        if(TARGET ${candidate})
            get_target_property(candidateType ${candidate} TYPE)
            get_target_property(candidateLinks ${candidate} LINK_LIBRARIES)
            if(candidateType STREQUAL "STATIC_LIBRARY" AND "ExyokiOffice" IN_LIST candidateLinks)
                list(APPEND foldedLibraries ${candidate})
                _exyokioffice_append_target_sources(${candidate} TRUE monolithSources)
                list(APPEND candidateLibraries ${candidateLinks})
                continue()
            endif()
        endif()
        list(APPEND keptLibraries ${candidate})
    endwhile()

    # TestMain.cpp arrives once per layer, and the folded archives share
    # vendored translation units with the library; each is compiled once.
    list(REMOVE_DUPLICATES monolithSources)

    add_executable(ExyokiOfficeMonolithTests ${monolithSources})
    target_link_libraries(ExyokiOfficeMonolithTests PRIVATE ${keptLibraries})

    # The library's dependency on the generator does not travel with its
    # source list, and the generated sources are compiled here directly.
    if(TARGET generate_openxml)
        add_dependencies(ExyokiOfficeMonolithTests generate_openxml)
    endif()

    # The union of the compile environment of the library, the folded helper
    # archives, and every layer - the MCP layer, for one, adds the toolset
    # headers on top of the shared set.
    foreach(mirrored ExyokiOffice ${foldedLibraries} ${layerTargets})
        target_include_directories(ExyokiOfficeMonolithTests PRIVATE
            $<TARGET_PROPERTY:${mirrored},INCLUDE_DIRECTORIES>)
        target_compile_definitions(ExyokiOfficeMonolithTests PRIVATE
            $<TARGET_PROPERTY:${mirrored},COMPILE_DEFINITIONS>)
    endforeach()

    # The library sources see an empty EXYOKIOFFICE_EXPORT: they are neither
    # exported from a DLL nor imported from one, whatever the library kind of
    # the surrounding build is.
    target_compile_definitions(ExyokiOfficeMonolithTests PRIVATE EXYOKIOFFICE_STATIC_DEFINE)

    # The report's denominator is the whole library. cmake/Coverage.cmake
    # keeps the ordinary coverage build shared for exactly this reason; the
    # static monolith states it as linker options, so a function no test
    # calls stays in the binary as an uncovered row instead of being dropped
    # from the population by reference elimination or identical folding.
    if(MSVC)
        target_link_options(ExyokiOfficeMonolithTests PRIVATE /OPT:NOREF /OPT:NOICF)
    endif()

    # System libraries the library itself links; the library and the folded
    # archives are already part of the executable.
    get_target_property(libraryLinkLibraries ExyokiOffice LINK_LIBRARIES)
    if(libraryLinkLibraries)
        foreach(entry IN LISTS libraryLinkLibraries)
            if(NOT entry STREQUAL "ExyokiOffice" AND NOT entry IN_LIST foldedLibraries)
                target_link_libraries(ExyokiOfficeMonolithTests PRIVATE ${entry})
            endif()
        endforeach()
    endif()
endfunction()

# Appends @p target's SOURCES to the list variable @p outputVariable, made
# absolute against the target's source directory - the monolith references
# them from another directory. Resource scripts are skipped when
# @p skipResources is true: the coverage harness carries no version stamp,
# and whole-archiving a resource member is what broke the archive route.
function(_exyokioffice_append_target_sources target skipResources outputVariable)
    get_target_property(sources ${target} SOURCES)
    get_target_property(sourceDirectory ${target} SOURCE_DIR)
    set(collected ${${outputVariable}})
    foreach(source IN LISTS sources)
        if(skipResources AND source MATCHES "\\.(rc|res)$")
            continue()
        endif()
        if(IS_ABSOLUTE "${source}")
            list(APPEND collected "${source}")
        else()
            list(APPEND collected "${sourceDirectory}/${source}")
        endif()
    endforeach()
    set(${outputVariable} ${collected} PARENT_SCOPE)
endfunction()
