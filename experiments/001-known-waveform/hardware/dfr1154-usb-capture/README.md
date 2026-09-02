# DFR1154 USB capture instrument

This experimental instrument records MCL-AP E3 through the DFR1154 board's
onboard PDM microphone and returns a PCM16 mono WAV over native USB CDC. It does
not initialize Wi-Fi or the board speaker.

Hardware authority comes from the vendor recording example:

- ESP32-S3 PDM clock: GPIO 38
- ESP32-S3 PDM data: GPIO 39
- capture format: 48 kHz, signed 16-bit, mono
- USB transport: hardware CDC/JTAG on COM3

The sketch accepts `PING` and `CAPTURE`. A capture is three seconds long. The
The host preloads and starts the immutable `exp001_e3_source.wav` immediately
after the board announces its fixed 500 ms pre-capture interval. This accounts
for Windows playback-device startup latency while keeping the chirp inside the
decoder's fixed first-0.5-second acquisition window. The host saves the returned
WAV without overwriting an existing artifact and always runs the laptop audio
restoration script.
The firmware sends 64-byte chunks through an enlarged native CDC transmit ring,
throttles producer bursts, and emits a post-frame USB drain guard. The host
accepts a WAV only when the board-reported IEEE CRC-32 and `MCLEND` frame both
verify; guard bytes are outside the length-delimited frame.

The board must be backed up before flashing. Only the existing factory
application partition at `0x20000` may be replaced. Do not upload through the
Arduino CLI because that can also replace the bootloader or partition table.
The original application is restored with the separately retained
`RESTORE_DFR1154_APP.cmd` after testing.


## Firmware v2: transmit as well as capture

Firmware v2 keeps `CAPTURE` and `PING` unchanged and adds `PLAY`, which emits the Experiment
003 candidate frame through the onboard MAX98357 amplifier (BCLK 45, LRCLK 46, DIN 42). The
frame is embedded in flash as `exp003_pcm.h`, generated from `exp003_source.wav`, and is
streamed from PROGMEM in 512-sample chunks so no large runtime allocation is needed. As with
capture, the board guards 500 ms after announcing `MCLPLAY ARMED` so the host recorder is
already running before any sound is emitted.

This makes the board a transmitter as well as a receiver, which is what allows the reverse
direction and a second device pair to be measured (E4).

Flash the app partition only, exactly as the restore script does:

```
esptool --chip esp32s3 --port COM3 --baud 921600 write-flash 0x20000 dfr1154_usb_capture.ino.bin
```

Do not upload through the Arduino CLI, which can also replace the bootloader and partition
table. Restore the factory application afterwards with `RESTORE_DFR1154_APP.cmd`.
