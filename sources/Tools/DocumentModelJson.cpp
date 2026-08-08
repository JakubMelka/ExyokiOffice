// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "ExyokiOffice/Tools/DocumentModelIO.hpp"

#include "Tools/DocumentModelJsonInternal.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <cctype>

namespace ExyokiOffice::Tools
{

namespace detail
{

using Json = nlohmann::ordered_json;

namespace
{

// --- Tolerant accessors -----------------------------------------------------
//
// The semantic-XML variant of the envelope stores every scalar as an attribute
// string, so string/number/bool distinctions can blur on round trips. These
// accessors accept any scalar shape and coerce.

std::string GetString(const Json& object, std::string_view key)
{
    const auto it = object.find(key);
    if (it == object.end())
    {
        return {};
    }
    if (it->is_string())
    {
        return it->get<std::string>();
    }
    if (it->is_boolean())
    {
        return it->get<bool>() ? "true" : "false";
    }
    if (it->is_number_integer())
    {
        return std::to_string(it->get<Int64>());
    }
    if (it->is_number())
    {
        return std::to_string(it->get<Real>());
    }
    return {};
}

bool GetBool(const Json& object, std::string_view key)
{
    const auto it = object.find(key);
    if (it == object.end())
    {
        return false;
    }
    if (it->is_boolean())
    {
        return it->get<bool>();
    }
    if (it->is_string())
    {
        return it->get<std::string>() == "true";
    }
    if (it->is_number())
    {
        return it->get<Real>() != 0.0;
    }
    return false;
}

Int64 GetInt(const Json& object, std::string_view key, Int64 defaultValue = 0)
{
    const auto it = object.find(key);
    if (it == object.end())
    {
        return defaultValue;
    }
    if (it->is_number_integer())
    {
        return it->get<Int64>();
    }
    if (it->is_number())
    {
        return static_cast<Int64>(it->get<Real>());
    }
    if (it->is_string())
    {
        try
        {
            return std::stoll(it->get<std::string>());
        }
        catch (...)
        {
            return defaultValue;
        }
    }
    return defaultValue;
}

Real GetDouble(const Json& object, std::string_view key, Real defaultValue = 0.0)
{
    const auto it = object.find(key);
    if (it == object.end())
    {
        return defaultValue;
    }
    if (it->is_number())
    {
        return it->get<Real>();
    }
    if (it->is_string())
    {
        try
        {
            return std::stod(it->get<std::string>());
        }
        catch (...)
        {
            return defaultValue;
        }
    }
    return defaultValue;
}

const Json* GetArray(const Json& object, std::string_view key)
{
    const auto it = object.find(key);
    return it != object.end() && it->is_array() ? &*it : nullptr;
}

const Json* GetObject(const Json& object, std::string_view key)
{
    const auto it = object.find(key);
    return it != object.end() && it->is_object() ? &*it : nullptr;
}

void SetIfNotEmpty(Json& object, std::string_view key, const std::string& value)
{
    if (!value.empty())
    {
        object[std::string(key)] = value;
    }
}

void SetIfTrue(Json& object, std::string_view key, bool value)
{
    if (value)
    {
        object[std::string(key)] = true;
    }
}

void Warn(std::vector<ToolDiagnostic>& diagnostics, std::string message, std::string context = {})
{
    diagnostics.push_back(ToolDiagnostic{ToolSeverity::Warning, std::move(message), std::move(context)});
}

// --- Word serialization -----------------------------------------------------

Json WordInlineToJson(const WordInline& node);

Json WordInlinesToJson(const std::vector<WordInline>& inlines)
{
    Json array = Json::array();
    for (const auto& node : inlines)
    {
        array.push_back(WordInlineToJson(node));
    }
    return array;
}

Json WordInlineToJson(const WordInline& node)
{
    Json object = Json::object();
    switch (node.Kind)
    {
        case WordInline::Type::Text:
        {
            object["type"] = "text";
            object["text"] = node.Text;
            SetIfTrue(object, "bold", node.Props.Bold);
            SetIfTrue(object, "italic", node.Props.Italic);
            SetIfTrue(object, "underline", node.Props.Underline);
            SetIfTrue(object, "strike", node.Props.Strike);
            SetIfTrue(object, "caps", node.Props.Caps);
            SetIfTrue(object, "smallCaps", node.Props.SmallCaps);
            SetIfNotEmpty(object, "color", node.Props.Color);
            SetIfNotEmpty(object, "highlight", node.Props.Highlight);
            SetIfNotEmpty(object, "font", node.Props.Font);
            SetIfNotEmpty(object, "style", node.Props.StyleId);
            if (node.Props.FontSizePt > 0.0)
            {
                object["sizePt"] = node.Props.FontSizePt;
            }
            break;
        }
        case WordInline::Type::Hyperlink:
        {
            object["type"] = "link";
            SetIfNotEmpty(object, "target", node.Target);
            SetIfNotEmpty(object, "anchor", node.Anchor);
            SetIfNotEmpty(object, "tooltip", node.Tooltip);
            object["content"] = WordInlinesToJson(node.Children);
            break;
        }
        case WordInline::Type::Image:
        {
            object["type"] = "image";
            SetIfNotEmpty(object, "media", node.MediaId);
            SetIfNotEmpty(object, "alt", node.AltText);
            if (node.WidthEmu > 0)
            {
                object["widthEmu"] = node.WidthEmu;
            }
            if (node.HeightEmu > 0)
            {
                object["heightEmu"] = node.HeightEmu;
            }
            break;
        }
        case WordInline::Type::FootnoteRef:
            object["type"] = "footnoteRef";
            object["id"] = node.NoteId;
            break;
        case WordInline::Type::EndnoteRef:
            object["type"] = "endnoteRef";
            object["id"] = node.NoteId;
            break;
        case WordInline::Type::CommentRef:
            object["type"] = "commentRef";
            object["id"] = node.NoteId;
            break;
        case WordInline::Type::Field:
            object["type"] = "field";
            SetIfNotEmpty(object, "code", node.FieldCode);
            SetIfNotEmpty(object, "text", node.Text);
            break;
        case WordInline::Type::Break:
            object["type"] = "break";
            SetIfNotEmpty(object, "kind", node.BreakKind);
            break;
    }
    return object;
}

Json WordBlocksToJson(const std::vector<WordBlock>& blocks);

Json WordBlockToJson(const WordBlock& block)
{
    Json object = Json::object();
    switch (block.Kind)
    {
        case WordBlock::Type::Paragraph:
        {
            object["type"] = "paragraph";
            if (block.Paragraph)
            {
                const auto& paragraph = *block.Paragraph;
                SetIfNotEmpty(object, "style", paragraph.StyleId);
                if (paragraph.HeadingLevel > 0)
                {
                    object["heading"] = paragraph.HeadingLevel;
                }
                SetIfNotEmpty(object, "align", paragraph.Alignment);
                if (paragraph.List)
                {
                    Json list = Json::object();
                    list["id"] = paragraph.List->NumberingId;
                    if (paragraph.List->Level > 0)
                    {
                        list["level"] = paragraph.List->Level;
                    }
                    object["list"] = std::move(list);
                }
                object["content"] = WordInlinesToJson(paragraph.Inlines);
            }
            break;
        }
        case WordBlock::Type::Table:
        {
            object["type"] = "table";
            if (block.Table)
            {
                SetIfNotEmpty(object, "style", block.Table->StyleId);
                Json rows = Json::array();
                for (const auto& row : block.Table->Rows)
                {
                    Json rowObject = Json::object();
                    Json cells = Json::array();
                    for (const auto& cell : row.Cells)
                    {
                        Json cellObject = Json::object();
                        if (cell.Covered)
                        {
                            cellObject["covered"] = true;
                        }
                        else
                        {
                            if (cell.RowSpan > 1)
                            {
                                cellObject["rowSpan"] = cell.RowSpan;
                            }
                            if (cell.ColSpan > 1)
                            {
                                cellObject["colSpan"] = cell.ColSpan;
                            }
                            cellObject["blocks"] = WordBlocksToJson(cell.Blocks);
                        }
                        cells.push_back(std::move(cellObject));
                    }
                    rowObject["cells"] = std::move(cells);
                    rows.push_back(std::move(rowObject));
                }
                object["rows"] = std::move(rows);
            }
            break;
        }
        case WordBlock::Type::SectionBreak:
            object["type"] = "sectionBreak";
            break;
    }
    return object;
}

Json WordBlocksToJson(const std::vector<WordBlock>& blocks)
{
    Json array = Json::array();
    for (const auto& block : blocks)
    {
        array.push_back(WordBlockToJson(block));
    }
    return array;
}

Json WordModelToJson(const WordDocumentModel& model)
{
    Json object = Json::object();
    object["body"] = WordBlocksToJson(model.Body);

    if (!model.Lists.empty())
    {
        Json lists = Json::array();
        for (const auto& definition : model.Lists)
        {
            Json listObject = Json::object();
            listObject["id"] = definition.NumberingId;
            Json levels = Json::array();
            for (const auto& level : definition.Levels)
            {
                Json levelObject = Json::object();
                levelObject["format"] = level.Format;
                SetIfNotEmpty(levelObject, "text", level.LevelText);
                if (level.Start != 1)
                {
                    levelObject["start"] = level.Start;
                }
                levels.push_back(std::move(levelObject));
            }
            listObject["levels"] = std::move(levels);
            lists.push_back(std::move(listObject));
        }
        object["lists"] = std::move(lists);
    }

    const auto notesToJson = [](const std::vector<WordNote>& notes)
    {
        Json array = Json::array();
        for (const auto& note : notes)
        {
            Json noteObject = Json::object();
            noteObject["id"] = note.Id;
            noteObject["blocks"] = WordBlocksToJson(note.Blocks);
            array.push_back(std::move(noteObject));
        }
        return array;
    };
    if (!model.Footnotes.empty())
    {
        object["footnotes"] = notesToJson(model.Footnotes);
    }
    if (!model.Endnotes.empty())
    {
        object["endnotes"] = notesToJson(model.Endnotes);
    }

    if (!model.Comments.empty())
    {
        Json comments = Json::array();
        for (const auto& comment : model.Comments)
        {
            Json commentObject = Json::object();
            commentObject["id"] = comment.Id;
            SetIfNotEmpty(commentObject, "author", comment.Author);
            SetIfNotEmpty(commentObject, "initials", comment.Initials);
            SetIfNotEmpty(commentObject, "date", comment.Date);
            commentObject["blocks"] = WordBlocksToJson(comment.Blocks);
            comments.push_back(std::move(commentObject));
        }
        object["comments"] = std::move(comments);
    }

    const auto headersToJson = [](const std::vector<WordHeaderFooter>& entries)
    {
        Json array = Json::array();
        for (const auto& entry : entries)
        {
            Json entryObject = Json::object();
            entryObject["kind"] = entry.Kind;
            entryObject["blocks"] = WordBlocksToJson(entry.Blocks);
            array.push_back(std::move(entryObject));
        }
        return array;
    };
    if (!model.Headers.empty())
    {
        object["headers"] = headersToJson(model.Headers);
    }
    if (!model.Footers.empty())
    {
        object["footers"] = headersToJson(model.Footers);
    }
    return object;
}

// --- Excel serialization ----------------------------------------------------

Json ExcelModelToJson(const ExcelWorkbookModel& model)
{
    Json object = Json::object();
    Json sheets = Json::array();
    for (const auto& sheet : model.Sheets)
    {
        Json sheetObject = Json::object();
        sheetObject["name"] = sheet.Name;

        Json cells = Json::array();
        for (const auto& cell : sheet.Cells)
        {
            Json cellObject = Json::object();
            cellObject["cell"] = cell.Address;
            cellObject["type"] = cell.Type;
            if (cell.Type == "formula")
            {
                cellObject["formula"] = cell.Formula;
                if (!cell.CachedType.empty())
                {
                    Json cached = Json::object();
                    cached["type"] = cell.CachedType;
                    cached["value"] = cell.CachedValue;
                    cellObject["cached"] = std::move(cached);
                }
            }
            else
            {
                cellObject["value"] = cell.Value;
            }
            cells.push_back(std::move(cellObject));
        }
        sheetObject["cells"] = std::move(cells);

        if (!sheet.Merges.empty())
        {
            sheetObject["merges"] = sheet.Merges;
        }
        if (!sheet.Tables.empty())
        {
            Json tables = Json::array();
            for (const auto& table : sheet.Tables)
            {
                Json tableObject = Json::object();
                tableObject["name"] = table.Name;
                tableObject["range"] = table.Range;
                tables.push_back(std::move(tableObject));
            }
            sheetObject["tables"] = std::move(tables);
        }
        if (!sheet.Hyperlinks.empty())
        {
            Json hyperlinks = Json::array();
            for (const auto& hyperlink : sheet.Hyperlinks)
            {
                Json linkObject = Json::object();
                linkObject["cell"] = hyperlink.Cell;
                linkObject["target"] = hyperlink.Target;
                SetIfNotEmpty(linkObject, "tooltip", hyperlink.Tooltip);
                hyperlinks.push_back(std::move(linkObject));
            }
            sheetObject["hyperlinks"] = std::move(hyperlinks);
        }
        sheets.push_back(std::move(sheetObject));
    }
    object["sheets"] = std::move(sheets);
    return object;
}

// --- PowerPoint serialization -----------------------------------------------

std::string_view PptShapeTypeToken(PptShape::Type kind)
{
    switch (kind)
    {
        case PptShape::Type::TextBox:
            return "textBox";
        case PptShape::Type::Placeholder:
            return "placeholder";
        case PptShape::Type::Picture:
            return "picture";
        case PptShape::Type::Table:
            return "table";
        case PptShape::Type::Group:
            return "group";
        case PptShape::Type::Other:
            break;
    }
    return "other";
}

Json PptTextFrameToJson(const PptTextFrame& frame)
{
    Json paragraphs = Json::array();
    for (const auto& paragraph : frame.Paragraphs)
    {
        Json paragraphObject = Json::object();
        if (paragraph.Level > 0)
        {
            paragraphObject["level"] = paragraph.Level;
        }
        Json runs = Json::array();
        for (const auto& run : paragraph.Runs)
        {
            Json runObject = Json::object();
            runObject["text"] = run.Text;
            SetIfTrue(runObject, "bold", run.Bold);
            SetIfTrue(runObject, "italic", run.Italic);
            SetIfTrue(runObject, "underline", run.Underline);
            if (run.FontSizePt > 0.0)
            {
                runObject["sizePt"] = run.FontSizePt;
            }
            SetIfNotEmpty(runObject, "color", run.Color);
            SetIfNotEmpty(runObject, "link", run.Hyperlink);
            runs.push_back(std::move(runObject));
        }
        paragraphObject["runs"] = std::move(runs);
        paragraphs.push_back(std::move(paragraphObject));
    }
    return paragraphs;
}

Json PptShapeToJson(const PptShape& shape)
{
    Json object = Json::object();
    object["type"] = std::string(PptShapeTypeToken(shape.Kind));
    SetIfNotEmpty(object, "name", shape.Name);
    SetIfNotEmpty(object, "placeholder", shape.PlaceholderType);

    if (shape.Transform.Present)
    {
        Json transform = Json::object();
        transform["x"] = shape.Transform.X;
        transform["y"] = shape.Transform.Y;
        transform["cx"] = shape.Transform.Cx;
        transform["cy"] = shape.Transform.Cy;
        object["transform"] = std::move(transform);
    }

    if (shape.Text)
    {
        object["text"] = PptTextFrameToJson(*shape.Text);
    }
    if (shape.Table)
    {
        Json rows = Json::array();
        for (const auto& row : shape.Table->Rows)
        {
            Json cells = Json::array();
            for (const auto& cell : row)
            {
                Json cellObject = Json::object();
                if (cell.Covered)
                {
                    cellObject["covered"] = true;
                }
                else
                {
                    if (cell.RowSpan > 1)
                    {
                        cellObject["rowSpan"] = cell.RowSpan;
                    }
                    if (cell.ColSpan > 1)
                    {
                        cellObject["colSpan"] = cell.ColSpan;
                    }
                    cellObject["text"] = PptTextFrameToJson(cell.Text);
                }
                cells.push_back(std::move(cellObject));
            }
            rows.push_back(std::move(cells));
        }
        object["rows"] = std::move(rows);
    }
    SetIfNotEmpty(object, "media", shape.MediaId);
    SetIfNotEmpty(object, "alt", shape.AltText);

    if (!shape.Children.empty())
    {
        Json children = Json::array();
        for (const auto& child : shape.Children)
        {
            children.push_back(PptShapeToJson(child));
        }
        object["children"] = std::move(children);
    }
    return object;
}

Json PowerPointModelToJson(const PowerPointDeckModel& model)
{
    Json object = Json::object();
    Json slides = Json::array();
    for (const auto& slide : model.Slides)
    {
        Json slideObject = Json::object();
        SetIfNotEmpty(slideObject, "layout", slide.LayoutName);
        SetIfTrue(slideObject, "hidden", slide.Hidden);
        Json shapes = Json::array();
        for (const auto& shape : slide.Shapes)
        {
            shapes.push_back(PptShapeToJson(shape));
        }
        slideObject["shapes"] = std::move(shapes);
        SetIfNotEmpty(slideObject, "notes", slide.NotesText);
        if (!slide.Comments.empty())
        {
            Json comments = Json::array();
            for (const auto& comment : slide.Comments)
            {
                Json commentObject = Json::object();
                SetIfNotEmpty(commentObject, "author", comment.Author);
                commentObject["text"] = comment.Text;
                comments.push_back(std::move(commentObject));
            }
            slideObject["comments"] = std::move(comments);
        }
        slides.push_back(std::move(slideObject));
    }
    object["slides"] = std::move(slides);
    return object;
}

// --- Word parsing -----------------------------------------------------------

std::vector<WordInline> WordInlinesFromJson(const Json& array, std::vector<ToolDiagnostic>& diagnostics);

WordInline WordInlineFromJson(const Json& object, std::vector<ToolDiagnostic>& diagnostics)
{
    WordInline node;
    const auto type = GetString(object, "type");
    if (type == "text" || type.empty())
    {
        node.Kind = WordInline::Type::Text;
        node.Text = GetString(object, "text");
        node.Props.Bold = GetBool(object, "bold");
        node.Props.Italic = GetBool(object, "italic");
        node.Props.Underline = GetBool(object, "underline");
        node.Props.Strike = GetBool(object, "strike");
        node.Props.Caps = GetBool(object, "caps");
        node.Props.SmallCaps = GetBool(object, "smallCaps");
        node.Props.Color = GetString(object, "color");
        node.Props.Highlight = GetString(object, "highlight");
        node.Props.Font = GetString(object, "font");
        node.Props.StyleId = GetString(object, "style");
        node.Props.FontSizePt = GetDouble(object, "sizePt");
    }
    else if (type == "link")
    {
        node.Kind = WordInline::Type::Hyperlink;
        node.Target = GetString(object, "target");
        node.Anchor = GetString(object, "anchor");
        node.Tooltip = GetString(object, "tooltip");
        if (const auto* content = GetArray(object, "content"))
        {
            node.Children = WordInlinesFromJson(*content, diagnostics);
        }
    }
    else if (type == "image")
    {
        node.Kind = WordInline::Type::Image;
        node.MediaId = GetString(object, "media");
        node.AltText = GetString(object, "alt");
        node.WidthEmu = GetInt(object, "widthEmu");
        node.HeightEmu = GetInt(object, "heightEmu");
    }
    else if (type == "footnoteRef")
    {
        node.Kind = WordInline::Type::FootnoteRef;
        node.NoteId = static_cast<int>(GetInt(object, "id"));
    }
    else if (type == "endnoteRef")
    {
        node.Kind = WordInline::Type::EndnoteRef;
        node.NoteId = static_cast<int>(GetInt(object, "id"));
    }
    else if (type == "commentRef")
    {
        node.Kind = WordInline::Type::CommentRef;
        node.NoteId = static_cast<int>(GetInt(object, "id"));
    }
    else if (type == "field")
    {
        node.Kind = WordInline::Type::Field;
        node.FieldCode = GetString(object, "code");
        node.Text = GetString(object, "text");
    }
    else if (type == "break")
    {
        node.Kind = WordInline::Type::Break;
        node.BreakKind = GetString(object, "kind");
        if (node.BreakKind.empty())
        {
            node.BreakKind = "line";
        }
    }
    else
    {
        Warn(diagnostics, "Unknown inline type treated as text", type);
        node.Kind = WordInline::Type::Text;
        node.Text = GetString(object, "text");
    }
    return node;
}

std::vector<WordInline> WordInlinesFromJson(const Json& array, std::vector<ToolDiagnostic>& diagnostics)
{
    std::vector<WordInline> inlines;
    for (const auto& item : array)
    {
        if (item.is_object())
        {
            inlines.push_back(WordInlineFromJson(item, diagnostics));
        }
    }
    return inlines;
}

std::vector<WordBlock> WordBlocksFromJson(const Json& array, std::vector<ToolDiagnostic>& diagnostics);

WordBlock WordBlockFromJson(const Json& object, std::vector<ToolDiagnostic>& diagnostics)
{
    WordBlock block;
    const auto type = GetString(object, "type");
    if (type == "table")
    {
        block.Kind = WordBlock::Type::Table;
        WordTable table;
        table.StyleId = GetString(object, "style");
        if (const auto* rows = GetArray(object, "rows"))
        {
            for (const auto& rowValue : *rows)
            {
                WordTableRow row;
                const Json* cells = rowValue.is_object() ? GetArray(rowValue, "cells")
                                                         : (rowValue.is_array() ? &rowValue : nullptr);
                if (cells != nullptr)
                {
                    for (const auto& cellValue : *cells)
                    {
                        WordTableCell cell;
                        if (cellValue.is_object())
                        {
                            cell.Covered = GetBool(cellValue, "covered");
                            cell.RowSpan = static_cast<int>(GetInt(cellValue, "rowSpan", 1));
                            cell.ColSpan = static_cast<int>(GetInt(cellValue, "colSpan", 1));
                            if (const auto* blocks = GetArray(cellValue, "blocks"))
                            {
                                cell.Blocks = WordBlocksFromJson(*blocks, diagnostics);
                            }
                        }
                        row.Cells.push_back(std::move(cell));
                    }
                }
                table.Rows.push_back(std::move(row));
            }
        }
        block.Table = std::move(table);
    }
    else if (type == "sectionBreak")
    {
        block.Kind = WordBlock::Type::SectionBreak;
    }
    else
    {
        if (type != "paragraph" && !type.empty())
        {
            Warn(diagnostics, "Unknown block type treated as paragraph", type);
        }
        block.Kind = WordBlock::Type::Paragraph;
        WordParagraph paragraph;
        paragraph.StyleId = GetString(object, "style");
        paragraph.HeadingLevel = static_cast<int>(GetInt(object, "heading"));
        paragraph.Alignment = GetString(object, "align");
        if (const auto* list = GetObject(object, "list"))
        {
            WordListRef reference;
            reference.NumberingId = static_cast<int>(GetInt(*list, "id"));
            reference.Level = static_cast<int>(GetInt(*list, "level"));
            paragraph.List = reference;
        }
        if (const auto* content = GetArray(object, "content"))
        {
            paragraph.Inlines = WordInlinesFromJson(*content, diagnostics);
        }
        block.Paragraph = std::move(paragraph);
    }
    return block;
}

std::vector<WordBlock> WordBlocksFromJson(const Json& array, std::vector<ToolDiagnostic>& diagnostics)
{
    std::vector<WordBlock> blocks;
    for (const auto& item : array)
    {
        if (item.is_object())
        {
            blocks.push_back(WordBlockFromJson(item, diagnostics));
        }
    }
    return blocks;
}

WordDocumentModel WordModelFromJson(const Json& object, std::vector<ToolDiagnostic>& diagnostics)
{
    WordDocumentModel model;
    if (const auto* body = GetArray(object, "body"))
    {
        model.Body = WordBlocksFromJson(*body, diagnostics);
    }
    if (const auto* lists = GetArray(object, "lists"))
    {
        for (const auto& listValue : *lists)
        {
            if (!listValue.is_object())
            {
                continue;
            }
            WordListDefinition definition;
            definition.NumberingId = static_cast<int>(GetInt(listValue, "id"));
            if (const auto* levels = GetArray(listValue, "levels"))
            {
                for (const auto& levelValue : *levels)
                {
                    if (!levelValue.is_object())
                    {
                        continue;
                    }
                    WordListLevel level;
                    level.Format = GetString(levelValue, "format");
                    if (level.Format.empty())
                    {
                        level.Format = "decimal";
                    }
                    level.LevelText = GetString(levelValue, "text");
                    level.Start = static_cast<int>(GetInt(levelValue, "start", 1));
                    definition.Levels.push_back(std::move(level));
                }
            }
            model.Lists.push_back(std::move(definition));
        }
    }

    const auto notesFromJson = [&](const Json* array, std::vector<WordNote>& notes)
    {
        if (array == nullptr)
        {
            return;
        }
        for (const auto& noteValue : *array)
        {
            if (!noteValue.is_object())
            {
                continue;
            }
            WordNote note;
            note.Id = static_cast<int>(GetInt(noteValue, "id"));
            if (const auto* blocks = GetArray(noteValue, "blocks"))
            {
                note.Blocks = WordBlocksFromJson(*blocks, diagnostics);
            }
            notes.push_back(std::move(note));
        }
    };
    notesFromJson(GetArray(object, "footnotes"), model.Footnotes);
    notesFromJson(GetArray(object, "endnotes"), model.Endnotes);

    if (const auto* comments = GetArray(object, "comments"))
    {
        for (const auto& commentValue : *comments)
        {
            if (!commentValue.is_object())
            {
                continue;
            }
            WordComment comment;
            comment.Id = static_cast<int>(GetInt(commentValue, "id"));
            comment.Author = GetString(commentValue, "author");
            comment.Initials = GetString(commentValue, "initials");
            comment.Date = GetString(commentValue, "date");
            if (const auto* blocks = GetArray(commentValue, "blocks"))
            {
                comment.Blocks = WordBlocksFromJson(*blocks, diagnostics);
            }
            model.Comments.push_back(std::move(comment));
        }
    }

    const auto headersFromJson = [&](const Json* array, std::vector<WordHeaderFooter>& entries)
    {
        if (array == nullptr)
        {
            return;
        }
        for (const auto& entryValue : *array)
        {
            if (!entryValue.is_object())
            {
                continue;
            }
            WordHeaderFooter entry;
            entry.Kind = GetString(entryValue, "kind");
            if (entry.Kind.empty())
            {
                entry.Kind = "default";
            }
            if (const auto* blocks = GetArray(entryValue, "blocks"))
            {
                entry.Blocks = WordBlocksFromJson(*blocks, diagnostics);
            }
            entries.push_back(std::move(entry));
        }
    };
    headersFromJson(GetArray(object, "headers"), model.Headers);
    headersFromJson(GetArray(object, "footers"), model.Footers);
    return model;
}

// --- Excel parsing ----------------------------------------------------------

ExcelWorkbookModel ExcelModelFromJson(const Json& object, std::vector<ToolDiagnostic>& diagnostics)
{
    ExcelWorkbookModel model;
    if (const auto* sheets = GetArray(object, "sheets"))
    {
        for (const auto& sheetValue : *sheets)
        {
            if (!sheetValue.is_object())
            {
                continue;
            }
            ExcelSheetModel sheet;
            sheet.Name = GetString(sheetValue, "name");
            if (const auto* cells = GetArray(sheetValue, "cells"))
            {
                for (const auto& cellValue : *cells)
                {
                    if (!cellValue.is_object())
                    {
                        continue;
                    }
                    ExcelCellModel cell;
                    cell.Address = GetString(cellValue, "cell");
                    cell.Type = GetString(cellValue, "type");
                    cell.Value = GetString(cellValue, "value");
                    cell.Formula = GetString(cellValue, "formula");
                    if (cell.Type.empty())
                    {
                        cell.Type = cell.Formula.empty() ? "string" : "formula";
                    }
                    if (const auto* cached = GetObject(cellValue, "cached"))
                    {
                        cell.CachedType = GetString(*cached, "type");
                        cell.CachedValue = GetString(*cached, "value");
                    }
                    sheet.Cells.push_back(std::move(cell));
                }
            }
            if (const auto* merges = GetArray(sheetValue, "merges"))
            {
                for (const auto& mergeValue : *merges)
                {
                    if (mergeValue.is_string())
                    {
                        sheet.Merges.push_back(mergeValue.get<std::string>());
                    }
                    else if (mergeValue.is_object())
                    {
                        // Semantic-XML round trip stores merges as objects with a "value" attribute.
                        auto text = GetString(mergeValue, "value");
                        if (!text.empty())
                        {
                            sheet.Merges.push_back(std::move(text));
                        }
                    }
                }
            }
            if (const auto* tables = GetArray(sheetValue, "tables"))
            {
                for (const auto& tableValue : *tables)
                {
                    if (!tableValue.is_object())
                    {
                        continue;
                    }
                    ExcelTableModel table;
                    table.Name = GetString(tableValue, "name");
                    table.Range = GetString(tableValue, "range");
                    sheet.Tables.push_back(std::move(table));
                }
            }
            if (const auto* hyperlinks = GetArray(sheetValue, "hyperlinks"))
            {
                for (const auto& linkValue : *hyperlinks)
                {
                    if (!linkValue.is_object())
                    {
                        continue;
                    }
                    ExcelHyperlinkModel hyperlink;
                    hyperlink.Cell = GetString(linkValue, "cell");
                    hyperlink.Target = GetString(linkValue, "target");
                    hyperlink.Tooltip = GetString(linkValue, "tooltip");
                    sheet.Hyperlinks.push_back(std::move(hyperlink));
                }
            }
            model.Sheets.push_back(std::move(sheet));
        }
    }
    if (model.Sheets.empty())
    {
        Warn(diagnostics, "Excel model has no sheets");
    }
    return model;
}

// --- PowerPoint parsing -----------------------------------------------------

PptTextFrame PptTextFrameFromJson(const Json& array)
{
    PptTextFrame frame;
    for (const auto& paragraphValue : array)
    {
        if (!paragraphValue.is_object())
        {
            continue;
        }
        PptParagraph paragraph;
        paragraph.Level = static_cast<int>(GetInt(paragraphValue, "level"));
        if (const auto* runs = GetArray(paragraphValue, "runs"))
        {
            for (const auto& runValue : *runs)
            {
                if (!runValue.is_object())
                {
                    continue;
                }
                PptRun run;
                run.Text = GetString(runValue, "text");
                run.Bold = GetBool(runValue, "bold");
                run.Italic = GetBool(runValue, "italic");
                run.Underline = GetBool(runValue, "underline");
                run.FontSizePt = GetDouble(runValue, "sizePt");
                run.Color = GetString(runValue, "color");
                run.Hyperlink = GetString(runValue, "link");
                paragraph.Runs.push_back(std::move(run));
            }
        }
        frame.Paragraphs.push_back(std::move(paragraph));
    }
    return frame;
}

PptShape PptShapeFromJson(const Json& object, std::vector<ToolDiagnostic>& diagnostics)
{
    PptShape shape;
    const auto type = GetString(object, "type");
    if (type == "placeholder")
    {
        shape.Kind = PptShape::Type::Placeholder;
    }
    else if (type == "picture")
    {
        shape.Kind = PptShape::Type::Picture;
    }
    else if (type == "table")
    {
        shape.Kind = PptShape::Type::Table;
    }
    else if (type == "group")
    {
        shape.Kind = PptShape::Type::Group;
    }
    else if (type == "other")
    {
        shape.Kind = PptShape::Type::Other;
    }
    else
    {
        shape.Kind = PptShape::Type::TextBox;
    }

    shape.Name = GetString(object, "name");
    shape.PlaceholderType = GetString(object, "placeholder");
    shape.MediaId = GetString(object, "media");
    shape.AltText = GetString(object, "alt");

    if (const auto* transform = GetObject(object, "transform"))
    {
        shape.Transform.Present = true;
        shape.Transform.X = GetInt(*transform, "x");
        shape.Transform.Y = GetInt(*transform, "y");
        shape.Transform.Cx = GetInt(*transform, "cx");
        shape.Transform.Cy = GetInt(*transform, "cy");
    }

    if (const auto* text = GetArray(object, "text"))
    {
        shape.Text = PptTextFrameFromJson(*text);
    }
    if (const auto* rows = GetArray(object, "rows"))
    {
        PptTableModel table;
        for (const auto& rowValue : *rows)
        {
            std::vector<PptTableCell> row;
            const Json* cells = rowValue.is_array() ? &rowValue
                                                    : (rowValue.is_object() ? GetArray(rowValue, "cells") : nullptr);
            if (cells != nullptr)
            {
                for (const auto& cellValue : *cells)
                {
                    PptTableCell cell;
                    if (cellValue.is_object())
                    {
                        cell.Covered = GetBool(cellValue, "covered");
                        cell.RowSpan = static_cast<int>(GetInt(cellValue, "rowSpan", 1));
                        cell.ColSpan = static_cast<int>(GetInt(cellValue, "colSpan", 1));
                        if (const auto* cellText = GetArray(cellValue, "text"))
                        {
                            cell.Text = PptTextFrameFromJson(*cellText);
                        }
                    }
                    row.push_back(std::move(cell));
                }
            }
            table.Rows.push_back(std::move(row));
        }
        shape.Table = std::move(table);
    }
    if (const auto* children = GetArray(object, "children"))
    {
        for (const auto& childValue : *children)
        {
            if (childValue.is_object())
            {
                shape.Children.push_back(PptShapeFromJson(childValue, diagnostics));
            }
        }
    }
    return shape;
}

PowerPointDeckModel PowerPointModelFromJson(const Json& object, std::vector<ToolDiagnostic>& diagnostics)
{
    PowerPointDeckModel model;
    if (const auto* slides = GetArray(object, "slides"))
    {
        for (const auto& slideValue : *slides)
        {
            if (!slideValue.is_object())
            {
                continue;
            }
            PptSlide slide;
            slide.LayoutName = GetString(slideValue, "layout");
            slide.Hidden = GetBool(slideValue, "hidden");
            slide.NotesText = GetString(slideValue, "notes");
            if (const auto* shapes = GetArray(slideValue, "shapes"))
            {
                for (const auto& shapeValue : *shapes)
                {
                    if (shapeValue.is_object())
                    {
                        slide.Shapes.push_back(PptShapeFromJson(shapeValue, diagnostics));
                    }
                }
            }
            if (const auto* comments = GetArray(slideValue, "comments"))
            {
                for (const auto& commentValue : *comments)
                {
                    if (!commentValue.is_object())
                    {
                        continue;
                    }
                    PptCommentModel comment;
                    comment.Author = GetString(commentValue, "author");
                    comment.Text = GetString(commentValue, "text");
                    slide.Comments.push_back(std::move(comment));
                }
            }
            model.Slides.push_back(std::move(slide));
        }
    }
    return model;
}

} // namespace

// --- Shared tree builders ---------------------------------------------------

std::string EncodeBase64(const std::vector<Byte>& bytes)
{
    static constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((bytes.size() + 2) / 3 * 4);
    for (Size i = 0; i < bytes.size(); i += 3)
    {
        const unsigned value = (static_cast<unsigned>(bytes[i]) << 16) |
                               (i + 1 < bytes.size() ? static_cast<unsigned>(bytes[i + 1]) << 8 : 0) |
                               (i + 2 < bytes.size() ? bytes[i + 2] : 0);
        out.push_back(alphabet[(value >> 18) & 63]);
        out.push_back(alphabet[(value >> 12) & 63]);
        out.push_back(i + 1 < bytes.size() ? alphabet[(value >> 6) & 63] : '=');
        out.push_back(i + 2 < bytes.size() ? alphabet[value & 63] : '=');
    }
    return out;
}

bool DecodeBase64(std::string_view text, std::vector<Byte>& out)
{
    int value = 0;
    int bits = -8;
    bool padding = false;
    for (const char rawCh : text)
    {
        const auto ch = static_cast<unsigned char>(rawCh);
        if (std::isspace(ch) != 0)
        {
            continue;
        }
        if (ch == '=')
        {
            padding = true;
            continue;
        }
        if (padding)
        {
            return false;
        }
        const int digit = ch >= 'A' && ch <= 'Z'   ? ch - 'A'
                          : ch >= 'a' && ch <= 'z' ? ch - 'a' + 26
                          : ch >= '0' && ch <= '9' ? ch - '0' + 52
                          : ch == '+'              ? 62
                          : ch == '/'              ? 63
                                                   : -1;
        if (digit < 0)
        {
            return false;
        }
        value = (value << 6) | digit;
        bits += 6;
        if (bits >= 0)
        {
            out.push_back(static_cast<UInt8>((value >> bits) & 255));
            bits -= 8;
        }
    }
    return true;
}

Json BuildModelTree(const DocumentModel& model, bool embedMedia)
{
    Json root = Json::object();
    root["format"] = "exyokioffice-document";
    root["version"] = DocumentModelVersion;
    root["family"] = std::string(ToString(model.Family));

    Json properties = Json::object();
    SetIfNotEmpty(properties, "title", model.Properties.Title);
    SetIfNotEmpty(properties, "subject", model.Properties.Subject);
    SetIfNotEmpty(properties, "creator", model.Properties.Creator);
    SetIfNotEmpty(properties, "keywords", model.Properties.Keywords);
    SetIfNotEmpty(properties, "description", model.Properties.Description);
    SetIfNotEmpty(properties, "lastModifiedBy", model.Properties.LastModifiedBy);
    SetIfNotEmpty(properties, "category", model.Properties.Category);
    SetIfNotEmpty(properties, "created", model.Properties.Created);
    SetIfNotEmpty(properties, "modified", model.Properties.Modified);
    SetIfNotEmpty(properties, "application", model.Properties.Application);
    SetIfNotEmpty(properties, "company", model.Properties.Company);
    if (!properties.empty())
    {
        root["properties"] = std::move(properties);
    }

    if (!model.Media.empty())
    {
        Json media = Json::array();
        for (const auto& item : model.Media)
        {
            Json itemObject = Json::object();
            itemObject["id"] = item.Id;
            SetIfNotEmpty(itemObject, "file", item.FileName);
            SetIfNotEmpty(itemObject, "contentType", item.ContentType);
            if (embedMedia && !item.Data.empty())
            {
                itemObject["data"] = EncodeBase64(item.Data);
            }
            media.push_back(std::move(itemObject));
        }
        root["media"] = std::move(media);
    }

    switch (model.Family)
    {
        case DocumentFamily::Word:
            root["document"] = model.Word ? WordModelToJson(*model.Word) : Json::object();
            break;
        case DocumentFamily::Excel:
            root["document"] = model.Excel ? ExcelModelToJson(*model.Excel) : Json::object();
            break;
        case DocumentFamily::PowerPoint:
            root["document"] = model.PowerPoint ? PowerPointModelToJson(*model.PowerPoint) : Json::object();
            break;
        case DocumentFamily::Unknown:
            root["document"] = Json::object();
            break;
    }
    return root;
}

DocumentModel ParseModelTree(const Json& tree, std::vector<ToolDiagnostic>& diagnostics)
{
    DocumentModel model;
    if (!tree.is_object())
    {
        diagnostics.push_back(ToolDiagnostic{ToolSeverity::Error, "Envelope is not an object"});
        return model;
    }

    const auto format = GetString(tree, "format");
    if (!format.empty() && format != "exyokioffice-document")
    {
        Warn(diagnostics, "Unexpected envelope format identifier", format);
    }
    const auto version = GetInt(tree, "version", DocumentModelVersion);
    if (version > DocumentModelVersion)
    {
        Warn(diagnostics, "Envelope version is newer than this tool; parsing best-effort",
             std::to_string(version));
    }

    const auto family = GetString(tree, "family");
    if (family == "word")
    {
        model.Family = DocumentFamily::Word;
    }
    else if (family == "excel")
    {
        model.Family = DocumentFamily::Excel;
    }
    else if (family == "powerpoint")
    {
        model.Family = DocumentFamily::PowerPoint;
    }
    else
    {
        diagnostics.push_back(ToolDiagnostic{ToolSeverity::Error, "Envelope has no recognizable family", family});
        return model;
    }

    if (const auto* properties = GetObject(tree, "properties"))
    {
        model.Properties.Title = GetString(*properties, "title");
        model.Properties.Subject = GetString(*properties, "subject");
        model.Properties.Creator = GetString(*properties, "creator");
        model.Properties.Keywords = GetString(*properties, "keywords");
        model.Properties.Description = GetString(*properties, "description");
        model.Properties.LastModifiedBy = GetString(*properties, "lastModifiedBy");
        model.Properties.Category = GetString(*properties, "category");
        model.Properties.Created = GetString(*properties, "created");
        model.Properties.Modified = GetString(*properties, "modified");
        model.Properties.Application = GetString(*properties, "application");
        model.Properties.Company = GetString(*properties, "company");
    }

    if (const auto* media = GetArray(tree, "media"))
    {
        for (const auto& itemValue : *media)
        {
            if (!itemValue.is_object())
            {
                continue;
            }
            MediaReference item;
            item.Id = GetString(itemValue, "id");
            item.FileName = GetString(itemValue, "file");
            item.ContentType = GetString(itemValue, "contentType");
            const auto data = GetString(itemValue, "data");
            if (!data.empty() && !DecodeBase64(data, item.Data))
            {
                Warn(diagnostics, "Invalid base64 media data", item.Id);
                item.Data.clear();
            }
            model.Media.push_back(std::move(item));
        }
    }

    const auto* document = GetObject(tree, "document");
    if (document == nullptr)
    {
        Warn(diagnostics, "Envelope has no document payload");
        static const Json empty = Json::object();
        document = &empty;
    }

    switch (model.Family)
    {
        case DocumentFamily::Word:
            model.Word = WordModelFromJson(*document, diagnostics);
            break;
        case DocumentFamily::Excel:
            model.Excel = ExcelModelFromJson(*document, diagnostics);
            break;
        case DocumentFamily::PowerPoint:
            model.PowerPoint = PowerPointModelFromJson(*document, diagnostics);
            break;
        case DocumentFamily::Unknown:
            break;
    }
    return model;
}

} // namespace detail

std::string SerializeModelJson(const DocumentModel& model, bool embedMedia)
{
    return detail::BuildModelTree(model, embedMedia).dump(2) + "\n";
}

DocumentModel ParseModelJson(std::string_view json, std::vector<ToolDiagnostic>& diagnostics)
{
    const auto parsed = nlohmann::ordered_json::parse(json, nullptr, false);
    if (parsed.is_discarded())
    {
        DocumentModel model;
        diagnostics.push_back(ToolDiagnostic{ToolSeverity::Error, "Input is not valid JSON"});
        return model;
    }
    return detail::ParseModelTree(parsed, diagnostics);
}

} // namespace ExyokiOffice::Tools
