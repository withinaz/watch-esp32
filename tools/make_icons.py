#!/usr/bin/env python3
"""
Generateur des icones du lanceur (une icone par application).

Sortie :
  main/icons/app_icon_<nom>.c   -> tableaux LVGL (ARGB8888) + descripteurs
  main/icons/app_icons.h        -> declarations des descripteurs
  tools/icons/<nom>.png         -> apercu (non embarque)

Chaque icone est dessinee en 600x600 (4x) puis reduite a 150x150, ce qui donne
un anticrenelage propre. La taille 150 correspond exactement a
`icon.image.default_size` du stylesheet : l'image n'est donc jamais agrandie
par le lanceur (c'est ce qui rendait l'icone par defaut floue).

Usage :
    python tools/make_icons.py
"""

import math
import os

from PIL import Image, ImageDraw

SIZE = 150          # taille finale, en pixels ecran
SS = 4              # facteur de suréchantillonnage
N = SIZE * SS       # toile de travail

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_C = os.path.join(ROOT, "main", "icons")
OUT_PNG = os.path.join(ROOT, "tools", "icons")


# --------------------------------------------------------------------------
# outils de dessin
# --------------------------------------------------------------------------

def lerp(a, b, t):
    return a + (b - a) * t


def mix(c1, c2, t):
    return tuple(int(round(lerp(c1[i], c2[i], t))) for i in range(len(c1)))


def gradient(size, top, bottom):
    """Degrade vertical RGB."""
    img = Image.new("RGB", (1, size), top)
    px = img.load()
    for y in range(size):
        px[0, y] = mix(top, bottom, y / (size - 1))
    return img.resize((size, size), Image.NEAREST)


def rounded_mask(radius, inset=0, size=N):
    m = Image.new("L", (size, size), 0)
    d = ImageDraw.Draw(m)
    d.rounded_rectangle(
        [inset, inset, size - 1 - inset, size - 1 - inset],
        radius=radius,
        fill=255,
    )
    return m


def smoothstep(t):
    t = max(0.0, min(1.0, t))
    return t * t * (3.0 - 2.0 * t)


def paste_glyph(tile, mask, color, opacity=1.0):
    """Compose un glyphe (masque L) sur la tuile, apres l'avoir recentre."""
    bbox = mask.getbbox()
    if bbox is None:
        return tile
    glyph = mask.crop(bbox)
    w, h = glyph.size
    # recentrage optique dans la tuile
    x = (N - w) // 2
    y = (N - h) // 2
    canvas = Image.new("L", (N, N), 0)
    canvas.paste(glyph, (x, y))

    rgba = Image.new("RGBA", (N, N), color + (0,))
    alpha = canvas.point(lambda v: int(v * opacity))
    rgba.putalpha(alpha)
    return Image.alpha_composite(tile, rgba)


# --------------------------------------------------------------------------
# formes
# --------------------------------------------------------------------------

def gear_mask(teeth=8, r_out=43, r_body=34, r_hole=12.5):
    """Engrenage : rayon continu, dent a flancs arrondis, trou central."""
    m = Image.new("L", (N, N), 0)
    d = ImageDraw.Draw(m)

    cx = cy = N / 2.0
    R = r_out * SS
    Rb = r_body * SS
    period = 2.0 * math.pi / teeth
    tooth = 0.55          # fraction angulaire de la dent (0..1)
    flatten = 0.13        # largeur de la transition

    pts = []
    steps = teeth * 90
    for i in range(steps):
        th = 2.0 * math.pi * i / steps
        ph = (th % period) / period          # 0..1 dans le motif
        dth = abs(ph - 0.5) * 2.0            # 0 au centre de la dent, 1 au creux
        edge = tooth
        if dth <= edge:
            r = R
        else:
            r = R - (R - Rb) * smoothstep((dth - edge) / flatten)
        pts.append((cx + r * math.cos(th), cy + r * math.sin(th)))

    d.polygon(pts, fill=255)
    # trou central
    h = r_hole * SS
    d.ellipse([cx - h, cy - h, cx + h, cy + h], fill=0)
    return m


def mic_mask():
    """Microphone : capsule, berceau en arc, pied."""
    m = Image.new("L", (N, N), 0)
    d = ImageDraw.Draw(m)
    cx = N / 2.0
    cy = N / 2.0

    cap_w = 30 * SS
    cap_h = 62 * SS
    cap_top = cy - 40 * SS
    d.rounded_rectangle(
        [cx - cap_w / 2, cap_top, cx + cap_w / 2, cap_top + cap_h],
        radius=cap_w / 2,
        fill=255,
    )

    # berceau : demi-cercle ouvert vers le haut
    rad = 30 * SS
    thick = 7 * SS
    box = [cx - rad, cy - 14 * SS, cx + rad, cy - 14 * SS + 2 * rad]
    d.arc(box, start=0, end=180, fill=255, width=int(thick))

    # pied vertical
    stem_top = cy - 14 * SS + 2 * rad
    d.rounded_rectangle(
        [cx - thick / 2, stem_top - thick / 2, cx + thick / 2, stem_top + 14 * SS],
        radius=thick / 2,
        fill=255,
    )
    return m


