# 🐾 Desk Pet (ESP32-S3-BOX-3)

An interactive, animated desktop pet and live workstation companion built for the **Espressif ESP32-S3-BOX-3**, connected via a single USB-C cable to an Ubuntu / Linux workstation.

Desk Pet combines a responsive, procedurally-drawn emotional face with real-time workstation hardware telemetry, dual-zone touchscreen shortcuts for PC lighting and night mode, and full hardware integration.

---

## ✨ Features

### 1. 🎨 Dynamic Emotional Face (LVGL 8)
- Procedurally rendered using LVGL vector primitives—zero external bitmap images or flash assets.
- Smooth breathing bob animation, blinking eyes, and glance micro-movements.
- Dynamic color moods reflecting live workstation state:
  - 🩵 **Happy (`#00E5FF` Cyan)**: Normal active state with a cheerful smile and relaxed glance.
  - 🧡 **Focus (`#FF9F0A` Warm Amber)**: Concentrated squint and focused eyebrows when CPU (>35%) or GPU (>30%) is under active workload.
  - 💚 **Relax / Chill (`#30D158` Mint Green)**: Sleepy half-lidded zen expression with slow breathing during system idle.
  - 🩷 **Loved (`#FF375F` Blossom Pink)**: Excited curved eyes `(^.^)`, rosy blush, and wide smile triggered on touch interaction or returning from the status screen.
  - ❤️ **Hot / Spicy (`#FF453A` Crimson)**: Furrowed brows and angry arc `(>_<)` when GPU temp exceeds 70°C or heavy load spikes occur.

### 2. 📱 Invisible Touchscreen Hotzones
The 2.4" capacitive touch display (GT911) is divided into clean, responsive touch control areas:

| Touch Area | Region (X, Y) | Function | Behavior |
| :--- | :--- | :--- | :--- |
| **Top-Left** | `(0, 0)` to `(160, 120)` | **PC RGB Toggle** | Toggles all PC lighting (case/cooler fans + DDR5 RAM) between **Speed-2 Rainbow** and **Completely Off**. |
| **Top-Right** | `(160, 0)` to `(320, 120)` | **Night Mode Toggle** | Dims workstation monitors to 0% brightness (`ddcutil`), activates GNOME Night Light, and turns off the BOX-3 screen. Tap again to restore monitors to 30%, turn off night light, and restore BOX-3 LCD to 50%. |
| **Bottom Row** | `(0, 120)` to `(320, 240)` | **System Status** | Switches between the animated Pet face and the live System Status dashboard. |

> **Physical Buttons**: The physical **MUTE button** (GPIO 1) and capacitive **home button** (MAIN) also remain active as direct hardware shortcuts to toggle the System Status screen.

### 3. 📊 Real-Time Workstation Telemetry
The System Status screen streams live hardware metrics at 1 Hz directly from Linux kernel interfaces:
- **CPU %**: Utilization delta sampled over `/proc/stat`.
- **RAM**: High-precision memory usage matching `htop`'s exact formula (`MemTotal - MemFree - Buffers - (Cached + SReclaimable - Shmem)`) displayed in GB (`%.2f / %.2f GB`).
- **GPU %**: Live NVIDIA GPU compute utilization via `nvidia-smi`.
- **VRAM**: True memory usage matching `nvtop` (`memory.total - memory.free`) displayed in GB (`%.2f / %.2f GB`).
- **GPU Temp**: Real-time thermal tracking with color-coded warning range.
- **Clock & Date Sync**: Continuous synchronization with workstation time in UTC+8.

### 4. 💡 Direct Hardware RGB Control
Desk Pet solves the Linux motherboard ARGB limitation through custom low-level driver integration:
- **ASUS Aura Motherboard ARGB Headers** (`0b05:1bed`):
  - Automatically wakes and powers on headers via the proprietary Aura `0x38` channel power register.
  - Streams fluid 25–30 FPS Direct Mode (`0x40`) frames across all addressable fan headers with virtually 0% CPU overhead using a precomputed 360-color LUT.
- **DDR5 RAM**:
  - Addressed via OpenRGB over SMBus I2C (`ENE DRAM`) with matching hardware rainbow speed.
  - Clean separation prevents USB HID bus collisions between OpenRGB and the fan streamer.

---

## 🗂️ Repository Structure

