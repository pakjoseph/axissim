#!/usr/bin/env python3
"""Record the native shared libraries actually present in a package tree."""

from __future__ import annotations

import argparse
import hashlib
import json
import pathlib


def digest(path: pathlib.Path) -> str:
    value = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            value.update(block)
    return value.hexdigest()


def is_runtime_library(path: pathlib.Path) -> bool:
    name = path.name.lower()
    if name.endswith((".dll", ".dylib", ".so")) or ".so." in name:
        return True
    return any(parent.name.endswith(".framework") and path.name == parent.name[:-len(".framework")]
               for parent in path.parents)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", required=True, type=pathlib.Path)
    parser.add_argument("--platform", required=True, choices=("windows", "linux", "macos"))
    parser.add_argument("--qt-version", required=True)
    parser.add_argument("--runtime-mode", required=True, choices=("bundled-dynamic", "system-dynamic"))
    parser.add_argument("--output-dir", required=True, type=pathlib.Path)
    args = parser.parse_args()
    root = args.root.resolve()
    output_dir = args.output_dir.resolve()
    output_dir.mkdir(parents=True, exist_ok=True)

    libraries = []
    for path in sorted(root.rglob("*"), key=lambda item: item.as_posix().lower()):
        if not path.is_file() or not is_runtime_library(path):
            continue
        relative = path.relative_to(root).as_posix()
        libraries.append({"path": relative, "sha256": digest(path)})
    sbom_dir = root / "third-party/qt-installation-sboms"
    if not sbom_dir.is_dir():
        # macOS package assets live under Resources/axissim-package.
        candidate = root / "Resources/axissim-package/third-party/qt-installation-sboms"
        if candidate.is_dir():
            sbom_dir = candidate
    sbom_files = []
    if sbom_dir.is_dir():
        for path in sorted(sbom_dir.rglob("*"), key=lambda item: item.as_posix().lower()):
            if path.is_file() and path.suffix.lower() in (".spdx", ".json"):
                sbom_files.append({
                    "path": path.relative_to(root).as_posix(),
                    "sha256": digest(path),
                })

    if args.runtime_mode == "bundled-dynamic" and not libraries:
        raise SystemExit("bundled Qt runtime was selected but no native shared libraries were found")
    if args.runtime_mode == "bundled-dynamic" and not sbom_files:
        raise SystemExit("bundled Qt runtime requires Qt installation SBOM files under third-party/qt-installation-sboms/")

    record = {
        "schema_version": "axissim.package-runtime-inventory.v1",
        "platform": args.platform,
        "qt_version": args.qt_version,
        "qt_runtime_mode": args.runtime_mode,
        "qt_installation_sbom_status": (
            "included_installation_catalog_not_runtime_mapping" if sbom_files else "unavailable"
        ),
        "qt_installation_sboms": sbom_files,
        "runtime_files": libraries,
        "interpretation": (
            "runtime_files is the exact shipped native-file inventory. qt_installation_sboms are an installation-level "
            "license catalog and are not asserted to map one-to-one to runtime_files. This record is not a complete "
            "license conclusion or a physical measurement."
        ),
    }
    target = output_dir / "qt-runtime-inventory.json"
    target.write_text(json.dumps(record, sort_keys=True, indent=2) + "\n", encoding="utf-8")
    print(f"Recorded {len(libraries)} packaged native runtime libraries: {target}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
