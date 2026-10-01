# ESP32 Earbud

A Bluetooth Classic earbud built on the ESP32 using ESP-IDF. It pairs with a phone as a headset and supports music streaming, media controls, and hands-free calling, with status shown on a small OLED.

## Features

- **A2DP sink**: streams stereo music from the phone to a MAX98357A I2S amplifier (44.1 kHz, 16-bit).
- **AVRCP controller**: play/pause/next/previous, track title and artist metadata, play-status and track-change notifications.
- **HFP client (hands-free)**: incoming/outgoing call state, caller number (CLIP), answer/reject/hang up, SCO audio over the HCI data path.
- **Call audio**: phone audio plays at 8 kHz; the INMP441 microphone is captured at 16 kHz, downsampled to 8 kHz and sent back in 120-byte frames.
- **Automatic mode switching**: speaker switches between media (44.1 kHz) and call (8 kHz) when SCO audio connects or disconnects.
- **OLED UI**: SSD1306 128x64 screens for music, incoming call, outgoing call and active call.
- Advertises itself as an audio headset (Class of Device) so phones route calls to it.

## Hardware

| Part | Purpose | Interface |
|------|---------|-----------|
| ESP32 (classic Bluetooth capable) | MCU | n/a |
| MAX98357A | I2S speaker amplifier | I2S0 |
| INMP441 | I2S MEMS microphone (L/R tied to GND) | I2S1 |
| SSD1306 OLED, 128x64 | Status display | I2C, address `0x3C` |

### Pinout

| Signal | GPIO |
|--------|------|
| Speaker BCLK | 27 |
| Speaker WS (LRCLK) | 14 |
| Speaker DOUT | 25 |
| Mic SCK | 26 |
| Mic WS | 33 |
| Mic SD | 32 |
| OLED SDA | 21 |
| OLED SCL | 22 |

Pins are defined in `main/app.h`.

## Architecture

```
            +-----------+   events    +-----------+
 Phone <--> |   bt.c    | ----------> |  main.c   | ---> ui.c ---> OLED
            | A2DP/AVRC |  app_post() | app_task  |
            |   /HFP    | <---------- | state     |
            +-----+-----+  commands   +-----+-----+
                  |                         |
          PCM / SCO data              audio_set_mode()
                  v                         v
            +---------------------------------+
            |            audio.c              |
            | ring buffers, I2S speaker + mic |
            +---------------------------------+
```

| File | Role |
|------|------|
| `main.c` | `app_main`, event queue, application state machine (call state, playback, metadata) |
| `bt.c` / `bt.h` | Bluedroid setup, GAP, A2DP sink, AVRCP controller, HFP client |
| `audio.c` / `audio.h` | I2S speaker and microphone, ring buffers, media/call mode switching, SCO in/out |
| `ui.c` / `ui.h` | SSD1306 driver, 5x7 font, screen rendering |
| `app.h` | Pin configuration, event types, shared state structs |

Bluetooth callbacks run on the Bluetooth task, so they only post small events to a queue; all state handling and UI rendering happen in `app_task`.

## Build and flash

Requires [ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/get-started/) v5.x (uses the new `driver/i2s_std.h` and `driver/i2c_master.h` APIs).

```bash
idf.py set-target esp32
idf.py menuconfig      # see required settings below
idf.py build
idf.py -p <PORT> flash monitor
```

### Required menuconfig settings

- Component config -> Bluetooth -> **Enable**, Host: **Bluedroid**
- Bluetooth Controller mode: **BR/EDR Only** (code releases BLE memory when this is set)
- Bluedroid -> enable **Classic Bluetooth**, **A2DP**, **A2DP Sink**, **AVRCP Controller**, **Hands-free Profile -> Client role**
- Hands-free: audio data path = **HCI**
- Hands-free: enable **wide band speech (mSBC)** only if you add mSBC handling (current audio path is 8 kHz CVSD)

> Adjust the exact option names to your IDF version. If the build fails on missing `esp_hf_client_*` symbols, the HFP client is not enabled.

## Usage

1. Power the board. The OLED shows `DISCONNECTED / WAIT PHONE`.
2. On your phone, pair with **ESP32_EARBUD** (SSP, or PIN `0000` for legacy pairing).
3. Play music: the OLED shows play state, title and artist.
4. Place or receive a call: the display switches to the call screen and audio moves to the call path.

## Known limitations

- Call audio is 8 kHz narrowband (CVSD) only.
- OLED font covers uppercase letters, digits and `- . : / ?`; other characters render blank. Metadata is truncated to 31 characters.
- No on-device buttons yet: answer/reject/media control functions (`bt_hf_answer`, `bt_hf_hangup`, `bt_media`) exist but are not wired to any input.
- Pairing uses auto-confirm for SSP, which is fine for development but not secure.

## Project structure

```
.
├── CMakeLists.txt
├── main.c
├── app.h
├── audio.c / audio.h
├── bt.c / bt.h
└── ui.c / ui.h
```

If this is the `main/` component of an ESP-IDF project, add a top-level `CMakeLists.txt` with `include($ENV{IDF_PATH}/tools/cmake/project.cmake)` and `project(esp32_earbud)`.

## License

Add a license of your choice (for example MIT) before publishing.
