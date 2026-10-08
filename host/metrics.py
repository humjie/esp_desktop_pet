"""Low-overhead CPU/RAM sampling and read-only NVIDIA management queries."""

import ctypes
import logging
import subprocess
import time

log = logging.getLogger("deskpet")


class CpuSampler:
    """Sample utilization between telemetry ticks, without sleeping."""

    def __init__(self):
        self.previous = None

    def sample(self):
        try:
            with open("/proc/stat", encoding="ascii") as source:
                # Guest times are already included in user/nice; do not count twice.
                values = [int(value) for value in source.readline().split()[1:9]]
            current = (sum(values), values[3] + values[4])  # idle + iowait
            previous, self.previous = self.previous, current
            if previous is None or current[0] <= previous[0]:
                return 0.0
            return max(0.0, min(100.0, 100.0 *
                       (1.0 - (current[1] - previous[1]) / (current[0] - previous[0]))))
        except (OSError, ValueError, IndexError):
            return 0.0


def read_ram():
    """Return htop-compatible used/total RAM in MiB."""
    wanted = {"MemTotal", "MemFree", "Buffers", "Cached", "SReclaimable", "Shmem"}
    values = {}
    try:
        with open("/proc/meminfo", encoding="ascii") as source:
            for line in source:
                key, _, value = line.partition(":")
                if key in wanted:
                    values[key] = int(value.split()[0])
                    if len(values) == len(wanted):
                        break
        total = values.get("MemTotal", 0)
        used = (total - values.get("MemFree", 0) - values.get("Buffers", 0)
                - values.get("Cached", 0) - values.get("SReclaimable", 0)
                + values.get("Shmem", 0))
        return max(0, used) / 1024.0, total / 1024.0
    except (OSError, ValueError):
        return 0.0, 0.0


class _Utilization(ctypes.Structure):
    _fields_ = [("gpu", ctypes.c_uint), ("memory", ctypes.c_uint)]


class _Memory(ctypes.Structure):
    _fields_ = [("total", ctypes.c_ulonglong), ("free", ctypes.c_ulonglong),
                ("used", ctypes.c_ulonglong)]


class GpuSampler:
    """Use NVML's C API; it reads metrics without CUDA execution or VRAM allocation."""

    def __init__(self):
        self.library = None
        self.handle = ctypes.c_void_p()
        self.last = (0.0, 0.0, 0.0, 0.0)
        self.retry_at = 0.0
        try:
            library = ctypes.CDLL("libnvidia-ml.so.1")
            self._check(library.nvmlInit_v2())
            self.library = library
            library.nvmlDeviceGetHandleByIndex_v2.argtypes = [ctypes.c_uint, ctypes.POINTER(ctypes.c_void_p)]
            library.nvmlDeviceGetUtilizationRates.argtypes = [ctypes.c_void_p, ctypes.POINTER(_Utilization)]
            library.nvmlDeviceGetTemperature.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.POINTER(ctypes.c_uint)]
            library.nvmlDeviceGetMemoryInfo.argtypes = [ctypes.c_void_p, ctypes.POINTER(_Memory)]
            self._check(library.nvmlDeviceGetHandleByIndex_v2(0, ctypes.byref(self.handle)))
        except (OSError, AttributeError, RuntimeError) as error:
            log.debug("NVML unavailable; using nvidia-smi fallback: %s", error)
            self.close()

    @staticmethod
    def _check(result):
        if result:
            raise RuntimeError(f"NVML returned {result}")

    def sample(self):
        if time.monotonic() < self.retry_at:
            return self.last
        try:
            if self.library is not None:
                utilization, temperature, memory = _Utilization(), ctypes.c_uint(), _Memory()
                self._check(self.library.nvmlDeviceGetUtilizationRates(self.handle, ctypes.byref(utilization)))
                self._check(self.library.nvmlDeviceGetTemperature(self.handle, 0, ctypes.byref(temperature)))
                self._check(self.library.nvmlDeviceGetMemoryInfo(self.handle, ctypes.byref(memory)))
                self.last = (float(utilization.gpu), float(temperature.value),
                             max(0, memory.total - memory.free) / 1048576.0,
                             memory.total / 1048576.0)
            else:
                output = subprocess.run(
                    ["nvidia-smi", "--query-gpu=utilization.gpu,temperature.gpu,memory.total,memory.free",
                     "--format=csv,noheader,nounits"],
                    capture_output=True, text=True, timeout=1, check=True)
                gpu, temperature, total, free = map(float, output.stdout.splitlines()[0].split(","))
                self.last = gpu, temperature, max(0.0, total - free), total
        except (OSError, AttributeError, RuntimeError, subprocess.SubprocessError, ValueError, IndexError):
            self.last = (0.0, 0.0, 0.0, 0.0)
            # Avoid repeatedly spawning commands on machines without an NVIDIA GPU.
            self.retry_at = time.monotonic() + 30.0
        return self.last

    def close(self):
        if self.library is not None:
            self.library.nvmlShutdown()
            self.library = None
