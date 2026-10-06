/*
 * SPDX-FileCopyrightText: 2026 ESP32-S3 USB DAC Project
 * SPDX-License-Identifier: MIT
 */

#include "web_server.h"
#include <string.h>
#include <stdio.h>
#include "esp_http_server.h"
#include "esp_log.h"
#include "cJSON.h"
#include "web_ui_html.h"
#include "audio_pipeline.h"
#include "usb_audio.h"
#include "storage/preset_manager.h"
#include "storage/fs_manager.h"
#include "ota_ui_html.h"
#include "ota/ota_manager.h"
#include "preferences_ui_html.h"
#include "storage/hw_config.h"
#include "neopixel.h"

static const char *TAG = "WEB_SERVER";
static httpd_handle_t s_server = NULL;

// GET / - Root UI page
static esp_err_t root_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");
    return httpd_resp_send(req, s_web_ui_html, HTTPD_RESP_USE_STRLEN);
}

// Captive Portal Redirect Handler (302)
static esp_err_t captive_redirect_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    return httpd_resp_send(req, NULL, 0);
}

// GET /api/status
static esp_err_t api_status_handler(httpd_req_t *req)
{
    audio_pipeline_stats_t stats;
    audio_pipeline_get_stats(&stats);

    usb_audio_status_t usb_status;
    usb_audio_get_status(&usb_status);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "sample_rate", stats.sample_rate);
    cJSON_AddNumberToObject(root, "bit_depth", stats.bit_depth);
    cJSON_AddBoolToObject(root, "bit_perfect", stats.bit_perfect);
    cJSON_AddNumberToObject(root, "buffer_size", (int)stats.buffer_size_samples);
    cJSON_AddBoolToObject(root, "is_streaming", usb_status.is_streaming);
    cJSON_AddNumberToObject(root, "underruns", stats.underrun_count);
    cJSON_AddNumberToObject(root, "overruns", stats.overrun_count);

    char *json_str = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);

    free(json_str);
    cJSON_Delete(root);
    return ESP_OK;
}

// GET /api/vu
static esp_err_t api_vu_handler(httpd_req_t *req)
{
    dsp_engine_t *engine = audio_pipeline_get_dsp_engine();
    dsp_meter_values_t m;
    dsp_engine_get_meters(engine, &m);

    char buf[160];
    snprintf(buf, sizeof(buf),
             "{\"in_peak_l\":%.1f,\"in_peak_r\":%.1f,\"in_rms_l\":%.1f,\"in_rms_r\":%.1f,"
             "\"out_peak_l\":%.1f,\"out_peak_r\":%.1f,\"out_rms_l\":%.1f,\"out_rms_r\":%.1f}",
             m.in_peak_l, m.in_peak_r, m.in_rms_l, m.in_rms_r,
             m.out_peak_l, m.out_peak_r, m.out_rms_l, m.out_rms_r);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
}

// GET /api/wave
static esp_err_t api_wave_handler(httpd_req_t *req)
{
    dsp_engine_t *engine = audio_pipeline_get_dsp_engine();
    float wave[64];
    dsp_engine_get_waveform(engine, wave, 64);

    char buf[512];
    int pos = snprintf(buf, sizeof(buf), "{\"samples\":[");
    for (int i = 0; i < 64 && pos < (int)sizeof(buf) - 8; i++) {
        pos += snprintf(buf + pos, sizeof(buf) - pos, "%.2f%s", wave[i], (i == 63) ? "" : ",");
    }
    snprintf(buf + pos, sizeof(buf) - pos, "]}");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
}

