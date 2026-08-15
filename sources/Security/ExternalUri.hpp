// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

#include "ExyokiOffice/StandardTypes.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace ExyokiOffice::Security
{

/**
 * @brief An absolute URI split into the pieces an allowlist has to look at.
 *
 * This is deliberately not a general purpose URI class. It exists so the
 * external resource policy can compare a scheme, a host, a port, and a path
 * against a list, which means the parts have to be normalized the same way
 * every time: scheme and host lower case, the path percent-decoded and freed of
 * "." and ".." segments before it is compared with anything.
 */
struct ExternalUriParts
{
    /// Scheme in lower case, without the colon.
    std::string Scheme;
    /// Everything between "//" and "@", empty when the URI carries no credentials.
    std::string UserInfo;
    /// Host in lower case; for an IPv6 literal the brackets are stripped.
    std::string Host;
    /// Port when the URI states one explicitly.
    std::optional<UInt16> Port;
    /// True when the URI had an authority component ("scheme://...").
    bool HasAuthority = false;
    /// Percent-decoded and normalized path, starting with "/" whenever a host is present.
    ///
    /// This is the form an allowlist prefix is compared against, so a prefix is
    /// written the way a human writes a path ("/C:/Program Files/Templates"),
    /// not the way a URI escapes it.
    std::string Path;
    /// Query string including the leading "?", kept verbatim.
    std::string Query;
    /// Fragment including the leading "#", kept verbatim.
    std::string Fragment;
    /// True when the path as written navigated upwards, before normalization removed it.
    bool HadTraversal = false;
    /// Canonical form rebuilt from the parts; this is what a resolver is handed.
    std::string Normalized;
};

/**
 * @brief Parsing and normalization of the URIs stored in external relationships.
 *
 * The package layer has its own URI helpers (sources/OpenXmlPackageUri.hpp), but
 * those describe part paths inside a package and know nothing about schemes or
 * hosts. External targets are ordinary URIs, and on Windows they are just as
 * often a drive path or a UNC share, so both are folded into the file scheme
 * here rather than being special cased by every caller.
 */
class ExternalUri final
{
public:
    ExternalUri() = delete;

    /**
     * @brief Parses an absolute URI, a Windows drive path, or a UNC path.
     *
     * @param uri Target to parse.
     * @return The parts, or nullopt when the value is relative, malformed, or
     *         contains control characters.
     */
    [[nodiscard]] static std::optional<ExternalUriParts> Parse(std::string_view uri);

    /**
     * @brief Returns true when the value names a scheme and can be parsed on its own.
     */
    [[nodiscard]] static bool IsAbsolute(std::string_view uri) noexcept;

    /**
     * @brief Resolves a relative reference against an absolute base URI.
     *
     * @param base Absolute base; a Windows or UNC path is accepted as well.
     * @param relative Reference to resolve.
     * @return The combined absolute URI, or an empty string when @p base is not
     *         absolute.
     */
    [[nodiscard]] static std::string ResolveAgainstBase(std::string_view base, std::string_view relative);

    /**
     * @brief Returns the port a scheme uses when the URI does not state one.
     *
     * @return The default port, or 0 for schemes that have no port at all.
     */
    [[nodiscard]] static UInt16 DefaultPort(std::string_view scheme) noexcept;

    /**
     * @brief Returns true for the schemes that address a file system rather than a network service.
     */
    [[nodiscard]] static bool IsFileSystemScheme(std::string_view scheme) noexcept;

private:
    static bool ContainsForbiddenCharacter(std::string_view value) noexcept;
    static std::string NormalizeSeparators(std::string_view uri);
    static std::string ConvertWindowsPath(std::string_view uri);
    static std::string PercentDecode(std::string_view value);
    static std::string NormalizePath(std::string_view path, bool& hadTraversal);
    static std::string EncodePath(std::string_view path);
    static std::string BuildNormalized(const ExternalUriParts& parts);
};

} // namespace ExyokiOffice::Security
