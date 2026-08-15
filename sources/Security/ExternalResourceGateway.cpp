// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "ExternalResourceGateway.hpp"

#include "OpenXmlPackageInternal.hpp"
#include "ExyokiOffice/StandardTypes.hpp"
#include "AsciiText.hpp"

#include <algorithm>
#include <utility>

namespace ExyokiOffice::Security
{

std::vector<ExternalReference> ExternalResourceGateway::Collect(const OpenXmlPackage& package)
{
    std::vector<ExternalReference> references;

    const auto append = [&references](const std::string& sourceUri, const std::vector<OpenXmlRelationship>& source)
    {
        for (const auto& relationship : source)
        {
            if (!relationship.IsExternal)
            {
                continue;
            }

            ExternalReference reference;
            reference.SourcePartUri = sourceUri;
            reference.RelationshipId = relationship.Id;
            reference.RelationshipType = relationship.Type;
            reference.Target = relationship.Target;
            reference.Kind = ClassifyRelationshipType(relationship.Type);
            references.push_back(std::move(reference));
        }
    };

    append("/", package.Relationships());

    // partsByUri holds every part once, unlike the relationship graph, where a
    // shared part is reachable through several edges.
    std::vector<std::string> partUris;
    partUris.reserve(package.m_impl->partsByUri.size());
    for (const auto& [uri, part] : package.m_impl->partsByUri)
    {
        partUris.push_back(uri);
    }
    std::sort(partUris.begin(), partUris.end());

    for (const auto& uri : partUris)
    {
        const auto& part = package.m_impl->partsByUri.at(uri);
        if (part)
        {
            append(uri, part->Relationships());
        }
    }

    return references;
}

std::optional<ExternalReference> ExternalResourceGateway::Find(const OpenXmlPackage& package,
                                                               std::string_view sourcePartUri,
                                                               std::string_view relationshipId)
{
    for (const auto& reference : Collect(package))
    {
        if (reference.SourcePartUri == sourcePartUri && reference.RelationshipId == relationshipId)
        {
            return reference;
        }
    }
    return std::nullopt;
}

void ExternalResourceGateway::Report(OpenXmlPackage& package,
                                     const ExternalResourceRequest& request,
                                     ValidationErrorId id,
                                     const std::string& message)
{
    ValidationIssue issue;
    issue.Severity = ValidationSeverity::Warning;
    issue.Domain = ValidationDomain::Security;
    issue.Id = id;
    issue.Message = message;
    issue.PartUri = request.SourcePartUri;
    issue.RelationshipSourceUri = request.SourcePartUri;
    issue.RelationshipId = request.RelationshipId;
    issue.RelationshipType = request.RelationshipType;
    issue.TargetUri = request.OriginalTarget.empty() ? request.Uri : request.OriginalTarget;
    issue.TargetMode = "External";
    package.m_impl->lastValidationResult.AddIssue(std::move(issue));
}

ExternalResourceResponse ExternalResourceGateway::Deny(OpenXmlPackage& package,
                                                       const ExternalResourceRequest& request,
                                                       ExternalResourceStatus status,
                                                       ValidationErrorId id,
                                                       std::string message)
{
    Report(package, request, id, message);

    ExternalResourceResponse response;
    response.Status = status;
    response.Message = std::move(message);
    return response;
}

bool ExternalResourceGateway::IsKindAllowed(const ExternalResourcePolicy& policy, ExternalResourceKind kind)
{
    return std::find(policy.AllowedKinds.begin(), policy.AllowedKinds.end(), kind) != policy.AllowedKinds.end();
}

bool ExternalResourceGateway::IsHostAllowed(const ExternalResourcePolicy& policy, const std::string& host)
{
    for (const auto& allowed : policy.AllowedHosts)
    {
        if (allowed.empty())
        {
            continue;
        }

        const std::string lowered = AsciiText::ToLower(allowed);

        // A leading dot means "this domain and everything under it".
        if (lowered.front() == '.')
        {
            if (host == std::string_view(lowered).substr(1) || host.ends_with(lowered))
            {
                return true;
            }
            continue;
        }

        if (host == lowered)
        {
            return true;
        }
    }
    return false;
}

bool ExternalResourceGateway::IsPortAllowed(const ExternalResourcePolicy& policy, const ExternalUriParts& parts)
{
    const auto defaultPort = ExternalUri::DefaultPort(parts.Scheme);
    const auto effective = parts.Port.value_or(defaultPort);

    if (policy.AllowedPorts.empty())
    {
        // Without an explicit list only the scheme's own port is acceptable, so
        // a target cannot quietly reach an unrelated service on an allowed host.
        return effective == defaultPort;
    }
    return std::find(policy.AllowedPorts.begin(), policy.AllowedPorts.end(), effective) != policy.AllowedPorts.end();
}

bool ExternalResourceGateway::IsPathAllowed(const ExternalResourcePolicy& policy, const std::string& path)
{
    if (policy.AllowedPathPrefixes.empty())
    {
        return true;
    }

    for (const auto& prefix : policy.AllowedPathPrefixes)
    {
        if (prefix.empty() || !path.starts_with(prefix))
        {
            continue;
        }
        // "/data" must not match "/database"; require a directory boundary.
        if (path.size() == prefix.size() || prefix.back() == '/' || path[prefix.size()] == '/')
        {
            return true;
        }
    }
    return false;
}

std::string ExternalResourceGateway::CheckAgainstPolicy(const ExternalResourcePolicy& policy,
                                                        const ExternalUriParts& parts)
{
    if (std::find(policy.AllowedSchemes.begin(), policy.AllowedSchemes.end(), parts.Scheme) ==
        policy.AllowedSchemes.end())
    {
        return "the scheme \"" + parts.Scheme + "\" is not in the allowed schemes";
    }

    if (!parts.UserInfo.empty() && !policy.AllowUserInfoInUri)
    {
        return "the target carries credentials in the URI";
    }

    if (ExternalUri::IsFileSystemScheme(parts.Scheme) && parts.HadTraversal)
    {
        return "the target path navigates upwards";
    }

    // A file URI addressing the local machine has no host, and that is the only
    // case in which an empty host is acceptable.
    if (parts.Host.empty())
    {
        if (!ExternalUri::IsFileSystemScheme(parts.Scheme))
        {
            return "the target names no host";
        }
    }
    else if (!IsHostAllowed(policy, parts.Host))
    {
        return "the host \"" + parts.Host + "\" is not in the allowed hosts";
    }

    if (!IsPortAllowed(policy, parts))
    {
        return "the target uses a port that is not allowed";
    }

    if (!IsPathAllowed(policy, parts.Path))
    {
        return "the path \"" + parts.Path + "\" is outside the allowed path prefixes";
    }

    return {};
}

ExternalResourceResponse ExternalResourceGateway::Fetch(OpenXmlPackage& package, ExternalResourceRequest request)
{
    const auto& policy = package.m_impl->externalResourcePolicy;
    const auto& resolver = package.m_impl->externalResourceResolver;

    if (request.OriginalTarget.empty())
    {
        request.OriginalTarget = request.Uri;
    }

    if (!resolver && request.Access == ExternalResourceAccess::Read)
    {
        return Deny(package,
                    request,
                    ExternalResourceStatus::NoResolver,
                    ValidationErrorId::ExternalResourceNoResolver,
                    "No external resource resolver is installed, so nothing outside the package was accessed.");
    }

    if (!IsKindAllowed(policy, request.Kind))
    {
        return Deny(package,
                    request,
                    ExternalResourceStatus::AccessDenied,
                    ValidationErrorId::ExternalResourceDenied,
                    "External resources of kind " + std::string(ToString(request.Kind)) +
                        " are not allowed by the package policy.");
    }

    // A relative target has no meaning of its own; only a base makes one.
    std::string absolute = request.Uri;
    if (!ExternalUri::IsAbsolute(absolute))
    {
        if (policy.BaseUri.empty())
        {
            return Deny(package,
                        request,
                        ExternalResourceStatus::AccessDenied,
                        ValidationErrorId::ExternalResourceDenied,
                        "The target \"" + request.OriginalTarget +
                            "\" is relative and the policy sets no base URI to resolve it against.");
        }
        absolute = ExternalUri::ResolveAgainstBase(policy.BaseUri, absolute);
    }

    const auto parts = ExternalUri::Parse(absolute);
    if (!parts.has_value())
    {
        return Deny(package,
                    request,
                    ExternalResourceStatus::AccessDenied,
                    ValidationErrorId::ExternalResourceDenied,
                    "The target \"" + request.OriginalTarget + "\" is not a URI this library can evaluate.");
    }

    if (const auto reason = CheckAgainstPolicy(policy, *parts); !reason.empty())
    {
        return Deny(package,
                    request,
                    ExternalResourceStatus::AccessDenied,
                    ValidationErrorId::ExternalResourceDenied,
                    "Access to \"" + parts->Normalized + "\" was denied because " + reason + ".");
    }

    request.Uri = parts->Normalized;

    if (request.Access == ExternalResourceAccess::CheckOnly)
    {
        ExternalResourceResponse response;
        response.Status = ExternalResourceStatus::Ok;
        return response;
    }

    if (policy.MaxRequests != 0 && package.m_impl->externalRequestCount >= policy.MaxRequests)
    {
        return Deny(package,
                    request,
                    ExternalResourceStatus::BudgetExceeded,
                    ValidationErrorId::ExternalResourceBudgetExceeded,
                    "The package has already made the maximum number of external resource requests.");
    }

    UInt64 remaining = policy.MaxResourceBytes;
    if (policy.MaxTotalBytes != 0)
    {
        if (package.m_impl->externalBytesUsed >= policy.MaxTotalBytes)
        {
            return Deny(package,
                        request,
                        ExternalResourceStatus::BudgetExceeded,
                        ValidationErrorId::ExternalResourceBudgetExceeded,
                        "The package has already read the maximum total number of external resource bytes.");
        }

        const auto left = policy.MaxTotalBytes - package.m_impl->externalBytesUsed;
        remaining = remaining == 0 ? left : std::min(remaining, left);
    }

    request.Timeout = policy.Timeout;
    request.MaxBytes = remaining;

    ExternalResourceResponse response;
    try
    {
        response = resolver->Resolve(request);
    }
    catch (...)
    {
        // A resolver is application code. It must not throw, but a library that
        // reports failures through status codes cannot let one escape either.
        return Deny(package,
                    request,
                    ExternalResourceStatus::Failed,
                    ValidationErrorId::ExternalResourceUnavailable,
                    "The external resource resolver threw an exception for \"" + request.Uri + "\".");
    }

    ++package.m_impl->externalRequestCount;

    if (request.CancellationToken != nullptr && request.CancellationToken->IsCancelled())
    {
        response.Data.clear();
        response.Status = ExternalResourceStatus::Cancelled;
        return response;
    }

    if (response.Status != ExternalResourceStatus::Ok)
    {
        response.Data.clear();
        Report(package,
               request,
               ValidationErrorId::ExternalResourceUnavailable,
               "The resolver reported " + std::string(ToString(response.Status)) + " for \"" + request.Uri + "\"" +
                   (response.Message.empty() ? std::string(".") : ": " + response.Message));
        return response;
    }

    if (request.MaxBytes != 0 && response.Data.size() > request.MaxBytes)
    {
        response.Data.clear();
        response.Status = ExternalResourceStatus::TooLarge;
        Report(package,
               request,
               ValidationErrorId::ExternalResourceTooLarge,
               "The resolver returned more data than the policy allows for \"" + request.Uri + "\".");
        return response;
    }

    // A resolver that follows redirects can end up somewhere the policy never
    // approved, so the destination it actually read from is checked as well.
    if (!response.EffectiveUri.empty() && response.EffectiveUri != request.Uri &&
        !policy.FollowRedirectOutsideAllowlist)
    {
        const auto effective = ExternalUri::Parse(response.EffectiveUri);
        const auto reason =
            effective.has_value() ? CheckAgainstPolicy(policy, *effective) : std::string("it is not a usable URI");
        if (!reason.empty())
        {
            response.Data.clear();
            response.Status = ExternalResourceStatus::AccessDenied;
            Report(package,
                   request,
                   ValidationErrorId::ExternalResourceDenied,
                   "The resolver read \"" + response.EffectiveUri + "\" instead, which was denied because " + reason +
                       ".");
            return response;
        }
    }

    package.m_impl->externalBytesUsed += response.Data.size();
    return response;
}

} // namespace ExyokiOffice::Security
