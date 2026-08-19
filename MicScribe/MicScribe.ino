// MicScribe - push-to-talk speech to text, entirely on an ESP32-S3.
//
// Hold the button -> INMP441 audio is captured over I2S into PSRAM.
// Release the button -> a WAV is streamed straight to the ElevenLabs
// Scribe API from the ESP32 and the transcript comes back out of the
// serial port. No host-side helper is involved in the transcription.
//
// Board: "ESP32S3 Dev Module", PSRAM enabled.

#include <Arduino.h>
#include <WiFi.h>
#include <NetworkClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <ESP_I2S.h>

#include "config.h"

// ---------------------------------------------------------------- wiring ---
// INMP441 -> ESP32-S3
//   VDD  -> 3V3
//   GND  -> GND
//   L/R  -> GND   (left channel; tie to 3V3 and set MIC_SLOT to RIGHT)
//   WS   -> PIN_I2S_WS
//   SCK  -> PIN_I2S_BCLK
//   SD   -> PIN_I2S_DIN
static const int PIN_I2S_BCLK = 4;
static const int PIN_I2S_WS   = 5;
static const int PIN_I2S_DIN  = 6;

// Push-to-talk button, wired between the pin and GND. GPIO0 is the BOOT
// button on most S3 dev boards, so this works with no extra hardware.
static const int PIN_BUTTON = 0;

// INMP441 with L/R tied low sits in the left slot.
static const i2s_std_slot_mask_t MIC_SLOT = I2S_STD_SLOT_LEFT;

// ---------------------------------------------------------------- audio ----
static const uint32_t SAMPLE_RATE = 16000;   // plenty for speech, small payloads
static const float    MIC_GAIN    = 10.0f;   // fixed pre-gain, see normalise()
static const float    HPF_ALPHA   = 0.995f;  // ~13 Hz high-pass, kills DC offset
static const uint32_t MIN_RECORD_MS = 300;   // ignore accidental taps

static const size_t WAV_HEADER_SIZE = 44;
static const size_t I2S_CHUNK_FRAMES = 256;  // 16 ms per read at 16 kHz

// Recording length depends on where the buffer can live.
static const uint32_t MAX_SECONDS_PSRAM    = 20;
static const uint32_t MAX_SECONDS_NO_PSRAM = 6;

// ---------------------------------------------------------------- state ----
static I2SClass I2S;

static uint8_t *gBuffer     = nullptr;  // [WAV header][PCM ...]
static size_t   gCapacity   = 0;        // total bytes allocated
static size_t   gMaxSamples = 0;
static size_t   gSamples    = 0;        // samples captured this take

static bool     gRecording      = false;
static uint32_t gRecordStartMs  = 0;
static int32_t  gPeak           = 0;
static float    gHpfX1 = 0.0f, gHpfY1 = 0.0f;

static inline int16_t *pcm() { return (int16_t *)(gBuffer + WAV_HEADER_SIZE); }

// ------------------------------------------------------------ status LED ---
// Plain constants rather than an enum: the .ino preprocessor hoists function
// prototypes above any type declared in this file.
static const uint8_t STATUS_IDLE      = 0;
static const uint8_t STATUS_RECORDING = 1;
static const uint8_t STATUS_WORKING   = 2;
static const uint8_t STATUS_OK        = 3;
static const uint8_t STATUS_ERROR     = 4;

static void setStatus(uint8_t s) {
#ifdef RGB_BUILTIN
  switch (s) {
    case STATUS_IDLE:      rgbLedWrite(RGB_BUILTIN, 0, 0, 2);    break;  // dim blue
    case STATUS_RECORDING: rgbLedWrite(RGB_BUILTIN, 40, 0, 0);   break;  // red
    case STATUS_WORKING:   rgbLedWrite(RGB_BUILTIN, 30, 20, 0);  break;  // amber
    case STATUS_OK:        rgbLedWrite(RGB_BUILTIN, 0, 40, 0);   break;  // green
    case STATUS_ERROR:     rgbLedWrite(RGB_BUILTIN, 40, 0, 40);  break;  // magenta
  }
#else
  (void)s;
#endif
}

// --------------------------------------------------------- serial output ---
// Human-readable chatter is prefixed with '#'. Results are emitted as one
// JSON object per line so a bridge can parse them without guessing.
static void logf(const char *fmt, ...) {
  char line[256];
  va_list args;
  va_start(args, fmt);
  vsnprintf(line, sizeof(line), fmt, args);
  va_end(args);
  Serial.print("# ");
  Serial.println(line);
}

static void emitEvent(const char *type, const char *key, const char *value) {
  JsonDocument doc;
  doc["type"] = type;
  doc[key] = value;
  serializeJson(doc, Serial);
  Serial.println();
}

