# Copyright (c) 2026 Jakub Melka and Collaborators
# SPDX-License-Identifier: MIT
# See LICENSE file in the project root for full license text.

# WordprocessingML layer: everything reachable from WordDocumentEditor.
exyokioffice_add_test_executable(ExyokiOfficeWordTests
    LAYER word
    PREFIX Word
    SOURCES
        WordLifecycleTests.cpp
        WordBodyCursorTests.cpp
        WordEditorAuthoringTests.cpp
        WordReadWrapperTests.cpp
        WordFormattingTests.cpp
        WordStyleManagerTests.cpp
        WordNumberingTests.cpp
        WordSectionApiTests.cpp
        WordHeaderFooterTests.cpp
        WordTableModelTests.cpp
        WordImageTests.cpp
        WordHyperlinkBookmarkRangeTests.cpp
        WordFieldTests.cpp
        WordChartTests.cpp
        WordNotesCommentsContentControlTests.cpp
        WordRevisionTests.cpp
        WordCopyMergeTests.cpp
        WordTemplateMergeTests.cpp
        WordProtectionTests.cpp)

# Areas follow the rows of the Word table in docs/Compatibility.md. Their tags
# must stay disjoint: a case matched by two areas would run twice.
exyokioffice_add_test_area(word NAME Word.Documents LABELS word-documents
    TAGS "[word-lifecycle]")
exyokioffice_add_test_area(word NAME Word.Text LABELS word-text
    TAGS "[word-body-cursor]" "[word-authoring]" "[word-formatting]" "[word-read-wrapper]")
exyokioffice_add_test_area(word NAME Word.Styles LABELS word-styles
    TAGS "[word-style-manager]" "[word-numbering]")
exyokioffice_add_test_area(word NAME Word.Tables LABELS word-tables
    TAGS "[word-table-model]")
exyokioffice_add_test_area(word NAME Word.Images LABELS word-images
    TAGS "[word-image]")
exyokioffice_add_test_area(word NAME Word.Hyperlinks LABELS word-hyperlinks
    TAGS "[word-hyperlink-bookmark-range]")
exyokioffice_add_test_area(word NAME Word.Fields LABELS word-fields
    TAGS "[word-field]")
exyokioffice_add_test_area(word NAME Word.Charts LABELS word-charts
    TAGS "[word-chart]")
exyokioffice_add_test_area(word NAME Word.Sections LABELS word-sections
    TAGS "[word-section-api]" "[word-header-footer]")
# One file covers footnotes, endnotes, comments and content controls, so the two
# matrix rows share it rather than splitting a tag that was never split.
exyokioffice_add_test_area(word NAME Word.NotesAndControls
    LABELS word-notes word-content-controls
    TAGS "[word-notes-comments-content-control]")
exyokioffice_add_test_area(word NAME Word.Revisions LABELS word-revisions
    TAGS "[word-revision]")
exyokioffice_add_test_area(word NAME Word.Merge LABELS word-merge
    TAGS "[word-copy-merge]" "[word-template-merge]")
exyokioffice_add_test_area(word NAME Word.Protection LABELS word-protection protection
    TAGS "[protection]")

exyokioffice_finalize_test_layer(word)
