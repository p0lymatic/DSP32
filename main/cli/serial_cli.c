/*
 * SPDX-FileCopyrightText: 2026 ESP32-S3 USB DAC Project
 * SPDX-License-Identifier: MIT
 */

#include "serial_cli.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <inttypes.h>
#include <dirent.h>
#include <sys/stat.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_log.h"
#include "tusb.h"
#include "cJSON.h"

#include "audio_pipeline.h"
#include "usb_audio.h"
#include "dsp_types.h"
#include "dsp_engine.h"
#include "led/neopixel.h"
#include "wifi/wifi_ap.h"
#include "storage/preset_manager.h"
#include "storage/fs_manager.h"
#include "storage/hw_config.h"

static const char *TAG = "CLI";
#define CLI_LINE_BUF_SZ 1024

#if CFG_TUD_CDC

static bool s_echo_enabled = true;

typedef enum {
    STREAM_NONE = 0,
    STREAM_VU,
    STREAM_WAVE
} stream_type_t;

static stream_type_t s_stream_type = STREAM_NONE;
static uint32_t s_stream_interval_ms = 50;
static int64_t s_last_stream_time_us = 0;

void serial_cli_set_echo(bool enable)
{
    s_echo_enabled = enable;
}

bool serial_cli_get_echo(void)
{
    return s_echo_enabled;
}

static void cdc_send_raw(const char *str, size_t len)
{
    if (len > 0 && tud_cdc_connected()) {
        tud_cdc_write(str, (uint32_t)len);
        tud_cdc_write_flush();
    }
}

static void cdc_printf(const char *fmt, ...)
{
    char buf[512];
    va_list args;
    va_start(args, fmt);
    int len = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    if (len > 0) {
        cdc_send_raw(buf, (size_t)len);
    }
}

static void cdc_resp_ok(const char *msg)
{
    if (s_echo_enabled) {
        if (msg && *msg) cdc_printf("%s\r\n", msg);
    } else {
        if (msg && *msg) cdc_printf("OK %s\r\n", msg);
        else cdc_printf("OK\r\n");
    }
}

static void cdc_resp_err(const char *err)
{
    if (s_echo_enabled) {
        cdc_printf("Error: %s\r\n", err ? err : "unknown error");
    } else {
        cdc_printf("ERR %s\r\n", err ? err : "unknown error");
    }
}

static void print_help(void)
{
    cdc_printf("\r\n=== DSP32-S3 USB DAC Serial CLI / API ===\r\n");
    cdc_printf("  help                                 - Show this help manual\r\n");
    cdc_printf("  echo <on|off>                        - Toggle terminal echo & prompt (use off for apps)\r\n");
    cdc_printf("  status                               - Human readable audio & Wi-Fi status\r\n");
    cdc_printf("  vol [dB]                             - Get or set master volume (-60.0 to 0.0 dB)\r\n");
    cdc_printf("  bypass [on|off]                      - Get or toggle Bit-Perfect direct mode\r\n");
    cdc_printf("  buffer [16|32|64|128|256]            - Get or set DMA buffer latency\r\n");
    cdc_printf("  eq <band 0-9> [gain_db]              - Get or set single EQ band gain\r\n");
    cdc_printf("  eq all <g0> <g1> ... <g9>            - Set all 10 EQ bands at once\r\n");
    cdc_printf("  eq list                              - Print all 10 EQ bands\r\n");
    cdc_printf("  fx <name> <on|off>                   - Toggle effect (tape, crush, comp, lim, etc.)\r\n");
    cdc_printf("  fx <name> <param> <val>              - Set effect parameter (e.g. fx tape drive 0.35)\r\n");
    cdc_printf("  preset <list|load <n>|save <n>|reset>- Manage presets\r\n");
    cdc_printf("  led [mode 0-5] [brightness 0-100]    - Configure RGB NeoPixel indicator\r\n");
    cdc_printf("  vu                                   - Print instant Peak & RMS VU readings\r\n");
    cdc_printf("  stream <vu|wave|stop> [ms]           - Real-time periodic data push (30-60 FPS)\r\n");
    cdc_printf("  json get <config|status|vu|wave|led> - Machine JSON API for Android App\r\n");
    cdc_printf("  json set <config|led> <json_payload> - Machine JSON config updater\r\n");
    cdc_printf("  wifi <on|off|toggle|status>          - Control Wi-Fi SoftAP\r\n");
    cdc_printf("  pins [info|set <bck> <din> <ws>|reset] Hardware pinout configuration\r\n");
    cdc_printf("  scope [gain <x>|mode <pre|post>]     - Calibrate oscilloscope sensitivity & tap\r\n");
    cdc_printf("  oled [enable|disable|fps <15-60>]   - Configure OLED display status / FPS\r\n");
    cdc_printf("  storage <info|ls|cat|format>         - LittleFS flash storage manager\r\n");
    cdc_printf("=========================================\r\n\r\n");
}

/* --------------------------------------------------------------------------
 * JSON API Handlers
 * -------------------------------------------------------------------------- */