// GET /api/config
static esp_err_t api_config_get_handler(httpd_req_t *req)
{
    dsp_engine_t *engine = audio_pipeline_get_dsp_engine();
    dsp_config_t cfg;
    dsp_engine_get_config(engine, &cfg);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "bit_perfect_bypass", cfg.bit_perfect_bypass);
    cJSON_AddNumberToObject(root, "master_volume_db", cfg.master_volume_db);
    cJSON_AddNumberToObject(root, "buffer_size", (int)audio_pipeline_get_buffer_size());

    // EQ
    cJSON *eq_obj = cJSON_CreateObject();
    cJSON_AddBoolToObject(eq_obj, "enabled", cfg.eq.enabled);
    cJSON *bands = cJSON_CreateArray();
    for (int i = 0; i < DSP_EQ_NUM_BANDS; i++) {
        cJSON *b = cJSON_CreateObject();
        cJSON_AddNumberToObject(b, "freq", cfg.eq.bands[i].freq);
        cJSON_AddNumberToObject(b, "gain_db", cfg.eq.bands[i].gain_db);
        cJSON_AddNumberToObject(b, "q", cfg.eq.bands[i].q);
        cJSON_AddBoolToObject(b, "enabled", cfg.eq.bands[i].enabled);
        cJSON_AddItemToArray(bands, b);
    }
    cJSON_AddItemToObject(eq_obj, "bands", bands);
    cJSON_AddItemToObject(root, "eq", eq_obj);

    // Compressor
    cJSON *comp_obj = cJSON_CreateObject();
    cJSON_AddBoolToObject(comp_obj, "enabled", cfg.comp.enabled);
    cJSON_AddNumberToObject(comp_obj, "threshold_db", cfg.comp.threshold_db);
    cJSON_AddNumberToObject(comp_obj, "ratio", cfg.comp.ratio);
    cJSON_AddNumberToObject(comp_obj, "attack_ms", cfg.comp.attack_ms);
    cJSON_AddNumberToObject(comp_obj, "release_ms", cfg.comp.release_ms);
    cJSON_AddNumberToObject(comp_obj, "knee_db", cfg.comp.knee_db);
    cJSON_AddNumberToObject(comp_obj, "makeup_db", cfg.comp.makeup_db);
    cJSON_AddItemToObject(root, "comp", comp_obj);

    // Limiter
    cJSON *lim_obj = cJSON_CreateObject();
    cJSON_AddBoolToObject(lim_obj, "enabled", cfg.limiter.enabled);
    cJSON_AddNumberToObject(lim_obj, "ceiling_db", cfg.limiter.ceiling_db);
    cJSON_AddNumberToObject(lim_obj, "release_ms", cfg.limiter.release_ms);
    cJSON_AddItemToObject(root, "limiter", lim_obj);

    // Cassette Tape Emulator
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

    // Bitcrusher / Downsampler
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

    // Deesser
    cJSON *deess_obj = cJSON_CreateObject();
    cJSON_AddBoolToObject(deess_obj, "enabled", cfg.deesser.enabled);
    cJSON_AddNumberToObject(deess_obj, "freq", cfg.deesser.freq);
    cJSON_AddNumberToObject(deess_obj, "threshold_db", cfg.deesser.threshold_db);
    cJSON_AddNumberToObject(deess_obj, "ratio", cfg.deesser.ratio);
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

    // FX (Delay & Reverb)
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

    char *json_str = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);

    free(json_str);
    cJSON_Delete(root);
    return ESP_OK;
}

