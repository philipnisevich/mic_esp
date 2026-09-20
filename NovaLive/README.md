# NovaLive

Nova on the ESP32-S3: the same assistant as the Nova web app, on the MicScribe
hardware. GPT-Live-1 runs on the device over WebRTC. All the thinking stays in
Nova's backend: Claude, web search, and the Gmail/Calendar/etc. connectors
through Composio.

```
 "Hey Nova" (MultiNet, on-chip, free)
      │
      ▼
 ESP32 ──POST /api/live/session {sdp}──► Nova backend ──► OpenAI: create GPT-Live-1
   │                                       │               session (client delegation)
   │◄───────────── {sdp answer} ───────────┘
   │
   ├── WebRTC: Opus mic audio ──────────────► GPT-Live-1 ─delegation─► liveDelegate.ts
   │◄───────── Opus speech ◄────────────────   (listens/speaks)          Claude + Composio
   └── "oai-events" data channel: transcripts, delegation, session.closed
```

The backend doesn't know or care whether the WebRTC offer came from a browser
or from this board. It's the same `/api/live/session` route, the same
`liveDelegate.ts`, and the same Claude + Composio code. The OpenAI key never
leaves the server.

It works like the browser (`useNovaConversation.ts`):

| | Browser | NovaLive |
|---|---|---|
| Wake word | Porcupine "Nova" | MultiNet7 "hey nova" / "hi nova" / "okay nova" |
| Tap the orb | start / hang up an idle session | BOOT button (GPIO 0), same behavior |
| Voice | GPT-Live-1 over WebRTC | GPT-Live-1 over WebRTC (Espressif `esp_peer`, Opus) |
| Brain | backend: Claude + Composio | backend: identical |
| Hang-up | 1.8 s pause → thinking, +20 s → idle, idle ≥ 8 s → `session.close` | 5 s reply window after Nova's *answer* (see below), then `session.close` |
| Background tasks | polls `/api/tasks/status`; orb light | same poll; onboard RGB LED and the `BG` / `OK` header |
| Status light | orb colours | onboard RGB LED: pulsing yellow while a background task runs, green when one finishes, blue in a conversation |
| Barge-in | browser echo cancellation | esp-sr AEC with a software speaker reference |

There's also a short blip when the wake word is heard, and a two-note chime once
the session is live (connecting takes about 2 s). Talk after the chime.

## Reply window and the LED

After Nova finishes an answer you have **5 seconds** to say something (`NOVA_FOLLOWUP_MS`). If you do,
the conversation continues; if you don't, the session hangs up and the billing stops. The very first
window, right after the connect chime, is 8 s (`NOVA_FIRST_WINDOW_MS`) so you can react to the chime.

The window opens only when Nova is really done: GPT-Live sends `session.delegation.created` when your
request goes to the backend and `session.commentary.appended` when the backend's answer arrives, so the
board knows an answer is still owed after her quick "Checking..." and waits (up to 45 s) instead of
hanging up on a slow Claude/Composio job. If you speak and nothing is delegated or said back for 8 s,
it hangs up ("Didn't catch that.").

The RGB LED is a single WS2812, driven on GPIO 48 and GPIO 38 (v1.0 and v1.1 of the DevKitC-1 put it on
different pins; a pin with no LED is harmless):

| Colour | Meaning |
|---|---|
| pulsing yellow | a background task is running (Claude working on a big job) |
| green | a background task finished and Nova hasn't mentioned it yet |
| blue | in a conversation, from the wake word until hang-up |

Highest priority first: a task colour shows even mid-conversation, so you can watch a job start and
finish while you keep talking. Blue is what you see when no task needs attention. The status is polled
every 2.5 s, so a task shorter than that can be missed. At power-on
the LED does a red-green-blue self-test. `{"cmd":"led_test"}` plays blue, pulsing yellow, green.
`NOVA_LED_BRIGHTNESS` (config.h) sets the peak level.

If the round TFT (below) is attached, it carries the same idea further: a minimalist clock face at
rest, and from the wake word through hang-up a rotating gradient ring - styled after the web app's
`NovaOrb.tsx` - whose speed and colour change with state (connecting, live-idle, recording, thinking,
speaking). The background-task colours overlay it the same way they overlay the LED. The LED itself is
unchanged and keeps working whether or not the display is attached.

## Setup

