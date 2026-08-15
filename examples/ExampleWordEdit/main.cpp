// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

// Editing a document that already exists — the other examples all author a
// document from nothing, which is the smaller half of what a document library
// is used for. This one opens a package, reads what is inside it, fills in
// placeholders that span several runs, inserts new content next to existing
// blocks through body cursors, extends a table that was already there, and
// saves the result under a new name so the input file stays untouched.
//
// Usage:
//   ExampleWordEdit [input.docx]
//
// Without an argument the example first writes the small template it then
// edits, so it runs from a clean checkout with no asset files.
#include "ExyokiOffice/Word/WordDocument.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <array>
#include <cstddef>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

using namespace ExyokiOffice::Word;
namespace W = ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing;
using ExyokiOffice::Color;
using ExyokiOffice::MeasurementUnit;
using ExyokiOffice::MeasuringUnits;

namespace ExampleWordEdit
{

MeasuringUnits Points(ExyokiOffice::Real value)
{
    return MeasuringUnits(value, MeasurementUnit::Point);
}

// The placeholders the template carries, and what they are filled with. A real
// application would read these from a database, a form, or a JSON payload.
constexpr std::array<std::pair<std::string_view, std::string_view>, 4> Fields = {{
    {"{{CUSTOMER}}", "Contoso Ltd."},
    {"{{PROJECT}}", "Document automation"},
    {"{{DATE}}", "2026-07-30"},
    {"{{AMOUNT}}", "48 500"},
}};

// Writes the template this example edits. Nothing here is specific to editing;
// it is the same authoring API examples/ExampleWordEditor tours in depth.
bool WriteSampleTemplate(const std::filesystem::path& path)
{
    auto editor = WordDocumentEditor::CreateNew();
    if (!editor)
    {
        return false;
    }

    auto properties = editor->Properties();
    properties.SetTitle("Statement of work (template)");
    properties.SetCreator("ExyokiOffice Library");

    editor->AddHeading("Statement of work", 1);

    // The customer placeholder is deliberately split across three runs, the way
    // a template edited by hand in Word ends up: the middle of the token
    // carries different formatting, so a naive per-run replacement misses it.
    auto intro = editor->AddParagraph();
    intro->AddText("This statement of work is agreed between ExyokiOffice and {{CUS");
    intro->AddRun("TOM", RunStyle{.Bold = true});
    intro->AddText("ER}} for the project {{PROJECT}}, effective {{DATE}}.");

    editor->AddHeading("Scope", 2);
    editor->AddParagraph("The scope is described in the schedule below.");

    auto table = editor->AddTable(2, 3);
    if (!table)
    {
        return false;
    }
    table->SetCellText(0, 0, "Phase");
    table->SetCellText(0, 1, "Deliverable");
    table->SetCellText(0, 2, "Fee");
    table->SetRowHeader(0, true);
    table->SetCellText(1, 0, "1");
    table->SetCellText(1, 1, "Analysis");
    table->SetCellText(1, 2, "12 000");

    editor->AddHeading("Fees", 2);
    editor->AddParagraph("The total fee is {{AMOUNT}} excluding VAT.");

    return editor->SaveToFile(path);
}

} // namespace ExampleWordEdit

