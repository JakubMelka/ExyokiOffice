// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

// ---------------------------------------------------------------------------
// Regression suite for context-sensitive resolution of ambiguous element names.
//
// Around 190 Open XML element names are declared by more than one generated
// class - `x:r` is a pivot cache record (CT_Record) under `x:pivotCacheRecords`
// but a shared-string rich text run (CT_RElt) under `x:si`; `w:rPr` is run
// properties in a run (CT_RPr), style run properties in a style (CT_RPrStyle),
// and paragraph-mark run properties in a pPr (CT_ParaRPr); `x:i` and `x:b`
// even share one schema type (CT_BooleanProperty) on top of that. The element
// name alone therefore cannot identify the class.
//
// The machinery under test:
//  - the generator resolves each `MetadataElementParticle::ElementType()` by
//    (schema type, element name) whenever the element name is ambiguous;
//  - `OpenXmlElementFactory::IsAmbiguousElementName()` reports such names and
//    `ResolveClassByTypeName()` resolves the recorded class;
//  - `OpenXMLElement::ChildrenInContentModel()` types children from the
//    parent's particle metadata;
//  - `OpenXmlDomValidator` walks the tree through it.
//
// Ordinary editing traversal (`Children()`, typed accessors) intentionally
// keeps the historical name-based behavior; a test below pins that too.
// ---------------------------------------------------------------------------

#include "doctest.h"

#include "ExyokiOffice/MetadataBuilder.hpp"
#include "ExyokiOffice/OpenXmlDomValidator.hpp"
#include "ExyokiOffice/OpenXmlPackage.hpp"
#include "ExyokiOffice/OpenXmlPackagePart.hpp"
#include "ExyokiOffice/OpenXMLElement.hpp"
#include "ExyokiOffice/DOM/OpenXmlElementFactory.hpp"
#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/Drawing.hpp"
#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/Drawing/Charts.hpp"
#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/Drawing/Diagrams.hpp"
#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/Presentation.hpp"
#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/Spreadsheet.hpp"
#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/Wordprocessing.hpp"
#include "zip/zip.h"
#include "ExyokiOffice/StandardTypes.hpp"

