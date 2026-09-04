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
#include <Preferences.h>
#include <NetworkClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <ESP_I2S.h>
#include <ESP_SR.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <ArduinoWebsockets.h>
#include "mbedtls/base64.h"

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
static const uint32_t VAD_SILENCE_MS  = 500;   // quiet this long ends a take
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

// --------------------------------------------------------- wifi provision ---
// Runtime WiFi setup over the same serial port, so the network isn't fixed at
// flash time. Credentials live in NVS (via Preferences) and take priority over
// the WIFI_SSID/WIFI_PASS compile-time defaults in config.h, which now serve
// only as the value used on a completely fresh board.
//
// Host protocol (one JSON object per line, matching the output convention):
//   {"cmd":"wifi_scan"}                            -> triggers a scan
//   {"cmd":"wifi_connect","ssid":"...","pass":"..."} -> connects and persists
//   {"cmd":"wifi_status"}                          -> reports current status
//
// Emitted in response:
//   {"type":"wifi_scan_result","networks":[{"ssid":"...","rssi":-51,"secure":true}]}
//   {"type":"wifi_status","status":"connecting","ssid":"..."}
//   {"type":"wifi_status","status":"connected","ssid":"...","ip":"..."}
//   {"type":"wifi_status","status":"failed","ssid":"..."}
//   {"type":"wifi_status","status":"disconnected"}
static Preferences gWifiPrefs;
static bool gWifiScanRunning = false;

static void wifiPrefsLoad(String &ssid, String &pass) {
  gWifiPrefs.begin("wifi", true);
  ssid = gWifiPrefs.getString("ssid", "");
  pass = gWifiPrefs.getString("pass", "");
  gWifiPrefs.end();
}

static void wifiPrefsSave(const String &ssid, const String &pass) {
  gWifiPrefs.begin("wifi", false);
  gWifiPrefs.putString("ssid", ssid);
  gWifiPrefs.putString("pass", pass);
  gWifiPrefs.end();
}

static void emitWifiStatus(const char *status, const char *ssid = nullptr) {
  JsonDocument doc;
  doc["type"] = "wifi_status";
  doc["status"] = status;
  if (ssid) doc["ssid"] = ssid;
  if (strcmp(status, "connected") == 0) doc["ip"] = WiFi.localIP().toString();
  serializeJson(doc, Serial);
  Serial.println();
}

// Async, so a scan (2-4 s) never blocks audio capture or the OLED. loop()
// polls wifiPollScan() for the result.
static void wifiStartScan() {
  if (gWifiScanRunning) return;
  WiFi.scanDelete();
  int16_t r = WiFi.scanNetworks(/*async=*/true, /*show_hidden=*/false);
  gWifiScanRunning = (r == WIFI_SCAN_RUNNING);
  logf("wifi scan started");
}

static void wifiPollScan() {
  if (!gWifiScanRunning) return;
  int16_t n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING) return;
  gWifiScanRunning = false;
  if (n < 0) {
    logf("wifi scan failed");
    return;
  }

  // Collapse duplicate SSIDs (multiple APs on one network) down to the
  // strongest signal, and cap the list at something a small screen can show.
  struct Net { String ssid; int32_t rssi; bool secure; };
  static const int MAX_NETS = 40;
  Net nets[MAX_NETS];
  int count = 0;
  for (int i = 0; i < n && count < MAX_NETS; i++) {
    String ssid = WiFi.SSID(i);
    if (ssid.length() == 0) continue;
    int32_t rssi = WiFi.RSSI(i);
    bool secure = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
    int existing = -1;
    for (int j = 0; j < count; j++) {
      if (nets[j].ssid == ssid) { existing = j; break; }
    }
    if (existing >= 0) {
      if (rssi > nets[existing].rssi) nets[existing].rssi = rssi;
    } else {
      nets[count++] = Net{ ssid, rssi, secure };
    }
  }
  for (int i = 1; i < count; i++) {
    Net key = nets[i];
    int j = i - 1;
    while (j >= 0 && nets[j].rssi < key.rssi) { nets[j + 1] = nets[j]; j--; }
    nets[j + 1] = key;
  }

  JsonDocument doc;
  doc["type"] = "wifi_scan_result";
  JsonArray arr = doc["networks"].to<JsonArray>();
  for (int i = 0; i < count; i++) {
    JsonObject o = arr.add<JsonObject>();
    o["ssid"] = nets[i].ssid;
    o["rssi"] = nets[i].rssi;
    o["secure"] = nets[i].secure;
  }
  serializeJson(doc, Serial);
  Serial.println();
  WiFi.scanDelete();
  logf("wifi scan: %d network(s)", count);
}

