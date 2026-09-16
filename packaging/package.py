#!/usr/bin/env python3
"""Build the distributable forms of the ExyokiOffice MCP servers.

Everything starts from an install tree, the directory ``cmake --install``
writes (``bin/``, ``lib/``, ``share/doc/ExyokiOffice/``), which is also what
the create_install workflow zips. From it this script produces:

  wheel        the exyokioffice-mcp platform wheel for PyPI
  mcpb         one MCP bundle per server for Claude Desktop
  server-json  the MCP Registry manifests, one per server, from finished bundles

``manifest`` prints a bundle manifest for inspection and ``names`` lists the
servers with their registry names, for shell scripts. Everything the outputs
say about the servers comes from packaging/servers.json, the tool catalogs
under docs/schemas and VERSION.txt; nothing is spelled out here twice.

The script needs only the standard library. Building the wheel additionally
needs hatchling, either importable (used in process) or reachable through
``python -m build``.

    python packaging/package.py wheel       --install-dir build/install --output build/packages
    python packaging/package.py mcpb        --install-dir build/install --output build/packages
    python packaging/package.py server-json --output build/packages/registry build/packages/*.mcpb

See docs/tools/mcp-packages.md.
"""

import argparse
import hashlib
import json
import os
import platform
import re
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
PACKAGING_DIR = REPO_ROOT / "packaging"
PYTHON_PROJECT = PACKAGING_DIR / "python"
PYTHON_PACKAGE = "exyokioffice_mcp"
ICON_FILE = REPO_ROOT / "logo_symbol.png"
ICON_LIMIT = 512 * 1024

MANIFEST_VERSION = "0.3"
REGISTRY_SCHEMA = "https://static.modelcontextprotocol.io/schemas/2025-12-11/server.schema.json"
REGISTRY_DESCRIPTION_LIMIT = 100

LIBRARY_STEM = "ExyokiOffice"
DOC_DIR = Path("share/doc/ExyokiOffice")
LICENSE_FILES = ("LICENSE", "THIRD-PARTY-LICENSES.md")

MCPB_NAME = re.compile(
    r"^(?P<executable>exyoki-mcp-[a-z-]+)-(?P<version>\d+\.\d+\.\d+)"
    r"-(?P<platform>win32|linux|darwin)-(?P<arch>[a-z0-9_]+)\.mcpb$"
)


class PackagingError(Exception):
    pass


def read_version():
    text = (REPO_ROOT / "VERSION.txt").read_text(encoding="utf-8").splitlines()[0].strip()
    if not re.fullmatch(r"\d+\.\d+\.\d+", text):
        raise PackagingError(f"VERSION.txt must contain a MAJOR.MINOR.PATCH number, got '{text}'")
    return text


def abi_version(version):
    return ".".join(version.split(".")[:2])


def load_servers():
    with open(PACKAGING_DIR / "servers.json", encoding="utf-8") as handle:
        data = json.load(handle)
    for server in data["servers"]:
        server["registryName"] = f"{data['registryNamespace']}/{server['executable']}"
    return data


def select_servers(data, shorts):
    if not shorts:
        return list(data["servers"])
    by_short = {server["short"]: server for server in data["servers"]}
    chosen = []
    for short in shorts:
        if short not in by_short:
            raise PackagingError(f"unknown server '{short}'; expected one of {', '.join(by_short)}")
        chosen.append(by_short[short])
    return chosen


def load_catalog(server):
    with open(REPO_ROOT / server["catalog"], encoding="utf-8") as handle:
        catalog = json.load(handle)
    if catalog.get("server") != server["executable"]:
        raise PackagingError(
            f"{server['catalog']} describes {catalog.get('server')}, not {server['executable']}"
        )
    return catalog


def sha256_of(path):
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