// POST /api/config
static esp_err_t api_config_post_handler(httpd_req_t *req)
{
    char buf[2048];
    int total_len = req->content_len;
    if (total_len >= sizeof(buf)) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    int cur_len = 0;
    while (cur_len < total_len) {
        int received = httpd_req_recv(req, buf + cur_len, total_len - cur_len);
        if (received <= 0) {
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }
        cur_len += received;
    }
    buf[total_len] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    dsp_engine_t *engine = audio_pipeline_get_dsp_engine();
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
                cJSON *f = cJSON_GetObjectItem(b, "freq");
                if (f) cfg.eq.bands[i].freq = (float)f->valuedouble;
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
        cJSON *cl = cJSON_GetObjectItem(lim_obj, "ceiling_db");
        if (cl) cfg.limiter.ceiling_db = (float)cl->valuedouble;
        cJSON *rl = cJSON_GetObjectItem(lim_obj, "release_ms");
        if (rl) cfg.limiter.release_ms = (float)rl->valuedouble;
    }

    // Cassette Tape Emulator
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

    // Bitcrusher / Downsampler
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

    // Bass Enhancer
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

    // De-Esser
    cJSON *deess_obj = cJSON_GetObjectItem(root, "deesser");
    if (deess_obj) {
        cJSON *en = cJSON_GetObjectItem(deess_obj, "enabled");
        if (en) cfg.deesser.enabled = cJSON_IsTrue(en);
        cJSON *f = cJSON_GetObjectItem(deess_obj, "freq");
        if (f) cfg.deesser.freq = (float)f->valuedouble;
        cJSON *th = cJSON_GetObjectItem(deess_obj, "threshold_db");
        if (th) cfg.deesser.threshold_db = (float)th->valuedouble;
    }

    // Pitch Shifter
    cJSON *pitch_obj = cJSON_GetObjectItem(root, "pitch");
    if (pitch_obj) {
        cJSON *en = cJSON_GetObjectItem(pitch_obj, "enabled");
        if (en) cfg.pitch.enabled = cJSON_IsTrue(en);
        cJSON *st = cJSON_GetObjectItem(pitch_obj, "semitones");
        if (st) cfg.pitch.semitones = (int8_t)st->valueint;
        cJSON *ct = cJSON_GetObjectItem(pitch_obj, "cents");
        if (ct) cfg.pitch.cents = (int8_t)ct->valueint;
    }

    // Loudness
    cJSON *loud_obj = cJSON_GetObjectItem(root, "loudness");
    if (loud_obj) {
        cJSON *en = cJSON_GetObjectItem(loud_obj, "enabled");
        if (en) cfg.loudness.enabled = cJSON_IsTrue(en);
        cJSON *tg = cJSON_GetObjectItem(loud_obj, "target_db");
        if (tg) cfg.loudness.target_db = (float)tg->valuedouble;
    }

    // Delay & Reverb
    cJSON *fx_obj = cJSON_GetObjectItem(root, "fx");
    if (fx_obj) {
        cJSON *de = cJSON_GetObjectItem(fx_obj, "delay_enabled");
        if (de) cfg.fx.delay_enabled = cJSON_IsTrue(de);
        cJSON *dt = cJSON_GetObjectItem(fx_obj, "delay_time_ms");
        if (dt) cfg.fx.delay_time_ms = (float)dt->valuedouble;
        cJSON *dm = cJSON_GetObjectItem(fx_obj, "delay_mix");
        if (dm) cfg.fx.delay_mix = (float)dm->valuedouble;

        cJSON *re = cJSON_GetObjectItem(fx_obj, "reverb_enabled");
        if (re) cfg.fx.reverb_enabled = cJSON_IsTrue(re);
        cJSON *rs = cJSON_GetObjectItem(fx_obj, "reverb_room_size");
        if (rs) cfg.fx.reverb_room_size = (float)rs->valuedouble;
        cJSON *rm = cJSON_GetObjectItem(fx_obj, "reverb_mix");
        if (rm) cfg.fx.reverb_mix = (float)rm->valuedouble;
    }

    dsp_engine_set_config(engine, &cfg);

    cJSON_Delete(root);
    httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
    return ESP_OK;
}

// GET /api/presets
static esp_err_t api_presets_get_handler(httpd_req_t *req)
{
    cJSON *arr = cJSON_CreateArray();
    size_t count = preset_manager_get_count();
    for (size_t i = 0; i < count; i++) {
        const preset_entry_t *p = preset_manager_get_preset(i);
        if (p) {
            cJSON *item = cJSON_CreateObject();
            cJSON_AddStringToObject(item, "name", p->name);
            cJSON_AddBoolToObject(item, "factory", p->is_factory);
            cJSON_AddItemToArray(arr, item);
        }
    }

    char *json_str = cJSON_PrintUnformatted(arr);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);

    free(json_str);
    cJSON_Delete(arr);
    return ESP_OK;
}

