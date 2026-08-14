// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "ExyokiOffice/Tools/DocumentStats.hpp"
#include "ExyokiOffice/Tools/PackageLimits.hpp"

#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/Math.hpp"
#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/Wordprocessing.hpp"
#include "ExyokiOffice/Excel/ExcelDocument.hpp"
#include "ExyokiOffice/OpenXmlPackage.hpp"
#include "ExyokiOffice/PowerPoint/PowerPointDocument.hpp"
#include "ExyokiOffice/Tools/PackageInspector.hpp"
#include "ExyokiOffice/Word/WordDocument.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

namespace ExyokiOffice::Tools
{

/// File-local counters behind the document statistics.
class DocumentStatsHelper
{
public:
    static constexpr Real WordsPerMinute = 200.0;

    static UInt64 CountWords(const std::string& text)
    {
        UInt64 count = 0;
        bool inWord = false;
        for (const char rawCh : text)
        {
            const auto ch = static_cast<unsigned char>(rawCh);
            const bool isSpace = ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f' || ch == '\v';
            if (isSpace)
            {
                inWord = false;
            }
            else if (!inWord)
            {
                inWord = true;
                ++count;
            }
        }
        return count;
    }

    /// Counts Unicode codepoints in a UTF-8 string (skips continuation bytes).
    static UInt64 CountCodepoints(const std::string& text)
    {
        UInt64 count = 0;
        for (const char rawCh : text)
        {
            const auto ch = static_cast<unsigned char>(rawCh);
            if ((ch & 0xC0) != 0x80)
            {
                ++count;
            }
        }
        return count;
    }

    static bool IsHeadingStyle(const std::string& styleId)
    {
        return styleId == "Title" || styleId.rfind("Heading", 0) == 0;
    }

    struct WordAccumulator
    {
        UInt64 Words = 0;
        UInt64 Characters = 0;
        UInt64 Paragraphs = 0;
        UInt64 Headings = 0;
        UInt64 Images = 0;
        UInt64 Equations = 0;
        UInt64 Hyperlinks = 0;
    };

    static void AccumulateParagraph(const std::shared_ptr<Word::Paragraph>& paragraph, WordAccumulator& acc)
    {
        ++acc.Paragraphs;

        const auto text = paragraph->PlainText();
        acc.Words += CountWords(text);
        acc.Characters += CountCodepoints(text);

        if (IsHeadingStyle(paragraph->GetStyleId()))
        {
            ++acc.Headings;
        }

        acc.Images += paragraph->Images().size();
        acc.Hyperlinks += paragraph->Hyperlinks().size();

        if (auto element = paragraph->GetLowLevelApi())
        {
            acc.Equations +=
                element->Descendants<DocumentFormat::OpenXml::Math::OfficeMath>().size();
        }
    }

    static void AccumulateParagraphs(const std::vector<std::shared_ptr<Word::Paragraph>>& paragraphs, WordAccumulator& acc)
    {
        for (const auto& paragraph : paragraphs)
        {
            AccumulateParagraph(paragraph, acc);
        }
    }

    static UInt64 CountTablesRecursive(const std::shared_ptr<Word::Table>& table)
    {
        UInt64 count = 1;
        for (const auto& nested : table->Tables())
        {
            count += CountTablesRecursive(nested);
        }
        return count;
    }

