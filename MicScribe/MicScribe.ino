// MicScribe - hands-free speech to text, entirely on an ESP32-S3.
//
// Two ways to start a take:
//   say "Hey Nova"   -> ESP-SR MultiNet spots the phrase on-chip, then audio
//                       is captured until you stop talking (energy VAD)
//   hold the button  -> classic push-to-talk, captured until you let go
//
// Either way the audio lands in PSRAM as a WAV and is streamed straight to the
// ElevenLabs Scribe API from the board itself. The transcript comes back out
// of the serial port. No host-side helper is involved.
//
// Wake detection uses MultiNet (mn5q8_en) in continuous command mode rather
// than WakeNet, because WakeNet wake words are trained models and the only one
// bundled with the core is "Hi, ESP". MultiNet takes phrases as runtime
// phoneme strings, so "Hey Nova" costs nothing but a table entry. The
// trade-off is more false accepts than a purpose-trained wake model.
//
// Board: "ESP32S3 Dev Module", PSRAM=OPI, Partition Scheme "ESP SR 16M".

#include <Arduino.h>
#include <WiFi.h>
#include <NetworkClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <ESP_I2S.h>
#include <ESP_SR.h>

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
// 10.0 clipped at full scale on normal speech, which costs wake-word accuracy
// far more than it costs Scribe. 4.0 leaves headroom for normalise() to use.
static const float    MIC_GAIN    = 4.0f;
static const float    HPF_ALPHA   = 0.995f;  // ~13 Hz high-pass, kills DC offset
static const uint32_t MIN_RECORD_MS = 300;   // ignore accidental taps

// Wake-word capture. Detection fires slightly after the phrase ends, so a
// short pre-roll keeps the first word of what follows from being clipped. Too
// much pre-roll and the tail of "Nova" lands in the transcript - if you see
// that, lower this first.
static const uint32_t PREROLL_MS      = 300;
static const uint32_t VAD_SILENCE_MS  = 800;   // quiet this long ends a take
static const uint32_t VAD_MIN_SPEECH_MS = 400; // ...but never before this much
static const float    VAD_NOISE_MULT  = 3.0f;  // speech = this x the noise floor
static const uint32_t VAD_MIN_LEVEL   = 250;   // absolute floor for a dead-quiet room

// A take whose loudest sample never gets above this almost certainly contains
// no speech - a false wake on room noise, or a stray button tap. Cheap to
// check, and it saves an API call. Real speech peaks around 5000-8000 with
// MIC_GAIN at 4.0; an idle room false-accept measured 271.
static const int32_t  SPEECH_MIN_PEAK = 1200;

// After the wake word, wait this long for you to actually start talking before
// giving up. Without it the silence timer starts counting immediately and the
// take ends ~800 ms after the wake word if you pause to think.
static const uint32_t WAKE_LEAD_IN_MS = 5000;
// Detection fires as the phrase finishes, so the first moments of live audio
// can still hold the tail of "Nova". Ignore them, or that tail counts as
// speech and the silence timer starts anyway - reintroducing the same bug.
static const uint32_t WAKE_BLANK_MS = 250;

static const size_t WAV_HEADER_SIZE = 44;
static const size_t I2S_CHUNK_FRAMES = 256;  // 16 ms per read at 16 kHz

// Recording length depends on where the buffer can live.
static const uint32_t MAX_SECONDS_PSRAM    = 20;
// Without PSRAM the buffer competes with the TLS stack for internal RAM, and
// mbedtls needs ~40-50 KB free to complete a handshake. 3 s costs 96 KB, which
// still leaves it room; 6 s did not, and the upload failed with
// "SSL - Memory allocation failed".
static const uint32_t MAX_SECONDS_NO_PSRAM = 3;

// ---------------------------------------------------------------- state ----
static I2SClass I2S;

static uint8_t *gBuffer     = nullptr;  // [WAV header][PCM ...]
static size_t   gCapacity   = 0;        // total bytes allocated
static size_t   gMaxSamples = 0;
static size_t   gSamples    = 0;        // samples captured this take

static int32_t  gPeak           = 0;
static float    gHpfX1 = 0.0f, gHpfY1 = 0.0f;

