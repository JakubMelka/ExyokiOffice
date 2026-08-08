# Introduction

ExyokiOffice is a C++20 shared library for creating, opening, editing, and
saving Microsoft Office Open XML packages — Word documents (`.docx`), Excel
workbooks (`.xlsx`), and PowerPoint presentations (`.pptx`), together with
their macro-enabled and template variants. The library reads and writes the
ZIP/XML package directly; Microsoft Office is neither a build nor a runtime
dependency, so the same code runs on a server, in a CI pipeline, or on a
desktop machine that has never had Office installed.

This chapter explains what the library is made of, the conventions every part
of the API follows, and how the rest of this manual is organized. If you would
rather see working code first, jump straight to one of the quickstarts —
[Word](Word.md), [Excel](Excel.md), or [PowerPoint](PowerPoint.md) — and come
back here when you want the underlying model.

## What the library does

- **Authors documents from scratch.** A few lines of code produce a valid
  `.docx`, `.xlsx`, or `.pptx` that Word, Excel, PowerPoint, LibreOffice, and
  other OOXML consumers open without repair prompts.
- **Edits existing documents.** Packages are loaded completely into memory,
  edited through typed APIs, and saved explicitly. Content the API does not
  model is preserved untouched, so a document round-trips without losing
  features the library never looked at.
- **Inspects and transforms packages.** The `ExyokiOffice::Tools` module —
  and `exyoki`, its command-line front end — validates, diffs, splits, merges,
  queries, and converts packages, including conversion to and from Markdown,
  JSON, XML, plain text, and CSV for workbooks.
- **Hands documents to AI agents.** Three Model Context Protocol servers
  expose the same editing APIs as typed, schema-published tools inside a
  sandboxed workspace, so an agent authors and edits documents without
  generating code. See [MCP servers](tools/mcp-servers.md).

## What the library deliberately does not do

Knowing the non-goals up front saves surprises later:

- **No layout or rendering.** The library writes markup; it does not compute
  page breaks, line breaks, or pixel positions, and it cannot export PDF or
  images.
- **No field evaluation in Word.** Fields such as `PAGE` or `TOC` are written
  with a placeholder value and marked dirty; Word computes the real value when
  the document is opened. See [Fields and tables of contents](word/fields.md).
- **No implicit formula calculation in Excel.** Formulas are stored as text.
  The built-in `FormulaEngine` recalculates cached results only when asked;
  see [Formulas](excel/formulas.md).
- **No macro execution.** VBA projects are carried as opaque binary payloads
  that can be detected, extracted, replaced, or removed — never parsed or run.
- **No network or file-system access behind your back.** External targets
  (linked images, attached templates, hyperlinks) are never resolved unless
  the application installs an explicit resolver and policy; see
  [External resources](ExternalResources.md).
- **No cryptography of its own.** Digital signatures are created and verified
  through an application-supplied provider interface; see
  [Digital signatures](Signatures.md).

## Architecture: four layers

The library is organized as four layers, and every chapter of this manual
tells you which layer it operates on. The first three are the C++ API you
link against, listed from the highest to the lowest; the fourth is what the
repository builds on top of them.

1. **High-level editing APIs** — hand-written, task-oriented classes for
   authoring and editing:
   `ExyokiOffice::Word::WordDocumentEditor`,
   `ExyokiOffice::Excel::ExcelDocumentEditor`, and
   `ExyokiOffice::PowerPoint::PowerPointDocumentEditor`.
   These are the recommended entry points and the subject of most of this
   manual.
2. **Typed OpenXML DOM** — generated element classes under
   `ExyokiOffice::DocumentFormat::OpenXml::…` covering WordprocessingML,
   SpreadsheetML, PresentationML, and DrawingML. Every element and attribute
   of the ECMA-376 schemas is available as a C++ type. Insertion through the
   typed DOM is schema-aware: `AppendChild<T>()` places a child at the
   position the content model requires, regardless of call order.
3. **Packaging layer** — `ExyokiOffice::OpenXmlPackage`,
   `ExyokiOffice::OpenXmlPackagePart`, and the classes in
   `ExyokiOffice::Packaging` implement the Open Packaging Conventions
   (OPC): ZIP entries, content types, relationships, and the per-format
   document lifecycle.
