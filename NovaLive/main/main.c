// Nova on the ESP32-S3. The state machine below is a port of
// Nova/frontend/src/hooks/useNovaConversation.ts: a free on-device wake
// word gates the paid GPT-Live-1 session, and the session hangs up again
// after things go quiet. Everything Nova actually knows or does (Claude,
// web search, Gmail/Calendar via Composio, background tasks) runs in the
// backend, attached to the session by liveDelegate.ts.

#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "media_lib_adapter.h"
#include "media_lib_os.h"
#include "esp_capture.h"
#include "config.h"
#include "nova.h"

#define TAG "nova"

// Reply window: after Nova finishes an answer you have NOVA_FOLLOWUP_MS to say
// something; silence that long hangs up. The first window, right after the
// connect chime, is longer because you have to react to the chime.
#ifndef NOVA_FOLLOWUP_MS
#define NOVA_FOLLOWUP_MS 5000
#endif
#ifndef NOVA_FIRST_WINDOW_MS
#define NOVA_FIRST_WINDOW_MS 8000
#endif
#define SPEAKING_PAUSE_MS   1200   // no speech from Nova this long = she stopped talking
#define ANSWER_START_MS     3000   // once the backend's answer lands, speech should start within this
#define BACKEND_TIMEOUT_MS  45000  // delegated but never answered
#define RECOVERY_GRACE_MS   25000  // GPT-Live acknowledged you but never delegated; the backend rescues it (5 s + Claude)
#define USER_STALL_MS       8000   // you spoke, but nothing was delegated or said back
#define FORCE_CLOSE_MS      4000   // wait for session.closed before dropping media
#define CONNECT_TIMEOUT_MS  15000

#define TRANSCRIPT_MAX 600

typedef enum {
    ST_NO_NETWORK,
    ST_WAKE_LISTENING,
    ST_POWERING_ON,
    ST_LIVE_IDLE,
    ST_RECORDING,
    ST_THINKING,
    ST_SPEAKING,
} nova_state_t;

static const char *STATE_LABEL[] = {
    [ST_NO_NETWORK] = "No WiFi",
    [ST_WAKE_LISTENING] = "Say \"Hey Nova\"",
    [ST_POWERING_ON] = "Connecting",
    [ST_LIVE_IDLE] = "Listening",
    [ST_RECORDING] = "Listening",
    [ST_THINKING] = "Thinking",
    [ST_SPEAKING] = "Nova",
};
static const char *STATE_NAME[] = {
    [ST_NO_NETWORK] = "no-network",
    [ST_WAKE_LISTENING] = "wake-listening",
    [ST_POWERING_ON] = "powering-on",
    [ST_LIVE_IDLE] = "live-idle",
    [ST_RECORDING] = "recording",
    [ST_THINKING] = "thinking",
    [ST_SPEAKING] = "speaking",
};
// The round display's clock face covers both "no network" and "say the
// wake word" - everything else is the orb, one state each.
static const nova_display_state_t STATE_DISPLAY[] = {
    [ST_NO_NETWORK] = DISP_CLOCK,
    [ST_WAKE_LISTENING] = DISP_CLOCK,
    [ST_POWERING_ON] = DISP_CONNECTING,
    [ST_LIVE_IDLE] = DISP_LIVE_IDLE,
    [ST_RECORDING] = DISP_RECORDING,
    [ST_THINKING] = DISP_THINKING,
    [ST_SPEAKING] = DISP_SPEAKING,
};

typedef struct {
    nova_event_type_t type;
    char *text;
} nova_event_t;

static QueueHandle_t s_events;
static nova_state_t s_state = ST_NO_NETWORK;
static bool s_busy, s_closing;
static int s_pending;  // delegations the backend hasn't answered yet
static bool s_undelegated;  // you spoke and nothing has been delegated or answered since
static esp_reset_reason_t s_boot_reason;
static esp_timer_handle_t s_idle_timer, s_speaking_timer, s_thinking_timer, s_force_timer;
static char s_user[TRANSCRIPT_MAX + 1], s_nova[TRANSCRIPT_MAX + 1];
static bool s_user_new_turn = true, s_nova_new_turn = true;

void nova_post(nova_event_type_t type, const char *text)
{
    nova_event_t ev = {.type = type, .text = text ? strdup(text) : NULL};
    if (xQueueSend(s_events, &ev, 0) != pdTRUE) {
        free(ev.text);
    }
}

static void set_state(nova_state_t st)
{
    if (st == s_state) {
        return;
    }
    s_state = st;
    led_set_session(st != ST_NO_NETWORK && st != ST_WAKE_LISTENING);
    display_state(STATE_LABEL[st]);
    display_orb_state(STATE_DISPLAY[st]);
    serial_emit("state", "state", STATE_NAME[st]);
}

