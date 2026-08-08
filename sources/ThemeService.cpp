// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "ExyokiOffice/ThemeService.hpp"

#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/Drawing.hpp"
#include "ExyokiOffice/OpenXmlSimpleTypes.hpp"
#include "ExyokiOffice/Packaging/GeneratedParts.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <utility>
#include <vector>

namespace ExyokiOffice
{

namespace Drawing = ExyokiOffice::DocumentFormat::OpenXml::Drawing;

/// Shared read/write primitives for the DrawingML color and font schemes.
class ThemeSchemeHelpers
{
public:
    static std::shared_ptr<Drawing::Color2Type> ColorSlotElement(
        const std::shared_ptr<Drawing::ColorScheme>& scheme, ThemeColorSlot slot)
    {
        if (!scheme)
        {
            return nullptr;
        }
        switch (slot)
        {
            case ThemeColorSlot::Dark1:
            {
                return scheme->GetFirstChildOfType<Drawing::Dark1Color>();
            }
            case ThemeColorSlot::Light1:
            {
                return scheme->GetFirstChildOfType<Drawing::Light1Color>();
            }
            case ThemeColorSlot::Dark2:
            {
                return scheme->GetFirstChildOfType<Drawing::Dark2Color>();
            }
            case ThemeColorSlot::Light2:
            {
                return scheme->GetFirstChildOfType<Drawing::Light2Color>();
            }
            case ThemeColorSlot::Accent1:
            {
                return scheme->GetFirstChildOfType<Drawing::Accent1Color>();
            }
            case ThemeColorSlot::Accent2:
            {
                return scheme->GetFirstChildOfType<Drawing::Accent2Color>();
            }
            case ThemeColorSlot::Accent3:
            {
                return scheme->GetFirstChildOfType<Drawing::Accent3Color>();
            }
            case ThemeColorSlot::Accent4:
            {
                return scheme->GetFirstChildOfType<Drawing::Accent4Color>();
            }
            case ThemeColorSlot::Accent5:
            {
                return scheme->GetFirstChildOfType<Drawing::Accent5Color>();
            }
            case ThemeColorSlot::Accent6:
            {
                return scheme->GetFirstChildOfType<Drawing::Accent6Color>();
            }
            case ThemeColorSlot::Hyperlink:
            {
                return scheme->GetFirstChildOfType<Drawing::Hyperlink>();
            }
            case ThemeColorSlot::FollowedHyperlink:
            {
                return scheme->GetFirstChildOfType<Drawing::FollowedHyperlinkColor>();
            }
            case ThemeColorSlot::Count:
            {
                return nullptr;
            }
        }
        return nullptr;
    }

    static std::optional<Color> ReadSlotColor(const std::shared_ptr<Drawing::Color2Type>& slot)
    {
        auto rgb = slot ? slot->GetFirstChildOfType<Drawing::RgbColorModelHex>() : nullptr;
        if (rgb)
        {
            return Color::FromHexString(rgb->GetVal().ToString());
        }
        auto system = slot ? slot->GetFirstChildOfType<Drawing::SystemColor>() : nullptr;
        return system ? Color::FromHexString(system->GetLastColor().ToString()) : std::nullopt;
    }

    static bool WriteSlotColor(const std::shared_ptr<Drawing::Color2Type>& slot, const Color& value)
    {
        if (!slot || value.IsAuto())
        {
            return false;
        }
        for (const auto& child : slot->Children())
        {
            slot->RemoveChild(child);
        }
        return WriteRgbColor(slot, value.ToHexString());
    }

    /// Appends `a:srgbClr val="RRGGBB"` to any color-holding element.
    static bool WriteRgbColor(const std::shared_ptr<Drawing::Color2Type>& slot, std::string_view hex)
    {
        auto rgb = slot ? slot->AppendChild<Drawing::RgbColorModelHex>() : nullptr;
        if (!rgb)
        {
            return false;
        }
        rgb->SetVal(OpenXmlSimpleValueConvertor::GetHexBinaryValueFromString(hex));
        return true;
    }

