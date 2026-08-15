// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include <doctest.h>

#include "ExyokiOffice/Excel/ExcelDocument.hpp"

using namespace ExyokiOffice::Excel;

TEST_CASE("Worksheet hyperlinks support external and internal targets [unit] [excel] [layout]")
{
    auto editor = ExcelDocumentEditor::CreateNew();
    auto sheet = editor->FirstWorksheet();
    const auto a1 = *CellAddress::ParseA1("A1");
    const auto b2 = *CellAddress::ParseA1("B2");

    CHECK(sheet->SetHyperlink({a1, "https://example.com/report", {}, "Report", "Open report"}));
    CHECK(sheet->SetHyperlink({b2, {}, "'Sheet1'!A1", "Back", {}}));
    REQUIRE(sheet->Hyperlinks().size() == 2);
    CHECK(sheet->GetHyperlink(a1)->Target == "https://example.com/report");
    CHECK(sheet->GetHyperlink(b2)->Location == "'Sheet1'!A1");

    CHECK(sheet->RemoveHyperlink(a1));
    CHECK_FALSE(sheet->GetHyperlink(a1));
    CHECK_FALSE(sheet->RemoveHyperlink(a1));
}

TEST_CASE("Traditional comments replace values and clean an empty part [unit] [excel] [layout]")
{
    auto editor = ExcelDocumentEditor::CreateNew();
    auto sheet = editor->FirstWorksheet();
    const auto address = *CellAddress::ParseA1("C4");

    CHECK(sheet->SetComment({address, "Ada", "First"}));
    CHECK(sheet->SetComment({address, "Grace", "Replacement"}));
    REQUIRE(sheet->Comments().size() == 1);
    CHECK(sheet->GetComment(address)->Author == "Grace");
    CHECK(sheet->GetComment(address)->Text == "Replacement");

    CHECK(sheet->RemoveComment(address));
    CHECK(sheet->Comments().empty());
    CHECK(sheet->GetPart()->GetWorksheetCommentsPart() == nullptr);
}

TEST_CASE("A comment brings the legacy VML box Excel expects [unit] [excel] [layout]")
{
    auto editor = ExcelDocumentEditor::CreateNew();
    auto sheet = editor->FirstWorksheet();
    const auto address = *CellAddress::ParseA1("E3");
    REQUIRE(sheet->SetComment({address, "Reviewer", "Check this total"}));

    auto drawings = sheet->GetPart()->GetVmlDrawingParts();
    REQUIRE(drawings.size() == 1);
    const auto vml = drawings.front()->GetBinaryData();
    const std::string vmlText(vml.begin(), vml.end());
    CHECK(vmlText.find("ObjectType=\"Note\"") != std::string::npos);
    CHECK(vmlText.find("<x:Row>2</x:Row>") != std::string::npos);
    CHECK(vmlText.find("<x:Column>4</x:Column>") != std::string::npos);
    CHECK(sheet->GetPart()->GetXmlString().find("legacyDrawing") != std::string::npos);

    // The worksheet keeps its required child order: legacyDrawing before tableParts.
    auto reopened = ExcelDocumentEditor::Open(editor->SaveToMemory());
    REQUIRE(reopened != nullptr);
    auto reopenedSheet = reopened->FirstWorksheet();
    REQUIRE(reopenedSheet != nullptr);
    REQUIRE(reopenedSheet->Comments().size() == 1);
    CHECK(reopenedSheet->GetPart()->GetVmlDrawingParts().size() == 1);

    REQUIRE(reopenedSheet->RemoveComment(address));
    CHECK(reopenedSheet->GetPart()->GetVmlDrawingParts().empty());
    CHECK(reopenedSheet->GetPart()->GetXmlString().find("legacyDrawing") == std::string::npos);
}

