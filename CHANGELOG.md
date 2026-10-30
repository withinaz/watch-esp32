# Changelog

All notable changes to this firmware. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the project uses
its own version rule: **MAJOR only increases when an application is added**
(see [docs/versioning.md](docs/versioning.md)).

## [Unreleased]

### Added
- `tools/publish/`: the scheduled publisher that mirrors this development
  session into the repository automatically. A version declared in the session
  becomes a release; any other change becomes a snapshot commit. See
  [docs/auto-publish.md](docs/auto-publish.md).

Candidate work is listed in [ROADMAP.md](ROADMAP.md).

## [7.1.0] - 2026-10-30

Published automatically from the development session as **v7.1**.
Notes below are the session's own, in French.

| | |
|---|---|
| Fichier | `v7.1/montre-v7.1_0x200000.bin` |
| Taille | 3 794 080 octets |
| SHA-256 | `c7fcf2bffcca60851520225694bf9f9ce81b2ce979333e25b7244f42dd8e21c7` |
| Applications | Parametres (1), Assistant (2), Musique (3), Perplexe (4), Guerre (5), Fight (6) |
| Date | 2026-10-30 |

Correctif : les appuis sur les combattants ne faisaient rien — seule la croix
de fermeture repondait.

Cause : sur cette carte, un objet LVGL ne recoit le doigt que s'il porte
`LV_OBJ_FLAG_CLICKABLE`. Le canevas du jeu ne l'avait pas (un `lv_canvas`
n'est pas cliquable par defaut), contrairement au plateau de Perplexe qui le
pose explicitement ; les appuis remontaient au parent sans atteindre le
gestionnaire.

Correctifs :

- `LV_OBJ_FLAG_CLICKABLE` sur le canevas, et sur la racine en secours ;
- reaction des l'**appui** (`LV_EVENT_PRESSED`) et non au relachement : un jeu
  de combat doit repondre au premier contact ;
- lecture du peripherique d'entree avec repli sur le premier declare ;
- journal a chaque appui (`doigt (x,y) -> Ryo/Kai`) pour diagnostiquer sans
  reflasher.

Verifie au doigt sur la montre : 22 appuis enregistres, coordonnees conformes
(140 a 185 en x pour Ryo, 258 a 303 pour Kai), attaques declenchees a chaque
fois, aucune erreur du pilote d'ecran, aucune panique.

## v7 — Fight : jeu de combat 16 bits  *(remplacee par la v7.1)*

| | |
|---|---|
| Fichier | `v7/montre-v7_0x200000.bin` |
| Taille | 3 793 808 octets |
| SHA-256 | `a2d82dea5fa511c5a78039c9049d7f2aec086c87440cd5abca8d3b49b1434e08` |
| Applications | Parametres (1), Assistant (2), Musique (3), Perplexe (4), Guerre (5), **Fight (6)** |
| Date | 2026-10-30 |

Sixieme application : un jeu de combat facon borne d'arcade, decor 16 bits
tres sature (rue au coucher du soleil, skyline, soleil a rayures, foule,
sol en perspective).

Regles :

- deux combattants pixel art de 66 x 88 pixels d'art, affiches en 3x
  (198 x 264 px) : **Ryo** (gi rouge et or, cheveux noirs) a gauche, **Kai**
  (gi bleu et cyan, cheveux blonds) a droite ;
- **10 appuis par combattant** : un appui sur la moitie gauche fait attaquer
  Ryo, un appui sur la moitie droite fait attaquer Kai. Le compteur
  `ROUND n/10` avance quand les deux ont frappe une fois (un round = un
  echange) ; quand un combattant a ses dix appuis, sa zone devient inerte ;
- **5 attaques par combattant**, tirees au hasard a chaque appui, chacune avec
  son animation de 12 images (garde, arme, frappe, impact, retour) et son
  poids : JAB 7, CROCHET 11, PIED HAUT 14, UPPERCUT 17, HADOKEN 22 pour Ryo ;
  GRIFFE 7, GENOU 11, BALAYAGE 14, DOUBLE 17, ONDE 22 pour Kai ;
- **point de choc** : un rond blanc plein, cercle d'un anneau magenta, pose sur
  la partie du corps touchee (tete, machoire, menton, torse, ventre, jambe) ;
  la victime encaisse en blanc deux images et recule ;
- **fin** : apres les dix rounds, le vainqueur est celui qui a le plus de vie
  (240 points de depart, barres segmentees en haut). Un bandeau `WINNER`
  clignote au-dessus de sa tete, l'autre s'affaisse, puis tout repart de zero
  apres 4,2 s ;
- **K.O.** : si une barre tombe a zero avant la fin, le combat s'arrete la et
  l'autre gagne (filet de securite, jamais atteint avec 240 points de vie).

Rendu : un seul canevas de 410 x 396 place a y=56. Le decor complet est peint
une fois (PSRAM), puis seule la bande des combattants (380 x 270) est
restauree, redessinee et invalidee a chaque image d'attaque ; au repos, rien
n'est redessine. Cadence de 45 ms (22 images/s).

