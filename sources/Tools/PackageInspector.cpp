// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "ExyokiOffice/Tools/PackageInspector.hpp"

#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/ExtendedProperties.hpp"
#include "ExyokiOffice/Packaging/GeneratedParts.hpp"
#include "ExyokiOffice/Packaging/PowerPointDocument.hpp"
#include "ExyokiOffice/Packaging/SpreadsheetDocument.hpp"
#include "ExyokiOffice/Packaging/WordprocessingDocument.hpp"
#include "ConformanceClass.hpp"
#include "CorePropertiesXml.hpp"
#include "OpenXmlPackageUri.hpp"
#include "XmlParseOptions.hpp"
#include "pugixml/pugixml.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <algorithm>
#include <cctype>
#include <deque>
#include <sstream>
#include <unordered_set>

namespace ExyokiOffice::Tools
{

/// File-local helpers behind package inspection.
class PackageInspectorHelper
{
public:
    static std::string_view WordDocumentTypeName(Packaging::WordprocessingDocumentType type)
    {
        switch (type)
        {
            case Packaging::WordprocessingDocumentType::Document:
                return "Document";
            case Packaging::WordprocessingDocumentType::Template:
                return "Template";
            case Packaging::WordprocessingDocumentType::MacroEnabledDocument:
                return "MacroEnabledDocument";
            case Packaging::WordprocessingDocumentType::MacroEnabledTemplate:
                return "MacroEnabledTemplate";
        }
        return "Document";
    }

    static std::string_view ExcelDocumentTypeName(Packaging::SpreadsheetDocumentType type)
    {
        switch (type)
        {
            case Packaging::SpreadsheetDocumentType::Workbook:
                return "Workbook";
            case Packaging::SpreadsheetDocumentType::Template:
                return "Template";
            case Packaging::SpreadsheetDocumentType::MacroEnabledWorkbook:
                return "MacroEnabledWorkbook";
            case Packaging::SpreadsheetDocumentType::MacroEnabledTemplate:
                return "MacroEnabledTemplate";
        }
        return "Workbook";
    }

    static std::string_view PowerPointDocumentTypeName(Packaging::PowerPointDocumentType type)
    {
        switch (type)
        {
            case Packaging::PowerPointDocumentType::Presentation:
                return "Presentation";
            case Packaging::PowerPointDocumentType::MacroEnabledPresentation:
                return "MacroEnabledPresentation";
            case Packaging::PowerPointDocumentType::Template:
                return "Template";
            case Packaging::PowerPointDocumentType::MacroEnabledTemplate:
                return "MacroEnabledTemplate";
            case Packaging::PowerPointDocumentType::SlideShow:
                return "SlideShow";
            case Packaging::PowerPointDocumentType::MacroEnabledSlideShow:
                return "MacroEnabledSlideShow";
        }
        return "Presentation";
    }

    static std::string DescriptorNameForPart(const OpenXmlPackagePart& part)
    {
        const auto name = part.Descriptor().Name;
        return name.empty() ? std::string("OpaquePackagePart") : std::string(name);
    }

    // --- docProps/core.xml access -----------------------------------------------
    //
    // The element lookup itself lives in Xml::CorePropertiesXml, shared with the
    // Packaging document-property API: both used to carry a prefix-literal copy of
    // this logic, which read nothing from a document that binds the core-property
    // namespaces to other prefixes.

    static std::string GetCorePropertyText(const OpenXmlPackagePart* part, std::string_view canonicalName)
    {
        if (!part)
        {
            return {};
        }
        auto xml = part->GetXmlString();
        if (xml.empty())
        {
            return {};
        }
        Pugi::xml_document doc;
        // load_buffer, not load_string: c_str() ends the document at the first
        // embedded NUL, which would report the properties of a truncated part.
        if (!doc.load_buffer(xml.data(), xml.size(), Xml::ParseOptions::Preserving))
        {
            return {};
        }
        auto root = Xml::CorePropertiesXml::FindRoot(doc);
        if (!root)
        {
            return {};
        }
        auto node = Xml::CorePropertiesXml::FindChild(root, canonicalName);
        return node ? std::string(node.text().get()) : std::string();
    }

    /**
     * @brief Finds the first part of type T reachable from the package.
     *
     * OpenXmlPartContainer::GetPartOfType() is protected, so external Tools code
     * cannot call it on a plain OpenXmlPackage. dynamic_pointer_cast over the
     * public part list is the supported substitute.
     */
    template <typename TPart>
    static std::shared_ptr<TPart> FindPartOfType(const OpenXmlPackage& package)
    {
        for (const auto& part : CollectAllParts(package))
        {
            if (auto typed = std::dynamic_pointer_cast<TPart>(part))
            {
                return typed;
            }
        }
        return nullptr;
    }

