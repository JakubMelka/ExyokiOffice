# Pictures, media, and tables

This chapter covers embedding and linking pictures, audio and video frames,
and DrawingML tables.

The namespace aliases from [Presentations](presentations.md) are assumed.

## Pictures

```cpp
PresentationPictureData picture;
picture.Embedded = PresentationEmbeddedPicture{.Data = bytes, .ContentType = "image/png"};
picture.Transform = {
    .Position = {{0.75, MeasurementUnit::Inch}, {0.4, MeasurementUnit::Inch}},
    .Size = {{3.5, MeasurementUnit::Inch}, {2.625, MeasurementUnit::Inch}},
};
tree->AddPicture(picture);

// Signature detection supplies image/png and the intrinsic size from pixels/DPI.
auto detectedPicture = tree->AddPictureFromFile("logo.png");
detectedPicture->ReplacePictureFromFile("updated-logo.png"); // retains crop and styling
```

PNG, JPEG, GIF, and BMP insertion can use `AddPictureFromData` or
`AddPictureFromFile`; their signatures, dimensions, and DPI are read through
the shared `ExyokiOffice::DetectImageFormat` utility. Supplying a zero
width or height selects the corresponding intrinsic physical size. Explicit
`PresentationPictureData` remains available for linked images, unsupported
formats, exact content types, and complete metadata control.

Exactly one of `Embedded`/`LinkedUri` must be set; linked URIs are retained
as external relationships and are never resolved, downloaded, or displayed
by ExyokiOffice (see [External resources](../ExternalResources.md)).

Pictures use the same `SetOutline` and `SetEffects` API as other visual
shapes ([Shapes](shapes.md)). Replacing only the payload
(`ReplacePictureFromFile`/`ReplacePictureFromData`) retains crop, transform,
accessibility metadata, outline, glow, shadow, and reflection.

## Audio and video

```cpp
PresentationMediaData media;
media.Kind = PresentationMediaKind::Video;
media.LinkedUri = "https://example.com/clip.mp4";     // never dereferenced by this library
tree->AddMedia(media);

PresentationShapeTransform videoTransform;
videoTransform.Position = {{0.75, MeasurementUnit::Inch}, {0.4, MeasurementUnit::Inch}};
videoTransform.Size = {{7.0, MeasurementUnit::Inch}, {3.9375, MeasurementUnit::Inch}};
auto video = tree->AddMediaFromFile(PresentationMediaKind::Video,
                                    "demo.mp4",
                                    "video/mp4",
                                    videoTransform,
                                    posterFrame);
```

A media frame is a picture shape with playback behavior: the poster frame is
its visible image, and the media payload (embedded bytes or a linked URI)
hangs behind it. Replacing a media payload retains the poster and playback
settings along with the visual styling.

## Tables

```cpp
PresentationTableData table;
table.ColumnWidths = {
    {3.944, MeasurementUnit::Inch},
    {3.944, MeasurementUnit::Inch},
    {3.944, MeasurementUnit::Inch},
};
table.Rows = {PresentationTableRow{
    .Height = {0.406, MeasurementUnit::Inch},
    .Cells = {{"Area"}, {"Status"}, {"Notes"}},
}};
table.Transform = PresentationShapeTransform{
    .Position = {{0.75, MeasurementUnit::Inch}, {1.4, MeasurementUnit::Inch}},
    .Size = {{11.833, MeasurementUnit::Inch}, {0.406, MeasurementUnit::Inch}},
};
auto tableShape = tree->AddTable(table);

tableShape->InsertTableRow(1);
tableShape->MergeTableCells(0, 0, 1, 2);
```

A PowerPoint table is a DrawingML table inside a graphic frame — a different
schema from Word tables, though the authoring concepts (rows, cells, merges)
match. Structural edits go through the shape wrapper (`InsertTableRow`,
`MergeTableCells`, …), which keeps the underlying grid and span bookkeeping
consistent.
