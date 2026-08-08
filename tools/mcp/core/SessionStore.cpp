// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "SessionStore.hpp"

#include <algorithm>
#include <system_error>
#include <utility>

namespace ExyokiOffice::Mcp
{

DocumentSession::DocumentSession(std::string id, std::unique_ptr<DocumentHandle> document,
                                 std::filesystem::path path, Size snapshotDepth)
    : m_id(std::move(id)), m_document(std::move(document)), m_path(std::move(path)), m_snapshotDepth(snapshotDepth)
{
    CaptureFileTime();
}

bool DocumentSession::PushSnapshot()
{
    if (m_snapshotDepth == 0)
    {
        return false;
    }

    Snapshot snapshot;
    snapshot.Bytes = m_document->SaveToMemory();
    if (snapshot.Bytes.empty())
    {
        return false;
    }

    snapshot.Revision = m_revision;
    snapshot.Dirty = m_dirty;

    m_snapshots.push_back(std::move(snapshot));
    while (m_snapshots.size() > m_snapshotDepth)
    {
        m_snapshots.pop_front();
    }

    return true;
}

void DocumentSession::MarkMutated()
{
    ++m_revision;
    m_dirty = true;
}

std::optional<UInt64> DocumentSession::Undo()
{
    if (m_snapshots.empty())
    {
        return std::nullopt;
    }

    const auto snapshot = std::move(m_snapshots.back());
    m_snapshots.pop_back();
    if (!m_document->LoadFromMemory(snapshot.Bytes))
    {
        return std::nullopt;
    }

    // The content is back at the revision the snapshot was taken for. The
    // counter itself is not rewound: the caller records the undo as a mutation
    // so a client can tell that its cached indices are stale either way.
    return snapshot.Revision;
}

void DocumentSession::DiscardNewestSnapshot()
{
    if (!m_snapshots.empty())
    {
        m_snapshots.pop_back();
    }
}

bool DocumentSession::RollbackToNewestSnapshot()
{
    if (m_snapshots.empty())
    {
        return false;
    }

    const auto snapshot = std::move(m_snapshots.back());
    m_snapshots.pop_back();
    if (!m_document->LoadFromMemory(snapshot.Bytes))
    {
        return false;
    }

    m_revision = snapshot.Revision;
    m_dirty = snapshot.Dirty;
    return true;
}

void DocumentSession::MarkRollbackFailed()
{
    m_rollbackFailed = true;
}

std::optional<DocumentSession::BatchMarker> DocumentSession::BeginBatch()
{
    BatchMarker marker;
    marker.Bytes = m_document->SaveToMemory();
    if (marker.Bytes.empty())
    {
        return std::nullopt;
    }

    marker.SnapshotCount = m_snapshots.size();
    marker.Revision = m_revision;
    marker.Dirty = m_dirty;
    return marker;
}

bool DocumentSession::RollbackBatch(const BatchMarker& marker)
{
    while (m_snapshots.size() > marker.SnapshotCount)
    {
        m_snapshots.pop_back();
    }

    m_revision = marker.Revision;
    m_dirty = marker.Dirty;
    return m_document->LoadFromMemory(marker.Bytes);
}

void DocumentSession::CommitBatch(BatchMarker marker)
{
    // The individual operations each pushed their own snapshot; the batch
    // replaces all of them with the single state it started from.
    while (m_snapshots.size() > marker.SnapshotCount)
    {
        m_snapshots.pop_back();
    }

    m_revision = marker.Revision;
    m_dirty = marker.Dirty;

    if (m_snapshotDepth > 0)
    {
        Snapshot snapshot;
        snapshot.Bytes = std::move(marker.Bytes);
        snapshot.Revision = marker.Revision;
        snapshot.Dirty = marker.Dirty;

        m_snapshots.push_back(std::move(snapshot));
        while (m_snapshots.size() > m_snapshotDepth)
        {
            m_snapshots.pop_front();
        }
    }

    MarkMutated();
}

void DocumentSession::MarkSaved(std::filesystem::path path)
{
    m_path = std::move(path);
    m_dirty = false;
    CaptureFileTime();
}

bool DocumentSession::HasFileChangedOnDisk() const
{
    if (m_path.empty() || !m_fileTime.has_value())
    {
        return false;
    }

    std::error_code error;
    const auto current = std::filesystem::last_write_time(m_path, error);
    if (error)
    {
        // The file disappeared or became unreadable; saving over it is not a
        // lost-update hazard, so this is deliberately not reported as a change.
        return false;
    }

    return current != *m_fileTime;
}

void DocumentSession::CaptureFileTime()
{
    m_fileTime.reset();
    if (m_path.empty())
    {
        return;
    }

    std::error_code error;
    const auto written = std::filesystem::last_write_time(m_path, error);
    if (!error)
    {
        m_fileTime = written;
    }
}

SessionStore::SessionStore(Size maxDocuments, Size snapshotDepth)
    : m_maxDocuments(maxDocuments), m_snapshotDepth(snapshotDepth)
{
}

DocumentSession* SessionStore::Create(std::unique_ptr<DocumentHandle> document, std::filesystem::path path)
{
    if (m_sessions.size() >= m_maxDocuments)
    {
        return nullptr;
    }

    auto id = "doc-" + std::to_string(m_nextId);
    ++m_nextId;

    m_sessions.push_back(
        std::make_unique<DocumentSession>(std::move(id), std::move(document), std::move(path), m_snapshotDepth));
    return m_sessions.back().get();
}

DocumentSession* SessionStore::Find(std::string_view id) const
{
    const auto match = std::find_if(m_sessions.begin(), m_sessions.end(),
                                    [id](const std::unique_ptr<DocumentSession>& session)
                                    { return session->Id() == id; });
    return match == m_sessions.end() ? nullptr : match->get();
}

bool SessionStore::Close(std::string_view id)
{
    const auto match = std::find_if(m_sessions.begin(), m_sessions.end(),
                                    [id](const std::unique_ptr<DocumentSession>& session)
                                    { return session->Id() == id; });
    if (match == m_sessions.end())
    {
        return false;
    }

    m_sessions.erase(match);
    return true;
}

DocumentSession* SessionStore::FindByPath(const std::filesystem::path& path) const
{
    if (path.empty())
    {
        return nullptr;
    }

    const auto match = std::find_if(m_sessions.begin(), m_sessions.end(),
                                    [&path](const std::unique_ptr<DocumentSession>& session)
                                    { return session->Path() == path; });
    return match == m_sessions.end() ? nullptr : match->get();
}

} // namespace ExyokiOffice::Mcp