// Capture state machine. The SR fill callback is the only writer of the audio
// buffers, so every transition happens there and loop() just observes.
static const uint8_t ST_LISTENING = 0;  // idle, filling the pre-roll ring
static const uint8_t ST_CAPTURING = 1;  // recording a take
static const uint8_t ST_PENDING   = 2;  // take finished, waiting for loop()
static const uint8_t ST_UPLOADING = 3;  // loop() is talking to ElevenLabs

static volatile uint8_t gState = ST_LISTENING;
static volatile bool    gWakeRequested = false;  // set by the SR event task
static volatile bool    gButtonHeld    = false;  // set by loop()
static bool     gManualCapture = false;          // this take came from the button
static const char *gEndReason = "";

// Pre-roll ring, so a take does not start mid-syllable.
static int16_t *gRing    = nullptr;
static size_t   gRingLen = 0;
static size_t   gRingPos = 0;
static bool     gRingWrapped = false;

// Energy VAD.
static float    gNoiseFloor = 200.0f;
static uint32_t gSilenceMs  = 0;
static uint32_t gCapturedMs = 0;
static size_t   gPrerollSamples = 0;  // how much of a take is replayed pre-roll
static bool     gSpeechSeen = false;  // has the talking actually started yet?

// Scratch for the 32-bit I2S reads that get converted down to 16-bit.
static int32_t *gScratch      = nullptr;
static size_t   gScratchCount = 0;

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

// ------------------------------------------------------------- wake word ---
// Phonemes generated with the core's own tool, so they match what MultiNet
// was trained to expect:
//   python3 libraries/ESP_SR/tools/gen_sr_commands.py "Hey Nova,Hi Nova,Okay Nova"
// All three map to command 0 - any of them starts a take. Bare "Nova" is
// deliberately left out: one short syllable false-accepts constantly.
static const sr_cmd_t SR_COMMANDS[] = {
  {0, "Hey Nova",  "hd NbVc"},
  {0, "Hi Nova",   "hi NbVc"},
  {0, "Okay Nova", "bKd NbVc"},
};

// ------------------------------------------------------------- recording ---
static void ringWrite(const int16_t *src, size_t n) {
  if (!gRing || gRingLen == 0) return;
  if (n >= gRingLen) {           // this chunk alone overruns the ring
    memcpy(gRing, src + (n - gRingLen), gRingLen * sizeof(int16_t));
    gRingPos = 0;
    gRingWrapped = true;
    return;
  }
  size_t tail = gRingLen - gRingPos;
  if (n <= tail) {
    memcpy(gRing + gRingPos, src, n * sizeof(int16_t));
    gRingPos += n;
  } else {
    memcpy(gRing + gRingPos, src, tail * sizeof(int16_t));
    memcpy(gRing, src + tail, (n - tail) * sizeof(int16_t));
    gRingPos = n - tail;
    gRingWrapped = true;
  }
  if (gRingPos >= gRingLen) {
    gRingPos = 0;
    gRingWrapped = true;
  }
}

static void ringReset() {
  gRingPos = 0;
  gRingWrapped = false;
}

// Append raw samples to the take, tracking the peak as we go.
static void appendSamples(const int16_t *src, size_t n) {
  int16_t *out = pcm();
  for (size_t i = 0; i < n && gSamples < gMaxSamples; i++) {
    int16_t s = src[i];
    out[gSamples++] = s;
    int32_t mag = s < 0 ? -(int32_t)s : (int32_t)s;
    if (mag > gPeak) gPeak = mag;
  }
}

static void startCapture(bool manual) {
  gSamples = 0;
  gPeak = 0;
  gSilenceMs = 0;
  gCapturedMs = 0;
  gManualCapture = manual;
  gSpeechSeen = false;
  gEndReason = "";

  // Replay the pre-roll so the take does not begin mid-word.
  if (gRing && gRingLen) {
    if (gRingWrapped) {
      appendSamples(gRing + gRingPos, gRingLen - gRingPos);
      appendSamples(gRing, gRingPos);
    } else {
      appendSamples(gRing, gRingPos);
    }
  }
  gPrerollSamples = gSamples;  // everything so far is pre-roll, not live audio
  ringReset();
  gState = ST_CAPTURING;
}

