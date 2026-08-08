// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

#include "ExyokiOffice/OpenXmlPackage.hpp"
#include "ExyokiOffice/StandardTypes.hpp"
#include "ExyokiOffice/Tools/DocumentModel.hpp"
#include "ExyokiOffice/Tools/DocumentModelIO.hpp"
#include "ExyokiOffice/Tools/DocumentRedactor.hpp"
#include "ExyokiOffice/Tools/DocumentStats.hpp"
#include "ExyokiOffice/Tools/DocumentTextTools.hpp"
#include "ExyokiOffice/Tools/PackageModel.hpp"
#include "ExyokiOffice/Tools/TextExtractor.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ExyokiOffice::Mcp
{

/**
 * @brief One open document, owned by a session.
 *
 * The core never knows which editor it is driving: it creates, saves, and
 * snapshots documents through this interface, while a family toolset
 * downcasts to its own handle to reach `WordDocumentEditor` and friends.
 */
class DocumentHandle
{
public:
    virtual ~DocumentHandle() = default;

    DocumentHandle() = default;
    DocumentHandle(const DocumentHandle&) = delete;
    DocumentHandle& operator=(const DocumentHandle&) = delete;
    DocumentHandle(DocumentHandle&&) = delete;
    DocumentHandle& operator=(DocumentHandle&&) = delete;

    [[nodiscard]] virtual Tools::DocumentFamily Family() const = 0;

    /// Writes the document atomically; false on any I/O or serialization failure.
    virtual bool SaveToFile(const std::filesystem::path& path) = 0;

    /// Serializes the whole package into memory (snapshots and the session bridge).
    [[nodiscard]] virtual std::vector<Byte> SaveToMemory() = 0;

    /// Replaces the edited document with one loaded from @p bytes (undo, bridge reload).
    virtual bool LoadFromMemory(std::span<const Byte> bytes) = 0;

    /// The underlying OPC package, for property and media access.
    [[nodiscard]] virtual std::shared_ptr<OpenXmlPackage> Package() const = 0;

    /// Family-specific structural counts reported by `open_document`.
    [[nodiscard]] virtual nlohmann::json Summary() const = 0;
};

/**
 * @brief Everything the shared core needs to know about one document family.
 *
 * Each binary constructs exactly one adapter, which is what turns the shared
 * tools into Word, Excel, or PowerPoint tools without duplicating them.
 */
class FamilyAdapter
{
public:
    virtual ~FamilyAdapter() = default;

    FamilyAdapter() = default;
    FamilyAdapter(const FamilyAdapter&) = delete;
    FamilyAdapter& operator=(const FamilyAdapter&) = delete;
    FamilyAdapter(FamilyAdapter&&) = delete;
    FamilyAdapter& operator=(FamilyAdapter&&) = delete;

    /**
     * @brief ZIP/XML safety limits every document this adapter loads is opened with.
     *
     * A server opens whatever file an agent names, and an agent's documents
     * arrive from wherever the agent has been. Recommended() rather than the
     * library's Unlimited() default is therefore the adapter's own policy, and
     * it holds even for an adapter constructed directly by a test — one place
     * to forget it is one place too many. `--package-limits` overrides it
     * through SetPackageLimits before the first request is served.
     *
     * This is the explicit half of the defence. The other half is
     * OpenXmlPackage::SetDefaultPackageLimits, which RunMcpServer also sets,
     * because several tools reach the loader through `ExyokiOffice::Tools`
     * entry points that construct their own packages and take no settings.
     */
    void SetPackageLimits(OpenXmlPackageLimits limits) noexcept { m_packageLimits = limits; }
    [[nodiscard]] const OpenXmlPackageLimits& PackageLimits() const noexcept { return m_packageLimits; }

    [[nodiscard]] virtual Tools::DocumentFamily Family() const = 0;

    /// Family name used in prose and in error messages ("Word").
    [[nodiscard]] virtual std::string FamilyName() const = 0;

    /// Default document extension including the leading dot (".docx").
    [[nodiscard]] virtual std::string FileExtension() const = 0;

    /// Creates an empty document of this family; nullptr on failure.
    [[nodiscard]] virtual std::unique_ptr<DocumentHandle> CreateNew() const = 0;

    /// Opens a document from disk; nullptr when the package cannot be loaded.
    [[nodiscard]] virtual std::unique_ptr<DocumentHandle> Open(const std::filesystem::path& path) const = 0;

    /// Opens a document from a serialized package; nullptr on failure.
    [[nodiscard]] virtual std::unique_ptr<DocumentHandle> OpenFromMemory(std::span<const Byte> bytes) const = 0;

    /**
     * @brief The `ExyokiOffice::Tools` operations that need the typed editor.
     *
     * Each of these has a family-specific overload in `ExyokiOffice::Tools`
     * that takes the editor directly; the adapter exists to pick the right one
     * for a handle whose concrete type the shared core does not know. An
     * implementation downcasts @p document to its own handle and delegates.
     *
     * None of them touch the file system. Replace and Redact mutate the open
     * document and leave saving to the caller, which is what lets a server run
     * them against a document it is already holding.
     */
    [[nodiscard]] virtual Tools::DocumentModel ReadModel(DocumentHandle& document,
                                                         const Tools::ModelReadOptions& options,
                                                         std::vector<Tools::ToolDiagnostic>& diagnostics) const = 0;
    [[nodiscard]] virtual Tools::DocumentStats Stat(DocumentHandle& document) const = 0;
    [[nodiscard]] virtual Tools::ExtractedDocumentText ExtractText(DocumentHandle& document) const = 0;
    [[nodiscard]] virtual Tools::DocumentSearchResult SearchText(DocumentHandle& document, std::string_view needle,
                                                                 Size contextChars, bool useRegex,
                                                                 bool ignoreCase) const = 0;
    [[nodiscard]] virtual Tools::DocumentReplaceResult ReplaceText(DocumentHandle& document, std::string_view needle,
                                                                   std::string_view replacement, bool dryRun,
                                                                   bool useRegex, bool ignoreCase) const = 0;
    [[nodiscard]] virtual Tools::RedactResult Redact(DocumentHandle& document,
                                                     const Tools::RedactOptions& options) const = 0;

private:
    OpenXmlPackageLimits m_packageLimits = OpenXmlPackageLimits::Recommended();
};

} // namespace ExyokiOffice::Mcp
