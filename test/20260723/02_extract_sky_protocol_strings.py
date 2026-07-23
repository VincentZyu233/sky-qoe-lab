#!/usr/bin/env python3
"""Extract keyword-matched protocol strings from a Sky module memory image."""

from __future__ import annotations

import argparse
import json
import re
from pathlib import Path
from typing import Iterable


DEFAULT_KEYWORDS = (
    "friend",
    "invite",
    "relationship",
    "outfit",
    "remote",
    "height",
    "account",
    "code",
)
ASCII_STRING_RE = re.compile(rb"[\x20-\x7e]{4,}")
FRIEND_CODE_RE = re.compile(r"(?i)\b[a-z0-9]{4}(?:-[a-z0-9]{4}){2}\b")
UUID_RE = re.compile(
    r"(?i)\b[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}\b"
)
JWT_RE = re.compile(r"\beyJ[A-Za-z0-9_-]{12,}\.[A-Za-z0-9_-]{8,}(?:\.[A-Za-z0-9_-]{8,})?\b")
BEARER_RE = re.compile(r"(?i)\bBearer\s+[A-Za-z0-9._~+/=-]{12,}")


def redact(text: str) -> str:
    text = FRIEND_CODE_RE.sub(":friend-code", text)
    text = UUID_RE.sub(":uuid", text)
    text = JWT_RE.sub(":jwt", text)
    return BEARER_RE.sub("Bearer :token", text)


def crop_match(text: str, matcher: re.Pattern[str], max_length: int) -> str:
    if len(text) <= max_length:
        return text
    match = matcher.search(text)
    center = match.start() if match else 0
    start = max(0, center - max_length // 3)
    end = min(len(text), start + max_length)
    start = max(0, end - max_length)
    return ("..." if start else "") + text[start:end] + ("..." if end < len(text) else "")


def utf16le_strings(data: bytes, minimum: int) -> Iterable[tuple[int, str]]:
    pattern = re.compile(rb"(?:[\x20-\x7e]\x00){" + str(minimum).encode("ascii") + rb",}")
    for match in pattern.finditer(data):
        yield match.start(), match.group().decode("utf-16-le")


def extract(
    image_path: Path,
    keywords: list[str],
    module_base: int,
    minimum: int,
    max_length: int,
) -> dict[str, object]:
    data = image_path.read_bytes()
    matcher = re.compile("|".join(re.escape(keyword) for keyword in keywords), re.IGNORECASE)
    records = []

    candidates: list[tuple[int, str, str]] = [
        (match.start(), "ascii", match.group().decode("ascii"))
        for match in ASCII_STRING_RE.finditer(data)
        if len(match.group()) >= minimum
    ]
    candidates.extend(
        (offset, "utf-16-le", text) for offset, text in utf16le_strings(data, minimum)
    )

    for offset, encoding, text in candidates:
        if not matcher.search(text):
            continue
        text = redact(crop_match(text, matcher, max_length))
        records.append(
            {
                "file_offset": f"0x{offset:X}",
                "rva": f"0x{offset:X}",
                "va": f"0x{module_base + offset:X}",
                "encoding": encoding,
                "text": text,
            }
        )

    records.sort(key=lambda item: (int(item["file_offset"], 16), item["encoding"]))
    return {
        "source_file": image_path.name,
        "module_base": f"0x{module_base:X}",
        "keywords": keywords,
        "match_count": len(records),
        "privacy": "Friend codes, UUIDs, JWTs, and bearer tokens are redacted.",
        "matches": records,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", type=Path, help="Module-layout image, such as Sky_dump.bin")
    parser.add_argument("-o", "--output", type=Path, help="Write JSON results to this path")
    parser.add_argument("--keyword", action="append", dest="keywords", default=[])
    parser.add_argument("--module-base", type=lambda value: int(value, 0), default=0x140000000)
    parser.add_argument("--minimum", type=int, default=4)
    parser.add_argument("--max-length", type=int, default=320)
    args = parser.parse_args()

    keywords = args.keywords or list(DEFAULT_KEYWORDS)
    result = extract(args.image, keywords, args.module_base, args.minimum, args.max_length)
    rendered = json.dumps(result, ensure_ascii=False, indent=2) + "\n"
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(rendered, encoding="utf-8")
    print(rendered, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
