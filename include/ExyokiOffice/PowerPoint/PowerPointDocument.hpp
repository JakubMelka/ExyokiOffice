// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/Drawing.Enums.hpp"
#include "ExyokiOffice/DOM/DocumentFormat/OpenXml/Presentation.Enums.hpp"
#include "ExyokiOffice/Color.hpp"
#include "ExyokiOffice/DocumentEditTransaction.hpp"
#include "ExyokiOffice/ImageFormat.hpp"
#include "ExyokiOffice/MeasuringAngle.hpp"
#include "ExyokiOffice/MeasuringUnits.hpp"
#include "ExyokiOffice/Packaging/PowerPointDocument.hpp"
#include "ExyokiOffice/Security/ExternalResources.hpp"
#include "ExyokiOffice/ThemeService.hpp"
#include "ExyokiOffice/StandardTypes.hpp"

#include <array>
#include <optional>

// Declared rather than included. Every use below is behind a shared_ptr, so an
// incomplete type is enough, and pulling in Presentation.hpp for it would put
// 213 KB of generated declarations - the whole PresentationML DOM - into every
// translation unit that only wants to add a slide. The enums are values and do
// have to be complete, which is what the .Enums.hpp above is for. The
// Wordprocessing header is written the same way.
namespace ExyokiOffice::DocumentFormat::OpenXml::Presentation
{
class PlaceholderShape;
class Shape;
class SlideId;
class SlideLayoutId;
class SlideMasterId;
} // namespace ExyokiOffice::DocumentFormat::OpenXml::Presentation