static void endCapture(const char *reason) {
  gEndReason = reason;
  gState = ST_PENDING;
}

// Runs in the SR feed task, once per audio chunk. This is the only place the
// audio buffers are written, which is what keeps the state machine race-free.
static void processAudio(const int16_t *pcm16, size_t n, uint32_t level) {
  if (n == 0) return;
  uint32_t chunkMs = (uint32_t)((n * 1000) / SAMPLE_RATE);

  uint8_t st = gState;
  if (st == ST_PENDING || st == ST_UPLOADING) {
    return;  // still draining I2S so nothing goes stale, but discard the audio
  }

  if (st == ST_LISTENING) {
    // Track the room's noise floor while nothing is happening.
    gNoiseFloor = gNoiseFloor * 0.98f + (float)level * 0.02f;
    ringWrite(pcm16, n);

    bool manual = gButtonHeld;
    if (!gWakeRequested && !manual) return;
    gWakeRequested = false;
    startCapture(manual);
    // fall through so this chunk is captured too
  }

  appendSamples(pcm16, n);
  gCapturedMs += chunkMs;

  if (gManualCapture) {
    if (!gButtonHeld) endCapture("button released");
  } else if (gCapturedMs >= WAKE_BLANK_MS) {
    // gCapturedMs counts live audio only - the pre-roll is not added to it.
    float threshold = gNoiseFloor * VAD_NOISE_MULT;
    if (threshold < (float)VAD_MIN_LEVEL) threshold = (float)VAD_MIN_LEVEL;

    if ((float)level >= threshold) {
      gSpeechSeen = true;
      gSilenceMs = 0;
    } else if (gSpeechSeen) {
      gSilenceMs += chunkMs;  // only meaningful once talking has begun
    }

    if (!gSpeechSeen) {
      if (gCapturedMs >= WAKE_LEAD_IN_MS) endCapture("no speech after wake");
    } else if (gSilenceMs >= VAD_SILENCE_MS && gCapturedMs >= VAD_MIN_SPEECH_MS) {
      endCapture("silence");
    }
  }

  if (gSamples >= gMaxSamples) endCapture("length limit");
}

