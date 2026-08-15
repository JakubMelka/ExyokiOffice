// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "ExyokiOffice/Tools/DocumentModelSchema.hpp"

#include "ExyokiOffice/Tools/DocumentModel.hpp"

#include <nlohmann/json.hpp>
#include <nlohmann/json-schema.hpp>

#include <exception>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

namespace ExyokiOffice::Tools
{

/**
 * @brief Small vocabulary for spelling out draft-07 subschemas.
 *
 * Every node the factory below builds goes through these helpers, so the
 * emitted schema keeps one shape and one key order throughout - the published
 * artifact under docs/schemas/ has to be byte-stable to be diffable.
 */
class DocumentModelSchemaNode
{
public:
    using Json = nlohmann::ordered_json;

    static Json Scalar(const char* type, const char* description)
    {
        Json node = Json::object();
        node["type"] = type;
        node["description"] = description;
        return node;
    }

    static Json String(const char* description) { return Scalar("string", description); }
    static Json Boolean(const char* description) { return Scalar("boolean", description); }
    static Json Number(const char* description) { return Scalar("number", description); }
    static Json Integer(const char* description) { return Scalar("integer", description); }

    /// Integer restricted to [minimum, maximum].
    static Json Integer(const char* description, int minimum, int maximum)
    {
        Json node = Integer(description);
        node["minimum"] = minimum;
        node["maximum"] = maximum;
        return node;
    }

    static Json Constant(const char* value, const char* description)
    {
        Json node = Json::object();
        node["const"] = value;
        node["description"] = description;
        return node;
    }

    static Json Constant(int value, const char* description)
    {
        Json node = Json::object();
        node["const"] = value;
        node["description"] = description;
        return node;
    }

    static Json Enumeration(std::initializer_list<const char*> values, const char* description)
    {
        Json node = Json::object();
        node["type"] = "string";
        node["description"] = description;
        Json tokens = Json::array();
        for (const char* value : values)
        {
            tokens.push_back(value);
        }
        node["enum"] = std::move(tokens);
        return node;
    }

    static Json ArrayOf(Json items, const char* description)
    {
        Json node = Json::object();
        node["type"] = "array";
        node["description"] = description;
        node["items"] = std::move(items);
        return node;
    }

    static Json Ref(const char* definition)
    {
        Json node = Json::object();
        node["$ref"] = std::string("#/definitions/") + definition;
        return node;
    }

    static Json RefArray(const char* definition, const char* description)
    {
        return ArrayOf(Ref(definition), description);
    }

    /**
     * @brief A closed object.
     *
     * `additionalProperties: false` is what turns the schema into a drift
     * detector: a key the serializer starts writing without being described
     * here fails the conformance suite instead of passing unnoticed.
     */
    static Json Object(const char* description, std::initializer_list<const char*> required, Json properties)
    {
        Json node = Json::object();
        node["type"] = "object";
        node["description"] = description;
        Json names = Json::array();
        for (const char* name : required)
        {
            names.push_back(name);
        }
        if (!names.empty())
        {
            node["required"] = std::move(names);
        }
        node["additionalProperties"] = false;
        node["properties"] = std::move(properties);
        return node;
    }

    /**
     * @brief A tagged union: one closed object per `type` token.
     *
     * Spelled as `if`/`then` on the discriminator rather than `oneOf`, because
     * the diagnostics are the product here: `oneOf` reports every branch that
     * failed, so one stray key on a paragraph produces a page of noise about
     * tables and section breaks. With `if`/`then` only the branch the document
     * actually selected has anything to say.
     */
    static Json Tagged(const char* description,
                       std::initializer_list<std::pair<const char*, const char*>> variants)
    {
        Json tokens = Json::array();
        Json branches = Json::array();
        for (const auto& [token, definition] : variants)
        {
            tokens.push_back(token);

            Json expected = Json::object();
            expected["const"] = token;

            Json condition = Json::object();
            condition["properties"] = Json::object({{"type", std::move(expected)}});
            condition["required"] = Json::array({"type"});

            Json branch = Json::object();
            branch["if"] = std::move(condition);
            branch["then"] = Ref(definition);
            branches.push_back(std::move(branch));
        }

        Json discriminator = Json::object();
        discriminator["type"] = "string";
        discriminator["description"] = "Discriminator selecting one of the variants below.";
        discriminator["enum"] = std::move(tokens);

        Json node = Json::object();
        node["type"] = "object";
        node["description"] = description;
        node["required"] = Json::array({"type"});
        node["properties"] = Json::object({{"type", std::move(discriminator)}});
        node["allOf"] = std::move(branches);
        return node;
    }
};

/// Builds the definition set. One method per node of the envelope tree, in the
/// order the serializer in DocumentModelJson.cpp writes them.
class DocumentModelSchemaFactory
{
public:
    using Json = DocumentModelSchemaNode::Json;
    using Node = DocumentModelSchemaNode;

