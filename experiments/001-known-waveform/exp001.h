/*
 * MCL-AP Experiment 001: Known-Waveform Acoustic Modem
 * =====================================================
 *
 * LAB / EXPERIMENTAL waveform only.
 * This is NOT a normative AP profile.
 * This is NOT AP-B0.
 *
 * Purpose: First bit-perfect acoustic encode/decode path using real MCL Wire bytes,
 * with rigorous preamble acquisition, resource equalization, and impairment benchmarking.
 *
 * Modulation: Binary FSK (deliberately simple, robust)
 * Sample rate: 48000 Hz mono PCM
 * Band: Mid-audible (~2000-6000 Hz region), suitable for laptop/phone speakers
 *
 * Frame structure:
 *   [preamble (equalized energy)]
 *   [FSK training sequence: alternating bits for symbol timing acquisition]
 *   [PHY header: payload_len (1 byte) + CRC-16 (2 bytes)]
 *   [FSK payload: canonical MCL Wire bytes]
 *   [silence]
 *
 * The preamble, training pattern, and modulation parameters are experimental
 * and NOT frozen into MCL-AP.
 */

#ifndef MCL_AP_EXP001_H
#define MCL_AP_EXP001_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------- Configuration ---------- */

#define EXP001_SAMPLE_RATE          48000u
#define EXP001_BITS_PER_SAMPLE      16u
#define EXP001_CHANNELS             1u

/* Binary FSK parameters */
#define EXP001_FSK_FREQ_0           3000.0  /* Hz — mark (bit 0) */
#define EXP001_FSK_FREQ_1           5000.0  /* Hz — space (bit 1) */
#define EXP001_FSK_BAUD             300u    /* symbols/sec (= bits/sec for binary FSK) */

/* Training sequence for symbol timing acquisition (NOT AP-B0) */
#define EXP001_TRAINING_BITS        16u
#define EXP001_TRAINING_BYTE        0x55u   /* 01010101 alternating bit pattern */

/* Preamble capacity */
#define EXP001_PREAMBLE_MAX_SAMPLES (EXP001_SAMPLE_RATE * 2u)  /* max 2s preamble */

/* Maximum payload (MCL Wire Tier-0 max is 17 bytes) */
#define EXP001_MAX_PAYLOAD_BYTES    64u

/* CRC-16/CCITT for integrity check */
#define EXP001_CRC16_INIT           0xFFFFu
#define EXP001_CRC16_POLY           0x1021u

/* Target normalized energy per sample (for resource equalization) */
#define EXP001_TARGET_ENERGY_PER_SAMPLE 0.40

/* Maximum multipath paths */
#define EXP001_MAX_MULTIPATH_PATHS  5u

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
    EXP001_PREAMBLE_ZC_DERIVED   = 1u,
    EXP001_PREAMBLE_PN_MSEQ      = 2u,
    EXP001_PREAMBLE_FREQ_DIVERSE = 3u,
    EXP001_PREAMBLE_TYPE_COUNT   = 4u
};

/* ---------- Resource equalization metrics ---------- */

typedef struct {
    size_t sample_count;
    double energy;          /* sum(x[n]^2) */
    double rms;             /* sqrt(energy / sample_count) */
    float peak;             /* max |x[n]| */
} exp001_preamble_energy_t;

/* ---------- Preamble generation ---------- */

/*
 * Generate in-phase (I) and optionally quadrature (Q) preamble samples.
 * Invariant: returns EXACTLY (size_t)round(duration_s * EXP001_SAMPLE_RATE)
 * when out_capacity >= required.
 *
 * If out_samples_q is non-NULL, writes the 90-degree phase-shifted quadrature
 * reference for phase-insensitive correlation.
 */
size_t exp001_generate_preamble(
    exp001_preamble_type_t type,
    double duration_s,
    double f_start_hz,
    double f_end_hz,
    float *out_samples,
    size_t out_capacity);

size_t exp001_generate_preamble_iq(
    exp001_preamble_type_t type,
    double duration_s,
    double f_start_hz,
    double f_end_hz,
    float *out_samples_i,
    float *out_samples_q,
    size_t out_capacity);

/*
 * Equalize preamble energy to an exact target energy budget.
 * Multiplies samples by scale factor so sum(x[n]^2) == target_energy.
 */
exp001_status_t exp001_equalize_preamble_energy(
    float *samples,
    size_t num_samples,
    double target_energy,
    exp001_preamble_energy_t *metrics);

/*
 * Verify m-sequence LFSR properties (period 127, all non-zero states visited once).
 * Returns 1 if valid, 0 on failure.
 */
uint8_t exp001_verify_mseq_properties(void);