class InstallTree:
    """A ``cmake --install`` prefix and the platform its binaries are for."""

    def __init__(self, root, version):
        self.root = Path(root).resolve()
        self.version = version
        if not (self.root / "bin").is_dir():
            raise PackagingError(f"{self.root} has no bin/ directory; is it an install prefix?")
        if (self.root / "bin" / "exyoki.exe").is_file():
            self.platform = "win32"
        elif (self.root / "bin" / "exyoki").is_file():
            self.platform = "darwin" if list(self.lib_dir().glob(f"lib{LIBRARY_STEM}*.dylib")) else "linux"
        else:
            raise PackagingError(f"{self.root}/bin holds neither exyoki.exe nor exyoki")

    def lib_dir(self):
        for name in ("lib", "lib64"):
            if (self.root / name).is_dir():
                return self.root / name
        return self.root / "lib"

    def executable_name(self, program):
        return program + ".exe" if self.platform == "win32" else program

    def executable(self, program):
        path = self.root / "bin" / self.executable_name(program)
        if not path.is_file():
            raise PackagingError(f"{path} is missing from the install tree")
        return path

    def library_files(self):
        """The shared library as (source, destination) pairs, destination relative to bin/'s parent."""
        abi = abi_version(self.version)
        if self.platform == "win32":
            source = self.root / "bin" / f"{LIBRARY_STEM}.dll"
            destination = f"bin/{LIBRARY_STEM}.dll"
        elif self.platform == "darwin":
            source = self.lib_dir() / f"lib{LIBRARY_STEM}.{abi}.dylib"
            destination = f"lib/lib{LIBRARY_STEM}.{abi}.dylib"
        else:
            # The executables ask the loader for the soname and find the
            # library through $ORIGIN/../lib. Only the soname is shipped, as a
            # real file: a zip carries no symbolic links a wheel installer or a
            # bundle loader would honour.
            source = self.lib_dir() / f"lib{LIBRARY_STEM}.so.{abi}"
            destination = f"lib/lib{LIBRARY_STEM}.so.{abi}"
        if not source.exists():
            raise PackagingError(f"{source} is missing from the install tree")
        return [(source.resolve(), destination)]

    def license_files(self):
        pairs = []
        doc = self.root / DOC_DIR
        for name in LICENSE_FILES:
            path = doc / name
            if not path.is_file():
                raise PackagingError(f"{path} is missing; a binary distribution has to carry it")
            pairs.append((path, name))
        notices = sorted((doc / "licenses").glob("*"))
        if not notices:
            raise PackagingError(f"{doc / 'licenses'} holds no third-party notices")
        for path in notices:
            pairs.append((path, f"licenses/{path.name}"))
        return pairs

    def runtime_files(self, programs):
        """Everything one or more programs need at run time: executables, library, notices."""
        pairs = [(self.executable(program), f"bin/{self.executable_name(program)}") for program in programs]
        pairs.extend(self.library_files())
        pairs.extend(self.license_files())
        return pairs


def is_executable_destination(destination):
    return destination.startswith("bin/")


# --- wheel ------------------------------------------------------------------


def default_wheel_tag(tree, override):
    if override:
        return override
    if tree.platform == "win32":
        return "py3-none-win_amd64"
    if tree.platform == "linux":
        host = platform.libc_ver()
        if sys.platform != "linux" or host[0] != "glibc" or not host[1]:
            raise PackagingError(
                "the manylinux tag names the glibc the binaries were built against; "
                "run this on the machine that built them, or pass --wheel-tag"
            )
        major, minor = host[1].split(".")[:2]
        machine = platform.machine()
        if machine not in ("x86_64", "aarch64"):
            raise PackagingError(f"no manylinux tag known for {machine}; pass --wheel-tag")
        return f"py3-none-manylinux_{major}_{minor}_{machine}"
    raise PackagingError("pass --wheel-tag for a macOS install tree")


