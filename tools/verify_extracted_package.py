#!/usr/bin/env python3
"""Run a native AxisSim package from its extracted/relocated layout."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import pathlib
import re
import subprocess
import tempfile

from provenance_contract import parse_run_provenance


def sha256(path: pathlib.Path) -> str:
    value = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            value.update(block)
    return value.hexdigest()


def require_file(path: pathlib.Path, description: str) -> pathlib.Path:
    if not path.is_file():
        raise AssertionError(f"missing {description}: {path}")
    return path


def check_dependencies(binary: pathlib.Path, platform: str) -> str:
    if platform == "linux":
        result = subprocess.run(["ldd", str(binary)], capture_output=True, text=True, check=True)
        if "not found" in result.stdout:
            raise AssertionError(f"unresolved shared library for {binary.name}:\n{result.stdout}")
        return result.stdout
    if platform == "windows":
        return "Native Windows process launch and GUI smoke validate loader dependencies."
    return "macOS dependencies are checked by the package script with otool-based closure verification."


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", required=True, type=pathlib.Path)
    parser.add_argument("--platform", required=True, choices=("windows", "linux", "macos"))
    parser.add_argument("--layout", required=True, choices=("portable", "deb", "macos"))
    parser.add_argument("--expected-source-revision", required=True)
    parser.add_argument("--gui-smoke", action="store_true")
    args = parser.parse_args()
    root = args.root.resolve()
    if args.layout == "portable":
        docs = root
        cli = root / ("axissim-cli.exe" if args.platform == "windows" else "axissim-cli")
        gui = root / ("AxisSim.exe" if args.platform == "windows" else "AxisSim")
        data = root / "axissim-data"
    elif args.layout == "deb":
        docs = root / "usr/share/doc/axissim"
        cli = root / "usr/bin/axissim"
        gui = root / "usr/bin/axissim-gui"
        data = root / "usr/share/axissim/axissim-data"
    else:
        docs = root / "Resources/axissim-package"
        cli = root / "MacOS/axissim-cli"
        gui = root / "MacOS/AxisSim"
        data = root / "Resources/axissim-data"

    require_file(docs / "LICENSE", "AxisSim license")
    require_file(docs / "THIRD_PARTY_NOTICES.md", "third-party notices")
    require_file(docs / "licenses/LGPL-3.0.txt", "Qt LGPL license text")
    metadata = json.loads(require_file(docs / "package-provenance.json", "package provenance").read_text(encoding="utf-8"))
    if metadata.get("schema_version") != "axissim.package-provenance.v1":
        raise AssertionError("package provenance schema is unsupported")
    if metadata.get("source_revision") != args.expected_source_revision:
        raise AssertionError("package source revision differs from the expected checkout")
    if metadata.get("source_tree_state") != "clean":
        raise AssertionError("package source tree was not clean at configure time")
    if metadata.get("artifact_kind") not in ("validation", "release"):
        raise AssertionError("package artifact kind must be explicitly validation or release")
    if metadata["artifact_kind"] == "release":
        if metadata.get("release_tag") in (None, "", "unknown") or metadata.get("package_id") != metadata.get("release_tag"):
            raise AssertionError("release package identity is incomplete")
    elif not metadata.get("package_id", "").startswith("validation-"):
        raise AssertionError("untagged build was not explicitly labeled as a validation package")

    require_file(cli, "packaged C++ CLI")
    require_file(gui, "packaged Qt GUI")
    manifest = require_file(data / "configs/baseline/manifest.conf", "bundled baseline manifest")
    inventory_path = require_file(docs / "third-party/qt-runtime-inventory.json", "runtime dependency inventory")
    inventory = json.loads(inventory_path.read_text(encoding="utf-8"))
    if inventory.get("schema_version") != "axissim.package-runtime-inventory.v1":
        raise AssertionError("runtime inventory schema is unsupported")
    if inventory.get("platform") != args.platform:
        raise AssertionError("runtime inventory platform does not match package platform")
    if metadata["build"]["qt_runtime_mode"] == "bundled-dynamic":
        if inventory.get("qt_installation_sbom_status") != "included":
            raise AssertionError("bundled Qt package omitted its installation SBOM")
        for item in inventory.get("runtime_files", []):
            require_file(root / item["path"], "inventoried runtime library")
            if sha256(root / item["path"]) != item["sha256"]:
                raise AssertionError(f"runtime library hash mismatch: {item['path']}")
        if not inventory.get("runtime_files"):
            raise AssertionError("bundled Qt package has an empty runtime inventory")
    elif inventory.get("qt_runtime_mode") != "system-dynamic":
        raise AssertionError("Linux package must declare system Qt runtime dependency")

    dependencies = "\n".join(
        filter(None, (check_dependencies(cli, args.platform), check_dependencies(gui, args.platform)))
    )
    with tempfile.TemporaryDirectory(prefix="axissim-extracted-package-") as temporary:
        workspace = pathlib.Path(temporary)
        result_path = workspace / "baseline-result.json"
        trace_path = workspace / "baseline-trace.csv"
        subprocess.run([str(cli), "validate", str(manifest)], cwd=workspace, check=True, capture_output=True, text=True, timeout=60)
        subprocess.run([str(cli), "run", str(manifest), "--json", str(result_path), "--trace", str(trace_path)], cwd=workspace, check=True, capture_output=True, text=True, timeout=120)
        result = json.loads(result_path.read_text(encoding="utf-8"))
        sidecar = parse_run_provenance(
            pathlib.Path(str(result_path) + ".provenance.json").read_text(encoding="utf-8"),
            require_complete=True,
        )
        if result.get("schema_version") not in ("1.2", "1.3"):
            raise AssertionError("packaged baseline emitted an unexpected result schema")
        if not trace_path.is_file() or trace_path.stat().st_size <= 30:
            raise AssertionError("packaged baseline did not emit a task trace")
        provenance = sidecar.get("provenance", {})
        if provenance.get("source", {}).get("revision") != args.expected_source_revision:
            raise AssertionError("run sidecar source revision does not match package metadata")
        if provenance.get("executable", {}).get("sha256") != sha256(cli):
            raise AssertionError("run sidecar executable SHA-256 does not match packaged CLI")
        if re.fullmatch(r"[0-9a-f]{64}", sidecar.get("identity_sha256", "")) is None:
            raise AssertionError("run provenance identity hash is malformed")
        if pathlib.Path(provenance.get("manifest", {}).get("path", "")).is_absolute():
            raise AssertionError("run provenance contains a machine-specific manifest path")

        if args.gui_smoke:
            environment = os.environ.copy()
            if args.platform == "linux":
                environment["QT_QPA_PLATFORM"] = "offscreen"
            else:
                environment.pop("QT_QPA_PLATFORM", None)
            subprocess.run([str(gui), "--smoke-test-run"], cwd=workspace, env=environment,
                           check=True, capture_output=True, text=True, timeout=120)

    print(f"Extracted package verified: {args.platform}/{args.layout}; source {args.expected_source_revision}; Qt mode {metadata['build']['qt_runtime_mode']}")
    if dependencies.strip():
        print("Runtime dependency evidence captured.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
