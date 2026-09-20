// Microphone (INMP441), speaker (MAX98357A) and the echo canceller between
// them. One task owns the mic: while idle its audio goes to the wake word
// detector, during a session it goes through AEC and on to GPT-Live-1.

#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"
#include "freertos/semphr.h"
#include "driver/i2s_std.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "esp_afe_aec.h"
#include "esp_capture_types.h"
#include "config.h"
#include "nova.h"

#define TAG "audio"

// Tuned on this mic in MicScribe: 10.0 clipped on normal speech.
#define MIC_GAIN   4.0f
#define HPF_ALPHA  0.995f  // ~13 Hz high-pass, strips the INMP441's DC offset

// The MAX98357A will not run at 16 kHz reliably with 16-bit slots, and
// MicScribe found 48 kHz / 32-bit stereo slots to be the combination that
// works. Decoded audio is 16 kHz, so it is upsampled 3x.
#define SPK_RATE     48000
#define SPK_UPSAMPLE (SPK_RATE / NOVA_SAMPLE_RATE)

// The speaker reference reaches the echo canceller before the echo reaches
// the mic (it is taken as audio is queued to I2S, not as it leaves the
// cone). Holding it back this long keeps the lead inside AEC's ~64 ms
// filter. Raise it if echo leaks through; lower it if AEC seems to do
// nothing at all.
#define AEC_REF_DELAY_MS 20
#define AEC_FILTER_LEN   4

// Half-duplex fallback: mic is muted until this long after the last
// speaker sample, so the tail of Nova's own voice does not trigger barge-in.
#define HALF_DUPLEX_TAIL_MS 300

#define REF_RING_SAMPLES     (NOVA_SAMPLE_RATE)       // 1 s
#define CAPTURE_BUFFER_BYTES (NOVA_SAMPLE_RATE * 2)   // 1 s of 16-bit mono

static i2s_chan_handle_t s_rx, s_tx;
static volatile audio_mode_t s_mode = AUDIO_MODE_WAKE;
static afe_aec_handle_t *s_aec;
static int s_chunk;  // samples per mic read, = AEC chunk size

// ----------------------------------------------------- speaker reference ---
static int16_t *s_ref;
static int s_ref_head, s_ref_len;
static int64_t s_ref_last_write_us;
static int64_t s_speaker_busy_until_us;
static portMUX_TYPE s_ref_lock = portMUX_INITIALIZER_UNLOCKED;

static void ref_reset(void)
{
    portENTER_CRITICAL(&s_ref_lock);
    s_ref_head = 0;
    s_ref_len = 0;
    portEXIT_CRITICAL(&s_ref_lock);
}

static void ref_push(const int16_t *pcm, int n)
{
    portENTER_CRITICAL(&s_ref_lock);
    for (int i = 0; i < n; i++) {
        int tail = (s_ref_head + s_ref_len) % REF_RING_SAMPLES;
        s_ref[tail] = pcm[i];
        if (s_ref_len < REF_RING_SAMPLES) {
            s_ref_len++;
        } else {
            s_ref_head = (s_ref_head + 1) % REF_RING_SAMPLES;
        }
    }
    s_ref_last_write_us = esp_timer_get_time();
    portEXIT_CRITICAL(&s_ref_lock);
}

static __attribute__((unused)) void ref_pop(int16_t *out, int n)
{
    const int hold = NOVA_SAMPLE_RATE * AEC_REF_DELAY_MS / 1000;
    portENTER_CRITICAL(&s_ref_lock);
    bool draining = esp_timer_get_time() - s_ref_last_write_us > 3 * AEC_REF_DELAY_MS * 1000;
    int take = 0;
    if (s_ref_len >= n + hold) {
        take = n;
    } else if (draining) {
        take = s_ref_len < n ? s_ref_len : n;
    }
    for (int i = 0; i < take; i++) {
        out[i] = s_ref[s_ref_head];
        s_ref_head = (s_ref_head + 1) % REF_RING_SAMPLES;
    }
    s_ref_len -= take;
    portEXIT_CRITICAL(&s_ref_lock);
    if (take < n) {
        memset(out + take, 0, (n - take) * sizeof(int16_t));
    }
}

// --------------------------------------------------------- capture source ---
// esp_capture pulls PCM from here and Opus-encodes it for the peer connection.
typedef struct {
    esp_capture_audio_src_if_t base;
    esp_capture_audio_info_t   info;
    StreamBufferHandle_t       buf;
    uint64_t                   frames;
    volatile bool              started;
    volatile bool              abort;
} mic_src_t;

static mic_src_t s_src;

