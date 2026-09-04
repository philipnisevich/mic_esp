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
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

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

// MAX98357A I2S amplifier (output). Uses the S3's second I2S controller, so it
// runs alongside the microphone rather than competing with it.
//   VIN -> 5V, GND -> GND, GAIN and SD left floating
// One-shot diagnostic: sweep every pin permutation at boot. Set to 0 for normal use.
#define AMP_PIN_SWEEP 0

// The MAX98357A supports LRCLK of 8/16/32/44.1/48/88.2/96 kHz ONLY. Its
// datasheet explicitly excludes 24 kHz - which is exactly what OpenAI's pcm
// format returns, and why the amp sat silent through every other experiment.
// Play at 48 kHz instead and duplicate each source sample.
static const uint32_t SPEAKER_SAMPLE_RATE = 48000;

static const int PIN_AMP_BCLK = 10;
static const int PIN_AMP_LRC  = 11;
static const int PIN_AMP_DIN  = 12;

// SSD1306 OLED over I2C. These are the S3's default Wire pins.
//   VCC -> 3V3, GND -> GND, SCL -> GPIO9, SDA -> GPIO8
static const int PIN_I2C_SDA = 8;
static const int PIN_I2C_SCL = 9;

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
static I2SClass I2S;       // microphone, RX
static I2SClass Speaker;   // MAX98357A, TX
static bool     gSpeakerReady = false;
static uint8_t *gTtsBuf = nullptr;
static size_t   gTtsCap = 0;

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

// --------------------------------------------------------------- display ---
static Adafruit_SSD1306 gOled(OLED_WIDTH, OLED_HEIGHT, &Wire, -1);
static bool gOledReady = false;

// Wrapped text is paged rather than scrolled: simpler, and easier to read on a
// screen this small. 6x8 font -> 21 columns, 8 rows, one row spent on the header.
static const uint8_t OLED_COLS  = OLED_WIDTH / 6;
static const uint8_t OLED_ROWS  = OLED_HEIGHT / 8;
static const uint8_t BODY_ROWS  = OLED_ROWS - 1;
static const uint8_t MAX_LINES  = BODY_ROWS * 4;  // cap at four pages

static char     gLines[MAX_LINES][OLED_WIDTH / 6 + 1];
static uint8_t  gLineCount = 0;
static uint8_t  gPage = 0;
static uint32_t gPageShownMs = 0;
static char     gHeader[24] = "";
static const uint32_t PAGE_DWELL_MS = 3500;

// The GFX default font is CP437, so UTF-8 multi-byte sequences render as pairs
// of garbage glyphs. Search answers are full of them - degree signs, curly
// quotes, en-dashes - so fold the common ones to ASCII and drop the rest.
static void asciiFold(const char *src, char *dst, size_t dstLen) {
  size_t o = 0;
  const uint8_t *p = (const uint8_t *)src;
  while (*p && o + 1 < dstLen) {
    if (*p < 0x80) {
      // The prompt asks for plain text and the models emit markdown anyway, so
      // drop the markers rather than draw "**$315.45**" literally.
      if (*p == '*' || *p == '`') { p++; continue; }
      if (*p == '#' && (o == 0 || dst[o - 1] == '\n')) { p++; continue; }
      dst[o++] = (char)*p++;
      continue;
    }

    // Decode just enough of the code point to recognise the ones we care about.
    uint32_t cp = 0;
    int len = 1;
    if ((*p & 0xE0) == 0xC0) { cp = *p & 0x1F; len = 2; }
    else if ((*p & 0xF0) == 0xE0) { cp = *p & 0x0F; len = 3; }
    else if ((*p & 0xF8) == 0xF0) { cp = *p & 0x07; len = 4; }
    for (int i = 1; i < len; i++) {
      if ((p[i] & 0xC0) != 0x80) { len = i; break; }
      cp = (cp << 6) | (p[i] & 0x3F);
    }
    p += len;

    const char *sub = nullptr;
    switch (cp) {
      case 0x00B0: sub = "deg"; break;                    // degree sign
      case 0x2018: case 0x2019: sub = "'"; break;         // curly single quotes
      case 0x201C: case 0x201D: sub = "\""; break;        // curly double quotes
      case 0x2013: case 0x2014: sub = "-"; break;         // en/em dash
      case 0x2212: sub = "-"; break;                      // true minus: dropping
                                                          // this turns -0.44% into
                                                          // 0.44%, silently
                                                          // inverting the meaning
      case 0x00D7: sub = "x"; break;                      // multiplication sign
      case 0x2022: sub = "-"; break;                      // bullet
      case 0x2026: sub = "..."; break;                    // ellipsis
      case 0x00A0: sub = " "; break;                      // non-breaking space
      case 0x00BD: sub = "1/2"; break;
      default: break;                                     // anything else: drop
    }
    if (sub) {
      while (*sub && o + 1 < dstLen) dst[o++] = *sub++;
    }
  }
  dst[o] = '\0';
}

