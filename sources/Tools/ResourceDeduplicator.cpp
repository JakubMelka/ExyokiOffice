// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "ExyokiOffice/Tools/ResourceDeduplicator.hpp"
#include "ExyokiOffice/Tools/PackageLimits.hpp"

#include "ExyokiOffice/OpenXmlPackagePart.hpp"
#include "ExyokiOffice/Tools/PackageInspector.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <algorithm>
#include <map>
#include <optional>
#include <string_view>
#include <utility>

namespace ExyokiOffice::Tools
{

/// Internal selection and relationship-rewriting helpers for the deduplicator.
class ResourceDeduplicationHelpers
{
public:
    /// One deduplication candidate together with its already-read payload.
    struct CandidateResource
    {
        std::shared_ptr<OpenXmlPackagePart> Part;
        std::vector<Byte> Payload;
    };

    static void AddDiagnostic(std::vector<ToolDiagnostic>& diagnostics,
                              ToolSeverity severity,
                              std::string message,
                              std::string context)
    {
        diagnostics.push_back(ToolDiagnostic{severity, std::move(message), std::move(context)});
    }

    static bool StartsWith(std::string_view text, std::string_view prefix)
    {
        return text.size() >= prefix.size() && text.substr(0, prefix.size()) == prefix;
    }

    static bool IsFontContentType(std::string_view contentType)
    {
        return contentType == "application/x-fontdata" || contentType == "application/x-font-ttf" ||
               contentType == "application/vnd.openxmlformats-officedocument.obfuscatedFont" ||
               StartsWith(contentType, "font/");
    }

    static bool IsCandidateContentType(std::string_view contentType, const ResourceDeduplicationOptions& options)
    {
        if (options.IncludeAllBinaryParts)
        {
            return true;
        }
        if (options.IncludeImages && StartsWith(contentType, "image/"))
        {
            return true;
        }
        if (options.IncludeAudio && StartsWith(contentType, "audio/"))
        {
            return true;
        }
        if (options.IncludeVideo && StartsWith(contentType, "video/"))
        {
            return true;
        }
        return options.IncludeFonts && IsFontContentType(contentType);
    }

    static bool IsCandidatePart(const OpenXmlPackagePart& part, const ResourceDeduplicationOptions& options)
    {
        // Only leaf binary payloads are safe to merge: parts with children or
        // outgoing relationships would need their whole subgraph compared.
        return part.IsBinaryPart() && part.Parts().empty() && part.Relationships().empty() &&
               IsCandidateContentType(part.ContentType(), options);
    }

