"""Small standard-library PE import-table reader for package verification."""

from __future__ import annotations

import pathlib
import struct


class PEFormatError(ValueError):
    pass


def _u16(data: bytes, offset: int) -> int:
    if offset < 0 or offset + 2 > len(data):
        raise PEFormatError("truncated PE 16-bit field")
    return struct.unpack_from("<H", data, offset)[0]


def _u32(data: bytes, offset: int) -> int:
    if offset < 0 or offset + 4 > len(data):
        raise PEFormatError("truncated PE 32-bit field")
    return struct.unpack_from("<I", data, offset)[0]


def _rva_offset(rva: int, sections: list[tuple[int, int, int, int]],
                headers_size: int, file_size: int) -> int:
    if rva < headers_size:
        offset = rva
    else:
        offset = -1
        for virtual_address, virtual_size, raw_offset, raw_size in sections:
            span = max(virtual_size, raw_size)
            if virtual_address <= rva < virtual_address + span:
                offset = raw_offset + rva - virtual_address
                break
        if offset < 0:
            raise PEFormatError(f"RVA 0x{rva:x} is outside PE sections")
    if offset >= file_size:
        raise PEFormatError(f"RVA 0x{rva:x} maps beyond the PE file")
    return offset


def imports_from_bytes(data: bytes) -> tuple[str, ...]:
    """Return normal import DLL names, rejecting malformed/truncated images."""
    if len(data) < 0x40 or data[:2] != b"MZ":
        raise PEFormatError("missing DOS MZ header")
    pe_offset = _u32(data, 0x3C)
    if pe_offset + 24 > len(data) or data[pe_offset:pe_offset + 4] != b"PE\0\0":
        raise PEFormatError("missing PE signature or truncated COFF header")
    section_count = _u16(data, pe_offset + 6)
    optional_size = _u16(data, pe_offset + 20)
    optional = pe_offset + 24
    if optional + optional_size > len(data):
        raise PEFormatError("truncated PE optional header")
    magic = _u16(data, optional)
    if magic == 0x10B:
        number_of_directories_offset, directories_offset = 92, 96
    elif magic == 0x20B:
        number_of_directories_offset, directories_offset = 108, 112
    else:
        raise PEFormatError(f"unsupported PE optional-header magic 0x{magic:x}")
    if optional_size < directories_offset:
        raise PEFormatError("PE optional header omits data directories")
    directory_count = _u32(data, optional + number_of_directories_offset)
    import_rva = import_size = 0
    if directory_count > 1:
        if optional_size < directories_offset + 16:
            raise PEFormatError("truncated PE import directory entry")
        import_rva = _u32(data, optional + directories_offset + 8)
        import_size = _u32(data, optional + directories_offset + 12)
    if import_rva == 0 and import_size == 0:
        return ()
    if import_rva == 0 or import_size < 20:
        raise PEFormatError("inconsistent PE import directory")

    headers_size = _u32(data, optional + 60)
    section_table = optional + optional_size
    sections = []
    for index in range(section_count):
        header = section_table + index * 40
        if header + 40 > len(data):
            raise PEFormatError("truncated PE section table")
        virtual_size, virtual_address, raw_size, raw_offset = struct.unpack_from("<IIII", data, header + 8)
        sections.append((virtual_address, virtual_size, raw_offset, raw_size))

    directory_offset = _rva_offset(import_rva, sections, headers_size, len(data))
    limit = min(len(data), directory_offset + import_size)
    names = []
    cursor = directory_offset
    terminated = False
    while cursor + 20 <= limit:
        original_thunk, timestamp, forwarder, name_rva, first_thunk = struct.unpack_from("<IIIII", data, cursor)
        if not (original_thunk or timestamp or forwarder or name_rva or first_thunk):
            terminated = True
            break
        name_offset = _rva_offset(name_rva, sections, headers_size, len(data))
        end = data.find(b"\0", name_offset)
        if end < 0:
            raise PEFormatError("unterminated PE import DLL name")
        try:
            names.append(data[name_offset:end].decode("ascii"))
        except UnicodeDecodeError as error:
            raise PEFormatError("non-ASCII PE import DLL name") from error
        cursor += 20
    if not terminated:
        raise PEFormatError("PE import descriptor table has no terminator")
    return tuple(names)


def imports_from_file(path: pathlib.Path) -> tuple[str, ...]:
    return imports_from_bytes(path.read_bytes())
