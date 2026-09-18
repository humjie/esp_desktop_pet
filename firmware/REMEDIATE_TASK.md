# REMEDIATION — Desk Pet firmware boots into an abort loop. Fix the BSP/touch config.

## Situation
You (AGY) built `/home/humjie/fun/deskpet/firmware` successfully (esp32s3, board = ESP32-S3-BOX-3,
console = USB Serial/JTAG). It flashed, but at runtime it crashes and reboots forever during display
+ touch init. You must fix, rebuild, reflash, and VERIFY by reading the serial boot log until no more
abort appears. You have flash access (`idf.py -p /dev/ttyACM0 flash`) and can read the console with a
pyserial reader at 115200 (e.g. `python3 -c` one-liner or a tiny script under /tmp).

## The crash (from the serial boot log)
```
I (757) deskpet_main: Desk Pet firmware starting...
I (763) LVGL: Starting LVGL task
W (763) ledc: GPIO 45 is not usable, maybe conflict with others
E (887) lcd_panel.io.i2c: esp_lcd_new_panel_io_i2c_v1(60): scl_speed_hz is not need to set in legacy i2c_lcd driver
E (888) ESP-BOX: bsp_touch_new(381):
ESP_ERROR_CHECK failed: esp_err_t 0x102 (ESP_ERR_INVALID_ARG) at ...
file: "./managed_components/espressif__esp-box/esp-box.c" line 387
func: bsp_display_indev_init
expression: bsp_touch_new(((void *)0), &tp)
abort() was called ...
```

KEY DIAGNOSTIC: the crash is inside **`managed_components/espressif__esp-box/esp-box.c`** — the GENERIC
ESP32-S3-BOX BSP — NOT `esp-box-3.c`. The app is linking/routing touch init through the WRONG BSP. The
esp-box-3 BSP is the correct one for this board and it probes TT21100/GT911 touch over I2C and uses
`bsp_i2c_device_probe(...)`.

## Likely root cause (confirm then fix)
The `main` component hard-REQUIRES the registry `espressif/esp-box-3` (in both
`main/idf_component.yml` and `main/CMakeLists.txt` REQUIRES), while ALSO using the LOCAL esp-box repo
BSP via `EXTRA_COMPONENT_DIRS = /home/humjie/fun/esp-box/components`. This pulls BOTH `espressif/esp-box-3`
AND the generic `espressif/esp-box` (3.0.*) BSP, causing a symbol conflict so touch init runs in the
wrong component.

KNOWN-GOOD REFERENCE: `/home/humjie/fun/esp-box/examples/factory_demo` builds and RUNS CORRECTLY on this
exact board with the SAME local components. Its top-level `CMakeLists.txt` sets
`set(EXTRA_COMPONENT_DIRS ../../components)` and its `main/idf_component.yml` does NOT hard-require the
registry esp-box-3 from the component; the local `components/bsp` component pulls the right board
(`PRIV_REQUIRES esp-box-3` via its `Kconfig.projbuild`/`CMakeLists.txt` board selection). Study how
factory_demo wires BSP + LVGL + touch and replicate for the desk pet.

## Acceptable fixes (pick the cleanest; preserve the UI/protocol code in main/*.c|h, telemetry, ui)
1. Fix the component/requirement graph so ONLY the local board BSP (esp-box-3 path) is used and the
   generic esp-box BSP is not pulled into main. Adjust `main/idf_component.yml` and `main/CMakeLists.txt`
   REQUIRES accordingly; add `lvgl` to REQUIRES if removing the registry dep drops the lvgl include path
   (the compiler will tell you exact component names if an include goes missing).
2. Confirm `sdkconfig` still has `CONFIG_BSP_BOARD_ESP32_S3_BOX_3=y` and console USB Serial/JTAG.
3. Keep the desk-pet app behavior: idle screen = big clock/date + procedurally-animated face (ui.c);
   tap-anywhere toggles AI screen; AI screen shows agent state chip (RED/GREEN/YELLOW) + CPU/RAM/GPU/VRAM
   parsed from `PET,<state>,<epoch>,<cpu>,<ram_used_mb>,<ram_total_mb>,<gpu>,<vram_used_mb>,<vram_total_mb>`
   lines on the USB Serial/JTAG console; HOST OFFLINE (gray) if no PET line in ~10s. Do not lose the
   serial_rx telemetry wiring in main.c. If the touch init is genuinely broken with the chosen BSP and
   blocks boot, make touch init non-fatal (ESP_LOGE + continue) so the device still boots and shows the
   UI with mouse/touch via LVGL if it can, and clearly note what you did.

## Verify (REQUIRED — do not return until this passes)
- `idf.py build` succeeds.
- `idf.py -p /dev/ttyACM0 flash` succeeds.
- Read the serial console for ~8s and CONFIRM the app boots PAST display/touch init, running the desk
  pet loop with NO abort/reboot (paste the full boot log tail in your report). If it still aborts,
  iterate again.
- Report: exact file/diff you changed to fix the BSP wiring, the boot log tail showing no abort, and any
  runtime quirks (LVGL version, touch working or bypassed).