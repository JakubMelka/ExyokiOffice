# Shapes

Everything visible on a slide is a shape in its shape tree: auto shapes,
connectors, pictures, media frames, tables, charts, and groups. This chapter
covers the tree itself, geometry, transforms, fills, outlines, and effects.

The namespace aliases from [Presentations](presentations.md) are assumed.

## The shape tree

Every slide exposes a `PresentationShapeTree` for its `p:spTree` in exact
z-order (index 0 is furthest back):

```cpp
auto tree = slide->ShapeTree();
auto shape = tree->AddShape("Box");
auto connector = tree->AddConnector("Link");
auto picture = tree->AddPicture(pictureData);
auto media = tree->AddMedia(mediaData);
auto table = tree->AddTable(tableData);

tree->BringToFront(0);
tree->SendBackward(2);
auto group = tree->Group({0, 1});
tree->Ungroup(0); // dissolve the group, re-projecting child coordinates
tree->Remove(3);
```

Reordering, grouping, and ungrouping rebuild the shape XML, so every
`PresentationShape` wrapper obtained before such a call must be reacquired
through `tree->Get(index)`. Capture what you need — typically `shape->Id()`
for later animation targets — before reordering:

```cpp
const auto titleId = title->Id();
tree->SendToBack(tree->Count() - 1);   // title wrapper is stale from here on
slide->AddAnimationEffect({.TargetShapeId = titleId, /* ... */});
```

## Geometry

A new shape has no geometry until you set one:

```cpp
shape->SetPresetGeometry(Drawing::ShapeTypeValues::RoundRectangle,
                         {{"adj", "val 25000"}});     // preset + adjustment guides
// or:
shape->SetFreeformGeometry(paths, connectionSites);   // custom DrawingML path
```

The preset catalog is DrawingML's full set of auto shapes; adjustment guides
tune the parameterized ones (corner radius, arrow-head proportions, and so
on).

## Transforms

Position and size on `PresentationShapeTransform` use `MeasuringUnits`, so
callers can express each coordinate in EMU, points, twips, inches,
centimeters, or millimeters. Values are converted to integral EMU when
serialized. Rotation remains in the native OOXML angle unit (1/60000 of a
degree), while flips are booleans:

```cpp
shape->SetTransform({
    .Position = {{0.75, MeasurementUnit::Inch}, {0.4, MeasurementUnit::Inch}},
    .Size = {{11.833, MeasurementUnit::Inch}, {0.875, MeasurementUnit::Inch}},
});
auto current = shape->GetTransform();
```

## Fills and outlines

Auto shapes, pictures, and connectors carry a fill and an outline. A fill is
solid, a linear gradient, an explicit `noFill`, or inherited (no fill
element); an outline exposes its color model plus width, dash, cap, and
compound type. All lengths accept any physical unit and colors must be
explicit sRGB:

```cpp
PresentationShapeFill fill;
fill.Kind = PresentationFillKind::Gradient;
fill.GradientStops = {{Color(0xFF, 0x00, 0x00), 0.0},
                      {Color(0x00, 0x00, 0xFF), 100.0}};
fill.GradientAngle = MeasuringAngle(45.0, AngleUnit::Degree);
shape->SetFill(fill);

PresentationShapeOutline outline;
outline.Fill = PresentationFillKind::Solid;
outline.ColorValue = Color(0x1F, 0x4E, 0x79);
outline.Width = MeasuringUnits(2.0, MeasurementUnit::Point);
outline.Dash = Drawing::PresetLineDashValues::Dash;
shape->SetOutline(outline);

auto readFill = shape->GetFill();       // std::nullopt for groups and graphic frames
auto readOutline = shape->GetOutline(); // std::nullopt when no a:ln is present
```

## Effects

A shape also carries an `a:effectLst` with glow, outer-shadow, and
reflection effects. The effect descriptors are the same ones used for
run-level text formatting. Passing an all-absent value clears the effect
list:

```cpp
PresentationShapeEffects effects;
PresentationTextShadow shadow;
shadow.BlurRadius = MeasuringUnits(4.0, MeasurementUnit::Point);
shadow.Distance = MeasuringUnits(3.0, MeasurementUnit::Point);
shadow.ColorValue = Color(0x80, 0x80, 0x80);
effects.Shadow = shadow;
shape->SetEffects(effects);

shape->SetEffects({}); // remove the effect list
```

Putting text inside a shape is the subject of the [Text](text.md) chapter.