    static Json BuildRoot()
    {
        Json root = Json::object();
        root["$schema"] = "http://json-schema.org/draft-07/schema#";
        root["$id"] = GetDocumentModelJsonSchemaFileName();
        root["title"] = "ExyokiOffice semantic document envelope";
        root["description"] =
            "Canonical JSON serialization of the ExyokiOffice semantic document model, produced and "
            "consumed by `exyoki convert` and by ExyokiOffice::Tools::SerializeModelJson / "
            "ParseModelJson. The prose specification is docs/tools/conversion-formats.md.";
        root["type"] = "object";
        root["required"] = Json::array({"format", "version", "family", "document"});
        root["additionalProperties"] = false;

        Json properties = Json::object();
        properties["format"] = Node::Constant("exyokioffice-document", "Envelope format identifier.");
        properties["version"] =
            Node::Constant(DocumentModelVersion, "Model version this schema describes.");
        properties["family"] = Node::Enumeration({"word", "excel", "powerpoint"},
                                                 "Document family; selects the shape of `document`.");
        properties["properties"] = Node::Ref("coreProperties");
        properties["media"] = Node::RefArray("mediaItem", "Media payloads referenced from content by id.");
        properties["document"] = BuildDocumentPlaceholder();
        root["properties"] = std::move(properties);

        root["allOf"] = BuildFamilySelector();
        root["definitions"] = BuildDefinitions();
        return root;
    }

private:
    /// `document` is family-specific; the root only states that it is an object
    /// and the allOf below narrows it once `family` is known.
    static Json BuildDocumentPlaceholder()
    {
        Json node = Json::object();
        node["type"] = "object";
        node["description"] = "Family-specific payload; see the allOf branches and the family definitions.";
        return node;
    }

    static Json BuildFamilySelector()
    {
        Json branches = Json::array();
        branches.push_back(FamilyBranch("word", "wordDocument"));
        branches.push_back(FamilyBranch("excel", "excelDocument"));
        branches.push_back(FamilyBranch("powerpoint", "powerPointDocument"));
        return branches;
    }

    static Json FamilyBranch(const char* family, const char* definition)
    {
        Json familyConst = Json::object();
        familyConst["const"] = family;

        Json condition = Json::object();
        condition["properties"] = Json::object({{"family", std::move(familyConst)}});
        condition["required"] = Json::array({"family"});

        Json consequence = Json::object();
        consequence["properties"] = Json::object({{"document", Node::Ref(definition)}});

        Json branch = Json::object();
        branch["if"] = std::move(condition);
        branch["then"] = std::move(consequence);
        return branch;
    }

