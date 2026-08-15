// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

#include "FamilyAdapter.hpp"
#include "ToolRegistry.hpp"

#include "ExyokiOffice/Word/WordDocument.hpp"

#include <memory>

namespace ExyokiOffice::Mcp
{

/** @brief A Word document held open by a session. */
class WordDocumentHandle final : public DocumentHandle
{
public:
    /// @param limits Applied when LoadFromMemory reopens the package (undo, session bridge).
    WordDocumentHandle(Word::WordDocumentEditor::Ptr editor, OpenXmlPackageLimits limits);

    [[nodiscard]] Tools::DocumentFamily Family() const override;
    bool SaveToFile(const std::filesystem::path& path) override;
    [[nodiscard]] std::vector<Byte> SaveToMemory() override;
    bool LoadFromMemory(std::span<const Byte> bytes) override;
    [[nodiscard]] std::shared_ptr<OpenXmlPackage> Package() const override;
    [[nodiscard]] nlohmann::json Summary() const override;

    /// The editor the Word toolset drives; never null for a live handle.
    [[nodiscard]] Word::WordDocumentEditor& Editor() const noexcept { return *m_editor; }

private:
    Word::WordDocumentEditor::Ptr m_editor;
    OpenXmlPackageLimits m_packageLimits;
};

/** @brief Word implementation of the family operations the shared core needs. */
class WordFamilyAdapter final : public FamilyAdapter
{
public:
    [[nodiscard]] Tools::DocumentFamily Family() const override;
    [[nodiscard]] std::string FamilyName() const override;
    [[nodiscard]] std::string FileExtension() const override;
    [[nodiscard]] std::unique_ptr<DocumentHandle> CreateNew() const override;
    [[nodiscard]] std::unique_ptr<DocumentHandle> Open(const std::filesystem::path& path) const override;
    [[nodiscard]] std::unique_ptr<DocumentHandle> OpenFromMemory(std::span<const Byte> bytes) const override;
    [[nodiscard]] Tools::DocumentModel ReadModel(DocumentHandle& document,
                                                 const Tools::ModelReadOptions& options,
                                                 std::vector<Tools::ToolDiagnostic>& diagnostics) const override;
    [[nodiscard]] Tools::DocumentStats Stat(DocumentHandle& document) const override;
    [[nodiscard]] Tools::ExtractedDocumentText ExtractText(DocumentHandle& document) const override;
    [[nodiscard]] Tools::DocumentSearchResult SearchText(DocumentHandle& document, std::string_view needle,
                                                         Size contextChars, bool useRegex,
                                                         bool ignoreCase) const override;
    [[nodiscard]] Tools::DocumentReplaceResult ReplaceText(DocumentHandle& document, std::string_view needle,
                                                           std::string_view replacement, bool dryRun, bool useRegex,
                                                           bool ignoreCase) const override;
    [[nodiscard]] Tools::RedactResult Redact(DocumentHandle& document,
                                             const Tools::RedactOptions& options) const override;
};

/// Registers the Word content, table, layout, review, and automation groups.
void RegisterWordToolset(ToolRegistry& registry);

} // namespace ExyokiOffice::Mcp
