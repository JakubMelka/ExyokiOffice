// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "ExyokiOffice/Packaging/DocumentProperties.hpp"

#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/CustomProperties.hpp"
#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/ExtendedProperties.hpp"
#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/VariantTypes.hpp"
#include "ExyokiOffice/OpenXmlPackage.hpp"
#include "ExyokiOffice/OpenXmlPackagePart.hpp"
#include "ExyokiOffice/Packaging/GeneratedParts.hpp"
#include "CorePropertiesXml.hpp"
#include "XmlParseOptions.hpp"
#include "pugixml/pugixml.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include "AsciiText.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <limits>
#include <sstream>
#include <string>
#include <utility>

namespace ExyokiOffice::Packaging
{

namespace Ap = ExyokiOffice::DocumentFormat::OpenXml::ExtendedProperties;
namespace Op = ExyokiOffice::DocumentFormat::OpenXml::CustomProperties;
namespace Vt = ExyokiOffice::DocumentFormat::OpenXml::VariantTypes;

/// Format id Office assigns to the user-defined custom property set.
constexpr std::string_view kUserDefinedFormatId = "{D5CDD505-2E9C-101B-9397-08002B2CF9AE}";
/// Property ids 0 and 1 are reserved (dictionary and code page).
constexpr Int32 kFirstCustomPropertyId = 2;

/// Core, extended, and custom document-property plumbing shared by the
/// DocumentProperties accessors. Core-property element lookup itself lives in
/// Xml::CorePropertiesXml, which resolves namespace URIs instead of assuming
/// the conventional cp:/dc:/dcterms: prefixes; this class used to carry a
/// prefix-literal copy of it.
class DocumentPropertyHelpers
{
public:
    using CoreXml = Xml::CorePropertiesXml;

    static Pugi::xml_node FindCorePropertyChild(Pugi::xml_node parent, std::string_view qualifiedName)
    {
        return CoreXml::FindChild(parent, qualifiedName);
    }

    static Pugi::xml_node EnsureCorePropertyChild(Pugi::xml_node parent, std::string_view qualifiedName)
    {
        return CoreXml::EnsureChild(parent, qualifiedName);
    }

    static Pugi::xml_node FindCorePropertiesRoot(Pugi::xml_document& doc)
    {
        return CoreXml::FindRoot(doc);
    }

    static Pugi::xml_node EnsureCorePropertiesRoot(Pugi::xml_document& doc)
    {
        return CoreXml::EnsureRoot(doc);
    }

    static bool IsDublinCoreTermsDate(std::string_view qualifiedName)
    {
        return CoreXml::IsDateProperty(qualifiedName);
    }

    template <typename TPart>
    static std::shared_ptr<TPart> FindPackagePart(const OpenXmlPackage& package)
    {
        for (const auto& part : package.Parts())
        {
            if (auto typed = std::dynamic_pointer_cast<TPart>(part))
            {
                return typed;
            }
        }
        return nullptr;
    }

    template <typename TPart>
    static std::shared_ptr<TPart> EnsurePackagePart(OpenXmlPackage& package)
    {
        if (auto existing = FindPackagePart<TPart>(package))
        {
            return existing;
        }

        auto part = std::make_shared<TPart>();
        if (!package.AttachCustomPart(part, TPart::Descriptor(), false))
        {
            return nullptr;
        }
        return part;
    }

    static std::string LoadCorePropertiesXml(const OpenXmlPackagePart& part, Pugi::xml_document& doc)
    {
        auto xml = part.GetXmlString();
        if (!xml.empty())
        {
            // load_buffer, not load_string: c_str() ends the document at the
            // first embedded NUL, which would truncate the part silently.
            doc.load_buffer(xml.data(), xml.size(), Xml::ParseOptions::Preserving);
        }
        return xml;
    }

    static void StoreCorePropertiesXml(OpenXmlPackagePart& part, Pugi::xml_document& doc)
    {
        std::ostringstream output;
        doc.save(output);
        part.SetXmlString(output.str());
    }

