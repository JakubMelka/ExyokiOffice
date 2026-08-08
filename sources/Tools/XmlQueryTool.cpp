// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "ExyokiOffice/Tools/XmlQueryTool.hpp"
#include "ExyokiOffice/Tools/PackageLimits.hpp"

#include "ExyokiOffice/OpenXMLElement.hpp"
#include "ExyokiOffice/OpenXmlPackage.hpp"
#include "ExyokiOffice/OpenXmlPackagePart.hpp"
#include "ExyokiOffice/Xml/XmlQuery.hpp"
#include "ConformanceClass.hpp"
#include "OpenXmlPackageUri.hpp"

namespace ExyokiOffice::Tools
{

namespace
{

void AddError(QueryResult& result, std::string message, std::string context = {})
{
    result.Diagnostics.push_back(ToolDiagnostic{ToolSeverity::Error, std::move(message), std::move(context)});
}

std::shared_ptr<OpenXmlPackagePart> FindMainDocumentPart(const OpenXmlPackage& package)
{
    for (const auto& relationship : package.Relationships())
    {
        if (ConformanceClass::IsTransitionalOfficeDocument(relationship.Type) && !relationship.IsExternal)
        {
            const auto resolved = Detail::ResolveRelationshipTarget("/", relationship.Target);
            return package.GetPartByUri(resolved);
        }
    }
    return nullptr;
}

} // namespace

QueryResult Query(const std::filesystem::path& packagePath, std::string_view xpath, const QueryOptions& options)
{
    OpenXmlPackage package;
    ApplyDefaultPackageLimits(package);
    if (!package.LoadFromFile(packagePath))
    {
        QueryResult result;
        AddError(result, "Failed to open package.", packagePath.string());
        return result;
    }

    return Query(package, xpath, options);
}

QueryResult Query(const OpenXmlPackage& package, std::string_view xpath, const QueryOptions& options)
{
    QueryResult result;

    std::shared_ptr<OpenXmlPackagePart> part;
    if (options.PartName.empty())
    {
        part = FindMainDocumentPart(package);
        if (!part)
        {
            AddError(result, "Package has no main document part; specify a part explicitly.");
            return result;
        }
    }
    else
    {
        const auto normalized = Detail::NormalizePartUri(options.PartName);
        part = package.GetPartByUri(normalized);
        if (!part)
        {
            AddError(result, "Part not found in package.", options.PartName);
            return result;
        }
    }

    if (!part->IsXmlPart())
    {
        AddError(result, "Part is not an XML part.", part->Uri());
        return result;
    }

    result.PartName = part->Uri();

    const auto root = part->GetRootElement();
    if (!root)
    {
        AddError(result, "Part has no XML root element.", part->Uri());
        return result;
    }

    Xml::XmlQueryOptions xmlOptions;
    xmlOptions.NamespaceBindings = options.NamespaceBindings;

    std::string error;
    const auto nodes = Xml::SelectNodes(root, xpath, xmlOptions, &error);
    if (!error.empty())
    {
        AddError(result, error, std::string(xpath));
        return result;
    }

    for (const auto& node : nodes)
    {
        if (options.MaxMatches != 0 && result.Matches.size() >= options.MaxMatches)
        {
            break;
        }
        auto described = Xml::Describe(node);
        QueryMatch match;
        match.Location = std::move(described.Location);
        match.Name = std::move(described.Name);
        match.Attributes = std::move(described.Attributes);
        match.Text = std::move(described.Text);
        result.Matches.push_back(std::move(match));
    }

    result.Ok = true;
    return result;
}

} // namespace ExyokiOffice::Tools
