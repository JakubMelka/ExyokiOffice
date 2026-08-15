// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

#include "ExyokiOffice/Export.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace ExyokiOffice
{

class OpenXmlPackage;

namespace Packaging
{

/**
 * @brief Typed value of one OOXML custom document property (docProps/custom.xml).
 *
 * The alternatives map to the variant types Office writes for user-defined
 * properties: `vt:lpwstr` (string), `vt:i4` (32-bit integer), `vt:r8`
 * (double), `vt:bool`, and `vt:filetime` (UTC date/time).
 */
using DocumentCustomPropertyValue =
    std::variant<std::string, Int32, Real, bool, std::chrono::system_clock::time_point>;

/** @brief One named custom document property together with its typed value. */
struct EXYOKIOFFICE_EXPORT DocumentCustomProperty
{
    std::string Name;
    DocumentCustomPropertyValue Value;

    bool operator==(const DocumentCustomProperty&) const = default;
};

/**
 * @brief Unified core, extended, and custom document-properties editor.
 *
 * The editor works identically for Word, Excel, and PowerPoint packages: it
 * reads and writes the OPC properties parts (`docProps/core.xml`,
 * `docProps/app.xml`, and `docProps/custom.xml`) that every Office family
 * shares. Missing parts are created on the first write; reads on documents
 * without a properties part return empty or `std::nullopt` values.
 *
 * Core string setters remove the underlying XML element when the value is
 * empty, and date setters remove it when passed `std::nullopt`; the
 * corresponding getters return an empty string or `std::nullopt` in both
 * cases. Written elements keep the canonical ECMA-376 child order so strict
 * validators accept the result.
 *
 * The editor holds a non-owning reference: the package must outlive it.
 * Obtain an instance from `WordDocument::Properties()`,
 * `ExcelDocument::Properties()`, `PowerPointDocument::Properties()`, one of
 * the high-level editors, or construct it directly around any loaded package.
 *
 * @code
 * auto editor = Word::WordDocumentEditor::CreateNew();
 * auto properties = editor->Properties();
 * properties.SetTitle("Quarterly report");
 * properties.SetCompany("Contoso");
 * properties.SetCustomProperty("Reviewed", true);
 * properties.SetCustomProperty("Revision", Int32(7));
 * @endcode
 */
class EXYOKIOFFICE_EXPORT DocumentProperties
{
public:
    /**
     * @brief Wraps a package without taking ownership.
     * @param package Package whose properties parts are edited. It must stay
     *        alive for the lifetime of this editor.
     */
    explicit DocumentProperties(OpenXmlPackage& package) noexcept;

    /// @name Core properties (docProps/core.xml)
    /// @{

    /** @return `dc:title`, or an empty string when absent. */
    std::string GetTitle() const;
    /** @brief Sets `dc:title`; an empty value removes the element. */
    bool SetTitle(std::string_view value);

    /** @return `dc:subject`, or an empty string when absent. */
    std::string GetSubject() const;
    bool SetSubject(std::string_view value);

    /** @return `dc:creator`, or an empty string when absent. */
    std::string GetCreator() const;
    bool SetCreator(std::string_view value);

    /** @return `cp:keywords`, or an empty string when absent. */
    std::string GetKeywords() const;
    bool SetKeywords(std::string_view value);

    /** @return `dc:description`, or an empty string when absent. */
    std::string GetDescription() const;
    bool SetDescription(std::string_view value);

    /** @return `cp:lastModifiedBy`, or an empty string when absent. */
    std::string GetLastModifiedBy() const;
    bool SetLastModifiedBy(std::string_view value);

    /** @return `cp:category`, or an empty string when absent. */
    std::string GetCategory() const;
    bool SetCategory(std::string_view value);

    /** @return `cp:contentStatus`, or an empty string when absent. */
    std::string GetContentStatus() const;
    bool SetContentStatus(std::string_view value);

    /** @return `dc:language`, or an empty string when absent. */
    std::string GetLanguage() const;
    bool SetLanguage(std::string_view value);

    /** @return `dc:identifier`, or an empty string when absent. */
    std::string GetIdentifier() const;
    bool SetIdentifier(std::string_view value);

    /** @return `cp:revision`, or an empty string when absent. */
    std::string GetRevision() const;
    bool SetRevision(std::string_view value);

    /** @return `cp:version`, or an empty string when absent. */
    std::string GetVersion() const;
    bool SetVersion(std::string_view value);

    /** @return Parsed `dcterms:created`, or `std::nullopt` when absent or unparsable. */
    std::optional<std::chrono::system_clock::time_point> GetCreated() const;
    /** @brief Sets `dcterms:created` (W3CDTF, UTC); `std::nullopt` removes the element. */
    bool SetCreated(std::optional<std::chrono::system_clock::time_point> value);

    /** @return Parsed `dcterms:modified`, or `std::nullopt` when absent or unparsable. */
    std::optional<std::chrono::system_clock::time_point> GetModified() const;
    bool SetModified(std::optional<std::chrono::system_clock::time_point> value);

    /** @return Parsed `cp:lastPrinted`, or `std::nullopt` when absent or unparsable. */
    std::optional<std::chrono::system_clock::time_point> GetLastPrinted() const;
    bool SetLastPrinted(std::optional<std::chrono::system_clock::time_point> value);

    /// @}
    /// @name Extended properties (docProps/app.xml)
    /// @{

    /** @return `ap:Application` (producing application), or an empty string. */
    std::string GetApplication() const;
    bool SetApplication(std::string_view value);

    /** @return `ap:AppVersion`, or an empty string. */
    std::string GetApplicationVersion() const;
    bool SetApplicationVersion(std::string_view value);

    /** @return `ap:Company`, or an empty string. */
    std::string GetCompany() const;
    bool SetCompany(std::string_view value);

    /** @return `ap:Manager`, or an empty string. */
    std::string GetManager() const;
    bool SetManager(std::string_view value);

    /** @return `ap:HyperlinkBase`, or an empty string. */
    std::string GetHyperlinkBase() const;
    bool SetHyperlinkBase(std::string_view value);

    /** @return `ap:Template` (attached template name), or an empty string. */
    std::string GetTemplate() const;
    bool SetTemplate(std::string_view value);

    /**
     * @name Read-only application statistics
     * Values are written by the producing application; ExyokiOffice does not
     * compute layout-dependent statistics, so these are exposed read-only.
     */
    /// @{
    std::optional<Int32> GetPages() const;                ///< `ap:Pages`
    std::optional<Int32> GetWords() const;                ///< `ap:Words`
    std::optional<Int32> GetCharacters() const;           ///< `ap:Characters`
    std::optional<Int32> GetCharactersWithSpaces() const; ///< `ap:CharactersWithSpaces`
    std::optional<Int32> GetLines() const;                ///< `ap:Lines`
    std::optional<Int32> GetParagraphs() const;           ///< `ap:Paragraphs`
    std::optional<Int32> GetSlides() const;               ///< `ap:Slides`
    std::optional<Int32> GetNotes() const;                ///< `ap:Notes`
    std::optional<Int32> GetHiddenSlides() const;         ///< `ap:HiddenSlides`
    std::optional<Int32> GetTotalTime() const;            ///< `ap:TotalTime` (minutes)
    /// @}

    /// @}
    /// @name Custom properties (docProps/custom.xml)
    /// @{

    /**
     * @return Names of every `op:property` element, including properties whose
     *         value type is not representable as DocumentCustomPropertyValue.
     */
    std::vector<std::string> GetCustomPropertyNames() const;

    /**
     * @return Every custom property with a representable typed value, in
     *         document order. Properties with unsupported value types
     *         (vectors, blobs, ...) are skipped.
     */
    std::vector<DocumentCustomProperty> GetCustomProperties() const;

    /**
     * @brief Reads one custom property by name (ASCII case-insensitive).
     * @return The typed value, or `std::nullopt` when the property does not
     *         exist or its value type is not representable.
     */
    std::optional<DocumentCustomPropertyValue> GetCustomProperty(std::string_view name) const;

    /**
     * @brief Creates or replaces a custom property (upsert).
     *
     * New properties receive the standard user-defined-properties format id
     * and the next free property id (`pid`, starting at 2). Replacing an
     * existing property keeps its `pid` and replaces only the typed value.
     *
     * @param name Property name; must not be empty. Lookup is ASCII
     *        case-insensitive; the stored spelling is preserved on update.
     * @param value Typed value written as the matching `vt:*` element.
     * @return True when the property was written.
     */
    bool SetCustomProperty(std::string_view name, const DocumentCustomPropertyValue& value);

    /** @brief Removes one custom property by name (ASCII case-insensitive). */
    bool RemoveCustomProperty(std::string_view name);

    /**
     * @brief Removes every custom property while keeping the part itself.
     * @return The number of removed properties.
     */
    Size ClearCustomProperties();

    /// @}
    /// @name Save-time maintenance
    /// @{

    /**
     * @brief Refreshes bookkeeping properties before a save.
     *
     * Ensures `dcterms:created` exists (initialized to now when missing),
     * updates `dcterms:modified` to the current UTC time, and fills in
     * `ap:Application` when the document does not name a producing
     * application yet. Values previously set by the user are preserved.
     *
     * @param applicationName Fallback producing-application name; ignored when empty.
     * @return True when the properties parts exist (or were created) and were updated.
     */
    bool UpdateSaveTimeProperties(std::string_view applicationName);

    /// @}
    /// @name Date helpers
    /// @{

    /** @brief Formats a time point as a W3CDTF UTC string (`2026-01-31T09:30:00Z`). */
    static std::string FormatW3cDateTime(std::chrono::system_clock::time_point value);

    /**
     * @brief Parses a W3CDTF/ISO-8601 date-time string.
     *
     * Accepts reduced precision (`2026`, `2026-01`, `2026-01-31`) and full
     * timestamps with `Z` or `±hh:mm` offsets; fractional seconds are
     * truncated. Returns `std::nullopt` for unparsable input.
     */
    static std::optional<std::chrono::system_clock::time_point> ParseW3cDateTime(std::string_view text);

    /// @}

private:
    OpenXmlPackage* m_package;
};

} // namespace Packaging
} // namespace ExyokiOffice