namespace ExyokiOffice::PowerPoint
{

using PowerPointDocument = ExyokiOffice::Packaging::PowerPointDocument;
using PowerPointDocumentType = ExyokiOffice::Packaging::PowerPointDocumentType;

class PresentationSlide;
class PresentationShape;
class PresentationShapeTree;
class PresentationSlideLayout;
class PresentationSlideMaster;
class PowerPointDocumentEditor;

/** @brief A two-dimensional physical coordinate whose axes may use any supported measurement unit. */
struct PresentationPoint
{
    MeasuringUnits X{};
    MeasuringUnits Y{};
    PresentationPoint() = default;
    PresentationPoint(MeasuringUnits x, MeasuringUnits y) : X(std::move(x)), Y(std::move(y)) {}
    /** @brief Convenience constructor for native EMU coordinates. */
    PresentationPoint(Int64 xEmu, Int64 yEmu)
        : X(static_cast<Real>(xEmu), MeasurementUnit::Emu),
          Y(static_cast<Real>(yEmu), MeasurementUnit::Emu) {}
    bool operator==(const PresentationPoint&) const = default;
};

/** @brief A non-negative physical extent whose dimensions may use any supported measurement unit. */
struct PresentationSize
{
    MeasuringUnits Width{};
    MeasuringUnits Height{};
    PresentationSize() = default;
    PresentationSize(MeasuringUnits width, MeasuringUnits height)
        : Width(std::move(width)), Height(std::move(height)) {}
    /** @brief Convenience constructor for native EMU dimensions. */
    PresentationSize(Int64 widthEmu, Int64 heightEmu)
        : Width(static_cast<Real>(widthEmu), MeasurementUnit::Emu),
          Height(static_cast<Real>(heightEmu), MeasurementUnit::Emu) {}
    bool operator==(const PresentationSize&) const = default;
};

/**
 * @brief Lossless PresentationML shape transform data.
 *
 * Position and size accept EMU, points, twips, inches, centimeters, or millimeters
 * and are rounded to integral EMU when serialized. Rotation uses the native OOXML
 * angle unit (1/60000 of a degree), so no floating-point conversion occurs.
 * GroupChildPosition and GroupChildSize describe the group child coordinate
 * system (`a:chOff` and `a:chExt`) and must either both be set or both omitted.
 */
struct PresentationShapeTransform
{
    PresentationPoint Position{};
    PresentationSize Size{};
    Int32 Rotation = 0;
    bool FlipHorizontal = false;
    bool FlipVertical = false;
    std::optional<PresentationPoint> GroupChildPosition;
    std::optional<PresentationSize> GroupChildSize;
    bool operator==(const PresentationShapeTransform&) const = default;
};

/** @brief Percentage-based crop offsets in DrawingML units (1/1000 of one percent). */
struct PresentationPictureCrop
{
    Int32 Left = 0;   ///< Amount cropped from the left edge.
    Int32 Top = 0;    ///< Amount cropped from the top edge.
    Int32 Right = 0;  ///< Amount cropped from the right edge.
    Int32 Bottom = 0; ///< Amount cropped from the bottom edge.
    bool operator==(const PresentationPictureCrop&) const = default;
};

/** @brief Binary payload and media type of an image embedded in the presentation package. */
struct PresentationEmbeddedPicture
{
    std::vector<Byte> Data;  ///< Unmodified image bytes.
    std::string ContentType; ///< IANA media type, for example `image/png`.
    bool operator==(const PresentationEmbeddedPicture&) const = default;
};

/**
 * @brief Complete editable picture payload and presentation metadata.
 *
 * Exactly one of Embedded and LinkedUri must be set. Linked resources are
 * represented only by their URI; this API never accesses the URI or performs
 * network I/O. A caller may explicitly resolve it and replace LinkedUri with
 * an Embedded payload.
 */
struct PresentationPictureData
{
    std::optional<PresentationEmbeddedPicture> Embedded; ///< Package-resident image payload.
    std::optional<std::string> LinkedUri;                ///< External image URI, retained without resolving it.
    PresentationPictureCrop Crop{};                      ///< Source rectangle crop offsets.
    std::string Name;                                    ///< Non-visual object name; an automatic name is used when empty.
    std::string AltText;                                 ///< Accessibility description stored in `a:cNvPr/@descr`.
    std::string Title;                                   ///< Optional accessibility title stored in `a:cNvPr/@title`.
    std::optional<std::string> Hyperlink;                ///< Optional external click target.
    std::string HyperlinkTooltip;                        ///< Optional click-target tooltip.
    PresentationShapeTransform Transform{};              ///< Position, extent, rotation, and flip metadata.
    bool operator==(const PresentationPictureData&) const = default;
};

/** Identifies the kind of time-based media hosted by a presentation picture shape. */
enum class PresentationMediaKind
{
    Audio, ///< An audio stream represented by `a:audioFile`.
    Video  ///< A video stream represented by `a:videoFile`.
};

/** @brief Binary bytes and MIME type of package-resident audio or video. */
struct PresentationEmbeddedMedia
{
    std::vector<Byte> Data;  ///< Exact media bytes; the library does not decode or transcode them.
    std::string ContentType; ///< IANA media type, for example `audio/mpeg` or `video/mp4`.
    bool operator==(const PresentationEmbeddedMedia&) const = default;
};

/** @brief Playback properties stored in the slide's PresentationML timing tree. */
struct PresentationMediaPlayback
{
    Int32 Volume = 100000;       ///< Volume in OOXML thousandths of one percent, from 0 through 100000.
    bool Muted = false;          ///< Starts playback without audible output when true.
    bool ShowWhenStopped = true; ///< Keeps the media shape visible while playback is stopped.
    bool Loop = false;           ///< Repeats the media indefinitely when true.
    bool FullScreen = false;     ///< Requests full-screen playback; meaningful only for video.
    bool operator==(const PresentationMediaPlayback&) const = default;
};

/**
 * @brief Editable audio or video source, poster frame, and playback metadata.
 *
 * Exactly one of Embedded and LinkedUri must be set. Linked URIs are retained
 * as external OPC relationships and are never opened by the library. PosterFrame,
 * when present, is stored as an independent image relationship on the media shape.
 * Saving preserves media bytes verbatim; ExyokiOffice does not play, inspect,
 * download, or transcode media streams.
 */
struct PresentationMediaData
{
    PresentationMediaKind Kind = PresentationMediaKind::Video;
    std::optional<PresentationEmbeddedMedia> Embedded;      ///< Package-resident opaque media payload.
    std::optional<std::string> LinkedUri;                   ///< Unresolved external media URI.
    std::optional<PresentationEmbeddedPicture> PosterFrame; ///< Optional package-resident preview image.
    PresentationMediaPlayback Playback{};                   ///< Playback behavior represented by a media timing node.
    std::string Name;                                       ///< Non-visual object name; an automatic name is used when empty.
    std::string AltText;                                    ///< Accessibility description for the media shape.
    PresentationShapeTransform Transform{};                 ///< Position, extent, rotation, and flip metadata.
    bool operator==(const PresentationMediaData&) const = default;
};

/**
 * Identifies a transition effect supported by the high-level presentation API.
 *
 * Every value except @ref PresentationTransitionKind::Unsupported maps to one
 * concrete PresentationML child element of `p:transition`. Effects differ in
 * which members of @ref PresentationTransitionOptions they accept; the accepted
 * set is documented per value and enforced by
 * @ref PresentationSlide::SetTransition.
 */
enum class PresentationTransitionKind
{
    Blinds,     ///< `p:blinds`; accepts Orientation.
    Checker,    ///< `p:checker`; accepts Orientation.
    Circle,     ///< `p:circle`; accepts no options.
    Comb,       ///< `p:comb`; accepts Orientation.
    Cover,      ///< `p:cover`; accepts any of the eight Direction values.
    Cut,        ///< `p:cut`; accepts ThroughBlack.
    Diamond,    ///< `p:diamond`; accepts no options.
    Dissolve,   ///< `p:dissolve`; accepts no options.
    Fade,       ///< `p:fade`; accepts ThroughBlack.
    Newsflash,  ///< `p:newsflash`; accepts no options.
    Plus,       ///< `p:plus`; accepts no options.
    Pull,       ///< `p:pull`; accepts any of the eight Direction values.
    Push,       ///< `p:push`; accepts the four side Direction values.
    Random,     ///< `p:random`; accepts no options.
    RandomBar,  ///< `p:randomBar`; accepts Orientation.
    Split,      ///< `p:split`; accepts Orientation and an in/out Direction.
    Strips,     ///< `p:strips`; accepts the four corner Direction values.
    Wedge,      ///< `p:wedge`; accepts no options.
    Wheel,      ///< `p:wheel`; accepts Spokes.
    Wipe,       ///< `p:wipe`; accepts the four side Direction values.
    Zoom,       ///< `p:zoom`; accepts an in/out Direction.
    Unsupported ///< An unrecognized effect retained unchanged during metadata edits.
};

/**
 * Direction of a slide transition effect.
 *
 * The four side values, the four corner values, and the two in/out values form
 * three disjoint families. Each effect accepts exactly one family, as documented
 * on @ref PresentationTransitionKind.
 */
enum class PresentationTransitionDirection
{
    Left,      ///< Side direction `l`.
    Up,        ///< Side direction `u`.
    Right,     ///< Side direction `r`.
    Down,      ///< Side direction `d`.
    LeftUp,    ///< Corner direction `lu`.
    RightUp,   ///< Corner direction `ru`.
    LeftDown,  ///< Corner direction `ld`.
    RightDown, ///< Corner direction `rd`.
    In,        ///< In/out direction `in`.
    Out        ///< In/out direction `out`.
};

/** Axis along which an orientation-based transition effect is drawn. */
enum class PresentationTransitionOrientation
{
    Horizontal, ///< PresentationML `horz`.
    Vertical    ///< PresentationML `vert`.
};

/**
 * @brief Effect-specific parameters of a slide transition.
 *
 * Every member is optional and is written only when present. Supplying a member
 * that the selected @ref PresentationTransitionKind does not accept makes
 * @ref PresentationSlide::SetTransition fail without modifying the slide, so an
 * effect can never be paired with parameters PowerPoint would ignore or reject.
 * Reading returns exactly the parameters present in the file.
 */
struct PresentationTransitionOptions
{
    std::optional<PresentationTransitionDirection> Direction;     ///< Effect direction; family depends on the effect.
    std::optional<PresentationTransitionOrientation> Orientation; ///< Horizontal or vertical effect axis.
    std::optional<bool> ThroughBlack;                             ///< Fades through black between the two slides.
    std::optional<UInt32> Spokes;                                 ///< Number of wheel spokes; must be non-zero.
    bool operator==(const PresentationTransitionOptions&) const = default;
};

/** Controls the coarse PresentationML transition speed. */
enum class PresentationTransitionSpeed
{
    Slow,
    Medium,
    Fast
};

/** @brief Optional sound played when a slide transition starts. */
struct PresentationTransitionSound
{
    PresentationEmbeddedMedia Audio; ///< Package-resident opaque audio bytes and MIME type.
    std::string Name;                ///< Display name stored with the sound reference.
    bool BuiltIn = false;            ///< Marks the sound as a recognized built-in PowerPoint sound.
    bool Loop = false;               ///< Repeats the sound until another sound starts.
    bool operator==(const PresentationTransitionSound&) const = default;
};

/**
 * @brief Editable slide-transition effect, timing, advance, and sound metadata.
 *
 * Duration and AdvanceAfter are expressed in milliseconds. AdvanceAfter being
 * absent disables automatic advance. An Unsupported value is returned for an
 * effect unknown to this API; passing that value back to SetTransition updates
 * common metadata while retaining the original effect subtree unchanged.
 */
struct PresentationTransitionData
{
    PresentationTransitionKind Kind = PresentationTransitionKind::Fade;
    PresentationTransitionSpeed Speed = PresentationTransitionSpeed::Medium;
    std::optional<UInt32> Duration;                   ///< Effect duration in milliseconds.
    bool AdvanceOnClick = true;                       ///< Allows a click to advance the slide.
    std::optional<UInt32> AdvanceAfter;               ///< Automatic-advance delay in milliseconds.
    std::optional<PresentationTransitionSound> Sound; ///< Optional embedded transition sound.
    PresentationTransitionOptions Options{};          ///< Effect-specific parameters accepted by @ref Kind.
    bool operator==(const PresentationTransitionData&) const = default;
};

/**
 * @brief One editable free-standing `p:anim` behavior in a slide animation timeline.
 *
 * This is the low-level escape hatch for hand-built timing markup: it addresses
 * `p:anim` elements that are direct children of the slide's `p:timing/p:tnLst`.
 * Behaviors nested inside the animation sequences built by
 * @ref PresentationSlide::SetAnimationEffects are deliberately out of scope here
 * and are edited through @ref PresentationAnimationEffectData instead.
 */
struct PresentationAnimationNode
{
    UInt32 Id = 0;                  ///< Stable timing-node ID; zero requests allocation when adding.
    UInt32 TargetShapeId = 0;       ///< Non-visual ID of a shape on the same slide.
    std::optional<UInt32> Duration; ///< Duration in milliseconds; absent means indefinite.
    std::string From;               ///< Optional PresentationML start-value expression.
    std::string To;                 ///< Optional PresentationML end-value expression.
    std::string By;                 ///< Optional PresentationML relative-value expression.
    bool operator==(const PresentationAnimationNode&) const = default;
};

/** Gallery an animation effect belongs to, which also selects its `presetClass`. */
enum class PresentationAnimationEffectClass
{
    Entrance,  ///< Makes a hidden shape visible (`entr`).
    Emphasis,  ///< Draws attention to an already visible shape (`emph`).
    Exit,      ///< Hides a visible shape (`exit`).
    MotionPath ///< Moves a shape along an explicit path (`path`).
};

/**
 * Concrete animation effect modelled by the high-level API.
 *
 * Effects are orthogonal to @ref PresentationAnimationEffectClass wherever
 * PowerPoint itself offers the same effect in more than one gallery: for example
 * @ref PresentationAnimationEffect::Fade is an entrance effect when combined with
 * @ref PresentationAnimationEffectClass::Entrance and an exit effect when combined
 * with @ref PresentationAnimationEffectClass::Exit. The combinations accepted by
 * @ref PresentationSlide::SetAnimationEffects are:
 *
 * - Entrance and Exit: Appear, Fade, Fly, Wipe, Zoom
 * - Emphasis: GrowShrink, Spin, ChangeFillColor
 * - MotionPath: MotionPath
 */
enum class PresentationAnimationEffect
{
    Appear,          ///< Instant visibility toggle; PowerPoint calls the exit variant "Disappear".
    Fade,            ///< Opacity fade in or out.
    Fly,             ///< Motion from or to a slide edge; requires a side Direction.
    Wipe,            ///< Directional reveal or conceal; requires a side Direction.
    Zoom,            ///< Scale in or out; requires an in/out Direction.
    GrowShrink,      ///< Emphasis scale to @ref PresentationAnimationEffectData::ScalePercent.
    Spin,            ///< Emphasis rotation by @ref PresentationAnimationEffectData::RotationDegrees.
    ChangeFillColor, ///< Emphasis solid-fill color change to @ref PresentationAnimationEffectData::Color.
    MotionPath,      ///< Motion along @ref PresentationAnimationEffectData::MotionPath.
    Unsupported      ///< An effect present in the file that this API does not model.
};

/** Start relationship between an animation effect and the effect preceding it. */
enum class PresentationAnimationTrigger
{
    OnClick,      ///< Starts its own click group and waits for the next advance click.
    WithPrevious, ///< Starts simultaneously with the preceding effect in the same group.
    AfterPrevious ///< Starts when the preceding effect in the same click group finishes.
};

/** Edge or axis an entrance, exit, or motion effect animates along. */
enum class PresentationAnimationDirection
{
    Left,  ///< From or towards the left slide edge.
    Up,    ///< From or towards the top slide edge.
    Right, ///< From or towards the right slide edge.
    Down,  ///< From or towards the bottom slide edge.
    In,    ///< Scales up from nothing (entrance) or down to nothing (exit).
    Out    ///< Scales down from oversize (entrance) or up past full size (exit).
};

/**
 * @brief Timing applied to one animation effect.
 *
 * All times are milliseconds. Values are stored on the effect's own `p:cTn`
 * time node, so they describe the effect as a whole rather than any single
 * behavior inside it. @ref PresentationSlide::SetAnimationEffects rejects a
 * zero @ref Duration, a zero @ref RepeatCount, a @ref RepeatCount combined with
 * @ref RepeatIndefinitely, and an @ref Acceleration and @ref Deceleration whose
 * sum exceeds 100000.
 */
struct PresentationAnimationTiming
{
    UInt32 Delay = 0;                  ///< Delay before the effect starts once triggered.
    UInt32 Duration = 500;             ///< Duration of a single iteration; must be non-zero.
    std::optional<UInt32> RepeatCount; ///< Total iterations; absent means a single pass.
    bool RepeatIndefinitely = false;   ///< Repeats until the slide advances.
    bool AutoReverse = false;          ///< Plays the effect backwards after each forward pass.
    UInt32 Acceleration = 0;           ///< Ease-in fraction in thousandths of one percent.
    UInt32 Deceleration = 0;           ///< Ease-out fraction in thousandths of one percent.
    bool operator==(const PresentationAnimationTiming&) const = default;
};

/**
 * @brief One animation effect applied to one shape of a slide.
 *
 * An effect combines a target shape, a gallery (@ref Class), a concrete
 * @ref Effect, a @ref Trigger describing how it chains onto the previous effect,
 * and @ref Timing. Effect-specific parameters live in the optional members below
 * and must be present exactly when the selected effect uses them:
 *
 * | Effect | Required parameter |
 * |---|---|
 * | Fly, Wipe | @ref Direction, one of Left, Up, Right, Down |
 * | Zoom | @ref Direction, one of In, Out |
 * | GrowShrink | @ref ScalePercent |
 * | Spin | @ref RotationDegrees |
 * | ChangeFillColor | @ref Color |
 * | MotionPath | @ref MotionPath |
 *
 * Appear and Fade take no parameter. Supplying a parameter an effect does not
 * use, or omitting one it requires, makes the whole write fail without touching
 * the slide.
 */
struct PresentationAnimationEffectData
{
    UInt32 Id = 0;            ///< Stable effect time-node ID; zero requests allocation when writing.
    UInt32 TargetShapeId = 0; ///< Non-visual ID of the animated shape on the same slide.
    /**
     * Non-visual ID of a shape whose click starts this effect.
     *
     * Zero places the effect in the slide's main sequence, which plays on normal
     * slide-advance clicks. A non-zero value places it in an interactive sequence
     * triggered by clicking that shape; consecutive effects sharing the same
     * trigger shape share one interactive sequence.
     */
    UInt32 TriggerShapeId = 0;
    PresentationAnimationEffectClass Class = PresentationAnimationEffectClass::Entrance;
    PresentationAnimationEffect Effect = PresentationAnimationEffect::Fade;
    PresentationAnimationTrigger Trigger = PresentationAnimationTrigger::OnClick;
    PresentationAnimationTiming Timing{};                    ///< Delay, duration, repetition, and easing.
    std::optional<PresentationAnimationDirection> Direction; ///< Direction for Fly, Wipe, and Zoom.
    std::optional<Int32> ScalePercent;                       ///< Target size for GrowShrink, in percent; must be positive.
    std::optional<Int32> RotationDegrees;                    ///< Signed rotation for Spin, in whole degrees.
    std::optional<std::string> Color;                        ///< Target fill color for ChangeFillColor as six hexadecimal digits.
    std::optional<std::string> MotionPath;                   ///< DrawingML motion path, for example `M 0 0 L 0.5 0.25 E`.
    bool operator==(const PresentationAnimationEffectData&) const = default;
};

/** @brief Named PowerPoint 2010 section and its ordered presentation slide IDs. */
struct PresentationSection
{
    std::string Id;               ///< Stable non-empty section identifier, commonly a GUID including braces.
    std::string Name;             ///< Human-readable section name.
    std::vector<UInt32> SlideIds; ///< Unique persistent slide IDs in presentation order.
    bool operator==(const PresentationSection&) const = default;
};

/** @brief Named custom slide-show sequence. */
struct PresentationCustomShow
{
    UInt32 Id = 0;                ///< Stable non-zero custom-show identifier.
    std::string Name;             ///< Non-empty display name, unique within the presentation.
    std::vector<UInt32> SlideIds; ///< Persistent slide IDs in explicit playback order.
    bool operator==(const PresentationCustomShow&) const = default;
};

/** Controls how shape removal handles animation behaviors targeting that shape. */
enum class PresentationAnimationRemovalPolicy
{
    Warn,                     ///< Keep the shape and dependent animations and report a dependency warning.
    RemoveDependentAnimations ///< Remove dependent animations before removing the shape.
};

/** Describes the outcome of dependency-aware shape removal. */
enum class PresentationShapeRemovalResult
{
    Removed,                   ///< The shape and any policy-selected dependencies were removed.
    NotAttached,               ///< The wrapper no longer identifies an attached drawable element.
    AnimationDependencyWarning ///< At least one animation targets the shape; nothing was removed.
};

/** @brief Text content of one physical cell in a rectangular DrawingML table grid. */
struct PresentationTableCell
{
    /** UTF-8 plain text. Newline characters are serialized as separate paragraphs. */
    std::string Text;
    bool operator==(const PresentationTableCell&) const = default;
};

/** @brief One physical table row and its unit-aware height. */
struct PresentationTableRow
{
    MeasuringUnits Height{};                  ///< Physical row height; must be non-negative.
    std::vector<PresentationTableCell> Cells; ///< Physical cells, one for every grid column.
    bool operator==(const PresentationTableRow&) const = default;
};

/**
 * @brief Rectangular merged region in a presentation table.
 *
 * Row and Column identify the top-left anchor cell. RowSpan and ColumnSpan
 * include that anchor and must describe at least two physical cells. Regions
 * may not overlap. Text is retained in the anchor cell; continuation-cell text
 * is ignored by PowerPoint while the merge exists but remains present in this
 * lossless high-level model.
 */
struct PresentationTableMerge
{
    Size Row = 0;
    Size Column = 0;
    Size RowSpan = 1;
    Size ColumnSpan = 1;
    bool operator==(const PresentationTableMerge&) const = default;
};

/** @brief Built-in table-style identifier and the conditional formatting flags applied to it. */
struct PresentationTableStyle
{
    std::string Id;             ///< Optional DrawingML table-style GUID, including braces when supplied by the file.
    bool FirstRow = false;      ///< Applies the style's header-row formatting.
    bool FirstColumn = false;   ///< Applies the style's first-column formatting.
    bool LastRow = false;       ///< Applies the style's last-row formatting.
    bool LastColumn = false;    ///< Applies the style's last-column formatting.
    bool BandedRows = false;    ///< Enables alternating row bands.
    bool BandedColumns = false; ///< Enables alternating column bands.
    bool RightToLeft = false;   ///< Uses right-to-left table direction.
    bool operator==(const PresentationTableStyle&) const = default;
};

/**
 * @brief Complete editable representation of a DrawingML presentation table.
 *
 * ColumnWidths and every row's Cells collection form a strictly rectangular
 * physical grid. A table must contain at least one row and one column. Merge
 * metadata is stored separately so structural operations can preserve both
 * rectangularity and unmerged cell content.
 */
struct PresentationTableData
{
    std::vector<MeasuringUnits> ColumnWidths;   ///< Physical column widths; values must be non-negative.
    std::vector<PresentationTableRow> Rows;     ///< Rows whose cell counts exactly match ColumnWidths.
    std::vector<PresentationTableMerge> Merges; ///< Non-overlapping merged regions.
    PresentationTableStyle Style{};             ///< Style identifier and conditional style flags.
    PresentationShapeTransform Transform{};     ///< Graphic-frame position and extent metadata.
    bool operator==(const PresentationTableData&) const = default;
};

/** Identifies a complex, package-backed object hosted by a presentation shape. */
enum class PresentationEmbeddedObjectKind
{
    Chart,    ///< A classic DrawingML chart (`c:chart`).
    SmartArt, ///< A DrawingML diagram described by `dgm:relIds`.
    Ole       ///< An embedded or linked OLE object (`p:oleObj`).
};

/**
 * @brief A package payload referenced by a chart, SmartArt, or OLE shape.
 *
 * RelationshipId is the stable identifier used by the shape markup. Xml and
 * BinaryData are mutually exclusive and reflect the target part's storage
 * kind. ContentType is retained verbatim so callers can identify opaque OLE
 * and embedded-package formats without interpreting their bytes.
 */
struct PresentationObjectPayload
{
    std::string RelationshipId;     ///< Relationship identifier in the owning slide part.
    std::string RelationshipType;   ///< Full OPC relationship type URI.
    std::string ContentType;        ///< MIME content type of the referenced package part.
    std::optional<std::string> Xml; ///< Serialized XML for an XML target part.
    std::vector<Byte> BinaryData;   ///< Exact bytes for a binary target part.
    bool operator==(const PresentationObjectPayload&) const = default;
};

/** @brief Preservation-oriented view of a chart, SmartArt, or OLE shape and its direct payloads. */
struct PresentationEmbeddedObject
{
    PresentationEmbeddedObjectKind Kind = PresentationEmbeddedObjectKind::Chart;
    std::vector<PresentationObjectPayload> Payloads; ///< Payloads in shape-reference order.
    bool operator==(const PresentationEmbeddedObject&) const = default;
};

/** @brief Plot type of a DrawingML chart hosted by a presentation graphic frame. */
enum class PresentationChartType
{
    Column,    ///< Vertical clustered column chart.
    Bar,       ///< Horizontal clustered bar chart.
    Line,      ///< Line chart with a category axis.
    Pie,       ///< Pie chart from a single series.
    Area,      ///< Area chart with a category axis.
    XyScatter, ///< XY scatter chart from paired numeric values.
    Bubble,    ///< Bubble chart from paired numeric values plus bubble sizes.
    Unknown    ///< A chart part whose plot type is not one of the recognized values above.
};

/** @brief Placement of a presentation chart legend. */
enum class PresentationChartLegendPosition
{
    None,  ///< No legend is drawn.
    Right, ///< Legend to the right of the plot area.
    Left,  ///< Legend to the left of the plot area.
    Top,   ///< Legend above the plot area.
    Bottom ///< Legend below the plot area.
};

/**
 * @brief One data series of a presentation chart, expressed as literal cached values.
 *
 * A presentation chart has no worksheet of its own, so a series carries literal
 * numbers rather than cell ranges (mirroring @ref ExyokiOffice::Word::WordChartSeries).
 * For @ref PresentationChartType::XyScatter and @ref PresentationChartType::Bubble,
 * @ref Values holds the Y values and @ref Categories instead holds the decimal
 * text of the X values; otherwise @ref Categories holds category axis labels.
 */
struct PresentationChartSeries
{
    std::string Name;                                   ///< Series display name shown in the legend.
    std::vector<Real> Values;                           ///< Series numeric (Y) values in point order.
    std::optional<std::vector<std::string>> Categories; ///< Category labels, or X-value text for scatter/bubble.
    std::optional<std::vector<Real>> BubbleSizes;       ///< Bubble sizes; used only when creating a bubble chart.
    bool operator==(const PresentationChartSeries&) const = default;
};

/**
 * @brief Complete description of a chart to insert into a slide's shape tree.
 *
 * The chart is anchored by @ref Transform and rendered from the cached values in
 * @ref Series. A minimal cached data source is written directly into the chart
 * part; no separate embedded workbook is generated (one can be attached with
 * @ref PresentationShape::SetChartEmbeddedWorkbook).
 */
struct PresentationChartDefinition
{
    PresentationChartType Type = PresentationChartType::Column;                              ///< Chart plot type.
    std::string Name;                                                                        ///< Non-visual graphic-frame name; a default is generated when empty.
    std::string Title;                                                                       ///< Chart title; no title element is written when empty.
    std::string CategoryAxisTitle;                                                           ///< Category (X) axis title; ignored by pie charts.
    std::string ValueAxisTitle;                                                              ///< Value (Y) axis title; ignored by pie charts.
    bool ShowLegend = true;                                                                  ///< Whether the legend is drawn.
    PresentationChartLegendPosition LegendPosition = PresentationChartLegendPosition::Right; ///< Legend placement.
    bool ShowGridLines = true;                                                               ///< Whether major value-axis gridlines are drawn.
    std::vector<PresentationChartSeries> Series;                                             ///< Data series; at least one is required.
    PresentationShapeTransform Transform{};                                                  ///< Graphic-frame position and extent on the slide.
    bool operator==(const PresentationChartDefinition&) const = default;
};

/** @brief Read-only snapshot of a chart hosted by a presentation graphic frame. */
struct PresentationChartInfo
{
    std::string Title;                                           ///< Chart title, or empty when the chart has no title element.
    PresentationChartType Type = PresentationChartType::Unknown; ///< Plot type of the first plot-type group.
    std::vector<PresentationChartSeries> Series;                 ///< Series read from the chart's cached values.
    bool HasEmbeddedWorkbook = false;                            ///< True when the chart part carries an embedded workbook package.
    bool operator==(const PresentationChartInfo&) const = default;
};

/** @brief Identity information for an author used by modern PowerPoint comments. */
struct PresentationCommentAuthor
{
    std::string Id;         ///< Stable author identifier; must be unique and non-empty.
    std::string Name;       ///< Display name shown by PowerPoint.
    std::string Initials;   ///< Short initials used by comment indicators.
    std::string UserId;     ///< Optional provider-specific user identifier.
    std::string ProviderId; ///< Optional identity-provider identifier.
    bool operator==(const PresentationCommentAuthor&) const = default;
};

/** @brief One reply in a modern PowerPoint comment thread. */
struct PresentationCommentReply
{
    std::string Id;       ///< Stable reply identifier, unique within the presentation.
    std::string AuthorId; ///< Identifier of an existing presentation comment author.
    std::string Text;     ///< UTF-8 plain-text reply body.
    bool operator==(const PresentationCommentReply&) const = default;
};

/** @brief Lifecycle state of a modern PowerPoint comment thread. */
enum class PresentationCommentStatus
{
    Active,   ///< The thread is open and accepts further discussion.
    Resolved, ///< The thread has been marked as resolved.
    Closed    ///< The thread has been closed.
};

/** @brief A modern PowerPoint comment and its ordered replies. */
struct PresentationComment
{
    std::string Id;                                                       ///< Stable comment identifier, unique within the presentation.
    std::string AuthorId;                                                 ///< Identifier of an existing presentation comment author.
    std::string Text;                                                     ///< UTF-8 plain-text comment body.
    PresentationPoint Position{};                                         ///< Unit-aware physical comment-indicator position.
    std::vector<PresentationCommentReply> Replies;                        ///< Replies in document order.
    PresentationCommentStatus Status = PresentationCommentStatus::Active; ///< Thread lifecycle state.
    bool operator==(const PresentationComment&) const = default;
};

/** @brief Editable content and inheritance settings of one slide's notes page. */
struct PresentationNotesPage
{
    std::string Text;                            ///< UTF-8 speaker-notes text; newline characters create separate paragraphs.
    bool ShowMasterShapes = true;                ///< Whether shapes inherited from the notes master are displayed.
    bool ShowMasterPlaceholderAnimations = true; ///< Whether inherited placeholder animations are displayed.
    bool operator==(const PresentationNotesPage&) const = default;
};

/** @brief Presentation-wide formatting stored by the handout master. */
struct PresentationHandoutSettings
{
    PresentationSize PageSize{}; ///< Physical notes/handout page extent stored in `p:notesSz`.
    bool ShowHeader = true;      ///< Whether the handout header placeholder is displayed.
    bool ShowFooter = true;      ///< Whether the handout footer placeholder is displayed.
    bool ShowDateTime = true;    ///< Whether the handout date/time placeholder is displayed.
    bool ShowSlideNumber = true; ///< Whether the handout slide-number placeholder is displayed.
    bool operator==(const PresentationHandoutSettings&) const = default;
};

/**
 * @brief Physical slide extent and its PresentationML size classification (`p:sldSz`).
 *
 * A freshly created presentation has no explicit slide size; PowerPoint then
 * falls back to its own default. Set one to pin the deck to a known aspect
 * ratio. Width and height accept any supported measurement unit and are
 * rounded to integral EMU when serialized. Type is the classification
 * PowerPoint shows in its page-setup dialog; leaving it unset writes no
 * `type` attribute, which PresentationML reads as a custom size.
 */
struct PresentationSlideSize
{
    /** Physical slide extent; both dimensions must be positive. */
    PresentationSize Size{};
    /** Optional size classification, for example `Screen16x9`. */
    std::optional<DocumentFormat::OpenXml::Presentation::SlideSizeValues::Value> Type;
    bool operator==(const PresentationSlideSize&) const = default;

