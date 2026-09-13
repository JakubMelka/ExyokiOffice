"""Writes one document per family through every mutating tool, for the Office oracle.

The Office oracle (tests/office-oracle) opens documents in Microsoft Office,
which is the only check that sees what schema and package validation cannot.
It needs documents to open, and the useful ones are those the servers actually
produce. This script makes them without a hand-written script per tool: each
tool in the published catalog carries an example call that the C++ suite
validates against its own schema, so every mutating session tool is called with
its example, twice, in catalog order. The second pass reaches tools whose
example addresses something an earlier tool creates, such as a slide or a table.

An example that does not fit the document is refused by the server and counted,
never fatal; what matters is that everything that was applied ends up in a file
Office has to accept.

    .venv\\Scripts\\python oracle_corpus.py <output directory>

The servers are found the same way the test suite finds them; set
EXYOKI_MCP_WORD_EXE, EXYOKI_MCP_EXCEL_EXE and EXYOKI_MCP_POWERPOINT_EXE to test
an installed build.
"""

from __future__ import annotations

import asyncio
import base64
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / "tests"))

from conftest import REPOSITORY_ROOT, SERVER_SPECS, open_client, resolve_executable  # noqa: E402


# Lifecycle tools, file-to-file utilities, and tools that end or rewind the
# session. None of them adds content to the document being built.
SKIPPED = {
    "create_document", "open_document", "save_document", "close_document", "undo", "batch",
    "convert_document", "merge_documents", "split_document", "redact_document", "diff_documents",
    "export_media",
}


# A 1x1 PNG, for the examples that insert a picture named logo.png.
LOGO_PNG = base64.b64decode(
    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAEhQGAhKmMIQAAAABJRU5ErkJggg=="
)

# The leading `ftyp` box of an MP4 file. The servers never decode media, so
# the example that embeds intro.mp4 needs only a file of that name.
INTRO_MP4 = bytes.fromhex("0000001866747970697736d000000000697736d06d703432")


def write_fixtures(output: Path) -> None:
    """Writes the input files the catalog examples name.

    No macros.bin is written: a VBA project Excel accepts has to come from
    Excel, and the VBA path is tested for fidelity rather than acceptance.
    """
    (output / "logo.png").write_bytes(LOGO_PNG)
    (output / "intro.mp4").write_bytes(INTRO_MP4)


async def write_template(client, output: Path) -> None:
    """Writes template.pptx, the source copy_slide_from's example copies from."""
    created = await client.call_tool("create_document", {"path": "template.pptx", "overwrite": True})
    document = created.structured_content["data"]["documentId"]
    for _ in range(3):
        await client.call_tool("add_slide", {"documentId": document})
    await client.call_tool("save_document", {"documentId": document})
    await client.call_tool("close_document", {"documentId": document})


async def build(family: str, output: Path) -> dict:
    spec = SERVER_SPECS[family]
    catalog = json.loads((REPOSITORY_ROOT / "docs/schemas" / spec.catalog_name).read_text(encoding="utf-8"))
    applied: set[str] = set()
    refused: dict[str, str] = {}
    target = f"tour-{family}{spec.extension}"

    async with open_client(resolve_executable(spec), output) as client:
        if family == "powerpoint":
            await write_template(client, output)

        created = await client.call_tool("create_document", {"path": target, "overwrite": True})
        document = created.structured_content["data"]["documentId"]
        if family == "powerpoint":
            # The examples address slides up to the third; a deck starts empty.
            for _ in range(3):
                await client.call_tool("add_slide", {"documentId": document})

        for _ in range(2):
            for tool in catalog["tools"]:
                name = tool["name"]
                annotations = tool["annotations"]
                required = tool["inputSchema"].get("required", [])
                if (name in SKIPPED or name in applied or annotations["readOnlyHint"]
                        or annotations["destructiveHint"] or "documentId" not in required):
                    continue

                arguments = dict(tool.get("example", {}))
                arguments["documentId"] = document
                result = await client.call_tool(name, arguments)
                if result.is_error:
                    refused[name] = result.structured_content["error"]["message"]
                else:
                    applied.add(name)
                    refused.pop(name, None)

        saved = await client.call_tool("save_document", {"documentId": document})
        if saved.is_error:
            raise RuntimeError(f"{family}: {saved.structured_content['error']['message']}")

        validated = await client.call_tool("validate_document", {"path": target})
        errors = validated.structured_content.get("data", {}).get("errorCount")

    return {"family": family, "file": target, "applied": sorted(applied), "refused": refused,
            "validationErrors": errors}


async def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__)
        return 2

    output = Path(sys.argv[1]).resolve()
    output.mkdir(parents=True, exist_ok=True)
    write_fixtures(output)
    reports = [await build(family, output) for family in SERVER_SPECS]
    (output / "corpus.json").write_text(json.dumps(reports, indent=2), encoding="utf-8")

    failed = False
    for report in reports:
        print(f"{report['file']}: {len(report['applied'])} tools applied, "
              f"{len(report['refused'])} examples refused, validation errors: {report['validationErrors']}")
        failed = failed or report["validationErrors"] != 0
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
