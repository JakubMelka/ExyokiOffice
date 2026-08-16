// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "doctest.h"

#include "ExyokiOffice/Word/WordDocument.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <set>
#include <string>

namespace
{
using ExyokiOffice::MeasurementUnit;
using ExyokiOffice::MeasuringUnits;
using ExyokiOffice::Word::CommentAuthor;
using ExyokiOffice::Word::DocumentMergeOptions;
using ExyokiOffice::Word::NumberingDefinition;
using ExyokiOffice::Word::NumberingLevelDefinition;
using ExyokiOffice::Word::ParagraphNumbering;
using ExyokiOffice::Word::StyleCopyConflictPolicy;
using ExyokiOffice::Word::StyleDefinition;
using ExyokiOffice::Word::StyleType;
using ExyokiOffice::Word::WordDocumentEditor;

NumberingDefinition MakeSingleLevelList(std::string name)
{
    NumberingDefinition definition;
    definition.Name = std::move(name);
    NumberingLevelDefinition level;
    level.Level = 0;
    level.LevelText = "%1.";
    definition.Levels.push_back(level);
    return definition;
}

// Every source document used by these tests carries at least one instance of
// every dependency kind WRD-015 has to remap: a paragraph style, a character
// style, a numbering list, a bookmark, an internal hyperlink pointing at that
// bookmark, an external hyperlink, a footnote, an endnote, a comment, a
// content control, and an image.
WordDocumentEditor::Ptr BuildRichSourceDocument()
{
    auto source = WordDocumentEditor::CreateNew();
    if (!source)
    {
        return nullptr;
    }

    source->Styles().CreateStyle(
        StyleDefinition{"SharedStyle", "Source Shared Style", StyleType::Paragraph});
    source->Styles().CreateStyle(
        StyleDefinition{"SourceEmphasis", "Source Emphasis", StyleType::Character});

    auto list = source->Numbering().EnsureMultilevelList(MakeSingleLevelList("SourceList"));

    auto first = source->AddParagraph("First source paragraph");
    first->SetStyleId("SharedStyle");
    auto emphasizedRun = first->AddRun();
    emphasizedRun->AddText(" emphasized");
    emphasizedRun->SetStyleId("SourceEmphasis");
    first->AddBookmark("Anchor1");

    auto second = source->AddParagraph("Second source paragraph");
    second->SetListStyle(list);
    second->AddFootnote("Source footnote text.");
    second->AddEndnote("Source endnote text.");
    second->AddCommentOnParagraph("Source comment text.", CommentAuthor{"Alice", "AL"});
    second->AddHyperlink("Docs", "https://example.com/docs");
    second->AddInternalHyperlink("Back to top", "Anchor1");

    auto control = source->Body().InsertContentControl("sourceTag", "Source Alias");
    REQUIRE(control != nullptr);
    control->SetText("Control text");

    source->AddImageFromData(std::vector<ExyokiOffice::Byte>{1, 2, 3, 4}, "image/png",
                             MeasuringUnits(1.0, MeasurementUnit::Centimeter),
                             MeasuringUnits(1.0, MeasurementUnit::Centimeter));

    return source;
}

// Every target document already defines a *different* style under the same
// ID as the source's "SharedStyle" and a bookmark under the same name as the
// source's "Anchor1", so merges exercise the rename-on-collision paths.
WordDocumentEditor::Ptr BuildTargetDocumentWithCollisions()
{
    auto target = WordDocumentEditor::CreateNew();
    if (!target)
    {
        return nullptr;
    }

    target->Styles().CreateStyle(
        StyleDefinition{"SharedStyle", "Target Shared Style", StyleType::Paragraph});
    auto existing = target->AddParagraph("Existing target paragraph");
    existing->SetStyleId("SharedStyle");
    existing->AddBookmark("Anchor1");

    return target;
}

std::set<int> BookmarkIds(const WordDocumentEditor::Ptr& editor)
{
    std::set<int> ids;
    for (const auto& bookmark : editor->Bookmarks())
    {
        ids.insert(bookmark->GetId());
    }
    return ids;
}

std::set<std::string> BookmarkNames(const WordDocumentEditor::Ptr& editor)
{
    std::set<std::string> names;
    for (const auto& bookmark : editor->Bookmarks())
    {
        names.insert(bookmark->GetName());
    }
    return names;
}
} // namespace

