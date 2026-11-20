#!/usr/bin/env python3
"""Run a native AxisSim package from its extracted/relocated layout."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import pathlib
import posixpath
import re
import subprocess
import tempfile

from provenance_contract import parse_run_provenance
from windows_pe import PEFormatError, imports_from_file


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


def package_member_path(root: pathlib.Path, relative: str, description: str) -> pathlib.Path:
    """Resolve a POSIX package-relative inventory path without escaping root."""
    if (not isinstance(relative, str) or not relative or "\\" in relative or ":" in relative or
            posixpath.isabs(relative)):
        raise AssertionError(f"invalid package-relative {description} path")
    pure_path = pathlib.PurePosixPath(relative)
    parts = pure_path.parts
    if (not parts or pure_path.as_posix() != relative or
            any(part in ("", ".", "..") for part in parts)):
        raise AssertionError(f"invalid package-relative {description} path")
    base = root.resolve()
    candidate = (base.joinpath(*parts)).resolve()
    try:
        candidate.relative_to(base)
    except ValueError as error:
        raise AssertionError(f"{description} path escapes extracted package root") from error
    return candidate


def check_windows_package_dependencies(root: pathlib.Path) -> str:
    system_root = pathlib.Path(os.environ.get("SystemRoot", r"C:\Windows"))
    system_dirs = [system_root / "System32", system_root / "SysWOW64"]
    packaged_files = [path for path in root.rglob("*") if path.is_file()]
    dlls: dict[str, pathlib.Path] = {}
    binaries = []
    for path in packaged_files:
        if path.suffix.lower() not in (".exe", ".dll"):
            continue
        try:
            imports = imports_from_file(path)
        except PEFormatError as error:
            raise AssertionError(f"invalid packaged PE image {path.relative_to(root)}: {error}") from error
        binaries.append((path, imports))
        if path.suffix.lower() == ".dll":
            key = path.name.casefold()
            previous = dlls.get(key)
            if previous is not None and previous.resolve() != path.resolve():
                raise AssertionError(f"duplicate packaged DLL name: {previous} and {path}")
            dlls[key] = path
    if not binaries:
        raise AssertionError("portable Windows package contains no PE executables or DLLs")

    missing = []
    for binary, imports in binaries:
        for name in imports:
            key = name.casefold()
            if key in dlls:
                continue
            if key.startswith(("api-ms-win-", "ext-ms-win-")):
                continue
            if any((directory / name).is_file() for directory in system_dirs):
                continue
            missing.append(f"{binary.relative_to(root)} -> {name}")
    if missing:
        raise AssertionError("unresolved Windows PE imports outside package/System32:\n" + "\n".join(missing))
    if not (root / "platforms/qwindows.dll").is_file():
        raise AssertionError("Windows package is missing the Qt qwindows platform plugin")
    return f"Normal PE import closure verified for {len(binaries)} package executables/DLLs; only package and OS DLLs resolve. Delay-load imports and runtime LoadLibrary calls are outside this static check."


def check_dependencies(binary: pathlib.Path, platform: str, package_root: pathlib.Path) -> str:
    if platform == "linux":
        environment = runtime_environment(platform, package_root)
        result = subprocess.run(["ldd", str(binary)], capture_output=True, text=True,
                                check=True, env=environment)
        if "not found" in result.stdout:
            raise AssertionError(f"unresolved shared library for {binary.name}:\n{result.stdout}")
        return result.stdout
    if platform == "windows":
        return check_windows_package_dependencies(package_root)
    return "macOS dependencies are checked by the package script with otool-based closure verification."


def check_deb_qt_dependencies(deb_path: pathlib.Path, extracted_root: pathlib.Path) -> str:
    depends = subprocess.run(["dpkg-deb", "-f", str(deb_path), "Depends"],
                             capture_output=True, text=True, check=True).stdout.strip()
    declared = set()
    for clause in depends.split(","):
        for alternative in clause.split("|"):
            match = re.match(r"\s*([A-Za-z0-9+.-]+)", alternative)
            if match:
                declared.add(match.group(1))
    required = {"qt6-qpa-plugins"}
    binaries = [extracted_root / "usr/bin/axissim", extracted_root / "usr/bin/axissim-gui"]
    observed_qt_packages = set()
    environment = runtime_environment("linux", extracted_root)
    for binary in binaries:
        output = subprocess.run(["ldd", str(binary)], capture_output=True, text=True,
                                check=True, env=environment).stdout
        if "not found" in output:
            raise AssertionError(f"extracted Debian binary has unresolved libraries:\n{output}")
        for line in output.splitlines():
            if "libQt6" not in line or "=>" not in line:
                continue
            path_text = line.split("=>", 1)[1].strip().split(" ", 1)[0]
            library = pathlib.Path(path_text).resolve()
            owner = subprocess.run(["dpkg-query", "-S", str(library)],
                                   capture_output=True, text=True)
            if owner.returncode != 0:
                raise AssertionError(f"cannot identify Debian package owning Qt runtime library {library}")
            package_name = owner.stdout.split(":", 1)[0].strip()
            if package_name:
                observed_qt_packages.add(package_name)
    required.update(observed_qt_packages)
    missing = sorted(required - declared)
    if missing:
        raise AssertionError(
            "Debian Depends omits required system Qt runtime packages: " + ", ".join(missing) +
            f"; declared dependencies are: {depends}"
        )
    return f"Debian Depends declares Qt runtime packages resolved from extracted binaries: {', '.join(sorted(required))}"


def runtime_environment(platform: str, root: pathlib.Path) -> dict[str, str]:
    environment = os.environ.copy()
    for key in list(environment):
        if key.upper().startswith(("QT_", "QML")) or key.upper() in ("LD_LIBRARY_PATH", "DYLD_LIBRARY_PATH"):
            environment.pop(key, None)
    if platform == "windows":
        system_root = os.environ.get("SystemRoot", r"C:\Windows")
        environment["PATH"] = ";".join((str(root), str(pathlib.Path(system_root) / "System32"), system_root))
    elif platform == "linux":
        environment.pop("LD_PRELOAD", None)
    return environment


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", required=True, type=pathlib.Path)
    parser.add_argument("--platform", required=True, choices=("windows", "linux", "macos"))
    parser.add_argument("--layout", required=True, choices=("portable", "deb", "macos"))
    parser.add_argument("--expected-source-revision", required=True)
    parser.add_argument("--deb-file", type=pathlib.Path)
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
        raise AssertionError(
            "package source tree was not clean at configure time: "
            f"state={metadata.get('source_tree_state')!r}, package_id={metadata.get('package_id')!r}"
        )
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
    sbom_status = inventory.get("qt_installation_sbom_status")
    sbom_files = inventory.get("qt_installation_sboms", [])
    if sbom_status != metadata.get("build", {}).get("qt_installation_sbom_status"):
        raise AssertionError("Qt SBOM inventory status differs from package build provenance")
    if not isinstance(sbom_files, list):
        raise AssertionError("Qt installation SBOM inventory must be a list")
    if sbom_status == "included_installation_catalog_not_runtime_mapping":
        if not sbom_files:
            raise AssertionError("runtime inventory declares an empty Qt installation SBOM catalog")
        for item in sbom_files:
            if not isinstance(item, dict):
                raise AssertionError("malformed Qt installation SBOM inventory entry")
            sbom_path = package_member_path(root, item.get("path"), "Qt installation SBOM catalog file")
            sbom = require_file(sbom_path, "Qt installation SBOM catalog file")
            if sha256(sbom) != item.get("sha256"):
                raise AssertionError(f"Qt installation SBOM hash mismatch: {item['path']}")
    elif sbom_status != "unavailable" or sbom_files:
        raise AssertionError("Qt installation SBOM status and inventory are inconsistent")

    if metadata["build"]["qt_runtime_mode"] == "bundled-dynamic":
        if sbom_status != "included_installation_catalog_not_runtime_mapping":
            raise AssertionError("bundled Qt package omitted its declared installation SBOM catalog")
        for item in inventory.get("runtime_files", []):
            if not isinstance(item, dict):
                raise AssertionError("malformed native runtime inventory entry")
            runtime_path = package_member_path(root, item.get("path"), "inventoried runtime library")
            require_file(runtime_path, "inventoried runtime library")
            if sha256(runtime_path) != item["sha256"]:
                raise AssertionError(f"runtime library hash mismatch: {item['path']}")
        if not inventory.get("runtime_files"):
            raise AssertionError("bundled Qt package has an empty runtime inventory")
    elif inventory.get("qt_runtime_mode") != "system-dynamic":
        raise AssertionError("Linux package must declare system Qt runtime dependency")
    if args.layout == "deb":
        if args.deb_file is None or not args.deb_file.is_file():
            raise AssertionError("Debian package verification requires the original .deb file")
        check_deb_qt_dependencies(args.deb_file.resolve(), root)

    dependencies = "\n".join(
        filter(None, (check_dependencies(cli, args.platform, root), check_dependencies(gui, args.platform, root)))
    )
    with tempfile.TemporaryDirectory(prefix="axissim-extracted-package-") as temporary:
        workspace = pathlib.Path(temporary)
        result_path = workspace / "baseline-result.json"
        trace_path = workspace / "baseline-trace.csv"
        environment = runtime_environment(args.platform, root)
        subprocess.run([str(cli), "validate", str(manifest)], cwd=workspace, env=environment,
                       check=True, capture_output=True, text=True, timeout=60)
        subprocess.run([str(cli), "run", str(manifest), "--json", str(result_path), "--trace", str(trace_path)],
                       cwd=workspace, env=environment, check=True, capture_output=True, text=True, timeout=120)
        result = json.loads(result_path.read_text(encoding="utf-8"))
        sidecar = parse_run_provenance(
            pathlib.Path(str(result_path) + ".provenance.json").read_text(encoding="utf-8"),
            result_json_bytes=result_path.read_bytes(),
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
            environment = runtime_environment(args.platform, root)
            if args.platform == "linux":
                environment["QT_QPA_PLATFORM"] = "offscreen"
            subprocess.run([str(gui), "--smoke-test-run"], cwd=workspace, env=environment,
                           check=True, capture_output=True, text=True, timeout=120)

    print(f"Extracted package verified: {args.platform}/{args.layout}; source {args.expected_source_revision}; Qt mode {metadata['build']['qt_runtime_mode']}")
    if dependencies.strip():
        print("Runtime dependency evidence captured.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