TEST_CASE("Threaded comments create replies and clean supporting parts [unit] [excel] [layout]")
{
    auto editor = ExcelDocumentEditor::CreateNew();
    auto sheet = editor->FirstWorksheet();
    const auto address = *CellAddress::ParseA1("D5");

    ExcelThreadedComment root;
    root.Address = address;
    root.PersonName = "Ada";
    root.PersonEmail = "ada@example.test";
    root.Text = "Review this";
    const auto rootId = sheet->AddThreadedComment(root);
    REQUIRE(rootId);

    ExcelThreadedComment reply;
    reply.Address = address;
    reply.PersonName = "Grace";
    reply.Text = "Done";
    reply.ParentId = *rootId;
    REQUIRE(sheet->AddThreadedComment(reply));
    CHECK(sheet->ThreadedComments().size() == 2);

    CHECK(sheet->RemoveThreadedComment(*rootId));
    CHECK(sheet->ThreadedComments().empty());
    CHECK(sheet->GetPart()->GetWorksheetThreadedCommentsParts().empty());
}

TEST_CASE("Threaded comments report their author [unit] [excel] [layout]")
{
    auto editor = ExcelDocumentEditor::CreateNew();
    auto sheet = editor->FirstWorksheet();

    ExcelThreadedComment root;
    root.Address = *CellAddress::ParseA1("B2");
    root.PersonName = "Ada";
    root.PersonEmail = "ada@example.test";
    root.Text = "Review this";
    REQUIRE(sheet->AddThreadedComment(root));

    const auto reopened = ExcelDocumentEditor::Open(editor->SaveToMemory());
    REQUIRE(reopened != nullptr);
    const auto items = reopened->FirstWorksheet()->ThreadedComments();
    REQUIRE(items.size() == 1);

    // The author lives in the workbook person list; a reader that skipped the
    // join reported every comment as anonymous.
    CHECK(items.front().PersonName == "Ada");
    CHECK(items.front().PersonEmail == "ada@example.test");
    CHECK_FALSE(items.front().PersonId.empty());
}

TEST_CASE("Removing one threaded comment keeps the rest [unit] [excel] [layout]")
{
    auto editor = ExcelDocumentEditor::CreateNew();
    auto sheet = editor->FirstWorksheet();

    ExcelThreadedComment first;
    first.Address = *CellAddress::ParseA1("A1");
    first.PersonName = "Ada";
    first.Text = "First";
    const auto firstId = sheet->AddThreadedComment(first);
    REQUIRE(firstId);

    ExcelThreadedComment second;
    second.Address = *CellAddress::ParseA1("A2");
    second.PersonName = "Grace";
    second.Text = "Second";
    const auto secondId = sheet->AddThreadedComment(second);
    REQUIRE(secondId);

    REQUIRE(sheet->RemoveThreadedComment(*firstId));

    // Replaying the survivors through AddThreadedComment used to put them back
    // through its new-comment validation, which dropped every one of them.
    const auto remaining = sheet->ThreadedComments();
    REQUIRE(remaining.size() == 1);
    CHECK(remaining.front().Id == *secondId);
    CHECK(remaining.front().Text == "Second");
    CHECK(remaining.front().PersonName == "Grace");

    const auto reopened = ExcelDocumentEditor::Open(editor->SaveToMemory());
    REQUIRE(reopened != nullptr);
    CHECK(reopened->FirstWorksheet()->ThreadedComments().size() == 1);
}

