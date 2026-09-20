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
import io
import wave
import glob
import time
import signal
import logging
import colorsys
import threading
import subprocess
import configparser
import base64
import re
from pathlib import Path
import numpy as np

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
    target_port = PORT
    if not os.path.exists(target_port):
        candidates = glob.glob("/dev/ttyACM*") + glob.glob("/dev/ttyUSB*")
        if candidates:
            target_port = sorted(candidates)[0]
        else:
            log.warning("No serial port available (%s not found)", PORT)
            return None
    try:
        import serial
        ser = serial.Serial()
        ser.port = target_port
        ser.baudrate = BAUD
        ser.timeout = SERIAL_TIMEOUT
        ser.write_timeout = SERIAL_TIMEOUT
        try:
            ser.dtr = False
            ser.rts = False
            ser.exclusive = True
        except Exception:
            pass
        ser.open()
        return ser
    except Exception as e:  # noqa: BLE001
        log.warning("Cannot open %s: %s", target_port, e)
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

s_fan_thread = None
s_fan_stop_event = threading.Event()


def find_aura_hidraw():
    for dev_path in glob.glob('/sys/class/hidraw/hidraw*'):
        try:
            uevent_file = os.path.join(dev_path, 'device', 'uevent')
            if os.path.exists(uevent_file):
                with open(uevent_file) as f:
                    content = f.read()
                if '0B05:1BED' in content.upper() or '0B05' in content.upper():
                    return '/dev/' + os.path.basename(dev_path)
        except Exception:
            pass
    return '/dev/hidraw4'


def _send_aura_pkt(f, data):
    buf = bytearray(65)
    for i, b in enumerate(data):
        buf[i] = b
    f.write(buf)


def _send_aura_channel_colors(f, ch_id, colors_rgb):
    for pkt_idx in range(6):
        offset = pkt_idx * 20
        is_last = (pkt_idx == 5)
        ch_byte = (0x80 if is_last else 0x00) | (ch_id & 0x7F)
        buf = bytearray(65)
        buf[0] = 0xEC
        buf[1] = 0x40
        buf[2] = ch_byte
        buf[3] = offset
        buf[4] = 20
        for i in range(20):
            led_idx = offset + i
            r, g, b = colors_rgb[led_idx] if led_idx < len(colors_rgb) else (0, 0, 0)
            buf[5 + i*3] = r
            buf[6 + i*3] = g
            buf[7 + i*3] = b
        f.write(buf)


def _turn_off_fans():
    try:
        dev = find_aura_hidraw()
        with open(dev, 'rb+', buffering=0) as f:
            for ch in range(3):
                _send_aura_channel_colors(f, ch, [(0, 0, 0)] * 120)
            time.sleep(0.005)
            for ch in [0x00, 0x01, 0x02, 0x10, 0x11, 0x12]:
                _send_aura_pkt(f, [0xEC, 0x38, ch, 0x00, 0x00, 0x00])
                _send_aura_pkt(f, [0xEC, 0x35, ch, 0x00, 0x00, 0x00])
            _send_aura_pkt(f, [0xEC, 0x3F, 0x55])
    except Exception as e:
        log.warning("Failed to turn off fans: %s", e)


RAINBOW_SPEED = 2


def _fan_animate_worker():
    dev = find_aura_hidraw()
    try:
        with open(dev, 'rb+', buffering=0) as f:
            # 1. Power ON all channels with 0x38
            for ch in [0x00, 0x01, 0x02, 0x10, 0x11, 0x12]:
                _send_aura_pkt(f, [0xEC, 0x38, ch, 0x01, 0x00, 0x00])
                time.sleep(0.005)

            # 2. Set Direct mode on headers
            for ch in [0x00, 0x01, 0x02, 0x10, 0x11, 0x12]:
                _send_aura_pkt(f, [0xEC, 0x35, ch, 0x00, 0x00, 0xFF])
                time.sleep(0.005)

            # 3. Precompute 360-color rainbow lookup table for minimal CPU usage
            rainbow_lut = []
            for deg in range(360):
                r, g, b = colorsys.hsv_to_rgb(deg / 360.0, 1.0, 1.0)
                rainbow_lut.append((int(r * 255), int(g * 255), int(b * 255)))

            step = 0
            while not s_fan_stop_event.is_set():
                colors = [rainbow_lut[(i * 3 + step) % 360] for i in range(120)]
                for ch in range(3):
                    _send_aura_channel_colors(f, ch, colors)

                step = (step + RAINBOW_SPEED) % 360
                time.sleep(0.03)

            # Cleanly turn off fans on thread exit
            for ch in range(3):
                _send_aura_channel_colors(f, ch, [(0, 0, 0)] * 120)
            for ch in [0x00, 0x01, 0x02, 0x10, 0x11, 0x12]:
                _send_aura_pkt(f, [0xEC, 0x38, ch, 0x00, 0x00, 0x00])
                _send_aura_pkt(f, [0xEC, 0x35, ch, 0x00, 0x00, 0x00])
            _send_aura_pkt(f, [0xEC, 0x3F, 0x55])
    except Exception as e:
        log.warning("Fan RGB animation error: %s", e)


