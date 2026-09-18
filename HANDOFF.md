# Desk Pet — ESP32-S3-BOX-3 Project Handoff

> Handover document so a fresh agent can continue development. Everything below was
> verified working on this machine (Ubuntu 22.04, user `humjie`) as of the last run.

## 1. One-line status
The ESP32-S3-BOX-3 is a **working "desk pet"**: on boot it shows an animated face + clock
with dynamic emotions (Happy, Focus, Relax, Love, Hot) and vibrant colors (inspired by Tabbie),
and toggles via **touchscreen (tap anywhere)** or buttons (MUTE GPIO 1, capacitive MAIN home button)
to a **SYSTEM STATUS** screen showing live **CPU, RAM, GPU, VRAM, GPU temp** streamed over USB
from the PC. Host service auto-starts via systemd. Touchscreen is **fully enabled & verified**.

## 2. Project location & key files
Project root: **`/home/humjie/fun/deskpet/`** (NOT a git repo).

```
deskpet/
├── host/                      # Ubuntu host-side service
│   ├── deskpet_host.py        # main service (system metrics -> USB serial)
│   ├── deskpet.service        # systemd unit (auto-start on boot)
│   ├── 99-deskpet.rules       # udev rule (world-writable /dev/ttyACM0)
│   ├── install.sh             # one-shot installer (run as root)
│   └── .venv/                 # python3.11 venv with pyserial
└── firmware/                  # ESP-IDF app (ESP32-S3-BOX-3)
    ├── CMakeLists.txt
    ├── sdkconfig / sdkconfig.defaults
    ├── dependencies.lock
    ├── main/
    │   ├── main.c             # app_main: boot, display, serial RX task, button
    │   ├── ui.c / ui.h        # LVGL: idle face+clock, AI/status screen, animations
    │   ├── telemetry.c / .h   # parse PET, protocol + timezone (UTC+8)
    │   ├── CMakeLists.txt
    │   └── idf_component.yml  # idf>=5.1 only
    ├── managed_components/    # auto-fetched (esp-box-3, lvgl, etc.) - see §10
    ├── BRIEF.md               # original build brief
    ├── AGENT_TASK.md          # AGY task (authoring)
    ├── REMEDIATE_TASK.md      # AGY task (boot-crash fix) - historical
    └── build/                 # ESP-IDF build artifacts
```

Reference repos:
- Espressif esp-box SDK: `/home/humjie/fun/esp-box` (used only as reference / EXTRA_COMPONENT_DIRS earlier; the app currently relies on the **registry** `espressif/esp-box-3` component instead).
- ESP-IDF v5.5: `/home/humjie/esp/esp-idf` (`source /home/humjie/esp/esp-idf/export.sh` to use `idf.py`).

## 3. Hardware / connections
- **ESP32-S3-BOX-3** main unit, single USB-C = power + USB Serial/JTAG (`/dev/ttyACM0`).
- 2.4" 320x240 **ILI9341** SPI LCD + backlight. GT911/TT21100 **touch over I2C** (NOT currently working).
- 3 physical buttons (GPIO): **MUTE=GPIO1**, **CONFIG=GPIO0**, **MAIN=touch-based home** (needs touch).
- 16MB PSRAM, 16MB flash, ESP32-S3, Wi-Fi/BT (unused).

## 4. System setup currently in place (host)
- **udev rule** `/etc/udev/rules.d/99-deskpet.rules`:
  `ACTION=="add", SUBSYSTEM=="tty", ATTRS{idVendor}=="303a", MODE="0666", GROUP="dialout"`
- User `humjie` is in `dialout` group (needed until reboot pick-up; udev rule makes it writable regardless).
- **systemd service** `deskpet.service`, enabled + active, runs `host/.venv/bin/python host/deskpet_host.py`.
- Logs: `~/.local/share/deskpet/deskpet.log`; check via `journalctl -u deskpet -f`.

**Correct way to run/reload (IMPORTANT):**
```bash
sudo systemctl restart deskpet        # reload host service
sudo systemctl stop deskpet           # before flashing? NOT required, but do not leave 2 writers
systemctl status deskpet
```
When re-flashing firmware, it's fine to keep the service running, but **do not** have the
service AND a manual serial reader/pyserial session open at the same time on `/dev/ttyACM0`
(USB-Serial/JTAG supports one connection; a second opener resets the chip).

## 5. Wire protocol (host -> device), 1 line / sec over USB Serial/JTAG @115200
```
PET,<epoch_utc>,<cpu_pct>,<ram_used_mb>,<ram_total_mb>,<gpu_pct>,<gpu_temp_c>,<vram_used_mb>,<vram_total_mb>\n
```
Example (from live log): `PET,1787344..,3.5,7742,63447,10.0,43.0,960,16311`
- Epoch is UTC seconds; firmware converts to **UTC+8** (`TIMEZONE_OFFSET_HOURS 8` in `telemetry.h`).
- Firmware parses **only** lines starting with `PET,` and ignores everything else (logs share the port).
- **Units:** RAM/VRAM are sent in **MB (decimals allowed for vram)**; UI divides by 1024 to show **GB**.
- **GPU temp** is °C.

## 6. Host metric definitions (match htop / nvtop — verified)
- **CPU %**: `/proc/stat` delta over 0.25s → total utilization %.
- **RAM used (matches htop)**: `MemTotal − MemFree − Buffers − Cached − SReclaimable` (MB).
  This reproduces htop's "used". (Raw: total 63447 MB, used ~7.7 GB at idle.)
