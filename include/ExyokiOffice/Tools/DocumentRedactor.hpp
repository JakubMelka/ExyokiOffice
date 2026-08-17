// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

#include "ExyokiOffice/Export.hpp"
#include "ExyokiOffice/Tools/DocumentEditors.hpp"
#include "ExyokiOffice/Tools/PackageModel.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace ExyokiOffice::Tools
{

/**
 * @brief Selects which review and identity artifacts RedactDocument() removes.
 *
 * Every switch defaults to on: the plain call scrubs everything the tool knows
 * how to scrub, and callers opt out per category.
 */
struct EXYOKIOFFICE_EXPORT RedactOptions
{
    /// Remove comments: Word comment parts and in-text markers, Excel classic
    /// and threaded comments and their person registry, PowerPoint comments -
    /// modern and legacy `ppt/comments/*` alike - and their authors.
    bool RemoveComments = true;
    /// Word only: accept every tracked revision in every story part - body,
    /// headers, footers, footnotes, endnotes, comments - including deleted
    /// paragraph marks, deleted table rows, and the records of former
    /// formatting (`w:rPrChange` and its siblings).
    bool ResolveRevisions = true;
    /// Word only: delete runs formatted as hidden text, whether the run says so
    /// (`w:vanish`, `w:specVanish`) or its style does. Either element switched
    /// off with `w:val="false"` means the run is visible and stays.
    bool RemoveHiddenText = true;
    /// Clear identity and descriptive metadata (Creator, LastModifiedBy,
    /// Company, Manager, Title, Subject, Description, Keywords, Category,
    /// Revision, last printed, attached template) and remove the parts that
    /// hold more of it: custom properties, `customXml`, and the `docProps`
    /// thumbnail. In Word it also strips the `w:rsid*` editing-session
    /// identifiers from the story parts.
    ///
    /// The registries that name comment authors - the workbook person registry,
    /// the presentation comment authors, Word's `people.xml` - are not in that
    /// list, because they are only removable together with the comments that
    /// refer to them. With RemoveComments off they stay, and they still name
    /// the people who wrote the comments that stayed with them.
    bool RemovePersonalMetadata = true;
};

/** @brief Counts and outcome reported by RedactDocument(). */
struct EXYOKIOFFICE_EXPORT RedactResult
{
    bool Ok = false;
    DocumentFamily Family = DocumentFamily::Unknown;
    /// Comments removed across the whole package (all families).
    Size CommentsRemoved = 0;
    /// Tracked revisions accepted (Word only).
    Size RevisionsResolved = 0;
    /// Hidden-text runs deleted (Word only).
    Size HiddenRunsRemoved = 0;
    /// Identity fields cleared plus one for a removed custom-properties part.
    Size MetadataFieldsCleared = 0;
    /// Whole parts detached because of what they carried: the thumbnail,
    /// `customXml`, person and author registries, legacy comment parts.
    Size PartsRemoved = 0;
    /// True when the redacted document was written to disk.
    bool Saved = false;
    std::vector<ToolDiagnostic> Diagnostics;
};

/**
 * @brief Removes review and identity artifacts from a Word, Excel, or
 * PowerPoint package before publication.
 *
 * The scrub is structural, not typographic: it deletes comments, resolves
 * tracked revisions, deletes hidden-text runs, and clears identity metadata.
 * It does not search body text for sensitive words — combine it with
 * ReplaceDocumentText() for content-level redaction.
 *
 * Word specifics: comment parts (`comments.xml` and its modern companion
 * parts, plus `people.xml`) are detached and every `w:commentRangeStart`,
 * `w:commentRangeEnd`, and `w:commentReference` marker is stripped from the
 * body, headers, footers, footnotes, and endnotes. Revision handling accepts
 * (rather than rejects) changes, matching the usual "final version" publish
 * flow, and it runs over every one of those story parts rather than the body
 * alone. Accepting a revision also drops the property-level records that stand
 * for one - `w:trPr/w:ins` on an inserted row, `w:pPr/w:rPr/w:del` on a deleted
 * paragraph mark - each of which carries an author and a date. Hidden-text
 * removal deletes runs hidden by `w:vanish`, by `w:specVanish`, or by a
 * character style that carries either.
 *
 * @warning This is not a guarantee that a package holds nothing else. It
 * removes what is listed above and what RedactOptions describes, in the three
 * formats named there; a scrub that must be complete for a legal or safety
 * purpose has to be verified against the file, not against this list. Known
 * gaps: embedded objects and their own packages are not opened, media is not
 * inspected (an image can carry EXIF), VBA projects are left alone, and text a
 * document merely stops displaying - a shape placed off the slide, a column of
 * zero width, a run coloured like the page - is content rather than metadata
 * and stays.
 *
 * The result is saved to @p outputPath, or back to @p path when
 * @p outputPath is empty. The input file is never modified when an output
 * path is given.
 */
[[nodiscard]] EXYOKIOFFICE_EXPORT RedactResult RedactDocument(const std::filesystem::path& path,
                                                              const std::filesystem::path& outputPath = {},
                                                              const RedactOptions& options = {});

/**
 * @brief Redacts a document that is already open, in place.
 *
 * The difference from the overload above is not only where the document comes
 * from: these never save. The document is left redacted in memory and writing
 * it out — or discarding it — is the caller's decision, which is the only
 * sensible contract when the caller is holding the document for other reasons
 * as well. Saved is therefore always false; Ok still reports whether the
 * redaction itself succeeded.
 */
[[nodiscard]] EXYOKIOFFICE_EXPORT RedactResult RedactDocument(Word::WordDocumentEditor& editor,
                                                              const RedactOptions& options = {});
[[nodiscard]] EXYOKIOFFICE_EXPORT RedactResult RedactDocument(Excel::ExcelDocumentEditor& editor,
                                                              const RedactOptions& options = {});
[[nodiscard]] EXYOKIOFFICE_EXPORT RedactResult RedactDocument(PowerPoint::PowerPointDocumentEditor& editor,
                                                              const RedactOptions& options = {});

} // namespace ExyokiOffice::Tools
