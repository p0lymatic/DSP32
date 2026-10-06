/*
 * SPDX-FileCopyrightText: 2026 ESP32-S3 USB DAC Project
 * SPDX-License-Identifier: MIT
 */

#include "audio_pipeline.h"
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"
#include "esp_log.h"
#include "i2s_dac.h"

static const char *TAG = "AUDIO_PIPE";

#define RINGBUF_SIZE_BYTES  (32 * 1024)
#define MAX_CHUNK_SAMPLES   256

// Inter-Core Ring Buffers:
// s_usb_rx_ringbuf: Filled by TinyUSB on Core 1, consumed by DSP Engine on Core 0
static RingbufHandle_t s_usb_rx_ringbuf = NULL;
// s_dsp_tx_ringbuf: Filled by DSP Engine on Core 0, consumed by I2S DMA on Core 1
static RingbufHandle_t s_dsp_tx_ringbuf = NULL;

static TaskHandle_t s_dsp_task_handle = NULL;
static TaskHandle_t s_i2s_task_handle = NULL;
static dsp_engine_t s_dsp_engine;

static volatile uint32_t s_sample_rate = 48000;
static volatile uint8_t  s_bit_depth = 16;
static volatile audio_buffer_size_t s_chunk_samples = LATENCY_MODE_SAFE_128;
static volatile uint32_t s_underruns = 0;
static volatile uint32_t s_overruns = 0;
static volatile bool     s_is_buffering = true;

// DSP Working Buffers (Core 0)
static float s_proc_l[MAX_CHUNK_SAMPLES];
static float s_proc_r[MAX_CHUNK_SAMPLES];
static uint8_t s_dsp_in_buf[MAX_CHUNK_SAMPLES * 8];
static uint8_t s_dsp_out_buf[MAX_CHUNK_SAMPLES * 8];

// I2S Output Buffer (Core 1)
static uint8_t s_i2s_out_buf[MAX_CHUNK_SAMPLES * 8];

//--------------------------------------------------------------------+
// CORE 0: DSP Worker Task (Priority 15)
// Consumes raw PCM from USB, executes float DSP chain, pushes to I2S ringbuf
//--------------------------------------------------------------------+
static void dsp_worker_task(void *pvParameters)
{
    ESP_LOGI(TAG, "DSP Compute Task started on Core %d (Priority 15)", xPortGetCoreID());

    while (1) {
        size_t current_chunk = s_chunk_samples;
        uint8_t current_depth = s_bit_depth;
        size_t bytes_per_sample = (current_depth == 24) ? 4 : 2;
        size_t bytes_needed = current_chunk * 2 * bytes_per_sample; // Stereo

        // Read raw USB audio chunk from s_usb_rx_ringbuf
        size_t bytes_collected = 0;
        while (bytes_collected < bytes_needed) {
            size_t item_size = 0;
            size_t to_read = bytes_needed - bytes_collected;
            uint8_t *item = (uint8_t *)xRingbufferReceiveUpTo(s_usb_rx_ringbuf, &item_size, pdMS_TO_TICKS(10), to_read);
            if (item && item_size > 0) {
                memcpy(s_dsp_in_buf + bytes_collected, item, item_size);
                bytes_collected += item_size;
                vRingbufferReturnItem(s_usb_rx_ringbuf, item);
            } else {
                break;
            }
        }

        if (bytes_collected == 0) {
            // No USB data waiting - yield to other Core 0 services (Wi-Fi, HTTP, LED)
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }

        if (bytes_collected < bytes_needed) {
            // Pad silence for remainder
            memset(s_dsp_in_buf + bytes_collected, 0, bytes_needed - bytes_collected);
        }

        // 1. Bit-Perfect Direct Mode: Bypass all DSP calculations!
        if (dsp_engine_is_bit_perfect(&s_dsp_engine)) {
            dsp_engine_meter_update_pcm(&s_dsp_engine, s_dsp_in_buf, bytes_needed, current_depth);
            xRingbufferSend(s_dsp_tx_ringbuf, s_dsp_in_buf, bytes_needed, pdMS_TO_TICKS(20));
            continue;
        }

        // 2. DSP Mode: Convert PCM to Float
        if (current_depth == 16) {
            const int16_t *src = (const int16_t *)s_dsp_in_buf;
            const float norm16 = 1.0f / 32768.0f;
            for (size_t i = 0; i < current_chunk; i++) {
                s_proc_l[i] = (float)src[2 * i] * norm16;
                s_proc_r[i] = (float)src[2 * i + 1] * norm16;
            }
        } else {
            const int32_t *src = (const int32_t *)s_dsp_in_buf;
            const float norm24 = 1.0f / 8388608.0f;
            for (size_t i = 0; i < current_chunk; i++) {
                s_proc_l[i] = (float)(src[2 * i] >> 8) * norm24;
                s_proc_r[i] = (float)(src[2 * i + 1] >> 8) * norm24;
            }
        }

        // 3. Process complete studio 32-bit float DSP chain
        dsp_engine_process(&s_dsp_engine, s_proc_l, s_proc_r, current_chunk);

        // 4. Convert Float back to PCM
        if (current_depth == 16) {
            int16_t *dst = (int16_t *)s_dsp_out_buf;
            for (size_t i = 0; i < current_chunk; i++) {
                float fl = s_proc_l[i];
                float fr = s_proc_r[i];

                if (fl > 1.0f) fl = 1.0f;
                else if (fl < -1.0f) fl = -1.0f;
                if (fr > 1.0f) fr = 1.0f;
                else if (fr < -1.0f) fr = -1.0f;

                dst[2 * i]     = (int16_t)(fl * 32767.0f);
                dst[2 * i + 1] = (int16_t)(fr * 32767.0f);
            }
        } else {
            int32_t *dst = (int32_t *)s_dsp_out_buf;
            for (size_t i = 0; i < current_chunk; i++) {
                float fl = s_proc_l[i];
                float fr = s_proc_r[i];

                if (fl > 1.0f) fl = 1.0f;
                else if (fl < -1.0f) fl = -1.0f;
                if (fr > 1.0f) fr = 1.0f;
                else if (fr < -1.0f) fr = -1.0f;

                dst[2 * i]     = ((int32_t)(fl * 8388607.0f)) << 8;
                dst[2 * i + 1] = ((int32_t)(fr * 8388607.0f)) << 8;
            }
        }

        // Push processed audio to Core 1 I2S ringbuffer
        xRingbufferSend(s_dsp_tx_ringbuf, s_dsp_out_buf, bytes_needed, pdMS_TO_TICKS(20));
    }
}

