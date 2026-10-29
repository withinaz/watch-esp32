# Version history

How this firmware got to v4.0, including the versions that were thrown away.
The rule: a **major** bump means a new application in the launcher; a **minor**
marks a revision of the same application set. Internal fixes do not change the
number on their own.

| Version | Date | Apps | Size | What it is |
|---|---|---|---|---|
| v1 | 2026-10-24 | 1 | 2 821 744 | Shell, status bar, Settings in seven pages |
| v2 | 2026-10-26 | 2 | 3 245 936 | + Assistant (XiaoZhi), one app per page |
| v2.1 | 2026-10-26 | 2 | 3 375 792 | + custom 150 × 150 icons |
| v2.2 | 2026-10-26 | 2 | 3 382 560 | + live level meter in the Assistant |
| v2.3 | 2026-10-26 | 2 | 3 382 560 | button reads `PARLER` while connecting |
| v3 | 2026-10-26 | 3 | 3 484 304 | + Music, generative music box |
| v3.1 | 2026-10-26 | 3 | 3 485 040 | 452 px containers, assistant auto-retry |
| v3.2 | 2026-10-26 | 3 | 3 484 976 | violin timbre, full-screen invalidation |
| v3.3 | 2026-10-27 | 3 | 3 485 680 | back to the piano, display fixes kept |
| v3.4 | 2026-10-27 | 3 | 3 486 016 | launcher icons masked while an app is open |
| v3.5 | 2026-10-27 | 3 | 3 486 160 | + top band repaint, no status icon in the bar |
| v3.6 | — | 3 | 3 485 312 | full-screen apps — **dropped, binary deleted** |
| **v4.0** | 2026-10-28 | **4** | **3 589 296** | + Perplexe (marble maze), IMU address and scale fixed |
| v4.1 | — | 4 | — | three sounds in the maze — **dropped, binary deleted**, number burned |

---

## v1 — the base

esp-brookesia 0.5.0, LVGL 9.3, orange status bar, everything working: display,
touch, WiFi, clock, battery, IMU, sound, and the seven Settings pages. One
application, so the launcher had a single page and the icon was still the
component's default.

## v2 — the voice assistant

Two things at once: the launcher switched to **one application per page**
(210 px cells) and the Assistant arrived — OTA pairing, WebSocket, Opus at
24 kHz, 90 % volume, microphone at 37.5 dB plus ×2 software gain.

Known limitation from the first day: **touch does not respond inside the
Assistant**. The app works (auto-connect, voice dialogue), you leave it by
restarting the watch, and it deliberately has no back button.

## v2.1 — icons that are actually icons

Each app got its own ARGB8888 150 × 150 icon (slate gear, orange microphone),
replacing the component default that was upscaled from 112 px and looked blurry.
The icon generator (`tools/make_icons.py`, `tools/png2lvgl.py`) shipped with the
project, plus the `XZ_ICONSNAP` diagnostic that dumps launcher pages over serial.

## v2.2 — a level meter that follows the real signal

26 bars in a 62 px strip, between the status card and the transcript, driven by
the peak of the actual PCM frames in both audio tasks (`std::atomic<int>`, no
copy, no extra task, no allocation). Red while listening, green while the
assistant speaks, orange sweep while thinking, slow breathing at rest.
Refreshed by its own 40 ms timer, independent of the 120 ms text timer.

## v2.3 — one label

The button read `ANNULER` while connecting; it now reads `PARLER`. Nothing else
changed. It was worth a version number because the user asked for it
explicitly and the version is the unit of testing.

## v3 — the music box

A third application, and the first one that asks nothing: it composes and plays
as soon as the page opens.

Pentatonic sequencer (no dissonant interval is reachable, so a random walk is
always listenable), I–V–vi–IV chord cycle, 78–96 BPM, key drift every four
bars. Twelve voices of additive synthesis at 24 kHz from a 1024-point sine
table. UI: a 12 × 10 piano roll, the four chords with the current one
highlighted, a real-signal energy gauge, a note counter. No back button, same
reason as the Assistant.

Measured rhythm: four bars at 78 BPM in 12.3 s.

## v3.1 — two display and connection fixes

1. **The 50 px strip under apps.** An app screen is **452 px** tall (the display
   is reduced from 502 to 452 on open) while the usable area is only 402: the
   container stopped at 402 and the remaining 50 px showed the launcher behind.
   Containers now cover the **whole screen height**; the content stays laid out
   for 402 px, so nothing moved.
2. **Assistant stuck offline.** `RECONNECTER` could never be pressed (touch is
   inoperative in apps), so the app was a dead end. The button now reads
   `PARLER` in every state and the app **retries every 8 s** on its own. Opening
   the codec gained a second attempt, in case Music still holds it.

## v3.2 — the violin, and a screen that repaints itself

The synthesis engine moved from a dry decay (plucked string) to a real
**attack / sustain / release** envelope, with delayed vibrato (5.5 Hz, ±0.55 %)
and 2–5 harmonics of weight 1/n on a single phase. Eight voices instead of
twelve to hold the tempo at `-Og`. The sequencer sends note-offs, so the melody
holds most of the eighth while the pad holds the bar.

