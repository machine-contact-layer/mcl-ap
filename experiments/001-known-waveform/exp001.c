/*
 * MCL-AP Experiment 001: Known-Waveform — Core Implementation
 *
 * LAB / EXPERIMENTAL waveform only. NOT a normative AP profile.
 */

#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "exp001.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ========== CRC-16/CCITT ========== */

uint16_t exp001_crc16(const uint8_t *data, size_t len)
{
    uint16_t crc = EXP001_CRC16_INIT;
    size_t i;
    unsigned bit;

    if (data == NULL) {
        return 0u;
    }

    for (i = 0u; i < len; ++i) {
        crc ^= (uint16_t)((uint16_t)data[i] << 8u);
        for (bit = 0u; bit < 8u; ++bit) {
            if ((crc & 0x8000u) != 0u) {
                crc = (uint16_t)(((uint32_t)crc << 1u) ^ (uint32_t)EXP001_CRC16_POLY);
            } else {
                crc = (uint16_t)((uint32_t)crc << 1u);
            }
        }
    }

    return crc;
}

/* ========== Verified 7-bit m-sequence LFSR ========== */

/*
 * Primitive polynomial: x^7 + x^6 + 1
 * Standard Fibonacci LFSR with left shift:
 *   b6 = (state >> 6) & 1
 *   b5 = (state >> 5) & 1
 *   fb = b6 ^ b5
 *   state = ((state << 1) & 0x7F) | fb
 * Cycle length is exactly 2^7 - 1 = 127.
 */
static void generate_mseq_bits(uint8_t out_mseq[127])
{
    uint8_t state = 1u;
    size_t i;

    for (i = 0u; i < 127u; ++i) {
        uint8_t b6 = (uint8_t)((state >> 6u) & 1u);
        uint8_t b5 = (uint8_t)((state >> 5u) & 1u);
        uint8_t fb = (uint8_t)(b6 ^ b5);

        out_mseq[i] = b6;
        state = (uint8_t)((((uint32_t)state << 1u) & 0x7Fu) | (uint32_t)fb);
    }
}

uint8_t exp001_verify_mseq_properties(void)
{
    uint8_t state = 1u;
    uint8_t visited[128];
    unsigned count = 0u;
    size_t i;

    for (i = 0u; i < 128u; ++i) {
        visited[i] = 0u;
    }

    /* 1. Starting state must be non-zero */
    if (state == 0u) {
        return 0u;
    }

    /* 2. Traverse sequence */
    while (visited[state] == 0u && state != 0u && count < 256u) {
        visited[state] = 1u;
        count++;

        uint8_t b6 = (uint8_t)((state >> 6u) & 1u);
        uint8_t b5 = (uint8_t)((state >> 5u) & 1u);
        uint8_t fb = (uint8_t)(b6 ^ b5);
        state = (uint8_t)((((uint32_t)state << 1u) & 0x7Fu) | (uint32_t)fb);
    }

    /* 3. Period must be exactly 127 */
    if (count != 127u) {
        return 0u;
    }

    /* 4. First state repeated only after 127 steps (returned to 1) */
    if (state != 1u) {
        return 0u;
    }

    /* 5. All 127 non-zero states occur exactly once */
    if (visited[0] != 0u) {
        return 0u;
    }
    for (i = 1u; i <= 127u; ++i) {
        if (visited[i] != 1u) {
            return 0u;
        }
    }

    return 1u;
}

/* ========== Preamble generation (Exact Length Invariant) ========== */

static size_t gen_lfm_chirp_iq(
    double duration_s, double f_start, double f_end,
    float *out_i, float *out_q, size_t capacity)
{
    size_t n = (size_t)floor(duration_s * (double)EXP001_SAMPLE_RATE + 0.5);
    size_t i;
    double k;

    if (n > capacity) n = capacity;
    if (n == 0u) return 0u;

    k = (f_end - f_start) / duration_s;

    for (i = 0u; i < n; ++i) {
        double t = (double)i / (double)EXP001_SAMPLE_RATE;
        double phase = 2.0 * M_PI * (f_start * t + 0.5 * k * t * t);
        out_i[i] = (float)sin(phase);
        if (out_q != NULL) {
            out_q[i] = (float)cos(phase);
        }
    }

    return n;
}

static size_t gen_zc_derived_iq(
    double duration_s, double f_center,
    float *out_i, float *out_q, size_t capacity)
{
    const size_t N_zc = 127u;
    const size_t u = 7u;
    size_t n = (size_t)floor(duration_s * (double)EXP001_SAMPLE_RATE + 0.5);
    size_t i;

    if (n > capacity) n = capacity;
    if (n == 0u) return 0u;

    for (i = 0u; i < n; ++i) {
        /* Deterministic mapping across exactly n output samples */
        size_t chip = (size_t)(((uint64_t)i * (uint64_t)N_zc) / (uint64_t)n);
        double zc_phase = M_PI * (double)u * (double)chip * (double)(chip + 1u) / (double)N_zc;
        double t = (double)i / (double)EXP001_SAMPLE_RATE;
        double theta = 2.0 * M_PI * f_center * t + zc_phase;

        out_i[i] = (float)sin(theta);
        if (out_q != NULL) {
            out_q[i] = (float)cos(theta);
        }
    }

    return n;
}

