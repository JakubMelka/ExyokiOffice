from __future__ import annotations

import asyncio
import json
from pathlib import Path

import pytest


class JsonLineProcess:
    def __init__(self, process: asyncio.subprocess.Process):
        self.process = process

    @classmethod
    async def start(cls, executable: Path, workspace: Path):
        process = await asyncio.create_subprocess_exec(
            str(executable), "--workspace", str(workspace),
            stdin=asyncio.subprocess.PIPE,
            stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.PIPE,
            cwd=workspace,
            limit=2 * 1024 * 1024,
        )
        return cls(process)

    async def send(self, message: dict) -> None:
        assert self.process.stdin is not None
        self.process.stdin.write(json.dumps(message, separators=(",", ":")).encode() + b"\n")
        await self.process.stdin.drain()

    async def receive(self) -> dict:
        assert self.process.stdout is not None
        line = await asyncio.wait_for(self.process.stdout.readline(), timeout=5)
        assert line, "server closed stdout without a response"
        return json.loads(line)

    async def request(self, message: dict) -> dict:
        await self.send(message)
        return await self.receive()

    async def close(self) -> None:
        if self.process.stdin is not None:
            self.process.stdin.close()
            await self.process.stdin.wait_closed()
        try:
            await asyncio.wait_for(self.process.wait(), timeout=5)
        except TimeoutError:
            self.process.kill()
            await self.process.wait()
            raise AssertionError("server did not exit after stdin reached EOF")


def initialize_params(**overrides) -> dict:
    params = {
        "protocolVersion": "2025-11-25",
        "capabilities": {},
        "clientInfo": {"name": "raw-conformance-probe", "version": "1"},
    }
    params.update(overrides)
    return params


@pytest.mark.conformance
async def test_initialized_notification_cannot_replace_initialize(server_executables, workspace):
    server = await JsonLineProcess.start(server_executables["word"], workspace)
    try:
        await server.send({"jsonrpc": "2.0", "method": "notifications/initialized"})
        response = await server.request({
            "jsonrpc": "2.0", "id": 1, "method": "tools/list", "params": {},
        })
        assert response["error"]["code"] == -32002

        # The handshake still works after the stray notification.
        initialized = await server.request({
            "jsonrpc": "2.0", "id": 2, "method": "initialize", "params": initialize_params(),
        })
        assert initialized["result"]["protocolVersion"] == "2025-11-25"
        await server.send({"jsonrpc": "2.0", "method": "notifications/initialized"})
        listed = await server.request({
            "jsonrpc": "2.0", "id": 3, "method": "tools/list", "params": {},
        })
        assert listed["result"]["tools"]
    finally:
        await server.close()


@pytest.mark.conformance
@pytest.mark.parametrize("missing", ["protocolVersion", "capabilities", "clientInfo"])
async def test_initialize_rejects_missing_required_members(
    missing, server_executables, workspace
):
    params = initialize_params()
    del params[missing]
    server = await JsonLineProcess.start(server_executables["word"], workspace)
    try:
        response = await server.request({
            "jsonrpc": "2.0", "id": 1, "method": "initialize", "params": params,
        })
        assert response["error"]["code"] == -32602
        assert missing in response["error"]["data"]["required"]

        # A refused handshake leaves the server uninitialized.
        locked = await server.request({
            "jsonrpc": "2.0", "id": 2, "method": "tools/list", "params": {},
        })
        assert locked["error"]["code"] == -32002
    finally:
        await server.close()


@pytest.mark.conformance
@pytest.mark.parametrize(
    "override",
    [
        {"protocolVersion": ""},
        {"protocolVersion": 20251125},
        {"capabilities": []},
        {"clientInfo": {"version": "1"}},
        {"clientInfo": {"name": "raw-conformance-probe"}},
    ],
    ids=["empty-version", "numeric-version", "listed-capabilities", "nameless", "unversioned"],
)
async def test_initialize_rejects_malformed_members(override, server_executables, workspace):
    server = await JsonLineProcess.start(server_executables["word"], workspace)
    try:
        response = await server.request({
            "jsonrpc": "2.0", "id": 1, "method": "initialize",
            "params": initialize_params(**override),
        })
        assert response["error"]["code"] == -32602
    finally:
        await server.close()


@pytest.mark.conformance
async def test_initialize_happens_once_per_connection(server_executables, workspace):
    server = await JsonLineProcess.start(server_executables["word"], workspace)
    try:
        first = await server.request({
            "jsonrpc": "2.0", "id": 1, "method": "initialize", "params": initialize_params(),
        })
        assert first["result"]["protocolVersion"] == "2025-11-25"
        await server.send({"jsonrpc": "2.0", "method": "notifications/initialized"})

        second = await server.request({
            "jsonrpc": "2.0", "id": 2, "method": "initialize",
            "params": initialize_params(protocolVersion="2024-11-05"),
        })
        assert second["error"]["code"] == -32600

        # The refused re-handshake changes nothing; the session keeps working.
        ping = await server.request({"jsonrpc": "2.0", "id": 3, "method": "ping", "params": {}})
        assert ping["result"] == {}
    finally:
        await server.close()


@pytest.mark.conformance
async def test_tools_are_locked_before_initialize(server_executables, workspace):
    server = await JsonLineProcess.start(server_executables["word"], workspace)
    try:
        response = await server.request({
            "jsonrpc": "2.0", "id": 1, "method": "tools/list", "params": {},
        })
        assert response["error"]["code"] == -32002
    finally:
        await server.close()


@pytest.mark.conformance
async def test_complete_initialize_negotiates_requested_version(server_executables, workspace):
    server = await JsonLineProcess.start(server_executables["word"], workspace)
    try:
        response = await server.request({
            "jsonrpc": "2.0", "id": 1, "method": "initialize",
            "params": {
                "protocolVersion": "2025-06-18",
                "capabilities": {},
                "clientInfo": {"name": "raw-conformance-probe", "version": "1"},
            },
        })
        assert response["result"]["protocolVersion"] == "2025-06-18"
        await server.send({"jsonrpc": "2.0", "method": "notifications/initialized"})
        listed = await server.request({
            "jsonrpc": "2.0", "id": 2, "method": "tools/list", "params": {},
        })
        assert listed["result"]["tools"]
    finally:
        await server.close()


@pytest.mark.conformance
async def test_malformed_json_returns_parse_error_and_server_recovers(server_executables, workspace):
    server = await JsonLineProcess.start(server_executables["word"], workspace)
    try:
        assert server.process.stdin is not None
        server.process.stdin.write(b"{not-json\n")
        await server.process.stdin.drain()
        parse_error = await server.receive()
        assert parse_error["error"]["code"] == -32700
        ping = await server.request({"jsonrpc": "2.0", "id": 2, "method": "ping", "params": {}})
        assert ping["result"] == {}
    finally:
        await server.close()


@pytest.mark.conformance
async def test_notification_has_no_response(server_executables, workspace):
    server = await JsonLineProcess.start(server_executables["word"], workspace)
    try:
        await server.send({"jsonrpc": "2.0", "method": "notifications/unknown", "params": {}})
        await server.send({"jsonrpc": "2.0", "id": 7, "method": "ping", "params": {}})
        response = await server.receive()
        assert response["id"] == 7
    finally:
        await server.close()
