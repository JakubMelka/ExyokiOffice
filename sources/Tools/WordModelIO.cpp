// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "ExyokiOffice/Tools/DocumentModelIO.hpp"

#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/Drawing.hpp"
#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/Wordprocessing.hpp"
#include "ExyokiOffice/Tools/PackageInspector.hpp"
#include "ExyokiOffice/Tools/PackageLimits.hpp"
#include "ExyokiOffice/Word/WordDocument.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include "AsciiText.hpp"

#include <charconv>
#include <cmath>
#include <ctime>
#include <map>
#include <regex>
#include <set>

namespace ExyokiOffice::Tools
{

namespace W = ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing;
namespace A = ExyokiOffice::DocumentFormat::OpenXml::Drawing;
using ExyokiOffice::EnumValue;

/// File-local helpers for the Word document model round trip.
class WordModelIoHelper
{
public:
    static void Warn(std::vector<ToolDiagnostic>& diagnostics, std::string message, std::string context = {})
    {
        diagnostics.push_back(ToolDiagnostic{ToolSeverity::Warning, std::move(message), std::move(context)});
    }

    static void Info(std::vector<ToolDiagnostic>& diagnostics, std::string message, std::string context = {})
    {
        diagnostics.push_back(ToolDiagnostic{ToolSeverity::Info, std::move(message), std::move(context)});
    }

    static int HeadingLevelFromStyleId(const std::string& styleId)
    {
        static const std::regex pattern("^Heading([1-9])$");
        std::smatch match;
        if (std::regex_match(styleId, match, pattern))
        {
            // The pattern already limited the capture to one digit, so this is
            // arithmetic on a known character rather than a parse - and it
            // cannot throw, which std::stoi can.
            return match[1].str().front() - '0';
        }
        return 0;
    }

    /// A `w:id` attribute as a number, or 0 when it is not one. std::strtol
    /// would answer 0 for rubbish as well but would also accept a numeric
    /// prefix, and it reads its digits through the global C locale.
    static int ParseCommentId(std::string_view text)
    {
        int parsed = 0;
        const auto* const end = text.data() + text.size();
        const auto result = std::from_chars(text.data(), end, parsed);
        return (result.ec == std::errc{} && result.ptr == end) ? parsed : 0;
    }

    template <typename TEnum>
    static std::string EnumToken(const std::optional<TEnum>& value)
    {
        return value ? EnumValue<TEnum>(*value).ToString() : std::string();
    }

    template <typename TEnum>
    static std::optional<TEnum> TokenToEnum(const std::string& token)
    {
        if (token.empty())
        {
            return std::nullopt;
        }
        EnumValue<TEnum> value;
        if (!value.AssignFromString(token) || !value.IsDefined())
        {
            return std::nullopt;
        }
        return value.Value();
    }

    // ---------------------------------------------------------------------------
    // Reader
    // ---------------------------------------------------------------------------

    class WordModelReader
    {
    public:
        WordModelReader(Word::WordDocumentEditor& editor, const ModelReadOptions& options, DocumentModel& model,
                        std::vector<ToolDiagnostic>& diagnostics)
            : m_editor(editor), m_options(options), m_model(model), m_diagnostics(diagnostics)
        {
            m_mainPart = editor.GetDocument() ? editor.GetDocument()->GetMainDocumentPart() : nullptr;
        }

        void Read()
        {
            auto& word = m_model.Word.emplace();

            Size blockIndex = 1;
            for (const auto& block : m_editor.BodyBlocks())
            {
                ReadBodyBlock(block, word.Body, "body block " + std::to_string(blockIndex));
                ++blockIndex;
            }

            ReadNotes(word);
            ReadComments(word);
            ReadHeadersFooters(word);
            ReadListDefinitions(word);
        }

    private:
        void ReadBodyBlock(const Word::BodyBlock& block, std::vector<WordBlock>& out, const std::string& context)
        {
            switch (block.Type())
            {
                case Word::BodyBlockType::Paragraph:
                {
                    if (auto paragraph = block.AsParagraph())
                    {
                        AppendParagraphOrSectionBreak(paragraph->GetLowLevelApi(), out);
                    }
                    break;
                }
                case Word::BodyBlockType::Table:
                {
                    if (auto table = block.AsTable())
                    {
                        WordBlock modelBlock;
                        modelBlock.Kind = WordBlock::Type::Table;
                        modelBlock.Table = ReadTable(*table);
                        out.push_back(std::move(modelBlock));
                    }
                    break;
                }
                case Word::BodyBlockType::Section:
                {
                    // The final body-level section is metadata, not content; headers
                    // and footers are read separately from the section wrappers.
                    break;
                }
                case Word::BodyBlockType::ContentControl:
                {
                    if (auto control = block.AsContentControl())
                    {
                        Info(m_diagnostics, "Content control flattened to plain content", context);
                        for (const auto& paragraph : control->Paragraphs())
                        {
                            if (paragraph)
                            {
                                AppendParagraphOrSectionBreak(paragraph->GetLowLevelApi(), out);
                            }
                        }
                    }
                    break;
                }
                case Word::BodyBlockType::Unsupported:
                {
                    Warn(m_diagnostics, "Unsupported body content skipped", context);
                    break;
                }
            }
        }

        void AppendParagraphOrSectionBreak(const std::shared_ptr<W::Paragraph>& lowParagraph,
                                           std::vector<WordBlock>& out)
        {
            if (!lowParagraph)
            {
                return;
            }
            if (auto properties = lowParagraph->GetFirstChildOfType<W::ParagraphProperties>();
                properties && properties->GetFirstChildOfType<W::SectionProperties>())
            {
                WordBlock modelBlock;
                modelBlock.Kind = WordBlock::Type::SectionBreak;
                out.push_back(std::move(modelBlock));
                return;
            }

            WordBlock modelBlock;
            modelBlock.Kind = WordBlock::Type::Paragraph;
            modelBlock.Paragraph = ReadParagraph(lowParagraph);
            out.push_back(std::move(modelBlock));
        }

