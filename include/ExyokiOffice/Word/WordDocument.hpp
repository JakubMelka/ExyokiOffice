// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

#include "ExyokiOffice/Packaging/WordprocessingDocument.hpp"
#include "ExyokiOffice/Color.hpp"
#include "ExyokiOffice/DocumentEditTransaction.hpp"
#include "ExyokiOffice/ImageFormat.hpp"
#include "ExyokiOffice/MeasuringUnits.hpp"
#include "ExyokiOffice/TextPattern.hpp"
#include "ExyokiOffice/ThemeService.hpp"
#include "ExyokiOffice/Word/WordChart.hpp"
#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/Wordprocessing.Enums.hpp"
#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/Drawing/Wordprocessing.Enums.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace ExyokiOffice
{
class OpenXMLElement;
}

namespace ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing
{
class Body;
class Document;
class Drawing;
class Footer;
class Header;
class Paragraph;
class Run;
class Text;
class Table;
class Styles;
class Style;
class LatentStyles;
class LatentStyleExceptionInfo;
class TableRow;
class TableCell;
class ParagraphProperties;
class RunProperties;
class TableProperties;
class TableRowProperties;
class TableCellProperties;
class NumberingProperties;
class Justification;
class SpacingBetweenLines;
class Indentation;
class ParagraphStyleId;
class RunFonts;
class FontSize;
class Color;
class Highlight;
class Underline;
class TableBorders;
class TableCellMargin;
class TableCellMarginDefault;
class TableCellVerticalAlignment;
class TableCellWidth;
class TableWidth;
class Shading;
class HorizontalMerge;
class VerticalMerge;
class GridSpan;
class SectionProperties;
class Hyperlink;
class BookmarkStart;
class BookmarkEnd;
class SimpleField;
class FieldChar;
class Footnote;
class Endnote;
class Comment;
class CommentRangeStart;
class CommentRangeEnd;
class CommentReference;
class FootnoteReference;
class EndnoteReference;
} // namespace ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing

namespace ExyokiOffice::Packaging
{
class FooterPart;
class HeaderPart;
class MainDocumentPart;
class StyleDefinitionsPart;
} // namespace ExyokiOffice::Packaging

namespace ExyokiOffice::Word
{
class WordDocumentEditor;
class Paragraph;
class Run;
class Text;
class Image;
class Table;
class Section;
class BodyBlock;
class HeaderFooterContent;
class StyleManager;
class NumberingManager;
class Hyperlink;
class Bookmark;
class Field;
class Note;
class Comment;
class ContentControl;
class Revision;

using WordDocument = ExyokiOffice::Packaging::WordDocument;

/**
 * @brief High-level layout mode for images.
 *
 * Inline images flow with text, while floating images are anchored
 * relative to the page and can use wrapping.
 */
enum class ImageLayout
{
    Inline,
    Floating
};

/**
 * @brief Text wrapping strategies for floating images.
 */
enum class ImageWrap
{
    None,
    Square,
    Tight,
    Through,
    TopAndBottom
};

/**
 * @brief Result of sniffing an image payload's real format, pixel size, and resolution.
 *
 * Word documents store an image's logical size in EMUs, but callers frequently only
 * have the raw file bytes. DetectImageFormat() inspects the payload signature instead
 * of trusting a file extension or a caller-supplied content type, so a mislabeled file
 * (for example a PNG saved with a `.jpg` extension) is still recognized correctly.
 *
 * @see DetectImageFormat
 */
using ImageFormatInfo = ExyokiOffice::ImageFormatInfo;

/**
 * @brief Detects the real format, pixel size, and resolution of an image payload.
 *
 * This inspects the payload's binary signature and format-specific header fields. It
 * currently recognizes PNG, JPEG (including embedded JFIF resolution), GIF, and BMP.
 * Vector and legacy container formats (EMF, WMF, TIFF) are not sniffed and cause this
 * function to return std::nullopt; callers that need those formats should continue to
 * supply an explicit content type and size to AddImageFromData()/AddImageFromFile().
 *
 * @param data Raw image bytes to inspect. The buffer is only read, never retained.
 * @return Detected format metadata, or std::nullopt when the payload is too short or
 * does not match a recognized signature.
 *
 * @code
 * auto bytes = ExyokiOffice::Packaging::ReadFileFully("photo.png");
 * if (auto format = ExyokiOffice::Word::DetectImageFormat(bytes))
 * {
 *     // format->ContentType == "image/png"
 * }
 * @endcode
 */
EXYOKIOFFICE_EXPORT std::optional<ImageFormatInfo> DetectImageFormat(std::span<const Byte> data);

/**
 * @brief Common numbering list styles supported by WordDocumentEditor.
 */
enum class ListStyleType
{
    Numbered,
    Bulleted
};

/**
 * @brief High-level kind of an explicit text break (`<w:br>`).
 *
 * Line breaks end the current line without ending the paragraph, page breaks
 * continue on the next page, and column breaks continue in the next text
 * column. To force a page break *before* a whole paragraph, prefer
 * Paragraph::SetPageBreakBefore(); to insert a break at the current content
 * position, use Paragraph::AddBreak() or Run::AddBreak().
 */
enum class BreakType
{
    Line,
    Page,
    Column
};

/**
 * @brief Identifies the high-level type of a top-level body block.
 *
 * Word document bodies can contain paragraphs, tables, block-level content
 * controls, and a final section properties element. Other Open XML elements
 * are preserved by the DOM and reported as Unsupported so callers can keep a
 * stable body-order view without dropping content they do not understand yet.
 */
enum class BodyBlockType
{
    Paragraph,
    Table,
    Section,
    ContentControl,
    Unsupported
};

/**
 * @brief Distinguishes footnotes from endnotes for the Note wrapper.
 */
enum class NoteKind
{
    Footnote,
    Endnote
};

/**
 * @brief Classifies the role of one footnote/endnote entry.
 *
 * Word stores one Separator and one ContinuationSeparator entry per notes
 * part in addition to the actual note content (Normal). These bookkeeping
 * entries are created automatically the first time a note is added and are
 * not meant to be edited like regular notes.
 */
enum class NoteEntryType
{
    Normal,
    Separator,
    ContinuationSeparator,
    ContinuationNotice
};

/**
 * @brief Author metadata attached to a Word comment.
 */
struct CommentAuthor
{
    /// Display name stored as the comment's author.
    std::string Name;
    /// Author initials shown alongside the comment in Word's UI.
    std::string Initials;
};

/**
 * @brief Distinguishes block-level from inline (run-level) content controls.
 *
 * Word represents both with the same `<w:sdt>` element; the difference is
 * purely structural: a block-level content control is a sibling of
 * paragraphs and tables, while an inline content control lives inside a
 * paragraph alongside runs.
 */
enum class ContentControlLevel
{
    Block,
    Inline
};

/**
 * @brief High-level classification of a tracked Word revision.
 *
 * The values map to the common WordprocessingML revision containers. Insertions
 * and deletions are exposed first because they are the stable base for
 * read/accept/reject workflows; move ranges are reported with their own values
 * so callers can build compatibility handling without losing the original XML.
 */
enum class RevisionType
{
    Insertion,
    Deletion,
    MoveFrom,
    MoveTo,
    Unknown
};

/**
 * @brief Metadata used when creating tracked revisions.
 */
struct RevisionAuthor
{
    /// Author written to the revision's `w:author` attribute.
    std::string Name = "ExyokiOffice";
    /// Revision identifier written to `w:id`. When empty, the editor allocates one.
    std::string Id;
    /// Optional ISO-like date text written to `w:date`. The library preserves it as text.
    std::string Date;
};

/**
 * @brief High-level section break behavior.
 *
 * The values map to WordprocessingML `w:type/@w:val` on a section properties
 * element. They are exposed here so application code can work with sections
 * without depending on generated DOM enum names.
 */
enum class SectionStartType
{
    NextPage,
    NextColumn,
    Continuous,
    EvenPage,
    OddPage
};

/**
 * @brief Identifies the page role of a section header or footer reference.
 *
 * Default applies to all pages that do not have a more specific first/even
 * reference. First applies when the section uses different first-page content.
 * Even applies when the document displays different odd and even pages.
 */
enum class HeaderFooterType
{
    Default,
    Even,
    First
};

/**
 * @brief Conflict handling used when importing a style into another document.
 */
enum class StyleCopyConflictPolicy
{
    KeepExisting,
    Replace,
    Rename
};

/**
 * @brief Controls dependency import behavior for BodyCursor::InsertDocument.
 *
 * @see WordDocumentEditor::BodyCursor::InsertDocument
 */
struct DocumentMergeOptions
{
    /**
     * Conflict policy applied to every paragraph, character, and table style
     * referenced by the copied content (via StyleManager::ImportStyle).
     * Rename is the safe default: a style ID that is not yet used in the
     * target document keeps its original ID, and a style ID that collides
     * with a differently defined target style is imported under a fresh
     * unique ID instead of silently reusing the target's existing
     * definition. This guarantees copied content keeps its original
     * appearance and never introduces a duplicate style ID.
     */
    StyleCopyConflictPolicy StyleConflictPolicy = StyleCopyConflictPolicy::Rename;
};

/**
 * @brief In-memory data used by WordDocumentEditor::MergeTemplate().
 *
 * The template merge API deliberately accepts only literal values supplied by
 * application code. Field names, bookmark names, and repeating-region names are
 * treated as dictionary keys; the library does not parse formulas, call script
 * engines, load external data, or execute native callbacks stored in the
 * document. This keeps mail-merge style workflows deterministic and safe for
 * untrusted templates.
 *
 * Repeating regions use the common Word mail-merge marker convention:
 * `MERGEFIELD TableStart:Orders` starts a region and `MERGEFIELD TableEnd:Orders`
 * ends it. The paragraphs between those markers are copied once for each row in
 * Regions["Orders"], then merged with that row's values. The marker paragraphs
 * are removed from the final document.
 */
struct TemplateMergeData
{
    /// Literal scalar values used for `MERGEFIELD Name` fields and matching bookmarks.
    std::unordered_map<std::string, std::string> Values;
    /// Literal row values used by `TableStart:Name` / `TableEnd:Name` repeating regions.
    std::unordered_map<std::string, std::vector<std::unordered_map<std::string, std::string>>> Regions;
};

/**
 * @brief Summary returned by WordDocumentEditor::MergeTemplate().
 */
struct TemplateMergeResult
{
    /// Number of scalar MERGEFIELD results replaced.
    Size FieldsMerged = 0;
    /// Number of same-paragraph bookmarks replaced from TemplateMergeData::Values.
    Size BookmarksMerged = 0;
    /// Number of repeating regions expanded.
    Size RegionsMerged = 0;
    /// Number of row copies inserted across all repeating regions.
    Size RegionRowsInserted = 0;
};

/**
 * @brief High-level style type matching WordprocessingML `w:style/@w:type`.
 */
enum class StyleType
{
    Paragraph,
    Character,
    Table,
    Numbering
};

/**
 * @brief Metadata used to create or update a Word style definition.
 */
struct StyleDefinition
{
    std::string StyleId;
    std::string Name;
    StyleType Type = StyleType::Paragraph;
    bool IsDefault = false;
    bool IsCustom = true;
    bool IsPrimary = false;
    bool IsSemiHidden = false;
    bool IsUnhideWhenUsed = false;
    std::optional<int> UiPriority;
    std::string BasedOnStyleId;
    std::string NextStyleId;
    std::string LinkedStyleId;
    std::string Aliases;
};

/**
 * @brief Default behavior for latent styles.
 */
struct LatentStyleSettings
{
    std::optional<bool> DefaultLocked;
    std::optional<int> DefaultUiPriority;
    std::optional<bool> DefaultSemiHidden;
    std::optional<bool> DefaultUnhideWhenUsed;
    std::optional<bool> DefaultPrimaryStyle;
    std::optional<int> Count;
};

/**
 * @brief Per-style latent style exception metadata.
 */
struct LatentStyleException
{
    std::string Name;
    std::optional<bool> Locked;
    std::optional<int> UiPriority;
    std::optional<bool> SemiHidden;
    std::optional<bool> UnhideWhenUsed;
    std::optional<bool> PrimaryStyle;
};

/**
 * @brief Page orientation for a Word section.
 */
enum class PageOrientation
{
    Portrait,
    Landscape
};

/**
 * @brief Page width, height, and orientation for a Word section.
 *
 * Word stores page dimensions in twips. This structure accepts any
 * MeasuringUnits value and the editor converts it when writing the document.
 */
struct SectionPageSize
{
    /// Physical page width.
    ExyokiOffice::MeasuringUnits Width;
    /// Physical page height.
    ExyokiOffice::MeasuringUnits Height;
    /// Page orientation metadata.
    PageOrientation Orientation = PageOrientation::Portrait;
};

/**
 * @brief Page margins for a Word section.
 *
 * All values are physical distances and are converted to twips when written to
 * `w:pgMar`. Header and footer specify the distance from the page edge to the
 * header/footer area. Gutter specifies extra binding space.
 */
struct SectionMargins
{
    ExyokiOffice::MeasuringUnits Top;
    ExyokiOffice::MeasuringUnits Right;
    ExyokiOffice::MeasuringUnits Bottom;
    ExyokiOffice::MeasuringUnits Left;
    ExyokiOffice::MeasuringUnits Header;
    ExyokiOffice::MeasuringUnits Footer;
    ExyokiOffice::MeasuringUnits Gutter;
};

/**
 * @brief Equal-width text column settings for a Word section.
 */
struct SectionColumns
{
    /// Number of equal-width columns. Values below 1 are written as 1.
    UInt16 Count = 1;
    /// Spacing between equal-width columns.
    ExyokiOffice::MeasuringUnits Spacing;
    /// Whether Word should draw a separator line between columns.
    bool Separator = false;
};

/**
 * @brief Identifies a list style definition and its default level.
 */
struct ListStyle
{
    /// Numbering definition instance ID to apply to paragraphs.
    int NumberingId = 0;
    /// Default list level (usually 0 for single-level lists).
    int Level = 0;
};

/**
 * @brief Defines one level in a Word multi-level numbering definition.
 *
 * A level maps to one `w:lvl` child of an abstract numbering definition. Word
 * supports levels 0 through 8. The level text may reference previous levels
 * using WordprocessingML placeholders such as `%1.`, `%1.%2.`, or bullet text.
 */
struct NumberingLevelDefinition
{
    /// Zero-based list level. Values outside 0..8 are clamped by NumberingManager.
    int Level = 0;
    /// First number generated for this level when no instance override is present.
    int Start = 1;
    /// Numbering format, for example decimal, lowerLetter, upperRoman, or bullet.
    DocumentFormat::OpenXml::Wordprocessing::NumberFormatValues Format =
        DocumentFormat::OpenXml::Wordprocessing::NumberFormatValues::Decimal;
    /// Word level text pattern, for example "%1." or "%1.%2.".
    std::string LevelText;
    /// Separator written after the rendered numbering symbol.
    DocumentFormat::OpenXml::Wordprocessing::LevelSuffixValues Suffix =
        DocumentFormat::OpenXml::Wordprocessing::LevelSuffixValues::Tab;
    /// Alignment of the numbering symbol.
    DocumentFormat::OpenXml::Wordprocessing::LevelJustificationValues Justification =
        DocumentFormat::OpenXml::Wordprocessing::LevelJustificationValues::left;
    /// Optional paragraph style ID associated with this level.
    std::string ParagraphStyleId;
    /// Optional left indent for paragraphs using this level.
    std::optional<ExyokiOffice::MeasuringUnits> LeftIndent;
    /// Optional hanging indent for the numbering symbol.
    std::optional<ExyokiOffice::MeasuringUnits> HangingIndent;
    /// Optional lower level that restarts this level, stored as `w:lvlRestart`.
    std::optional<int> RestartAfterLevel;
    /// Whether Word should render all inherited numbering levels as decimal digits.
    bool LegalNumbering = false;
};

/**
 * @brief Creates or updates an abstract multi-level numbering definition.
 */
struct NumberingDefinition
{
    /// User-visible name stored in `w:name`.
    std::string Name;
    /// Levels to write. Word supports at most nine levels, indexed 0 through 8.
    std::vector<NumberingLevelDefinition> Levels;
};

/**
 * @brief Snapshot of a restart override stored on one numbering instance.
 */
struct NumberingLevelOverride
{
    /// Zero-based level affected by the override.
    int Level = 0;
    /// Restart value stored in `w:startOverride`.
    int Start = 1;
};

/**
 * @brief Snapshot of one numbering instance (`w:num`).
 */
struct NumberingInstanceInfo
{
    /// Numbering instance ID used by paragraph `w:numPr/w:numId`.
    int NumberingId = 0;
    /// Abstract numbering definition ID referenced by this instance.
    int AbstractNumberingId = 0;
    /// Per-level start overrides, if any.
    std::vector<NumberingLevelOverride> Overrides;
};

/**
 * @brief High-level manager for Word numbering definitions and instances.
 *
 * NumberingManager owns the common list workflow for Word documents: creating
 * nine-level abstract numbering definitions, creating instances that continue
 * an existing sequence, creating restart instances with `w:lvlOverride`, and
 * importing a numbering definition into another document with fresh IDs.
 *
 * The manager works with the WordprocessingML numbering part (`/word/numbering.xml`).
 * Definitions are shared templates (`w:abstractNum`), while instances (`w:num`)
 * are what paragraphs reference. Use ContinueList() when another paragraph
 * should stay in the same sequence, and RestartList() when the same definition
 * should begin again with one or more overridden start values.
 *
 * Example:
 * @code
 * using namespace ExyokiOffice::Word;
 *
 * auto editor = WordDocumentEditor::CreateNew();
 * auto numbering = editor->Numbering();
 *
 * NumberingDefinition outline;
 * outline.Name = "Outline";
 * for (int level = 0; level < 9; ++level)
 * {
 *     NumberingLevelDefinition item;
 *     item.Level = level;
 *     item.LevelText = "%" + std::to_string(level + 1) + ".";
 *     outline.Levels.push_back(item);
 * }
 *
 * auto list = numbering.EnsureMultilevelList(outline);
 * editor->AddParagraph("Chapter")->SetListStyle(list);
 * editor->AddParagraph("Topic")->SetNumbering(list.NumberingId, 1);
 *
 * auto restarted = numbering.RestartList(list.NumberingId, {{0, 1}});
 * editor->AddParagraph("Appendix")->SetListStyle(restarted);
 * @endcode
 *
 * @see WordDocumentEditor::Numbering
 * @see Paragraph::SetNumbering
 * @see Paragraph::SetListStyle
 */
class EXYOKIOFFICE_EXPORT NumberingManager
{
public:
    using Ptr = std::shared_ptr<NumberingManager>;

    /**
     * @brief Creates an invalid manager.
     */
    NumberingManager() = default;

    /**
     * @brief Wraps a Word document for numbering operations.
     */
    explicit NumberingManager(const WordDocument::Ptr& document);

    /**
     * @brief Checks whether this manager references a document.
     */
    [[nodiscard]] bool IsValid() const;

    /**
     * @brief Returns true when the document has a numbering part.
     */
    [[nodiscard]] bool HasNumberingPart() const;

    /**
     * @brief Creates or reuses a multi-level list definition by name.
     *
     * If a definition with the same name already exists, the method reuses it
     * and returns the first instance that references it, creating that instance
     * when needed. Otherwise it writes the provided levels as a new
     * `w:abstractNum` and creates the first `w:num` instance.
     *
     * @return ListStyle with the instance ID and level 0, or an empty ListStyle
     * on failure.
     */
    ListStyle EnsureMultilevelList(const NumberingDefinition& definition);

    /**
     * @brief Returns a style reference that continues an existing numbering instance.
     *
     * In WordprocessingML, paragraphs continue the same list sequence by using
     * the same `w:numId`. This method validates that the instance exists and
     * returns the same ID for fluent high-level code.
     */
    ListStyle ContinueList(int numberingId);

    /**
     * @brief Creates a restarted instance from an existing numbering instance.
     *
     * Each override writes one `w:lvlOverride` with `w:startOverride`. This is
     * the standard WordprocessingML way to restart a list while sharing the same
     * abstract multi-level definition.
     */
    ListStyle RestartList(int numberingId, const std::vector<NumberingLevelOverride>& overrides);

    /**
     * @brief Reads all concrete numbering instances in document order.
     */
    std::vector<NumberingInstanceInfo> Instances() const;

    /**
     * @brief Reads one numbering instance by ID.
     */
    std::optional<NumberingInstanceInfo> GetInstance(int numberingId) const;

    /**
     * @brief Reads one abstract numbering definition by abstract numbering ID.
     *
     * This is the read inverse of EnsureMultilevelList(): it returns the
     * definition name and the per-level format, level text, and start values
     * stored in `w:abstractNum/w:lvl`. Use GetInstance() first to map a
     * paragraph's numbering instance ID to its abstract numbering ID.
     *
     * @return Definition snapshot, or std::nullopt when the document has no
     * numbering part or no abstract definition with that ID.
     */
    std::optional<NumberingDefinition> GetDefinition(int abstractNumberingId) const;

    /**
     * @brief Imports a numbering definition and instance from another document.
     *
     * The target receives new abstract numbering and numbering instance IDs.
     * Level definitions and restart overrides are copied so paragraphs can be
     * assigned to the returned NumberingId without colliding with existing IDs.
     *
     * @return Imported list style, or an empty ListStyle on failure.
     */
    ListStyle ImportList(const NumberingManager& source, int sourceNumberingId);

private:
    WordDocument::Ptr m_document;
};

/**
 * @brief Simple run formatting preset for quick text insertion.
 *
 * The fields are optional in the sense that default values indicate "do not apply".
 */
struct RunStyle
{
    bool Bold = false;
    bool Italic = false;
    bool Underline = false;
    bool Strike = false;
    bool DoubleStrike = false;
    bool Caps = false;
    bool SmallCaps = false;
    bool NoProof = false;

    /// Optional highlight color.
    std::optional<DocumentFormat::OpenXml::Wordprocessing::HighlightColorValues> Highlight;
    /// Optional font family overrides.
    std::string AsciiFont;
    std::string HighAnsiFont;
    /// Optional font size (converted to points).
    std::optional<ExyokiOffice::MeasuringUnits> FontSize;
    /// Optional paragraph style ID to apply to the run.
    std::string StyleId;
    /// Optional run color value.
    std::optional<ExyokiOffice::Color> Color;
};

/**
 * @brief High-level manager for Word style definitions stored in a document.
 *
 * StyleManager is the main entry point for working with the WordprocessingML
 * styles part (`/word/styles.xml`) through the handwritten Word API. It lets
 * application code create, inspect, update, remove, and import named styles
 * without manually constructing the surrounding package relationship, the
 * `StyleDefinitionsPart`, or the common `w:style` metadata elements.
 *
 * In Word documents, styles are more than visual presets. They are stable
 * semantic identifiers used by paragraphs, runs, tables, numbering definitions,
 * templates, generated tables of contents, and document-level defaults. A
 * document generator that emits explicit formatting on every paragraph is hard
 * to maintain and produces documents that are awkward for users to restyle in
 * Word. A generator that emits meaningful styles can change the whole document
 * design by updating a few definitions, while the document content keeps
 * readable names such as `Heading1`, `BodyText`, or `WarningTable`.
 *
 * The manager supports the four Word style families:
 *  - Paragraph styles (`StyleType::Paragraph`), referenced from paragraph
 *    properties (`w:pPr/w:pStyle`).
 *  - Character styles (`StyleType::Character`), referenced from run properties
 *    (`w:rPr/w:rStyle`).
 *  - Table styles (`StyleType::Table`), used by table properties.
 *  - Numbering styles (`StyleType::Numbering`), used to name list-related
 *    style definitions.
 *
 * Each style definition exposes the common identity and organization metadata
 * that most generators need: style ID, display name, style type, default style
 * flag, `basedOn`, `next`, `link`, aliases, UI priority, and common visibility
 * flags. For advanced formatting, call GetLowLevelStyle() and append or edit
 * generated DOM children such as `StyleParagraphProperties`,
 * `StyleRunProperties`, or `StyleTableProperties`; the manager will preserve
 * those children during normal reads and style imports.
 *
 * StyleManager is intentionally lightweight. It stores only a shared reference
 * to the package and can be obtained repeatedly from WordDocumentEditor::Styles().
 * Read operations do not create a styles part. Mutating operations such as
 * CreateStyle(), SetLatentStyleSettings(), and ImportStyle() create the styles
 * part on demand when the document does not already have one.
 *
 * Example: create paragraph and character styles, then apply their IDs to
 * content:
 * @code
 * using namespace ExyokiOffice::Word;
 *
 * auto editor = WordDocumentEditor::CreateNew();
 * auto styles = editor->Styles();
 *
 * StyleDefinition body;
 * body.StyleId = "BodyText";
 * body.Name = "Body Text";
 * body.Type = StyleType::Paragraph;
 * body.IsDefault = true;
 * body.IsCustom = true;
 * body.UiPriority = 20;
 * styles.CreateStyle(body);
 *
 * StyleDefinition emphasis;
 * emphasis.StyleId = "InlineEmphasis";
 * emphasis.Name = "Inline Emphasis";
 * emphasis.Type = StyleType::Character;
 * emphasis.IsCustom = true;
 * styles.CreateStyle(emphasis);
 *
 * auto paragraph = editor->AddParagraph("Styled body text");
 * paragraph->SetStyleId("BodyText");
 * paragraph->AddRun(" emphasized")->SetStyleId("InlineEmphasis");
 * @endcode
 *
 * Example: express style relationships so Word's style gallery and Enter-key
 * behavior work naturally:
 * @code
 * StyleDefinition heading;
 * heading.StyleId = "ReportHeading";
 * heading.Name = "Report Heading";
 * heading.Type = StyleType::Paragraph;
 * heading.BasedOnStyleId = "Heading1";
 * heading.NextStyleId = "BodyText";
 * heading.LinkedStyleId = "ReportHeadingChar";
 * heading.IsPrimary = true;
 * styles.UpsertStyle(heading);
 * @endcode
 *
 * Example: copy a style from another document while defining conflict behavior:
 * @code
 * auto source = WordDocumentEditor::Open("template.docx");
 * auto target = WordDocumentEditor::CreateNew();
 *
 * auto importedId = target->Styles().ImportStyle(
 *     source->Styles(),
 *     "TemplateQuote",
 *     StyleCopyConflictPolicy::Rename);
 *
 * target->AddParagraph("Copied style")->SetStyleId(importedId);
 * @endcode
 *
 * Conflict policies are explicit:
 *  - KeepExisting returns the existing target ID without changing the target
 *    style when the requested ID already exists.
 *  - Replace removes the target style with that ID and imports the source style
 *    under the requested ID.
 *  - Rename imports the source style under a unique suffixed ID when the target
 *    ID is already taken.
 *
 * Example: configure latent styles. Latent styles control how built-in style
 * names that are not explicitly present in `styles.xml` appear in Word's UI:
 * @code
 * LatentStyleSettings latent;
 * latent.DefaultUiPriority = 99;
 * latent.DefaultSemiHidden = true;
 * styles.SetLatentStyleSettings(latent);
 *
 * LatentStyleException heading1;
 * heading1.Name = "heading 1";
 * heading1.UiPriority = 9;
 * heading1.PrimaryStyle = true;
 * heading1.UnhideWhenUsed = true;
 * styles.SetLatentStyleException(heading1);
 * @endcode
 *
 * @see WordDocumentEditor::Styles
 * @see StyleDefinition
 * @see LatentStyleSettings
 * @see LatentStyleException
 * @see StyleCopyConflictPolicy
 */
class EXYOKIOFFICE_EXPORT StyleManager
{
public:
    using Ptr = std::shared_ptr<StyleManager>;

    /**
     * @brief Creates an invalid manager.
     *
     * The default-constructed manager is useful as a placeholder. IsValid()
     * returns false and mutating operations return false or an empty string.
     */
    StyleManager() = default;

    /**
     * @brief Wraps a Word document for style operations.
     *
     * @param document Document whose main document part owns the styles part.
     */
    explicit StyleManager(const WordDocument::Ptr& document);

    /**
     * @brief Checks whether this manager references a document.
     */
    [[nodiscard]] bool IsValid() const;

    /**
     * @brief Returns the existing styles part without creating it.
     *
     * @return The styles part, or nullptr when the document has no styles part.
     */
    std::shared_ptr<Packaging::StyleDefinitionsPart> GetStylesPart() const;

    /**
     * @brief Returns the existing `w:styles` root without creating a styles part.
     * @return The `w:styles` root, or nullptr when the document has no styles part.
     */
    std::shared_ptr<DocumentFormat::OpenXml::Wordprocessing::Styles> GetRoot() const;

    /**
     * @brief Checks whether a style with the given style ID exists.
     */
    [[nodiscard]] bool HasStyle(std::string_view styleId) const;

    /**
     * @brief Reads all explicit style definitions in document order.
     */
    std::vector<StyleDefinition> Styles() const;