// Synchronous: a deliberate user action from the provisioning page, so a
// few-second block is fine. Saves to NVS only on success, so a typo'd
// password never overwrites a working set of credentials.
static void wifiConnectAndSave(const String &ssid, const String &pass) {
  logf("wifi: connecting to %s (provisioning)", ssid.c_str());
  emitWifiStatus("connecting", ssid.c_str());
  oledShow("WiFi Setup", ssid.c_str());

  WiFi.disconnect(true);
  delay(100);
  WiFi.begin(ssid.c_str(), pass.c_str());

  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    delay(250);
  }

  if (WiFi.status() == WL_CONNECTED) {
    wifiPrefsSave(ssid, pass);
    logf("wifi ok, ip %s", WiFi.localIP().toString().c_str());
    emitWifiStatus("connected", ssid.c_str());
    oledShow("WiFi Setup", "Connected");
  } else {
    logf("wifi FAILED to connect to %s", ssid.c_str());
    emitWifiStatus("failed", ssid.c_str());
    oledShow("WiFi Setup", "Failed - retry");
  }
}

static void handleWifiCommand(JsonDocument &doc, const char *cmd) {
  if (strcmp(cmd, "wifi_scan") == 0) {
    wifiStartScan();
  } else if (strcmp(cmd, "wifi_connect") == 0) {
    String ssid = doc["ssid"] | "";
    String pass = doc["pass"] | "";
    if (ssid.length() > 0) wifiConnectAndSave(ssid, pass);
  } else if (strcmp(cmd, "wifi_status") == 0) {
    if (WiFi.status() == WL_CONNECTED) {
      emitWifiStatus("connected", WiFi.SSID().c_str());
    } else {
      emitWifiStatus("disconnected");
    }
  }
}

// One JSON object per line in from the host, mirroring what goes out. Fed a
// byte at a time from loop() so a slow/partial write never stalls anything.
static void pollSerialCommands() {
  static String lineBuf;
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n') {
      lineBuf.trim();
      if (lineBuf.length() > 0) {
        JsonDocument doc;
        if (!deserializeJson(doc, lineBuf)) {
          const char *cmd = doc["cmd"] | "";
          if (*cmd) handleWifiCommand(doc, cmd);
        }
      }
      lineBuf = "";
    } else if (c != '\r') {
      lineBuf += c;
      if (lineBuf.length() > 512) lineBuf = "";  // guard against line noise
    }
  }
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
#if REALTIME_ENABLED
  // The realtime session reads I2S itself, so this callback only has to keep
  // feeding the wake-word detector. No capture buffers, no VAD, no pre-roll.
  (void)pcm16;
  (void)level;
  return;
#else
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
#endif  // REALTIME_ENABLED
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

