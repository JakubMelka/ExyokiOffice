// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

#include "ExyokiOffice/Export.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ExyokiOffice::Excel
{

/** @brief Source used to resolve an OOXML spreadsheet color. */
enum class ExcelColorKind
{
    Automatic,
    Rgb,
    Theme,
    Indexed
};

/**
 * @brief Spreadsheet color with optional tint adjustment.
 *
 * RGB colors use eight hexadecimal ARGB digits (for example `FFFF0000` for
 * opaque red). Theme and indexed colors retain workbook-relative color
 * semantics. Tint is optional and, when present, must be in `[-1, 1]`.
 */
struct EXYOKIOFFICE_EXPORT ExcelColor
{
    ExcelColorKind Kind = ExcelColorKind::Automatic;
    std::string Argb;
    UInt32 Index = 0;
    std::optional<Real> Tint;

    static ExcelColor Automatic();
    static ExcelColor Rgb(std::string argb, std::optional<Real> tint = std::nullopt);
    static ExcelColor Theme(UInt32 themeIndex, std::optional<Real> tint = std::nullopt);
    static ExcelColor Indexed(UInt32 paletteIndex, std::optional<Real> tint = std::nullopt);
    bool operator==(const ExcelColor&) const = default;
};

/** @brief Font underline variants supported by SpreadsheetML. */
enum class ExcelUnderlineStyle
{
    None,
    Single,
    Double,
    SingleAccounting,
    DoubleAccounting
};
/** @brief Theme font scheme selection. */
enum class ExcelFontScheme
{
    None,
    Major,
    Minor
};
/** @brief Baseline position of spreadsheet font glyphs. */
enum class ExcelFontVerticalAlignment
{
    Baseline,
    Superscript,
    Subscript
};

/**
 * @brief Complete reusable spreadsheet font definition.
 *
 * Optional scalar fields are omitted from XML when not specified, allowing
 * theme/default inheritance. Boolean decorations are emitted only when true.
 */
struct EXYOKIOFFICE_EXPORT ExcelFont
{
    std::optional<std::string> Name;
    std::optional<Real> Size;
    std::optional<ExcelColor> Color;
    bool Bold = false;
    bool Italic = false;
    bool Strike = false;
    bool Outline = false;
    bool Shadow = false;
    bool Condense = false;
    bool Extend = false;
    ExcelUnderlineStyle Underline = ExcelUnderlineStyle::None;
    ExcelFontVerticalAlignment VerticalAlignment = ExcelFontVerticalAlignment::Baseline;
    std::optional<Int32> Family;
    std::optional<Int32> CharacterSet;
    ExcelFontScheme Scheme = ExcelFontScheme::None;
    bool operator==(const ExcelFont&) const = default;
};

/** @brief Pattern types available for spreadsheet fills. */
enum class ExcelFillPattern
{
    None,
    Solid,
    MediumGray,
    DarkGray,
    LightGray,
    DarkHorizontal,
    DarkVertical,
    DarkDown,
    DarkUp,
    DarkGrid,
    DarkTrellis,
    LightHorizontal,
    LightVertical,
    LightDown,
    LightUp,
    LightGrid,
    LightTrellis,
    Gray125,
    Gray0625
};
/** @brief Fill representation stored in a style. */
enum class ExcelFillKind
{
    Pattern,
    LinearGradient,
    PathGradient
};

/** @brief One color stop in a gradient fill. */
struct EXYOKIOFFICE_EXPORT ExcelGradientStop
{
    Real Position = 0.0;
    ExcelColor Color;
    bool operator==(const ExcelGradientStop&) const = default;
};

/**
 * @brief Pattern or gradient fill definition.
 *
 * Pattern fills use `pattern`, `foreground`, and `background`. Gradient fills
 * use ordered `gradientStops`; linear gradients additionally use `degree`,
 * while path gradients may specify convergence offsets.
 */
struct EXYOKIOFFICE_EXPORT ExcelFill
{
    ExcelFillKind Kind = ExcelFillKind::Pattern;
    ExcelFillPattern Pattern = ExcelFillPattern::None;
    std::optional<ExcelColor> Foreground;
    std::optional<ExcelColor> Background;
    std::vector<ExcelGradientStop> GradientStops;
    Real Degree = 0.0;
    Real Left = 0.0;
    Real Right = 0.0;
    Real Top = 0.0;
    Real Bottom = 0.0;
    bool operator==(const ExcelFill&) const = default;
};

/** @brief Spreadsheet border line styles. */
enum class ExcelBorderStyle
{
    None,
    Thin,
    Medium,
    Dashed,
    Dotted,
    Thick,
    Double,
    Hair,
    MediumDashed,
    DashDot,
    MediumDashDot,
    DashDotDot,
    MediumDashDotDot,
    SlantDashDot
};

/** @brief Style and color of one border edge. */
struct EXYOKIOFFICE_EXPORT ExcelBorderSide
{
    ExcelBorderStyle Style = ExcelBorderStyle::None;
    std::optional<ExcelColor> Color;
    bool operator==(const ExcelBorderSide&) const = default;
};

/**
 * @brief Full spreadsheet border definition.
 *
 * In addition to outer edges, the model supports diagonal and inner horizontal
 * and vertical borders. `diagonalUp` and `diagonalDown` control which diagonal
 * directions use the `diagonal` edge definition.
 */
struct EXYOKIOFFICE_EXPORT ExcelBorder
{
    ExcelBorderSide Left;
    ExcelBorderSide Right;
    ExcelBorderSide Top;
    ExcelBorderSide Bottom;
    ExcelBorderSide Diagonal;
    ExcelBorderSide Horizontal;
    ExcelBorderSide Vertical;
    bool DiagonalUp = false;
    bool DiagonalDown = false;
    bool Outline = true;
    bool operator==(const ExcelBorder&) const = default;
};

/** @brief Horizontal cell-content alignment. */
enum class ExcelHorizontalAlignment
{
    General,
    Left,
    Center,
    Right,
    Fill,
    Justify,
    CenterContinuous,
    Distributed
};
/** @brief Vertical cell-content alignment. */
enum class ExcelVerticalAlignment
{
    Top,
    Center,
    Bottom,
    Justify,
    Distributed
};

/**
 * @brief Optional cell alignment properties.
 *
 * Text rotation is expressed using SpreadsheetML's `0..180` representation.
 * Reading order uses `0` for context, `1` for left-to-right, and `2` for
 * right-to-left. Unspecified properties inherit from the base style.
 */
struct EXYOKIOFFICE_EXPORT ExcelAlignment
{
    std::optional<ExcelHorizontalAlignment> Horizontal;
    std::optional<ExcelVerticalAlignment> Vertical;
    std::optional<UInt32> TextRotation;
    std::optional<bool> WrapText;
    std::optional<UInt32> Indent;
    std::optional<Int32> RelativeIndent;
    std::optional<bool> JustifyLastLine;
    std::optional<bool> ShrinkToFit;
    std::optional<UInt32> ReadingOrder;
    bool operator==(const ExcelAlignment&) const = default;
};

/** @brief Optional locked/hidden flags used with worksheet protection. */
struct EXYOKIOFFICE_EXPORT ExcelProtection
{
    std::optional<bool> Locked;
    std::optional<bool> Hidden;
    bool operator==(const ExcelProtection&) const = default;
};

/**
 * @brief Built-in or custom number format.
 *
 * Set `builtInId` to reference one of Excel's predefined formats. A non-empty
 * `formatCode` creates or reuses a custom format with an allocated id of at
 * least 164; when both are supplied the custom code takes precedence.
 */
struct EXYOKIOFFICE_EXPORT ExcelNumberFormat
{
    std::optional<UInt32> BuiltInId;
    std::string FormatCode;

    /** @brief Creates the General built-in number format. */
    static ExcelNumberFormat General();
    /** @brief Creates a built-in integer format (`0`). */
    static ExcelNumberFormat Integer();
    /** @brief Creates a built-in number format with two decimal places (`0.00`). */
    static ExcelNumberFormat Decimal();
    /** @brief Creates a built-in thousands-separated integer format (`#,##0`). */
    static ExcelNumberFormat ThousandsInteger();
    /** @brief Creates a built-in thousands-separated decimal format (`#,##0.00`). */
    static ExcelNumberFormat ThousandsDecimal();
    /** @brief Creates Excel's built-in integer percentage format (`0%`). */
    static ExcelNumberFormat Percent();
    /** @brief Creates Excel's built-in two-decimal percentage format (`0.00%`). */
    static ExcelNumberFormat PercentDecimal();
    /** @brief Creates Excel's built-in two-decimal scientific format (`0.00E+00`). */
    static ExcelNumberFormat Scientific();
    /** @brief Creates Excel's locale-aware short-date built-in format. */
    static ExcelNumberFormat ShortDate();
    /** @brief Creates Excel's locale-aware time-with-seconds built-in format. */
    static ExcelNumberFormat TimeWithSeconds();
    /**
     * @brief Creates a locale-aware accounting format.
     *
     * The returned custom code uses the supplied currency symbol and displays
     * zero values as a dash. The symbol is XML-escaped by the package writer.
     *
     * @param currencySymbol Currency text such as `$`, `EUR`, or `Kč`.
     * @param decimalPlaces Number of decimal digits in the range 0..30.
     * @return A custom accounting format, or std::nullopt for invalid input.
     */
    static std::optional<ExcelNumberFormat> Accounting(std::string_view currencySymbol = "$",
                                                       UInt32 decimalPlaces = 2);
    /**
     * @brief Creates a custom number format from an OOXML format code.
     *
     * @return A custom format, or std::nullopt when the code is empty.
     */
    static std::optional<ExcelNumberFormat> Custom(std::string formatCode);

    bool operator==(const ExcelNumberFormat&) const = default;
};

/**
 * @brief Composite cell-style definition registered as one cell XF.
 *
 * Every optional component is independently deduplicated. Equal complete
 * definitions always resolve to the same style index, including across
 * separate StyleRepository service instances for the same document.
 */
struct EXYOKIOFFICE_EXPORT ExcelStyle
{
    std::optional<ExcelNumberFormat> NumberFormat;
    std::optional<ExcelFont> Font;
    std::optional<ExcelFill> Fill;
    std::optional<ExcelBorder> Border;
    std::optional<ExcelAlignment> Alignment;
    std::optional<ExcelProtection> Protection;
    bool QuotePrefix = false;
    bool PivotButton = false;
    bool operator==(const ExcelStyle&) const = default;
};

} // namespace ExyokiOffice::Excel