    /**
     * @brief Reads all explicit style definitions of a specific type.
     */
    std::vector<StyleDefinition> StylesByType(StyleType type) const;

    /**
     * @brief Reads common metadata for a single style.
     */
    std::optional<StyleDefinition> GetStyle(std::string_view styleId) const;

    /**
     * @brief Finds the style marked as default for a style type.
     */
    std::optional<StyleDefinition> GetDefaultStyle(StyleType type) const;

    /**
     * @brief Returns the generated DOM element for advanced style editing.
     *
     * Use this when the common StyleDefinition metadata is not enough, for
     * example to add `StyleRunProperties`, `StyleParagraphProperties`, table
     * conditional formatting, or other generated WordprocessingML children.
     * @return The matching `w:style` element, or nullptr when no style has that ID.
     */
    std::shared_ptr<DocumentFormat::OpenXml::Wordprocessing::Style> GetLowLevelStyle(
        std::string_view styleId) const;

    /**
     * @brief Creates a new style definition.
     *
     * @param definition Common style metadata to write.
     * @param replaceExisting When true, an existing style with the same ID is
     * updated instead of causing the operation to fail.
     * @return True when the style was created or replaced.
     */
    bool CreateStyle(const StyleDefinition& definition, bool replaceExisting = false);

    /**
     * @brief Updates an existing style definition by style ID.
     *
     * @return False when the style does not already exist.
     */
    bool UpdateStyle(const StyleDefinition& definition);

    /**
     * @brief Creates or updates a style definition by style ID.
     */
    bool UpsertStyle(const StyleDefinition& definition);

    /**
     * @brief Removes a style definition by style ID.
     *
     * This removes only the style definition. Existing paragraphs, runs, or
     * tables that still reference the removed ID are left unchanged so callers
     * can decide how to migrate content.
     */
    bool RemoveStyle(std::string_view styleId);

    /**
     * @brief Marks one style as the default for its type.
     *
     * Any existing default flag on another style of the same type is cleared.
     */
    bool SetDefaultStyle(StyleType type, std::string_view styleId);

    /**
     * @brief Clears the default flag on all styles of a given type.
     */
    bool ClearDefaultStyle(StyleType type);

    /**
     * @brief Reads document-level latent style defaults.
     */
    LatentStyleSettings GetLatentStyleSettings() const;

    /**
     * @brief Writes document-level latent style defaults.
     */
    bool SetLatentStyleSettings(const LatentStyleSettings& settings);

    /**
     * @brief Reads all explicit latent style exceptions.
     */
    std::vector<LatentStyleException> LatentStyleExceptions() const;

    /**
     * @brief Reads one latent style exception by built-in style name.
     */
    std::optional<LatentStyleException> GetLatentStyleException(std::string_view name) const;

    /**
     * @brief Creates or updates one latent style exception by name.
     */
    bool SetLatentStyleException(const LatentStyleException& exception);

    /**
     * @brief Removes one latent style exception by name.
     */
    bool RemoveLatentStyleException(std::string_view name);

    /**
     * @brief Imports one style definition from another StyleManager.
     *
     * The import preserves the full source `w:style` XML subtree, including
     * formatting children that are not represented by StyleDefinition. The
     * imported style ID is returned. An empty string indicates failure.
     *
     * @param source Source manager to copy from.
     * @param sourceStyleId Style ID in the source document.
     * @param policy Conflict behavior when the target style ID already exists.
     * @param preferredStyleId Optional target style ID. When empty, the source
     * style ID is used.
     * @return The target style ID that exists after the operation, or empty on failure.
     */
    std::string ImportStyle(const StyleManager& source,
                            std::string_view sourceStyleId,
                            StyleCopyConflictPolicy policy = StyleCopyConflictPolicy::KeepExisting,
                            std::string_view preferredStyleId = {});

private:
    WordDocument::Ptr m_document;
};

/**
 * @brief Editing restriction requested by Word document protection.
 *
 * The values mirror the `w:edit` attribute of `w:documentProtection`.
 */
enum class WordProtectionType
{
    /** No editing restriction; only formatting restrictions can still apply. */
    None,
    /** The document may not be edited at all. */
    ReadOnly,
    /** Only comments may be inserted, edited, and deleted. */
    Comments,
    /** Editing is allowed but revision tracking cannot be turned off. */
    TrackedChanges,
    /** Only form fields and explicitly unlocked regions may be edited. */
    Forms
};

/**
 * @brief Restrictions applied by Word document protection.
 *
 * Document protection is a user-interface restriction stored in the document
 * settings part. Word honors it, but the document itself stays plain, readable
 * OOXML: it is not encryption, and any tool that ignores the setting can still
 * read and rewrite every part. Use package encryption when confidentiality is
 * required; this API deliberately does not provide it.
 */
struct EXYOKIOFFICE_EXPORT WordProtectionOptions
{
    /** Editing restriction Word should enforce. */
    WordProtectionType Editing = WordProtectionType::ReadOnly;
    /** Restrict direct formatting to styles that are not locked. */
    bool RestrictFormattingToUnlockedStyles = false;
    /** Whether the restrictions are actually enforced rather than merely recorded. */
    bool Enforce = true;
    bool operator==(const WordProtectionOptions&) const = default;
};

/**
 * @brief Current Word document-protection state.
 *
 * The stored password verifier is deliberately not exposed. `hasPassword` only
 * reports whether removing protection requires the same password.
 */
struct EXYOKIOFFICE_EXPORT WordProtectionInfo
{
    /** Restrictions currently recorded on the document. */
    WordProtectionOptions Options;
    /** True when a password verifier is stored alongside the restrictions. */
    bool HasPassword = false;
    bool operator==(const WordProtectionInfo&) const = default;
};

/** @brief Error code returned by Word document-protection operations. */
enum class WordProtectionError
{
    None,
    InvalidDocument,
    InvalidOptions,
    PasswordMismatch,
    UnsupportedVerifier,
    WriteFailed
};

/** @brief Structured result of protecting or unprotecting a Word document. */
struct EXYOKIOFFICE_EXPORT WordProtectionResult
{
    WordProtectionError Error = WordProtectionError::None;
    std::string Message;

    /** @brief Returns true when the requested operation completed. */
    [[nodiscard]] bool Succeeded() const noexcept { return Error == WordProtectionError::None; }
    /** @brief Provides convenient success testing in conditional statements. */
    [[nodiscard]] explicit operator bool() const noexcept { return Succeeded(); }
};

/**
 * @brief Provides high-level editing APIs on top of WordDocument.
 *
 * The editor holds a `std::shared_ptr<WordDocument>` and keeps the package in
 * memory until it is saved explicitly. Block-level content is inserted through
 * BodyCursor objects obtained from Body(), Before(), and After(); the wrappers
 * they return (Paragraph, Run, Text, Image, Table, Section, and the rest) are
 * views over the underlying DOM rather than copies of it.
 *
 * The surface covers sections and page setup, headers and footers, styles
 * (StyleManager) and multilevel numbering (NumberingManager), paragraphs, runs
 * and text formatting, tables including merges and nested tables, images with
 * format detection and cropping, fields, bookmarks, hyperlinks, footnotes and
 * endnotes, comments, content controls, tracked revisions, charts, templates
 * and mail merge (MergeTemplate), document protection, and find/replace over
 * existing content. Anything the editor does not model is reachable through
 * GetDocument() and the typed DOM.
 *
 * @par Thread safety
 * An editor and every wrapper it hands out are **not** thread-safe. All
 * mutating calls on one editor, and any read that races with such a call, must
 * be externally serialized. Distinct editors over distinct documents may be
 * used concurrently; schema metadata is immutable and shared safely.
 *
 * @see WordDocument for the package lifecycle underneath this editor.
 */
class EXYOKIOFFICE_EXPORT WordDocumentEditor
{
public:
    using Ptr = std::shared_ptr<WordDocumentEditor>;

    /**
     * @brief Cursor used to insert block-level content into the main document body.
     *
     * BodyCursor is a lightweight value object. It does not own the document,
     * but it keeps the document alive through the same shared WordDocument
     * instance as the editor that created it. A cursor represents an insertion
     * point, not a selected range. Inserting content does not move existing
     * paragraphs or tables except for the natural shift caused by placing a new
     * XML block before another block.
     *
     * The default body cursor returned by WordDocumentEditor::Body inserts at
     * the logical end of the body. If the body ends with a section properties
     * element (`<w:sectPr>`), new blocks are inserted before that element,
     * because WordprocessingML requires final section properties to stay last.
     *
     * Typical usage:
     * @code
     * using ExyokiOffice::Word::WordDocumentEditor;
     *
     * auto editor = WordDocumentEditor::CreateNew();
     *
     * // Appends at the logical end of the document body.
     * auto title = editor->Body().InsertParagraph("Quarterly report");
     *
     * // Insert a paragraph before an existing paragraph wrapper.
     * editor->Before(title).InsertParagraph("Draft");
     *
     * // Insert a table after that paragraph.
     * auto table = editor->After(title).InsertTable(2, 3);
     * table->SetCellText(0, 0, "Region");
     * @endcode
     */
    class EXYOKIOFFICE_EXPORT BodyCursor
    {
    public:
        /**
         * @brief Creates an invalid cursor.
         *
         * Invalid cursors are returned only when the editor has no document or
         * an anchor wrapper does not point to a supported body child. Insert
         * methods on an invalid cursor return nullptr.
         */
        BodyCursor() = default;

        /**
         * @brief Checks whether this cursor has a document and a usable insertion point.
         *
         * @return True when insert operations can be attempted.
         */
        [[nodiscard]] bool IsValid() const;

        /**
         * @brief Inserts an empty paragraph at this cursor.
         *
         * @return A high-level paragraph wrapper, or nullptr when insertion fails.
         */
        std::shared_ptr<Paragraph> InsertParagraph() const;

        /**
         * @brief Inserts a paragraph containing one text run at this cursor.
         *
         * @param text Text content for the inserted paragraph.
         * @return A high-level paragraph wrapper, or nullptr when insertion fails.
         *
         * @code
         * auto paragraph = editor->Body().InsertParagraph("Inserted at the body end");
         * @endcode
         */
        std::shared_ptr<Paragraph> InsertParagraph(std::string_view text) const;

        /**
         * @brief Inserts a table at this cursor.
         *
         * The table is created with the requested number of rows and columns.
         * Passing zero rows creates an empty table wrapper; passing zero columns
         * creates rows with no cells, matching Table::AddRow behavior.
         *
         * @param rows Number of rows to create.
         * @param columns Number of cells to create per row.
         * @return A high-level table wrapper, or nullptr when insertion fails.
         *
         * @code
         * auto table = editor->Body().InsertTable(3, 2);
         * table->SetCellText(0, 0, "Name");
         * @endcode
         */
        std::shared_ptr<Table> InsertTable(Size rows, Size columns) const;

        /**
         * @brief Inserts a section break paragraph at this cursor.
         *
         * The inserted break is represented by an empty paragraph containing
         * `w:pPr/w:sectPr`. This is the standard location for all non-final
         * section properties in WordprocessingML. The returned Section wrapper
         * can be used immediately to set page size, margins, columns, or the
         * start type.
         *
         * @param startType How the new section should begin.
         * @return Section wrapper for the newly inserted section break, or nullptr on failure.
         *
         * @code
         * auto editor = WordDocumentEditor::CreateNew();
         * editor->Body().InsertParagraph("Portrait section");
         *
         * auto landscape = editor->Body().InsertSectionBreak(SectionStartType::NextPage);
         * landscape->SetPageSize({{11.0, MeasurementUnit::Inch},
         *                         {8.5, MeasurementUnit::Inch},
         *                         PageOrientation::Landscape});
         *
         * editor->Body().InsertParagraph("Landscape section");
         * @endcode
         */
        std::shared_ptr<Section> InsertSectionBreak(
            SectionStartType startType = SectionStartType::NextPage) const;

        /**
         * @brief Inserts a block-level content control (structured document tag) at this cursor.
         *
         * The new content control starts with an assigned, document-unique ID and
         * empty content; use ContentControl::AddParagraph()/SetText() to fill it in.
         *
         * @param tag Optional programmatic tag (`w:tag`).
         * @param alias Optional human-readable alias (`w:alias`) shown in Word's UI.
         * @return Content control wrapper, or nullptr when insertion fails.
         *
         * @code
         * auto control = editor->Body().InsertContentControl("customerName", "Customer Name");
         * control->SetText("Acme Corp.");
         * @endcode
         */
        std::shared_ptr<ContentControl> InsertContentControl(std::string_view tag = {},
                                                             std::string_view alias = {}) const;

        /**
         * @brief Inserts a deep copy of another document's body content at this cursor.
         *
         * Every top-level body block of `source` (paragraphs, tables, and any
         * section-break paragraphs) is copied in order to this cursor's
         * position, using the same placement rules as the other Insert*
         * methods. Nothing in `source` is modified; the copy is a fully
         * independent XML subtree in the target document.
         *
         * `source`'s own trailing body-level `<w:sectPr>` — the page setup
         * for the source document's own final section, not a break within
         * the copied content — is intentionally not copied. Section breaks
         * stored earlier in the source body (a `w:pPr/w:sectPr` on a normal
         * paragraph) travel with their owning paragraph exactly like any
         * other paragraph property and need no special handling.
         *
         * Copying merges every dependency the copied content can reference,
         * so the result never contains duplicate IDs or a reference that
         * fails to resolve in the target document:
         *  - paragraph, character, and table styles are imported into the
         *    target style catalog via StyleManager::ImportStyle, following
         *    `options.StyleConflictPolicy`; a style ID already used for an
         *    identical purpose in the target document is left alone, and a
         *    colliding-but-different style is imported under a fresh ID;
         *  - numbering lists (`w:numId`) are imported via
         *    NumberingManager::ImportList, so copied paragraphs keep their
         *    list formatting under freshly allocated target instance IDs;
         *  - bookmarks get fresh target IDs; a bookmark name that already
         *    exists in the target document is suffixed to stay unique, and
         *    any internal hyperlink inside the copied content that anchors
         *    to a renamed bookmark is rewritten to the new name;
         *  - footnotes and endnotes referenced by the copied content are
         *    copied into the target footnotes/endnotes part under fresh
         *    entry IDs; the standard Separator/ContinuationSeparator
         *    bookkeeping entries are reused rather than duplicated;
         *  - comments referenced by the copied content are copied into the
         *    target comments part (author, initials, date, and body text
         *    preserved) under a fresh comment ID;
         *  - content control IDs (`w:sdtId`) are reassigned to fresh target
         *    values;
         *  - images keep their appearance: each referenced image payload is
         *    copied into a new target image part (bytes and content type
         *    preserved, payload deduplication is not performed), the
         *    drawing's relationship is rewritten to the new part, and the
         *    drawing's shape IDs (`wp:docPr`/`pic:cNvPr`) are reassigned;
         *  - external hyperlinks get a new target relationship pointing at
         *    the same URL; internal hyperlinks are rewritten to the
         *    remapped bookmark name.
         *
         * @param source Document whose body content is copied. Its own
         * document is not modified.
         * @param options Controls style import conflict handling.
         * @return True when the cursor was valid and the source had a main
         * document part with a body (even if the body was empty). False
         * only when this cursor or the target document has no usable body.
         *
         * @code
         * auto target = WordDocumentEditor::CreateNew();
         * auto appendix = WordDocumentEditor::Open("appendix.docx");
         *
         * target->Body().InsertParagraph("Main report");
         * target->Body().InsertDocument(*appendix);
         * @endcode
         */
        bool InsertDocument(const WordDocumentEditor& source,
                            const DocumentMergeOptions& options = {}) const;

    private:
        enum class Placement
        {
            Start,
            End,
            Before,
            After
        };

        friend class WordDocumentEditor;

        BodyCursor(WordDocument::Ptr document,
                   Placement placement,
                   std::shared_ptr<ExyokiOffice::OpenXMLElement> anchor = nullptr);

        std::shared_ptr<DocumentFormat::OpenXml::Wordprocessing::Body> GetBody() const;
        std::shared_ptr<ExyokiOffice::OpenXMLElement> ResolveInsertionReference(
            const std::shared_ptr<DocumentFormat::OpenXml::Wordprocessing::Body>& body) const;

        WordDocument::Ptr m_document;
        Placement m_placement = Placement::End;
        std::shared_ptr<ExyokiOffice::OpenXMLElement> m_anchor;
    };

    /**
     * @brief Creates an editor without an attached document.
     *
     * Attach a document later with SetDocument() or CreateDefaultDocument().
     * Most callers should prefer the static factories CreateNew(), Open(),
     * or CreateFromTemplate().
     */
    WordDocumentEditor() = default;
    ~WordDocumentEditor();

    /**
     * @brief Creates an editor operating on an existing low-level document.
     *
     * @param document Low-level document to edit. May be nullptr; editing
     * calls are safe no-ops until a document is attached.
     */
    explicit WordDocumentEditor(const WordDocument::Ptr& document);

    /// Word package flavor (.docx/.dotx/.docm/.dotm); re-exported from the packaging layer.
    using WordprocessingDocumentType = ExyokiOffice::Packaging::WordprocessingDocumentType;

    /**
     * @brief Creates a new editor wrapper around the supplied document.
     *
     * This does not create or initialize a Word package by itself. Pass an
     * existing document when you want to edit a package opened through the
     * lower-level Packaging API, or use CreateNew for the common "start a new
     * document" workflow.
     *
     * @param document Optional low-level document instance to wrap.
     * @return Editor instance. The editor may contain no document when document is null.
     */
    static Ptr Create(const WordDocument::Ptr& document = nullptr);

    /**
     * @brief Creates a new initialized Word document and wraps it in an editor.
     *
     * The returned editor owns an in-memory package with the main document part,
     * core properties, extended properties, document settings, and an empty
     * body ready for AddParagraph, AddTable, and AddImageFromData. Nothing is
     * written to disk until SaveToFile is called.
     *
     * The document type controls the package flavor that Word sees:
     * Document creates a regular .docx document, Template creates a .dotx
     * template, MacroEnabledDocument creates a .docm document, and
     * MacroEnabledTemplate creates a .dotm template.
     *
     * @param type Desired Word package flavor. Document is the default.
     * @return Editor for the new document, or nullptr if initialization fails.
     *
     * @code
     * auto editor = ExyokiOffice::Word::WordDocumentEditor::CreateNew();
     * editor->AddParagraph("Hello from ExyokiOffice");
     * editor->SaveToFile("hello.docx");
     * @endcode
     */
    static Ptr CreateNew(WordprocessingDocumentType type = WordprocessingDocumentType::Document);

    /**
     * @brief Opens a Word package from disk and wraps it in an editor.
     *
     * The package is loaded into memory and detached from its source path. Call
     * SaveToFile explicitly when you want to persist changes. OpenSettings lets
     * advanced callers configure compatibility behavior, safety limits, and OPC
     * validation without leaving the high-level editor API.
     *
     * @param path Source .docx/.dotx/.docm/.dotm file path.
     * @param settings Optional open settings for compatibility and validation.
     * @param cancellationToken Optional non-owning token used to cancel opening.
     * @param error Optional out-parameter; when not null and the call fails, it
     *        receives why. See Packaging::OpenError.
     * @return Editor for the opened document, or nullptr when the package cannot be opened.
     */
    static Ptr Open(const std::filesystem::path& path,
                    const ExyokiOffice::Packaging::OpenSettings& settings = {},
                    const ICancellationToken* cancellationToken = nullptr,
                    ExyokiOffice::Packaging::OpenError* error = nullptr);

    /**
     * @brief Opens a Word package from an in-memory byte vector.
     *
     * The source buffer is not retained and later saves do not modify it. This
     * overload is useful for services and tests that already keep packages in
     * memory.
     *
     * @param packageBuffer Buffer containing a valid WordprocessingML ZIP package.
     * @param settings Optional open settings for compatibility and validation.
     * @param cancellationToken Optional non-owning token used to cancel opening.
     * @param error Optional out-parameter; when not null and the call fails, it
     *        receives why. See Packaging::OpenError.
     * @return Editor for the opened document, or nullptr when the package cannot be opened.
     */
    static Ptr Open(const std::vector<Byte>& packageBuffer,
                    const ExyokiOffice::Packaging::OpenSettings& settings = {},
                    const ICancellationToken* cancellationToken = nullptr,
                    ExyokiOffice::Packaging::OpenError* error = nullptr);

    /**
     * @brief Opens a Word package from a contiguous byte range.
     *
     * The bytes are parsed into a new in-memory document. The range must contain
     * a valid WordprocessingML ZIP package and may be released after this call.
     *
     * @param packageBuffer Byte range containing a valid WordprocessingML ZIP package.
     * @param settings Optional open settings for compatibility and validation.
     * @param cancellationToken Optional non-owning token used to cancel opening.
     * @param error Optional out-parameter; when not null and the call fails, it
     *        receives why. See Packaging::OpenError.
     * @return Editor for the opened document, or nullptr when the package cannot be opened.
     */
    static Ptr Open(std::span<const Byte> packageBuffer,
                    const ExyokiOffice::Packaging::OpenSettings& settings = {},
                    const ICancellationToken* cancellationToken = nullptr,
                    ExyokiOffice::Packaging::OpenError* error = nullptr);

    /**
     * @brief Creates a new regular document by cloning a Word template.
     *
     * The template is read into memory and converted to a regular Document
     * package type so the result saves as a .docx-style document. When
     * attachTemplate is true, the editor also preserves a Word attached-template
     * relationship that points back to templatePath.
     *
     * @param templatePath Source .dotx or .dotm template path.
     * @param attachTemplate Whether to store an external attached-template relationship.
     * @return Editor for the cloned document, or nullptr if the template cannot be opened.
     */
    static Ptr CreateFromTemplate(const std::filesystem::path& templatePath, bool attachTemplate = true);

    /**
     * @brief Creates a new document with the default Word parts.
     *
     * The created document is stored on this editor and initialized.
     * @param type Desired document type for the main part.
     */
    bool CreateDefaultDocument(WordprocessingDocumentType type = WordprocessingDocumentType::Document);

    /**
     * @brief Saves the current document to disk.
     *
     * This is a high-level shortcut for WordDocument::SaveToFile. It updates
     * document properties through the normal save lifecycle and returns false
     * when the editor has no document, the package cannot be serialized, the
     * destination cannot be written, or cancellation is requested.
     *
     * @param path Destination file path.
     * @param atomicSave When true, write to a temporary sibling file before publishing.
     * @param cancellationToken Optional non-owning token used to cancel saving.
     * @return True when the package was saved successfully.
     */
    bool SaveToFile(const std::filesystem::path& path,
                    bool atomicSave = true,
                    const ICancellationToken* cancellationToken = nullptr);

    /**
     * @brief Serializes the current document to an in-memory package.
     *
     * The returned bytes contain the complete ZIP package and can be written to
     * storage by the caller or passed to Open later. An empty vector indicates
     * that the editor has no document, save failed, or cancellation was requested.
     *
     * @param cancellationToken Optional non-owning token used to cancel saving.
     * @return Serialized Word package bytes, or an empty vector on failure.
     */
    std::vector<Byte> SaveToMemory(const ICancellationToken* cancellationToken = nullptr);

    /**
     * @brief Captures the complete Word package as an immutable edit memento.
     *
     * The snapshot includes every part, relationship, content type, binary
     * payload, and current XML DOM mutation. Creating it runs the normal
     * in-memory save preparation without writing to disk.
     *
     * @param cancellationToken Optional non-owning token used to cancel serialization.
     * @return Complete package memento, or `std::nullopt` when no document is
     * attached, serialization fails, or cancellation is requested.
     */
    std::optional<DocumentEditMemento> CreateMemento(
        const ICancellationToken* cancellationToken = nullptr);

    /**
     * @brief Replaces the current Word package with the state stored in a memento.
     *
     * The memento must have been created for the Word document family. A
     * successful restore invalidates all previously obtained high-level
     * wrappers, cursors, low-level parts, and DOM node references.
     *
     * @param memento Snapshot to restore.
     * @param cancellationToken Optional non-owning token used to cancel package loading.
     * @return True when the complete package was reopened and installed.
     */
    bool RestoreMemento(const DocumentEditMemento& memento,
                        const ICancellationToken* cancellationToken = nullptr);

    /**
     * @brief Starts a scope-bound transaction over the complete Word package.
     *
     * The returned transaction automatically restores its memento unless
     * Commit() is called. An inactive transaction indicates snapshot failure.
     * The transaction must not outlive this editor.
     *
     * @param cancellationToken Optional non-owning token used while capturing the snapshot.
     * @return Active transaction on success; otherwise an inactive transaction.
     */
    DocumentEditTransaction BeginTransaction(
        const ICancellationToken* cancellationToken = nullptr);

    /**
     * @brief Assigns the WordDocument instance that this editor manipulates.
     */
    void SetDocument(const WordDocument::Ptr& document);

    /**
     * @brief Returns the current WordDocument instance.
     */
    WordDocument::Ptr GetDocument() const;

    /**
     * @brief Returns the underlying low-level WordDocument API.
     * @return The document, or nullptr when no document is attached to this editor.
     */
    WordDocument::Ptr GetLowLevelApi() const;

    /**
     * @brief Returns the unified core/extended/custom document-properties editor.
     *
     * The returned editor is a lightweight view over the attached document;
     * it stays valid while this editor keeps the document alive. Calling this
     * without an attached document is undefined; check GetDocument() first
     * when the editor may be empty.
     */
    Packaging::DocumentProperties Properties() const;

    /**
     * @brief Returns a high-level manager for the document style definitions.
     */
    StyleManager Styles() const;

    /**
     * @return Essential typed theme settings of the document theme part, or
     *         `std::nullopt` when the document has no theme or the theme does
     *         not contain a complete color/font/format scheme.
     */
    std::optional<ExyokiOffice::ThemeSettings> ThemeSettings() const;

    /**
     * @brief Applies essential theme colors, fonts, and scheme names.
     *
     * The document must already have a theme part; call EnsureTheme() first
     * for documents created without one. Existing fill, line, effect, and
     * extension XML is preserved.
     *
     * @return `false` when the theme is missing or the settings are invalid.
     */
    bool SetThemeSettings(const ExyokiOffice::ThemeSettings& settings);

    /**
     * @return The complete lossless DrawingML theme XML, or `std::nullopt`
     *         when the main document part has no theme relationship.
     */
    std::optional<std::string> ThemeXml() const;

    /**
     * @brief Creates or replaces the document theme from a complete `a:theme` document.
     * @return `true` when the XML was parsed and attached; invalid XML leaves
     *         the current theme unchanged.
     */
    bool SetThemeXml(std::string xml);

    /**
     * @brief Ensures the document has a theme part.
     *
     * When the main document part has no theme relationship, a theme part with
     * the standard Office default theme is created. An existing theme is left
     * untouched.
     *
     * @return `true` when a theme part exists after the call.
     */
    bool EnsureTheme();

    /**
     * @brief Removes the document's theme relationship and part.
     * @return `true` when a theme was present and removed.
     */
    bool RemoveTheme();

    /**
     * @brief Returns a high-level manager for Word numbering definitions.
     */
    NumberingManager Numbering() const;

    /**
     * @brief Reads the Word document-protection state.
     *
     * @return Protection information, or `std::nullopt` when the document has
     *         no settings part, no `w:documentProtection` element, or an element
     *         that restricts nothing.
     */
    std::optional<WordProtectionInfo> GetDocumentProtection() const;

    /**
     * @brief Applies Word document protection.
     *
     * Protection restricts editing in word processors that honor the setting.
     * It is not encryption: the package stays plain OOXML and any consumer can
     * ignore or strip the restriction. The optional password is stored as an
     * ISO/IEC 29500 salted, iterated SHA-512 verifier, which only lets Word
     * recognize the correct password.
     *
     * Any existing `w:documentProtection` element is replaced atomically; the
     * settings part is created when the document does not have one yet.
     *
     * @param options Editing and formatting restrictions to record.
     * @param password Password required to remove protection; empty for none.
     * @return Structured result describing success or the reason for failure.
     */
    WordProtectionResult ProtectDocument(const WordProtectionOptions& options = {},
                                         std::string_view password = {});

    /**
     * @brief Removes Word document protection after verifying its password.
     *
     * Removing protection from an unprotected document succeeds and changes
     * nothing. A document whose verifier uses an algorithm this library cannot
     * compute reports WordProtectionError::UnsupportedVerifier.
     *
     * @param password Password supplied when protection was applied, or empty
     *        when protection was applied without a password.
     * @return Structured result describing success or the reason for failure.
     */
    WordProtectionResult UnprotectDocument(std::string_view password = {});

    /**
     * @brief Returns a cursor at the logical end of the main document body.
     *
     * This is the shortest high-level insertion path. It appends block content
     * while preserving a trailing `<w:sectPr>` as the last body child.
     *
     * @return Body cursor for appending before final section properties.
     *
     * @code
     * auto editor = ExyokiOffice::Word::WordDocumentEditor::CreateNew();
     * editor->Body().InsertParagraph("Last visible paragraph");
     * @endcode
     */
    BodyCursor Body() const;

