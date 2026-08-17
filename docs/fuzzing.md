# Fuzzing

Coverage-guided fuzzing of the parsing surfaces, driven by libFuzzer
(SEC-006). Everything here runs locally through `WinFuzz.ps1`; no CI workflow
runs the fuzzers, in line with the repository rule that nothing is triggered
automatically.

## What fuzzing does here

libFuzzer runs in-process: the executable starts once and calls
`LLVMFuzzerTestOneInput(data, size)` millions of times inside that one process.
The compiler has instrumented the code with edge counters and comparison hooks,
so after each input libFuzzer can tell whether new code was reached. Inputs that
reach new code are kept and mutated further; the rest are discarded. Over a run
the fuzzer therefore teaches itself the input format.

It is not looking for a wrong answer. It is looking for a crash, a hang, a
sanitizer report, or a violated invariant. Every fuzz build has
AddressSanitizer enabled, which is what turns a silent out-of-bounds read into
a report.

## Why the main entry point is Flat OPC and not a `.docx`

A `.docx` is a ZIP archive, and miniz verifies a CRC32 for every entry it
extracts (`mz_zip_reader_extract_to_heap` in `sources/zip/miniz.h`). Mutating a
single byte of compressed data therefore fails the CRC check, `zip_entry_read`
returns an error, and the part is never parsed. A naive "random bytes into
`LoadFromMemory`" target spends its entire budget on archive framing and never
reaches the OPC, relationship or DOM layers - and most of what it does reach is
vendored miniz rather than this library.

Flat OPC removes the obstacle completely. It expresses an entire package as one
XML file (`pkg:package` / `pkg:part`, with `pkg:xmlData` or base64
`pkg:binaryData`), so:

- the input is plain text, which mutates well and has no checksums or length
  fields to invalidate;
- `Tools::ConvertFromFlatOpc` builds a valid archive with correct CRCs itself;
- the resulting bytes go straight into `LoadFromMemory`, so every input reaches
  the full loader.

The library already ships the structure-aware bridge that fuzzing a
checksum-protected format normally requires, and in a documented Microsoft
format rather than an ad-hoc one.

That leaves the `packageload` target, which does feed raw bytes to
`LoadFromMemory` and would be stopped by the same CRC. A fuzz build therefore
compiles the library with `MINIZ_DISABLE_ZIP_READER_CRC32_CHECKS`, set in the
top-level `CMakeLists.txt` under `EXYOKIOFFICE_BUILD_FUZZERS` and nowhere else.
The define does not weaken what the fuzzer proves: the bytes that now reach the
loader are bytes a well-formed archive could have carried, and an archive is
free to declare a correct CRC over anything at all. It only removes the step
that would have rejected them before the code under test ran. A shipped build
keeps the check, which is how it tells damage from data.

## Targets

| Target | Entry point | Notes |
|---|---|---|
| `flatopc` | `ConvertFromFlatOpc` then `LoadFromMemory` | The main one. Covers pugixml, the hand-written base64 decoder, part naming, the zip writer, content types, relationships and DOM construction. |
| `packageload` | `LoadFromMemory` on raw bytes | Small and secondary. Aimed at the wrapper code around miniz - `ValidateZipMetadata`, `CheckCurrentEntryLimits`, entry names, truncated archives. Reaches past the entry CRC because a fuzz build disables it. |
| `xmlpart` | `SetXmlString`, `XmlQuery::Select` | Raw XML into a part without the OPC wrapper, plus the XPath grammar. |
| `simpletypes` | `OpenXmlSimpleValueConvertor` | First byte selects the type, the rest is the text. |
| `formula` | `ValidateFormula`, `CellAddress`/`CellRange`/`SheetCellRange`, `FormulaReferenceRewriter` | |
| `copyops` | `ClonePartGraph`, `ImportPartGraph`, DOM structural operations | Package shape comes from Flat OPC text, operations from a command tape. |
| `mcprpc` | `McpServer::HandleMessage` with the shared and Word toolsets | The only surface an untrusted MCP client reaches: the line parser, the JSON Schema validation of every tool argument, and the handlers behind it. Built only when `EXYOKIOFFICE_BUILD_MCP` is on, which the fuzz presets now enable. |

## Invariants

A target that only checks "did not crash" wastes most of what the fuzzer finds.
These are checked explicitly:

- **simpletypes** - parsing then formatting then parsing again must be stable.
  All the parsers are `noexcept`, so an escaping exception is reported for free.
- **formula/address** - `ParseA1` and `ToA1` must agree, the A1 and R1C1
  spellings of one address must agree, and rewriting with an identity transform
  (insert zero rows) must return the formula byte for byte.
- **flatopc** - a package that loaded once must survive `SaveToMemory` and
  reload with the same set of part URIs. The longer form also runs
  `ConvertToFlatOpc` and back, which exercises the base64 encoder.
- **xmlpart** - serialization must reach a fixed point. The comparison is
  between the second and third passes, not the first and second, because the
  first parse legitimately normalizes the input.
- **mcprpc** - no exception escapes `HandleMessage`, and every answer is one
  line of valid JSON carrying `jsonrpc: "2.0"`, an `id`, and exactly one of
  `result` and `error`. The transport frames one message per line, so an
  embedded newline or a half-written object desynchronizes the client's parser
  for the rest of the connection - a failure no schema catches and no client
  recovers from. The workspace root deliberately does not exist, so an input
  cannot leave state behind for the next one: a crash artifact has to reproduce
  on its own.

