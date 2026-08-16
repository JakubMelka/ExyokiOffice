// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "doctest.h"

#include "ExyokiOffice/Word/WordDocument.hpp"

#include <string>

namespace
{
using ExyokiOffice::Color;
using ExyokiOffice::Word::Bookmark;
using ExyokiOffice::Word::ContentRange;
using ExyokiOffice::Word::Hyperlink;
using ExyokiOffice::Word::Paragraph;
using ExyokiOffice::Word::Run;
using ExyokiOffice::Word::WordDocumentEditor;

std::shared_ptr<Run> AddPlainRun(const std::shared_ptr<Paragraph>& paragraph, std::string_view text)
{
    auto run = paragraph->AddRun();
    if (run)
    {
        run->AddText(text);
    }
    return run;
}
} // namespace

TEST_SUITE("WordHyperlinkBookmarkRangeTests")
{

    TEST_CASE("Paragraph::AddHyperlink creates an external relationship and round-trips through save and open [unit] [word] [word-hyperlink-bookmark-range]")
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);

        auto paragraph = editor->AddParagraph();
        REQUIRE(paragraph != nullptr);

        auto link = paragraph->AddHyperlink("ExyokiOffice", "https://example.com/docs", "Documentation", true);
        REQUIRE(link != nullptr);

        CHECK(link->IsExternal());
        CHECK_FALSE(link->IsInternal());
        CHECK(link->GetUrl() == "https://example.com/docs");
        CHECK(link->GetTooltip() == "Documentation");
        CHECK(link->GetNewWindow());
        CHECK(link->PlainText() == "ExyokiOffice");

        auto reopened = WordDocumentEditor::Open(editor->SaveToMemory());
        REQUIRE(reopened != nullptr);
        auto paragraphs = reopened->Paragraphs();
        REQUIRE(paragraphs.size() == 1);

        auto links = paragraphs.front()->Hyperlinks();
        REQUIRE(links.size() == 1);
        CHECK(links.front()->IsExternal());
        CHECK(links.front()->GetUrl() == "https://example.com/docs");
        CHECK(links.front()->GetTooltip() == "Documentation");
        CHECK(links.front()->GetNewWindow());
        CHECK(links.front()->PlainText() == "ExyokiOffice");
    }

    TEST_CASE("Paragraph::AddInternalHyperlink targets a bookmark without requiring a main document part [unit] [word] [word-hyperlink-bookmark-range]")
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);

        auto target = editor->AddParagraph("Chapter one");
        REQUIRE(target != nullptr);
        auto bookmark = target->AddBookmark("Chapter1");
        REQUIRE(bookmark != nullptr);

        auto source = editor->AddParagraph();
        REQUIRE(source != nullptr);
        auto link = source->AddInternalHyperlink("Go to chapter one", "Chapter1");
        REQUIRE(link != nullptr);

        CHECK_FALSE(link->IsExternal());
        CHECK(link->IsInternal());
        CHECK(link->GetAnchor() == "Chapter1");
        CHECK(link->GetUrl().empty());
        CHECK(link->PlainText() == "Go to chapter one");

        auto reopened = WordDocumentEditor::Open(editor->SaveToMemory());
        REQUIRE(reopened != nullptr);
        auto paragraphs = reopened->Paragraphs();
        REQUIRE(paragraphs.size() == 2);

        auto links = paragraphs.back()->Hyperlinks();
        REQUIRE(links.size() == 1);
        CHECK(links.front()->IsInternal());
        CHECK(links.front()->GetAnchor() == "Chapter1");
    }

    TEST_CASE("Hyperlink::SetUrl and SetAnchor are mutually exclusive [unit] [word] [word-hyperlink-bookmark-range]")
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);
        auto paragraph = editor->AddParagraph();
        REQUIRE(paragraph != nullptr);

        auto link = paragraph->AddHyperlink("text", "https://example.com");
        REQUIRE(link != nullptr);
        REQUIRE(link->IsExternal());

        link->SetAnchor("SomeBookmark");
        CHECK_FALSE(link->IsExternal());
        CHECK(link->IsInternal());
        CHECK(link->GetUrl().empty());
        CHECK(link->GetAnchor() == "SomeBookmark");

        link->SetUrl("https://example.com/again");
        CHECK(link->IsExternal());
        CHECK_FALSE(link->IsInternal());
        CHECK(link->GetAnchor().empty());
        CHECK(link->GetUrl() == "https://example.com/again");
    }

    TEST_CASE("A link in a table cell is a link [unit] [word] [word-hyperlink-bookmark-range]")
    {
        // Table::Paragraphs() used to hand out paragraphs with no part at all,
        // so AddHyperlink returned a wrapper whose SetUrl had nowhere to record
        // the target: a <w:hyperlink> with no r:id, which Word shows as plain
        // text. The table now passes on the part it lives in.
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);

        auto table = editor->AddTable(1, 1);
        REQUIRE(table != nullptr);
        auto cellParagraphs = table->Paragraphs();
        REQUIRE(cellParagraphs.size() == 1);

        auto link = cellParagraphs.front()->AddHyperlink("text", "https://example.com");
        REQUIRE(link != nullptr);
        CHECK(link->IsExternal());
        CHECK(link->GetUrl() == "https://example.com");
    }

    TEST_CASE("A paragraph with no part refuses an external link [unit] [word] [word-hyperlink-bookmark-range]")
    {
        // A hyperlink is a relationship plus an element naming it. With no part
        // to hold the relationship, the element alone would be a dead link the
        // caller was never told about, so nothing is added.
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);
        auto attached = editor->AddParagraph("text");
        REQUIRE(attached != nullptr);

        Paragraph detached(attached->GetLowLevelApi());
        CHECK(detached.OwningPart() == nullptr);
        CHECK(detached.AddHyperlink("text", "https://example.com") == nullptr);

        // An internal link is an anchor, not a relationship, so it needs no part.
        CHECK(detached.AddInternalHyperlink("text", "Bookmark") != nullptr);

        auto mainPart = editor->GetDocument()->GetMainDocumentPart();
        REQUIRE(mainPart != nullptr);
        detached.AttachOwningPart(mainPart);
        auto link = detached.AddHyperlink("text", "https://example.com");
        REQUIRE(link != nullptr);
        CHECK(link->GetUrl() == "https://example.com");
    }

    TEST_CASE("Hyperlink::Remove detaches the relationship and the element [unit] [word] [word-hyperlink-bookmark-range]")
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);
        auto paragraph = editor->AddParagraph();
        REQUIRE(paragraph != nullptr);

        auto link = paragraph->AddHyperlink("text", "https://example.com");
        REQUIRE(link != nullptr);
        REQUIRE(paragraph->Hyperlinks().size() == 1);

        link->Remove();
        CHECK(paragraph->Hyperlinks().empty());
        CHECK(paragraph->PlainText().empty());
    }

    TEST_CASE("Paragraph::AddBookmark allocates unique IDs and round-trips through save and open [unit] [word] [word-hyperlink-bookmark-range]")
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);

        auto first = editor->AddParagraph("First");
        REQUIRE(first != nullptr);
        auto secondParagraph = editor->AddParagraph("Second");
        REQUIRE(secondParagraph != nullptr);

        auto bookmarkA = first->AddBookmark("BookmarkA");
        auto bookmarkB = secondParagraph->AddBookmark("BookmarkB");
        REQUIRE(bookmarkA != nullptr);
        REQUIRE(bookmarkB != nullptr);

        CHECK(bookmarkA->GetName() == "BookmarkA");
        CHECK(bookmarkB->GetName() == "BookmarkB");
        CHECK(bookmarkA->GetId() != bookmarkB->GetId());
        CHECK(bookmarkA->GetEndElement() != nullptr);
        CHECK(bookmarkB->GetEndElement() != nullptr);

        auto found = editor->FindBookmark("BookmarkB");
        REQUIRE(found != nullptr);
        CHECK(found->GetId() == bookmarkB->GetId());

        auto reopened = WordDocumentEditor::Open(editor->SaveToMemory());
        REQUIRE(reopened != nullptr);

        auto reopenedBookmarks = reopened->Bookmarks();
        REQUIRE(reopenedBookmarks.size() == 2);
        CHECK(reopened->FindBookmark("BookmarkA") != nullptr);
        CHECK(reopened->FindBookmark("BookmarkB") != nullptr);
        CHECK(reopened->FindBookmark("NoSuchBookmark") == nullptr);
    }

    TEST_CASE("Bookmark::Remove removes both markers without touching surrounding text [unit] [word] [word-hyperlink-bookmark-range]")
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);
        auto paragraph = editor->AddParagraph("Some text");
        REQUIRE(paragraph != nullptr);

        auto bookmark = paragraph->AddBookmark("Marker");
        REQUIRE(bookmark != nullptr);
        REQUIRE(editor->Bookmarks().size() == 1);

        bookmark->Remove();
        CHECK(editor->Bookmarks().empty());
        CHECK(paragraph->PlainText() == "Some text");
    }

    TEST_CASE("Paragraph::Find and GetText locate substrings across run boundaries [unit] [word] [word-hyperlink-bookmark-range]")
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);
        auto paragraph = editor->AddParagraph();
        REQUIRE(paragraph != nullptr);

        AddPlainRun(paragraph, "Hello ")->SetBold(true);
        AddPlainRun(paragraph, "cruel world")->SetItalic(true);

        auto range = paragraph->Find("cruel ");
        REQUIRE(range.has_value());
        CHECK(range->Start == 6);
        CHECK(range->End == 12);
        CHECK(paragraph->GetText(*range) == "cruel ");

        auto notFound = paragraph->Find("missing");
        CHECK_FALSE(notFound.has_value());

        auto secondSearch = paragraph->Find("o", 5);
        REQUIRE(secondSearch.has_value());
        CHECK(secondSearch->Start == 13); // "world" -> 'o' at offset 13 within "Hello cruel world"
    }

    TEST_CASE("Paragraph::FindAll finds non-overlapping matches in order [unit] [word] [word-hyperlink-bookmark-range]")
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);
        auto paragraph = editor->AddParagraph("aaaa");

        auto matches = paragraph->FindAll("aa");
        REQUIRE(matches.size() == 2);
        CHECK(matches[0].Start == 0);
        CHECK(matches[0].End == 2);
        CHECK(matches[1].Start == 2);
        CHECK(matches[1].End == 4);
    }

    TEST_CASE("Paragraph::ReplaceText spanning multiple runs preserves unaffected run formatting [unit] [word] [word-hyperlink-bookmark-range]")
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);
        auto paragraph = editor->AddParagraph();
        REQUIRE(paragraph != nullptr);

        AddPlainRun(paragraph, "Hello ")->SetBold(true);
        AddPlainRun(paragraph, "cruel world")->SetItalic(true);

        REQUIRE(paragraph->PlainText() == "Hello cruel world");

        auto range = paragraph->Find("cruel ");
        REQUIRE(range.has_value());
        CHECK(paragraph->ReplaceText(*range, ""));
        CHECK(paragraph->PlainText() == "Hello world");

        auto runs = paragraph->Runs();
        REQUIRE(runs.size() == 2);
        CHECK(runs[0]->PlainText() == "Hello ");
        CHECK(runs[0]->GetBold() == true);
        CHECK(runs[1]->PlainText() == "world");
        CHECK(runs[1]->GetItalic() == true);
    }

    TEST_CASE("Paragraph::ReplaceText inherits formatting from the run where the range starts [unit] [word] [word-hyperlink-bookmark-range]")
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);
        auto paragraph = editor->AddParagraph();
        REQUIRE(paragraph != nullptr);

        AddPlainRun(paragraph, "AAA")->SetColor(Color(1, 0, 0));
        AddPlainRun(paragraph, "BBB")->SetColor(Color(0, 1, 0));
        AddPlainRun(paragraph, "CCC")->SetColor(Color(0, 0, 1));

        REQUIRE(paragraph->PlainText() == "AAABBBCCC");

        // The range exactly covers the middle run.
        ContentRange range{3, 6};
        CHECK(paragraph->GetText(range) == "BBB");
        CHECK(paragraph->ReplaceText(range, "XYZ"));
        CHECK(paragraph->PlainText() == "AAAXYZCCC");

        auto runs = paragraph->Runs();
        REQUIRE(runs.size() == 3);
        CHECK(runs[0]->PlainText() == "AAA");
        CHECK(runs[0]->GetColor() == "010000");
        CHECK(runs[1]->PlainText() == "XYZ");
        CHECK(runs[1]->GetColor() == "000100"); // Inherited from the original middle run.
        CHECK(runs[2]->PlainText() == "CCC");
        CHECK(runs[2]->GetColor() == "000001");
    }

    TEST_CASE("Paragraph::ReplaceText removes runs fully covered by a range spanning more than two runs [unit] [word] [word-hyperlink-bookmark-range]")
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);
        auto paragraph = editor->AddParagraph();
        REQUIRE(paragraph != nullptr);

        AddPlainRun(paragraph, "AAA");
        AddPlainRun(paragraph, "BBB");
        AddPlainRun(paragraph, "CCC");
        AddPlainRun(paragraph, "DDD");

        REQUIRE(paragraph->PlainText() == "AAABBBCCCDDD");

        // Covers the second half of "AAA", all of "BBB"/"CCC", and the first half of "DDD".
        ContentRange range{1, 10};
        CHECK(paragraph->ReplaceText(range, "-"));
        CHECK(paragraph->PlainText() == "A-DD");

        auto runs = paragraph->Runs();
        REQUIRE(runs.size() == 2);
        CHECK(runs[0]->PlainText() == "A-");
        CHECK(runs[1]->PlainText() == "DD");
    }

    TEST_CASE("Paragraph::ReplaceText supports zero-length insertion points [unit] [word] [word-hyperlink-bookmark-range]")
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);
        auto paragraph = editor->AddParagraph("Hello world");
        REQUIRE(paragraph != nullptr);

        ContentRange insertion{5, 5};
        CHECK(paragraph->ReplaceText(insertion, ","));
        CHECK(paragraph->PlainText() == "Hello, world");
    }

    TEST_CASE("Paragraph::ReplaceAll replaces every occurrence and reports the count [unit] [word] [word-hyperlink-bookmark-range]")
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);
        auto paragraph = editor->AddParagraph("foo foo foo");
        REQUIRE(paragraph != nullptr);

        const auto count = paragraph->ReplaceAll("foo", "bar");
        CHECK(count == 3);
        CHECK(paragraph->PlainText() == "bar bar bar");
    }

    TEST_CASE("Paragraph::GetText and ReplaceText reject out-of-bounds ranges [unit] [word] [word-hyperlink-bookmark-range]")
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);
        auto paragraph = editor->AddParagraph("short");
        REQUIRE(paragraph != nullptr);

        ContentRange outOfBounds{0, 100};
        CHECK(paragraph->GetText(outOfBounds).empty());
        CHECK_FALSE(paragraph->ReplaceText(outOfBounds, "x"));
    }

    TEST_CASE("Paragraph::FindAllRegex finds non-overlapping regex matches across run boundaries [unit] [word] [word-hyperlink-bookmark-range]")
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);
        auto paragraph = editor->AddParagraph();
        REQUIRE(paragraph != nullptr);

        AddPlainRun(paragraph, "Order 12");
        AddPlainRun(paragraph, "34 and 56");
        REQUIRE(paragraph->PlainText() == "Order 1234 and 56");

        const ExyokiOffice::RegexPattern pattern{R"(\d+)"};
        auto matches = paragraph->FindAllRegex(pattern);
        REQUIRE(matches.size() == 2);
        CHECK(paragraph->GetText(matches[0]) == "1234");
        CHECK(paragraph->GetText(matches[1]) == "56");
    }

    TEST_CASE("Paragraph::ReplaceAllRegex substitutes capture groups while preserving unaffected run formatting [unit] [word] [word-hyperlink-bookmark-range]")
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);
        auto paragraph = editor->AddParagraph();
        REQUIRE(paragraph != nullptr);

        AddPlainRun(paragraph, "Hello ")->SetBold(true);
        AddPlainRun(paragraph, "John Doe")->SetItalic(true);
        REQUIRE(paragraph->PlainText() == "Hello John Doe");

        // Greedy \w+ \w+ matches the first two words ("Hello John"), leaving " Doe" untouched.
        const ExyokiOffice::RegexPattern pattern{R"((\w+) (\w+))"};
        const auto count = paragraph->ReplaceAllRegex(pattern, "$2, $1");
        CHECK(count == 1);
        CHECK(paragraph->PlainText() == "John, Hello Doe");

        auto runs = paragraph->Runs();
        REQUIRE(runs.size() == 2);
        CHECK(runs[0]->PlainText() == "John, Hello");
        CHECK(runs[0]->GetBold() == true); // Formatting is a run property, unaffected by the text rewrite.
        CHECK(runs[1]->PlainText() == " Doe");
        CHECK(runs[1]->GetItalic() == true);
    }

    TEST_CASE("Paragraph::ReplaceAllRegex makes forward progress on a zero-length match [unit] [word] [word-hyperlink-bookmark-range]")
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);
        auto paragraph = editor->AddParagraph("abc");
        REQUIRE(paragraph != nullptr);

        // "x*" matches an empty string at every position when there is no 'x'.
        const ExyokiOffice::RegexPattern pattern{"x*"};
        const auto count = paragraph->ReplaceAllRegex(pattern, "-");
        CHECK(count == 4); // one empty match before each character plus one at the end
        CHECK(paragraph->PlainText() == "-a-b-c-");
    }

} // TEST_SUITE("WordHyperlinkBookmarkRangeTests")
