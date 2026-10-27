# Pitfalls already paid for

Twenty-one problems that cost real time on this board, in the order they were
found. Each one lists the symptom as it appeared, the measurement that
identified it, and the fix that survived. Several are specific to
esp-brookesia + LVGL 9.3 + this panel; the reasoning generalises.

If you are porting this firmware to another board, read 6, 7, 8, 17, 18 and 19
before you start — they are the ones that look like something else.

---

## 1. esp-brookesia 0.8 requires ESP-IDF 6, which has no offline installer

**Symptom.** The component cannot be added: its manifest requires IDF ≥ 6.0.

**Fix.** Stay on **0.5.0**, the same family as the factory firmware. Its
manifest still resolves against IDF 5.5.1, and the shell behaves identically
for a one-app-per-page launcher.

---

## 2. `LV_COLOR_FORMAT_RGB565_SWAPPED` does not exist

**Symptom.** Build error in `esp_lvgl_port` 2.x: the symbol is unknown.

**Cause.** `esp_lvgl_port` 2.x requires LVGL ≥ 9.3, while brookesia 0.5.0 pins
`9.2.*` in its own manifest.

**Fix.** In the vendored `components/esp_brookesia/idf_component.yml`, relax the
constraint to `9.3.*`, and pin `lvgl/lvgl: "9.3.*"` in `main/idf_component.yml`.
This is patch 1 of the component — see [interface.md](interface.md#5-exact-list-of-component-patches).

---

## 3. `lv_font_montserrat_18` is not declared

**Symptom.** `error: 'lv_font_montserrat_18' undeclared`.

**Cause.** LVGL only compiles the font sizes enabled in `sdkconfig`.

**Fix.** `sdkconfig.defaults` enables **every** Montserrat size from 8 to 48. It
is not laziness: a squareline example inside the component references
`montserrat_18`, and the status bar derives its font size from the bar height,
so the exact size needed changes when the bar geometry changes.

---

## 4. The clock hides itself, permanently

**Symptom.** No clock in the status bar, and this line once in the log:

```
Clock out of area, hide it
```

**Cause.** `ESP_Brookesia_StatusBar::updateClockByNewData()` tests **once**, at
boot, whether the clock object fits in its area. At that instant the labels
still hold the placeholder string `Text` (~150 px) instead of the final time
(~62 px). If the test fails, the object is hidden with `LV_OBJ_FLAG_HIDDEN` and
nothing ever brings it back.

**Fix.** Make the left area wide enough for the placeholder, not for the time:
give it 60 % of the bar (246 px) with a 65 px margin, so `65 + 150 < 246`.
The rule to remember: **`margin + 150 px < width of area 0`**.

---

## 5. Clock and battery clipped by the rounded corners

**Symptom.** The hour is cut on the left, the battery icon is invisible on the
right.

**Cause.** The panel has rounded corners. Below roughly 40 px of margin, the
widgets sit under the corner radius.

**Fix.** 65 px of margin on both sides, with the two areas split 60 / 40 instead
of 50 / 50. Measured geometry is in [interface.md](interface.md#1-status-bar).

---

## 6. Striped screen, and icons cut in two

Two different mechanisms produce almost the same picture. Check the second one
first, because it is the one that survives a pool fix.

### 6a — LVGL's internal pool is too small

**Symptom.** Stripes, or an icon drawn in two halves. One icon works, two in a
row do not.

**Cause.** LVGL allocated from its **internal 64 KB pool**
(`CONFIG_LV_MEM_SIZE_KILOBYTES=64`). The launcher scales each icon
(112 → 150, `lv_image_set_scale`) and every transformation costs ~50 KB. The
second allocation failed and LVGL drew garbage.

The project had `CONFIG_LV_MEM_CUSTOM=y`, which is an **LVGL 8 name** and is
silently ignored in 9.3. The correct switch is the source of the malloc
functions:

```ini
CONFIG_LV_USE_CLIB_MALLOC=y
# CONFIG_LV_USE_BUILTIN_MALLOC is not set
```

With libc malloc, allocations land in PSRAM (5 MB free) and rendering is crisp.
Any new app that scales an image reopens this trap if the pool ever goes back
to the built-in allocator.

### 6b — render buffers in PSRAM, code executing from PSRAM

**Symptom.** Horizontal bands; the first band correct, the rest garbled. Icons
cut in two or three. A `lv_snapshot` of the same screen is **perfect**.

**Cause.** Two things at once: the render buffers lived in **PSRAM**, and the
code was executing from **PSRAM** too (`CONFIG_SPIRAM_FETCH_INSTRUCTIONS` +
`CONFIG_SPIRAM_RODATA`). The display DMA then competed with instruction fetches
on the same bus, and received truncated bands. Band height 60 = the height of
one render buffer (`H_RES × 60`).

**Fix.**

```c
.buffer_size = BSP_LCD_H_RES * 60,
.flags = { .buff_dma = true, .buff_spiram = false },
```

```ini
# CONFIG_SPIRAM_FETCH_INSTRUCTIONS is not set
# CONFIG_SPIRAM_RODATA is not set
```

Cost: 2 × 410 × 60 × 2 = 98 KB of internal RAM. 218 KB remain free after the
display buffers, enough for the TLS handshake of the OTA pairing.

> The snapshot being clean while the panel is striped is the key observation: it
> proves the drawing is right and the transfer is broken. Measure the transfer,
> not the scene.

---

## 7. The UI freezes when an app opens

**Symptom.** The touch stops responding, then the whole interface locks a
second later, without a panic.

**Cause.** I2C traffic on the LVGL task. Opening an app initialises the audio
codecs, which talk I2C — and the touch controller reads the same bus from
another task. Deadlock.

**Fix.** Initialise the audio in a **dedicated task**, never on the LVGL task.
The LVGL task stack also went from 12 to 16 KB: only 2.7 KB of margin were left
during `run()`.

---

## 8. `bsp_display_start_with_config` ignores `buffer_size`, `double_buffer` and `flags`

**Symptom.** Changing those fields has no effect at all.

**Cause.** The Waveshare BSP only reads `lvgl_port_cfg` from that structure and
builds its display configuration from its own Kconfig.

**Consequence.** Buffer geometry is configured in `menuconfig`/`sdkconfig`, not
in code. Do not lose time editing the struct.

---

## 9. `i2c.master: Please check pull-up resistances` is benign

**Symptom.** This warning at boot.

**Reality.** Every AXP2101 / QMI8658 / PCF85063 / FT3168 read succeeds. It is a
driver hint, not an error. Ignore it.

---

## 10. `-Werror=format-truncation`

**Symptom.** A warning that only appears at `-O2`, and fails the build because
IDF compiles with `-Werror`.

**Cause.** `snprintf` with a `%s` that can be up to 32 bytes into a small
buffer.

**Fix.** Bound the precision: `"%.32s  %d dBm%s"`. Generally: give yourself room
in every `snprintf` in this project.

---

## 11. Static callbacks must be `public`

**Symptom.** A free helper function cannot reference a class's LVGL callback.

**Fix.** Declare callbacks `public` in the app class. C++ access control applies
to the function pointer, not to how LVGL calls it.

---

## 12. Blurry launcher icon

**Symptom.** The icon looks soft, visibly resampled.

**Cause.** `esp_brookesia_app_launcher_icon.cpp` computes
`scale = default_size / header.w`. The default icon shipped 112 px wide for a
150 px target, so LVGL scaled it up.

**Fix.** Generate icons at **exactly 150 × 150** (`LV_SCALE_NONE`). One icon is
~90 KB of flash.

---

## 13. Colours inverted inside an icon

**Symptom.** A red tile renders blue.

**Cause.** For `LV_COLOR_FORMAT_ARGB8888`, LVGL expects 32-bit words in
`0xAARRGGBB` order — bytes B, G, R, A in memory. Writing `0xRRGGBBAA` gives
blue where red was intended.

**Fix.** Never write those arrays by hand; use `tools/make_icons.py` or
`tools/png2lvgl.py`.

---

## 14. `lv_obj_get_content_width()` returns 0 right after creation

**Symptom.** Children are positioned at negative x, or squeezed.

**Cause.** Geometry is only computed during the layout pass.

**Fix.** Measure the parent (whose size is explicit) after
`lv_obj_update_layout()`.

---

## 15. `#if __has_include("xz_test.h")` is not reliably re-evaluated

**Symptom.** A diagnostic build flag has no effect, with no error: the test code
is simply not compiled, because the `.cpp` did not change and the preprocessor
result was cached.

**Fix.** Drive diagnostics with `target_compile_definitions(${COMPONENT_LIB}
PRIVATE XZ_...)` in `main/CMakeLists.txt`, **and remove the line before a
production build**. Verify that `defines : 0` appears in the build output.

---

## 16. A screen dump costs five seconds of logs

**Symptom.** A module that clearly runs leaves no trace in the serial log.

**Cause.** A diagnostic snapshot is 136 × 150 px at scale 3 ≈ **55 KB of
base64**, about **5 s at 115200 baud**, during which the snapshot helper has
raised every log level to `ESP_LOG_NONE`. Whatever happens in that window never
reaches the UART.

**Rule.** Never conclude "this module is not running" from silence around a
snapshot. Space dumps out, and take one dedicated capture with no snapshot if
you need to read the log.

---

## 17. An app screen is 452 px tall, not 402

**Symptom.** A 50 px strip at the bottom shows the launcher's background; or the
content is cut.

**Cause.** When an app opens, the core reduces the display resolution from 502
to **452 px** and moves the app screen to `y = 50`. `getVisualArea()` returns
`(0,50)-(409,501)` in **absolute** coordinates, so `lv_area_get_height()` is 452
and subtracting `y1 = 50` again double-counts.

**Fix.** Size the root container from the **screen** height (452 px), keep the
content laid out for 402 px:

```cpp
const lv_area_t &va = getVisualArea();
int usable_h = lv_area_get_height(&va) - va.y1;   // 402, content budget
int screen_h = (int)lv_obj_get_height(lv_scr_act());  // 452, container height
_root = lv_obj_create(lv_scr_act());
lv_obj_set_size(_root, LV_PCT(100), screen_h);
```

Do not use `lv_obj_get_y(lv_scr_act())`: at `run()` time the screen is still at
`y = 0`, the move has not happened yet.

---

## 18. The top 50 rows belong to no screen

**Symptom.** Whatever the launcher leaves in the top 50 pixel rows stays there,
frozen, under the next app.

**Cause.** Those rows are the **status bar**, on the system layer, at
`y = 0..49`. The app screen starts at `y = 50`. LVGL only repaints a region when
an object inside it changes, and the status bar does not change — so the band is
never rewritten.

**Consequence.** A child placed at `y = -50` inside the app screen does not fix
it: LVGL 9 clips children to their parent, that rectangle is never drawn.

**Fix (v3.5).** Invalidate the area explicitly on the **system layer** at every
launcher ↔ app transition (`repaint_top_band()`), which forces the status bar to
repaint over the frozen pixels. Verified with the per-row counter: the band
receives a write at app open (`0-49:1`) where it previously received none.

> The repaint works — and the user still saw 2–3 rows of an icon under apps
> opened with a finger. So this was a real defect but not the whole story. See
> 21, still open.

---

## 19. Never add `-O2` to the main component

**Symptom.** The system freezes about one second after the Music app opens. No
log, no panic, no watchdog.

**Cause.** `-O2` on `main` changes timing enough that the audio task misses its
deadline. This is not a compiler bug; it is a real-time budget.

**Rule.** `-Og` stays. The margin comes from doing less work in the audio
engine, not from optimising it. Corollary: no library calls in the audio loop —
one `floorf()` per harmonic per sample slowed the tempo by **27 %** (measured);
a single `tanhf()` at the output is fine.

**Tempo is the best detector.** Four bars must last exactly
`32 × 60 / (2 × BPM)` seconds. Measured 11.27 s against 11.29 s theoretical at
85 BPM is a pass; 14 s is a CPU problem.

---

## 20. A flushed-pixel counter settles display arguments

**Symptom.** Endless debate about whether a region is painted.

**Technique.** Wrap `disp->flush_cb` with `lv_display_set_flush_cb` and
accumulate `lv_area_get_size(area)`. One full screen is 410 × 452 = 185 320 px.
Measured: the app repaints 2.3 screens at open, then 0 px/s at rest.

**Limitation.** A total does not say **where**. It cannot prove that a specific
row is painted. That is pitfall 21.

---

## 21. Count writes per **row**, not pixels

**Symptom.** "The band is painted, I measured it" — and the artefact is still
visible.

**Technique.** A `uint16_t hits[502]` table, incremented for every `y` covered by
`area` in the flush callback, zeroed at screen change and read 1.5 s later. This
is what showed that rows 0..49 were **never** written while 50..63 were written
three times.

```
ROWCOV: writes per row 0-63: 0-49:1 50-63:3
ROWCOV: rows 0-63 never written: 0 ; rows 64-451: min 3 max 20
```

**What it proved.** The top band was frozen (pitfall 18), and the explicit
invalidation fixed it — the `1` above is that fix.

**What it did not prove.** That the artefact is gone.

### ⚠ Still open

With a **real finger**, 2–3 rows of a launcher icon can still appear at the top
when an app opens. Three attempts (mask the launcher icons, repaint the band,
repaint from every transition) did not remove it, and it has **never been
reproduced outside the watch**, because every automated test opens apps
programmatically and therefore never goes through `LV_EVENT_PRESSED` — the
event that enlarges the icon (`press_size`) and forces a table relayout.

The one measurable difference between the failing and passing cases is the
**finger**, not the pixel composition. Anyone picking this up should start
there: instrument `LV_EVENT_PRESSED` → `RELEASED` → `CLICKED` on a real touch
sequence, and log the icon's absolute coordinates plus its scale at each step.