def ensure_desktop_env():
    """Ensure graphical and audio session environment variables exist for child processes."""
    if "DISPLAY" not in os.environ:
        os.environ["DISPLAY"] = ":0"
    if "XAUTHORITY" not in os.environ:
        os.environ["XAUTHORITY"] = os.path.expanduser("~/.Xauthority")
    uid = os.getuid()
    if "XDG_RUNTIME_DIR" not in os.environ:
        os.environ["XDG_RUNTIME_DIR"] = f"/run/user/{uid}"
    if "PULSE_SERVER" not in os.environ:
        pulse_sock = f"/run/user/{uid}/pulse/native"
        if os.path.exists(pulse_sock):
            os.environ["PULSE_SERVER"] = f"unix:{pulse_sock}"
    if "DBUS_SESSION_BUS_ADDRESS" not in os.environ:
        bus_path = f"/run/user/{uid}/bus"
        if os.path.exists(bus_path):
            os.environ["DBUS_SESSION_BUS_ADDRESS"] = f"unix:path={bus_path}"


def _set_ram_mode(mode_name, speed=None):
    """Set RAM mode asynchronously via OpenRGB without blocking the main loop."""
    def _worker():
        try:
            cmd = ["/usr/local/bin/openrgb", "--noautoconnect",
                   "-d", "0", "-m", mode_name]
            if speed is not None:
                cmd += ["-s", str(speed)]
            cmd += ["-d", "1", "-m", mode_name]
            if speed is not None:
                cmd += ["-s", str(speed)]
            subprocess.run(
                cmd,
                check=False,
                timeout=10,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
        except Exception as e:
            log.warning("OpenRGB RAM %s failed: %s", mode_name, e)

    threading.Thread(target=_worker, daemon=True).start()


def action_rgb_toggle():
    global s_rgb_on, s_fan_thread
    if not s_rgb_lock.acquire(blocking=False):
        log.info("[ACTION] RGB command already in progress, skipping duplicate request")
        return
    try:
        ensure_desktop_env()
        s_rgb_on = not s_rgb_on
        log.info("[ACTION] RGB toggle -> state=%s", s_rgb_on)

        if s_rgb_on:
            # 1. Start animated rainbow worker for motherboard fan headers immediately
            if s_fan_thread is not None and s_fan_thread.is_alive():
                s_fan_stop_event.set()
                s_fan_thread.join(timeout=1.0)
            s_fan_stop_event.clear()
            s_fan_thread = threading.Thread(target=_fan_animate_worker, daemon=True)
            s_fan_thread.start()

            # 2. Turn on RAM via OpenRGB into hardware Rainbow mode with speed 2
            _set_ram_mode("rainbow", speed=RAINBOW_SPEED)
        else:
            # 1. Stop animated fan worker and ensure fans are turned off
            if s_fan_thread is not None and s_fan_thread.is_alive():
                s_fan_stop_event.set()
                s_fan_thread.join(timeout=1.0)
            _turn_off_fans()

            # 2. Turn off RAM via OpenRGB asynchronously
            _set_ram_mode("off")
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


# ----------------------------------------------------------------------------
# Voice Recognition (STT) & Piper Speech (TTS) & Command Dispatch ("meow")
# ----------------------------------------------------------------------------
s_whisper_model = None
try:
    from faster_whisper import WhisperModel
    log.info("Loading faster-whisper speech recognition model (tiny.en)...")
    s_whisper_model = WhisperModel("tiny.en", device="cpu", compute_type="int8")
    log.info("Whisper model loaded and ready for voice commands.")
except Exception as e:
    log.error("Failed to load Whisper model: %s", e)

# Piper Neural TTS
s_piper_voice = None
PIPER_MODEL_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "models", "piper")
PIPER_MODEL_PATH = os.path.join(PIPER_MODEL_DIR, "en_US-lessac-low.onnx")
try:
    if os.path.exists(PIPER_MODEL_PATH):
        from piper.voice import PiperVoice
        log.info("Loading Piper Neural TTS model (%s)...", PIPER_MODEL_PATH)
        s_piper_voice = PiperVoice.load(PIPER_MODEL_PATH)
        log.info("Piper Neural TTS loaded and ready.")
    else:
        log.warning("Piper model file not found at %s. Spoken TTS will be unavailable until downloaded.", PIPER_MODEL_PATH)
