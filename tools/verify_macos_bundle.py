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


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--contents", required=True, type=pathlib.Path)
    args = parser.parse_args()
    contents = args.contents.resolve()
    app_exec = contents / "MacOS/AxisSim"
    if not app_exec.is_file():
        raise SystemExit(f"missing app executable: {app_exec}")
    executable_dir = app_exec.parent
    binaries = []
    for path in sorted(contents.rglob("*")):
        if not path.is_file():
            continue
        probe = subprocess.run(["file", "-b", str(path)], capture_output=True, text=True)
        if probe.returncode == 0 and "Mach-O" in probe.stdout:
            binaries.append(path)

    checked = 0
    unresolved = []
    for binary in binaries:
        lines = output("otool", "-L", str(binary)).splitlines()[1:]
        load_commands = output("otool", "-l", str(binary))
        rpaths = re.findall(r"\s+path (.+?) \(offset \d+\)", load_commands)
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
            else:
                candidates.append(expand(dependency, binary, executable_dir))
            resolved = next((candidate for candidate in candidates if candidate.exists()), None)
            if resolved is None:
                unresolved.append(f"{binary.relative_to(contents)} -> {dependency}")
            elif str(resolved).startswith(("/opt/homebrew/", "/usr/local/")):
                unresolved.append(f"{binary.relative_to(contents)} -> developer-prefix dependency {dependency}")
            checked += 1

    if unresolved:
        raise SystemExit("unresolved or developer-local Mach-O dependencies:\n" + "\n".join(unresolved))
    if not binaries:
        raise SystemExit("no Mach-O binaries were found in the app bundle")
    print(f"macOS dependency closure verified: {len(binaries)} Mach-O files, {checked} load references; no developer-prefix dependency")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
