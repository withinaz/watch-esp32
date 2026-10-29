#!/usr/bin/env python3
"""
Generateur des labyrinthes de l'app Perplexe.

Sortie :
    main/levels_perplexe.h   -> les dix niveaux, en chaines C

Chaque niveau est un labyrinthe parfait (parcours en profondeur aleatoire)
sur une grille de 9 x 8 cellules, soit 19 x 17 cases a l'ecran -- une case
fait 20 px, le plateau 380 x 340 px. On perce ensuite des boucles (des murs
en moins) puis des trous.

Le generateur refuse un niveau ou le but n'est pas atteignable sans passer
par un trou : un niveau livre est toujours jouable.

Legende des cases :
    '#' mur      '.' sol      'o' trou
    'S' depart   'G' arrivee  '*' point de passage

Usage :
    python tools/make_levels.py
"""

import os
import random

W, H = 9, 8                          # cellules logiques
COLS, ROWS = 2 * W + 1, 2 * H + 1    # 19 x 17 cases a l'ecran

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_H = os.path.join(ROOT, "main", "levels_perplexe.h")

START = (1, 1)
GOAL = (2 * W - 1, 2 * H - 1)


# --------------------------------------------------------------------------
# difficulte
# --------------------------------------------------------------------------

def settings(level):
    """(proportion de cloisons ouvertes, nombre de trous) pour un niveau."""
    t = (level - 1) / 9.0
    open_ratio = 0.30 - 0.26 * t     # beaucoup de boucles au debut, presque plus a la fin
    holes = 2 + round(10 * t)        # 2 trous au niveau 1, 12 au niveau 10
    return open_ratio, holes


# --------------------------------------------------------------------------
# generation
# --------------------------------------------------------------------------

def carve(rng):
    """Labyrinthe parfait : chaque cellule est reliee, sans boucle."""
    g = [["#"] * COLS for _ in range(ROWS)]
    seen = [[False] * W for _ in range(H)]
    seen[0][0] = True
    g[START[1]][START[0]] = "."
    stack = [(0, 0)]
    while stack:
        cx, cy = stack[-1]
        nbrs = []
        for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            nx, ny = cx + dx, cy + dy
            if 0 <= nx < W and 0 <= ny < H and not seen[ny][nx]:
                nbrs.append((nx, ny, dx, dy))
        if not nbrs:
            stack.pop()
            continue
        nx, ny, dx, dy = rng.choice(nbrs)
        g[2 * cy + 1 + dy][2 * cx + 1 + dx] = "."
        g[2 * ny + 1][2 * nx + 1] = "."
        seen[ny][nx] = True
        stack.append((nx, ny))
    return g


def open_loops(g, rng, ratio):
    """Abat une partie des cloisons : le labyrinthe gagne des raccourcis."""
    walls = []
    for y in range(1, ROWS - 1):
        for x in range(1, COLS - 1):
            if g[y][x] != "#":
                continue
            # une cloison separe deux cellules : une coordonnee paire, une impaire
            if (y % 2 == 0) != (x % 2 == 0):
                walls.append((x, y))
    rng.shuffle(walls)
    for x, y in walls:
        if rng.random() < ratio:
            g[y][x] = "."


def bfs(g, src, blocked="#"):
    """Distances et predecesseurs depuis src, en marchant sur le sol."""
    dist = {src: 0}
    prev = {}
    queue = [src]
    head = 0
    while head < len(queue):
        x, y = queue[head]
        head += 1
        for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            nx, ny = x + dx, y + dy
            if not (0 <= nx < COLS and 0 <= ny < ROWS):
                continue
            if g[ny][nx] == "o" or g[ny][nx] in blocked:
                continue
            if (nx, ny) in dist:
                continue
            dist[(nx, ny)] = dist[(x, y)] + 1
            prev[(nx, ny)] = (x, y)
            queue.append((nx, ny))
    return dist, prev


def path_to(prev, goal):
    path = [goal]
    while path[-1] in prev:
        path.append(prev[path[-1]])
    path.reverse()
    return path


def build_level(level):
    """Construit un niveau jouable, ou leve une exception."""
    ratio, holes = settings(level)

    for attempt in range(40):
        rng = random.Random(1000 * level + attempt)
        g = carve(rng)
        open_loops(g, rng, ratio)

        dist, prev = bfs(g, START)
        if GOAL not in dist:
            continue
        path = path_to(prev, GOAL)

        # Un parcours trop court ne fait pas un niveau.
        if len(path) < 18:
            continue

        on_path = set(path)

        # Points de passage au tiers et aux deux tiers du trajet.
        cps = []
        for frac in (0.34, 0.67):
            cell = path[int(len(path) * frac)]
            if cell not in (START, GOAL) and cell not in cps:
                cps.append(cell)

        # Trous : jamais sur le trajet, jamais sur le depart ou l'arrivee.
        cand = []
        for y in range(1, ROWS - 1):
            for x in range(1, COLS - 1):
                if g[y][x] != ".":
                    continue
                if (x, y) in on_path or (x, y) in cps:
                    continue
                if dist.get((x, y), 0) < 3:
                    continue
                cand.append((x, y))
        rng.shuffle(cand)
        placed = 0
        for x, y in cand:
            if placed >= holes:
                break
            g[y][x] = "o"
            placed += 1

        # Verification : le but reste atteignable sans traverser un trou.
        check, _ = bfs(g, START)
        if GOAL not in check:
            continue

        for x, y in cps:
            g[y][x] = "*"
        g[START[1]][START[0]] = "S"
        g[GOAL[1]][GOAL[0]] = "G"

        return g, len(path), placed, len(cps)

    raise RuntimeError("niveau %d : aucun labyrinthe valable" % level)


# --------------------------------------------------------------------------
# sortie
# --------------------------------------------------------------------------

def as_c_string(g):
    lines = []
    for row in g:
        lines.append('        "%s"' % "".join(row))
    return "\n".join(lines)


def main():
    levels = []
    for level in range(1, 11):
        g, plen, holes, cps = build_level(level)
        levels.append(g)
        print("niveau %2d : trajet %2d cases, %2d trous, %d point(s) de passage"
              % (level, plen, holes, cps))
        print("            " + "\n            ".join("".join(r) for r in g))

    body = ",\n".join(as_c_string(g) for g in levels)

    src = """\
/*
 * Labyrinthes de l'app Perplexe -- fichier genere par tools/make_levels.py
 * Ne pas modifier a la main : relancer le script.
 *
 * 19 x 17 cases de 20 px = un plateau de 380 x 340 px.
 * '#' mur, '.' sol, 'o' trou, 'S' depart, 'G' arrivee, '*' point de passage.
 */
#pragma once

#define PERPLEXE_COLS    %d
#define PERPLEXE_ROWS    %d
#define PERPLEXE_LEVELS  %d

static const char *const PERPLEXE_LEVEL[PERPLEXE_LEVELS] = {
%s,
};
""" % (COLS, ROWS, len(levels), body)

    with open(OUT_H, "w", encoding="utf-8", newline="\n") as f:
        f.write(src)
    print("ecrit", os.path.relpath(OUT_H, ROOT))


if __name__ == "__main__":
    main()
