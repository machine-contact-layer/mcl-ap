# Experiment 009 — MCL-AP from an Android handset

**Question.** Does the MCL-AP candidate waveform survive a consumer phone's
loudspeaker, and does a laptop running the reference modem recover MCL objects
from it?

**Answer.** It survives acquisition every time and recovers sometimes. Ten
trials per payload, one session, 2026-09-05:

| payload | acquired | recovered |
|---|--:|--:|
| 10-byte Wire object (`PRESENCE`) | 10/10 | **4/10** |
| 24-byte Link frame carrying it | 10/10 | **1/10** |

Correlation at acquisition ran 0.73–0.87 in every trial, so the preamble was
never marginal. Every failure is a payload bit error, not a missed frame.

The recoveries decode all the way down:

```text
FRAME  link_major=1 class=0 flags=0x14 source_ref=0A11EDC0 sequence=1 payload_len=10
OBJECT kind=PRESENCE priority=1 source_ref=0A11EDC0 capability_tag=4 ttl=60
```

A **major-1 Link frame carrying a major-1 `PRESENCE`** — the bytes v1.0 freezes
— crossed a room from a phone speaker and was read back field by field.

## The rig

```text
iQOO 9 (vivo I2017)            air            Windows laptop
Android 14, API 34, SM8350   -------->        Realtek microphone array
loudspeaker, media index 11/15                48 kHz mono capture
                                              mcl_ap_node decode-{wire,frame}
```

Waveform: FSK 3000/6000 Hz, LFM preamble chirp 2000→6000 Hz, 300 baud, 48 kHz —
the Experiment 003 band, chosen by measurement because the 4200–5000 Hz notch
had already cost an earlier attempt everything.

The phone plays a WAV that `mcl_ap_node` modulated; the laptop demodulates with
the same `ap_modem.c` the DFR1154 runs. No protocol code is duplicated here.

## One direction only, and the reason is Android

**The phone emits. It does not receive.** Capture on Android requires
`RECORD_AUDIO`, which belongs to an application: `/dev/snd` is `system:audio`
and the adb shell user is not in the `audio` group, so no shell binary can open
the microphone. Recording from the phone needs an APK, and an APK is a larger
claim than this experiment makes.

That limit is Android's permission model, not the modem. It is written here
rather than left for a reader to infer from a missing table row.

## Three ways to record silence and call it a channel

Each of these produced captures indistinguishable from a dead room, and each is
now handled by the runner:

1. **A bare `VIEW` intent raises a player-chooser dialog.** It waits for a tap.
   The runner launches an explicit component (`-n`) so no chooser can appear.
2. **Re-launching the same activity plays nothing** — `am` reports "intent
   delivered to currently running top-most instance" and returns success. Each
   trial force-stops the player first.
3. **With the screen off, nothing plays at all.** The activity never reaches
   the foreground. Each trial wakes the device, and the runner holds the screen
   on for the duration.

All three look identical in the results table: `acquired=0`, correlation ~0.04.
The run log therefore records the **peak level of every capture** next to its
decode. A capture with no signal in it is an instrument fault; a capture at
−2 dB that fails to recover is a channel result. Separating those two is the
whole reason this experiment is trustworthy, and it is the same lesson
Experiment 008 recorded after three harness defects produced numbers that looked
acoustic and were not.

## Level, and why not louder

Media volume index 11 of 15. The waveform is a continuous 2–6 kHz tone burst,
which is not what a phone microspeaker is voiced for, and nothing here needs the
last two steps of level. The runner refuses `-Volume` above 13, sets the level
it was asked for, and restores whatever it found.

## What this does NOT establish

- **Not a usable acoustic link.** 1/10 and 4/10 are not a link, and the release
  does not claim one. MCL-AP stays **Experimental** and AP-B0 stays unselected.
- **Not an Android acoustic binding.** There is no Android AP implementation.
  The phone is a loudspeaker with a file on it.
- **Not independent interoperability.** The waveform was produced by the same
  modem that decoded it. See `mcl-core/conformance/independent/`.
- **Not a characterisation of the phone's speaker.** One device, one position,
  one room, one session. Experiment 002 is what a path characterisation looks
  like, and this is not one.
- **Nothing about the phone's microphone**, which was never opened.
- **Nothing about what the phone's OUTPUT path did to the waveform.** See the
  next section. This one matters more than it looks, because Experiment 012
  wants to use this phone as a third transmitter class.

## The Android emission path, and what a phone curve actually measures

Experiment 012 needs a third independent transmitter class and this handset is
the candidate. Before a curve from it counts as a transmitter measurement, what
sits between the file and the air has to be written down, because most of it
cannot be observed or disabled from a shell.

**How the sound is actually produced here.** A WAV on the device, opened by an
explicit component (`com.google.android.apps.nbu.files/.gateway.preview.PreviewActivity`)
— so it goes out on the **media** path, `STREAM_MUSIC` / `USAGE_MEDIA`, with
whatever that path applies on this OEM's build. It is not an application this
project wrote, and it does not open an `AudioTrack` with stated attributes.

**What that path may be doing, unobserved:**

