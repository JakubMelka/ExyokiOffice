// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "ExyokiOffice/Packaging/SpreadsheetDocument.hpp"

#include "OpenErrorReporting.hpp"

#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/ExtendedProperties.hpp"
#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/Spreadsheet.hpp"
#include "ExyokiOffice/MarkupCompatibility.hpp"
#include "ExyokiOffice/OpenXmlPackageValidator.hpp"
#include "ExyokiOffice/Packaging/PackageUtilities.hpp"
#include "pugixml/pugixml.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <utility>

namespace ExyokiOffice::Packaging
{
/// File-local part lookup helpers for the Excel package.
class SpreadsheetDocumentHelper
{
public:
    static bool IsCancellationRequested(const ICancellationToken* cancellationToken)
    {
        return cancellationToken != nullptr && cancellationToken->IsCancelled();
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
};

SpreadsheetDocumentType ExcelDocument::GetDocumentType() const noexcept
{
    return m_documentType;
}

ExcelDocument::Ptr ExcelDocument::Create(SpreadsheetDocumentType type)
{
    auto document = std::make_shared<ExcelDocument>();
    if (!document)
    {
        return nullptr;
    }
    document->m_documentType = type;
    return document;
}

std::shared_ptr<WorkbookPart> ExcelDocument::AddWorkbookPart(const std::shared_ptr<WorkbookPart>& part)
{
    auto instance = SpreadsheetDocument::AddWorkbookPart(part);
    EnsureWorkbookPartContentType();
    return instance;
}

bool ExcelDocument::InitDocument()
{
    auto workbookPart = GetWorkbookPart();
    if (!workbookPart)
    {
        workbookPart = AddWorkbookPart();
        if (!workbookPart)
        {
            return false;
        }
    }

    auto workbook = workbookPart->GetTypedRootElement();
    if (!workbook)
    {
        return false;
    }
    if (!workbook->GetFirstChildOfType<DocumentFormat::OpenXml::Spreadsheet::Sheets>())
    {
        workbook->AppendChild<DocumentFormat::OpenXml::Spreadsheet::Sheets>();
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

bool ExcelDocument::UpdateDocumentProperties()
{
    return Properties().UpdateSaveTimeProperties("ExyokiOffice");
}

DocumentProperties ExcelDocument::Properties()
{
    return DocumentProperties(*this);
}

ExcelDocument::Ptr ExcelDocument::Open(const std::filesystem::path& path,
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
    if (SpreadsheetDocumentHelper::IsCancellationRequested(cancellationToken))
    {
        OpenErrorReporter::ReportCancelled(error);
        return nullptr;
    }
    auto document = std::make_shared<ExcelDocument>();
    document->SetPackageLimits(SpreadsheetDocumentHelper::BuildPackageLimits(settings));
    document->SetPartByteRetention(settings.ByteRetention);
    if (!document->LoadFromFile(path, cancellationToken))
    {
        OpenErrorReporter::ReportFileLoadFailure(error, *document, path, cancellationToken);
        return nullptr;
    }
    return FinishOpen(std::move(document), settings, cancellationToken, error);
}

ExcelDocument::Ptr ExcelDocument::Open(std::iostream& stream,
                                       const OpenSettings& settings,
                                       const ICancellationToken* cancellationToken,
                                       OpenError* error)
{
    if (SpreadsheetDocumentHelper::IsCancellationRequested(cancellationToken))
    {
        Detail::OpenErrorReporter::ReportCancelled(error);
        return nullptr;
    }
    const auto buffer = ReadStreamFully(stream);
    return Open(std::span<const Byte>(buffer.data(), buffer.size()), settings, cancellationToken, error);
}

ExcelDocument::Ptr ExcelDocument::Open(const std::vector<Byte>& packageBuffer,
                                       const OpenSettings& settings,
                                       const ICancellationToken* cancellationToken,
                                       OpenError* error)
{
    return Open(std::span<const Byte>(packageBuffer.data(), packageBuffer.size()), settings, cancellationToken, error);
}

ExcelDocument::Ptr ExcelDocument::Open(std::span<const Byte> packageBuffer,
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
    if (SpreadsheetDocumentHelper::IsCancellationRequested(cancellationToken))
    {
        OpenErrorReporter::ReportCancelled(error);
        return nullptr;
    }
    auto document = std::make_shared<ExcelDocument>();
    document->SetPackageLimits(SpreadsheetDocumentHelper::BuildPackageLimits(settings));
    document->SetPartByteRetention(settings.ByteRetention);
    if (!document->LoadFromMemory(packageBuffer, cancellationToken))
    {
        OpenErrorReporter::ReportLoadFailure(error, *document, {}, cancellationToken);
        return nullptr;
    }
    return FinishOpen(std::move(document), settings, cancellationToken, error);
}

ExcelDocument::Ptr ExcelDocument::FinishOpen(Ptr document,
                                             const OpenSettings& settings,
                                             const ICancellationToken* cancellationToken,
                                             OpenError* error)
{
    using Detail::OpenErrorReporter;

    document->ApplyOpenSettings(settings);
    if (SpreadsheetDocumentHelper::IsCancellationRequested(cancellationToken))
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
    if (SpreadsheetDocumentHelper::IsCancellationRequested(cancellationToken))
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
    document->UpdateDocumentTypeFromWorkbookPart();
    if (!document->GetWorkbookPart())
    {
        // The generic OPC loader reads any Open XML package, a .docx and a .pptx
        // included, so reaching this point says nothing about the family. Without
        // the workbook part there is no workbook to edit, and returning an editor
        // over one would move the failure to the first call that used it.
        OpenErrorReporter::Report(error, OpenErrorCode::WrongDocumentType,
                                  "The package has no workbook part, so it is not an Excel document.",
                                  document.get());
        return nullptr;
    }
    if (SpreadsheetDocumentHelper::IsCancellationRequested(cancellationToken))
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

void ExcelDocument::ChangeDocumentType(SpreadsheetDocumentType newType)
{
    m_documentType = newType;
    EnsureWorkbookPartContentType();
}

bool ExcelDocument::HasVbaProject() const noexcept
{
    const auto workbookPart = GetWorkbookPart();
    return workbookPart && workbookPart->GetVbaProjectPart();
}

std::vector<Byte> ExcelDocument::GetVbaProjectData() const
{
    const auto workbookPart = GetWorkbookPart();
    const auto vbaProjectPart = workbookPart ? workbookPart->GetVbaProjectPart() : nullptr;
    return vbaProjectPart ? vbaProjectPart->GetBinaryData() : std::vector<Byte>{};
}

bool ExcelDocument::SetVbaProjectData(std::span<const Byte> data)
{
    if (data.empty())
    {
        return false;
    }

    auto workbookPart = GetWorkbookPart();
    if (!workbookPart)
    {
        if (!InitDocument())
        {
            return false;
        }
        workbookPart = GetWorkbookPart();
    }

    auto vbaProjectPart = workbookPart ? workbookPart->GetVbaProjectPart() : nullptr;
    if (!vbaProjectPart)
    {
        vbaProjectPart = workbookPart->AddVbaProjectPart();
    }
    if (!vbaProjectPart)
    {
        return false;
    }

    vbaProjectPart->SetBinaryData(std::vector<Byte>(data.begin(), data.end()));
    switch (m_documentType)
    {
        case SpreadsheetDocumentType::Workbook:
            ChangeDocumentType(SpreadsheetDocumentType::MacroEnabledWorkbook);
            break;
        case SpreadsheetDocumentType::Template:
            ChangeDocumentType(SpreadsheetDocumentType::MacroEnabledTemplate);
            break;
        case SpreadsheetDocumentType::MacroEnabledWorkbook:
        case SpreadsheetDocumentType::MacroEnabledTemplate:
            break;
    }
    return true;
}

bool ExcelDocument::RemoveVbaProject()
{
    auto workbookPart = GetWorkbookPart();
    if (!workbookPart || !workbookPart->RemoveVbaProjectPart())
    {
        return false;
    }

    switch (m_documentType)
    {
        case SpreadsheetDocumentType::MacroEnabledWorkbook:
            ChangeDocumentType(SpreadsheetDocumentType::Workbook);
            break;
        case SpreadsheetDocumentType::MacroEnabledTemplate:
            ChangeDocumentType(SpreadsheetDocumentType::Template);
            break;
        case SpreadsheetDocumentType::Workbook:
        case SpreadsheetDocumentType::Template:
            break;
    }
    return true;
}

bool ExcelDocument::BeforeSave()
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

std::string_view ExcelDocument::MimeForDocumentType(SpreadsheetDocumentType type)
{
    switch (type)
    {
        case SpreadsheetDocumentType::Workbook:
            return "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml";
        case SpreadsheetDocumentType::Template:
            return "application/vnd.openxmlformats-officedocument.spreadsheetml.template.main+xml";
        case SpreadsheetDocumentType::MacroEnabledWorkbook:
            return "application/vnd.ms-excel.sheet.macroEnabled.main+xml";
        case SpreadsheetDocumentType::MacroEnabledTemplate:
            return "application/vnd.ms-excel.template.macroEnabled.main+xml";
    }
    return {};
}

std::optional<SpreadsheetDocumentType> ExcelDocument::DocumentTypeFromMime(std::string_view contentType)
{
    if (contentType == "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml")
    {
        return SpreadsheetDocumentType::Workbook;
    }
    if (contentType == "application/vnd.openxmlformats-officedocument.spreadsheetml.template.main+xml")
    {
        return SpreadsheetDocumentType::Template;
    }
    if (contentType == "application/vnd.ms-excel.sheet.macroEnabled.main+xml")
    {
        return SpreadsheetDocumentType::MacroEnabledWorkbook;
    }
    if (contentType == "application/vnd.ms-excel.template.macroEnabled.main+xml")
    {
        return SpreadsheetDocumentType::MacroEnabledTemplate;
    }
    return std::nullopt;
}

bool ExcelDocument::ApplyMarkupCompatibilityPolicy(const OpenSettings& settings)
{
    return ProcessMarkupCompatibility(*this, GetWorkbookPart(), settings.MarkupCompatibility,
                                      settings.ValidationDiagnostics);
}

void ExcelDocument::ApplyOpenSettings(const OpenSettings& settings)
{
    m_openSettings = settings;
    SetExternalResourceResolver(settings.ExternalResources);
    SetExternalResourcePolicy(settings.ExternalResourcePolicy);
}

bool ExcelDocument::ApplyOpcValidationPolicy(const OpenSettings& settings)
{
    ClearValidationResult();
    if (settings.OpcValidation == OpcValidationMode::None)
    {
        return true;
    }

    const auto validation = OpenXmlPackageValidator().Validate(*this);
    if (settings.OpcValidation == OpcValidationMode::Tolerant)
    {
        auto warnings = SpreadsheetDocumentHelper::CopyWithSeverity(validation, ValidationSeverity::Warning);
        SpreadsheetDocumentHelper::ReportDiagnostics(warnings, settings.ValidationDiagnostics);
        SetLastValidationResult(std::move(warnings));
        return true;
    }

    SpreadsheetDocumentHelper::ReportDiagnostics(validation, settings.ValidationDiagnostics);
    SetLastValidationResult(validation);
    return !validation.HasErrors();
}

void ExcelDocument::UpdateDocumentTypeFromWorkbookPart()
{
    auto workbookPart = GetWorkbookPart();
    if (!workbookPart)
    {
        return;
    }
    if (auto mapped = DocumentTypeFromMime(workbookPart->ContentType()))
    {
        m_documentType = *mapped;
    }
}

void ExcelDocument::EnsureWorkbookPartContentType()
{
    auto workbookPart = GetWorkbookPart();
    if (!workbookPart)
    {
        return;
    }
    const auto desired = std::string(MimeForDocumentType(m_documentType));
    if (workbookPart->ContentType() != desired)
    {
        workbookPart->SetContentType(desired);
    }
}

bool ExcelDocument::EnforcePartCharacterBudget() const
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
        if (part->GetXmlString().size() > budget)
        {
            withinBudget = false;
        } });
    return withinBudget;
}

} // namespace ExyokiOffice::Packaging