except Exception as e:
    log.error("Failed to load Piper TTS: %s", e)


def stream_pcm_to_esp(pcm_data, chunk_size=1024):
    """Stream 16kHz mono 16-bit PCM bytes to ESP32 speaker over USB serial."""
    global s_active_ser
    if not pcm_data:
        return

    send_device_raw_cmd("SPK,TTS,START")

    # 1024 bytes = 512 samples @ 16kHz = 0.032 seconds
    chunk_delay = (chunk_size / 2) / 16000.0
    # Stream slightly ahead of realtime (~80%) so the 32KB ESP ring buffer stays primed
    sleep_interval = chunk_delay * 0.80

    for offset in range(0, len(pcm_data), chunk_size):
        chunk = pcm_data[offset : offset + chunk_size]
        b64_chunk = base64.b64encode(chunk).decode("ascii")
        line = f"SPK,D,{b64_chunk}\n"
        with s_serial_lock:
            if s_active_ser is not None:
                try:
                    s_active_ser.write(line.encode("ascii"))
                    s_active_ser.flush()
                except Exception as e:
                    log.warning("Failed to write SPK chunk to serial: %s", e)
                    break
        time.sleep(sleep_interval)

    send_device_raw_cmd("SPK,TTS,END")


def speak_tts(text):
    """Speak response aloud using Piper Neural TTS streamed directly to the ESP speaker."""
    if not s_piper_voice:
        return

    def _speak_worker():
        try:
            clean_text = text.replace("=^.^=", "").replace("•", "").strip()
            if not clean_text:
                return
            log.info("[TTS] Synthesizing speech: '%s'", clean_text)
            chunks = list(s_piper_voice.synthesize(clean_text))
            if not chunks:
                return
            raw_pcm = b"".join(c.audio_int16_bytes for c in chunks)
            log.info("[TTS] Streaming %d bytes of speech to ESP speaker...", len(raw_pcm))
            stream_pcm_to_esp(raw_pcm)
            log.info("[TTS] Speech playback complete.")
        except Exception as err:
            log.warning("Piper TTS speech error: %s", err)

    threading.Thread(target=_speak_worker, daemon=True).start()


# Music streaming process management
s_music_proc = None
s_music_stop_event = threading.Event()
s_music_lock = threading.Lock()
MUSIC_STREAMS = {
    "lofi": "http://ice1.somafm.com/groovesalad-128-mp3",
    "chill": "http://ice1.somafm.com/defcon-128-mp3",
    "radio": "http://stream.radioparadise.com/mp3-128",
}


def music_stop():
    global s_music_proc
    with s_music_lock:
        s_music_stop_event.set()
        if s_music_proc is not None:
            log.info("[MUSIC] Stopping internet music stream...")
            try:
                s_music_proc.terminate()
                s_music_proc.wait(timeout=2)
            except Exception:
                pass
            s_music_proc = None
        send_device_raw_cmd("SPK,STOP")


