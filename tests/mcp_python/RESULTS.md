# Baseline results

Baseline date: 2026-08-05 (third run, after the destination-family check)

Environment:

- Windows, Python 3.13.14;
- official MCP Python SDK 2.0.0;
- ExyokiOffice Debug MCP executables from `build/vs/tools/mcp/Debug`;
- test command: `.venv\Scripts\python.exe -m pytest -q`.

Result: **69 passed, no expected failures**.

Every `xfail(strict=True)` marker this suite once carried is gone, and each was
removed by changing the server rather than the test:

- the four lifecycle ones of the 2026-08-04 baseline — the servers now refuse
  an `initialize` that omits or malforms `protocolVersion`, `capabilities` or
  `clientInfo`, refuse a second `initialize` on the same connection, and no
  longer let a lone `notifications/initialized` unlock the catalog;
- the three destination-extension ones — a destination whose extension belongs
  to another Office family is now `family_mismatch`, so a Word package can no
  longer be created under an `.xlsx` name whose bytes and filename would
  disagree.

Coverage demonstrated by passing tests:

- official SDK startup, MCP 2025-11-25 negotiation, capabilities and shutdown
  for all three executables;
- stable discovery of 50 Word, 48 Excel and 50 PowerPoint tools;
- syntactic validation of every input and output schema as JSON Schema
  2020-12;
- presence and consistency of tool annotations;
- read-only and `--toolsets lifecycle` catalog filtering;
- rejection of an unknown property by every one of the 148 tools;
- output-schema validation of all 148 resulting error envelopes;
- byte-for-byte semantic equality of their JSON text blocks and
  `structuredContent` objects;
- DOCX create/edit/table/read/validate/save/close/reopen round trip;
- XLSX values/formula/style/freeze/recalculate/read/validate/save/reopen round
  trip;
- PPTX slide/placeholders/text box/notes/read/validate/save/reopen round trip;
- batch revision accounting, one-step undo and failed-batch rollback;
- parent traversal and absolute outside-workspace rejection for all families;
- refusal of Windows device names, alternate data streams and `\\?\` paths with
  `path_invalid`, and of a `split_document` prefix that would leave the output
  directory;
- refusal of a destination naming another Office family, with nothing written;
- dirty-close protection for all families;
- schema errors with JSON pointers;
- configured open-document limit;
- overwrite refusal preserving original bytes;
- detection of an external change before save;
- pre-initialization tool lock, strict `initialize` member checking, one
  handshake per connection, version negotiation, parse-error recovery and
  notification silence at the raw JSON-RPC boundary.

Not covered by this black-box suite:

- visual rendering or screenshot comparison in Microsoft Office/LibreOffice;
- GUI-specific behaviour of Claude Desktop, Cursor or VS Code;
- Streamable HTTP, because these servers intentionally expose stdio only;
- symlink and junction escapes: creating one needs a privilege a default
  Windows account does not have, so neither this suite nor the native one
  attempts it. What the deleted native test proved — that containment is decided
  after links are expanded — is now covered by the refusal of any path the file
  system cannot canonicalize;
- performance and memory profiling of genuinely large real-world documents;
- fuzzing the JSON parser or Office package parser (covered by separate native
  fuzzing infrastructure where configured).