static void emitTranscript(const char *text, const char *language) {
  JsonDocument doc;
  doc["type"] = "transcript";
  doc["text"] = text;
  if (language && *language) doc["language"] = language;
  serializeJson(doc, Serial);
  Serial.println();
}

// -------------------------------------------------------- multipart body ---
// HTTPClient can stream a request body out of any Stream, which lets us POST
// a ~640 KB WAV without ever building a second copy of it in RAM.
struct Segment {
  const uint8_t *data;
  size_t len;
};

class SegmentStream : public Stream {
public:
  SegmentStream(const Segment *segs, size_t count) : _segs(segs), _count(count) {
    for (size_t i = 0; i < count; i++) _total += segs[i].len;
    _remaining = _total;
  }

  size_t total() const { return _total; }

  int available() override {
    return _remaining > (size_t)INT_MAX ? INT_MAX : (int)_remaining;
  }

  int peek() override {
    skipEmpty();
    if (_seg >= _count) return -1;
    return _segs[_seg].data[_off];
  }

  int read() override {
    uint8_t b;
    return readBytes((char *)&b, 1) == 1 ? b : -1;
  }

  using Stream::readBytes;
  size_t readBytes(char *buffer, size_t length) override {
    size_t done = 0;
    while (done < length) {
      skipEmpty();
      if (_seg >= _count) break;
      size_t chunk = _segs[_seg].len - _off;
      if (chunk > length - done) chunk = length - done;
      memcpy(buffer + done, _segs[_seg].data + _off, chunk);
      _off += chunk;
      done += chunk;
      _remaining -= chunk;
    }
    return done;
  }

  size_t write(uint8_t) override { return 0; }
  void flush() override {}

private:
  void skipEmpty() {
    while (_seg < _count && _off >= _segs[_seg].len) {
      _seg++;
      _off = 0;
    }
  }

  const Segment *_segs;
  size_t _count;
  size_t _seg = 0;
  size_t _off = 0;
  size_t _total = 0;
  size_t _remaining = 0;
};

// ------------------------------------------------------------ wav header ---
static void writeWavHeader(uint8_t *dst, size_t pcmBytes) {
  const uint32_t byteRate   = SAMPLE_RATE * 2;  // mono, 16-bit
  const uint32_t riffSize   = 36 + pcmBytes;
  auto put32 = [](uint8_t *p, uint32_t v) {
    p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; p[2] = (v >> 16) & 0xFF; p[3] = (v >> 24) & 0xFF;
  };
  auto put16 = [](uint8_t *p, uint16_t v) {
    p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF;
  };

  memcpy(dst + 0, "RIFF", 4);
  put32(dst + 4, riffSize);
  memcpy(dst + 8, "WAVEfmt ", 8);
  put32(dst + 16, 16);          // fmt chunk size
  put16(dst + 20, 1);           // PCM
  put16(dst + 22, 1);           // mono
  put32(dst + 24, SAMPLE_RATE);
  put32(dst + 28, byteRate);
  put16(dst + 32, 2);           // block align
  put16(dst + 34, 16);          // bits per sample
  memcpy(dst + 36, "data", 4);
  put32(dst + 40, pcmBytes);
}

// ------------------------------------------------------------- recording ---
static void startRecording() {
  gSamples = 0;
  gPeak = 0;
  gHpfX1 = 0.0f;
  gHpfY1 = 0.0f;
  gRecordStartMs = millis();
  gRecording = true;

  // The DMA ring still holds audio from before the button press, plus the mic
  // needs a moment to settle. Throw away the first ~100 ms.
  static int32_t scratch[I2S_CHUNK_FRAMES];
  for (int i = 0; i < 7; i++) {
    I2S.readBytes((char *)scratch, sizeof(scratch));
  }

  setStatus(STATUS_RECORDING);
  logf("recording...");
  emitEvent("state", "state", "recording");
}

// Pull one chunk out of I2S and append it to the buffer.
static void captureChunk() {
  static int32_t raw[I2S_CHUNK_FRAMES];

  size_t bytes = I2S.readBytes((char *)raw, sizeof(raw));
  size_t frames = bytes / sizeof(int32_t);
  if (frames == 0) return;

  int16_t *out = pcm();
  for (size_t i = 0; i < frames && gSamples < gMaxSamples; i++) {
    // INMP441 gives 24 bits left-justified inside a 32-bit slot.
    float x = (float)(raw[i] >> 8);

    // One-pole high-pass to strip the mic's DC offset and low rumble.
    float y = HPF_ALPHA * (gHpfY1 + x - gHpfX1);
    gHpfX1 = x;
    gHpfY1 = y;

    int32_t s = (int32_t)(y * MIC_GAIN / 256.0f);  // 24-bit domain -> 16-bit
    if (s > 32767) s = 32767;
    if (s < -32768) s = -32768;

    int32_t mag = s < 0 ? -s : s;
    if (mag > gPeak) gPeak = mag;

    out[gSamples++] = (int16_t)s;
  }
}

