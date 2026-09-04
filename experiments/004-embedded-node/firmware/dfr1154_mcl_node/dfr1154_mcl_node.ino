/*
 * MCL embedded node, DFR1154 (FireBeetle 2 ESP32-S3).
 *
 * Experiment 004. LAB / EXPERIMENTAL.
 *
 * WHAT MAKES THIS DIFFERENT FROM FIRMWARE v2
 *
 * v2 is an instrument. It plays a WAV that was computed on a laptop and
 * embedded in flash, and it records audio for a laptop to decode. No part of
 * MCL runs on the board.
 *
 * This firmware runs the stack. It builds a Tier-0 object with mcl-wire,
 * wraps it in a Link frame with mcl-link, modulates it with the MCL-AP
 * candidate modem, and emits it -- and in the other direction it captures,
 * demodulates, verifies, and decodes back to named field values, all on the
 * microcontroller. The laptop is the other peer, not the brain.
 *
 * That is the point of the exercise. `mcl-core/governance/V1_SCOPE.md` §5.9
 * requirement (c) asks whether the freestanding C99 claim is true: does the
 * stack run where there is no host, no heap and no libc, and can a machine
 * there understand a machine here.
 *
 * It is NOT a second implementation and nothing here claims it is. The board
 * compiles the same source the host compiles, with a different compiler for a
 * different architecture. What that demonstrates is portability and
 * end-to-end operation.
 *
 * SAFETY
 *
 * Flash the APPLICATION PARTITION ONLY, at 0x20000, with esptool. Do not
 * upload through the Arduino CLI: its upload step can also rewrite the
 * bootloader and the partition table. build.ps1 in the parent directory
 * builds without uploading, and refuses to run if the factory backup is
 * missing.
 */

#include <Arduino.h>
#include "ESP_I2S.h"

#include "src/mcl/ap_modem.h"
#include "src/mcl/wire.h"
#include "src/mcl/link.h"

