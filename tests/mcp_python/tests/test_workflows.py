from __future__ import annotations

from pathlib import Path

import pytest

from conftest import call_ok, open_client


@pytest.mark.workflow
async def test_word_create_edit_verify_save_and_reopen(server_executables, workspace):
    output = "sdk-word.docx"
    async with open_client(server_executables["word"], workspace) as client:
        created = await call_ok(client, "create_document", {"path": output})
        document_id = created["data"]["documentId"]
        assert created["revision"] == 0
        assert created["dirty"] is False

        heading = await call_ok(client, "insert_paragraph", {
            "documentId": document_id,
            "anchor": {"position": "end"},
            "text": "Python SDK interoperability report",
            "heading_level": 1,
        })
        assert heading["revision"] == 1
        assert heading["dirty"] is True

        await call_ok(client, "insert_paragraph", {
            "documentId": document_id,
            "anchor": {"position": "end"},
            "text": "Created through the official MCP Python SDK.",
        })
        await call_ok(client, "insert_table", {
            "documentId": document_id,
            "anchor": {"position": "end"},
            "rows": 3,
            "cols": 2,
            "header_row": True,
            "data": [["Check", "Result"], ["Handshake", "Passed"], ["Save", "Pending"]],
        })

        outline = await call_ok(client, "get_outline", {"documentId": document_id})
        assert outline["data"]["headings"][0]["text"] == "Python SDK interoperability report"

        markdown = await call_ok(client, "get_document_markdown", {"documentId": document_id})
        assert "Python SDK interoperability report" in markdown["data"]["markdown"]
        assert "Handshake" in markdown["data"]["markdown"]

        validation = await call_ok(client, "validate_document", {"documentId": document_id})
        assert validation["data"]["errorCount"] == 0
        saved = await call_ok(client, "save_document", {"documentId": document_id})
        assert saved["dirty"] is False
        assert saved["data"]["bytesWritten"] > 0
        await call_ok(client, "close_document", {"documentId": document_id})

        reopened = await call_ok(client, "open_document", {"path": output})
        reopened_id = reopened["data"]["documentId"]
        text = await call_ok(client, "get_document_text", {"documentId": reopened_id})
        assert "Created through the official MCP Python SDK." in text["data"]["text"]
        await call_ok(client, "close_document", {"documentId": reopened_id})

    assert (workspace / output).is_file()
    assert (workspace / output).stat().st_size > 0


@pytest.mark.workflow
async def test_excel_create_formula_format_recalculate_save_and_reopen(server_executables, workspace):
    output = "sdk-excel.xlsx"
    async with open_client(server_executables["excel"], workspace) as client:
        created = await call_ok(client, "create_document", {"path": output})
        document_id = created["data"]["documentId"]
        await call_ok(client, "write_range", {
            "documentId": document_id,
            "origin": "A1",
            "values": [
                ["Region", "Revenue"],
                ["North", 1200],
                ["South", 900],
                ["Total", {"formula": "SUM(B2:B3)"}],
            ],
        })
        await call_ok(client, "format_range", {
            "documentId": document_id,
            "range": "A1:B1",
            "font": {"bold": True, "color": "#FFFFFF"},
            "fill": {"color": "#1F4E78"},
        })
        await call_ok(client, "freeze_panes", {
            "documentId": document_id, "cell": "A2",
        })
        recalculated = await call_ok(client, "recalculate", {"documentId": document_id})
        assert recalculated["data"]["recalculatedCells"] >= 1
        assert recalculated["data"]["formulaErrors"] == []

        values = await call_ok(client, "read_range", {
            "documentId": document_id, "range": "A1:B4",
        })
        assert values["data"]["values"][0] == ["Region", "Revenue"]
        assert str(values["data"]["values"][3][1]) in {"2100", "2100.0"}

        validation = await call_ok(client, "validate_document", {"documentId": document_id})
        assert validation["data"]["errorCount"] == 0
        await call_ok(client, "save_document", {"documentId": document_id})
        await call_ok(client, "close_document", {"documentId": document_id})

        reopened = await call_ok(client, "open_document", {"path": output})
        reopened_id = reopened["data"]["documentId"]
        reread = await call_ok(client, "read_range", {
            "documentId": reopened_id, "range": "A1:B4",
        })
        assert reread["data"]["values"][1][0] == "North"
        await call_ok(client, "close_document", {"documentId": reopened_id})

    assert (workspace / output).stat().st_size > 0


@pytest.mark.workflow
async def test_powerpoint_create_content_notes_save_and_reopen(server_executables, workspace):
    output = "sdk-powerpoint.pptx"
    async with open_client(server_executables["powerpoint"], workspace) as client:
        created = await call_ok(client, "create_document", {"path": output})
        document_id = created["data"]["documentId"]
        await call_ok(client, "set_slide_size", {"documentId": document_id, "preset": "16:9"})
        added = await call_ok(client, "add_slide", {
            "documentId": document_id,
            "title": "Python SDK interoperability",
            "bullets": ["Official SDK client", "Typed tool result", "Round-trip save"],
        })
        assert added["data"]["slide"] == 1
        await call_ok(client, "set_notes", {
            "documentId": document_id,
            "slide": 1,
            "text": "Speaker notes written through MCP.",
        })
        await call_ok(client, "add_text_box", {
            "documentId": document_id,
            "slide": 1,
            "text": "Black-box test",
            "x": "1in", "y": "5in", "width": "3in", "height": "0.5in",
        })

        listed = await call_ok(client, "list_slides", {"documentId": document_id})
        assert listed["data"]["slides"][0]["title"] == "Python SDK interoperability"
        slide = await call_ok(client, "get_slide", {
            "documentId": document_id, "slide": 1, "format": "text",
        })
        assert "Python SDK interoperability" in str(slide["data"])

        validation = await call_ok(client, "validate_document", {"documentId": document_id})
        assert validation["data"]["errorCount"] == 0
        await call_ok(client, "save_document", {"documentId": document_id})
        await call_ok(client, "close_document", {"documentId": document_id})

        reopened = await call_ok(client, "open_document", {"path": output})
        reopened_id = reopened["data"]["documentId"]
        text = await call_ok(client, "get_document_text", {"documentId": reopened_id})
        assert "Official SDK client" in text["data"]["text"]
        await call_ok(client, "close_document", {"documentId": reopened_id})

    assert (workspace / output).stat().st_size > 0


@pytest.mark.workflow
async def test_word_batch_is_one_revision_and_one_undo_step(server_executables, workspace):
    async with open_client(server_executables["word"], workspace) as client:
        created = await call_ok(client, "create_document", {})
        document_id = created["data"]["documentId"]
        batch = await call_ok(client, "batch", {
            "documentId": document_id,
            "operations": [
                {"tool": "insert_paragraph", "arguments": {
                    "anchor": {"position": "end"}, "text": "First"}},
                {"tool": "insert_paragraph", "arguments": {
                    "anchor": {"position": "end"}, "text": "Second"}},
            ],
        })
        assert batch["revision"] == 1
        assert batch["data"]["applied"] == 2
        before = await call_ok(client, "read_blocks", {"documentId": document_id})
        assert before["data"]["blockCount"] == 2
        undone = await call_ok(client, "undo", {"documentId": document_id})
        assert undone["data"]["restoredRevision"] == 0
        after = await call_ok(client, "read_blocks", {"documentId": document_id})
        assert after["data"]["blockCount"] == 0
