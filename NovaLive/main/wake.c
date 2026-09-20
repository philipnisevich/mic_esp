// "Hey Nova" spotting with MultiNet7 in continuous command mode, the same
// approach MicScribe uses. WakeNet wake words are trained models and there
// is no free one for "Nova"; MultiNet takes the phrase as text.

#include <stdatomic.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mn_iface.h"
#include "esp_mn_models.h"
#include "esp_mn_speech_commands.h"
#include "model_path.h"
#include "nova.h"

#define TAG "wake"

#define CMD_WAKE 1

// Bare "nova" is left out: one short word false-accepts constantly.
// MultiNet7's grammar builder crashes when every phrase maps to one command
// id, so the list keeps two unused ones (found the hard way in MicScribe).
static const struct {
    int id;
    const char *phrase;
} COMMANDS[] = {
    {CMD_WAKE, "hey nova"},
    {CMD_WAKE, "hi nova"},
    {CMD_WAKE, "okay nova"},
    {2, "cancel that"},
    {3, "never mind"},
};

static const esp_mn_iface_t *s_mn;
static model_iface_data_t *s_mn_data;
static int s_chunk;
static StreamBufferHandle_t s_buf;
static atomic_bool s_reset;

void wake_feed(const int16_t *samples, int count)
{
    if (s_buf) {
        xStreamBufferSend(s_buf, samples, count * sizeof(int16_t), 0);
    }
}

void wake_reset(void)
{
    atomic_store(&s_reset, true);
}

static void wake_task(void *arg)
{
    int16_t *frame = heap_caps_malloc(s_chunk * sizeof(int16_t), MALLOC_CAP_INTERNAL);
    const size_t bytes = s_chunk * sizeof(int16_t);
    while (true) {
        size_t got = 0;
        while (got < bytes) {
            got += xStreamBufferReceive(s_buf, (uint8_t *)frame + got, bytes - got, portMAX_DELAY);
        }
        if (atomic_exchange(&s_reset, false)) {
            s_mn->clean(s_mn_data);
            xStreamBufferReset(s_buf);
            continue;
        }
        esp_mn_state_t state = s_mn->detect(s_mn_data, frame);
        if (state == ESP_MN_STATE_DETECTED) {
            esp_mn_results_t *r = s_mn->get_results(s_mn_data);
            if (r->num > 0) {
                ESP_LOGI(TAG, "heard \"%s\" (command %d, p=%.2f)", r->string, r->command_id[0], r->prob[0]);
                if (r->command_id[0] == CMD_WAKE) {
                    nova_post(EV_WAKE, NULL);
                }
            }
            // Detection stops after a hit until the model is cleaned.
            s_mn->clean(s_mn_data);
        } else if (state == ESP_MN_STATE_TIMEOUT) {
            s_mn->clean(s_mn_data);
        }
    }
}

esp_err_t wake_init(void)
{
    srmodel_list_t *models = esp_srmodel_init("model");
    char *name = models ? esp_srmodel_filter(models, ESP_MN_PREFIX, ESP_MN_ENGLISH) : NULL;
    if (name == NULL) {
        ESP_LOGE(TAG, "no English MultiNet model in the 'model' partition - was it flashed? (idf.py flash writes it)");
        return ESP_ERR_NOT_FOUND;
    }
    s_mn = esp_mn_handle_from_name(name);
    s_mn_data = s_mn->create(name, 6000);
    if (s_mn_data == NULL) {
        return ESP_FAIL;
    }
    esp_mn_commands_alloc(s_mn, s_mn_data);
    for (size_t i = 0; i < sizeof(COMMANDS) / sizeof(COMMANDS[0]); i++) {
        esp_mn_commands_add(COMMANDS[i].id, COMMANDS[i].phrase);
    }
    esp_mn_error_t *err = esp_mn_commands_update();
    if (err && err->num > 0) {
        ESP_LOGE(TAG, "%d wake phrase(s) rejected by %s", err->num, name);
    }
    s_chunk = s_mn->get_samp_chunksize(s_mn_data);
    s_buf = xStreamBufferCreateWithCaps(s_chunk * sizeof(int16_t) * 8, 1, MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG, "model %s, %d-sample chunks, listening for \"hey nova\"", name, s_chunk);
    xTaskCreatePinnedToCore(wake_task, "wake", 8 * 1024, NULL, 5, NULL, 1);
    return ESP_OK;
}
