/*
 * SPDX-FileCopyrightText: 2026 ESP32-S3 USB DAC Project
 * SPDX-License-Identifier: MIT
 */

#include "oled_display.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/i2c_master.h"

#include "u8g2.h"
#include "u8x8.h"
#include "esp32_hw_i2c.h"

#include "storage/hw_config.h"
#include "storage/preset_manager.h"
#include "audio_pipeline.h"
#include "dsp/dsp_engine.h"
#include "wifi/wifi_ap.h"

static const char *TAG = "OLED_DISP";

static u8g2_t s_u8g2;
static u8g2_esp32_i2c_ctx_t s_i2c_ctx;
static bool s_is_active = false;
static TaskHandle_t s_oled_task_hdl = NULL;

bool oled_display_is_active(void)
{
    return s_is_active;
}

// ----------------------------------------------------------------------------
// Layout Drawing Functions (Vib-Ribbon Monochrome Vector Style)
// ----------------------------------------------------------------------------

static void draw_top_bar(u8g2_t *u8g2, const dsp_meter_values_t *meters, const audio_pipeline_stats_t *stats, float vol_db)
{
    // Stereo VU Meter Bars (X = 0..67)
    // Usable meter range: -48 dBFS to 0 dBFS
    const float min_db = -48.0f;
    const int bar_max_w = 64;

    float peak_l = meters->out_peak_l;
    float peak_r = meters->out_peak_r;
    if (peak_l < min_db) peak_l = min_db;
    if (peak_r < min_db) peak_r = min_db;

    int w_l = (int)((peak_l - min_db) * ((float)bar_max_w / (-min_db)));
    int w_r = (int)((peak_r - min_db) * ((float)bar_max_w / (-min_db)));
    if (w_l > bar_max_w) w_l = bar_max_w;
    if (w_r > bar_max_w) w_r = bar_max_w;

    // L Channel bar
    u8g2_DrawFrame(u8g2, 0, 0, 68, 4);
    if (w_l > 0) {
        u8g2_DrawBox(u8g2, 2, 1, w_l, 2);
    }

    // R Channel bar
    u8g2_DrawFrame(u8g2, 0, 5, 68, 4);
    if (w_r > 0) {
        u8g2_DrawBox(u8g2, 2, 6, w_r, 2);
    }

    // Right side: Sample rate and Volume (X = 72..127)
    u8g2_SetFont(u8g2, u8g2_font_4x6_tr);

    char rate_str[16];
    if (stats->bit_perfect) {
        snprintf(rate_str, sizeof(rate_str), "DIR:%uk", (unsigned)(stats->sample_rate / 1000));
    } else {
        snprintf(rate_str, sizeof(rate_str), "%uk/%ub", (unsigned)(stats->sample_rate / 1000), (unsigned)stats->bit_depth);
    }
    u8g2_DrawStr(u8g2, 72, 4, rate_str);

    char vol_str[16];
    if (stats->bit_perfect) {
        snprintf(vol_str, sizeof(vol_str), "0dB");
    } else if (vol_db <= -60.0f) {
        snprintf(vol_str, sizeof(vol_str), "MUTE");
    } else {
        snprintf(vol_str, sizeof(vol_str), "%.0fdB", vol_db);
    }
    u8g2_DrawStr(u8g2, 72, 9, vol_str);

    // Divider line below top bar
    u8g2_DrawHLine(u8g2, 0, 10, 128);
}