    static ThemeFontCollection ReadFontCollection(const std::shared_ptr<Drawing::FontCollectionType>& fonts)
    {
        ThemeFontCollection result;
        auto latin = fonts ? fonts->GetFirstChildOfType<Drawing::LatinFont>() : nullptr;
        auto eastAsian = fonts ? fonts->GetFirstChildOfType<Drawing::EastAsianFont>() : nullptr;
        auto complex = fonts ? fonts->GetFirstChildOfType<Drawing::ComplexScriptFont>() : nullptr;
        result.Latin = latin ? latin->GetTypeface().ToString() : std::string{};
        result.EastAsian = eastAsian ? eastAsian->GetTypeface().ToString() : std::string{};
        result.ComplexScript = complex ? complex->GetTypeface().ToString() : std::string{};
        if (fonts)
        {
            for (const auto& font : fonts->Elements<Drawing::SupplementalFont>())
            {
                result.SupplementalFonts.emplace_back(font->GetScript().ToString(),
                                                      font->GetTypeface().ToString());
            }
        }
        return result;
    }

    static bool WriteFontCollection(const std::shared_ptr<Drawing::FontCollectionType>& fonts,
                                    const ThemeFontCollection& value)
    {
        if (!fonts || value.Latin.empty())
        {
            return false;
        }
        auto latin = fonts->GetFirstChildOfType<Drawing::LatinFont>();
        auto eastAsian = fonts->GetFirstChildOfType<Drawing::EastAsianFont>();
        auto complex = fonts->GetFirstChildOfType<Drawing::ComplexScriptFont>();
        if (!latin || !eastAsian || !complex)
        {
            return false;
        }
        latin->SetTypeface(StringValue(value.Latin));
        eastAsian->SetTypeface(StringValue(value.EastAsian));
        complex->SetTypeface(StringValue(value.ComplexScript));
        for (const auto& font : fonts->Elements<Drawing::SupplementalFont>())
        {
            fonts->RemoveChild(font);
        }
        for (const auto& [script, typeface] : value.SupplementalFonts)
        {
            if (script.empty() || typeface.empty())
            {
                return false;
            }
            auto font = fonts->AppendChild<Drawing::SupplementalFont>();
            font->SetScript(StringValue(script));
            font->SetTypeface(StringValue(typeface));
        }
        return true;
    }
};

/**
 * @brief Builds the standard Office theme through the typed DrawingML DOM.
 *
 * The values mirror the theme Office writes for new documents: the Office color
 * scheme, Calibri Light/Calibri fonts, and the three-entry fill, line, effect,
 * and background-fill style matrices referenced by `phClr` placeholders.
 */
class DefaultThemeBuilder
{
public:
    static bool Build(const std::shared_ptr<Drawing::Theme>& theme, std::string_view name)
    {
        if (!theme || name.empty())
        {
            return false;
        }

        // Start from a clean root so rebuilding a populated part cannot append
        // a second set of theme elements.
        for (const auto& child : theme->Children())
        {
            theme->RemoveChild(child);
        }
        theme->SetName(StringValue(std::string(name)));

        auto elements = theme->AppendChild<Drawing::ThemeElements>();
        if (!elements)
        {
            return false;
        }
        return BuildColorScheme(elements) && BuildFontScheme(elements) && BuildFormatScheme(elements) &&
               theme->AppendChild<Drawing::ObjectDefaults>() != nullptr &&
               theme->AppendChild<Drawing::ExtraColorSchemeList>() != nullptr;
    }

private:
    /// One `a:schemeClr val="phClr"` color transform, e.g. `a:tint val="67000"`.
    enum class ColorTransform
    {
        Tint,
        Shade,
        Alpha,
        SaturationModulation,
        LuminanceModulation
    };

    struct TransformSpec
    {
        ColorTransform Kind;
        Int32 Value;
    };

    struct GradientStopSpec
    {
        Int32 Position;
        std::vector<TransformSpec> Transforms;
    };