// POST /api/presets/load
static esp_err_t api_presets_load_handler(httpd_req_t *req)
{
    char buf[128];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) return ESP_FAIL;
    buf[ret] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) return ESP_FAIL;

    cJSON *idx = cJSON_GetObjectItem(root, "index");
    if (idx) {
        dsp_config_t cfg;
        if (preset_manager_load((size_t)idx->valueint, &cfg) == ESP_OK) {
            dsp_engine_set_config(audio_pipeline_get_dsp_engine(), &cfg);
        }
    }

    cJSON_Delete(root);
    httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
    return ESP_OK;
}

// POST /api/presets/save
static esp_err_t api_presets_save_handler(httpd_req_t *req)
{
    char buf[256];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) return ESP_FAIL;
    buf[ret] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) return ESP_FAIL;

    cJSON *idx = cJSON_GetObjectItem(root, "index");
    cJSON *name = cJSON_GetObjectItem(root, "name");
    if (idx) {
        dsp_config_t cfg;
        dsp_engine_get_config(audio_pipeline_get_dsp_engine(), &cfg);
        preset_manager_save((size_t)idx->valueint, name ? name->valuestring : "Preset", &cfg);
    }

    cJSON_Delete(root);
    httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
    return ESP_OK;
}

// POST /api/presets/reset
static esp_err_t api_presets_reset_handler(httpd_req_t *req)
{
    preset_manager_reset_defaults();
    dsp_config_t cfg;
    preset_manager_load(0, &cfg);
    dsp_engine_set_config(audio_pipeline_get_dsp_engine(), &cfg);
    httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
    return ESP_OK;
}

// GET /api/led
static esp_err_t api_led_get_handler(httpd_req_t *req)
{
    neopixel_config_t cfg;
    neopixel_get_config(&cfg);

    char buf[64];
    snprintf(buf, sizeof(buf), "{\"mode\":%d,\"brightness\":%d}", (int)cfg.mode, (int)cfg.brightness);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
}

// POST /api/led
static esp_err_t api_led_post_handler(httpd_req_t *req)
{
    char buf[128];
    int total_len = req->content_len;
    if (total_len >= sizeof(buf)) total_len = sizeof(buf) - 1;
    int cur_len = 0;
    while (cur_len < total_len) {
        int received = httpd_req_recv(req, buf + cur_len, total_len - cur_len);
        if (received <= 0) break;
        cur_len += received;
    }
    buf[cur_len] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (root) {
        neopixel_config_t cfg;
        neopixel_get_config(&cfg);

        cJSON *mode_item = cJSON_GetObjectItem(root, "mode");
        if (mode_item && cJSON_IsNumber(mode_item)) {
            cfg.mode = (neopixel_mode_t)mode_item->valueint;
        }

        cJSON *br_item = cJSON_GetObjectItem(root, "brightness");
        if (br_item && cJSON_IsNumber(br_item)) {
            cfg.brightness = (uint8_t)br_item->valueint;
        }

        neopixel_set_config(&cfg);
        cJSON_Delete(root);
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
}

// GET /api/storage
static esp_err_t api_storage_get_handler(httpd_req_t *req)
{
    size_t total = 0, used = 0;
    bool mounted = fs_manager_is_mounted();
    if (mounted) {
        fs_manager_get_info(&total, &used);
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "mounted", mounted);
    cJSON_AddStringToObject(root, "fs", "LittleFS");
    cJSON_AddStringToObject(root, "base_path", FS_STORAGE_MOUNT_POINT);
    cJSON_AddNumberToObject(root, "total_bytes", total);
    cJSON_AddNumberToObject(root, "used_bytes", used);
    cJSON_AddNumberToObject(root, "free_bytes", total >= used ? (total - used) : 0);
    cJSON_AddNumberToObject(root, "percent_used", total > 0 ? ((double)used / (double)total * 100.0) : 0.0);

    char *json_str = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);

    free(json_str);
    cJSON_Delete(root);
    return ESP_OK;
}

