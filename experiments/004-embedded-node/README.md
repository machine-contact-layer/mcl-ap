# MCL-AP Experiment 004: the embedded node

**Status**: LAB / EXPERIMENTAL. Not AP-B0, not a selected profile, nothing here
freezes spectrum.

**Evidence level**: E4 — multi-device over-air, both directions.

## What is new here, and why it is not another E4 run

Experiments 001–003 measured a **waveform**. The board was an instrument: it
played a WAV computed on a laptop and embedded in flash, and it recorded audio
for a laptop to decode. No part of MCL ran on the microcontroller.

This experiment runs the **stack** on the board.

```text
                    experiment 003              experiment 004
  object built by   laptop                      the board, with mcl-wire
  frame built by    nothing (raw Wire bytes)    the board, with mcl-link
  modulated by      laptop, offline             the board, at 240 MHz
  demodulated by    laptop                      the board
  decoded by        laptop                      the board, to field values
```

In the `host-to-board` direction there is no host in the loop at all. The
laptop emits sound; the ESP32-S3 acquires, demodulates, verifies a CRC, decodes
a Link frame, decodes the Tier-0 object inside it, and reports the field values
it read. That is `V1_SCOPE.md` §5.9 requirement (c), and it is the smallest
honest demonstration of what MCL claims: a machine here is understood by a
machine there, with no shared runtime between them.

**It is not a second implementation and this document does not call it one.**
The board compiles the same three source files the host compiles —
`ap_modem.c`, `wire.c`, `link.c` — with a different compiler for a different
architecture. What that demonstrates is **portability and end-to-end
operation**: that the freestanding C99 claim is true, that the stack runs with
no heap and no libc, and that both ends agree about bytes when the bytes have
crossed a room instead of a function call.

## Result, 2026-09-04

One session, one room, one geometry, one operator. Both peers running the same
stack. Ten trials per cell.

| Direction | Payload | Acquired | Exact recovery |
|---|---|--:|--:|
| board → host | 24-byte Link frame | 10/10 | **2/10** |
| board → host | 10-byte Wire object | 10/10 | **9/10** |
| host → board | 10-byte Wire object | 10/10 | **6/10** |
| host → board | 24-byte Link frame | 10/10 | **7/10** |

`host → board` recovery is decoded **on the microcontroller**. Nothing in those
two rows was decoded by a laptop.

Acquisition and recovery are counted separately throughout. "Heard nothing" and
"heard something and could not read it" are different problems with the same
shape, and only the second means a link is nearly working. Every cell above
acquired 10/10, so every failure in this session is a bit error in the payload,
never a missed preamble.

### The two directions are not the same channel

`board → host` loses 9/10 to 2/10 when the payload grows from 10 bytes to 24 —
2.2× the symbols at the same acquisition quality, so bit errors dominate.
`host → board` does **not** show that: 6/10 at 10 bytes and 7/10 at 24. On that
path the payload length is not the limiting factor.

The architecture has always held that A→B and B→A are different channels and
must be measured separately. This is a measurement where they behave
differently in kind, not only in degree, and it is consistent with Experiment
002's finding that the weak element is the laptop speaker rather than either
microphone.

### The host audio stack is part of the channel

The Nahimic audio processing driver was disabled partway through this session,
between the two-trial pilot and the full runs. The pilot recovered 2/2 at
correlations of 0.92 and 0.89; the full `board → host` frame run that followed
recovered 2/10 at 0.55–0.88.

That is one uncontrolled change and it is reported as an observation rather
than a result — there is no controlled pair here and none was run. But it
points at something the architecture should say out loud:

> **A consumer audio path is designed for human listeners and is not a neutral
> medium for machine signalling.** Enhancement, automatic gain, noise
> suppression and beam-forming all operate on the assumption that the signal is
> speech. They are present on essentially every laptop and phone, they differ
> per vendor, and they can be reconfigured by software the deployment does not
> control.

For MCL-AP this is not an inconvenience to be tuned away. It is a reason the
architecture selects a profile from a **measurement of the actual path**
instead of assuming one — and the path includes the driver stack, not only the
air.

## What this does NOT establish

- **AP-B0 is still NOT selected.** One room, one geometry, one operator, one
  session, two devices.