    static constexpr std::string_view kSchemeName = "Office";
    /// Gradient direction Office uses for every theme fill: 90 degrees, in 60000ths.
    static constexpr Int32 kGradientAngle = 5400000;

    static bool BuildColorScheme(const std::shared_ptr<Drawing::ThemeElements>& elements)
    {
        auto colors = elements->AppendChild<Drawing::ColorScheme>();
        if (!colors)
        {
            return false;
        }
        colors->SetName(StringValue(std::string(kSchemeName)));

        // Office maps the primary text/background pair to system colors so the
        // theme follows the user's Windows appearance settings.
        return SystemSlot<Drawing::Dark1Color>(colors, Drawing::SystemColorValues::WindowText, "000000") &&
               SystemSlot<Drawing::Light1Color>(colors, Drawing::SystemColorValues::Window, "FFFFFF") &&
               RgbSlot<Drawing::Dark2Color>(colors, "44546A") &&
               RgbSlot<Drawing::Light2Color>(colors, "E7E6E6") &&
               RgbSlot<Drawing::Accent1Color>(colors, "4472C4") &&
               RgbSlot<Drawing::Accent2Color>(colors, "ED7D31") &&
               RgbSlot<Drawing::Accent3Color>(colors, "A5A5A5") &&
               RgbSlot<Drawing::Accent4Color>(colors, "FFC000") &&
               RgbSlot<Drawing::Accent5Color>(colors, "5B9BD5") &&
               RgbSlot<Drawing::Accent6Color>(colors, "70AD47") &&
               RgbSlot<Drawing::Hyperlink>(colors, "0563C1") &&
               RgbSlot<Drawing::FollowedHyperlinkColor>(colors, "954F72");
    }

    template <typename TSlot>
    static bool RgbSlot(const std::shared_ptr<Drawing::ColorScheme>& colors, std::string_view hex)
    {
        return ThemeSchemeHelpers::WriteRgbColor(colors->AppendChild<TSlot>(), hex);
    }

    template <typename TSlot>
    static bool SystemSlot(const std::shared_ptr<Drawing::ColorScheme>& colors,
                           Drawing::SystemColorValues::Value value,
                           std::string_view lastColorHex)
    {
        auto slot = colors->AppendChild<TSlot>();
        auto system = slot ? slot->template AppendChild<Drawing::SystemColor>() : nullptr;
        if (!system)
        {
            return false;
        }
        system->SetVal(EnumValue<Drawing::SystemColorValues>(Drawing::SystemColorValues(value)));
        system->SetLastColor(OpenXmlSimpleValueConvertor::GetHexBinaryValueFromString(lastColorHex));
        return true;
    }

    static bool BuildFontScheme(const std::shared_ptr<Drawing::ThemeElements>& elements)
    {
        auto fonts = elements->AppendChild<Drawing::FontScheme>();
        if (!fonts)
        {
            return false;
        }
        fonts->SetName(StringValue(std::string(kSchemeName)));
        return BuildFontCollection(fonts->AppendChild<Drawing::MajorFont>(), "Calibri Light") &&
               BuildFontCollection(fonts->AppendChild<Drawing::MinorFont>(), "Calibri");
    }

    static bool BuildFontCollection(const std::shared_ptr<Drawing::FontCollectionType>& fonts,
                                    std::string_view latinTypeface)
    {
        if (!fonts)
        {
            return false;
        }
        auto latin = fonts->AppendChild<Drawing::LatinFont>();
        auto eastAsian = fonts->AppendChild<Drawing::EastAsianFont>();
        auto complex = fonts->AppendChild<Drawing::ComplexScriptFont>();
        if (!latin || !eastAsian || !complex)
        {
            return false;
        }
        latin->SetTypeface(StringValue(std::string(latinTypeface)));
        // Empty east-asian and complex-script typefaces mean "inherit"; Office
        // writes them explicitly and ThemeSettings round-trips them as empty.
        eastAsian->SetTypeface(StringValue(std::string()));
        complex->SetTypeface(StringValue(std::string()));
        return true;
    }

