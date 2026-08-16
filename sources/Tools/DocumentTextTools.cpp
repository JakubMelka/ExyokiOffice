// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "ExyokiOffice/Tools/DocumentTextTools.hpp"
#include "ExyokiOffice/Tools/PackageLimits.hpp"

#include "ExyokiOffice/Excel/ExcelDocument.hpp"
#include "ExyokiOffice/OpenXmlPackage.hpp"
#include "ExyokiOffice/PowerPoint/PowerPointDocument.hpp"
#include "ExyokiOffice/Word/WordDocument.hpp"
#include "ExyokiOffice/Tools/PackageInspector.hpp"
#include "ExyokiOffice/Tools/WordTextTools.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include "Tools/TextPatternHelpers.hpp"

#include <algorithm>
#include <optional>
#include <regex>
#include <string>

namespace ExyokiOffice::Tools
{

/// File-local pattern and range helpers for the document text tools.
class DocumentTextToolsHelper
{
public:
    /// One located match with its already-expanded replacement text.
    struct LocatedMatch
    {
        Size Offset = 0;
        Size Length = 0;
        std::string Replacement;
    };

    /// Finds every non-overlapping match of the needle/pattern in one text unit,
    /// expanding the replacement per match (capture groups for regex needles).
    static std::vector<LocatedMatch> FindMatches(const std::string& text, std::string_view needle,
                                                 const std::optional<std::regex>& pattern, bool useRegex,
                                                 std::string_view replacement)
    {
        std::vector<LocatedMatch> matches;
        if (pattern)
        {
            if (!Detail::IsSearchableSubject(text))
            {
                // The one place these tools hand text to the regex engine, and
                // therefore the one place the subject limit has to hold. Every
                // Excel cell, PowerPoint shape, table cell and notes page
                // arrives here, and a limit the Word API enforces but the tools
                // do not is a limit an attacker picks the other frontend to
                // avoid. See RegexPattern::MaximumSubjectLength.
                return matches;
            }

            const std::string format = useRegex ? std::string(replacement) : TextPattern::EscapeFormatLiteral(replacement);
            for (auto it = std::sregex_iterator(text.begin(), text.end(), *pattern); it != std::sregex_iterator(); ++it)
            {
                if (it->length(0) == 0)
                {
                    // Zero-width matches would replace nothing and can repeat at
                    // the same position; skip them instead of looping.
                    continue;
                }
                LocatedMatch match;
                match.Offset = static_cast<Size>(it->position(0));
                match.Length = static_cast<Size>(it->length(0));
                match.Replacement = it->format(format);
                matches.push_back(std::move(match));
            }
            return matches;
        }

        if (needle.empty())
        {
            return matches;
        }
        Size searchStart = 0;
        while (true)
        {
            const auto found = text.find(needle, searchStart);
            if (found == std::string::npos)
            {
                break;
            }
            matches.push_back(LocatedMatch{static_cast<Size>(found), needle.size(), std::string(replacement)});
            searchStart = found + needle.size();
        }
        return matches;
    }

    static std::string MakeContext(const std::string& text, Size offset, Size length, Size contextChars)
    {
        const Size begin = offset > contextChars ? offset - contextChars : 0;
        const Size end = std::min<Size>(text.size(), offset + length + contextChars);
        return text.substr(begin, end - begin);
    }

    /// Applies located matches to one text unit (matches must be in ascending offset order).
    static std::string ApplyMatches(const std::string& text, const std::vector<LocatedMatch>& matches)
    {
        std::string output;
        output.reserve(text.size());
        Size position = 0;
        for (const auto& match : matches)
        {
            output.append(text, position, match.Offset - position);
            output.append(match.Replacement);
            position = match.Offset + match.Length;
        }
        output.append(text, position, text.size() - position);
        return output;
    }

    /// Family detection shared by search and replace.
    static DocumentFamily DetectFamily(const std::filesystem::path& path, std::vector<ToolDiagnostic>& diagnostics)
    {
        OpenXmlPackage package;
        ApplyDefaultPackageLimits(package);
        if (!package.LoadFromFile(path))
        {
            diagnostics.push_back(ToolDiagnostic{ToolSeverity::Error, "Failed to open package", path.string()});
            return DocumentFamily::Unknown;
        }
        const auto info = GetInfo(package);
        if (info.Family == DocumentFamily::Unknown)
        {
            diagnostics.push_back(ToolDiagnostic{ToolSeverity::Error, DescribeUnknownFamily(info), path.string()});
        }
        return info.Family;
    }

