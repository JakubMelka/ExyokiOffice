// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

#include "ExyokiOffice/Packaging/GeneratedParts.hpp"
#include "ExyokiOffice/Packaging/WordprocessingDocument.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

namespace ExyokiOffice::Packaging
{

/** @brief Identifies a supported PresentationML package flavor. */
enum class PowerPointDocumentType
{
    /** @brief Standard macro-free `.pptx` presentation. */
    Presentation,
    /** @brief Macro-enabled `.pptm` presentation. */
    MacroEnabledPresentation,
    /** @brief Standard macro-free `.potx` presentation template. */
    Template,
    /** @brief Macro-enabled `.potm` presentation template. */
    MacroEnabledTemplate,
    /** @brief Standard macro-free `.ppsx` slide show. */
    SlideShow,
    /** @brief Macro-enabled `.ppsm` slide show. */
    MacroEnabledSlideShow
};

/**
 * @brief Low-level lifecycle API for PresentationML packages.
 *
 * The class augments the generated PresentationDocument with package-flavor
 * mapping, minimal package initialization, open helpers, and save-time
 * properties. It deliberately does not create slides; slide collection
 * management belongs to the high-level PowerPoint editing API.
 */
class EXYOKIOFFICE_EXPORT PowerPointDocument : public PresentationDocument
{
public:
    using Ptr = std::shared_ptr<PowerPointDocument>;

    PowerPointDocument() = default;
    ~PowerPointDocument() override = default;

    /** @return The flavor represented by the main presentation part MIME type. */
    PowerPointDocumentType GetDocumentType() const noexcept;
    /** @brief Creates an empty package helper with the requested flavor. */
    static Ptr Create(PowerPointDocumentType type = PowerPointDocumentType::Presentation);
    /**
     * @brief Attaches a presentation part and applies the selected MIME type.
     * @param part Optional pre-created part; a new part is allocated when null.
     * @return The attached part, or nullptr on failure.
     */
    std::shared_ptr<PresentationPart> AddPresentationPart(const std::shared_ptr<PresentationPart>& part = nullptr);
    /** @brief Ensures the main part and core/extended properties exist. */
    bool InitDocument();
    /** @brief Refreshes package properties written by ExyokiOffice. */
    bool UpdateDocumentProperties();

    /**
     * @brief Returns the unified core/extended/custom properties editor.
     *
     * The returned editor is a lightweight non-owning view; this document must
     * outlive it. It exposes the same shared properties API as the Word and
     * Excel document classes.
     */
    DocumentProperties Properties();

    /** @brief Opens a PresentationML package from a filesystem path. */
    static Ptr Open(const std::filesystem::path& path, const OpenSettings& settings = {},
                    const ICancellationToken* cancellationToken = nullptr);
    /** @brief Opens a PresentationML package from a seekable stream. */
    static Ptr Open(std::iostream& stream, const OpenSettings& settings = {},
                    const ICancellationToken* cancellationToken = nullptr);
    /** @brief Opens a PresentationML package from an owned byte buffer. */
    static Ptr Open(const std::vector<Byte>& packageBuffer, const OpenSettings& settings = {},
                    const ICancellationToken* cancellationToken = nullptr);
    /** @brief Opens a PresentationML package from a contiguous byte range. */
    static Ptr Open(std::span<const Byte> packageBuffer, const OpenSettings& settings = {},
                    const ICancellationToken* cancellationToken = nullptr);

    /**
     * @brief Changes the package flavor and main-part content type.
     * @param newType Presentation, template, or slide-show flavor to declare.
     */
    void ChangeDocumentType(PowerPointDocumentType newType);

    /** @brief Maps document type to the main part MIME content type. */
    static std::string_view MimeForDocumentType(PowerPointDocumentType type);
    /** @brief Converts a MIME content type to a document type value, or std::nullopt when unrecognized. */
    static std::optional<PowerPointDocumentType> DocumentTypeFromMime(std::string_view mime);

protected:
    bool BeforeSave() override;

    /** @brief Stores the settings the document was opened with. */
    void ApplyOpenSettings(const OpenSettings& settings);

    /**
     * @brief Resolves markup compatibility markup according to the open settings.
     *
     * Does nothing in the default NoProcess mode. Returns false when a part declares
     * through mc:MustUnderstand that it needs a namespace the requested Office
     * version does not cover, in which case Open fails.
     */
    bool ApplyMarkupCompatibilityPolicy(const OpenSettings& settings);

private:
    PowerPointDocumentType m_documentType = PowerPointDocumentType::Presentation;
    OpenSettings m_openSettings;
    void UpdateDocumentTypeFromPresentationPart();
    void EnsurePresentationPartContentType();
};

} // namespace ExyokiOffice::Packaging
