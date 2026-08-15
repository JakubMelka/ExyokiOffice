// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

#include "ExyokiOffice/Export.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace ExyokiOffice
{

namespace detail
{

/**
 * @brief Lifetime identity shared weakly with edit transactions.
 *
 * This is an implementation detail exposed in the header only because document
 * editors store it without adding separate editor implementation objects.
 */
class DocumentEditTransactionOwner
{
public:
    explicit DocumentEditTransactionOwner(const void* owner) noexcept
        : m_owner(owner)
    {
    }

    [[nodiscard]] bool IsAlive(const void* owner) const noexcept
    {
        return m_owner.load(std::memory_order_acquire) == owner;
    }

    [[nodiscard]] bool TryBeginTransaction(const void* owner) noexcept
    {
        if (!IsAlive(owner))
        {
            return false;
        }

        bool expected = false;
        return m_transactionActive.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel, std::memory_order_acquire);
    }

    void EndTransaction(const void* owner) noexcept
    {
        if (IsAlive(owner))
        {
            m_transactionActive.store(false, std::memory_order_release);
        }
    }

    void Invalidate(const void* owner) noexcept
    {
        const void* expected = owner;
        if (m_owner.compare_exchange_strong(
                expected, nullptr, std::memory_order_acq_rel, std::memory_order_acquire))
        {
            m_transactionActive.store(false, std::memory_order_release);
        }
    }

private:
    std::atomic<const void*> m_owner;
    std::atomic<bool> m_transactionActive = false;
};

class DocumentEditTransactionStarter;

} // namespace detail

namespace Excel
{
class ExcelDocumentEditor;
}

namespace PowerPoint
{
class PowerPointDocumentEditor;
}

namespace Word
{
class WordDocumentEditor;
}

/**
 * @brief Identifies the Office document family stored in an edit memento.
 */
enum class DocumentFamily
{
    Word,
    Excel,
    PowerPoint
};

/**
 * @brief Immutable snapshot of a complete editable Office package.
 *
 * A memento contains the serialized OPC ZIP package, including XML DOM state,
 * package parts, relationships, content types, and binary payloads. It can be
 * retained independently of the editor that created it and restored into
 * another editor of the same document family.
 *
 * The byte representation is intentionally exposed so applications can store
 * mementos in memory, persist them externally, or account for their memory
 * usage. The returned span remains valid for the lifetime of this memento.
 */
class EXYOKIOFFICE_EXPORT DocumentEditMemento
{
public:
    DocumentEditMemento(const DocumentEditMemento&) = default;
    DocumentEditMemento& operator=(const DocumentEditMemento&) = default;
    DocumentEditMemento(DocumentEditMemento&&) noexcept = default;
    DocumentEditMemento& operator=(DocumentEditMemento&&) noexcept = default;
    ~DocumentEditMemento() = default;

    /**
     * @brief Reconstructs a memento from a previously retained package buffer.
     *
     * The package is parsed only when an editor restores the memento. This
     * factory rejects an empty buffer but otherwise preserves bytes exactly.
     *
     * @param family Office family that is allowed to restore the package.
     * @param bytes Complete serialized OPC ZIP package.
     * @return Reconstructed memento, or `std::nullopt` for an empty buffer.
     */
    static std::optional<DocumentEditMemento> FromBytes(
        DocumentFamily family,
        std::span<const Byte> bytes);

    /** @return The Office document family accepted by this memento. */
    DocumentFamily Family() const noexcept;

    /** @return Read-only access to the complete serialized OPC package. */
    std::span<const Byte> Bytes() const noexcept;

    /** @return The number of serialized bytes retained by this memento. */
    ExyokiOffice::Size Size() const noexcept;

private:
    friend class Excel::ExcelDocumentEditor;
    friend class PowerPoint::PowerPointDocumentEditor;
    friend class Word::WordDocumentEditor;

    DocumentEditMemento(DocumentFamily family, std::vector<Byte> bytes);

    DocumentFamily m_family;
    std::vector<Byte> m_bytes;
};