static size_t gen_pn_mseq_iq(
    double duration_s, double f_center,
    float *out_i, float *out_q, size_t capacity)
{
    uint8_t mseq[127];
    size_t n = (size_t)floor(duration_s * (double)EXP001_SAMPLE_RATE + 0.5);
    size_t i;

    if (n > capacity) n = capacity;
    if (n == 0u) return 0u;

    generate_mseq_bits(mseq);

    for (i = 0u; i < n; ++i) {
        size_t chip = (size_t)(((uint64_t)i * 127u) / (uint64_t)n);
        double phase_offset = (mseq[chip] != 0u) ? M_PI : 0.0;
        double t = (double)i / (double)EXP001_SAMPLE_RATE;
        double theta = 2.0 * M_PI * f_center * t + phase_offset;

        out_i[i] = (float)sin(theta);
        if (out_q != NULL) {
            out_q[i] = (float)cos(theta);
        }
    }

    return n;
}

static size_t gen_freq_diverse_iq(
    double duration_s, double f_start, double f_end,
    float *out_i, float *out_q, size_t capacity)
{
    const unsigned n_tones = 4u;
    const unsigned n_segments = 8u; /* 4 tones x 2 repeats */
    size_t n = (size_t)floor(duration_s * (double)EXP001_SAMPLE_RATE + 0.5);
    size_t i;

    if (n > capacity) n = capacity;
    if (n == 0u) return 0u;

    for (i = 0u; i < n; ++i) {
        size_t seg = (size_t)(((uint64_t)i * (uint64_t)n_segments) / (uint64_t)n);
        unsigned tone_idx = (unsigned)(seg % n_tones);
        double freq = f_start + (double)tone_idx * (f_end - f_start) / (double)(n_tones - 1u);
        double t = (double)i / (double)EXP001_SAMPLE_RATE;
        double theta = 2.0 * M_PI * freq * t;

        out_i[i] = (float)sin(theta);
        if (out_q != NULL) {
            out_q[i] = (float)cos(theta);
        }
    }

    return n;
}

size_t exp001_generate_preamble_iq(
    exp001_preamble_type_t type,
    double duration_s,
    double f_start_hz,
    double f_end_hz,
    float *out_samples_i,
    float *out_samples_q,
    size_t out_capacity)
{
    if (out_samples_i == NULL || out_capacity == 0u || duration_s <= 0.0) {
        return 0u;
    }

    switch (type) {
    case EXP001_PREAMBLE_LFM_CHIRP:
        return gen_lfm_chirp_iq(duration_s, f_start_hz, f_end_hz, out_samples_i, out_samples_q, out_capacity);
    case EXP001_PREAMBLE_ZC_DERIVED:
        return gen_zc_derived_iq(duration_s, (f_start_hz + f_end_hz) / 2.0, out_samples_i, out_samples_q, out_capacity);
    case EXP001_PREAMBLE_PN_MSEQ:
        return gen_pn_mseq_iq(duration_s, (f_start_hz + f_end_hz) / 2.0, out_samples_i, out_samples_q, out_capacity);
    case EXP001_PREAMBLE_FREQ_DIVERSE:
        return gen_freq_diverse_iq(duration_s, f_start_hz, f_end_hz, out_samples_i, out_samples_q, out_capacity);
    default:
        return 0u;
    }
}

size_t exp001_generate_preamble(
    exp001_preamble_type_t type,
    double duration_s,
    double f_start_hz,
    double f_end_hz,
    float *out_samples,
    size_t out_capacity)
{
    return exp001_generate_preamble_iq(type, duration_s, f_start_hz, f_end_hz,
                                       out_samples, NULL, out_capacity);
}

/* ========== Resource Equalization ========== */

exp001_status_t exp001_equalize_preamble_energy(
    float *samples,
    size_t num_samples,
    double target_energy,
    exp001_preamble_energy_t *metrics)
{
    double current_energy = 0.0;
    double scale;
    float peak = 0.0f;
    size_t i;

    if (samples == NULL || num_samples == 0u || target_energy <= 0.0) {
        return EXP001_ERR_INVALID_ARGUMENT;
    }

    for (i = 0u; i < num_samples; ++i) {
        current_energy += (double)samples[i] * (double)samples[i];
    }
    if (current_energy <= 0.0) {
        return EXP001_ERR_INVALID_ARGUMENT;
    }

    scale = sqrt(target_energy / current_energy);

    for (i = 0u; i < num_samples; ++i) {
        samples[i] = (float)((double)samples[i] * scale);
        float mag = (float)fabs((double)samples[i]);
        if (mag > peak) {
            peak = mag;
        }
    }

    if (metrics != NULL) {
        metrics->sample_count = num_samples;
        metrics->energy = target_energy;
        metrics->rms = sqrt(target_energy / (double)num_samples);
        metrics->peak = peak;
    }

    return EXP001_OK;
}

