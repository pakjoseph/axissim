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
    sbom_path = root / "third-party/qt-installation-sbom.spdx"
    if not sbom_path.is_file():
        # macOS package assets live under Resources/axissim-package.
        candidate = root / "Resources/axissim-package/third-party/qt-installation-sbom.spdx"
        if candidate.is_file():
            sbom_path = candidate

    if args.runtime_mode == "bundled-dynamic" and not libraries:
        raise SystemExit("bundled Qt runtime was selected but no native shared libraries were found")
    if args.runtime_mode == "bundled-dynamic" and not sbom_path.is_file():
        raise SystemExit("bundled Qt runtime requires its installation SBOM in third-party/qt-installation-sbom.spdx")

    record = {
        "schema_version": "axissim.package-runtime-inventory.v1",
        "platform": args.platform,
        "qt_version": args.qt_version,
        "qt_runtime_mode": args.runtime_mode,
        "qt_installation_sbom_status": "included" if sbom_path.is_file() else "not-included-system-runtime",
        "runtime_files": libraries,
        "interpretation": "File inventory only; it is not a complete license conclusion or a physical measurement.",
    }
    target = output_dir / "qt-runtime-inventory.json"
    target.write_text(json.dumps(record, sort_keys=True, indent=2) + "\n", encoding="utf-8")
    print(f"Recorded {len(libraries)} packaged native runtime libraries: {target}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