        WordParagraph ReadParagraph(const std::shared_ptr<W::Paragraph>& lowParagraph)
        {
            Word::Paragraph wrapper(lowParagraph, m_mainPart);
            WordParagraph paragraph;
            paragraph.StyleId = wrapper.GetStyleId();
            paragraph.HeadingLevel = HeadingLevelFromStyleId(paragraph.StyleId);
            paragraph.Alignment = EnumToken(wrapper.GetAlignment());

            Word::ParagraphNumbering numbering;
            if (wrapper.TryGetNumbering(numbering) && numbering.NumberingId)
            {
                WordListRef reference;
                reference.NumberingId = *numbering.NumberingId;
                reference.Level = numbering.Level.value_or(0);
                paragraph.List = reference;
                m_usedNumberingIds.insert(reference.NumberingId);
            }

            ReadInlineContent(lowParagraph, paragraph.Inlines);
            return paragraph;
        }

        /// Walks the ordered children of a paragraph (or hyperlink) and appends inlines.
        void ReadInlineContent(const std::shared_ptr<ExyokiOffice::OpenXMLElement>& container,
                               std::vector<WordInline>& out)
        {
            enum class FieldState
            {
                None,
                Instruction,
                Result
            };
            FieldState fieldState = FieldState::None;
            std::string fieldInstruction;
            std::string fieldResult;

            const auto emitField = [&]()
            {
                WordInline node;
                node.Kind = WordInline::Type::Field;
                node.FieldCode = TrimCopy(fieldInstruction);
                node.Text = fieldResult;
                out.push_back(std::move(node));
                fieldState = FieldState::None;
                fieldInstruction.clear();
                fieldResult.clear();
            };

            for (const auto& child : container->Children())
            {
                if (auto lowRun = openxmlelement_cast<W::Run>(child))
                {
                    const auto fieldChar = lowRun->GetFirstChildOfType<W::FieldChar>();
                    if (fieldState != FieldState::None)
                    {
                        if (fieldChar && fieldChar->GetFieldCharType().IsDefined())
                        {
                            // By value: GetFieldCharType() returns a temporary and
                            // Value() a reference into it, which a reference here
                            // would outlive.
                            const auto type = fieldChar->GetFieldCharType().Value();
                            if (type == W::FieldCharValues::Separate)
                            {
                                fieldState = FieldState::Result;
                            }
                            else if (type == W::FieldCharValues::End)
                            {
                                emitField();
                            }
                            continue;
                        }
                        if (fieldState == FieldState::Instruction)
                        {
                            for (const auto& code : lowRun->Elements<W::FieldCode>())
                            {
                                fieldInstruction += code->GetText();
                            }
                        }
                        else
                        {
                            Word::Run runWrapper(lowRun);
                            fieldResult += runWrapper.PlainText();
                        }
                        continue;
                    }
                    if (fieldChar && fieldChar->GetFieldCharType().IsDefined() &&
                        fieldChar->GetFieldCharType().Value() == W::FieldCharValues::Begin)
                    {
                        fieldState = FieldState::Instruction;
                        continue;
                    }
                    ReadRunContent(lowRun, out);
                    continue;
                }

                // "w:hyperlink" is an ambiguous element QName (run-level vs ruby hyperlink),
                // so the factory-resolved type cannot be trusted; match by name like
                // Word::Paragraph::Hyperlinks() does.
                static const ExyokiOffice::OpenXmlQualifiedName hyperlinkName(
                    "http://schemas.openxmlformats.org/wordprocessingml/2006/main", "hyperlink");
                if (child && child->QualifiedName() == hyperlinkName)
                {
                    auto lowHyperlink = std::static_pointer_cast<W::Hyperlink>(child);
                    Word::Hyperlink wrapper(lowHyperlink, m_mainPart);
                    WordInline node;
                    node.Kind = WordInline::Type::Hyperlink;
                    node.Target = wrapper.GetUrl();
                    node.Anchor = wrapper.GetAnchor();
                    node.Tooltip = wrapper.GetTooltip();
                    ReadInlineContent(lowHyperlink, node.Children);
                    out.push_back(std::move(node));
                    continue;
                }

                if (auto simpleField = openxmlelement_cast<W::SimpleField>(child))
                {
                    WordInline node;
                    node.Kind = WordInline::Type::Field;
                    node.FieldCode = TrimCopy(simpleField->GetInstruction().IsDefined()
                                                  ? simpleField->GetInstruction().ToString()
                                                  : std::string());
                    for (const auto& text : simpleField->Descendants<W::Text>())
                    {
                        node.Text += text->GetText();
                    }
                    out.push_back(std::move(node));
                    continue;
                }

                if (auto sdt = openxmlelement_cast<W::SdtRun>(child))
                {
                    Info(m_diagnostics, "Inline content control flattened to plain content");
                    if (auto content = sdt->GetFirstChildOfType<W::SdtContentRun>())
                    {
                        ReadInlineContent(content, out);
                    }
                    continue;
                }

                // Bookmarks, proofing marks, and other non-content children are skipped.
            }

            if (fieldState != FieldState::None)
            {
                emitField();
            }
        }

