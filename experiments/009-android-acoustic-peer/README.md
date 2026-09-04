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
