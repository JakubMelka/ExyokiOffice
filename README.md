# ExyokiOffice

![ExyokiOffice logo](LOGO.png)

**A native C++20 toolkit for creating and editing Word, Excel, and PowerPoint
documents.** Work with `.docx`, `.xlsx`, and `.pptx` through high-level editors
or a typed OpenXML DOM, without Microsoft Office, .NET, Java, or Python at build
time or runtime.

ExyokiOffice is designed for applications that need to generate or modify
Office documents while retaining access to the underlying package. Content
outside the high-level editing model is preserved instead of being silently
dropped.

[Quick start](#quick-start) · [Choose an API](#choose-an-interface) ·
[Compatibility](docs/Compatibility.md) · [Documentation](docs/README.md) ·
[Comparison](docs/comparison.md)

## Why ExyokiOffice

- **One native library for all three Office formats.** Create and edit Word
  documents, Excel workbooks, and PowerPoint presentations from C++20.
- **Start high-level, go low-level when needed.** Friendly editors cover common
  authoring tasks; the generated typed DOM and OPC packaging layer remain
  available for precise OpenXML work.
- **Preserve what you do not edit.** The compatibility matrix distinguishes
  features the library can create, edit, and preserve, with a corresponding
  compatibility test for every row.
- **Use the same capabilities from code, a shell, or an AI agent.** The library
  also powers the `exyoki` command line and three workspace-confined MCP
  servers.
- **Deploy without an office suite.** Packages are written directly as ZIP/XML;
  no Microsoft Office installation or automation process is involved.

### How it compares

| | ExyokiOffice | Open XML SDK | Apache POI | Python Office libraries | C++ spreadsheet libraries |
| --- | --- | --- | --- | --- | --- |
| Language | C++20 | C# / .NET | Java | Python | C++ / C |
| Runtime dependency | none | .NET | JVM | CPython | none |
| Word / Excel / PowerPoint | all three | all three | all three | separate projects | Excel only |
| High-level editing API | yes | typed DOM | yes | yes | yes |
| Typed schema DOM | yes | yes | yes | no | no |
| Schema and semantic validation | yes | yes | no | no | no |
| Command-line front end | `exyoki` | no | no | no | no |
| MCP servers for AI agents | three | no | no | no | no |
| License | MIT | MIT | Apache-2.0 | mostly MIT | permissive |

The detailed comparison explains the tradeoffs against each alternative and
when another project is a better fit: [How ExyokiOffice
compares](docs/comparison.md).

### Know the boundaries

ExyokiOffice edits document structure; it is not a rendering engine. It does
not provide PDF or image rendering, legacy `.doc`/`.xls`/`.ppt`,
ODF/RTF/HTML, encrypted packages, or ISO 29500 Strict. If a workflow ends in a
PDF or preview image, pair ExyokiOffice with a renderer. See the complete
[out-of-scope list](docs/Compatibility.md#out-of-scope).

## Choose an interface

| Goal | Start with | Guide | Runnable example |
| --- | --- | --- | --- |
| Create or edit Word documents | `Word::WordDocumentEditor` | [Word](docs/Word.md) | [ExampleWordEditor](examples/ExampleWordEditor/main.cpp) |
| Modify an existing Word document | `WordDocumentEditor::Open` and body cursors | [Documents](docs/word/documents.md) and [text](docs/word/text.md) | [ExampleWordEdit](examples/ExampleWordEdit/main.cpp) |
| Create or edit Excel workbooks | `Excel::ExcelDocumentEditor` | [Excel](docs/Excel.md) | [ExampleExcelEditor](examples/ExampleExcelEditor/main.cpp) |
| Create or edit PowerPoint presentations | `PowerPoint::PowerPointDocumentEditor` | [PowerPoint](docs/PowerPoint.md) | [ExamplePowerPointEditor](examples/ExamplePowerPointEditor/main.cpp) |
| Work directly with OpenXML elements or package parts | Typed DOM and `ExyokiOffice::Packaging` | [Architecture](docs/introduction.md) | [ExampleWord](examples/ExampleWord/main.cpp) |
| Inspect, validate, convert, diff, or query packages | `ExyokiOffice::Tools`, `ExyokiOffice::Xml`, or `exyoki` | [Command-line guide](docs/tools/exyoki.md) | [`exyoki`](tools/exyoki/main.cpp) |
| Let an AI agent edit documents | `exyoki-mcp-*` | [MCP servers](docs/tools/mcp-servers.md) | [Word MCP server](tools/mcp/word/main.cpp) |

Use a high-level editor whenever it models the task. Drop to the typed DOM or
packaging layer only when lower-level control is required.

## Quick start

### Word

```cpp
#include "ExyokiOffice/Word/WordDocument.hpp"

int main()
{
    using namespace ExyokiOffice::Word;

    auto editor = WordDocumentEditor::CreateNew();
    editor->AddHeading("Hello world");
    editor->AddParagraph("This document was created by ExyokiOffice.");
    editor->SaveToFile("Hello.docx");
}
```

The complete [Word editor example](examples/ExampleWordEditor/main.cpp) covers
styles, lists, tables, images, links, notes, comments, fields, page setup, and
search and replace. [ExampleWordEdit](examples/ExampleWordEdit/main.cpp) shows
how to open an existing document, fill placeholders, insert content, extend a
table, and save a new copy.

### Excel

```cpp
#include "ExyokiOffice/Excel/ExcelDocument.hpp"

int main()
{
    using namespace ExyokiOffice::Excel;

    auto editor = ExcelDocumentEditor::CreateNew();
    auto sheet = editor->FirstWorksheet();
    sheet->SetCellText(1, 1, "Hello world");
    sheet->SetCellText(2, 1, "This workbook was created by ExyokiOffice.");
    editor->SaveToFile("Hello.xlsx");
}
```

The complete [Excel editor example](examples/ExampleExcelEditor/main.cpp)
includes styles, formulas and recalculation, named ranges, charts, tables,
slicers, validation, frozen panes, print setup, links, and comments.

### PowerPoint

```cpp
#include "ExyokiOffice/PowerPoint/PowerPointDocument.hpp"

int main()
{
    using namespace ExyokiOffice::PowerPoint;
    namespace Drawing = ExyokiOffice::DocumentFormat::OpenXml::Drawing;
    namespace Presentation = ExyokiOffice::DocumentFormat::OpenXml::Presentation;

    using ExyokiOffice::MeasurementUnit;
    using ExyokiOffice::MeasuringUnits;
    const auto Inches = [](double value) { return MeasuringUnits(value, MeasurementUnit::Inch); };

    auto editor = PowerPointDocumentEditor::CreateNew();
    editor->SetSlideSize(PresentationSlideSize::Widescreen16x9());

    // Every slide needs a layout, and every layout a master.
    auto master = editor->AddSlideMaster("Default");
    auto layout = editor->AddSlideLayout(master, "Title", Presentation::SlideLayoutValues::Title);

    auto shape = editor->AddSlide()->ShapeTree()->AddShape("Title");
    editor->SetSlideLayout(0, layout);
    shape->SetPresetGeometry(Drawing::ShapeTypeValues::Rectangle);
    shape->SetTransform({.Position = {Inches(0.75), Inches(2.5)},
                         .Size = {Inches(11.833), Inches(1.3)}});

    PresentationTextRun run;
    run.Text = "Hello world";
    run.Language = "en-US";
    run.Bold = true;

    PresentationTextFrame frame;
    frame.Paragraphs = {PresentationTextParagraph{.Runs = {run}}};
    shape->SetTextFrame(frame);

    editor->SaveToFile("Hello.pptx");
}
```

For a complete tour — slide size and properties, a `SlideBuilder` title slide,
formatted multi-level text, shape fills, outlines and effects, a picture, a
chart, a DrawingML table with a merged cell, a transition, animation effects,
speaker notes, comments, sections, and a custom show — see
[examples/ExamplePowerPointEditor/main.cpp](examples/ExamplePowerPointEditor/main.cpp)
and the [PowerPoint quickstart](docs/PowerPoint.md).

Every example saves and reopens its document and is registered with CTest, so
the examples also serve as round-trip tests of the public API.

## Command-line automation

The `exyoki` utility brings the library's inspection, validation, conversion,
editing, text extraction, media export, diffing, redaction, and XML query tools
to a shell or script:

```bash
exyoki validate report.docx
exyoki convert report.docx report.md
exyoki search presentation.pptx "quarterly revenue"
exyoki redact reviewed.docx public.docx
```

It converts Office packages to automation-friendly Markdown, JSON, plain text,
semantic XML, and workbook CSV, and can import supported representations back
into Office formats. `exyoki commands --format json` describes every command,
option, constraint, and exit code for scripts and agents; `exyoki schema`
publishes the JSON Schema of the conversion model. See the complete
[`exyoki` guide](docs/tools/exyoki.md).

## MCP servers for AI agents

`exyoki-mcp-word`, `exyoki-mcp-excel`, and
`exyoki-mcp-power-point` expose the high-level editors as typed tools over
stdio. Each server confines document paths to a configured workspace, validates
tool calls against published JSON schemas, and provides no code-execution
escape hatch.

```jsonc
// .mcp.json
{
  "mcpServers": {
    "word": {
      "command": "C:/ExyokiOffice/bin/exyoki-mcp-word.exe",
      "args": ["--workspace", "C:/Users/me/Documents/agentwork"]
    }
  }
}
```

The [MCP servers guide](docs/tools/mcp-servers.md) covers registration,
security, worked examples, and the complete tool catalogs.

A distroless [container image](docs/tools/docker.md) carries the library,
command line, and all three servers:

```bash
docker pull ghcr.io/jakubmelka/exyokioffice:1.0.0

docker run --rm --network none -v "$PWD:/work" \
  ghcr.io/jakubmelka/exyokioffice:1.0.0 exyoki convert report.docx report.md
```

## Build and install

ExyokiOffice requires a C++20 compiler and CMake 3.25 or newer. The repository's
[CMake presets](CMakePresets.json) provide known-good MSVC, Clang, GCC,
sanitizer, and fuzzing configurations.

On Windows, the build script locates Visual Studio, enters its developer
environment, and drives the presets:

```powershell
.\WinBuild.ps1 -Clean -Test
.\WinBuild.ps1 -Configuration RelWithDebInfo -Install
```

On Linux:

```bash
cmake --preset linux-ninja-release
cmake --build --preset linux-ninja-release
ctest --preset linux-ninja-release
cmake --install build/linux-ninja-release --prefix /opt/ExyokiOffice
```

Consume an installed package through its exported CMake target:

```cmake
find_package(ExyokiOffice 1.0 CONFIG REQUIRED)
target_link_libraries(MyApplication PRIVATE ExyokiOffice::ExyokiOffice)
```

The generated DOM sources are committed. Set
`EXYOKIOFFICE_RUN_GENERATOR=OFF` for packaging and cross-compilation builds
that must not execute a target-architecture generator or modify the source
tree. Other build options are documented alongside their definitions in the
root [CMakeLists.txt](CMakeLists.txt).

## Documentation and project status

- The [documentation index](docs/README.md) links the complete Word, Excel,
  PowerPoint, tooling, security, and packaging guides.
- The [compatibility matrix](docs/Compatibility.md) records supported formats,
  Office versions, and create/edit/preserve depth for every feature area.
- The [threading guide](docs/Threading.md) defines the concurrency contract.
- [CI](docs/ci.md), [fuzzing](docs/fuzzing.md), and compatibility tests describe
  how the implementation and its claims are verified.
- [CONTRIBUTING.md](CONTRIBUTING.md) contains the contributor workflow and
  review checklist.

## Development approach

ExyokiOffice is a human-directed, AI-assisted open-source project. Claude Code
and OpenAI Codex have been used extensively during implementation,
documentation, testing, and review. Design decisions, release responsibility,
and final acceptance remain with the project maintainer.

## License

ExyokiOffice is released under the [MIT License](LICENSE). Vendored dependencies
use compatible permissive licenses; their notices are collected in
[THIRD-PARTY-LICENSES.md](THIRD-PARTY-LICENSES.md).