def music_play(station_key="lofi", delay_s=0.0):
    global s_music_proc
    def _starter():
        global s_music_proc
        if delay_s > 0:
            time.sleep(delay_s)
        with s_music_lock:
            s_music_stop_event.set()
            if s_music_proc is not None:
                try:
                    s_music_proc.terminate()
                    s_music_proc.wait(timeout=2)
                except Exception:
                    pass
                s_music_proc = None

            s_music_stop_event.clear()
            url = MUSIC_STREAMS.get(station_key, MUSIC_STREAMS["lofi"])
            log.info("[MUSIC] Streaming music to ESP speaker: %s (%s)", station_key, url)
            send_device_raw_cmd("SPK,MUS,START")

            # ffmpeg transcodes the internet audio stream to 16kHz mono 16-bit PCM in realtime (-re)
            cmd = [
                "ffmpeg",
                "-re",
                "-i", url,
                "-f", "s16le",
                "-ar", "16000",
                "-ac", "1",
                "-loglevel", "quiet",
                "pipe:1",
            ]
            try:
                s_music_proc = subprocess.Popen(
                    cmd,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.DEVNULL,
                )
            except Exception as e:
                log.error("Failed to start ffmpeg music transcode: %s", e)
                return

        def _music_worker(proc, stop_evt):
            CHUNK_SIZE = 1024  # 512 samples = 32ms
            log.info("[MUSIC] Streaming audio chunks to ESP speaker...")
            try:
                while not stop_evt.is_set():
                    raw = proc.stdout.read(CHUNK_SIZE)
                    if not raw:
                        break
                    b64_chunk = base64.b64encode(raw).decode("ascii")
                    line = f"SPK,D,{b64_chunk}\n"
                    with s_serial_lock:
                        if s_active_ser is not None:
                            try:
                                s_active_ser.write(line.encode("ascii"))
                                s_active_ser.flush()
                            except Exception:
                                break
            except Exception as err:
                log.warning("Music streaming error: %s", err)
            finally:
                send_device_raw_cmd("SPK,STOP")
                log.info("[MUSIC] Music stream ended.")

        threading.Thread(target=_music_worker, args=(s_music_proc, s_music_stop_event), daemon=True).start()

    threading.Thread(target=_starter, daemon=True).start()


def pc_audio_play(delay_s=0.0):
    """Stream all PC sound output (PulseAudio/PipeWire monitor) to the ESP hardware speaker at MAX volume."""
    global s_music_proc
    def _starter():
        global s_music_proc
        if delay_s > 0:
            time.sleep(delay_s)
        with s_music_lock:
            s_music_stop_event.set()
            if s_music_proc is not None:
                try:
                    s_music_proc.terminate()
                    s_music_proc.wait(timeout=2)
                except Exception:
                    pass
                s_music_proc = None

            s_music_stop_event.clear()
            log.info("[PC AUDIO] Streaming all PC sound to ESP speaker via PulseAudio monitor at MAX volume (100%)...")
            # Set ESP hardware speaker volume to max (100%) for PC sound
            send_device_raw_cmd("CMD,VOL,100")
            send_device_raw_cmd("SPK,MUS,START")

            # Capture all PC system output in realtime @ 16kHz mono 16-bit PCM
            env = os.environ.copy()
            if "XDG_RUNTIME_DIR" not in env or not os.path.exists(env["XDG_RUNTIME_DIR"]):
                env["XDG_RUNTIME_DIR"] = "/run/user/1000"
            if "PULSE_SERVER" not in env:
                env["PULSE_SERVER"] = "unix:/run/user/1000/pulse/native"

            cmd = [
                "parec",
                "--format=s16le",
                "--rate=16000",
                "--channels=1",
            ]
            try:
                s_music_proc = subprocess.Popen(
                    cmd,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.DEVNULL,
                    env=env,
                )
            except Exception as e:
                log.error("Failed to start parec PC audio stream: %s", e)
                return

        def _pc_audio_worker(proc, stop_evt):
            CHUNK_SIZE = 1024  # 512 samples = 32ms
            log.info("[PC AUDIO] Streaming PC audio chunks to ESP speaker...")
            try:
                while not stop_evt.is_set():
                    raw = proc.stdout.read(CHUNK_SIZE)
                    if not raw:
                        break
                    b64_chunk = base64.b64encode(raw).decode("ascii")
                    line = f"SPK,D,{b64_chunk}\n"
                    with s_serial_lock:
                        if s_active_ser is not None:
                            try:
                                s_active_ser.write(line.encode("ascii"))
                                s_active_ser.flush()
                            except Exception:
                                break
            except Exception as err:
                log.warning("PC audio streaming error: %s", err)
            finally:
                send_device_raw_cmd("SPK,STOP")
                # Restore pleasant default speaker volume (70%) for TTS speech
                send_device_raw_cmd("CMD,VOL,70")
                log.info("[PC AUDIO] PC audio stream ended, restored volume to 70%.")

        threading.Thread(target=_pc_audio_worker, args=(s_music_proc, s_music_stop_event), daemon=True).start()

    threading.Thread(target=_starter, daemon=True).start()


