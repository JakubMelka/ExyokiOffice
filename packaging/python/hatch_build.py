"""Hatchling build hook that turns the launcher into a platform wheel.

packaging/package.py stages the native files out of an install tree, writes a
JSON map of where each one goes inside the package, and hands both the map and
the wheel tag over through the environment. This hook only relays them: the
files become part of the wheel through ``force_include`` and the wheel is
tagged for one platform instead of ``py3-none-any``.

Running hatchling without those variables is an error rather than a pure
Python wheel, because a launcher with no binaries behind it would install fine
and fail at first use.
"""

import json
import os

from hatchling.builders.hooks.plugin.interface import BuildHookInterface

FILES_VARIABLE = "EXYOKIOFFICE_MCP_WHEEL_FILES"
TAG_VARIABLE = "EXYOKIOFFICE_MCP_WHEEL_TAG"


class CustomBuildHook(BuildHookInterface):
    PLUGIN_NAME = "custom"

    def initialize(self, version, build_data):
        files_path = os.environ.get(FILES_VARIABLE)
        tag = os.environ.get(TAG_VARIABLE)
        if not files_path or not tag:
            raise RuntimeError(
                "exyokioffice-mcp is built by packaging/package.py, which stages the "
                f"native binaries and sets {FILES_VARIABLE} and {TAG_VARIABLE}; "
                "see docs/tools/mcp-packages.md"
            )
        with open(files_path, encoding="utf-8") as handle:
            files = json.load(handle)
        if not files:
            raise RuntimeError(f"{FILES_VARIABLE} names no files")

        force_include = build_data.setdefault("force_include", {})
        for source, destination in files.items():
            if not os.path.isfile(source):
                raise RuntimeError(f"staged file is missing: {source}")
            force_include[source] = destination

        build_data["pure_python"] = False
        build_data["infer_tag"] = False
        build_data["tag"] = tag