        void ReadRunContent(const std::shared_ptr<W::Run>& lowRun, std::vector<WordInline>& out)
        {
            Word::Run wrapper(lowRun);
            WordRunProps props = ReadRunProps(wrapper);

            std::string pendingText;
            const auto flushText = [&]()
            {
                if (pendingText.empty())
                {
                    return;
                }
                WordInline node;
                node.Kind = WordInline::Type::Text;
                node.Text = std::move(pendingText);
                node.Props = props;
                pendingText.clear();
                out.push_back(std::move(node));
            };

            for (const auto& child : lowRun->Children())
            {
                if (auto text = openxmlelement_cast<W::Text>(child))
                {
                    pendingText += text->GetText();
                    continue;
                }
                if (openxmlelement_cast<W::TabChar>(child))
                {
                    pendingText += '\t';
                    continue;
                }
                if (auto lineBreak = openxmlelement_cast<W::Break>(child))
                {
                    flushText();
                    WordInline node;
                    node.Kind = WordInline::Type::Break;
                    node.BreakKind = "line";
                    if (lineBreak->GetType().IsDefined())
                    {
                        const auto token = lineBreak->GetType().ToString();
                        if (token == "page" || token == "column")
                        {
                            node.BreakKind = token;
                        }
                    }
                    out.push_back(std::move(node));
                    continue;
                }
                if (auto drawing = openxmlelement_cast<W::Drawing>(child))
                {
                    flushText();
                    ReadImage(drawing, out);
                    continue;
                }
                if (auto footnoteRef = openxmlelement_cast<W::FootnoteReference>(child))
                {
                    flushText();
                    WordInline node;
                    node.Kind = WordInline::Type::FootnoteRef;
                    node.NoteId = footnoteRef->GetId().IsDefined()
                                      ? static_cast<int>(footnoteRef->GetId().Value())
                                      : 0;
                    out.push_back(std::move(node));
                    continue;
                }
                if (auto endnoteRef = openxmlelement_cast<W::EndnoteReference>(child))
                {
                    flushText();
                    WordInline node;
                    node.Kind = WordInline::Type::EndnoteRef;
                    node.NoteId = endnoteRef->GetId().IsDefined()
                                      ? static_cast<int>(endnoteRef->GetId().Value())
                                      : 0;
                    out.push_back(std::move(node));
                    continue;
                }
                if (auto commentRef = openxmlelement_cast<W::CommentReference>(child))
                {
                    flushText();
                    WordInline node;
                    node.Kind = WordInline::Type::CommentRef;
                    node.NoteId = commentRef->GetId().IsDefined() ? ParseCommentId(commentRef->GetId().ToString()) : 0;
                    out.push_back(std::move(node));
                    continue;
                }
                // Run properties and other non-content children are skipped.
            }
            flushText();
        }

        static WordRunProps ReadRunProps(Word::Run& run)
        {
            WordRunProps props;
            props.Bold = run.GetBold().value_or(false);
            props.Italic = run.GetItalic().value_or(false);
            if (const auto underline = run.GetUnderline())
            {
                props.Underline = *underline != W::UnderlineValues::None;
            }
            props.Strike = run.GetStrike().value_or(false);
            props.Caps = run.GetCaps().value_or(false);
            props.SmallCaps = run.GetSmallCaps().value_or(false);
            props.Color = run.GetColor();
            if (props.Color == "auto")
            {
                props.Color.clear();
            }
            props.Highlight = EnumToken(run.GetHighlight());
            if (const auto fonts = run.GetFont())
            {
                props.Font = fonts->Ascii;
            }
            props.StyleId = run.GetStyleId();
            if (const auto size = run.GetFontSize())
            {
                props.FontSizePt = size->ToPt().GetValue();
            }
            return props;
        }

        void ReadImage(const std::shared_ptr<W::Drawing>& drawing, std::vector<WordInline>& out)
        {
            Word::Image wrapper(drawing, m_mainPart);
            WordInline node;
            node.Kind = WordInline::Type::Image;
            node.AltText = wrapper.GetDescription();
            if (node.AltText.empty())
            {
                node.AltText = wrapper.GetTitle();
            }
            Word::ImageSize size;
            if (wrapper.TryGetSize(size))
            {
                node.WidthEmu = static_cast<Int64>(std::llround(size.Width.ToEmu().GetValue()));
                node.HeightEmu = static_cast<Int64>(std::llround(size.Height.ToEmu().GetValue()));
            }

            std::string relationshipId;
            for (const auto& blip : drawing->Descendants<A::Blip>())
            {
                if (blip->GetEmbed().IsDefined())
                {
                    relationshipId = blip->GetEmbed().ToString();
                    break;
                }
            }
            node.MediaId = EnsureMedia(relationshipId);
            if (node.MediaId.empty())
            {
                Warn(m_diagnostics, "Image payload not found; image exported without media", relationshipId);
            }
            out.push_back(std::move(node));
        }

        std::string EnsureMedia(const std::string& relationshipId)
        {
            if (relationshipId.empty() || !m_mainPart)
            {
                return {};
            }
            if (const auto existing = m_mediaByRelationshipId.find(relationshipId);
                existing != m_mediaByRelationshipId.end())
            {
                return existing->second;
            }
            for (const auto& imagePart : m_mainPart->GetImageParts())
            {
                if (!imagePart || imagePart->RelationshipId() != relationshipId)
                {
                    continue;
                }
                MediaReference media;
                media.Id = "media" + std::to_string(m_model.Media.size() + 1);
                media.ContentType = std::string(imagePart->ContentType());
                if (m_options.IncludeMediaData)
                {
                    media.Data = imagePart->GetBinaryData();
                }
                m_mediaByRelationshipId.emplace(relationshipId, media.Id);
                m_model.Media.push_back(std::move(media));
                return m_model.Media.back().Id;
            }
            return {};
        }