// Everything here is read aloud, and the models emit symbols however firmly the
// prompt forbids them - we have already seen markdown and citations come back.
// So rewrite the text into words before it reaches the speech API rather than
// trusting the instruction.
static void speechSanitize(String &text) {
  // Parenthetical asides are almost always a restatement ("72F (22C)") and add
  // nothing when spoken. Drop the whole span, nested included.
  String stripped;
  stripped.reserve(text.length());
  int depth = 0;
  for (int i = 0; i < (int)text.length(); i++) {
    char c = text[i];
    if (c == '(' || c == '[' || c == '{') { depth++; continue; }
    if (c == ')' || c == ']' || c == '}') { if (depth > 0) depth--; continue; }
    if (depth == 0) stripped += c;
  }

  // Smart punctuation first: these are multi-byte UTF-8, and the character
  // filter below would otherwise turn each byte into a space, so "It's" would
  // be spoken as "It s".
  stripped.replace("\u2019", "'");
  stripped.replace("\u2018", "'");
  stripped.replace("\u201C", "");
  stripped.replace("\u201D", "");
  stripped.replace("\u2026", ".");

  // Units and signs that follow their number read correctly as suffixes.
  stripped.replace("\u00B0C", " degrees celsius");
  stripped.replace("\u00B0F", " degrees fahrenheit");
  stripped.replace("\u00B0", " degrees");
  stripped.replace("%", " percent");
  stripped.replace("&", " and ");
  stripped.replace("+", " plus ");
  stripped.replace("=", " equals ");
  stripped.replace("#", " number ");
  stripped.replace("@", " at ");
  stripped.replace("/", " ");
  stripped.replace("\u2013", " to ");
  stripped.replace("\u2014", ", ");

  // Currency is the exception: the sign precedes its number, so move it after.
  String out;
  out.reserve(stripped.length() + 16);
  for (int i = 0; i < (int)stripped.length(); i++) {
    char c = stripped[i];
    if (c == '$') {
      int j = i + 1;
      String number;
      while (j < (int)stripped.length() &&
             (isdigit((unsigned char)stripped[j]) || stripped[j] == '.' || stripped[j] == ',')) {
        number += stripped[j++];
      }
      if (number.length()) {
        out += number;
        out += " dollars";
        i = j - 1;
      }
      continue;  // a bare $ with no number is simply dropped
    }
    // Keep letters, digits and the punctuation that shapes speech; drop the rest.
    if (isalnum((unsigned char)c) || c == ' ' || c == '.' || c == ',' ||
        c == '\'' || c == '?' || c == '!' || c == '-' || c == ':') {
      out += c;
    } else {
      out += ' ';
    }
  }

  // Collapse the whitespace all that substitution leaves behind.
  String tidy;
  tidy.reserve(out.length());
  bool lastSpace = false;
  for (int i = 0; i < (int)out.length(); i++) {
    char c = out[i];
    if (c == ' ') {
      if (!lastSpace && tidy.length()) tidy += c;
      lastSpace = true;
    } else {
      if (lastSpace && (c == '.' || c == ',' || c == '?' || c == '!' || c == ':') && tidy.length()) {
        tidy.remove(tidy.length() - 1);  // no space before punctuation
      }
      tidy += c;
      lastSpace = false;
    }
  }
  tidy.trim();
  text = tidy;
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
#if REALTIME_ENABLED
  // The one-shot TTS path is unused here; 1.17 MB of PSRAM is better spent on
  // the receive ring.
  gTtsCap = 0;
  gTtsBuf = nullptr;
#else
  gTtsCap = (size_t)TTS_SAMPLE_RATE * 2 * TTS_MAX_SECONDS;
  gTtsBuf = (uint8_t *)ps_malloc(gTtsCap);
#endif
#if !REALTIME_ENABLED
  if (!gTtsBuf) {
    gTtsCap = 0;
    logf("no PSRAM for the tts buffer");
    return false;
  }
#endif
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

static void playPcm(const uint8_t *pcm, size_t bytes, float gain) {
  const int16_t *src = (const int16_t *)pcm;
  size_t total = bytes / sizeof(int16_t);
  static int32_t frame[256 * 2];

  size_t i = 0;
  while (i < total) {
    size_t n = 0;
    while (n < 254 && i < total) {
      // 24 -> 48 kHz. Duplicating each sample (zero-order hold) is cheap but
      // leaves a full-amplitude image around the Nyquist point, which is what
      // makes it sound gritty. Averaging with the next sample is a 2-tap FIR
      // that puts a null right there - far cleaner for one extra add.
      int32_t cur = (int32_t)(src[i] * gain);
      int32_t nxt = (i + 1 < total) ? (int32_t)(src[i + 1] * gain) : cur;
      i++;

      int32_t mid = (cur + nxt) / 2;
      if (cur > 32767) cur = 32767;
      if (cur < -32768) cur = -32768;
      if (mid > 32767) mid = 32767;
      if (mid < -32768) mid = -32768;

      frame[n * 2] = cur << 16;
      frame[n * 2 + 1] = cur << 16;
      n++;
      frame[n * 2] = mid << 16;
      frame[n * 2 + 1] = mid << 16;
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

  // Pause detection for the whole call, not just playback. ESP-SR saturates
  // both cores, and the TLS download measured only 46 KB/s against a 48 KB/s
  // playback rate - slower than real time. Nothing needs detecting while the
  // device is fetching and speaking its own reply.
  sr_pause();

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
  if (!http.begin(client, "https://api.openai.com/v1/audio/speech")) {
    sr_resume();
    return false;
  }
  http.addHeader("Authorization", String("Bearer ") + OPENAI_API_KEY);
  http.addHeader("Content-Type", "application/json");

  uint32_t t0 = millis();
  int code = http.POST(body);
  if (code != HTTP_CODE_OK) {
    logf("tts http %d: %s", code, http.getString().substring(0, 120).c_str());
    http.end();
    sr_resume();
    return false;
  }

  BufferSink sink(gTtsBuf, gTtsCap);
  http.writeToStream(&sink);
  size_t got = sink.length();
  if (got < 2) {
    logf("tts returned no audio");
    sr_resume();
    return false;
  }
  // TTS rarely comes back near full scale, and at 3V3 the amplifier has ~8 dB
  // less output than it would at 5V, so use the headroom rather than waste it.
  int32_t peak = 0;
  const int16_t *samples = (const int16_t *)gTtsBuf;
  for (size_t i = 0; i < got / 2; i++) {
    int32_t mag = samples[i] < 0 ? -(int32_t)samples[i] : (int32_t)samples[i];
    if (mag > peak) peak = mag;
  }

  // Target well short of full scale. At 3V3 the amplifier runs out of swing
  // before the samples do, so normalising to ~95% buys a little loudness and a
  // lot of clipping distortion. 80% keeps transients clean.
  float gain = TTS_VOLUME;
  if (peak > 0) {
    float boost = 26000.0f / (float)peak;
    if (boost > TTS_MAX_BOOST) boost = TTS_MAX_BOOST;
    if (boost > 1.0f) gain = boost;
  }

  uint32_t fetchMs = millis() - t0;
  logf("tts %u KB in %lu ms (%lu KB/s), playing %.1f s (peak %d, gain x%.2f)",
       (unsigned)(got / 1024), (unsigned long)fetchMs,
       (unsigned long)(fetchMs ? (got / fetchMs) : 0),
       got / 2.0f / TTS_SAMPLE_RATE, (int)peak, gain);

  // ESP-SR runs its AFE and MultiNet continuously across both cores. Leaving it
  // running during playback means the feed task, the detect task and the
  // playback loop all contend for CPU and PSRAM bandwidth, which shows up as
  // dropouts. Nothing needs to be detected while we are talking anyway.
  playPcm(gTtsBuf, got, gain);
  sr_resume();
  return true;
}
#endif  // TTS_ENABLED


// ------------------------------------------------------------- realtime ----
#if REALTIME_ENABLED
using namespace websockets;

static WebsocketsClient gWs;

// Decoded 8 kHz audio waiting to be played. The socket callback fills it and
// the session loop drains it in small blocks, so neither starves the other.
// Playback runs at 24 kHz, not the 8 kHz of the uplink: mu-law caps audio
// bandwidth at 4 kHz, which is telephone quality and audibly thin. The API
// accepts different formats per direction, so the microphone stays cheap while
// the reply comes back at full rate.
static const uint32_t RT_RX_RATE = 24000;

// Audio arrives faster than it plays, so the ring holds a whole reply. 30 s at
// 24 kHz is 1.44 MB of PSRAM.
static const size_t RT_RING = 24000 * 30;
static int16_t *gRtRing = nullptr;
static volatile size_t gRtHead = 0, gRtTail = 0;
static String gRtTranscript;
static bool gRtSawAudio = false;
static uint32_t gRtRxBytes = 0;   // mu-law received from the API
static uint32_t gRtTxBytes = 0;   // mu-law sent from the microphone
static uint32_t gRtPlayed = 0;    // samples actually written to I2S
static uint32_t gRtEvents = 0;    // websocket messages seen
static uint32_t gRtDropped = 0;   // ring overruns
static volatile bool gRtResponseDone = false;  // the model finished a turn
static String gRtTurnText;        // transcript of just the current turn
static volatile uint32_t gRtTurnRx = 0;  // audio bytes in the current turn

// What the model signalled about this turn, via a forced tool call.
static const uint8_t RT_INTENT_UNKNOWN = 0;
static const uint8_t RT_INTENT_END     = 1;
static const uint8_t RT_INTENT_STAY    = 2;
static volatile uint8_t gRtIntent = RT_INTENT_UNKNOWN;
static volatile bool gRtStopRequested = false;  // you said stop / shut up / etc.
static String gRtCallId;

static inline size_t rtAvailable() {
  size_t h = gRtHead, t = gRtTail;
  return (h >= t) ? (h - t) : (RT_RING - t + h);
}

// G.711 mu-law. Both directions are a handful of shifts - no table needed.
static inline int16_t ulawToPcm(uint8_t u) {
  u = ~u;
  int16_t t = ((u & 0x0F) << 3) + 0x84;
  t <<= ((unsigned)u & 0x70) >> 4;
  return (u & 0x80) ? (0x84 - t) : (t - 0x84);
}

static inline uint8_t pcmToUlaw(int16_t pcm) {
  const int16_t BIAS = 0x84, CLIP = 32635;
  int sign = (pcm >> 8) & 0x80;
  if (sign) pcm = -pcm;
  if (pcm > CLIP) pcm = CLIP;
  pcm += BIAS;
  int exponent = 7;
  for (int mask = 0x4000; (pcm & mask) == 0 && exponent > 0; exponent--, mask >>= 1) {}
  int mantissa = (pcm >> (exponent + 3)) & 0x0F;
  return (uint8_t)~(sign | (exponent << 4) | mantissa);
}

// "stop", "shut up", "never mind" and friends end the session. Matched only
// against a short whole utterance, so "when does the bus stop running" is safe.
static bool isStopPhrase(const String &raw) {
  static const char *PHRASES[] = {
    "stop", "stop it", "shut up", "be quiet", "quiet",
    "turn off", "turn it off", "shut down", "shut off",
    "never mind", "nevermind", "cancel", "forget it",
    "that is all", "thats all", "that's all", "we are done", "were done",
    "goodbye", "good bye", "bye", "bye bye", "exit", "end",
  };

  String t;
  for (int i = 0; i < (int)raw.length(); i++) {
    char c = raw[i];
    if (isalnum((unsigned char)c) || c == ' ' || c == '\'') t += (char)tolower((unsigned char)c);
    else if (c == ',' || c == '.' || c == '!' || c == '?') t += ' ';
  }
  t.trim();
  while (t.indexOf("  ") >= 0) t.replace("  ", " ");
  if (t.length() == 0 || t.length() > 24) return false;  // long sentences are not commands

  for (size_t i = 0; i < sizeof(PHRASES) / sizeof(PHRASES[0]); i++) {
    if (t == PHRASES[i]) return true;
    String withPlease = String(PHRASES[i]) + " please";
    if (t == withPlease) return true;
  }
  return false;
}

static void rtOnMessage(websockets::WebsocketsMessage msg) {
  // Only two fields matter on the hot path, and audio frames are several KB,
  // so filter rather than materialise the whole event.
  JsonDocument filter;
  filter["type"] = true;
  filter["delta"] = true;
  filter["transcript"] = true;
  filter["item"]["type"] = true;
  filter["item"]["name"] = true;
  filter["item"]["call_id"] = true;
  filter["error"]["message"] = true;

  JsonDocument doc;
  if (deserializeJson(doc, msg.data(), DeserializationOption::Filter(filter))) return;
  gRtEvents++;

  const char *type = doc["type"] | "";

  if (!strcmp(type, "response.output_audio.delta")) {
    const char *b64 = doc["delta"] | "";
    size_t b64Len = strlen(b64);
    if (!b64Len || !gRtRing) return;

    // Sized for a whole delta, in PSRAM. 24 kHz PCM chunks are several times
    // larger than the 8 kHz mu-law ones this started with, and a too-small
    // buffer makes mbedtls_base64_decode fail and drop the chunk entirely -
    // which showed up as 2400 bytes of audio for a full spoken reply.
    static uint8_t *decoded = nullptr;
    static size_t decodedCap = 0;
    size_t needed = (b64Len / 4) * 3 + 4;
    if (needed > decodedCap) {
      uint8_t *grown = (uint8_t *)ps_realloc(decoded, needed);
      if (!grown) {
        gRtDropped++;
        return;
      }
      decoded = grown;
      decodedCap = needed;
    }

    size_t outLen = 0;
    if (mbedtls_base64_decode(decoded, decodedCap, &outLen,
                              (const unsigned char *)b64, b64Len) != 0) {
      gRtDropped++;
      return;
    }
    gRtRxBytes += outLen;
    gRtTurnRx += outLen;
    // Little-endian signed 16-bit, straight from the wire.
    for (size_t i = 0; i + 1 < outLen; i += 2) {
      size_t next = (gRtHead + 1) % RT_RING;
      if (next == gRtTail) { gRtDropped++; break; }  // ring full
      gRtRing[gRtHead] = (int16_t)((uint16_t)decoded[i] | ((uint16_t)decoded[i + 1] << 8));
      gRtHead = next;
    }
    gRtSawAudio = true;

  } else if (!strcmp(type, "response.output_audio_transcript.delta")) {
    const char *d = doc["delta"] | "";
    gRtTranscript += d;
    gRtTurnText += d;

  } else if (!strcmp(type, "response.created")) {
    gRtTurnText = "";
    gRtTurnRx = 0;
    gRtResponseDone = false;
    gRtIntent = RT_INTENT_UNKNOWN;

  } else if (!strcmp(type, "response.output_item.done")) {
    const char *itemType = doc["item"]["type"] | "";
    if (!strcmp(itemType, "function_call")) {
      const char *name = doc["item"]["name"] | "";
      gRtCallId = (const char *)(doc["item"]["call_id"] | "");
      if (!strcmp(name, "end_conversation")) gRtIntent = RT_INTENT_END;
      else if (!strcmp(name, "stay_open"))   gRtIntent = RT_INTENT_STAY;
    }

  } else if (!strcmp(type, "conversation.item.input_audio_transcription.completed")) {
    String heard = (const char *)(doc["transcript"] | "");
    if (isStopPhrase(heard)) {
      logf("heard \"%s\" - ending", heard.c_str());
      gRtStopRequested = true;
    }

  } else if (!strcmp(type, "response.done")) {
    gRtResponseDone = true;

  } else if (!strcmp(type, "error")) {
    logf("realtime error: %s", (const char *)(doc["error"]["message"] | "unknown"));
  }
}

// Pop one 20 ms block and play it. 8 kHz in, 48 kHz out, linearly interpolated
// so the amplifier sees the rate it actually supports.
static void rtPlayBlock() {
  static int16_t src[481];
  // 7.7 KB, and internal RAM is exactly what the TLS handshake is short of.
  static int32_t *frame = nullptr;
  if (!frame) {
    frame = (int32_t *)ps_malloc(160 * 6 * 2 * sizeof(int32_t));
    if (!frame) return;
  }

  size_t have = rtAvailable();
  if (have < 481) return;

  for (size_t i = 0; i < 481; i++) {
    src[i] = gRtRing[(gRtTail + i) % RT_RING];
  }
  gRtTail = (gRtTail + 480) % RT_RING;

  // 480 samples at 24 kHz -> 960 frames at 48 kHz, 20 ms either way.
  size_t f = 0;
  for (size_t i = 0; i < 480; i++) {
    int32_t a = src[i], b = src[i + 1];
    for (int k = 0; k < 2; k++) {
      int32_t v = a + ((b - a) * k) / 2;
      v = (int32_t)(v * TTS_VOLUME);
      if (v > 32767) v = 32767;
      if (v < -32768) v = -32768;
      frame[f * 2] = v << 16;
      frame[f * 2 + 1] = v << 16;
      f++;
    }
  }
  Speaker.write((uint8_t *)frame, f * 2 * sizeof(int32_t));
  gRtPlayed += 480;
}

static bool rtSendJson(JsonDocument &doc) {
  String out;
  serializeJson(doc, out);
  return gWs.send(out);
}

// One conversation. Opens the socket, streams microphone up and audio down
// until the user goes quiet, then tears everything down.
static void realtimeSession() {
  if (!gRtRing) {
    gRtRing = (int16_t *)ps_malloc(RT_RING * sizeof(int16_t));
    if (!gRtRing) {
      logf("no PSRAM for the realtime ring");
      return;
    }
  }
  gRtHead = gRtTail = 0;
  gRtTranscript = "";
  gRtSawAudio = false;
  gRtRxBytes = gRtTxBytes = gRtPlayed = gRtEvents = gRtDropped = 0;
  gRtResponseDone = false;
  gRtTurnText = "";
  gRtTurnRx = 0;
  gRtIntent = RT_INTENT_UNKNOWN;
  gRtStopRequested = false;
  gRtCallId = "";

  setStatus(STATUS_WORKING);
  oledShow("Nova", "Connecting...");

  // setInsecure() is a no-op on ESP32 in this library: upgradeToSecuredConnection()
  // only applies setInsecure on ESP8266, so without a CA the WiFiClientSecure it
  // creates has no trust anchor and the handshake fails before the upgrade.
  logf("heap before connect: free=%u largest=%u",
       (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());

  // The TLS probe that used to live here confirmed transport is fine and the
  // failures are in the websocket upgrade, so it is gone: opening and tearing
  // down a full TLS session immediately before the real one only fragments the
  // heap at the worst moment.

  String url = String("wss://api.openai.com/v1/realtime?model=") + REALTIME_MODEL;
  uint32_t t0 = millis();

  // Configure exactly once for the lifetime of the program. addHeader() appends
  // to a vector that WebsocketsClient::operator= does not assign, so neither a
  // second call nor `gWs = WebsocketsClient()` clears it - headers accumulate
  // across attempts and across sessions. OpenAI answers a duplicated
  // Authorization header with 400 Bad Request, confirmed by replaying both
  // handshakes against the live endpoint, which is why this worked for the
  // first few sessions after boot and then failed until reboot.
  static bool wsConfigured = false;
  if (!wsConfigured) {
    gWs.setCACert(OPENAI_ROOT_CA);
    gWs.addHeader("Authorization", String("Bearer ") + OPENAI_API_KEY);
    gWs.onMessage(rtOnMessage);
    wsConfigured = true;
  }

  bool connected = false;
  for (int attempt = 1; attempt <= 3 && !connected; attempt++) {
    connected = gWs.connect(url);
    if (!connected) {
      logf("connect attempt %d failed (largest block %u)",
           attempt, (unsigned)ESP.getMaxAllocHeap());
      delay(500);
    }
  }
  if (!connected) {
    // Only probe on total failure, so the extra session does not fragment the
    // heap on the common path. It separates transport from the upgrade.
    NetworkClientSecure probe;
    probe.setCACert(OPENAI_ROOT_CA);
    probe.setHandshakeTimeout(15);
    if (probe.connect("api.openai.com", 443)) {
      logf("but plain TLS succeeded - the websocket upgrade is what failed");
      probe.stop();
    } else {
      char err[128] = {0};
      int code = probe.lastError(err, sizeof(err));
      logf("plain TLS also failed (%d) %s - transport problem", code, err);
    }
    logf("realtime connect failed after 3 attempts");
    oledShow("Error", "Could not connect");
    setStatus(STATUS_ERROR);
    return;
  }
  logf("realtime connected in %lu ms", (unsigned long)(millis() - t0));

  // GA session shape: formats are objects, and mu-law keeps this to ~8 KB/s
  // each way instead of the 48 KB/s that pcm16 would need.
  {
    JsonDocument cfg;
    cfg["type"] = "session.update";
    JsonObject sess = cfg["session"].to<JsonObject>();
    sess["type"] = "realtime";
    sess["instructions"] = REALTIME_INSTRUCTIONS;
    sess["output_modalities"].to<JsonArray>().add("audio");

    // No tools here. Registering them with tool_choice "required" makes the
    // model emit the tool call INSTEAD of speech - measured against the live
    // API: 0 bytes of audio with "required", 96 KB with "auto" but then it
    // never calls the tool. Turn intent is judged locally instead.

    JsonObject audio = sess["audio"].to<JsonObject>();
    JsonObject in = audio["input"].to<JsonObject>();
    in["format"]["type"] = "audio/pcmu";
    // Transcribing your side too, so "stop" and "shut up" can be recognised.
    in["transcription"]["model"] = "whisper-1";
    JsonObject turn = in["turn_detection"].to<JsonObject>();
    turn["type"] = "server_vad";
    turn["silence_duration_ms"] = 400;

    JsonObject out = audio["output"].to<JsonObject>();
    out["format"]["type"] = "audio/pcm";
    out["format"]["rate"] = RT_RX_RATE;
    out["voice"] = REALTIME_VOICE;
    rtSendJson(cfg);
  }

  // The wake phrase is still in the I2S buffer at this point. Throw it away,
  // both locally and server-side, or it gets treated as the first utterance.
  {
    static int32_t flush[I2S_CHUNK_FRAMES];
    for (int i = 0; i < 12; i++) I2S.readBytes((char *)flush, sizeof(flush));
    JsonDocument clr;
    clr["type"] = "input_audio_buffer.clear";
    rtSendJson(clr);
  }

  oledShow("Nova", "Listening...");
  setStatus(STATUS_RECORDING);
  emitEvent("state", "state", "realtime");

  static int32_t raw[I2S_CHUNK_FRAMES];
  static uint8_t ulaw[1024];
  static char b64[1500];
  size_t ulawLen = 0;

  uint32_t start = millis();
  uint32_t lastVoice = millis();
  bool awaitingReply = false;
  String shown;

  while (gWs.available() && (millis() - start) < REALTIME_MAX_MS) {
    gWs.poll();

    // Draining the ring takes priority: a gap here is audible, a late
    // microphone chunk is not.
    if (rtAvailable() >= 481) {
      rtPlayBlock();
      lastVoice = millis();
      if (gRtTranscript.length() && gRtTranscript != shown) {
        shown = gRtTranscript;
        oledShow("Nova", shown.c_str());
      }
      continue;
    }

    // No echo cancellation here, so only capture when nothing is playing -
    // otherwise the server hears our own speaker and interrupts itself.
    size_t bytes = I2S.readBytes((char *)raw, sizeof(raw));
    size_t frames = bytes / sizeof(int32_t);

    int32_t peak = 0;
    for (size_t i = 0; i + 1 < frames; i += 2) {
      // 24-bit -> 16-bit, high-passed, then 16 kHz -> 8 kHz by averaging pairs.
      float x0 = (float)(raw[i] >> 8);
      float y0 = HPF_ALPHA * (gHpfY1 + x0 - gHpfX1);
      gHpfX1 = x0; gHpfY1 = y0;
      float x1 = (float)(raw[i + 1] >> 8);
      float y1 = HPF_ALPHA * (gHpfY1 + x1 - gHpfX1);
      gHpfX1 = x1; gHpfY1 = y1;

      int32_t v = (int32_t)(((y0 + y1) * 0.5f) * MIC_GAIN / 256.0f);
      if (v > 32767) v = 32767;
      if (v < -32768) v = -32768;
      int32_t mag = v < 0 ? -v : v;
      if (mag > peak) peak = mag;

      if (ulawLen < sizeof(ulaw)) ulaw[ulawLen++] = pcmToUlaw((int16_t)v);
    }
    if (peak > SPEECH_MIN_PEAK) {
      lastVoice = millis();
      awaitingReply = false;  // you answered; the next turn decides afresh
    }

    if (ulawLen >= 800) {
      size_t n = 0;
      if (mbedtls_base64_encode((unsigned char *)b64, sizeof(b64), &n, ulaw, ulawLen) == 0) {
        b64[n] = '\0';
        JsonDocument msg;
        msg["type"] = "input_audio_buffer.append";
        msg["audio"] = b64;
        rtSendJson(msg);
        gRtTxBytes += ulawLen;
      }
      ulawLen = 0;
    }

    // A finished turn with the ring drained means the reply has been spoken.
    // End there unless it asked a question, in which case stay open just long
    // enough for an answer.
    if (gRtStopRequested) {
      logf("stop requested, closing");
      break;
    }

    // Require audio: the server's VAD can hear the tail of the wake word and
    // emit an empty turn, which would otherwise close the session instantly.
    if (gRtResponseDone && gRtTurnRx > 0 && rtAvailable() < 481) {
      gRtResponseDone = false;

      String turn = gRtTurnText;
      turn.trim();

      // A lookup answers in one short sentence; an explanation runs longer and
      // invites a follow-up. Measured on real replies: "Chelsea's next match is
      // on Saturday against Liverpool." is 53 characters, a weather answer 79,
      // while an explanation of a concept runs well past 150.
      bool stay = turn.endsWith("?") || turn.length() >= REALTIME_OPEN_CHARS;

      if (stay) {
        logf("open topic (%d chars), listening %lu ms for a follow-up",
             (int)turn.length(), (unsigned long)REALTIME_FOLLOWUP_MS);
        awaitingReply = true;
        lastVoice = millis();
      } else {
        logf("lookup answered (%d chars), closing", (int)turn.length());
        break;
      }
    }

    uint32_t idleLimit = awaitingReply ? REALTIME_FOLLOWUP_MS : REALTIME_IDLE_MS;
    if ((millis() - lastVoice) > idleLimit) {
      logf("realtime idle, closing");
      break;
    }
    if (gButtonHeld) {
      logf("button pressed, closing");
      break;
    }
  }

  gWs.close();
  logf("realtime ended after %lu ms: events=%lu tx=%lu B (%.1f s) rx=%lu B (%.1f s) "
       "played=%.1f s dropped=%lu",
       (unsigned long)(millis() - start), (unsigned long)gRtEvents,
       (unsigned long)gRtTxBytes, gRtTxBytes / 8000.0f,
       (unsigned long)gRtRxBytes, gRtRxBytes / 2.0f / RT_RX_RATE,
       gRtPlayed / (float)RT_RX_RATE, (unsigned long)gRtDropped);
  if (gRtTranscript.length()) logf("said: %s", gRtTranscript.c_str());
  if (gRtTranscript.length()) emitEvent("answer", "text", gRtTranscript.c_str());
}
#endif  // REALTIME_ENABLED

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
// NVS (set by the provisioning page) wins over the config.h defaults, so a
// board reprovisioned in the field never reverts to its flash-time network.
static void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);

  String ssid, pass;
  wifiPrefsLoad(ssid, pass);
  if (ssid.length() == 0 && strcmp(WIFI_SSID, "your-wifi-ssid") != 0) {
    ssid = WIFI_SSID;
    pass = WIFI_PASS;
  }

  if (ssid.length() == 0) {
    logf("no WiFi credentials stored - connect over USB to set one");
    oledShow("WiFi Setup", "Connect via USB");
    return;
  }

  logf("connecting to %s", ssid.c_str());
  oledShow("WiFi", ssid.c_str());
  WiFi.begin(ssid.c_str(), pass.c_str());

  // Stay responsive to the provisioning page even while the initial connect
  // is pending, so a wrong password saved at flash time doesn't strand the
  // board for 20 s with no way to fix it short of a reflash.
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    pollSerialCommands();
    wifiPollScan();
    delay(50);
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
    // (SR is not running yet at this point in setup, so no pause is needed.)
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
  pollSerialCommands();
  wifiPollScan();

  gButtonHeld = buttonPressedStable();

#if REALTIME_ENABLED
  if (gState == ST_LISTENING && (gWakeRequested || gButtonHeld)) {
    gWakeRequested = false;
    gState = ST_UPLOADING;  // stops the detector re-triggering mid-session

    // sr_pause(), not sr_stop(). Tearing ESP-SR down from loop() while its feed
    // task may be inside our fill callback corrupts the heap - seen as
    // "CORRUPT HEAP: Bad tail" in PSRAM on every wake. It is also unnecessary:
    // CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL is 4096, so mbedtls's large buffers
    // already come from PSRAM rather than the internal block.
    sr_pause();
    realtimeSession();
    sr_resume();
    sr_set_mode(SR_MODE_COMMAND);

    gWakeRequested = false;
    gState = ST_LISTENING;
    setStatus(STATUS_IDLE);
    emitEvent("state", "state", "ready");
    return;
  }
  // Fall through to the shared housekeeping below rather than returning: the
  // WiFi keepalive lives at the end of loop() and still needs to run.
  oledTick();

  static uint32_t rtWifiCheck = 0;
  if (millis() - rtWifiCheck > 10000) {
    rtWifiCheck = millis();
    if (WiFi.status() != WL_CONNECTED) {
      logf("wifi dropped, reconnecting");
      WiFi.reconnect();
    }
  }
  delay(5);
  return;
#endif


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
          String spoken = answer;
          speechSanitize(spoken);
          if (spoken != answer) logf("speaking as: %s", spoken.c_str());
          speak(spoken.c_str());
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
