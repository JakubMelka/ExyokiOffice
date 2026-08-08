# tests/fuzz

libFuzzer targets for the parsing surfaces. See [docs/fuzzing.md](../../docs/fuzzing.md)
for the full description; this file is the map of the directory.

```
FuzzHarness.hpp     ByteTape input reader, SafeLimits(), EXYOKIOFFICE_FUZZ_CHECK
FuzzTargets.hpp     declarations of every RunXxx entry point
FuzzTargets.cpp     the name -> entry point table used by the replay test
Targets/            one translation unit per target
Entry/              three-line LLVMFuzzerTestOneInput wrappers, fuzz builds only
corpus/<target>/    committed seed inputs, read-only during a run
crashes/<target>/   committed findings, each described in that folder's README
dictionaries/       token lists that give the mutator a head start
```

The entry points are built as `ExyokiOfficeFuzzTargets`, a plain static
library with no libFuzzer dependency. It is linked both into the fuzz
executables and into `ExyokiOfficeUnitTests`, where `FuzzCorpusReplayTests`
replays `corpus/` and `crashes/` in a normal MSVC build. That is what makes a
committed crash artifact a regression test.

Quick start:

```powershell
.\WinFuzz.ps1 -Target all -Seconds 60
```
