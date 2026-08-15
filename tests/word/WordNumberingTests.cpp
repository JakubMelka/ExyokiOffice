// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "doctest.h"

#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/Wordprocessing.hpp"
#include "ExyokiOffice/Word/WordDocument.hpp"

#include <memory>
#include <string>
#include <vector>

namespace
{
namespace W = ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing;

using ExyokiOffice::MeasurementUnit;
using ExyokiOffice::MeasuringUnits;
using ExyokiOffice::Word::NumberingDefinition;
using ExyokiOffice::Word::NumberingLevelDefinition;
using ExyokiOffice::Word::NumberingLevelOverride;
using ExyokiOffice::Word::ParagraphNumbering;
using ExyokiOffice::Word::WordDocumentEditor;

NumberingDefinition MakeNineLevelOutline(std::string name = "NineLevelOutline")
{
    NumberingDefinition definition;
    definition.Name = std::move(name);
    for (int level = 0; level < 9; ++level)
    {
        NumberingLevelDefinition item;
        item.Level = level;
        item.Start = level + 1;
        item.Format = level % 2 == 0 ? W::NumberFormatValues::Decimal : W::NumberFormatValues::LowerLetter;
        item.LevelText = "%" + std::to_string(level + 1) + ".";
        item.LeftIndent = MeasuringUnits(0.5 + (level * 0.25), MeasurementUnit::Inch);
        item.HangingIndent = MeasuringUnits(0.25, MeasurementUnit::Inch);
        if (level > 0)
        {
            item.RestartAfterLevel = level - 1;
        }
        definition.Levels.push_back(item);
    }
    return definition;
}

std::shared_ptr<W::Numbering> NumberingRoot(const WordDocumentEditor::Ptr& editor)
{
    auto document = editor ? editor->GetDocument() : nullptr;
    auto mainPart = document ? document->GetMainDocumentPart() : nullptr;
    auto numberingPart = mainPart ? mainPart->GetNumberingDefinitionsPart() : nullptr;
    return numberingPart ? numberingPart->GetTypedRootElement() : nullptr;
}

std::shared_ptr<W::NumberingInstance> FindInstance(const std::shared_ptr<W::Numbering>& numbering, int numberingId)
{
    if (!numbering)
    {
        return nullptr;
    }
    for (const auto& instance : numbering->Elements<W::NumberingInstance>())
    {
        if (instance && instance->GetNumberID().IsDefined() && instance->GetNumberID().Value() == numberingId)
        {
            return instance;
        }
    }
    return nullptr;
}

std::shared_ptr<W::AbstractNum> AbstractForInstance(const std::shared_ptr<W::Numbering>& numbering, int numberingId)
{
    auto instance = FindInstance(numbering, numberingId);
    auto abstractId = instance ? instance->GetFirstChildOfType<W::AbstractNumId>() : nullptr;
    if (!numbering || !abstractId || !abstractId->GetVal().IsDefined())
    {
        return nullptr;
    }
    for (const auto& abstractNum : numbering->Elements<W::AbstractNum>())
    {
        if (abstractNum && abstractNum->GetAbstractNumberId().IsDefined() && abstractNum->GetAbstractNumberId().Value() == abstractId->GetVal().Value())
        {
            return abstractNum;
        }
    }
    return nullptr;
}

std::shared_ptr<W::Level> FindLevel(const std::shared_ptr<W::AbstractNum>& abstractNum, int level)
{
    if (!abstractNum)
    {
        return nullptr;
    }
    for (const auto& item : abstractNum->Elements<W::Level>())
    {
        if (item && item->GetLevelIndex().IsDefined() && item->GetLevelIndex().Value() == level)
        {
            return item;
        }
    }
    return nullptr;
}

std::shared_ptr<ExyokiOffice::Word::Paragraph> ReopenParagraph(
    const WordDocumentEditor::Ptr& editor,
    std::string_view text,
    WordDocumentEditor::Ptr& reopenedEditor)
{
    reopenedEditor = WordDocumentEditor::Open(editor->SaveToMemory());
    REQUIRE(reopenedEditor != nullptr);
    for (const auto& paragraph : reopenedEditor->Paragraphs())
    {
        if (paragraph && paragraph->PlainText() == text)
        {
            return paragraph;
        }
    }
    FAIL("Expected paragraph was not found");
    return nullptr;
}

} // namespace

