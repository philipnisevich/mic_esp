// Round 1.28" TFT (GC9A01, 240x240, SPI), driven through LVGL via
// esp_lvgl_port. Two screens, swapped by display_orb_state():
//   clock  - minimalist HH:MM, shown at rest (no network / waiting for the
//            wake word).
//   orb    - a rotating gradient ring + soft glow, one look per state, from
//            the wake word through the end of the conversation. Styled
//            after NovaOrb.tsx in the seva7747/Nova web frontend this
//            firmware already mirrors (see main.c).
// Optional: nothing breaks if it's not wired up - SPI is write-only, though,
// so (unlike the OLED's I2C probe) there's no way to detect that and skip
// LVGL; it just renders to a panel that isn't there. There's also no room
// for the live transcript on a 240x240 round face, so display_text() is a
// no-op here - the serial log and bridge.py still carry the full transcript.

#include <stdio.h>
#include <string.h>
#include <time.h>
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_gc9a01.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include "nova.h"

#define TAG "display"

#define LCD_HOST     SPI2_HOST
#define LCD_W        240
#define LCD_H        240
#define LCD_PCLK_HZ  (40 * 1000 * 1000)  // 80 MHz default is tight over dupont wires

#define COL_BG      lv_color_hex(0x000000)
#define COL_TEXT    lv_color_hex(0xE5E7EB)
#define COL_RING    lv_color_hex(0x2A2F3A)
#define COL_AMBER   lv_color_hex(0xFBBF24)
#define COL_GREEN   lv_color_hex(0x34D399)

static bool s_ready;
static lv_disp_t *s_disp;
static lv_obj_t *s_clock_screen, *s_clock_label, *s_clock_task_dot;
static lv_obj_t *s_orb_screen, *s_orb_glow, *s_orb_ring, *s_orb_task_ring, *s_orb_label;
static nova_display_state_t s_state = DISP_CLOCK;
static task_light_t s_task = TASK_LIGHT_NONE;
static char s_label[40] = "";
static volatile bool s_time_synced;

void display_time_synced(void)  // called by net.c once SNTP lands the first fix
{
    s_time_synced = true;
}

// -------------------------------------------------------------- per-state orb look ---
// Ring rotation period, indicator arc span (degrees), and a representative
// colour per state - approximating NovaOrb.tsx's conic-gradient chase ring
// (arcWidth is that component's "% of the circle lit up").
typedef struct {
    uint32_t period_ms;
    int16_t  arc_deg;
    lv_color_t color;
} orb_look_t;

static const orb_look_t ORB_LOOK[] = {
    [DISP_CONNECTING] = {850,  198, {.blue = 0xFA, .green = 0xA5, .red = 0x60}},  // #60a5fa
    [DISP_LIVE_IDLE]  = {4000, 108, {.blue = 0x99, .green = 0xD3, .red = 0x34}},  // #34d399
    [DISP_RECORDING]  = {2100, 115, {.blue = 0xD4, .green = 0xEA, .red = 0x5E}},  // #5eead4
    [DISP_THINKING]   = {650,  79,  {.blue = 0xFA, .green = 0x8B, .red = 0xA7}},  // #a78bfa
    [DISP_SPEAKING]   = {3200, 162, {.blue = 0xFA, .green = 0xA5, .red = 0x60}},  // #60a5fa
};

// -------------------------------------------------------------------- anim helpers ---
static void set_opa(void *obj, int32_t v)
{
    lv_obj_set_style_opa(obj, (lv_opa_t)v, LV_PART_MAIN);
}

static void set_arc_opa(void *obj, int32_t v)
{
    lv_obj_set_style_arc_opa(obj, (lv_opa_t)v, LV_PART_INDICATOR);
}

static void breathe_anim(lv_obj_t *obj)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_exec_cb(&a, set_opa);
    lv_anim_set_values(&a, 70, 160);
    lv_anim_set_time(&a, 2200);
    lv_anim_set_playback_time(&a, 2200);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&a);
}

static void pulse_anim(lv_obj_t *obj, lv_anim_exec_xcb_t exec_cb)
{
    lv_anim_delete(obj, NULL);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_exec_cb(&a, exec_cb);
    lv_anim_set_values(&a, 40, 255);
    lv_anim_set_time(&a, 800);
    lv_anim_set_playback_time(&a, 800);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&a);
}