    static Json BuildDefinitions()
    {
        Json definitions = Json::object();

        // --- Envelope ------------------------------------------------------
        definitions["coreProperties"] = BuildCoreProperties();
        definitions["mediaItem"] = BuildMediaItem();

        // --- Word ----------------------------------------------------------
        definitions["wordDocument"] = BuildWordDocument();
        definitions["wordBlock"] = Node::Tagged("One block of Word content, tagged by `type`.",
                                                {{"paragraph", "wordParagraphBlock"},
                                                 {"table", "wordTableBlock"},
                                                 {"sectionBreak", "wordSectionBreakBlock"}});
        definitions["wordParagraphBlock"] = BuildWordParagraphBlock();
        definitions["wordListReference"] = BuildWordListReference();
        definitions["wordTableBlock"] = BuildWordTableBlock();
        definitions["wordTableRow"] = BuildWordTableRow();
        definitions["wordTableCell"] = BuildWordTableCell();
        definitions["wordSectionBreakBlock"] = BuildWordSectionBreakBlock();
        definitions["wordInline"] = Node::Tagged("One inline element of a Word paragraph, tagged by `type`.",
                                                 {{"text", "wordTextInline"},
                                                  {"link", "wordLinkInline"},
                                                  {"image", "wordImageInline"},
                                                  {"footnoteRef", "wordFootnoteRefInline"},
                                                  {"endnoteRef", "wordEndnoteRefInline"},
                                                  {"commentRef", "wordCommentRefInline"},
                                                  {"field", "wordFieldInline"},
                                                  {"break", "wordBreakInline"}});
        definitions["wordTextInline"] = BuildWordTextInline();
        definitions["wordLinkInline"] = BuildWordLinkInline();
        definitions["wordImageInline"] = BuildWordImageInline();
        definitions["wordFootnoteRefInline"] =
            BuildWordNoteRefInline("footnoteRef", "Reference to an entry of `footnotes`.");
        definitions["wordEndnoteRefInline"] =
            BuildWordNoteRefInline("endnoteRef", "Reference to an entry of `endnotes`.");
        definitions["wordCommentRefInline"] =
            BuildWordNoteRefInline("commentRef", "Reference to an entry of `comments`.");
        definitions["wordFieldInline"] = BuildWordFieldInline();
        definitions["wordBreakInline"] = BuildWordBreakInline();
        definitions["wordListDefinition"] = BuildWordListDefinition();
        definitions["wordListLevel"] = BuildWordListLevel();
        definitions["wordNote"] = BuildWordNote();
        definitions["wordComment"] = BuildWordComment();
        definitions["wordHeaderFooter"] = BuildWordHeaderFooter();

        // --- Excel ---------------------------------------------------------
        definitions["excelDocument"] = BuildExcelDocument();
        definitions["excelSheet"] = BuildExcelSheet();
        definitions["excelCell"] = BuildExcelCell();
        definitions["excelCachedValue"] = BuildExcelCachedValue();
        definitions["excelTable"] = BuildExcelTable();
        definitions["excelHyperlink"] = BuildExcelHyperlink();

        // --- PowerPoint ----------------------------------------------------
        definitions["powerPointDocument"] = BuildPowerPointDocument();
        definitions["powerPointSlide"] = BuildPowerPointSlide();
        definitions["powerPointShape"] = BuildPowerPointShape();
        definitions["powerPointTransform"] = BuildPowerPointTransform();
        definitions["powerPointTextFrame"] = BuildPowerPointTextFrame();
        definitions["powerPointParagraph"] = BuildPowerPointParagraph();
        definitions["powerPointRun"] = BuildPowerPointRun();
        definitions["powerPointTableRow"] = BuildPowerPointTableRow();
        definitions["powerPointTableCell"] = BuildPowerPointTableCell();
        definitions["powerPointComment"] = BuildPowerPointComment();
        return definitions;
    }

    // --- Envelope ----------------------------------------------------------

    static Json BuildCoreProperties()
    {
        Json properties = Json::object();
        properties["title"] = Node::String("dc:title.");
        properties["subject"] = Node::String("dc:subject.");
        properties["creator"] = Node::String("dc:creator.");
        properties["keywords"] = Node::String("cp:keywords.");
        properties["description"] = Node::String("dc:description.");
        properties["lastModifiedBy"] = Node::String("cp:lastModifiedBy.");
        properties["category"] = Node::String("cp:category.");
        properties["created"] = Node::String("dcterms:created, ISO-8601.");
        properties["modified"] = Node::String("dcterms:modified, ISO-8601.");
        properties["application"] = Node::String("Extended property Application.");
        properties["company"] = Node::String("Extended property Company.");
        return Node::Object("OPC core and extended properties; empty values are omitted.", {},
                            std::move(properties));
    }

    static Json BuildMediaItem()
    {
        Json properties = Json::object();
        properties["id"] = Node::String("Identifier content refers to (\"media1\", \"media2\", ...).");
        properties["file"] =
            Node::String("Path relative to the envelope file, written when media stays external.");
        properties["contentType"] = Node::String("MIME type, e.g. \"image/png\".");
        properties["data"] = Node::String("Base64 payload, written by --embed-media instead of `file`.");
        return Node::Object("One media payload referenced from document content.", {"id"},
                            std::move(properties));
    }

    // --- Word --------------------------------------------------------------