    /// Resolves the display text of one Excel cell (shared strings resolved).
    static std::string CellText(const Excel::ExcelCellValue& value, const Excel::SharedStringTableService& sharedStrings)
    {
        if (value.Kind() == Excel::CellValueKind::SharedString)
        {
            if (const auto index = value.SharedStringIndex())
            {
                return sharedStrings.Lookup(*index).value_or(std::string());
            }
            return {};
        }
        return value.Text();
    }

    [[nodiscard]] static bool IsTextCell(const Excel::ExcelCellValue& value)
    {
        return value.Kind() == Excel::CellValueKind::SharedString ||
               value.Kind() == Excel::CellValueKind::InlineString;
    }

    // --- Search ----------------------------------------------------------------

    static DocumentSearchResult SearchWordFamily(Word::WordDocumentEditor& editor, std::string_view needle,
                                                 Size contextChars, bool useRegex, bool ignoreCase)
    {
        DocumentSearchResult result;
        result.Family = DocumentFamily::Word;

        auto wordResult = Search(editor, needle, contextChars, useRegex, ignoreCase);
        result.Ok = wordResult.Ok;
        result.Diagnostics = std::move(wordResult.Diagnostics);
        result.Matches.reserve(wordResult.Matches.size());
        for (auto& match : wordResult.Matches)
        {
            DocumentSearchMatch mapped;
            mapped.Label = std::string(ToString(match.Scope)) + ": " + match.Label;
            mapped.Offset = match.Offset;
            mapped.Length = match.Length;
            mapped.MatchText = std::move(match.MatchText);
            mapped.Context = std::move(match.Context);
            result.Matches.push_back(std::move(mapped));
        }
        return result;
    }

    static DocumentSearchResult SearchExcelFamily(Excel::ExcelDocumentEditor& editor, std::string_view needle, Size contextChars,
                                                  bool useRegex, bool ignoreCase)
    {
        DocumentSearchResult result;
        result.Family = DocumentFamily::Excel;

        const auto pattern = TextPattern::BuildPattern(needle, useRegex, ignoreCase, result.Diagnostics);
        if ((useRegex || ignoreCase) && !pattern)
        {
            return result;
        }

        const auto sharedStrings = editor.SharedStrings();
        for (const auto& worksheet : editor.Worksheets())
        {
            const auto sheetName = worksheet->Name();
            for (const auto& address : worksheet->StoredCellAddresses())
            {
                const auto value = worksheet->GetCellValue(address);
                if (!value || value->IsBlank())
                {
                    continue;
                }
                const auto text = CellText(*value, sharedStrings);
                if (text.empty())
                {
                    continue;
                }
                for (const auto& match : FindMatches(text, needle, pattern, useRegex, {}))
                {
                    DocumentSearchMatch mapped;
                    mapped.Label = sheetName + "!" + address.ToA1();
                    mapped.Offset = match.Offset;
                    mapped.Length = match.Length;
                    mapped.MatchText = text.substr(match.Offset, match.Length);
                    mapped.Context = MakeContext(text, match.Offset, match.Length, contextChars);
                    result.Matches.push_back(std::move(mapped));
                }
            }
        }

        result.Ok = true;
        return result;
    }

