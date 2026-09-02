#include <Arduino.h>
#include "ESP_I2S.h"
#include "exp003_pcm.h"

namespace {

constexpr uint32_t kSampleRateHz = 48000;
constexpr uint32_t kCaptureSeconds = 3;
constexpr gpio_num_t kPdmClockPin = GPIO_NUM_38;
constexpr gpio_num_t kPdmDataPin = GPIO_NUM_39;
constexpr gpio_num_t kActivityLedPin = GPIO_NUM_3;
constexpr size_t kSerialChunkBytes = 64;
constexpr size_t kUsbDrainGuardBytes = 256;

// MAX98357 I2S amplifier driving the onboard speaker. Pin assignment comes
// from the vendor "Recording & Playback" example.
constexpr uint8_t kAmpBclkPin = 45;
constexpr uint8_t kAmpLrclkPin = 46;
constexpr uint8_t kAmpDataPin = 42;
constexpr size_t kPlayChunkSamples = 512;

I2SClass microphone;
I2SClass speaker;
bool microphone_ready = false;
bool speaker_ready = false;

uint32_t crc32_ieee(const uint8_t *data, size_t size) {
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < size; ++i) {
    crc ^= data[i];
    for (uint8_t bit = 0; bit < 8; ++bit) {
      const uint32_t mask = 0u - (crc & 1u);
      crc = (crc >> 1u) ^ (0xEDB88320u & mask);
    }
  }
  return ~crc;
}

bool write_all(const uint8_t *data, size_t size) {
  size_t written = 0;
  while (written < size) {
    const size_t remaining = size - written;
    const size_t chunk = remaining < kSerialChunkBytes ? remaining : kSerialChunkBytes;
    const size_t result = Serial.write(data + written, chunk);
    if (result == 0) {
      delay(1);
      continue;
    }
    written += result;
    if ((written % 1024u) == 0u) {
      delay(1);
    }
  }
  return true;
}

void capture_once() {
  if (!microphone_ready) {
    Serial.println("MCLERROR PDM_NOT_READY");
    return;
  }

  Serial.printf("MCLCAPTURE ARMED %lu %lu\n",
                static_cast<unsigned long>(kSampleRateHz),
                static_cast<unsigned long>(kCaptureSeconds));
  Serial.flush();

  // The host starts playback 650 ms after ARMED. This makes recording begin
  // first while leaving enough time for USB CDC and the laptop player.
  delay(500);
  digitalWrite(kActivityLedPin, HIGH);

  size_t wav_size = 0;
  uint8_t *wav = microphone.recordWAV(kCaptureSeconds, &wav_size);

  digitalWrite(kActivityLedPin, LOW);
  if (wav == nullptr || wav_size < 44 || wav_size > 400000) {
    Serial.printf("MCLERROR CAPTURE_FAILED %u\n", static_cast<unsigned>(wav_size));
    free(wav);
    return;
  }

  const uint32_t wav_crc32 = crc32_ieee(wav, wav_size);
  Serial.printf("MCLWAV %u %08lX\n",
                static_cast<unsigned>(wav_size),
                static_cast<unsigned long>(wav_crc32));
  delay(100);
  write_all(wav, wav_size);
  delay(500);
  Serial.print("\nMCLEND\n");
  // Native USB CDC can retain its final short packet until more data arrives.
  // Bytes after MCLEND are outside the frame and force that packet to drain.
  for (size_t i = 0; i < kUsbDrainGuardBytes; ++i) {
    Serial.write(static_cast<uint8_t>('\n'));
  }
  delay(500);
  free(wav);
}

// Transmit the embedded Experiment 003 candidate frame through the onboard
// speaker. This makes the board a transmitter as well as a receiver, so a
// second device pair and the reverse direction can be measured.
void play_once() {
  if (!speaker_ready) {
    Serial.println("MCLERROR AMP_NOT_READY");
    return;
  }

  Serial.printf("MCLPLAY ARMED %lu %lu\n",
                static_cast<unsigned long>(EXP003_PCM_SAMPLE_RATE),
                static_cast<unsigned long>(EXP003_PCM_SAMPLE_COUNT));
  Serial.flush();

  // Give the host recorder time to be running before any sound is emitted.
  delay(500);
  digitalWrite(kActivityLedPin, HIGH);

  static int16_t chunk[kPlayChunkSamples];
  size_t emitted = 0;
  while (emitted < EXP003_PCM_SAMPLE_COUNT) {
    const size_t remaining = EXP003_PCM_SAMPLE_COUNT - emitted;
    const size_t count = remaining < kPlayChunkSamples ? remaining : kPlayChunkSamples;
    for (size_t i = 0; i < count; ++i) {
      chunk[i] = static_cast<int16_t>(pgm_read_word(&kExp003Pcm[emitted + i]));
    }
    speaker.write(reinterpret_cast<uint8_t *>(chunk),
                  count * sizeof(int16_t));
    emitted += count;
  }

  digitalWrite(kActivityLedPin, LOW);
  Serial.println("MCLPLAYEND");
}

}  // namespace

void setup() {
  pinMode(kActivityLedPin, OUTPUT);
  digitalWrite(kActivityLedPin, LOW);

  Serial.setTxBufferSize(8192);
  Serial.setTxTimeoutMs(500);
  Serial.begin(921600);
  const uint32_t wait_started_ms = millis();
  while (!Serial && millis() - wait_started_ms < 5000) {
    delay(10);
  }

  microphone.setPinsPdmRx(kPdmClockPin, kPdmDataPin);
  microphone_ready = microphone.begin(I2S_MODE_PDM_RX,
                                      kSampleRateHz,
                                      I2S_DATA_BIT_WIDTH_16BIT,
                                      I2S_SLOT_MODE_MONO);
  if (!microphone_ready) {
    Serial.println("MCLERROR PDM_INIT_FAILED");
    return;
  }

  speaker.setPins(kAmpBclkPin, kAmpLrclkPin, kAmpDataPin);
  speaker_ready = speaker.begin(I2S_MODE_STD,
                                kSampleRateHz,
                                I2S_DATA_BIT_WIDTH_16BIT,
                                I2S_SLOT_MODE_MONO);
  if (!speaker_ready) {
    Serial.println("MCLWARN AMP_INIT_FAILED");
  }

  Serial.println("MCL_USB_CAPTURE_READY v2 PDM=38,39 AMP=45,46,42 RATE=48000 CHANNELS=1 BITS=16");
}

void loop() {
  if (!Serial.available()) {
    delay(1);
    return;
  }

  String command = Serial.readStringUntil('\n');
  command.trim();
  if (command == "CAPTURE") {
    capture_once();
  } else if (command == "PLAY") {
    play_once();
  } else if (command == "PING") {
    Serial.println("MCLPONG v1");
  } else if (command.length() != 0) {
    Serial.println("MCLERROR UNKNOWN_COMMAND");
  }
}
