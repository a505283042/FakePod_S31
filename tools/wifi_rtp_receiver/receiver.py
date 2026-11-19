#!/usr/bin/env python3
"""Receive RTP L24 stereo audio from FakePod and play it through PortAudio."""

from __future__ import annotations

import argparse
import queue
from collections import deque
import socket
import struct
import threading
import time
from typing import Optional

import numpy as np
import sounddevice as sd

RTP_VERSION = 2
RTP_PAYLOAD_TYPE_L24 = 96
RTP_FIXED_HEADER_BYTES = 12
BYTES_PER_STEREO_FRAME = 6
PACKET_FRAMES = 240


def decode_l24_stereo(payload: bytes) -> np.ndarray:
    if not payload or len(payload) % BYTES_PER_STEREO_FRAME:
        raise ValueError("RTP L24 payload length is not a whole number of stereo frames")

    octets = np.frombuffer(payload, dtype=np.uint8).reshape(-1, 2, 3).astype(np.int32)
    values = (
        (octets[:, :, 0] << 16)
        | (octets[:, :, 1] << 8)
        | octets[:, :, 2]
    )
    values = (values ^ 0x800000) - 0x800000
    return values.astype(np.float32) * (1.0 / 8388608.0)


def parse_rtp(packet: bytes):
    if len(packet) < RTP_FIXED_HEADER_BYTES:
        raise ValueError("short RTP packet")

    first = packet[0]
    second = packet[1]
    version = first >> 6
    if version != RTP_VERSION:
        raise ValueError("unsupported RTP version")

    padding = bool(first & 0x20)
    extension = bool(first & 0x10)
    csrc_count = first & 0x0F
    marker = bool(second & 0x80)
    payload_type = second & 0x7F
    if payload_type != RTP_PAYLOAD_TYPE_L24:
        raise ValueError(f"unexpected RTP payload type {payload_type}")

    sequence, timestamp, ssrc = struct.unpack_from("!HII", packet, 2)
    offset = RTP_FIXED_HEADER_BYTES + csrc_count * 4
    if len(packet) < offset:
        raise ValueError("truncated RTP CSRC list")

    if extension:
        if len(packet) < offset + 4:
            raise ValueError("truncated RTP extension header")
        extension_words = struct.unpack_from("!H", packet, offset + 2)[0]
        offset += 4 + extension_words * 4
        if len(packet) < offset:
            raise ValueError("truncated RTP extension data")

    end = len(packet)
    if padding:
        pad_bytes = packet[-1]
        if pad_bytes == 0 or pad_bytes > end - offset:
            raise ValueError("invalid RTP padding")
        end -= pad_bytes

    payload = packet[offset:end]
    pcm = decode_l24_stereo(payload)
    return marker, sequence, timestamp, ssrc, pcm


