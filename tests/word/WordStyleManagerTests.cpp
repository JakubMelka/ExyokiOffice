// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "doctest.h"

#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/Wordprocessing.hpp"
#include "ExyokiOffice/Packaging/GeneratedParts.hpp"
#include "ExyokiOffice/Word/WordDocument.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <algorithm>
#include <string>

namespace
{
using ExyokiOffice::OpenXmlQualifiedName;
using ExyokiOffice::Word::LatentStyleException;
using ExyokiOffice::Word::LatentStyleSettings;
using ExyokiOffice::Word::StyleCopyConflictPolicy;
using ExyokiOffice::Word::StyleDefinition;
using ExyokiOffice::Word::StyleManager;
using ExyokiOffice::Word::StyleType;
using ExyokiOffice::Word::WordDocumentEditor;

constexpr std::string_view kWordNamespace =
    "http://schemas.openxmlformats.org/wordprocessingml/2006/main";

StyleDefinition MakeStyle(std::string id, std::string name, StyleType type)
{
    StyleDefinition style;
    style.StyleId = std::move(id);
    style.Name = std::move(name);
    style.Type = type;
    style.IsCustom = true;
    return style;
}

ExyokiOffice::Size CountStyleChildren(const StyleManager& manager, std::string_view styleId, std::string_view childName)
{
    auto style = manager.GetLowLevelStyle(styleId);
    if (!style)
    {
        return 0;
    }

    const OpenXmlQualifiedName target(kWordNamespace, childName);
    auto children = style->Children();
    return static_cast<ExyokiOffice::Size>(std::count_if(children.begin(), children.end(),
                                                         [&](const auto& child)
                                                         {
                                                             return child && child->QualifiedName() == target;
                                                         }));
}

StyleManager ReopenStyles(const WordDocumentEditor::Ptr& editor)
{
    auto bytes = editor->SaveToMemory();
    REQUIRE_FALSE(bytes.empty());
    auto reopened = WordDocumentEditor::Open(bytes);
    REQUIRE(reopened != nullptr);
    return reopened->Styles();
}

} // namespace

