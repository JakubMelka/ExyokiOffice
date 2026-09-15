// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "WordAddressing.hpp"

#include "SharedToolset.hpp"
#include "ToolRegistry.hpp"
#include "Units.hpp"

#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/Wordprocessing.hpp"

#include <span>
#include <utility>

namespace ExyokiOffice::Mcp
{

namespace W = ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing;

/// File-local helpers for the Word inline model.
class WordAddressingHelper
{
public:
    static inline const OpenXmlQualifiedName kParagraphPropertiesName{
        "http://schemas.openxmlformats.org/wordprocessingml/2006/main", "pPr"};

    /// Applies the formatting members of a text inline to a run style.
    static Word::RunStyle ReadRunStyle(const nlohmann::json& inlineValue)
    {
        Word::RunStyle style;
        style.Bold = inlineValue.value("bold", false);
        style.Italic = inlineValue.value("italic", false);
        style.Underline = inlineValue.value("underline", false);
        style.Strike = inlineValue.value("strike", false);
        style.StyleId = inlineValue.value("styleId", std::string());
        style.AsciiFont = inlineValue.value("font", std::string());
        style.HighAnsiFont = style.AsciiFont;

        const auto color = inlineValue.value("color", std::string());
        if (!color.empty())
        {
            style.Color = ParseColor(color);
        }

        const auto sizePt = inlineValue.value("sizePt", 0.0);
        if (sizePt > 0.0)
        {
            style.FontSize = MeasuringUnits(sizePt, MeasurementUnit::Point);
        }

        const auto highlight = inlineValue.value("highlight", std::string());
        if (!highlight.empty())
        {
            style.Highlight = ParseHighlight(highlight);
        }

        return style;
    }

    /**
     * @brief Resolves a highlight token through the schema metadata of the enum.
     *
     * Going through the generated metadata rather than a hand-written table
     * means the tool accepts exactly the spellings `ST_HighlightColor` defines
     * and that `get_document_model` reports back, with no second list to keep
     * in step.
     */
    static std::optional<W::HighlightColorValues> ParseHighlight(const std::string& token)
    {
        return ParseEnum<W::HighlightColorValues>(token);
    }

    /// Resolves a token of any generated enum, or std::nullopt when it is unknown.
    template <typename TEnum>
    static std::optional<TEnum> ParseEnum(const std::string& token)
    {
        const auto* meta = TEnum::GetMetaEnum();
        if (meta == nullptr || token.empty())
        {
            return std::nullopt;
        }

        const TEnum value(static_cast<typename TEnum::Value>(meta->FromString(token)));
        if (!value.IsValid())
        {
            return std::nullopt;
        }

        return value;
    }