    static bool BuildFormatScheme(const std::shared_ptr<Drawing::ThemeElements>& elements)
    {
        auto format = elements->AppendChild<Drawing::FormatScheme>();
        if (!format)
        {
            return false;
        }
        format->SetName(StringValue(std::string(kSchemeName)));
        return BuildFillStyles(format) && BuildLineStyles(format) && BuildEffectStyles(format) &&
               BuildBackgroundFillStyles(format);
    }

    static bool BuildFillStyles(const std::shared_ptr<Drawing::FormatScheme>& format)
    {
        auto fills = format->AppendChild<Drawing::FillStyleList>();
        if (!fills)
        {
            return false;
        }
        return AppendSolidFill(fills, {}) &&
               AppendGradientFill(fills,
                                  {{0, {{ColorTransform::LuminanceModulation, 110000}, {ColorTransform::SaturationModulation, 105000}, {ColorTransform::Tint, 67000}}},
                                   {50000, {{ColorTransform::LuminanceModulation, 105000}, {ColorTransform::SaturationModulation, 103000}, {ColorTransform::Tint, 73000}}},
                                   {100000, {{ColorTransform::LuminanceModulation, 105000}, {ColorTransform::SaturationModulation, 109000}, {ColorTransform::Tint, 81000}}}}) &&
               AppendGradientFill(fills,
                                  {{0, {{ColorTransform::SaturationModulation, 103000}, {ColorTransform::LuminanceModulation, 102000}, {ColorTransform::Tint, 94000}}},
                                   {50000, {{ColorTransform::SaturationModulation, 110000}, {ColorTransform::LuminanceModulation, 100000}, {ColorTransform::Shade, 100000}}},
                                   {100000, {{ColorTransform::LuminanceModulation, 99000}, {ColorTransform::SaturationModulation, 120000}, {ColorTransform::Shade, 78000}}}});
    }

    static bool BuildLineStyles(const std::shared_ptr<Drawing::FormatScheme>& format)
    {
        auto lines = format->AppendChild<Drawing::LineStyleList>();
        if (!lines)
        {
            return false;
        }
        // Thin, medium, and thick theme outlines in EMU.
        return AppendOutline(lines, 6350) && AppendOutline(lines, 12700) && AppendOutline(lines, 19050);
    }

    static bool AppendOutline(const std::shared_ptr<Drawing::LineStyleList>& lines, Int32 width)
    {
        auto outline = lines->AppendChild<Drawing::Outline>();
        if (!outline)
        {
            return false;
        }
        outline->SetWidth(Int32Value(width));
        outline->SetCapType(EnumValue<Drawing::LineCapValues>(Drawing::LineCapValues(Drawing::LineCapValues::Flat)));
        outline->SetCompoundLineType(
            EnumValue<Drawing::CompoundLineValues>(Drawing::CompoundLineValues(Drawing::CompoundLineValues::Single)));
        outline->SetAlignment(
            EnumValue<Drawing::PenAlignmentValues>(Drawing::PenAlignmentValues(Drawing::PenAlignmentValues::Center)));

        if (!AppendSolidFill(outline, {}))
        {
            return false;
        }
        auto dash = outline->AppendChild<Drawing::PresetDash>();
        auto miter = outline->AppendChild<Drawing::Miter>();
        if (!dash || !miter)
        {
            return false;
        }
        dash->SetVal(EnumValue<Drawing::PresetLineDashValues>(
            Drawing::PresetLineDashValues(Drawing::PresetLineDashValues::Solid)));
        miter->SetLimit(Int32Value(800000));
        return true;
    }

    static bool BuildEffectStyles(const std::shared_ptr<Drawing::FormatScheme>& format)
    {
        auto styles = format->AppendChild<Drawing::EffectStyleList>();
        if (!styles)
        {
            return false;
        }
        // Office leaves the first two effect levels empty and applies a soft
        // drop shadow at the third.
        return AppendEffectStyle(styles, false) && AppendEffectStyle(styles, false) &&
               AppendEffectStyle(styles, true);
    }