def build_wheel(tree, output, wheel_tag, builder):
    data = load_servers()
    programs = ["exyoki"] + [server["executable"] for server in data["servers"]]
    files = {
        str(source): f"{PYTHON_PACKAGE}/{destination}"
        for source, destination in tree.runtime_files(programs)
    }
    output = Path(output).resolve()
    output.mkdir(parents=True, exist_ok=True)
    tag = default_wheel_tag(tree, wheel_tag)

    with tempfile.TemporaryDirectory() as staging:
        files_path = Path(staging) / "wheel-files.json"
        files_path.write_text(json.dumps(files, indent=2), encoding="utf-8")
        environment = dict(os.environ)
        environment["EXYOKIOFFICE_MCP_VERSION"] = tree.version
        environment["EXYOKIOFFICE_MCP_WHEEL_FILES"] = str(files_path)
        environment["EXYOKIOFFICE_MCP_WHEEL_TAG"] = tag

        if builder == "auto":
            try:
                import hatchling.build  # noqa: F401

                builder = "hatchling"
            except ImportError:
                builder = "build"

        if builder == "hatchling":
            import hatchling.build

            previous = dict(os.environ)
            cwd = os.getcwd()
            try:
                os.environ.update(environment)
                os.chdir(PYTHON_PROJECT)
                name = hatchling.build.build_wheel(str(output))
            finally:
                os.environ.clear()
                os.environ.update(previous)
                os.chdir(cwd)
        else:
            command = [sys.executable, "-m", "build", "--wheel", "--outdir", str(output), str(PYTHON_PROJECT)]
            subprocess.run(command, check=True, env=environment)
            candidates = sorted(output.glob(f"{PYTHON_PACKAGE}-{tree.version}-{tag}.whl"))
            if not candidates:
                raise PackagingError(f"python -m build wrote no {tag} wheel into {output}")
            name = candidates[-1].name

    wheel = output / name
    write_digest(wheel)
    return wheel


# --- MCP bundles ------------------------------------------------------------


def bundle_manifest(server, data, version, platform_name, tools, icon):
    executable = server["executable"] + (".exe" if platform_name == "win32" else "")
    manifest = {
        "manifest_version": MANIFEST_VERSION,
        "name": server["executable"],
        "display_name": server["displayName"],
        "version": version,
        "description": server["description"],
        "long_description": (
            f"{server['displayName']} is one of the three ExyokiOffice MCP servers. It is a native "
            "program with no runtime to install: this bundle carries the executable and the ExyokiOffice "
            "library. Every path in a tool call is resolved inside the folder you choose below and can "
            "never leave it; the server validates each call against a published JSON schema and has no "
            "way to run code. The complete tool catalog, worked sessions and troubleshooting are in the "
            f"[MCP servers guide]({data['documentation']})."
        ),
        "author": dict(data["author"]),
        "repository": {"type": "git", "url": data["repository"]["url"]},
        "homepage": data["repository"]["url"],
        "documentation": data["documentation"],
        "support": data["support"],
        "server": {
            "type": "binary",
            "entry_point": f"bin/{executable}",
            "mcp_config": {
                "command": "${__dirname}/bin/" + executable,
                "args": ["--workspace", "${user_config.workspace}"],
                "env": {},
            },
        },
        "user_config": {
            "workspace": {
                "type": "directory",
                "title": "Document folder",
                "description": data["workspaceDescription"],
                "required": True,
            }
        },
        "tools": [{"name": tool["name"], "description": tool["description"]} for tool in tools],
        "tools_generated": False,
        "keywords": list(server["keywords"]),
        "license": data["license"],
        "compatibility": {"platforms": [platform_name]},
    }
    if icon:
        manifest["icon"] = "icon.png"
    return manifest


def icon_path(explicit):
    if explicit is not None:
        if explicit == "":
            return None
        path = Path(explicit)
        if not path.is_file():
            raise PackagingError(f"icon {path} does not exist")
        return path
    if ICON_FILE.is_file() and ICON_FILE.stat().st_size <= ICON_LIMIT:
        return ICON_FILE
    return None


def add_to_zip(archive, source, destination, executable):
    info = zipfile.ZipInfo.from_file(source, destination)
    info.compress_type = zipfile.ZIP_DEFLATED
    # A bundle is unpacked on macOS and Linux as well as on Windows, and a
    # binary server needs its execute bit there. zipfile records nothing
    # unless told; 0o755 << 16 is the Unix mode field of the external attributes.
    info.external_attr = ((0o755 if executable else 0o644) << 16) | (info.external_attr & 0xFFFF)
    with open(source, "rb") as handle:
        archive.writestr(info, handle.read())


