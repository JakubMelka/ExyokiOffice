// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

#include "ExyokiOffice/Color.hpp"
#include "ExyokiOffice/Export.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <array>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ExyokiOffice
{

namespace Packaging
{
class ThemePart;
}

/** Ordered semantic positions in a DrawingML theme color scheme. */
enum class ThemeColorSlot
{
    Dark1,
    Light1,
    Dark2,
    Light2,
    Accent1,
    Accent2,
    Accent3,
    Accent4,
    Accent5,
    Accent6,
    Hyperlink,
    FollowedHyperlink,
    Count
};

/** Typeface selection for one major or minor DrawingML font collection. */
struct ThemeFontCollection
{
    std::string Latin;         ///< Latin-script typeface.
    std::string EastAsian;     ///< Default East Asian typeface; may be empty.
    std::string ComplexScript; ///< Default complex-script typeface; may be empty.
    /** Script code to typeface mappings, for example `"Jpan"` to `"Yu Gothic"`. */
    std::vector<std::pair<std::string, std::string>> SupplementalFonts;
    bool operator==(const ThemeFontCollection&) const = default;
};

/**
 * @brief Essential, strongly typed DrawingML theme settings.
 *
 * Colors are stored in the fixed OOXML scheme order represented by
 * ThemeColorSlot. Reading resolves system colors through their `lastClr`
 * fallback. Applying the model writes explicit sRGB colors while preserving
 * the existing format/effect style matrix and extension elements.
 *
 * The same model is shared by every Office family: Word and Excel themes hang
 * off the main document/workbook part, PowerPoint themes off each slide
 * master.
 */
struct ThemeSettings
{
    std::string Name;            ///< `a:theme/@name`.
    std::string ColorSchemeName; ///< `a:clrScheme/@name`.
    std::array<Color, static_cast<Size>(ThemeColorSlot::Count)> Colors;
    std::string FontSchemeName; ///< `a:fontScheme/@name`.
    ThemeFontCollection MajorFonts;
    ThemeFontCollection MinorFonts;
    std::string FormatSchemeName; ///< `a:fmtScheme/@name`; style members remain lossless.
    bool operator==(const ThemeSettings&) const = default;
};

/**
 * @brief Shared DrawingML theme operations over a `ThemePart`.
 *
 * The service does not render anything; it reads and writes the theme XML that
 * Word (main document part), Excel (workbook part), and PowerPoint (slide
 * master parts) reference through their theme relationship. Family-specific
 * editors expose convenience wrappers built on top of these primitives.
 */
class EXYOKIOFFICE_EXPORT ThemeService
{
public:
    /**
     * @return Essential typed theme settings, or `std::nullopt` when the part
     *         is missing or does not contain a complete color/font/format
     *         scheme.
     */
    static std::optional<ThemeSettings> ReadSettings(const std::shared_ptr<Packaging::ThemePart>& part);

    /**
     * @brief Applies essential theme colors, fonts, and scheme names.
     *
     * Existing fill, line, effect, background-fill, and extension XML is
     * preserved; only the color scheme, font scheme, and scheme names change.
     *
     * @return `false` when the part is missing, the current theme is
     *         incomplete, or required names, typefaces, or colors are invalid.
     */
    static bool WriteSettings(const std::shared_ptr<Packaging::ThemePart>& part,
                              const ThemeSettings& settings);

    /** @return The complete lossless theme XML, or `std::nullopt` when the part is missing. */
    static std::optional<std::string> ReadXml(const std::shared_ptr<Packaging::ThemePart>& part);

    /**
     * @brief Replaces the theme XML after validating that it parses into an `a:theme` root.
     * @return `true` when the XML was parsed and stored; invalid XML leaves the part unchanged.
     */
    static bool WriteXml(const std::shared_ptr<Packaging::ThemePart>& part, std::string xml);

    /**
     * @brief Checks that the XML parses into a typed `a:theme` root element.
     *
     * Use this before creating a theme part for a document that has none, so
     * invalid input does not leave an empty part behind.
     */
    [[nodiscard]] static bool IsValidThemeXml(const std::string& xml);

    /**
     * @brief Fills a theme part with the standard Office default theme.
     *
     * The content is built through the typed DrawingML DOM: the Office color
     * scheme (accent colors 4472C4, ED7D31, ...), the Calibri Light/Calibri
     * font scheme, and the three-entry fill, line, effect, and background-fill
     * style matrices. The result satisfies ReadSettings() immediately. Use it
     * to initialize a theme part for documents created without one.
     *
     * Any content already present in the part is replaced.
     *
     * @param part Theme part to populate.
     * @param name Theme name written to `a:theme/@name`; must not be empty.
     * @return `true` when the complete theme was written.
     */
    static bool WriteDefaultTheme(const std::shared_ptr<Packaging::ThemePart>& part,
                                  std::string_view name = "Office Theme");
};

} // namespace ExyokiOffice
