// Background-task indicator, polled the same way the web app does
// (useNovaConversation.ts): plain HTTP, no live session needed, so a task
// that finishes after the session hung up still shows up.

#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "cJSON.h"
#include "config.h"
#include "nova.h"

#define TAG "tasks"
#define POLL_MS 2500

static char *fetch_status(void)
{
    char url[200];
    snprintf(url, sizeof(url), "%s/api/tasks/status", nova_server_url());
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 4000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    esp_http_client_set_header(c, "Authorization", "Bearer " NOVA_DEVICE_TOKEN);
    char *body = NULL;
    if (esp_http_client_open(c, 0) == ESP_OK) {
        int len = esp_http_client_fetch_headers(c);
        if (esp_http_client_get_status_code(c) == 200) {
            int cap = len > 0 ? len : 1024;
            body = calloc(1, cap + 1);
            int got = 0, n;
            while (body && got < cap && (n = esp_http_client_read(c, body + got, cap - got)) > 0) {
                got += n;
            }
        }
    }
    esp_http_client_cleanup(c);
    return body;
}

static void poll_task(void *arg)
{
    bool was_done = false;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
        if (!net_connected()) {
            continue;
        }
        char *body = fetch_status();
        cJSON *root = body ? cJSON_Parse(body) : NULL;
        free(body);
        if (root == NULL) {
            continue;
        }
        bool active = cJSON_IsTrue(cJSON_GetObjectItem(root, "active"));
        bool done = cJSON_IsTrue(cJSON_GetObjectItem(root, "done"));
        const cJSON *result = cJSON_GetObjectItem(root, "result");
        // Yellow while anything runs; green once finished until Nova
        // mentions it (the backend clears it on the next question).
        task_light_t light = active ? TASK_LIGHT_RUNNING : done ? TASK_LIGHT_DONE : TASK_LIGHT_NONE;
        display_task_light(light);
        led_set_task(light);
        if (done && !was_done && cJSON_IsString(result)) {
            ESP_LOGI(TAG, "finished in the background: %s", result->valuestring);
            serial_emit("task_done", "text", result->valuestring);
        }
        was_done = done;
        cJSON_Delete(root);
    }
}

void tasks_poll_start(void)
{
    xTaskCreate(poll_task, "tasks", 6 * 1024, NULL, 3, NULL);
}
