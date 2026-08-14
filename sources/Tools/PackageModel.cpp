// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "ExyokiOffice/Tools/PackageModel.hpp"

#include <algorithm>
#include <cctype>

namespace ExyokiOffice::Tools
{

std::string_view ToString(DocumentFamily family) noexcept
{
    switch (family)
    {
        case DocumentFamily::Word:
            return "word";
        case DocumentFamily::Excel:
            return "excel";
        case DocumentFamily::PowerPoint:
            return "powerpoint";
        case DocumentFamily::Unknown:
            break;
    }
    return "unknown";
}

std::string_view ToString(ToolSeverity severity) noexcept
{
    switch (severity)
    {
        case ToolSeverity::Info:
            return "info";
        case ToolSeverity::Warning:
            return "warning";
        case ToolSeverity::Error:
            break;
    }
    return "error";
}

std::string_view ToString(ValidationSeverity severity) noexcept
{
    switch (severity)
    {
        case ValidationSeverity::Warning:
            return "warning";
        case ValidationSeverity::Error:
            break;
    }
    return "error";
}

std::string_view ToString(ValidationDomain domain) noexcept
{
    switch (domain)
    {
        case ValidationDomain::General:
            return "general";
        case ValidationDomain::Opc:
            return "opc";
        case ValidationDomain::Xml:
            return "xml";
        case ValidationDomain::Dom:
            return "dom";
        case ValidationDomain::Schema:
            return "schema";
        case ValidationDomain::Packaging:
            return "packaging";
        case ValidationDomain::Security:
            return "security";
        case ValidationDomain::Io:
            return "io";
    }
    return "general";
}

std::string_view ToString(ValidationErrorId id) noexcept
{
    switch (id)
    {
        case ValidationErrorId::Unknown:
            return "Unknown";
        case ValidationErrorId::MissingAttribute:
            return "MissingAttribute";
        case ValidationErrorId::EmptyAttribute:
            return "EmptyAttribute";
        case ValidationErrorId::AttributeExactLengthMismatch:
            return "AttributeExactLengthMismatch";
        case ValidationErrorId::AttributeMinLengthMismatch:
            return "AttributeMinLengthMismatch";
        case ValidationErrorId::AttributeMaxLengthMismatch:
            return "AttributeMaxLengthMismatch";
        case ValidationErrorId::AttributePatternMismatch:
            return "AttributePatternMismatch";
        case ValidationErrorId::AttributeTokenMismatch:
            return "AttributeTokenMismatch";
        case ValidationErrorId::AttributeNcNameViolation:
            return "AttributeNcNameViolation";
        case ValidationErrorId::AttributeQNameViolation:
            return "AttributeQNameViolation";
        case ValidationErrorId::AttributeIdViolation:
            return "AttributeIdViolation";
        case ValidationErrorId::AttributeUriViolation:
            return "AttributeUriViolation";
        case ValidationErrorId::AttributeNumberParsingFailed:
            return "AttributeNumberParsingFailed";
        case ValidationErrorId::AttributeMinInclusiveViolation:
            return "AttributeMinInclusiveViolation";
        case ValidationErrorId::AttributeMaxInclusiveViolation:
            return "AttributeMaxInclusiveViolation";
        case ValidationErrorId::AttributeMinExclusiveViolation:
            return "AttributeMinExclusiveViolation";
        case ValidationErrorId::AttributeMaxExclusiveViolation:
            return "AttributeMaxExclusiveViolation";
        case ValidationErrorId::AttributePositiveViolation:
            return "AttributePositiveViolation";
        case ValidationErrorId::AttributeNonNegativeViolation:
            return "AttributeNonNegativeViolation";
        case ValidationErrorId::AttributeEnumViolation:
            return "AttributeEnumViolation";
        case ValidationErrorId::AttributeVersionViolation:
            return "AttributeVersionViolation";
        case ValidationErrorId::TextExactLengthMismatch:
            return "TextExactLengthMismatch";
        case ValidationErrorId::TextMinLengthMismatch:
            return "TextMinLengthMismatch";
        case ValidationErrorId::TextMaxLengthMismatch:
            return "TextMaxLengthMismatch";
        case ValidationErrorId::TextPatternMismatch:
            return "TextPatternMismatch";
        case ValidationErrorId::TextTokenMismatch:
            return "TextTokenMismatch";
        case ValidationErrorId::TextNcNameViolation:
            return "TextNcNameViolation";
        case ValidationErrorId::TextQNameViolation:
            return "TextQNameViolation";
        case ValidationErrorId::TextIdViolation:
            return "TextIdViolation";
        case ValidationErrorId::TextUriViolation:
            return "TextUriViolation";
        case ValidationErrorId::TextNumberParsingFailed:
            return "TextNumberParsingFailed";
        case ValidationErrorId::TextMinInclusiveViolation:
            return "TextMinInclusiveViolation";
        case ValidationErrorId::TextMaxInclusiveViolation:
            return "TextMaxInclusiveViolation";
        case ValidationErrorId::TextMinExclusiveViolation:
            return "TextMinExclusiveViolation";
        case ValidationErrorId::TextMaxExclusiveViolation:
            return "TextMaxExclusiveViolation";
        case ValidationErrorId::TextPositiveViolation:
            return "TextPositiveViolation";
        case ValidationErrorId::TextNonNegativeViolation:
            return "TextNonNegativeViolation";
        case ValidationErrorId::TextEnumViolation:
            return "TextEnumViolation";
        case ValidationErrorId::ParticleConstraintViolation:
            return "ParticleConstraintViolation";
        case ValidationErrorId::ElementVersionViolation:
            return "ElementVersionViolation";
        case ValidationErrorId::PartVersionViolation:
            return "PartVersionViolation";
        case ValidationErrorId::PackageMissingMainPart:
            return "PackageMissingMainPart";
        case ValidationErrorId::PackageMultipleMainParts:
            return "PackageMultipleMainParts";
        case ValidationErrorId::PackageRelationshipTypeMismatch:
            return "PackageRelationshipTypeMismatch";
        case ValidationErrorId::PackageContentTypeMismatch:
            return "PackageContentTypeMismatch";
        case ValidationErrorId::OpcDuplicateRelationshipId:
            return "OpcDuplicateRelationshipId";
        case ValidationErrorId::OpcEmptyRelationshipId:
            return "OpcEmptyRelationshipId";
        case ValidationErrorId::OpcEmptyRelationshipType:
            return "OpcEmptyRelationshipType";
        case ValidationErrorId::OpcEmptyRelationshipTarget:
            return "OpcEmptyRelationshipTarget";
        case ValidationErrorId::OpcInvalidRelationshipTargetMode:
            return "OpcInvalidRelationshipTargetMode";
        case ValidationErrorId::OpcDanglingRelationshipTarget:
            return "OpcDanglingRelationshipTarget";
        case ValidationErrorId::OpcDuplicatePartUri:
            return "OpcDuplicatePartUri";
        case ValidationErrorId::OpcMalformedPartXml:
            return "OpcMalformedPartXml";
        case ValidationErrorId::PackageStrictConformanceUnsupported:
            return "PackageStrictConformanceUnsupported";
        case ValidationErrorId::OpcLimitExceeded:
            return "OpcLimitExceeded";
        case ValidationErrorId::XmlLimitExceeded:
            return "XmlLimitExceeded";
        case ValidationErrorId::MarkupCompatibilityMustUnderstandUnsupported:
            return "MarkupCompatibilityMustUnderstandUnsupported";
        case ValidationErrorId::MarkupCompatibilityMalformedAlternateContent:
            return "MarkupCompatibilityMalformedAlternateContent";
        case ValidationErrorId::MarkupCompatibilityUnresolvablePrefix:
            return "MarkupCompatibilityUnresolvablePrefix";
        case ValidationErrorId::SchematronConstraintViolation:
            return "SchematronConstraintViolation";
        case ValidationErrorId::SchematronRuleNotEvaluable:
            return "SchematronRuleNotEvaluable";
        case ValidationErrorId::SignatureMalformed:
            return "SignatureMalformed";
        case ValidationErrorId::SignatureDigestMismatch:
            return "SignatureDigestMismatch";
        case ValidationErrorId::SignaturePartMissing:
            return "SignaturePartMissing";
        case ValidationErrorId::SignatureUnsupportedAlgorithm:
            return "SignatureUnsupportedAlgorithm";
        case ValidationErrorId::SignatureValueInvalid:
            return "SignatureValueInvalid";
        case ValidationErrorId::SignatureNotVerified:
            return "SignatureNotVerified";
        case ValidationErrorId::SignatureInvalidatedBySave:
            return "SignatureInvalidatedBySave";
        case ValidationErrorId::ExternalResourceNoResolver:
            return "ExternalResourceNoResolver";
        case ValidationErrorId::ExternalResourceDenied:
            return "ExternalResourceDenied";
        case ValidationErrorId::ExternalResourceTooLarge:
            return "ExternalResourceTooLarge";
        case ValidationErrorId::ExternalResourceBudgetExceeded:
            return "ExternalResourceBudgetExceeded";
        case ValidationErrorId::ExternalResourceUnavailable:
            return "ExternalResourceUnavailable";
        case ValidationErrorId::ContentModelCrossCheckMismatch:
            return "ContentModelCrossCheckMismatch";
    }
    return "Unknown";
}

std::optional<ExyokiOffice::OpenXml::FileFormatVersions> ParseFileFormatVersion(std::string_view text) noexcept
{
    using ExyokiOffice::OpenXml::FileFormatVersions;

    std::string lowered;
    lowered.reserve(text.size());
    for (char ch : text)
    {
        lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }

    if (lowered == "2007")
    {
        return FileFormatVersions::Office2007;
    }
    if (lowered == "2010")
    {
        return FileFormatVersions::Office2010;
    }
    if (lowered == "2013")
    {
        return FileFormatVersions::Office2013;
    }
    if (lowered == "2016")
    {
        return FileFormatVersions::Office2016;
    }
    if (lowered == "2019")
    {
        return FileFormatVersions::Office2019;
    }
    if (lowered == "2021")
    {
        return FileFormatVersions::Office2021;
    }
    if (lowered == "365" || lowered == "microsoft365")
    {
        return FileFormatVersions::Microsoft365;
    }
    return std::nullopt;
}

/// File-local helpers behind the package model projection.
class PackageModelHelper
{
public:
    /// Case-insensitive filename wildcard match supporting '*' and '?'.
    static bool WildcardMatches(std::string_view pattern, std::string_view name)
    {
        const auto lower = [](char c)
        { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); };