static void handle_json_get(dsp_engine_t *engine, const char *target)
{
    if (!target) {
        cdc_resp_err("missing target for json get (config|status|vu|wave|presets|led)");
        return;
    }

    if (strcmp(target, "config") == 0) {
        dsp_config_t cfg;
        dsp_engine_get_config(engine, &cfg);

        cJSON *root = cJSON_CreateObject();
        cJSON_AddBoolToObject(root, "bit_perfect_bypass", cfg.bit_perfect_bypass);
        cJSON_AddNumberToObject(root, "buffer_size", (double)audio_pipeline_get_buffer_size());
        cJSON_AddNumberToObject(root, "master_volume_db", cfg.master_volume_db);

        // EQ
        cJSON *eq_obj = cJSON_CreateObject();
        cJSON_AddBoolToObject(eq_obj, "enabled", cfg.eq.enabled);
        cJSON *bands_arr = cJSON_CreateArray();
        for (int i = 0; i < DSP_EQ_NUM_BANDS; i++) {
            cJSON *b = cJSON_CreateObject();
            cJSON_AddNumberToObject(b, "gain_db", cfg.eq.bands[i].gain_db);
            cJSON_AddNumberToObject(b, "freq", cfg.eq.bands[i].freq);
            cJSON_AddItemToArray(bands_arr, b);
        }
        cJSON_AddItemToObject(eq_obj, "bands", bands_arr);
        cJSON_AddItemToObject(root, "eq", eq_obj);

        // Compressor
        cJSON *comp_obj = cJSON_CreateObject();
        cJSON_AddBoolToObject(comp_obj, "enabled", cfg.comp.enabled);
        cJSON_AddNumberToObject(comp_obj, "threshold_db", cfg.comp.threshold_db);
        cJSON_AddNumberToObject(comp_obj, "ratio", cfg.comp.ratio);
        cJSON_AddNumberToObject(comp_obj, "attack_ms", cfg.comp.attack_ms);
        cJSON_AddNumberToObject(comp_obj, "release_ms", cfg.comp.release_ms);
        cJSON_AddItemToObject(root, "comp", comp_obj);

        // Limiter
        cJSON *lim_obj = cJSON_CreateObject();
        cJSON_AddBoolToObject(lim_obj, "enabled", cfg.limiter.enabled);
        cJSON_AddNumberToObject(lim_obj, "ceiling_db", cfg.limiter.ceiling_db);
        cJSON_AddNumberToObject(lim_obj, "release_ms", cfg.limiter.release_ms);
        cJSON_AddItemToObject(root, "limiter", lim_obj);

        // Tape
        cJSON *tape_obj = cJSON_CreateObject();
        cJSON_AddBoolToObject(tape_obj, "enabled", cfg.tape.enabled);
        cJSON_AddNumberToObject(tape_obj, "drive", cfg.tape.drive);
        cJSON_AddNumberToObject(tape_obj, "hf_cut_hz", cfg.tape.hf_cut_hz);
        cJSON_AddNumberToObject(tape_obj, "head_bump", cfg.tape.head_bump);
        cJSON_AddNumberToObject(tape_obj, "hiss_level", cfg.tape.hiss_level);
        cJSON_AddNumberToObject(tape_obj, "click_rate", cfg.tape.click_rate);
        cJSON_AddNumberToObject(tape_obj, "click_level", cfg.tape.click_level);
        cJSON_AddNumberToObject(tape_obj, "wow_flutter", cfg.tape.wow_flutter);
        cJSON_AddItemToObject(root, "tape", tape_obj);

        // Bitcrusher
        cJSON *bc_obj = cJSON_CreateObject();
        cJSON_AddBoolToObject(bc_obj, "enabled", cfg.bitcrusher.enabled);
        cJSON_AddBoolToObject(bc_obj, "enable_downsample", cfg.bitcrusher.enable_downsample);
        cJSON_AddBoolToObject(bc_obj, "enable_quantize", cfg.bitcrusher.enable_quantize);
        cJSON_AddBoolToObject(bc_obj, "enable_overflow", cfg.bitcrusher.enable_overflow);
        cJSON_AddBoolToObject(bc_obj, "dither", cfg.bitcrusher.dither);
        cJSON_AddNumberToObject(bc_obj, "bit_depth", cfg.bitcrusher.bit_depth);
        cJSON_AddNumberToObject(bc_obj, "downsample_rate", cfg.bitcrusher.downsample_rate);
        cJSON_AddNumberToObject(bc_obj, "overflow_intensity", cfg.bitcrusher.overflow_intensity);
        cJSON_AddNumberToObject(bc_obj, "mix", cfg.bitcrusher.mix);
        cJSON_AddItemToObject(root, "bitcrusher", bc_obj);

        // Crossfeed
        cJSON *cf_obj = cJSON_CreateObject();
        cJSON_AddBoolToObject(cf_obj, "enabled", cfg.crossfeed.enabled);
        cJSON_AddNumberToObject(cf_obj, "amount", cfg.crossfeed.amount);
        cJSON_AddNumberToObject(cf_obj, "cutoff_hz", cfg.crossfeed.cutoff_hz);
        cJSON_AddItemToObject(root, "crossfeed", cf_obj);

        // Widener
        cJSON *w_obj = cJSON_CreateObject();
        cJSON_AddBoolToObject(w_obj, "enabled", cfg.widener.enabled);
        cJSON_AddNumberToObject(w_obj, "width", cfg.widener.width);
        cJSON_AddItemToObject(root, "widener", w_obj);

        // Bass
        cJSON *bass_obj = cJSON_CreateObject();
        cJSON_AddBoolToObject(bass_obj, "enabled", cfg.bass.enabled);
        cJSON_AddNumberToObject(bass_obj, "cutoff_hz", cfg.bass.cutoff_hz);
        cJSON_AddNumberToObject(bass_obj, "drive", cfg.bass.drive);
        cJSON_AddNumberToObject(bass_obj, "blend", cfg.bass.blend);
        cJSON_AddItemToObject(root, "bass", bass_obj);

        // De-esser
        cJSON *deess_obj = cJSON_CreateObject();
        cJSON_AddBoolToObject(deess_obj, "enabled", cfg.deesser.enabled);
        cJSON_AddNumberToObject(deess_obj, "freq", cfg.deesser.freq);
        cJSON_AddNumberToObject(deess_obj, "threshold_db", cfg.deesser.threshold_db);
        cJSON_AddItemToObject(root, "deesser", deess_obj);

        // Pitch
        cJSON *pitch_obj = cJSON_CreateObject();
        cJSON_AddBoolToObject(pitch_obj, "enabled", cfg.pitch.enabled);
        cJSON_AddNumberToObject(pitch_obj, "semitones", cfg.pitch.semitones);
        cJSON_AddNumberToObject(pitch_obj, "cents", cfg.pitch.cents);
        cJSON_AddItemToObject(root, "pitch", pitch_obj);

        // Loudness
        cJSON *loud_obj = cJSON_CreateObject();
        cJSON_AddBoolToObject(loud_obj, "enabled", cfg.loudness.enabled);
        cJSON_AddNumberToObject(loud_obj, "target_db", cfg.loudness.target_db);
        cJSON_AddItemToObject(root, "loudness", loud_obj);

        // FX
        cJSON *fx_obj = cJSON_CreateObject();
        cJSON_AddBoolToObject(fx_obj, "delay_enabled", cfg.fx.delay_enabled);
        cJSON_AddNumberToObject(fx_obj, "delay_time_ms", cfg.fx.delay_time_ms);
        cJSON_AddNumberToObject(fx_obj, "delay_feedback", cfg.fx.delay_feedback);
        cJSON_AddNumberToObject(fx_obj, "delay_mix", cfg.fx.delay_mix);
        cJSON_AddBoolToObject(fx_obj, "reverb_enabled", cfg.fx.reverb_enabled);
        cJSON_AddNumberToObject(fx_obj, "reverb_room_size", cfg.fx.reverb_room_size);
        cJSON_AddNumberToObject(fx_obj, "reverb_damping", cfg.fx.reverb_damping);
        cJSON_AddNumberToObject(fx_obj, "reverb_mix", cfg.fx.reverb_mix);
        cJSON_AddItemToObject(root, "fx", fx_obj);

        char *str = cJSON_PrintUnformatted(root);
        cdc_resp_ok(str);
        free(str);
        cJSON_Delete(root);

    } else if (strcmp(target, "status") == 0) {
        audio_pipeline_stats_t stats;
        audio_pipeline_get_stats(&stats);
        usb_audio_status_t u_stats;
        usb_audio_get_status(&u_stats);

        cJSON *root = cJSON_CreateObject();
        cJSON_AddNumberToObject(root, "sample_rate", stats.sample_rate);
        cJSON_AddNumberToObject(root, "bit_depth", stats.bit_depth);
        cJSON_AddBoolToObject(root, "is_streaming", u_stats.is_streaming);
        cJSON_AddBoolToObject(root, "bit_perfect", stats.bit_perfect);
        cJSON_AddNumberToObject(root, "buffer_size", (double)stats.buffer_size_samples);
        float lat_ms = stats.sample_rate > 0 ? ((float)stats.buffer_size_samples / (stats.sample_rate / 1000.0f)) : 0.0f;
        cJSON_AddNumberToObject(root, "latency_ms", lat_ms);
        cJSON_AddNumberToObject(root, "underruns", stats.underrun_count);
        cJSON_AddNumberToObject(root, "overruns", stats.overrun_count);
        cJSON_AddBoolToObject(root, "wifi_active", wifi_ap_is_active());
        char cur_pname[PRESET_NAME_MAX_LEN] = "Default";
        preset_manager_get_current_name(cur_pname, sizeof(cur_pname));
        cJSON_AddStringToObject(root, "preset_name", cur_pname);

        char *str = cJSON_PrintUnformatted(root);
        cdc_resp_ok(str);
        free(str);
        cJSON_Delete(root);

    } else if (strcmp(target, "vu") == 0) {
        dsp_meter_values_t m;
        dsp_engine_get_meters(engine, &m);

        cJSON *root = cJSON_CreateObject();
        cJSON_AddNumberToObject(root, "in_l", m.in_peak_l);
        cJSON_AddNumberToObject(root, "in_r", m.in_peak_r);
        cJSON_AddNumberToObject(root, "out_l", m.out_peak_l);
        cJSON_AddNumberToObject(root, "out_r", m.out_peak_r);

        char *str = cJSON_PrintUnformatted(root);
        cdc_resp_ok(str);
        free(str);
        cJSON_Delete(root);

    } else if (strcmp(target, "wave") == 0) {
        float wave[64];
        dsp_engine_get_waveform(engine, wave, 64);

        cJSON *root = cJSON_CreateArray();
        for (int i = 0; i < 64; i++) {
            cJSON_AddItemToArray(root, cJSON_CreateNumber((double)wave[i]));
        }

        char *str = cJSON_PrintUnformatted(root);
        cdc_resp_ok(str);
        free(str);
        cJSON_Delete(root);

    } else if (strcmp(target, "presets") == 0) {
        size_t count = preset_manager_get_count();
        cJSON *root = cJSON_CreateArray();
        for (size_t i = 0; i < count; i++) {
            const preset_entry_t *p = preset_manager_get_preset(i);
            if (p) {
                cJSON *item = cJSON_CreateObject();
                cJSON_AddNumberToObject(item, "index", (double)i);
                cJSON_AddStringToObject(item, "name", p->name);
                cJSON_AddBoolToObject(item, "is_factory", p->is_factory);
                cJSON_AddItemToArray(root, item);
            }
        }

        char *str = cJSON_PrintUnformatted(root);
        cdc_resp_ok(str);
        free(str);
        cJSON_Delete(root);

    } else if (strcmp(target, "led") == 0) {
        neopixel_config_t lcfg;
        neopixel_get_config(&lcfg);

        cJSON *root = cJSON_CreateObject();
        cJSON_AddNumberToObject(root, "mode", lcfg.mode);
        cJSON_AddNumberToObject(root, "brightness", lcfg.brightness);

        char *str = cJSON_PrintUnformatted(root);
        cdc_resp_ok(str);
        free(str);
        cJSON_Delete(root);

    } else {
        cdc_resp_err("unknown json get target");
    }
}

