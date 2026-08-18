# Notes and comments

Speaker notes attach presenter text to a slide; comments attach review
remarks with an author and a position.

The namespace aliases from [Presentations](presentations.md) are assumed.

## Speaker notes

```cpp
slide->SetNotesText("Speaker notes for this slide.\nSecond paragraph.");
```

Newlines become separate paragraphs on the notes page. The first notes
slide also creates the presentation-wide notes master (with its own theme
part) and the `p:notesMasterIdLst` entry that PowerPoint expects alongside
it — without either PowerPoint repairs the file, so the editor maintains
them for you.

## Comments

```cpp
editor->AddCommentAuthor({.Id = "{7C0F1D8E-2B7A-4E5B-9C6D-1A2B3C4D5E6F}",
                          .Name = "Reviewer", .Initials = "R"});
slide->AddComment({.Id = "{0B9E4F21-6D3C-4A8B-8F1E-2C3D4E5F6A7B}",
                   .AuthorId = "{7C0F1D8E-2B7A-4E5B-9C6D-1A2B3C4D5E6F}",
                   .Text = "Looks good.", .Position = {100, 100}});
```

A comment must reference an existing author, so add the author first. The
schema-required creation timestamp is written automatically (current UTC
time); `UpdateComment` keeps the timestamp the thread already carried.

PowerPoint reads author, comment, and reply identifiers as GUIDs and
repairs a file that carries anything else, so `AddCommentAuthor`,
`AddComment`, and `UpdateComment` accept only braced GUIDs like the ones
above (`ExyokiOffice::Guid::New()` mints one; `Guid::IsBraced()` checks one)
and return `false` otherwise. Identifiers read from an existing file are
returned unchanged. Each comment is anchored to its slide (`pc:sldMkLst`),
and the slide records the comment part in its `p:extLst`
(`p188:commentRel`), the way PowerPoint itself does; editing a comment in a
file written without that extension adds it.

Text extraction across slides and notes — for indexing or review tooling —
is available from the command line as [exyoki](../tools/exyoki.md)
`extract-text`.
