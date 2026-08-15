// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

#include "ExyokiOffice/Packaging/GeneratedParts.hpp"
#include "ExyokiOffice/Packaging/WordprocessingDocument.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

namespace ExyokiOffice::Packaging
{

/**
 * @brief Enumerates the high-level package flavors supported by spreadsheet workbooks.
 *
 * The document type controls the MIME content type of the workbook part.
 * Office uses that content type to decide whether the package is a regular
 * workbook, a template, or a macro-enabled file. The type does not change the
 * XML structure by itself; it changes the package metadata that is written when
 * the file is saved.
 */
enum class SpreadsheetDocumentType
{
    /**
     * @brief Standard .xlsx workbook without macros.
     *
     * This is the default spreadsheet flavor and is appropriate for ordinary
     * workbooks that contain cells, formulas, styles, and other SpreadsheetML
     * content without VBA projects.
     */
    Workbook,
    /**
     * @brief Spreadsheet template stored as .xltx.
     *
     * Templates are reusable workbook blueprints. Use this when the saved
     * package should be treated by Office as a template instead of a normal
     * workbook.
     */
    Template,
    /**
     * @brief Macro-enabled workbook stored as .xlsm.
     *
     * This value declares that the package may contain VBA macro content.
     * The helper only updates package metadata; adding or editing a VBA project
     * remains a lower-level package operation.
     */
    MacroEnabledWorkbook,
    /**
     * @brief Macro-enabled spreadsheet template stored as .xltm.
     *
     * Use this for templates that are expected to carry macro functionality.
     */
    MacroEnabledTemplate
};

/**
 * @brief High-level representation of an Excel SpreadsheetML package.
 *
 * ExcelDocument extends the generated SpreadsheetDocument package with
 * lifecycle helpers that mirror the WordDocument API: creation, opening from
 * common sources, document type tracking, initialization of required workbook
 * parts, and save-time property updates.
 *
 * The class keeps package data in memory. Opening a file reads the package into
 * the object; saving must be requested explicitly through the inherited
 * OpenXmlPackage save APIs or through ExcelDocumentEditor.
 */
class EXYOKIOFFICE_EXPORT ExcelDocument : public SpreadsheetDocument
{
public:
    using Ptr = std::shared_ptr<ExcelDocument>;

    /**
     * @brief Constructs an empty in-memory spreadsheet package helper.
     *
     * The instance initially has no workbook part. Prefer Create(), Open(), or
     * ExcelDocumentEditor::CreateNew() for normal workflows because those helpers
     * also initialize required package metadata.
     */
    ExcelDocument() = default;
    /**
     * @brief Destroys the in-memory package helper.
     */
    ~ExcelDocument() override = default;

    /**
     * @brief Returns the semantic spreadsheet package flavor.
     *
     * The value is inferred when opening an existing package and is used to keep
     * the workbook part content type consistent when saving.
     */
    SpreadsheetDocumentType GetDocumentType() const noexcept;

    /**
     * @brief Creates a new empty ExcelDocument with the requested package type.
     *
     * The returned package has no parts until InitDocument() or higher-level
     * editor helpers add them. This is useful for low-level callers that want to
     * construct the package explicitly.
     *
     * @param type Desired workbook flavor and workbook part MIME type.
     * @return A new in-memory document instance, or nullptr if allocation fails.
     */
    static Ptr Create(SpreadsheetDocumentType type = SpreadsheetDocumentType::Workbook);

    /**
     * @brief Adds or attaches the workbook part and applies the selected content type.
     *
     * This overload wraps the generated SpreadsheetDocument helper so newly
     * added workbook parts match GetDocumentType().
     *
     * @param part Optional pre-created workbook part. When null, a part is created.
     * @return The attached workbook part, or nullptr on failure.
     */
    std::shared_ptr<WorkbookPart> AddWorkbookPart(const std::shared_ptr<WorkbookPart>& part = nullptr);

    /**
     * @brief Ensures the package contains the core workbook structure.
     *
     * The method creates the workbook part, workbook sheets container, core file
     * properties part, and extended file properties part when they are missing.
     * Existing parts are reused.
     *
     * @return True when the required structure is available.
     */
    bool InitDocument();
    /**
     * @brief Ensures package properties exist and updates save-time metadata.
     *
     * Core properties are stamped with W3CDTF timestamps and extended properties
     * identify ExyokiOffice as the application. This method is called before
     * saving.
     *
     * @return True when properties were updated successfully.
     */
    bool UpdateDocumentProperties();

    /**
     * @brief Returns the unified core/extended/custom properties editor.
     *
     * The returned editor is a lightweight non-owning view; this document must
     * outlive it. It exposes the same shared properties API as the Word and
     * PowerPoint document classes.
     */
    DocumentProperties Properties();

