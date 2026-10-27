# Changelog

All notable changes to this firmware. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the project uses
its own version rule: **MAJOR only increases when an application is added**
(see [docs/versioning.md](docs/versioning.md)).

## [Unreleased]

### Added
- 	ools/publish/: the scheduled publisher that mirrors this development
  session into the repository automatically. A version declared in the session
  becomes a release; any other change becomes a snapshot commit. See
  [docs/auto-publish.md](docs/auto-publish.md).

Candidate work is listed in [ROADMAP.md](ROADMAP.md).

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
[3.5.0]: https://github.com/withinaz/watch-esp32/releases/tag/v3.5.0
[3.4.0]: https://github.com/withinaz/watch-esp32/releases/tag/v3.4.0
