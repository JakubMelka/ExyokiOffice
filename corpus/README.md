# Document corpus

Real Word, Excel and PowerPoint packages, saved by Microsoft Office and checked
into the repository as test fixtures. `manifest.json` describes every one of
them, and the `tests/corpus` layer is driven entirely from that manifest.

## Why the corpus exists

Every other test layer builds its fixtures with ExyokiOffice's own API. That is
enough to catch a round trip that loses what the library wrote, but not one that
loses what the library never writes: a pivot cache, a legacy VML comment shape,
a chart extension list, an `mc:Ignorable` namespace. A validator built from the
same metadata as the writer agrees with itself no matter how badly both misread
that metadata.

Packages Office produced are the independent evidence. A diagnostic raised
against a file in this directory is a statement about the library, because the
file is by definition what Office considers correct.

That is not a hypothetical. Seven distinct validator defects were found the
first time these files were run through `OpenXmlPackageValidator`, together
accounting for 631 spurious errors across ten of the fifteen documents — among
them reading `ST_LongHexNumber` attributes as decimal, treating a stripped
`not(...)` schematron test as its own opposite, and rejecting
`mc:AlternateContent` wherever it appeared.

## What the manifest records

`manifest.json` has one entry per file:

| Field | Meaning |
| --- | --- |
| `File` | Path relative to this directory. |
| `Family`, `DocumentType` | What the package inspector reports the file to be. |
| `Producer` | The application that saved it, read from `docProps/app.xml`. |
| `Purpose` | Why this file earns its place — what it covers that the others do not. |
| `Features` | Capability tags this fixture is the evidence for. |
| `MainPart` | URI of the main document part. |
| `PartCount`, `RelationshipCount`, `ContentTypeCount` | The shape of the part graph. |
| `FileSize`, `Sha256` | Pin the fixture itself, so a file re-saved by hand is noticed. |
| `RequiredParts` | The parts whose loss would make the fixture pointless. Not the full list. |

The counts and the checksum are a baseline, not a specification: they were read
off the files as Office wrote them. Their value is that a change which silently
drops a part, merges two content types, or stops following a relationship fails
with a number instead of passing quietly.

## What the tests check

`ctest -L corpus` runs the areas below.

**`Corpus.Manifest`** — every file on disk has an entry and every entry matches
the file: checksum, size, part count, relationship count, content type count,
main part URI, and the presence of each required part. Dropping a package into
this directory fails the suite until the manifest says what it is for.

**`Corpus.Validation.*`** — every document validates with no errors and no
warnings, against the current Office generation. `Corpus.Validation.TargetVersion`
validates one document per family against Office 2007 and requires that each one
*does* report violations, so a `TargetVersion` that quietly stopped applying
could not make the clean run pass for the wrong reason.

**`Corpus.RoundTrip.*`** — the matrix required by OPC-015. Each file goes through
open-save-open and the two packages are compared part by part:

- the set of part URIs, so nothing is added or dropped;
- the content type of each part, which is `[Content_Types].xml` seen from the
  other side;
- the SHA-256 of each part payload — an XML part's serialized tree, a binary
  part's buffer — rather than the retained input bytes, so a writer that mangles
  the tree cannot pass by comparing the input to itself;
- every relationship edge, by source, id, type, target and target mode;
- that a second save reproduces the first, so the written format is a fixed
  point rather than something that drifts with each edit;
- that the reopened package still validates, because keeping every part is not
  the same as keeping the markup inside them valid.

**`Corpus.Editors`** — the same open-save cycle through `WordDocumentEditor`,
`ExcelDocumentEditor` and `PowerPointDocumentEditor`. The package layer knows
nothing about the three families and so cannot drop a part because it does not
model one; the editors build a typed view on open and write it back on save,
and everything outside that view depends on being carried along untouched. This
area is where that assumption is checked against real documents.

## Cost

