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