static void handle_json_set(dsp_engine_t *engine, const char *target, const char *json_payload)
{
    if (!target || !json_payload) {
        cdc_resp_err("missing payload for json set <config|led> <json>");
        return;
    }

    cJSON *root = cJSON_Parse(json_payload);
    if (!root) {
        cdc_resp_err("invalid JSON payload");
        return;
    }

    if (strcmp(target, "config") == 0) {
        dsp_config_t cfg;
        dsp_engine_get_config(engine, &cfg);

        cJSON *item = NULL;
        if ((item = cJSON_GetObjectItem(root, "bit_perfect_bypass")) != NULL) {
            dsp_engine_set_bit_perfect(engine, cJSON_IsTrue(item));
        }
        if ((item = cJSON_GetObjectItem(root, "buffer_size")) != NULL) {
            audio_pipeline_set_buffer_size((audio_buffer_size_t)item->valueint);
        }
        if ((item = cJSON_GetObjectItem(root, "master_volume_db")) != NULL) {
            dsp_engine_set_master_volume(engine, (float)item->valuedouble);
        }

        // EQ
        cJSON *eq_obj = cJSON_GetObjectItem(root, "eq");
        if (eq_obj) {
            cJSON *en = cJSON_GetObjectItem(eq_obj, "enabled");
            if (en) cfg.eq.enabled = cJSON_IsTrue(en);
            cJSON *bands = cJSON_GetObjectItem(eq_obj, "bands");
            if (bands && cJSON_IsArray(bands)) {
                int count = cJSON_GetArraySize(bands);
                if (count > DSP_EQ_NUM_BANDS) count = DSP_EQ_NUM_BANDS;
                for (int i = 0; i < count; i++) {
                    cJSON *b = cJSON_GetArrayItem(bands, i);
                    cJSON *g = cJSON_GetObjectItem(b, "gain_db");
                    if (g) cfg.eq.bands[i].gain_db = (float)g->valuedouble;
                }
            }
        }

        // Compressor
        cJSON *comp_obj = cJSON_GetObjectItem(root, "comp");
        if (comp_obj) {
            cJSON *en = cJSON_GetObjectItem(comp_obj, "enabled");
            if (en) cfg.comp.enabled = cJSON_IsTrue(en);
            cJSON *th = cJSON_GetObjectItem(comp_obj, "threshold_db");
            if (th) cfg.comp.threshold_db = (float)th->valuedouble;
            cJSON *rt = cJSON_GetObjectItem(comp_obj, "ratio");
            if (rt) cfg.comp.ratio = (float)rt->valuedouble;
            cJSON *at = cJSON_GetObjectItem(comp_obj, "attack_ms");
            if (at) cfg.comp.attack_ms = (float)at->valuedouble;
            cJSON *rl = cJSON_GetObjectItem(comp_obj, "release_ms");
            if (rl) cfg.comp.release_ms = (float)rl->valuedouble;
        }

        // Limiter
        cJSON *lim_obj = cJSON_GetObjectItem(root, "limiter");
        if (lim_obj) {
            cJSON *en = cJSON_GetObjectItem(lim_obj, "enabled");
            if (en) cfg.limiter.enabled = cJSON_IsTrue(en);
            cJSON *c = cJSON_GetObjectItem(lim_obj, "ceiling_db");
            if (c) cfg.limiter.ceiling_db = (float)c->valuedouble;
            cJSON *rl = cJSON_GetObjectItem(lim_obj, "release_ms");
            if (rl) cfg.limiter.release_ms = (float)rl->valuedouble;
        }

        // Tape
        cJSON *tape_obj = cJSON_GetObjectItem(root, "tape");
        if (tape_obj) {
            cJSON *en = cJSON_GetObjectItem(tape_obj, "enabled");
            if (en) cfg.tape.enabled = cJSON_IsTrue(en);
            cJSON *dr = cJSON_GetObjectItem(tape_obj, "drive");
            if (dr) cfg.tape.drive = (float)dr->valuedouble;
            cJSON *hf = cJSON_GetObjectItem(tape_obj, "hf_cut_hz");
            if (hf) cfg.tape.hf_cut_hz = (float)hf->valuedouble;
            cJSON *hb = cJSON_GetObjectItem(tape_obj, "head_bump");
            if (hb) cfg.tape.head_bump = (float)hb->valuedouble;
            cJSON *hs = cJSON_GetObjectItem(tape_obj, "hiss_level");
            if (hs) cfg.tape.hiss_level = (float)hs->valuedouble;
            cJSON *cr = cJSON_GetObjectItem(tape_obj, "click_rate");
            if (cr) cfg.tape.click_rate = (float)cr->valuedouble;
            cJSON *cl = cJSON_GetObjectItem(tape_obj, "click_level");
            if (cl) cfg.tape.click_level = (float)cl->valuedouble;
            cJSON *wf = cJSON_GetObjectItem(tape_obj, "wow_flutter");
            if (wf) cfg.tape.wow_flutter = (float)wf->valuedouble;
        }

        // Bitcrusher
        cJSON *bc_obj = cJSON_GetObjectItem(root, "bitcrusher");
        if (bc_obj) {
            cJSON *en = cJSON_GetObjectItem(bc_obj, "enabled");
            if (en) cfg.bitcrusher.enabled = cJSON_IsTrue(en);
            cJSON *ed = cJSON_GetObjectItem(bc_obj, "enable_downsample");
            if (ed) cfg.bitcrusher.enable_downsample = cJSON_IsTrue(ed);
            cJSON *eq = cJSON_GetObjectItem(bc_obj, "enable_quantize");
            if (eq) cfg.bitcrusher.enable_quantize = cJSON_IsTrue(eq);
            cJSON *eo = cJSON_GetObjectItem(bc_obj, "enable_overflow");
            if (eo) cfg.bitcrusher.enable_overflow = cJSON_IsTrue(eo);
            cJSON *dt = cJSON_GetObjectItem(bc_obj, "dither");
            if (dt) cfg.bitcrusher.dither = cJSON_IsTrue(dt);
            cJSON *bd = cJSON_GetObjectItem(bc_obj, "bit_depth");
            if (bd) cfg.bitcrusher.bit_depth = (uint8_t)bd->valueint;
            cJSON *ds = cJSON_GetObjectItem(bc_obj, "downsample_rate");
            if (ds) cfg.bitcrusher.downsample_rate = (float)ds->valuedouble;
            cJSON *oi = cJSON_GetObjectItem(bc_obj, "overflow_intensity");
            if (oi) cfg.bitcrusher.overflow_intensity = (float)oi->valuedouble;
            cJSON *mx = cJSON_GetObjectItem(bc_obj, "mix");
            if (mx) cfg.bitcrusher.mix = (float)mx->valuedouble;
        }

        // Crossfeed
        cJSON *cf_obj = cJSON_GetObjectItem(root, "crossfeed");
        if (cf_obj) {
            cJSON *en = cJSON_GetObjectItem(cf_obj, "enabled");
            if (en) cfg.crossfeed.enabled = cJSON_IsTrue(en);
            cJSON *am = cJSON_GetObjectItem(cf_obj, "amount");
            if (am) cfg.crossfeed.amount = (float)am->valuedouble;
            cJSON *cf_f = cJSON_GetObjectItem(cf_obj, "cutoff_hz");
            if (cf_f) cfg.crossfeed.cutoff_hz = (float)cf_f->valuedouble;
        }

        // Widener
        cJSON *w_obj = cJSON_GetObjectItem(root, "widener");
        if (w_obj) {
            cJSON *en = cJSON_GetObjectItem(w_obj, "enabled");
            if (en) cfg.widener.enabled = cJSON_IsTrue(en);
            cJSON *w = cJSON_GetObjectItem(w_obj, "width");
            if (w) cfg.widener.width = (float)w->valuedouble;
        }

        // Bass
        cJSON *bass_obj = cJSON_GetObjectItem(root, "bass");
        if (bass_obj) {
            cJSON *en = cJSON_GetObjectItem(bass_obj, "enabled");
            if (en) cfg.bass.enabled = cJSON_IsTrue(en);
            cJSON *cf_f = cJSON_GetObjectItem(bass_obj, "cutoff_hz");
            if (cf_f) cfg.bass.cutoff_hz = (float)cf_f->valuedouble;
            cJSON *dr = cJSON_GetObjectItem(bass_obj, "drive");
            if (dr) cfg.bass.drive = (float)dr->valuedouble;
            cJSON *bl = cJSON_GetObjectItem(bass_obj, "blend");
            if (bl) cfg.bass.blend = (float)bl->valuedouble;
        }

        // De-esser
        cJSON *deess_obj = cJSON_GetObjectItem(root, "deesser");
        if (deess_obj) {
            cJSON *en = cJSON_GetObjectItem(deess_obj, "enabled");
            if (en) cfg.deesser.enabled = cJSON_IsTrue(en);
            cJSON *f = cJSON_GetObjectItem(deess_obj, "freq");
            if (f) cfg.deesser.freq = (float)f->valuedouble;
            cJSON *th = cJSON_GetObjectItem(deess_obj, "threshold_db");
            if (th) cfg.deesser.threshold_db = (float)th->valuedouble;
        }

        // Pitch
        cJSON *pitch_obj = cJSON_GetObjectItem(root, "pitch");
        if (pitch_obj) {
            cJSON *en = cJSON_GetObjectItem(pitch_obj, "enabled");
            if (en) cfg.pitch.enabled = cJSON_IsTrue(en);
            cJSON *st = cJSON_GetObjectItem(pitch_obj, "semitones");
            if (st) cfg.pitch.semitones = (int8_t)st->valueint;
            cJSON *c = cJSON_GetObjectItem(pitch_obj, "cents");
            if (c) cfg.pitch.cents = (int8_t)c->valueint;
        }

        // Loudness
        cJSON *loud_obj = cJSON_GetObjectItem(root, "loudness");
        if (loud_obj) {
            cJSON *en = cJSON_GetObjectItem(loud_obj, "enabled");
            if (en) cfg.loudness.enabled = cJSON_IsTrue(en);
            cJSON *t = cJSON_GetObjectItem(loud_obj, "target_db");
            if (t) cfg.loudness.target_db = (float)t->valuedouble;
        }

        // FX
        cJSON *fx_obj = cJSON_GetObjectItem(root, "fx");
        if (fx_obj) {
            cJSON *den = cJSON_GetObjectItem(fx_obj, "delay_enabled");
            if (den) cfg.fx.delay_enabled = cJSON_IsTrue(den);
            cJSON *dt = cJSON_GetObjectItem(fx_obj, "delay_time_ms");
            if (dt) cfg.fx.delay_time_ms = (float)dt->valuedouble;
            cJSON *dfb = cJSON_GetObjectItem(fx_obj, "delay_feedback");
            if (dfb) cfg.fx.delay_feedback = (float)dfb->valuedouble;
            cJSON *dmx = cJSON_GetObjectItem(fx_obj, "delay_mix");
            if (dmx) cfg.fx.delay_mix = (float)dmx->valuedouble;

            cJSON *ren = cJSON_GetObjectItem(fx_obj, "reverb_enabled");
            if (ren) cfg.fx.reverb_enabled = cJSON_IsTrue(ren);
            cJSON *rsz = cJSON_GetObjectItem(fx_obj, "reverb_room_size");
            if (rsz) cfg.fx.reverb_room_size = (float)rsz->valuedouble;
            cJSON *rdm = cJSON_GetObjectItem(fx_obj, "reverb_damping");
            if (rdm) cfg.fx.reverb_damping = (float)rdm->valuedouble;
            cJSON *rmx = cJSON_GetObjectItem(fx_obj, "reverb_mix");
            if (rmx) cfg.fx.reverb_mix = (float)rmx->valuedouble;
        }

        dsp_engine_set_config(engine, &cfg);
        cdc_resp_ok("config updated");

    } else if (strcmp(target, "led") == 0) {
        cJSON *m = cJSON_GetObjectItem(root, "mode");
        cJSON *b = cJSON_GetObjectItem(root, "brightness");
        if (m) neopixel_set_mode((neopixel_mode_t)m->valueint);
        if (b) neopixel_set_brightness((uint8_t)b->valueint);
        cdc_resp_ok("led updated");
    } else {
        cdc_resp_err("unknown json set target");
    }

    cJSON_Delete(root);
}

