/*
 * MCL-AP Experiment 001: Known-Waveform Acoustic Modem
 * =====================================================
 *
 * LAB / EXPERIMENTAL waveform only.
 * This is NOT a normative AP profile.
 * This is NOT AP-B0.
 *
 * Purpose: First bit-perfect acoustic encode/decode path using real MCL Wire bytes.
 *
 * Modulation: Binary FSK (deliberately simple, robust)
 * Sample rate: 48000 Hz mono PCM
 * Band: Mid-audible (~2000-6000 Hz region), suitable for laptop/phone speakers
 *
 * Frame structure:
 *   [preamble] [PHY header: payload_len, crc16] [FSK-modulated MCL Wire bytes] [silence]
 *
 * The preamble and modulation parameters are experimental and NOT frozen into MCL-AP.
 */

#ifndef MCL_AP_EXP001_H
#define MCL_AP_EXP001_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------- Configuration ---------- */

#define EXP001_SAMPLE_RATE      48000u
#define EXP001_BITS_PER_SAMPLE  16u
#define EXP001_CHANNELS         1u

/* Binary FSK parameters */
#define EXP001_FSK_FREQ_0       3000.0  /* Hz — mark (bit 0) */
#define EXP001_FSK_FREQ_1       5000.0  /* Hz — space (bit 1) */
#define EXP001_FSK_BAUD         300u    /* symbols/sec (= bits/sec for binary FSK) */

/* Preamble */
#define EXP001_PREAMBLE_MAX_SAMPLES  (EXP001_SAMPLE_RATE * 2u)  /* max 2s preamble */

/* Maximum payload (MCL Wire Tier-0 max is 17 bytes) */
#define EXP001_MAX_PAYLOAD_BYTES     64u

/* CRC-16/CCITT for integrity check */
#define EXP001_CRC16_INIT       0xFFFFu
#define EXP001_CRC16_POLY       0x1021u

/* ---------- Status ---------- */

typedef int32_t exp001_status_t;
enum {
    EXP001_OK = 0,
    EXP001_ERR_INVALID_ARGUMENT = 1,
    EXP001_ERR_BUFFER_TOO_SMALL = 2,
    EXP001_ERR_WAV_FORMAT = 3,
    EXP001_ERR_FILE_IO = 4,
    EXP001_ERR_PREAMBLE_NOT_FOUND = 5,
    EXP001_ERR_CRC_MISMATCH = 6,
    EXP001_ERR_PAYLOAD_TOO_LARGE = 7,
    EXP001_ERR_SYNC_FAILURE = 8
};

/* ---------- Preamble candidates ---------- */

typedef uint8_t exp001_preamble_type_t;
enum {
    EXP001_PREAMBLE_LFM_CHIRP    = 0u,
    EXP001_PREAMBLE_ZADOFF_CHU   = 1u,
    EXP001_PREAMBLE_PN_MSEQ      = 2u,
    EXP001_PREAMBLE_FREQ_DIVERSE  = 3u,
    EXP001_PREAMBLE_TYPE_COUNT   = 4u
};

/* ---------- Preamble generation ---------- */

/*
 * Generate preamble samples into out_samples.
 * Returns the number of samples written.
 */
size_t exp001_generate_preamble(
    exp001_preamble_type_t type,
    double duration_s,
    double f_start_hz,
    double f_end_hz,
    float *out_samples,
    size_t out_capacity);

/* ---------- FSK modulation ---------- */

/*
 * Modulate payload bytes to PCM samples using binary FSK.
 * Returns number of samples written.
 */
size_t exp001_fsk_modulate(
    const uint8_t *payload,
    size_t payload_len,
    float *out_samples,
    size_t out_capacity);

/*
 * Demodulate PCM samples back to payload bytes using binary FSK.
 * Assumes exact timing (clean channel).
 * Returns number of payload bytes recovered.
 */
size_t exp001_fsk_demodulate(
    const float *samples,
    size_t num_samples,
    uint8_t *out_payload,
    size_t out_capacity);

/* ---------- CRC-16 ---------- */

uint16_t exp001_crc16(const uint8_t *data, size_t len);

/* ---------- Frame encode/decode ---------- */

/*
 * Encode a complete experimental frame:
 *   preamble + PHY header (1-byte len + 2-byte CRC16) + FSK payload + silence
 *
 * Returns total number of PCM samples written.
 */
typedef struct {
    exp001_preamble_type_t preamble_type;
    double preamble_duration_s;
    double preamble_f_start_hz;
    double preamble_f_end_hz;
    double silence_duration_s;
} exp001_frame_config_t;

size_t exp001_frame_encode(
    const exp001_frame_config_t *config,
    const uint8_t *payload,
    size_t payload_len,
    float *out_samples,
    size_t out_capacity);

/*
 * Decode an experimental frame from PCM samples.
 * Performs preamble detection, FSK demodulation, CRC verification.
 *
 * Returns EXP001_OK on success with payload bytes in out_payload.
 */
typedef struct {
    size_t preamble_end_sample;   /* sample index where preamble ends */
    size_t payload_start_sample;  /* sample index where payload FSK begins */
    size_t payload_bytes;         /* recovered payload length */
    uint16_t received_crc;        /* CRC from PHY header */
    uint16_t computed_crc;        /* CRC computed over recovered payload */
    uint8_t crc_valid;            /* 1 if match, 0 if mismatch */
} exp001_decode_result_t;

exp001_status_t exp001_frame_decode(
    const exp001_frame_config_t *config,
    const float *samples,
    size_t num_samples,
    uint8_t *out_payload,
    size_t out_capacity,
    exp001_decode_result_t *result);

/* ---------- WAV I/O ---------- */

exp001_status_t exp001_wav_write(
    const char *path,
    const float *samples,
    size_t num_samples,
    uint32_t sample_rate,
    uint16_t bits_per_sample,
    uint16_t channels);

exp001_status_t exp001_wav_read(
    const char *path,
    float *out_samples,
    size_t out_capacity,
    size_t *num_samples_read,
    uint32_t *sample_rate,
    uint16_t *bits_per_sample,
    uint16_t *channels);

/* ---------- Impairment harness ---------- */

typedef struct {
    double awgn_snr_db;           /* Signal-to-noise ratio; <= 0 disables */
    double sample_rate_offset_ppm; /* Parts-per-million clock offset */
    double clipping_threshold;     /* 0.0-1.0; 1.0 = no clipping */
    uint32_t rng_seed;            /* Deterministic RNG seed */
} exp001_impairment_config_t;

/*
 * Apply deterministic impairments to PCM samples in-place.
 */
exp001_status_t exp001_apply_impairments(
    const exp001_impairment_config_t *config,
    float *samples,
    size_t num_samples);

/* ---------- Metrics ---------- */

typedef struct {
    uint8_t preamble_detected;
    uint8_t payload_recovered;
    uint8_t crc_valid;
    uint8_t bit_perfect;          /* 1 if recovered bytes == source bytes */
    double timing_error_samples;  /* estimated preamble timing error */
    size_t source_payload_len;
    size_t recovered_payload_len;
    uint32_t bit_errors;          /* number of differing bits */
} exp001_metrics_t;

#ifdef __cplusplus
}
#endif

#endif /* MCL_AP_EXP001_H */
