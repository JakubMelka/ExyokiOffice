from __future__ import annotations

import contextlib
import os
import shutil
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import AsyncIterator, Iterator

import pytest
from mcp import Client
from mcp.client.stdio import StdioServerParameters, stdio_client


TEST_ROOT = Path(__file__).resolve().parents[1]
REPOSITORY_ROOT = TEST_ROOT.parents[1]


@dataclass(frozen=True)
class ServerSpec:
    family: str
    executable_name: str
    environment_variable: str
    extension: str
    expected_tool_count: int
    identity_tool: str


SERVER_SPECS = {
    "word": ServerSpec(
        "word", "exyoki-mcp-word", "EXYOKI_MCP_WORD_EXE", ".docx", 50, "insert_paragraph"
    ),
    "excel": ServerSpec(
        "excel", "exyoki-mcp-excel", "EXYOKI_MCP_EXCEL_EXE", ".xlsx", 48, "write_range"
    ),
    "powerpoint": ServerSpec(
        "powerpoint",
        "exyoki-mcp-power-point",
        "EXYOKI_MCP_POWERPOINT_EXE",
        ".pptx",
        50,
        "add_slide",
    ),
}


def pytest_addoption(parser: pytest.Parser) -> None:
    group = parser.getgroup("ExyokiOffice MCP")
    group.addoption(
        "--keep-workspaces",
        action="store_true",
        help="Retain per-test MCP workspaces for manual inspection.",
    )
    group.addoption(
        "--artifact-root",
        type=Path,
        default=TEST_ROOT / "artifacts",
        help="Directory used with --keep-workspaces.",
    )


def _candidate_directories() -> list[Path]:
    return [
        REPOSITORY_ROOT / "build/vs/tools/mcp/Debug",
        REPOSITORY_ROOT / "build/vs/tools/mcp/RelWithDebInfo",
        REPOSITORY_ROOT / "build/vs/tools/mcp/Release",
        REPOSITORY_ROOT / "build/ninja-debug/tools/mcp",
        REPOSITORY_ROOT / "build/ninja-release/tools/mcp",
        REPOSITORY_ROOT / "build/ninja-clang-debug/tools/mcp",
        REPOSITORY_ROOT / "build/ninja-clang-release/tools/mcp",
        REPOSITORY_ROOT / "build/linux-ninja-debug/tools/mcp",
        REPOSITORY_ROOT / "build/linux-ninja-release/tools/mcp",
    ]


def resolve_executable(spec: ServerSpec) -> Path:
    configured = os.environ.get(spec.environment_variable)
    if configured:
        path = Path(configured).expanduser().resolve()
        if not path.is_file():
            raise pytest.UsageError(f"{spec.environment_variable} does not name a file: {path}")
        return path

    suffixes = [".exe", ""] if os.name == "nt" else ["", ".exe"]
    for directory in _candidate_directories():
        for suffix in suffixes:
            path = directory / f"{spec.executable_name}{suffix}"
            if path.is_file():
                return path.resolve()

    search = ", ".join(str(path) for path in _candidate_directories())
    raise pytest.UsageError(
        f"Cannot find {spec.executable_name}. Build the MCP targets, set "
        f"{spec.environment_variable}, or place it under one of: {search}"
    )


@pytest.fixture(scope="session", params=tuple(SERVER_SPECS))
def server_spec(request: pytest.FixtureRequest) -> ServerSpec:
    return SERVER_SPECS[request.param]


@pytest.fixture(scope="session")
def server_executables() -> dict[str, Path]:
    return {family: resolve_executable(spec) for family, spec in SERVER_SPECS.items()}


@pytest.fixture
def workspace(request: pytest.FixtureRequest) -> Iterator[Path]:
    keep = request.config.getoption("--keep-workspaces")
    if keep:
        artifact_root = request.config.getoption("--artifact-root").resolve()
        artifact_root.mkdir(parents=True, exist_ok=True)
        safe_name = request.node.nodeid.replace("/", "_").replace("\\", "_").replace("::", "__")
        path = Path(tempfile.mkdtemp(prefix=f"{safe_name[:80]}-", dir=artifact_root))
        yield path
        return

    path = Path(tempfile.mkdtemp(prefix="exyoki-mcp-python-"))
    try:
        yield path
    finally:
        shutil.rmtree(path, ignore_errors=True)


@contextlib.asynccontextmanager
async def open_client(
    executable: Path,
    workspace: Path,
    *extra_args: str,
    timeout_seconds: float = 20,
) -> AsyncIterator[Client]:
    parameters = StdioServerParameters(
        command=str(executable),
        args=["--workspace", str(workspace), *extra_args],
        cwd=str(workspace),
        env=dict(os.environ),
        encoding="utf-8",
        encoding_error_handler="strict",
    )
    async with Client(stdio_client(parameters), read_timeout_seconds=timeout_seconds) as client:
        yield client


def structured(result) -> dict:
    assert result.structured_content is not None, "tool result has no structuredContent"
    assert isinstance(result.structured_content, dict)
    return result.structured_content


async def call_ok(client: Client, name: str, arguments: dict | None = None) -> dict:
    result = await client.call_tool(name, arguments or {})
    payload = structured(result)
    assert result.is_error is False, payload
    assert payload.get("ok") is True, payload
    return payload


async def call_error(
    client: Client, name: str, arguments: dict | None = None, *, code: str | None = None
) -> dict:
    result = await client.call_tool(name, arguments or {})
    payload = structured(result)
    assert result.is_error is True, payload
    assert payload.get("ok") is False, payload
    if code is not None:
        assert payload["error"]["code"] == code, payload
    return payload