This is the slowest layer in the suite. A full DOM and schema validation of all
fifteen packages takes minutes in a Debug build — the particle matcher is
quadratic in the number of siblings, and these are real documents with hundreds
of them. The cases are written to pay that price as few times as possible:
errors and warnings are collected in one pass rather than two, and the
older-target case uses one document per family instead of all fifteen.

The areas are separate CTest entries, and the three that sweep every package —
validation, the content-model cross-check and the round trip — are registered
one entry per document, generated from `manifest.json` when CMake configures.
`ctest --parallel` therefore runs the documents concurrently and the layer costs
what its slowest single file costs, not the sum. `ctest -LE corpus` skips it
altogether while iterating on something else. Only the validation areas and the
round trip's revalidation carry the cost; the manifest and editor areas do no
schema validation at all and finish in seconds.

Adding a document adds its entries: the manifest is listed in
`CMAKE_CONFIGURE_DEPENDS`, so the next build reconfigures and the new
`Corpus.Validation.<Family>.<Name>` and siblings appear with no CMake edit.

## Coverage and known gaps

| Family | Files | Covers |
| --- | --- | --- |
| Word | 5 | Sections, three header/footer pairs, footnotes, endnotes, numbering, tables, PNG and JPEG images, fields, `w14` run properties. |
| Excel | 5 | Formulas and calculation chains, shared strings, tables, styles, conditional formatting in `x14` extension lists, classic and extended charts, drawings, pivot tables over a shared cache, legacy VML comments, threaded comments, cell metadata. |
| PowerPoint | 5 | Slides, layouts, masters, notes slides, notes masters, themes, table styles, thumbnails, JPEG media, layouts shared by several slides. |

Deliberately not covered yet, and worth adding when suitable files exist:

- macro-enabled variants (`.docm`, `.xlsm`, `.pptm`) and templates (`.dotx`,
  `.xltx`, `.potx`) — BLD-004 asks for these, and no fixture here is one;
- packages carrying digital signatures or encryption;
- deliberately corrupt packages — those live in `tests/fuzz/corpus`, where the
  fuzz targets replay them.

## Where the content came from

These files ship with the repository under its licence, which makes the content
inside them a licensing question and not only a testing one. All of it
originates here:

- **The text, the workbooks and the slide content** were written for this
  corpus. Every package records `Jakub Melka` as its `dc:creator`, and Office
  saved each one.
- **Every image** — 32 JPEG and PNG files across the Word and PowerPoint
  fixtures — is AI-generated, produced with Microsoft Copilot. Most still carry
  the C2PA manifest that records it, naming `Azure OpenAI ImageGen` as the
  software agent and `trainedAlgorithmicMedia` as the source type; the two
  photographs in `word/The_United_States_of_America.docx` lost theirs when Word
  recompressed them on save, which strips the metadata but says nothing about
  where the image came from.
- **The one signature**, in `word/DigitalSignature.docx`, is over a self-signed
  test certificate. No real identity is bound into that fixture.

Nothing here is stock photography, a downloaded image, a Microsoft template, or
an embedded font. Keep it that way — see below.

## Adding a fixture

1. Copy the file under `corpus/<family>/`. Keep it small and keep its content
   free of anything that is not yours to publish; these files ship with the
   repository under its licence. Images are the easy place to get this wrong:
   generate them or shoot them yourself rather than downloading one, and record
   which in the section above. A saved package can also carry an embedded font
   or a template's artwork without anyone intending it — check what parts the
   file actually has before committing it.
2. Read the numbers off it:
   `exyoki info --format json <file>` and `exyoki parts --format json <file>`.
3. Add a `manifest.json` entry, including a `Purpose` that says what this file
   covers that no existing one does. A fixture that duplicates another's
   coverage costs test time and buys nothing.
4. Run `ctest -L corpus`. If validation fails, the first question is whether the
   library is wrong — Office wrote the file.
