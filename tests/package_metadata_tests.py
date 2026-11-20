#!/usr/bin/env python3
"""Checks configured package identity, license staging, and tag mismatch rejection."""

from __future__ import annotations

import argparse
import json
import pathlib
import re
import shutil
import subprocess
import tempfile


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-root", required=True, type=pathlib.Path)
    parser.add_argument("--build-dir", required=True, type=pathlib.Path)
    args = parser.parse_args()
    root = args.source_root.resolve()
    build = args.build_dir.resolve()
    metadata_path = build / "package-assets/package-provenance.json"
    metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
    revision = subprocess.run(
        ["git", "-C", str(root), "rev-parse", "HEAD"],
        check=True,
        capture_output=True,
        text=True,
    ).stdout.strip()
    if metadata.get("schema_version") != "axissim.package-provenance.v1":
        raise AssertionError("unsupported package provenance schema")
    if metadata.get("source_revision") != revision:
        raise AssertionError("package metadata does not bind the configured source revision")
    tree_status = subprocess.run(
        ["git", "-C", str(root), "status", "--porcelain", "--untracked-files=all"],
        check=True,
        capture_output=True,
        text=True,
    ).stdout.strip()
    expected_tree_state = "clean" if not tree_status else "modified"
    if metadata.get("source_tree_state") != expected_tree_state:
        raise AssertionError("package metadata does not report tracked and untracked source-tree state")
    if metadata.get("artifact_kind") != "validation" or metadata.get("release_tag") != "unknown":
        raise AssertionError("untagged package must be labeled as a validation artifact")
    if not metadata.get("package_id", "").startswith("validation-"):
        raise AssertionError("validation package ID is missing")
    if metadata["build"].get("compiler_version") in (None, "", "unknown"):
        raise AssertionError("configured compiler identity is missing")
    if metadata["build"].get("cxx_standard") != "20":
        raise AssertionError("package metadata must record C++20")
    if (build / "package-assets/LICENSE").read_bytes() != (root / "LICENSE").read_bytes():
        raise AssertionError("repository LICENSE was not staged exactly")
    if (build / "package-assets/licenses/LGPL-3.0.txt").stat().st_size < 7000:
        raise AssertionError("Qt LGPL license text is missing or truncated")
    notices = (build / "package-assets/THIRD_PARTY_NOTICES.md").read_text(encoding="utf-8")
    normalized_notices = " ".join(notices.split())
    if "source-code" not in normalized_notices or "validation artifacts only" not in normalized_notices:
        raise AssertionError("package notices must preserve Qt redistribution limitations")
    if "not yet prove a complete per-runtime-file license mapping" not in normalized_notices:
        raise AssertionError("package notices must not overstate the installation SBOM as a runtime license map")
    import sys
    sys.path.insert(0, str(root / "tools"))
    from verify_extracted_package import package_member_path
    with tempfile.TemporaryDirectory(prefix="axissim-qt-inventory-") as temporary:
        fixture = pathlib.Path(temporary)
        sbom_root = fixture / "third-party/qt-installation-sboms/share/qt/sbom"
        sbom_root.mkdir(parents=True)
        (sbom_root / "qtbase.spdx").write_text("SPDXVersion: SPDX-2.3\n", encoding="utf-8")
        (sbom_root / "qtgui.spdx.json").write_text('{"spdxVersion":"SPDX-2.3"}\n', encoding="utf-8")
        (fixture / "Qt6Core.dll").write_bytes(b"synthetic runtime file")
        output_dir = fixture / "inventory-output"
        subprocess.run(
            [
                sys.executable, str(root / "tools/generate_package_runtime_inventory.py"),
                "--root", str(fixture), "--platform", "windows", "--qt-version", "6.8.3",
                "--runtime-mode", "bundled-dynamic", "--output-dir", str(output_dir),
            ],
            check=True, capture_output=True, text=True,
        )
        inventory = json.loads((output_dir / "qt-runtime-inventory.json").read_text(encoding="utf-8"))
        if inventory.get("qt_installation_sbom_status") != "included_installation_catalog_not_runtime_mapping":
            raise AssertionError("runtime inventory must label the SBOMs as a catalog, not a file mapping")
        sboms = inventory.get("qt_installation_sboms", [])
        expected_sbom_paths = {
            "third-party/qt-installation-sboms/share/qt/sbom/qtbase.spdx",
            "third-party/qt-installation-sboms/share/qt/sbom/qtgui.spdx.json",
        }
        if {item.get("path") for item in sboms} != expected_sbom_paths:
            raise AssertionError("runtime inventory must retain every SBOM path without basename collisions")
        for item in sboms:
            if not re.fullmatch(r"[0-9a-f]{64}", item.get("sha256", "")):
                raise AssertionError("runtime inventory must hash every installation SBOM")
            if not (fixture / item["path"]).is_file():
                raise AssertionError("runtime inventory SBOM path must exist in the package root")
        if len(inventory.get("runtime_files", [])) != 1:
            raise AssertionError("runtime inventory must retain the exact native package file list")
        (fixture / "Qt6Core.dll").unlink()
        system_output = fixture / "system-inventory-output"
        subprocess.run(
            [
                sys.executable, str(root / "tools/generate_package_runtime_inventory.py"),
                "--root", str(fixture), "--platform", "linux", "--qt-version", "6.8.3",
                "--runtime-mode", "system-dynamic", "--output-dir", str(system_output),
            ],
            check=True, capture_output=True, text=True,
        )
        system_inventory = json.loads((system_output / "qt-runtime-inventory.json").read_text(encoding="utf-8"))
        if ({item.get("path") for item in system_inventory.get("qt_installation_sboms", [])} != expected_sbom_paths or
                system_inventory.get("qt_installation_sbom_status") != "included_installation_catalog_not_runtime_mapping" or
                system_inventory.get("runtime_files") != []):
            raise AssertionError("system-Qt package assets must retain available SBOM hashes without inventing bundled libraries")
        for unsafe_path in ("../outside.spdx", "sub/../../outside.spdx", "/outside.spdx", "C:/outside.spdx"):
            try:
                package_member_path(fixture, unsafe_path, "synthetic inventory item")
            except AssertionError:
                pass
            else:
                raise AssertionError(f"package verifier accepted path outside its extraction root: {unsafe_path}")
    workflow = (root / ".github/workflows/release-packages.yml").read_text(encoding="utf-8")
    if 'run: ./scripts/package_linux.sh build/package-linux-release "$AXISSIM_RELEASE_TAG"' not in workflow:
        raise AssertionError("Linux release tag must enter the shell through an environment variable")
    if "ReleaseTag $env:AXISSIM_RELEASE_TAG" not in workflow:
        raise AssertionError("Windows release tag must enter PowerShell through an environment variable")
    if 'run: ./scripts/package_macos.sh build/package-macos-release "$AXISSIM_RELEASE_TAG"' not in workflow:
        raise AssertionError("macOS release tag must enter the shell through an environment variable")
    if "build/package-windows-release" not in workflow:
        raise AssertionError("Windows package build must stay under the ignored build directory")
    if "runs-on: macos-15" not in workflow:
        raise AssertionError("Qt 6.8.3 package validation must avoid the incompatible macOS 26 SDK")
    if "upload-artifact" in workflow or "create-release" in workflow:
        raise AssertionError("package validation workflow must not publish or upload package outputs")

    with tempfile.TemporaryDirectory(prefix="axissim-tag-mismatch-") as temporary:
        temporary_root = pathlib.Path(temporary)
        clean_source = temporary_root / "source"
        shutil.copytree(
            root,
            clean_source,
            ignore=shutil.ignore_patterns(
                ".git", ".codex", "build", "build-*", "__pycache__", ".venv", "output", "deliverables"
            ),
        )
        subprocess.run(["git", "-C", str(clean_source), "init", "-q"], check=True)
        subprocess.run(["git", "-C", str(clean_source), "config", "user.name", "Package test"], check=True)
        subprocess.run(["git", "-C", str(clean_source), "config", "user.email", "package-test@example.invalid"], check=True)
        subprocess.run(["git", "-C", str(clean_source), "add", "."], check=True)
        subprocess.run(["git", "-C", str(clean_source), "commit", "-qm", "fixture source"], check=True)
        first_commit = subprocess.run(
            ["git", "-C", str(clean_source), "rev-parse", "HEAD"],
            check=True,
            capture_output=True,
            text=True,
        ).stdout.strip()
        subprocess.run(["git", "-C", str(clean_source), "tag", "existing-tag"], check=True)
        subprocess.run(["git", "-C", str(clean_source), "branch", "branch-only-tag"], check=True)
        subprocess.run(["git", "-C", str(clean_source), "checkout", "-q", first_commit], check=True)
        branch_only = subprocess.run(
            [
                "cmake",
                "-S",
                str(clean_source),
                "-B",
                str(temporary_root / "branch-only-build"),
                "-DAXISSIM_BUILD_GUI=OFF",
                "-DAXISSIM_RELEASE_TAG=branch-only-tag",
            ],
            capture_output=True,
            text=True,
        )
        if branch_only.returncode == 0:
            raise AssertionError("a branch without a same-named tag was accepted as a release tag")
        matching = subprocess.run(
            [
                "cmake",
                "-S",
                str(clean_source),
                "-B",
                str(temporary_root / "matching-build"),
                "-DAXISSIM_BUILD_GUI=OFF",
                "-DAXISSIM_RELEASE_TAG=existing-tag",
            ],
            capture_output=True,
            text=True,
        )
        if matching.returncode != 0:
            raise AssertionError(f"a tag resolving to the exact source revision was rejected: {matching.stdout}{matching.stderr}")
        release_metadata = json.loads(
            (temporary_root / "matching-build/package-assets/package-provenance.json").read_text(encoding="utf-8")
        )
        if (release_metadata.get("artifact_kind") != "release" or
                release_metadata.get("release_tag") != "existing-tag" or
                release_metadata.get("source_revision") != first_commit or
                release_metadata.get("package_id") != "existing-tag"):
            raise AssertionError("verified package release identity does not bind the matching tag and source revision")
        untracked_input = clean_source / "configs" / "untracked-package-input.conf"
        untracked_input.write_text("scenario_name = untracked_package_input\n", encoding="utf-8")
        untracked_release = subprocess.run(
            [
                "cmake",
                "-S",
                str(clean_source),
                "-B",
                str(temporary_root / "untracked-release-build"),
                "-DAXISSIM_BUILD_GUI=OFF",
                "-DAXISSIM_RELEASE_TAG=existing-tag",
            ],
            capture_output=True,
            text=True,
        )
        if untracked_release.returncode == 0 or "clean source tree" not in (untracked_release.stdout + untracked_release.stderr):
            raise AssertionError("an untracked configuration that would ship in the package was accepted")
        untracked_input.unlink()
        (clean_source / "tag-fixture.txt").write_text("second revision\n", encoding="utf-8")
        subprocess.run(["git", "-C", str(clean_source), "add", "tag-fixture.txt"], check=True)
        subprocess.run(["git", "-C", str(clean_source), "commit", "-qm", "second fixture"], check=True)
        if subprocess.run(["git", "-C", str(clean_source), "rev-parse", "HEAD"], check=True, capture_output=True, text=True).stdout.strip() == first_commit:
            raise AssertionError("tag mismatch fixture did not create distinct revisions")
        mismatch = subprocess.run(
            [
                "cmake",
                "-S",
                str(clean_source),
                "-B",
                str(temporary_root / "build"),
                "-DAXISSIM_BUILD_GUI=OFF",
                "-DAXISSIM_RELEASE_TAG=existing-tag",
            ],
            capture_output=True,
            text=True,
        )
        combined = mismatch.stdout + mismatch.stderr
        normalized = re.sub(r"\s+", " ", combined)
        if mismatch.returncode == 0 or "does not resolve to packaged source revision" not in normalized:
            raise AssertionError(f"a mismatched release tag was not rejected explicitly: {combined}")

    print("package metadata contract passed: source revision, tree state, validation identity, exact tag refs, licenses, matching/mismatched tags, and untracked package input rejection")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