s_serial_lock = threading.Lock()
s_active_ser = None
s_audio_chunks = []
s_audio_lock = threading.Lock()

s_listening_window_active = False
s_listening_window_expires = 0.0
s_listening_window_lock = threading.Lock()


def send_device_raw_cmd(cmd):
    global s_active_ser
    with s_serial_lock:
        if s_active_ser is not None:
            try:
                line = f"{cmd.strip()}\n"
                s_active_ser.write(line.encode("utf-8"))
                s_active_ser.flush()
                log.info("[RAW CMD] Sent to BOX-3: %s", line.strip())
            except Exception as e:
                log.warning("Failed to send command to BOX-3: %s", e)


def send_device_response(action, text, speak_text=None):
    global s_active_ser
    with s_serial_lock:
        if s_active_ser is not None:
            try:
                line = f"CMD,VOICE_RESP,{action},{text}\n"
                s_active_ser.write(line.encode("utf-8"))
                s_active_ser.flush()
                log.info("[VOICE RESP] Sent to BOX-3: %s", line.strip())
            except Exception as e:
                log.warning("Failed to send response to BOX-3: %s", e)

    # Speak response using Piper TTS
    tts_message = speak_text if speak_text is not None else text
    if tts_message:
        speak_tts(tts_message)


def execute_voice_instruction(instruction):
    inst = instruction.lower().strip(" .,!?")
    log.info("[VOICE ACTION] Parsing instruction: '%s'", inst)

    # 1. Greetings
    if any(k in inst for k in ["hello", "hi", "hey", "good morning", "how are you", "good day", "what's up", "meow"]):
        log.info("Voice: Greeting handled")
        send_device_response("HAPPY", "MEOW HELLO! =^.^=", speak_text="Meow! Hello there, human!")
        return

    # 2. Appearance
    if any(k in inst for k in ["robot", "cyber", "original"]):
        log.info("Voice: Switch to robot")
        send_device_response("APPEARANCE_ORIGINAL", "CYBER PET READY", speak_text="Cyber robot pet mode activated!")
        return
    if "cat" in inst or "kitty" in inst:
        log.info("Voice: Switch to cat")
        send_device_response("APPEARANCE_CAT", "SWITCHED TO CAT", speak_text="Meow! Cat pet mode activated.")
        return
    if any(k in inst for k in ["switch", "change", "appearance", "transform"]):
        log.info("Voice: Toggle appearance")
        send_device_response("APPEARANCE_TOGGLE", "TRANSFORM!", speak_text="Transforming appearance!")
        return

    # 3. PC Lighting / RGB
    if any(k in inst for k in ["light", "lights", "rgb", "lamp", "color"]):
        log.info("Voice: Toggle RGB")
        threading.Thread(target=action_rgb_toggle, daemon=True).start()
        send_device_response("LOVE", "RGB TOGGLED", speak_text="Toggling PC lights.")
        return

    # 4. Night Mode / Sleep
    if any(k in inst for k in ["night", "sleep", "goodnight", "bedtime", "dark"]):
        log.info("Voice: Night mode")
        if not s_night_mode:
            threading.Thread(target=action_night_toggle, daemon=True).start()
        send_device_response("RELAX", "NIGHT MODE ON", speak_text="Good night. Entering sleep mode.")
        return

    # 5. Day Mode / Wake up
    if any(k in inst for k in ["day", "wake up", "wake", "morning", "bright"]):
        log.info("Voice: Day mode")
        if s_night_mode:
            threading.Thread(target=action_night_toggle, daemon=True).start()
        send_device_response("HAPPY", "DAY MODE READY", speak_text="Good morning! Day mode ready.")
        return

    # 6. Volume controls
    if any(k in inst for k in ["volume up", "louder", "turn up", "turn it up"]):
        log.info("Voice: Volume Up")
        send_device_raw_cmd("CMD,VOL,UP")
        send_device_response("HAPPY", "VOL +10%", speak_text="Volume up.")
        return

    if any(k in inst for k in ["volume down", "quieter", "turn down", "lower volume"]):
        log.info("Voice: Volume Down")
        send_device_raw_cmd("CMD,VOL,DOWN")
        send_device_response("HAPPY", "VOL -10%", speak_text="Volume down.")
        return

    if "mute" in inst or "unmute" in inst:
        log.info("Voice: Mute Toggle")
        send_device_raw_cmd("CMD,VOL,0")
        send_device_response("HAPPY", "MUTED", speak_text="Speaker muted.")
        return

    # 7. Music & Media controls
    if any(k in inst for k in ["stop music", "pause music", "stop stream", "be quiet", "silence"]):
        log.info("Voice: Music Stop")
        music_stop()
        try:
            subprocess.run(["playerctl", "pause"], check=False)
        except Exception:
            pass
        send_device_response("RELAX", "MUSIC STOPPED", speak_text="Music stopped.")
        return

    if any(k in inst for k in ["pc sound", "pc audio", "computer sound", "computer audio", "system sound", "play pc", "pc mode"]):
        log.info("Voice: Play PC Audio / Sound")
        threading.Thread(target=pc_audio_play, args=(1.8,), daemon=True).start()
        send_device_response("HAPPY", "PC AUDIO STREAM", speak_text="Streaming PC sound to pet speaker.")
        return

    if any(k in inst for k in ["play lofi", "lofi music", "lo-fi", "lofi"]):
        log.info("Voice: Play Lo-Fi Music")
        threading.Thread(target=music_play, args=("lofi", 1.8), daemon=True).start()
        send_device_response("HAPPY", "PLAYING LO-FI", speak_text="Playing lo fi radio.")
        return

    if any(k in inst for k in ["play chill", "chill music", "ambient"]):
        log.info("Voice: Play Chill Music")
        threading.Thread(target=music_play, args=("chill", 1.8), daemon=True).start()
        send_device_response("HAPPY", "PLAYING CHILL", speak_text="Playing chill groove radio.")
        return

    if any(k in inst for k in ["play music", "play radio", "play song", "start music", "play"]):
        log.info("Voice: Play Music")
        threading.Thread(target=music_play, args=("lofi", 1.8), daemon=True).start()
        try:
            subprocess.run(["playerctl", "play"], check=False)
        except Exception:
            pass
        send_device_response("HAPPY", "PLAYING MUSIC", speak_text="Playing music now!")
        return

    if any(k in inst for k in ["pause"]):
        log.info("Voice: Media Pause")
        music_stop()
        try:
            subprocess.run(["playerctl", "pause"], check=False)
        except Exception:
            pass
        send_device_response("RELAX", "MUSIC PAUSED", speak_text="Paused.")
        return

    if any(k in inst for k in ["next", "skip"]):
        log.info("Voice: Media Next")
        try:
            subprocess.run(["playerctl", "next"], check=False)
            send_device_response("HAPPY", "NEXT TRACK", speak_text="Next track.")
        except Exception as e:
            log.warning("playerctl failed: %s", e)
        return

    # 8. System Status
    if any(k in inst for k in ["status", "cpu", "stats", "dashboard", "specs", "load"]):
        log.info("Voice: Show system status")
        send_device_response("SCREEN_AI", "SYSTEM STATUS", speak_text="Displaying system performance dashboard.")
        return

    # 9. Default friendly reaction for any unrecognized instruction after meow
    log.info("Voice: Unrecognized instruction '%s' -> friendly reply", inst)
    send_device_response("LOVE", "PURR! MEOW =^.^=", speak_text="Meow! Purr!")


