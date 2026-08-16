// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "Workspace.hpp"

#include "ExyokiOffice/Tools/OutputNaming.hpp"

#include "AsciiText.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <format>
#include <system_error>

namespace ExyokiOffice::Mcp
{

/// File-local helpers for path containment and glob matching.
class WorkspacePathHelper
{
public:
    /// True when @p candidate is @p root itself or lives underneath it.
    static bool IsInside(const std::filesystem::path& root, const std::filesystem::path& candidate)
    {
        auto rootIterator = root.begin();
        auto candidateIterator = candidate.begin();
        for (; rootIterator != root.end(); ++rootIterator, ++candidateIterator)
        {
            if (candidateIterator == candidate.end())
            {
                return false;
            }

            if (!ComponentsEqual(rootIterator->string(), candidateIterator->string()))
            {
                return false;
            }
        }

        return true;
    }

    /**
     * @brief Compares one path component the way the file system would.
     *
     * Containment decides whether a path escapes the sandbox, so it must follow
     * the platform rather than a convenience rule: on a case-sensitive file
     * system `/home/u/WS` and `/home/u/ws` are different directories, and
     * folding their case would let a path out of the workspace.
     */
    static bool ComponentsEqual(std::string_view left, std::string_view right)
    {
#if defined(_WIN32)
        return AsciiText::EqualsIgnoreCase(left, right);
#else
        return left == right;
#endif
    }

    /**
     * @brief True for one path component a client-supplied path may not contain.
     *
     * Which spellings a single file name may not use — an alternate data stream
     * (`report.docx:hidden`, and `report:evil.docx` whose extension() still
     * reads `.docx` so the family check passes), a DOS device name reached with
     * or without an extension, a trailing dot or space Win32 strips before
     * opening the file — is `Tools::IsPlainOutputName`. The rule is kept there
     * because the library has to apply it to names it derives from a document
     * itself, and one list of device names is one list to keep correct.
     *
     * What stays here is the part that is about a *path*: which components are
     * exempt, and that the rules follow the platform, exactly like the case
     * folding in the containment test.
     */
    static bool IsRefusedComponent(std::string_view component)
    {
#if defined(_WIN32)
        // A trailing separator makes the last component empty, and `.` and `..`
        // are ordinary navigation even though they end in a dot; a path that
        // navigates out of the workspace is reported as leaving it, not as
        // malformed.
        if (component.empty() || component == "." || component == "..")
        {
            return false;
        }

        return !Tools::IsPlainOutputName(component);
#else
        static_cast<void>(component);
        return false;
#endif
    }

