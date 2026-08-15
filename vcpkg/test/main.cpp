// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

// Smoke test for the vcpkg package. It deliberately reaches only for what the
// port promises a consumer: the installed headers, the exported
// ExyokiOffice::ExyokiOffice target, and a round trip through all three
// document families. Anything deeper belongs in the repository test suite.

#include "ExyokiOffice/Excel/ExcelDocument.hpp"
#include "ExyokiOffice/PowerPoint/PowerPointDocument.hpp"
#include "ExyokiOffice/Tools/TextExtractor.hpp"
#include "ExyokiOffice/Version.hpp"
#include "ExyokiOffice/Word/WordDocument.hpp"

#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>

namespace
{

// The marker travels through the package and comes back out of the text
// extractor, so finding it proves the whole write/read path, not just that the
// file exists.
constexpr std::string_view Marker = "ExyokiOffice vcpkg smoke";

int failures = 0;

bool Check(bool condition, std::string_view what)
{
    std::cout << (condition ? "ok   " : "FAIL ") << what << '\n';
    if (!condition)
    {
        ++failures;
    }

    return condition;
}

bool ContainsMarker(const std::filesystem::path& path)
{
    const auto extracted = ExyokiOffice::Tools::Extract(path);
    if (!extracted.Ok)
    {
        return false;
    }

    for (const auto& block : extracted.Blocks)
    {
        if (block.Text.find(Marker) != std::string::npos)
        {
            return true;
        }
    }

    return false;
}

void CheckVersion()
{
    Check(ExyokiOffice::GetVersion() == ExyokiOffice::Version::String,
          "Version: the linked library matches its headers");
    Check(ExyokiOffice::GetAbiVersion() == ExyokiOffice::Version::Abi,
          "Version: the linked library matches its ABI identity");

    std::cout << "     linked ExyokiOffice " << ExyokiOffice::GetVersion() << " (ABI "
              << ExyokiOffice::GetAbiVersion() << ")\n";
}

void CheckWord(const std::filesystem::path& directory)
{
    using namespace ExyokiOffice::Word;

    const auto path = directory / "smoke.docx";

    const auto editor = WordDocumentEditor::CreateNew();
    if (!Check(editor != nullptr, "Word: CreateNew"))
    {
        return;
    }

    editor->AddHeading(Marker);
    editor->AddParagraph("This document was created by the ExyokiOffice vcpkg package.");

    Check(editor->SaveToFile(path), "Word: SaveToFile");
    Check(WordDocumentEditor::Open(path) != nullptr, "Word: Open");
    Check(ContainsMarker(path), "Word: text round-trips");
}

void CheckExcel(const std::filesystem::path& directory)
{
    using namespace ExyokiOffice::Excel;

    const auto path = directory / "smoke.xlsx";

    const auto editor = ExcelDocumentEditor::CreateNew();
    if (!Check(editor != nullptr, "Excel: CreateNew"))
    {
        return;
    }

    const auto sheet = editor->FirstWorksheet();
    if (!Check(sheet != nullptr, "Excel: FirstWorksheet"))
    {
        return;
    }

    Check(sheet->SetCellText(1, 1, Marker), "Excel: SetCellText");
    Check(editor->SaveToFile(path), "Excel: SaveToFile");

    const auto reopened = ExcelDocumentEditor::Open(path);
    if (!Check(reopened != nullptr, "Excel: Open"))
    {
        return;
    }

    // SetCellText stores the text in the workbook's shared string table, so the
    // reopened cell carries an index into it rather than the text itself.
    const auto reopenedSheet = reopened->FirstWorksheet();
    const auto value = reopenedSheet ? reopenedSheet->GetCellValue(1, 1) : std::nullopt;
    std::optional<std::string> text;
    if (value && value->Kind() == CellValueKind::SharedString)
    {
        text = reopened->SharedStrings().Lookup(value->SharedStringIndex().value_or(0));
    }
    else if (value)
    {
        text = value->Text();
    }

    Check(text.has_value() && *text == Marker, "Excel: cell A1 round-trips");
    Check(ContainsMarker(path), "Excel: text round-trips");
}

void CheckPowerPoint(const std::filesystem::path& directory)
{
    using namespace ExyokiOffice::PowerPoint;
    namespace Drawing = ExyokiOffice::DocumentFormat::OpenXml::Drawing;
    namespace Presentation = ExyokiOffice::DocumentFormat::OpenXml::Presentation;

    using ExyokiOffice::MeasurementUnit;
    using ExyokiOffice::MeasuringUnits;
    const auto Inches = [](double value)
    { return MeasuringUnits(value, MeasurementUnit::Inch); };

    const auto path = directory / "smoke.pptx";

    const auto editor = PowerPointDocumentEditor::CreateNew();
    if (!Check(editor != nullptr, "PowerPoint: CreateNew"))
    {
        return;
    }

    editor->SetSlideSize(PresentationSlideSize::Widescreen16x9());

    // Every slide needs a layout, and every layout a master.
    const auto master = editor->AddSlideMaster("Default");
    const auto layout =
        editor->AddSlideLayout(master, "Title", Presentation::SlideLayoutValues::Title);

    const auto slide = editor->AddSlide();
    if (!Check(slide != nullptr, "PowerPoint: AddSlide"))
    {
        return;
    }

    const auto shape = slide->ShapeTree()->AddShape("Title");
    editor->SetSlideLayout(0, layout);
    shape->SetPresetGeometry(Drawing::ShapeTypeValues::Rectangle);
    shape->SetTransform({.Position = {Inches(0.75), Inches(2.5)},
                         .Size = {Inches(11.833), Inches(1.3)}});

    PresentationTextRun run;
    run.Text = Marker;
    run.Language = "en-US";
    run.Bold = true;

    PresentationTextFrame frame;
    frame.Paragraphs = {PresentationTextParagraph{.Runs = {run}}};
    Check(shape->SetTextFrame(frame), "PowerPoint: SetTextFrame");

    Check(editor->SaveToFile(path), "PowerPoint: SaveToFile");

    const auto reopened = PowerPointDocumentEditor::Open(path);
    Check(reopened != nullptr && reopened->SlideCount() == 1, "PowerPoint: Open");
    Check(ContainsMarker(path), "PowerPoint: text round-trips");
}

} // namespace

int main()
{
    // CTest runs this from the build directory; keep the produced packages in a
    // subdirectory of their own so a failed run is easy to inspect.
    const auto directory = std::filesystem::current_path() / "smoke-output";

    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error)
    {
        std::cerr << "Cannot create " << directory.string() << ": " << error.message() << '\n';
        return 1;
    }

    CheckVersion();
    CheckWord(directory);
    CheckExcel(directory);
    CheckPowerPoint(directory);

    if (failures != 0)
    {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }

    std::cout << "All checks passed\n";
    return 0;
}
