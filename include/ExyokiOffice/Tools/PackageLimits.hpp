// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

#include "ExyokiOffice/Export.hpp"
#include "ExyokiOffice/OpenXmlPackage.hpp"
#include "ExyokiOffice/Packaging/WordprocessingDocument.hpp"

namespace ExyokiOffice::Tools
{

/**
 * @brief ZIP/XML safety limits the Tools entry points load packages under.
 *
 * This is the same policy the core library applies — packages start at
 * OpenXmlPackageLimits::Recommended() — stated in its own function because
 * this module must not inherit "no limits" even by accident: `Stat`, `Diff`,
 * `Detect`, `Redact`, `Unpack` and their neighbours are pointed at files the
 * caller did not make, and most of them take no settings at all, so there is
 * nowhere to pass a limit even if the caller wanted to.
 *
 * An application that installed a process-wide policy with
 * OpenXmlPackage::SetDefaultPackageLimits gets that policy instead, including
 * a deliberate Unlimited(): `exyoki --package-limits unlimited` has to reach
 * these functions, or the escape hatch would not be one.
 *
 * @return The configured process-wide default, or Recommended() when the
 *         application configured none.
 */
[[nodiscard]] EXYOKIOFFICE_EXPORT OpenXmlPackageLimits DefaultPackageLimits();

/// Applies DefaultPackageLimits() to @p package, for entry points with no options struct.
EXYOKIOFFICE_EXPORT void ApplyDefaultPackageLimits(OpenXmlPackage& package);

/**
 * @brief Open settings for a document this program did not produce.
 *
 * `Packaging::OpenSettings` already starts at the core default, so this is
 * usually the same thing spelled explicitly. It stays a separate function
 * because "these bytes are foreign" is a fact about the call site rather than
 * about the process: every Tools function that opens an editor over a
 * caller-supplied path passes this, and it keeps the policy from following a
 * future change of the core default in the wrong direction.
 */
[[nodiscard]] EXYOKIOFFICE_EXPORT Packaging::OpenSettings UntrustedOpenSettings();

/**
 * @brief Open settings for reading back a package this program just wrote.
 *
 * The counterpart to UntrustedOpenSettings(): splitting and merging serialize
 * an editor to memory and open the result again, and holding that read to the
 * limits meant for foreign input checks the program against itself. The
 * compression ratio rule in particular fails that check on perfectly ordinary
 * content — a paragraph of a megabyte of one repeated character deflates far
 * past 200:1 without being a decompression bomb in any sense, because the whole
 * thing is a megabyte. Use only where the bytes demonstrably came from this
 * process.
 */
[[nodiscard]] EXYOKIOFFICE_EXPORT Packaging::OpenSettings OwnOutputOpenSettings();

} // namespace ExyokiOffice::Tools