The screen composition was also fixed: `run()` is called **before** the core
loads the app screen, so LVGL did not invalidate the whole surface and static
areas kept the launcher's pixels. The screen background is now painted in the
app colour and two full invalidations (350 ms and 1.4 s after open) force a
repaint. Measured tempo: 11.82 s for four bars at 81 BPM against 11.85 s
theoretical.

## v3.3 — back to the piano

The violin was abandoned: too much CPU at `-Og`, and the result on this speaker
did not justify it. The v3.1 engine is restored verbatim (plucked string,
12 voices), verified at 11.27 s for four bars at 85 BPM against 11.29 s
theoretical, and the three display fixes of v3.2 are kept.

## v3.4 — hide the launcher while an app is open

This version finally explained the long-standing "icon visible under the app"
report: it only appeared **on the watch**, never in automated runs. The
difference is not screen composition, it is the **finger** — a real press sends
`LV_EVENT_PRESSED`, which enlarges the icon (`press_size`) and forces a launcher
relayout; a programmatic click never goes through that path.

Rather than keep chasing composition, the cause was neutralised: as soon as an
app is open, launcher icons are **reset to their normal size and hidden**; they
come back on return. An icon left enlarged by a `PRESSED` without `RELEASED` is
corrected on the way. Proof in the log, at the exact moment of opening:

```
I (11467) MUSIQUE: run OK
I (11471) MONTRE: icones du lanceur masquees (app ouverte)
```

## v3.5 — the top band, repainted

A per-row write counter (not a pixel total) settled it: with an app open, rows
**0 to 49 received no write at all**, ever. That band belongs to no screen — it
is the status bar, on the system layer, and LVGL only repaints it when its
content changes.

```
ROWCOV: writes per row 0-63: 0-49:1 50-63:3   (the 1 is the fix)
ROWCOV: rows 0-63 never written: 0 ; rows 64-451: min 3 max 20
```

The fix invalidates the 0..49 band explicitly on the system layer at every
launcher ↔ app transition, which forces the status bar to repaint over the
frozen pixels, and hides the 150 × 150 "status" icon brookesia puts in a 30 × 30
slot of that bar.

**…and the residue is still there with a finger.** See
[pitfall 21](pitfalls.md#21-count-writes-per-row-not-pixels): 2–3 rows can still
appear, three fixes did not remove it, and it has never been reproduced outside
the watch. This is written down rather than papered over.

## v3.6 — dropped, and why it is documented

The three apps declared `use_status_bar = false`, so the app screen became
410 × 502 at (0,0) and painted all 502 rows; the top band simply stopped
existing. It compiled, flashed and booted (3 485 312 bytes) — and it was
abandoned the same day: the watch lost the clock, battery and WiFi indicators
inside applications. The source changes were reverted and its binary deleted.
It is listed here so that nobody discovers the idea and thinks it was never
tried.

## Intermediate builds (not versions)

Fifteen binaries led to v2 and are kept for comparison only, not for flashing:
first assistant assembly, back button, Launcher per page, visible-area
experiments, deferred audio init (which ended the freezes), full screen, back
button at the bottom, boot-button exit, 402 px content, no back button at all.
They are not part of the version history because none of them was ever a
"state to keep".

## v4.0 — the marble maze

A fourth application, and the first one that is a game: a 19 × 17 board seen
from above, ten generated levels, a ball pushed by the accelerometer, slowed by
a finger, sent back to the last checkpoint by a shake.

Two defects were found along the way, and neither was in the maze:

1. **The QMI8658 answers at `0x6B`, not `0x6A`** — so the Sensors page had
   displayed "sensor not found" since the first version.
2. **Its scale was wrong by a factor of two.** `CTRL2 = 0x95` is ±4 g, i.e.
   8192 counts per g, not 4096: at rest the magnitude read 2.00 g, which armed
   every shake threshold permanently. After the fix, `|a| = 1.00 g`.

The axes are also rotated a quarter turn from the screen, which `imu_to_screen()`
now corrects. Details in [pitfalls 22 and 23](pitfalls.md#22-the-imu-answers-at-0x6b-and-its-scale-is-8192-counts-per-g).

Levels are generated by `tools/make_levels.py` and then **verified by
breadth-first search**: no level ships with an unreachable goal. The renderer
merges walls into horizontal segments (about 80 objects instead of 300) and moves
one 10 px ball, so only two rectangles are repainted per frame.

## v4.1 — dropped, and why

Three small sounds were added to the maze. Opening the codec from that
application made the launcher icon visible again over the screen and clipped the
display, although the audio itself initialised cleanly. Undiagnosed, so the
version was abandoned, its binary deleted and **the number burned**: the next
revision of the game is v4.2. Recorded as
[pitfall 25](pitfalls.md#25-audio-inside-perplexe-broke-the-display), because the
obvious next step is to try again.