    static std::string ReadCoreText(const std::shared_ptr<CoreFilePropertiesPart>& part, std::string_view qualifiedName)
    {
        if (!part)
        {
            return {};
        }

        Pugi::xml_document doc;
        if (LoadCorePropertiesXml(*part, doc).empty())
        {
            return {};
        }

        auto root = FindCorePropertiesRoot(doc);
        if (!root)
        {
            return {};
        }

        return FindCorePropertyChild(root, qualifiedName).text().get();
    }

    static bool WriteCoreText(const std::shared_ptr<CoreFilePropertiesPart>& part,
                              std::string_view qualifiedName,
                              std::string_view value)
    {
        if (!part)
        {
            return false;
        }

        Pugi::xml_document doc;
        LoadCorePropertiesXml(*part, doc);
        auto root = EnsureCorePropertiesRoot(doc);

        if (value.empty())
        {
            if (auto node = FindCorePropertyChild(root, qualifiedName))
            {
                root.remove_child(node);
            }
        }
        else
        {
            auto node = EnsureCorePropertyChild(root, qualifiedName);
            if (!node)
            {
                return false;
            }
            if (IsDublinCoreTermsDate(qualifiedName))
            {
                CoreXml::EnsureDateTypeAttribute(node);
            }
            node.text().set(value.data(), value.size());
        }

        StoreCorePropertiesXml(*part, doc);
        return true;
    }

    static std::string TrimAscii(std::string_view text)
    {
        return std::string(AsciiText::Trim(text));
    }

    static bool EqualsIgnoreAsciiCase(std::string_view left, std::string_view right)
    {
        return AsciiText::EqualsIgnoreCase(left, right);
    }

    // Value parsing delegates to the shared simple-type infrastructure so custom
    // properties follow the same xsd lexical rules as typed DOM attributes.

    static std::optional<Int64> ParseInt64(std::string_view text)
    {
        const auto parsed = OpenXmlSimpleValueConvertor::GetInt64ValueFromString(TrimAscii(text));
        if (!parsed.IsDefined())
        {
            return std::nullopt;
        }
        return parsed.Value();
    }

    static std::optional<Real> ParseDouble(std::string_view text)
    {
        const auto parsed = OpenXmlSimpleValueConvertor::GetDoubleValueFromString(TrimAscii(text));
        if (!parsed.IsDefined())
        {
            return std::nullopt;
        }
        return parsed.Value();
    }

    static std::optional<bool> ParseBool(std::string_view text)
    {
        const auto parsed = OpenXmlSimpleValueConvertor::GetBooleanValueFromString(TrimAscii(text));
        if (!parsed.IsDefined())
        {
            return std::nullopt;
        }
        return parsed.Value();
    }

    static std::shared_ptr<Ap::Properties> ExtendedRoot(const std::shared_ptr<ExtendedFilePropertiesPart>& part)
    {
        return part ? part->GetTypedRootElement() : nullptr;
    }

    template <typename TElement>
    static std::string ReadExtendedText(const std::shared_ptr<ExtendedFilePropertiesPart>& part)
    {
        auto root = ExtendedRoot(part);
        auto element = root ? root->GetFirstChildOfType<TElement>() : nullptr;
        return element ? std::string(element->GetText()) : std::string();
    }

    template <typename TElement>
    static bool WriteExtendedText(const std::shared_ptr<ExtendedFilePropertiesPart>& part, std::string_view value)
    {
        auto root = ExtendedRoot(part);
        if (!root)
        {
            return false;
        }

        auto element = root->GetFirstChildOfType<TElement>();
        if (value.empty())
        {
            if (element)
            {
                root->RemoveChild(element);
            }
            return true;
        }

        if (!element)
        {
            element = root->AppendChild<TElement>();
            if (!element)
            {
                element = root->AppendChildRaw<TElement>();
            }
        }
        if (!element)
        {
            return false;
        }
        element->SetText(value);
        return true;
    }

    template <typename TElement>
    static std::optional<Int32> ReadExtendedInt32(const std::shared_ptr<ExtendedFilePropertiesPart>& part)
    {
        const auto text = ReadExtendedText<TElement>(part);
        const auto value = ParseInt64(text);
        if (!value || *value < std::numeric_limits<Int32>::min() ||
            *value > std::numeric_limits<Int32>::max())
        {
            return std::nullopt;
        }
        return static_cast<Int32>(*value);
    }

