# Slides

This chapter covers managing the slide list, importing slides from other
presentations, the fluent `SlideBuilder`, and organizing a deck with
sections and custom shows.

The namespace aliases from [Presentations](presentations.md) are assumed.

## Managing slides

```cpp
auto slide = editor->AddSlide();
auto copy  = editor->CopySlide(0);                       // deep copy within the same deck
auto other = editor->CopySlideFrom(sourceEditor, 0);     // import from another presentation

editor->MoveSlide(0, 2);
editor->RemoveSlide(1);

for (const auto& slide : editor->Slides())
{
    // ...
}

slide->SetHidden(true);          // exclude from slide-show playback
```

`CopySlideFrom` imports the complete dependency graph — layout, master,
theme, media, charts, embedded workbooks, notes, and comments — allocating
new package-local identifiers in the destination. The command-line
equivalents for whole decks are [exyoki](../tools/exyoki.md) `split` and
`merge`.

Every slide needs a layout assignment
(`editor->SetSlideLayout(slideIndex, layout)`); see
[Masters, layouts, and placeholders](masters.md).

## SlideBuilder

`CreateSlideBuilder()` is a fluent shortcut for the common "title plus a few
boxes" slide. `Build()` appends the slide and returns it, or returns
`nullptr` without leaving a partial slide behind; one builder can produce
several slides.

```cpp
auto slide = editor->CreateSlideBuilder()
                 .SetLayout(titleLayout)
                 .SetTitle("Quarterly report", TitleFrame())
                 .AddTextBox("Prepared for the board", SubtitleFrame(), "Subtitle")
                 .AddShape(Drawing::ShapeTypeValues::RoundRectangle, PanelFrame(), "Panel")
                 .Build();
```

Content is authored in title, text-box, then preset-shape order, which is
also its back-to-front z-order. `AddTextBox` takes either plain text
(newlines become separate paragraphs) or a full `PresentationTextFrame`;
`ClearTitle` and `ClearContent` reset parts of the builder for reuse.

> **`SetTitle` is not a placeholder.** It authors an ordinary text box whose
> shape name is `"Title"` — no `p:ph` element. That is enough to *look* like
> a title, but PowerPoint's outline pane, "Reset Slide", accessibility
> checks, and `slide->Placeholders()` all look for a real placeholder. When
> the title has to be recognized as one, declare it with
> `slide->AddPlaceholder(...)` instead — see
> [Masters, layouts, and placeholders](masters.md).

## Sections and custom shows

Sections group slides in the editing UI; custom shows define named playback
subsets:

```cpp
editor->AddSection({.Id = "{GUID}", .Name = "Introduction", .SlideIds = {slide->Id()}});
editor->AddCustomShow({.Id = 1, .Name = "Executive summary", .SlideIds = {slide->Id()}});
```

Both reference slides by their stable slide ID (`slide->Id()`), so they
survive reordering.
