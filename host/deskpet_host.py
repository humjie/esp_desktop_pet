#!/usr/bin/env python3
"""
Desk Pet host service — ESP32-S3-BOX-3 companion.

Streams live system load (CPU/RAM/GPU/VRAM) + GPU temperature from the Ubuntu
PC to the BOX-3 desk pet over the USB serial link (/dev/ttyACM0, 115200 baud),
and syncs the pet's clock every cycle.

Wire protocol (one CSV line per ~1s, magic prefix 'PET,'):
  PET,<epoch_sec>,<cpu_pct>,<ram_used_mb>,<ram_total_mb>,<gpu_pct>,<gpu_temp_c>,<vram_used_mb>,<vram_total_mb>
  (epoch is UTC seconds; the pet converts to local time)

The ESP32 parses only lines beginning with "PET,"; everything else on the
link is ignored by the pet (firmware logs may appear — they are ignored).
"""

import os
import sys
import time
import signal
import logging
import threading
import subprocess
import configparser
from pathlib import Path

# ----------------------------------------------------------------------------
# Configuration (defaults; override via ~/.config/deskpet/deskpet.conf)
# ----------------------------------------------------------------------------
DEFAULTS = {
    "serial_port": "/dev/ttyACM0",
    "baudrate": "115200",
    "interval_s": "1.0",
    "serial_write_timeout_s": "2.0",
}

CFG_DIR = Path.home() / ".config" / "deskpet"
LOG_DIR = Path.home() / ".local" / "share" / "deskpet"
LOG_DIR.mkdir(parents=True, exist_ok=True)


def load_config():
    cfg = dict(DEFAULTS)
    p = CFG_DIR / "deskpet.conf"
    if p.exists():
        cp = configparser.ConfigParser()
        cp.read(p)
        if cp.has_section("deskpet"):
            for k, v in cp.items("deskpet"):
                cfg[k] = v
    return cfg


CFG = load_config()
PORT = CFG["serial_port"]
BAUD = int(CFG["baudrate"])
INTERVAL = float(CFG["interval_s"])
SERIAL_TIMEOUT = float(CFG["serial_write_timeout_s"])

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s %(levelname)s %(message)s",
    handlers=[logging.StreamHandler(sys.stdout),
              logging.FileHandler(LOG_DIR / "deskpet.log", encoding="utf-8")],
)
log = logging.getLogger("deskpet")


def read_ram():
    """Return (used_mb, total_mb) matching htop's "used" value.

    htop computes used = MemTotal - MemFree - Buffers - (Cached + SReclaimable - Shmem)
    = MemTotal - MemFree - Buffers - Cached - SReclaimable + Shmem
    (shared memory like tmpfs/shm is accounted under Cached in /proc/meminfo
    but is unreclaimable, so htop adds it back to used application memory).
    Mirror that so the pet agrees with htop.
    """
    total = free = buffers = cached = sreclaim = shmem = used = 0.0
    try:
        with open("/proc/meminfo") as f:
            for line in f:
                if line.startswith("MemTotal:"):
                    total = int(line.split()[1]) / 1024.0
                elif line.startswith("MemFree:"):
                    free = int(line.split()[1]) / 1024.0
                elif line.startswith("Buffers:"):
                    buffers = int(line.split()[1]) / 1024.0
                elif line.startswith("Cached:"):
                    cached = int(line.split()[1]) / 1024.0
                elif line.startswith("SReclaimable:"):
                    sreclaim = int(line.split()[1]) / 1024.0
                elif line.startswith("Shmem:"):
                    shmem = int(line.split()[1]) / 1024.0
        used = total - free - buffers - cached - sreclaim + shmem
        if used < 0:
            used = 0.0
    except Exception:  # noqa: BLE001
        pass
    return used, total


def read_cpu_pct():
    """CPU utilization percentage over a short sample interval."""
    sample = 0.25
    try:
        f1 = open("/proc/stat")
        line1 = f1.readline()
        f1.close()
        time.sleep(sample)
        f2 = open("/proc/stat")
        line2 = f2.readline()
        f2.close()
        f1_vals = [int(x) for x in line1.split()[1:]]
        f2_vals = [int(x) for x in line2.split()[1:]]
        dtotal = sum(f2_vals) - sum(f1_vals)
        didle = (f2_vals[3] + (f2_vals[5] if len(f2_vals) > 5 else 0)) - \
                (f1_vals[3] + (f1_vals[5] if len(f1_vals) > 5 else 0))
        if dtotal <= 0:
            return 0.0
        return 100.0 * (1.0 - didle / dtotal)
    except Exception:  # noqa: BLE001
        return 0.0


