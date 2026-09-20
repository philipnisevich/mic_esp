// WiFi station plus the settings stored in NVS. Provisioning works exactly
// like MicScribe: web/wifi-setup.html sends wifi_scan / wifi_connect over
// serial, and credentials are saved only once a connection succeeds.

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "cJSON.h"
#include "config.h"
#include "nova.h"

#define TAG "net"
#define NVS_NS "nova"
#define ATTEMPT_MAX_FAILURES 3

// newlib's TZ wants a POSIX string (e.g. "PST8PDT,M3.2.0,M11.1.0"), but
// NOVA_TIMEZONE is the IANA name ("America/Los_Angeles") the backend wants -
// ESP-IDF ships no zoneinfo database to convert between them. This is a
// deliberately small table, not full tzdata: an unrecognized zone falls
// back to UTC (a warning is logged once). A literal POSIX string in
// NOVA_TIMEZONE (no '/') is passed through as-is.
static const struct { const char *iana, *posix; } TZ_TABLE[] = {
    {"America/Los_Angeles", "PST8PDT,M3.2.0,M11.1.0"},
    {"America/Denver",      "MST7MDT,M3.2.0,M11.1.0"},
    {"America/Chicago",     "CST6CDT,M3.2.0,M11.1.0"},
    {"America/New_York",    "EST5EDT,M3.2.0,M11.1.0"},
    {"America/Anchorage",   "AKST9AKDT,M3.2.0,M11.1.0"},
    {"Pacific/Honolulu",    "HST10"},
    {"Europe/London",       "GMT0BST,M3.5.0/1,M10.5.0"},
    {"Europe/Berlin",       "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Paris",        "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Madrid",       "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Asia/Tokyo",          "JST-9"},
    {"Asia/Shanghai",       "CST-8"},
    {"Asia/Kolkata",        "IST-5:30"},
    {"Australia/Sydney",    "AEST-10AEDT,M10.1.0,M4.1.0/3"},
    {"UTC",                 "UTC0"},
};

static bool s_connected, s_attempt, s_have_saved;
static volatile bool s_scanning;
static int s_attempt_failures;
static char s_ssid[33], s_pass[65];            // what we're connected / connecting to
static char s_saved_ssid[33], s_saved_pass[65]; // last known-good
static char s_ip[16];
static char s_server[160] = NOVA_SERVER_URL;

static void emit_status(const char *status)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "wifi_status");
    cJSON_AddStringToObject(o, "status", status);
    if (s_ssid[0]) {
        cJSON_AddStringToObject(o, "ssid", s_ssid);
    }
    if (strcmp(status, "connected") == 0) {
        cJSON_AddStringToObject(o, "ip", s_ip);
    }
    char *line = cJSON_PrintUnformatted(o);
    serial_emit_raw(line);
    cJSON_free(line);
    cJSON_Delete(o);
}

static void apply_and_connect(const char *ssid, const char *pass)
{
    wifi_config_t cfg = {0};
    strlcpy((char *)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid));
    strlcpy((char *)cfg.sta.password, pass, sizeof(cfg.sta.password));
    cfg.sta.threshold.authmode = pass[0] ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
    strlcpy(s_ssid, ssid, sizeof(s_ssid));
    strlcpy(s_pass, pass, sizeof(s_pass));
    esp_wifi_disconnect();
    esp_wifi_set_config(WIFI_IF_STA, &cfg);
    esp_wifi_connect();
    emit_status("connecting");
}

static void save_credentials(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, "ssid", s_ssid);
        nvs_set_str(h, "pass", s_pass);
        nvs_commit(h);
        nvs_close(h);
    }
    strlcpy(s_saved_ssid, s_ssid, sizeof(s_saved_ssid));
    strlcpy(s_saved_pass, s_pass, sizeof(s_saved_pass));
    s_have_saved = true;
}

static void apply_timezone(void)
{
    const char *posix = "UTC0";
    if (NOVA_TIMEZONE[0]) {
        bool found = false;
        for (size_t i = 0; i < sizeof(TZ_TABLE) / sizeof(TZ_TABLE[0]) && !found; i++) {
            if (strcmp(NOVA_TIMEZONE, TZ_TABLE[i].iana) == 0) {
                posix = TZ_TABLE[i].posix;
                found = true;
            }
        }
        if (!found && strchr(NOVA_TIMEZONE, '/') == NULL) {
            posix = NOVA_TIMEZONE;  // literal POSIX TZ string
            found = true;
        }
        if (!found) {
            ESP_LOGW(TAG, "unknown NOVA_TIMEZONE \"%s\" - falling back to UTC (add it to TZ_TABLE in net.c)", NOVA_TIMEZONE);
        }
    }
    setenv("TZ", posix, 1);
    tzset();
}

static void on_time_sync(struct timeval *tv)
{
    (void)tv;
    ESP_LOGI(TAG, "time synced");
    display_time_synced();
}