    static Json BuildWordDocument()
    {
        Json properties = Json::object();
        properties["body"] = Node::RefArray("wordBlock", "Top-level document content in reading order.");
        properties["lists"] =
            Node::RefArray("wordListDefinition", "Numbering definitions referenced by paragraphs.");
        properties["footnotes"] = Node::RefArray("wordNote", "Footnote entries referenced by footnoteRef.");
        properties["endnotes"] = Node::RefArray("wordNote", "Endnote entries referenced by endnoteRef.");
        properties["comments"] = Node::RefArray("wordComment", "Comment entries referenced by commentRef.");
        properties["headers"] = Node::RefArray("wordHeaderFooter", "Header content per kind.");
        properties["footers"] = Node::RefArray("wordHeaderFooter", "Footer content per kind.");
        return Node::Object("Word payload of the envelope.", {"body"}, std::move(properties));
    }

    static Json BuildWordParagraphBlock()
    {
        Json properties = Json::object();
        properties["type"] = Node::Constant("paragraph", "Block discriminator.");
        properties["style"] = Node::String("Paragraph style ID.");
        properties["heading"] =
            Node::Integer("Heading level derived from a Heading<N> style.", 1, 9);
        properties["align"] = Node::String("Justification token (left, center, right, both, ...).");
        properties["list"] = Node::Ref("wordListReference");
        properties["content"] = Node::RefArray("wordInline", "Inline content in reading order.");
        return Node::Object("A paragraph.", {"type", "content"}, std::move(properties));
    }

    static Json BuildWordListReference()
    {
        Json properties = Json::object();
        properties["id"] = Node::Integer("Numbering instance ID; matches an entry of `lists`.");
        properties["level"] = Node::Integer("Zero-based list level; omitted when 0.");
        return Node::Object("Numbering reference of a list paragraph.", {"id"}, std::move(properties));
    }

    static Json BuildWordTableBlock()
    {
        Json properties = Json::object();
        properties["type"] = Node::Constant("table", "Block discriminator.");
        properties["style"] = Node::String("Table style ID.");
        properties["rows"] = Node::RefArray("wordTableRow", "Table rows in document order.");
        return Node::Object("A table.", {"type", "rows"}, std::move(properties));
    }

    static Json BuildWordTableRow()
    {
        Json properties = Json::object();
        properties["cells"] = Node::RefArray("wordTableCell", "Logical-grid cells, left to right.");
        return Node::Object("One table row.", {"cells"}, std::move(properties));
    }

    static Json BuildWordTableCell()
    {
        Json properties = Json::object();
        properties["rowSpan"] = Node::Integer("Vertical span; omitted when 1.");
        properties["colSpan"] = Node::Integer("Horizontal span; omitted when 1.");
        properties["covered"] =
            Node::Boolean("True for a grid position covered by another cell's span; then no content.");
        properties["blocks"] = Node::RefArray("wordBlock", "Cell content; may nest further tables.");
        return Node::Object("One logical-grid table cell.", {}, std::move(properties));
    }

    static Json BuildWordSectionBreakBlock()
    {
        Json properties = Json::object();
        properties["type"] = Node::Constant("sectionBreak", "Block discriminator.");
        return Node::Object("A section break.", {"type"}, std::move(properties));
    }

    static Json BuildWordTextInline()
    {
        Json properties = Json::object();
        properties["type"] = Node::Constant("text", "Inline discriminator.");
        properties["text"] = Node::String("Text content.");
        properties["bold"] = Node::Boolean("Bold; omitted when false.");
        properties["italic"] = Node::Boolean("Italic; omitted when false.");
        properties["underline"] = Node::Boolean("Underline; omitted when false.");
        properties["strike"] = Node::Boolean("Strikethrough; omitted when false.");
        properties["caps"] = Node::Boolean("All capitals; omitted when false.");
        properties["smallCaps"] = Node::Boolean("Small capitals; omitted when false.");
        properties["color"] = Node::String("Run color, usually RRGGBB (\"auto\" is also valid Word).");
        properties["highlight"] = Node::String("Highlight token, e.g. \"yellow\".");
        properties["font"] = Node::String("ASCII font family.");
        properties["style"] = Node::String("Character style ID.");
        properties["sizePt"] = Node::Number("Font size in points.");
        return Node::Object("Text with character formatting.", {"type", "text"}, std::move(properties));
    }

