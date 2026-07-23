#!/usr/bin/env python3
"""Create a focused scene and wax-element index from a world string scan."""

from __future__ import annotations

import argparse
import json
import re
from pathlib import Path


RULES = (
    (
        "scene",
        re.compile(r"Levels/.+\.level|RainForest|Hidden Forest", re.IGNORECASE),
    ),
    (
        "wax_runtime",
        re.compile(
            r"Create\s*Wax|Pop\s*Wax|Spawn\s*Wax|ActivateWax|OnWax|WaxChunk|WaxPickup",
            re.IGNORECASE,
        ),
    ),
    (
        "wax_behavior",
        re.compile(
            r"kWax(?:Chunk|Pickup)|autoCollectWax|collectAllWax|onWaxSpawn|waxSpawn|waxPerSpawn",
            re.IGNORECASE,
        ),
    ),
    (
        "collectible",
        re.compile(r"collectible.*wax|wax.*collectible|collect.*wax|wax.*pickup", re.IGNORECASE),
    ),
)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("index", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()

    entries = json.loads(args.index.read_text(encoding="utf-8"))
    seen = set()
    selected = []
    counts = {category: 0 for category, _ in RULES}
    for entry in entries:
        text = entry["text"]
        if len(text) > 512:
            continue
        categories = [category for category, pattern in RULES if pattern.search(text)]
        if not categories:
            continue
        key = text.casefold()
        if key in seen:
            continue
        seen.add(key)
        for category in categories:
            counts[category] += 1
        selected.append({**entry, "categories": categories})

    result = {
        "source": args.index.name,
        "count": len(selected),
        "categoryCounts": counts,
        "entries": selected,
    }
    args.output.write_text(
        json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    print(json.dumps({"count": len(selected), "categoryCounts": counts}, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
