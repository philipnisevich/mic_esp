#pragma once
// Copy to config.h and fill in. config.h is gitignored.

// Used only until a network is provisioned over serial with
// web/wifi-setup.html, which saves to NVS and wins from then on.
#define WIFI_SSID "your-wifi-ssid"
#define WIFI_PASS "your-wifi-password"

// Where Nova's backend runs. The backend prints this on startup as
// "On your network (for the ESP32 device) -> http://...". Can be changed
// later without reflashing: {"cmd":"set_server","url":"http://..."}
#define NOVA_SERVER_URL "http://192.168.1.10:8787"

// Must match NOVA_DEVICE_TOKEN in Nova/backend/.env. The backend treats this
// device as NOVA_DEVICE_USER_ID, so it uses that user's Gmail/Calendar/etc.
#define NOVA_DEVICE_TOKEN "paste-the-same-token-as-the-backend"

// IANA timezone for "this Thursday at 4". Empty = the server's timezone.
#define NOVA_TIMEZONE ""

// GPT-Live-1 voice. Empty = the backend's default.
#define NOVA_VOICE ""

// Reply window: after Nova finishes an answer you have this long to say
// something. Silence for that long hangs up the session (and stops the billing).
#define NOVA_FOLLOWUP_MS 5000
// The first window, right after the connect chime, is longer so you have time
// to react to the chime.
#define NOVA_FIRST_WINDOW_MS 8000

// Speaker level, 0-100.
#define NOVA_VOLUME 85

// 1: echo cancellation, so you can interrupt Nova mid-sentence like in the
//    browser. 0: the mic is muted while Nova talks. Use 0 if Nova keeps
//    cutting herself off (speaker too loud or too close to the mic).
#define NOVA_ECHO_CANCEL 1

// Onboard RGB LED: blue in a conversation, pulsing yellow while a background
// task runs, green when one finishes. Peak channel value, 0-255; the LED is
// blinding at full scale.
#define NOVA_LED_BRIGHTNESS 40