4. **Tooling and front ends** — what sits on top of the three layers and does
   not add document behavior of its own. Two modules of the shared library
   form its C++ face:

   - **`ExyokiOffice::Tools`** — package inspection, validation, archiving,
     Flat OPC conversion, media export, text extraction, splitting, merging,
     diffing, and document conversion.
   - **`ExyokiOffice::Xml`** — a dynamic, namespace-precise XPath 1.0 query
     layer over any XML part, complementing the strongly typed DOM when you
     need to reach an element by name rather than by C++ type.

   Two executable front ends make the same capabilities reachable without
   writing C++ at all:

   - **`exyoki`** — a command-line utility for a shell, a script, or a CI
     pipeline: it inspects, validates, converts, queries, redacts, merges and
     edits packages, and `exyoki commands --format json` describes its whole
     interface machine-readably. It is a thin adapter over `Tools`; see
     [exyoki](tools/exyoki.md).
   - **`exyoki-mcp-word`, `exyoki-mcp-excel`, `exyoki-mcp-power-point`** —
     three Model Context Protocol servers that expose the editing APIs to an
     AI agent as roughly fifty typed tools each, over JSON-RPC on stdio.
     Every tool publishes a JSON Schema; there is no code-execution escape
     hatch, no raw XML writing, and no file access outside the configured
     workspace. See [MCP servers](tools/mcp-servers.md).

   If a front end cannot do something, the fix belongs in the layer below it,
   not in the front end.

### Documents and editors

Each format follows the same two-class split:

| Format | Package class (lifecycle) | Editor class (content) |
| --- | --- | --- |
| Word | `WordDocument` | `WordDocumentEditor` |
| Excel | `ExcelDocument` | `ExcelDocumentEditor` |
| PowerPoint | `PowerPointDocument` | `PowerPointDocumentEditor` |

The `*Document` class owns the package: creation, opening, part
initialization, document type (`.docx` versus `.dotx` versus `.docm`, and the
Excel and PowerPoint equivalents), and save-time properties. The
`*DocumentEditor` class owns content authoring and holds a
`std::shared_ptr` to its document, which `GetDocument()` exposes, so the
lower layers always stay reachable. Every wrapper object in the editor APIs
offers a `GetLowLevelApi()` (and, in PowerPoint, `GetElement()`/`GetPart()`)
escape hatch down to the typed DOM and the packaging layer — you never have
to choose a layer up front, and you can drop down for exactly one element
when the high-level API does not model it.

### The package model in two paragraphs

An Office Open XML file is a ZIP archive laid out according to OPC. Each ZIP
entry is a *part* with a declared *content type*; parts point at one another
through *relationships* stored in `_rels/*.rels` entries; and one root
relationship designates the main document part, which is how the document
family (Word, Excel, PowerPoint) is detected. Word's main part is
`/word/document.xml`, Excel's is `/xl/workbook.xml`, PowerPoint's is
`/ppt/presentation.xml`; images, themes, styles, and everything else hang off
these through relationships.

You rarely need to think about this while using the editors — they maintain
parts, content types, and relationships for you. It becomes relevant when
you inspect packages with `exyoki parts` and `exyoki relationships`, when a
feature requires a new part (the editors create it), and when you read the
[External resources](ExternalResources.md) and
[Digital signatures](Signatures.md) chapters, which operate on the package
level.

Parts belong to their package. `Parts()`, `GetPartByUri()` and the typed
`Get*Part()` accessors hand out a `std::shared_ptr`, but that is a borrowed
handle rather than co-ownership of the document. The relationship graph is
not a tree — a slide layout points back at its slide master, a notes slide
back at the slide it annotates — and every edge owns its target, so a package
that is cleared, reloaded, or destroyed cuts the edges between its parts
instead of waiting for reference counts that would never reach zero. A part
you still hold afterwards stays valid but empty: no children, no
relationships. Keep the document alive for as long as you work with its
parts, which is what the editors do for you by holding their document through
`GetDocument()`.

## Conventions used across the API

These rules hold everywhere; the format chapters do not repeat them.

**Everything is in memory; saving is explicit.** Opening a package loads it
completely; edits mutate the in-memory model; nothing touches the disk until
`SaveToFile` (atomic by default — the file is replaced only after the new
package has been written successfully) or `SaveToMemory`.

