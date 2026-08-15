// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "ExyokiOffice/Excel/ExcelDocument.hpp"

#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/Spreadsheet.hpp"
#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/Spreadsheet.Enums.hpp"
#include "OpenXmlDomInternal.hpp"
#include "XmlNamespaceResolver.hpp"
#include "ExyokiOffice/StandardTypes.hpp"
#include "AsciiText.hpp"

#include <algorithm>
#include <cstdint>
#include <cmath>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ExyokiOffice::Excel
{

namespace Spreadsheet = ExyokiOffice::DocumentFormat::OpenXml::Spreadsheet;

class StyleRepositoryImplementation final
{
public:
    StyleRepositoryImplementation() = delete;

    struct Context
    {
        std::shared_ptr<Packaging::WorkbookStylesPart> part;
        std::shared_ptr<Spreadsheet::Stylesheet> stylesheet;
        std::shared_ptr<Spreadsheet::NumberingFormats> numberingFormats;
        std::shared_ptr<Spreadsheet::Fonts> fonts;
        std::shared_ptr<Spreadsheet::Fills> fills;
        std::shared_ptr<Spreadsheet::Borders> borders;
        std::shared_ptr<Spreadsheet::CellStyleFormats> styleFormats;
        std::shared_ptr<Spreadsheet::CellFormats> cellFormats;
        std::shared_ptr<Spreadsheet::CellStyles> cellStyles;
    };

    static std::optional<Context> EnsureContext(const ExcelDocument::Ptr& document)
    {
        const auto workbookPart = document ? document->GetWorkbookPart() : nullptr;
        if (!workbookPart)
        {
            return std::nullopt;
        }
        auto part = workbookPart->GetWorkbookStylesPart();
        if (!part)
        {
            part = workbookPart->AddWorkbookStylesPart();
        }
        auto stylesheet = part ? part->GetTypedRootElement() : nullptr;
        if (!stylesheet)
        {
            return std::nullopt;
        }

        Context context;
        context.part = part;
        context.stylesheet = stylesheet;
        context.numberingFormats = FindFirstChild<Spreadsheet::NumberingFormats>(stylesheet);
        context.fonts = EnsureChild<Spreadsheet::Fonts>(stylesheet);
        context.fills = EnsureChild<Spreadsheet::Fills>(stylesheet);
        context.borders = EnsureChild<Spreadsheet::Borders>(stylesheet);
        context.styleFormats = EnsureChild<Spreadsheet::CellStyleFormats>(stylesheet);
        context.cellFormats = EnsureChild<Spreadsheet::CellFormats>(stylesheet);
        context.cellStyles = EnsureChild<Spreadsheet::CellStyles>(stylesheet);
        if (!context.fonts || !context.fills || !context.borders || !context.styleFormats ||
            !context.cellFormats || !context.cellStyles)
        {
            return std::nullopt;
        }
        EnsureDefaults(context);
        return context;
    }

    static UInt32 Count(const ExcelDocument::Ptr& document)
    {
        const auto workbookPart = document ? document->GetWorkbookPart() : nullptr;
        const auto part = workbookPart ? workbookPart->GetWorkbookStylesPart() : nullptr;
        const auto stylesheet = part ? part->GetTypedRootElement() : nullptr;
        const auto formats = FindFirstChild<Spreadsheet::CellFormats>(stylesheet);
        return static_cast<UInt32>(FindChildren<Spreadsheet::CellFormat>(formats).size());
    }

    static StyleRegistrationResult GetOrAdd(const ExcelDocument::Ptr& document, const ExcelStyle& style)
    {
        std::string validationMessage;
        if (!Validate(style, validationMessage))
        {
            return {Error(RangeOperationError::InvalidStyle, std::move(validationMessage)), 0};
        }
        auto context = EnsureContext(document);
        if (!context)
        {
            return {Error(RangeOperationError::InvalidWorksheet,
                          "The workbook does not contain a usable workbook part."),
                    0};
        }
        const auto originalXml = context->part->GetXmlString();

        UInt32 numberFormatId = 0;
        if (style.NumberFormat)
        {
            if (!style.NumberFormat->FormatCode.empty())
            {
                const auto customId = GetOrAddNumberFormat(*context, style.NumberFormat->FormatCode);
                if (!customId)
                {
                    context->part->SetXmlString(originalXml);
                    return {Error(RangeOperationError::WriteFailed, "The custom number format could not be registered."), 0};
                }
                numberFormatId = *customId;
            }
            else
            {
                numberFormatId = style.NumberFormat->BuiltInId.value_or(0);
            }
        }

        const auto fontId = style.Font ? GetOrAddComponent<Spreadsheet::Fonts, Spreadsheet::Font>(
                                             context->fonts, [&](const auto& node)
                                             { BuildFont(node, *style.Font); })
                                       : std::optional<UInt32>{0};
        const auto fillId = style.Fill ? GetOrAddComponent<Spreadsheet::Fills, Spreadsheet::Fill>(
                                             context->fills, [&](const auto& node)
                                             { BuildFill(node, *style.Fill); })
                                       : std::optional<UInt32>{0};
        const auto borderId = style.Border ? GetOrAddComponent<Spreadsheet::Borders, Spreadsheet::Border>(
                                                 context->borders,
                                                 [&](const auto& node)
                                                 { BuildBorder(node, *style.Border); })
                                           : std::optional<UInt32>{0};
        if (!fontId || !fillId || !borderId)
        {
            context->part->SetXmlString(originalXml);
            return {Error(RangeOperationError::WriteFailed, "A style component could not be registered."), 0};
        }

        const auto styleIndex = GetOrAddCellFormat(*context, style, numberFormatId, *fontId, *fillId, *borderId);
        if (!styleIndex)
        {
            context->part->SetXmlString(originalXml);
            return {Error(RangeOperationError::WriteFailed, "The cell format could not be registered."), 0};
        }
        return {RangeOperationResult{RangeOperationError::None, {}, 1}, *styleIndex};
    }

    static RangeOperationResult ApplyToCell(const ExcelDocument::Ptr& document,
                                            Worksheet& worksheet,
                                            CellAddress address,
                                            UInt32 styleIndex)
    {
        if (!address.IsValid())
        {
            return Error(RangeOperationError::InvalidAddress, "The target cell address is invalid.");
        }
        if (styleIndex >= Count(document))
        {
            return Error(RangeOperationError::StyleNotFound, "The requested cell style index does not exist.");
        }
        const auto current = worksheet.GetCellValue(address);
        if (!current || (!worksheet.ContainsCell(address) && !worksheet.SetCellValue(address, ExcelCellValue::Blank())))
        {
            return Error(RangeOperationError::InvalidWorksheet, "The target worksheet cannot materialize the cell.");
        }
        const auto cell = FindCell(worksheet, address);
        if (!cell)
        {
            return Error(RangeOperationError::WriteFailed, "The target cell could not be located after materialization.");
        }
        cell->SetStyleIndex(UInt32Value(styleIndex));
        return RangeOperationResult{RangeOperationError::None, {}, 1};
    }

    static std::optional<ExcelStyle> GetStyle(const ExcelDocument::Ptr& document, UInt32 styleIndex)
    {
        const auto workbookPart = document ? document->GetWorkbookPart() : nullptr;
        const auto part = workbookPart ? workbookPart->GetWorkbookStylesPart() : nullptr;
        const auto stylesheet = part ? part->GetTypedRootElement() : nullptr;
        const auto formats =
            stylesheet ? FindChildren<Spreadsheet::CellFormat>(FindFirstChild<Spreadsheet::CellFormats>(stylesheet))
                       : std::vector<std::shared_ptr<Spreadsheet::CellFormat>>{};
        if (formats.empty())
        {
            // A workbook that has never registered a style still has an
            // implicit default cell XF at index zero.
            return workbookPart && styleIndex == 0 ? std::optional<ExcelStyle>(ExcelStyle{}) : std::nullopt;
        }
        if (styleIndex >= formats.size())
        {
            return std::nullopt;
        }
        const auto& format = formats[styleIndex];

        ExcelStyle style;
        style.QuotePrefix = format->GetQuotePrefix().ValueOr(false);
        style.PivotButton = format->GetPivotButton().ValueOr(false);

        const auto numberFormatId = format->GetNumberFormatId().ValueOr(0);
        if (format->GetApplyNumberFormat().ValueOr(false) || numberFormatId != 0)
        {
            style.NumberFormat = ReadNumberFormat(stylesheet, numberFormatId);
        }
        const auto fontId = format->GetFontId().ValueOr(0);
        if (format->GetApplyFont().ValueOr(false) || fontId != 0)
        {
            if (const auto node = ComponentAt<Spreadsheet::Fonts, Spreadsheet::Font>(stylesheet, fontId))
            {
                style.Font = ReadFont(node);
            }
        }
        const auto fillId = format->GetFillId().ValueOr(0);
        if (format->GetApplyFill().ValueOr(false) || fillId != 0)
        {
            if (const auto node = ComponentAt<Spreadsheet::Fills, Spreadsheet::Fill>(stylesheet, fillId))
            {
                style.Fill = ReadFill(node);
            }
        }
        const auto borderId = format->GetBorderId().ValueOr(0);
        if (format->GetApplyBorder().ValueOr(false) || borderId != 0)
        {
            if (const auto node = ComponentAt<Spreadsheet::Borders, Spreadsheet::Border>(stylesheet, borderId))
            {
                style.Border = ReadBorder(node);
            }
        }
        if (const auto alignment = FindFirstChild<Spreadsheet::Alignment>(format))
        {
            style.Alignment = ReadAlignment(alignment);
        }
        if (const auto protection = FindFirstChild<Spreadsheet::Protection>(format))
        {
            style.Protection = ReadProtection(protection);
        }
        return style;
    }

    static std::optional<UInt32> CellStyleIndex(const Worksheet& worksheet, CellAddress address)
    {
        if (!address.IsValid() || !worksheet.GetLowLevelApi())
        {
            return std::nullopt;
        }
        const auto cell = FindCell(worksheet, address);
        return cell ? std::optional<UInt32>(cell->GetStyleIndex().ValueOr(0))
                    : std::optional<UInt32>(0);
    }

private:
    static RangeOperationResult Error(RangeOperationError error, std::string message)
    {
        return RangeOperationResult{error, std::move(message), 0};
    }

    static bool NodeHasName(const Pugi::xml_node& node, const OpenXmlQualifiedName& expected)
    {
        const std::string_view name(node.name());
        const auto separator = name.find(':');
        const auto local = separator == std::string_view::npos ? name : name.substr(separator + 1);
        if (local != expected.localName())
        {
            return false;
        }
        const auto prefix = separator == std::string_view::npos ? std::string_view{} : name.substr(0, separator);
        const auto uri = Xml::NamespaceResolver::LookupUriForPrefix(node, prefix);
        return uri && *uri == expected.namespaceUri();
    }

    template <typename T>
    static std::vector<std::shared_ptr<T>> FindChildren(const std::shared_ptr<OpenXMLElement>& parent)
    {
        std::vector<std::shared_ptr<T>> result;
        const auto parentNode = Detail::NodeOf(parent);
        for (auto child = parentNode.first_child(); child; child = child.next_sibling())
        {
            if (child.type() == Pugi::node_element && NodeHasName(child, T::StaticMetaClass()->QualifiedName()))
            {
                result.push_back(Detail::WrapNode<T>(child));
            }
        }
        return result;
    }

    template <typename T>
    static std::shared_ptr<T> FindFirstChild(const std::shared_ptr<OpenXMLElement>& parent)
    {
        const auto children = FindChildren<T>(parent);
        return children.empty() ? nullptr : children.front();
    }

    template <typename T>
    static std::shared_ptr<T> EnsureChild(const std::shared_ptr<Spreadsheet::Stylesheet>& stylesheet)
    {
        if (auto child = FindFirstChild<T>(stylesheet))
        {
            return child;
        }
        return stylesheet->AppendChild<T>();
    }

    static std::string Serialize(const std::shared_ptr<OpenXMLElement>& element)
    {
        const auto node = Detail::NodeOf(element);
        if (!node)
        {
            return {};
        }
        std::ostringstream stream;
        node.print(stream, "", Pugi::format_raw);
        return stream.str();
    }

    template <typename TCollection, typename TComponent, typename TBuilder>
    static std::optional<UInt32> GetOrAddComponent(const std::shared_ptr<TCollection>& collection,
                                                   TBuilder builder)
    {
        auto existing = FindChildren<TComponent>(collection);
        auto candidate = collection ? collection->template AppendChild<TComponent>() : nullptr;
        if (!candidate)
        {
            return std::nullopt;
        }
        builder(candidate);
        const auto signature = Serialize(candidate);
        for (UInt32 index = 0; index < existing.size(); ++index)
        {
            if (Serialize(existing[index]) == signature)
            {
                collection->RemoveChild(candidate);
                collection->SetCount(UInt32Value(static_cast<UInt32>(existing.size())));
                return index;
            }
        }
        collection->SetCount(UInt32Value(static_cast<UInt32>(existing.size() + 1)));
        return static_cast<UInt32>(existing.size());
    }

    static void EnsureDefaults(Context& context)
    {
        if (FindChildren<Spreadsheet::Font>(context.fonts).empty())
        {
            ExcelFont font;
            font.Name = "Calibri";
            font.Size = 11.0;
            font.Color = ExcelColor::Theme(1);
            font.Family = 2;
            font.Scheme = ExcelFontScheme::Minor;
            GetOrAddComponent<Spreadsheet::Fonts, Spreadsheet::Font>(
                context.fonts, [&](const auto& node)
                { BuildFont(node, font); });
        }
        if (FindChildren<Spreadsheet::Fill>(context.fills).empty())
        {
            ExcelFill none;
            GetOrAddComponent<Spreadsheet::Fills, Spreadsheet::Fill>(
                context.fills, [&](const auto& node)
                { BuildFill(node, none); });
            ExcelFill gray;
            gray.Pattern = ExcelFillPattern::Gray125;
            GetOrAddComponent<Spreadsheet::Fills, Spreadsheet::Fill>(
                context.fills, [&](const auto& node)
                { BuildFill(node, gray); });
        }
        else if (FindChildren<Spreadsheet::Fill>(context.fills).size() == 1)
        {
            ExcelFill gray;
            gray.Pattern = ExcelFillPattern::Gray125;
            GetOrAddComponent<Spreadsheet::Fills, Spreadsheet::Fill>(
                context.fills, [&](const auto& node)
                { BuildFill(node, gray); });
        }
        if (FindChildren<Spreadsheet::Border>(context.borders).empty())
        {
            GetOrAddComponent<Spreadsheet::Borders, Spreadsheet::Border>(
                context.borders, [&](const auto& node)
                { BuildBorder(node, ExcelBorder{}); });
        }
        if (FindChildren<Spreadsheet::CellFormat>(context.styleFormats).empty())
        {
            auto format = context.styleFormats->AppendChild<Spreadsheet::CellFormat>();
            SetBaseFormatIds(format);
            context.styleFormats->SetCount(UInt32Value(1));
        }
        if (FindChildren<Spreadsheet::CellFormat>(context.cellFormats).empty())
        {
            auto format = context.cellFormats->AppendChild<Spreadsheet::CellFormat>();
            SetBaseFormatIds(format);
            format->SetFormatId(UInt32Value(0));
            context.cellFormats->SetCount(UInt32Value(1));
        }
        if (FindChildren<Spreadsheet::CellStyle>(context.cellStyles).empty())
        {
            auto normal = context.cellStyles->AppendChild<Spreadsheet::CellStyle>();
            normal->SetName(StringValue("Normal"));
            normal->SetFormatId(UInt32Value(0));
            normal->SetBuiltinId(UInt32Value(0));
            context.cellStyles->SetCount(UInt32Value(1));
        }
    }

    static void SetBaseFormatIds(const std::shared_ptr<Spreadsheet::CellFormat>& format)
    {
        format->SetNumberFormatId(UInt32Value(0));
        format->SetFontId(UInt32Value(0));
        format->SetFillId(UInt32Value(0));
        format->SetBorderId(UInt32Value(0));
    }

    static std::optional<UInt32> GetOrAddNumberFormat(Context& context, std::string_view formatCode)
    {
        if (!context.numberingFormats)
        {
            context.numberingFormats = context.stylesheet->InsertChild<Spreadsheet::NumberingFormats>(context.fonts);
        }
        if (!context.numberingFormats)
        {
            return std::nullopt;
        }
        auto formats = FindChildren<Spreadsheet::NumberingFormat>(context.numberingFormats);
        UInt32 nextId = 164;
        for (const auto& format : formats)
        {
            if (format->GetFormatCode().ToString() == formatCode)
            {
                return format->GetNumberFormatId().ValueOr(0);
            }
            nextId = std::max(nextId, format->GetNumberFormatId().ValueOr(163) + 1);
        }
        auto format = context.numberingFormats->AppendChild<Spreadsheet::NumberingFormat>();
        if (!format)
        {
            return std::nullopt;
        }
        format->SetNumberFormatId(UInt32Value(nextId));
        format->SetFormatCode(StringValue(std::string(formatCode)));
        context.numberingFormats->SetCount(UInt32Value(static_cast<UInt32>(formats.size() + 1)));
        return nextId;
    }

    static std::optional<UInt32> GetOrAddCellFormat(Context& context,
                                                    const ExcelStyle& style,
                                                    UInt32 numberFormatId,
                                                    UInt32 fontId,
                                                    UInt32 fillId,
                                                    UInt32 borderId)
    {
        auto existing = FindChildren<Spreadsheet::CellFormat>(context.cellFormats);
        auto format = context.cellFormats->AppendChild<Spreadsheet::CellFormat>();
        if (!format)
        {
            return std::nullopt;
        }
        format->SetNumberFormatId(UInt32Value(numberFormatId));
        format->SetFontId(UInt32Value(fontId));
        format->SetFillId(UInt32Value(fillId));
        format->SetBorderId(UInt32Value(borderId));
        format->SetFormatId(UInt32Value(0));
        if (style.QuotePrefix)
        {
            format->SetQuotePrefix(BooleanValue(true));
        }
        if (style.PivotButton)
        {
            format->SetPivotButton(BooleanValue(true));
        }
        if (style.NumberFormat)
        {
            format->SetApplyNumberFormat(BooleanValue(true));
        }
        if (style.Font)
        {
            format->SetApplyFont(BooleanValue(true));
        }
        if (style.Fill)
        {
            format->SetApplyFill(BooleanValue(true));
        }
        if (style.Border)
        {
            format->SetApplyBorder(BooleanValue(true));
        }
        if (style.Alignment)
        {
            format->SetApplyAlignment(BooleanValue(true));
            BuildAlignment(format->AppendChild<Spreadsheet::Alignment>(), *style.Alignment);
        }
        if (style.Protection)
        {
            format->SetApplyProtection(BooleanValue(true));
            BuildProtection(format->AppendChild<Spreadsheet::Protection>(), *style.Protection);
        }
        const auto signature = Serialize(format);
        for (UInt32 index = 0; index < existing.size(); ++index)
        {
            if (Serialize(existing[index]) == signature)
            {
                context.cellFormats->RemoveChild(format);
                context.cellFormats->SetCount(UInt32Value(static_cast<UInt32>(existing.size())));
                return index;
            }
        }
        context.cellFormats->SetCount(UInt32Value(static_cast<UInt32>(existing.size() + 1)));
        return static_cast<UInt32>(existing.size());
    }

    template <typename TCollection, typename TComponent>
    static std::shared_ptr<TComponent> ComponentAt(const std::shared_ptr<Spreadsheet::Stylesheet>& stylesheet,
                                                   UInt32 index)
    {
        const auto components = FindChildren<TComponent>(FindFirstChild<TCollection>(stylesheet));
        return index < components.size() ? components[index] : nullptr;
    }

    static ExcelNumberFormat ReadNumberFormat(const std::shared_ptr<Spreadsheet::Stylesheet>& stylesheet,
                                              UInt32 numberFormatId)
    {
        const auto container = FindFirstChild<Spreadsheet::NumberingFormats>(stylesheet);
        for (const auto& format : FindChildren<Spreadsheet::NumberingFormat>(container))
        {
            if (format->GetNumberFormatId().ValueOr(0) == numberFormatId)
            {
                return ExcelNumberFormat{std::nullopt, format->GetFormatCode().ToString()};
            }
        }
        return ExcelNumberFormat{numberFormatId, {}};
    }

    template <typename TColor>
    static std::optional<ExcelColor> ReadColor(const std::shared_ptr<TColor>& node)
    {
        if (!node)
        {
            return std::nullopt;
        }
        ExcelColor color;
        if (node->GetRgb().IsDefined())
        {
            color.Kind = ExcelColorKind::Rgb;
            color.Argb = node->GetRgb().ToString();
        }
        else if (node->GetTheme().IsDefined())
        {
            color.Kind = ExcelColorKind::Theme;
            color.Index = node->GetTheme().Value();
        }
        else if (node->GetIndexed().IsDefined())
        {
            color.Kind = ExcelColorKind::Indexed;
            color.Index = node->GetIndexed().Value();
        }
        else if (node->GetAuto().IsDefined())
        {
            color.Kind = ExcelColorKind::Automatic;
        }
        else
        {
            return std::nullopt;
        }
        if (node->GetTint().IsDefined())
        {
            color.Tint = node->GetTint().Value();
        }
        return color;
    }

    static ExcelFont ReadFont(const std::shared_ptr<Spreadsheet::Font>& node)
    {
        ExcelFont font;
        font.Bold = ReadBooleanProperty<Spreadsheet::Bold>(node);
        font.Italic = ReadBooleanProperty<Spreadsheet::Italic>(node);
        font.Strike = ReadBooleanProperty<Spreadsheet::Strike>(node);
        font.Outline = ReadBooleanProperty<Spreadsheet::Outline>(node);
        font.Shadow = ReadBooleanProperty<Spreadsheet::Shadow>(node);
        font.Condense = ReadBooleanProperty<Spreadsheet::Condense>(node);
        font.Extend = ReadBooleanProperty<Spreadsheet::Extend>(node);
        if (const auto underline = FindFirstChild<Spreadsheet::Underline>(node))
        {
            // An <u/> element without @val means single underline.
            font.Underline = underline->GetVal().IsDefined()
                                 ? MapBack(underline->GetVal().Value().GetValue(), ExcelUnderlineStyle::None)
                                 : ExcelUnderlineStyle::Single;
        }
        if (const auto vertical = FindFirstChild<Spreadsheet::VerticalTextAlignment>(node))
        {
            font.VerticalAlignment = vertical->GetVal().IsDefined()
                                         ? MapBack(vertical->GetVal().Value().GetValue(),
                                                   ExcelFontVerticalAlignment::Baseline)
                                         : ExcelFontVerticalAlignment::Baseline;
        }
        if (const auto size = FindFirstChild<Spreadsheet::FontSize>(node); size && size->GetVal().IsDefined())
        {
            font.Size = size->GetVal().Value();
        }
        font.Color = ReadColor(FindFirstChild<Spreadsheet::Color>(node));
        if (const auto name = FindFirstChild<Spreadsheet::FontName>(node); name && name->GetVal().IsDefined())
        {
            font.Name = name->GetVal().ToString();
        }
        if (const auto family = FindFirstChild<Spreadsheet::FontFamilyNumbering>(node);
            family && family->GetVal().IsDefined())
        {
            font.Family = family->GetVal().Value();
        }
        if (const auto charset = FindFirstChild<Spreadsheet::FontCharSet>(node); charset && charset->GetVal().IsDefined())
        {
            font.CharacterSet = charset->GetVal().Value();
        }
        if (const auto scheme = FindFirstChild<Spreadsheet::FontScheme>(node); scheme && scheme->GetVal().IsDefined())
        {
            font.Scheme = MapBack(scheme->GetVal().Value().GetValue(), ExcelFontScheme::None);
        }
        return font;
    }

    static ExcelFill ReadFill(const std::shared_ptr<Spreadsheet::Fill>& node)
    {
        ExcelFill fill;
        if (const auto pattern = FindFirstChild<Spreadsheet::PatternFill>(node))
        {
            fill.Kind = ExcelFillKind::Pattern;
            fill.Pattern = pattern->GetPatternType().IsDefined()
                               ? MapBack(pattern->GetPatternType().Value().GetValue(), ExcelFillPattern::None)
                               : ExcelFillPattern::None;
            fill.Foreground = ReadColor(FindFirstChild<Spreadsheet::ForegroundColor>(pattern));
            fill.Background = ReadColor(FindFirstChild<Spreadsheet::BackgroundColor>(pattern));
            return fill;
        }
        const auto gradient = FindFirstChild<Spreadsheet::GradientFill>(node);
        if (!gradient)
        {
            return fill;
        }
        const bool isPath = gradient->GetType().IsDefined() &&
                            gradient->GetType().Value().GetValue() == Spreadsheet::GradientValues::Path;
        fill.Kind = isPath ? ExcelFillKind::PathGradient : ExcelFillKind::LinearGradient;
        fill.Degree = gradient->GetDegree().ValueOr(0.0);
        fill.Left = gradient->GetLeft().ValueOr(0.0);
        fill.Right = gradient->GetRight().ValueOr(0.0);
        fill.Top = gradient->GetTop().ValueOr(0.0);
        fill.Bottom = gradient->GetBottom().ValueOr(0.0);
        for (const auto& stop : FindChildren<Spreadsheet::GradientStop>(gradient))
        {
            ExcelGradientStop parsed;
            parsed.Position = stop->GetPosition().ValueOr(0.0);
            parsed.Color = ReadColor(FindFirstChild<Spreadsheet::Color>(stop)).value_or(ExcelColor::Automatic());
            fill.GradientStops.push_back(parsed);
        }
        return fill;
    }

    template <typename TSide>
    static ExcelBorderSide ReadBorderSide(const std::shared_ptr<Spreadsheet::Border>& border)
    {
        ExcelBorderSide side;
        const auto node = FindFirstChild<TSide>(border);
        if (!node)
        {
            return side;
        }
        side.Style = node->GetStyle().IsDefined() ? MapBack(node->GetStyle().Value().GetValue(), ExcelBorderStyle::None)
                                                  : ExcelBorderStyle::None;
        side.Color = ReadColor(FindFirstChild<Spreadsheet::Color>(node));
        return side;
    }

    static ExcelBorder ReadBorder(const std::shared_ptr<Spreadsheet::Border>& node)
    {
        ExcelBorder border;
        border.DiagonalUp = node->GetDiagonalUp().ValueOr(false);
        border.DiagonalDown = node->GetDiagonalDown().ValueOr(false);
        border.Outline = node->GetOutline().ValueOr(true);
        border.Left = ReadBorderSide<Spreadsheet::LeftBorder>(node);
        border.Right = ReadBorderSide<Spreadsheet::RightBorder>(node);
        border.Top = ReadBorderSide<Spreadsheet::TopBorder>(node);
        border.Bottom = ReadBorderSide<Spreadsheet::BottomBorder>(node);
        border.Diagonal = ReadBorderSide<Spreadsheet::DiagonalBorder>(node);
        border.Vertical = ReadBorderSide<Spreadsheet::VerticalBorder>(node);
        border.Horizontal = ReadBorderSide<Spreadsheet::HorizontalBorder>(node);
        return border;
    }

    static ExcelAlignment ReadAlignment(const std::shared_ptr<Spreadsheet::Alignment>& node)
    {
        ExcelAlignment alignment;
        if (node->GetHorizontal().IsDefined())
        {
            alignment.Horizontal =
                MapBack(node->GetHorizontal().Value().GetValue(), ExcelHorizontalAlignment::General);
        }
        if (node->GetVertical().IsDefined())
        {
            alignment.Vertical = MapBack(node->GetVertical().Value().GetValue(), ExcelVerticalAlignment::Top);
        }
        if (node->GetTextRotation().IsDefined())
        {
            alignment.TextRotation = node->GetTextRotation().Value();
        }
        if (node->GetWrapText().IsDefined())
        {
            alignment.WrapText = node->GetWrapText().Value();
        }
        if (node->GetIndent().IsDefined())
        {
            alignment.Indent = node->GetIndent().Value();
        }
        if (node->GetRelativeIndent().IsDefined())
        {
            alignment.RelativeIndent = node->GetRelativeIndent().Value();
        }
        if (node->GetJustifyLastLine().IsDefined())
        {
            alignment.JustifyLastLine = node->GetJustifyLastLine().Value();
        }
        if (node->GetShrinkToFit().IsDefined())
        {
            alignment.ShrinkToFit = node->GetShrinkToFit().Value();
        }
        if (node->GetReadingOrder().IsDefined())
        {
            alignment.ReadingOrder = node->GetReadingOrder().Value();
        }
        return alignment;
    }

    static ExcelProtection ReadProtection(const std::shared_ptr<Spreadsheet::Protection>& node)
    {
        ExcelProtection protection;
        if (node->GetLocked().IsDefined())
        {
            protection.Locked = node->GetLocked().Value();
        }
        if (node->GetHidden().IsDefined())
        {
            protection.Hidden = node->GetHidden().Value();
        }
        return protection;
    }

    template <typename TProperty>
    static bool ReadBooleanProperty(const std::shared_ptr<Spreadsheet::Font>& font)
    {
        const auto node = FindFirstChild<TProperty>(font);
        // A boolean font property element without @val means "true".
        return node && node->GetVal().ValueOr(true);
    }

    static void BuildFont(const std::shared_ptr<Spreadsheet::Font>& node, const ExcelFont& font)
    {
        if (font.Bold)
        {
            node->AppendChild<Spreadsheet::Bold>()->SetVal(BooleanValue(true));
        }
        if (font.Italic)
        {
            node->AppendChild<Spreadsheet::Italic>()->SetVal(BooleanValue(true));
        }
        if (font.Strike)
        {
            node->AppendChild<Spreadsheet::Strike>()->SetVal(BooleanValue(true));
        }
        if (font.Condense)
        {
            node->AppendChild<Spreadsheet::Condense>()->SetVal(BooleanValue(true));
        }
        if (font.Extend)
        {
            node->AppendChild<Spreadsheet::Extend>()->SetVal(BooleanValue(true));
        }
        if (font.Outline)
        {
            node->AppendChild<Spreadsheet::Outline>()->SetVal(BooleanValue(true));
        }
        if (font.Shadow)
        {
            node->AppendChild<Spreadsheet::Shadow>()->SetVal(BooleanValue(true));
        }
        if (font.Underline != ExcelUnderlineStyle::None)
        {
            node->AppendChild<Spreadsheet::Underline>()->SetVal(EnumValue<Spreadsheet::UnderlineValues>(Map(font.Underline)));
        }
        if (font.VerticalAlignment != ExcelFontVerticalAlignment::Baseline)
        {
            node->AppendChild<Spreadsheet::VerticalTextAlignment>()->SetVal(
                EnumValue<Spreadsheet::VerticalAlignmentRunValues>(Map(font.VerticalAlignment)));
        }
        if (font.Size)
        {
            node->AppendChild<Spreadsheet::FontSize>()->SetVal(DoubleValue(*font.Size));
        }
        if (font.Color)
        {
            ApplyColor(node->AppendChild<Spreadsheet::Color>(), *font.Color);
        }
        if (font.Name)
        {
            node->AppendChild<Spreadsheet::FontName>()->SetVal(StringValue(*font.Name));
        }
        if (font.Family)
        {
            node->AppendChild<Spreadsheet::FontFamilyNumbering>()->SetVal(Int32Value(*font.Family));
        }
        if (font.CharacterSet)
        {
            node->AppendChild<Spreadsheet::FontCharSet>()->SetVal(Int32Value(*font.CharacterSet));
        }
        if (font.Scheme != ExcelFontScheme::None)
        {
            node->AppendChild<Spreadsheet::FontScheme>()->SetVal(EnumValue<Spreadsheet::FontSchemeValues>(Map(font.Scheme)));
        }
    }

    static void BuildFill(const std::shared_ptr<Spreadsheet::Fill>& node, const ExcelFill& fill)
    {
        if (fill.Kind == ExcelFillKind::Pattern)
        {
            auto pattern = node->AppendChild<Spreadsheet::PatternFill>();
            pattern->SetPatternType(EnumValue<Spreadsheet::PatternValues>(Map(fill.Pattern)));
            if (fill.Foreground)
            {
                ApplyColor(pattern->AppendChild<Spreadsheet::ForegroundColor>(), *fill.Foreground);
            }
            if (fill.Background)
            {
                ApplyColor(pattern->AppendChild<Spreadsheet::BackgroundColor>(), *fill.Background);
            }
            return;
        }
        auto gradient = node->AppendChild<Spreadsheet::GradientFill>();
        gradient->SetType(EnumValue<Spreadsheet::GradientValues>(
            fill.Kind == ExcelFillKind::LinearGradient ? Spreadsheet::GradientValues::Linear
                                                       : Spreadsheet::GradientValues::Path));
        if (fill.Kind == ExcelFillKind::LinearGradient)
        {
            gradient->SetDegree(DoubleValue(fill.Degree));
        }
        else
        {
            gradient->SetLeft(DoubleValue(fill.Left));
            gradient->SetRight(DoubleValue(fill.Right));
            gradient->SetTop(DoubleValue(fill.Top));
            gradient->SetBottom(DoubleValue(fill.Bottom));
        }
        for (const auto& stopDefinition : fill.GradientStops)
        {
            auto stop = gradient->AppendChild<Spreadsheet::GradientStop>();
            stop->SetPosition(DoubleValue(stopDefinition.Position));
            ApplyColor(stop->AppendChild<Spreadsheet::Color>(), stopDefinition.Color);
        }
    }

    template <typename TSide>
    static void BuildBorderSide(const std::shared_ptr<Spreadsheet::Border>& border, const ExcelBorderSide& side)
    {
        auto node = border->AppendChild<TSide>();
        node->SetStyle(EnumValue<Spreadsheet::BorderStyleValues>(Map(side.Style)));
        if (side.Color)
        {
            ApplyColor(node->template AppendChild<Spreadsheet::Color>(), *side.Color);
        }
    }

    static void BuildBorder(const std::shared_ptr<Spreadsheet::Border>& node, const ExcelBorder& border)
    {
        node->SetDiagonalUp(BooleanValue(border.DiagonalUp));
        node->SetDiagonalDown(BooleanValue(border.DiagonalDown));
        node->SetOutline(BooleanValue(border.Outline));
        BuildBorderSide<Spreadsheet::LeftBorder>(node, border.Left);
        BuildBorderSide<Spreadsheet::RightBorder>(node, border.Right);
        BuildBorderSide<Spreadsheet::TopBorder>(node, border.Top);
        BuildBorderSide<Spreadsheet::BottomBorder>(node, border.Bottom);
        BuildBorderSide<Spreadsheet::DiagonalBorder>(node, border.Diagonal);
        BuildBorderSide<Spreadsheet::VerticalBorder>(node, border.Vertical);
        BuildBorderSide<Spreadsheet::HorizontalBorder>(node, border.Horizontal);
    }

    static void BuildAlignment(const std::shared_ptr<Spreadsheet::Alignment>& node, const ExcelAlignment& value)
    {
        if (value.Horizontal)
        {
            node->SetHorizontal(EnumValue<Spreadsheet::HorizontalAlignmentValues>(Map(*value.Horizontal)));
        }
        if (value.Vertical)
        {
            node->SetVertical(EnumValue<Spreadsheet::VerticalAlignmentValues>(Map(*value.Vertical)));
        }
        if (value.TextRotation)
        {
            node->SetTextRotation(UInt32Value(*value.TextRotation));
        }
        if (value.WrapText)
        {
            node->SetWrapText(BooleanValue(*value.WrapText));
        }
        if (value.Indent)
        {
            node->SetIndent(UInt32Value(*value.Indent));
        }
        if (value.RelativeIndent)
        {
            node->SetRelativeIndent(Int32Value(*value.RelativeIndent));
        }
        if (value.JustifyLastLine)
        {
            node->SetJustifyLastLine(BooleanValue(*value.JustifyLastLine));
        }
        if (value.ShrinkToFit)
        {
            node->SetShrinkToFit(BooleanValue(*value.ShrinkToFit));
        }
        if (value.ReadingOrder)
        {
            node->SetReadingOrder(UInt32Value(*value.ReadingOrder));
        }
    }

    static void BuildProtection(const std::shared_ptr<Spreadsheet::Protection>& node, const ExcelProtection& value)
    {
        if (value.Locked)
        {
            node->SetLocked(BooleanValue(*value.Locked));
        }
        if (value.Hidden)
        {
            node->SetHidden(BooleanValue(*value.Hidden));
        }
    }

    template <typename TColor>
    static void ApplyColor(const std::shared_ptr<TColor>& node, const ExcelColor& color)
    {
        if (color.Kind == ExcelColorKind::Automatic)
        {
            node->SetAuto(BooleanValue(true));
        }
        else if (color.Kind == ExcelColorKind::Rgb)
        {
            node->SetRgb(HexBinaryValue(ParseHex(color.Argb)));
        }
        else if (color.Kind == ExcelColorKind::Theme)
        {
            node->SetTheme(UInt32Value(color.Index));
        }
        else
        {
            node->SetIndexed(UInt32Value(color.Index));
        }
        if (color.Tint)
        {
            node->SetTint(DoubleValue(*color.Tint));
        }
    }

    static std::vector<Byte> ParseHex(std::string_view text)
    {
        std::vector<Byte> bytes;
        for (Size index = 0; index + 1 < text.size(); index += 2)
        {
            bytes.push_back(static_cast<UInt8>((HexDigit(text[index]) << 4) | HexDigit(text[index + 1])));
        }
        return bytes;
    }

    static UInt8 HexDigit(char value)
    {
        if (value >= '0' && value <= '9')
        {
            return static_cast<UInt8>(value - '0');
        }
        value = AsciiText::ToUpper(value);
        return static_cast<UInt8>(10 + value - 'A');
    }

    static bool Validate(const ExcelStyle& style, std::string& message)
    {
        const auto validColor = [](const ExcelColor& color)
        {
            const bool rgbValid = color.Kind != ExcelColorKind::Rgb ||
                                  (color.Argb.size() == 8 && std::all_of(color.Argb.begin(), color.Argb.end(), [](char ch)
                                                                         { return AsciiText::IsHexDigit(ch); }));
            return rgbValid && (!color.Tint || (*color.Tint >= -1.0 && *color.Tint <= 1.0));
        };
        if (style.Font && ((style.Font->Size && *style.Font->Size <= 0.0) ||
                           (style.Font->Color && !validColor(*style.Font->Color))))
        {
            message = "Font size must be positive and font colors must contain valid ARGB/tint values.";
            return false;
        }
        if (style.Fill)
        {
            if ((style.Fill->Foreground && !validColor(*style.Fill->Foreground)) ||
                (style.Fill->Background && !validColor(*style.Fill->Background)))
            {
                message = "Fill colors must contain valid ARGB/tint values.";
                return false;
            }
            if (style.Fill->Kind != ExcelFillKind::Pattern)
            {
                if (style.Fill->GradientStops.size() < 2 ||
                    !std::is_sorted(style.Fill->GradientStops.begin(), style.Fill->GradientStops.end(),
                                    [](const auto& left, const auto& right)
                                    { return left.Position < right.Position; }) ||
                    std::any_of(style.Fill->GradientStops.begin(), style.Fill->GradientStops.end(),
                                [&](const auto& stop)
                                { return stop.Position < 0.0 || stop.Position > 1.0 || !validColor(stop.Color); }))
                {
                    message = "Gradient fills require at least two ordered stops with positions in [0, 1].";
                    return false;
                }
            }
        }
        if (style.Alignment && ((style.Alignment->TextRotation && *style.Alignment->TextRotation > 180) ||
                                (style.Alignment->ReadingOrder && *style.Alignment->ReadingOrder > 2)))
        {
            message = "Text rotation must be at most 180 and reading order must be 0, 1, or 2.";
            return false;
        }
        return true;
    }

    static std::shared_ptr<Spreadsheet::Cell> FindCell(const Worksheet& worksheet, CellAddress address)
    {
        const auto root = worksheet.GetLowLevelApi();
        for (const auto& cell : root ? root->Descendants<Spreadsheet::Cell>()
                                     : std::vector<std::shared_ptr<Spreadsheet::Cell>>{})
        {
            if (cell && cell->GetCellReference().ToString() == address.ToA1())
            {
                return cell;
            }
        }
        return nullptr;
    }

    static Spreadsheet::UnderlineValues::Value Map(ExcelUnderlineStyle value)
    {
        using V = Spreadsheet::UnderlineValues;
        switch (value)
        {
            case ExcelUnderlineStyle::Single:
                return V::Single;
            case ExcelUnderlineStyle::Double:
                return V::Double;
            case ExcelUnderlineStyle::SingleAccounting:
                return V::SingleAccounting;
            case ExcelUnderlineStyle::DoubleAccounting:
                return V::DoubleAccounting;
            default:
                return V::None;
        }
    }
    static Spreadsheet::FontSchemeValues::Value Map(ExcelFontScheme value)
    {
        return value == ExcelFontScheme::Major ? Spreadsheet::FontSchemeValues::Major : Spreadsheet::FontSchemeValues::Minor;
    }
    static Spreadsheet::VerticalAlignmentRunValues::Value Map(ExcelFontVerticalAlignment value)
    {
        return value == ExcelFontVerticalAlignment::Superscript ? Spreadsheet::VerticalAlignmentRunValues::Superscript : Spreadsheet::VerticalAlignmentRunValues::Subscript;
    }
    static Spreadsheet::PatternValues::Value Map(ExcelFillPattern value)
    {
        return static_cast<Spreadsheet::PatternValues::Value>(static_cast<int>(Spreadsheet::PatternValues::None) + static_cast<int>(value));
    }
    static Spreadsheet::BorderStyleValues::Value Map(ExcelBorderStyle value)
    {
        return static_cast<Spreadsheet::BorderStyleValues::Value>(static_cast<int>(Spreadsheet::BorderStyleValues::None) + static_cast<int>(value));
    }
    static Spreadsheet::HorizontalAlignmentValues::Value Map(ExcelHorizontalAlignment value)
    {
        return static_cast<Spreadsheet::HorizontalAlignmentValues::Value>(static_cast<int>(Spreadsheet::HorizontalAlignmentValues::General) + static_cast<int>(value));
    }
    static Spreadsheet::VerticalAlignmentValues::Value Map(ExcelVerticalAlignment value)
    {
        return static_cast<Spreadsheet::VerticalAlignmentValues::Value>(static_cast<int>(Spreadsheet::VerticalAlignmentValues::Top) + static_cast<int>(value));
    }

    /**
     * Inverts an offset-compatible enum mapping.
     *
     * The high-level enums above are declared in the same order as their
     * SpreadsheetML counterparts, so the forward mapping is a constant offset
     * and can be inverted arithmetically. Values outside the mapped window
     * (including `NotDefinedEnumValue` and `InvalidEnumValue`) fall back.
     */
    template <typename TDomValue, typename TExcelEnum>
    static TExcelEnum MapBackOffset(TDomValue value, TDomValue base, TExcelEnum fallback, int count)
    {
        const int offset = static_cast<int>(value) - static_cast<int>(base);
        return offset >= 0 && offset < count ? static_cast<TExcelEnum>(offset) : fallback;
    }

    static ExcelFillPattern MapBack(Spreadsheet::PatternValues::Value value, ExcelFillPattern fallback)
    {
        return MapBackOffset(value, Spreadsheet::PatternValues::None, fallback, 19);
    }
    static ExcelBorderStyle MapBack(Spreadsheet::BorderStyleValues::Value value, ExcelBorderStyle fallback)
    {
        return MapBackOffset(value, Spreadsheet::BorderStyleValues::None, fallback, 14);
    }
    static ExcelHorizontalAlignment MapBack(Spreadsheet::HorizontalAlignmentValues::Value value, ExcelHorizontalAlignment fallback)
    {
        return MapBackOffset(value, Spreadsheet::HorizontalAlignmentValues::General, fallback, 8);
    }
    static ExcelVerticalAlignment MapBack(Spreadsheet::VerticalAlignmentValues::Value value, ExcelVerticalAlignment fallback)
    {
        return MapBackOffset(value, Spreadsheet::VerticalAlignmentValues::Top, fallback, 5);
    }
    static ExcelFontVerticalAlignment MapBack(Spreadsheet::VerticalAlignmentRunValues::Value value, ExcelFontVerticalAlignment fallback)
    {
        return MapBackOffset(value, Spreadsheet::VerticalAlignmentRunValues::Baseline, fallback, 3);
    }
    static ExcelFontScheme MapBack(Spreadsheet::FontSchemeValues::Value value, ExcelFontScheme fallback)
    {
        return MapBackOffset(value, Spreadsheet::FontSchemeValues::None, fallback, 3);
    }
    /** SpreadsheetML declares `ST_UnderlineValues` in a different order, so this mapping is explicit. */
    static ExcelUnderlineStyle MapBack(Spreadsheet::UnderlineValues::Value value, ExcelUnderlineStyle fallback)
    {
        using V = Spreadsheet::UnderlineValues;
        switch (value)
        {
            case V::Single:
                return ExcelUnderlineStyle::Single;
            case V::Double:
                return ExcelUnderlineStyle::Double;
            case V::SingleAccounting:
                return ExcelUnderlineStyle::SingleAccounting;
            case V::DoubleAccounting:
                return ExcelUnderlineStyle::DoubleAccounting;
            case V::None:
                return ExcelUnderlineStyle::None;
            default:
                return fallback;
        }
    }
};

StyleRepository::StyleRepository(ExcelDocument::Ptr document)
    : m_document(std::move(document)) {}
bool StyleRepository::IsValid() const noexcept
{
    return m_document != nullptr;
}
UInt32 StyleRepository::Count() const
{
    return StyleRepositoryImplementation::Count(m_document);
}
StyleRegistrationResult StyleRepository::GetOrAdd(const ExcelStyle& style)
{
    return StyleRepositoryImplementation::GetOrAdd(m_document, style);
}
RangeOperationResult StyleRepository::ApplyToCell(Worksheet& worksheet, CellAddress address, UInt32 styleIndex)
{
    return StyleRepositoryImplementation::ApplyToCell(m_document, worksheet, address, styleIndex);
}
RangeOperationResult StyleRepository::ApplyToRange(Worksheet& worksheet, CellRange range, UInt32 styleIndex)
{
    if (!range.IsValid())
    {
        return RangeOperationResult{RangeOperationError::InvalidAddress, "The target range is invalid.", 0};
    }
    if (styleIndex >= Count())
    {
        return RangeOperationResult{RangeOperationError::StyleNotFound, "The requested cell style index does not exist.", 0};
    }
    const auto part = worksheet.GetPart();
    if (!part)
    {
        return RangeOperationResult{RangeOperationError::InvalidWorksheet, "The worksheet is detached.", 0};
    }
    const auto originalXml = part->GetXmlString();
    Size affected = 0;
    for (UInt32 row = range.First().Row().Value(); row <= range.Last().Row().Value(); ++row)
    {
        for (UInt32 column = range.First().Column().Value(); column <= range.Last().Column().Value(); ++column)
        {
            const auto result = ApplyToCell(worksheet, *CellAddress::TryCreate(row, column), styleIndex);
            if (!result)
            {
                part->SetXmlString(originalXml);
                return RangeOperationResult{RangeOperationError::WriteFailed, "A range style write failed; the worksheet was restored.", 0};
            }
            ++affected;
        }
    }
    return RangeOperationResult{RangeOperationError::None, {}, affected};
}
std::optional<UInt32> StyleRepository::CellStyleIndex(const Worksheet& worksheet, CellAddress address) const
{
    return StyleRepositoryImplementation::CellStyleIndex(worksheet, address);
}
std::optional<ExcelStyle> StyleRepository::GetStyle(UInt32 styleIndex) const
{
    return StyleRepositoryImplementation::GetStyle(m_document, styleIndex);
}
std::optional<ExcelStyle> StyleRepository::GetCellStyle(const Worksheet& worksheet, CellAddress address) const
{
    const auto index = CellStyleIndex(worksheet, address);
    return index ? GetStyle(*index) : std::nullopt;
}

} // namespace ExyokiOffice::Excel