    static std::shared_ptr<Op::Properties> CustomRoot(const std::shared_ptr<CustomFilePropertiesPart>& part)
    {
        return part ? part->GetTypedRootElement() : nullptr;
    }

    static std::shared_ptr<Op::CustomDocumentProperty> FindCustomProperty(
        const std::shared_ptr<Op::Properties>& root, std::string_view name)
    {
        if (!root)
        {
            return nullptr;
        }
        for (const auto& property : root->Elements<Op::CustomDocumentProperty>())
        {
            if (EqualsIgnoreAsciiCase(property->GetName().ToString(), name))
            {
                return property;
            }
        }
        return nullptr;
    }

    static std::optional<DocumentCustomPropertyValue> ReadCustomPropertyValue(
        const std::shared_ptr<Op::CustomDocumentProperty>& property)
    {
        if (!property)
        {
            return std::nullopt;
        }

        const auto asString = [&](const auto& element) -> std::optional<DocumentCustomPropertyValue>
        {
            return std::string(element->GetText());
        };
        if (auto element = property->GetFirstChildOfType<Vt::VTLPWSTR>())
        {
            return asString(element);
        }
        if (auto element = property->GetFirstChildOfType<Vt::VTLPSTR>())
        {
            return asString(element);
        }
        if (auto element = property->GetFirstChildOfType<Vt::VTBString>())
        {
            return asString(element);
        }

        const auto asInt32 = [](std::string_view text) -> std::optional<DocumentCustomPropertyValue>
        {
            const auto value = ParseInt64(text);
            if (!value)
            {
                return std::nullopt;
            }
            if (*value >= std::numeric_limits<Int32>::min() &&
                *value <= std::numeric_limits<Int32>::max())
            {
                return static_cast<Int32>(*value);
            }
            return static_cast<Real>(*value);
        };
        if (auto element = property->GetFirstChildOfType<Vt::VTInt32>())
        {
            return asInt32(element->GetText());
        }
        if (auto element = property->GetFirstChildOfType<Vt::VTShort>())
        {
            return asInt32(element->GetText());
        }
        if (auto element = property->GetFirstChildOfType<Vt::VTByte>())
        {
            return asInt32(element->GetText());
        }
        if (auto element = property->GetFirstChildOfType<Vt::VTUnsignedByte>())
        {
            return asInt32(element->GetText());
        }
        if (auto element = property->GetFirstChildOfType<Vt::VTUnsignedShort>())
        {
            return asInt32(element->GetText());
        }
        if (auto element = property->GetFirstChildOfType<Vt::VTInt64>())
        {
            return asInt32(element->GetText());
        }
        if (auto element = property->GetFirstChildOfType<Vt::VTInteger>())
        {
            return asInt32(element->GetText());
        }
        if (auto element = property->GetFirstChildOfType<Vt::VTUnsignedInt32>())
        {
            return asInt32(element->GetText());
        }
        if (auto element = property->GetFirstChildOfType<Vt::VTUnsignedInt64>())
        {
            return asInt32(element->GetText());
        }
        if (auto element = property->GetFirstChildOfType<Vt::VTUnsignedInteger>())
        {
            return asInt32(element->GetText());
        }

        const auto asDouble = [](std::string_view text) -> std::optional<DocumentCustomPropertyValue>
        {
            const auto value = ParseDouble(text);
            if (!value)
            {
                return std::nullopt;
            }
            return *value;
        };
        if (auto element = property->GetFirstChildOfType<Vt::VTDouble>())
        {
            return asDouble(element->GetText());
        }
        if (auto element = property->GetFirstChildOfType<Vt::VTFloat>())
        {
            return asDouble(element->GetText());
        }
        if (auto element = property->GetFirstChildOfType<Vt::VTDecimal>())
        {
            return asDouble(element->GetText());
        }

        if (auto element = property->GetFirstChildOfType<Vt::VTBool>())
        {
            const auto value = ParseBool(element->GetText());
            if (!value)
            {
                return std::nullopt;
            }
            return *value;
        }

        const auto asDateTime = [](std::string_view text) -> std::optional<DocumentCustomPropertyValue>
        {
            const auto value = DocumentProperties::ParseW3cDateTime(text);
            if (!value)
            {
                return std::nullopt;
            }
            return *value;
        };
        if (auto element = property->GetFirstChildOfType<Vt::VTFileTime>())
        {
            return asDateTime(element->GetText());
        }
        if (auto element = property->GetFirstChildOfType<Vt::VTDate>())
        {
            return asDateTime(element->GetText());
        }

        return std::nullopt;
    }