static void start_sntp(void)
{
    static bool started;
    if (started) {
        return;
    }
    started = true;
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    cfg.sync_cb = on_time_sync;
    esp_netif_sntp_init(&cfg);
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (s_ssid[0]) {
            esp_wifi_connect();
            emit_status("connecting");
        } else {
            emit_status("disconnected");
            ESP_LOGW(TAG, "no WiFi configured - open web/wifi-setup.html in Chrome to set one up");
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *d = data;
        ESP_LOGW(TAG, "\"%s\" disconnected, reason %d%s", s_ssid, d->reason,
                 d->reason == WIFI_REASON_NO_AP_FOUND ? " (network not in range)"
                 : d->reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT || d->reason == WIFI_REASON_AUTH_FAIL ? " (wrong password?)" : "");
        if (s_connected) {
            s_connected = false;
            nova_post(EV_NET_DOWN, NULL);
        }
        if (d->reason == WIFI_REASON_ASSOC_LEAVE) {
            return;  // our own disconnect before switching networks
        }
        if (s_attempt && ++s_attempt_failures >= ATTEMPT_MAX_FAILURES) {
            s_attempt = false;
            emit_status("failed");
            if (s_have_saved) {
                apply_and_connect(s_saved_ssid, s_saved_pass);
            }
            return;
        }
        if (s_ssid[0] && !s_scanning) {
            esp_wifi_connect();
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&e->ip_info.ip));
        s_connected = true;
        if (s_attempt || !s_have_saved || strcmp(s_ssid, s_saved_ssid) != 0) {
            save_credentials();
        }
        s_attempt = false;
        ESP_LOGI(TAG, "on \"%s\" as %s", s_ssid, s_ip);
        emit_status("connected");
        start_sntp();
        nova_post(EV_NET_UP, NULL);
    }
}

bool net_connected(void)
{
    return s_connected;
}

void net_report_status(void)
{
    emit_status(s_connected ? "connected" : s_attempt || s_ssid[0] ? "connecting" : "disconnected");
}

void net_connect(const char *ssid, const char *pass)
{
    s_attempt = true;
    s_attempt_failures = 0;
    apply_and_connect(ssid, pass ? pass : "");
}

void net_scan(void)
{
    // The driver refuses to scan mid-connect, and a board retrying a network
    // that isn't in range is always mid-connect - exactly when the setup page
    // needs a scan. Pause reconnecting for the scan.
    s_scanning = true;
    if (!s_connected) {
        esp_wifi_disconnect();
    }
    wifi_scan_config_t scan = {.show_hidden = false};
    esp_err_t err = esp_wifi_scan_start(&scan, true);
    s_scanning = false;
    if (!s_connected && s_ssid[0]) {
        esp_wifi_connect();
    }
    if (err != ESP_OK) {
        serial_emit_raw("{\"type\":\"wifi_scan_result\",\"networks\":[]}");
        return;
    }
    uint16_t n = 30;
    wifi_ap_record_t *aps = calloc(n, sizeof(wifi_ap_record_t));
    esp_wifi_scan_get_ap_records(&n, aps);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "wifi_scan_result");
    cJSON *list = cJSON_AddArrayToObject(o, "networks");
    for (int i = 0; i < n; i++) {
        const char *ssid = (const char *)aps[i].ssid;
        if (!ssid[0]) {
            continue;
        }
        bool dup = false;  // records come strongest first; keep the first of each name
        for (int j = 0; j < i && !dup; j++) {
            dup = strcmp(ssid, (const char *)aps[j].ssid) == 0;
        }
        if (dup) {
            continue;
        }
        cJSON *net = cJSON_CreateObject();
        cJSON_AddStringToObject(net, "ssid", ssid);
        cJSON_AddNumberToObject(net, "rssi", aps[i].rssi);
        cJSON_AddBoolToObject(net, "secure", aps[i].authmode != WIFI_AUTH_OPEN);
        cJSON_AddItemToArray(list, net);
    }
    free(aps);
    char *line = cJSON_PrintUnformatted(o);
    serial_emit_raw(line);
    cJSON_free(line);
    cJSON_Delete(o);
}

const char *nova_server_url(void)
{
    return s_server;
}

void nova_set_server_url(const char *url)
{
    strlcpy(s_server, url, sizeof(s_server));
    size_t len = strlen(s_server);
    while (len > 0 && s_server[len - 1] == '/') {
        s_server[--len] = 0;
    }
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, "server", s_server);
        nvs_commit(h);
        nvs_close(h);
    }
}

static void load_settings(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        size_t len = sizeof(s_saved_ssid);
        if (nvs_get_str(h, "ssid", s_saved_ssid, &len) == ESP_OK && s_saved_ssid[0]) {
            len = sizeof(s_saved_pass);
            nvs_get_str(h, "pass", s_saved_pass, &len);
            s_have_saved = true;
        }
        len = sizeof(s_server);
        nvs_get_str(h, "server", s_server, &len);
        nvs_close(h);
    }
    if (s_have_saved) {
        strlcpy(s_ssid, s_saved_ssid, sizeof(s_ssid));
        strlcpy(s_pass, s_saved_pass, sizeof(s_pass));
    } else if (strcmp(WIFI_SSID, "your-wifi-ssid") != 0) {
        strlcpy(s_ssid, WIFI_SSID, sizeof(s_ssid));
        strlcpy(s_pass, WIFI_PASS, sizeof(s_pass));
    }
}

esp_err_t net_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        return err;
    }
    load_settings();
    apply_timezone();

    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t ret = esp_wifi_init(&init);
    if (ret != ESP_OK) {
        return ret;
    }
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL);
    esp_wifi_set_mode(WIFI_MODE_STA);
    // Power save adds 100+ ms of receive latency, which a live voice
    // session can't afford.
    esp_wifi_set_ps(WIFI_PS_NONE);
    if (s_ssid[0]) {
        wifi_config_t cfg = {0};
        strlcpy((char *)cfg.sta.ssid, s_ssid, sizeof(cfg.sta.ssid));
        strlcpy((char *)cfg.sta.password, s_pass, sizeof(cfg.sta.password));
        esp_wifi_set_config(WIFI_IF_STA, &cfg);
    }
    return esp_wifi_start();
}
