// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "doctest.h"

#include "TestSupport.hpp"

#include "ExyokiOffice/Excel/ExcelDocument.hpp"
#include "ExyokiOffice/PowerPoint/PowerPointDocument.hpp"
#include "ExyokiOffice/Tools/DocumentTools.hpp"
#include "ExyokiOffice/Word/WordDocument.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <filesystem>

namespace
{
using namespace ExyokiOffice::Tools;
using ExyokiOffice::Excel::CellAddress;
using ExyokiOffice::Excel::ExcelDocumentEditor;
using ExyokiOffice::PowerPoint::PowerPointDocumentEditor;

std::filesystem::path Temp(std::string_view name)
{
    const std::filesystem::path parts(name);
    return ExyokiOfficeTests::MakeTemporaryPath(parts.stem().string(), parts.extension().string());
}

CellAddress Address(std::string_view value)
{
    auto address = CellAddress::ParseA1(value);
    REQUIRE(address);
    return *address;
}

std::string CellText(const ExcelDocumentEditor::Ptr& editor,
                     const ExyokiOffice::Excel::Worksheet::Ptr& sheet,
                     CellAddress address)
{
    auto value = sheet->GetCellValue(address);
    if (!value)
    {
        return {};
    }
    if (auto index = value->SharedStringIndex())
    {
        return editor->SharedStrings().Lookup(*index).value_or(std::string{});
    }
    return value->Text();
}

void Remove(const std::filesystem::path& path)
{
    std::error_code error;
    std::filesystem::remove_all(path, error);
}
} // namespace