/* --------------------------------------------------------------------------
 * Main Command Dispatcher
 * -------------------------------------------------------------------------- */
static void handle_command(char *line)
{
    // Trim leading spaces
    while (*line == ' ' || *line == '\t') line++;
    if (*line == '\0') return;

    dsp_engine_t *engine = audio_pipeline_get_dsp_engine();

    // Check for JSON commands first (which might contain spaces in payload)
    if (strncmp(line, "json ", 5) == 0) {
        char *sub = line + 5;
        while (*sub == ' ') sub++;
        if (strncmp(sub, "get ", 4) == 0) {
            char *target = sub + 4;
            while (*target == ' ') target++;
            char *end = target;
            while (*end && *end != ' ' && *end != '\r' && *end != '\n') end++;
            *end = '\0';
            handle_json_get(engine, target);
            return;
        } else if (strncmp(sub, "set ", 4) == 0) {
            char *target = sub + 4;
            while (*target == ' ') target++;
            char *payload = strchr(target, ' ');
            if (payload) {
                *payload = '\0';
                payload++;
                while (*payload == ' ') payload++;
                // Trim trailing newline from payload
                size_t plen = strlen(payload);
                while (plen > 0 && (payload[plen - 1] == '\r' || payload[plen - 1] == '\n')) {
                    payload[--plen] = '\0';
                }
                handle_json_set(engine, target, payload);
            } else {
                cdc_resp_err("missing payload for json set");
            }
            return;
        }
    }

    // Tokenized commands
    char *cmd = strtok(line, " \t\r\n");
    if (!cmd) return;

    if (strcmp(cmd, "help") == 0) {
        print_help();
    } else if (strcmp(cmd, "echo") == 0) {
        char *val = strtok(NULL, " \t\r\n");
        if (val) {
            if (strcmp(val, "off") == 0 || strcmp(val, "0") == 0) {
                s_echo_enabled = false;
                cdc_printf("OK echo disabled\r\n");
            } else {
                s_echo_enabled = true;
                cdc_printf("OK echo enabled\r\n");
            }
        } else {
            cdc_printf("Echo: %s\r\n", s_echo_enabled ? "ON" : "OFF");
        }
    } else if (strcmp(cmd, "status") == 0) {
        audio_pipeline_stats_t stats;
        audio_pipeline_get_stats(&stats);
        usb_audio_status_t u_stats;
        usb_audio_get_status(&u_stats);

        cdc_printf("\r\n--- Status Report ---\r\n");
        cdc_printf("Sample Rate : %" PRIu32 " Hz\r\n", stats.sample_rate);
        cdc_printf("Bit Depth   : %d-bit PCM (%s)\r\n", stats.bit_depth, stats.bit_depth == 24 ? "in 32-bit slot" : "standard");
        cdc_printf("Bit-Perfect : %s\r\n", stats.bit_perfect ? "ACTIVE (Direct DAC)" : "OFF (DSP Chain Active)");
        cdc_printf("Buffer Size : %d samples (~%.1f ms)\r\n", (int)stats.buffer_size_samples, stats.sample_rate > 0 ? ((float)stats.buffer_size_samples / (stats.sample_rate / 1000.0f)) : 0.0f);
        cdc_printf("Streaming   : %s\r\n", u_stats.is_streaming ? "YES (Active)" : "NO (Idle)");
        cdc_printf("Underruns   : %" PRIu32 " | Overruns: %" PRIu32 "\r\n", stats.underrun_count, stats.overrun_count);
        char cur_pname[PRESET_NAME_MAX_LEN] = "Default";
        preset_manager_get_current_name(cur_pname, sizeof(cur_pname));
        cdc_printf("Preset      : %s\r\n", cur_pname);
        cdc_printf("Wi-Fi SoftAP: %s (Stations: %d)\r\n", wifi_ap_is_active() ? "ON (192.168.4.1)" : "OFF (Zero RF Noise)", wifi_ap_get_station_count());
        cdc_printf("---------------------\r\n");
    } else if (strcmp(cmd, "bypass") == 0) {
        char *arg = strtok(NULL, " \t\r\n");
        if (arg) {
            bool on = (strcmp(arg, "on") == 0 || strcmp(arg, "1") == 0);
            dsp_engine_set_bit_perfect(engine, on);
        }
        if (s_echo_enabled) {
            cdc_printf("Bit-Perfect Bypass: %s\r\n", dsp_engine_is_bit_perfect(engine) ? "ON (DIRECT)" : "OFF");
        } else {
            cdc_resp_ok(dsp_engine_is_bit_perfect(engine) ? "bypass:on" : "bypass:off");
        }
    } else if (strcmp(cmd, "buffer") == 0) {
        char *arg = strtok(NULL, " \t\r\n");
        if (arg) {
            int sz = atoi(arg);
            audio_pipeline_set_buffer_size((audio_buffer_size_t)sz);
        }
        cdc_resp_ok(arg ? "buffer size updated" : "buffer size query");
    } else if (strcmp(cmd, "vol") == 0) {
        char *arg = strtok(NULL, " \t\r\n");
        if (arg) {
            float v = atof(arg);
            dsp_engine_set_master_volume(engine, v);
            cdc_resp_ok("volume updated");
        } else {
            dsp_config_t cfg;
            dsp_engine_get_config(engine, &cfg);
            cdc_printf("%.1f dB\r\n", cfg.master_volume_db);
        }
    } else if (strcmp(cmd, "eq") == 0) {
        char *arg1 = strtok(NULL, " \t\r\n");
        if (!arg1 || strcmp(arg1, "list") == 0) {
            dsp_config_t cfg;
            dsp_engine_get_config(engine, &cfg);
            cdc_printf("10-Band EQ (%s):\r\n", cfg.eq.enabled ? "ENABLED" : "BYPASSED");
            for (int i = 0; i < DSP_EQ_NUM_BANDS; i++) {
                cdc_printf("  [%d] %6.0f Hz : %+.1f dB\r\n", i, cfg.eq.bands[i].freq, cfg.eq.bands[i].gain_db);
            }
        } else if (strcmp(arg1, "all") == 0) {
            dsp_config_t cfg;
            dsp_engine_get_config(engine, &cfg);
            for (int i = 0; i < DSP_EQ_NUM_BANDS; i++) {
                char *g = strtok(NULL, " \t\r\n");
                if (g) cfg.eq.bands[i].gain_db = atof(g);
            }
            dsp_engine_set_config(engine, &cfg);
            cdc_resp_ok("all EQ bands updated");
        } else {
            int b = atoi(arg1);
            char *g_str = strtok(NULL, " \t\r\n");
            if (b >= 0 && b < DSP_EQ_NUM_BANDS) {
                if (g_str) {
                    float g = atof(g_str);
                    dsp_eq_set_band(&engine->eq, b, g, 0.0f, 0.0f, false);
                    cdc_resp_ok("band updated");
                } else {
                    dsp_config_t cfg;
                    dsp_engine_get_config(engine, &cfg);
                    cdc_printf("Band %d (%.0f Hz): %+.1f dB\r\n", b, cfg.eq.bands[b].freq, cfg.eq.bands[b].gain_db);
                }
            } else {
                cdc_resp_err("invalid band index (0-9)");
            }
        }
    } else if (strcmp(cmd, "fx") == 0) {
        char *fx_name = strtok(NULL, " \t\r\n");
        char *param = strtok(NULL, " \t\r\n");
        char *val = strtok(NULL, " \t\r\n");

        if (fx_name && param) {
            dsp_config_t cfg;
            dsp_engine_get_config(engine, &cfg);

            // Simple ON/OFF toggle: fx <name> <on|off|1|0>
            if (!val && (strcmp(param, "on") == 0 || strcmp(param, "off") == 0 || strcmp(param, "1") == 0 || strcmp(param, "0") == 0)) {
                bool on = (strcmp(param, "on") == 0 || strcmp(param, "1") == 0);
                if (strcmp(fx_name, "tape") == 0) cfg.tape.enabled = on;
                else if (strcmp(fx_name, "crush") == 0) cfg.bitcrusher.enabled = on;
                else if (strcmp(fx_name, "comp") == 0) cfg.comp.enabled = on;
                else if (strcmp(fx_name, "lim") == 0) cfg.limiter.enabled = on;
                else if (strcmp(fx_name, "cross") == 0) cfg.crossfeed.enabled = on;
                else if (strcmp(fx_name, "wide") == 0) cfg.widener.enabled = on;
                else if (strcmp(fx_name, "bass") == 0) cfg.bass.enabled = on;
                else if (strcmp(fx_name, "deess") == 0) cfg.deesser.enabled = on;
                else if (strcmp(fx_name, "pitch") == 0) cfg.pitch.enabled = on;
                else if (strcmp(fx_name, "loud") == 0) cfg.loudness.enabled = on;
                else if (strcmp(fx_name, "rev") == 0) cfg.fx.reverb_enabled = on;
                else if (strcmp(fx_name, "delay") == 0) cfg.fx.delay_enabled = on;
                else if (strcmp(fx_name, "eq") == 0) cfg.eq.enabled = on;
                else {
                    cdc_resp_err("unknown fx name");
                    return;
                }
                dsp_engine_set_config(engine, &cfg);
                cdc_resp_ok("fx state updated");
                return;
            }

            // Parameter tuning: fx <name> <param> <val>
            if (!val) {
                cdc_resp_err("missing parameter value");
                return;
            }

            float fval = atof(val);

            if (strcmp(fx_name, "tape") == 0) {
                if (strcmp(param, "drive") == 0) cfg.tape.drive = fval;
                else if (strcmp(param, "hf") == 0) cfg.tape.hf_cut_hz = fval;
                else if (strcmp(param, "bump") == 0) cfg.tape.head_bump = fval;
                else if (strcmp(param, "hiss") == 0) cfg.tape.hiss_level = fval;
                else if (strcmp(param, "clicks") == 0) cfg.tape.click_rate = fval;
                else if (strcmp(param, "camp") == 0) cfg.tape.click_level = fval;
                else if (strcmp(param, "wow") == 0) cfg.tape.wow_flutter = fval;
                else { cdc_resp_err("unknown tape parameter"); return; }

            } else if (strcmp(fx_name, "crush") == 0) {
                if (strcmp(param, "srr") == 0) cfg.bitcrusher.enable_downsample = (atoi(val) != 0 || strcmp(val, "on") == 0);
                else if (strcmp(param, "quant") == 0) cfg.bitcrusher.enable_quantize = (atoi(val) != 0 || strcmp(val, "on") == 0);
                else if (strcmp(param, "ovf") == 0) cfg.bitcrusher.enable_overflow = (atoi(val) != 0 || strcmp(val, "on") == 0);
                else if (strcmp(param, "dither") == 0) cfg.bitcrusher.dither = (atoi(val) != 0 || strcmp(val, "on") == 0);
                else if (strcmp(param, "rate") == 0) cfg.bitcrusher.downsample_rate = fval;
                else if (strcmp(param, "bits") == 0) cfg.bitcrusher.bit_depth = (uint8_t)atoi(val);
                else if (strcmp(param, "intensity") == 0) cfg.bitcrusher.overflow_intensity = fval;
                else if (strcmp(param, "mix") == 0) cfg.bitcrusher.mix = fval;
                else { cdc_resp_err("unknown crush parameter"); return; }

            } else if (strcmp(fx_name, "comp") == 0) {
                if (strcmp(param, "thresh") == 0) cfg.comp.threshold_db = fval;
                else if (strcmp(param, "ratio") == 0) cfg.comp.ratio = fval;
                else if (strcmp(param, "attack") == 0) cfg.comp.attack_ms = fval;
                else if (strcmp(param, "release") == 0) cfg.comp.release_ms = fval;
                else { cdc_resp_err("unknown comp parameter"); return; }

            } else if (strcmp(fx_name, "lim") == 0) {
                if (strcmp(param, "ceiling") == 0) cfg.limiter.ceiling_db = fval;
                else if (strcmp(param, "release") == 0) cfg.limiter.release_ms = fval;
                else { cdc_resp_err("unknown limiter parameter"); return; }

            } else if (strcmp(fx_name, "bass") == 0) {
                if (strcmp(param, "cutoff") == 0) cfg.bass.cutoff_hz = fval;
                else if (strcmp(param, "drive") == 0) cfg.bass.drive = fval;
                else if (strcmp(param, "blend") == 0) cfg.bass.blend = fval;
                else { cdc_resp_err("unknown bass parameter"); return; }

            } else if (strcmp(fx_name, "wide") == 0) {
                if (strcmp(param, "width") == 0) cfg.widener.width = fval;
                else { cdc_resp_err("unknown widener parameter"); return; }

            } else if (strcmp(fx_name, "cross") == 0) {
                if (strcmp(param, "amount") == 0) cfg.crossfeed.amount = fval;
                else if (strcmp(param, "cutoff") == 0) cfg.crossfeed.cutoff_hz = fval;
                else { cdc_resp_err("unknown crossfeed parameter"); return; }

            } else if (strcmp(fx_name, "deess") == 0) {
                if (strcmp(param, "freq") == 0) cfg.deesser.freq = fval;
                else if (strcmp(param, "thresh") == 0) cfg.deesser.threshold_db = fval;
                else { cdc_resp_err("unknown deesser parameter"); return; }

            } else if (strcmp(fx_name, "pitch") == 0) {
                if (strcmp(param, "semi") == 0) cfg.pitch.semitones = (int8_t)atoi(val);
                else if (strcmp(param, "cents") == 0) cfg.pitch.cents = (int8_t)atoi(val);
                else { cdc_resp_err("unknown pitch parameter"); return; }

            } else if (strcmp(fx_name, "loud") == 0) {
                if (strcmp(param, "target") == 0) cfg.loudness.target_db = fval;
                else { cdc_resp_err("unknown loudness parameter"); return; }

            } else if (strcmp(fx_name, "rev") == 0) {
                if (strcmp(param, "size") == 0) cfg.fx.reverb_room_size = fval;
                else if (strcmp(param, "mix") == 0) cfg.fx.reverb_mix = fval;
                else { cdc_resp_err("unknown reverb parameter"); return; }

            } else if (strcmp(fx_name, "delay") == 0) {
                if (strcmp(param, "time") == 0) cfg.fx.delay_time_ms = fval;
                else if (strcmp(param, "feedback") == 0) cfg.fx.delay_feedback = fval;
                else if (strcmp(param, "mix") == 0) cfg.fx.delay_mix = fval;
                else { cdc_resp_err("unknown delay parameter"); return; }

            } else {
                cdc_resp_err("unknown fx module");
                return;
            }

            dsp_engine_set_config(engine, &cfg);
            cdc_resp_ok("fx parameter updated");
        } else {
            cdc_resp_err("usage: fx <module> <on|off> OR fx <module> <param> <val>");
        }
    } else if (strcmp(cmd, "stream") == 0) {
        char *target = strtok(NULL, " \t\r\n");
        char *ms_str = strtok(NULL, " \t\r\n");
        if (target && strcmp(target, "vu") == 0) {
            s_stream_type = STREAM_VU;
            s_stream_interval_ms = ms_str ? (uint32_t)atoi(ms_str) : 50;
            if (s_stream_interval_ms < 10) s_stream_interval_ms = 10;
            cdc_resp_ok("stream vu started");
        } else if (target && strcmp(target, "wave") == 0) {
            s_stream_type = STREAM_WAVE;
            s_stream_interval_ms = ms_str ? (uint32_t)atoi(ms_str) : 40;
            if (s_stream_interval_ms < 15) s_stream_interval_ms = 15;
            cdc_resp_ok("stream wave started");
        } else if (target && strcmp(target, "stop") == 0) {
            s_stream_type = STREAM_NONE;
            cdc_resp_ok("stream stopped");
        } else {
            cdc_resp_err("usage: stream <vu|wave|stop> [interval_ms]");
        }
    } else if (strcmp(cmd, "vu") == 0) {
        dsp_meter_values_t m;
        dsp_engine_get_meters(engine, &m);
        cdc_printf("In L: %.1f dB | In R: %.1f dB || Out L: %.1f dB | Out R: %.1f dB\r\n",
                   m.in_peak_l, m.in_peak_r, m.out_peak_l, m.out_peak_r);
    } else if (strcmp(cmd, "led") == 0) {
        char *m_str = strtok(NULL, " \t\r\n");
        char *b_str = strtok(NULL, " \t\r\n");
        if (m_str) {
            int m = atoi(m_str);
            neopixel_set_mode((neopixel_mode_t)m);
            if (b_str) {
                int b = atoi(b_str);
                neopixel_set_brightness((uint8_t)b);
            }
            cdc_resp_ok("led updated");
        } else {
            neopixel_config_t lcfg;
            neopixel_get_config(&lcfg);
            cdc_printf("LED Mode: %d, Brightness: %d%%\r\n", lcfg.mode, lcfg.brightness);
        }
    } else if (strcmp(cmd, "preset") == 0) {
        char *sub = strtok(NULL, " \t\r\n");
        if (sub && strcmp(sub, "list") == 0) {
            size_t cnt = preset_manager_get_count();
            for (size_t i = 0; i < cnt; i++) {
                const preset_entry_t *p = preset_manager_get_preset(i);
                if (p) cdc_printf("[%d] %s (%s)\r\n", (int)i, p->name, p->is_factory ? "Factory" : "User");
            }
        } else if (sub && strcmp(sub, "load") == 0) {
            char *idx_str = strtok(NULL, " \t\r\n");
            if (idx_str) {
                int idx = atoi(idx_str);
                dsp_config_t cfg;
                if (preset_manager_load(idx, &cfg) == ESP_OK) {
                    dsp_engine_set_config(engine, &cfg);
                    cdc_resp_ok("preset loaded");
                } else {
                    cdc_resp_err("failed to load preset");
                }
            }
        } else if (sub && strcmp(sub, "save") == 0) {
            char *idx_str = strtok(NULL, " \t\r\n");
            char *pname = strtok(NULL, "\r\n");
            if (idx_str) {
                int idx = atoi(idx_str);
                dsp_config_t cfg;
                dsp_engine_get_config(engine, &cfg);
                if (preset_manager_save(idx, pname ? pname : "Custom Preset", &cfg) == ESP_OK) {
                    cdc_resp_ok("preset saved");
                } else {
                    cdc_resp_err("cannot save to factory preset slot");
                }
            }
        } else if (sub && strcmp(sub, "reset") == 0) {
            preset_manager_reset_defaults();
            cdc_resp_ok("presets reset to factory defaults");
        }
    } else if (strcmp(cmd, "wifi") == 0) {
        char *arg = strtok(NULL, " \t\r\n");
        if (arg) {
            if (strcmp(arg, "on") == 0) wifi_ap_start();
            else if (strcmp(arg, "off") == 0) wifi_ap_stop();
            else if (strcmp(arg, "toggle") == 0) wifi_ap_toggle();
        }
        cdc_printf("Wi-Fi SoftAP: %s\r\n", wifi_ap_is_active() ? "ON" : "OFF");
    } else if (strcmp(cmd, "pins") == 0) {
        char *sub = strtok(NULL, " \t\r\n");
        if (!sub || strcmp(sub, "info") == 0) {
            const hw_config_t *cfg = hw_config_get();
            cdc_printf("Hardware Pinout:\r\n");
            cdc_printf("  I2S BCK : GPIO %d\r\n", cfg->i2s_bck_gpio);
            cdc_printf("  I2S DIN : GPIO %d\r\n", cfg->i2s_din_gpio);
            cdc_printf("  I2S WS  : GPIO %d\r\n", cfg->i2s_ws_gpio);
            cdc_printf("  OLED SCL: GPIO %d\r\n", cfg->oled_scl_gpio);
            cdc_printf("  OLED SDA: GPIO %d\r\n", cfg->oled_sda_gpio);
            cdc_printf("  OLED    : %s\r\n", cfg->oled_enabled ? "Enabled" : "Disabled");
            cdc_printf("  NeoPixel: GPIO %d\r\n", cfg->neopixel_gpio);
            cdc_printf("  BOOT Btn: GPIO %d\r\n", cfg->boot_button_gpio);
            cdc_printf("  Wi-Fi   : SSID '%s' (Ch %d)\r\n", cfg->wifi_ssid, cfg->wifi_channel);
            cdc_printf("  OLED FPS: %d\r\n", cfg->oled_fps);
            cdc_printf("  Web FPS : %d\r\n", cfg->web_scope_fps);
            cdc_printf("  Scope   : %.2fx gain, %s\r\n", cfg->scope_gain, cfg->scope_pre_vol ? "Pre-Volume (Studio)" : "Post-Volume");
        } else if (strcmp(sub, "set") == 0) {
            char *b_str = strtok(NULL, " \t\r\n");
            char *d_str = strtok(NULL, " \t\r\n");
            char *w_str = strtok(NULL, " \t\r\n");
            if (b_str && d_str && w_str) {
                hw_config_t cfg = *hw_config_get();
                cfg.i2s_bck_gpio = atoi(b_str);
                cfg.i2s_din_gpio = atoi(d_str);
                cfg.i2s_ws_gpio = atoi(w_str);
                char *scl_str = strtok(NULL, " \t\r\n");
                char *sda_str = strtok(NULL, " \t\r\n");
                if (scl_str) cfg.oled_scl_gpio = atoi(scl_str);
                if (sda_str) cfg.oled_sda_gpio = atoi(sda_str);
                hw_config_set(&cfg);
                cdc_printf("Pins saved. Rebooting...\r\n");
                vTaskDelay(pdMS_TO_TICKS(500));
                esp_restart();
            } else {
                cdc_resp_err("usage: pins set <bck> <din> <ws> [scl] [sda]");
            }
        } else if (strcmp(sub, "reset") == 0) {
            hw_config_reset_defaults();
            cdc_printf("Hardware config reset to defaults. Rebooting...\r\n");
            vTaskDelay(pdMS_TO_TICKS(500));
            esp_restart();
        }
    } else if (strcmp(cmd, "scope") == 0) {
        char *sub = strtok(NULL, " \t\r\n");
        if (!sub) {
            const hw_config_t *cfg = hw_config_get();
            cdc_printf("Oscilloscope Calibration:\r\n");
            cdc_printf("  Gain: %.2fx (0.25 - 10.0)\r\n", cfg->scope_gain);
            cdc_printf("  Tap : %s\r\n", cfg->scope_pre_vol ? "Pre-Volume (Studio)" : "Post-Volume");
            cdc_printf("  Web FPS: %d\r\n", cfg->web_scope_fps);
        } else if (strcmp(sub, "gain") == 0) {
            char *g_str = strtok(NULL, " \t\r\n");
            if (g_str) {
                float g = atof(g_str);
                if (g >= 0.25f && g <= 10.0f) {
                    hw_config_t cfg = *hw_config_get();
                    cfg.scope_gain = g;
                    hw_config_set(&cfg);
                    cdc_resp_ok("scope gain updated");
                } else {
                    cdc_resp_err("gain must be between 0.25 and 10.0");
                }
            } else {
                cdc_resp_err("usage: scope gain <0.25..10.0>");
            }
        } else if (strcmp(sub, "mode") == 0) {
            char *m_str = strtok(NULL, " \t\r\n");
            if (m_str) {
                hw_config_t cfg = *hw_config_get();
                if (strcmp(m_str, "pre") == 0 || strcmp(m_str, "studio") == 0 || strcmp(m_str, "1") == 0) {
                    cfg.scope_pre_vol = true;
                    hw_config_set(&cfg);
                    cdc_resp_ok("scope set to pre-volume");
                } else if (strcmp(m_str, "post") == 0 || strcmp(m_str, "0") == 0) {
                    cfg.scope_pre_vol = false;
                    hw_config_set(&cfg);
                    cdc_resp_ok("scope set to post-volume");
                } else {
                    cdc_resp_err("mode must be pre or post");
                }
            } else {
                cdc_resp_err("usage: scope mode <pre|post>");
            }
        } else {
            cdc_resp_err("usage: scope [gain <x> | mode <pre|post>]");
        }
    } else if (strcmp(cmd, "oled") == 0 || strcmp(cmd, "hw") == 0) {
        char *sub = strtok(NULL, " \t\r\n");
        if (strcmp(cmd, "hw") == 0) {
            if (sub && strcmp(sub, "oled") == 0) {
                sub = strtok(NULL, " \t\r\n");
            } else {
                cdc_resp_err("usage: hw oled <enable|disable|fps <15-60>>");
                return;
            }
        }
        if (sub && (strcmp(sub, "enable") == 0 || strcmp(sub, "on") == 0 || strcmp(sub, "1") == 0)) {
            hw_config_t cfg = *hw_config_get();
            cfg.oled_enabled = true;
            hw_config_set(&cfg);
            cdc_resp_ok("oled enabled");
        } else if (sub && (strcmp(sub, "disable") == 0 || strcmp(sub, "off") == 0 || strcmp(sub, "0") == 0)) {
            hw_config_t cfg = *hw_config_get();
            cfg.oled_enabled = false;
            hw_config_set(&cfg);
            cdc_resp_ok("oled disabled");
        } else if (sub && strcmp(sub, "fps") == 0) {
            char *fps_str = strtok(NULL, " \t\r\n");
            if (fps_str) {
                int fps = atoi(fps_str);
                if (fps >= 15 && fps <= 60) {
                    hw_config_t cfg = *hw_config_get();
                    cfg.oled_fps = (uint8_t)fps;
                    hw_config_set(&cfg);
                    cdc_resp_ok("oled fps updated");
                } else {
                    cdc_resp_err("fps must be between 15 and 60");
                }
            } else {
                cdc_printf("OLED FPS: %d\r\n", hw_config_get()->oled_fps);
            }
        } else {
            const hw_config_t *cfg = hw_config_get();
            cdc_printf("OLED: %s, FPS: %d\r\n", cfg->oled_enabled ? "Enabled" : "Disabled", cfg->oled_fps);
        }
    } else if (strcmp(cmd, "storage") == 0) {
        char *sub = strtok(NULL, " \t\r\n");
        if (!sub || strcmp(sub, "info") == 0) {
            size_t total = 0, used = 0;
            if (fs_manager_get_info(&total, &used) == ESP_OK) {
                cdc_printf("\r\n--- LittleFS Flash Storage ---\r\n");
                cdc_printf("Status      : Mounted at " FS_STORAGE_MOUNT_POINT "\r\n");
                cdc_printf("Total Space : %u bytes (~%u KB / %.2f MB)\r\n", (unsigned int)total, (unsigned int)(total / 1024), (double)total / (1024.0 * 1024.0));
                cdc_printf("Used Space  : %u bytes (~%u KB)\r\n", (unsigned int)used, (unsigned int)(used / 1024));
                cdc_printf("Free Space  : %u bytes (~%u KB / %.2f MB)\r\n", (unsigned int)(total - used), (unsigned int)((total - used) / 1024), (double)(total - used) / (1024.0 * 1024.0));
                cdc_printf("Utilization : %.1f%%\r\n", total > 0 ? ((double)used / (double)total * 100.0) : 0.0);
                cdc_printf("------------------------------\r\n");
            } else {
                cdc_resp_err("LittleFS not mounted");
            }
        } else if (strcmp(sub, "ls") == 0) {
            char *dir_arg = strtok(NULL, " \t\r\n");
            const char *path = dir_arg ? dir_arg : FS_STORAGE_MOUNT_POINT;
            DIR *d = opendir(path);
            if (!d) {
                cdc_resp_err("failed to open directory");
            } else {
                cdc_printf("\r\nDirectory listing of '%s':\r\n", path);
                struct dirent *de;
                int count = 0;
                while ((de = readdir(d)) != NULL) {
                    char fullpath[512];
                    snprintf(fullpath, sizeof(fullpath), "%s/%s", path, de->d_name);
                    struct stat st;
                    if (stat(fullpath, &st) == 0) {
                        if (S_ISDIR(st.st_mode)) {
                            cdc_printf("  <DIR>  %-24s\r\n", de->d_name);
                        } else {
                            cdc_printf("  %6ld %-24s\r\n", (long)st.st_size, de->d_name);
                        }
                    }
                    count++;
                }
                closedir(d);
                cdc_printf("Total %d entries.\r\n", count);
            }
        } else if (strcmp(sub, "cat") == 0) {
            char *fpath = strtok(NULL, " \t\r\n");
            if (!fpath) {
                cdc_resp_err("usage: storage cat <file_path>");
            } else {
                FILE *f = fopen(fpath, "r");
                if (!f) {
                    cdc_resp_err("cannot open file");
                } else {
                    char readbuf[64];
                    size_t nr;
                    while ((nr = fread(readbuf, 1, sizeof(readbuf) - 1, f)) > 0) {
                        readbuf[nr] = '\0';
                        cdc_printf("%s", readbuf);
                    }
                    fclose(f);
                    cdc_printf("\r\n");
                }
            }
        } else if (strcmp(sub, "format") == 0) {
            if (fs_manager_format() == ESP_OK) {
                cdc_resp_ok("filesystem formatted");
            } else {
                cdc_resp_err("format failed");
            }
        }
    } else {
        cdc_resp_err("unknown command");
    }

    if (s_echo_enabled) {
        cdc_printf("DSP32> ");
    }
}

