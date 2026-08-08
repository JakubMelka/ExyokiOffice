from __future__ import annotations

import json
import re

import pytest
from jsonschema import Draft202012Validator
from jsonschema.validators import validator_for

from conftest import SERVER_SPECS, open_client


TOOL_NAME = re.compile(r"^[A-Za-z0-9_.-]{1,128}$")
SHARED_TOOLS = {
    "create_document", "open_document", "save_document", "close_document",
    "list_documents", "undo", "get_document_info", "get_document_model",
    "get_document_markdown", "get_document_text", "search_text",
    "validate_document", "query_xml", "get_properties", "list_media",
    "get_media", "replace_text", "set_properties", "batch",
    "list_workspace", "convert_document", "diff_documents", "merge_documents",
    "split_document", "redact_document", "export_media",
}


def validate_schema(schema: dict) -> None:
    validator_type = validator_for(schema, default=Draft202012Validator)
    validator_type.check_schema(schema)


@pytest.mark.catalog
@pytest.mark.parametrize("family", tuple(SERVER_SPECS))
async def test_complete_catalog_contract(family, server_executables, workspace):
    spec = SERVER_SPECS[family]
    async with open_client(server_executables[family], workspace) as client:
        listed = await client.list_tools()
        tools = listed.tools
        assert listed.next_cursor is None
        assert len(tools) == spec.expected_tool_count

        names = [tool.name for tool in tools]
        assert len(names) == len(set(names))
        assert SHARED_TOOLS <= set(names)
        assert spec.identity_tool in names

        for tool in tools:
            assert TOOL_NAME.fullmatch(tool.name), tool.name
            assert tool.title and tool.title.strip(), tool.name
            assert tool.description and len(tool.description.strip()) >= 20, tool.name
            assert tool.input_schema["type"] == "object", tool.name
            assert tool.input_schema.get("additionalProperties") is False, tool.name
            assert tool.output_schema is not None, tool.name
            assert tool.output_schema["type"] == "object", tool.name
            validate_schema(tool.input_schema)
            validate_schema(tool.output_schema)

            annotations = tool.annotations
            assert annotations is not None, tool.name
            assert annotations.read_only_hint is not None, tool.name
            assert annotations.destructive_hint is not None, tool.name
            assert annotations.idempotent_hint is not None, tool.name
            assert annotations.open_world_hint is False, tool.name


@pytest.mark.catalog
@pytest.mark.parametrize("family", tuple(SERVER_SPECS))
async def test_catalog_is_stable_during_session(family, server_executables, workspace):
    async with open_client(server_executables[family], workspace) as client:
        first = await client.list_tools()
        second = await client.list_tools()
        assert [item.model_dump() for item in first.tools] == [item.model_dump() for item in second.tools]


@pytest.mark.catalog
@pytest.mark.parametrize("family", tuple(SERVER_SPECS))
async def test_read_only_mode_publishes_only_read_only_tools(family, server_executables, workspace):
    async with open_client(server_executables[family], workspace, "--read-only") as client:
        tools = (await client.list_tools()).tools
        assert tools
        assert all(tool.annotations.read_only_hint for tool in tools)
        names = {tool.name for tool in tools}
        assert "get_document_info" in names
        assert "create_document" not in names
        assert SERVER_SPECS[family].identity_tool not in names


@pytest.mark.catalog
async def test_toolset_filter_is_reflected_in_sdk_catalog(server_executables, workspace):
    async with open_client(
        server_executables["word"], workspace, "--toolsets", "lifecycle"
    ) as client:
        names = {tool.name for tool in (await client.list_tools()).tools}
        assert names == {
            "create_document", "open_document", "save_document",
            "close_document", "list_documents", "undo",
        }


@pytest.mark.catalog
@pytest.mark.parametrize("family", tuple(SERVER_SPECS))
async def test_every_tool_rejects_unknown_property(family, server_executables, workspace):
    async with open_client(server_executables[family], workspace) as client:
        for tool in (await client.list_tools()).tools:
            result = await client.call_tool(tool.name, {"__unknown_sdk_probe__": True})
            assert result.is_error is True, tool.name
            payload = result.structured_content
            assert payload["error"]["code"] == "input_invalid", (tool.name, payload)
            assert payload["error"]["details"], tool.name
            Draft202012Validator(tool.output_schema).validate(payload)
            assert result.content and result.content[0].type == "text", tool.name
            assert json.loads(result.content[0].text) == payload, tool.name
