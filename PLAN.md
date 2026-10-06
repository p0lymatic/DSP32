# Plan: DSP32-S3 Firmware v0.2.2 Improvements

## 1. Fix PCM5102A I2S DAC Pops / Clicks & Low-level Artifacts
- **Root Cause**:
  In `i2s_tx_task` (`main/audio_pipeline.c`), when the ringbuffer runs dry (`bytes_collected == 0`), the loop enters `if (bytes_collected == 0) { s_is_buffering = true; vTaskDelay(2); continue; }`.
  Because no I2S DMA writes happen during this time, the I2S DMA TX buffer underflows, the clock or stream output starves/stops, and PCM5102A's internal auto-mute / PLL transitions fire, causing audible clicks and pops whenever playback pauses, stops, or underruns at boundary levels.
- **Solution**:
  - Never stop writing to I2S.
  - When `bytes_collected == 0` (or `bytes_collected < bytes_needed`), feed a clean zero-filled silence buffer (`memset(s_i2s_out_buf, 0, bytes_needed)`) directly to `i2s_dac_write(...)`.
  - Maintain continuous continuous bit clock and word select (BCK/WS) with silence frames so the PCM5102A never loses PLL synchronization and never transitions abruptly.
  - In `dsp_worker_task`, ensure smooth trailing samples when USB RX buffer starves.

## 2. Option to Disable Built-in SSD1306 OLED Display
- **Hardware Config & Storage**:
  - Add `bool oled_enabled;` to `hw_config_t` (default: `true`).
  - Update `hw_config_init`, defaults, validation, and JSON serialization/deserialization.
- **OLED Task**:
  - In `oled_display_init` and `oled_task`, check `hw_config_get()->oled_enabled`.
  - If disabled, shut down or sleep display (`u8g2_SetPowerSave(&s_u8g2, 1)`), suspend or gracefully bypass `oled_task`, freeing I2C bus and Core 0 CPU cycles.
- **Web UI & REST API**:
  - Expose `oled_enabled` in `/api/preferences` (GET & POST).
  - Add UI toggle in `preferences.html` ("ENABLE SSD1306 OLED DISPLAY").
  - Update Serial CLI command (`hw oled enable/disable`).

## 3. Remove Vector Oscilloscope from Web UI
- **Rationale**:
  The Web UI currently polls `/api/wave` at 30-60 FPS via `pollWave()` / `setInterval`, creating frequent JSON allocations, HTTP request overhead on Core 0, and memory thrashing that starves CPU and causes OLED frames to drop and tear.
- **Changes**:
  - In `main/web/web_ui_html.h`, remove the `<div class="sec-title">// LIVE VECTOR OSCILLOSCOPE //</div>`, canvas, and `pollWave()` / `renderScope()` loops.
  - Remove unnecessary `/api/wave` polling.

## 4. Bump Project Version to 0.2.2
- In `CMakeLists.txt`:
  - `set(PROJECT_VER "0.2.2")`
  - `project(esp32s3_usb_dac VERSION 0.2.2)`
- Verify all git changes, commit as Aleph, and push to GitHub.
