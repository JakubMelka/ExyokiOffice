// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "ExyokiOffice/Security/ResourceResolver.hpp"

#include <utility>

namespace ExyokiOffice::Security
{

/// Last segment of the relationship type URIs that name an external resource.
///
/// Only the segment is matched, not the whole URI, because the same relationship
/// exists under both the ECMA-376 namespace and the Microsoft extension
/// namespace ("...officeDocument/2006/relationships/video" and
/// "...office/2007/relationships/media" mean the same thing to this layer).
class ExternalRelationshipNames final
{
public:
    ExternalRelationshipNames() = delete;

    static constexpr std::string_view Image = "image";
    static constexpr std::string_view Audio = "audio";
    static constexpr std::string_view Video = "video";
    static constexpr std::string_view Media = "media";
    static constexpr std::string_view AttachedTemplate = "attachedTemplate";
    static constexpr std::string_view ExternalLink = "externalLink";
    static constexpr std::string_view ExternalLinkPath = "externalLinkPath";
    static constexpr std::string_view OleObject = "oleObject";
    static constexpr std::string_view Package = "package";
    static constexpr std::string_view Hyperlink = "hyperlink";

    /// Returns the part of the type URI after the last slash.
    static constexpr std::string_view LastSegment(std::string_view relationshipType) noexcept
    {
        const auto slash = relationshipType.rfind('/');
        if (slash == std::string_view::npos)
        {
            return relationshipType;
        }
        return relationshipType.substr(slash + 1);
    }
};

ExternalResourcePolicy ExternalResourcePolicy::Deny()
{
    return ExternalResourcePolicy{};
}

ExternalResourcePolicy ExternalResourcePolicy::HttpsOnly(std::vector<std::string> hosts,
                                                         std::vector<ExternalResourceKind> kinds)
{
    ExternalResourcePolicy policy;
    policy.AllowedSchemes.emplace_back("https");
    policy.AllowedHosts = std::move(hosts);
    policy.AllowedKinds = std::move(kinds);
    return policy;
}

std::string_view ToString(ExternalResourceKind kind) noexcept
{
    switch (kind)
    {
        case ExternalResourceKind::Unknown:
            return "Unknown";
        case ExternalResourceKind::LinkedImage:
            return "LinkedImage";
        case ExternalResourceKind::LinkedMedia:
            return "LinkedMedia";
        case ExternalResourceKind::AttachedTemplate:
            return "AttachedTemplate";
        case ExternalResourceKind::ExternalWorkbook:
            return "ExternalWorkbook";
        case ExternalResourceKind::LinkedOleObject:
            return "LinkedOleObject";
        case ExternalResourceKind::Hyperlink:
            return "Hyperlink";
    }
    return "Unknown";
}

std::string_view ToString(ExternalResourceStatus status) noexcept
{
    switch (status)
    {
        case ExternalResourceStatus::Ok:
            return "Ok";
        case ExternalResourceStatus::NoResolver:
            return "NoResolver";
        case ExternalResourceStatus::AccessDenied:
            return "AccessDenied";
        case ExternalResourceStatus::NotFound:
            return "NotFound";
        case ExternalResourceStatus::TimedOut:
            return "TimedOut";
        case ExternalResourceStatus::TooLarge:
            return "TooLarge";
        case ExternalResourceStatus::BudgetExceeded:
            return "BudgetExceeded";
        case ExternalResourceStatus::Unsupported:
            return "Unsupported";
        case ExternalResourceStatus::Cancelled:
            return "Cancelled";
        case ExternalResourceStatus::Failed:
            return "Failed";
    }
    return "Failed";
}

ExternalResourceKind ClassifyRelationshipType(std::string_view relationshipType) noexcept
{
    const auto segment = ExternalRelationshipNames::LastSegment(relationshipType);
    if (segment == ExternalRelationshipNames::Image)
    {
        return ExternalResourceKind::LinkedImage;
    }
    if (segment == ExternalRelationshipNames::Audio || segment == ExternalRelationshipNames::Video ||
        segment == ExternalRelationshipNames::Media)
    {
        return ExternalResourceKind::LinkedMedia;
    }
    if (segment == ExternalRelationshipNames::AttachedTemplate)
    {
        return ExternalResourceKind::AttachedTemplate;
    }
    if (segment == ExternalRelationshipNames::ExternalLink || segment == ExternalRelationshipNames::ExternalLinkPath)
    {
        return ExternalResourceKind::ExternalWorkbook;
    }
    if (segment == ExternalRelationshipNames::OleObject || segment == ExternalRelationshipNames::Package)
    {
        return ExternalResourceKind::LinkedOleObject;
    }
    if (segment == ExternalRelationshipNames::Hyperlink)
    {
        return ExternalResourceKind::Hyperlink;
    }
    return ExternalResourceKind::Unknown;
}

} // namespace ExyokiOffice::Security
