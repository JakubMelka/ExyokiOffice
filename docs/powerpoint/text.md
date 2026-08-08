# Text

Text on a slide lives in a shape's text frame: a list of paragraphs, each a
list of runs, with optional per-paragraph bullets. The model is a plain
value type — build it, then assign it to the shape.

The namespace aliases from [Presentations](presentations.md) are assumed.

## Text frames, paragraphs, and runs

```cpp
PresentationTextRun run;
run.Text = "Key features";
run.Bold = true;

PresentationTextParagraph paragraph;
paragraph.Runs = {run};
paragraph.Bullet = PresentationTextBullet{.Character = "\xE2\x80\xA2"};   // UTF-8 bullet "•"

PresentationTextFrame frame;
frame.Paragraphs = {paragraph};
shape->SetTextFrame(frame);
```

`SetTextFrame` replaces the shape's text wholesale; read the current frame
back, modify it, and reassign to edit incrementally.

## Bullets

`PresentationTextBullet` supports either a literal `Character` or an
automatic `Numbering` scheme, mutually exclusive; leaving `Bullet` unset
disables bullets for that paragraph. Indentation levels combined with
bullets produce the classic multi-level content box —
`examples/ExamplePowerPointEditor/main.cpp` builds one.

## Hyperlinks

A run can carry an external hyperlink and a tooltip; the relationship
bookkeeping happens when the frame is assigned to a shape in a live
presentation.

## Limitations

`PresentationTextRun` currently exposes `Bold`, `Italic`, the language tag,
an optional external hyperlink, and a tooltip. Other DrawingML run
properties — font size, color, typeface, effects — are not yet exposed by
this wrapper; set them through `shape->GetElement()` and the typed DOM (the
run properties element is `a:rPr`). Because slides inherit text styling from
their layout and master, the design-level defaults from
[Masters, layouts, and placeholders](masters.md) are often the better place
for font choices anyway.
