// SDP exchange through Nova's backend - the same POST /api/live/session the
// browser makes (Nova/frontend/src/hooks/useNovaLive.ts). The backend holds
// the OpenAI key, creates the GPT-Live-1 session in client-delegation mode
// and attaches Claude + Composio to it; this device only carries audio.

#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"
#include "nova.h"

#define TAG "signaling"

typedef struct {
    esp_peer_signaling_cfg_t cfg;
    nova_signaling_cfg_t     nova;
    char                    *answer;
} nova_sig_t;

typedef struct {
    char *data;
    int   len;
} body_t;

static esp_err_t on_http_event(esp_http_client_event_t *evt)
{
    body_t *body = evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && evt->data_len > 0) {
        char *grown = realloc(body->data, body->len + evt->data_len + 1);
        if (grown == NULL) {
            return ESP_ERR_NO_MEM;
        }
        body->data = grown;
        memcpy(body->data + body->len, evt->data, evt->data_len);
        body->len += evt->data_len;
        body->data[body->len] = 0;
    }
    return ESP_OK;
}

static char *error_from_body(int status, const char *body)
{
    char msg[160];
    cJSON *root = body ? cJSON_Parse(body) : NULL;
    cJSON *err = cJSON_GetObjectItem(root, "error");
    if (cJSON_IsString(err)) {
        snprintf(msg, sizeof(msg), "%s", err->valuestring);
    } else {
        snprintf(msg, sizeof(msg), "Nova's backend answered %d", status);
    }
    cJSON_Delete(root);
    return strdup(msg);
}

// Returns the answer SDP, or NULL with *error set.
static char *exchange_sdp(nova_sig_t *sig, const char *offer, char **error)
{
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "sdp", offer);
    if (sig->nova.timezone && sig->nova.timezone[0]) {
        cJSON_AddStringToObject(req, "timezone", sig->nova.timezone);
    }
    if (sig->nova.voice && sig->nova.voice[0]) {
        cJSON_AddStringToObject(req, "voice", sig->nova.voice);
    }
    char *payload = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);

    char url[200];
    snprintf(url, sizeof(url), "%s/api/live/session", sig->nova.server_url);
    char auth[200];
    snprintf(auth, sizeof(auth), "Bearer %s", sig->nova.token);

    body_t body = {0};
    esp_http_client_config_t http = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 20000,
        .event_handler = on_http_event,
        .user_data = &body,
        .crt_bundle_attach = esp_crt_bundle_attach,  // only used for an https:// server
        .buffer_size_tx = 2048,
    };
    esp_http_client_handle_t client = esp_http_client_init(&http);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Authorization", auth);
    esp_http_client_set_post_field(client, payload, strlen(payload));

    ESP_LOGI(TAG, "POST %s (offer %d bytes)", url, (int)strlen(offer));
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    cJSON_free(payload);

    char *answer = NULL;
    if (err != ESP_OK) {
        char msg[200];
        snprintf(msg, sizeof(msg), "Can't reach Nova's backend at %s (%s)", sig->nova.server_url, esp_err_to_name(err));
        *error = strdup(msg);
    } else if (status != 201 && status != 200) {
        *error = error_from_body(status, body.data);
    } else {
        cJSON *root = cJSON_Parse(body.data);
        cJSON *sdp = cJSON_GetObjectItem(root, "sdp");
        cJSON *id = cJSON_GetObjectItem(root, "sessionId");
        if (cJSON_IsString(sdp)) {
            answer = strdup(sdp->valuestring);
            ESP_LOGI(TAG, "GPT-Live-1 session %s", cJSON_IsString(id) ? id->valuestring : "?");
        } else {
            *error = strdup("Nova's backend returned no SDP answer");
        }
        cJSON_Delete(root);
    }
    free(body.data);
    return answer;
}

static int sig_start(esp_peer_signaling_cfg_t *cfg, esp_peer_signaling_handle_t *h)
{
    nova_signaling_cfg_t *nova = cfg->extra_cfg;
    if (nova == NULL || nova->server_url == NULL || nova->token == NULL) {
        return ESP_PEER_ERR_INVALID_ARG;
    }
    nova_sig_t *sig = calloc(1, sizeof(nova_sig_t));
    if (sig == NULL) {
        return ESP_PEER_ERR_NO_MEM;
    }
    sig->cfg = *cfg;
    sig->nova = *nova;
    *h = sig;
    // OpenAI's side is ICE-lite with public candidates, so no STUN/TURN is
    // needed and this device drives ICE as the controlling agent.
    esp_peer_signaling_ice_info_t ice = {.is_initiator = true};
    if (sig->cfg.on_ice_info) {
        sig->cfg.on_ice_info(&ice, sig->cfg.ctx);
    }
    if (sig->cfg.on_connected) {
        sig->cfg.on_connected(sig->cfg.ctx);
    }
    return ESP_PEER_ERR_NONE;
}

static int sig_send(esp_peer_signaling_handle_t h, esp_peer_signaling_msg_t *msg)
{
    nova_sig_t *sig = h;
    if (msg->type != ESP_PEER_SIGNALING_MSG_SDP) {
        return 0;  // candidates ride inside the SDP offer
    }
    char *offer = strndup((const char *)msg->data, msg->size);
    if (offer == NULL) {
        return ESP_PEER_ERR_NO_MEM;
    }
    char *error = NULL;
    free(sig->answer);
    sig->answer = exchange_sdp(sig, offer, &error);
    free(offer);
    if (sig->answer == NULL) {
        ESP_LOGE(TAG, "%s", error ? error : "SDP exchange failed");
        nova_post(EV_SESSION_FAILED, error);
        free(error);
        return -1;
    }
    esp_peer_signaling_msg_t answer = {
        .type = ESP_PEER_SIGNALING_MSG_SDP,
        .data = (uint8_t *)sig->answer,
        .size = strlen(sig->answer),
    };
    if (sig->cfg.on_msg) {
        sig->cfg.on_msg(&answer, sig->cfg.ctx);
    }
    return 0;
}

static int sig_stop(esp_peer_signaling_handle_t h)
{
    nova_sig_t *sig = h;
    if (sig->cfg.on_close) {
        sig->cfg.on_close(sig->cfg.ctx);
    }
    free(sig->answer);
    free(sig);
    return 0;
}

const esp_peer_signaling_impl_t *nova_signaling_impl(void)
{
    static const esp_peer_signaling_impl_t impl = {
        .start = sig_start,
        .send_msg = sig_send,
        .stop = sig_stop,
    };
    return &impl;
}
