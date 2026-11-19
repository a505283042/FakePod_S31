# FakePod RTP L24 PC Receiver

This tool receives standard RTP L24 stereo PCM (RTP payload type 96) from the
optional FakePod Wi-Fi test sender and plays it through the Windows audio
device using PortAudio.

The audio payload is not a FakePod-specific UDP frame: it is packed 24-bit
signed PCM in network byte order as defined for RTP L24. By default, the PC
receiver automatically estimates the PCM clock rate from ordered RTP timestamps
and packet arrival times, then opens/reopens the Windows output device at the
detected rate.

## 1. Network setup

1. Connect FakePod and the Windows PC to the same LAN.
2. Find the PC IPv4 address with `ipconfig`.
3. Make sure the PC address is reachable from FakePod and that AP/client
   isolation is disabled.
4. Allow inbound UDP on port 5004 in Windows Firewall if prompted.

For example, in an elevated PowerShell window:

```powershell
New-NetFirewallRule -DisplayName "FakePod RTP L24" -Direction Inbound -Protocol UDP -LocalPort 5004 -Action Allow
```

## 2. Install the receiver

Use Python 3.10 or newer:

```powershell
cd tools\wifi_rtp_receiver
py -m venv .venv
.\.venv\Scripts\Activate.ps1
python -m pip install --upgrade pip
pip install -r requirements.txt
```

List available output devices:

```powershell
python receiver.py --list-devices
```

Start the receiver with automatic sample-rate detection (recommended):

```powershell
python receiver.py --bind 0.0.0.0 --port 5004 --rate auto
```

The default is `auto`, so `--rate auto` can also be omitted. The receiver
uses a 1.25–2.5 second rolling regression of RTP timestamp progression against
packet arrival time, then classifies the stream as 44.1, 48, 88.2 or 96 kHz. The regression is evaluated four times per second (rather than on every packet) to keep the receive thread responsive.
It automatically reopens the Windows output stream when FakePod starts a new
RTP session after switching to a track with a different sample rate.

For a fixed-rate diagnostic test, `--rate 44100`, `48000`, `88200` or
`96000` disables automatic detection.

If the default output device cannot open the requested sample rate, select a
suitable device with `--device` after checking `--list-devices`.

## 3. Configure and build FakePod

Edit `sdkconfig.wifi_rtp_test.defaults` and set
`CONFIG_FAKEPOD_WIFI_RTP_TEST_TARGET_IP` to the PC's IPv4 address before the
first build. If `sdkconfig.fakepod_s31_wifi_rtp_test` has already been
generated, change the same option in that generated file or run the test
environment's menuconfig target; defaults do not overwrite an existing
sdkconfig.

Build the isolated test environment:

```powershell
pio run -e fakepod_s31_wifi_rtp_test
```

Flash it:

```powershell
pio run -e fakepod_s31_wifi_rtp_test -t upload
```

The test environment uses a separate
`sdkconfig.fakepod_s31_wifi_rtp_test`; the normal `fakepod_s31` environment
and its generated sdkconfig are not changed.

After FakePod has connected to Wi-Fi, start the PC receiver first and then
play a normal Music track. The test sender taps decoded stereo PCM after a
successful I2S submission and sends it to the configured PC address.

## 4. RTP format

- RTP version: 2
- Payload type: 96 (dynamic)
- Audio encoding: L24
- Channels: 2, interleaved
- Sample representation: signed 24-bit two's complement
- Payload byte order: network byte order (big-endian)
- Packetization: up to 240 stereo frames per RTP packet
- Destination: configured IPv4 address, UDP port 5004 by default

At 96 kHz / 24-bit / stereo, the PCM payload rate is 4.608 Mbit/s. A full
240-frame packet contains 1,440 payload bytes plus the 12-byte RTP header.

The receiver uses a bounded RTP reorder stage followed by a preallocated,
frame-based PCM ring buffer. The PortAudio callback only copies already-decoded
PCM from this ring; it does not acquire Python Queue items or perform packet
work. The default startup prebuffer is 150 ms. The Windows UDP receive buffer
is enlarged to absorb short scheduling bursts. It reports packet rate,
throughput, loss, reordering, buffered audio in milliseconds, ring overruns,
UDP socket buffer size and playback underruns.

A matching SDP example for a 96 kHz / 24-bit stereo stream is included as
`fakepod-96k24.sdp`. It documents the standard dynamic payload mapping
`L24/96000/2` for use with other RTP-capable tools.

## 5. Scope and limitations of this first validation

- This is a test-only output tap for regular Music playback (local or NAS).
- Existing CS43131/I2S playback remains active; PC playback is an additional
  output. Adjust or mute the FakePod's local analog output if both are audible.
- The sender is disabled in the normal firmware environment.
- RTP L24 does not carry its clock rate in each RTP packet. This test receiver
  infers the rate from RTP timestamps and arrival timing; it is intended for
  this FakePod sender, whose RTP timestamp increments in PCM sample frames and
  whose SSRC changes on a sample-rate/session restart. This inference is not
  a substitute for SDP/RTSP clock-rate signaling when interoperating with
  arbitrary third-party RTP senders.
- No audio is sent while Wi-Fi is disconnected. The sender queue is bounded;
  on overflow it drops stale queued audio and starts a new RTP session rather
  than delaying playback indefinitely.
- This validates FakePod-to-PC RTP L24 transport and decoded PCM playback. It
  does not yet validate Snapcast, DLNA, two-speaker clock synchronization,
  or the final speaker hardware.