class RtpAudioReceiver:
    def __init__(self, sample_rate: Optional[int], prebuffer_ms: int, queue_packets: int):
        # None means automatic RTP clock-rate detection. RTP L24 carries no
        # explicit clock rate in each packet, so auto mode estimates it from
        # ordered RTP timestamps versus packet arrival time.
        self.manual_sample_rate = sample_rate
        self.detected_rate: Optional[int] = sample_rate
        self.rate_estimate_hz: Optional[float] = (
            float(sample_rate) if sample_rate is not None else None
        )
        self.prebuffer_ms = prebuffer_ms
        self.prebuffer_packets = self._prebuffer_count(sample_rate, prebuffer_ms) if sample_rate else 40
        self.rate_observations = deque(maxlen=1024)
        self.last_rtp_timestamp: Optional[int] = None
        self.unwrapped_rtp_timestamp = 0
        self.rate_candidate: Optional[int] = None
        self.rate_candidate_streak = 0
        self.audio_queue: queue.Queue[np.ndarray] = queue.Queue(maxsize=queue_packets)
        self.stop_event = threading.Event()
        self.thread: Optional[threading.Thread] = None
        self.sock: Optional[socket.socket] = None

        self.expected_sequence: Optional[int] = None
        self.ssrc: Optional[int] = None
        self.pending: dict[int, tuple[float, int, np.ndarray]] = {}
        self.reorder_wait_seconds = 0.006

        self.callback_generation = 0
        self.generation = 0
        self.current_block: Optional[np.ndarray] = None
        self.current_offset = 0
        self.playing = False

        self.packets = 0
        self.bytes_received = 0
        self.lost_packets = 0
        self.late_packets = 0
        self.duplicate_packets = 0
        self.bad_packets = 0
        self.queue_overruns = 0
        self.underruns = 0
        self.stream_restarts = 0
        self.last_sender = ""

    @staticmethod
    def _prebuffer_count(sample_rate: Optional[int], prebuffer_ms: int) -> int:
        if not sample_rate:
            return 40
        return max(
            2, int(np.ceil(sample_rate * prebuffer_ms / 1000.0 / PACKET_FRAMES))
        )

    def _reset_rate_detector(self):
        self.rate_observations.clear()
        self.last_rtp_timestamp = None
        self.unwrapped_rtp_timestamp = 0
        self.rate_candidate = None
        self.rate_candidate_streak = 0
        self.rate_estimate_hz = None
        if self.manual_sample_rate is None:
            self.detected_rate = None

    def _observe_sample_rate(self, rtp_timestamp: int, arrival_time: float):
        if self.manual_sample_rate is not None:
            return

        if self.last_rtp_timestamp is None:
            self.last_rtp_timestamp = rtp_timestamp
            self.unwrapped_rtp_timestamp = rtp_timestamp
            self.rate_observations.append((arrival_time, self.unwrapped_rtp_timestamp))
            return

        # Unwrap the 32-bit RTP timestamp. Packets reach this method in RTP
        # sequence order, after the receiver's small reorder buffer.
        delta = (rtp_timestamp - self.last_rtp_timestamp) & 0xFFFFFFFF
        if delta == 0 or delta >= 0x80000000:
            return
        self.unwrapped_rtp_timestamp += delta
        self.last_rtp_timestamp = rtp_timestamp
        self.rate_observations.append((arrival_time, self.unwrapped_rtp_timestamp))

        # Use a multi-second regression to average out Wi-Fi jitter and task
        # scheduling bursts. The RTP timestamp clock is the PCM sample clock.
        while (
            len(self.rate_observations) > 2
            and arrival_time - self.rate_observations[0][0] > 2.5
        ):
            self.rate_observations.popleft()

        if len(self.rate_observations) < 40:
            return

        first_time = self.rate_observations[0][0]
        last_time = self.rate_observations[-1][0]
        if last_time - first_time < 1.25:
            return

        times = np.fromiter(
            (item[0] for item in self.rate_observations), dtype=np.float64
        )
        ticks = np.fromiter(
            (item[1] for item in self.rate_observations), dtype=np.float64
        )
        times -= times[0]
        times_centered = times - times.mean()
        denominator = float(np.dot(times_centered, times_centered))
        if denominator <= 0.0:
            return

        ticks_centered = ticks - ticks.mean()
        estimate = float(np.dot(times_centered, ticks_centered) / denominator)
        self.rate_estimate_hz = estimate

        supported_rates = (44100, 48000, 88200, 96000)
        candidate = min(supported_rates, key=lambda rate: abs(rate - estimate))
        relative_error = abs(candidate - estimate) / candidate
        if relative_error > 0.015:
            self.rate_candidate = None
            self.rate_candidate_streak = 0
            return

        if candidate == self.rate_candidate:
            self.rate_candidate_streak += 1
        else:
            self.rate_candidate = candidate
            self.rate_candidate_streak = 1

        # Require repeated agreement after a >=1.25 s observation window.
        if self.rate_candidate_streak >= 8 and self.detected_rate != candidate:
            self.detected_rate = candidate
            self.prebuffer_packets = self._prebuffer_count(candidate, self.prebuffer_ms)
            # Discard packets accumulated while identifying the stream so
            # playback starts near live time after PortAudio opens the device.
            self._clear_audio_queue()
            self.playing = False
            print(f"\n[AUTO] RTP clock rate detected: {candidate} Hz")

    def _clear_audio_queue(self):
        while True:
            try:
                self.audio_queue.get_nowait()
            except queue.Empty:
                break
        self.generation += 1

    def _enqueue_audio(self, pcm: np.ndarray):
        if self.audio_queue.full():
            try:
                self.audio_queue.get_nowait()
                self.queue_overruns += 1
            except queue.Empty:
                pass
        try:
            self.audio_queue.put_nowait(pcm)
        except queue.Full:
            self.queue_overruns += 1

    def _flush_reorder_buffer(self, now: float):
        while self.expected_sequence is not None and self.pending:
            expected = self.expected_sequence
            item = self.pending.pop(expected, None)
            if item is not None:
                arrival, rtp_timestamp, pcm = item
                self._observe_sample_rate(rtp_timestamp, arrival)
                self._enqueue_audio(pcm)
                self.expected_sequence = (expected + 1) & 0xFFFF
                continue

            ahead = []
            for sequence, (arrival, rtp_timestamp, pcm) in self.pending.items():
                distance = (sequence - expected) & 0xFFFF
                if 0 < distance < 0x8000:
                    ahead.append((distance, arrival, pcm))

            if not ahead:
                return

            distance, arrival, next_pcm = min(ahead, key=lambda item: item[0])
            if now - arrival < self.reorder_wait_seconds:
                return

            # One missing RTP packet is represented by one packet-sized silence
            # block. This preserves the RTP sample timeline instead of shifting it.
            self._enqueue_audio(np.zeros_like(next_pcm))
            self.lost_packets += 1
            self.expected_sequence = (expected + 1) & 0xFFFF

    def _handle_packet(self, packet: bytes, sender):
        marker, sequence, rtp_timestamp, ssrc, pcm = parse_rtp(packet)
        self.last_sender = f"{sender[0]}:{sender[1]}"

        if self.ssrc != ssrc or marker:
            if self.ssrc is not None:
                self.stream_restarts += 1
            self.ssrc = ssrc
            self.expected_sequence = sequence
            self.pending.clear()
            self._clear_audio_queue()
            self.playing = False
            if self.manual_sample_rate is None:
                self._reset_rate_detector()

        if self.expected_sequence is None:
            self.expected_sequence = sequence

        distance = (sequence - self.expected_sequence) & 0xFFFF
        if distance >= 0x8000:
            self.late_packets += 1
            return
        if distance > 128:
            self.lost_packets += distance
            self.pending.clear()
            self._clear_audio_queue()
            self.expected_sequence = sequence
            self.playing = False

        if sequence in self.pending:
            self.duplicate_packets += 1
            return

        self.pending[sequence] = (time.monotonic(), rtp_timestamp, pcm)
        self.packets += 1
        self.bytes_received += len(packet)
        self._flush_reorder_buffer(time.monotonic())

    def _receive_loop(self, bind_address: str, port: int):
        assert self.sock is not None
        self.sock.settimeout(0.005)
        while not self.stop_event.is_set():
            try:
                packet, sender = self.sock.recvfrom(2048)
            except socket.timeout:
                self._flush_reorder_buffer(time.monotonic())
                continue
            except OSError:
                if not self.stop_event.is_set():
                    self.bad_packets += 1
                break

            try:
                self._handle_packet(packet, sender)
            except (ValueError, struct.error):
                self.bad_packets += 1

    def start(self, bind_address: str, port: int):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1 << 20)
        self.sock.bind((bind_address, port))
        self.thread = threading.Thread(
            target=self._receive_loop,
            args=(bind_address, port),
            name="RTP-L24-Receiver",
            daemon=True,
        )
        self.thread.start()

    def stop(self):
        self.stop_event.set()
        if self.sock is not None:
            self.sock.close()
            self.sock = None
        if self.thread is not None:
            self.thread.join(timeout=1.0)

    def audio_callback(self, outdata, frames, _time_info, status):
        outdata.fill(0)

        if status:
            self.underruns += 1

        if self.callback_generation != self.generation:
            self.current_block = None
            self.current_offset = 0
            self.callback_generation = self.generation
            self.playing = False

        if not self.playing:
            if self.audio_queue.qsize() < self.prebuffer_packets:
                return
            self.playing = True

        written = 0
        while written < frames:
            if self.current_block is None:
                try:
                    self.current_block = self.audio_queue.get_nowait()
                    self.current_offset = 0
                except queue.Empty:
                    self.underruns += 1
                    self.playing = False
                    break

            available = len(self.current_block) - self.current_offset
            count = min(frames - written, available)
            outdata[written:written + count] = self.current_block[
                self.current_offset:self.current_offset + count
            ]
            written += count
            self.current_offset += count

            if self.current_offset >= len(self.current_block):
                self.current_block = None
                self.current_offset = 0

    def stats_snapshot(self):
        return {
            "packets": self.packets,
            "bytes": self.bytes_received,
            "lost": self.lost_packets,
            "late": self.late_packets,
            "duplicate": self.duplicate_packets,
            "bad": self.bad_packets,
            "queue": self.audio_queue.qsize(),
            "queue_overruns": self.queue_overruns,
            "underruns": self.underruns,
            "ssrc": self.ssrc,
            "sender": self.last_sender,
            "rate": self.detected_rate,
            "rate_estimate": self.rate_estimate_hz,
            "rate_candidate": self.rate_candidate,
        }


