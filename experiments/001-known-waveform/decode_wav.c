/*
 * MCL-AP Experiment 001: Standalone Offline WAV Decoder
 *
 * Usage:
 *   decode_wav <capture.wav>
 *
 * Reads a 48 kHz 16-bit mono WAV file, performs preamble acquisition,
 * Goertzel FSK demodulation, CRC-16 verification, and feeds the recovered
 * bytes into mcl_wire_tier0_decode.
 *
 * Used for the E3 over-the-air validation path:
 *   speaker -> air -> microphone -> capture.wav -> decode_wav
 */

#include "exp001.h"
#include "mcl/wire.h"
#include <stdio.h>
#include <string.h>

#define MAX_SAMPLES 960000u
static float g_samples[MAX_SAMPLES];

static const char *kind_str(mcl_wire_kind_t k)
{
    switch (k) {
    case MCL_WIRE_KIND_PRESENCE: return "PRESENCE";
    case MCL_WIRE_KIND_HAZARD: return "HAZARD";
    case MCL_WIRE_KIND_REQUEST: return "REQUEST";
    case MCL_WIRE_KIND_AUTHORITY_CLAIM: return "AUTHORITY_CLAIM";
    case MCL_WIRE_KIND_DEGRADED_STATE: return "DEGRADED_STATE";
    case MCL_WIRE_KIND_TRANSPORT_OFFER: return "TRANSPORT_OFFER";
    default: return "UNKNOWN";
    }
}

int main(int argc, char **argv)
{
    const char *wav_path;
    size_t num_samples = 0u;
    uint32_t sample_rate = 0u;
    uint16_t bits_per_sample = 0u;
    uint16_t channels = 0u;
    exp001_status_t est;
    exp001_frame_config_t fconfig;
    exp001_decode_result_t decode_res;
    uint8_t recovered_bytes[EXP001_MAX_PAYLOAD_BYTES];
    mcl_wire_tier0_t obj;
    size_t wire_consumed = 0u;
    mcl_wire_status_t wst;
    size_t i;

    printf("MCL-AP Experiment 001: Offline WAV Decoder\n");
    printf("==========================================\n");

    if (argc < 2) {
        fprintf(stderr, "Usage: %s <wav-file>\n", argv[0]);
        return 1;
    }
    wav_path = argv[1];

    printf("Reading WAV: %s\n", wav_path);
    est = exp001_wav_read(wav_path, g_samples, MAX_SAMPLES,
                          &num_samples, &sample_rate, &bits_per_sample, &channels);
    if (est != EXP001_OK) {
        fprintf(stderr, "Error: Failed to read WAV file (code %d)\n", (int)est);
        return 2;
    }

    printf("  Sample rate: %u Hz\n", sample_rate);
    printf("  Channels: %u\n", (unsigned)channels);
    printf("  Bits/sample: %u\n", (unsigned)bits_per_sample);
    printf("  Samples: %zu (%.3f s)\n", num_samples, (double)num_samples / (double)sample_rate);

    if (sample_rate != EXP001_SAMPLE_RATE) {
        fprintf(stderr, "Error: Unsupported sample rate %u Hz (required: %u Hz)\n",
                sample_rate, EXP001_SAMPLE_RATE);
        return 3;
    }
    if (channels != 1u) {
        fprintf(stderr, "Error: Unsupported channel count %u (required: 1 mono)\n",
                (unsigned)channels);
        return 4;
    }

    /* Configure expected frame parameters (LFM chirp, 2000-6000 Hz) */
    memset(&fconfig, 0, sizeof(fconfig));
    fconfig.preamble_type = EXP001_PREAMBLE_LFM_CHIRP;
    fconfig.preamble_duration_s = 0.2;  /* Match E3 source WAV */
    fconfig.preamble_f_start_hz = 2000.0;
    fconfig.preamble_f_end_hz = 6000.0;
    fconfig.leading_silence_s = 0.1;
    fconfig.silence_duration_s = 0.5;
    fconfig.include_training = 1u;
    fconfig.detection_threshold = 0.40;

    printf("\nAcquiring preamble & decoding frame...\n");
    est = exp001_frame_decode(&fconfig, g_samples, num_samples,
                              recovered_bytes, sizeof(recovered_bytes),
                              &decode_res);

    if (est == EXP001_ERR_PREAMBLE_NOT_FOUND) {
        /* Try 100ms preamble as fallback */
        fconfig.preamble_duration_s = 0.1;
        est = exp001_frame_decode(&fconfig, g_samples, num_samples,
                                  recovered_bytes, sizeof(recovered_bytes),
                                  &decode_res);
    }

    if (est != EXP001_OK) {
        fprintf(stderr, "Decode failed with status %d:\n", (int)est);
        if (est == EXP001_ERR_PREAMBLE_NOT_FOUND) fprintf(stderr, "  Preamble not acquired (correlation threshold unmet)\n");
        else if (est == EXP001_ERR_CRC_MISMATCH) fprintf(stderr, "  CRC-16 mismatch: received=0x%04X, computed=0x%04X\n",
                                                         decode_res.received_crc, decode_res.computed_crc);
        else if (est == EXP001_ERR_SYNC_FAILURE) fprintf(stderr, "  Sync failure: buffer ended prematurely\n");
        return 3;
    }

    printf("  Preamble acquired! End sample: %zu\n", decode_res.preamble_end_sample);
    printf("  Payload start sample: %zu\n", decode_res.payload_start_sample);
    printf("  CRC-16: 0x%04X (VALID)\n", decode_res.computed_crc);
    printf("  Payload length: %zu bytes\n", decode_res.payload_bytes);
    printf("  Raw Wire bytes: ");
    for (i = 0u; i < decode_res.payload_bytes; ++i) {
        printf("%02X ", recovered_bytes[i]);
    }
    printf("\n");

    /* Pass recovered bytes through MCL Wire codec */
    printf("\nPassing recovered bytes to mcl_wire_tier0_decode...\n");
    wst = mcl_wire_tier0_decode(recovered_bytes, decode_res.payload_bytes, &obj, &wire_consumed);
    if (wst != MCL_WIRE_OK) {
        fprintf(stderr, "MCL Wire decode failed with status %d\n", (int)wst);
        return 4;
    }

    printf("  MCL Wire decode SUCCESS!\n");
    printf("  Kind: %s (%u)\n", kind_str(obj.kind), (unsigned)obj.kind);
    printf("  Priority: %u\n", (unsigned)obj.priority);
    printf("  Source ref: 0x%08X\n", obj.source_ref);

    switch (obj.kind) {
    case MCL_WIRE_KIND_PRESENCE:
        printf("  Machine class: %u\n", (unsigned)obj.body.presence.machine_class);
        printf("  Capability digest: 0x%06X\n", obj.body.presence.capability_digest);
        printf("  TTL: %u\n", (unsigned)obj.body.presence.ttl);
        break;
    case MCL_WIRE_KIND_HAZARD:
        printf("  Hazard class: %u, severity: %u, confidence: %u\n",
               (unsigned)obj.body.hazard.hazard_class, (unsigned)obj.body.hazard.severity, (unsigned)obj.body.hazard.confidence);
        printf("  Position: (%d, %d, %d), radius: %u, TTL: %u\n",
               obj.body.hazard.x, obj.body.hazard.y, obj.body.hazard.z, (unsigned)obj.body.hazard.radius, (unsigned)obj.body.hazard.ttl);
        break;
    default:
        break;
    }

    printf("\n>>> STATUS: BIT-PERFECT RECOVERY VERIFIED <<<\n");
    return 0;
}
