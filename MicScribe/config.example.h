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
  "You are Nova, a voice assistant. Your answer is read aloud, so write it the " \
  "way a person would say it. ONE short sentence, 15 words maximum. Plain words " \
  "only. Never use parentheses, brackets, symbols, markdown, emoji, URLs or " \
  "abbreviations. Spell out units and signs as words, for example say degrees, " \
  "percent, dollars. Answer directly with no preamble and no offer of more help. " \
  "If you do not know, say so in a few words."

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
  "You are Nova, a voice assistant. Your answer is read aloud, so write it the " \
  "way a person would say it. Use the search results to answer in ONE short " \
  "sentence, 20 words maximum. Plain words only. Never use parentheses, " \
  "brackets, symbols, markdown, emoji, URLs, citations or source names. Spell " \
  "out units and signs as words, for example say degrees, percent, dollars. " \
  "Lead with the fact and nothing else."

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

// ISRG Root X1. api.openai.com chains to it via Let's Encrypt, verified
// against the live server. Required, not optional: ArduinoWebsockets calls
// setInsecure() only on ESP8266, so on ESP32 a socket with no CA has no root
// of trust at all and the handshake simply fails.
#define OPENAI_ROOT_CA R"CERT(
-----BEGIN CERTIFICATE-----
MIIFazCCA1OgAwIBAgIRAIIQz7DSQONZRGPgu2OCiwAwDQYJKoZIhvcNAQELBQAw
TzELMAkGA1UEBhMCVVMxKTAnBgNVBAoTIEludGVybmV0IFNlY3VyaXR5IFJlc2Vh
cmNoIEdyb3VwMRUwEwYDVQQDEwxJU1JHIFJvb3QgWDEwHhcNMTUwNjA0MTEwNDM4
WhcNMzUwNjA0MTEwNDM4WjBPMQswCQYDVQQGEwJVUzEpMCcGA1UEChMgSW50ZXJu
ZXQgU2VjdXJpdHkgUmVzZWFyY2ggR3JvdXAxFTATBgNVBAMTDElTUkcgUm9vdCBY
MTCCAiIwDQYJKoZIhvcNAQEBBQADggIPADCCAgoCggIBAK3oJHP0FDfzm54rVygc
h77ct984kIxuPOZXoHj3dcKi/vVqbvYATyjb3miGbESTtrFj/RQSa78f0uoxmyF+
0TM8ukj13Xnfs7j/EvEhmkvBioZxaUpmZmyPfjxwv60pIgbz5MDmgK7iS4+3mX6U
A5/TR5d8mUgjU+g4rk8Kb4Mu0UlXjIB0ttov0DiNewNwIRt18jA8+o+u3dpjq+sW
T8KOEUt+zwvo/7V3LvSye0rgTBIlDHCNAymg4VMk7BPZ7hm/ELNKjD+Jo2FR3qyH
B5T0Y3HsLuJvW5iB4YlcNHlsdu87kGJ55tukmi8mxdAQ4Q7e2RCOFvu396j3x+UC
B5iPNgiV5+I3lg02dZ77DnKxHZu8A/lJBdiB3QW0KtZB6awBdpUKD9jf1b0SHzUv
KBds0pjBqAlkd25HN7rOrFleaJ1/ctaJxQZBKT5ZPt0m9STJEadao0xAH0ahmbWn
OlFuhjuefXKnEgV4We0+UXgVCwOPjdAvBbI+e0ocS3MFEvzG6uBQE3xDk3SzynTn
jh8BCNAw1FtxNrQHusEwMFxIt4I7mKZ9YIqioymCzLq9gwQbooMDQaHWBfEbwrbw
qHyGO0aoSCqI3Haadr8faqU9GY/rOPNk3sgrDQoo//fb4hVC1CLQJ13hef4Y53CI
rU7m2Ys6xt0nUW7/vGT1M0NPAgMBAAGjQjBAMA4GA1UdDwEB/wQEAwIBBjAPBgNV
HRMBAf8EBTADAQH/MB0GA1UdDgQWBBR5tFnme7bl5AFzgAiIyBpY9umbbjANBgkq
hkiG9w0BAQsFAAOCAgEAVR9YqbyyqFDQDLHYGmkgJykIrGF1XIpu+ILlaS/V9lZL
ubhzEFnTIZd+50xx+7LSYK05qAvqFyFWhfFQDlnrzuBZ6brJFe+GnY+EgPbk6ZGQ
3BebYhtF8GaV0nxvwuo77x/Py9auJ/GpsMiu/X1+mvoiBOv/2X/qkSsisRcOj/KK
NFtY2PwByVS5uCbMiogziUwthDyC3+6WVwW6LLv3xLfHTjuCvjHIInNzktHCgKQ5
ORAzI4JMPJ+GslWYHb4phowim57iaztXOoJwTdwJx4nLCgdNbOhdjsnvzqvHu7Ur
TkXWStAmzOVyyghqpZXjFaH3pO3JLF+l+/+sKAIuvtd7u+Nxe5AW0wdeRlN8NwdC
jNPElpzVmbUq4JUagEiuTDkHzsxHpFKVK7q4+63SM1N95R1NbdWhscdCb+ZAJzVc
oyi3B43njTOQ5yOf+1CceWxG1bQVs5ZufpsMljq4Ui0/1lvh+wjChP4kqKOJ2qxq
4RgqsahDYVvTH9w7jXbyLeiNdd8XM2w9U/t7y0Ff/9yi0GE44Za4rF2LN9d11TPA
mRGunUHBcnWEvgJBQl9nJEiU0Zsnvgc/ubhPgXRR4Xq37Z0j4r7g1SgEEzwxA57d
emyPxgcYxn/eR44/KJ4EBs+lVDR3veyJm+kXQ99b21/+jh5Xos1AnX5iItreGCc=
-----END CERTIFICATE-----
)CERT"

// -------------------------------------------------------------- realtime ---
// Speech-to-speech over one WebSocket instead of transcribe -> ask -> speak.
// g711 mu-law at 8 kHz is ~8 KB/s each way; pcm16 would be 48 KB/s each way,
// which is more than this chip sustains over TLS while doing anything else.
#define REALTIME_ENABLED 1
#define REALTIME_MODEL   "gpt-realtime-mini"
#define REALTIME_VOICE   "alloy"

#define REALTIME_INSTRUCTIONS \
  "You are Nova, a voice assistant on a small device. Keep every reply to one " \
  "or two short spoken sentences. Be direct and warm. Never spell out symbols " \
  "or markdown. If you do not know, say so briefly. Answer a simple lookup, " \
  "such as the time, the weather, a score or a price, in one short sentence. " \
  "Explain a concept or give advice in two sentences so the answer stands on " \
  "its own."

// A reply that is not a question ends the session immediately. These only
// apply while waiting for you to speak.
#define REALTIME_IDLE_MS       12000  // no speech at all after connecting
#define REALTIME_FOLLOWUP_MS    3000  // open topic: how long to wait for you
// Replies at least this long read as explanations rather than lookups, and
// leave the session open for a follow-up.
#define REALTIME_OPEN_CHARS      110
#define REALTIME_MAX_MS       120000  // hard ceiling on one session

// ------------------------------------------------------------------ oled ---
// 0.96" SSD1306 over I2C. 128x64 is the usual 0.96" panel; set 32 for the
// half-height 128x32 modules.
#define OLED_WIDTH  128
#define OLED_HEIGHT 64
