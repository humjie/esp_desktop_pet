You are engineering ESP-IDF firmware for an Espressif ESP32-S3-BOX-3. Author the app from scratch inside `/home/humjie/fun/deskpet/firmware/` (currently contains only BRIEF.md — read it too) and BUILD it. Your final deliverable is a successful `idf.py build` producing a merged .bin + .elf, with the artifact paths reported. DO NOT flash — the orchestrator flashes and verifies separately.

ENVIRONMENT (already set up):
- ESP-IDF v5.5 installed at `/home/humjie/esp/esp-idf`. Use it by:
    source /home/humjie/esp/esp-idf/export.sh
  IDF_PATH may already be exported. Do NOT clone a second ESP-IDF.
- Local Espressif esp-box repo at `/home/humjie/fun/esp-box` (git checkout, master). Its board-support package is at `/home/humjie/fun/esp-box/components/bsp` — reuse it (it depends on `espressif/esp-box-3` v1.1.*, auto-fetched by idf.py from the Espressif component registry).
- Active shell python3 may be a Hermes venv; if `idf.py` misbehaves, use the ESP-IDF export (it sets the right python env). Prefer `export PATH=/usr/bin:/bin:$PATH` and unset VIRTUAL_ENV before building if needed.

TARGET: esp32s3, board = ESP32-S3-BOX-3, console = USB Serial/JTAG.

GOAL (desk pet):
1. On boot show a 320x240 LVGL UI: big date/time clock at top + a cute procedurally-drawn animated face (blinking eyes, soft mouth, gentle idle animation using a timer) using the full width. No external image assets — draw with LVGL primitives.
2. Touching ANYWHERE on the touch LCD toggles to an "AI Mode" screen; touching anywhere again toggles back.
3. AI Mode screen shows live (~1s) data parsed from the USB Serial/JTAG console:
   - Agent state chip: RED "AGENT OFFLINE" (0), GREEN "AGENT IDLE" (1), YELLOW "AGENT WORKING" (2).
   - System load: CPU %, RAM used/total MB, GPU %, VRAM used/total MB.
   - Small hint "TAP ANYWHERE TO EXIT".
   - If no data line seen in ~10s, show gray "HOST OFFLINE" and keep last clock.

WIRE PROTOCOL (host -> device over the USB Serial/JTAG console, one CSV line/s):
  PET,<state>,<epoch_sec_utc>,<cpu_pct>,<ram_used_mb>,<ram_total_mb>,<gpu_pct>,<vram_used_mb>,<vram_total_mb>\n
Parse ONLY lines beginning with "PET,"; ignore everything else (firmware logs share the port). Baud source default 115200 (it's the USB Serial/JTAG). Clock: epoch is UTC; display local time assuming UTC+8 (put timezone offset in a #define). Route ESP_LOGI normally (host ignores non-PET lines).

KEY HARDWARE / BSP FACTS:
- Display: bsp_display_start_with_config(...) + bsp_display_backlight_on(); see /home/humjie/fun/esp-box/examples/factory_demo/main/main.c for the pattern.
- Touch: via the LVGL touch input port (tap anywhere).
- Buttons exist (BSP_BUTTON_MAIN/CONFIG/MUTE) but the spec uses touch-anywhere, not buttons.
- Keep memory low; 16MB PSRAM available.

STRUCTURE: create the ESP-IDF app files under /home/humjie/fun/deskpet/firmware/ (main/ with main.c, CMakeLists.txt, sdkconfig.defaults, idf_component.yml, etc.). Wire the local esp-box bsp via EXTRA_COMPONENT_DIRS or a dependency so the BSP is used.

ACCEPTANCE:
- `idf.py build` completes with no errors.
- Report: merged/application .bin absolute path, .elf absolute path, firmware size, and a short note on how the face + AI screen are structured (so visuals can be tweaked later).
- Note any SDK quirks, LVGL version, or deviant decisions.
- Do NOT flash. Do NOT modify files outside /home/humjie/fun/deskpet/firmware/ and /home/humjie/esp/esp-idf (build artifacts only).
- If the component registry is unreachable (no network for idf.py to fetch espressif/esp-box-3), fall back to pointing EXTRA_COMPONENT_DIRS directly at /home/humjie/fun/esp-box/components/bsp and any needed local components, and say so.