    /**
     * @brief Applies the `layout` descriptor of insert_image to a placed image.
     *
     * Every setter that takes a floating-only property switches an inline image
     * to floating on its own, so the mode is applied first and the rest follow
     * only when the caller asked for floating. Anything else would turn a
     * picture the caller wanted in the text flow into an anchored one because
     * of a stray member.
     */
    static bool ApplyImageLayout(Word::Image& image, const nlohmann::json& layout, ToolOutcome& failure)
    {
        namespace WP = ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing;

        if (layout.value("mode", std::string("inline")) != "floating")
        {
            return true;
        }

        const auto wrapToken = layout.value("wrap", std::string("square"));
        const auto wrap = wrapToken == "tight"          ? Word::ImageWrap::Tight
                          : wrapToken == "through"      ? Word::ImageWrap::Through
                          : wrapToken == "topAndBottom" ? Word::ImageWrap::TopAndBottom
                          : wrapToken == "none"         ? Word::ImageWrap::None
                                                        : Word::ImageWrap::Square;

        const auto sideToken = layout.value("wrap_side", std::string("bothSides"));
        const auto side = sideToken == "left"      ? WP::WrapTextValues::Left
                          : sideToken == "right"   ? WP::WrapTextValues::Right
                          : sideToken == "largest" ? WP::WrapTextValues::Largest
                                                   : WP::WrapTextValues::BothSides;
        image.SetWrap(wrap, side);

        const auto horizontal = layout.find("horizontal");
        const auto vertical = layout.find("vertical");
        const bool hasHorizontal = horizontal != layout.end() && horizontal->is_object();
        const bool hasVertical = vertical != layout.end() && vertical->is_object();
        if (hasHorizontal || hasVertical)
        {
            const nlohmann::json empty = nlohmann::json::object();
            const auto& h = hasHorizontal ? *horizontal : empty;
            const auto& v = hasVertical ? *vertical : empty;

            const auto horizontalFromToken = h.value("from", std::string("column"));
            const auto horizontalFrom =
                horizontalFromToken == "page"            ? WP::HorizontalRelativePositionValues::Page
                : horizontalFromToken == "margin"        ? WP::HorizontalRelativePositionValues::Margin
                : horizontalFromToken == "character"     ? WP::HorizontalRelativePositionValues::Character
                : horizontalFromToken == "leftMargin"    ? WP::HorizontalRelativePositionValues::LeftMargin
                : horizontalFromToken == "rightMargin"   ? WP::HorizontalRelativePositionValues::RightMargin
                : horizontalFromToken == "insideMargin"  ? WP::HorizontalRelativePositionValues::InsideMargin
                : horizontalFromToken == "outsideMargin" ? WP::HorizontalRelativePositionValues::OutsideMargin
                                                         : WP::HorizontalRelativePositionValues::Column;

            const auto verticalFromToken = v.value("from", std::string("paragraph"));
            const auto verticalFrom =
                verticalFromToken == "page"            ? WP::VerticalRelativePositionValues::Page
                : verticalFromToken == "margin"        ? WP::VerticalRelativePositionValues::Margin
                : verticalFromToken == "line"          ? WP::VerticalRelativePositionValues::Line
                : verticalFromToken == "topMargin"     ? WP::VerticalRelativePositionValues::TopMargin
                : verticalFromToken == "bottomMargin"  ? WP::VerticalRelativePositionValues::BottomMargin
                : verticalFromToken == "insideMargin"  ? WP::VerticalRelativePositionValues::InsideMargin
                : verticalFromToken == "outsideMargin" ? WP::VerticalRelativePositionValues::OutsideMargin
                                                       : WP::VerticalRelativePositionValues::Paragraph;

            const auto horizontalAlign = h.value("align", std::string());
            const auto verticalAlign = v.value("align", std::string());
            if (!horizontalAlign.empty() || !verticalAlign.empty())
            {
                // The library writes both axes together, so an axis the caller
                // only gave an anchor for gets the neutral alignment rather
                // than an offset it never asked for.
                const auto horizontalValue =
                    horizontalAlign == "left"      ? WP::HorizontalAlignmentValues::Left
                    : horizontalAlign == "right"   ? WP::HorizontalAlignmentValues::Right
                    : horizontalAlign == "inside"  ? WP::HorizontalAlignmentValues::Inside
                    : horizontalAlign == "outside" ? WP::HorizontalAlignmentValues::Outside
                                                   : WP::HorizontalAlignmentValues::Center;
                const auto verticalValue = verticalAlign == "top"       ? WP::VerticalAlignmentValues::Top
                                           : verticalAlign == "bottom"  ? WP::VerticalAlignmentValues::Bottom
                                           : verticalAlign == "inside"  ? WP::VerticalAlignmentValues::Inside
                                           : verticalAlign == "outside" ? WP::VerticalAlignmentValues::Outside
                                                                        : WP::VerticalAlignmentValues::Center;
                image.SetPositionAligned(horizontalFrom, horizontalValue, verticalFrom, verticalValue);
            }
            else
            {
                MeasuringUnits horizontalOffset;
                MeasuringUnits verticalOffset;
                if (!ReadImageOffset(h, "offset", horizontalOffset, failure) ||
                    !ReadImageOffset(v, "offset", verticalOffset, failure))
                {
                    return false;
                }

                image.SetPosition(horizontalFrom, horizontalOffset, verticalFrom, verticalOffset);
            }
        }

        if (const auto distance = layout.find("distance"); distance != layout.end() && distance->is_object())
        {
            MeasuringUnits left;
            MeasuringUnits top;
            MeasuringUnits right;
            MeasuringUnits bottom;
            if (!ReadImageOffset(*distance, "left", left, failure) ||
                !ReadImageOffset(*distance, "top", top, failure) ||
                !ReadImageOffset(*distance, "right", right, failure) ||
                !ReadImageOffset(*distance, "bottom", bottom, failure))
            {
                return false;
            }

            image.SetDistanceFromText(left, top, right, bottom);
        }

        if (layout.contains("behind_text"))
        {
            image.SetBehindText(layout.value("behind_text", false));
        }

        if (layout.contains("allow_overlap"))
        {
            image.SetAllowOverlap(layout.value("allow_overlap", true));
        }

        return true;
    }