TEST_CASE("The person list survives while another sheet still uses it [unit] [excel] [layout]")
{
    auto editor = ExcelDocumentEditor::CreateNew();
    auto first = editor->FirstWorksheet();
    auto second = editor->AddWorksheet("Second");
    REQUIRE(second != nullptr);

    ExcelThreadedComment onFirst;
    onFirst.Address = *CellAddress::ParseA1("A1");
    onFirst.PersonName = "Ada";
    onFirst.Text = "First sheet";
    const auto firstId = first->AddThreadedComment(onFirst);
    REQUIRE(firstId);

    ExcelThreadedComment onSecond;
    onSecond.Address = *CellAddress::ParseA1("A1");
    onSecond.PersonName = "Grace";
    onSecond.Text = "Second sheet";
    REQUIRE(second->AddThreadedComment(onSecond));

    REQUIRE(first->RemoveThreadedComment(*firstId));

    // The person list is workbook-level: emptying one sheet must not take away
    // the authors the other sheet still refers to.
    const auto workbook = editor->GetDocument()->GetWorkbookPart();
    REQUIRE(workbook != nullptr);
    CHECK_FALSE(workbook->GetWorkbookPersonParts().empty());

    const auto remaining = second->ThreadedComments();
    REQUIRE(remaining.size() == 1);
    CHECK(remaining.front().PersonName == "Grace");
}

TEST_CASE("Worksheet images own drawing and media parts [unit] [excel] [layout]")
{
    auto editor = ExcelDocumentEditor::CreateNew();
    auto sheet = editor->FirstWorksheet();
    ExcelWorksheetImage image;
    image.Name = "Logo";
    image.Description = "Company logo";
    image.From = *CellAddress::ParseA1("B2");
    image.To = *CellAddress::ParseA1("E8");
    image.Data = {0x89, 0x50, 0x4e, 0x47};

    const auto id = sheet->AddImage(image);
    REQUIRE(id);
    REQUIRE(sheet->Images().size() == 1);
    CHECK(sheet->Images().front().Data == image.Data);
    CHECK(sheet->GetPart()->GetDrawingsPart()->GetImageParts().size() == 1);

    CHECK(sheet->RemoveImage(*id));
    CHECK(sheet->Images().empty());
    CHECK(sheet->GetPart()->GetDrawingsPart() == nullptr);
}

TEST_CASE("Worksheet content survives a package round trip [unit] [excel] [layout]")
{
    auto editor = ExcelDocumentEditor::CreateNew();
    auto sheet = editor->FirstWorksheet();
    const auto address = *CellAddress::ParseA1("A1");
    REQUIRE(sheet->SetHyperlink({address, "https://example.com", {}, {}, {}}));
    REQUIRE(sheet->SetComment({address, "Author", "Text"}));

    const auto bytes = editor->SaveToMemory();
    REQUIRE_FALSE(bytes.empty());
    auto reopened = ExcelDocumentEditor::Open(bytes);
    REQUIRE(reopened);
    REQUIRE(reopened->FirstWorksheet()->GetHyperlink(address));
    CHECK(reopened->FirstWorksheet()->GetComment(address)->Text == "Text");
}

TEST_CASE("Hyperlink, comment, and threaded comment text round-trip XML special characters [unit] [excel] [layout]")
{
    auto editor = ExcelDocumentEditor::CreateNew();
    auto sheet = editor->FirstWorksheet();
    const auto address = *CellAddress::ParseA1("A1");

    REQUIRE(sheet->SetHyperlink(
        {address, "https://example.com/report?a=1&b=2", {}, "Report <1>", "Open \"report\""}));
    const auto hyperlink = sheet->GetHyperlink(address);
    REQUIRE(hyperlink);
    CHECK(hyperlink->Target == "https://example.com/report?a=1&b=2");
    CHECK(hyperlink->Display == "Report <1>");
    CHECK(hyperlink->Tooltip == "Open \"report\"");

    REQUIRE(sheet->SetComment({address, "Ada & Bob", "First <comment>"}));
    const auto comment = sheet->GetComment(address);
    REQUIRE(comment);
    CHECK(comment->Author == "Ada & Bob");
    CHECK(comment->Text == "First <comment>");

    ExcelThreadedComment threadedComment;
    threadedComment.Address = address;
    threadedComment.PersonName = "Grace \"G\" Hopper";
    threadedComment.Text = "Thread & reply";
    REQUIRE(sheet->AddThreadedComment(threadedComment));
    REQUIRE(sheet->ThreadedComments().size() == 1);
    CHECK(sheet->ThreadedComments().front().Text == "Thread & reply");
}