TEST_SUITE("WordNumberingTests")
{

    TEST_CASE("NumberingManager creates a nine-level list and paragraphs retain all levels [unit] [word] [word-numbering]")
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);

        auto style = editor->Numbering().EnsureMultilevelList(MakeNineLevelOutline());
        REQUIRE(style.NumberingId > 0);
        CHECK(style.Level == 0);

        for (int level = 0; level < 9; ++level)
        {
            auto paragraph = editor->AddParagraph("Level " + std::to_string(level));
            REQUIRE(paragraph != nullptr);
            paragraph->SetNumbering(style.NumberingId, level);
        }

        auto numbering = NumberingRoot(editor);
        REQUIRE(numbering != nullptr);
        auto abstractNum = AbstractForInstance(numbering, style.NumberingId);
        REQUIRE(abstractNum != nullptr);
        CHECK(abstractNum->Elements<W::Level>().size() == 9);

        for (int level = 0; level < 9; ++level)
        {
            auto levelElement = FindLevel(abstractNum, level);
            REQUIRE(levelElement != nullptr);
            auto text = levelElement->GetFirstChildOfType<W::LevelText>();
            REQUIRE(text != nullptr);
            CHECK(text->GetVal().ToString() == "%" + std::to_string(level + 1) + ".");
            auto format = levelElement->GetFirstChildOfType<W::NumberingFormat>();
            REQUIRE(format != nullptr);
            CHECK(format->GetVal().Value() == (level % 2 == 0 ? W::NumberFormatValues::Decimal : W::NumberFormatValues::LowerLetter));
        }

        WordDocumentEditor::Ptr reopened;
        auto paragraph = ReopenParagraph(editor, "Level 8", reopened);
        REQUIRE(paragraph != nullptr);
        ParagraphNumbering paragraphNumbering;
        REQUIRE(paragraph->TryGetNumbering(paragraphNumbering));
        REQUIRE(paragraphNumbering.NumberingId.has_value());
        REQUIRE(paragraphNumbering.Level.has_value());
        CHECK(*paragraphNumbering.NumberingId == style.NumberingId);
        CHECK(*paragraphNumbering.Level == 8);

        auto reopenedInstances = reopened->Numbering().Instances();
        REQUIRE(reopenedInstances.size() == 1);
        CHECK(reopenedInstances.front().NumberingId == style.NumberingId);
    }

    TEST_CASE("NumberingManager creates continue and restart instances with level overrides [unit] [word] [word-numbering]")
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);

        auto base = editor->Numbering().EnsureMultilevelList(MakeNineLevelOutline("RestartableOutline"));
        REQUIRE(base.NumberingId > 0);

        auto continued = editor->Numbering().ContinueList(base.NumberingId);
        REQUIRE(continued.NumberingId > 0);
        CHECK(continued.NumberingId == base.NumberingId);

        auto restarted = editor->Numbering().RestartList(base.NumberingId, {{0, 1}, {2, 7}});
        REQUIRE(restarted.NumberingId > 0);
        CHECK(restarted.NumberingId != base.NumberingId);

        auto instances = editor->Numbering().Instances();
        CHECK(instances.size() == 2);

        auto continuedInfo = editor->Numbering().GetInstance(continued.NumberingId);
        REQUIRE(continuedInfo.has_value());
        CHECK(continuedInfo->Overrides.empty());

        auto restartedInfo = editor->Numbering().GetInstance(restarted.NumberingId);
        REQUIRE(restartedInfo.has_value());
        REQUIRE(restartedInfo->Overrides.size() == 2);
        CHECK(restartedInfo->Overrides[0].Level == 0);
        CHECK(restartedInfo->Overrides[0].Start == 1);
        CHECK(restartedInfo->Overrides[1].Level == 2);
        CHECK(restartedInfo->Overrides[1].Start == 7);

        auto paragraph = editor->AddParagraph("Restarted level");
        REQUIRE(paragraph != nullptr);
        paragraph->SetNumbering(restarted.NumberingId, 2);

        WordDocumentEditor::Ptr reopened;
        auto reopenedParagraph = ReopenParagraph(editor, "Restarted level", reopened);
        REQUIRE(reopenedParagraph != nullptr);
        ParagraphNumbering paragraphNumbering;
        REQUIRE(reopenedParagraph->TryGetNumbering(paragraphNumbering));
        CHECK(paragraphNumbering.NumberingId == restarted.NumberingId);
        CHECK(paragraphNumbering.Level == 2);

        auto reopenedRestart = reopened->Numbering().GetInstance(restarted.NumberingId);
        REQUIRE(reopenedRestart.has_value());
        REQUIRE(reopenedRestart->Overrides.size() == 2);
        CHECK(reopenedRestart->Overrides[1].Level == 2);
        CHECK(reopenedRestart->Overrides[1].Start == 7);
    }

    TEST_CASE("NumberingManager imports a nine-level list into another document with fresh IDs [unit] [word] [word-numbering]")
    {
        auto source = WordDocumentEditor::CreateNew();
        auto target = WordDocumentEditor::CreateNew();
        REQUIRE(source != nullptr);
        REQUIRE(target != nullptr);

        auto sourceStyle = source->Numbering().EnsureMultilevelList(MakeNineLevelOutline("ImportedOutline"));
        REQUIRE(sourceStyle.NumberingId > 0);
        auto sourceRestart = source->Numbering().RestartList(sourceStyle.NumberingId, {{0, 3}});
        REQUIRE(sourceRestart.NumberingId > 0);

        auto imported = target->Numbering().ImportList(source->Numbering(), sourceRestart.NumberingId);
        REQUIRE(imported.NumberingId > 0);
        CHECK(imported.NumberingId != sourceRestart.NumberingId);

        auto paragraph = target->AddParagraph("Imported level");
        REQUIRE(paragraph != nullptr);
        paragraph->SetNumbering(imported.NumberingId, 8);

        auto targetNumbering = NumberingRoot(target);
        REQUIRE(targetNumbering != nullptr);
        auto importedAbstract = AbstractForInstance(targetNumbering, imported.NumberingId);
        REQUIRE(importedAbstract != nullptr);
        CHECK(importedAbstract->Elements<W::Level>().size() == 9);
        auto level8 = FindLevel(importedAbstract, 8);
        REQUIRE(level8 != nullptr);
        auto levelText = level8->GetFirstChildOfType<W::LevelText>();
        REQUIRE(levelText != nullptr);
        CHECK(levelText->GetVal().ToString() == "%9.");

        auto importedInfo = target->Numbering().GetInstance(imported.NumberingId);
        REQUIRE(importedInfo.has_value());
        REQUIRE(importedInfo->Overrides.size() == 1);
        CHECK(importedInfo->Overrides[0].Level == 0);
        CHECK(importedInfo->Overrides[0].Start == 3);

        WordDocumentEditor::Ptr reopened;
        auto reopenedParagraph = ReopenParagraph(target, "Imported level", reopened);
        REQUIRE(reopenedParagraph != nullptr);
        ParagraphNumbering paragraphNumbering;
        REQUIRE(reopenedParagraph->TryGetNumbering(paragraphNumbering));
        CHECK(paragraphNumbering.NumberingId == imported.NumberingId);
        CHECK(paragraphNumbering.Level == 8);

        auto reopenedAbstract = AbstractForInstance(NumberingRoot(reopened), imported.NumberingId);
        REQUIRE(reopenedAbstract != nullptr);
        CHECK(reopenedAbstract->Elements<W::Level>().size() == 9);
    }

} // TEST_SUITE("WordNumberingTests")