    /** @brief 13.333 x 7.5 inch widescreen size, the modern PowerPoint default. */
    static PresentationSlideSize Widescreen16x9();
    /** @brief 10 x 6.25 inch widescreen size offered by PowerPoint's page setup. */
    static PresentationSlideSize Widescreen16x10();
    /** @brief 10 x 7.5 inch on-screen size used by pre-2013 PowerPoint. */
    static PresentationSlideSize Standard4x3();
    /** @brief 297 x 210 mm landscape A4 paper size. */
    static PresentationSlideSize A4Landscape();
};

inline PresentationSlideSize PresentationSlideSize::Widescreen16x9()
{
    // 13 1/3 x 7.5 inches, spelled in EMU because the width is not exact in inches.
    return {PresentationSize(Int64{12192000}, Int64{6858000}),
            DocumentFormat::OpenXml::Presentation::SlideSizeValues::Screen16x9};
}

inline PresentationSlideSize PresentationSlideSize::Widescreen16x10()
{
    return {PresentationSize(MeasuringUnits(10.0, MeasurementUnit::Inch), MeasuringUnits(6.25, MeasurementUnit::Inch)),
            DocumentFormat::OpenXml::Presentation::SlideSizeValues::Screen16x10};
}

inline PresentationSlideSize PresentationSlideSize::Standard4x3()
{
    return {PresentationSize(MeasuringUnits(10.0, MeasurementUnit::Inch), MeasuringUnits(7.5, MeasurementUnit::Inch)),
            DocumentFormat::OpenXml::Presentation::SlideSizeValues::Screen4x3};
}

inline PresentationSlideSize PresentationSlideSize::A4Landscape()
{
    return {PresentationSize(MeasuringUnits(297.0, MeasurementUnit::Millimeter),
                             MeasuringUnits(210.0, MeasurementUnit::Millimeter)),
            DocumentFormat::OpenXml::Presentation::SlideSizeValues::A4};
}

/** @brief A named DrawingML geometry adjustment formula. */
struct PresentationGeometryAdjustment
{
    /** DrawingML guide name, for example `adj`. */
    std::string Name;
    /** DrawingML guide formula, for example `val 25000`. */
    std::string Formula;
    bool operator==(const PresentationGeometryAdjustment&) const = default;
};

/** @brief A formula-based point used by freeform paths and connection sites. */
struct PresentationGeometryPoint
{
    /** X coordinate or guide expression in the path coordinate system. */
    std::string X;
    /** Y coordinate or guide expression in the path coordinate system. */
    std::string Y;
    bool operator==(const PresentationGeometryPoint&) const = default;
};

/** @brief A connection site in a custom geometry coordinate system. */
struct PresentationConnectionSite
{
    /** Outgoing connection angle as an OOXML angle or guide expression. */
    std::string Angle;
    /** Site position in the custom geometry coordinate system. */
    PresentationGeometryPoint Position;
    bool operator==(const PresentationConnectionSite&) const = default;
};

/** Identifies a DrawingML freeform path command. */
enum class PresentationPathCommandType
{
    MoveTo,            ///< Starts a new subpath at one point.
    LineTo,            ///< Adds a straight segment ending at one point.
    QuadraticBezierTo, ///< Adds a quadratic Bézier segment using two points.
    CubicBezierTo,     ///< Adds a cubic Bézier segment using three points.
    Close              ///< Closes the current subpath; takes no points.
};

/**
 * @brief One command in a freeform path.
 * @note MoveTo and LineTo require one point, QuadraticBezierTo two, CubicBezierTo three, and Close none.
 */
struct PresentationPathCommand
{
    /** Command kind controlling the required number of points. */
    PresentationPathCommandType Type = PresentationPathCommandType::MoveTo;
    /** Formula-based command points in DrawingML order. */
    std::vector<PresentationGeometryPoint> Points;
    bool operator==(const PresentationPathCommand&) const = default;
};

/** @brief A DrawingML freeform path and its local coordinate extent. */
struct PresentationFreeformPath
{
    /** Width of the local path coordinate system. */
    Int64 Width = 0;
    /** Height of the local path coordinate system. */
    Int64 Height = 0;
    /** Whether PresentationML draws the path outline. */
    bool Stroke = true;
    /** Ordered path commands. */
    std::vector<PresentationPathCommand> Commands;
    bool operator==(const PresentationFreeformPath&) const = default;
};

/** @brief Reference from a connector end to a shape connection site. */
struct PresentationConnectorEndpoint
{
    /** Non-visual identifier of the connected shape. */
    UInt32 ShapeId = 0;
    /** Zero-based connection-site index on the connected shape. */
    UInt32 SiteIndex = 0;
    bool operator==(const PresentationConnectorEndpoint&) const = default;
};

/** @brief Selects the fill or line-color model applied to a shape or its outline. */
enum class PresentationFillKind
{
    /** No explicit fill element is written; DrawingML style, placeholder, and theme inheritance apply. */
    Inherited,
    /** An explicit `a:noFill`; the region is fully transparent. */
    None,
    /** A single sRGB color, serialized as `a:solidFill`. */
    Solid,
    /** A multi-stop linear gradient, serialized as `a:gradFill`; valid for shape fill only, not outlines. */
    Gradient
};

/** @brief One color stop in a linear gradient fill. */
struct PresentationGradientStop
{
    /** Explicit sRGB stop color; an automatic Color value is invalid. */
    Color ColorValue{};
    /** Stop position as a percentage from 0.0 through 100.0. */
    Real Position = 0.0;
    bool operator==(const PresentationGradientStop&) const = default;
};

/**
 * @brief Solid-color or linear-gradient fill applied to a shape's `a:spPr`.
 *
 * Kind selects which of the remaining fields are meaningful:
 * - Inherited writes no fill element, letting normal DrawingML inheritance apply.
 * - None writes `a:noFill`.
 * - Solid writes `a:solidFill` using ColorValue.
 * - Gradient writes `a:gradFill` using GradientStops (at least two stops with
 *   positions in the closed interval 0..100) and GradientAngle for the linear
 *   sweep direction.
 *
 * Gradient stop positions are converted to DrawingML thousandths of one percent
 * and the angle to the native 1/60000-degree unit only during serialization.
 */
struct PresentationShapeFill
{
    PresentationFillKind Kind = PresentationFillKind::Inherited; ///< Fill-model selector.
    Color ColorValue{};                                          ///< Solid fill color; used only when Kind is Solid.
    std::vector<PresentationGradientStop> GradientStops;         ///< Gradient stops; used only when Kind is Gradient.
    MeasuringAngle GradientAngle{0.0, AngleUnit::Degree};        ///< Linear gradient direction; used only when Kind is Gradient.
    bool operator==(const PresentationShapeFill&) const = default;
};

/**
 * @brief Line style, width, color, and dash applied to a shape outline (`a:ln`).
 *
 * Fill selects the outline color model:
 * - Solid uses ColorValue and writes `a:solidFill`.
 * - None writes `a:noFill`, hiding the outline.
 * - Inherited writes no line color element, so the effective color is inherited
 *   while the supplied width, dash, cap, and compound attributes still apply.
 * - Gradient is not supported for outlines and is rejected by SetOutline.
 *
 * Every attribute-bearing field is optional; an absent field lets the effective
 * value be inherited. Width accepts any supported physical unit and is converted
 * to EMU during serialization.
 */
struct PresentationShapeOutline
{
    PresentationFillKind Fill = PresentationFillKind::Inherited;                         ///< Outline color model; Gradient is invalid.
    Color ColorValue{};                                                                  ///< Solid outline color; used only when Fill is Solid.
    std::optional<MeasuringUnits> Width;                                                 ///< Line width; absent inherits the effective width.
    std::optional<DocumentFormat::OpenXml::Drawing::PresetLineDashValues::Value> Dash;   ///< Dash pattern.
    std::optional<DocumentFormat::OpenXml::Drawing::LineCapValues::Value> Cap;           ///< Line-end cap style.
    std::optional<DocumentFormat::OpenXml::Drawing::CompoundLineValues::Value> Compound; ///< Compound line type.
    bool operator==(const PresentationShapeOutline&) const = default;
};

/** @brief One unit-aware tab stop in a PresentationML paragraph. */
struct PresentationTextTab
{
    MeasuringUnits Position{}; ///< Physical position relative to the paragraph text origin.
    DocumentFormat::OpenXml::Drawing::TextTabAlignmentValues::Value Alignment =
        DocumentFormat::OpenXml::Drawing::TextTabAlignmentValues::Left; ///< Tab alignment.
    bool operator==(const PresentationTextTab&) const = default;
};

/** @brief Bullet configuration for one text paragraph. */
struct PresentationTextBullet
{
    std::optional<std::string> Character;                                                         ///< Literal bullet character; mutually exclusive with Numbering.
    std::optional<DocumentFormat::OpenXml::Drawing::TextAutoNumberSchemeValues::Value> Numbering; ///< Numbering scheme.
    Int32 StartAt = 1;                                                                            ///< Initial number for automatic numbering.
    bool operator==(const PresentationTextBullet&) const = default;
};

/**
 * @brief DrawingML paragraph spacing expressed either as a physical point length or a percentage.
 *
 * Exactly one representation must be supplied. Percent uses ordinary human-readable
 * percentages (`100.0` means single line spacing); it is converted to DrawingML's
 * thousandths-of-a-percent representation during serialization.
 */
struct PresentationTextSpacing
{
    std::optional<MeasuringUnits> Points; ///< Physical spacing; any supported unit is accepted.
    std::optional<Real> Percent;          ///< Percentage from 0.0 through 20116.8.
    bool operator==(const PresentationTextSpacing&) const = default;
};

/**
 * @brief A DrawingML glow applied to a text run.
 *
 * The effect is serialized as `a:glow` in the run's `a:effectLst`. Physical
 * lengths are accepted through MeasuringUnits and converted to English Metric
 * Units (EMUs) only when the document is serialized. The color must be an
 * explicit sRGB color; an automatic Color value is rejected.
 */
struct PresentationTextGlow
{
    /** Blur radius extending outward from the glyphs; must be non-negative. */
    MeasuringUnits Radius;
    /** Explicit sRGB glow color, serialized as `a:srgbClr`. */
    Color ColorValue;
    bool operator==(const PresentationTextGlow&) const = default;
};

/**
 * @brief A DrawingML outer shadow applied to a text run.
 *
 * The effect is serialized as `a:outerShdw` in the run's `a:effectLst`.
 * BlurRadius and Distance use MeasuringUnits because they are physical
 * distances. Angular properties use MeasuringAngle and accept degrees or
 * radians. Percentage properties retain their native DrawingML integer
 * representation so that exact OOXML values can be round-tripped.
 *
 * DrawingML represents angles in 1/60000 of a degree. For example, 5400000
 * represents 90 degrees. It represents percentages in thousandths of one
 * percent: 100000 represents 100% and 50000 represents 50%.
 */
struct PresentationTextShadow
{
    /** Shadow blur radius; must be non-negative. */
    MeasuringUnits BlurRadius;
    /** Distance by which the shadow is offset from the text; must be non-negative. */
    MeasuringUnits Distance;
    /** Explicit sRGB shadow color; an automatic Color value is invalid. */
    Color ColorValue;
    /** Offset direction; converted to DrawingML's 1/60000-degree representation. */
    MeasuringAngle Direction{0.0, AngleUnit::Degree};
    /** Horizontal size in thousandths of a percent; 100000 means 100%. */
    Int32 HorizontalScale = 100000;
    /** Vertical size in thousandths of a percent; 100000 means 100%. */
    Int32 VerticalScale = 100000;
    /** Horizontal skew angle. */
    MeasuringAngle HorizontalSkew{0.0, AngleUnit::Degree};
    /** Vertical skew angle. */
    MeasuringAngle VerticalSkew{0.0, AngleUnit::Degree};
    /** Alignment point used as the origin for scaling, skewing, and positioning. */
    DocumentFormat::OpenXml::Drawing::RectangleAlignmentValues::Value Alignment =
        DocumentFormat::OpenXml::Drawing::RectangleAlignmentValues::BottomRight;
    /** Whether the shadow direction rotates together with its containing shape. */
    bool RotateWithShape = true;
    bool operator==(const PresentationTextShadow&) const = default;
};

/**
 * @brief A DrawingML reflection applied to a text run.
 *
 * The effect is serialized as `a:reflection` in the run's `a:effectLst`.
 * BlurRadius and Distance are physical lengths and therefore use
 * MeasuringUnits. Angular properties use MeasuringAngle and accept degrees or
 * radians. Opacity, gradient position, and scale fields use the native integer
 * units defined by DrawingML to preserve exact OOXML values.
 *
 * Percentage values are stored in thousandths of one percent: 100000 is 100%,
 * 75000 is 75%, and -100000 is -100%. Angles are stored in 1/60000 of a
 * degree, making the default value 5400000 equal to 90 degrees.
 */
struct PresentationTextReflection
{
    /** Reflection blur radius; must be non-negative. */
    MeasuringUnits BlurRadius;
    /** Gap between the text and the reflection; must be non-negative. */
    MeasuringUnits Distance;
    /** Opacity at StartPosition in thousandths of a percent; 100000 means opaque. */
    Int32 StartOpacity = 100000;
    /** Start of the opacity fade in thousandths of a percent; 0 means 0%. */
    Int32 StartPosition = 0;
    /** Opacity at EndPosition in thousandths of a percent; 0 means transparent. */
    Int32 EndOpacity = 0;
    /** End of the opacity fade in thousandths of a percent; 100000 means 100%. */
    Int32 EndPosition = 100000;
    /** Reflection offset direction; the default is 90 degrees. */
    MeasuringAngle Direction{90.0, AngleUnit::Degree};
    /** Direction of the opacity fade; the default is 90 degrees. */
    MeasuringAngle FadeDirection{90.0, AngleUnit::Degree};
    /** Horizontal size in thousandths of a percent; 100000 means 100%. */
    Int32 HorizontalScale = 100000;
    /** Vertical size in thousandths of a percent; -100000 flips at 100% size. */
    Int32 VerticalScale = -100000;
    /** Horizontal skew angle. */
    MeasuringAngle HorizontalSkew{0.0, AngleUnit::Degree};
    /** Vertical skew angle. */
    MeasuringAngle VerticalSkew{0.0, AngleUnit::Degree};
    /** Alignment point used as the origin for the reflection transform. */
    DocumentFormat::OpenXml::Drawing::RectangleAlignmentValues::Value Alignment =
        DocumentFormat::OpenXml::Drawing::RectangleAlignmentValues::Bottom;
    /** Whether the reflection direction rotates together with its containing shape. */
    bool RotateWithShape = true;
    bool operator==(const PresentationTextReflection&) const = default;
};

/**
 * @brief Bevel geometry for one face of DrawingML three-dimensional text.
 *
 * A bevel is serialized as `a:bevelT` or `a:bevelB` within `a:sp3d`.
 * Width and Height accept any physical unit supported by MeasuringUnits and
 * are converted to EMUs during serialization.
 */
struct PresentationTextBevel
{
    /** Horizontal extent of the bevel; must be non-negative. */
    MeasuringUnits Width;
    /** Vertical extent of the bevel; must be non-negative. */
    MeasuringUnits Height;
    /** DrawingML preset that defines the bevel profile. */
    DocumentFormat::OpenXml::Drawing::BevelPresetValues::Value Preset =
        DocumentFormat::OpenXml::Drawing::BevelPresetValues::Circle;
    bool operator==(const PresentationTextBevel&) const = default;
};

/**
 * @brief The DrawingML scene and shape geometry used to render text in 3-D.
 *
 * Unlike glow, shadow, and reflection, DrawingML stores text 3-D properties on
 * the text body's `a:bodyPr`, not on an individual run. The value is serialized
 * as a paired `a:scene3d` and `a:sp3d`; consequently it applies to the complete
 * PresentationTextFrame. All geometric distances use MeasuringUnits and are
 * converted to EMUs during serialization. Optional colors must be explicit
 * sRGB colors rather than automatic Color values.
 */
struct PresentationText3D
{
    /** Z coordinate of the text shape in the 3-D scene; must be non-negative. */
    MeasuringUnits Depth;
    /** Distance through which the front face is extruded; must be non-negative. */
    MeasuringUnits ExtrusionHeight;
    /** Width of the contour around the extruded text; must be non-negative. */
    MeasuringUnits ContourWidth;
    /** Optional explicit sRGB color of the extrusion side faces. */
    std::optional<Color> ExtrusionColor;
    /** Optional explicit sRGB color of the text contour. */
    std::optional<Color> ContourColor;
    /** Optional bevel on the front face, serialized as `a:bevelT`. */
    std::optional<PresentationTextBevel> TopBevel;
    /** Optional bevel on the back face, serialized as `a:bevelB`. */
    std::optional<PresentationTextBevel> BottomBevel;
    /** Surface material preset controlling the text's 3-D shading response. */
    DocumentFormat::OpenXml::Drawing::PresetMaterialTypeValues::Value Material =
        DocumentFormat::OpenXml::Drawing::PresetMaterialTypeValues::Matte;
    /** Camera preset used to project the 3-D text scene. */
    DocumentFormat::OpenXml::Drawing::PresetCameraValues::Value Camera =
        DocumentFormat::OpenXml::Drawing::PresetCameraValues::OrthographicFront;
    /** Preset arrangement of lights illuminating the 3-D text. */
    DocumentFormat::OpenXml::Drawing::LightRigValues::Value LightRig =
        DocumentFormat::OpenXml::Drawing::LightRigValues::ThreePoints;
    /** Direction from which the selected light rig illuminates the scene. */
    DocumentFormat::OpenXml::Drawing::LightRigDirectionValues::Value LightDirection =
        DocumentFormat::OpenXml::Drawing::LightRigDirectionValues::Top;
    bool operator==(const PresentationText3D&) const = default;
};

/**
 * @brief Glow, outer-shadow, and reflection effects applied to a whole shape.
 *
 * These are serialized as `a:effectLst` inside the shape's `a:spPr` and therefore
 * decorate the entire shape rather than an individual text run. The effect payload
 * types are shared with run-level text formatting: DrawingML defines identical
 * `a:glow`, `a:outerShdw`, and `a:reflection` elements in both positions, so the
 * same descriptors are reused here. Every effect is optional; an effect that is
 * absent is not written, letting shape-style, placeholder, and theme inheritance
 * apply. An all-absent value clears the shape's effect list entirely.
 */
struct PresentationShapeEffects
{
    std::optional<PresentationTextGlow> Glow;             ///< Optional `a:glow` effect.
    std::optional<PresentationTextShadow> Shadow;         ///< Optional `a:outerShdw` effect.
    std::optional<PresentationTextReflection> Reflection; ///< Optional `a:reflection` effect.
    bool operator==(const PresentationShapeEffects&) const = default;
};

/**
 * @brief Text, direct character formatting, and an optional hyperlink for one DrawingML run.
 *
 * Font sizes and character spacing accept the shared unit-aware MeasuringUnits
 * type also used by Word and Excel. Only explicit sRGB colors are authored;
 * leave FontColor absent to inherit the presentation theme or layout style.
 */
struct PresentationTextRun
{
    std::string Text;                     ///< UTF-8 text stored in `a:t`.
    std::string Language;                 ///< Optional BCP 47 language tag.
    bool Bold = false;                    ///< Bold character formatting.
    bool Italic = false;                  ///< Italic character formatting.
    std::optional<std::string> Hyperlink; ///< Optional external hyperlink target.
    std::string HyperlinkTooltip;         ///< Optional hyperlink tooltip.
    DocumentFormat::OpenXml::Drawing::TextUnderlineValues::Value Underline =
        DocumentFormat::OpenXml::Drawing::TextUnderlineValues::None; ///< Underline style.
    DocumentFormat::OpenXml::Drawing::TextStrikeValues::Value Strike =
        DocumentFormat::OpenXml::Drawing::TextStrikeValues::NoStrike; ///< Strike-through style.
    DocumentFormat::OpenXml::Drawing::TextCapsValues::Value Capitalization =
        DocumentFormat::OpenXml::Drawing::TextCapsValues::None; ///< Normal, small-cap, or all-cap rendering.
    std::string Typeface;                                       ///< Optional Latin typeface; empty retains theme/inherited selection.
    std::optional<MeasuringUnits> FontSize;                     ///< Character size; serialized in hundredths of a point.
    std::optional<Color> FontColor;                             ///< Explicit RGB color; automatic colors are invalid here.
    std::optional<MeasuringUnits> CharacterSpacing;             ///< Extra character spacing in physical units.
    std::optional<PresentationTextGlow> Glow;                   ///< Optional run-level `a:glow` effect.
    std::optional<PresentationTextShadow> Shadow;               ///< Optional run-level `a:outerShdw` effect.
    std::optional<PresentationTextReflection> Reflection;       ///< Optional run-level `a:reflection` effect.
    bool operator==(const PresentationTextRun&) const = default;
};

/**
 * @brief A DrawingML paragraph with runs, layout, spacing, indentation, bullets, and tabs.
 *
 * Optional properties represent direct formatting. Their absence lets layout,
 * master, and theme text styles participate in PowerPoint's normal inheritance.
 */
struct PresentationTextParagraph
{
    std::vector<PresentationTextRun> Runs; ///< Runs in document order.
    DocumentFormat::OpenXml::Drawing::TextAlignmentTypeValues::Value Alignment =
        DocumentFormat::OpenXml::Drawing::TextAlignmentTypeValues::Left; ///< Horizontal alignment.
    std::optional<PresentationTextBullet> Bullet;                        ///< Bullet configuration; absent means `a:buNone`.
    std::vector<PresentationTextTab> Tabs;                               ///< Explicit tab stops in ascending or author-supplied order.
    std::optional<MeasuringUnits> LeftMargin;                            ///< Paragraph left margin.
    std::optional<MeasuringUnits> RightMargin;                           ///< Paragraph right margin.
    std::optional<MeasuringUnits> FirstLineIndent;                       ///< First-line or hanging indent; negative values are allowed.
    std::optional<MeasuringUnits> DefaultTabSize;                        ///< Default tab interval; must be non-negative.
    std::optional<PresentationTextSpacing> LineSpacing;                  ///< Line spacing as points or percentage.
    std::optional<PresentationTextSpacing> SpaceBefore;                  ///< Space before the paragraph.
    std::optional<PresentationTextSpacing> SpaceAfter;                   ///< Space after the paragraph.
    Int32 Level = 0;                                                     ///< List level from 0 through 8.
    bool RightToLeft = false;                                            ///< Enables right-to-left paragraph layout.
    DocumentFormat::OpenXml::Drawing::TextFontAlignmentValues::Value FontAlignment =
        DocumentFormat::OpenXml::Drawing::TextFontAlignmentValues::Automatic; ///< Vertical font alignment in the line.
    bool operator==(const PresentationTextParagraph&) const = default;
};

/** @brief Complete editable text-frame content and body layout metadata. */
struct PresentationTextFrame
{
    MeasuringUnits LeftMargin{0.1, MeasurementUnit::Inch};    ///< Left text inset.
    MeasuringUnits TopMargin{0.05, MeasurementUnit::Inch};    ///< Top text inset.
    MeasuringUnits RightMargin{0.1, MeasurementUnit::Inch};   ///< Right text inset.
    MeasuringUnits BottomMargin{0.05, MeasurementUnit::Inch}; ///< Bottom text inset.
    DocumentFormat::OpenXml::Drawing::TextVerticalValues::Value Vertical =
        DocumentFormat::OpenXml::Drawing::TextVerticalValues::Horizontal; ///< Text direction.
    DocumentFormat::OpenXml::Drawing::TextAnchoringTypeValues::Value Anchor =
        DocumentFormat::OpenXml::Drawing::TextAnchoringTypeValues::Top; ///< Vertical anchoring.
    /** Optional body-level 3-D scene and geometry applied to every paragraph and run. */
    std::optional<PresentationText3D> ThreeD;
    std::vector<PresentationTextParagraph> Paragraphs; ///< Paragraphs in document order.
    bool operator==(const PresentationTextFrame&) const = default;
};

/** Identifies the PresentationML level that supplies an effective placeholder. */
enum class PlaceholderOrigin
{
    /** Placeholder is physically stored in the presentation slide part. */
    Slide,
    /** Placeholder is inherited from or physically stored in a slide layout part. */
    Layout,
    /** Placeholder is inherited from or physically stored in a slide master part. */
    Master
};

/**
 * @brief Represents one drawable child of a PresentationML shape tree.
 *
 * The wrapper is intentionally geometry-neutral: PPT-006 manages tree
 * structure and stacking order, while callers may use GetElement() for typed
 * DrawingML edits. A group exposes its own child tree through Children().
 */
class EXYOKIOFFICE_EXPORT PresentationShape
{
public:
    using Ptr = std::shared_ptr<PresentationShape>;