// Greedy word wrap into gLines. Words longer than a line get hard-split.
static void wrapText(const char *text) {
  gLineCount = 0;
  if (!text) return;

  size_t col = 0;
  gLines[0][0] = '\0';

  while (*text && gLineCount < MAX_LINES) {
    while (*text == ' ' || *text == '\n' || *text == '\r' || *text == '\t') text++;
    if (!*text) break;

    const char *wordEnd = text;
    while (*wordEnd && *wordEnd != ' ' && *wordEnd != '\n' && *wordEnd != '\r' && *wordEnd != '\t') {
      wordEnd++;
    }
    size_t wordLen = (size_t)(wordEnd - text);

    if (wordLen > OLED_COLS) {  // no wrap point: hard-split it
      while (wordLen && gLineCount < MAX_LINES) {
        size_t room = OLED_COLS - col;
        size_t take = wordLen < room ? wordLen : room;
        memcpy(gLines[gLineCount] + col, text, take);
        col += take;
        gLines[gLineCount][col] = '\0';
        text += take;
        wordLen -= take;
        if (col >= OLED_COLS) {
          gLineCount++;
          col = 0;
          if (gLineCount < MAX_LINES) gLines[gLineCount][0] = '\0';
        }
      }
      continue;
    }

    if (col > 0 && col + 1 + wordLen > OLED_COLS) {
      gLineCount++;
      col = 0;
      if (gLineCount >= MAX_LINES) break;
      gLines[gLineCount][0] = '\0';
    }
    if (col > 0) gLines[gLineCount][col++] = ' ';
    memcpy(gLines[gLineCount] + col, text, wordLen);
    col += wordLen;
    gLines[gLineCount][col] = '\0';
    text = wordEnd;
  }

  if (col > 0 && gLineCount < MAX_LINES) gLineCount++;
}

static void oledRenderPage() {
  if (!gOledReady) return;
  gOled.clearDisplay();

  // Header bar, inverted so state is readable at a glance.
  gOled.fillRect(0, 0, OLED_WIDTH, 8, SSD1306_WHITE);
  gOled.setTextColor(SSD1306_BLACK);
  gOled.setCursor(0, 0);
  gOled.print(gHeader);

  uint8_t pages = gLineCount ? (uint8_t)((gLineCount + BODY_ROWS - 1) / BODY_ROWS) : 1;
  if (pages > 1) {
    char tag[8];
    snprintf(tag, sizeof(tag), "%u/%u", (unsigned)(gPage + 1), (unsigned)pages);
    gOled.setCursor(OLED_WIDTH - (int)strlen(tag) * 6, 0);
    gOled.print(tag);
  }

  gOled.setTextColor(SSD1306_WHITE);
  uint8_t first = gPage * BODY_ROWS;
  for (uint8_t i = 0; i < BODY_ROWS && (first + i) < gLineCount; i++) {
    gOled.setCursor(0, 8 + i * 8);
    gOled.print(gLines[first + i]);
  }
  gOled.display();
}

static void oledShow(const char *header, const char *body) {
  snprintf(gHeader, sizeof(gHeader), "%s", header ? header : "");
  static char folded[512];
  asciiFold(body ? body : "", folded, sizeof(folded));
  wrapText(folded);
  gPage = 0;
  gPageShownMs = millis();
  oledRenderPage();
}