// GET /ota.html - OTA Update Page
static esp_err_t ota_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");
    return httpd_resp_send(req, s_ota_ui_html, HTTPD_RESP_USE_STRLEN);
}

// GET /api/ota/status
static esp_err_t api_ota_status_handler(httpd_req_t *req)
{
    ota_status_info_t st;
    ota_manager_get_status(&st);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "running_partition", st.running_partition);
    cJSON_AddStringToObject(root, "target_partition", st.target_partition);
    cJSON_AddStringToObject(root, "version", st.version);
    cJSON_AddStringToObject(root, "project_name", st.project_name);
    cJSON_AddStringToObject(root, "compile_date", st.compile_date);
    cJSON_AddStringToObject(root, "compile_time", st.compile_time);
    cJSON_AddStringToObject(root, "idf_version", st.idf_version);
    cJSON_AddNumberToObject(root, "littlefs_free_bytes", st.littlefs_free_bytes);
    cJSON_AddNumberToObject(root, "littlefs_total_bytes", st.littlefs_total_bytes);

    char *json_str = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);

    free(json_str);
    cJSON_Delete(root);
    return ESP_OK;
}

// POST /api/ota/upload
static esp_err_t api_ota_upload_handler(httpd_req_t *req)
{
    int total_len = req->content_len;
    if (total_len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty payload");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Receiving OTA upload (size: %d bytes)...", total_len);

    esp_err_t err = ota_manager_start_cache();
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Cannot create cache file in LittleFS");
        return ESP_FAIL;
    }

    char buf[4096];
    int remaining = total_len;
    while (remaining > 0) {
        int to_read = remaining > (int)sizeof(buf) ? (int)sizeof(buf) : remaining;
        int received = httpd_req_recv(req, buf, to_read);
        if (received <= 0) {
            if (received == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            ESP_LOGE(TAG, "Socket read error during OTA upload");
            ota_manager_abort();
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Socket error during upload");
            return ESP_FAIL;
        }

        err = ota_manager_write_cache(buf, (size_t)received);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed writing OTA chunk to cache");
            ota_manager_abort();
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Write error to LittleFS cache");
            return ESP_FAIL;
        }

        remaining -= received;
    }

    ESP_LOGI(TAG, "OTA cache upload complete. Commencing validation and flash write...");

    char err_msg[128] = {0};
    err = ota_manager_finish_cache_and_flash(err_msg, sizeof(err_msg));

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    if (err != ESP_OK) {
        char err_json[256];
        snprintf(err_json, sizeof(err_json), "{\"status\":\"error\",\"message\":\"%s\"}", err_msg[0] ? err_msg : "Flash error");
        httpd_resp_sendstr(req, err_json);
        return ESP_OK;
    }

    httpd_resp_sendstr(req, "{\"status\":\"ok\",\"message\":\"OTA update successfully written. Rebooting...\"}");
    return ESP_OK;
}

// GET /preferences.html
static esp_err_t preferences_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");
    return httpd_resp_send(req, s_preferences_ui_html, HTTPD_RESP_USE_STRLEN);
}

// GET /api/preferences
static esp_err_t api_preferences_get_handler(httpd_req_t *req)
{
    const hw_config_t *cfg = hw_config_get();

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "i2s_bck_gpio", cfg->i2s_bck_gpio);
    cJSON_AddNumberToObject(root, "i2s_din_gpio", cfg->i2s_din_gpio);
    cJSON_AddNumberToObject(root, "i2s_ws_gpio", cfg->i2s_ws_gpio);
    cJSON_AddNumberToObject(root, "oled_scl_gpio", cfg->oled_scl_gpio);
    cJSON_AddNumberToObject(root, "oled_sda_gpio", cfg->oled_sda_gpio);
    cJSON_AddNumberToObject(root, "neopixel_gpio", cfg->neopixel_gpio);
    cJSON_AddNumberToObject(root, "boot_button_gpio", cfg->boot_button_gpio);
    cJSON_AddStringToObject(root, "wifi_ssid", cfg->wifi_ssid);
    cJSON_AddStringToObject(root, "wifi_pass", cfg->wifi_pass);
    cJSON_AddNumberToObject(root, "wifi_channel", cfg->wifi_channel);
    cJSON_AddBoolToObject(root, "oled_enabled", cfg->oled_enabled);
    cJSON_AddNumberToObject(root, "oled_fps", cfg->oled_fps);
    cJSON_AddNumberToObject(root, "web_scope_fps", cfg->web_scope_fps);
    cJSON_AddNumberToObject(root, "scope_gain", (double)cfg->scope_gain);
    cJSON_AddBoolToObject(root, "scope_pre_vol", cfg->scope_pre_vol);

    char *json_str = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);

    free(json_str);
    cJSON_Delete(root);
    return ESP_OK;
}

