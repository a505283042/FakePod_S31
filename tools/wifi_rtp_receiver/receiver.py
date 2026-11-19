#!/usr/bin/env python3
"""Receive RTP L24 stereo audio from FakePod and play it through PortAudio."""

from __future__ import annotations

import argparse
import queue
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
    def __init__(self, sample_rate: int, prebuffer_ms: int, queue_packets: int):
        self.sample_rate = sample_rate
        self.prebuffer_packets = max(
            2, int(np.ceil(sample_rate * prebuffer_ms / 1000.0 / PACKET_FRAMES))
        )
        self.audio_queue: queue.Queue[np.ndarray] = queue.Queue(maxsize=queue_packets)
        self.stop_event = threading.Event()
        self.thread: Optional[threading.Thread] = None
        self.sock: Optional[socket.socket] = None

        self.expected_sequence: Optional[int] = None
        self.ssrc: Optional[int] = None
        self.pending: dict[int, tuple[float, np.ndarray]] = {}
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
                self._enqueue_audio(item[1])
                self.expected_sequence = (expected + 1) & 0xFFFF
                continue

            ahead = []
            for sequence, (arrival, pcm) in self.pending.items():
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
        marker, sequence, _timestamp, ssrc, pcm = parse_rtp(packet)
        self.last_sender = f"{sender[0]}:{sender[1]}"

        if self.ssrc != ssrc or marker:
            if self.ssrc is not None:
                self.stream_restarts += 1
            self.ssrc = ssrc
            self.expected_sequence = sequence
            self.pending.clear()
            self._clear_audio_queue()
            self.playing = False

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

        self.pending[sequence] = (time.monotonic(), pcm)
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
        }


def main():
    parser = argparse.ArgumentParser(
        description="Receive standard RTP L24 stereo PCM and play it on the PC."
    )
    parser.add_argument("--bind", default="0.0.0.0", help="local IPv4 bind address")
    parser.add_argument("--port", type=int, default=5004, help="RTP UDP port")
    parser.add_argument("--rate", type=int, default=96000,
                        help="PCM sample rate; must match the playing track")
    parser.add_argument("--device", default=None,
                        help="PortAudio output device index or name")
    parser.add_argument("--prebuffer-ms", type=int, default=100)
    parser.add_argument("--queue-packets", type=int, default=512)
    parser.add_argument("--list-devices", action="store_true")
    args = parser.parse_args()

    if args.list_devices:
        print(sd.query_devices())
        return

    receiver = RtpAudioReceiver(
        sample_rate=args.rate,
        prebuffer_ms=args.prebuffer_ms,
        queue_packets=args.queue_packets,
    )
    receiver.start(args.bind, args.port)

    print(f"Listening RTP L24 stereo on {args.bind}:{args.port}")
    print(f"Expected format: {args.rate} Hz, 24-bit, 2 channels")
    print(f"Prebuffer: {args.prebuffer_ms} ms ({receiver.prebuffer_packets} packets)")
    print("Press Ctrl+C to stop.")
    print()

    try:
        with sd.OutputStream(
            device=args.device,
            samplerate=args.rate,
            channels=2,
            dtype="float32",
            blocksize=0,
            latency="high",
            callback=receiver.audio_callback,
        ):
            previous = receiver.stats_snapshot()
            previous_time = time.monotonic()
            while True:
                time.sleep(1.0)
                current = receiver.stats_snapshot()
                now = time.monotonic()
                elapsed = max(0.001, now - previous_time)
                packet_rate = (current["packets"] - previous["packets"]) / elapsed
                mbps = (
                    (current["bytes"] - previous["bytes"]) * 8.0
                    / elapsed / 1_000_000.0
                )
                print(
                    f"rx={packet_rate:6.1f} pkt/s  {mbps:5.2f} Mbps  "
                    f"lost={current['lost']} late={current['late']} "
                    f"dup={current['duplicate']} bad={current['bad']} "
                    f"q={current['queue']} overrun={current['queue_overruns']} "
                    f"underrun={current['underruns']} "
                    f"ssrc={current['ssrc']} from={current['sender']}"
                )
                previous = current
                previous_time = now
    except KeyboardInterrupt:
        print("\nStopping receiver...")
    finally:
        receiver.stop()


if __name__ == "__main__":
    main()