    static DocumentStats StatWord(Word::WordDocumentEditor& editor)
    {
        DocumentStats result;
        result.Family = DocumentFamily::Word;

        WordAccumulator acc;
        AccumulateParagraphs(editor.Paragraphs(), acc);

        UInt64 tableCount = 0;
        for (const auto& table : editor.Tables())
        {
            tableCount += CountTablesRecursive(table);
            AccumulateParagraphs(table->Paragraphs(), acc);
        }

        UInt64 footnoteCount = 0;
        for (const auto& note : editor.Footnotes())
        {
            if (note->GetEntryType() == Word::NoteEntryType::Normal)
            {
                ++footnoteCount;
            }
        }

        UInt64 endnoteCount = 0;
        for (const auto& note : editor.Endnotes())
        {
            if (note->GetEntryType() == Word::NoteEntryType::Normal)
            {
                ++endnoteCount;
            }
        }

        result.WordCount = acc.Words;
        result.CharacterCount = acc.Characters;
        result.ParagraphCount = acc.Paragraphs;
        result.HeadingCount = acc.Headings;
        result.TableCount = tableCount;
        result.ImageCount = acc.Images;
        result.EquationCount = acc.Equations;
        result.FootnoteCount = footnoteCount;
        result.EndnoteCount = endnoteCount;
        result.BookmarkCount = static_cast<UInt64>(editor.Bookmarks().size());
        result.HyperlinkCount = acc.Hyperlinks;
        result.CommentCount = static_cast<UInt64>(editor.Comments().size());
        result.SectionCount = static_cast<UInt64>(editor.Sections().size());
        result.ReadingTimeMinutes = static_cast<Real>(acc.Words) / WordsPerMinute;
        result.Ok = true;
        return result;
    }

    static DocumentStats StatExcel(Excel::ExcelDocumentEditor& editor)
    {
        DocumentStats result;
        result.Family = DocumentFamily::Excel;

        const auto worksheets = editor.Worksheets();

        UInt64 cellCount = 0;
        UInt64 formulaCount = 0;
        UInt64 tableCount = 0;
        UInt64 hyperlinkCount = 0;
        UInt64 commentCount = 0;
        UInt64 imageCount = 0;
        UInt64 mergedRangeCount = 0;

        for (const auto& worksheet : worksheets)
        {
            cellCount += worksheet->StoredCellCount();
            for (const auto& address : worksheet->StoredCellAddresses())
            {
                if (worksheet->GetCellFormula(address))
                {
                    ++formulaCount;
                }
            }
            tableCount += worksheet->Tables().size();
            hyperlinkCount += worksheet->Hyperlinks().size();
            commentCount += worksheet->Comments().size() + worksheet->ThreadedComments().size();
            imageCount += worksheet->Images().size();
            mergedRangeCount += worksheet->MergedRanges().size();
        }

        result.WorksheetCount = static_cast<UInt64>(worksheets.size());
        result.CellCount = cellCount;
        result.FormulaCount = formulaCount;
        result.TableCount = tableCount;
        result.HyperlinkCount = hyperlinkCount;
        result.CommentCount = commentCount;
        result.ImageCount = imageCount;
        result.MergedRangeCount = mergedRangeCount;
        result.Ok = true;
        return result;
    }

    struct PowerPointAccumulator
    {
        UInt64 Words = 0;
        UInt64 Shapes = 0;
        UInt64 Images = 0;
        UInt64 Tables = 0;
        UInt64 Charts = 0;
        UInt64 Hyperlinks = 0;
    };

    static void WalkShapes(const std::vector<std::shared_ptr<PowerPoint::PresentationShape>>& shapes, PowerPointAccumulator& acc)
    {
        for (const auto& shape : shapes)
        {
            ++acc.Shapes;

            if (shape->IsGroup())
            {
                WalkShapes(shape->Children(), acc);
                continue;
            }

            if (auto frame = shape->GetTextFrame())
            {
                for (const auto& paragraph : frame->Paragraphs)
                {
                    for (const auto& run : paragraph.Runs)
                    {
                        acc.Words += CountWords(run.Text);
                        if (run.Hyperlink)
                        {
                            ++acc.Hyperlinks;
                        }
                    }
                }
            }

            if (shape->GetPicture())
            {
                ++acc.Images;
            }

            if (auto table = shape->GetTable())
            {
                ++acc.Tables;
                for (const auto& row : table->Rows)
                {
                    for (const auto& cell : row.Cells)
                    {
                        acc.Words += CountWords(cell.Text);
                    }
                }
            }

            if (auto embedded = shape->GetEmbeddedObject();
                embedded && embedded->Kind == PowerPoint::PresentationEmbeddedObjectKind::Chart)
            {
                ++acc.Charts;
            }
        }
    }