    static bool AppendEffectStyle(const std::shared_ptr<Drawing::EffectStyleList>& styles, bool withShadow)
    {
        auto style = styles->AppendChild<Drawing::EffectStyle>();
        auto effects = style ? style->AppendChild<Drawing::EffectList>() : nullptr;
        if (!effects)
        {
            return false;
        }
        if (!withShadow)
        {
            return true;
        }

        auto shadow = effects->AppendChild<Drawing::OuterShadow>();
        if (!shadow)
        {
            return false;
        }
        shadow->SetBlurRadius(Int64Value(57150));
        shadow->SetDistance(Int64Value(19050));
        shadow->SetDirection(Int32Value(kGradientAngle));
        shadow->SetAlignment(EnumValue<Drawing::RectangleAlignmentValues>(
            Drawing::RectangleAlignmentValues(Drawing::RectangleAlignmentValues::Center)));
        shadow->SetRotateWithShape(BooleanValue(false));

        auto rgb = shadow->AppendChild<Drawing::RgbColorModelHex>();
        if (!rgb)
        {
            return false;
        }
        rgb->SetVal(OpenXmlSimpleValueConvertor::GetHexBinaryValueFromString("000000"));
        auto alpha = rgb->AppendChild<Drawing::Alpha>();
        if (!alpha)
        {
            return false;
        }
        alpha->SetVal(Int32Value(63000));
        return true;
    }

    static bool BuildBackgroundFillStyles(const std::shared_ptr<Drawing::FormatScheme>& format)
    {
        auto fills = format->AppendChild<Drawing::BackgroundFillStyleList>();
        if (!fills)
        {
            return false;
        }
        return AppendSolidFill(fills, {}) &&
               AppendSolidFill(fills, {{ColorTransform::Tint, 95000},
                                       {ColorTransform::SaturationModulation, 170000}}) &&
               AppendGradientFill(fills,
                                  {{0, {{ColorTransform::Tint, 93000}, {ColorTransform::SaturationModulation, 150000}, {ColorTransform::Shade, 98000}, {ColorTransform::LuminanceModulation, 102000}}},
                                   {50000, {{ColorTransform::Tint, 98000}, {ColorTransform::SaturationModulation, 130000}, {ColorTransform::Shade, 90000}, {ColorTransform::LuminanceModulation, 103000}}},
                                   {100000, {{ColorTransform::Shade, 63000}, {ColorTransform::SaturationModulation, 120000}}}});
    }

    /// Appends `a:solidFill` holding the `phClr` placeholder plus transforms.
    template <typename TParent>
    static bool AppendSolidFill(const std::shared_ptr<TParent>& parent,
                                std::initializer_list<TransformSpec> transforms)
    {
        auto fill = parent->template AppendChild<Drawing::SolidFill>();
        if (!fill)
        {
            return false;
        }
        return AppendPlaceholderColor(fill, transforms) != nullptr;
    }

    template <typename TParent>
    static bool AppendGradientFill(const std::shared_ptr<TParent>& parent,
                                   std::initializer_list<GradientStopSpec> stops)
    {
        const std::shared_ptr<Drawing::GradientFill> fill =
            parent->template AppendChild<Drawing::GradientFill>();
        if (!fill)
        {
            return false;
        }
        fill->SetRotateWithShape(BooleanValue(true));

        const std::shared_ptr<Drawing::GradientStopList> stopList =
            fill->AppendChild<Drawing::GradientStopList>();
        if (!stopList)
        {
            return false;
        }
        for (const auto& stop : stops)
        {
            auto stopElement = stopList->AppendChild<Drawing::GradientStop>();
            if (!stopElement)
            {
                return false;
            }
            stopElement->SetPosition(Int32Value(stop.Position));
            if (!AppendPlaceholderColor(stopElement, stop.Transforms))
            {
                return false;
            }
        }

        auto linear = fill->AppendChild<Drawing::LinearGradientFill>();
        if (!linear)
        {
            return false;
        }
        linear->SetAngle(Int32Value(kGradientAngle));
        linear->SetScaled(BooleanValue(false));
        return true;
    }