- **RAM total**: `MemTotal` MB.
- **GPU %**: `nvidia-smi utilization.gpu`.
- **GPU temp °C**: `nvidia-smi temperature.gpu`.
- **VRAM used (matches nvtop)**: `memory.total − memory.free` (nvidia-smi), i.e. total-free,
  NOT the smaller `memory.used`. (Raw: used ~0.94 GB, total 16311 MiB.)
- **VRAM total**: `memory.total`.

`nvidia-smi` query used: `--query-gpu=utilization.gpu,temperature.gpu,memory.total,memory.free`.
GPU = RTX 5060 Ti (16 GB).

## 7. Firmware UI (LVGL, 320x240)
- **Idle screen (default at boot):** big clock (`MM:SS`), date (`FRIDAY 18 SEP 2026`), and a
  procedurally-drawn animated face (blinking eyes, breathing bob, looking-around). Colors/face
  drawn in `ui.c` with LVGL primitives — no image assets.
- **AI/Status screen (toggle):** title "SYSTEM STATUS", 5 metric rows:
  ```
  CPU    <pct> %
  RAM    <used>/<total> GB        (MB→GB /1024, 1 decimal)
  GPU    <pct> %
  VRAM   <used>/<total> GB        (MB→GB /1024, 1 decimal)
  TEMP   <temp> C                 (bar range 0..120°C)
  ```
  Footer hint "TAP ANYWHERE TO EXIT" (touch currently disabled, so the hint is aspirational).
- Toggle mechanism: **physical MUTE button (GPIO 1)** single-click swaps screens via
  `ui_toggle_ai_mode()`. BSP `bsp_btn_init()` is called before registering the callback.

## 8. Build / flash firmware
```bash
cd /home/humjie/fun/deskpet/firmware
source /home/humjie/esp/esp-idf/export.sh
idf.py build            # target esp32s3 (already set in sdkconfig)
idf.py -p /dev/ttyACM0 flash
```
- Target chip `esp32s3`, board = BOX-3; console = USB Serial/JTAG (set in `sdkconfig.defaults`).
- Kernel-free build: `deskpet.bin` ~625 KB; app partition 1 MB (40% headroom).
- To read the boot log: use `idf.py -p /dev/ttyACM0 monitor`, or pyserial with **DTR/RTS kept low**
  (see §11 — toggling RTS hard-resets the chip).

## 9. Current behavior verified
- Pet boots once and stays running — **no reboot loop** (previously fixed).
- Host streams continuously; **0 write failures** in windowed tests.
- RAM/VRAM values match htop/nvtop.
- Service and pet auto-start after a normal PC reboot.

## 10. Touch Screen Support — RESOLVED
- **Root cause (diagnosed & resolved):** The pulled `esp_lcd_touch_gt911` (and tt21100)
  macro sets `.scl_speed_hz = 100000` for the newer `i2c_master` v2 API. The BOX-3 BSP uses
  the legacy `driver/i2c.h` driver where bus clock speed is set once during `bsp_i2c_init()`.
  The legacy driver `esp_lcd_new_panel_io_i2c_v1()` checks `io_config->scl_speed_hz == 0`,
  and rejects any non-zero value with `ESP_ERR_INVALID_ARG`.
- **Fix applied:**
  1. Isolated BSP components into local project directory `firmware/components/esp-box-3`
     and `firmware/components/bsp` so changes are permanent and git-tracked (independent of
     `managed_components/` downloads).
  2. Set `tp_io_config.scl_speed_hz = 0` before calling `esp_lcd_new_panel_io_i2c()`.
  3. Stored returned input device in `disp_indev = lvgl_port_add_touch(&touch_cfg)`.
  4. Registered both `BSP_BUTTON_MAIN` (capacitive touch home) and `BSP_BUTTON_MUTE` (physical GPIO 1).
  5. Tested and verified on physical hardware: GT911 detected, LVGL indev registered, tap-anywhere
     toggling verified working.

## 11. Pitfalls / gotchas (learned)
- **Never open `/dev/ttyACM0` with DTR/RTS raised** — on ESP32-S3 USB-Serial/JTAG, raising RTS
  hard-resets the MCU (a reboot loop that looked like a blinking screen). Host's `open_serial()`
  suppresses DTR/RTS and you must too (pyserial: set `ser.dtr=False; ser.rts=False` after open).
- **Only one process should hold the USB-CDC port.** A second opener glitches/re-enumerates the device.
- ESP-IDF `install.sh` fails with "called from a virtual environment" if Hermes' venv is active; it must
  use the system/uv python (project uses `~/.local/share/uv/python/.../python3.11`). Current IDF env is
  fine and already installed.
- `idf.py` requires `source /home/humjie/esp/esp-idf/export.sh` (sets `PYTHON`/`IDF_PATH`).
- The `BSP_ERROR_CHECK defined in multiple locations` INFO at build is harmless.

## 12. Useful commands
```bash
# Host service
sudo systemctl status|restart|stop deskpet
journalctl -u deskpet -f
# GPU / memory ground truth
nvidia-smi --query-gpu=utilization.gpu,temperature.gpu,memory.total,memory.free --format=csv
# ESP-IDF
cd /home/humjie/fun/deskpet/firmware && source /home/humjie/esp/esp-idf/export.sh && idf.py build
```