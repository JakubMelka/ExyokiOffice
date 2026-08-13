// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "doctest.h"

#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/ExtendedProperties.hpp"
#include "ExyokiOffice/Excel/ExcelDocument.hpp"
#include "ExyokiOffice/Packaging/DocumentProperties.hpp"
#include "ExyokiOffice/PowerPoint/PowerPointDocument.hpp"
#include "ExyokiOffice/Word/WordDocument.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <chrono>
#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace
{

namespace Ap = ExyokiOffice::DocumentFormat::OpenXml::ExtendedProperties;

using ExyokiOffice::Excel::ExcelDocumentEditor;
using ExyokiOffice::Packaging::DocumentCustomPropertyValue;
using ExyokiOffice::Packaging::DocumentProperties;
using ExyokiOffice::PowerPoint::PowerPointDocumentEditor;
using ExyokiOffice::Word::WordDocumentEditor;

std::chrono::system_clock::time_point MakeUtcTime(int year, unsigned month, unsigned day,
                                                  int hour, int minute, int second)
{
    using namespace std::chrono;
    return sys_days(year_month_day(std::chrono::year(year), std::chrono::month(month),
                                   std::chrono::day(day))) +
           hours(hour) + minutes(minute) + seconds(second);
}

void FillCommonProperties(DocumentProperties properties)
{
    CHECK(properties.SetTitle("Quarterly report"));
    CHECK(properties.SetSubject("Finance"));
    CHECK(properties.SetCreator("Alice Author"));
    CHECK(properties.SetKeywords("finance;q3;report"));
    CHECK(properties.SetDescription("Detailed Q3 numbers"));
    CHECK(properties.SetLastModifiedBy("Bob Editor"));
    CHECK(properties.SetCategory("Reports"));
    CHECK(properties.SetContentStatus("Draft"));
    CHECK(properties.SetLanguage("en-US"));
    CHECK(properties.SetIdentifier("DOC-42"));
    CHECK(properties.SetRevision("7"));
    CHECK(properties.SetVersion("1.3"));
    CHECK(properties.SetCompany("Contoso"));
    CHECK(properties.SetManager("Carol Manager"));
    CHECK(properties.SetHyperlinkBase("https://contoso.example"));
    CHECK(properties.SetCustomProperty("Project", std::string("Apollo")));
    CHECK(properties.SetCustomProperty("Reviewed", true));
}

void VerifyCommonProperties(DocumentProperties properties)
{
    CHECK(properties.GetTitle() == "Quarterly report");
    CHECK(properties.GetSubject() == "Finance");
    CHECK(properties.GetCreator() == "Alice Author");
    CHECK(properties.GetKeywords() == "finance;q3;report");
    CHECK(properties.GetDescription() == "Detailed Q3 numbers");
    CHECK(properties.GetLastModifiedBy() == "Bob Editor");
    CHECK(properties.GetCategory() == "Reports");
    CHECK(properties.GetContentStatus() == "Draft");
    CHECK(properties.GetLanguage() == "en-US");
    CHECK(properties.GetIdentifier() == "DOC-42");
    CHECK(properties.GetRevision() == "7");
    CHECK(properties.GetVersion() == "1.3");
    CHECK(properties.GetCompany() == "Contoso");
    CHECK(properties.GetManager() == "Carol Manager");
    CHECK(properties.GetHyperlinkBase() == "https://contoso.example");

    const auto project = properties.GetCustomProperty("Project");
    REQUIRE(project.has_value());
    CHECK(std::get<std::string>(*project) == "Apollo");
    const auto reviewed = properties.GetCustomProperty("Reviewed");
    REQUIRE(reviewed.has_value());
    CHECK(std::get<bool>(*reviewed) == true);

    // Save-time bookkeeping never destroys user values but always maintains timestamps.
    CHECK(properties.GetCreated().has_value());
    CHECK(properties.GetModified().has_value());
}

} // namespace