- Recovery rates of 2/10 to 9/10 are **not a usable link**. No claim about
  range, motion, multipath, or multiple nodes contending follows from this.
- The board is not a second implementation. Same source, different toolchain
  and architecture. `V1_SCOPE.md` §5.9 states what that is worth and what it is
  not.
- The 3000/6000 Hz pair remains tuned to a path measured in Experiment 002.
  Nothing here promotes it.
- Trailing silence is 0.10 s here rather than the 0.5 s of Experiment 003, so
  these numbers are **not** directly comparable to that evidence. The frame had
  to share a 72000-sample buffer with the receiver's working set.

## The rig

| | |
|---|---|
| Board | DFR1154 FireBeetle 2 ESP32-S3, COM3, VID 303A / PID 1001 |
| Board firmware | `firmware/dfr1154_mcl_node`, flashed to the app partition at `0x20000` only |
| Board microphone | onboard PDM, GPIO 38 clock / 39 data, 48 kHz PCM16 mono |
| Board speaker | MAX98357 on BCLK 45 / LRCLK 46 / DIN 42 |
| Host | laptop Realtek microphone array via ffmpeg dshow; laptop speaker via `SoundPlayer` |
| Host audio processing | Nahimic disabled for the full runs |
| Waveform | Experiment 003 candidate, FSK 3000/6000 Hz, 300 baud, 0.2 s LFM 2–6 kHz preamble |
| Wire major | 1 |
| Link major | 1 |

### On-board cost

| | |
|---|---|
| Modulate a 24-byte frame | 674 ms |
| Demodulate and decode a 1.5 s capture | 4744 ms |
| Audio buffer | 144 000 bytes, shared between transmit and receive |
| Receiver working set | 76 868 bytes |
| Free heap after init | 110 576 bytes |

The first working build decoded in **20 858 ms**. Acquisition was computing
each candidate window's mean in a separate pass over the same samples; the
signal mean cancels out of the correlation numerator, and the sums it is still
needed for slide from one window to the next. Removing that pass took the board
to 4744 ms and the host suite from 24.1 s to 10.1 s, **with every archived
capture still decoding to the same result** — the optimisation had to be proved
not to have changed any decision, and that is what proved it.

## Reproducing

```powershell
# build (never uploads; refuses without the verified factory backup)
firmware\build.ps1

# flash the application partition only, at 0x20000, with esptool
firmware\flash-app-only.ps1

# talk to it
.\node.ps1 PING
.\node.ps1 SELFTEST          # encode and decode on-board, no audio at all

# trials
.\run-node-trials.ps1 -Direction board-to-host -Payload wire  -Trials 10
.\run-node-trials.ps1 -Direction host-to-board -Payload frame -Trials 10
```

`SELFTEST` exists so that a failed acoustic run can be attributed. It builds
the object, builds the frame, modulates, demodulates and decodes entirely
on-chip with no sound involved. If it passes and an over-air run fails, the
problem is the channel; if it fails, nothing about the channel has been
measured.

**Restore the board afterwards** with
`MCL_DFR1154_BACKUP_20260902\RESTORE_DFR1154_APP.cmd`.

## What the runs found before they found anything acoustic

Three defects in this harness produced results that looked like acoustic
failures and were not. They are recorded because each one would have been
reported as a measurement:

1. **0/2 recovered, board → host.** `Start-Process` joins an argument array
   without quoting, so the device name — which contains spaces and parentheses
   — reached ffmpeg as several arguments and no file was written. The decoder
   was reading files that did not exist.
2. **0/2 acquired, host → board.** The reader stopped at the literal
   `MCLNODE LISTEN `, which the board's *progress* line matches, so every trial
   was scored while the board was still decoding.
3. **0/2 recovered at 0.56–0.60 correlation, host → board.** `SoundPlayer`
   takes 490–570 ms to start the device after `Play()` returns. The frame
   therefore landed 0.72 s into a 1.5 s capture and its payload ran off the end
   of the buffer — `ERR_SYNC`, not a bit error. Separate acquisition and
   recovery counters are what made this visible: 100% acquisition with 0%
   recovery is not what a bad room looks like.

The board also stops answering after the host closes and reopens the serial
port — measured, three DTR/RTS combinations, all silent — so every run resets
it first and starts from the same state.