static void draw_oscilloscope(u8g2_t *u8g2, const float *waveform, size_t count)
{
    // Oscilloscope area: Y = 11..39 (midline Y = 25, half-height = 12)
    const int mid_y = 25;
    const int max_dev = 12;

    // Measure peak amplitude in current frame
    float peak = 0.0f;
    if (waveform && count > 0) {
        for (size_t i = 0; i < count; i++) {
            float a = fabsf(waveform[i]);
            if (a > peak) peak = a;
        }
    }

    // Dotted center zero-axis for vector oscilloscope look
    for (int x = 0; x < 128; x += 4) {
        u8g2_DrawPixel(u8g2, x, mid_y);
    }

    static float s_smooth_gain = 12.0f;
    static float s_idle_phase = 0.0f;

    const hw_config_t *hw = hw_config_get();
    float calib = (hw && hw->scope_gain > 0.0f) ? hw->scope_gain : 1.0f;

    if (peak < 0.0003f) {
        // Standby / Silence: gentle subtle idle vector ripple (Vib-Ribbon aesthetic)
        s_idle_phase += 0.08f;
        if (s_idle_phase > 6.28318f) s_idle_phase -= 6.28318f;

        for (int x = 0; x < 127; x++) {
            float angle1 = s_idle_phase + (float)x * 0.08f;
            float angle2 = s_idle_phase + (float)(x + 1) * 0.08f;
            int y1 = mid_y - (int)roundf(sinf(angle1) * 1.5f);
            int y2 = mid_y - (int)roundf(sinf(angle2) * 1.5f);
            u8g2_DrawLine(u8g2, x, y1, x + 1, y2);
        }
        s_smooth_gain = 12.0f;
    } else {
        // Dynamic Auto-Gain (AGC): scale quiet audio so waveform clearly fills screen
        // Target peak amplitude = 10 pixels (leaving 2px headroom below max_dev = 12)
        float target_gain = ((float)(max_dev - 2) / (peak + 0.0001f)) * calib;
        if (target_gain > 120.0f) target_gain = 120.0f; // High boost for whisper/low listening volume
        if (target_gain < 8.0f) target_gain = 8.0f;     // Baseline for full scale tracks

        // Smooth gain transitions without stepping
        s_smooth_gain += 0.25f * (target_gain - s_smooth_gain);

        if (waveform && count > 1) {
            for (int x = 0; x < 127; x++) {
                float s1 = waveform[x];
                float s2 = waveform[x + 1];

                int y1 = mid_y - (int)roundf(s1 * s_smooth_gain);
                int y2 = mid_y - (int)roundf(s2 * s_smooth_gain);

                if (y1 < 12) y1 = 12;
                if (y1 > 38) y1 = 38;
                if (y2 < 12) y2 = 12;
                if (y2 > 38) y2 = 38;

                u8g2_DrawLine(u8g2, x, y1, x + 1, y2);
            }
        }
    }

    // Divider line below oscilloscope
    u8g2_DrawHLine(u8g2, 0, 40, 128);
}

static void draw_dsp_badges(u8g2_t *u8g2, const dsp_config_t *cfg)
{
    // Y area: 42..51 (height 10px)
    if (cfg->bit_perfect_bypass) {
        u8g2_DrawFrame(u8g2, 1, 42, 126, 10);
        u8g2_SetFont(u8g2, u8g2_font_4x6_tr);
        u8g2_DrawStr(u8g2, 10, 49, "--- BIT-PERFECT DIRECT ---");
    } else {
        typedef struct {
            const char *label;
            bool enabled;
        } badge_t;

        badge_t badges[9] = {
            { "EQ", cfg->eq.enabled },
            { "CP", cfg->comp.enabled },
            { "TP", cfg->tape.enabled },
            { "BC", cfg->bitcrusher.enabled },
            { "LM", cfg->limiter.enabled },
            { "BS", cfg->bass.enabled },
            { "CF", cfg->crossfeed.enabled },
            { "WD", cfg->widener.enabled },
            { "FX", cfg->fx.delay_enabled || cfg->fx.reverb_enabled },
        };

        // 9 badges of width 12, spacing 2 across 124px (fits in 128px screen)
        u8g2_SetFont(u8g2, u8g2_font_4x6_tr);
        for (int i = 0; i < 9; i++) {
            int bx = 2 + i * 14;
            int by = 42;
            int bw = 12;
            int bh = 9;

            if (badges[i].enabled) {
                // Inverted solid box with black font
                u8g2_DrawBox(u8g2, bx, by, bw, bh);
                u8g2_SetDrawColor(u8g2, 0);
                u8g2_DrawStr(u8g2, bx + 2, by + 7, badges[i].label);
                u8g2_SetDrawColor(u8g2, 1);
            } else {
                // Wireframe outline with white font
                u8g2_DrawFrame(u8g2, bx, by, bw, bh);
                u8g2_DrawStr(u8g2, bx + 2, by + 7, badges[i].label);
            }
        }
    }

    // Divider line below badges
    u8g2_DrawHLine(u8g2, 0, 53, 128);
}