        Size p = 0;
        Size n = 0;
        Size starPattern = std::string_view::npos;
        Size starName = 0;
        while (n < name.size())
        {
            if (p < pattern.size() && (pattern[p] == '?' || lower(pattern[p]) == lower(name[n])))
            {
                ++p;
                ++n;
            }
            else if (p < pattern.size() && pattern[p] == '*')
            {
                starPattern = p++;
                starName = n;
            }
            else if (starPattern != std::string_view::npos)
            {
                p = starPattern + 1;
                n = ++starName;
            }
            else
            {
                return false;
            }
        }
        while (p < pattern.size() && pattern[p] == '*')
        {
            ++p;
        }
        return p == pattern.size();
    }
};

std::vector<std::filesystem::path> ExpandInputPaths(const std::vector<std::string>& patterns,
                                                    std::vector<ToolDiagnostic>& diagnostics)
{
    std::vector<std::filesystem::path> paths;
    for (const auto& pattern : patterns)
    {
        const std::filesystem::path patternPath(pattern);
        const auto filename = patternPath.filename().string();
        if (filename.find('*') == std::string::npos && filename.find('?') == std::string::npos)
        {
            paths.push_back(patternPath);
            continue;
        }

        auto directory = patternPath.parent_path();
        if (directory.empty())
        {
            directory = ".";
        }

        std::vector<std::filesystem::path> matches;
        std::error_code errorCode;
        for (const auto& entry : std::filesystem::directory_iterator(directory, errorCode))
        {
            if (entry.is_regular_file(errorCode) &&
                PackageModelHelper::WildcardMatches(filename, entry.path().filename().string()))
            {
                matches.push_back(entry.path());
            }
        }
        if (errorCode)
        {
            diagnostics.push_back(
                ToolDiagnostic{ToolSeverity::Error, "Cannot enumerate directory", directory.string()});
            continue;
        }
        if (matches.empty())
        {
            diagnostics.push_back(
                ToolDiagnostic{ToolSeverity::Warning, "Pattern matched no files", pattern});
            continue;
        }
        std::sort(matches.begin(), matches.end());
        paths.insert(paths.end(), matches.begin(), matches.end());
    }
    return paths;
}

} // namespace ExyokiOffice::Tools
