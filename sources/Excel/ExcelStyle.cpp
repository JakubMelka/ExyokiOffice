// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "ExyokiOffice/Excel/ExcelStyle.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <utility>

namespace ExyokiOffice::Excel
{

ExcelColor ExcelColor::Automatic()
{
    return {};
}

ExcelColor ExcelColor::Rgb(std::string argb, std::optional<Real> tint)
{
    ExcelColor color;
    color.Kind = ExcelColorKind::Rgb;
    color.Argb = std::move(argb);
    color.Tint = tint;
    return color;
}

ExcelColor ExcelColor::Theme(UInt32 themeIndex, std::optional<Real> tint)
{
    ExcelColor color;
    color.Kind = ExcelColorKind::Theme;
    color.Index = themeIndex;
    color.Tint = tint;
    return color;
}

ExcelColor ExcelColor::Indexed(UInt32 paletteIndex, std::optional<Real> tint)
{
    ExcelColor color;
    color.Kind = ExcelColorKind::Indexed;
    color.Index = paletteIndex;
    color.Tint = tint;
    return color;
}

ExcelNumberFormat ExcelNumberFormat::General()
{
    return ExcelNumberFormat{0, {}};
}

ExcelNumberFormat ExcelNumberFormat::Integer()
{
    return ExcelNumberFormat{1, {}};
}

ExcelNumberFormat ExcelNumberFormat::Decimal()
{
    return ExcelNumberFormat{2, {}};
}

ExcelNumberFormat ExcelNumberFormat::ThousandsInteger()
{
    return ExcelNumberFormat{3, {}};
}

ExcelNumberFormat ExcelNumberFormat::ThousandsDecimal()
{
    return ExcelNumberFormat{4, {}};
}

ExcelNumberFormat ExcelNumberFormat::Percent()
{
    return ExcelNumberFormat{9, {}};
}

ExcelNumberFormat ExcelNumberFormat::PercentDecimal()
{
    return ExcelNumberFormat{10, {}};
}

ExcelNumberFormat ExcelNumberFormat::Scientific()
{
    return ExcelNumberFormat{11, {}};
}

ExcelNumberFormat ExcelNumberFormat::ShortDate()
{
    return ExcelNumberFormat{14, {}};
}

ExcelNumberFormat ExcelNumberFormat::TimeWithSeconds()
{
    return ExcelNumberFormat{21, {}};
}

std::optional<ExcelNumberFormat> ExcelNumberFormat::Accounting(std::string_view currencySymbol,
                                                               UInt32 decimalPlaces)
{
    if (currencySymbol.empty() || decimalPlaces > 30)
    {
        return std::nullopt;
    }

    std::string escapedSymbol(currencySymbol);
    for (Size position = 0; (position = escapedSymbol.find('"', position)) != std::string::npos;
         position += 2)
    {
        escapedSymbol.insert(position, 1, '"');
    }

    const std::string decimals = decimalPlaces == 0 ? std::string{} : "." + std::string(decimalPlaces, '0');
    const std::string symbol = "\"" + escapedSymbol + "\"";
    return ExcelNumberFormat{
        std::nullopt,
        "_(" + symbol + "* #,##0" + decimals + "_);_(" + symbol + "* (#,##0" + decimals +
            ");_(" + symbol + "* \"-\"" + std::string(decimalPlaces == 0 ? 0 : decimalPlaces + 1, '_') +
            "_);_(@_)"};
}

std::optional<ExcelNumberFormat> ExcelNumberFormat::Custom(std::string formatCode)
{
    if (formatCode.empty())
    {
        return std::nullopt;
    }
    return ExcelNumberFormat{std::nullopt, std::move(formatCode)};
}

} // namespace ExyokiOffice::Excel