// ------------------------------------------------------------------- task light ---
// Same priority rule as led.c: amber pulse while a background task runs,
// steady green once it's done, invisible otherwise. Applied to both
// screens' indicators unconditionally, so whichever one is on screen is
// already right.
void display_task_light(task_light_t light)
{
    if (!s_ready || light == s_task) {
        return;
    }
    s_task = light;
    lvgl_port_lock(0);
    lv_anim_delete(s_clock_task_dot, NULL);
    lv_anim_delete(s_orb_task_ring, NULL);
    if (light == TASK_LIGHT_NONE) {
        lv_obj_set_style_opa(s_clock_task_dot, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_arc_opa(s_orb_task_ring, LV_OPA_TRANSP, LV_PART_INDICATOR);
    } else if (light == TASK_LIGHT_RUNNING) {
        lv_obj_set_style_bg_color(s_clock_task_dot, COL_AMBER, LV_PART_MAIN);
        lv_obj_set_style_arc_color(s_orb_task_ring, COL_AMBER, LV_PART_INDICATOR);
        pulse_anim(s_clock_task_dot, set_opa);
        pulse_anim(s_orb_task_ring, set_arc_opa);
    } else {  // TASK_LIGHT_DONE
        lv_obj_set_style_bg_color(s_clock_task_dot, COL_GREEN, LV_PART_MAIN);
        lv_obj_set_style_arc_color(s_orb_task_ring, COL_GREEN, LV_PART_INDICATOR);
        lv_obj_set_style_opa(s_clock_task_dot, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_arc_opa(s_orb_task_ring, LV_OPA_COVER, LV_PART_INDICATOR);
    }
    lvgl_port_unlock();
}

// ------------------------------------------------------------------------- clock ---
static void clock_tick_cb(lv_timer_t *timer)
{
    (void)timer;
    if (s_state != DISP_CLOCK) {
        return;
    }
    char buf[8];
    if (!s_time_synced) {
        strlcpy(buf, "--:--", sizeof(buf));
    } else {
        time_t now = time(NULL);
        struct tm t;
        localtime_r(&now, &t);
        snprintf(buf, sizeof(buf), t.tm_sec % 2 ? "%02d:%02d" : "%02d %02d", t.tm_hour, t.tm_min);
    }
    lv_label_set_text(s_clock_label, buf);
}

// --------------------------------------------------------------------------- orb ---
void display_orb_state(nova_display_state_t st)
{
    if (!s_ready || st == s_state) {
        return;
    }
    s_state = st;
    lvgl_port_lock(0);
    if (st == DISP_CLOCK) {
        lv_disp_load_scr(s_clock_screen);
    } else {
        const orb_look_t *look = &ORB_LOOK[st];
        lv_label_set_text(s_orb_label, s_label);
        lv_arc_set_angles(s_orb_ring, 0, look->arc_deg);
        lv_obj_set_style_arc_color(s_orb_ring, look->color, LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(s_orb_glow, look->color, LV_PART_MAIN);
        lv_arc_set_rotation(s_orb_ring, 0);
        lv_anim_delete(s_orb_ring, NULL);
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, s_orb_ring);
        lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_arc_set_rotation);
        lv_anim_set_values(&a, 0, 360);
        lv_anim_set_time(&a, look->period_ms);
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_set_path_cb(&a, lv_anim_path_linear);
        lv_anim_start(&a);
        lv_disp_load_scr(s_orb_screen);
    }
    lvgl_port_unlock();
}

void display_state(const char *label)
{
    ESP_LOGI(TAG, "[%s]", label);
    strlcpy(s_label, label, sizeof(s_label));
    if (!s_ready || s_state == DISP_CLOCK) {
        return;
    }
    lvgl_port_lock(0);
    lv_label_set_text(s_orb_label, s_label);
    lvgl_port_unlock();
}

// A 240x240 round face has no room for a scrolling transcript the way the
// OLED did - the serial log (and bridge.py) carry the full text instead.
void display_text(nova_role_t role, const char *text)
{
    (void)role;
    (void)text;
}

void display_selftest(void)
{
    display_state("Display test");
    char line[64];
    snprintf(line, sizeof(line), "{\"type\":\"display_test\",\"panel\":%s}", s_ready ? "true" : "false");
    serial_emit_raw(line);
}

// ------------------------------------------------------------------------- setup ---
static lv_obj_t *make_screen(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, COL_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(scr, 0, LV_PART_MAIN);
    lv_obj_set_scrollable(scr, false);
    return scr;
}

static void build_clock_screen(void)
{
    s_clock_screen = make_screen();

    lv_obj_t *ring = lv_obj_create(s_clock_screen);
    lv_obj_remove_style_all(ring);
    lv_obj_set_size(ring, 224, 224);
    lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ring, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(ring, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(ring, COL_RING, LV_PART_MAIN);
    lv_obj_set_style_border_opa(ring, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_center(ring);

    s_clock_label = lv_label_create(s_clock_screen);
    lv_obj_set_style_text_font(s_clock_label, &lv_font_montserrat_48, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_clock_label, COL_TEXT, LV_PART_MAIN);
    lv_label_set_text(s_clock_label, "--:--");
    lv_obj_center(s_clock_label);

    s_clock_task_dot = lv_obj_create(s_clock_screen);
    lv_obj_remove_style_all(s_clock_task_dot);
    lv_obj_set_size(s_clock_task_dot, 14, 14);
    lv_obj_set_style_radius(s_clock_task_dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_clock_task_dot, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_align(s_clock_task_dot, LV_ALIGN_BOTTOM_MID, 0, -22);
}

static lv_obj_t *make_ring(lv_obj_t *parent, int size, int width)
{
    lv_obj_t *arc = lv_arc_create(parent);
    lv_obj_remove_style(arc, NULL, LV_PART_KNOB);
    lv_obj_set_clickable(arc, false);
    lv_obj_set_size(arc, size, size);
    lv_obj_set_style_arc_opa(arc, LV_OPA_TRANSP, LV_PART_MAIN);  // hide the background track
    lv_obj_set_style_arc_width(arc, width, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(arc, true, LV_PART_INDICATOR);
    lv_obj_center(arc);
    return arc;
}

static void build_orb_screen(void)
{
    s_orb_screen = make_screen();

    s_orb_glow = lv_obj_create(s_orb_screen);
    lv_obj_remove_style_all(s_orb_glow);
    lv_obj_set_size(s_orb_glow, 170, 170);
    lv_obj_set_style_radius(s_orb_glow, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_orb_glow, 100, LV_PART_MAIN);
    lv_obj_center(s_orb_glow);
    breathe_anim(s_orb_glow);

    s_orb_task_ring = make_ring(s_orb_screen, 232, 6);
    lv_arc_set_angles(s_orb_task_ring, 0, 360);
    lv_obj_set_style_arc_opa(s_orb_task_ring, LV_OPA_TRANSP, LV_PART_INDICATOR);

    s_orb_ring = make_ring(s_orb_screen, 216, 14);
    lv_arc_set_angles(s_orb_ring, 0, 108);

    s_orb_label = lv_label_create(s_orb_screen);
    lv_obj_set_style_text_font(s_orb_label, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_orb_label, COL_TEXT, LV_PART_MAIN);
    lv_obj_center(s_orb_label);
}

static esp_err_t panel_init(esp_lcd_panel_handle_t *panel, esp_lcd_panel_io_handle_t *io)
{
    spi_bus_config_t bus = GC9A01_PANEL_BUS_SPI_CONFIG(PIN_TFT_SCLK, PIN_TFT_MOSI, LCD_W * 80 * sizeof(uint16_t));
    esp_err_t err = spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        return err;
    }
    esp_lcd_panel_io_spi_config_t io_cfg = GC9A01_PANEL_IO_SPI_CONFIG(PIN_TFT_CS, PIN_TFT_DC, NULL, NULL);
    io_cfg.pclk_hz = LCD_PCLK_HZ;
    err = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_cfg, io);
    if (err != ESP_OK) {
        return err;
    }
    esp_lcd_panel_dev_config_t dev = {
        .reset_gpio_num = PIN_TFT_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    err = esp_lcd_new_panel_gc9a01(*io, &dev, panel);
    if (err != ESP_OK) {
        return err;
    }
    esp_lcd_panel_reset(*panel);
    esp_lcd_panel_init(*panel);
    esp_lcd_panel_disp_on_off(*panel, true);
    return ESP_OK;
}

void display_init(void)
{
    gpio_config_t bl = {.pin_bit_mask = 1ULL << PIN_TFT_BL, .mode = GPIO_MODE_OUTPUT};
    gpio_config(&bl);
    gpio_set_level(PIN_TFT_BL, 0);

    esp_lcd_panel_handle_t panel = NULL;
    esp_lcd_panel_io_handle_t io = NULL;
    if (panel_init(&panel, &io) != ESP_OK) {
        ESP_LOGW(TAG, "no round display found - check wiring on SCLK=%d MOSI=%d CS=%d DC=%d RST=%d",
                 PIN_TFT_SCLK, PIN_TFT_MOSI, PIN_TFT_CS, PIN_TFT_DC, PIN_TFT_RST);
        return;
    }

    const lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    if (lvgl_port_init(&port_cfg) != ESP_OK) {
        ESP_LOGW(TAG, "lvgl_port_init failed");
        return;
    }
    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = io,
        .panel_handle = panel,
        .buffer_size = LCD_W * 60,
        .double_buffer = false,
        .hres = LCD_W,
        .vres = LCD_H,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags = {.buff_dma = true, .swap_bytes = true},
    };
    s_disp = lvgl_port_add_disp(&disp_cfg);
    if (s_disp == NULL) {
        ESP_LOGW(TAG, "lvgl_port_add_disp failed");
        return;
    }

    lvgl_port_lock(0);
    build_clock_screen();
    build_orb_screen();
    lv_disp_load_scr(s_clock_screen);
    lvgl_port_unlock();
    lv_timer_create(clock_tick_cb, 500, NULL);

    gpio_set_level(PIN_TFT_BL, 1);
    s_ready = true;
    ESP_LOGI(TAG, "round display up on SPI2, backlight GPIO %d", PIN_TFT_BL);
}