// Bring quiet takes up to a healthy level without clipping loud ones.
static void normalise() {
  if (gPeak <= 0 || gPeak >= 26000) return;
  float scale = 26000.0f / (float)gPeak;
  if (scale > 8.0f) scale = 8.0f;

  int16_t *out = pcm();
  for (size_t i = 0; i < gSamples; i++) {
    int32_t s = (int32_t)(out[i] * scale);
    if (s > 32767) s = 32767;
    if (s < -32768) s = -32768;
    out[i] = (int16_t)s;
  }
  logf("normalised x%.1f (peak was %d)", scale, (int)gPeak);
}

// ----------------------------------------------------------- elevenlabs ----
static bool transcribe(size_t pcmBytes) {
  if (WiFi.status() != WL_CONNECTED) {
    emitEvent("error", "error", "wifi not connected");
    return false;
  }

  const char *boundary = "----MicScribeBoundaryZ7Qk1x";

  String head;
  head.reserve(512);
  head += "--"; head += boundary; head += "\r\n";
  head += "Content-Disposition: form-data; name=\"model_id\"\r\n\r\nscribe_v1\r\n";
#ifdef STT_LANGUAGE
  head += "--"; head += boundary; head += "\r\n";
  head += "Content-Disposition: form-data; name=\"language_code\"\r\n\r\n";
  head += STT_LANGUAGE; head += "\r\n";
#endif
  head += "--"; head += boundary; head += "\r\n";
  head += "Content-Disposition: form-data; name=\"file\"; filename=\"speech.wav\"\r\n";
  head += "Content-Type: audio/wav\r\n\r\n";

  String tail = "\r\n--";
  tail += boundary;
  tail += "--\r\n";

  Segment segments[3] = {
    {(const uint8_t *)head.c_str(), head.length()},
    {gBuffer, WAV_HEADER_SIZE + pcmBytes},
    {(const uint8_t *)tail.c_str(), tail.length()},
  };
  SegmentStream body(segments, 3);

  NetworkClientSecure client;
#ifdef ELEVENLABS_ROOT_CA
  client.setCACert(ELEVENLABS_ROOT_CA);
#else
  // No CA pinned: the payload is still encrypted, but the server identity is
  // not verified. Define ELEVENLABS_ROOT_CA in config.h to harden this.
  client.setInsecure();
#endif
  client.setHandshakeTimeout(15);

  HTTPClient http;
  http.setConnectTimeout(10000);
  http.setTimeout(45000);
  http.setReuse(false);

  if (!http.begin(client, "https://api.elevenlabs.io/v1/speech-to-text")) {
    emitEvent("error", "error", "http begin failed");
    return false;
  }

  http.addHeader("xi-api-key", ELEVENLABS_API_KEY);
  http.addHeader("Content-Type", String("multipart/form-data; boundary=") + boundary);
  http.addHeader("Accept", "application/json");

  logf("uploading %u bytes...", (unsigned)body.total());
  uint32_t t0 = millis();
  int code = http.sendRequest("POST", (Stream *)&body, body.total());

  if (code <= 0) {
    String err = HTTPClient::errorToString(code);
    logf("transport error: %s", err.c_str());
    emitEvent("error", "error", err.c_str());
    http.end();
    return false;
  }

  String payload = http.getString();
  http.end();
  logf("http %d in %lu ms", code, (unsigned long)(millis() - t0));

  if (code != HTTP_CODE_OK) {
    String msg = String("http ") + code + ": " + payload.substring(0, 180);
    emitEvent("error", "error", msg.c_str());
    return false;
  }

  JsonDocument doc;
  DeserializationError jsonErr = deserializeJson(doc, payload);
  if (jsonErr) {
    emitEvent("error", "error", jsonErr.c_str());
    return false;
  }

  const char *text = doc["text"] | "";
  const char *lang = doc["language_code"] | "";
  emitTranscript(text, lang);
  return true;
}

// ---------------------------------------------------------------- button ---
// Simple debounce: the level has to stay put for DEBOUNCE_MS before it counts.
static const uint32_t DEBOUNCE_MS = 25;
static bool     gButtonDown    = false;
static bool     gLastRawDown   = false;
static uint32_t gLastChangeMs  = 0;