    /// Reads one optional length of the layout descriptor; absent means zero.
    static bool ReadImageOffset(const nlohmann::json& owner, const char* name, MeasuringUnits& result,
                                ToolOutcome& failure)
    {
        const auto member = owner.find(name);
        if (member == owner.end())
        {
            result = MeasuringUnits(0.0, MeasurementUnit::Point);
            return true;
        }

        const auto parsed = ParseLength(*member);
        if (!parsed.has_value())
        {
            failure = MakeError(ErrorCode::InputInvalid,
                                "'" + std::string(name) + "' of the image layout is not a length.",
                                member->dump(), "Use a number of points or a string such as \"2cm\".");
            return false;
        }

        result = *parsed;
        return true;
    }

    /**
     * @brief Applies a run formatting preset to an already created run.
     *
     * Paragraph::AddRun() takes a RunStyle, but a run appended to a hyperlink
     * has no such overload, so the preset is replayed through the individual
     * setters here.
     */
    static void ApplyRunStyle(Word::Run& run, const Word::RunStyle& style)
    {
        if (style.Bold)
        {
            run.SetBold(true);
        }

        if (style.Italic)
        {
            run.SetItalic(true);
        }

        if (style.Underline)
        {
            run.SetUnderline(true);
        }

        if (style.Strike)
        {
            run.SetStrike(true);
        }

        if (!style.StyleId.empty())
        {
            run.SetStyleId(style.StyleId);
        }

        if (!style.AsciiFont.empty())
        {
            run.SetFont(style.AsciiFont, style.HighAnsiFont);
        }

        if (style.Color.has_value())
        {
            run.SetColor(*style.Color);
        }

        if (style.FontSize.has_value())
        {
            run.SetFontSize(*style.FontSize);
        }

        if (style.Highlight.has_value())
        {
            run.SetHighlight(*style.Highlight);
        }
    }

    /**
     * @brief Natural size of an image payload in points.
     *
     * Both values stay at zero when the payload is not one of the formats
     * DetectImageFormat() recognizes; the caller then falls back to a nominal
     * ratio rather than writing a zero extent.
     */
    static void NaturalSizePt(std::span<const Byte> bytes, Real& widthPt, Real& heightPt)
    {
        widthPt = 0.0;
        heightPt = 0.0;

        const auto detected = DetectImageFormat(bytes);
        if (!detected.has_value() || detected->HorizontalDpi <= 0.0 || detected->VerticalDpi <= 0.0)
        {
            return;
        }

        widthPt = static_cast<Real>(detected->PixelWidth) / detected->HorizontalDpi * 72.0;
        heightPt = static_cast<Real>(detected->PixelHeight) / detected->VerticalDpi * 72.0;
    }

    static Word::BreakType ParseBreak(const std::string& token)
    {
        if (token == "page")
        {
            return Word::BreakType::Page;
        }

        if (token == "column")
        {
            return Word::BreakType::Column;
        }

        return Word::BreakType::Line;
    }

    /// A marker that opens a bookmark, comment, or permission range.
    static bool IsRangeStart(const std::shared_ptr<OpenXMLElement>& element)
    {
        return openxmlelement_cast<W::BookmarkStart>(element) != nullptr ||
               openxmlelement_cast<W::CommentRangeStart>(element) != nullptr ||
               openxmlelement_cast<W::PermStart>(element) != nullptr;
    }

    /// The run that anchors a comment: nothing but its properties and the `w:commentReference`.
    static bool IsCommentReferenceRun(const std::shared_ptr<OpenXMLElement>& element)
    {
        auto run = openxmlelement_cast<W::Run>(element);
        if (run == nullptr)
        {
            return false;
        }

        bool reference = false;
        for (const auto& child : run->Children())
        {
            if (openxmlelement_cast<W::CommentReference>(child) != nullptr)
            {
                reference = true;
            }
            else if (child && openxmlelement_cast<W::RunProperties>(child) == nullptr)
            {
                return false;
            }
        }
        return reference;
    }

