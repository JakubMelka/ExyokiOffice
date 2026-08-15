// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

#include "ExyokiOffice/StandardTypes.hpp"
#include "AsciiText.hpp"

#include <algorithm>
#include <string>
#include <array>
#include <string_view>
#include <vector>

namespace ExyokiOffice::Detail
{

inline bool IsHexDigit(char ch) noexcept
{
    return AsciiText::IsHexDigit(ch);
}

inline char UpperHexDigit(char ch) noexcept
{
    return AsciiText::ToUpper(ch);
}

inline bool IsPackagePathChar(char ch) noexcept
{
    if (AsciiText::IsAlnum(ch))
    {
        return true;
    }

    switch (ch)
    {
        case '-':
        case '.':
        case '_':
        case '~':
        case '!':
        case '$':
        case '&':
        case '\'':
        case '(':
        case ')':
        case '*':
        case '+':
        case ',':
        case ';':
        case '=':
        case ':':
        case '@':
            return true;
        default:
            return false;
    }
}

inline std::vector<std::string_view> SplitUriSegments(std::string_view uri)
{
    std::vector<std::string_view> segments;
    Size start = 0;
    while (start <= uri.size())
    {
        const auto slash = uri.find('/', start);
        const auto end = slash == std::string_view::npos ? uri.size() : slash;
        segments.push_back(uri.substr(start, end - start));
        if (slash == std::string_view::npos)
        {
            break;
        }
        start = slash + 1;
    }
    return segments;
}

inline std::string JoinUriSegments(const std::vector<std::string>& segments)
{
    if (segments.empty())
    {
        return "/";
    }

    std::string result;
    for (const auto& segment : segments)
    {
        result.push_back('/');
        result.append(segment);
    }
    return result;
}

inline void AppendPercentEncoded(std::string& output, unsigned char value)
{
    constexpr char hex[] = "0123456789ABCDEF";
    output.push_back('%');
    output.push_back(hex[value >> 4]);
    output.push_back(hex[value & 0x0F]);
}

inline std::string EscapeUriSegment(std::string_view segment)
{
    std::string escaped;
    escaped.reserve(segment.size());
    for (Size index = 0; index < segment.size(); ++index)
    {
        const char ch = segment[index];
        if (ch == '%' && index + 2 < segment.size() && IsHexDigit(segment[index + 1]) && IsHexDigit(segment[index + 2]))
        {
            escaped.push_back('%');
            escaped.push_back(UpperHexDigit(segment[index + 1]));
            escaped.push_back(UpperHexDigit(segment[index + 2]));
            index += 2;
            continue;
        }

        if (IsPackagePathChar(ch))
        {
            escaped.push_back(ch);
            continue;
        }

        AppendPercentEncoded(escaped, static_cast<unsigned char>(ch));
    }
    return escaped;
}

inline std::string NormalizePartUri(std::string_view uri)
{
    std::string normalizedInput;
    normalizedInput.reserve(uri.size() + 1);
    if (uri.empty() || uri.front() != '/')
    {
        normalizedInput.push_back('/');
    }
    for (char ch : uri)
    {
        normalizedInput.push_back(ch == '\\' ? '/' : ch);
    }

    std::vector<std::string> segments;
    for (auto segment : SplitUriSegments(normalizedInput))
    {
        if (segment.empty() || segment == ".")
        {
            continue;
        }
        if (segment == "..")
        {
            if (!segments.empty())
            {
                segments.pop_back();
            }
            continue;
        }
        segments.emplace_back(EscapeUriSegment(segment));
    }

    return JoinUriSegments(segments);
}

inline std::string TrimLeadingPackageSlash(std::string_view uri)
{
    if (!uri.empty() && uri.front() == '/')
    {
        uri.remove_prefix(1);
    }
    return std::string(uri);
}

inline std::string PartUriDirectory(std::string_view uri)
{
    const auto normalized = NormalizePartUri(uri);
    const auto slash = normalized.rfind('/');
    if (slash == std::string::npos || slash == 0)
    {
        return "/";
    }
    return normalized.substr(0, slash);
}

inline std::string PartUriFileName(std::string_view uri)
{
    const auto normalized = NormalizePartUri(uri);
    const auto slash = normalized.rfind('/');
    if (slash == std::string::npos)
    {
        return normalized;
    }
    return normalized.substr(slash + 1);
}

inline std::string PartUriExtension(std::string_view uri)
{
    const auto fileName = PartUriFileName(uri);
    const auto dot = fileName.rfind('.');
    if (dot == std::string::npos || dot + 1 >= fileName.size())
    {
        return {};
    }
    return fileName.substr(dot + 1);
}

inline std::string CombinePartUri(std::string_view baseUri, std::string_view relativeUri)
{
    if (relativeUri.empty() || relativeUri == ".")
    {
        return NormalizePartUri(baseUri);
    }
    if (relativeUri.front() == '/')
    {
        return NormalizePartUri(relativeUri);
    }

    auto base = NormalizePartUri(baseUri);
    if (base.empty() || base.back() != '/')
    {
        base.push_back('/');
    }
    base.append(relativeUri);
    return NormalizePartUri(base);
}

/**
 * Resolves the folder a new child part is created in.
 *
 * A part descriptor carries one default path, but the same part type can be
 * attached to many different parents (an image part has more than thirty), so
 * the path cannot name an absolute location and is resolved against the parent.
 * A handful of descriptors nevertheless repeat the folder their only sensible
 * parent already sits in — a signature part under the signature origin part, a
 * slide under another slide — and combining those blindly would nest the folder
 * twice (`/_xmlsignatures/_xmlsignatures/sig1.xml`), which no consumer expects.
 *
 * When the default path is a plain folder name that the container is already
 * in, the child therefore stays next to its parent. Paths that navigate
 * (`../media`) or descend further (`xl/worksheets`) are always combined.
 *
 * A few parts live in one fixed place per document family whatever they hang
 * off — images in `/word/media`, a theme in `/ppt/theme`. Their descriptors
 * carry an absolute family path (see `data/exyokioffice_part_paths.json`),
 * which `CombinePartUri` returns unchanged.
 */
inline std::string CombineChildPartFolder(std::string_view containerUri, std::string_view defaultPath)
{
    const bool isPlainName =
        !defaultPath.empty() && defaultPath != "." && defaultPath != ".." && defaultPath.find('/') == std::string_view::npos;
    if (isPlainName)
    {
        const auto container = NormalizePartUri(containerUri);
        const auto slash = container.rfind('/');
        if (slash != std::string::npos && std::string_view(container).substr(slash + 1) == defaultPath)
        {
            return container;
        }
    }

    return CombinePartUri(containerUri, defaultPath);
}

/**
 * @brief File extension a content type is normally stored under.
 *
 * A part descriptor cannot name the extension of a payload whose type is only
 * known at runtime - an image part holds a PNG, a JPEG or a BMP - and falls back
 * to the `.bin` placeholder. Office reads the content type from
 * `[Content_Types].xml` rather than the name, but its own packages use the
 * conventional extension, and so do the tools around them.
 *
 * @return The extension including the dot, or an empty string for a content
 * type without a conventional one.
 */
inline std::string PartExtensionForContentType(std::string_view contentType)
{
    struct Mapping
    {
        std::string_view ContentType;
        std::string_view Extension;
    };
    static constexpr std::array<Mapping, 22> kMappings = {
        Mapping{"image/png", ".png"},
        Mapping{"image/jpeg", ".jpeg"},
        Mapping{"image/jpg", ".jpg"},
        Mapping{"image/gif", ".gif"},
        Mapping{"image/bmp", ".bmp"},
        Mapping{"image/tiff", ".tiff"},
        Mapping{"image/svg+xml", ".svg"},
        Mapping{"image/x-emf", ".emf"},
        Mapping{"image/x-wmf", ".wmf"},
        Mapping{"audio/mpeg", ".mp3"},
        Mapping{"audio/mp4", ".m4a"},
        Mapping{"audio/wav", ".wav"},
        Mapping{"audio/x-wav", ".wav"},
        Mapping{"video/mp4", ".mp4"},
        Mapping{"video/mpeg", ".mpg"},
        Mapping{"video/quicktime", ".mov"},
        Mapping{"video/x-ms-wmv", ".wmv"},
        Mapping{"video/unknown", ".avi"},
        Mapping{"model/gltf-binary", ".glb"},
        Mapping{"application/xml", ".xml"},
        Mapping{"text/xml", ".xml"},
        Mapping{"application/pdf", ".pdf"},
    };

    // Content types are compared without their parameters ("; charset=...") and
    // case-insensitively, as RFC 2045 requires.
    const auto parameter = contentType.find(';');
    if (parameter != std::string_view::npos)
    {
        contentType = contentType.substr(0, parameter);
    }
    while (!contentType.empty() && contentType.back() == ' ')
    {
        contentType.remove_suffix(1);
    }

    const std::string normalized = AsciiText::ToLower(contentType);
    for (const auto& mapping : kMappings)
    {
        if (normalized == mapping.ContentType)
        {
            return std::string(mapping.Extension);
        }
    }
    return {};
}

inline std::string ResolveRelationshipTarget(std::string_view sourceUri, std::string_view targetUri)
{
    if (targetUri.empty())
    {
        return {};
    }
    if (targetUri.front() == '/')
    {
        return NormalizePartUri(targetUri);
    }
    return CombinePartUri(PartUriDirectory(sourceUri), targetUri);
}

inline std::string BuildRelationshipTarget(std::string_view sourceUri, std::string_view targetUri)
{
    const auto normalizedSource = NormalizePartUri(sourceUri);
    const auto normalizedTarget = NormalizePartUri(targetUri);

    if (normalizedSource == "/")
    {
        return TrimLeadingPackageSlash(normalizedTarget);
    }

    const auto sourcePath = TrimLeadingPackageSlash(normalizedSource);
    const auto targetPath = TrimLeadingPackageSlash(normalizedTarget);
    auto sourceSegments = SplitUriSegments(sourcePath);
    auto targetSegments = SplitUriSegments(targetPath);
    sourceSegments.erase(std::remove(sourceSegments.begin(), sourceSegments.end(), std::string_view{}),
                         sourceSegments.end());
    targetSegments.erase(std::remove(targetSegments.begin(), targetSegments.end(), std::string_view{}),
                         targetSegments.end());

    Size common = 0;
    while (common < sourceSegments.size() && common < targetSegments.size() && sourceSegments[common] == targetSegments[common])
    {
        ++common;
    }

    std::string relative;
    for (Size index = common; index < sourceSegments.size(); ++index)
    {
        if (!relative.empty())
        {
            relative.push_back('/');
        }
        relative.append("..");
    }
    for (Size index = common; index < targetSegments.size(); ++index)
    {
        if (!relative.empty())
        {
            relative.push_back('/');
        }
        relative.append(targetSegments[index]);
    }

    return relative.empty() ? TrimLeadingPackageSlash(normalizedTarget) : relative;
}

inline std::string RelationshipPartEntryName(std::string_view partUri)
{
    const auto directory = PartUriDirectory(partUri);
    const auto fileName = PartUriFileName(partUri);
    std::string relationshipUri = directory;
    if (relationshipUri.empty() || relationshipUri.back() != '/')
    {
        relationshipUri.push_back('/');
    }
    relationshipUri.append("_rels/");
    relationshipUri.append(fileName);
    relationshipUri.append(".rels");
    return TrimLeadingPackageSlash(NormalizePartUri(relationshipUri));
}

} // namespace ExyokiOffice::Detail