    /**
     * @return The underlying `p:sp`, `p:grpSp`, `p:pic`, `p:graphicFrame`, `p:cxnSp`,
     * or `p:contentPart`; nullptr when this wrapper is detached.
     */
    std::shared_ptr<OpenXMLElement> GetElement() const;
    /** @return The non-visual shape identifier, or zero when no valid identity is present. */
    UInt32 Id() const;
    /** @return True when this item is a `p:grpSp` container. */
    [[nodiscard]] bool IsGroup() const noexcept;
    /** @return Direct children of a group in XML (back-to-front) order; empty for non-groups. */
    std::vector<Ptr> Children() const;
    /**
     * @brief Reads this shape's transform without unit conversion.
     * @return Transform data, or `std::nullopt` when the shape type has no transform.
     * @note Missing optional XML attributes and child elements use OOXML defaults of zero and false.
     */
    std::optional<PresentationShapeTransform> GetTransform() const;
    /**
     * @brief Replaces this shape's position, extent, rotation, and flip metadata.
     * @param transform Native OOXML transform values. Width and height must be non-negative.
     * @return `true` on success; `false` for unsupported shapes or invalid group coordinates.
     * @note Group child coordinates are required for groups and forbidden for non-group shapes.
     */
    bool SetTransform(const PresentationShapeTransform& transform);
    /** @return The preset geometry kind, or `std::nullopt` for custom/absent geometry. */
    std::optional<DocumentFormat::OpenXml::Drawing::ShapeTypeValues::Value> GetPresetGeometry() const;
    /** @return Preset geometry adjustment formulas in XML order. */
    std::vector<PresentationGeometryAdjustment> GeometryAdjustments() const;
    /** @brief Replaces existing geometry with a preset and its adjustment formulas. */
    bool SetPresetGeometry(DocumentFormat::OpenXml::Drawing::ShapeTypeValues::Value preset,
                           const std::vector<PresentationGeometryAdjustment>& adjustments = {});
    /** @return Custom geometry paths in XML order; empty for preset or absent geometry. */
    std::vector<PresentationFreeformPath> FreeformPaths() const;
    /** @return Connection sites declared by custom geometry in XML order. */
    std::vector<PresentationConnectionSite> ConnectionSites() const;
    /**
     * @brief Replaces existing geometry with custom freeform paths and connection sites.
     * @return `false` if no path is supplied, an extent is negative, or a command has the wrong point count.
     */
    bool SetFreeformGeometry(const std::vector<PresentationFreeformPath>& paths,
                             const std::vector<PresentationConnectionSite>& connectionSites = {});
    /** @return Connector start endpoint, or `std::nullopt` when unconnected/not a connector. */
    std::optional<PresentationConnectorEndpoint> StartEndpoint() const;
    /** @return Connector end endpoint, or `std::nullopt` when unconnected/not a connector. */
    std::optional<PresentationConnectorEndpoint> EndEndpoint() const;
    /**
     * @brief Sets or clears both connector endpoint references.
     * @return `false` when this wrapper does not represent a connector.
     */
    bool SetConnectorEndpoints(std::optional<PresentationConnectorEndpoint> start,
                               std::optional<PresentationConnectorEndpoint> end);
    /**
     * @brief Reads the shape's explicit fill from its `a:spPr`.
     * @return The current fill descriptor, or `std::nullopt` for shape kinds that
     *         cannot carry a shape fill (groups and graphic frames such as tables,
     *         charts, SmartArt, and OLE objects).
     * @note A supported shape with no explicit fill element reports Kind == Inherited.
     */
    std::optional<PresentationShapeFill> GetFill() const;
    /**
     * @brief Replaces the shape's fill, writing `a:solidFill`, `a:gradFill`, or `a:noFill`.
     * @param fill Fill descriptor. Solid requires an explicit color; Gradient requires
     *             at least two stops with explicit colors and positions in 0..100.
     * @return `false` for unsupported shape kinds or invalid fill data; the existing
     *         fill is left unchanged when validation fails.
     * @note Any previously authored fill element (solid, gradient, none, blip, or
     *       pattern) is removed before the new fill is written.
     *
     * @code
     * PresentationShapeFill fill;
     * fill.Kind = PresentationFillKind::Solid;
     * fill.ColorValue = Color(ColorPreset::Blue);
     * shape->SetFill(fill);
     * @endcode
     */
    bool SetFill(const PresentationShapeFill& fill);
    /**
     * @brief Reads the shape's explicit outline from its `a:spPr/a:ln`.
     * @return The current outline descriptor, or `std::nullopt` when no `a:ln`
     *         element is present or the shape kind cannot carry an outline.
     */
    std::optional<PresentationShapeOutline> GetOutline() const;
    /**
     * @brief Replaces the shape's outline element `a:ln` and its attributes.
     * @param outline Outline descriptor. Fill == Solid requires an explicit color;
     *                Fill == Gradient is rejected. A negative width is rejected.
     * @return `false` for unsupported shape kinds or invalid outline data; the
     *         existing outline is left unchanged when validation fails.
     */
    bool SetOutline(const PresentationShapeOutline& outline);
    /**
     * @brief Reads the shape-level glow, shadow, and reflection from its `a:spPr/a:effectLst`.
     * @return The current effect descriptor, or `std::nullopt` for shape kinds that
     *         cannot carry a shape effect list (groups and graphic frames).
     * @note A supported shape with no effect list reports every effect absent.
     */
    std::optional<PresentationShapeEffects> GetEffects() const;
    /**
     * @brief Replaces the shape's `a:effectLst` with the supplied glow, shadow, and reflection.
     * @param effects Effect descriptor. Each present effect is validated; radii and
     *                distances must be non-negative and effect colors explicit sRGB.
     * @return `false` for unsupported shape kinds or invalid effect data; the existing
     *         effect list is left unchanged when validation fails.
     * @note An all-absent value removes the effect list entirely.
     *
     * @code
     * PresentationShapeEffects effects;
     * PresentationTextShadow shadow;
     * shadow.BlurRadius = MeasuringUnits(4.0, MeasurementUnit::Point);
     * shadow.Distance = MeasuringUnits(3.0, MeasurementUnit::Point);
     * shadow.ColorValue = Color(0x80, 0x80, 0x80);
     * effects.Shadow = shadow;
     * shape->SetEffects(effects);
     * @endcode
     */
    bool SetEffects(const PresentationShapeEffects& effects);
    /** @return The complete text frame, or `std::nullopt` when this shape has none. */
    std::optional<PresentationTextFrame> GetTextFrame() const;
    /**
     * @brief Replaces the shape text frame, including paragraphs, runs, bullets, tabs, and hyperlinks.
     * @return `false` for shape kinds that cannot contain `p:txBody` or invalid bullet settings.
     * @note Hyperlinks create external relationships on the owning slide part; no resource is accessed.
     * @note Input is validated before the existing text body or hyperlink relationships are modified.
     *
     * @code
     * PresentationTextRun run;
     * run.Text = "Quarterly report";
     * run.Typeface = "Aptos Display";
     * run.FontSize = MeasuringUnits(24.0, MeasurementUnit::Point);
     * run.FontColor = Color(ColorPreset::Blue);
     * run.Bold = true;
     *
     * PresentationTextParagraph paragraph;
     * paragraph.Runs = {run};
     * paragraph.Alignment = DocumentFormat::OpenXml::Drawing::TextAlignmentTypeValues::Center;
     * paragraph.SpaceAfter = PresentationTextSpacing{MeasuringUnits(6.0, MeasurementUnit::Point), std::nullopt};
     *
     * PresentationTextFrame frame;
     * frame.Paragraphs = {paragraph};
     * shape->SetTextFrame(frame);
     * @endcode
     */
    bool SetTextFrame(const PresentationTextFrame& frame);
    /** @return Picture payload and metadata, or `std::nullopt` when this shape is not a picture. */
    std::optional<PresentationPictureData> GetPicture() const;
    /**
     * @brief Replaces the picture payload, crop, accessibility, hyperlink, and transform metadata.
     * @return `false` for non-picture shapes or when the source, media type, crop, or transform is invalid.
     * @note Linked image URIs are stored as external relationships and are never dereferenced.
     */
    bool SetPicture(const PresentationPictureData& picture);
    /**
     * @brief Replaces only the embedded image payload, preserving crop, transform,
     *        accessibility, hyperlink, outline, and effect metadata.
     * @param data PNG, JPEG, GIF, or BMP bytes detected from their signature.
     * @return `false` for a non-picture shape or unsupported/malformed image data.
     */
    bool ReplacePictureFromData(std::vector<Byte> data);
    /**
     * @brief Reads an image file and replaces only this picture's embedded payload.
     * @return `false` when the file cannot be read or its image format is unsupported.
     */
    bool ReplacePictureFromFile(const std::filesystem::path& path);
    /**
     * @brief Turns a linked picture into an embedded one by reading the link.
     *
     * The library does not fetch the image; it asks the resolver installed on
     * the presentation package, and only after the target has passed the
     * external resource policy. On success the bytes become an image part, the
     * `a:blip r:link` becomes `r:embed`, and the external relationship is
     * dropped, so the slide no longer depends on anything outside the package.
     * The picture is left untouched when the resource cannot be read.
     *
     * @param cancellationToken Optional non-owning token handed to the resolver.
     * @return Ok when the picture is now embedded; otherwise the reason it is
     *         not. A picture that is already embedded reports NotFound.
     */
    [[nodiscard]] Security::ExternalResourceStatus EmbedLinkedPicture(
        const ICancellationToken* cancellationToken = nullptr);
    /** @return Audio/video data for this media picture, or `std::nullopt` for ordinary shapes. */
    std::optional<PresentationMediaData> GetMedia() const;
    /**
     * @brief Replaces the media source, poster frame, and playback metadata.
     * @return `false` for non-media shapes or invalid source, content type, volume, or transform data.
     * @note Embedded bytes and linked URIs remain opaque; no playback or external I/O is performed.
     */
    bool SetMedia(const PresentationMediaData& media);
    /**
     * @brief Replaces only the embedded audio/video payload from a file.
     * @param path File whose bytes are stored verbatim.
     * @param contentType IANA media type such as `audio/mpeg` or `video/mp4`.
     * @return `false` for a non-media shape, unreadable file, or empty content type.
     * @note Kind, poster frame, playback, accessibility, and transform are preserved.
     */
    bool ReplaceMediaFromFile(const std::filesystem::path& path, std::string contentType);
    /**
     * @brief Turns linked audio or video into an embedded payload by reading the link.
     *
     * Works exactly like EmbedLinkedPicture(): the resolver installed on the
     * package is asked, the policy decides, and the bytes are stored verbatim.
     * The media stream is never inspected or transcoded.
     *
     * @param contentType IANA media type to store for the embedded payload, for
     *        example `video/mp4`. When empty, the type the resolver reported is
     *        used, and the call fails when the resolver reported none.
     * @param cancellationToken Optional non-owning token handed to the resolver.
     * @return Ok when the media is now embedded; otherwise the reason it is not.
     */
    [[nodiscard]] Security::ExternalResourceStatus EmbedLinkedMedia(
        std::string contentType = {},
        const ICancellationToken* cancellationToken = nullptr);
    /** @return Complete table data, or `std::nullopt` when this shape is not a DrawingML table. */
    std::optional<PresentationTableData> GetTable() const;
    /**
     * @brief Replaces a table's complete rectangular grid, merge map, text, style, and transform.
     * @return `false` for non-table frames or invalid dimensions, extents, or overlapping merges.
     * @note Input is fully validated before the existing table is modified.
     */
    bool SetTable(const PresentationTableData& table);
    /**
     * @return A lossless package-payload view for chart, SmartArt, and OLE
     *         shapes, or `std::nullopt` for other shapes.
     * @note The wrapper does not interpret or normalize application-specific XML.
     */
    std::optional<PresentationEmbeddedObject> GetEmbeddedObject() const;
    /**
     * @brief Replaces one referenced payload without changing its relationship or part URI.
     * @param payload Replacement data. RelationshipId selects an existing direct target.
     * @return `false` if the shape does not reference that relationship, the target is
     *         external, or the supplied XML/binary representation does not match the part kind.
     * @note Descendant relationships of the target part remain attached and unchanged.
     */
    bool ReplaceEmbeddedObjectPayload(const PresentationObjectPayload& payload);
    /**
     * @brief Reads the chart hosted by this graphic frame from its cached values.
     * @return The chart snapshot, or `std::nullopt` when this shape is not a chart
     *         graphic frame or its chart part cannot be resolved.
     * @note The returned series are the cached numbers the chart displays without
     *       recalculation, not a live read of any embedded workbook.
     */
    std::optional<PresentationChartInfo> GetChart() const;
    /**
     * @brief Replaces the cached data (and optionally the title) of this chart.
     * @param series Replacement series in document order. Bubble sizes are not updated.
     * @param title New title; `std::nullopt` leaves the existing title unchanged, an
     *              empty string removes it.
     * @return `false` when this shape is not a chart or the chart plot type is unrecognized.
     * @note The chart plot type, formatting, and series source formulas are preserved.
     */
    bool UpdateChartData(const std::vector<PresentationChartSeries>& series,
                         std::optional<std::string> title = std::nullopt);
    /**
     * @brief Returns the bytes of this chart's embedded workbook package, if any.
     * @return The workbook bytes, or `std::nullopt` when the chart has no embedded
     *         workbook or this shape is not a chart.
     */
    std::optional<std::vector<Byte>> GetChartEmbeddedWorkbook() const;
    /**
     * @brief Sets (creating if needed) this chart's embedded workbook package bytes.
     * @return `false` when this shape is not a chart graphic frame.
     * @note The bytes are stored verbatim and never opened; a full `.xlsx` package is expected.
     */
    bool SetChartEmbeddedWorkbook(std::span<const Byte> bytes);
    /**
     * @brief Re-reads this chart's series values and labels from its embedded workbook.
     *
     * Each series' existing value and category source formula (for example
     * `Sheet1!$A$2:$A$5`, as authored by AddChart or a prior refresh) is resolved
     * against the embedded workbook and used to rewrite the chart's cached data,
     * exactly as @ref UpdateChartData does. Series names, the chart title, plot
     * type, and formatting are preserved; only the cached values change.
     *
     * @return `false` when this shape is not a chart, it has no embedded workbook,
     *         the workbook cannot be opened, or any series formula does not
     *         resolve to an existing sheet and range in that workbook. The chart
     *         is left unchanged when refreshing fails.
     */
    bool RefreshChartDataFromEmbeddedWorkbook();
    /**
     * @return Raw XML of this chart's chart-style part (`c:chartStyle`), or
     *         `std::nullopt` when the chart has no style part or this shape is not a chart.
     * @note The XML is returned and accepted verbatim; this API does not interpret
     *       the DrawingML 2013 chart-style schema.
     */
    std::optional<std::string> GetChartStyleXml() const;
    /**
     * @brief Creates or replaces this chart's chart-style part from raw XML.
     * @return `false` when this shape is not a chart.
     */
    bool SetChartStyleXml(const std::string& xml);
    /**
     * @return Raw XML of this chart's color-style part (`cs:colorStyle`), or
     *         `std::nullopt` when the chart has no color-style part or this shape is not a chart.
     * @note The XML is returned and accepted verbatim; this API does not interpret
     *       the DrawingML 2013 chart-style schema.
     */
    std::optional<std::string> GetChartColorStyleXml() const;
    /**
     * @brief Creates or replaces this chart's color-style part from raw XML.
     * @return `false` when this shape is not a chart.
     */
    bool SetChartColorStyleXml(const std::string& xml);
    /** @brief Inserts an empty row and updates merge coordinates/spans. */
    bool InsertTableRow(Size index, const MeasuringUnits& height = {});
    /** @brief Removes a row and updates or removes affected merged regions. */
    bool RemoveTableRow(Size index);
    /** @brief Inserts an empty column and updates merge coordinates/spans. */
    bool InsertTableColumn(Size index, const MeasuringUnits& width = {});
    /** @brief Removes a column and updates or removes affected merged regions. */
    bool RemoveTableColumn(Size index);
    /** @brief Adds one non-overlapping merged region to the table. */
    bool MergeTableCells(Size row, Size column,
                         Size rowSpan, Size columnSpan);
    /** @brief Removes the merged region whose anchor is at the specified cell. */
    bool UnmergeTableCells(Size row, Size column);
    /**
     * @brief Removes this shape from its current tree using the safe Warn policy.
     * @return True only when removed; false also reports animation dependencies non-destructively.
     * @note Use RemoveWithAnimationPolicy() to distinguish a dependency warning from a detached wrapper.
     */
    bool Remove();
    /**
     * @brief Removes this shape using an explicit animation-dependency policy.
     * @return A removal result; Warn leaves the shape intact when animations target it.
     */
    PresentationShapeRemovalResult RemoveWithAnimationPolicy(PresentationAnimationRemovalPolicy policy);

private:
    friend class PresentationShapeTree;
    PresentationShape(std::shared_ptr<OpenXMLElement> element,
                      std::shared_ptr<Packaging::SlidePart> slidePart = nullptr);
    std::shared_ptr<OpenXMLElement> m_element;
    std::shared_ptr<Packaging::SlidePart> m_slidePart;
};

/**
 * @brief Structural view of a slide's `p:spTree`.
 *
 * Shape order is the XML child order and therefore the presentation stacking
 * order: index zero is furthest back and the final item is furthest forward.
 * Required non-drawable group metadata is never exposed as a shape.
 */
class EXYOKIOFFICE_EXPORT PresentationShapeTree
{
public:
    using Ptr = std::shared_ptr<PresentationShapeTree>;