    /// A marker that closes a range, or the run a comment hangs on; both belong behind the content.
    static bool IsRangeEnd(const std::shared_ptr<OpenXMLElement>& element)
    {
        return openxmlelement_cast<W::BookmarkEnd>(element) != nullptr ||
               openxmlelement_cast<W::CommentRangeEnd>(element) != nullptr ||
               openxmlelement_cast<W::PermEnd>(element) != nullptr || IsCommentReferenceRun(element);
    }

    /**
     * @brief Strips the content of a paragraph, keeping its properties and range markers.
     *
     * Bookmarks and comments are ranges whose other marker may sit in another
     * paragraph; dropping the one here would orphan it, and Word then drops
     * the whole bookmark. The start markers stay where they are, in front of
     * whatever content follows; the end markers are returned so the caller can
     * put them back behind the new content once it is written.
     */
    static std::vector<std::shared_ptr<OpenXMLElement>> ClearParagraphContent(Word::Paragraph& paragraph)
    {
        std::vector<std::shared_ptr<OpenXMLElement>> trailing;
        auto lowLevel = paragraph.GetLowLevelApi();
        if (!lowLevel)
        {
            return trailing;
        }

        for (const auto& child : lowLevel->Children())
        {
            // `w:pPr` by name: the DOM wraps it under more than one class.
            if (!child || child->QualifiedName() == kParagraphPropertiesName || IsRangeStart(child))
            {
                continue;
            }
            if (IsRangeEnd(child))
            {
                trailing.push_back(child);
                continue;
            }

            child->Remove();
        }
        return trailing;
    }

    /// Puts the end markers an edit kept back behind the paragraph's new content, whichever way the edit ends.
    class TrailingMarkerRestorer
    {
    public:
        TrailingMarkerRestorer(std::shared_ptr<OpenXMLElement> paragraph,
                               std::vector<std::shared_ptr<OpenXMLElement>> markers)
            : m_paragraph(std::move(paragraph)), m_markers(std::move(markers))
        {
        }

        TrailingMarkerRestorer(const TrailingMarkerRestorer&) = delete;
        TrailingMarkerRestorer& operator=(const TrailingMarkerRestorer&) = delete;

        ~TrailingMarkerRestorer()
        {
            if (!m_paragraph)
            {
                return;
            }
            for (const auto& marker : m_markers)
            {
                if (marker && marker->Parent())
                {
                    marker->MoveInto(m_paragraph);
                }
            }
        }

