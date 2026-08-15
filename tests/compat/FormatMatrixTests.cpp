// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

// The "File formats", "Other formats" and "Office versions" sections of
// docs/Compatibility.md.
//
// Every document type the matrix lists is created from nothing, saved, opened
// again, and saved once more; the round trip is what the Create/Open/Save
// columns actually promise. The conversion rows check the direction and the
// fidelity the matrix claims — including the two places it says "no", which are
// as much part of the contract as the yes cells.

#include "doctest.h"

#include "TestSupport.hpp"

#include "ExyokiOffice/Excel/ExcelDocument.hpp"
#include "ExyokiOffice/FileFormatVersions.h"
#include "ExyokiOffice/MarkupCompatibility.hpp"
#include "ExyokiOffice/OpenXmlDomValidator.hpp"
#include "ExyokiOffice/OpenXmlPackage.hpp"
#include "ExyokiOffice/OpenXmlPackageValidator.hpp"
#include "ExyokiOffice/PowerPoint/PowerPointDocument.hpp"
#include "ExyokiOffice/Tools/DocumentConverter.hpp"
#include "ExyokiOffice/Tools/FlatOpcConverter.hpp"
#include "ExyokiOffice/Tools/PackageDiff.hpp"
#include "ExyokiOffice/Word/WordDocument.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <array>
#include <fstream>
#include <string>
#include <string_view>

namespace
{

using ExyokiOffice::Excel::ExcelDocumentEditor;
using ExyokiOffice::PowerPoint::PowerPointDocumentEditor;
using ExyokiOffice::Word::WordDocumentEditor;
using ExyokiOfficeTests::MakeTemporaryPath;
using ExyokiOfficeTests::ScopedTemporaryFile;

using ExyokiOffice::Packaging::PowerPointDocumentType;
using ExyokiOffice::Packaging::SpreadsheetDocumentType;
using ExyokiOffice::Packaging::WordprocessingDocumentType;

/// Writes @p text to @p path, so a conversion can be asked to read it back.
void WriteTextFile(const std::filesystem::path& path, std::string_view text)
{
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    REQUIRE(stream.is_open());
    stream.write(text.data(), static_cast<std::streamsize>(text.size()));
}

std::string ReadTextFile(const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    REQUIRE(stream.is_open());
    return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
}

} // namespace

