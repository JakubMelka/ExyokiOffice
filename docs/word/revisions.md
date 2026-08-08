# Revisions and document merging

This chapter covers tracked changes (reading, accepting, rejecting, and
generating them), copying one document into another, and mail-merge-style
template filling.

```cpp
#include "ExyokiOffice/Word/WordDocument.hpp"
using namespace ExyokiOffice::Word;
```

## Tracked revisions

Tracked changes appear in the body as insertion and deletion containers with
an author, a date, and an ID. The editor exposes them as `Revision` wrappers:

```cpp
for (const auto& revision : editor->Revisions())
{
    revision->Type();          // RevisionType: insertion, deletion, ...
    revision->GetAuthor();
    revision->GetDate();
    revision->Text();          // the affected text
    revision->Accept();        // or revision->Reject();
}

Size accepted = editor->AcceptAllRevisions();
Size rejected = editor->RejectAllRevisions();
```

Accepting an insertion unwraps the inserted content into the document;
accepting a deletion removes it. Rejecting mirrors this: inserted content is
removed and deleted content is restored (deleted text nodes are converted
back to normal text).

## Comparing two documents

`CompareWith` produces a tracked comparison against a revised document:

```cpp
auto original = WordDocumentEditor::Open("v1.docx");
auto revised  = WordDocumentEditor::Open("v2.docx");

Size changes = original->CompareWith(*revised,
                                            RevisionAuthor{.Name = "Comparison"});
original->SaveToFile("v1-vs-v2.docx");
```

The comparison is deliberately conservative: it works at the level of
top-level paragraph plain text, marking paragraphs missing from the revised
document as tracked deletions and paragraphs only present there as tracked
insertions. It does not emulate Word's full diff engine (no move detection,
formatting comparison, or table diffing). The generated revisions are
ordinary tracked changes: they can be inspected, accepted, rejected, and
round-tripped like any other. For a package-level comparison (which parts
and elements changed), see [exyoki](../tools/exyoki.md) `diff`.

## Inserting one document into another

`BodyCursor::InsertDocument` deep-copies another document's body content at
the cursor position:

```cpp
auto target   = WordDocumentEditor::Open("report.docx");
auto appendix = WordDocumentEditor::Open("appendix.docx");

target->Body().InsertDocument(*appendix);
target->SaveToFile("report-with-appendix.docx");
```

The copy merges every dependency the content can reference, so the result
never contains a duplicate ID or a dangling reference:

- **Styles** are imported through `StyleManager::ImportStyle`, following
  `DocumentMergeOptions::StyleConflictPolicy`. The default (`Rename`) keeps
  copied content looking exactly as it did: an unused style ID is kept, and
  an ID that collides with a *different* target style is imported under a
  fresh ID.
- **Numbering lists** are imported with fresh instance IDs, so copied
  paragraphs keep their list formatting.
- **Bookmarks** get fresh IDs; colliding names are suffixed, and internal
  hyperlinks in the copied content are rewritten to the renamed bookmarks.
- **Footnotes, endnotes, and comments** are copied into the target parts
  under fresh IDs.
- **Images** are copied as new image parts with the drawing relationships
  and shape IDs rewritten.
- **External hyperlinks** get new relationships pointing at the same URLs.

The source's trailing `w:sectPr` — its own final page setup — is
intentionally not copied; section breaks inside the copied content travel
with their paragraphs. The source document is never modified.

The same operation is available from the command line as
[exyoki](../tools/exyoki.md) `merge`, including Excel and PowerPoint
variants.

## Template filling (mail merge)

`MergeTemplate` fills `MERGEFIELD` placeholders, same-paragraph bookmarks,
and repeating regions with literal data:

```cpp
TemplateMergeData data;
data.Values = {{"Customer", "Contoso Ltd."}, {"Date", "2026-07-30"}};
data.Regions["Orders"] = {
    {{"Item", "Widget"}, {"Qty", "12"}},
    {{"Item", "Gadget"}, {"Qty", "3"}},
};

TemplateMergeResult result = editor->MergeTemplate(data);
// result.FieldsMerged, result.BookmarksMerged,
// result.RegionsMerged, result.RegionRowsInserted
```

Repeating regions use the common Word mail-merge marker convention:
`MERGEFIELD TableStart:Orders` … `MERGEFIELD TableEnd:Orders`. The
paragraphs between the markers are copied once per row in
`Regions["Orders"]` and merged with that row's values; the marker paragraphs
are removed from the final document. Empty or missing row data removes the
whole region.

Two properties make this safe for untrusted templates: only literal values
supplied by application code are merged — no expression language, script
engine, or external data source is ever consulted — and scalar merges
replace the field's cached *result* while preserving the instruction, so the
document remains recognizable as a template.

For plain-text placeholders (`{{NAME}}`-style), a `ReplaceAll` sweep is
simpler — see [Text and paragraphs](text.md).