**Factories return `nullptr` on failure.** `CreateNew`, `Open`,
`CreateFromTemplate`, and the various `Add…` methods return a null smart
pointer when the source cannot be read or the operation is invalid. Always
check the result. `Open` overloads accept an `OpenSettings` argument
controlling safety limits (maximum part sizes) and validation behavior.

### Opening untrusted packages safely

The core package and editor APIs start without ZIP/XML ceilings unless the
application installs a process-wide policy. That preserves compatibility for
trusted packages an application produced itself, but it is not a safe default
for uploads, e-mail attachments, or other untrusted input. Apply the recommended
limits explicitly:

```cpp
ExyokiOffice::Packaging::OpenSettings settings;
settings.PackageLimits = ExyokiOffice::OpenXmlPackageLimits::Recommended();

auto editor = ExyokiOffice::Word::WordDocumentEditor::Open("upload.docx", settings);
if (!editor)
{
    // Malformed input and packages exceeding a limit are both refused.
}
```

`Recommended()` currently bounds the ZIP entry count, compressed and
uncompressed totals, individual part size, relationships per relationship
part, compression ratio, XML nesting, XML node and attribute counts, and XML
text. A limit violation fails the open operation; content is never silently
truncated. The values are intentionally generous for ordinary Office files:
10,000 entries, 256 MiB compressed, 2 GiB uncompressed, 512 MiB per part,
65,536 relationships per part, a 200:1 compression ratio, XML depth 256,
50 million XML nodes and attributes per part, and 512 Mi characters of XML
text per part. Applications that know their workload should tighten them
further.

An application-wide policy can be installed once, before constructing open
settings or opening documents:

```cpp
ExyokiOffice::OpenXmlPackage::SetDefaultPackageLimits(
    ExyokiOffice::OpenXmlPackageLimits::Recommended());
```

`OpenSettings` captures that default when it is constructed; assigning its
`PackageLimits` member still overrides the policy for one open. Use
`OpenXmlPackageLimits::Unlimited()` only for deliberately trusted input so a
reviewer can distinguish that decision from a forgotten limit. File-oriented
entry points in `ExyokiOffice::Tools`, `exyoki`, and the MCP servers use
`Recommended()` by default even when the core policy was never configured.
See [SECURITY.md](../SECURITY.md) for the threat model and reporting policy.

**Structured results for fallible operations.** Operations with meaningful
failure modes (range writes in Excel, protection changes, transitions in
PowerPoint) return result objects carrying a structured error code and a
message — check `Succeeded()` or use the result in a boolean context.
Validating operations reject bad input *before* touching the document, so a
failed call leaves the document unchanged.

**Physical units are explicit.** Lengths are passed as `MeasuringUnits`, a
value plus a `MeasurementUnit` (EMU, twip, point, inch, centimeter,
millimeter), and are converted to the native OOXML unit on serialization.
Helper factories such as `Millimeters(20.0)` and `Points(12.0)` exist where
the API is used most heavily. Angles use `MeasuringAngle` the same way.

**Colors are explicit sRGB.** `ExyokiOffice::Color(r, g, b)` everywhere;
Excel additionally models theme and indexed colors through `ExcelColor`.

**Scalars are spelled through library aliases.** `ExyokiOffice/StandardTypes.hpp`
defines `Int8`…`Int64`, `UInt8`…`UInt64`, `Real` (`double`), `Single` (`float`),
`RealExtended` (`long double`), `Size` (`std::size_t`), `PtrDiff`, and `Byte`, and
the API declares itself in those names. They are plain `using` aliases, so a
`UInt32` parameter accepts a `std::uint32_t` argument and vice versa — calling
code is free to keep using the standard spellings.

**Preservation over interpretation.** Content the API does not model — a
transition effect outside the typed set, a specialized content control, a
chart style part — is reported as "unsupported" where relevant and carried
through edits verbatim rather than being dropped.

**One document graph is one synchronization domain.** Packages, editors, parts,
DOM elements, and wrappers obtained from one document are not thread-safe and
must be confined to one thread or externally serialized. Independent documents
may be processed concurrently. See [Threading and concurrency](Threading.md)
for locking patterns, cancellation, and callback requirements.