static bool buttonPressedStable() {
  bool raw = digitalRead(PIN_BUTTON) == LOW;
  if (raw != gLastRawDown) {
    gLastRawDown = raw;
    gLastChangeMs = millis();
  } else if (raw != gButtonDown && (millis() - gLastChangeMs) >= DEBOUNCE_MS) {
    gButtonDown = raw;
  }
  return gButtonDown;
}

// ----------------------------------------------------------------- setup ---
static void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  logf("connecting to %s", WIFI_SSID);

  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    delay(250);
  }
  if (WiFi.status() == WL_CONNECTED) {
    logf("wifi ok, ip %s, rssi %d", WiFi.localIP().toString().c_str(), WiFi.RSSI());
  } else {
    logf("wifi FAILED - will retry in the background");
  }
}

static bool allocateBuffer() {
  size_t psram = ESP.getPsramSize();
  uint32_t seconds = psram > 0 ? MAX_SECONDS_PSRAM : MAX_SECONDS_NO_PSRAM;

  gMaxSamples = (size_t)SAMPLE_RATE * seconds;
  gCapacity = WAV_HEADER_SIZE + gMaxSamples * 2;

  gBuffer = psram > 0 ? (uint8_t *)ps_malloc(gCapacity) : (uint8_t *)malloc(gCapacity);
  if (!gBuffer) {
    gMaxSamples = 0;
    return false;
  }

  logf("buffer: %u KB in %s, max %lu s of audio",
       (unsigned)(gCapacity / 1024), psram > 0 ? "PSRAM" : "internal RAM",
       (unsigned long)seconds);
  return true;
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println();
  logf("MicScribe booting");

  pinMode(PIN_BUTTON, INPUT_PULLUP);

  if (!allocateBuffer()) {
    logf("FATAL: could not allocate the audio buffer");
    while (true) {
      setStatus(STATUS_ERROR);
      delay(1000);
    }
  }

  I2S.setPins(PIN_I2S_BCLK, PIN_I2S_WS, -1 /* no dout */, PIN_I2S_DIN, -1 /* no mclk */);
  if (!I2S.begin(I2S_MODE_STD, SAMPLE_RATE, I2S_DATA_BIT_WIDTH_32BIT,
                 I2S_SLOT_MODE_MONO, (int8_t)MIC_SLOT)) {
    logf("FATAL: I2S init failed");
    while (true) {
      setStatus(STATUS_ERROR);
      delay(1000);
    }
  }
  logf("i2s up: bclk=%d ws=%d din=%d @ %lu Hz",
       PIN_I2S_BCLK, PIN_I2S_WS, PIN_I2S_DIN, (unsigned long)SAMPLE_RATE);

  connectWiFi();

  setStatus(STATUS_IDLE);
  logf("ready - hold GPIO%d to talk, release to transcribe", PIN_BUTTON);
  emitEvent("state", "state", "ready");
}

void loop() {
  bool pressed = buttonPressedStable();

  if (pressed && !gRecording) {
    startRecording();
  }

  if (gRecording) {
    captureChunk();

    if (gSamples >= gMaxSamples) {
      logf("hit the length limit, cutting the take short");
      pressed = false;
    }

    if (!pressed) {
      gRecording = false;
      uint32_t durationMs = millis() - gRecordStartMs;
      size_t pcmBytes = gSamples * 2;

      if (durationMs < MIN_RECORD_MS || gSamples == 0) {
        logf("too short (%lu ms), ignoring", (unsigned long)durationMs);
        setStatus(STATUS_IDLE);
        emitEvent("state", "state", "ready");
        return;
      }

      logf("captured %.2f s, peak %d", gSamples / (float)SAMPLE_RATE, (int)gPeak);
      if (gPeak < 200) {
        logf("WARNING: signal is nearly silent - check mic wiring and MIC_SLOT");
      }

      normalise();
      writeWavHeader(gBuffer, pcmBytes);

      setStatus(STATUS_WORKING);
      emitEvent("state", "state", "transcribing");

      bool ok = transcribe(pcmBytes);
      setStatus(ok ? STATUS_OK : STATUS_ERROR);
      delay(400);
      setStatus(STATUS_IDLE);
      emitEvent("state", "state", "ready");
    }
    return;
  }

  // Idle: keep WiFi alive and stay responsive to the button.
  static uint32_t lastWifiCheck = 0;
  if (millis() - lastWifiCheck > 10000) {
    lastWifiCheck = millis();
    if (WiFi.status() != WL_CONNECTED) {
      logf("wifi dropped, reconnecting");
      WiFi.reconnect();
    }
  }
  delay(5);
}