// ---------------------------------------------------------------------------
// Styles and numbering
// ---------------------------------------------------------------------------

TEST_SUITE("WordCopyMergeTests")
{

    TEST_CASE("BodyCursor::InsertDocument renames a colliding style and keeps the target's own style intact [unit] [word] [word-copy-merge]")
    {
        auto source = BuildRichSourceDocument();
        auto target = BuildTargetDocumentWithCollisions();
        REQUIRE(source != nullptr);
        REQUIRE(target != nullptr);

        REQUIRE(target->Body().InsertDocument(*source));

        auto paragraphs = target->Paragraphs();
        REQUIRE(paragraphs.size() >= 3);

        // The pre-existing target paragraph keeps its own "SharedStyle" definition.
        CHECK(target->Styles().GetStyle("SharedStyle")->Name == "Target Shared Style");

        // The merged paragraph's style was renamed away from the colliding ID and
        // still carries the source's own definition and text formatting intent.
        const auto& mergedFirst = paragraphs[1];
        CHECK(mergedFirst->PlainText() == "First source paragraph emphasized");
        const auto mergedStyleId = mergedFirst->GetStyleId();
        CHECK(mergedStyleId != "SharedStyle");
        REQUIRE(target->Styles().HasStyle(mergedStyleId));
        CHECK(target->Styles().GetStyle(mergedStyleId)->Name == "Source Shared Style");

        // The character style on the emphasized run was imported without collision
        // (there was no target style with that ID), so it keeps its original ID.
        auto runs = mergedFirst->Runs();
        REQUIRE(runs.size() >= 2);
        CHECK(runs[1]->GetStyleId() == "SourceEmphasis");
        REQUIRE(target->Styles().HasStyle("SourceEmphasis"));
    }

    TEST_CASE("BodyCursor::InsertDocument imports numbering under a collision-free instance ID [unit] [word] [word-copy-merge]")
    {
        auto source = BuildRichSourceDocument();
        auto target = WordDocumentEditor::CreateNew();
        REQUIRE(source != nullptr);
        REQUIRE(target != nullptr);

        // Give the target its own pre-existing list first, so the numbering ID the
        // source's list used ("1", since it is the first list in a fresh document)
        // is already taken in the target and a real collision must be avoided.
        auto targetList = target->Numbering().EnsureMultilevelList(MakeSingleLevelList("TargetList"));
        auto targetParagraph = target->AddParagraph("Existing target paragraph");
        targetParagraph->SetListStyle(targetList);

        REQUIRE(target->Body().InsertDocument(*source));

        auto paragraphs = target->Paragraphs();
        REQUIRE(paragraphs.size() >= 3);

        ParagraphNumbering mergedNumbering;
        REQUIRE(paragraphs[2]->TryGetNumbering(mergedNumbering));
        REQUIRE(mergedNumbering.NumberingId.has_value());

        // No duplicate numbering instance IDs anywhere in the merged target document.
        std::set<int> ids;
        for (const auto& instance : target->Numbering().Instances())
        {
            CHECK(ids.insert(instance.NumberingId).second);
        }
        CHECK(ids.count(*mergedNumbering.NumberingId) == 1);
    }

    // ---------------------------------------------------------------------------
    // Bookmarks and hyperlinks
    // ---------------------------------------------------------------------------

    TEST_CASE("BodyCursor::InsertDocument renames a colliding bookmark and rewrites its internal hyperlink [unit] [word] [word-copy-merge]")
    {
        auto source = BuildRichSourceDocument();
        auto target = BuildTargetDocumentWithCollisions();
        REQUIRE(source != nullptr);
        REQUIRE(target != nullptr);

        const auto targetBookmarksBefore = target->Bookmarks().size();
        const auto sourceBookmarkIds = BookmarkIds(source);
        const auto sourceBookmarkNames = BookmarkNames(source);

        REQUIRE(target->Body().InsertDocument(*source));

        auto allBookmarks = target->Bookmarks();
        CHECK(allBookmarks.size() == targetBookmarksBefore + sourceBookmarkIds.size());

        // No duplicate IDs or names anywhere in the merged target document.
        std::set<int> ids;
        std::set<std::string> names;
        for (const auto& bookmark : allBookmarks)
        {
            CHECK(ids.insert(bookmark->GetId()).second);
            CHECK(names.insert(bookmark->GetName()).second);
        }

        // The original target bookmark keeps its name...
        auto originalAnchor = target->FindBookmark("Anchor1");
        REQUIRE(originalAnchor != nullptr);

        // ...and the merged one was renamed to something else, still findable.
        std::string mergedAnchorName;
        for (const auto& name : names)
        {
            if (name != "Anchor1")
            {
                mergedAnchorName = name;
            }
        }
        REQUIRE(!mergedAnchorName.empty());
        auto mergedAnchor = target->FindBookmark(mergedAnchorName);
        REQUIRE(mergedAnchor != nullptr);
        CHECK(mergedAnchor->GetId() != originalAnchor->GetId());

        // The merged internal hyperlink now points at the renamed bookmark, not
        // at the original "Anchor1" (which belongs to the pre-existing content).
        bool foundRewrittenAnchor = false;
        for (const auto& paragraph : target->Paragraphs())
        {
            for (const auto& hyperlink : paragraph->Hyperlinks())
            {
                if (hyperlink->IsInternal() && hyperlink->GetAnchor() == mergedAnchorName)
                {
                    foundRewrittenAnchor = true;
                }
                CHECK(hyperlink->GetAnchor() != "Anchor1");
            }
        }
        CHECK(foundRewrittenAnchor);
    }

    TEST_CASE("BodyCursor::InsertDocument copies an external hyperlink's URL through a new relationship [unit] [word] [word-copy-merge]")
    {
        auto source = BuildRichSourceDocument();
        auto target = WordDocumentEditor::CreateNew();
        REQUIRE(source != nullptr);
        REQUIRE(target != nullptr);

        REQUIRE(target->Body().InsertDocument(*source));

        bool foundExternal = false;
        for (const auto& paragraph : target->Paragraphs())
        {
            for (const auto& hyperlink : paragraph->Hyperlinks())
            {
                if (hyperlink->IsExternal())
                {
                    CHECK(hyperlink->GetUrl() == "https://example.com/docs");
                    foundExternal = true;
                }
            }
        }
        CHECK(foundExternal);
    }

    // ---------------------------------------------------------------------------
    // Notes, comments, content controls
    // ---------------------------------------------------------------------------

    TEST_CASE("BodyCursor::InsertDocument merges footnotes, endnotes, and comments without colliding IDs [unit] [word] [word-copy-merge]")
    {
        auto source = BuildRichSourceDocument();
        auto target = WordDocumentEditor::CreateNew();
        REQUIRE(source != nullptr);
        REQUIRE(target != nullptr);

        // Give the target its own pre-existing footnote/endnote/comment so the
        // merge has to allocate IDs that do not collide with existing ones.
        auto existingParagraph = target->AddParagraph("Existing target paragraph");
        existingParagraph->AddFootnote("Existing footnote.");
        existingParagraph->AddEndnote("Existing endnote.");
        existingParagraph->AddCommentOnParagraph("Existing comment.");

        REQUIRE(target->Body().InsertDocument(*source));

        std::set<int> footnoteIds;
        for (const auto& note : target->Footnotes())
        {
            CHECK(footnoteIds.insert(note->GetId()).second);
        }
        std::set<int> endnoteIds;
        for (const auto& note : target->Endnotes())
        {
            CHECK(endnoteIds.insert(note->GetId()).second);
        }
        std::set<int> commentIds;
        for (const auto& comment : target->Comments())
        {
            CHECK(commentIds.insert(comment->GetId()).second);
        }

        bool foundFootnoteText = false;
        for (const auto& note : target->Footnotes())
        {
            if (note->PlainText() == "Source footnote text.")
            {
                foundFootnoteText = true;
            }
        }
        CHECK(foundFootnoteText);

        bool foundCommentText = false;
        for (const auto& comment : target->Comments())
        {
            if (comment->PlainText() == "Source comment text.")
            {
                CHECK(comment->GetAuthor() == "Alice");
                CHECK(comment->GetInitials() == "AL");
                foundCommentText = true;
            }
        }
        CHECK(foundCommentText);
    }

    TEST_CASE("BodyCursor::InsertDocument reassigns content control IDs [unit] [word] [word-copy-merge]")
    {
        auto source = BuildRichSourceDocument();
        auto target = WordDocumentEditor::CreateNew();
        REQUIRE(source != nullptr);
        REQUIRE(target != nullptr);

        auto existingControl = target->Body().InsertContentControl("existingTag", "Existing Alias");
        REQUIRE(existingControl != nullptr);

        REQUIRE(target->Body().InsertDocument(*source));

        std::set<int> ids;
        bool foundSourceControl = false;
        for (const auto& control : target->ContentControls())
        {
            CHECK(ids.insert(control->GetId()).second);
            if (control->GetTag() == "sourceTag")
            {
                CHECK(control->GetAlias() == "Source Alias");
                CHECK(control->PlainText() == "Control text");
                foundSourceControl = true;
            }
        }
        CHECK(foundSourceControl);
    }

    // ---------------------------------------------------------------------------
    // Images and tables
    // ---------------------------------------------------------------------------

    TEST_CASE("BodyCursor::InsertDocument copies image payloads into new target relationships [unit] [word] [word-copy-merge]")
    {
        auto source = BuildRichSourceDocument();
        auto target = WordDocumentEditor::CreateNew();
        REQUIRE(source != nullptr);
        REQUIRE(target != nullptr);

        REQUIRE(target->Body().InsertDocument(*source));

        bool foundImage = false;
        for (const auto& paragraph : target->Paragraphs())
        {
            for (const auto& run : paragraph->Runs())
            {
                for (const auto& image : run->Images())
                {
                    foundImage = true;
                    (void)image;
                }
            }
        }
        CHECK(foundImage);

        auto mainPart = target->GetDocument()->GetMainDocumentPart();
        REQUIRE(mainPart != nullptr);
        auto images = mainPart->GetImageParts();
        REQUIRE(images.size() == 1);
        CHECK(images.front()->ContentType() == "image/png");
        auto bytes = images.front()->GetBinaryData();
        REQUIRE(bytes.size() == 4);
        CHECK(bytes[0] == 1);
        CHECK(bytes[3] == 4);
    }

    TEST_CASE("BodyCursor::InsertDocument remaps paragraph styles inside table cells [unit] [word] [word-copy-merge]")
    {
        auto source = WordDocumentEditor::CreateNew();
        REQUIRE(source != nullptr);
        source->Styles().CreateStyle(StyleDefinition{"CellStyle", "Cell Style", StyleType::Paragraph});
        auto table = source->AddTable(1, 1);
        REQUIRE(table != nullptr);
        table->SetCellText(0, 0, "Cell text");
        REQUIRE(!table->Paragraphs().empty());
        table->Paragraphs().front()->SetStyleId("CellStyle");

        auto target = WordDocumentEditor::CreateNew();
        REQUIRE(target != nullptr);
        REQUIRE(target->Body().InsertDocument(*source));

        auto tables = target->Tables();
        REQUIRE(tables.size() == 1);
        REQUIRE(!tables.front()->Paragraphs().empty());
        auto cellParagraph = tables.front()->Paragraphs().front();
        CHECK(cellParagraph->PlainText() == "Cell text");
        CHECK(cellParagraph->GetStyleId() == "CellStyle");
        REQUIRE(target->Styles().HasStyle("CellStyle"));
    }

    // ---------------------------------------------------------------------------
    // Placement and round-trip
    // ---------------------------------------------------------------------------

    TEST_CASE("BodyCursor::InsertDocument preserves order and respects Before/After placement [unit] [word] [word-copy-merge]")
    {
        auto source = WordDocumentEditor::CreateNew();
        REQUIRE(source != nullptr);
        source->AddParagraph("Source A");
        source->AddParagraph("Source B");

        auto target = WordDocumentEditor::CreateNew();
        REQUIRE(target != nullptr);
        auto anchor = target->AddParagraph("Target anchor");

        REQUIRE(target->Before(anchor).InsertDocument(*source));

        auto paragraphs = target->Paragraphs();
        REQUIRE(paragraphs.size() == 3);
        CHECK(paragraphs[0]->PlainText() == "Source A");
        CHECK(paragraphs[1]->PlainText() == "Source B");
        CHECK(paragraphs[2]->PlainText() == "Target anchor");
    }

    TEST_CASE("BodyCursor::InsertDocument leaves the source document unmodified [unit] [word] [word-copy-merge]")
    {
        auto source = BuildRichSourceDocument();
        auto target = BuildTargetDocumentWithCollisions();
        REQUIRE(source != nullptr);
        REQUIRE(target != nullptr);

        REQUIRE(target->Body().InsertDocument(*source));

        CHECK(source->Styles().GetStyle("SharedStyle")->Name == "Source Shared Style");
        auto sourceAnchor = source->FindBookmark("Anchor1");
        REQUIRE(sourceAnchor != nullptr);
        // Two authored paragraphs, the one the image was placed in, and the one
        // inside the content control - Paragraphs() reaches into a block-level
        // structured document tag rather than stopping at it.
        CHECK(source->Paragraphs().size() == 4);
    }

    TEST_CASE("BodyCursor::InsertDocument result survives a save/reopen round-trip [unit] [word] [word-copy-merge]")
    {
        auto source = BuildRichSourceDocument();
        auto target = BuildTargetDocumentWithCollisions();
        REQUIRE(source != nullptr);
        REQUIRE(target != nullptr);

        REQUIRE(target->Body().InsertDocument(*source, DocumentMergeOptions{StyleCopyConflictPolicy::Rename}));

        auto reopened = WordDocumentEditor::Open(target->SaveToMemory());
        REQUIRE(reopened != nullptr);

        std::set<int> bookmarkIds;
        std::set<std::string> bookmarkNames;
        for (const auto& bookmark : reopened->Bookmarks())
        {
            CHECK(bookmarkIds.insert(bookmark->GetId()).second);
            CHECK(bookmarkNames.insert(bookmark->GetName()).second);
        }
        CHECK(bookmarkNames.size() == 2);

        bool foundFootnote = false;
        for (const auto& note : reopened->Footnotes())
        {
            if (note->PlainText() == "Source footnote text.")
            {
                foundFootnote = true;
            }
        }
        CHECK(foundFootnote);

        bool foundExternal = false;
        for (const auto& paragraph : reopened->Paragraphs())
        {
            for (const auto& hyperlink : paragraph->Hyperlinks())
            {
                if (hyperlink->IsExternal())
                {
                    CHECK(hyperlink->GetUrl() == "https://example.com/docs");
                    foundExternal = true;
                }
            }
        }
        CHECK(foundExternal);
    }

} // TEST_SUITE("WordCopyMergeTests")
