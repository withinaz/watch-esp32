# Contributing

This is a personal firmware project published in the open. Bug reports and
patches are welcome; here is what makes them actionable.

## Reporting a bug

The most useful report contains:

1. **the version** (`VERSION`, or the release you flashed);
2. **what you did** — including whether you used a finger, the launcher, or a
   script;
3. **what you expected** and **what happened**;
4. **the serial log**, at least from boot to the problem, with
   `python tools/capture.py COM4 serial.log 30`;
5. for a visual defect, **where** on the panel: in the orange band, just below
   it, elsewhere. This distinction has decided more than one investigation.

A report that says "the screen glitches" cannot be acted on. A report that says
"rows 0–2 keep the icon's top edge, only when I tap with a finger, not when the
autotest opens the app" can.

## Sending a change

- Open an issue first for anything larger than a typo. The design constraints
  here are unusual (one app per page, 452 px screens, no `-O2`) and a patch that
  ignores them will be rejected even if it is good code.
- Keep the change to one subject. A PR that fixes a pitfall and reformats a file
  cannot be reviewed.
- **Test on the board.** A change that only compiles is not tested; the display
  pipeline has three separate failure modes that a build cannot see.
- Update `CHANGELOG.md` and, if behaviour changed, the relevant document under
  `docs/`.
- Run `python tools/verify_repo.py` before pushing. CI runs the same script.

## Code conventions

| | |
|---|---|
| Language | C++17, ESP-IDF style, 4-space indent, 100 columns |
| Comments | French in the app sources (the project's working language), English in tools and docs |
| Logging | `ESP_LOGI(TAG, ...)`, `TAG` is the app name (`MONTRE`, `MUSIQUE`, `XIAOZHI`) |
| LVGL | every call from a non-LVGL task must hold `bsp_display_lock()` |
| Blocking work | never in `run()` and never on the LVGL task (see pitfall 7) |
| Compiler flags | `-Og` on `main`, no exceptions |
| Dependencies | none beyond ESP-IDF and `dependencies.lock`; the app code has no third-party runtime dependency |

## What will not be accepted

- Secrets in the repository: no WiFi credentials, no API key, no private key, no
  token. The watch stores its credentials in `nvs`; the repository must never
  contain any.
- A change to `partitions.csv` that moves `nvs` — it would wipe credentials on
  every update.
- A replacement of `components/esp_brookesia` by the registry version, which
  silently reverts the four patches listed in
  [docs/interface.md](docs/interface.md#5-exact-list-of-component-patches).
- `-O2` on the main component, for the reason documented in
  [pitfall 19](docs/pitfalls.md#19-never-add--o2-to-the-main-component).
- Renamed or moved files without a reason: the paths are referenced from the
  documentation and from the release checklist.

## Licence of contributions

By opening a pull request you agree that your contribution is licensed under
Apache-2.0, like the rest of the repository.