/* ========== Quadrature Correlation Detector ========== */

exp001_preamble_detect_t exp001_detect_preamble_iq(
    exp001_preamble_type_t type,
    double duration_s,
    double f_start_hz,
    double f_end_hz,
    double threshold,
    const float *samples,
    size_t num_samples)
{
    exp001_preamble_detect_t result;
    float ref_i[EXP001_PREAMBLE_MAX_SAMPLES];
    float ref_q[EXP001_PREAMBLE_MAX_SAMPLES];
    size_t ref_len;
    double ref_energy = 0.0;
    size_t best_offset = 0u;
    double best_mag = -1.0;
    size_t offset, i;

    result.peak_sample_index = 0u;
    result.peak_correlation = 0.0;
    result.detected = 0u;

    ref_len = exp001_generate_preamble_iq(
        type, duration_s, f_start_hz, f_end_hz,
        ref_i, ref_q, EXP001_PREAMBLE_MAX_SAMPLES);

    if (ref_len == 0u || ref_len > num_samples || samples == NULL) {
        return result;
    }

    /* Reference energy E_ref = sum(ref_i^2) */
    for (i = 0u; i < ref_len; ++i) {
        ref_energy += (double)ref_i[i] * (double)ref_i[i];
    }
    if (ref_energy <= 0.0) {
        ref_energy = 1.0;
    }

    /* Slide across search window */
    for (offset = 0u; offset <= num_samples - ref_len; ++offset) {
        double corr_i = 0.0;
        double corr_q = 0.0;
        double sig_energy = 0.0;

        for (i = 0u; i < ref_len; ++i) {
            double s = (double)samples[offset + i];
            corr_i += (double)ref_i[i] * s;
            corr_q += (double)ref_q[i] * s;
            sig_energy += s * s;
        }

        double norm = sqrt(ref_energy * sig_energy);
        double mag = 0.0;
        if (norm > 1e-12) {
            mag = sqrt(corr_i * corr_i + corr_q * corr_q) / norm;
        }

        if (mag > best_mag) {
            best_mag = mag;
            best_offset = offset;
        }
    }

    result.peak_sample_index = best_offset;
    result.peak_correlation = (best_mag > 0.0) ? best_mag : 0.0;
    result.detected = (result.peak_correlation >= threshold) ? 1u : 0u;

    return result;
}

/* ========== FSK Modulation ========== */

size_t exp001_fsk_modulate(
    const uint8_t *payload,
    size_t payload_len,
    float *out_samples,
    size_t out_capacity)
{
    size_t samples_per_bit = EXP001_SAMPLE_RATE / EXP001_FSK_BAUD;
    size_t total_bits = payload_len * 8u;
    size_t total_samples = total_bits * samples_per_bit;
    size_t bit_idx, s;
    double phase = 0.0;

    if (payload == NULL || out_samples == NULL || payload_len == 0u) {
        return 0u;
    }
    if (total_samples > out_capacity) {
        return 0u;
    }

    for (bit_idx = 0u; bit_idx < total_bits; ++bit_idx) {
        size_t byte_idx = bit_idx / 8u;
        unsigned bit_pos = 7u - (unsigned)(bit_idx % 8u);  /* MSB first */
        uint8_t bit_val = (uint8_t)((payload[byte_idx] >> bit_pos) & 1u);
        double freq = (bit_val == 0u) ? EXP001_FSK_FREQ_0 : EXP001_FSK_FREQ_1;
        double phase_inc = 2.0 * M_PI * freq / (double)EXP001_SAMPLE_RATE;

        for (s = 0u; s < samples_per_bit; ++s) {
            size_t idx = bit_idx * samples_per_bit + s;
            out_samples[idx] = (float)sin(phase);
            phase += phase_inc;
            if (phase >= 2.0 * M_PI) {
                phase -= 2.0 * M_PI;
            }
        }
    }

    return total_samples;
}

/* ========== FSK Demodulation with Symbol Timing Acquisition ========== */

static double goertzel_power(const float *samples, size_t n, double target_freq_hz)
{
    double w = 2.0 * M_PI * target_freq_hz / (double)EXP001_SAMPLE_RATE;
    double coeff = 2.0 * cos(w);
    double s_prev = 0.0, s_prev2 = 0.0;
    size_t i;

    for (i = 0u; i < n; ++i) {
        double x = (double)samples[i];
        double temp = coeff * s_prev - s_prev2 + x;
        s_prev2 = s_prev;
        s_prev = temp;
    }

    return s_prev * s_prev + s_prev2 * s_prev2 - coeff * s_prev * s_prev2;
}

