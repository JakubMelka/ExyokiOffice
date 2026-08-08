from __future__ import annotations

import pytest

from conftest import SERVER_SPECS, open_client


@pytest.mark.conformance
@pytest.mark.parametrize("family", tuple(SERVER_SPECS))
async def test_official_sdk_initializes_and_pings(family, server_executables, workspace):
    async with open_client(server_executables[family], workspace) as client:
        assert client.protocol_version == "2025-11-25"
        assert client.server_info.name == SERVER_SPECS[family].executable_name
        assert client.server_info.version
        assert client.server_capabilities.tools is not None
        assert client.server_capabilities.tools.list_changed is False
        assert client.instructions
        assert "save_document" in client.instructions
        await client.send_ping()


@pytest.mark.conformance
@pytest.mark.parametrize("family", tuple(SERVER_SPECS))
async def test_sdk_clean_shutdown_after_idle_connection(family, server_executables, workspace):
    async with open_client(server_executables[family], workspace) as client:
        await client.send_ping()
    # Exiting Client and stdio_client is the assertion: pytest-timeout catches
    # a server that ignores EOF or leaves the subprocess behind.


@pytest.mark.conformance
@pytest.mark.parametrize("family", tuple(SERVER_SPECS))
async def test_only_tools_capability_is_advertised(family, server_executables, workspace):
    async with open_client(server_executables[family], workspace) as client:
        capabilities = client.server_capabilities.model_dump(by_alias=True, exclude_none=True)
        assert set(capabilities) == {"tools"}

