// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "ExternalUri.hpp"

#include "OpenXmlPackageUri.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include "AsciiText.hpp"

#include <algorithm>
#include <vector>

namespace ExyokiOffice::Security
{

/// Scheme names and the small character helpers the parser needs.
class ExternalUriSchemes final
{
public:
    ExternalUriSchemes() = delete;

    static constexpr std::string_view File = "file";
    static constexpr std::string_view Http = "http";
    static constexpr std::string_view Https = "https";
    static constexpr std::string_view Ftp = "ftp";
    static constexpr std::string_view Ftps = "ftps";
    static constexpr std::string_view Smb = "smb";

    /// True for the characters a scheme may contain after its first letter.
    static bool IsSchemeCharacter(char ch) noexcept
    {
        return AsciiText::IsAlnum(ch) || ch == '+' || ch == '-' || ch == '.';
    }

    static bool IsDigits(std::string_view value) noexcept
    {
        return !value.empty() && std::all_of(value.begin(),
                                             value.end(),
                                             [](char ch)
                                             { return AsciiText::IsDigit(ch); });
    }

    /// Parses a port without throwing on an out of range value.
    static std::optional<UInt16> ParsePort(std::string_view value) noexcept
    {
        if (!IsDigits(value) || value.size() > 5)
        {
            return std::nullopt;
        }

        UInt32 port = 0;
        for (char ch : value)
        {
            port = port * 10 + static_cast<UInt32>(ch - '0');
        }
        if (port == 0 || port > 65535)
        {
            return std::nullopt;
        }
        return static_cast<UInt16>(port);
    }

