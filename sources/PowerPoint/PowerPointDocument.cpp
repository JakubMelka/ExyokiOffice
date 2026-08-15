// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "ExyokiOffice/PowerPoint/PowerPointDocument.hpp"

#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/Drawing.hpp"
#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/Drawing/Charts.hpp"
#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/Drawing/Diagrams.hpp"
#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/Office2021/PowerPoint/Comment.hpp"
#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/Office2010/PowerPoint.hpp"
#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/Presentation.hpp"
#include "ExyokiOffice/OpenXmlSimpleTypes.hpp"
#include "ExyokiOffice/Packaging/PackageUtilities.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <unordered_set>

namespace ExyokiOffice::PowerPoint
{
namespace Presentation = ExyokiOffice::DocumentFormat::OpenXml::Presentation;
namespace Drawing = ExyokiOffice::DocumentFormat::OpenXml::Drawing;
namespace Charts = ExyokiOffice::DocumentFormat::OpenXml::Drawing::Charts;
namespace Diagrams = ExyokiOffice::DocumentFormat::OpenXml::Drawing::Diagrams;
namespace ModernComments = ExyokiOffice::DocumentFormat::OpenXml::Office2021::PowerPoint::Comment;
namespace PowerPoint2010 = ExyokiOffice::DocumentFormat::OpenXml::Office2010::PowerPoint;

class PresentationMeasurementHelpers
{
public:
    static MeasuringUnits FromEmu(Int64 value)
    {
        return MeasuringUnits(static_cast<Real>(value), MeasurementUnit::Emu);
    }

    static bool IsNonNegative(const MeasuringUnits& value)
    {
        const auto emu = value.ToEmu().GetValue();
        return std::isfinite(emu) && emu >= 0.0;
    }

    static std::optional<Int64> ToInt64Emu(const MeasuringUnits& value)
    {
        const auto emu = value.ToEmu().GetValue();
        const auto extended = static_cast<RealExtended>(emu);
        if (!std::isfinite(emu) ||
            extended < static_cast<RealExtended>(std::numeric_limits<Int64>::min()) ||
            extended > static_cast<RealExtended>(std::numeric_limits<Int64>::max()))
        {
            return std::nullopt;
        }
        return static_cast<Int64>(std::llround(emu));
    }

    static std::optional<Int32> ToInt32Emu(const MeasuringUnits& value)
    {
        const auto emu = ToInt64Emu(value);
        if (!emu || *emu < std::numeric_limits<Int32>::min() ||
            *emu > std::numeric_limits<Int32>::max())
        {
            return std::nullopt;
        }
        return static_cast<Int32>(*emu);
    }

    static MeasuringUnits FromHundredthPoint(Int32 value)
    {
        return MeasuringUnits(static_cast<Real>(value) / 100.0, MeasurementUnit::Point);
    }

    static std::optional<Int32> ToHundredthPoint(const MeasuringUnits& value, bool nonNegative = false)
    {
        const auto points = value.ToPt().GetValue();
        const auto scaled = static_cast<RealExtended>(points) * 100.0L;
        if (!std::isfinite(points) || (nonNegative && points < 0.0) ||
            scaled < static_cast<RealExtended>(std::numeric_limits<Int32>::min()) ||
            scaled > static_cast<RealExtended>(std::numeric_limits<Int32>::max()))
        {
            return std::nullopt;
        }
        return static_cast<Int32>(std::llround(scaled));
    }
};

class PresentationTextEffectHelpers
{
public:
    static MeasuringAngle FromDrawingMlAngle(Int32 value)
    {
        constexpr Real unitsPerDegree = 60000.0;
        return MeasuringAngle(static_cast<Real>(value) / unitsPerDegree, AngleUnit::Degree);
    }

    static std::optional<Int32> ToDrawingMlAngle(const MeasuringAngle& value)
    {
        constexpr RealExtended unitsPerDegree = 60000.0L;
        const Real degrees = value.ToDegrees().GetValue();
        const RealExtended scaled = static_cast<RealExtended>(degrees) * unitsPerDegree;
        if (!std::isfinite(degrees) ||
            scaled < static_cast<RealExtended>(std::numeric_limits<Int32>::min()) ||
            scaled > static_cast<RealExtended>(std::numeric_limits<Int32>::max()))
        {
            return std::nullopt;
        }
        return static_cast<Int32>(std::llround(scaled));
    }

    static std::optional<Color> ReadColor(const std::shared_ptr<OpenXMLElement>& parent)
    {
        auto rgb = parent ? parent->GetFirstChildOfType<Drawing::RgbColorModelHex>() : nullptr;
        return rgb ? Color::FromHexString(rgb->GetVal().ToString()) : std::nullopt;
    }

    static bool AppendColor(const std::shared_ptr<OpenXMLElement>& parent, const Color& color)
    {
        if (!parent || color.IsAuto())
        {
            return false;
        }
        auto rgb = parent->AppendChild<Drawing::RgbColorModelHex>();
        if (!rgb)
        {
            return false;
        }
        rgb->SetVal(OpenXmlSimpleValueConvertor::GetHexBinaryValueFromString(color.ToHexString()));
        return true;
    }

    static std::optional<Int64> Length(const MeasuringUnits& value)
    {
        if (!PresentationMeasurementHelpers::IsNonNegative(value))
        {
            return std::nullopt;
        }
        return PresentationMeasurementHelpers::ToInt64Emu(value);
    }

    static bool IsValid(const PresentationTextGlow& value)
    {
        return Length(value.Radius).has_value() && !value.ColorValue.IsAuto();
    }

    static bool IsValid(const PresentationTextShadow& value)
    {
        return Length(value.BlurRadius).has_value() && Length(value.Distance).has_value() &&
               !value.ColorValue.IsAuto() && ToDrawingMlAngle(value.Direction).has_value() &&
               ToDrawingMlAngle(value.HorizontalSkew).has_value() &&
               ToDrawingMlAngle(value.VerticalSkew).has_value() &&
               Drawing::RectangleAlignmentValues(value.Alignment).IsValid();
    }

    static bool IsValid(const PresentationTextReflection& value)
    {
        return Length(value.BlurRadius).has_value() && Length(value.Distance).has_value() &&
               ToDrawingMlAngle(value.Direction).has_value() &&
               ToDrawingMlAngle(value.FadeDirection).has_value() &&
               ToDrawingMlAngle(value.HorizontalSkew).has_value() &&
               ToDrawingMlAngle(value.VerticalSkew).has_value() &&
               Drawing::RectangleAlignmentValues(value.Alignment).IsValid();
    }

    static bool IsValid(const PresentationTextBevel& value)
    {
        return Length(value.Width).has_value() && Length(value.Height).has_value() &&
               Drawing::BevelPresetValues(value.Preset).IsValid();
    }

    static bool IsValid(const PresentationText3D& value)
    {
        return Length(value.Depth).has_value() && Length(value.ExtrusionHeight).has_value() &&
               Length(value.ContourWidth).has_value() &&
               Drawing::PresetMaterialTypeValues(value.Material).IsValid() &&
               Drawing::PresetCameraValues(value.Camera).IsValid() &&
               Drawing::LightRigValues(value.LightRig).IsValid() &&
               Drawing::LightRigDirectionValues(value.LightDirection).IsValid() &&
               (!value.ExtrusionColor || !value.ExtrusionColor->IsAuto()) &&
               (!value.ContourColor || !value.ContourColor->IsAuto()) &&
               (!value.TopBevel || IsValid(*value.TopBevel)) &&
               (!value.BottomBevel || IsValid(*value.BottomBevel));
    }

    // Reads the glow, shadow, and reflection children of an existing a:effectLst.
    // Glow and shadow require an explicit color and are otherwise skipped, matching
    // the way DrawingML omits a colorless effect.
    static std::optional<PresentationTextGlow> ReadGlow(const std::shared_ptr<OpenXMLElement>& effects)
    {
        auto glow = effects ? effects->GetFirstChildOfType<Drawing::Glow>() : nullptr;
        if (!glow)
        {
            return std::nullopt;
        }
        auto color = ReadColor(glow);
        if (!color)
        {
            return std::nullopt;
        }
        return PresentationTextGlow{PresentationMeasurementHelpers::FromEmu(glow->GetRadius().ValueOr(0)), *color};
    }

    static std::optional<PresentationTextShadow> ReadShadow(const std::shared_ptr<OpenXMLElement>& effects)
    {
        auto shadow = effects ? effects->GetFirstChildOfType<Drawing::OuterShadow>() : nullptr;
        if (!shadow)
        {
            return std::nullopt;
        }
        auto color = ReadColor(shadow);
        if (!color)
        {
            return std::nullopt;
        }
        PresentationTextShadow value;
        value.BlurRadius = PresentationMeasurementHelpers::FromEmu(shadow->GetBlurRadius().ValueOr(0));
        value.Distance = PresentationMeasurementHelpers::FromEmu(shadow->GetDistance().ValueOr(0));
        value.ColorValue = *color;
        value.Direction = FromDrawingMlAngle(shadow->GetDirection().ValueOr(0));
        value.HorizontalScale = shadow->GetHorizontalRatio().ValueOr(100000);
        value.VerticalScale = shadow->GetVerticalRatio().ValueOr(100000);
        value.HorizontalSkew = FromDrawingMlAngle(shadow->GetHorizontalSkew().ValueOr(0));
        value.VerticalSkew = FromDrawingMlAngle(shadow->GetVerticalSkew().ValueOr(0));
        value.Alignment = shadow->GetAlignment().ValueOr(Drawing::RectangleAlignmentValues::BottomRight).GetValue();
        value.RotateWithShape = shadow->GetRotateWithShape().ValueOr(true);
        return value;
    }

    static std::optional<PresentationTextReflection> ReadReflection(const std::shared_ptr<OpenXMLElement>& effects)
    {
        auto reflection = effects ? effects->GetFirstChildOfType<Drawing::Reflection>() : nullptr;
        if (!reflection)
        {
            return std::nullopt;
        }
        PresentationTextReflection value;
        value.BlurRadius = PresentationMeasurementHelpers::FromEmu(reflection->GetBlurRadius().ValueOr(0));
        value.Distance = PresentationMeasurementHelpers::FromEmu(reflection->GetDistance().ValueOr(0));
        value.StartOpacity = reflection->GetStartOpacity().ValueOr(100000);
        value.StartPosition = reflection->GetStartPosition().ValueOr(0);
        value.EndOpacity = reflection->GetEndAlpha().ValueOr(0);
        value.EndPosition = reflection->GetEndPosition().ValueOr(100000);
        value.Direction = FromDrawingMlAngle(reflection->GetDirection().ValueOr(5400000));
        value.FadeDirection = FromDrawingMlAngle(reflection->GetFadeDirection().ValueOr(5400000));
        value.HorizontalScale = reflection->GetHorizontalRatio().ValueOr(100000);
        value.VerticalScale = reflection->GetVerticalRatio().ValueOr(-100000);
        value.HorizontalSkew = FromDrawingMlAngle(reflection->GetHorizontalSkew().ValueOr(0));
        value.VerticalSkew = FromDrawingMlAngle(reflection->GetVerticalSkew().ValueOr(0));
        value.Alignment = reflection->GetAlignment().ValueOr(Drawing::RectangleAlignmentValues::Bottom).GetValue();
        value.RotateWithShape = reflection->GetRotateWithShape().ValueOr(true);
        return value;
    }

    // Appends the effect children to an a:effectLst. Callers validate the values
    // with IsValid() first, so the Length/ToDrawingMlAngle conversions cannot fail.
    static bool WriteGlow(const std::shared_ptr<OpenXMLElement>& effects, const PresentationTextGlow& value)
    {
        auto glow = effects->AppendChild<Drawing::Glow>();
        if (!glow || !AppendColor(glow, value.ColorValue))
        {
            return false;
        }
        glow->SetRadius(Int64Value(*Length(value.Radius)));
        return true;
    }

    static bool WriteShadow(const std::shared_ptr<OpenXMLElement>& effects, const PresentationTextShadow& value)
    {
        auto shadow = effects->AppendChild<Drawing::OuterShadow>();
        if (!shadow || !AppendColor(shadow, value.ColorValue))
        {
            return false;
        }
        shadow->SetBlurRadius(Int64Value(*Length(value.BlurRadius)));
        shadow->SetDistance(Int64Value(*Length(value.Distance)));
        shadow->SetDirection(Int32Value(*ToDrawingMlAngle(value.Direction)));
        shadow->SetHorizontalRatio(Int32Value(value.HorizontalScale));
        shadow->SetVerticalRatio(Int32Value(value.VerticalScale));
        shadow->SetHorizontalSkew(Int32Value(*ToDrawingMlAngle(value.HorizontalSkew)));
        shadow->SetVerticalSkew(Int32Value(*ToDrawingMlAngle(value.VerticalSkew)));
        shadow->SetAlignment(EnumValue<Drawing::RectangleAlignmentValues>(Drawing::RectangleAlignmentValues(value.Alignment)));
        shadow->SetRotateWithShape(BooleanValue(value.RotateWithShape));
        return true;
    }

    static bool WriteReflection(const std::shared_ptr<OpenXMLElement>& effects, const PresentationTextReflection& value)
    {
        auto reflection = effects->AppendChild<Drawing::Reflection>();
        if (!reflection)
        {
            return false;
        }
        reflection->SetBlurRadius(Int64Value(*Length(value.BlurRadius)));
        reflection->SetDistance(Int64Value(*Length(value.Distance)));
        reflection->SetStartOpacity(Int32Value(value.StartOpacity));
        reflection->SetStartPosition(Int32Value(value.StartPosition));
        reflection->SetEndAlpha(Int32Value(value.EndOpacity));
        reflection->SetEndPosition(Int32Value(value.EndPosition));
        reflection->SetDirection(Int32Value(*ToDrawingMlAngle(value.Direction)));
        reflection->SetFadeDirection(Int32Value(*ToDrawingMlAngle(value.FadeDirection)));
        reflection->SetHorizontalRatio(Int32Value(value.HorizontalScale));
        reflection->SetVerticalRatio(Int32Value(value.VerticalScale));
        reflection->SetHorizontalSkew(Int32Value(*ToDrawingMlAngle(value.HorizontalSkew)));
        reflection->SetVerticalSkew(Int32Value(*ToDrawingMlAngle(value.VerticalSkew)));
        reflection->SetAlignment(EnumValue<Drawing::RectangleAlignmentValues>(Drawing::RectangleAlignmentValues(value.Alignment)));
        reflection->SetRotateWithShape(BooleanValue(value.RotateWithShape));
        return true;
    }
};

std::shared_ptr<Presentation::SlideIdList> SlideIds(const PowerPointDocument::Ptr& document, bool create)
{
    auto part = document ? document->GetPresentationPart() : nullptr;
    auto root = part ? part->GetTypedRootElement() : nullptr;
    if (!root)
    {
        return nullptr;
    }
    auto list = root->GetFirstChildOfType<Presentation::SlideIdList>();
    return list || !create ? list : root->AppendChild<Presentation::SlideIdList>();
}

std::shared_ptr<Packaging::SlidePart> PartForRelationship(const PowerPointDocument::Ptr& document,
                                                          std::string_view relationshipId)
{
    auto presentationPart = document ? document->GetPresentationPart() : nullptr;
    if (!presentationPart || relationshipId.empty())
    {
        return nullptr;
    }
    // A slide can be the target of more than one relationship - its notes slide
    // points back at it - and OpenXmlPackagePart::RelationshipId() is empty as
    // soon as that happens. Resolve the id against the presentation part's own
    // edges instead.
    for (const auto& part : presentationPart->GetSlideParts())
    {
        if (!part)
        {
            continue;
        }
        for (const auto& incoming : part->IncomingRelationships())
        {
            if (incoming.SourceUri == presentationPart->Uri() && incoming.Id == relationshipId)
            {
                return part;
            }
        }
    }
    return nullptr;
}

UInt32 NextSlideId(const std::shared_ptr<Presentation::SlideIdList>& list)
{
    UInt32 next = 256;
    if (list)
    {
        for (const auto& entry : list->Elements<Presentation::SlideId>())
        {
            next = std::max(next, entry->GetId().ValueOr(255) + 1);
        }
    }
    return next;
}

class PresentationIdAllocator
{
public:
    static constexpr UInt32 MinimumSlideMasterId = 0x80000000u;
    static constexpr UInt32 MinimumSlideLayoutId = 0x80000000u;

    static std::optional<UInt32> NextSlideMasterId(
        const std::shared_ptr<Presentation::SlideMasterIdList>& list)
    {
        std::unordered_set<UInt32> used;
        if (list)
        {
            for (const auto& entry : list->Elements<Presentation::SlideMasterId>())
            {
                if (entry->GetId().IsDefined())
                {
                    used.insert(entry->GetId().ValueOr(0));
                }
            }
        }

        return NextAbove(used, MinimumSlideMasterId);
    }

    /**
     * @brief Allocates a slide layout id.
     *
     * `p:sldLayoutId/@id` shares the slide master's value range: PresentationML
     * rejects anything below 2 147 483 648.
     */
    static std::optional<UInt32> NextSlideLayoutId(
        const std::shared_ptr<Presentation::SlideLayoutIdList>& list)
    {
        std::unordered_set<UInt32> used;
        if (list)
        {
            for (const auto& entry : list->Elements<Presentation::SlideLayoutId>())
            {
                if (entry->GetId().IsDefined())
                {
                    used.insert(entry->GetId().ValueOr(0));
                }
            }
        }

        return NextAbove(used, MinimumSlideLayoutId);
    }

private:
    static std::optional<UInt32> NextAbove(const std::unordered_set<UInt32>& used,
                                           UInt32 minimum)
    {
        auto candidate = minimum;
        while (used.contains(candidate))
        {
            if (candidate == std::numeric_limits<UInt32>::max())
            {
                return std::nullopt;
            }
            ++candidate;
        }
        return candidate;
    }
};

class PresentationDomBuilders
{
public:
    static void Clear(const std::shared_ptr<OpenXMLElement>& element)
    {
        if (!element)
        {
            return;
        }
        const auto children = element->Children();
        for (const auto& child : children)
        {
            element->RemoveChild(child);
        }
    }

    static Presentation::ShapeTree::Ptr AppendShapeTree(const std::shared_ptr<OpenXMLElement>& root)
    {
        auto common = root ? root->AppendChild<Presentation::CommonSlideData>() : nullptr;
        auto tree = common ? common->AppendChild<Presentation::ShapeTree>() : nullptr;
        auto nonVisual = tree ? tree->AppendChild<Presentation::NonVisualGroupShapeProperties>() : nullptr;
        auto drawing = nonVisual ? nonVisual->AppendChild<Presentation::NonVisualDrawingProperties>() : nullptr;
        auto groupDrawing =
            nonVisual ? nonVisual->AppendChild<Presentation::NonVisualGroupShapeDrawingProperties>() : nullptr;
        auto application =
            nonVisual ? nonVisual->AppendChild<Presentation::ApplicationNonVisualDrawingProperties>() : nullptr;
        if (!drawing || !groupDrawing || !application)
        {
            return nullptr;
        }
        drawing->SetId(UInt32Value(1));
        drawing->SetName(StringValue(""));
        auto properties = tree->AppendChild<Presentation::GroupShapeProperties>();
        auto transform = properties ? properties->AppendChild<Drawing::TransformGroup>() : nullptr;
        auto offset = transform ? transform->AppendChild<Drawing::Offset>() : nullptr;
        auto extents = transform ? transform->AppendChild<Drawing::Extents>() : nullptr;
        auto childOffset = transform ? transform->AppendChild<Drawing::ChildOffset>() : nullptr;
        auto childExtents = transform ? transform->AppendChild<Drawing::ChildExtents>() : nullptr;
        if (!offset || !extents || !childOffset || !childExtents)
        {
            return nullptr;
        }
        offset->SetX(Int64Value(0));
        offset->SetY(Int64Value(0));
        extents->SetCx(Int64Value(0));
        extents->SetCy(Int64Value(0));
        childOffset->SetX(Int64Value(0));
        childOffset->SetY(Int64Value(0));
        childExtents->SetCx(Int64Value(0));
        childExtents->SetCy(Int64Value(0));
        return tree;
    }

    static bool AppendColorMap(const std::shared_ptr<OpenXMLElement>& root)
    {
        auto map = root ? root->AppendChild<Presentation::ColorMap>() : nullptr;
        if (!map)
        {
            return false;
        }
        using Values = Drawing::ColorSchemeIndexValues;
        map->SetBackground1(EnumValue<Values>(Values::Light1));
        map->SetText1(EnumValue<Values>(Values::Dark1));
        map->SetBackground2(EnumValue<Values>(Values::Light2));
        map->SetText2(EnumValue<Values>(Values::Dark2));
        map->SetAccent1(EnumValue<Values>(Values::Accent1));
        map->SetAccent2(EnumValue<Values>(Values::Accent2));
        map->SetAccent3(EnumValue<Values>(Values::Accent3));
        map->SetAccent4(EnumValue<Values>(Values::Accent4));
        map->SetAccent5(EnumValue<Values>(Values::Accent5));
        map->SetAccent6(EnumValue<Values>(Values::Accent6));
        map->SetHyperlink(EnumValue<Values>(Values::Hyperlink));
        map->SetFollowedHyperlink(EnumValue<Values>(Values::FollowedHyperlink));
        return true;
    }

    static bool AppendColorMapOverride(const std::shared_ptr<OpenXMLElement>& root)
    {
        auto override = root ? root->AppendChild<Presentation::ColorMapOverride>() : nullptr;
        auto mapping = override ? override->AppendChild<Drawing::MasterColorMapping>() : nullptr;
        return mapping != nullptr;
    }

    static bool InitializeSlide(const Presentation::Slide::Ptr& root)
    {
        Clear(root);
        return AppendShapeTree(root) && AppendColorMapOverride(root);
    }

    static bool InitializeSlideMaster(const Presentation::SlideMaster::Ptr& root)
    {
        Clear(root);
        auto tree = AppendShapeTree(root);
        const bool hasColorMap = AppendColorMap(root);
        auto layouts = root ? root->AppendChild<Presentation::SlideLayoutIdList>() : nullptr;
        return tree && hasColorMap && layouts;
    }

    static bool InitializeSlideLayout(const Presentation::SlideLayout::Ptr& root)
    {
        Clear(root);
        if (!root)
        {
            return false;
        }
        root->SetPreserve(BooleanValue(true));
        auto tree = AppendShapeTree(root);
        const bool hasColorMapOverride = AppendColorMapOverride(root);
        return tree && hasColorMapOverride;
    }

    static bool AppendParagraphs(const std::shared_ptr<OpenXMLElement>& body, const std::string& text)
    {
        Size start = 0;
        do
        {
            const auto end = text.find('\n', start);
            auto paragraph = body ? body->AppendChild<Drawing::Paragraph>() : nullptr;
            auto run = paragraph ? paragraph->AppendChild<Drawing::Run>() : nullptr;
            auto textElement = run ? run->AppendChild<Drawing::Text>() : nullptr;
            if (!textElement)
            {
                return false;
            }
            textElement->SetText(text.substr(start, end == std::string::npos ? end : end - start));
            if (end == std::string::npos)
            {
                break;
            }
            start = end + 1;
        } while (true);
        return true;
    }

    static bool InitializeNotesPage(const Presentation::NotesSlide::Ptr& root,
                                    const PresentationNotesPage& page)
    {
        Clear(root);
        if (!root)
        {
            return false;
        }
        root->SetShowMasterShapes(BooleanValue(page.ShowMasterShapes));
        root->SetShowMasterPlaceholderAnimations(BooleanValue(page.ShowMasterPlaceholderAnimations));
        auto tree = AppendShapeTree(root);
        auto shape = tree ? tree->AppendChild<Presentation::Shape>() : nullptr;
        auto nonVisual = shape ? shape->AppendChild<Presentation::NonVisualShapeProperties>() : nullptr;
        auto drawing = nonVisual ? nonVisual->AppendChild<Presentation::NonVisualDrawingProperties>() : nullptr;
        auto application =
            nonVisual ? nonVisual->AppendChild<Presentation::ApplicationNonVisualDrawingProperties>() : nullptr;
        auto shapeDrawing =
            nonVisual ? nonVisual->AppendChild<Presentation::NonVisualShapeDrawingProperties>() : nullptr;
        auto shapeProperties = shape ? shape->AppendChild<Presentation::ShapeProperties>() : nullptr;
        if (!drawing || !application || !shapeDrawing || !shapeProperties)
        {
            return false;
        }
        drawing->SetId(UInt32Value(2));
        drawing->SetName(StringValue("Notes Placeholder"));
        auto placeholder = application->AppendChild<Presentation::PlaceholderShape>();
        if (!placeholder)
        {
            return false;
        }
        placeholder->SetType(EnumValue<Presentation::PlaceholderValues>(Presentation::PlaceholderValues::Body));
        auto body = shape->AppendChild<Presentation::TextBody>();
        auto bodyProperties = body ? body->AppendChild<Drawing::BodyProperties>() : nullptr;
        auto listStyle = body ? body->AppendChild<Drawing::ListStyle>() : nullptr;
        const bool hasParagraphs = AppendParagraphs(body, page.Text);
        if (!body || !bodyProperties || !listStyle || !hasParagraphs)
        {
            return false;
        }
        return AppendColorMapOverride(root);
    }

    static bool InitializeHandoutMaster(const Presentation::HandoutMaster::Ptr& root)
    {
        Clear(root);
        auto tree = AppendShapeTree(root);
        const bool hasColorMap = AppendColorMap(root);
        auto headerFooter = root ? root->AppendChild<Presentation::HeaderFooter>() : nullptr;
        return tree && hasColorMap && headerFooter;
    }

    static bool InitializeNotesMaster(const Presentation::NotesMaster::Ptr& root)
    {
        Clear(root);
        auto tree = AppendShapeTree(root);
        const bool hasColorMap = AppendColorMap(root);
        return tree && hasColorMap;
    }
};

constexpr std::string_view SlideLayoutRelationship =
    "http://schemas.openxmlformats.org/officeDocument/2006/relationships/"
    "slideLayout";
constexpr std::string_view SlideMasterRelationship =
    "http://schemas.openxmlformats.org/officeDocument/2006/relationships/"
    "slideMaster";
constexpr std::string_view SlideRelationship =
    "http://schemas.openxmlformats.org/officeDocument/2006/relationships/slide";
constexpr std::string_view NotesMasterRelationship =
    "http://schemas.openxmlformats.org/officeDocument/2006/relationships/"
    "notesMaster";
constexpr std::string_view ImageRelationship =
    "http://schemas.openxmlformats.org/officeDocument/2006/relationships/image";
constexpr std::string_view HyperlinkRelationship =
    "http://schemas.openxmlformats.org/officeDocument/2006/relationships/"
    "hyperlink";
constexpr std::string_view TableGraphicDataUri = "http://schemas.openxmlformats.org/drawingml/2006/table";

/** @brief Creates or reuses the presentation-wide notes master every notes slide refers to. */
class PresentationNotesMasterHelpers
{
public:
    static std::shared_ptr<Packaging::NotesMasterPart> Ensure(const std::shared_ptr<Packaging::SlidePart>& slidePart)
    {
        auto presentationPart = slidePart ? PresentationPartOf(slidePart) : nullptr;
        auto presentation = presentationPart ? presentationPart->GetTypedRootElement() : nullptr;
        if (!presentation)
        {
            return nullptr;
        }
        if (auto existing = presentationPart->GetNotesMasterPart())
        {
            return existing;
        }

        auto part = presentationPart->AddNotesMasterPart();
        if (!part || !PresentationDomBuilders::InitializeNotesMaster(part->GetTypedRootElement()))
        {
            if (part)
            {
                presentationPart->RemoveNotesMasterPart();
            }
            return nullptr;
        }

        // The notes master list precedes the slide list in the presentation's
        // content model; InsertChild places it there whatever the call order.
        auto list = presentation->GetFirstChildOfType<Presentation::NotesMasterIdList>();
        if (!list)
        {
            list = presentation->InsertChild<Presentation::NotesMasterIdList>(
                presentation->GetFirstChildOfType<Presentation::SlideIdList>());
        }
        auto entry = list ? list->AppendChild<Presentation::NotesMasterId>() : nullptr;
        if (!entry)
        {
            presentationPart->RemoveNotesMasterPart();
            return nullptr;
        }
        entry->SetId(StringValue(part->RelationshipId()));
        return part;
    }

private:
    static std::shared_ptr<Packaging::PresentationPart> PresentationPartOf(
        const std::shared_ptr<Packaging::SlidePart>& slidePart)
    {
        auto package = slidePart ? dynamic_cast<PowerPointDocument*>(slidePart->Package()) : nullptr;
        return package ? package->GetPresentationPart() : nullptr;
    }
};

/**
 * @brief Relationship id of the edge that leads from the presentation to @p part.
 *
 * Preferred over OpenXmlPackagePart::RelationshipId(), which reports nothing once
 * a slide is referenced from more than one place (its notes slide points back at
 * it), and `p:sldId/@r:id` must name the presentation's own edge.
 */
std::string SlideRelationshipId(const std::shared_ptr<Packaging::PresentationPart>& presentationPart,
                                const std::shared_ptr<Packaging::SlidePart>& part)
{
    if (!presentationPart || !part)
    {
        return {};
    }
    for (const auto& incoming : part->IncomingRelationships())
    {
        if (incoming.SourceUri == presentationPart->Uri() && incoming.Type == SlideRelationship)
        {
            return incoming.Id;
        }
    }
    return {};
}

/// Builds the descriptor the security layer needs for a linked resource on a slide.
class PresentationLinkHelpers
{
public:
    PresentationLinkHelpers() = delete;

    /// Recovers the relationship id belonging to a linked target so the diagnostics
    /// the security layer writes name the edge and not only the URI.
    static Security::ExternalReference Reference(const std::shared_ptr<Packaging::SlidePart>& part,
                                                 std::string_view relationshipType,
                                                 const std::string& target,
                                                 Security::ExternalResourceKind kind)
    {
        Security::ExternalReference reference;
        reference.SourcePartUri = part ? part->Uri() : std::string();
        reference.RelationshipType = std::string(relationshipType);
        reference.Target = target;
        reference.Kind = kind;
        if (part)
        {
            for (const auto& relationship : part->Relationships())
            {
                if (relationship.IsExternal && relationship.Type == relationshipType && relationship.Target == target)
                {
                    reference.RelationshipId = relationship.Id;
                    break;
                }
            }
        }
        return reference;
    }
};

class PresentationShapeTreeHelpers
{
public:
    static bool IsDrawable(const std::shared_ptr<OpenXMLElement>& element)
    {
        return std::dynamic_pointer_cast<Presentation::Shape>(element) ||
               std::dynamic_pointer_cast<Presentation::GroupShape>(element) ||
               std::dynamic_pointer_cast<Presentation::Picture>(element) ||
               std::dynamic_pointer_cast<Presentation::GraphicFrame>(element) ||
               std::dynamic_pointer_cast<Presentation::ConnectionShape>(element) ||
               std::dynamic_pointer_cast<Presentation::ContentPart>(element);
    }

    static std::vector<std::shared_ptr<OpenXMLElement>> Elements(const std::shared_ptr<OpenXMLElement>& tree)
    {
        std::vector<std::shared_ptr<OpenXMLElement>> result;
        if (tree)
        {
            for (const auto& child : tree->Children())
            {
                if (IsDrawable(child))
                {
                    result.push_back(child);
                }
            }
        }
        return result;
    }

    static std::shared_ptr<OpenXMLElement> TrailingAnchor(const std::shared_ptr<OpenXMLElement>& tree)
    {
        bool sawShape = false;
        for (const auto& child : tree->Children())
        {
            if (IsDrawable(child))
            {
                sawShape = true;
            }
            else if (sawShape)
            {
                return child;
            }
        }
        return nullptr;
    }

    static UInt32 NextId(const std::shared_ptr<OpenXMLElement>& tree)
    {
        UInt32 id = 2;
        for (const auto& property : tree->Descendants<Presentation::NonVisualDrawingProperties>())
        {
            id = std::max(id, property->GetId().ValueOr(0) + 1);
        }
        return id;
    }

    static std::shared_ptr<OpenXMLElement> TransformHost(const std::shared_ptr<OpenXMLElement>& element)
    {
        if (auto group = std::dynamic_pointer_cast<Presentation::GroupShape>(element))
        {
            return group->GetFirstChildOfType<Presentation::GroupShapeProperties>();
        }
        if (auto frame = std::dynamic_pointer_cast<Presentation::GraphicFrame>(element))
        {
            return frame;
        }
        if (std::dynamic_pointer_cast<Presentation::Shape>(element) ||
            std::dynamic_pointer_cast<Presentation::Picture>(element) ||
            std::dynamic_pointer_cast<Presentation::ConnectionShape>(element))
        {
            return element->GetFirstChildOfType<Presentation::ShapeProperties>();
        }
        return nullptr;
    }

    static std::shared_ptr<OpenXMLElement> EnsureTransformHost(const std::shared_ptr<OpenXMLElement>& element)
    {
        if (auto host = TransformHost(element))
        {
            return host;
        }
        if (auto group = std::dynamic_pointer_cast<Presentation::GroupShape>(element))
        {
            return group->AppendChild<Presentation::GroupShapeProperties>();
        }
        if (std::dynamic_pointer_cast<Presentation::GraphicFrame>(element))
        {
            return element;
        }
        if (std::dynamic_pointer_cast<Presentation::Shape>(element) ||
            std::dynamic_pointer_cast<Presentation::Picture>(element) ||
            std::dynamic_pointer_cast<Presentation::ConnectionShape>(element))
        {
            return element->AppendChild<Presentation::ShapeProperties>();
        }
        return nullptr;
    }
};

class PresentationEmbeddedObjectHelpers
{
public:
    static std::optional<std::pair<PresentationEmbeddedObjectKind, std::vector<std::string>>> References(
        const std::shared_ptr<OpenXMLElement>& element)
    {
        if (!element)
        {
            return std::nullopt;
        }
        const auto charts = element->Descendants<Charts::ChartReference>();
        if (!charts.empty())
        {
            return std::pair{PresentationEmbeddedObjectKind::Chart,
                             std::vector<std::string>{charts.front()->GetId().ToString()}};
        }
        const auto diagrams = element->Descendants<Diagrams::RelationshipIds>();
        if (!diagrams.empty())
        {
            const auto& diagram = diagrams.front();
            std::vector<std::string> ids;
            for (const auto& id : {diagram->GetDataPart().ToString(), diagram->GetLayoutPart().ToString(),
                                   diagram->GetStylePart().ToString(), diagram->GetColorPart().ToString()})
            {
                if (!id.empty())
                {
                    ids.push_back(id);
                }
            }
            return std::pair{PresentationEmbeddedObjectKind::SmartArt, std::move(ids)};
        }
        const auto objects = element->Descendants<Presentation::OleObject>();
        if (!objects.empty())
        {
            return std::pair{PresentationEmbeddedObjectKind::Ole,
                             std::vector<std::string>{objects.front()->GetId().ToString()}};
        }
        return std::nullopt;
    }

    static std::shared_ptr<OpenXmlPackagePart> Target(const std::shared_ptr<Packaging::SlidePart>& slide,
                                                      std::string_view id)
    {
        if (!slide || id.empty())
        {
            return nullptr;
        }
        for (const auto& part : slide->Parts())
        {
            if (part)
            {
                for (const auto& incoming : part->IncomingRelationships())
                {
                    if (incoming.SourceUri == slide->Uri() && incoming.Id == id)
                    {
                        return part;
                    }
                }
            }
        }
        return nullptr;
    }

    /// Returns a copy on purpose: Relationships() exposes the part's live vector, and
    /// registering another relationship reallocates it. A pointer or reference into that
    /// vector would dangle as soon as the caller attaches or removes a part.
    static std::optional<OpenXmlRelationship> Relationship(const std::shared_ptr<Packaging::SlidePart>& slide,
                                                           std::string_view id)
    {
        if (!slide)
        {
            return std::nullopt;
        }
        for (const auto& relationship : slide->Relationships())
        {
            if (relationship.Id == id)
            {
                return relationship;
            }
        }
        return std::nullopt;
    }
};

class PresentationMediaHelpers
{
public:
    static constexpr std::string_view AudioRelationship =
        "http://schemas.openxmlformats.org/officeDocument/2006/relationships/audio";
    static constexpr std::string_view VideoRelationship =
        "http://schemas.openxmlformats.org/officeDocument/2006/relationships/video";

    static const OpenXmlPartDescriptor& Descriptor(PresentationMediaKind kind)
    {
        static constexpr OpenXmlPartDescriptor audio = {
            "PresentationAudioPart", AudioRelationship, "", "audio", ".bin", "../media",
            OpenXmlPartKind::Binary, OpenXml::FileFormatVersions::Office2007};
        static constexpr OpenXmlPartDescriptor video = {
            "PresentationVideoPart", VideoRelationship, "", "video", ".bin", "../media",
            OpenXmlPartKind::Binary, OpenXml::FileFormatVersions::Office2007};
        return kind == PresentationMediaKind::Audio ? audio : video;
    }

    static std::shared_ptr<OpenXMLElement> Marker(const std::shared_ptr<OpenXMLElement>& element)
    {
        if (!element)
        {
            return nullptr;
        }
        const auto videos = element->Descendants<Drawing::VideoFromFile>();
        if (!videos.empty())
        {
            return videos.front();
        }
        const auto audios = element->Descendants<Drawing::AudioFromFile>();
        return audios.empty() ? nullptr : audios.front();
    }

    static std::string RelationshipId(const std::shared_ptr<OpenXMLElement>& marker)
    {
        if (auto video = std::dynamic_pointer_cast<Drawing::VideoFromFile>(marker))
        {
            return video->GetLink().ToString();
        }
        if (auto audio = std::dynamic_pointer_cast<Drawing::AudioFromFile>(marker))
        {
            return audio->GetLink().ToString();
        }
        return {};
    }

    static UInt32 ShapeId(const std::shared_ptr<OpenXMLElement>& element)
    {
        const auto properties = element ? element->Descendants<Presentation::NonVisualDrawingProperties>()
                                        : std::vector<Presentation::NonVisualDrawingProperties::Ptr>{};
        return properties.empty() ? 0 : properties.front()->GetId().ValueOr(0);
    }

    static std::shared_ptr<OpenXmlPackagePart> Target(const std::shared_ptr<Packaging::SlidePart>& slide,
                                                      std::string_view id)
    {
        return PresentationEmbeddedObjectHelpers::Target(slide, id);
    }

    static std::optional<OpenXmlRelationship> Relationship(const std::shared_ptr<Packaging::SlidePart>& slide,
                                                           std::string_view id)
    {
        return PresentationEmbeddedObjectHelpers::Relationship(slide, id);
    }

    static std::shared_ptr<Presentation::CommonMediaNode> TimingNode(
        const std::shared_ptr<Packaging::SlidePart>& slide, UInt32 shapeId)
    {
        auto root = slide ? slide->GetSlide() : nullptr;
        if (!root)
        {
            return nullptr;
        }
        for (const auto& node : root->Descendants<Presentation::CommonMediaNode>())
        {
            const auto targets = node->Descendants<Presentation::ShapeTarget>();
            if (!targets.empty() && targets.front()->GetShapeId().ToString() == std::to_string(shapeId))
            {
                return node;
            }
        }
        return nullptr;
    }

    static bool Valid(const PresentationMediaData& value)
    {
        const bool source = value.Embedded.has_value() != value.LinkedUri.has_value();
        const bool embedded = !value.Embedded || (!value.Embedded->Data.empty() &&
                                                  !value.Embedded->ContentType.empty());
        return source && embedded && (!value.LinkedUri || !value.LinkedUri->empty()) &&
               value.Playback.Volume >= 0 && value.Playback.Volume <= 100000 &&
               PresentationMeasurementHelpers::IsNonNegative(value.Transform.Size.Width) &&
               PresentationMeasurementHelpers::IsNonNegative(value.Transform.Size.Height) &&
               !value.Transform.GroupChildPosition && !value.Transform.GroupChildSize;
    }
};

class PresentationTransitionHelpers
{
public:
    /**
     * Lexical values of the `ST_TransitionEightDirectionType` simple type.
     *
     * The generated DOM currently exposes this schema type as StringValue, so
     * keep its closed value set explicit here instead of spreading tokens.
     */
    enum class EightDirectionValue
    {
        Left,
        Up,
        Right,
        Down,
        LeftUp,
        RightUp,
        LeftDown,
        RightDown
    };

    static std::optional<UInt32> Milliseconds(const StringValue& value)
    {
        const auto text = value.ToString();
        if (text.empty())
        {
            return std::nullopt;
        }
        UInt32 result = 0;
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
        if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
        {
            return std::nullopt;
        }
        return result;
    }

    /// Groups transition effects by the option members their PresentationML type carries.
    enum class OptionFamily
    {
        None,                         ///< `p:CT_Empty` effects without parameters.
        ThroughBlack,                 ///< `p:CT_OptionalBlackTransition`.
        SideDirection,                ///< `p:CT_SideDirectionTransition`.
        EightDirection,               ///< `p:CT_EightDirectionTransition`.
        CornerDirection,              ///< `p:CT_CornerDirectionTransition`.
        InOutDirection,               ///< `p:CT_InOutTransition`.
        Orientation,                  ///< `p:CT_OrientationTransition`.
        SplitOrientationAndDirection, ///< `p:CT_SplitTransition`.
        Spokes                        ///< `p:CT_WheelTransition`.
    };

    static OptionFamily Family(PresentationTransitionKind kind)
    {
        switch (kind)
        {
            case PresentationTransitionKind::Cut:
            case PresentationTransitionKind::Fade:
                return OptionFamily::ThroughBlack;
            case PresentationTransitionKind::Push:
            case PresentationTransitionKind::Wipe:
                return OptionFamily::SideDirection;
            case PresentationTransitionKind::Cover:
            case PresentationTransitionKind::Pull:
                return OptionFamily::EightDirection;
            case PresentationTransitionKind::Strips:
                return OptionFamily::CornerDirection;
            case PresentationTransitionKind::Zoom:
                return OptionFamily::InOutDirection;
            case PresentationTransitionKind::Blinds:
            case PresentationTransitionKind::Checker:
            case PresentationTransitionKind::Comb:
            case PresentationTransitionKind::RandomBar:
                return OptionFamily::Orientation;
            case PresentationTransitionKind::Split:
                return OptionFamily::SplitOrientationAndDirection;
            case PresentationTransitionKind::Wheel:
                return OptionFamily::Spokes;
            default:
                return OptionFamily::None;
        }
    }

    static bool IsSideDirection(PresentationTransitionDirection direction)
    {
        return direction == PresentationTransitionDirection::Left ||
               direction == PresentationTransitionDirection::Up ||
               direction == PresentationTransitionDirection::Right ||
               direction == PresentationTransitionDirection::Down;
    }

    static bool IsCornerDirection(PresentationTransitionDirection direction)
    {
        return direction == PresentationTransitionDirection::LeftUp ||
               direction == PresentationTransitionDirection::RightUp ||
               direction == PresentationTransitionDirection::LeftDown ||
               direction == PresentationTransitionDirection::RightDown;
    }

    static bool IsInOutDirection(PresentationTransitionDirection direction)
    {
        return direction == PresentationTransitionDirection::In ||
               direction == PresentationTransitionDirection::Out;
    }

    /// Rejects any option the effect's PresentationML type cannot express.
    static bool OptionsAccepted(PresentationTransitionKind kind, const PresentationTransitionOptions& options)
    {
        const auto family = Family(kind);
        const bool directionAllowed = family == OptionFamily::SideDirection ||
                                      family == OptionFamily::EightDirection ||
                                      family == OptionFamily::CornerDirection ||
                                      family == OptionFamily::InOutDirection ||
                                      family == OptionFamily::SplitOrientationAndDirection;
        const bool orientationAllowed = family == OptionFamily::Orientation ||
                                        family == OptionFamily::SplitOrientationAndDirection;
        if (options.Direction && !directionAllowed)
        {
            return false;
        }
        if (options.Orientation && !orientationAllowed)
        {
            return false;
        }
        if (options.ThroughBlack && family != OptionFamily::ThroughBlack)
        {
            return false;
        }
        if (options.Spokes && (family != OptionFamily::Spokes || *options.Spokes == 0))
        {
            return false;
        }
        if (!options.Direction)
        {
            return true;
        }
        switch (family)
        {
            case OptionFamily::SideDirection:
                return IsSideDirection(*options.Direction);
            case OptionFamily::EightDirection:
                return IsSideDirection(*options.Direction) || IsCornerDirection(*options.Direction);
            case OptionFamily::CornerDirection:
                return IsCornerDirection(*options.Direction);
            case OptionFamily::InOutDirection:
            case OptionFamily::SplitOrientationAndDirection:
                return IsInOutDirection(*options.Direction);
            default:
                return false;
        }
    }

    static PresentationTransitionKind Kind(const Presentation::Transition::Ptr& transition)
    {
        if (!transition)
        {
            return PresentationTransitionKind::Unsupported;
        }
        if (transition->GetFirstChildOfType<Presentation::BlindsTransition>())
        {
            return PresentationTransitionKind::Blinds;
        }
        if (transition->GetFirstChildOfType<Presentation::CheckerTransition>())
        {
            return PresentationTransitionKind::Checker;
        }
        if (transition->GetFirstChildOfType<Presentation::CircleTransition>())
        {
            return PresentationTransitionKind::Circle;
        }
        if (transition->GetFirstChildOfType<Presentation::CombTransition>())
        {
            return PresentationTransitionKind::Comb;
        }
        if (transition->GetFirstChildOfType<Presentation::CoverTransition>())
        {
            return PresentationTransitionKind::Cover;
        }
        if (transition->GetFirstChildOfType<Presentation::CutTransition>())
        {
            return PresentationTransitionKind::Cut;
        }
        if (transition->GetFirstChildOfType<Presentation::DiamondTransition>())
        {
            return PresentationTransitionKind::Diamond;
        }
        if (transition->GetFirstChildOfType<Presentation::DissolveTransition>())
        {
            return PresentationTransitionKind::Dissolve;
        }
        if (transition->GetFirstChildOfType<Presentation::FadeTransition>())
        {
            return PresentationTransitionKind::Fade;
        }
        if (transition->GetFirstChildOfType<Presentation::NewsflashTransition>())
        {
            return PresentationTransitionKind::Newsflash;
        }
        if (transition->GetFirstChildOfType<Presentation::PlusTransition>())
        {
            return PresentationTransitionKind::Plus;
        }
        if (transition->GetFirstChildOfType<Presentation::PullTransition>())
        {
            return PresentationTransitionKind::Pull;
        }
        if (transition->GetFirstChildOfType<Presentation::PushTransition>())
        {
            return PresentationTransitionKind::Push;
        }
        if (transition->GetFirstChildOfType<Presentation::RandomTransition>())
        {
            return PresentationTransitionKind::Random;
        }
        if (transition->GetFirstChildOfType<Presentation::RandomBarTransition>())
        {
            return PresentationTransitionKind::RandomBar;
        }
        if (transition->GetFirstChildOfType<Presentation::SplitTransition>())
        {
            return PresentationTransitionKind::Split;
        }
        if (transition->GetFirstChildOfType<Presentation::StripsTransition>())
        {
            return PresentationTransitionKind::Strips;
        }
        if (transition->GetFirstChildOfType<Presentation::WedgeTransition>())
        {
            return PresentationTransitionKind::Wedge;
        }
        if (transition->GetFirstChildOfType<Presentation::WheelTransition>())
        {
            return PresentationTransitionKind::Wheel;
        }
        if (transition->GetFirstChildOfType<Presentation::WipeTransition>())
        {
            return PresentationTransitionKind::Wipe;
        }
        if (transition->GetFirstChildOfType<Presentation::ZoomTransition>())
        {
            return PresentationTransitionKind::Zoom;
        }
        return PresentationTransitionKind::Unsupported;
    }

    static Presentation::SideDirectionTransitionType::Ptr SideEffect(
        const Presentation::Transition::Ptr& transition, PresentationTransitionKind kind)
    {
        if (kind == PresentationTransitionKind::Push)
        {
            return transition->GetFirstChildOfType<Presentation::PushTransition>();
        }
        return transition->GetFirstChildOfType<Presentation::WipeTransition>();
    }

    static Presentation::EightDirectionTransitionType::Ptr EightEffect(
        const Presentation::Transition::Ptr& transition, PresentationTransitionKind kind)
    {
        if (kind == PresentationTransitionKind::Cover)
        {
            return transition->GetFirstChildOfType<Presentation::CoverTransition>();
        }
        return transition->GetFirstChildOfType<Presentation::PullTransition>();
    }

    static Presentation::OrientationTransitionType::Ptr OrientationEffect(
        const Presentation::Transition::Ptr& transition, PresentationTransitionKind kind)
    {
        switch (kind)
        {
            case PresentationTransitionKind::Blinds:
                return transition->GetFirstChildOfType<Presentation::BlindsTransition>();
            case PresentationTransitionKind::Checker:
                return transition->GetFirstChildOfType<Presentation::CheckerTransition>();
            case PresentationTransitionKind::Comb:
                return transition->GetFirstChildOfType<Presentation::CombTransition>();
            default:
                return transition->GetFirstChildOfType<Presentation::RandomBarTransition>();
        }
    }

    static Presentation::OptionalBlackTransitionType::Ptr BlackEffect(
        const Presentation::Transition::Ptr& transition, PresentationTransitionKind kind)
    {
        if (kind == PresentationTransitionKind::Cut)
        {
            return transition->GetFirstChildOfType<Presentation::CutTransition>();
        }
        return transition->GetFirstChildOfType<Presentation::FadeTransition>();
    }

    static std::optional<PresentationTransitionDirection> FromSide(
        const EnumValue<Presentation::TransitionSlideDirectionValues>& value)
    {
        if (!value.IsDefined())
        {
            return std::nullopt;
        }
        switch (value.Value().GetValue())
        {
            case Presentation::TransitionSlideDirectionValues::Left:
                return PresentationTransitionDirection::Left;
            case Presentation::TransitionSlideDirectionValues::Up:
                return PresentationTransitionDirection::Up;
            case Presentation::TransitionSlideDirectionValues::Right:
                return PresentationTransitionDirection::Right;
            case Presentation::TransitionSlideDirectionValues::Down:
                return PresentationTransitionDirection::Down;
            default:
                return std::nullopt;
        }
    }

    static Presentation::TransitionSlideDirectionValues::Value ToSide(PresentationTransitionDirection direction)
    {
        switch (direction)
        {
            case PresentationTransitionDirection::Up:
                return Presentation::TransitionSlideDirectionValues::Up;
            case PresentationTransitionDirection::Right:
                return Presentation::TransitionSlideDirectionValues::Right;
            case PresentationTransitionDirection::Down:
                return Presentation::TransitionSlideDirectionValues::Down;
            default:
                return Presentation::TransitionSlideDirectionValues::Left;
        }
    }

    static std::optional<PresentationTransitionDirection> FromCorner(
        const EnumValue<Presentation::TransitionCornerDirectionValues>& value)
    {
        if (!value.IsDefined())
        {
            return std::nullopt;
        }
        switch (value.Value().GetValue())
        {
            case Presentation::TransitionCornerDirectionValues::LeftUp:
                return PresentationTransitionDirection::LeftUp;
            case Presentation::TransitionCornerDirectionValues::RightUp:
                return PresentationTransitionDirection::RightUp;
            case Presentation::TransitionCornerDirectionValues::LeftDown:
                return PresentationTransitionDirection::LeftDown;
            case Presentation::TransitionCornerDirectionValues::RightDown:
                return PresentationTransitionDirection::RightDown;
            default:
                return std::nullopt;
        }
    }

    static Presentation::TransitionCornerDirectionValues::Value ToCorner(PresentationTransitionDirection direction)
    {
        switch (direction)
        {
            case PresentationTransitionDirection::RightUp:
                return Presentation::TransitionCornerDirectionValues::RightUp;
            case PresentationTransitionDirection::LeftDown:
                return Presentation::TransitionCornerDirectionValues::LeftDown;
            case PresentationTransitionDirection::RightDown:
                return Presentation::TransitionCornerDirectionValues::RightDown;
            default:
                return Presentation::TransitionCornerDirectionValues::LeftUp;
        }
    }

    static std::optional<PresentationTransitionDirection> FromInOut(
        const EnumValue<Presentation::TransitionInOutDirectionValues>& value)
    {
        if (!value.IsDefined())
        {
            return std::nullopt;
        }
        switch (value.Value().GetValue())
        {
            case Presentation::TransitionInOutDirectionValues::In:
                return PresentationTransitionDirection::In;
            case Presentation::TransitionInOutDirectionValues::Out:
                return PresentationTransitionDirection::Out;
            default:
                return std::nullopt;
        }
    }

    static Presentation::TransitionInOutDirectionValues::Value ToInOut(PresentationTransitionDirection direction)
    {
        return direction == PresentationTransitionDirection::In
                   ? Presentation::TransitionInOutDirectionValues::In
                   : Presentation::TransitionInOutDirectionValues::Out;
    }

    static std::optional<EightDirectionValue> ParseEightDirection(const StringValue& value)
    {
        const auto text = value.ToString();
        if (text == "l")
        {
            return EightDirectionValue::Left;
        }
        if (text == "u")
        {
            return EightDirectionValue::Up;
        }
        if (text == "r")
        {
            return EightDirectionValue::Right;
        }
        if (text == "d")
        {
            return EightDirectionValue::Down;
        }
        if (text == "lu")
        {
            return EightDirectionValue::LeftUp;
        }
        if (text == "ru")
        {
            return EightDirectionValue::RightUp;
        }
        if (text == "ld")
        {
            return EightDirectionValue::LeftDown;
        }
        if (text == "rd")
        {
            return EightDirectionValue::RightDown;
        }
        return std::nullopt;
    }

    static StringValue SerializeEightDirection(EightDirectionValue direction)
    {
        switch (direction)
        {
            case EightDirectionValue::Up:
                return StringValue("u");
            case EightDirectionValue::Right:
                return StringValue("r");
            case EightDirectionValue::Down:
                return StringValue("d");
            case EightDirectionValue::LeftUp:
                return StringValue("lu");
            case EightDirectionValue::RightUp:
                return StringValue("ru");
            case EightDirectionValue::LeftDown:
                return StringValue("ld");
            case EightDirectionValue::RightDown:
                return StringValue("rd");
            default:
                return StringValue("l");
        }
    }

    static std::optional<PresentationTransitionDirection> FromEight(const StringValue& value)
    {
        const auto direction = ParseEightDirection(value);
        if (!direction)
        {
            return std::nullopt;
        }
        switch (*direction)
        {
            case EightDirectionValue::Up:
                return PresentationTransitionDirection::Up;
            case EightDirectionValue::Right:
                return PresentationTransitionDirection::Right;
            case EightDirectionValue::Down:
                return PresentationTransitionDirection::Down;
            case EightDirectionValue::LeftUp:
                return PresentationTransitionDirection::LeftUp;
            case EightDirectionValue::RightUp:
                return PresentationTransitionDirection::RightUp;
            case EightDirectionValue::LeftDown:
                return PresentationTransitionDirection::LeftDown;
            case EightDirectionValue::RightDown:
                return PresentationTransitionDirection::RightDown;
            default:
                return PresentationTransitionDirection::Left;
        }
    }

    static EightDirectionValue ToEight(PresentationTransitionDirection direction)
    {
        switch (direction)
        {
            case PresentationTransitionDirection::Up:
                return EightDirectionValue::Up;
            case PresentationTransitionDirection::Right:
                return EightDirectionValue::Right;
            case PresentationTransitionDirection::Down:
                return EightDirectionValue::Down;
            case PresentationTransitionDirection::LeftUp:
                return EightDirectionValue::LeftUp;
            case PresentationTransitionDirection::RightUp:
                return EightDirectionValue::RightUp;
            case PresentationTransitionDirection::LeftDown:
                return EightDirectionValue::LeftDown;
            case PresentationTransitionDirection::RightDown:
                return EightDirectionValue::RightDown;
            default:
                return EightDirectionValue::Left;
        }
    }

    static std::optional<PresentationTransitionOrientation> FromOrientation(
        const EnumValue<Presentation::DirectionValues>& value)
    {
        if (!value.IsDefined())
        {
            return std::nullopt;
        }
        switch (value.Value().GetValue())
        {
            case Presentation::DirectionValues::Horizontal:
                return PresentationTransitionOrientation::Horizontal;
            case Presentation::DirectionValues::Vertical:
                return PresentationTransitionOrientation::Vertical;
            default:
                return std::nullopt;
        }
    }

    static Presentation::DirectionValues::Value ToOrientation(PresentationTransitionOrientation orientation)
    {
        return orientation == PresentationTransitionOrientation::Vertical
                   ? Presentation::DirectionValues::Vertical
                   : Presentation::DirectionValues::Horizontal;
    }

    static PresentationTransitionOptions ReadOptions(const Presentation::Transition::Ptr& transition,
                                                     PresentationTransitionKind kind)
    {
        PresentationTransitionOptions options;
        switch (Family(kind))
        {
            case OptionFamily::ThroughBlack:
                if (auto effect = BlackEffect(transition, kind); effect && effect->GetThroughBlack().IsDefined())
                {
                    options.ThroughBlack = effect->GetThroughBlack().Value();
                }
                break;
            case OptionFamily::SideDirection:
                if (auto effect = SideEffect(transition, kind))
                {
                    options.Direction = FromSide(effect->GetDirection());
                }
                break;
            case OptionFamily::EightDirection:
                if (auto effect = EightEffect(transition, kind))
                {
                    options.Direction = FromEight(effect->GetDirection());
                }
                break;
            case OptionFamily::CornerDirection:
                if (auto effect = transition->GetFirstChildOfType<Presentation::StripsTransition>())
                {
                    options.Direction = FromCorner(effect->GetDirection());
                }
                break;
            case OptionFamily::InOutDirection:
                if (auto effect = transition->GetFirstChildOfType<Presentation::ZoomTransition>())
                {
                    options.Direction = FromInOut(effect->GetDirection());
                }
                break;
            case OptionFamily::Orientation:
                if (auto effect = OrientationEffect(transition, kind))
                {
                    options.Orientation = FromOrientation(effect->GetDirection());
                }
                break;
            case OptionFamily::SplitOrientationAndDirection:
                if (auto effect = transition->GetFirstChildOfType<Presentation::SplitTransition>())
                {
                    options.Orientation = FromOrientation(effect->GetOrientation());
                    options.Direction = FromInOut(effect->GetDirection());
                }
                break;
            case OptionFamily::Spokes:
                if (auto effect = transition->GetFirstChildOfType<Presentation::WheelTransition>();
                    effect && effect->GetSpokes().IsDefined())
                {
                    options.Spokes = effect->GetSpokes().Value();
                }
                break;
            default:
                break;
        }
        return options;
    }

    static PresentationTransitionSpeed Speed(const Presentation::Transition::Ptr& transition)
    {
        switch (transition->GetSpeed().ValueOr(Presentation::TransitionSpeedValues::Medium))
        {
            case Presentation::TransitionSpeedValues::Slow:
                return PresentationTransitionSpeed::Slow;
            case Presentation::TransitionSpeedValues::Fast:
                return PresentationTransitionSpeed::Fast;
            default:
                return PresentationTransitionSpeed::Medium;
        }
    }

    static Presentation::TransitionSpeedValues::Value Speed(PresentationTransitionSpeed speed)
    {
        switch (speed)
        {
            case PresentationTransitionSpeed::Slow:
                return Presentation::TransitionSpeedValues::Slow;
            case PresentationTransitionSpeed::Fast:
                return Presentation::TransitionSpeedValues::Fast;
            default:
                return Presentation::TransitionSpeedValues::Medium;
        }
    }

    /// True for children that carry the effect itself rather than sound or extension metadata.
    static bool IsEffectChild(const std::shared_ptr<OpenXMLElement>& child)
    {
        return child && !std::dynamic_pointer_cast<Presentation::SoundAction>(child) &&
               !std::dynamic_pointer_cast<Presentation::ExtensionListWithModification>(child);
    }

    static bool HasOpaqueEffect(const Presentation::Transition::Ptr& transition)
    {
        if (!transition || Kind(transition) != PresentationTransitionKind::Unsupported)
        {
            return false;
        }
        for (const auto& child : transition->Children())
        {
            if (IsEffectChild(child))
            {
                return true;
            }
        }
        return false;
    }

    static void RemoveEffect(const Presentation::Transition::Ptr& transition)
    {
        for (const auto& child : transition->Children())
        {
            if (IsEffectChild(child))
            {
                transition->RemoveChild(child);
            }
        }
    }

    /// Appends the effect element for `kind` and applies the already validated `options`.
    static bool AppendEffect(const Presentation::Transition::Ptr& transition, PresentationTransitionKind kind,
                             const PresentationTransitionOptions& options)
    {
        switch (kind)
        {
            case PresentationTransitionKind::Circle:
                return transition->AppendChild<Presentation::CircleTransition>() != nullptr;
            case PresentationTransitionKind::Diamond:
                return transition->AppendChild<Presentation::DiamondTransition>() != nullptr;
            case PresentationTransitionKind::Dissolve:
                return transition->AppendChild<Presentation::DissolveTransition>() != nullptr;
            case PresentationTransitionKind::Newsflash:
                return transition->AppendChild<Presentation::NewsflashTransition>() != nullptr;
            case PresentationTransitionKind::Plus:
                return transition->AppendChild<Presentation::PlusTransition>() != nullptr;
            case PresentationTransitionKind::Random:
                return transition->AppendChild<Presentation::RandomTransition>() != nullptr;
            case PresentationTransitionKind::Wedge:
                return transition->AppendChild<Presentation::WedgeTransition>() != nullptr;
            case PresentationTransitionKind::Cut:
            case PresentationTransitionKind::Fade:
            {
                const Presentation::OptionalBlackTransitionType::Ptr effect =
                    kind == PresentationTransitionKind::Cut
                        ? std::static_pointer_cast<Presentation::OptionalBlackTransitionType>(
                              transition->AppendChild<Presentation::CutTransition>())
                        : std::static_pointer_cast<Presentation::OptionalBlackTransitionType>(
                              transition->AppendChild<Presentation::FadeTransition>());
                if (effect && options.ThroughBlack)
                {
                    effect->SetThroughBlack(BooleanValue(*options.ThroughBlack));
                }
                return effect != nullptr;
            }
            case PresentationTransitionKind::Push:
            case PresentationTransitionKind::Wipe:
            {
                const Presentation::SideDirectionTransitionType::Ptr effect =
                    kind == PresentationTransitionKind::Push
                        ? std::static_pointer_cast<Presentation::SideDirectionTransitionType>(
                              transition->AppendChild<Presentation::PushTransition>())
                        : std::static_pointer_cast<Presentation::SideDirectionTransitionType>(
                              transition->AppendChild<Presentation::WipeTransition>());
                if (effect && options.Direction)
                {
                    effect->SetDirection(
                        EnumValue<Presentation::TransitionSlideDirectionValues>(ToSide(*options.Direction)));
                }
                return effect != nullptr;
            }
            case PresentationTransitionKind::Cover:
            case PresentationTransitionKind::Pull:
            {
                const Presentation::EightDirectionTransitionType::Ptr effect =
                    kind == PresentationTransitionKind::Cover
                        ? std::static_pointer_cast<Presentation::EightDirectionTransitionType>(
                              transition->AppendChild<Presentation::CoverTransition>())
                        : std::static_pointer_cast<Presentation::EightDirectionTransitionType>(
                              transition->AppendChild<Presentation::PullTransition>());
                if (effect && options.Direction)
                {
                    effect->SetDirection(SerializeEightDirection(ToEight(*options.Direction)));
                }
                return effect != nullptr;
            }
            case PresentationTransitionKind::Strips:
            {
                auto effect = transition->AppendChild<Presentation::StripsTransition>();
                if (effect && options.Direction)
                {
                    effect->SetDirection(
                        EnumValue<Presentation::TransitionCornerDirectionValues>(ToCorner(*options.Direction)));
                }
                return effect != nullptr;
            }
            case PresentationTransitionKind::Zoom:
            {
                auto effect = transition->AppendChild<Presentation::ZoomTransition>();
                if (effect && options.Direction)
                {
                    effect->SetDirection(
                        EnumValue<Presentation::TransitionInOutDirectionValues>(ToInOut(*options.Direction)));
                }
                return effect != nullptr;
            }
            case PresentationTransitionKind::Blinds:
            case PresentationTransitionKind::Checker:
            case PresentationTransitionKind::Comb:
            case PresentationTransitionKind::RandomBar:
            {
                Presentation::OrientationTransitionType::Ptr effect;
                switch (kind)
                {
                    case PresentationTransitionKind::Blinds:
                        effect = transition->AppendChild<Presentation::BlindsTransition>();
                        break;
                    case PresentationTransitionKind::Checker:
                        effect = transition->AppendChild<Presentation::CheckerTransition>();
                        break;
                    case PresentationTransitionKind::Comb:
                        effect = transition->AppendChild<Presentation::CombTransition>();
                        break;
                    default:
                        effect = transition->AppendChild<Presentation::RandomBarTransition>();
                        break;
                }
                if (effect && options.Orientation)
                {
                    effect->SetDirection(EnumValue<Presentation::DirectionValues>(ToOrientation(*options.Orientation)));
                }
                return effect != nullptr;
            }
            case PresentationTransitionKind::Split:
            {
                auto effect = transition->AppendChild<Presentation::SplitTransition>();
                if (effect && options.Orientation)
                {
                    effect->SetOrientation(EnumValue<Presentation::DirectionValues>(ToOrientation(*options.Orientation)));
                }
                if (effect && options.Direction)
                {
                    effect->SetDirection(
                        EnumValue<Presentation::TransitionInOutDirectionValues>(ToInOut(*options.Direction)));
                }
                return effect != nullptr;
            }
            case PresentationTransitionKind::Wheel:
            {
                auto effect = transition->AppendChild<Presentation::WheelTransition>();
                if (effect && options.Spokes)
                {
                    effect->SetSpokes(UInt32Value(*options.Spokes));
                }
                return effect != nullptr;
            }
            default:
                return false;
        }
    }

    static std::string SoundId(const Presentation::Transition::Ptr& transition)
    {
        const auto sounds = transition ? transition->Descendants<Presentation::Sound>()
                                       : std::vector<Presentation::Sound::Ptr>{};
        return sounds.empty() ? std::string{} : sounds.front()->GetEmbed().ToString();
    }
};

/** Allocates identifiers shared by every `p:cTn` in one slide timing tree. */
class PresentationTimeNodeIdAllocator
{
public:
    using IdSet = std::unordered_set<UInt32>;

    explicit PresentationTimeNodeIdAllocator(IdSet reserved) : m_reserved(std::move(reserved)) {}

    UInt32 Next()
    {
        while (m_reserved.find(m_next) != m_reserved.end())
        {
            ++m_next;
        }
        return m_next++;
    }

    static UInt32 Next(const std::shared_ptr<Packaging::SlidePart>& part)
    {
        auto slide = part ? part->GetTypedRootElement() : nullptr;
        UInt32 next = 1;
        if (slide)
        {
            for (const auto& node : slide->Descendants<Presentation::CommonTimeNode>())
            {
                next = std::max(next, node->GetId().ValueOr(0) + 1);
            }
        }
        return next;
    }

    static bool IsAvailable(const std::shared_ptr<Packaging::SlidePart>& part, UInt32 id)
    {
        auto slide = part ? part->GetTypedRootElement() : nullptr;
        if (!slide || id == 0)
        {
            return false;
        }
        for (const auto& node : slide->Descendants<Presentation::CommonTimeNode>())
        {
            if (node->GetId().ValueOr(0) == id)
            {
                return false;
            }
        }
        return true;
    }

private:
    IdSet m_reserved;
    UInt32 m_next = 1;
};

class PresentationAnimationHelpers
{
public:
    /// Free-standing `p:anim` behaviors, that is direct children of `p:timing/p:tnLst`.
    static std::vector<Presentation::Animate::Ptr> Elements(const std::shared_ptr<Packaging::SlidePart>& part)
    {
        auto slide = part ? part->GetTypedRootElement() : nullptr;
        auto timing = slide ? slide->GetFirstChildOfType<Presentation::Timing>() : nullptr;
        auto list = timing ? timing->GetFirstChildOfType<Presentation::TimeNodeList>() : nullptr;
        return list ? list->Elements<Presentation::Animate>() : std::vector<Presentation::Animate::Ptr>{};
    }

    static Presentation::CommonTimeNode::Ptr TimeNode(const Presentation::Animate::Ptr& animation)
    {
        auto behavior = animation ? animation->GetFirstChildOfType<Presentation::CommonBehavior>() : nullptr;
        return behavior ? behavior->GetFirstChildOfType<Presentation::CommonTimeNode>() : nullptr;
    }

    static Presentation::ShapeTarget::Ptr Target(const Presentation::Animate::Ptr& animation)
    {
        auto behavior = animation ? animation->GetFirstChildOfType<Presentation::CommonBehavior>() : nullptr;
        const auto targets = behavior ? behavior->Descendants<Presentation::ShapeTarget>()
                                      : std::vector<Presentation::ShapeTarget::Ptr>{};
        return targets.empty() ? nullptr : targets.front();
    }

    static UInt32 TargetId(const Presentation::Animate::Ptr& animation)
    {
        auto target = Target(animation);
        if (!target)
        {
            return 0;
        }
        const auto text = target->GetShapeId().ToString();
        UInt32 result = 0;
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
        return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() ? result : 0;
    }

    static Presentation::Animate::Ptr Find(const std::shared_ptr<Packaging::SlidePart>& part, UInt32 id)
    {
        for (const auto& animation : Elements(part))
        {
            auto node = TimeNode(animation);
            if (node && node->GetId().ValueOr(0) == id)
            {
                return animation;
            }
        }
        return nullptr;
    }

    static bool ShapeExists(const std::shared_ptr<Packaging::SlidePart>& part, UInt32 id)
    {
        auto slide = part ? part->GetTypedRootElement() : nullptr;
        if (!slide || id == 0)
        {
            return false;
        }
        for (const auto& shape : slide->Descendants<Presentation::NonVisualDrawingProperties>())
        {
            if (shape->GetId().ValueOr(0) == id)
            {
                return true;
            }
        }
        return false;
    }

    static bool Write(const Presentation::Animate::Ptr& animation, const PresentationAnimationNode& value,
                      UInt32 id)
    {
        auto node = TimeNode(animation);
        auto target = Target(animation);
        if (!animation || !node || !target)
        {
            return false;
        }
        node->SetId(UInt32Value(id));
        node->SetDuration(value.Duration ? StringValue(std::to_string(*value.Duration))
                                         : StringValue("indefinite"));
        target->SetShapeId(StringValue(std::to_string(value.TargetShapeId)));
        animation->SetFrom(StringValue(value.From));
        animation->SetTo(StringValue(value.To));
        animation->SetBy(StringValue(value.By));
        return true;
    }

    static bool RemoveTargeting(const std::shared_ptr<Packaging::SlidePart>& part, UInt32 shapeId)
    {
        bool removed = false;
        for (const auto& animation : Elements(part))
        {
            if (TargetId(animation) == shapeId)
            {
                auto parent = animation->Parent();
                removed = (parent && parent->RemoveChild(animation)) || removed;
            }
        }
        return removed;
    }
};

/**
 * Reads and rebuilds the PresentationML timing sequences behind
 * PresentationSlide::AnimationEffects and PresentationSlide::SetAnimationEffects.
 *
 * The generated markup mirrors the shape PowerPoint itself writes: a timing root
 * `p:par`, one `p:seq` per sequence, and, inside every sequence, click groups
 * that contain timing groups that contain the effect nodes carrying the
 * `presetClass`/`presetID`/`presetSubtype` triple plus the concrete behaviors.
 */
class PresentationAnimationEffectHelpers
{
public:
    using EffectList = std::vector<PresentationAnimationEffectData>;
    using IdSet = std::unordered_set<UInt32>;
    using NodeMap = std::unordered_map<UInt32, Presentation::ParallelTimeNode::Ptr>;
    using IdAllocator = PresentationTimeNodeIdAllocator;

    // ------------------------------------------------------------------ preset mapping

    /**
     * Values assigned by the PowerPoint animation gallery to `presetID`.
     *
     * The identifier is scoped by `presetClass`, which is why entrance Fly and
     * emphasis ChangeFillColor legitimately share the numeric value 2.
     */
    enum class AnimationPresetId : Int32
    {
        MotionPath = 0,
        Appear = 1,
        Fly = 2,
        ChangeFillColor = 2,
        GrowShrink = 6,
        Spin = 8,
        Fade = 10,
        Wipe = 22,
        Zoom = 23
    };

    /** Bit values PowerPoint stores in `presetSubtype` for modelled directions. */
    enum class AnimationPresetSubtype : Int32
    {
        None = 0,
        Up = 1,
        Right = 2,
        Down = 4,
        Left = 8,
        In = 16,
        Out = 32
    };

    static std::optional<PresentationAnimationEffectClass> ReadClass(
        const EnumValue<Presentation::TimeNodePresetClassValues>& value)
    {
        if (!value.IsDefined())
        {
            return std::nullopt;
        }
        switch (value.Value().GetValue())
        {
            case Presentation::TimeNodePresetClassValues::Entrance:
                return PresentationAnimationEffectClass::Entrance;
            case Presentation::TimeNodePresetClassValues::Emphasis:
                return PresentationAnimationEffectClass::Emphasis;
            case Presentation::TimeNodePresetClassValues::Exit:
                return PresentationAnimationEffectClass::Exit;
            case Presentation::TimeNodePresetClassValues::Path:
                return PresentationAnimationEffectClass::MotionPath;
            default:
                return std::nullopt;
        }
    }

    static Presentation::TimeNodePresetClassValues::Value WriteClass(PresentationAnimationEffectClass value)
    {
        switch (value)
        {
            case PresentationAnimationEffectClass::Emphasis:
                return Presentation::TimeNodePresetClassValues::Emphasis;
            case PresentationAnimationEffectClass::Exit:
                return Presentation::TimeNodePresetClassValues::Exit;
            case PresentationAnimationEffectClass::MotionPath:
                return Presentation::TimeNodePresetClassValues::Path;
            default:
                return Presentation::TimeNodePresetClassValues::Entrance;
        }
    }

    /// True when the effect belongs to the gallery, mirroring the table in the public header.
    static bool IsSupportedPair(PresentationAnimationEffectClass effectClass, PresentationAnimationEffect effect)
    {
        switch (effectClass)
        {
            case PresentationAnimationEffectClass::Entrance:
            case PresentationAnimationEffectClass::Exit:
                return effect == PresentationAnimationEffect::Appear || effect == PresentationAnimationEffect::Fade ||
                       effect == PresentationAnimationEffect::Fly || effect == PresentationAnimationEffect::Wipe ||
                       effect == PresentationAnimationEffect::Zoom;
            case PresentationAnimationEffectClass::Emphasis:
                return effect == PresentationAnimationEffect::GrowShrink ||
                       effect == PresentationAnimationEffect::Spin ||
                       effect == PresentationAnimationEffect::ChangeFillColor;
            case PresentationAnimationEffectClass::MotionPath:
                return effect == PresentationAnimationEffect::MotionPath;
            default:
                return false;
        }
    }

    static AnimationPresetId PresetId(PresentationAnimationEffectClass effectClass,
                                      PresentationAnimationEffect effect)
    {
        if (effectClass == PresentationAnimationEffectClass::Emphasis)
        {
            switch (effect)
            {
                case PresentationAnimationEffect::ChangeFillColor:
                    return AnimationPresetId::ChangeFillColor;
                case PresentationAnimationEffect::GrowShrink:
                    return AnimationPresetId::GrowShrink;
                default:
                    return AnimationPresetId::Spin;
            }
        }
        if (effectClass == PresentationAnimationEffectClass::MotionPath)
        {
            return AnimationPresetId::MotionPath;
        }
        switch (effect)
        {
            case PresentationAnimationEffect::Appear:
                return AnimationPresetId::Appear;
            case PresentationAnimationEffect::Fly:
                return AnimationPresetId::Fly;
            case PresentationAnimationEffect::Wipe:
                return AnimationPresetId::Wipe;
            case PresentationAnimationEffect::Zoom:
                return AnimationPresetId::Zoom;
            default:
                return AnimationPresetId::Fade;
        }
    }

    static PresentationAnimationEffect ReadEffect(PresentationAnimationEffectClass effectClass, Int32 presetId)
    {
        if (effectClass == PresentationAnimationEffectClass::MotionPath)
        {
            return PresentationAnimationEffect::MotionPath;
        }
        if (effectClass == PresentationAnimationEffectClass::Emphasis)
        {
            switch (presetId)
            {
                case static_cast<Int32>(AnimationPresetId::ChangeFillColor):
                    return PresentationAnimationEffect::ChangeFillColor;
                case static_cast<Int32>(AnimationPresetId::GrowShrink):
                    return PresentationAnimationEffect::GrowShrink;
                case static_cast<Int32>(AnimationPresetId::Spin):
                    return PresentationAnimationEffect::Spin;
                default:
                    return PresentationAnimationEffect::Unsupported;
            }
        }
        switch (presetId)
        {
            case static_cast<Int32>(AnimationPresetId::Appear):
                return PresentationAnimationEffect::Appear;
            case static_cast<Int32>(AnimationPresetId::Fly):
                return PresentationAnimationEffect::Fly;
            case static_cast<Int32>(AnimationPresetId::Fade):
                return PresentationAnimationEffect::Fade;
            case static_cast<Int32>(AnimationPresetId::Wipe):
                return PresentationAnimationEffect::Wipe;
            case static_cast<Int32>(AnimationPresetId::Zoom):
                return PresentationAnimationEffect::Zoom;
            default:
                return PresentationAnimationEffect::Unsupported;
        }
    }

    /// PowerPoint encodes Fly and Wipe directions as a preset subtype bit value.
    static AnimationPresetSubtype PresetSubtype(
        PresentationAnimationEffect effect, const std::optional<PresentationAnimationDirection>& direction)
    {
        if (!direction)
        {
            return AnimationPresetSubtype::None;
        }
        if (effect == PresentationAnimationEffect::Zoom)
        {
            return *direction == PresentationAnimationDirection::In ? AnimationPresetSubtype::In
                                                                    : AnimationPresetSubtype::Out;
        }
        switch (*direction)
        {
            case PresentationAnimationDirection::Up:
                return AnimationPresetSubtype::Up;
            case PresentationAnimationDirection::Right:
                return AnimationPresetSubtype::Right;
            case PresentationAnimationDirection::Down:
                return AnimationPresetSubtype::Down;
            case PresentationAnimationDirection::Left:
                return AnimationPresetSubtype::Left;
            default:
                return AnimationPresetSubtype::None;
        }
    }

    static std::optional<PresentationAnimationDirection> ReadDirection(PresentationAnimationEffect effect,
                                                                       Int32 subtype)
    {
        if (effect == PresentationAnimationEffect::Zoom)
        {
            return subtype == static_cast<Int32>(AnimationPresetSubtype::Out)
                       ? PresentationAnimationDirection::Out
                       : PresentationAnimationDirection::In;
        }
        if (effect != PresentationAnimationEffect::Fly && effect != PresentationAnimationEffect::Wipe)
        {
            return std::nullopt;
        }
        switch (subtype)
        {
            case static_cast<Int32>(AnimationPresetSubtype::Up):
                return PresentationAnimationDirection::Up;
            case static_cast<Int32>(AnimationPresetSubtype::Right):
                return PresentationAnimationDirection::Right;
            case static_cast<Int32>(AnimationPresetSubtype::Down):
                return PresentationAnimationDirection::Down;
            default:
                return PresentationAnimationDirection::Left;
        }
    }

    static Presentation::TimeNodeValues::Value WriteTrigger(PresentationAnimationTrigger trigger)
    {
        switch (trigger)
        {
            case PresentationAnimationTrigger::WithPrevious:
                return Presentation::TimeNodeValues::WithEffect;
            case PresentationAnimationTrigger::AfterPrevious:
                return Presentation::TimeNodeValues::AfterEffect;
            default:
                return Presentation::TimeNodeValues::ClickEffect;
        }
    }

    static PresentationAnimationTrigger ReadTrigger(const EnumValue<Presentation::TimeNodeValues>& value)
    {
        if (value.IsDefined())
        {
            switch (value.Value().GetValue())
            {
                case Presentation::TimeNodeValues::WithEffect:
                case Presentation::TimeNodeValues::WithGroup:
                    return PresentationAnimationTrigger::WithPrevious;
                case Presentation::TimeNodeValues::AfterEffect:
                case Presentation::TimeNodeValues::AfterGroup:
                    return PresentationAnimationTrigger::AfterPrevious;
                default:
                    break;
            }
        }
        return PresentationAnimationTrigger::OnClick;
    }

    // ------------------------------------------------------------------ navigation

    static Presentation::Timing::Ptr TimingTree(const std::shared_ptr<Packaging::SlidePart>& part, bool create)
    {
        auto slide = part ? part->GetTypedRootElement() : nullptr;
        if (!slide)
        {
            return nullptr;
        }
        auto timing = slide->GetFirstChildOfType<Presentation::Timing>();
        if (!timing && create)
        {
            timing = slide->AppendChild<Presentation::Timing>();
        }
        return timing;
    }

    static Presentation::TimeNodeList::Ptr RootList(const std::shared_ptr<Packaging::SlidePart>& part, bool create)
    {
        auto timing = TimingTree(part, create);
        if (!timing)
        {
            return nullptr;
        }
        auto list = timing->GetFirstChildOfType<Presentation::TimeNodeList>();
        if (!list && create)
        {
            list = timing->AppendChild<Presentation::TimeNodeList>();
        }
        return list;
    }

    static bool IsBehavior(const std::shared_ptr<OpenXMLElement>& element)
    {
        return std::dynamic_pointer_cast<Presentation::SetBehavior>(element) != nullptr ||
               std::dynamic_pointer_cast<Presentation::Animate>(element) != nullptr ||
               std::dynamic_pointer_cast<Presentation::AnimateEffect>(element) != nullptr ||
               std::dynamic_pointer_cast<Presentation::AnimateColor>(element) != nullptr ||
               std::dynamic_pointer_cast<Presentation::AnimateMotion>(element) != nullptr ||
               std::dynamic_pointer_cast<Presentation::AnimateRotation>(element) != nullptr ||
               std::dynamic_pointer_cast<Presentation::AnimateScale>(element) != nullptr ||
               std::dynamic_pointer_cast<Presentation::Command>(element) != nullptr;
    }

    static Presentation::ChildTimeNodeList::Ptr Children(const Presentation::ParallelTimeNode::Ptr& node)
    {
        auto common = node ? node->GetFirstChildOfType<Presentation::CommonTimeNode>() : nullptr;
        return common ? common->GetFirstChildOfType<Presentation::ChildTimeNodeList>() : nullptr;
    }

    /// Descends through the click and timing groups and yields the leaf effect nodes in order.
    static void CollectEffects(const Presentation::ChildTimeNodeList::Ptr& list,
                               std::vector<Presentation::ParallelTimeNode::Ptr>& output)
    {
        if (!list)
        {
            return;
        }
        for (const auto& child : list->Elements<Presentation::ParallelTimeNode>())
        {
            auto children = Children(child);
            bool leaf = false;
            if (children)
            {
                for (const auto& candidate : children->Children())
                {
                    if (IsBehavior(candidate))
                    {
                        leaf = true;
                        break;
                    }
                }
            }
            if (leaf)
            {
                output.push_back(child);
            }
            else
            {
                CollectEffects(children, output);
            }
        }
    }

    static UInt32 ShapeId(const Presentation::ShapeTarget::Ptr& target)
    {
        if (!target)
        {
            return 0;
        }
        const auto text = target->GetShapeId().ToString();
        UInt32 result = 0;
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
        return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() ? result : 0;
    }

    static UInt32 SequenceTrigger(const Presentation::SequenceTimeNode::Ptr& sequence)
    {
        auto node = sequence ? sequence->GetFirstChildOfType<Presentation::CommonTimeNode>() : nullptr;
        auto conditions = node ? node->GetFirstChildOfType<Presentation::StartConditionList>() : nullptr;
        if (!conditions)
        {
            return 0;
        }
        for (const auto& condition : conditions->Elements<Presentation::Condition>())
        {
            const auto targets = condition->Descendants<Presentation::ShapeTarget>();
            if (!targets.empty())
            {
                return ShapeId(targets.front());
            }
        }
        return 0;
    }

    static std::optional<UInt32> ParseTime(const StringValue& value)
    {
        const auto text = value.ToString();
        if (text.empty())
        {
            return std::nullopt;
        }
        UInt32 result = 0;
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
        if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
        {
            return std::nullopt;
        }
        return result;
    }

    // ------------------------------------------------------------------ reading

    static PresentationAnimationTiming ReadTiming(const Presentation::CommonTimeNode::Ptr& node)
    {
        PresentationAnimationTiming timing;
        timing.Duration = ParseTime(node->GetDuration()).value_or(0);
        const auto repeat = node->GetRepeatCount();
        if (repeat.ToString() == "indefinite")
        {
            timing.RepeatIndefinitely = true;
        }
        else if (const auto count = ParseTime(repeat); count && *count >= 2000)
        {
            timing.RepeatCount = *count / 1000;
        }
        timing.AutoReverse = node->GetAutoReverse().ValueOr(false);
        timing.Acceleration = static_cast<UInt32>(std::max(0, node->GetAcceleration().ValueOr(0)));
        timing.Deceleration = static_cast<UInt32>(std::max(0, node->GetDeceleration().ValueOr(0)));
        if (auto conditions = node->GetFirstChildOfType<Presentation::StartConditionList>())
        {
            const auto list = conditions->Elements<Presentation::Condition>();
            if (!list.empty())
            {
                timing.Delay = ParseTime(list.front()->GetDelay()).value_or(0);
            }
        }
        return timing;
    }

    static PresentationAnimationEffectData Read(const Presentation::ParallelTimeNode::Ptr& effect,
                                                UInt32 triggerShapeId)
    {
        PresentationAnimationEffectData data;
        data.TriggerShapeId = triggerShapeId;
        auto node = effect->GetFirstChildOfType<Presentation::CommonTimeNode>();
        if (!node)
        {
            data.Effect = PresentationAnimationEffect::Unsupported;
            return data;
        }
        data.Id = node->GetId().ValueOr(0);
        data.Trigger = ReadTrigger(node->GetNodeType());
        data.Timing = ReadTiming(node);

        const auto targets = effect->Descendants<Presentation::ShapeTarget>();
        data.TargetShapeId = targets.empty() ? 0 : ShapeId(targets.front());

        const auto effectClass = ReadClass(node->GetPresetClass());
        if (!effectClass || data.TargetShapeId == 0)
        {
            data.Effect = PresentationAnimationEffect::Unsupported;
            return data;
        }
        data.Class = *effectClass;
        data.Effect = ReadEffect(*effectClass, node->GetPresetId().ValueOr(-1));
        if (data.Effect == PresentationAnimationEffect::Unsupported)
        {
            return data;
        }
        data.Direction = ReadDirection(data.Effect, node->GetPresetSubtype().ValueOr(0));
        ReadParameters(effect, data);
        return data;
    }

    static void ReadParameters(const Presentation::ParallelTimeNode::Ptr& effect,
                               PresentationAnimationEffectData& data)
    {
        switch (data.Effect)
        {
            case PresentationAnimationEffect::GrowShrink:
            {
                const auto scales = effect->Descendants<Presentation::AnimateScale>();
                auto by = scales.empty() ? nullptr : scales.front()->GetFirstChildOfType<Presentation::ByPosition>();
                data.ScalePercent = by ? std::optional<Int32>(by->GetX().ValueOr(0) / 1000) : std::nullopt;
                break;
            }
            case PresentationAnimationEffect::Spin:
            {
                const auto rotations = effect->Descendants<Presentation::AnimateRotation>();
                data.RotationDegrees = rotations.empty()
                                           ? std::nullopt
                                           : std::optional<Int32>(rotations.front()->GetBy().ValueOr(0) / 60000);
                break;
            }
            case PresentationAnimationEffect::ChangeFillColor:
            {
                const auto colors = effect->Descendants<Drawing::RgbColorModelHex>();
                if (!colors.empty())
                {
                    data.Color = colors.front()->GetVal().ToString();
                }
                break;
            }
            case PresentationAnimationEffect::MotionPath:
            {
                const auto motions = effect->Descendants<Presentation::AnimateMotion>();
                if (!motions.empty())
                {
                    data.MotionPath = motions.front()->GetPath().ToString();
                }
                break;
            }
            default:
                break;
        }
    }

    static EffectList ReadAll(const std::shared_ptr<Packaging::SlidePart>& part, NodeMap* opaque)
    {
        EffectList result;
        auto timing = TimingTree(part, false);
        if (!timing)
        {
            return result;
        }
        for (const auto& sequence : timing->Descendants<Presentation::SequenceTimeNode>())
        {
            const auto trigger = SequenceTrigger(sequence);
            auto node = sequence->GetFirstChildOfType<Presentation::CommonTimeNode>();
            std::vector<Presentation::ParallelTimeNode::Ptr> nodes;
            CollectEffects(node ? node->GetFirstChildOfType<Presentation::ChildTimeNodeList>() : nullptr, nodes);
            for (const auto& effect : nodes)
            {
                auto data = Read(effect, trigger);
                if (opaque && data.Effect == PresentationAnimationEffect::Unsupported)
                {
                    (*opaque)[data.Id] = effect;
                }
                result.push_back(std::move(data));
            }
        }
        return result;
    }

    // ------------------------------------------------------------------ validation

    static bool ValidateParameters(const PresentationAnimationEffectData& effect)
    {
        const bool usesDirection = effect.Effect == PresentationAnimationEffect::Fly ||
                                   effect.Effect == PresentationAnimationEffect::Wipe ||
                                   effect.Effect == PresentationAnimationEffect::Zoom;
        if (effect.Direction.has_value() != usesDirection)
        {
            return false;
        }
        if (effect.Direction)
        {
            const bool inOut = *effect.Direction == PresentationAnimationDirection::In ||
                               *effect.Direction == PresentationAnimationDirection::Out;
            if (inOut != (effect.Effect == PresentationAnimationEffect::Zoom))
            {
                return false;
            }
        }
        if (effect.ScalePercent.has_value() != (effect.Effect == PresentationAnimationEffect::GrowShrink) ||
            (effect.ScalePercent && *effect.ScalePercent <= 0))
        {
            return false;
        }
        if (effect.RotationDegrees.has_value() != (effect.Effect == PresentationAnimationEffect::Spin))
        {
            return false;
        }
        if (effect.Color.has_value() != (effect.Effect == PresentationAnimationEffect::ChangeFillColor))
        {
            return false;
        }
        if (effect.Color && (effect.Color->size() != 6 ||
                             !std::all_of(effect.Color->begin(), effect.Color->end(), [](unsigned char character)
                                          { return std::isxdigit(character) != 0; })))
        {
            return false;
        }
        if (effect.MotionPath.has_value() != (effect.Effect == PresentationAnimationEffect::MotionPath) ||
            (effect.MotionPath && effect.MotionPath->empty()))
        {
            return false;
        }
        return true;
    }

    static bool ValidateTiming(const PresentationAnimationTiming& timing)
    {
        if (timing.Duration == 0)
        {
            return false;
        }
        if (timing.RepeatCount && (*timing.RepeatCount < 2 || timing.RepeatIndefinitely))
        {
            return false;
        }
        return static_cast<UInt64>(timing.Acceleration) + timing.Deceleration <= 100000;
    }

    static bool Validate(const std::shared_ptr<Packaging::SlidePart>& part, const EffectList& effects,
                         const NodeMap& opaque)
    {
        IdSet identifiers;
        for (const auto& effect : effects)
        {
            if (effect.Id != 0 && !identifiers.insert(effect.Id).second)
            {
                return false;
            }
            if (!PresentationAnimationHelpers::ShapeExists(part, effect.TargetShapeId) ||
                (effect.TriggerShapeId != 0 &&
                 !PresentationAnimationHelpers::ShapeExists(part, effect.TriggerShapeId)))
            {
                return false;
            }
            if (effect.Effect == PresentationAnimationEffect::Unsupported)
            {
                if (opaque.find(effect.Id) == opaque.end())
                {
                    return false;
                }
                continue;
            }
            if (!IsSupportedPair(effect.Class, effect.Effect) || !ValidateTiming(effect.Timing) ||
                !ValidateParameters(effect))
            {
                return false;
            }
        }
        return true;
    }

    // ------------------------------------------------------------------ behavior construction

    /// Builds `p:cBhvr` with its time node, shape target, and optional animated attribute names.
    static bool AddCommonBehavior(const std::shared_ptr<OpenXMLElement>& owner, IdAllocator& ids,
                                  UInt32 shapeId, UInt32 duration, UInt32 delay,
                                  const std::vector<std::string>& attributes)
    {
        auto behavior = owner->AppendChild<Presentation::CommonBehavior>();
        auto node = behavior ? behavior->AppendChild<Presentation::CommonTimeNode>() : nullptr;
        auto targetElement = behavior ? behavior->AppendChild<Presentation::TargetElement>() : nullptr;
        auto target = targetElement ? targetElement->AppendChild<Presentation::ShapeTarget>() : nullptr;
        if (!node || !target)
        {
            return false;
        }
        node->SetId(UInt32Value(ids.Next()));
        node->SetDuration(StringValue(std::to_string(duration)));
        node->SetFill(EnumValue<Presentation::TimeNodeFillValues>(Presentation::TimeNodeFillValues::Hold));
        if (delay != 0 && !AddStartCondition(node, delay))
        {
            return false;
        }
        target->SetShapeId(StringValue(std::to_string(shapeId)));
        if (attributes.empty())
        {
            return true;
        }
        auto names = behavior->AppendChild<Presentation::AttributeNameList>();
        if (!names)
        {
            return false;
        }
        for (const auto& attribute : attributes)
        {
            auto name = names->AppendChild<Presentation::AttributeName>();
            if (!name)
            {
                return false;
            }
            name->SetText(attribute);
        }
        return true;
    }

    static bool AddStartCondition(const Presentation::CommonTimeNode::Ptr& node, UInt32 delay)
    {
        auto conditions = node->AppendChild<Presentation::StartConditionList>();
        auto condition = conditions ? conditions->AppendChild<Presentation::Condition>() : nullptr;
        if (!condition)
        {
            return false;
        }
        condition->SetDelay(StringValue(std::to_string(delay)));
        return true;
    }

    /// Emits the `p:set` behavior PowerPoint uses to show or hide the shape.
    static bool AddVisibility(const Presentation::ChildTimeNodeList::Ptr& list, IdAllocator& ids,
                              UInt32 shapeId, bool visible, UInt32 delay)
    {
        auto behavior = list->AppendChild<Presentation::SetBehavior>();
        if (!behavior || !AddCommonBehavior(behavior, ids, shapeId, 1, delay, {"style.visibility"}))
        {
            return false;
        }
        auto to = behavior->AppendChild<Presentation::ToVariantValue>();
        auto value = to ? to->AppendChild<Presentation::StringVariantValue>() : nullptr;
        if (!value)
        {
            return false;
        }
        value->SetVal(StringValue(visible ? "visible" : "hidden"));
        return true;
    }

    static bool AddFilter(const Presentation::ChildTimeNodeList::Ptr& list, IdAllocator& ids, UInt32 shapeId,
                          UInt32 duration, const std::string& filter, bool entering)
    {
        auto behavior = list->AppendChild<Presentation::AnimateEffect>();
        if (!behavior)
        {
            return false;
        }
        behavior->SetTransition(EnumValue<Presentation::AnimateEffectTransitionValues>(
            entering ? Presentation::AnimateEffectTransitionValues::In
                     : Presentation::AnimateEffectTransitionValues::Out));
        behavior->SetFilter(StringValue(filter));
        return AddCommonBehavior(behavior, ids, shapeId, duration, 0, {});
    }

    static std::string WipeFilter(PresentationAnimationDirection direction)
    {
        switch (direction)
        {
            case PresentationAnimationDirection::Up:
                return "wipe(up)";
            case PresentationAnimationDirection::Right:
                return "wipe(right)";
            case PresentationAnimationDirection::Down:
                return "wipe(down)";
            default:
                return "wipe(left)";
        }
    }

    /// Off-slide coordinate expression for the given fly-in or fly-out edge.
    static std::string FlyEdge(PresentationAnimationDirection direction)
    {
        switch (direction)
        {
            case PresentationAnimationDirection::Up:
                return "0-#ppt_h/2";
            case PresentationAnimationDirection::Right:
                return "1+#ppt_w/2";
            case PresentationAnimationDirection::Down:
                return "1+#ppt_h/2";
            default:
                return "0-#ppt_w/2";
        }
    }

    static bool IsVerticalFly(PresentationAnimationDirection direction)
    {
        return direction == PresentationAnimationDirection::Up || direction == PresentationAnimationDirection::Down;
    }

    static bool AddFly(const Presentation::ChildTimeNodeList::Ptr& list, IdAllocator& ids, UInt32 shapeId,
                       UInt32 duration, PresentationAnimationDirection direction, bool entering)
    {
        const std::string attribute = IsVerticalFly(direction) ? "ppt_y" : "ppt_x";
        const std::string anchor = "#" + attribute;
        const std::string edge = FlyEdge(direction);
        auto behavior = list->AppendChild<Presentation::Animate>();
        if (!behavior)
        {
            return false;
        }
        behavior->SetCalculationMode(EnumValue<Presentation::AnimateBehaviorCalculateModeValues>(
            Presentation::AnimateBehaviorCalculateModeValues::Linear));
        behavior->SetValueType(
            EnumValue<Presentation::AnimateBehaviorValues>(Presentation::AnimateBehaviorValues::Number));
        behavior->SetFrom(StringValue(entering ? edge : anchor));
        behavior->SetTo(StringValue(entering ? anchor : edge));
        return AddCommonBehavior(behavior, ids, shapeId, duration, 0, {attribute});
    }

    static bool AddScale(const Presentation::ChildTimeNodeList::Ptr& list, IdAllocator& ids, UInt32 shapeId,
                         UInt32 duration, std::optional<Int32> from, std::optional<Int32> to,
                         std::optional<Int32> by)
    {
        auto behavior = list->AppendChild<Presentation::AnimateScale>();
        if (!behavior || !AddCommonBehavior(behavior, ids, shapeId, duration, 0, {}))
        {
            return false;
        }
        if (from)
        {
            auto point = behavior->AppendChild<Presentation::FromPosition>();
            if (!point)
            {
                return false;
            }
            point->SetX(Int32Value(*from));
            point->SetY(Int32Value(*from));
        }
        if (to)
        {
            auto point = behavior->AppendChild<Presentation::ToPosition>();
            if (!point)
            {
                return false;
            }
            point->SetX(Int32Value(*to));
            point->SetY(Int32Value(*to));
        }
        if (by)
        {
            auto point = behavior->AppendChild<Presentation::ByPosition>();
            if (!point)
            {
                return false;
            }
            point->SetX(Int32Value(*by));
            point->SetY(Int32Value(*by));
        }
        return true;
    }

    /// Writes the behaviors that realize one effect inside its own `p:childTnLst`.
    static bool AddBehaviors(const Presentation::ChildTimeNodeList::Ptr& list, IdAllocator& ids,
                             const PresentationAnimationEffectData& effect)
    {
        const auto shapeId = effect.TargetShapeId;
        const auto duration = effect.Timing.Duration;
        const auto entering = effect.Class == PresentationAnimationEffectClass::Entrance;
        const auto exiting = effect.Class == PresentationAnimationEffectClass::Exit;
        const UInt32 hideDelay = duration > 0 ? duration - 1 : 0;
        if (entering && !AddVisibility(list, ids, shapeId, true, 0))
        {
            return false;
        }
        switch (effect.Effect)
        {
            case PresentationAnimationEffect::Appear:
                break;
            case PresentationAnimationEffect::Fade:
                if (!AddFilter(list, ids, shapeId, duration, "fade", entering))
                {
                    return false;
                }
                break;
            case PresentationAnimationEffect::Fly:
                if (!AddFly(list, ids, shapeId, duration, *effect.Direction, entering))
                {
                    return false;
                }
                break;
            case PresentationAnimationEffect::Wipe:
                if (!AddFilter(list, ids, shapeId, duration, WipeFilter(*effect.Direction), entering))
                {
                    return false;
                }
                break;
            case PresentationAnimationEffect::Zoom:
            {
                const Int32 extreme = *effect.Direction == PresentationAnimationDirection::In ? 0 : 200000;
                if (!AddFilter(list, ids, shapeId, duration, "fade", entering) ||
                    !AddScale(list, ids, shapeId, duration, entering ? extreme : 100000,
                              entering ? 100000 : extreme, std::nullopt))
                {
                    return false;
                }
                break;
            }
            case PresentationAnimationEffect::GrowShrink:
                if (!AddScale(list, ids, shapeId, duration, std::nullopt, std::nullopt, *effect.ScalePercent * 1000))
                {
                    return false;
                }
                break;
            case PresentationAnimationEffect::Spin:
            {
                auto behavior = list->AppendChild<Presentation::AnimateRotation>();
                if (!behavior)
                {
                    return false;
                }
                behavior->SetBy(Int32Value(*effect.RotationDegrees * 60000));
                if (!AddCommonBehavior(behavior, ids, shapeId, duration, 0, {"ppt_r"}))
                {
                    return false;
                }
                break;
            }
            case PresentationAnimationEffect::ChangeFillColor:
            {
                auto behavior = list->AppendChild<Presentation::AnimateColor>();
                if (!behavior)
                {
                    return false;
                }
                behavior->SetColorSpace(
                    EnumValue<Presentation::AnimateColorSpaceValues>(Presentation::AnimateColorSpaceValues::Rgb));
                if (!AddCommonBehavior(behavior, ids, shapeId, duration, 0, {"fillcolor"}))
                {
                    return false;
                }
                auto to = behavior->AppendChild<Presentation::ToColor>();
                auto color = to ? to->AppendChild<Drawing::RgbColorModelHex>() : nullptr;
                if (!color)
                {
                    return false;
                }
                color->SetVal(HexBinaryValue(*effect.Color));
                break;
            }
            case PresentationAnimationEffect::MotionPath:
            {
                auto behavior = list->AppendChild<Presentation::AnimateMotion>();
                if (!behavior)
                {
                    return false;
                }
                behavior->SetOrigin(EnumValue<Presentation::AnimateMotionBehaviorOriginValues>(
                    Presentation::AnimateMotionBehaviorOriginValues::Layout));
                behavior->SetPath(StringValue(*effect.MotionPath));
                behavior->SetPathEditMode(EnumValue<Presentation::AnimateMotionPathEditModeValues>(
                    Presentation::AnimateMotionPathEditModeValues::Relative));
                if (!AddCommonBehavior(behavior, ids, shapeId, duration, 0, {"ppt_x", "ppt_y"}))
                {
                    return false;
                }
                auto center = behavior->AppendChild<Presentation::RotationCenter>();
                if (!center)
                {
                    return false;
                }
                center->SetX(Int32Value(0));
                center->SetY(Int32Value(0));
                break;
            }
            default:
                return false;
        }
        return !exiting || AddVisibility(list, ids, shapeId, false, hideDelay);
    }

    // ------------------------------------------------------------------ sequence construction

    /// Appends an empty `p:par` group node and returns its child list.
    static Presentation::ChildTimeNodeList::Ptr AddGroup(const Presentation::ChildTimeNodeList::Ptr& parent,
                                                         IdAllocator& ids, const StringValue& delay)
    {
        auto group = parent->AppendChild<Presentation::ParallelTimeNode>();
        auto node = group ? group->AppendChild<Presentation::CommonTimeNode>() : nullptr;
        if (!node)
        {
            return nullptr;
        }
        node->SetId(UInt32Value(ids.Next()));
        node->SetFill(EnumValue<Presentation::TimeNodeFillValues>(Presentation::TimeNodeFillValues::Hold));
        auto conditions = node->AppendChild<Presentation::StartConditionList>();
        auto condition = conditions ? conditions->AppendChild<Presentation::Condition>() : nullptr;
        if (!condition)
        {
            return nullptr;
        }
        condition->SetDelay(delay);
        return node->AppendChild<Presentation::ChildTimeNodeList>();
    }

    static bool AddEffect(const Presentation::ChildTimeNodeList::Ptr& parent, IdAllocator& ids,
                          const PresentationAnimationEffectData& effect, const NodeMap& opaque,
                          UInt32& assignedId)
    {
        if (effect.Effect == PresentationAnimationEffect::Unsupported)
        {
            const auto original = opaque.find(effect.Id);
            assignedId = effect.Id;
            return original != opaque.end() && original->second->CopyInto(parent) != nullptr;
        }

        auto node = parent->AppendChild<Presentation::ParallelTimeNode>();
        auto common = node ? node->AppendChild<Presentation::CommonTimeNode>() : nullptr;
        if (!common)
        {
            return false;
        }
        assignedId = effect.Id != 0 ? effect.Id : ids.Next();
        common->SetId(UInt32Value(assignedId));
        common->SetPresetId(
            Int32Value(static_cast<Int32>(PresetId(effect.Class, effect.Effect))));
        common->SetPresetClass(EnumValue<Presentation::TimeNodePresetClassValues>(WriteClass(effect.Class)));
        common->SetPresetSubtype(
            Int32Value(static_cast<Int32>(PresetSubtype(effect.Effect, effect.Direction))));
        common->SetDuration(StringValue(std::to_string(effect.Timing.Duration)));
        common->SetFill(EnumValue<Presentation::TimeNodeFillValues>(Presentation::TimeNodeFillValues::Hold));
        common->SetNodeType(EnumValue<Presentation::TimeNodeValues>(WriteTrigger(effect.Trigger)));
        if (effect.Timing.RepeatIndefinitely)
        {
            common->SetRepeatCount(StringValue("indefinite"));
        }
        else if (effect.Timing.RepeatCount)
        {
            common->SetRepeatCount(StringValue(std::to_string(*effect.Timing.RepeatCount * 1000)));
        }
        if (effect.Timing.AutoReverse)
        {
            common->SetAutoReverse(BooleanValue(true));
        }
        if (effect.Timing.Acceleration != 0)
        {
            common->SetAcceleration(Int32Value(static_cast<Int32>(effect.Timing.Acceleration)));
        }
        if (effect.Timing.Deceleration != 0)
        {
            common->SetDeceleration(Int32Value(static_cast<Int32>(effect.Timing.Deceleration)));
        }
        if (!AddStartCondition(common, effect.Timing.Delay))
        {
            return false;
        }
        auto list = common->AppendChild<Presentation::ChildTimeNodeList>();
        return list && AddBehaviors(list, ids, effect);
    }

    static bool AddSlideCondition(const std::shared_ptr<OpenXMLElement>& owner,
                                  Presentation::TriggerEventValues::Value event)
    {
        auto condition = owner->AppendChild<Presentation::Condition>();
        auto target = condition ? condition->AppendChild<Presentation::TargetElement>() : nullptr;
        auto slide = target ? target->AppendChild<Presentation::SlideTarget>() : nullptr;
        if (!slide)
        {
            return false;
        }
        condition->SetEvent(EnumValue<Presentation::TriggerEventValues>(event));
        condition->SetDelay(StringValue("0"));
        return true;
    }

    /// Builds one `p:seq` for the effects in `[first, last)`, which all share a trigger shape.
    static bool AddSequence(const Presentation::ChildTimeNodeList::Ptr& parent, IdAllocator& ids,
                            const EffectList& effects, Size first, Size last,
                            UInt32 triggerShapeId, const NodeMap& opaque, EffectList& written)
    {
        auto sequence = parent->AppendChild<Presentation::SequenceTimeNode>();
        auto node = sequence ? sequence->AppendChild<Presentation::CommonTimeNode>() : nullptr;
        if (!node)
        {
            return false;
        }
        sequence->SetConcurrent(BooleanValue(true));
        sequence->SetNextAction(EnumValue<Presentation::NextActionValues>(Presentation::NextActionValues::Seek));
        node->SetId(UInt32Value(ids.Next()));
        if (triggerShapeId == 0)
        {
            node->SetDuration(StringValue("indefinite"));
            node->SetNodeType(
                EnumValue<Presentation::TimeNodeValues>(Presentation::TimeNodeValues::MainSequence));
        }
        else
        {
            node->SetRestart(
                EnumValue<Presentation::TimeNodeRestartValues>(Presentation::TimeNodeRestartValues::WhenNotActive));
            node->SetFill(EnumValue<Presentation::TimeNodeFillValues>(Presentation::TimeNodeFillValues::Hold));
            node->SetEventFilter(StringValue("cancelBubble"));
            node->SetNodeType(
                EnumValue<Presentation::TimeNodeValues>(Presentation::TimeNodeValues::InteractiveSequence));
            auto conditions = node->AppendChild<Presentation::StartConditionList>();
            auto condition = conditions ? conditions->AppendChild<Presentation::Condition>() : nullptr;
            auto target = condition ? condition->AppendChild<Presentation::TargetElement>() : nullptr;
            auto shape = target ? target->AppendChild<Presentation::ShapeTarget>() : nullptr;
            if (!shape)
            {
                return false;
            }
            condition->SetEvent(
                EnumValue<Presentation::TriggerEventValues>(Presentation::TriggerEventValues::OnClick));
            condition->SetDelay(StringValue("0"));
            shape->SetShapeId(StringValue(std::to_string(triggerShapeId)));
        }

        auto children = node->AppendChild<Presentation::ChildTimeNodeList>();
        if (!children)
        {
            return false;
        }
        Presentation::ChildTimeNodeList::Ptr clickGroup;
        Presentation::ChildTimeNodeList::Ptr timeGroup;
        for (Size index = first; index < last; ++index)
        {
            const auto& effect = effects[index];
            const bool opensClickGroup =
                !clickGroup || (index != first && effect.Trigger == PresentationAnimationTrigger::OnClick);
            if (opensClickGroup)
            {
                clickGroup = AddGroup(children, ids, StringValue("indefinite"));
                timeGroup = clickGroup ? AddGroup(clickGroup, ids, StringValue("0")) : nullptr;
            }
            else if (effect.Trigger == PresentationAnimationTrigger::AfterPrevious)
            {
                timeGroup = AddGroup(clickGroup, ids, StringValue("0"));
            }
            if (!timeGroup)
            {
                return false;
            }
            auto stored = effect;
            if (!AddEffect(timeGroup, ids, effect, opaque, stored.Id))
            {
                return false;
            }
            written.push_back(std::move(stored));
        }

        if (triggerShapeId != 0)
        {
            return true;
        }
        auto previous = sequence->AppendChild<Presentation::PreviousConditionList>();
        auto next = sequence->AppendChild<Presentation::NextConditionList>();
        return previous && next &&
               AddSlideCondition(previous, Presentation::TriggerEventValues::OnPrevious) &&
               AddSlideCondition(next, Presentation::TriggerEventValues::OnNext);
    }

    /// Groups main-sequence effects first, then one run per interactive trigger shape.
    static EffectList Canonicalize(const EffectList& effects)
    {
        EffectList result;
        result.reserve(effects.size());
        std::vector<UInt32> triggers;
        for (const auto& effect : effects)
        {
            if (std::find(triggers.begin(), triggers.end(), effect.TriggerShapeId) == triggers.end())
            {
                triggers.push_back(effect.TriggerShapeId);
            }
        }
        std::stable_sort(triggers.begin(), triggers.end(),
                         [](UInt32 left, UInt32 right)
                         { return left == 0 && right != 0; });
        for (const auto trigger : triggers)
        {
            for (const auto& effect : effects)
            {
                if (effect.TriggerShapeId == trigger)
                {
                    result.push_back(effect);
                }
            }
        }
        return result;
    }

    static IdSet ReservedIdentifiers(const std::shared_ptr<Packaging::SlidePart>& part, const EffectList& effects,
                                     const NodeMap& opaque)
    {
        IdSet reserved;
        auto slide = part ? part->GetTypedRootElement() : nullptr;
        auto timing = TimingTree(part, false);
        if (slide)
        {
            for (const auto& node : slide->Descendants<Presentation::CommonTimeNode>())
            {
                if (!timing || !IsInsideSequences(node, timing))
                {
                    reserved.insert(node->GetId().ValueOr(0));
                }
            }
        }
        for (const auto& entry : opaque)
        {
            for (const auto& node : entry.second->Descendants<Presentation::CommonTimeNode>())
            {
                reserved.insert(node->GetId().ValueOr(0));
            }
            reserved.insert(entry.first);
        }
        for (const auto& effect : effects)
        {
            if (effect.Id != 0)
            {
                reserved.insert(effect.Id);
            }
        }
        reserved.insert(0);
        return reserved;
    }

    /// True when `node` sits below a `p:seq`, that is inside the region rebuilt from scratch.
    static bool IsInsideSequences(const std::shared_ptr<OpenXMLElement>& node,
                                  const Presentation::Timing::Ptr& timing)
    {
        for (auto current = node ? node->Parent() : nullptr; current; current = current->Parent())
        {
            if (std::dynamic_pointer_cast<Presentation::SequenceTimeNode>(current))
            {
                return true;
            }
            if (current->IsSameNode(timing))
            {
                return false;
            }
        }
        return false;
    }

    static bool Rebuild(const std::shared_ptr<Packaging::SlidePart>& part, const EffectList& effects,
                        EffectList* written)
    {
        NodeMap opaque;
        ReadAll(part, &opaque);
        if (!part || !Validate(part, effects, opaque))
        {
            return false;
        }
        const auto ordered = Canonicalize(effects);
        auto list = RootList(part, !ordered.empty());
        if (!list)
        {
            return ordered.empty();
        }

        // The previous roots stay attached while the replacement is built so that
        // opaque effect subtrees can still be deep-copied out of them.
        const auto previousRoots = list->Elements<Presentation::ParallelTimeNode>();
        IdAllocator ids(ReservedIdentifiers(part, ordered, opaque));
        EffectList result;
        Presentation::ParallelTimeNode::Ptr root;
        if (!ordered.empty())
        {
            // `p:tnLst` admits a single timing root, so the replacement is inserted
            // without a schema check and the superseded roots are dropped afterwards.
            const auto existing = list->Children();
            root = list->InsertChildRaw<Presentation::ParallelTimeNode>(existing.empty() ? nullptr
                                                                                         : existing.front());
            auto node = root ? root->AppendChild<Presentation::CommonTimeNode>() : nullptr;
            auto children = node ? node->AppendChild<Presentation::ChildTimeNodeList>() : nullptr;
            if (!children)
            {
                if (root)
                {
                    list->RemoveChild(root);
                }
                return false;
            }
            node->SetId(UInt32Value(ids.Next()));
            node->SetDuration(StringValue("indefinite"));
            node->SetRestart(
                EnumValue<Presentation::TimeNodeRestartValues>(Presentation::TimeNodeRestartValues::Never));
            node->SetNodeType(EnumValue<Presentation::TimeNodeValues>(Presentation::TimeNodeValues::TmingRoot));

            Size index = 0;
            while (index < ordered.size())
            {
                const auto trigger = ordered[index].TriggerShapeId;
                Size end = index;
                while (end < ordered.size() && ordered[end].TriggerShapeId == trigger)
                {
                    ++end;
                }
                if (!AddSequence(children, ids, ordered, index, end, trigger, opaque, result))
                {
                    list->RemoveChild(root);
                    return false;
                }
                index = end;
            }
        }

        for (const auto& previous : previousRoots)
        {
            list->RemoveChild(previous);
        }
        if (written)
        {
            *written = std::move(result);
        }
        return true;
    }

    /// Removes every effect targeting `shapeId`, either as the animated or the trigger shape.
    static bool RemoveTargeting(const std::shared_ptr<Packaging::SlidePart>& part, UInt32 shapeId)
    {
        const auto effects = ReadAll(part, nullptr);
        EffectList remaining;
        for (const auto& effect : effects)
        {
            if (effect.TargetShapeId != shapeId && effect.TriggerShapeId != shapeId)
            {
                remaining.push_back(effect);
            }
        }
        if (remaining.size() == effects.size())
        {
            return false;
        }
        return Rebuild(part, remaining, nullptr);
    }

    static bool Targets(const std::shared_ptr<Packaging::SlidePart>& part, UInt32 shapeId)
    {
        for (const auto& effect : ReadAll(part, nullptr))
        {
            if (effect.TargetShapeId == shapeId || effect.TriggerShapeId == shapeId)
            {
                return true;
            }
        }
        return false;
    }
};

class PresentationCollectionHelpers
{
public:
    static constexpr std::string_view SectionExtensionUri = "{521415D9-36F7-43E2-AB2F-B90AF26B5E84}";

    static std::shared_ptr<Presentation::Presentation> Root(const PowerPointDocument::Ptr& document)
    {
        auto part = document ? document->GetPresentationPart() : nullptr;
        return part ? part->GetTypedRootElement() : nullptr;
    }

    static PowerPoint2010::SectionList::Ptr SectionList(const PowerPointDocument::Ptr& document, bool create)
    {
        auto root = Root(document);
        if (!root)
        {
            return nullptr;
        }
        const auto lists = root->Descendants<PowerPoint2010::SectionList>();
        if (!lists.empty() || !create)
        {
            return lists.empty() ? nullptr : lists.front();
        }
        auto extensions = root->GetFirstChildOfType<Presentation::PresentationExtensionList>();
        if (!extensions)
        {
            extensions = root->AppendChild<Presentation::PresentationExtensionList>();
        }
        auto extension = extensions ? extensions->AppendChild<Presentation::PresentationExtension>() : nullptr;
        if (!extension)
        {
            return nullptr;
        }
        extension->SetUri(StringValue(SectionExtensionUri));
        return extension->AppendChild<PowerPoint2010::SectionList>();
    }

    static std::vector<PresentationSection> Sections(const PowerPointDocument::Ptr& document)
    {
        std::vector<PresentationSection> result;
        auto list = SectionList(document, false);
        if (!list)
        {
            return result;
        }
        for (const auto& element : list->Elements<PowerPoint2010::Section>())
        {
            PresentationSection section;
            section.Id = element->GetId().ToString();
            section.Name = element->GetName().ToString();
            auto slideList = element->GetFirstChildOfType<PowerPoint2010::SectionSlideIdList>();
            if (slideList)
            {
                for (const auto& entry : slideList->Elements<PowerPoint2010::SectionSlideIdListEntry>())
                {
                    section.SlideIds.push_back(entry->GetId().ValueOr(0));
                }
            }
            result.push_back(std::move(section));
        }
        return result;
    }

    static bool WriteSections(const PowerPointDocument::Ptr& document,
                              const std::vector<PresentationSection>& sections)
    {
        auto list = SectionList(document, !sections.empty());
        if (!list)
        {
            return sections.empty();
        }
        for (const auto& child : list->Children())
        {
            list->RemoveChild(child);
        }
        for (const auto& value : sections)
        {
            auto section = list->AppendChild<PowerPoint2010::Section>();
            auto slideList = section ? section->AppendChild<PowerPoint2010::SectionSlideIdList>() : nullptr;
            if (!section || !slideList)
            {
                return false;
            }
            section->SetId(StringValue(value.Id));
            section->SetName(StringValue(value.Name));
            for (const auto slideId : value.SlideIds)
            {
                auto entry = slideList->AppendChild<PowerPoint2010::SectionSlideIdListEntry>();
                if (!entry)
                {
                    return false;
                }
                entry->SetId(UInt32Value(slideId));
            }
        }
        return true;
    }

    static Presentation::CustomShowList::Ptr CustomShowList(const PowerPointDocument::Ptr& document, bool create)
    {
        auto root = Root(document);
        if (!root)
        {
            return nullptr;
        }
        auto list = root->GetFirstChildOfType<Presentation::CustomShowList>();
        if (!list && create)
        {
            list = root->InsertChild<Presentation::CustomShowList>(
                root->GetFirstChildOfType<Presentation::PresentationExtensionList>());
        }
        return list;
    }

    static std::vector<PresentationCustomShow> CustomShows(
        const PowerPointDocument::Ptr& document,
        const std::unordered_map<std::string, UInt32>& slideIds)
    {
        std::vector<PresentationCustomShow> result;
        auto list = CustomShowList(document, false);
        if (!list)
        {
            return result;
        }
        for (const auto& element : list->Elements<Presentation::CustomShow>())
        {
            PresentationCustomShow show;
            show.Id = element->GetId().ValueOr(0);
            show.Name = element->GetName().ToString();
            auto slides = element->GetFirstChildOfType<Presentation::SlideList>();
            if (slides)
            {
                for (const auto& entry : slides->Elements<Presentation::SlideListEntry>())
                {
                    const auto found = slideIds.find(entry->GetId().ToString());
                    if (found != slideIds.end())
                    {
                        show.SlideIds.push_back(found->second);
                    }
                }
            }
            result.push_back(std::move(show));
        }
        return result;
    }

    static bool WriteCustomShows(const PowerPointDocument::Ptr& document,
                                 const std::vector<PresentationCustomShow>& shows,
                                 const std::unordered_map<UInt32, std::string>& relationships)
    {
        auto list = CustomShowList(document, !shows.empty());
        if (!list)
        {
            return shows.empty();
        }
        for (const auto& child : list->Children())
        {
            list->RemoveChild(child);
        }
        for (const auto& value : shows)
        {
            auto show = list->AppendChild<Presentation::CustomShow>();
            auto slides = show ? show->AppendChild<Presentation::SlideList>() : nullptr;
            if (!show || !slides)
            {
                return false;
            }
            show->SetId(UInt32Value(value.Id));
            show->SetName(StringValue(value.Name));
            for (const auto slideId : value.SlideIds)
            {
                const auto found = relationships.find(slideId);
                if (found == relationships.end())
                {
                    return false;
                }
                auto entry = slides->AppendChild<Presentation::SlideListEntry>();
                if (!entry)
                {
                    return false;
                }
                entry->SetId(StringValue(found->second));
            }
        }
        return true;
    }
};

class PresentationCommentHelpers
{
public:
    static PresentationCommentStatus ReadStatus(const EnumValue<ModernComments::CommentStatus>& status)
    {
        switch (status.ValueOr(ModernComments::CommentStatus::active).GetValue())
        {
            case ModernComments::CommentStatus::resolved:
                return PresentationCommentStatus::Resolved;
            case ModernComments::CommentStatus::closed:
                return PresentationCommentStatus::Closed;
            default:
                return PresentationCommentStatus::Active;
        }
    }

    static ModernComments::CommentStatus WriteStatus(PresentationCommentStatus status)
    {
        switch (status)
        {
            case PresentationCommentStatus::Resolved:
                return ModernComments::CommentStatus::resolved;
            case PresentationCommentStatus::Closed:
                return ModernComments::CommentStatus::closed;
            default:
                return ModernComments::CommentStatus::active;
        }
    }

    static std::string Text(const std::shared_ptr<OpenXMLElement>& element)
    {
        std::string result;
        if (!element)
        {
            return result;
        }
        bool first = true;
        for (const auto& paragraph : element->Descendants<Drawing::Paragraph>())
        {
            if (!first)
            {
                result.push_back('\n');
            }
            first = false;
            for (const auto& text : paragraph->Descendants<Drawing::Text>())
            {
                result += text->GetText();
            }
        }
        return result;
    }

    static bool SetText(const std::shared_ptr<OpenXMLElement>& parent, const std::string& value)
    {
        auto body = parent ? parent->AppendChild<ModernComments::TextBodyType>() : nullptr;
        auto bodyProperties = body ? body->AppendChild<Drawing::BodyProperties>() : nullptr;
        auto listStyle = body ? body->AppendChild<Drawing::ListStyle>() : nullptr;
        if (!body || !bodyProperties || !listStyle)
        {
            return false;
        }
        Size start = 0;
        do
        {
            const auto end = value.find('\n', start);
            auto paragraph = body->AppendChild<Drawing::Paragraph>();
            auto run = paragraph ? paragraph->AppendChild<Drawing::Run>() : nullptr;
            auto text = run ? run->AppendChild<Drawing::Text>() : nullptr;
            if (!text)
            {
                return false;
            }
            text->SetText(value.substr(start, end == std::string::npos ? end : end - start));
            if (end == std::string::npos)
            {
                break;
            }
            start = end + 1;
        } while (true);
        return true;
    }

    static PresentationComment Read(const ModernComments::Comment::Ptr& source)
    {
        PresentationComment value;
        value.Id = source->GetId().ToString();
        value.AuthorId = source->GetAuthorId().ToString();
        value.Text = Text(source->GetFirstChildOfType<ModernComments::TextBodyType>());
        value.Status = ReadStatus(source->GetStatus());
        if (auto position = source->GetFirstChildOfType<ModernComments::Point2DType>())
        {
            value.Position = PresentationPoint(position->GetX().ValueOr(0), position->GetY().ValueOr(0));
        }
        if (auto replies = source->GetFirstChildOfType<ModernComments::CommentReplyList>())
        {
            for (const auto& reply : replies->Elements<ModernComments::CommentReply>())
            {
                value.Replies.push_back({reply->GetId().ToString(), reply->GetAuthorId().ToString(),
                                         Text(reply->GetFirstChildOfType<ModernComments::TextBodyType>())});
            }
        }
        return value;
    }

    static bool IsValid(const PresentationComment& value)
    {
        if (value.Id.empty() || value.AuthorId.empty() ||
            !PresentationMeasurementHelpers::ToInt64Emu(value.Position.X) ||
            !PresentationMeasurementHelpers::ToInt64Emu(value.Position.Y))
        {
            return false;
        }
        std::unordered_set<std::string> ids;
        for (const auto& reply : value.Replies)
        {
            if (reply.Id.empty() || reply.AuthorId.empty() || reply.Id == value.Id || !ids.insert(reply.Id).second)
            {
                return false;
            }
        }
        return true;
    }

    static bool AuthorsExist(const std::shared_ptr<Packaging::SlidePart>& slide, const PresentationComment& value)
    {
        std::unordered_set<std::string> required{value.AuthorId};
        for (const auto& reply : value.Replies)
        {
            required.insert(reply.AuthorId);
        }
        auto package = slide ? slide->Package() : nullptr;
        if (!package)
        {
            return false;
        }
        for (const auto& rootPart : package->Parts())
        {
            if (auto presentation = std::dynamic_pointer_cast<Packaging::PresentationPart>(rootPart))
            {
                auto authorsPart = presentation->GetauthorsPart();
                auto authors = authorsPart ? authorsPart->GetTypedRootElement() : nullptr;
                if (!authors)
                {
                    return false;
                }
                for (const auto& author : authors->Elements<ModernComments::Author>())
                {
                    required.erase(author->GetId().ToString());
                }
                return required.empty();
            }
        }
        return false;
    }

    static bool IdsAreUnique(const std::shared_ptr<Packaging::SlidePart>& slide,
                             const PresentationComment& value,
                             std::string_view replacedId = {})
    {
        std::unordered_set<std::string> proposed{value.Id};
        for (const auto& reply : value.Replies)
        {
            proposed.insert(reply.Id);
        }
        auto package = slide ? slide->Package() : nullptr;
        if (!package)
        {
            return false;
        }
        for (const auto& rootPart : package->Parts())
        {
            auto presentation = std::dynamic_pointer_cast<Packaging::PresentationPart>(rootPart);
            if (!presentation)
            {
                continue;
            }
            for (const auto& slidePart : presentation->GetSlideParts())
            {
                for (const auto& commentPart : slidePart->GetcommentParts())
                {
                    auto list = commentPart ? commentPart->GetTypedRootElement() : nullptr;
                    if (!list)
                    {
                        continue;
                    }
                    for (const auto& comment : list->Elements<ModernComments::Comment>())
                    {
                        if (!replacedId.empty() && comment->GetId().ToString() == replacedId)
                        {
                            continue;
                        }
                        if (proposed.contains(comment->GetId().ToString()))
                        {
                            return false;
                        }
                        if (auto replies = comment->GetFirstChildOfType<ModernComments::CommentReplyList>())
                        {
                            for (const auto& reply : replies->Elements<ModernComments::CommentReply>())
                            {
                                if (proposed.contains(reply->GetId().ToString()))
                                {
                                    return false;
                                }
                            }
                        }
                    }
                }
            }
            return true;
        }
        return false;
    }

    /**
     * @brief Appends a comment thread.
     * @param created Creation timestamp to store; the current UTC time is used
     *        when it is undefined. The schema requires the attribute, so an
     *        edited comment passes the timestamp it already carried.
     */
    static ModernComments::Comment::Ptr Append(const ModernComments::CommentList::Ptr& list,
                                               const PresentationComment& value,
                                               const DateTimeValue& created = {})
    {
        if (!list || !IsValid(value))
        {
            return nullptr;
        }
        auto comment = list->AppendChild<ModernComments::Comment>();
        comment->SetId(StringValue(value.Id));
        comment->SetAuthorId(StringValue(value.AuthorId));
        comment->SetCreated(created.IsDefined() ? created : DateTimeValue(std::chrono::system_clock::now()));
        comment->SetStatus(EnumValue<ModernComments::CommentStatus>(WriteStatus(value.Status)));
        auto position = comment->AppendChild<ModernComments::Point2DType>();
        position->SetX(Int64Value(*PresentationMeasurementHelpers::ToInt64Emu(value.Position.X)));
        position->SetY(Int64Value(*PresentationMeasurementHelpers::ToInt64Emu(value.Position.Y)));
        if (!value.Replies.empty())
        {
            auto replies = comment->AppendChild<ModernComments::CommentReplyList>();
            for (const auto& valueReply : value.Replies)
            {
                auto reply = replies->AppendChild<ModernComments::CommentReply>();
                reply->SetId(StringValue(valueReply.Id));
                reply->SetAuthorId(StringValue(valueReply.AuthorId));
                reply->SetCreated(DateTimeValue(std::chrono::system_clock::now()));
                if (!SetText(reply, valueReply.Text))
                {
                    list->RemoveChild(comment);
                    return nullptr;
                }
            }
        }
        if (!SetText(comment, value.Text))
        {
            list->RemoveChild(comment);
            return nullptr;
        }
        return comment;
    }
};

class PresentationTableHelpers
{
public:
    static bool Overlaps(const PresentationTableMerge& left, const PresentationTableMerge& right)
    {
        return left.Row < right.Row + right.RowSpan && right.Row < left.Row + left.RowSpan &&
               left.Column < right.Column + right.ColumnSpan && right.Column < left.Column + left.ColumnSpan;
    }

    static bool IsValid(const PresentationTableData& table)
    {
        if (table.ColumnWidths.empty() || table.Rows.empty() ||
            !PresentationMeasurementHelpers::IsNonNegative(table.Transform.Size.Width) ||
            !PresentationMeasurementHelpers::IsNonNegative(table.Transform.Size.Height) ||
            table.Transform.GroupChildPosition || table.Transform.GroupChildSize)
        {
            return false;
        }
        if (std::any_of(table.ColumnWidths.begin(), table.ColumnWidths.end(), [](const auto& width)
                        { return !PresentationMeasurementHelpers::IsNonNegative(width) ||
                                 !PresentationMeasurementHelpers::ToInt64Emu(width); }))
        {
            return false;
        }
        for (const auto& row : table.Rows)
        {
            if (!PresentationMeasurementHelpers::IsNonNegative(row.Height) ||
                !PresentationMeasurementHelpers::ToInt64Emu(row.Height) ||
                row.Cells.size() != table.ColumnWidths.size())
            {
                return false;
            }
        }
        for (Size index = 0; index < table.Merges.size(); ++index)
        {
            const auto& merge = table.Merges[index];
            if (merge.RowSpan == 0 || merge.ColumnSpan == 0 || (merge.RowSpan == 1 && merge.ColumnSpan == 1) ||
                merge.Row >= table.Rows.size() || merge.Column >= table.ColumnWidths.size() ||
                merge.RowSpan > table.Rows.size() - merge.Row ||
                merge.ColumnSpan > table.ColumnWidths.size() - merge.Column)
            {
                return false;
            }
            for (Size other = 0; other < index; ++other)
            {
                if (Overlaps(merge, table.Merges[other]))
                {
                    return false;
                }
            }
        }
        return true;
    }

    static std::shared_ptr<Drawing::Table> Find(const std::shared_ptr<OpenXMLElement>& element)
    {
        auto frame = std::dynamic_pointer_cast<Presentation::GraphicFrame>(element);
        auto graphic = frame ? frame->GetFirstChildOfType<Drawing::Graphic>() : nullptr;
        auto data = graphic ? graphic->GetFirstChildOfType<Drawing::GraphicData>() : nullptr;
        if (!data || data->GetUri().ToString() != TableGraphicDataUri)
        {
            return nullptr;
        }
        return data->GetFirstChildOfType<Drawing::Table>();
    }

    static std::string CellText(const std::shared_ptr<Drawing::TableCell>& cell)
    {
        std::string result;
        auto body = cell ? cell->GetFirstChildOfType<Drawing::TextBody>() : nullptr;
        if (!body)
        {
            return result;
        }
        bool first = true;
        for (const auto& paragraph : body->Elements<Drawing::Paragraph>())
        {
            if (!first)
            {
                result.push_back('\n');
            }
            first = false;
            for (const auto& text : paragraph->Descendants<Drawing::Text>())
            {
                result += text->GetText();
            }
        }
        return result;
    }

    static bool SetCellText(const std::shared_ptr<Drawing::TableCell>& cell, const std::string& value)
    {
        auto body = cell->AppendChild<Drawing::TextBody>();
        auto bodyProperties = body ? body->AppendChild<Drawing::BodyProperties>() : nullptr;
        auto listStyle = body ? body->AppendChild<Drawing::ListStyle>() : nullptr;
        if (!body || !bodyProperties || !listStyle)
        {
            return false;
        }
        Size start = 0;
        do
        {
            const auto end = value.find('\n', start);
            auto paragraph = body->AppendChild<Drawing::Paragraph>();
            if (!paragraph)
            {
                return false;
            }
            const auto segment = value.substr(start, end == std::string::npos ? end : end - start);
            if (!segment.empty())
            {
                auto run = paragraph->AppendChild<Drawing::Run>();
                auto properties = run ? run->AppendChild<Drawing::RunProperties>() : nullptr;
                auto text = run ? run->AppendChild<Drawing::Text>() : nullptr;
                if (!run || !properties || !text)
                {
                    return false;
                }
                text->SetText(segment);
            }
            if (end == std::string::npos)
            {
                break;
            }
            start = end + 1;
        } while (true);
        return true;
    }
};

PresentationShape::PresentationShape(std::shared_ptr<OpenXMLElement> element,
                                     std::shared_ptr<Packaging::SlidePart> slidePart)
    : m_element(std::move(element)), m_slidePart(std::move(slidePart))
{
}

std::shared_ptr<OpenXMLElement> PresentationShape::GetElement() const
{
    return m_element;
}

UInt32 PresentationShape::Id() const
{
    return PresentationMediaHelpers::ShapeId(m_element);
}

bool PresentationShape::IsGroup() const noexcept
{
    return std::dynamic_pointer_cast<Presentation::GroupShape>(m_element) != nullptr;
}

std::vector<PresentationShape::Ptr> PresentationShape::Children() const
{
    std::vector<Ptr> result;
    if (!IsGroup())
    {
        return result;
    }
    for (const auto& child : PresentationShapeTreeHelpers::Elements(m_element))
    {
        result.push_back(Ptr(new PresentationShape(child, m_slidePart)));
    }
    return result;
}

std::optional<PresentationShapeTransform> PresentationShape::GetTransform() const
{
    auto host = PresentationShapeTreeHelpers::TransformHost(m_element);
    if (!host)
    {
        return std::nullopt;
    }
    const bool group = IsGroup();
    auto drawingTransform =
        group ? std::static_pointer_cast<OpenXMLElement>(host->GetFirstChildOfType<Drawing::TransformGroup>())
              : std::static_pointer_cast<OpenXMLElement>(host->GetFirstChildOfType<Drawing::Transform2D>());
    auto frameTransform = group || drawingTransform ? nullptr : host->GetFirstChildOfType<Presentation::Transform>();
    auto transform = drawingTransform ? drawingTransform : std::static_pointer_cast<OpenXMLElement>(frameTransform);
    PresentationShapeTransform result;
    if (!transform)
    {
        return result;
    }
    if (auto offset = transform->GetFirstChildOfType<Drawing::Offset>())
    {
        result.Position = {offset->GetX().ValueOr(0), offset->GetY().ValueOr(0)};
    }
    if (auto extents = transform->GetFirstChildOfType<Drawing::Extents>())
    {
        result.Size = {extents->GetCx().ValueOr(0), extents->GetCy().ValueOr(0)};
    }
    if (auto typed = std::dynamic_pointer_cast<Drawing::TransformGroup>(transform))
    {
        result.Rotation = typed->GetRotation().ValueOr(0);
        result.FlipHorizontal = typed->GetHorizontalFlip().ValueOr(false);
        result.FlipVertical = typed->GetVerticalFlip().ValueOr(false);
        auto childOffset = typed->GetFirstChildOfType<Drawing::ChildOffset>();
        auto childExtents = typed->GetFirstChildOfType<Drawing::ChildExtents>();
        result.GroupChildPosition = PresentationPoint{childOffset ? childOffset->GetX().ValueOr(0) : 0,
                                                      childOffset ? childOffset->GetY().ValueOr(0) : 0};
        result.GroupChildSize = PresentationSize{childExtents ? childExtents->GetCx().ValueOr(0) : 0,
                                                 childExtents ? childExtents->GetCy().ValueOr(0) : 0};
    }
    else if (auto transform2D = std::dynamic_pointer_cast<Drawing::Transform2D>(transform))
    {
        result.Rotation = transform2D->GetRotation().ValueOr(0);
        result.FlipHorizontal = transform2D->GetHorizontalFlip().ValueOr(false);
        result.FlipVertical = transform2D->GetVerticalFlip().ValueOr(false);
    }
    else if (frameTransform)
    {
        result.Rotation = frameTransform->GetRotation().ValueOr(0);
        result.FlipHorizontal = frameTransform->GetHorizontalFlip().ValueOr(false);
        result.FlipVertical = frameTransform->GetVerticalFlip().ValueOr(false);
    }
    return result;
}

bool PresentationShape::SetTransform(const PresentationShapeTransform& value)
{
    const bool group = IsGroup();
    const auto x = PresentationMeasurementHelpers::ToInt64Emu(value.Position.X);
    const auto y = PresentationMeasurementHelpers::ToInt64Emu(value.Position.Y);
    const auto width = PresentationMeasurementHelpers::ToInt64Emu(value.Size.Width);
    const auto height = PresentationMeasurementHelpers::ToInt64Emu(value.Size.Height);
    const auto childX = value.GroupChildPosition
                            ? PresentationMeasurementHelpers::ToInt64Emu(value.GroupChildPosition->X)
                            : std::optional<Int64>{};
    const auto childY = value.GroupChildPosition
                            ? PresentationMeasurementHelpers::ToInt64Emu(value.GroupChildPosition->Y)
                            : std::optional<Int64>{};
    const auto childWidth = value.GroupChildSize
                                ? PresentationMeasurementHelpers::ToInt64Emu(value.GroupChildSize->Width)
                                : std::optional<Int64>{};
    const auto childHeight = value.GroupChildSize
                                 ? PresentationMeasurementHelpers::ToInt64Emu(value.GroupChildSize->Height)
                                 : std::optional<Int64>{};
    if (!x || !y || !width || !height || !PresentationMeasurementHelpers::IsNonNegative(value.Size.Width) ||
        !PresentationMeasurementHelpers::IsNonNegative(value.Size.Height) ||
        value.GroupChildSize.has_value() != value.GroupChildPosition.has_value() ||
        (group != value.GroupChildSize.has_value()) ||
        (value.GroupChildSize && (!childX || !childY || !childWidth || !childHeight ||
                                  !PresentationMeasurementHelpers::IsNonNegative(value.GroupChildSize->Width) ||
                                  !PresentationMeasurementHelpers::IsNonNegative(value.GroupChildSize->Height))))
    {
        return false;
    }
    auto host = PresentationShapeTreeHelpers::EnsureTransformHost(m_element);
    if (!host)
    {
        return false;
    }
    std::shared_ptr<OpenXMLElement> transform;
    if (group)
    {
        transform = host->GetFirstChildOfType<Drawing::TransformGroup>();
    }
    else if (std::dynamic_pointer_cast<Presentation::GraphicFrame>(m_element))
    {
        transform = host->GetFirstChildOfType<Presentation::Transform>();
    }
    else
    {
        transform = host->GetFirstChildOfType<Drawing::Transform2D>();
    }
    if (!transform)
    {
        if (group)
        {
            transform = host->AppendChild<Drawing::TransformGroup>();
        }
        else if (std::dynamic_pointer_cast<Presentation::GraphicFrame>(m_element))
        {
            transform = host->AppendChild<Presentation::Transform>();
        }
        else
        {
            transform = host->AppendChild<Drawing::Transform2D>();
        }
    }
    if (!transform)
    {
        return false;
    }
    auto offset = transform->GetFirstChildOfType<Drawing::Offset>();
    if (!offset)
    {
        offset = transform->AppendChild<Drawing::Offset>();
    }
    auto extents = transform->GetFirstChildOfType<Drawing::Extents>();
    if (!extents)
    {
        extents = transform->AppendChild<Drawing::Extents>();
    }
    if (!offset || !extents)
    {
        return false;
    }
    offset->SetX(Int64Value(*x));
    offset->SetY(Int64Value(*y));
    extents->SetCx(Int64Value(*width));
    extents->SetCy(Int64Value(*height));
    if (auto typed = std::dynamic_pointer_cast<Drawing::TransformGroup>(transform))
    {
        typed->SetRotation(Int32Value(value.Rotation));
        typed->SetHorizontalFlip(BooleanValue(value.FlipHorizontal));
        typed->SetVerticalFlip(BooleanValue(value.FlipVertical));
        auto childOffset = typed->GetFirstChildOfType<Drawing::ChildOffset>();
        if (!childOffset)
        {
            childOffset = typed->AppendChild<Drawing::ChildOffset>();
        }
        auto childExtents = typed->GetFirstChildOfType<Drawing::ChildExtents>();
        if (!childExtents)
        {
            childExtents = typed->AppendChild<Drawing::ChildExtents>();
        }
        if (!childOffset || !childExtents)
        {
            return false;
        }
        childOffset->SetX(Int64Value(*childX));
        childOffset->SetY(Int64Value(*childY));
        childExtents->SetCx(Int64Value(*childWidth));
        childExtents->SetCy(Int64Value(*childHeight));
    }
    else if (auto transform2D = std::dynamic_pointer_cast<Drawing::Transform2D>(transform))
    {
        transform2D->SetRotation(Int32Value(value.Rotation));
        transform2D->SetHorizontalFlip(BooleanValue(value.FlipHorizontal));
        transform2D->SetVerticalFlip(BooleanValue(value.FlipVertical));
    }
    else if (auto presentationTransform = std::dynamic_pointer_cast<Presentation::Transform>(transform))
    {
        presentationTransform->SetRotation(Int32Value(value.Rotation));
        presentationTransform->SetHorizontalFlip(BooleanValue(value.FlipHorizontal));
        presentationTransform->SetVerticalFlip(BooleanValue(value.FlipVertical));
    }
    return true;
}

std::optional<Drawing::ShapeTypeValues::Value> PresentationShape::GetPresetGeometry() const
{
    auto host = PresentationShapeTreeHelpers::TransformHost(m_element);
    auto geometry = host ? host->GetFirstChildOfType<Drawing::PresetGeometry>() : nullptr;
    if (!geometry || !geometry->GetPreset().IsDefined())
    {
        return std::nullopt;
    }
    return geometry->GetPreset().Value().GetValue();
}

std::vector<PresentationGeometryAdjustment> PresentationShape::GeometryAdjustments() const
{
    std::vector<PresentationGeometryAdjustment> result;
    auto host = PresentationShapeTreeHelpers::TransformHost(m_element);
    auto geometry = host ? host->GetFirstChildOfType<Drawing::PresetGeometry>() : nullptr;
    auto list = geometry ? geometry->GetFirstChildOfType<Drawing::AdjustValueList>() : nullptr;
    if (list)
    {
        for (const auto& guide : list->Elements<Drawing::ShapeGuide>())
        {
            result.push_back({guide->GetName().ToString(), guide->GetFormula().ToString()});
        }
    }
    return result;
}

bool PresentationShape::SetPresetGeometry(Drawing::ShapeTypeValues::Value preset,
                                          const std::vector<PresentationGeometryAdjustment>& adjustments)
{
    if (preset == Drawing::ShapeTypeValues::NotDefinedEnumValue || preset == Drawing::ShapeTypeValues::InvalidEnumValue)
    {
        return false;
    }
    for (const auto& adjustment : adjustments)
    {
        if (adjustment.Name.empty() || adjustment.Formula.empty())
        {
            return false;
        }
    }
    auto host = PresentationShapeTreeHelpers::EnsureTransformHost(m_element);
    if (!host || IsGroup() || std::dynamic_pointer_cast<Presentation::GraphicFrame>(m_element))
    {
        return false;
    }
    if (auto old = host->GetFirstChildOfType<Drawing::PresetGeometry>())
    {
        host->RemoveChild(old);
    }
    if (auto old = host->GetFirstChildOfType<Drawing::CustomGeometry>())
    {
        host->RemoveChild(old);
    }
    auto geometry = host->AppendChild<Drawing::PresetGeometry>();
    auto list = geometry ? geometry->AppendChild<Drawing::AdjustValueList>() : nullptr;
    if (!geometry || !list)
    {
        return false;
    }
    geometry->SetPreset(EnumValue<Drawing::ShapeTypeValues>(Drawing::ShapeTypeValues(preset)));
    for (const auto& adjustment : adjustments)
    {
        auto guide = list->AppendChild<Drawing::ShapeGuide>();
        if (!guide)
        {
            return false;
        }
        guide->SetName(StringValue(adjustment.Name));
        guide->SetFormula(StringValue(adjustment.Formula));
    }
    return true;
}

std::vector<PresentationFreeformPath> PresentationShape::FreeformPaths() const
{
    std::vector<PresentationFreeformPath> result;
    auto host = PresentationShapeTreeHelpers::TransformHost(m_element);
    auto geometry = host ? host->GetFirstChildOfType<Drawing::CustomGeometry>() : nullptr;
    auto list = geometry ? geometry->GetFirstChildOfType<Drawing::PathList>() : nullptr;
    if (!list)
    {
        return result;
    }
    for (const auto& path : list->Elements<Drawing::Path>())
    {
        PresentationFreeformPath value{
            path->GetWidth().ValueOr(0), path->GetHeight().ValueOr(0), path->GetStroke().ValueOr(true), {}};
        for (const auto& child : path->Children())
        {
            PresentationPathCommand command;
            if (std::dynamic_pointer_cast<Drawing::MoveTo>(child))
            {
                command.Type = PresentationPathCommandType::MoveTo;
            }
            else if (std::dynamic_pointer_cast<Drawing::LineTo>(child))
            {
                command.Type = PresentationPathCommandType::LineTo;
            }
            else if (std::dynamic_pointer_cast<Drawing::QuadraticBezierCurveTo>(child))
            {
                command.Type = PresentationPathCommandType::QuadraticBezierTo;
            }
            else if (std::dynamic_pointer_cast<Drawing::CubicBezierCurveTo>(child))
            {
                command.Type = PresentationPathCommandType::CubicBezierTo;
            }
            else if (std::dynamic_pointer_cast<Drawing::CloseShapePath>(child))
            {
                command.Type = PresentationPathCommandType::Close;
            }
            else
            {
                continue;
            }
            for (const auto& point : child->Elements<Drawing::Point>())
            {
                command.Points.push_back({point->GetX().ToString(), point->GetY().ToString()});
            }
            value.Commands.push_back(std::move(command));
        }
        result.push_back(std::move(value));
    }
    return result;
}

std::vector<PresentationConnectionSite> PresentationShape::ConnectionSites() const
{
    std::vector<PresentationConnectionSite> result;
    auto host = PresentationShapeTreeHelpers::TransformHost(m_element);
    auto geometry = host ? host->GetFirstChildOfType<Drawing::CustomGeometry>() : nullptr;
    auto list = geometry ? geometry->GetFirstChildOfType<Drawing::ConnectionSiteList>() : nullptr;
    if (list)
    {
        for (const auto& site : list->Elements<Drawing::ConnectionSite>())
        {
            auto position = site->GetFirstChildOfType<Drawing::Position>();
            result.push_back({site->GetAngle().ToString(),
                              {position ? position->GetX().ToString() : std::string{},
                               position ? position->GetY().ToString() : std::string{}}});
        }
    }
    return result;
}

bool PresentationShape::SetFreeformGeometry(const std::vector<PresentationFreeformPath>& paths,
                                            const std::vector<PresentationConnectionSite>& sites)
{
    if (paths.empty())
    {
        return false;
    }
    const auto expectedPoints = [](PresentationPathCommandType type) -> Size
    {
        switch (type)
        {
            case PresentationPathCommandType::MoveTo:
            case PresentationPathCommandType::LineTo:
                return 1;
            case PresentationPathCommandType::QuadraticBezierTo:
                return 2;
            case PresentationPathCommandType::CubicBezierTo:
                return 3;
            case PresentationPathCommandType::Close:
                return 0;
        }
        return 0;
    };
    for (const auto& path : paths)
    {
        if (path.Width < 0 || path.Height < 0 || path.Commands.empty())
        {
            return false;
        }
        for (const auto& command : path.Commands)
        {
            if (command.Points.size() != expectedPoints(command.Type))
            {
                return false;
            }
        }
    }
    for (const auto& site : sites)
    {
        if (site.Angle.empty() || site.Position.X.empty() || site.Position.Y.empty())
        {
            return false;
        }
    }
    auto host = PresentationShapeTreeHelpers::EnsureTransformHost(m_element);
    if (!host || IsGroup() || std::dynamic_pointer_cast<Presentation::GraphicFrame>(m_element))
    {
        return false;
    }
    if (auto old = host->GetFirstChildOfType<Drawing::PresetGeometry>())
    {
        host->RemoveChild(old);
    }
    if (auto old = host->GetFirstChildOfType<Drawing::CustomGeometry>())
    {
        host->RemoveChild(old);
    }
    auto geometry = host->AppendChild<Drawing::CustomGeometry>();
    auto adjust = geometry ? geometry->AppendChild<Drawing::AdjustValueList>() : nullptr;
    auto guides = geometry ? geometry->AppendChild<Drawing::ShapeGuideList>() : nullptr;
    auto handles = geometry ? geometry->AppendChild<Drawing::AdjustHandleList>() : nullptr;
    auto connections = geometry ? geometry->AppendChild<Drawing::ConnectionSiteList>() : nullptr;
    auto pathList = geometry ? geometry->AppendChild<Drawing::PathList>() : nullptr;
    if (!geometry || !adjust || !guides || !handles || !connections || !pathList)
    {
        return false;
    }
    for (const auto& siteValue : sites)
    {
        auto site = connections->AppendChild<Drawing::ConnectionSite>();
        auto position = site ? site->AppendChild<Drawing::Position>() : nullptr;
        if (!site || !position)
        {
            return false;
        }
        site->SetAngle(StringValue(siteValue.Angle));
        position->SetX(StringValue(siteValue.Position.X));
        position->SetY(StringValue(siteValue.Position.Y));
    }
    for (const auto& pathValue : paths)
    {
        auto path = pathList->AppendChild<Drawing::Path>();
        if (!path)
        {
            return false;
        }
        path->SetWidth(Int64Value(pathValue.Width));
        path->SetHeight(Int64Value(pathValue.Height));
        path->SetStroke(BooleanValue(pathValue.Stroke));
        for (const auto& commandValue : pathValue.Commands)
        {
            std::shared_ptr<OpenXMLElement> command;
            switch (commandValue.Type)
            {
                case PresentationPathCommandType::MoveTo:
                    command = path->AppendChild<Drawing::MoveTo>();
                    break;
                case PresentationPathCommandType::LineTo:
                    command = path->AppendChild<Drawing::LineTo>();
                    break;
                case PresentationPathCommandType::QuadraticBezierTo:
                    command = path->AppendChild<Drawing::QuadraticBezierCurveTo>();
                    break;
                case PresentationPathCommandType::CubicBezierTo:
                    command = path->AppendChild<Drawing::CubicBezierCurveTo>();
                    break;
                case PresentationPathCommandType::Close:
                    command = path->AppendChild<Drawing::CloseShapePath>();
                    break;
            }
            if (!command)
            {
                return false;
            }
            for (const auto& pointValue : commandValue.Points)
            {
                auto point = command->AppendChild<Drawing::Point>();
                if (!point)
                {
                    return false;
                }
                point->SetX(StringValue(pointValue.X));
                point->SetY(StringValue(pointValue.Y));
            }
        }
    }
    return true;
}

std::optional<PresentationConnectorEndpoint> PresentationShape::StartEndpoint() const
{
    auto nv = m_element ? m_element->GetFirstChildOfType<Presentation::NonVisualConnectionShapeProperties>() : nullptr;
    auto props = nv ? nv->GetFirstChildOfType<Presentation::NonVisualConnectorShapeDrawingProperties>() : nullptr;
    auto endpoint = props ? props->GetFirstChildOfType<Drawing::StartConnection>() : nullptr;
    return endpoint ? std::optional<PresentationConnectorEndpoint>{{endpoint->GetId().ValueOr(0),
                                                                    endpoint->GetIndex().ValueOr(0)}}
                    : std::nullopt;
}

std::optional<PresentationConnectorEndpoint> PresentationShape::EndEndpoint() const
{
    auto nv = m_element ? m_element->GetFirstChildOfType<Presentation::NonVisualConnectionShapeProperties>() : nullptr;
    auto props = nv ? nv->GetFirstChildOfType<Presentation::NonVisualConnectorShapeDrawingProperties>() : nullptr;
    auto endpoint = props ? props->GetFirstChildOfType<Drawing::EndConnection>() : nullptr;
    return endpoint ? std::optional<PresentationConnectorEndpoint>{{endpoint->GetId().ValueOr(0),
                                                                    endpoint->GetIndex().ValueOr(0)}}
                    : std::nullopt;
}

bool PresentationShape::SetConnectorEndpoints(std::optional<PresentationConnectorEndpoint> start,
                                              std::optional<PresentationConnectorEndpoint> end)
{
    auto connector = std::dynamic_pointer_cast<Presentation::ConnectionShape>(m_element);
    if (!connector)
    {
        return false;
    }
    auto nv = connector->GetFirstChildOfType<Presentation::NonVisualConnectionShapeProperties>();
    auto props = nv ? nv->GetFirstChildOfType<Presentation::NonVisualConnectorShapeDrawingProperties>() : nullptr;
    if (!props)
    {
        return false;
    }
    if (auto old = props->GetFirstChildOfType<Drawing::StartConnection>())
    {
        props->RemoveChild(old);
    }
    if (auto old = props->GetFirstChildOfType<Drawing::EndConnection>())
    {
        props->RemoveChild(old);
    }
    if (start)
    {
        auto value = props->AppendChild<Drawing::StartConnection>();
        if (!value)
        {
            return false;
        }
        value->SetId(UInt32Value(start->ShapeId));
        value->SetIndex(UInt32Value(start->SiteIndex));
    }
    if (end)
    {
        auto value = props->AppendChild<Drawing::EndConnection>();
        if (!value)
        {
            return false;
        }
        value->SetId(UInt32Value(end->ShapeId));
        value->SetIndex(UInt32Value(end->SiteIndex));
    }
    return true;
}

class PresentationShapeStyleHelpers
{
public:
    // Fill and outline apply to the visual shape properties (a:spPr) hosted by
    // auto shapes, pictures, and connectors. Groups (grpSpPr) and graphic frames
    // (tables, charts, SmartArt, OLE) do not carry a CT_ShapeProperties fill/line.
    static bool SupportsShapeStyle(const std::shared_ptr<OpenXMLElement>& element)
    {
        return std::dynamic_pointer_cast<Presentation::Shape>(element) != nullptr ||
               std::dynamic_pointer_cast<Presentation::Picture>(element) != nullptr ||
               std::dynamic_pointer_cast<Presentation::ConnectionShape>(element) != nullptr;
    }

    // Removes every EG_FillProperties child so a new fill can replace any prior one.
    static void RemoveFillChildren(const std::shared_ptr<OpenXMLElement>& host)
    {
        if (auto child = host->GetFirstChildOfType<Drawing::NoFill>())
        {
            host->RemoveChild(child);
        }
        if (auto child = host->GetFirstChildOfType<Drawing::SolidFill>())
        {
            host->RemoveChild(child);
        }
        if (auto child = host->GetFirstChildOfType<Drawing::GradientFill>())
        {
            host->RemoveChild(child);
        }
        if (auto child = host->GetFirstChildOfType<Drawing::BlipFill>())
        {
            host->RemoveChild(child);
        }
        if (auto child = host->GetFirstChildOfType<Drawing::PatternFill>())
        {
            host->RemoveChild(child);
        }
        if (auto child = host->GetFirstChildOfType<Drawing::GroupFill>())
        {
            host->RemoveChild(child);
        }
    }
};

std::optional<PresentationShapeFill> PresentationShape::GetFill() const
{
    if (!PresentationShapeStyleHelpers::SupportsShapeStyle(m_element))
    {
        return std::nullopt;
    }
    PresentationShapeFill result;
    auto host = PresentationShapeTreeHelpers::TransformHost(m_element);
    if (!host)
    {
        return result;
    }
    if (host->GetFirstChildOfType<Drawing::NoFill>())
    {
        result.Kind = PresentationFillKind::None;
    }
    else if (auto solid = host->GetFirstChildOfType<Drawing::SolidFill>())
    {
        result.Kind = PresentationFillKind::Solid;
        if (auto color = PresentationTextEffectHelpers::ReadColor(solid))
        {
            result.ColorValue = *color;
        }
    }
    else if (auto gradient = host->GetFirstChildOfType<Drawing::GradientFill>())
    {
        result.Kind = PresentationFillKind::Gradient;
        if (auto list = gradient->GetFirstChildOfType<Drawing::GradientStopList>())
        {
            for (const auto& stop : list->Elements<Drawing::GradientStop>())
            {
                PresentationGradientStop value;
                value.Position = static_cast<Real>(stop->GetPosition().ValueOr(0)) / 1000.0;
                if (auto color = PresentationTextEffectHelpers::ReadColor(stop))
                {
                    value.ColorValue = *color;
                }
                result.GradientStops.push_back(value);
            }
        }
        if (auto linear = gradient->GetFirstChildOfType<Drawing::LinearGradientFill>())
        {
            result.GradientAngle = PresentationTextEffectHelpers::FromDrawingMlAngle(linear->GetAngle().ValueOr(0));
        }
    }
    return result;
}

bool PresentationShape::SetFill(const PresentationShapeFill& fill)
{
    if (!PresentationShapeStyleHelpers::SupportsShapeStyle(m_element))
    {
        return false;
    }
    switch (fill.Kind)
    {
        case PresentationFillKind::Solid:
            if (fill.ColorValue.IsAuto())
            {
                return false;
            }
            break;
        case PresentationFillKind::Gradient:
        {
            if (fill.GradientStops.size() < 2 || !PresentationTextEffectHelpers::ToDrawingMlAngle(fill.GradientAngle))
            {
                return false;
            }
            for (const auto& stop : fill.GradientStops)
            {
                if (stop.ColorValue.IsAuto() || !std::isfinite(stop.Position) || stop.Position < 0.0 ||
                    stop.Position > 100.0)
                {
                    return false;
                }
            }
            break;
        }
        case PresentationFillKind::None:
        case PresentationFillKind::Inherited:
            break;
    }
    auto host = PresentationShapeTreeHelpers::EnsureTransformHost(m_element);
    if (!host)
    {
        return false;
    }
    PresentationShapeStyleHelpers::RemoveFillChildren(host);
    switch (fill.Kind)
    {
        case PresentationFillKind::Inherited:
            break;
        case PresentationFillKind::None:
        {
            auto noFill = host->AppendChild<Drawing::NoFill>();
            if (!noFill)
            {
                return false;
            }
            break;
        }
        case PresentationFillKind::Solid:
        {
            auto solid = host->AppendChild<Drawing::SolidFill>();
            if (!solid || !PresentationTextEffectHelpers::AppendColor(solid, fill.ColorValue))
            {
                return false;
            }
            break;
        }
        case PresentationFillKind::Gradient:
        {
            auto gradient = host->AppendChild<Drawing::GradientFill>();
            auto list = gradient ? gradient->AppendChild<Drawing::GradientStopList>() : nullptr;
            if (!gradient || !list)
            {
                return false;
            }
            for (const auto& stop : fill.GradientStops)
            {
                auto element = list->AppendChild<Drawing::GradientStop>();
                if (!element || !PresentationTextEffectHelpers::AppendColor(element, stop.ColorValue))
                {
                    return false;
                }
                element->SetPosition(Int32Value(static_cast<Int32>(std::llround(stop.Position * 1000.0))));
            }
            auto linear = gradient->AppendChild<Drawing::LinearGradientFill>();
            if (!linear)
            {
                return false;
            }
            linear->SetAngle(Int32Value(*PresentationTextEffectHelpers::ToDrawingMlAngle(fill.GradientAngle)));
            linear->SetScaled(BooleanValue(true));
            break;
        }
    }
    return true;
}

std::optional<PresentationShapeOutline> PresentationShape::GetOutline() const
{
    if (!PresentationShapeStyleHelpers::SupportsShapeStyle(m_element))
    {
        return std::nullopt;
    }
    auto host = PresentationShapeTreeHelpers::TransformHost(m_element);
    auto line = host ? host->GetFirstChildOfType<Drawing::Outline>() : nullptr;
    if (!line)
    {
        return std::nullopt;
    }
    PresentationShapeOutline result;
    if (line->GetWidth().IsDefined())
    {
        result.Width = PresentationMeasurementHelpers::FromEmu(line->GetWidth().Value());
    }
    if (line->GetCapType().IsDefined())
    {
        result.Cap = line->GetCapType().Value().GetValue();
    }
    if (line->GetCompoundLineType().IsDefined())
    {
        result.Compound = line->GetCompoundLineType().Value().GetValue();
    }
    if (auto dash = line->GetFirstChildOfType<Drawing::PresetDash>(); dash && dash->GetVal().IsDefined())
    {
        result.Dash = dash->GetVal().Value().GetValue();
    }
    if (line->GetFirstChildOfType<Drawing::NoFill>())
    {
        result.Fill = PresentationFillKind::None;
    }
    else if (auto solid = line->GetFirstChildOfType<Drawing::SolidFill>())
    {
        result.Fill = PresentationFillKind::Solid;
        if (auto color = PresentationTextEffectHelpers::ReadColor(solid))
        {
            result.ColorValue = *color;
        }
    }
    else
    {
        result.Fill = PresentationFillKind::Inherited;
    }
    return result;
}

bool PresentationShape::SetOutline(const PresentationShapeOutline& outline)
{
    if (!PresentationShapeStyleHelpers::SupportsShapeStyle(m_element))
    {
        return false;
    }
    if (outline.Fill == PresentationFillKind::Gradient)
    {
        return false;
    }
    if (outline.Fill == PresentationFillKind::Solid && outline.ColorValue.IsAuto())
    {
        return false;
    }
    std::optional<Int64> width;
    if (outline.Width)
    {
        if (!PresentationMeasurementHelpers::IsNonNegative(*outline.Width))
        {
            return false;
        }
        width = PresentationMeasurementHelpers::ToInt64Emu(*outline.Width);
        if (!width || *width > std::numeric_limits<Int32>::max())
        {
            return false;
        }
    }
    if (outline.Dash && !Drawing::PresetLineDashValues(*outline.Dash).IsValid())
    {
        return false;
    }
    if (outline.Cap && !Drawing::LineCapValues(*outline.Cap).IsValid())
    {
        return false;
    }
    if (outline.Compound && !Drawing::CompoundLineValues(*outline.Compound).IsValid())
    {
        return false;
    }
    auto host = PresentationShapeTreeHelpers::EnsureTransformHost(m_element);
    if (!host)
    {
        return false;
    }
    if (auto old = host->GetFirstChildOfType<Drawing::Outline>())
    {
        host->RemoveChild(old);
    }
    auto line = host->AppendChild<Drawing::Outline>();
    if (!line)
    {
        return false;
    }
    if (width)
    {
        line->SetWidth(Int32Value(static_cast<Int32>(*width)));
    }
    if (outline.Cap)
    {
        line->SetCapType(EnumValue<Drawing::LineCapValues>(Drawing::LineCapValues(*outline.Cap)));
    }
    if (outline.Compound)
    {
        line->SetCompoundLineType(EnumValue<Drawing::CompoundLineValues>(Drawing::CompoundLineValues(*outline.Compound)));
    }
    if (outline.Fill == PresentationFillKind::None)
    {
        auto noFill = line->AppendChild<Drawing::NoFill>();
        if (!noFill)
        {
            return false;
        }
    }
    else if (outline.Fill == PresentationFillKind::Solid)
    {
        auto solid = line->AppendChild<Drawing::SolidFill>();
        if (!solid || !PresentationTextEffectHelpers::AppendColor(solid, outline.ColorValue))
        {
            return false;
        }
    }
    if (outline.Dash)
    {
        auto dash = line->AppendChild<Drawing::PresetDash>();
        if (!dash)
        {
            return false;
        }
        dash->SetVal(EnumValue<Drawing::PresetLineDashValues>(Drawing::PresetLineDashValues(*outline.Dash)));
    }
    return true;
}

std::optional<PresentationShapeEffects> PresentationShape::GetEffects() const
{
    if (!PresentationShapeStyleHelpers::SupportsShapeStyle(m_element))
    {
        return std::nullopt;
    }
    PresentationShapeEffects result;
    auto host = PresentationShapeTreeHelpers::TransformHost(m_element);
    auto effects = host ? host->GetFirstChildOfType<Drawing::EffectList>() : nullptr;
    if (effects)
    {
        result.Glow = PresentationTextEffectHelpers::ReadGlow(effects);
        result.Shadow = PresentationTextEffectHelpers::ReadShadow(effects);
        result.Reflection = PresentationTextEffectHelpers::ReadReflection(effects);
    }
    return result;
}

bool PresentationShape::SetEffects(const PresentationShapeEffects& value)
{
    if (!PresentationShapeStyleHelpers::SupportsShapeStyle(m_element))
    {
        return false;
    }
    if ((value.Glow && !PresentationTextEffectHelpers::IsValid(*value.Glow)) ||
        (value.Shadow && !PresentationTextEffectHelpers::IsValid(*value.Shadow)) ||
        (value.Reflection && !PresentationTextEffectHelpers::IsValid(*value.Reflection)))
    {
        return false;
    }
    auto host = PresentationShapeTreeHelpers::EnsureTransformHost(m_element);
    if (!host)
    {
        return false;
    }
    if (auto old = host->GetFirstChildOfType<Drawing::EffectList>())
    {
        host->RemoveChild(old);
    }
    if (!value.Glow && !value.Shadow && !value.Reflection)
    {
        return true;
    }
    auto effects = host->AppendChild<Drawing::EffectList>();
    if (!effects)
    {
        return false;
    }
    if (value.Glow && !PresentationTextEffectHelpers::WriteGlow(effects, *value.Glow))
    {
        return false;
    }
    if (value.Shadow && !PresentationTextEffectHelpers::WriteShadow(effects, *value.Shadow))
    {
        return false;
    }
    if (value.Reflection && !PresentationTextEffectHelpers::WriteReflection(effects, *value.Reflection))
    {
        return false;
    }
    return true;
}

std::optional<PresentationTextFrame> PresentationShape::GetTextFrame() const
{
    auto shape = std::dynamic_pointer_cast<Presentation::Shape>(m_element);
    auto body = shape ? shape->GetFirstChildOfType<Presentation::TextBody>() : nullptr;
    if (!body)
    {
        return std::nullopt;
    }
    PresentationTextFrame result;
    if (auto properties = body->GetFirstChildOfType<Drawing::BodyProperties>())
    {
        result.LeftMargin = PresentationMeasurementHelpers::FromEmu(properties->GetLeftInset().ValueOr(91440));
        result.TopMargin = PresentationMeasurementHelpers::FromEmu(properties->GetTopInset().ValueOr(45720));
        result.RightMargin = PresentationMeasurementHelpers::FromEmu(properties->GetRightInset().ValueOr(91440));
        result.BottomMargin = PresentationMeasurementHelpers::FromEmu(properties->GetBottomInset().ValueOr(45720));
        result.Vertical = properties->GetVertical().ValueOr(Drawing::TextVerticalValues::Horizontal).GetValue();
        result.Anchor = properties->GetAnchor().ValueOr(Drawing::TextAnchoringTypeValues::Top).GetValue();
        auto scene = properties->GetFirstChildOfType<Drawing::Scene3DType>();
        auto shape3D = properties->GetFirstChildOfType<Drawing::Shape3DType>();
        if (scene && shape3D)
        {
            PresentationText3D value;
            value.Depth = PresentationMeasurementHelpers::FromEmu(shape3D->GetZ().ValueOr(0));
            value.ExtrusionHeight = PresentationMeasurementHelpers::FromEmu(shape3D->GetExtrusionHeight().ValueOr(0));
            value.ContourWidth = PresentationMeasurementHelpers::FromEmu(shape3D->GetContourWidth().ValueOr(0));
            value.Material = shape3D->GetPresetMaterial().ValueOr(Drawing::PresetMaterialTypeValues::Matte).GetValue();
            if (auto camera = scene->GetFirstChildOfType<Drawing::Camera>())
            {
                value.Camera = camera->GetPreset().ValueOr(Drawing::PresetCameraValues::OrthographicFront).GetValue();
            }
            if (auto light = scene->GetFirstChildOfType<Drawing::LightRig>())
            {
                value.LightRig = light->GetRig().ValueOr(Drawing::LightRigValues::ThreePoints).GetValue();
                value.LightDirection = light->GetDirection().ValueOr(Drawing::LightRigDirectionValues::Top).GetValue();
            }
            if (auto bevel = shape3D->GetFirstChildOfType<Drawing::BevelTop>())
            {
                value.TopBevel = PresentationTextBevel{
                    PresentationMeasurementHelpers::FromEmu(bevel->GetWidth().ValueOr(0)),
                    PresentationMeasurementHelpers::FromEmu(bevel->GetHeight().ValueOr(0)),
                    bevel->GetPreset().ValueOr(Drawing::BevelPresetValues::Circle).GetValue()};
            }
            if (auto bevel = shape3D->GetFirstChildOfType<Drawing::BevelBottom>())
            {
                value.BottomBevel = PresentationTextBevel{
                    PresentationMeasurementHelpers::FromEmu(bevel->GetWidth().ValueOr(0)),
                    PresentationMeasurementHelpers::FromEmu(bevel->GetHeight().ValueOr(0)),
                    bevel->GetPreset().ValueOr(Drawing::BevelPresetValues::Circle).GetValue()};
            }
            value.ExtrusionColor = PresentationTextEffectHelpers::ReadColor(
                shape3D->GetFirstChildOfType<Drawing::ExtrusionColor>());
            value.ContourColor = PresentationTextEffectHelpers::ReadColor(
                shape3D->GetFirstChildOfType<Drawing::ContourColor>());
            result.ThreeD = value;
        }
    }
    for (const auto& paragraph : body->Elements<Drawing::Paragraph>())
    {
        PresentationTextParagraph value;
        auto properties = paragraph->GetFirstChildOfType<Drawing::ParagraphProperties>();
        if (properties)
        {
            value.Alignment = properties->GetAlignment().ValueOr(Drawing::TextAlignmentTypeValues::Left).GetValue();
            if (properties->GetLeftMargin().IsDefined())
            {
                value.LeftMargin = PresentationMeasurementHelpers::FromEmu(properties->GetLeftMargin().Value());
            }
            if (properties->GetRightMargin().IsDefined())
            {
                value.RightMargin = PresentationMeasurementHelpers::FromEmu(properties->GetRightMargin().Value());
            }
            if (properties->GetIndent().IsDefined())
            {
                value.FirstLineIndent = PresentationMeasurementHelpers::FromEmu(properties->GetIndent().Value());
            }
            if (properties->GetDefaultTabSize().IsDefined())
            {
                value.DefaultTabSize =
                    PresentationMeasurementHelpers::FromEmu(properties->GetDefaultTabSize().Value());
            }
            value.Level = properties->GetLevel().ValueOr(0);
            value.RightToLeft = properties->GetRightToLeft().ValueOr(false);
            value.FontAlignment =
                properties->GetFontAlignment().ValueOr(Drawing::TextFontAlignmentValues::Automatic).GetValue();
            const auto readSpacing = [](const std::shared_ptr<Drawing::TextSpacingType>& spacing)
                -> std::optional<PresentationTextSpacing>
            {
                if (!spacing)
                {
                    return std::nullopt;
                }
                if (auto points = spacing->GetFirstChildOfType<Drawing::SpacingPoints>())
                {
                    return PresentationTextSpacing{
                        PresentationMeasurementHelpers::FromHundredthPoint(points->GetVal().ValueOr(0)),
                        std::nullopt};
                }
                if (auto percent = spacing->GetFirstChildOfType<Drawing::SpacingPercent>())
                {
                    return PresentationTextSpacing{std::nullopt,
                                                   static_cast<Real>(percent->GetVal().ValueOr(0)) / 1000.0};
                }
                return std::nullopt;
            };
            value.LineSpacing = readSpacing(properties->GetFirstChildOfType<Drawing::LineSpacing>());
            value.SpaceBefore = readSpacing(properties->GetFirstChildOfType<Drawing::SpaceBefore>());
            value.SpaceAfter = readSpacing(properties->GetFirstChildOfType<Drawing::SpaceAfter>());
            if (auto bullet = properties->GetFirstChildOfType<Drawing::CharacterBullet>())
            {
                value.Bullet = PresentationTextBullet{bullet->GetChar().ToString(), std::nullopt, 1};
            }
            else if (auto numberedBullet = properties->GetFirstChildOfType<Drawing::AutoNumberedBullet>())
            {
                value.Bullet = PresentationTextBullet{
                    std::nullopt,
                    numberedBullet->GetType().ValueOr(Drawing::TextAutoNumberSchemeValues::ArabicPeriod).GetValue(),
                    numberedBullet->GetStartAt().ValueOr(1)};
            }
            if (auto tabs = properties->GetFirstChildOfType<Drawing::TabStopList>())
            {
                for (const auto& tab : tabs->Elements<Drawing::TabStop>())
                {
                    value.Tabs.push_back(
                        {PresentationMeasurementHelpers::FromEmu(tab->GetPosition().ValueOr(0)),
                         tab->GetAlignment().ValueOr(Drawing::TextTabAlignmentValues::Left).GetValue()});
                }
            }
        }
        for (const auto& run : paragraph->Elements<Drawing::Run>())
        {
            PresentationTextRun runValue;
            if (auto text = run->GetFirstChildOfType<Drawing::Text>())
            {
                runValue.Text = text->GetText();
            }
            auto runProperties = run->GetFirstChildOfType<Drawing::RunProperties>();
            if (runProperties)
            {
                runValue.Language = runProperties->GetLanguage().ToString();
                runValue.Bold = runProperties->GetBold().ValueOr(false);
                runValue.Italic = runProperties->GetItalic().ValueOr(false);
                runValue.Underline =
                    runProperties->GetUnderline().ValueOr(Drawing::TextUnderlineValues::None).GetValue();
                runValue.Strike = runProperties->GetStrike().ValueOr(Drawing::TextStrikeValues::NoStrike).GetValue();
                runValue.Capitalization =
                    runProperties->GetCapital().ValueOr(Drawing::TextCapsValues::None).GetValue();
                if (runProperties->GetFontSize().IsDefined())
                {
                    runValue.FontSize =
                        PresentationMeasurementHelpers::FromHundredthPoint(runProperties->GetFontSize().Value());
                }
                if (runProperties->GetSpacing().IsDefined())
                {
                    runValue.CharacterSpacing =
                        PresentationMeasurementHelpers::FromHundredthPoint(runProperties->GetSpacing().Value());
                }
                if (auto latin = runProperties->GetFirstChildOfType<Drawing::LatinFont>())
                {
                    runValue.Typeface = latin->GetTypeface().ToString();
                }
                if (auto fill = runProperties->GetFirstChildOfType<Drawing::SolidFill>())
                {
                    if (auto rgb = fill->GetFirstChildOfType<Drawing::RgbColorModelHex>())
                    {
                        runValue.FontColor = Color::FromHexString(rgb->GetVal().ToString());
                    }
                }
                if (auto effects = runProperties->GetFirstChildOfType<Drawing::EffectList>())
                {
                    runValue.Glow = PresentationTextEffectHelpers::ReadGlow(effects);
                    runValue.Shadow = PresentationTextEffectHelpers::ReadShadow(effects);
                    runValue.Reflection = PresentationTextEffectHelpers::ReadReflection(effects);
                }
                if (auto hyperlink = runProperties->GetFirstChildOfType<Drawing::HyperlinkOnClick>())
                {
                    runValue.HyperlinkTooltip = hyperlink->GetTooltip().ToString();
                    if (m_slidePart)
                    {
                        for (const auto& relationship : m_slidePart->Relationships())
                        {
                            if (relationship.Id == hyperlink->GetId().ToString() && relationship.IsExternal)
                            {
                                runValue.Hyperlink = relationship.Target;
                                break;
                            }
                        }
                    }
                }
            }
            value.Runs.push_back(std::move(runValue));
        }
        result.Paragraphs.push_back(std::move(value));
    }
    return result;
}

bool PresentationShape::SetTextFrame(const PresentationTextFrame& frame)
{
    auto shape = std::dynamic_pointer_cast<Presentation::Shape>(m_element);
    const auto leftMargin = PresentationMeasurementHelpers::ToInt32Emu(frame.LeftMargin);
    const auto topMargin = PresentationMeasurementHelpers::ToInt32Emu(frame.TopMargin);
    const auto rightMargin = PresentationMeasurementHelpers::ToInt32Emu(frame.RightMargin);
    const auto bottomMargin = PresentationMeasurementHelpers::ToInt32Emu(frame.BottomMargin);
    if (!shape || !leftMargin || !topMargin || !rightMargin || !bottomMargin ||
        !PresentationMeasurementHelpers::IsNonNegative(frame.LeftMargin) ||
        !PresentationMeasurementHelpers::IsNonNegative(frame.TopMargin) ||
        !PresentationMeasurementHelpers::IsNonNegative(frame.RightMargin) ||
        !PresentationMeasurementHelpers::IsNonNegative(frame.BottomMargin))
    {
        return false;
    }
    if (frame.ThreeD && !PresentationTextEffectHelpers::IsValid(*frame.ThreeD))
    {
        return false;
    }
    for (const auto& paragraph : frame.Paragraphs)
    {
        const auto validOptionalEmu = [](const std::optional<MeasuringUnits>& value, bool nonNegative)
        {
            return !value || (PresentationMeasurementHelpers::ToInt32Emu(*value) &&
                              (!nonNegative || PresentationMeasurementHelpers::IsNonNegative(*value)));
        };
        const auto validSpacing = [](const std::optional<PresentationTextSpacing>& spacing)
        {
            if (!spacing)
            {
                return true;
            }
            if (spacing->Points.has_value() == spacing->Percent.has_value())
            {
                return false;
            }
            if (spacing->Points)
            {
                const auto points = PresentationMeasurementHelpers::ToHundredthPoint(*spacing->Points, true);
                return points && *points <= 20116800;
            }
            const auto percent = *spacing->Percent;
            return std::isfinite(percent) && percent >= 0.0 && percent <= 20116.8;
        };
        const auto validHundredthPoint = [](const std::optional<MeasuringUnits>& value, Int32 minimum,
                                            Int32 maximum, bool nonNegative)
        {
            if (!value)
            {
                return true;
            }
            const auto points = PresentationMeasurementHelpers::ToHundredthPoint(*value, nonNegative);
            return points && *points >= minimum && *points <= maximum;
        };
        if (paragraph.Bullet && (paragraph.Bullet->Character.has_value() == paragraph.Bullet->Numbering.has_value()))
        {
            return false;
        }
        if (paragraph.Bullet &&
            ((paragraph.Bullet->Character && paragraph.Bullet->Character->empty()) ||
             (paragraph.Bullet->Numbering &&
              (!Drawing::TextAutoNumberSchemeValues(*paragraph.Bullet->Numbering).IsValid() ||
               paragraph.Bullet->StartAt < 1 || paragraph.Bullet->StartAt > 32767))))
        {
            return false;
        }
        if (paragraph.Level < 0 || paragraph.Level > 8 ||
            !Drawing::TextAlignmentTypeValues(paragraph.Alignment).IsValid() ||
            !Drawing::TextFontAlignmentValues(paragraph.FontAlignment).IsValid() ||
            !validOptionalEmu(paragraph.LeftMargin, false) || !validOptionalEmu(paragraph.RightMargin, false) ||
            !validOptionalEmu(paragraph.FirstLineIndent, false) ||
            !validOptionalEmu(paragraph.DefaultTabSize, true) || !validSpacing(paragraph.LineSpacing) ||
            !validSpacing(paragraph.SpaceBefore) || !validSpacing(paragraph.SpaceAfter))
        {
            return false;
        }
        if (std::any_of(paragraph.Tabs.begin(), paragraph.Tabs.end(), [](const auto& tab)
                        { return !PresentationMeasurementHelpers::ToInt32Emu(tab.Position) ||
                                 !Drawing::TextTabAlignmentValues(tab.Alignment).IsValid(); }))
        {
            return false;
        }
        for (const auto& run : paragraph.Runs)
        {
            const bool validGlow = !run.Glow || PresentationTextEffectHelpers::IsValid(*run.Glow);
            const bool validShadow = !run.Shadow || PresentationTextEffectHelpers::IsValid(*run.Shadow);
            const bool validReflection =
                !run.Reflection || PresentationTextEffectHelpers::IsValid(*run.Reflection);
            if (!Drawing::TextUnderlineValues(run.Underline).IsValid() ||
                !Drawing::TextStrikeValues(run.Strike).IsValid() ||
                !Drawing::TextCapsValues(run.Capitalization).IsValid() ||
                !validHundredthPoint(run.FontSize, 100, 400000, true) ||
                !validHundredthPoint(run.CharacterSpacing, -400000, 400000, false) ||
                (run.FontColor && run.FontColor->IsAuto()) || (run.Hyperlink && run.Hyperlink->empty()) ||
                !validGlow || !validShadow || !validReflection)
            {
                return false;
            }
        }
    }
    auto oldBody = shape->GetFirstChildOfType<Presentation::TextBody>();
    if (oldBody && m_slidePart)
    {
        for (const auto& hyperlink : oldBody->Descendants<Drawing::HyperlinkOnClick>())
        {
            const auto id = hyperlink->GetId().ToString();
            bool external = false;
            for (const auto& relationship : m_slidePart->Relationships())
            {
                if (relationship.Id == id && relationship.IsExternal)
                {
                    external = true;
                    break;
                }
            }
            if (external)
            {
                m_slidePart->RemoveExternalRelationship(id);
            }
        }
    }
    if (oldBody)
    {
        shape->RemoveChild(oldBody);
    }
    auto body = shape->AppendChild<Presentation::TextBody>();
    auto bodyProperties = body ? body->AppendChild<Drawing::BodyProperties>() : nullptr;
    auto listStyle = body ? body->AppendChild<Drawing::ListStyle>() : nullptr;
    if (!body || !bodyProperties || !listStyle)
    {
        return false;
    }
    bodyProperties->SetLeftInset(Int32Value(*leftMargin));
    bodyProperties->SetTopInset(Int32Value(*topMargin));
    bodyProperties->SetRightInset(Int32Value(*rightMargin));
    bodyProperties->SetBottomInset(Int32Value(*bottomMargin));
    bodyProperties->SetVertical(EnumValue<Drawing::TextVerticalValues>(Drawing::TextVerticalValues(frame.Vertical)));
    bodyProperties->SetAnchor(
        EnumValue<Drawing::TextAnchoringTypeValues>(Drawing::TextAnchoringTypeValues(frame.Anchor)));
    if (frame.ThreeD)
    {
        auto scene = bodyProperties->AppendChild<Drawing::Scene3DType>();
        auto shape3D = bodyProperties->AppendChild<Drawing::Shape3DType>();
        auto camera = scene ? scene->AppendChild<Drawing::Camera>() : nullptr;
        auto light = scene ? scene->AppendChild<Drawing::LightRig>() : nullptr;
        if (!scene || !shape3D || !camera || !light)
        {
            return false;
        }
        camera->SetPreset(EnumValue<Drawing::PresetCameraValues>(Drawing::PresetCameraValues(frame.ThreeD->Camera)));
        light->SetRig(EnumValue<Drawing::LightRigValues>(Drawing::LightRigValues(frame.ThreeD->LightRig)));
        light->SetDirection(EnumValue<Drawing::LightRigDirectionValues>(
            Drawing::LightRigDirectionValues(frame.ThreeD->LightDirection)));
        shape3D->SetZ(Int64Value(*PresentationTextEffectHelpers::Length(frame.ThreeD->Depth)));
        shape3D->SetExtrusionHeight(
            Int64Value(*PresentationTextEffectHelpers::Length(frame.ThreeD->ExtrusionHeight)));
        shape3D->SetContourWidth(Int64Value(*PresentationTextEffectHelpers::Length(frame.ThreeD->ContourWidth)));
        shape3D->SetPresetMaterial(EnumValue<Drawing::PresetMaterialTypeValues>(
            Drawing::PresetMaterialTypeValues(frame.ThreeD->Material)));
        auto appendBevel = [shape3D](const std::optional<PresentationTextBevel>& value, bool top)
        {
            std::shared_ptr<Drawing::BevelType> bevel = top
                                                            ? std::static_pointer_cast<Drawing::BevelType>(
                                                                  shape3D->AppendChild<Drawing::BevelTop>())
                                                            : std::static_pointer_cast<Drawing::BevelType>(
                                                                  shape3D->AppendChild<Drawing::BevelBottom>());
            if (!value || !bevel)
            {
                return !value;
            }
            bevel->SetWidth(Int64Value(*PresentationTextEffectHelpers::Length(value->Width)));
            bevel->SetHeight(Int64Value(*PresentationTextEffectHelpers::Length(value->Height)));
            bevel->SetPreset(EnumValue<Drawing::BevelPresetValues>(Drawing::BevelPresetValues(value->Preset)));
            return true;
        };
        if (!appendBevel(frame.ThreeD->TopBevel, true) || !appendBevel(frame.ThreeD->BottomBevel, false))
        {
            return false;
        }
        if (frame.ThreeD->ExtrusionColor)
        {
            auto color = shape3D->AppendChild<Drawing::ExtrusionColor>();
            if (!PresentationTextEffectHelpers::AppendColor(color, *frame.ThreeD->ExtrusionColor))
            {
                return false;
            }
        }
        if (frame.ThreeD->ContourColor)
        {
            auto color = shape3D->AppendChild<Drawing::ContourColor>();
            if (!PresentationTextEffectHelpers::AppendColor(color, *frame.ThreeD->ContourColor))
            {
                return false;
            }
        }
    }
    for (const auto& paragraphValue : frame.Paragraphs)
    {
        auto paragraph = body->AppendChild<Drawing::Paragraph>();
        auto properties = paragraph ? paragraph->AppendChild<Drawing::ParagraphProperties>() : nullptr;
        if (!paragraph || !properties)
        {
            return false;
        }
        properties->SetAlignment(
            EnumValue<Drawing::TextAlignmentTypeValues>(Drawing::TextAlignmentTypeValues(paragraphValue.Alignment)));
        properties->SetLevel(Int32Value(paragraphValue.Level));
        properties->SetRightToLeft(BooleanValue(paragraphValue.RightToLeft));
        properties->SetFontAlignment(EnumValue<Drawing::TextFontAlignmentValues>(
            Drawing::TextFontAlignmentValues(paragraphValue.FontAlignment)));
        if (paragraphValue.LeftMargin)
        {
            properties->SetLeftMargin(
                Int32Value(*PresentationMeasurementHelpers::ToInt32Emu(*paragraphValue.LeftMargin)));
        }
        if (paragraphValue.RightMargin)
        {
            properties->SetRightMargin(
                Int32Value(*PresentationMeasurementHelpers::ToInt32Emu(*paragraphValue.RightMargin)));
        }
        if (paragraphValue.FirstLineIndent)
        {
            properties->SetIndent(
                Int32Value(*PresentationMeasurementHelpers::ToInt32Emu(*paragraphValue.FirstLineIndent)));
        }
        if (paragraphValue.DefaultTabSize)
        {
            properties->SetDefaultTabSize(
                Int32Value(*PresentationMeasurementHelpers::ToInt32Emu(*paragraphValue.DefaultTabSize)));
        }
        const auto appendSpacing = [](const std::shared_ptr<Drawing::TextSpacingType>& target,
                                      const PresentationTextSpacing& spacing)
        {
            if (spacing.Points)
            {
                target->AppendChild<Drawing::SpacingPoints>()->SetVal(
                    Int32Value(*PresentationMeasurementHelpers::ToHundredthPoint(*spacing.Points, true)));
            }
            else
            {
                target->AppendChild<Drawing::SpacingPercent>()->SetVal(
                    Int32Value(static_cast<Int32>(std::llround(*spacing.Percent * 1000.0))));
            }
        };
        if (paragraphValue.LineSpacing)
        {
            appendSpacing(properties->AppendChild<Drawing::LineSpacing>(), *paragraphValue.LineSpacing);
        }
        if (paragraphValue.SpaceBefore)
        {
            appendSpacing(properties->AppendChild<Drawing::SpaceBefore>(), *paragraphValue.SpaceBefore);
        }
        if (paragraphValue.SpaceAfter)
        {
            appendSpacing(properties->AppendChild<Drawing::SpaceAfter>(), *paragraphValue.SpaceAfter);
        }
        if (!paragraphValue.Bullet)
        {
            properties->AppendChild<Drawing::NoBullet>();
        }
        else if (paragraphValue.Bullet->Character)
        {
            auto bullet = properties->AppendChild<Drawing::CharacterBullet>();
            if (!bullet)
            {
                return false;
            }
            bullet->SetChar(StringValue(*paragraphValue.Bullet->Character));
        }
        else if (paragraphValue.Bullet->Numbering)
        {
            auto bullet = properties->AppendChild<Drawing::AutoNumberedBullet>();
            if (!bullet)
            {
                return false;
            }
            bullet->SetType(EnumValue<Drawing::TextAutoNumberSchemeValues>(
                Drawing::TextAutoNumberSchemeValues(*paragraphValue.Bullet->Numbering)));
            bullet->SetStartAt(Int32Value(paragraphValue.Bullet->StartAt));
        }
        if (!paragraphValue.Tabs.empty())
        {
            auto tabs = properties->AppendChild<Drawing::TabStopList>();
            if (!tabs)
            {
                return false;
            }
            for (const auto& tabValue : paragraphValue.Tabs)
            {
                auto tab = tabs->AppendChild<Drawing::TabStop>();
                if (!tab)
                {
                    return false;
                }
                tab->SetPosition(Int32Value(*PresentationMeasurementHelpers::ToInt32Emu(tabValue.Position)));
                tab->SetAlignment(
                    EnumValue<Drawing::TextTabAlignmentValues>(Drawing::TextTabAlignmentValues(tabValue.Alignment)));
            }
        }
        for (const auto& runValue : paragraphValue.Runs)
        {
            auto run = paragraph->AppendChild<Drawing::Run>();
            auto runProperties = run ? run->AppendChild<Drawing::RunProperties>() : nullptr;
            auto text = run ? run->AppendChild<Drawing::Text>() : nullptr;
            if (!run || !runProperties || !text)
            {
                return false;
            }
            if (!runValue.Language.empty())
            {
                runProperties->SetLanguage(StringValue(runValue.Language));
            }
            runProperties->SetBold(BooleanValue(runValue.Bold));
            runProperties->SetItalic(BooleanValue(runValue.Italic));
            runProperties->SetUnderline(
                EnumValue<Drawing::TextUnderlineValues>(Drawing::TextUnderlineValues(runValue.Underline)));
            runProperties->SetStrike(EnumValue<Drawing::TextStrikeValues>(Drawing::TextStrikeValues(runValue.Strike)));
            runProperties->SetCapital(
                EnumValue<Drawing::TextCapsValues>(Drawing::TextCapsValues(runValue.Capitalization)));
            if (runValue.FontSize)
            {
                runProperties->SetFontSize(
                    Int32Value(*PresentationMeasurementHelpers::ToHundredthPoint(*runValue.FontSize, true)));
            }
            if (runValue.CharacterSpacing)
            {
                runProperties->SetSpacing(
                    Int32Value(*PresentationMeasurementHelpers::ToHundredthPoint(*runValue.CharacterSpacing)));
            }
            if (!runValue.Typeface.empty())
            {
                runProperties->AppendChild<Drawing::LatinFont>()->SetTypeface(StringValue(runValue.Typeface));
            }
            if (runValue.FontColor)
            {
                auto fill = runProperties->AppendChild<Drawing::SolidFill>();
                auto color = fill ? fill->AppendChild<Drawing::RgbColorModelHex>() : nullptr;
                if (!color)
                {
                    return false;
                }
                color->SetVal(
                    OpenXmlSimpleValueConvertor::GetHexBinaryValueFromString(runValue.FontColor->ToHexString()));
            }
            if (runValue.Glow || runValue.Shadow || runValue.Reflection)
            {
                auto effects = runProperties->AppendChild<Drawing::EffectList>();
                if (!effects)
                {
                    return false;
                }
                if (runValue.Glow && !PresentationTextEffectHelpers::WriteGlow(effects, *runValue.Glow))
                {
                    return false;
                }
                if (runValue.Shadow && !PresentationTextEffectHelpers::WriteShadow(effects, *runValue.Shadow))
                {
                    return false;
                }
                if (runValue.Reflection &&
                    !PresentationTextEffectHelpers::WriteReflection(effects, *runValue.Reflection))
                {
                    return false;
                }
            }
            if (runValue.Hyperlink)
            {
                if (!m_slidePart)
                {
                    return false;
                }
                const auto id =
                    m_slidePart->AddExternalRelationship("http://schemas.openxmlformats.org/officeDocument/2006/"
                                                         "relationships/hyperlink",
                                                         *runValue.Hyperlink);
                auto hyperlink = runProperties->AppendChild<Drawing::HyperlinkOnClick>();
                if (id.empty() || !hyperlink)
                {
                    return false;
                }
                hyperlink->SetId(StringValue(id));
                hyperlink->SetTooltip(StringValue(runValue.HyperlinkTooltip));
            }
            text->SetText(runValue.Text);
        }
    }
    if (frame.Paragraphs.empty())
    {
        body->AppendChild<Drawing::Paragraph>();
    }
    return true;
}

std::optional<PresentationPictureData> PresentationShape::GetPicture() const
{
    auto picture = std::dynamic_pointer_cast<Presentation::Picture>(m_element);
    if (!picture)
    {
        return std::nullopt;
    }
    PresentationPictureData result;
    auto nonVisual = picture->GetFirstChildOfType<Presentation::NonVisualPictureProperties>();
    auto properties = nonVisual ? nonVisual->GetFirstChildOfType<Presentation::NonVisualDrawingProperties>() : nullptr;
    if (properties)
    {
        result.Name = properties->GetName().ToString();
        result.AltText = properties->GetDescription().ToString();
        result.Title = properties->GetTitle().ToString();
        if (auto hyperlink = properties->GetFirstChildOfType<Drawing::HyperlinkOnClick>())
        {
            result.HyperlinkTooltip = hyperlink->GetTooltip().ToString();
            if (m_slidePart)
            {
                for (const auto& relationship : m_slidePart->Relationships())
                {
                    if (relationship.Id == hyperlink->GetId().ToString() && relationship.IsExternal)
                    {
                        result.Hyperlink = relationship.Target;
                        break;
                    }
                }
            }
        }
    }
    auto fill = picture->GetFirstChildOfType<Presentation::BlipFill>();
    auto blip = fill ? fill->GetFirstChildOfType<Drawing::Blip>() : nullptr;
    if (auto crop = fill ? fill->GetFirstChildOfType<Drawing::SourceRectangle>() : nullptr)
    {
        result.Crop = {crop->GetLeft().ValueOr(0), crop->GetTop().ValueOr(0), crop->GetRight().ValueOr(0),
                       crop->GetBottom().ValueOr(0)};
    }
    if (blip && m_slidePart)
    {
        const auto embeddedId = blip->GetEmbed().ToString();
        const auto linkedId = blip->GetLink().ToString();
        if (!embeddedId.empty())
        {
            for (const auto& image : m_slidePart->GetImageParts())
            {
                if (image && image->RelationshipId() == embeddedId)
                {
                    result.Embedded =
                        PresentationEmbeddedPicture{image->GetBinaryData(), std::string(image->ContentType())};
                    break;
                }
            }
        }
        if (!linkedId.empty())
        {
            for (const auto& relationship : m_slidePart->Relationships())
            {
                if (relationship.Id == linkedId && relationship.IsExternal && relationship.Type == ImageRelationship)
                {
                    result.LinkedUri = relationship.Target;
                    break;
                }
            }
        }
    }
    if (auto transform = GetTransform())
    {
        result.Transform = *transform;
    }
    return result;
}

bool PresentationShape::SetPicture(const PresentationPictureData& value)
{
    auto picture = std::dynamic_pointer_cast<Presentation::Picture>(m_element);
    const auto validCrop = [](Int32 crop)
    { return crop >= 0 && crop <= 100000; };
    if (!picture || !m_slidePart || value.Embedded.has_value() == value.LinkedUri.has_value() ||
        (value.Embedded && (value.Embedded->Data.empty() || value.Embedded->ContentType.empty())) ||
        (value.LinkedUri && value.LinkedUri->empty()) ||
        !PresentationMeasurementHelpers::IsNonNegative(value.Transform.Size.Width) ||
        !PresentationMeasurementHelpers::IsNonNegative(value.Transform.Size.Height) ||
        value.Transform.GroupChildPosition || value.Transform.GroupChildSize ||
        !validCrop(value.Crop.Left) || !validCrop(value.Crop.Top) || !validCrop(value.Crop.Right) ||
        !validCrop(value.Crop.Bottom))
    {
        return false;
    }

    auto oldFill = picture->GetFirstChildOfType<Presentation::BlipFill>();
    if (oldFill)
    {
        for (const auto& blip : oldFill->Descendants<Drawing::Blip>())
        {
            const auto embeddedId = blip->GetEmbed().ToString();
            const auto linkedId = blip->GetLink().ToString();
            if (!embeddedId.empty())
            {
                for (const auto& image : m_slidePart->GetImageParts())
                {
                    if (image && image->RelationshipId() == embeddedId)
                    {
                        m_slidePart->RemoveImagePart(image);
                        break;
                    }
                }
            }
            if (!linkedId.empty())
            {
                m_slidePart->RemoveExternalRelationship(linkedId);
            }
        }
    }
    auto nonVisual = picture->GetFirstChildOfType<Presentation::NonVisualPictureProperties>();
    auto properties = nonVisual ? nonVisual->GetFirstChildOfType<Presentation::NonVisualDrawingProperties>() : nullptr;
    if (!properties)
    {
        return false;
    }
    if (auto old = properties->GetFirstChildOfType<Drawing::HyperlinkOnClick>())
    {
        m_slidePart->RemoveExternalRelationship(old->GetId().ToString());
        properties->RemoveChild(old);
    }
    if (oldFill)
    {
        picture->RemoveChild(oldFill);
    }
    auto shapeProperties = picture->GetFirstChildOfType<Presentation::ShapeProperties>();
    auto fill = picture->InsertChild<Presentation::BlipFill>(shapeProperties);
    auto blip = fill ? fill->AppendChild<Drawing::Blip>() : nullptr;
    auto crop = fill ? fill->AppendChild<Drawing::SourceRectangle>() : nullptr;
    auto stretch = fill ? fill->AppendChild<Drawing::Stretch>() : nullptr;
    auto rectangle = stretch ? stretch->AppendChild<Drawing::FillRectangle>() : nullptr;
    if (!fill || !blip || !crop || !stretch || !rectangle)
    {
        return false;
    }
    crop->SetLeft(Int32Value(value.Crop.Left));
    crop->SetTop(Int32Value(value.Crop.Top));
    crop->SetRight(Int32Value(value.Crop.Right));
    crop->SetBottom(Int32Value(value.Crop.Bottom));
    if (value.Embedded)
    {
        // Attaching the part after its content type is known lets the package
        // name the file after the image format, not the `.bin` placeholder.
        auto image = std::make_shared<Packaging::ImagePart>();
        image->SetContentType(value.Embedded->ContentType);
        if (!m_slidePart->AddImagePart(image))
        {
            return false;
        }
        image->SetBinaryData(value.Embedded->Data);
        blip->SetEmbed(StringValue(image->RelationshipId()));
    }
    else
    {
        const auto id = m_slidePart->AddExternalRelationship(ImageRelationship, *value.LinkedUri);
        if (id.empty())
        {
            return false;
        }
        blip->SetLink(StringValue(id));
    }
    properties->SetName(StringValue(value.Name));
    properties->SetDescription(StringValue(value.AltText));
    properties->SetTitle(StringValue(value.Title));
    if (value.Hyperlink)
    {
        const auto id = m_slidePart->AddExternalRelationship(HyperlinkRelationship, *value.Hyperlink);
        auto hyperlink = properties->AppendChild<Drawing::HyperlinkOnClick>();
        if (id.empty() || !hyperlink)
        {
            return false;
        }
        hyperlink->SetId(StringValue(id));
        hyperlink->SetTooltip(StringValue(value.HyperlinkTooltip));
    }
    return SetTransform(value.Transform);
}

bool PresentationShape::ReplacePictureFromData(std::vector<Byte> data)
{
    const auto format = ExyokiOffice::DetectImageFormat(data);
    auto picture = GetPicture();
    if (!format || !picture)
    {
        return false;
    }
    picture->Embedded = PresentationEmbeddedPicture{std::move(data), format->ContentType};
    picture->LinkedUri.reset();
    return SetPicture(*picture);
}

bool PresentationShape::ReplacePictureFromFile(const std::filesystem::path& path)
{
    auto data = Packaging::ReadFileFully(path);
    return !data.empty() && ReplacePictureFromData(std::move(data));
}

Security::ExternalResourceStatus PresentationShape::EmbedLinkedPicture(const ICancellationToken* cancellationToken)
{
    auto picture = GetPicture();
    if (!picture || !picture->LinkedUri || picture->LinkedUri->empty() || !m_slidePart)
    {
        return Security::ExternalResourceStatus::NotFound;
    }

    auto* package = m_slidePart->Package();
    if (!package)
    {
        return Security::ExternalResourceStatus::Failed;
    }

    auto response = Security::ResolveExternalResource(
        *package,
        PresentationLinkHelpers::Reference(
            m_slidePart, ImageRelationship, *picture->LinkedUri, Security::ExternalResourceKind::LinkedImage),
        cancellationToken);
    if (!response)
    {
        return response.Status;
    }

    // The content type reported by the source is a hint from an untrusted
    // party, so the format is taken from the bytes themselves.
    const auto format = ExyokiOffice::DetectImageFormat(response.Data);
    if (!format)
    {
        return Security::ExternalResourceStatus::Unsupported;
    }

    picture->Embedded = PresentationEmbeddedPicture{std::move(response.Data), format->ContentType};
    picture->LinkedUri.reset();
    return SetPicture(*picture) ? Security::ExternalResourceStatus::Ok : Security::ExternalResourceStatus::Failed;
}

std::optional<PresentationMediaData> PresentationShape::GetMedia() const
{
    auto marker = PresentationMediaHelpers::Marker(m_element);
    const auto id = PresentationMediaHelpers::RelationshipId(marker);
    const auto relationship = PresentationMediaHelpers::Relationship(m_slidePart, id);
    if (!marker || !relationship)
    {
        return std::nullopt;
    }

    PresentationMediaData result;
    result.Kind = std::dynamic_pointer_cast<Drawing::AudioFromFile>(marker)
                      ? PresentationMediaKind::Audio
                      : PresentationMediaKind::Video;
    if (relationship->IsExternal)
    {
        result.LinkedUri = relationship->Target;
    }
    else if (auto part = PresentationMediaHelpers::Target(m_slidePart, id))
    {
        result.Embedded = PresentationEmbeddedMedia{part->GetBinaryData(), std::string(part->ContentType())};
    }
    else
    {
        return std::nullopt;
    }

    if (auto picture = GetPicture())
    {
        if (picture->Embedded)
        {
            result.PosterFrame = *picture->Embedded;
        }
        result.Name = picture->Name;
        result.AltText = picture->AltText;
        result.Transform = picture->Transform;
    }
    const auto shapeId = PresentationMediaHelpers::ShapeId(m_element);
    if (auto node = PresentationMediaHelpers::TimingNode(m_slidePart, shapeId))
    {
        result.Playback.Volume = node->GetVolume().ValueOr(100000);
        result.Playback.Muted = node->GetMute().ValueOr(false);
        result.Playback.ShowWhenStopped = node->GetShowWhenStopped().ValueOr(true);
        const auto timeNodes = node->Descendants<Presentation::CommonTimeNode>();
        result.Playback.Loop = !timeNodes.empty() && timeNodes.front()->GetRepeatCount().ToString() == "indefinite";
        if (auto video = std::dynamic_pointer_cast<Presentation::Video>(node->Parent()))
        {
            result.Playback.FullScreen = video->GetFullScreen().ValueOr(false);
        }
    }
    return result;
}

bool PresentationShape::SetMedia(const PresentationMediaData& value)
{
    auto marker = PresentationMediaHelpers::Marker(m_element);
    if (!marker || !m_slidePart || !PresentationMediaHelpers::Valid(value))
    {
        return false;
    }
    const auto oldId = PresentationMediaHelpers::RelationshipId(marker);
    const auto oldRelationship = PresentationMediaHelpers::Relationship(m_slidePart, oldId);
    auto oldPart = oldRelationship && !oldRelationship->IsExternal
                       ? PresentationMediaHelpers::Target(m_slidePart, oldId)
                       : nullptr;

    if (value.PosterFrame)
    {
        PresentationPictureData picture;
        picture.Embedded = *value.PosterFrame;
        picture.Name = value.Name;
        picture.AltText = value.AltText;
        picture.Transform = value.Transform;
        if (!SetPicture(picture))
        {
            return false;
        }
    }
    else
    {
        if (auto picture = std::dynamic_pointer_cast<Presentation::Picture>(m_element))
        {
            if (auto fill = picture->GetFirstChildOfType<Presentation::BlipFill>())
            {
                if (auto blip = fill->GetFirstChildOfType<Drawing::Blip>())
                {
                    const auto posterId = blip->GetEmbed().ToString();
                    if (auto poster = PresentationMediaHelpers::Target(m_slidePart, posterId))
                    {
                        m_slidePart->RemovePartReference(poster);
                    }
                }
                picture->RemoveChild(fill);
            }
        }
        const auto properties = m_element->Descendants<Presentation::NonVisualDrawingProperties>();
        if (!properties.empty())
        {
            properties.front()->SetName(StringValue(value.Name));
            properties.front()->SetDescription(StringValue(value.AltText));
        }
        if (!SetTransform(value.Transform))
        {
            return false;
        }
    }

    std::string newId;
    const auto relationshipType = value.Kind == PresentationMediaKind::Audio
                                      ? PresentationMediaHelpers::AudioRelationship
                                      : PresentationMediaHelpers::VideoRelationship;
    if (value.Embedded)
    {
        const auto& descriptor = PresentationMediaHelpers::Descriptor(value.Kind);
        auto part = std::make_shared<OpenXmlPackagePart>(descriptor);
        if (!m_slidePart->AttachCustomPart(part, descriptor, true))
        {
            return false;
        }
        newId = part->RelationshipId();
        part->SetContentType(value.Embedded->ContentType);
        part->SetBinaryData(value.Embedded->Data);
    }
    else
    {
        newId = m_slidePart->AddExternalRelationship(relationshipType, *value.LinkedUri);
    }
    if (newId.empty())
    {
        return false;
    }

    auto markerParent = marker->Parent();
    if (!markerParent)
    {
        return false;
    }
    markerParent->RemoveChild(marker);
    if (value.Kind == PresentationMediaKind::Audio)
    {
        markerParent->AppendChild<Drawing::AudioFromFile>()->SetLink(StringValue(newId));
    }
    else
    {
        markerParent->AppendChild<Drawing::VideoFromFile>()->SetLink(StringValue(newId));
    }

    if (oldRelationship)
    {
        if (oldRelationship->IsExternal)
        {
            m_slidePart->RemoveExternalRelationship(oldId);
        }
        else if (oldPart)
        {
            m_slidePart->RemovePartReference(oldPart);
        }
    }

    const auto shapeId = PresentationMediaHelpers::ShapeId(m_element);
    if (auto oldNode = PresentationMediaHelpers::TimingNode(m_slidePart, shapeId))
    {
        if (auto host = oldNode->Parent(); host && host->Parent())
        {
            host->Parent()->RemoveChild(host);
        }
    }
    auto slide = m_slidePart->GetSlide();
    auto timing = slide ? slide->GetFirstChildOfType<Presentation::Timing>() : nullptr;
    if (!timing && slide)
    {
        timing = slide->AppendChild<Presentation::Timing>();
    }
    auto list = timing ? timing->GetFirstChildOfType<Presentation::TimeNodeList>() : nullptr;
    if (!list && timing)
    {
        list = timing->AppendChild<Presentation::TimeNodeList>();
    }
    std::shared_ptr<OpenXMLElement> host = value.Kind == PresentationMediaKind::Audio
                                               ? std::static_pointer_cast<OpenXMLElement>(list->AppendChild<Presentation::Audio>())
                                               : std::static_pointer_cast<OpenXMLElement>(list->AppendChild<Presentation::Video>());
    if (auto video = std::dynamic_pointer_cast<Presentation::Video>(host))
    {
        video->SetFullScreen(BooleanValue(value.Playback.FullScreen));
    }
    auto node = host ? host->AppendChild<Presentation::CommonMediaNode>() : nullptr;
    auto common = node ? node->AppendChild<Presentation::CommonTimeNode>() : nullptr;
    auto target = node ? node->AppendChild<Presentation::TargetElement>() : nullptr;
    auto shapeTarget = target ? target->AppendChild<Presentation::ShapeTarget>() : nullptr;
    if (!node || !common || !target || !shapeTarget)
    {
        return false;
    }
    node->SetVolume(Int32Value(value.Playback.Volume));
    node->SetMute(BooleanValue(value.Playback.Muted));
    node->SetShowWhenStopped(BooleanValue(value.Playback.ShowWhenStopped));
    common->SetId(UInt32Value(shapeId));
    common->SetDuration(StringValue("indefinite"));
    if (value.Playback.Loop)
    {
        common->SetRepeatCount(StringValue("indefinite"));
    }
    shapeTarget->SetShapeId(StringValue(std::to_string(shapeId)));
    return true;
}

bool PresentationShape::ReplaceMediaFromFile(const std::filesystem::path& path, std::string contentType)
{
    auto media = GetMedia();
    auto data = Packaging::ReadFileFully(path);
    if (!media || data.empty() || contentType.empty())
    {
        return false;
    }
    media->Embedded = PresentationEmbeddedMedia{std::move(data), std::move(contentType)};
    media->LinkedUri.reset();
    return SetMedia(*media);
}

Security::ExternalResourceStatus PresentationShape::EmbedLinkedMedia(std::string contentType,
                                                                     const ICancellationToken* cancellationToken)
{
    auto media = GetMedia();
    if (!media || !media->LinkedUri || media->LinkedUri->empty() || !m_slidePart)
    {
        return Security::ExternalResourceStatus::NotFound;
    }

    auto* package = m_slidePart->Package();
    if (!package)
    {
        return Security::ExternalResourceStatus::Failed;
    }

    const auto relationshipType = media->Kind == PresentationMediaKind::Audio
                                      ? PresentationMediaHelpers::AudioRelationship
                                      : PresentationMediaHelpers::VideoRelationship;
    auto response = Security::ResolveExternalResource(
        *package,
        PresentationLinkHelpers::Reference(
            m_slidePart, relationshipType, *media->LinkedUri, Security::ExternalResourceKind::LinkedMedia),
        cancellationToken);
    if (!response)
    {
        return response.Status;
    }

    // Media bytes stay opaque, so the type has to be stated by the caller or by
    // the source; there is nothing in the payload the library reads.
    if (contentType.empty())
    {
        contentType = response.ContentType;
    }
    if (contentType.empty())
    {
        return Security::ExternalResourceStatus::Unsupported;
    }

    media->Embedded = PresentationEmbeddedMedia{std::move(response.Data), std::move(contentType)};
    media->LinkedUri.reset();
    return SetMedia(*media) ? Security::ExternalResourceStatus::Ok : Security::ExternalResourceStatus::Failed;
}

std::optional<PresentationTableData> PresentationShape::GetTable() const
{
    auto table = PresentationTableHelpers::Find(m_element);
    if (!table)
    {
        return std::nullopt;
    }
    PresentationTableData result;
    if (auto properties = table->GetFirstChildOfType<Drawing::TableProperties>())
    {
        result.Style.FirstRow = properties->GetFirstRow().ValueOr(false);
        result.Style.FirstColumn = properties->GetFirstColumn().ValueOr(false);
        result.Style.LastRow = properties->GetLastRow().ValueOr(false);
        result.Style.LastColumn = properties->GetLastColumn().ValueOr(false);
        result.Style.BandedRows = properties->GetBandRow().ValueOr(false);
        result.Style.BandedColumns = properties->GetBandColumn().ValueOr(false);
        result.Style.RightToLeft = properties->GetRightToLeft().ValueOr(false);
        if (auto style = properties->GetFirstChildOfType<Drawing::TableStyleId>())
        {
            result.Style.Id = std::string(style->GetText());
        }
    }
    if (auto grid = table->GetFirstChildOfType<Drawing::TableGrid>())
    {
        for (const auto& column : grid->Elements<Drawing::GridColumn>())
        {
            result.ColumnWidths.push_back(PresentationMeasurementHelpers::FromEmu(column->GetWidth().ValueOr(0)));
        }
    }
    Size rowIndex = 0;
    for (const auto& row : table->Elements<Drawing::TableRow>())
    {
        PresentationTableRow rowValue;
        rowValue.Height = PresentationMeasurementHelpers::FromEmu(row->GetHeight().ValueOr(0));
        Size columnIndex = 0;
        for (const auto& cell : row->Elements<Drawing::TableCell>())
        {
            rowValue.Cells.push_back({PresentationTableHelpers::CellText(cell)});
            const auto rowSpan = static_cast<Size>(std::max(1, cell->GetRowSpan().ValueOr(1)));
            const auto columnSpan = static_cast<Size>(std::max(1, cell->GetGridSpan().ValueOr(1)));
            if (rowSpan > 1 || columnSpan > 1)
            {
                result.Merges.push_back({rowIndex, columnIndex, rowSpan, columnSpan});
            }
            ++columnIndex;
        }
        result.Rows.push_back(std::move(rowValue));
        ++rowIndex;
    }
    if (auto transform = GetTransform())
    {
        result.Transform = *transform;
    }
    return result;
}

bool PresentationShape::SetTable(const PresentationTableData& value)
{
    auto frame = std::dynamic_pointer_cast<Presentation::GraphicFrame>(m_element);
    if (!frame || !PresentationTableHelpers::Find(frame) || !PresentationTableHelpers::IsValid(value))
    {
        return false;
    }
    auto oldGraphic = frame->GetFirstChildOfType<Drawing::Graphic>();
    if (oldGraphic)
    {
        frame->RemoveChild(oldGraphic);
    }
    auto graphic = frame->AppendChild<Drawing::Graphic>();
    auto data = graphic ? graphic->AppendChild<Drawing::GraphicData>() : nullptr;
    auto table = data ? data->AppendChild<Drawing::Table>() : nullptr;
    auto properties = table ? table->AppendChild<Drawing::TableProperties>() : nullptr;
    auto grid = table ? table->AppendChild<Drawing::TableGrid>() : nullptr;
    if (!graphic || !data || !table || !properties || !grid)
    {
        return false;
    }
    data->SetUri(StringValue(std::string(TableGraphicDataUri)));
    properties->SetFirstRow(BooleanValue(value.Style.FirstRow));
    properties->SetFirstColumn(BooleanValue(value.Style.FirstColumn));
    properties->SetLastRow(BooleanValue(value.Style.LastRow));
    properties->SetLastColumn(BooleanValue(value.Style.LastColumn));
    properties->SetBandRow(BooleanValue(value.Style.BandedRows));
    properties->SetBandColumn(BooleanValue(value.Style.BandedColumns));
    properties->SetRightToLeft(BooleanValue(value.Style.RightToLeft));
    if (!value.Style.Id.empty())
    {
        auto style = properties->AppendChild<Drawing::TableStyleId>();
        if (!style)
        {
            return false;
        }
        style->SetText(value.Style.Id);
    }
    for (const auto& width : value.ColumnWidths)
    {
        auto column = grid->AppendChild<Drawing::GridColumn>();
        if (!column)
        {
            return false;
        }
        column->SetWidth(Int64Value(*PresentationMeasurementHelpers::ToInt64Emu(width)));
    }
    for (Size rowIndex = 0; rowIndex < value.Rows.size(); ++rowIndex)
    {
        const auto& rowValue = value.Rows[rowIndex];
        auto row = table->AppendChild<Drawing::TableRow>();
        if (!row)
        {
            return false;
        }
        row->SetHeight(Int64Value(*PresentationMeasurementHelpers::ToInt64Emu(rowValue.Height)));
        for (Size columnIndex = 0; columnIndex < rowValue.Cells.size(); ++columnIndex)
        {
            auto cell = row->AppendChild<Drawing::TableCell>();
            if (!cell || !PresentationTableHelpers::SetCellText(cell, rowValue.Cells[columnIndex].Text))
            {
                return false;
            }
            for (const auto& merge : value.Merges)
            {
                const bool covered = rowIndex >= merge.Row && rowIndex < merge.Row + merge.RowSpan &&
                                     columnIndex >= merge.Column && columnIndex < merge.Column + merge.ColumnSpan;
                if (!covered)
                {
                    continue;
                }
                if (rowIndex == merge.Row && columnIndex == merge.Column)
                {
                    if (merge.RowSpan > 1)
                    {
                        cell->SetRowSpan(Int32Value(static_cast<Int32>(merge.RowSpan)));
                    }
                    if (merge.ColumnSpan > 1)
                    {
                        cell->SetGridSpan(Int32Value(static_cast<Int32>(merge.ColumnSpan)));
                    }
                }
                else
                {
                    if (columnIndex > merge.Column)
                    {
                        cell->SetHorizontalMerge(BooleanValue(true));
                    }
                    if (rowIndex > merge.Row)
                    {
                        cell->SetVerticalMerge(BooleanValue(true));
                    }
                }
                break;
            }
            auto cellProperties = cell->AppendChild<Drawing::TableCellProperties>();
            if (!cellProperties)
            {
                return false;
            }
        }
    }
    return SetTransform(value.Transform);
}

std::optional<PresentationEmbeddedObject> PresentationShape::GetEmbeddedObject() const
{
    const auto references = PresentationEmbeddedObjectHelpers::References(m_element);
    if (!references || !m_slidePart)
    {
        return std::nullopt;
    }
    PresentationEmbeddedObject result;
    result.Kind = references->first;
    for (const auto& id : references->second)
    {
        const auto relationship = PresentationEmbeddedObjectHelpers::Relationship(m_slidePart, id);
        const auto part = PresentationEmbeddedObjectHelpers::Target(m_slidePart, id);
        if (!relationship || relationship->IsExternal || !part)
        {
            continue;
        }
        PresentationObjectPayload payload;
        payload.RelationshipId = id;
        payload.RelationshipType = relationship->Type;
        payload.ContentType = std::string(part->ContentType());
        if (part->IsXmlPart())
        {
            payload.Xml = part->GetXmlString();
        }
        else if (part->IsBinaryPart())
        {
            payload.BinaryData = part->GetBinaryData();
        }
        result.Payloads.push_back(std::move(payload));
    }
    return result;
}

bool PresentationShape::ReplaceEmbeddedObjectPayload(const PresentationObjectPayload& payload)
{
    const auto references = PresentationEmbeddedObjectHelpers::References(m_element);
    if (!references || !m_slidePart || payload.RelationshipId.empty())
    {
        return false;
    }
    if (std::find(references->second.begin(), references->second.end(), payload.RelationshipId) ==
        references->second.end())
    {
        return false;
    }
    const auto relationship = PresentationEmbeddedObjectHelpers::Relationship(m_slidePart, payload.RelationshipId);
    const auto part = PresentationEmbeddedObjectHelpers::Target(m_slidePart, payload.RelationshipId);
    if (!relationship || relationship->IsExternal || !part)
    {
        return false;
    }
    if (part->IsXmlPart())
    {
        if (!payload.Xml || !payload.BinaryData.empty())
        {
            return false;
        }
        part->SetXmlString(*payload.Xml);
    }
    else
    {
        if (payload.Xml)
        {
            return false;
        }
        part->SetBinaryData(payload.BinaryData);
    }
    if (!payload.ContentType.empty())
    {
        part->SetContentType(payload.ContentType);
    }
    return true;
}

bool PresentationShape::InsertTableRow(Size index, const MeasuringUnits& height)
{
    auto table = GetTable();
    if (!table || index > table->Rows.size() || !PresentationMeasurementHelpers::IsNonNegative(height) ||
        !PresentationMeasurementHelpers::ToInt64Emu(height))
    {
        return false;
    }
    for (auto& merge : table->Merges)
    {
        if (index <= merge.Row)
        {
            ++merge.Row;
        }
        else if (index < merge.Row + merge.RowSpan)
        {
            ++merge.RowSpan;
        }
    }
    table->Rows.insert(table->Rows.begin() + static_cast<std::vector<PresentationTableRow>::difference_type>(index),
                       PresentationTableRow{height, std::vector<PresentationTableCell>(table->ColumnWidths.size())});
    return SetTable(*table);
}

bool PresentationShape::RemoveTableRow(Size index)
{
    auto table = GetTable();
    if (!table || table->Rows.size() <= 1 || index >= table->Rows.size())
    {
        return false;
    }
    for (auto& merge : table->Merges)
    {
        if (index == merge.Row && merge.RowSpan > 1)
        {
            table->Rows[index + 1].Cells[merge.Column].Text = table->Rows[index].Cells[merge.Column].Text;
        }
    }
    table->Rows.erase(table->Rows.begin() + static_cast<std::vector<PresentationTableRow>::difference_type>(index));
    for (auto& merge : table->Merges)
    {
        if (index < merge.Row)
        {
            --merge.Row;
        }
        else if (index < merge.Row + merge.RowSpan)
        {
            --merge.RowSpan;
        }
    }
    std::erase_if(table->Merges, [](const auto& merge)
                  { return merge.RowSpan == 1 && merge.ColumnSpan == 1; });
    return SetTable(*table);
}

bool PresentationShape::InsertTableColumn(Size index, const MeasuringUnits& width)
{
    auto table = GetTable();
    if (!table || index > table->ColumnWidths.size() || !PresentationMeasurementHelpers::IsNonNegative(width) ||
        !PresentationMeasurementHelpers::ToInt64Emu(width))
    {
        return false;
    }
    for (auto& merge : table->Merges)
    {
        if (index <= merge.Column)
        {
            ++merge.Column;
        }
        else if (index < merge.Column + merge.ColumnSpan)
        {
            ++merge.ColumnSpan;
        }
    }
    table->ColumnWidths.insert(
        table->ColumnWidths.begin() + static_cast<std::vector<MeasuringUnits>::difference_type>(index), width);
    for (auto& row : table->Rows)
    {
        row.Cells.insert(row.Cells.begin() + static_cast<std::vector<PresentationTableCell>::difference_type>(index),
                         PresentationTableCell{});
    }
    return SetTable(*table);
}

bool PresentationShape::RemoveTableColumn(Size index)
{
    auto table = GetTable();
    if (!table || table->ColumnWidths.size() <= 1 || index >= table->ColumnWidths.size())
    {
        return false;
    }
    for (auto& merge : table->Merges)
    {
        if (index == merge.Column && merge.ColumnSpan > 1)
        {
            table->Rows[merge.Row].Cells[index + 1].Text = table->Rows[merge.Row].Cells[index].Text;
        }
    }
    table->ColumnWidths.erase(
        table->ColumnWidths.begin() + static_cast<std::vector<MeasuringUnits>::difference_type>(index));
    for (auto& row : table->Rows)
    {
        row.Cells.erase(row.Cells.begin() + static_cast<std::vector<PresentationTableCell>::difference_type>(index));
    }
    for (auto& merge : table->Merges)
    {
        if (index < merge.Column)
        {
            --merge.Column;
        }
        else if (index < merge.Column + merge.ColumnSpan)
        {
            --merge.ColumnSpan;
        }
    }
    std::erase_if(table->Merges, [](const auto& merge)
                  { return merge.RowSpan == 1 && merge.ColumnSpan == 1; });
    return SetTable(*table);
}

bool PresentationShape::MergeTableCells(Size row, Size column, Size rowSpan,
                                        Size columnSpan)
{
    auto table = GetTable();
    if (!table)
    {
        return false;
    }
    PresentationTableMerge merge{row, column, rowSpan, columnSpan};
    table->Merges.push_back(merge);
    return PresentationTableHelpers::IsValid(*table) && SetTable(*table);
}

bool PresentationShape::UnmergeTableCells(Size row, Size column)
{
    auto table = GetTable();
    if (!table)
    {
        return false;
    }
    const auto oldSize = table->Merges.size();
    std::erase_if(table->Merges, [=](const auto& merge)
                  { return merge.Row == row && merge.Column == column; });
    return table->Merges.size() != oldSize && SetTable(*table);
}

bool PresentationShape::Remove()
{
    return RemoveWithAnimationPolicy(PresentationAnimationRemovalPolicy::Warn) ==
           PresentationShapeRemovalResult::Removed;
}

PresentationShapeRemovalResult PresentationShape::RemoveWithAnimationPolicy(
    PresentationAnimationRemovalPolicy policy)
{
    auto parent = m_element ? m_element->Parent() : nullptr;
    if (!parent || !PresentationShapeTreeHelpers::IsDrawable(m_element))
    {
        return PresentationShapeRemovalResult::NotAttached;
    }
    const auto shapeId = Id();
    bool hasDependencies = false;
    for (const auto& animation : PresentationAnimationHelpers::Elements(m_slidePart))
    {
        if (PresentationAnimationHelpers::TargetId(animation) == shapeId)
        {
            hasDependencies = true;
            break;
        }
    }
    hasDependencies = hasDependencies || PresentationAnimationEffectHelpers::Targets(m_slidePart, shapeId);
    if (hasDependencies && policy == PresentationAnimationRemovalPolicy::Warn)
    {
        return PresentationShapeRemovalResult::AnimationDependencyWarning;
    }
    if (hasDependencies)
    {
        PresentationAnimationHelpers::RemoveTargeting(m_slidePart, shapeId);
        PresentationAnimationEffectHelpers::RemoveTargeting(m_slidePart, shapeId);
    }
    if (m_slidePart)
    {
        if (auto marker = PresentationMediaHelpers::Marker(m_element))
        {
            const auto mediaId = PresentationMediaHelpers::RelationshipId(marker);
            if (const auto relationship = PresentationMediaHelpers::Relationship(m_slidePart, mediaId))
            {
                if (relationship->IsExternal)
                {
                    m_slidePart->RemoveExternalRelationship(mediaId);
                }
                else if (auto part = PresentationMediaHelpers::Target(m_slidePart, mediaId))
                {
                    m_slidePart->RemovePartReference(part);
                }
            }
            if (auto node = PresentationMediaHelpers::TimingNode(
                    m_slidePart, PresentationMediaHelpers::ShapeId(m_element)))
            {
                if (auto host = node->Parent(); host && host->Parent())
                {
                    host->Parent()->RemoveChild(host);
                }
            }
        }
        std::vector<std::shared_ptr<Presentation::Picture>> pictures;
        if (auto picture = std::dynamic_pointer_cast<Presentation::Picture>(m_element))
        {
            pictures.push_back(picture);
        }
        const auto descendants = m_element->Descendants<Presentation::Picture>();
        pictures.insert(pictures.end(), descendants.begin(), descendants.end());
        for (const auto& picture : pictures)
        {
            if (auto fill = picture->GetFirstChildOfType<Presentation::BlipFill>())
            {
                if (auto blip = fill->GetFirstChildOfType<Drawing::Blip>())
                {
                    const auto embeddedId = blip->GetEmbed().ToString();
                    const auto linkedId = blip->GetLink().ToString();
                    if (!embeddedId.empty())
                    {
                        for (const auto& image : m_slidePart->GetImageParts())
                        {
                            if (image && image->RelationshipId() == embeddedId)
                            {
                                m_slidePart->RemoveImagePart(image);
                                break;
                            }
                        }
                    }
                    if (!linkedId.empty())
                    {
                        m_slidePart->RemoveExternalRelationship(linkedId);
                    }
                }
            }
            if (auto nv = picture->GetFirstChildOfType<Presentation::NonVisualPictureProperties>())
            {
                if (auto properties = nv->GetFirstChildOfType<Presentation::NonVisualDrawingProperties>())
                {
                    if (auto hyperlink = properties->GetFirstChildOfType<Drawing::HyperlinkOnClick>())
                    {
                        m_slidePart->RemoveExternalRelationship(hyperlink->GetId().ToString());
                    }
                }
            }
        }
    }
    return parent->RemoveChild(m_element) ? PresentationShapeRemovalResult::Removed
                                          : PresentationShapeRemovalResult::NotAttached;
}

PresentationShapeTree::PresentationShapeTree(std::shared_ptr<OpenXMLElement> tree,
                                             std::shared_ptr<Packaging::SlidePart> slidePart)
    : m_tree(std::move(tree)), m_slidePart(std::move(slidePart))
{
}

std::vector<PresentationShape::Ptr> PresentationShapeTree::Shapes() const
{
    std::vector<PresentationShape::Ptr> result;
    for (const auto& element : PresentationShapeTreeHelpers::Elements(m_tree))
    {
        result.push_back(PresentationShape::Ptr(new PresentationShape(element, m_slidePart)));
    }
    return result;
}

Size PresentationShapeTree::Count() const
{
    return Shapes().size();
}

PresentationShape::Ptr PresentationShapeTree::Get(Size index) const
{
    auto shapes = Shapes();
    return index < shapes.size() ? shapes[index] : nullptr;
}

PresentationShape::Ptr PresentationShapeTree::AddShape(std::string name)
{
    auto tree = std::dynamic_pointer_cast<Presentation::GroupShapeType>(m_tree);
    if (!tree)
    {
        return nullptr;
    }
    const auto id = PresentationShapeTreeHelpers::NextId(tree);
    auto shape = tree->InsertChild<Presentation::Shape>(PresentationShapeTreeHelpers::TrailingAnchor(tree));
    auto nv = shape ? shape->AppendChild<Presentation::NonVisualShapeProperties>() : nullptr;
    auto props = nv ? nv->AppendChild<Presentation::NonVisualDrawingProperties>() : nullptr;
    auto drawing = nv ? nv->AppendChild<Presentation::NonVisualShapeDrawingProperties>() : nullptr;
    auto app = nv ? nv->AppendChild<Presentation::ApplicationNonVisualDrawingProperties>() : nullptr;
    auto visual = shape ? shape->AppendChild<Presentation::ShapeProperties>() : nullptr;
    if (!shape || !nv || !props || !drawing || !app || !visual)
    {
        if (shape)
        {
            tree->RemoveChild(shape);
        }
        return nullptr;
    }
    props->SetId(UInt32Value(id));
    props->SetName(StringValue(name.empty() ? "Shape " + std::to_string(id) : std::move(name)));
    return PresentationShape::Ptr(new PresentationShape(shape, m_slidePart));
}

PresentationShape::Ptr PresentationShapeTree::AddConnector(std::string name)
{
    auto tree = std::dynamic_pointer_cast<Presentation::GroupShapeType>(m_tree);
    if (!tree)
    {
        return nullptr;
    }
    const auto id = PresentationShapeTreeHelpers::NextId(tree);
    auto connector =
        tree->InsertChild<Presentation::ConnectionShape>(PresentationShapeTreeHelpers::TrailingAnchor(tree));
    auto nv = connector ? connector->AppendChild<Presentation::NonVisualConnectionShapeProperties>() : nullptr;
    auto props = nv ? nv->AppendChild<Presentation::NonVisualDrawingProperties>() : nullptr;
    auto drawing = nv ? nv->AppendChild<Presentation::NonVisualConnectorShapeDrawingProperties>() : nullptr;
    auto app = nv ? nv->AppendChild<Presentation::ApplicationNonVisualDrawingProperties>() : nullptr;
    auto visual = connector ? connector->AppendChild<Presentation::ShapeProperties>() : nullptr;
    if (!connector || !nv || !props || !drawing || !app || !visual)
    {
        if (connector)
        {
            tree->RemoveChild(connector);
        }
        return nullptr;
    }
    props->SetId(UInt32Value(id));
    props->SetName(StringValue(name.empty() ? "Connector " + std::to_string(id) : std::move(name)));
    auto wrapper = PresentationShape::Ptr(new PresentationShape(connector, m_slidePart));
    if (!wrapper->SetPresetGeometry(Drawing::ShapeTypeValues::Line))
    {
        tree->RemoveChild(connector);
        return nullptr;
    }
    return wrapper;
}

PresentationShape::Ptr PresentationShapeTree::AddPicture(const PresentationPictureData& value)
{
    auto tree = std::dynamic_pointer_cast<Presentation::GroupShapeType>(m_tree);
    if (!tree || !m_slidePart)
    {
        return nullptr;
    }
    const auto id = PresentationShapeTreeHelpers::NextId(tree);
    auto picture = tree->InsertChild<Presentation::Picture>(PresentationShapeTreeHelpers::TrailingAnchor(tree));
    auto nonVisual = picture ? picture->AppendChild<Presentation::NonVisualPictureProperties>() : nullptr;
    auto properties = nonVisual ? nonVisual->AppendChild<Presentation::NonVisualDrawingProperties>() : nullptr;
    auto drawing = nonVisual ? nonVisual->AppendChild<Presentation::NonVisualPictureDrawingProperties>() : nullptr;
    auto app = nonVisual ? nonVisual->AppendChild<Presentation::ApplicationNonVisualDrawingProperties>() : nullptr;
    auto shapeProperties = picture ? picture->AppendChild<Presentation::ShapeProperties>() : nullptr;
    if (!picture || !nonVisual || !properties || !drawing || !app || !shapeProperties)
    {
        if (picture)
        {
            tree->RemoveChild(picture);
        }
        return nullptr;
    }
    properties->SetId(UInt32Value(id));
    properties->SetName(StringValue(value.Name.empty() ? "Picture " + std::to_string(id) : value.Name));
    auto wrapper = PresentationShape::Ptr(new PresentationShape(picture, m_slidePart));
    auto authored = value;
    if (authored.Name.empty())
    {
        authored.Name = "Picture " + std::to_string(id);
    }
    if (!wrapper->SetPicture(authored))
    {
        tree->RemoveChild(picture);
        return nullptr;
    }
    return wrapper;
}

PresentationShape::Ptr PresentationShapeTree::AddPictureFromData(
    std::vector<Byte> data, const PresentationShapeTransform& transform, std::string name)
{
    const auto format = ExyokiOffice::DetectImageFormat(data);
    if (!format || format->PixelWidth == 0 || format->PixelHeight == 0 ||
        format->HorizontalDpi <= 0.0 || format->VerticalDpi <= 0.0)
    {
        return nullptr;
    }
    PresentationPictureData picture;
    picture.Embedded = PresentationEmbeddedPicture{std::move(data), format->ContentType};
    picture.Name = std::move(name);
    picture.Transform = transform;
    if (picture.Transform.Size.Width.ToEmu().GetValue() == 0.0)
    {
        const auto width =
            static_cast<Real>(format->PixelWidth) / format->HorizontalDpi * 914400.0;
        picture.Transform.Size.Width = MeasuringUnits(width, MeasurementUnit::Emu);
    }
    if (picture.Transform.Size.Height.ToEmu().GetValue() == 0.0)
    {
        const auto height =
            static_cast<Real>(format->PixelHeight) / format->VerticalDpi * 914400.0;
        picture.Transform.Size.Height = MeasuringUnits(height, MeasurementUnit::Emu);
    }
    return AddPicture(picture);
}

PresentationShape::Ptr PresentationShapeTree::AddPictureFromFile(
    const std::filesystem::path& path, const PresentationShapeTransform& transform, std::string name)
{
    auto data = Packaging::ReadFileFully(path);
    if (data.empty())
    {
        return nullptr;
    }
    if (name.empty())
    {
        name = path.filename().string();
    }
    return AddPictureFromData(std::move(data), transform, std::move(name));
}

PresentationShape::Ptr PresentationShapeTree::AddMedia(const PresentationMediaData& value)
{
    auto tree = std::dynamic_pointer_cast<Presentation::GroupShapeType>(m_tree);
    if (!tree || !m_slidePart || !PresentationMediaHelpers::Valid(value))
    {
        return nullptr;
    }
    const auto id = PresentationShapeTreeHelpers::NextId(tree);
    auto picture = tree->InsertChild<Presentation::Picture>(PresentationShapeTreeHelpers::TrailingAnchor(tree));
    auto nonVisual = picture ? picture->AppendChild<Presentation::NonVisualPictureProperties>() : nullptr;
    auto properties = nonVisual ? nonVisual->AppendChild<Presentation::NonVisualDrawingProperties>() : nullptr;
    auto drawing = nonVisual ? nonVisual->AppendChild<Presentation::NonVisualPictureDrawingProperties>() : nullptr;
    auto app = nonVisual ? nonVisual->AppendChild<Presentation::ApplicationNonVisualDrawingProperties>() : nullptr;
    auto shapeProperties = picture ? picture->AppendChild<Presentation::ShapeProperties>() : nullptr;
    auto marker = app ? (value.Kind == PresentationMediaKind::Audio
                             ? std::static_pointer_cast<OpenXMLElement>(app->AppendChild<Drawing::AudioFromFile>())
                             : std::static_pointer_cast<OpenXMLElement>(app->AppendChild<Drawing::VideoFromFile>()))
                      : nullptr;
    if (!picture || !properties || !drawing || !app || !shapeProperties || !marker)
    {
        if (picture)
        {
            tree->RemoveChild(picture);
        }
        return nullptr;
    }
    properties->SetId(UInt32Value(id));
    properties->SetName(StringValue(value.Name.empty() ? "Media " + std::to_string(id) : value.Name));
    auto wrapper = PresentationShape::Ptr(new PresentationShape(picture, m_slidePart));
    auto authored = value;
    if (authored.Name.empty())
    {
        authored.Name = "Media " + std::to_string(id);
    }
    if (!wrapper->SetMedia(authored))
    {
        tree->RemoveChild(picture);
        return nullptr;
    }
    return wrapper;
}

PresentationShape::Ptr PresentationShapeTree::AddMediaFromFile(
    PresentationMediaKind kind,
    const std::filesystem::path& path,
    std::string contentType,
    const PresentationShapeTransform& transform,
    std::optional<PresentationEmbeddedPicture> posterFrame,
    const PresentationMediaPlayback& playback,
    std::string name)
{
    auto data = Packaging::ReadFileFully(path);
    if (data.empty() || contentType.empty())
    {
        return nullptr;
    }
    PresentationMediaData media;
    media.Kind = kind;
    media.Embedded = PresentationEmbeddedMedia{std::move(data), std::move(contentType)};
    media.PosterFrame = std::move(posterFrame);
    media.Playback = playback;
    media.Name = name.empty() ? path.filename().string() : std::move(name);
    media.Transform = transform;
    return AddMedia(media);
}

PresentationShape::Ptr PresentationShapeTree::AddTable(const PresentationTableData& value)
{
    auto tree = std::dynamic_pointer_cast<Presentation::GroupShapeType>(m_tree);
    if (!tree || !PresentationTableHelpers::IsValid(value))
    {
        return nullptr;
    }
    const auto id = PresentationShapeTreeHelpers::NextId(tree);
    auto frame = tree->InsertChild<Presentation::GraphicFrame>(PresentationShapeTreeHelpers::TrailingAnchor(tree));
    auto nonVisual = frame ? frame->AppendChild<Presentation::NonVisualGraphicFrameProperties>() : nullptr;
    auto properties = nonVisual ? nonVisual->AppendChild<Presentation::NonVisualDrawingProperties>() : nullptr;
    auto drawing = nonVisual ? nonVisual->AppendChild<Presentation::NonVisualGraphicFrameDrawingProperties>() : nullptr;
    auto app = nonVisual ? nonVisual->AppendChild<Presentation::ApplicationNonVisualDrawingProperties>() : nullptr;
    auto transform = frame ? frame->AppendChild<Presentation::Transform>() : nullptr;
    auto graphic = frame ? frame->AppendChild<Drawing::Graphic>() : nullptr;
    auto data = graphic ? graphic->AppendChild<Drawing::GraphicData>() : nullptr;
    auto table = data ? data->AppendChild<Drawing::Table>() : nullptr;
    if (!frame || !nonVisual || !properties || !drawing || !app || !transform || !graphic || !data || !table)
    {
        if (frame)
        {
            tree->RemoveChild(frame);
        }
        return nullptr;
    }
    properties->SetId(UInt32Value(id));
    properties->SetName(StringValue("Table " + std::to_string(id)));
    data->SetUri(StringValue(std::string(TableGraphicDataUri)));
    auto wrapper = PresentationShape::Ptr(new PresentationShape(frame, m_slidePart));
    if (!wrapper->SetTable(value))
    {
        tree->RemoveChild(frame);
        return nullptr;
    }
    return wrapper;
}

bool PresentationShapeTree::Remove(Size index)
{
    auto shape = Get(index);
    return shape && shape->Remove();
}

PresentationShape::Ptr PresentationShapeTree::Group(const std::vector<Size>& indices)
{
    auto tree = std::dynamic_pointer_cast<Presentation::GroupShapeType>(m_tree);
    auto elements = PresentationShapeTreeHelpers::Elements(tree);
    if (!tree || indices.size() < 2)
    {
        return nullptr;
    }
    std::vector<Size> sorted = indices;
    std::sort(sorted.begin(), sorted.end());
    if (sorted.back() >= elements.size() || std::adjacent_find(sorted.begin(), sorted.end()) != sorted.end())
    {
        return nullptr;
    }
    const auto id = PresentationShapeTreeHelpers::NextId(tree);
    auto group = tree->InsertChild<Presentation::GroupShape>(elements[sorted.front()]);
    auto nv = group ? group->AppendChild<Presentation::NonVisualGroupShapeProperties>() : nullptr;
    auto props = nv ? nv->AppendChild<Presentation::NonVisualDrawingProperties>() : nullptr;
    auto drawing =
        nv ? nv->AppendChild<Presentation::NonVisualGroupShapeDrawingProperties>() : nullptr;
    auto app = nv ? nv->AppendChild<Presentation::ApplicationNonVisualDrawingProperties>() : nullptr;
    auto groupProps = group ? group->AppendChild<Presentation::GroupShapeProperties>() : nullptr;
    auto transform = groupProps ? groupProps->AppendChild<DocumentFormat::OpenXml::Drawing::TransformGroup>() : nullptr;
    if (!group || !nv || !props || !drawing || !app || !groupProps || !transform)
    {
        if (group)
        {
            tree->RemoveChild(group);
        }
        return nullptr;
    }
    props->SetId(UInt32Value(id));
    props->SetName(StringValue("Group " + std::to_string(id)));
    for (const auto index : sorted)
    {
        if (!elements[index]->CopyInto(group))
        {
            tree->RemoveChild(group);
            return nullptr;
        }
    }
    for (auto it = sorted.rbegin(); it != sorted.rend(); ++it)
    {
        tree->RemoveChild(elements[*it]);
    }
    return PresentationShape::Ptr(new PresentationShape(group, m_slidePart));
}

bool PresentationShapeTree::Ungroup(Size index)
{
    auto elements = PresentationShapeTreeHelpers::Elements(m_tree);
    if (index >= elements.size())
    {
        return false;
    }
    auto groupElement = elements[index];
    if (!std::dynamic_pointer_cast<Presentation::GroupShape>(groupElement))
    {
        return false;
    }

    // Precompute the mapping from the group's child coordinate system to this
    // tree's coordinate system: a translation to the group origin plus the scale
    // implied by the group extent versus its child extent.
    struct Projection
    {
        Real OffsetX, OffsetY, ScaleX, ScaleY, ChildOffsetX, ChildOffsetY;
    };
    std::optional<Projection> projection;
    auto groupWrapper = PresentationShape::Ptr(new PresentationShape(groupElement, m_slidePart));
    if (auto transform = groupWrapper->GetTransform(); transform && transform->GroupChildPosition &&
                                                       transform->GroupChildSize)
    {
        const auto emu = [](const MeasuringUnits& value)
        { return value.ToEmu().GetValue(); };
        const Real extentX = emu(transform->Size.Width);
        const Real extentY = emu(transform->Size.Height);
        const Real childExtentX = emu(transform->GroupChildSize->Width);
        const Real childExtentY = emu(transform->GroupChildSize->Height);
        Projection value{emu(transform->Position.X),
                         emu(transform->Position.Y),
                         childExtentX != 0.0 ? extentX / childExtentX : 1.0,
                         childExtentY != 0.0 ? extentY / childExtentY : 1.0,
                         emu(transform->GroupChildPosition->X),
                         emu(transform->GroupChildPosition->Y)};
        if (std::isfinite(value.OffsetX) && std::isfinite(value.OffsetY) && std::isfinite(value.ScaleX) &&
            std::isfinite(value.ScaleY) && std::isfinite(value.ChildOffsetX) && std::isfinite(value.ChildOffsetY))
        {
            projection = value;
        }
    }

    // Copy each child in front of the group so relative z-order is preserved, then
    // remove the now-empty group. Rolls back cleanly if any copy fails.
    auto children = PresentationShapeTreeHelpers::Elements(groupElement);
    std::vector<std::shared_ptr<OpenXMLElement>> inserted;
    for (const auto& child : children)
    {
        auto copy = child->CopyInto(m_tree, groupElement);
        if (!copy)
        {
            for (const auto& element : inserted)
            {
                m_tree->RemoveChild(element);
            }
            return false;
        }
        inserted.push_back(copy);
    }
    if (projection)
    {
        const auto emu = [](const MeasuringUnits& value)
        { return value.ToEmu().GetValue(); };
        for (const auto& copy : inserted)
        {
            auto wrapper = PresentationShape::Ptr(new PresentationShape(copy, m_slidePart));
            auto transform = wrapper->GetTransform();
            if (!transform)
            {
                continue;
            }
            const Real x = projection->OffsetX + (emu(transform->Position.X) - projection->ChildOffsetX) * projection->ScaleX;
            const Real y = projection->OffsetY + (emu(transform->Position.Y) - projection->ChildOffsetY) * projection->ScaleY;
            const Real width = emu(transform->Size.Width) * projection->ScaleX;
            const Real height = emu(transform->Size.Height) * projection->ScaleY;
            if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(width) || !std::isfinite(height) ||
                width < 0.0 || height < 0.0)
            {
                continue;
            }
            transform->Position = PresentationPoint{static_cast<Int64>(std::llround(x)),
                                                    static_cast<Int64>(std::llround(y))};
            transform->Size = PresentationSize{static_cast<Int64>(std::llround(width)),
                                               static_cast<Int64>(std::llround(height))};
            wrapper->SetTransform(*transform);
        }
    }
    m_tree->RemoveChild(groupElement);
    return true;
}

bool PresentationShapeTree::Move(Size fromIndex, Size toIndex)
{
    auto elements = PresentationShapeTreeHelpers::Elements(m_tree);
    if (fromIndex >= elements.size() || toIndex >= elements.size())
    {
        return false;
    }
    if (fromIndex == toIndex)
    {
        return true;
    }
    auto order = elements;
    auto moving = order[fromIndex];
    order.erase(order.begin() + static_cast<PtrDiff>(fromIndex));
    order.insert(order.begin() + static_cast<PtrDiff>(toIndex), moving);
    auto anchor = PresentationShapeTreeHelpers::TrailingAnchor(m_tree);
    std::vector<std::shared_ptr<OpenXMLElement>> copies;
    for (const auto& element : order)
    {
        auto copy = element->CopyInto(m_tree, anchor);
        if (!copy)
        {
            for (const auto& inserted : copies)
            {
                m_tree->RemoveChild(inserted);
            }
            return false;
        }
        copies.push_back(copy);
    }
    for (const auto& element : elements)
    {
        m_tree->RemoveChild(element);
    }
    return true;
}

bool PresentationShapeTree::BringForward(Size index)
{
    return index + 1 < Count() && Move(index, index + 1);
}
bool PresentationShapeTree::SendBackward(Size index)
{
    return index > 0 && index < Count() && Move(index, index - 1);
}
bool PresentationShapeTree::BringToFront(Size index)
{
    const auto count = Count();
    return count && Move(index, count - 1);
}
bool PresentationShapeTree::SendToBack(Size index)
{
    return Move(index, 0);
}

class PresentationHierarchyHelpers
{
public:
    static std::vector<PresentationPlaceholder::Ptr> DirectPlaceholders(const std::shared_ptr<OpenXMLElement>& root,
                                                                        PlaceholderOrigin origin)
    {
        std::vector<PresentationPlaceholder::Ptr> result;
        if (!root)
        {
            return result;
        }
        for (const auto& placeholder : root->Descendants<Presentation::PlaceholderShape>())
        {
            auto applicationProperties = placeholder->Parent();
            auto nonVisualProperties = applicationProperties ? applicationProperties->Parent() : nullptr;
            auto host = nonVisualProperties ? nonVisualProperties->Parent() : nullptr;
            if (host)
            {
                result.push_back(PresentationPlaceholder::Ptr(new PresentationPlaceholder(host, placeholder, origin)));
            }
        }
        return result;
    }

    static PresentationPlaceholder::Ptr AddPlaceholder(const std::shared_ptr<OpenXMLElement>& root,
                                                       PlaceholderOrigin origin,
                                                       Presentation::PlaceholderValues::Value type,
                                                       std::optional<UInt32> index)
    {
        auto trees = root ? root->Descendants<Presentation::ShapeTree>() : std::vector<Presentation::ShapeTree::Ptr>{};
        auto tree = trees.empty() ? nullptr : trees.front();
        if (!tree)
        {
            return nullptr;
        }
        UInt32 shapeId = 2;
        for (const auto& property : root->Descendants<Presentation::NonVisualDrawingProperties>())
        {
            shapeId = std::max(shapeId, property->GetId().ValueOr(0) + 1);
        }
        auto shape = tree->AppendChild<Presentation::Shape>();
        auto nonVisual = shape ? shape->AppendChild<Presentation::NonVisualShapeProperties>() : nullptr;
        auto drawingProperties =
            nonVisual ? nonVisual->AppendChild<Presentation::NonVisualDrawingProperties>()
                      : nullptr;
        auto shapeProperties =
            nonVisual ? nonVisual->AppendChild<Presentation::NonVisualShapeDrawingProperties>() : nullptr;
        auto applicationProperties =
            nonVisual ? nonVisual->AppendChild<Presentation::ApplicationNonVisualDrawingProperties>() : nullptr;
        auto placeholder =
            applicationProperties ? applicationProperties->AppendChild<Presentation::PlaceholderShape>() : nullptr;
        auto visualProperties = shape ? shape->AppendChild<Presentation::ShapeProperties>() : nullptr;
        if (!shape || !nonVisual || !drawingProperties || !shapeProperties || !applicationProperties || !placeholder ||
            !visualProperties)
        {
            if (shape)
            {
                tree->RemoveChild(shape);
            }
            return nullptr;
        }
        drawingProperties->SetId(UInt32Value(shapeId));
        drawingProperties->SetName(StringValue("Placeholder " + std::to_string(shapeId)));
        placeholder->SetType(EnumValue<Presentation::PlaceholderValues>(Presentation::PlaceholderValues(type)));
        if (index)
        {
            placeholder->SetIndex(UInt32Value(*index));
        }
        return PresentationPlaceholder::Ptr(new PresentationPlaceholder(shape, placeholder, origin));
    }

    static std::string PlaceholderKey(const PresentationPlaceholder::Ptr& placeholder)
    {
        if (const auto index = placeholder ? placeholder->Index() : std::nullopt)
        {
            return "i:" + std::to_string(*index);
        }
        return "t:" + std::to_string(static_cast<int>(
                          placeholder ? placeholder->Type() : Presentation::PlaceholderValues::NotDefinedEnumValue));
    }

    static std::vector<PresentationPlaceholder::Ptr> MergePlaceholders(
        std::vector<PresentationPlaceholder::Ptr> direct, const std::vector<PresentationPlaceholder::Ptr>& inherited)
    {
        std::unordered_set<std::string> keys;
        for (const auto& placeholder : direct)
        {
            keys.insert(PlaceholderKey(placeholder));
        }
        for (const auto& placeholder : inherited)
        {
            if (keys.insert(PlaceholderKey(placeholder)).second)
            {
                direct.push_back(placeholder);
            }
        }
        return direct;
    }

    static std::string CommonSlideName(const std::shared_ptr<OpenXMLElement>& root)
    {
        auto common = root ? root->GetFirstChildOfType<Presentation::CommonSlideData>() : nullptr;
        return common ? common->GetName().ToString() : std::string{};
    }

    static PresentationSlideMaster::Ptr WrapMaster(const std::shared_ptr<Packaging::SlideMasterPart>& part)
    {
        auto* document = part ? dynamic_cast<PowerPointDocument*>(part->Package()) : nullptr;
        auto presentationPart = document ? document->GetPresentationPart() : nullptr;
        auto presentation = presentationPart ? presentationPart->GetTypedRootElement() : nullptr;
        auto list = presentation ? presentation->GetFirstChildOfType<Presentation::SlideMasterIdList>() : nullptr;
        if (!part || !presentationPart || !list)
        {
            return nullptr;
        }
        std::string relationshipId;
        for (const auto& incoming : part->IncomingRelationships())
        {
            if (incoming.SourceUri == presentationPart->Uri())
            {
                relationshipId = incoming.Id;
                break;
            }
        }
        for (const auto& entry : list->Elements<Presentation::SlideMasterId>())
        {
            if (entry->GetRelationshipId().ToString() == relationshipId)
            {
                return PresentationSlideMaster::Ptr(new PresentationSlideMaster(part, entry));
            }
        }
        return nullptr;
    }

    static PresentationSlideLayout::Ptr WrapLayout(const std::shared_ptr<Packaging::SlideLayoutPart>& part)
    {
        auto masterPart = part ? part->GetSlideMasterPart() : nullptr;
        if (!masterPart && part)
        {
            for (const auto& incoming : part->IncomingRelationships())
            {
                auto candidate = part->Package()->GetPartByUri(incoming.SourceUri);
                masterPart = std::dynamic_pointer_cast<Packaging::SlideMasterPart>(candidate);
                if (masterPart)
                {
                    break;
                }
            }
        }
        auto master = WrapMaster(masterPart);
        auto masterRoot = masterPart ? masterPart->GetTypedRootElement() : nullptr;
        auto list = masterRoot ? masterRoot->GetFirstChildOfType<Presentation::SlideLayoutIdList>() : nullptr;
        if (!part || !master || !list)
        {
            return nullptr;
        }
        std::string relationshipId;
        for (const auto& incoming : part->IncomingRelationships())
        {
            if (incoming.SourceUri == masterPart->Uri())
            {
                relationshipId = incoming.Id;
                break;
            }
        }
        for (const auto& entry : list->Elements<Presentation::SlideLayoutId>())
        {
            if (entry->GetRelationshipId().ToString() == relationshipId)
            {
                return PresentationSlideLayout::Ptr(new PresentationSlideLayout(part, entry, master));
            }
        }
        return nullptr;
    }
};

PresentationPlaceholder::PresentationPlaceholder(std::shared_ptr<OpenXMLElement> element,
                                                 std::shared_ptr<Presentation::PlaceholderShape> placeholder,
                                                 PlaceholderOrigin origin)
    : m_element(std::move(element)), m_placeholder(std::move(placeholder)), m_origin(origin)
{
}

PlaceholderOrigin PresentationPlaceholder::Origin() const noexcept
{
    return m_origin;
}

Presentation::PlaceholderValues::Value PresentationPlaceholder::Type() const
{
    return !m_placeholder ? Presentation::PlaceholderValues::Object
                          : m_placeholder->GetType().ValueOr(Presentation::PlaceholderValues::Object).GetValue();
}

std::optional<UInt32> PresentationPlaceholder::Index() const
{
    if (!m_placeholder || !m_placeholder->GetIndex().IsDefined())
    {
        return std::nullopt;
    }
    return m_placeholder->GetIndex().ValueOr(0);
}

std::shared_ptr<Presentation::Shape> PresentationPlaceholder::GetShape() const
{
    return std::dynamic_pointer_cast<Presentation::Shape>(m_element);
}

std::shared_ptr<OpenXMLElement> PresentationPlaceholder::GetElement() const
{
    return m_element;
}

bool PresentationPlaceholder::Remove()
{
    auto parent = m_element ? m_element->Parent() : nullptr;
    return parent && parent->RemoveChild(m_element);
}

PresentationSlideMaster::PresentationSlideMaster(std::shared_ptr<Packaging::SlideMasterPart> part,
                                                 std::shared_ptr<Presentation::SlideMasterId> entry)
    : m_part(std::move(part)), m_entry(std::move(entry))
{
}

UInt32 PresentationSlideMaster::Id() const
{
    return m_entry ? m_entry->GetId().ValueOr(0) : 0;
}

std::string PresentationSlideMaster::Name() const
{
    return PresentationHierarchyHelpers::CommonSlideName(m_part ? m_part->GetTypedRootElement() : nullptr);
}

std::vector<PresentationSlideLayout::Ptr> PresentationSlideMaster::Layouts() const
{
    std::vector<PresentationSlideLayout::Ptr> result;
    auto root = m_part ? m_part->GetTypedRootElement() : nullptr;
    auto list = root ? root->GetFirstChildOfType<Presentation::SlideLayoutIdList>() : nullptr;
    if (!list)
    {
        return result;
    }
    for (const auto& entry : list->Elements<Presentation::SlideLayoutId>())
    {
        for (const auto& part : m_part->GetSlideLayoutParts())
        {
            for (const auto& incoming : part->IncomingRelationships())
            {
                if (incoming.SourceUri == m_part->Uri() && incoming.Id == entry->GetRelationshipId().ToString())
                {
                    auto layout = PresentationHierarchyHelpers::WrapLayout(part);
                    if (layout)
                    {
                        result.push_back(std::move(layout));
                    }
                    break;
                }
            }
        }
    }
    return result;
}

std::vector<PresentationPlaceholder::Ptr> PresentationSlideMaster::Placeholders() const
{
    return PresentationHierarchyHelpers::DirectPlaceholders(m_part ? m_part->GetTypedRootElement() : nullptr,
                                                            PlaceholderOrigin::Master);
}

PresentationPlaceholder::Ptr PresentationSlideMaster::AddPlaceholder(Presentation::PlaceholderValues::Value type,
                                                                     std::optional<UInt32> index)
{
    return PresentationHierarchyHelpers::AddPlaceholder(m_part ? m_part->GetTypedRootElement() : nullptr,
                                                        PlaceholderOrigin::Master, type, index);
}

std::optional<std::string> PresentationSlideMaster::ThemeXml() const
{
    return ThemeService::ReadXml(m_part ? m_part->GetThemePart() : nullptr);
}

bool PresentationSlideMaster::SetThemeXml(std::string xml)
{
    if (!m_part || !ThemeService::IsValidThemeXml(xml))
    {
        return false;
    }
    auto theme = m_part->GetThemePart();
    if (!theme)
    {
        theme = m_part->AddThemePart();
    }
    return ThemeService::WriteXml(theme, std::move(xml));
}

std::optional<PresentationThemeSettings> PresentationSlideMaster::ThemeSettings() const
{
    return ThemeService::ReadSettings(m_part ? m_part->GetThemePart() : nullptr);
}

bool PresentationSlideMaster::SetThemeSettings(const PresentationThemeSettings& settings)
{
    return ThemeService::WriteSettings(m_part ? m_part->GetThemePart() : nullptr, settings);
}

bool PresentationSlideMaster::RemoveTheme()
{
    return m_part && m_part->RemoveThemePart();
}

std::shared_ptr<Packaging::SlideMasterPart> PresentationSlideMaster::GetPart() const
{
    return m_part;
}

PresentationSlideLayout::PresentationSlideLayout(std::shared_ptr<Packaging::SlideLayoutPart> part,
                                                 std::shared_ptr<Presentation::SlideLayoutId> entry,
                                                 std::shared_ptr<PresentationSlideMaster> master)
    : m_part(std::move(part)), m_entry(std::move(entry)), m_master(std::move(master))
{
}

UInt32 PresentationSlideLayout::Id() const
{
    return m_entry ? m_entry->GetId().ValueOr(0) : 0;
}

std::string PresentationSlideLayout::Name() const
{
    return PresentationHierarchyHelpers::CommonSlideName(m_part ? m_part->GetTypedRootElement() : nullptr);
}

Presentation::SlideLayoutValues::Value PresentationSlideLayout::Type() const
{
    auto root = m_part ? m_part->GetTypedRootElement() : nullptr;
    return root ? root->GetType().ValueOr(Presentation::SlideLayoutValues::Custom).GetValue()
                : Presentation::SlideLayoutValues::NotDefinedEnumValue;
}

PresentationSlideMaster::Ptr PresentationSlideLayout::Master() const
{
    return m_master;
}

std::vector<PresentationPlaceholder::Ptr> PresentationSlideLayout::Placeholders(bool includeInherited) const
{
    auto direct = PresentationHierarchyHelpers::DirectPlaceholders(m_part ? m_part->GetTypedRootElement() : nullptr,
                                                                   PlaceholderOrigin::Layout);
    if (!includeInherited || !m_master)
    {
        return direct;
    }
    return PresentationHierarchyHelpers::MergePlaceholders(std::move(direct), m_master->Placeholders());
}

PresentationPlaceholder::Ptr PresentationSlideLayout::AddPlaceholder(Presentation::PlaceholderValues::Value type,
                                                                     std::optional<UInt32> index)
{
    return PresentationHierarchyHelpers::AddPlaceholder(m_part ? m_part->GetTypedRootElement() : nullptr,
                                                        PlaceholderOrigin::Layout, type, index);
}

std::shared_ptr<Packaging::SlideLayoutPart> PresentationSlideLayout::GetPart() const
{
    return m_part;
}

PresentationSlide::PresentationSlide(std::shared_ptr<Packaging::SlidePart> part,
                                     std::shared_ptr<Presentation::SlideId> entry)
    : m_part(std::move(part)), m_entry(std::move(entry))
{
}

UInt32 PresentationSlide::Id() const
{
    return m_entry ? m_entry->GetId().ValueOr(0) : 0;
}
std::string PresentationSlide::RelationshipId() const
{
    return m_entry ? m_entry->GetRelationshipId().ToString() : std::string{};
}
bool PresentationSlide::IsHidden() const
{
    auto slide = m_part ? m_part->GetTypedRootElement() : nullptr;
    return slide && !slide->GetShow().ValueOr(true);
}
bool PresentationSlide::SetHidden(bool hidden)
{
    auto slide = m_part ? m_part->GetTypedRootElement() : nullptr;
    if (!slide)
    {
        return false;
    }
    slide->SetShow(BooleanValue(!hidden));
    return true;
}

std::optional<PresentationTransitionData> PresentationSlide::GetTransition() const
{
    auto slide = m_part ? m_part->GetTypedRootElement() : nullptr;
    auto transition = slide ? slide->GetFirstChildOfType<Presentation::Transition>() : nullptr;
    if (!transition)
    {
        return std::nullopt;
    }

    PresentationTransitionData result;
    result.Kind = PresentationTransitionHelpers::Kind(transition);
    result.Speed = PresentationTransitionHelpers::Speed(transition);
    result.Duration = PresentationTransitionHelpers::Milliseconds(transition->GetDuration());
    result.AdvanceOnClick = transition->GetAdvanceOnClick().ValueOr(true);
    result.AdvanceAfter = PresentationTransitionHelpers::Milliseconds(transition->GetAdvanceAfterTime());
    result.Options = PresentationTransitionHelpers::ReadOptions(transition, result.Kind);

    const auto sounds = transition->Descendants<Presentation::Sound>();
    if (!sounds.empty())
    {
        const auto id = sounds.front()->GetEmbed().ToString();
        if (auto part = PresentationMediaHelpers::Target(m_part, id))
        {
            PresentationTransitionSound sound;
            sound.Audio = {part->GetBinaryData(), std::string(part->ContentType())};
            sound.Name = sounds.front()->GetName().ToString();
            sound.BuiltIn = sounds.front()->GetBuiltIn().ValueOr(false);
            const auto starts = transition->Descendants<Presentation::StartSoundAction>();
            sound.Loop = !starts.empty() && starts.front()->GetLoop().ValueOr(false);
            result.Sound = std::move(sound);
        }
    }
    return result;
}

bool PresentationSlide::SetTransition(const PresentationTransitionData& value)
{
    if (!m_part || (value.Sound && (value.Sound->Audio.Data.empty() || value.Sound->Audio.ContentType.empty())) ||
        !PresentationTransitionHelpers::OptionsAccepted(value.Kind, value.Options))
    {
        return false;
    }
    auto slide = m_part->GetTypedRootElement();
    auto transition = slide ? slide->GetFirstChildOfType<Presentation::Transition>() : nullptr;
    if (!slide || (value.Kind == PresentationTransitionKind::Unsupported &&
                   !PresentationTransitionHelpers::HasOpaqueEffect(transition)))
    {
        return false;
    }

    std::shared_ptr<OpenXmlPackagePart> newSoundPart;
    if (value.Sound)
    {
        const auto& descriptor = PresentationMediaHelpers::Descriptor(PresentationMediaKind::Audio);
        newSoundPart = std::make_shared<OpenXmlPackagePart>(descriptor);
        if (!m_part->AttachCustomPart(newSoundPart, descriptor, true))
        {
            return false;
        }
        newSoundPart->SetContentType(value.Sound->Audio.ContentType);
        newSoundPart->SetBinaryData(value.Sound->Audio.Data);
    }

    if (!transition)
    {
        std::shared_ptr<OpenXMLElement> before;
        for (const auto& child : slide->Children())
        {
            if (std::dynamic_pointer_cast<Presentation::Timing>(child) ||
                std::dynamic_pointer_cast<Presentation::SlideExtensionList>(child))
            {
                before = child;
                break;
            }
        }
        transition = slide->InsertChild<Presentation::Transition>(before);
        if (!transition)
        {
            if (newSoundPart)
            {
                m_part->RemovePartReference(newSoundPart);
            }
            return false;
        }
    }

    const auto oldSoundId = PresentationTransitionHelpers::SoundId(transition);
    auto oldSoundPart = PresentationMediaHelpers::Target(m_part, oldSoundId);
    if (value.Kind != PresentationTransitionKind::Unsupported)
    {
        PresentationTransitionHelpers::RemoveEffect(transition);
        if (!PresentationTransitionHelpers::AppendEffect(transition, value.Kind, value.Options))
        {
            if (newSoundPart)
            {
                m_part->RemovePartReference(newSoundPart);
            }
            return false;
        }
    }
    if (auto oldAction = transition->GetFirstChildOfType<Presentation::SoundAction>())
    {
        transition->RemoveChild(oldAction);
    }

    transition->SetSpeed(EnumValue<Presentation::TransitionSpeedValues>(
        PresentationTransitionHelpers::Speed(value.Speed)));
    transition->SetDuration(value.Duration ? StringValue(std::to_string(*value.Duration)) : StringValue{});
    transition->SetAdvanceOnClick(BooleanValue(value.AdvanceOnClick));
    transition->SetAdvanceAfterTime(value.AdvanceAfter ? StringValue(std::to_string(*value.AdvanceAfter))
                                                       : StringValue{});
    if (value.Sound)
    {
        auto action = transition->AppendChild<Presentation::SoundAction>();
        auto start = action ? action->AppendChild<Presentation::StartSoundAction>() : nullptr;
        auto sound = start ? start->AppendChild<Presentation::Sound>() : nullptr;
        if (!action || !start || !sound)
        {
            m_part->RemovePartReference(newSoundPart);
            return false;
        }
        start->SetLoop(BooleanValue(value.Sound->Loop));
        sound->SetEmbed(StringValue(newSoundPart->RelationshipId()));
        sound->SetName(StringValue(value.Sound->Name));
        sound->SetBuiltIn(BooleanValue(value.Sound->BuiltIn));
    }

    if (oldSoundPart)
    {
        m_part->RemovePartReference(oldSoundPart);
    }
    else if (!oldSoundId.empty())
    {
        m_part->RemoveExternalRelationship(oldSoundId);
    }
    return true;
}

bool PresentationSlide::RemoveTransition()
{
    auto slide = m_part ? m_part->GetTypedRootElement() : nullptr;
    auto transition = slide ? slide->GetFirstChildOfType<Presentation::Transition>() : nullptr;
    if (!slide || !transition)
    {
        return false;
    }
    const auto soundId = PresentationTransitionHelpers::SoundId(transition);
    if (auto soundPart = PresentationMediaHelpers::Target(m_part, soundId))
    {
        m_part->RemovePartReference(soundPart);
    }
    else if (!soundId.empty())
    {
        m_part->RemoveExternalRelationship(soundId);
    }
    return slide->RemoveChild(transition);
}

std::vector<PresentationAnimationNode> PresentationSlide::Animations() const
{
    std::vector<PresentationAnimationNode> result;
    for (const auto& animation : PresentationAnimationHelpers::Elements(m_part))
    {
        auto node = PresentationAnimationHelpers::TimeNode(animation);
        const auto targetId = PresentationAnimationHelpers::TargetId(animation);
        if (!node || targetId == 0)
        {
            continue;
        }
        PresentationAnimationNode value;
        value.Id = node->GetId().ValueOr(0);
        value.TargetShapeId = targetId;
        value.Duration = PresentationTransitionHelpers::Milliseconds(node->GetDuration());
        value.From = animation->GetFrom().ToString();
        value.To = animation->GetTo().ToString();
        value.By = animation->GetBy().ToString();
        result.push_back(std::move(value));
    }
    return result;
}

std::optional<UInt32> PresentationSlide::AddAnimation(const PresentationAnimationNode& value)
{
    if (!m_part || !PresentationAnimationHelpers::ShapeExists(m_part, value.TargetShapeId))
    {
        return std::nullopt;
    }
    const auto id = value.Id == 0 ? PresentationTimeNodeIdAllocator::Next(m_part) : value.Id;
    if (!PresentationTimeNodeIdAllocator::IsAvailable(m_part, id))
    {
        return std::nullopt;
    }

    auto slide = m_part->GetTypedRootElement();
    auto timing = slide ? slide->GetFirstChildOfType<Presentation::Timing>() : nullptr;
    if (!timing && slide)
    {
        timing = slide->AppendChild<Presentation::Timing>();
    }
    auto list = timing ? timing->GetFirstChildOfType<Presentation::TimeNodeList>() : nullptr;
    if (!list && timing)
    {
        list = timing->AppendChild<Presentation::TimeNodeList>();
    }
    auto animation = list ? list->AppendChild<Presentation::Animate>() : nullptr;
    auto behavior = animation ? animation->AppendChild<Presentation::CommonBehavior>() : nullptr;
    auto node = behavior ? behavior->AppendChild<Presentation::CommonTimeNode>() : nullptr;
    auto targetElement = behavior ? behavior->AppendChild<Presentation::TargetElement>() : nullptr;
    auto target = targetElement ? targetElement->AppendChild<Presentation::ShapeTarget>() : nullptr;
    if (!list || !animation || !behavior || !node || !targetElement || !target ||
        !PresentationAnimationHelpers::Write(animation, value, id))
    {
        if (list && animation)
        {
            list->RemoveChild(animation);
        }
        return std::nullopt;
    }
    return id;
}

bool PresentationSlide::UpdateAnimation(UInt32 id, const PresentationAnimationNode& value)
{
    auto animation = PresentationAnimationHelpers::Find(m_part, id);
    if (!animation || (value.Id != 0 && value.Id != id) ||
        !PresentationAnimationHelpers::ShapeExists(m_part, value.TargetShapeId))
    {
        return false;
    }
    return PresentationAnimationHelpers::Write(animation, value, id);
}

bool PresentationSlide::RemoveAnimation(UInt32 id)
{
    auto animation = PresentationAnimationHelpers::Find(m_part, id);
    auto parent = animation ? animation->Parent() : nullptr;
    return parent && parent->RemoveChild(animation);
}

std::vector<PresentationAnimationEffectData> PresentationSlide::AnimationEffects() const
{
    return PresentationAnimationEffectHelpers::ReadAll(m_part, nullptr);
}

bool PresentationSlide::SetAnimationEffects(const std::vector<PresentationAnimationEffectData>& effects)
{
    return PresentationAnimationEffectHelpers::Rebuild(m_part, effects, nullptr);
}

std::optional<UInt32> PresentationSlide::AddAnimationEffect(const PresentationAnimationEffectData& effect)
{
    auto effects = AnimationEffects();
    effects.push_back(effect);
    std::vector<PresentationAnimationEffectData> written;
    if (!PresentationAnimationEffectHelpers::Rebuild(m_part, effects, &written) || written.empty())
    {
        return std::nullopt;
    }
    // Canonical ordering can move the appended effect, so identify it by trigger and position.
    for (auto candidate = written.rbegin(); candidate != written.rend(); ++candidate)
    {
        if (candidate->TriggerShapeId == effect.TriggerShapeId)
        {
            return candidate->Id;
        }
    }
    return std::nullopt;
}

bool PresentationSlide::UpdateAnimationEffect(UInt32 id, const PresentationAnimationEffectData& effect)
{
    if (id == 0 || (effect.Id != 0 && effect.Id != id))
    {
        return false;
    }
    auto effects = AnimationEffects();
    bool found = false;
    for (auto& candidate : effects)
    {
        if (candidate.Id != id)
        {
            continue;
        }
        // An opaque effect is stored verbatim and cannot be replaced by a modelled one.
        if (candidate.Effect == PresentationAnimationEffect::Unsupported ||
            effect.Effect == PresentationAnimationEffect::Unsupported)
        {
            return false;
        }
        candidate = effect;
        candidate.Id = id;
        found = true;
        break;
    }
    return found && PresentationAnimationEffectHelpers::Rebuild(m_part, effects, nullptr);
}

bool PresentationSlide::RemoveAnimationEffect(UInt32 id)
{
    auto effects = AnimationEffects();
    const auto position = std::find_if(effects.begin(), effects.end(),
                                       [id](const PresentationAnimationEffectData& effect)
                                       {
                                           return effect.Id == id;
                                       });
    if (id == 0 || position == effects.end())
    {
        return false;
    }
    effects.erase(position);
    return PresentationAnimationEffectHelpers::Rebuild(m_part, effects, nullptr);
}

bool PresentationSlide::MoveAnimationEffect(UInt32 id, Size index)
{
    auto effects = AnimationEffects();
    const auto position = std::find_if(effects.begin(), effects.end(),
                                       [id](const PresentationAnimationEffectData& effect)
                                       {
                                           return effect.Id == id;
                                       });
    if (id == 0 || position == effects.end() || index >= effects.size())
    {
        return false;
    }
    const auto moved = *position;
    effects.erase(position);
    effects.insert(effects.begin() + static_cast<PtrDiff>(index), moved);
    return PresentationAnimationEffectHelpers::Rebuild(m_part, effects, nullptr);
}

bool PresentationSlide::ClearAnimationEffects()
{
    return PresentationAnimationEffectHelpers::Rebuild(m_part, {}, nullptr);
}

PresentationSlideLayout::Ptr PresentationSlide::Layout() const
{
    return PresentationHierarchyHelpers::WrapLayout(m_part ? m_part->GetSlideLayoutPart() : nullptr);
}

std::vector<PresentationPlaceholder::Ptr> PresentationSlide::Placeholders(bool includeInherited) const
{
    auto direct = PresentationHierarchyHelpers::DirectPlaceholders(m_part ? m_part->GetTypedRootElement() : nullptr,
                                                                   PlaceholderOrigin::Slide);
    auto layout = Layout();
    if (!includeInherited || !layout)
    {
        return direct;
    }
    return PresentationHierarchyHelpers::MergePlaceholders(std::move(direct), layout->Placeholders(true));
}

PresentationPlaceholder::Ptr PresentationSlide::AddPlaceholder(Presentation::PlaceholderValues::Value type,
                                                               std::optional<UInt32> index)
{
    return PresentationHierarchyHelpers::AddPlaceholder(m_part ? m_part->GetTypedRootElement() : nullptr,
                                                        PlaceholderOrigin::Slide, type, index);
}

PresentationShapeTree::Ptr PresentationSlide::ShapeTree() const
{
    auto root = m_part ? m_part->GetTypedRootElement() : nullptr;
    auto trees = root ? root->Descendants<Presentation::ShapeTree>() : std::vector<Presentation::ShapeTree::Ptr>{};
    return trees.empty() ? nullptr : PresentationShapeTree::Ptr(new PresentationShapeTree(trees.front(), m_part));
}

std::string PresentationSlide::NotesText() const
{
    auto page = NotesPage();
    return page ? page->Text : std::string{};
}

std::optional<PresentationNotesPage> PresentationSlide::NotesPage() const
{
    auto part = m_part ? m_part->GetNotesSlidePart() : nullptr;
    auto root = part ? part->GetTypedRootElement() : nullptr;
    if (!root)
    {
        return std::nullopt;
    }
    PresentationNotesPage page;
    page.Text = PresentationCommentHelpers::Text(root);
    page.ShowMasterShapes = root->GetShowMasterShapes().ValueOr(true);
    page.ShowMasterPlaceholderAnimations = root->GetShowMasterPlaceholderAnimations().ValueOr(true);
    return page;
}

bool PresentationSlide::SetNotesText(const std::string& text)
{
    auto page = NotesPage().value_or(PresentationNotesPage{});
    page.Text = text;
    return SetNotesPage(page);
}

bool PresentationSlide::SetNotesPage(const PresentationNotesPage& page)
{
    if (!m_part)
    {
        return false;
    }
    auto part = m_part->GetNotesSlidePart();
    if (!part)
    {
        part = m_part->AddNotesSlidePart();
        // A notes slide points back at the slide it annotates and at the
        // presentation-wide notes master; PowerPoint reports a package without
        // those relationships as damaged.
        auto notesMaster = PresentationNotesMasterHelpers::Ensure(m_part);
        if (!part || !notesMaster || part->AddPartReference(m_part, SlideRelationship).empty() ||
            part->AddPartReference(notesMaster, NotesMasterRelationship).empty())
        {
            m_part->RemoveNotesSlidePart();
            return false;
        }
    }
    if (!part)
    {
        return false;
    }
    return PresentationDomBuilders::InitializeNotesPage(part->GetTypedRootElement(), page);
}

bool PresentationSlide::RemoveNotes()
{
    return m_part && m_part->RemoveNotesSlidePart();
}

std::vector<PresentationComment> PresentationSlide::Comments() const
{
    std::vector<PresentationComment> result;
    if (!m_part)
    {
        return result;
    }
    for (const auto& part : m_part->GetcommentParts())
    {
        if (auto list = part ? part->GetTypedRootElement() : nullptr)
        {
            for (const auto& comment : list->Elements<ModernComments::Comment>())
            {
                result.push_back(PresentationCommentHelpers::Read(comment));
            }
        }
    }
    return result;
}

bool PresentationSlide::AddComment(const PresentationComment& value)
{
    if (!m_part || !PresentationCommentHelpers::IsValid(value) ||
        !PresentationCommentHelpers::AuthorsExist(m_part, value) ||
        !PresentationCommentHelpers::IdsAreUnique(m_part, value))
    {
        return false;
    }
    auto parts = m_part->GetcommentParts();
    auto part = parts.empty() ? m_part->AddPowerPointCommentPart() : parts.front();
    auto list = part ? part->GetTypedRootElement() : nullptr;
    return PresentationCommentHelpers::Append(list, value) != nullptr;
}

bool PresentationSlide::UpdateComment(std::string_view id, const PresentationComment& value)
{
    if (id.empty() || value.Id != id || !PresentationCommentHelpers::IsValid(value) || !m_part ||
        !PresentationCommentHelpers::AuthorsExist(m_part, value) ||
        !PresentationCommentHelpers::IdsAreUnique(m_part, value, id))
    {
        return false;
    }
    for (const auto& part : m_part->GetcommentParts())
    {
        auto list = part ? part->GetTypedRootElement() : nullptr;
        if (!list)
        {
            continue;
        }
        for (const auto& comment : list->Elements<ModernComments::Comment>())
        {
            if (comment->GetId().ToString() == id)
            {
                // An edit keeps the thread's original creation timestamp.
                auto replacement = PresentationCommentHelpers::Append(list, value, comment->GetCreated());
                if (!replacement)
                {
                    return false;
                }
                list->RemoveChild(comment);
                return true;
            }
        }
    }
    return false;
}

bool PresentationSlide::SetCommentStatus(std::string_view id, PresentationCommentStatus status)
{
    if (!m_part || id.empty())
    {
        return false;
    }
    for (const auto& part : m_part->GetcommentParts())
    {
        auto list = part ? part->GetTypedRootElement() : nullptr;
        if (!list)
        {
            continue;
        }
        for (const auto& comment : list->Elements<ModernComments::Comment>())
        {
            if (comment->GetId().ToString() == id)
            {
                comment->SetStatus(
                    EnumValue<ModernComments::CommentStatus>(PresentationCommentHelpers::WriteStatus(status)));
                return true;
            }
        }
    }
    return false;
}

bool PresentationSlide::RemoveComment(std::string_view id)
{
    if (!m_part || id.empty())
    {
        return false;
    }
    for (const auto& part : m_part->GetcommentParts())
    {
        auto list = part ? part->GetTypedRootElement() : nullptr;
        if (!list)
        {
            continue;
        }
        for (const auto& comment : list->Elements<ModernComments::Comment>())
        {
            if (comment->GetId().ToString() == id)
            {
                list->RemoveChild(comment);
                if (list->Elements<ModernComments::Comment>().empty())
                {
                    m_part->RemovePowerPointCommentPart(part);
                }
                return true;
            }
        }
    }
    return false;
}

std::shared_ptr<Packaging::SlidePart> PresentationSlide::GetPart() const
{
    return m_part;
}

PowerPointDocumentEditor::SlideBuilder::SlideBuilder(PowerPointDocumentEditor* editor) : m_editor(editor)
{
}

PowerPointDocumentEditor::SlideBuilder& PowerPointDocumentEditor::SlideBuilder::SetLayout(
    const PresentationSlideLayout::Ptr& layout)
{
    m_layout = layout;
    return *this;
}

PowerPointDocumentEditor::SlideBuilder& PowerPointDocumentEditor::SlideBuilder::SetHidden(bool hidden)
{
    m_hidden = hidden;
    return *this;
}

/// Builds a paragraph holding a single unformatted run.
class SlideBuilderTextHelpers
{
public:
    static PresentationTextParagraph SingleRunParagraph(std::string text)
    {
        PresentationTextRun run;
        run.Text = std::move(text);

        PresentationTextParagraph paragraph;
        paragraph.Runs.push_back(std::move(run));
        return paragraph;
    }
};

PowerPointDocumentEditor::SlideBuilder& PowerPointDocumentEditor::SlideBuilder::SetTitle(
    std::string title, const PresentationShapeTransform& transform)
{
    PresentationTextFrame frame;
    frame.Paragraphs.push_back(SlideBuilderTextHelpers::SingleRunParagraph(std::move(title)));
    m_title = TextBoxInstruction{std::move(frame), transform, "Title"};
    return *this;
}

PowerPointDocumentEditor::SlideBuilder& PowerPointDocumentEditor::SlideBuilder::ClearTitle()
{
    m_title.reset();
    return *this;
}

PowerPointDocumentEditor::SlideBuilder& PowerPointDocumentEditor::SlideBuilder::AddTextBox(
    std::string text, const PresentationShapeTransform& transform, std::string name)
{
    PresentationTextFrame frame;
    Size begin = 0;
    do
    {
        const auto end = text.find('\n', begin);
        auto line = text.substr(begin, end == std::string::npos ? end : end - begin);
        if (!line.empty() && line.back() == '\r')
        {
            line.pop_back();
        }
        frame.Paragraphs.push_back(SlideBuilderTextHelpers::SingleRunParagraph(std::move(line)));
        if (end == std::string::npos)
        {
            break;
        }
        begin = end + 1;
    } while (true);
    return AddTextBox(std::move(frame), transform, std::move(name));
}

PowerPointDocumentEditor::SlideBuilder& PowerPointDocumentEditor::SlideBuilder::AddTextBox(
    PresentationTextFrame frame, const PresentationShapeTransform& transform, std::string name)
{
    m_textBoxes.push_back({std::move(frame), transform, std::move(name)});
    return *this;
}

PowerPointDocumentEditor::SlideBuilder& PowerPointDocumentEditor::SlideBuilder::AddShape(
    Drawing::ShapeTypeValues::Value type, const PresentationShapeTransform& transform, std::string name)
{
    m_shapes.push_back({type, transform, std::move(name)});
    return *this;
}

PowerPointDocumentEditor::SlideBuilder& PowerPointDocumentEditor::SlideBuilder::ClearContent()
{
    m_textBoxes.clear();
    m_shapes.clear();
    return *this;
}

PresentationSlide::Ptr PowerPointDocumentEditor::SlideBuilder::Build() const
{
    return m_editor ? m_editor->AddSlide(*this) : nullptr;
}

PowerPointDocumentEditor::PowerPointDocumentEditor(const PowerPointDocument::Ptr& document) : m_document(document)
{
}

PowerPointDocumentEditor::~PowerPointDocumentEditor()
{
    if (m_transactionOwner)
    {
        m_transactionOwner->Invalidate(this);
    }
}

PowerPointDocumentEditor::Ptr PowerPointDocumentEditor::Create(const PowerPointDocument::Ptr& document)
{
    return std::make_shared<PowerPointDocumentEditor>(document);
}

PowerPointDocumentEditor::Ptr PowerPointDocumentEditor::CreateNew(PowerPointDocumentType type)
{
    auto document = PowerPointDocument::Create(type);
    if (!document || !document->InitDocument())
    {
        return nullptr;
    }
    return Create(document);
}

PowerPointDocumentEditor::Ptr PowerPointDocumentEditor::Open(const std::filesystem::path& path,
                                                             const Packaging::OpenSettings& settings,
                                                             const ICancellationToken* token)
{
    auto document = PowerPointDocument::Open(path, settings, token);
    return document ? Create(document) : nullptr;
}

PowerPointDocumentEditor::Ptr PowerPointDocumentEditor::Open(const std::vector<Byte>& bytes,
                                                             const Packaging::OpenSettings& settings,
                                                             const ICancellationToken* token)
{
    auto document = PowerPointDocument::Open(bytes, settings, token);
    return document ? Create(document) : nullptr;
}

PowerPointDocumentEditor::Ptr PowerPointDocumentEditor::Open(std::span<const Byte> bytes,
                                                             const Packaging::OpenSettings& settings,
                                                             const ICancellationToken* token)
{
    auto document = PowerPointDocument::Open(bytes, settings, token);
    return document ? Create(document) : nullptr;
}

bool PowerPointDocumentEditor::SaveToFile(const std::filesystem::path& path, bool atomicSave,
                                          const ICancellationToken* token)
{
    return m_document && m_document->SaveToFile(path, atomicSave, token);
}

std::vector<Byte> PowerPointDocumentEditor::SaveToMemory(const ICancellationToken* token)
{
    return m_document ? m_document->SaveToMemory(token) : std::vector<Byte>{};
}

std::optional<DocumentEditMemento> PowerPointDocumentEditor::CreateMemento(
    const ICancellationToken* cancellationToken)
{
    if (!m_document)
    {
        return std::nullopt;
    }

    auto bytes = m_document->SaveToMemory(cancellationToken);
    if (bytes.empty())
    {
        return std::nullopt;
    }

    return DocumentEditMemento(DocumentFamily::PowerPoint, std::move(bytes));
}

bool PowerPointDocumentEditor::RestoreMemento(const DocumentEditMemento& memento,
                                              const ICancellationToken* cancellationToken)
{
    if (memento.Family() != DocumentFamily::PowerPoint || memento.Bytes().empty())
    {
        return false;
    }

    auto document = PowerPointDocument::Open(memento.Bytes(), {}, cancellationToken);
    if (!document)
    {
        return false;
    }

    m_document = std::move(document);
    return true;
}

DocumentEditTransaction PowerPointDocumentEditor::BeginTransaction(const ICancellationToken* cancellationToken)
{
    return detail::DocumentEditTransactionStarter::Begin(
        m_transactionOwner,
        this,
        [this, cancellationToken]
        { return CreateMemento(cancellationToken); },
        [this](const DocumentEditMemento& value)
        { return RestoreMemento(value); });
}

void PowerPointDocumentEditor::SetDocument(const PowerPointDocument::Ptr& document)
{
    m_document = document;
}
PowerPointDocument::Ptr PowerPointDocumentEditor::GetDocument() const
{
    return m_document;
}
PowerPointDocument::Ptr PowerPointDocumentEditor::GetLowLevelApi() const
{
    return m_document;
}

Packaging::DocumentProperties PowerPointDocumentEditor::Properties() const
{
    return Packaging::DocumentProperties(*m_document);
}

PowerPointDocumentEditor::SlideBuilder PowerPointDocumentEditor::CreateSlideBuilder()
{
    return SlideBuilder(this);
}

PresentationSlide::Ptr PowerPointDocumentEditor::AddSlide()
{
    auto presentationPart = m_document ? m_document->GetPresentationPart() : nullptr;
    auto list = SlideIds(m_document, true);
    if (!presentationPart || !list)
    {
        return nullptr;
    }

    auto part = presentationPart->AddSlidePart();
    if (!part)
    {
        return nullptr;
    }
    if (!PresentationDomBuilders::InitializeSlide(part->GetTypedRootElement()))
    {
        presentationPart->RemoveSlidePart(part);
        return nullptr;
    }
    auto entry = list->AppendChild<Presentation::SlideId>();
    if (!entry)
    {
        presentationPart->RemoveSlidePart(part);
        return nullptr;
    }
    entry->SetId(UInt32Value(NextSlideId(list)));
    entry->SetRelationshipId(StringValue(SlideRelationshipId(presentationPart, part)));
    return PresentationSlide::Ptr(new PresentationSlide(part, entry));
}

PresentationSlide::Ptr PowerPointDocumentEditor::AddSlide(const SlideBuilder& builder)
{
    if (builder.m_layout &&
        (!builder.m_layout->GetPart() || builder.m_layout->GetPart()->Package() != m_document.get()))
    {
        return nullptr;
    }

    const auto index = SlideCount();
    auto slide = AddSlide();
    if (!slide)
    {
        return nullptr;
    }

    const auto rollback = [&]()
    {
        RemoveSlide(index);
        return PresentationSlide::Ptr{};
    };
    if ((builder.m_layout && !SetSlideLayout(index, builder.m_layout)) ||
        (builder.m_hidden && !slide->SetHidden(true)))
    {
        return rollback();
    }

    auto tree = slide->ShapeTree();
    if (!tree)
    {
        return rollback();
    }
    const auto addTextBox = [&](const SlideBuilder::TextBoxInstruction& instruction)
    {
        auto shape = tree->AddShape(instruction.Name);
        if (!shape || !shape->SetTransform(instruction.Transform))
        {
            return false;
        }
        return shape->SetTextFrame(instruction.Frame);
    };

    if (builder.m_title && !addTextBox(*builder.m_title))
    {
        return rollback();
    }
    for (const auto& textBox : builder.m_textBoxes)
    {
        if (!addTextBox(textBox))
        {
            return rollback();
        }
    }
    for (const auto& instruction : builder.m_shapes)
    {
        auto shape = tree->AddShape(instruction.Name);
        if (!shape || !shape->SetTransform(instruction.Transform) ||
            !shape->SetPresetGeometry(instruction.Type))
        {
            return rollback();
        }
    }
    return slide;
}

PresentationSlide::Ptr PowerPointDocumentEditor::CopySlide(Size index)
{
    auto presentationPart = m_document ? m_document->GetPresentationPart() : nullptr;
    auto list = SlideIds(m_document, false);
    auto entries = list ? list->Elements<Presentation::SlideId>() : std::vector<Presentation::SlideId::Ptr>{};
    if (!presentationPart || !list || index >= entries.size())
    {
        return nullptr;
    }
    auto source = PartForRelationship(m_document, entries[index]->GetRelationshipId().ToString());
    if (!source)
    {
        return nullptr;
    }

    static const std::vector<std::string_view> sharedRelationships = {
        "http://schemas.openxmlformats.org/officeDocument/2006/relationships/"
        "image",
        "http://schemas.openxmlformats.org/officeDocument/2006/relationships/"
        "audio",
        "http://schemas.openxmlformats.org/officeDocument/2006/relationships/"
        "video",
        "http://schemas.microsoft.com/office/2007/relationships/media",
        "http://schemas.openxmlformats.org/officeDocument/2006/relationships/"
        "slideLayout",
        "http://schemas.openxmlformats.org/officeDocument/2006/relationships/"
        "slideMaster",
        "http://schemas.openxmlformats.org/officeDocument/2006/relationships/"
        "notesMaster",
        "http://schemas.openxmlformats.org/officeDocument/2006/relationships/"
        "theme"};
    auto copiedBase = presentationPart->ClonePartGraph(source, sharedRelationships);
    auto copiedPart = std::dynamic_pointer_cast<Packaging::SlidePart>(copiedBase);
    if (!copiedPart)
    {
        return nullptr;
    }

    auto entry = list->InsertChild<Presentation::SlideId>(index + 1 < entries.size() ? entries[index + 1] : nullptr);
    if (!entry)
    {
        presentationPart->RemoveSlidePart(copiedPart);
        return nullptr;
    }
    entry->SetId(UInt32Value(NextSlideId(list)));
    entry->SetRelationshipId(StringValue(SlideRelationshipId(presentationPart, copiedPart)));
    return PresentationSlide::Ptr(new PresentationSlide(copiedPart, entry));
}

PresentationSlide::Ptr PowerPointDocumentEditor::CopySlideFrom(const PowerPointDocumentEditor& sourceEditor,
                                                               Size index)
{
    auto presentationPart = m_document ? m_document->GetPresentationPart() : nullptr;
    auto sourceDocument = sourceEditor.GetDocument();
    auto sourceSlide = sourceEditor.GetSlide(index);
    auto sourcePart = sourceSlide ? sourceSlide->GetPart() : nullptr;
    auto slideList = SlideIds(m_document, true);
    if (!presentationPart || !sourceDocument || sourceDocument == m_document || !sourcePart || !slideList)
    {
        return nullptr;
    }

    auto importedBase = presentationPart->ImportPartGraph(sourcePart);
    auto importedSlide = std::dynamic_pointer_cast<Packaging::SlidePart>(importedBase);
    if (!importedSlide)
    {
        return nullptr;
    }

    std::shared_ptr<Packaging::SlideMasterPart> importedMaster;
    std::shared_ptr<Presentation::SlideMasterIdList> masterList;
    std::shared_ptr<Presentation::SlideMasterId> masterEntry;
    if (auto importedLayout = importedSlide->GetSlideLayoutPart())
    {
        importedMaster = importedLayout->GetSlideMasterPart();
    }
    if (importedMaster)
    {
        auto presentation = presentationPart->GetTypedRootElement();
        masterList = presentation ? presentation->GetFirstChildOfType<Presentation::SlideMasterIdList>() : nullptr;
        if (!masterList && presentation)
        {
            masterList = presentation->InsertChild<Presentation::SlideMasterIdList>(slideList);
        }
        const auto masterRelationshipId =
            masterList ? presentationPart->AddPartReference(importedMaster, SlideMasterRelationship) : std::string{};
        const auto masterId = PresentationIdAllocator::NextSlideMasterId(masterList);
        masterEntry = !masterRelationshipId.empty() && masterId
                          ? masterList->AppendChild<Presentation::SlideMasterId>()
                          : nullptr;
        if (!masterEntry || !masterId)
        {
            if (!masterRelationshipId.empty())
            {
                presentationPart->RemovePartReference(importedMaster);
            }
            presentationPart->RemoveSlidePart(importedSlide);
            return nullptr;
        }
        masterEntry->SetId(UInt32Value(*masterId));
        masterEntry->SetRelationshipId(StringValue(masterRelationshipId));

        std::unordered_set<UInt32> usedLayoutIds;
        for (const auto& master : SlideMasters())
        {
            if (master->GetPart() == importedMaster)
            {
                continue;
            }
            for (const auto& layout : master->Layouts())
            {
                usedLayoutIds.insert(layout->Id());
            }
        }
        auto masterRoot = importedMaster->GetTypedRootElement();
        auto importedLayoutList =
            masterRoot ? masterRoot->GetFirstChildOfType<Presentation::SlideLayoutIdList>() : nullptr;
        UInt32 nextLayoutId = PresentationIdAllocator::MinimumSlideLayoutId;
        if (importedLayoutList)
        {
            for (const auto& layoutEntry : importedLayoutList->Elements<Presentation::SlideLayoutId>())
            {
                while (usedLayoutIds.contains(nextLayoutId))
                {
                    ++nextLayoutId;
                }
                layoutEntry->SetId(UInt32Value(nextLayoutId));
                usedLayoutIds.insert(nextLayoutId++);
            }
        }
    }

    auto slideEntry = slideList->AppendChild<Presentation::SlideId>();
    if (!slideEntry)
    {
        if (masterEntry)
        {
            masterList->RemoveChild(masterEntry);
        }
        if (importedMaster)
        {
            presentationPart->RemovePartReference(importedMaster);
        }
        presentationPart->RemoveSlidePart(importedSlide);
        return nullptr;
    }
    slideEntry->SetId(UInt32Value(NextSlideId(slideList)));
    slideEntry->SetRelationshipId(StringValue(SlideRelationshipId(presentationPart, importedSlide)));
    return PresentationSlide::Ptr(new PresentationSlide(importedSlide, slideEntry));
}

PresentationSlide::Ptr PowerPointDocumentEditor::GetSlide(Size index) const
{
    auto list = SlideIds(m_document, false);
    if (!list)
    {
        return nullptr;
    }
    auto entries = list->Elements<Presentation::SlideId>();
    if (index >= entries.size())
    {
        return nullptr;
    }
    auto part = PartForRelationship(m_document, entries[index]->GetRelationshipId().ToString());
    return part ? PresentationSlide::Ptr(new PresentationSlide(part, entries[index])) : nullptr;
}

std::vector<PresentationSlide::Ptr> PowerPointDocumentEditor::Slides() const
{
    std::vector<PresentationSlide::Ptr> result;
    auto list = SlideIds(m_document, false);
    if (!list)
    {
        return result;
    }
    for (const auto& entry : list->Elements<Presentation::SlideId>())
    {
        auto part = PartForRelationship(m_document, entry->GetRelationshipId().ToString());
        if (part)
        {
            result.push_back(PresentationSlide::Ptr(new PresentationSlide(part, entry)));
        }
    }
    return result;
}

Size PowerPointDocumentEditor::SlideCount() const
{
    return Slides().size();
}

std::vector<PresentationSection> PowerPointDocumentEditor::Sections() const
{
    auto sections = PresentationCollectionHelpers::Sections(m_document);
    std::unordered_map<UInt32, Size> positions;
    const auto slides = Slides();
    for (Size index = 0; index < slides.size(); ++index)
    {
        positions[slides[index]->Id()] = index;
    }
    for (auto& section : sections)
    {
        std::stable_sort(section.SlideIds.begin(), section.SlideIds.end(), [&](auto left, auto right)
                         {
            const auto leftPosition = positions.find(left);
            const auto rightPosition = positions.find(right);
            const auto leftIndex = leftPosition == positions.end() ? positions.size() : leftPosition->second;
            const auto rightIndex = rightPosition == positions.end() ? positions.size() : rightPosition->second;
            return leftIndex < rightIndex; });
    }
    return sections;
}

bool PowerPointDocumentEditor::AddSection(const PresentationSection& value)
{
    auto sections = Sections();
    if (value.Id.empty() || value.Name.empty() || value.SlideIds.empty() ||
        std::any_of(sections.begin(), sections.end(), [&](const auto& section)
                    { return section.Id == value.Id; }))
    {
        return false;
    }
    std::unordered_set<UInt32> available;
    for (const auto& slide : Slides())
    {
        available.insert(slide->Id());
    }
    std::unordered_set<UInt32> assigned;
    for (const auto& section : sections)
    {
        assigned.insert(section.SlideIds.begin(), section.SlideIds.end());
    }
    for (const auto slideId : value.SlideIds)
    {
        if (!available.contains(slideId) || !assigned.insert(slideId).second)
        {
            return false;
        }
    }
    sections.push_back(value);
    return PresentationCollectionHelpers::WriteSections(m_document, sections);
}

bool PowerPointDocumentEditor::UpdateSection(std::string_view id, const PresentationSection& value)
{
    auto sections = Sections();
    const auto selected = std::find_if(sections.begin(), sections.end(),
                                       [&](const auto& section)
                                       { return section.Id == id; });
    if (selected == sections.end() || value.Id != id || value.Name.empty() || value.SlideIds.empty())
    {
        return false;
    }
    std::unordered_set<UInt32> available;
    for (const auto& slide : Slides())
    {
        available.insert(slide->Id());
    }
    std::unordered_set<UInt32> assigned;
    for (const auto& section : sections)
    {
        if (section.Id != id)
        {
            assigned.insert(section.SlideIds.begin(), section.SlideIds.end());
        }
    }
    for (const auto slideId : value.SlideIds)
    {
        if (!available.contains(slideId) || !assigned.insert(slideId).second)
        {
            return false;
        }
    }
    *selected = value;
    return PresentationCollectionHelpers::WriteSections(m_document, sections);
}

bool PowerPointDocumentEditor::RemoveSection(std::string_view id)
{
    auto sections = Sections();
    const auto oldSize = sections.size();
    std::erase_if(sections, [&](const auto& section)
                  { return section.Id == id; });
    return sections.size() != oldSize && PresentationCollectionHelpers::WriteSections(m_document, sections);
}

std::vector<PresentationCustomShow> PowerPointDocumentEditor::CustomShows() const
{
    std::unordered_map<std::string, UInt32> slideIds;
    for (const auto& slide : Slides())
    {
        slideIds[slide->RelationshipId()] = slide->Id();
    }
    return PresentationCollectionHelpers::CustomShows(m_document, slideIds);
}

bool PowerPointDocumentEditor::AddCustomShow(const PresentationCustomShow& value)
{
    auto shows = CustomShows();
    if (value.Id == 0 || value.Name.empty() || value.SlideIds.empty() ||
        std::any_of(shows.begin(), shows.end(), [&](const auto& show)
                    { return show.Id == value.Id || show.Name == value.Name; }))
    {
        return false;
    }
    std::unordered_map<UInt32, std::string> relationships;
    for (const auto& slide : Slides())
    {
        relationships[slide->Id()] = slide->RelationshipId();
    }
    if (std::any_of(value.SlideIds.begin(), value.SlideIds.end(),
                    [&](auto slideId)
                    { return !relationships.contains(slideId); }))
    {
        return false;
    }
    shows.push_back(value);
    return PresentationCollectionHelpers::WriteCustomShows(m_document, shows, relationships);
}

bool PowerPointDocumentEditor::UpdateCustomShow(UInt32 id, const PresentationCustomShow& value)
{
    auto shows = CustomShows();
    const auto selected = std::find_if(shows.begin(), shows.end(), [&](const auto& show)
                                       { return show.Id == id; });
    if (selected == shows.end() || value.Id != id || value.Name.empty() || value.SlideIds.empty() ||
        std::any_of(shows.begin(), shows.end(), [&](const auto& show)
                    { return show.Id != id && show.Name == value.Name; }))
    {
        return false;
    }
    std::unordered_map<UInt32, std::string> relationships;
    for (const auto& slide : Slides())
    {
        relationships[slide->Id()] = slide->RelationshipId();
    }
    if (std::any_of(value.SlideIds.begin(), value.SlideIds.end(),
                    [&](auto slideId)
                    { return !relationships.contains(slideId); }))
    {
        return false;
    }
    *selected = value;
    return PresentationCollectionHelpers::WriteCustomShows(m_document, shows, relationships);
}

bool PowerPointDocumentEditor::RemoveCustomShow(UInt32 id)
{
    auto shows = CustomShows();
    const auto oldSize = shows.size();
    std::erase_if(shows, [&](const auto& show)
                  { return show.Id == id; });
    if (shows.size() == oldSize)
    {
        return false;
    }
    std::unordered_map<UInt32, std::string> relationships;
    for (const auto& slide : Slides())
    {
        relationships[slide->Id()] = slide->RelationshipId();
    }
    return PresentationCollectionHelpers::WriteCustomShows(m_document, shows, relationships);
}

bool PowerPointDocumentEditor::MoveSlide(Size fromIndex, Size toIndex)
{
    auto list = SlideIds(m_document, false);
    auto entries = list ? list->Elements<Presentation::SlideId>() : std::vector<Presentation::SlideId::Ptr>{};
    if (!list || fromIndex >= entries.size() || toIndex >= entries.size())
    {
        return false;
    }
    if (fromIndex == toIndex)
    {
        return true;
    }

    const auto source = entries[fromIndex];
    auto before = entries[toIndex];
    if (fromIndex < toIndex)
    {
        before = toIndex + 1 < entries.size() ? entries[toIndex + 1] : nullptr;
    }
    auto moved = list->InsertChild<Presentation::SlideId>(before);
    if (!moved)
    {
        return false;
    }
    moved->SetId(source->GetId());
    moved->SetRelationshipId(source->GetRelationshipId());
    if (!list->RemoveChild(source))
    {
        return false;
    }
    PresentationCollectionHelpers::WriteSections(m_document, Sections());
    std::unordered_map<UInt32, std::string> relationships;
    for (const auto& slide : Slides())
    {
        relationships[slide->Id()] = slide->RelationshipId();
    }
    PresentationCollectionHelpers::WriteCustomShows(m_document, CustomShows(), relationships);
    return true;
}

bool PowerPointDocumentEditor::ReorderSlides(const std::vector<Size>& newOrder)
{
    const auto original = Slides();
    if (newOrder.size() != original.size())
    {
        return false;
    }
    std::vector<bool> seen(original.size(), false);
    for (const auto index : newOrder)
    {
        if (index >= original.size() || seen[index])
        {
            return false;
        }
        seen[index] = true;
    }

    for (Size destination = 0; destination < newOrder.size(); ++destination)
    {
        const auto wantedId = original[newOrder[destination]]->Id();
        const auto current = Slides();
        const auto found = std::find_if(current.begin() + static_cast<PtrDiff>(destination), current.end(),
                                        [&](const auto& slide)
                                        { return slide->Id() == wantedId; });
        if (found == current.end())
        {
            return false;
        }
        const auto source = static_cast<Size>(std::distance(current.begin(), found));
        if (source != destination && !MoveSlide(source, destination))
        {
            return false;
        }
    }
    return true;
}

bool PowerPointDocumentEditor::RemoveSlide(Size index)
{
    auto presentationPart = m_document ? m_document->GetPresentationPart() : nullptr;
    auto list = SlideIds(m_document, false);
    auto entries = list ? list->Elements<Presentation::SlideId>() : std::vector<Presentation::SlideId::Ptr>{};
    if (!presentationPart || !list || index >= entries.size())
    {
        return false;
    }
    const auto removedSlideId = entries[index]->GetId().ValueOr(0);
    auto sections = Sections();
    auto shows = CustomShows();
    auto part = PartForRelationship(m_document, entries[index]->GetRelationshipId().ToString());
    if (!part || !list->RemoveChild(entries[index]))
    {
        return false;
    }
    if (!presentationPart->RemoveSlidePart(part))
    {
        return false;
    }
    for (auto& section : sections)
    {
        std::erase(section.SlideIds, removedSlideId);
    }
    std::erase_if(sections, [](const auto& section)
                  { return section.SlideIds.empty(); });
    for (auto& show : shows)
    {
        std::erase(show.SlideIds, removedSlideId);
    }
    std::erase_if(shows, [](const auto& show)
                  { return show.SlideIds.empty(); });
    PresentationCollectionHelpers::WriteSections(m_document, sections);
    std::unordered_map<UInt32, std::string> relationships;
    for (const auto& slide : Slides())
    {
        relationships[slide->Id()] = slide->RelationshipId();
    }
    PresentationCollectionHelpers::WriteCustomShows(m_document, shows, relationships);
    return true;
}

std::vector<PresentationCommentAuthor> PowerPointDocumentEditor::CommentAuthors() const
{
    std::vector<PresentationCommentAuthor> result;
    auto presentation = m_document ? m_document->GetPresentationPart() : nullptr;
    auto part = presentation ? presentation->GetauthorsPart() : nullptr;
    auto list = part ? part->GetTypedRootElement() : nullptr;
    if (list)
    {
        for (const auto& author : list->Elements<ModernComments::Author>())
        {
            result.push_back({author->GetId().ToString(), author->GetName().ToString(),
                              author->GetInitials().ToString(), author->GetUserId().ToString(),
                              author->GetProviderId().ToString()});
        }
    }
    return result;
}

bool PowerPointDocumentEditor::AddCommentAuthor(const PresentationCommentAuthor& value)
{
    const auto authors = CommentAuthors();
    if (value.Id.empty() ||
        std::any_of(authors.begin(), authors.end(), [&](const auto& author)
                    { return author.Id == value.Id; }))
    {
        return false;
    }
    auto presentation = m_document ? m_document->GetPresentationPart() : nullptr;
    auto part = presentation ? presentation->GetauthorsPart() : nullptr;
    if (!part && presentation)
    {
        part = presentation->AddPowerPointAuthorsPart();
    }
    auto list = part ? part->GetTypedRootElement() : nullptr;
    auto author = list ? list->AppendChild<ModernComments::Author>() : nullptr;
    if (!author)
    {
        return false;
    }
    author->SetId(StringValue(value.Id));
    author->SetName(StringValue(value.Name));
    author->SetInitials(StringValue(value.Initials));
    author->SetUserId(StringValue(value.UserId));
    author->SetProviderId(StringValue(value.ProviderId));
    return true;
}

bool PowerPointDocumentEditor::UpdateCommentAuthor(std::string_view id, const PresentationCommentAuthor& value)
{
    if (id.empty() || value.Id != id)
    {
        return false;
    }
    auto presentation = m_document ? m_document->GetPresentationPart() : nullptr;
    auto part = presentation ? presentation->GetauthorsPart() : nullptr;
    auto list = part ? part->GetTypedRootElement() : nullptr;
    if (list)
    {
        for (const auto& author : list->Elements<ModernComments::Author>())
        {
            if (author->GetId().ToString() == id)
            {
                author->SetName(StringValue(value.Name));
                author->SetInitials(StringValue(value.Initials));
                author->SetUserId(StringValue(value.UserId));
                author->SetProviderId(StringValue(value.ProviderId));
                return true;
            }
        }
    }
    return false;
}

bool PowerPointDocumentEditor::RemoveCommentAuthor(std::string_view id)
{
    if (id.empty())
    {
        return false;
    }
    for (const auto& slide : Slides())
    {
        for (const auto& comment : slide->Comments())
        {
            if (comment.AuthorId == id)
            {
                return false;
            }
            if (std::any_of(comment.Replies.begin(), comment.Replies.end(),
                            [&](const auto& reply)
                            { return reply.AuthorId == id; }))
            {
                return false;
            }
        }
    }
    auto presentation = m_document ? m_document->GetPresentationPart() : nullptr;
    auto part = presentation ? presentation->GetauthorsPart() : nullptr;
    auto list = part ? part->GetTypedRootElement() : nullptr;
    if (list)
    {
        for (const auto& author : list->Elements<ModernComments::Author>())
        {
            if (author->GetId().ToString() == id)
            {
                list->RemoveChild(author);
                if (list->Elements<ModernComments::Author>().empty())
                {
                    presentation->RemoveauthorsPart();
                }
                return true;
            }
        }
    }
    return false;
}

std::optional<PresentationSlideSize> PowerPointDocumentEditor::GetSlideSize() const
{
    auto presentationPart = m_document ? m_document->GetPresentationPart() : nullptr;
    auto presentation = presentationPart ? presentationPart->GetTypedRootElement() : nullptr;
    auto size = presentation ? presentation->GetFirstChildOfType<Presentation::SlideSize>() : nullptr;
    if (!size)
    {
        return std::nullopt;
    }
    PresentationSlideSize result;
    result.Size = PresentationSize(static_cast<Int64>(size->GetCx().ValueOr(0)),
                                   static_cast<Int64>(size->GetCy().ValueOr(0)));
    if (const auto type = size->GetType(); type.IsDefined() && type.Value().IsValid())
    {
        result.Type = type.Value().GetValue();
    }
    return result;
}

bool PowerPointDocumentEditor::SetSlideSize(const PresentationSlideSize& size)
{
    // ST_SlideSizeCoordinate accepts one inch up to 56 inches on either axis.
    constexpr Int32 minimumExtent = 914400;
    constexpr Int32 maximumExtent = 51206400;
    const auto width = PresentationMeasurementHelpers::ToInt32Emu(size.Size.Width);
    const auto height = PresentationMeasurementHelpers::ToInt32Emu(size.Size.Height);
    if (!width || !height || *width < minimumExtent || *width > maximumExtent || *height < minimumExtent ||
        *height > maximumExtent)
    {
        return false;
    }
    auto presentationPart = m_document ? m_document->GetPresentationPart() : nullptr;
    auto presentation = presentationPart ? presentationPart->GetTypedRootElement() : nullptr;
    if (!presentation)
    {
        return false;
    }
    auto element = presentation->GetFirstChildOfType<Presentation::SlideSize>();
    if (!element)
    {
        element = presentation->AppendChild<Presentation::SlideSize>();
    }
    if (!element)
    {
        return false;
    }
    element->SetCx(Int32Value(*width));
    element->SetCy(Int32Value(*height));
    if (size.Type)
    {
        element->SetType(EnumValue<Presentation::SlideSizeValues>(Presentation::SlideSizeValues(*size.Type)));
    }
    else
    {
        element->SetType(EnumValue<Presentation::SlideSizeValues>());
    }
    return true;
}

bool PowerPointDocumentEditor::RemoveSlideSize()
{
    auto presentationPart = m_document ? m_document->GetPresentationPart() : nullptr;
    auto presentation = presentationPart ? presentationPart->GetTypedRootElement() : nullptr;
    auto size = presentation ? presentation->GetFirstChildOfType<Presentation::SlideSize>() : nullptr;
    if (!size)
    {
        return false;
    }
    return presentation->RemoveChild(size);
}

std::optional<PresentationHandoutSettings> PowerPointDocumentEditor::HandoutSettings() const
{
    auto presentationPart = m_document ? m_document->GetPresentationPart() : nullptr;
    auto part = presentationPart ? presentationPart->GetHandoutMasterPart() : nullptr;
    auto root = part ? part->GetTypedRootElement() : nullptr;
    auto presentation = presentationPart ? presentationPart->GetTypedRootElement() : nullptr;
    if (!root || !presentation)
    {
        return std::nullopt;
    }
    PresentationHandoutSettings settings;
    if (auto size = presentation->GetFirstChildOfType<Presentation::NotesSize>())
    {
        settings.PageSize = PresentationSize(size->GetCx().ValueOr(0), size->GetCy().ValueOr(0));
    }
    if (auto headerFooter = root->GetFirstChildOfType<Presentation::HeaderFooter>())
    {
        settings.ShowHeader = headerFooter->GetHeader().ValueOr(true);
        settings.ShowFooter = headerFooter->GetFooter().ValueOr(true);
        settings.ShowDateTime = headerFooter->GetDateTime().ValueOr(true);
        settings.ShowSlideNumber = headerFooter->GetSlideNumber().ValueOr(true);
    }
    return settings;
}

bool PowerPointDocumentEditor::SetHandoutSettings(const PresentationHandoutSettings& settings)
{
    const auto width = PresentationMeasurementHelpers::ToInt64Emu(settings.PageSize.Width);
    const auto height = PresentationMeasurementHelpers::ToInt64Emu(settings.PageSize.Height);
    auto presentationPart = m_document ? m_document->GetPresentationPart() : nullptr;
    auto presentation = presentationPart ? presentationPart->GetTypedRootElement() : nullptr;
    if (!width || !height || *width <= 0 || *height <= 0 || !presentationPart || !presentation)
    {
        return false;
    }
    auto part = presentationPart->GetHandoutMasterPart();
    if (!part)
    {
        part = presentationPart->AddHandoutMasterPart();
    }
    if (!part)
    {
        return false;
    }
    auto root = part->GetTypedRootElement();
    if (!PresentationDomBuilders::InitializeHandoutMaster(root))
    {
        return false;
    }
    auto headerFooter = root ? root->GetFirstChildOfType<Presentation::HeaderFooter>() : nullptr;
    if (!headerFooter)
    {
        return false;
    }
    headerFooter->SetHeader(BooleanValue(settings.ShowHeader));
    headerFooter->SetFooter(BooleanValue(settings.ShowFooter));
    headerFooter->SetDateTime(BooleanValue(settings.ShowDateTime));
    headerFooter->SetSlideNumber(BooleanValue(settings.ShowSlideNumber));

    auto size = presentation->GetFirstChildOfType<Presentation::NotesSize>();
    if (!size)
    {
        size = presentation->AppendChild<Presentation::NotesSize>();
    }
    if (!size)
    {
        return false;
    }
    size->SetCx(Int64Value(*width));
    size->SetCy(Int64Value(*height));

    auto list = presentation->GetFirstChildOfType<Presentation::HandoutMasterIdList>();
    if (!list)
    {
        list = presentation->InsertChild<Presentation::HandoutMasterIdList>(
            presentation->GetFirstChildOfType<Presentation::SlideIdList>());
    }
    if (!list)
    {
        return false;
    }
    auto entries = list->Elements<Presentation::HandoutMasterId>();
    if (entries.empty())
    {
        auto entry = list->AppendChild<Presentation::HandoutMasterId>();
        if (!entry)
        {
            return false;
        }
        entry->SetId(StringValue(part->RelationshipId()));
    }
    return true;
}

bool PowerPointDocumentEditor::RemoveHandoutSettings()
{
    auto presentationPart = m_document ? m_document->GetPresentationPart() : nullptr;
    auto presentation = presentationPart ? presentationPart->GetTypedRootElement() : nullptr;
    auto list = presentation ? presentation->GetFirstChildOfType<Presentation::HandoutMasterIdList>() : nullptr;
    if (!presentationPart || !presentationPart->GetHandoutMasterPart())
    {
        return false;
    }
    if (list)
    {
        presentation->RemoveChild(list);
    }
    return presentationPart->RemoveHandoutMasterPart();
}

PresentationSlideMaster::Ptr PowerPointDocumentEditor::AddSlideMaster(std::string name)
{
    auto presentationPart = m_document ? m_document->GetPresentationPart() : nullptr;
    auto presentation = presentationPart ? presentationPart->GetTypedRootElement() : nullptr;
    if (!presentationPart || !presentation)
    {
        return nullptr;
    }
    auto list = presentation->GetFirstChildOfType<Presentation::SlideMasterIdList>();
    if (!list)
    {
        list = presentation->InsertChild<Presentation::SlideMasterIdList>(
            presentation->GetFirstChildOfType<Presentation::SlideIdList>());
    }
    if (!list)
    {
        return nullptr;
    }

    auto part = presentationPart->AddSlideMasterPart();
    if (!part)
    {
        return nullptr;
    }
    if (!PresentationDomBuilders::InitializeSlideMaster(part->GetTypedRootElement()))
    {
        presentationPart->RemoveSlideMasterPart(part);
        return nullptr;
    }
    auto common = part->GetTypedRootElement()->GetFirstChildOfType<Presentation::CommonSlideData>();
    if (common)
    {
        common->SetName(StringValue(std::move(name)));
    }
    // Every slide master needs its own theme; a master without one makes
    // PowerPoint treat the presentation as damaged. The default Office theme is
    // built through the typed DrawingML DOM and can be replaced afterwards with
    // SetThemeXml()/SetThemeSettings().
    if (!ThemeService::WriteDefaultTheme(part->AddThemePart()))
    {
        presentationPart->RemoveSlideMasterPart(part);
        return nullptr;
    }
    const auto id = PresentationIdAllocator::NextSlideMasterId(list);
    if (!id)
    {
        presentationPart->RemoveSlideMasterPart(part);
        return nullptr;
    }
    auto entry = list->AppendChild<Presentation::SlideMasterId>();
    if (!entry)
    {
        presentationPart->RemoveSlideMasterPart(part);
        return nullptr;
    }
    entry->SetId(UInt32Value(*id));
    entry->SetRelationshipId(StringValue(part->RelationshipId()));
    return PresentationSlideMaster::Ptr(new PresentationSlideMaster(part, entry));
}

PresentationSlideMaster::Ptr PowerPointDocumentEditor::ImportSlideMaster(
    const PresentationSlideMaster::Ptr& source)
{
    auto presentationPart = m_document ? m_document->GetPresentationPart() : nullptr;
    auto presentation = presentationPart ? presentationPart->GetTypedRootElement() : nullptr;
    auto sourcePart = source ? source->GetPart() : nullptr;
    if (!presentationPart || !presentation || !sourcePart || !sourcePart->Package() ||
        !PresentationHierarchyHelpers::WrapMaster(sourcePart))
    {
        return nullptr;
    }
    auto list = presentation->GetFirstChildOfType<Presentation::SlideMasterIdList>();
    if (!list)
    {
        list = presentation->InsertChild<Presentation::SlideMasterIdList>(
            presentation->GetFirstChildOfType<Presentation::SlideIdList>());
    }
    if (!list)
    {
        return nullptr;
    }
    auto imported =
        std::dynamic_pointer_cast<Packaging::SlideMasterPart>(presentationPart->ImportPartGraph(sourcePart));
    if (!imported)
    {
        return nullptr;
    }
    const auto id = PresentationIdAllocator::NextSlideMasterId(list);
    if (!id)
    {
        presentationPart->RemoveSlideMasterPart(imported);
        return nullptr;
    }
    auto entry = list->AppendChild<Presentation::SlideMasterId>();
    if (!entry)
    {
        presentationPart->RemoveSlideMasterPart(imported);
        return nullptr;
    }
    std::string presentationRelationshipId;
    for (const auto& incoming : imported->IncomingRelationships())
    {
        if (incoming.SourceUri == presentationPart->Uri())
        {
            presentationRelationshipId = incoming.Id;
            break;
        }
    }
    if (presentationRelationshipId.empty())
    {
        list->RemoveChild(entry);
        presentationPart->RemoveSlideMasterPart(imported);
        return nullptr;
    }
    entry->SetId(UInt32Value(*id));
    entry->SetRelationshipId(StringValue(std::move(presentationRelationshipId)));
    return PresentationSlideMaster::Ptr(new PresentationSlideMaster(imported, entry));
}

PresentationSlideMaster::Ptr PowerPointDocumentEditor::GetSlideMaster(Size index) const
{
    auto masters = SlideMasters();
    return index < masters.size() ? masters[index] : nullptr;
}

std::vector<PresentationSlideMaster::Ptr> PowerPointDocumentEditor::SlideMasters() const
{
    std::vector<PresentationSlideMaster::Ptr> result;
    auto presentationPart = m_document ? m_document->GetPresentationPart() : nullptr;
    auto presentation = presentationPart ? presentationPart->GetTypedRootElement() : nullptr;
    auto list = presentation ? presentation->GetFirstChildOfType<Presentation::SlideMasterIdList>() : nullptr;
    if (!presentationPart || !list)
    {
        return result;
    }
    for (const auto& entry : list->Elements<Presentation::SlideMasterId>())
    {
        for (const auto& part : presentationPart->GetSlideMasterParts())
        {
            for (const auto& incoming : part->IncomingRelationships())
            {
                if (incoming.SourceUri == presentationPart->Uri() &&
                    incoming.Id == entry->GetRelationshipId().ToString())
                {
                    result.push_back(PresentationSlideMaster::Ptr(new PresentationSlideMaster(part, entry)));
                }
            }
        }
    }
    return result;
}

bool PowerPointDocumentEditor::RemoveSlideMaster(const PresentationSlideMaster::Ptr& master,
                                                 const PresentationSlideLayout::Ptr& replacementLayout)
{
    auto presentationPart = m_document ? m_document->GetPresentationPart() : nullptr;
    auto presentation = presentationPart ? presentationPart->GetTypedRootElement() : nullptr;
    auto list = presentation ? presentation->GetFirstChildOfType<Presentation::SlideMasterIdList>() : nullptr;
    auto masterPart = master ? master->GetPart() : nullptr;
    auto replacementPart = replacementLayout ? replacementLayout->GetPart() : nullptr;
    if (!presentationPart || !list || !masterPart || masterPart->Package() != m_document.get())
    {
        return false;
    }
    const auto registeredMasters = SlideMasters();
    if (std::none_of(registeredMasters.begin(), registeredMasters.end(),
                     [&](const auto& value)
                     { return value->GetPart() == masterPart; }))
    {
        return false;
    }
    const auto registeredLayouts = SlideLayouts();
    auto replacementMaster = replacementLayout ? replacementLayout->Master() : nullptr;
    if (replacementPart &&
        (replacementPart->Package() != m_document.get() || !replacementMaster ||
         replacementMaster->GetPart() == masterPart ||
         std::none_of(registeredLayouts.begin(), registeredLayouts.end(),
                      [&](const auto& value)
                      { return value->GetPart() == replacementPart; })))
    {
        return false;
    }

    std::vector<Size> affectedSlides;
    const auto slides = Slides();
    for (Size index = 0; index < slides.size(); ++index)
    {
        auto layout = slides[index]->Layout();
        if (layout && layout->Master()->GetPart() == masterPart)
        {
            affectedSlides.push_back(index);
        }
    }
    if (!affectedSlides.empty() && !replacementLayout)
    {
        return false;
    }
    std::vector<PresentationSlideLayout::Ptr> previousLayouts;
    for (const auto index : affectedSlides)
    {
        previousLayouts.push_back(slides[index]->Layout());
        if (!SetSlideLayout(index, replacementLayout))
        {
            for (Size rollback = 0; rollback < previousLayouts.size() - 1; ++rollback)
            {
                SetSlideLayout(affectedSlides[rollback], previousLayouts[rollback]);
            }
            return false;
        }
    }
    if (!list->RemoveChild(master->m_entry))
    {
        for (Size rollback = 0; rollback < previousLayouts.size(); ++rollback)
        {
            SetSlideLayout(affectedSlides[rollback], previousLayouts[rollback]);
        }
        return false;
    }
    return presentationPart->RemoveSlideMasterPart(masterPart);
}

PresentationSlideLayout::Ptr PowerPointDocumentEditor::AddSlideLayout(const PresentationSlideMaster::Ptr& master,
                                                                      std::string name,
                                                                      Presentation::SlideLayoutValues::Value type)
{
    auto masterPart = master ? master->GetPart() : nullptr;
    if (!m_document || !masterPart || masterPart->Package() != m_document.get())
    {
        return nullptr;
    }
    auto masterRoot = masterPart->GetTypedRootElement();
    auto list = masterRoot ? masterRoot->GetFirstChildOfType<Presentation::SlideLayoutIdList>() : nullptr;
    if (!list)
    {
        return nullptr;
    }
    auto part = masterPart->AddSlideLayoutPart();
    if (!part)
    {
        return nullptr;
    }
    const auto masterRelationshipId = part->RelationshipId();
    if (!PresentationDomBuilders::InitializeSlideLayout(part->GetTypedRootElement()))
    {
        masterPart->RemoveSlideLayoutPart(part);
        return nullptr;
    }
    auto layoutRoot = part->GetTypedRootElement();
    auto common = layoutRoot ? layoutRoot->GetFirstChildOfType<Presentation::CommonSlideData>() : nullptr;
    if (!layoutRoot || !common)
    {
        masterPart->RemoveSlideLayoutPart(part);
        return nullptr;
    }
    common->SetName(StringValue(std::move(name)));
    layoutRoot->SetMatchingName(common->GetName());
    layoutRoot->SetType(EnumValue<Presentation::SlideLayoutValues>(Presentation::SlideLayoutValues(type)));

    const auto id = PresentationIdAllocator::NextSlideLayoutId(list);
    auto entry = id ? list->AppendChild<Presentation::SlideLayoutId>() : nullptr;
    if (!entry)
    {
        masterPart->RemoveSlideLayoutPart(part);
        return nullptr;
    }
    entry->SetId(UInt32Value(*id));
    entry->SetRelationshipId(StringValue(masterRelationshipId));
    if (part->AddPartReference(masterPart, SlideMasterRelationship).empty())
    {
        list->RemoveChild(entry);
        masterPart->RemoveSlideLayoutPart(part);
        return nullptr;
    }
    return PresentationSlideLayout::Ptr(new PresentationSlideLayout(part, entry, master));
}

std::vector<PresentationSlideLayout::Ptr> PowerPointDocumentEditor::SlideLayouts() const
{
    std::vector<PresentationSlideLayout::Ptr> result;
    for (const auto& master : SlideMasters())
    {
        auto layouts = master->Layouts();
        result.insert(result.end(), layouts.begin(), layouts.end());
    }
    return result;
}

bool PowerPointDocumentEditor::RemoveSlideLayout(const PresentationSlideLayout::Ptr& layout,
                                                 const PresentationSlideLayout::Ptr& replacementLayout)
{
    auto layoutPart = layout ? layout->GetPart() : nullptr;
    auto master = layout ? layout->Master() : nullptr;
    auto masterPart = master ? master->GetPart() : nullptr;
    auto masterRoot = masterPart ? masterPart->GetTypedRootElement() : nullptr;
    auto list = masterRoot ? masterRoot->GetFirstChildOfType<Presentation::SlideLayoutIdList>() : nullptr;
    auto replacementPart = replacementLayout ? replacementLayout->GetPart() : nullptr;
    if (!m_document || !layoutPart || !masterPart || !list || layoutPart->Package() != m_document.get())
    {
        return false;
    }
    const auto registeredLayouts = SlideLayouts();
    if (std::none_of(registeredLayouts.begin(), registeredLayouts.end(),
                     [&](const auto& value)
                     { return value->GetPart() == layoutPart; }))
    {
        return false;
    }
    if (replacementPart &&
        (replacementPart == layoutPart || replacementPart->Package() != m_document.get() ||
         std::none_of(registeredLayouts.begin(), registeredLayouts.end(),
                      [&](const auto& value)
                      { return value->GetPart() == replacementPart; })))
    {
        return false;
    }

    std::vector<Size> affectedSlides;
    const auto slides = Slides();
    for (Size index = 0; index < slides.size(); ++index)
    {
        auto current = slides[index]->Layout();
        if (current && current->GetPart() == layoutPart)
        {
            affectedSlides.push_back(index);
        }
    }
    if (!affectedSlides.empty() && !replacementLayout)
    {
        return false;
    }
    for (Size changed = 0; changed < affectedSlides.size(); ++changed)
    {
        if (!SetSlideLayout(affectedSlides[changed], replacementLayout))
        {
            for (Size rollback = 0; rollback < changed; ++rollback)
            {
                SetSlideLayout(affectedSlides[rollback], layout);
            }
            return false;
        }
    }
    if (!list->RemoveChild(layout->m_entry))
    {
        for (const auto index : affectedSlides)
        {
            SetSlideLayout(index, layout);
        }
        return false;
    }
    return masterPart->RemoveSlideLayoutPart(layoutPart);
}

bool PowerPointDocumentEditor::SetSlideLayout(Size slideIndex, const PresentationSlideLayout::Ptr& layout)
{
    auto slide = GetSlide(slideIndex);
    auto slidePart = slide ? slide->GetPart() : nullptr;
    auto layoutPart = layout ? layout->GetPart() : nullptr;
    if (!m_document || !slidePart || !layoutPart || layoutPart->Package() != m_document.get())
    {
        return false;
    }
    auto oldLayout = slidePart->GetSlideLayoutPart();
    if (oldLayout == layoutPart)
    {
        return true;
    }
    if (oldLayout && !slidePart->RemovePartReference(oldLayout))
    {
        return false;
    }
    if (slidePart->AddPartReference(layoutPart, SlideLayoutRelationship).empty())
    {
        if (oldLayout)
        {
            slidePart->AddPartReference(oldLayout, SlideLayoutRelationship);
        }
        return false;
    }
    return true;
}
} // namespace ExyokiOffice::PowerPoint