size_t exp001_fsk_demodulate_timed(
    const float *samples,
    size_t num_samples,
    size_t training_bits,
    uint8_t *out_payload,
    size_t out_capacity,
    double *estimated_phase_offset,
    double *estimated_samples_per_symbol)
{
    const size_t nominal_sps = EXP001_SAMPLE_RATE / EXP001_FSK_BAUD;
    double best_score = -1e30;
    int best_phase = 0;
    double best_sps = (double)nominal_sps;
    int phase_try;
    double sps_try;
    size_t total_payload_bytes, bit_idx;

    if (samples == NULL || out_payload == NULL || out_capacity == 0u) {
        return 0u;
    }

    /*
     * 1. Symbol timing search over training sequence:
     * Search timing phase in [-16, +16] samples around start,
     * and samples-per-symbol in [159.5, 160.5] (step 0.05).
     */
    if (training_bits > 0u) {
        for (phase_try = -16; phase_try <= 16; phase_try += 2) {
            for (sps_try = (double)nominal_sps - 0.5;
                 sps_try <= (double)nominal_sps + 0.5;
                 sps_try += 0.05) {

                double score = 0.0;
                size_t b;

                for (b = 0u; b < training_bits; ++b) {
                    double t0 = (double)phase_try + (double)b * sps_try;
                    if (t0 < 0.0 || (size_t)(t0 + sps_try) > num_samples) {
                        score -= 1e6;
                        continue;
                    }

                    size_t start_idx = (size_t)t0;
                    size_t win_len = (size_t)sps_try;
                    double p0 = goertzel_power(samples + start_idx, win_len, EXP001_FSK_FREQ_0);
                    double p1 = goertzel_power(samples + start_idx, win_len, EXP001_FSK_FREQ_1);

                    /* Expected training bit: 01010101 pattern (b & 1) */
                    uint8_t expected = (uint8_t)(b & 1u);
                    if (expected == 0u) {
                        score += (p0 - p1);
                    } else {
                        score += (p1 - p0);
                    }
                }

                if (score > best_score) {
                    best_score = score;
                    best_phase = phase_try;
                    best_sps = sps_try;
                }
            }
        }
    }

    if (estimated_phase_offset != NULL) {
        *estimated_phase_offset = (double)best_phase;
    }
    if (estimated_samples_per_symbol != NULL) {
        *estimated_samples_per_symbol = best_sps;
    }

    /*
     * 2. Demodulate payload symbols using acquired timing.
     */
    double payload_start_t = (double)best_phase + (double)training_bits * best_sps;
    if (payload_start_t < 0.0 || (size_t)payload_start_t >= num_samples) {
        return 0u;
    }

    size_t remaining_samples = num_samples - (size_t)payload_start_t;
    size_t total_bits = (size_t)((double)remaining_samples / best_sps);
    total_payload_bytes = total_bits / 8u;

    if (total_payload_bytes > out_capacity) {
        total_payload_bytes = out_capacity;
    }
    if (total_payload_bytes == 0u) {
        return 0u;
    }

    memset(out_payload, 0, total_payload_bytes);
    total_bits = total_payload_bytes * 8u;

    for (bit_idx = 0u; bit_idx < total_bits; ++bit_idx) {
        double t0 = payload_start_t + (double)bit_idx * best_sps;
        size_t start_idx = (size_t)t0;
        size_t win_len = (size_t)best_sps;

        if (start_idx + win_len > num_samples) {
            break;
        }

        double p0 = goertzel_power(samples + start_idx, win_len, EXP001_FSK_FREQ_0);
        double p1 = goertzel_power(samples + start_idx, win_len, EXP001_FSK_FREQ_1);

        if (p1 > p0) {
            size_t byte_idx = bit_idx / 8u;
            unsigned bit_pos = 7u - (unsigned)(bit_idx % 8u);
            out_payload[byte_idx] |= (uint8_t)(1u << bit_pos);
        }
    }

    return total_payload_bytes;
}

size_t exp001_fsk_demodulate(
    const float *samples,
    size_t num_samples,
    uint8_t *out_payload,
    size_t out_capacity)
{
    return exp001_fsk_demodulate_timed(samples, num_samples, 0u,
                                       out_payload, out_capacity, NULL, NULL);
}

/* ========== Frame Encode / Decode ========== */

