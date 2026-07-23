#!/usr/bin/env python3
"""Inspect a captured Sky player manager inside a Windows minidump."""

from __future__ import annotations

import argparse
import json
import struct
from pathlib import Path

from minidump.minidumpfile import MinidumpFile


AVATAR_COUNT = 60
AVATAR_ARRAY_OFFSET = 0x30
AVATAR_STRIDE = 0x10B20
AVATAR_OUTFIT_OFFSET = 0x58
AVATAR_ACTIVE_OFFSET = 0xB850
AVATAR_FLAGS_OFFSET = 0x109EC


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("dump", type=Path)
    parser.add_argument("manager", type=lambda value: int(value, 0))
    parser.add_argument("--only-valid", action="store_true")
    args = parser.parse_args()

    dump = MinidumpFile.parse(str(args.dump))
    reader = dump.get_reader()

    def read(address: int, fmt: str) -> int:
        size = struct.calcsize(fmt)
        return struct.unpack(fmt, reader.read(address, size))[0]

    candidates = []
    for index in range(AVATAR_COUNT):
        avatar = args.manager + AVATAR_ARRAY_OFFSET + index * AVATAR_STRIDE
        errors = {}
        try:
            active = read(avatar + AVATAR_ACTIVE_OFFSET, "<B")
        except Exception as exception:
            active = None
            errors["active"] = str(exception)
        try:
            flags = read(avatar + AVATAR_FLAGS_OFFSET, "<H")
        except Exception as exception:
            flags = None
            errors["flags"] = str(exception)
        try:
            outfit = read(avatar + AVATAR_OUTFIT_OFFSET, "<Q")
            reverse_avatar = read(outfit + 8, "<Q") if outfit else 0
            database = read(outfit + 0x10, "<Q") if outfit else 0
        except Exception as exception:
            outfit = None
            reverse_avatar = None
            database = None
            errors["outfit_chain"] = str(exception)

        structurally_valid = bool(
            active
            and outfit
            and reverse_avatar == avatar
            and database
        )
        if not active and not structurally_valid:
            continue

        candidates.append(
            {
                "index": index,
                "avatar": f"0x{avatar:X}",
                "active": active,
                "flags": f"0x{flags:X}" if flags is not None else None,
                "flags_bit_08": bool(flags & 0x08) if flags is not None else None,
                "outfit": f"0x{outfit:X}" if outfit is not None else None,
                "reverse_avatar": (
                    f"0x{reverse_avatar:X}" if reverse_avatar is not None else None
                ),
                "database": f"0x{database:X}" if database is not None else None,
                "structurally_valid": structurally_valid,
                "valid_local_avatar": bool(
                    structurally_valid and flags is not None and flags & 0x08
                ),
                "errors": errors,
            }
        )

    if args.only_valid:
        candidates = [candidate for candidate in candidates if candidate["structurally_valid"]]

    print(
        json.dumps(
            {
                "dump": args.dump.name,
                "manager": f"0x{args.manager:X}",
                "active_candidate_count": len(candidates),
                "candidates": candidates,
            },
            indent=2,
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
