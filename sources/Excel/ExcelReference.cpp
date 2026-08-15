// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "ExyokiOffice/Excel/ExcelReference.hpp"
#include "ExyokiOffice/StandardTypes.hpp"
#include "AsciiText.hpp"

#include <algorithm>
#include <optional>

namespace ExyokiOffice::Excel
{

FormulaReferenceTransform
FormulaReferenceTransform::InsertRows(UInt32 index,
                                      UInt32 count)
{
    return {ReferenceTransformKind::InsertRows, index, count, {}, {}};
}
FormulaReferenceTransform
FormulaReferenceTransform::DeleteRows(UInt32 index,
                                      UInt32 count)
{
    return {ReferenceTransformKind::DeleteRows, index, count, {}, {}};
}
FormulaReferenceTransform
FormulaReferenceTransform::InsertColumns(UInt32 index,
                                         UInt32 count)
{
    return {ReferenceTransformKind::InsertColumns, index, count, {}, {}};
}
FormulaReferenceTransform
FormulaReferenceTransform::DeleteColumns(UInt32 index,
                                         UInt32 count)
{
    return {ReferenceTransformKind::DeleteColumns, index, count, {}, {}};
}
FormulaReferenceTransform
FormulaReferenceTransform::MoveRange(CellRange source,
                                     CellAddress destination)
{
    return {ReferenceTransformKind::MoveRange, 0, 0, source, destination};
}

class FormulaReferenceImplementation final
{
public:
    struct Cell
    {
        UInt32 row{}, column{};
        bool absRow{}, absColumn{};
        Size length{};
    };
    struct Token
    {
        Size start{}, length{};
        std::string qualifier;
        bool external{};
        Cell first;
        std::optional<Cell> last;
    };