size_t exp001_frame_encode(
    const exp001_frame_config_t *config,
    const uint8_t *payload,
    size_t payload_len,
    float *out_samples,
    size_t out_capacity,
    size_t *actual_preamble_samples)
{
    size_t pos = 0u;
    size_t preamble_samples, training_samples, header_samples, payload_samples;
    size_t leading_silence_samples, trailing_silence_samples;
    uint8_t training_bytes[EXP001_TRAINING_BITS / 8u];
    uint8_t phy_header[3];
    uint16_t crc;
    size_t i;

    if (config == NULL || payload == NULL || out_samples == NULL) {
        return 0u;
    }
    if (payload_len > EXP001_MAX_PAYLOAD_BYTES) {
        return 0u;
    }

    /* 1. Leading silence */
    leading_silence_samples = (size_t)floor(config->leading_silence_s * (double)EXP001_SAMPLE_RATE + 0.5);
    for (i = 0u; i < leading_silence_samples && pos < out_capacity; ++i) {
        out_samples[pos++] = 0.0f;
    }

    /* 2. Preamble with energy equalization */
    preamble_samples = exp001_generate_preamble(
        config->preamble_type,
        config->preamble_duration_s,
        config->preamble_f_start_hz,
        config->preamble_f_end_hz,
        out_samples + pos,
        out_capacity - pos);
    if (preamble_samples == 0u) return 0u;

    {
        double target_e = (double)preamble_samples * EXP001_TARGET_ENERGY_PER_SAMPLE;
        exp001_equalize_preamble_energy(out_samples + pos, preamble_samples, target_e, NULL);
    }
    if (actual_preamble_samples != NULL) {
        *actual_preamble_samples = preamble_samples;
    }
    pos += preamble_samples;

    /* 3. Optional FSK training sequence (alternating bits) */
    if (config->include_training != 0u) {
        for (i = 0u; i < sizeof(training_bytes); ++i) {
            training_bytes[i] = EXP001_TRAINING_BYTE;
        }
        training_samples = exp001_fsk_modulate(
            training_bytes, sizeof(training_bytes),
            out_samples + pos, out_capacity - pos);
        if (training_samples == 0u) return 0u;
        pos += training_samples;
    }

    /* 4. PHY header: [payload_len (1 byte)] [CRC16 (2 bytes)] */
    crc = exp001_crc16(payload, payload_len);
    phy_header[0] = (uint8_t)payload_len;
    phy_header[1] = (uint8_t)(crc >> 8u);
    phy_header[2] = (uint8_t)(crc & 0xFFu);

    header_samples = exp001_fsk_modulate(
        phy_header, 3u,
        out_samples + pos, out_capacity - pos);
    if (header_samples == 0u) return 0u;
    pos += header_samples;

    /* 5. Payload */
    payload_samples = exp001_fsk_modulate(
        payload, payload_len,
        out_samples + pos, out_capacity - pos);
    if (payload_samples == 0u) return 0u;
    pos += payload_samples;

    /* 6. Trailing silence */
    trailing_silence_samples = (size_t)floor(config->silence_duration_s * (double)EXP001_SAMPLE_RATE + 0.5);
    for (i = 0u; i < trailing_silence_samples && pos < out_capacity; ++i) {
        out_samples[pos++] = 0.0f;
    }

    return pos;
}

exp001_status_t exp001_frame_decode(
    const exp001_frame_config_t *config,
    const float *samples,
    size_t num_samples,
    uint8_t *out_payload,
    size_t out_capacity,
    exp001_decode_result_t *result)
{
    exp001_preamble_detect_t det;
    size_t preamble_len;
    size_t fsk_start;
    uint8_t header_and_payload[3 + EXP001_MAX_PAYLOAD_BYTES];
    size_t demod_bytes;
    uint8_t payload_len;
    uint16_t rx_crc, calc_crc;

    if (config == NULL || samples == NULL || out_payload == NULL || result == NULL) {
        return EXP001_ERR_INVALID_ARGUMENT;
    }

    memset(result, 0, sizeof(*result));

    /* 1. Cross-correlation preamble detection using quadrature magnitude */
    double thresh = (config->detection_threshold > 0.0) ? config->detection_threshold : 0.5;
    size_t acq_samples = (num_samples < 24000u) ? num_samples : 24000u;
    det = exp001_detect_preamble_iq(
        config->preamble_type,
        config->preamble_duration_s,
        config->preamble_f_start_hz,
        config->preamble_f_end_hz,
        thresh,
        samples,
        acq_samples);

    result->estimated_preamble_start = det.peak_sample_index;
    result->peak_correlation = det.peak_correlation;
    result->preamble_detected = det.detected;

    /* Exact preamble length invariant */
    preamble_len = (size_t)floor(config->preamble_duration_s * (double)EXP001_SAMPLE_RATE + 0.5);
    result->actual_preamble_length = preamble_len;
    result->preamble_end_sample = det.peak_sample_index + preamble_len;

    if (det.detected == 0u) {
        return EXP001_ERR_PREAMBLE_NOT_FOUND;
    }

    fsk_start = result->preamble_end_sample;
    if (fsk_start >= num_samples) {
        return EXP001_ERR_SYNC_FAILURE;
    }

    /* 2. Demodulate FSK with symbol timing search */
    size_t training_bits = (config->include_training != 0u) ? EXP001_TRAINING_BITS : 0u;
    demod_bytes = exp001_fsk_demodulate_timed(
        samples + fsk_start,
        num_samples - fsk_start,
        training_bits,
        header_and_payload,
        sizeof(header_and_payload),
        &result->estimated_symbol_phase,
        &result->estimated_samples_per_symbol);

    if (demod_bytes < 3u) {
        return EXP001_ERR_SYNC_FAILURE;
    }

    payload_len = header_and_payload[0];
    rx_crc = (uint16_t)(((uint16_t)header_and_payload[1] << 8u) | (uint16_t)header_and_payload[2]);
    result->received_crc = rx_crc;

    if (payload_len == 0u || payload_len > out_capacity) {
        return EXP001_ERR_PAYLOAD_TOO_LARGE;
    }
    if (demod_bytes < 3u + (size_t)payload_len) {
        return EXP001_ERR_SYNC_FAILURE;
    }

    memcpy(out_payload, header_and_payload + 3, payload_len);
    result->payload_bytes = payload_len;
    result->payload_start_sample = fsk_start;

    /* 3. CRC integrity verification */
    calc_crc = exp001_crc16(out_payload, payload_len);
    result->computed_crc = calc_crc;
    result->crc_valid = (rx_crc == calc_crc) ? 1u : 0u;

    if (result->crc_valid == 0u) {
        return EXP001_ERR_CRC_MISMATCH;
    }

    return EXP001_OK;
}

