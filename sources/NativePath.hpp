// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

#include <filesystem>
#include <string>

namespace ExyokiOffice::NativePath
{

/**
 * @file
 * @brief Spelling a filesystem path the way the bundled ZIP layer reads it.
 *
 * miniz takes `const char*` file names and, on Windows, widens them with
 * `MultiByteToWideChar(CP_UTF8, ...)` before calling `_wfopen`. `path::string()`
 * there produces the *native narrow* spelling, which is the active code page and
 * only happens to be UTF-8 on a system configured for it. The two disagree for
 * every path outside ASCII, so `Příloha.docx` opened through `string()` reaches
 * `_wfopen` as mojibake and the package "does not exist".
 *
 * Everywhere else the native narrow encoding already is UTF-8 and the two are
 * the same string, which is why this has one implementation and one comment
 * rather than a call-site decision each time.
 */

/// @brief The UTF-8 spelling of @p path, for an API that decodes its argument as UTF-8.
[[nodiscard]] inline std::string ToUtf8(const std::filesystem::path& path)
{
    const auto utf8 = path.u8string();
    return std::string(reinterpret_cast<const char*>(utf8.data()), utf8.size());
}

} // namespace ExyokiOffice::NativePath
