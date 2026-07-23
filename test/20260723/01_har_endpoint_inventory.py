#!/usr/bin/env python3
"""Build a credential-free endpoint inventory from a HAR file."""

from __future__ import annotations

import argparse
import json
import re
from collections import defaultdict
from pathlib import Path
from typing import Any
from urllib.parse import parse_qsl, urlsplit


FRIEND_CODE_RE = re.compile(r"(?i)[a-z0-9]{4}(?:-[a-z0-9]{4}){2}")
UUID_RE = re.compile(
    r"(?i)^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$"
)
LONG_HEX_RE = re.compile(r"(?i)^[0-9a-f]{16,}$")
LONG_TOKEN_RE = re.compile(r"^[A-Za-z0-9_+=.-]{24,}$")
LONG_NUMBER_RE = re.compile(r"^\d{7,}$")


def redact_path(path: str) -> str:
    path = FRIEND_CODE_RE.sub(":friend-code", path)
    segments = []
    for segment in path.split("/"):
        if UUID_RE.fullmatch(segment):
            segment = ":uuid"
        elif LONG_HEX_RE.fullmatch(segment):
            segment = ":hex-id"
        elif LONG_NUMBER_RE.fullmatch(segment):
            segment = ":number"
        elif LONG_TOKEN_RE.fullmatch(segment):
            segment = ":opaque-id"
        segments.append(segment)
    return "/".join(segments) or "/"


def value_shape(value: Any, depth: int = 0) -> Any:
    if depth >= 6:
        return "..."
    if value is None:
        return "null"
    if isinstance(value, bool):
        return "boolean"
    if isinstance(value, int):
        return "integer"
    if isinstance(value, float):
        return "number"
    if isinstance(value, str):
        return "string"
    if isinstance(value, list):
        if not value:
            return []
        return [value_shape(value[0], depth + 1)]
    if isinstance(value, dict):
        return {
            str(key): value_shape(child, depth + 1)
            for key, child in sorted(value.items(), key=lambda item: str(item[0]))
        }
    return type(value).__name__


def parse_json_shape(text: str | None, mime_type: str = "") -> Any | None:
    if not text or len(text) > 8 * 1024 * 1024:
        return None
    stripped = text.lstrip()
    if "json" not in mime_type.lower() and not stripped.startswith(("{", "[")):
        return None
    try:
        return value_shape(json.loads(text))
    except (json.JSONDecodeError, UnicodeDecodeError):
        return None


def request_field_names(request: dict[str, Any]) -> tuple[list[str], Any | None]:
    parsed = urlsplit(str(request.get("url", "")))
    query_names = sorted({name for name, _ in parse_qsl(parsed.query, keep_blank_values=True)})

    post_data = request.get("postData") or {}
    post_names = {
        str(param.get("name"))
        for param in post_data.get("params") or []
        if param.get("name") is not None
    }
    mime_type = str(post_data.get("mimeType", ""))
    body_shape = parse_json_shape(post_data.get("text"), mime_type)
    if body_shape is None and "x-www-form-urlencoded" in mime_type.lower():
        post_names.update(
            name for name, _ in parse_qsl(str(post_data.get("text", "")), keep_blank_values=True)
        )
    return sorted(set(query_names) | post_names), body_shape


def build_inventory(har_path: Path) -> dict[str, Any]:
    with har_path.open("r", encoding="utf-8-sig") as stream:
        har = json.load(stream)

    grouped: dict[tuple[str, str, str, int, str], dict[str, Any]] = defaultdict(
        lambda: {
            "count": 0,
            "field_names": set(),
            "request_json_shapes": {},
            "response_json_shapes": {},
        }
    )
    entries = (har.get("log") or {}).get("entries") or []
    for entry in entries:
        request = entry.get("request") or {}
        response = entry.get("response") or {}
        content = response.get("content") or {}
        parsed_url = urlsplit(str(request.get("url", "")))
        method = str(request.get("method", "UNKNOWN")).upper()
        host = (parsed_url.hostname or "").lower()
        path = redact_path(parsed_url.path or "/")
        status = int(response.get("status") or 0)
        content_type = str(content.get("mimeType") or "").split(";", 1)[0].strip().lower()
        key = (method, host, path, status, content_type)
        record = grouped[key]
        record["count"] += 1

        field_names, request_shape = request_field_names(request)
        record["field_names"].update(field_names)
        if request_shape is not None:
            encoded = json.dumps(request_shape, ensure_ascii=False, sort_keys=True)
            record["request_json_shapes"][encoded] = request_shape

        response_shape = parse_json_shape(content.get("text"), str(content.get("mimeType", "")))
        if response_shape is not None:
            encoded = json.dumps(response_shape, ensure_ascii=False, sort_keys=True)
            record["response_json_shapes"][encoded] = response_shape

    endpoints = []
    for key, record in sorted(grouped.items()):
        method, host, path, status, content_type = key
        endpoints.append(
            {
                "method": method,
                "host": host,
                "path": path,
                "status": status,
                "content_type": content_type,
                "count": record["count"],
                "field_names": sorted(record["field_names"]),
                "request_json_shapes": list(record["request_json_shapes"].values()),
                "response_json_shapes": list(record["response_json_shapes"].values()),
            }
        )

    return {
        "source_file": har_path.name,
        "entry_count": len(entries),
        "endpoint_count": len(endpoints),
        "privacy": "Header values, cookies, query values, body values, and dynamic path identifiers are omitted.",
        "endpoints": endpoints,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("har", type=Path, help="Input HAR file")
    parser.add_argument("-o", "--output", type=Path, help="Write JSON inventory to this path")
    parser.add_argument(
        "--match",
        action="append",
        default=[],
        help="Only print endpoints whose host/path/field names contain this text (repeatable)",
    )
    args = parser.parse_args()

    inventory = build_inventory(args.har)
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(
            json.dumps(inventory, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
        )

    matches = [value.lower() for value in args.match]
    printable = inventory
    if matches:
        endpoints = []
        for endpoint in inventory["endpoints"]:
            haystack = " ".join(
                [endpoint["host"], endpoint["path"], *endpoint["field_names"]]
            ).lower()
            if any(value in haystack for value in matches):
                endpoints.append(endpoint)
        printable = {**inventory, "endpoint_count": len(endpoints), "endpoints": endpoints}

    print(json.dumps(printable, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
