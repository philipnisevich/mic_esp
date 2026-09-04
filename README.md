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

   Fill in an ElevenLabs API key from
   <https://elevenlabs.io/app/settings/api-keys>. `config.h` is gitignored.

   WiFi credentials are optional here — leave `WIFI_SSID` as the placeholder
   and provision the network after flashing instead (see
   [WiFi setup](#wifi-setup) below), or fill them in if you'd rather bake in
   a default.

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

## WiFi setup

`web/wifi-setup.html` is a self-contained page (no server, no build step)
that talks to the board over [Web Serial](https://developer.chrome.com/docs/capabilities/serial),
so you aren't locked into whatever network was configured at flash time.
Open it directly in Chrome or Edge — Safari and Firefox don't implement Web
Serial:

```bash
open web/wifi-setup.html
```

1. **Connect over USB** — picks the board's serial port. Only one thing can
   hold the port at a time, so close `arduino-cli monitor`/`bridge`/the
   Arduino IDE first.
2. **Scan for networks** — lists what the board can see, strongest first,
   with a lock icon on secured ones. Click one to fill in the SSID, or type
   a hidden network's name by hand.
3. Enter the password and **Send to board**.

The board saves the credentials to NVS on a successful connection and uses
them on every boot after that, ahead of the `WIFI_SSID`/`WIFI_PASS` compile-time
defaults in `config.h` — so re-provisioning in the field never reverts to
whatever was baked in at flash time. A failed attempt (wrong password) is
not saved, so it can't strand the board on bad credentials.

This rides the same serial connection as everything else, using the same
line-oriented JSON convention (see [Serial protocol](#serial-protocol)):

```json
{"cmd":"wifi_scan"}
{"cmd":"wifi_connect","ssid":"...","pass":"..."}
{"cmd":"wifi_status"}
```

```json
{"type":"wifi_scan_result","networks":[{"ssid":"Home","rssi":-45,"secure":true}]}
{"type":"wifi_status","status":"connecting","ssid":"Home"}
{"type":"wifi_status","status":"connected","ssid":"Home","ip":"192.168.1.44"}
{"type":"wifi_status","status":"failed","ssid":"Home"}
```

## Wake word

Say **"Hey Nova"** (or "Hi Nova" / "Okay Nova") and the board starts a take,
ending it when you stop talking. The button still works as push-to-talk.

Detection is MultiNet (`mn5q8_en`) running continuously in command mode, not
WakeNet. WakeNet wake words are trained models and the only one bundled with
the Arduino core is "Hi, ESP"; custom words are a paid Espressif service.
MultiNet takes phrases as runtime phoneme strings, so a custom word costs
nothing but a table entry. The trade-off is more false accepts than a
purpose-trained wake model.

Phonemes come from the core's own G2P tool, not guesswork:

```bash
pip install g2p_en
python3 ~/Library/Arduino15/packages/esp32/hardware/esp32/*/libraries/ESP_SR/tools/gen_sr_commands.py "Hey Nova,Hi Nova,Okay Nova"
```

To change the word, regenerate and replace `SR_COMMANDS` in the sketch. Avoid
single short syllables — they false-accept constantly, which is why bare
"Nova" is left out.

### The channel-count trap

`input_format` must describe **three** channels, e.g. `"MNN"` for one mic.
`esp32-hal-sr.c` hardcodes `SR_CHANNEL_NUM = 3` and always expands the fill
callback's output into 3 interleaved channels before calling the AFE's
`feed()`. Passing `"M"` makes the AFE read that 3-channel stream as mono, and
**nothing is ever detected** — no error, no warning, just silence. The core's
own example has the same mismatch (`"MM"` with 3 channels fed).

### How the audio path works

ESP-SR owns the I2S reads: it pulls samples through `srFill()` rather than
reading the bus itself. That gives one reader teeing the same audio into both
the detector and the capture buffer, and makes the fill callback the only
writer of the buffers — which is what keeps the state machine race-free across
SR's feed task, its detect task, and `loop()`.

| Constant | Default | What it does |
|---|---|---|
| `PREROLL_MS` | 300 | replayed at the start of a take so the first word is not clipped; if the tail of "Nova" lands in transcripts, lower it |
| `VAD_SILENCE_MS` | 800 | quiet this long ends a take |
| `VAD_MIN_SPEECH_MS` | 400 | ...but never before this much audio |
| `VAD_NOISE_MULT` | 3.0 | speech = this much above the tracked noise floor |

## Asking a model

After transcription the text goes to an OpenAI chat model and the reply is
shown on the OLED and emitted on serial as `{"type":"answer","text":"..."}`.
Set `OPENAI_API_KEY` and `OPENAI_MODEL` in `config.h`.

The request sends only `model` and `messages`. Optional fields like the token
cap are deliberately omitted: the correct name differs by model family
(`max_tokens` on older chat models, `max_completion_tokens` on newer ones) and
sending the wrong one is a 400, not a graceful degrade. Reply length is steered
by `OPENAI_SYSTEM_PROMPT` instead, which asks for at most two plain-text
sentences so answers fit the screen. `OPENAI_MAX_COMPLETION_TOKENS` is there,
commented out, if you want a hard cap.

Note this is a plain chat model with no tools or live data, so "what is the
weather" gets a polite refusal rather than a forecast.

## OLED display

A 0.96" SSD1306 over I2C. Four wires:

| OLED pin | ESP32-S3 | Note |
|---|---|---|
| VCC | 3V3 | most 0.96" modules are 3.3 V parts |
| GND | GND | |
| SCL | GPIO 9 | I2C clock (`PIN_I2C_SCL`) |
| SDA | GPIO 8 | I2C data (`PIN_I2C_SDA`) |

These are the S3's default `Wire` pins and are clear of the I2S pins. No reset
or chip-select line is needed and the module carries its own pull-ups. The
firmware probes `0x3C` then `0x3D` and logs which it found, or
`no OLED found at 0x3C/0x3D` if neither answers. Everything else still works
without a display attached.

The screen shows an inverted header bar with the current state
(`Ready`/`Listening`/`Transcribing`/`You said`/`Nova`) over word-wrapped body
text. Replies longer than one screen paginate every 3.5 s with a `1/3` counter,
and the last answer stays up until the next take. Set `OLED_HEIGHT` to 32 in
`config.h` for the half-height modules.

## Amplifier (MAX98357A)

| MAX98357A | ESP32-S3 | Note |
|---|---|---|
| VIN | **3V3** | see below |
| GND | GND | shared with the ESP32 |
| DIN | GPIO 12 | `PIN_AMP_DIN` |
| BCLK | GPIO 10 | `PIN_AMP_BCLK` |
| LRC | GPIO 11 | `PIN_AMP_LRC` |
| GAIN | floating | 9 dB; tie to GND for 12 dB if too quiet |
| SD | VIN | forces enable and selects the left channel |

Speaker across the two output pads. They are **bridge-tied** - both are driven,
neither is ground, so never connect one side to GND.

Three things here cost hours to find, all of which fail as pure silence with no
error anywhere:

- **Power.** The dev board's 5V pin was dead; VIN on 3V3 works. Nothing in
  firmware can tell a disconnected amplifier from a working one, so measure VIN
  to GND before debugging anything else.
- **Sample rate.** The datasheet allows LRCLK of 8/16/32/44.1/48/88.2/96 kHz and
  explicitly excludes 11.025/12/22.05/**24** kHz. OpenAI's `pcm` format is 24 kHz,
  so the speaker path runs at 48 kHz with each sample written twice.
- **I2S controller.** `I2SClass` defaults to `I2S_NUM_AUTO`. With the microphone
  already holding a channel, the amplifier can land on the same controller and
  share its clock. The mic is pinned to `I2S_NUM_0` and the amplifier to
  `I2S_NUM_1`, and both ports are logged at boot.

`AMP_PIN_SWEEP 1` in the sketch plays a distinct pitch through all six
permutations of the three amplifier pins at boot, which identifies a miswire in
one boot rather than six flash cycles.

## Serial protocol

Two kinds of line come out of the board, so a bridge never has to guess:

- Human-readable logs are prefixed with `# `.
- Everything else is one JSON object per line:

```json
{"type":"state","state":"recording"}
{"type":"state","state":"transcribing"}
{"type":"transcript","text":"testing one two three","language":"eng"}
{"type":"answer","text":"Paris is the capital of France."}
{"type":"noise","reason":"below speech threshold"}
{"type":"error","error":"http 401: {\"detail\":...}"}
```

The board also accepts JSON commands in the other direction, one per line,
used by the [WiFi setup](#wifi-setup) page — `{"cmd":"wifi_scan"}`,
`{"cmd":"wifi_connect","ssid":"...","pass":"..."}`, `{"cmd":"wifi_status"}`.

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
web/
  wifi-setup.html     Web Serial page for provisioning WiFi post-flash
flash.sh               compile + upload helper
```
