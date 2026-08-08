from __future__ import annotations

import os

import pytest

from conftest import SERVER_SPECS, call_error, call_ok, open_client


@pytest.mark.security
@pytest.mark.parametrize("family", tuple(SERVER_SPECS))
async def test_parent_traversal_is_rejected(family, server_executables, workspace):
    spec = SERVER_SPECS[family]
    async with open_client(server_executables[family], workspace) as client:
        payload = await call_error(
            client, "create_document", {"path": f"../escape{spec.extension}"},
            code="path_outside_workspace",
        )
        assert payload["error"]["hint"]
    assert not (workspace.parent / f"escape{spec.extension}").exists()


@pytest.mark.security
@pytest.mark.parametrize("family", tuple(SERVER_SPECS))
async def test_absolute_path_outside_workspace_is_rejected(family, server_executables, workspace):
    spec = SERVER_SPECS[family]
    outside = workspace.parent / f"outside-{os.getpid()}{spec.extension}"
    async with open_client(server_executables[family], workspace) as client:
        await call_error(client, "create_document", {"path": str(outside)}, code="path_outside_workspace")
    assert not outside.exists()


@pytest.mark.security
@pytest.mark.skipif(os.name != "nt", reason="device names and streams are Windows path forms")
@pytest.mark.parametrize("family", tuple(SERVER_SPECS))
@pytest.mark.parametrize("path", ["NUL", "report.docx:hidden", r"\\?\C:\escape.docx"])
async def test_windows_path_forms_are_refused(path, family, server_executables, workspace):
    # None of these leave the workspace, so they get their own code: writing to
    # NUL would report success and store nothing, and a stream suffix hides the
    # bytes in a file the agent sees as ordinary.
    async with open_client(server_executables[family], workspace) as client:
        payload = await call_error(client, "create_document", {"path": path}, code="path_invalid")
        assert payload["error"]["hint"]


@pytest.mark.security
@pytest.mark.parametrize("family", tuple(SERVER_SPECS))
async def test_split_prefix_cannot_leave_the_output_directory(family, server_executables, workspace):
    spec = SERVER_SPECS[family]
    source = f"source{spec.extension}"
    async with open_client(server_executables[family], workspace) as client:
        created = await call_ok(client, "create_document", {"path": source})
        await call_ok(client, "save_document", {"documentId": created["data"]["documentId"]})
        await call_error(
            client, "split_document",
            {"input_path": source, "output_dir": "parts", "prefix": "../../evil"},
            code="input_invalid",
        )
    assert not list(workspace.parent.glob("evil*"))


@pytest.mark.security
@pytest.mark.parametrize("family", tuple(SERVER_SPECS))
async def test_dirty_close_requires_explicit_discard(family, server_executables, workspace):
    spec = SERVER_SPECS[family]
    async with open_client(server_executables[family], workspace) as client:
        created = await call_ok(client, "create_document", {})
        document_id = created["data"]["documentId"]
        if family == "word":
            await call_ok(client, "insert_paragraph", {
                "documentId": document_id, "anchor": {"position": "end"}, "text": "dirty"})
        elif family == "excel":
            await call_ok(client, "write_range", {
                "documentId": document_id, "origin": "A1", "values": [["dirty"]]})
        else:
            await call_ok(client, "add_slide", {"documentId": document_id, "title": "dirty"})
        failure = await call_error(
            client, "close_document", {"documentId": document_id}, code="operation_failed"
        )
        assert "unsaved changes" in failure["error"]["message"].lower()
        documents = await call_ok(client, "list_documents", {})
        assert any(item["documentId"] == document_id for item in documents["data"]["documents"])
        await call_ok(client, "close_document", {"documentId": document_id, "discard": True})


@pytest.mark.security
async def test_failed_batch_rolls_back_all_changes(server_executables, workspace):
    async with open_client(server_executables["word"], workspace) as client:
        created = await call_ok(client, "create_document", {})
        document_id = created["data"]["documentId"]
        failure = await call_error(client, "batch", {
            "documentId": document_id,
            "operations": [
                {"tool": "insert_paragraph", "arguments": {
                    "anchor": {"position": "end"}, "text": "must disappear"}},
                {"tool": "apply_style", "arguments": {
                    "blocks": [1], "style_id": "DefinitelyMissingStyle"}},
            ],
        }, code="batch_aborted")
        assert failure["error"]["details"][0]["restored"] is True
        blocks = await call_ok(client, "read_blocks", {"documentId": document_id})
        assert blocks["data"]["blockCount"] == 0


@pytest.mark.security
@pytest.mark.parametrize("family", tuple(SERVER_SPECS))
async def test_wrong_extension_or_family_is_rejected(family, server_executables, workspace):
    wrong = {"word": "wrong.xlsx", "excel": "wrong.pptx", "powerpoint": "wrong.docx"}[family]
    async with open_client(server_executables[family], workspace) as client:
        payload = await call_error(client, "create_document", {"path": wrong})
        assert payload["error"]["code"] in {"family_mismatch", "input_invalid"}
        assert payload["error"]["hint"]
    assert not (workspace / wrong).exists()


@pytest.mark.security
async def test_invalid_schema_reports_json_pointer(server_executables, workspace):
    async with open_client(server_executables["word"], workspace) as client:
        payload = await call_error(client, "open_document", {"path": 42}, code="input_invalid")
        assert payload["error"]["details"]
        assert "pointer" in payload["error"]["details"][0]


@pytest.mark.security
async def test_configured_document_limit_is_enforced_without_losing_sessions(
    server_executables, workspace
):
    async with open_client(
        server_executables["word"], workspace, "--max-documents", "2"
    ) as client:
        first = await call_ok(client, "create_document", {})
        second = await call_ok(client, "create_document", {})
        await call_error(client, "create_document", {}, code="document_limit_reached")
        listed = await call_ok(client, "list_documents", {})
        assert {item["documentId"] for item in listed["data"]["documents"]} == {
            first["data"]["documentId"], second["data"]["documentId"]
        }


@pytest.mark.security
async def test_save_as_refuses_overwrite_without_explicit_permission(server_executables, workspace):
    destination = workspace / "occupied.docx"
    destination.write_bytes(b"do not replace")
    async with open_client(server_executables["word"], workspace) as client:
        created = await call_ok(client, "create_document", {})
        document_id = created["data"]["documentId"]
        await call_error(
            client,
            "save_document",
            {"documentId": document_id, "path": destination.name},
            code="file_exists",
        )
        assert destination.read_bytes() == b"do not replace"


@pytest.mark.security
async def test_existing_file_change_is_detected_before_save(server_executables, workspace):
    async with open_client(server_executables["word"], workspace) as client:
        created = await call_ok(client, "create_document", {"path": "changed.docx"})
        document_id = created["data"]["documentId"]
        await call_ok(client, "save_document", {"documentId": document_id})
        await call_ok(client, "close_document", {"documentId": document_id})

        opened = await call_ok(client, "open_document", {"path": "changed.docx"})
        opened_id = opened["data"]["documentId"]
        await call_ok(client, "insert_paragraph", {
            "documentId": opened_id,
            "anchor": {"position": "end"},
            "text": "unsaved server change",
        })
        original = (workspace / "changed.docx").read_bytes()
        (workspace / "changed.docx").write_bytes(original + b"external change")
        await call_error(
            client, "save_document", {"documentId": opened_id}, code="file_changed_on_disk"
        )
        assert (workspace / "changed.docx").read_bytes().endswith(b"external change")
