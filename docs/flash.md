# Flash

## The short version

```powershell
cd firmware
.\flash.ps1            # verifies SHA-256, then writes the app at 0x200000
```

Everything below explains what that does and how to undo it.

## What gets written, and where

| Image | Offset | Needed for |
|---|---|---|
| `bootloader.bin` | `0x0` | a full write only |
| `partition-table.bin` | `0x8000` | a full write only |
| `montre.bin` | **`0x200000`** | every update |

An **application-only** write (the default) replaces the `factory` partition and
nothing else. `nvs` at `0x3B000` is untouched, so WiFi credentials, the RTC
calibration and anything else stored there survive. The bootloader and partition
table only change when the layout itself changes — which it does not, because
`partitions.csv` mirrors the factory one.

## Manual flash

```powershell
python -m esptool --chip esp32s3 -p COM4 -b 921600 --before default_reset `
  --after hard_reset write_flash --flash_mode dio --flash_freq 80m --flash_size 32MB `
  0x200000 firmware\v3.5\montre.bin
```

`--flash_mode dio` is required even though the flash runs in QIO: with octal
PSRAM the image header must be DIO (shared pins), and the ROM re-enables QIO at
run time.

Full write:

```powershell
python -m esptool --chip esp32s3 -p COM4 -b 921600 --before default_reset `
  --after hard_reset write_flash --flash_mode dio --flash_freq 80m --flash_size 32MB `
  0x0      firmware\v3.5\bootloader.bin `
  0x8000   firmware\v3.5\partition-table.bin `
  0x200000 firmware\v3.5\montre.bin
```

## Finding the port

```powershell
Get-CimInstance Win32_PnPEntity | Where-Object { $_.Name -match 'COM\d+' } |
  Select-Object Name
```

The board is a native USB-Serial/JTAG device (`VID_303A`/`PID_1001`). It appears
as a normal COM port; no driver beyond the built-in one is needed on Windows 10
or 11.

Baud rate barely matters on this transport — the USB-JTAG link is not a UART, and
every tool reprograms the rate anyway. Measured throughput: 91 kbit/s at 115200,
580 kbit/s at 921600, 655 kbit/s at 1500000. `-b 921600` is the sweet spot.

## Verify the flash

`esptool` prints `Hash of data verified.` on success. Then read the boot log:

```powershell
python -c "import serial,time; s=serial.Serial('COM4',115200,timeout=0.1); t=time.time(); b=b''
while time.time()-t<15: b+=s.read(8192)
print(b.decode('utf-8','replace'))"
```

Expected, in this order:

```
MONTRE: === montre : demarrage ===
MONTRE: ecran 410x502 pret
Brookesia: Library version: 0.5.0
Brookesia: (addIcon): All table pages are full, create a new page
MONTRE: === montre : interface prete ===
MONTRE: batterie : lecture AXP2101 0xA4 -> ESP_OK, valeur 100
MONTRE: WiFi : connecte, IP ...
MONTRE: NTP : heure synchronisee -> ...
```

A clean boot has no `Guru Meditation`, no `abort()`, no `Clock out of area,
hide it`, and `create a new page` appears **three times** for four apps (once per app
beyond the first) — that line is the proof that each app owns its own page.

## If the board does not come back

1. Unplug, hold **BOOT** (GPIO 0), replug, release. The ROM bootloader stays in
   download mode and `esptool` can always write.
2. Flash a full image (`-Full`).
3. If the panel stays black but the log is alive, brightness may be at 0:
   `bsp_display_brightness_set(80)` is called at boot, so a black screen with a
   live log means the app did not reach that line — read the log again.

## Going back

Two levels of rollback:

- **Previous application** — flash the old `montre.bin` at `0x200000`. Keep at
  least one previous image locally; `firmware/` only ships the current release.
- **Factory firmware** — the vendor image covers the whole 16 or 32 MB chip and
  is written at `0x0`:

  ```powershell
  python -m esptool --chip esp32s3 -p COM4 -b 921600 write_flash 0x0 factory-backup.bin
  ```

  That image is not redistributed here (it is vendor firmware, several
  megabytes). Dump it **before** the first custom flash:

  ```powershell
  python -m esptool --chip esp32s3 -p COM4 -b 921600 read_flash 0x0 0x2000000 factory-backup.bin
  ```

  and keep its SHA-256 somewhere safe. Restoring it also restores the factory
  `nvs`, so the watch loses its WiFi credentials — expected.

## A note on the `nvs` partition

Everything the user configures on the watch (WiFi SSID and password) lives in
`nvs` at `0x3B000`. This is why the project reproduces the factory partition
layout instead of inventing a simpler one: a layout change would wipe
credentials on every update, and the watch would silently stop connecting.

`partitions.csv` is therefore **not** a free parameter.
