#!/usr/bin/env python3
"""
Convertit une image PNG (carree, fond transparent) en icone de lanceur LVGL.

    python tools/png2lvgl.py mon-icone.png app_icon_monapp

Produit `main/icons/app_icon_monapp.c` (150x150, ARGB8888) et met a jour
`main/icons/app_icons.h`. Il reste ensuite deux lignes a ecrire a la main :

  * main/CMakeLists.txt : ajouter "icons/app_icon_monapp.c" dans SRCS
  * app_xxx.cpp         : passer &app_icon_monapp au constructeur

L'image est reduite a 150x150, la taille exacte utilisee par le lanceur : elle
n'est donc jamais agrandie a l'affichage (c'est ce qui rendait l'icone par
defaut floue).
"""

import os
import sys

from PIL import Image

import make_icons as mi


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 1

    src = sys.argv[1]
    name = sys.argv[2]
    if not name.startswith("app_icon_"):
        name = "app_icon_" + name
    if not os.path.exists(src):
        print("introuvable :", src)
        return 1

    img = Image.open(src).convert("RGBA")
    if img.width != img.height:
        print("l'image doit etre carree (elle fait %dx%d)" % (img.size))
        return 1
    if img.size != (mi.SIZE, mi.SIZE):
        img = img.resize((mi.SIZE, mi.SIZE), Image.LANCZOS)

    os.makedirs(mi.OUT_C, exist_ok=True)
    os.makedirs(mi.OUT_PNG, exist_ok=True)
    img.save(os.path.join(mi.OUT_PNG, name + ".png"))
    path = mi.write_c(name, img, "Icone de lanceur importee depuis " + os.path.basename(src))
    print("ecrit", os.path.relpath(path, mi.ROOT))
    names = mi.write_header()
    print("entete main/icons/app_icons.h :", ", ".join(names))
    return 0


if __name__ == "__main__":
    sys.exit(main())
