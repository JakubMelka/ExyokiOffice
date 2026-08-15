// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

#include "ExyokiOffice/Export.hpp"

#include <filesystem>
#include <string>
#include <string_view>

namespace ExyokiOffice::Tools
{

/**
 * @brief True when @p name is a single file name a tool may write into an output directory.
 *
 * Several tools name their output after something the caller does not control:
 * `ExportMedia` uses the file name of a part URI, and the split tools
 * concatenate a caller-supplied prefix into every numbered file. Such a name
 * has to stay one component of the chosen directory, so this refuses:
 *
 * - a path separator, `.` and `..`, which would leave the directory;
 * - `:`, which names a drive on Windows and otherwise opens an NTFS alternate
 *   data stream, hiding the payload inside a file that looks ordinary;
 * - a reserved DOS device name (`NUL`, `CON`, `COM1`, ... , with or without an
 *   extension) — writing to one reports success and stores nothing;
 * - a trailing dot or space, which Windows strips before opening the file, so
 *   the name that was checked is not the name that gets written;
 * - a control character.
 *
 * The rules are identical on every platform on purpose. An output directory is
 * routinely copied to another machine, and a document is not trusted input:
 * a name that is safe only where it happened to be produced is not safe.
 */
[[nodiscard]] EXYOKIOFFICE_EXPORT bool IsPlainOutputName(std::string_view name) noexcept;

/**
 * @brief Rewrites @p name into one IsPlainOutputName() accepts.
 *
 * Refused characters become `_`; when nothing usable is left — the name was
 * empty, a traversal segment, or a device name — @p fallback is returned. This
 * is for names derived from document content, where refusing would let one
 * hostile part name stop an entire export; the caller's collision handling
 * keeps two names that were rewritten the same way apart.
 */
[[nodiscard]] EXYOKIOFFICE_EXPORT std::string MakePlainOutputName(std::string_view name, std::string_view fallback);

/**
 * @brief True when @p path is @p directory or lies below it.
 *
 * Lexical and component-wise: both paths are normalized, and every component of
 * @p directory must open @p path. Nothing is read from the file system, so this
 * does not resolve symbolic links — it is the containment check for a path a
 * tool has just composed from a directory it was given and a name it derived,
 * not a sandbox check for a path handed in from outside.
 */
[[nodiscard]] EXYOKIOFFICE_EXPORT bool IsInsideDirectory(const std::filesystem::path& directory,
                                                         const std::filesystem::path& path);

} // namespace ExyokiOffice::Tools