    /** @return Direct drawable children in exact XML order. */
    std::vector<PresentationShape::Ptr> Shapes() const;
    /** @return Number of direct drawable children. */
    Size Count() const;
    /** @return Shape at index, or `nullptr` when out of range. */
    PresentationShape::Ptr Get(Size index) const;
    /**
     * @brief Appends a minimal, valid `p:sp` shape at the front of the z-order.
     * @param name Non-visual shape name. An automatic name is used when empty.
     * @return The new shape, or `nullptr` when the tree is unavailable.
     */
    PresentationShape::Ptr AddShape(std::string name = {});
    /**
     * @brief Appends a minimal connector shape with line preset geometry.
     * @return The new connector, or nullptr when the shape tree is unavailable.
     */
    PresentationShape::Ptr AddConnector(std::string name = {});
    /**
     * @brief Appends a picture backed by embedded bytes or an unresolved external URI.
     * @return The new picture wrapper, or `nullptr` when the input is invalid or no slide part is available.
     */
    PresentationShape::Ptr AddPicture(const PresentationPictureData& picture);
    /**
     * @brief Adds an embedded PNG, JPEG, GIF, or BMP detected from its bytes.
     *
     * A zero extent in @p transform is replaced by the image's physical size
     * calculated from its pixel dimensions and DPI; a non-zero extent is retained.
     * @return The new picture, or nullptr when the bytes are not a supported image or no slide part is available.
     */
    PresentationShape::Ptr AddPictureFromData(
        std::vector<Byte> data,
        const PresentationShapeTransform& transform = {},
        std::string name = {});
    /**
     * @brief Reads and inserts an image file using signature-based format detection.
     * @return The picture wrapper, or `nullptr` for an unreadable/unsupported file.
     */
    PresentationShape::Ptr AddPictureFromFile(
        const std::filesystem::path& path,
        const PresentationShapeTransform& transform = {},
        std::string name = {});
    /**
     * @brief Appends an audio or video picture with an embedded or linked source.
     * @return The new media shape, or `nullptr` when the input is invalid.
     */
    PresentationShape::Ptr AddMedia(const PresentationMediaData& media);
    /**
     * @brief Reads and embeds an audio or video file without decoding or transcoding it.
     * @param kind Audio or video PresentationML marker to create.
     * @param path File whose bytes are stored verbatim.
     * @param contentType IANA media type such as `audio/mpeg` or `video/mp4`.
     * @param transform Position, size, rotation, and flip metadata.
     * @param posterFrame Optional preview image, normally used for video.
     * @param playback Initial playback behavior.
     * @param name Non-visual object name; generated automatically when empty.
     * @return The new media shape, or nullptr for an unreadable or unsupported file.
     */
    PresentationShape::Ptr AddMediaFromFile(
        PresentationMediaKind kind,
        const std::filesystem::path& path,
        std::string contentType,
        const PresentationShapeTransform& transform,
        std::optional<PresentationEmbeddedPicture> posterFrame = std::nullopt,
        const PresentationMediaPlayback& playback = {},
        std::string name = {});
    /**
     * @brief Appends a validated DrawingML table graphic frame to the shape tree.
     * @return The new graphic frame, or nullptr when the table data is invalid or
     * the shape tree is unavailable.
     */
    PresentationShape::Ptr AddTable(const PresentationTableData& table);
    /**
     * @brief Appends a chart graphic frame backed by a new chart part with cached data.
     * @param chart Chart type, formatting, series, and anchor transform. At least one
     *              series with values is required.
     * @return The new chart shape, or `nullptr` when the definition is invalid or no
     *         slide part is available.
     * @note The chart part stores the supplied values as its display cache. Use
     *       @ref PresentationShape::SetChartEmbeddedWorkbook to attach an editable workbook.
     */
    PresentationShape::Ptr AddChart(const PresentationChartDefinition& chart);
    /** @brief Removes the shape at index. */
    bool Remove(Size index);
    /**
     * @brief Groups two or more direct shapes.
     * @param indices Direct-child indices; duplicates and invalid indices fail the operation.
     * @return The new group at the lowest selected position, or `nullptr` on failure.
     * @note Children retain their original relative XML order.
     */
    PresentationShape::Ptr Group(const std::vector<Size>& indices);
    /**
     * @brief Dissolves the group at @p index, promoting its children into this tree.
     *
     * The group's direct drawable children replace the group at its stacking
     * position, preserving both their relative z-order and their absolute
     * placement: each child's position and size are re-projected from the group's
     * child coordinate system (`a:chOff`/`a:chExt`) into this tree's coordinate
     * system. Nested groups are promoted intact rather than recursively dissolved.
     *
     * @param index Zero-based position of a group shape in this tree.
     * @return `true` when a group was dissolved; `false` when @p index is out of
     *         range or does not identify a group. The tree is unchanged on failure.
     * @note Group-level rotation and flip are not baked into the promoted children.
     * @note The XML nodes are rebuilt; reacquire shape wrappers after a successful call.
     */
    bool Ungroup(Size index);
    /**
     * @brief Moves a shape to an absolute z-order index.
     * @note The XML nodes are rebuilt; reacquire shape wrappers after a successful move.
     */
    bool Move(Size fromIndex, Size toIndex);
    /** @brief Moves a shape one level toward the viewer. */
    bool BringForward(Size index);
    /** @brief Moves a shape one level away from the viewer. */
    bool SendBackward(Size index);
    /** @brief Moves a shape to the front of the stacking order. */
    bool BringToFront(Size index);
    /** @brief Moves a shape to the back of the stacking order. */
    bool SendToBack(Size index);

private:
    friend class PresentationSlide;
    PresentationShapeTree(std::shared_ptr<OpenXMLElement> tree,
                          std::shared_ptr<Packaging::SlidePart> slidePart);
    std::shared_ptr<OpenXMLElement> m_tree;
    std::shared_ptr<Packaging::SlidePart> m_slidePart;
};

/**
 * @brief Represents a placeholder shape declared by a slide, layout, or master.
 *
 * A placeholder is matched during inheritance by its explicit `p:ph/@idx` when
 * present, and otherwise by its `p:ph/@type`. The wrapper exposes the underlying
 * shape for advanced DrawingML editing while keeping common identity operations
 * available without raw XML traversal.
 */
class EXYOKIOFFICE_EXPORT PresentationPlaceholder
{
public:
    using Ptr = std::shared_ptr<PresentationPlaceholder>;