    private:
        std::shared_ptr<OpenXMLElement> m_paragraph;
        std::vector<std::shared_ptr<OpenXMLElement>> m_markers;
    };
};

nlohmann::json WordAddressing::AnchorSchema()
{
    return Schema::Object(
        "Where the new content goes. 'before' and 'after' need a 1-based block index from read_blocks.",
        {"position"},
        nlohmann::json{{"position", Schema::Enumeration("Insertion point.", {"start", "end", "before", "after"})},
                       {"block", Schema::Integer("1-based body block index.", 1)}});
}

nlohmann::json WordAddressing::InlineSchema()
{
    nlohmann::json properties = nlohmann::json::object();
    properties["text"] = Schema::String("Literal text of a text run.");
    properties["bold"] = Schema::Boolean("Bold text.");
    properties["italic"] = Schema::Boolean("Italic text.");
    properties["underline"] = Schema::Boolean("Single underline.");
    properties["strike"] = Schema::Boolean("Strikethrough.");
    properties["color"] = Schema::String("Text color as \"#RRGGBB\".");
    properties["highlight"] = Schema::String("Highlight token, for example \"yellow\" or \"none\".");
    properties["font"] = Schema::String("Font family name.");
    properties["sizePt"] = Schema::Number("Font size in points.");
    properties["styleId"] = Schema::String("Character style identifier.");
    properties["break"] = Schema::Enumeration("Emits a break instead of text.", {"line", "page", "column"});
    properties["link"] = Schema::Object(
        "Emits a hyperlink instead of text.", {},
        nlohmann::json{{"target", Schema::String("External URL.")},
                       {"anchor", Schema::String("Bookmark name for an internal link.")},
                       {"tooltip", Schema::String("Tooltip shown on hover.")},
                       {"text", Schema::String("Link text; shorthand for a single unformatted run.")},
                       {"runs", Schema::Array("Formatted link text, one entry per run; appended after 'text'.",
                                              Schema::Object("One text run of the link.", {},
                                                             nlohmann::json{
                                                                 {"text", Schema::String("Literal run text.")},
                                                                 {"bold", Schema::Boolean("Bold text.")},
                                                                 {"italic", Schema::Boolean("Italic text.")},
                                                                 {"underline", Schema::Boolean("Single underline.")},
                                                                 {"strike", Schema::Boolean("Strikethrough.")},
                                                                 {"color", Schema::String("Text color as "
                                                                                          "\"#RRGGBB\".")},
                                                                 {"highlight", Schema::String("Highlight token.")},
                                                                 {"font", Schema::String("Font family name.")},
                                                                 {"sizePt", Schema::Number("Font size in points.")},
                                                                 {"styleId", Schema::String("Character style "
                                                                                            "identifier.")}}))}});
    properties["image"] = Schema::Object(
        "Emits an inline picture instead of text.", {},
        nlohmann::json{{"path", Schema::String("Workspace-relative image file.")},
                       {"dataBase64", Schema::String("Base64 image payload.")},
                       {"contentType", Schema::String("Media type; detected when omitted.")},
                       {"width", Schema::Length("Rendered width; the aspect ratio is kept when only one is given.")},
                       {"height", Schema::Length("Rendered height; the aspect ratio is kept when only one is given.")},
                       {"alt", Schema::String("Alternative text.")}});
    properties["field"] = Schema::Object("Emits a field instead of text.", {"code"},
                                         nlohmann::json{{"code", Schema::String("Field instruction, e.g. \"PAGE\".")},
                                                        {"text", Schema::String("Cached result text.")}});

    return Schema::Object("One inline item; supply exactly one kind per entry.", {}, std::move(properties));
}

nlohmann::json WordAddressing::TextSchema()
{
    return Schema::String("Plain text shorthand for a single unformatted run; alternative to 'inlines'.");
}

bool WordAddressing::ParseAnchor(const nlohmann::json& arguments, WordAnchor& anchor, ToolOutcome& failure)
{
    const auto member = arguments.find("anchor");
    if (member == arguments.end() || !member->is_object())
    {
        failure = MakeError(ErrorCode::InputInvalid, "The 'anchor' object is required.", {},
                            "Use {\"position\": \"end\"} to append at the end of the body.");
        return false;
    }

    const auto position = member->value("position", std::string());
    if (position == "start")
    {
        anchor.Where = WordAnchor::Position::Start;
    }
    else if (position == "end")
    {
        anchor.Where = WordAnchor::Position::End;
    }
    else if (position == "before")
    {
        anchor.Where = WordAnchor::Position::Before;
    }
    else if (position == "after")
    {
        anchor.Where = WordAnchor::Position::After;
    }
    else
    {
        failure = MakeError(ErrorCode::AnchorInvalid, "Unknown anchor position '" + position + "'.", position,
                            "Use start, end, before, or after.");
        return false;
    }

    anchor.Block = member->value("block", static_cast<Size>(0));
    if ((anchor.Where == WordAnchor::Position::Before || anchor.Where == WordAnchor::Position::After) &&
        anchor.Block == 0)
    {
        failure = MakeError(ErrorCode::AnchorInvalid, "Anchor positions 'before' and 'after' need a 'block' index.",
                            position, "Call read_blocks to obtain the block indices.");
        return false;
    }

    return true;
}

bool WordAddressing::ResolveCursor(Word::WordDocumentEditor& editor, const WordAnchor& anchor,
                                   Word::WordDocumentEditor::BodyCursor& cursor, ToolOutcome& failure)
{
    if (anchor.Where == WordAnchor::Position::Start)
    {
        cursor = editor.BodyStart();
        return true;
    }

    if (anchor.Where == WordAnchor::Position::End)
    {
        cursor = editor.Body();
        return true;
    }

    Word::BodyBlock block;
    if (!BlockAt(editor, anchor.Block, block, failure))
    {
        return false;
    }

    if (auto paragraph = block.AsParagraph())
    {
        cursor = anchor.Where == WordAnchor::Position::Before ? editor.Before(paragraph) : editor.After(paragraph);
        return true;
    }

    if (auto table = block.AsTable())
    {
        cursor = anchor.Where == WordAnchor::Position::Before ? editor.Before(table) : editor.After(table);
        return true;
    }

    failure = MakeError(ErrorCode::AnchorInvalid,
                        "Block " + std::to_string(anchor.Block) +
                            " is neither a paragraph nor a table, so nothing can be anchored to it.",
                        std::to_string(anchor.Block),
                        "Anchor to a neighbouring paragraph, or use position start or end.");
    return false;
}

bool WordAddressing::BlockAt(Word::WordDocumentEditor& editor, Size index, Word::BodyBlock& block,
                             ToolOutcome& failure)
{
    const auto blocks = editor.BodyBlocks();
    if (index == 0 || index > blocks.size())
    {
        failure = MakeError(ErrorCode::BlockNotFound,
                            "The document has " + std::to_string(blocks.size()) + " body block(s); block " +
                                std::to_string(index) + " does not exist.",
                            std::to_string(index), "Call read_blocks to see the current block indices.");
        return false;
    }

    block = blocks[index - 1];
    return true;
}

std::shared_ptr<Word::Paragraph> WordAddressing::ParagraphAt(Word::WordDocumentEditor& editor, Size index,
                                                             ToolOutcome& failure)
{
    Word::BodyBlock block;
    if (!BlockAt(editor, index, block, failure))
    {
        return nullptr;
    }

    auto paragraph = block.AsParagraph();
    if (paragraph == nullptr)
    {
        failure = MakeError(ErrorCode::BlockNotFound, "Block " + std::to_string(index) + " is not a paragraph.",
                            std::to_string(index), "Call read_blocks to see the type of each block.");
    }

    return paragraph;
}

std::shared_ptr<Word::Table> WordAddressing::TableAt(Word::WordDocumentEditor& editor, Size index,
                                                     ToolOutcome& failure)
{
    Word::BodyBlock block;
    if (!BlockAt(editor, index, block, failure))
    {
        return nullptr;
    }

    auto table = block.AsTable();
    if (table == nullptr)
    {
        failure = MakeError(ErrorCode::BlockNotFound, "Block " + std::to_string(index) + " is not a table.",
                            std::to_string(index), "Call read_blocks to see the type of each block.");
    }

    return table;
}

Size WordAddressing::IndexOfElement(Word::WordDocumentEditor& editor, const std::shared_ptr<OpenXMLElement>& element)
{
    if (element == nullptr)
    {
        return 0;
    }

    // Element wrappers are node views, so two wrappers for the same body child
    // are different objects; identity has to be decided on the XML node.
    const auto blocks = editor.BodyBlocks();
    for (Size index = 0; index < blocks.size(); ++index)
    {
        const auto& candidate = blocks[index].GetLowLevelApi();
        if (candidate && candidate->IsSameNode(element))
        {
            return index + 1;
        }
    }

    return 0;
}

bool WordAddressing::HasContent(const nlohmann::json& arguments)
{
    return arguments.contains("text") || arguments.contains("inlines");
}

bool WordAddressing::ApplyContent(ToolContext& context, Word::WordDocumentEditor& editor, Word::Paragraph& paragraph,
                                  const nlohmann::json& arguments, ToolOutcome& failure)
{
    const WordAddressingHelper::TrailingMarkerRestorer markers(paragraph.GetLowLevelApi(),
                                                               WordAddressingHelper::ClearParagraphContent(paragraph));

    const auto inlines = arguments.find("inlines");
    if (inlines == arguments.end() || !inlines->is_array())
    {
        const auto text = arguments.value("text", std::string());
        if (!text.empty())
        {
            paragraph.AddText(text, true);
        }

        return true;
    }

    for (const auto& item : *inlines)
    {
        if (!item.is_object())
        {
            failure = MakeError(ErrorCode::InputInvalid, "Every entry of 'inlines' must be an object.");
            return false;
        }

        const auto breakKind = item.find("break");
        if (breakKind != item.end() && breakKind->is_string())
        {
            paragraph.AddBreak(WordAddressingHelper::ParseBreak(breakKind->get<std::string>()));
            continue;
        }

        const auto link = item.find("link");
        if (link != item.end() && link->is_object())
        {
            const auto linkText = link->value("text", std::string());
            const auto target = link->value("target", std::string());
            const auto bookmark = link->value("anchor", std::string());
            const auto tooltip = link->value("tooltip", std::string());
            std::shared_ptr<Word::Hyperlink> hyperlink;
            if (!target.empty())
            {
                hyperlink = paragraph.AddHyperlink(linkText, target, tooltip);
            }
            else if (!bookmark.empty())
            {
                hyperlink = paragraph.AddInternalHyperlink(linkText, bookmark, tooltip);
            }
            else
            {
                failure = MakeError(ErrorCode::InputInvalid, "A 'link' inline needs 'target' or 'anchor'.", {},
                                    "Set 'target' to a URL, or 'anchor' to a bookmark name from get_outline.");
                return false;
            }

            if (hyperlink == nullptr)
            {
                failure = MakeError(ErrorCode::OperationFailed, "The hyperlink could not be created.",
                                    target.empty() ? bookmark : target);
                return false;
            }

            const auto runs = link->find("runs");
            if (runs != link->end() && runs->is_array())
            {
                for (const auto& runItem : *runs)
                {
                    if (!runItem.is_object())
                    {
                        failure = MakeError(ErrorCode::InputInvalid,
                                            "Every entry of a link's 'runs' must be an object.");
                        return false;
                    }

                    auto run = hyperlink->AddRun();
                    if (run == nullptr)
                    {
                        failure = MakeError(ErrorCode::OperationFailed, "A hyperlink run could not be created.");
                        return false;
                    }

                    run->AddText(runItem.value("text", std::string()), true);
                    WordAddressingHelper::ApplyRunStyle(*run, WordAddressingHelper::ReadRunStyle(runItem));
                }
            }

            continue;
        }

        const auto image = item.find("image");
        if (image != item.end() && image->is_object())
        {
            std::vector<Byte> bytes;
            std::string contentType;
            if (!ToolSupport::LoadImagePayload(context, *image, bytes, contentType, failure))
            {
                return false;
            }

            if (!AppendImage(editor, paragraph, std::move(bytes), contentType, *image, failure))
            {
                return false;
            }

            continue;
        }

        const auto field = item.find("field");
        if (field != item.end() && field->is_object())
        {
            paragraph.AddField(field->value("code", std::string()), field->value("text", std::string()));
            continue;
        }

        const auto text = item.value("text", std::string());
        paragraph.AddRun(text, WordAddressingHelper::ReadRunStyle(item), true);
    }

    return true;
}

bool WordAddressing::AppendImage(Word::WordDocumentEditor& editor, Word::Paragraph& paragraph,
                                 std::vector<Byte> bytes, const std::string& contentType,
                                 const nlohmann::json& descriptor, ToolOutcome& failure)
{
    std::shared_ptr<Word::Image> image;
    const auto width = descriptor.find("width");
    const auto height = descriptor.find("height");
    const bool hasWidth = width != descriptor.end() && !width->is_null();
    const bool hasHeight = height != descriptor.end() && !height->is_null();
    if (hasWidth || hasHeight)
    {
        std::optional<MeasuringUnits> parsedWidth;
        std::optional<MeasuringUnits> parsedHeight;
        if (hasWidth)
        {
            parsedWidth = ParseLength(*width);
        }

        if (hasHeight)
        {
            parsedHeight = ParseLength(*height);
        }

        if ((hasWidth && !parsedWidth.has_value()) || (hasHeight && !parsedHeight.has_value()))
        {
            failure = MakeError(ErrorCode::InputInvalid, "The image width or height is not a valid length.", {},
                                "Use a number of points or a string such as \"4cm\".");
            return false;
        }

        // One dimension is enough: the missing one follows from the payload's
        // own pixel size and resolution, so the picture is never distorted.
        if (!parsedWidth.has_value() || !parsedHeight.has_value())
        {
            Real naturalWidthPt = 0.0;
            Real naturalHeightPt = 0.0;
            WordAddressingHelper::NaturalSizePt(bytes, naturalWidthPt, naturalHeightPt);

            if (!parsedHeight.has_value())
            {
                const Real ratio = naturalWidthPt > 0.0 ? naturalHeightPt / naturalWidthPt : 0.75;
                parsedHeight = MeasuringUnits(ToPointValue(*parsedWidth) * ratio, MeasurementUnit::Point);
            }
            else
            {
                const Real ratio = naturalHeightPt > 0.0 ? naturalWidthPt / naturalHeightPt : 4.0 / 3.0;
                parsedWidth = MeasuringUnits(ToPointValue(*parsedHeight) * ratio, MeasurementUnit::Point);
            }
        }

        image = editor.AddImageFromData(std::move(bytes), contentType, *parsedWidth, *parsedHeight,
                                        Word::ImageLayout::Inline);
    }
    else
    {
        // Without an explicit size the library derives it from the image's own
        // pixel dimensions and resolution.
        image = editor.AddImageFromData(std::move(bytes), Word::ImageLayout::Inline);
    }

    if (image == nullptr)
    {
        failure = MakeError(ErrorCode::OperationFailed, "The image could not be added to the document.", {},
                            "Check that the payload is a supported image format.");
        return false;
    }

    // Switching an inline picture to floating rebuilds the drawing around a
    // `wp:anchor`, so the layout is applied first and the alt text written into
    // whichever container the picture ended up in.
    const auto layout = descriptor.find("layout");
    if (layout != descriptor.end() && layout->is_object() &&
        !WordAddressingHelper::ApplyImageLayout(*image, *layout, failure))
    {
        return false;
    }

    const auto alt = descriptor.value("alt", std::string());
    if (!alt.empty())
    {
        image->SetAltText(alt, alt);
    }

    // AddImageFromData appends a fresh paragraph at the end of the body; the
    // drawing is moved into the target paragraph and the leftover is removed.
    auto drawing = image->GetLowLevelApi();
    if (!drawing)
    {
        return true;
    }

    auto sourceRun = drawing->Parent();
    auto sourceParagraph = sourceRun ? sourceRun->Parent() : nullptr;

    auto run = paragraph.AddRun();
    if (run == nullptr)
    {
        failure = MakeError(ErrorCode::OperationFailed, "The image run could not be created.");
        return false;
    }

    if (drawing->MoveInto(run->GetLowLevelApi()) == nullptr)
    {
        failure = MakeError(ErrorCode::OperationFailed, "The image could not be placed into the paragraph.");
        return false;
    }

    if (sourceParagraph && sourceParagraph->Parent())
    {
        sourceParagraph->Remove();
    }

    return true;
}

std::optional<W::JustificationValues> WordAddressing::ParseAlignment(const std::string& token)
{
    // AlignmentTokens() narrows what the schema advertises; the metadata
    // decides what the value actually means.
    return WordAddressingHelper::ParseEnum<W::JustificationValues>(token);
}

std::optional<W::NumberFormatValues> WordAddressing::ParseNumberFormat(const std::string& token)
{
    // The schema publishes the common formats; the metadata knows the whole
    // ST_NumberFormat list, so a document read back may name one the schema
    // never offered and still round-trip.
    return WordAddressingHelper::ParseEnum<W::NumberFormatValues>(token);
}

std::string WordAddressing::NumberFormatToken(W::NumberFormatValues format)
{
    const auto* meta = W::NumberFormatValues::GetMetaEnum();
    return meta != nullptr ? std::string(meta->ToString(static_cast<UInt32>(format.GetValue()))) : std::string();
}

std::vector<std::string> WordAddressing::AlignmentTokens()
{
    return {"left", "center", "right", "both", "distribute"};
}

std::string WordAddressing::BlockTypeToken(Word::BodyBlockType type)
{
    switch (type)
    {
        case Word::BodyBlockType::Paragraph:
            return "paragraph";
        case Word::BodyBlockType::Table:
            return "table";
        case Word::BodyBlockType::Section:
            return "section";
        case Word::BodyBlockType::ContentControl:
            return "contentControl";
        case Word::BodyBlockType::Unsupported:
            break;
    }

    return "unsupported";
}

nlohmann::json WordAddressing::BlockToJson(const Word::BodyBlock& block, Size index)
{
    nlohmann::json entry = nlohmann::json::object();
    entry["block"] = static_cast<UInt64>(index);
    entry["kind"] = BlockTypeToken(block.Type());

    if (auto paragraph = block.AsParagraph())
    {
        entry["text"] = paragraph->PlainText();
        entry["styleId"] = paragraph->GetStyleId();
    }
    else if (auto table = block.AsTable())
    {
        entry["rows"] = static_cast<UInt64>(table->GetRowCount());
        entry["columns"] = static_cast<UInt64>(table->GetLogicalColumnCount());
    }

    return entry;
}

} // namespace ExyokiOffice::Mcp
