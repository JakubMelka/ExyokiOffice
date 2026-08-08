// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

#include "FamilyAdapter.hpp"

#include "ExyokiOffice/StandardTypes.hpp"

#include <deque>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ExyokiOffice::Mcp
{

/**
 * @brief One document kept open between tool calls.
 *
 * The session is what makes the servers cheap to use: an agent opens a
 * document once, mutates it through many calls, and saves once. `Revision`
 * counts successful mutations so a client can detect that its view of the
 * block or slide indices is stale, and `Dirty` tells it whether a save is
 * still owed.
 */
class DocumentSession
{
public:
    DocumentSession(std::string id, std::unique_ptr<DocumentHandle> document, std::filesystem::path path,
                    Size snapshotDepth);

    [[nodiscard]] const std::string& Id() const noexcept { return m_id; }
    [[nodiscard]] const std::filesystem::path& Path() const noexcept { return m_path; }
    [[nodiscard]] UInt64 Revision() const noexcept { return m_revision; }
    [[nodiscard]] bool IsDirty() const noexcept { return m_dirty; }
    [[nodiscard]] DocumentHandle& Document() const noexcept { return *m_document; }
    [[nodiscard]] Tools::DocumentFamily Family() const { return m_document->Family(); }

    /**
     * @brief Captures the current state so the next mutation can be undone.
     *
     * Called before a mutation is attempted, never after: a failed mutation
     * that already touched the document is rolled back by the same snapshot.
     *
     * @return False when nothing was captured, either because the configured
     *         snapshot depth is zero or because the document could not be
     *         serialized. A caller that intends to roll back must check this:
     *         restoring "the newest snapshot" it never took would revert an
     *         earlier, successful mutation instead.
     */
    bool PushSnapshot();

    /// Counts one successful mutation and marks the session unsaved.
    void MarkMutated();

    /**
     * @brief Restores the newest snapshot as an undo step.
     *
     * @return The revision the restored content corresponds to, or nullopt
     *         when no snapshot was available or restoring failed. The revision
     *         counter itself keeps rising; the caller marks the undo as a
     *         mutation of its own.
     */
    [[nodiscard]] std::optional<UInt64> Undo();

    /// Drops the newest snapshot without restoring it (a mutation that succeeded cleanly).
    void DiscardNewestSnapshot();

    /**
     * @brief Restores the newest snapshot and drops it (failed-mutation rollback).
     *
     * Also restores the revision and dirty bookkeeping captured with it, so a
     * mutation that failed leaves no trace at all.
     */
    bool RollbackToNewestSnapshot();

    [[nodiscard]] bool HasSnapshots() const noexcept { return !m_snapshots.empty(); }

    /**
     * @brief Records that a rollback failed and the document may be partial.
     *
     * Sticky for the lifetime of the session: every later result carries a
     * warning, because silently handing back a half-edited document is the one
     * outcome an agent cannot detect on its own.
     */
    void MarkRollbackFailed();

    [[nodiscard]] bool RollbackFailed() const noexcept { return m_rollbackFailed; }

    /// Records a successful save; resets the dirty flag and the on-disk timestamp.
    void MarkSaved(std::filesystem::path path);

    /// Last write time captured when the file was opened or last saved.
    [[nodiscard]] const std::optional<std::filesystem::file_time_type>& FileTime() const noexcept
    {
        return m_fileTime;
    }

    /// True when the file on disk changed since this session last touched it.
    [[nodiscard]] bool HasFileChangedOnDisk() const;

    /**
     * @brief State captured at the start of a batch.
     *
     * A batch is one undo unit even though it runs many mutating tools, each
     * of which snapshots and counts its own revision. The marker restores both
     * bookkeeping items so the batch collapses into a single step.
     */
    struct BatchMarker
    {
        std::vector<Byte> Bytes;
        Size SnapshotCount = 0;
        UInt64 Revision = 0;
        bool Dirty = false;
    };

    /// Captures the state a batch can be rolled back to; nullopt on failure.
    [[nodiscard]] std::optional<BatchMarker> BeginBatch();

    /// Restores the marker's state and discards everything the batch did.
    bool RollbackBatch(const BatchMarker& marker);

    /// Collapses the batch into one snapshot and one revision increment.
    void CommitBatch(BatchMarker marker);

private:
    /** @brief One captured document state and the bookkeeping it belongs to. */
    struct Snapshot
    {
        std::vector<Byte> Bytes;
        /// Revision the captured content corresponds to.
        UInt64 Revision = 0;
        /// Whether the session owed a save when the state was captured.
        bool Dirty = false;
    };

    void CaptureFileTime();

    std::string m_id;
    std::unique_ptr<DocumentHandle> m_document;
    std::filesystem::path m_path;
    Size m_snapshotDepth = 0;
    UInt64 m_revision = 0;
    bool m_dirty = false;
    bool m_rollbackFailed = false;
    std::optional<std::filesystem::file_time_type> m_fileTime;
    std::deque<Snapshot> m_snapshots;
};

/** @brief The process-wide table of open documents. */
class SessionStore
{
public:
    SessionStore(Size maxDocuments, Size snapshotDepth);

    /// Creates a session; nullptr when the configured document limit is reached.
    [[nodiscard]] DocumentSession* Create(std::unique_ptr<DocumentHandle> document, std::filesystem::path path);

    [[nodiscard]] DocumentSession* Find(std::string_view id) const;

    /// Finds an already open session for a file path, or nullptr.
    [[nodiscard]] DocumentSession* FindByPath(const std::filesystem::path& path) const;

    bool Close(std::string_view id);

    [[nodiscard]] const std::vector<std::unique_ptr<DocumentSession>>& Sessions() const noexcept
    {
        return m_sessions;
    }

    [[nodiscard]] Size Count() const noexcept { return m_sessions.size(); }
    [[nodiscard]] Size MaxDocuments() const noexcept { return m_maxDocuments; }

private:
    Size m_maxDocuments = 16;
    Size m_snapshotDepth = 8;
    UInt64 m_nextId = 1;
    std::vector<std::unique_ptr<DocumentSession>> m_sessions;
};

} // namespace ExyokiOffice::Mcp