    /** @return The level at which this placeholder is physically declared. */
    PlaceholderOrigin Origin() const noexcept;
    /** @return The placeholder type; omitted `type` attributes use the OOXML `Object` default. */
    DocumentFormat::OpenXml::Presentation::PlaceholderValues::Value Type() const;
    /** @return The explicit placeholder index, or `std::nullopt` when omitted. */
    std::optional<UInt32> Index() const;
    /**
     * @return The low-level `p:sp` element, or `nullptr` when the placeholder
     *         belongs to another valid host such as `p:pic` or `p:graphicFrame`.
     */
    std::shared_ptr<DocumentFormat::OpenXml::Presentation::Shape> GetShape() const;
    /**
     * @return The physical shape-like host element containing `p:ph`, or
     * nullptr when this wrapper is detached.
     *
     * This is the lossless accessor for placeholders hosted by `p:sp`, `p:pic`,
     * `p:graphicFrame`, connectors, or future PresentationML host elements.
     */
    std::shared_ptr<OpenXMLElement> GetElement() const;
    /**
     * @brief Removes the declared placeholder shape from its owning XML tree.
     * @return `true` when the shape was attached and removed.
     * @note Removing an inherited wrapper removes it from the level reported by Origin().
     */
    bool Remove();

private:
    friend class PresentationSlide;
    friend class PresentationSlideLayout;
    friend class PresentationSlideMaster;
    friend class PowerPointDocumentEditor;
    friend class PresentationHierarchyHelpers;
    PresentationPlaceholder(std::shared_ptr<OpenXMLElement> element,
                            std::shared_ptr<DocumentFormat::OpenXml::Presentation::PlaceholderShape> placeholder,
                            PlaceholderOrigin origin);

    std::shared_ptr<OpenXMLElement> m_element;
    std::shared_ptr<DocumentFormat::OpenXml::Presentation::PlaceholderShape> m_placeholder;
    PlaceholderOrigin m_origin;
};

/**
 * Ordered semantic positions in a DrawingML theme color scheme.
 *
 * The theme model is shared by every Office family; these aliases keep the
 * established PowerPoint spelling while the definitions live in
 * `ExyokiOffice/ThemeService.hpp`.
 */
using PresentationThemeColorSlot = ExyokiOffice::ThemeColorSlot;

/** Typeface selection for one major or minor DrawingML font collection. */
using PresentationThemeFontCollection = ExyokiOffice::ThemeFontCollection;

/**
 * @brief Essential, strongly typed DrawingML theme settings.
 *
 * Colors are stored in the fixed OOXML scheme order represented by
 * PresentationThemeColorSlot. Reading resolves system colors through their
 * `lastClr` fallback. Applying the model writes explicit sRGB colors while
 * preserving the existing format/effect style matrix and extension elements.
 */
using PresentationThemeSettings = ExyokiOffice::ThemeSettings;

/** @brief High-level wrapper for a slide master registered by the presentation. */
class EXYOKIOFFICE_EXPORT PresentationSlideMaster
{
public:
    using Ptr = std::shared_ptr<PresentationSlideMaster>;

    /** @return The persistent `p:sldMasterId` value. */
    UInt32 Id() const;
    /** @return The master name stored on `p:cSld`, or an empty string. */
    std::string Name() const;
    /** @return Layouts in the order declared by this master's layout ID list. */
    std::vector<std::shared_ptr<PresentationSlideLayout>> Layouts() const;
    /** @return Placeholders declared directly by the master. */
    std::vector<PresentationPlaceholder::Ptr> Placeholders() const;
    /**
     * @brief Appends a placeholder shape to the master.
     * @param type PresentationML placeholder semantic type.
     * @param index Optional inheritance key; use the same index at lower levels to override it.
     * @return Wrapper for the new placeholder, or `nullptr` if the master XML is unavailable.
     */
    PresentationPlaceholder::Ptr AddPlaceholder(
        DocumentFormat::OpenXml::Presentation::PlaceholderValues::Value type,
        std::optional<UInt32> index = std::nullopt);
    /**
     * @return The complete DrawingML theme XML applied by this master, or
     *         `std::nullopt` when the master has no theme relationship.
     *
     * Returning the lossless XML representation preserves custom color schemes,
     * font schemes, effects, and vendor extensions that are not modeled by the
     * high-level API.
     */
    std::optional<std::string> ThemeXml() const;
    /**
     * @brief Creates or replaces the DrawingML theme used by this master.
     * @param xml A complete, well-formed `a:theme` document.
     * @return `true` when the theme was parsed and attached.
     * @note The operation is transactional: invalid XML leaves the current theme unchanged.
     */
    bool SetThemeXml(std::string xml);
    /**
     * @return Essential typed theme settings, or `std::nullopt` when the theme
     *         is missing or does not contain a complete color/font/style scheme.
     */
    std::optional<PresentationThemeSettings> ThemeSettings() const;
    /**
     * @brief Applies essential theme colors, fonts, and scheme names.
     * @return `false` when required names, typefaces, or colors are invalid.
     * @note Existing fill, line, effect, background-fill, and extension XML is preserved.
     */
    bool SetThemeSettings(const PresentationThemeSettings& settings);
    /**
     * @brief Removes the master's explicit theme relationship.
     * @return `true` when a theme was present and removed.
     */
    bool RemoveTheme();
    /** @return The low-level master part, or nullptr when this wrapper is detached. */
    std::shared_ptr<Packaging::SlideMasterPart> GetPart() const;

private:
    friend class PowerPointDocumentEditor;
    friend class PresentationHierarchyHelpers;
    PresentationSlideMaster(std::shared_ptr<Packaging::SlideMasterPart> part,
                            std::shared_ptr<DocumentFormat::OpenXml::Presentation::SlideMasterId> entry);

    std::shared_ptr<Packaging::SlideMasterPart> m_part;
    std::shared_ptr<DocumentFormat::OpenXml::Presentation::SlideMasterId> m_entry;
};

/** @brief High-level wrapper for a slide layout owned by a slide master. */
class EXYOKIOFFICE_EXPORT PresentationSlideLayout
{
public:
    using Ptr = std::shared_ptr<PresentationSlideLayout>;

    /** @return The persistent layout identifier from the master's `p:sldLayoutId`. */
    UInt32 Id() const;
    /** @return The human-readable layout name from `p:cSld`. */
    std::string Name() const;
    /** @return The declared PresentationML layout type, or `Custom` when absent or invalid. */
    DocumentFormat::OpenXml::Presentation::SlideLayoutValues::Value Type() const;
    /**
     * @return The master from which this layout inherits theme and placeholders,
     * or nullptr when this layout wrapper is detached.
     */
    PresentationSlideMaster::Ptr Master() const;
    /**
     * @brief Returns placeholders visible at the layout level.
     * @param includeInherited Include non-overridden master placeholders when true.
     * @return Direct layout placeholders followed by inherited master placeholders.
     */
    std::vector<PresentationPlaceholder::Ptr> Placeholders(bool includeInherited = true) const;
    /**
     * @brief Appends a directly declared layout placeholder.
     * @return The new placeholder, or nullptr when the layout part is unavailable.
     */
    PresentationPlaceholder::Ptr AddPlaceholder(
        DocumentFormat::OpenXml::Presentation::PlaceholderValues::Value type,
        std::optional<UInt32> index = std::nullopt);
    /** @return The low-level layout part, or nullptr when this wrapper is detached. */
    std::shared_ptr<Packaging::SlideLayoutPart> GetPart() const;

private:
    friend class PowerPointDocumentEditor;
    friend class PresentationHierarchyHelpers;
    PresentationSlideLayout(std::shared_ptr<Packaging::SlideLayoutPart> part,
                            std::shared_ptr<DocumentFormat::OpenXml::Presentation::SlideLayoutId> entry,
                            std::shared_ptr<PresentationSlideMaster> master);

    std::shared_ptr<Packaging::SlideLayoutPart> m_part;
    std::shared_ptr<DocumentFormat::OpenXml::Presentation::SlideLayoutId> m_entry;
    std::shared_ptr<PresentationSlideMaster> m_master;
};

/**
 * @brief Lightweight high-level wrapper for a slide registered in a presentation.
 *
 * A PresentationSlide references both the slide part and its `p:sldId` entry.
 * Mutations are applied directly to the owning package. The wrapper remains
 * valid while those underlying nodes remain attached to the document.
 */
class EXYOKIOFFICE_EXPORT PresentationSlide
{
public:
    using Ptr = std::shared_ptr<PresentationSlide>;