    /**
     * @brief Returns a cursor at the beginning of the main document body.
     *
     * Inserted blocks become the first body children. If the body is empty, this
     * behaves like Body().
     *
     * @return Body cursor for inserting before the first existing body child.
     */
    BodyCursor BodyStart() const;

    /**
     * @brief Returns a cursor that inserts before an existing paragraph.
     *
     * @param paragraph Paragraph wrapper whose low-level element is used as the anchor.
     * @return Cursor positioned before paragraph, or an invalid cursor when paragraph is null.
     */
    BodyCursor Before(const std::shared_ptr<Paragraph>& paragraph) const;

    /**
     * @brief Returns a cursor that inserts after an existing paragraph.
     *
     * If the paragraph is immediately followed by final body section properties,
     * insertion still happens before those section properties.
     *
     * @param paragraph Paragraph wrapper whose low-level element is used as the anchor.
     * @return Cursor positioned after paragraph, or an invalid cursor when paragraph is null.
     */
    BodyCursor After(const std::shared_ptr<Paragraph>& paragraph) const;

    /**
     * @brief Returns a cursor that inserts before an existing table.
     *
     * @param table Table wrapper whose low-level element is used as the anchor.
     * @return Cursor positioned before table, or an invalid cursor when table is null.
     */
    BodyCursor Before(const std::shared_ptr<Table>& table) const;

    /**
     * @brief Returns a cursor that inserts after an existing table.
     *
     * @param table Table wrapper whose low-level element is used as the anchor.
     * @return Cursor positioned after table, or an invalid cursor when table is null.
     */
    BodyCursor After(const std::shared_ptr<Table>& table) const;

    /**
     * @brief Enumerates the top-level body blocks in document order.
     *
     * The returned wrappers reference the existing document DOM. Mutating a
     * returned paragraph or table changes the document owned by this editor.
     * The list is a snapshot: if you insert or remove body children, call
     * BodyBlocks again to get the new order.
     *
     * This method is intended as the main read path for opened documents:
     * it lets application code inspect and edit body content without traversing
     * generated DOM classes directly.
     *
     * @return Body block wrappers in the same order as the XML body children.
     *
     * @code
     * auto editor = WordDocumentEditor::Open("input.docx");
     * for (const auto& block : editor->BodyBlocks())
     * {
     *     if (auto paragraph = block.AsParagraph())
     *     {
     *         std::cout << paragraph->PlainText() << "\n";
     *     }
     * }
     * @endcode
     */
    std::vector<BodyBlock> BodyBlocks() const;

    /**
     * @brief Enumerates top-level paragraphs from the main document body.
     *
     * Nested table-cell paragraphs are not returned here; use
     * Table::Paragraphs on a table wrapper when you need paragraphs inside
     * tables.
     *
     * Paragraphs wrapped in a block-level structured document tag - a cover
     * page, a table of contents, a repeating section - are returned as though
     * the tag were not there, because to a reader it is not: the tag is a
     * container for the same body content. Use BodyBlocks() when the structure
     * itself matters.
     *
     * @return Paragraph wrappers in body order.
     */
    std::vector<std::shared_ptr<Paragraph>> Paragraphs() const;

    /**
     * @brief Enumerates top-level tables from the main document body.
     *
     * Nested tables are reachable from Table::Tables on the containing table.
     *
     * @return Table wrappers in body order.
     */
    std::vector<std::shared_ptr<Table>> Tables() const;

    /**
     * @brief Enumerates section properties from the main document.
     *
     * This includes both the final body-level `<w:sectPr>` and section breaks
     * stored under paragraph properties. The wrappers are returned in document
     * order and reference the existing DOM.
     *
     * @return Section wrappers for every `<w:sectPr>` in the main document.
     *
     * @code
     * auto sections = editor->Sections();
     * if (!sections.empty() && sections.back()->IsFinalBodySection())
     * {
     *     // The last wrapper represents the final document section.
     * }
     * @endcode
     */
    std::vector<std::shared_ptr<Section>> Sections() const;

    /**
     * @brief Enumerates every bookmark in the main document.
     *
     * Each bookmarkStart is paired with its matching bookmarkEnd (by ID) anywhere in the
     * document; the end wrapper is nullptr when no match is found (a malformed document).
     *
     * @return Bookmark wrappers in document order.
     */
    std::vector<std::shared_ptr<Bookmark>> Bookmarks() const;

    /**
     * @brief Enumerates every high-level field in the main document.
     *
     * Fields are returned in body paragraph order and include both simple
     * fields (`w:fldSimple`) and complex fields represented by `w:fldChar`
     * begin/separate/end markers. The wrappers reference the existing DOM, so
     * editing a returned field updates the document.
     *
     * The library does not evaluate field instructions. Fields whose result
     * depends on layout, such as `PAGE`, `NUMPAGES`, or `SECTIONPAGES`, are
     * exposed for inspection and can be marked dirty, but ExyokiOffice never
     * synthesizes a page-dependent result without a layout engine.
     *
     * @return Field wrappers in document order.
     */
    std::vector<std::shared_ptr<Field>> Fields() const;

    /**
     * @brief Enumerates tracked revisions in the main document body.
     *
     * The result includes run-level insertion/deletion containers (`w:ins`,
     * `w:del`, `w:moveFrom`, `w:moveTo`) and paragraph-property revision
     * markers that appear below the main document body. Each wrapper references
     * the existing DOM. Accepting or rejecting a wrapper mutates the document
     * immediately and invalidates that wrapper's structural assumptions.
     *
     * @return Revision wrappers in document order.
     */
    std::vector<std::shared_ptr<Revision>> Revisions() const;

    /**
     * @brief Accepts all tracked revisions currently present in the main body.
     *
     * Inserted content is unwrapped and kept; deleted and move-from content is
     * removed; move-to content is unwrapped and kept. Property-change markers
     * are removed while leaving the current properties in place.
     *
     * @return Number of revision wrappers that were processed.
     */
    Size AcceptAllRevisions();

    /**
     * @brief Rejects all tracked revisions currently present in the main body.
     *
     * Inserted and move-to content is removed; deleted and move-from content is
     * restored by unwrapping it into the document. Deleted text nodes are
     * converted back to normal `w:t` nodes during restoration.
     *
     * @return Number of revision wrappers that were processed.
     */
    Size RejectAllRevisions();

    /**
     * @brief Creates a simple paragraph-level tracked comparison against another document.
     *
     * This method intentionally implements a conservative compatibility stage:
     * it compares top-level paragraph plain text, marks paragraphs missing from
     * @p revised as deletions, and inserts paragraphs present only in @p revised
     * as tracked insertions. It does not try to emulate Word's full diff engine,
     * move detection, formatting comparison, table diffing, or layout-aware
     * behavior. The resulting revisions can be inspected, accepted, rejected,
     * saved, and round-tripped through the normal API.
     *
     * @param revised Revised document to compare against this editor's document.
     * @param author Revision metadata written to generated insertion/deletion containers.
     * @return Number of tracked revision containers created.
     */
    Size CompareWith(const WordDocumentEditor& revised,
                     const RevisionAuthor& author = {});

    /**
     * @brief Merges literal data into MERGEFIELD fields, bookmarks, and repeating regions.
     *
     * Scalar merge replaces cached results for non-layout `MERGEFIELD` fields whose
     * field name exists in TemplateMergeData::Values. The field instruction is
     * preserved so the document remains recognizable as a template if callers
     * inspect it later. Bookmarks with matching scalar names are replaced only
     * when their start and end markers live in the same paragraph; malformed or
     * cross-paragraph bookmarks are left unchanged.
     *
     * Repeating regions are driven by `MERGEFIELD TableStart:RegionName` and
     * `MERGEFIELD TableEnd:RegionName` markers in top-level body paragraphs.
     * The content between the marker paragraphs is copied once per supplied
     * row. Each copy is merged using the row values only, then the original
     * template content and marker paragraphs are removed. Empty or missing row
     * data removes the whole region without inserting rows.
     *
     * No expression language is evaluated. Field switches are ignored after the
     * field name is parsed, and all inserted text is written as plain literal
     * WordprocessingML text.
     *
     * @param data Literal scalar and region data to merge into this document.
     * @param preserveSpaces Whether replacement text should preserve leading and
     *        trailing spaces in generated text nodes.
     * @return Counts describing the merge operations that changed the document.
     */
    TemplateMergeResult MergeTemplate(const TemplateMergeData& data, bool preserveSpaces = true);

    /**
     * @brief Finds the first bookmark with the given name.
     *
     * @param name Bookmark name to search for.
     * @return Matching bookmark, or nullptr when not found.
     */
    std::shared_ptr<Bookmark> FindBookmark(std::string_view name) const;

    /**
     * @brief Enumerates every footnote entry in the document's footnotes part.
     *
     * The result includes the automatically managed Separator and
     * ContinuationSeparator entries alongside normal note content; use
     * Note::GetEntryType() to distinguish them. Returns an empty vector when
     * the document has no footnotes part yet.
     *
     * @return Note wrappers in footnotes-part order.
     */
    std::vector<std::shared_ptr<Note>> Footnotes() const;

    /**
     * @brief Enumerates every endnote entry in the document's endnotes part.
     *
     * @return Note wrappers in endnotes-part order. See Footnotes() for details
     * on Separator/ContinuationSeparator entries.
     */
    std::vector<std::shared_ptr<Note>> Endnotes() const;

    /**
     * @brief Finds a footnote entry by ID.
     *
     * @return Matching note, or nullptr when not found.
     */
    std::shared_ptr<Note> FindFootnote(int id) const;

    /**
     * @brief Finds an endnote entry by ID.
     *
     * @return Matching note, or nullptr when not found.
     */
    std::shared_ptr<Note> FindEndnote(int id) const;

    /**
     * @brief Enumerates every comment entry in the document's comments part.
     *
     * @return Comment wrappers in comments-part order. Returns an empty vector
     * when the document has no comments part.
     */
    std::vector<std::shared_ptr<Comment>> Comments() const;

    /**
     * @brief Finds a comment entry by ID.
     *
     * @return Matching comment, or nullptr when not found.
     */
    std::shared_ptr<Comment> FindComment(int id) const;

    /**
     * @brief Enumerates every content control (`<w:sdt>`) in the main document body.
     *
     * Both block-level and inline content controls are returned, in document
     * order; ContentControl::Level() reports which shape each entry has.
     *
     * @return Content control wrappers in document order.
     */
    std::vector<std::shared_ptr<ContentControl>> ContentControls() const;

    /**
     * @brief Finds a content control by ID.
     *
     * @return Matching content control, or nullptr when not found.
     */
    std::shared_ptr<ContentControl> FindContentControl(int id) const;

    /**
     * @brief Ensures and returns the final body-level section properties.
     *
     * Word stores the document's final section metadata as a trailing
     * `w:sectPr` child of `w:body`. If the element is missing, this method
     * creates it as the last body child. Later BodyCursor insertions continue
     * to preserve it at the end.
     *
     * @return Final section wrapper, or nullptr when the editor has no usable document body.
     *
     * @code
     * auto finalSection = editor->EnsureFinalSection();
     * finalSection->SetMargins({{1.0, MeasurementUnit::Inch},
     *                           {1.0, MeasurementUnit::Inch},
     *                           {1.0, MeasurementUnit::Inch},
     *                           {1.0, MeasurementUnit::Inch},
     *                           {0.5, MeasurementUnit::Inch},
     *                           {0.5, MeasurementUnit::Inch},
     *                           {}});
     * @endcode
     */
    std::shared_ptr<Section> EnsureFinalSection();

    /**
     * @brief Appends an empty paragraph to the document body.
     */
    std::shared_ptr<Paragraph> AddParagraph();

    /**
     * @brief Appends a paragraph with a single text run.
     */
    std::shared_ptr<Paragraph> AddParagraph(std::string_view text);

    /**
     * @brief Appends a paragraph containing a single explicit page break.
     *
     * Content added after this call starts on a new page. To force a page
     * break before an existing paragraph without adding a new one, use
     * Paragraph::SetPageBreakBefore() instead.
     *
     * @return The paragraph holding the page break, or nullptr if the
     * document is invalid.
     */
    std::shared_ptr<Paragraph> AddPageBreak();

    /**
     * @brief Appends a heading paragraph using the built-in heading styles.
     *
     * Ensures that the paragraph styles "Normal" and "HeadingN" exist (new
     * documents created by this library start without any styles) and applies
     * "HeadingN" to a new paragraph. When the document already defines the
     * style — for example a document created by Word or from a template — the
     * existing definition is left untouched, so headings match the document's
     * design. Styles created by this method carry Word-like defaults: bold
     * colored text, size decreasing with the level, keep-with-next
     * pagination, and the outline level used by the navigation pane and
     * AddTableOfContents().
     *
     * @param text Heading text.
     * @param level Heading level 1-9; values outside this range are clamped.
     * @return The heading paragraph, or nullptr if the document is invalid.
     *
     * @code
     * editor->AddHeading("Introduction");
     * editor->AddHeading("Background", 2);
     * editor->AddParagraph("Body text...");
     * @endcode
     */
    std::shared_ptr<Paragraph> AddHeading(std::string_view text, int level = 1);

    /**
     * @brief Appends a table-of-contents field.
     *
     * The paragraph receives a complex `TOC` field covering the requested
     * heading levels with hyperlinked entries. ExyokiOffice never evaluates
     * fields, so the stored result is placeholder text and the field is
     * marked dirty; Word (or another layout-aware consumer) populates the
     * actual entries when the document is opened and fields are updated.
     *
     * @param fromLevel First heading level to include (1-9).
     * @param toLevel Last heading level to include (1-9, clamped to at least
     * fromLevel).
     * @return The paragraph holding the TOC field, or nullptr if the document
     * is invalid.
     *
     * @code
     * editor->AddTableOfContents();      // headings 1-3
     * editor->AddTableOfContents(1, 5);  // headings 1-5
     * @endcode
     */
    std::shared_ptr<Paragraph> AddTableOfContents(int fromLevel = 1, int toLevel = 3);

    /**
     * @brief Appends a table to the document body.
     *
     * @param rows Number of rows to create.
     * @param columns Number of columns to create.
     */
    std::shared_ptr<Table> AddTable(Size rows, Size columns);

    /**
     * @brief Appends an image loaded from disk.
     *
     * @param filePath Image file path.
     * @param width Image width expressed in any supported unit.
     * @param height Image height expressed in any supported unit.
     * @param layout Layout mode (inline or floating).
     * @param wrap Wrap mode for floating images.
     */
    std::shared_ptr<Image> AddImageFromFile(const std::filesystem::path& filePath,
                                            const ExyokiOffice::MeasuringUnits& width,
                                            const ExyokiOffice::MeasuringUnits& height,
                                            ImageLayout layout = ImageLayout::Inline,
                                            ImageWrap wrap = ImageWrap::Square);

    /**
     * @brief Appends an image from raw bytes.
     *
     * @param data Image bytes to store in the package.
     * @param contentType MIME content type (e.g. image/png). Optional.
     * @param width Image width expressed in any supported unit.
     * @param height Image height expressed in any supported unit.
     * @param layout Layout mode (inline or floating).
     * @param wrap Wrap mode for floating images.
     */
    std::shared_ptr<Image> AddImageFromData(std::vector<Byte> data,
                                            std::string_view contentType,
                                            const ExyokiOffice::MeasuringUnits& width,
                                            const ExyokiOffice::MeasuringUnits& height,
                                            ImageLayout layout = ImageLayout::Inline,
                                            ImageWrap wrap = ImageWrap::Square);

    /**
     * @brief Appends an image loaded from disk using its natural size and detected format.
     *
     * This overload calls DetectImageFormat() on the file bytes instead of trusting the
     * file extension or requiring the caller to compute a physical size. The image is
     * inserted at its native pixel size, converted to EMUs using the resolution reported
     * by the payload (or 96 DPI when the format does not carry resolution metadata).
     *
     * @param filePath Image file path.
     * @param layout Layout mode (inline or floating).
     * @param wrap Wrap mode for floating images.
     * @return Image wrapper, or nullptr when the file cannot be read or its format is
     * not recognized by DetectImageFormat().
     *
     * @code
     * auto image = editor->AddImageFromFile("logo.png");
     * @endcode
     */
    std::shared_ptr<Image> AddImageFromFile(const std::filesystem::path& filePath,
                                            ImageLayout layout = ImageLayout::Inline,
                                            ImageWrap wrap = ImageWrap::Square);

    /**
     * @brief Appends an image from raw bytes using its natural size and detected format.
     *
     * @param data Image bytes to store in the package.
     * @param layout Layout mode (inline or floating).
     * @param wrap Wrap mode for floating images.
     * @return Image wrapper, or nullptr when the format is not recognized by
     * DetectImageFormat().
     *
     * @see AddImageFromFile(const std::filesystem::path&, ImageLayout, ImageWrap)
     */
    std::shared_ptr<Image> AddImageFromData(std::vector<Byte> data,
                                            ImageLayout layout = ImageLayout::Inline,
                                            ImageWrap wrap = ImageWrap::Square);

    /**
     * @brief Enumerates every chart embedded directly in the document body.
     *
     * Scans the main document part for `<w:drawing>` references to a chart
     * part (the same DrawingML chart schema used by
     * @ref ExyokiOffice::Excel::Worksheet::AddChart) and parses each
     * referenced chart part's title, plot type, and series, including the
     * embedded value/label caches, so numbers and category labels are
     * available without needing an embedded workbook or live recalculation.
     * This covers charts pasted or produced by any Open XML tool - Word,
     * Excel, PowerPoint, or Open-XML-SDK - not only charts created by this
     * library, since no Word-side API here creates new chart anchors.
     *
     * Only the document body is scanned; headers, footers, footnotes,
     * endnotes, and comments are not, since embedded charts are exceedingly
     * rare there.
     *
     * @return One entry per embedded chart, in document order. Empty when
     * the document has no main document part or no embedded charts.
     *
     * @code
     * for (const auto& chart : editor->Charts())
     * {
     *     std::cout << chart.title << ": " << chart.Series.size() << " series\n";
     * }
     * @endcode
     */
    std::vector<WordChartInfo> Charts() const;

    /**
     * @brief Replaces one embedded chart's title and series cache in place.
     *
     * Only the chart part XML is rewritten (title and per-series cached
     * values/labels); the chart's position in the document, its
     * relationship id, and any embedded workbook are left untouched. The
     * chart's `<c:ser>` elements are rebuilt to match @p seriesData exactly,
     * so series may be added, removed, or reordered relative to what the
     * chart previously had. Each rebuilt series keeps its previous `<c:f>`
     * formula reference (by position) when one existed; a series beyond the
     * previous count gets an empty formula, since there is no known source
     * range for it. Rebuilding a series' XML also drops any per-series
     * styling that previously existed on it (data point colors, markers,
     * data labels); this mirrors the shape @ref
     * ExyokiOffice::Excel::Worksheet::AddChart itself writes.
     *
     * For @ref WordChartType::XyScatter and @ref WordChartType::Bubble
     * charts, @p seriesData entries' `categories` strings are parsed as
     * numbers and written as the X value series; text that fails to parse
     * is treated as 0.
     *
     * @param chartRelationshipId Identifies the chart, as returned in
     * @ref WordChartInfo::relationshipId by @ref Charts.
     * @param seriesData Replacement series (name, values, optional category
     * labels). At least one series is required.
     * @param title Replacement chart title. std::nullopt leaves the
     * existing title (or absence of one) unchanged; an empty string removes
     * the title.
     * @return True when a chart with @p chartRelationshipId was found, its
     * chart part XML was well-formed, and it had a recognized plot-type
     * group to update.
     *
     * @code
     * auto chart = editor->Charts().front();
     * editor->UpdateChartData(chart.RelationshipId,
     *                         {{"Actuals", {12.0, 18.0, 9.0}},
     *                          {"Forecast", {10.0, 20.0, 15.0}}},
     *                         "Q3 Results");
     * @endcode
     */
    bool UpdateChartData(const std::string& chartRelationshipId,
                         const std::vector<WordChartSeries>& seriesData,
                         std::optional<std::string> title = std::nullopt);

    /**
     * @brief Reads an embedded chart's embedded workbook payload, if any.
     *
     * Charts copied from Excel typically carry a full workbook (`.xlsx`)
     * behind the chart part, used by Word/Excel/PowerPoint to power "Edit
     * Data in Excel". The returned bytes are a complete, independent Excel
     * package: open them with the `std::span<const Byte>` overload
     * of @ref ExyokiOffice::Excel::ExcelDocumentEditor::Open to inspect or
     * edit the source data.
     *
     * @param chartRelationshipId Identifies the chart, as returned in
     * @ref WordChartInfo::relationshipId by @ref Charts.
     * @return The embedded package bytes, or std::nullopt when the chart is
     * not found or carries no embedded workbook (see
     * @ref WordChartInfo::hasEmbeddedWorkbook).
     */
    std::optional<std::vector<Byte>> GetChartEmbeddedWorkbook(const std::string& chartRelationshipId) const;

    /**
     * @brief Replaces (or attaches) an embedded chart's embedded workbook payload.
     *
     * Use this after editing the workbook obtained from @ref
     * GetChartEmbeddedWorkbook (for example via
     * @ref ExyokiOffice::Excel::ExcelDocumentEditor::SaveToMemory) to
     * write the updated source data back. This does not touch the chart's
     * own cached series values; call @ref UpdateChartData as well if the
     * chart's displayed cache should match the new workbook contents.
     *
     * @param chartRelationshipId Identifies the chart, as returned in
     * @ref WordChartInfo::relationshipId by @ref Charts.
     * @param workbookBytes Complete `.xlsx` package bytes.
     * @return True when a chart with @p chartRelationshipId was found. A
     * chart with no pre-existing embedded workbook gets one attached.
     */
    bool SetChartEmbeddedWorkbook(const std::string& chartRelationshipId,
                                  std::span<const Byte> workbookBytes);

    /**
     * @brief Ensures a standard numbered list definition exists.
     *
     * This creates (or reuses) a numbering definition part and a single-level
     * abstract numbering definition named as requested. The returned ListStyle
     * contains the numbering instance ID to apply to paragraphs.
     *
     * @param name User-visible list name stored in the numbering definition.
     * @param format Numbering format (decimal, roman, letters, ...).
     * @param levelText Level text pattern (e.g. "%1.").
     * @return List style reference containing numbering ID and default level.
     *
     * @code
     * using namespace ExyokiOffice::Word;
     * auto style = editor->EnsureNumberedListStyle("Steps");
     * editor->AddParagraph("First")->SetListStyle(style);
     * editor->AddParagraph("Second")->SetListStyle(style);
     * @endcode
     */
    ListStyle EnsureNumberedListStyle(
        std::string_view name = "NumberedList",
        DocumentFormat::OpenXml::Wordprocessing::NumberFormatValues format =
            DocumentFormat::OpenXml::Wordprocessing::NumberFormatValues::Decimal,
        std::string_view levelText = "%1.");

    /**
     * @brief Ensures a standard bulleted list definition exists.
     *
     * @param name User-visible list name stored in the numbering definition.
     * @param bulletText Bullet glyph to use (ASCII by default).
     * @return List style reference containing numbering ID and default level.
     *
     * @code
     * using namespace ExyokiOffice::Word;
     * auto style = editor->EnsureBulletedListStyle("BulletList", "*");
     * editor->AddParagraph("First")->SetListStyle(style);
     * editor->AddParagraph("Second")->SetListStyle(style);
     * @endcode
     */
    ListStyle EnsureBulletedListStyle(std::string_view name = "BulletedList",
                                      std::string_view bulletText = "*");

private:
    WordDocument::Ptr m_document;
    std::shared_ptr<detail::DocumentEditTransactionOwner> m_transactionOwner;
};

/**
 * @brief Captures paragraph spacing values expressed in physical units.
 */
struct ParagraphSpacing
{
    std::optional<ExyokiOffice::MeasuringUnits> Before;
    std::optional<ExyokiOffice::MeasuringUnits> After;
    std::optional<ExyokiOffice::MeasuringUnits> Line;
    DocumentFormat::OpenXml::Wordprocessing::LineSpacingRuleValues LineRule =
        DocumentFormat::OpenXml::Wordprocessing::LineSpacingRuleValues::Auto;
};

/**
 * @brief Captures paragraph spacing values expressed in line units.
 */
struct ParagraphSpacingLines
{
    std::optional<int> BeforeLines;
    std::optional<int> AfterLines;
    std::optional<int> LineLines;
    DocumentFormat::OpenXml::Wordprocessing::LineSpacingRuleValues LineRule =
        DocumentFormat::OpenXml::Wordprocessing::LineSpacingRuleValues::Auto;
};

/**
 * @brief Captures paragraph indentation values expressed in physical units.
 */
struct ParagraphIndentation
{
    std::optional<ExyokiOffice::MeasuringUnits> Left;
    std::optional<ExyokiOffice::MeasuringUnits> Right;
    std::optional<ExyokiOffice::MeasuringUnits> FirstLine;
    std::optional<ExyokiOffice::MeasuringUnits> Hanging;
};

/**
 * @brief Captures paragraph numbering (list) information.
 */
struct ParagraphNumbering
{
    std::optional<int> NumberingId;
    std::optional<int> Level;
};

/**
 * @brief Captures paragraph shading information.
 */
struct ParagraphShading
{
    std::string FillColor;
    DocumentFormat::OpenXml::Wordprocessing::ShadingPatternValues Pattern =
        DocumentFormat::OpenXml::Wordprocessing::ShadingPatternValues::Clear;
    std::string PatternColor;
};

/**
 * @brief Captures the common paragraph formatting exposed by Paragraph.
 *
 * Paragraph formatting affects the block as a whole: style, alignment,
 * spacing, indentation, numbering, borders/shading, and pagination behavior.
 * This struct intentionally mirrors the high-level setter API so callers can
 * inspect existing documents without dropping down to the generated DOM.
 */
struct ParagraphFormatting
{
    std::string StyleId;
    std::optional<DocumentFormat::OpenXml::Wordprocessing::JustificationValues> Alignment;
    std::optional<ParagraphSpacing> Spacing;
    std::optional<ParagraphSpacingLines> SpacingLines;
    std::optional<ParagraphIndentation> Indentation;
    std::optional<ParagraphNumbering> Numbering;
    std::optional<ParagraphShading> Shading;
    bool HasBorders = false;
    std::optional<bool> KeepWithNext;
    std::optional<bool> KeepLines;
    std::optional<bool> PageBreakBefore;
    std::optional<bool> WidowControl;
};

/**
 * @brief Identifies a span of text within a paragraph.
 *
 * A ContentRange describes offsets into the text of a paragraph, which is what
 * Paragraph::PlainText() returns and what Paragraph::Runs() concatenates: the
 * runs of the paragraph in document order, including those inside hyperlinks,
 * tracked insertions, content controls, smart tags and simple fields, with a
 * `w:tab` counting as a tab character and a `w:br` as a newline. Text a reader
 * does not see is not part of it — tracked deletions, field instructions, and
 * the separate content of a text box anchored in the paragraph.
 *
 * @warning Offsets are **bytes**, not code points. Text is UTF-8, so a
 * character outside ASCII spans several offsets, and a range that starts or
 * ends in the middle of one describes half a character: Find() and FindAll()
 * never produce such a range, but one built by hand from a character count
 * will, and replacing it produces invalid UTF-8.
 *
 * Ranges are produced by Paragraph::Find()/FindAll() or built directly from
 * known offsets, and are consumed by Paragraph::GetText()/ReplaceText(). A
 * range with Start == End denotes an insertion point rather than a selection.
 */
struct ContentRange
{
    /// Inclusive start offset, in UTF-8 bytes, from the beginning of the paragraph's text.
    Size Start = 0;
    /// Exclusive end offset, in UTF-8 bytes, from the beginning of the paragraph's text.
    Size End = 0;