TEST_SUITE("DocumentPropertiesTests")
{

    TEST_CASE("Word core, extended, and custom properties survive open-save-open [unit] [shared] [properties] [word]")
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);
        FillCommonProperties(editor->Properties());

        const auto bytes = editor->SaveToMemory();
        REQUIRE_FALSE(bytes.empty());

        auto reopened = WordDocumentEditor::Open(bytes);
        REQUIRE(reopened != nullptr);
        VerifyCommonProperties(reopened->Properties());
    }

    TEST_CASE("Excel core, extended, and custom properties survive open-save-open [unit] [shared] [properties] [excel]")
    {
        auto editor = ExcelDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);
        FillCommonProperties(editor->Properties());

        const auto bytes = editor->SaveToMemory();
        REQUIRE_FALSE(bytes.empty());

        auto reopened = ExcelDocumentEditor::Open(bytes);
        REQUIRE(reopened != nullptr);
        VerifyCommonProperties(reopened->Properties());
    }

    TEST_CASE("PowerPoint core, extended, and custom properties survive open-save-open [unit] [shared] [properties] [powerpoint]")
    {
        auto editor = PowerPointDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);
        FillCommonProperties(editor->Properties());

        const auto bytes = editor->SaveToMemory();
        REQUIRE_FALSE(bytes.empty());

        auto reopened = PowerPointDocumentEditor::Open(bytes);
        REQUIRE(reopened != nullptr);
        VerifyCommonProperties(reopened->Properties());
    }

    TEST_CASE("Custom properties round-trip string, number, bool, and date values [unit] [shared] [properties]")
    {
        auto editor = ExcelDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);
        auto properties = editor->Properties();

        const auto timestamp = MakeUtcTime(2026, 3, 14, 9, 26, 53);
        CHECK(properties.SetCustomProperty("Client", std::string("Fabrikam")));
        CHECK(properties.SetCustomProperty("Iterations", ExyokiOffice::Int32(42)));
        CHECK(properties.SetCustomProperty("Tolerance", 2.5));
        CHECK(properties.SetCustomProperty("Approved", false));
        CHECK(properties.SetCustomProperty("Deadline", timestamp));

        const auto bytes = editor->SaveToMemory();
        REQUIRE_FALSE(bytes.empty());
        auto reopened = ExcelDocumentEditor::Open(bytes);
        REQUIRE(reopened != nullptr);
        auto restored = reopened->Properties();

        const auto client = restored.GetCustomProperty("Client");
        REQUIRE(client.has_value());
        CHECK(std::get<std::string>(*client) == "Fabrikam");

        const auto iterations = restored.GetCustomProperty("Iterations");
        REQUIRE(iterations.has_value());
        CHECK(std::get<ExyokiOffice::Int32>(*iterations) == 42);

        const auto tolerance = restored.GetCustomProperty("Tolerance");
        REQUIRE(tolerance.has_value());
        CHECK(std::get<ExyokiOffice::Real>(*tolerance) == doctest::Approx(2.5));

        const auto approved = restored.GetCustomProperty("Approved");
        REQUIRE(approved.has_value());
        CHECK(std::get<bool>(*approved) == false);

        const auto deadline = restored.GetCustomProperty("Deadline");
        REQUIRE(deadline.has_value());
        CHECK(std::get<std::chrono::system_clock::time_point>(*deadline) == timestamp);

        const auto all = restored.GetCustomProperties();
        CHECK(all.size() == 5);
    }

    TEST_CASE("Custom property part carries the user-defined format id and unique pids [unit] [shared] [properties]")
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);
        auto properties = editor->Properties();

        CHECK(properties.SetCustomProperty("First", std::string("a")));
        CHECK(properties.SetCustomProperty("Second", ExyokiOffice::Int32(2)));
        CHECK(properties.SetCustomProperty("Third", 3.0));

        auto document = editor->GetDocument();
        REQUIRE(document != nullptr);
        auto customPart = document->GetCustomFilePropertiesPart();
        REQUIRE(customPart != nullptr);
        const auto xml = customPart->GetXmlString();
        CHECK(xml.find("{D5CDD505-2E9C-101B-9397-08002B2CF9AE}") != std::string::npos);
        CHECK(xml.find("pid=\"2\"") != std::string::npos);
        CHECK(xml.find("pid=\"3\"") != std::string::npos);
        CHECK(xml.find("pid=\"4\"") != std::string::npos);

        SUBCASE("updating a property keeps its pid and changes only the value")
        {
            CHECK(properties.SetCustomProperty("second", std::string("replaced")));
            const auto value = properties.GetCustomProperty("Second");
            REQUIRE(value.has_value());
            CHECK(std::get<std::string>(*value) == "replaced");
            const auto updatedXml = customPart->GetXmlString();
            CHECK(updatedXml.find("pid=\"3\"") != std::string::npos);
            CHECK(properties.GetCustomPropertyNames().size() == 3);
        }

        SUBCASE("remove deletes a single property, clear removes the rest")
        {
            CHECK(properties.RemoveCustomProperty("First"));
            CHECK_FALSE(properties.RemoveCustomProperty("First"));
            CHECK_FALSE(properties.GetCustomProperty("First").has_value());
            CHECK(properties.ClearCustomProperties() == 2);
            CHECK(properties.GetCustomPropertyNames().empty());
        }
    }

    TEST_CASE("Empty core values remove elements instead of storing empty text [unit] [shared] [properties]")
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);
        auto properties = editor->Properties();

        CHECK(properties.SetTitle("Temporary"));
        auto corePart = editor->GetDocument()->GetCoreFilePropertiesPart();
        REQUIRE(corePart != nullptr);
        CHECK(corePart->GetXmlString().find("dc:title") != std::string::npos);

        CHECK(properties.SetTitle(""));
        CHECK(properties.GetTitle().empty());
        CHECK(corePart->GetXmlString().find("dc:title") == std::string::npos);
    }

    TEST_CASE("Core date properties are typed and removable [unit] [shared] [properties]")
    {
        auto editor = ExcelDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);
        auto properties = editor->Properties();

        const auto printed = MakeUtcTime(2026, 7, 1, 12, 0, 0);
        CHECK(properties.SetLastPrinted(printed));
        CHECK(properties.GetLastPrinted() == printed);

        const auto created = MakeUtcTime(2020, 1, 2, 3, 4, 5);
        CHECK(properties.SetCreated(created));
        CHECK(properties.GetCreated() == created);

        CHECK(properties.SetLastPrinted(std::nullopt));
        CHECK_FALSE(properties.GetLastPrinted().has_value());
    }

    TEST_CASE("W3CDTF parsing accepts reduced precision and offsets [unit] [shared] [properties]")
    {
        using DP = DocumentProperties;

        const auto full = DP::ParseW3cDateTime("2026-03-14T09:26:53Z");
        REQUIRE(full.has_value());
        CHECK(*full == MakeUtcTime(2026, 3, 14, 9, 26, 53));
        CHECK(DP::FormatW3cDateTime(*full) == "2026-03-14T09:26:53Z");

        const auto offset = DP::ParseW3cDateTime("2026-03-14T10:26:53+01:00");
        REQUIRE(offset.has_value());
        CHECK(*offset == MakeUtcTime(2026, 3, 14, 9, 26, 53));

        const auto negativeOffset = DP::ParseW3cDateTime("2026-03-14T04:26:53-05:00");
        REQUIRE(negativeOffset.has_value());
        CHECK(*negativeOffset == MakeUtcTime(2026, 3, 14, 9, 26, 53));

        const auto fractional = DP::ParseW3cDateTime("2026-03-14T09:26:53.500Z");
        REQUIRE(fractional.has_value());
        CHECK(*fractional == MakeUtcTime(2026, 3, 14, 9, 26, 53) + std::chrono::milliseconds(500));

        const auto dateOnly = DP::ParseW3cDateTime("2026-03-14");
        REQUIRE(dateOnly.has_value());
        CHECK(*dateOnly == MakeUtcTime(2026, 3, 14, 0, 0, 0));

        const auto yearMonth = DP::ParseW3cDateTime("2026-03");
        REQUIRE(yearMonth.has_value());
        CHECK(*yearMonth == MakeUtcTime(2026, 3, 1, 0, 0, 0));

        const auto yearOnly = DP::ParseW3cDateTime("2026");
        REQUIRE(yearOnly.has_value());
        CHECK(*yearOnly == MakeUtcTime(2026, 1, 1, 0, 0, 0));

        CHECK_FALSE(DP::ParseW3cDateTime("").has_value());
        CHECK_FALSE(DP::ParseW3cDateTime("not-a-date").has_value());
        CHECK_FALSE(DP::ParseW3cDateTime("2026-13-01").has_value());
        CHECK_FALSE(DP::ParseW3cDateTime("2026-02-30").has_value());
        CHECK_FALSE(DP::ParseW3cDateTime("2026-03-14T25:00:00Z").has_value());
        CHECK_FALSE(DP::ParseW3cDateTime("2026-03-14T09:26:53Zjunk").has_value());
    }

    TEST_CASE("Saving preserves a user-selected producing application [unit] [shared] [properties]")
    {
        auto editor = PowerPointDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);
        auto properties = editor->Properties();
        CHECK(properties.SetApplication("CustomExporter"));
        CHECK(properties.SetApplicationVersion("2.5"));

        const auto bytes = editor->SaveToMemory();
        REQUIRE_FALSE(bytes.empty());
        auto reopened = PowerPointDocumentEditor::Open(bytes);
        REQUIRE(reopened != nullptr);
        CHECK(reopened->Properties().GetApplication() == "CustomExporter");
        CHECK(reopened->Properties().GetApplicationVersion() == "2.5");
    }

    TEST_CASE("Word legacy property accessors and the shared editor stay consistent [unit] [shared] [properties] [word]")
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);
        auto document = editor->GetDocument();
        REQUIRE(document != nullptr);

        document->SetTitle("Legacy title");
        CHECK(editor->Properties().GetTitle() == "Legacy title");

        CHECK(editor->Properties().SetCreator("Shared creator"));
        CHECK(document->GetCreator() == "Shared creator");

        document->SetCompany("Legacy company");
        CHECK(editor->Properties().GetCompany() == "Legacy company");
    }

    TEST_CASE("Reads on documents without properties parts return empty values [unit] [shared] [properties]")
    {
        auto document = ExyokiOffice::Packaging::WordDocument::Create(
            ExyokiOffice::Packaging::WordprocessingDocumentType::Document);
        REQUIRE(document != nullptr);
        DocumentProperties properties(*document);

        CHECK(properties.GetTitle().empty());
        CHECK_FALSE(properties.GetCreated().has_value());
        CHECK(properties.GetCompany().empty());
        CHECK_FALSE(properties.GetPages().has_value());
        CHECK(properties.GetCustomPropertyNames().empty());
        CHECK_FALSE(properties.GetCustomProperty("Missing").has_value());
        CHECK_FALSE(properties.RemoveCustomProperty("Missing"));
        CHECK(properties.ClearCustomProperties() == 0);

        // Removing a value on a document without parts must not create the part.
        CHECK(properties.SetTitle(""));
        CHECK(document->GetCoreFilePropertiesPart() == nullptr);
    }

    TEST_CASE("Application statistics written by the producing application are read back [unit] [shared] [properties]")
    {
        // These are read-only: ExyokiOffice never computes a page or line count,
        // so the only way they carry a value is that another application wrote
        // one. The document below stands in for that application - the elements
        // are placed in `docProps/app.xml` directly, which is exactly what Word
        // leaves behind.
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);

        // Template is the one extended value this library does write, so it goes
        // through the setter and the rest through the part.
        CHECK(editor->Properties().SetTemplate("Report.dotx"));

        auto extendedPart = editor->GetDocument()->GetExtendedFilePropertiesPart();
        REQUIRE(extendedPart != nullptr);
        auto root = extendedPart->GetTypedRootElement();
        REQUIRE(root != nullptr);

        root->AppendChild<Ap::Pages>()->SetText("12");
        root->AppendChild<Ap::Words>()->SetText("3400");
        root->AppendChild<Ap::Characters>()->SetText("19000");
        root->AppendChild<Ap::CharactersWithSpaces>()->SetText("22400");
        root->AppendChild<Ap::Lines>()->SetText("290");
        root->AppendChild<Ap::Paragraphs>()->SetText("84");
        root->AppendChild<Ap::TotalTime>()->SetText("45");

        const auto bytes = editor->SaveToMemory();
        REQUIRE_FALSE(bytes.empty());
        auto reopened = WordDocumentEditor::Open(bytes);
        REQUIRE(reopened != nullptr);
        auto reloaded = reopened->Properties();

        CHECK(reloaded.GetTemplate() == "Report.dotx");
        REQUIRE(reloaded.GetPages().has_value());
        CHECK(*reloaded.GetPages() == 12);
        REQUIRE(reloaded.GetWords().has_value());
        CHECK(*reloaded.GetWords() == 3400);
        REQUIRE(reloaded.GetCharacters().has_value());
        CHECK(*reloaded.GetCharacters() == 19000);
        REQUIRE(reloaded.GetCharactersWithSpaces().has_value());
        CHECK(*reloaded.GetCharactersWithSpaces() == 22400);
        REQUIRE(reloaded.GetLines().has_value());
        CHECK(*reloaded.GetLines() == 290);
        REQUIRE(reloaded.GetParagraphs().has_value());
        CHECK(*reloaded.GetParagraphs() == 84);
        REQUIRE(reloaded.GetTotalTime().has_value());
        CHECK(*reloaded.GetTotalTime() == 45);

        // A presentation-only statistic is absent from a Word document rather
        // than defaulting to zero.
        CHECK_FALSE(reloaded.GetSlides().has_value());
        CHECK_FALSE(reloaded.GetNotes().has_value());
        CHECK_FALSE(reloaded.GetHiddenSlides().has_value());

        // Clearing the template removes the element instead of storing an empty
        // one, which is what the core setters do as well.
        CHECK(reopened->Properties().SetTemplate(""));
        CHECK(reopened->Properties().GetTemplate().empty());
    }

    TEST_CASE("A statistic that is not a 32-bit number reads as absent [unit] [shared] [properties]")
    {
        // `ap:Words` is xsd:int in the schema, but the reader is pointed at
        // files this library did not produce. Text that is not a number, and a
        // number that does not fit, both have to come back as "no value" - the
        // alternative is a truncated count presented as fact.
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);
        CHECK(editor->Properties().SetTemplate("Normal.dotm"));

        auto root = editor->GetDocument()->GetExtendedFilePropertiesPart()->GetTypedRootElement();
        REQUIRE(root != nullptr);
        root->AppendChild<Ap::Words>()->SetText("many");
        root->AppendChild<Ap::Lines>()->SetText("3000000000");
        root->AppendChild<Ap::Characters>()->SetText("");
        root->AppendChild<Ap::Paragraphs>()->SetText("-17");

        auto reopened = WordDocumentEditor::Open(editor->SaveToMemory());
        REQUIRE(reopened != nullptr);
        auto reloaded = reopened->Properties();

        CHECK_FALSE(reloaded.GetWords().has_value());
        CHECK_FALSE(reloaded.GetLines().has_value());
        CHECK_FALSE(reloaded.GetCharacters().has_value());
        // A negative count is still a number the file states, so it is reported.
        REQUIRE(reloaded.GetParagraphs().has_value());
        CHECK(*reloaded.GetParagraphs() == -17);
    }

} // TEST_SUITE("DocumentPropertiesTests")