#include <algorithm>
#include <cstdlib>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace DomContextResolutionTestHelpers
{

constexpr std::string_view kSpreadsheetNs = "http://schemas.openxmlformats.org/spreadsheetml/2006/main";
constexpr std::string_view kWordNs = "http://schemas.openxmlformats.org/wordprocessingml/2006/main";

void AddZipEntry(zip_t* archive, const char* name, std::string_view content)
{
    REQUIRE(zip_entry_open(archive, name) == 0);
    CHECK(zip_entry_write(archive, content.data(), content.size()) == 0);
    zip_entry_close(archive);
}

std::vector<ExyokiOffice::Byte> FinishZip(zip_t* archive)
{
    void* rawBuffer = nullptr;
    ExyokiOffice::Size rawSize = 0;
    REQUIRE(zip_stream_copy(archive, &rawBuffer, &rawSize) > 0);
    zip_stream_close(archive);
    REQUIRE(rawBuffer != nullptr);

    const auto* bytes = static_cast<const ExyokiOffice::UInt8*>(rawBuffer);
    std::vector<ExyokiOffice::Byte> result(bytes, bytes + rawSize);
    std::free(rawBuffer);
    return result;
}

std::vector<ExyokiOffice::Byte> BuildSingleXmlPartPackage(std::string_view xml)
{
    auto* archive = zip_stream_open(nullptr, 0, ZIP_DEFAULT_COMPRESSION_LEVEL, 'w');
    REQUIRE(archive != nullptr);

    AddZipEntry(archive,
                "[Content_Types].xml",
                R"(<?xml version="1.0" encoding="UTF-8"?>
<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">
  <Default Extension="xml" ContentType="application/xml"/>
</Types>)");
    AddZipEntry(archive, "custom.xml", xml);
    return FinishZip(archive);
}

std::shared_ptr<ExyokiOffice::OpenXMLElement> LoadRootElement(std::string_view xml)
{
    static std::vector<std::shared_ptr<ExyokiOffice::OpenXmlPackage>> packages;

    auto package = std::make_shared<ExyokiOffice::OpenXmlPackage>();
    REQUIRE(package->LoadFromMemory(BuildSingleXmlPartPackage(xml)));

    auto part = package->GetPartByUri("/custom.xml");
    REQUIRE(part != nullptr);

    auto root = part->GetRootElement();
    REQUIRE(root != nullptr);
    packages.push_back(std::move(package));
    return root;
}

bool HasValidationIssue(const ExyokiOffice::ValidationResult& result,
                        ExyokiOffice::ValidationErrorId id)
{
    return std::any_of(result.Issues().begin(), result.Issues().end(),
                       [id](const auto& issue)
                       { return issue.Id == id; });
}

/**
 * @brief Returns the class name a content model expects for a child element.
 *
 * Walks the particle tree of @p elementClass looking for the element particle
 * that declares @p name, and returns its recorded C++ class name.
 */
std::string ParticleElementType(const ExyokiOffice::OpenXMLElementClass* elementClass,
                                const ExyokiOffice::OpenXmlQualifiedName& name)
{
    const auto metadata = elementClass ? elementClass->GetMetadata() : nullptr;
    if (!metadata)
    {
        return {};
    }

    const std::function<std::string(const ExyokiOffice::MetadataParticlePtr&)> search =
        [&](const ExyokiOffice::MetadataParticlePtr& particle) -> std::string
    {
        if (!particle)
        {
            return {};
        }
        if (particle->Kind() == ExyokiOffice::MetadataParticleKind::Element)
        {
            const auto& element = static_cast<const ExyokiOffice::MetadataElementParticle&>(*particle);
            return element.Element() == name ? element.ElementType() : std::string{};
        }
        if (particle->Kind() == ExyokiOffice::MetadataParticleKind::Any)
        {
            return {};
        }
        const auto& composite = static_cast<const ExyokiOffice::MetadataCompositeParticle&>(*particle);
        for (const auto& child : composite.Children())
        {
            if (auto found = search(child); !found.empty())
            {
                return found;
            }
        }
        return {};
    };
    return search(metadata->ParticleTree());
}

/** Everything the schema corpus walk collects about ambiguous element names. */
struct CorpusResult
{
    /** "namespace|localName" of an ambiguous element -> every recorded class. */
    std::map<std::string, std::set<std::string>> classesByAmbiguousName;
    /** Human-readable invariant violations; must stay empty. */
    std::vector<std::string> violations;
    ExyokiOffice::Size elementParticleCount = 0;
    ExyokiOffice::Size visitedClassCount = 0;
};

/**
 * @brief Walks every content model transitively reachable from the major part
 * roots and checks the ambiguity invariants on each element particle.
 *
 * The walk resolves each particle's recorded `ElementType()` through the
 * factory and recurses into the resolved class's own content model, so it
 * covers Word, Excel, PowerPoint, DrawingML, and chart markup without listing
 * classes one by one. New schema releases extend the corpus automatically.
 */
CorpusResult WalkSchemaCorpus()
{
    namespace S = ExyokiOffice::DocumentFormat::OpenXml::Spreadsheet;
    namespace W = ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing;
    namespace P = ExyokiOffice::DocumentFormat::OpenXml::Presentation;
    namespace A = ExyokiOffice::DocumentFormat::OpenXml::Drawing;
    namespace C = ExyokiOffice::DocumentFormat::OpenXml::Drawing::Charts;
    using ExyokiOffice::Generated::OpenXmlElementFactory;

    namespace D = ExyokiOffice::DocumentFormat::OpenXml::Drawing::Diagrams;

    CorpusResult result;
    std::vector<const ExyokiOffice::OpenXMLElementClass*> pending = {
        // SpreadsheetML parts.
        S::Workbook::StaticMetaClass(),
        S::Worksheet::StaticMetaClass(),
        S::Chartsheet::StaticMetaClass(),
        S::SharedStringTable::StaticMetaClass(),
        S::Stylesheet::StaticMetaClass(),
        S::PivotCacheDefinition::StaticMetaClass(),
        S::PivotCacheRecords::StaticMetaClass(),
        S::PivotTableDefinition::StaticMetaClass(),
        S::Table::StaticMetaClass(),
        S::ExternalLink::StaticMetaClass(),
        S::Comments::StaticMetaClass(),
        S::Connections::StaticMetaClass(),
        S::CalculationChain::StaticMetaClass(),
        S::MapInfo::StaticMetaClass(),
        S::Metadata::StaticMetaClass(),
        S::QueryTable::StaticMetaClass(),
        S::VolatileTypes::StaticMetaClass(),
        S::SingleXmlCells::StaticMetaClass(),
        // WordprocessingML parts.
        W::Document::StaticMetaClass(),
        W::Styles::StaticMetaClass(),
        W::Numbering::StaticMetaClass(),
        W::Footnotes::StaticMetaClass(),
        W::Endnotes::StaticMetaClass(),
        W::Comments::StaticMetaClass(),
        W::Settings::StaticMetaClass(),
        W::Fonts::StaticMetaClass(),
        W::WebSettings::StaticMetaClass(),
        W::GlossaryDocument::StaticMetaClass(),
        W::Recipients::StaticMetaClass(),
        // PresentationML parts.
        P::Presentation::StaticMetaClass(),
        P::Slide::StaticMetaClass(),
        P::SlideMaster::StaticMetaClass(),
        P::SlideLayout::StaticMetaClass(),
        P::NotesSlide::StaticMetaClass(),
        P::NotesMaster::StaticMetaClass(),
        P::HandoutMaster::StaticMetaClass(),
        P::CommentList::StaticMetaClass(),
        P::CommentAuthorList::StaticMetaClass(),
        P::ViewProperties::StaticMetaClass(),
        P::PresentationProperties::StaticMetaClass(),
        // DrawingML, diagram, and chart parts.
        A::Theme::StaticMetaClass(),
        A::TableStyleList::StaticMetaClass(),
        C::ChartSpace::StaticMetaClass(),
        C::UserShapes::StaticMetaClass(),
        D::DataModelRoot::StaticMetaClass(),
        D::ColorsDefinition::StaticMetaClass(),
        D::StyleDefinition::StaticMetaClass(),
        D::LayoutDefinition::StaticMetaClass(),
    };
    std::set<const ExyokiOffice::OpenXMLElementClass*> visited;

    const std::function<void(const ExyokiOffice::MetadataParticlePtr&)> walkParticle =
        [&](const ExyokiOffice::MetadataParticlePtr& particle)
    {
        if (!particle)
        {
            return;
        }
        if (particle->Kind() == ExyokiOffice::MetadataParticleKind::Element)
        {
            const auto& element = static_cast<const ExyokiOffice::MetadataElementParticle&>(*particle);
            ++result.elementParticleCount;
            const auto name = element.Element();
            const auto* resolved = OpenXmlElementFactory::ResolveClassByTypeName(element.ElementType());
            if (OpenXmlElementFactory::IsAmbiguousElementName(name))
            {
                auto key = std::string(name.namespaceUri()) + "|" + std::string(name.localName());
                result.classesByAmbiguousName[key].insert(element.ElementType());
                if (resolved == nullptr)
                {
                    result.violations.push_back(key + " -> '" + element.ElementType() +
                                                "' is not resolvable through the factory");
                }
                else if (!(resolved->QualifiedName() == name))
                {
                    result.violations.push_back(
                        key + " -> '" + element.ElementType() + "' serializes as '" +
                        std::string(resolved->QualifiedName().localName()) +
                        "', not as the particle's element");
                }
            }
            if (resolved != nullptr)
            {
                pending.push_back(resolved);
            }
            return;
        }
        if (particle->Kind() == ExyokiOffice::MetadataParticleKind::Any)
        {
            return;
        }
        const auto& composite = static_cast<const ExyokiOffice::MetadataCompositeParticle&>(*particle);
        for (const auto& child : composite.Children())
        {
            walkParticle(child);
        }
    };

    while (!pending.empty())
    {
        const auto* elementClass = pending.back();
        pending.pop_back();
        if (elementClass == nullptr || !visited.insert(elementClass).second)
        {
            continue;
        }
        ++result.visitedClassCount;
        if (const auto metadata = elementClass->GetMetadata())
        {
            walkParticle(metadata->ParticleTree());
        }
    }
    return result;
}

} // namespace DomContextResolutionTestHelpers

