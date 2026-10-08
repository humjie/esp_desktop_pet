#!/usr/bin/env python3
"""
Desk Pet host service — ESP32-S3-BOX-3 companion.

Streams live system load (CPU/RAM/GPU/VRAM) + GPU temperature from the Ubuntu
PC to the BOX-3 desk pet over the USB serial link (/dev/ttyACM0, 115200 baud),
and syncs the pet's clock every cycle.

Wire protocol (one CSV line per ~1s, magic prefix 'PET,'):
  PET,<epoch_sec>,<cpu_pct>,<ram_used_mb>,<ram_total_mb>,<gpu_pct>,<gpu_temp_c>,<vram_used_mb>,<vram_total_mb>
  (epoch is UTC seconds; the pet converts to local time)

The ESP32 also accepts command and speaker packets. The host batches microphone
input and processes commands without polling. Speech inference stays on the CPU;
GPU statistics come from read-only NVML queries, with no CUDA execution or VRAM
allocation.
"""

import os
import sys
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
import json
import queue
import ctypes
from logging.handlers import RotatingFileHandler

# NumPy operations here are small; BLAS pools add memory and idle threads.
os.environ["OPENBLAS_NUM_THREADS"] = "1"
os.environ["MKL_NUM_THREADS"] = "1"

from metrics import CpuSampler, GpuSampler, read_ram

# ----------------------------------------------------------------------------
# Configuration (defaults; override via ~/.config/deskpet/deskpet.conf)
# ----------------------------------------------------------------------------
DEFAULTS = {
    "serial_port": "/dev/ttyACM0",
    "baudrate": "115200",
    "interval_s": "1.0",
    "serial_write_timeout_s": "2.0",
    "speech_threads": "4",
    "tts_threads": "6",
    "tts_memory_arena": "false",
    "telemetry_log_interval_s": "60.0",
    "log_level": "INFO",
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
    level=getattr(logging, CFG["log_level"].upper(), logging.INFO),
    format="%(asctime)s %(levelname)s %(message)s",
    handlers=[logging.StreamHandler(sys.stdout),
              RotatingFileHandler(LOG_DIR / "deskpet.log", maxBytes=1048576,
                                  backupCount=2, encoding="utf-8")],
)
log = logging.getLogger("deskpet")

try:
    # ONNX frees its temporary workspace, but glibc can retain those free pages.
    # Return them after a response; models and active buffers remain allocated.
    s_malloc_trim = ctypes.CDLL(None).malloc_trim
    s_malloc_trim.argtypes = [ctypes.c_size_t]
    s_malloc_trim.restype = ctypes.c_int
except AttributeError:
    s_malloc_trim = None


def trim_native_memory():
    if s_malloc_trim is not None:
        s_malloc_trim(0)


s_cpu_sampler = CpuSampler()
s_gpu_sampler = None


def read_cpu_pct():
    return s_cpu_sampler.sample()


def read_gpu():
    global s_gpu_sampler
    if s_gpu_sampler is None:
        s_gpu_sampler = GpuSampler()
    return s_gpu_sampler.sample()


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
# Workstation Lighting and Night Mode Actions
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


def rainbow_planes():
    # Three starting phases, each duplicated for contiguous wraparound slices.
    lut = [bytes(int(component * 255) for component in colorsys.hsv_to_rgb(deg / 360.0, 1.0, 1.0))
           for deg in range(360)]
    return tuple(b"".join(lut[(i * 3 + phase) % 360] for i in range(120)) * 2
                 for phase in range(3))


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

            planes = rainbow_planes()
            packets = [bytearray([0xEC, 0x40, 0, offset, 20]) + bytearray(60)
                       for offset in range(0, 120, 20)]
            step = 0
            while not s_fan_stop_event.is_set():
                plane = planes[step % 3]
                start = (step // 3) * 3
                for index, packet in enumerate(packets):
                    offset = start + index * 60
                    packet[5:] = plane[offset:offset + 60]
                for channel in range(3):
                    for index, packet in enumerate(packets):
                        packet[2] = channel | (0x80 if index == 5 else 0)
                        f.write(packet)
                step = (step + RAINBOW_SPEED) % 360
                s_fan_stop_event.wait(0.03)

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
s_piper_voice = None
np = None
PIPER_MODEL_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "models", "piper")
PIPER_MODEL_PATH = os.path.join(PIPER_MODEL_DIR, "en_US-lessac-low.onnx")