def process_voice_audio(audio_data):
    global s_listening_window_active, s_listening_window_expires
    if s_whisper_model is None or len(audio_data) < 3200:
        return

    try:
        # Ensure buffer length is an even number of bytes (16-bit int)
        if len(audio_data) % 2 != 0:
            audio_data = audio_data[: len(audio_data) - (len(audio_data) % 2)]

        audio_np = np.frombuffer(audio_data, dtype=np.int16).astype(np.float32) / 32768.0
        prompt = "Meow, robot, cat, lights, sleep, night mode, status, volume, pause, play, play music, lofi, chill, transform, stop music, pc sound, pc audio."
        segments, _info = s_whisper_model.transcribe(audio_np, beam_size=1, language="en", initial_prompt=prompt)
        raw_text = " ".join([s.text for s in segments]).strip()
        if not raw_text:
            return

        text = raw_text.lower().strip(" .,!?")
        log.info("[VOICE STT] Transcribed: '%s'", raw_text)

        wake_pattern = r'\b(meow|miao|mio|mew|me-ow|meow-meow)\b'
        match = re.search(wake_pattern, text)

        with s_listening_window_lock:
            now = time.time()
            is_active_window = s_listening_window_active and (now < s_listening_window_expires)

        if match:
            wake_end = match.end()
            raw_instruction = text[wake_end:].strip(" ,.!?")
            # If the instruction contains only wake word repetitions (e.g. "meow meow" or "mio mio"), strip them
            remaining = re.sub(wake_pattern, '', text).strip(" ,.!?")

            # Stop any music playing so user can speak and listen cleanly
            music_stop()

            if not raw_instruction or not remaining:
                # User called 'meow' -> Activate Listening Window!
                with s_listening_window_lock:
                    s_listening_window_active = True
                    s_listening_window_expires = time.time() + 6.0
                log.info("🌟 Wake word 'meow' detected! Entering LISTENING window (6s).")
                send_device_raw_cmd("CMD,VOICE_WAKE")
                return
            else:
                # User spoke 'meow <command>' in one sentence
                with s_listening_window_lock:
                    s_listening_window_active = False
                log.info("🌟 Wake word 'meow' with immediate command: '%s'", remaining)
                execute_voice_instruction(remaining)
                return

        # Direct stop/pause commands during music streaming without requiring 'meow'
        if s_music_proc is not None and any(k in text for k in ["stop", "pause", "quiet", "silence", "shut up", "turn off"]):
            log.info("🎯 Direct music stop command detected during playback: '%s'", text)
            execute_voice_instruction("stop music")
            return

        elif is_active_window:
            # Listening window is active, user spoke subsequent command (e.g. 'robot', 'lights', etc.)!
            with s_listening_window_lock:
                s_listening_window_active = False
            log.info("🎯 Executing subsequent voice command during active window: '%s'", text)
            execute_voice_instruction(text)
            return

        else:
            log.debug("Speech ignored (no 'meow' wake word and not in active listening window): '%s'", text)
            return

    except Exception as e:
        log.error("Error processing voice audio: %s", e)


