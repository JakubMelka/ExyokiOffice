// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

#include "ExyokiOffice/Export.hpp"
#include "ExyokiOffice/FileFormatVersions.h"
#include "ExyokiOffice/MarkupCompatibility.hpp"
#include "ExyokiOffice/Packaging/DocumentProperties.hpp"
#include "ExyokiOffice/Packaging/GeneratedParts.hpp"
#include "ExyokiOffice/Security/ExternalResources.hpp"
#include "ExyokiOffice/ValidationResult.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ExyokiOffice::Packaging
{

/**
 * @brief Enumerates the high-level shapes a Wordprocessing document can take.
 *
 * The document type drives the MIME content type of the main document part.
 * Office uses that content type to decide how to load the package.
 * The type does not change the XML schema, only the declared flavor.
 * Use these values to indicate document vs template and macro capability.
 */
enum class WordprocessingDocumentType
{
    /**
     * @brief Standard .docx document that does not contain macros.
     *
     * The package stores WordprocessingML markup only.
     * Use this for most ordinary documents without VBA code.
     * The main document part uses the regular .docx content type.
     * This is the default document flavor for new documents.
     */
    Document,
    /**
     * @brief Template stored as .dotx that provides reusable styles and content.
     *
     * Templates act as blueprints for new documents.
     * They typically include styles, headers, and placeholder text.
     * The main part uses the template-specific content type.
     * Use this when you want Word to treat the file as a template.
     */
    Template,
    /**
     * @brief Macro-enabled .docm document.
     *
     * This type indicates the package may include VBA macros.
     * Office treats these documents as potentially executable.
     * The main part uses the macro-enabled content type.
     * Choose this only when macros are expected or required.
     */
    MacroEnabledDocument,
    /**
     * @brief Macro-enabled template (.dotm).
     *
     * Templates of this type can carry VBA macros.
     * Use it when new documents should inherit macro logic.
     * The main part uses the macro-enabled template content type.
     * This is the template equivalent of MacroEnabledDocument.
     */
    MacroEnabledTemplate
};

enum class OpcValidationMode
{
    None,
    Tolerant,
    Strict
};

/**
 * @brief Markup compatibility processing options, shared by all three formats.
 *
 * Declared by the DOM layer, because markup compatibility is a property of Open XML
 * markup rather than of any one document family. The aliases keep the names usable
 * where they have always been spelled, next to OpenSettings.
 */
using ExyokiOffice::MarkupCompatibilityProcessMode;
using ExyokiOffice::MarkupCompatibilityProcessSettings;

/**
 * @brief Represents the knobs that influence how a Word document is opened.
 *
 * MarkupCompatibility aggregates settings for the mc: vocabulary.
 * MaxCharactersInPart limits how much XML is buffered for any part.
 * PackageLimits are enforced by the OPC loader before large entries are read.
 * Use these settings to tune safety and compatibility for your workloads.
 */
struct EXYOKIOFFICE_EXPORT OpenSettings
{
    /**
     * @brief Markup compatibility behavior.
     *
     * This bundles the mc: processing mode and target versions.
     * It determines how conditional markup is resolved.
     * The default setting preserves the raw markup.
     * Use it when documents rely on mc:Choice/mc:Fallback branches.
     */
    MarkupCompatibilityProcessSettings MarkupCompatibility;
    /**
     * @brief Maximum number of characters allowed in a single XML part (0 = unlimited).
     *
     * This is a safety limit against extremely large XML parts.
     * Set to 0 to disable the limit entirely.
     * If exceeded, Open returns nullptr.
     * The limit is checked after the package is loaded into memory.
     */
    UInt64 MaxCharactersInPart = 0;
    /**
     * @brief OPC ZIP/XML limits enforced while the package is being loaded.
     *
     * Zero values disable individual limits. When MaxCharactersInPart is set
     * and PackageLimits.MaxPartBytes is zero, the loader also uses
     * MaxCharactersInPart as a pre-read XML/package part byte limit.
     *
     * The initial value is the process-wide policy installed with
     * OpenXmlPackage::SetDefaultPackageLimits, read when the settings object is
     * constructed, and OpenXmlPackageLimits::Recommended() when none was
     * installed. It is read here rather than left all zeros because otherwise a
     * default-constructed OpenSettings would silently widen the policy back to
     * "no limits" — which is what every caller that opens a document without
     * passing settings would then get. Assigning the member still wins,
     * including an explicit OpenXmlPackageLimits::Unlimited().
     */
    OpenXmlPackageLimits PackageLimits = OpenXmlPackage::DefaultPackageLimits();
    /**
     * @brief Controls OPC relationship graph validation during Open.
     *
     * None preserves the historical behavior. Tolerant stores diagnostics as
     * warnings and still returns the document. Strict fails Open when OPC
     * validation reports errors.
     */
    OpcValidationMode OpcValidation = OpcValidationMode::None;
    /**
     * @brief Optional sink that receives OPC validation diagnostics during Open.
     *
     * In tolerant mode diagnostics are reported as warnings. In strict mode
     * original error severities are reported before Open returns nullptr.
     */
    DiagnosticSink* ValidationDiagnostics = nullptr;
    /**
     * @brief Controls whether the bytes read for each XML part are kept.
     *
     * The default keeps them only for packages that carry digital signatures,
     * because verifying a signature needs the part exactly as it was stored.
     * Set Never to save memory when signatures do not matter, or Always when
     * you want byte-level access to every loaded part.
     */
    PartByteRetention ByteRetention = PartByteRetention::WhenSignaturesPresent;
    /**
     * @brief Optional access to resources the document links to from outside itself.
     *
     * Opening never uses it. It is installed on the package so that later,
     * explicit calls such as Security::ResolveExternalResource have something to
     * ask. Leaving it null keeps every external target unreachable, which is the
     * default.
     */
    Security::IExternalResourceResolver::Ptr ExternalResources;
    /**
     * @brief Which external targets the resolver may be asked about, and under which limits.
     *
     * The default denies everything, so a resolver alone changes nothing until
     * this is widened as well.
     */
    Security::ExternalResourcePolicy ExternalResourcePolicy;
};

/**
 * @brief Why an Open() call returned nullptr.
 *
 * The order is stable and new members are appended, because the value crosses
 * the ABI boundary.
 */
enum class OpenErrorCode
{
    /** @brief Nothing failed; the document was opened. */
    None,
    /** @brief The path was empty, or the buffer contained no bytes. */
    InvalidArgument,
    /** @brief The cancellation token was signalled before opening finished. */
    Cancelled,
    /** @brief The file does not exist, or is a directory. */
    FileNotFound,
    /** @brief The file exists but could not be opened for reading - locked by
     *         another program, or not readable by this process. */
    FileUnreadable,
    /** @brief The bytes are not a readable OPC package - not a ZIP, or malformed. */
    NotAPackage,
    /** @brief A ZIP or XML limit from OpenSettings::PackageLimits was exceeded. */
    LimitExceeded,
    /** @brief OpcValidationMode::Strict found errors in the relationship graph. */
    ValidationFailed,
    /** @brief Markup compatibility processing failed for a part. */
    MarkupCompatibilityFailed,
    /** @brief A part held more characters than OpenSettings::MaxCharactersInPart allows. */
    PartTooLarge,
    /** @brief The package is readable but is not a document of the expected
     *         family: its main part is missing, as when a .xlsx is handed to
     *         WordDocument::Open(). */
    WrongDocumentType
};

/**
 * @brief Optional explanation of an Open() failure.
 *
 * Every `Open` overload takes a pointer to one of these as its last argument.
 * Pass nullptr - which is the default - and nothing is written; pass an object
 * and it is filled in when the call returns nullptr. This exists because
 * "returns nullptr" cannot distinguish a missing file from a zip bomb, an
 * encrypted document, or a `.xlsx` handed to the Word API, and the package that
 * knows the reason is destroyed on the way out, taking its
 * OpenXmlPackage::LastValidationResult() with it.
 *
 * @code
 * ExyokiOffice::Packaging::OpenError error;
 * auto editor = Word::WordDocumentEditor::Open("upload.docx", settings, nullptr, &error);
 * if (!editor)
 * {
 *     std::cerr << error.Message << '\n';
 *     for (const auto& issue : error.Diagnostics.Issues()) { log(issue.Message); }
 * }
 * @endcode
 */
struct EXYOKIOFFICE_EXPORT OpenError
{
    /** @brief What went wrong, as a value a caller can branch on. */
    OpenErrorCode Code = OpenErrorCode::None;
    /** @brief One sentence in English describing the failure, for logs. */
    std::string Message;
    /**
     * @brief Diagnostics the loader collected before it gave up.
     *
     * This is the package's LastValidationResult() copied out before the
     * package is discarded, so it names the offending part and the limit.
     */
    ValidationResult Diagnostics;

    /** @brief True when a failure was recorded. */
    [[nodiscard]] explicit operator bool() const noexcept { return Code != OpenErrorCode::None; }
};

/**
 * @brief High level representation of a Word (WordprocessingML) package with helper APIs.
 *
 * This class extends the generated WordprocessingDocument with behavior-based helpers.
 * It provides factory methods, document type management, and template attachment logic.
 * It keeps all package data in memory; you explicitly save when ready.
 * The documentation assumes no prior knowledge of Word or Open Packaging conventions.
 */
class EXYOKIOFFICE_EXPORT WordDocument : public WordprocessingDocument
{
public:
    using Ptr = std::shared_ptr<WordDocument>;

    /**
     * @brief Constructs an empty in-memory Word package helper.
     *
     * The instance owns no parts initially; it is just an empty package.
     * Use Create/Open helpers for most workflows because they set metadata.
     * The base WordprocessingDocument remains responsible for part accessors.
     * This constructor does not touch the file system or allocate parts.
     */
    WordDocument() = default;
    virtual ~WordDocument() override = default;

    /**
     * @brief Returns the semantic type of the document.
     *
     * The type determines which MIME string is assigned to the main document part.
     * It is inferred automatically when a package is opened.
     * It is updated when ChangeDocumentType is called.
     * This value is used when saving to preserve the correct document flavor.
     */
    WordprocessingDocumentType GetDocumentType() const noexcept;

    /**
     * @brief Creates a new in-memory WordDocument instance.
     *
     * The package is created empty and does not touch the file system.
     * It configures the document type and content type metadata.
     * Use SaveToFile or SaveToMemory when you are ready to persist.
     * Returns nullptr only if the object allocation fails.
     *
     * @param type  Desired document type (controls content type and macro/template capabilities).
     */
    static Ptr Create(WordprocessingDocumentType type);

    /**
     * @brief Adds the main document part and ensures its content type matches the document type.
     *
     * This overload hides the generated WordprocessingDocument implementation.
     * It guarantees that newly added main parts are initialized for immediate use.
     */
    std::shared_ptr<MainDocumentPart> AddMainDocumentPart(const std::shared_ptr<MainDocumentPart>& part = nullptr);

    /**
     * @brief Initializes a new document with the core Word parts.
     *
     * Ensures the main document, core properties, extended properties,
     * and document settings parts exist. Existing parts are reused.
     * @return True when the required parts are available.
     */
    bool InitDocument();

    /**
     * @brief Ensures core and extended properties exist and updates timestamps.
     *
     * Creates missing core/extended properties parts and stamps the current UTC
     * time into core properties. This should be called before saving.
     * @return True when properties were updated successfully.
     */
    bool UpdateDocumentProperties();

    /**
     * @brief Returns the unified core/extended/custom properties editor.
     *
     * The returned editor is a lightweight non-owning view; this document must
     * outlive it. It provides the complete shared properties API (all core
     * properties, extended application properties, and typed custom
     * properties), of which the Get/Set methods below expose the most common
     * subset.
     */
    DocumentProperties Properties();

    /**
     * @brief Returns the document title from core properties (empty when missing).
     */
    std::string GetTitle() const;
    /**
     * @brief Updates the document title in core properties.
     */
    void SetTitle(std::string_view title);

    /**
     * @brief Returns the document creator from core properties (empty when missing).
     */
    std::string GetCreator() const;
    /**
     * @brief Updates the document creator in core properties.
     */
    void SetCreator(std::string_view creator);

    /**
     * @brief Returns the lastModifiedBy value from core properties (empty when missing).
     */
    std::string GetLastModifiedBy() const;
    /**
     * @brief Updates the lastModifiedBy value in core properties.
     */
    void SetLastModifiedBy(std::string_view name);

    /**
     * @brief Returns the document subject from core properties (empty when missing).
     */
    std::string GetSubject() const;
    /**
     * @brief Updates the document subject in core properties.
     */
    void SetSubject(std::string_view subject);

    /**
     * @brief Returns the document keywords from core properties (empty when missing).
     */
    std::string GetKeywords() const;
    /**
     * @brief Updates the document keywords in core properties.
     *
     * Word shows this value as the document's "Tags"; multiple keywords are
     * conventionally separated by commas or semicolons.
     */
    void SetKeywords(std::string_view keywords);

    /**
     * @brief Returns the document description from core properties (empty when missing).
     */
    std::string GetDescription() const;
    /**
     * @brief Updates the document description (shown as "Comments" in Word) in core properties.
     */
    void SetDescription(std::string_view description);

    /**
     * @brief Returns the document category from core properties (empty when missing).
     */
    std::string GetCategory() const;
    /**
     * @brief Updates the document category in core properties.
     */
    void SetCategory(std::string_view category);

    /**
     * @brief Returns the document content status from core properties (empty when missing).
     */
    std::string GetContentStatus() const;
    /**
     * @brief Updates the document content status (e.g. "Draft", "Final") in core properties.
     */
    void SetContentStatus(std::string_view contentStatus);

    /**
     * @brief Returns the company name from extended (application) properties (empty when missing).
     */
    std::string GetCompany() const;
    /**
     * @brief Updates the company name in extended (application) properties.
     */
    void SetCompany(std::string_view company);

    /**
     * @brief Opens an existing file from disk.
     *
     * The file is loaded entirely into memory and kept detached from its origin.
     * Call SaveToFile or SaveToMemory explicitly whenever persistence is required.
     * Returns nullptr when the file cannot be read or parsed.
     * Returns nullptr when cancellationToken reports cancellation; no partially
     * initialized document is returned.
     * Use OpenSettings to control compatibility and safety limits.
     *
     * @param path        Source file path.
     * @param settings    Optional open settings controlling compatibility and quotas.
     * @param cancellationToken Optional non-owning token used to cancel a long-running open.
     */
    static Ptr Open(const std::filesystem::path& path,
                    const OpenSettings& settings = {},
                    const ICancellationToken* cancellationToken = nullptr,
                    OpenError* error = nullptr);

    /**
     * @brief Opens a document that is stored inside a seekable stream.
     *
     * The stream is read entirely into memory; the stream is not kept open.
     * The caller decides if and when the document is written back.
     * The caller is responsible for preparing the stream state and position so
     * it can be read from the beginning of the package.
     * Returns nullptr when the stream cannot be read or parsed.
     * Returns nullptr when cancellationToken reports cancellation; no partially
     * initialized document is returned.
     * Use this when the document comes from a custom I/O source.
     *
     * @param stream      Seekable stream containing a valid WordprocessingML ZIP package.
     * @param settings    Optional open settings controlling compatibility and quotas.
     * @param cancellationToken Optional non-owning token used to cancel a long-running open.
     */
    static Ptr Open(std::iostream& stream,
                    const OpenSettings& settings = {},
                    const ICancellationToken* cancellationToken = nullptr,
                    OpenError* error = nullptr);

    /**
     * @brief Opens a package that lives entirely in a byte buffer.
     *
     * The bytes are read into the document model; the source buffer is not
     * retained and is not updated by later save operations.
     * Returns nullptr when the bytes cannot be parsed into a package.
     * Returns nullptr when cancellationToken reports cancellation; no partially
     * initialized document is returned.
     * The buffer must contain a valid WordprocessingML ZIP package.
     *
     * @param packageBuffer  Buffer containing a valid WordprocessingML ZIP package.
     * @param settings       Advanced open settings (compatibility, limits).
     * @param cancellationToken Optional non-owning token used to cancel a long-running open.
     */
    static Ptr Open(const std::vector<Byte>& packageBuffer,
                    const OpenSettings& settings = {},
                    const ICancellationToken* cancellationToken = nullptr,
                    OpenError* error = nullptr);

    /**
     * @brief Opens a package from a contiguous byte range.
     *
     * The bytes are read into the document model; the source range is not
     * retained and is not updated by later save operations.
     * Returns nullptr when the bytes cannot be parsed into a package.
     * Returns nullptr when cancellationToken reports cancellation; no partially
     * initialized document is returned.
     * The range must contain a valid WordprocessingML ZIP package.
     *
     * @param packageBuffer  Byte range containing a valid WordprocessingML ZIP package.
     * @param settings       Advanced open settings (compatibility, limits).
     * @param cancellationToken Optional non-owning token used to cancel a long-running open.
     */
    static Ptr Open(std::span<const Byte> packageBuffer,
                    const OpenSettings& settings = {},
                    const ICancellationToken* cancellationToken = nullptr,
                    OpenError* error = nullptr);

    /**
     * @brief Generates a new document by cloning a template on disk.
     *
     * The template is read into an in-memory buffer to mimic a MemoryStream workflow.
     * By default the template remains attached via an external relationship.
     * This lets Word track the original template file for future updates.
     * Returns nullptr when the template cannot be read or parsed.
     *
     * @param templatePath  File system path to the template to clone.
     * @param attachTemplate  When true, the document points back to the template path.
     */
    static Ptr CreateFromTemplate(const std::filesystem::path& templatePath, bool attachTemplate = true);

    /**
     * @brief Changes the document type and updates the main document part content type.
     *
     * Word stores the document type inside the MIME string of /word/document.xml.
     * Switching between normal and macro-enabled types therefore requires metadata changes.
     * This helper performs both the in-memory flag update and the part metadata update.
     * If no main document part exists, the method only updates the cached type.
     */
    void ChangeDocumentType(WordprocessingDocumentType newType);

    /**
     * @brief Maps document type to the main part MIME content type.
     *
     * Word stores the document flavor in the content type of the main part.
     * This helper centralizes the mapping used when creating or switching types.
     *
     * @return A string literal with static storage duration; it stays valid for
     * the lifetime of the process.
     */
    static std::string_view MimeForDocumentType(WordprocessingDocumentType type);
    /**
     * @brief Converts a MIME content type to a document type value.
     *
     * This is the inverse of MimeForDocumentType() and is what Open() uses to
     * infer a package's WordprocessingDocumentType from its main part.
     *
     * @return The matching document type, or std::nullopt when the content type
     * is not one this library recognizes.
     */
    static std::optional<WordprocessingDocumentType> DocumentTypeFromMime(std::string_view contentType);

    /**
     * @brief Returns the document settings part, creating it when needed.
     *
     * The settings part holds high-level Word options such as templates and
     * document protection. If missing, it is created and seeded with a minimal
     * settings XML. The method never throws and returns nullptr on failure.
     * @return The settings part or nullptr when no main document part exists.
     */
    std::shared_ptr<DocumentSettingsPart> EnsureDocumentSettingsPart();

    /**
     * @brief Returns the external relationship naming the attached template.
     *
     * A document attached to a template stores the template as an external
     * relationship on the settings part. The value is kept exactly as written
     * and is often a plain file path rather than a URI.
     *
     * @return The reference, or nullopt when the document is attached to nothing.
     */
    [[nodiscard]] std::optional<Security::ExternalReference> GetAttachedTemplateReference() const;

    /**
     * @brief Reads the attached template through the resolver installed on this package.
     *
     * The library does not open the template itself; it asks the resolver, and
     * only after the target has passed the external resource policy. Without a
     * resolver the result is Security::ExternalResourceStatus::NoResolver, and
     * with the default policy it is AccessDenied.
     *
     * Attached template targets are usually relative or plain file paths, so
     * Security::ExternalResourcePolicy::BaseUri normally has to be set for this
     * to resolve at all.
     *
     * @param cancellationToken Optional non-owning token handed to the resolver.
     * @return The template bytes, or the status explaining why there are none.
     */
    [[nodiscard]] Security::ExternalResourceResponse ResolveAttachedTemplate(
        const ICancellationToken* cancellationToken = nullptr);

protected:
    virtual bool BeforeSave() override;

private:
    WordprocessingDocumentType m_documentType = WordprocessingDocumentType::Document;
    OpenSettings m_openSettings;

    /**
     * @brief The part of opening that follows loading, whatever the bytes came from.
     *
     * Kept in one place because every step of it can fail for a reason the
     * caller has to be able to tell apart, and one copy per overload would drift
     * apart one policy at a time.
     */
    static Ptr FinishOpen(Ptr document,
                          const OpenSettings& settings,
                          const ICancellationToken* cancellationToken,
                          OpenError* error);

    /**
     * @brief Stores open settings on the instance.
     *
     * Settings include compatibility level, markup compatibility behavior,
     * and character budget limits for XML parts.
     * This does not modify existing parts; it only updates configuration.
     * Use this immediately after opening to keep settings consistent.
     */
    void ApplyOpenSettings(const OpenSettings& settings);
    bool ApplyOpcValidationPolicy(const OpenSettings& settings);

    /**
     * @brief Resolves markup compatibility markup according to the open settings.
     *
     * Does nothing in the default NoProcess mode. Returns false when a part
     * declares through mc:MustUnderstand that it needs a namespace the requested
     * Office version does not cover, in which case Open fails.
     */
    bool ApplyMarkupCompatibilityPolicy(const OpenSettings& settings);
    /**
     * @brief Updates the cached document type based on the main part content type.
     *
     * This is called after opening to keep the in-memory document type consistent.
     * If the main part is missing or unknown, the type remains unchanged.
     * The method never creates parts and never throws.
     * Use it when you need to re-sync after external changes.
     */
    void UpdateDocumentTypeFromMainPart();
    /**
     * @brief Ensures the main document part has a content type that matches m_documentType.
     *
     * If the main part is missing, nothing happens.
     * If present, its content type is updated to the correct MIME string.
     * This keeps SaveToFile/SaveToMemory consistent with the selected type.
     * Call it after changing the document type or replacing the main part.
     */
    void EnsureMainPartContentType();
    /**
     * @brief Checks XML part sizes against the configured character budget.
     *
     * When MaxCharactersInPart is zero, the check is disabled.
     * This is a defensive measure against very large XML payloads.
     * The method walks all parts and inspects only XML parts.
     * @return True when all parts stay within the limit, false otherwise.
     */
    [[nodiscard]] bool EnforcePartCharacterBudget() const;
    /**
     * @brief Adds a relationship that links the document to its template file.
     *
     * The relationship is stored in the document settings part.
     * A w:attachedTemplate element references the relationship Id.
     * If settings are missing, they are created with a minimal settings XML.
     * This helper does nothing when no main document part exists.
     */
    void AttachTemplateRelationship(const std::filesystem::path& templatePath);
};

} // namespace ExyokiOffice::Packaging
