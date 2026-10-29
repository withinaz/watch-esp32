# Architecture

## Boot sequence

`app_main()` in `main/main.cpp` does, in this order:

1. **Time zone first.** `TZ=CET-1CEST,M3.5.0,M10.5.0/3` and `tzset()` before
   anything else, so any timestamp printed during boot is local time.
2. **Services** (NVS, event loop, netif).
3. **RTC read.** The PCF85063 is read and pushed into the system clock, so the
   watch shows the right time immediately, without waiting for WiFi. If the RTC
   has no valid time, the log says so and the clock waits for NTP.
4. **Display.** `bsp_display_start_with_config()` starts the panel, the LVGL
   task (priority 4, 16 KB stack) and the touch driver; brightness 80 %, then
   backlight on.
5. **Shell.** `ESP_Brookesia_Phone` is created on top of the display, given the
   touch device and the LVGL lock/unlock callbacks, then `begin()`.
6. **Applications**, in install order — this order *is* the page order:
   `AppSettings`, `AppAssistant`, `AppMusic`. The launcher allocates one page
   per app and logs `All table pages are full, create a new page` each time it
   has to grow.
7. **Status bar timer** (1 s): clock text, date, battery, WiFi bars.
8. **WiFi**, then **SNTP**, then the synchronised time is written back to the
   RTC.

Everything that talks to I2C off the LVGL task is deliberate — see
[pitfall 7](pitfalls.md#7-the-ui-freezes-when-an-app-opens).

## Tasks

| Task | Priority | Stack | Core | Role |
|---|---|---|---|---|
| `main` | 1 | 8 KB (static) | 0 | boot, then idle |
| LVGL (esp_lvgl_port) | 4 | **16 KB** | 0 | one `lv_timer_handler()` loop; every widget call must be under `bsp_display_lock()` |
| `xz_connect` | 5 | 12 KB | 0 | OTA pairing + WebSocket for the assistant |
| `xz_mic` | 7 | **64 KB static, in PSRAM** | 1 | ES7210 → Opus encode → WebSocket |
| `xz_spk` | 6 | **48 KB static, in PSRAM** | 1 | WebSocket → Opus decode → ES8311 |
| `mus_audio` | 5 | 8 KB | 0 | music sequencer + synthesis, paces itself on `esp_codec_dev_write` |
| `prx_imu` | 3 | 4 KB | 0 | accelerometer sampling at 30 Hz for the maze, atomics only |
| `xz_start` | 5 | 8 KB | 0 | app-open hand-off, so `run()` stays short |

The two audio stacks are the reason `CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY`
is enabled: `78/esp-opus` is built with `USE_ALLOCA` and needs more than 16 KB
per `opus_encode()` call. A 32 KB stack cannot even be allocated in internal
RAM. Symptom if you get this wrong: `vApplicationStackOverflowHook`, then
`Guru Meditation ... |<-CORRUPTED`.

## Memory

### Flash — 32 MB, factory layout

| Partition | Type | Offset | Size |
|---|---|---|---|
| `nvsfactory` | data/nvs | `0x009000` | `0x032000` |
| `nvs` | data/nvs | `0x03B000` | `0x0D2000` |
| `otadata` | data/ota | `0x10D000` | `0x002000` |
| `phy_init` | data/phy | `0x10F000` | `0x001000` |
| `model` | data/spiffs | `0x110000` | `0x0F0000` |
| `factory` | app/factory | **`0x200000`** | `0x800000` |
| `storage` | data/spiffs | `0xA00000` | `0x600000` |

The project reproduces the factory layout **exactly**, which is what lets an
application-only flash keep the `nvs` partition — and therefore the WiFi
credentials — intact. The application occupies 3.59 MB of the 8 MB slot.

### RAM

| Resource | Budget |
|---|---|
| Display buffers | 2 × 410 × 60 × 2 = **98 KB**, internal RAM, DMA-capable |
| Internal RAM free after display | **~218 KB** (enough for a TLS handshake) |
| PSRAM | 8 MB octal @ 80 MHz, used by LVGL's allocator and the audio stacks |
| LVGL allocator | libc malloc (`CONFIG_LV_USE_CLIB_MALLOC`), so widget allocations land in PSRAM |

`CONFIG_SPIRAM_FETCH_INSTRUCTIONS` and `CONFIG_SPIRAM_RODATA` are **off**. The
code stays in flash; see [pitfall 6](pitfalls.md#6-striped-screen-and-icons-cut-in-two).

## Display pipeline

```
app widget tree
   └─ LVGL 9.3 render  ──► partial buffers (410 × 60, 2 of them)
          └─ flush_cb  ──► CO5300 over QSPI (DMA)
```

LVGL is configured for 16-bit colour. The panel is 410 × 502; when an app opens,
the core shrinks the *display* to 410 × 452 and places the app screen at
`y = 50`, under the status bar. That 50 px difference is the source of two
pitfalls ([17](pitfalls.md#17-an-app-screen-is-452-px-tall-not-402) and
[18](pitfalls.md#18-the-top-50-rows-belong-to-no-screen)).

## Why this stack

| Layer | Choice | Why |
|---|---|---|
| Shell | esp-brookesia **0.5.0**, vendored | Same family as the factory firmware; 0.8 needs ESP-IDF 6, which has no offline installer here. Patched for LVGL 9.3 and the one-app-per-page launcher. |
| Graphics | LVGL **9.3** | Imposed by `esp_lvgl_port` 2.x, which the BSP pulls in. |
| Board | `waveshare/esp32_s3_touch_amoled_2_06` **^2.0.0** | Display, touch, audio, PMIC, IMU, RTC and SD in one component, with the pin map already right. |
| Audio | `esp_codec_dev` + ES8311/ES7210 | Same codecs the BSP configures; `esp_codec_dev_write` blocks, which conveniently paces the music generator. |
| Voice | `esp_websocket_client` + `78/esp-opus` | The XiaoZhi transport is a plain WebSocket; Opus at 24 kHz keeps frames at ~150 bytes. |
| Build | ESP-IDF **5.5.1**, CMake, no external dependency in the app code | Everything is in the tree; a clone builds with `idf.py build`. |

## Code map

| File | Lines | What it owns |
|---|---|---|
| `main/main.cpp` | ~830 | boot, WiFi, NTP, RTC, status bar, I2C helpers, diagnostic harness |
| `main/app_settings.cpp` | ~800 | seven pages, WiFi scan/connect, sensors, storage |
| `main/app_assistant.cpp` | ~600 | assistant UI + level meter + state machine |
| `main/xiaozhi_client.cpp` | ~950 | OTA pairing, WebSocket, Opus, mic/speaker tasks |
| `main/app_music.cpp` | ~1080 | sequencer, synthesis, piano-roll UI |
| `main/app_perplexe.cpp` | ~1000 | maze: levels, physics, IMU steering, rendering |
| `main/icons/*.c` | 3 × 90 KB | launcher icons, generated |
| `components/esp_brookesia` | vendored | the shell, with four patches |

## Where the documentation lives

- Geometry, colours, patch list → [interface.md](interface.md)
- Pinout, I2C, partitions → [board.md](board.md)
- Build and flash → [build.md](build.md), [flash.md](flash.md)
- Assistant internals → [assistant.md](assistant.md)
- Everything that went wrong → [pitfalls.md](pitfalls.md)