    static Json BuildWordLinkInline()
    {
        Json properties = Json::object();
        properties["type"] = Node::Constant("link", "Inline discriminator.");
        properties["target"] = Node::String("External URL; empty for internal links.");
        properties["anchor"] = Node::String("Bookmark name for an internal link.");
        properties["tooltip"] = Node::String("Hyperlink tooltip.");
        properties["content"] = Node::RefArray("wordInline", "The link text.");
        return Node::Object("A hyperlink.", {"type", "content"}, std::move(properties));
    }

    static Json BuildWordImageInline()
    {
        Json properties = Json::object();
        properties["type"] = Node::Constant("image", "Inline discriminator.");
        properties["media"] = Node::String("Media ID; matches an entry of the envelope `media`.");
        properties["alt"] = Node::String("Alternative text.");
        properties["widthEmu"] = Node::Integer("Width in EMU (914400 EMU = 1 inch).");
        properties["heightEmu"] = Node::Integer("Height in EMU.");
        return Node::Object("An inline image.", {"type"}, std::move(properties));
    }

    static Json BuildWordNoteRefInline(const char* token, const char* description)
    {
        Json properties = Json::object();
        properties["type"] = Node::Constant(token, "Inline discriminator.");
        properties["id"] = Node::Integer("Identifier of the referenced entry.");
        return Node::Object(description, {"type", "id"}, std::move(properties));
    }

    static Json BuildWordFieldInline()
    {
        Json properties = Json::object();
        properties["type"] = Node::Constant("field", "Inline discriminator.");
        properties["code"] = Node::String("Field instruction, e.g. \"PAGE\".");
        properties["text"] = Node::String("Cached result; fields are never evaluated.");
        return Node::Object("A field with its cached result.", {"type"}, std::move(properties));
    }

    static Json BuildWordBreakInline()
    {
        Json properties = Json::object();
        properties["type"] = Node::Constant("break", "Inline discriminator.");
        properties["kind"] = Node::Enumeration({"line", "page", "column"}, "Break kind; defaults to line.");
        return Node::Object("A line, page, or column break.", {"type"}, std::move(properties));
    }

    static Json BuildWordListDefinition()
    {
        Json properties = Json::object();
        properties["id"] = Node::Integer("Numbering instance ID paragraphs refer to.");
        properties["levels"] = Node::RefArray("wordListLevel", "Level definitions, outermost first.");
        return Node::Object("One numbering definition.", {"id", "levels"}, std::move(properties));
    }

    static Json BuildWordListLevel()
    {
        Json properties = Json::object();
        properties["format"] =
            Node::String("Numbering format token (decimal, bullet, lowerLetter, upperRoman, ...).");
        properties["text"] = Node::String("Level text pattern such as \"%1.\", or the bullet glyph.");
        properties["start"] = Node::Integer("First number of the level; omitted when 1.");
        return Node::Object("One level of a numbering definition.", {"format"}, std::move(properties));
    }

    static Json BuildWordNote()
    {
        Json properties = Json::object();
        properties["id"] = Node::Integer("Note ID referenced by footnoteRef/endnoteRef.");
        properties["blocks"] = Node::RefArray("wordBlock", "Note content.");
        return Node::Object("One footnote or endnote.", {"id", "blocks"}, std::move(properties));
    }

    static Json BuildWordComment()
    {
        Json properties = Json::object();
        properties["id"] = Node::Integer("Comment ID referenced by commentRef.");
        properties["author"] = Node::String("Comment author.");
        properties["initials"] = Node::String("Author initials.");
        properties["date"] = Node::String("ISO-8601 timestamp.");
        properties["blocks"] = Node::RefArray("wordBlock", "Comment content.");
        return Node::Object("One comment.", {"id", "blocks"}, std::move(properties));
    }

    static Json BuildWordHeaderFooter()
    {
        Json properties = Json::object();
        properties["kind"] = Node::Enumeration({"default", "first", "even"}, "Which pages the entry covers.");
        properties["blocks"] = Node::RefArray("wordBlock", "Header or footer content.");
        return Node::Object("Content of one header or footer.", {"kind", "blocks"}, std::move(properties));
    }

    // --- Excel -------------------------------------------------------------

    static Json BuildExcelDocument()
    {
        Json properties = Json::object();
        properties["sheets"] = Node::RefArray("excelSheet", "Worksheets in workbook order.");
        return Node::Object("Excel payload of the envelope.", {"sheets"}, std::move(properties));
    }

