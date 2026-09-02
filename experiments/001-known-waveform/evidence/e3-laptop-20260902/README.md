# Experiment 001 laptop E3 attempt — 2026-09-02

Status: **E3 ATTEMPT RUN — EXACT RECOVERY NOT ACHIEVED**

This directory retains the first controlled attempt to transmit the fixed Experiment 001 waveform through the built-in speaker and microphone array of one laptop. It is negative evidence. It does not establish an over-air MCL success claim.

## Fixed source and receiver

- source: `../../exp001_e3_source.wav`
- source SHA-256: `1bca567f7f68f5a18f41add8cde03863c336d50f332097f8361262f56eca1241`
- source format: 48 kHz, mono, PCM16, 1.226667 s
- expected canonical Wire bytes: `00 02 00 00 00 01 01 00 00 01 3c`
- detector threshold: `0.4`
- speaker endpoint: `Speakers (Realtek(R) Audio)`
- capture endpoint: `Microphone Array (Realtek(R) Audio)`
- endpoint state after testing: speaker 0 dB and unmuted; microphone 15.92 dB and unmuted; no SysFx override
- geometry: built-in speaker and microphone on the same laptop; distance and room response were not calibrated

The decoder required all three gates: valid frame CRC, exact 11-byte equality with the fixed source vector, and exact semantic-object equality.

## Retained attempts

| Artifact | Capture path | Result |
|---|---|---|
| `capture-raw.wav` | DirectShow microphone; SDL/FFplay render | No acquisition; best correlation `0.058468` |
| `capture-native-player.wav` | DirectShow microphone; Windows native render | No acquisition; best correlation `0.040383` |
| `control-mic-silence.wav` | 1.2 s tail from the native-player capture | Negative control; best correlation `0.061283` |
| `capture-sysfx-override-not-applied.wav` | DirectShow after an attempted property-store override | Override did not persist; no acquisition; best correlation `0.060207` |
| `capture-openal.wav` | OpenAL microphone; Windows native render | No acquisition; best correlation `0.039375` |
| `capture-wasapi-raw-native.wav` | WASAPI RAW microphone; Windows native render | Two-channel float raw authority; playback began outside the 0.5 s acquisition window |
| `capture-wasapi-raw-native-preloaded.wav` | WASAPI RAW microphone; preloaded Windows native render | Two-channel float raw authority; chirp and symbol energy retained |
| `capture-wasapi-raw-aligned-450ms-ch1-s16.wav` | Derived from the preceding raw capture | Preamble acquired at correlation `0.624806`; decoded header was invalid (`payload_length=0`, received CRC `0x0010`); exact recovery failed |
| `capture-wasapi-raw-full-duplex.wav` | WASAPI RAW render and RAW capture | Two-channel float raw authority; no acquisition after channel extraction (`0.152388` and `0.141916`) |

`capture-sysfx-override-not-applied.wav` is deliberately named to prevent a false claim: Windows reported the master effects property as absent both before and after the attempted write, so this was not an effects-disabled measurement.

## Reproduce the strongest decode attempt

Create the retained channel-1 aligned input from the immutable raw capture:

```powershell
ffmpeg -ss 0.450 `
  -i capture-wasapi-raw-native-preloaded.wav `
  -af "pan=mono|c0=c1" -ar 48000 -c:a pcm_s16le `
  capture-wasapi-raw-aligned-450ms-ch1-s16.wav
```

Then run the Experiment 001 decoder without changing its waveform or threshold:

```powershell
mcl_ap_exp001_decode_wav.exe capture-wasapi-raw-aligned-450ms-ch1-s16.wav
```

Expected result for this retained capture: exit code `3`, Experiment 001 status `7` (`EXP001_ERR_PAYLOAD_TOO_LARGE`), correlation `0.624806`, decoded payload length `0`, and no E3 claim.

## Interpretation

The standard shared capture paths strongly suppressed same-laptop playback. WASAPI RAW preserved the LFM chirp and raised acquisition above threshold on one physical microphone channel, proving the speaker-to-air-to-microphone path was observable. The 3/5 kHz FSK header and payload were not recovered exactly. The completed development-only frequency-response follow-up is retained in `../calibration-followup-20260902/`; it did not recover the fixed frame. The next discriminating physical experiment requires a separated speaker/microphone device pair. The fixed source and this negative result must remain unchanged.

All hashes are recorded in `SHA256SUMS.txt`.
