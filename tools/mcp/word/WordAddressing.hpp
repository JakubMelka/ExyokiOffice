// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

#include "Results.hpp"
#include "ToolContext.hpp"

#include "ExyokiOffice/OpenXMLElement.hpp"
#include "ExyokiOffice/StandardTypes.hpp"
#include "ExyokiOffice/Word/WordDocument.hpp"

#include <nlohmann/json.hpp>

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ExyokiOffice::Mcp
{

/**
 * @brief An insertion point in the Word body.
 *
 * The body is a flat sequence of blocks numbered from one, exactly as
 * `read_blocks` reports it. An anchor is either one of the two ends or a
 * position relative to an existing block.
 */
struct WordAnchor
{
    enum class Position
    {
        Start,
        End,
        Before,
        After
    };

    Position Where = Position::End;
    /// 1-based block index; required for Before and After.
    Size Block = 0;
};

/**
 * @brief Translation between the tool-facing block addressing and the editor.
 *
 * Word has no stable identifier for a body block, so the MCP interface
 * addresses blocks by position and every mutation reports the new indices of
 * whatever it touched. All of that bookkeeping lives here rather than in the
 * individual tools.
 */
class WordAddressing
{
public:
    /// Schema of the `anchor` argument shared by every inserting tool.
    [[nodiscard]] static nlohmann::json AnchorSchema();

    /// Schema of one `inlines[]` entry.
    [[nodiscard]] static nlohmann::json InlineSchema();

    /// Schema of the `text` shorthand accepted next to `inlines`.
    [[nodiscard]] static nlohmann::json TextSchema();

    [[nodiscard]] static bool ParseAnchor(const nlohmann::json& arguments, WordAnchor& anchor, ToolOutcome& failure);

    /// Builds the body cursor an anchor denotes; fills @p failure on a bad anchor.
    [[nodiscard]] static bool ResolveCursor(Word::WordDocumentEditor& editor, const WordAnchor& anchor,
                                            Word::WordDocumentEditor::BodyCursor& cursor, ToolOutcome& failure);

    /// Returns the 1-based body block, or fills @p failure with `block_not_found`.
    [[nodiscard]] static bool BlockAt(Word::WordDocumentEditor& editor, Size index, Word::BodyBlock& block,
                                      ToolOutcome& failure);

    /// Returns the paragraph at a 1-based block index, or fills @p failure.
    [[nodiscard]] static std::shared_ptr<Word::Paragraph> ParagraphAt(Word::WordDocumentEditor& editor, Size index,
                                                                      ToolOutcome& failure);

    /// Returns the table at a 1-based block index, or fills @p failure.
    [[nodiscard]] static std::shared_ptr<Word::Table> TableAt(Word::WordDocumentEditor& editor, Size index,
                                                              ToolOutcome& failure);

    /// 1-based index of a body block element, or 0 when it is not a body child.
    [[nodiscard]] static Size IndexOfElement(Word::WordDocumentEditor& editor,
                                             const std::shared_ptr<OpenXMLElement>& element);

    /**
     * @brief Fills a paragraph from the `text` or `inlines` argument.
     *
     * Existing content is replaced. The inline model mirrors what
     * `get_document_model` reports, so a model read back can be edited and
     * written without translation.
     */
    [[nodiscard]] static bool ApplyContent(ToolContext& context, Word::WordDocumentEditor& editor,
                                           Word::Paragraph& paragraph, const nlohmann::json& arguments,
                                           ToolOutcome& failure);

    /// True when the arguments carry either `text` or `inlines`.
    [[nodiscard]] static bool HasContent(const nlohmann::json& arguments);

    [[nodiscard]] static std::optional<DocumentFormat::OpenXml::Wordprocessing::JustificationValues> ParseAlignment(
        const std::string& token);

    /// Renders a block type as its tool-facing token.
    [[nodiscard]] static std::string BlockTypeToken(Word::BodyBlockType type);

    /// Compact JSON description of one body block.
    [[nodiscard]] static nlohmann::json BlockToJson(const Word::BodyBlock& block, Size index);

    /// Inserts an inline image into a run of @p paragraph, without leaving an empty block behind.
    [[nodiscard]] static bool AppendImage(Word::WordDocumentEditor& editor, Word::Paragraph& paragraph,
                                          std::vector<Byte> bytes, const std::string& contentType,
                                          const nlohmann::json& descriptor, ToolOutcome& failure);

    /// Alignment tokens accepted by the paragraph tools.
    [[nodiscard]] static std::vector<std::string> AlignmentTokens();
};

} // namespace ExyokiOffice::Mcp
