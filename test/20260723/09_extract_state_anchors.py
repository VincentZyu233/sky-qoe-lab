#!/usr/bin/env python3
"""Extract stable player-state anchor regions from a full Sky minidump."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path

from minidump.minidumpfile import MinidumpFile


def parse_address(value: str) -> int:
    return int(value, 0)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("dump", type=Path)
    parser.add_argument("snapshot", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)

    snapshot = json.loads(args.snapshot.read_text(encoding="utf-8"))
    regions = (
        ("avatar", parse_address(snapshot["avatar"]), 0x10B20),
        ("transform", parse_address(snapshot["transform"]["address"]), 0x200),
        ("outfit", parse_address(snapshot["outfit"]), 0x2000),
        ("outfit-database-header", parse_address(snapshot["database"]), 0x6000),
    )
    reader = MinidumpFile.parse(str(args.dump)).get_reader()
    manifest = []
    for name, address, size in regions:
        data = reader.read(address, size)
        filename = f"{name}-0x{address:X}-0x{size:X}.bin"
        (args.output / filename).write_bytes(data)
        manifest.append(
            {
                "name": name,
                "address": f"0x{address:X}",
                "size": size,
                "file": filename,
                "sha256": hashlib.sha256(data).hexdigest().upper(),
            }
        )

    (args.output / "anchors.json").write_text(
        json.dumps(manifest, indent=2) + "\n", encoding="utf-8"
    )
    print(json.dumps(manifest, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