    /** @return The persistent numeric slide identifier from `p:sldId`. */
    UInt32 Id() const;
    /** @return The relationship identifier linking the presentation to the slide part. */
    std::string RelationshipId() const;
    /** @return True when the slide is excluded from slide-show playback. */
    [[nodiscard]] bool IsHidden() const;
    /**
     * @brief Includes or excludes this slide from slide-show playback.
     * @param hidden True to hide the slide; false to show it.
     * @return True when the slide XML was updated.
     */
    bool SetHidden(bool hidden);
    /** @return The slide transition, or `std::nullopt` when none is declared. */
    std::optional<PresentationTransitionData> GetTransition() const;
    /**
     * @brief Creates or replaces the slide transition, its options, and its optional sound.
     *
     * The write is transactional: when the effect rejects one of the supplied
     * @ref PresentationTransitionOptions members, or the sound payload is empty,
     * the slide is left exactly as it was.
     *
     * @return `false` for invalid sound data, options the effect does not accept,
     *         or an Unsupported effect without an existing opaque effect.
     * @note Unsupported existing effects are retained while common timing and advance metadata is edited.
     */
    bool SetTransition(const PresentationTransitionData& transition);
    /** @brief Removes the transition, its sound relationship, and any opaque effect subtree. */
    bool RemoveTransition();
    /**
     * @return Free-standing `p:anim` behaviors directly below `p:timing/p:tnLst`, in document order.
     * @note Media, animation sequences, and unknown timing nodes remain untouched and are not returned.
     */
    std::vector<PresentationAnimationNode> Animations() const;
    /**
     * @brief Adds a validated animation and returns its stable timing-node ID.
     * @note A zero input ID is allocated automatically; explicit IDs must be unique across the timing tree.
     */
    std::optional<UInt32> AddAnimation(const PresentationAnimationNode& animation);
    /**
     * @brief Replaces an animation selected by its stable timing-node ID.
     * @return False when the node is missing, its target shape does not exist, or the input attempts to change its ID.
     */
    bool UpdateAnimation(UInt32 id, const PresentationAnimationNode& animation);
    /** @brief Removes an animation selected by its stable timing-node ID. */
    bool RemoveAnimation(UInt32 id);
    /**
     * @brief Reads the slide's animation effects in playback order.
     *
     * Main-sequence effects come first, followed by the effects of every
     * interactive (shape-triggered) sequence in document order. Effects the API
     * does not model are reported with @ref PresentationAnimationEffect::Unsupported
     * and keep their position, so a read/modify/write round trip preserves them.
     *
     * @return The modelled effects, or an empty vector when the slide has no sequences.
     */
    std::vector<PresentationAnimationEffectData> AnimationEffects() const;
    /**
     * @brief Rebuilds the slide's main and interactive animation sequences from a list.
     *
     * The whole timing tree below `p:timing` is regenerated: a timing root, one
     * main sequence for effects whose @ref PresentationAnimationEffectData::TriggerShapeId
     * is zero, and one interactive sequence per run of consecutive effects sharing
     * a non-zero trigger shape. Within a sequence, an
     * @ref PresentationAnimationTrigger::OnClick effect opens a new click group,
     * @ref PresentationAnimationTrigger::WithPrevious joins the current group, and
     * @ref PresentationAnimationTrigger::AfterPrevious opens a new timing group inside
     * the current click group. The first effect of every sequence always waits for
     * that sequence's own trigger regardless of its @ref PresentationAnimationTrigger.
     *
     * Free-standing `p:anim` behaviors managed through @ref AddAnimation and media
     * timing nodes are preserved, as are effects read back as
     * @ref PresentationAnimationEffect::Unsupported.
     *
     * Every effect is validated before anything is written: the target shape and
     * any trigger shape must exist on this slide, the effect and class must form a
     * supported pair, effect-specific parameters must match the effect exactly,
     * timing must be in range, and explicit non-zero IDs must be unique. A single
     * invalid effect leaves the slide untouched.
     *
     * A presentation carries a single main sequence, so effects are first
     * regrouped into main-sequence effects followed by one run per trigger shape
     * in order of first appearance. Relative order inside each group is kept, and
     * @ref AnimationEffects reports exactly that regrouped order.
     *
     * @param effects Effects in the desired playback order; an empty list clears all sequences.
     * @return True when the sequences were rebuilt.
     */
    bool SetAnimationEffects(const std::vector<PresentationAnimationEffectData>& effects);
    /**
     * @brief Appends one animation effect to the end of the slide's effect list.
     * @return The stable effect ID, or `std::nullopt` when the effect is invalid.
     */
    std::optional<UInt32> AddAnimationEffect(const PresentationAnimationEffectData& effect);
    /**
     * @brief Replaces the effect carrying the given stable effect ID.
     * @return False when no such effect exists, the replacement is invalid, or it attempts to change its ID.
     */
    bool UpdateAnimationEffect(UInt32 id, const PresentationAnimationEffectData& effect);
    /** @brief Removes the effect carrying the given stable effect ID. */
    bool RemoveAnimationEffect(UInt32 id);
    /**
     * @brief Moves the effect carrying the given stable effect ID to another playback position.
     * @param id Stable effect ID to move.
     * @param index Zero-based destination index within the full effect list.
     * @return False when the ID is unknown or the index is out of range.
     */
    bool MoveAnimationEffect(UInt32 id, Size index);
    /** @brief Removes every animation sequence, leaving free-standing behaviors and media timing intact. */
    bool ClearAnimationEffects();
    /** @return The currently assigned layout, or `nullptr` when no layout relationship exists. */
    PresentationSlideLayout::Ptr Layout() const;
    /**
     * @brief Returns effective slide placeholders with PresentationML inheritance applied.
     *
     * Direct slide placeholders are returned first. Layout placeholders with the
     * same index (or, when index is absent, the same type) are suppressed, then
     * non-overridden master placeholders are appended.
     */
    std::vector<PresentationPlaceholder::Ptr> Placeholders(bool includeInherited = true) const;
    /**
     * @brief Appends a placeholder declared directly by this slide.
     * @return The new placeholder, or nullptr when the slide part is unavailable.
     */
    PresentationPlaceholder::Ptr AddPlaceholder(
        DocumentFormat::OpenXml::Presentation::PlaceholderValues::Value type,
        std::optional<UInt32> index = std::nullopt);
    /** @return Structural wrapper for this slide's shape tree, or `nullptr` for malformed slide XML. */
    PresentationShapeTree::Ptr ShapeTree() const;
    /** @return Plain text stored in the slide's notes body, or an empty string when no notes exist. */
    std::string NotesText() const;
    /** @return The complete notes-page model, or `std::nullopt` when the slide has no notes part. */
    std::optional<PresentationNotesPage> NotesPage() const;
    /**
     * @brief Creates or replaces the slide's notes page.
     * @return `false` when the notes part or its XML content cannot be written.
     */
    bool SetNotesPage(const PresentationNotesPage& page);
    /**
     * @brief Creates or replaces the slide notes body.
     * @note Newline characters are represented as separate DrawingML paragraphs.
     */
    bool SetNotesText(const std::string& text);
    /** @brief Removes the notes-slide relationship and its package part. */
    bool RemoveNotes();
    /** @return Modern comments attached to this slide in part and XML order. */
    std::vector<PresentationComment> Comments() const;
    /** @brief Adds a modern comment after validating author and identifier uniqueness. */
    bool AddComment(const PresentationComment& comment);
    /** @brief Replaces a modern comment while retaining its package-part relationship. */
    bool UpdateComment(std::string_view id, const PresentationComment& comment);
    /**
     * @brief Changes the lifecycle state of an existing modern comment thread.
     * @param id Stable identifier of the root comment.
     * @param status New thread state.
     * @return `true` when the comment was found and updated.
     */
    bool SetCommentStatus(std::string_view id, PresentationCommentStatus status);
    /** @brief Removes a modern comment and removes an empty comment part. */
    bool RemoveComment(std::string_view id);
    /**
     * @return The generated low-level part containing this slide's XML, or nullptr
     * when this wrapper is detached.
     */
    std::shared_ptr<Packaging::SlidePart> GetPart() const;

private:
    friend class PowerPointDocumentEditor;
    PresentationSlide(std::shared_ptr<Packaging::SlidePart> part,
                      std::shared_ptr<DocumentFormat::OpenXml::Presentation::SlideId> entry);

    std::shared_ptr<Packaging::SlidePart> m_part;
    std::shared_ptr<DocumentFormat::OpenXml::Presentation::SlideId> m_entry;
};

/**
 * @brief Current PowerPoint modify-protection state.
 *
 * Modify protection is the "password to modify" recorded in
 * `p:modifyVerifier`. PowerPoint opens such a presentation read-only unless the
 * password is supplied. It is a user-interface restriction only: the package
 * remains plain, readable OOXML, so this is not encryption and provides no
 * confidentiality. The stored verifier itself is deliberately not exposed.
 */
struct EXYOKIOFFICE_EXPORT PresentationModifyProtectionInfo
{
    /** True when a password verifier is stored on the presentation. */
    bool HasPassword = false;
    /** True when the verifier uses an algorithm this library can validate. */
    bool VerifierSupported = false;
    bool operator==(const PresentationModifyProtectionInfo&) const = default;
};

/** @brief Error code returned by PowerPoint modify-protection operations. */
enum class PresentationProtectionError
{
    None,
    InvalidPresentation,
    InvalidPassword,
    PasswordMismatch,
    UnsupportedVerifier,
    WriteFailed
};

/** @brief Structured result of protecting or unprotecting a presentation. */
struct EXYOKIOFFICE_EXPORT PresentationProtectionResult
{
    PresentationProtectionError Error = PresentationProtectionError::None;
    std::string Message;

    /** @brief Returns true when the requested operation completed. */
    [[nodiscard]] bool Succeeded() const noexcept { return Error == PresentationProtectionError::None; }
    /** @brief Provides convenient success testing in conditional statements. */
    [[nodiscard]] explicit operator bool() const noexcept { return Succeeded(); }
};

/**
 * @brief High-level entry point for creating, opening, and saving presentations.
 *
 * The editor holds a `std::shared_ptr<PowerPointDocument>` and keeps the
 * package in memory until it is saved explicitly.
 *
 * It supplies package-safe operations over the slide, master, layout, and
 * placeholder hierarchy, plus slide size and sections, custom shows, speaker
 * notes, comments, transitions and animation timing, modify protection, and
 * slide copying between presentations. Slide content is authored through
 * PresentationSlide and its PresentationShapeTree: shapes and transforms,
 * text frames, DrawingML tables, pictures and media, charts, and embedded
 * objects. CreateSlideBuilder() offers a fluent shortcut for the common
 * "title plus a few boxes" slide.
 *
 * @par Thread safety
 * An editor, its slides, and every wrapper they hand out are **not**
 * thread-safe. All mutating calls on one presentation, and any read that races
 * with such a call, must be externally serialized. Distinct editors over
 * distinct presentations may be used concurrently; schema metadata is
 * immutable and shared safely.
 *
 * @code
 * auto editor = ExyokiOffice::PowerPoint::PowerPointDocumentEditor::CreateNew();
 * editor->SaveToFile("empty.pptx");
 * @endcode
 */
class EXYOKIOFFICE_EXPORT PowerPointDocumentEditor
{
public:
    using Ptr = std::shared_ptr<PowerPointDocumentEditor>;

    /**
     * @brief Fluent slide-authoring operation bound to a PowerPointDocumentEditor.
     *
     * Instances are created by CreateSlideBuilder(). Build() appends a slide to
     * that editor and is transactional at slide granularity. A builder may be
     * reused to create multiple slides in the same presentation. Elements are
     * authored in title, text-box, then preset-shape order, which also defines
     * their back-to-front z-order.
     *
     * @code
     * using ExyokiOffice::MeasurementUnit;
     * using ExyokiOffice::MeasuringUnits;
     * auto slide = editor->CreateSlideBuilder()
     *     .SetTitle("Quarterly report",
     *               {{MeasuringUnits(1.0, MeasurementUnit::Inch), MeasuringUnits(0.5, MeasurementUnit::Inch)},
     *                {MeasuringUnits(8.0, MeasurementUnit::Inch), MeasuringUnits(0.75, MeasurementUnit::Inch)}})
     *     .AddTextBox("Prepared for the board",
     *                 {{MeasuringUnits(1.0, MeasurementUnit::Inch), MeasuringUnits(1.5, MeasurementUnit::Inch)},
     *                  {MeasuringUnits(8.0, MeasurementUnit::Inch), MeasuringUnits(2.0, MeasurementUnit::Inch)}})
     *     .Build();
     * @endcode
     */
    class EXYOKIOFFICE_EXPORT SlideBuilder
    {
    public:
        /** @brief Assigns a destination-owned layout; pass nullptr for no layout. */
        SlideBuilder& SetLayout(const PresentationSlideLayout::Ptr& layout);
        /** @brief Controls whether the authored slide participates in slide-show playback. */
        SlideBuilder& SetHidden(bool hidden = true);
        /** @brief Adds or replaces a UTF-8 plain-text title and its unit-aware physical transform. */
        SlideBuilder& SetTitle(std::string title, const PresentationShapeTransform& transform = {});
        /** @brief Removes a previously configured title from this operation. */
        SlideBuilder& ClearTitle();
        /** @brief Appends a UTF-8 text box; newlines produce separate DrawingML paragraphs. */
        SlideBuilder& AddTextBox(std::string text, const PresentationShapeTransform& transform,
                                 std::string name = {});
        /**
         * @brief Appends a richly formatted DrawingML text frame.
         * @param frame Paragraphs, runs, bullets, tabs, spacing, fonts, colors, and body layout.
         * @param transform Unit-aware physical position and extent.
         * @param name Optional non-visual shape name.
         */
        SlideBuilder& AddTextBox(PresentationTextFrame frame, const PresentationShapeTransform& transform,
                                 std::string name = {});
        /** @brief Appends a standard DrawingML preset shape with a unit-aware physical transform. */
        SlideBuilder& AddShape(DocumentFormat::OpenXml::Drawing::ShapeTypeValues::Value type,
                               const PresentationShapeTransform& transform, std::string name = {});
        /** @brief Removes configured text boxes and shapes while retaining title, layout, and hidden state. */
        SlideBuilder& ClearContent();
        /** @brief Appends the completed slide, or returns `nullptr` without retaining a partial slide. */
        PresentationSlide::Ptr Build() const;

    private:
        friend class PowerPointDocumentEditor;
        explicit SlideBuilder(PowerPointDocumentEditor* editor);
        struct TextBoxInstruction
        {
            PresentationTextFrame Frame;
            PresentationShapeTransform Transform;
            std::string Name;
        };
        struct ShapeInstruction
        {
            DocumentFormat::OpenXml::Drawing::ShapeTypeValues::Value Type;
            PresentationShapeTransform Transform;
            std::string Name;
        };

        PowerPointDocumentEditor* m_editor;
        PresentationSlideLayout::Ptr m_layout;
        bool m_hidden = false;
        std::optional<TextBoxInstruction> m_title;
        std::vector<TextBoxInstruction> m_textBoxes;
        std::vector<ShapeInstruction> m_shapes;
    };

    PowerPointDocumentEditor() = default;
    ~PowerPointDocumentEditor();
    /** @brief Wraps an existing low-level document and keeps it alive. */
    explicit PowerPointDocumentEditor(const PowerPointDocument::Ptr& document);

