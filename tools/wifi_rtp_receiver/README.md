# FakePod RTP L24 PC Receiver

This tool receives standard RTP L24 stereo PCM (RTP payload type 96) from the
optional FakePod Wi-Fi test sender and plays it through the Windows audio
device using PortAudio.

The receiver is discovered automatically over the local IPv4 network. FakePod
sends a small UDP broadcast discovery request; this program replies with its
RTP port. FakePod then sends the audio stream to the responding PC using UDP
unicast. No PC IP address needs to be entered in the firmware.

The audio payload is standard RTP L24: packed signed 24-bit PCM in network
byte order. The receiver automatically estimates the PCM clock rate from
ordered RTP timestamps and packet arrival times, then opens/reopens the
Windows output device at the detected rate.

## 1. Network setup

1. Connect FakePod and the Windows PC to the same Wi-Fi/LAN.
2. Make sure AP/client isolation is disabled so the devices can exchange
   local-network UDP packets.
3. Allow inbound UDP on both the RTP port (5004) and discovery port (5005) in
   Windows Firewall.

For example, in an elevated PowerShell window:

```powershell
New-NetFirewallRule -DisplayName "FakePod RTP L24 + Discovery" -Direction Inbound -Protocol UDP -LocalPort 5004,5005 -Action Allow
```

The discovery service listens on UDP 5005 by default. It answers only valid
FakePod discovery requests and returns the RTP port (5004 by default).

## 2. Install and start the receiver

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

Start the receiver with automatic sample-rate detection:

```powershell
python receiver.py --bind 0.0.0.0 --port 5004 --discovery-port 5005 --rate auto
```

The default values are RTP UDP 5004, discovery UDP 5005 and sample-rate
detection `auto`, so the shorter command below is equivalent:

```powershell
python receiver.py
```

Keep the receiver running before starting playback on FakePod. The console
prints that automatic discovery is ready. When FakePod probes the LAN, the
receiver replies automatically; FakePod logs the selected PC IP and RTP port.

The receiver uses a 1.25–2.5 second rolling regression of RTP timestamp
progression against packet arrival time, then classifies the stream as 44.1,
48, 88.2 or 96 kHz. The regression is evaluated four times per second to keep
the receive thread responsive. It automatically reopens the Windows output
stream when FakePod starts a new RTP session after switching to a track with a
different sample rate.

For a fixed-rate diagnostic test, use `--rate 44100`, `48000`, `88200` or
`96000` to disable automatic detection.

If the default output device cannot open the requested sample rate, select a
suitable device with `--device` after checking `--list-devices`.

## 3. Configure and build FakePod

No receiver IP configuration is required anymore. The isolated test build
enables automatic discovery and uses these defaults:

- RTP audio UDP port: `CONFIG_FAKEPOD_WIFI_RTP_TEST_PORT=5004`
- Receiver discovery UDP port:
  `CONFIG_FAKEPOD_WIFI_RTP_TEST_DISCOVERY_PORT=5005`

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

Start the PC receiver, make sure FakePod is connected to the same LAN, and
then play a normal Music track. FakePod broadcasts a discovery probe every two
seconds. The first valid receiver response is selected; audio is then sent to
that PC by unicast. The active receiver is refreshed by subsequent discovery
responses. If it disappears for about seven seconds, FakePod drops the target
and resumes discovery.

If more than one PC receiver is running, the first valid response wins while
that receiver remains online. Stop the selected receiver to allow FakePod to
discover another one.

## 4. Discovery and RTP protocol

### Automatic discovery

Discovery uses a compact 12-byte UDP request/response on port 5005:

- Request magic: `FPRD`, protocol version 1, random nonce.
- Response magic: `FPRR`, protocol version 1, advertised RTP port, echoed nonce.
- The response is sent by unicast to the source IP/port of the discovery request.
- FakePod accepts only a response matching its most recent nonce.

This nonce check filters unrelated UDP traffic and stale responses. Discovery
is limited to the local IPv4 broadcast domain; it does not cross routers/VLANs.

### RTP L24 audio

- RTP version: 2
- Payload type: 96 (dynamic)
- Audio encoding: L24
- Channels: 2, interleaved
- Sample representation: signed 24-bit two's complement
- Payload byte order: network byte order (big-endian)
- Packetization: up to 240 stereo frames per RTP packet
- Destination: automatically discovered PC IP, UDP port 5004 by default

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

## 5. Scope and limitations

- This is a test-only output tap for regular Music playback (local or NAS).
- Existing CS43131/I2S playback remains active; PC playback is an additional
  output. Adjust or mute the FakePod's local analog output if both are audible.
- The sender is disabled in the normal firmware environment.
- Automatic discovery requires FakePod and the PC to be on the same IPv4
  broadcast domain, with UDP 5005 permitted by the PC firewall and AP/client
  isolation disabled.
- One receiver is selected automatically. This version does not yet manage a
  user-selectable list of multiple receivers or synchronized multi-speaker
  playback.
- RTP L24 does not carry its clock rate in each RTP packet. This test receiver
  infers the rate from RTP timestamps and arrival timing; this inference is
  intended for this FakePod sender and is not a substitute for SDP/RTSP
  signaling with arbitrary third-party RTP senders.
- No audio is sent while Wi-Fi is disconnected or before a receiver is
  discovered. The sender queue is bounded; on overflow it drops stale queued
  audio and starts a new RTP session rather than delaying playback indefinitely.
- This validates FakePod-to-PC RTP L24 transport and decoded PCM playback. It
  does not yet validate Snapcast, DLNA, two-speaker clock synchronization,
  or the final speaker hardware.
