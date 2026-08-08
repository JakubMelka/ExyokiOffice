# Copyright (c) 2026 Jakub Melka and Collaborators
# SPDX-License-Identifier: MIT
# See LICENSE file in the project root for full license text.

# PresentationML layer: everything reachable from PowerPointDocumentEditor. The
# layer answers to both "presentation" (its directory) and "powerpoint" (the tag
# its tests have always used and the name the matrix uses).
exyokioffice_add_test_executable(ExyokiOfficePresentationTests
    LAYER presentation
    PREFIX Presentation
    LABELS powerpoint
    SOURCES
        PowerPointLifecycleTests.cpp
        PowerPointSlideSizeTests.cpp
        PowerPointSlideCollectionTests.cpp
        PowerPointSlideBuilderTests.cpp
        PowerPointSlideCopyTests.cpp
        PowerPointSectionCustomShowTests.cpp
        PowerPointMasterLayoutTests.cpp
        PowerPointShapeTreeTests.cpp
        PowerPointShapeTransformTests.cpp
        PowerPointShapeStyleTests.cpp
        PowerPointGeometryTests.cpp
        PowerPointTextFrameTests.cpp
        PowerPointTableTests.cpp
        PowerPointPictureTests.cpp
        PowerPointMediaTests.cpp
        PowerPointEmbeddedObjectTests.cpp
        PowerPointChartTests.cpp
        PowerPointTransitionTests.cpp
        PowerPointAnimationTests.cpp
        PowerPointAnimationEffectTests.cpp
        PowerPointNotesCommentsTests.cpp
        PowerPointProtectionTests.cpp)

# Areas follow the rows of the PowerPoint table in docs/Compatibility.md. Their
# tags must stay disjoint: a case matched by two areas would run twice.
exyokioffice_add_test_area(presentation NAME Presentation.Presentations LABELS ppt-presentations
    TAGS "[powerpoint-lifecycle]" "[powerpoint-slide-size]" "[handouts]")
exyokioffice_add_test_area(presentation NAME Presentation.Slides LABELS ppt-slides
    TAGS "[slides]" "[slide-builder]" "[slide-copy]")
exyokioffice_add_test_area(presentation NAME Presentation.Sections LABELS ppt-sections
    TAGS "[section-custom-show]")
exyokioffice_add_test_area(presentation NAME Presentation.Masters LABELS ppt-masters
    TAGS "[masters-layouts]")
exyokioffice_add_test_area(presentation NAME Presentation.Shapes LABELS ppt-shapes
    TAGS "[shapes]" "[shape-style]" "[transform]" "[geometry]")
exyokioffice_add_test_area(presentation NAME Presentation.Text LABELS ppt-text
    TAGS "[text-frame]")
exyokioffice_add_test_area(presentation NAME Presentation.Media LABELS ppt-media
    TAGS "[picture]" "[media]")
exyokioffice_add_test_area(presentation NAME Presentation.Tables LABELS ppt-tables
    TAGS "[table]")
exyokioffice_add_test_area(presentation NAME Presentation.Charts LABELS ppt-charts
    TAGS "[chart]")
exyokioffice_add_test_area(presentation NAME Presentation.Transitions LABELS ppt-transitions
    TAGS "[transition]")
exyokioffice_add_test_area(presentation NAME Presentation.Animations LABELS ppt-animations
    TAGS "[animation]")
exyokioffice_add_test_area(presentation NAME Presentation.Notes LABELS ppt-notes
    TAGS "[notes]" "[comments]")
# OLE objects have no typed API; the tests prove they survive a round trip.
exyokioffice_add_test_area(presentation NAME Presentation.Preserved LABELS ppt-preserved
    TAGS "[embedded-object]")
exyokioffice_add_test_area(presentation NAME Presentation.Protection LABELS protection
    TAGS "[protection]")

exyokioffice_finalize_test_layer(presentation)