Mesures : match complet de 20 attaques, 0 erreur du pilote d'ecran, 0 panique,
vainqueur au point pres (109 a 105). Captures de controle : garde, impact,
bandeau du vainqueur.

Image: `firmware/v7.1/montre.bin`,
3 794 080 bytes, SHA-256 `c7fcf2bffcca60851520225694bf9f9ce81b2ce979333e25b7244f42dd8e21c7`, write at `0x200000`.

## [5.1.0] - 2026-10-30

Published automatically from the development session as **v5.1**.
Notes below are the session's own, in French.

| | |
|---|---|
| Fichier | `v5.1/montre-v5.1_0x200000.bin` |
| Taille | 3 692 736 octets |
| SHA-256 | `6a90a757e96b5ec2a25338ad800bd6197e7fa10b1b38f522604b4d9474423507` |
| Applications | Parametres (1), Assistant (2), Musique (3), Perplexe (4), **Guerre (5)** |
| Date | 2026-10-29 |

Revision de l'app Guerre, aucun changement d'application :

- **missiles et explosions 2 a 3 fois plus gros** : tete de 7 x 7 px (disque
  a la couleur du continent, coeur blanc de 3 x 3), trainee de neuf blocs de
  2 x 2 px, explosion dont l'anneau va jusqu'a 33 px de rayon sur 3 px
  d'epaisseur, avec coeur blanc et huit eclats ;
- **fin du monde** : quand **90 % des cases de terre** ont un cratere (723
  cases sur 804), la guerre s'arrete, **« THE END »** s'affiche en blanc, gras
  (contour de 3 px), au milieu de la carte et **au-dessus** d'elle, la carte
  reste figee **3 secondes**, puis tout est remis a zero : carte neuve, jauges
  a zero, nouvelle paire de continents. Boucle sans fin.
- le gel de la carte pendant les 3 secondes evite de payer le redessin du
  texte a chaque image (le rythme tombait a ~12 i/s sinon).

Image: `firmware/v5.1/montre.bin`,
3 692 736 bytes, SHA-256 `6a90a757e96b5ec2a25338ad800bd6197e7fa10b1b38f522604b4d9474423507`, write at `0x200000`.

## [4.0.0] - 2026-10-29

Published automatically from the development session as **v4.0**.
Notes below are the session's own, in French.

| | |
|---|---|
| Fichier | `v4.0/montre-v4.0_0x200000.bin` |
| Taille | 3 589 296 octets |
| SHA-256 | `51ca2508dba666b339d33a8d35b85d54eb90d478980369837379327daac34d43` |
| Applications | Parametres (1), Assistant (2), Musique (3), **Perplexe (4)** |
| Date | 2026-10-28 |

**Le jeu.** Un labyrinthe vu de dessus : la bille roule quand on incline la
montre, le doigt pose sur le plateau la freine, une secousse la ramene au
dernier point de passage. Dix niveaux de difficulte croissante, generes par
`tools/make_levels.py` puis **verifies par parcours en largeur** : le but est
toujours atteignable sans traverser un trou. 100 points par niveau, moins 10
par chute, plancher 20.

**Deux defauts trouves en chemin, qui n'etaient pas dans le jeu.**

1. **Le QMI8658 repond en `0x6B`, pas en `0x6A`.** La page Capteurs de l'app
   Parametres affichait donc « QMI8658 introuvable » depuis toujours. Les deux
   applications essaient maintenant les deux adresses.
2. **L'echelle de l'accelerometre etait fausse d'un facteur 2.** CTRL2 = 0x95
   selectionne la plage +-4 g, soit **8192 points par g** et non 4096 : au
   repos on lisait 2,00 g. Corrige aux deux endroits ; mesure apres correctif :
   `|a| = 1,00 g`.

**Les axes sont tournes d'un quart de tour.** Releve sur la montre : incliner
le bord droit vers le bas faisait *monter* la bille, le bord bas la faisait
aller a *droite*. Le capteur est monte a 90 degres ; `imu_to_screen()` remet
ses axes dans le repere de l'ecran (X du capteur = bas de l'ecran, Y = gauche).

**Le tactile est confirme dans cette application** : les boutons REJOUER et
QUITTER repondent au doigt. C'est la premiere application tactile depuis l'app
Parametres ; l'Assistant, lui, reste muet au doigt.

Verifie : demarrage propre, quatre pages de lanceur (`create a new page`
trois fois), `Hash of data verified`, zero erreur, WiFi et NTP inchanges.

Image: `firmware/v4.0/montre.bin`,
3 589 296 bytes, SHA-256 `51ca2508dba666b339d33a8d35b85d54eb90d478980369837379327daac34d43`, write at `0x200000`.

## [3.5.0] — 2026-10-27

### Added
- `repaint_top_band()`: the 0..49 pixel band is explicitly invalidated on the
  system layer at every launcher ↔ app transition, so the status bar repaints
  over pixels left by the launcher.