    template <typename TElement>
    static bool AppendValueElement(const std::shared_ptr<Op::CustomDocumentProperty>& property, std::string_view text)
    {
        auto element = property->AppendChild<TElement>();
        if (!element)
        {
            element = property->AppendChildRaw<TElement>();
        }
        if (!element)
        {
            return false;
        }
        element->SetText(text);
        return true;
    }

    static bool WriteCustomPropertyValue(const std::shared_ptr<Op::CustomDocumentProperty>& property,
                                         const DocumentCustomPropertyValue& value)
    {
        for (const auto& child : property->Children())
        {
            property->RemoveChild(child);
        }

        return std::visit(
            [&](const auto& alternative) -> bool
            {
                using T = std::decay_t<decltype(alternative)>;
                if constexpr (std::is_same_v<T, std::string>)
                {
                    return AppendValueElement<Vt::VTLPWSTR>(property, alternative);
                }
                else if constexpr (std::is_same_v<T, Int32>)
                {
                    return AppendValueElement<Vt::VTInt32>(property, Int32Value(alternative).ToString());
                }
                else if constexpr (std::is_same_v<T, Real>)
                {
                    return AppendValueElement<Vt::VTDouble>(property, DoubleValue(alternative).ToString());
                }
                else if constexpr (std::is_same_v<T, bool>)
                {
                    // Office conventionally writes the word forms; the shared
                    // boolean parser accepts both these and "1"/"0".
                    return AppendValueElement<Vt::VTBool>(property, alternative ? "true" : "false");
                }
                else
                {
                    return AppendValueElement<Vt::VTFileTime>(
                        property, DocumentProperties::FormatW3cDateTime(alternative));
                }
            },
            value);
    }

    static Int32 NextCustomPropertyId(const std::shared_ptr<Op::Properties>& root)
    {
        Int32 maxId = kFirstCustomPropertyId - 1;
        for (const auto& property : root->Elements<Op::CustomDocumentProperty>())
        {
            maxId = std::max(maxId, property->GetPropertyId().ValueOr(0));
        }
        return maxId + 1;
    }

    static std::shared_ptr<CoreFilePropertiesPart> CorePart(const OpenXmlPackage& package)
    {
        return FindPackagePart<CoreFilePropertiesPart>(package);
    }

    static std::optional<std::chrono::system_clock::time_point> ReadCoreDate(
        const OpenXmlPackage& package, std::string_view qualifiedName)
    {
        return DocumentProperties::ParseW3cDateTime(ReadCoreText(CorePart(package), qualifiedName));
    }

    static bool WriteCoreValue(OpenXmlPackage& package, std::string_view qualifiedName, std::string_view value)
    {
        if (value.empty())
        {
            // Nothing to remove when the part does not exist yet; do not create it.
            auto part = CorePart(package);
            return part ? WriteCoreText(part, qualifiedName, value) : true;
        }
        return WriteCoreText(EnsurePackagePart<CoreFilePropertiesPart>(package), qualifiedName, value);
    }

    static bool WriteCoreDate(OpenXmlPackage& package,
                              std::string_view qualifiedName,
                              std::optional<std::chrono::system_clock::time_point> value)
    {
        return WriteCoreValue(package, qualifiedName,
                              value ? DocumentProperties::FormatW3cDateTime(*value) : std::string());
    }

    static std::shared_ptr<ExtendedFilePropertiesPart> ExtendedPart(const OpenXmlPackage& package)
    {
        return FindPackagePart<ExtendedFilePropertiesPart>(package);
    }

