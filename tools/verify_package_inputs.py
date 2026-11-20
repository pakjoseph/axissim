#!/usr/bin/env python3
"""Require packaged configuration/data files to match the selected Git tree."""

from __future__ import annotations

import argparse
import os
import pathlib
import stat
import subprocess
import sys


def run_git(root: pathlib.Path, *arguments: str, input_bytes: bytes | None = None) -> bytes:
    result = subprocess.run(
        ["git", *arguments],
        cwd=root,
        input=input_bytes,
        check=True,
        capture_output=True,
    )
    return result.stdout


def committed_inputs(
    root: pathlib.Path,
    revision: str,
    subtrees: tuple[str, ...] | None,
) -> dict[str, tuple[str, str]]:
    arguments = ["ls-tree", "-r", "-z", revision]
    if subtrees is not None:
        arguments.extend(("--", *subtrees))
    raw_entries = run_git(root, *arguments)
    entries: dict[str, tuple[str, str]] = {}
    for raw_entry in raw_entries.split(b"\0"):
        if not raw_entry:
            continue
        metadata, raw_path = raw_entry.split(b"\t", 1)
        mode, object_type, object_id = metadata.decode("ascii").split(" ", 2)
        if object_type != "blob":
            raise ValueError(f"unsupported non-file package input: {os.fsdecode(raw_path)}")
        path = os.fsdecode(raw_path)
        entries[path] = (mode, object_id)
    return entries


def working_inputs(root: pathlib.Path) -> dict[str, pathlib.Path]:
    files: dict[str, pathlib.Path] = {}
    for subtree in ("configs", "reference_data"):
        base = root / subtree
        if not base.exists():
            continue
        for current, directories, names in os.walk(base, followlinks=False):
            current_path = pathlib.Path(current)
            for name in list(directories):
                path = current_path / name
                relative = path.relative_to(root).as_posix()
                if path.is_symlink():
                    files[relative] = path
                    directories.remove(name)
            for name in names:
                path = current_path / name
                files[path.relative_to(root).as_posix()] = path
    return files


def symlinked_parent(root: pathlib.Path, relative_path: str) -> pathlib.Path | None:
    current = root
    for component in pathlib.PurePosixPath(relative_path).parts[:-1]:
        current = current / component
        if current.is_symlink():
            return current
    return None


def validate_package_inputs(root: pathlib.Path, revision: str = "HEAD") -> list[str]:
    errors: list[str] = []
    for subtree in ("configs", "reference_data"):
        if (root / subtree).is_symlink():
            return [f"package input directory is a symlink and cannot be verified safely: {subtree}"]
    try:
        resolved_revision = run_git(root, "rev-parse", "--verify", f"{revision}^{{commit}}").decode("ascii").strip()
        expected = committed_inputs(root, resolved_revision, ("configs", "reference_data"))
    except (OSError, subprocess.CalledProcessError, UnicodeDecodeError, ValueError) as error:
        return [f"cannot read the selected Git source tree: {error}"]

    actual = working_inputs(root)
    missing = sorted(set(expected) - set(actual))
    extra = sorted(set(actual) - set(expected))
    for path in missing:
        errors.append(f"missing committed package input: {path}")
    for path in extra:
        errors.append(f"uncommitted package input: {path}")

    for path in sorted(set(expected) & set(actual)):
        mode, expected_object = expected[path]
        source_path = actual[path]
        if stat.S_ISLNK(source_path.lstat().st_mode):
            errors.append(f"symbolic links are not supported as package inputs: {path}")
            continue
        if mode == "120000":
            errors.append(f"package input changed from a symlink to a regular file: {path}")
            continue
        try:
            result = subprocess.run(
                ["git", "hash-object", f"--path={path}", "--", path],
                cwd=root,
                check=True,
                capture_output=True,
            )
            actual_object = result.stdout
        except (OSError, subprocess.CalledProcessError) as error:
            errors.append(f"cannot verify package input {path}: {error}")
            continue
        actual_id = actual_object.decode("ascii").strip()
        if actual_id != expected_object:
            errors.append(f"package input differs from {resolved_revision}: {path}")
    return errors


def validate_all_tracked_inputs(root: pathlib.Path, revision: str = "HEAD") -> list[str]:
    """Check every tracked file against the commit, ignoring index visibility flags."""
    try:
        resolved_revision = run_git(root, "rev-parse", "--verify", f"{revision}^{{commit}}").decode("ascii").strip()
        expected = committed_inputs(root, resolved_revision, None)
    except (OSError, subprocess.CalledProcessError, UnicodeDecodeError, ValueError) as error:
        return [f"cannot read the selected Git source tree: {error}"]

    errors: list[str] = []
    regular_paths: list[str] = []
    for path, (mode, _object_id) in sorted(expected.items()):
        linked_parent = symlinked_parent(root, path)
        if linked_parent is not None:
            errors.append(f"release source path has a symlinked parent: {linked_parent.relative_to(root)}")
            continue
        source_path = root / pathlib.Path(path)
        if not source_path.exists() and not source_path.is_symlink():
            errors.append(f"missing committed source file: {path}")
        elif source_path.is_symlink():
            errors.append(f"symbolic links are not supported as release source files: {path}")
        elif mode == "120000":
            errors.append(f"release source file changed from a symlink to a regular file: {path}")
        else:
            regular_paths.append(path)

    # Git's stdin-paths mode hashes many files in one process and applies each
    # file's clean filter, including on Windows checkouts with CRLF worktrees.
    # Git paths containing newlines are handled separately because this input
    # protocol is line-delimited.
    batch_paths = [path for path in regular_paths if "\n" not in path and "\r" not in path]
    individual_paths = [path for path in regular_paths if path not in batch_paths]
    actual_objects: dict[str, str] = {}
    if batch_paths:
        try:
            raw_hashes = run_git(
                root,
                "hash-object",
                "--stdin-paths",
                input_bytes=("\n".join(batch_paths) + "\n").encode("utf-8", errors="surrogateescape"),
            )
            hashes = raw_hashes.decode("ascii").splitlines()
            if len(hashes) != len(batch_paths):
                errors.append("Git returned an incomplete source-file hash list")
            else:
                actual_objects.update(zip(batch_paths, hashes))
        except (OSError, subprocess.CalledProcessError, UnicodeDecodeError) as error:
            errors.append(f"cannot hash tracked release source files: {error}")
    for path in individual_paths:
        try:
            actual_objects[path] = run_git(root, "hash-object", f"--path={path}", "--", path).decode("ascii").strip()
        except (OSError, subprocess.CalledProcessError, UnicodeDecodeError) as error:
            errors.append(f"cannot hash release source file {path}: {error}")

    for path, actual_object in actual_objects.items():
        if actual_object != expected[path][1]:
            errors.append(f"release source file differs from {resolved_revision}: {path}")
    return errors


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-root", required=True, type=pathlib.Path)
    parser.add_argument("--revision", default="HEAD")
    parser.add_argument("--all-tracked", action="store_true")
    args = parser.parse_args()
    root = args.source_root.resolve()
    errors = (
        validate_all_tracked_inputs(root, args.revision)
        if args.all_tracked
        else validate_package_inputs(root, args.revision)
    )
    if errors:
        print("Package inputs do not match the selected Git tree:", file=sys.stderr)
        for error in errors:
            print(f"- {error}", file=sys.stderr)
        return 1
    print("Package inputs match the selected Git tree")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
