#pragma once
// Copy this file to "config.h" and fill in your values.
// config.h is gitignored so your key never gets committed.

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
#define OPENAI_MODEL "gpt-4o-mini"

// Keep answers short: they have to fit a 128x64 screen and be read aloud-ish.
#define OPENAI_SYSTEM_PROMPT \
  "You are Nova, a voice assistant on a tiny device with a 128x64 screen. " \
  "Answer in at most 2 short sentences. Plain text only - no markdown, no " \
  "lists, no emoji. If you do not know, say so briefly."

// Optional hard cap on the reply. Left out by default because the correct
// field name varies by model family (max_tokens on older chat models,
// max_completion_tokens on newer ones) and sending the wrong one is a 400.
// The system prompt already keeps replies short.
// #define OPENAI_MAX_COMPLETION_TOKENS 150

// ------------------------------------------------------------------ oled ---
// 0.96" SSD1306 over I2C. 128x64 is the usual 0.96" panel; set 32 for the
// half-height 128x32 modules.
#define OLED_WIDTH  128
#define OLED_HEIGHT 64
