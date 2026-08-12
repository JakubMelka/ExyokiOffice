# Code coverage

Coverage answers one question about the test suite: which lines and branches
of the library do the tests actually reach? It is a map of what is untested,
not a score to maximize — a covered line is one that ran, not one whose result
anybody checked.

The repository measures it with LLVM source-based coverage, driven by
[`WinCoverage.ps1`](../WinCoverage.ps1) on Windows. Nothing in CI runs it; it
is a tool you reach for when you want to know where the suite is thin.

## Running it

```powershell
.\WinCoverage.ps1
.\WinCoverage.ps1 -Label unit -LabelExclude slow
.\WinCoverage.ps1 -Label word, spreadsheet
.\WinCoverage.ps1 -AllTests
```

The script discovers Visual Studio with `vswhere` the same way
[`WinBuild.ps1`](../WinBuild.ps1) does, and uses the LLVM tools bundled with it
(`VC\Tools\Llvm\x64\bin`), so no separate Clang installation is needed. It
needs the **C++ Clang tools for Windows** component, which supplies `clang-cl`
together with the matching `llvm-profdata` and `llvm-cov`.

Without arguments it measures what the `unit` layer reaches. `-Label` takes any
of the CTest layer or area labels described in
[the compatibility matrix](Compatibility.md); `-AllTests` measures the whole
suite, which takes as long as a full `ctest` run plus the instrumentation
overhead.

Three artifacts land under `build\coverage`:

| Path | What it is |
| --- | --- |
| `report.txt` | The per-file summary table, also printed to the console. |
| `html\index.html` | The browsable report, with each source annotated by hit count. |
| `coverage.profdata` | The merged profile, should you want to run `llvm-cov` by hand. |

Other useful switches: `-Configuration Debug` for an unoptimized build,
`-ExcludeGenerated` to narrow the report to the hand-written library, `-NoBuild`
to reuse the binaries already in the coverage build tree, and `-Clean` to start
from an empty one.

## MC/DC

```powershell
.\WinCoverage.ps1 -AllTests -Mcdc
```

Modified condition/decision coverage asks a harder question than branch
coverage: for every boolean decision, did the tests show each individual
condition independently changing the outcome? A decision can have both its
branches taken — full branch coverage — while no condition has been proven to
matter on its own, which is why the criterion is what DO-178C level A requires.

`-Mcdc` builds its own tree (`build\ninja-clang-coverage-mcdc`) and writes to
`build\coverage-mcdc`, so an MC/DC run lands beside an ordinary one instead of
over it. It is a separate mode rather than the default because the
instrumentation adds a test-vector bitmap per decision on top of the counters,
which costs binary size, run time and raw profile size that a plain line-coverage
question has no reason to pay.

The report gains MC/DC columns, and `llvm-cov show` marks the decisions in the
HTML with the condition combinations the tests did and did not exercise.

Clang refuses to instrument a decision with more conditions than its limit
allows and warns instead; nothing in this repository is near that limit, so the
warnings-as-errors build is unaffected. Should that ever change,
`-Xclang -fmcdc-max-conditions=N` raises it.

## What the report covers

Everything compiled into the `ExyokiOffice` library, with nothing filtered out.
That includes the generated DOM under `sources/DOM/`, the three generated
translation units under `sources/Packaging/`, and the vendored components — they
are all in the shipped binary, so how much of them the suite reaches is a real
question about the suite.

Two things are not in it. The toolchain's own headers — the MSVC standard
library and the Windows SDK — carry coverage mappings like anything else but
are not part of this repository; `-IncludeToolchainHeaders` puts them back. And
the test executables are not reported on: the question is what the tests reach
in the library, and test code is close to fully covered by construction, so
including it would only flatter the number.

The generated tree dominates by volume — it is roughly four fifths of the
library's lines — so the single total is mostly a statement about generated
accessors. `-ExcludeGenerated` gives the complementary view; reading both is
more informative than reading either. It suppresses the DOM directories
wholesale and the generated packaging units by name, because `sources/Packaging`
holds hand-written code next to generated code (`OpenXmlPackageFactory.hpp` is
hand-written even though its `.cpp` is not).

## How it works, and why it is built this way

The instrumentation is Clang's own: `-fprofile-instr-generate` places a counter
in each region and `-fcoverage-mapping` records the source range that counter
belongs to. Because the mapping is emitted by the front end rather than
reconstructed from debug information, an optimized build still attributes every
hit to the code it came from — which is why the default configuration is
`RelWithDebInfo` and not a Debug build costing roughly six times the running
time. `-Configuration Debug` remains available for sharper counts inside code
the optimizer reshaped heavily.

Three consequences of that design are worth knowing about.

**A file compiled without the mapping is missing from the report, not reported
as uncovered.** The flags are therefore applied to the whole directory scope in
[`cmake/Coverage.cmake`](../cmake/Coverage.cmake), the same way the sanitizers
are, rather than to individual targets.

**The library stays a DLL.** A static coverage build would let the linker drop
every function no test calls, and those are precisely the functions a coverage
report exists to point at; the reported percentage would rise as coverage got
worse. Keeping the library shared keeps the whole of it in the report.

**Each module writes its own raw profile.** The DLL and every test executable
carry their own copy of the profile runtime, so the `LLVM_PROFILE_FILE` pattern
the script sets ends in `%8m`: `%m` separates the modules, and the count in
front of it asks the runtime to merge into a pool of eight files per module
instead of one file per process. A full suite run is hundreds of processes, and
one raw profile each — every one of them holding a counter for every region of
a 60 MB library — would be gigabytes of intermediate data.

