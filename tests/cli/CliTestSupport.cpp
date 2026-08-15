// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "CliTestSupport.hpp"

#include "TestSupport.hpp"
#include "ImageFixtures.hpp"

#include "ExyokiOffice/Excel/ExcelDocument.hpp"
#include "ExyokiOffice/OpenXmlPackage.hpp"
#include "ExyokiOffice/PowerPoint/PowerPointDocument.hpp"
#include "ExyokiOffice/Word/WordDocument.hpp"

#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>

namespace ExyokiOfficeCliTests
{

using ExyokiOffice::Excel::ExcelDocumentEditor;
using ExyokiOffice::PowerPoint::PowerPointDocumentEditor;
using ExyokiOffice::Word::WordDocumentEditor;
using ExyokiOfficeTests::MakeTemporaryPath;

CapturedOutput::CapturedOutput()
    : m_previousOut(std::cout.rdbuf(m_out.rdbuf())), m_previousErr(std::cerr.rdbuf(m_err.rdbuf()))
{
}

CapturedOutput::~CapturedOutput()
{
    std::cout.rdbuf(m_previousOut);
    std::cerr.rdbuf(m_previousErr);
}

ParserFixture::ParserFixture()
    : m_dispatch(exyoki::BuildCommandLine(m_app, m_options, m_commands)), m_context{m_app, m_options}
{
}

void ParserFixture::Parse(std::vector<std::string> arguments)
{
    arguments.insert(arguments.begin(), "exyoki");
    std::vector<char*> argv;
    argv.reserve(arguments.size());
    for (auto& argument : arguments)
    {
        argv.push_back(argument.data());
    }
    m_app.parse(static_cast<int>(argv.size()), argv.data());
}

std::filesystem::path Fixture::WordDocument(std::string_view marker)
{
    auto editor = WordDocumentEditor::CreateNew();
    if (!editor)
    {
        throw std::runtime_error("could not create a Word document");
    }

    editor->AddParagraph(std::string(marker) + " beta gamma");
    editor->AddParagraph("Delta epsilon zeta");
    editor->AddParagraph("Eta theta iota");
    editor->AddParagraph("Kappa lambda mu");

    const auto path = MakeTemporaryPath("cli-word", ".docx");
    if (!editor->SaveToFile(path))
    {
        throw std::runtime_error("could not save the Word document");
    }

    return path;
}

std::filesystem::path Fixture::WordDocumentWithImage()
{
    auto editor = WordDocumentEditor::CreateNew();
    if (!editor || !editor->AddImageFromData(ExyokiOfficeTests::BuildPng(8, 8)))
    {
        throw std::runtime_error("could not create a Word document with an image");
    }

    const auto path = MakeTemporaryPath("cli-word-image", ".docx");
    if (!editor->SaveToFile(path))
    {
        throw std::runtime_error("could not save the Word document with an image");
    }
    return path;
}

std::filesystem::path Fixture::InvalidWordDocument()
{
    ExyokiOffice::OpenXmlPackage package;
    if (!package.LoadFromFile(WordDocument()))
    {
        throw std::runtime_error("could not load the Word document to invalidate");
    }

    const auto document = package.GetPartByUri("/word/document.xml");
    if (!document)
    {
        throw std::runtime_error("the Word document has no main part");
    }
    document->SetXmlString(
        R"(<w:document xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main"><w:body><w:p><w:bookmarkStart/></w:p><w:p><w:bookmarkStart/></w:p></w:body></w:document>)");

    const auto path = MakeTemporaryPath("cli-invalid-word", ".docx");
    if (!package.SaveToFile(path))
    {
        throw std::runtime_error("could not save the invalid Word document");
    }
    return path;
}

std::filesystem::path Fixture::WordTemplate()
{
    auto editor = WordDocumentEditor::CreateNew();
    auto paragraph = editor ? editor->AddParagraph("Dear ") : nullptr;
    if (!paragraph || !paragraph->AddField("MERGEFIELD FirstName", "First"))
    {
        throw std::runtime_error("could not create a Word mail-merge template");
    }

    const auto path = MakeTemporaryPath("cli-word-template", ".docx");
    if (!editor->SaveToFile(path))
    {
        throw std::runtime_error("could not save the Word mail-merge template");
    }
    return path;
}

std::filesystem::path Fixture::Workbook()
{
    auto editor = ExcelDocumentEditor::CreateNew();
    if (!editor)
    {
        throw std::runtime_error("could not create a workbook");
    }

    auto sheet = editor->FirstWorksheet();
    if (!sheet)
    {
        throw std::runtime_error("the new workbook has no worksheet");
    }

    editor->RenameWorksheet(0, "Data");
    sheet->SetCellText(1, 1, "Name");
    sheet->SetCellText(1, 2, "Value");
    sheet->SetCellText(2, 1, "Alpha");
    sheet->SetCellNumber(2, 2, 11.0);
    sheet->SetCellText(3, 1, "Beta");
    sheet->SetCellNumber(3, 2, 31.0);
    const auto formulaCell = ExyokiOffice::Excel::CellAddress::ParseA1("B4");
    if (!formulaCell || !sheet->SetCellFormula(*formulaCell, "=SUM(B2:B3)"))
    {
        throw std::runtime_error("could not add a formula to the workbook");
    }

    const auto path = MakeTemporaryPath("cli-excel", ".xlsx");
    if (!editor->SaveToFile(path))
    {
        throw std::runtime_error("could not save the workbook");
    }

    return path;
}

std::filesystem::path Fixture::TamperedSignedDocument()
{
    const auto signedPath = std::filesystem::path(EXYOKIOFFICE_SOURCE_DIR) / "corpus" / "word" /
                            "DigitalSignature.docx";
    ExyokiOffice::OpenXmlPackage package;
    if (!package.LoadFromFile(signedPath))
    {
        throw std::runtime_error("could not load the signed corpus document");
    }

    const auto document = package.GetPartByUri("/word/document.xml");
    if (!document)
    {
        throw std::runtime_error("the signed corpus document has no main part");
    }
    document->SetXmlString(document->GetXmlString() + " ");
    package.SetSignatureSavePolicy(ExyokiOffice::SignatureSavePolicy::Ignore);

    const auto path = MakeTemporaryPath("cli-tampered-signature", ".docx");
    if (!package.SaveToFile(path))
    {
        throw std::runtime_error("could not save the tampered signed document");
    }
    return path;
}

std::filesystem::path Fixture::Presentation()
{
    auto editor = PowerPointDocumentEditor::CreateNew();
    if (!editor)
    {
        throw std::runtime_error("could not create a presentation");
    }

    // Two slides, so that a split by slide count has something to split.
    if (!editor->CreateSlideBuilder().SetTitle("Alpha slide").Build() ||
        !editor->CreateSlideBuilder().SetTitle("Beta slide").Build())
    {
        throw std::runtime_error("could not add a slide");
    }

    const auto path = MakeTemporaryPath("cli-ppt", ".pptx");
    if (!editor->SaveToFile(path))
    {
        throw std::runtime_error("could not save the presentation");
    }

    return path;
}

std::filesystem::path Fixture::EmptyDirectory()
{
    const auto path = MakeTemporaryPath("cli-dir", "");
    std::filesystem::create_directories(path);
    return path;
}

std::filesystem::path Fixture::UnusedPath(std::string_view extension)
{
    return MakeTemporaryPath("cli-out", extension);
}

std::filesystem::path Fixture::TextFile(std::string_view contents, std::string_view extension)
{
    const auto path = MakeTemporaryPath("cli-text", extension);
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file << contents;
    return path;
}

std::string Fixture::ReadText(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

CommandLineResult RunCommandLine(std::vector<std::string> arguments)
{
    arguments.insert(arguments.begin(), "exyoki");

    std::vector<char*> argv;
    argv.reserve(arguments.size());
    for (auto& argument : arguments)
    {
        argv.push_back(argument.data());
    }

    CommandLineResult result;
    {
        const CapturedOutput captured;
        result.Code = exyoki::RunCommandLine(static_cast<int>(argv.size()), argv.data());
        result.Out = captured.Out();
        result.Err = captured.Err();
    }

    return result;
}

} // namespace ExyokiOfficeCliTests
