// One GPT-Live-1 session over WebRTC: Opus mic audio up, Opus speech down,
// and the "oai-events" data channel for transcripts and session state -
// the same four events Nova/frontend/src/hooks/useNovaLive.ts listens to.

#include <string.h>
#include "esp_log.h"
#include "esp_webrtc.h"
#include "esp_peer_default.h"
#include "esp_capture.h"
#include "av_render.h"
#include "esp_audio_enc_default.h"
#include "esp_audio_dec_default.h"
#include "cJSON.h"
#include "config.h"
#include "nova.h"

#define TAG "session"

static esp_capture_handle_t s_capture;
static av_render_handle_t s_player;
static esp_webrtc_handle_t s_webrtc;

esp_err_t session_media_init(void)
{
    esp_audio_enc_register_default();
    esp_audio_dec_register_default();

    esp_capture_cfg_t cap = {
        .sync_mode = ESP_CAPTURE_SYNC_MODE_AUDIO,
        .audio_src = audio_capture_src(),
    };
    if (esp_capture_open(&cap, &s_capture) != ESP_CAPTURE_ERR_OK) {
        ESP_LOGE(TAG, "capture open failed");
        return ESP_FAIL;
    }

    av_render_cfg_t render = {
        .audio_render = audio_speaker_render(),
        .audio_raw_fifo_size = 8 * 4096,
        .audio_render_fifo_size = 100 * 1024,
        .allow_drop_data = false,
    };
    s_player = av_render_open(&render);
    if (s_player == NULL) {
        ESP_LOGE(TAG, "player open failed");
        return ESP_FAIL;
    }
    // Decode to 16 kHz mono so the speaker path and the echo canceller's
    // reference share the mic's rate.
    av_render_audio_frame_info_t info = {
        .sample_rate = NOVA_SAMPLE_RATE,
        .channel = 1,
        .bits_per_sample = 16,
    };
    av_render_set_fixed_frame_info(s_player, &info);
    return ESP_OK;
}

static int on_data(esp_webrtc_custom_data_via_t via, uint8_t *data, int size, void *ctx)
{
    cJSON *root = cJSON_ParseWithLength((const char *)data, size);
    if (root == NULL) {
        return 0;
    }
    const cJSON *type = cJSON_GetObjectItem(root, "type");
    const char *t = cJSON_IsString(type) ? type->valuestring : "";
    const cJSON *delta = cJSON_GetObjectItem(root, "delta");
    const char *d = cJSON_IsString(delta) ? delta->valuestring : "";

    if (strcmp(t, "session.input_transcript.delta") == 0) {
        nova_post(EV_USER_DELTA, d);
    } else if (strcmp(t, "session.delegation.created") == 0) {
        nova_post(EV_DELEGATING, NULL);
    } else if (strcmp(t, "session.commentary.appended") == 0) {
        nova_post(EV_COMMENTARY, NULL);
    } else if (strcmp(t, "session.output_transcript.delta") == 0) {
        nova_post(EV_NOVA_DELTA, d);
    } else if (strcmp(t, "session.closed") == 0) {
        nova_post(EV_SESSION_CLOSED, NULL);
    } else if (strcmp(t, "error") == 0) {
        const cJSON *msg = cJSON_GetObjectItem(cJSON_GetObjectItem(root, "error"), "message");
        nova_post(EV_LIVE_ERROR, cJSON_IsString(msg) ? msg->valuestring : "Live session error.");
    }
    cJSON_Delete(root);
    return 0;
}

static int on_event(esp_webrtc_event_t *event, void *ctx)
{
    switch (event->type) {
    case ESP_WEBRTC_EVENT_CONNECTED:
        ESP_LOGI(TAG, "peer connected");
        nova_post(EV_SESSION_CONNECTED, NULL);
        break;
    case ESP_WEBRTC_EVENT_CONNECT_FAILED:
        nova_post(EV_SESSION_FAILED, "Couldn't connect to GPT-Live-1.");
        break;
    case ESP_WEBRTC_EVENT_DISCONNECTED:
        nova_post(EV_SESSION_CLOSED, NULL);
        break;
    case ESP_WEBRTC_EVENT_DATA_CHANNEL_CONNECTED: {
        // esp_peer acts as the SCTP server and doesn't open channels itself;
        // the browser's equivalent is pc.createDataChannel("oai-events").
        esp_peer_data_channel_cfg_t cfg = {.label = "oai-events"};
        esp_peer_handle_t peer = NULL;
        esp_webrtc_get_peer_connection(s_webrtc, &peer);
        esp_peer_create_data_channel(peer, &cfg);
        break;
    }
    default:
        break;
    }
    return 0;
}

bool session_active(void)
{
    return s_webrtc != NULL;
}

int session_open(void)
{
    if (s_webrtc) {
        return 0;
    }
    static nova_signaling_cfg_t nova;
    nova = (nova_signaling_cfg_t){
        .server_url = nova_server_url(),
        .token = NOVA_DEVICE_TOKEN,
        .timezone = NOVA_TIMEZONE,
        .voice = NOVA_VOICE,
    };
    esp_peer_default_cfg_t peer_cfg = {
        .agent_recv_timeout = 500,
        .ice_use_lite_mode = true,
    };
    esp_webrtc_cfg_t cfg = {
        .peer_cfg = {
            .audio_info = {
                .codec = ESP_PEER_AUDIO_CODEC_OPUS,
                .sample_rate = NOVA_SAMPLE_RATE,
                .channel = 1,
            },
            .audio_dir = ESP_PEER_MEDIA_DIR_SEND_RECV,
            .enable_data_channel = true,
            .manual_ch_create = true,
            .on_custom_data = on_data,
            .extra_cfg = &peer_cfg,
            .extra_size = sizeof(peer_cfg),
        },
        .signaling_cfg = {
            .extra_cfg = &nova,
            .extra_size = sizeof(nova),
        },
        .peer_impl = esp_peer_get_default_impl(),
        .signaling_impl = nova_signaling_impl(),
    };
    int ret = esp_webrtc_open(&cfg, &s_webrtc);
    if (ret != 0) {
        ESP_LOGE(TAG, "webrtc open failed (%d)", ret);
        s_webrtc = NULL;
        return ret;
    }
    esp_webrtc_media_provider_t media = {.capture = s_capture, .player = s_player};
    esp_webrtc_set_media_provider(s_webrtc, &media);
    esp_webrtc_set_event_handler(s_webrtc, on_event, NULL);

    audio_set_mode(AUDIO_MODE_SESSION);
    ret = esp_webrtc_start(s_webrtc);
    if (ret != 0) {
        ESP_LOGE(TAG, "webrtc start failed (%d)", ret);
        session_teardown();
    }
    return ret;
}

void session_request_close(void)
{
    if (s_webrtc == NULL) {
        return;
    }
    // Same as the browser: ask politely, the server confirms with
    // session.closed (and reports usage); the caller force-closes after 4 s.
    static const char close_msg[] = "{\"type\":\"session.close\"}";
    esp_webrtc_send_custom_data(s_webrtc, ESP_WEBRTC_CUSTOM_DATA_VIA_DATA_CHANNEL,
                                (uint8_t *)close_msg, sizeof(close_msg) - 1);
}

void session_teardown(void)
{
    if (s_webrtc) {
        esp_webrtc_handle_t h = s_webrtc;
        s_webrtc = NULL;
        esp_webrtc_close(h);
    }
    av_render_reset(s_player);
    audio_set_mode(AUDIO_MODE_WAKE);
}