        WordTable ReadTable(Word::Table& table)
        {
            WordTable modelTable;
            if (auto lowTable = table.GetLowLevelApi())
            {
                if (auto properties = lowTable->GetFirstChildOfType<W::TableProperties>())
                {
                    if (auto style = properties->GetFirstChildOfType<W::TableStyle>();
                        style && style->GetVal().IsDefined())
                    {
                        modelTable.StyleId = style->GetVal().ToString();
                    }
                }
            }

            const auto grid = table.GetLogicalGrid();
            for (const auto& gridRow : grid)
            {
                WordTableRow modelRow;
                for (const auto& gridCell : gridRow)
                {
                    WordTableCell modelCell;
                    if (!gridCell.IsOrigin)
                    {
                        modelCell.Covered = true;
                    }
                    else
                    {
                        modelCell.RowSpan = static_cast<int>(gridCell.RowSpan);
                        modelCell.ColSpan = static_cast<int>(gridCell.ColumnSpan);
                        if (gridCell.Cell)
                        {
                            for (const auto& child : gridCell.Cell->Children())
                            {
                                if (auto paragraph = openxmlelement_cast<W::Paragraph>(child))
                                {
                                    AppendParagraphOrSectionBreak(paragraph, modelCell.Blocks);
                                }
                                else if (auto nested = openxmlelement_cast<W::Table>(child))
                                {
                                    Word::Table nestedWrapper(nested);
                                    WordBlock nestedBlock;
                                    nestedBlock.Kind = WordBlock::Type::Table;
                                    nestedBlock.Table = ReadTable(nestedWrapper);
                                    modelCell.Blocks.push_back(std::move(nestedBlock));
                                }
                            }
                        }
                    }
                    modelRow.Cells.push_back(std::move(modelCell));
                }
                modelTable.Rows.push_back(std::move(modelRow));
            }
            return modelTable;
        }

        void ReadNotes(WordDocumentModel& word)
        {
            const auto readNoteList = [&](const std::vector<std::shared_ptr<Word::Note>>& notes,
                                          std::vector<WordNote>& out)
            {
                for (const auto& note : notes)
                {
                    if (!note || note->GetEntryType() != Word::NoteEntryType::Normal)
                    {
                        continue;
                    }
                    WordNote modelNote;
                    modelNote.Id = note->GetId();
                    for (const auto& paragraph : note->Paragraphs())
                    {
                        if (paragraph)
                        {
                            AppendParagraphOrSectionBreak(paragraph->GetLowLevelApi(), modelNote.Blocks);
                        }
                    }
                    out.push_back(std::move(modelNote));
                }
            };
            readNoteList(m_editor.Footnotes(), word.Footnotes);
            readNoteList(m_editor.Endnotes(), word.Endnotes);
        }

        void ReadComments(WordDocumentModel& word)
        {
            for (const auto& comment : m_editor.Comments())
            {
                if (!comment)
                {
                    continue;
                }
                WordComment modelComment;
                modelComment.Id = comment->GetId();
                modelComment.Author = comment->GetAuthor();
                modelComment.Initials = comment->GetInitials();
                if (const auto date = comment->GetDate())
                {
                    modelComment.Date = FormatIsoTimestamp(*date);
                }
                for (const auto& paragraph : comment->Paragraphs())
                {
                    if (paragraph)
                    {
                        AppendParagraphOrSectionBreak(paragraph->GetLowLevelApi(), modelComment.Blocks);
                    }
                }
                word.Comments.push_back(std::move(modelComment));
            }
        }

        void ReadHeadersFooters(WordDocumentModel& word)
        {
            std::shared_ptr<Word::Section> finalSection;
            for (const auto& section : m_editor.Sections())
            {
                if (section && section->IsFinalBodySection())
                {
                    finalSection = section;
                }
            }
            if (!finalSection)
            {
                return;
            }

            static constexpr std::pair<Word::HeaderFooterType, const char*> kinds[] = {
                {Word::HeaderFooterType::Default, "default"},
                {Word::HeaderFooterType::First, "first"},
                {Word::HeaderFooterType::Even, "even"},
            };
            for (const auto& [type, kind] : kinds)
            {
                const auto readContent = [&](const std::shared_ptr<Word::HeaderFooterContent>& content,
                                             std::vector<WordHeaderFooter>& out)
                {
                    if (!content)
                    {
                        return;
                    }
                    WordHeaderFooter entry;
                    entry.Kind = kind;
                    for (const auto& paragraph : content->Paragraphs())
                    {
                        if (paragraph)
                        {
                            AppendParagraphOrSectionBreak(paragraph->GetLowLevelApi(), entry.Blocks);
                        }
                    }
                    out.push_back(std::move(entry));
                };
                if (finalSection->HasHeader(type))
                {
                    readContent(finalSection->GetHeader(type), word.Headers);
                }
                if (finalSection->HasFooter(type))
                {
                    readContent(finalSection->GetFooter(type), word.Footers);
                }
            }
        }

        void ReadListDefinitions(WordDocumentModel& word)
        {
            if (m_usedNumberingIds.empty())
            {
                return;
            }
            auto numbering = m_editor.Numbering();
            for (const int numberingId : m_usedNumberingIds)
            {
                const auto instance = numbering.GetInstance(numberingId);
                if (!instance)
                {
                    Warn(m_diagnostics, "Paragraph references an unknown numbering instance",
                         std::to_string(numberingId));
                    continue;
                }
                const auto definition = numbering.GetDefinition(instance->AbstractNumberingId);
                WordListDefinition modelDefinition;
                modelDefinition.NumberingId = numberingId;
                if (definition)
                {
                    for (const auto& level : definition->Levels)
                    {
                        WordListLevel modelLevel;
                        modelLevel.Format = EnumValue<W::NumberFormatValues>(level.Format).ToString();
                        modelLevel.LevelText = level.LevelText;
                        modelLevel.Start = level.Start;
                        modelDefinition.Levels.push_back(std::move(modelLevel));
                    }
                }
                word.Lists.push_back(std::move(modelDefinition));
            }
        }

        static std::string TrimCopy(const std::string& text)
        {
            return std::string(AsciiText::Trim(text));
        }

        static std::string FormatIsoTimestamp(std::chrono::system_clock::time_point timePoint)
        {
            const std::time_t time = std::chrono::system_clock::to_time_t(timePoint);
            std::tm utc{};
#if defined(_WIN32)
            gmtime_s(&utc, &time);
#else
            gmtime_r(&time, &utc);
#endif
            char buffer[32] = {};
            std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &utc);
            return buffer;
        }