/* ========== WAV I/O ========== */

static void wav_write_u16(FILE *f, uint16_t v)
{
    uint8_t b[2];
    b[0] = (uint8_t)(v & 0xFFu);
    b[1] = (uint8_t)((v >> 8u) & 0xFFu);
    fwrite(b, 1, 2, f);
}

static void wav_write_u32(FILE *f, uint32_t v)
{
    uint8_t b[4];
    b[0] = (uint8_t)(v & 0xFFu);
    b[1] = (uint8_t)((v >> 8u) & 0xFFu);
    b[2] = (uint8_t)((v >> 16u) & 0xFFu);
    b[3] = (uint8_t)((v >> 24u) & 0xFFu);
    fwrite(b, 1, 4, f);
}

static uint16_t wav_read_u16(FILE *f)
{
    uint8_t b[2];
    fread(b, 1, 2, f);
    return (uint16_t)((uint16_t)b[0] | ((uint16_t)b[1] << 8u));
}

static uint32_t wav_read_u32(FILE *f)
{
    uint8_t b[4];
    fread(b, 1, 4, f);
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8u) |
           ((uint32_t)b[2] << 16u) | ((uint32_t)b[3] << 24u);
}

exp001_status_t exp001_wav_write(
    const char *path,
    const float *samples,
    size_t num_samples,
    uint32_t sample_rate,
    uint16_t bits_per_sample,
    uint16_t channels)
{
    FILE *f;
    uint32_t data_size;
    uint32_t file_size;
    uint16_t block_align;
    uint32_t byte_rate;
    size_t i;

    if (path == NULL || samples == NULL || num_samples == 0u) {
        return EXP001_ERR_INVALID_ARGUMENT;
    }
    if (bits_per_sample != 16u) {
        return EXP001_ERR_INVALID_ARGUMENT;
    }

    f = fopen(path, "wb");
    if (f == NULL) {
        return EXP001_ERR_FILE_IO;
    }

    data_size = (uint32_t)(num_samples * channels * (bits_per_sample / 8u));
    file_size = 36u + data_size;
    block_align = (uint16_t)(channels * (bits_per_sample / 8u));
    byte_rate = sample_rate * (uint32_t)block_align;

    fwrite("RIFF", 1, 4, f);
    wav_write_u32(f, file_size);
    fwrite("WAVE", 1, 4, f);

    fwrite("fmt ", 1, 4, f);
    wav_write_u32(f, 16u);
    wav_write_u16(f, 1u);
    wav_write_u16(f, channels);
    wav_write_u32(f, sample_rate);
    wav_write_u32(f, byte_rate);
    wav_write_u16(f, block_align);
    wav_write_u16(f, bits_per_sample);

    fwrite("data", 1, 4, f);
    wav_write_u32(f, data_size);

    for (i = 0u; i < num_samples; ++i) {
        double clamped = (double)samples[i];
        int16_t pcm_val;

        if (clamped > 1.0) clamped = 1.0;
        if (clamped < -1.0) clamped = -1.0;

        pcm_val = (int16_t)(clamped * 32767.0);
        wav_write_u16(f, (uint16_t)pcm_val);
    }

    fclose(f);
    return EXP001_OK;
}

