#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_capture_audio_src_if.h"
#include "audio_render.h"
#include "esp_peer_signaling.h"

// ------------------------------------------------------------------ pins ---
// Same wiring as MicScribe (see ../README.md).
#define PIN_MIC_BCLK 4
#define PIN_MIC_WS   5
#define PIN_MIC_DIN  6
#define PIN_AMP_BCLK 10
#define PIN_AMP_LRC  11
#define PIN_AMP_DIN  12
#define PIN_I2C_SDA  8
#define PIN_I2C_SCL  9
#define PIN_BUTTON   0

#define NOVA_SAMPLE_RATE 16000

// ------------------------------------------------------------ controller ---
typedef enum {
    EV_NET_UP,
    EV_NET_DOWN,
    EV_WAKE,
    EV_BUTTON,
    EV_SESSION_CONNECTED,
    EV_SESSION_FAILED,   // text = reason
    EV_SESSION_CLOSED,
    EV_USER_DELTA,       // text
    EV_DELEGATING,
    EV_COMMENTARY,       // the backend's answer reached GPT-Live (speech follows)
    EV_NOVA_DELTA,       // text
    EV_LIVE_ERROR,       // text
    EV_TIMER_IDLE,
    EV_TIMER_SPEAKING,
    EV_TIMER_THINKING,
    EV_TIMER_FORCE_CLOSE,
} nova_event_type_t;

// Takes ownership of nothing: `text` is copied.
void nova_post(nova_event_type_t type, const char *text);
void nova_report_info(void);  // {"type":"info",reset_reason,uptime_s,heap...} on serial

// ----------------------------------------------------------------- audio ---
typedef enum {
    AUDIO_MODE_WAKE,     // mic -> wake word detector
    AUDIO_MODE_SESSION,  // mic -> echo canceller -> GPT-Live-1
} audio_mode_t;

esp_err_t audio_init(void);
void audio_set_mode(audio_mode_t mode);
// false: "heard you" blip. true: "session is live, talk now" chime.
void audio_chime(bool ready);
esp_capture_audio_src_if_t *audio_capture_src(void);
audio_render_handle_t audio_speaker_render(void);

// ------------------------------------------------------------------ wake ---
esp_err_t wake_init(void);
void wake_feed(const int16_t *samples, int count);
void wake_reset(void);

// --------------------------------------------------------------- session ---
esp_err_t session_media_init(void);
int session_open(void);
void session_request_close(void);
void session_teardown(void);
bool session_active(void);

// ------------------------------------------------------------- signaling ---
typedef struct {
    const char *server_url;
    const char *token;
    const char *timezone;
    const char *voice;
} nova_signaling_cfg_t;

const esp_peer_signaling_impl_t *nova_signaling_impl(void);

// --------------------------------------------------------------- display ---
typedef enum { ROLE_USER, ROLE_NOVA, ROLE_SYSTEM } nova_role_t;
typedef enum { TASK_LIGHT_NONE, TASK_LIGHT_RUNNING, TASK_LIGHT_DONE } task_light_t;

void display_init(void);
void display_state(const char *label);
void display_text(nova_role_t role, const char *text);
void display_task_light(task_light_t light);
void display_selftest(void);  // fills the screen with sample text; reports draw errors on serial

// ------------------------------------------------------------------- led ---
void led_init(void);
void led_set_session(bool active);
void led_set_task(task_light_t light);
void led_demo(void);  // blue, pulsing yellow, green - for checking the LED

// ------------------------------------------------------------------- net ---
esp_err_t net_init(void);
bool net_connected(void);
void net_scan(void);
void net_connect(const char *ssid, const char *pass);
void net_report_status(void);
const char *nova_server_url(void);
void nova_set_server_url(const char *url);

// ---------------------------------------------------------------- serial ---
// One JSON object per line, same convention as MicScribe, so bridge.py and
// web/wifi-setup.html keep working.
void serial_init(void);
void serial_emit(const char *type, const char *key, const char *value);
void serial_emit_raw(const char *json_line);

// ----------------------------------------------------------------- tasks ---
void tasks_poll_start(void);
