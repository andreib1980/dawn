# DAWN Satellite — ESP32-S3 (DAP2 Tier 2)

Push-to-talk voice satellite for the DAWN assistant. Streams raw audio to the
daemon over WebSocket; the server handles ASR, LLM, and TTS. Audio responses
are played back through an I2S speaker.

## Hardware

| Component | Part | Notes |
|-----------|------|-------|
| MCU | [Adafruit ESP32-S3 TFT Feather](https://www.adafruit.com/product/5483) | Built-in TFT + NeoPixels, PSRAM required |
| Speaker amp | MAX98357 I2S breakout | 3.3V logic, mono |
| Microphone | Analog electret mic module | Connected to ADC pin |
| Button | Momentary push button | Push-to-talk trigger |

### Pin Assignments

| Function | GPIO |
|----------|------|
| Push-to-talk button | 18 |
| I2S BCLK | 5 |
| I2S LRCLK (WS) | 6 |
| I2S DOUT | 9 |
| Microphone ADC | 1 |
| NeoPixel (on-board) | 17 |
| TFT backlight (on-board) | 45 |

## Arduino IDE Setup

### Board

1. **File** > **Preferences** > **Additional Board Manager URLs** — add:
   ```
   https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
   ```
2. **Tools** > **Board** > **Boards Manager** — search **esp32** by Espressif, install
3. Select board: **Adafruit Feather ESP32-S3 TFT**
4. Under **Tools**, set:
   - **PSRAM**: `QSPI PSRAM` — this board is an ESP32-S3R2 (2 MB **QSPI** PSRAM),
     the board's default. Do **not** pick `OPI PSRAM`: OPI mode maps zero PSRAM on
     this chip (`psramFound()` stays true but `getPsramSize()` is 0, so the audio
     buffers fail to allocate at boot).
   - **Flash Size**: `4MB (32Mb)`
   - **Partition Scheme**: `Minimal SPIFFS (1.9MB APP with OTA)` — dual-app layout
     required for over-the-air updates (see **OTA bootstrap** below). The
     repo's `partitions.csv` documents the exact byte layout.
   - **Upload Speed**: `921600`

### OTA bootstrap (one-time)

Server→satellite OTA needs two app partitions, but the stock `Default 4MB with
spiffs` scheme has only one. Switching schemes is a **one-time USB re-flash** —
OTA cannot lay down its own partition table. The dual-OTA layout
(`partitions.csv`) keeps the `nvs` partition at its stock `0x9000/0x5000`
offset, so a device that has already paired **keeps its UUID and reconnect
secret** across this re-flash (no re-registration needed). After this one USB
flash, all subsequent updates arrive over the air. See
`docs/OTA_DESIGN.md` §8.

### Libraries

Install all via **Sketch** > **Include Library** > **Manage Libraries...**:

| Library | Author | Search term |
|---------|--------|-------------|
| WebSockets | Markus Sattler (Links2004) | `WebSockets` |
| ArduinoJson | Benoit Blanchon | `ArduinoJson` |
| Adafruit GFX Library | Adafruit | `Adafruit GFX` |
| Adafruit ST7735 and ST7789 | Adafruit | `Adafruit ST7789` |
| Adafruit NeoPixel | Adafruit | `Adafruit NeoPixel` |

## Configuration

Copy the example secrets header and edit it with your credentials:

```bash
cp arduino_secrets.h.example arduino_secrets.h
```

Then edit `arduino_secrets.h`:

```cpp
#define SECRET_SSID     "YOUR_SSID"
#define SECRET_PASSWORD "YOUR_PASSWORD"

#define SECRET_SERVER_IP   "192.168.1.159"
#define SECRET_SERVER_PORT 3000

#define SECRET_SATELLITE_NAME     "Office Speaker"
#define SECRET_SATELLITE_LOCATION "office"
```

**Note:** `arduino_secrets.h` is gitignored to keep credentials out of version control.

## Over-the-air updates (OTA)

After the one-time USB bootstrap (above), new firmware arrives over the air. The device verifies an
Ed25519-signed manifest, reboots into a minimal WiFi-only path that streams the image into the inactive
app slot, SHA-256-checks it, switches the boot partition, and reboots; if the new image can't re-register
within a few boots, an NVS guard reverts to the old slot. See `docs/OTA_DESIGN.md` §8.

**Two one-time build prerequisites** (both are required to compile — like `arduino_secrets.h`):

1. **Vendor the Ed25519 verify library.** Download the canonical, public-domain **TweetNaCl** two-file
   build — `tweetnacl.c` + `tweetnacl.h` — from the authoritative source **<https://tweetnacl.cr.yp.to/>**
   and drop them in this directory **unmodified**, then commit them (a normal vendored dependency, not a
   secret). The sketch declares the real exported symbol `crypto_sign_ed25519_tweet_open` itself (the name
   `crypto_sign_open` is a header macro, not a link symbol) and supplies the `randombytes()` stub, so do
   **not** edit the files or `#include "tweetnacl.h"` from the sketch.
2. **Provision your verify key.** `cp ota_pubkey.h.example ota_pubkey.h` and paste your operator Ed25519
   **public** key hex (the prebuilt sketch ships unsigned — you bake your own key, so you sign releases for
   your own fleet). `ota_pubkey.h` is gitignored.

**Publishing an update** (on the daemon host):

1. Bump `#define FIRMWARE_VERSION` in `satellite_version.h`. **This is mandatory** — unlike the Tier-1 RPi build there
   is no marker-check gate, so an unbumped version will flash and run but the server will never finalize
   the push as `success` (it commits only when the device re-registers reporting the pushed version).
2. Build the `.bin`, either way:
   - **Arduino IDE:** **Sketch** > **Export Compiled Binary** → `…/build/…/dawn_satellite_arduino.ino.bin`.
   - **Command line:** `./build-esp32.sh` (uses `arduino-cli` with the right FQBN/PSRAM/partition options
     and writes the same `build/<fqbn>/dawn_satellite_arduino.ino.bin`). The repo also compiles this sketch
     in CI as a gate via `build-esp32.sh --stub` (placeholder headers, compile-only) — see `ci.yml`.
3. Sign + stage + push:
   ```bash
   sudo ./dawn_satellite/ota-release.sh --version X.Y.Z --platform esp32 --tier 2 \
        --abi-tag esp32s3 --image <path-to>.ino.bin --push <device-uuid>
   ```
   (`esp32s3` must match the `abi_tag` the device checks; `ota-release.sh` already accepts `--platform
   esp32`.) Restart the daemon if it hasn't scanned the new release dir.

## Upload & Run

1. Connect the ESP32-S3 via USB
2. Select the correct port under **Tools** > **Port**
3. **Sketch** > **Upload**
4. Open **Serial Monitor** at 115200 baud to see status output

## Usage

- **Press and hold** the button to record
- **Release** to send audio to the daemon
- NeoPixels and TFT show current state:
  - Cycling colors = idle/ready
  - Blue = recording
  - Yellow = waiting for response
  - Green = playing TTS response
  - Red = error (clears after 3 seconds)

## How It Works

This is a DAP2 **Tier 2** satellite — it has no local ASR or TTS. The full
pipeline is:

1. ESP32 samples the analog mic at 16 kHz and streams 100 ms PCM chunks over
   WebSocket (binary type `0x01`)
2. On button release, sends end-of-utterance marker (`0x02`)
3. Daemon runs ASR (Whisper), sends text to LLM, synthesizes TTS
4. Daemon streams 48 kHz PCM audio back (binary type `0x11` / `0x12`)
5. ESP32 plays audio through I2S, calling `webSocket.loop()` between writes to
   keep receiving data

See `docs/WEBSOCKET_PROTOCOL.md` in the main repository for the full protocol spec.

## Memory

All large buffers are allocated in PSRAM:

| Buffer | Size | Purpose |
|--------|------|---------|
| Audio recording | 480 KB | 15 sec at 16 kHz, 16-bit |
| TTS ring buffer | 1.0 MB | ~23.8 sec at 22050 Hz, 16-bit (power-of-two for fast modulo) |
| WS send buffer | 3.2 KB | One 100 ms audio chunk |
