#!/usr/bin/env python3
"""Find x64 RIP-relative and immediate references to RVAs in a PE memory image."""

from __future__ import annotations

import argparse
import json
import re
import struct
from collections import deque
from pathlib import Path
from typing import Any

from capstone import CS_ARCH_X86, CS_MODE_64, Cs


IMAGE_SCN_MEM_EXECUTE = 0x20000000
RIP_OPERAND_RE = re.compile(r"\brip\s*([+-])\s*(0x[0-9a-f]+|\d+)", re.IGNORECASE)
IMMEDIATE_RE = re.compile(r"(?<![\w])0x[0-9a-f]+(?![\w])", re.IGNORECASE)


def read_pe(data: bytes) -> tuple[int, list[dict[str, int | str]]]:
    if data[:2] != b"MZ":
        raise ValueError("input does not start with an MZ header")
    pe_offset = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe_offset : pe_offset + 4] != b"PE\0\0":
        raise ValueError("PE signature not found")

    file_header = pe_offset + 4
    section_count = struct.unpack_from("<H", data, file_header + 2)[0]
    optional_size = struct.unpack_from("<H", data, file_header + 16)[0]
    optional_header = file_header + 20
    magic = struct.unpack_from("<H", data, optional_header)[0]
    if magic != 0x20B:
        raise ValueError(f"expected PE32+ optional header, got 0x{magic:X}")
    image_base = struct.unpack_from("<Q", data, optional_header + 24)[0]

    sections = []
    section_table = optional_header + optional_size
    for index in range(section_count):
        offset = section_table + index * 40
        name = data[offset : offset + 8].split(b"\0", 1)[0].decode("ascii", "replace")
        virtual_size, virtual_address, raw_size, raw_offset = struct.unpack_from(
            "<IIII", data, offset + 8
        )
        characteristics = struct.unpack_from("<I", data, offset + 36)[0]
        sections.append(
            {
                "name": name,
                "virtual_size": virtual_size,
                "virtual_address": virtual_address,
                "raw_size": raw_size,
                "raw_offset": raw_offset,
                "characteristics": characteristics,
            }
        )
    return image_base, sections


def parse_target(value: str) -> tuple[str, int]:
    if "=" in value:
        name, raw_rva = value.split("=", 1)
    else:
        name, raw_rva = value, value
    return name, int(raw_rva, 0)


def instruction_dict(
    address: int, mnemonic: str, operands: str, module_base: int
) -> dict[str, str]:
    return {
        "rva": f"0x{address - module_base:X}",
        "va": f"0x{address:X}",
        "instruction": f"{mnemonic} {operands}".rstrip(),
    }


def find_references(
    data: bytes,
    sections: list[dict[str, int | str]],
    module_base: int,
    targets: dict[int, list[str]],
    layout: str,
    context: int,
) -> list[dict[str, Any]]:
    disassembler = Cs(CS_ARCH_X86, CS_MODE_64)
    disassembler.detail = False
    disassembler.skipdata = True
    results: list[dict[str, Any]] = []

    for section in sections:
        characteristics = int(section["characteristics"])
        if not characteristics & IMAGE_SCN_MEM_EXECUTE:
            continue
        rva = int(section["virtual_address"])
        if layout == "memory":
            data_offset = rva
            size = int(section["virtual_size"])
        else:
            data_offset = int(section["raw_offset"])
            size = int(section["raw_size"])
        section_data = data[data_offset : data_offset + size]

        history: deque[dict[str, str]] = deque(maxlen=context)
        pending: list[dict[str, Any]] = []
        for address, size, mnemonic, operands in disassembler.disasm_lite(
            section_data, module_base + rva
        ):
            rendered = instruction_dict(address, mnemonic, operands, module_base)
            for result in list(pending):
                result["context_after"].append(rendered)
                result["remaining"] -= 1
                if result["remaining"] <= 0:
                    pending.remove(result)

            referenced: set[int] = set()
            for match in RIP_OPERAND_RE.finditer(operands):
                displacement = int(match.group(2), 0)
                if match.group(1) == "-":
                    displacement = -displacement
                referenced.add(address + size + displacement)
            for match in IMMEDIATE_RE.finditer(operands):
                referenced.add(int(match.group(0), 0))

            for target_va in sorted(referenced & targets.keys()):
                result = {
                    "target_names": targets[target_va],
                    "target_rva": f"0x{target_va - module_base:X}",
                    "target_va": f"0x{target_va:X}",
                    "section": section["name"],
                    "reference": rendered,
                    "context_before": list(history),
                    "context_after": [],
                    "remaining": context,
                }
                results.append(result)
                if context:
                    pending.append(result)
            history.append(rendered)

    for result in results:
        result.pop("remaining", None)
    return results


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", type=Path)
    parser.add_argument("--target", action="append", required=True, help="NAME=RVA or RVA")
    parser.add_argument("--module-base", type=lambda value: int(value, 0))
    parser.add_argument("--layout", choices=("memory", "file"), default="memory")
    parser.add_argument("--context", type=int, default=6)
    parser.add_argument("-o", "--output", type=Path)
    args = parser.parse_args()

    data = args.image.read_bytes()
    pe_image_base, sections = read_pe(data)
    module_base = args.module_base if args.module_base is not None else pe_image_base
    targets: dict[int, list[str]] = {}
    target_rows = []
    for raw_target in args.target:
        name, rva = parse_target(raw_target)
        targets.setdefault(module_base + rva, []).append(name)
        target_rows.append({"name": name, "rva": f"0x{rva:X}", "va": f"0x{module_base + rva:X}"})

    references = find_references(
        data, sections, module_base, targets, args.layout, max(0, args.context)
    )
    result = {
        "source_file": args.image.name,
        "module_base": f"0x{module_base:X}",
        "layout": args.layout,
        "targets": target_rows,
        "reference_count": len(references),
        "references": references,
    }
    rendered = json.dumps(result, ensure_ascii=False, indent=2) + "\n"
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(rendered, encoding="utf-8")
    print(rendered, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
