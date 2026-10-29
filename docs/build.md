# Build

## What you need

| | |
|---|---|
| ESP-IDF | **5.5.1** (5.5.x should be fine; 5.6+ is untested) |
| Python | 3.11, the one bundled with ESP-IDF is enough |
| Toolchain | Xtensa `esp-14.2.0_20241119` or newer, installed by the IDF installer |
| Disk | ~4 GB for the toolchain, ~1 GB for `build/` and `managed_components/` |
| Network | required **on the first build only** (component registry) |

Nothing else. The application code has **no third-party runtime dependency**;
everything it uses comes from ESP-IDF or the component manager.

## First build

```powershell
# 1. Load the ESP-IDF environment (adjust the paths to your install)
$env:IDF_PATH = 'C:\Espressif\frameworks\esp-idf-v5.5.1'
& "$env:IDF_PATH\export.ps1"

# 2. Build
cd watch-esp32
idf.py build
```

Expected tail:

```
Project build complete. To flash, run:
 idf.py flash
```

and a binary at `build/montre.bin`:

```
3 589 296 bytes   sha256 51ca2508...
```

The first build resolves `dependencies.lock` and downloads LVGL 9.3,
`esp_lvgl_port`, the Waveshare BSP, `esp_codec_dev`, `esp_websocket_client` and
`78/esp-opus` into `managed_components/` (~165 MB, git-ignored).

> A rebuild does **not** reproduce the reference binary byte for byte: a few
> bytes differ in a literal pool, on a three-and-a-half megabyte image. That is
> why `firmware/` ships the tested image: a rebuild proves the sources compile,
> not that it is the same artefact.

## Never run `idf.py set-target`

The target is already `esp32s3` in `sdkconfig`. `set-target` regenerates
`sdkconfig` from scratch and silently loses the settings this board needs —
octal PSRAM at 80 MHz, 32 MB flash, the LVGL font set, the disabled PSRAM
instruction fetch. You will get a build that compiles and a watch that shows
stripes.

If you must start over, delete `sdkconfig` (not `sdkconfig.defaults`) and let
it be regenerated.

## `sdkconfig.defaults`, key by key

| Key | Why it is there |
|---|---|
| `CONFIG_SPIRAM_MODE_OCT`, `SPEED_80M` | 8 MB octal PSRAM at 80 MHz |
| `# CONFIG_SPIRAM_FETCH_INSTRUCTIONS is not set` | code stays in flash: the display DMA must not share the PSRAM bus ([pitfall 6](pitfalls.md#6-striped-screen-and-icons-cut-in-two)) |
| `# CONFIG_SPIRAM_RODATA is not set` | same reason |
| `CONFIG_ESPTOOLPY_FLASHSIZE_32MB` | the board has 32 MB |
| `CONFIG_PARTITION_TABLE_CUSTOM` + `partitions.csv` | factory layout, so `nvs` survives updates |
| `CONFIG_LV_COLOR_DEPTH_16` | 16-bit colour, 2 bytes per pixel |
| `CONFIG_LV_USE_CLIB_MALLOC` / `# CONFIG_LV_USE_BUILTIN_MALLOC` | LVGL allocations go through libc, therefore into PSRAM. `LV_MEM_CUSTOM` is an LVGL **8** name and is silently ignored in 9.3 ([pitfall 6a](pitfalls.md#6-striped-screen-and-icons-cut-in-two)) |
| `CONFIG_LV_FONT_MONTSERRAT_8..48` | every size must exist; the status bar font is derived from the bar height |
| `CONFIG_LV_USE_SNAPSHOT` | framebuffer dumps for diagnostics |
| `CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192` | boot does TLS-free work but a lot of it |
| `CONFIG_FREERTOS_HZ=1000` | 1 ms tick, needed for the audio scheduling |

## Checking a build

```powershell
idf.py size            # total image, DRAM, IRAM, flash
idf.py size-components # per-component breakdown
```

Sanity values for v3.5:

| | |
|---|---|
| Application | 3 589 296 bytes (of 8 MB) |
| Internal RAM free after display init | ~218 KB |
| Compiler flags on `main` | `-Og`, **never `-O2`** ([pitfall 19](pitfalls.md#19-never-add--o2-to-the-main-component)) |

`main/CMakeLists.txt` carries a comment explaining why `-O2` must not be added.
It is not decoration: the system freezes one second after opening the Music app
if you add it.

## Common build failures

| Error | Cause | Fix |
|---|---|---|
| `LV_COLOR_FORMAT_RGB565_SWAPPED undeclared` | LVGL 9.2 pulled instead of 9.3 | check `components/esp_brookesia/idf_component.yml` still says `9.3.*` |
| `lv_font_montserrat_18 undeclared` | font size disabled | re-apply `sdkconfig.defaults`, delete `sdkconfig`, rebuild |
| `-Werror=format-truncation` in `app_settings.cpp` | a `%s` wider than the buffer | bound it: `%.32s` |
| component manager error on first build | no network, or a proxy | connect once, or pre-populate `managed_components/` |
| `idf.py: command not found` | environment not exported | run `export.ps1` in the same shell |

## Adding a file

Source files are listed explicitly in `main/CMakeLists.txt` — there is no glob.
A new `.cpp` that is not listed simply does not compile, without an error
saying so.
