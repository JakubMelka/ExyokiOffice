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
 * The core library defaults to OpenXmlPackageLimits::Unlimited(), because a
 * caller reaching for `OpenXmlPackage` or an editor directly has the seam to
 * pass its own limits and may legitimately be opening a package it produced.
 * This module is used differently: `Stat`, `Diff`, `Detect`, `Redact`,
 * `Unpack` and their neighbours are pointed at files the caller did not make,
 * and most of them take no settings at all — there is nowhere to pass a limit
 * even if the caller wanted to. So the default here is Recommended(), and a
 * caller who wants no limits says so.
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
 * `Packaging::OpenSettings` starts at the core default, which is Unlimited()
 * unless the application installed a policy. That is the right starting point
 * for the library at large and the wrong one here, for the reasons
 * DefaultPackageLimits() gives: these entry points are pointed at files the
 * caller did not make. Every Tools function that opens an editor over a
 * caller-supplied path passes this; a default-constructed OpenSettings would
 * quietly widen the policy back to the core default on the way in.
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