    /// @brief Returns the number of bytes covered by this range.
    Size Length() const noexcept
    {
        return End > Start ? End - Start : 0;
    }
};

/**
 * @brief High-level paragraph wrapper with fluent formatting and inspection helpers.
 *
 * This class represents a single WordprocessingML paragraph (`<w:p>`). It is a
 * convenience layer on top of the generated DOM types and focuses on common tasks
 * such as adding runs/text, setting, reading, and clearing alignment, spacing,
 * indentation, numbering, shading, and basic pagination behavior.
 *
 * If you are new to Word documents, a paragraph is the basic block element that
 * contains one or more runs (pieces of text with formatting). Most formatting
 * related to layout is stored in `<w:pPr>` (paragraph properties), while the
 * actual text lives in `<w:r>` / `<w:t>`.
 *
 * Typical usage is:
 *  - Create a paragraph (usually via WordDocumentEditor).
 *  - Add runs/text.
 *  - Apply formatting (alignment, spacing, indentation, numbering).
 *
 * @code
 * using namespace ExyokiOffice;
 * using namespace ExyokiOffice::Word;
 *
 * auto editor = WordDocumentEditor::Create();
 * editor->CreateDefaultDocument();
 *
 * auto paragraph = editor->AddParagraph("Hello, world!");
 * paragraph->SetAlignment(DocumentFormat::OpenXml::Wordprocessing::JustificationValues::Center);
 *
 * paragraph->SetSpacing(MeasuringUnits(6.0, MeasurementUnit::Point),
 *                       MeasuringUnits(6.0, MeasurementUnit::Point));
 * paragraph->SetIndentation(MeasuringUnits(1.0, MeasurementUnit::Centimeter),
 *                           std::nullopt);
 * paragraph->SetKeepWithNext(true);
 *
 * ParagraphFormatting formatting;
 * if (auto formatting = paragraph->GetFormatting(); formatting && formatting->Alignment)
 * {
 *     // Existing documents can be inspected without using generated DOM types.
 * }
 *
 * paragraph->ClearSpacing()
 *          .ClearIndentation()
 *          .ClearPagination();
 * @endcode
 */
class EXYOKIOFFICE_EXPORT Paragraph
{
public:
    using Ptr = std::shared_ptr<Paragraph>;

    /**
     * @brief Wraps an existing low-level paragraph element.
     *
     * @param paragraph Low-level `<w:p>` element to wrap.
     * @param mainDocumentPart Optional main document part, used for the document-wide
     * services a paragraph reaches into: footnotes, endnotes, comments, and the
     * identifier counters shared with the rest of the document. Paragraphs returned by
     * WordDocumentEditor::AddParagraph(), WordDocumentEditor::Paragraphs(), and
     * WordDocumentEditor::BodyCursor::InsertParagraph() already have this attached.
     * @param owningPart Part this paragraph actually lives in, which is where its
     * relationships belong. Leave it null for a paragraph in the main document; pass the
     * header, footer, footnotes, endnotes, or comments part for a paragraph in one of
     * those, or a hyperlink added here would write its relationship into
     * `document.xml.rels` and resolve against a wholly unrelated set of targets.
     * The wrappers that hand out paragraphs (HeaderFooterContent::Paragraphs(),
     * Note::Paragraphs(), Comment::Paragraphs(), Table::Paragraphs()) pass their own
     * part, so this matters only when wrapping an element by hand.
     */
    explicit Paragraph(const std::shared_ptr<DocumentFormat::OpenXml::Wordprocessing::Paragraph>& paragraph,
                       const std::shared_ptr<Packaging::MainDocumentPart>& mainDocumentPart = nullptr,
                       const std::shared_ptr<OpenXmlPackagePart>& owningPart = nullptr);

    /**
     * @brief Returns the underlying low-level paragraph element.
     *
     * Use this when you need access to properties or child elements that are not
     * exposed through the high-level wrapper.
     * @return The wrapped element, or nullptr when this wrapper is detached.
     */
    std::shared_ptr<DocumentFormat::OpenXml::Wordprocessing::Paragraph> GetLowLevelApi() const;

    /**
     * @brief Attaches (or replaces) the main document part used for document-wide features.
     *
     * @param mainDocumentPart Main document part of this paragraph's package.
     * @return Reference to this paragraph for fluent chaining.
     */
    Paragraph& AttachMainDocumentPart(const std::shared_ptr<Packaging::MainDocumentPart>& mainDocumentPart);

    /**
     * @brief Attaches (or replaces) the part this paragraph lives in.
     *
     * Relationships a paragraph creates - a hyperlink target, an image - are
     * stored by the part that contains the reference, not by the document. A
     * paragraph in `footnotes.xml` whose owning part is the main document part
     * writes its hyperlink into `document.xml.rels`, where Word will not look
     * for it, and reads back whatever relationship happens to share the
     * identifier.
     *
     * @param owningPart Part that stores this paragraph; null means the main document part.
     * @return Reference to this paragraph for fluent chaining.
     */
    Paragraph& AttachOwningPart(const std::shared_ptr<OpenXmlPackagePart>& owningPart);

    /**
     * @brief Part whose relationships this paragraph reads and writes.
     *
     * The owning part when one was attached, the main document part otherwise,
     * and null when neither is - which is when AddHyperlink() cannot record a
     * target and says so by returning nullptr.
     */
    [[nodiscard]] std::shared_ptr<OpenXmlPackagePart> OwningPart() const;

    /**
     * @brief Appends an empty run (`<w:r>`) to the paragraph.
     *
     * @return A high-level Run wrapper or nullptr if the paragraph is invalid.
     */
    std::shared_ptr<Run> AddRun();

    /**
     * @brief Appends text as a new run to the paragraph.
     *
     * @param text Text content to insert.
     * @param preserveSpaces Writes `xml:space="preserve"`; on by default, see Run::AddText.
     * @return A Text wrapper or nullptr if the paragraph is invalid.
     */
    std::shared_ptr<Text> AddText(std::string_view text, bool preserveSpaces = true);

    /**
     * @brief Appends a run with text and applies a formatting preset.
     *
     * @param text Text content to insert.
     * @param style Formatting preset to apply.
     * @param preserveSpaces Writes `xml:space="preserve"`; on by default, see Run::AddText.
     * @return A Run wrapper or nullptr if the paragraph is invalid.
     */
    std::shared_ptr<Run> AddRun(std::string_view text,
                                const RunStyle& style,
                                bool preserveSpaces = true);

    /**
     * @brief Appends an explicit break (`<w:br>`) in a new run.
     *
     * Line breaks end the line without starting a new paragraph; page and
     * column breaks continue the following content on the next page or column.
     *
     * @param type Break kind to insert (line, page, or column).
     * @return The Run wrapper containing the break, or nullptr if the
     * paragraph is invalid. The run can be used to append further content
     * after the break.
     *
     * @code
     * auto paragraph = editor->AddParagraph("First line");
     * paragraph->AddBreak();
     * paragraph->AddText("Second line, same paragraph");
     * @endcode
     */
    std::shared_ptr<Run> AddBreak(BreakType type = BreakType::Line);

    /**
     * @brief Enumerates direct run children in this paragraph.
     *
     * The returned wrappers reference existing `<w:r>` elements and can be used
     * to read or update text and run formatting. The vector is a snapshot.
     *
     * @return Run wrappers in paragraph order.
     *
     * @code
     * for (const auto& run : paragraph->Runs())
     * {
     *     run->SetBold(true);
     * }
     * @endcode
     */
    std::vector<std::shared_ptr<Run>> Runs() const;

    /**
     * @brief Enumerates all text nodes contained by this paragraph.
     *
     * Text nodes are returned in document order, including text nested inside
     * fields or other run-level content. Updating a returned Text wrapper edits
     * the original paragraph.
     *
     * @return Text wrappers in document order.
     */
    std::vector<std::shared_ptr<Text>> Texts() const;

    /**
     * @brief Enumerates all drawing elements contained by this paragraph.
     *
     * Each returned Image wrapper references an existing `<w:drawing>` element.
     * The wrapper can inspect or update layout for embedded pictures when the
     * drawing uses the image shape expected by the high-level image API.
     *
     * @return Image wrappers in document order.
     */
    std::vector<std::shared_ptr<Image>> Images() const;

    /**
     * @brief Concatenates all paragraph text nodes into plain text.
     *
     * This is a convenience read helper. It does not insert separators for tabs,
     * line breaks, fields, or drawings; callers that need exact Word rendering
     * semantics should inspect the low-level DOM.
     *
     * @return Concatenated text from all `<w:t>` descendants.
     */
    std::string PlainText() const;

    /**
     * @brief Sets the paragraph style ID (`<w:pStyle>`).
     *
     * @param styleId Style identifier (e.g. "Heading1").
     * @return Reference to this paragraph for fluent chaining.
     */
    Paragraph& SetStyleId(std::string_view styleId);

    /**
     * @brief Reads the paragraph style ID, if present.
     *
     * @return Style identifier or an empty string when no direct style is set.
     */
    std::string GetStyleId() const;

    /**
     * @brief Removes the direct paragraph style override.
     *
     * The paragraph may still inherit formatting from the document's default
     * paragraph style after this call.
     *
     * @return Reference to this paragraph for fluent chaining.
     */
    Paragraph& ClearStyleId();

    /**
     * @brief Sets paragraph alignment.
     *
     * @param alignment Alignment value (left, center, right, justified, ...).
     * @return Reference to this paragraph for fluent chaining.
     */
    Paragraph& SetAlignment(DocumentFormat::OpenXml::Wordprocessing::JustificationValues alignment);

    /**
     * @brief Reads direct paragraph alignment.
     *
     * @return Alignment value, or std::nullopt when no direct alignment exists.
     */
    std::optional<DocumentFormat::OpenXml::Wordprocessing::JustificationValues> GetAlignment() const;

    /**
     * @brief Removes direct paragraph alignment.
     *
     * @return Reference to this paragraph for fluent chaining.
     */
    Paragraph& ClearAlignment();

    /**
     * @brief Sets paragraph spacing using a measurement unit (internally stored in twips).
     *
     * @param before Space before the paragraph.
     * @param after Space after the paragraph.
     * @param line Line spacing (e.g. 12pt). When provided, lineRule is also set.
     * @param lineRule Line spacing rule (Auto, AtLeast, Exact).
     * @return Reference to this paragraph for fluent chaining.
     */
    Paragraph& SetSpacing(std::optional<ExyokiOffice::MeasuringUnits> before,
                          std::optional<ExyokiOffice::MeasuringUnits> after,
                          std::optional<ExyokiOffice::MeasuringUnits> line = std::nullopt,
                          DocumentFormat::OpenXml::Wordprocessing::LineSpacingRuleValues lineRule =
                              DocumentFormat::OpenXml::Wordprocessing::LineSpacingRuleValues::Auto);

    /**
     * @brief Sets paragraph spacing in line units.
     *
     * This controls spacing using values relative to the current line height.
     * Word interprets these as line-based units (not physical length), so the
     * spacing scales with the font size and line spacing settings. This is the
     * same concept as Word's "Line spacing: Multiple" option.
     *
     * @param beforeLines Space before in line units.
     * @param afterLines Space after in line units.
     * @param lineLines Line spacing in line units.
     * @param lineRule Line spacing rule (Auto, AtLeast, Exact).
     * @return Reference to this paragraph for fluent chaining.
     */
    Paragraph& SetSpacingLines(std::optional<int> beforeLines,
                               std::optional<int> afterLines,
                               std::optional<int> lineLines = std::nullopt,
                               DocumentFormat::OpenXml::Wordprocessing::LineSpacingRuleValues lineRule =
                                   DocumentFormat::OpenXml::Wordprocessing::LineSpacingRuleValues::Auto);

    /**
     * @brief Sets paragraph indentation using a measurement unit (internally stored in twips).
     *
     * @param left Left indent.
     * @param right Right indent.
     * @param firstLine First line indent.
     * @param hanging Hanging indent.
     * @return Reference to this paragraph for fluent chaining.
     */
    Paragraph& SetIndentation(std::optional<ExyokiOffice::MeasuringUnits> left,
                              std::optional<ExyokiOffice::MeasuringUnits> right,
                              std::optional<ExyokiOffice::MeasuringUnits> firstLine = std::nullopt,
                              std::optional<ExyokiOffice::MeasuringUnits> hanging = std::nullopt);

    /**
     * @brief Reads paragraph spacing in physical units.
     *
     * @param output Receives spacing values when defined.
     * @return True if spacing properties exist.
     */
    [[nodiscard]] bool TryGetSpacing(ParagraphSpacing& output) const;

    /**
     * @brief Reads paragraph spacing in line units.
     *
     * @param output Receives line-based spacing values when defined.
     * @return True if spacing properties exist.
     */
    [[nodiscard]] bool TryGetSpacingLines(ParagraphSpacingLines& output) const;

    /**
     * @brief Reads paragraph indentation in physical units.
     *
     * @param output Receives indentation values when defined.
     * @return True if indentation properties exist.
     */
    [[nodiscard]] bool TryGetIndentation(ParagraphIndentation& output) const;

    /**
     * @brief Removes all direct spacing values from the paragraph.
     *
     * @return Reference to this paragraph for fluent chaining.
     */
    Paragraph& ClearSpacing();

    /**
     * @brief Removes all direct indentation values from the paragraph.
     *
     * @return Reference to this paragraph for fluent chaining.
     */
    Paragraph& ClearIndentation();

    /**
     * @brief Applies numbering to the paragraph.
     *
     * @param numberingId Numbering definition ID (from the numbering part).
     * @param level List level (0-based).
     * @return Reference to this paragraph for fluent chaining.
     */
    Paragraph& SetNumbering(int numberingId, int level);

    /**
     * @brief Applies a list style helper returned by WordDocumentEditor.
     *
     * @param style List style (numbering id + level).
     * @return Reference to this paragraph for fluent chaining.
     */
    Paragraph& SetListStyle(const ListStyle& style);

    /**
     * @brief Reads the numbering (list) properties, if present.
     *
     * @param output Receives numbering data when defined.
     * @return True if numbering properties exist.
     */
    [[nodiscard]] bool TryGetNumbering(ParagraphNumbering& output) const;

    /**
     * @brief Removes numbering from the paragraph.
     *
     * @return Reference to this paragraph for fluent chaining.
     */
    Paragraph& ClearNumbering();

    /**
     * @brief Adds a custom tab stop to the paragraph.
     *
     * @param position Tab stop position in any unit (stored in twips).
     * @param alignment Tab stop alignment (left, right, center, decimal).
     * @param leader Leader character style (dots, dashes, ...).
     * @return Reference to this paragraph for fluent chaining.
     */
    Paragraph& AddTabStop(const ExyokiOffice::MeasuringUnits& position,
                          DocumentFormat::OpenXml::Wordprocessing::TabStopValues alignment =
                              DocumentFormat::OpenXml::Wordprocessing::TabStopValues::Left,
                          DocumentFormat::OpenXml::Wordprocessing::TabStopLeaderCharValues leader =
                              DocumentFormat::OpenXml::Wordprocessing::TabStopLeaderCharValues::None);

    /**
     * @brief Removes all custom tab stops from the paragraph.
     *
     * @return Reference to this paragraph for fluent chaining.
     */
    Paragraph& ClearTabStops();

    /**
     * @brief Sets paragraph borders on all sides.
     *
     * @param style Border style.
     * @param size Border size (eighths of a point, Word's native unit).
     * @param color Border color value.
     * @param spacing Optional spacing between border and text.
     * @return Reference to this paragraph for fluent chaining.
     */
    Paragraph& SetBorders(DocumentFormat::OpenXml::Wordprocessing::BorderValues style,
                          UInt32 size,
                          const ExyokiOffice::Color& color = ExyokiOffice::Color(),
                          std::optional<ExyokiOffice::MeasuringUnits> spacing = std::nullopt);
    /**
     * @brief Sets paragraph borders on all sides.
     *
     * @param style Border style.
     * @param size Border size expressed in any supported unit (converted to eighths of a point).
     * @param color Border color value.
     * @param spacing Optional spacing between border and text.
     * @return Reference to this paragraph for fluent chaining.
     */
    Paragraph& SetBorders(DocumentFormat::OpenXml::Wordprocessing::BorderValues style,
                          const ExyokiOffice::MeasuringUnits& size,
                          const ExyokiOffice::Color& color = ExyokiOffice::Color(),
                          std::optional<ExyokiOffice::MeasuringUnits> spacing = std::nullopt);

    /**
     * @brief Removes paragraph border settings.
     *
     * @return Reference to this paragraph for fluent chaining.
     */
    Paragraph& ClearBorders();

    /**
     * @brief Sets paragraph shading (background fill).
     *
     * @param fill Background fill color.
     * @param pattern Shading pattern.
     * @param patternColor Pattern color.
     * @return Reference to this paragraph for fluent chaining.
     */
    Paragraph& SetShading(const ExyokiOffice::Color& fill,
                          DocumentFormat::OpenXml::Wordprocessing::ShadingPatternValues pattern =
                              DocumentFormat::OpenXml::Wordprocessing::ShadingPatternValues::Clear,
                          const ExyokiOffice::Color& patternColor = ExyokiOffice::Color());

    /**
     * @brief Clears paragraph shading (background fill).
     *
     * @return Reference to this paragraph for fluent chaining.
     */
    Paragraph& ClearShading();

    /**
     * @brief Reads paragraph shading (background fill), if present.
     *
     * @return Shading values, or std::nullopt when direct shading is absent.
     */
    std::optional<ParagraphShading> GetShading() const;

    /**
     * @brief Keeps the paragraph with the next one.
     *
     * When enabled, Word will try to keep this paragraph and the following
     * paragraph on the same page.
     *
     * @param enabled True to enable, false to clear.
     * @return Reference to this paragraph for fluent chaining.
     */
    Paragraph& SetKeepWithNext(bool enabled);

    /**
     * @brief Keeps all lines of the paragraph on the same page.
     *
     * @param enabled True to enable, false to clear.
     * @return Reference to this paragraph for fluent chaining.
     */
    Paragraph& SetKeepLines(bool enabled);

    /**
     * @brief Forces a page break before the paragraph.
     *
     * @param enabled True to insert a page break before this paragraph.
     * @return Reference to this paragraph for fluent chaining.
     */
    Paragraph& SetPageBreakBefore(bool enabled);

    /**
     * @brief Enables or disables widow control.
     *
     * Widow control prevents single lines of a paragraph from appearing
     * at the top or bottom of a page.
     *
     * @param enabled True to enable, false to clear.
     * @return Reference to this paragraph for fluent chaining.
     */
    Paragraph& SetWidowControl(bool enabled);

    /**
     * @brief Reads direct keep-with-next pagination setting.
     *
     * @return Direct setting value, or std::nullopt when absent.
     */
    std::optional<bool> GetKeepWithNext() const;

    /**
     * @brief Reads direct keep-lines pagination setting.
     *
     * @return Direct setting value, or std::nullopt when absent.
     */
    std::optional<bool> GetKeepLines() const;

    /**
     * @brief Reads direct page-break-before setting.
     *
     * @return Direct setting value, or std::nullopt when absent.
     */
    std::optional<bool> GetPageBreakBefore() const;

    /**
     * @brief Reads direct widow-control setting.
     *
     * @return Direct setting value, or std::nullopt when absent.
     */
    std::optional<bool> GetWidowControl() const;

    /**
     * @brief Removes all direct pagination flags exposed by this wrapper.
     *
     * Clears keep-with-next, keep-lines, page-break-before, and widow-control.
     *
     * @return Reference to this paragraph for fluent chaining.
     */
    Paragraph& ClearPagination();

    /**
     * @brief Reads all common direct paragraph formatting in one call.
     *
     * This is useful for importers, document analysis tools, and tests that
     * need a stable high-level view of direct formatting without inspecting
     * generated OpenXML element classes directly.
     *
     * @return Formatting snapshot, or std::nullopt when this wrapper is invalid.
     */
    std::optional<ParagraphFormatting> GetFormatting() const;

    /**
     * @brief Removes every direct paragraph formatting property exposed here.
     *
     * Text and runs remain intact. The paragraph can still inherit formatting
     * from styles after direct formatting is cleared.
     *
     * @return Reference to this paragraph for fluent chaining.
     */
    Paragraph& ClearFormatting();

    /**
     * @brief Appends an external hyperlink run (`<w:hyperlink r:id="...">`) to the paragraph.
     *
     * This creates an external relationship on the part that owns the paragraph (see
     * OwningPart()) and a `<w:hyperlink>` element wrapping a single run containing text.
     * A paragraph with no part cannot record the target, so nothing is added and the
     * call returns nullptr rather than a hyperlink element that points nowhere.
     *
     * @param text Visible text for the hyperlink run. May be empty to add the link with no text.
     * @param url Target URL (e.g. "https://example.com"). Must be non-empty.
     * @param tooltip Optional screen-tip text shown when hovering the link.
     * @param newWindow When true, opens the target in a new browser window/tab.
     * @return Hyperlink wrapper, or nullptr when the paragraph is invalid, url is empty,
     * or the paragraph has no part to store the relationship in.
     *
     * @code
     * paragraph->AddHyperlink("ExyokiOffice on the web", "https://example.com/docs");
     * @endcode
     */
    std::shared_ptr<Hyperlink> AddHyperlink(std::string_view text,
                                            std::string_view url,
                                            std::string_view tooltip = {},
                                            bool newWindow = false);

    /**
     * @brief Appends an internal hyperlink run that jumps to a bookmark in the same document.
     *
     * Internal hyperlinks use the `w:anchor` attribute instead of a relationship, so they
     * work even without a main document part attached.
     *
     * @param text Visible text for the hyperlink run. May be empty to add the link with no text.
     * @param bookmarkName Target bookmark name (see Paragraph::AddBookmark()). Must be non-empty.
     * @param tooltip Optional screen-tip text shown when hovering the link.
     * @return Hyperlink wrapper, or nullptr when the paragraph is invalid or bookmarkName is empty.
     */
    std::shared_ptr<Hyperlink> AddInternalHyperlink(std::string_view text,
                                                    std::string_view bookmarkName,
                                                    std::string_view tooltip = {});

    /**
     * @brief Enumerates direct `<w:hyperlink>` children of this paragraph.
     *
     * @return Hyperlink wrappers in paragraph order.
     */
    std::vector<std::shared_ptr<Hyperlink>> Hyperlinks() const;

    /**
     * @brief Inserts a named bookmark at the logical end of the paragraph's content.
     *
     * The bookmark is a zero-width point (`<w:bookmarkStart>` immediately followed by
     * `<w:bookmarkEnd>`), which is sufficient to serve as a navigation target for internal
     * hyperlinks and cross-references. The bookmark ID is allocated to be unique across
     * the whole document when a main document part is attached; otherwise uniqueness is
     * only guaranteed within this paragraph.
     *
     * @param name Bookmark name. Word restricts bookmark names to 40 characters, letters,
     * digits, and underscore, starting with a letter; this method does not validate that.
     * @return Bookmark wrapper, or nullptr when the paragraph is invalid or name is empty.
     */
    std::shared_ptr<Bookmark> AddBookmark(std::string_view name);

    /**
     * @brief Enumerates bookmarks that start within this paragraph.
     *
     * The matching `<w:bookmarkEnd>` is resolved from this paragraph first and, when a
     * main document part is attached, from the whole document afterward (a bookmark may
     * legally span multiple paragraphs). The end wrapper is nullptr when no match is found.
     *
     * @return Bookmark wrappers in paragraph order.
     */
    std::vector<std::shared_ptr<Bookmark>> Bookmarks() const;

    /**
     * @brief Appends a complex Word field to the paragraph.
     *
     * The generated XML uses the standard complex field structure:
     * `w:fldChar` begin, `w:instrText`, `w:fldChar` separate, cached result
     * text, and `w:fldChar` end. The cached result is written exactly as
     * provided; ExyokiOffice does not evaluate the instruction.
     *
     * If the instruction names a layout-dependent field such as `PAGE`,
     * `NUMPAGES`, or `SECTIONPAGES`, the created field is marked dirty so Word
     * or another layout-aware processor can update it later.
     *
     * @param instruction Field instruction text, for example `DATE \\@ "yyyy-MM-dd"`.
     * @param cachedResult Existing visible field result to store.
     * @param preserveResultSpaces Whether to preserve leading/trailing spaces in the result text.
     * @return Field wrapper, or nullptr when the paragraph is invalid or instruction is empty.
     */
    std::shared_ptr<Field> AddField(std::string_view instruction,
                                    std::string_view cachedResult = {},
                                    bool preserveResultSpaces = false);

    /**
     * @brief Appends a simple Word field (`w:fldSimple`) to the paragraph.
     *
     * Simple fields store their instruction on the field element and the
     * cached result as normal run text inside that element. This representation
     * is compact and is commonly used for fields that do not need nested field
     * content. The library stores and edits the cached result, but it does not
     * compute new field values.
     *
     * @param instruction Field instruction text.
     * @param cachedResult Existing visible field result to store.
     * @param preserveResultSpaces Whether to preserve leading/trailing spaces in the result text.
     * @return Field wrapper, or nullptr when the paragraph is invalid or instruction is empty.
     */
    std::shared_ptr<Field> AddSimpleField(std::string_view instruction,
                                          std::string_view cachedResult = {},
                                          bool preserveResultSpaces = false);

    /**
     * @brief Enumerates simple and complex fields contained by this paragraph.
     *
     * Complex fields are detected from direct run-level `w:fldChar` markers in
     * the paragraph. Nested fields are returned as separate wrappers when their
     * begin/end marker pair is complete. Malformed fields without an end marker
     * are ignored.
     *
     * @return Field wrappers in paragraph order.
     */
    std::vector<std::shared_ptr<Field>> Fields() const;

    /**
     * @brief Appends a footnote reference to this paragraph and creates its content.
     *
     * This ensures the document's footnotes part exists (creating the standard
     * Separator and ContinuationSeparator bookkeeping entries the first time),
     * allocates a document-unique note ID, appends a `<w:footnoteReference>`
     * run at the end of this paragraph, and creates the corresponding footnote
     * entry containing the reference mark followed by the given text.
     *
     * Requires a main document part attached through the constructor or
     * AttachMainDocumentPart(); returns nullptr otherwise.
     *
     * @param text Footnote text.
     * @param preserveSpaces Writes `xml:space="preserve"`; on by default, see Run::AddText.
     * @return Note wrapper, or nullptr when the paragraph is invalid or has no
     * main document part attached.
     *
     * @code
     * auto note = paragraph->AddFootnote("Explanatory text.");
     * @endcode
     */
    std::shared_ptr<Note> AddFootnote(std::string_view text = {}, bool preserveSpaces = true);

    /**
     * @brief Appends an endnote reference to this paragraph and creates its content.
     *
     * Behaves like AddFootnote() but uses the document's endnotes part
     * (`/word/endnotes.xml`) and `<w:endnoteReference>` markers instead.
     *
     * @param text Endnote text.
     * @param preserveSpaces Writes `xml:space="preserve"`; on by default, see Run::AddText.
     * @return Note wrapper, or nullptr when the paragraph is invalid or has no
     * main document part attached.
     */
    std::shared_ptr<Note> AddEndnote(std::string_view text = {}, bool preserveSpaces = true);

    /**
     * @brief Wraps a contiguous span of this paragraph's direct runs with a comment.
     *
     * Inserts a `<w:commentRangeStart>` before the first run, a
     * `<w:commentRangeEnd>` after the last run, and a run carrying the
     * `<w:commentReference>` right after that. Creates (or reuses) the
     * document's comments part and allocates a document-unique comment ID.
     *
     * Requires a main document part attached through the constructor or
     * AttachMainDocumentPart(); returns nullptr otherwise. All entries in
     * `runs` must be direct run children of this paragraph.
     *
     * @param runs Non-empty, in-order span of this paragraph's direct runs to comment on.
     * @param text Comment text.
     * @param author Optional author metadata for the comment.
     * @return Comment wrapper, or nullptr when the paragraph is invalid, has no
     * main document part attached, or `runs` is empty.
     */
    std::shared_ptr<Comment> AddComment(const std::vector<std::shared_ptr<Run>>& runs,
                                        std::string_view text,
                                        const CommentAuthor& author = {});

    /**
     * @brief Convenience overload that comments on this paragraph's entire direct run content.
     *
     * Equivalent to `AddComment(Runs(), text, author)`.
     *
     * @return Comment wrapper, or nullptr when the paragraph is invalid, has no
     * main document part attached, or has no direct runs.
     */
    std::shared_ptr<Comment> AddCommentOnParagraph(std::string_view text, const CommentAuthor& author = {});

    /**
     * @brief Enumerates comments whose reference lies directly in this paragraph.
     *
     * This only finds comments whose range start, range end, and reference are
     * all located within this paragraph (the shape produced by AddComment()).
     * Comments spanning multiple paragraphs are not returned here; use
     * WordDocumentEditor::Comments() for a complete listing.
     *
     * @return Comment wrappers in paragraph order.
     */
    std::vector<std::shared_ptr<Comment>> Comments() const;

    /**
     * @brief Appends an inline content control (structured document tag) to this paragraph.
     *
     * The new content control starts with an assigned, document-unique ID and
     * empty content; use ContentControl::AddRun()/AddText()/SetText() to fill it in.
     *
     * @param tag Optional programmatic tag (`w:tag`).
     * @param alias Optional human-readable alias (`w:alias`) shown in Word's UI.
     * @return Content control wrapper, or nullptr when the paragraph is invalid.
     */
    std::shared_ptr<ContentControl> AddInlineContentControl(std::string_view tag = {},
                                                            std::string_view alias = {});

    /**
     * @brief Enumerates inline content controls that are direct children of this paragraph.
     *
     * @return Content control wrappers in paragraph order.
     */
    std::vector<std::shared_ptr<ContentControl>> ContentControls() const;

