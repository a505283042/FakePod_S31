# FakePod S31 · A2DP UI-stutter A/B test

This test isolates the actual A2DP encoder backend from the runtime codec selected in the settings UI. Selecting "SBC only" on an external-codec firmware does **not** switch to the built-in SBC encoder.

## Build matrix

| PlatformIO environment | Source backend | Source codec | External encoder CPU |
| --- | --- | --- | --- |
| `fakepod_s31` | ESP-IDF built-in | SBC | N/A |
| `fakepod_s31_aac` | External source | SBC / AAC | Core1 / P6 |
| `fakepod_s31_aac_tx_core0` | External source | SBC / AAC | Core0 / P6 |

All external-codec builds retain the existing SBC/AAC Sink decoders; the internal-SBC build does not enable external AAC reception. Do not switch the codec while listening; perform one test per fresh Bluetooth connection.

## Procedure

1. `git pull --ff-only` and flash one environment at a time with `pio run -e <ENV> -t upload`.
2. After boot, confirm `A2DP_TX_BACKEND=IDF_INTERNAL_SBC` or `A2DP_TX_BACKEND=EXTERNAL_SBC_AAC`. In the external build, also confirm `BT_TX_ENC: Encoder running ... core=0` or `core=1`.
3. Connect the **same** headphones. Use the same local 44.1 kHz FLAC file and volume, then start playback.
4. Let playback run for 10 seconds *after* PCM starts. In the external build this excludes startup `short_pcm` due to connecting before pressing Play.
5. Switch Home -> Settings -> Home and Home -> Spectrum -> Home repeatedly. Record the exact device uptime seconds when audio stutters.
6. Compare `BT_TX_ENC: A2DP_TX_EXT ... send_gap_max=... late_max=... encode_max=...` and `BT_AUDIO: A2DP_TX ... queued=... underrun=...`. Internal-SBC runs do not emit the external task's log.

## Interpretation

- Internal SBC smooth, external on Core1 stutters, external on Core0 smooth: encoder task affinity/competition with LVGL is implicated. Core0 contention still needs long-run validation.
- Internal SBC also stutters under this verified build: investigate shared memory/PSRAM, display DMA, Bluetooth controller and Core0 decoder paths rather than attributing the issue only to the external encoder.
- `send_gap_max` captures actual accepted packet intervals. For 44.1k SBC with four 128-sample frames, the expected nominal interval is ~11.61ms. AAC with 1024-sample frames is ~23.22ms. These are timing references, not guarantees.
- Startup `short_pcm` is expected before a song starts; its **increase while music is playing** is the key underrun evidence.
- Very low `DMA_free` warrants investigation but does not alone prove it caused the stutter.

Do not switch to the experimental Core0 build as a permanent default without verifying UI latency, network stability, decode throughput and audio over several songs.