    /**
     * @brief Opens an existing spreadsheet package from disk.
     *
     * The package is read fully into memory and detached from the source path.
     * Call SaveToFile() explicitly to persist later changes.
     *
     * @param path Source .xlsx/.xltx/.xlsm/.xltm path.
     * @param settings Open settings controlling limits and validation behavior.
     * @param cancellationToken Optional non-owning token used to cancel loading.
     * @return Loaded document, or nullptr when loading, parsing, validation, or cancellation fails.
     */
    static Ptr Open(const std::filesystem::path& path,
                    const OpenSettings& settings = {},
                    const ICancellationToken* cancellationToken = nullptr);
    /**
     * @brief Opens a spreadsheet package from a seekable stream.
     *
     * The stream is read into memory and is not retained by the document.
     *
     * @param stream Seekable stream containing an OPC SpreadsheetML package.
     * @param settings Open settings controlling limits and validation behavior.
     * @param cancellationToken Optional non-owning token used to cancel loading.
     * @return Loaded document, or nullptr on failure.
     */
    static Ptr Open(std::iostream& stream,
                    const OpenSettings& settings = {},
                    const ICancellationToken* cancellationToken = nullptr);
    /**
     * @brief Opens a spreadsheet package from an owned byte buffer.
     *
     * The source buffer is copied into the document model and is not updated by
     * future save operations.
     *
     * @param packageBuffer Bytes containing an OPC SpreadsheetML package.
     * @param settings Open settings controlling limits and validation behavior.
     * @param cancellationToken Optional non-owning token used to cancel loading.
     * @return Loaded document, or nullptr on failure.
     */
    static Ptr Open(const std::vector<Byte>& packageBuffer,
                    const OpenSettings& settings = {},
                    const ICancellationToken* cancellationToken = nullptr);
    /**
     * @brief Opens a spreadsheet package from a contiguous byte range.
     *
     * @param packageBuffer Byte span containing an OPC SpreadsheetML package.
     * @param settings Open settings controlling limits and validation behavior.
     * @param cancellationToken Optional non-owning token used to cancel loading.
     * @return Loaded document, or nullptr on failure.
     */
    static Ptr Open(std::span<const Byte> packageBuffer,
                    const OpenSettings& settings = {},
                    const ICancellationToken* cancellationToken = nullptr);

    /**
     * @brief Changes the spreadsheet package flavor and workbook part content type.
     *
     * If the workbook part already exists, its content type is updated
     * immediately. If no workbook part exists yet, the new type is stored and
     * applied when the part is created.
     *
     * @param newType New workbook/template/macro-enabled package flavor.
     */
    void ChangeDocumentType(SpreadsheetDocumentType newType);

    /**
     * @brief Reports whether the workbook contains an attached VBA project.
     *
     * The project is treated as an opaque binary payload. ExyokiOffice never
     * interprets or executes VBA code.
     */
    [[nodiscard]] bool HasVbaProject() const noexcept;
    /**
     * @brief Returns an exact copy of the embedded `vbaProject.bin` payload.
     *
     * An empty vector is returned when the workbook has no VBA project.
     */
    std::vector<Byte> GetVbaProjectData() const;
    /**
     * @brief Adds or replaces the workbook's opaque VBA project payload.
     *
     * A macro-free workbook/template is automatically converted to its
     * macro-enabled counterpart. Empty payloads are rejected; use
     * RemoveVbaProject() to remove a project.
     *
     * @return True when the payload was attached or replaced.
     */
    bool SetVbaProjectData(std::span<const Byte> data);
    /**
     * @brief Removes the VBA project and converts the package to a macro-free type.
     *
     * Workbook/template semantics are retained (`.xlsm` becomes `.xlsx` and
     * `.xltm` becomes `.xltx`).
     *
     * @return True when an existing VBA project was removed.
     */
    bool RemoveVbaProject();

    /**
     * @brief Maps document type to the workbook part MIME content type.
     *
     * This does not read from disk; it only returns a string literal.
     */
    static std::string_view MimeForDocumentType(SpreadsheetDocumentType type);
    /**
     * @brief Converts a MIME content type to a document type value.
     *
     * Returns std::nullopt when the content type is not recognized.
     */
    static std::optional<SpreadsheetDocumentType> DocumentTypeFromMime(std::string_view contentType);

protected:
    /**
     * @brief Performs save-time metadata updates before package serialization.
     *
     * This hook is called by OpenXmlPackage save operations.
     */
    bool BeforeSave() override;

private:
    SpreadsheetDocumentType m_documentType = SpreadsheetDocumentType::Workbook;
    OpenSettings m_openSettings;

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
    void UpdateDocumentTypeFromWorkbookPart();
    void EnsureWorkbookPartContentType();
    [[nodiscard]] bool EnforcePartCharacterBudget() const;
};

} // namespace ExyokiOffice::Packaging
