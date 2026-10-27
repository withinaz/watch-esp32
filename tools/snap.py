"""
Decode les captures d'ecran envoyees par la montre sur le port serie.

La montre imprime :

    SNAP BEGIN <tag> <largeur> <hauteur>
    <base64 continu, sur plusieurs lignes>
    SNAP END

Ce script lit un fichier de log serie, recolle le flux base64 (le port perd
parfois quelques caracteres, donc on ne decode pas ligne par ligne) et ecrit
un PNG. Pas de dependance externe.
"""
import base64
import re
import struct
import sys
import zlib
from pathlib import Path

B64CHARS = re.compile(r"[^A-Za-z0-9+/=]")


def write_png(path, width, height, rows):
    raw = b"".join(b"\x00" + r for r in rows)

    def chunk(tag, data):
        return (struct.pack(">I", len(data)) + tag + data
                + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 9))
    png += chunk(b"IEND", b"")
    Path(path).write_bytes(png)


def to_row(blob, w):
    row = bytearray()
    for x in range(w):
        px = blob[2 * x] | (blob[2 * x + 1] << 8)
        row += bytes((((px >> 11) & 0x1F) * 255 // 31,
                      ((px >> 5) & 0x3F) * 255 // 63,
                      (px & 0x1F) * 255 // 31))
    return bytes(row)


def to_rows(blob, w, h):
    """RGB565 little-endian -> lignes RGB888."""
    need = w * h * 2
    if len(blob) < need:
        blob = blob + b"\x00" * (need - len(blob))
    return [to_row(blob[y * w * 2:], w) for y in range(h)]


def parse(log_text):
    """Chaque ligne du flux encode w*2 octets (RGB565), avec son propre
    remplissage base64. On decode donc ligne par ligne : le port perd parfois
    des caracteres, et une ligne abimee est remplacee par du noir plutot que
    de decaler toute l'image."""
    images = {}
    tag = None
    w = h = 0
    rows = []
    for line in log_text.splitlines():
        s = line.strip()
        if s.startswith("SNAP BEGIN"):
            p = s.split()
            tag, w, h = p[2], int(p[3]), int(p[4])
            rows = []
        elif s.startswith("SNAP END"):
            if tag and w and h and rows:
                while len(rows) < h:
                    rows.append(bytes(w * 3))
                images[tag] = (w, h, rows[:h])
                print(f"{tag}: {w}x{h}, {len(rows)} lignes decodees")
            tag = None
            rows = []
        elif tag and s:
            s = re.sub(r"[^A-Za-z0-9+/]", "", s)
            s = s[: len(s) - (len(s) % 4)]
            if len(s) < 4:
                rows.append(bytes(w * 3))
                continue
            try:
                blob = base64.b64decode(s, validate=False)
            except Exception:
                rows.append(bytes(w * 3))
                continue
            if len(blob) < w * 2:
                blob = blob + bytes(w * 2 - len(blob))
            rows.append(to_row(blob, w))
    return images


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 1
    log = Path(sys.argv[1]).read_text(errors="replace")
    out = Path(sys.argv[2])
    out.mkdir(parents=True, exist_ok=True)
    images = parse(log)
    if not images:
        print("aucune capture exploitable")
        return 2
    for tag, (w, h, rows) in images.items():
        path = out / f"snap-{tag}.png"
        write_png(path, w, h, rows)
        print(f"ecrit {path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
