#!/usr/bin/env python3
"""Find little-endian pointer references in a contiguous virtual-memory dump."""

from __future__ import annotations

import argparse
import json
import mmap
import struct
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("dump", type=Path)
    parser.add_argument("target", type=lambda value: int(value, 0))
    parser.add_argument("--file-base", type=lambda value: int(value, 0), required=True)
    parser.add_argument("--pointer-size", type=int, choices=(4, 8), default=8)
    parser.add_argument("--max-results", type=int, default=256)
    parser.add_argument("--container-offset", type=lambda value: int(value, 0), default=0)
    parser.add_argument("--vtable-min", type=lambda value: int(value, 0))
    parser.add_argument("--vtable-max", type=lambda value: int(value, 0))
    parser.add_argument("--location-min", type=lambda value: int(value, 0))
    parser.add_argument("--location-max", type=lambda value: int(value, 0))
    args = parser.parse_args()

    pattern = struct.pack("<Q" if args.pointer_size == 8 else "<I", args.target)
    matches = []
    with args.dump.open("rb") as stream, mmap.mmap(stream.fileno(), 0, access=mmap.ACCESS_READ) as image:
        position = 0
        while len(matches) < args.max_results:
            position = image.find(pattern, position)
            if position < 0:
                break
            virtual_address = args.file_base + position
            if args.location_min is not None and virtual_address < args.location_min:
                position += 1
                continue
            if args.location_max is not None and virtual_address >= args.location_max:
                position += 1
                continue
            container_position = position - args.container_offset
            first_qword = None
            if args.container_offset:
                if container_position < 0 or container_position + 8 > len(image):
                    position += 1
                    continue
                first_qword = struct.unpack_from("<Q", image, container_position)[0]
                if args.vtable_min is not None and first_qword < args.vtable_min:
                    position += 1
                    continue
                if args.vtable_max is not None and first_qword >= args.vtable_max:
                    position += 1
                    continue
            matches.append(
                {
                    "file_offset": f"0x{position:X}",
                    "virtual_address": f"0x{virtual_address:X}",
                    "container_address": f"0x{args.file_base + container_position:X}",
                    "container_first_qword": f"0x{first_qword:X}" if first_qword is not None else None,
                }
            )
            position += 1

    print(
        json.dumps(
            {
                "source_file": args.dump.name,
                "file_base": f"0x{args.file_base:X}",
                "target": f"0x{args.target:X}",
                "match_count": len(matches),
                "matches": matches,
            },
            indent=2,
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
