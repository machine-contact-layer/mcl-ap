# Experiment 001 calibration follow-up - 2026-09-02

Status: **FREQUENCY RESPONSE MEASURED; E3 EXACT RECOVERY NOT ACHIEVED**

This directory preserves a development-only laptop frequency-response measurement and five fresh attempts to decode the unchanged Experiment 001 frame. These are negative results. None establishes E3, an over-air MCL link, or an AP-B0 result.

## Fixed authorities

- Experiment 001 source: `../../exp001_e3_source.wav`
- source SHA-256: `1bca567f7f68f5a18f41add8cde03863c336d50f332097f8361262f56eca1241`
- expected canonical Wire bytes: `00 02 00 00 00 01 01 00 00 01 3c`
- detector threshold: `0.4`
- endpoints: built-in Realtek speakers and two-channel Realtek microphone array
- capture format: WASAPI RAW, 48 kHz, stereo, 32-bit float
- final endpoint state: speaker 0 dB and unmuted; microphone 13.9089851 dB and unmuted; no SysFx override

The decoder still required preamble acquisition, a valid frame CRC, exact 11-byte equality, and exact semantic equality. Its threshold and source were not changed.

## Sweep method

`frequency-sweep-source.wav` is a 48 kHz mono PCM16 signal at 0.25 peak amplitude. It contains a 1 kHz synchronization pilot followed by 300 ms tones at 500, 1000, 2000, 3000, 4000, 5000, 6000, 8000, 10000, 12000, 14000, and 16000 Hz, separated by 100 ms silence. Five-millisecond linear ramps limit clicks.

The response tables use a 100 Hz-wide band-pass and a 200 ms center window per tone. A playback offset of 0.64 s was estimated from the pilot. The baseline is a 200 ms pre-playback window. These measurements characterize this laptop path only; geometry and absolute acoustic SPL were not calibrated.

Before the operator reported Nahimic disabled, the measured 5 kHz tone was 14.6 dB below 3 kHz on microphone channel 0 and 20.9 dB below it on channel 1. After the operator reported Nahimic disabled, the corresponding differences were 15.4 dB and 21.7 dB. Disabling Nahimic therefore did not remove the dominant 3/5 kHz imbalance in this run.

The Nahimic state is operator-reported, not programmatically attested. An asynchronous microphone-gain rebound to 20 dB was observed after one capture, so absolute capture level is not a controlled result. The differential frequency response and all raw authorities are retained. The desktop restore script was strengthened to perform a delayed second pass, and a separate delayed query confirmed restoration to 13.9089851 dB.

## Predeclared calibration

For each condition, a single 800 Hz-wide equalizer centered at 5 kHz was derived from the independent sweep before the next fixed-frame capture:

| Condition | Channel 0 gain | Channel 1 gain |
|---|---:|---:|
| first sweep | +14.6 dB | +20.9 dB |
| operator-reported Nahimic disabled | +15.4 dB | +21.7 dB |

Each fixed-frame raw capture was converted into channel 0, channel 1, and the two corresponding calibrated mono PCM16 inputs. The derived WAV files are omitted because they are deterministic products of the retained raw captures. The CSV summaries preserve decoder exit codes and diagnostics.

## Fixed-frame results

| Raw authority | Receiver path | Best correlation | Result |
|---|---|---:|---|
| `fixed-source-raw-full-duplex.wav` | RAW render and RAW capture; raw and first-sweep calibration | 0.006874 | no acquisition |
| `fixed-source-raw-native-player.wav` | preloaded native render and RAW capture; raw and first-sweep calibration | 0.024130 | no acquisition |
| `fixed-source-raw-native-player-nahimic-off.wav` | same native/RAW path after operator-reported Nahimic disable; raw and new calibration | 0.026658 | no acquisition |
| `fixed-source-raw-native-player-nahimic-off-gain20.wav` | same path at a declared 20 dB microphone test gain; raw and new calibration | 0.011938 | no acquisition |
| `fixed-source-raw-operator-boost.wav` | same path after the operator manually boosted the microphone; raw and new calibration | 0.076189 | no acquisition |

All correlations remained below 0.4. No attempt reached header acceptance, frame CRC, exact bytes, or semantic recovery. The 20 dB test gain was temporary; the two-pass restore returned the endpoint to 13.9089851 dB.

## Reproduction transforms

For channel 0 in the Nahimic-disabled condition:

```powershell
ffmpeg -i fixed-source-raw-native-player-nahimic-off.wav `
  -af "pan=mono|c0=c0,equalizer=f=5000:t=h:w=800:g=15.4" `
  -c:a pcm_s16le channel0-calibrated.wav
```

Use `c1` and gain `21.7` for channel 1. Run the unmodified `mcl_ap_exp001_decode_wav` against each derived WAV. Expected result: exit code `3`, status `EXP001_ERR_NO_ACQUISITION`, and no E3 claim.

The next discriminating E3 experiment requires a physically separated COTS speaker/microphone pair. Repeating or tuning against this same-laptop path is not evidence of protocol success.

Data-artifact hashes are recorded in `SHA256SUMS.txt`.
