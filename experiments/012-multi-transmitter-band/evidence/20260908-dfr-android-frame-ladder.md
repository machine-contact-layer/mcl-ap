# DFR1154 and Android over air: band ladder by frame recovery

**2026-09-08. Not the Experiment 012 spectral sweep. A different measurement,
reported separately so the two are not confused.**

Experiment 012 proper emits a 27-tone ladder and recovers a response curve from
one burst, with the laptop microphone as the analyser. That rig needs the
board's SWEEP firmware, and reflashing this board needs the USB-C port, which
the phone occupies — the machine has one port. So this is the other measurement
available with both machines live: **emit a real object N times at a band and
count recoveries on the peer.**

It measures what the profile ultimately cares about — does a frame arrive — and
it does not produce a response curve. It cannot replace 012 and is not offered
as doing so.

## Rig

DFR1154 on a charger, driven over its own SoftAP; iQOO 9 (Android 14) on USB-C,
driven over adb. Both control planes only; every byte counted here crossed the
air as sound. Phone media volume at the vendor safe maximum (10–11 of 15) — it
cannot be raised further, see the note at the end.

## Board to phone: 7/7 at every candidate band

| band (Hz) | emitted | recovered |
|---|---|---|
| 3000/6000 | 7 | 7 |
| 4800/7800 | 7 | 7 |
| 6000/9000 | 7 | 7 |

Every recovery decoded to a PRESENCE object carrying the board's own
`source_ref`, which changes each run because it is randomised at boot — so
these are seven separate receptions, not one cached result.

**This path is saturated and therefore ranks nothing.** At this distance all
three bands deliver everything. A ceiling is not a measurement of which band is
better, and it is reported as a ceiling.

## Phone to board: marginal, 0–1 per 10, and equally so at every band

| band (Hz) | emitted | recovered |
|---|---|---|
| 3000/6000 | 10 | 1 |
| 4800/7800 | 10 | 1 |
| 6000/9000 | 10 | 1 |

The board's captured peak during these runs is 12 700–14 600 against a measured
silence baseline of **10 578** — two to three decibels of margin. For
comparison, the laptop speaker reaches 18 800 at the same board and recovers
four in a run. The phone is audible to the board and barely more than that.

**No band separates on either path, so there is no evidence here to move
AP-BOOTSTRAP-1 off its 3000/6000 default.** That is the honest conclusion and
it is a weak one: it says the measurement could not discriminate, not that the
bands are equal.

## The three objects

Emitted by the phone and decoded by a receiver sharing no code with it (the
Experiment 011 host tool, laptop microphone):

| object | bytes | acquired | CRC |
|---|---|---|---|
| PRESENCE | 10 | corr 0.473 | BAD |
| TRANSPORT_ACCEPT | 16 | corr 0.481 | ok |
| TRANSPORT_OFFER | 17 | corr 0.596 | ok |

The 16- and 17-byte objects crossed the air and passed CRC on the first
attempt. PRESENCE was then repeated four more times, because one failure is not
a result:

| attempt | acquired | sps estimate | CRC |
|---|---|---|---|
| 1 | corr 0.510 | 159.600 → 159.970 | BAD |
| 2 | corr 0.510 | 159.850 → 159.860 | BAD |
| 3 | corr 0.531 | 160.050 → 160.429 | BAD |
| 4 | corr 0.521 | 160.000 → 160.010 | BAD |

At that point this looked like an anomaly worth flagging: the SHORTEST object
failing where the two LONGER ones passed is backwards for a marginal channel,
which punishes the long frame first. **More data did not support that, and the
retraction is the finding.**

The set was repeated three times per object after the phone was re-seated:

| object | bytes | attempts | CRC ok | correlations |
|---|---|---|---|---|
| PRESENCE | 10 | 3 | **2** | 0.455, 0.441, 0.417 |
| TRANSPORT_ACCEPT | 16 | 3 | **1** | 0.438, 0.420, 0.420 |
| TRANSPORT_OFFER | 17 | 3 | **0** | 0.383 (not acquired), 0.417, 0.408 |

PRESENCE now recovers where OFFER does not — the exact reverse of the first
set. Correlations in the second set are uniformly lower (0.41–0.46 against
0.47–0.60), so the path itself degraded between the two; the acquisition
threshold is 0.40 and one attempt fell below it.

Pooled across both sets: **PRESENCE 2/8, TRANSPORT_ACCEPT 2/4,
TRANSPORT_OFFER 1/4.**

The honest reading is that this path sits close to the acquisition threshold
and CRC success is intermittent for every object size, with **no ordering by
size** in the data. The first set's 0/4 was a run of bad luck on eight trials,
not a mechanism. It is recorded here rather than deleted because the wrong
conclusion was reached first and briefly written down, and a reader who sees
only the corrected table cannot tell how much evidence it took to get there.

What this does establish: all three AP-BOOTSTRAP-1 object sizes, emitted by the
Android bench, have been acquired and CRC-verified over air by a receiver that
shares no code with it. What it does not establish is a recovery *rate* for any
of them — the path is too marginal for these counts to mean much.

`obj=--` in the raw tool output for all three is expected and is not a failure:
the verb compares the payload against an object it generated itself, and the
phone's `source_ref` differs.

## A defect this campaign existed to find, and nearly did not

For the whole first half of this campaign the phone emitted **no sound at all**
while reporting success. `AudioBench.play()` sized its AudioTrack buffer to hold
the entire waveform, so `write(..., WRITE_BLOCKING)` returned as soon as the
samples were queued — about 270 ms for 1.2 s of audio — and a fixed 120 ms
sleep then preceded `stop()` and `release()`. Roughly nine tenths of every
frame was discarded unplayed. AP-BOOTSTRAP-1 opens with 0.1 s of leading
silence followed by a 0.2 s preamble, so **none of the payload reached the
speaker**.

Every symptom pointed elsewhere. The phone's own loopback recovered nothing,
which was misread as the handset's echo canceller. Phone-to-board was 0/10 at
every band, which was misread as a level problem, and the board's captured peak
during those runs was *identical to its silence baseline* — which was the clue,
and was read as "the phone is too quiet" rather than "the phone is silent".

It was settled by recording the phone on the laptop's microphone and looking at
the spectrum rather than at a correlation score: no 6 kHz or 9 kHz energy in any
block, across three emissions, at half a metre. A log line saying
`AUDIO played 57600 samples` is a statement about a buffer, not about a speaker.

Fixed by polling `getPlaybackHeadPosition()` to the end of the waveform before
tearing the track down. After the fix the same emission is acquired at
**correlation 0.819** by the same microphone, and emission wall time went from
268 ms to 1 472 ms for 1.2 s of audio. Every phone-transmit number above was
taken after the fix.

## What is not established

- No band ranking. Both paths were unable to discriminate, for opposite
  reasons: one is saturated, the other is noise-limited.
- The phone-to-board path is marginal at this placement and volume. The
  handset's ROM pins STREAM_MUSIC at 10–11 of 15 and refuses `--set`,
  `--adjust`, DND-off and `audio_safe_volume_state`; the waveform is already at
  0 dBFS, so there is no headroom in software either.
- Board-to-phone was measured with PRESENCE only. The board announces PRESENCE
  in this scenario and has no path that emits TRANSPORT_OFFER or
  TRANSPORT_ACCEPT without a peer, so the 16- and 17-byte objects were measured
  in the phone-to-receiver direction only.