// ---------------------------------------------------------------- timers ---
static void timer_cb(void *arg)
{
    nova_post((nova_event_type_t)(intptr_t)arg, NULL);
}

static esp_timer_handle_t make_timer(nova_event_type_t ev, const char *name)
{
    esp_timer_handle_t t;
    esp_timer_create_args_t args = {.callback = timer_cb, .arg = (void *)(intptr_t)ev, .name = name};
    ESP_ERROR_CHECK(esp_timer_create(&args, &t));
    return t;
}

static void stop_timer(esp_timer_handle_t t)
{
    esp_timer_stop(t);  // ESP_ERR_INVALID_STATE when not running is fine
}

static void start_timer(esp_timer_handle_t t, int ms)
{
    stop_timer(t);
    esp_timer_start_once(t, (uint64_t)ms * 1000);
}

static void stop_all_timers(void)
{
    stop_timer(s_idle_timer);
    stop_timer(s_speaking_timer);
    stop_timer(s_thinking_timer);
    stop_timer(s_force_timer);
}

// -------------------------------------------------------------- transcript ---
static void append(char *buf, bool *new_turn, const char *delta)
{
    if (*new_turn) {
        buf[0] = 0;
        *new_turn = false;
    }
    size_t have = strlen(buf), add = strlen(delta);
    if (have + add > TRANSCRIPT_MAX) {
        // Keep the tail - the display shows the newest words.
        size_t drop = have + add - TRANSCRIPT_MAX;
        if (drop >= have) {
            buf[0] = 0;
            delta += add > TRANSCRIPT_MAX ? add - TRANSCRIPT_MAX : 0;
        } else {
            memmove(buf, buf + drop, have - drop + 1);
        }
    }
    strncat(buf, delta, TRANSCRIPT_MAX - strlen(buf));
}

// --------------------------------------------------------------- actions ---
static void begin_command(void)
{
    if (s_busy) {
        return;
    }
    if (!net_connected()) {
        display_text(ROLE_SYSTEM, "No WiFi yet.");
        return;
    }
    s_busy = true;
    s_closing = false;
    s_pending = 0;
    s_undelegated = false;
    s_user_new_turn = s_nova_new_turn = true;
    set_state(ST_POWERING_ON);
    audio_chime(false);
    if (session_open() != 0) {
        s_busy = false;
        display_text(ROLE_SYSTEM, "Couldn't start live voice.");
        set_state(ST_WAKE_LISTENING);
        return;
    }
    start_timer(s_force_timer, CONNECT_TIMEOUT_MS);
}

static void close_session(void)
{
    if (s_closing || !session_active()) {
        return;
    }
    s_closing = true;
    session_request_close();
    start_timer(s_force_timer, FORCE_CLOSE_MS);
}

