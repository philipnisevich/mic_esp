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

   In the Arduino IDE, pick *ESP32S3 Dev Module* and match these — all four
   matter, and two of them fail silently if wrong:

   | Setting | Value | Why |
   |---|---|---|
   | USB CDC On Boot | **Enabled** | otherwise `Serial` goes to UART0 on GPIO 43/44 and nothing reaches the host over USB |
   | PSRAM | **OPI PSRAM** | the N16R8 module has *octal* PSRAM; QSPI mode yields 0 MB with no error |
   | Flash Size | **16MB** | |
   | Partition Scheme | 16M Flash (3MB APP/9.9MB FATFS) | |

   Getting PSRAM wrong is not just a smaller buffer: the audio buffer falls
   back to internal RAM, which starves mbedtls and every upload dies with
   `SSL - Memory allocation failed` long before it reaches ElevenLabs.

   Check your module with `esptool --chip esp32s3 flash_id`, which reports the
   PSRAM size and mode.

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
python3 -m venv .venv && ./.venv/bin/pip install pyserial
```

A venv is needed because Homebrew's Python is marked `EXTERNALLY-MANAGED` and
refuses a bare `pip install`.

`bridge/bridge.sh` defines a `bridge` command. Source it from your shell rc —
it is POSIX enough to work in both bash and zsh, so add it to whichever you
actually use (or both):

```bash
LINE='[ -f "$HOME/mic_esp/bridge/bridge.sh" ] && . "$HOME/mic_esp/bridge/bridge.sh"'
echo "$LINE" >> ~/.zshrc     # zsh is the macOS default
echo "$LINE" >> ~/.bashrc
```

Check what you are running with `echo $0` — the login shell recorded in
`dscl` is not always the one your terminal actually opens.

```bash
bridge run              # start it; auto-detects the port
bridge run --type       # type transcripts into the focused app (macOS)
bridge run --copy       # copy them to the clipboard
bridge run -v           # also show the device log lines
bridge --type           # flags with no subcommand imply `run`
bridge stop             # release the serial port
bridge status           # show what currently holds the port
bridge --help           # full flag list
```

It finds the checkout by walking up from `$PWD`, so it works from the repo
root, from `bridge/`, or from anywhere if the checkout is at `~/mic_esp`.

The board accepts only one connection at a time, so `bridge run` stops any
existing instance first rather than failing with a busy error. If something
else has the port (`arduino-cli monitor`, the Arduino IDE), `bridge status`
will name it.

Or call the script directly:

```bash
./.venv/bin/python bridge/bridge.py --type --verbose
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

If a take reports `peak 32768` the input is clipping at full scale — drop
`MIC_GAIN` until peaks land around 20000-28000. Scribe tolerates some clipping,
but sustained clipping costs accuracy on quiet or fast speech.

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
  bridge.sh            `bridge run|stop|status` shell command
flash.sh               compile + upload helper
```