TEST_SUITE("FormatMatrixTests")
{

    TEST_CASE("Every Word document type is created, saved, opened and saved again "
              "[compat] [word] [formats-word]")
    {
        struct Case
        {
            WordprocessingDocumentType Type;
            std::string_view Extension;
        };

        constexpr std::array cases{
            Case{WordprocessingDocumentType::Document, ".docx"},
            Case{WordprocessingDocumentType::Template, ".dotx"},
            Case{WordprocessingDocumentType::MacroEnabledDocument, ".docm"},
            Case{WordprocessingDocumentType::MacroEnabledTemplate, ".dotm"}};

        for (const auto& testCase : cases)
        {
            CAPTURE(testCase.Extension);

            // Create.
            auto editor = WordDocumentEditor::CreateNew(testCase.Type);
            REQUIRE(editor != nullptr);
            REQUIRE(editor->AddParagraph("Format matrix") != nullptr);

            // Save and open, both through a file and through memory.
            const ScopedTemporaryFile file("ExyokiOffice_formats_word", testCase.Extension);
            REQUIRE(editor->SaveToFile(file.Path()));

            auto fromDisk = WordDocumentEditor::Open(file.Path());
            REQUIRE(fromDisk != nullptr);
            CHECK(fromDisk->GetDocument()->GetDocumentType() == testCase.Type);

            const auto bytes = editor->SaveToMemory();
            REQUIRE_FALSE(bytes.empty());
            auto fromMemory = WordDocumentEditor::Open(bytes);
            REQUIRE(fromMemory != nullptr);
            CHECK(fromMemory->GetDocument()->GetDocumentType() == testCase.Type);
            REQUIRE_FALSE(fromMemory->Paragraphs().empty());
            CHECK(fromMemory->Paragraphs().front()->PlainText() == "Format matrix");

            // Save again: the type has to survive the second write too.
            auto resaved = ExyokiOfficeTests::RoundTrip(fromMemory);
            REQUIRE(resaved != nullptr);
            CHECK(resaved->GetDocument()->GetDocumentType() == testCase.Type);

            const auto validation = ExyokiOfficeTests::ValidatePackage(bytes);
            CAPTURE(validation.FirstError);
            CHECK_FALSE(validation.HasErrors);
        }
    }

    TEST_CASE("Every Excel document type is created, saved, opened and saved again "
              "[compat] [excel] [formats-excel]")
    {
        struct Case
        {
            SpreadsheetDocumentType Type;
            std::string_view Extension;
        };

        constexpr std::array cases{
            Case{SpreadsheetDocumentType::Workbook, ".xlsx"},
            Case{SpreadsheetDocumentType::Template, ".xltx"},
            Case{SpreadsheetDocumentType::MacroEnabledWorkbook, ".xlsm"},
            Case{SpreadsheetDocumentType::MacroEnabledTemplate, ".xltm"}};

        for (const auto& testCase : cases)
        {
            CAPTURE(testCase.Extension);

            auto editor = ExcelDocumentEditor::CreateNew(testCase.Type);
            REQUIRE(editor != nullptr);
            REQUIRE(editor->FirstWorksheet() != nullptr);
            REQUIRE(editor->FirstWorksheet()->SetCellText(1, 1, "Format matrix"));

            const ScopedTemporaryFile file("ExyokiOffice_formats_excel", testCase.Extension);
            REQUIRE(editor->SaveToFile(file.Path()));

            auto fromDisk = ExcelDocumentEditor::Open(file.Path());
            REQUIRE(fromDisk != nullptr);
            CHECK(fromDisk->GetDocument()->GetDocumentType() == testCase.Type);

            const auto bytes = editor->SaveToMemory();
            REQUIRE_FALSE(bytes.empty());
            auto fromMemory = ExcelDocumentEditor::Open(bytes);
            REQUIRE(fromMemory != nullptr);
            CHECK(fromMemory->GetDocument()->GetDocumentType() == testCase.Type);

            auto resaved = ExyokiOfficeTests::RoundTrip(fromMemory);
            REQUIRE(resaved != nullptr);
            CHECK(resaved->GetDocument()->GetDocumentType() == testCase.Type);

            const auto validation = ExyokiOfficeTests::ValidatePackage(bytes);
            CAPTURE(validation.FirstError);
            CHECK_FALSE(validation.HasErrors);
        }
    }

    TEST_CASE("Every PowerPoint document type is created, saved, opened and saved again "
              "[compat] [powerpoint] [formats-powerpoint]")
    {
        struct Case
        {
            PowerPointDocumentType Type;
            std::string_view Extension;
        };

        constexpr std::array cases{
            Case{PowerPointDocumentType::Presentation, ".pptx"},
            Case{PowerPointDocumentType::Template, ".potx"},
            Case{PowerPointDocumentType::SlideShow, ".ppsx"},
            Case{PowerPointDocumentType::MacroEnabledPresentation, ".pptm"},
            Case{PowerPointDocumentType::MacroEnabledTemplate, ".potm"},
            Case{PowerPointDocumentType::MacroEnabledSlideShow, ".ppsm"}};

        for (const auto& testCase : cases)
        {
            CAPTURE(testCase.Extension);

            auto editor = PowerPointDocumentEditor::CreateNew(testCase.Type);
            REQUIRE(editor != nullptr);

            const ScopedTemporaryFile file("ExyokiOffice_formats_powerpoint", testCase.Extension);
            REQUIRE(editor->SaveToFile(file.Path()));

            auto fromDisk = PowerPointDocumentEditor::Open(file.Path());
            REQUIRE(fromDisk != nullptr);
            CHECK(fromDisk->GetDocument()->GetDocumentType() == testCase.Type);

            const auto bytes = editor->SaveToMemory();
            REQUIRE_FALSE(bytes.empty());
            auto fromMemory = PowerPointDocumentEditor::Open(bytes);
            REQUIRE(fromMemory != nullptr);
            CHECK(fromMemory->GetDocument()->GetDocumentType() == testCase.Type);

            auto resaved = ExyokiOfficeTests::RoundTrip(fromMemory);
            REQUIRE(resaved != nullptr);
            CHECK(resaved->GetDocument()->GetDocumentType() == testCase.Type);

            const auto validation = ExyokiOfficeTests::ValidatePackage(bytes);
            CAPTURE(validation.FirstError);
            CHECK_FALSE(validation.HasErrors);
        }
    }

    TEST_CASE("Flat OPC is lossless in both directions [compat] [formats-flatopc]")
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);
        REQUIRE(editor->AddParagraph("Flat OPC keeps every part.") != nullptr);
        auto table = editor->AddTable(2, 2);
        REQUIRE(table != nullptr);
        table->SetCellText(0, 0, "cell");

        const ScopedTemporaryFile original("ExyokiOffice_flatopc_original", ".docx");
        const ScopedTemporaryFile flat("ExyokiOffice_flatopc", ".xml");
        const ScopedTemporaryFile rebuilt("ExyokiOffice_flatopc_rebuilt", ".docx");
        REQUIRE(editor->SaveToFile(original.Path()));

        const auto toFlat = ExyokiOffice::Tools::ConvertToFlatOpc(original.Path(), flat.Path());
        REQUIRE(toFlat.Ok);
        CHECK(toFlat.PartCount > 0);

        const auto fromFlat = ExyokiOffice::Tools::ConvertFromFlatOpc(flat.Path(), rebuilt.Path());
        REQUIRE(fromFlat.Ok);

        // "Lossless" in the matrix means exactly this: the rebuilt package has
        // the same parts, content types and relationships as the original.
        const auto diff = ExyokiOffice::Tools::Compare(original.Path(), rebuilt.Path());
        REQUIRE(diff.Ok);
        for (const auto& change : diff.PartChanges)
        {
            CAPTURE(change.Uri);
            CHECK_MESSAGE(false, "part changed through Flat OPC");
        }
        CHECK(diff.Identical);

        auto reopened = WordDocumentEditor::Open(rebuilt.Path());
        REQUIRE(reopened != nullptr);
        CHECK(reopened->Tables().size() == 1);
    }

    TEST_CASE("Markdown, JSON, semantic XML and plain text convert in the documented directions "
              "[compat] [formats-conversion]")
    {
        using namespace ExyokiOffice::Tools;

        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);
        REQUIRE(editor->AddHeading("Conversion", 1) != nullptr);
        REQUIRE(editor->AddParagraph("Structure survives the round trip.") != nullptr);

        const ScopedTemporaryFile docx("ExyokiOffice_convert", ".docx");
        REQUIRE(editor->SaveToFile(docx.Path()));

        SUBCASE("Office to every text format and back")
        {
            struct Case
            {
                std::string_view Extension;
                ConvertFormat Format;
            };

            constexpr std::array cases{Case{".md", ConvertFormat::Markdown},
                                       Case{".json", ConvertFormat::Json},
                                       Case{".xml", ConvertFormat::Xml},
                                       Case{".txt", ConvertFormat::Text}};

            for (const auto& testCase : cases)
            {
                CAPTURE(testCase.Extension);

                const ScopedTemporaryFile text("ExyokiOffice_convert_out", testCase.Extension);
                const ScopedTemporaryFile back("ExyokiOffice_convert_back", ".docx");

                const auto exported = ConvertDocument(docx.Path(), text.Path());
                REQUIRE(exported.Ok);
                CHECK(exported.To == testCase.Format);
                CHECK(exported.Family == DocumentFamily::Word);
                CHECK_FALSE(ReadTextFile(text.Path()).empty());

                const auto imported = ConvertDocument(text.Path(), back.Path());
                REQUIRE(imported.Ok);
                CHECK(imported.From == testCase.Format);

                auto reopened = WordDocumentEditor::Open(back.Path());
                REQUIRE(reopened != nullptr);
                REQUIRE_FALSE(reopened->Paragraphs().empty());

                // Lossy by design, but the text itself has to come through.
                bool foundBodyText = false;
                for (const auto& paragraph : reopened->Paragraphs())
                {
                    if (paragraph && paragraph->PlainText().find("Structure survives") != std::string::npos)
                    {
                        foundBodyText = true;
                    }
                }
                CHECK(foundBodyText);
            }
        }

        SUBCASE("Excel and PowerPoint render to Markdown and plain text")
        {
            auto workbook = ExcelDocumentEditor::CreateNew();
            REQUIRE(workbook != nullptr);
            REQUIRE(workbook->FirstWorksheet()->SetCellText(1, 1, "Sheet cell"));
            const ScopedTemporaryFile xlsx("ExyokiOffice_convert", ".xlsx");
            REQUIRE(workbook->SaveToFile(xlsx.Path()));

            auto deck = PowerPointDocumentEditor::CreateNew();
            REQUIRE(deck != nullptr);
            REQUIRE(deck->AddSlide() != nullptr);
            const ScopedTemporaryFile pptx("ExyokiOffice_convert", ".pptx");
            REQUIRE(deck->SaveToFile(pptx.Path()));

            for (const auto& source : {xlsx.Path(), pptx.Path()})
            {
                CAPTURE(source.extension().string());
                const ScopedTemporaryFile markdown("ExyokiOffice_convert_out", ".md");
                const ScopedTemporaryFile plain("ExyokiOffice_convert_out", ".txt");
                CHECK(ConvertDocument(source, markdown.Path()).Ok);
                CHECK(ConvertDocument(source, plain.Path()).Ok);
            }
        }

        SUBCASE("Plain text converts back into Word only")
        {
            const ScopedTemporaryFile plain("ExyokiOffice_convert_source", ".txt");
            WriteTextFile(plain.Path(), "First line\nSecond line\n");

            const ScopedTemporaryFile intoWord("ExyokiOffice_convert_word", ".docx");
            const auto word = ConvertDocument(plain.Path(), intoWord.Path());
            REQUIRE(word.Ok);
            auto reopened = WordDocumentEditor::Open(intoWord.Path());
            REQUIRE(reopened != nullptr);
            CHECK(reopened->Paragraphs().size() >= 2);

            // The matrix states this is rejected rather than approximated.
            const ScopedTemporaryFile intoExcel("ExyokiOffice_convert_excel", ".xlsx");
            const auto excel = ConvertDocument(plain.Path(), intoExcel.Path());
            CHECK_FALSE(excel.Ok);
            CHECK_FALSE(excel.Diagnostics.empty());

            const ScopedTemporaryFile intoPowerPoint("ExyokiOffice_convert_ppt", ".pptx");
            const auto powerPoint = ConvertDocument(plain.Path(), intoPowerPoint.Path());
            CHECK_FALSE(powerPoint.Ok);
            CHECK_FALSE(powerPoint.Diagnostics.empty());
        }

        SUBCASE("Office to Office is a usage error, not a conversion")
        {
            const ScopedTemporaryFile other("ExyokiOffice_convert_other", ".xlsx");
            const auto result = ConvertDocument(docx.Path(), other.Path());
            CHECK_FALSE(result.Ok);
            CHECK_FALSE(result.UsageOk);
        }
    }

    TEST_CASE("The target Office version is a checking knob, not a save-as knob "
              "[compat] [office-versions]")
    {
        using ExyokiOffice::OpenXml::FileFormatVersions;

        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);
        REQUIRE(editor->AddParagraph("Version targeting") != nullptr);
        const auto bytes = editor->SaveToMemory();
        REQUIRE_FALSE(bytes.empty());

        SUBCASE("a lower target never rewrites the document")
        {
            // Opening with the most conservative markup-compatibility target and
            // saving again must not down-convert anything: the matrix promises
            // there is no "save as Office 2010" conversion.
            ExyokiOffice::Packaging::OpenSettings settings;
            settings.MarkupCompatibility.TargetFileFormatVersions = FileFormatVersions::Office2007;

            auto conservative = WordDocumentEditor::Open(bytes, settings);
            REQUIRE(conservative != nullptr);
            const auto resaved = conservative->SaveToMemory();
            REQUIRE_FALSE(resaved.empty());

            auto reopened = WordDocumentEditor::Open(resaved);
            REQUIRE(reopened != nullptr);
            REQUIRE_FALSE(reopened->Paragraphs().empty());
            CHECK(reopened->Paragraphs().front()->PlainText() == "Version targeting");
        }

        SUBCASE("every version the matrix lists is accepted as a validation target")
        {
            constexpr std::array targets{FileFormatVersions::Office2007, FileFormatVersions::Office2010,
                                         FileFormatVersions::Office2013, FileFormatVersions::Office2016,
                                         FileFormatVersions::Office2019, FileFormatVersions::Office2021,
                                         FileFormatVersions::Microsoft365};

            ExyokiOffice::OpenXmlPackage package;
            REQUIRE(package.LoadFromMemory(bytes));

            for (const auto target : targets)
            {
                CAPTURE(static_cast<int>(target));
                ExyokiOffice::OpenXmlDomValidationSettings settings;
                settings.TargetVersion = target;
                const auto result = ExyokiOffice::OpenXmlPackageValidator(settings).Validate(package);
                CHECK_FALSE(result.HasErrors());
            }
        }

        SUBCASE("the two defaults differ, as documented")
        {
            CHECK(ExyokiOffice::OpenXmlDomValidationSettings{}.TargetVersion == FileFormatVersions::Microsoft365);
            CHECK(ExyokiOffice::MarkupCompatibilityProcessSettings{}.TargetFileFormatVersions ==
                  FileFormatVersions::Office2007);
        }
    }

} // TEST_SUITE("FormatMatrixTests")