    static int HexValue(char ch) noexcept
    {
        if (ch >= '0' && ch <= '9')
        {
            return ch - '0';
        }
        if (ch >= 'a' && ch <= 'f')
        {
            return ch - 'a' + 10;
        }
        if (ch >= 'A' && ch <= 'F')
        {
            return ch - 'A' + 10;
        }
        return -1;
    }
};

bool ExternalUri::ContainsForbiddenCharacter(std::string_view value) noexcept
{
    return std::any_of(value.begin(),
                       value.end(),
                       [](char ch)
                       {
                           const auto code = static_cast<unsigned char>(ch);
                           return code < 0x20 || code == 0x7F;
                       });
}

std::string ExternalUri::NormalizeSeparators(std::string_view uri)
{
    std::string normalized;
    normalized.reserve(uri.size());
    for (char ch : uri)
    {
        normalized.push_back(ch == '\\' ? '/' : ch);
    }
    return normalized;
}

std::string ExternalUri::ConvertWindowsPath(std::string_view uri)
{
    // A UNC path is written with two backslashes. Two forward slashes are a
    // protocol relative reference instead, and that is not an absolute URI.
    if (uri.size() > 2 && uri[0] == '\\' && uri[1] == '\\')
    {
        return "file://" + NormalizeSeparators(uri.substr(2));
    }

    // A drive path has to be recognized before the scheme is parsed, because
    // "C:" would otherwise look like a one letter scheme.
    if (uri.size() > 2 && AsciiText::IsAlpha(uri[0]) && uri[1] == ':' &&
        (uri[2] == '\\' || uri[2] == '/'))
    {
        return "file:///" + NormalizeSeparators(uri);
    }

    return std::string(uri);
}

std::string ExternalUri::PercentDecode(std::string_view value)
{
    std::string decoded;
    decoded.reserve(value.size());
    for (Size index = 0; index < value.size(); ++index)
    {
        const char ch = value[index];
        if (ch == '%' && index + 2 < value.size())
        {
            const auto high = ExternalUriSchemes::HexValue(value[index + 1]);
            const auto low = ExternalUriSchemes::HexValue(value[index + 2]);
            if (high >= 0 && low >= 0)
            {
                decoded.push_back(static_cast<char>((high << 4) | low));
                index += 2;
                continue;
            }
        }
        decoded.push_back(ch);
    }
    return decoded;
}

std::string ExternalUri::NormalizePath(std::string_view path, bool& hadTraversal)
{
    std::vector<std::string> segments;
    for (auto segment : Detail::SplitUriSegments(path))
    {
        if (segment.empty() || segment == ".")
        {
            continue;
        }
        if (segment == "..")
        {
            hadTraversal = true;
            if (!segments.empty())
            {
                segments.pop_back();
            }
            continue;
        }
        segments.emplace_back(segment);
    }

    auto joined = Detail::JoinUriSegments(segments);
    if (!path.empty() && path.back() == '/' && joined != "/")
    {
        joined.push_back('/');
    }
    return joined;
}

std::string ExternalUri::EncodePath(std::string_view path)
{
    if (path.empty())
    {
        return {};
    }

    const bool absolute = path.front() == '/';
    std::string encoded;
    encoded.reserve(path.size());
    for (auto segment : Detail::SplitUriSegments(path))
    {
        if (segment.empty())
        {
            continue;
        }
        if (absolute || !encoded.empty())
        {
            encoded.push_back('/');
        }
        encoded.append(Detail::EscapeUriSegment(segment));
    }

    if (encoded.empty())
    {
        return absolute ? std::string("/") : std::string();
    }
    if (path.back() == '/' && encoded.back() != '/')
    {
        encoded.push_back('/');
    }
    return encoded;
}

std::string ExternalUri::BuildNormalized(const ExternalUriParts& parts)
{
    std::string normalized = parts.Scheme;
    normalized.push_back(':');

    if (parts.HasAuthority)
    {
        normalized.append("//");
        if (!parts.UserInfo.empty())
        {
            normalized.append(parts.UserInfo);
            normalized.push_back('@');
        }
        if (parts.Host.find(':') != std::string::npos)
        {
            normalized.push_back('[');
            normalized.append(parts.Host);
            normalized.push_back(']');
        }
        else
        {
            normalized.append(parts.Host);
        }
        if (parts.Port.has_value())
        {
            normalized.push_back(':');
            normalized.append(std::to_string(*parts.Port));
        }
    }

    normalized.append(EncodePath(parts.Path));
    normalized.append(parts.Query);
    normalized.append(parts.Fragment);
    return normalized;
}

UInt16 ExternalUri::DefaultPort(std::string_view scheme) noexcept
{
    if (scheme == ExternalUriSchemes::Http)
    {
        return 80;
    }
    if (scheme == ExternalUriSchemes::Https)
    {
        return 443;
    }
    if (scheme == ExternalUriSchemes::Ftp)
    {
        return 21;
    }
    if (scheme == ExternalUriSchemes::Ftps)
    {
        return 990;
    }
    if (scheme == ExternalUriSchemes::Smb)
    {
        return 445;
    }
    return 0;
}

bool ExternalUri::IsFileSystemScheme(std::string_view scheme) noexcept
{
    return scheme == ExternalUriSchemes::File || scheme == ExternalUriSchemes::Smb;
}

bool ExternalUri::IsAbsolute(std::string_view uri) noexcept
{
    if (uri.size() > 2 && uri[0] == '\\' && uri[1] == '\\')
    {
        return true;
    }
    if (uri.size() > 2 && AsciiText::IsAlpha(uri[0]) && uri[1] == ':' &&
        (uri[2] == '\\' || uri[2] == '/'))
    {
        return true;
    }

    if (uri.empty() || !AsciiText::IsAlpha(uri.front()))
    {
        return false;
    }

    for (Size index = 1; index < uri.size(); ++index)
    {
        if (uri[index] == ':')
        {
            return true;
        }
        if (!ExternalUriSchemes::IsSchemeCharacter(uri[index]))
        {
            return false;
        }
    }
    return false;
}

std::optional<ExternalUriParts> ExternalUri::Parse(std::string_view uri)
{
    if (uri.empty() || ContainsForbiddenCharacter(uri))
    {
        return std::nullopt;
    }

    const std::string converted = ConvertWindowsPath(uri);
    std::string_view value(converted);

    if (!AsciiText::IsAlpha(value.front()))
    {
        return std::nullopt;
    }

    Size schemeEnd = 1;
    while (schemeEnd < value.size() && value[schemeEnd] != ':')
    {
        if (!ExternalUriSchemes::IsSchemeCharacter(value[schemeEnd]))
        {
            return std::nullopt;
        }
        ++schemeEnd;
    }
    if (schemeEnd >= value.size())
    {
        return std::nullopt;
    }

    ExternalUriParts parts;
    parts.Scheme = AsciiText::ToLower(value.substr(0, schemeEnd));
    value.remove_prefix(schemeEnd + 1);

    std::string authority;
    std::string_view remainder = value;
    if (value.size() >= 2 && value[0] == '/' && value[1] == '/')
    {
        parts.HasAuthority = true;
        value.remove_prefix(2);
        const auto end = value.find_first_of("/?#");
        authority = std::string(end == std::string_view::npos ? value : value.substr(0, end));
        remainder = end == std::string_view::npos ? std::string_view{} : value.substr(end);
    }

    if (!authority.empty())
    {
        const auto at = authority.rfind('@');
        if (at != std::string::npos)
        {
            parts.UserInfo = authority.substr(0, at);
            authority = authority.substr(at + 1);
        }

        if (!authority.empty() && authority.front() == '[')
        {
            const auto close = authority.find(']');
            if (close == std::string::npos)
            {
                return std::nullopt;
            }
            parts.Host = authority.substr(1, close - 1);
            const auto after = authority.substr(close + 1);
            if (!after.empty())
            {
                if (after.front() != ':')
                {
                    return std::nullopt;
                }
                parts.Port = ExternalUriSchemes::ParsePort(after.substr(1));
                if (!parts.Port.has_value())
                {
                    return std::nullopt;
                }
            }
        }
        else
        {
            const auto colon = authority.rfind(':');
            if (colon != std::string::npos)
            {
                const auto portText = authority.substr(colon + 1);
                if (!portText.empty())
                {
                    parts.Port = ExternalUriSchemes::ParsePort(portText);
                    if (!parts.Port.has_value())
                    {
                        return std::nullopt;
                    }
                }
                parts.Host = authority.substr(0, colon);
            }
            else
            {
                parts.Host = authority;
            }
        }
        parts.Host = AsciiText::ToLower(parts.Host);
    }

    std::string rest(remainder);
    const auto hash = rest.find('#');
    if (hash != std::string::npos)
    {
        parts.Fragment = rest.substr(hash);
        rest.erase(hash);
    }
    const auto question = rest.find('?');
    if (question != std::string::npos)
    {
        parts.Query = rest.substr(question);
        rest.erase(question);
    }

    const auto decodedPath = PercentDecode(rest);
    if (ContainsForbiddenCharacter(decodedPath))
    {
        return std::nullopt;
    }

    if (parts.HasAuthority || decodedPath.starts_with("/"))
    {
        parts.Path = NormalizePath(decodedPath, parts.HadTraversal);
    }
    else
    {
        // An opaque target such as "mailto:someone@example.com" has no
        // hierarchical path, so there is nothing to normalize.
        parts.Path = decodedPath;
    }

    parts.Normalized = BuildNormalized(parts);
    return parts;
}

std::string ExternalUri::ResolveAgainstBase(std::string_view base, std::string_view relative)
{
    if (relative.empty())
    {
        return std::string(base);
    }
    if (IsAbsolute(relative))
    {
        return std::string(relative);
    }

    const auto baseParts = Parse(base);
    if (!baseParts.has_value())
    {
        return {};
    }

    std::string authority;
    if (!baseParts->UserInfo.empty())
    {
        authority.append(baseParts->UserInfo);
        authority.push_back('@');
    }
    authority.append(baseParts->Host);
    if (baseParts->Port.has_value())
    {
        authority.push_back(':');
        authority.append(std::to_string(*baseParts->Port));
    }

    std::string combined = baseParts->Scheme;
    combined.append("://");
    combined.append(authority);

    const std::string normalizedRelative = NormalizeSeparators(relative);
    if (normalizedRelative.starts_with("/"))
    {
        combined.append(normalizedRelative);
        return combined;
    }

    auto directory = baseParts->Path;
    const auto slash = directory.rfind('/');
    directory = slash == std::string::npos ? std::string("/") : directory.substr(0, slash + 1);
    combined.append(directory);
    combined.append(normalizedRelative);
    return combined;
}

} // namespace ExyokiOffice::Security