    /**
     * @brief Creates an editor, optionally wrapping an existing document.
     *
     * Unlike CreateNew() and Open() this never fails: passing nullptr yields an
     * editor with no attached document, which SetDocument() can fill in later.
     */
    static Ptr Create(const PowerPointDocument::Ptr& document = nullptr);
    /**
     * @brief Creates a minimally initialized presentation of the requested flavor.
     * @return The new editor, or nullptr when the presentation could not be initialized.
     */
    static Ptr CreateNew(PowerPointDocumentType type = PowerPointDocumentType::Presentation);
    /**
     * @brief Opens a presentation package from disk.
     * @return The editor, or nullptr when the package cannot be read, exceeds the
     * configured OpenSettings::PackageLimits, or is not a PresentationML package.
     */
    static Ptr Open(const std::filesystem::path& path, const Packaging::OpenSettings& settings = {},
                    const ICancellationToken* cancellationToken = nullptr,
                    Packaging::OpenError* error = nullptr);
    /**
     * @brief Opens a presentation package from a byte buffer.
     * @return The editor, or nullptr when the buffer is not a readable PresentationML package.
     */
    static Ptr Open(const std::vector<Byte>& bytes, const Packaging::OpenSettings& settings = {},
                    const ICancellationToken* cancellationToken = nullptr,
                    Packaging::OpenError* error = nullptr);
    /**
     * @brief Opens a presentation package from a contiguous byte range.
     * @return The editor, or nullptr when the range is not a readable PresentationML package.
     */
    static Ptr Open(std::span<const Byte> bytes, const Packaging::OpenSettings& settings = {},
                    const ICancellationToken* cancellationToken = nullptr,
                    Packaging::OpenError* error = nullptr);

    /** @brief Saves the package, atomically replacing an existing file by default. */
    bool SaveToFile(const std::filesystem::path& path, bool atomicSave = true,
                    const ICancellationToken* cancellationToken = nullptr);
    /** @brief Serializes the package to an in-memory OPC ZIP buffer. */
    std::vector<Byte> SaveToMemory(const ICancellationToken* cancellationToken = nullptr);

    /**
     * @brief Captures the complete presentation package as an immutable edit memento.
     *
     * Slides, masters, layouts, notes, comments, relationships, media, charts,
     * embedded objects, and all other package state are included.
     *
     * @param cancellationToken Optional non-owning token used to cancel serialization.
     * @return Complete package memento, or `std::nullopt` on failure.
     */
    std::optional<DocumentEditMemento> CreateMemento(
        const ICancellationToken* cancellationToken = nullptr);

    /**
     * @brief Restores a complete presentation from a PowerPoint-family memento.
     *
     * Successful restoration replaces the low-level document graph. Slide,
     * shape, part, and DOM wrappers obtained before this call must be reacquired.
     *
     * @param memento Snapshot to restore.
     * @param cancellationToken Optional non-owning token used to cancel package loading.
     * @return True when the complete presentation was reopened and installed.
     */
    bool RestoreMemento(const DocumentEditMemento& memento,
                        const ICancellationToken* cancellationToken = nullptr);

    /**
     * @brief Starts an all-or-nothing presentation edit transaction.
     *
     * Destruction rolls back automatically unless Commit() has accepted the
     * changes. An inactive result indicates that the initial snapshot failed.
     * The transaction must not outlive this editor.
     */
    DocumentEditTransaction BeginTransaction(
        const ICancellationToken* cancellationToken = nullptr);

    /** @brief Replaces the document owned by this editor. */
    void SetDocument(const PowerPointDocument::Ptr& document);
    /** @return The currently owned low-level document, or nullptr when none is attached. */
    PowerPointDocument::Ptr GetDocument() const;
    /** @return The low-level package API; equivalent to GetDocument(), including its nullptr case. */
    PowerPointDocument::Ptr GetLowLevelApi() const;

    /**
     * @brief Returns the unified core/extended/custom document-properties editor.
     *
     * The returned editor is a lightweight view over the attached document;
     * it stays valid while this editor keeps the document alive. Calling this
     * without an attached document is undefined; check GetDocument() first
     * when the editor may be empty.
     */
    Packaging::DocumentProperties Properties() const;

    /**
     * @brief Reads the PowerPoint modify-protection state.
     *
     * @return Protection information, or `std::nullopt` when the presentation
     *         has no `p:modifyVerifier` element.
     */
    std::optional<PresentationModifyProtectionInfo> GetModifyProtection() const;

    /**
     * @brief Requires a password before PowerPoint allows modifications.
     *
     * PowerPoint opens a presentation carrying this verifier read-only unless
     * the password is supplied. The restriction is enforced by the application,
     * not by cryptography: the package stays plain OOXML and any consumer may
     * ignore or strip the verifier. Use package encryption when the content
     * itself must be unreadable; this API does not provide it.
     *
     * The password is stored as an ISO/IEC 29500 salted, iterated SHA-512
     * verifier. Any existing `p:modifyVerifier` element is replaced atomically.
     *
     * @param password Non-empty password required to modify the presentation.
     * @return Structured result describing success or the reason for failure.
     */
    PresentationProtectionResult ProtectFromModification(std::string_view password);

    /**
     * @brief Removes modify protection after verifying its password.
     *
     * Removing protection from an unprotected presentation succeeds and changes
     * nothing. A presentation whose verifier uses an algorithm this library
     * cannot compute reports PresentationProtectionError::UnsupportedVerifier.
     *
     * @param password Password supplied when protection was applied.
     * @return Structured result describing success or the reason for failure.
     */
    PresentationProtectionResult UnprotectFromModification(std::string_view password);

    /** @return A fluent slide-authoring operation bound to this editor. */
    SlideBuilder CreateSlideBuilder();

    /**
     * @brief Appends a new blank slide to the presentation.
     *
     * The operation creates a SlidePart, assigns a unique relationship ID,
     * allocates a persistent slide ID of at least 256, and appends the matching
     * `p:sldId` entry. A layout can subsequently be assigned with SetSlideLayout().
     *
     * @return Wrapper for the new slide, or nullptr if creation fails.
     */
    PresentationSlide::Ptr AddSlide();
    /**
     * @brief Appends a slide authored from a reusable fluent recipe.
     * @return The completed slide, or `nullptr`; failure does not change the slide collection.
     * @see SlideBuilder::Build()
     */
    PresentationSlide::Ptr AddSlide(const SlideBuilder& builder);
    /**
     * @brief Creates an independent copy of a slide in this presentation.
     *
     * The slide XML, notes slide, charts, embedded objects, comments, and other
     * mutable descendants are deep-copied. Relationship identifiers inside the
     * copied graph are retained so references in DrawingML and PresentationML
     * continue to resolve. Immutable media, layout, master, and theme resources
     * may be shared safely within the same package.
     *
     * The copy is inserted immediately after its source, receives a new
     * persistent slide identifier and presentation relationship identifier,
     * and initially preserves the source slide's hidden state.
     *
     * @param index Zero-based index of the slide to copy.
     * @return Wrapper for the copied slide, or `nullptr` when the index is
     *         invalid or the complete graph cannot be copied. Failure leaves
     *         the slide collection unchanged.
     */
    PresentationSlide::Ptr CopySlide(Size index);
    /**
     * @brief Appends an independent copy of a slide from another presentation.
     *
     * The complete dependency graph is imported, including the assigned layout,
     * master, theme, media, charts, embedded workbooks, notes, and comments.
     * Package-local URIs and presentation-level identifiers are allocated in the
     * destination, while relationship identifiers referenced by imported XML are
     * preserved within their respective parts.
     *
     * @param source Editor that owns the source slide. It must wrap a different,
     *               valid presentation package.
     * @param index Zero-based source slide index.
     * @return The appended destination slide, or `nullptr` if validation or the
     *         complete import fails. The destination slide list is unchanged on
     *         failure.
     */
    PresentationSlide::Ptr CopySlideFrom(const PowerPointDocumentEditor& source,
                                         Size index);
    /** @return The slide at a zero-based presentation position, or nullptr. */
    PresentationSlide::Ptr GetSlide(Size index) const;
    /** @return All slides in the exact order stored in presentation XML. */
    std::vector<PresentationSlide::Ptr> Slides() const;
    /** @return The number of slides registered in presentation XML. */
    Size SlideCount() const;
    /**
     * @brief Moves a slide within presentation order without changing its ID or part.
     * @param fromIndex Existing zero-based position.
     * @param toIndex Destination zero-based position.
     * @return True when indices are valid and the requested order is established.
     */
    bool MoveSlide(Size fromIndex, Size toIndex);
    /**
     * @brief Reorders the complete slide collection from a permutation of old indices.
     * @param newOrder For every new position, the zero-based position it occupied before the call.
     * @return `true` when @p newOrder contains every current index exactly once.
     * @note Slide IDs, parts, relationships, and slide content are preserved.
     */
    bool ReorderSlides(const std::vector<Size>& newOrder);
    /**
     * @brief Removes a slide entry, relationship, and slide part together.
     * @param index Zero-based presentation position.
     * @return True when a slide was removed; false for an invalid index.
     */
    bool RemoveSlide(Size index);

    /** @return PowerPoint sections in stored order with slide IDs normalized to presentation order. */
    std::vector<PresentationSection> Sections() const;
    /** @brief Adds a section after validating identifiers and slide membership. */
    bool AddSection(const PresentationSection& section);
    /** @brief Replaces a section selected by its stable identifier. */
    bool UpdateSection(std::string_view id, const PresentationSection& section);
    /** @brief Removes a section without removing any slides. */
    bool RemoveSection(std::string_view id);

    /** @return Custom slide shows in stored order, preserving each explicit playback sequence. */
    std::vector<PresentationCustomShow> CustomShows() const;
    /** @brief Adds a custom show with a unique non-zero ID, name, and non-empty valid slide sequence. */
    bool AddCustomShow(const PresentationCustomShow& show);
    /** @brief Replaces a custom show selected by its stable identifier. */
    bool UpdateCustomShow(UInt32 id, const PresentationCustomShow& show);
    /** @brief Removes a custom show without removing any slides. */
    bool RemoveCustomShow(UInt32 id);

    /** @return Modern PowerPoint comment authors in stored order. */
    std::vector<PresentationCommentAuthor> CommentAuthors() const;
    /** @brief Adds an author with a non-empty, presentation-unique identifier. */
    bool AddCommentAuthor(const PresentationCommentAuthor& author);
    /** @brief Updates author metadata without changing the author identifier. */
    bool UpdateCommentAuthor(std::string_view id, const PresentationCommentAuthor& author);
    /**
     * @brief Removes an unused author.
     * @return `false` when a comment or reply still references the author.
     */
    bool RemoveCommentAuthor(std::string_view id);

    /**
     * @return Physical slide extent and size classification, or `std::nullopt`
     * when the presentation carries no explicit `p:sldSz`.
     */
    std::optional<PresentationSlideSize> GetSlideSize() const;
    /**
     * @brief Creates or replaces the presentation-wide slide size.
     *
     * Existing slides keep their own shape coordinates; PowerPoint rescales
     * nothing implicitly, so set the size before laying content out.
     *
     * @param size Positive extent representable as integral EMUs, plus an
     * optional size classification.
     * @return `false` when the extent is missing, non-positive, or out of the
     * PresentationML coordinate range, leaving the presentation unchanged.
     */
    bool SetSlideSize(const PresentationSlideSize& size);
    /** @brief Removes the explicit slide size, restoring PowerPoint's own default. */
    bool RemoveSlideSize();

    /** @return Handout-master formatting, or `std::nullopt` when no handout master exists. */
    std::optional<PresentationHandoutSettings> HandoutSettings() const;
    /**
     * @brief Creates or replaces presentation-wide handout-master formatting.
     * @note `PageSize` must contain positive dimensions representable as integral EMUs.
     */
    bool SetHandoutSettings(const PresentationHandoutSettings& settings);
    /** @brief Removes the handout master while retaining the shared notes/handout page size. */
    bool RemoveHandoutSettings();

    /**
     * @brief Creates and registers a minimal slide master.
     * @return The new master, or nullptr when the presentation part or its master
     * list is unavailable.
     */
    PresentationSlideMaster::Ptr AddSlideMaster(std::string name = {});
    /**
     * @brief Imports a complete slide-design graph from another presentation.
     *
     * The master, its layouts, theme, images, and all other internally related
     * parts are deep-copied into this document. No package parts are shared with
     * the source document.
     *
     * @param source Master returned by any PowerPointDocumentEditor.
     * @return The newly registered master, or `nullptr` when the source is invalid
     *         or the graph cannot be imported.
     */
    PresentationSlideMaster::Ptr ImportSlideMaster(const PresentationSlideMaster::Ptr& source);
    /** @return The master at a zero-based presentation position, or `nullptr`. */
    PresentationSlideMaster::Ptr GetSlideMaster(Size index) const;
    /** @return All registered masters in presentation XML order. */
    std::vector<PresentationSlideMaster::Ptr> SlideMasters() const;
    /**
     * @brief Removes a slide master and every layout owned by it.
     * @param master Master registered by this editor.
     * @param replacementLayout Optional layout to assign to slides that currently
     *        use a layout owned by @p master.
     * @return `false` for a foreign master, a replacement owned by the removed
     *         master, or an in-use master without a replacement.
     * @post On success, slides formerly using this master reference
     *       @p replacementLayout and their slide XML remains unchanged.
     */
    bool RemoveSlideMaster(const PresentationSlideMaster::Ptr& master,
                           const PresentationSlideLayout::Ptr& replacementLayout = nullptr);
    /**
     * @brief Creates a layout owned by an existing master.
     * @param master Master wrapper returned by this editor.
     * @param name Human-readable layout name stored on `p:cSld`.
     * @param type Standard or custom layout classification.
     * @return The registered layout, or `nullptr` for a foreign/invalid master.
     */
    PresentationSlideLayout::Ptr AddSlideLayout(
        const PresentationSlideMaster::Ptr& master,
        std::string name,
        DocumentFormat::OpenXml::Presentation::SlideLayoutValues::Value type =
            DocumentFormat::OpenXml::Presentation::SlideLayoutValues::Custom);
    /** @return All layouts grouped by master order and then master layout order. */
    std::vector<PresentationSlideLayout::Ptr> SlideLayouts() const;
    /**
     * @brief Removes a layout from its owning master.
     * @param layout Layout registered by this editor.
     * @param replacementLayout Optional different layout to assign to slides that
     *        currently use @p layout.
     * @return `false` for foreign layouts, an identical/foreign replacement, or
     *         an in-use layout without a replacement.
     * @post On success, affected slide XML and user-authored content are unchanged.
     */
    bool RemoveSlideLayout(const PresentationSlideLayout::Ptr& layout,
                           const PresentationSlideLayout::Ptr& replacementLayout = nullptr);
    /**
     * @brief Assigns a registered layout to a slide without rewriting slide XML.
     * @return `true` on success; `false` for invalid indices or foreign layouts.
     * @post Existing slide placeholders, text, shapes, and media are unchanged.
     */
    bool SetSlideLayout(Size slideIndex, const PresentationSlideLayout::Ptr& layout);

private:
    PowerPointDocument::Ptr m_document;
    std::shared_ptr<detail::DocumentEditTransactionOwner> m_transactionOwner;
};

} // namespace ExyokiOffice::PowerPoint