Same wiring as MicScribe (see `../README.md`): INMP441 on GPIO 4/5/6,
MAX98357A on 10/11/12. Round 1.28" TFT (GC9A01, 240x240, 4-wire SPI, optional)
on SCK 13 / SDA 14 / CS 15 / DC 16 / RST 17 (no backlight pin on this board -
it's tied on-board, always on).

1. **Backend** (`Nova/backend/.env`): set a device token and choose whose
   connectors the device uses:

   ```
   NOVA_DEVICE_TOKEN=<openssl rand -hex 24>
   NOVA_DEVICE_USER_ID=demo-user
   ```

   `demo-user` is who the web app acts as while login is off, so the device
   sees the same Gmail/Calendar you connected there. Start the backend. It
   prints the address the device should use:
   `On your network (for the ESP32 device) → http://10.0.0.103:8787`.

   Port 8787 must be free. If another program holds it, set `PORT=` in `.env`
   and use that port in the URL below.

2. **Device config**:

   ```bash
   cp NovaLive/main/config.example.h NovaLive/main/config.h
   ```

   Fill in `NOVA_SERVER_URL` (the address from step 1), `NOVA_DEVICE_TOKEN`
   (the same value as the backend), and optionally WiFi, timezone, and voice.

3. **Flash** (ESP-IDF 5.4.4 or newer; `flash.sh` looks for `~/esp/esp-idf-v5.5`):

   ```bash
   ./NovaLive/flash.sh                      # auto-detects the port
   ```

   This flashes the app and the wake word model partition.

4. **WiFi**: `open web/wifi-setup.html` in Chrome, the same page and protocol
   as MicScribe.

5. **Watch it**: `bridge run -v` shows state changes, what you said, what Nova
   said, and background tasks finishing.

If the laptop's IP changes, update the server address over serial without
reflashing:

```json
{"cmd":"set_server","url":"http://10.0.0.57:8787"}
```

Other serial commands: `{"cmd":"server"}` prints the current address, `{"cmd":"talk"}` works like
pressing the button, and `{"cmd":"info"}` prints the last reset reason, uptime and free memory.

## Echo cancellation

The MAX98357A has no loopback, so the echo canceller's reference is taken in
software: every sample queued to the speaker also goes into a ring that the
mic task reads alongside the microphone (`audio.c`). Two knobs, both at the top
of `audio.c`:

- `AEC_REF_DELAY_MS` (20): holds the reference back so it stays just ahead of
  the echo the mic actually hears. Raise it if Nova's voice leaks back to
  GPT-Live; lower it if AEC seems to do nothing.
- `NOVA_ECHO_CANCEL` in `config.h`: set it to 0 for half duplex (the mic is
  muted while Nova talks). This is the fallback if Nova keeps interrupting
  herself. You lose the ability to talk over her.

## Verified vs. not

Run on the board against the real backend and GPT-Live-1, by voice: weather, email summaries, Canvas to
Google Calendar to Notion chains, background jobs (start, status, cancel), clarifying questions.

Not yet verified on the board:

- The reply window (5 s) and the LED colours. They are implemented and the LED self-test plays at boot,
  but the timing and colours have not been watched in a real conversation. The event order they rely on
  (delegation, then commentary, then speech) was checked against GPT-Live-1 from the Mac.
- Power: the board has hard-reset with no panic text a few times, twice within a second of starting a
  session, and once again during WiFi calibration at boot. `{"cmd":"info"}` prints the last reset reason,
  uptime and free internal RAM (it was down to ~7.8 KB at boot). If resets persist, try a different cable,
  a powered USB hub, or a wall adapter before suspecting firmware.
- A serial capture that holds DTR while the board resets can leave it in ROM download mode (silent).
  Use `esptool.py --after watchdog_reset read_mac` to reboot it.

## Files

```
main/main.c        controller: the useNovaConversation.ts state machine, button, boot
main/session.c     esp_webrtc session, oai-events data channel
main/signaling.c   SDP exchange through Nova's backend
main/audio.c       I2S mic/speaker, AEC, capture source + speaker render for WebRTC
main/wake.c        "hey nova" (MultiNet7)
main/display.c     round TFT clock + orb (GC9A01 via LVGL/esp_lvgl_port)
main/net.c         WiFi + NVS settings (same provisioning as MicScribe)
main/serial.c      JSON-lines protocol (bridge.py / wifi-setup.html compatible)
main/tasks.c       background-task indicator
main/led.c         onboard RGB LED (blue / pulsing yellow / green)
```

WebRTC comes from Espressif's `esp-webrtc-solution`, which is a git submodule
at `../third_party/esp-webrtc-solution`.