def load_voice_models():
    """Keep models warm for immediate commands; all inference stays on the CPU."""
    global s_whisper_model, s_piper_voice, np
    import numpy as np
    try:
        from faster_whisper import WhisperModel
        log.info("Loading CPU speech recognition (tiny.en, int8)...")
        options = dict(device="cpu", compute_type="int8",
                       cpu_threads=max(1, int(CFG["speech_threads"])), num_workers=1)
        try:
            s_whisper_model = WhisperModel("tiny.en", local_files_only=True, **options)
        except FileNotFoundError:
            # Only the initial installation needs a model download.
            s_whisper_model = WhisperModel("tiny.en", **options)
        log.info("Whisper model ready.")
    except Exception as error:
        log.error("Failed to load Whisper model: %s", error)

    try:
        if not os.path.exists(PIPER_MODEL_PATH):
            log.warning("Piper model missing at %s; spoken TTS is unavailable.", PIPER_MODEL_PATH)
            return
        import onnxruntime as ort
        from piper.config import PiperConfig
        from piper.voice import PiperVoice
        options = ort.SessionOptions()
        options.intra_op_num_threads = max(1, int(CFG["tts_threads"]))
        options.inter_op_num_threads = 1
        options.enable_cpu_mem_arena = CFG["tts_memory_arena"].lower() in ("true", "1", "yes")
        options.add_session_config_entry("session.intra_op.allow_spinning", "0")
        options.add_session_config_entry("session.inter_op.allow_spinning", "0")
        with open(PIPER_MODEL_PATH + ".json", encoding="utf-8") as source:
            config = PiperConfig.from_dict(json.load(source))
        s_piper_voice = PiperVoice(config=config, session=ort.InferenceSession(
            PIPER_MODEL_PATH, sess_options=options, providers=["CPUExecutionProvider"]))
        log.info("Piper ready (CPU only, %d threads).", options.intra_op_num_threads)
    except Exception as error:
        log.error("Failed to load Piper TTS: %s", error)
    trim_native_memory()


def write_pcm_chunk(chunk):
    """Encode in C and write directly; pyserial already submits writes immediately."""
    line = b"SPK,D," + base64.b64encode(chunk) + b"\n"
    with s_serial_lock:
        if s_active_ser is None:
            return False
        try:
            s_active_ser.write(line)
            return True
        except Exception as error:
            log.warning("Audio serial write failed: %s", error)
            return False


def stream_pcm_to_esp(pcm_chunks, chunk_size=1024):
    """Stream synthesis as it arrives, with at most 64 ms of buffered lead."""
    if isinstance(pcm_chunks, (bytes, bytearray)):
        pcm_chunks = (pcm_chunks,)
    started = False
    next_send = 0.0
    try:
        for pcm in pcm_chunks:
            if not pcm:
                continue
            if not started:
                send_device_raw_cmd("SPK,TTS,START")
                next_send = time.monotonic()
                started = True
            view = memoryview(pcm)
            for offset in range(0, len(view), chunk_size):
                chunk = view[offset:offset + chunk_size]
                if not write_pcm_chunk(chunk):
                    return
                now = time.monotonic()
                next_send = max(next_send, now - 0.064) + len(chunk) / 32000.0
                delay = next_send - time.monotonic() - 0.064
                if delay > 0:
                    time.sleep(delay)
    finally:
        if started:
            send_device_raw_cmd("SPK,TTS,END")


s_tts_queue = queue.Queue(maxsize=2)
s_voice_queue = queue.Queue(maxsize=2)
s_control_event = threading.Event()


def enqueue_latest(work_queue, item):
    try:
        work_queue.put_nowait(item)
    except queue.Full:
        # Keep memory bounded and avoid processing stale speech during overload.
        try:
            work_queue.get_nowait()
            work_queue.task_done()
        except queue.Empty:
            pass
        try:
            work_queue.put_nowait(item)
        except queue.Full:
            pass
        log.debug("Replaced queued speech with a more recent request")


def speak_tts(text):
    if s_piper_voice is not None:
        clean_text = text.replace("=^.^=", "").replace("•", "").strip()
        if clean_text:
            enqueue_latest(s_tts_queue, clean_text)


def start_voice_workers():
    def tts_worker():
        while True:
            text = s_tts_queue.get()
            try:
                log.info("[TTS] Speaking: %s", text)
                stream_pcm_to_esp(chunk.audio_int16_bytes for chunk in s_piper_voice.synthesize(text))
            except Exception as error:
                log.warning("Piper speech error: %s", error)
            finally:
                s_tts_queue.task_done()
                trim_native_memory()

    def voice_worker():
        while True:
            audio = s_voice_queue.get()
            try:
                process_voice_audio(audio)
            finally:
                s_voice_queue.task_done()
                trim_native_memory()

    if s_piper_voice is not None:
        threading.Thread(target=tts_worker, name="deskpet-tts", daemon=True).start()
    if s_whisper_model is not None:
        threading.Thread(target=voice_worker, name="deskpet-stt", daemon=True).start()


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