        Word::WordDocumentEditor& m_editor;
        std::shared_ptr<Packaging::MainDocumentPart> m_mainPart;
        ModelReadOptions m_options;
        DocumentModel& m_model;
        std::vector<ToolDiagnostic>& m_diagnostics;
        std::map<std::string, std::string> m_mediaByRelationshipId;
        std::set<int> m_usedNumberingIds;
    };

    // ---------------------------------------------------------------------------
    // Writer
    // ---------------------------------------------------------------------------

    class WordModelWriter
    {
    public:
        WordModelWriter(const DocumentModel& model, std::vector<ToolDiagnostic>& diagnostics)
            : m_model(model), m_diagnostics(diagnostics)
        {
        }

        bool Write(const std::filesystem::path& path)
        {
            m_editor = Word::WordDocumentEditor::CreateNew();
            if (!m_editor || !m_model.Word)
            {
                m_diagnostics.push_back(
                    ToolDiagnostic{ToolSeverity::Error, "Cannot create Word document from model"});
                return false;
            }
            const auto& word = *m_model.Word;
            m_word = &word;

            RebuildListDefinitions(word);
            WriteBodyBlocks(word.Body);
            WriteHeadersFooters(word);
            WriteProperties();

            if (!m_editor->SaveToFile(path))
            {
                m_diagnostics.push_back(
                    ToolDiagnostic{ToolSeverity::Error, "Failed to save Word document", path.string()});
                return false;
            }
            return true;
        }

    private:
        void RebuildListDefinitions(const WordDocumentModel& word)
        {
            for (const auto& definition : word.Lists)
            {
                Word::NumberingDefinition target;
                target.Name = "exyoki-list-" + std::to_string(definition.NumberingId);
                int levelIndex = 0;
                for (const auto& level : definition.Levels)
                {
                    Word::NumberingLevelDefinition targetLevel;
                    targetLevel.Level = levelIndex++;
                    targetLevel.Start = level.Start;
                    if (const auto format = TokenToEnum<W::NumberFormatValues>(level.Format))
                    {
                        targetLevel.Format = *format;
                    }
                    targetLevel.LevelText = level.LevelText;
                    target.Levels.push_back(std::move(targetLevel));
                }
                const auto style = m_editor->Numbering().EnsureMultilevelList(target);
                if (style.NumberingId != 0)
                {
                    m_numberingRemap[definition.NumberingId] = style.NumberingId;
                }
            }
        }

        int RemapNumberingId(int modelNumberingId)
        {
            if (const auto existing = m_numberingRemap.find(modelNumberingId);
                existing != m_numberingRemap.end())
            {
                return existing->second;
            }
            // Fallback for references without a stored definition.
            Word::NumberingDefinition definition;
            definition.Name = "exyoki-list-" + std::to_string(modelNumberingId);
            const auto style = m_editor->Numbering().EnsureMultilevelList(definition);
            m_numberingRemap[modelNumberingId] = style.NumberingId;
            return style.NumberingId;
        }

        void WriteBodyBlocks(const std::vector<WordBlock>& blocks)
        {
            for (const auto& block : blocks)
            {
                switch (block.Kind)
                {
                    case WordBlock::Type::Paragraph:
                    {
                        if (block.Paragraph)
                        {
                            WriteBodyParagraph(*block.Paragraph);
                        }
                        break;
                    }
                    case WordBlock::Type::Table:
                    {
                        if (block.Table)
                        {
                            WriteTable(*block.Table);
                        }
                        break;
                    }
                    case WordBlock::Type::SectionBreak:
                    {
                        m_editor->Body().InsertSectionBreak();
                        break;
                    }
                }
            }
        }

        void WriteBodyParagraph(const WordParagraph& paragraph)
        {
            // Standalone images become their own body paragraph via the editor API,
            // so split the paragraph's inlines around images.
            auto target = CreateBodyParagraph(paragraph);
            for (const auto& node : paragraph.Inlines)
            {
                if (node.Kind == WordInline::Type::Image)
                {
                    WriteImage(node);
                    target = nullptr;
                    continue;
                }
                if (!target)
                {
                    target = CreateBodyParagraph(paragraph);
                }
                WriteInline(node, *target);
            }
        }

        std::shared_ptr<Word::Paragraph> CreateBodyParagraph(const WordParagraph& paragraph)
        {
            std::shared_ptr<Word::Paragraph> target;
            if (paragraph.HeadingLevel > 0)
            {
                // AddHeading ensures the Heading<N> style definition exists.
                target = m_editor->AddHeading({}, paragraph.HeadingLevel);
            }
            else
            {
                target = m_editor->AddParagraph();
            }
            if (!target)
            {
                return nullptr;
            }
            if (!paragraph.StyleId.empty() && HeadingLevelFromStyleId(paragraph.StyleId) == 0)
            {
                target->SetStyleId(paragraph.StyleId);
            }
            if (const auto alignment = TokenToEnum<W::JustificationValues>(paragraph.Alignment))
            {
                target->SetAlignment(*alignment);
            }
            if (paragraph.List)
            {
                const int numberingId = RemapNumberingId(paragraph.List->NumberingId);
                if (numberingId != 0)
                {
                    target->SetNumbering(numberingId, paragraph.List->Level);
                }
            }
            return target;
        }