static esp_capture_err_t src_open(esp_capture_audio_src_if_t *h)
{
    return ESP_CAPTURE_ERR_OK;
}

static esp_capture_err_t src_get_codecs(esp_capture_audio_src_if_t *h, const esp_capture_format_id_t **codecs, uint8_t *num)
{
    static const esp_capture_format_id_t pcm[] = {ESP_CAPTURE_FMT_ID_PCM};
    *codecs = pcm;
    *num = 1;
    return ESP_CAPTURE_ERR_OK;
}

static esp_capture_err_t src_set_fixed_caps(esp_capture_audio_src_if_t *h, const esp_capture_audio_info_t *caps)
{
    return ESP_CAPTURE_ERR_OK;
}

static esp_capture_err_t src_negotiate(esp_capture_audio_src_if_t *h, esp_capture_audio_info_t *in, esp_capture_audio_info_t *out)
{
    if (in->format_id != ESP_CAPTURE_FMT_ID_PCM) {
        return ESP_CAPTURE_ERR_NOT_SUPPORTED;
    }
    // Only one format comes out of the mic pipeline; the capture path
    // resamples/converts from here if the encoder wants something else.
    s_src.info = (esp_capture_audio_info_t){
        .format_id = ESP_CAPTURE_FMT_ID_PCM,
        .sample_rate = NOVA_SAMPLE_RATE,
        .channel = 1,
        .bits_per_sample = 16,
    };
    *out = s_src.info;
    return ESP_CAPTURE_ERR_OK;
}

static esp_capture_err_t src_start(esp_capture_audio_src_if_t *h)
{
    xStreamBufferReset(s_src.buf);
    s_src.frames = 0;
    s_src.abort = false;
    s_src.started = true;
    return ESP_CAPTURE_ERR_OK;
}

static esp_capture_err_t src_read(esp_capture_audio_src_if_t *h, esp_capture_stream_frame_t *frame)
{
    int got = 0;
    while (got < frame->size) {
        if (s_src.abort || !s_src.started) {
            return ESP_CAPTURE_ERR_NOT_SUPPORTED;
        }
        got += xStreamBufferReceive(s_src.buf, frame->data + got, frame->size - got, pdMS_TO_TICKS(50));
    }
    int samples = frame->size / 2;
    frame->pts = (uint32_t)(s_src.frames * 1000 / NOVA_SAMPLE_RATE);
    s_src.frames += samples;
    return ESP_CAPTURE_ERR_OK;
}

static esp_capture_err_t src_abort(esp_capture_audio_src_if_t *h)
{
    s_src.abort = true;
    return ESP_CAPTURE_ERR_OK;
}

static esp_capture_err_t src_stop(esp_capture_audio_src_if_t *h)
{
    s_src.started = false;
    return ESP_CAPTURE_ERR_OK;
}

static esp_capture_err_t src_close(esp_capture_audio_src_if_t *h)
{
    return ESP_CAPTURE_ERR_OK;
}

esp_capture_audio_src_if_t *audio_capture_src(void)
{
    return &s_src.base;
}

// --------------------------------------------------------- speaker render ---
// av_render decodes GPT-Live-1's Opus and hands PCM to these ops.
static av_render_audio_frame_info_t s_render_info;
static int16_t s_last_sample;
static const float s_volume = NOVA_VOLUME / 100.0f;

static audio_render_handle_t render_init(void *cfg, int cfg_size)
{
    return (audio_render_handle_t)&s_render_info;
}

static int render_open(audio_render_handle_t h, av_render_audio_frame_info_t *info)
{
    if (info->sample_rate != NOVA_SAMPLE_RATE || info->bits_per_sample != 16) {
        ESP_LOGE(TAG, "speaker got %d Hz / %d bit; expected %d Hz / 16 bit",
                 (int)info->sample_rate, info->bits_per_sample, NOVA_SAMPLE_RATE);
        return -1;
    }
    s_render_info = *info;
    s_last_sample = 0;
    return 0;
}

#define SPK_BLOCK 80

// 16 kHz mono in; 48 kHz 32-bit stereo out. Also feeds the AEC reference.
static void speaker_block(const int16_t *mono, int m)
{
    static int32_t out[SPK_BLOCK * SPK_UPSAMPLE * 2];
    ref_push(mono, m);
    int o = 0;
    for (int i = 0; i < m; i++) {
        // Linear interpolation from the previous sample to this one.
        float a = s_last_sample, b = mono[i];
        for (int k = 1; k <= SPK_UPSAMPLE; k++) {
            int32_t v = (int32_t)((a + (b - a) * k / SPK_UPSAMPLE) * s_volume);
            out[o++] = v << 16;  // left slot
            out[o++] = v << 16;  // right slot (the amp averages L+R when SD floats)
        }
        s_last_sample = mono[i];
    }
    size_t written = 0;
    i2s_channel_write(s_tx, out, o * sizeof(int32_t), &written, portMAX_DELAY);
    s_speaker_busy_until_us = esp_timer_get_time() + HALF_DUPLEX_TAIL_MS * 1000;
}

