#!/usr/bin/env python3
"""Independent PE import-table parser fixtures; no Windows runtime required."""

from __future__ import annotations

import pathlib
import struct
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "tools"))
from windows_pe import PEFormatError, imports_from_bytes


def fixture_pe(import_name: str = "Qt6Core.dll") -> bytes:
    data = bytearray(0x400)
    data[0:2] = b"MZ"
    struct.pack_into("<I", data, 0x3C, 0x80)
    data[0x80:0x84] = b"PE\0\0"
    struct.pack_into("<HHIIIHH", data, 0x84, 0x8664, 1, 0, 0, 0, 240, 0x2022)
    optional = 0x98
    struct.pack_into("<H", data, optional, 0x20B)
    struct.pack_into("<I", data, optional + 60, 0x200)
    struct.pack_into("<I", data, optional + 108, 16)
    struct.pack_into("<II", data, optional + 112 + 8, 0x1000, 40)
    section = optional + 240
    struct.pack_into("<IIII", data, section + 8, 0x200, 0x1000, 0x200, 0x200)
    struct.pack_into("<IIIII", data, 0x200, 0x1090, 0, 0, 0x1050, 0x10A0)
    data[0x250:0x250 + len(import_name) + 1] = import_name.encode("ascii") + b"\0"
    return bytes(data)


def must_reject(data: bytes, label: str) -> None:
    try:
        imports_from_bytes(data)
    except PEFormatError:
        return
    raise AssertionError(f"malformed PE fixture was accepted: {label}")


def main() -> int:
    expected = ("Qt6Core.dll",)
    if imports_from_bytes(fixture_pe()) != expected:
        raise AssertionError("PE parser did not map the import RVA to its DLL name")
    must_reject(b"not a PE", "missing DOS header")
    malformed = bytearray(fixture_pe())
    struct.pack_into("<I", malformed, 0x3C, 0x3F0)
    must_reject(bytes(malformed), "truncated PE header")
    malformed = bytearray(fixture_pe())
    struct.pack_into("<I", malformed, 0x20C, 0xFFFF_FFF0)
    must_reject(bytes(malformed), "out-of-range name RVA")
    malformed = bytearray(fixture_pe())
    malformed[0x250:] = b"A" * (len(malformed) - 0x250)
    must_reject(bytes(malformed), "unterminated import name")
    print("Windows PE import parser contract passed: independent import RVA and malformed-header fixtures")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