Those profiles must then be told apart again, because merging all of them and
reporting against the library makes `llvm-cov` warn that several hundred
functions have mismatched data and quietly drop them: the library and a test
executable compile the same inline functions into differently shaped counters.
Each raw profile carries the binary ID of the module that wrote it, and for a
COFF image that ID is the PDB GUID from its debug directory, so the script reads
it from both sides — `llvm-readobj --coff-debug-directory` on the DLL,
`llvm-profdata show --binary-ids` on one profile per module signature — and
merges only the matching group. If a future toolchain stops emitting binary IDs
the script says so and falls back to merging everything.

## Doing it by hand

The presets are usable without the script. The build tree is instrumented as
soon as `EXYOKIOFFICE_COVERAGE` is on:

```powershell
cmake --preset windows-ninja-clang-coverage
cmake --build --preset ninja-clang-coverage

$env:LLVM_PROFILE_FILE = "$PWD\build\coverage\profraw\%8m.profraw"
ctest --preset ninja-clang-coverage -L unit

llvm-profdata merge -sparse build\coverage\profraw\*.profraw `
    -o build\coverage\coverage.profdata
llvm-cov report build\ninja-clang-coverage\ExyokiOffice.dll `
    -instr-profile=build\coverage\coverage.profdata
```

Merging every raw profile like that is the shortcut, and it is what produces the
mismatched-function warning described above; the script narrows the merge to the
library's own profiles instead.

The coverage preset turns `EXYOKIOFFICE_RUN_GENERATOR` off. The generated
sources are committed, so the build does not need the generator to produce
them, and running an instrumented generator over the source tree would scatter
raw profiles through it and rewrite tracked files for no benefit.

`llvm-cov show` accepts `-name=` and `-name-regex=` to narrow a report to one
class or function, which is usually a better way in than scrolling the HTML:

```powershell
llvm-cov show build\ninja-clang-coverage\ExyokiOffice.dll `
    -instr-profile=build\coverage\coverage.profdata `
    -name-regex=".*FormulaParser.*"
```

## Inline code in headers

Coverage of a function is attributed to the binary that compiled it. For the
library's `.cpp` files - nearly all of its mass - that is always the DLL. An
inline function defined in a header is different: each binary that instantiates
it carries its own copy, and the DLL-scoped report above only sees the DLL's.
A test that exercises, say, a generated enum's `IsValid()` purely from test
code improves the test executable's copy, which this report does not measure.

Two remedies suggest themselves, and neither works:

- **Moving such functions out of the header** would make them attributable,
  but the generated enum accessors are `constexpr` two-instruction bodies on
  hot validation paths; out-of-line definitions would cost the `constexpr`,
  add an exported symbol per enum class, and turn an inlined compare into a
  cross-DLL call.
- **Reporting across every binary** (`llvm-cov` accepts repeated `-object`
  arguments) does count header code instantiated anywhere. In this tree it
  also reports thousands of *functions have mismatched data*: the same inline
  function hashes differently across modules - measurably even between two
  test executables, and identically so on a fully clean build - so a
  meaningful share of records cannot be attributed cleanly and the combined
  numbers carry an error bar that is hard to state.

What does work is removing the seams altogether: `-Monolith` below. Short of
that, treat the layered report as authoritative for everything the DLL
compiles, and expect header-only code to be under-attributed by however much
of it only tests instantiate.

## The monolith

```powershell
.\WinCoverage.ps1 -Monolith
```

`ExyokiOfficeMonolithTests` is the library's translation units and every test
layer compiled into a single executable (`EXYOKIOFFICE_TEST_MONOLITH`, built
by the `windows-ninja-clang-coverage-monolith` preset, which also switches to
a static-library configuration; the assembly lives in
`exyokioffice_add_test_monolith` in [`cmake/Tests.cmake`](../cmake/Tests.cmake)).
One binary means one coverage map and one raw profile: no records are dropped
as mismatched, and inline code in headers is attributed no matter which test
instantiated it. MC/DC is always on, and the report is pinned to the
`include/`, `sources/` and `3rdparty/` trees so the test code compiled into
the same binary does not pad the numbers.

The mode's costs are the reasons it is not the default. The suite runs as one
serial process instead of a parallel CTest schedule, and the executable is a
second full compilation of the library and every test layer. The selection
switches do not apply and the script says so: `-Label`, `-LabelExclude`, and
`-AllTests` are refused with an error under `-Monolith`, and `-Configuration
Debug` is refused as well because no Debug monolith preset exists. The
subprocess-driven CTest entries - the tool catalog checks, the MCP replay,
the example smoke runs - stay outside the executable, so the sliver of
coverage only they produce is absent.

The totals differ slightly from the layered report in both directions, and
that is not an error: header code instantiated only by tests joins the
population, while duplicate per-module instantiations collapse into one.

## The MSVC alternative

Source-based coverage has no cl.exe equivalent, so `EXYOKIOFFICE_COVERAGE=ON`
is rejected outright unless the compiler is Clang or clang-cl. If you would
rather not maintain a second build tree,
[OpenCppCoverage](https://github.com/OpenCppCoverage/OpenCppCoverage) reads the
PDBs of an ordinary `windows-vs` build and needs no instrumentation at all:

```powershell
OpenCppCoverage.exe --sources sources --sources include `
                    --export_type html:build\coverage-msvc `
                    -- build\vs\Debug\tests\unit\ExyokiOfficeUnitTests.exe
```

It is line coverage only — no regions, no branches — and it slows execution
down considerably more, but it measures the toolchain the project actually
ships with.