def finish_audio_stream(proc, stop_event, restore_volume=False):
    global s_music_proc
    with s_music_lock:
        # An old stream must not stop a newer stream or reset its volume.
        if s_music_proc is proc:
            s_music_proc = None
            stop_event.set()
            if proc.poll() is None:
                proc.terminate()
            try:
                proc.wait(timeout=2)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
            send_device_raw_cmd("SPK,STOP")
            if restore_volume:
                send_device_raw_cmd("CMD,VOL,70")
    proc.stdout.close()


def music_play(station_key="lofi", delay_s=0.0):
    global s_music_proc
    def _starter():
        global s_music_proc, s_music_stop_event
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

            s_music_stop_event = threading.Event()
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
                send_device_raw_cmd("SPK,STOP")
                return

        def _music_worker(proc, stop_evt):
            CHUNK_SIZE = 1024  # 512 samples = 32ms
            log.info("[MUSIC] Streaming audio chunks to ESP speaker...")
            try:
                while not stop_evt.is_set():
                    raw = proc.stdout.read(CHUNK_SIZE)
                    if not raw:
                        break
                    if not write_pcm_chunk(raw):
                        break
            except Exception as err:
                log.warning("Music streaming error: %s", err)
            finally:
                finish_audio_stream(proc, stop_evt)
                log.info("[MUSIC] Music stream ended.")

        threading.Thread(target=_music_worker, args=(s_music_proc, s_music_stop_event), daemon=True).start()

    threading.Thread(target=_starter, daemon=True).start()


def pc_audio_play(delay_s=0.0):
    """Stream all PC sound output (PulseAudio/PipeWire monitor) to the ESP hardware speaker at MAX volume."""
    global s_music_proc
    def _starter():
        global s_music_proc, s_music_stop_event
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

            s_music_stop_event = threading.Event()
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
                send_device_raw_cmd("SPK,STOP")
                send_device_raw_cmd("CMD,VOL,70")
                return

        def _pc_audio_worker(proc, stop_evt):
            CHUNK_SIZE = 1024  # 512 samples = 32ms
            log.info("[PC AUDIO] Streaming PC audio chunks to ESP speaker...")
            try:
                while not stop_evt.is_set():
                    raw = proc.stdout.read(CHUNK_SIZE)
                    if not raw:
                        break
                    if not write_pcm_chunk(raw):
                        break
            except Exception as err:
                log.warning("PC audio streaming error: %s", err)
            finally:
                finish_audio_stream(proc, stop_evt, restore_volume=True)
                log.info("[PC AUDIO] PC audio stream ended.")

        threading.Thread(target=_pc_audio_worker, args=(s_music_proc, s_music_stop_event), daemon=True).start()

    threading.Thread(target=_starter, daemon=True).start()


s_serial_lock = threading.Lock()
s_active_ser = None

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
    if re.search(r"\b(?:hello|hi|hey|good morning|how are you|good day|what's up|meow)\b", inst):
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
        pc_audio_play(delay_s=1.8)
        send_device_response("HAPPY", "PC AUDIO STREAM", speak_text="Streaming PC sound to pet speaker.")
        return

    if any(k in inst for k in ["play lofi", "lofi music", "lo-fi", "lofi"]):
        log.info("Voice: Play Lo-Fi Music")
        music_play("lofi", delay_s=1.8)
        send_device_response("HAPPY", "PLAYING LO-FI", speak_text="Playing lo fi radio.")
        return

    if any(k in inst for k in ["play chill", "chill music", "ambient"]):
        log.info("Voice: Play Chill Music")
        music_play("chill", delay_s=1.8)
        send_device_response("HAPPY", "PLAYING CHILL", speak_text="Playing chill groove radio.")
        return

    if any(k in inst for k in ["play music", "play radio", "play song", "start music", "play"]):
        log.info("Voice: Play Music")
        music_play("lofi", delay_s=1.8)
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

        audio_np = np.frombuffer(audio_data, dtype=np.int16).astype(np.float32)
        audio_np *= 1.0 / 32768.0
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
            now = time.monotonic()
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
                    s_listening_window_expires = time.monotonic() + 6.0
                s_control_event.set()
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
    # One blocking reader consumes available bytes in batches rather than
    # polling every 10 ms or making one system call per serial byte.
    pending = bytearray()
    audio = bytearray()
    recording = False
    while not stop_event.is_set():
        try:
            raw = ser.read(min(8192, ser.in_waiting or 1))
            if not raw:
                continue
            pending.extend(raw)
            while True:
                end = pending.find(b"\n")
                if end < 0:
                    break
                line = bytes(pending[:end]).strip()
                del pending[:end + 1]
                if line.startswith(b"AUD,START,"):
                    audio.clear()
                    recording = True
                elif line.startswith(b"AUD,D,") and recording:
                    try:
                        chunk = base64.b64decode(line[6:], validate=True)
                        if len(audio) + len(chunk) > 131072:
                            recording = False
                            audio.clear()
                            log.warning("Discarded oversized speech capture")
                        else:
                            audio.extend(chunk)
                    except ValueError as error:
                        log.warning("Invalid audio chunk: %s", error)
                elif line == b"AUD,END" and recording:
                    if len(audio) >= 3200 and s_whisper_model is not None:
                        enqueue_latest(s_voice_queue, audio)
                    audio = bytearray()
                    recording = False
                elif line.startswith(b"CMD,"):
                    handle_device_command(line.decode("utf-8", errors="replace"))
                elif line == b"EVENT,MUSIC_STOP":
                    threading.Thread(target=music_stop, daemon=True).start()
                elif line:
                    severity = re.match(rb"(?:\x1b\[[0-9;]*m)?([EW]) \(", line)
                    level = (logging.ERROR if severity[1] == b"E" else logging.WARNING) if severity else logging.DEBUG
                    log.log(level, "[DEVICE] %s", line.decode("utf-8", errors="replace"))
            if len(pending) > 4096:
                pending.clear()
                log.warning("Discarded oversized serial line")
        except Exception as error:
            if not stop_event.is_set():
                log.warning("Serial reader stopped: %s", error)
            break


