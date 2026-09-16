"""Tests for packaging/package.py, run from an install tree made of stand-ins.

The script is exercised the way the create_install workflow uses it, but on a
fabricated prefix whose executables are a few bytes long, so the tests need
no build. The wheel test needs hatchling importable and is skipped otherwise;
everything else is standard library only.

    python -m unittest discover -s packaging/tests
"""

import hashlib
import importlib.util
import json
import os
import stat
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]

_spec = importlib.util.spec_from_file_location("exyoki_package", REPO_ROOT / "packaging" / "package.py")
package = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(package)

VERSION = package.read_version()
ABI = package.abi_version(VERSION)
SERVERS = package.load_servers()
PROGRAMS = ["exyoki"] + [server["executable"] for server in SERVERS["servers"]]


def make_tree(root, platform_name):
    """A cmake --install prefix with stand-in binaries for one platform."""
    root = Path(root)
    (root / "bin").mkdir(parents=True)
    suffix = ".exe" if platform_name == "win32" else ""
    for program in PROGRAMS:
        (root / "bin" / (program + suffix)).write_bytes(b"MZ" + program.encode())
    if platform_name == "win32":
        (root / "bin" / "ExyokiOffice.dll").write_bytes(b"MZdll")
    else:
        (root / "lib").mkdir()
        (root / "lib" / f"libExyokiOffice.so.{VERSION}").write_bytes(b"\x7fELF")
        soname = root / "lib" / f"libExyokiOffice.so.{ABI}"
        try:
            soname.symlink_to(f"libExyokiOffice.so.{VERSION}")
        except (OSError, NotImplementedError):
            soname.write_bytes(b"\x7fELF")
    doc = root / "share" / "doc" / "ExyokiOffice"
    (doc / "licenses").mkdir(parents=True)
    (doc / "LICENSE").write_text("MIT License\n", encoding="utf-8")
    (doc / "THIRD-PARTY-LICENSES.md").write_text("# Third-party licenses\n", encoding="utf-8")
    (doc / "licenses" / "pugixml-LICENSE").write_text("MIT\n", encoding="utf-8")
    (doc / "licenses" / "CLI11-LICENSE").write_text("BSD\n", encoding="utf-8")
    return root


