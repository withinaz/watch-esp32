# Diagnostics

The serial port is the only window on this device. No debugger, no on-screen
console: just `ESP_LOG` and, when you need it, a framebuffer dump. These are the
tools that made 21 pitfalls findable.

## Capture the log

```powershell
python tools\capture.py COM4 serial.log 30
```

Waits for the port to be released after a flash, then reads for 30 s and writes
the raw stream. `tools/snap.py` can decode it afterwards.

Without the helper:

```powershell
python -c "import serial,time; s=serial.Serial('COM4',115200,timeout=0.1); t=time.time(); b=b''
while time.time()-t<15: b+=s.read(8192)
print(b.decode('utf-8','replace'))"
```

## Dump the framebuffer

The firmware can print a screen as base64 over the UART; `tools/snap.py` turns
that into a PNG.

```
SNAP BEGIN <tag> <width> <height>
<base64, possibly split over many lines>
SNAP END
```

```powershell
python tools\capture.py COM4 snap.log 95
python tools\snap.py snap.log snaps
```

`snap-<TAG>.png` appears in `snaps/`. Enable the dump with `XZ_SNAPSHOT` (see
below).

**This costs about 5 seconds of log.** 136 × 150 px at scale 3 is ~55 KB of
base64, ~5 s at 115200 baud, and during that window the snapshot helper raises
every log level to `ESP_LOG_NONE`. Lines that fall in that window are lost
forever — never conclude "this module is not running" from silence around a
snapshot ([pitfall 16](pitfalls.md#16-a-screen-dump-costs-five-seconds-of-logs)).

## Build-time switches

Add them in `main/CMakeLists.txt`, **and remove them before a production
build**:

```cmake
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    XZ_AUTOTEST XZ_AUTOTEST_PAGE=2 XZ_AUTOTEST_MS=42000 XZ_ICONSNAP)
```

| Define | Effect |
|---|---|
| `XZ_SNAPSHOT` | dumps the current screen once, tag `APP` |
| `XZ_ICONSNAP` | captures **all three launcher pages** at t = 5 s, before any app opens |
| `XZ_AUTOTEST` | opens the app of page `XZ_AUTOTEST_PAGE` (0 = Settings, 1 = Assistant, 2 = Music) after `XZ_AUTOTEST_MS` ms, by sending `LV_EVENT_CLICKED` |
| `XZ_AUTOTEST_BACK` | presses the back path 22 s later — an automated test of returning to the launcher |
| `XZ_MUSSNAP` | captures the Music app three times in a row (use with `XZ_AUTOTEST_PAGE=2`) |

Verify `defines : 0` in the build output afterwards. `#if __has_include(...)`
is **not** reliably re-evaluated when a header appears without the `.cpp`
changing ([pitfall 15](pitfalls.md#15-__has_include-is-not-reliably-re-evaluated)),
which is why these are compile definitions and not a magic header.

### The known blind spot

`XZ_AUTOTEST` sends `LV_EVENT_CLICKED` directly. A real finger sends
`LV_EVENT_PRESSED` → `LV_EVENT_RELEASED` → `LV_EVENT_CLICKED`, and the first of
those **enlarges the icon** (`press_size`) and forces a table relayout. Any bug
that lives in the press path is invisible to the harness. That is exactly how
the residue described in [pitfall 21](pitfalls.md#21-count-writes-per-row-not-pixels)
survived three fixes.

## Measuring what is actually painted

Two instruments, both temporary, both worth the ten lines they cost.

### 1. Flushed pixels

Wrap the display flush callback and accumulate the area size:

```cpp
static uint32_t s_px;
static void count_flush(lv_display_t *d, const lv_area_t *a, uint8_t *px)
{
    s_px += lv_area_get_size(a);
    /* call the original callback here */
}
```

One full screen = 410 × 452 = **185 320 px**. Measured on this app: 2.3 screens
repainted at open, then 0 px/s at rest. Useful to prove "the app does repaint",
useless to know **where**.

### 2. Writes per row

```cpp
static uint16_t s_hits[502];
/* in the flush callback: */
for (int y = a->y1; y <= a->y2; y++) s_hits[y]++;
```

Zero the table at screen change, read it 1.5 s later. This is the instrument
that showed rows 0..49 were never written while 50..63 were written three times
— and therefore that the top band belonged to the status bar layer, not to the
app ([pitfall 18](pitfalls.md#18-the-top-50-rows-belong-to-no-screen)).

## Reading a boot log

A clean v3.5 boot:

```
MONTRE: === montre : demarrage ===
MONTRE: RTC PCF85063 : reprise 08:31 le 27/10/2026
MONTRE: ecran 410x502 pret
MONTRE: RAM interne libre : ... o (plus gros bloc ... o)
Brookesia: Library version: 0.5.0
Brookesia: (addIcon): All table pages are full, create a new page
MONTRE: === montre : interface prete ===
MONTRE: batterie : lecture AXP2101 0xA4 -> ESP_OK, valeur 100
MONTRE: WiFi : connecte, IP ...
MONTRE: NTP : heure synchronisee -> ...
```

Things that are **normal** and look like errors:

| Line | Meaning |
|---|---|
| `i2c.master: Please check pull-up resistances` | benign on this board, all reads succeed |
| `create a new page` three times | one per app beyond the first — four apps, three lines |
| `sdmmc: no card` after ~6 s | no microSD inserted; the mount is retried by the app only |
| `wifi: ... reason 2` at boot | first association attempt, retried automatically |

Things that are **not** normal:

| Line | Meaning |
|---|---|
| `Clock out of area, hide it` | status bar geometry is wrong; the clock is hidden for the session |
| `Guru Meditation` | crash — read the backtrace, check the audio stack sizes |
| `vApplicationStackOverflowHook` | almost always `opus_encode` and a 16 KB stack |
| `esp_lcd_panel_io_tx_color` timeouts | display DMA starved by PSRAM code execution |

## Reproducing a report

When a user reports a visual defect, ask two questions before touching code:

1. **Where exactly** — in the orange band, just below it, or elsewhere?
2. **With a finger or with the autotest?** A defect that only appears with a
   finger is in the press path, and no amount of pixel counting will find it.