static void pref_reboot_task(void *pvParameter)
{
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
}

// POST /api/preferences
static esp_err_t api_preferences_post_handler(httpd_req_t *req)
{
    char buf[512];
    int total_len = req->content_len;
    if (total_len >= sizeof(buf)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Payload too large");
        return ESP_FAIL;
    }

    int cur_len = 0;
    while (cur_len < total_len) {
        int received = httpd_req_recv(req, buf + cur_len, total_len - cur_len);
        if (received <= 0) break;
        cur_len += received;
    }
    buf[cur_len] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    hw_config_t prev = *hw_config_get();
    hw_config_t cfg = prev;
    bool need_reboot = false;

    cJSON *item;
    if ((item = cJSON_GetObjectItem(root, "i2s_bck_gpio")) && cJSON_IsNumber(item)) {
        if (cfg.i2s_bck_gpio != (int8_t)item->valueint) { cfg.i2s_bck_gpio = (int8_t)item->valueint; need_reboot = true; }
    }
    if ((item = cJSON_GetObjectItem(root, "i2s_din_gpio")) && cJSON_IsNumber(item)) {
        if (cfg.i2s_din_gpio != (int8_t)item->valueint) { cfg.i2s_din_gpio = (int8_t)item->valueint; need_reboot = true; }
    }
    if ((item = cJSON_GetObjectItem(root, "i2s_ws_gpio")) && cJSON_IsNumber(item)) {
        if (cfg.i2s_ws_gpio != (int8_t)item->valueint) { cfg.i2s_ws_gpio = (int8_t)item->valueint; need_reboot = true; }
    }
    if ((item = cJSON_GetObjectItem(root, "oled_scl_gpio")) && cJSON_IsNumber(item)) {
        if (cfg.oled_scl_gpio != (int8_t)item->valueint) { cfg.oled_scl_gpio = (int8_t)item->valueint; need_reboot = true; }
    }
    if ((item = cJSON_GetObjectItem(root, "oled_sda_gpio")) && cJSON_IsNumber(item)) {
        if (cfg.oled_sda_gpio != (int8_t)item->valueint) { cfg.oled_sda_gpio = (int8_t)item->valueint; need_reboot = true; }
    }
    if ((item = cJSON_GetObjectItem(root, "neopixel_gpio")) && cJSON_IsNumber(item)) {
        if (cfg.neopixel_gpio != (int8_t)item->valueint) { cfg.neopixel_gpio = (int8_t)item->valueint; need_reboot = true; }
    }
    if ((item = cJSON_GetObjectItem(root, "boot_button_gpio")) && cJSON_IsNumber(item)) {
        if (cfg.boot_button_gpio != (int8_t)item->valueint) { cfg.boot_button_gpio = (int8_t)item->valueint; need_reboot = true; }
    }
    if ((item = cJSON_GetObjectItem(root, "wifi_ssid")) && cJSON_IsString(item)) {
        if (strcmp(cfg.wifi_ssid, item->valuestring) != 0) { strncpy(cfg.wifi_ssid, item->valuestring, sizeof(cfg.wifi_ssid) - 1); need_reboot = true; }
    }
    if ((item = cJSON_GetObjectItem(root, "wifi_pass")) && cJSON_IsString(item)) {
        if (strcmp(cfg.wifi_pass, item->valuestring) != 0) { strncpy(cfg.wifi_pass, item->valuestring, sizeof(cfg.wifi_pass) - 1); need_reboot = true; }
    }
    if ((item = cJSON_GetObjectItem(root, "wifi_channel")) && cJSON_IsNumber(item)) {
        if (cfg.wifi_channel != (uint8_t)item->valueint) { cfg.wifi_channel = (uint8_t)item->valueint; need_reboot = true; }
    }

    // Dynamic display & visualizer calibration parameters
    if ((item = cJSON_GetObjectItem(root, "oled_enabled"))) cfg.oled_enabled = cJSON_IsTrue(item);
    if ((item = cJSON_GetObjectItem(root, "oled_fps")) && cJSON_IsNumber(item)) cfg.oled_fps = (uint8_t)item->valueint;
    if ((item = cJSON_GetObjectItem(root, "web_scope_fps")) && cJSON_IsNumber(item)) cfg.web_scope_fps = (uint8_t)item->valueint;
    if ((item = cJSON_GetObjectItem(root, "scope_gain")) && cJSON_IsNumber(item)) cfg.scope_gain = (float)item->valuedouble;
    if ((item = cJSON_GetObjectItem(root, "scope_pre_vol"))) cfg.scope_pre_vol = cJSON_IsTrue(item);

    esp_err_t err = hw_config_set(&cfg);
    cJSON_Delete(root);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    if (err == ESP_OK) {
        if (need_reboot) {
            httpd_resp_sendstr(req, "{\"status\":\"ok\",\"reboot\":true,\"message\":\"Hardware pins changed. Rebooting...\"}");
            xTaskCreate(pref_reboot_task, "pref_reboot", 2048, NULL, 5, NULL);
        } else {
            httpd_resp_sendstr(req, "{\"status\":\"ok\",\"reboot\":false,\"message\":\"Display and visualizer calibration applied live!\"}");
        }
    } else {
        httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"Failed to save to NVS\"}");
    }
    return ESP_OK;
}