    static DocumentSearchResult SearchPowerPointFamily(PowerPoint::PowerPointDocumentEditor& editor, std::string_view needle, Size contextChars,
                                                       bool useRegex, bool ignoreCase)
    {
        DocumentSearchResult result;
        result.Family = DocumentFamily::PowerPoint;

        const auto pattern = TextPattern::BuildPattern(needle, useRegex, ignoreCase, result.Diagnostics);
        if ((useRegex || ignoreCase) && !pattern)
        {
            return result;
        }

        const auto searchUnit = [&](const std::string& text, const std::string& label)
        {
            for (const auto& match : FindMatches(text, needle, pattern, useRegex, {}))
            {
                DocumentSearchMatch mapped;
                mapped.Label = label;
                mapped.Offset = match.Offset;
                mapped.Length = match.Length;
                mapped.MatchText = text.substr(match.Offset, match.Length);
                mapped.Context = MakeContext(text, match.Offset, match.Length, contextChars);
                result.Matches.push_back(std::move(mapped));
            }
        };

        Size slideIndex = 1;
        for (const auto& slide : editor.Slides())
        {
            Size shapeIndex = 1;
            if (auto tree = slide->ShapeTree())
            {
                for (const auto& shape : tree->Shapes())
                {
                    if (const auto frame = shape->GetTextFrame())
                    {
                        Size paragraphIndex = 1;
                        for (const auto& paragraph : frame->Paragraphs)
                        {
                            std::string text;
                            for (const auto& run : paragraph.Runs)
                            {
                                text += run.Text;
                            }
                            if (!text.empty())
                            {
                                searchUnit(text, "slide " + std::to_string(slideIndex) + " shape " +
                                                     std::to_string(shapeIndex) + " paragraph " +
                                                     std::to_string(paragraphIndex));
                            }
                            ++paragraphIndex;
                        }
                    }
                    ++shapeIndex;
                }
            }

            if (const auto notes = slide->NotesText(); !notes.empty())
            {
                searchUnit(notes, "slide " + std::to_string(slideIndex) + " notes");
            }
            ++slideIndex;
        }

        result.Ok = true;
        return result;
    }

    // --- Replace ---------------------------------------------------------------

    static DocumentReplaceResult ReplaceWordFamily(Word::WordDocumentEditor& editor, std::string_view needle,
                                                   std::string_view replacement, bool dryRun, bool useRegex,
                                                   bool ignoreCase)
    {
        DocumentReplaceResult result;
        result.Family = DocumentFamily::Word;

        auto wordResult = Replace(editor, needle, replacement, dryRun, useRegex, ignoreCase);
        result.Ok = wordResult.Ok;
        result.ReplacementCount = wordResult.ReplacementCount;
        result.Saved = wordResult.Saved;
        result.Diagnostics = std::move(wordResult.Diagnostics);
        return result;
    }

    static DocumentReplaceResult ReplaceExcelFamily(Excel::ExcelDocumentEditor& editor, std::string_view needle,
                                                    std::string_view replacement, bool dryRun, bool useRegex,
                                                    bool ignoreCase)
    {
        DocumentReplaceResult result;
        result.Family = DocumentFamily::Excel;

        const auto pattern = TextPattern::BuildPattern(needle, useRegex, ignoreCase, result.Diagnostics);
        if ((useRegex || ignoreCase) && !pattern)
        {
            return result;
        }

        const auto sharedStrings = editor.SharedStrings();
        for (const auto& worksheet : editor.Worksheets())
        {
            for (const auto& address : worksheet->StoredCellAddresses())
            {
                const auto value = worksheet->GetCellValue(address);
                if (!value || value->IsBlank())
                {
                    continue;
                }
                const auto text = CellText(*value, sharedStrings);
                if (text.empty())
                {
                    continue;
                }
                const auto matches = FindMatches(text, needle, pattern, useRegex, replacement);
                if (matches.empty())
                {
                    continue;
                }
                if (!IsTextCell(*value))
                {
                    result.SkippedMatches += matches.size();
                    continue;
                }
                result.ReplacementCount += matches.size();
                if (!dryRun)
                {
                    worksheet->SetCellText(address, ApplyMatches(text, matches));
                }
            }
        }

        if (result.SkippedMatches > 0)
        {
            result.Diagnostics.push_back(ToolDiagnostic{
                ToolSeverity::Warning,
                std::to_string(result.SkippedMatches) +
                    " match(es) in non-text cells (numbers, dates, booleans, formulas) were not replaced"});
        }

        result.Ok = true;
        return result;
    }