def build_bundles(tree, output, shorts, arch, icon):
    data = load_servers()
    output = Path(output).resolve()
    output.mkdir(parents=True, exist_ok=True)
    icon = icon_path(icon)
    written = []
    for server in select_servers(data, shorts):
        catalog = load_catalog(server)
        manifest = bundle_manifest(server, data, tree.version, tree.platform, catalog["tools"], icon)
        name = f"{server['executable']}-{tree.version}-{tree.platform}-{arch}.mcpb"
        bundle = output / name
        with zipfile.ZipFile(bundle, "w", zipfile.ZIP_DEFLATED) as archive:
            archive.writestr("manifest.json", json.dumps(manifest, indent=2) + "\n")
            for source, destination in tree.runtime_files([server["executable"]]):
                add_to_zip(archive, source, destination, is_executable_destination(destination))
            if icon:
                add_to_zip(archive, icon, "icon.png", False)
        write_digest(bundle)
        written.append(bundle)
    return written


def write_digest(path):
    digest = sha256_of(path)
    (path.parent / (path.name + ".sha256")).write_text(f"{digest}  {path.name}\n", encoding="ascii")
    return digest


# --- registry manifests -----------------------------------------------------


def describe_bundle(path):
    match = MCPB_NAME.match(Path(path).name)
    if not match:
        raise PackagingError(
            f"{Path(path).name} is not named <server>-<version>-<platform>-<arch>.mcpb, "
            "so it cannot be matched to a server"
        )
    return match.groupdict()


def workspace_argument(data):
    return {
        "type": "named",
        "name": "--workspace",
        "description": data["workspaceDescription"],
        "isRequired": True,
        "format": "filepath",
    }


def family_argument(server):
    return {
        "type": "positional",
        "valueHint": "family",
        "value": server["short"],
        "description": "The document family this server handles.",
    }


def registry_manifest(server, data, version, bundles, release_tag):
    if len(server["summary"]) > REGISTRY_DESCRIPTION_LIMIT:
        raise PackagingError(
            f"the summary of {server['short']} is {len(server['summary'])} characters; "
            f"the registry allows {REGISTRY_DESCRIPTION_LIMIT}"
        )
    packages = [
        {
            "registryType": "pypi",
            "registryBaseUrl": "https://pypi.org",
            "identifier": data["pypiPackage"],
            "version": version,
            "runtimeHint": "uvx",
            "transport": {"type": "stdio"},
            "packageArguments": [family_argument(server), workspace_argument(data)],
        },
        {
            "registryType": "oci",
            "registryBaseUrl": "https://ghcr.io",
            "identifier": f"{data['ociImage']}:{version}-{server['short']}",
            "version": version,
            "runtimeHint": "docker",
            "transport": {"type": "stdio"},
            "runtimeArguments": [
                {
                    "type": "named",
                    "name": "--mount",
                    "value": "type=bind,src={workspace},dst=/work",
                    "description": "Mounts the document folder as the container's workspace.",
                    "isRequired": True,
                    "variables": {
                        "workspace": {
                            "description": data["workspaceDescription"],
                            "isRequired": True,
                            "format": "filepath",
                        }
                    },
                },
                {
                    "type": "named",
                    "name": "--network",
                    "value": "none",
                    "description": "The server speaks over standard input and output and needs no network.",
                },
            ],
            "packageArguments": [family_argument(server)],
        },
    ]
    for bundle in bundles:
        packages.append(
            {
                "registryType": "mcpb",
                "registryBaseUrl": "https://github.com",
                "identifier": f"{data['repository']['url']}/releases/download/{release_tag}/{bundle['name']}",
                "version": version,
                "fileSha256": bundle["sha256"],
                "transport": {"type": "stdio"},
            }
        )
    return {
        "$schema": REGISTRY_SCHEMA,
        "name": server["registryName"],
        "title": server["displayName"],
        "description": server["summary"],
        "version": version,
        "websiteUrl": data["documentation"],
        "repository": {
            "url": data["repository"]["url"],
            "source": data["repository"]["source"],
            "id": data["repository"]["id"],
            "subfolder": server["sourceSubfolder"],
        },
        "packages": packages,
    }


