# Styles, headings, and lists

Word formatting is built on styles: named, reusable definitions that
paragraphs and runs reference by ID. This chapter covers the built-in heading
shortcuts, the full `StyleManager`, latent styles, and list numbering through
`NumberingManager`.

```cpp
#include "ExyokiOffice/Word/WordDocument.hpp"
using namespace ExyokiOffice::Word;
namespace W = ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing;
```

## Headings

`AddHeading` applies the built-in `Heading1`–`Heading9` styles and creates
them with Word-like defaults when the document does not define them yet:

```cpp
editor->AddHeading("Introduction");        // Heading1
editor->AddHeading("Background", 2);       // Heading2
```

Headings created this way are what `AddTableOfContents` collects — see
[Fields and tables of contents](fields.md).

## The style manager

`editor->Styles()` returns a `StyleManager` over the document's style
definitions part. `StyleDefinition` is the typed description of one style:

```cpp
auto styles = editor->Styles();

StyleDefinition quote;
quote.StyleId = "MyQuote";
quote.Name = "My Quote";
quote.Type = StyleType::Paragraph;         // Paragraph, Character, Table, Numbering
quote.BasedOnStyleId = "Normal";
styles.CreateStyle(quote);

editor->AddParagraph("...")->SetStyleId("MyQuote");
```

The manager covers the full style lifecycle:

| Operation | Methods |
| --- | --- |
| Query | `HasStyle`, `Styles()`, `StylesByType`, `GetStyle`, `GetDefaultStyle` |
| Create and edit | `CreateStyle(definition, replaceExisting)`, `UpdateStyle`, `UpsertStyle`, `RemoveStyle` |
| Defaults | `SetDefaultStyle(type, styleId)`, `ClearDefaultStyle` |
| Import | `ImportStyle(source, styleId, conflictPolicy)` |
| Low level | `GetLowLevelStyle(styleId)`, `GetStylesPart()`, `GetRoot()` |

`ImportStyle` copies a style (with its dependencies) from another document's
`StyleManager`; `StyleCopyConflictPolicy` decides what happens when the
target already has a style with the same ID. `GetLowLevelStyle` returns the
DOM `w:style` element for formatting that `StyleDefinition` does not model.

### Latent styles

Latent styles are Word's mechanism for describing built-in styles that are
not physically present in the document — which ones appear in the style
gallery, their priority, and their locking state. The manager exposes them
as typed values:

```cpp
auto settings = styles.GetLatentStyleSettings();     // counts and defaults
styles.SetLatentStyleSettings(settings);

auto exceptions = styles.LatentStyleExceptions();    // per-style overrides
styles.SetLatentStyleException({.Name = "Subtle Emphasis", .SemiHidden = false});
styles.RemoveLatentStyleException("Subtle Emphasis");
```

## Simple lists

For a plain bulleted or numbered list, one call creates (or reuses) the
definition and returns a `ListStyle` to apply per paragraph:

```cpp
auto bullets = editor->EnsureBulletedListStyle();
editor->AddParagraph("First")->SetListStyle(bullets);
editor->AddParagraph("Second")->SetListStyle(bullets);

auto numbers = editor->EnsureNumberedListStyle();    // decimal "1." by default
editor->AddParagraph("Step one")->SetListStyle(numbers);
```

`EnsureNumberedListStyle` optionally takes the list name, a
`W::NumberFormatValues` (roman numerals, letters, …), and the level-text
pattern (for example `"%1)"`), so simple variations do not require the full
numbering machinery.

## Multi-level lists: the numbering manager

WordprocessingML separates *abstract numbering definitions* (what the levels
look like) from *numbering instances* (a concrete list in the document that
references an abstract definition). `editor->Numbering()` returns a
`NumberingManager` that handles both:

```cpp
NumberingDefinition outline;
outline.Name = "SampleOutline";
for (int level = 0; level < 3; ++level)
{
    NumberingLevelDefinition definition;
    definition.Level = level;
    definition.Format = W::NumberFormatValues::Decimal;
    definition.LevelText = level == 0 ? "%1." : (level == 1 ? "%1.%2." : "%1.%2.%3.");
    definition.LeftIndent = Millimeters(10.0 * (level + 1));
    definition.HangingIndent = Millimeters(8.0);
    outline.Levels.push_back(definition);
}

auto style = editor->Numbering().EnsureMultilevelList(outline);
style.Level = 1;                                  // second outline level
editor->AddParagraph("Nested item")->SetListStyle(style);
```

Up to nine levels are supported, matching Word. The `%1`, `%2`, … tokens in
`LevelText` insert the counter of the corresponding level.

Beyond creation, the manager supports the operations real documents need:

- `ContinueList(numberingId)` returns a `ListStyle` for adding paragraphs to
  an existing list so numbering continues.
- `RestartList(numberingId, overrides)` creates a new instance of the same
  abstract definition that restarts counting (per-level start overrides via
  `NumberingLevelOverride`).
- `Instances()` / `GetInstance(numberingId)` enumerate the numbering
  instances actually present in the document.
- `GetDefinition(abstractNumberingId)` reads an abstract definition back as
  a `NumberingDefinition` — the read inverse of `EnsureMultilevelList`.
- `ImportList(source, sourceNumberingId)` copies a definition from another
  document.

`Paragraph::SetNumbering(numberingId, level)` is the low-level form of
`SetListStyle` when you already know the raw IDs, and
`TryGetNumbering`/`ClearNumbering` read and remove a paragraph's list
membership.
