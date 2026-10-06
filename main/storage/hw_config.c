/*
 * SPDX-FileCopyrightText: 2026 ESP32-S3 USB DAC Project
 * SPDX-License-Identifier: MIT
 */

#include "hw_config.h"
#include <string.h>
#include <stdio.h>
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"

static const char *TAG = "HW_CFG";
#define NVS_NAMESPACE "hw_cfg"
#define NVS_BLOB_KEY  "cfg"

static hw_config_t s_config;

static void set_defaults(void)
{
    s_config.i2s_bck_gpio = HW_DEFAULT_I2S_BCK_PIN;
    s_config.i2s_din_gpio = HW_DEFAULT_I2S_DIN_PIN;
    s_config.i2s_ws_gpio = HW_DEFAULT_I2S_WS_PIN;
    s_config.neopixel_gpio = HW_DEFAULT_NEOPIXEL_PIN;
    s_config.boot_button_gpio = HW_DEFAULT_BOOT_BUTTON_PIN;
    s_config.oled_scl_gpio = HW_DEFAULT_OLED_SCL_PIN;
    s_config.oled_sda_gpio = HW_DEFAULT_OLED_SDA_PIN;
    strncpy(s_config.wifi_ssid, HW_DEFAULT_WIFI_SSID, sizeof(s_config.wifi_ssid) - 1);
    s_config.wifi_ssid[sizeof(s_config.wifi_ssid) - 1] = '\0';
    strncpy(s_config.wifi_pass, HW_DEFAULT_WIFI_PASS, sizeof(s_config.wifi_pass) - 1);
    s_config.wifi_pass[sizeof(s_config.wifi_pass) - 1] = '\0';
    s_config.wifi_channel = HW_DEFAULT_WIFI_CHANNEL;
    s_config.oled_enabled = HW_DEFAULT_OLED_ENABLED;
    s_config.oled_fps = HW_DEFAULT_OLED_FPS;
    s_config.web_scope_fps = HW_DEFAULT_WEB_SCOPE_FPS;
    s_config.scope_gain = HW_DEFAULT_SCOPE_GAIN;
    s_config.scope_pre_vol = HW_DEFAULT_SCOPE_PRE_VOL;
}

esp_err_t hw_config_init(void)
{
    set_defaults();

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_OK) {
        size_t sz = sizeof(hw_config_t);
        hw_config_t loaded;
        err = nvs_get_blob(handle, NVS_BLOB_KEY, &loaded, &sz);
        if (err == ESP_OK && sz == sizeof(hw_config_t)) {
            s_config = loaded;
            ESP_LOGI(TAG, "Hardware config loaded from NVS: I2S (BCK=%d, DIN=%d, WS=%d), OLED (SCL=%d, SDA=%d, enabled=%d, %d FPS), SCOPE (gain=%.1fx, pre_vol=%d)",
                     s_config.i2s_bck_gpio, s_config.i2s_din_gpio, s_config.i2s_ws_gpio,
                     s_config.oled_scl_gpio, s_config.oled_sda_gpio, (int)s_config.oled_enabled, s_config.oled_fps,
                     s_config.scope_gain, (int)s_config.scope_pre_vol);
        } else {
            ESP_LOGW(TAG, "Hardware config in NVS invalid or size mismatch, using factory defaults");
        }
        nvs_close(handle);
    } else {
        ESP_LOGI(TAG, "No custom hardware config in NVS, using factory defaults: I2S (BCK=%d, DIN=%d, WS=%d), OLED (enabled=%d, %d FPS)",
                 s_config.i2s_bck_gpio, s_config.i2s_din_gpio, s_config.i2s_ws_gpio, (int)s_config.oled_enabled, s_config.oled_fps);
    }

    return ESP_OK;
}

const hw_config_t *hw_config_get(void)
{
    return &s_config;
}

esp_err_t hw_config_set(const hw_config_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;

    // Validate GPIO numbers (-1 to 48)
    if (cfg->i2s_bck_gpio < -1 || cfg->i2s_bck_gpio > 48 ||
        cfg->i2s_din_gpio < -1 || cfg->i2s_din_gpio > 48 ||
        cfg->i2s_ws_gpio < -1 || cfg->i2s_ws_gpio > 48 ||
        cfg->neopixel_gpio < -1 || cfg->neopixel_gpio > 48 ||
        cfg->boot_button_gpio < -1 || cfg->boot_button_gpio > 48 ||
        cfg->oled_scl_gpio < -1 || cfg->oled_scl_gpio > 48 ||
        cfg->oled_sda_gpio < -1 || cfg->oled_sda_gpio > 48) {
        ESP_LOGE(TAG, "GPIO out of valid range (-1..48)");
        return ESP_ERR_INVALID_ARG;
    }

    hw_config_t validated = *cfg;
    if (validated.oled_fps < 15 || validated.oled_fps > 60) validated.oled_fps = HW_DEFAULT_OLED_FPS;
    if (validated.web_scope_fps < 10 || validated.web_scope_fps > 60) validated.web_scope_fps = HW_DEFAULT_WEB_SCOPE_FPS;
    if (validated.scope_gain < 0.25f || validated.scope_gain > 10.0f) validated.scope_gain = HW_DEFAULT_SCOPE_GAIN;

    s_config = validated;

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = nvs_set_blob(handle, NVS_BLOB_KEY, &s_config, sizeof(hw_config_t));
        if (err == ESP_OK) {
            nvs_commit(handle);
            ESP_LOGI(TAG, "Saved new hardware config to NVS");
        }
        nvs_close(handle);
    }
    return err;
}

esp_err_t hw_config_reset_defaults(void)
{
    set_defaults();

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        nvs_erase_all(handle);
        nvs_commit(handle);
        nvs_close(handle);
        ESP_LOGI(TAG, "Hardware config reset to factory defaults in NVS");
    }
    return ESP_OK;
}