def wave_mask():
    """Ondes sonores (variante possible de l'assistant)."""
    m = Image.new("L", (N, N), 0)
    d = ImageDraw.Draw(m)
    cx = cy = N / 2.0
    bars = [(-30, 0.30), (-18, 0.62), (-6, 1.0), (6, 0.62), (18, 0.30)]
    w = 9 * SS
    for dx, f in bars:
        h = 70 * SS * f / 2
        x = cx + dx * SS
        d.rounded_rectangle([x - w / 2, cy - h, x + w / 2, cy + h], radius=w / 2, fill=255)
    return m


def note_mask():
    """Double croche : deux tetes, deux hampes et une ligature.

    Dessinee d'abord bien verticale puis legerement inclinee : c'est ce
    desequilibre qui fait lire une note de musique plutot qu'un pictogramme.
    """
    layer = Image.new("L", (N, N), 0)
    d = ImageDraw.Draw(layer)

    cx = cy = N / 2.0
    hw = 34 * SS            # largeur d'une tete
    hh = 24 * SS            # hauteur d'une tete
    stem = 7 * SS           # epaisseur de hampe

    # tete gauche plus basse que la droite : mouvement ascendant
    h1x, h1y = cx - 44 * SS, cy + 30 * SS
    h2x, h2y = cx + 20 * SS, cy + 8 * SS

    for hx, hy in ((h1x, h1y), (h2x, h2y)):
        d.ellipse([hx - hw / 2, hy - hh / 2, hx + hw / 2, hy + hh / 2], fill=255)

    # sommets des hampes
    top1 = cy - 34 * SS
    top2 = cy - 56 * SS

    x1 = h1x + hw / 2 - stem
    x2 = h2x + hw / 2 - stem
    d.rounded_rectangle([x1, top1, x1 + stem, h1y - hh / 2 + 2], radius=stem / 2, fill=255)
    d.rounded_rectangle([x2, top2, x2 + stem, h2y - hh / 2 + 2], radius=stem / 2, fill=255)

    # ligature : parallelogramme entre les deux sommets
    beam = 15 * SS
    d.polygon(
        [(x1, top1), (x2, top2), (x2 + stem, top2 + beam), (x1 + stem, top1 + beam)],
        fill=255,
    )

    return layer.rotate(-11, resample=Image.BICUBIC, center=(cx, cy))


# --------------------------------------------------------------------------
# composition d'une icone
# --------------------------------------------------------------------------

def build(tile_top, tile_bottom, rim, glyph_mask, glyph_color, glyph_opacity=1.0):
    inset = 1 * SS
    radius = 34 * SS

    base = gradient(N, tile_top, tile_bottom).convert("RGBA")
    mask = rounded_mask(radius, inset)
    tile = Image.new("RGBA", (N, N), (0, 0, 0, 0))
    tile.paste(base, (0, 0), mask)

    # liseré lumineux en haut, ombre douce en bas (effet verre)
    lip = Image.new("L", (N, N), 0)
    dl = ImageDraw.Draw(lip)
    dl.rounded_rectangle(
        [inset + 1.2 * SS, inset + 1.2 * SS, N - 1 - inset - 1.2 * SS, N - 1 - inset - 1.2 * SS],
        radius=radius,
        outline=255,
        width=int(1.4 * SS),
    )
    fade = Image.linear_gradient("L").resize((N, N))   # 0 en haut -> 255 en bas
    lip_top = Image.composite(lip, Image.new("L", (N, N), 0), fade.point(lambda v: 255 - v))
    shadow_lip = lip.point(lambda v: int(v * 0.10))
    tile = Image.alpha_composite(tile, _tint(lip_top.point(lambda v: int(v * 0.22)), rim))
    tile = Image.alpha_composite(tile, _tint(shadow_lip, (0, 0, 0)))

    tile = paste_glyph(tile, glyph_mask, glyph_color, glyph_opacity)
    return tile.resize((SIZE, SIZE), Image.LANCZOS)


def _tint(mask, color):
    layer = Image.new("RGBA", mask.size, color + (0,))
    layer.putalpha(mask)
    return layer


# --------------------------------------------------------------------------
# ecriture C
# --------------------------------------------------------------------------

HEADER = """\
/*
 * Icones du lanceur -- fichier genere par tools/make_icons.py
 * Ne pas modifier a la main : relancer le script.
 */
#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {{
#endif

{decls}
#ifdef __cplusplus
}}
#endif
"""


