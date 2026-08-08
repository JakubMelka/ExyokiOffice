// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "ExyokiOffice/Security/ExternalResources.hpp"

#include "ExternalResourceGateway.hpp"

#include <utility>

namespace ExyokiOffice::Security
{

/// Builds the request the gateway evaluates from one enumerated reference.
class ExternalResourceRequestBuilder final
{
public:
    ExternalResourceRequestBuilder() = delete;

    static ExternalResourceRequest Build(const ExternalReference& reference,
                                         ExternalResourceAccess access,
                                         const ICancellationToken* cancellationToken)
    {
        ExternalResourceRequest request;
        request.Uri = reference.Target;
        request.OriginalTarget = reference.Target;
        request.Kind = reference.Kind;
        request.Access = access;
        request.SourcePartUri = reference.SourcePartUri;
        request.RelationshipId = reference.RelationshipId;
        request.RelationshipType = reference.RelationshipType;
        request.CancellationToken = cancellationToken;
        return request;
    }
};

std::vector<ExternalReference> CollectExternalReferences(const OpenXmlPackage& package)
{
    return ExternalResourceGateway::Collect(package);
}

ExternalResourceStatus CheckExternalReference(OpenXmlPackage& package, const ExternalReference& reference)
{
    auto request = ExternalResourceRequestBuilder::Build(reference, ExternalResourceAccess::CheckOnly, nullptr);
    return ExternalResourceGateway::Fetch(package, std::move(request)).Status;
}

ExternalResourceResponse ResolveExternalResource(OpenXmlPackage& package,
                                                 const ExternalReference& reference,
                                                 const ICancellationToken* cancellationToken)
{
    auto request = ExternalResourceRequestBuilder::Build(reference, ExternalResourceAccess::Read, cancellationToken);
    return ExternalResourceGateway::Fetch(package, std::move(request));
}

ExternalResourceResponse ResolveExternalResource(OpenXmlPackage& package,
                                                 std::string_view sourcePartUri,
                                                 std::string_view relationshipId,
                                                 const ICancellationToken* cancellationToken)
{
    const auto reference = ExternalResourceGateway::Find(package, sourcePartUri, relationshipId);
    if (!reference.has_value())
    {
        ExternalResourceResponse response;
        response.Status = ExternalResourceStatus::AccessDenied;
        response.Message = "The package has no external relationship \"" + std::string(relationshipId) + "\" in \"" +
                           std::string(sourcePartUri) + "\".";
        return response;
    }

    return ResolveExternalResource(package, *reference, cancellationToken);
}

} // namespace ExyokiOffice::Security
