// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "ExyokiOffice/Excel/ExcelDocument.hpp"
#include "ExyokiOffice/StandardTypes.hpp"
#include "doctest.h"

#include <array>
#include <cstdint>
#include <vector>

namespace
{
using ExyokiOffice::Excel::ExcelDocument;
using ExyokiOffice::Excel::ExcelDocumentEditor;
using ExyokiOffice::Excel::SpreadsheetDocumentType;

constexpr std::array<ExyokiOffice::Byte, 12> kVbaProject = {
    0xD0, 0xCF, 0x11, 0xE0, 0xA1, 0xB1, 0x1A, 0xE1, 0x56, 0x42, 0x41, 0x00};
constexpr std::array<ExyokiOffice::Byte, 5> kReplacement = {0x56, 0x42, 0x41, 0x02, 0xFF};
} // namespace

TEST_SUITE("ExcelVbaTests")
{
    TEST_CASE("low-level document adds extracts and removes an opaque VBA project [unit] [excel] [vba]")
    {
        auto document = ExcelDocument::Create(SpreadsheetDocumentType::Template);
        REQUIRE(document);
        REQUIRE(document->InitDocument());
        CHECK_FALSE(document->HasVbaProject());
        CHECK(document->GetVbaProjectData().empty());
        CHECK_FALSE(document->SetVbaProjectData({}));

        REQUIRE(document->SetVbaProjectData(kVbaProject));
        CHECK(document->HasVbaProject());
        CHECK(document->GetDocumentType() == SpreadsheetDocumentType::MacroEnabledTemplate);
        CHECK(document->GetVbaProjectData() ==
              std::vector<ExyokiOffice::Byte>(kVbaProject.begin(), kVbaProject.end()));

        REQUIRE(document->RemoveVbaProject());
        CHECK_FALSE(document->HasVbaProject());
        CHECK(document->GetDocumentType() == SpreadsheetDocumentType::Template);
        CHECK_FALSE(document->RemoveVbaProject());
    }

    TEST_CASE("editor preserves and replaces VBA bytes through save-open round trips [unit] [excel] [vba]")
    {
        auto editor = ExcelDocumentEditor::CreateNew();
        REQUIRE(editor);
        REQUIRE(editor->SetVbaProjectData(kVbaProject));
        CHECK(editor->HasVbaProject());
        CHECK(editor->GetDocument()->GetDocumentType() ==
              SpreadsheetDocumentType::MacroEnabledWorkbook);

        auto saved = editor->SaveToMemory();
        REQUIRE_FALSE(saved.empty());
        auto reopened = ExcelDocumentEditor::Open(saved);
        REQUIRE(reopened);
        CHECK(reopened->HasVbaProject());
        CHECK(reopened->GetVbaProjectData() ==
              std::vector<ExyokiOffice::Byte>(kVbaProject.begin(), kVbaProject.end()));
        CHECK(reopened->GetDocument()->GetDocumentType() ==
              SpreadsheetDocumentType::MacroEnabledWorkbook);

        REQUIRE(reopened->SetVbaProjectData(kReplacement));
        CHECK(reopened->GetVbaProjectData() ==
              std::vector<ExyokiOffice::Byte>(kReplacement.begin(), kReplacement.end()));
        auto replaced = ExcelDocumentEditor::Open(reopened->SaveToMemory());
        REQUIRE(replaced);
        CHECK(replaced->GetVbaProjectData() ==
              std::vector<ExyokiOffice::Byte>(kReplacement.begin(), kReplacement.end()));

        REQUIRE(replaced->RemoveVbaProject());
        CHECK(replaced->GetDocument()->GetDocumentType() == SpreadsheetDocumentType::Workbook);
        auto macroFree = ExcelDocumentEditor::Open(replaced->SaveToMemory());
        REQUIRE(macroFree);
        CHECK_FALSE(macroFree->HasVbaProject());
        CHECK(macroFree->GetDocument()->GetDocumentType() == SpreadsheetDocumentType::Workbook);
    }

    TEST_CASE("detached editor rejects VBA mutations [unit] [excel] [vba]")
    {
        auto editor = ExcelDocumentEditor::Create();
        REQUIRE(editor);
        CHECK_FALSE(editor->HasVbaProject());
        CHECK(editor->GetVbaProjectData().empty());
        CHECK_FALSE(editor->SetVbaProjectData(kVbaProject));
        CHECK_FALSE(editor->RemoveVbaProject());
    }
}