// ESP-SR pulls audio through this instead of reading I2S itself, which lets a
// single reader tee the same samples into both the detector and our buffers.
// It hands us a 16-bit mono buffer; the INMP441 gives 24-bit in 32-bit slots,
// so we read double and convert in place.
static esp_err_t srFill(void *arg, void *out, size_t len, size_t *bytes_read, uint32_t timeout_ms) {
  size_t frames = len / sizeof(int16_t);

  if (gScratchCount < frames) {
    int32_t *grown = (int32_t *)heap_caps_realloc(gScratch, frames * sizeof(int32_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!grown) {
      *bytes_read = 0;
      return ESP_ERR_NO_MEM;
    }
    gScratch = grown;
    gScratchCount = frames;
  }

  size_t got = I2S.readBytes((char *)gScratch, frames * sizeof(int32_t));
  size_t n = got / sizeof(int32_t);

  int16_t *dst = (int16_t *)out;
  uint64_t acc = 0;
  for (size_t i = 0; i < n; i++) {
    float x = (float)(gScratch[i] >> 8);          // 24-bit, sign preserved
    float y = HPF_ALPHA * (gHpfY1 + x - gHpfX1);  // strip DC and rumble
    gHpfX1 = x;
    gHpfY1 = y;

    int32_t s = (int32_t)(y * MIC_GAIN / 256.0f);
    if (s > 32767) s = 32767;
    if (s < -32768) s = -32768;
    dst[i] = (int16_t)s;
    acc += (uint32_t)(s < 0 ? -s : s);
  }
  for (size_t i = n; i < frames; i++) dst[i] = 0;

  *bytes_read = frames * sizeof(int16_t);
  processAudio(dst, n, n ? (uint32_t)(acc / n) : 0);
  return ESP_OK;
}

// Runs in the SR detect task. It only raises a flag - the fill callback owns
// the buffers, so it performs the actual transition on the next chunk.
static void srEvent(void *arg, sr_event_t event, int command_id, int phrase_id) {
  switch (event) {
    case SR_EVENT_COMMAND:
      if (gState == ST_LISTENING) {
        gWakeRequested = true;
      }
      // MultiNet stops detecting after a hit until the mode is set again, so
      // without this the wake word works exactly once per boot.
      sr_set_mode(SR_MODE_COMMAND);
      break;
    case SR_EVENT_TIMEOUT:
      // Command mode times out on its own; we want it listening forever.
      sr_set_mode(SR_MODE_COMMAND);
      break;
    default: break;
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

// ---------------------------------------------------------- diagnostics ----
// Called when the upload fails at the transport layer, which lumps together
// DNS failure, a blocked port, and a broken TLS handshake. Walk the stack one
// layer at a time so the log says which one it actually was.
static void logNetworkDiagnostics() {
  logf("wifi: status=%d ssid=%s ip=%s gw=%s dns=%s rssi=%d",
       (int)WiFi.status(), WiFi.SSID().c_str(),
       WiFi.localIP().toString().c_str(), WiFi.gatewayIP().toString().c_str(),
       WiFi.dnsIP().toString().c_str(), WiFi.RSSI());
  logf("heap: free=%u largest=%u psram_free=%u",
       (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap(),
       (unsigned)ESP.getFreePsram());

  IPAddress addr;
  if (!WiFi.hostByName("api.elevenlabs.io", addr)) {
    logf("dns: FAILED to resolve api.elevenlabs.io");
    return;
  }
  logf("dns: api.elevenlabs.io -> %s", addr.toString().c_str());

  NetworkClient plain;
  if (!plain.connect(addr, 443, 8000)) {
    logf("tcp: FAILED to open port 443 - blocked, or a captive portal");
    return;
  }
  logf("tcp: port 443 reachable");
  plain.stop();

  NetworkClientSecure tls;
  tls.setInsecure();
  tls.setHandshakeTimeout(15);
  if (tls.connect("api.elevenlabs.io", 443)) {
    logf("tls: handshake OK - the transport is fine, retry may succeed");
    tls.stop();
  } else {
    char err[128] = {0};
    int code = tls.lastError(err, sizeof(err));
    logf("tls: handshake FAILED (%d) %s", code, err);
  }
}

// Scribe returns bracketed event tags like "[typing]", "[silence]" or
// "[keyboard clacking]" when it hears no speech. Those are not transcripts and
// must never be typed into whatever the user has focused, so treat a result
// that is nothing but tags (or empty) as noise.
static bool isNonSpeech(const char *text) {
  if (!text) return true;
  const char *p = text;
  while (*p) {
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (!*p) break;
    if (*p != '[') return false;  // real words are present
    const char *close = strchr(p, ']');
    if (!close) return false;
    p = close + 1;
  }
  return true;  // only tags, or nothing at all
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
    http.end();
    logNetworkDiagnostics();
    emitEvent("error", "error", err.c_str());
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

  if (isNonSpeech(text)) {
    logf("no speech in result, discarding: %s", text);
    emitEvent("noise", "text", text);
    return true;
  }

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

  gRingLen = (size_t)((SAMPLE_RATE * PREROLL_MS) / 1000);
  gRing = psram > 0 ? (int16_t *)ps_malloc(gRingLen * sizeof(int16_t))
                    : (int16_t *)malloc(gRingLen * sizeof(int16_t));
  if (!gRing) {
    gRingLen = 0;
    logf("WARNING: no pre-roll buffer, takes may clip their first word");
  }

  logf("buffer: %u KB in %s, max %lu s of audio, %lu ms pre-roll",
       (unsigned)(gCapacity / 1024), psram > 0 ? "PSRAM" : "internal RAM",
       (unsigned long)seconds, (unsigned long)PREROLL_MS);
  return true;
}

// Continuous on-chip phrase spotting. MultiNet is normally the second stage
// behind a wake word, so we run it in command mode permanently and treat a
// command hit as the wake.
static bool startWakeWord() {
  // input_format must describe THREE channels, not one. esp32-hal-sr.c hardcodes
  // SR_CHANNEL_NUM = 3 and always expands whatever the fill callback returns
  // into 3 interleaved channels before calling afe feed(). Passing "M" makes
  // the AFE read that 3-channel stream as mono, so the detector only ever sees
  // garbage and nothing is ever recognised. "MNN" = our mic in channel 0, the
  // two channels the HAL zero-fills marked unused.
  esp_err_t err = sr_start(
    srFill, nullptr, SR_CHANNELS_MONO, SR_MODE_COMMAND, "MNN",
    SR_COMMANDS, sizeof(SR_COMMANDS) / sizeof(SR_COMMANDS[0]), srEvent, nullptr
  );
  if (err != ESP_OK) {
    logf("FATAL: sr_start failed (%d) %s", (int)err, esp_err_to_name(err));
    return false;
  }
  logf("wake word ready: say \"Hey Nova\" (or Hi/Okay Nova)");
  logf("heap after SR: free=%u largest=%u psram_free=%u",
       (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap(),
       (unsigned)ESP.getFreePsram());
  return true;
}

void setup() {
  Serial.begin(115200);
  // Over native USB CDC the host has to enumerate and open the port before
  // anything we print is actually delivered, so give it a moment. Falls
  // through after the timeout when nobody is listening.
  uint32_t serialWait = millis();
  while (!Serial && millis() - serialWait < 2000) {
    delay(10);
  }
  delay(200);
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

  // After WiFi, so the network stack claims its internal RAM before ESP-SR
  // takes what it needs.
  if (!startWakeWord()) {
    logf("continuing with the button only");
  }

  setStatus(STATUS_IDLE);
  logf("ready - say \"Hey Nova\", or hold GPIO%d to talk", PIN_BUTTON);
  emitEvent("state", "state", "ready");
}

void loop() {
  gButtonHeld = buttonPressedStable();

  if (gState == ST_CAPTURING) {
    setStatus(STATUS_RECORDING);
  }

  if (gState == ST_PENDING) {
    gState = ST_UPLOADING;

    size_t samples = gSamples;
    size_t pcmBytes = samples * 2;
    float seconds = samples / (float)SAMPLE_RATE;

    // The pre-roll is replayed padding, not audio the user actually gave us,
    // so the minimum-length check has to discount it or a 20 ms button tap
    // sails through as a "300 ms" take.
    size_t liveSamples = samples > gPrerollSamples ? samples - gPrerollSamples : 0;
    uint32_t liveMs = (uint32_t)((liveSamples * 1000) / SAMPLE_RATE);

    logf("captured %.2f s (%s, %lu ms live), peak %d",
         seconds, gEndReason, (unsigned long)liveMs, (int)gPeak);

    if (samples == 0 || liveMs < MIN_RECORD_MS) {
      logf("too short, ignoring");
    } else if (gPeak < SPEECH_MIN_PEAK) {
      // Almost certainly a false wake on room noise. Don't spend an API call.
      logf("no speech (peak %d < %d), skipping upload", (int)gPeak, (int)SPEECH_MIN_PEAK);
      emitEvent("noise", "reason", "below speech threshold");
    } else {
      normalise();
      writeWavHeader(gBuffer, pcmBytes);

      setStatus(STATUS_WORKING);
      emitEvent("state", "state", "transcribing");

      bool ok = transcribe(pcmBytes);
      setStatus(ok ? STATUS_OK : STATUS_ERROR);
      delay(400);
    }

    // Reset the detector's context so the take we just sent cannot re-trigger.
    gWakeRequested = false;
    ringReset();
    gState = ST_LISTENING;
    sr_set_mode(SR_MODE_COMMAND);  // make sure the detector is armed again
    setStatus(STATUS_IDLE);
    emitEvent("state", "state", "ready");
  }

  // Keep WiFi alive. Audio is handled entirely by the SR feed task now, so
  // loop() has nothing to do between takes.
  static uint32_t lastWifiCheck = 0;
  if (millis() - lastWifiCheck > 10000) {
    lastWifiCheck = millis();
    if (WiFi.status() != WL_CONNECTED) {
      logf("wifi dropped, reconnecting");
      WiFi.reconnect();
    }
  }
  delay(10);
}