    /**
     * @brief Finds the first occurrence of a substring in the paragraph's direct run text.
     *
     * @param needle Text to search for. An empty needle never matches.
     * @param searchFrom Character offset to start searching from.
     * @return Matching range, or std::nullopt when not found.
     *
     * @code
     * if (auto range = paragraph->Find("TODO"))
     * {
     *     paragraph->ReplaceText(*range, "Done");
     * }
     * @endcode
     */
    std::optional<ContentRange> Find(std::string_view needle, Size searchFrom = 0) const;

    /**
     * @brief Finds every non-overlapping occurrence of a substring.
     *
     * @param needle Text to search for. An empty needle returns no matches.
     * @return Matching ranges in left-to-right order.
     */
    std::vector<ContentRange> FindAll(std::string_view needle) const;

    /**
     * @brief Reads the text covered by a range.
     *
     * @param range Range to read, typically obtained from Find()/FindAll().
     * @return Text within the range, or an empty string when the range is out of bounds.
     */
    std::string GetText(const ContentRange& range) const;

    /**
     * @brief Replaces the text covered by a range, preserving formatting outside the range.
     *
     * Runs that are only partially covered by the range keep their original run
     * formatting (`w:rPr`) for the untouched prefix/suffix text. Runs fully covered by
     * the range other than the first and last overlapped run are removed outright. The
     * replacement text is written into the run where the range starts, so it inherits
     * that run's formatting; unaffected text before and after the range is never rewritten.
     *
     * This operates purely on this paragraph's direct run sequence (see ContentRange); it
     * does not reach into nested elements such as hyperlinks.
     *
     * @param range Range to replace, typically obtained from Find()/FindAll().
     * @param replacement Replacement text. May be empty to delete the range.
     * @return True when the replacement was applied.
     *
     * @code
     * auto paragraph = editor->AddParagraph();
     * paragraph->AddRun("Hello ")->SetBold(true);
     * paragraph->AddRun("cruel world")->SetItalic(true);
     * // "cruel " straddles the two runs above; only the middle text changes.
     * if (auto range = paragraph->Find("cruel "))
     * {
     *     paragraph->ReplaceText(*range, "");
     * }
     * @endcode
     */
    bool ReplaceText(const ContentRange& range, std::string_view replacement);

    /**
     * @brief Replaces every non-overlapping occurrence of a substring.
     *
     * Equivalent to repeatedly calling Find() and ReplaceText(), but adjusts subsequent
     * search offsets for the replacement length automatically.
     *
     * @param needle Text to search for. An empty needle performs no replacements.
     * @param replacement Replacement text for each match.
     * @return Number of replacements performed.
     */
    Size ReplaceAll(std::string_view needle, std::string_view replacement);

    /**
     * @brief Finds every non-overlapping regex match in the paragraph's direct run text.
     *
     * Uses the same text/offset model as FindAll() (see ContentRange): matches are
     * located in the concatenation of this paragraph's direct run text and never reach
     * into nested elements such as hyperlinks.
     *
     * @param pattern ECMAScript regular expression and its options; see RegexPattern
     * for the syntax, the byte-wise character classes, and the length past which a
     * paragraph is not searched at all.
     * @return Matching ranges in left-to-right order. Empty when the expression does
     * not compile - ask RegexPattern::IsValid() to tell that apart from no matches.
     */
    std::vector<ContentRange> FindAllRegex(const RegexPattern& pattern) const;

    /**
     * @brief Replaces every non-overlapping regex match, substituting capture groups.
     *
     * For each match, @p replacementFormat is expanded using ECMAScript substitution
     * syntax ($1, $2, ..., $&, $`, $', $$), and the resulting text is applied through
     * ReplaceText(), so formatting is preserved exactly as it is for ReplaceAll().
     *
     * @param pattern ECMAScript regular expression and its options.
     * @param replacementFormat Replacement template, may reference capture groups.
     * @return Number of replacements performed; zero when the expression does not compile.
     */
    Size ReplaceAllRegex(const RegexPattern& pattern, std::string_view replacementFormat);

private:
    std::shared_ptr<DocumentFormat::OpenXml::Wordprocessing::Paragraph> m_paragraph;
    std::shared_ptr<Packaging::MainDocumentPart> m_mainDocumentPart;
    std::shared_ptr<OpenXmlPackagePart> m_owningPart;
};

/**
 * @brief Captures language tags assigned to a run.
 */
struct RunLanguage
{
    std::string Latin;
    std::string EastAsia;
    std::string Bidi;
};

/**
 * @brief Captures font names assigned to a run.
 */
struct RunFonts
{
    std::string Ascii;
    std::string HighAnsi;
    std::string EastAsia;
    std::string ComplexScript;
};

/**
 * @brief Captures common direct run formatting.
 *
 * Run formatting is character-level formatting stored in `<w:rPr>`. It affects
 * only the text and inline content inside a single run, so a paragraph can mix
 * many different run formatting snapshots. This struct mirrors the high-level
 * Run setter API and is designed for document readers, converters, style
 * normalization passes, and unit tests that need to inspect direct formatting
 * without depending on generated DOM details.
 */
struct RunFormatting
{
    std::string StyleId;
    std::optional<bool> Bold;
    std::optional<bool> Italic;
    std::optional<DocumentFormat::OpenXml::Wordprocessing::UnderlineValues> Underline;
    std::optional<bool> Strike;
    std::optional<bool> DoubleStrike;
    std::optional<bool> Caps;
    std::optional<bool> SmallCaps;
    std::optional<bool> NoProof;
    std::string Color;
    std::optional<DocumentFormat::OpenXml::Wordprocessing::HighlightColorValues> Highlight;
    std::optional<RunFonts> Fonts;
    std::optional<RunLanguage> Language;
    std::optional<ExyokiOffice::MeasuringUnits> Kerning;
    std::optional<ExyokiOffice::MeasuringUnits> Position;
    std::optional<ExyokiOffice::MeasuringUnits> Spacing;
    std::optional<ExyokiOffice::MeasuringUnits> FontSize;
    std::optional<DocumentFormat::OpenXml::Wordprocessing::TextEffectValues> TextEffect;
};

/**
 * @brief High-level run wrapper with fluent formatting and inspection helpers.
 *
 * A run is the smallest formatting unit in a Word paragraph. It owns one or
 * more text nodes and carries character-level formatting (bold, font, size,
 * underline, color, language, proofing flags, spacing, and effects). Direct
 * run formatting lives in `<w:rPr>`; inherited formatting still comes from
 * paragraph styles, character styles, and document defaults.
 *
 * The wrapper exposes three complementary workflows:
 *  - Set direct formatting for newly generated content.
 *  - Read direct formatting from opened documents through GetFormatting().
 *  - Clear direct formatting to let styles drive appearance again.
 *
 * Typical usage:
 * @code
 * using namespace ExyokiOffice::Word;
 * auto paragraph = editor->AddParagraph();
 * auto run = paragraph->AddRun();
 * run->AddText("Warning:")->SetPreserveSpaces(true);
 * run->SetBold()
 *    .SetColor(ExyokiOffice::Color(ExyokiOffice::ColorPreset::Red))
 *    .SetNoProof(true);
 *
 * RunFormatting formatting;
 * if (auto formatting = run->GetFormatting(); formatting && formatting->Bold == true)
 * {
 *     run->ClearBold().SetStyleId("StrongEmphasis");
 * }
 *
 * run->ClearFormatting(); // Leaves run text intact.
 * @endcode
 */
class EXYOKIOFFICE_EXPORT Run
{
public:
    using Ptr = std::shared_ptr<Run>;

    /**
     * @brief Wraps an existing low-level run element.
     *
     * @param run Low-level `<w:r>` element to wrap.
     */
    explicit Run(const std::shared_ptr<DocumentFormat::OpenXml::Wordprocessing::Run>& run);

    /**
     * @brief Returns the underlying low-level run element.
     *
     * Use this when you need access to properties or child elements not exposed
     * by the high-level wrapper.
     * @return The wrapped element, or nullptr when this wrapper is detached.
     */
    std::shared_ptr<DocumentFormat::OpenXml::Wordprocessing::Run> GetLowLevelApi() const;

    /**
     * @brief Appends text to the run.
     *
     * The `<w:t>` element carries `xml:space="preserve"` unless you ask for the
     * opposite, and every text-taking overload in this API defaults the same
     * way. Without the attribute an XML consumer is entitled to collapse
     * leading and trailing whitespace, so `AddParagraph("Hello ")` followed by
     * `AddText("world")` produced "Helloworld" — text the caller wrote,
     * silently lost, and only for the runs that happened to end in a space.
     * Writing the attribute always costs a few bytes per run and is what Word
     * itself does; that is the trade this default takes.
     *
     * @param text Text to append as a `<w:t>` element.
     * @param preserveSpaces Writes `xml:space="preserve"`; on by default. Pass
     *        false only when the document deliberately wants whitespace
     *        collapsed by whoever reads it.
     * @return High-level Text wrapper, or nullptr if the run is invalid.
     *
     * @code
     * auto run = paragraph->AddRun();
     * auto text = run->AddText("  padded  ");
     * @endcode
     */
    std::shared_ptr<Text> AddText(std::string_view text, bool preserveSpaces = true);

    /**
     * @brief Appends an explicit break (`<w:br>`) to the run.
     *
     * The break is inserted after the run's current content, so text appended
     * afterwards continues on the next line, page, or column.
     *
     * @param type Break kind to insert (line, page, or column).
     * @return Reference to this run for fluent chaining.
     */
    Run& AddBreak(BreakType type = BreakType::Line);

    /**
     * @brief Enumerates direct text children in this run.
     *
     * @return Text wrappers in run order.
     */
    std::vector<std::shared_ptr<Text>> Texts() const;

    /**
     * @brief Enumerates drawing elements contained by this run.
     *
     * @return Image wrappers for `<w:drawing>` descendants in document order.
     */
    std::vector<std::shared_ptr<Image>> Images() const;

    /**
     * @brief Concatenates direct text children from this run.
     *
     * @return Plain text stored directly in this run.
     */
    std::string PlainText() const;

    /**
     * @brief Enables or disables bold formatting.
     *
     * @param enabled True to enable bold; false to clear.
     * @return Reference to this run for fluent chaining.
     */
    Run& SetBold(bool enabled = true);
    /**
     * @brief Reads direct bold formatting.
     *
     * @return Direct bold value, or std::nullopt when bold is inherited.
     */
    std::optional<bool> GetBold() const;
    /**
     * @brief Removes direct bold formatting.
     *
     * @return Reference to this run for fluent chaining.
     */
    Run& ClearBold();
    /**
     * @brief Enables or disables italic formatting.
     *
     * @param enabled True to enable italic; false to clear.
     * @return Reference to this run for fluent chaining.
     */
    Run& SetItalic(bool enabled = true);
    /**
     * @brief Reads direct italic formatting.
     *
     * @return Direct italic value, or std::nullopt when italic is inherited.
     */
    std::optional<bool> GetItalic() const;
    /**
     * @brief Removes direct italic formatting.
     *
     * @return Reference to this run for fluent chaining.
     */
    Run& ClearItalic();
    /**
     * @brief Sets underline style explicitly.
     *
     * @param style Underline style (single, double, none, ...).
     * @return Reference to this run for fluent chaining.
     */
    Run& SetUnderline(DocumentFormat::OpenXml::Wordprocessing::UnderlineValues style);
    /**
     * @brief Reads direct underline style.
     *
     * @return Underline style, or std::nullopt when underline is inherited.
     */
    std::optional<DocumentFormat::OpenXml::Wordprocessing::UnderlineValues> GetUnderline() const;
    /**
     * @brief Removes direct underline formatting.
     *
     * @return Reference to this run for fluent chaining.
     */
    Run& ClearUnderline();
    /**
     * @brief Convenience overload for simple underline on/off.
     *
     * When enabled, sets the underline style to `Single`; when disabled,
     * sets it to `None`.
     *
     * @param enabled True to enable underline; false to clear.
     * @return Reference to this run for fluent chaining.
     */
    Run& SetUnderline(bool enabled);
    /**
     * @brief Enables or disables single strike-through formatting.
     *
     * @param enabled True to enable strike-through; false to clear.
     * @return Reference to this run for fluent chaining.
     */
    Run& SetStrike(bool enabled = true);
    /**
     * @brief Reads direct single strike-through formatting.
     *
     * @return Direct strike value, or std::nullopt when inherited.
     */
    std::optional<bool> GetStrike() const;
    /**
     * @brief Removes direct single strike-through formatting.
     *
     * @return Reference to this run for fluent chaining.
     */
    Run& ClearStrike();
    /**
     * @brief Enables or disables double strike-through formatting.
     *
     * @param enabled True to enable double strike-through; false to clear.
     * @return Reference to this run for fluent chaining.
     */
    Run& SetDoubleStrike(bool enabled = true);
    /**
     * @brief Reads direct double strike-through formatting.
     *
     * @return Direct double-strike value, or std::nullopt when inherited.
     */
    std::optional<bool> GetDoubleStrike() const;
    /**
     * @brief Removes direct double strike-through formatting.
     *
     * @return Reference to this run for fluent chaining.
     */
    Run& ClearDoubleStrike();
    /**
     * @brief Enables or disables all-caps display for the run.
     *
     * @param enabled True to enable caps; false to clear.
     * @return Reference to this run for fluent chaining.
     */
    Run& SetCaps(bool enabled = true);
    /**
     * @brief Reads direct all-caps formatting.
     *
     * @return Direct caps value, or std::nullopt when inherited.
     */
    std::optional<bool> GetCaps() const;
    /**
     * @brief Removes direct all-caps formatting.
     *
     * @return Reference to this run for fluent chaining.
     */
    Run& ClearCaps();
    /**
     * @brief Enables or disables small-caps display for the run.
     *
     * @param enabled True to enable small caps; false to clear.
     * @return Reference to this run for fluent chaining.
     */
    Run& SetSmallCaps(bool enabled = true);
    /**
     * @brief Reads direct small-caps formatting.
     *
     * @return Direct small-caps value, or std::nullopt when inherited.
     */
    std::optional<bool> GetSmallCaps() const;
    /**
     * @brief Removes direct small-caps formatting.
     *
     * @return Reference to this run for fluent chaining.
     */
    Run& ClearSmallCaps();
    /**
     * @brief Enables or disables proofing for the run.
     *
     * When enabled, Word skips spellcheck and grammar checking for this run
     * (`<w:noProof>`), which is useful for codes, identifiers, or foreign text.
     *
     * @param enabled True to disable proofing; false to allow proofing.
     * @return Reference to this run for fluent chaining.
     */
    Run& SetNoProof(bool enabled = true);
    /**
     * @brief Reads direct no-proofing setting.
     *
     * @return Direct no-proof value, or std::nullopt when inherited.
     */
    std::optional<bool> GetNoProof() const;
    /**
     * @brief Removes direct no-proofing setting.
     *
     * @return Reference to this run for fluent chaining.
     */
    Run& ClearNoProof();
    /**
     * @brief Sets the run color using a color value.
     *
     * @param color Color value (auto or RGB).
     * @return Reference to this run for fluent chaining.
     */
    Run& SetColor(const ExyokiOffice::Color& color);
    /**
     * @brief Reads direct run text color.
     *
     * @return OpenXML color text such as "auto" or "FF0000"; empty when absent.
     */
    std::string GetColor() const;
    /**
     * @brief Removes direct run text color.
     *
     * @return Reference to this run for fluent chaining.
     */
    Run& ClearColor();
    /**
     * @brief Sets the highlight color for the run.
     *
     * @param color Highlight color enumeration value.
     * @return Reference to this run for fluent chaining.
     */
    Run& SetHighlight(DocumentFormat::OpenXml::Wordprocessing::HighlightColorValues color);
    /**
     * @brief Reads direct highlight color.
     *
     * @return Highlight value, or std::nullopt when no direct highlight exists.
     */
    std::optional<DocumentFormat::OpenXml::Wordprocessing::HighlightColorValues> GetHighlight() const;
    /**
     * @brief Removes direct highlight color.
     *
     * @return Reference to this run for fluent chaining.
     */
    Run& ClearHighlight();
    /**
     * @brief Sets the font for ASCII and optionally High ANSI text.
     *
     * @param asciiFont Font name for ASCII text (e.g. "Calibri").
     * @param highAnsiFont Optional font name for High ANSI text. If empty,
     *        the ASCII font remains the only override.
     * @return Reference to this run for fluent chaining.
     */
    Run& SetFont(std::string_view asciiFont, std::string_view highAnsiFont = {});
    /**
     * @brief Reads direct run font names.
     *
     * @return Font names, or std::nullopt when no direct font override exists.
     */
    std::optional<RunFonts> GetFont() const;
    /**
     * @brief Removes direct run font overrides.
     *
     * @return Reference to this run for fluent chaining.
     */
    Run& ClearFont();
    /**
     * @brief Sets the run language (proofing and typography).
     *
     * This corresponds to `<w:lang>` and controls the language used for
     * spellcheck/grammar and typographic features.
     *
     * @param latin Language tag for Latin text (e.g. "en-US", "cs-CZ").
     * @param eastAsia Language tag for East Asian text (optional).
     * @param bidi Language tag for complex script (optional).
     * @return Reference to this run for fluent chaining.
     *
     * @code
     * run->SetLanguage("cs-CZ");
     * run->SetLanguage("en-US", "ja-JP");
     * @endcode
     */
    Run& SetLanguage(std::string_view latin,
                     std::string_view eastAsia = {},
                     std::string_view bidi = {});
    /**
     * @brief Reads direct run language tags.
     *
     * @return Language tags, or std::nullopt when no direct language is set.
     */
    std::optional<RunLanguage> GetLanguage() const;
    /**
     * @brief Removes direct language tags.
     *
     * @return Reference to this run for fluent chaining.
     */
    Run& ClearLanguage();
    /**
     * @brief Sets the minimum font size for kerning to apply.
     *
     * Word enables kerning pairs only when the run's font size is at or above
     * this threshold. The value is stored in half-points.
     *
     * @param minimumSize Minimum font size for kerning (e.g. 10pt).
     * @return Reference to this run for fluent chaining.
     *
     * @code
     * run->SetKerning(ExyokiOffice::MeasuringUnits(10.0, MeasurementUnit::Point));
     * @endcode
     */
    Run& SetKerning(const ExyokiOffice::MeasuringUnits& minimumSize);
    /**
     * @brief Reads the direct kerning threshold.
     *
     * @return Threshold size, or std::nullopt when absent.
     */
    std::optional<ExyokiOffice::MeasuringUnits> GetKerning() const;
    /**
     * @brief Removes the direct kerning threshold.
     *
     * @return Reference to this run for fluent chaining.
     */
    Run& ClearKerning();
    /**
     * @brief Offsets the run vertically relative to the baseline.
     *
     * Positive values raise text; negative values lower it. The value is stored
     * in signed half-points (1/2 pt).
     *
     * @param offset Vertical offset in any supported unit (e.g. 2pt, -1pt).
     * @return Reference to this run for fluent chaining.
     */
    Run& SetPosition(const ExyokiOffice::MeasuringUnits& offset);
    /**
     * @brief Reads direct baseline offset.
     *
     * @return Baseline offset, or std::nullopt when absent.
     */
    std::optional<ExyokiOffice::MeasuringUnits> GetPosition() const;
    /**
     * @brief Removes direct baseline offset.
     *
     * @return Reference to this run for fluent chaining.
     */
    Run& ClearPosition();
    /**
     * @brief Adjusts character spacing for the run.
     *
     * This sets the run's extra spacing in twips (1/20 pt). Positive values
     * add space; negative values tighten spacing.
     *
     * @param spacing Extra character spacing in any supported unit.
     * @return Reference to this run for fluent chaining.
     */
    Run& SetSpacing(const ExyokiOffice::MeasuringUnits& spacing);
    /**
     * @brief Reads direct character spacing.
     *
     * @return Character spacing, or std::nullopt when absent.
     */
    std::optional<ExyokiOffice::MeasuringUnits> GetSpacing() const;
    /**
     * @brief Removes direct character spacing.
     *
     * @return Reference to this run for fluent chaining.
     */
    Run& ClearSpacing();
    /**
     * @brief Sets the font size expressed in any supported unit.
     *
     * The value is converted to points and stored in Word's half-point units.
     *
     * @param size Font size (e.g. MeasuringUnits(11, MeasurementUnit::Point)).
     * @return Reference to this run for fluent chaining.
     *
     * @code
     * run->SetFontSize(ExyokiOffice::MeasuringUnits(12.0, MeasurementUnit::Point));
     * @endcode
     */
    Run& SetFontSize(const ExyokiOffice::MeasuringUnits& size);
    /**
     * @brief Reads direct font size.
     *
     * @return Font size, or std::nullopt when inherited.
     */
    std::optional<ExyokiOffice::MeasuringUnits> GetFontSize() const;
    /**
     * @brief Removes direct font size.
     *
     * @return Reference to this run for fluent chaining.
     */
    Run& ClearFontSize();
    /**
     * @brief Sets a text effect for the run.
     *
     * @param effect Text effect enum value (e.g. outline, shadow, emboss).
     * @return Reference to this run for fluent chaining.
     */
    Run& SetTextEffect(DocumentFormat::OpenXml::Wordprocessing::TextEffectValues effect);
    /**
     * @brief Reads direct animated text effect.
     *
     * @return Text effect, or std::nullopt when absent.
     */
    std::optional<DocumentFormat::OpenXml::Wordprocessing::TextEffectValues> GetTextEffect() const;
    /**
     * @brief Removes direct animated text effect.
     *
     * @return Reference to this run for fluent chaining.
     */
    Run& ClearTextEffect();
    /**
     * @brief Applies a character style to the run.
     *
     * @param styleId Style identifier (e.g. "Emphasis").
     * @return Reference to this run for fluent chaining.
     */
    Run& SetStyleId(std::string_view styleId);
    /**
     * @brief Reads direct character style ID.
     *
     * @return Style identifier, or an empty string when absent.
     */
    std::string GetStyleId() const;
    /**
     * @brief Removes direct character style ID.
     *
     * @return Reference to this run for fluent chaining.
     */
    Run& ClearStyleId();

    /**
     * @brief Reads all common direct run formatting in one call.
     *
     * @return Formatting snapshot, or std::nullopt when this wrapper is invalid.
     */
    std::optional<RunFormatting> GetFormatting() const;

    /**
     * @brief Removes every direct run formatting property exposed by this wrapper.
     *
     * Text, drawings, fields, and other run content remain in place.
     *
     * @return Reference to this run for fluent chaining.
     */
    Run& ClearFormatting();

private:
    std::shared_ptr<DocumentFormat::OpenXml::Wordprocessing::Run> m_run;
};

/**
 * @brief High-level text wrapper with helpers for content and whitespace.
 *
 * A Text wraps a single `<w:t>` element inside a run. It exposes getters/setters
 * for text content and whitespace handling.
 *
 * @code
 * auto run = paragraph->AddRun();
 * auto text = run->AddText("  Keep padding  ", true);
 * text->SetText("Now updated");
 * text->SetPreserveSpaces(false);
 * @endcode
 */
class EXYOKIOFFICE_EXPORT Text
{
public:
    using Ptr = std::shared_ptr<Text>;

    /**
     * @brief Wraps an existing low-level text element.
     *
     * @param text Low-level `<w:t>` element to wrap.
     */
    explicit Text(const std::shared_ptr<DocumentFormat::OpenXml::Wordprocessing::Text>& text);

    /**
     * @brief Returns the underlying low-level text element.
     *
     * Use this when you need access to attributes not exposed by the wrapper.
     * @return The wrapped element, or nullptr when this wrapper is detached.
     */
    std::shared_ptr<DocumentFormat::OpenXml::Wordprocessing::Text> GetLowLevelApi() const;

    /**
     * @brief Returns the text content.
     *
     * @return Current text of the `<w:t>` element.
     */
    std::string GetText() const;

    /**
     * @brief Updates the text content.
     *
     * @param text New text content to set.
     * @return Reference to this text wrapper for fluent chaining.
     */
    Text& SetText(std::string_view text);

    /**
     * @brief Controls `xml:space="preserve"` for whitespace.
     *
     * When enabled, Word preserves leading/trailing spaces and consecutive
     * spaces in the text node.
     *
     * @param preserve True to preserve whitespace; false to allow normal
     *        whitespace collapsing.
     * @return Reference to this text wrapper for fluent chaining.
     */
    Text& SetPreserveSpaces(bool preserve);

private:
    std::shared_ptr<DocumentFormat::OpenXml::Wordprocessing::Text> m_text;
};

/**
 * @brief Captures image size information.
 */
struct ImageSize
{
    ExyokiOffice::MeasuringUnits Width;
    ExyokiOffice::MeasuringUnits Height;
};

/**
 * @brief Captures the distance between an image and surrounding text.
 */
struct ImageDistanceFromText
{
    ExyokiOffice::MeasuringUnits Left;
    ExyokiOffice::MeasuringUnits Top;
    ExyokiOffice::MeasuringUnits Right;
    ExyokiOffice::MeasuringUnits Bottom;
};

/**
 * @brief Captures image wrap settings for floating images.
 */
struct ImageWrapSettings
{
    ImageWrap Wrap = ImageWrap::Square;
    DocumentFormat::OpenXml::Drawing::Wordprocessing::WrapTextValues WrapText =
        DocumentFormat::OpenXml::Drawing::Wordprocessing::WrapTextValues::BothSides;
};

/**
 * @brief Captures image anchor position settings.
 *
 * Offsets are expressed relative to the chosen anchor reference.
 */
struct ImagePosition
{
    DocumentFormat::OpenXml::Drawing::Wordprocessing::HorizontalRelativePositionValues HorizontalFrom =
        DocumentFormat::OpenXml::Drawing::Wordprocessing::HorizontalRelativePositionValues::NotDefinedEnumValue;
    std::optional<ExyokiOffice::MeasuringUnits> HorizontalOffset;
    std::optional<DocumentFormat::OpenXml::Drawing::Wordprocessing::HorizontalAlignmentValues> HorizontalAlignment;
    DocumentFormat::OpenXml::Drawing::Wordprocessing::VerticalRelativePositionValues VerticalFrom =
        DocumentFormat::OpenXml::Drawing::Wordprocessing::VerticalRelativePositionValues::NotDefinedEnumValue;
    std::optional<ExyokiOffice::MeasuringUnits> VerticalOffset;
    std::optional<DocumentFormat::OpenXml::Drawing::Wordprocessing::VerticalAlignmentValues> VerticalAlignment;
};

/**
 * @brief Captures simple anchor positioning coordinates.
 */
struct ImageSimplePosition
{
    ExyokiOffice::MeasuringUnits X;
    ExyokiOffice::MeasuringUnits Y;
};

/**
 * @brief Captures anchor-level options for floating images.
 */
struct ImageAnchorOptions
{
    bool BehindText = false;
    bool AllowOverlap = true;
    bool AnchorLocked = false;
    bool LayoutInCell = true;
    bool SimplePositionEnabled = false;
    UInt32 RelativeHeight = 0;
    std::optional<ImageSimplePosition> SimplePosition;
};

/**
 * @brief Captures how much of the source picture is cropped away on each edge.
 *
 * Values are fractions of the picture's full width/height in the 0.0 to 1.0 range,
 * matching the meaning of WordprocessingML's `a:srcRect` percentages (a value of 0.1
 * crops away 10% of the picture from that edge). The cropped-away area is not part of
 * the visible picture but the underlying image payload is left untouched.
 */
struct ImageCrop
{
    Real Left = 0.0;
    Real Top = 0.0;
    Real Right = 0.0;
    Real Bottom = 0.0;
};

/**
 * @brief Captures the hyperlink attached to an image (`a:hlinkClick`).
 */
struct ImageHyperlink
{
    /// Target URI resolved from the image's external hyperlink relationship.
    std::string Url;
    /// Optional tooltip text shown when hovering over the image.
    std::string Tooltip;
    /// True when the hyperlink is configured to open in a new window/tab.
    bool NewWindow = false;
};

/**
 * @brief High-level wrapper for a WordprocessingML hyperlink (`<w:hyperlink>`).
 *
 * A hyperlink wraps one or more runs and points either to an external target
 * (a URL, backed by an external relationship on the main document part) or an
 * internal target (a bookmark in the same document, referenced by name via
 * `w:anchor`). The two target kinds are mutually exclusive in this API:
 * calling SetUrl() clears any anchor, and calling SetAnchor() clears any
 * existing relationship.
 *
 * Instances are normally obtained from Paragraph::AddHyperlink(),
 * Paragraph::AddInternalHyperlink(), or Paragraph::Hyperlinks(), which already
 * attach the owning main document part when one is available.
 *
 * @code
 * using namespace ExyokiOffice::Word;
 * auto link = paragraph->AddHyperlink("Docs", "https://example.com/docs");
 * link->SetTooltip("Opens the documentation site").SetNewWindow(true);
 *
 * for (const auto& existing : paragraph->Hyperlinks())
 * {
 *     if (existing->IsExternal())
 *     {
 *         std::cout << existing->GetUrl() << "\n";
 *     }
 * }
 * @endcode
 */
class EXYOKIOFFICE_EXPORT Hyperlink
{
public:
    using Ptr = std::shared_ptr<Hyperlink>;