    static FormulaReferenceRewriteResult
    Rewrite(std::string_view formula, std::string_view sheet,
            const FormulaReferenceTransform& transform)
    {
        FormulaReferenceRewriteResult result;
        if (!Validate(transform, result.ErrorMessage))
        {
            result.Succeeded = false;
            return result;
        }
        result.Formula.reserve(formula.size());
        for (Size cursor = 0; cursor < formula.size();)
        {
            if (formula[cursor] == '"')
            {
                CopyString(formula, cursor, result.Formula);
                continue;
            }
            const auto token = ParseToken(formula, cursor);
            if (!token)
            {
                result.Formula.push_back(formula[cursor++]);
                continue;
            }
            const auto original = formula.substr(token->start, token->length);
            if (token->external)
            {
                result.Diagnostics.push_back(
                    {cursor, std::string(original),
                     "External workbook reference was preserved because its target "
                     "grid is not owned by this worksheet."});
                result.Formula += original;
                cursor += token->length;
                continue;
            }
            if (!token->qualifier.empty() && !EqualSheet(token->qualifier, sheet))
            {
                result.Formula += original;
                cursor += token->length;
                continue;
            }
            auto rewritten = RewriteToken(*token, transform);
            if (!rewritten)
            {
                result.Succeeded = false;
                result.ErrorMessage =
                    "The reference transformation exceeds the Excel worksheet grid.";
                return result;
            }
            result.Formula += rewritten->empty() ? "#REF!" : *rewritten;
            if (result.Formula.substr(result.Formula.size() -
                                      (rewritten->empty() ? 5 : rewritten->size())) !=
                original)
            {
                ++result.RewrittenReferenceCount;
            }
            cursor += token->length;
        }
        return result;
    }

private:
    static bool Validate(const FormulaReferenceTransform& t, std::string& error)
    {
        if (t.Kind == ReferenceTransformKind::MoveRange)
        {
            if (!t.Source.IsValid() || !t.DestinationTopLeft.IsValid())
            {
                error = "The move source and destination must be valid.";
                return false;
            }
            const auto lastRow = UInt64(t.DestinationTopLeft.Row().Value()) +
                                 t.Source.RowCount() - 1;
            const auto lastCol =
                UInt64(t.DestinationTopLeft.Column().Value()) +
                t.Source.ColumnCount() - 1;
            if (lastRow > MaxRowIndex || lastCol > MaxColumnIndex)
            {
                error = "The move destination exceeds the Excel worksheet grid.";
                return false;
            }
            return true;
        }
        const bool rows = t.Kind == ReferenceTransformKind::InsertRows ||
                          t.Kind == ReferenceTransformKind::DeleteRows;
        const auto max = rows ? MaxRowIndex : MaxColumnIndex;
        if (!t.Index || t.Index > max || !t.Count ||
            ((t.Kind == ReferenceTransformKind::DeleteRows ||
              t.Kind == ReferenceTransformKind::DeleteColumns) &&
             UInt64(t.Index) + t.Count - 1 > max))
        {
            error = "The insertion or deletion interval is outside the Excel "
                    "worksheet grid.";
            return false;
        }
        return true;
    }
    static void CopyString(std::string_view text, Size& cursor,
                           std::string& out)
    {
        const auto start = cursor++;
        while (cursor < text.size())
        {
            if (text[cursor++] == '"')
            {
                if (cursor < text.size() && text[cursor] == '"')
                {
                    ++cursor;
                    continue;
                }
                break;
            }
        }
        out += text.substr(start, cursor - start);
    }
    /// True when @p c cannot be part of an identifier, so a match beside it is a whole token.
    static bool Boundary(char c)
    {
        // A byte of a UTF-8 sequence is part of the identifier around it. Were
        // it a boundary, the `A1` inside a defined name such as `cA1` would be
        // taken for a cell reference and rewritten.
        return !AsciiText::IsAlnum(c) && !AsciiText::IsNonAscii(c) && c != '_' && c != '.';
    }
    static std::optional<Cell> ParseCell(std::string_view text, Size at)
    {
        Cell c;
        auto p = at;
        if (p < text.size() && text[p] == '$')
        {
            c.absColumn = true;
            ++p;
        }
        const auto cs = p;
        while (p < text.size() && AsciiText::IsAlpha(text[p]))
        {
            ++p;
        }
        if (p == cs || p - cs > 3)
        {
            return std::nullopt;
        }
        const auto col = ColumnIndex::ParseName(text.substr(cs, p - cs));
        if (!col)
        {
            return std::nullopt;
        }
        if (p < text.size() && text[p] == '$')
        {
            c.absRow = true;
            ++p;
        }
        const auto rs = p;
        UInt64 row = 0;
        while (p < text.size() &&
               AsciiText::IsDigit(text[p]))
        {
            row = row * 10 + static_cast<UInt64>(text[p++] - '0');
            if (row > MaxRowIndex)
            {
                return std::nullopt;
            }
        }
        if (p == rs || !row)
        {
            return std::nullopt;
        }
        c.row = static_cast<UInt32>(row);
        c.column = col->Value();
        c.length = p - at;
        return c;
    }
    static std::optional<Token> ParseToken(std::string_view text,
                                           Size at)
    {
        if (at && ((!Boundary(text[at - 1]) && text[at - 1] != '!') ||
                   text[at - 1] == '['))
        {
            return std::nullopt;
        }
        Token t;
        t.start = at;
        auto cellAt = at;
        auto bang = text.find('!', at);
        if (bang != std::string_view::npos)
        {
            bool qualifier = false;
            if (text[at] == '\'')
            {
                auto p = at + 1;
                while (p < text.size())
                {
                    if (text[p++] == '\'')
                    {
                        if (p < text.size() && text[p] == '\'')
                        {
                            ++p;
                            continue;
                        }
                        qualifier = p == bang;
                        break;
                    }
                }
            }
            else
            {
                qualifier = bang > at && bang - at < 256 &&
                            std::all_of(
                                text.begin() + static_cast<std::string_view::difference_type>(at),
                                text.begin() + static_cast<std::string_view::difference_type>(bang),
                                [](char c)
                                {
                                    return !AsciiText::IsSpace(c) &&
                                           c != '+' && c != '-' && c != '*' && c != '/';
                                });
            }
            if (qualifier)
            {
                t.qualifier = std::string(text.substr(at, bang - at));
                t.external = t.qualifier.find('[') != std::string::npos;
                cellAt = bang + 1;
            }
        }
        const auto first = ParseCell(text, cellAt);
        if (!first)
        {
            return std::nullopt;
        }
        t.first = *first;
        auto end = cellAt + first->length;
        if (end < text.size() && text[end] == ':')
        {
            auto second = ParseCell(text, end + 1);
            if (second)
            {
                t.last = *second;
                end += 1 + second->length;
            }
        }
        if (end < text.size() &&
            (text[end] == '(' || (!Boundary(text[end]) && text[end] != ')' &&
                                  text[end] != ',' && text[end] != ';')))
        {
            return std::nullopt;
        }
        t.length = end - at;
        return t;
    }
    static bool EqualSheet(std::string qualifier, std::string_view sheet)
    {
        if (!qualifier.empty() && qualifier.front() == '\'' &&
            qualifier.back() == '\'')
        {
            qualifier = qualifier.substr(1, qualifier.size() - 2);
            for (Size p = 0;
                 (p = qualifier.find("''", p)) != std::string::npos;)
            {
                qualifier.erase(p, 1);
            }
        }
        return AsciiText::EqualsIgnoreCase(qualifier, sheet);
    }
    static std::string Format(const Cell& c)
    {
        return std::string(c.absColumn ? "$" : "") +
               ColumnIndex(c.column).ToName() + (c.absRow ? "$" : "") +
               std::to_string(c.row);
    }
    static std::optional<bool>
    RewriteCoordinate(UInt32& v, bool row,
                      const FormulaReferenceTransform& t)
    {
        const bool relevant =
            row ? t.Kind == ReferenceTransformKind::InsertRows ||
                      t.Kind == ReferenceTransformKind::DeleteRows
                : t.Kind == ReferenceTransformKind::InsertColumns ||
                      t.Kind == ReferenceTransformKind::DeleteColumns;
        if (!relevant)
        {
            return false;
        }
        const bool insert = t.Kind == ReferenceTransformKind::InsertRows ||
                            t.Kind == ReferenceTransformKind::InsertColumns;
        if (insert)
        {
            if (v < t.Index)
            {
                return false;
            }
            auto n = UInt64(v) + t.Count;
            if (n > (row ? MaxRowIndex : MaxColumnIndex))
            {
                return std::nullopt;
            }
            v = static_cast<UInt32>(n);
            return false;
        }
        const auto last = t.Index + t.Count - 1;
        if (v < t.Index)
        {
            return false;
        }
        if (v <= last)
        {
            return true;
        }
        v -= t.Count;
        return false;
    }
    static std::optional<std::string>
    RewriteToken(const Token& token, const FormulaReferenceTransform& t)
    {
        auto a = token.first;
        auto b = token.last;
        bool removed = false;
        if (t.Kind == ReferenceTransformKind::MoveRange)
        {
            const auto move = [&](Cell& c)
            {
                if (c.row >= t.Source.First().Row().Value() &&
                    c.row <= t.Source.Last().Row().Value() &&
                    c.column >= t.Source.First().Column().Value() &&
                    c.column <= t.Source.Last().Column().Value())
                {
                    c.row = t.DestinationTopLeft.Row().Value() + c.row -
                            t.Source.First().Row().Value();
                    c.column = t.DestinationTopLeft.Column().Value() + c.column -
                               t.Source.First().Column().Value();
                }
            };
            move(a);
            if (b)
            {
                move(*b);
            }
        }
        else if (!b)
        {
            auto r =
                RewriteCoordinate(t.Kind == ReferenceTransformKind::InsertRows ||
                                          t.Kind == ReferenceTransformKind::DeleteRows
                                      ? a.row
                                      : a.column,
                                  t.Kind == ReferenceTransformKind::InsertRows ||
                                      t.Kind == ReferenceTransformKind::DeleteRows,
                                  t);
            if (!r)
            {
                return std::nullopt;
            }
            removed = *r;
        }
        else
        {
            const bool row = t.Kind == ReferenceTransformKind::InsertRows ||
                             t.Kind == ReferenceTransformKind::DeleteRows;
            auto& start = row ? a.row : a.column;
            auto& finish = row ? b->row : b->column;
            const bool insert = t.Kind == ReferenceTransformKind::InsertRows ||
                                t.Kind == ReferenceTransformKind::InsertColumns;
            if (insert)
            {
                auto r1 = RewriteCoordinate(start, row, t),
                     r2 = RewriteCoordinate(finish, row, t);
                if (!r1 || !r2)
                {
                    return std::nullopt;
                }
            }
            else
            {
                auto dl = t.Index + t.Count - 1;
                if (finish < t.Index)
                {
                }
                else if (start > dl)
                {
                    start -= t.Count;
                    finish -= t.Count;
                }
                else
                {
                    auto ns = start < t.Index ? start : t.Index;
                    auto nf = finish > dl ? finish - t.Count : t.Index - 1;
                    if (!ns || ns > nf)
                    {
                        removed = true;
                    }
                    else
                    {
                        start = ns;
                        finish = nf;
                    }
                }
            }
        }
        if (removed)
        {
            return std::string{};
        }
        return token.qualifier.empty() ? Format(a) + (b ? ":" + Format(*b) : "")
                                       : token.qualifier + "!" + Format(a) +
                                             (b ? ":" + Format(*b) : "");
    }
};

FormulaReferenceRewriteResult FormulaReferenceRewriter::RewriteA1(
    std::string_view formula, std::string_view sheet,
    const FormulaReferenceTransform& transform)
{
    return FormulaReferenceImplementation::Rewrite(formula, sheet, transform);
}

} // namespace ExyokiOffice::Excel