/* --------------------------------------------------------------------------
 * Periodic Stream Processor
 * -------------------------------------------------------------------------- */
static void process_stream(dsp_engine_t *engine)
{
    if (s_stream_type == STREAM_NONE || !tud_cdc_connected()) {
        return;
    }

    int64_t now_us = esp_timer_get_time();
    int64_t interval_us = (int64_t)s_stream_interval_ms * 1000;

    if (now_us - s_last_stream_time_us >= interval_us) {
        s_last_stream_time_us = now_us;

        if (s_stream_type == STREAM_VU) {
            dsp_meter_values_t m;
            dsp_engine_get_meters(engine, &m);
            cdc_printf("$VU:%.1f,%.1f,%.1f,%.1f\n", m.in_peak_l, m.in_peak_r, m.out_peak_l, m.out_peak_r);
        } else if (s_stream_type == STREAM_WAVE) {
            float wave[64];
            dsp_engine_get_waveform(engine, wave, 64);
            char wave_str[384];
            int pos = snprintf(wave_str, sizeof(wave_str), "$WAVE:");
            for (int i = 0; i < 64 && pos < sizeof(wave_str) - 8; i++) {
                pos += snprintf(wave_str + pos, sizeof(wave_str) - pos, "%.2f%s", wave[i], (i == 63) ? "" : ",");
            }
            snprintf(wave_str + pos, sizeof(wave_str) - pos, "\n");
            cdc_send_raw(wave_str, strlen(wave_str));
        }
    }
}

