// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

// Low-level tour: the same .docx is built straight out of the typed OpenXML DOM
// and the packaging layer, without the WordDocumentEditor facade. This is the
// escape hatch every high-level wrapper leaves open — package parts, typed
// elements, attributes, and the schema-aware child insertion that keeps the
// element order valid.
//
// The high-level equivalent is examples/ExampleWordEditor, which is what
// applications should normally use.
#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/Wordprocessing.hpp"
#include "ExyokiOffice/OpenXmlSimpleTypes.hpp"
#include "ExyokiOffice/Packaging/WordprocessingDocument.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <array>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>

namespace W = ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing;

using ExyokiOffice::EnumValue;
using ExyokiOffice::Int32Value;
using ExyokiOffice::OnOffValue;
using ExyokiOffice::StringValue;
using ExyokiOffice::UInt32Value;
using ExyokiOffice::Packaging::MainDocumentPart;
using ExyokiOffice::Packaging::WordDocument;
using ExyokiOffice::Packaging::WordprocessingDocumentType;

namespace ExampleWordLowLevel
{

// WordprocessingML measures pages in twentieths of a point (twips) and font
// sizes in half-points, so the raw numbers below are spelled out once here.
constexpr ExyokiOffice::UInt32 TwipsPerMillimeter = 56; // 1 mm = 1440/25.4 twips
constexpr ExyokiOffice::UInt32 A4WidthTwips = 11906;
constexpr ExyokiOffice::UInt32 A4HeightTwips = 16838;

ExyokiOffice::UInt32 Millimeters(ExyokiOffice::Real value)
{
    return static_cast<ExyokiOffice::UInt32>(value * TwipsPerMillimeter);
}

// Appends a paragraph carrying a single run of text, optionally styled.
W::Paragraph::Ptr AddParagraph(const W::Body::Ptr& body, std::string_view text,
                               std::string_view styleId = {},
                               W::JustificationValues::Value alignment = W::JustificationValues::Left)
{
    auto paragraph = body->AppendChild<W::Paragraph>();
    if (!paragraph)
    {
        return nullptr;
    }

    // AppendChild is schema-aware: w:pPr lands before the runs no matter in
    // which order the children are created.
    auto properties = paragraph->AppendChild<W::ParagraphProperties>();
    if (!styleId.empty())
    {
        auto style = properties->AppendChild<W::ParagraphStyleId>();
        style->SetVal(StringValue(std::string(styleId)));
    }
    auto justification = properties->AppendChild<W::Justification>();
    justification->SetVal(EnumValue<W::JustificationValues>(W::JustificationValues(alignment)));

    auto run = paragraph->AppendChild<W::Run>();
    auto textElement = run->AppendChild<W::Text>();
    textElement->SetText(text);
    return paragraph;
}

// Reads every w:t of the document back, which is all the text extraction a
// typed DOM needs for a document this simple.
std::string CollectText(const W::Document::Ptr& document)
{
    std::string result;
    if (!document)
    {
        return result;
    }
    for (const auto& text : document->Descendants<W::Text>())
    {
        result += text->GetText();
    }
    return result;
}

} // namespace ExampleWordLowLevel