def handle_device_command(cmd_line):
    cmd = cmd_line.strip()
    log.info("[DEVICE CMD] %s", cmd)
    if cmd == "CMD,RGB_TOGGLE":
        threading.Thread(target=action_rgb_toggle, daemon=True).start()
    elif cmd == "CMD,NIGHT_TOGGLE":
        threading.Thread(target=action_night_toggle, daemon=True).start()


def serial_reader_loop(ser, stop_event):
    global s_audio_chunks
    while not stop_event.is_set():
        try:
            if ser.in_waiting > 0:
                raw = ser.readline()
                if not raw:
                    time.sleep(0.005)
                    continue
                line = raw.decode("utf-8", errors="ignore").strip()
                if not line:
                    continue

                if line.startswith("AUD,START,"):
                    with s_audio_lock:
                        s_audio_chunks = []
                    log.info("[AUDIO] Speech capture started...")
                elif line.startswith("AUD,D,"):
                    b64_str = line[6:].strip()
                    try:
                        chunk = base64.b64decode(b64_str)
                        with s_audio_lock:
                            s_audio_chunks.append(chunk)
                    except Exception as e:
                        log.warning("Failed to decode audio chunk: %s", e)
                elif line.startswith("AUD,END"):
                    with s_audio_lock:
                        full_data = b"".join(s_audio_chunks)
                        s_audio_chunks = []
                    log.info("[AUDIO] Speech capture finished (%d bytes). Transcribing...", len(full_data))
                    threading.Thread(target=process_voice_audio, args=(full_data,), daemon=True).start()
                elif line.startswith("CMD,"):
                    handle_device_command(line)
                elif line.startswith("EVENT,"):
                    log.info("[EVENT] %s", line)
                    if line == "EVENT,MUSIC_STOP":
                        log.info("Physical button / touch pressed to stop music")
                        music_stop()
                else:
                    log.info("[DEVICE] %s", line)
            else:
                time.sleep(0.01)
        except Exception as e:
            log.warning("Serial reader loop error: %s", e)
            break