```text
esp_desktop_pet/
├── firmware/                       # ESP-IDF firmware for ESP32-S3-BOX-3
│   ├── CMakeLists.txt              # Top-level ESP-IDF build configuration
│   ├── sdkconfig / sdkconfig.defaults
│   ├── dependencies.lock           # Pinned component dependencies
│   ├── components/                 # Project components
│   │   ├── bsp/                    # Board support package integration
│   │   └── esp-box-3/              # BOX-3 BSP driver with GT911 legacy I2C fix
│   └── main/
│       ├── main.c                  # Boot orchestration, display init, RX task, touch handlers
│       ├── ui.c / ui.h             # LVGL 8 UI: procedural face animations & status dashboard
│       ├── telemetry.c / .h        # CSV protocol parser, metrics store, thread safety
│       ├── idf_component.yml       # Component requirements
│       └── CMakeLists.txt
├── host/                           # Ubuntu host-side service
│   ├── deskpet_host.py             # System telemetry collector & USB serial streamer
│   ├── deskpet.service             # systemd service unit for auto-start on boot
│   ├── 99-deskpet.rules            # udev rule granting user access to /dev/ttyACM0
│   └── install.sh                  # Host service installer script
├── .gitignore                      # Ignore rules for build artifacts and virtualenvs
└── README.md                       # Documentation
```

---

## 🚀 Setup & Installation

### 1. Hardware Connection
Connect the **ESP32-S3-BOX-3** directly to your PC via a USB-C data cable. This supplies power and establishes the USB-Serial/JTAG communication link at `/dev/ttyACM0`.

### 2. Host Service (Ubuntu / Linux)

Run the included installer to configure the `udev` rule and systemd service:

```bash
cd host
sudo bash install.sh
```

Or configure manually:

```bash
# 1. Install udev rule for non-root serial access
sudo cp host/99-deskpet.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules && sudo udevadm trigger

# 2. Create Python virtual environment & install dependencies
cd host
python3 -m venv .venv
.venv/bin/pip install pyserial

# 3. Install and start the systemd service
sudo cp deskpet.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now deskpet.service
```

Check service status and monitor live serial telemetry:
```bash
systemctl status deskpet
journalctl -u deskpet -f
```

### 3. Firmware Build & Flash (ESP-IDF v5.5)

If you modify the firmware, compile and flash it to the device using ESP-IDF:

```bash
# 1. Source ESP-IDF environment (v5.5)
source ~/esp/esp-idf/export.sh

# 2. Build project
cd firmware
idf.py build

# 3. Flash to ESP32-S3-BOX-3
idf.py -p /dev/ttyACM0 flash
```

> [!TIP]
> If `deskpet.service` is actively streaming over `/dev/ttyACM0`, stop the service before flashing:
> ```bash
> sudo systemctl stop deskpet
> idf.py -p /dev/ttyACM0 flash
> sudo systemctl start deskpet
> ```

---

## 📡 Serial Wire Protocol

The host service streams one CSV telemetry packet per second over `/dev/ttyACM0` at `115200` baud:

```text
PET,<epoch_utc>,<cpu_pct>,<ram_used_mb>,<ram_total_mb>,<gpu_pct>,<gpu_temp_c>,<vram_used_mb>,<vram_total_mb>\n
```

### Example Packet
```text
PET,1787344120,12.5,5428.20,63447.80,25.0,45.0,960.00,16311.00
```

### Bidirectional Control Commands
- **Firmware → Host**:
  - `CMD,RGB_TOGGLE\n`: Toggles all workstation RGB devices (Fans & RAM).
  - `CMD,NIGHT_TOGGLE\n`: Toggles workstation night mode and monitor brightness.
- **Host → Firmware**:
  - Continuous telemetry updates keep the pet's clock, emotion, and telemetry synced.
  - If no telemetry is received for >10 seconds, the display shows `HOST OFFLINE` while retaining the last known time.

---

## 🔧 Hardware Troubleshooting Notes

- **GT911 Touch Controller Clock**:
  In ESP-IDF v5.x, newer `esp_lcd_touch` macros default `.scl_speed_hz = 100000`. Because the BOX-3 BSP uses legacy I2C where the bus clock is configured at bus level, passing a non-zero device clock causes an `ESP_ERR_INVALID_ARG` abort. The bundled [`components/esp-box-3`](file:///home/humjie/fun/deskpet/firmware/components/esp-box-3) overrides this to zero, ensuring reliable touch detection.
- **USB-Serial/JTAG Reset Guard**:
  Opening serial connections with `RTS=True` (default in generic serial monitors) asserts the ESP32-S3 hardware reset line. The host script explicitly forces `ser.dtr = False; ser.rts = False` on connect to prevent reset loops.
- **ASUS AM5 ARGB Direct Mode**:
  ASUS TUF B850 motherboards require wake packet `0xEC, 0x38, ch, 0x01` before ARGB headers accept Direct Mode (`0x40`) color updates.

---

## 📜 License & Credits
- Built for **Espressif ESP32-S3-BOX-3**.
- UI powered by [LVGL 8](https://lvgl.io/).
- Concept inspired by [Tabbie](https://github.com/humjie/tabbie).