        void WriteInline(const WordInline& node, Word::Paragraph& target)
        {
            switch (node.Kind)
            {
                case WordInline::Type::Text:
                {
                    target.AddRun(node.Text, RunStyleFromProps(node.Props), true);
                    break;
                }
                case WordInline::Type::Hyperlink:
                {
                    std::shared_ptr<Word::Hyperlink> hyperlink;
                    if (!node.Target.empty())
                    {
                        hyperlink = target.AddHyperlink({}, node.Target, node.Tooltip);
                    }
                    else if (!node.Anchor.empty())
                    {
                        hyperlink = target.AddInternalHyperlink({}, node.Anchor, node.Tooltip);
                    }
                    if (!hyperlink)
                    {
                        Warn(m_diagnostics, "Hyperlink could not be created; text kept without link",
                             node.Target);
                        for (const auto& child : node.Children)
                        {
                            WriteInline(child, target);
                        }
                        break;
                    }
                    for (const auto& child : node.Children)
                    {
                        if (child.Kind == WordInline::Type::Text)
                        {
                            // Hyperlink content is restricted to formatted text runs.
                            auto run = hyperlink->AddRun();
                            if (run)
                            {
                                run->AddText(child.Text, true);
                                ApplyRunProps(*run, child.Props);
                            }
                        }
                        else
                        {
                            Warn(m_diagnostics, "Non-text hyperlink content dropped");
                        }
                    }
                    break;
                }
                case WordInline::Type::Image:
                {
                    // Handled by WriteBodyParagraph for body content; inside other
                    // containers (notes, cells) images are not supported.
                    Warn(m_diagnostics, "Image inside nested content dropped", node.MediaId);
                    break;
                }
                case WordInline::Type::FootnoteRef:
                {
                    WriteNoteReference(node, target, true);
                    break;
                }
                case WordInline::Type::EndnoteRef:
                {
                    WriteNoteReference(node, target, false);
                    break;
                }
                case WordInline::Type::CommentRef:
                {
                    WriteCommentReference(node, target);
                    break;
                }
                case WordInline::Type::Field:
                {
                    if (!node.FieldCode.empty())
                    {
                        target.AddSimpleField(node.FieldCode, node.Text);
                    }
                    break;
                }
                case WordInline::Type::Break:
                {
                    Word::BreakType type = Word::BreakType::Line;
                    if (node.BreakKind == "page")
                    {
                        type = Word::BreakType::Page;
                    }
                    else if (node.BreakKind == "column")
                    {
                        type = Word::BreakType::Column;
                    }
                    target.AddBreak(type);
                    break;
                }
            }
        }

        static Word::RunStyle RunStyleFromProps(const WordRunProps& props)
        {
            Word::RunStyle style;
            style.Bold = props.Bold;
            style.Italic = props.Italic;
            style.Underline = props.Underline;
            style.Strike = props.Strike;
            style.Caps = props.Caps;
            style.SmallCaps = props.SmallCaps;
            if (const auto highlight = TokenToEnum<W::HighlightColorValues>(props.Highlight))
            {
                style.Highlight = *highlight;
            }
            style.AsciiFont = props.Font;
            style.StyleId = props.StyleId;
            if (props.FontSizePt > 0.0)
            {
                style.FontSize = MeasuringUnits(props.FontSizePt, MeasurementUnit::Point);
            }
            if (!props.Color.empty())
            {
                if (const auto color = Color::FromHexString(props.Color))
                {
                    style.Color = *color;
                }
            }
            return style;
        }

        void ApplyRunProps(Word::Run& run, const WordRunProps& props)
        {
            if (props.Bold)
            {
                run.SetBold(true);
            }
            if (props.Italic)
            {
                run.SetItalic(true);
            }
            if (props.Underline)
            {
                run.SetUnderline(true);
            }
            if (props.Strike)
            {
                run.SetStrike(true);
            }
            if (props.Caps)
            {
                run.SetCaps(true);
            }
            if (props.SmallCaps)
            {
                run.SetSmallCaps(true);
            }
            if (const auto highlight = TokenToEnum<W::HighlightColorValues>(props.Highlight))
            {
                run.SetHighlight(*highlight);
            }
            if (!props.Font.empty())
            {
                run.SetFont(props.Font);
            }
            if (!props.StyleId.empty())
            {
                run.SetStyleId(props.StyleId);
            }
            if (props.FontSizePt > 0.0)
            {
                run.SetFontSize(MeasuringUnits(props.FontSizePt, MeasurementUnit::Point));
            }
            if (!props.Color.empty())
            {
                if (const auto color = Color::FromHexString(props.Color))
                {
                    run.SetColor(*color);
                }
            }
        }

        void WriteImage(const WordInline& node)
        {
            const auto* media = FindMedia(node.MediaId);
            if (media == nullptr || media->Data.empty())
            {
                Warn(m_diagnostics, "Image media payload missing; image skipped", node.MediaId);
                return;
            }
            std::shared_ptr<Word::Image> image;
            if (node.WidthEmu > 0 && node.HeightEmu > 0)
            {
                image = m_editor->AddImageFromData(media->Data, media->ContentType,
                                                   MeasuringUnits(static_cast<Real>(node.WidthEmu),
                                                                  MeasurementUnit::Emu),
                                                   MeasuringUnits(static_cast<Real>(node.HeightEmu),
                                                                  MeasurementUnit::Emu));
            }
            else
            {
                image = m_editor->AddImageFromData(media->Data);
            }
            if (!image)
            {
                Warn(m_diagnostics, "Image could not be inserted", node.MediaId);
                return;
            }
            if (!node.AltText.empty())
            {
                image->SetAltText(node.AltText, node.AltText);
            }
        }

        const MediaReference* FindMedia(const std::string& mediaId) const
        {
            for (const auto& media : m_model.Media)
            {
                if (media.Id == mediaId)
                {
                    return &media;
                }
            }
            return nullptr;
        }

