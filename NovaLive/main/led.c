// The dev board's RGB LED (a single WS2812), highest priority first:
//   pulsing yellow a background task is running (Claude working on a big job)
//   green          a background task finished and Nova hasn't mentioned it yet
//   blue           in a conversation, from the wake word until hang-up
// Task colours win even mid-conversation, so you can see a job start and finish
// while you keep talking; blue is what shows when no task needs your attention.

#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_strip.h"
#include "esp_log.h"
#include "config.h"
#include "nova.h"

#define TAG "led"

// The ESP32-S3-DevKitC-1 v1.0 has the LED on GPIO 48 and v1.1 on GPIO 38.
// Both are driven with the same colour; a pin with no LED on it is harmless.
static const int LED_GPIOS[] = {48, 38};
#define LED_COUNT (int)(sizeof(LED_GPIOS) / sizeof(LED_GPIOS[0]))

// The WS2812 is painfully bright at full scale; this is the peak channel value.
#ifndef NOVA_LED_BRIGHTNESS
#define NOVA_LED_BRIGHTNESS 40
#endif

#define TICK_MS       25
#define PULSE_TICKS   64  // 1.6 s per yellow pulse

static led_strip_handle_t s_strip[LED_COUNT];
static volatile bool s_session;
static volatile task_light_t s_task = TASK_LIGHT_NONE;
static volatile int s_force;  // led_demo(): 0 none, 1 blue, 2 yellow pulse, 3 green

static void show(uint8_t r, uint8_t g, uint8_t b)
{
    for (int i = 0; i < LED_COUNT; i++) {
        if (s_strip[i]) {
            led_strip_set_pixel(s_strip[i], 0, r, g, b);
            led_strip_refresh(s_strip[i]);
        }
    }
}

static void led_task(void *arg)
{
    uint32_t tick = 0;
    uint32_t last = 0xFFFFFFFF;
    while (true) {
        uint8_t r = 0, g = 0, b = 0;
        int force = s_force;
        bool pulse = force == 2 || (force == 0 && s_task == TASK_LIGHT_RUNNING);
        bool green = force == 3 || (force == 0 && s_task == TASK_LIGHT_DONE);
        bool blue = force == 1 || (force == 0 && s_session);
        if (pulse) {
            float phase = (float)(tick % PULSE_TICKS) / PULSE_TICKS;
            float k = 0.10f + 0.90f * (0.5f - 0.5f * cosf(2.0f * (float)M_PI * phase));
            r = (uint8_t)(NOVA_LED_BRIGHTNESS * k);
            g = (uint8_t)(NOVA_LED_BRIGHTNESS * 0.7f * k);
        } else if (green) {
            g = NOVA_LED_BRIGHTNESS;
        } else if (blue) {
            b = NOVA_LED_BRIGHTNESS;
        }
        uint32_t packed = (uint32_t)r << 16 | (uint32_t)g << 8 | b;
        if (packed != last) {
            show(r, g, b);
            last = packed;
        }
        tick++;
        vTaskDelay(pdMS_TO_TICKS(TICK_MS));
    }
}

void led_set_session(bool active)
{
    s_session = active;
}

void led_set_task(task_light_t light)
{
    s_task = light;
}

static void demo_task(void *arg)
{
    s_force = 1;
    vTaskDelay(pdMS_TO_TICKS(1500));
    s_force = 2;
    vTaskDelay(pdMS_TO_TICKS(3500));
    s_force = 3;
    vTaskDelay(pdMS_TO_TICKS(1500));
    s_force = 0;
    vTaskDelete(NULL);
}

void led_demo(void)
{
    xTaskCreate(demo_task, "led_demo", 2048, NULL, 3, NULL);
}

void led_init(void)
{
    int ok = 0;
    for (int i = 0; i < LED_COUNT; i++) {
        led_strip_config_t strip = {
            .strip_gpio_num = LED_GPIOS[i],
            .max_leds = 1,
            .led_model = LED_MODEL_WS2812,
            .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        };
        led_strip_rmt_config_t rmt = {
            .clk_src = RMT_CLK_SRC_DEFAULT,
            .resolution_hz = 10 * 1000 * 1000,
        };
        if (led_strip_new_rmt_device(&strip, &rmt, &s_strip[i]) == ESP_OK) {
            ok++;
        } else {
            s_strip[i] = NULL;
            ESP_LOGW(TAG, "no LED driver on GPIO %d", LED_GPIOS[i]);
        }
    }
    if (ok == 0) {
        return;
    }
    // Power-on self-test so you can see the LED works: red, green, blue.
    const uint8_t B = NOVA_LED_BRIGHTNESS;
    show(B, 0, 0);
    vTaskDelay(pdMS_TO_TICKS(180));
    show(0, B, 0);
    vTaskDelay(pdMS_TO_TICKS(180));
    show(0, 0, B);
    vTaskDelay(pdMS_TO_TICKS(180));
    show(0, 0, 0);
    ESP_LOGI(TAG, "LED on GPIO %d/%d", LED_GPIOS[0], LED_GPIOS[1]);
    xTaskCreate(led_task, "led", 3 * 1024, NULL, 2, NULL);
}
