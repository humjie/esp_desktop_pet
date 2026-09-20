# 🐾 Desk Pet (ESP32-S3-BOX-3)

An interactive, animated desktop pet, voice assistant, and live workstation telemetry companion built for the **Espressif ESP32-S3-BOX-3**, connected via a single USB-C cable to an Ubuntu / Linux workstation.

Desk Pet combines a responsive, procedurally-drawn emotional face with real-time workstation hardware telemetry, dual-zone touchscreen shortcuts for PC lighting and night mode, dual microphone voice control with local AI transcription (`faster-whisper`), and full hardware integration.

---

## 🎙️ Voice Control ("meow")

Desk Pet features hands-free voice control powered by the onboard **ES7210 dual microphone ADC** with real-time on-chip Voice Activity Detection (VAD) and local, low-latency AI speech-to-text on your workstation.

### 2-Stage Voice Control & Audio Feedback ("meow" Wake Word)

1. **Stage 1 (Wake & Listen)**:
   - Say **"meow"** (or *"mew"*, *"miao"*).
   - The device immediately plays a **soft, gentle bell ring (A5 -> E6)** to acknowledge wake-up.
   - The screen subtitle changes to **`• LISTENING...`** for a 6-second window.
2. **Stage 2 (Execute Command)**:
   - Speak your desired instruction (e.g. *"robot"*, *"cat"*, *"lights"*, *"night"*, *"louder"*, *"pause"*, etc.).
   - The device plays an **affirmative confirmation double-ring (C6 -> G6)** to let you know the command was recognized and executed, displays the response text, and returns to normal status.
3. **Single-Breath Command**:
   - You can also say *"meow robot"* or *"meow lights"* all at once. It will directly execute and ring the confirmation chime!
4. **Timeout**:
   - If 6 seconds elapse without a follow-up command, the listening window silently expires and returns to the normal clock/date subtitle.
5. **Local Neural Speech (Piper TTS)**: Responses are spoken aloud in real time using the **Piper Neural TTS** engine (`en_US-lessac`) on your workstation CPU.
6. **100% Local & Private**: Audio is transcribed locally using `faster-whisper` (`tiny.en` int8) and synthesized locally via `piper-tts`. Zero data is sent to the cloud.

### 📋 Supported Voice Commands

During the **`• LISTENING...`** window (or prefixed with *"meow"*):

| Category | Voice Command Phrases | Action Executed |
| :--- | :--- | :--- |
| **Pet Appearance** | `robot`<br>`cyber`<br>`original` | Switches pet appearance to the futuristic **Cyber Pet** and speaks confirmation aloud via Piper TTS. |
| | `cat`<br>`kitty` | Switches pet appearance to the adorable **Cat** with ears, whiskers, and cute mouth. |
| | `switch pet`<br>`change`<br>`transform` | Cycles to the next pet appearance. |
| **Music Streaming & Control** | `play music`<br>`play`<br>`start music` | Starts streaming ambient music radio, confirms via Piper TTS, and resumes local media. |
| | `play lofi`<br>`lofi`<br>`lo-fi music` | Streams 24/7 Lo-Fi & Chillhop radio directly to your workstation speakers. |
| | `play chill`<br>`chill music`<br>`ambient` | Streams chill groove electronic music. |
| | `stop music`<br>`pause music`<br>`silence` | Stops the internet radio stream and pauses media players. |
| | `next`<br>`next song`<br>`skip` | Skips to the next track via `playerctl`. |
| **Workstation RGB Lighting** | `turn on lights`<br>`turn off lights`<br>`lights`<br>`rgb`<br>`rainbow` | Toggles workstation motherboard ARGB fan headers and DDR5 RAM between rainbow mode and off. |
| **Night / Sleep Mode** | `good night`<br>`sleep`<br>`night mode`<br>`lights out` | Dims workstation monitors to 0% brightness (`ddcutil`), activates GNOME Night Light, and turns off the BOX-3 screen. |
| | `wake up`<br>`day mode`<br>`morning` | Restores workstation monitors to 30%, turns off Night Light, and brings BOX-3 LCD backlight to 50%. |
| **Workstation Volume** | `volume up`<br>`louder`<br>`increase volume` | Increases workstation system audio output by **+10%** (`pactl`). |
| | `volume down`<br>`quieter`<br>`lower volume` | Decreases workstation system audio output by **-10%** (`pactl`). |
| | `mute`<br>`unmute`<br>`silence` | Toggles workstation system audio mute (`pactl`). |
| **System Diagnostics** | `status`<br>`system status`<br>`cpu`<br>`hardware` | Flips the BOX-3 screen to the live System Status dashboard and announces status. |
| **Greetings & Pet Reactions** | `hello`<br>`hi`<br>`hey`<br>`good morning`<br>`petpet` | Plays a cheerful chime, displays a loved blush expression `(^.^)`, and speaks *"Meow! Hello there, human!"* via Piper TTS. |

---

## 🎮 Hardware Buttons & Touch Controls

### 🔘 Physical Hardware Buttons
- **Side Button (MUTE / CONFIG - GPIO 1 / GPIO 0)**:
  - Cycles pet character appearance (**Original Cyber Pet <-> Cat**).
  - Selected appearance is instantly saved to ESP32 NVS flash and retained across power cycles.