    /// Recursive glob matcher supporting `?`, `*`, and `**`.
    static bool Match(std::string_view path, Size pathIndex, std::string_view glob, Size globIndex)
    {
        while (globIndex < glob.size())
        {
            const char pattern = glob[globIndex];
            if (pattern == '*')
            {
                const bool crossesSeparators = globIndex + 1 < glob.size() && glob[globIndex + 1] == '*';
                Size nextGlobIndex = globIndex + (crossesSeparators ? 2 : 1);

                // "**/" also matches zero directory levels, so the separator
                // that follows it is optional.
                if (crossesSeparators && nextGlobIndex < glob.size() && glob[nextGlobIndex] == '/')
                {
                    if (Match(path, pathIndex, glob, nextGlobIndex + 1))
                    {
                        return true;
                    }
                }

                for (Size candidate = pathIndex; candidate <= path.size(); ++candidate)
                {
                    if (Match(path, candidate, glob, nextGlobIndex))
                    {
                        return true;
                    }

                    if (!crossesSeparators && candidate < path.size() && path[candidate] == '/')
                    {
                        break;
                    }
                }

                return false;
            }

            if (pathIndex >= path.size())
            {
                return false;
            }

            if (pattern != '?' && AsciiText::ToLower(pattern) != AsciiText::ToLower(path[pathIndex]))
            {
                return false;
            }

            ++globIndex;
            ++pathIndex;
        }

        return pathIndex == path.size();
    }
};

Workspace::Workspace(std::vector<std::filesystem::path> roots)
{
    if (roots.empty())
    {
        std::error_code error;
        auto current = std::filesystem::current_path(error);
        if (!error)
        {
            roots.push_back(std::move(current));
        }
    }

    for (const auto& root : roots)
    {
        std::error_code error;
        auto canonical = std::filesystem::weakly_canonical(root, error);
        if (error || canonical.empty())
        {
            canonical = std::filesystem::absolute(root, error);
        }

        if (canonical.empty())
        {
            continue;
        }

        canonical = canonical.lexically_normal();
        if (std::find(m_roots.begin(), m_roots.end(), canonical) == m_roots.end())
        {
            m_roots.push_back(std::move(canonical));
        }
    }
}

bool Workspace::IsRefusedForm(std::string_view path)
{
    // An embedded NUL would end the path for every operating system entry point
    // while the checks here still see the whole string, so the path that is
    // tested and the path that is opened would be different ones.
    if (path.find('\0') != std::string_view::npos)
    {
        return true;
    }

    // These two prefixes exist to switch off Win32 path normalization and to
    // address devices directly; nothing a document tool needs is named that way
    // on any platform.
    if (path.rfind(R"(\\?\)", 0) == 0 || path.rfind(R"(\\.\)", 0) == 0)
    {
        return true;
    }

    const std::filesystem::path candidate(path);

#if defined(_WIN32)
    const auto rootName = candidate.root_name().string();
    if (!rootName.empty())
    {
        // Only a drive letter may name a root. `\\server\share` would make the
        // process authenticate against a host the client picked, before
        // containment is ever consulted.
        const bool driveLetter = rootName.size() == 2 && rootName[1] == ':' &&
                                 ((rootName[0] >= 'A' && rootName[0] <= 'Z') ||
                                  (rootName[0] >= 'a' && rootName[0] <= 'z'));
        if (!driveLetter)
        {
            return true;
        }

        // `C:report.docx` resolves against the per-drive current directory,
        // process state this server never sets.
        if (!candidate.has_root_directory())
        {
            return true;
        }
    }
#endif

    for (const auto& element : candidate.relative_path())
    {
        if (WorkspacePathHelper::IsRefusedComponent(element.string()))
        {
            return true;
        }
    }

    return false;
}

bool Workspace::IsAcceptableName(std::string_view name)
{
    // Not IsRefusedForm: that one asks whether a client may name a path, which
    // follows the platform, while this asks whether a name may become a file —
    // the question Tools::ExportMedia answers about names it derives itself. The
    // same question deserves the same answer everywhere, or `prefix: "NUL"`
    // would be input_invalid on Windows and reach the library on Linux, only to
    // come back as operation_failed.
    return Tools::IsPlainOutputName(name);
}

std::optional<std::filesystem::path> Workspace::Resolve(std::string_view path) const
{
    if (path.empty() || m_roots.empty() || IsRefusedForm(path))
    {
        return std::nullopt;
    }

    std::filesystem::path candidate(path);
    if (candidate.is_relative())
    {
        candidate = m_roots.front() / candidate;
    }

    std::error_code error;
    auto canonical = std::filesystem::weakly_canonical(candidate, error);
    if (error || canonical.empty())
    {
        // A path that does not exist yet is not an error here — that is what
        // "weakly" means — so this is a file system that refused to answer:
        // an unreachable share, an inaccessible parent, a name too long for the
        // volume. Falling back to the uncanonicalized path would decide
        // containment on text alone, with symlinks left unresolved.
        return std::nullopt;
    }

    // Canonicalization is also what makes a short-name rule unnecessary: an
    // existing `PROGRA~1` comes back expanded, and a short name that resolves to
    // nothing cannot alias anything. Refusing `~1` textually would instead make
    // a legitimate `report~1.docx` unreachable. If the refusal above is ever
    // relaxed, that rule has to be added.
    canonical = canonical.lexically_normal();

    for (const auto& root : m_roots)
    {
        if (WorkspacePathHelper::IsInside(root, canonical))
        {
            return canonical;
        }
    }

    return std::nullopt;
}

std::string Workspace::Relativize(const std::filesystem::path& path) const
{
    for (const auto& root : m_roots)
    {
        if (!WorkspacePathHelper::IsInside(root, path))
        {
            continue;
        }

        auto relative = path.lexically_relative(root);
        if (relative.empty())
        {
            continue;
        }

        return relative.generic_string();
    }

    return path.generic_string();
}

WorkspaceListing Workspace::List(std::string_view glob, Size limit) const
{
    // A workspace this large is pathological, but the walk must not be able to
    // exhaust memory before the caller's limit ever applies.
    constexpr Size MaximumCollectedFiles = 100000;

    WorkspaceListing listing;
    std::vector<WorkspaceFile>& files = listing.Files;
    for (const auto& root : m_roots)
    {
        std::error_code error;
        std::filesystem::recursive_directory_iterator iterator(
            root, std::filesystem::directory_options::skip_permission_denied, error);
        if (error)
        {
            continue;
        }

        const std::filesystem::recursive_directory_iterator end;
        for (; iterator != end; iterator.increment(error))
        {
            if (error)
            {
                break;
            }

            if (!iterator->is_regular_file(error) || error)
            {
                continue;
            }

            auto relative = iterator->path().lexically_relative(root).generic_string();
            if (relative.empty() || !MatchesGlob(relative, glob))
            {
                continue;
            }

            WorkspaceFile file;
            file.Path = std::move(relative);
            file.Bytes = static_cast<UInt64>(iterator->file_size(error));
            if (error)
            {
                file.Bytes = 0;
                error.clear();
            }

            const auto written = iterator->last_write_time(error);
            file.Modified = error ? std::string() : FormatTimestamp(written);
            error.clear();

            files.push_back(std::move(file));
            if (files.size() >= MaximumCollectedFiles)
            {
                listing.Truncated = true;
                break;
            }
        }

        if (listing.Truncated)
        {
            break;
        }
    }

    std::sort(files.begin(), files.end(),
              [](const WorkspaceFile& left, const WorkspaceFile& right)
              { return left.Path < right.Path; });

    if (limit > 0 && files.size() > limit)
    {
        files.resize(limit);
        listing.Truncated = true;
    }

    return listing;
}

bool Workspace::MatchesGlob(std::string_view path, std::string_view glob)
{
    if (glob.empty())
    {
        return true;
    }

    if (WorkspacePathHelper::Match(path, 0, glob, 0))
    {
        return true;
    }

    // A bare "*.docx" is meant as "any .docx anywhere", which is what an agent
    // expects from a workspace listing; a pattern that already navigates
    // directories is matched literally.
    if (glob.find('/') == std::string_view::npos)
    {
        std::string prefixed = "**/";
        prefixed.append(glob);
        return WorkspacePathHelper::Match(path, 0, prefixed, 0);
    }

    return false;
}

std::string Workspace::FormatTimestamp(std::filesystem::file_time_type time)
{
    const auto systemTime = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        time - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
    const auto asTimeT = std::chrono::system_clock::to_time_t(systemTime);

    std::tm utc{};
#if defined(_WIN32)
    if (gmtime_s(&utc, &asTimeT) != 0)
    {
        return {};
    }
#else
    if (gmtime_r(&asTimeT, &utc) == nullptr)
    {
        return {};
    }
#endif

    return std::format("{:04}-{:02}-{:02}T{:02}:{:02}:{:02}Z", utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday,
                       utc.tm_hour, utc.tm_min, utc.tm_sec);
}

} // namespace ExyokiOffice::Mcp