Invariants use `EXYOKIOFFICE_FUZZ_CHECK`, not `assert`: fuzz builds are
RelWithDebInfo, where `assert` compiles to nothing.

## Running

```powershell
.\WinFuzz.ps1 -Target flatopc -Seconds 300      # clang-cl, the default
.\WinFuzz.ps1 -Target all -Seconds 60
.\WinFuzz.ps1 -Target all -Toolchain MSVC -Seconds 60
.\WinFuzz.ps1 -Target formula -Minimize         # fold new coverage into the seeds
.\WinFuzz.ps1 -Replay                           # run the committed inputs once
```

The script finds Visual Studio with `vswhere`, enters its developer
environment, and configures the `windows-ninja-clang-fuzz` or
`windows-ninja-fuzz` preset. The developer environment is needed to *run* the
binaries too, because the sanitizer runtime lives in the toolchain directories;
for clang-cl the script also puts the compiler-rt directory on `PATH`.

Both toolchains are supported and verified. clang-cl is the default and the
better supported path. Linux is not covered by this task.

## The two corpora

libFuzzer writes every input that reaches new coverage into the **first**
directory on its command line, naming each after the SHA-1 of its contents. A
one-minute run adds thousands of files.

- **Working corpus** - `build/<preset>/fuzz-corpus/<target>`. Grows freely, not
  tracked, safe to delete.
- **Seed corpus** - `tests/fuzz/corpus/<target>`. Small, curated, committed,
  passed to libFuzzer read-only. Flat OPC seeds are readable XML files that can
  be edited by hand.

`-Minimize` runs `-merge=1` to fold the working corpus down into the seed
corpus, keeping only inputs that add coverage. Review the result with
`git status` before committing.

Seed layouts, where a target needs more than a payload:

| Target | Layout |
|---|---|
| `flatopc` | the whole file is the Flat OPC document |
| `packageload` | the whole file is a package |
| `simpletypes` | 1 selector byte, then the text |
| `formula` | 1 selector byte, 1 sheet-name length byte, the sheet name, then the text |
| `xmlpart` | 1 XPath length byte, the XPath, then the XML |
| `copyops` | 1 operation-count byte, 48 command bytes, then the Flat OPC document |
| `mcprpc` | 1 flag byte (bit 0 replays a conformant handshake first), then newline-separated JSON-RPC messages |

## Crashes become regression tests automatically

The fuzz entry points live in `ExyokiOfficeFuzzTargets`, an ordinary static
library. It is linked into the libFuzzer executables *and* into
`ExyokiOfficePackageTests`, where `FuzzCorpusReplayTests` replays every file
under `tests/fuzz/corpus/**` and `tests/fuzz/crashes/**`.

So reproducing a finding needs no fuzz build at all:

1. libFuzzer writes the artifact to `tests/fuzz/crashes/<target>/`.
2. Commit it, with a note in that directory's `README.md` saying what it showed.
3. It runs in every `ctest` from then on.

Never delete a crash artifact because it passes now.

## Findings from the fuzz campaigns

Four defects have surfaced, which is the clearest argument for keeping this
running:

- `DateTimeValueTraits::Format` converted the whole time point to nanoseconds.
  That count is a 64-bit integer spanning only about 1678 to 2262, so every
  xsd:dateTime outside that window overflowed and was written back as a
  different instant. `0001-01-01T00:00:00Z`, the usual null-date sentinel, was
  enough to trigger it.
- The same date later exposed the inverse overflow in
  `DateTimeValueTraits::TryParse`: converting the whole-second count to a
  nanosecond-based `system_clock::duration` overflowed on libstdc++ and libc++
  even though MSVC's 100-nanosecond clock happened not to show it. Parsing now
  range-checks the target clock and rejects dates that representation cannot
  hold instead of returning a wrapped instant.
- `OpenXmlPackagePart::SetXmlString` parsed with `load_string(xml.c_str())`,
  which ends the document at the first embedded NUL. A `&#0;` reference was
  enough to truncate a part silently.
- The generator's `TryExtractConditionalForbiddenValues` and
  `TryExtractParenthesizedTrigger` matched a regex against a temporary
  `std::string`. `std::smatch` keeps iterators into the subject, so every
  capture read afterwards was a use-after-scope. AddressSanitizer caught this
  one during the build itself.

## Known gaps

- The fuzz presets build the library **statically**. libFuzzer keeps its
  coverage counters in one instance per process; with the library in a DLL the
  instrumented code and the runtime sit on opposite sides of a module boundary.
- Package limits are pinned by `FuzzHarness::SafeLimits()`. The defaults in
  `OpenXmlPackageLimits` are all zero, meaning unlimited, and a fuzzer left
  unlimited finds nothing but zip bombs and out-of-memory reports.
- `mcprpc` runs the whole dispatch, so it executes roughly five hundred inputs
  per second where a parser-only target manages tens of thousands. That is the
  price of reaching the handlers; a first 90-second campaign still grew coverage
  from 2 178 to 5 612 edges without a finding.
- On this machine a fuzz build needs the ASan runtime on `PATH`
  (`VC\Tools\Llvm\x64\lib\clang\<version>\lib\windows`); without it the
  instrumented code generator fails at start-up with `0xc0000139` before
  anything is compiled.