static void draw_status_bar(u8g2_t *u8g2, const char *preset_name, bool wifi_active)
{
    // Status bar at bottom (Y = 54..63, text baseline at 62)
    u8g2_SetFont(u8g2, u8g2_font_4x6_tr);

    // Left: Preset Name
    char pbuf[24];
    snprintf(pbuf, sizeof(pbuf), "[%.14s]", preset_name ? preset_name : "FLAT");
    u8g2_DrawStr(u8g2, 0, 61, pbuf);

    // Right: Wi-Fi status and USB status
    const char *wifi_tag = wifi_active ? "W:AP" : "W:--";
    u8g2_DrawStr(u8g2, 86, 61, wifi_tag);
    u8g2_DrawStr(u8g2, 108, 61, "USB");
}

// ----------------------------------------------------------------------------
// Display Refresh Task (Core 0, ~25 FPS)
// ----------------------------------------------------------------------------

static void oled_task(void *pvParameters)
{
    ESP_LOGI(TAG, "SSD1306 OLED Task running on Core %d (Priority 1)", xPortGetCoreID());

    float waveform[128];
    dsp_meter_values_t meters;
    audio_pipeline_stats_t stats;
    dsp_config_t dsp_cfg;
    char preset_name[32];

    while (1) {
        const hw_config_t *hw_run = hw_config_get();
        if (hw_run && !hw_run->oled_enabled) {
            if (s_is_active) {
                u8g2_ClearDisplay(&s_u8g2);
                u8g2_SetPowerSave(&s_u8g2, 1);
                s_is_active = false;
                ESP_LOGI(TAG, "SSD1306 OLED display disabled, entering power save");
            }
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        // If display is not active, attempt probe every 2 seconds
        if (!s_is_active) {
            if (s_i2c_ctx.bus_handle != NULL) {
                esp_err_t probe_err = i2c_master_probe((i2c_master_bus_handle_t)s_i2c_ctx.bus_handle, 0x3C, 15);
                if (probe_err == ESP_OK) {
                    ESP_LOGI(TAG, "SSD1306 OLED detected on I2C bus! Initializing display...");
                    u8g2_InitDisplay(&s_u8g2);
                    u8g2_SetPowerSave(&s_u8g2, 0);
                    u8g2_ClearDisplay(&s_u8g2);
                    s_is_active = true;
                } else {
                    i2c_master_bus_reset((i2c_master_bus_handle_t)s_i2c_ctx.bus_handle);
                    vTaskDelay(pdMS_TO_TICKS(2000));
                    continue;
                }
            } else {
                vTaskDelay(pdMS_TO_TICKS(2000));
                continue;
            }
        }

        // Verify device is still acknowledging before sending full buffer
        esp_err_t probe_err = i2c_master_probe((i2c_master_bus_handle_t)s_i2c_ctx.bus_handle, 0x3C, 15);
        if (probe_err != ESP_OK) {
            ESP_LOGW(TAG, "SSD1306 OLED unresponsive on I2C bus, pausing render task...");
            i2c_master_bus_reset((i2c_master_bus_handle_t)s_i2c_ctx.bus_handle);
            s_is_active = false;
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        // Fetch pipeline and DSP parameters
        dsp_engine_t *engine = audio_pipeline_get_dsp_engine();
        audio_pipeline_get_stats(&stats);

        if (engine) {
            dsp_engine_get_meters(engine, &meters);
            dsp_engine_get_config(engine, &dsp_cfg);
            dsp_engine_get_waveform(engine, waveform, 128);
        } else {
            memset(&meters, 0, sizeof(meters));
            memset(&dsp_cfg, 0, sizeof(dsp_cfg));
            memset(waveform, 0, sizeof(waveform));
        }

        preset_manager_get_current_name(preset_name, sizeof(preset_name));
        bool wifi_on = wifi_ap_is_active();

        // Render frame in U8g2 memory buffer
        u8g2_ClearBuffer(&s_u8g2);

        draw_top_bar(&s_u8g2, &meters, &stats, dsp_cfg.master_volume_db);
        draw_oscilloscope(&s_u8g2, waveform, 128);
        draw_dsp_badges(&s_u8g2, &dsp_cfg);
        draw_status_bar(&s_u8g2, preset_name, wifi_on);

        // Push frame to SSD1306 via I2C
        u8g2_SendBuffer(&s_u8g2);

        // Dynamic refresh delay based on configured FPS (15..60)
        int target_fps = (hw_run && hw_run->oled_fps >= 15 && hw_run->oled_fps <= 60) ? hw_run->oled_fps : 30;
        int delay_ms = 1000 / target_fps;
        if (delay_ms < 10) delay_ms = 10;
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
}

// ----------------------------------------------------------------------------
// Initialization
// ----------------------------------------------------------------------------

esp_err_t oled_display_init(void)
{
    const hw_config_t *hw = hw_config_get();
    int scl_pin = hw->oled_scl_gpio;
    int sda_pin = hw->oled_sda_gpio;

    if (scl_pin < 0 || sda_pin < 0) {
        ESP_LOGI(TAG, "SSD1306 OLED disabled by pin configuration (SCL=%d, SDA=%d)", scl_pin, sda_pin);
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Configuring SSD1306 OLED on I2C (SCL=GPIO %d, SDA=GPIO %d, %d FPS)", scl_pin, sda_pin, hw->oled_fps);

    memset(&s_i2c_ctx, 0, sizeof(s_i2c_ctx));
    u8g2_esp32_i2c_config_t i2c_cfg = {
        .i2c_port = 0,
        .sda_pin = sda_pin,
        .scl_pin = scl_pin,
        .clk_hz = (hw->oled_fps > 30) ? 800000 : 400000,
        .dev_addr_7bit = 0x3C,
        .timeout_ms = 15,
        .reset_pin = U8G2_ESP32_PIN_UNUSED,
    };
    s_i2c_ctx.cfg = i2c_cfg;
    u8g2_esp32_i2c_set_default_context(&s_i2c_ctx);

    u8g2_Setup_ssd1306_i2c_128x64_noname_f(
        &s_u8g2,
        U8G2_R0,
        u8x8_byte_esp32_hw_i2c,
        u8x8_gpio_and_delay_esp32_i2c
    );
    u8g2_SetI2CAddress(&s_u8g2, 0x3C * 2);

    // Initial bus init via u8x8 MSG BYTE INIT
    u8x8_byte_esp32_hw_i2c(u8g2_GetU8x8(&s_u8g2), U8X8_MSG_BYTE_INIT, 0, NULL);

    if (!hw->oled_enabled) {
        ESP_LOGI(TAG, "SSD1306 OLED disabled by configuration");
    }

    s_is_active = false;

    // Launch display task on Core 0 with low priority (priority 1) to never preempt Wi-Fi or buttons
    BaseType_t task_ret = xTaskCreatePinnedToCore(
        oled_task,
        "oled_task",
        4096,
        NULL,
        1, // Priority 1 (safe background task)
        &s_oled_task_hdl,
        0  // Core 0 (audio stays on Core 1)
    );

    if (task_ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create oled_task");
        return ESP_FAIL;
    }

    return ESP_OK;
}