exp001_status_t exp001_wav_read(
    const char *path,
    float *out_samples,
    size_t out_capacity,
    size_t *num_samples_read,
    uint32_t *sample_rate,
    uint16_t *bits_per_sample,
    uint16_t *channels)
{
    FILE *f;
    char chunk_id[4];
    uint32_t chunk_size;
    uint16_t audio_format;
    uint32_t data_size;
    size_t num_samples;
    size_t i;

    if (path == NULL || out_samples == NULL || num_samples_read == NULL) {
        return EXP001_ERR_INVALID_ARGUMENT;
    }

    f = fopen(path, "rb");
    if (f == NULL) {
        return EXP001_ERR_FILE_IO;
    }

    fread(chunk_id, 1, 4, f);
    if (memcmp(chunk_id, "RIFF", 4) != 0) {
        fclose(f);
        return EXP001_ERR_WAV_FORMAT;
    }
    wav_read_u32(f);
    fread(chunk_id, 1, 4, f);
    if (memcmp(chunk_id, "WAVE", 4) != 0) {
        fclose(f);
        return EXP001_ERR_WAV_FORMAT;
    }

    fread(chunk_id, 1, 4, f);
    if (memcmp(chunk_id, "fmt ", 4) != 0) {
        fclose(f);
        return EXP001_ERR_WAV_FORMAT;
    }
    chunk_size = wav_read_u32(f);
    audio_format = wav_read_u16(f);
    if (audio_format != 1u) {
        fclose(f);
        return EXP001_ERR_WAV_FORMAT;
    }
    *channels = wav_read_u16(f);
    *sample_rate = wav_read_u32(f);
    wav_read_u32(f);
    wav_read_u16(f);
    *bits_per_sample = wav_read_u16(f);

    if (chunk_size > 16u) {
        fseek(f, (long)(chunk_size - 16u), SEEK_CUR);
    }

    fread(chunk_id, 1, 4, f);
    if (memcmp(chunk_id, "data", 4) != 0) {
        fclose(f);
        return EXP001_ERR_WAV_FORMAT;
    }
    data_size = wav_read_u32(f);

    if (*bits_per_sample != 16u) {
        fclose(f);
        return EXP001_ERR_WAV_FORMAT;
    }

    num_samples = data_size / (*channels * (*bits_per_sample / 8u));
    if (num_samples > out_capacity) {
        num_samples = out_capacity;
    }

    for (i = 0u; i < num_samples; ++i) {
        int16_t pcm_val = (int16_t)wav_read_u16(f);
        out_samples[i] = (float)pcm_val / 32768.0f;
    }

    *num_samples_read = num_samples;
    fclose(f);
    return EXP001_OK;
}

/* ========== Out-of-place SRO Resampler ========== */

exp001_status_t exp001_resample_sro(
    const float *src,
    size_t num_src_samples,
    double ppm,
    float *dst,
    size_t dst_capacity,
    size_t *num_dst_samples)
{
    double ratio;
    size_t j = 0u;

    if (src == NULL || dst == NULL || num_src_samples < 2u || num_dst_samples == NULL) {
        return EXP001_ERR_INVALID_ARGUMENT;
    }

    ratio = 1.0 + ppm * 1e-6;
    if (ratio <= 0.0) {
        return EXP001_ERR_INVALID_ARGUMENT;
    }

    while (j < dst_capacity) {
        double src_idx = (double)j * ratio;
        size_t k = (size_t)src_idx;
        double frac;

        if (k + 1u >= num_src_samples) {
            break;
        }

        frac = src_idx - (double)k;
        dst[j] = (float)((1.0 - frac) * (double)src[k] + frac * (double)src[k + 1u]);
        j++;
    }

    *num_dst_samples = j;
    return EXP001_OK;
}

/* ========== Deterministic Impairment Harness ========== */

static uint32_t xorshift32(uint32_t *state)
{
    uint32_t x = *state;
    x ^= x << 13u;
    x ^= x >> 17u;
    x ^= x << 5u;
    *state = x;
    return x;
}

static double gaussian(uint32_t *state)
{
    double u1, u2;
    do {
        u1 = (double)(xorshift32(state) & 0x7FFFFFFFu) / (double)0x7FFFFFFF;
    } while (u1 == 0.0);
    u2 = (double)(xorshift32(state) & 0x7FFFFFFFu) / (double)0x7FFFFFFF;
    return sqrt(-2.0 * log(u1)) * cos(2.0 * M_PI * u2);
}