//--------------------------------------------------------------------+
// CORE 1: Dedicated I2S Output Task (Priority 24 - REALTIME MAX)
// Pulls processed PCM from s_dsp_tx_ringbuf and feeds I2S DMA with zero jitter
//--------------------------------------------------------------------+
static void i2s_tx_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Dedicated I2S Output Task started on Core %d (Priority %d)",
             xPortGetCoreID(), configMAX_PRIORITIES - 1);

    while (1) {
        size_t current_chunk = s_chunk_samples;
        uint8_t current_depth = s_bit_depth;
        size_t bytes_per_sample = (current_depth == 24) ? 4 : 2;
        size_t bytes_needed = current_chunk * 2 * bytes_per_sample;

        // 1. Watermark Pre-buffering to prevent jitter underruns and crackling
        if (s_is_buffering) {
            size_t free_sz = xRingbufferGetCurFreeSize(s_dsp_tx_ringbuf);
            size_t buffered = (free_sz < RINGBUF_SIZE_BYTES) ? (RINGBUF_SIZE_BYTES - free_sz) : 0;
            // 25ms jitter watermark cushion (~4800 bytes for 48kHz 16-bit)
            size_t watermark = (size_t)(s_sample_rate * 0.025f) * 2 * bytes_per_sample;
            if (watermark < bytes_needed * 4) watermark = bytes_needed * 4;
            if (watermark > RINGBUF_SIZE_BYTES / 2) watermark = RINGBUF_SIZE_BYTES / 2;

            if (buffered < watermark) {
                // Continuous silence to PCM5102A during prebuffering to maintain bit clock & PLL lock
                memset(s_i2s_out_buf, 0, bytes_needed);
                size_t written = 0;
                i2s_dac_write(s_i2s_out_buf, bytes_needed, &written, 20);
                vTaskDelay(pdMS_TO_TICKS(2));
                continue;
            }
            s_is_buffering = false;
        }

        // 2. Accumulate processed audio data from s_dsp_tx_ringbuf
        size_t bytes_collected = 0;
        while (bytes_collected < bytes_needed) {
            size_t item_size = 0;
            size_t to_read = bytes_needed - bytes_collected;
            uint8_t *item = (uint8_t *)xRingbufferReceiveUpTo(s_dsp_tx_ringbuf, &item_size, pdMS_TO_TICKS(10), to_read);
            if (item && item_size > 0) {
                memcpy(s_i2s_out_buf + bytes_collected, item, item_size);
                bytes_collected += item_size;
                vRingbufferReturnItem(s_dsp_tx_ringbuf, item);
            } else {
                break;
            }
        }

        if (bytes_collected == 0) {
            // Buffer dry -> stream paused. Write silence continuously to prevent I2S clock loss and pops
            s_is_buffering = true;
            memset(s_i2s_out_buf, 0, bytes_needed);
            size_t written = 0;
            i2s_dac_write(s_i2s_out_buf, bytes_needed, &written, 20);
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }

        if (bytes_collected < bytes_needed) {
            s_underruns++;
            memset(s_i2s_out_buf + bytes_collected, 0, bytes_needed - bytes_collected);
            s_is_buffering = true;
        }

        // Output to PCM5102A I2S DMA (Core 1, hardware DMA)
        size_t written = 0;
        i2s_dac_write(s_i2s_out_buf, bytes_needed, &written, 100);
    }
}

