// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

// Reader for corpus/manifest.json, the description of the file-based fixtures.
//
// The corpus is a set of real packages saved by Microsoft Office, and the
// manifest is what makes each of them a test rather than a blob: it records why
// the file is in the tree, which parts it must keep, and the shape of its part
// graph. Every test in this layer is driven from the manifest, so adding a
// fixture without describing it fails rather than being quietly ignored.

#include "ExyokiOffice/StandardTypes.hpp"

#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace ExyokiOfficeTests
{

/** One entry of `corpus/manifest.json`. */
struct CorpusDocument
{
    /** Path relative to `corpus/`, e.g. `word/Open_Source_Software.docx`. */
    std::string File;
    /** `word`, `excel` or `powerpoint`, as the package inspector reports it. */
    std::string Family;
    /** Document type name, e.g. `Document`, `Workbook`, `Presentation`. */
    std::string DocumentType;
    /** The application that saved the file, from `docProps/app.xml`. */
    std::string Producer;
    /** Why this file earns its place in the corpus. */
    std::string Purpose;
    /** Capability tags this fixture is the evidence for. */
    std::vector<std::string> Features;
    /** URI of the main document part. */
    std::string MainPart;
    ExyokiOffice::Size PartCount = 0;
    ExyokiOffice::Size RelationshipCount = 0;
    ExyokiOffice::Size ContentTypeCount = 0;
    ExyokiOffice::Size FileSize = 0;
    /** SHA-256 of the file on disk, lowercase hex, pinning the fixture itself. */
    std::string Sha256;
    /** Parts whose loss would make the fixture pointless. Not the full part list. */
    std::vector<std::string> RequiredParts;

    /** Absolute path of the file in the source tree. */
    [[nodiscard]] std::filesystem::path Path() const;
};

/** Directory holding the corpus in the source tree. */
[[nodiscard]] std::filesystem::path CorpusRoot();

/**
 * @brief Reads `corpus/manifest.json`.
 *
 * Throws when the manifest is missing or malformed rather than returning an
 * empty list: a corpus layer that silently runs no cases would report success
 * for a suite that tested nothing.
 */
[[nodiscard]] const std::vector<CorpusDocument>& CorpusManifest();

/** Environment variable naming the one document a sharded CTest entry runs. */
inline constexpr const char* CorpusDocumentVariable = "EXYOKIOFFICE_CORPUS_DOCUMENT";

/**
 * @brief The corpus documents this process is meant to sweep.
 *
 * The three cases that read every package end to end - validation, the
 * content-model cross-check and the round trip - are the bulk of the suite's
 * running time, and each of them is one CTest entry per corpus document rather
 * than one entry for the whole corpus. tests/corpus/CMakeLists.txt generates
 * those entries from the manifest and tells each one which document it owns
 * through @ref CorpusDocumentVariable; this returns that document, or the whole
 * manifest when the variable is unset.
 *
 * Cases that ask a question about the corpus as a whole - that every file is
 * described, that all three families are present - use CorpusManifest()
 * instead, and are registered as a single entry.
 *
 * Throws when the variable names a document the manifest does not list, which
 * is what a renamed fixture and a stale build tree look like from here.
 */
[[nodiscard]] const std::vector<CorpusDocument>& CorpusShard();

/** Every package file present under `corpus/`, as manifest-relative paths. */
[[nodiscard]] std::vector<std::string> CorpusFilesOnDisk();

/** Reads a file into memory; returns an empty vector when it cannot be read. */
[[nodiscard]] std::vector<ExyokiOffice::Byte> ReadAllBytes(const std::filesystem::path& path);

/** Lowercase hex SHA-256 of @p bytes. */
[[nodiscard]] std::string Sha256Hex(std::span<const ExyokiOffice::Byte> bytes);

} // namespace ExyokiOfficeTests
