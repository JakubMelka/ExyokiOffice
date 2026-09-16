"""Launcher for the ExyokiOffice MCP servers shipped in this wheel.

The servers are native executables under ``exyokioffice_mcp/bin``; the
functions here are the console scripts that start them. On POSIX the launcher
replaces itself with the server, so the MCP host talks to the server process
directly. Windows has no ``exec``, so there the server runs as a child that
inherits standard input and output, and the launcher passes its exit code on.
"""

import os
import subprocess
import sys
from pathlib import Path

_BIN = Path(__file__).resolve().parent / "bin"

# The family names the dispatcher accepts, matching the container image's
# dispatcher so that one set of instructions covers both.
_PROGRAMS = {
    "word": "exyoki-mcp-word",
    "excel": "exyoki-mcp-excel",
    "powerpoint": "exyoki-mcp-power-point",
    "power-point": "exyoki-mcp-power-point",
    "exyoki": "exyoki",
}

_USAGE = """usage: exyokioffice-mcp <word|excel|powerpoint|exyoki> [arguments...]

Start one of the ExyokiOffice MCP servers, or the exyoki command-line tool,
passing the remaining arguments through. Each server takes --workspace DIR,
the only folder it may read and write; --help lists the rest.

  exyokioffice-mcp word --workspace ./documents
  exyokioffice-mcp excel --workspace ./documents --read-only
  exyokioffice-mcp powerpoint --print-tools
  exyokioffice-mcp exyoki validate report.docx
"""


def _executable(program):
    name = program + ".exe" if sys.platform == "win32" else program
    path = _BIN / name
    if not path.is_file():
        sys.stderr.write(
            f"exyokioffice-mcp: {name} is not in this installation ({_BIN}); "
            "the wheel may not match this platform\n"
        )
        return None
    return path


def _run(program, arguments):
    path = _executable(program)
    if path is None:
        return 2
    command = [str(path), *arguments]
    if os.name == "posix":
        os.execv(command[0], command)
    # The host ends a stdio server by closing its input; a Ctrl+C on a console
    # reaches the child as well, which is where it should be handled.
    try:
        import signal

        signal.signal(signal.SIGINT, signal.SIG_IGN)
    except (ValueError, OSError):
        pass
    return subprocess.call(command)


def _version():
    try:
        from importlib.metadata import version

        return version("exyokioffice-mcp")
    except Exception:  # pragma: no cover - only outside an installed wheel
        return "unknown"


def main():
    """The ``exyokioffice-mcp`` dispatcher: family first, then the server's arguments."""
    arguments = sys.argv[1:]
    if not arguments or arguments[0] in ("-h", "--help"):
        sys.stdout.write(_USAGE)
        return 0 if arguments else 2
    if arguments[0] == "--version":
        sys.stdout.write(f"exyokioffice-mcp {_version()}\n")
        return 0
    program = _PROGRAMS.get(arguments[0])
    if program is None:
        sys.stderr.write(
            f"exyokioffice-mcp: unknown family '{arguments[0]}'; "
            "expected word, excel, powerpoint or exyoki\n"
        )
        return 2
    return _run(program, arguments[1:])


def word():
    return _run("exyoki-mcp-word", sys.argv[1:])


def excel():
    return _run("exyoki-mcp-excel", sys.argv[1:])


def power_point():
    return _run("exyoki-mcp-power-point", sys.argv[1:])


def exyoki():
    return _run("exyoki", sys.argv[1:])


if __name__ == "__main__":  # pragma: no cover
    sys.exit(main())