    /// Maps a friendly CoreProperties field name (case-insensitive) to its docProps/core.xml element name.
    static std::string_view CorePropertyElementName(std::string_view name)
    {
        std::string lowered;
        lowered.reserve(name.size());
        for (char ch : name)
        {
            lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
        }
        if (lowered == "title")
        {
            return "dc:title";
        }
        if (lowered == "subject")
        {
            return "dc:subject";
        }
        if (lowered == "creator")
        {
            return "dc:creator";
        }
        if (lowered == "keywords")
        {
            return "cp:keywords";
        }
        if (lowered == "description")
        {
            return "dc:description";
        }
        if (lowered == "lastmodifiedby")
        {
            return "cp:lastModifiedBy";
        }
        if (lowered == "category")
        {
            return "cp:category";
        }
        if (lowered == "contentstatus")
        {
            return "cp:contentStatus";
        }
        return {};
    }
};

std::vector<std::shared_ptr<OpenXmlPackagePart>> CollectAllParts(const OpenXmlPackage& package)
{
    std::vector<std::shared_ptr<OpenXmlPackagePart>> result;
    std::unordered_set<std::string> visited;
    std::deque<std::shared_ptr<OpenXmlPackagePart>> queue(package.Parts().begin(), package.Parts().end());

    while (!queue.empty())
    {
        auto part = queue.front();
        queue.pop_front();
        if (!part || !visited.insert(part->Uri()).second)
        {
            continue;
        }
        result.push_back(part);
        for (const auto& child : part->Parts())
        {
            queue.push_back(child);
        }
    }

    std::sort(result.begin(), result.end(),
              [](const auto& lhs, const auto& rhs)
              { return lhs->Uri() < rhs->Uri(); });
    return result;
}

std::vector<PartRecord> ListParts(const OpenXmlPackage& package)
{
    std::vector<PartRecord> result;
    for (const auto& part : CollectAllParts(package))
    {
        PartRecord record;
        record.Uri = part->Uri();
        record.ContentType = std::string(part->ContentType());
        record.Kind = part->IsBinaryPart() ? PartPayloadKind::Binary : PartPayloadKind::Xml;
        record.DescriptorName = PackageInspectorHelper::DescriptorNameForPart(*part);
        record.Incoming = part->IncomingRelationships();
        record.Size = part->IsBinaryPart() ? part->GetBinaryData().size() : part->GetXmlString().size();
        result.push_back(std::move(record));
    }
    return result;
}

std::vector<RelationshipRecord> ListRelationships(const OpenXmlPackage& package)
{
    std::vector<RelationshipRecord> result;

    std::unordered_set<std::string> partUris;
    for (const auto& part : CollectAllParts(package))
    {
        partUris.insert(part->Uri());
    }

    auto appendFrom = [&](std::string_view sourceUri, const OpenXmlPartContainer& container)
    {
        for (const auto& relationship : container.Relationships())
        {
            RelationshipRecord record;
            record.SourceUri = std::string(sourceUri);
            record.Relationship = relationship;
            if (!relationship.IsExternal)
            {
                record.ResolvedTargetUri = Detail::ResolveRelationshipTarget(sourceUri, relationship.Target);
                record.TargetExists = partUris.count(record.ResolvedTargetUri) != 0;
            }
            result.push_back(std::move(record));
        }
    };

    appendFrom("/", package);
    for (const auto& part : CollectAllParts(package))
    {
        appendFrom(part->Uri(), *part);
    }

    return result;
}

PackageInfo GetInfo(const OpenXmlPackage& package)
{
    PackageInfo info;

    std::shared_ptr<OpenXmlPackagePart> mainPart;
    for (const auto& relationship : package.Relationships())
    {
        if (relationship.IsExternal)
        {
            continue;
        }
        if (ConformanceClass::IsStrictOfficeDocument(relationship.Type))
        {
            info.IsStrictConformance = true;
            break;
        }
        if (ConformanceClass::IsTransitionalOfficeDocument(relationship.Type))
        {
            const auto resolved = Detail::ResolveRelationshipTarget("/", relationship.Target);
            mainPart = package.GetPartByUri(resolved);
            break;
        }
    }

    if (mainPart)
    {
        info.MainPartUri = mainPart->Uri();
        info.MainPartContentType = std::string(mainPart->ContentType());

        if (auto wordType = Packaging::WordDocument::DocumentTypeFromMime(info.MainPartContentType))
        {
            info.Family = DocumentFamily::Word;
            info.DocumentTypeName = PackageInspectorHelper::WordDocumentTypeName(*wordType);
        }
        else if (auto excelType = Packaging::ExcelDocument::DocumentTypeFromMime(info.MainPartContentType))
        {
            info.Family = DocumentFamily::Excel;
            info.DocumentTypeName = PackageInspectorHelper::ExcelDocumentTypeName(*excelType);
        }
        else if (auto pptType = Packaging::PowerPointDocument::DocumentTypeFromMime(info.MainPartContentType))
        {
            info.Family = DocumentFamily::PowerPoint;
            info.DocumentTypeName = PackageInspectorHelper::PowerPointDocumentTypeName(*pptType);
        }
    }

    info.Properties = ReadCoreProperties(package);

    const auto parts = CollectAllParts(package);
    info.PartCount = parts.size();
    for (const auto& part : parts)
    {
        info.TotalPartSize += part->IsBinaryPart() ? part->GetBinaryData().size() : part->GetXmlString().size();
    }
    info.RelationshipCount = ListRelationships(package).size();

    return info;
}

std::string DescribeUnknownFamily(const PackageInfo& info)
{
    if (info.IsStrictConformance)
    {
        return "ISO 29500 Strict packages are not supported; only the Transitional conformance class "
               "is (see docs/Compatibility.md). Re-save the document as Transitional to process it.";
    }
    return "Unrecognized document family";
}

CoreProperties ReadCoreProperties(const OpenXmlPackage& package)
{
    CoreProperties properties;

    auto corePart = PackageInspectorHelper::FindPartOfType<Packaging::CoreFilePropertiesPart>(package);
    if (corePart)
    {
        properties.Title = PackageInspectorHelper::GetCorePropertyText(corePart.get(), "dc:title");
        properties.Subject = PackageInspectorHelper::GetCorePropertyText(corePart.get(), "dc:subject");
        properties.Creator = PackageInspectorHelper::GetCorePropertyText(corePart.get(), "dc:creator");
        properties.Keywords = PackageInspectorHelper::GetCorePropertyText(corePart.get(), "cp:keywords");
        properties.Description = PackageInspectorHelper::GetCorePropertyText(corePart.get(), "dc:description");
        properties.LastModifiedBy = PackageInspectorHelper::GetCorePropertyText(corePart.get(), "cp:lastModifiedBy");
        properties.Category = PackageInspectorHelper::GetCorePropertyText(corePart.get(), "cp:category");
        properties.ContentStatus = PackageInspectorHelper::GetCorePropertyText(corePart.get(), "cp:contentStatus");
        properties.Created = PackageInspectorHelper::GetCorePropertyText(corePart.get(), "dcterms:created");
        properties.Modified = PackageInspectorHelper::GetCorePropertyText(corePart.get(), "dcterms:modified");
    }

    auto extendedPart = PackageInspectorHelper::FindPartOfType<Packaging::ExtendedFilePropertiesPart>(package);
    if (extendedPart)
    {
        if (auto root = extendedPart->GetTypedRootElement())
        {
            using namespace ExyokiOffice::DocumentFormat::OpenXml::ExtendedProperties;
            if (auto application = root->GetFirstChildOfType<Application>())
            {
                properties.Application = application->GetText();
            }
            if (auto appVersion = root->GetFirstChildOfType<ApplicationVersion>())
            {
                properties.AppVersion = appVersion->GetText();
            }
            if (auto company = root->GetFirstChildOfType<Company>())
            {
                properties.Company = company->GetText();
            }
        }
    }

    return properties;
}

bool WriteCoreProperty(OpenXmlPackage& package, std::string_view name, std::string_view value)
{
    std::string lowered;
    lowered.reserve(name.size());
    for (char ch : name)
    {
        lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }

    if (lowered == "company")
    {
        auto extendedPart = PackageInspectorHelper::FindPartOfType<Packaging::ExtendedFilePropertiesPart>(package);
        if (!extendedPart)
        {
            return false;
        }
        auto root = extendedPart->GetTypedRootElement();
        if (!root)
        {
            return false;
        }
        using namespace ExyokiOffice::DocumentFormat::OpenXml::ExtendedProperties;
        auto company = root->GetFirstChildOfType<Company>();
        if (!company)
        {
            company = root->AppendChild<Company>();
        }
        if (!company)
        {
            return false;
        }
        company->SetText(value);
        return true;
    }

    const auto elementName = PackageInspectorHelper::CorePropertyElementName(name);
    if (elementName.empty())
    {
        return false;
    }

    auto corePart = PackageInspectorHelper::FindPartOfType<Packaging::CoreFilePropertiesPart>(package);
    if (!corePart)
    {
        return false;
    }

    auto xml = corePart->GetXmlString();
    Pugi::xml_document doc;
    if (!xml.empty())
    {
        doc.load_buffer(xml.data(), xml.size(), Xml::ParseOptions::Preserving);
    }
    auto root = Xml::CorePropertiesXml::EnsureRoot(doc);

    // An empty value clears the property by removing the element, matching the
    // Packaging API. Leaving an empty element behind would keep the name in the
    // file, which is the opposite of what clearing an identity field means.
    if (value.empty())
    {
        Xml::CorePropertiesXml::RemoveChild(root, elementName);
    }
    else
    {
        auto node = Xml::CorePropertiesXml::EnsureChild(root, elementName);
        if (!node)
        {
            return false;
        }
        if (Xml::CorePropertiesXml::IsDateProperty(elementName))
        {
            Xml::CorePropertiesXml::EnsureDateTypeAttribute(node);
        }
        node.text().set(std::string(value).c_str());
    }

    std::ostringstream output;
    doc.save(output);
    corePart->SetXmlString(output.str());
    return true;
}

} // namespace ExyokiOffice::Tools
