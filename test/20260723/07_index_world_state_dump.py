#!/usr/bin/env python3
"""Index memory regions and world-related strings from a full Sky minidump."""

from __future__ import annotations

import argparse
import json
from collections import defaultdict
from pathlib import Path

from minidump.minidumpfile import MinidumpFile


KEYWORDS = (
    "rain",
    "forest",
    "candle",
    "wax",
    "flame",
    "light",
    "collectible",
    "pickup",
    "currency",
    "shard",
    "orb",
    "scene",
    "world",
    "level",
    "map",
)
CHUNK_SIZE = 8 * 1024 * 1024
OVERLAP = 512
MAX_MATCHES_PER_KEYWORD = 2000


def enum_name(value: object) -> str | None:
    if value is None:
        return None
    return getattr(value, "name", None) or str(value).split(".")[-1]


def printable_ascii(byte: int) -> bool:
    return 0x20 <= byte <= 0x7E


def extract_ascii(data: bytes, position: int) -> str:
    start = position
    while start > 0 and printable_ascii(data[start - 1]) and position - start < 256:
        start -= 1
    end = position
    while end < len(data) and printable_ascii(data[end]) and end - position < 512:
        end += 1
    return data[start:end].decode("ascii", "replace")


def extract_utf16(data: bytes, position: int) -> tuple[int, str]:
    start = position - position % 2
    while (
        start >= 2
        and printable_ascii(data[start - 2])
        and data[start - 1] == 0
        and position - start < 512
    ):
        start -= 2
    end = position - position % 2
    while (
        end + 1 < len(data)
        and printable_ascii(data[end])
        and data[end + 1] == 0
        and end - position < 1024
    ):
        end += 2
    return start, data[start:end].decode("utf-16-le", "replace")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("dump", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)

    dump = MinidumpFile.parse(str(args.dump))
    reader = dump.get_reader()
    memory_regions = []
    committed_private = []
    for info in dump.memory_info.infos:
        row = {
            "base": f"0x{info.BaseAddress:X}",
            "allocationBase": f"0x{info.AllocationBase:X}",
            "regionSize": info.RegionSize,
            "state": enum_name(info.State),
            "protect": enum_name(info.Protect),
            "allocationProtect": enum_name(info.AllocationProtect),
            "type": enum_name(info.Type),
        }
        memory_regions.append(row)
        if row["state"] == "MEM_COMMIT" and row["type"] == "MEM_PRIVATE":
            committed_private.append(info)

    (args.output / "memory-regions.json").write_text(
        json.dumps(memory_regions, indent=2) + "\n", encoding="utf-8"
    )

    counts: dict[str, int] = defaultdict(int)
    matches: dict[tuple[str, int, str], dict[str, object]] = {}
    unreadable_regions = []
    scanned_bytes = 0
    for info in committed_private:
        region_offset = 0
        while region_offset < info.RegionSize:
            size = min(CHUNK_SIZE + OVERLAP, info.RegionSize - region_offset)
            address = info.BaseAddress + region_offset
            try:
                data = reader.read(address, size)
            except Exception as exception:
                unreadable_regions.append(
                    {"address": f"0x{address:X}", "size": size, "error": str(exception)}
                )
                break
            scanned_bytes += min(CHUNK_SIZE, info.RegionSize - region_offset)
            lowered = data.lower()
            for keyword in KEYWORDS:
                if counts[keyword] >= MAX_MATCHES_PER_KEYWORD:
                    continue
                for encoding, pattern in (
                    ("ascii", keyword.encode("ascii")),
                    ("utf16", keyword.encode("utf-16-le")),
                ):
                    position = 0
                    while counts[keyword] < MAX_MATCHES_PER_KEYWORD:
                        position = lowered.find(pattern, position)
                        if position < 0:
                            break
                        if encoding == "ascii":
                            text = extract_ascii(data, position)
                            text_start = position - max(0, text.lower().find(keyword))
                        else:
                            text_start, text = extract_utf16(data, position)
                        if keyword in text.lower() and len(text) >= 4:
                            virtual_address = address + text_start
                            key = (encoding, virtual_address, text)
                            if key not in matches:
                                found_keywords = [item for item in KEYWORDS if item in text.lower()]
                                matches[key] = {
                                    "address": f"0x{virtual_address:X}",
                                    "encoding": encoding,
                                    "keywords": found_keywords,
                                    "text": text,
                                }
                                for item in found_keywords:
                                    counts[item] += 1
                        position += max(1, len(pattern))
            region_offset += CHUNK_SIZE

    ordered_matches = sorted(matches.values(), key=lambda item: int(str(item["address"]), 16))
    (args.output / "world-string-index.json").write_text(
        json.dumps(ordered_matches, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    captured_segments = dump.memory_segments_64.memory_segments
    summary = {
        "dump": args.dump.name,
        "memoryRegionCount": len(memory_regions),
        "capturedSegmentCount": len(captured_segments),
        "capturedBytes": sum(segment.size for segment in captured_segments),
        "committedPrivateRegionCount": len(committed_private),
        "committedPrivateBytes": sum(info.RegionSize for info in committed_private),
        "scannedPrivateBytes": scanned_bytes,
        "worldStringCount": len(ordered_matches),
        "keywordCounts": dict(sorted(counts.items())),
        "unreadableRegionCount": len(unreadable_regions),
        "unreadableBytes": sum(item["size"] for item in unreadable_regions),
        "unreadableRegionSamples": unreadable_regions[:64],
    }
    (args.output / "dump-summary.json").write_text(
        json.dumps(summary, indent=2) + "\n", encoding="utf-8"
    )
    print(json.dumps(summary, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