    static Json BuildExcelSheet()
    {
        Json properties = Json::object();
        properties["name"] = Node::String("Worksheet name.");
        properties["cells"] = Node::RefArray("excelCell", "Stored cells, sparse and row-major.");
        properties["merges"] =
            Node::ArrayOf(Node::String("Merged range in A1 form, e.g. \"A1:B2\"."), "Merged ranges.");
        properties["tables"] = Node::RefArray("excelTable", "Worksheet tables (ListObjects).");
        properties["hyperlinks"] = Node::RefArray("excelHyperlink", "Cell hyperlinks.");
        return Node::Object("One worksheet.", {"name", "cells"}, std::move(properties));
    }

    static Json BuildExcelCell()
    {
        Json properties = Json::object();
        properties["cell"] = Node::String("A1-style address, e.g. \"B2\".");
        properties["type"] = Node::Enumeration({"string", "number", "bool", "error", "datetime", "formula"},
                                               "How to interpret the cell.");
        properties["value"] = Node::String("Canonical value text; absent on formula cells.");
        properties["formula"] = Node::String("Formula text without the leading '='.");
        properties["cached"] = Node::Ref("excelCachedValue");
        return Node::Object("One stored worksheet cell.", {"cell", "type"}, std::move(properties));
    }

    static Json BuildExcelCachedValue()
    {
        Json properties = Json::object();
        properties["type"] = Node::String("Type of the cached result.");
        properties["value"] = Node::String("Cached result text; never recomputed.");
        return Node::Object("Cached formula result stored in the package.", {"type", "value"},
                            std::move(properties));
    }

    static Json BuildExcelTable()
    {
        Json properties = Json::object();
        properties["name"] = Node::String("Table name.");
        properties["range"] = Node::String("A1-style range, e.g. \"A1:C4\".");
        return Node::Object("One worksheet table.", {"name", "range"}, std::move(properties));
    }

    static Json BuildExcelHyperlink()
    {
        Json properties = Json::object();
        properties["cell"] = Node::String("A1-style anchor cell.");
        properties["target"] = Node::String("Link target.");
        properties["tooltip"] = Node::String("Link tooltip.");
        return Node::Object("One cell hyperlink.", {"cell", "target"}, std::move(properties));
    }

    // --- PowerPoint --------------------------------------------------------

    static Json BuildPowerPointDocument()
    {
        Json properties = Json::object();
        properties["slides"] = Node::RefArray("powerPointSlide", "Slides in presentation order.");
        return Node::Object("PowerPoint payload of the envelope.", {"slides"}, std::move(properties));
    }

    static Json BuildPowerPointSlide()
    {
        Json properties = Json::object();
        properties["layout"] = Node::String("Layout display name; informational on import.");
        properties["hidden"] = Node::Boolean("Slide is hidden; omitted when false.");
        properties["shapes"] = Node::RefArray("powerPointShape", "Shapes in z-order.");
        properties["notes"] = Node::String("Speaker notes as plain text.");
        properties["comments"] =
            Node::RefArray("powerPointComment", "Slide comments; informational on import.");
        return Node::Object("One slide.", {"shapes"}, std::move(properties));
    }

    static Json BuildPowerPointShape()
    {
        Json properties = Json::object();
        properties["type"] = Node::Enumeration({"textBox", "placeholder", "picture", "table", "group", "other"},
                                               "Shape kind; `other` is unsupported content exported empty.");
        properties["name"] = Node::String("Shape name.");
        properties["placeholder"] =
            Node::String("Placeholder token (title, ctrTitle, subTitle, body, ftr, sldNum, dt, ...).");
        properties["transform"] = Node::Ref("powerPointTransform");
        properties["text"] = Node::Ref("powerPointTextFrame");
        properties["rows"] = Node::RefArray("powerPointTableRow", "Table rows; table shapes only.");
        properties["media"] = Node::String("Media ID; matches an entry of the envelope `media`.");
        properties["alt"] = Node::String("Alternative text.");
        properties["children"] = Node::RefArray("powerPointShape", "Child shapes; group shapes only.");
        return Node::Object("One slide shape.", {"type"}, std::move(properties));
    }

    static Json BuildPowerPointTransform()
    {
        Json properties = Json::object();
        properties["x"] = Node::Integer("Offset X in EMU.");
        properties["y"] = Node::Integer("Offset Y in EMU.");
        properties["cx"] = Node::Integer("Width in EMU.");
        properties["cy"] = Node::Integer("Height in EMU.");
        return Node::Object("Shape placement in EMU; omitted when the shape has no explicit transform.",
                            {"x", "y", "cx", "cy"}, std::move(properties));
    }

