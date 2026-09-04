#pragma once
// Copy this file to "config.h" and fill in your values.
// config.h is gitignored so your key never gets committed.

// Used only on a completely fresh board (empty NVS). Once the WiFi setup
// page (web/wifi-setup.html) provisions a network over serial, that choice
// is saved to NVS and wins from then on - these values are never read again
// until NVS is erased. Leave WIFI_SSID as "your-wifi-ssid" to skip the
// compile-time default entirely and require provisioning at first boot.
#define WIFI_SSID "your-wifi-ssid"
#define WIFI_PASS "your-wifi-password"

// https://elevenlabs.io/app/settings/api-keys
#define ELEVENLABS_API_KEY "sk_xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"

// Optional: force a transcription language (ISO-639-1/3, e.g. "en", "es").
// Leave undefined to let Scribe auto-detect.
// #define STT_LANGUAGE "en"

// Optional: pin the TLS root CA instead of skipping certificate validation.
// Grab it with:
//   openssl s_client -showcerts -connect api.elevenlabs.io:443 </dev/null
// and paste the LAST certificate in the chain (the root) here.
// #define ELEVENLABS_ROOT_CA R"CERT(
// -----BEGIN CERTIFICATE-----
// ...
// -----END CERTIFICATE-----
// )CERT"

// ---------------------------------------------------------------- openai ---
// https://platform.openai.com/api-keys
#define OPENAI_API_KEY "sk-proj-xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"

// Pick something fast - this sits in the round trip the user is waiting on.
#define OPENAI_MODEL "gpt-4.1-nano"

// Keep answers short: they have to fit a 128x64 screen and be read aloud-ish.
#define OPENAI_SYSTEM_PROMPT \
  "You are Nova, a voice assistant with a tiny screen. Answer in ONE short " \
  "sentence, 20 words maximum. Give the answer directly with no preamble, no " \
  "restating the question, and no offers of further help. Plain text only: no " \
  "markdown, lists, or emoji. If you do not know, say so in a few words."

// Optional hard cap on the reply. Left out by default because the correct
// field name varies by model family (max_tokens on older chat models,
// max_completion_tokens on newer ones) and sending the wrong one is a 400.
// The system prompt already keeps replies short.
// #define OPENAI_MAX_COMPLETION_TOKENS 150

// Questions about live facts go to a search-backed model instead. It is far
// slower and vastly more expensive per call (measured: 5.7 s / 16.5k tokens
// versus 0.7 s / 37 tokens), so it is used only when the question needs it.
#define OPENAI_SEARCH_MODEL "gpt-5-search-api"

#define OPENAI_SEARCH_SYSTEM_PROMPT \
  "You are Nova, a voice assistant with a tiny screen. Use the search results " \
  "to answer in at most two short sentences. Lead with the fact, and include " \
  "the date only if it matters. Plain text only: no markdown, no URLs, no " \
  "citations, no emoji."

// ------------------------------------------------------------------- tts ---
// Spoken replies through a MAX98357A. "pcm" gives raw 24 kHz 16-bit mono, so
// no decoder is needed on-device. tts-1 measured ~1.6 s versus ~3.2 s for
// gpt-4o-mini-tts, which matters when it sits in the response path.
#define TTS_ENABLED 1
// tts-1 is the fast one (~1.6 s). tts-1-hd sounds noticeably better on a
// small speaker but roughly doubles the wait.
#define TTS_MODEL   "tts-1"
#define TTS_VOICE   "alloy"   // alloy, echo, fable, onyx, nova, shimmer
#define TTS_SAMPLE_RATE 24000 // fixed by the API's pcm format
#define TTS_VOLUME  1.0f      // floor gain; normalisation raises quiet clips
#define TTS_MAX_BOOST 3.0f    // cap on that normalisation; higher just clips
#define TTS_MAX_SECONDS 25    // buffer cap; ~48 KB per second in PSRAM

// ------------------------------------------------------------------ oled ---
// 0.96" SSD1306 over I2C. 128x64 is the usual 0.96" panel; set 32 for the
// half-height 128x32 modules.
#define OLED_WIDTH  128
#define OLED_HEIGHT 64