    /**
     * @brief Wraps an existing low-level hyperlink element.
     *
     * @param hyperlink Low-level `<w:hyperlink>` element to wrap.
     * @param mainDocumentPart Optional main document part of the package.
     * @param owningPart Part that stores this hyperlink's relationship; null means the
     * main document part. A hyperlink in a header, footer, footnote, endnote, or comment
     * belongs to that part, and resolving `r:id` against the main document's
     * relationships would answer with an unrelated target.
     */
    explicit Hyperlink(const std::shared_ptr<DocumentFormat::OpenXml::Wordprocessing::Hyperlink>& hyperlink,
                       const std::shared_ptr<Packaging::MainDocumentPart>& mainDocumentPart = nullptr,
                       const std::shared_ptr<OpenXmlPackagePart>& owningPart = nullptr);

    /**
     * @brief Returns the underlying low-level hyperlink element.
     * @return The wrapped element, or nullptr when this wrapper is detached.
     */
    std::shared_ptr<DocumentFormat::OpenXml::Wordprocessing::Hyperlink> GetLowLevelApi() const;

    /**
     * @brief Attaches (or replaces) the main document part of this hyperlink's package.
     *
     * @param mainDocumentPart Main document part that owns this hyperlink's package.
     * @return Reference to this hyperlink for fluent chaining.
     */
    Hyperlink& AttachMainDocumentPart(const std::shared_ptr<Packaging::MainDocumentPart>& mainDocumentPart);

    /**
     * @brief Attaches (or replaces) the part whose relationships hold this link's target.
     *
     * @param owningPart Part that stores this hyperlink; null means the main document part.
     * @return Reference to this hyperlink for fluent chaining.
     */
    Hyperlink& AttachOwningPart(const std::shared_ptr<OpenXmlPackagePart>& owningPart);

    /** @brief Part whose relationships this hyperlink reads and writes; see AttachOwningPart(). */
    [[nodiscard]] std::shared_ptr<OpenXmlPackagePart> OwningPart() const;

    /**
     * @brief Appends an empty run (`<w:r>`) to the hyperlink.
     *
     * @return A high-level Run wrapper, or nullptr if the hyperlink is invalid.
     */
    std::shared_ptr<Run> AddRun();

    /**
     * @brief Appends text as a new run to the hyperlink.
     *
     * @param text Text content to insert.
     * @param preserveSpaces Writes `xml:space="preserve"`; on by default, see Run::AddText.
     * @return A Text wrapper, or nullptr if the hyperlink is invalid.
     */
    std::shared_ptr<Text> AddText(std::string_view text, bool preserveSpaces = true);

    /**
     * @brief Enumerates direct run children of this hyperlink.
     *
     * @return Run wrappers in hyperlink order.
     */
    std::vector<std::shared_ptr<Run>> Runs() const;

    /**
     * @brief Concatenates all text from this hyperlink's runs.
     *
     * @return Plain text stored inside the hyperlink.
     */
    std::string PlainText() const;

    /**
     * @brief Checks whether this hyperlink targets an external URL.
     *
     * @return True when a relationship ID (`r:id`) is present.
     */
    [[nodiscard]] bool IsExternal() const;

    /**
     * @brief Checks whether this hyperlink targets a bookmark in the same document.
     *
     * @return True when a `w:anchor` is present and no relationship ID is set.
     */
    [[nodiscard]] bool IsInternal() const;

    /**
     * @brief Sets the external target URL, creating an external relationship.
     *
     * Replaces any existing relationship and clears an existing internal anchor.
     * Requires a main document part to be attached; otherwise this is a no-op.
     *
     * @param url Target URL. Must be non-empty.
     * @return Reference to this hyperlink for fluent chaining.
     */
    Hyperlink& SetUrl(std::string_view url);

    /**
     * @brief Resolves and returns the external target URL.
     *
     * @return Target URL, or an empty string when this hyperlink is not external or no
     * main document part is attached.
     */
    std::string GetUrl() const;

    /**
     * @brief Sets the internal bookmark target, clearing any external relationship.
     *
     * @param bookmarkName Target bookmark name. Must be non-empty.
     * @return Reference to this hyperlink for fluent chaining.
     */
    Hyperlink& SetAnchor(std::string_view bookmarkName);

    /**
     * @brief Reads the internal bookmark target name.
     *
     * @return Bookmark name, or an empty string when this hyperlink has no anchor.
     */
    std::string GetAnchor() const;

    /**
     * @brief Sets the hyperlink screen-tip text.
     *
     * @param tooltip Tooltip text. Pass an empty string to clear it.
     * @return Reference to this hyperlink for fluent chaining.
     */
    Hyperlink& SetTooltip(std::string_view tooltip);

    /**
     * @brief Reads the hyperlink screen-tip text.
     *
     * @return Tooltip text, or an empty string when absent.
     */
    std::string GetTooltip() const;

    /**
     * @brief Sets whether the target opens in a new window/tab.
     *
     * @param enabled True to open in a new window; false to open in the current one.
     * @return Reference to this hyperlink for fluent chaining.
     */
    Hyperlink& SetNewWindow(bool enabled);

    /**
     * @brief Reads whether the target opens in a new window/tab.
     *
     * @return True when configured to open in a new window/tab.
     */
    [[nodiscard]] bool GetNewWindow() const;

    /**
     * @brief Removes this hyperlink, including any relationship and the runs it contains.
     *
     * To keep the visible text as plain (non-linked) content, add replacement runs with
     * the desired text to the paragraph before calling this.
     */
    void Remove();

private:
    void RemoveExistingRelationship();

    std::shared_ptr<DocumentFormat::OpenXml::Wordprocessing::Hyperlink> m_hyperlink;
    std::shared_ptr<Packaging::MainDocumentPart> m_mainDocumentPart;
    std::shared_ptr<OpenXmlPackagePart> m_owningPart;
};

/**
 * @brief High-level wrapper for a named bookmark (`<w:bookmarkStart>`/`<w:bookmarkEnd>` pair).
 *
 * Bookmarks mark a named location or range in a document. They are used as targets for
 * internal hyperlinks (Paragraph::AddInternalHyperlink()) and cross-references. This
 * wrapper only exposes the bookmark's identity and removal; editing the covered content
 * is done through the normal Paragraph/Run API.
 */
class EXYOKIOFFICE_EXPORT Bookmark
{
public:
    using Ptr = std::shared_ptr<Bookmark>;

    /**
     * @brief Wraps an existing bookmark start/end pair.
     *
     * @param start Low-level `<w:bookmarkStart>` element.
     * @param end Low-level `<w:bookmarkEnd>` element, or nullptr when no matching end was found.
     */
    explicit Bookmark(const std::shared_ptr<DocumentFormat::OpenXml::Wordprocessing::BookmarkStart>& start,
                      const std::shared_ptr<DocumentFormat::OpenXml::Wordprocessing::BookmarkEnd>& end = nullptr);

    /**
     * @brief Returns the underlying low-level bookmark start element.
     * @return The `w:bookmarkStart`, or nullptr when this wrapper is detached.
     */
    std::shared_ptr<DocumentFormat::OpenXml::Wordprocessing::BookmarkStart> GetStartElement() const;

    /**
     * @brief Returns the underlying low-level bookmark end element, if resolved.
     * @return The `w:bookmarkEnd`, or nullptr when the bookmark has no end marker.
     */
    std::shared_ptr<DocumentFormat::OpenXml::Wordprocessing::BookmarkEnd> GetEndElement() const;

    /**
     * @brief Reads the bookmark name.
     *
     * @return Bookmark name, or an empty string when this wrapper is invalid.
     */
    std::string GetName() const;

    /**
     * @brief Reads the bookmark's numeric ID.
     *
     * @return Bookmark ID, or -1 when this wrapper is invalid or the ID cannot be parsed.
     */
    int GetId() const;

    /**
     * @brief Removes both the bookmarkStart and bookmarkEnd elements from the document.
     *
     * Content between the start and end markers (paragraphs, runs, text) is left intact;
     * only the bookmark markers themselves are removed.
     */
    void Remove();

private:
    std::shared_ptr<DocumentFormat::OpenXml::Wordprocessing::BookmarkStart> m_start;
    std::shared_ptr<DocumentFormat::OpenXml::Wordprocessing::BookmarkEnd> m_end;
};

/**
 * @brief Identifies the OpenXML representation used by a field.
 */
enum class FieldKind
{
    /// The field is stored as a single `w:fldSimple` element.
    Simple,
    /// The field is stored as complex `w:fldChar` begin/separate/end markers.
    Complex
};

/**
 * @brief High-level wrapper for Word fields.
 *
 * Word fields store an instruction (for example `DATE`, `PAGE`, `REF Name`,
 * or `MERGEFIELD CustomerName`) and a cached result visible in the document.
 * ExyokiOffice can create, parse, inspect, and edit those stored OpenXML
 * structures, but it deliberately does not evaluate field instructions. This
 * distinction is especially important for layout-dependent fields such as
 * `PAGE`, `NUMPAGES`, and `SECTIONPAGES`: without a layout engine the library
 * cannot know the correct result, so these fields are only marked dirty.
 *
 * Field wrappers are normally obtained from Paragraph::AddField(),
 * Paragraph::AddSimpleField(), Paragraph::Fields(), or WordDocumentEditor::Fields().
 * The wrapper references the existing DOM; mutating it updates the document.
 */
class EXYOKIOFFICE_EXPORT Field
{
public:
    using Ptr = std::shared_ptr<Field>;

    /**
     * @brief Wraps a simple field element.
     *
     * @param simpleField Low-level `w:fldSimple` element.
     */
    explicit Field(const std::shared_ptr<DocumentFormat::OpenXml::Wordprocessing::SimpleField>& simpleField);

    /**
     * @brief Wraps a complex field marker range.
     *
     * @param paragraph Paragraph that owns the field marker runs.
     * @param begin Begin `w:fldChar` marker.
     * @param separator Optional separate `w:fldChar` marker.
     * @param end End `w:fldChar` marker.
     * @param beginRun Optional run that directly owns the begin marker.
     * @param separatorRun Optional run that directly owns the separator marker.
     * @param endRun Optional run that directly owns the end marker.
     * @param instructionSnapshot Optional instruction text captured while parsing.
     * @param resultSnapshot Optional cached result text captured while parsing.
     */
    Field(const std::shared_ptr<DocumentFormat::OpenXml::Wordprocessing::Paragraph>& paragraph,
          const std::shared_ptr<DocumentFormat::OpenXml::Wordprocessing::FieldChar>& begin,
          const std::shared_ptr<DocumentFormat::OpenXml::Wordprocessing::FieldChar>& separator,
          const std::shared_ptr<DocumentFormat::OpenXml::Wordprocessing::FieldChar>& end,
          const std::shared_ptr<DocumentFormat::OpenXml::Wordprocessing::Run>& beginRun = nullptr,
          const std::shared_ptr<DocumentFormat::OpenXml::Wordprocessing::Run>& separatorRun = nullptr,
          const std::shared_ptr<DocumentFormat::OpenXml::Wordprocessing::Run>& endRun = nullptr,
          std::optional<std::string> instructionSnapshot = std::nullopt,
          std::optional<std::string> resultSnapshot = std::nullopt);

    /**
     * @brief Returns the field representation.
     */
    FieldKind Kind() const noexcept;

    /**
     * @brief Returns the low-level `w:fldSimple` element for simple fields.
     *
     * @return Simple field element, or nullptr for complex fields.
     */
    std::shared_ptr<DocumentFormat::OpenXml::Wordprocessing::SimpleField> GetSimpleFieldElement() const;

    /**
     * @brief Returns the low-level begin `w:fldChar` marker for complex fields.
     *
     * @return Begin marker, or nullptr for simple fields.
     */
    std::shared_ptr<DocumentFormat::OpenXml::Wordprocessing::FieldChar> GetBeginFieldCharElement() const;

    /**
     * @brief Returns the low-level separate `w:fldChar` marker for complex fields.
     *
     * @return Separator marker, or nullptr when the complex field has no separator.
     */
    std::shared_ptr<DocumentFormat::OpenXml::Wordprocessing::FieldChar> GetSeparatorFieldCharElement() const;

    /**
     * @brief Returns the low-level end `w:fldChar` marker for complex fields.
     *
     * @return End marker, or nullptr for simple fields.
     */
    std::shared_ptr<DocumentFormat::OpenXml::Wordprocessing::FieldChar> GetEndFieldCharElement() const;

    /**
     * @brief Reads the field instruction text.
     *
     * For simple fields this returns the `w:instr` attribute. For complex
     * fields it concatenates `w:instrText` nodes between the begin marker and
     * the separator/end marker.
     *
     * @return Field instruction, or an empty string when unavailable.
     */
    std::string GetInstruction() const;

    /**
     * @brief Replaces the field instruction and marks the field result dirty.
     *
     * The stored cached result is left intact. A later layout-aware processor,
     * usually Word, can recalculate it because the field is marked dirty.
     *
     * @param instruction New field instruction text. Empty text is allowed and
     *        leaves a syntactically present but semantically empty instruction.
     * @return Reference to this field for fluent chaining.
     */
    Field& SetInstruction(std::string_view instruction);

    /**
     * @brief Reads the cached visible field result.
     *
     * This returns the stored result text only; it does not evaluate the field.
     *
     * @return Cached result text, or an empty string when no result is stored.
     */
    std::string GetResult() const;

    /**
     * @brief Replaces the cached field result when that is safe to do without layout.
     *
     * Non-layout fields have their result content replaced with a single run
     * containing the provided text. For layout-dependent fields such as `PAGE`,
     * `NUMPAGES`, and `SECTIONPAGES`, the method does not overwrite the cached
     * result because the library cannot compute a truthful value; it marks the
     * field dirty and returns false.
     *
     * @param result New cached result text.
     * @param preserveSpaces Writes `xml:space="preserve"`; on by default, see Run::AddText.
     * @return True when the cached result was replaced; false when the field was
     *         only marked dirty because it depends on layout or is invalid.
     */
    bool SetResult(std::string_view result, bool preserveSpaces = true);

    /**
     * @brief Marks the field result as invalid and in need of recalculation.
     *
     * This is the correct operation after changing an instruction when the
     * library cannot or should not compute the result itself.
     *
     * @return Reference to this field for fluent chaining.
     */
    Field& InvalidateResult();

    /**
     * @brief Reads whether the field is marked dirty.
     *
     * @return True when the OpenXML dirty flag is present and enabled.
     */
    [[nodiscard]] bool IsDirty() const;

    /**
     * @brief Returns true for fields whose correct result depends on pagination or layout.
     *
     * The detection is intentionally conservative and recognizes common
     * built-in field names such as `PAGE`, `NUMPAGES`, and `SECTIONPAGES`.
     */
    [[nodiscard]] bool IsLayoutDependent() const;

private:
    std::shared_ptr<DocumentFormat::OpenXml::Wordprocessing::SimpleField> m_simpleField;
    std::shared_ptr<DocumentFormat::OpenXml::Wordprocessing::Paragraph> m_paragraph;
    std::shared_ptr<DocumentFormat::OpenXml::Wordprocessing::FieldChar> m_begin;
    std::shared_ptr<DocumentFormat::OpenXml::Wordprocessing::FieldChar> m_separator;
    std::shared_ptr<DocumentFormat::OpenXml::Wordprocessing::FieldChar> m_end;
    std::shared_ptr<DocumentFormat::OpenXml::Wordprocessing::Run> m_beginRun;
    std::shared_ptr<DocumentFormat::OpenXml::Wordprocessing::Run> m_separatorRun;
    std::shared_ptr<DocumentFormat::OpenXml::Wordprocessing::Run> m_endRun;
    std::optional<std::string> m_instructionSnapshot;
    std::optional<std::string> m_resultSnapshot;
};

/**
 * @brief High-level wrapper for one tracked Word revision container.
 *
 * A Revision wraps an existing WordprocessingML revision element such as
 * `w:ins`, `w:del`, `w:moveFrom`, or `w:moveTo`. The wrapper is deliberately
 * structural: it preserves the original XML and exposes common metadata,
 * visible/deleted text, and accept/reject operations without attempting to
 * evaluate Word's complete revision model.
 *
 * Accepting an insertion keeps its children and removes the revision wrapper.
 * Accepting a deletion removes the wrapper and its children. Rejecting performs
 * the inverse operation. For deleted text, Reject() converts `w:delText` back
 * to normal `w:t` text so the restored content becomes visible through the
 * high-level paragraph/run APIs.
 *
 * @see WordDocumentEditor::Revisions
 * @see WordDocumentEditor::AcceptAllRevisions
 * @see WordDocumentEditor::RejectAllRevisions
 */
class EXYOKIOFFICE_EXPORT Revision
{
public:
    using Ptr = std::shared_ptr<Revision>;

    /**
     * @brief Wraps an existing low-level revision element.
     *
     * @param element Revision container or property-change marker.
     */
    explicit Revision(const std::shared_ptr<ExyokiOffice::OpenXMLElement>& element);

    /**
     * @brief Returns the wrapped low-level revision element.
     * @return The wrapped element, or nullptr when this wrapper is detached.
     */
    std::shared_ptr<ExyokiOffice::OpenXMLElement> GetLowLevelApi() const;

    /**
     * @brief Returns the detected revision type.
     */
    RevisionType Type() const;

    /**
     * @brief Reads the revision identifier from `w:id`.
     */
    std::string GetId() const;

    /**
     * @brief Reads the author stored on the revision.
     */
    std::string GetAuthor() const;

    /**
     * @brief Reads the revision date text stored on `w:date`.
     */
    std::string GetDate() const;

    /**
     * @brief Concatenates text represented by this revision.
     *
     * Insertions return normal `w:t` text. Deletions return `w:delText` text,
     * which is intentionally not part of Paragraph::PlainText() until the
     * deletion is rejected.
     */
    std::string Text() const;

    /**
     * @brief Accepts this revision in place.
     *
     * @return True when the wrapped XML was processed; false when the wrapper is invalid.
     */
    bool Accept();

    /**
     * @brief Rejects this revision in place.
     *
     * @return True when the wrapped XML was processed; false when the wrapper is invalid.
     */
    bool Reject();

private:
    std::shared_ptr<ExyokiOffice::OpenXMLElement> m_element;
};

/**
 * @brief High-level wrapper for a Word footnote or endnote entry.
 *
 * Word stores footnote and endnote content in separate package parts
 * (`/word/footnotes.xml` and `/word/endnotes.xml`), each holding a flat list
 * of entries identified by a numeric ID. The visible body text only carries
 * a reference marker (`<w:footnoteReference>`/`<w:endnoteReference>`)
 * pointing at that ID; the actual note text lives in the separate part.
 *
 * Note wrappers are normally obtained from Paragraph::AddFootnote(),
 * Paragraph::AddEndnote(), WordDocumentEditor::Footnotes(), or
 * WordDocumentEditor::Endnotes(). The wrapper references the existing DOM,
 * so editing it updates the document.
 *
 * The library does not lay out or renumber notes; it only manages the
 * package structure (parts, IDs, reference markers) needed to create, read,
 * edit, and remove note content safely.
 *
 * @code
 * using namespace ExyokiOffice::Word;
 *
 * auto editor = WordDocumentEditor::CreateNew();
 * auto paragraph = editor->AddParagraph("See the note below");
 * auto note = paragraph->AddFootnote("Explanatory text.");
 *
 * for (const auto& footnote : editor->Footnotes())
 * {
 *     if (footnote->GetEntryType() == NoteEntryType::Normal)
 *     {
 *         // footnote->PlainText() == "Explanatory text."
 *     }
 * }
 * @endcode
 */
class EXYOKIOFFICE_EXPORT Note
{
public:
    using Ptr = std::shared_ptr<Note>;

    /**
     * @brief Wraps an existing footnote or endnote entry.
     *
     * @param kind Whether the wrapped entry is a footnote or an endnote.
     * @param element Low-level `w:footnote`/`w:endnote` entry element. This is kept as a
     * generic element (rather than a typed `Footnote`/`Endnote` pointer) because both share
     * their element name with an unrelated special-reference type
     * (`FootnoteSpecialReference`/`EndnoteSpecialReference`); reading ID/type therefore goes
     * through generic attribute access instead of the typed accessors.
     * @param mainDocumentPart Optional owning main document part. Required for Remove() to
     * also clean up the matching reference marker in the document body.
     */
    Note(NoteKind kind,
         const std::shared_ptr<ExyokiOffice::OpenXMLElement>& element,
         const std::shared_ptr<Packaging::MainDocumentPart>& mainDocumentPart = nullptr);

    /**
     * @brief Returns whether this wrapper represents a footnote or an endnote.
     */
    NoteKind Kind() const noexcept;

    /**
     * @brief Returns the underlying low-level footnote/endnote entry element.
     * @return The wrapped element, or nullptr when this wrapper is detached.
     */
    std::shared_ptr<ExyokiOffice::OpenXMLElement> GetLowLevelApi() const;

    /**
     * @brief Reads the note's numeric ID, as referenced by the body reference marker.
     *
     * @return Note ID, or -1 when this wrapper is invalid.
     */
    int GetId() const;

    /**
     * @brief Reads the entry role (normal content, separator, or continuation separator).
     */
    NoteEntryType GetEntryType() const;

    /**
     * @brief Enumerates the paragraphs that make up this note's content.
     */
    std::vector<std::shared_ptr<Paragraph>> Paragraphs() const;

    /**
     * @brief The footnotes or endnotes part this note lives in.
     *
     * Paragraphs handed out by this note carry it, so a hyperlink added inside
     * a footnote records its target in `footnotes.xml.rels` where Word looks
     * for it. Null when no main document part is attached.
     */
    [[nodiscard]] std::shared_ptr<OpenXmlPackagePart> OwningPart() const;

    /**
     * @brief Appends an additional paragraph to this note's content.
     *
     * @param text Optional text content for the new paragraph.
     * @param preserveSpaces Writes `xml:space="preserve"`; on by default, see Run::AddText.
     * @return Paragraph wrapper, or nullptr when this wrapper is invalid.
     */
    std::shared_ptr<Paragraph> AddParagraph(std::string_view text = {}, bool preserveSpaces = true);

    /**
     * @brief Concatenates the plain text of every paragraph in this note.
     */
    std::string PlainText() const;

    /**
     * @brief Removes all paragraphs from this note, leaving it empty.
     *
     * @return Reference to this note for fluent chaining.
     */
    Note& Clear();

    /**
     * @brief Replaces this note's content with a single paragraph.
     *
     * The new paragraph begins with the standard footnote/endnote reference
     * mark (`<w:footnoteRef>`/`<w:endnoteRef>`) followed by the given text,
     * matching the structure Word itself generates.
     *
     * @param text New note text.
     * @param preserveSpaces Writes `xml:space="preserve"`; on by default, see Run::AddText.
     * @return Reference to this note for fluent chaining.
     */
    Note& SetText(std::string_view text, bool preserveSpaces = true);

    /**
     * @brief Removes this note entry and its body reference marker.
     *
     * When a main document part was attached, every `<w:footnoteReference>`/
     * `<w:endnoteReference>` in the document body that targets this note's ID
     * is removed along with the run that owns it. The entry is then removed
     * from the footnotes/endnotes part. After this call the wrapper is invalid.
     */
    void Remove();

private:
    NoteKind m_kind = NoteKind::Footnote;
    std::shared_ptr<ExyokiOffice::OpenXMLElement> m_element;
    std::shared_ptr<Packaging::MainDocumentPart> m_mainDocumentPart;
};

/**
 * @brief High-level wrapper for a classic (non-threaded) Word comment.
 *
 * Word stores comment text in a separate package part (`/word/comments.xml`)
 * as a flat list of `<w:comment>` entries identified by a numeric ID. The
 * commented range in the document body is marked by a
 * `<w:commentRangeStart>`/`<w:commentRangeEnd>` pair, followed by a run
 * containing a `<w:commentReference>` pointing at that ID.
 *
 * This wrapper supports both the classic comment model used by every version
 * of Word and the modern threaded model: AddReply() adds a reply to the
 * thread, Replies() and GetParent() navigate it, and SetResolved()/
 * IsResolved() carry the thread's resolution state. Threading is not stored in
 * `/word/comments.xml` at all — a reply is a plain `<w:comment>` entry with its
 * own ID and its own range markers, and the parent/child relation lives in the
 * satellite parts Word writes alongside it: `commentsExtended` (thread shape
 * and resolution), `commentsIds` and `commentsExtensible` (durable identity and
 * UTC timestamp), and `people` (author presence). This class maintains those
 * parts as typed package parts, so they stay consistent with the comment
 * entries instead of being carried along untouched.
 *
 * Comment wrappers are normally obtained from Paragraph::AddComment(),
 * Paragraph::AddCommentOnParagraph(), or WordDocumentEditor::Comments().
 *
 * @code
 * using namespace ExyokiOffice::Word;
 *
 * auto editor = WordDocumentEditor::CreateNew();
 * auto paragraph = editor->AddParagraph("Please review this section.");
 *
 * CommentAuthor author;
 * author.Name = "Reviewer";
 * author.Initials = "RV";
 * auto comment = paragraph->AddCommentOnParagraph("Needs a citation.", author);
 *
 * for (const auto& existing : editor->Comments())
 * {
 *     // existing->GetAuthor() == "Reviewer"
 * }
 * @endcode
 */
class EXYOKIOFFICE_EXPORT Comment
{
public:
    using Ptr = std::shared_ptr<Comment>;

    /**
     * @brief Wraps an existing comment entry.
     *
     * @param comment Low-level `w:comment` entry element.
     * @param mainDocumentPart Optional owning main document part. Required for Remove() to
     * also clean up the matching range markers and reference in the document body.
     */
    explicit Comment(const std::shared_ptr<DocumentFormat::OpenXml::Wordprocessing::Comment>& comment,
                     const std::shared_ptr<Packaging::MainDocumentPart>& mainDocumentPart = nullptr);

    /**
     * @brief Returns the underlying low-level comment entry element.
     * @return The wrapped element, or nullptr when this wrapper is detached.
     */
    std::shared_ptr<DocumentFormat::OpenXml::Wordprocessing::Comment> GetLowLevelApi() const;

    /**
     * @brief Reads the comment's numeric ID, as referenced by the body range markers.
     *
     * @return Comment ID, or -1 when this wrapper is invalid.
     */
    int GetId() const;

    /**
     * @brief The comments part this comment lives in.
     *
     * Paragraphs handed out by this comment carry it, so a hyperlink added
     * inside a comment records its target in `comments.xml.rels`. Null when no
     * main document part is attached, or when the document has no comments part.
     */
    [[nodiscard]] std::shared_ptr<OpenXmlPackagePart> OwningPart() const;

    /**
     * @brief Reads the comment author's display name.
     */
    std::string GetAuthor() const;

    /**
     * @brief Sets the comment author's display name.
     *
     * @return Reference to this comment for fluent chaining.
     */
    Comment& SetAuthor(std::string_view author);

    /**
     * @brief Reads the comment author's initials.
     */
    std::string GetInitials() const;

    /**
     * @brief Sets the comment author's initials.
     *
     * @return Reference to this comment for fluent chaining.
     */
    Comment& SetInitials(std::string_view initials);

    /**
     * @brief Reads the comment's authoring timestamp, if present.
     */
    std::optional<std::chrono::system_clock::time_point> GetDate() const;

    /**
     * @brief Sets the comment's authoring timestamp.
     *
     * @return Reference to this comment for fluent chaining.
     */
    Comment& SetDate(std::chrono::system_clock::time_point date);

    /**
     * @brief Enumerates the paragraphs that make up this comment's text.
     */
    std::vector<std::shared_ptr<Paragraph>> Paragraphs() const;

    /**
     * @brief Appends an additional paragraph to this comment's text.
     *
     * @param text Optional text content for the new paragraph.
     * @param preserveSpaces Writes `xml:space="preserve"`; on by default, see Run::AddText.
     * @return Paragraph wrapper, or nullptr when this wrapper is invalid.
     */
    std::shared_ptr<Paragraph> AddParagraph(std::string_view text = {}, bool preserveSpaces = true);

    /**
     * @brief Concatenates the plain text of every paragraph in this comment.
     */
    std::string PlainText() const;

    /**
     * @brief Removes all paragraphs from this comment's text, leaving it empty.
     *
     * A comment that takes part in a thread keeps one empty paragraph: it
     * carries the paragraph ID the thread is keyed by, and dropping it would
     * orphan this comment's replies and its resolution state.
     *
     * @return Reference to this comment for fluent chaining.
     */
    Comment& Clear();

    /**
     * @brief Replaces this comment's text with a single paragraph.
     *
     * @param text New comment text.
     * @param preserveSpaces Writes `xml:space="preserve"`; on by default, see Run::AddText.
     * @return Reference to this comment for fluent chaining.
     */
    Comment& SetText(std::string_view text, bool preserveSpaces = true);