def read_gpu():
    """Return (gpu_pct, gpu_temp_c, vram_used_mb, vram_total_mb).

    VRAM used is computed as memory.total - memory.free, matching how nvtop
    reports the MEM meter (nvtop sums total-free, not the smaller
    'memory.used' value). (0.0,0.0,0.0,0.0) if nvidia-smi fails.
    """
    try:
        out = subprocess.run(
            ["nvidia-smi",
             "--query-gpu=utilization.gpu,temperature.gpu,memory.total,memory.free",
             "--format=csv,noheader,nounits"],
            capture_output=True, text=True, timeout=5,
        )
    except Exception:  # noqa: BLE001
        return 0.0, 0.0, 0.0, 0.0
    if out.returncode != 0:
        return 0.0, 0.0, 0.0, 0.0
    try:
        cols = [c.strip() for c in out.stdout.strip().splitlines()[0].split(",")]
        # cols: utilization.gpu, temperature.gpu, memory.total, memory.free
        vram_total = float(cols[2])
        vram_free = float(cols[3])
        vram_used = vram_total - vram_free
        if vram_used < 0:
            vram_used = 0.0
        return float(cols[0]), float(cols[1]), vram_used, vram_total
    except (IndexError, ValueError):
        return 0.0, 0.0, 0.0, 0.0


def open_serial():
    """Open the serial port; return a file-like object or None.

    IMPORTANT: DTR/RTS are kept low. On an ESP32-S3 USB-Serial/JTAG, raising
    RTS (pyserial's default on open) hard-resets the MCU, causing a reboot
    loop on every reconnect. We must never toggle those lines.
    """
    try:
        import serial
        ser = serial.Serial(
            port=PORT, baudrate=BAUD, timeout=SERIAL_TIMEOUT, write_timeout=SERIAL_TIMEOUT)
        # Suppress DTR/RTS so opening the port does NOT reset the device.
        try:
            ser.dtr = False
            ser.rts = False
        except Exception:  # noqa: BLE001  (some ports don't expose these)
            pass
        return ser
    except Exception as e:  # noqa: BLE001
        log.warning("Cannot open %s: %s", PORT, e)
        return None


def make_line(epoch, cpu, ram_u, ram_t, gpu, gpu_temp, vram_u, vram_t):
    return ("PET,%d,%.1f,%.2f,%.2f,%.1f,%.1f,%.2f,%.2f\n"
            % (int(epoch), cpu, ram_u, ram_t, gpu, gpu_temp, vram_u, vram_t))


# ----------------------------------------------------------------------------
# Interactive Device Action Handlers (Top-Left RGB, Top-Right Night)
# ----------------------------------------------------------------------------
s_rgb_lock = threading.Lock()
s_night_lock = threading.Lock()
s_rgb_on = False  # Start False so first tap turns on Rainbow mode!
s_night_mode = False


def ensure_desktop_env():
    """Ensure graphical session environment variables exist for child processes."""
    if "DISPLAY" not in os.environ:
        os.environ["DISPLAY"] = ":0"
    if "XAUTHORITY" not in os.environ:
        os.environ["XAUTHORITY"] = os.path.expanduser("~/.Xauthority")
    if "DBUS_SESSION_BUS_ADDRESS" not in os.environ:
        uid = os.getuid()
        bus_path = f"/run/user/{uid}/bus"
        if os.path.exists(bus_path):
            os.environ["DBUS_SESSION_BUS_ADDRESS"] = f"unix:path={bus_path}"