    template <typename TElement>
    static bool WriteExtendedValue(OpenXmlPackage& package, std::string_view value)
    {
        if (value.empty())
        {
            auto part = ExtendedPart(package);
            return part ? WriteExtendedText<TElement>(part, value) : true;
        }
        return WriteExtendedText<TElement>(EnsurePackagePart<ExtendedFilePropertiesPart>(package), value);
    }
    // Package-level conveniences so the public accessors stay one-liners.

    static std::shared_ptr<Op::Properties> CustomRootOf(const OpenXmlPackage& package)
    {
        return CustomRoot(FindPackagePart<CustomFilePropertiesPart>(package));
    }

    static std::string ReadCore(const OpenXmlPackage& package, std::string_view qualifiedName)
    {
        return ReadCoreText(CorePart(package), qualifiedName);
    }

    template <typename TElement>
    static std::string ReadExtended(const OpenXmlPackage& package)
    {
        return ReadExtendedText<TElement>(ExtendedPart(package));
    }

    template <typename TElement>
    static std::optional<Int32> ReadExtendedNumber(const OpenXmlPackage& package)
    {
        return ReadExtendedInt32<TElement>(ExtendedPart(package));
    }
};

DocumentProperties::DocumentProperties(OpenXmlPackage& package) noexcept
    : m_package(&package)
{
}

// --- core properties -------------------------------------------------------

std::string DocumentProperties::GetTitle() const
{
    return DocumentPropertyHelpers::ReadCore(*m_package, "dc:title");
}
bool DocumentProperties::SetTitle(std::string_view value)
{
    return DocumentPropertyHelpers::WriteCoreValue(*m_package, "dc:title", value);
}

std::string DocumentProperties::GetSubject() const
{
    return DocumentPropertyHelpers::ReadCore(*m_package, "dc:subject");
}
bool DocumentProperties::SetSubject(std::string_view value)
{
    return DocumentPropertyHelpers::WriteCoreValue(*m_package, "dc:subject", value);
}

std::string DocumentProperties::GetCreator() const
{
    return DocumentPropertyHelpers::ReadCore(*m_package, "dc:creator");
}
bool DocumentProperties::SetCreator(std::string_view value)
{
    return DocumentPropertyHelpers::WriteCoreValue(*m_package, "dc:creator", value);
}

std::string DocumentProperties::GetKeywords() const
{
    return DocumentPropertyHelpers::ReadCore(*m_package, "cp:keywords");
}
bool DocumentProperties::SetKeywords(std::string_view value)
{
    return DocumentPropertyHelpers::WriteCoreValue(*m_package, "cp:keywords", value);
}

std::string DocumentProperties::GetDescription() const
{
    return DocumentPropertyHelpers::ReadCore(*m_package, "dc:description");
}
bool DocumentProperties::SetDescription(std::string_view value)
{
    return DocumentPropertyHelpers::WriteCoreValue(*m_package, "dc:description", value);
}

std::string DocumentProperties::GetLastModifiedBy() const
{
    return DocumentPropertyHelpers::ReadCore(*m_package, "cp:lastModifiedBy");
}
bool DocumentProperties::SetLastModifiedBy(std::string_view value)
{
    return DocumentPropertyHelpers::WriteCoreValue(*m_package, "cp:lastModifiedBy", value);
}

std::string DocumentProperties::GetCategory() const
{
    return DocumentPropertyHelpers::ReadCore(*m_package, "cp:category");
}
bool DocumentProperties::SetCategory(std::string_view value)
{
    return DocumentPropertyHelpers::WriteCoreValue(*m_package, "cp:category", value);
}

std::string DocumentProperties::GetContentStatus() const
{
    return DocumentPropertyHelpers::ReadCore(*m_package, "cp:contentStatus");
}
bool DocumentProperties::SetContentStatus(std::string_view value)
{
    return DocumentPropertyHelpers::WriteCoreValue(*m_package, "cp:contentStatus", value);
}

std::string DocumentProperties::GetLanguage() const
{
    return DocumentPropertyHelpers::ReadCore(*m_package, "dc:language");
}
bool DocumentProperties::SetLanguage(std::string_view value)
{
    return DocumentPropertyHelpers::WriteCoreValue(*m_package, "dc:language", value);
}

std::string DocumentProperties::GetIdentifier() const
{
    return DocumentPropertyHelpers::ReadCore(*m_package, "dc:identifier");
}
bool DocumentProperties::SetIdentifier(std::string_view value)
{
    return DocumentPropertyHelpers::WriteCoreValue(*m_package, "dc:identifier", value);
}

std::string DocumentProperties::GetRevision() const
{
    return DocumentPropertyHelpers::ReadCore(*m_package, "cp:revision");
}
bool DocumentProperties::SetRevision(std::string_view value)
{
    return DocumentPropertyHelpers::WriteCoreValue(*m_package, "cp:revision", value);
}

std::string DocumentProperties::GetVersion() const
{
    return DocumentPropertyHelpers::ReadCore(*m_package, "cp:version");
}
bool DocumentProperties::SetVersion(std::string_view value)
{
    return DocumentPropertyHelpers::WriteCoreValue(*m_package, "cp:version", value);
}

std::optional<std::chrono::system_clock::time_point> DocumentProperties::GetCreated() const
{
    return DocumentPropertyHelpers::ReadCoreDate(*m_package, "dcterms:created");
}
bool DocumentProperties::SetCreated(std::optional<std::chrono::system_clock::time_point> value)
{
    return DocumentPropertyHelpers::WriteCoreDate(*m_package, "dcterms:created", value);
}

std::optional<std::chrono::system_clock::time_point> DocumentProperties::GetModified() const
{
    return DocumentPropertyHelpers::ReadCoreDate(*m_package, "dcterms:modified");
}
bool DocumentProperties::SetModified(std::optional<std::chrono::system_clock::time_point> value)
{
    return DocumentPropertyHelpers::WriteCoreDate(*m_package, "dcterms:modified", value);
}

std::optional<std::chrono::system_clock::time_point> DocumentProperties::GetLastPrinted() const
{
    return DocumentPropertyHelpers::ReadCoreDate(*m_package, "cp:lastPrinted");
}
bool DocumentProperties::SetLastPrinted(std::optional<std::chrono::system_clock::time_point> value)
{
    return DocumentPropertyHelpers::WriteCoreDate(*m_package, "cp:lastPrinted", value);
}

// --- extended properties ----------------------------------------------------

std::string DocumentProperties::GetApplication() const
{
    return DocumentPropertyHelpers::ReadExtended<Ap::Application>(*m_package);
}
bool DocumentProperties::SetApplication(std::string_view value)
{
    return DocumentPropertyHelpers::WriteExtendedValue<Ap::Application>(*m_package, value);
}

std::string DocumentProperties::GetApplicationVersion() const
{
    return DocumentPropertyHelpers::ReadExtended<Ap::ApplicationVersion>(*m_package);
}
bool DocumentProperties::SetApplicationVersion(std::string_view value)
{
    return DocumentPropertyHelpers::WriteExtendedValue<Ap::ApplicationVersion>(*m_package, value);
}

std::string DocumentProperties::GetCompany() const
{
    return DocumentPropertyHelpers::ReadExtended<Ap::Company>(*m_package);
}
bool DocumentProperties::SetCompany(std::string_view value)
{
    return DocumentPropertyHelpers::WriteExtendedValue<Ap::Company>(*m_package, value);
}

std::string DocumentProperties::GetManager() const
{
    return DocumentPropertyHelpers::ReadExtended<Ap::Manager>(*m_package);
}
bool DocumentProperties::SetManager(std::string_view value)
{
    return DocumentPropertyHelpers::WriteExtendedValue<Ap::Manager>(*m_package, value);
}

std::string DocumentProperties::GetHyperlinkBase() const
{
    return DocumentPropertyHelpers::ReadExtended<Ap::HyperlinkBase>(*m_package);
}
bool DocumentProperties::SetHyperlinkBase(std::string_view value)
{
    return DocumentPropertyHelpers::WriteExtendedValue<Ap::HyperlinkBase>(*m_package, value);
}

std::string DocumentProperties::GetTemplate() const
{
    return DocumentPropertyHelpers::ReadExtended<Ap::Template>(*m_package);
}
bool DocumentProperties::SetTemplate(std::string_view value)
{
    return DocumentPropertyHelpers::WriteExtendedValue<Ap::Template>(*m_package, value);
}

std::optional<Int32> DocumentProperties::GetPages() const
{
    return DocumentPropertyHelpers::ReadExtendedNumber<Ap::Pages>(*m_package);
}
std::optional<Int32> DocumentProperties::GetWords() const
{
    return DocumentPropertyHelpers::ReadExtendedNumber<Ap::Words>(*m_package);
}
std::optional<Int32> DocumentProperties::GetCharacters() const
{
    return DocumentPropertyHelpers::ReadExtendedNumber<Ap::Characters>(*m_package);
}
std::optional<Int32> DocumentProperties::GetCharactersWithSpaces() const
{
    return DocumentPropertyHelpers::ReadExtendedNumber<Ap::CharactersWithSpaces>(*m_package);
}
std::optional<Int32> DocumentProperties::GetLines() const
{
    return DocumentPropertyHelpers::ReadExtendedNumber<Ap::Lines>(*m_package);
}
std::optional<Int32> DocumentProperties::GetParagraphs() const
{
    return DocumentPropertyHelpers::ReadExtendedNumber<Ap::Paragraphs>(*m_package);
}
std::optional<Int32> DocumentProperties::GetSlides() const
{
    return DocumentPropertyHelpers::ReadExtendedNumber<Ap::Slides>(*m_package);
}
std::optional<Int32> DocumentProperties::GetNotes() const
{
    return DocumentPropertyHelpers::ReadExtendedNumber<Ap::Notes>(*m_package);
}
std::optional<Int32> DocumentProperties::GetHiddenSlides() const
{
    return DocumentPropertyHelpers::ReadExtendedNumber<Ap::HiddenSlides>(*m_package);
}
std::optional<Int32> DocumentProperties::GetTotalTime() const
{
    return DocumentPropertyHelpers::ReadExtendedNumber<Ap::TotalTime>(*m_package);
}

// --- custom properties ------------------------------------------------------

std::vector<std::string> DocumentProperties::GetCustomPropertyNames() const
{
    std::vector<std::string> names;
    auto root = DocumentPropertyHelpers::CustomRootOf(*m_package);
    if (!root)
    {
        return names;
    }
    for (const auto& property : root->Elements<Op::CustomDocumentProperty>())
    {
        names.push_back(property->GetName().ToString());
    }
    return names;
}

std::vector<DocumentCustomProperty> DocumentProperties::GetCustomProperties() const
{
    std::vector<DocumentCustomProperty> properties;
    auto root = DocumentPropertyHelpers::CustomRootOf(*m_package);
    if (!root)
    {
        return properties;
    }
    for (const auto& property : root->Elements<Op::CustomDocumentProperty>())
    {
        if (auto value = DocumentPropertyHelpers::ReadCustomPropertyValue(property))
        {
            properties.push_back(DocumentCustomProperty{property->GetName().ToString(), std::move(*value)});
        }
    }
    return properties;
}

std::optional<DocumentCustomPropertyValue> DocumentProperties::GetCustomProperty(std::string_view name) const
{
    auto root = DocumentPropertyHelpers::CustomRootOf(*m_package);
    return DocumentPropertyHelpers::ReadCustomPropertyValue(DocumentPropertyHelpers::FindCustomProperty(root, name));
}

bool DocumentProperties::SetCustomProperty(std::string_view name, const DocumentCustomPropertyValue& value)
{
    if (name.empty())
    {
        return false;
    }

    auto root = DocumentPropertyHelpers::CustomRoot(DocumentPropertyHelpers::EnsurePackagePart<CustomFilePropertiesPart>(*m_package));
    if (!root)
    {
        return false;
    }

    auto property = DocumentPropertyHelpers::FindCustomProperty(root, name);
    if (!property)
    {
        property = root->AppendChild<Op::CustomDocumentProperty>();
        if (!property)
        {
            property = root->AppendChildRaw<Op::CustomDocumentProperty>();
        }
        if (!property)
        {
            return false;
        }
        property->SetFormatId(StringValue(kUserDefinedFormatId));
        property->SetPropertyId(Int32Value(DocumentPropertyHelpers::NextCustomPropertyId(root)));
        property->SetName(StringValue(name));
    }
    return DocumentPropertyHelpers::WriteCustomPropertyValue(property, value);
}

bool DocumentProperties::RemoveCustomProperty(std::string_view name)
{
    auto root = DocumentPropertyHelpers::CustomRootOf(*m_package);
    auto property = DocumentPropertyHelpers::FindCustomProperty(root, name);
    return property && root->RemoveChild(property);
}

Size DocumentProperties::ClearCustomProperties()
{
    auto root = DocumentPropertyHelpers::CustomRootOf(*m_package);
    if (!root)
    {
        return 0;
    }

    Size removed = 0;
    for (const auto& property : root->Elements<Op::CustomDocumentProperty>())
    {
        if (root->RemoveChild(property))
        {
            ++removed;
        }
    }
    return removed;
}

// --- save-time maintenance --------------------------------------------------

bool DocumentProperties::UpdateSaveTimeProperties(std::string_view applicationName)
{
    auto corePart = DocumentPropertyHelpers::EnsurePackagePart<CoreFilePropertiesPart>(*m_package);
    auto extendedPart = DocumentPropertyHelpers::EnsurePackagePart<ExtendedFilePropertiesPart>(*m_package);
    if (!corePart || !extendedPart)
    {
        return false;
    }

    const auto now = FormatW3cDateTime(std::chrono::system_clock::now());
    if (DocumentPropertyHelpers::ReadCoreText(corePart, "dcterms:created").empty())
    {
        DocumentPropertyHelpers::WriteCoreText(corePart, "dcterms:created", now);
    }
    DocumentPropertyHelpers::WriteCoreText(corePart, "dcterms:modified", now);

    if (!applicationName.empty() && DocumentPropertyHelpers::ReadExtendedText<Ap::Application>(extendedPart).empty())
    {
        DocumentPropertyHelpers::WriteExtendedText<Ap::Application>(extendedPart, applicationName);
    }
    return true;
}

// --- date helpers -----------------------------------------------------------

std::string DocumentProperties::FormatW3cDateTime(std::chrono::system_clock::time_point value)
{
    // Core properties conventionally carry whole seconds; drop the fraction so
    // the shared xsd:dateTime formatter emits the canonical `...:SSZ` form.
    const auto seconds = std::chrono::floor<std::chrono::seconds>(value);
    return DateTimeValue(std::chrono::time_point_cast<std::chrono::system_clock::duration>(seconds))
        .ToString();
}

std::optional<std::chrono::system_clock::time_point> DocumentProperties::ParseW3cDateTime(
    std::string_view text)
{
    const auto trimmed = DocumentPropertyHelpers::TrimAscii(text);
    if (trimmed.empty())
    {
        return std::nullopt;
    }

    // Full timestamps use the shared xsd:dateTime parser (fractions, Z, offsets).
    const auto parsed = OpenXmlSimpleValueConvertor::GetDateTimeValueFromString(trimmed);
    if (parsed.IsDefined())
    {
        return parsed.Value();
    }

    // W3CDTF additionally allows reduced precision the xsd parser rejects:
    // YYYY, YYYY-MM, and YYYY-MM-DD. Normalize those to a full timestamp and
    // reuse the shared parser for digit and calendar validation.
    std::string normalized(trimmed);
    switch (trimmed.size())
    {
        case 4:
        {
            normalized += "-01-01T00:00:00Z";
            break;
        }
        case 7:
        {
            normalized += "-01T00:00:00Z";
            break;
        }
        case 10:
        {
            normalized += "T00:00:00Z";
            break;
        }
        default:
        {
            return std::nullopt;
        }
    }

    const auto reduced = OpenXmlSimpleValueConvertor::GetDateTimeValueFromString(normalized);
    if (!reduced.IsDefined())
    {
        return std::nullopt;
    }
    return reduced.Value();
}

} // namespace ExyokiOffice::Packaging
