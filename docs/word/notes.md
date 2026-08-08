# Footnotes, comments, and content controls

This chapter covers the annotation layer of a Word document: footnotes and
endnotes, reviewer comments, and structured document tags (content
controls).

```cpp
#include "ExyokiOffice/Word/WordDocument.hpp"
using namespace ExyokiOffice::Word;
namespace W = ExyokiOffice::DocumentFormat::OpenXml::Wordprocessing;
```

## Footnotes and endnotes

```cpp
auto note = paragraph->AddFootnote("Source: annual report, page 12.");
auto end  = paragraph->AddEndnote("Discussed further in the appendix.");
```

`AddFootnote`/`AddEndnote` create the footnotes or endnotes part on first
use, append the note with the next free ID, and insert the reference mark at
the end of the paragraph. Both return a `Note`:

```cpp
note->Kind();               // NoteKind::Footnote or Endnote
note->GetId();
note->Paragraphs();         // note content as ordinary paragraphs
note->AddParagraph("A second paragraph in the same note.");
note->PlainText();
note->SetText("Replacement text.");
note->Remove();             // removes the note and its reference mark
```

Enumeration and lookup go through the editor: `Footnotes()`, `Endnotes()`,
`FindFootnote(id)`, `FindEndnote(id)`. `GetEntryType()` distinguishes real
notes from the special separator entries the notes parts carry.

## Comments

Classic comments annotate a range of document text with an author, initials,
a timestamp, and rich content:

```cpp
auto comment = paragraph->AddCommentOnParagraph(
    "Please double-check these figures.",
    CommentAuthor{.Name = "Reviewer", .Initials = "RV"});
```

`AddCommentOnParagraph` anchors the comment range around the whole
paragraph. `AddComment(runs, author)` anchors it around a specific run
sequence within the paragraph instead. The returned `Comment` edits
everything in place:

```cpp
comment->GetId();
comment->GetAuthor();  comment->SetAuthor("J. Doe");
comment->GetInitials(); comment->SetInitials("JD");
comment->GetDate();     comment->SetDate(std::chrono::system_clock::now());
comment->Paragraphs();  comment->AddParagraph("Second paragraph.");
comment->SetText("Replacement text.");
comment->Remove();      // removes the comment and its range markers
```

`editor->Comments()` and `editor->FindComment(id)` enumerate and look up.

Threaded (modern) comments are edited through the same wrapper:

```cpp
auto reply = comment->AddReply("Fixed in revision 3.", author);
comment->Replies();      // direct replies, in document order
reply->GetParent();      // the comment it answers, or nullptr for a root
comment->SetResolved(true);
comment->IsResolved();
```

A reply is a comment in its own right — its own identifier, its own entry, and
its own range markers over the same span as its parent — so it also shows up in
`editor->Comments()`. What makes it a reply lives in the satellite parts Word
writes alongside `comments.xml`: `commentsExtended` records the thread shape
and the resolution flag, `commentsIds` and `commentsExtensible` the durable
identity and the UTC timestamp, and `people` the authors. ExyokiOffice writes
and maintains all four, so a document it produces threads correctly in Word.

Resolution belongs to the thread, not to one comment: `SetResolved()` marks
the root and every reply below it, whichever member you call it on. Removing a
comment removes its replies with it.

## Content controls

Content controls (structured document tags, `w:sdt`) mark a region of the
document with machine-readable identity — a tag, an alias, an ID — which is
the standard hook for programmatic filling:

```cpp
auto block = editor->Body().InsertContentControl("CustomerName", "Customer name");
block->SetText("Contoso Ltd.");

auto inline_ = paragraph->AddInlineContentControl("PolicyNumber", "Policy no.");
inline_->AddText("A-1024");
```

Block-level controls contain paragraphs; inline controls live inside a
paragraph and contain runs. The wrapper serves both:

```cpp
control->Level();               // Block or Inline
control->GetTag();      control->SetTag("CustomerName");
control->GetAlias();    control->SetAlias("Customer name");
control->GetId();       control->EnsureId();
control->SetLock(W::LockingValues::SdtContentLocked);  // protect content
control->SetShowingPlaceholder(false);
control->Paragraphs();  control->Runs();
control->SetText("Contoso Ltd.");                      // replaces the content
control->Remove();      // removes the control; use Clear() to empty it instead
```

`editor->ContentControls()` and `FindContentControl(id)` enumerate and look
up controls anywhere in the body.

Specialized controls — checkboxes, dropdowns, date pickers — are preserved
as opaque XML; `GetProperties()`/`GetContent()` expose the underlying
elements when you need to reach into one.
