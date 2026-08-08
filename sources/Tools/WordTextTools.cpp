// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "ExyokiOffice/Tools/WordTextTools.hpp"
#include "ExyokiOffice/Tools/PackageLimits.hpp"

#include "ExyokiOffice/OpenXmlPackage.hpp"
#include "ExyokiOffice/Tools/PackageInspector.hpp"
#include "ExyokiOffice/Word/WordDocument.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <functional>
#include <optional>
#include <regex>
#include <unordered_set>

namespace ExyokiOffice::Tools
{

std::string_view ToString(TextScope scope) noexcept
{
    switch (scope)
    {
        case TextScope::Body:
            return "body";
        case TextScope::Table:
            return "table";
        case TextScope::Header:
            return "header";
        case TextScope::Footer:
            return "footer";
        case TextScope::Footnote:
            return "footnote";
        case TextScope::Endnote:
            return "endnote";
        case TextScope::Comment:
            return "comment";
    }
    return "body";
}

namespace
{

using ExyokiOffice::Word::HeaderFooterType;
using ExyokiOffice::Word::Paragraph;
using ExyokiOffice::Word::WordDocumentEditor;

std::string_view HeaderFooterTypeName(HeaderFooterType type)
{
    switch (type)
    {
        case HeaderFooterType::Default:
            return "default";
        case HeaderFooterType::Even:
            return "even";
        case HeaderFooterType::First:
            return "first";
    }
    return "default";
}

/// Escapes ECMAScript regex metacharacters so a literal needle can be matched via std::regex.
std::string EscapeRegexLiteral(std::string_view text)
{
    static constexpr std::string_view special = "\\^$.|?*+()[]{}";
    std::string escaped;
    escaped.reserve(text.size());
    for (const char c : text)
    {
        if (special.find(c) != std::string_view::npos)
        {
            escaped += '\\';
        }
        escaped += c;
    }
    return escaped;
}

/// Builds a compiled pattern for regex or case-insensitive matching, or std::nullopt when
/// neither is requested (callers then take the plain substring code path). On an invalid
/// regex, appends a diagnostic and returns std::nullopt as well; distinguish the two cases
/// via useRegex/ignoreCase at the call site.
std::optional<std::regex> BuildPattern(std::string_view needle, bool useRegex, bool ignoreCase,
                                       std::vector<ToolDiagnostic>& diagnostics)
{
    if (!useRegex && !ignoreCase)
    {
        return std::nullopt;
    }

    const std::string patternText = useRegex ? std::string(needle) : EscapeRegexLiteral(needle);
    auto flags = std::regex::ECMAScript;
    if (ignoreCase)
    {
        flags |= std::regex::icase;
    }

    try
    {
        return std::regex(patternText, flags);
    }
    catch (const std::regex_error& error)
    {
        diagnostics.push_back(ToolDiagnostic{ToolSeverity::Error, "Invalid regular expression", error.what()});
        return std::nullopt;
    }
}

bool IsWordFamily(const std::filesystem::path& path, std::vector<ToolDiagnostic>& diagnostics)
{
    OpenXmlPackage package;
    ApplyDefaultPackageLimits(package);
    if (!package.LoadFromFile(path))
    {
        diagnostics.push_back(ToolDiagnostic{ToolSeverity::Error, "Failed to open package", path.string()});
        return false;
    }
    const auto info = GetInfo(package);
    if (info.Family != DocumentFamily::Word)
    {
        diagnostics.push_back(
            ToolDiagnostic{ToolSeverity::Error, "Not a Word document", std::string(ToString(info.Family))});
        return false;
    }
    return true;
}

/**
 * @brief Visits every paragraph reachable from a Word document: body, tables
 * (including nested tables), headers/footers of every section, footnotes,
 * endnotes, and comments.
 */
void ForEachParagraph(WordDocumentEditor& editor,
                      const std::function<void(TextScope, std::string, const std::shared_ptr<Paragraph>&)>& visit)
{
    Size index = 1;
    for (const auto& paragraph : editor.Paragraphs())
    {
        visit(TextScope::Body, "body paragraph " + std::to_string(index), paragraph);
        ++index;
    }

    Size tableIndex = 1;
    for (const auto& table : editor.Tables())
    {
        Size paragraphIndex = 1;
        for (const auto& paragraph : table->Paragraphs())
        {
            visit(TextScope::Table,
                  "table " + std::to_string(tableIndex) + " paragraph " + std::to_string(paragraphIndex), paragraph);
            ++paragraphIndex;
        }
        ++tableIndex;
    }

    std::unordered_set<const void*> seenHeaders;
    std::unordered_set<const void*> seenFooters;
    for (const auto& section : editor.Sections())
    {
        for (auto type : {HeaderFooterType::Default, HeaderFooterType::Even, HeaderFooterType::First})
        {
            if (auto header = section->GetHeader(type))
            {
                if (seenHeaders.insert(header->GetHeaderPart().get()).second)
                {
                    Size paragraphIndex = 1;
                    for (const auto& paragraph : header->Paragraphs())
                    {
                        visit(TextScope::Header,
                              "header (" + std::string(HeaderFooterTypeName(type)) + ") paragraph " +
                                  std::to_string(paragraphIndex),
                              paragraph);
                        ++paragraphIndex;
                    }
                }
            }
            if (auto footer = section->GetFooter(type))
            {
                if (seenFooters.insert(footer->GetFooterPart().get()).second)
                {
                    Size paragraphIndex = 1;
                    for (const auto& paragraph : footer->Paragraphs())
                    {
                        visit(TextScope::Footer,
                              "footer (" + std::string(HeaderFooterTypeName(type)) + ") paragraph " +
                                  std::to_string(paragraphIndex),
                              paragraph);
                        ++paragraphIndex;
                    }
                }
            }
        }
    }

    for (const auto& note : editor.Footnotes())
    {
        Size paragraphIndex = 1;
        for (const auto& paragraph : note->Paragraphs())
        {
            visit(TextScope::Footnote, "footnote " + std::to_string(note->GetId()) + " paragraph " + std::to_string(paragraphIndex),
                  paragraph);
            ++paragraphIndex;
        }
    }

    for (const auto& note : editor.Endnotes())
    {
        Size paragraphIndex = 1;
        for (const auto& paragraph : note->Paragraphs())
        {
            visit(TextScope::Endnote, "endnote " + std::to_string(note->GetId()) + " paragraph " + std::to_string(paragraphIndex),
                  paragraph);
            ++paragraphIndex;
        }
    }

    for (const auto& comment : editor.Comments())
    {
        Size paragraphIndex = 1;
        for (const auto& paragraph : comment->Paragraphs())
        {
            visit(TextScope::Comment,
                  "comment " + std::to_string(comment->GetId()) + " (" + comment->GetAuthor() + ") paragraph " +
                      std::to_string(paragraphIndex),
                  paragraph);
            ++paragraphIndex;
        }
    }
}

} // namespace

SearchResult Search(const std::filesystem::path& docxPath, std::string_view needle, Size contextChars,
                    bool useRegex, bool ignoreCase)
{
    SearchResult result;
    if (!IsWordFamily(docxPath, result.Diagnostics))
    {
        return result;
    }

    auto editor = WordDocumentEditor::Open(docxPath, UntrustedOpenSettings());
    if (!editor)
    {
        result.Diagnostics.push_back(ToolDiagnostic{ToolSeverity::Error, "Failed to open Word document",
                                                    docxPath.string()});
        return result;
    }

    auto opened = Search(*editor, needle, contextChars, useRegex, ignoreCase);
    opened.Diagnostics.insert(opened.Diagnostics.begin(), result.Diagnostics.begin(), result.Diagnostics.end());
    return opened;
}

SearchResult Search(Word::WordDocumentEditor& editor, std::string_view needle, Size contextChars, bool useRegex,
                    bool ignoreCase)
{
    SearchResult result;

    const auto pattern = BuildPattern(needle, useRegex, ignoreCase, result.Diagnostics);
    if ((useRegex || ignoreCase) && !pattern)
    {
        return result;
    }

    ForEachParagraph(editor,
                     [&](TextScope scope, std::string label, const std::shared_ptr<Paragraph>& paragraph)
                     {
                         const auto text = paragraph->PlainText();
                         const auto ranges = pattern ? paragraph->FindAllRegex(*pattern) : paragraph->FindAll(needle);
                         for (const auto& range : ranges)
                         {
                             SearchMatch match;
                             match.Scope = scope;
                             match.Label = label;
                             match.Offset = range.Start;
                             match.Length = range.Length();
                             match.MatchText = text.substr(range.Start, range.Length());

                             const auto contextStart = range.Start > contextChars ? range.Start - contextChars : 0;
                             const auto contextEnd = std::min(text.size(), range.End + contextChars);
                             match.Context = text.substr(contextStart, contextEnd - contextStart);

                             result.Matches.push_back(std::move(match));
                         }
                     });

    result.Ok = true;
    return result;
}

ExtractTextResult ExtractText(const std::filesystem::path& docxPath)
{
    ExtractTextResult result;
    if (!IsWordFamily(docxPath, result.Diagnostics))
    {
        return result;
    }

    auto editor = WordDocumentEditor::Open(docxPath, UntrustedOpenSettings());
    if (!editor)
    {
        result.Diagnostics.push_back(ToolDiagnostic{ToolSeverity::Error, "Failed to open Word document",
                                                    docxPath.string()});
        return result;
    }

    return ExtractText(*editor);
}

ExtractTextResult ExtractText(Word::WordDocumentEditor& editor)
{
    ExtractTextResult result;

    ForEachParagraph(editor, [&](TextScope scope, std::string label, const std::shared_ptr<Paragraph>& paragraph)
                     { result.Blocks.push_back(TextBlock{scope, std::move(label), paragraph->PlainText()}); });

    result.Ok = true;
    return result;
}

ReplaceResult Replace(const std::filesystem::path& docxPath, std::string_view needle, std::string_view replacement,
                      bool dryRun, const std::filesystem::path& outputPath, bool useRegex, bool ignoreCase)
{
    ReplaceResult result;
    if (!IsWordFamily(docxPath, result.Diagnostics))
    {
        return result;
    }

    auto editor = WordDocumentEditor::Open(docxPath, UntrustedOpenSettings());
    if (!editor)
    {
        result.Diagnostics.push_back(ToolDiagnostic{ToolSeverity::Error, "Failed to open Word document",
                                                    docxPath.string()});
        return result;
    }

    result = Replace(*editor, needle, replacement, dryRun, useRegex, ignoreCase);
    if (!result.Ok || dryRun)
    {
        return result;
    }

    const auto destination = outputPath.empty() ? docxPath : outputPath;
    result.Saved = editor->SaveToFile(destination);
    if (!result.Saved)
    {
        result.Diagnostics.push_back(
            ToolDiagnostic{ToolSeverity::Error, "Failed to save document", destination.string()});
    }
    return result;
}

ReplaceResult Replace(Word::WordDocumentEditor& editor, std::string_view needle, std::string_view replacement,
                      bool dryRun, bool useRegex, bool ignoreCase)
{
    ReplaceResult result;

    const auto pattern = BuildPattern(needle, useRegex, ignoreCase, result.Diagnostics);
    if ((useRegex || ignoreCase) && !pattern)
    {
        return result;
    }

    Size count = 0;
    ForEachParagraph(editor,
                     [&](TextScope, std::string, const std::shared_ptr<Paragraph>& paragraph)
                     {
                         if (dryRun)
                         {
                             count += pattern ? paragraph->FindAllRegex(*pattern).size()
                                              : paragraph->FindAll(needle).size();
                         }
                         else
                         {
                             count += pattern ? paragraph->ReplaceAllRegex(*pattern, replacement)
                                              : paragraph->ReplaceAll(needle, replacement);
                         }
                     });

    result.ReplacementCount = count;
    result.Ok = true;
    return result;
}

} // namespace ExyokiOffice::Tools