exp001_status_t exp001_apply_impairments(
    const exp001_impairment_config_t *config,
    const float *src,
    size_t num_samples,
    float *dst,
    size_t dst_capacity,
    size_t *out_samples)
{
    size_t i, p;
    uint32_t rng_state;
    double sig_power = 0.0;
    size_t current_len = num_samples;

    if (config == NULL || src == NULL || dst == NULL || out_samples == NULL) {
        return EXP001_ERR_INVALID_ARGUMENT;
    }
    if (num_samples > dst_capacity) {
        return EXP001_ERR_BUFFER_TOO_SMALL;
    }

    rng_state = (config->rng_seed != 0u) ? config->rng_seed : 1u;

    /* Copy initial source to destination */
    memcpy(dst, src, num_samples * sizeof(float));

    /* 1. Multipath (2-path or up to 5-path with bounded 0-30 ms delay) */
    if (config->num_multipath_paths > 1u) {
        unsigned paths = config->num_multipath_paths;
        if (paths > EXP001_MAX_MULTIPATH_PATHS) paths = EXP001_MAX_MULTIPATH_PATHS;

        double sum_sq_gains = 1.0;
        for (p = 1u; p < paths; ++p) {
            sum_sq_gains += config->multipath_gains[p] * config->multipath_gains[p];
        }
        double norm_factor = 1.0 / sqrt(sum_sq_gains);

        for (i = 0u; i < num_samples; ++i) {
            double val = (double)src[i];
            for (p = 1u; p < paths; ++p) {
                double delay_s = config->multipath_delays_ms[p] * 1e-3;
                if (delay_s < 0.0) delay_s = 0.0;
                if (delay_s > 0.030) delay_s = 0.030; /* bounded 0-30 ms */
                size_t d_samples = (size_t)floor(delay_s * (double)EXP001_SAMPLE_RATE + 0.5);

                if (i >= d_samples) {
                    val += config->multipath_gains[p] * (double)src[i - d_samples];
                }
            }
            dst[i] = (float)(val * norm_factor);
        }
    }

    /* 2. Band attenuation / erasure */
    if (config->band_atten_f_low_hz > 0.0 &&
        config->band_atten_f_high_hz > config->band_atten_f_low_hz) {
        /*
         * Apply 2nd-order IIR bandstop / notch attenuation centered at (f_low + f_high)/2.
         * For simplicity and determinism, a 3-tap FIR filter or frequency-domain suppression.
         */
        double f_mid = (config->band_atten_f_low_hz + config->band_atten_f_high_hz) / 2.0;
        double w = 2.0 * M_PI * f_mid / (double)EXP001_SAMPLE_RATE;
        double alpha = config->band_atten_factor; /* e.g. 0.1 for 20 dB suppression */

        for (i = 1u; i + 1u < num_samples; ++i) {
            double notch_component = (double)dst[i] - cos(w) * ((double)dst[i - 1] + (double)dst[i + 1]) * 0.5;
            dst[i] = (float)((double)dst[i] - (1.0 - alpha) * notch_component);
        }
    }

    /* Compute signal power for noise additions */
    for (i = 0u; i < current_len; ++i) {
        sig_power += (double)dst[i] * (double)dst[i];
    }
    sig_power /= (double)current_len;
    if (sig_power <= 0.0) sig_power = 1e-6;

    /* 3. AWGN */
    if (config->awgn_snr_db > 0.0) {
        double noise_power = sig_power / pow(10.0, config->awgn_snr_db / 10.0);
        double noise_std = sqrt(noise_power);

        for (i = 0u; i < current_len; ++i) {
            dst[i] += (float)(noise_std * gaussian(&rng_state));
        }
    }

    /* 4. Colored noise (first-order lowpass / pink-ish filtered noise) */
    if (config->colored_noise_snr_db > 0.0) {
        double noise_power = sig_power / pow(10.0, config->colored_noise_snr_db / 10.0);
        double noise_std = sqrt(noise_power);
        double filter_state = 0.0;

        for (i = 0u; i < current_len; ++i) {
            double white = noise_std * gaussian(&rng_state);
            filter_state = 0.9 * filter_state + 0.1 * white;
            dst[i] += (float)filter_state;
        }
    }

    /* 5. Clipping */
    if (config->clipping_threshold > 0.0 && config->clipping_threshold < 1.0) {
        float thresh = (float)config->clipping_threshold;
        for (i = 0u; i < current_len; ++i) {
            if (dst[i] > thresh) dst[i] = thresh;
            if (dst[i] < -thresh) dst[i] = -thresh;
        }
    }

    /* 6. Sample Rate Offset (out-of-place) */
    if (config->sample_rate_offset_ppm != 0.0) {
        float temp_buf[EXP001_PREAMBLE_MAX_SAMPLES * 2u];
        size_t sro_out = 0u;
        size_t copy_len = (current_len < sizeof(temp_buf)/sizeof(temp_buf[0])) ?
                           current_len : sizeof(temp_buf)/sizeof(temp_buf[0]);

        memcpy(temp_buf, dst, copy_len * sizeof(float));
        exp001_status_t rst = exp001_resample_sro(
            temp_buf, copy_len,
            config->sample_rate_offset_ppm,
            dst, dst_capacity,
            &sro_out);
        if (rst == EXP001_OK) {
            current_len = sro_out;
        }
    }

    *out_samples = current_len;
    return EXP001_OK;
}