static void log_heap(const char *when)
{
    ESP_LOGI(TAG, "heap %s: internal free %u (lowest ever %u), psram free %u", when,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

static const char *reset_reason_name(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON:  return "power-on";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_PANIC:    return "panic";
    case ESP_RST_INT_WDT:  return "interrupt-watchdog";
    case ESP_RST_TASK_WDT: return "task-watchdog";
    case ESP_RST_WDT:      return "watchdog";
    case ESP_RST_SW:       return "software";
    case ESP_RST_USB:      return "usb";
    case ESP_RST_EXT:      return "external-pin";
    default:               return "unknown";
    }
}

void nova_report_info(void)
{
    char line[220];
    snprintf(line, sizeof(line),
             "{\"type\":\"info\",\"reset_reason\":\"%s\",\"uptime_s\":%lld,\"internal_free\":%u,\"internal_min\":%u,\"psram_free\":%u}",
             reset_reason_name(s_boot_reason), (long long)(esp_timer_get_time() / 1000000),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    serial_emit_raw(line);
}

static void finish_session(void)
{
    stop_all_timers();
    session_teardown();
    log_heap("after session");
    s_busy = false;
    s_closing = false;
    s_pending = 0;
    s_undelegated = false;
    if (!s_nova_new_turn && s_nova[0]) {
        serial_emit("answer", "text", s_nova);
    }
    s_user_new_turn = s_nova_new_turn = true;
    set_state(net_connected() ? ST_WAKE_LISTENING : ST_NO_NETWORK);
}

static void handle(nova_event_t *ev)
{
    switch (ev->type) {
    case EV_NET_UP:
        if (s_state == ST_NO_NETWORK) {
            set_state(ST_WAKE_LISTENING);
        }
        break;
    case EV_NET_DOWN:
        if (s_busy) {
            finish_session();
        }
        set_state(ST_NO_NETWORK);
        break;
    case EV_WAKE:
        if (s_state == ST_WAKE_LISTENING) {
            begin_command();
        }
        break;
    case EV_BUTTON:
        // The orb tap: start a session, or hang up an idle one early.
        if (s_state == ST_WAKE_LISTENING) {
            begin_command();
        } else if (s_state == ST_LIVE_IDLE) {
            close_session();
        }
        break;
    case EV_SESSION_CONNECTED:
        if (s_state == ST_POWERING_ON) {
            stop_timer(s_force_timer);
            set_state(ST_LIVE_IDLE);
            audio_chime(true);
            start_timer(s_idle_timer, NOVA_FIRST_WINDOW_MS);
        }
        break;
    case EV_SESSION_FAILED:
        if (!s_busy) {
            break;  // signaling and the peer connection can both report one failure
        }
        display_text(ROLE_SYSTEM, ev->text ? ev->text : "Live session failed.");
        serial_emit("error", "error", ev->text ? ev->text : "live session failed");
        finish_session();
        break;
    case EV_SESSION_CLOSED:
    case EV_TIMER_FORCE_CLOSE:
        if (s_state == ST_POWERING_ON) {
            display_text(ROLE_SYSTEM, "Couldn't connect to GPT-Live-1.");
        }
        if (s_busy) {
            finish_session();
        }
        break;
    case EV_USER_DELTA:
        if (!s_busy) {
            break;
        }
        // You have the floor: any reply window closes, and Nova's "she's
        // done" guess is void. If nothing gets delegated or said back, the
        // stall timer hangs up rather than leaving the session open.
        stop_timer(s_idle_timer);
        stop_timer(s_speaking_timer);
        start_timer(s_thinking_timer, s_pending ? BACKEND_TIMEOUT_MS : USER_STALL_MS);
        s_undelegated = true;
        set_state(ST_RECORDING);
        append(s_user, &s_user_new_turn, ev->text ? ev->text : "");
        display_text(ROLE_USER, s_user);
        break;
    case EV_DELEGATING:
        if (!s_busy) {
            break;
        }
        if (!s_user_new_turn) {
            serial_emit("transcript", "text", s_user);
        }
        s_user_new_turn = true;
        s_undelegated = false;
        s_pending++;
        stop_timer(s_idle_timer);
        stop_timer(s_speaking_timer);
        start_timer(s_thinking_timer, BACKEND_TIMEOUT_MS);
        set_state(ST_THINKING);
        break;
    case EV_COMMENTARY:
        // The backend's answer just reached GPT-Live; speech follows within
        // a second or so. Don't let a quiet moment before it start the window.
        if (!s_busy) {
            break;
        }
        s_undelegated = false;
        if (s_pending > 0) {
            s_pending--;
        }
        stop_timer(s_thinking_timer);
        stop_timer(s_idle_timer);
        start_timer(s_speaking_timer, ANSWER_START_MS);
        break;
    case EV_NOVA_DELTA:
        if (!s_busy) {
            break;
        }
        // Any speech from Nova proves the session is alive - cancel every
        // pending "she's done" guess, including a hang-up.
        stop_timer(s_speaking_timer);
        stop_timer(s_thinking_timer);
        stop_timer(s_idle_timer);
        set_state(ST_SPEAKING);
        append(s_nova, &s_nova_new_turn, ev->text ? ev->text : "");
        display_text(ROLE_NOVA, s_nova);
        start_timer(s_speaking_timer, SPEAKING_PAUSE_MS);
        break;
    case EV_TIMER_SPEAKING:
        // GPT-Live has no "done speaking" event, so this is the guess: Nova
        // went quiet. If the backend still owes an answer she's only said
        // her quick acknowledgement, so keep waiting. Otherwise she's done,
        // and the reply window opens.
        if (!s_nova_new_turn) {
            serial_emit("answer", "text", s_nova);
        }
        s_nova_new_turn = true;
        if (!s_busy) {
            break;
        }
        if (s_pending > 0) {
            set_state(ST_THINKING);
            start_timer(s_thinking_timer, BACKEND_TIMEOUT_MS);
        } else if (s_undelegated) {
            // Nova answered but your request was never handed to the backend.
            // The backend notices and rescues it within ~5 s plus Claude's time;
            // hanging up now would cut that answer off.
            set_state(ST_THINKING);
            start_timer(s_thinking_timer, RECOVERY_GRACE_MS);
        } else {
            set_state(ST_LIVE_IDLE);
            start_timer(s_idle_timer, NOVA_FOLLOWUP_MS);
        }
        break;
    case EV_TIMER_THINKING:
        if (s_busy) {
            display_text(ROLE_SYSTEM, s_pending ? "No answer from Nova's backend." : "Didn't catch that.");
            close_session();
        }
        break;
    case EV_TIMER_IDLE:
        close_session();
        break;
    case EV_LIVE_ERROR:
        ESP_LOGW(TAG, "live error: %s", ev->text ? ev->text : "");
        display_text(ROLE_SYSTEM, ev->text ? ev->text : "Live session error.");
        serial_emit("error", "error", ev->text ? ev->text : "live session error");
        break;
    }
}

// ---------------------------------------------------------------- button ---
static void button_task(void *arg)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << PIN_BUTTON,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&io);
    int last = 1, stable_ms = 0;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(10));
        int level = gpio_get_level(PIN_BUTTON);
        if (level != last) {
            stable_ms += 10;
            if (stable_ms >= 30) {
                last = level;
                stable_ms = 0;
                if (level == 0) {
                    nova_post(EV_BUTTON, NULL);
                }
            }
        } else {
            stable_ms = 0;
        }
    }
}