    /// Retargets every relationship pointing at `duplicate` to `canonical`.
    /// Returns the number of rewritten relationships, or std::nullopt on failure.
    static std::optional<Size> RedirectIncomingEdges(
        OpenXmlPackage& package,
        const std::shared_ptr<OpenXmlPackagePart>& duplicate,
        const std::shared_ptr<OpenXmlPackagePart>& canonical,
        std::vector<ToolDiagnostic>& diagnostics)
    {
        // Copy the edges: retargeting mutates the incoming-relationship view.
        const auto edges = duplicate->IncomingRelationships();
        Size rewritten = 0;
        for (const auto& edge : edges)
        {
            OpenXmlPartContainer* source = nullptr;
            if (edge.SourceUri == "/" || edge.SourceUri.empty())
            {
                source = &package;
            }
            else
            {
                source = package.GetPartByUri(edge.SourceUri).get();
            }
            if (!source)
            {
                AddDiagnostic(diagnostics, ToolSeverity::Error,
                              "Relationship source part not found; duplicate kept", edge.SourceUri);
                return std::nullopt;
            }
            if (!source->RetargetRelationship(edge.Id, canonical))
            {
                AddDiagnostic(diagnostics, ToolSeverity::Error,
                              "Failed to retarget relationship " + edge.Id + "; duplicate kept",
                              duplicate->Uri());
                return std::nullopt;
            }
            ++rewritten;
        }
        return rewritten;
    }
};

using CandidateResource = ResourceDeduplicationHelpers::CandidateResource;

ResourceDeduplicationResult DeduplicateSharedResources(OpenXmlPackage& package,
                                                       const ResourceDeduplicationOptions& options)
{
    ResourceDeduplicationResult result;

    // Collect candidates in deterministic URI order.
    std::vector<CandidateResource> candidates;
    for (const auto& part : CollectAllParts(package))
    {
        if (part && ResourceDeduplicationHelpers::IsCandidatePart(*part, options))
        {
            candidates.push_back(CandidateResource{part, part->GetBinaryData()});
        }
    }
    std::sort(candidates.begin(), candidates.end(),
              [](const CandidateResource& left, const CandidateResource& right)
              { return left.Part->Uri() < right.Part->Uri(); });

    // Group by content type and payload size first, then confirm byte equality.
    std::map<std::pair<std::string, Size>, std::vector<const CandidateResource*>> buckets;
    for (const auto& candidate : candidates)
    {
        buckets[{std::string(candidate.Part->ContentType()), candidate.Payload.size()}].push_back(&candidate);
    }

    for (const auto& [key, bucket] : buckets)
    {
        if (bucket.size() < 2)
        {
            continue;
        }

        std::vector<bool> consumed(bucket.size(), false);
        for (Size i = 0; i < bucket.size(); ++i)
        {
            if (consumed[i])
            {
                continue;
            }

            ResourceDuplicateGroup group;
            group.ContentType = key.first;
            group.KeptPartUri = bucket[i]->Part->Uri();
            group.PayloadBytes = bucket[i]->Payload.size();

            for (Size j = i + 1; j < bucket.size(); ++j)
            {
                if (consumed[j] || bucket[i]->Payload != bucket[j]->Payload)
                {
                    continue;
                }
                consumed[j] = true;

                const auto& duplicate = bucket[j]->Part;
                if (options.DryRun)
                {
                    group.DuplicatePartUris.push_back(duplicate->Uri());
                    result.BytesSaved += group.PayloadBytes;
                    continue;
                }

                const auto duplicateUri = duplicate->Uri();
                const auto rewritten = ResourceDeduplicationHelpers::RedirectIncomingEdges(
                    package, duplicate, bucket[i]->Part, result.Diagnostics);
                if (!rewritten)
                {
                    continue;
                }
                result.RewrittenRelationships += *rewritten;

                if (package.GetPartByUri(duplicateUri))
                {
                    ResourceDeduplicationHelpers::AddDiagnostic(
                        result.Diagnostics, ToolSeverity::Warning,
                        "Duplicate part is still referenced after retargeting", duplicateUri);
                    continue;
                }
                group.DuplicatePartUris.push_back(duplicateUri);
                ++result.RemovedParts;
                result.BytesSaved += group.PayloadBytes;
            }

            if (!group.DuplicatePartUris.empty())
            {
                result.Groups.push_back(std::move(group));
            }
        }
    }

    result.Ok = std::none_of(result.Diagnostics.begin(), result.Diagnostics.end(),
                             [](const ToolDiagnostic& diagnostic)
                             { return diagnostic.Severity == ToolSeverity::Error; });
    return result;
}

ResourceDeduplicationResult DeduplicateSharedResources(const std::filesystem::path& inputFile,
                                                       const std::filesystem::path& outputFile,
                                                       const ResourceDeduplicationOptions& options)
{
    ResourceDeduplicationResult result;

    if (!options.DryRun && !options.Overwrite && outputFile != inputFile &&
        std::filesystem::exists(outputFile))
    {
        ResourceDeduplicationHelpers::AddDiagnostic(result.Diagnostics, ToolSeverity::Error,
                                                    "Output file already exists", outputFile.string());
        return result;
    }

    OpenXmlPackage package;
    ApplyDefaultPackageLimits(package);
    if (!package.LoadFromFile(inputFile))
    {
        ResourceDeduplicationHelpers::AddDiagnostic(result.Diagnostics, ToolSeverity::Error,
                                                    "Failed to open package", inputFile.string());
        return result;
    }

    result = DeduplicateSharedResources(package, options);
    if (!result.Ok || options.DryRun)
    {
        return result;
    }

    if (!package.SaveToFile(outputFile))
    {
        ResourceDeduplicationHelpers::AddDiagnostic(result.Diagnostics, ToolSeverity::Error,
                                                    "Failed to save package", outputFile.string());
        result.Ok = false;
    }
    return result;
}

} // namespace ExyokiOffice::Tools
