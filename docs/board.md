# The board

Waveshare **ESP32-S3-Touch-AMOLED-2.06**. Everything below was measured on a
real unit on 2026-10-24, not copied from a datasheet.

## SoC and memory

| Element | Value |
|---|---|
| MCU | ESP32-S3 (QFN56), revision v0.2, dual core, 240 MHz |
| PSRAM | 8 MB **octal** (AP, generation 3), 80 MHz, timing index 4 |
| Flash | 32 MB, GD (`c8 4019`), QIO 80 MHz |
| USB | native USB-Serial/JTAG, `VID_303A`:`PID_1001`, enumerates as a COM port |
| Reset | through RTS (`--before=default_reset --after=hard_reset`) |
| Download mode | hold **BOOT** (GPIO 0). The firmware never configures or reads that pin, so it stays available. |

With octal PSRAM, ESP-IDF forces `CONFIG_ESPTOOLPY_FLASHMODE="dio"` in the image
header (shared pins) and switches back to QIO at run time. That is normal, not a
regression.

## Flash layout — 32 MB

| Name | Type | Offset | Size |
|---|---|---|---|
| `nvsfactory` | data/nvs | `0x009000` | `0x032000` |
| `nvs` | data/nvs | `0x03B000` | `0x0D2000` |
| `otadata` | data/ota | `0x10D000` | `0x002000` |
| `phy_init` | data/phy | `0x10F000` | `0x001000` |
| `model` | data/spiffs | `0x110000` | `0x0F0000` |
| `factory` | app/factory | `0x200000` | `0x800000` |
| `storage` | data/spiffs | `0xA00000` | `0x600000` |

This is the **factory** layout, reproduced identically by `partitions.csv`.
Application-only updates therefore never touch `nvs` (WiFi credentials) or
`storage`.

## Display

| Element | Value |
|---|---|
| Panel | AMOLED **410 × 502** (portrait), **rounded corners** |
| Controller | CO5300, SH8601-compatible (BSP driver `sh8601`) |
| Bus | QSPI: SIO0=4, SIO1=5, SIO2=6, SIO3=7, SCL=11, CS=12 |
| Reset | GPIO 8 |
| TE | GPIO 13 |
| Backlight | driven by the PMIC, `bsp_display_brightness_set()` 10..100 |

The resolution is **physical**; it cannot be raised. To show more apps per
screen you change the launcher grid density, not the resolution.

## Touch

| Element | Value |
|---|---|
| Controller | FT3168, BSP driver `ft5x06` |
| Bus | I2C, SDA=15, SCL=14 |
| INT | GPIO 38 |
| RST | GPIO 9 |
| Area | 410 × 502 |

Touch works in the launcher and in the Settings app, and **not** inside the
Assistant and Music apps — see [pitfalls](pitfalls.md) and the roadmap.

## I2C bus (SDA 15 / SCL 14)

| Component | Address | Role | Key register |
|---|---|---|---|
| AXP2101 | `0x34` | PMIC, battery gauge | `0xA4` = battery percentage |
| QMI8658C | `0x6A` (`0x6B` alt.) | accelerometer + gyroscope | `0x00` WHO_AM_I = `0x05` |
| PCF85063ATL | `0x51` | real-time clock | `0x04..0x0A`, BCD |
| ES8311 | `0x18` (codec) / `0x30` (8-bit variant) | audio codec | — |
| ES7210 | `0x40` (`0x80` in 8-bit) | microphone ADC | — |

QMI8658 initialisation used here: `CTRL1=0x40` (auto-increment),
`CTRL2=0x95` (accelerometer), `CTRL3=0xD5` (gyroscope), `CTRL7=0x03` (enable
both). Accelerometer data from `0x35`, gyroscope from `0x3B`, temperature at
`0x33` in 1/256 °C.

The I2C driver prints `Please check pull-up resistances whether be connected
properly` on this board. It is benign: every read succeeds.

## Audio

| Element | Value |
|---|---|
| Codec | ES8311 |
| Amplifier | NS4150B, PA enable on GPIO 46 |
| I2S MCLK | GPIO 16 |
| I2S SCLK | GPIO 41 |
| I2S LRCK | GPIO 45 |
| I2S DSDIN (to codec) | GPIO 40 |
| I2S ASDOUT (from mic) | GPIO 42 |

Microphone path: ES7210 in **stereo**, channel 0 kept (channel 1 is the
reference). Hardware gain 37.5 dB, plus a ×2 software gain in `xiaozhi_client.cpp`.

## microSD

| Signal | GPIO |
|---|---|
| CMD / MOSI | 1 |
| SCK | 2 |
| D0 / MISO | 3 |
| CS | 17 |

Mounted with `bsp_sdcard_mount()` at `BSP_SD_MOUNT_POINT`. With no card inserted
the mount fails after ~6 s; that delay is normal, not a hang.

## What is deliberately not here

Serial numbers, MAC addresses, WiFi SSID and passwords, local IP addresses and
any credential of any kind. WiFi credentials live only in the watch's `nvs`
partition, entered on the device.
