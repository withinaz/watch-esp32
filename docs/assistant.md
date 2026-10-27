# The assistant app

The second app of the launcher speaks the **XiaoZhi** protocol to the same
server family as the factory firmware: press, talk, the watch answers.

## Why not the official XiaoZhi firmware

`78/xiaozhi-esp32` (v2.5.3) requires **ESP-IDF ≥ 6.0.1**, LVGL 9.5 and `esp-sr`;
it does not compile on 5.5.1. It is also a **whole firmware**, not an
application: flashing it would replace the launcher. So the protocol was
re-implemented on the device side (`main/xiaozhi_client.cpp`), reusing the BSP
codecs.

The server does STT → LLM → TTS. **The LLM key is configured in the server
console, never on the watch.**

## First run

1. Open the app: a 6-digit code appears, with `xiaozhi.me`.
2. On <https://xiaozhi.me>, create an account and add the device with that code.
3. In the console, pick the language model.
4. The watch reconnects by itself within seconds and the state becomes `Pret`.

The pairing code changes on every attempt; the one on screen is the valid one.
The OTA call is retried roughly every 5 s while the device is unpaired.

## Protocol, as implemented

```
POST https://api.tenclass.net/xiaozhi/ota/
     header: Activation-Version: 1
     body:   device / client identifiers
     reply:  activation.code      (unpaired: show the code)
          or websocket {url, token}   (paired)

WS   <url>?token=...            text frames = JSON, binary frames = Opus
     <- {"type":"hello","session_id":...,"audio_params":{"sample_rate":24000,"frame_duration":60}}
     -> {"session_id":...,"type":"listen","state":"start","mode":"manual"}
     -> <binary> Opus, 60 ms, 24 kHz mono
     -> {"session_id":...,"type":"listen","state":"stop"}
     <- {"type":"stt", ...}   recognised text
     <- {"type":"tts","state":"start"|"stop"|...}  and binary Opus replies
```

Nothing else is needed: no MCP, no wake word, no audio codec negotiation beyond
`hello`. That is what makes a ~950-line client sufficient.

## States

| State | Trigger | UI |
|---|---|---|
| `XZ_PAIRING` | no websocket credentials yet | code + `xiaozhi.me`, button `VERIFIER` |
| `XZ_CONNECTING` | OTA or WebSocket in progress | button `PARLER` |
| `XZ_READY` | `hello` received | button `PARLER` |
| `XZ_LISTENING` | `startListening()` | button `ENVOYER`, red level meter |
| `XZ_SPEAKING` | `tts` start / audio arriving | button `COUPER`, green level meter |
| `XZ_THINKING` | `listen stop` sent, waiting for `stt` | orange sweep |
| `XZ_OFF` | server unreachable, WiFi down, or app reopened | grey, auto-retry every 8 s |

The retry loop matters: before v3.1, `Hors ligne` was a dead end requiring a
reboot. Now `_off_since` drives a retry from the app itself.

## Audio chain

| Element | Value |
|---|---|
| Rate | **24 000 Hz** in and out (the codec's native rate) |
| Opus frame | **60 ms** = 1440 samples, ~148–160 bytes |
| Mic codec | ES7210, **2 channels** (mic + reference), channel 0 kept |
| Mic gain | **37.5 dB** hardware (ES7210 maximum) **+ ×2 software** (`XZ_MIC_SW_GAIN`) |
| Output codec | ES8311 + NS4150B amplifier, volume **90 %** |
| Opus bitrate | 24 kbit/s, complexity 0 (real time) |

## Level meter

26 bars in a 62 px strip, driven by the **real** signal, not simulated:

| State | Source | Colour |
|---|---|---|
| `J'ecoute` | `XzClient::micLevel()`, peak of mic frames | red `0xFF453A` |
| `Je reponds` | `XzClient::spkLevel()`, peak of decoded frames | green `0x00E5A0` |
| `Je reflechis` | sweeping comet (decorative) | orange |
| `Pret` | slow breathing (decorative) | dark green |
| other | flat bars | grey |

Both levels are `std::atomic<int>` (0..100) written by the **existing** audio
tasks (`mic_task`, `spk_task`): PCM is untouched, no extra task, buffer or
allocation. `peak_to_level()` applies a square root so end-of-sentence tails
stay visible.

Rendering: a dedicated **40 ms** `lv_timer` (`on_anim`), independent of the
120 ms text timer. Bars are absolutely positioned, so changing one height
invalidates only its own rectangle. Fast attack (0.55), slow release (0.20).

## Testing without the official server

`tools/mock_xiaozhi.py` is a parrot server: it records the frames it receives
while listening and sends them back as the reply, so the watch hears itself.

```powershell
# on the PC, same network as the watch, port opened in the firewall
python tools/mock_xiaozhi.py 8765
```

To point the watch at it, create `main/xz_test.h`:

```cpp
#pragma once
#define XZ_TEST_WS "ws://192.168.1.10:8765/xiaozhi/v1/"
#define XZ_TEST_AUTO_LISTEN 1
```

rebuild and flash: the file bypasses OTA, connects directly and starts a 4 s
listen at boot. **Delete `main/xz_test.h` afterwards** — without it, everything
returns to normal behaviour.

Success looks like:

```
XIAOZHI: audio pret : 24000 Hz, trame 60 ms, micro 2 canal(aux)
XIAOZHI: websocket connecte
XIAOZHI: decodeur opus pret a 24000 Hz
XIAOZHI: micro ouvert
XIAOZHI: premiere trame micro : crete 116 / 32767
XIAOZHI: trame 1 encodee : 148 o
```

and on the server side, `listening finished, 66 frames received` for a 4 s
capture.

## Failure modes already paid for

1. **`opus_encode` stack.** `78/esp-opus` is built with `USE_ALLOCA` and uses
   more than 16 KB of stack per call. Symptom:
   `vApplicationStackOverflowHook`, then `Guru Meditation ... |<-CORRUPTED`.
   Fix: the two audio tasks have **static stacks of 64 KB / 48 KB allocated in
   PSRAM** (`CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY=y`). A 32 KB stack in
   internal RAM cannot even be allocated.
2. **`esp_http_client_perform()` swallows the response body.** Without an
   `HTTP_EVENT_ON_DATA` handler, `esp_http_client_read()` returns 0. The OTA
   body is captured in `http_evt()`.
3. **Do not create the audio tasks on every press.** They are permanent and
   driven by `_mic_run` / `_spk_run`; two fast presses would otherwise reuse the
   same static stack.
4. **No `goto` after a declaration in C++.** The microphone task's error path
   returns directly instead.
5. **The codec can still be held by the other audio app.** `esp_codec_dev_open`
   is retried once after 300 ms; without that, switching from Music to Assistant
   could leave the speaker closed.
6. **A failed `tts` never ends.** The state machine has a timeout back to
   `XZ_READY`, otherwise the button stays on `COUPER`.

## What is deliberately absent

- **A back button.** Touch does not work inside this app, so it would be dead
  UI. You leave by restarting the watch — [known limitation](../README.md#known-limitations).
- **Wake-word detection.** It needs the ESP-SR models (960 KB `model`
  partition). You talk by touching the screen.
- **Local STT or TTS.** Everything is server-side.