namespace {

constexpr uint32_t kSampleRateHz = 48000;
constexpr gpio_num_t kPdmClockPin = GPIO_NUM_38;
constexpr gpio_num_t kPdmDataPin = GPIO_NUM_39;
constexpr gpio_num_t kActivityLedPin = GPIO_NUM_3;
constexpr uint8_t kAmpBclkPin = 45;
constexpr uint8_t kAmpLrclkPin = 46;
constexpr uint8_t kAmpDataPin = 42;

/*
 * One 72000-sample buffer, 1.5 s, shared between transmit and receive.
 *
 * They never run at once, and two buffers would not fit: this is 144 KB and
 * the receiver's working set is another 77 KB, against roughly 320 KB of
 * usable internal DRAM. The first build of this firmware asked for 2.0 s and
 * the linker refused with `.dram0.bss will not fit in region dram0_0_seg`,
 * which is the honest way to discover a memory budget.
 *
 * PSRAM was the other way out and was not taken. The correlation inner loop
 * reads this buffer tens of millions of times per acquisition, and moving it
 * off-chip trades a link error for a slow receiver -- and it would make the
 * firmware depend on a board option this project has not verified for this
 * part.
 */
constexpr size_t kAudioSamples = 72000;
int16_t g_audio[kAudioSamples];

/* The modem's receive working set. Static, because putting 77 KB on a
   FreeRTOS task stack is not an option. */
mcl_ap_modem_scratch_t g_scratch;

/* Identity of this node. A source_ref is a contact reference for
   correlation, never proof of anything -- mcl-link/spec/link-v0.md. */
constexpr uint32_t kSourceRef = 0x0DFB1154u;

I2SClass microphone;
I2SClass speaker;
bool microphone_ready = false;
bool speaker_ready = false;

void print_hex(const uint8_t *bytes, size_t count) {
  for (size_t i = 0; i < count; ++i) {
    Serial.printf("%02X", bytes[i]);
  }
}

/* ------------------------------------------------------------ MCL objects */

size_t build_presence(uint8_t *out, size_t capacity) {
  mcl_wire_tier0_t object;
  size_t written = 0;

  memset(&object, 0, sizeof(object));
  object.kind = MCL_WIRE_KIND_PRESENCE;
  object.priority = 1u;
  object.source_ref = kSourceRef;
  object.body.presence.capability_tag = 0x000004u;  /* acoustic only */
  object.body.presence.ttl = 60u;

  if (mcl_wire_tier0_encode_at_major(MCL_WIRE_STABLE_MAJOR, &object,
                                     out, capacity, &written) != MCL_WIRE_OK) {
    return 0;
  }
  return written;
}

size_t build_frame(uint8_t *out, size_t capacity,
                   const uint8_t *payload, size_t payload_len,
                   uint16_t sequence) {
  mcl_link_frame_t frame;
  size_t written = 0;

  memset(&frame, 0, sizeof(frame));
  frame.frame_class = MCL_LINK_CLASS_CONTACT;
  frame.flags = MCL_LINK_FLAG_SEQUENCE | MCL_LINK_FLAG_FRAME_CHECK;
  frame.source_ref = kSourceRef;
  frame.sequence = sequence;
  frame.payload = payload;
  frame.payload_len = static_cast<uint16_t>(payload_len);

  if (mcl_link_frame_encode_at_major(MCL_LINK_STABLE_MAJOR, &frame,
                                     out, capacity, &written) != MCL_LINK_OK) {
    return 0;
  }
  return written;
}

/*
 * Report a recovered Tier-0 object by NAMED FIELD VALUES, not as a hex blob.
 *
 * Printing hex would prove the bytes crossed. Printing the fields proves the
 * board read the layout -- which is the distinction that caught a real
 * defect elsewhere in this project, where a wrong field layout consumed
 * exactly the right number of bytes and passed every length check.
 */
void report_object(const uint8_t *bytes, size_t count) {
  mcl_wire_tier0_t object;
  size_t consumed = 0;

  if (mcl_wire_tier0_decode(bytes, count, &object, &consumed) != MCL_WIRE_OK) {
    Serial.println("MCLNODE OBJECT decode=refused");
    return;
  }
  if (object.kind != MCL_WIRE_KIND_PRESENCE) {
    Serial.printf("MCLNODE OBJECT kind=%u (not PRESENCE)\n",
                  static_cast<unsigned>(object.kind));
    return;
  }
  Serial.printf("MCLNODE OBJECT kind=PRESENCE consumed=%u priority=%u "
                "source_ref=%lu capability_tag=%lu ttl=%u\n",
                static_cast<unsigned>(consumed),
                static_cast<unsigned>(object.priority),
                static_cast<unsigned long>(object.source_ref),
                static_cast<unsigned long>(object.body.presence.capability_tag),
                static_cast<unsigned>(object.body.presence.ttl));
}

void report_frame(const uint8_t *bytes, size_t count) {
  mcl_link_frame_t frame;
  size_t consumed = 0;

  if (mcl_link_frame_decode(bytes, count, &frame, &consumed) != MCL_LINK_OK) {
    Serial.println("MCLNODE FRAME decode=refused");
    return;
  }
  Serial.printf("MCLNODE FRAME link_major=%u class=%u flags=0x%02X "
                "source_ref=%lu sequence=%u payload_len=%u consumed=%u\n",
                static_cast<unsigned>(frame.link_major),
                static_cast<unsigned>(frame.frame_class),
                static_cast<unsigned>(frame.flags),
                static_cast<unsigned long>(frame.source_ref),
                static_cast<unsigned>(frame.sequence),
                static_cast<unsigned>(frame.payload_len),
                static_cast<unsigned>(consumed));
  report_object(frame.payload, frame.payload_len);
}

/* ---------------------------------------------------------------- selftest */

/*
 * Encode and decode on the board with no audio at all.
 *
 * If an acoustic run fails, this is what says whether the problem is the
 * channel or the stack. Without it every failure looks the same.
 */
void selftest() {
  uint8_t object[MCL_WIRE_TIER0_MAX_SIZE];
  uint8_t frame[64];
  uint8_t recovered[MCL_AP_MODEM_MAX_PAYLOAD_BYTES];
  mcl_ap_modem_config_t config;
  mcl_ap_modem_rx_t info;
  size_t object_len, frame_len, samples = 0;
  uint32_t started;

  Serial.println("MCLNODE SELFTEST begin");

  object_len = build_presence(object, sizeof(object));
  Serial.printf("MCLNODE SELFTEST wire_encode bytes=%u hex=",
                static_cast<unsigned>(object_len));
  print_hex(object, object_len);
  Serial.println();
  if (object_len != 10u) {
    Serial.println("MCLNODE SELFTEST FAIL wire_encode");
    return;
  }

  frame_len = build_frame(frame, sizeof(frame), object, object_len, 1u);
  Serial.printf("MCLNODE SELFTEST link_encode bytes=%u hex=",
                static_cast<unsigned>(frame_len));
  print_hex(frame, frame_len);
  Serial.println();
  if (frame_len == 0u) {
    Serial.println("MCLNODE SELFTEST FAIL link_encode");
    return;
  }

  mcl_ap_modem_default_config(&config);
  config.trailing_silence_s = 0.10f;   /* as the transmit path uses */
  started = millis();
  if (mcl_ap_modem_encode(&config, frame, frame_len,
                          g_audio, kAudioSamples, &samples)
      != MCL_AP_MODEM_OK) {
    Serial.println("MCLNODE SELFTEST FAIL modulate");
    return;
  }
  Serial.printf("MCLNODE SELFTEST modulate samples=%u ms=%lu\n",
                static_cast<unsigned>(samples),
                static_cast<unsigned long>(millis() - started));

  started = millis();
  mcl_ap_modem_status_t rc = mcl_ap_modem_decode(&config, g_audio, samples,
                                                 &g_scratch, recovered,
                                                 sizeof(recovered), &info);
  Serial.printf("MCLNODE SELFTEST demodulate rc=%ld ms=%lu acquired=%u "
                "corr=%.4f crc_valid=%u bytes=%u\n",
                static_cast<long>(rc),
                static_cast<unsigned long>(millis() - started),
                static_cast<unsigned>(info.acquired),
                static_cast<double>(info.correlation),
                static_cast<unsigned>(info.crc_valid),
                static_cast<unsigned>(info.payload_bytes));

  if (rc != MCL_AP_MODEM_OK || info.payload_bytes != frame_len ||
      memcmp(recovered, frame, frame_len) != 0) {
    Serial.println("MCLNODE SELFTEST FAIL round_trip");
    return;
  }
  report_frame(recovered, info.payload_bytes);
  Serial.println("MCLNODE SELFTEST PASS");
}

/* ------------------------------------------------------------------ send */

void send(bool as_frame, uint16_t sequence) {
  uint8_t object[MCL_WIRE_TIER0_MAX_SIZE];
  uint8_t frame[64];
  const uint8_t *payload;
  size_t payload_len;
  mcl_ap_modem_config_t config;
  size_t samples = 0;

  if (!speaker_ready) {
    Serial.println("MCLNODE ERROR AMP_NOT_READY");
    return;
  }

  payload_len = build_presence(object, sizeof(object));
  if (payload_len == 0u) {
    Serial.println("MCLNODE ERROR WIRE_ENCODE");
    return;
  }
  payload = object;

  if (as_frame) {
    payload_len = build_frame(frame, sizeof(frame), object, payload_len,
                              sequence);
    if (payload_len == 0u) {
      Serial.println("MCLNODE ERROR LINK_ENCODE");
      return;
    }
    payload = frame;
  }

  mcl_ap_modem_default_config(&config);
  /*
   * Trailing silence trimmed from 0.5 s to 0.1 s for transmit only.
   *
   * It is padding, not signal: the receiver locates the payload from the
   * preamble and needs its own capture to extend past the last symbol, not
   * the transmitter's silence. Trimming it is what lets a 24-byte Link frame
   * -- 56320 samples -- share the 72000-sample buffer with the receiver.
   *
   * This is a departure from the Experiment 003 waveform and it is why the
   * results here are not directly comparable to that evidence.
   */
  config.trailing_silence_s = 0.10f;
  if (mcl_ap_modem_encode(&config, payload, payload_len,
                          g_audio, kAudioSamples, &samples)
      != MCL_AP_MODEM_OK) {
    Serial.printf("MCLNODE ERROR MODULATE need=%u have=%u\n",
                  static_cast<unsigned>(
                      mcl_ap_modem_encoded_samples(&config, payload_len)),
                  static_cast<unsigned>(kAudioSamples));
    return;
  }

  Serial.printf("MCLNODE SEND %s bytes=%u samples=%u hex=",
                as_frame ? "FRAME" : "WIRE",
                static_cast<unsigned>(payload_len),
                static_cast<unsigned>(samples));
  print_hex(payload, payload_len);
  Serial.println();
  Serial.println("MCLNODE SEND ARMED");
  Serial.flush();

  /* The same 500 ms guard firmware v2 uses, so the host recorder is running
     before any sound is emitted. */
  delay(500);
  digitalWrite(kActivityLedPin, HIGH);

  const size_t chunk = 512;
  size_t emitted = 0;
  while (emitted < samples) {
    size_t count = (samples - emitted < chunk) ? (samples - emitted) : chunk;
    speaker.write(reinterpret_cast<uint8_t *>(g_audio + emitted),
                  count * sizeof(int16_t));
    emitted += count;
  }
  digitalWrite(kActivityLedPin, LOW);
  Serial.println("MCLNODE SEND DONE");
}

/* ---------------------------------------------------------------- listen */

void listen(bool as_frame, float seconds) {
  uint8_t recovered[MCL_AP_MODEM_MAX_PAYLOAD_BYTES];
  mcl_ap_modem_config_t config;
  mcl_ap_modem_rx_t info;
  size_t wanted;
  uint32_t started;

  if (!microphone_ready) {
    Serial.println("MCLNODE ERROR PDM_NOT_READY");
    return;
  }
  if (seconds <= 0.0f) {
    seconds = 2.0f;
  }
  wanted = static_cast<size_t>(seconds * kSampleRateHz);
  if (wanted > kAudioSamples) {
    wanted = kAudioSamples;
  }

  Serial.printf("MCLNODE LISTEN ARMED %s samples=%u\n",
                as_frame ? "FRAME" : "WIRE",
                static_cast<unsigned>(wanted));
  Serial.flush();
  delay(500);

  digitalWrite(kActivityLedPin, HIGH);
  started = millis();
  size_t got = microphone.readBytes(reinterpret_cast<char *>(g_audio),
                                    wanted * sizeof(int16_t))
               / sizeof(int16_t);
  uint32_t capture_ms = millis() - started;
  digitalWrite(kActivityLedPin, LOW);

  Serial.printf("MCLNODE LISTEN captured=%u ms=%lu\n",
                static_cast<unsigned>(got),
                static_cast<unsigned long>(capture_ms));
  if (got < 24000u) {
    Serial.println("MCLNODE LISTEN FAIL capture_short");
    return;
  }

  mcl_ap_modem_default_config(&config);
  started = millis();
  mcl_ap_modem_status_t rc = mcl_ap_modem_decode(&config, g_audio, got,
                                                 &g_scratch, recovered,
                                                 sizeof(recovered), &info);
  uint32_t decode_ms = millis() - started;

  /*
   * Report acquisition and CRC separately even when the decode failed.
   * "Heard nothing" and "heard something and could not read it" are
   * different problems, and only the second one means a link is close.
   */
  Serial.printf("MCLNODE LISTEN rc=%ld ms=%lu acquired=%u index=%u "
                "corr=%.6f crc_valid=%u rx_crc=%04X calc_crc=%04X bytes=%u\n",
                static_cast<long>(rc),
                static_cast<unsigned long>(decode_ms),
                static_cast<unsigned>(info.acquired),
                static_cast<unsigned>(info.acquisition_index),
                static_cast<double>(info.correlation),
                static_cast<unsigned>(info.crc_valid),
                static_cast<unsigned>(info.received_crc),
                static_cast<unsigned>(info.computed_crc),
                static_cast<unsigned>(info.payload_bytes));

  if (rc != MCL_AP_MODEM_OK) {
    Serial.println("MCLNODE LISTEN NO_RECOVERY");
    return;
  }

  Serial.print("MCLNODE LISTEN payload=");
  print_hex(recovered, info.payload_bytes);
  Serial.println();

  if (as_frame) {
    report_frame(recovered, info.payload_bytes);
  } else {
    report_object(recovered, info.payload_bytes);
  }
  Serial.println("MCLNODE LISTEN RECOVERED");
}

}  // namespace