int main()
{
    using namespace ExampleWordLowLevel;

    // --- Package: create the document and its main part. ---------------------
    WordDocument::Ptr document = WordDocument::Create(WordprocessingDocumentType::Document);
    if (!document || !document->InitDocument())
    {
        std::cerr << "Failed to create the WordDocument.\n";
        return 1;
    }

    document->SetTitle("ExyokiOffice Low-Level Example");
    document->SetCreator("ExyokiOffice Library");
    document->SetSubject("Typed OpenXML DOM demonstration");

    std::shared_ptr<MainDocumentPart> mainPart = document->GetMainDocumentPart();
    std::shared_ptr<W::Document> typedDocument = mainPart ? mainPart->GetDocument() : nullptr;
    if (!typedDocument)
    {
        std::cerr << "Failed to retrieve the main document part.\n";
        return 1;
    }

    // --- A style definitions part, created and populated by hand. ------------
    auto stylesPart = mainPart->GetStyleDefinitionsPart();
    if (!stylesPart)
    {
        stylesPart = mainPart->AddStyleDefinitionsPart();
    }
    auto styles = stylesPart ? stylesPart->GetStyles() : nullptr;
    if (!styles)
    {
        std::cerr << "Failed to create the style definitions part.\n";
        return 1;
    }

    auto headingStyle = styles->AppendChild<W::Style>();
    headingStyle->SetType(EnumValue<W::StyleValues>(W::StyleValues(W::StyleValues::Paragraph)));
    headingStyle->SetStyleId(StringValue("SampleHeading"));
    headingStyle->SetCustomStyle(OnOffValue(true));
    headingStyle->AppendChild<W::StyleName>()->SetVal(StringValue("Sample Heading"));
    headingStyle->AppendChild<W::BasedOn>()->SetVal(StringValue("Normal"));

    auto headingRunProperties = headingStyle->AppendChild<W::StyleRunProperties>();
    headingRunProperties->AppendChild<W::Bold>()->SetVal(OnOffValue(true));
    headingRunProperties->AppendChild<W::FontSize>()->SetVal(StringValue("32")); // half-points: 16 pt
    headingRunProperties->AppendChild<W::Color>()->SetVal(StringValue("1F4E79"));

    // --- Body content. --------------------------------------------------------
    auto body = typedDocument->AppendChild<W::Body>();
    AddParagraph(body, "Hello world!", "SampleHeading");

    constexpr std::array<std::string_view, 4> lines = {
        "This document was generated straight from the typed OpenXML DOM.",
        "Every element below is a checked C++ type generated from the schema metadata.",
        "AppendChild inserts each child at the position the content model requires.",
        "The high-level WordDocumentEditor is built on exactly these primitives.",
    };
    for (const auto line : lines)
    {
        AddParagraph(body, line, {}, W::JustificationValues::Both);
    }

    // --- A two-column table, assembled element by element. -------------------
    auto table = body->AppendChild<W::Table>();
    auto tableProperties = table->AppendChild<W::TableProperties>();
    auto tableWidth = tableProperties->AppendChild<W::TableWidth>();
    tableWidth->SetWidth(StringValue(std::to_string(Millimeters(160.0))));
    tableWidth->SetType(EnumValue<W::TableWidthUnitValues>(W::TableWidthUnitValues(W::TableWidthUnitValues::Dxa)));

    auto grid = table->AppendChild<W::TableGrid>();
    for (int column = 0; column < 2; ++column)
    {
        grid->AppendChild<W::GridColumn>()->SetWidth(StringValue(std::to_string(Millimeters(80.0))));
    }

    constexpr std::array<std::array<std::string_view, 2>, 3> cells = {{
        {"Layer", "Responsibility"},
        {"OPC package", "Parts, relationships, content types"},
        {"Typed DOM", "Elements, attributes, schema-aware insertion"},
    }};
    for (const auto& rowCells : cells)
    {
        auto row = table->AppendChild<W::TableRow>();
        for (const auto& cellText : rowCells)
        {
            auto cell = row->AppendChild<W::TableCell>();
            auto paragraph = cell->AppendChild<W::Paragraph>();
            auto run = paragraph->AppendChild<W::Run>();
            run->AppendChild<W::Text>()->SetText(cellText);
        }
    }

    // A paragraph after a table keeps Word from merging it with what follows.
    AddParagraph(body, "Saved as HelloWorld.docx.");

    // --- Section properties: A4 with 20 mm margins. --------------------------
    auto sectionProperties = body->AppendChild<W::SectionProperties>();
    auto pageSize = sectionProperties->AppendChild<W::PageSize>();
    pageSize->SetWidth(UInt32Value(A4WidthTwips));
    pageSize->SetHeight(UInt32Value(A4HeightTwips));
    pageSize->SetOrient(
        EnumValue<W::PageOrientationValues>(W::PageOrientationValues(W::PageOrientationValues::Portrait)));

    auto pageMargin = sectionProperties->AppendChild<W::PageMargin>();
    pageMargin->SetTop(Int32Value(static_cast<ExyokiOffice::Int32>(Millimeters(20.0))));
    pageMargin->SetBottom(Int32Value(static_cast<ExyokiOffice::Int32>(Millimeters(20.0))));
    pageMargin->SetLeft(UInt32Value(Millimeters(20.0)));
    pageMargin->SetRight(UInt32Value(Millimeters(20.0)));
    pageMargin->SetHeader(UInt32Value(Millimeters(12.5)));
    pageMargin->SetFooter(UInt32Value(Millimeters(12.5)));

    // --- Save, then re-open and read the document back through the DOM. ------
    const char* fileName = "HelloWorld.docx";
    if (!document->SaveToFile(fileName))
    {
        std::cerr << "Failed to write " << fileName << ".\n";
        return 1;
    }

    auto reopened = WordDocument::Open(fileName);
    auto reopenedPart = reopened ? reopened->GetMainDocumentPart() : nullptr;
    auto reopenedDocument = reopenedPart ? reopenedPart->GetDocument() : nullptr;
    if (!reopenedDocument)
    {
        std::cerr << "Failed to re-open " << fileName << ".\n";
        return 1;
    }

    const auto paragraphCount = reopenedDocument->Descendants<W::Paragraph>().size();
    const auto tableCount = reopenedDocument->Descendants<W::Table>().size();
    const auto reopenedStyles = reopenedPart->GetStyleDefinitionsPart();
    const auto styleCount = reopenedStyles && reopenedStyles->GetStyles()
                                ? reopenedStyles->GetStyles()->Elements<W::Style>().size()
                                : 0;

    std::cout << "Generated " << fileName << ": " << paragraphCount << " paragraphs, " << tableCount
              << " table(s), " << styleCount << " style definition(s), "
              << CollectText(reopenedDocument).size() << " characters of text.\n";
    return 0;
}
