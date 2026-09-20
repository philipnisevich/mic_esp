// 0.96" SSD1306 (128x64, I2C): an inverted header with Nova's state - the
// orb, in words - over the live transcript. Optional: everything works
// without a display attached.

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "driver/i2c_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_ssd1306.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "font5x7.h"
#include "nova.h"

#define TAG "display"

#define W 128
#define H 64
#define COLS (W / 6)
#define ROWS (H / 8)
#define BODY_ROWS (ROWS - 1)

static esp_lcd_panel_handle_t s_panel;
static SemaphoreHandle_t s_lock;
static uint8_t s_fb[W * H / 8];
static char s_state[COLS + 1] = "";
static char s_body[BODY_ROWS][COLS + 1];
static task_light_t s_task = TASK_LIGHT_NONE;
static int s_draw_errors;

static void draw_char(int col, int row, char c, bool invert)
{
    if (c < 32 || c > 126) {
        c = '?';
    }
    uint8_t *p = &s_fb[row * W + col * 6];
    for (int i = 0; i < 5; i++) {
        p[i] = invert ? ~FONT5X7[c - 32][i] : FONT5X7[c - 32][i];
    }
    p[5] = invert ? 0xFF : 0x00;
}

static void draw_text(int row, const char *text, bool invert)
{
    for (int col = 0; col < COLS; col++) {
        draw_char(col, row, *text ? *text++ : ' ', invert);
    }
    if (invert) {  // fill the 2 px right of the last column
        s_fb[row * W + W - 2] = s_fb[row * W + W - 1] = 0xFF;
    }
}

static void flush(void)
{
    if (s_panel == NULL) {
        return;
    }
    char header[COLS + 1];
    const char *light = s_task == TASK_LIGHT_RUNNING ? "BG" : s_task == TASK_LIGHT_DONE ? "OK" : "";
    snprintf(header, sizeof(header), " %-*.*s%s", COLS - 1 - (int)strlen(light), COLS - 1 - (int)strlen(light), s_state, light);
    draw_text(0, header, true);
    for (int r = 0; r < BODY_ROWS; r++) {
        draw_text(r + 1, s_body[r], false);
    }
    esp_err_t err = esp_lcd_panel_draw_bitmap(s_panel, 0, 0, W, H, s_fb);
    if (err != ESP_OK) {
        static int64_t last_log_us;
        s_draw_errors++;
        if (esp_timer_get_time() - last_log_us > 1000000) {
            ESP_LOGW(TAG, "OLED draw failed (%d so far): %s", s_draw_errors, esp_err_to_name(err));
            last_log_us = esp_timer_get_time();
        }
    }
}

// UTF-8 punctuation GPT-Live likes (curly quotes, dashes) -> ASCII.
static void to_ascii(const char *in, char *out, size_t cap)
{
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p && o + 1 < cap; p++) {
        if (*p < 0x80) {
            out[o++] = *p == '\n' ? ' ' : *p;
        } else if (p[0] == 0xE2 && p[1] == 0x80 && p[2]) {
            uint8_t k = p[2];
            out[o++] = (k == 0x98 || k == 0x99) ? '\'' : (k == 0x9C || k == 0x9D) ? '"' : (k == 0x93 || k == 0x94) ? '-' : (k == 0xA6) ? '.' : '?';
            p += 2;
        } else if ((*p & 0xC0) == 0xC0) {
            out[o++] = '?';
            while ((p[1] & 0xC0) == 0x80) {
                p++;
            }
        }
    }
    out[o] = 0;
}