/**
 * @brief Scope-bound, all-or-nothing edit transaction.
 *
 * BeginTransaction() on a Word, Excel, or PowerPoint editor captures a complete
 * package memento. Call Commit() after every step succeeds. If Commit() is not
 * called, destruction attempts to restore the captured package automatically.
 * Rollback() is available when the caller needs an explicit success result.
 *
 * Transactions are move-only. An editor permits at most one active transaction;
 * a concurrent or nested BeginTransaction() call returns an inactive
 * transaction without capturing another snapshot. A new transaction can be
 * started after the active transaction is committed, rolled back, or destroyed.
 * A transaction may physically outlive its editor, but it becomes inactive
 * immediately when that editor is destroyed; Commit(), Rollback(), and the
 * destructor then perform no editor access.
 * This API is entirely opt-in: editors do not create snapshots or change their
 * lifecycle, mutation, or save behavior unless BeginTransaction(),
 * CreateMemento(), or RestoreMemento() is called explicitly.
 *
 * Restoring a transaction replaces the editor's complete low-level document
 * graph. Consequently, wrappers, cursors, parts, and DOM nodes obtained before
 * rollback continue to refer to the abandoned graph and must not be reused.
 * Obtain fresh wrappers from the editor after rollback.
 *
 * @code
 * auto transaction = editor->BeginTransaction();
 * if (!transaction.IsActive())
 * {
 *     return false;
 * }
 *
 * if (!ApplyFirstStep(*editor) || !ApplySecondStep(*editor))
 * {
 *     return transaction.Rollback();
 * }
 *
 * return transaction.Commit();
 * @endcode
 */
class EXYOKIOFFICE_EXPORT DocumentEditTransaction
{
public:
    DocumentEditTransaction() noexcept;
    DocumentEditTransaction(const DocumentEditTransaction&) = delete;
    DocumentEditTransaction& operator=(const DocumentEditTransaction&) = delete;
    DocumentEditTransaction(DocumentEditTransaction&& other) noexcept;
    DocumentEditTransaction& operator=(DocumentEditTransaction&& other) noexcept;
    ~DocumentEditTransaction();

    /**
     * @brief Tests whether this transaction can still be committed or rolled back.
     */
    [[nodiscard]] bool IsActive() const noexcept;

    /**
     * @brief Accepts all changes made since the transaction began.
     *
     * @return True when the transaction was active and is now committed.
     */
    bool Commit() noexcept;

    /**
     * @brief Restores the complete package snapshot captured at transaction start.
     *
     * @return True when restoration succeeded. False means the transaction was
     * inactive or the serialized package could not be reopened.
     */
    bool Rollback() noexcept;

private:
    friend class Excel::ExcelDocumentEditor;
    friend class PowerPoint::PowerPointDocumentEditor;
    friend class Word::WordDocumentEditor;
    friend class detail::DocumentEditTransactionStarter;

    using RestoreFunction = std::function<bool(const DocumentEditMemento&)>;

    DocumentEditTransaction(
        DocumentEditMemento memento,
        RestoreFunction restore,
        std::weak_ptr<detail::DocumentEditTransactionOwner> owner,
        const void* ownerIdentity);

    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

namespace detail
{

/**
 * @brief The begin-transaction protocol, shared by the three document editors.
 *
 * Word, Excel and PowerPoint differ only in how they snapshot and restore
 * themselves; the ordering around that is identical and easy to get subtly
 * wrong. Ownership has to be (re)established before the flag is claimed, a
 * refused claim must not run the snapshot, and a snapshot that fails - by
 * returning nothing or by throwing - has to release the flag again, or the
 * editor stays locked out of transactions for the rest of its life.
 */
class DocumentEditTransactionStarter
{
public:
    /**
     * @param owner The editor's owner handle; created or replaced when the
     *              editor does not own a live one yet.
     * @param identity The editor address the owner handle is keyed on.
     * @param createMemento Returns `std::optional<DocumentEditMemento>`.
     * @param restoreMemento Restores a memento into the same editor.
     * @return An inactive transaction when another one is already running or
     *         the snapshot could not be taken.
     */
    template <typename TCreateMemento, typename TRestoreMemento>
    static DocumentEditTransaction Begin(std::shared_ptr<DocumentEditTransactionOwner>& owner,
                                         const void* identity,
                                         TCreateMemento&& createMemento,
                                         TRestoreMemento&& restoreMemento)
    {
        if (!owner || !owner->IsAlive(identity))
        {
            owner = std::make_shared<DocumentEditTransactionOwner>(identity);
        }

        if (!owner->TryBeginTransaction(identity))
        {
            return {};
        }

        try
        {
            auto memento = createMemento();
            if (!memento)
            {
                owner->EndTransaction(identity);
                return {};
            }

            return DocumentEditTransaction(
                std::move(*memento), std::forward<TRestoreMemento>(restoreMemento), owner, identity);
        }
        catch (...)
        {
            owner->EndTransaction(identity);
            throw;
        }
    }
};

} // namespace detail

} // namespace ExyokiOffice