def main():
    parser = argparse.ArgumentParser(
        description="Receive standard RTP L24 stereo PCM and play it on the PC."
    )
    parser.add_argument("--bind", default="0.0.0.0", help="local IPv4 bind address")
    parser.add_argument("--port", type=int, default=5004, help="RTP UDP port")
    parser.add_argument("--rate", default="auto",
                        help="PCM sample rate (44100/48000/88200/96000) or auto")
    parser.add_argument("--device", default=None,
                        help="PortAudio output device index or name")
    parser.add_argument("--prebuffer-ms", type=int, default=100)
    parser.add_argument("--queue-packets", type=int, default=512)
    parser.add_argument("--list-devices", action="store_true")
    args = parser.parse_args()

    if args.list_devices:
        print(sd.query_devices())
        return

    try:
        sample_rate = None if args.rate.lower() == "auto" else int(args.rate)
    except ValueError:
        parser.error("--rate must be auto, 44100, 48000, 88200 or 96000")
    if sample_rate is not None and sample_rate not in (44100, 48000, 88200, 96000):
        parser.error("supported rates: 44100, 48000, 88200, 96000")

    receiver = RtpAudioReceiver(
        sample_rate=sample_rate,
        prebuffer_ms=args.prebuffer_ms,
        queue_packets=args.queue_packets,
    )
    receiver.start(args.bind, args.port)

    print(f"Listening RTP L24 stereo on {args.bind}:{args.port}")
    print(
        "Sample rate: automatic detection from RTP timestamps"
        if sample_rate is None
        else f"Sample rate: fixed at {sample_rate} Hz"
    )
    print("Press Ctrl+C to stop.")
    print()

    output_stream = None
    output_rate = None
    attempted_rate = None
    previous = receiver.stats_snapshot()
    previous_time = time.monotonic()
    last_stats_time = previous_time

    try:
        while True:
            current = receiver.stats_snapshot()
            requested_rate = current["rate"]

            # PortAudio streams cannot change sample rate in-place. Reopen the
            # output stream when a new RTP session is detected after a track
            # sample-rate change.
            if requested_rate != output_rate and requested_rate != attempted_rate:
                if output_stream is not None:
                    try:
                        output_stream.stop()
                    finally:
                        output_stream.close()
                    output_stream = None
                    output_rate = None

                attempted_rate = requested_rate
                if requested_rate is not None:
                    candidate_stream = None
                    try:
                        candidate_stream = sd.OutputStream(
                            device=args.device,
                            samplerate=requested_rate,
                            channels=2,
                            dtype="float32",
                            blocksize=0,
                            latency="high",
                            callback=receiver.audio_callback,
                        )
                        candidate_stream.start()
                        output_stream = candidate_stream
                        output_rate = requested_rate
                        print(f"[AUDIO] Windows output opened at {requested_rate} Hz")
                    except Exception as exc:
                        if candidate_stream is not None:
                            try:
                                candidate_stream.close()
                            except Exception:
                                pass
                        output_stream = None
                        output_rate = None
                        print(
                            f"[AUDIO] Cannot open output at {requested_rate} Hz: {exc}. "
                            "Check the selected Windows output device."
                        )

            now = time.monotonic()
            if now - last_stats_time >= 1.0:
                elapsed = max(0.001, now - previous_time)
                packet_rate = (current["packets"] - previous["packets"]) / elapsed
                mbps = (
                    (current["bytes"] - previous["bytes"]) * 8.0
                    / elapsed / 1_000_000.0
                )
                rate_text = (
                    f"{current['rate']} Hz"
                    if current["rate"] is not None
                    else "detecting"
                )
                estimate = current["rate_estimate"]
                estimate_text = f"{estimate:.0f}" if estimate is not None else "-"
                print(
                    f"rate={rate_text} est={estimate_text} Hz  "
                    f"rx={packet_rate:6.1f} pkt/s  {mbps:5.2f} Mbps  "
                    f"lost={current['lost']} late={current['late']} "
                    f"dup={current['duplicate']} bad={current['bad']} "
                    f"q={current['queue']} overrun={current['queue_overruns']} "
                    f"underrun={current['underruns']} "
                    f"ssrc={current['ssrc']} from={current['sender']}"
                )
                previous = current
                previous_time = now
                last_stats_time = now

            time.sleep(0.1)
    except KeyboardInterrupt:
        print("\nStopping receiver...")
    finally:
        if output_stream is not None:
            try:
                output_stream.stop()
            finally:
                output_stream.close()
        receiver.stop()


if __name__ == "__main__":
    main()