/* --------------------------------------------------------------------------
 * FreeRTOS Background Task
 * -------------------------------------------------------------------------- */
static void serial_cli_task(void *pvParameters)
{
    char line_buf[CLI_LINE_BUF_SZ];
    size_t line_pos = 0;
    dsp_engine_t *engine = audio_pipeline_get_dsp_engine();

    while (1) {
        // 1. Process active streaming
        process_stream(engine);

        // 2. Read incoming commands
        if (tud_cdc_available()) {
            uint8_t ch = 0;
            if (tud_cdc_read(&ch, 1) == 1) {
                if (s_echo_enabled) {
                    tud_cdc_write(&ch, 1);
                    tud_cdc_write_flush();
                }

                if (ch == '\r' || ch == '\n') {
                    if (s_echo_enabled) {
                        cdc_printf("\r\n");
                    }
                    line_buf[line_pos] = '\0';
                    if (line_pos > 0) {
                        handle_command(line_buf);
                        line_pos = 0;
                    } else if (s_echo_enabled) {
                        cdc_printf("DSP32> ");
                    }
                } else if (ch == '\b' || ch == 127) {
                    if (line_pos > 0) {
                        line_pos--;
                        if (s_echo_enabled) {
                            cdc_printf(" \b");
                        }
                    }
                } else if (line_pos < CLI_LINE_BUF_SZ - 1) {
                    line_buf[line_pos++] = (char)ch;
                }
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
}
#endif

esp_err_t serial_cli_init(void)
{
#if CFG_TUD_CDC
    BaseType_t ret = xTaskCreatePinnedToCore(serial_cli_task, "serial_cli", 6144, NULL, 4, NULL, 0);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create CLI task");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "USB CDC Serial CLI / API engine initialized on Core 0");
#else
    ESP_LOGI(TAG, "Serial CLI over USB CDC disabled (Pure Audio mode)");
#endif
    return ESP_OK;
}
