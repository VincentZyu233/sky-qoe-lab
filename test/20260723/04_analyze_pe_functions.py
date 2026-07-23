#!/usr/bin/env python3
"""Analyze selected x64 PE functions and resolve their RIP-relative string references."""

from __future__ import annotations

import argparse
import json
import re
import struct
from pathlib import Path
from typing import Any

from capstone import CS_ARCH_X86, CS_MODE_64, Cs


RIP_OPERAND_RE = re.compile(r"\brip\s*([+-])\s*(0x[0-9a-f]+|\d+)", re.IGNORECASE)
CALL_TARGET_RE = re.compile(r"^0x[0-9a-f]+$", re.IGNORECASE)
FRIEND_CODE_RE = re.compile(r"(?i)\b[a-z0-9]{4}(?:-[a-z0-9]{4}){2}\b")
UUID_RE = re.compile(
    r"(?i)\b[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}\b"
)


def parse_pe(data: bytes) -> tuple[int, list[dict[str, int | str]]]:
    pe_offset = struct.unpack_from("<I", data, 0x3C)[0]
    if data[:2] != b"MZ" or data[pe_offset : pe_offset + 4] != b"PE\0\0":
        raise ValueError("input is not a PE memory image")
    file_header = pe_offset + 4
    section_count = struct.unpack_from("<H", data, file_header + 2)[0]
    optional_size = struct.unpack_from("<H", data, file_header + 16)[0]
    optional_header = file_header + 20
    if struct.unpack_from("<H", data, optional_header)[0] != 0x20B:
        raise ValueError("only PE32+ images are supported")
    image_base = struct.unpack_from("<Q", data, optional_header + 24)[0]
    section_table = optional_header + optional_size
    sections = []
    for index in range(section_count):
        offset = section_table + index * 40
        name = data[offset : offset + 8].split(b"\0", 1)[0].decode("ascii", "replace")
        virtual_size, virtual_address = struct.unpack_from("<II", data, offset + 8)
        sections.append(
            {"name": name, "virtual_size": virtual_size, "virtual_address": virtual_address}
        )
    return image_base, sections


def runtime_functions(
    data: bytes, sections: list[dict[str, int | str]]
) -> list[tuple[int, int, int]]:
    section = next((item for item in sections if item["name"] == ".pdata"), None)
    if section is None:
        raise ValueError(".pdata section not found")
    start = int(section["virtual_address"])
    stop = start + int(section["virtual_size"])
    records = []
    for offset in range(start, stop - 11, 12):
        begin, end, unwind = struct.unpack_from("<III", data, offset)
        if begin and end > begin:
            records.append((begin, end, unwind))
    records.sort()
    return records


def find_function(records: list[tuple[int, int, int]], rva: int) -> tuple[int, int, int]:
    for record in records:
        if record[0] <= rva < record[1]:
            return record
    raise ValueError(f"no .pdata function contains RVA 0x{rva:X}")


def parse_named_rva(value: str) -> tuple[str, int]:
    if "=" in value:
        name, raw_rva = value.split("=", 1)
    else:
        name, raw_rva = value, value
    return name, int(raw_rva, 0)


def redact(value: str) -> str:
    return UUID_RE.sub(":uuid", FRIEND_CODE_RE.sub(":friend-code", value))


def static_string(data: bytes, rva: int, max_length: int = 512) -> tuple[str, str] | None:
    if rva < 0 or rva >= len(data):
        return None
    raw = data[rva : rva + max_length]
    ascii_end = raw.find(b"\0")
    if ascii_end >= 4:
        candidate = raw[:ascii_end]
        if all(0x20 <= byte <= 0x7E or byte in (9, 10, 13) for byte in candidate):
            return "ascii", redact(candidate.decode("ascii", "replace"))

    chars = []
    for index in range(0, len(raw) - 1, 2):
        codepoint = raw[index] | (raw[index + 1] << 8)
        if codepoint == 0:
            break
        if codepoint < 0x20 or codepoint > 0x7E:
            chars = []
            break
        chars.append(chr(codepoint))
    if len(chars) >= 4:
        return "utf-16-le", redact("".join(chars))
    return None


def analyze_function(
    data: bytes,
    module_base: int,
    records: list[tuple[int, int, int]],
    name: str,
    requested_rva: int,
    include_full: bool,
) -> dict[str, Any]:
    begin, end, unwind = find_function(records, requested_rva)
    disassembler = Cs(CS_ARCH_X86, CS_MODE_64)
    disassembler.skipdata = True
    strings = []
    calls = []
    instructions = []
    seen_strings: set[tuple[int, int]] = set()

    for address, size, mnemonic, operands in disassembler.disasm_lite(
        data[begin:end], module_base + begin
    ):
        rva = address - module_base
        rendered = f"{mnemonic} {operands}".rstrip()
        if include_full:
            instructions.append({"rva": f"0x{rva:X}", "instruction": rendered})

        for match in RIP_OPERAND_RE.finditer(operands):
            displacement = int(match.group(2), 0)
            if match.group(1) == "-":
                displacement = -displacement
            target_va = address + size + displacement
            target_rva = target_va - module_base
            resolved = static_string(data, target_rva)
            if resolved and (rva, target_rva) not in seen_strings:
                seen_strings.add((rva, target_rva))
                encoding, text = resolved
                strings.append(
                    {
                        "reference_rva": f"0x{rva:X}",
                        "instruction": rendered,
                        "target_rva": f"0x{target_rva:X}",
                        "encoding": encoding,
                        "text": text,
                    }
                )

        if mnemonic == "call" and CALL_TARGET_RE.fullmatch(operands):
            target_va = int(operands, 0)
            calls.append(
                {
                    "rva": f"0x{rva:X}",
                    "target_rva": f"0x{target_va - module_base:X}",
                }
            )

    result: dict[str, Any] = {
        "name": name,
        "requested_rva": f"0x{requested_rva:X}",
        "function_begin_rva": f"0x{begin:X}",
        "function_end_rva": f"0x{end:X}",
        "unwind_rva": f"0x{unwind:X}",
        "size": end - begin,
        "string_references": strings,
        "direct_calls": calls,
    }
    if include_full:
        result["instructions"] = instructions
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", type=Path)
    parser.add_argument("--function", action="append", required=True, help="NAME=RVA or RVA")
    parser.add_argument("--module-base", type=lambda value: int(value, 0))
    parser.add_argument("--full", action="store_true", help="Include complete function disassembly")
    parser.add_argument("-o", "--output", type=Path)
    args = parser.parse_args()

    data = args.image.read_bytes()
    pe_image_base, sections = parse_pe(data)
    module_base = args.module_base if args.module_base is not None else pe_image_base
    records = runtime_functions(data, sections)
    functions = [
        analyze_function(data, module_base, records, *parse_named_rva(value), args.full)
        for value in args.function
    ]
    result = {
        "source_file": args.image.name,
        "module_base": f"0x{module_base:X}",
        "functions": functions,
    }
    rendered = json.dumps(result, ensure_ascii=False, indent=2) + "\n"
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(rendered, encoding="utf-8")
    print(rendered, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