## Building, installing, and linking

Configure and build with CMake (3.25 or newer) from the repository root; MSVC,
Clang, and GCC with C++20 are supported. The repository defines CMake presets
(`CMakePresets.json`) for Visual Studio, Ninja/MSVC, Ninja/clang-cl, and Linux
with GCC or Clang, plus sanitizer and libFuzzer configurations. On Windows the
`WinBuild.ps1` script selects the newest Visual Studio installation and drives
the right preset for you:

```powershell
.\WinBuild.ps1 -Test
```

`-Configuration` chooses between `RelWithDebInfo` (the default) and `Debug`. The
sources carry no runtime assertions, so a debug build adds only MSVC's iterator
debugging and an unoptimized debugger view — at roughly six times the running
time.

The script can also install the library, headers, CMake package files, and
`exyoki`. Its default installation prefix is `build/install`:

```powershell
.\WinBuild.ps1 -Configuration RelWithDebInfo -Install
.\WinBuild.ps1 -Configuration RelWithDebInfo -Install -InstallPrefix C:\ExyokiOffice
```

On Linux, invoke the presets directly:

```bash
cmake --preset linux-ninja-debug
cmake --build --preset linux-ninja-debug
ctest --preset linux-ninja-debug
```

Each preset pins a host system, so `cmake --list-presets` shows only those that
apply to the machine you are on. The repository
[README](../README.md#cmake-presets) lists every preset with its toolchain and
binary directory. Presets are a convenience, not a constraint: a plain
`cmake -S . -B <dir>` with your own options, generator, and toolchain file
works exactly as it does for any other CMake project.

The build runs the OpenXML source generator automatically before compiling
the library; the generated translation units make a clean build relatively
slow, which is normal. `exyoki` is built by default
(`EXYOKIOFFICE_BUILD_TOOL=ON`), and the examples and doctest-based unit
tests have their own options (see the repository `README.md` for the full
table).

Install and consume the library from another CMake project:

```powershell
cmake --install build/vs --prefix C:/ExyokiOffice
```

```cmake
find_package(ExyokiOffice 1.0 CONFIG REQUIRED)
target_link_libraries(MyApplication PRIVATE ExyokiOffice::ExyokiOffice)
```

The installed package contains all public and generated headers, the shared
library, and its CMake target files. Versioning follows semantic versioning
with a documented ABI policy; see [Versioning and ABI](ABI.md).

## How this manual is organized

The manual reads front to back, but every chapter also stands on its own:

1. **This introduction** — the model and the conventions.
2. **Quickstarts** — one per format: [Word](Word.md), [Excel](Excel.md),
   [PowerPoint](PowerPoint.md). Each shows a hello world, the lifecycle
   calls, and a short tour, then hands off to its chapters.
3. **Format chapters** — the thorough treatment, one folder per format:
   [word/](word/documents.md), [excel/](excel/workbooks.md), and
   [powerpoint/](powerpoint/presentations.md).
4. **Cross-cutting subsystems** — [Digital signatures](Signatures.md) and
   [External resources](ExternalResources.md), which apply to every format.
5. **Tooling and front ends** — [exyoki](tools/exyoki.md), its
   [conversion formats](tools/conversion-formats.md), and the
   [MCP servers](tools/mcp-servers.md).
6. **Project policies** — [Versioning and ABI](ABI.md),
   [Continuous integration](ci.md), and [Fuzzing](fuzzing.md).

The complete API reference is the Doxygen documentation generated from the
headers under `include/ExyokiOffice`; this manual quotes the headers but
does not replace them.

### Runnable examples

The guides quote from examples that are built and smoke-tested with the rest
of the repository — each one writes a document, opens it again, and fails the
build if anything breaks:

| Example | What it demonstrates |
| --- | --- |
| `examples/ExampleWordEditor` | A complete Word report authored from scratch. |
| `examples/ExampleWordEdit` | Opening an existing document, filling placeholders, inserting through cursors. |
| `examples/ExampleExcelEditor` | A two-sheet workbook with formulas, a chart, a table, and a slicer. |
| `examples/ExamplePowerPointEditor` | A five-slide deck with placeholders, media, animations, and sections. |
| `examples/ExampleWord` | The typed DOM and packaging layers underneath all three editors. |