// Word-wrap and keep the last BODY_ROWS lines: text streams in, so the
// newest words are the ones worth showing.
static void layout(const char *text)
{
    char ring[BODY_ROWS][COLS + 1];
    char cur[COLS + 1];
    int count = 0, len = 0;
    const char *p = text;

#define PUSH_LINE()                                               \
    do {                                                          \
        cur[len] = 0;                                             \
        strlcpy(ring[count % BODY_ROWS], cur, sizeof(ring[0]));   \
        count++;                                                  \
        len = 0;                                                  \
    } while (0)

    while (*p) {
        while (*p == ' ') {
            p++;
        }
        const char *w = p;
        while (*p && *p != ' ') {
            p++;
        }
        int wl = p - w;
        while (wl > 0) {
            if (len && wl + 1 > COLS - len) {
                PUSH_LINE();  // word doesn't fit after what's there: new line
                continue;
            }
            if (len) {
                cur[len++] = ' ';
            }
            int take = wl < COLS - len ? wl : COLS - len;  // hard-break words longer than a line
            memcpy(cur + len, w, take);
            len += take;
            w += take;
            wl -= take;
            if (wl > 0) {
                PUSH_LINE();
            }
        }
    }
    if (len) {
        PUSH_LINE();
    }
#undef PUSH_LINE

    int first = count > BODY_ROWS ? count - BODY_ROWS : 0;
    for (int r = 0; r < BODY_ROWS; r++) {
        int i = first + r;
        strlcpy(s_body[r], i < count ? ring[i % BODY_ROWS] : "", sizeof(s_body[r]));
    }
}

void display_state(const char *label)
{
    ESP_LOGI(TAG, "[%s]", label);
    if (s_lock == NULL) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(s_state, label, sizeof(s_state));
    flush();
    xSemaphoreGive(s_lock);
}

void display_text(nova_role_t role, const char *text)
{
    if (s_lock == NULL) {
        return;
    }
    static char buf[700];
    const char *prefix = role == ROLE_USER ? "You: " : role == ROLE_SYSTEM ? "! " : "";
    char ascii[640];
    to_ascii(text, ascii, sizeof(ascii));
    snprintf(buf, sizeof(buf), "%s%s", prefix, ascii);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    layout(buf);
    flush();
    xSemaphoreGive(s_lock);
}

void display_task_light(task_light_t light)
{
    if (s_lock == NULL || light == s_task) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_task = light;
    flush();
    xSemaphoreGive(s_lock);
}

void display_selftest(void)
{
    display_state("Display test");
    display_text(ROLE_NOVA, "Row two of the transcript area. This text wraps across several rows so all seven body rows fill up: one two three four five six seven eight nine ten eleven twelve thirteen.");
    char line[96];
    snprintf(line, sizeof(line), "{\"type\":\"display_test\",\"oled\":%s,\"draw_errors\":%d}", s_panel ? "true" : "false", s_draw_errors);
    serial_emit_raw(line);
}

void display_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    i2c_master_bus_handle_t bus;
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = PIN_I2C_SDA,
        .scl_io_num = PIN_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    if (i2c_new_master_bus(&bus_cfg, &bus) != ESP_OK) {
        ESP_LOGW(TAG, "I2C bus init failed");
        return;
    }
    uint8_t addr = 0;
    if (i2c_master_probe(bus, 0x3C, 50) == ESP_OK) {
        addr = 0x3C;
    } else if (i2c_master_probe(bus, 0x3D, 50) == ESP_OK) {
        addr = 0x3D;
    }
    if (addr == 0) {
        ESP_LOGW(TAG, "no OLED found at 0x3C/0x3D - check SDA=%d SCL=%d and 3V3", PIN_I2C_SDA, PIN_I2C_SCL);
        return;
    }
    esp_lcd_panel_io_handle_t io;
    esp_lcd_panel_io_i2c_config_t io_cfg = {
        .dev_addr = addr,
        .scl_speed_hz = 400000,
        .control_phase_bytes = 1,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .dc_bit_offset = 6,
    };
    esp_lcd_panel_ssd1306_config_t ssd = {.height = H};
    esp_lcd_panel_dev_config_t dev = {
        .bits_per_pixel = 1,
        .reset_gpio_num = -1,
        .vendor_config = &ssd,
    };
    if (esp_lcd_new_panel_io_i2c(bus, &io_cfg, &io) != ESP_OK ||
        esp_lcd_new_panel_ssd1306(io, &dev, &s_panel) != ESP_OK) {
        s_panel = NULL;
        return;
    }
    esp_lcd_panel_reset(s_panel);
    esp_lcd_panel_init(s_panel);
    esp_lcd_panel_disp_on_off(s_panel, true);
    ESP_LOGI(TAG, "OLED at 0x%02X", addr);
}