def main():
    global s_active_ser, s_listening_window_active
    log.info("Desk Pet host starting. port=%s baud=%s", PORT, BAUD)
    ensure_desktop_env()
    stopped = threading.Event()

    def stop(*_args):
        stopped.set()
        s_control_event.set()

    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)
    try:
        import serial  # noqa: F401
    except ImportError:
        log.error("pyserial not installed. Run: pip install pyserial")
        return 1

    load_voice_models()
    start_voice_workers()
    ser = None
    rx_stop_event = threading.Event()
    rx_thread = None
    interval = max(0.1, INTERVAL)
    next_tick = time.monotonic()
    next_log = 0.0
    s_cpu_sampler.sample()
    try:
        while not stopped.is_set():
            s_control_event.clear()
            now = time.monotonic()
            if now >= next_tick:
                if os.path.exists("/tmp/deskpet_flash_pause"):
                    if ser is not None:
                        rx_stop_event.set()
                        ser.close()
                        ser = s_active_ser = None
                else:
                    if ser is None or (rx_thread is not None and not rx_thread.is_alive()):
                        if ser is not None:
                            rx_stop_event.set()
                            ser.close()
                        ser = open_serial()
                        s_active_ser = ser
                        if ser is not None:
                            rx_stop_event = threading.Event()
                            rx_thread = threading.Thread(target=serial_reader_loop,
                                args=(ser, rx_stop_event), name="deskpet-serial", daemon=True)
                            rx_thread.start()
                    if ser is not None:
                        cpu = read_cpu_pct()
                        ram_u, ram_t = read_ram()
                        gpu, gpu_temp, vram_u, vram_t = read_gpu()
                        line = make_line(time.time(), cpu, ram_u, ram_t, gpu, gpu_temp, vram_u, vram_t)
                        if now >= next_log:
                            log.info("cpu=%.1f%% ram=%.1f/%.1fMB gpu=%.1f%% temp=%.1fC vram=%.0f/%.0fMB",
                                     cpu, ram_u, ram_t, gpu, gpu_temp, vram_u, vram_t)
                            next_log = now + max(interval, float(CFG["telemetry_log_interval_s"]))
                        try:
                            with s_serial_lock:
                                ser.write(line.encode("ascii"))
                        except Exception as error:
                            log.warning("Serial write failed; reconnecting: %s", error)
                            rx_stop_event.set()
                            ser.close()
                            ser = s_active_ser = None
                next_tick = max(next_tick + interval, time.monotonic())

                # Retain the developer command hook without a filesystem poll loop.
                trigger = Path("/tmp/deskpet_test_cmd")
                if trigger.exists():
                    try:
                        instruction = trigger.read_text().strip()
                        trigger.unlink()
                        if instruction:
                            execute_voice_instruction(instruction)
                    except OSError as error:
                        log.warning("Test command trigger error: %s", error)

            timeout = max(0.0, next_tick - time.monotonic())
            expired = False
            with s_listening_window_lock:
                if s_listening_window_active:
                    remaining = s_listening_window_expires - time.monotonic()
                    if remaining <= 0:
                        s_listening_window_active = False
                        expired = True
                    else:
                        timeout = min(timeout, remaining)
            if expired:
                log.info("Listening window timed out.")
                send_device_raw_cmd("CMD,VOICE_IDLE")
            if not stopped.is_set():
                s_control_event.wait(timeout)
    finally:
        rx_stop_event.set()
        if ser is not None:
            ser.close()
        s_active_ser = None
        if s_gpu_sampler is not None:
            s_gpu_sampler.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