// POST /api/preferences/reset
static esp_err_t api_preferences_reset_handler(httpd_req_t *req)
{
    hw_config_reset_defaults();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
    return ESP_OK;
}

esp_err_t web_server_start(void)
{
    if (s_server) return ESP_OK;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 36;
    config.stack_size = 8192;
    config.lru_purge_enable = true;
    config.core_id = 0;              // Strictly Core 0 (keep Core 1 dedicated to audio DSP)
    config.task_priority = 3;        // Low priority (never interrupt tusb_task or audio)

    esp_err_t ret = httpd_start(&s_server, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server: %s", esp_err_to_name(ret));
        return ret;
    }

    // URI Handlers
    httpd_uri_t uri_root = { .uri = "/", .method = HTTP_GET, .handler = root_get_handler };
    httpd_register_uri_handler(s_server, &uri_root);

    httpd_uri_t uri_pref = { .uri = "/preferences.html", .method = HTTP_GET, .handler = preferences_get_handler };
    httpd_register_uri_handler(s_server, &uri_pref);

    httpd_uri_t uri_pref_get = { .uri = "/api/preferences", .method = HTTP_GET, .handler = api_preferences_get_handler };
    httpd_register_uri_handler(s_server, &uri_pref_get);

    httpd_uri_t uri_pref_post = { .uri = "/api/preferences", .method = HTTP_POST, .handler = api_preferences_post_handler };
    httpd_register_uri_handler(s_server, &uri_pref_post);

    httpd_uri_t uri_pref_reset = { .uri = "/api/preferences/reset", .method = HTTP_POST, .handler = api_preferences_reset_handler };
    httpd_register_uri_handler(s_server, &uri_pref_reset);

    httpd_uri_t uri_ota = { .uri = "/ota.html", .method = HTTP_GET, .handler = ota_get_handler };
    httpd_register_uri_handler(s_server, &uri_ota);

    httpd_uri_t uri_ota_status = { .uri = "/api/ota/status", .method = HTTP_GET, .handler = api_ota_status_handler };
    httpd_register_uri_handler(s_server, &uri_ota_status);

    httpd_uri_t uri_ota_upload = { .uri = "/api/ota/upload", .method = HTTP_POST, .handler = api_ota_upload_handler };
    httpd_register_uri_handler(s_server, &uri_ota_upload);

    httpd_uri_t uri_status = { .uri = "/api/status", .method = HTTP_GET, .handler = api_status_handler };
    httpd_register_uri_handler(s_server, &uri_status);

    httpd_uri_t uri_vu = { .uri = "/api/vu", .method = HTTP_GET, .handler = api_vu_handler };
    httpd_register_uri_handler(s_server, &uri_vu);

    httpd_uri_t uri_wave = { .uri = "/api/wave", .method = HTTP_GET, .handler = api_wave_handler };
    httpd_register_uri_handler(s_server, &uri_wave);

    httpd_uri_t uri_config_get = { .uri = "/api/config", .method = HTTP_GET, .handler = api_config_get_handler };
    httpd_register_uri_handler(s_server, &uri_config_get);

    httpd_uri_t uri_config = { .uri = "/api/config", .method = HTTP_POST, .handler = api_config_post_handler };
    httpd_register_uri_handler(s_server, &uri_config);

    httpd_uri_t uri_presets = { .uri = "/api/presets", .method = HTTP_GET, .handler = api_presets_get_handler };
    httpd_register_uri_handler(s_server, &uri_presets);

    httpd_uri_t uri_presets_load = { .uri = "/api/presets/load", .method = HTTP_POST, .handler = api_presets_load_handler };
    httpd_register_uri_handler(s_server, &uri_presets_load);

    httpd_uri_t uri_presets_save = { .uri = "/api/presets/save", .method = HTTP_POST, .handler = api_presets_save_handler };
    httpd_register_uri_handler(s_server, &uri_presets_save);

    httpd_uri_t uri_presets_reset = { .uri = "/api/presets/reset", .method = HTTP_POST, .handler = api_presets_reset_handler };
    httpd_register_uri_handler(s_server, &uri_presets_reset);

    httpd_uri_t uri_led_get = { .uri = "/api/led", .method = HTTP_GET, .handler = api_led_get_handler };
    httpd_register_uri_handler(s_server, &uri_led_get);

    httpd_uri_t uri_led_post = { .uri = "/api/led", .method = HTTP_POST, .handler = api_led_post_handler };
    httpd_register_uri_handler(s_server, &uri_led_post);

    httpd_uri_t uri_storage_get = { .uri = "/api/storage", .method = HTTP_GET, .handler = api_storage_get_handler };
    httpd_register_uri_handler(s_server, &uri_storage_get);

    // Captive Portal Redirects
    httpd_uri_t uri_c1 = { .uri = "/generate_204", .method = HTTP_GET, .handler = captive_redirect_handler };
    httpd_register_uri_handler(s_server, &uri_c1);

    httpd_uri_t uri_c2 = { .uri = "/hotspot-detect.html", .method = HTTP_GET, .handler = captive_redirect_handler };
    httpd_register_uri_handler(s_server, &uri_c2);

    httpd_uri_t uri_c3 = { .uri = "/ncsi.txt", .method = HTTP_GET, .handler = captive_redirect_handler };
    httpd_register_uri_handler(s_server, &uri_c3);

    httpd_uri_t uri_c4 = { .uri = "/connecttest.txt", .method = HTTP_GET, .handler = captive_redirect_handler };
    httpd_register_uri_handler(s_server, &uri_c4);

    ESP_LOGI(TAG, "HTTP Server started on port %d with Captive Portal handlers", config.server_port);
    return ESP_OK;
}

void web_server_stop(void)
{
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
        ESP_LOGI(TAG, "HTTP Server stopped");
    }
}
