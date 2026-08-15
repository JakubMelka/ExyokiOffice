// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "doctest.h"

#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/Drawing.hpp"
#include "ExyokiOffice/Excel/ExcelDocument.hpp"
#include "ExyokiOffice/Packaging/GeneratedParts.hpp"
#include "ExyokiOffice/ThemeService.hpp"
#include "ExyokiOffice/Word/WordDocument.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <cstddef>
#include <memory>
#include <string>

namespace
{

namespace Drawing = ExyokiOffice::DocumentFormat::OpenXml::Drawing;

using ExyokiOffice::Color;
using ExyokiOffice::ThemeColorSlot;
using ExyokiOffice::ThemeService;
using ExyokiOffice::Excel::ExcelDocumentEditor;
using ExyokiOffice::Word::WordDocumentEditor;

constexpr ExyokiOffice::Size kAccent1 = static_cast<ExyokiOffice::Size>(ThemeColorSlot::Accent1);

} // namespace

TEST_SUITE("ThemeServiceTests")
{

    TEST_CASE("Word EnsureTheme creates a complete default theme [unit] [shared] [theme] [word]")
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);

        CHECK_FALSE(editor->ThemeSettings().has_value());
        REQUIRE(editor->EnsureTheme());

        const auto settings = editor->ThemeSettings();
        REQUIRE(settings.has_value());
        CHECK(settings->Name == "Office Theme");
        CHECK(settings->ColorSchemeName == "Office");
        CHECK(settings->MajorFonts.Latin == "Calibri Light");
        CHECK(settings->MinorFonts.Latin == "Calibri");
        CHECK(settings->Colors[kAccent1].ToHexString() == "4472C4");

        SUBCASE("EnsureTheme is idempotent")
        {
            auto modified = *settings;
            modified.Name = "Customized";
            REQUIRE(editor->SetThemeSettings(modified));
            CHECK(editor->EnsureTheme());
            CHECK(editor->ThemeSettings()->Name == "Customized");
        }
    }

    TEST_CASE("Word accent color change lands in the theme XML and survives reopen [unit] [shared] [theme] [word]")
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);
        REQUIRE(editor->EnsureTheme());

        auto settings = editor->ThemeSettings();
        REQUIRE(settings.has_value());
        const auto accent = Color::FromHexString("FF00A0");
        REQUIRE(accent.has_value());
        settings->Colors[kAccent1] = *accent;
        REQUIRE(editor->SetThemeSettings(*settings));

        const auto xml = editor->ThemeXml();
        REQUIRE(xml.has_value());
        CHECK(xml->find("FF00A0") != std::string::npos);

        const auto bytes = editor->SaveToMemory();
        REQUIRE_FALSE(bytes.empty());
        auto reopened = WordDocumentEditor::Open(bytes);
        REQUIRE(reopened != nullptr);
        const auto restored = reopened->ThemeSettings();
        REQUIRE(restored.has_value());
        CHECK(restored->Colors[kAccent1].ToHexString() == "FF00A0");
    }

    TEST_CASE("Excel workbook theme is created, edited, and removed [unit] [shared] [theme] [excel]")
    {
        auto editor = ExcelDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);

        CHECK_FALSE(editor->ThemeSettings().has_value());
        CHECK_FALSE(editor->RemoveTheme());
        REQUIRE(editor->EnsureTheme());

        auto settings = editor->ThemeSettings();
        REQUIRE(settings.has_value());
        const auto accent = Color::FromHexString("112233");
        REQUIRE(accent.has_value());
        settings->Colors[kAccent1] = *accent;
        settings->MajorFonts.Latin = "Georgia";
        REQUIRE(editor->SetThemeSettings(*settings));

        const auto bytes = editor->SaveToMemory();
        REQUIRE_FALSE(bytes.empty());
        auto reopened = ExcelDocumentEditor::Open(bytes);
        REQUIRE(reopened != nullptr);
        const auto restored = reopened->ThemeSettings();
        REQUIRE(restored.has_value());
        CHECK(restored->Colors[kAccent1].ToHexString() == "112233");
        CHECK(restored->MajorFonts.Latin == "Georgia");

        CHECK(reopened->RemoveTheme());
        CHECK_FALSE(reopened->ThemeSettings().has_value());
        CHECK_FALSE(reopened->ThemeXml().has_value());
    }

    TEST_CASE("SetThemeXml validates input and applies complete documents [unit] [shared] [theme]")
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);

        SUBCASE("invalid XML neither creates nor replaces a theme")
        {
            CHECK_FALSE(editor->SetThemeXml("<w:document/>"));
            CHECK_FALSE(editor->SetThemeXml("not xml at all"));
            CHECK_FALSE(editor->SetThemeXml(""));
            CHECK_FALSE(editor->ThemeXml().has_value());
        }

        SUBCASE("a well-formed theme document is accepted")
        {
            auto source = ExcelDocumentEditor::CreateNew();
            REQUIRE(source != nullptr);
            REQUIRE(source->EnsureTheme());
            const auto themeXml = source->ThemeXml();
            REQUIRE(themeXml.has_value());

            REQUIRE(editor->SetThemeXml(*themeXml));
            const auto settings = editor->ThemeSettings();
            REQUIRE(settings.has_value());
            CHECK(settings->Name == "Office Theme");
        }

        SUBCASE("invalid XML leaves an existing theme untouched")
        {
            REQUIRE(editor->EnsureTheme());
            const auto before = editor->ThemeXml();
            REQUIRE(before.has_value());
            CHECK_FALSE(editor->SetThemeXml("<broken"));
            CHECK(editor->ThemeXml() == before);
        }
    }

    TEST_CASE("Theme settings validation rejects incomplete input [unit] [shared] [theme]")
    {
        auto editor = ExcelDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);

        // No theme part yet: writing settings must fail.
        ExyokiOffice::ThemeSettings settings;
        CHECK_FALSE(editor->SetThemeSettings(settings));

        REQUIRE(editor->EnsureTheme());
        auto valid = editor->ThemeSettings();
        REQUIRE(valid.has_value());

        auto missingName = *valid;
        missingName.Name.clear();
        CHECK_FALSE(editor->SetThemeSettings(missingName));

        auto missingLatin = *valid;
        missingLatin.MinorFonts.Latin.clear();
        CHECK_FALSE(editor->SetThemeSettings(missingLatin));

        auto badSupplemental = *valid;
        badSupplemental.MajorFonts.SupplementalFonts.emplace_back("", "Typeface");
        CHECK_FALSE(editor->SetThemeSettings(badSupplemental));

        // The failed writes must not have corrupted the stored theme.
        CHECK(editor->ThemeSettings() == valid);
    }

    TEST_CASE("The default theme is built through the typed DOM with a complete style matrix [unit] [shared] [theme]")
    {
        auto part = std::make_shared<ExyokiOffice::Packaging::ThemePart>();
        REQUIRE(ThemeService::WriteDefaultTheme(part, "Built Theme"));

        auto theme = part->GetTypedRootElement();
        REQUIRE(theme != nullptr);
        CHECK(theme->GetName().ToString() == "Built Theme");

        auto elements = theme->GetFirstChildOfType<Drawing::ThemeElements>();
        REQUIRE(elements != nullptr);
        auto format = elements->GetFirstChildOfType<Drawing::FormatScheme>();
        REQUIRE(format != nullptr);

        // ECMA-376 requires at least three entries in each style matrix.
        auto fills = format->GetFirstChildOfType<Drawing::FillStyleList>();
        auto lines = format->GetFirstChildOfType<Drawing::LineStyleList>();
        auto effects = format->GetFirstChildOfType<Drawing::EffectStyleList>();
        auto backgroundFills = format->GetFirstChildOfType<Drawing::BackgroundFillStyleList>();
        REQUIRE(fills != nullptr);
        REQUIRE(lines != nullptr);
        REQUIRE(effects != nullptr);
        REQUIRE(backgroundFills != nullptr);
        CHECK(fills->Children().size() == 3);
        CHECK(lines->Elements<Drawing::Outline>().size() == 3);
        CHECK(effects->Elements<Drawing::EffectStyle>().size() == 3);
        CHECK(backgroundFills->Children().size() == 3);

        CHECK(elements->GetFirstChildOfType<Drawing::ColorScheme>() != nullptr);
        CHECK(theme->GetFirstChildOfType<Drawing::ObjectDefaults>() != nullptr);
        CHECK(theme->GetFirstChildOfType<Drawing::ExtraColorSchemeList>() != nullptr);

        SUBCASE("the serialized markup uses the DrawingML namespace Office expects")
        {
            // The DOM must emit prefixed elements bound to the DrawingML namespace;
            // reading the theme back through the same DOM would not catch a wrong
            // prefix or namespace, but Office would reject the part.
            const auto xml = part->GetXmlString();
            CHECK(xml.find("<a:theme") != std::string::npos);
            CHECK(xml.find(R"(xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main")") !=
                  std::string::npos);
            CHECK(xml.find("<a:clrScheme") != std::string::npos);
            CHECK(xml.find("<a:accent1") != std::string::npos);
            CHECK(xml.find(R"(<a:srgbClr val="4472C4")") != std::string::npos);
            CHECK(xml.find(R"(val="windowText")") != std::string::npos);
            CHECK(xml.find(R"(lastClr="000000")") != std::string::npos);
            CHECK(xml.find(R"(<a:latin typeface="Calibri Light")") != std::string::npos);
            CHECK(xml.find(R"(<a:gs pos="50000")") != std::string::npos);
            CHECK(xml.find(R"(<a:lin ang="5400000")") != std::string::npos);
            CHECK(xml.find(R"(<a:schemeClr val="phClr")") != std::string::npos);
        }

        SUBCASE("the built theme satisfies the typed settings reader")
        {
            const auto settings = ThemeService::ReadSettings(part);
            REQUIRE(settings.has_value());
            CHECK(settings->Colors[kAccent1].ToHexString() == "4472C4");
            // dk1/lt1 are system colors; reading resolves them through lastClr.
            CHECK(settings->Colors[static_cast<ExyokiOffice::Size>(ThemeColorSlot::Dark1)].ToHexString() == "000000");
            CHECK(settings->Colors[static_cast<ExyokiOffice::Size>(ThemeColorSlot::Light1)].ToHexString() == "FFFFFF");
            CHECK(settings->MajorFonts.Latin == "Calibri Light");
            CHECK(settings->MinorFonts.EastAsian.empty());
        }

        SUBCASE("rebuilding replaces the previous content instead of appending")
        {
            REQUIRE(ThemeService::WriteDefaultTheme(part, "Rebuilt"));
            CHECK(part->GetTypedRootElement()->Elements<Drawing::ThemeElements>().size() == 1);
            CHECK(ThemeService::ReadSettings(part)->Name == "Rebuilt");
        }

        SUBCASE("an empty name and a missing part are rejected")
        {
            CHECK_FALSE(ThemeService::WriteDefaultTheme(part, ""));
            CHECK_FALSE(ThemeService::WriteDefaultTheme(nullptr));
        }
    }

    TEST_CASE("Theme XML validation rejects malformed documents [unit] [shared] [theme]")
    {
        CHECK_FALSE(ThemeService::IsValidThemeXml(""));
        CHECK_FALSE(ThemeService::IsValidThemeXml("<a:theme"));

        auto part = std::make_shared<ExyokiOffice::Packaging::ThemePart>();
        REQUIRE(ThemeService::WriteDefaultTheme(part));
        CHECK(ThemeService::IsValidThemeXml(part->GetXmlString()));
    }

} // TEST_SUITE("ThemeServiceTests")
