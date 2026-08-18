# Presentations: lifecycle and document-level services

This chapter covers the presentation as a whole: creating, opening, and
saving; slide size; modify protection; and the escape hatch to the low-level
API. Slides themselves start in [Slides](slides.md).

Everything shown here lives in one header; the namespace aliases below are
assumed by every PowerPoint chapter:

```cpp
#include "ExyokiOffice/PowerPoint/PowerPointDocument.hpp"
using namespace ExyokiOffice::PowerPoint;
namespace Drawing = ExyokiOffice::DocumentFormat::OpenXml::Drawing;
namespace Presentation = ExyokiOffice::DocumentFormat::OpenXml::Presentation;
using ExyokiOffice::MeasurementUnit;
using ExyokiOffice::MeasuringUnits;
```

## Creating, opening, and saving

```cpp
auto fresh    = PowerPointDocumentEditor::CreateNew();                // new .pptx
auto macro    = PowerPointDocumentEditor::CreateNew(
                    PowerPointDocumentType::MacroEnabledPresentation);
auto fromDisk = PowerPointDocumentEditor::Open("existing.pptx");      // nullptr on failure
auto fromBytes= PowerPointDocumentEditor::Open(bytes);                // std::vector<uint8_t> or std::span

editor->SaveToFile("out.pptx");             // atomic save by default
std::vector<Byte> blob = editor->SaveToMemory();
```

`Open` accepts an `OpenSettings` argument to control ZIP/XML safety limits and
validation behavior. New settings start with
`OpenXmlPackageLimits::Recommended()`; an application that knows its inputs
should usually tighten those general-purpose limits.
`OpenXmlPackageLimits::Unlimited()` disables them explicitly for a trusted
source. The final optional `Packaging::OpenError*` argument reports why an open
failed. See
[Opening untrusted packages safely](../introduction.md#opening-untrusted-packages-safely).
All factories return `nullptr` when the source cannot be read or parsed —
always check the result.

One thing distinguishes PowerPoint from the other two formats:
**`CreateNew()` gives you an empty presentation, not a design.**
PresentationML requires every slide to reference a layout, every layout to
belong to a master, and every master to have a theme; PowerPoint reports a
deck whose slides have no layout as damaged. Create the design before (or
right after) the first slide — see
[Masters, layouts, and placeholders](masters.md).

## Slide size

A freshly created presentation has no explicit slide size, so PowerPoint
applies its own default. Set one to pin the deck to a known aspect ratio:

```cpp
editor->SetSlideSize(PresentationSlideSize::Widescreen16x9());   // 13.333 x 7.5 in
editor->SetSlideSize(PresentationSlideSize::Widescreen16x10());  // 10 x 6.25 in
editor->SetSlideSize(PresentationSlideSize::Standard4x3());      // 10 x 7.5 in
editor->SetSlideSize(PresentationSlideSize::A4Landscape());      // 297 x 210 mm

// Any physical extent works; leaving Type unset writes a custom size.
PresentationSlideSize custom;
custom.Size = {MeasuringUnits(16.0, MeasurementUnit::Centimeter),
               MeasuringUnits(9.0, MeasurementUnit::Centimeter)};
editor->SetSlideSize(custom);

auto size = editor->GetSlideSize();   // std::nullopt when no p:sldSz is present
editor->RemoveSlideSize();            // back to PowerPoint's default
```

`SetSlideSize` validates the extent against PresentationML's coordinate
range (one inch through 56 inches on either axis) and leaves the
presentation untouched when it does not fit. Existing slides keep their own
shape coordinates — nothing is rescaled implicitly — so set the size before
laying content out.

## Modify protection

Modify protection is PowerPoint's "password to modify": the presentation
opens read-only unless the password is supplied. Like Word document
protection and Excel worksheet protection, it is a user-interface
restriction rather than encryption — the package stays plain OOXML, so it
protects against accidental edits, not against a determined reader.

```cpp
auto applied = editor->ProtectFromModification("board only");
auto state = editor->GetModifyProtection();        // hasPassword, verifierSupported
auto removed = editor->UnprotectFromModification("board only");
```

The password is stored in `p:modifyVerifier` as the ISO/IEC 29500 verifier —
a salted SHA-512 hash iterated 100 000 times — matching what current
PowerPoint writes. `ProtectFromModification` requires a non-empty password,
and `UnprotectFromModification` validates it before removing the element,
reporting structured `PresentationProtectionError` values. Presentations
carrying a pre-2010 legacy `hashData` verifier report
`verifierSupported == false` and `UnsupportedVerifier`.

## Escape hatch to the low-level DOM

Every wrapper exposes `GetElement()`/`GetPart()`/`GetLowLevelApi()`
returning the underlying typed DOM element or package part, mirroring the
Word editor's pattern. Presentation-level metadata without a high-level
wrapper yet is reached this way:

```cpp
auto presentation = editor->GetDocument()->GetPresentationPart()->GetPresentation();
auto view = presentation->AppendChild<Presentation::DefaultTextStyle>();
```

`AppendChild<T>()` is schema-aware: it inserts the new child at the correct
position in the parent's content model regardless of call order, so
low-level DOM edits like this one do not need to happen first.

`examples/ExamplePowerPointEditor` deliberately uses none of this — it is a
check that the high-level API alone is enough to author a complete deck.

## The chapters

| Chapter | Covers |
| --- | --- |
| [Slides](slides.md) | Slide management, `SlideBuilder`, sections, custom shows. |
| [Shapes](shapes.md) | The shape tree, geometry, transforms, fills, outlines, effects. |
| [Text](text.md) | Text frames, runs, bullets. |
| [Pictures, media, and tables](pictures-and-media.md) | Embedded and linked pictures, audio/video, DrawingML tables. |
| [Charts](charts.md) | Chart creation, cached data, embedded workbooks. |
| [Masters, layouts, and placeholders](masters.md) | The design hierarchy, themes, placeholder inheritance. |
| [Transitions](transitions.md) | Slide transition effects and timing. |
| [Animations](animations.md) | Animation sequences, effects, triggers. |
| [Notes and comments](notes-and-comments.md) | Speaker notes and slide comments. |
