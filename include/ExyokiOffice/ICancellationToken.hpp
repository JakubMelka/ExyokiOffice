// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

#include "ExyokiOffice/Export.hpp"

namespace ExyokiOffice
{

/**
 * @brief Cooperative cancellation interface for long-running package operations.
 *
 * Pass an implementation of ICancellationToken to load/save APIs when the caller
 * must be able to stop work from another thread or from a UI event loop. The
 * operation periodically calls IsCancelled() at safe checkpoints. When it
 * returns true, open/load APIs fail without returning a partially initialized
 * document and save APIs fail without publishing a partial target file.
 *
 * The token is non-owning: the caller must keep it alive until the API call
 * returns. Passing nullptr means the operation cannot be cancelled.
 *
 * @code
 * class CancellationFlag final : public ExyokiOffice::ICancellationToken
 * {
 * public:
 *     bool IsCancelled() const override { return cancelled; }
 *
 *     bool cancelled = false;
 * };
 *
 * CancellationFlag token;
 * auto document = ExyokiOffice::Packaging::WordDocument::Open("report.docx", {}, &token);
 * if (!document)
 * {
 *     // Loading failed or token.cancelled became true before the operation completed.
 * }
 * @endcode
 */
class EXYOKIOFFICE_EXPORT ICancellationToken
{
public:
    /**
     * @brief Destroys the cancellation token interface.
     */
    virtual ~ICancellationToken() = default;

    /**
     * @brief Returns true when the current operation should stop as soon as it is safe.
     *
     * Implementations should make this method cheap and thread-safe if the flag
     * can be changed by another thread while a package operation is running.
     */
    [[nodiscard]] virtual bool IsCancelled() const = 0;
};

} // namespace ExyokiOffice