TEST_CASE("A hyperlink needs a destination and a valid cell [unit] [excel] [layout]")
{
    auto editor = ExcelDocumentEditor::CreateNew();
    auto sheet = editor->FirstWorksheet();
    const auto a1 = *CellAddress::ParseA1("A1");

    // Neither an external target nor a workbook location: there is nowhere to
    // go, so the link must be refused rather than written as a dead entry.
    CHECK_FALSE(sheet->SetHyperlink({a1, {}, {}, "Nowhere", {}}));
    CHECK(sheet->Hyperlinks().empty());

    // A default-constructed address is not a cell.
    CHECK_FALSE(sheet->SetHyperlink({CellAddress{}, "https://example.com", {}, {}, {}}));
    CHECK(sheet->Hyperlinks().empty());

    // Both at once is Excel's shape for an external link that lands on a spot
    // inside the target workbook: r:id carries the file, location the anchor.
    CHECK(sheet->SetHyperlink({a1, "https://example.com/data.xlsx", "Prices!B2", {}, {}}));
    const auto reopened = ExcelDocumentEditor::Open(editor->SaveToMemory());
    REQUIRE(reopened != nullptr);
    const auto link = reopened->FirstWorksheet()->GetHyperlink(a1);
    REQUIRE(link);
    CHECK(link->Target == "https://example.com/data.xlsx");
    CHECK(link->Location == "Prices!B2");
}

TEST_CASE("A threaded comment is accepted only with an author it can resolve [unit] [excel] [layout]")
{
    auto editor = ExcelDocumentEditor::CreateNew();
    auto sheet = editor->FirstWorksheet();
    const auto address = *CellAddress::ParseA1("E5");

    // No name and no person id: there is nobody to attribute the comment to.
    ExcelThreadedComment anonymous;
    anonymous.Address = address;
    anonymous.Text = "Who wrote this?";
    CHECK_FALSE(sheet->AddThreadedComment(anonymous));

    // An id the workbook has never seen is how a comment pasted from another
    // workbook arrives; without a display name it cannot mint a person record.
    ExcelThreadedComment foreign;
    foreign.Address = address;
    foreign.PersonId = "{11111111-2222-3333-4444-555555555555}";
    foreign.Text = "Pasted from elsewhere";
    CHECK_FALSE(sheet->AddThreadedComment(foreign));
    CHECK(sheet->ThreadedComments().empty());

    // A reply carrying only the id of an already-known person is the shape
    // ThreadedComments() itself hands back, and must round-trip.
    ExcelThreadedComment root;
    root.Address = address;
    root.PersonName = "Ada";
    root.Text = "Please check";
    const auto rootId = sheet->AddThreadedComment(root);
    REQUIRE(rootId);
    const auto stored = sheet->ThreadedComments();
    REQUIRE(stored.size() == 1);
    REQUIRE_FALSE(stored.front().PersonId.empty());

    ExcelThreadedComment reply;
    reply.Address = address;
    reply.PersonId = stored.front().PersonId;
    reply.ParentId = *rootId;
    reply.Text = "Checked";
    REQUIRE(sheet->AddThreadedComment(reply));

    const auto reopened = ExcelDocumentEditor::Open(editor->SaveToMemory());
    REQUIRE(reopened != nullptr);
    const auto items = reopened->FirstWorksheet()->ThreadedComments();
    REQUIRE(items.size() == 2);
    // The reply resolves to the same person the root minted.
    CHECK(items[0].PersonName == "Ada");
    CHECK(items[1].PersonName == "Ada");

    // A duplicate comment id is refused instead of silently forking the thread.
    ExcelThreadedComment duplicate;
    duplicate.Address = address;
    duplicate.Id = *rootId;
    duplicate.PersonName = "Ada";
    duplicate.Text = "Copy";
    CHECK_FALSE(sheet->AddThreadedComment(duplicate));
}
