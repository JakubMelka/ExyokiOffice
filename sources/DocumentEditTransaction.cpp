// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "ExyokiOffice/DocumentEditTransaction.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <utility>

namespace ExyokiOffice
{

DocumentEditMemento::DocumentEditMemento(DocumentFamily family, std::vector<Byte> bytes)
    : m_family(family),
      m_bytes(std::move(bytes))
{
}

std::optional<DocumentEditMemento> DocumentEditMemento::FromBytes(
    DocumentFamily family,
    std::span<const Byte> bytes)
{
    if (bytes.empty())
    {
        return std::nullopt;
    }

    return DocumentEditMemento(family, std::vector<Byte>(bytes.begin(), bytes.end()));
}

DocumentFamily DocumentEditMemento::Family() const noexcept
{
    return m_family;
}

std::span<const Byte> DocumentEditMemento::Bytes() const noexcept
{
    return m_bytes;
}

ExyokiOffice::Size DocumentEditMemento::Size() const noexcept
{
    return m_bytes.size();
}

struct DocumentEditTransaction::Impl
{
    Impl(DocumentEditMemento value,
         RestoreFunction function,
         std::weak_ptr<detail::DocumentEditTransactionOwner> owner,
         const void* ownerIdentity)
        : Memento(std::move(value)),
          Restore(std::move(function)),
          Owner(std::move(owner)),
          OwnerIdentity(ownerIdentity)
    {
    }

    DocumentEditMemento Memento;
    RestoreFunction Restore;
    std::weak_ptr<detail::DocumentEditTransactionOwner> Owner;
    const void* OwnerIdentity = nullptr;
    bool Active = true;
};

DocumentEditTransaction::DocumentEditTransaction() noexcept = default;

DocumentEditTransaction::DocumentEditTransaction(
    DocumentEditMemento memento,
    RestoreFunction restore,
    std::weak_ptr<detail::DocumentEditTransactionOwner> owner,
    const void* ownerIdentity)
    : m_impl(std::make_unique<Impl>(
          std::move(memento), std::move(restore), std::move(owner), ownerIdentity))
{
}

DocumentEditTransaction::DocumentEditTransaction(DocumentEditTransaction&& other) noexcept = default;

DocumentEditTransaction& DocumentEditTransaction::operator=(DocumentEditTransaction&& other) noexcept
{
    if (this != &other)
    {
        if (IsActive())
        {
            Rollback();
        }
        m_impl = std::move(other.m_impl);
    }
    return *this;
}

DocumentEditTransaction::~DocumentEditTransaction()
{
    if (IsActive())
    {
        Rollback();
    }
}

bool DocumentEditTransaction::IsActive() const noexcept
{
    if (!m_impl || !m_impl->Active)
    {
        return false;
    }

    auto owner = m_impl->Owner.lock();
    return owner && owner->IsAlive(m_impl->OwnerIdentity);
}

bool DocumentEditTransaction::Commit() noexcept
{
    if (!IsActive())
    {
        return false;
    }

    auto owner = m_impl->Owner.lock();
    m_impl->Active = false;
    m_impl->Restore = {};
    if (owner)
    {
        owner->EndTransaction(m_impl->OwnerIdentity);
    }
    return true;
}

bool DocumentEditTransaction::Rollback() noexcept
{
    if (!IsActive())
    {
        return false;
    }

    auto owner = m_impl->Owner.lock();
    m_impl->Active = false;
    auto restore = std::move(m_impl->Restore);
    if (owner)
    {
        owner->EndTransaction(m_impl->OwnerIdentity);
    }
    if (!restore)
    {
        return false;
    }

    try
    {
        return restore(m_impl->Memento);
    }
    catch (...)
    {
        return false;
    }
}

} // namespace ExyokiOffice
