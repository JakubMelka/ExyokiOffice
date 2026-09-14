# Masters, layouts, and placeholders

A presentation is only valid once it has a design: PresentationML requires
each slide to reference a slide layout, each layout to belong to a slide
master, and each master to have a theme, and PowerPoint refuses a deck that
lacks any of them. `CreateNew()` therefore writes the default design described
below, and `AddSlide()` places a new slide on the first registered layout;
`SetSlideLayout` moves it to any other. Masters you add come after the
default one in `SlideMasters()`.

The namespace aliases from [Presentations](presentations.md) are assumed.

## The default design

`EnsureDefaultLayout()` returns the first layout the presentation has; when
there is none (a deck opened from a file without one, or after every master
was removed) it creates a master named `ExyokiOffice` with the Office
default theme, a theme background, the title/body/other text styles, and
the five standard placeholders (title, body, date, footer, slide number)
positioned for the presentation's slide size, plus a "Title and Content"
layout carrying the same placeholders. `CreateNew()` calls it, so a fresh
presentation already carries this design:

```cpp
auto layout = editor->EnsureDefaultLayout();
auto slide = editor->AddSlide(editor->CreateSlideBuilder().SetLayout(layout));
slide->AddPlaceholder(Presentation::PlaceholderValues::Title);   // drawn where the layout says
```

PowerPoint draws a slide placeholder at the position it inherits, so a
master whose placeholders had no geometry rendered every title and body at
0 x 0. The converters (`exyoki convert`, the MCP servers' `add_slide`) go
through this call.

## Building the design hierarchy

```cpp
auto master = editor->AddSlideMaster("Corporate");
auto layout = editor->AddSlideLayout(master, "Title and content",
                                     Presentation::SlideLayoutValues::Object);
editor->SetSlideLayout(0, layout);   // assign to slide 0 without rewriting its XML

layout->AddPlaceholder(Presentation::PlaceholderValues::Body, 2);
auto slidePlaceholder = slide->AddPlaceholder(Presentation::PlaceholderValues::Object, 2);

for (const auto& placeholder : slide->Placeholders())
{
    // Direct slide placeholders first, then non-overridden layout/master
    // placeholders — PresentationML inheritance resolved for you.
}
```

`AddSlideMaster` writes the default Office theme and the default design
described above into the new master; `SetThemeXml`/`SetThemeSettings`
replace the theme (below). `AddSlideLayout` creates an empty layout; a
placeholder added to it takes the geometry of the master placeholder it
inherits from (same index, else same type, else the type it draws where — a
subtitle or content placeholder sits in the body area), so a layout built by
hand renders like one PowerPoint wrote.
`layout->FindPlaceholder(type)` answers which of the layout's own
placeholders a slide placeholder of that type would inherit from, or
`nullptr` when the layout offers none — a Blank layout has no title even
though the master does.

## Importing a master

Masters are complete presentation designs rather than isolated XML nodes.
`ImportSlideMaster` deep-copies a master and its entire relationship graph
from another editor, including layouts, themes, images, and extension
parts:

```cpp
auto sourceMaster = templateEditor->GetSlideMaster(0);
auto imported = editor->ImportSlideMaster(sourceMaster);
auto titleLayout = imported->Layouts().at(0);
editor->SetSlideLayout(0, titleLayout);
```

This is the recommended way to give generated decks a corporate design:
maintain one template presentation, import its master, and assign its
layouts.

## Themes

Essential theme editing is strongly typed. All twelve scheme colors,
major/minor Latin, East Asian and complex-script fonts, supplemental
script-specific fonts, and scheme names can be changed together:

```cpp
auto theme = imported->ThemeSettings();
theme->Name = "Corporate";
theme->Colors[static_cast<Size>(PresentationThemeColorSlot::Accent1)] =
    Color(0x16, 0x5D, 0xA7);
theme->MajorFonts.Latin = "Aptos Display";
theme->MinorFonts.Latin = "Aptos";
theme->MinorFonts.SupplementalFonts = {{"Jpan", "Yu Gothic"}};
imported->SetThemeSettings(*theme);
```

Applying typed settings deliberately retains the existing fill, line,
effect, background-fill, and extension subtrees. System colors are read
through their `lastClr` fallback and become explicit sRGB values when the
typed model is applied. Use `ThemeXml`/`SetThemeXml` — the lossless
DrawingML view — when exact system-color semantics or specialized style
matrix editing is required.

## Removing layouts and masters

Removal is reference-safe. An in-use layout or master is not removed unless
a replacement layout is supplied; affected slides are redirected without
rewriting their slide XML:

```cpp
editor->RemoveSlideLayout(oldLayout, replacementLayout);
editor->RemoveSlideMaster(oldMaster, replacementLayout);
```

## Placeholders in depth

Placeholder inheritance matches by explicit `idx` when present, and
otherwise by placeholder `type`. `AddPlaceholder` appends a shape to the
owning level's shape tree, so a placeholder can also be positioned and
filled with text like any other shape. `PresentationPlaceholder` carries the
`p:ph` semantics and `PresentationShape` carries geometry and text, and both
are views over the same `p:sp` node, so the shape is matched back out of the
tree by node identity:

```cpp
auto placeholder = slide->AddPlaceholder(Presentation::PlaceholderValues::Title, 1);

PresentationShape::Ptr titleShape;
for (const auto& shape : slide->ShapeTree()->Shapes())
{
    if (shape->GetElement()->IsSameNode(placeholder->GetElement()))
    {
        titleShape = shape;
        break;
    }
}
titleShape->SetTransform(frame);
titleShape->SetTextFrame(text);
```

A slide placeholder does not need a transform of its own: without one it is
drawn where the layout (or, failing that, the master) puts the placeholder
it inherits from. `GetTransform()` reports only what the shape carries;
`GetEffectiveTransform()` resolves that inheritance and reports the box
PowerPoint draws, which is what the MCP servers' `get_slide` shows.

This is how a slide gets a title that other tools — the outline pane,
"Reset Slide", accessibility checkers — recognize as a title;
`SlideBuilder::SetTitle` deliberately does *not* do this (see
[Slides](slides.md)).
[examples/ExamplePowerPointEditor/main.cpp](../../examples/ExamplePowerPointEditor/main.cpp)
does exactly this on its title slide.

## Limitations

`PresentationSlideLayout` and `PresentationSlideMaster` do not expose their
own `ShapeTree()`, so setting transforms or text on layout- and
master-level placeholder shapes currently requires the low-level DOM.