static int render_write(audio_render_handle_t h, av_render_audio_frame_t *frame)
{
    const int16_t *in = (const int16_t *)frame->data;
    const int ch = s_render_info.channel ? s_render_info.channel : 1;
    const int n = frame->size / (2 * ch);
    int16_t mono[SPK_BLOCK];
    for (int start = 0; start < n; start += SPK_BLOCK) {
        int m = n - start < SPK_BLOCK ? n - start : SPK_BLOCK;
        for (int i = 0; i < m; i++) {
            mono[i] = in[(start + i) * ch];
        }
        speaker_block(mono, m);
    }
    return 0;
}

static void tone(float hz, int ms)
{
    const int n = NOVA_SAMPLE_RATE * ms / 1000;
    const int fade = NOVA_SAMPLE_RATE * 8 / 1000;  // 8 ms ramps, no clicks
    int16_t block[SPK_BLOCK];
    for (int start = 0; start < n; start += SPK_BLOCK) {
        int m = n - start < SPK_BLOCK ? n - start : SPK_BLOCK;
        for (int i = 0; i < m; i++) {
            int t = start + i;
            float env = t < fade ? (float)t / fade : t > n - fade ? (float)(n - t) / fade : 1.0f;
            block[i] = (int16_t)(7000.0f * env * sinf(2.0f * (float)M_PI * hz * t / NOVA_SAMPLE_RATE));
        }
        speaker_block(block, m);
    }
}

void audio_chime(bool ready)
{
    if (ready) {
        tone(660, 80);
        tone(990, 120);
    } else {
        tone(880, 70);
    }
}

static int render_latency(audio_render_handle_t h, uint32_t *latency)
{
    *latency = 0;
    return 0;
}

static int render_frame_info(audio_render_handle_t h, av_render_audio_frame_info_t *info)
{
    *info = s_render_info;
    return 0;
}

static int render_speed(audio_render_handle_t h, float speed)
{
    return 0;
}

static int render_close(audio_render_handle_t h)
{
    return 0;
}

static void render_deinit(audio_render_handle_t h)
{
}

audio_render_handle_t audio_speaker_render(void)
{
    static audio_render_handle_t handle;
    if (handle == NULL) {
        audio_render_cfg_t cfg = {
            .ops = {
                .init = render_init,
                .open = render_open,
                .write = render_write,
                .get_latency = render_latency,
                .get_frame_info = render_frame_info,
                .set_speed = render_speed,
                .close = render_close,
                .deinit = render_deinit,
            },
        };
        handle = audio_render_alloc_handle(&cfg);
    }
    return handle;
}

// -------------------------------------------------------------- mic task ---
void audio_set_mode(audio_mode_t mode)
{
    if (mode == AUDIO_MODE_SESSION) {
        ref_reset();
        xStreamBufferReset(s_src.buf);
    } else {
        wake_reset();
    }
    s_mode = mode;
}