def sha256_of(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


class BundleTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name)

    def build(self, platform_name, **kwargs):
        tree = package.InstallTree(make_tree(self.base / platform_name, platform_name), VERSION)
        self.assertEqual(tree.platform, platform_name)
        output = self.base / f"out-{platform_name}"
        return tree, package.build_bundles(tree, output, kwargs.get("servers"), "x64", kwargs.get("icon", ""))

    def test_one_bundle_per_server_with_manifest_binaries_and_notices(self):
        tree, bundles = self.build("win32")
        self.assertEqual(
            [bundle.name for bundle in bundles],
            [f"{server['executable']}-{VERSION}-win32-x64.mcpb" for server in SERVERS["servers"]],
        )
        for bundle, server in zip(bundles, SERVERS["servers"]):
            with zipfile.ZipFile(bundle) as archive:
                names = set(archive.namelist())
                manifest = json.loads(archive.read("manifest.json"))
            executable = f"bin/{server['executable']}.exe"
            self.assertEqual(
                names,
                {
                    "manifest.json",
                    executable,
                    "bin/ExyokiOffice.dll",
                    "LICENSE",
                    "THIRD-PARTY-LICENSES.md",
                    "licenses/CLI11-LICENSE",
                    "licenses/pugixml-LICENSE",
                },
            )
            self.assertEqual(manifest["manifest_version"], package.MANIFEST_VERSION)
            self.assertEqual(manifest["name"], server["executable"])
            self.assertEqual(manifest["version"], VERSION)
            self.assertEqual(manifest["server"]["type"], "binary")
            self.assertEqual(manifest["server"]["entry_point"], executable)
            self.assertEqual(manifest["server"]["mcp_config"]["command"], "${__dirname}/" + executable)
            self.assertEqual(manifest["server"]["mcp_config"]["args"], ["--workspace", "${user_config.workspace}"])
            self.assertEqual(manifest["user_config"]["workspace"]["type"], "directory")
            self.assertTrue(manifest["user_config"]["workspace"]["required"])
            self.assertEqual(manifest["compatibility"]["platforms"], ["win32"])
            self.assertNotIn("icon", manifest)
            catalog = json.loads((REPO_ROOT / server["catalog"]).read_text(encoding="utf-8"))
            self.assertEqual([tool["name"] for tool in manifest["tools"]], [tool["name"] for tool in catalog["tools"]])
            self.assertEqual(len(manifest["tools"]), catalog["toolCount"])
            digest_line = (bundle.parent / (bundle.name + ".sha256")).read_text(encoding="ascii")
            self.assertEqual(digest_line, f"{sha256_of(bundle)}  {bundle.name}\n")

    def test_linux_bundle_ships_the_soname_and_marks_executables(self):
        tree, bundles = self.build("linux", servers=["word"])
        self.assertEqual(len(bundles), 1)
        with zipfile.ZipFile(bundles[0]) as archive:
            names = set(archive.namelist())
            modes = {info.filename: (info.external_attr >> 16) & 0o777 for info in archive.infolist()}
            manifest = json.loads(archive.read("manifest.json"))
            library = archive.read(f"lib/libExyokiOffice.so.{ABI}")
        self.assertIn("bin/exyoki-mcp-word", names)
        self.assertIn(f"lib/libExyokiOffice.so.{ABI}", names)
        self.assertNotIn(f"lib/libExyokiOffice.so.{VERSION}", names)
        self.assertEqual(library, b"\x7fELF")
        self.assertEqual(modes["bin/exyoki-mcp-word"], 0o755)
        self.assertEqual(modes["LICENSE"], 0o644)
        self.assertEqual(manifest["server"]["entry_point"], "bin/exyoki-mcp-word")
        self.assertEqual(manifest["compatibility"]["platforms"], ["linux"])

    def test_unknown_server_and_incomplete_tree_are_refused(self):
        tree = package.InstallTree(make_tree(self.base / "win32", "win32"), VERSION)
        with self.assertRaises(package.PackagingError):
            package.build_bundles(tree, self.base / "out", ["access"], "x64", "")
        os.remove(tree.root / "share" / "doc" / "ExyokiOffice" / "LICENSE")
        with self.assertRaises(package.PackagingError):
            package.build_bundles(tree, self.base / "out", ["word"], "x64", "")
        with self.assertRaises(package.PackagingError):
            package.InstallTree(self.base / "nowhere", VERSION)

    def test_bundle_descriptions_fit_a_bundle_and_the_registry(self):
        for server in SERVERS["servers"]:
            self.assertLessEqual(len(server["summary"]), package.REGISTRY_DESCRIPTION_LIMIT)
            self.assertTrue(server["description"])
            self.assertEqual(server["registryName"], f"{SERVERS['registryNamespace']}/{server['executable']}")


class RegistryManifestTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name)
        self.bundles = []
        for platform_name in ("win32", "linux"):
            tree = package.InstallTree(make_tree(self.base / platform_name, platform_name), VERSION)
            self.bundles.extend(package.build_bundles(tree, self.base / "bundles", None, "x64", ""))

    def test_one_manifest_per_server_pointing_at_pypi_image_and_bundles(self):
        written = package.build_registry_manifests(self.base / "registry", self.bundles, None, VERSION)
        self.assertEqual(
            [path.parent.name for path in written], [server["executable"] for server in SERVERS["servers"]]
        )
        for path, server in zip(written, SERVERS["servers"]):
            manifest = json.loads(path.read_text(encoding="utf-8"))
            self.assertEqual(manifest["$schema"], package.REGISTRY_SCHEMA)
            self.assertEqual(manifest["name"], server["registryName"])
            self.assertEqual(manifest["version"], VERSION)
            self.assertLessEqual(len(manifest["description"]), package.REGISTRY_DESCRIPTION_LIMIT)
            self.assertEqual(manifest["repository"]["subfolder"], server["sourceSubfolder"])
            kinds = [entry["registryType"] for entry in manifest["packages"]]
            self.assertEqual(kinds, ["pypi", "oci", "mcpb", "mcpb"])
            pypi, oci, *bundles = manifest["packages"]
            self.assertEqual(pypi["identifier"], SERVERS["pypiPackage"])
            self.assertEqual(pypi["runtimeHint"], "uvx")
            self.assertEqual(pypi["packageArguments"][0]["value"], server["short"])
            self.assertEqual(pypi["packageArguments"][1]["name"], "--workspace")
            self.assertEqual(oci["identifier"], f"{SERVERS['ociImage']}:{VERSION}-{server['short']}")
            self.assertEqual(oci["runtimeHint"], "docker")
            self.assertEqual(oci["packageArguments"][0]["value"], server["short"])
            for entry in bundles:
                name = entry["identifier"].rsplit("/", 1)[1]
                self.assertTrue(name.startswith(server["executable"] + "-"))
                self.assertIn(f"/releases/download/v{VERSION}/", entry["identifier"])
                bundle = self.base / "bundles" / name
                self.assertEqual(entry["fileSha256"], sha256_of(bundle))
            for entry in manifest["packages"]:
                self.assertEqual(entry["transport"], {"type": "stdio"})

    def test_release_tag_can_be_named_and_foreign_bundles_are_refused(self):
        written = package.build_registry_manifests(self.base / "registry", self.bundles[:1], "v9.9.9-rc1", VERSION)
        manifest = json.loads(written[0].read_text(encoding="utf-8"))
        self.assertIn("/releases/download/v9.9.9-rc1/", manifest["packages"][-1]["identifier"])
        stray = self.base / f"exyoki-mcp-word-0.0.1-win32-x64.mcpb"
        stray.write_bytes(b"PK")
        with self.assertRaises(package.PackagingError):
            package.build_registry_manifests(self.base / "registry", [stray], None, VERSION)
        with self.assertRaises(package.PackagingError):
            package.build_registry_manifests(self.base / "registry", [self.base / "word.mcpb"], None, VERSION)


@unittest.skipUnless(importlib.util.find_spec("hatchling"), "hatchling is not importable")
class WheelTests(unittest.TestCase):
    def test_wheel_carries_every_program_and_a_platform_tag(self):
        with tempfile.TemporaryDirectory() as temp:
            base = Path(temp)
            tree = package.InstallTree(make_tree(base / "linux", "linux"), VERSION)
            wheel = package.build_wheel(tree, base / "out", "py3-none-manylinux_2_39_x86_64", "hatchling")
            self.assertEqual(wheel.name, f"exyokioffice_mcp-{VERSION}-py3-none-manylinux_2_39_x86_64.whl")
            with zipfile.ZipFile(wheel) as archive:
                names = set(archive.namelist())
                info = f"exyokioffice_mcp-{VERSION}.dist-info"
                tags = archive.read(f"{info}/WHEEL").decode("utf-8")
                entry_points = archive.read(f"{info}/entry_points.txt").decode("utf-8")
                metadata = archive.read(f"{info}/METADATA").decode("utf-8")
            for program in PROGRAMS:
                self.assertIn(f"exyokioffice_mcp/bin/{program}", names)
            self.assertIn(f"exyokioffice_mcp/lib/libExyokiOffice.so.{ABI}", names)
            self.assertIn("exyokioffice_mcp/LICENSE", names)
            self.assertIn("exyokioffice_mcp/licenses/pugixml-LICENSE", names)
            self.assertIn("Tag: py3-none-manylinux_2_39_x86_64", tags)
            self.assertIn("Root-Is-Purelib: false", tags)
            for script in ("exyokioffice-mcp", "exyoki-mcp-word", "exyoki-mcp-excel", "exyoki-mcp-power-point", "exyoki"):
                self.assertIn(f"{script} = exyokioffice_mcp:", entry_points)
            self.assertIn(f"Version: {VERSION}", metadata)
            for server in SERVERS["servers"]:
                self.assertIn(f"mcp-name: {server['registryName']}", metadata)
            self.assertTrue((wheel.parent / (wheel.name + ".sha256")).is_file())


if __name__ == "__main__":
    unittest.main()