TEST_SUITE("DocumentToolsTests")
{
    TEST_CASE("Excel split groups worksheets in order and preserves content graphs [unit] [tools] [document-tools]")
    {
        const auto input = Temp("exyokioffice_document_tools_split.xlsx");
        const auto directory = Temp("exyokioffice_document_tools_excel_parts");
        Remove(input);
        Remove(directory);
        auto workbook = ExcelDocumentEditor::CreateNew();
        REQUIRE(workbook);
        REQUIRE(workbook->RenameWorksheet(0, "First"));
        REQUIRE(workbook->FirstWorksheet()->SetCellText(Address("A1"), "alpha"));
        REQUIRE(workbook->AddWorksheet("Second")->SetCellText(Address("A1"), "beta"));
        auto third = workbook->AddWorksheet("Third");
        REQUIRE(third);
        REQUIRE(third->SetCellText(Address("A1"), "gamma"));
        ExyokiOffice::Excel::ExcelWorksheetImage image;
        image.Name = "Logo";
        image.From = Address("B2");
        image.To = Address("C3");
        image.Data = {0x89, 0x50, 0x4e, 0x47};
        REQUIRE(third->AddImage(image));
        REQUIRE(workbook->SaveToFile(input));

        DocumentSplitOptions options;
        options.ItemCount = 2;
        options.OutputPrefix = "book";
        const auto split = SplitDocument(input, directory, options);
        REQUIRE(split.Ok);
        CHECK(split.Family == DocumentFamily::Excel);
        REQUIRE(split.OutputFiles.size() == 2);
        CHECK(split.OutputFiles[0].extension() == ".xlsx");
        auto first = ExcelDocumentEditor::Open(split.OutputFiles[0]);
        auto second = ExcelDocumentEditor::Open(split.OutputFiles[1]);
        REQUIRE(first);
        REQUIRE(second);
        REQUIRE(first->Worksheets().size() == 2);
        CHECK(first->Worksheets()[0]->Name() == "First");
        CHECK(first->Worksheets()[1]->Name() == "Second");
        CHECK(CellText(first, first->Worksheets()[1], Address("A1")) == "beta");
        REQUIRE(second->Worksheets().size() == 1);
        CHECK(second->FirstWorksheet()->Name() == "Third");
        CHECK(CellText(second, second->FirstWorksheet(), Address("A1")) == "gamma");
        REQUIRE(second->FirstWorksheet()->Images().size() == 1);
        CHECK(second->FirstWorksheet()->Images()[0].Data == image.Data);
        Remove(input);
        Remove(directory);
    }

    TEST_CASE("PowerPoint split groups slides and preserves slide-owned relationships [unit] [tools] [document-tools]")
    {
        const auto input = Temp("exyokioffice_document_tools_split.pptx");
        const auto directory = Temp("exyokioffice_document_tools_powerpoint_parts");
        Remove(input);
        Remove(directory);
        auto presentation = PowerPointDocumentEditor::CreateNew();
        REQUIRE(presentation);
        for (int index = 0; index < 3; ++index)
        {
            auto slide = presentation->AddSlide();
            REQUIRE(slide);
            slide->SetHidden(index == 1);
        }
        auto image = presentation->GetSlide(2)->GetPart()->AddImagePart();
        REQUIRE(image);
        image->SetContentType("image/png");
        image->SetBinaryData({1, 2, 3, 4});
        REQUIRE(presentation->SaveToFile(input));

        DocumentSplitOptions options;
        options.ItemCount = 2;
        const auto split = SplitDocument(input, directory, options);
        REQUIRE(split.Ok);
        CHECK(split.Family == DocumentFamily::PowerPoint);
        REQUIRE(split.OutputFiles.size() == 2);
        auto first = PowerPointDocumentEditor::Open(split.OutputFiles[0]);
        auto second = PowerPointDocumentEditor::Open(split.OutputFiles[1]);
        REQUIRE(first);
        REQUIRE(second);
        CHECK(first->SlideCount() == 2);
        CHECK_FALSE(first->GetSlide(0)->IsHidden());
        CHECK(first->GetSlide(1)->IsHidden());
        REQUIRE(second->SlideCount() == 1);
        CHECK_FALSE(second->GetSlide(0)->IsHidden());
        REQUIRE(second->GetSlide(0)->GetPart()->GetImageParts().size() == 1);
        CHECK(second->GetSlide(0)->GetPart()->GetImageParts()[0]->GetBinaryData() == std::vector<ExyokiOffice::Byte>({1, 2, 3, 4}));
        Remove(input);
        Remove(directory);
    }

    TEST_CASE("Excel merge remaps shared strings and creates unique worksheet names [unit] [tools] [document-tools]")
    {
        const auto leftPath = Temp("exyokioffice_document_tools_left.xlsx");
        const auto rightPath = Temp("exyokioffice_document_tools_right.xlsx");
        const auto outputPath = Temp("exyokioffice_document_tools_merged.xlsx");
        Remove(leftPath);
        Remove(rightPath);
        Remove(outputPath);
        auto left = ExcelDocumentEditor::CreateNew();
        auto right = ExcelDocumentEditor::CreateNew();
        REQUIRE(left);
        REQUIRE(right);
        REQUIRE(left->FirstWorksheet()->SetCellText(Address("A1"), "left text"));
        REQUIRE(right->FirstWorksheet()->SetCellText(Address("A1"), "right text"));
        REQUIRE(right->FirstWorksheet()->SetCellFormula(Address("B1"), "1+2"));
        REQUIRE(left->SaveToFile(leftPath));
        REQUIRE(right->SaveToFile(rightPath));

        const auto merged = MergeDocuments({leftPath, rightPath}, outputPath);
        REQUIRE(merged.Ok);
        CHECK(merged.Family == DocumentFamily::Excel);
        CHECK(merged.DocumentsMerged == 2);
        CHECK(merged.ItemsMerged == 2);
        auto reopened = ExcelDocumentEditor::Open(outputPath);
        REQUIRE(reopened);
        REQUIRE(reopened->Worksheets().size() == 2);
        CHECK(reopened->Worksheets()[0]->Name() == "Sheet1");
        CHECK(reopened->Worksheets()[1]->Name() != "Sheet1");
        CHECK(CellText(reopened, reopened->Worksheets()[0], Address("A1")) == "left text");
        CHECK(CellText(reopened, reopened->Worksheets()[1], Address("A1")) == "right text");
        CHECK(reopened->Worksheets()[1]->GetCellValue(Address("B1"))->FormulaValue().Formula == "1+2");
        Remove(leftPath);
        Remove(rightPath);
        Remove(outputPath);
    }

    TEST_CASE("Excel merge imports worksheet image graphs [unit] [tools] [document-tools]")
    {
        const auto leftPath = Temp("exyokioffice_document_tools_graph_left.xlsx");
        const auto rightPath = Temp("exyokioffice_document_tools_graph_right.xlsx");
        const auto outputPath = Temp("exyokioffice_document_tools_graph_merged.xlsx");
        Remove(leftPath);
        Remove(rightPath);
        Remove(outputPath);
        auto left = ExcelDocumentEditor::CreateNew();
        auto right = ExcelDocumentEditor::CreateNew();
        ExyokiOffice::Excel::ExcelWorksheetImage value;
        value.Name = "Imported";
        value.From = Address("A1");
        value.To = Address("D5");
        value.Data = {9, 8, 7};
        REQUIRE(right->FirstWorksheet()->AddImage(value));
        REQUIRE(left->SaveToFile(leftPath));
        REQUIRE(right->SaveToFile(rightPath));
        REQUIRE(MergeDocuments({leftPath, rightPath}, outputPath).Ok);
        auto reopened = ExcelDocumentEditor::Open(outputPath);
        REQUIRE(reopened);
        REQUIRE(reopened->Worksheets().size() == 2);
        REQUIRE(reopened->Worksheets()[1]->Images().size() == 1);
        CHECK(reopened->Worksheets()[1]->Images()[0].Data == value.Data);
        Remove(leftPath);
        Remove(rightPath);
        Remove(outputPath);
    }

    TEST_CASE("Excel merge rejects incompatible style catalogs without writing output [unit] [tools] [document-tools]")
    {
        const auto leftPath = Temp("exyokioffice_document_tools_style_left.xlsx");
        const auto rightPath = Temp("exyokioffice_document_tools_style_right.xlsx");
        const auto outputPath = Temp("exyokioffice_document_tools_style_merged.xlsx");
        Remove(leftPath);
        Remove(rightPath);
        Remove(outputPath);
        auto left = ExcelDocumentEditor::CreateNew();
        auto right = ExcelDocumentEditor::CreateNew();
        ExyokiOffice::Excel::ExcelStyle style;
        ExyokiOffice::Excel::ExcelFont font;
        font.Bold = true;
        style.Font = font;
        const auto registered = right->Styles().GetOrAdd(style);
        REQUIRE(registered);
        REQUIRE(right->Styles().ApplyToCell(*right->FirstWorksheet(), Address("A1"), registered.StyleIndex));
        REQUIRE(left->SaveToFile(leftPath));
        REQUIRE(right->SaveToFile(rightPath));
        const auto merged = MergeDocuments({leftPath, rightPath}, outputPath);
        CHECK_FALSE(merged.Ok);
        CHECK_FALSE(std::filesystem::exists(outputPath));
        REQUIRE_FALSE(merged.Diagnostics.empty());
        CHECK(merged.Diagnostics.back().Message.find("style") != std::string::npos);
        Remove(leftPath);
        Remove(rightPath);
        Remove(outputPath);
    }

    TEST_CASE("Cross-workbook worksheet copy accepts equivalent styles and leaves source independent [unit] [tools] [document-tools]")
    {
        auto source = ExcelDocumentEditor::CreateNew();
        auto target = ExcelDocumentEditor::CreateNew();
        REQUIRE(source);
        REQUIRE(target);
        ExyokiOffice::Excel::ExcelStyle style;
        ExyokiOffice::Excel::ExcelFont font;
        font.Italic = true;
        style.Font = font;
        const auto sourceStyle = source->Styles().GetOrAdd(style);
        const auto targetStyle = target->Styles().GetOrAdd(style);
        REQUIRE(sourceStyle);
        REQUIRE(targetStyle);
        REQUIRE(sourceStyle.StyleIndex == targetStyle.StyleIndex);
        REQUIRE(source->Styles().ApplyToCell(*source->FirstWorksheet(), Address("C4"), sourceStyle.StyleIndex));
        REQUIRE(source->FirstWorksheet()->SetCellText(Address("C4"), "styled source"));

        auto imported = target->CopyWorksheetFrom(*source, 0, "Imported");
        REQUIRE(imported);
        CHECK(target->Worksheets().size() == 2);
        CHECK(target->Styles().CellStyleIndex(*imported, Address("C4")) == sourceStyle.StyleIndex);
        CHECK(CellText(target, imported, Address("C4")) == "styled source");
        REQUIRE(imported->SetCellText(Address("C4"), "target edit"));
        CHECK(CellText(source, source->FirstWorksheet(), Address("C4")) == "styled source");
        CHECK(CellText(target, imported, Address("C4")) == "target edit");
    }

    TEST_CASE("Cross-workbook worksheet copy validates ownership index and names without mutation [unit] [tools] [document-tools]")
    {
        auto source = ExcelDocumentEditor::CreateNew();
        auto target = ExcelDocumentEditor::CreateNew();
        REQUIRE(source);
        REQUIRE(target);
        const auto before = target->Worksheets().size();
        CHECK(target->CopyWorksheetFrom(*target, 0) == nullptr);
        CHECK(target->CopyWorksheetFrom(*source, 99) == nullptr);
        CHECK(target->CopyWorksheetFrom(*source, 0, "Sheet1") == nullptr);
        CHECK(target->CopyWorksheetFrom(*source, 0, "bad/name") == nullptr);
        CHECK(target->Worksheets().size() == before);
        CHECK(source->Worksheets().size() == 1);
    }

    TEST_CASE("PowerPoint merge preserves order notes media and hidden state [unit] [tools] [document-tools]")
    {
        const auto leftPath = Temp("exyokioffice_document_tools_left.pptx");
        const auto rightPath = Temp("exyokioffice_document_tools_right.pptx");
        const auto outputPath = Temp("exyokioffice_document_tools_merged.pptx");
        Remove(leftPath);
        Remove(rightPath);
        Remove(outputPath);
        auto left = PowerPointDocumentEditor::CreateNew();
        auto right = PowerPointDocumentEditor::CreateNew();
        auto leftSlide = left->AddSlide();
        auto rightSlide = right->AddSlide();
        REQUIRE(leftSlide);
        REQUIRE(rightSlide);
        REQUIRE(leftSlide->SetNotesText("left"));
        REQUIRE(rightSlide->SetNotesText("right"));
        REQUIRE(rightSlide->SetHidden(true));
        auto image = rightSlide->GetPart()->AddImagePart();
        REQUIRE(image);
        image->SetContentType("image/png");
        image->SetBinaryData({5, 4, 3});
        REQUIRE(left->SaveToFile(leftPath));
        REQUIRE(right->SaveToFile(rightPath));
        const auto merged = MergeDocuments({leftPath, rightPath}, outputPath);
        REQUIRE(merged.Ok);
        CHECK(merged.DocumentsMerged == 2);
        CHECK(merged.ItemsMerged == 2);
        auto reopened = PowerPointDocumentEditor::Open(outputPath);
        REQUIRE(reopened);
        REQUIRE(reopened->SlideCount() == 2);
        CHECK(reopened->GetSlide(0)->NotesText() == "left");
        CHECK(reopened->GetSlide(1)->NotesText() == "right");
        CHECK(reopened->GetSlide(1)->IsHidden());
        REQUIRE(reopened->GetSlide(1)->GetPart()->GetImageParts().size() == 1);
        CHECK(reopened->GetSlide(1)->GetPart()->GetImageParts()[0]->GetBinaryData() == std::vector<ExyokiOffice::Byte>({5, 4, 3}));
        Remove(leftPath);
        Remove(rightPath);
        Remove(outputPath);
    }

    TEST_CASE("Merge rejects mixed families before creating output [unit] [tools] [document-tools]")
    {
        const auto wordPath = Temp("exyokioffice_document_tools_mixed.docx");
        const auto excelPath = Temp("exyokioffice_document_tools_mixed.xlsx");
        const auto outputPath = Temp("exyokioffice_document_tools_mixed_output.docx");
        Remove(wordPath);
        Remove(excelPath);
        Remove(outputPath);
        auto word = ExyokiOffice::Word::WordDocumentEditor::CreateNew();
        auto excel = ExcelDocumentEditor::CreateNew();
        REQUIRE(word->SaveToFile(wordPath));
        REQUIRE(excel->SaveToFile(excelPath));
        const auto merged = MergeDocuments({wordPath, excelPath}, outputPath);
        CHECK_FALSE(merged.Ok);
        CHECK_FALSE(std::filesystem::exists(outputPath));
        REQUIRE_FALSE(merged.Diagnostics.empty());
        CHECK(merged.Diagnostics.back().Message.find("same document family") != std::string::npos);
        Remove(wordPath);
        Remove(excelPath);
        Remove(outputPath);
    }

    TEST_CASE("Split validates family-specific strategies and preflights output collisions [unit] [tools] [document-tools]")
    {
        const auto input = Temp("exyokioffice_document_tools_options.xlsx");
        const auto directory = Temp("exyokioffice_document_tools_options_parts");
        Remove(input);
        Remove(directory);
        auto workbook = ExcelDocumentEditor::CreateNew();
        REQUIRE(workbook->SaveToFile(input));
        DocumentSplitOptions invalid;
        invalid.Strategy = DocumentSplitStrategy::Slides;
        CHECK_FALSE(SplitDocument(input, directory, invalid).Ok);

        DocumentSplitOptions valid;
        const auto first = SplitDocument(input, directory, valid);
        REQUIRE(first.Ok);
        REQUIRE(first.OutputFiles.size() == 1);
        const auto second = SplitDocument(input, directory, valid);
        CHECK_FALSE(second.Ok);
        REQUIRE_FALSE(second.Diagnostics.empty());
        CHECK(second.Diagnostics.back().Message.find("already exists") != std::string::npos);
        Remove(input);
        Remove(directory);
    }

    TEST_CASE("Split refuses an output prefix that leaves the output directory [unit] [tools] [document-tools]")
    {
        const auto input = Temp("exyokioffice_document_tools_prefix.xlsx");
        const auto directory = Temp("exyokioffice_document_tools_prefix_parts");
        Remove(input);
        Remove(directory);
        auto workbook = ExcelDocumentEditor::CreateNew();
        REQUIRE(workbook->SaveToFile(input));

        // The prefix is concatenated straight into every output name, so it has
        // to be a file-name fragment and nothing more.
        for (const std::string prefix : {"../evil", "sub/evil", R"(..\evil)", "NUL"})
        {
            DocumentSplitOptions options;
            options.OutputPrefix = prefix;
            const auto split = SplitDocument(input, directory, options);
            CHECK_FALSE(split.Ok);
            CHECK(split.OutputFiles.empty());
            REQUIRE_FALSE(split.Diagnostics.empty());
            CHECK(split.Diagnostics.back().Message.find("plain file name") != std::string::npos);
        }

        CHECK_FALSE(std::filesystem::exists(directory.parent_path() / "evil_01.xlsx"));

        // An empty prefix keeps its documented meaning, and an ordinary one is
        // untouched by the check.
        DocumentSplitOptions accepted;
        accepted.OutputPrefix.clear();
        CHECK(SplitDocument(input, directory, accepted).Ok);
        Remove(directory);
        accepted.OutputPrefix = "book";
        CHECK(SplitDocument(input, directory, accepted).Ok);

        Remove(input);
        Remove(directory);
    }
}