TEST_SUITE("WordStyleManagerTests")
{

    TEST_CASE("Style manager creates reads updates and removes all style types [unit] [word] [word-style-manager]")
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);

        auto styles = editor->Styles();
        CHECK(styles.IsValid());
        CHECK_FALSE(styles.GetStylesPart());

        auto paragraph = MakeStyle("BodyText", "Body Text", StyleType::Paragraph);
        paragraph.BasedOnStyleId = "Normal";
        paragraph.NextStyleId = "BodyText";
        paragraph.LinkedStyleId = "BodyTextChar";
        paragraph.Aliases = "Body,Text";
        paragraph.UiPriority = 22;
        paragraph.IsPrimary = true;
        paragraph.IsUnhideWhenUsed = true;
        paragraph.IsDefault = true;

        CHECK(styles.CreateStyle(paragraph));
        CHECK_FALSE(styles.CreateStyle(paragraph));
        CHECK(styles.HasStyle("BodyText"));

        CHECK(styles.CreateStyle(MakeStyle("BodyTextChar", "Body Text Char", StyleType::Character)));
        CHECK(styles.CreateStyle(MakeStyle("GridTable", "Grid Table", StyleType::Table)));
        CHECK(styles.CreateStyle(MakeStyle("ListNumber", "List Number", StyleType::Numbering)));

        auto read = styles.GetStyle("BodyText");
        REQUIRE(read.has_value());
        CHECK(read->StyleId == "BodyText");
        CHECK(read->Name == "Body Text");
        CHECK(read->Type == StyleType::Paragraph);
        CHECK(read->BasedOnStyleId == "Normal");
        CHECK(read->NextStyleId == "BodyText");
        CHECK(read->LinkedStyleId == "BodyTextChar");
        CHECK(read->Aliases == "Body,Text");
        CHECK(read->UiPriority == 22);
        CHECK(read->IsPrimary);
        CHECK(read->IsUnhideWhenUsed);
        CHECK(read->IsDefault);

        CHECK(styles.StylesByType(StyleType::Paragraph).size() == 1);
        CHECK(styles.StylesByType(StyleType::Character).size() == 1);
        CHECK(styles.StylesByType(StyleType::Table).size() == 1);
        CHECK(styles.StylesByType(StyleType::Numbering).size() == 1);

        paragraph.Name = "Body Text Updated";
        paragraph.BasedOnStyleId.clear();
        paragraph.LinkedStyleId.clear();
        paragraph.UiPriority.reset();
        paragraph.IsPrimary = false;
        paragraph.IsSemiHidden = true;
        CHECK(styles.UpdateStyle(paragraph));

        auto updated = styles.GetStyle("BodyText");
        REQUIRE(updated.has_value());
        CHECK(updated->Name == "Body Text Updated");
        CHECK(updated->BasedOnStyleId.empty());
        CHECK(updated->LinkedStyleId.empty());
        CHECK_FALSE(updated->UiPriority.has_value());
        CHECK_FALSE(updated->IsPrimary);
        CHECK(updated->IsSemiHidden);

        CHECK(styles.RemoveStyle("GridTable"));
        CHECK_FALSE(styles.HasStyle("GridTable"));
        CHECK_FALSE(styles.RemoveStyle("GridTable"));
    }

    TEST_CASE("Style manager keeps one default style per type and round trips style metadata [unit] [word] [word-style-manager]")
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);
        auto styles = editor->Styles();

        auto normal = MakeStyle("NormalCustom", "Normal Custom", StyleType::Paragraph);
        normal.IsDefault = true;
        CHECK(styles.CreateStyle(normal));

        auto body = MakeStyle("BodyCustom", "Body Custom", StyleType::Paragraph);
        CHECK(styles.CreateStyle(body));
        CHECK(styles.SetDefaultStyle(StyleType::Paragraph, "BodyCustom"));

        auto defaultParagraph = styles.GetDefaultStyle(StyleType::Paragraph);
        REQUIRE(defaultParagraph.has_value());
        CHECK(defaultParagraph->StyleId == "BodyCustom");
        CHECK_FALSE(styles.GetStyle("NormalCustom")->IsDefault);

        CHECK_FALSE(styles.SetDefaultStyle(StyleType::Character, "BodyCustom"));
        CHECK(styles.ClearDefaultStyle(StyleType::Paragraph));
        CHECK_FALSE(styles.GetDefaultStyle(StyleType::Paragraph).has_value());

        body.IsDefault = true;
        body.NextStyleId = "BodyCustom";
        body.Aliases = "Body Alias";
        CHECK(styles.UpdateStyle(body));

        auto reopenedStyles = ReopenStyles(editor);
        auto reopened = reopenedStyles.GetStyle("BodyCustom");
        REQUIRE(reopened.has_value());
        CHECK(reopened->StyleId == "BodyCustom");
        CHECK(reopened->Name == "Body Custom");
        CHECK(reopened->NextStyleId == "BodyCustom");
        CHECK(reopened->Aliases == "Body Alias");
        CHECK(reopened->IsDefault);
    }

    TEST_CASE("Style manager manages latent style defaults and exceptions [unit] [word] [word-style-manager]")
    {
        auto editor = WordDocumentEditor::CreateNew();
        REQUIRE(editor != nullptr);
        auto styles = editor->Styles();

        LatentStyleSettings settings;
        settings.DefaultLocked = false;
        settings.DefaultUiPriority = 99;
        settings.DefaultSemiHidden = true;
        settings.DefaultUnhideWhenUsed = false;
        settings.DefaultPrimaryStyle = true;
        settings.Count = 267;
        CHECK(styles.SetLatentStyleSettings(settings));

        LatentStyleException heading;
        heading.Name = "heading 1";
        heading.Locked = false;
        heading.UiPriority = 9;
        heading.SemiHidden = false;
        heading.UnhideWhenUsed = true;
        heading.PrimaryStyle = true;
        CHECK(styles.SetLatentStyleException(heading));

        LatentStyleException title;
        title.Name = "Title";
        title.UiPriority = 10;
        CHECK(styles.SetLatentStyleException(title));
        CHECK(styles.LatentStyleExceptions().size() == 2);

        auto reopenedStyles = ReopenStyles(editor);
        auto reopenedSettings = reopenedStyles.GetLatentStyleSettings();
        CHECK(reopenedSettings.DefaultLocked == false);
        CHECK(reopenedSettings.DefaultUiPriority == 99);
        CHECK(reopenedSettings.DefaultSemiHidden == true);
        CHECK(reopenedSettings.DefaultUnhideWhenUsed == false);
        CHECK(reopenedSettings.DefaultPrimaryStyle == true);
        CHECK(reopenedSettings.Count == 267);

        auto reopenedHeading = reopenedStyles.GetLatentStyleException("heading 1");
        REQUIRE(reopenedHeading.has_value());
        CHECK(reopenedHeading->UiPriority == 9);
        CHECK(reopenedHeading->UnhideWhenUsed == true);
        CHECK(reopenedHeading->PrimaryStyle == true);

        heading.UiPriority = 7;
        heading.PrimaryStyle = false;
        CHECK(reopenedStyles.SetLatentStyleException(heading));
        CHECK(reopenedStyles.GetLatentStyleException("heading 1")->UiPriority == 7);
        CHECK(reopenedStyles.GetLatentStyleException("heading 1")->PrimaryStyle == false);
        CHECK(reopenedStyles.RemoveLatentStyleException("Title"));
        CHECK_FALSE(reopenedStyles.GetLatentStyleException("Title").has_value());
    }

    TEST_CASE("Style import preserves full style XML and handles conflict policies [unit] [word] [word-style-manager]")
    {
        auto sourceEditor = WordDocumentEditor::CreateNew();
        auto targetEditor = WordDocumentEditor::CreateNew();
        REQUIRE(sourceEditor != nullptr);
        REQUIRE(targetEditor != nullptr);

        auto source = sourceEditor->Styles();
        auto target = targetEditor->Styles();

        auto imported = MakeStyle("SharedStyle", "Shared Style", StyleType::Paragraph);
        imported.IsCustom = true;
        imported.BasedOnStyleId = "Normal";
        CHECK(source.CreateStyle(imported));

        auto lowLevel = source.GetLowLevelStyle("SharedStyle");
        REQUIRE(lowLevel != nullptr);
        CHECK(lowLevel->AppendChild<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::StyleParagraphProperties>() != nullptr);
        CHECK(lowLevel->AppendChild<ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::StyleRunProperties>() != nullptr);

        CHECK(target.ImportStyle(source, "SharedStyle") == "SharedStyle");
        CHECK(target.HasStyle("SharedStyle"));
        CHECK(CountStyleChildren(target, "SharedStyle", "pPr") == 1);
        CHECK(CountStyleChildren(target, "SharedStyle", "rPr") == 1);

        auto conflict = MakeStyle("SharedStyle", "Existing Style", StyleType::Paragraph);
        CHECK(target.CreateStyle(conflict, true));
        CHECK(target.ImportStyle(source, "SharedStyle", StyleCopyConflictPolicy::KeepExisting) == "SharedStyle");
        CHECK(target.GetStyle("SharedStyle")->Name == "Existing Style");

        const auto renamedId = target.ImportStyle(source, "SharedStyle", StyleCopyConflictPolicy::Rename);
        CHECK(renamedId == "SharedStyle_2");
        CHECK(target.HasStyle("SharedStyle_2"));
        CHECK(CountStyleChildren(target, "SharedStyle_2", "pPr") == 1);

        CHECK(target.ImportStyle(source, "SharedStyle", StyleCopyConflictPolicy::Replace) == "SharedStyle");
        CHECK(target.GetStyle("SharedStyle")->Name == "Shared Style");
        CHECK(CountStyleChildren(target, "SharedStyle", "pPr") == 1);

        CHECK(target.ImportStyle(source, "SharedStyle", StyleCopyConflictPolicy::Rename, "Preferred") == "Preferred");
        CHECK(target.HasStyle("Preferred"));

        auto reopenedTarget = ReopenStyles(targetEditor);
        CHECK(reopenedTarget.HasStyle("SharedStyle"));
        CHECK(reopenedTarget.HasStyle("SharedStyle_2"));
        CHECK(reopenedTarget.HasStyle("Preferred"));
        CHECK(CountStyleChildren(reopenedTarget, "SharedStyle", "rPr") == 1);

        auto reopenedLowLevel = reopenedTarget.GetLowLevelStyle("SharedStyle");
        REQUIRE(reopenedLowLevel != nullptr);
        CHECK(reopenedLowLevel->GetFirstChildOfType<
                  ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::StyleParagraphProperties>() != nullptr);
        CHECK(reopenedLowLevel->GetFirstChildOfType<
                  ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing::StyleRunProperties>() != nullptr);
    }

    TEST_CASE("Style import keeps the target styles tree alive [unit] [word] [word-style-manager]")
    {
        auto sourceEditor = WordDocumentEditor::CreateNew();
        auto targetEditor = WordDocumentEditor::CreateNew();
        REQUIRE(sourceEditor != nullptr);
        REQUIRE(targetEditor != nullptr);

        auto source = sourceEditor->Styles();
        auto target = targetEditor->Styles();
        CHECK(source.CreateStyle(MakeStyle("Imported", "Imported Style", StyleType::Paragraph)));
        CHECK(target.CreateStyle(MakeStyle("Untouched", "Untouched Style", StyleType::Paragraph)));

        // Wrappers into the target styles part, taken before the import. The
        // import must edit that live tree instead of reparsing the whole part,
        // which would leave these dangling.
        auto stylesRoot = target.GetRoot();
        auto untouched = target.GetLowLevelStyle("Untouched");
        REQUIRE(stylesRoot != nullptr);
        REQUIRE(untouched != nullptr);

        CHECK(target.ImportStyle(source, "Imported") == "Imported");

        CHECK_FALSE(stylesRoot->IsNull());
        CHECK_FALSE(untouched->IsNull());
        CHECK(untouched->Parent()->IsSameNode(*stylesRoot));
        CHECK(target.GetLowLevelStyle("Imported")->Parent()->IsSameNode(*stylesRoot));
    }

    TEST_CASE("Replacing a style keeps its position in the part [unit] [word] [word-style-manager]")
    {
        auto sourceEditor = WordDocumentEditor::CreateNew();
        auto targetEditor = WordDocumentEditor::CreateNew();
        REQUIRE(sourceEditor != nullptr);
        REQUIRE(targetEditor != nullptr);

        auto source = sourceEditor->Styles();
        auto target = targetEditor->Styles();
        CHECK(source.CreateStyle(MakeStyle("Replaced", "From Source", StyleType::Paragraph)));
        CHECK(target.CreateStyle(MakeStyle("Replaced", "From Target", StyleType::Paragraph)));
        CHECK(target.CreateStyle(MakeStyle("Trailing", "Trailing Style", StyleType::Paragraph)));

        const auto positionOf = [](const StyleManager& manager, std::string_view styleId)
        {
            auto root = manager.GetRoot();
            REQUIRE(root != nullptr);
            const auto children = root->Children();
            for (ExyokiOffice::Size index = 0; index < children.size(); ++index)
            {
                if (children[index]->IsSameNode(*manager.GetLowLevelStyle(styleId)))
                {
                    return index;
                }
            }
            return children.size();
        };

        const auto before = positionOf(target, "Replaced");
        CHECK(target.ImportStyle(source, "Replaced", StyleCopyConflictPolicy::Replace) == "Replaced");
        CHECK(target.GetStyle("Replaced")->Name == "From Source");
        CHECK(positionOf(target, "Replaced") == before);
        CHECK(positionOf(target, "Trailing") > before);
    }

} // TEST_SUITE("WordStyleManagerTests")
