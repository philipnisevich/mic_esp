// Line-oriented JSON over the USB serial port, same protocol as MicScribe:
// events go out as one JSON object per line, commands come in the same way.
// Log lines are plain text, which bridge.py already skips.

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_log.h"
#include "cJSON.h"
#include "nova.h"

#define TAG "serial"

void serial_emit_raw(const char *json_line)
{
    printf("%s\n", json_line);
    fflush(stdout);
}

void serial_emit(const char *type, const char *key, const char *value)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", type);
    if (key) {
        cJSON_AddStringToObject(o, key, value ? value : "");
    }
    char *line = cJSON_PrintUnformatted(o);
    serial_emit_raw(line);
    cJSON_free(line);
    cJSON_Delete(o);
}

static void handle_command(const char *line)
{
    cJSON *root = cJSON_Parse(line);
    const cJSON *cmd = cJSON_GetObjectItem(root, "cmd");
    if (!cJSON_IsString(cmd)) {
        cJSON_Delete(root);
        return;
    }
    const char *c = cmd->valuestring;
    if (strcmp(c, "wifi_scan") == 0) {
        net_scan();
    } else if (strcmp(c, "wifi_connect") == 0) {
        const cJSON *ssid = cJSON_GetObjectItem(root, "ssid");
        const cJSON *pass = cJSON_GetObjectItem(root, "pass");
        if (cJSON_IsString(ssid) && ssid->valuestring[0]) {
            net_connect(ssid->valuestring, cJSON_IsString(pass) ? pass->valuestring : "");
        }
    } else if (strcmp(c, "wifi_status") == 0) {
        net_report_status();
    } else if (strcmp(c, "set_server") == 0) {
        const cJSON *url = cJSON_GetObjectItem(root, "url");
        if (cJSON_IsString(url) && strncmp(url->valuestring, "http", 4) == 0) {
            nova_set_server_url(url->valuestring);
        }
        serial_emit("server", "url", nova_server_url());
    } else if (strcmp(c, "server") == 0) {
        serial_emit("server", "url", nova_server_url());
    } else if (strcmp(c, "info") == 0) {
        nova_report_info();
    } else if (strcmp(c, "display_test") == 0) {
        display_selftest();
    } else if (strcmp(c, "led_test") == 0) {
        led_demo();
    } else if (strcmp(c, "talk") == 0) {
        nova_post(EV_BUTTON, NULL);
    }
    cJSON_Delete(root);
}

static void serial_task(void *arg)
{
    static char line[512];
    while (true) {
        if (fgets(line, sizeof(line), stdin) == NULL) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        line[strcspn(line, "\r\n")] = 0;
        if (line[0] == '{') {
            handle_command(line);
        }
    }
}

void serial_init(void)
{
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    cfg.rx_buffer_size = 1024;
    if (usb_serial_jtag_driver_install(&cfg) == ESP_OK) {
        usb_serial_jtag_vfs_use_driver();  // blocking stdin reads instead of polling
    }
    setvbuf(stdin, NULL, _IONBF, 0);
    xTaskCreate(serial_task, "serial", 6 * 1024, NULL, 4, NULL);
}