    /// Appends `a:schemeClr val="phClr"` with the requested color transforms.
    template <typename TParent, typename TTransforms>
    static std::shared_ptr<Drawing::SchemeColor> AppendPlaceholderColor(const std::shared_ptr<TParent>& parent,
                                                                        const TTransforms& transforms)
    {
        auto color = parent->template AppendChild<Drawing::SchemeColor>();
        if (!color)
        {
            return nullptr;
        }
        color->SetVal(EnumValue<Drawing::SchemeColorValues>(
            Drawing::SchemeColorValues(Drawing::SchemeColorValues::PhColor)));
        for (const auto& transform : transforms)
        {
            if (!ApplyTransform(color, transform))
            {
                return nullptr;
            }
        }
        return color;
    }

    static bool ApplyTransform(const std::shared_ptr<Drawing::SchemeColor>& color, const TransformSpec& transform)
    {
        switch (transform.Kind)
        {
            case ColorTransform::Tint:
            {
                return SetPercentage<Drawing::Tint>(color, transform.Value);
            }
            case ColorTransform::Shade:
            {
                return SetPercentage<Drawing::Shade>(color, transform.Value);
            }
            case ColorTransform::Alpha:
            {
                return SetPercentage<Drawing::Alpha>(color, transform.Value);
            }
            case ColorTransform::SaturationModulation:
            {
                return SetPercentage<Drawing::SaturationModulation>(color, transform.Value);
            }
            case ColorTransform::LuminanceModulation:
            {
                return SetPercentage<Drawing::LuminanceModulation>(color, transform.Value);
            }
        }
        return false;
    }

