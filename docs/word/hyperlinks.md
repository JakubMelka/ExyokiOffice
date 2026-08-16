# Hyperlinks and bookmarks

Hyperlinks point out of the document (a URL) or within it (a bookmark);
bookmarks name a place so links, fields, and cross-references can find it.

```cpp
#include "ExyokiOffice/Word/WordDocument.hpp"
using namespace ExyokiOffice::Word;
```

## Creating links and bookmarks

```cpp
paragraph->AddHyperlink("ExyokiOffice", "https://example.com", "tooltip");
paragraph->AddBookmark("Chapter1");
other->AddInternalHyperlink("See chapter 1", "Chapter1");
```

An external hyperlink stores its URL as a package relationship (URLs never
appear in the document XML itself), so the paragraph has to know which part it
lives in. Relationships belong to the part that holds the reference, not to the
document: a link in a header is resolved against `header1.xml.rels`, one in a
footnote against `footnotes.xml.rels`. Every paragraph the API hands out —
`editor->Paragraphs()`, `table->Paragraphs()`, `header->Paragraphs()`,
`note->Paragraphs()`, `comment->Paragraphs()` — carries its own part, and
`Paragraph::OwningPart()` says which. A `Paragraph` wrapped around a raw
element by hand carries none, and `AddHyperlink` then returns `nullptr` rather
than adding a link with nowhere to record its target; `AttachOwningPart` fixes
that.

An internal hyperlink stores the bookmark name as its anchor and needs no
relationship, so it works on any paragraph.

## The Hyperlink wrapper

`AddHyperlink` returns a `Hyperlink`, and `paragraph->Hyperlinks()` or
`editor` traversal find existing ones. The wrapper is both a container of
runs — `AddRun`, `AddText`, `Runs()`, `PlainText()` — and the place where the
link target lives:

```cpp
auto link = paragraph->AddHyperlink("Docs", "https://example.com");
link->SetTooltip("Opens the documentation");
link->SetNewWindow(true);

link->IsExternal();          // URL-backed
link->IsInternal();          // bookmark-backed
link->GetUrl();              // resolves the relationship
link->SetAnchor("Chapter1"); // switch to an internal link (clears the URL)
link->Remove();              // unwrap: keeps the text, removes the link
```

`SetUrl` and `SetAnchor` are mutually exclusive by design — setting one
clears the other, and the relationship bookkeeping (creating or removing the
external relationship) happens automatically.

## The Bookmark wrapper

A WordprocessingML bookmark is a *range* delimited by a start and an end
marker, which may span paragraphs. The wrapper pairs the two:

```cpp
auto bookmark = paragraph->AddBookmark("Chapter1");
bookmark->GetName();
bookmark->GetId();
bookmark->Remove();          // removes both markers, never the content

auto found = editor->FindBookmark("Chapter1");   // nullptr when absent
auto all = editor->Bookmarks();
```

Bookmark names must be unique in the document; Word additionally requires
them to start with a letter and contain no spaces. Bookmarks are also the
anchor mechanism for `PAGEREF`/`REF` fields — see
[Fields and tables of contents](fields.md).
