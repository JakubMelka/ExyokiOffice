# Images

This chapter covers inserting pictures into a Word document and controlling
their size, layout, wrapping, position, crop, rotation, accessibility
metadata, and click behavior.

```cpp
#include "ExyokiOffice/Word/WordDocument.hpp"
using namespace ExyokiOffice::Word;
```

## Inserting an image

```cpp
// From disk, natural size, format detected from the bytes:
editor->AddImageFromFile("logo.png");

// From memory with explicit size and floating layout:
editor->AddImageFromData(bytes, "image/png",
                         {50.0, ExyokiOffice::MeasurementUnit::Millimeter},
                         {30.0, ExyokiOffice::MeasurementUnit::Millimeter},
                         ImageLayout::Floating, ImageWrap::Square);
```

Both calls exist in two flavors. The convenient overloads call
`DetectImageFormat` on the bytes to determine the content type and the
intrinsic size (from pixel dimensions and DPI); they recognize PNG, JPEG, GIF,
BMP, TIFF, EMF, and placeable WMF. For anything else — a bare WMF, which states
no bounding box, or SVG, which Word only renders with a raster fallback beside
it — use the explicit overloads that take a content type and size. The payload
is embedded as-is either way.

A JPEG's resolution is read from Exif as well as from JFIF, which is what
matters for photographs: cameras and phones write it into Exif and leave the
JFIF segment claiming "no units". A file that states no resolution at all is
taken as 96 DPI.

An image added without an explicit size is scaled down to the section's text
width if it would not otherwise fit — a four thousand pixel photograph at 96 DPI
is forty-one inches across, and Word would place it mostly off the page. Pass a
size explicitly to get the native one.

The returned `Image` wrapper edits the picture in place; `AddImageFromFile`
and `AddImageFromData` bind it to the main document part automatically, which
click-hyperlink support requires. `Paragraph::Images()` and `Run::Images()`
find existing images when editing a document.

## Layout: inline versus floating

```cpp
image->SetLayout(ImageLayout::Inline);      // flows with the text like a character
image->SetLayout(ImageLayout::Floating);    // anchored, text wraps around it
auto layout = image->GetLayout();
```

An inline image occupies a position in the run text. A floating image is
anchored to a paragraph and positioned on the page independently; everything
in the next two sections applies to floating images.

## Size, wrap, and position

```cpp
image->SetSize(Millimeters(50.0), Millimeters(30.0));

image->SetWrap(ImageWrap::Square);          // Square, Tight, TopAndBottom, ...
image->SetDistanceFromText(Millimeters(3.0), Millimeters(3.0),
                           Millimeters(3.0), Millimeters(3.0));

// Offset from a reference edge (enums from the DrawingML
// Wordprocessing namespace: Page, Margin, Paragraph, Column, ...):
namespace DW = ExyokiOffice::DocumentFormat::OpenXml::Drawing::Wordprocessing;
image->SetPosition(DW::HorizontalRelativePositionValues::Margin, Millimeters(10.0),
                   DW::VerticalRelativePositionValues::Paragraph, Millimeters(5.0));
```

`SetPositionAligned` expresses the same relative to reference edges with
alignment keywords (left/center/right, top/middle/bottom) instead of offsets.

Anchor behavior has its own switches, mirroring Word's layout dialog:
`SetBehindText`, `SetAllowOverlap`, `SetAnchorLocked`, `SetRelativeHeight`
(z-order among overlapping images), `SetLayoutInCell`, and
`SetSimplePosition`/`SetSimplePositionEnabled` for absolute page coordinates.
The `TryGet…` counterparts (`TryGetSize`, `TryGetWrap`, `TryGetPosition`,
`TryGetDistanceFromText`, `TryGetAnchorOptions`) read the current state into
small structs.

## Crop, rotation, and flip

```cpp
image->SetCrop(0.10, 0.0, 0.10, 0.0);   // fractions cut from left/top/right/bottom
image->SetRotation(90.0);               // degrees clockwise
image->SetFlip(true, false);            // horizontal, vertical
image->ClearCrop();
```

## Alt text and click hyperlink

```cpp
image->SetAltText("Company logo", "ExyokiOffice logo in blue");
image->SetHyperlink("https://example.com", /*newWindow=*/true, "Visit the site");
```

Alt text (`title` and `description`) is what screen readers announce; set it
on every meaningful image. `TryGetHyperlink` resolves the link target through
the main document part, and `RemoveHyperlink` deletes it.

## Limitations

- `DetectImageFormat` does not sniff SVG or a WMF without a placeable header;
  supply an explicit content type and size for those. An SVG that Word will
  render also needs a raster fallback picture beside it, which this API does not
  build for you.
- `AddImageFromFile`/`AddImageFromData` append the image at the end of the
  body; there is no cursor-relative image insertion yet. A floating layout
  with a page- or margin-relative position covers most placement needs;
  beyond that, drop to the low-level DOM to move the `w:drawing` run.
