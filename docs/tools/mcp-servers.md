# MCP servers

ExyokiOffice ships three Model Context Protocol servers that let an AI agent
create, read, edit, validate, and save Office documents through typed tools
instead of through generated code:

| Binary | Format | Server title |
| --- | --- | --- |
| `exyoki-mcp-word` | `.docx` | ExyokiOffice Word MCP Server |
| `exyoki-mcp-excel` | `.xlsx` | ExyokiOffice Excel MCP Server |
| `exyoki-mcp-power-point` | `.pptx` | ExyokiOffice PowerPoint MCP Server |

Each server speaks JSON-RPC 2.0 over stdio, one UTF-8 JSON message per line.
They link the same public API as [`exyoki`](exyoki.md) and share one static
core (`tools/mcp/core`), so lifecycle, reading, editing, and file utilities
behave identically in all three; only the family toolset differs.

The servers implement MCP revision **2025-11-25** and also accept
`2025-06-18` and `2024-11-05`; a client that asks for any of those gets that
same revision back from `initialize`, and a client that asks for anything else
is answered with `2025-11-25` so it can decide whether to continue. Revision
`2025-03-26` is deliberately not offered: it is the one revision that requires
servers to accept JSON-RPC batches, and this transport reads exactly one
message per line. Only the `tools` capability is declared — no resources,
prompts, sampling, or logging — and `tools/list` never changes during a run.

Initialization is checked strictly, because everything after it depends on
what was agreed there. An `initialize` request must carry `protocolVersion`,
`capabilities`, and a `clientInfo` with a name and a version, or it is refused
with `-32602`; it is answered once per connection, a second one being `-32600`;
and a `notifications/initialized` that arrives without it is ignored rather
than treated as a handshake, so a client cannot reach the tools without having
negotiated a revision. Every MCP client does this correctly on its own; it
matters when you write request lines by hand.

Everything an agent can do goes through a named tool with a published JSON
Schema. There is no code-execution escape hatch, no raw XML writing, and no
access outside the configured workspace.

## Contents

