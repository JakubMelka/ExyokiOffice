# ExyokiOffice OpenXML Generator

This directory contains the native C++ generator that produces the strongly typed
OpenXML metadata and helpers used by the ExyokiOffice library.

## Building

The root `CMakeLists.txt` wires the generator into the default build. Running

```
cmake --build <build-dir>
```

automatically builds the `openxml_generator` executable and executes it to
produce the generated headers before compiling the main `ExyokiOffice` shared
library.

If you need to invoke the generator manually, run:

```
OpenXmlGenerator --data <path-to-data> --out-include <include-output> --out-source <source-output> \
  --warning-report <report.json>
```

The three input/output parameters are required. `--warning-report` selects the
machine-readable compatibility report location; without it the generator writes
`OpenXmlGeneratorWarnings.json` in its working directory. The report contains
every warning, stable category counts, and context for unsupported metadata.
Additional command-line flags (`--no-schema`,
`--no-namespaces`, `--no-parts`) are available to select which generated
families are emitted.

## Metadata the generator interprets

Two inputs need a word of explanation, because the imported metadata does not
say everything the generated code has to know:

- **Schematron rules** (`data/schematrons.json`) name their context by element
  name and spell every attribute with that element's prefix, even when the
  attribute belongs to another namespace (`a:hlinkClick/@a:id` is really
  `r:id`). The generator resolves each rule's attribute against the attributes
  the element actually declares, following its base classes inside its own
  namespace, and attaches the rule to every element of that name that can carry
  it. A rule no candidate can carry is dropped rather than reported against
  markup that is valid.
- **Part folders** (`data/parts/*.json`) give one folder relative to the parent
  the part hangs off. `data/exyokioffice_part_paths.json`, owned by this
  repository, overrides that with absolute per-family folders for the parts
  Office keeps in one fixed place (`/word/media`, `/ppt/theme`, `/xl/charts`).
  An overlay entry naming a part that no longer exists fails the build.
- **Ambiguous element names** are element QNames several classes declare
  (`w:rPr`, `x:sheetData`). The element factory resolves a name without the
  parent context, so `data/exyokioffice_ambiguous_elements.json`, owned by
  this repository, nominates the class such a name creates; parsing through the
  parent's content model resolves it from the particle tree instead. An entry
  naming an element that is no longer ambiguous, or a class that does not
  declare it, fails the build; an ambiguous name the file does not cover is
  reported as a warning and falls back to the first declared class.
