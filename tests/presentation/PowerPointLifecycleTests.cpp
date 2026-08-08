// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "doctest.h"

#include "TestSupport.hpp"

#include "ExyokiOffice/PowerPoint/PowerPointDocument.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <array>
#include <filesystem>

using ExyokiOffice::PowerPoint::PowerPointDocumentEditor;
using ExyokiOffice::PowerPoint::PowerPointDocumentType;

TEST_SUITE("PowerPointLifecycleTests")
{
    TEST_CASE("all PowerPoint package flavors survive create save open [unit] [powerpoint] [powerpoint-lifecycle]")
    {
        constexpr std::array types{
            PowerPointDocumentType::Presentation,
            PowerPointDocumentType::MacroEnabledPresentation,
            PowerPointDocumentType::Template,
            PowerPointDocumentType::MacroEnabledTemplate,
            PowerPointDocumentType::SlideShow,
            PowerPointDocumentType::MacroEnabledSlideShow};

        for (const auto type : types)
        {
            CAPTURE(static_cast<int>(type));
            auto editor = PowerPointDocumentEditor::CreateNew(type);
            REQUIRE(editor != nullptr);
            REQUIRE(editor->GetDocument() != nullptr);
            REQUIRE(editor->GetDocument()->GetPresentationPart() != nullptr);
            CHECK(editor->GetDocument()->GetDocumentType() == type);

            const auto bytes = editor->SaveToMemory();
            REQUIRE_FALSE(bytes.empty());
            auto reopened = PowerPointDocumentEditor::Open(bytes);
            REQUIRE(reopened != nullptr);
            CHECK(reopened->GetDocument()->GetDocumentType() == type);
            REQUIRE(reopened->GetDocument()->GetPresentationPart() != nullptr);
            CHECK(reopened->GetDocument()->GetCoreFilePropertiesPart() != nullptr);
            CHECK(reopened->GetDocument()->GetExtendedFilePropertiesPart() != nullptr);

            const auto secondSave = reopened->SaveToMemory();
            REQUIRE_FALSE(secondSave.empty());
            auto reopenedAgain = PowerPointDocumentEditor::Open(secondSave);
            REQUIRE(reopenedAgain != nullptr);
            CHECK(reopenedAgain->GetDocument()->GetDocumentType() == type);
        }
    }

    TEST_CASE("PowerPoint document type can be changed before round trip [unit] [powerpoint] [powerpoint-lifecycle]")
    {
        auto editor = PowerPointDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);
        editor->GetDocument()->ChangeDocumentType(PowerPointDocumentType::MacroEnabledSlideShow);
        CHECK(editor->GetDocument()->GetPresentationPart()->ContentType() ==
              "application/vnd.ms-powerpoint.slideshow.macroEnabled.main+xml");

        auto reopened = PowerPointDocumentEditor::Open(editor->SaveToMemory());
        REQUIRE(reopened != nullptr);
        CHECK(reopened->GetDocument()->GetDocumentType() == PowerPointDocumentType::MacroEnabledSlideShow);
    }

    TEST_CASE("PowerPoint lifecycle rejects missing input and supports file IO [unit] [powerpoint] [powerpoint-lifecycle]")
    {
        CHECK(PowerPointDocumentEditor::Open(std::vector<ExyokiOffice::Byte>{}) == nullptr);
        PowerPointDocumentEditor empty;
        CHECK(empty.SaveToMemory().empty());

        auto editor = PowerPointDocumentEditor::CreateNew(PowerPointDocumentType::Template);
        REQUIRE(editor != nullptr);
        const auto path = ExyokiOfficeTests::MakeTemporaryPath("ExyokiOffice_PPT001_roundtrip", ".potx");
        REQUIRE(editor->SaveToFile(path));
        auto reopened = PowerPointDocumentEditor::Open(path);
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        REQUIRE(reopened != nullptr);
        CHECK(reopened->GetDocument()->GetDocumentType() == PowerPointDocumentType::Template);
    }
} // namespace
