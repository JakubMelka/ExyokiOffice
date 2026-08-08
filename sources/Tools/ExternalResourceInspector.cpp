// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "ExyokiOffice/Tools/ExternalResourceInspector.hpp"

namespace ExyokiOffice::Tools
{

ExternalResourceReport InspectExternalResources(const OpenXmlPackage& package)
{
    ExternalResourceReport report;
    report.Loaded = true;
    for (const auto& reference : Security::CollectExternalReferences(package))
    {
        ExternalReferenceRecord record;
        record.SourcePartUri = reference.SourcePartUri;
        record.RelationshipId = reference.RelationshipId;
        record.RelationshipType = reference.RelationshipType;
        record.Target = reference.Target;
        record.Kind = Security::ToString(reference.Kind);
        report.References.push_back(std::move(record));
    }
    return report;
}

ExternalResourceReport InspectExternalResources(const std::filesystem::path& path,
                                                const ExternalResourceInspectionOptions& options)
{
    OpenXmlPackage package;
    package.SetPackageLimits(options.Limits);
    // Relationships are all that is needed, so the loaded bytes are of no use here.
    package.SetPartByteRetention(PartByteRetention::Never);
    if (!package.LoadFromFile(path))
    {
        ExternalResourceReport report;
        report.Diagnostics.push_back(
            ToolDiagnostic{ToolSeverity::Error, "The package could not be loaded.", path.string()});
        return report;
    }
    return InspectExternalResources(package);
}

} // namespace ExyokiOffice::Tools