- The 150 × 150 "status" icon brookesia places in a 30 × 30 slot of the status
  bar is hidden, instead of being drawn clipped.

### Changed
- Documentation: the measured write pattern (`0-49:1 50-63:3`) is recorded, and
  the top-band entry now states that the fix is real **and** that the 2–3 row
  residue seen with a finger is still open.

### Fixed
- Rows 0..49 of the panel received no write at all while an app was open.

### Notes
- Application: 3 486 160 bytes, SHA-256
  `aecb03997166e2ad88f6f52b553427cf0419e7aa39dfb83d93e7d5d654010b64`.
- Rebuilding the sources reproduces the size, but ~97 bytes differ in a literal
  pool. The shipped binary remains the reference.

## [3.4.0] — 2026-10-27

### Added
- Launcher guard: while an app is open, the launcher icons are reset to their
  normal size and hidden; they reappear on return. This also repairs an icon
  left enlarged by a `PRESSED` without a `RELEASED`.

### Notes
- The defect it targets only reproduces with a **real finger**: a programmatic
  click never sends `LV_EVENT_PRESSED`, which is the event that enlarges the
  icon. Every automated test had been passing for that reason.

## [3.3.0] — 2026-10-27

### Changed
- Music timbre reverted to the v3.1 plucked-string engine (12 voices). The
  violin of v3.2 is abandoned: too expensive at `-Og`, not convincing on this
  speaker.
- The three display fixes of v3.2 are kept.

### Verified
- Four bars at 85 BPM in 11.27 s, against 11.29 s theoretical.

## [3.2.0] — 2026-10-26

### Added
- Bowed-string synthesis: attack / sustain / release envelope, delayed vibrato
  (5.5 Hz, ±0.55 %), 2–5 harmonics at weight 1/n, note-off from the sequencer.
- Full-screen invalidation 350 ms and 1.4 s after an app opens, and an opaque
  screen background in the app colour.

### Changed
- 8 voices instead of 12 to hold the tempo at `-Og`.

### Verified
- 11.82 s for four bars at 81 BPM, against 11.85 s theoretical.

## [3.1.0] — 2026-10-26

### Fixed
- A 50 px strip under every app showed the launcher: app containers now cover
  the full screen height (452 px) while their content stays laid out for 402 px.
- The Assistant could be stuck in `Hors ligne` with an unreachable button; it now
  retries the connection every 8 s and shows `PARLER` in every state.
- The speaker codec open is retried once after 300 ms, in case the Music app
  still holds it.

## [3.0.0] — 2026-10-26

### Added
- **Music application** (page 3): generative music box — pentatonic sequencer,
  I–V–vi–IV cycle, 78–96 BPM, 12 voices at 24 kHz, 12 × 10 piano-roll UI, energy
  gauge, note counter.
- Custom icon `app_icon_music` (violet tile, double eighth note).

### Verified
- Four bars at 78 BPM in 12.3 s.

## [2.3.0] — 2026-10-26

### Changed
- Assistant button reads `PARLER` instead of `ANNULER` while connecting.

## [2.2.0] — 2026-10-26

### Added
- Live level meter in the Assistant: 26 bars driven by the real peak of the
  microphone and speaker streams, 40 ms refresh, no extra task or allocation.

### Changed
- Transcript area reduced from 192 to 128 px to make room.

## [2.1.0] — 2026-10-26

### Added
- One custom 150 × 150 ARGB8888 icon per application (slate gear, orange
  microphone), replacing the upscaled component default.
- `tools/make_icons.py`, `tools/png2lvgl.py`, and the `XZ_ICONSNAP` diagnostic.

## [2.0.0] — 2026-10-26

### Added
- **Assistant application** (page 2): XiaoZhi protocol client — OTA pairing,
  WebSocket, Opus 24 kHz, microphone at 37.5 dB + ×2 software gain, 90 % volume.
- Launcher: one application per page (210 px cells), a page added per app.

### Known limitation
- Touch does not respond inside the Assistant; the app has no back button and is
  left by restarting the watch.

## [1.0.0] — 2026-10-24

### Added
- Firmware for the Waveshare ESP32-S3-Touch-AMOLED-2.06: esp-brookesia 0.5.0
  shell, LVGL 9.3, orange status bar (clock, date, battery, WiFi).
- Settings application in seven pages: WiFi, Display, Sound, Clock, Sensors,
  Storage, Device.
- Display, touch, WiFi, NTP, RTC, battery gauge, IMU and audio, all working.

[Unreleased]: https://github.com/withinaz/watch-esp32/compare/v3.5.0...HEAD
[7.1.0]: https://github.com/withinaz/watch-esp32/releases/tag/v7.1.0
[5.1.0]: https://github.com/withinaz/watch-esp32/releases/tag/v5.1.0
[4.0.0]: https://github.com/withinaz/watch-esp32/releases/tag/v4.0.0
[3.5.0]: https://github.com/withinaz/watch-esp32/releases/tag/v3.5.0
[3.4.0]: https://github.com/withinaz/watch-esp32/releases/tag/v3.4.0
