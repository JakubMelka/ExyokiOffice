# ExyokiOffice MCP Python SDK interoperability tests

This directory is an independent black-box test suite for the three
ExyokiOffice MCP executables. It deliberately uses the official
[`mcp`](https://pypi.org/project/mcp/) Python SDK as the client instead of the
C++ test harness from `tests/mcp`.

The suite checks four different things:

1. real stdio startup, initialization, shutdown, and SDK interoperability;
2. the complete published tool catalog and its JSON Schema contracts;
3. realistic create/edit/read/validate/save/reopen workflows for DOCX, XLSX,
   and PPTX;
4. security and failure behaviour such as workspace traversal, dirty close,
   overwrite protection, family mismatch, batch rollback, and read-only mode.

It does not import or link ExyokiOffice. Every observation crosses the same
stdio boundary that Claude Code, Claude Desktop, VS Code, Cursor, or another
MCP host uses.

## Setup

From this directory on Windows:

```powershell
python -m venv .venv
.\.venv\Scripts\python -m pip install -e .
```

The official MCP SDK is MIT licensed. The dependency is used only by this
test project and is not linked into, packaged with, or required by the C++
library or its MCP server executables.

## Selecting server binaries

By default the tests search the repository build trees, preferring these
locations:

```text
build/vs/tools/mcp/Debug
build/vs/tools/mcp/RelWithDebInfo
build/vs/tools/mcp/Release
build/ninja-debug/tools/mcp
build/ninja-release/tools/mcp
```

Override individual binaries when testing an installed build:

```powershell
$env:EXYOKI_MCP_WORD_EXE = 'C:\ExyokiOffice\bin\exyoki-mcp-word.exe'
$env:EXYOKI_MCP_EXCEL_EXE = 'C:\ExyokiOffice\bin\exyoki-mcp-excel.exe'
$env:EXYOKI_MCP_POWERPOINT_EXE = 'C:\ExyokiOffice\bin\exyoki-mcp-power-point.exe'
```

## Running

Run everything:

```powershell
.\.venv\Scripts\python -m pytest
```

## Running it from CTest

The suite is also registered as a single CTest entry, `Mcp.Python`, when the
build is configured with `EXYOKIOFFICE_BUILD_MCP_PYTHON_TESTS=ON`:

```powershell
cmake --preset windows-vs -DEXYOKIOFFICE_BUILD_MCP_PYTHON_TESTS=ON
ctest --test-dir build\vs -C Debug -L mcp-python
```

The option is off by default because it needs the environment above, which the
C++ build cannot produce. A `.venv` next to this file is found automatically;
`EXYOKIOFFICE_MCP_PYTHON` points the entry at another interpreter. The entry
sets `EXYOKI_MCP_*_EXE` to the binaries of that build, so it never picks up a
different build tree. The `mcp-python` CI job runs exactly this — see
[docs/ci.md](../../docs/ci.md).

Useful subsets:

```powershell
.\.venv\Scripts\python -m pytest -m conformance
.\.venv\Scripts\python -m pytest -m catalog
.\.venv\Scripts\python -m pytest -m workflow
.\.venv\Scripts\python -m pytest -m security
.\.venv\Scripts\python -m pytest -k word -vv
```

Use `--keep-workspaces` to retain produced documents for manual inspection:

```powershell
.\.venv\Scripts\python -m pytest --keep-workspaces --artifact-root artifacts
```

Without that option every test receives a fresh temporary workspace and
pytest removes it after the run. Tests never use the repository root as an MCP
workspace.

## Expected failures and interpretation

Conformance tests express requirements of MCP, not the current implementation.
If one fails, do not weaken the assertion merely to make the suite green.
In particular, initialization must be the first interaction, and an
`initialized` notification alone must not unlock tools.

Passing this suite demonstrates interoperability with the official Python SDK
and the tested black-box behaviour. It does not by itself prove visual fidelity
in Microsoft Office or compatibility with every MCP host GUI.

The latest checked baseline, including deliberate expected failures and the
exact coverage demonstrated by the run, is recorded in [RESULTS.md](RESULTS.md).
