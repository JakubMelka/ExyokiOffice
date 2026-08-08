# exyoki — ExyokiOffice command-line utility

`exyoki` is a command-line tool for inspecting, validating, unpacking,
repacking, and editing Office Open XML packages (`.docx`, `.xlsx`, `.pptx`)
built on top of the ExyokiOffice library. It is designed to be scriptable
and AI-friendly: every command accepts `--format plain|markdown|json|xml` and
returns a distinct process exit code depending on the outcome, so it can be
used both interactively and as a building block in larger pipelines (CI
checks, batch document audits, agent tool-calling, etc.). The interface
describes itself too — [`exyoki commands`](#commands--describe-the-interface-machine-readably)
lists every command, option and exit code as data, so a caller never has to
parse `--help`.

The CLI is a thin adapter (`tools/exyoki/`) over a reusable library module,
`ExyokiOffice::Tools` (`include/ExyokiOffice/Tools/`,
`sources/Tools/`), which any C++ program linking against `ExyokiOffice`
can use directly without going through the CLI at all — see
[Using the Tools library from C++](#using-the-tools-library-from-c) below.

## Building and installing

`exyoki` is built automatically alongside the main library:

```powershell
cmake -S . -B build
cmake --build build --config Debug
```

It is controlled by the `EXYOKIOFFICE_BUILD_TOOL` CMake option (default
`ON`). To disable it:

```powershell
cmake -S . -B build -DEXYOKIOFFICE_BUILD_TOOL=OFF
```

The built executable lives at `build/tools/exyoki/<config>/exyoki.exe`
(alongside a copy of `ExyokiOffice.dll`, placed there automatically by the
build). Installing the project also installs `exyoki`:

```powershell
cmake --install build --prefix C:/ExyokiOffice
# -> C:/ExyokiOffice/bin/exyoki.exe
# -> C:/ExyokiOffice/bin/ExyokiOffice.dll
```

`exyoki` is intentionally **not** part of the `ExyokiOffice::ExyokiOffice`
CMake export set — it is a standalone executable, not a library consumers
link against.

## Global options

These apply to every subcommand and can be given either before or after the
subcommand name:

```
exyoki --format json info report.docx
exyoki info report.docx --format json
```

| Option | Default | Description |
| --- | --- | --- |
| `--format {plain,markdown,json,xml}` | `plain` | Output format for the report. |
| `--output <path>` | `-` (stdout) | Where to write the rendered report. |
| `--package-limits {recommended,unlimited}` | `recommended` | ZIP/XML safety limits applied to every package read. |
| `--quiet` | off | Suppress human-readable diagnostics on stderr (plain/markdown only). |
| `--version` | — | Print the exyoki version and exit. |
| `--help` | — | Print usage and exit (also available per-subcommand). |

`--package-limits` decides what the tool refuses to read. `recommended` is
[`OpenXmlPackageLimits::Recommended()`](../../include/ExyokiOffice/OpenXmlPackage.hpp):
at most 10 000 ZIP entries, 256 MiB compressed, 2 GiB uncompressed, 512 MiB per
part, a 200:1 compression ratio, and 256 levels of XML nesting. Those bounds are
far above any real document and are what stops a decompression bomb or deeply
nested XML from exhausting memory or the stack — a command line utility is
routinely pointed at files that arrived by mail or download, so the guard is on
by default. `unlimited` switches it off for the cases where the limit is what
stands in the way, such as forensic work on a damaged package or a legitimately
enormous workbook; it prints a warning unless `--quiet` is given. The limits
apply to every command, including `unpack`, which reads the archive directly
rather than through the package loader.

The same bound is the default inside the library too: the `ExyokiOffice::Tools`
module that backs these commands loads packages under `Recommended()` whether or
not a front end asked for it, so a program embedding `Tools::Stat` or
`Tools::Diff` is not unprotected merely for not having read this page. See
`Tools::DefaultPackageLimits` and [SECURITY.md](../../SECURITY.md).

No command redefines any of these names, so their meaning never depends on
where they sit on the command line. `--output` is always the *report*
destination; a command that writes a document writes it to `--out-package`
(`merge`, `props set`, `replace`, `redact`, `fill`, `recalc`, `compare`) or to
a positional `outpackage`/`outdir` (`pack`, `from-flat-opc`, `dedup`,
`unpack`, `split`, `export-media`). The `commands` catalog fails, and with it
the build's `Tool.CommandCatalog` test, if a command ever reintroduces a
shadowing name.

## Output model

Every command produces one report with the same shape regardless of format:

- **`command`** — the subcommand name.
- **`status`** — `"ok"` or `"error"` (whether the operation itself completed;
  a `validate` run that finds errors is still `status: ok` — see
  [Exit codes](#exit-codes)).
- **`data`** — command-specific payload (parts list, validation issues, ...).
- **`diagnostics`** — a list of `{severity, message, context}` entries.

In `plain`/`markdown` mode, the report goes to stdout (or `--output`) and
diagnostics are additionally printed to stderr (suppressed by `--quiet`). In
`json`/`xml` mode, diagnostics are only in the envelope, so stderr can be
ignored (and `--quiet` has no additional effect).

The JSON envelope looks like:

```json
{
  "tool": "exyoki",
  "toolVersion": "1.0.0",
  "command": "info",
  "status": "ok",
  "data": { "...": "..." },
  "diagnostics": []
}
```

The XML rendering mirrors the same structure under a root `<exyoki>` element.
`toolVersion` reports the library version the tool was built from, so the
examples on this page show whatever release you are reading about.

## Exit codes

| Code | Meaning |
| --- | --- |
| `0` | Success. For `validate`: no errors found. For `diff`/`compare`: packages/documents identical. For `search`: at least one match. |
| `1` | Operation failed (file not found, I/O error, wrong document family for a family-specific command, ...). |
| `2` | CLI usage error (bad arguments — reported by CLI11). |
| `3` | `validate` found one or more errors (or warnings, with `--warnings-as-errors`); `schema --check` found the document does not conform. |
| `4` | `diff` found at least one difference; `compare` created at least one tracked revision. |
| `5` | `search` found no matches (grep-like convention). |
| `6` | `query` ran successfully but matched no elements (grep-like convention). |
| `7` | An unexpected exception escaped the command. |
| `8` | `signatures` found a signature whose signed content has changed. |

The same table, with a stable machine-readable name per code and the subset each
command can return, is part of the [`commands`](#commands--describe-the-interface-machine-readably)
output.

## Commands

### `commands` — describe the interface machine-readably

```
exyoki commands
```

Emits the whole command line interface as data: the global options, the exit
code table, and every command with its description, positionals, options, value
constraints, defaults, and the exit codes that command can return. It is meant
for programs that drive `exyoki` — CI scripts, wrappers, and AI agents doing
tool discovery — so they never have to scrape `--help`:

```
$ exyoki commands --format json
```

```json
{
  "tool": "exyoki",
  "toolVersion": "1.0.0",
  "command": "commands",
  "status": "ok",
  "data": {
    "schemaVersion": 1,
    "description": "exyoki - ExyokiOffice command-line utility for OPC packages",
    "globalOptions": [
      {
        "name": "--format",
        "positional": false,
        "names": ["--format"],
        "description": "Output format",
        "required": false,
        "repeatable": false,
        "flag": false,
        "valueType": "TEXT",
        "choices": ["plain", "markdown", "json", "xml"],
        "default": "plain"
      }
    ],
    "exitCodes": [
      { "code": 0, "name": "ok", "meaning": "The command completed successfully." }
    ],
    "commandCount": 28,
    "commands": [
      {
        "name": "search",
        "path": "search",
        "description": "Search text in a Word, Excel, or PowerPoint document (paragraphs, cells, slide shapes, notes)",
        "usage": "exyoki search <package> <needle> [options]",
        "requiresSubcommand": false,
        "positionals": [
          {
            "name": "package",
            "positional": true,
            "description": "Path to the .docx/.xlsx/.pptx package",
            "required": true,
            "repeatable": false,
            "flag": false,
            "valueType": "TEXT"
          }
        ],
        "options": [
          {
            "name": "--ignore-case",
            "positional": false,
            "names": ["--ignore-case"],
            "description": "Case-insensitive match (plain text or regex)",
            "required": false,
            "repeatable": false,
            "flag": true
          }
        ],
        "exitCodes": [0, 1, 2, 5, 7]
      }
    ]
  }
}
```

Field notes:

- **`schemaVersion`** — bumped whenever the shape of this payload changes
  incompatibly, so a consumer can refuse a version it does not understand.
- **`commandCount`** — the number of top-level commands, i.e. the length of
  `commands`. A command that groups others (`props`) carries them in its own
  `subcommands` array and sets `requiresSubcommand`.
- **`path`** — how the command is spelled on the command line, including the
  group for nested ones (`props set`). Names are unique by path.
- **`flag`** — the option takes no value. Flags have no `valueType`.
- **`repeatable`** — the option may be given more than once and accumulates.
- **`choices`** / **`constraint`** — the accepted values, either as an explicit
  list or as a free-form description such as `INT in [0 - 9]` for a range.
- **`exitCodes`** — the codes this command can return, a subset of the top-level
  `exitCodes` table.

The catalog is derived from the parser the tool actually runs on, not from a
list maintained beside it, so it cannot describe a command that does not exist
or miss an option that does. A CTest entry (`Tool.CommandCatalog`) additionally
checks that everything `--help` offers is present here.

Since `--output` and `--format` apply as usual, a wrapper can cache the catalog:

```powershell
exyoki commands --format json --output exyoki-commands.json
```

### `parts` — list every part

```
exyoki parts <package> [--sort uri|size|type]
```

```
$ exyoki parts report.docx --sort size
command: parts
status: ok
data:
  partCount: 9
  parts:
    - uri=/word/document.xml, contentType=application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml, kind=xml, size=12345, descriptor=MainDocumentPart
    ...
```

### `relationships` — list relationship edges

```
exyoki relationships <package> [--part <uri>]... [--dangling-only]
```

`--part` restricts the listing to relationships whose *source* is one of the
given part URIs (repeatable). `--dangling-only` shows only relationships
whose target does not resolve to an existing part (external relationships
are never considered dangling).

```
$ exyoki relationships report.docx --dangling-only --format json
{
  "tool": "exyoki", "toolVersion": "1.0.0", "command": "relationships", "status": "ok",
  "data": { "relationshipCount": 10, "danglingCount": 0, "relationships": [] },
  "diagnostics": []
}
```

### `info` — package summary and properties

```
exyoki info <package> [--props-only]
```

Detects the document family (Word/Excel/PowerPoint) by following the
package's root `officeDocument` relationship and matching the target part's
content type, then reports part/relationship counts, total size, and core +
extended document properties. `--props-only` omits the summary fields and
reports only `properties`.

`strictConformance` is `true` for an ISO 29500 Strict package. Such a package
reports `family: unknown` — only the Transitional conformance class is
supported, see [Compatibility.md](../Compatibility.md) — and the field lets a
caller tell that apart from a genuinely unrecognized file.

```
$ exyoki info report.docx
command: info
status: ok
data:
  family: word
  documentType: Document
  strictConformance: false
  mainPartUri: /word/document.xml
  mainPartContentType: application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml
  partCount: 9
  relationshipCount: 10
  totalPartSize: 65587
  properties:
    title: Quarterly report
    creator: Acme Corp.
    ...
```

### `props get` / `props set` — read/write document properties

```
exyoki props get <package>
exyoki props set <package> [--title T] [--creator C] [--subject S] [--keywords K]
                            [--set name=value]... [--out-package <path>]
```

`--set` accepts any of `Title`, `Subject`, `Creator`, `Keywords`,
`Description`, `LastModifiedBy`, `Category`, `ContentStatus` (core
properties, case-insensitive), or `Company` (an extended property); it can
be repeated for multiple arbitrary properties. `--title`/`--creator`/
`--subject`/`--keywords` are shortcuts for the same. Without `--out-package`,
the package is updated in place. `props set` never creates a properties part
that doesn't already exist — virtually every real Word/Excel/PowerPoint file
has one; if a package genuinely lacks core/extended properties, the affected
property update reports `updated: false` with a diagnostic.

```
$ exyoki props set report.docx --title "New Title" --out-package retitled.docx
command: props set
status: ok
data:
  name: Title
  value: New Title
  updated: true
```

### `validate` — OPC + DOM/schema validation

```
exyoki validate <package>... [--office-version 2007|2010|2013|2016|2019|2021|365]
                             [--no-dom] [--max-issues N] [--errors-only]
                             [--warnings-as-errors] [--cross-check-content-model]
```

Runs OPC-level checks (dangling relationships, duplicate part URIs, content
type mismatches, ...) plus, unless `--no-dom` is given, full DOM/schema
validation targeting the selected Office generation (default `365`, the most
permissive). Validating the same document against an older `--office-version`
can surface *more* issues, because elements and attributes introduced by a later
Office generation are not available for an older target.

```
$ exyoki validate report.docx; echo $?
command: validate
status: ok
data:
  loaded: true
  errorCount: 2
  warningCount: 0
  issues:
    - severity=error, domain=schema, errorId=AttributePatternMismatch, message=Attribute 'w:val' must be an even-length sequence of hexadecimal digits., partUri=/word/document.xml, xmlPath=/w:document/w:body/w:p[7]/w:r[6]/w:rPr/w:color
    - severity=error, domain=schema, errorId=AttributeExactLengthMismatch, message=Attribute 'w:val' must have length 3 octets., partUri=/word/document.xml, xmlPath=/w:document/w:body/w:p[7]/w:r[6]/w:rPr/w:color
3
```

Length facets of `xsd:hexBinary` attributes are reported in octets, which is how
the schemas express them: a colour of length 3 is the six hex digits of
`C00000`.

`xmlPath` addresses a single element: it carries positional predicates, so
`w:p[7]/w:r[6]` is the sixth run of the seventh paragraph and not merely "some
run of some paragraph".
Prefixes are normalized to the canonical Open XML ones, whatever prefixes the
document itself declared. JSON output additionally carries `attributeName` for
issues about a specific attribute and `constraintId` for the schema or
schematron rule that produced them.

Exit code `3` is returned whenever `errorCount > 0` (or `warningCount > 0`
with `--warnings-as-errors`); `1` is returned only when the package could not
be loaded at all (`loaded: false`).

Besides schema rules, the run asserts that every XML part really is one
well-formed XML document. A part carrying two root elements is accepted by
most pull parsers — including the one this library uses — but rejected by
Word and by conforming XML parsers, so it is reported as an error
(`OpcMalformedPartXml`) rather than passing silently.

An ISO 29500 Strict package is likewise rejected, with
`PackageStrictConformanceUnsupported`. Only the Transitional conformance class
is implemented (see [Compatibility.md](../Compatibility.md)); the OPC
container of a Strict file is structurally fine, but a clean validation report
would say "this file is supported", which it is not.

**Checking the validator itself.** Whether an element's children satisfy its
schema content model is decided by an automaton compiled from the schema, and
the library keeps the older recursive matcher that answers the same question a
much slower way. `--cross-check-content-model` runs both on every element and
reports a `ContentModelCrossCheckMismatch` wherever they disagree — which is a
defect in this library, not in the document, and worth reporting as one. It is
off by default because it pays for both matchers; expect a run to take minutes
rather than seconds on a large document.

```
$ exyoki validate --cross-check-content-model report.docx
command: validate
status: ok
data:
  loaded: true
  errorCount: 0
  warningCount: 0
```

**Batch mode.** `validate` accepts several packages, and `*`/`?` wildcards in
the filename component are expanded by the tool itself — Windows shells pass
them through verbatim, so `exyoki validate *.docx` works the same in
PowerShell, `cmd`, and POSIX shells (where the shell usually expands them
first; both spellings behave identically). A single input keeps the report
shape shown above; several inputs produce one entry per file plus totals:

```
$ exyoki validate reports\*.docx; echo $?
command: validate
status: ok
data:
  fileCount: 3
  filesFailedToLoad: 0
  totalErrorCount: 2
  totalWarningCount: 0
  files:
    - file: reports\april.docx
      loaded: true
      errorCount: 0
      warningCount: 0
      issues:
    - file: reports\may.docx
      loaded: true
      errorCount: 2
      warningCount: 0
      issues:
        - severity=error, domain=schema, ...
    ...
3
```

The exit code is the worst outcome across all files: `1` when any file failed
to load, else `3` when any file has errors, else `0`. `--max-issues` caps the
issue list per file, not in total. A wildcard that matches nothing adds a
warning diagnostic; when no input remains at all, the command fails with
exit code `1`.

### `signatures` — list digital signatures and check the signed content

```
exyoki signatures <package>
```

Lists every XML signature the package carries, with its identifier, algorithms,
signing time, number of certificates, and the state of each reference.

The command checks **content integrity only**: it recomputes every digest and
answers whether a signed part has changed since it was signed. It never reports
on the signature value, because verifying that needs a private-key backend and
the command line has none — see [Signatures.md](../Signatures.md) for the
`ICryptoProvider` interface that the library API takes for that.

```
$ exyoki signatures signed.docx; echo $?
command: signatures
status: ok
data:
  signatureCount: 1
  contentIntact: true
  signatures:
    - partUri=/_xmlsignatures/sig1.xml, signatureId=idPackageSignature, contentIntegrity=valid, signatureValue=notChecked, signingTime=2026-07-27T09:12:44Z
  issues:
0
```

Exit code `8` is returned when the package carries a signature whose content no
longer matches; a package without signatures is reported with
`signatureCount: 0` and exit code `0`.

**There is deliberately no `exyoki sign` command.** Creating a signature needs
a private key and a cryptographic backend, and the CLI links no cryptography
at all — that keeps the tool dependency-free and keeps key handling out of
shell history and scripts. Signing is available through the library API:
implement `ICryptoProvider` and use `ExyokiOffice::Security` as described in
[Signatures.md](../Signatures.md).

### `external` — list resources referenced from outside the package

```
exyoki external <package>
```

Lists every OPC relationship whose target lies outside the package: linked
images and media, an attached template, workbooks an Excel file links to,
hyperlinks, and linked OLE objects. `kind` is derived from the relationship
type.

The command **accesses nothing**. ExyokiOffice never follows an external
target on its own, and the command line supplies no resolver, so this reports
what the document claims to reference — the audit worth running before deciding
whether to allow any of it. See
[ExternalResources.md](../ExternalResources.md) for the
`IExternalResourceResolver` interface and the allowlist policy the library API
takes for actually reading such a target.

```
$ exyoki external report.docx; echo $?
command: external
status: ok
data:
  referenceCount: 1
  references:
    - sourcePartUri=/word/document.xml, relationshipId=rId5, kind=Hyperlink, target=https://en.wikipedia.org/wiki/Office_Open_XML
0
```

A package with no external references is reported with `referenceCount: 0` and
exit code `0`; only a package that fails to load is an error.

### `unpack` / `pack` — round-trip a package to/from a directory tree

```
exyoki unpack <package> <outdir> [--pretty] [--overwrite]
exyoki pack <indir> <outpackage> [--regenerate-content-types] [--validate] [--compression 0-9] [--overwrite]
```

`unpack` extracts every ZIP entry to `<outdir>`, using the entry name as the
relative disk path so `[Content_Types].xml` and every `_rels/*.rels` file
keep their exact on-disk layout. Entry names that aren't valid Windows paths
(reserved device names like `CON`, forbidden characters, `..` segments) are
percent-escaped; whenever that happens — or `--pretty` was requested — a
small manifest (`_exyoki.manifest.xml`) is written into `<outdir>` recording
the original entry names, which `pack` reads back automatically.

**Fidelity guarantee:** `unpack` (without `--pretty`) followed by `pack`
(default options) reproduces the original ZIP entries **byte-for-byte** —
verify this yourself with `exyoki diff original.docx repacked.docx` (exit
code `0` means identical). With `--pretty` (XML/`.rels` entries re-indented
for readability), the round-trip is only *semantically* equivalent.

```
$ exyoki unpack report.docx unpacked/
command: unpack
status: ok
data:
  entryCount: 12
  manifestWritten: false

$ exyoki pack unpacked/ repacked.docx
command: pack
status: ok
data:
  entryCount: 12
  contentTypesRegenerated: false

$ exyoki diff report.docx repacked.docx; echo $?
command: diff
status: ok
data:
  identical: true
  ...
0
```

By default `pack` preserves `[Content_Types].xml` from the tree verbatim.
`--regenerate-content-types` (or a missing `[Content_Types].xml`) rebuilds it
instead, from a small built-in file-extension table — a best-effort fallback,
not a full content-type inference engine.

An existing `<outpackage>` is an error; `--overwrite` replaces it. Packing a
tree back over the document it came from is the normal way to lose the
original, so it takes the same explicit flag as every other command that
writes.

### `to-flat-opc` / `from-flat-opc` — convert to/from a single Flat OPC XML file

```
exyoki to-flat-opc <package> <outfile> [--no-pretty]
exyoki from-flat-opc <flatopc> <outpackage> [--compression 0-9]
```

Flat OPC stores an entire Office package as one XML document: XML parts are
embedded directly and binary parts are base64-encoded. This makes packages
convenient to keep in source control, review with text diffs, or process with
XML tools. The format follows Microsoft's `xmlPackage` convention and can be
consumed by Office and other Flat OPC implementations.

`to-flat-opc` writes readable indented XML by default; `--no-pretty` produces
compact output. `from-flat-opc` accepts any prefix bound to the Flat OPC
namespace and ignores whitespace in base64 data.

**Fidelity guarantee:** the round-trip is semantically equivalent, not
byte-identical. XML parts are parsed and serialized again, so formatting and
attribute quoting can change, while part content types are preserved.

Which parts count as XML is decided from the content type and the extension,
the same way the package layer decides it — not by trying to parse the bytes.
The difference is visible on a VML drawing: it is a binary part whose bytes
happen to be well-formed XML, and sniffing would send it through the XML
serializer and return it with an XML declaration it never had. Every binary
part therefore comes back byte-for-byte, however XML-shaped its content is.

Verify semantic equivalence with the default normalized `diff`:

```console
$ exyoki to-flat-opc report.docx report.flat.xml
$ exyoki from-flat-opc report.flat.xml rebuilt.docx
$ exyoki diff report.docx rebuilt.docx; echo $?
0
```

### `convert` — convert between Office and AI-friendly formats

```
exyoki convert <input> <outputfile> [--from fmt] [--to fmt] [--media-dir DIR]
               [--embed-media] [--no-media] [--overwrite]
               [--sheet NAME] [--csv-separator SEP]
```

Converts between Office packages (`.docx`, `.xlsx`, `.pptx`) and semantic
text formats — Markdown, JSON, plain text, semantic XML, and (for workbooks)
CSV — in **both directions**, through a shared semantic document model. The exact schemas,
Markdown conventions, and a construct-by-construct fidelity matrix are
specified in [conversion-formats.md](conversion-formats.md). For a
whole-package XML round trip that keeps every part use
`to-flat-opc`/`from-flat-opc`
instead; `convert` optimizes for clean, structure-preserving output that AI
agents and scripts can read and regenerate.

Both endpoint formats are inferred from the file extensions and can be
overridden with `--from`/`--to` (`docx`, `xlsx`, `pptx`, `md`, `json`,
`txt`, `xml`, `csv`):

| Extension | Format |
| --- | --- |
| `.docx` `.docm` `.dotx` `.dotm` | `docx` |
| `.xlsx` `.xlsm` `.xltx` `.xltm` | `xlsx` |
| `.pptx` `.pptm` `.potx` `.potm` | `pptx` |
| `.md` `.markdown` | `md` |
| `.json` / `.xml` / `.txt` `.text` / `.csv` | `json` / `xml` / `txt` / `csv` |

Supported pairs: Office → text format, text format → Office, and
`json`/`xml` input → any text format (the envelope carries the family).
Markdown/text/CSV input needs an Office output extension (or `--to`) to fix
the target family; plain text imports to Word only; CSV maps to the Excel
family only in both directions; Office → Office is a usage error (exit code
`2`).

**CSV specifics** (see [conversion-formats.md](conversion-formats.md) for the
exact rules): `xlsx → csv` exports one worksheet — the first by default, with
a warning when the workbook has more; `--sheet` picks another. Rows are RFC
4180: fields with separators, quotes, or line breaks are quoted, rows end in
CRLF, formula cells emit their cached result, booleans emit `TRUE`/`FALSE`.
`csv → xlsx` creates a single-worksheet workbook (`--sheet` names it) with
conservative type inference: `TRUE`/`FALSE` become booleans, plain decimal
numbers become numbers, and everything else stays text — including
leading-zero codes such as `007` and anything starting with `=` (CSV import
never creates formulas). `--csv-separator` changes the delimiter in both
directions (e.g. `--csv-separator ";"`).

An `<outputfile>` of `-` writes the **converted payload itself** to stdout
(not the report envelope); this requires an explicit `--to` with a text
format. Diagnostics still go to stderr unless `--quiet`.

Media handling: exporting to md/json/xml writes images to a media directory
(default `<output stem>_media` next to the output) and references them
relatively; `--embed-media` inlines base64 payloads into json/xml instead;
`--no-media` drops them. Importing resolves the references relative to the
input file's directory (or `--media-dir`).

Content the semantic model does not capture (charts, content controls,
tracked revisions, ...) is dropped with a `warning` diagnostic naming the
location — nothing is dropped silently. JSON keeps the most detail; see the
fidelity matrix in [conversion-formats.md](conversion-formats.md).

```
$ exyoki convert report.docx report.md
command: convert
status: ok
data:
  input: report.docx
  output: report.md
  from: docx
  to: md
  family: word
  blockCount: 24

$ exyoki convert report.md report.docx        # Markdown -> new Word document
$ exyoki convert data.xlsx data.json          # workbook -> semantic JSON
$ exyoki convert data.json rebuilt.xlsx       # semantic JSON -> workbook
$ exyoki convert slides.pptx - --to md        # slide deck as Markdown on stdout
$ exyoki convert report.docx report.json --embed-media --format json
$ exyoki convert data.xlsx data.csv           # first worksheet as CSV
$ exyoki convert data.xlsx - --to csv --sheet "Q3"   # named sheet to stdout
$ exyoki convert data.csv imported.xlsx       # CSV -> new workbook
```

```
$ exyoki convert data.xlsx data.csv
command: convert
status: ok
data:
  input: data.xlsx
  output: data.csv
  from: xlsx
  to: csv
  family: excel
  sheetCount: 2
  cellCount: 57
```

```
$ exyoki convert data.xlsx - --to csv
Name,Value
"Widget, Large",42
Total,52
```

### `export-media` — extract images/audio/video/embedded objects

```
exyoki export-media <package> <outdir> [--overwrite]
```

Exports every part whose content type starts with `image/`, `audio/`, or
`video/`, plus OLE/embedded-package parts. File extensions are derived from
the part's own file name when possible, but for images the actual byte
signature (PNG/JPEG/GIF/BMP) always wins over a claimed extension — many
tools (including ExyokiOffice itself) store images under a generic `.bin`
name, so sniffing is what actually produces a usable file. Name collisions
get a numeric suffix (`image1.png`, `image1-2.png`, ...).

The name comes out of the package, which is not trusted input, so it has to be
usable as one plain file name: a part called `NUL.png` or `image1:hidden.png`
is renamed — writing to a device reports success and stores nothing, and a
stream suffix hides the payload inside a file that looks ordinary. Every rename
is reported as a warning naming the part it came from.

```
$ exyoki export-media report.docx media/
command: export-media
status: ok
data:
  itemCount: 1
  items:
    - partUri=/media/image1.bin, outputPath=media/image1.png, contentType=image/png, size=55350
```

### `dedup` — merge byte-identical shared resources

```
exyoki dedup <package> [<outpackage>] [--dry-run] [--overwrite] [--fonts] [--all-binary]
```

By default this finds byte-identical image, audio, and video parts with the
same content type and keeps only one copy of each payload. Every relationship
that targeted a duplicate is redirected to the canonical part while retaining
its relationship ID, so existing `r:embed`/`r:id` references remain valid.
The lexicographically smallest part URI is kept and the now-unreachable copies
are removed.

Only leaf binary parts are candidates: XML and parts with outgoing
relationships or child parts are never merged. `--fonts` also considers
embedded font parts (off by default because Office fonts are commonly
obfuscated per document). `--all-binary` considers every leaf binary part and
supersedes the media filters, but retains the XML/relationship safety rule.

Without `outpackage`, the input is rewritten in place. A separate existing
output is protected unless `--overwrite` is present. Start with `--dry-run` to
report duplicate groups and potential savings without modifying or writing
anything:

```
$ exyoki dedup report.docx --dry-run
command: dedup
status: ok
data:
  dryRun: true
  groupCount: 1
  removedParts: 0
  rewrittenRelationships: 0
  bytesSaved: 55350
```

The group details identify the content type, retained URI, duplicate URIs, and
one-copy payload size. In a dry run, `bytesSaved` is the potential saving while
`removedParts` and `rewrittenRelationships` stay zero because nothing was
changed. In a real run all three fields describe the completed operation.

### `search` — find text in a Word, Excel, or PowerPoint document

```
exyoki search <package> <needle> [--context N] [--regex] [--ignore-case]
```

The document family is detected from the package and the search covers the
same scope as `extract-text`:

- **Word** — every paragraph reachable from the document: body, table cells
  (including nested tables), headers/footers of every section, footnotes,
  endnotes, and comments. Labels carry the scope: `body: body paragraph 1`.
- **Excel** — every stored non-blank cell of every worksheet, with
  shared-string cells resolved; number/date/formula cells are matched by
  their stored value text. Labels are cell references: `Sheet1!B2`.
- **PowerPoint** — every text-frame paragraph of every slide shape plus the
  speaker notes: `slide 1 shape 2 paragraph 1`, `slide 1 notes`.

Exit code `5` (no matches) mirrors `grep`. Matching never crosses a
paragraph, cell, or notes-page boundary.

`--regex` treats `<needle>` as an ECMAScript regular expression (the
`std::regex` default grammar) instead of a literal substring. `--ignore-case`
makes either mode case-insensitive.

```
$ exyoki search report.docx "Quarterly"
command: search
status: ok
data:
  family: word
  matchCount: 2
  matches:
    - label=body: body paragraph 1, offset=0, matchText=Quarterly, context=Quarterly report — Q3 2026
    - label=header: header (default) paragraph 1, offset=0, matchText=Quarterly, context=Quarterly report

$ exyoki search book.xlsx "total" --ignore-case
command: search
status: ok
data:
  family: excel
  matchCount: 2
  matches:
    - label=Summary!A9, offset=0, matchText=Total, context=Total
    - label=Q3!C1, offset=6, matchText=total, context=Grand total (EUR)

$ exyoki search report.docx "Q[1-4] \d{4}" --regex
command: search
status: ok
data:
  family: word
  matchCount: 1
  matches:
    - label=body: body paragraph 1, offset=11, matchText=Q3 2026, context=Quarterly report — Q3 2026
```

### `query` — run a dynamic XPath query over a package part

```
exyoki query <package> <xpath> [--part <partUri>] [--ns prefix=uri]... [--max N]
```

Evaluates an XPath 1.0 expression against one XML part of any OPC package
(Word, Excel, or PowerPoint) and reports each matched element's location,
prefixed name, attributes, and aggregated inner text. This is the dynamic,
weakly-typed complement to the strongly-typed DOM: you can reach any element
by name without knowing its C++ type. The XPath engine is the vendored
pugixml XPath 1.0 implementation, so the full grammar is available (`//`,
positional predicates `[n]`, `contains()`, `last()`, unions, and so on).

Matching is **namespace-precise, not prefix-literal**. A prefixed name test
such as `w:p` or `@w:val` is resolved to its namespace URI and matched by
that URI, so a query keeps working even when the document happens to declare
the namespace under a different prefix. Prefixes resolve in this order:
`--ns` bindings (repeatable, `prefix=uri`), then the queried part's own
`xmlns:` declarations, then a built-in table of well-known Open XML prefixes
(`w`, `r`, `a`, `p`, `c`, `mc`, ...). An unbound prefix or a malformed
expression fails with a diagnostic and exit code `1`; a well-formed query
with no matches exits `6` (grep-like).

`--part` selects the part URI to query (for example `/word/styles.xml` or
`/xl/worksheets/sheet1.xml`); it defaults to the package's main document
part. `--max` caps the number of returned matches (`0` = unlimited). Note
that XPath expressions beginning with `/` are ordinary positional arguments —
quote them on shells that treat a leading slash specially.

```
$ exyoki query report.docx "//w:p" --max 2
command: query
status: ok
data:
  part: /word/document.xml
  matchCount: 2
  matches:
    - path=/w:document/w:body/w:p, name=w:p, attributes=, text=Quarterly report
    - path=/w:document/w:body/w:p, name=w:p, attributes=w:rsidR=00AA11, text=Q3 2026 summary

$ exyoki query report.docx "//w:hyperlink" --format json --max 1
{
  "tool": "exyoki", "toolVersion": "1.0.0", "command": "query", "status": "ok",
  "data": {
    "part": "/word/document.xml",
    "matchCount": 1,
    "matches": [
      { "path": "/w:document/w:body/w:p/w:hyperlink", "name": "w:hyperlink",
        "attributes": "r:id=rId5", "text": "Office Open XML on Wikipedia" }
    ]
  },
  "diagnostics": []
}
```

Query any part of any family, and bind extra prefixes as needed:

```
exyoki query book.xlsx "//c[@t='s']" --part /xl/worksheets/sheet1.xml
exyoki query deck.pptx "//a:t" --part /ppt/slides/slide1.xml
exyoki query report.docx "//x:t" --ns x=http://schemas.openxmlformats.org/wordprocessingml/2006/main
```

### `extract-text` — dump all readable text (Word, Excel, PowerPoint)

```
exyoki extract-text <package>
```

Dispatches by document family:

- **Word** — the same body/table/header/footer/footnote/endnote/comment walk
  as `search`, one block per paragraph, labeled e.g. `body: body paragraph 1`.
- **PowerPoint** — one block per shape with text (`slide 1 shape 2`) plus one
  block per non-empty speaker-notes page (`slide 1 notes`).
- **Excel** — one block per non-blank cell, labeled `SheetName!A1`, with
  shared-string cells resolved to their text automatically.

```
$ exyoki extract-text deck.pptx --format json | python -m json.tool
{
  "...": "...",
  "data": {
    "family": "powerpoint",
    "blockCount": 3,
    "blocks": [
      {"label": "slide 1 shape 1", "text": "Quarterly Report"},
      {"label": "slide 1 notes", "text": "Remember to mention Q4 targets."}
    ]
  }
}
```

### `stat` — content statistics (Word, Excel, PowerPoint)

```
exyoki stat <package>
```

Computes aggregate content statistics. Which fields appear in `data` depends
on the document family — a spreadsheet has no "headings" and a presentation
has no "footnotes":

- **Word** — `wordCount`, `characterCount`, `paragraphCount`, `headingCount`
  (style ID `Title` or `Heading1`-`Heading9`), `tableCount` (including nested
  tables), `imageCount`, `equationCount` (`m:oMath` elements), `footnoteCount`,
  `endnoteCount`, `bookmarkCount`, `hyperlinkCount`, `commentCount`,
  `sectionCount`, `readingTimeMinutes`/`readingTimeText`. Word/paragraph/image/
  hyperlink/equation counts walk the main document body and table cells
  (including nested tables); headers, footers, footnotes, and endnotes are
  excluded, matching Word's own default word-count scope.
- **Excel** — `worksheetCount`, `cellCount` (non-empty stored cells),
  `formulaCount`, `tableCount` (worksheet tables), `imageCount`,
  `hyperlinkCount`, `commentCount` (classic + threaded), `mergedRangeCount`.
- **PowerPoint** — `slideCount`, `hiddenSlideCount`, `slideWithNotesCount`,
  `shapeCount` (recursing into groups), `imageCount`, `tableCount`,
  `chartCount`, `hyperlinkCount`, `commentCount`, `wordCount` (slide shape and
  table text; speaker notes are excluded), `readingTimeMinutes`/
  `readingTimeText`.

`readingTimeMinutes` is estimated at 200 words/minute; `readingTimeText` is
the same value formatted as `"N min"` (or `"N sec"` under a minute).

```
$ exyoki stat report.docx
command: stat
status: ok
data:
  family: word
  wordCount: 812
  characterCount: 4820
  paragraphCount: 46
  headingCount: 5
  tableCount: 2
  imageCount: 1
  equationCount: 0
  footnoteCount: 3
  endnoteCount: 0
  bookmarkCount: 1
  hyperlinkCount: 4
  commentCount: 0
  sectionCount: 1
  readingTimeMinutes: 4.06
  readingTimeText: 4 min

$ exyoki stat deck.pptx --format json | python -m json.tool
{
  "...": "...",
  "data": {
    "family": "powerpoint",
    "slideCount": 12,
    "hiddenSlideCount": 0,
    "slideWithNotesCount": 5,
    "shapeCount": 34,
    "imageCount": 6,
    "tableCount": 1,
    "chartCount": 2,
    "hyperlinkCount": 3,
    "commentCount": 0,
    "wordCount": 540,
    "readingTimeMinutes": 2.7,
    "readingTimeText": "3 min"
  }
}
```

### `replace` — find-and-replace across a Word, Excel, or PowerPoint document

```
exyoki replace <package> <needle> <replacement> [--dry-run] [--out-package <path>]
               [--regex] [--ignore-case]
```

Replaces every occurrence in the same scope `search` covers, with one Excel
rule: **only text cells are rewritten**. A match inside a number, date,
boolean, error, or formula cell is never touched — rewriting it would
silently change the cell's type — and is instead counted in a warning
diagnostic. PowerPoint replacements preserve run formatting around the match;
a match spanning several differently-formatted runs keeps the formatting of
the run it starts in.

`--dry-run` only counts matches and never modifies or saves anything; without
it, the change is saved to `--out-package` (or back to the input path in
place).

`--regex` treats `<needle>` as an ECMAScript regular expression; `<replacement>`
may then reference capture groups as `$1`, `$2`, ... (and `$&`, `` $` ``, `$'`,
`$$`), expanded per match via `std::match_results::format`. `--ignore-case`
makes either mode case-insensitive. Like `search`, matching never crosses a
paragraph, cell, or notes-page boundary.

```
$ exyoki replace report.docx "2026" "2027" --dry-run
command: replace
status: ok
data:
  family: word
  replacementCount: 3
  skippedNonTextMatches: 0
  saved: false

$ exyoki replace book.xlsx "Widget" "Gadget" --out-package renamed.xlsx
command: replace
status: ok
data:
  family: excel
  replacementCount: 12
  skippedNonTextMatches: 2
  saved: true
[warning] 2 match(es) in non-text cells (numbers, dates, booleans, formulas) were not replaced

$ exyoki replace report.docx "(Q[1-4]) (\d{4})" "$1 FY$2" --regex --dry-run
command: replace
status: ok
data:
  family: word
  replacementCount: 1
  skippedNonTextMatches: 0
  saved: false
```

### `split` — split a Word, Excel, or PowerPoint document

```
exyoki split <package> <outdir>
             [--by auto|section|page|paragraphs|marker|worksheets|slides]
             [--count N] [--marker TEXT] [--prefix NAME] [--overwrite]
```

The package family is detected from its main OPC part. `--by auto` is the
default and selects section breaks for Word, worksheets for Excel, and slides
for PowerPoint. Family-incompatible choices are rejected instead of being
silently reinterpreted.

For Word, page mode recognizes explicit and last-rendered page breaks and
`pageBreakBefore`; paragraph mode requires `--count`; marker mode requires
`--marker` and starts a new document at each matching paragraph. This is
structural pagination, not page-layout calculation.

Excel and PowerPoint default to one worksheet or slide per output. `--count`
groups that many consecutive items into each package. These formats use a
clone-and-prune implementation, preserving package-wide data and all
relationship graphs reachable from retained sheets or slides. Output files
retain the source extension and use numbered names such as `part_01.xlsx`.

`--prefix` is concatenated into every output name, so it must be one plain
file-name fragment: a separator, `..`, or a Windows device name is refused
rather than deciding where the files land. `<outdir>` is what chooses the
directory.

```
exyoki split report.docx chapters --by section
exyoki split report.docx batches --by paragraphs --count 100 --prefix batch
exyoki split workbook.xlsx sheets
exyoki split presentation.pptx decks --by slides --count 10
```

### `merge` — merge same-family Office documents

```
exyoki merge <input>... --out-package <merged-package>
             [--no-page-breaks] [--style-conflict rename|keep|replace]
             [--overwrite]
```

All inputs must be Word, all Excel, or all PowerPoint; mixed-family input is
rejected before the destination is saved. Inputs are appended in command-line
order.

- Word imports body content and remaps styles, numbering, bookmarks, notes,
  comments, content controls, images, and hyperlinks. Page breaks are inserted
  by default. `--no-page-breaks` and `--style-conflict` are Word-only options.
- Excel appends worksheets, automatically making duplicate sheet names unique.
  Shared strings are remapped and complete worksheet-owned OPC graphs are
  imported, including drawings, images, tables, comments, and hyperlinks.
  Because cell style indices are workbook-global, styled sheets require
  byte-equivalent style catalogs; incompatible workbooks fail explicitly rather
  than producing cells with incorrect formatting.
- PowerPoint appends slides and imports their complete relationship graphs,
  including layouts, masters, themes, notes, media, charts, and embedded
  packages.

```
exyoki merge cover.docx report.docx appendix.docx --out-package complete.docx
exyoki merge january.xlsx february.xlsx --out-package quarter.xlsx
exyoki merge intro.pptx results.pptx --out-package meeting.pptx
```

### `diff` — compare two packages

```
exyoki diff <left> <right> [--no-normalize] [--parts-only]
```

Compares parts by URI (Added/Removed/ContentTypeChanged/ChangedBinary/
ChangedXml) and relationships by `(container, id)` (Added/Removed/Changed).
By default, XML parts are compared as normalized trees — insignificant
whitespace and attribute order are ignored, and `firstDifferencePath` gives
an approximate element path (e.g. `/w:document/w:body/w:p[3]`) to the first
real difference. `--no-normalize` compares raw XML bytes instead (whitespace
differences then count as changes). `--parts-only` omits relationship
changes from the report. Exit code `4` means the packages differ.

```
$ exyoki diff v1.docx v2.docx
command: diff
status: ok
data:
  identical: false
  partChangeCount: 1
  partChanges:
    - uri=/word/document.xml, kind=changedXml, firstDifferencePath=/w:document/w:body/w:p[3]
  relationshipChangeCount: 0
```

### `compare` — semantic Word comparison with tracked revisions

```
exyoki compare <original> <revised> --out-package <path> [--author NAME]
```

Where `diff` compares two packages part-by-part at the XML level, `compare`
compares two **Word documents by content** and writes a third document in
which the differences appear as tracked revisions — deletions for paragraphs
missing from the revised document, insertions for paragraphs present only
there — ready to be reviewed, accepted, or rejected in Word. Neither input is
modified.

The comparison is paragraph-level plain text (a conservative compatibility
stage via `WordDocumentEditor::CompareWith`), not Word's full diff engine: it
does not detect moves, formatting-only changes, or intra-paragraph edits.
Exit code `4` mirrors `diff`: the documents differ (at least one revision was
created); `0` means no revisions were needed.

```
$ exyoki compare v1.docx v2.docx --out-package changes.docx --author "Review Bot"; echo $?
command: compare
status: ok
data:
  revisionsCreated: 3
  identical: false
  outputFile: changes.docx
4
```

### `redact` — scrub review and identity artifacts before publication

```
exyoki redact <package> [--out-package <path>]
              [--keep-comments] [--keep-revisions] [--keep-hidden-text]
              [--keep-metadata]
```

Removes the things that most often leak from a published document. By default
all four categories are scrubbed; each `--keep-*` flag opts one out:

- **Comments** — Word comment parts (`comments.xml`, the modern
  `commentsExtended`/`commentsIds`/`commentsExtensible` companions, and
  `people.xml`) plus every in-text comment marker; Excel classic and threaded
  comments; PowerPoint modern comments and their author registry.
- **Tracked revisions** (Word) — every revision in the main body is
  **accepted**, matching the usual "final version" publish flow. Nothing of
  the revision metadata (author, date) remains.
- **Hidden text** (Word) — runs formatted with `w:vanish` are deleted
  outright, from the body, headers, footers, footnotes, and endnotes.
- **Personal metadata** — the `Creator`, `LastModifiedBy`, and `Company`
  properties are cleared and the custom-properties part
  (`docProps/custom.xml`), a frequent home of workflow identity data, is
  removed.

The scrub is structural: it does not search body text for sensitive words.
Combine it with `replace` for content-level redaction. Without
`--out-package` the file is redacted in place.

```
$ exyoki redact report.docx --out-package public.docx
command: redact
status: ok
data:
  family: word
  commentsRemoved: 4
  revisionsResolved: 7
  hiddenRunsRemoved: 1
  metadataFieldsCleared: 3
  saved: true
[info] Custom properties part removed (/docProps/custom.xml)
```

Verify the result the same way a recipient would look at it:
`exyoki extract-text public.docx`, `exyoki props get public.docx`, and
`exyoki stat public.docx` (`commentCount: 0`).

### `fill` — JSON-driven mail merge into a Word template

```
exyoki fill <package> <data> [--out-package <path>]
```

Fills a Word template from a JSON data file. `<data>` must contain a JSON
object:

- String, number, boolean, and null members become **scalar values** merged
  into matching `MERGEFIELD Name` fields (the cached result is replaced; the
  field instruction is preserved) and into same-paragraph bookmarks of the
  same name. Null merges as empty text.
- A member whose value is an **array of objects** drives a repeating region
  delimited by `MERGEFIELD TableStart:Name` and `MERGEFIELD TableEnd:Name`
  markers: the content between the markers is copied once per array element
  and merged with that element's members.
- Members of any other shape are skipped with a warning — nothing is dropped
  silently.

The merge is strictly literal: no expression language is evaluated, and the
data can never make the document fetch anything external. Without
`--out-package`, the template file itself is overwritten.

```
$ cat data.json
{
  "Customer": "Acme Corp.",
  "Total": 1249.50,
  "Orders": [
    { "Item": "Widget", "Qty": 12 },
    { "Item": "Gadget", "Qty": 3 }
  ]
}

$ exyoki fill invoice-template.docx data.json --out-package invoice.docx
command: fill
status: ok
data:
  fieldsMerged: 2
  bookmarksMerged: 0
  regionsMerged: 1
  regionRowsInserted: 2
  saved: true
```

### `recalc` — recalculate workbook formulas

```
exyoki recalc <package> [--sheet NAME] [--out-package <path>] [--dry-run]
```

Recomputes every formula cell of an `.xlsx` workbook with the library's
built-in formula engine and rewrites the cached results, so the workbook
shows up-to-date values the moment it opens — the missing step after editing
cell values with `replace`, `convert`, or the C++ API. `--sheet` restricts
recalculation to one worksheet (cross-sheet precedents are still read);
`--dry-run` evaluates and reports without saving.

Formulas are evaluated in dependency order with Excel-compatible semantics.
Worksheet errors such as `#DIV/0!` are ordinary results, not failures.
Circular references are reported as diagnostics and the affected cells keep
their previous cached values. Unsupported constructs degrade to documented
error values (`#NAME?` for unknown names, `#REF!` for external workbook
references) rather than being silently miscalculated.

```
$ exyoki recalc budget.xlsx
command: recalc
status: ok
data:
  recalculatedCellCount: 184
  circularReferenceCount: 0
  circularReferences:
  saved: true
```

### `schema` — the JSON Schema of the document model

```
exyoki schema
exyoki schema --check <envelope.json>
```

Without arguments, prints the JSON Schema (draft 07) of the
`exyokioffice-document` envelope that `convert` produces and consumes. Like
`completions`, the schema is the payload itself rather than a report
envelope, so `--format` does not apply while `--output` still selects the
destination. The same document is published in the repository as
[`docs/schemas/exyokioffice-document-v1.schema.json`](../schemas/exyokioffice-document-v1.schema.json)
— a test fails if the two ever diverge.

With `--check`, the given file is validated against that schema and the
result comes back as a normal report:

```
exyoki schema --check draft.json --format json
```

```json
{
  "tool": "exyoki",
  "command": "schema",
  "status": "error",
  "data": {
    "input": "draft.json",
    "schema": "exyokioffice-document-v1.schema.json",
    "valid": false,
    "violationCount": 1
  },
  "diagnostics": [
    {
      "severity": "error",
      "message": "validation failed for additional property 'weight': instance invalid as per false-schema",
      "context": "/document/body/0"
    }
  ]
}
```

Each diagnostic's `context` is the JSON pointer of the offending value, so a
generator can jump straight to what it got wrong. Exit code is `0` when the
document conforms and `3` when it does not — the same code `validate` uses
for a failed package.

This is the self-check an agent needs before feeding hand-written JSON to
`convert`: the schema states what the importer accepts, and cross-references
it deliberately does not cover (a `commentRef` naming a missing comment, an
image naming a missing media entry) are reported by `convert` itself as
diagnostics. See [conversion-formats.md](conversion-formats.md#json-schema)
for the schema's relationship to the prose specification.

### `completions` — shell tab-completion scripts

```
exyoki completions bash|zsh|powershell
```

Prints a completion script for the requested shell to stdout (or `--output`).
Like the `commands` catalog, the script is generated from the live parser, so
it always matches the commands and options the binary actually accepts —
regenerate it after upgrading `exyoki`. It completes command names first,
then the chosen command's options plus the global ones, and falls back to
filename completion.

```bash
# bash — either of:
exyoki completions bash > /etc/bash_completion.d/exyoki
source <(exyoki completions bash)

# zsh — place on $fpath as _exyoki:
exyoki completions zsh > ~/.zsh/completions/_exyoki

# PowerShell — add to $PROFILE:
exyoki completions powershell | Out-String | Invoke-Expression
```

## Using the Tools library from C++

Every command above is a thin adapter over `ExyokiOffice::Tools`, which is
part of the regular `ExyokiOffice` shared library — no separate linking is
needed. CLI11 is used only by the executables — `tools/exyoki/` and the
[MCP servers](mcp-servers.md) — never by the library; nlohmann/json,
nlohmann/json-schema-validator and pugixml are private implementation
dependencies of the library (they never appear in public headers). This means
the same functionality is directly callable from any C++ code:

```cpp
#include "ExyokiOffice/OpenXmlPackage.hpp"
#include "ExyokiOffice/Tools/PackageInspector.hpp"
#include "ExyokiOffice/Tools/ValidationRunner.hpp"
#include "ExyokiOffice/Tools/WordTextTools.hpp"
#include "ExyokiOffice/Tools/DocumentTools.hpp"

using namespace ExyokiOffice;

OpenXmlPackage package;
if (package.LoadFromFile("report.docx"))
{
    const auto info = Tools::GetInfo(package);
    // info.Family, info.Properties, info.PartCount, ...

    const auto validation = Tools::Run(package);
    // validation.ErrorCount, validation.ValidationIssues, ...
}

const auto matches = Tools::Search("report.docx", "Quarterly");
// matches.Matches[i].Scope / .Label / .Context
```

Headers live under `include/ExyokiOffice/Tools/`:

| Header | Provides |
| --- | --- |
| `PackageModel.hpp` | `DocumentFamily`, `PartRecord`, `RelationshipRecord`, `CoreProperties`, `PackageInfo`, `ToolDiagnostic`, `ToString(...)` helpers, `ParseFileFormatVersion`, `ExpandInputPaths` (wildcard expansion behind batch `validate`). |
| `PackageInspector.hpp` | `CollectAllParts`, `ListParts`, `ListRelationships`, `GetInfo`, `ReadCoreProperties`, `WriteCoreProperty`, `DescribeUnknownFamily`. Core properties are addressed by namespace URI, so a `docProps/core.xml` binding them to prefixes other than `cp:`/`dc:`/`dcterms:` reads and writes correctly. |
| `PackageLimits.hpp` | The `Tools` safety policy: `DefaultPackageLimits`, `ApplyDefaultPackageLimits`, `UntrustedOpenSettings`, and `OwnOutputOpenSettings`. File-oriented Tools entry points use `Recommended()` unless the application explicitly configured a process-wide policy. |
| `ValidationRunner.hpp` | `Run(path\|package, options)` → `ValidationReport`. |
| `PackageArchiver.hpp` | `Unpack`, `Pack`. |
| `FlatOpcConverter.hpp` | `ConvertToFlatOpc`, `ConvertFromFlatOpc`. |
| `MediaExporter.hpp` | `ExportMedia`. |
| `ResourceDeduplicator.hpp` | `DeduplicateSharedResources` over an open package or file, with dry-run and media/font/all-binary selection. |
| `SignatureInspector.hpp` / `ExternalResourceInspector.hpp` | Non-mutating signature-integrity and outward-relationship reports behind `signatures` and `external`; inspecting external references never resolves them. |
| `OutputNaming.hpp` | `IsPlainOutputName`, `MakePlainOutputName`, `IsInsideDirectory` — the rule every tool applies to a file name it did not choose itself, whether that name came from a part URI or from a command-line prefix. |
| `WordTextTools.hpp` | `Search`, `ExtractText`, `Replace` (Word-only), `TextScope`. `Search`/`Replace` take optional `useRegex`/`ignoreCase` flags for ECMAScript regex and case-insensitive matching. |
| `DocumentTextTools.hpp` | Family-aware `SearchDocumentText` and `ReplaceDocumentText` behind the `search`/`replace` commands: Word delegates to `WordTextTools`, Excel walks worksheet cells, PowerPoint walks slide shapes and notes. |
| `DocumentStats.hpp` | `Stat` over a path or an already open family editor: family-specific counts plus shared image, table, hyperlink, and comment totals. |
| `DocumentRedactor.hpp` | `RedactDocument` + `RedactOptions` — comments, tracked revisions, hidden text, and identity metadata removal behind `redact`. |
| `WordAutomationTools.hpp` | `FillWordTemplate` (JSON mail merge behind `fill`) and `CompareWordDocuments` (tracked-revision comparison behind `compare`). |
| `WordDocumentTools.hpp` | Word-specific split and merge options/results used by the family-aware `DocumentTools` dispatcher. |
| `SpreadsheetTools.hpp` | `RecalculateWorkbook` — the `recalc` command over `Excel::FormulaEngine`. |
| `XmlQueryTool.hpp` | `Query(path, xpath, options)` — namespace-precise dynamic XPath over any XML part of any package. Built on `ExyokiOffice::Xml` (`include/ExyokiOffice/Xml/XmlQuery.hpp`: `SelectNodes`, `InnerText`, the fluent `XmlQuery`, and `XmlHelpers`). |
| `DocumentTools.hpp` | Family-aware `SplitDocument` and `MergeDocuments` for Word, Excel, and PowerPoint. |
| `TextExtractor.hpp` | `Extract` — multi-family dispatch (Word/Excel/PowerPoint). |
| `PackageDiff.hpp` | `Compare`. |
| `Report.hpp` | `ReportNode`/`ReportDocument` generic tree plus `RenderPlain`/`RenderMarkdown`/`RenderJson`/`RenderXml` (JSON rendering is implemented in `sources/Tools/Report.cpp` using the library's private nlohmann/json dependency). |
| `DocumentModel.hpp` | The semantic document model (IR) shared by every `convert` format: `DocumentModel` with `WordDocumentModel`/`ExcelWorkbookModel`/`PowerPointDeckModel` payloads and `MediaReference`. |
| `DocumentModelIO.hpp` | `ReadWordModel`/`ReadExcelModel`/`ReadPowerPointModel`, `WriteWordModel`/`WriteExcelModel`/`WritePowerPointModel`, and the format serializers/parsers (`SerializeModelJson`/`ParseModelJson`, `SerializeModelXml`/`ParseModelXml`, `SerializeModelMarkdown`/`ParseModelMarkdown`, `SerializeModelText`/`ParseModelText`). |
| `DocumentModelSchema.hpp` | `GetDocumentModelJsonSchema` (the draft-07 schema of the JSON envelope), `GetDocumentModelJsonSchemaFileName`, and `ValidateModelJson` — the `schema` command, and the self-check any importer can run before calling `ParseModelJson`. |
| `MarkdownDocument.hpp` | Office-agnostic Markdown AST (`MarkdownBlock`/`MarkdownInline`), `ParseMarkdown`, `RenderMarkdown`, `EscapeMarkdownText`. |
| `DocumentConverter.hpp` | `ConvertDocument` — the `convert` command's orchestrator (format inference, media handling). |
| `DocumentEditors.hpp` | Lightweight forward declarations for Tools headers whose overloads take an already open Word, Excel, or PowerPoint editor. Callers still include the corresponding editor header before invoking those overloads. |

Package-centric operations accept a plain `ExyokiOffice::OpenXmlPackage` or a
filesystem path; `OpenXmlPackage::LoadFromFile`/`LoadFromMemory` builds the
correct typed part graph without assuming a family up front. Family-aware
operations such as model reading, statistics, text extraction and replacement,
redaction, and XML queries additionally provide overloads taking an already
open `WordDocumentEditor`, `ExcelDocumentEditor`, or
`PowerPointDocumentEditor`. Those overloads see unsaved in-memory edits and
avoid family detection and a second open — use them whenever the application
already owns an editor.
