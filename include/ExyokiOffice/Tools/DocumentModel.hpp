// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

#include "ExyokiOffice/Export.hpp"
#include "ExyokiOffice/Tools/PackageModel.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ExyokiOffice::Tools
{

/// Version of the semantic document model written to the JSON/XML envelope.
inline constexpr int DocumentModelVersion = 1;

/**
 * @brief One media payload (image, ...) referenced from document content.
 *
 * While a model is held in memory, Data carries the payload bytes. When the
 * model is serialized with external media, the bytes are written to a media
 * directory and FileName records the reference stored in the output; with
 * embedded media the bytes are base64-encoded into the envelope instead.
 */
struct EXYOKIOFFICE_EXPORT MediaReference
{
    /// Stable identifier referenced by content ("media1", "media2", ...).
    std::string Id;
    /// Relative file reference used by external-media serialization (e.g. "report_media/image1.png").
    std::string FileName;
    /// MIME content type (e.g. "image/png").
    std::string ContentType;
    /// Payload bytes.
    std::vector<Byte> Data;
};

// ---------------------------------------------------------------------------
// Word
// ---------------------------------------------------------------------------

/// Character formatting captured for one text inline. Empty/false fields mean "not set".
struct EXYOKIOFFICE_EXPORT WordRunProps
{
    bool Bold = false;
    bool Italic = false;
    bool Underline = false;
    bool Strike = false;
    bool Caps = false;
    bool SmallCaps = false;
    /// Run color as #RRGGBB, empty when unset.
    std::string Color;
    /// Highlight color token ("yellow", "cyan", ...), empty when unset.
    std::string Highlight;
    /// ASCII font family, empty when unset.
    std::string Font;
    /// Character style ID, empty when unset.
    std::string StyleId;
    /// Font size in points, 0 when unset.
    Real FontSizePt = 0.0;
};

/**
 * @brief One inline element inside a Word paragraph, in document order.
 *
 * A flat tagged struct: Kind selects which fields are meaningful.
 */
struct EXYOKIOFFICE_EXPORT WordInline
{
    enum class Type
    {
        Text,        ///< Text with Props applied.
        Hyperlink,   ///< Target/Anchor/Tooltip + Children (the link text inlines).
        Image,       ///< MediaId/AltText/WidthEmu/HeightEmu.
        FootnoteRef, ///< NoteId.
        EndnoteRef,  ///< NoteId.
        CommentRef,  ///< NoteId (comment ID).
        Field,       ///< FieldCode + Text (cached result).
        Break        ///< BreakKind ("line", "page", "column").
    };

    Type Kind = Type::Text;

    /// Text content (Text) or cached field result (Field).
    std::string Text;
    /// Formatting of a Text inline.
    WordRunProps Props;

    /// External hyperlink URL, empty for internal links.
    std::string Target;
    /// Internal hyperlink bookmark anchor, empty for external links.
    std::string Anchor;
    /// Hyperlink tooltip.
    std::string Tooltip;
    /// Hyperlink content inlines.
    std::vector<WordInline> Children;

    /// Media ID referencing DocumentModel::Media (Image).
    std::string MediaId;
    /// Image alternative text.
    std::string AltText;
    /// Image extent in EMU (0 = unknown).
    Int64 WidthEmu = 0;
    Int64 HeightEmu = 0;

    /// Footnote/endnote/comment ID referencing the model's note collections.
    int NoteId = 0;

    /// Field instruction (e.g. "PAGE").
    std::string FieldCode;

    /// Break kind: "line", "page", or "column".
    std::string BreakKind;
};

/// Reference from a paragraph to a numbering (list) definition instance.
struct EXYOKIOFFICE_EXPORT WordListRef
{
    int NumberingId = 0;
    int Level = 0;
};

struct WordBlock;

/// One paragraph: style/format metadata plus ordered inline content.
struct EXYOKIOFFICE_EXPORT WordParagraph
{
    /// Paragraph style ID, empty when unset.
    std::string StyleId;
    /// Heading level 1-9 derived from the style ID, 0 for non-headings.
    int HeadingLevel = 0;
    /// Justification token ("left", "center", "right", "both", ...), empty when unset.
    std::string Alignment;
    /// Numbering reference when the paragraph is a list item.
    std::optional<WordListRef> List;
    std::vector<WordInline> Inlines;
};

/// One logical-grid table cell. Covered cells are placeholders under a span origin.
///
/// Word content nests (block -> table -> row -> cell -> block), and the cycle is
/// broken through std::vector, which accepts an incomplete element type. Marking
/// the cell - or any owner of one - as exported would define its implicit copy
/// and destroy members right here, where WordBlock is still incomplete, so the
/// four structs forming the cycle stay unexported. They are header-only
/// aggregates, so nothing has to be linked from the library anyway.
struct WordTableCell
{
    int RowSpan = 1;
    int ColSpan = 1;
    /// True when this grid position is covered by another cell's span.
    bool Covered = false;
    /// Cell content (paragraphs and nested tables).
    std::vector<WordBlock> Blocks;
};

struct WordTableRow
{
    std::vector<WordTableCell> Cells;
};

struct WordTable
{
    /// Table style ID, empty when unset.
    std::string StyleId;
    std::vector<WordTableRow> Rows;
};

/// One top-level (or cell-level) block of Word content.
struct EXYOKIOFFICE_EXPORT WordBlock
{
    enum class Type
    {
        Paragraph,
        Table,
        SectionBreak
    };

    Type Kind = Type::Paragraph;
    std::optional<WordParagraph> Paragraph;
    std::optional<WordTable> Table;
};

/// One level of a numbering definition.
struct EXYOKIOFFICE_EXPORT WordListLevel
{
    /// Numbering format token ("decimal", "bullet", "lowerLetter", "upperRoman", ...).
    std::string Format = "decimal";
    /// Word level text pattern such as "%1." (or the bullet glyph for bullet levels).
    std::string LevelText;
    int Start = 1;
};

/// One numbering instance with its resolved level definitions.
struct EXYOKIOFFICE_EXPORT WordListDefinition
{
    /// Numbering instance ID referenced by WordListRef::NumberingId.
    int NumberingId = 0;
    std::vector<WordListLevel> Levels;
};

/// One footnote or endnote entry.
struct EXYOKIOFFICE_EXPORT WordNote
{
    int Id = 0;
    std::vector<WordBlock> Blocks;
};

/// One comment entry.
struct EXYOKIOFFICE_EXPORT WordComment
{
    int Id = 0;
    std::string Author;
    std::string Initials;
    /// ISO-8601 timestamp, empty when unset.
    std::string Date;
    std::vector<WordBlock> Blocks;
};

/// Content of one header or footer.
struct EXYOKIOFFICE_EXPORT WordHeaderFooter
{
    /// "default", "first", or "even".
    std::string Kind = "default";
    std::vector<WordBlock> Blocks;
};

/// Semantic model of one Word document.
struct EXYOKIOFFICE_EXPORT WordDocumentModel
{
    std::vector<WordBlock> Body;
    /// Numbering definitions referenced by paragraphs via WordListRef.
    std::vector<WordListDefinition> Lists;
    std::vector<WordNote> Footnotes;
    std::vector<WordNote> Endnotes;
    std::vector<WordComment> Comments;
    std::vector<WordHeaderFooter> Headers;
    std::vector<WordHeaderFooter> Footers;
};

// ---------------------------------------------------------------------------
// Excel
// ---------------------------------------------------------------------------

/// One stored (non-empty) worksheet cell.
struct EXYOKIOFFICE_EXPORT ExcelCellModel
{
    /// A1-style cell address ("B2").
    std::string Address;
    /// Value type: "string", "number", "bool", "error", "datetime", or "formula".
    std::string Type;
    /// Canonical value text. For formulas this is empty; see CachedValue.
    std::string Value;
    /// Formula text without the leading '=' (formula cells only).
    std::string Formula;
    /// Cached formula result type/value, when the package stores one.
    std::string CachedType;
    std::string CachedValue;
};

/// One worksheet table (ListObject).
struct EXYOKIOFFICE_EXPORT ExcelTableModel
{
    std::string Name;
    /// A1-style range ("A1:C4").
    std::string Range;
};

struct EXYOKIOFFICE_EXPORT ExcelHyperlinkModel
{
    /// A1-style anchor cell.
    std::string Cell;
    std::string Target;
    std::string Tooltip;
};

struct EXYOKIOFFICE_EXPORT ExcelSheetModel
{
    std::string Name;
    /// Sparse stored cells in row-major order.
    std::vector<ExcelCellModel> Cells;
    /// Merged ranges in A1 form ("A1:B2").
    std::vector<std::string> Merges;
    std::vector<ExcelTableModel> Tables;
    std::vector<ExcelHyperlinkModel> Hyperlinks;
};

/// Semantic model of one Excel workbook.
struct EXYOKIOFFICE_EXPORT ExcelWorkbookModel
{
    std::vector<ExcelSheetModel> Sheets;
};

// ---------------------------------------------------------------------------
// PowerPoint
// ---------------------------------------------------------------------------

/// One formatted text run inside a slide text frame.
struct EXYOKIOFFICE_EXPORT PptRun
{
    std::string Text;
    bool Bold = false;
    bool Italic = false;
    bool Underline = false;
    /// Font size in points, 0 when unset.
    Real FontSizePt = 0.0;
    /// Run color as #RRGGBB, empty when unset.
    std::string Color;
    /// Hyperlink URL, empty when unset.
    std::string Hyperlink;
};

struct EXYOKIOFFICE_EXPORT PptParagraph
{
    /// Indent/outline level (0-8).
    int Level = 0;
    std::vector<PptRun> Runs;
};

struct EXYOKIOFFICE_EXPORT PptTextFrame
{
    std::vector<PptParagraph> Paragraphs;
};

struct EXYOKIOFFICE_EXPORT PptTableCell
{
    PptTextFrame Text;
    int RowSpan = 1;
    int ColSpan = 1;
    /// True when this grid position is covered by another cell's span.
    bool Covered = false;
};

struct EXYOKIOFFICE_EXPORT PptTableModel
{
    std::vector<std::vector<PptTableCell>> Rows;
};

/// Shape placement in EMU. Present is false when the shape has no explicit transform.
struct EXYOKIOFFICE_EXPORT PptTransform
{
    Int64 X = 0;
    Int64 Y = 0;
    Int64 Cx = 0;
    Int64 Cy = 0;
    bool Present = false;
};

/// One slide shape (recursive for groups).
struct EXYOKIOFFICE_EXPORT PptShape
{
    enum class Type
    {
        TextBox,
        Placeholder,
        Picture,
        Table,
        Group,
        Other
    };

    Type Kind = Type::TextBox;
    std::string Name;
    /// Placeholder type token ("title", "body", "subTitle", ...) for placeholder shapes.
    std::string PlaceholderType;
    std::optional<PptTextFrame> Text;
    std::optional<PptTableModel> Table;
    /// Media ID referencing DocumentModel::Media (Picture).
    std::string MediaId;
    std::string AltText;
    PptTransform Transform;
    /// Child shapes of a group.
    std::vector<PptShape> Children;
};

struct EXYOKIOFFICE_EXPORT PptCommentModel
{
    std::string Author;
    std::string Text;
};

struct EXYOKIOFFICE_EXPORT PptSlide
{
    /// Layout display name, empty when unknown.
    std::string LayoutName;
    bool Hidden = false;
    std::vector<PptShape> Shapes;
    std::string NotesText;
    std::vector<PptCommentModel> Comments;
};

/// Semantic model of one PowerPoint presentation.
struct EXYOKIOFFICE_EXPORT PowerPointDeckModel
{
    std::vector<PptSlide> Slides;
};

// ---------------------------------------------------------------------------
// Envelope
// ---------------------------------------------------------------------------

/**
 * @brief Family-neutral semantic document model.
 *
 * Exactly one of Word/Excel/PowerPoint is engaged, matching Family. This is
 * the intermediate representation shared by every `exyoki convert` format:
 * JSON and semantic XML serialize it canonically, Markdown and plain text
 * render it lossily.
 */
struct EXYOKIOFFICE_EXPORT DocumentModel
{
    DocumentFamily Family = DocumentFamily::Unknown;
    std::optional<WordDocumentModel> Word;
    std::optional<ExcelWorkbookModel> Excel;
    std::optional<PowerPointDeckModel> PowerPoint;
    std::vector<MediaReference> Media;
    CoreProperties Properties;
};

} // namespace ExyokiOffice::Tools