def build_registry_manifests(output, bundle_paths, release_tag, version):
    data = load_servers()
    by_executable = {server["executable"]: server for server in data["servers"]}
    bundles = {}
    for path in bundle_paths:
        described = describe_bundle(path)
        if described["version"] != version:
            raise PackagingError(f"{Path(path).name} is version {described['version']}, VERSION.txt says {version}")
        if described["executable"] not in by_executable:
            raise PackagingError(f"{Path(path).name} names no server in packaging/servers.json")
        bundles.setdefault(described["executable"], []).append(
            {"name": Path(path).name, "sha256": sha256_of(path), "platform": described["platform"]}
        )
    release_tag = release_tag or f"v{version}"
    output = Path(output).resolve()
    written = []
    for server in data["servers"]:
        manifest = registry_manifest(server, data, version, bundles.get(server["executable"], []), release_tag)
        directory = output / server["executable"]
        directory.mkdir(parents=True, exist_ok=True)
        path = directory / "server.json"
        path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
        written.append(path)
    return written


# --- command line -----------------------------------------------------------


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest="command", required=True)

    wheel = commands.add_parser("wheel", help="build the exyokioffice-mcp platform wheel")
    wheel.add_argument("--install-dir", required=True, help="a cmake --install prefix")
    wheel.add_argument("--output", required=True, help="directory the wheel is written into")
    wheel.add_argument("--wheel-tag", help="override the platform tag, e.g. py3-none-manylinux_2_39_x86_64")
    wheel.add_argument(
        "--builder",
        choices=("auto", "hatchling", "build"),
        default="auto",
        help="hatchling in process, or python -m build in an isolated environment (default: whichever is available)",
    )

    mcpb = commands.add_parser("mcpb", help="build one MCP bundle per server")
    mcpb.add_argument("--install-dir", required=True, help="a cmake --install prefix")
    mcpb.add_argument("--output", required=True, help="directory the bundles are written into")
    mcpb.add_argument("--server", action="append", help="build only this server (word, excel, powerpoint); repeatable")
    mcpb.add_argument("--arch", default="x64", help="architecture tag in the bundle name (default: x64)")
    mcpb.add_argument("--icon", help="PNG icon to embed; empty to embed none (default: the repository logo)")

    registry = commands.add_parser("server-json", help="write the MCP Registry manifests")
    registry.add_argument("--output", required=True, help="directory that gets one <server>/server.json each")
    registry.add_argument("--release-tag", help="GitHub release the bundles are attached to (default: v<version>)")
    registry.add_argument("bundles", nargs="*", help="finished .mcpb files whose digests and names go into the manifests")

    manifest = commands.add_parser("manifest", help="print a bundle manifest")
    manifest.add_argument("--server", required=True, help="word, excel or powerpoint")
    manifest.add_argument("--platform", choices=("win32", "linux", "darwin"), default="win32")

    commands.add_parser("names", help="print '<short> <executable> <registry name>' per server")

    args = parser.parse_args(argv)
    version = read_version()

    try:
        if args.command == "wheel":
            tree = InstallTree(args.install_dir, version)
            path = build_wheel(tree, args.output, args.wheel_tag, args.builder)
            print(path)
        elif args.command == "mcpb":
            tree = InstallTree(args.install_dir, version)
            for path in build_bundles(tree, args.output, args.server, args.arch, args.icon):
                print(path)
        elif args.command == "server-json":
            for path in build_registry_manifests(args.output, args.bundles, args.release_tag, version):
                print(path)
        elif args.command == "manifest":
            data = load_servers()
            server = select_servers(data, [args.server])[0]
            catalog = load_catalog(server)
            print(json.dumps(bundle_manifest(server, data, version, args.platform, catalog["tools"], None), indent=2))
        elif args.command == "names":
            for server in load_servers()["servers"]:
                print(server["short"], server["executable"], server["registryName"])
    except PackagingError as error:
        parser.exit(1, f"package.py: {error}\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