// ------------------------------------------------------- thread settings ---
// Stack sizes and cores for esp_webrtc/esp_capture/av_render threads, from
// Espressif's openai_demo. The Opus encoder needs a very large stack.
static void thread_scheduler(const char *name, media_lib_thread_cfg_t *cfg)
{
    if (strcmp(name, "aenc_0") == 0) {
        cfg->stack_size = 40 * 1024;
        cfg->priority = 10;
        cfg->core_id = 1;
    } else if (strcmp(name, "buffer_in") == 0) {
        cfg->stack_size = 6 * 1024;
        cfg->priority = 10;
        cfg->core_id = 0;
    } else if (strcmp(name, "AUD_SRC") == 0) {
        cfg->stack_size = 40 * 1024;
        cfg->priority = 15;
    } else if (strcmp(name, "pc_task") == 0) {
        cfg->stack_size = 25 * 1024;
        cfg->priority = 18;
        cfg->core_id = 1;
    } else if (strcmp(name, "pc_send") == 0) {
        cfg->stack_size = 4 * 1024;
        cfg->priority = 15;
        cfg->core_id = 1;
    } else if (strcmp(name, "Adec") == 0) {
        cfg->stack_size = 40 * 1024;
        cfg->priority = 15;
        cfg->core_id = 0;
    } else if (strcmp(name, "ARender") == 0) {
        cfg->priority = 20;
    } else if (strcmp(name, "start") == 0) {
        cfg->stack_size = 6 * 1024;
    }
}

static void capture_scheduler(const char *name, esp_capture_thread_schedule_cfg_t *cfg)
{
    media_lib_thread_cfg_t c = {
        .stack_size = cfg->stack_size,
        .priority = cfg->priority,
        .core_id = cfg->core_id,
    };
    cfg->stack_in_ext = true;
    thread_scheduler(name, &c);
    cfg->stack_size = c.stack_size;
    cfg->priority = c.priority;
    cfg->core_id = c.core_id;
}

// ------------------------------------------------------------------ boot ---
void app_main(void)
{
    s_events = xQueueCreate(32, sizeof(nova_event_t));
    s_idle_timer = make_timer(EV_TIMER_IDLE, "idle");
    s_speaking_timer = make_timer(EV_TIMER_SPEAKING, "speaking");
    s_thinking_timer = make_timer(EV_TIMER_THINKING, "thinking");
    s_force_timer = make_timer(EV_TIMER_FORCE_CLOSE, "force_close");

    led_init();
    serial_init();
    s_boot_reason = esp_reset_reason();
    ESP_LOGI(TAG, "boot: last reset was %s", reset_reason_name(s_boot_reason));
    serial_emit("boot", "reset_reason", reset_reason_name(s_boot_reason));
    display_init();
    display_state("Starting");

    media_lib_add_default_adapter();
    esp_capture_set_thread_scheduler(capture_scheduler);
    media_lib_thread_set_schedule_cb(thread_scheduler);

    ESP_ERROR_CHECK(net_init());
    if (audio_init() != ESP_OK) {
        display_text(ROLE_SYSTEM, "Audio init failed - see serial log.");
    }
    if (wake_init() != ESP_OK) {
        display_text(ROLE_SYSTEM, "No wake word model - press the button to talk.");
    }
    ESP_ERROR_CHECK(session_media_init());
    tasks_poll_start();
    xTaskCreate(button_task, "button", 3 * 1024, NULL, 5, NULL);

    ESP_LOGI(TAG, "Nova server: %s", nova_server_url());
    display_state(STATE_LABEL[s_state]);
    serial_emit("state", "state", STATE_NAME[s_state]);

    nova_event_t ev;
    while (true) {
        if (xQueueReceive(s_events, &ev, portMAX_DELAY) == pdTRUE) {
            handle(&ev);
            free(ev.text);
        }
    }
}