    template <typename TPercentage>
    static bool SetPercentage(const std::shared_ptr<Drawing::SchemeColor>& color, Int32 value)
    {
        auto element = color->AppendChild<TPercentage>();
        if (!element)
        {
            return false;
        }
        element->SetVal(Int32Value(value));
        return true;
    }
};

std::optional<ThemeSettings> ThemeService::ReadSettings(const std::shared_ptr<Packaging::ThemePart>& part)
{
    auto theme = part ? part->GetTypedRootElement() : nullptr;
    auto elements = theme ? theme->GetFirstChildOfType<Drawing::ThemeElements>() : nullptr;
    auto colors = elements ? elements->GetFirstChildOfType<Drawing::ColorScheme>() : nullptr;
    auto fonts = elements ? elements->GetFirstChildOfType<Drawing::FontScheme>() : nullptr;
    auto format = elements ? elements->GetFirstChildOfType<Drawing::FormatScheme>() : nullptr;
    auto major = fonts ? fonts->GetFirstChildOfType<Drawing::MajorFont>() : nullptr;
    auto minor = fonts ? fonts->GetFirstChildOfType<Drawing::MinorFont>() : nullptr;
    if (!theme || !colors || !fonts || !format || !major || !minor)
    {
        return std::nullopt;
    }

    ThemeSettings result;
    result.Name = theme->GetName().ToString();
    result.ColorSchemeName = colors->GetName().ToString();
    result.FontSchemeName = fonts->GetName().ToString();
    result.FormatSchemeName = format->GetName().ToString();
    for (Size index = 0; index < result.Colors.size(); ++index)
    {
        auto color = ThemeSchemeHelpers::ReadSlotColor(
            ThemeSchemeHelpers::ColorSlotElement(colors, static_cast<ThemeColorSlot>(index)));
        if (!color)
        {
            return std::nullopt;
        }
        result.Colors[index] = *color;
    }
    result.MajorFonts = ThemeSchemeHelpers::ReadFontCollection(major);
    result.MinorFonts = ThemeSchemeHelpers::ReadFontCollection(minor);
    return result;
}

bool ThemeService::WriteSettings(const std::shared_ptr<Packaging::ThemePart>& part,
                                 const ThemeSettings& settings)
{
    auto current = ReadSettings(part);
    auto theme = part ? part->GetTypedRootElement() : nullptr;
    auto elements = theme ? theme->GetFirstChildOfType<Drawing::ThemeElements>() : nullptr;
    auto colors = elements ? elements->GetFirstChildOfType<Drawing::ColorScheme>() : nullptr;
    auto fonts = elements ? elements->GetFirstChildOfType<Drawing::FontScheme>() : nullptr;
    auto format = elements ? elements->GetFirstChildOfType<Drawing::FormatScheme>() : nullptr;
    auto major = fonts ? fonts->GetFirstChildOfType<Drawing::MajorFont>() : nullptr;
    auto minor = fonts ? fonts->GetFirstChildOfType<Drawing::MinorFont>() : nullptr;
    const auto validSupplementalFonts = [](const ThemeFontCollection& collection)
    {
        return std::all_of(collection.SupplementalFonts.begin(), collection.SupplementalFonts.end(),
                           [](const auto& font)
                           { return !font.first.empty() && !font.second.empty(); });
    };
    if (!current || settings.Name.empty() || settings.ColorSchemeName.empty() || settings.FontSchemeName.empty() ||
        settings.FormatSchemeName.empty() || settings.MajorFonts.Latin.empty() || settings.MinorFonts.Latin.empty() ||
        !major || !minor || !major->GetFirstChildOfType<Drawing::LatinFont>() ||
        !major->GetFirstChildOfType<Drawing::EastAsianFont>() ||
        !major->GetFirstChildOfType<Drawing::ComplexScriptFont>() ||
        !minor->GetFirstChildOfType<Drawing::LatinFont>() ||
        !minor->GetFirstChildOfType<Drawing::EastAsianFont>() ||
        !minor->GetFirstChildOfType<Drawing::ComplexScriptFont>() ||
        !validSupplementalFonts(settings.MajorFonts) || !validSupplementalFonts(settings.MinorFonts))
    {
        return false;
    }
    for (const auto& color : settings.Colors)
    {
        if (color.IsAuto())
        {
            return false;
        }
    }

    theme->SetName(StringValue(settings.Name));
    colors->SetName(StringValue(settings.ColorSchemeName));
    fonts->SetName(StringValue(settings.FontSchemeName));
    format->SetName(StringValue(settings.FormatSchemeName));
    for (Size index = 0; index < settings.Colors.size(); ++index)
    {
        if (!ThemeSchemeHelpers::WriteSlotColor(
                ThemeSchemeHelpers::ColorSlotElement(colors, static_cast<ThemeColorSlot>(index)),
                settings.Colors[index]))
        {
            return false;
        }
    }
    return ThemeSchemeHelpers::WriteFontCollection(major, settings.MajorFonts) &&
           ThemeSchemeHelpers::WriteFontCollection(minor, settings.MinorFonts);
}

std::optional<std::string> ThemeService::ReadXml(const std::shared_ptr<Packaging::ThemePart>& part)
{
    return part ? std::optional<std::string>(part->GetXmlString()) : std::nullopt;
}

bool ThemeService::WriteXml(const std::shared_ptr<Packaging::ThemePart>& part, std::string xml)
{
    if (!part || !IsValidThemeXml(xml))
    {
        return false;
    }

    part->SetXmlString(std::move(xml));
    return part->GetTypedRootElement() != nullptr;
}

bool ThemeService::IsValidThemeXml(const std::string& xml)
{
    if (xml.empty())
    {
        return false;
    }

    // Validate against a scratch part so invalid XML cannot clobber a theme.
    auto candidate = std::make_shared<Packaging::ThemePart>();
    candidate->SetXmlString(xml);
    return candidate->GetTypedRootElement() != nullptr;
}

bool ThemeService::WriteDefaultTheme(const std::shared_ptr<Packaging::ThemePart>& part, std::string_view name)
{
    return DefaultThemeBuilder::Build(part ? part->GetTypedRootElement() : nullptr, name);
}

} // namespace ExyokiOffice
