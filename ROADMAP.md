# Roadmap

Written as an engineering plan, not a wish list: every item says what "done"
means. Nothing here is committed to a date.

## Next up

| # | Item | Why it matters | Effort | Done when |
|---|---|---|---|---|
| 1 | **Touch inside applications** | Without it, no app can offer a usable button — it is the real blocker for everything else | ? | a tap on a button in the Assistant is delivered to the app and logged |
| 2 | **The 2–3 pixel residue at the top** | Open defect; three fixes failed | ? | opening an app with a finger leaves no launcher pixel |
| 3 | ~~Fourth application (v4.0)~~ | **Done** — Perplexe shipped in v4.0 | — | — |
| 4 | **BLE notifications** | A watch that does not show your phone's messages is a demo | L | a notification from a phone appears in an app |
| 5 | **OTA updates** | Currently every update needs a USB cable | M | the watch updates itself over WiFi, with a rollback slot |

## 1. Touch inside applications — the investigation, not the guess

Facts already established:

- the launcher and the Settings app receive touch through the same FT3168 and
  the same `bsp_display_get_input_dev()`;
- the Assistant and Music apps receive **nothing**, while their timers, audio
  and rendering all run;
- the apps are full-screen containers created on `lv_scr_act()`, and their
  root object is `LV_PCT(100) × screen height`.

Hypotheses worth testing, in order:

1. **The app screen is not the input device's target.** Register a
   `lv_indev_set_group()` / log `lv_indev_get_act()` and `lv_indev_get_obj_act()`
   while pressing inside an app. If `act` is null, the press never reaches the
   screen; if it is set, the widget tree is the problem.
2. **The status bar layer keeps the touch.** Print `lv_layer_sys()`'s children
   and their coordinates at app open. A transparent full-screen object on the
   system layer would swallow everything — and would also explain why only apps
   opened from the launcher are affected.
3. **A second indev still bound to the home screen.** Brookesia creates its own
   input handling; check whether it re-registers an indev when a screen loads.

Instrument first: `ESP_LOGI` on `LV_EVENT_PRESSED` in a full-screen test object,
plus the two `lv_indev_get_*` calls. Ten lines, one flash, and the answer is
binary.

## 2. The top band residue

The band is now repainted (measured: rows 0..49 receive a write at app open)
and the residue still appears with a real finger, so the frozen-band theory was
necessary but not sufficient. The remaining difference between the failing and
passing cases is the press path: `LV_EVENT_PRESSED` enlarges the icon
(`press_size`) and forces a relauncher layout.

Next step is to stop reasoning and record the sequence: log the icon's absolute
coordinates and its scale at `PRESSED`, `RELEASED` and `CLICKED`, plus the first
and last row written by the flush callback between those events. The per-row
counter already exists ([diagnostics.md](docs/diagnostics.md)); it only needs a
time window instead of a total.

## 3. The fourth application — done

**Perplexe shipped in v4.0** (page 4): a 19 × 17 marble maze, ten generated
levels, steered by the accelerometer. It also proved that touch works inside an
application — see [pitfall 24](docs/pitfalls.md#24-touch-does-work-in-a-new-application)
— which narrows the Assistant defect considerably.

What it left behind: the IMU is now correctly addressed (`0x6B`) and correctly
scaled (8192 counts per g), so the next sensor application starts from a sensor
that tells the truth.

The fifth application would open **v5.0** and take page 5. The candidates that
remain interesting:

| Idea | Uses | Notes |
|---|---|---|
| **Compass and level** | QMI8658 | now that the axes are mapped, it is nearly free |
| **Sleep / activity** | QMI8658 + AXP2101 | needs a background sampler and flash storage |
| **Photo frame** | microSD | reads JPEGs from the card; proves the storage path |
| **Chronometer** | nothing but LVGL | trivial, but a useful reference implementation |

## 4. BLE notifications

Requires a companion app on the phone (there is no way around it) and a GATT
service the watch subscribes to. Minimal viable version: one characteristic
carrying `{app, title, body}` as JSON, rendered in a new app, with a vibration
or a tone on arrival. Then: dismiss from the watch, and a battery cost budget
(BLE advertising is cheap; a persistent connection is not).

## 5. OTA

The partition table has `otadata` but only **one** application slot
(`factory`, 8 MB). A real OTA needs two slots. Splitting the 8 MB factory
partition into `ota_0` / `ota_1` of 4 MB each is possible (the app is 3.59 MB)
and does not touch `nvs`, but it is a one-way layout change: going back to the
factory firmware would then need a full 16 MB write.

The sane plan: a new partition table for v4, shipped with a note that upgrading
crosses a boundary, plus `esp_https_ota` against a signed manifest and a
rollback check on first boot.

## Longer term

- **Watch faces** — the natural home of the "one app per page" launcher is a
  fourth kind of page: a face gallery, with the time as a widget rather than a
  bar.
- **A2A on the wrist.** The sibling project
  [`communityofbillions`](https://github.com/withinaz/communityofbillions)
  defines an agent identity and signed envelopes (COB/1). A watch is a
  surprisingly good place for the *human* end of that protocol: display a
  payment request, approve it with a physical tap, keep the private key in a
  partition the app cannot read back. This is exploration, not a plan — it
  depends on items 1 and 5 being solved first.
- **Host-side simulator.** LVGL runs on a desktop; a small SDL harness would let
  contributors work on UI without a board. The blocker is that the shell and the
  BSP are board-specific, so it would only simulate individual apps.
- **CI that builds the firmware.** Currently CI verifies the repository
  (checksums, structure, version consistency) because an ESP-IDF build in
  Actions costs ~10 minutes per push. Worth it once the project has external
  contributors.

## Explicitly not planned

- **A port to another board.** The BSP, the pin map and the panel geometry are
  this board. A port is a fork, not a branch.
- **Local speech recognition.** The ESP-SR models need a 960 KB partition and
  would still be worse than the server.
- **An app store.** The launcher is compiled in, deliberately: it is firmware,
  not a platform.
