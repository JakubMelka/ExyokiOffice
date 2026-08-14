// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "ExyokiOffice/Packaging/PowerPointDocument.hpp"

#include "ExyokiOffice/MarkupCompatibility.hpp"
#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/ExtendedProperties.hpp"
#include "ExyokiOffice/Packaging/PackageUtilities.hpp"
#include "pugixml/pugixml.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace ExyokiOffice::Packaging
{
namespace ExtendedProperties = ExyokiOffice::DocumentFormat::OpenXml::ExtendedProperties;

/// File-local part lookup helpers for the PowerPoint package.
class PowerPointDocumentHelper
{
public:
    static bool Cancelled(const ICancellationToken* token)
    {
        return token && token->IsCancelled();
    }

    static OpenXmlPackageLimits LimitsFor(const OpenSettings& settings)
    {
        auto limits = settings.PackageLimits;
        if (limits.MaxPartBytes == 0 && settings.MaxCharactersInPart != 0)
        {
            limits.MaxPartBytes = settings.MaxCharactersInPart;
        }
        return limits;
    }
};

class PowerPointPropertyXmlHelpers
{
public:
    /// Appends the element with the supplied default text only when it does not
    /// exist yet, so values written by users or other producers are preserved.
    template <typename T>
    static bool EnsureText(const std::shared_ptr<OpenXMLElement>& root, std::string_view value)
    {
        if (!root)
        {
            return false;
        }
        if (root->GetFirstChildOfType<T>())
        {
            return true;
        }
        auto element = root->AppendChild<T>();
        if (!element)
        {
            return false;
        }
        element->SetText(std::string(value));
        return true;
    }
};

PowerPointDocumentType PowerPointDocument::GetDocumentType() const noexcept
{
    return m_documentType;
}

PowerPointDocument::Ptr PowerPointDocument::Create(PowerPointDocumentType type)
{
    auto document = std::make_shared<PowerPointDocument>();
    document->m_documentType = type;
    return document;
}

std::shared_ptr<PresentationPart> PowerPointDocument::AddPresentationPart(const std::shared_ptr<PresentationPart>& part)
{
    auto result = PresentationDocument::AddPresentationPart(part);
    EnsurePresentationPartContentType();
    return result;
}

bool PowerPointDocument::InitDocument()
{
    if (!GetPresentationPart() && !AddPresentationPart())
    {
        return false;
    }
    if (!GetCoreFilePropertiesPart() && !AddCoreFilePropertiesPart())
    {
        return false;
    }
    if (!GetExtendedFilePropertiesPart() && !AddExtendedFilePropertiesPart())
    {
        return false;
    }
    return UpdateDocumentProperties();
}

bool PowerPointDocument::UpdateDocumentProperties()
{
    if (!Properties().UpdateSaveTimeProperties("ExyokiOffice"))
    {
        return false;
    }

    auto extended = GetExtendedFilePropertiesPart();
    if (!extended)
    {
        return false;
    }

    // PresentationML consumers expect these bookkeeping elements; provide
    // defaults when missing but never overwrite existing values.
    auto properties = extended->GetTypedRootElement();
    return PowerPointPropertyXmlHelpers::EnsureText<ExtendedProperties::PresentationFormat>(properties, "Custom") &&
           PowerPointPropertyXmlHelpers::EnsureText<ExtendedProperties::Slides>(properties, "0") &&
           PowerPointPropertyXmlHelpers::EnsureText<ExtendedProperties::Notes>(properties, "0") &&
           PowerPointPropertyXmlHelpers::EnsureText<ExtendedProperties::HiddenSlides>(properties, "0") &&
           PowerPointPropertyXmlHelpers::EnsureText<ExtendedProperties::MultimediaClips>(properties, "0") &&
           PowerPointPropertyXmlHelpers::EnsureText<ExtendedProperties::ScaleCrop>(properties, "false") &&
           PowerPointPropertyXmlHelpers::EnsureText<ExtendedProperties::LinksUpToDate>(properties, "false") &&
           PowerPointPropertyXmlHelpers::EnsureText<ExtendedProperties::SharedDocument>(properties, "false") &&
           PowerPointPropertyXmlHelpers::EnsureText<ExtendedProperties::HyperlinksChanged>(properties, "false") &&
           PowerPointPropertyXmlHelpers::EnsureText<ExtendedProperties::ApplicationVersion>(properties, "1.0");
}

void PowerPointDocument::ApplyOpenSettings(const OpenSettings& settings)
{
    m_openSettings = settings;
    SetExternalResourceResolver(settings.ExternalResources);
    SetExternalResourcePolicy(settings.ExternalResourcePolicy);
}

bool PowerPointDocument::ApplyMarkupCompatibilityPolicy(const OpenSettings& settings)
{
    return ProcessMarkupCompatibility(*this, GetPresentationPart(), settings.MarkupCompatibility,
                                      settings.ValidationDiagnostics);
}

DocumentProperties PowerPointDocument::Properties()
{
    return DocumentProperties(*this);
}

PowerPointDocument::Ptr PowerPointDocument::Open(const std::filesystem::path& path, const OpenSettings& settings,
                                                 const ICancellationToken* token)
{
    if (path.empty() || PowerPointDocumentHelper::Cancelled(token))
    {
        return nullptr;
    }
    auto document = std::make_shared<PowerPointDocument>();
    document->SetPackageLimits(PowerPointDocumentHelper::LimitsFor(settings));
    document->SetPartByteRetention(settings.ByteRetention);
    if (!document->LoadFromFile(path, token) || PowerPointDocumentHelper::Cancelled(token))
    {
        return nullptr;
    }
    document->ApplyOpenSettings(settings);
    if (!document->ApplyMarkupCompatibilityPolicy(settings))
    {
        return nullptr;
    }
    document->UpdateDocumentTypeFromPresentationPart();
    return document->GetPresentationPart() ? document : nullptr;
}

PowerPointDocument::Ptr PowerPointDocument::Open(std::iostream& stream, const OpenSettings& settings,
                                                 const ICancellationToken* token)
{
    const auto bytes = ReadStreamFully(stream);
    return Open(std::span<const Byte>(bytes.data(), bytes.size()), settings, token);
}

PowerPointDocument::Ptr PowerPointDocument::Open(const std::vector<Byte>& bytes, const OpenSettings& settings,
                                                 const ICancellationToken* token)
{
    return Open(std::span<const Byte>(bytes.data(), bytes.size()), settings, token);
}

PowerPointDocument::Ptr PowerPointDocument::Open(std::span<const Byte> bytes, const OpenSettings& settings,
                                                 const ICancellationToken* token)
{
    if (bytes.empty() || PowerPointDocumentHelper::Cancelled(token))
    {
        return nullptr;
    }
    auto document = std::make_shared<PowerPointDocument>();
    document->SetPackageLimits(PowerPointDocumentHelper::LimitsFor(settings));
    document->SetPartByteRetention(settings.ByteRetention);
    if (!document->LoadFromMemory(bytes, token) || PowerPointDocumentHelper::Cancelled(token))
    {
        return nullptr;
    }
    document->ApplyOpenSettings(settings);
    if (!document->ApplyMarkupCompatibilityPolicy(settings))
    {
        return nullptr;
    }
    document->UpdateDocumentTypeFromPresentationPart();
    return document->GetPresentationPart() ? document : nullptr;
}

void PowerPointDocument::ChangeDocumentType(PowerPointDocumentType type)
{
    m_documentType = type;
    EnsurePresentationPartContentType();
}

bool PowerPointDocument::BeforeSave()
{
    if (!GetPresentationPart())
    {
        return false;
    }
    // A signed package must be written exactly as it was digested, so the save
    // that follows signing leaves the properties alone. See
    // OpenXmlPackage::SuppressSaveTimePropertyUpdateOnce.
    if (ConsumeSaveTimePropertyUpdateSuppression())
    {
        return true;
    }
    return UpdateDocumentProperties();
}

std::string_view PowerPointDocument::MimeForDocumentType(PowerPointDocumentType type)
{
    switch (type)
    {
        case PowerPointDocumentType::Presentation:
            return "application/vnd.openxmlformats-officedocument.presentationml.presentation.main+xml";
        case PowerPointDocumentType::MacroEnabledPresentation:
            return "application/vnd.ms-powerpoint.presentation.macroEnabled.main+xml";
        case PowerPointDocumentType::Template:
            return "application/vnd.openxmlformats-officedocument.presentationml.template.main+xml";
        case PowerPointDocumentType::MacroEnabledTemplate:
            return "application/vnd.ms-powerpoint.template.macroEnabled.main+xml";
        case PowerPointDocumentType::SlideShow:
            return "application/vnd.openxmlformats-officedocument.presentationml.slideshow.main+xml";
        case PowerPointDocumentType::MacroEnabledSlideShow:
            return "application/vnd.ms-powerpoint.slideshow.macroEnabled.main+xml";
    }
    return {};
}

std::optional<PowerPointDocumentType> PowerPointDocument::DocumentTypeFromMime(std::string_view mime)
{
    for (auto type : {PowerPointDocumentType::Presentation, PowerPointDocumentType::MacroEnabledPresentation,
                      PowerPointDocumentType::Template, PowerPointDocumentType::MacroEnabledTemplate,
                      PowerPointDocumentType::SlideShow, PowerPointDocumentType::MacroEnabledSlideShow})
    {
        if (MimeForDocumentType(type) == mime)
        {
            return type;
        }
    }
    return std::nullopt;
}

void PowerPointDocument::UpdateDocumentTypeFromPresentationPart()
{
    if (auto part = GetPresentationPart())
    {
        if (auto type = DocumentTypeFromMime(part->ContentType()))
        {
            m_documentType = *type;
        }
    }
}

void PowerPointDocument::EnsurePresentationPartContentType()
{
    if (auto part = GetPresentationPart())
    {
        part->SetContentType(std::string(MimeForDocumentType(m_documentType)));
    }
}

} // namespace ExyokiOffice::Packaging