// Advance long answers a page at a time so everything eventually gets read.
static void oledTick() {
  if (!gOledReady || gLineCount <= BODY_ROWS) return;
  if (millis() - gPageShownMs < PAGE_DWELL_MS) return;
  uint8_t pages = (uint8_t)((gLineCount + BODY_ROWS - 1) / BODY_ROWS);
  gPage = (uint8_t)((gPage + 1) % pages);
  gPageShownMs = millis();
  oledRenderPage();
}

static bool oledBegin() {
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  // 0x3C is standard; a minority of modules are strapped to 0x3D.
  for (uint8_t addr = 0x3C; addr <= 0x3D; addr++) {
    if (gOled.begin(SSD1306_SWITCHCAPVCC, addr)) {
      gOledReady = true;
      gOled.setTextSize(1);
      gOled.setTextWrap(false);
      logf("oled ok at 0x%02X on sda=%d scl=%d", addr, PIN_I2C_SDA, PIN_I2C_SCL);
      return true;
    }
  }
  logf("no OLED found at 0x3C/0x3D - check SDA=%d SCL=%d and 3V3", PIN_I2C_SDA, PIN_I2C_SCL);
  return false;
}

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
// The core generates phonemes on-device (flite_g2p) from plain text, so
// sr_cmd_t is just {command_id, phrase}. All three map to command 0 - any of
// them starts a take. Bare "Nova" is deliberately left out: one short
// syllable false-accepts constantly.
static const sr_cmd_t SR_COMMANDS[] = {
  {0, "Hey Nova"},
  {0, "Hi Nova"},
  {0, "Okay Nova"},
  // MultiNet7's FST builder panics (StoreProhibited, index -1) when the list
  // resolves to a single command id. Every working example in the core ships
  // several, so keep at least one more distinct id present. These are ignored
  // by srEvent(); they exist to keep the grammar well-formed.
  {1, "Cancel that"},
  {2, "Never mind"},
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
      if (command_id == 0 && gState == ST_LISTENING) {
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
static bool transcribe(size_t pcmBytes, String &outText) {
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
    outText = "";
    return true;
  }

  outText = text;
  emitTranscript(text, lang);
  return true;
}

// -------------------------------------------------------------- routing ----
// Anything touching live facts needs the search-backed model; everything else
// gets the fast cheap one. Deliberately a keyword test rather than asking a
// model to classify: this runs in microseconds on-device, where a routing
// round trip would cost more than the answer itself.
//
// Phrases are specific on purpose. Bare "current", "hours" and "who is the"
// were tried first and misrouted "current limit an LED", "how many hours in a
// day" and "who is the author of Hamlet" - each of which would have cost ~450x
// the tokens of a correct route. Prefer missing a search over taking one:
// "search ..." and "look up ..." are always available as an override.
static const char *RESEARCH_KEYWORDS[] = {
  "weather", "forecast", "temperature", "raining", "snowing",
  "news", "headline", "happening now", "breaking news",
  "today", "tonight", "tomorrow", "yesterday", "this week", "this month",
  "latest", "currently", "right now", "recently", "just now",
  "who won", "final score", "score of", "standings", "who is playing", "who's playing",
  "stock price", "share price", "how much does", "cost of",
  "open now", "still open", "what time does", "schedule for", "released",
  "election", "president of", "prime minister", "live score", "live stream",
  "play next", "playing next", "next game", "next match", "next fixture",
  "kick off", "kickoff",
};

// A domain almost always means "go look this up". The fast model will happily
// invent a description of a site it has never seen.
static const char *RESEARCH_TLDS[] = {
  ".com", ".io", ".org", ".net", ".ai", ".dev", ".gov", ".edu",
};

static bool needsResearch(const String &question) {
  String lower = question;
  lower.toLowerCase();

  // Explicit override, so you are never stuck when the heuristic misses.
  if (lower.startsWith("search") || lower.startsWith("look up")) return true;

  for (size_t i = 0; i < sizeof(RESEARCH_KEYWORDS) / sizeof(RESEARCH_KEYWORDS[0]); i++) {
    if (lower.indexOf(RESEARCH_KEYWORDS[i]) >= 0) return true;
  }

  for (size_t i = 0; i < sizeof(RESEARCH_TLDS) / sizeof(RESEARCH_TLDS[0]); i++) {
    if (lower.indexOf(RESEARCH_TLDS[i]) >= 0) return true;
  }

  // A 20xx year almost always means "look this up".
  for (int i = 0; i + 3 < (int)lower.length(); i++) {
    if (lower[i] == '2' && lower[i + 1] == '0' &&
        isdigit((unsigned char)lower[i + 2]) && isdigit((unsigned char)lower[i + 3])) {
      return true;
    }
  }
  return false;
}

// The 300 ms pre-roll regularly catches the tail of the wake phrase, so
// transcripts arrive as "Hey Nova, what's the weather". Strip it here rather
// than shrinking the pre-roll, which would just start clipping real speech.
static void stripWakePhrase(String &question) {
  static const char *PREFIXES[] = {"hey nova", "hi nova", "okay nova", "ok nova", "nova"};
  String lower = question;
  lower.toLowerCase();

  for (size_t i = 0; i < sizeof(PREFIXES) / sizeof(PREFIXES[0]); i++) {
    size_t len = strlen(PREFIXES[i]);
    if (!lower.startsWith(PREFIXES[i])) continue;
    // Require a word boundary, or "nova" would eat the start of "Novak".
    if (question.length() > len && isalnum((unsigned char)question[len])) continue;

    size_t cut = len;
    while (cut < question.length() &&
           (question[cut] == ',' || question[cut] == '.' || question[cut] == '!' ||
            question[cut] == '?' || question[cut] == ' ')) {
      cut++;
    }
    if (cut >= question.length()) return;  // nothing but the wake phrase
    question = question.substring(cut);
    question.trim();
    return;
  }
}

// The fast model refuses live-data questions in very consistent language. Treat
// that as a routing miss and retry on the search path: it costs an extra call
// only when the keywords missed, and turns a dead end into an answer.
// Both a negation and a liveness reference are required, so an answer that
// merely discusses "real-time systems" is not mistaken for a refusal.
static bool isRefusal(const String &answer) {
  static const char *NEGATIONS[] = {
    "i can't", "i cannot", "i don't have", "i do not have",
    "i'm unable", "i am unable", "not able to", "sorry,",
  };
  static const char *LIVENESS[] = {
    "real-time", "real time", "current data", "live data", "up-to-date",
    "up to date", "browse", "internet access", "knowledge cutoff",
    "as of my last", "current information", "latest information",
  };

  String lower = answer;
  lower.toLowerCase();

  bool negated = false;
  for (size_t i = 0; i < sizeof(NEGATIONS) / sizeof(NEGATIONS[0]); i++) {
    if (lower.indexOf(NEGATIONS[i]) >= 0) { negated = true; break; }
  }
  if (!negated) return false;

  for (size_t i = 0; i < sizeof(LIVENESS) / sizeof(LIVENESS[0]); i++) {
    if (lower.indexOf(LIVENESS[i]) >= 0) return true;
  }
  return false;
}

// The search model appends citations like "([skysports.com](https://...))"
// however firmly the prompt forbids them. They wreck a 128x64 screen and, far
// worse, get read aloud as a URL. Reduce "[label](url)" to "label" and drop
// bare URLs, before the answer reaches either the display or the speaker.
static void stripLinks(String &text) {
  String out;
  out.reserve(text.length());

  for (int i = 0; i < (int)text.length();) {
    if (text[i] == '[') {
      int close = text.indexOf(']', i);
      int open = (close >= 0 && close + 1 < (int)text.length() && text[close + 1] == '(')
                   ? close + 1 : -1;
      if (open > 0) {
        int end = text.indexOf(')', open);
        if (end > 0) {
          out += text.substring(i + 1, close);  // keep the label
          i = end + 1;
          continue;
        }
      }
    }
    if (text.startsWith("http://", i) || text.startsWith("https://", i)) {
      while (i < (int)text.length() && text[i] != ' ' && text[i] != ')' && text[i] != '\n') i++;
      continue;
    }
    out += text[i++];
  }

  // Citations usually leave "(...)" or " ." behind once the link is gone.
  out.replace("()", "");
  out.replace(" .", ".");
  out.replace("..", ".");
  out.trim();
  text = out;
}

static void emitAnswer(const char *text, const char *model, bool researched) {
  JsonDocument doc;
  doc["type"] = "answer";
  doc["text"] = text;
  doc["model"] = model;
  doc["research"] = researched;
  serializeJson(doc, Serial);
  Serial.println();
}

// --------------------------------------------------------------- openai ----
// Send the transcript on to a chat model and hand back its reply. Deliberately
// minimal: only model + messages, because optional fields like the token cap
// have different names across model families and sending the wrong one is a
// 400 rather than a graceful degrade. Reply length is steered by the system
// prompt instead.
static bool askOpenAI(const char *question, String &answer, bool research) {
  if (WiFi.status() != WL_CONNECTED) {
    emitEvent("error", "error", "wifi not connected");
    return false;
  }

  const char *model  = research ? OPENAI_SEARCH_MODEL : OPENAI_MODEL;
  const char *sysMsg = research ? OPENAI_SEARCH_SYSTEM_PROMPT : OPENAI_SYSTEM_PROMPT;

  JsonDocument req;
  req["model"] = model;
#ifdef OPENAI_MAX_COMPLETION_TOKENS
  // The search model needs room for its results, so only cap the fast path.
  if (!research) req["max_completion_tokens"] = OPENAI_MAX_COMPLETION_TOKENS;
#endif
  JsonArray messages = req["messages"].to<JsonArray>();
  JsonObject sys = messages.add<JsonObject>();
  sys["role"] = "system";
  sys["content"] = sysMsg;
  JsonObject usr = messages.add<JsonObject>();
  usr["role"] = "user";
  usr["content"] = question;

  String body;
  serializeJson(req, body);

  NetworkClientSecure client;
#ifdef OPENAI_ROOT_CA
  client.setCACert(OPENAI_ROOT_CA);
#else
  client.setInsecure();
#endif
  client.setHandshakeTimeout(15);

  HTTPClient http;
  http.setConnectTimeout(10000);
  http.setTimeout(45000);
  http.setReuse(false);

  if (!http.begin(client, "https://api.openai.com/v1/chat/completions")) {
    emitEvent("error", "error", "openai http begin failed");
    return false;
  }
  http.addHeader("Authorization", String("Bearer ") + OPENAI_API_KEY);
  http.addHeader("Content-Type", "application/json");

  logf("asking %s%s...", model, research ? " (search)" : "");
  uint32_t t0 = millis();
  int code = http.POST(body);

  if (code <= 0) {
    String err = HTTPClient::errorToString(code);
    logf("openai transport error: %s", err.c_str());
    http.end();
    logNetworkDiagnostics();
    emitEvent("error", "error", err.c_str());
    return false;
  }

  String payload = http.getString();
  http.end();
  logf("openai http %d in %lu ms", code, (unsigned long)(millis() - t0));

  // Only pull out the two fields we care about - the full response carries a
  // lot of metadata we would otherwise have to find heap for.
  JsonDocument filter;
  filter["choices"][0]["message"]["content"] = true;
  filter["error"]["message"] = true;

  JsonDocument doc;
  DeserializationError jsonErr = deserializeJson(doc, payload, DeserializationOption::Filter(filter));
  if (jsonErr) {
    emitEvent("error", "error", jsonErr.c_str());
    return false;
  }

  if (code != HTTP_CODE_OK) {
    const char *msg = doc["error"]["message"] | "";
    String detail = String("openai ") + code + ": " + (msg[0] ? msg : payload.substring(0, 160).c_str());
    logf("%s", detail.c_str());
    emitEvent("error", "error", detail.c_str());
    return false;
  }

  const char *content = doc["choices"][0]["message"]["content"] | "";
  if (!content[0]) {
    emitEvent("error", "error", "openai returned an empty reply");
    return false;
  }

  answer = content;
  answer.trim();
  return true;
}

// ------------------------------------------------------------------ tts ----
#if TTS_ENABLED
static bool speakerBegin() {
  if (gSpeakerReady) return true;  // idempotent: a second call would orphan gTtsBuf

  // Pin the amplifier to its own I2S controller. I2SClass defaults to
  // I2S_NUM_AUTO, and since the microphone's RX channel is allocated first on
  // I2S0, a TX channel would be placed on I2S0 as well - where it shares clock
  // hardware with the 16 kHz microphone rather than running at our rate.
  if (!Speaker.setPort(I2S_NUM_1)) {
    logf("could not select I2S port 1 for the speaker");
  }
  Speaker.setPins(PIN_AMP_BCLK, PIN_AMP_LRC, PIN_AMP_DIN, -1 /* no din */, -1);
  // Stereo with both slots carrying the same sample. The MAX98357A averages
  // L and R when SD is floating, so sending true mono would halve the level.
  // 32-bit slots, not 16. The audio is 16-bit, but the slot width sets the bit
  // clock: 16-bit stereo gives BCLK = 32 x LRCLK, and plenty of MAX98357A parts
  // fail to lock their clock recovery at 32fs and then output silence with no
  // other symptom. 32-bit slots give the 64fs that working examples all use;
  // samples are left-justified into the top half, which the amp ignores below
  // its resolution anyway.
  if (!Speaker.begin(I2S_MODE_STD, SPEAKER_SAMPLE_RATE, I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO)) {
    logf("speaker I2S init failed - is another peripheral using bclk=%d ws=%d?",
         PIN_AMP_BCLK, PIN_AMP_LRC);
    return false;
  }
  gTtsCap = (size_t)TTS_SAMPLE_RATE * 2 * TTS_MAX_SECONDS;
  gTtsBuf = (uint8_t *)ps_malloc(gTtsCap);
  if (!gTtsBuf) {
    gTtsCap = 0;
    logf("no PSRAM for the tts buffer");
    return false;
  }
  gSpeakerReady = true;
  logf("speaker on I2S port %d (mic is on port %d)", (int)Speaker.getPort(), (int)I2S.getPort());
  logf("speaker ok: bclk=%d lrc=%d din=%d @ %lu Hz, %u KB buffer",
       PIN_AMP_BCLK, PIN_AMP_LRC, PIN_AMP_DIN, (unsigned long)SPEAKER_SAMPLE_RATE,
       (unsigned)(gTtsCap / 1024));
  return true;
}

// HTTPClient::writeToStream() handles both content-length and chunked bodies
// and stops exactly at the end. Reading getStreamPtr() directly does neither,
// which is how a 238 KB clip turned into 714 KB of over-read.
class BufferSink : public Stream {
public:
  BufferSink(uint8_t *buf, size_t cap) : _buf(buf), _cap(cap) {}
  size_t write(uint8_t b) override {
    if (_len >= _cap) return 0;
    _buf[_len++] = b;
    return 1;
  }
  size_t write(const uint8_t *data, size_t len) override {
    size_t n = len;
    if (_len + n > _cap) n = _cap - _len;
    if (n) memcpy(_buf + _len, data, n);
    _len += n;
    return n;
  }
  size_t length() const { return _len; }
  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  void flush() override {}
private:
  uint8_t *_buf;
  size_t _cap;
  size_t _len = 0;
};

static void playPcm(const uint8_t *pcm, size_t bytes) {
  const int16_t *src = (const int16_t *)pcm;
  size_t total = bytes / sizeof(int16_t);
  static int32_t frame[256 * 2];

  size_t i = 0;
  while (i < total) {
    size_t n = 0;
    while (n < 254 && i < total) {
      int32_t v = (int32_t)(src[i++] * TTS_VOLUME);
      if (v > 32767) v = 32767;
      if (v < -32768) v = -32768;
      // Each 24 kHz sample becomes two 48 kHz frames, both slots identical.
      frame[n * 2] = v << 16;
      frame[n * 2 + 1] = v << 16;
      n++;
      frame[n * 2] = v << 16;
      frame[n * 2 + 1] = v << 16;
      n++;
    }
    // Blocking write: the DMA queue paces this loop for us.
    Speaker.write((uint8_t *)frame, n * 2 * sizeof(int32_t));
  }
}

// A plain sine straight to the amp. Independent of WiFi, OpenAI and the whole
// TTS path, so if this is silent the problem is wiring, power or the SD pin.
static void playTone(uint16_t freq, uint16_t ms) {
  if (!gSpeakerReady) return;
  const size_t total = (size_t)SPEAKER_SAMPLE_RATE * ms / 1000;
  static int32_t frame[256 * 2];
  size_t i = 0;
  while (i < total) {
    size_t c = 0;
    while (c < 256 && i < total) {
      float t = (float)i / (float)SPEAKER_SAMPLE_RATE;
      int32_t v = (int32_t)(sinf(2.0f * PI * freq * t) * 26000.0f * TTS_VOLUME);
      if (v > 32767) v = 32767;
      if (v < -32768) v = -32768;
      frame[c * 2] = v << 16;      // left-justify into the 32-bit slot
      frame[c * 2 + 1] = v << 16;
      c++;
      i++;
    }
    Speaker.write((uint8_t *)frame, c * 2 * sizeof(int32_t));
  }
}

// Fetch the whole clip into PSRAM before playing. Streaming would start sooner
// but risks underruns mid-sentence if WiFi hiccups, and a glitch is far more
// noticeable than a second of latency.
static bool speak(const char *text) {
  if (!gSpeakerReady || !text || !*text) return false;
  if (WiFi.status() != WL_CONNECTED) return false;

  JsonDocument req;
  req["model"] = TTS_MODEL;
  req["voice"] = TTS_VOICE;
  req["input"] = text;
  req["response_format"] = "pcm";
  String body;
  serializeJson(req, body);

  NetworkClientSecure client;
  client.setInsecure();
  client.setHandshakeTimeout(15);

  HTTPClient http;
  http.setConnectTimeout(10000);
  http.setTimeout(30000);
  http.setReuse(false);
  if (!http.begin(client, "https://api.openai.com/v1/audio/speech")) return false;
  http.addHeader("Authorization", String("Bearer ") + OPENAI_API_KEY);
  http.addHeader("Content-Type", "application/json");

  uint32_t t0 = millis();
  int code = http.POST(body);
  if (code != HTTP_CODE_OK) {
    logf("tts http %d: %s", code, http.getString().substring(0, 120).c_str());
    http.end();
    return false;
  }

  BufferSink sink(gTtsBuf, gTtsCap);
  http.writeToStream(&sink);
  size_t got = sink.length();
  if (got < 2) {
    logf("tts returned no audio");
    return false;
  }
  logf("tts %u KB in %lu ms, playing %.1f s",
       (unsigned)(got / 1024), (unsigned long)(millis() - t0),
       got / 2.0f / TTS_SAMPLE_RATE);

  playPcm(gTtsBuf, got);
  return true;
}
#endif  // TTS_ENABLED

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
  oledShow("WiFi", WIFI_SSID);

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
  // Mono is "M" here: the format length must match the channel count, and
  // 3.3.11 honours it via get_feed_channel_num(). The old "MNN" worked around
  // 3.3.5 hardcoding SR_CHANNEL_NUM = 3, which is fixed now; leaving it would
  // make the AFE process two permanently silent channels.
  esp_err_t err = sr_start(
    srFill, nullptr, SR_CHANNELS_MONO, SR_MODE_COMMAND, "M",
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

  oledBegin();
  oledShow("MicScribe", "Booting...");

  if (!allocateBuffer()) {
    logf("FATAL: could not allocate the audio buffer");
    while (true) {
      setStatus(STATUS_ERROR);
      delay(1000);
    }
  }

  I2S.setPort(I2S_NUM_0);  // keep the split deterministic: mic on 0, speaker on 1
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

#if TTS_ENABLED
#if AMP_PIN_SWEEP
  speakerBegin();  // allocates the shared buffer before the sweep re-inits I2S
  // The datasheet says our clocking is legal and the wiring reads correct, so
  // the remaining candidate is a pin mismatch. Rather than guess, walk every
  // permutation of the three pins and play a distinct pitch on each. Whichever
  // beep is audible names the true wiring.
  {
    const int pins[3] = {PIN_AMP_BCLK, PIN_AMP_LRC, PIN_AMP_DIN};
    const int perm[6][3] = {{0,1,2},{0,2,1},{1,0,2},{1,2,0},{2,0,1},{2,1,0}};
    const uint16_t pitch[6] = {500, 700, 900, 1100, 1300, 1500};

    for (int i = 0; i < 6; i++) {
      int bclk = pins[perm[i][0]], lrc = pins[perm[i][1]], din = pins[perm[i][2]];
      Speaker.end();
      Speaker.setPins(bclk, lrc, din, -1, -1);
      if (!Speaker.begin(I2S_MODE_STD, SPEAKER_SAMPLE_RATE, I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO)) {
        logf("sweep %d: begin failed for bclk=%d lrc=%d din=%d", i + 1, bclk, lrc, din);
        continue;
      }
      gSpeakerReady = true;
      logf("sweep %d/6: %u Hz  bclk=%d lrc=%d din=%d", i + 1, (unsigned)pitch[i], bclk, lrc, din);
      playTone(pitch[i], 500);
      delay(400);
    }
    // Leave it on the configured pins for normal operation.
    Speaker.end();
    Speaker.setPins(PIN_AMP_BCLK, PIN_AMP_LRC, PIN_AMP_DIN, -1, -1);
    gSpeakerReady = Speaker.begin(I2S_MODE_STD, SPEAKER_SAMPLE_RATE, I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO);
    logf("sweep done - report which pitch you heard, if any");
  }
#else
  if (speakerBegin()) {
    uint32_t toneStart = millis();
    for (int i = 0; i < 3; i++) {
      playTone(1000, 400);
      delay(150);
    }
    logf("test tone done in %lu ms (expected ~1650 ms)",
         (unsigned long)(millis() - toneStart));
  }
#endif
#endif

  connectWiFi();

  // After WiFi, so the network stack claims its internal RAM before ESP-SR
  // takes what it needs.
  if (!startWakeWord()) {
    logf("continuing with the button only");
  }

  setStatus(STATUS_IDLE);
  oledShow("Ready", "Say \"Hey Nova\" or hold the button");
  logf("ready - say \"Hey Nova\", or hold GPIO%d to talk", PIN_BUTTON);
  emitEvent("state", "state", "ready");
}

void loop() {
  gButtonHeld = buttonPressedStable();

  // Edge-trigger the display so a finished answer stays on screen instead of
  // being redrawn (and reset to page 1) on every pass through loop().
  static uint8_t lastState = 255;
  uint8_t st = gState;
  if (st != lastState) {
    lastState = st;
    if (st == ST_CAPTURING) {
      setStatus(STATUS_RECORDING);
      oledShow("Listening", gManualCapture ? "Hold to talk..." : "Go ahead...");
    }
  }

  oledTick();

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
      oledShow("Transcribing", "...");
      emitEvent("state", "state", "transcribing");

      String question;
      bool ok = transcribe(pcmBytes, question);

      if (ok && question.length() > 0) {
        // Leave the question up while the model thinks - it doubles as
        // feedback that the transcription was right.
        stripWakePhrase(question);
        bool research = needsResearch(question);
        oledShow(research ? "Searching" : "You said", question.c_str());
        emitEvent("state", "state", research ? "searching" : "thinking");

        String answer;
        if (askOpenAI(question.c_str(), answer, research)) {
          // Backstop for anything the keyword router missed.
          if (!research && isRefusal(answer)) {
            logf("fast model refused - retrying on the search path");
            oledShow("Searching", question.c_str());
            emitEvent("state", "state", "searching");
            String retried;
            if (askOpenAI(question.c_str(), retried, true) && retried.length()) {
              answer = retried;
              research = true;
            }
          }
          stripLinks(answer);
          emitAnswer(answer.c_str(), research ? OPENAI_SEARCH_MODEL : OPENAI_MODEL, research);
          oledShow("Nova", answer.c_str());
          setStatus(STATUS_OK);
#if TTS_ENABLED
          // Still ST_UPLOADING here, so the detector ignores our own voice
          // coming back through the mic and cannot self-trigger.
          emitEvent("state", "state", "speaking");
          speak(answer.c_str());
#endif
        } else {
          oledShow("Error", "Could not reach OpenAI");
          setStatus(STATUS_ERROR);
        }
      } else {
        setStatus(ok ? STATUS_OK : STATUS_ERROR);
        if (!ok) oledShow("Error", "Transcription failed");
      }
      delay(400);
    }

    // Reset the detector's context so the take we just sent cannot re-trigger.
    gWakeRequested = false;
    ringReset();
    gState = ST_LISTENING;
    lastState = ST_LISTENING;      // do not repaint over the answer
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
