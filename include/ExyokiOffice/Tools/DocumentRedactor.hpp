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
    /// and threaded comments, PowerPoint modern comments and their authors.
    bool RemoveComments = true;
    /// Word only: accept every tracked revision in the main body, so inserted
    /// text stays, deleted text disappears, and no revision metadata remains.
    bool ResolveRevisions = true;
    /// Word only: delete runs formatted as hidden text (`w:vanish`).
    bool RemoveHiddenText = true;
    /// Clear identity properties (Creator, LastModifiedBy, Company) and remove
    /// the custom-properties part when the package has one.
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
 * flow. Hidden-text removal deletes runs whose `w:rPr/w:vanish` is in effect.
 *
 * The result is saved to @p outputPath, or back to @p path when
 * @p outputPath is empty. The input file is never modified when an output
 * path is given.
 */
EXYOKIOFFICE_EXPORT RedactResult RedactDocument(const std::filesystem::path& path,
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
EXYOKIOFFICE_EXPORT RedactResult RedactDocument(Word::WordDocumentEditor& editor,
                                                const RedactOptions& options = {});
EXYOKIOFFICE_EXPORT RedactResult RedactDocument(Excel::ExcelDocumentEditor& editor,
                                                const RedactOptions& options = {});
EXYOKIOFFICE_EXPORT RedactResult RedactDocument(PowerPoint::PowerPointDocumentEditor& editor,
                                                const RedactOptions& options = {});

} // namespace ExyokiOffice::Tools