esp_err_t audio_pipeline_init(uint32_t sample_rate, uint8_t bit_depth)
{
    s_sample_rate = sample_rate;
    s_bit_depth = bit_depth;

    // 1. Create USB RX ringbuffer (receives from Core 1 TinyUSB)
    s_usb_rx_ringbuf = xRingbufferCreate(RINGBUF_SIZE_BYTES, RINGBUF_TYPE_BYTEBUF);
    if (!s_usb_rx_ringbuf) {
        ESP_LOGE(TAG, "Failed to create USB RX ringbuffer");
        return ESP_ERR_NO_MEM;
    }

    // 2. Create DSP TX ringbuffer (feeds Core 1 I2S DMA)
    s_dsp_tx_ringbuf = xRingbufferCreate(RINGBUF_SIZE_BYTES, RINGBUF_TYPE_BYTEBUF);
    if (!s_dsp_tx_ringbuf) {
        ESP_LOGE(TAG, "Failed to create DSP TX ringbuffer");
        return ESP_ERR_NO_MEM;
    }

    // 3. Initialize I2S hardware
    esp_err_t ret = i2s_dac_init(sample_rate, bit_depth);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize I2S DAC: %s", esp_err_to_name(ret));
        return ret;
    }

    // 4. Initialize DSP Engine
    dsp_engine_init(&s_dsp_engine, (float)sample_rate);

    // 5. Spawn Core 0 DSP Worker Task (Priority 15)
    BaseType_t r = xTaskCreatePinnedToCore(dsp_worker_task,
                                           "dsp_worker",
                                           8192,
                                           NULL,
                                           15,
                                           &s_dsp_task_handle,
                                           0); // Core 0
    if (r != pdPASS) {
        ESP_LOGE(TAG, "Failed to create Core 0 DSP worker task");
        return ESP_FAIL;
    }

    // 6. Spawn Core 1 Dedicated I2S TX Task (Priority 24 - REALTIME MAX)
    r = xTaskCreatePinnedToCore(i2s_tx_task,
                                "i2s_tx",
                                4096,
                                NULL,
                                configMAX_PRIORITIES - 1,
                                &s_i2s_task_handle,
                                1); // Core 1
    if (r != pdPASS) {
        ESP_LOGE(TAG, "Failed to create Core 1 I2S TX task");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Dual-Core Audio Engine initialized successfully:");
    ESP_LOGI(TAG, " -> Core 1: Dedicated Real-Time I/O (USB RX + I2S TX @ Priority 24)");
    ESP_LOGI(TAG, " -> Core 0: 32-bit DSP Engine + Network/Web UI (Priority 15)");
    return ESP_OK;
}

void audio_pipeline_write_usb_data(const uint8_t *data, size_t len, uint8_t bit_depth)
{
    if (!s_usb_rx_ringbuf || !data || len == 0) return;
    s_bit_depth = bit_depth;

    BaseType_t res = xRingbufferSend(s_usb_rx_ringbuf, data, len, 0);
    if (res != pdTRUE) {
        s_overruns++;
    }
}

void audio_pipeline_set_format(uint32_t sample_rate, uint8_t bit_depth)
{
    s_sample_rate = sample_rate;
    s_bit_depth = bit_depth;
    i2s_dac_set_clock(sample_rate, bit_depth);
    dsp_engine_set_sample_rate(&s_dsp_engine, (float)sample_rate);
}

void audio_pipeline_set_buffer_size(audio_buffer_size_t size)
{
    if (size != 16 && size != 32 && size != 64 && size != 128 && size != 256) {
        size = LATENCY_MODE_SAFE_128;
    }
    s_chunk_samples = size;
    ESP_LOGI(TAG, "Audio buffer latency changed: %d samples", (int)size);
}

audio_buffer_size_t audio_pipeline_get_buffer_size(void)
{
    return s_chunk_samples;
}

void audio_pipeline_get_stats(audio_pipeline_stats_t *out_stats)
{
    if (!out_stats) return;
    out_stats->buffer_size_samples = s_chunk_samples;
    out_stats->sample_rate = s_sample_rate;
    out_stats->bit_depth = s_bit_depth;
    out_stats->bit_perfect = dsp_engine_is_bit_perfect(&s_dsp_engine);
    out_stats->ringbuf_total_bytes = RINGBUF_SIZE_BYTES;
    out_stats->ringbuf_fill_bytes = s_dsp_tx_ringbuf ? (RINGBUF_SIZE_BYTES - xRingbufferGetCurFreeSize(s_dsp_tx_ringbuf)) : 0;
    out_stats->underrun_count = s_underruns;
    out_stats->overrun_count = s_overruns;
}

dsp_engine_t *audio_pipeline_get_dsp_engine(void)
{
    return &s_dsp_engine;
}