def to_argb_words(img):
    """ARGB8888 en mots 32 bits 0xAARRGGBB (memoire LVGL : B,G,R,A)."""
    data = img.tobytes()          # RGBA
    words = []
    for i in range(0, len(data), 4):
        r, g, b, a = data[i], data[i + 1], data[i + 2], data[i + 3]
        words.append((a << 24) | (r << 16) | (g << 8) | b)
    return words


def write_c(name, img, comment):
    words = to_argb_words(img)
    w, h = img.size

    lines = []
    for i in range(0, len(words), 8):
        chunk = ", ".join("0x%08X" % v for v in words[i:i + 8])
        lines.append("    " + chunk + ",")
    body = "\n".join(lines)

    src = f"""\
/*
 * {comment}
 * {w}x{h} ARGB8888 -- genere par tools/make_icons.py
 */
#include "icons/app_icons.h"

static const uint32_t {name}_map[{len(words)}] = {{
{body}
}};

const lv_image_dsc_t {name} = {{
    .header.magic = LV_IMAGE_HEADER_MAGIC,
    .header.cf = LV_COLOR_FORMAT_ARGB8888,
    .header.flags = 0,
    .header.w = {w},
    .header.h = {h},
    .header.stride = {w * 4},
    .header.reserved_2 = 0,
    .data_size = (uint32_t)sizeof({name}_map),
    .data = (const uint8_t *){name}_map,
    .reserved = NULL,
    .reserved_2 = NULL,
}};
"""
    path = os.path.join(OUT_C, name + ".c")
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(src)
    return path


# --------------------------------------------------------------------------
# catalogue
# --------------------------------------------------------------------------

ICONS = [
    dict(
        name="app_icon_settings",
        comment="Icone de l'application Parametres : engrenage sur tuile ardoise.",
        tile_top=(0x41, 0x50, 0x74),
        tile_bottom=(0x16, 0x1B, 0x26),
        rim=(0xBF, 0xD3, 0xFF),
        mask=gear_mask,
        glyph=(0xE9, 0xF1, 0xFF),
    ),
    dict(
        name="app_icon_assistant",
        comment="Icone de l'application Assistant : microphone sur tuile orange.",
        tile_top=(0xFF, 0x93, 0x44),
        tile_bottom=(0xC2, 0x3B, 0x06),
        rim=(0xFF, 0xE2, 0xC8),
        mask=mic_mask,
        glyph=(0xFF, 0xFF, 0xFF),
    ),
    dict(
        name="app_icon_music",
        comment="Icone de l'application Musique : double croche sur tuile violette.",
        tile_top=(0x7B, 0x5C, 0xF0),
        tile_bottom=(0x2A, 0x18, 0x5C),
        rim=(0xD8, 0xCB, 0xFF),
        mask=note_mask,
        glyph=(0xFF, 0xFF, 0xFF),
    ),
]


def write_header():
    """Regenere main/icons/app_icons.h a partir des .c presents."""
    names = sorted(
        os.path.splitext(f)[0]
        for f in os.listdir(OUT_C)
        if f.startswith("app_icon_") and f.endswith(".c")
    )
    decls = ["extern const lv_image_dsc_t %s;" % n for n in names]
    with open(os.path.join(OUT_C, "app_icons.h"), "w", encoding="utf-8", newline="\n") as f:
        f.write(HEADER.format(decls="\n".join(decls) + "\n"))
    return names


def main():
    os.makedirs(OUT_C, exist_ok=True)
    os.makedirs(OUT_PNG, exist_ok=True)

    previews = []
    for spec in ICONS:
        img = build(
            spec["tile_top"],
            spec["tile_bottom"],
            spec["rim"],
            spec["mask"](),
            spec["glyph"],
        )
        img.save(os.path.join(OUT_PNG, spec["name"] + ".png"))
        path = write_c(spec["name"], img, spec["comment"])
        print("ecrit", os.path.relpath(path, ROOT))
        previews.append(img)

    names = write_header()
    print("entete main/icons/app_icons.h :", ", ".join(names))

    # planche d'apercu : les icones sur le fond du lanceur, a l'echelle 1
    bg = Image.new("RGBA", (len(previews) * 190 + 40, 210), (0x0A, 0x0C, 0x10, 255))
    for i, img in enumerate(previews):
        bg.alpha_composite(img, (30 + i * 190, 30))
    bg.resize((bg.width * 3, bg.height * 3), Image.LANCZOS).save(
        os.path.join(OUT_PNG, "preview.png")
    )
    print("apercu", os.path.relpath(os.path.join(OUT_PNG, "preview.png"), ROOT))


if __name__ == "__main__":
    main()