    static DocumentReplaceResult ReplacePowerPointFamily(PowerPoint::PowerPointDocumentEditor& editor, std::string_view needle,
                                                         std::string_view replacement, bool dryRun, bool useRegex,
                                                         bool ignoreCase)
    {
        DocumentReplaceResult result;
        result.Family = DocumentFamily::PowerPoint;

        const auto pattern = TextPattern::BuildPattern(needle, useRegex, ignoreCase, result.Diagnostics);
        if ((useRegex || ignoreCase) && !pattern)
        {
            return result;
        }

        // Applies the matches to the paragraph's runs, right-to-left so earlier
        // offsets stay valid. Text around a match keeps its run formatting; a match
        // spanning runs keeps the formatting of the run it starts in.
        const auto replaceInParagraph = [](PowerPoint::PresentationTextParagraph& paragraph,
                                           const std::vector<LocatedMatch>& matches)
        {
            std::vector<Size> runStarts;
            runStarts.reserve(paragraph.Runs.size());
            Size totalLength = 0;
            for (const auto& run : paragraph.Runs)
            {
                runStarts.push_back(totalLength);
                totalLength += run.Text.size();
            }

            const auto runIndexAt = [&](Size offset)
            {
                Size index = 0;
                for (Size i = 0; i < paragraph.Runs.size(); ++i)
                {
                    if (runStarts[i] <= offset)
                    {
                        index = i;
                    }
                }
                return index;
            };

            for (auto it = matches.rbegin(); it != matches.rend(); ++it)
            {
                const Size firstRun = runIndexAt(it->Offset);
                const Size lastRun = runIndexAt(it->Offset + it->Length - 1);
                const Size localStart = it->Offset - runStarts[firstRun];
                const Size localEnd = it->Offset + it->Length - runStarts[lastRun];

                if (firstRun == lastRun)
                {
                    auto& text = paragraph.Runs[firstRun].Text;
                    text = text.substr(0, localStart) + it->Replacement + text.substr(localEnd);
                }
                else
                {
                    paragraph.Runs[firstRun].Text =
                        paragraph.Runs[firstRun].Text.substr(0, localStart) + it->Replacement;
                    for (Size middle = firstRun + 1; middle < lastRun; ++middle)
                    {
                        paragraph.Runs[middle].Text.clear();
                    }
                    paragraph.Runs[lastRun].Text = paragraph.Runs[lastRun].Text.substr(localEnd);
                }
            }
        };

        for (const auto& slide : editor.Slides())
        {
            if (auto tree = slide->ShapeTree())
            {
                for (const auto& shape : tree->Shapes())
                {
                    auto frame = shape->GetTextFrame();
                    if (!frame)
                    {
                        continue;
                    }
                    bool frameChanged = false;
                    for (auto& paragraph : frame->Paragraphs)
                    {
                        std::string text;
                        for (const auto& run : paragraph.Runs)
                        {
                            text += run.Text;
                        }
                        if (text.empty())
                        {
                            continue;
                        }
                        const auto matches = FindMatches(text, needle, pattern, useRegex, replacement);
                        if (matches.empty())
                        {
                            continue;
                        }
                        result.ReplacementCount += matches.size();
                        if (!dryRun)
                        {
                            replaceInParagraph(paragraph, matches);
                            frameChanged = true;
                        }
                    }
                    if (frameChanged)
                    {
                        shape->SetTextFrame(*frame);
                    }
                }
            }

            const auto notes = slide->NotesText();
            if (!notes.empty())
            {
                const auto matches = FindMatches(notes, needle, pattern, useRegex, replacement);
                if (!matches.empty())
                {
                    result.ReplacementCount += matches.size();
                    if (!dryRun)
                    {
                        slide->SetNotesText(ApplyMatches(notes, matches));
                    }
                }
            }
        }

        result.Ok = true;
        return result;
    }

    /// The result a family reports when its editor could not open the file.
    template <typename TResult>
    static TResult FailedToOpen(DocumentFamily family, const char* message, const std::filesystem::path& path)
    {
        TResult result;
        result.Family = family;
        result.Diagnostics.push_back(ToolDiagnostic{ToolSeverity::Error, message, path.string()});
        return result;
    }

