# Desk Pet — BOX-3 Firmware Build Brief (for Antigravity / agy)

## Goal
Author and build ESP-IDF firmware for the **ESP32-S3-BOX-3** that turns it into a
"desk pet": on boot it shows a cute animated face + a big date/time clock, and it
toggles into an **AI Mode** screen when the user taps **anywhere** on the touch
LCD (tapping anywhere again returns to the idle face/clock).

## Files & environment you will need
- Project root: `/home/humjie/fun/deskpet/firmware/` (create your ESP-IDF app here)
- Reuse the Espressif board-support package (BSP) from the local esp-box repo:
  `/home/humjie/fun/esp-box/components/bsp`
  → `idf_component.yml` there depends on `espressif/esp-box-3` v1.1.* (from the
  Espressif component registry; `idf.py` fetches it automatically).
- ESP-IDF is being installed by the orchestrator at `~/esp/esp-idf` (branch
  `release/v5.5`). To use it: `source ~/esp/esp-idf/export.sh`. Confirm it exists
  before building; if not, wait — do NOT clone a second copy.
- Target chip: `esp32s3`. Target app: create with `idf.py create-project`.

## Hardware facts (verified)
- ESP32-S3-WROOM-1; 2.4" 320x240 SPI **touch** LCD; 3 physical buttons;
  speaker; 2 mics; gyro/accel; single USB-C = power + USB Serial/JTAG.
- The USB-C exposes the **USB Serial/JTAG controller** — on Linux it appears as
  `/dev/ttyACM0`. The host PC writes one line per second to it; the pet reads it.
- Use the board BSP from the esp-box `components/bsp` (via `EXTRA_COMPONENT_DIRS`
  or by depending on the local bsp component). Display: `bsp_display_start_with_config`
  + `bsp_display_backlight_on()` (see `/home/humjie/fun/esp-box/examples/factory_demo/main/main.c`).
  Buttons: `bsp_btn_register_callback(BSP_BUTTON_MAIN, BUTTON_SINGLE_CLICK, ...)` and
  `BSP_BUTTON_MUTE` / `BSP_BUTTON_CONFIG`. Touch: via the LVGL touch input port.

## Wire protocol (host -> pet), one CSV line / second on the USB Serial/JTAG console
```
PET,<state>,<epoch>,<cpu_pct>,<ram_used_mb>,<ram_total_mb>,<gpu_pct>,<vram_used_mb>,<vram_total_mb>\n
```
- `<state>`: `0`=RED (agent not started), `1`=GREEN (idle), `2`=YELLOW (busy).
- `<epoch>` = unix seconds (UTC) — the pet converts to local time for the clock
  (assume a fixed UTC+8 timezone offset, confirmed with the user; timezone is
  configurable via a #define).
- The pet must parse ONLY lines beginning with `PET,` and ignore everything else
  (firmware logs share the port and are not meant for the host).
- Baud: **115200**.

How to read the console on the ESP32: with `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG`,
stdin/stdout of the firmware ARE the USB Serial/JTAG. Prefer an explicit read task
using the ESP-IDF USB-Serial-JTAG driver (e.g. `esp_usb_serial_jtag_read_bytes`)
or a `fgets()` on stdin — implementer picks the robust option; `fgets`/`getchar`
on stdin is simplest and confirmed to work for this device. Route `ESP_LOGI`
logging normally (it goes to the same console; the host ignores non-PET lines).

## UI spec (LVGL, 320x240)
### Idle screen (default, on boot)
- Top: **time** (large, e.g. "14:03") + **date** ("FRIDAY 18 SEP 2026").
- Centre/below: a **cute animated face** — at minimum blinking eyes plus a soft
  mouth; add gentle idle animation (breathing scale, occasional eye blink using
  a periodic timer). The face should be charming and use the full 320px width
  comfortably. No external assets required — draw programmatically with LVGL
  (lv_point, lv_rect, lv_circle, ticks/arcs) or a simple pre-rendered sprite
  array. Simpler = more robust; a clean procedurally-drawn face is preferred.
- Tap anywhere on the screen → switch to AI Mode.

### AI Mode screen (tap anywhere toggles back)
Show clearly, updated live every ~1s from the serial line:
1. **Agent state chip** — a coloured dot + label:
   - RED: "AGENT OFFLINE"   (state 0)
   - GREEN: "AGENT IDLE"    (state 1)
   - YELLOW: "AGENT WORKING" (state 2)
2. **System load panel** (all % / MB from the PC):
   - CPU: `cpu_pct%`
   - RAM: `ram_used_mb / ram_total_mb MB`
   - GPU: `gpu_pct%`
   - VRAM: `vram_used_mb / vram_total_mb MB`
3. Small back hint: "TAP ANYWHERE TO EXIT".
Tap anywhere → back to idle screen.

Rows must fit 320x240 with a clean, readable layout (title bar + 4 big rows is
fine; use distinct colours, e.g. accent per metric).

### Defaults / robustness
- If no `PET,` line seen within ~10s, show "HOST OFFLINE" state (gray) on the AI
  screen and keep the clock as-is (clock last-known epoch).
- Keep memory low; run LVGL on core 1 (BSP default). PSRAM is available (16MB).

## Build & acceptance
1. `idf.py set-target esp32s3` / menuconfig: board = ESP32-S3-BOX-3, console =
   USB Serial/JTAG, and any LVGL config the BSP needs (BSP manages it).
2. Build cleanly: `idf.py build` → report the merged `.bin` size and path.
3. DO NOT flash yet — the orchestrator handles flashing + end-to-end serial
   verification after permissions are granted and it has confirmed the host
   service is sending PET frames. Just build successfully and report artifact
   paths so the orchestrator can flash/verify.

## Deliverables to return to the orchestrator
1. Full firmware source listing under `/home/humjie/fun/deskpet/firmware/`.
2. Build output: merged/application `.bin` + `.elf` absolute paths, firmware size.
3. Any deviant decisions you made (SDK quirks, LVGL version, timezone value).
4. A short note on how the face + AI screen are structured so the orchestrator can
   tweak visuals without surprise.