using namespace DomContextResolutionTestHelpers;

TEST_SUITE("DomContextResolutionTests")
{

    TEST_CASE("ambiguous element names are reported by the element factory [unit] [metadata] [dom-resolution]")
    {
        namespace S = ExyokiOffice::DocumentFormat::OpenXml::Spreadsheet;
        namespace W = ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing;
        using ExyokiOffice::Generated::OpenXmlElementFactory;

        const ExyokiOffice::OpenXmlQualifiedName spreadsheetR(kSpreadsheetNs, "r");
        const ExyokiOffice::OpenXmlQualifiedName spreadsheetX(kSpreadsheetNs, "x");
        const ExyokiOffice::OpenXmlQualifiedName sheetData(kSpreadsheetNs, "sheetData");
        const ExyokiOffice::OpenXmlQualifiedName worksheet(kSpreadsheetNs, "worksheet");
        const ExyokiOffice::OpenXmlQualifiedName runProperties(kWordNs, "rPr");
        const ExyokiOffice::OpenXmlQualifiedName paragraph(kWordNs, "p");

        CHECK(OpenXmlElementFactory::IsAmbiguousElementName(spreadsheetR));
        CHECK(OpenXmlElementFactory::IsAmbiguousElementName(spreadsheetX));
        CHECK(OpenXmlElementFactory::IsAmbiguousElementName(sheetData));
        CHECK(OpenXmlElementFactory::IsAmbiguousElementName(runProperties));
        CHECK_FALSE(OpenXmlElementFactory::IsAmbiguousElementName(worksheet));
        CHECK_FALSE(OpenXmlElementFactory::IsAmbiguousElementName(paragraph));

        // The class-name lookup that particle metadata is resolved through.
        CHECK(OpenXmlElementFactory::ResolveClassByTypeName(
                  "ExyokiOffice::DocumentFormat::OpenXml::Spreadsheet::PivotCacheRecord") ==
              S::PivotCacheRecord::StaticMetaClass());
        CHECK(OpenXmlElementFactory::ResolveClassByTypeName(
                  "ExyokiOffice::DocumentFormat::OpenXml::Spreadsheet::Run") == S::Run::StaticMetaClass());
        CHECK(OpenXmlElementFactory::ResolveClassByTypeName(
                  "ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::StyleRunProperties") ==
              W::StyleRunProperties::StaticMetaClass());
        CHECK(OpenXmlElementFactory::ResolveClassByTypeName("ExyokiOffice::NoSuchClass") == nullptr);
    }

    TEST_CASE("particle metadata records the schema type of ambiguous children [unit] [metadata] [dom-resolution]")
    {
        namespace S = ExyokiOffice::DocumentFormat::OpenXml::Spreadsheet;
        namespace W = ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing;

        // The generator resolves ambiguous element names by schema type, so the two
        // content models that both declare `x:r` name different classes.
        CHECK(ParticleElementType(S::PivotCacheRecords::StaticMetaClass(),
                                  ExyokiOffice::OpenXmlQualifiedName(kSpreadsheetNs, "r")) ==
              "ExyokiOffice::DocumentFormat::OpenXml::Spreadsheet::PivotCacheRecord");
        CHECK(ParticleElementType(S::SharedStringItem::StaticMetaClass(),
                                  ExyokiOffice::OpenXmlQualifiedName(kSpreadsheetNs, "r")) ==
              "ExyokiOffice::DocumentFormat::OpenXml::Spreadsheet::Run");

        // Same for `x:x`: a field item inside a record, a shared-item index inside
        // a row/column item.
        CHECK(ParticleElementType(S::PivotCacheRecord::StaticMetaClass(),
                                  ExyokiOffice::OpenXmlQualifiedName(kSpreadsheetNs, "x")) ==
              "ExyokiOffice::DocumentFormat::OpenXml::Spreadsheet::FieldItem");
        CHECK(ParticleElementType(S::RowItem::StaticMetaClass(),
                                  ExyokiOffice::OpenXmlQualifiedName(kSpreadsheetNs, "x")) ==
              "ExyokiOffice::DocumentFormat::OpenXml::Spreadsheet::MemberPropertyIndex");

        // `x:i` and `x:b` share one schema type (CT_BooleanProperty), so type-only
        // resolution cannot tell them apart; the compound (type, element) key must.
        CHECK(ParticleElementType(S::Font::StaticMetaClass(),
                                  ExyokiOffice::OpenXmlQualifiedName(kSpreadsheetNs, "i")) ==
              "ExyokiOffice::DocumentFormat::OpenXml::Spreadsheet::Italic");
        CHECK(ParticleElementType(S::Font::StaticMetaClass(),
                                  ExyokiOffice::OpenXmlQualifiedName(kSpreadsheetNs, "b")) ==
              "ExyokiOffice::DocumentFormat::OpenXml::Spreadsheet::Bold");

        // The `w:rPr` family fans out into distinct schema types per context.
        CHECK(ParticleElementType(W::Run::StaticMetaClass(),
                                  ExyokiOffice::OpenXmlQualifiedName(kWordNs, "rPr")) ==
              "ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::RunProperties");
        CHECK(ParticleElementType(W::Style::StaticMetaClass(),
                                  ExyokiOffice::OpenXmlQualifiedName(kWordNs, "rPr")) ==
              "ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::StyleRunProperties");
        CHECK(ParticleElementType(W::ParagraphProperties::StaticMetaClass(),
                                  ExyokiOffice::OpenXmlQualifiedName(kWordNs, "rPr")) ==
              "ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::ParagraphMarkRunProperties");
        CHECK(ParticleElementType(W::Style::StaticMetaClass(),
                                  ExyokiOffice::OpenXmlQualifiedName(kWordNs, "pPr")) ==
              "ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::StyleParagraphProperties");
    }

    TEST_CASE("every reachable ambiguous particle resolves to a class of the right element "
              "[unit] [metadata] [dom-resolution]")
    {
        const auto corpus = WalkSchemaCorpus();

        // Sanity: the walk really covered the schema, not a couple of stubs.
        CHECK(corpus.visitedClassCount > 500);
        CHECK(corpus.elementParticleCount > 2000);

        // Every ambiguous-name particle must resolve through the factory to a
        // class that serializes as that very element. This sweeps all ~190
        // ambiguous names in every reachable context - no hand-picked list.
        std::string report;
        for (const auto& violation : corpus.violations)
        {
            report += violation + "; ";
        }
        INFO("violations: ", report);
        CHECK(corpus.violations.empty());
    }

    TEST_CASE("ambiguous names resolve to different classes in different contexts "
              "[unit] [metadata] [dom-resolution]")
    {
        const auto corpus = WalkSchemaCorpus();

        // The canary against regressing to name-first resolution: if the generator
        // ever resolves these by element name again, every context collapses to a
        // single class and this test fails. Each listed name is known to occur with
        // at least two distinct classes in the walked corpus.
        const auto expectDistinct = [&](std::string_view ns, std::string_view local)
        {
            const auto key = std::string(ns) + "|" + std::string(local);
            const auto found = corpus.classesByAmbiguousName.find(key);
            REQUIRE_MESSAGE(found != corpus.classesByAmbiguousName.end(), key, " not reached by the walk");
            std::string classes;
            for (const auto& name : found->second)
            {
                classes += name + "; ";
            }
            INFO(key, " -> ", classes);
            CHECK(found->second.size() >= 2);
        };

        expectDistinct(kSpreadsheetNs, "r");           // PivotCacheRecord / Run
        expectDistinct(kSpreadsheetNs, "x");           // FieldItem / MemberPropertyIndex
        expectDistinct(kSpreadsheetNs, "i");           // Italic / RowItem
        expectDistinct(kSpreadsheetNs, "b");           // Bold / BooleanItem
        expectDistinct(kSpreadsheetNs, "sheetData");   // SheetData / ExternalSheetData
        expectDistinct(kSpreadsheetNs, "row");         // Row / ExternalRow
        expectDistinct(kSpreadsheetNs, "definedName"); // DefinedName / ExternalDefinedName
        expectDistinct(kSpreadsheetNs, "ext");         // WorkbookExtension / TableExtension / ...
        expectDistinct(kSpreadsheetNs, "extLst");      // one extension list class per host
        expectDistinct(kSpreadsheetNs, "filter");      // Filter / PivotFilter
        expectDistinct(kWordNs, "rPr");                // Run / Style / ParagraphMark / ... properties
        expectDistinct(kWordNs, "pPr");                // Paragraph / StyleParagraph / ... properties
        expectDistinct(kWordNs, "tblPr");              // TableProperties / StyleTableProperties / ...
        expectDistinct(kWordNs, "tcPr");               // TableCellProperties / StyleTableCellProperties / ...
        expectDistinct(kWordNs, "sdt");                // SdtBlock / SdtRun / SdtRow / SdtCell
    }

    TEST_CASE("ChildrenInContentModel types ambiguous children from the parent [unit] [metadata] [dom-resolution]")
    {
        auto records = LoadRootElement(
            R"(<?xml version="1.0" encoding="UTF-8"?>)"
            R"(<x:pivotCacheRecords xmlns:x="http://schemas.openxmlformats.org/spreadsheetml/2006/main" count="1">)"
            R"(<x:r><x:x v="0"/><x:n v="10"/></x:r></x:pivotCacheRecords>)");
        auto sharedStrings = LoadRootElement(
            R"(<?xml version="1.0" encoding="UTF-8"?>)"
            R"(<x:sst xmlns:x="http://schemas.openxmlformats.org/spreadsheetml/2006/main" count="1" uniqueCount="1">)"
            R"(<x:si><x:r><x:t>Hello</x:t></x:r></x:si></x:sst>)");

        const auto recordChildren = records->ChildrenInContentModel();
        REQUIRE(recordChildren.size() == 1);
        CHECK(recordChildren.front()->QualifiedName().localName() == "r");
        CHECK(recordChildren.front()->TypeQualifiedName().localName() == "CT_Record");

        const auto stringItems = sharedStrings->ChildrenInContentModel();
        REQUIRE(stringItems.size() == 1);
        const auto runs = stringItems.front()->ChildrenInContentModel();
        REQUIRE(runs.size() == 1);
        CHECK(runs.front()->QualifiedName().localName() == "r");
        CHECK(runs.front()->TypeQualifiedName().localName() == "CT_RElt");

        // Nested ambiguity: `x:x` is a field item inside a record.
        const auto recordItems = recordChildren.front()->ChildrenInContentModel();
        REQUIRE(recordItems.size() == 2);
        CHECK(recordItems.front()->TypeQualifiedName().localName() == "CT_Index");

        // Children() keeps the name-based resolution the editing APIs rely on, so
        // both contexts report the one class the registry maps `x:r` to.
        CHECK(records->Children().front()->TypeQualifiedName() ==
              sharedStrings->Children().front()->Children().front()->TypeQualifiedName());
    }

    TEST_CASE("worksheet and external-link sheetData resolve to their own types [unit] [metadata] [dom-resolution]")
    {
        auto worksheet = LoadRootElement(
            R"(<?xml version="1.0" encoding="UTF-8"?>)"
            R"(<x:worksheet xmlns:x="http://schemas.openxmlformats.org/spreadsheetml/2006/main">)"
            R"(<x:sheetData><x:row r="1"><x:c r="A1"><x:v>1</x:v></x:c></x:row></x:sheetData></x:worksheet>)");
        auto externalLink = LoadRootElement(
            R"(<?xml version="1.0" encoding="UTF-8"?>)"
            R"(<x:externalLink xmlns:x="http://schemas.openxmlformats.org/spreadsheetml/2006/main")"
            R"( xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships">)"
            R"(<x:externalBook r:id="rId1"><x:sheetNames><x:sheetName val="Data"/></x:sheetNames>)"
            R"(<x:sheetDataSet><x:sheetData sheetId="0"/></x:sheetDataSet></x:externalBook></x:externalLink>)");

        const auto worksheetChildren = worksheet->ChildrenInContentModel();
        REQUIRE(worksheetChildren.size() == 1);
        CHECK(worksheetChildren.front()->TypeQualifiedName().localName() == "CT_SheetData");

        auto book = externalLink->ChildrenInContentModel().front();
        const auto bookChildren = book->ChildrenInContentModel();
        REQUIRE(bookChildren.size() == 2);
        const auto dataSet = bookChildren.back();
        const auto sheetData = dataSet->ChildrenInContentModel();
        REQUIRE(sheetData.size() == 1);
        CHECK(sheetData.front()->TypeQualifiedName().localName() == "CT_ExternalSheetData");
    }

    TEST_CASE("shared-schema-type siblings keep their own identity [unit] [metadata] [dom-resolution]")
    {
        // `x:b` and `x:i` both carry the schema type CT_BooleanProperty. A type-only
        // lookup returns the type's first registrant for both, so a regression here
        // shows up as an <i> child typed as Bold.
        auto stylesheet = LoadRootElement(
            R"(<?xml version="1.0" encoding="UTF-8"?>)"
            R"(<x:styleSheet xmlns:x="http://schemas.openxmlformats.org/spreadsheetml/2006/main">)"
            R"(<x:fonts count="1"><x:font><x:b/><x:i/></x:font></x:fonts></x:styleSheet>)");

        const auto fonts = stylesheet->ChildrenInContentModel().front();
        const auto font = fonts->ChildrenInContentModel().front();
        const auto properties = font->ChildrenInContentModel();
        REQUIRE(properties.size() == 2);
        CHECK(properties[0]->QualifiedName().localName() == "b");
        CHECK(properties[1]->QualifiedName().localName() == "i");
        CHECK(properties[0]->TypeQualifiedName().localName() == "CT_BooleanProperty");
        CHECK(properties[1]->TypeQualifiedName().localName() == "CT_BooleanProperty");
    }

    TEST_CASE("Word run, paragraph-mark, and style properties resolve per context [unit] [metadata] [dom-resolution]")
    {
        auto document = LoadRootElement(
            R"(<?xml version="1.0" encoding="UTF-8"?>)"
            R"(<w:document xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main">)"
            R"(<w:body><w:p><w:pPr><w:rPr><w:b/></w:rPr></w:pPr>)"
            R"(<w:r><w:rPr><w:b/></w:rPr><w:t>Hello</w:t></w:r></w:p></w:body></w:document>)");
        auto styles = LoadRootElement(
            R"(<?xml version="1.0" encoding="UTF-8"?>)"
            R"(<w:styles xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main">)"
            R"(<w:style w:type="character" w:styleId="C1"><w:name w:val="Custom"/>)"
            R"(<w:rPr><w:b/></w:rPr></w:style></w:styles>)");

        const auto body = document->ChildrenInContentModel().front();
        const auto paragraph = body->ChildrenInContentModel().front();
        const auto paragraphChildren = paragraph->ChildrenInContentModel();
        REQUIRE(paragraphChildren.size() == 2);

        // w:pPr in a paragraph, and w:rPr nested inside it (paragraph mark).
        CHECK(paragraphChildren[0]->TypeQualifiedName().localName() == "CT_PPr");
        const auto paragraphMark = paragraphChildren[0]->ChildrenInContentModel();
        REQUIRE(paragraphMark.size() == 1);
        CHECK(paragraphMark.front()->TypeQualifiedName().localName() == "CT_ParaRPr");

        // w:rPr in a run.
        const auto runChildren = paragraphChildren[1]->ChildrenInContentModel();
        REQUIRE(runChildren.size() == 2);
        CHECK(runChildren.front()->TypeQualifiedName().localName() == "CT_RPr");

        // w:rPr in a style.
        const auto style = styles->ChildrenInContentModel().front();
        const auto styleChildren = style->ChildrenInContentModel();
        REQUIRE(styleChildren.size() == 2);
        CHECK(styleChildren.back()->TypeQualifiedName().localName() == "CT_RPrStyle");
    }

    TEST_CASE("typed accessors keep their name-based contract for ambiguous names [unit] [metadata] [dom-resolution]")
    {
        namespace S = ExyokiOffice::DocumentFormat::OpenXml::Spreadsheet;
        namespace W = ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing;

        auto records = LoadRootElement(
            R"(<?xml version="1.0" encoding="UTF-8"?>)"
            R"(<x:pivotCacheRecords xmlns:x="http://schemas.openxmlformats.org/spreadsheetml/2006/main" count="1">)"
            R"(<x:r><x:x v="0"/><x:n v="10"/></x:r></x:pivotCacheRecords>)");
        auto sharedStrings = LoadRootElement(
            R"(<?xml version="1.0" encoding="UTF-8"?>)"
            R"(<x:sst xmlns:x="http://schemas.openxmlformats.org/spreadsheetml/2006/main" count="1" uniqueCount="1">)"
            R"(<x:si><x:r><x:t>Hello</x:t></x:r></x:si></x:sst>)");

        // The preferred-class path: naming the wanted type always works, including
        // context-specific classes whose element name is ambiguous.
        CHECK(records->Elements<S::PivotCacheRecord>().size() == 1);
        const auto item = sharedStrings->GetFirstChildOfType<S::SharedStringItem>();
        REQUIRE(item);
        CHECK(item->GetFirstChildOfType<S::Run>() != nullptr);

        // Historic contract relied on by hand-written editing code: typed accessors
        // match by element name, so the other context's class matches as well. This
        // is intentional - context-sensitive typing is opt-in through
        // ChildrenInContentModel(), and this pin keeps editing behavior from being
        // changed by accident (routing Children() through the content model once
        // broke 88 editing tests).
        CHECK(records->GetFirstChildOfType<S::Run>() != nullptr);
        CHECK(item->GetFirstChildOfType<S::PivotCacheRecord>() != nullptr);

        // The same contract in Word: a style's rPr reads back both as the
        // context-specific class and as the run-level class.
        auto styles = LoadRootElement(
            R"(<?xml version="1.0" encoding="UTF-8"?>)"
            R"(<w:styles xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main">)"
            R"(<w:style w:type="character" w:styleId="C1"><w:name w:val="Custom"/>)"
            R"(<w:rPr><w:b/></w:rPr></w:style></w:styles>)");
        const auto style = styles->GetFirstChildOfType<W::Style>();
        REQUIRE(style);
        CHECK(style->GetFirstChildOfType<W::StyleRunProperties>() != nullptr);
        CHECK(style->GetFirstChildOfType<W::RunProperties>() != nullptr);
    }

    TEST_CASE("DOM validation resolves ambiguous elements by content model [unit] [metadata] [dom-resolution]")
    {
        const ExyokiOffice::OpenXmlDomValidator validator;

        // Pivot cache records validate as records, not as rich text runs.
        auto records = LoadRootElement(
            R"(<?xml version="1.0" encoding="UTF-8"?>)"
            R"(<x:pivotCacheRecords xmlns:x="http://schemas.openxmlformats.org/spreadsheetml/2006/main" count="2">)"
            R"(<x:r><x:x v="0"/><x:n v="10"/></x:r>)"
            R"(<x:r><x:x v="1"/><x:s v="text"/></x:r></x:pivotCacheRecords>)");
        CHECK(validator.Validate(*records).IsValid());

        // Shared-string rich text runs validate as runs in their own context.
        auto sharedStrings = LoadRootElement(
            R"(<?xml version="1.0" encoding="UTF-8"?>)"
            R"(<x:sst xmlns:x="http://schemas.openxmlformats.org/spreadsheetml/2006/main" count="1" uniqueCount="1">)"
            R"(<x:si><x:r><x:t>Hello</x:t></x:r></x:si></x:sst>)");
        CHECK(validator.Validate(*sharedStrings).IsValid());

        // Word markup exercises the largest ambiguous family (rPr/pPr/tblPr...).
        auto document = LoadRootElement(
            R"(<?xml version="1.0" encoding="UTF-8"?>)"
            R"(<w:document xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main">)"
            R"(<w:body><w:p><w:pPr><w:rPr><w:b/></w:rPr></w:pPr>)"
            R"(<w:r><w:rPr><w:b/></w:rPr><w:t>Hello</w:t></w:r></w:p></w:body></w:document>)");
        CHECK(validator.Validate(*document).IsValid());

        auto styles = LoadRootElement(
            R"(<?xml version="1.0" encoding="UTF-8"?>)"
            R"(<w:styles xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main">)"
            R"(<w:style w:type="character" w:styleId="C1"><w:name w:val="Custom"/>)"
            R"(<w:rPr><w:b/></w:rPr></w:style></w:styles>)");
        CHECK(validator.Validate(*styles).IsValid());

        // Resolution really is per context and not a blanket "accept anything":
        // record content under a shared-string item stays a content-model violation.
        auto mismatched = LoadRootElement(
            R"(<?xml version="1.0" encoding="UTF-8"?>)"
            R"(<x:sst xmlns:x="http://schemas.openxmlformats.org/spreadsheetml/2006/main" count="1" uniqueCount="1">)"
            R"(<x:si><x:r><x:x v="0"/><x:n v="10"/></x:r></x:si></x:sst>)");
        CHECK(HasValidationIssue(validator.Validate(*mismatched),
                                 ExyokiOffice::ValidationErrorId::ParticleConstraintViolation));
    }

} // TEST_SUITE("DomContextResolutionTests")
