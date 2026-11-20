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
    if "source-code" not in notices or "validation artifacts only" not in notices:
        raise AssertionError("package notices must preserve Qt redistribution limitations")
    workflow = (root / ".github/workflows/release-packages.yml").read_text(encoding="utf-8")
    if 'run: ./scripts/package_linux.sh build-release "$AXISSIM_RELEASE_TAG"' not in workflow:
        raise AssertionError("Linux release tag must enter the shell through an environment variable")
    if "ReleaseTag $env:AXISSIM_RELEASE_TAG" not in workflow:
        raise AssertionError("Windows release tag must enter PowerShell through an environment variable")
    if 'run: ./scripts/package_macos.sh build-release "$AXISSIM_RELEASE_TAG"' not in workflow:
        raise AssertionError("macOS release tag must enter the shell through an environment variable")
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