        void WriteNoteReference(const WordInline& node, Word::Paragraph& target, bool isFootnote)
        {
            const auto& notes = isFootnote ? m_word->Footnotes : m_word->Endnotes;
            const WordNote* modelNote = nullptr;
            for (const auto& candidate : notes)
            {
                if (candidate.Id == node.NoteId)
                {
                    modelNote = &candidate;
                    break;
                }
            }

            auto note = isFootnote ? target.AddFootnote() : target.AddEndnote();
            if (!note)
            {
                Warn(m_diagnostics, "Note reference could not be created");
                return;
            }
            if (modelNote != nullptr)
            {
                // AddFootnote/AddEndnote create the note with one empty content
                // paragraph; reuse it for the first block to avoid a blank line.
                std::shared_ptr<Word::Paragraph> reusable;
                if (const auto existing = note->Paragraphs();
                    existing.size() == 1 && existing.front() && existing.front()->PlainText().empty())
                {
                    reusable = existing.front();
                }
                for (const auto& block : modelNote->Blocks)
                {
                    if (block.Kind != WordBlock::Type::Paragraph || !block.Paragraph)
                    {
                        Warn(m_diagnostics, "Non-paragraph note content dropped");
                        continue;
                    }
                    auto paragraph = reusable ? reusable : note->AddParagraph();
                    reusable = nullptr;
                    if (paragraph)
                    {
                        for (const auto& child : block.Paragraph->Inlines)
                        {
                            WriteInline(child, *paragraph);
                        }
                    }
                }
            }
        }

        void WriteCommentReference(const WordInline& node, Word::Paragraph& target)
        {
            const WordComment* modelComment = nullptr;
            for (const auto& candidate : m_word->Comments)
            {
                if (candidate.Id == node.NoteId)
                {
                    modelComment = &candidate;
                    break;
                }
            }
            if (modelComment == nullptr)
            {
                return;
            }

            Word::CommentAuthor author;
            author.Name = modelComment->Author;
            author.Initials = modelComment->Initials;
            auto comment = target.AddCommentOnParagraph({}, author);
            if (!comment)
            {
                Warn(m_diagnostics, "Comment could not be created");
                return;
            }
            std::shared_ptr<Word::Paragraph> reusable;
            if (const auto existing = comment->Paragraphs();
                existing.size() == 1 && existing.front() && existing.front()->PlainText().empty())
            {
                reusable = existing.front();
            }
            for (const auto& block : modelComment->Blocks)
            {
                if (block.Kind != WordBlock::Type::Paragraph || !block.Paragraph)
                {
                    continue;
                }
                auto paragraph = reusable ? reusable : comment->AddParagraph();
                reusable = nullptr;
                if (paragraph)
                {
                    for (const auto& child : block.Paragraph->Inlines)
                    {
                        WriteInline(child, *paragraph);
                    }
                }
            }
        }

        void WriteTable(const WordTable& modelTable)
        {
            const Size rows = modelTable.Rows.size();
            Size columns = 0;
            for (const auto& row : modelTable.Rows)
            {
                columns = std::max(columns, row.Cells.size());
            }
            if (rows == 0 || columns == 0)
            {
                return;
            }

            auto table = m_editor->AddTable(rows, columns);
            if (!table)
            {
                Warn(m_diagnostics, "Table could not be created");
                return;
            }
            FillTable(*table, modelTable);
        }

        void FillTable(Word::Table& table, const WordTable& modelTable)
        {
            const auto grid = table.GetLogicalGrid();
            for (Size rowIndex = 0; rowIndex < modelTable.Rows.size() && rowIndex < grid.size();
                 ++rowIndex)
            {
                const auto& modelRow = modelTable.Rows[rowIndex];
                for (Size columnIndex = 0;
                     columnIndex < modelRow.Cells.size() && columnIndex < grid[rowIndex].size(); ++columnIndex)
                {
                    const auto& modelCell = modelRow.Cells[columnIndex];
                    if (modelCell.Covered)
                    {
                        continue;
                    }
                    const auto& gridCell = grid[rowIndex][columnIndex];
                    if (gridCell.Cell)
                    {
                        FillCell(gridCell.Cell, modelCell);
                    }
                }
            }

            // Merges are applied after content so covered-cell content is never lost.
            for (Size rowIndex = 0; rowIndex < modelTable.Rows.size(); ++rowIndex)
            {
                const auto& modelRow = modelTable.Rows[rowIndex];
                for (Size columnIndex = 0; columnIndex < modelRow.Cells.size(); ++columnIndex)
                {
                    const auto& modelCell = modelRow.Cells[columnIndex];
                    if (!modelCell.Covered && (modelCell.RowSpan > 1 || modelCell.ColSpan > 1))
                    {
                        table.MergeCells(rowIndex, columnIndex, static_cast<Size>(modelCell.RowSpan),
                                         static_cast<Size>(modelCell.ColSpan));
                    }
                }
            }
        }

        void FillCell(const std::shared_ptr<W::TableCell>& cell, const WordTableCell& modelCell)
        {
            auto mainPart = m_editor->GetDocument() ? m_editor->GetDocument()->GetMainDocumentPart() : nullptr;

            // A freshly created cell carries one empty paragraph; reuse it for the
            // first paragraph block to avoid a leading blank line.
            std::shared_ptr<W::Paragraph> reusable;
            for (const auto& child : cell->Children())
            {
                if (auto paragraph = openxmlelement_cast<W::Paragraph>(child))
                {
                    if (paragraph->Children().empty())
                    {
                        reusable = paragraph;
                    }
                    break;
                }
            }

            for (const auto& block : modelCell.Blocks)
            {
                switch (block.Kind)
                {
                    case WordBlock::Type::Paragraph:
                    {
                        if (!block.Paragraph)
                        {
                            break;
                        }
                        std::shared_ptr<W::Paragraph> lowParagraph = reusable;
                        reusable = nullptr;
                        if (!lowParagraph)
                        {
                            lowParagraph = cell->AppendChild<W::Paragraph>();
                        }
                        if (!lowParagraph)
                        {
                            break;
                        }
                        Word::Paragraph paragraph(lowParagraph, mainPart);
                        ApplyParagraphFormat(paragraph, *block.Paragraph);
                        for (const auto& child : block.Paragraph->Inlines)
                        {
                            WriteInline(child, paragraph);
                        }
                        break;
                    }
                    case WordBlock::Type::Table:
                    {
                        if (!block.Table || block.Table->Rows.empty())
                        {
                            break;
                        }
                        const WordTable& table = *block.Table;
                        Size columns = 0;
                        for (const auto& row : table.Rows)
                        {
                            columns = std::max(columns, row.Cells.size());
                        }
                        auto nested = cell->AppendChild<W::Table>();
                        if (!nested)
                        {
                            break;
                        }
                        Word::Table nestedTable(nested);
                        for (Size i = 0; i < table.Rows.size(); ++i)
                        {
                            nestedTable.AddRow(columns);
                        }
                        FillTable(nestedTable, table);
                        break;
                    }
                    case WordBlock::Type::SectionBreak:
                    {
                        Warn(m_diagnostics, "Section break inside a table cell dropped");
                        break;
                    }
                }
            }
        }

