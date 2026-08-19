# MicScribe

Push-to-talk speech-to-text that runs entirely on an ESP32-S3. Hold a button,
talk into an INMP441, let go — the board records the audio, uploads it to the
ElevenLabs Scribe API itself, and prints the transcript out of the serial port.
No host-side helper is involved in capturing or transcribing; the optional
Python bridge only decides what to do with the text once it arrives.

```
  hold button ──► I2S capture ──► HPF + gain + normalise ──► WAV in PSRAM
                                                                 │
  serial JSON ◄── parse response ◄── HTTPS multipart POST ◄───────┘
                                     api.elevenlabs.io/v1/speech-to-text
```

## Hardware

| INMP441 | ESP32-S3 | Notes |
|---|---|---|
| VDD | 3V3 | the INMP441 is **not** 5 V tolerant |
| GND | GND | |
| L/R | GND | left channel; tie to 3V3 and set `MIC_SLOT` to `I2S_STD_SLOT_RIGHT` |
| WS  | GPIO 5 | word select / LRCL |
| SCK | GPIO 4 | bit clock |
| SD  | GPIO 6 | mic data out → ESP32 data in |

The push-to-talk button defaults to **GPIO 0**, which is the BOOT button on
most S3 dev boards — so you can try this with no button wired at all. For an
external button, wire it between GPIO 0 (or whatever you set `PIN_BUTTON` to)
and GND; the internal pull-up handles the rest.

Pins live at the top of `MicScribe/MicScribe.ino` if you want to move them.

## Setup

1. **Config**

   ```bash
   cp MicScribe/config.example.h MicScribe/config.h
   ```

   Fill in your WiFi credentials and an ElevenLabs API key from
   <https://elevenlabs.io/app/settings/api-keys>. `config.h` is gitignored.

2. **Flash**

   ```bash
   ./flash.sh                          # auto-detects the port
   ./flash.sh /dev/cu.usbmodem101      # or name it
   ```

   The script uses `esp32:esp32:esp32s3` with PSRAM enabled. If you're using
   the Arduino IDE instead, pick *ESP32S3 Dev Module* and set **PSRAM** to
   *QSPI PSRAM* (or *OPI PSRAM* for WROOM-2 / N16R8 modules).

3. **Talk**

   ```bash
   arduino-cli monitor -p /dev/cu.usbmodem101 -c baudrate=115200
   ```

   Hold the button, say something, release. The transcript appears within a
   second or two.

## Serial protocol

Two kinds of line come out of the board, so a bridge never has to guess:

- Human-readable logs are prefixed with `# `.
- Everything else is one JSON object per line:

```json
{"type":"state","state":"recording"}
{"type":"state","state":"transcribing"}
{"type":"transcript","text":"testing one two three","language":"eng"}
{"type":"error","error":"http 401: {\"detail\":...}"}
```

## Bridge

```bash
pip install pyserial
./bridge/bridge.py --copy      # copy each transcript to the clipboard
./bridge/bridge.py --type      # type it into whatever app has focus (macOS)
./bridge/bridge.py -v          # also show the device's log lines
```

`--type` drives System Events via `osascript`, so your terminal needs
Accessibility permission under System Settings → Privacy & Security.

## Tuning

Constants at the top of the sketch:

| Constant | Default | What it does |
|---|---|---|
| `SAMPLE_RATE` | 16000 | 16 kHz mono is ample for speech and keeps uploads small |
| `MIC_GAIN` | 10.0 | fixed pre-gain applied to the 24-bit mic samples |
| `MAX_SECONDS_PSRAM` | 20 | recording cap with PSRAM (640 KB buffer) |
| `MAX_SECONDS_NO_PSRAM` | 6 | fallback cap when the buffer has to live in SRAM |
| `MIN_RECORD_MS` | 300 | anything shorter is treated as an accidental tap |
| `MIC_SLOT` | `I2S_STD_SLOT_LEFT` | match this to how you wired the mic's L/R pin |

The INMP441 is quiet at conversational distance, so the signal path is:
24-bit sample → one-pole high-pass at ~13 Hz (kills the mic's DC offset) →
`MIC_GAIN` → clip to 16-bit. After the button is released the whole take is
normalised to about −2 dBFS (up to 8× extra), which keeps quiet takes usable
without clipping loud ones. Each take prints its peak level, so if you see
`WARNING: signal is nearly silent` the mic wiring or `MIC_SLOT` is wrong.

## Security note

By default the TLS connection uses `setInsecure()` — traffic is encrypted, but
the server's certificate isn't verified. To pin the real root CA, uncomment
`ELEVENLABS_ROOT_CA` in `config.h` and paste in the root certificate:

```bash
openssl s_client -showcerts -connect api.elevenlabs.io:443 </dev/null
```

Use the **last** certificate in the chain. Note that pinning means the firmware
needs reflashing whenever that root rotates.

## Layout

```
MicScribe/
  MicScribe.ino        firmware: I2S capture, WAV assembly, HTTPS upload
  config.example.h     template for credentials
  config.h             your credentials (gitignored)
bridge/
  bridge.py            optional host-side listener: print / clipboard / type
flash.sh               compile + upload helper
```
