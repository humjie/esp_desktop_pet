"""Regression checks for resource limits, serial framing, and telemetry accuracy."""

import base64
import colorsys
import io
import queue
import threading
import unittest
from unittest.mock import Mock, mock_open, patch

import deskpet_host as host
import metrics


class MetricsTests(unittest.TestCase):
    def test_cpu_uses_interval_delta_and_correct_iowait(self):
        sampler = metrics.CpuSampler()
        lines = ["cpu 100 0 50 800 50 0 0 0 40 0\n",
                 "cpu 130 0 60 850 60 0 0 0 50 0\n"]
        with patch("builtins.open", side_effect=[io.StringIO(line) for line in lines]):
            self.assertEqual(sampler.sample(), 0)
            self.assertAlmostEqual(sampler.sample(), 40)

    def test_ram_matches_htop(self):
        data = "MemTotal: 10240 kB\nMemFree: 1024 kB\nBuffers: 512 kB\nCached: 2048 kB\nSReclaimable: 512 kB\nShmem: 256 kB\n"
        with patch("builtins.open", mock_open(read_data=data)):
            self.assertEqual(metrics.read_ram(), (6.25, 10))

    def test_missing_gpu_does_not_spawn_a_process_each_tick(self):
        with patch("metrics.ctypes.CDLL", side_effect=OSError), \
             patch("metrics.subprocess.run", side_effect=FileNotFoundError) as run, \
             patch("metrics.time.monotonic", return_value=100):
            sampler = metrics.GpuSampler()
            for _ in range(10):
                self.assertEqual(sampler.sample(), (0, 0, 0, 0))
            self.assertEqual(run.call_count, 1)


class FakeSerial:
    def __init__(self, packets=(), stop=None):
        self.packets = list(packets)
        self.stop = stop
        self.writes = []

    @property
    def in_waiting(self):
        return len(self.packets[0]) if self.packets else 0

    def read(self, size):
        if not self.packets:
            self.stop.set()
            return b""
        packet = self.packets.pop(0)
        if len(packet) > size:
            self.packets.insert(0, packet[size:])
        return packet[:size]

    def write(self, data):
        self.writes.append(bytes(data))

    def flush(self):
        pass


class SerialTests(unittest.TestCase):
    def capture(self, packets):
        stop = threading.Event()
        ser = FakeSerial(packets, stop)
        work = queue.Queue(maxsize=2)
        with patch.object(host, "s_voice_queue", work), \
             patch.object(host, "s_whisper_model", object()), \
             patch.object(host, "handle_device_command") as command:
            host.serial_reader_loop(ser, stop)
        return work, command

    def test_split_and_batched_serial_lines_preserve_audio(self):
        audio = b"\x01\x00" * 2048
        packet = b"AUD,START,16000,16,1\nAUD,D," + base64.b64encode(audio) + b"\nAUD,END\nCMD,NIGHT_TOGGLE\n"
        work, command = self.capture([packet[:7], packet[7:113], packet[113:]])
        self.assertEqual(work.get_nowait(), audio)
        command.assert_called_once_with("CMD,NIGHT_TOGGLE")

    def test_oversized_capture_is_discarded(self):
        packet = b"AUD,START,16000,16,1\n" + (b"AUD,D," + base64.b64encode(b"x" * 1024) + b"\n") * 130 + b"AUD,END\n"
        work, _ = self.capture([packet])
        self.assertTrue(work.empty())

    def test_audio_without_start_is_ignored(self):
        work, _ = self.capture([b"AUD,D," + base64.b64encode(b"x" * 4000) + b"\nAUD,END\n"])
        self.assertTrue(work.empty())

    def test_queue_retains_recent_work_and_balances_accounting(self):
        work = queue.Queue(maxsize=2)
        for number in range(100):
            host.enqueue_latest(work, number)
        self.assertEqual(work.unfinished_tasks, 2)
        self.assertEqual([work.get_nowait(), work.get_nowait()], [98, 99])

    def test_streaming_preserves_pcm_and_does_not_wait_for_whole_synthesis(self):
        ser = FakeSerial()
        first, second = b"a" * 2048, b"b" * 1024

        def chunks():
            yield first
            self.assertTrue(any(line.startswith(b"SPK,D,") for line in ser.writes))
            yield second

        with patch.object(host, "s_active_ser", ser), patch("deskpet_host.time.sleep"):
            host.stream_pcm_to_esp(chunks())
        decoded = b"".join(base64.b64decode(line[6:].strip()) for line in ser.writes if line.startswith(b"SPK,D,"))
        self.assertEqual(decoded, first + second)
        self.assertEqual(ser.writes[0], b"SPK,TTS,START\n")
        self.assertEqual(ser.writes[-1], b"SPK,TTS,END\n")


class PlaybackTests(unittest.TestCase):
    def test_chill_command_is_not_mistaken_for_hi(self):
        with patch.object(host, "music_play") as play, patch.object(host, "send_device_response"):
            host.execute_voice_instruction("play chill")
            play.assert_called_once_with("chill", delay_s=1.8)

    def test_old_stream_cannot_stop_its_replacement(self):
        old, new = Mock(), Mock()
        with patch.object(host, "s_music_proc", new), patch.object(host, "send_device_raw_cmd") as send:
            host.finish_audio_stream(old, threading.Event(), restore_volume=True)
            self.assertIs(host.s_music_proc, new)
            send.assert_not_called()
        old.stdout.close.assert_called_once()

    def test_finished_stream_is_reaped_and_volume_restored(self):
        proc = Mock()
        proc.poll.return_value = None
        stopped = threading.Event()
        with patch.object(host, "s_music_proc", proc), patch.object(host, "send_device_raw_cmd") as send:
            host.finish_audio_stream(proc, stopped, restore_volume=True)
            self.assertIsNone(host.s_music_proc)
            self.assertTrue(stopped.is_set())
            self.assertEqual([call.args[0] for call in send.call_args_list], ["SPK,STOP", "CMD,VOL,70"])
        proc.terminate.assert_called_once()
        proc.wait.assert_called_once()

    def test_compact_rainbow_planes_match_every_original_color(self):
        planes = host.rainbow_planes()
        self.assertEqual(sum(map(len, planes)), 2160)
        for step in range(360):
            start = (step // 3) * 3
            actual = planes[step % 3][start:start + 360]
            expected = bytes(int(component * 255) for led in range(120)
                             for component in colorsys.hsv_to_rgb(((led * 3 + step) % 360) / 360, 1, 1))
            self.assertEqual(actual, expected)


if __name__ == "__main__":
    unittest.main()