    /**
     * @brief Appends a reply to this comment's thread.
     *
     * The reply is a full comment in its own right: it gets its own numeric ID,
     * its own `<w:comment>` entry, and its own range markers covering exactly
     * the same body span as this comment. Only the `commentsExtended` part
     * records that it answers this comment.
     *
     * @param text Reply text.
     * @param author Optional display name and initials for the reply's author.
     * @return Wrapper for the new reply, or nullptr when this comment is not
     * attached to a main document part or the reply could not be placed.
     */
    std::shared_ptr<Comment> AddReply(std::string_view text, const CommentAuthor& author = {});

    /**
     * @brief Enumerates the direct replies to this comment, in document order.
     *
     * Only immediate children are returned; a reply's own replies are reached
     * by calling Replies() on it.
     */
    [[nodiscard]] std::vector<std::shared_ptr<Comment>> Replies() const;

    /**
     * @brief Returns the comment this one replies to.
     *
     * @return Wrapper for the parent comment, or nullptr when this comment is
     * the root of its thread.
     */
    [[nodiscard]] std::shared_ptr<Comment> GetParent() const;

    /**
     * @brief Tells whether this comment is marked as resolved.
     */
    [[nodiscard]] bool IsResolved() const;

    /**
     * @brief Marks the whole thread this comment belongs to as resolved or open.
     *
     * Word resolves threads, not individual comments, so the flag is written on
     * the thread's root comment and on every reply below it, no matter which
     * member of the thread this wrapper points at.
     *
     * @param resolved Whether the thread is resolved.
     * @return Reference to this comment for fluent chaining.
     */
    Comment& SetResolved(bool resolved);

    /**
     * @brief Removes this comment entry and its body range markers/reference.
     *
     * When a main document part was attached, every `<w:commentRangeStart>`/
     * `<w:commentRangeEnd>` and `<w:commentReference>` in the document body
     * that targets this comment's ID is removed (the reference's owning run
     * is removed along with it). The entry is then removed from the comments
     * part, together with its rows in the `commentsExtended`, `commentsIds`
     * and `commentsExtensible` parts. Replies are removed recursively, since a
     * reply cannot outlive the comment it answers. The `people` part is left
     * untouched: authors outlive their comments there, as in Word. After this
     * call the wrapper is invalid.
     */
    void Remove();

private:
    std::shared_ptr<DocumentFormat::OpenXml::Wordprocessing::Comment> m_comment;
    std::shared_ptr<Packaging::MainDocumentPart> m_mainDocumentPart;
};

/**
 * @brief High-level wrapper for a Word content control (structured document tag, `<w:sdt>`).
 *
 * Content controls (structured document tags) let a document expose bound or
 * templated regions with an identity (ID), a programmatic tag, a display
 * alias, an optional lock, and placeholder behavior. Word represents both
 * block-level content controls (siblings of paragraphs and tables) and
 * inline content controls (living inside a paragraph alongside runs) with
 * the same `<w:sdt>` element; ContentControlLevel records which shape this
 * wrapper has.
 *
 * This wrapper covers the common "rich text-like" content control: an ID,
 * tag, alias, lock, and showing-placeholder flag, plus plain paragraph/run
 * content. Specialized content types (checkboxes, date pickers, drop-down
 * lists, repeating sections) are preserved as opaque XML when present but are
 * not exposed through a dedicated API.
 *
 * Content control wrappers are normally obtained from
 * WordDocumentEditor::BodyCursor::InsertContentControl() (block-level),
 * Paragraph::AddInlineContentControl() (inline), WordDocumentEditor::ContentControls(),
 * or Paragraph::ContentControls().
 *
 * @code
 * using namespace ExyokiOffice::Word;
 *
 * auto editor = WordDocumentEditor::CreateNew();
 * auto control = editor->Body().InsertContentControl("customerName", "Customer Name");
 * control->SetText("Acme Corp.");
 *
 * auto paragraph = editor->AddParagraph("Status: ");
 * auto inlineControl = paragraph->AddInlineContentControl("status", "Status");
 * inlineControl->SetText("Draft");
 * @endcode
 */
class EXYOKIOFFICE_EXPORT ContentControl
{
public:
    using Ptr = std::shared_ptr<ContentControl>;

    /**
     * @brief Wraps an existing `<w:sdt>` element.
     *
     * @param sdt Low-level `w:sdt` element (block or run variant).
     * @param level Whether the wrapped element is block-level or inline.
     * @param mainDocumentPart Optional owning main document part, used for paragraph/run
     * wrappers returned by this content control and for document-wide ID allocation.
     * @param owningPart Part the control lives in; null means the main document part.
     * Content controls appear in headers, footers, and notes as well, and the paragraphs
     * handed out here inherit this part for their relationships.
     */
    ContentControl(const std::shared_ptr<ExyokiOffice::OpenXMLElement>& sdt,
                   ContentControlLevel level,
                   const std::shared_ptr<Packaging::MainDocumentPart>& mainDocumentPart = nullptr,
                   const std::shared_ptr<OpenXmlPackagePart>& owningPart = nullptr);

    /** @brief Part whose relationships the content of this control reads and writes. */
    [[nodiscard]] std::shared_ptr<OpenXmlPackagePart> OwningPart() const;

    /**
     * @brief Returns the underlying low-level `w:sdt` element.
     * @return The wrapped element, or nullptr when this wrapper is detached.
     */
    std::shared_ptr<ExyokiOffice::OpenXMLElement> GetLowLevelApi() const;

    /**
     * @brief Returns whether this content control is block-level or inline.
     */
    ContentControlLevel Level() const noexcept;

    /**
     * @brief Checks whether this content control is block-level.
     */
    [[nodiscard]] bool IsBlock() const noexcept;

    /**
     * @brief Checks whether this content control is inline (run-level).
     */
    [[nodiscard]] bool IsInline() const noexcept;

    /**
     * @brief Reads the content control's numeric ID.
     *
     * @return ID value, or -1 when no ID has been assigned yet.
     */
    int GetId() const;

    /**
     * @brief Ensures this content control has an assigned, document-unique ID.
     *
     * This is called automatically by the methods that create new content
     * controls. It is idempotent: calling it again on a control that already
     * has an ID simply returns the existing value.
     *
     * @return The content control's ID.
     */
    int EnsureId();

    /**
     * @brief Sets the programmatic tag (`w:tag`) used to identify this control in code.
     *
     * @return Reference to this content control for fluent chaining.
     */
    ContentControl& SetTag(std::string_view tag);

    /**
     * @brief Reads the programmatic tag, if present.
     */
    std::string GetTag() const;

    /**
     * @brief Sets the human-readable alias (`w:alias`) shown in Word's UI.
     *
     * @return Reference to this content control for fluent chaining.
     */
    ContentControl& SetAlias(std::string_view alias);

    /**
     * @brief Reads the human-readable alias, if present.
     */
    std::string GetAlias() const;

    /**
     * @brief Sets the editing lock applied to this content control.
     *
     * @return Reference to this content control for fluent chaining.
     */
    ContentControl& SetLock(DocumentFormat::OpenXml::Wordprocessing::LockingValues lock);

    /**
     * @brief Reads the editing lock, if present.
     */
    std::optional<DocumentFormat::OpenXml::Wordprocessing::LockingValues> GetLock() const;

    /**
     * @brief Removes the editing lock, restoring the default (unlocked) behavior.
     *
     * @return Reference to this content control for fluent chaining.
     */
    ContentControl& ClearLock();

    /**
     * @brief Sets whether Word should currently display the placeholder text for this control.
     *
     * @return Reference to this content control for fluent chaining.
     */
    ContentControl& SetShowingPlaceholder(bool enabled);

    /**
     * @brief Reads whether this control is currently showing placeholder text.
     */
    [[nodiscard]] bool IsShowingPlaceholder() const;

    /**
     * @brief Enumerates the paragraphs inside this content control.
     *
     * @return Paragraph wrappers, or an empty vector for inline content controls.
     */
    std::vector<std::shared_ptr<Paragraph>> Paragraphs() const;

    /**
     * @brief Appends a paragraph to this content control's content.
     *
     * @return Paragraph wrapper, or nullptr for inline content controls.
     */
    std::shared_ptr<Paragraph> AddParagraph(std::string_view text = {}, bool preserveSpaces = true);

    /**
     * @brief Enumerates the runs directly inside this content control.
     *
     * @return Run wrappers, or an empty vector for block-level content controls.
     */
    std::vector<std::shared_ptr<Run>> Runs() const;

    /**
     * @brief Appends an empty run to this content control's content.
     *
     * @return Run wrapper, or nullptr for block-level content controls.
     */
    std::shared_ptr<Run> AddRun();

    /**
     * @brief Appends a text run to this content control's content.
     *
     * @return Text wrapper, or nullptr for block-level content controls.
     */
    std::shared_ptr<Text> AddText(std::string_view text, bool preserveSpaces = true);

    /**
     * @brief Concatenates the plain text of this content control's content.
     */
    std::string PlainText() const;

    /**
     * @brief Removes all content, leaving this control empty.
     *
     * @return Reference to this content control for fluent chaining.
     */
    ContentControl& Clear();

    /**
     * @brief Replaces this content control's content with a single paragraph (block-level)
     * or a single run (inline).
     *
     * @return Reference to this content control for fluent chaining.
     */
    ContentControl& SetText(std::string_view text, bool preserveSpaces = true);

    /**
     * @brief Removes the whole content control, including its content.
     *
     * To keep the visible content, read Paragraphs()/Runs()/PlainText() and
     * insert equivalent replacement content into the surrounding body or
     * paragraph before calling this.
     */
    void Remove();

private:
    std::shared_ptr<ExyokiOffice::OpenXMLElement> GetProperties() const;
    std::shared_ptr<ExyokiOffice::OpenXMLElement> EnsureProperties();
    std::shared_ptr<ExyokiOffice::OpenXMLElement> GetContent() const;
    std::shared_ptr<ExyokiOffice::OpenXMLElement> EnsureContent();

    std::shared_ptr<ExyokiOffice::OpenXMLElement> m_sdt;
    ContentControlLevel m_level = ContentControlLevel::Block;
    std::shared_ptr<Packaging::MainDocumentPart> m_mainDocumentPart;
    std::shared_ptr<OpenXmlPackagePart> m_owningPart;
};

/**
 * @brief High-level image wrapper with inline and floating options.
 *
 * This class wraps a `<w:drawing>` element representing an embedded image.
 * Images can be inline (flow with text) or floating (anchored to the page with
 * wrapping behavior). Size and positioning use `ExyokiOffice::MeasuringUnits`
 * and are stored in EMUs (English Metric Units) internally.
 *
 * Typical usage:
 * @code
 * using namespace ExyokiOffice::Word;
 * auto image = editor->AddImageFromFile("logo.png",
 *                                      MeasuringUnits(3.0, MeasurementUnit::Centimeter),
 *                                      MeasuringUnits(2.0, MeasurementUnit::Centimeter));
 * image->SetLayout(ImageLayout::Floating)
 *      .SetWrap(ImageWrap::Square)
 *      .SetPosition(DocumentFormat::OpenXml::Drawing::Wordprocessing::HorizontalRelativePositionValues::Page,
 *                   MeasuringUnits(2.0, MeasurementUnit::Centimeter),
 *                   DocumentFormat::OpenXml::Drawing::Wordprocessing::VerticalRelativePositionValues::Page,
 *                   MeasuringUnits(3.0, MeasurementUnit::Centimeter))
 *      .SetDistanceFromText(MeasuringUnits(2.0, MeasurementUnit::Millimeter),
 *                           MeasuringUnits(2.0, MeasurementUnit::Millimeter),
 *                           MeasuringUnits(2.0, MeasurementUnit::Millimeter),
 *                           MeasuringUnits(2.0, MeasurementUnit::Millimeter));
 * @endcode
 */
class EXYOKIOFFICE_EXPORT Image
{
public:
    using Ptr = std::shared_ptr<Image>;

    /**
     * @brief Wraps an existing low-level drawing element.
     *
     * @param drawing Low-level `<w:drawing>` element to wrap.
     * @param mainDocumentPart Optional owning main document part. Supplying it enables
     * relationship-backed features such as SetHyperlink()/TryGetHyperlink(); it is not
     * required for size, layout, crop, alt text, or transform metadata. Images returned
     * by WordDocumentEditor::AddImageFromFile()/AddImageFromData() already carry this
     * reference. Images obtained through Paragraph::Images() or Run::Images() do not,
     * and should be bound with AttachMainDocumentPart() first if hyperlink support is needed.
     */
    explicit Image(const std::shared_ptr<DocumentFormat::OpenXml::Wordprocessing::Drawing>& drawing,
                   const std::shared_ptr<Packaging::MainDocumentPart>& mainDocumentPart = nullptr);

    /**
     * @brief Returns the underlying low-level drawing element.
     *
     * Use this when you need access to properties or child elements not exposed
     * by the high-level wrapper.
     * @return The wrapped element, or nullptr when this wrapper is detached.
     */
    std::shared_ptr<DocumentFormat::OpenXml::Wordprocessing::Drawing> GetLowLevelApi() const;

    /**
     * @brief Returns the current layout mode.
     *
     * @return Inline if the drawing uses `<wp:inline>`, floating if it uses `<wp:anchor>`.
     */
    ImageLayout GetLayout() const;
    /**
     * @brief Switches between inline and floating layout.
     *
     * Switching layout rebuilds the `<wp:inline>`/`<wp:anchor>` container while
     * preserving the image relationship and size.
     *
     * @param layout Desired layout (inline or floating).
     * @return Reference to this image for fluent chaining.
     */
    Image& SetLayout(ImageLayout layout);

    /**
     * @brief Sets the image size using a measurement unit (stored in EMUs).
     *
     * Updates both the WordprocessingML extent and the embedded drawing shape.
     *
     * @param width Image width in any supported unit.
     * @param height Image height in any supported unit.
     * @return Reference to this image for fluent chaining.
     *
     * @code
     * image->SetSize(MeasuringUnits(4.0, MeasurementUnit::Centimeter),
     *                MeasuringUnits(3.0, MeasurementUnit::Centimeter));
     * @endcode
     */
    Image& SetSize(const ExyokiOffice::MeasuringUnits& width, const ExyokiOffice::MeasuringUnits& height);
    /**
     * @brief Reads the current image size.
     *
     * @param output Receives image width and height in EMUs.
     * @return True if size is defined in the drawing.
     */
    [[nodiscard]] bool TryGetSize(ImageSize& output) const;
    /**
     * @brief Sets the text wrapping style for floating images.
     *
     * If the image is inline, this will switch it to floating layout.
     *
     * @param wrap Wrap strategy (square, tight, etc.).
     * @param wrapText Wrap text preference (both sides, left, right, ...).
     * @return Reference to this image for fluent chaining.
     */
    Image& SetWrap(ImageWrap wrap,
                   DocumentFormat::OpenXml::Drawing::Wordprocessing::WrapTextValues wrapText =
                       DocumentFormat::OpenXml::Drawing::Wordprocessing::WrapTextValues::BothSides);
    /**
     * @brief Reads the current wrapping settings.
     *
     * @param output Receives wrap type and wrap text behavior.
     * @return True if a wrap element exists on the anchor.
     */
    [[nodiscard]] bool TryGetWrap(ImageWrapSettings& output) const;
    /**
     * @brief Sets the image anchor position using a measurement unit (stored in EMUs).
     *
     * This sets the relative anchor and the offset from that anchor.
     * If the image is inline, this will switch it to floating layout.
     * Note: when simple positioning is enabled, Word may ignore these offsets.
     *
     * @param horizontalFrom Horizontal anchor reference (page, margin, column, ...).
     * @param horizontalOffset Horizontal offset in any supported unit.
     * @param verticalFrom Vertical anchor reference (page, margin, paragraph, ...).
     * @param verticalOffset Vertical offset in any supported unit.
     * @return Reference to this image for fluent chaining.
     *
     * @code
     * image->SetPosition(
     *     DocumentFormat::OpenXml::Drawing::Wordprocessing::HorizontalRelativePositionValues::Margin,
     *     MeasuringUnits(1.0, MeasurementUnit::Centimeter),
     *     DocumentFormat::OpenXml::Drawing::Wordprocessing::VerticalRelativePositionValues::Paragraph,
     *     MeasuringUnits(0.5, MeasurementUnit::Centimeter));
     * @endcode
     */
    Image& SetPosition(DocumentFormat::OpenXml::Drawing::Wordprocessing::HorizontalRelativePositionValues horizontalFrom,
                       const ExyokiOffice::MeasuringUnits& horizontalOffset,
                       DocumentFormat::OpenXml::Drawing::Wordprocessing::VerticalRelativePositionValues verticalFrom,
                       const ExyokiOffice::MeasuringUnits& verticalOffset);
    /**
     * @brief Sets the anchor position using alignment instead of offsets.
     *
     * This uses `<wp:align>` for horizontal/vertical alignment relative to the
     * provided anchor reference. If the image is inline, this will switch it to
     * floating layout. Any existing position offsets are removed.
     *
     * @param horizontalFrom Horizontal anchor reference (page, margin, column, ...).
     * @param horizontalAlign Relative horizontal alignment (left, center, right, ...).
     * @param verticalFrom Vertical anchor reference (page, margin, paragraph, ...).
     * @param verticalAlign Relative vertical alignment (top, center, bottom, ...).
     * @return Reference to this image for fluent chaining.
     *
     * @code
     * image->SetPositionAligned(
     *     DocumentFormat::OpenXml::Drawing::Wordprocessing::HorizontalRelativePositionValues::Page,
     *     DocumentFormat::OpenXml::Drawing::Wordprocessing::HorizontalAlignmentValues::Center,
     *     DocumentFormat::OpenXml::Drawing::Wordprocessing::VerticalRelativePositionValues::Page,
     *     DocumentFormat::OpenXml::Drawing::Wordprocessing::VerticalAlignmentValues::Top);
     * @endcode
     */
    Image& SetPositionAligned(
        DocumentFormat::OpenXml::Drawing::Wordprocessing::HorizontalRelativePositionValues horizontalFrom,
        DocumentFormat::OpenXml::Drawing::Wordprocessing::HorizontalAlignmentValues horizontalAlign,
        DocumentFormat::OpenXml::Drawing::Wordprocessing::VerticalRelativePositionValues verticalFrom,
        DocumentFormat::OpenXml::Drawing::Wordprocessing::VerticalAlignmentValues verticalAlign);
    /**
     * @brief Reads the current anchor position.
     *
     * @param output Receives the anchor references, offsets, and/or alignments.
     * @return True if position elements are present.
     */
    [[nodiscard]] bool TryGetPosition(ImagePosition& output) const;
    /**
     * @brief Sets text distance using a measurement unit (stored in EMUs).
     *
     * This configures the distance between the image boundary and surrounding text.
     * If the image is inline, this will switch it to floating layout.
     *
     * @param left Distance to text on the left.
     * @param top Distance to text above.
     * @param right Distance to text on the right.
     * @param bottom Distance to text below.
     * @return Reference to this image for fluent chaining.
     */
    Image& SetDistanceFromText(const ExyokiOffice::MeasuringUnits& left,
                               const ExyokiOffice::MeasuringUnits& top,
                               const ExyokiOffice::MeasuringUnits& right,
                               const ExyokiOffice::MeasuringUnits& bottom);
    /**
     * @brief Reads the current distance from surrounding text.
     *
     * @param output Receives distances in EMUs.
     * @return True if the drawing defines distances.
     */
    [[nodiscard]] bool TryGetDistanceFromText(ImageDistanceFromText& output) const;
    /**
     * @brief Places the image behind or in front of text.
     *
     * If the image is inline, this will switch it to floating layout.
     *
     * @param behindText True to place behind text; false to place in front.
     * @return Reference to this image for fluent chaining.
     */
    Image& SetBehindText(bool behindText);
    /**
     * @brief Enables or disables overlap with other floating objects.
     *
     * If the image is inline, this will switch it to floating layout.
     *
     * @param allowOverlap True to allow overlap; false to prevent it.
     * @return Reference to this image for fluent chaining.
     */
    Image& SetAllowOverlap(bool allowOverlap);
    /**
     * @brief Locks or unlocks the anchor.
     *
     * When locked, Word prevents moving the anchor relative to the paragraph.
     * If the image is inline, this will switch it to floating layout.
     *
     * @param locked True to lock the anchor; false to allow moving it.
     * @return Reference to this image for fluent chaining.
     */
    Image& SetAnchorLocked(bool locked);
    /**
     * @brief Sets the anchor Z-order (relative stacking height).
     *
     * Larger values appear in front of smaller ones.
     *
     * @param relativeHeight Relative Z-order value.
     * @return Reference to this image for fluent chaining.
     */
    Image& SetRelativeHeight(UInt32 relativeHeight);
    /**
     * @brief Controls whether the anchor is constrained to the table cell.
     *
     * @param layoutInCell True to constrain to the cell; false to allow
     *        overlap outside the cell.
     * @return Reference to this image for fluent chaining.
     */
    Image& SetLayoutInCell(bool layoutInCell);
    /**
     * @brief Enables or disables simple positioning.
     *
     * When enabled, Word uses the `<wp:simplePos>` coordinates instead of
     * the normal relative positioning elements.
     *
     * @param enabled True to use simple positioning; false to use normal positioning.
     * @return Reference to this image for fluent chaining.
     */
    Image& SetSimplePositionEnabled(bool enabled);
    /**
     * @brief Sets simple positioning coordinates.
     *
     * This updates the `<wp:simplePos>` coordinates and enables simple positioning.
     *
     * @param x X coordinate in EMUs.
     * @param y Y coordinate in EMUs.
     * @return Reference to this image for fluent chaining.
     *
     * @code
     * image->SetSimplePosition(MeasuringUnits(914400, MeasurementUnit::Emu),
     *                          MeasuringUnits(914400, MeasurementUnit::Emu));
     * @endcode
     */
    Image& SetSimplePosition(const ExyokiOffice::MeasuringUnits& x,
                             const ExyokiOffice::MeasuringUnits& y);
    /**
     * @brief Reads anchor-level options for a floating image.
     *
     * @param output Receives anchor options and optional simple position.
     * @return True if an anchor element exists.
     */
    [[nodiscard]] bool TryGetAnchorOptions(ImageAnchorOptions& output) const;

    /**
     * @brief Binds the main document part that owns this image's `<w:drawing>`.
     *
     * Call this when the image was obtained through Paragraph::Images() or
     * Run::Images(), which do not carry part context on their own, and you need
     * SetHyperlink() or a fully resolved TryGetHyperlink() to work. Images created
     * directly by WordDocumentEditor::AddImageFromFile()/AddImageFromData() already
     * have this set.
     *
     * @param mainDocumentPart Owning main document part.
     * @return Reference to this image for fluent chaining.
     */
    Image& AttachMainDocumentPart(const std::shared_ptr<Packaging::MainDocumentPart>& mainDocumentPart);

    /**
     * @brief Crops the picture by the given fraction on each edge.
     *
     * This writes `<a:srcRect>` inside the picture's blip fill. The underlying image
     * payload is never modified; cropping only changes which portion of it is visible.
     *
     * @param left Fraction (0.0-1.0) cropped away from the left edge.
     * @param top Fraction (0.0-1.0) cropped away from the top edge.
     * @param right Fraction (0.0-1.0) cropped away from the right edge.
     * @param bottom Fraction (0.0-1.0) cropped away from the bottom edge.
     * @return Reference to this image for fluent chaining.
     *
     * @code
     * image->SetCrop(0.1, 0.0, 0.1, 0.0); // crop 10% from the left and right
     * @endcode
     */
    Image& SetCrop(Real left, Real top, Real right, Real bottom);
    /**
     * @brief Reads the current crop fractions.
     *
     * @param output Receives crop fractions for each edge.
     * @return True if a `<a:srcRect>` element is present.
     */
    [[nodiscard]] bool TryGetCrop(ImageCrop& output) const;
    /**
     * @brief Removes any crop applied to the picture.
     *
     * @return Reference to this image for fluent chaining.
     */
    Image& ClearCrop();

    /**
     * @brief Sets the accessibility title and description (alt text) for the image.
     *
     * Word stores both values on the picture's non-visual drawing properties
     * (`pic:cNvPr`). Screen readers primarily use the description.
     *
     * @param title Short accessible name shown as the title.
     * @param description Longer accessible description (the conventional "alt text").
     * @return Reference to this image for fluent chaining.
     */
    Image& SetAltText(std::string_view title, std::string_view description);
    /**
     * @brief Reads the accessible title.
     *
     * @return Title text, or an empty string when not set or the image is invalid.
     */
    std::string GetTitle() const;
    /**
     * @brief Reads the accessible description (alt text).
     *
     * @return Description text, or an empty string when not set or the image is invalid.
     */
    std::string GetDescription() const;

    /**
     * @brief Sets the picture rotation.
     *
     * @param degrees Clockwise rotation in degrees. Values outside 0-360 are normalized.
     * @return Reference to this image for fluent chaining.
     */
    Image& SetRotation(Real degrees);
    /**
     * @brief Reads the current picture rotation.
     *
     * @return Clockwise rotation in degrees (0 when no transform is present).
     */
    Real GetRotation() const;
    /**
     * @brief Flips the picture horizontally and/or vertically.
     *
     * @param horizontal True to mirror the picture along the vertical axis.
     * @param vertical True to mirror the picture along the horizontal axis.
     * @return Reference to this image for fluent chaining.
     */
    Image& SetFlip(bool horizontal, bool vertical);
    /**
     * @brief Reads the current flip state.
     *
     * @param horizontal Receives the horizontal flip flag.
     * @param vertical Receives the vertical flip flag.
     * @return True if a shape transform element is present.
     */
    [[nodiscard]] bool TryGetFlip(bool& horizontal, bool& vertical) const;

    /**
     * @brief Attaches a hyperlink that activates when the image is clicked.
     *
     * This creates (or replaces) an external hyperlink relationship on the owning
     * main document part and writes `<a:hlinkClick>` on the picture's non-visual
     * drawing properties. Requires a main document part; see AttachMainDocumentPart().
     *
     * @param url Target URI. Nothing is written when empty.
     * @param newWindow True to request opening the target in a new window/tab.
     * @param tooltip Optional tooltip text shown while hovering over the image.
     * @return Reference to this image for fluent chaining.
     */
    Image& SetHyperlink(std::string_view url, bool newWindow = false, std::string_view tooltip = {});
    /**
     * @brief Reads the current hyperlink.
     *
     * The URL is resolved from the owning main document part's relationships when
     * AttachMainDocumentPart() (or an Add* factory method) has bound one; otherwise
     * Url stays empty while Tooltip/NewWindow are still reported.
     *
     * @param output Receives the hyperlink URL, tooltip, and new-window flag.
     * @return True if a `<a:hlinkClick>` element is present.
     */
    [[nodiscard]] bool TryGetHyperlink(ImageHyperlink& output) const;
    /**
     * @brief Removes the image's hyperlink and its backing relationship, if any.
     *
     * @return Reference to this image for fluent chaining.
     */
    Image& RemoveHyperlink();

private:
    std::shared_ptr<DocumentFormat::OpenXml::Wordprocessing::Drawing> m_drawing;
    std::shared_ptr<Packaging::MainDocumentPart> m_mainDocumentPart;
};

/**
 * @brief Describes one occupied position in a table's logical grid.
 *
 * Word tables are stored as rows of physical `<w:tc>` elements. A single cell
 * can occupy more than one logical column through `w:gridSpan`, and it can
 * occupy more than one row through `w:vMerge`. TableGridCell is the normalized
 * view used by the high-level table API: every visible grid coordinate has one
 * entry, and covered positions point back to the origin cell.
 *
 * `IsOrigin` is true only for the top-left coordinate of the merged region.
 * For unmerged cells, `RowSpan` and `ColumnSpan` are both 1 and the origin
 * coordinates match `Row` and `Column`.
 */
struct TableGridCell
{
    /// Logical row index in the table grid.
    Size Row = 0;
    /// Logical column index in the table grid.
    Size Column = 0;
    /// True when this coordinate owns the physical cell.
    bool IsOrigin = true;
    /// Logical row of the owning cell.
    Size OriginRow = 0;
    /// Logical column of the owning cell.
    Size OriginColumn = 0;
    /// Number of logical rows occupied by the owning cell.
    Size RowSpan = 1;
    /// Number of logical columns occupied by the owning cell.
    Size ColumnSpan = 1;
    /// Underlying `<w:tc>` element for the owning physical cell.
    std::shared_ptr<DocumentFormat::OpenXml::Wordprocessing::TableCell> Cell;
};

/**
 * @brief High-level table wrapper with fluent formatting helpers.
 *
 * This class wraps a `<w:tbl>` element and provides convenience APIs for
 * common table operations: sizing, borders, margins, alignment, cell content,
 * structural row/column edits, merged cells, and nested tables.
 *
 * The wrapper addresses cells by their logical grid coordinate. This matters
 * for merged tables: a cell with `w:gridSpan=3` occupies three logical columns
 * even though the row contains one physical `<w:tc>` element. Methods that edit
 * or merge cells normalize the affected area so high-level operations do not
 * produce conflicting `w:gridSpan`, `w:hMerge`, or `w:vMerge` markup.
 *
 * Typical usage:
 * @code
 * using namespace ExyokiOffice;
 * using namespace ExyokiOffice::Word;
 *
 * auto table = editor->AddTable(3, 2);
 * table->SetWidth(MeasuringUnits(14.0, MeasurementUnit::Centimeter))
 *      .SetAlignment(DocumentFormat::OpenXml::Wordprocessing::TableRowAlignmentValues::Center)
 *      .SetBorders(DocumentFormat::OpenXml::Wordprocessing::BorderValues::Single, 8,
 *                 Color(ColorPreset::Gray));
 *
 * table->SetCellText(0, 0, "Header A");
 * table->SetCellText(0, 1, "Header B");
 * table->SetCellBackgroundColor(0, 0, Color(ColorPreset::LightGray));
 * table->SetCellBackgroundColor(0, 1, Color(ColorPreset::LightGray));
 * @endcode
 */
class EXYOKIOFFICE_EXPORT Table
{
public:
    using Ptr = std::shared_ptr<Table>;
    using TableWidthUnitValues = DocumentFormat::OpenXml::Wordprocessing::TableWidthUnitValues;

