/*
 * SPDX-FileCopyrightText: 2026 ESP32-S3 USB DAC Project
 * SPDX-License-Identifier: MIT
 */

#ifndef _HW_CONFIG_H_
#define _HW_CONFIG_H_

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// New prototype hardware defaults (shifted by -1)
#define HW_DEFAULT_I2S_BCK_PIN     10
#define HW_DEFAULT_I2S_DIN_PIN     11
#define HW_DEFAULT_I2S_WS_PIN      12
#define HW_DEFAULT_NEOPIXEL_PIN    48
#define HW_DEFAULT_BOOT_BUTTON_PIN 0
#define HW_DEFAULT_OLED_SCL_PIN    9
#define HW_DEFAULT_OLED_SDA_PIN    8
#define HW_DEFAULT_WIFI_SSID       "ESP32-DSP-DAC"
#define HW_DEFAULT_WIFI_PASS       "12345678"
#define HW_DEFAULT_WIFI_CHANNEL    1
#define HW_DEFAULT_OLED_ENABLED    true
#define HW_DEFAULT_OLED_FPS        30
#define HW_DEFAULT_WEB_SCOPE_FPS   30
#define HW_DEFAULT_SCOPE_GAIN      2.0f
#define HW_DEFAULT_SCOPE_PRE_VOL   true

typedef struct {
    int8_t i2s_bck_gpio;
    int8_t i2s_din_gpio;
    int8_t i2s_ws_gpio;
    int8_t neopixel_gpio;
    int8_t boot_button_gpio;
    int8_t oled_scl_gpio;
    int8_t oled_sda_gpio;
    char wifi_ssid[32];
    char wifi_pass[64];
    uint8_t wifi_channel;
    bool    oled_enabled;      // true = SSD1306 OLED active, false = disabled/sleep
    uint8_t oled_fps;          // OLED refresh rate: 15 to 60 FPS
    uint8_t web_scope_fps;     // Web UI scope polling rate: 10 to 60 FPS
    float   scope_gain;        // Scope sensitivity multiplier: 0.25f to 10.0f
    bool    scope_pre_vol;     // true = Pre-Master Volume (Studio), false = Post-Volume
} hw_config_t;

esp_err_t hw_config_init(void);
const hw_config_t *hw_config_get(void);
esp_err_t hw_config_set(const hw_config_t *cfg);
esp_err_t hw_config_reset_defaults(void);

#ifdef __cplusplus
}
#endif

#endif /* _HW_CONFIG_H_ */
