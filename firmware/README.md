# Prebuilt firmware

Every release of this project ships a **verified, ready-to-flash** image here.
Nothing is rebuilt at flash time, so what lands on the board is exactly what
was tested on the bench.

## v4.0 — current

| | |
|---|---|
| Application | `v4.0/montre.bin` — **3 589 296 bytes** |
| SHA-256 | `51ca2508dba666b339d33a8d35b85d54eb90d478980369837379327daac34d43` |
| Write offset | **`0x200000`** |
| Bootloader | `v4.0/bootloader.bin` — `0x0` |
| Partition table | `v4.0/partition-table.bin` — `0x8000` |
| Apps | Paramètres, Assistant, Musique, Perplexe |

`SHA256SUMS` lists every image; `flash.ps1` checks all of them **before**
touching the port and refuses to flash on a mismatch.

## Flash

```powershell
.\flash.ps1                 # application only, port auto-detected
.\flash.ps1 -Port COM7
.\flash.ps1 -Full           # bootloader + partition table + application
.\flash.ps1 -DryRun         # verify the files, write nothing
```

The application-only write leaves the `nvs` partition at `0x3B000` alone, so
the WiFi credentials already stored on the watch survive the update. A full
write keeps `nvs` too — only the boot chain and the app are replaced.

With esptool directly:

```powershell
python -m esptool --chip esp32s3 -p COM4 -b 921600 --before default_reset `
  --after hard_reset write_flash 0x200000 v4.0\montre.bin
```

## Expected boot log

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

No `Guru Meditation`, no `abort()`, and no `Clock out of area, hide it`.
`create a new page` appears once per app beyond the first — with four apps it
shows up three times, and that line is the proof that each app owns its own page.

## Older versions

The development history (what each version changed and why) is in
[`docs/versions.md`](../docs/versions.md). Only the current release is shipped
as a binary; older images stay in the author's local archive and are rebuilt
from the corresponding sources instead of being redistributed here.