| stage | why the rig cannot see or control it |
|---|---|
| stream volume curve | volume is set by `input keyevent KEYCODE_VOLUME_UP` because `cmd media_session volume --set` reports success and does nothing. The rig can move the index; it cannot read the resulting gain, and the curve is not linear in the index. |
| OEM audio effects on `STREAM_MUSIC` | vivo builds ship effect chains on the media path. Nothing in a shell disables them for another app's playback. |
| loudness enhancement / dynamic range compression | frequency- **and level**-dependent by design |
| speaker protection and excursion limiting | deliberately clamps low frequencies at high output, which is exactly the region a band sweep is asking about |

**Why this is not covered by the SNR normalisation.** Experiment 012 scores on
SNR against each path's own room floor, and that cancels a constant **capture**
gain exactly — tone and room are multiplied alike. It does **not** cancel
anything on the transmit side that varies with frequency, because that changes
the tone and not the room. A compressor or a protection filter in the phone's
output path therefore appears in the curve as if it were the speaker's
response, and the minimax would select a band against it as though it were
physics.

**The falsification test, which has not been run.** Emit the same ladder at two
different volume indices and compare the curve *shapes* after normalising each
to its own level. A speaker's response shape does not change with drive level
over a modest range; a compressor's does. If the shapes differ, the phone curve
is a measurement of the phone's software and not of its transmitter, and it
must not be used as an independent transmitter class.

That test costs **two emissions** and no acoustic budget is authorised, so it
has not been done and no phone curve exists in Experiment 012.

**The receiver side of the same question cannot be asked at all on this rig.**
Whether the phone's `AudioRecord` path uses `UNPROCESSED` or
`VOICE_COMMUNICATION`, and whether AGC, noise suppression and echo cancellation
are engaged, decides whether a phone can be an MCL-AP *receiver* — and every
one of those is an application-level choice that needs an APK holding
`RECORD_AUDIO`. `/dev/snd` is `system:audio` mode 660 and the shell is not in
the `audio` group. Until an APK exists, this project has **no evidence
whatsoever** about Android as an acoustic receiver, and the release claims
none.

## Why the frame does worse than the object

The same 2.2× payload growth that collapsed the DFR1154's board→host direction
in Experiment 008 (9/10 at 10 bytes, 2/10 at 24) does the same thing here:
4/10 and 1/10. Two independent transmitters, two different loudspeakers, the
same shape of degradation — the payload length is doing this, not the device.

## Running it

```powershell
ffmpeg -list_devices true -f dshow -i dummy     # find the microphone name
.\run-android-acoustic.ps1 -Trials 10 `
    -Mic "<microphone>" `
    -Node <path>\mcl_ap_node.exe
```

The phone must be unlocked, connected over USB, and near the microphone. The
script pushes the waveforms, wakes the screen, drives the volume, runs the
trials, restores the volume and writes `evidence/` with a checksum for every
file.

## What these captures then fixed in the receiver

The failed trials were not thrown away. `mcl_ap_node` now prints the demodulated
bytes even when they fail the CRC — under the key `unverified=`, because nothing
has checked them — and comparing those against the known transmitted payload
turns a verdict into a measurement.

Across the six failed `wire` trials, 29 payload bits were wrong. **28 of them
were a transmitted 0 read as a 1**: 97% in one direction. That is not noise. A
decision threshold placed correctly produces errors in both directions roughly
equally; one that sits too close to one class produces exactly this.

The cause is that the two tones do not arrive equally. Experiment 002 measured
6 kHz sitting at or above the 3 kHz reference on both receivers it tested, and a
louder tone is also a tighter distribution — so the midpoint of the two class
means, which is where the threshold was, is not the point at which the two
classes are equally likely. It sits nearer the noisier one, and that class's
tail crosses it.

The threshold is now placed by **margin**: halfway between the worst 0 and the
worst 1 in the training sequence, falling back to the midpoint of means when the
training itself does not separate. The 0x55 training byte is what makes this
possible — eight of each symbol, known in advance, at the head of every frame.

Re-decoding **every archived capture in this repository**, with no
retransmission and no change to what goes on the wire:

| capture set | before | after |
|---|--:|--:|
| 008 board→host, 24-byte frame | 2/10 | **3/10** |
| 008 board→host, 10-byte object | 9/10 | 9/10 |
| 009 handset, 10-byte object | 4/10 | **5/10** |
| 009 handset, 24-byte frame | 1/10 | **2/10** |

Three cells improved and none regressed. Acquisition stayed 10/10 everywhere,
which it must: the change is downstream of acquisition entirely.

**The archived evidence above is not rewritten.** The numbers in
`008-embedded-node/README.md` and in the table at the top of this file are what
those sessions measured, under the decision rule in force when they ran. This
table is a separate measurement — the same recordings, a different receiver —
and it is reported as one.

It also does not make MCL-AP a usable link. 3/10 is not 2/10 and it is not a
link either. What it is, is a receiver-side gain that costs no bytes, no
airtime and no transmitter change, found by looking at which way the bits fell
rather than by trying parameters.