void setup() {
  pinMode(kActivityLedPin, OUTPUT);
  digitalWrite(kActivityLedPin, LOW);

  Serial.setTxBufferSize(8192);
  Serial.setTxTimeoutMs(500);
  Serial.begin(921600);
  const uint32_t waited = millis();
  while (!Serial && millis() - waited < 5000) {
    delay(10);
  }

  microphone.setPinsPdmRx(kPdmClockPin, kPdmDataPin);
  microphone_ready = microphone.begin(I2S_MODE_PDM_RX, kSampleRateHz,
                                      I2S_DATA_BIT_WIDTH_16BIT,
                                      I2S_SLOT_MODE_MONO);
  if (!microphone_ready) {
    Serial.println("MCLNODE WARN PDM_INIT_FAILED");
  }

  speaker.setPins(kAmpBclkPin, kAmpLrclkPin, kAmpDataPin);
  speaker_ready = speaker.begin(I2S_MODE_STD, kSampleRateHz,
                                I2S_DATA_BIT_WIDTH_16BIT,
                                I2S_SLOT_MODE_MONO);
  if (!speaker_ready) {
    Serial.println("MCLNODE WARN AMP_INIT_FAILED");
  }

  Serial.printf("MCLNODE READY v3 wire_major=%u link_major=%u "
                "audio_buffer=%u scratch=%u free_heap=%lu\n",
                static_cast<unsigned>(MCL_WIRE_STABLE_MAJOR),
                static_cast<unsigned>(MCL_LINK_STABLE_MAJOR),
                static_cast<unsigned>(sizeof(g_audio)),
                static_cast<unsigned>(sizeof(g_scratch)),
                static_cast<unsigned long>(ESP.getFreeHeap()));
}

void loop() {
  if (!Serial.available()) {
    delay(1);
    return;
  }

  String command = Serial.readStringUntil('\n');
  command.trim();

  if (command == "PING") {
    Serial.println("MCLNODE PONG v3");
  } else if (command == "SELFTEST") {
    selftest();
  } else if (command == "SEND FRAME") {
    send(true, 1u);
  } else if (command == "SEND WIRE") {
    send(false, 1u);
  } else if (command.startsWith("LISTEN FRAME")) {
    listen(true, command.substring(12).toFloat());
  } else if (command.startsWith("LISTEN WIRE")) {
    listen(false, command.substring(11).toFloat());
  } else if (command.length() != 0) {
    Serial.println("MCLNODE ERROR UNKNOWN_COMMAND");
  }
}