- [Installing and registering a server](#installing-and-registering-a-server)
- [Checking a server before you register it](#checking-a-server-before-you-register-it)
- [Command-line options](#command-line-options)
- [Security model](#security-model)
- [How a conversation looks](#how-a-conversation-looks)
- [Worked example: a Word report](#worked-example-a-word-report)
- [Worked example: an Excel workbook](#worked-example-an-excel-workbook)
- [Worked example: a PowerPoint deck](#worked-example-a-powerpoint-deck)
- [Task recipes](#task-recipes)
- [Result envelope](#result-envelope)
- [Error codes](#error-codes)
- [Addressing content](#addressing-content)
- [Units, colors, and limits](#units-colors-and-limits)
- [Sessions, undo, and batches](#sessions-undo-and-batches)
- [Tool catalog](#tool-catalog)
- [Published catalogs and schemas](#published-catalogs-and-schemas)
- [Looking up a tool's exact arguments](#looking-up-a-tools-exact-arguments)
- [Troubleshooting](#troubleshooting)
- [Limits of this version](#limits-of-this-version)

## Installing and registering a server

The servers are built by default (`EXYOKIOFFICE_BUILD_MCP`) and installed next
to `exyoki` in the binary directory:

```powershell
.\WinBuild.ps1 -Configuration RelWithDebInfo -Install -InstallPrefix C:\ExyokiOffice
```

Register one server per document family. In Claude Code, that is `.mcp.json`
in the project root:

```jsonc
{
  "mcpServers": {
    "word": {
      "command": "C:/ExyokiOffice/bin/exyoki-mcp-word.exe",
      "args": ["--workspace", "C:/Users/me/Documents/agentwork"]
    },
    "excel": {
      "command": "C:/ExyokiOffice/bin/exyoki-mcp-excel.exe",
      "args": ["--workspace", "C:/Users/me/Documents/agentwork"]
    },
    "powerpoint": {
      "command": "C:/ExyokiOffice/bin/exyoki-mcp-power-point.exe",
      "args": ["--workspace", "C:/Users/me/Documents/agentwork"]
    }
  }
}
```

Claude Desktop uses the same object under `mcpServers` in
`claude_desktop_config.json`; VS Code uses `.vscode/mcp.json` with a `servers`
member; Cursor uses `.cursor/mcp.json` with `mcpServers`. The shape of the
entry — `command` plus `args` — is the same in all four.

There is also a container image carrying all three servers, where the entry
becomes `"command": "docker"` and nothing has to be installed on the machine at
all. Run it with `--network none`: a server reached over standard input and
output needs no network, and denying it one puts a second boundary under the
workspace sandbox described below. See [The container image](docker.md) and
[Closing the network](docker.md#closing-the-network).

To try a server by hand, the reference inspector speaks the same protocol:

```powershell
npx @modelcontextprotocol/inspector C:/ExyokiOffice/bin/exyoki-mcp-word.exe --workspace .
```

## Checking a server before you register it

A misconfigured server shows up in a client as "no tools available", which says
nothing about the cause. These three checks isolate it in under a minute, and
none of them needs a client.

**1. The binary starts and reports its version.**

```powershell
exyoki-mcp-word --version
# exyoki-mcp-word 1.0.0
```

**2. The catalog is what you expect.** `--print-tools` writes the catalog to
standard output and exits, so it also tells you what `--read-only` and
`--toolsets` actually did:

```powershell
exyoki-mcp-word --print-tools | ConvertFrom-Json | Select-Object server, toolCount
# server            toolCount
# ------            ---------
# exyoki-mcp-word          50

exyoki-mcp-word --read-only --print-tools | ConvertFrom-Json | Select-Object toolCount
```

**3. A whole conversation runs.** `--replay` reads requests from a JSON Lines
file instead of standard input — one JSON-RPC message per line, exactly what a
client would send. Write the file, run it, read the answers:

```jsonc
// smoke.jsonl
{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-11-25","capabilities":{},"clientInfo":{"name":"smoke","version":"1"}}}
{"jsonrpc":"2.0","method":"notifications/initialized"}
{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"create_document","arguments":{"path":"smoke.docx","overwrite":true}}}
{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"insert_paragraph","arguments":{"documentId":"doc-1","anchor":{"position":"end"},"text":"Hello"}}}
{"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"save_document","arguments":{"documentId":"doc-1"}}}
```

```powershell
exyoki-mcp-word --workspace . --log-level info --replay smoke.jsonl
```

Standard output carries the JSON-RPC answers; the log goes to standard error:

```text
[info] exyoki-mcp-word 1.0.0 ready with 50 tools.
[info] Input closed; shutting down.
```

The same mechanism backs the `Mcp.Replay.word`, `Mcp.Replay.excel`, and
`Mcp.Replay.power-point` CTest entries, so a replay file is also the natural
way to hand over a reproducible bug report.

**What `initialize` tells the agent.** The response carries `serverInfo`,
`capabilities`, and an `instructions` string the client passes to the model. It
states the workflow (open or create, edit against a `documentId`, save), how
that family addresses content, what a length and a color look like, and that
indices shift after structural edits. An agent therefore arrives already
knowing the conventions of this page — you do not have to repeat them in your
own prompt.

## Command-line options

All three binaries take the same options. Every option also reads an
environment variable; the command line wins when both are set.

| Option | Environment variable | Default | Meaning |
| --- | --- | --- | --- |
| `--workspace <dir>` | `EXYOKI_MCP_WORKSPACE` | working directory | Root of the permitted file space. Repeat for several roots. |
| `--read-only` | `EXYOKI_MCP_READ_ONLY` | off | Register only tools that never modify anything. |
| `--toolsets <a,b,…>` | `EXYOKI_MCP_TOOLSETS` | all groups | Register only these tool groups. |
| `--max-documents <n>` | `EXYOKI_MCP_MAX_DOCUMENTS` | 16 | Limit on simultaneously open documents. |
| `--snapshot-depth <n>` | `EXYOKI_MCP_SNAPSHOT_DEPTH` | 8 | Undo snapshots per document; `0` disables `undo` and leaves a failed edit unrolled back. |
| `--max-media-size <MiB>` | `EXYOKI_MCP_MAX_MEDIA_SIZE` | 8 | Limit on base64 media payloads in and out. |
| `--package-limits <mode>` | `EXYOKI_MCP_PACKAGE_LIMITS` | `recommended` | ZIP/XML safety limits applied to every document opened; `recommended` or `unlimited`. |
| `--log-level <level>` | `EXYOKI_MCP_LOG_LEVEL` | `warn` | `error`, `warn`, `info`, or `debug`; the log goes to stderr. |
| `--print-tools` | — | — | Print the tool catalog as JSON and exit. |
| `--replay <file>` | — | — | Read requests from a JSON Lines file instead of standard input. |
| `--version` | — | — | Print the name and the library version. |

`--toolsets` filters strictly by group, including `lifecycle`. A server started
with `--toolsets content` can neither open nor save a document, so include
`lifecycle` unless you only want the path-based `files` utilities:

```powershell
exyoki-mcp-word --toolsets lifecycle,read,content
```

Standard output carries the protocol and nothing else. Diagnostics, including
everything `--log-level debug` prints, go to standard error.

## Security model

- **Workspace sandbox.** Every path an agent supplies is made absolute,
  canonicalized — which resolves `..` segments and expands symbolic links — and
  then tested for containment in one of the configured roots. Anything outside
  is refused with `path_outside_workspace`, and so is anything the file system
  declined to canonicalize: without the canonical form there is nothing to
  decide containment on. Outputs report paths relative to their root, with `/`
  separators.
- **Refused path shapes.** Some paths are turned away on their shape alone,
  with `path_invalid`, before anything is opened: an extended-length or device
  prefix (`\\?\`, `\\.\`) and a path carrying an embedded NUL, everywhere; and
  on Windows a UNC path, a drive-relative path such as `C:report.docx`, a
  reserved device name (`NUL`, `nul.txt`, `COM1.docx`), an alternate data
  stream (`report.docx:hidden`), and a name ending in a dot or a space. Each of
  these either acts merely by being opened — a UNC path authenticates against a
  host the client named, a save to `NUL` reports success and stores nothing — or
  means something different to Windows than to the checks built on top of the
  path. Arguments that become file names, such as the `prefix` of
  `split_document`, are held to the same rule, and so are the names the library
  derives from a document — `export_media` names each file after the part it
  came from, and a part called `NUL.png` or `image1:hidden.png` is renamed
  rather than written where it asked.
- **One family per destination.** A tool that writes a document writes this
  server's family whatever the file is called, so a destination whose extension
  belongs to another Office family — `report.xlsx` from the Word server — is
  refused with `family_mismatch`. This covers `create_document`,
  `save_document`, `merge_documents` and `redact_document`. An extension the
  server does not recognize as an Office one is accepted: it may be a
  deliberate working name. `convert_document` is exempt, since choosing another
  format is what it is for.
- **Package safety limits.** Every document is opened under
  `OpenXmlPackageLimits::Recommended()`: at most 10 000 ZIP entries, 256 MiB
  compressed, 2 GiB uncompressed, 512 MiB per part, a 200:1 compression ratio,
  and 256 levels of XML nesting. A document that exceeds any of them fails to
  open with `package_load_failed` instead of exhausting memory or the stack —
  the documents an agent is handed come from wherever the agent has been, and a
  decompression bomb is a plausible thing to be handed. `--package-limits
  unlimited` switches the guard off and logs a warning at start-up. The limits
  reach both kinds of open: the family adapter passes them to the editor, and
  the same values are installed as the process-wide default so the tools that
  work file to file (`validate_document`, `diff_documents`, `get_stats`,
  `convert_document`) load under them too. The `ExyokiOffice::Tools` module
  those tools call defaults to the same bound on its own, so the guard does not
  depend on the server remembering to install it.
- **No code execution.** The servers offer typed tools and a declarative
  `batch`; there is no scripting tool, no shell, and no document-builder
  bridge.
- **Nothing is written to work.** Reading tools operate on the open document in
  memory, and the editing tools that used to run file to file — `replace_text`
  and `redact_document` — now mutate the open document directly. A tool run
  against a session writes no temporary copy of it anywhere, so there is no
  window in which the whole document sits in a directory outside the workspace.
- **Bounded messages.** A JSON-RPC line longer than 16 MiB is dropped and
  answered with a parse error rather than read into memory, and the rest of the
  line is consumed so the next message starts on a boundary.
- **Read-only mode.** `--read-only` drops every mutating tool from the catalog
  at start-up rather than failing it at call time, so the published catalog is
  an honest description of what the server can do.
- **No outside access.** The library installs no external-resource resolver, so
  a document that points at a remote image or workbook is never followed. Every
  tool is annotated `openWorldHint: false`.
- **Overwrite protection.** Writing tools take `overwrite`, default false, and
  `save_document` refuses to overwrite a file that changed on disk since the
  session opened it unless `force` is passed.

## How a conversation looks

Every session has the same five phases, whatever the family:

1. **Handshake** — `initialize` carrying `protocolVersion`, `capabilities`, and
   `clientInfo`, then the `notifications/initialized` notification. The client
   does this; you never write it by hand.
2. **Open** — `create_document` or `open_document` returns a `documentId`.
   Nothing is on disk yet.
3. **Edit** — family tools take that `documentId`. Each answer reports the new
   `revision` and the indices of what it touched.
4. **Verify** — `get_document_markdown` for content, `validate_document` for
   package correctness. Both are cheap; do them before saving.
5. **Save and close** — `save_document` writes atomically, `close_document`
   releases the session and its undo history.

The three worked examples below are transcripts of real runs, produced by
feeding the request lines to the matching server with `--replay`. The `// ←`
comments are the `structuredContent` the server actually returned, abbreviated
only where a field is not the point of the example.

## Worked example: a Word report

Heading, table, check, save. The `anchor` decides where a block lands, and the
result of every insertion tells you the block index it got.

```jsonc
{"name": "create_document", "arguments": {"path": "report.docx", "overwrite": true}}
// ← {"ok": true, "documentId": "doc-1", "revision": 0, "dirty": false,
//    "summary": "Created a new Word document as doc-1.",
//    "data": {"documentId": "doc-1", "family": "word", "path": "report.docx"}}

{"name": "insert_paragraph",
 "arguments": {"documentId": "doc-1", "anchor": {"position": "end"},
               "text": "Quarterly results", "heading_level": 1}}
// ← {"ok": true, "revision": 1, "dirty": true,
//    "summary": "Inserted a paragraph as block 1.",
//    "data": {"block": 1, "blockCount": 1}}

{"name": "insert_table",
 "arguments": {"documentId": "doc-1", "anchor": {"position": "end"},
               "rows": 2, "cols": 2, "header_row": true,
               "data": [["Region", "Revenue"], ["North", "1200"]]}}
// ← {"ok": true, "revision": 2, "summary": "Inserted a 2x2 table as block 2.",
//    "data": {"block": 2, "rows": 2, "columns": 2}}

{"name": "get_outline", "arguments": {"documentId": "doc-1"}}
// ← {"ok": true, "summary": "The document has 1 heading(s).",
//    "data": {"headings": [{"block": 1, "level": 1,
//                           "styleId": "Heading1", "text": "Quarterly results"}],
//             "bookmarks": []}}

{"name": "validate_document", "arguments": {"documentId": "doc-1"}}
// ← {"ok": true, "summary": "Validation found 0 error(s) and 0 warning(s).",
//    "data": {"errorCount": 0, "warningCount": 0, "issues": []}}

{"name": "save_document", "arguments": {"documentId": "doc-1"}}
// ← {"ok": true, "revision": 2, "dirty": false,
//    "summary": "Saved doc-1 to report.docx.",
//    "data": {"path": "report.docx", "bytesWritten": 2373}}

{"name": "close_document", "arguments": {"documentId": "doc-1"}}
// ← {"ok": true, "summary": "Closed doc-1.", "data": {"documentId": "doc-1"}}
```

`save_document` without `path` writes back to where the session came from.
Pass `path` to save elsewhere, and `overwrite: true` if that file exists.

Filling the table afterwards uses `edit_table_cell` with `block`, `row`, `col`
— the block index the insertion just reported. To style existing text, call
`apply_style` with block indices from `get_outline` or `read_blocks`.

## Worked example: an Excel workbook

`write_range` fills a rectangle from an origin, and a cell value can be a
formula object. `recalculate` then computes it, so the saved file carries a
cached result rather than an empty cell.

```jsonc
{"name": "create_document", "arguments": {"path": "sales.xlsx", "overwrite": true}}
// ← {"ok": true, "documentId": "doc-1", "revision": 0,
//    "data": {"documentId": "doc-1", "family": "excel", "path": "sales.xlsx"}}

{"name": "write_range",
 "arguments": {"documentId": "doc-1", "origin": "A1",
               "values": [["Region", "Revenue"],
                          ["North", 1200],
                          ["South", 900],
                          ["Total", {"formula": "SUM(B2:B3)"}]]}}
// ← {"ok": true, "revision": 1, "summary": "Wrote 8 cell(s) into A1:B4.",
//    "data": {"range": "A1:B4", "written": 8}}

{"name": "format_range",
 "arguments": {"documentId": "doc-1", "range": "A1:B1", "font": {"bold": true}}}
// ← {"ok": true, "revision": 2, "summary": "Formatted A1:B1.",
//    "data": {"range": "A1:B1", "styleIndex": 1}}

{"name": "recalculate", "arguments": {"documentId": "doc-1"}}
// ← {"ok": true, "revision": 3, "summary": "Recalculated 1 formula cell(s).",
//    "data": {"recalculatedCells": 1, "circularReferences": [], "formulaErrors": []}}

{"name": "read_range", "arguments": {"documentId": "doc-1", "range": "A1:B4"}}
// ← {"ok": true, "summary": "Read rows 1 to 4 of Sheet1.",
//    "data": {"range": "A1:B4", "nextOffset": 0,
//             "values": [["Region", "Revenue"], ["North", 1200.0],
//                        ["South", 900.0], ["Total", "2100"]]}}
```

`recalculate` reports `circularReferences` and `formulaErrors` as arrays rather
than failing the call, so an agent can decide whether a `#REF!` matters. Omit
`sheet` and the tools use the active worksheet; `list_sheets` shows the names
and used ranges when you need to be explicit.

## Worked example: a PowerPoint deck

`add_slide` writes the title and bullets into the layout's real placeholders,
so the deck's formatting applies and the outline view sees the slide. A
presentation created from scratch has no master; the first `add_slide` creates
one together with a default layout.

```jsonc
{"name": "create_document", "arguments": {"path": "deck.pptx", "overwrite": true}}
// ← {"ok": true, "documentId": "doc-1", "revision": 0,
//    "data": {"documentId": "doc-1", "family": "powerpoint", "path": "deck.pptx"}}

{"name": "set_slide_size", "arguments": {"documentId": "doc-1", "preset": "16:9"}}
// ← {"ok": true, "revision": 1, "data": {"widthPt": 960.0, "heightPt": 540.0}}

{"name": "add_slide",
 "arguments": {"documentId": "doc-1", "title": "Results",
               "bullets": ["Revenue up 12%", "Churn down"]}}
// ← {"ok": true, "revision": 2, "summary": "Added slide 1.",
//    "data": {"slide": 1, "layout": "Title and Content"}}

{"name": "add_chart",
 "arguments": {"documentId": "doc-1", "slide": 1, "type": "column",
               "categories": ["Q1", "Q2"],
               "series": [{"name": "Revenue", "values": [10.0, 12.0]}],
               "x": "2cm", "y": "8cm", "width": "18cm", "height": "8cm"}}
// ← {"ok": true, "revision": 3, "summary": "Added a chart to slide 1.",
//    "data": {"shape": "3"}}

{"name": "set_notes",
 "arguments": {"documentId": "doc-1", "slide": 1, "text": "Mention the outlook."}}
// ← {"ok": true, "revision": 4, "summary": "Wrote the speaker notes."}

{"name": "get_slide", "arguments": {"documentId": "doc-1", "slide": 1}}
// ← {"ok": true, "data": {"layout": "Title and Content", "hidden": false,
//      "notes": "Mention the outlook.",
//      "shapes": [{"path": "1", "kind": "textBox", "placeholderType": "title",
//                  "text": "Results", "transform": {…}},
//                 {"path": "2", "kind": "textBox", "placeholderType": "body",
//                  "text": "Revenue up 12%\nChurn down", "transform": {…}},
//                 {"path": "3", "kind": "chart", "transform": {…}}]}}
```

`add_chart` returns the `shape` path — `"3"` here — which is what
`set_shape_transform` and `delete_shape` take. `get_slide` reports the path of
every shape, so the identifier an agent receives is always one it can send back.

## Task recipes

Short answers to the jobs people actually bring to these servers.

**Edit an existing document.** `open_document` with a workspace-relative path,
then the same editing tools. `get_document_info` first is cheap and tells you
the size and shape of what you opened before you read any content.

```jsonc
{"name": "open_document", "arguments": {"path": "report.docx"}}
{"name": "get_document_info", "arguments": {"documentId": "doc-1"}}
{"name": "read_blocks", "arguments": {"documentId": "doc-1", "from": 1, "count": 20}}
```

**Read a document without editing it.** Every reading tool takes `path`
*instead of* `documentId`, opens the file, answers, and closes it. No session,
nothing to clean up:

```jsonc
{"name": "get_document_markdown", "arguments": {"path": "report.docx"}}
{"name": "search_text", "arguments": {"path": "report.docx", "needle": "quarterly"}}
```

**Mail merge.** `fill_template` takes a JSON object and fills `MERGEFIELD`
results, same-paragraph bookmarks, and `TableStart`/`TableEnd` repeating
regions. It works on a session or file to file:

```jsonc
{"name": "fill_template",
 "arguments": {"documentId": "doc-1", "data": {"Customer": "Acme Corp."}}}
```

**Compare two versions.** `compare_documents` is path to path and writes a
third file whose differences are tracked revisions, which is what a reviewer
opens in Word:

```jsonc
{"name": "compare_documents",
 "arguments": {"original_path": "v1.docx", "revised_path": "v2.docx",
               "output_path": "compared.docx"}}
```

**Scrub a document before publishing.** `redact_document` removes comments,
tracked revisions, hidden text, and identity metadata. Against a session it
mutates the in-memory document (and participates in undo) without writing a
temporary package; save it explicitly afterwards. It can also work path to
path, where a separate `output_path` keeps the source unchanged:

```jsonc
{"name": "redact_document",
 "arguments": {"documentId": "doc-1"}}

{"name": "redact_document",
 "arguments": {"input_path": "draft.docx", "output_path": "public.docx"}}
```

**Feed a pipeline or an index.** `convert_document` writes Markdown, JSON,
plain text, semantic XML — and CSV on the Excel server. Losses are reported as
warnings, never dropped silently; the [conversion formats](conversion-formats.md)
chapter has the full matrix:

```jsonc
{"name": "convert_document",
 "arguments": {"input_path": "report.docx", "output_path": "report.md"}}
```

`get_document_model` returns the same semantic JSON envelope in the answer
instead of writing a file, which is usually what an agent wants.

**Find out what is in the workspace.** `list_workspace` takes a glob and
defaults to this server's own extension:

```jsonc
{"name": "list_workspace", "arguments": {"glob": "*.docx"}}
```

## Result envelope

Every tool answers with the same `structuredContent` envelope, and its
`outputSchema` describes it. The `content` array repeats the envelope as
serialized text for clients that do not read structured output; `get_media`
adds an `image` or `audio` block.

```jsonc
{
  "ok": true,
  "documentId": "doc-1",   // session tools only
  "revision": 7,           // successful mutations applied to this session
  "dirty": true,           // unsaved changes
  "summary": "Inserted a paragraph as block 12.",
  "data": { },             // tool-specific, described by outputSchema
  "warnings": [{"code": "content_warning", "message": "…", "context": "…"}],
  "truncated": false       // true when the payload was shortened
}
```

A failure sets `isError: true` on the `tools/call` result and replaces the body:

```jsonc
{
  "ok": false,
  "error": {
    "code": "sheet_not_found",
    "message": "The workbook has no worksheet 'Data2'.",
    "target": "Data2",
    "hint": "Call list_sheets to see the available worksheets.",
    "details": []
  }
}
```

The `hint` names the tool that resolves the situation wherever one exists, so
an agent can repair itself instead of guessing.

Only protocol failures travel as JSON-RPC errors: `-32700` for an unparsable
line, `-32600` for a malformed message or a repeated `initialize`, `-32601` for
an unknown method, `-32602` for an unknown tool, malformed `tools/call`
parameters, or an `initialize` missing a member the specification requires, and
`-32002` for a `tools/*` call before `initialize`.

## Error codes

`input_invalid`, `path_outside_workspace`, `path_invalid`, `file_not_found`, `file_exists`,
`file_changed_on_disk`, `package_load_failed`, `document_not_found`,
`document_limit_reached`, `read_only_mode`, `family_mismatch`,
`anchor_invalid`, `block_not_found`, `sheet_not_found`, `range_invalid`,
`slide_not_found`, `shape_not_found`, `layout_not_found`, `style_not_found`,
`media_not_found`, `comment_not_found`, `template_field_missing`,
`validation_failed`, `operation_failed`, `batch_aborted`,
`snapshot_unavailable`, `unsupported`, `internal_error`.

The list is closed: an agent may branch on these strings.

## Addressing content

**Word.** The body is a flat sequence of blocks — paragraphs, tables, and the
final section properties — numbered from 1, exactly as `read_blocks` reports
them. Inserting takes an anchor:

```jsonc
{"position": "end"}
{"position": "start"}
{"position": "before", "block": 12}
{"position": "after",  "block": 12}
```

Table cells are `block`, `row`, `col`, all 1-based, over the table's logical
grid; a cell covered by a merge is refused with `anchor_invalid`.

**Excel.** Worksheets are addressed by name (case-insensitive) or by 1-based
index. Cells and ranges are always A1 notation (`"B2"`, `"A1:C10"`); column and
row bands are `"B"`, `"B:D"`, `"2"`, and `"2:5"` where a tool says so. A cell
value is one of:

```jsonc
"text"
42.5
true
{"formula": "SUM(A1:A9)"}
{"value": "2026-08-02T00:00:00", "type": "datetime"}
null                                  // write_range: leave the cell untouched
```

**PowerPoint.** Slides are 1-based indices. Shapes are addressed by their path
in the shape tree: `"2"` is the second top-level shape, `"2/1"` the first child
of a group. `get_slide` reports the `path`, `kind`, and `placeholderType` of
every shape, so the path an agent receives is the one it can send back.
Placeholders are addressed by type (`"title"`, `"body"`, `"subtitle"`, …) or by
1-based index.

Structural edits shift indices. Every mutating tool reports the new indices of
what it touched, and `read_blocks`, `list_slides`, and `list_sheets` are cheap
enough to call again whenever that is not enough.

## Units, colors, and limits

Lengths are a number of points, or a string with a unit: `12`, `"2.5cm"`,
`"36pt"`, `"1in"`, `"914400emu"`, `"96px"` (screen pixels at 96 DPI). Sizes are
reported back as `{"pt": …, "emu": …}`. Colors are `"#RRGGBB"`; Word highlight
tokens are the WordprocessingML names (`"yellow"`, `"darkCyan"`, `"none"`, …).

Reading tools page with `offset` and `limit` and set `truncated` when the
payload was shortened; where a next page exists, `data.nextOffset` says where to
resume. One response carries at most about 1 MiB of payload, and `read_range`
additionally caps a single call at 10 000 cells. Base64 media is limited by
`--max-media-size`; for anything larger, use `export_media` and a workspace
file.

## Sessions, undo, and batches

`create_document` and `open_document` return a `documentId` that stays valid
until `close_document`. The document lives in memory: nothing reaches disk
until `save_document`, and `close_document` refuses a session with unsaved
changes unless `discard: true` is passed.

`undo` restores the state before the most recent mutation, up to
`--snapshot-depth` steps. Undo itself counts as a mutation, so the `revision`
counter only ever increases; `data.restoredRevision` says which state the
content came from.

`batch` applies up to 50 mutating session tools as one transaction. Every
operation is checked before the first one runs; the first failure restores the
document completely and answers `batch_aborted` with the failing index. A
successful batch counts as a single revision and a single undo step. Lifecycle
tools, file utilities, reading tools, and a nested `batch` are rejected.

```jsonc
{"name": "batch",
 "arguments": {"documentId": "doc-1",
   "operations": [
     {"tool": "set_properties",    "arguments": {"title": "Quarterly report"}},
     {"tool": "insert_paragraph",  "arguments": {"anchor": {"position": "end"},
                                                 "text": "Prepared by finance."}}]}}
// ← {"ok": true, "revision": 1, "summary": "Applied 2 operation(s) as one change.",
//    "data": {"applied": 2,
//             "results": [{"tool": "set_properties",   "data": {"written": ["title"]}, …},
//                         {"tool": "insert_paragraph", "data": {"block": 3, …}, …}]}}

{"name": "undo", "arguments": {"documentId": "doc-1"}}
// ← {"ok": true, "revision": 2, "summary": "Undid the last change to doc-1.",
//    "data": {"restoredRevision": 0}}
```

`data.results` mirrors the answer each operation would have given on its own,
so a batch stays as informative as the calls it replaces. Note the `revision`
after `undo`: it is 2, not 0. Undo is itself a mutation, and
`data.restoredRevision` is what says which state the content came from.

A failure names the operation and confirms the rollback:

```jsonc
// ← isError: true
{"ok": false,
 "error": {"code": "batch_aborted",
   "message": "Operation 2 ('apply_style') failed; the document was restored.",
   "target": "apply_style",
   "hint": "Fix that operation and resend the batch.",
   "details": [{"tool": "apply_style", "failedIndex": 1, "restored": true,
                "error": {"code": "style_not_found",
                          "message": "The document defines no style 'Quote'.",
                          "target": "Quote",
                          "hint": "Call list_styles to see the available style identifiers."}}]}}
```

`failedIndex` is 0-based while the message counts from one; the nested `error`
is the ordinary envelope of the operation that failed, hint included.

## Tool catalog

The annotation column reads **R** read-only, **M** mutating, **D**
destructive, **I** idempotent.

### Shared by all three servers

Lifecycle:

| Tool | | Purpose |
| --- | --- | --- |
| `create_document` | M | Create a document and open it as a session |
| `open_document` | M | Open an existing document as a session |
| `save_document` | M I | Write the session to disk atomically |
| `close_document` | D | Release the session and its undo history |
| `list_documents` | R I | List the open sessions |
| `undo` | M | Restore the state before the last mutation |

Reading — each accepts `documentId` **or** `path`:

| Tool | | Purpose |
| --- | --- | --- |
| `get_document_info` | R I | Package overview, properties, and content statistics |
| `get_document_model` | R I | The semantic `exyokioffice-document` JSON model |
| `get_document_markdown` | R I | Structure-preserving Markdown rendering |
| `get_document_text` | R I | Every readable text block as plain text |
| `search_text` | R I | Find text and report where each hit sits |
| `validate_document` | R I | OPC and schema validation report |
| `query_xml` | R I | XPath query over one XML part (read-only) |
| `get_properties` | R I | Core, extended, and custom properties |
| `list_media` | R I | Media inventory without payloads |
| `get_media` | R I | One image or audio payload as a content block |

Editing:

| Tool | | Purpose |
| --- | --- | --- |
| `replace_text` | M | Replace text throughout the document |
| `set_properties` | M I | Write core, extended, and custom properties |
| `batch` | M | Apply several mutating tools as one transaction |

File utilities — primarily path to path. `redact_document` and `export_media`
also accept `documentId`, so they can operate on unsaved session content:

| Tool | | Purpose |
| --- | --- | --- |
| `list_workspace` | R I | List workspace files matching a glob |
| `convert_document` | M | Convert between the package and Markdown/JSON/text/XML (and CSV on the Excel server) |
| `diff_documents` | R I | Compare two packages part by part |
| `merge_documents` | M | Merge several packages into one |
| `split_document` | M | Split one package into numbered files |
| `redact_document` | D | Remove comments, revisions, hidden text, and identity metadata from a session or file |
| `export_media` | M | Write every media payload into a directory |

### `exyoki-mcp-word`

| Tool | Group | | Purpose |
| --- | --- | --- | --- |
| `get_outline` | content | R I | Headings and bookmarks with their block indices |
| `read_blocks` | content | R I | A window of body blocks, as a model, Markdown, or text |
| `list_styles` | content | R I | The style catalog of the document |
| `insert_paragraph` | content | M | Insert one paragraph at an anchor |
| `insert_list` | content | M | Insert a bulleted or numbered list |
| `edit_paragraph` | content | M | Rewrite one paragraph |
| `delete_blocks` | content | D | Delete a range of body blocks |
| `apply_style` | content | M I | Apply a paragraph style to several blocks |
| `insert_image` | content | M | Insert a picture as its own paragraph |
| `add_bookmark` | content | M | Bookmark a paragraph, or the range from `block` to `end_block` |
| `insert_table` | tables | M | Insert a table, optionally filled |
| `edit_table_cell` | tables | M | Rewrite one table cell |
| `modify_table` | tables | D | Add or delete rows and columns, or merge cells |
| `set_header_footer` | layout | M I | Replace a header or footer, including fields such as PAGE |
| `set_section` | layout | M I | Page size, orientation, and margins |
| `set_tracked_changes` | review | M I | Turn the revision-tracking flag on or off |
| `list_revisions` | review | R I | List the tracked revisions |
| `resolve_revisions` | review | M | Accept or reject revisions |
| `list_comments` | review | R I | List the comments with their author, date, and block range |
| `add_comment` | review | M | Attach a comment to a paragraph or a block range |
| `delete_comment` | review | D | Remove one comment |
| `add_note` | review | M | Add a footnote or endnote |
| `fill_template` | automation | M | Fill MERGEFIELD, bookmark, and repeating-region placeholders in a session or a file |
| `compare_documents` | automation | M | Write a tracked-revision comparison of two files |

`set_tracked_changes` writes the document's tracking flag, which governs
editors that open the file. The tools of this server write content directly and
do not generate revisions themselves; use `compare_documents` when you need
tracked differences.

### `exyoki-mcp-excel`

| Tool | Group | | Purpose |
| --- | --- | --- | --- |
| `list_sheets` | sheets | R I | Worksheets with their used ranges and hidden state |
| `add_sheet` | sheets | M | Add a worksheet |
| `rename_sheet` | sheets | M I | Rename a worksheet |
| `delete_sheet` | sheets | D | Remove a worksheet |
| `read_range` | cells | R I | Read cells as values, records, or CSV |
| `write_cells` | cells | M | Write individually addressed cells; a null value skips the cell |
| `write_range` | cells | M | Write a rectangular block from an origin |
| `clear_range` | cells | D | Clear contents, formats, or both |
| `modify_sheet_structure` | cells | D | Insert or delete rows and columns |
| `set_hyperlink` | cells | M I | Attach or remove a cell hyperlink |
| `recalculate` | cells | M | Recompute formulas, rewrite cached results, and report cells left in error |
| `merge_cells` | formatting | M | Merge or split a range |
| `format_range` | formatting | M I | Number format, font, fill, border, alignment |
| `set_column_width` | formatting | M I | Column widths in character units |
| `set_row_height` | formatting | M I | Row heights in points |
| `freeze_panes` | formatting | M I | Freeze rows and columns |
| `add_table` | analysis | M | Turn a range into a structured table |
| `add_named_range` | analysis | M | Define a workbook or sheet name |
| `add_data_validation` | analysis | M | Constrain what a range accepts |
| `add_conditional_formatting` | analysis | M | Add a conditional formatting rule |
| `add_chart` | analysis | M | Add a chart anchored on the sheet, one series per column or row |
| `add_pivot_table` | analysis | M | Build a pivot report from a source range |

CSV import and export run through `convert_document`, which takes
`csv_separator` and `sheet` on this server.

### `exyoki-mcp-power-point`

| Tool | Group | | Purpose |
| --- | --- | --- | --- |
| `list_slides` | slides | R I | Slides with layout, title, and shape counts |
| `get_slide` | slides | R I | One slide as shapes with their paths, or as model, Markdown, or text |
| `list_layouts` | slides | R I | Layouts and the placeholders each offers |
| `add_slide` | slides | M | Add a slide built from a layout |
| `delete_slide` | slides | D | Remove a slide |
| `move_slide` | slides | M | Move a slide to another position |
| `duplicate_slide` | slides | M | Copy a slide inside the presentation |
| `copy_slide_from` | slides | M | Import a slide from another presentation |
| `set_slide_hidden` | slides | M I | Show or hide a slide in a show |
| `set_placeholder_text` | content | M I | Write text into a layout placeholder |
| `add_text_box` | content | M | Add a free-floating text box |
| `edit_text_frame` | content | M | Rewrite the text of a shape |
| `delete_shape` | content | D | Remove a shape |
| `set_shape_transform` | content | M I | Move, resize, or rotate a shape; rotation is in degrees |
| `add_table` | content | M | Add a table, optionally filled |
| `edit_table_cell` | content | M | Rewrite one table cell |
| `add_chart` | content | M | Add a chart from categories and series |
| `set_notes` | content | M I | Replace the speaker notes |
| `list_comments` | content | R I | List comments, optionally per slide |
| `add_comment` | content | M | Attach a comment to a slide |
| `add_image` | media | M | Place a picture on a slide |
| `set_transition` | design | M I | Set or remove a slide transition |
| `add_section` | design | M | Group slides into a named section |
| `set_slide_size` | design | M I | Slide size from a preset or dimensions |

`add_slide` writes the title and the bullets into real placeholders, not into
plain text boxes, so the layout's formatting applies and the outline view sees
the slide. A presentation created from scratch has no slide master; the first
`add_slide` creates one together with a default layout.

## Published catalogs and schemas

Each server prints its own catalog, and the result is published so a client can
read it without starting a process:

- [`docs/schemas/mcp-word-tools.json`](../schemas/mcp-word-tools.json)
- [`docs/schemas/mcp-excel-tools.json`](../schemas/mcp-excel-tools.json)
- [`docs/schemas/mcp-power-point-tools.json`](../schemas/mcp-power-point-tools.json)

Each file is exactly what `tools/list` returns, plus the `group` and a worked
`example` for every tool. Regenerate one after an intentional change:

```powershell
exyoki-mcp-word --print-tools > docs\schemas\mcp-word-tools.json
```

CTest entries `Mcp.Catalog.word`, `Mcp.Catalog.excel`, and
`Mcp.Catalog.power-point` compare the published files against the live servers,
so a catalog cannot silently go stale. The `example` of every tool is validated
against that tool's own `inputSchema` by the test suite, which keeps the
documentation from lying.

The `get_document_model` payload is the `exyokioffice-document` envelope
described in [Conversion formats](conversion-formats.md).

## Looking up a tool's exact arguments

This page names every tool and says what it is for; it deliberately does not
repeat the argument lists. Those live in one place — each tool's `inputSchema`
— and that place is machine-checked against the running server, so it cannot
drift from the code the way a hand-written table would.

Read it either from the published catalog or from the server itself:

```powershell
# What does insert_paragraph accept?
exyoki-mcp-word --print-tools |
  ConvertFrom-Json |
  Select-Object -ExpandProperty tools |
  Where-Object name -eq insert_paragraph |
  Select-Object -ExpandProperty inputSchema |
  ConvertTo-Json -Depth 6
```

Every entry carries the same members:

| Member | What it gives you |
| --- | --- |
| `description` | What the tool does and when to reach for it, in the words the agent sees. |
| `inputSchema` | Every argument, its type, its default, and which ones are required. |
| `outputSchema` | The shape of `data` in the answer, on top of the shared envelope. |
| `example` | A minimal call that works. Each one is validated against its own `inputSchema` by the test suite. |
| `group` | The `--toolsets` group the tool belongs to. |
| `annotations` | `readOnlyHint`, `destructiveHint`, `idempotentHint`, `openWorldHint`. |

A client does the same thing over the wire with `tools/list`, so an agent never
needs this page to call a tool correctly — it needs it to decide *which* tool.

## Troubleshooting

| Symptom | Cause | Fix |
| --- | --- | --- |
| The client lists no tools | The binary did not start, or the client swallowed the error | Run `exyoki-mcp-word --version` and then `--print-tools` by hand; a Windows path in `.mcp.json` needs `/` or escaped `\\` |
| Tools are listed but every edit fails | The server runs with `--read-only` | Mutating tools are absent from the catalog in that mode; drop the flag |
| Fewer tools than expected | `--toolsets` filtered them out | It filters strictly, `lifecycle` included; `--print-tools` shows what survived |
| `path_outside_workspace` | The path resolves outside every `--workspace` root, after `..` and symbolic links are expanded, or the file system could not canonicalize it at all | Pass a workspace-relative path; `list_workspace` shows what is reachable |
| `path_invalid` | The path has a shape the server refuses on sight: a device name, an alternate data stream, a UNC or `\\?\` prefix, a drive-relative path, or a trailing dot | Pass a plain workspace-relative file name |
| `file_changed_on_disk` on save | Something rewrote the file after the session opened it | Re-open and redo, or pass `force: true` if the on-disk copy is expendable |
| `file_exists` | The destination is taken and `overwrite` defaulted to false. `create_document` checks this when the session opens, not at save time | Pass `overwrite: true` deliberately, or choose another path |
| `document_limit_reached` | More than `--max-documents` sessions are open | `close_document` on what you finished, or raise the limit |
| `snapshot_unavailable` from `undo` | `--snapshot-depth 0`, or the history is exhausted | Raise the depth; at `0` a failed edit is also left unrolled back |
| `family_mismatch` | An `.xlsx` was handed to the Word server, either as a file to open or as a destination to write | Each binary serves one family; register all three, and name destinations with this family's extension |
| `-32002` on a `tools/*` call | The call arrived before `initialize`, or only `notifications/initialized` was sent, which alone negotiates nothing | A conformant client handles this; hand-written replay files must include the whole handshake |
| `-32602` on `initialize` | `protocolVersion`, `capabilities`, or `clientInfo` is missing or malformed | The `data.required` member of the error names what the request must carry |
| `-32600` on `initialize` | The connection was already initialized | Initialization happens once per connection; start a new process to renegotiate |
| `-32602` with "Unknown tool" | Typo, or the tool is not in this family's catalog | Check `--print-tools` |
| The client hangs at startup | Something is writing to standard output | Only the protocol may go there; all diagnostics belong on standard error |
| Nothing is written to disk | `save_document` was never called | Sessions are in memory by design; `list_documents` shows `dirty: true` for unsaved work |

To see what the server is doing, start it with `--log-level debug`. The log
goes to standard error and never touches the protocol stream:

```text
[info] exyoki-mcp-word 1.0.0 ready with 50 tools.
[warn] Discarding an unparsable JSON-RPC line.
[info] Input closed; shutting down.
```

## Limits of this version

Deliberately out of scope, and rejected rather than half-implemented:

- **No HTTP or SSE transport.** Only stdio; the transport is a separate layer
  and can be added without touching any tool.
- **No raw XML writing.** `query_xml` reads; nothing writes markup directly,
  because a hand-written fragment can break the package in ways no validator
  reports until Office refuses the file.
- **No OPC utility tools** (`parts`, `relationships`, `unpack`, `pack`,
  `to-flat-opc`, `dedup`, `signatures`, …). They cost catalog space an agent
  rarely converts into a better document; use [`exyoki`](exyoki.md) for those.
- **No `prompts`, `resources`, `logging`, or progress notifications**, and no
  preview rendering. Because there is no `resources` capability, no answer
  carries an embedded resource either: `get_media` hands back only the `image`
  and `audio` blocks every client understands, and video, OLE objects and
  embedded packages go to a file through `export_media`.
- **Basic charts only.** Categories, series, and the common plot types;
  anything richer answers `unsupported` with a hint.
- **Colour scales and data bars** are not offered by
  `add_conditional_formatting`; use a `cellIs` or `expression` rule.

`validate_document` checks OPC structure and markup schema, which is not the
same as full Microsoft Office compatibility — see
[Compatibility](../Compatibility.md) for what the library supports.