        void ApplyParagraphFormat(Word::Paragraph& paragraph, const WordParagraph& model)
        {
            if (!model.StyleId.empty())
            {
                paragraph.SetStyleId(model.StyleId);
            }
            if (const auto alignment = TokenToEnum<W::JustificationValues>(model.Alignment))
            {
                paragraph.SetAlignment(*alignment);
            }
            if (model.List)
            {
                const int numberingId = RemapNumberingId(model.List->NumberingId);
                if (numberingId != 0)
                {
                    paragraph.SetNumbering(numberingId, model.List->Level);
                }
            }
        }

        void WriteHeadersFooters(const WordDocumentModel& word)
        {
            if (word.Headers.empty() && word.Footers.empty())
            {
                return;
            }
            auto section = m_editor->EnsureFinalSection();
            if (!section)
            {
                Warn(m_diagnostics, "Headers/footers could not be written (no final section)");
                return;
            }

            const auto typeFromKind = [](const std::string& kind)
            {
                if (kind == "first")
                {
                    return Word::HeaderFooterType::First;
                }
                if (kind == "even")
                {
                    return Word::HeaderFooterType::Even;
                }
                return Word::HeaderFooterType::Default;
            };

            const auto writeContent = [&](const WordHeaderFooter& entry,
                                          const std::shared_ptr<Word::HeaderFooterContent>& content)
            {
                if (!content)
                {
                    return;
                }
                for (const auto& block : entry.Blocks)
                {
                    if (block.Kind != WordBlock::Type::Paragraph || !block.Paragraph)
                    {
                        Warn(m_diagnostics, "Non-paragraph header/footer content dropped");
                        continue;
                    }
                    auto paragraph = content->AddParagraph();
                    if (!paragraph)
                    {
                        continue;
                    }
                    for (const auto& node : block.Paragraph->Inlines)
                    {
                        if (node.Kind == WordInline::Type::Text)
                        {
                            paragraph->AddRun(node.Text, RunStyleFromProps(node.Props), true);
                        }
                        else if (node.Kind == WordInline::Type::Field && !node.FieldCode.empty())
                        {
                            paragraph->AddSimpleField(node.FieldCode, node.Text);
                        }
                        else
                        {
                            Warn(m_diagnostics, "Unsupported header/footer inline dropped");
                        }
                    }
                }
            };

            for (const auto& entry : word.Headers)
            {
                writeContent(entry, section->EnsureHeader(typeFromKind(entry.Kind)));
            }
            for (const auto& entry : word.Footers)
            {
                writeContent(entry, section->EnsureFooter(typeFromKind(entry.Kind)));
            }
        }

        void WriteProperties()
        {
            auto document = m_editor->GetDocument();
            if (!document)
            {
                return;
            }
            const auto writeProperty = [&](std::string_view name, const std::string& value)
            {
                if (!value.empty())
                {
                    WriteCoreProperty(*document, name, value);
                }
            };
            writeProperty("Title", m_model.Properties.Title);
            writeProperty("Subject", m_model.Properties.Subject);
            writeProperty("Creator", m_model.Properties.Creator);
            writeProperty("Keywords", m_model.Properties.Keywords);
            writeProperty("Description", m_model.Properties.Description);
            writeProperty("Category", m_model.Properties.Category);
        }

        const DocumentModel& m_model;
        std::vector<ToolDiagnostic>& m_diagnostics;
        Word::WordDocumentEditor::Ptr m_editor;
        /// Set by Write() once the model is known to carry Word content; the writing
        /// helpers run only from there, so they use this instead of the optional.
        const WordDocumentModel* m_word = nullptr;
        std::map<int, int> m_numberingRemap;
    };
};

DocumentModel ReadWordModel(const std::filesystem::path& path, const ModelReadOptions& options,
                            std::vector<ToolDiagnostic>& diagnostics)
{
    auto editor = Word::WordDocumentEditor::Open(path, UntrustedOpenSettings());
    if (!editor)
    {
        diagnostics.push_back(
            ToolDiagnostic{ToolSeverity::Error, "Failed to open Word document", path.string()});
        return {};
    }

    return ReadWordModel(*editor, options, diagnostics);
}

DocumentModel ReadWordModel(Word::WordDocumentEditor& editor, const ModelReadOptions& options,
                            std::vector<ToolDiagnostic>& diagnostics)
{
    DocumentModel model;
    model.Family = DocumentFamily::Word;
    if (auto document = editor.GetDocument())
    {
        model.Properties = ReadCoreProperties(*document);
    }

    WordModelIoHelper::WordModelReader reader(editor, options, model, diagnostics);
    reader.Read();
    return model;
}

bool WriteWordModel(const DocumentModel& model, const std::filesystem::path& path,
                    std::vector<ToolDiagnostic>& diagnostics)
{
    if (!model.Word)
    {
        diagnostics.push_back(ToolDiagnostic{ToolSeverity::Error, "Model carries no Word document"});
        return false;
    }
    WordModelIoHelper::WordModelWriter writer(model, diagnostics);
    return writer.Write(path);
}

} // namespace ExyokiOffice::Tools
