# Copyright (c) 2026 Jakub Melka and Collaborators
# SPDX-License-Identifier: MIT
# See LICENSE file in the project root for full license text.

include_guard(GLOBAL)

# Places a fresh copy of the ExyokiOffice runtime library next to @p target.
#
# Every executable that loads the shared library from its own directory - the
# test layers, the tools, the examples - used to do this with a POST_BUILD
# copy. That step only runs when the copying target itself relinks, and under
# Ninja a library change that leaves the export surface alone does not relink
# dependents: the import library is restat'ed as unchanged. The copies then
# silently keep the previous library, and an incremental test run exercises
# code that is no longer in the tree. (MSBuild happened to mask this by
# relinking on the import library's timestamp.)
#
# The copy is therefore modeled as what it is: the copied DLL, a file that
# depends on the library file. Declaring the destination itself as the OUTPUT
# means a deleted copy is restored by the next build; the trailing touch
# keeps the destination's timestamp fresh when copy_if_different left an
# identical file in place, so the rule does not rerun forever.
#
# One rule serves one output directory. Several executables often share a
# directory (the three MCP servers, for one), and per-target rules would be
# unordered writes of the same destination file - an intermittent sharing
# violation under a parallel build. Each consuming target instead depends on
# its directory's single copy target, so a target-scoped build places the
# DLL exactly as the old POST_BUILD step did, and the ALL membership keeps a
# plain build refreshing every copy whether or not the executables rebuilt.
#
# A no-op in a static build, where there is no runtime file to place, and
# outside Windows, where the loader finds the library through the build
# rpath and no copy needs to exist.
function(exyokioffice_copy_runtime_library target)
    if(NOT WIN32 OR NOT EXYOKIOFFICE_LIBRARY_KIND STREQUAL "SHARED")
        return()
    endif()

    # The executables this helper serves all land in the calling directory's
    # default runtime location: CMAKE_CURRENT_BINARY_DIR, plus the per-config
    # subdirectory of a multi-config generator.
    get_property(isMultiConfig GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
    if(isMultiConfig)
        set(destinationDirectory ${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>)
    else()
        set(destinationDirectory ${CMAKE_CURRENT_BINARY_DIR})
    endif()

    string(MD5 directoryKey "${CMAKE_CURRENT_BINARY_DIR}")
    string(SUBSTRING "${directoryKey}" 0 12 directoryKey)
    set(copyTarget exyokioffice_runtime_copy_${directoryKey})

    if(NOT TARGET ${copyTarget})
        # A literal file name: OUTPUT permits no target-dependent generator
        # expression, and the library's runtime name is fixed (WinCoverage.ps1
        # relies on it as well).
        set(destination ${destinationDirectory}/ExyokiOffice.dll)
        add_custom_command(
            OUTPUT ${destination}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${destinationDirectory}
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    $<TARGET_FILE:ExyokiOffice> ${destinationDirectory}
            COMMAND ${CMAKE_COMMAND} -E touch ${destination}
            DEPENDS ExyokiOffice $<TARGET_FILE:ExyokiOffice>
            COMMENT "Refreshing the ExyokiOffice runtime in ${destinationDirectory}"
            VERBATIM)
        add_custom_target(${copyTarget} ALL DEPENDS ${destination})
    endif()

    add_dependencies(${target} ${copyTarget})
endfunction()
