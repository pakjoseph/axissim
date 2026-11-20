#!/usr/bin/env python3
"""Check that a macOS app's actual Mach-O references resolve inside the bundle/system."""

from __future__ import annotations

import argparse
import pathlib
import re
import subprocess


def output(*command: str) -> str:
    return subprocess.run(command, check=True, capture_output=True, text=True).stdout


def expand(value: str, binary: pathlib.Path, executable_dir: pathlib.Path) -> pathlib.Path:
    if value.startswith("@loader_path"):
        return (binary.parent / value[len("@loader_path/"):]).resolve()
    if value.startswith("@executable_path"):
        return (executable_dir / value[len("@executable_path/"):]).resolve()
    return pathlib.Path(value).resolve()


def is_macho(path: pathlib.Path) -> bool:
    probe = subprocess.run(["file", "-b", str(path)], capture_output=True, text=True)
    return probe.returncode == 0 and "Mach-O" in probe.stdout


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--contents", required=True, type=pathlib.Path)
    args = parser.parse_args()
    contents = args.contents.resolve()
    app_exec = contents / "MacOS/AxisSim"
    if not app_exec.is_file():
        raise SystemExit(f"missing app executable: {app_exec}")
    executable_dir = app_exec.parent
    binaries: dict[pathlib.Path, pathlib.Path] = {}
    escaped_binaries = []
    for path in sorted(contents.rglob("*")):
        if not path.is_file():
            continue
        if is_macho(path):
            resolved = path.resolve()
            try:
                resolved.relative_to(contents)
            except ValueError:
                escaped_binaries.append(f"{path.relative_to(contents)} -> {resolved}")
                continue
            binaries.setdefault(resolved, path)

    checked = 0
    unresolved = list(escaped_binaries)
    visited: set[pathlib.Path] = set()
    bundled_frameworks = contents / "Frameworks"

    def visit(binary: pathlib.Path, executable_dir: pathlib.Path) -> None:
        nonlocal checked
        binary = binary.resolve()
        if binary in visited:
            return
        visited.add(binary)
        binary_type = subprocess.run(["file", "-b", str(binary)],
                                     capture_output=True, text=True, check=True).stdout
        lines = output("otool", "-L", str(binary)).splitlines()[1:]
        # A dylib's first listed path is its own install ID, not a dependency.
        if "dynamically linked shared library" in binary_type:
            lines = lines[1:]
        load_commands = output("otool", "-l", str(binary))
        rpaths = []
        load_lines = load_commands.splitlines()
        for index, line in enumerate(load_lines[:-1]):
            if line.strip() == "cmd LC_RPATH":
                match = re.search(r"path (.+?) \(offset \d+\)", load_lines[index + 2])
                if match:
                    rpaths.append(match.group(1))

        for line in lines:
            dependency = line.strip().split(" ", 1)[0]
            if dependency.startswith(("/System/Library/", "/usr/lib/")):
                checked += 1
                continue
            candidates = []
            if dependency.startswith("@rpath/"):
                suffix = dependency[len("@rpath/"):]
                for rpath in rpaths:
                    candidates.append(expand(rpath, binary, executable_dir) / suffix)
                # The app's Qt deployment layout places bundled frameworks and
                # native dylibs at this directory. This candidate also checks
                # transitive dependencies whose runpath is inherited from a
                # parent loader rather than repeated on the immediate dylib.
                candidates.append(bundled_frameworks / suffix)
            else:
                candidates.append(expand(dependency, binary, executable_dir))
            resolved = next((candidate.resolve() for candidate in candidates if candidate.exists()), None)
            if resolved is None:
                unresolved.append(f"{binary.relative_to(contents)} -> {dependency}")
            elif not resolved.is_relative_to(contents):
                unresolved.append(f"{binary.relative_to(contents)} -> external dependency {dependency}: {resolved}")
            else:
                checked += 1
                if is_macho(resolved):
                    visit(resolved, executable_dir)

    roots = [app_exec, contents / "MacOS/axissim-cli"]
    roots.extend(path for path in contents.joinpath("PlugIns").rglob("*") if path.is_file() and is_macho(path))
    for root in roots:
        if root.is_file() and is_macho(root):
            # @executable_path is anchored to the process image, including
            # when resolving a plug-in's load commands. Plug-ins are loaded
            # into AxisSim, so use the application executable's directory.
            visit(root, executable_dir)

    if unresolved:
        raise SystemExit("unresolved or developer-local Mach-O dependencies:\n" + "\n".join(unresolved))
    if not binaries:
        raise SystemExit("no Mach-O binaries were found in the app bundle")
    print(f"macOS dependency closure verified: {len(binaries)} Mach-O files, {checked} load references; no developer-prefix dependency")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
