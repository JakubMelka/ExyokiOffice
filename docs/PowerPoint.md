# PowerPoint quickstart

This quickstart introduces
`ExyokiOffice::PowerPoint::PowerPointDocumentEditor`, the high-level API
for authoring and editing PowerPoint presentations. It shows the essential
moves — creating a deck with a valid design, adding slides and shapes — and
hands off to the [PowerPoint chapters](powerpoint/presentations.md) for the
thorough treatment of each topic.

Everything shown here lives in one header:

```cpp
#include "ExyokiOffice/PowerPoint/PowerPointDocument.hpp"
using namespace ExyokiOffice::PowerPoint;
namespace Drawing = ExyokiOffice::DocumentFormat::OpenXml::Drawing;
namespace Presentation = ExyokiOffice::DocumentFormat::OpenXml::Presentation;
using ExyokiOffice::MeasurementUnit;
using ExyokiOffice::MeasuringUnits;
```

The snippets below assume both namespace aliases are in scope.

A complete runnable version of most snippets is in
[examples/ExamplePowerPointEditor/main.cpp](../examples/ExamplePowerPointEditor/main.cpp),
which builds a five-slide deck — a title slide with a real title
placeholder, multi-level text on a tinted panel, a picture, a chart, a
DrawingML table with a merged cell, transitions, animations, speaker notes,
comments, sections, and a custom show — and re-opens the saved package to
prove it round-trips. The full API reference is the Doxygen documentation in
[PowerPointDocument.hpp](../include/ExyokiOffice/PowerPoint/PowerPointDocument.hpp).

## Hello world

```cpp
auto editor = PowerPointDocumentEditor::CreateNew();
editor->SetSlideSize(PresentationSlideSize::Widescreen16x9());

// A deck needs a design: every slide references a layout, every layout belongs
// to a master. The master is created with the default Office theme.
auto master = editor->AddSlideMaster("Default");
auto layout = editor->AddSlideLayout(master, "Title", Presentation::SlideLayoutValues::Title);

auto shape = editor->AddSlide()->ShapeTree()->AddShape("Title");
editor->SetSlideLayout(0, layout);
shape->SetPresetGeometry(Drawing::ShapeTypeValues::Rectangle);
shape->SetTransform({
    .Position = {{0.75, MeasurementUnit::Inch}, {2.5, MeasurementUnit::Inch}},
    .Size = {{11.833, MeasurementUnit::Inch}, {1.3125, MeasurementUnit::Inch}},
});

PresentationTextFrame frame;
frame.Paragraphs = {{{{"Hello world", "en-US", true, false, std::nullopt, ""}}}};
shape->SetTextFrame(frame);

editor->SaveToFile("Hello.pptx");
```

The design requirement is the one thing that distinguishes PowerPoint from
the other formats: `CreateNew()` gives you an empty presentation, and
PowerPoint reports a deck whose slides have no layout as damaged. Create a
master and layout first (or import a designed master from a template — see
[Masters, layouts, and placeholders](powerpoint/masters.md)).

## Creating, opening, and saving

```cpp
auto fresh    = PowerPointDocumentEditor::CreateNew();                // new .pptx
auto fromDisk = PowerPointDocumentEditor::Open("existing.pptx");      // nullptr on failure
auto fromBytes= PowerPointDocumentEditor::Open(bytes);                // std::vector<uint8_t> or std::span

editor->SaveToFile("out.pptx");             // atomic save by default
std::vector<Byte> blob = editor->SaveToMemory();
```

All factories return `nullptr` when the source cannot be read or parsed —
always check the result. Slide size, modify protection, and the low-level
escape hatch are covered in [Presentations](powerpoint/presentations.md).

## A short tour

`SlideBuilder` is the fastest way to a complete slide:

```cpp
auto slide = editor->CreateSlideBuilder()
                 .SetLayout(titleLayout)
                 .SetTitle("Quarterly report", TitleFrame())
                 .AddTextBox("Prepared for the board", SubtitleFrame(), "Subtitle")
                 .Build();
```

(Note that `SetTitle` authors a text box *named* "Title", not a real title
placeholder — see [Slides](powerpoint/slides.md) for when that matters.)

Pictures, transitions, animations, and speaker notes are one call each:

```cpp
auto picture = slide->ShapeTree()->AddPictureFromFile("logo.png");

slide->SetTransition({.Kind = PresentationTransitionKind::Fade,
                      .Speed = PresentationTransitionSpeed::Medium,
                      .AdvanceOnClick = true});

slide->AddAnimationEffect({.TargetShapeId = picture->Id(),
                           .Class = PresentationAnimationEffectClass::Entrance,
                           .Effect = PresentationAnimationEffect::Fade});

slide->SetNotesText("Speaker notes for this slide.");
```

## The chapters

| Chapter | Covers |
| --- | --- |
| [Presentations](powerpoint/presentations.md) | Lifecycle, slide size, modify protection, low-level access. |
| [Slides](powerpoint/slides.md) | Slide management, `SlideBuilder`, sections, custom shows. |
| [Shapes](powerpoint/shapes.md) | The shape tree, geometry, transforms, fills, outlines, effects. |
| [Text](powerpoint/text.md) | Text frames, runs, bullets. |
| [Pictures, media, and tables](powerpoint/pictures-and-media.md) | Embedded and linked pictures, audio/video, DrawingML tables. |
| [Charts](powerpoint/charts.md) | Chart creation, cached data, embedded workbooks. |
| [Masters, layouts, and placeholders](powerpoint/masters.md) | The design hierarchy, themes, placeholder inheritance. |
| [Transitions](powerpoint/transitions.md) | Slide transition effects and timing. |
| [Animations](powerpoint/animations.md) | Animation sequences, effects, triggers. |
| [Notes and comments](powerpoint/notes-and-comments.md) | Speaker notes and slide comments. |

## Known limitations

- `PresentationTextRun` exposes bold, italic, language, and hyperlinks only;
  font size, color, and other run-level DrawingML formatting require
  `shape->GetElement()` and direct DOM access.
- `PresentationSlideLayout` and `PresentationSlideMaster` do not expose a
  `ShapeTree()`, so positioning or filling their own (non-inherited)
  placeholder shapes currently requires the low-level DOM.
- Presentation-level metadata other than slide size, handout settings,
  sections, and custom shows has no high-level wrapper yet; use
  `GetDocument()->GetPresentationPart()->GetPresentation()`.
- Linked pictures, media, charts, SmartArt, and OLE objects are preserved
  losslessly but never resolved, downloaded, played, or rendered by this
  library.

The [compatibility matrix](Compatibility.md) grades every PowerPoint feature
area for create, edit, and preserve support, alongside the supported document
types and Office versions.
