# Word documents: lifecycle and document-level services

This chapter covers everything that concerns a Word document as a whole:
creating, opening, and saving; document types and templates; undo snapshots
and transactions; document properties; themes; and the escape hatch to the
low-level API. Content authoring starts in [Text and paragraphs](text.md).

Everything shown here lives in one header:

```cpp
#include "ExyokiOffice/Word/WordDocument.hpp"
using namespace ExyokiOffice::Word;
```

## Creating, opening, and saving

```cpp
auto fresh    = WordDocumentEditor::CreateNew();                  // new .docx
auto macro    = WordDocumentEditor::CreateNew(
                    WordDocumentEditor::WordprocessingDocumentType::MacroEnabledDocument);
auto fromDisk = WordDocumentEditor::Open("existing.docx");        // nullptr on failure
auto fromBytes= WordDocumentEditor::Open(bytes);                  // std::vector<uint8_t> or std::span
auto fromTmpl = WordDocumentEditor::CreateFromTemplate("letter.dotx");

editor->SaveToFile("out.docx");             // atomic save by default
std::vector<Byte> blob = editor->SaveToMemory();
```

All factories return `nullptr` when the source cannot be read or parsed —
always check the result before using it. `Open` accepts an `OpenSettings`
argument to control compatibility, validation behavior, and ZIP/XML safety
limits. New settings start with `OpenXmlPackageLimits::Recommended()`; an
application that knows its inputs should usually tighten those general-purpose
limits. `OpenXmlPackageLimits::Unlimited()` disables them explicitly for a
trusted source. The final optional `Packaging::OpenError*` argument reports why
an open failed. See
[Opening untrusted packages safely](../introduction.md#opening-untrusted-packages-safely).

A few details about saving:

- `SaveToFile(path, atomicSave, cancellationToken)` writes to a temporary
  sibling file first and publishes it only after the package has been
  serialized successfully, so a failed save never destroys the previous file.
  Pass `atomicSave = false` to write in place.
- `SaveToMemory` returns the complete ZIP package as bytes; an empty vector
  signals failure. The bytes can be passed straight back to `Open`.
- Both methods accept an optional `ICancellationToken` for aborting long
  saves from another thread.
- Saving updates document properties (modification time and the like) through
  the normal save lifecycle.

## Document types and templates

`WordprocessingDocumentType` selects the package flavor: `Document`
(`.docx`), `MacroEnabledDocument` (`.docm`), `Template` (`.dotx`), and
`MacroEnabledTemplate` (`.dotm`). The type determines the main part's content
type, so Word shows the right behavior when the file is opened.

`CreateFromTemplate(templatePath, attachTemplate)` creates a new *document*
from a `.dotx`/`.dotm` file. With `attachTemplate = true` (the default) the
new document records the template as its attached template, the same way Word
does when you create a document from a template.

## Undo snapshots: mementos and transactions

The editor can capture the complete package — every part, relationship,
content type, and pending DOM mutation — as an immutable snapshot and restore
it later. This is the building block for undo, retry, and "all or nothing"
edit batches:

```cpp
auto memento = editor->CreateMemento();          // std::optional<DocumentEditMemento>
// ... edits that might go wrong ...
if (somethingWentWrong && memento)
{
    editor->RestoreMemento(*memento);
}
```

`BeginTransaction()` packages the same mechanism as a scope guard: the
returned `DocumentEditTransaction` restores its snapshot automatically when
it goes out of scope, unless `Commit()` was called:

```cpp
{
    auto transaction = editor->BeginTransaction();
    if (!transaction.IsActive())
        return;                                  // snapshot failed

    editor->AddHeading("Generated section");
    FillGeneratedSection(*editor);               // may throw

    transaction.Commit();                        // keep the edits
}   // without Commit(), the document rolls back here
```

A successful restore invalidates every previously obtained wrapper, cursor,
and low-level reference — reacquire them from the editor afterwards. The
transaction must not outlive its editor.

## Document properties

`Properties()` returns a unified editor over the core, extended, and custom
property parts. The most common properties also have direct setters on the
document:

```cpp
auto document = editor->GetDocument();
document->SetTitle("Quarterly report");
document->SetCreator("Jane Doe");
document->SetSubject("Q2 results");
document->SetKeywords("finance, quarterly");   // shown as "Tags" in Word
document->SetDescription("Internal draft");    // shown as "Comments" in Word
document->SetCategory("Reports");
document->SetContentStatus("Draft");
document->SetCompany("ACME Corp");
```

## Themes

A Word theme (the DrawingML `a:theme` part) defines the document's color
scheme and font scheme. The editor offers both a typed and a lossless view:

```cpp
editor->EnsureTheme();                        // create the default Office theme if absent

auto theme = editor->ThemeSettings();         // typed: colors, fonts, scheme names
if (theme)
{
    theme->MajorFonts.Latin = "Aptos Display";
    editor->SetThemeSettings(*theme);         // preserves fills, lines, effects, extensions
}

auto xml = editor->ThemeXml();                // complete lossless a:theme XML
editor->SetThemeXml(customThemeXml);          // replace the theme wholesale
editor->RemoveTheme();
```

`SetThemeSettings` deliberately touches only the essential settings — the
twelve scheme colors, the major/minor fonts, and the scheme names — and keeps
the existing fill, line, effect, and extension subtrees intact. Use
`ThemeXml`/`SetThemeXml` when full control over the DrawingML is required.

## Reading the document

The editor exposes the body as typed collections; all of them return the same
wrapper types that the authoring calls return, so reading and editing use one
vocabulary:

```cpp
editor->BodyBlocks();       // every top-level block, in order, as BodyBlock
editor->Paragraphs();       // all body paragraphs
editor->Tables();           // all body tables
editor->Sections();         // every section, ending with the final body section
editor->Bookmarks();        // bookmarks anywhere in the body
editor->Fields();           // fields in the body (not headers/footers/notes)
```

`BodyBlock` is a discriminated view over one top-level block: `Type()`
reports paragraph, table, section, or content control, and
`AsParagraph()`/`AsTable()`/`AsSection()`/`AsContentControl()` down-cast to
the typed wrapper.

## Escape hatch to the low-level API

Every wrapper exposes `GetLowLevelApi()` returning the underlying typed DOM
element, and `editor->GetLowLevelApi()` returns the packaging-level
`WordDocument` for direct part and relationship access. The typed DOM's
`AppendChild<T>()` is schema-aware — it inserts children at the position the
content model requires — so low-level edits compose safely with high-level
ones. `examples/ExampleWord/main.cpp` builds a document entirely at that
level.

## Where to go next

| Chapter | Covers |
| --- | --- |
| [Text and paragraphs](text.md) | Body cursors, paragraphs, runs, formatting, find and replace. |
| [Styles, headings, and lists](styles.md) | `StyleManager`, headings, numbering. |
| [Tables](tables.md) | Structure, formatting, merging, nesting. |
| [Images](images.md) | Inline and floating images, wrapping, crop, alt text. |
| [Hyperlinks and bookmarks](hyperlinks.md) | External and internal links, bookmark ranges. |
| [Fields and tables of contents](fields.md) | Field structures, TOC, page numbering. |
| [Embedded charts](charts.md) | Reading and updating charts and their embedded workbooks. |
| [Sections and page setup](sections.md) | Page size, margins, columns, headers and footers. |
| [Notes, comments, and content controls](notes.md) | Footnotes, endnotes, comments, structured document tags. |
| [Revisions and document merging](revisions.md) | Tracked changes, comparing, merging, mail merge. |
| [Document protection](protection.md) | Editing restrictions and password verifiers. |
