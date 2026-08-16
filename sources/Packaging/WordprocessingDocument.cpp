// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "ExyokiOffice/Packaging/WordprocessingDocument.hpp"

#include "OpenErrorReporting.hpp"

#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/ExtendedProperties.hpp"
#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/Wordprocessing.hpp"
#include "ExyokiOffice/OpenXmlPackagePart.hpp"
#include "ExyokiOffice/MarkupCompatibility.hpp"
#include "ExyokiOffice/OpenXmlPackageValidator.hpp"
#include "ExyokiOffice/Packaging/DocumentProperties.hpp"
#include "ExyokiOffice/Packaging/PackageUtilities.hpp"
#include "pugixml/pugixml.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <iterator>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace ExyokiOffice::Packaging
{
/// File-local part lookup helpers for the Word package.
class WordprocessingDocumentHelper
{
public:
    static constexpr std::string_view kAttachedTemplateRelationship =
        "http://schemas.openxmlformats.org/officeDocument/2006/relationships/attachedTemplate";
    static bool IsCancellationRequested(const ICancellationToken* cancellationToken)
    {
        return cancellationToken != nullptr && cancellationToken->IsCancelled();
    }

    static ValidationResult CopyWithSeverity(const ValidationResult& source, ValidationSeverity severity)
    {
        ValidationResult result;
        for (auto issue : source.Issues())
        {
            issue.Severity = severity;
            result.AddIssue(std::move(issue));
        }
        return result;
    }

    static void ReportDiagnostics(const ValidationResult& result, DiagnosticSink* sink)
    {
        if (!sink)
        {
            return;
        }

        for (const auto& issue : result.Issues())
        {
            sink->Report(issue);
        }
    }

    static OpenXmlPackageLimits BuildPackageLimits(const OpenSettings& settings)
    {
        auto limits = settings.PackageLimits;
        if (limits.MaxPartBytes == 0 && settings.MaxCharactersInPart != 0)
        {
            limits.MaxPartBytes = settings.MaxCharactersInPart;
        }
        return limits;
    }
};

WordprocessingDocumentType WordDocument::GetDocumentType() const noexcept
{
    return m_documentType;
}

WordDocument::Ptr WordDocument::Create(WordprocessingDocumentType type)
{
    auto document = std::make_shared<WordDocument>();
    if (!document)
    {
        return nullptr;
    }
    document->m_documentType = type;
    return document;
}

std::shared_ptr<MainDocumentPart> WordDocument::AddMainDocumentPart(const std::shared_ptr<MainDocumentPart>& part)
{
    auto instance = WordprocessingDocument::AddMainDocumentPart(part);
    EnsureMainPartContentType();
    return instance;
}

bool WordDocument::InitDocument()
{
    auto mainPart = GetMainDocumentPart();
    if (!mainPart)
    {
        mainPart = AddMainDocumentPart();
        if (!mainPart)
        {
            return false;
        }
    }

    if (!GetCoreFilePropertiesPart())
    {
        if (!AddCoreFilePropertiesPart())
        {
            return false;
        }
    }

    if (!GetExtendedFilePropertiesPart())
    {
        if (!AddExtendedFilePropertiesPart())
        {
            return false;
        }
    }

    if (!EnsureDocumentSettingsPart())
    {
        return false;
    }

    return UpdateDocumentProperties();
}

bool WordDocument::UpdateDocumentProperties()
{
    return Properties().UpdateSaveTimeProperties("ExyokiOffice");
}

DocumentProperties WordDocument::Properties()
{
    return DocumentProperties(*this);
}

std::string WordDocument::GetTitle() const
{
    return DocumentProperties(const_cast<WordDocument&>(*this)).GetTitle();
}

void WordDocument::SetTitle(std::string_view title)
{
    Properties().SetTitle(title);
}

std::string WordDocument::GetCreator() const
{
    return DocumentProperties(const_cast<WordDocument&>(*this)).GetCreator();
}

void WordDocument::SetCreator(std::string_view creator)
{
    Properties().SetCreator(creator);
}

std::string WordDocument::GetLastModifiedBy() const
{
    return DocumentProperties(const_cast<WordDocument&>(*this)).GetLastModifiedBy();
}

void WordDocument::SetLastModifiedBy(std::string_view name)
{
    Properties().SetLastModifiedBy(name);
}

std::string WordDocument::GetSubject() const
{
    return DocumentProperties(const_cast<WordDocument&>(*this)).GetSubject();
}

void WordDocument::SetSubject(std::string_view subject)
{
    Properties().SetSubject(subject);
}

std::string WordDocument::GetKeywords() const
{
    return DocumentProperties(const_cast<WordDocument&>(*this)).GetKeywords();
}

void WordDocument::SetKeywords(std::string_view keywords)
{
    Properties().SetKeywords(keywords);
}

std::string WordDocument::GetDescription() const
{
    return DocumentProperties(const_cast<WordDocument&>(*this)).GetDescription();
}

void WordDocument::SetDescription(std::string_view description)
{
    Properties().SetDescription(description);
}

std::string WordDocument::GetCategory() const
{
    return DocumentProperties(const_cast<WordDocument&>(*this)).GetCategory();
}

void WordDocument::SetCategory(std::string_view category)
{
    Properties().SetCategory(category);
}

std::string WordDocument::GetContentStatus() const
{
    return DocumentProperties(const_cast<WordDocument&>(*this)).GetContentStatus();
}

void WordDocument::SetContentStatus(std::string_view contentStatus)
{
    Properties().SetContentStatus(contentStatus);
}

std::string WordDocument::GetCompany() const
{
    return DocumentProperties(const_cast<WordDocument&>(*this)).GetCompany();
}

void WordDocument::SetCompany(std::string_view company)
{
    Properties().SetCompany(company);
}

WordDocument::Ptr WordDocument::Open(const std::filesystem::path& path,
                                     const OpenSettings& settings,
                                     const ICancellationToken* cancellationToken,
                                     OpenError* error)
{
    using Detail::OpenErrorReporter;

    if (path.empty())
    {
        OpenErrorReporter::Report(error, OpenErrorCode::InvalidArgument, "No path was given to open.");
        return nullptr;
    }
    if (WordprocessingDocumentHelper::IsCancellationRequested(cancellationToken))
    {
        OpenErrorReporter::ReportCancelled(error);
        return nullptr;
    }
    auto document = std::make_shared<WordDocument>();
    document->SetPackageLimits(WordprocessingDocumentHelper::BuildPackageLimits(settings));
    document->SetPartByteRetention(settings.ByteRetention);
    if (!document->LoadFromFile(path, cancellationToken))
    {
        OpenErrorReporter::ReportFileLoadFailure(error, *document, path, cancellationToken);
        return nullptr;
    }
    return FinishOpen(std::move(document), settings, cancellationToken, error);
}

WordDocument::Ptr WordDocument::Open(std::iostream& stream,
                                     const OpenSettings& settings,
                                     const ICancellationToken* cancellationToken,
                                     OpenError* error)
{
    if (WordprocessingDocumentHelper::IsCancellationRequested(cancellationToken))
    {
        Detail::OpenErrorReporter::ReportCancelled(error);
        return nullptr;
    }
    const auto buffer = ReadStreamFully(stream);
    return Open(std::span<const Byte>(buffer.data(), buffer.size()), settings, cancellationToken, error);
}

WordDocument::Ptr WordDocument::Open(const std::vector<Byte>& packageBuffer,
                                     const OpenSettings& settings,
                                     const ICancellationToken* cancellationToken,
                                     OpenError* error)
{
    return Open(std::span<const Byte>(packageBuffer.data(), packageBuffer.size()),
                settings,
                cancellationToken,
                error);
}

WordDocument::Ptr WordDocument::Open(std::span<const Byte> packageBuffer,
                                     const OpenSettings& settings,
                                     const ICancellationToken* cancellationToken,
                                     OpenError* error)
{
    using Detail::OpenErrorReporter;

    if (packageBuffer.empty())
    {
        OpenErrorReporter::Report(error, OpenErrorCode::InvalidArgument, "The package buffer is empty.");
        return nullptr;
    }
    if (WordprocessingDocumentHelper::IsCancellationRequested(cancellationToken))
    {
        OpenErrorReporter::ReportCancelled(error);
        return nullptr;
    }
    auto document = std::make_shared<WordDocument>();
    document->SetPackageLimits(WordprocessingDocumentHelper::BuildPackageLimits(settings));
    document->SetPartByteRetention(settings.ByteRetention);
    if (!document->LoadFromMemory(packageBuffer, cancellationToken))
    {
        OpenErrorReporter::ReportLoadFailure(error, *document, {}, cancellationToken);
        return nullptr;
    }
    return FinishOpen(std::move(document), settings, cancellationToken, error);
}

WordDocument::Ptr WordDocument::FinishOpen(Ptr document,
                                           const OpenSettings& settings,
                                           const ICancellationToken* cancellationToken,
                                           OpenError* error)
{
    using Detail::OpenErrorReporter;

    document->ApplyOpenSettings(settings);
    if (WordprocessingDocumentHelper::IsCancellationRequested(cancellationToken))
    {
        OpenErrorReporter::ReportCancelled(error);
        return nullptr;
    }
    if (!document->ApplyOpcValidationPolicy(settings))
    {
        OpenErrorReporter::Report(error, OpenErrorCode::ValidationFailed,
                                  "The package failed strict OPC validation.", document.get());
        return nullptr;
    }
    if (WordprocessingDocumentHelper::IsCancellationRequested(cancellationToken))
    {
        OpenErrorReporter::ReportCancelled(error);
        return nullptr;
    }
    if (!document->ApplyMarkupCompatibilityPolicy(settings))
    {
        OpenErrorReporter::Report(error, OpenErrorCode::MarkupCompatibilityFailed,
                                  "Markup compatibility processing failed for a part of the package.",
                                  document.get());
        return nullptr;
    }
    document->UpdateDocumentTypeFromMainPart();
    if (!document->GetMainDocumentPart())
    {
        // The generic OPC loader reads any Open XML package, a .xlsx and a .pptx
        // included, so reaching this point says nothing about the family. Without
        // the main document part there is no document to edit, and returning an
        // editor over one would move the failure to the first call that used it.
        OpenErrorReporter::Report(error, OpenErrorCode::WrongDocumentType,
                                  "The package has no main document part, so it is not a Word document.",
                                  document.get());
        return nullptr;
    }
    if (WordprocessingDocumentHelper::IsCancellationRequested(cancellationToken))
    {
        OpenErrorReporter::ReportCancelled(error);
        return nullptr;
    }
    if (!document->EnforcePartCharacterBudget())
    {
        OpenErrorReporter::Report(error, OpenErrorCode::PartTooLarge,
                                  "A part holds more characters than OpenSettings::MaxCharactersInPart allows.",
                                  document.get());
        return nullptr;
    }
    return document;
}

WordDocument::Ptr WordDocument::CreateFromTemplate(const std::filesystem::path& templatePath, bool attachTemplate)
{
    if (templatePath.empty())
    {
        return nullptr;
    }

    auto buffer = ReadFileFully(templatePath);
    if (buffer.empty())
    {
        return nullptr;
    }

    auto document = std::make_shared<WordDocument>();
    if (!document || !document->LoadFromMemory(buffer))
    {
        return nullptr;
    }
    document->UpdateDocumentTypeFromMainPart();
    document->ChangeDocumentType(WordprocessingDocumentType::Document);
    if (attachTemplate)
    {
        document->AttachTemplateRelationship(templatePath);
    }
    return document;
}

void WordDocument::ChangeDocumentType(WordprocessingDocumentType newType)
{
    if (m_documentType == newType)
    {
        return;
    }

    m_documentType = newType;
    EnsureMainPartContentType();
}

bool WordDocument::BeforeSave()
{
    // A signed package must be written exactly as it was digested, so the save
    // that follows signing leaves the properties alone. See
    // OpenXmlPackage::SuppressSaveTimePropertyUpdateOnce.
    if (ConsumeSaveTimePropertyUpdateSuppression())
    {
        return true;
    }
    return UpdateDocumentProperties();
}

std::string_view WordDocument::MimeForDocumentType(WordprocessingDocumentType type)
{
    switch (type)
    {
        case WordprocessingDocumentType::Document:
            return "application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml";
        case WordprocessingDocumentType::Template:
            return "application/vnd.openxmlformats-officedocument.wordprocessingml.template.main+xml";
        case WordprocessingDocumentType::MacroEnabledDocument:
            return "application/vnd.ms-word.document.macroEnabled.main+xml";
        case WordprocessingDocumentType::MacroEnabledTemplate:
            return "application/vnd.ms-word.template.macroEnabledTemplate.main+xml";
    }
    return {};
}

std::optional<WordprocessingDocumentType> WordDocument::DocumentTypeFromMime(std::string_view contentType)
{
    if (contentType == "application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml")
    {
        return WordprocessingDocumentType::Document;
    }
    if (contentType == "application/vnd.openxmlformats-officedocument.wordprocessingml.template.main+xml")
    {
        return WordprocessingDocumentType::Template;
    }
    if (contentType == "application/vnd.ms-word.document.macroEnabled.main+xml")
    {
        return WordprocessingDocumentType::MacroEnabledDocument;
    }
    if (contentType == "application/vnd.ms-word.template.macroEnabledTemplate.main+xml")
    {
        return WordprocessingDocumentType::MacroEnabledTemplate;
    }
    return std::nullopt;
}

bool WordDocument::ApplyMarkupCompatibilityPolicy(const OpenSettings& settings)
{
    return ProcessMarkupCompatibility(*this, GetMainDocumentPart(), settings.MarkupCompatibility,
                                      settings.ValidationDiagnostics);
}

void WordDocument::ApplyOpenSettings(const OpenSettings& settings)
{
    m_openSettings = settings;
    SetExternalResourceResolver(settings.ExternalResources);
    SetExternalResourcePolicy(settings.ExternalResourcePolicy);
}

bool WordDocument::ApplyOpcValidationPolicy(const OpenSettings& settings)
{
    ClearValidationResult();

    if (settings.OpcValidation == OpcValidationMode::None)
    {
        return true;
    }

    const auto validation = OpenXmlPackageValidator().Validate(*this);
    if (settings.OpcValidation == OpcValidationMode::Tolerant)
    {
        auto warnings = WordprocessingDocumentHelper::CopyWithSeverity(validation, ValidationSeverity::Warning);
        WordprocessingDocumentHelper::ReportDiagnostics(warnings, settings.ValidationDiagnostics);
        SetLastValidationResult(std::move(warnings));
        return true;
    }

    WordprocessingDocumentHelper::ReportDiagnostics(validation, settings.ValidationDiagnostics);
    SetLastValidationResult(validation);
    return !validation.HasErrors();
}

void WordDocument::UpdateDocumentTypeFromMainPart()
{
    auto mainPart = GetMainDocumentPart();
    if (!mainPart)
    {
        return;
    }
    const auto mapped = DocumentTypeFromMime(mainPart->ContentType());
    if (mapped)
    {
        m_documentType = *mapped;
    }
}

void WordDocument::EnsureMainPartContentType()
{
    auto mainPart = GetMainDocumentPart();
    if (!mainPart)
    {
        return;
    }
    const auto desired = std::string(MimeForDocumentType(m_documentType));
    if (mainPart->ContentType() != desired)
    {
        mainPart->SetContentType(desired);
    }
}

bool WordDocument::EnforcePartCharacterBudget() const
{
    if (m_openSettings.MaxCharactersInPart == 0)
    {
        return true;
    }

    const auto budget = m_openSettings.MaxCharactersInPart;
    bool withinBudget = true;
    ForEachPart([&](const std::shared_ptr<OpenXmlPackagePart>& part)
                {
        if (!withinBudget || !part || !part->IsXmlPart())
        {
            return;
        }
        const auto xml = part->GetXmlString();
        if (xml.size() > budget)
        {
            withinBudget = false;
        } });
    return withinBudget;
}

void WordDocument::AttachTemplateRelationship(const std::filesystem::path& templatePath)
{
    auto settingsPart = EnsureDocumentSettingsPart();
    if (!settingsPart)
    {
        return;
    }

    const auto relId =
        settingsPart->AddExternalRelationship(WordprocessingDocumentHelper::kAttachedTemplateRelationship, BuildRelationshipTarget(templatePath));
    if (relId.empty())
    {
        return;
    }

    auto settings = settingsPart->GetTypedRootElement();
    auto attachedTemplate =
        settings ? settings->GetFirstChildOfType<DocumentFormat::OpenXml::Wordprocessing::AttachedTemplate>() : nullptr;
    if (!attachedTemplate && settings)
    {
        attachedTemplate = settings->AppendChild<DocumentFormat::OpenXml::Wordprocessing::AttachedTemplate>();
    }
    if (attachedTemplate)
    {
        attachedTemplate->SetId(StringValue(relId));
    }
}

std::optional<Security::ExternalReference> WordDocument::GetAttachedTemplateReference() const
{
    for (const auto& reference : Security::CollectExternalReferences(*this))
    {
        if (reference.RelationshipType == WordprocessingDocumentHelper::kAttachedTemplateRelationship)
        {
            return reference;
        }
    }
    return std::nullopt;
}

Security::ExternalResourceResponse WordDocument::ResolveAttachedTemplate(const ICancellationToken* cancellationToken)
{
    const auto reference = GetAttachedTemplateReference();
    if (!reference.has_value())
    {
        Security::ExternalResourceResponse response;
        response.Status = Security::ExternalResourceStatus::NotFound;
        response.Message = "The document is not attached to a template.";
        return response;
    }

    return Security::ResolveExternalResource(*this, *reference, cancellationToken);
}

std::shared_ptr<DocumentSettingsPart> WordDocument::EnsureDocumentSettingsPart()
{
    auto mainPart = GetMainDocumentPart();
    if (!mainPart)
    {
        return nullptr;
    }
    auto settingsPart = mainPart->GetDocumentSettingsPart();
    if (!settingsPart)
    {
        settingsPart = mainPart->AddDocumentSettingsPart();
    }
    return settingsPart;
}

} // namespace ExyokiOffice::Packaging