static void mic_task(void *arg)
{
    int32_t *raw = heap_caps_malloc(s_chunk * sizeof(int32_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    int16_t *mic = heap_caps_malloc(s_chunk * sizeof(int16_t), MALLOC_CAP_INTERNAL);
    int16_t *clean = heap_caps_aligned_alloc(16, s_chunk * sizeof(int16_t), MALLOC_CAP_INTERNAL);
#if NOVA_ECHO_CANCEL
    int16_t *ref = heap_caps_malloc(s_chunk * sizeof(int16_t), MALLOC_CAP_INTERNAL);
    int16_t *mr = heap_caps_aligned_alloc(16, s_chunk * 2 * sizeof(int16_t), MALLOC_CAP_INTERNAL);
#endif
    float x1 = 0, y1 = 0;

    while (true) {
        size_t got = 0;
        if (i2s_channel_read(s_rx, raw, s_chunk * sizeof(int32_t), &got, portMAX_DELAY) != ESP_OK) {
            continue;
        }
        int n = got / sizeof(int32_t);
        for (int i = 0; i < n; i++) {
            float x = (float)(raw[i] >> 8);  // 24-bit sample in a 32-bit slot
            float y = HPF_ALPHA * (y1 + x - x1);
            x1 = x;
            y1 = y;
            float s = y * MIC_GAIN / 256.0f;
            mic[i] = s > 32767 ? 32767 : s < -32768 ? -32768 : (int16_t)s;
        }
        if (n != s_chunk) {
            continue;
        }

        if (s_mode == AUDIO_MODE_WAKE) {
            wake_feed(mic, n);
            continue;
        }
        if (!s_src.started) {
            continue;
        }
        const int16_t *send = mic;
#if NOVA_ECHO_CANCEL
        ref_pop(ref, n);
        for (int i = 0; i < n; i++) {
            mr[2 * i] = mic[i];
            mr[2 * i + 1] = ref[i];
        }
        afe_aec_process(s_aec, mr, clean);
        send = clean;
#else
        if (esp_timer_get_time() < s_speaker_busy_until_us) {
            memset(clean, 0, n * sizeof(int16_t));
            send = clean;
        }
#endif
        xStreamBufferSend(s_src.buf, send, n * sizeof(int16_t), 0);
    }
}

// ------------------------------------------------------------------ init ---
static esp_err_t mic_init(void)
{
    i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan.dma_desc_num = 6;
    chan.dma_frame_num = 256;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan, NULL, &s_rx), TAG, "mic channel");
    i2s_std_config_t std = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(NOVA_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = PIN_MIC_BCLK,
            .ws = PIN_MIC_WS,
            .dout = I2S_GPIO_UNUSED,
            .din = PIN_MIC_DIN,
        },
    };
    std.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;  // INMP441 L/R tied to GND
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_rx, &std), TAG, "mic std mode");
    return i2s_channel_enable(s_rx);
}

static esp_err_t speaker_init(void)
{
    i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    chan.dma_desc_num = 4;
    chan.dma_frame_num = 240;  // 4 x 5 ms keeps the speaker queue (and AEC lead) short
    chan.auto_clear = true;    // silence, not a stuck buffer, on underrun
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan, &s_tx, NULL), TAG, "speaker channel");
    i2s_std_config_t std = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SPK_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = PIN_AMP_BCLK,
            .ws = PIN_AMP_LRC,
            .dout = PIN_AMP_DIN,
            .din = I2S_GPIO_UNUSED,
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_tx, &std), TAG, "speaker std mode");
    return i2s_channel_enable(s_tx);
}

esp_err_t audio_init(void)
{
    s_ref = heap_caps_calloc(REF_RING_SAMPLES, sizeof(int16_t), MALLOC_CAP_SPIRAM);
    s_src.buf = xStreamBufferCreateWithCaps(CAPTURE_BUFFER_BYTES, 1, MALLOC_CAP_SPIRAM);
    if (s_ref == NULL || s_src.buf == NULL) {
        return ESP_ERR_NO_MEM;
    }
    s_src.base = (esp_capture_audio_src_if_t){
        .open = src_open,
        .get_support_codecs = src_get_codecs,
        .set_fixed_caps = src_set_fixed_caps,
        .negotiate_caps = src_negotiate,
        .start = src_start,
        .read_frame = src_read,
        .abort = src_abort,
        .stop = src_stop,
        .close = src_close,
    };

    s_aec = afe_aec_create("MR", AEC_FILTER_LEN, AFE_TYPE_VC, AFE_MODE_LOW_COST);
    if (s_aec == NULL) {
        ESP_LOGE(TAG, "echo canceller init failed");
        return ESP_FAIL;
    }
    s_chunk = afe_aec_get_chunksize(s_aec);
    ESP_LOGI(TAG, "echo canceller: %d-sample chunks, filter %d, ref delay %d ms, %s",
             s_chunk, AEC_FILTER_LEN, AEC_REF_DELAY_MS, NOVA_ECHO_CANCEL ? "on" : "off (half duplex)");

    ESP_RETURN_ON_ERROR(mic_init(), TAG, "mic");
    ESP_RETURN_ON_ERROR(speaker_init(), TAG, "speaker");
    ESP_LOGI(TAG, "mic on I2S0 (bclk=%d ws=%d din=%d), speaker on I2S1 (bclk=%d lrc=%d din=%d) @ %d Hz",
             PIN_MIC_BCLK, PIN_MIC_WS, PIN_MIC_DIN, PIN_AMP_BCLK, PIN_AMP_LRC, PIN_AMP_DIN, SPK_RATE);

    xTaskCreatePinnedToCore(mic_task, "mic", 6 * 1024, NULL, 18, NULL, 0);
    return ESP_OK;
}