    /**
     * @brief Wraps an existing low-level table element.
     *
     * @param table Low-level `<w:tbl>` element to wrap.
     * @param mainDocumentPart Optional main document part, passed on to the paragraphs
     * this table hands out so they reach footnotes, comments, and shared identifiers.
     * @param owningPart Part the table lives in; null means the main document part.
     * Without one, a hyperlink added to a paragraph inside a table cell has nowhere to
     * record its target - which is what Paragraphs() used to hand back.
     */
    explicit Table(const std::shared_ptr<DocumentFormat::OpenXml::Wordprocessing::Table>& table,
                   const std::shared_ptr<Packaging::MainDocumentPart>& mainDocumentPart = nullptr,
                   const std::shared_ptr<OpenXmlPackagePart>& owningPart = nullptr);

    /** @brief Part whose relationships the content of this table reads and writes. */
    [[nodiscard]] std::shared_ptr<OpenXmlPackagePart> OwningPart() const;

    /**
     * @brief Returns the underlying low-level table element.
     *
     * Use this when you need access to properties or child elements not exposed
     * by the high-level wrapper.
     *
     * @return The wrapped element, or nullptr when this wrapper is detached.
     */
    std::shared_ptr<DocumentFormat::OpenXml::Wordprocessing::Table> GetLowLevelApi() const;

    /**
     * @brief Returns the current number of rows.
     *
     * @return Row count (0 if the table is invalid).
     */
    Size GetRowCount() const;
    /**
     * @brief Returns the maximum number of columns across all rows.
     *
     * @return Column count (0 if the table is invalid).
     */
    Size GetColumnCount() const;

    /**
     * @brief Returns the current logical column count.
     *
     * The logical count includes columns covered by horizontal spans. For
     * regular tables this is the same value as GetColumnCount(); for merged
     * tables it reflects the visible grid used by Word.
     *
     * @return Maximum logical column count across all rows.
     */
    Size GetLogicalColumnCount() const;

    /**
     * @brief Builds the logical grid for this table.
     *
     * Each returned row contains one TableGridCell per occupied logical column.
     * Positions covered by a horizontal or vertical merge point to the origin
     * cell through OriginRow and OriginColumn.
     *
     * @return Logical table grid in row order.
     */
    std::vector<std::vector<TableGridCell>> GetLogicalGrid() const;

    /**
     * @brief Enumerates paragraphs contained anywhere inside this table.
     *
     * The returned wrappers include paragraphs in table cells, including nested
     * table content, in document order. They reference the existing DOM and can
     * be edited directly.
     *
     * @return Paragraph wrappers in document order.
     */
    std::vector<std::shared_ptr<Paragraph>> Paragraphs() const;

    /**
     * @brief Enumerates nested tables contained inside this table.
     *
     * The current table itself is not included. Returned wrappers are useful
     * when reading documents that contain tables inside cells.
     *
     * @return Nested table wrappers in document order.
     */
    std::vector<std::shared_ptr<Table>> Tables() const;

    /**
     * @brief Sets table width using a measurement unit (stored in twips, dxa).
     *
     * @param width Table width in any supported unit.
     * @return Reference to this table for fluent chaining.
     */
    Table& SetWidth(const ExyokiOffice::MeasuringUnits& width);
    /**
     * @brief Sets the horizontal alignment of the table.
     *
     * @param alignment Alignment value (left, center, right).
     * @return Reference to this table for fluent chaining.
     */
    Table& SetAlignment(DocumentFormat::OpenXml::Wordprocessing::TableRowAlignmentValues alignment);
    /**
     * @brief Sets table borders on all sides.
     *
     * @param style Border style.
     * @param size Border size (eighths of a point, Word's native unit).
     * @param color Border color value.
     * @return Reference to this table for fluent chaining.
     */
    Table& SetBorders(DocumentFormat::OpenXml::Wordprocessing::BorderValues style,
                      UInt32 size,
                      const ExyokiOffice::Color& color = ExyokiOffice::Color());
    /**
     * @brief Sets table borders using a measurement unit (converted to eighths of a point).
     *
     * @param style Border style.
     * @param size Border size in any supported unit (converted to 1/8 pt).
     * @param color Border color value.
     * @return Reference to this table for fluent chaining.
     */
    Table& SetBorders(DocumentFormat::OpenXml::Wordprocessing::BorderValues style,
                      const ExyokiOffice::MeasuringUnits& size,
                      const ExyokiOffice::Color& color = ExyokiOffice::Color());
    /**
     * @brief Sets default cell margins using a measurement unit (stored in twips, dxa).
     *
     * @param left Left margin.
     * @param top Top margin.
     * @param right Right margin.
     * @param bottom Bottom margin.
     * @return Reference to this table for fluent chaining.
     */
    Table& SetDefaultCellMargins(const ExyokiOffice::MeasuringUnits& left,
                                 const ExyokiOffice::MeasuringUnits& top,
                                 const ExyokiOffice::MeasuringUnits& right,
                                 const ExyokiOffice::MeasuringUnits& bottom);

    /**
     * @brief Appends a new row to the table.
     *
     * If columns is 0, the method uses the current maximum column count.
     *
     * @param columns Number of columns to create in the new row.
     * @return Reference to this table for fluent chaining.
     */
    Table& AddRow(Size columns = 0);
    /**
     * @brief Appends a new column to all existing rows.
     *
     * @return Reference to this table for fluent chaining.
     */
    Table& AddColumn();

    /**
     * @brief Inserts a row at the specified logical row index.
     *
     * The inserted row receives one empty cell for each logical table column.
     * If the index is greater than the current row count, the row is appended.
     * Existing merged regions are normalized before the structural edit.
     *
     * @param row Row index where the new row should be inserted.
     * @return Reference to this table for fluent chaining.
     */
    Table& InsertRow(Size row);

    /**
     * @brief Removes a row from the table.
     *
     * Existing merged regions are normalized before the row is removed, so the
     * table is not left with dangling vertical merge continuations.
     *
     * @param row Row index to remove.
     * @return Reference to this table for fluent chaining.
     */
    Table& RemoveRow(Size row);

    /**
     * @brief Inserts a logical column at the specified index.
     *
     * The inserted column receives one empty cell in each row. If the index is
     * greater than the current logical column count, the column is appended.
     * Existing merged regions are normalized before the structural edit.
     *
     * @param column Column index where the new column should be inserted.
     * @return Reference to this table for fluent chaining.
     */
    Table& InsertColumn(Size column);

    /**
     * @brief Removes a logical column from every row.
     *
     * Existing merged regions are normalized before the column is removed, so
     * the remaining rows keep a consistent physical cell layout.
     *
     * @param column Column index to remove.
     * @return Reference to this table for fluent chaining.
     */
    Table& RemoveColumn(Size column);

    /**
     * @brief Replaces the textual content of a cell.
     *
     * The cell is resolved through the table's logical grid, so the coordinates
     * may point to a position covered by a merged cell. The operation removes
     * existing paragraphs and nested tables from the target physical cell, then
     * creates one paragraph containing one text run. Cell properties (`w:tcPr`),
     * including borders, margins, shading, width, and merge markers, are
     * preserved.
     *
     * Use AppendCellText when you need to add text to the first paragraph
     * without clearing existing content.
     *
     * @param row Zero-based row index.
     * @param column Zero-based column index within the logical grid.
     * @param text Text that should become the complete cell content.
     * @param preserveSpaces Whether to write `xml:space="preserve"` on the
     *        created text node.
     * @return Reference to this table for fluent chaining.
     */
    Table& SetCellText(Size row,
                       Size column,
                       std::string_view text,
                       bool preserveSpaces = true);

    /**
     * @brief Appends text to the first paragraph of a cell.
     *
     * Unlike SetCellText, this operation does not remove existing runs,
     * paragraphs, or nested tables. If the cell does not contain a paragraph, one
     * is created before the new run is inserted. The cell is resolved through
     * logical grid coordinates, so covered positions inside merged cells are
     * supported as well.
     *
     * @param row Zero-based row index.
     * @param column Zero-based column index within the logical grid.
     * @param text Text to append as a new run in the first paragraph.
     * @param preserveSpaces Whether to write `xml:space="preserve"` on the
     *        created text node.
     * @return Reference to this table for fluent chaining.
     */
    Table& AppendCellText(Size row,
                          Size column,
                          std::string_view text,
                          bool preserveSpaces = true);
    /**
     * @brief Sets the background fill color of a cell.
     *
     * @param row Row index (0-based).
     * @param column Column index (0-based).
     * @param color Fill color value.
     * @return Reference to this table for fluent chaining.
     */
    Table& SetCellBackgroundColor(Size row,
                                  Size column,
                                  const ExyokiOffice::Color& color);
    /**
     * @brief Sets cell margins using a measurement unit (stored in twips, dxa).
     *
     * @param row Row index (0-based).
     * @param column Column index (0-based).
     * @param left Left margin.
     * @param top Top margin.
     * @param right Right margin.
     * @param bottom Bottom margin.
     * @return Reference to this table for fluent chaining.
     */
    Table& SetCellMargins(Size row,
                          Size column,
                          const ExyokiOffice::MeasuringUnits& left,
                          const ExyokiOffice::MeasuringUnits& top,
                          const ExyokiOffice::MeasuringUnits& right,
                          const ExyokiOffice::MeasuringUnits& bottom);
    /**
     * @brief Sets the horizontal alignment for content in a cell.
     *
     * This sets paragraph justification on the first paragraph within the cell.
     *
     * @param row Row index (0-based).
     * @param column Column index (0-based).
     * @param alignment Horizontal alignment value.
     * @return Reference to this table for fluent chaining.
     */
    Table& SetCellHorizontalAlignment(Size row,
                                      Size column,
                                      DocumentFormat::OpenXml::Wordprocessing::JustificationValues alignment);
    /**
     * @brief Sets the vertical alignment of the cell content.
     *
     * @param row Row index (0-based).
     * @param column Column index (0-based).
     * @param alignment Vertical alignment value.
     * @return Reference to this table for fluent chaining.
     */
    Table& SetCellVerticalAlignment(Size row,
                                    Size column,
                                    DocumentFormat::OpenXml::Wordprocessing::TableVerticalAlignmentValues alignment);
    /**
     * @brief Sets cell width using a measurement unit (stored in twips, dxa).
     *
     * @param row Row index (0-based).
     * @param column Column index (0-based).
     * @param width Cell width in any supported unit.
     * @return Reference to this table for fluent chaining.
     */
    Table& SetCellWidth(Size row, Size column, const ExyokiOffice::MeasuringUnits& width);
    /**
     * @brief Sets borders for a single table cell.
     *
     * @param row Row index (0-based).
     * @param column Column index (0-based).
     * @param style Border style.
     * @param size Border size (eighths of a point, Word's native unit).
     * @param color Border color value.
     * @return Reference to this table for fluent chaining.
     */
    Table& SetCellBorders(Size row,
                          Size column,
                          DocumentFormat::OpenXml::Wordprocessing::BorderValues style,
                          UInt32 size,
                          const ExyokiOffice::Color& color = ExyokiOffice::Color());
    /**
     * @brief Sets borders for a single table cell using a measurement unit.
     *
     * @param row Row index (0-based).
     * @param column Column index (0-based).
     * @param style Border style.
     * @param size Border size in any supported unit (converted to 1/8 pt).
     * @param color Border color value.
     * @return Reference to this table for fluent chaining.
     */
    Table& SetCellBorders(Size row,
                          Size column,
                          DocumentFormat::OpenXml::Wordprocessing::BorderValues style,
                          const ExyokiOffice::MeasuringUnits& size,
                          const ExyokiOffice::Color& color = ExyokiOffice::Color());
    /**
     * @brief Sets row height and height rule.
     *
     * @param row Row index (0-based).
     * @param height Row height in any supported unit (stored in twips).
     * @param rule Height rule (Auto, AtLeast, Exact).
     * @return Reference to this table for fluent chaining.
     */
    Table& SetRowHeight(Size row,
                        const ExyokiOffice::MeasuringUnits& height,
                        DocumentFormat::OpenXml::Wordprocessing::HeightRuleValues rule =
                            DocumentFormat::OpenXml::Wordprocessing::HeightRuleValues::AtLeast);
    /**
     * @brief Controls whether a row can split across pages.
     *
     * @param row Row index (0-based).
     * @param cantSplit True to prevent splitting; false to allow splitting.
     * @return Reference to this table for fluent chaining.
     */
    Table& SetRowCantSplit(Size row, bool cantSplit);
    /**
     * @brief Marks or unmarks a row as a repeating header row.
     *
     * Word repeats header rows at the top of each page when enabled.
     *
     * @param row Row index (0-based).
     * @param isHeader True to mark as header; false to clear.
     * @return Reference to this table for fluent chaining.
     */
    Table& SetRowHeader(Size row, bool isHeader);
    /**
     * @brief Sets the width of a table column using the table grid.
     *
     * This updates `<w:tblGrid>` for the specified column index.
     *
     * @param column Column index (0-based).
     * @param width Column width in any supported unit (stored in twips).
     * @return Reference to this table for fluent chaining.
     */
    Table& SetColumnWidth(Size column, const ExyokiOffice::MeasuringUnits& width);

    /**
     * @brief Merges a rectangular block of cells.
     *
     * The merge starts at (row, column). A rowSpan or columnSpan of 0 does nothing.
     *
     * @param row Starting row index (0-based).
     * @param column Starting column index (0-based).
     * @param rowSpan Number of rows to merge.
     * @param columnSpan Number of columns to merge.
     * @return Reference to this table for fluent chaining.
     */
    Table& MergeCells(Size row, Size column, Size rowSpan, Size columnSpan);

    /**
     * @brief Splits the cell that occupies the specified logical coordinate.
     *
     * If the coordinate belongs to a merged region, all covered positions are
     * restored as individual cells and merge metadata is removed. Calling this
     * for an unmerged cell is a no-op.
     *
     * @param row Logical row index.
     * @param column Logical column index.
     * @return Reference to this table for fluent chaining.
     */
    Table& SplitCell(Size row, Size column);

    /**
     * @brief Splits all merged cells in the table.
     *
     * This is useful before bulk structural changes or when a consumer needs a
     * rectangular physical table without `w:gridSpan`, `w:hMerge`, or `w:vMerge`.
     *
     * @return Reference to this table for fluent chaining.
     */
    Table& SplitAllCells();

    /**
     * @brief Adds a nested table inside the specified logical cell.
     *
     * The nested table is appended after existing cell content. The method
     * creates the target cell when necessary and ensures Word's required final
     * paragraph remains after the nested table.
     *
     * @param row Logical row index.
     * @param column Logical column index.
     * @param rows Number of rows in the nested table.
     * @param columns Number of columns in the nested table.
     * @return Nested table wrapper, or nullptr when this table is invalid.
     */
    std::shared_ptr<Table> AddNestedTable(Size row,
                                          Size column,
                                          Size rows,
                                          Size columns);

private:
    std::shared_ptr<DocumentFormat::OpenXml::Wordprocessing::Table> m_table;
    std::shared_ptr<Packaging::MainDocumentPart> m_mainDocumentPart;
    std::shared_ptr<OpenXmlPackagePart> m_owningPart;
};

/**
 * @brief High-level wrapper for a Word header or footer part.
 *
 * The wrapper exposes the same paragraph facade used by the main body, while
 * keeping access to the underlying package part for advanced scenarios.
 */
class EXYOKIOFFICE_EXPORT HeaderFooterContent
{
public:
    using Ptr = std::shared_ptr<HeaderFooterContent>;

    /// Creates an invalid wrapper referencing no header or footer part.
    HeaderFooterContent() = default;
    /// Wraps an existing header part. Usually obtained via Section::EnsureHeader().
    explicit HeaderFooterContent(const std::shared_ptr<Packaging::HeaderPart>& part);
    /// Wraps an existing footer part. Usually obtained via Section::EnsureFooter().
    explicit HeaderFooterContent(const std::shared_ptr<Packaging::FooterPart>& part);

    /**
     * @brief Returns true when this wrapper references a valid header part.
     */
    [[nodiscard]] bool IsHeader() const;

    /**
     * @brief Returns true when this wrapper references a valid footer part.
     */
    [[nodiscard]] bool IsFooter() const;

    /**
     * @brief Returns the underlying header part, or nullptr for footer/invalid wrappers.
     */
    std::shared_ptr<Packaging::HeaderPart> GetHeaderPart() const;

    /**
     * @brief Returns the underlying footer part, or nullptr for header/invalid wrappers.
     */
    std::shared_ptr<Packaging::FooterPart> GetFooterPart() const;

    /**
     * @brief Returns the relationship id used by section references.
     */
    std::string RelationshipId() const;

    /**
     * @brief The header or footer part itself, whichever this wrapper holds.
     *
     * Paragraphs handed out here carry it, so a hyperlink added to a header
     * records its target in that header's relationships rather than in the main
     * document's, where Word would not find it.
     */
    [[nodiscard]] std::shared_ptr<OpenXmlPackagePart> OwningPart() const;

    /**
     * @brief Enumerates direct paragraphs in this header or footer.
     */
    std::vector<std::shared_ptr<Paragraph>> Paragraphs() const;

    /**
     * @brief Appends a paragraph to the header or footer.
     *
     * @return The new paragraph, or nullptr when this wrapper is not attached
     * to a header or footer part.
     */
    std::shared_ptr<Paragraph> AddParagraph(std::string_view text = {}, bool preserveSpaces = true);

    /**
     * @brief Concatenates all direct paragraph text.
     */
    std::string PlainText() const;

    /**
     * @brief Removes all direct content from the header/footer root.
     */
    HeaderFooterContent& Clear();

    /**
     * @brief Replaces the content with a single text paragraph.
     */
    HeaderFooterContent& SetText(std::string_view text, bool preserveSpaces = true);

private:
    std::shared_ptr<DocumentFormat::OpenXml::Wordprocessing::Header> HeaderRoot() const;
    std::shared_ptr<DocumentFormat::OpenXml::Wordprocessing::Footer> FooterRoot() const;

    std::shared_ptr<Packaging::HeaderPart> m_headerPart;
    std::shared_ptr<Packaging::FooterPart> m_footerPart;
};

/**
 * @brief High-level read wrapper for section properties (`<w:sectPr>`).
 *
 * A Section wrapper represents an existing section-properties element. It may
 * be the final body-level section properties element or a section break stored
 * inside a paragraph's properties. The wrapper stores the generic Open XML
 * element so it also works when the generated DOM materializes `<w:sectPr>` as
 * a schema-specific variant rather than the common SectionProperties class.
 *
 * @code
 * auto editor = WordDocumentEditor::Open("document.docx");
 * for (const auto& section : editor->Sections())
 * {
 *     if (section->IsFinalBodySection())
 *     {
 *         auto lowLevel = section->GetLowLevelApi();
 *         // Read page size, margins, headers, or other section metadata.
 *     }
 * }
 * @endcode
 */
class EXYOKIOFFICE_EXPORT Section
{
public:
    using Ptr = std::shared_ptr<Section>;

    /**
     * @brief Wraps an existing low-level section properties element.
     *
     * @param sectionProperties Low-level `<w:sectPr>` element to wrap.
     */
    explicit Section(const std::shared_ptr<ExyokiOffice::OpenXMLElement>& sectionProperties,
                     const std::shared_ptr<Packaging::MainDocumentPart>& mainDocumentPart = nullptr);

    /**
     * @brief Returns the underlying low-level section properties element.
     *
     * @return Low-level `<w:sectPr>` element, or nullptr for an invalid wrapper.
     */
    std::shared_ptr<ExyokiOffice::OpenXMLElement> GetLowLevelApi() const;

    /**
     * @brief Checks whether this section properties element is directly under body.
     *
     * Word stores the final document section as a body child. Earlier section
     * breaks are usually stored under paragraph properties.
     *
     * @return True when the wrapped `<w:sectPr>` is a direct child of `<w:body>`.
     */
    [[nodiscard]] bool IsFinalBodySection() const;

    /**
     * @brief Reads the section start type.
     *
     * Missing `w:type` means Word uses its default behavior; in that case this
     * method returns std::nullopt instead of inventing a value.
     *
     * @return Section start type, or std::nullopt when not present.
     */
    std::optional<SectionStartType> GetStartType() const;

    /**
     * @brief Sets how this section begins.
     *
     * @param startType Desired section break behavior.
     * @return This section wrapper for fluent calls.
     */
    Section& SetStartType(SectionStartType startType);

    /**
     * @brief Reads page size and orientation for this section.
     *
     * @return Page size metadata, or std::nullopt when `w:pgSz` is missing or incomplete.
     */
    std::optional<SectionPageSize> GetPageSize() const;

    /**
     * @brief Sets page size and orientation for this section.
     *
     * @param pageSize Width, height, and orientation.
     * @return This section wrapper for fluent calls.
     */
    Section& SetPageSize(const SectionPageSize& pageSize);

    /**
     * @brief Convenience setter for page orientation.
     *
     * If page size is missing, this creates `w:pgSz` with only orientation set.
     *
     * @param orientation Desired page orientation.
     * @return This section wrapper for fluent calls.
     */
    Section& SetPageOrientation(PageOrientation orientation);

    /**
     * @brief Reads page margins for this section.
     *
     * @return Margin metadata, or std::nullopt when `w:pgMar` is missing.
     */
    std::optional<SectionMargins> GetMargins() const;

    /**
     * @brief Sets page margins for this section.
     *
     * @param margins Page, header, footer, and gutter margins.
     * @return This section wrapper for fluent calls.
     */
    Section& SetMargins(const SectionMargins& margins);

    /**
     * @brief Reads equal-width column settings for this section.
     *
     * @return Column settings, or std::nullopt when `w:cols` is missing.
     */
    std::optional<SectionColumns> GetColumns() const;

    /**
     * @brief Sets equal-width column settings for this section.
     *
     * @param columns Column count, spacing, and separator-line flag.
     * @return This section wrapper for fluent calls.
     */
    Section& SetColumns(const SectionColumns& columns);

    /**
     * @brief Returns true when this section has an explicit header reference.
     */
    [[nodiscard]] bool HasHeader(HeaderFooterType type = HeaderFooterType::Default) const;

    /**
     * @brief Returns true when this section has an explicit footer reference.
     */
    [[nodiscard]] bool HasFooter(HeaderFooterType type = HeaderFooterType::Default) const;

    /**
     * @brief Reads an explicitly referenced header part.
     *
     * Missing references return nullptr. In WordprocessingML that means the
     * section is linked to the previous section for that header type.
     */
    std::shared_ptr<HeaderFooterContent> GetHeader(
        HeaderFooterType type = HeaderFooterType::Default) const;

    /**
     * @brief Reads an explicitly referenced footer part.
     *
     * Missing references return nullptr. In WordprocessingML that means the
     * section is linked to the previous section for that footer type.
     */
    std::shared_ptr<HeaderFooterContent> GetFooter(
        HeaderFooterType type = HeaderFooterType::Default) const;

    /**
     * @brief Creates or returns the explicit header reference for this section.
     *
     * @return The header content, or nullptr when this section wrapper is
     * detached or the header part could not be created.
     */
    std::shared_ptr<HeaderFooterContent> EnsureHeader(
        HeaderFooterType type = HeaderFooterType::Default);

    /**
     * @brief Creates or returns the explicit footer reference for this section.
     *
     * @return The footer content, or nullptr when this section wrapper is
     * detached or the footer part could not be created.
     */
    std::shared_ptr<HeaderFooterContent> EnsureFooter(
        HeaderFooterType type = HeaderFooterType::Default);

    /**
     * @brief Replaces the explicit header content with a single text paragraph.
     */
    Section& SetHeaderText(HeaderFooterType type, std::string_view text, bool preserveSpaces = true);

    /**
     * @brief Replaces the explicit footer content with a single text paragraph.
     */
    Section& SetFooterText(HeaderFooterType type, std::string_view text, bool preserveSpaces = true);

    /**
     * @brief Removes this section's explicit header reference.
     *
     * When no other section references the same header part, the part and its
     * relationship are removed from the main document part.
     */
    bool RemoveHeader(HeaderFooterType type = HeaderFooterType::Default);

    /**
     * @brief Removes this section's explicit footer reference and cleans up unused parts.
     */
    bool RemoveFooter(HeaderFooterType type = HeaderFooterType::Default);

    /**
     * @brief Returns true when this section inherits the header from the previous section.
     */
    [[nodiscard]] bool IsHeaderLinkedToPrevious(HeaderFooterType type = HeaderFooterType::Default) const;

    /**
     * @brief Returns true when this section inherits the footer from the previous section.
     */
    [[nodiscard]] bool IsFooterLinkedToPrevious(HeaderFooterType type = HeaderFooterType::Default) const;

    /**
     * @brief Makes this section inherit the header from the previous section.
     */
    bool LinkHeaderToPrevious(HeaderFooterType type = HeaderFooterType::Default);

    /**
     * @brief Makes this section inherit the footer from the previous section.
     */
    bool LinkFooterToPrevious(HeaderFooterType type = HeaderFooterType::Default);

private:
    std::shared_ptr<ExyokiOffice::OpenXMLElement> m_sectionProperties;
    std::shared_ptr<Packaging::MainDocumentPart> m_mainDocumentPart;
};

/**
 * @brief Read wrapper for one top-level child of the main document body.
 *
 * BodyBlock preserves body order while giving callers typed access to the
 * high-level wrappers for supported block kinds. Unsupported elements remain
 * available through GetLowLevelApi so applications can decide whether to keep,
 * inspect, or ignore them.
 */
class EXYOKIOFFICE_EXPORT BodyBlock
{
public:
    /**
     * @brief Creates an unsupported/empty body block wrapper.
     */
    BodyBlock() = default;

    /**
     * @brief Creates a typed block wrapper around a low-level body child.
     *
     * @param lowLevel Existing body child element.
     * @param mainDocumentPart Optional owning main document part, forwarded to the
     * Paragraph/ContentControl wrapper created for this block, when applicable.
     */
    explicit BodyBlock(const std::shared_ptr<ExyokiOffice::OpenXMLElement>& lowLevel,
                       const std::shared_ptr<Packaging::MainDocumentPart>& mainDocumentPart = nullptr);

    /**
     * @brief Returns the detected block type.
     *
     * @return Paragraph, Table, Section, ContentControl, or Unsupported.
     */
    BodyBlockType Type() const;

    /**
     * @brief Returns the underlying low-level element.
     *
     * @return Low-level body child element, or nullptr for an empty wrapper.
     */
    std::shared_ptr<ExyokiOffice::OpenXMLElement> GetLowLevelApi() const;

    /**
     * @brief Returns this block as a paragraph when possible.
     *
     * @return Paragraph wrapper, or nullptr when Type() is not Paragraph.
     */
    std::shared_ptr<Paragraph> AsParagraph() const;

    /**
     * @brief Returns this block as a table when possible.
     *
     * @return Table wrapper, or nullptr when Type() is not Table.
     */
    std::shared_ptr<Table> AsTable() const;

    /**
     * @brief Returns this block as section properties when possible.
     *
     * @return Section wrapper, or nullptr when Type() is not Section.
     */
    std::shared_ptr<Section> AsSection() const;

    /**
     * @brief Returns this block as a content control when possible.
     *
     * @return Content control wrapper, or nullptr when Type() is not ContentControl.
     */
    std::shared_ptr<ContentControl> AsContentControl() const;

private:
    BodyBlockType m_type = BodyBlockType::Unsupported;
    std::shared_ptr<ExyokiOffice::OpenXMLElement> m_lowLevel;
    std::shared_ptr<Paragraph> m_paragraph;
    std::shared_ptr<Table> m_table;
    std::shared_ptr<Section> m_section;
    std::shared_ptr<ContentControl> m_contentControl;
};

} // namespace ExyokiOffice::Word
