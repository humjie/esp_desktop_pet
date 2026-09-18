# 🐾 Desk Pet (ESP32-S3-BOX-3)

An interactive, animated desktop companion and live PC system monitor built for the **ESP32-S3-BOX-3**, connected via a single USB-C cable to an Ubuntu workstation.

Inspired by [Tabbie](https://github.com/humjie/tabbie), Desk Pet expresses dynamic emotions with vibrant colors depending on what you and your PC are doing—and toggles with a simple tap on the screen to show live hardware metrics.

---

## ✨ Features

- **📱 Tap-Anywhere Touchscreen Toggle**:
  - Tap anywhere on the screen to switch between the **Pet Face** and the **System Status** dashboard.
  - Tap again to toggle back to the Pet face (greeting you with an affectionate pink blush!).
  - Physical **MUTE button** (GPIO 1) and the capacitive **touch home button** (MAIN) also toggle modes.
- **🎨 Dynamic Emotions & Vibrant Colors (Tabbie-Inspired)**:
  - 🩵 **Happy (Cyan `#00E5FF`)**: Playful friendly smile, gentle breathing, curious glances around.
  - 🧡 **Focus (Warm Amber `#FF9F0A`)**: Determined squint with concentrated eyebrows when CPU (>35%) or GPU (>30%) is under active workload.
  - 💚 **Chill / Relax (Mint Green `#30D158`)**: Sleepy half-lidded zen eyes with slow, peaceful breathing when your PC is idle.
  - 🩷 **Loved (Blossom Pink `#FF375F`)**: Cheerful curved eyes `(^.^)`, deep rosy blush, and wide smile triggered whenever you touch the pet or return to the face screen.
  - ❤️ **Hot / Spicy (Crimson `#FF453A`)**: Furrowed angry brows and frown arc `(>_<)` when GPU temperature hits ≥70°C or heavy load spikes.
- **📊 Real-Time Workstation Telemetry (1 Hz streaming)**:
  - **CPU %**: Utilization delta sampled over `/proc/stat`.
  - **RAM (GB)**: High-precision usage matching `htop`'s exact formula (`MemTotal - MemFree - Buffers - (Cached + SReclaimable - Shmem)`) displayed to **two decimal places** (`%.2f / %.2f GB`).
  - **GPU %**: Live NVIDIA GPU compute utilization.
  - **VRAM (GB)**: Memory usage matching `nvtop` (`memory.total - memory.free`) displayed to **two decimal places** (`%.2f / %.2f GB`).
  - **GPU Temp (°C)**: Thermal monitoring with color-coded warning range.
  - **Clock & Date Sync**: Synced continuously from workstation UTC time (converted to UTC+8 local time).
- **⚡ Automatic Boot & Reconnect**:
  - Powers on automatically via USB when the PC starts up.
  - Ubuntu host service starts automatically via `systemd` (`deskpet.service`).
  - Graceful auto-reconnect if unplugged and plugged back in.

---

## 🗂️ Project Structure

```text
deskpet/
├── firmware/                       # ESP-IDF firmware for ESP32-S3-BOX-3
│   ├── CMakeLists.txt              # Self-contained project build configuration
│   ├── sdkconfig / sdkconfig.defaults
│   ├── dependencies.lock           # Pinned component dependencies
│   ├── components/                 # Local project components
│   │   ├── bsp/                    # Board support package
│   │   └── esp-box-3/              # BOX-3 BSP with touch driver fixes
│   └── main/
│       ├── main.c                  # Boot orchestration, display init, RX task, buttons
│       ├── ui.c / ui.h             # LVGL 8 UI: dynamic emotions, colors, status screen
│       └── telemetry.c / .h        # Protocol parser, metrics store, mutex
├── host/                           # Ubuntu host-side monitoring service
│   ├── deskpet_host.py             # System metrics collector & USB serial streamer
│   ├── deskpet.service             # systemd unit for automatic startup on boot
│   ├── 99-deskpet.rules            # udev rule for world-writable /dev/ttyACM0
│   ├── install.sh                  # One-step host installation script
│   └── .venv/                      # Python virtual environment (pyserial)
├── HANDOFF.md                      # Architecture & hardware notes
├── .gitignore                      # Git ignore rules
└── README.md                       # Documentation
```

---

## 🚀 Getting Started

### 1. Hardware Requirements
- **ESP32-S3-BOX-3** development kit.
- USB-C cable connected directly to the PC (supplies power + USB-Serial/JTAG on `/dev/ttyACM0`).

### 2. Host Service Setup (Ubuntu)

Run the installer to configure the `udev` rule and enable the `systemd` service:

```bash
cd host
sudo ./install.sh
```

Or manually:
```bash
# 1. Install udev rule
sudo cp host/99-deskpet.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules && sudo udevadm trigger

# 2. Set up Python virtualenv
cd host
python3 -m venv .venv
.venv/bin/pip install pyserial

# 3. Enable and start systemd service
sudo cp deskpet.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now deskpet
```

Check service status and view device logs:
```bash
systemctl status deskpet
journalctl -u deskpet -f
```

### 3. Firmware Build & Flash (ESP-IDF v5.5)

Activate the ESP-IDF environment and flash the board:

```bash
# 1. Activate ESP-IDF
source /path/to/esp-idf/export.sh

# 2. Build firmware
cd firmware
idf.py build

# 3. Flash to ESP32-S3-BOX-3
idf.py -p /dev/ttyACM0 flash
```

> **Note**: If `deskpet.service` is actively streaming over `/dev/ttyACM0`, pause or stop the service before flashing so both processes do not conflict on the serial port:
> ```bash
> sudo systemctl stop deskpet
> idf.py -p /dev/ttyACM0 flash
> sudo systemctl start deskpet
> ```

---

## 📡 Serial Wire Protocol

Host sends 1 CSV line per second over `/dev/ttyACM0` at `115200` baud:

```text
PET,<epoch_utc>,<cpu_pct>,<ram_used_mb>,<ram_total_mb>,<gpu_pct>,<gpu_temp_c>,<vram_used_mb>,<vram_total_mb>\n
```

Example packet:
```text
PET,1787344120,12.5,5428.20,63447.80,25.0,45.0,960.00,16311.00
```

- Firmware filters strictly for lines prefixed with `PET,` (firmware log messages on the same port are safely ignored).
- Firmware also returns device logs back upstream to the host, which `deskpet_host.py` forwards to `systemd` journal under `[DEVICE]`.

---

## 💡 Touchscreen & Hardware Details

- **Touch Controller**: FocalTech / Goodix **GT911** over I2C (`0x5D` / `0x14`).
- **Legacy I2C Fix**: Modern `esp_lcd_touch` versions set `.scl_speed_hz = 100000` in their config macros for the newer `i2c_master` v2 API. The BOX-3 BSP uses legacy I2C where the bus clock is initialized at bus level; passing a non-zero device clock causes `ESP_ERR_INVALID_ARG`. The project's standalone `components/esp-box-3` resets `.scl_speed_hz = 0`, enabling GT911 touch recognition and native LVGL indev registration without breaking existing BSP drivers.
- **Serial DTR/RTS Gotcha**: On the ESP32-S3 USB-Serial/JTAG peripheral, raising RTS (default behavior of generic serial tools) triggers a hardware reset of the chip. The host driver explicitly forces `ser.dtr = False; ser.rts = False` upon connection to prevent reboot loops.

---

## 🤝 Attribution & References
- Inspired by [Tabbie](https://github.com/humjie/tabbie) (emotions, desk robot companion concept).
- Espressif [ESP-BOX](https://github.com/espressif/esp-box) BSP components.
- [LVGL](https://lvgl.io/) Graphics Library v8.