- **Front Home Button (MAIN capacitive touch button)**:
  - Toggles between the **Animated Pet Face** and the **System Status Dashboard**.

### 📱 Capacitive Touchscreen Hotzones
The 2.4" display (GT911 capacitive touch) features 3 invisible hotzones:

| Touch Area | Screen Region (X, Y) | Function | Behavior |
| :--- | :--- | :--- | :--- |
| **Top-Left** | `(0, 0)` to `(160, 120)` | **PC RGB Toggle** | Toggles all workstation lighting (case/cooler fans + DDR5 RAM) between **Speed-2 Rainbow** and **Off**. |
| **Top-Right** | `(160, 0)` to `(320, 120)` | **Night Mode Toggle** | Dims workstation monitors to 0%, enables GNOME Night Light, and turns off the BOX-3 screen. Tap again to restore day mode. |
| **Bottom Row** | `(0, 120)` to `(320, 240)` | **System Status** | Toggles between the animated Pet face and the live System Status dashboard. |

---

## ✨ Features & Capabilities

### 1. 🎨 Dynamic Emotional Face (LVGL 8)
- Procedurally rendered using LVGL vector primitives—zero external bitmap images or flash assets.
- Smooth breathing bob animation, blinking eyes, and glance micro-movements.
- Dynamic color moods reflecting live workstation state:
  - 🩵 **Happy (`#00E5FF` Cyan)**: Normal active state with a cheerful smile and relaxed glance.
  - 🧡 **Focus (`#FF9F0A` Warm Amber)**: Concentrated squint and focused eyebrows when CPU (>35%) or GPU (>30%) is under active workload.
  - 💚 **Relax / Chill (`#30D158` Mint Green)**: Sleepy half-lidded zen expression with slow breathing during system idle.
  - 🩷 **Loved (`#FF375F` Blossom Pink)**: Excited curved eyes `(^.^)`, rosy blush, and wide smile triggered on touch interaction, appearance switch, or voice greeting.
  - ❤️ **Hot / Spicy (`#FF453A` Crimson)**: Furrowed brows and angry arc `(>_<)` when GPU temp exceeds 70°C or heavy load spikes occur.

### 2. 🐾 Multiple Pet Appearances
- 🤖 **Original (Cyber Desk Pet)**: Clean futuristic cyber companion with glowing pill eyes and sleek brows.
- 🐱 **Cat**: Adorable feline with pointed perky ears, delicate whiskers, sweet pink nose, and iconic `:3` split mouth.

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
│       ├── audio.c / audio.h       # ES7210 mic capture, VAD, ES8311 speaker chime
│       ├── telemetry.c / .h        # CSV protocol parser, metrics store, thread safety
│       ├── idf_component.yml       # Component requirements
│       └── CMakeLists.txt
├── host/                           # Ubuntu host-side service
│   ├── deskpet_host.py             # System telemetry collector, voice transcription & control
│   ├── deskpet.service             # systemd service unit for auto-start on boot
│   ├── 99-deskpet.rules            # udev rule granting user access to /dev/ttyACM0
│   ├── requirements.txt            # Python dependencies (pyserial, faster-whisper)
│   └── install.sh                  # Host service installer script
├── .gitignore                      # Ignore rules for build artifacts and virtualenvs
└── README.md                       # Complete documentation
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
.venv/bin/pip install -r requirements.txt

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
> pkill -f deskpet_host.py
> idf.py -p /dev/ttyACM0 flash
> ```

---

## 📡 Serial Wire Protocol

### Telemetry Stream (Host → Firmware)
The host service streams one CSV telemetry packet per second over `/dev/ttyACM0` at `115200` baud:
```text
PET,<epoch_utc>,<cpu_pct>,<ram_used_mb>,<ram_total_mb>,<gpu_pct>,<gpu_temp_c>,<vram_used_mb>,<vram_total_mb>\n
```

### Audio Stream (Firmware → Host)
When the pet's on-chip VAD detects speech, it streams base64-encoded 16kHz 16-bit mono PCM chunks directly to the host companion:
```text
AUD,START,16000,16,1\n
AUD,D,<base64_chunk_1>\n
AUD,D,<base64_chunk_2>\n
AUD,END\n
```

### Bidirectional Control Commands
- **Firmware → Host**:
  - `CMD,RGB_TOGGLE\n`: Toggles all workstation RGB devices (Fans & RAM).
  - `CMD,NIGHT_TOGGLE\n`: Toggles workstation night mode and monitor brightness.
- **Host → Firmware**:
  - `CMD,VOICE_RESP,<EMOTION>,<SUBTITLE_TEXT>\n`: Triggers visual celebration, chime, and displays subtitle text.
  - `CMD,VOICE_RESP,APPEARANCE_CAT\n`: Sets appearance to Cat.
  - `CMD,VOICE_RESP,APPEARANCE_ORIGINAL\n`: Sets appearance to Original Cyber Pet.
  - `CMD,VOICE_RESP,SCREEN_AI,SYSTEM STATUS\n`: Flips screen to status dashboard.

---

## 📜 License & Credits
- Built for **Espressif ESP32-S3-BOX-3**.
- UI powered by [LVGL 8](https://lvgl.io/).
- Speech transcription powered by [faster-whisper](https://github.com/SYSTRAN/faster-whisper).
- Concept inspired by [Tabbie](https://github.com/humjie/tabbie).
