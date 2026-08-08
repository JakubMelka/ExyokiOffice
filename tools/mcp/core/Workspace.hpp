// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

#include "ExyokiOffice/StandardTypes.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ExyokiOffice::Mcp
{

/** @brief One file listed by Workspace::List. */
struct WorkspaceFile
{
    /// Path relative to its workspace root, with `/` separators.
    std::string Path;
    UInt64 Bytes = 0;
    /// Last write time as an ISO 8601 UTC timestamp.
    std::string Modified;
};

/** @brief One page of Workspace::List together with its completeness. */
struct WorkspaceListing
{
    /// Matching files in ascending path order.
    std::vector<WorkspaceFile> Files;
    /// True when more files matched than the caller asked to see.
    bool Truncated = false;
};

/**
 * @brief The sandbox every client-supplied path is resolved against.
 *
 * A path arriving from an agent is never used as given. Its shape is screened
 * first (IsRefusedForm), then it is made absolute against the first root,
 * canonicalized — which resolves `..` segments and expands symlinks — and only
 * then tested for containment in one of the configured roots. Anything that
 * lands outside is rejected, and so is anything that could not be canonicalized
 * at all: without the canonical form there is nothing to decide containment on.
 * The servers have no legitimate reason to touch the rest of the file system,
 * which is also what their `openWorldHint: false` annotation promises.
 */
class Workspace
{
public:
    /// Uses the current working directory when @p roots is empty.
    explicit Workspace(std::vector<std::filesystem::path> roots);

    [[nodiscard]] const std::vector<std::filesystem::path>& Roots() const noexcept { return m_roots; }

    /**
     * @brief Resolves a client path to an absolute path inside the sandbox.
     *
     * @return std::nullopt when the path has a refused shape, cannot be
     *         canonicalized, or escapes every root.
     */
    [[nodiscard]] std::optional<std::filesystem::path> Resolve(std::string_view path) const;

    /**
     * @brief True for a path shape a client may not name at all.
     *
     * Screened before anything touches the file system, because several of
     * these forms have an effect merely by being opened: a UNC path makes the
     * process authenticate against a host the client chose, and a device name
     * opens the device. The rest are forms whose meaning does not survive the
     * checks built on top of a path — an alternate data stream hides bytes in a
     * file that looks ordinary, and a trailing dot changes what `extension()`
     * reports without changing which file is written.
     *
     * The rules are the platform's, exactly like the case folding in the
     * containment test: a colon and a name of `nul.txt` are ordinary on a
     * POSIX file system and are refused only on Windows. Which spellings a
     * single component may not use is `Tools::IsPlainOutputName`, so the list
     * of device names lives in one place.
     */
    [[nodiscard]] static bool IsRefusedForm(std::string_view path);

    /**
     * @brief True when @p name is usable as one file-name component.
     *
     * For arguments that are not paths but end up inside one, such as the
     * output prefix of `split_document`. This is `Tools::IsPlainOutputName`,
     * the same question the library answers about a name it derives itself,
     * and therefore the same answer on every platform — unlike IsRefusedForm,
     * which decides whether a client may *name* an existing path and follows
     * the platform for that.
     */
    [[nodiscard]] static bool IsAcceptableName(std::string_view name);

    /// Renders an absolute path relative to its root, with `/` separators.
    [[nodiscard]] std::string Relativize(const std::filesystem::path& path) const;

    /**
     * @brief Lists workspace files matching a filename glob.
     *
     * The glob applies to the whole relative path and understands `*` (any run
     * of characters except the separator), `**` (any run including separators),
     * and `?`. Matching is case-insensitive, which keeps the behavior identical
     * on Windows and on case-sensitive file systems.
     *
     * The whole match set is ordered before @p limit is applied, so a limited
     * listing is the first page of a stable order rather than whichever files
     * the directory walk happened to reach first.
     */
    [[nodiscard]] WorkspaceListing List(std::string_view glob, Size limit) const;

    /// Tests one relative path against a glob using the rules of List().
    [[nodiscard]] static bool MatchesGlob(std::string_view path, std::string_view glob);

    /// Formats a file system timestamp as ISO 8601 UTC, or an empty string on failure.
    [[nodiscard]] static std::string FormatTimestamp(std::filesystem::file_time_type time);

private:
    std::vector<std::filesystem::path> m_roots;
};

} // namespace ExyokiOffice::Mcp