    /// Searching or replacing nothing is a caller error, not an empty result set.
    template <typename TResult>
    static TResult EmptyNeedle(DocumentFamily family)
    {
        TResult result;
        result.Family = family;
        result.Diagnostics.push_back(ToolDiagnostic{ToolSeverity::Error, "Search text must not be empty"});
        return result;
    }

    /// Writes a replacement result out, unless it was a dry run, changed nothing, or failed.
    static DocumentReplaceResult SaveReplaced(DocumentReplaceResult result, OpenXmlPackage* package, bool dryRun,
                                              const std::filesystem::path& destination, const char* failureMessage)
    {
        if (!result.Ok || dryRun || result.ReplacementCount == 0)
        {
            return result;
        }

        if (package == nullptr || !package->SaveToFile(destination))
        {
            result.Ok = false;
            result.Diagnostics.push_back(ToolDiagnostic{ToolSeverity::Error, failureMessage, destination.string()});
            return result;
        }

        result.Saved = true;
        return result;
    }
};

DocumentSearchResult SearchDocumentText(const std::filesystem::path& path, std::string_view needle,
                                        Size contextChars, bool useRegex, bool ignoreCase)
{
    DocumentSearchResult result;
    if (needle.empty())
    {
        result.Diagnostics.push_back(ToolDiagnostic{ToolSeverity::Error, "Search text must not be empty"});
        return result;
    }

    result.Family = DocumentTextToolsHelper::DetectFamily(path, result.Diagnostics);
    switch (result.Family)
    {
        case DocumentFamily::Word:
        {
            auto editor = Word::WordDocumentEditor::Open(path, UntrustedOpenSettings());
            return editor ? SearchDocumentText(*editor, needle, contextChars, useRegex, ignoreCase)
                          : DocumentTextToolsHelper::FailedToOpen<DocumentSearchResult>(DocumentFamily::Word,
                                                                                        "Failed to open Word document", path);
        }
        case DocumentFamily::Excel:
        {
            auto editor = Excel::ExcelDocumentEditor::Open(path, UntrustedOpenSettings());
            return editor ? SearchDocumentText(*editor, needle, contextChars, useRegex, ignoreCase)
                          : DocumentTextToolsHelper::FailedToOpen<DocumentSearchResult>(DocumentFamily::Excel,
                                                                                        "Failed to open Excel document", path);
        }
        case DocumentFamily::PowerPoint:
        {
            auto editor = PowerPoint::PowerPointDocumentEditor::Open(path, UntrustedOpenSettings());
            return editor ? SearchDocumentText(*editor, needle, contextChars, useRegex, ignoreCase)
                          : DocumentTextToolsHelper::FailedToOpen<DocumentSearchResult>(DocumentFamily::PowerPoint,
                                                                                        "Failed to open PowerPoint document", path);
        }
        case DocumentFamily::Unknown:
            break;
    }
    return result;
}

DocumentSearchResult SearchDocumentText(Word::WordDocumentEditor& editor, std::string_view needle,
                                        Size contextChars, bool useRegex, bool ignoreCase)
{
    return needle.empty() ? DocumentTextToolsHelper::EmptyNeedle<DocumentSearchResult>(DocumentFamily::Word)
                          : DocumentTextToolsHelper::SearchWordFamily(editor, needle, contextChars, useRegex, ignoreCase);
}

DocumentSearchResult SearchDocumentText(Excel::ExcelDocumentEditor& editor, std::string_view needle,
                                        Size contextChars, bool useRegex, bool ignoreCase)
{
    return needle.empty() ? DocumentTextToolsHelper::EmptyNeedle<DocumentSearchResult>(DocumentFamily::Excel)
                          : DocumentTextToolsHelper::SearchExcelFamily(editor, needle, contextChars, useRegex, ignoreCase);
}

DocumentSearchResult SearchDocumentText(PowerPoint::PowerPointDocumentEditor& editor, std::string_view needle,
                                        Size contextChars, bool useRegex, bool ignoreCase)
{
    return needle.empty() ? DocumentTextToolsHelper::EmptyNeedle<DocumentSearchResult>(DocumentFamily::PowerPoint)
                          : DocumentTextToolsHelper::SearchPowerPointFamily(editor, needle, contextChars, useRegex, ignoreCase);
}

DocumentReplaceResult ReplaceDocumentText(const std::filesystem::path& path, std::string_view needle,
                                          std::string_view replacement, bool dryRun,
                                          const std::filesystem::path& outputPath, bool useRegex,
                                          bool ignoreCase)
{
    DocumentReplaceResult result;
    if (needle.empty())
    {
        result.Diagnostics.push_back(ToolDiagnostic{ToolSeverity::Error, "Search text must not be empty"});
        return result;
    }

    result.Family = DocumentTextToolsHelper::DetectFamily(path, result.Diagnostics);
    const auto destination = outputPath.empty() ? path : outputPath;
    switch (result.Family)
    {
        case DocumentFamily::Word:
        {
            auto editor = Word::WordDocumentEditor::Open(path, UntrustedOpenSettings());
            if (!editor)
            {
                return DocumentTextToolsHelper::FailedToOpen<DocumentReplaceResult>(DocumentFamily::Word, "Failed to open Word document",
                                                                                    path);
            }
            return DocumentTextToolsHelper::SaveReplaced(ReplaceDocumentText(*editor, needle, replacement, dryRun, useRegex, ignoreCase),
                                                         editor->GetDocument().get(), dryRun, destination, "Failed to save document");
        }
        case DocumentFamily::Excel:
        {
            auto editor = Excel::ExcelDocumentEditor::Open(path, UntrustedOpenSettings());
            if (!editor)
            {
                return DocumentTextToolsHelper::FailedToOpen<DocumentReplaceResult>(DocumentFamily::Excel, "Failed to open Excel document",
                                                                                    path);
            }
            return DocumentTextToolsHelper::SaveReplaced(ReplaceDocumentText(*editor, needle, replacement, dryRun, useRegex, ignoreCase),
                                                         editor->GetDocument().get(), dryRun, destination, "Failed to save workbook");
        }
        case DocumentFamily::PowerPoint:
        {
            auto editor = PowerPoint::PowerPointDocumentEditor::Open(path, UntrustedOpenSettings());
            if (!editor)
            {
                return DocumentTextToolsHelper::FailedToOpen<DocumentReplaceResult>(DocumentFamily::PowerPoint,
                                                                                    "Failed to open PowerPoint document", path);
            }
            return DocumentTextToolsHelper::SaveReplaced(ReplaceDocumentText(*editor, needle, replacement, dryRun, useRegex, ignoreCase),
                                                         editor->GetDocument().get(), dryRun, destination, "Failed to save presentation");
        }
        case DocumentFamily::Unknown:
            break;
    }
    return result;
}

DocumentReplaceResult ReplaceDocumentText(Word::WordDocumentEditor& editor, std::string_view needle,
                                          std::string_view replacement, bool dryRun, bool useRegex,
                                          bool ignoreCase)
{
    return needle.empty() ? DocumentTextToolsHelper::EmptyNeedle<DocumentReplaceResult>(DocumentFamily::Word)
                          : DocumentTextToolsHelper::ReplaceWordFamily(editor, needle, replacement, dryRun, useRegex, ignoreCase);
}

DocumentReplaceResult ReplaceDocumentText(Excel::ExcelDocumentEditor& editor, std::string_view needle,
                                          std::string_view replacement, bool dryRun, bool useRegex,
                                          bool ignoreCase)
{
    return needle.empty() ? DocumentTextToolsHelper::EmptyNeedle<DocumentReplaceResult>(DocumentFamily::Excel)
                          : DocumentTextToolsHelper::ReplaceExcelFamily(editor, needle, replacement, dryRun, useRegex, ignoreCase);
}

DocumentReplaceResult ReplaceDocumentText(PowerPoint::PowerPointDocumentEditor& editor, std::string_view needle,
                                          std::string_view replacement, bool dryRun, bool useRegex,
                                          bool ignoreCase)
{
    return needle.empty() ? DocumentTextToolsHelper::EmptyNeedle<DocumentReplaceResult>(DocumentFamily::PowerPoint)
                          : DocumentTextToolsHelper::ReplacePowerPointFamily(editor, needle, replacement, dryRun, useRegex, ignoreCase);
}

} // namespace ExyokiOffice::Tools