def action_rgb_toggle():
    global s_rgb_on
    if not s_rgb_lock.acquire(blocking=False):
        log.info("[ACTION] RGB command already in progress, skipping duplicate request")
        return
    try:
        ensure_desktop_env()
        s_rgb_on = not s_rgb_on
        profile = "rainbow.orp" if s_rgb_on else "off.orp"
        log.info("[ACTION] RGB toggle -> %s (state=%s)", profile, s_rgb_on)
        try:
            res = subprocess.run(
                ["/usr/local/bin/openrgb", "--noautoconnect", "--profile", profile],
                check=False,
                timeout=15,
                capture_output=True,
                text=True,
            )
            log.info("OpenRGB profile loaded: code=%d stdout='%s'", res.returncode, res.stdout.strip())
        except Exception as e:  # noqa: BLE001
            log.warning("OpenRGB execution failed: %s", e)
    finally:
        s_rgb_lock.release()


def action_night_toggle():
    global s_night_mode
    if not s_night_lock.acquire(blocking=False):
        log.info("[ACTION] Night toggle command already in progress, skipping duplicate request")
        return
    try:
        ensure_desktop_env()
        s_night_mode = not s_night_mode
        brightness_val = "0" if s_night_mode else "30"
        night_light_val = "true" if s_night_mode else "false"
        log.info("[ACTION] Night toggle -> NightMode=%s (Monitors=%s%%, NightLight=%s)",
                 s_night_mode, brightness_val, night_light_val)

        # 1. GNOME Night Light
        try:
            subprocess.run(
                ["gsettings", "set", "org.gnome.settings-daemon.plugins.color", "night-light-enabled", night_light_val],
                check=False,
                timeout=5,
                env=os.environ,
            )
        except Exception as e:  # noqa: BLE001
            log.warning("gsettings night light failed: %s", e)

        # 2. Monitor hardware brightness via ddcutil
        for disp in ("1", "2"):
            try:
                subprocess.run(
                    ["ddcutil", "setvcp", "10", brightness_val, "-d", disp],
                    check=False,
                    timeout=8,
                )
            except Exception as e:  # noqa: BLE001
                log.warning("ddcutil display %s failed: %s", disp, e)
    finally:
        s_night_lock.release()


def handle_device_command(cmd_line):
    cmd = cmd_line.strip()
    log.info("[DEVICE CMD] %s", cmd)
    if cmd == "CMD,RGB_TOGGLE":
        threading.Thread(target=action_rgb_toggle, daemon=True).start()
    elif cmd == "CMD,NIGHT_TOGGLE":
        threading.Thread(target=action_night_toggle, daemon=True).start()


def main():
    log.info("Desk Pet host starting. port=%s baud=%s", PORT, BAUD)

    running = True

    def stop(*_args):
        nonlocal running
        running = False

    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)

    try:
        import serial  # noqa: F401  (verify availability early)
    except ImportError:
        log.error("pyserial not installed. Run: pip install pyserial")
        return 1

    ser = open_serial()

    try:
        while running:
            epoch = time.time()

            cpu = read_cpu_pct()
            ram_u, ram_t = read_ram()
            gpu, gpu_temp, vram_u, vram_t = read_gpu()

            line = make_line(epoch, cpu, ram_u, ram_t, gpu, gpu_temp, vram_u, vram_t)
            log.info("cpu=%4.1f%% ram=%.1f/%.1fMB gpu=%4.1f%% temp=%5.1fC vram=%.0f/%.0fMB",
                     cpu, ram_u, ram_t, gpu, gpu_temp, vram_u, vram_t)

            if ser is None:
                ser = open_serial()
            if ser is not None:
                try:
                    ser.write(line.encode("utf-8"))
                    ser.flush()

                    # Read incoming logs and commands from device if available
                    while ser.in_waiting > 0:
                        raw = ser.readline()
                        if not raw:
                            break
                        dev_line = raw.decode("utf-8", errors="ignore").strip()
                        if not dev_line:
                            continue
                        if dev_line.startswith("CMD,"):
                            handle_device_command(dev_line)
                        else:
                            log.info("[DEVICE] %s", dev_line)
                except Exception as e:  # noqa: BLE001
                    log.warning("serial write failed: %s — reopening", e)
                    try:
                        ser.close()
                    except Exception:  # noqa: BLE001
                        pass
                    ser = None

            # wait for the next cycle (account for the CPU sample sleep already done)
            deadline = time.time() + INTERVAL
            while running and time.time() < deadline:
                time.sleep(0.05)
    finally:
        if ser is not None:
            try:
                ser.close()
            except Exception:  # noqa: BLE001
                pass
    return 0


if __name__ == "__main__":
    sys.exit(main())