/* ---------- Preamble detector metrics ---------- */

typedef struct {
    size_t peak_sample_index;       /* estimated start offset of preamble */
    double peak_correlation;        /* normalized correlation magnitude [0, 1] */
    uint8_t detected;               /* 1 if peak_correlation >= threshold */
} exp001_preamble_detect_t;

/*
 * Cross-correlation preamble acquisition using magnitude / quadrature metric:
 *   rho[k] = sqrt(corr_I[k]^2 + corr_Q[k]^2) / sqrt(E_ref * E_sig[k])
 *
 * Insensitive to polarity inversion and arbitrary carrier phase.
 */
exp001_preamble_detect_t exp001_detect_preamble_iq(
    exp001_preamble_type_t type,
    double duration_s,
    double f_start_hz,
    double f_end_hz,
    double threshold,
    const float *samples,
    size_t num_samples);

/* ---------- FSK modulation & training ---------- */

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
 * Demodulate PCM samples with symbol timing acquisition.
 * Uses known training pattern to search for optimal symbol phase offset
 * and effective samples-per-symbol before demodulating payload.
 */
size_t exp001_fsk_demodulate_timed(
    const float *samples,
    size_t num_samples,
    size_t training_bits,
    uint8_t *out_payload,
    size_t out_capacity,
    double *estimated_phase_offset,
    double *estimated_samples_per_symbol);

/*
 * Demodulate PCM samples assuming exact nominal timing.
 */
size_t exp001_fsk_demodulate(
    const float *samples,
    size_t num_samples,
    uint8_t *out_payload,
    size_t out_capacity);

/* ---------- CRC-16 ---------- */

uint16_t exp001_crc16(const uint8_t *data, size_t len);

/* ---------- Frame encode/decode ---------- */

typedef struct {
    exp001_preamble_type_t preamble_type;
    double preamble_duration_s;
    double preamble_f_start_hz;
    double preamble_f_end_hz;
    double leading_silence_s;       /* configurable leading silence */
    double silence_duration_s;      /* trailing silence */
    uint8_t include_training;       /* 1 = include FSK training sequence */
    double detection_threshold;     /* correlation magnitude threshold */
} exp001_frame_config_t;

/*
 * Encodes a complete frame:
 *   [leading silence] [equalized preamble] [optional training] [PHY header] [payload] [trailing silence]
 *
 * Returns total samples written, and outputs exact preamble samples generated.
 */
size_t exp001_frame_encode(
    const exp001_frame_config_t *config,
    const uint8_t *payload,
    size_t payload_len,
    float *out_samples,
    size_t out_capacity,
    size_t *actual_preamble_samples);

typedef struct {
    size_t estimated_preamble_start;
    size_t actual_preamble_length;
    size_t preamble_end_sample;
    size_t payload_start_sample;
    size_t payload_bytes;
    double peak_correlation;
    double estimated_symbol_phase;
    double estimated_samples_per_symbol;
    uint16_t received_crc;
    uint16_t computed_crc;
    uint8_t preamble_detected;
    uint8_t crc_valid;
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
    double awgn_snr_db;             /* <= 0 disables */
    double colored_noise_snr_db;    /* <= 0 disables */
    double clipping_threshold;       /* 0.0-1.0; >= 1.0 disables */
    double sample_rate_offset_ppm;   /* 0.0 disables */

    /* Multipath */
    unsigned num_multipath_paths;
    double multipath_delays_ms[EXP001_MAX_MULTIPATH_PATHS];
    double multipath_gains[EXP001_MAX_MULTIPATH_PATHS];

    /* Band attenuation / erasure */
    double band_atten_f_low_hz;     /* 0 disables */
    double band_atten_f_high_hz;
    double band_atten_factor;        /* e.g. 0.1 for 20 dB suppression */

    uint32_t rng_seed;
} exp001_impairment_config_t;

/*
 * Out-of-place SRO resampler:
 * Maps input samples to output samples according to clock offset ppm.
 * Reads ONLY from immutable source buffer; destination is separate.
 */
exp001_status_t exp001_resample_sro(
    const float *src,
    size_t num_src_samples,
    double ppm,
    float *dst,
    size_t dst_capacity,
    size_t *num_dst_samples);

/*
 * Apply deterministic impairments out-of-place:
 * reads from src, writes to dst.
 */
exp001_status_t exp001_apply_impairments(
    const exp001_impairment_config_t *config,
    const float *src,
    size_t num_samples,
    float *dst,
    size_t dst_capacity,
    size_t *out_samples);

#ifdef __cplusplus
}
#endif

#endif /* MCL_AP_EXP001_H */
