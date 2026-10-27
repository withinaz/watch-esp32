# Frozen interface

Every value below was validated on the panel. Changing one changes what the
watch looks like; changing it without reflashing changes nothing.

## 1. Status bar

File: `components/esp_brookesia/src/systems/phone/stylesheets/default/dark/status_bar_data.hpp`

| Constant | Value | Role |
|---|---|---|
| `main.background_color` | **`0xE8590C`** | orange (was `0x38393A`, grey) |
| `main.text_color` | `0xFFFFFF` | white |
| `main.size` | `RECT_PERCENT(100, 10)` | full width, 10 % height → 410 × 50 |
| `main.size_min` | `RECT_W_PERCENT(100, 24)` | minimum height 24 |
| `main.size_max` | `RECT_W_PERCENT(100, 50)` | maximum height 50 |
| `main.text_font` | `FONT_HEIGHT_PERCENT(60)` | 60 % of 50 px → ~30 px body |
| `area.num` | `2` | two areas |
| area 0 (clock) | **`(60, ALIGN_START, offset 65)`** | 246 px wide |
| area 1 (icons) | **`(40, ALIGN_END, offset 65)`** | 164 px wide |
| `area.data[i].layout_column_pad` | `3` | spacing between items |
| `icon_common_size` | `SQUARE_PERCENT(60)` | icons 30 × 30 |
| battery | 5 images (4 levels + charging), `area_index = 1` | — |
| wifi | 4 images (`close`, `level1..3`), `area_index = 1` | — |
| `enable_battery_label` | `0` | no percentage text |

Measured geometry (from the panel, not computed):

```
bar     [  0, 0 410x50]
area 0  [  0, 0 246x50]   clock [ 65,10 62x29]   date [133,14 59x21]
area 1  [246, 0 164x50]   wifi  [282,10 30x30]   battery [315,10 30x30]
```

### Why 65 px and 60 %

- **65 px**: the panel has rounded corners. At 42 px the hour was clipped on the
  left and the battery icon sat entirely under the corner on the right.
- **60 %**: `updateClockByNewData()` tests **once**, at boot, whether the clock
  fits in its area — while the labels still hold the placeholder `Text` (~150 px
  instead of 62 px). With 50 % (205 px) and a 65 px margin, `65 + 150 > 205`, and
  the clock is hidden permanently (`Clock out of area, hide it`). 60 % = 246 px
  and it fits.

Rule: **`margin + 150 px < width of area 0`**.

## 2. Date in the bar

Brookesia 0.5 displays **only** the time. The date is added by
`add_date_to_status_bar()` in `main/main.cpp`:

- label created in area 0, so the flex row places it **after** the clock;
- font `lv_font_montserrat_18`, white, `pad_left = 6`;
- format `DD/MM`, refreshed once a day by `status_timer_cb`.

Access to the status bar root object required a new `getMainObj()` accessor in
`esp_brookesia_status_bar.hpp` — the component exposed none.

## 3. Launcher — one app per page

File: `components/esp_brookesia/src/systems/phone/stylesheets/default/dark/app_launcher_data.hpp`

Page capacity is **not** a setting; it is computed by
`esp_brookesia_app_launcher.cpp`:

```
app_num_hor = table.width  / icon.main.size.width
app_num_ver = table.height / icon.main.size.height
capacity    = app_num_hor * app_num_ver
pages       = max(default_num, ceil(apps / capacity))
```

With 410 px wide and 381 px of table height:

```
410 / 210 = 1 column      381 / 210 = 1 row   ->   1 icon per page
```

| Constant | Value | Effect |
|---|---|---|
| `icon.main.size` | **`SQUARE(210)`** | **one icon per page**, centred cell |
| `table.default_num` | **`1`** | no empty trailing page; the launcher adds pages as needed |
| `table.size` | `RECT_PERCENT(100, 76)` | 76 % of the height |
| `icon.main.layout_row_pad` | `10` | — |
| `icon.image.default_size` | `SQUARE(150)` | image 150 × 150 (same as the icons) |
| `icon.image.press_size` | `SQUARE(136)` | while pressed |
| `icon.label.text_font` | `FONT_SIZE(22)` | 22 px label |
| `icon.label.text_color` | `0xFFFFFF` | white |
| `indicator.main_layout_bottom_offset` | `30` | page dots |

Each installed app therefore owns its page, in install order (Settings page 1,
Assistant page 2, Music page 3, the next one page 4…). The log shows
`All table pages are full, create a new page` at boot.

To go back to a 2 × 2 grid: `icon.main.size = SQUARE(175)` and
`table.default_num = 2`.

## 3 bis. Launcher icons

Every app provides its **own** icon; the component's default
(`esp_brookesia_image_large_app_launcher_default_112_112`) is no longer used.

| | |
|---|---|
| Directory | `main/icons/` — `app_icons.h` + one `.c` per icon |
| Format | `LV_COLOR_FORMAT_ARGB8888`, **exactly 150 × 150** |
| Alignment | `uint32_t` array (4 bytes), word = `0xAARRGGBB` |
| Flash cost | ~90 KB per icon |
| Generators | `tools/make_icons.py` (parametric) and `tools/png2lvgl.py` (from a PNG) |

**Why 150 × 150:** `esp_brookesia_app_launcher_icon.cpp` computes
`scale = default_size / header.w`, i.e. 150/150 = `LV_SCALE_NONE`. Any other
size is resampled by LVGL — the original 112 px icon upscaled to 150 was
visibly blurry.

**Wiring:** the app constructor takes the descriptor as its second argument:

```cpp
ESP_Brookesia_PhoneApp("Parametres", &app_icon_settings, true, true, true)
```

and `main/CMakeLists.txt` lists the `.c` files in `SRCS`.

**Icon charter** (follow it for the next apps): 150 px square tile, 34 px corner
radius, vertical gradient, light top edge, white glyph centred and covering
~56 % of the tile, one hue per app.

| Icon | Tile | Glyph |
|---|---|---|
| `app_icon_settings` | slate `0x415074` → `0x161B26` | 8-tooth gear, `0xE9F1FF` |
| `app_icon_assistant` | orange `0xFF9344` → `0xC23B06` | microphone, white |
| `app_icon_music` | violet `0x7B5CF0` → `0x2A185C` | double eighth note, white |

## 4. App palette

Shared by all three apps, defined at the top of `app_settings.cpp` and repeated
in the other apps:

| Macro | Value | Use |
|---|---|---|
| `COL_BG` | `0x0E0E10` | screen background |
| `COL_CARD` | `0x1C1C1E` | cards and rows |
| `COL_TEXT` | `0xFFFFFF` | primary text |
| `COL_ACCENT` | `0x00E5A0` | titles, action buttons (mint) |
| `COL_DIM` | `0x8A8A8E` | secondary text |
| `COL_WARN` | `0xFF9F0A` | warnings (no WiFi, missing sensor) |

Status bar images live in
`components/esp_brookesia/src/systems/phone/assets/images/large/`
(`battery_level1..4_36_36`, `battery_charge_36_36`, `wifi_close_36_36`,
`wifi_level1..3_36_36`).

## 4 bis. Assistant app geometry

File: `main/app_assistant.cpp`.

| Element | Geometry |
|---|---|
| Root container | width 100 %, **height = screen height (452 px)**, inner margin 12; content laid out for 402 |
| Title `Assistant` | `TOP_MID`, y = 4, font 24 |
| Status card | `TOP_MID`, y = 42, height 74 (12 px dot at x = 16; state 22 px at (40, 10); detail 14 px at (40, 40)) |
| Level meter | y = 124, height **62**, full width, 26 bars of 10 × 6..56 px, step 3 px (x0 = 25) |
| Transcript | y = 194, height `402 - 60 - 20 - 194` = **128**, vertical scroll, column flex |
| Action button | full width, height 60, `BOTTOM_MID`, y = −2 |
| Back button | **none** (known limitation: touch is inoperative in this app) |

Bars are vertically centred (`y = (62 - h) / 2`) and **absolutely positioned**:
no flex inside the strip, so changing one bar's height invalidates only its own
rectangle.

Button label by state: `PARLER` (ready, connecting, offline), `VERIFIER`
(pairing), `ENVOYER` (listening), `COUPER` (speaking), `REESSAYER` (error).
Offline, the app retries by itself every 8 s.

Bar colours: red while listening, green while speaking, orange while thinking,
dark green at rest, grey offline.

## 4 ter. Music app geometry

File: `main/app_music.cpp`. Same palette, same root-container rule.

| Element | Geometry |
|---|---|
| Title `Musique` | `TOP_MID`, y = 2, font 24, accent colour |
| Status card | `TOP_MID`, y = 36, height 62 (dot at x = 16; state 20 px at (40, 8); key/BPM 14 px at (40, 34)) |
| Piano roll | `TOP_MID`, y = 104, height **186**, radius 16; 12 columns × 10 rows, cells 23 × 15, step 5 × 3 → 331 × 177, centred (x0 = 27, y0 = 4) |
| Chord cycle | y = 296, height 32, four 88 × 32 pills, `SPACE_BETWEEN` |
| Energy gauge | y = 334, height 8 (`COL_CARD` track, accent fill) |
| Footer | `TOP_MID`, y = 348, font 12 (`n notes . n voices`) |
| Back button | **none** (same reason as the assistant) |

In the roll, row 0 is the **lowest** note and is drawn at the bottom
(`y = y0 + (9 - row) × 18`). The rightmost column is the current eighth; older
columns fade (0.42 → 1.0). Per-row colour: indigo `0x3D6BFF` (low) → turquoise
`0x00E5A0` (high), off cell `0x232327`.

Audio: 24 kHz mono 16-bit, volume 68, blocks of 480 samples (20 ms), paced by
`esp_codec_dev_write`.

Timbre in v3.5: **plucked string** (music box), 12 voices, one or two detuned
sine components per note, short attack and exponential decay. The sequencer
sends note-offs so the melody holds most of the eighth while the pad holds the
whole bar.

## 5. Exact list of component patches

`components/esp_brookesia` is a local copy of 0.5.0, **modified**. Do not
replace it with the registry version.

1. `idf_component.yml`: LVGL constraint relaxed from `9.2.*` to `9.3.*`
   (`esp_lvgl_port` 2.x requires 9.3, otherwise
   `LV_COLOR_FORMAT_RGB565_SWAPPED` is undeclared).
2. `status_bar_data.hpp`: orange, 60/40 areas, 65 px margins, helper extended
   with a `start_offset` parameter.
3. `app_launcher_data.hpp`: `icon.main.size = SQUARE(210)` and
   `default_num = 1` → one app per page, 210 px cells, 150 px images.
4. `esp_brookesia_status_bar.hpp`: `getMainObj()` accessor added.

## 6. Fonts

`sdkconfig.defaults` enables every Montserrat size from 8 to 48. This is
required: a squareline example inside the component references
`montserrat_18`, and the status bar derives its font size from the bar height.
A missing size breaks the build with `lv_font_montserrat_XX undeclared`.
