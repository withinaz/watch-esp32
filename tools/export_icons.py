#!/usr/bin/env python3
"""Export the launcher icons (main/icons/*.c) to PNG for the documentation.

The .c files hold a 150x150 LVGL ARGB8888 buffer as uint32_t words in the
0xAARRGGBB order (memory bytes: B, G, R, A). This script reads those arrays
back and writes straight RGBA PNGs, so docs/img/*.png is exactly what the
watch draws -- nothing is redrawn or approximated.

    python tools/export_icons.py            # writes docs/img/icon-*.png

No third-party dependency: PNG is assembled with zlib.
"""

from __future__ import annotations

import re
import struct
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ICON_DIR = ROOT / "main" / "icons"
OUT_DIR = ROOT / "docs" / "img"

SIZE = 150
ARRAY_RE = re.compile(r"app_icon_(\w+)_map\[(\d+)\]\s*=\s*\{(.*?)\};", re.S)
WORD_RE = re.compile(r"0x([0-9A-Fa-f]{8})")


def read_words(text: str) -> list[int]:
    match = ARRAY_RE.search(text)
    if not match:
        raise SystemExit("no app_icon_*_map array found")
    words = [int(w, 16) for w in WORD_RE.findall(match.group(3))]
    declared = int(match.group(2))
    if len(words) != declared:
        raise SystemExit(f"declared {declared} words, parsed {len(words)}")
    if len(words) != SIZE * SIZE:
        raise SystemExit(f"expected {SIZE * SIZE} words, got {len(words)}")
    return words


def to_rgba(words: list[int]) -> bytearray:
    """0xAARRGGBB -> RGBA bytes, row by row."""
    out = bytearray(SIZE * SIZE * 4)
    for i, word in enumerate(words):
        a = (word >> 24) & 0xFF
        r = (word >> 16) & 0xFF
        g = (word >> 8) & 0xFF
        b = word & 0xFF
        o = i * 4
        out[o] = r
        out[o + 1] = g
        out[o + 2] = b
        out[o + 3] = a
    return out


def write_png(path: Path, rgba: bytearray) -> None:
    raw = b"".join(
        b"\x00" + bytes(rgba[y * SIZE * 4 : (y + 1) * SIZE * 4]) for y in range(SIZE)
    )

    def chunk(kind: bytes, payload: bytes) -> bytes:
        body = kind + payload
        return (
            struct.pack(">I", len(payload))
            + body
            + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)
        )

    png = (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", SIZE, SIZE, 8, 6, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(raw, 9))
        + chunk(b"IEND", b"")
    )
    path.write_bytes(png)


def main() -> None:
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    count = 0
    for source in sorted(ICON_DIR.glob("app_icon_*.c")):
        name = source.stem.removeprefix("app_icon_")
        target = OUT_DIR / f"icon-{name}.png"
        write_png(target, to_rgba(read_words(source.read_text(encoding="utf-8"))))
        print(f"{target.relative_to(ROOT)}  ({target.stat().st_size} bytes)")
        count += 1
    if not count:
        raise SystemExit("no icon .c file found")


if __name__ == "__main__":
    main()