    static DocumentStats StatPowerPoint(PowerPoint::PowerPointDocumentEditor& editor)
    {
        DocumentStats result;
        result.Family = DocumentFamily::PowerPoint;

        PowerPointAccumulator acc;
        UInt64 hiddenCount = 0;
        UInt64 notesCount = 0;
        UInt64 commentCount = 0;

        const auto slides = editor.Slides();
        for (const auto& slide : slides)
        {
            if (slide->IsHidden())
            {
                ++hiddenCount;
            }
            if (!slide->NotesText().empty())
            {
                ++notesCount;
            }
            commentCount += slide->Comments().size();

            if (auto tree = slide->ShapeTree())
            {
                WalkShapes(tree->Shapes(), acc);
            }
        }

        result.SlideCount = static_cast<UInt64>(slides.size());
        result.HiddenSlideCount = hiddenCount;
        result.SlideWithNotesCount = notesCount;
        result.ShapeCount = acc.Shapes;
        result.ImageCount = acc.Images;
        result.TableCount = acc.Tables;
        result.ChartCount = acc.Charts;
        result.HyperlinkCount = acc.Hyperlinks;
        result.CommentCount = commentCount;
        result.WordCount = acc.Words;
        result.ReadingTimeMinutes = static_cast<Real>(acc.Words) / WordsPerMinute;
        result.Ok = true;
        return result;
    }

    /// The result a family reports when its editor could not open the file.
    static DocumentStats FailedToOpen(DocumentFamily family, const char* message, const std::filesystem::path& path)
    {
        DocumentStats result;
        result.Family = family;
        result.Diagnostics.push_back(ToolDiagnostic{ToolSeverity::Error, message, path.string()});
        return result;
    }
};

DocumentStats Stat(const std::filesystem::path& path)
{
    OpenXmlPackage package;
    ApplyDefaultPackageLimits(package);
    if (!package.LoadFromFile(path))
    {
        DocumentStats result;
        result.Diagnostics.push_back(ToolDiagnostic{ToolSeverity::Error, "Failed to open package", path.string()});
        return result;
    }

    // Reopened through the family editor rather than reused: the probe answers
    // which family this is, and the statistics are gathered from the typed
    // editor. A caller that already holds one calls the overloads below and
    // pays for neither open.
    const auto info = GetInfo(package);
    switch (info.Family)
    {
        case DocumentFamily::Word:
        {
            auto editor = Word::WordDocumentEditor::Open(path, UntrustedOpenSettings());
            return editor ? Stat(*editor) : DocumentStatsHelper::FailedToOpen(DocumentFamily::Word, "Failed to open Word document", path);
        }
        case DocumentFamily::Excel:
        {
            auto editor = Excel::ExcelDocumentEditor::Open(path, UntrustedOpenSettings());
            return editor ? Stat(*editor) : DocumentStatsHelper::FailedToOpen(DocumentFamily::Excel, "Failed to open Excel document", path);
        }
        case DocumentFamily::PowerPoint:
        {
            auto editor = PowerPoint::PowerPointDocumentEditor::Open(path, UntrustedOpenSettings());
            return editor ? Stat(*editor)
                          : DocumentStatsHelper::FailedToOpen(DocumentFamily::PowerPoint, "Failed to open PowerPoint document", path);
        }
        case DocumentFamily::Unknown:
            break;
    }

    DocumentStats result;
    result.Diagnostics.push_back(
        ToolDiagnostic{ToolSeverity::Error, DescribeUnknownFamily(info), path.string()});
    return result;
}

DocumentStats Stat(Word::WordDocumentEditor& editor)
{
    return DocumentStatsHelper::StatWord(editor);
}

DocumentStats Stat(Excel::ExcelDocumentEditor& editor)
{
    return DocumentStatsHelper::StatExcel(editor);
}

DocumentStats Stat(PowerPoint::PowerPointDocumentEditor& editor)
{
    return DocumentStatsHelper::StatPowerPoint(editor);
}

} // namespace ExyokiOffice::Tools