    static Json BuildPowerPointTextFrame()
    {
        return Node::RefArray("powerPointParagraph", "A text frame: its paragraphs in order.");
    }

    static Json BuildPowerPointParagraph()
    {
        Json properties = Json::object();
        properties["level"] = Node::Integer("Indent/outline level; omitted when 0.", 0, 8);
        properties["runs"] = Node::RefArray("powerPointRun", "Formatted runs in order.");
        return Node::Object("One paragraph of a text frame.", {"runs"}, std::move(properties));
    }

    static Json BuildPowerPointRun()
    {
        Json properties = Json::object();
        properties["text"] = Node::String("Run text.");
        properties["bold"] = Node::Boolean("Bold; omitted when false.");
        properties["italic"] = Node::Boolean("Italic; omitted when false.");
        properties["underline"] = Node::Boolean("Underline; omitted when false.");
        properties["sizePt"] = Node::Number("Font size in points.");
        properties["color"] = Node::String("Run color as RRGGBB.");
        properties["link"] = Node::String("Hyperlink URL.");
        return Node::Object("One formatted text run.", {"text"}, std::move(properties));
    }

    static Json BuildPowerPointTableRow()
    {
        return Node::RefArray("powerPointTableCell", "One table row: its cells, left to right.");
    }

    static Json BuildPowerPointTableCell()
    {
        Json properties = Json::object();
        properties["rowSpan"] = Node::Integer("Vertical span; omitted when 1.");
        properties["colSpan"] = Node::Integer("Horizontal span; omitted when 1.");
        properties["covered"] =
            Node::Boolean("True for a grid position covered by another cell's span; then no content.");
        properties["text"] = Node::Ref("powerPointTextFrame");
        return Node::Object("One table cell.", {}, std::move(properties));
    }

    static Json BuildPowerPointComment()
    {
        Json properties = Json::object();
        properties["author"] = Node::String("Comment author.");
        properties["text"] = Node::String("Comment text.");
        return Node::Object("One slide comment.", {"text"}, std::move(properties));
    }
};

/// Turns every validator complaint into an Error diagnostic instead of an
/// exception, so a caller gets the whole list rather than the first failure.
class DocumentModelSchemaErrorHandler final : public nlohmann::json_schema::basic_error_handler
{
public:
    explicit DocumentModelSchemaErrorHandler(std::vector<ToolDiagnostic>& diagnostics)
        : m_diagnostics(&diagnostics)
    {
    }

    void error(const nlohmann::json::json_pointer& pointer, const nlohmann::json& instance,
               const std::string& message) override
    {
        nlohmann::json_schema::basic_error_handler::error(pointer, instance, message);

        auto location = pointer.to_string();
        if (location.empty())
        {
            location = "/";
        }
        m_diagnostics->push_back(ToolDiagnostic{ToolSeverity::Error, message, std::move(location)});
    }

private:
    std::vector<ToolDiagnostic>* m_diagnostics;
};

std::string GetDocumentModelJsonSchema()
{
    return DocumentModelSchemaFactory::BuildRoot().dump(2) + "\n";
}

std::string GetDocumentModelJsonSchemaFileName()
{
    return "exyokioffice-document-v" + std::to_string(DocumentModelVersion) + ".schema.json";
}

bool ValidateModelJson(std::string_view json, std::vector<ToolDiagnostic>& diagnostics)
{
    const auto instance = nlohmann::json::parse(json, nullptr, false);
    if (instance.is_discarded())
    {
        diagnostics.push_back(ToolDiagnostic{ToolSeverity::Error, "Input is not valid JSON"});
        return false;
    }

    try
    {
        nlohmann::json_schema::json_validator validator;
        validator.set_root_schema(nlohmann::json::parse(GetDocumentModelJsonSchema()));

        DocumentModelSchemaErrorHandler handler(diagnostics);
        validator.validate(instance, handler);
        return !static_cast<bool>(handler);
    }
    catch (const std::exception& error)
    {
        diagnostics.push_back(
            ToolDiagnostic{ToolSeverity::Error, "Schema validation could not run", error.what()});
        return false;
    }
}

} // namespace ExyokiOffice::Tools