def main():
    global s_active_ser, s_listening_window_active, s_listening_window_expires
    log.info("Desk Pet host starting. port=%s baud=%s", PORT, BAUD)
    ensure_desktop_env()

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

    while running and os.path.exists("/tmp/deskpet_flash_pause"):
        log.info("Flash pause requested (/tmp/deskpet_flash_pause exists), waiting...")
        time.sleep(1.0)

    ser = open_serial()
    s_active_ser = ser
    rx_stop_event = threading.Event()
    rx_thread = None
    if ser is not None:
        rx_thread = threading.Thread(target=serial_reader_loop, args=(ser, rx_stop_event), daemon=True)
        rx_thread.start()

    try:
        while running:
            if os.path.exists("/tmp/deskpet_flash_pause"):
                if ser is not None:
                    rx_stop_event.set()
                    try:
                        ser.close()
                    except Exception:
                        pass
                    ser = None
                    s_active_ser = None
                time.sleep(1.0)
                continue

            epoch = time.time()

            cpu = read_cpu_pct()
            ram_u, ram_t = read_ram()
            gpu, gpu_temp, vram_u, vram_t = read_gpu()

            line = make_line(epoch, cpu, ram_u, ram_t, gpu, gpu_temp, vram_u, vram_t)
            log.info("cpu=%4.1f%% ram=%.1f/%.1fMB gpu=%4.1f%% temp=%5.1fC vram=%.0f/%.0fMB",
                     cpu, ram_u, ram_t, gpu, gpu_temp, vram_u, vram_t)

            if ser is None or (rx_thread is not None and not rx_thread.is_alive()):
                if ser is not None:
                    rx_stop_event.set()
                    try:
                        ser.close()
                    except Exception:
                        pass
                    time.sleep(1.0)
                ser = open_serial()
                s_active_ser = ser
                if ser is not None:
                    rx_stop_event = threading.Event()
                    rx_thread = threading.Thread(target=serial_reader_loop, args=(ser, rx_stop_event), daemon=True)
                    rx_thread.start()

            if ser is not None:
                try:
                    with s_serial_lock:
                        ser.write(line.encode("utf-8"))
                        ser.flush()
                except Exception as e:  # noqa: BLE001
                    log.warning("serial write failed: %s — reopening", e)
                    rx_stop_event.set()
                    try:
                        ser.close()
                    except Exception:  # noqa: BLE001
                        pass
                    ser = None
                    s_active_ser = None

            # wait for the next cycle (account for the CPU sample sleep already done)
            deadline = time.time() + INTERVAL
            while running and time.time() < deadline:
                # Check for test command trigger file
                if os.path.exists("/tmp/deskpet_test_cmd"):
                    try:
                        with open("/tmp/deskpet_test_cmd", "r") as f:
                            t_cmd = f.read().strip()
                        os.remove("/tmp/deskpet_test_cmd")
                        if t_cmd:
                            log.info("[TEST TRIGGER] Received test command: '%s'", t_cmd)
                            execute_voice_instruction(t_cmd)
                    except Exception as err:
                        log.warning("Test command trigger error: %s", err)

                # Check for listening window timeout
                with s_listening_window_lock:
                    if s_listening_window_active and time.time() >= s_listening_window_expires:
                        s_listening_window_active = False
                        log.info("Listening window timed out. Returning to idle.")
                        send_device_raw_cmd("CMD,VOICE_IDLE")
                time.sleep(0.05)
    finally:
        rx_stop_event.set()
        if ser is not None:
            try:
                ser.close()
            except Exception:  # noqa: BLE001
                pass
        s_active_ser = None
    return 0


if __name__ == "__main__":
    sys.exit(main())