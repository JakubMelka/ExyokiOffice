# Notes and comments

Speaker notes attach presenter text to a slide; comments attach review
remarks with an author and a position.

The namespace aliases from [Presentations](presentations.md) are assumed.

## Speaker notes

```cpp
slide->SetNotesText("Speaker notes for this slide.\nSecond paragraph.");
```

Newlines become separate paragraphs on the notes page. The first notes
slide also creates the presentation-wide notes master and the
`p:notesMasterIdLst` entry that PowerPoint expects alongside it — without
that entry PowerPoint repairs the file, so the editor maintains it for you.

## Comments

```cpp
editor->AddCommentAuthor({.Id = "author-1", .Name = "Reviewer", .Initials = "R"});
slide->AddComment({.Id = "c1", .AuthorId = "author-1",
                   .Text = "Looks good.", .Position = {100, 100}});
```

A comment must reference an existing author, so add the author first. The
schema-required creation timestamp is written automatically (current UTC
time); `UpdateComment` keeps the timestamp the thread already carried.

Text extraction across slides and notes — for indexing or review tooling —
is available from the command line as [exyoki](../tools/exyoki.md)
`extract-text`.
