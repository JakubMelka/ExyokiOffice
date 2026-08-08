// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

#include "SessionStore.hpp"
#include "ToolContext.hpp"
#include "ToolRegistry.hpp"

#include "ExyokiOffice/Tools/PackageLimits.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ExyokiOffice::Mcp
{

/**
 * @brief Registers the tools every server offers.
 *
 * The lifecycle, reading, editing, and file-utility groups are identical for
 * Word, Excel, and PowerPoint; the family adapter is what makes them operate
 * on the right documents. A few schemas are narrowed by family — conversion
 * formats and the default workspace glob, for instance — which is why the
 * adapter is needed at registration time and not only at call time.
 */
void RegisterSharedToolset(ToolRegistry& registry, const FamilyAdapter& adapter);

/**
 * @brief Resolves the `documentId` / `path` pair a reading tool accepts.
 *
 * Exactly one of the two must be present. A `documentId` addresses an open
 * session and sees its unsaved changes; a `path` opens the file transiently
 * for the duration of the call and creates no session.
 */
class DocumentAccess
{
public:
    DocumentAccess(ToolContext& context, const nlohmann::json& arguments);

    DocumentAccess(const DocumentAccess&) = delete;
    DocumentAccess& operator=(const DocumentAccess&) = delete;
    DocumentAccess(DocumentAccess&&) = delete;
    DocumentAccess& operator=(DocumentAccess&&) = delete;
    ~DocumentAccess() = default;

    [[nodiscard]] bool IsValid() const noexcept { return m_valid; }
    /// The error envelope to return; meaningful only when IsValid() is false.
    [[nodiscard]] const ToolOutcome& Failure() const noexcept { return m_failure; }
    [[nodiscard]] DocumentHandle& Document() const noexcept { return *m_document; }
    /// The addressed session, or nullptr when the tool was pointed at a path.
    [[nodiscard]] DocumentSession* Session() const noexcept { return m_session; }

private:
    std::unique_ptr<DocumentHandle> m_transient;
    DocumentHandle* m_document = nullptr;
    DocumentSession* m_session = nullptr;
    ToolOutcome m_failure;
    bool m_valid = false;
};

/**
 * @brief Guards one document mutation.
 *
 * Takes an undo snapshot on construction and restores it unless the handler
 * commits, so a tool that fails halfway leaves the session exactly as it was.
 * A committed guard keeps the snapshot in the ring buffer, which is what
 * `undo` later restores.
 *
 * The guard rolls back only a snapshot it took itself. With `--snapshot-depth
 * 0`, or when the document cannot be serialized, there is none — and undoing
 * "the newest snapshot" would then discard an earlier successful mutation
 * instead of the failed one.
 */
class MutationGuard
{
public:
    explicit MutationGuard(DocumentSession& session);
    ~MutationGuard();

    MutationGuard(const MutationGuard&) = delete;
    MutationGuard& operator=(const MutationGuard&) = delete;
    MutationGuard(MutationGuard&&) = delete;
    MutationGuard& operator=(MutationGuard&&) = delete;

    /// Counts the mutation and keeps the snapshot as the next undo point.
    void Commit();

private:
    DocumentSession* m_session = nullptr;
    bool m_committed = false;
    bool m_snapshotTaken = false;
};

/** @brief Helpers shared by the family toolsets. */
class ToolSupport
{
public:
    /// The `documentId` and `path` properties every reading tool publishes.
    static void AddDocumentSourceProperties(nlohmann::json& properties);

    /// The `documentId` property every mutating session tool requires.
    static void AddDocumentIdProperty(nlohmann::json& properties);

    /// Looks up the session a mutating tool addresses; nullptr fills @p failure.
    [[nodiscard]] static DocumentSession* RequireSession(ToolContext& context, const nlohmann::json& arguments,
                                                         ToolOutcome& failure);

    /// Resolves a client path inside the workspace sandbox.
    [[nodiscard]] static std::optional<std::filesystem::path> ResolvePath(ToolContext& context,
                                                                          std::string_view path,
                                                                          ToolOutcome& failure);

    /// Resolves a path that must already exist and be a regular file.
    [[nodiscard]] static std::optional<std::filesystem::path> ResolveExistingFile(ToolContext& context,
                                                                                  std::string_view path,
                                                                                  ToolOutcome& failure);

    /// Resolves a destination, rejecting an existing file unless @p overwrite.
    [[nodiscard]] static std::optional<std::filesystem::path> ResolveOutputPath(ToolContext& context,
                                                                                std::string_view path,
                                                                                bool overwrite,
                                                                                ToolOutcome& failure);

    /**
     * @brief Refuses a destination whose extension names another Office family.
     *
     * Every tool that writes a document writes this server's family, whatever
     * the destination is called. Accepting `report.xlsx` from the Word server
     * would produce a Word package under a name Excel is expected to open, and
     * the client only finds out when something else fails to load it.
     *
     * An extension this server does not recognize as an Office one passes: it
     * may be a deliberate working name, and refusing it would not protect
     * anything.
     */
    [[nodiscard]] static bool CheckDestinationFamily(ToolContext& context, std::string_view path,
                                                     ToolOutcome& failure);

    /// Decodes a base64 payload; std::nullopt when the text is not valid base64.
    [[nodiscard]] static std::optional<std::vector<Byte>> DecodeBase64(std::string_view text);

    /// Encodes bytes as base64 without line breaks.
    [[nodiscard]] static std::string EncodeBase64(std::span<const Byte> bytes);

    /**
     * @brief Loads image bytes from either a workspace path or a base64 payload.
     *
     * Every tool that accepts a picture takes the same `path` or
     * `dataBase64` + `contentType` pair, so the resolution, the size limit,
     * and the content-type detection live in one place.
     */
    [[nodiscard]] static bool LoadImagePayload(ToolContext& context, const nlohmann::json& arguments,
                                               std::vector<Byte>& bytes, std::string& contentType,
                                               ToolOutcome& failure);

    /// Renders a Tools document family as its lowercase token.
    [[nodiscard]] static std::string FamilyToken(Tools::DocumentFamily family);

    /// Reads a whole file into memory; false when it cannot be read.
    [[nodiscard]] static bool ReadFileBytes(const std::filesystem::path& path, std::vector<Byte>& bytes);
};

} // namespace ExyokiOffice::Mcp