int main(int argc, char** argv)
{
    using namespace ExampleWordEdit;

    const std::filesystem::path input = argc > 1 ? std::filesystem::path(argv[1])
                                                 : std::filesystem::path("StatementOfWork-template.docx");
    if (argc <= 1 && !WriteSampleTemplate(input))
    {
        std::cerr << "Failed to write the sample template " << input.string() << ".\n";
        return 1;
    }

    // --- Open the existing package. ------------------------------------------
    // Open() materializes only what is asked for and keeps everything it does
    // not understand, so parts this example never touches survive the edit.
    auto editor = WordDocumentEditor::Open(input);
    if (!editor)
    {
        std::cerr << "Failed to open " << input.string() << ".\n";
        return 1;
    }

    // --- Read what is inside before changing anything. -----------------------
    auto properties = editor->Properties();
    const auto paragraphs = editor->Paragraphs();
    const auto tables = editor->Tables();
    std::cout << "Opened " << input.string() << ": \"" << properties.GetTitle() << "\" by \""
              << properties.GetCreator() << "\", " << paragraphs.size() << " paragraphs, " << tables.size()
              << " table(s).\n";

    // --- Fill the placeholders. ----------------------------------------------
    // ReplaceAll works on the paragraph's concatenated run text, so a token
    // split across runs is still found; the replacement inherits the formatting
    // of the run the match starts in.
    ExyokiOffice::Size replacements = 0;
    for (const auto& paragraph : paragraphs)
    {
        for (const auto& [token, value] : Fields)
        {
            replacements += paragraph->ReplaceAll(token, value);
        }
    }
    if (replacements == 0)
    {
        std::cerr << "No placeholder was filled in - is " << input.string() << " the expected template?\n";
        return 1;
    }

    // --- Insert next to existing content. ------------------------------------
    // Anchoring on a paragraph that was read out of the document is what body
    // cursors are for: the new blocks land in the right place without rebuilding
    // anything around them.
    std::shared_ptr<Paragraph> scopeHeading;
    for (const auto& paragraph : paragraphs)
    {
        if (paragraph->PlainText() == "Scope")
        {
            scopeHeading = paragraph;
            break;
        }
    }
    if (!scopeHeading)
    {
        std::cerr << "The template has no \"Scope\" heading to anchor on.\n";
        return 1;
    }

    auto note = editor->After(scopeHeading).InsertParagraph("Added during the edit pass, directly after "
                                                            "the heading that was already in the file:");
    if (!note)
    {
        std::cerr << "Failed to insert the note paragraph.\n";
        return 1;
    }
    note->SetSpacing(Points(6.0), Points(6.0), std::nullopt).SetShading(Color(0xF2, 0xF6, 0xFB));

    // --- Extend the table that was already in the document. ------------------
    if (tables.empty())
    {
        std::cerr << "The template has no schedule table to extend.\n";
        return 1;
    }
    auto schedule = tables.front();
    const auto firstNewRow = schedule->GetRowCount();
    schedule->AddRow().AddRow();
    schedule->SetCellText(firstNewRow, 0, "2");
    schedule->SetCellText(firstNewRow, 1, "Implementation");
    schedule->SetCellText(firstNewRow, 2, "24 500");
    schedule->SetCellText(firstNewRow + 1, 0, "3");
    schedule->SetCellText(firstNewRow + 1, 1, "Handover");
    schedule->SetCellText(firstNewRow + 1, 2, "12 000");

    // --- Record the edit and save under a new name. --------------------------
    properties.SetTitle("Statement of work - Contoso Ltd.");
    properties.SetLastModifiedBy("ExampleWordEdit");

    const std::filesystem::path output = "StatementOfWork-filled.docx";
    if (!editor->SaveToFile(output))
    {
        std::cerr << "Failed to write " << output.string() << ".\n";
        return 1;
    }

    // --- Re-open the result, so the example verifies its own output. ---------
    auto reopened = WordDocumentEditor::Open(output);
    if (!reopened)
    {
        std::cerr << "Failed to re-open " << output.string() << ".\n";
        return 1;
    }

    ExyokiOffice::Size remainingPlaceholders = 0;
    for (const auto& paragraph : reopened->Paragraphs())
    {
        remainingPlaceholders += paragraph->FindAll("{{").size();
    }
    const auto reopenedTables = reopened->Tables();
    if (remainingPlaceholders != 0 || reopenedTables.empty())
    {
        std::cerr << "The filled document did not survive the round trip.\n";
        return 1;
    }

    std::cout << "Wrote " << output.string() << ": " << replacements << " placeholder(s) filled, "
              << reopened->Paragraphs().size() << " paragraphs, "
              << reopenedTables.front()->GetRowCount() << " schedule rows, title now \""
              << reopened->Properties().GetTitle() << "\".\n"
              << input.string() << " is unchanged - saving to a new path leaves the template intact.\n";
    return 0;
}
