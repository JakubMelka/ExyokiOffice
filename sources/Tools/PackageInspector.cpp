// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "ExyokiOffice/Tools/PackageInspector.hpp"

#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/ExtendedProperties.hpp"
#include "ExyokiOffice/Packaging/DocumentProperties.hpp"
#include "ExyokiOffice/Packaging/GeneratedParts.hpp"
#include "ExyokiOffice/Packaging/PowerPointDocument.hpp"
#include "ExyokiOffice/Packaging/SpreadsheetDocument.hpp"
#include "ExyokiOffice/Packaging/WordprocessingDocument.hpp"
#include "AsciiText.hpp"
#include "ConformanceClass.hpp"
#include "CorePropertiesXml.hpp"
#include "OpenXmlPackageUri.hpp"
#include "XmlParseOptions.hpp"
#include "pugixml/pugixml.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <algorithm>
#include <array>
#include <deque>
#include <string>
#include <unordered_set>
#include <utility>

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

/**
 * @brief The property names this layer understands, mapped onto their setters.
 *
 * The spellings are the field names of CoreProperties, so that what
 * ReadCoreProperties reports back is what WriteCoreProperty accepts. The list
 * is longer than that structure because the underlying editor covers the whole
 * of docProps/core.xml and docProps/app.xml; a name it has a slot for must go
 * there rather than become a user-defined property that happens to share the
 * name.
 */
class CorePropertySetters
{
public:
    using Setter = bool (Packaging::DocumentProperties::*)(std::string_view);

    static constexpr std::array<std::pair<std::string_view, Setter>, 18> All{
        {{"Title", &Packaging::DocumentProperties::SetTitle},
         {"Subject", &Packaging::DocumentProperties::SetSubject},
         {"Creator", &Packaging::DocumentProperties::SetCreator},
         {"Keywords", &Packaging::DocumentProperties::SetKeywords},
         {"Description", &Packaging::DocumentProperties::SetDescription},
         {"LastModifiedBy", &Packaging::DocumentProperties::SetLastModifiedBy},
         {"Category", &Packaging::DocumentProperties::SetCategory},
         {"ContentStatus", &Packaging::DocumentProperties::SetContentStatus},
         {"Language", &Packaging::DocumentProperties::SetLanguage},
         {"Identifier", &Packaging::DocumentProperties::SetIdentifier},
         {"Revision", &Packaging::DocumentProperties::SetRevision},
         {"Version", &Packaging::DocumentProperties::SetVersion},
         {"Application", &Packaging::DocumentProperties::SetApplication},
         {"AppVersion", &Packaging::DocumentProperties::SetApplicationVersion},
         {"ApplicationVersion", &Packaging::DocumentProperties::SetApplicationVersion},
         {"Company", &Packaging::DocumentProperties::SetCompany},
         {"Manager", &Packaging::DocumentProperties::SetManager},
         {"HyperlinkBase", &Packaging::DocumentProperties::SetHyperlinkBase}}};

    /// The timestamps, which are typed rather than free text; see WriteCoreProperty.
    static constexpr std::array<std::string_view, 3> Timestamps{"Created", "Modified", "LastPrinted"};
};

std::vector<Packaging::DocumentCustomProperty> ReadCustomProperties(OpenXmlPackage& package)
{
    return Packaging::DocumentProperties(package).GetCustomProperties();
}

bool WriteCoreProperty(OpenXmlPackage& package, std::string_view name, std::string_view value)
{
    if (name.empty())
    {
        return false;
    }

    Packaging::DocumentProperties properties(package);

    for (const auto& [candidate, setter] : CorePropertySetters::All)
    {
        if (AsciiText::EqualsIgnoreCase(name, candidate))
        {
            return (properties.*setter)(value);
        }
    }

    for (const auto& timestamp : CorePropertySetters::Timestamps)
    {
        // A timestamp is a point in time, not a string: the editor takes one,
        // and writing whatever text arrived would produce a date no reader can
        // parse. Refused rather than diverted into a custom property, which
        // would leave the document with two properties of the same name.
        if (AsciiText::EqualsIgnoreCase(name, timestamp))
        {
            return false;
        }
    }

    // Everything else is a user-defined property, which is what the name the
    // caller chose says it is. An empty value clears it, matching the core
    // setters above, and clearing one that was never there is not a failure.
    if (value.empty())
    {
        properties.RemoveCustomProperty(name);
        return true;
    }

    return properties.SetCustomProperty(name, std::string(value));
}

} // namespace ExyokiOffice::Tools
