/*
 * MCL-AP Experiment 001: Known-Waveform — Core Implementation
 *
 * LAB / EXPERIMENTAL waveform. NOT a normative AP profile.
 */

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
                crc = (uint16_t)((crc << 1u) ^ EXP001_CRC16_POLY);
            } else {
                crc = (uint16_t)(crc << 1u);
            }
        }
    }

    return crc;
}

/* ========== Preamble generation ========== */

static size_t gen_lfm_chirp(
    double duration_s, double f_start, double f_end,
    float *out, size_t capacity)
{
    size_t n = (size_t)(duration_s * EXP001_SAMPLE_RATE);
    size_t i;
    double k;

    if (n > capacity) n = capacity;
    if (n == 0u) return 0u;

    k = (f_end - f_start) / duration_s;

    for (i = 0u; i < n; ++i) {
        double t = (double)i / (double)EXP001_SAMPLE_RATE;
        double phase = 2.0 * M_PI * (f_start * t + 0.5 * k * t * t);
        out[i] = (float)sin(phase);
    }

    return n;
}

static size_t gen_zadoff_chu(
    double duration_s, double f_center,
    float *out, size_t capacity)
{
    /*
     * Zadoff-Chu-derived preamble: generate a ZC sequence of length N_zc
     * (prime), then upsample to audio rate by modulating onto f_center.
     * Root index u = 7 (arbitrary prime coprime to N_zc).
     */
    const size_t N_zc = 127u;  /* prime sequence length */
    const size_t u = 7u;
    size_t total_samples = (size_t)(duration_s * EXP001_SAMPLE_RATE);
    size_t samples_per_chip;
    size_t i, chip;

    if (total_samples > capacity) total_samples = capacity;
    if (total_samples == 0u) return 0u;

    samples_per_chip = total_samples / N_zc;
    if (samples_per_chip == 0u) samples_per_chip = 1u;
    total_samples = samples_per_chip * N_zc;
    if (total_samples > capacity) total_samples = capacity;

    for (chip = 0u; chip < N_zc && chip * samples_per_chip < total_samples; ++chip) {
        /* ZC phase for this chip */
        double zc_phase = M_PI * (double)u * (double)chip * (double)(chip + 1u) / (double)N_zc;

        for (i = 0u; i < samples_per_chip; ++i) {
            size_t idx = chip * samples_per_chip + i;
            if (idx >= total_samples) break;
            double t = (double)idx / (double)EXP001_SAMPLE_RATE;
            double carrier = 2.0 * M_PI * f_center * t;
            out[idx] = (float)sin(carrier + zc_phase);
        }
    }

    return total_samples;
}

static size_t gen_pn_mseq(
    double duration_s, double f_center,
    float *out, size_t capacity)
{
    /*
     * PN/m-sequence preamble: 7-bit LFSR (period 127) BPSK modulated
     * onto f_center, repeated to fill duration.
     */
    const size_t mseq_len = 127u;
    uint8_t mseq[127];
    uint8_t lfsr = 0x01u;
    size_t total_samples = (size_t)(duration_s * EXP001_SAMPLE_RATE);
    size_t samples_per_chip;
    size_t chip, i;

    if (total_samples > capacity) total_samples = capacity;
    if (total_samples == 0u) return 0u;

    /* Generate m-sequence from 7-bit LFSR: taps at bits 6,5 (x^7 + x^6 + 1) */
    for (i = 0u; i < mseq_len; ++i) {
        mseq[i] = lfsr & 1u;
        uint8_t feedback = (uint8_t)(((lfsr >> 6u) ^ (lfsr >> 5u)) & 1u);
        lfsr = (uint8_t)((lfsr >> 1u) | (feedback << 6u));
    }

    samples_per_chip = total_samples / mseq_len;
    if (samples_per_chip == 0u) samples_per_chip = 1u;
    total_samples = samples_per_chip * mseq_len;
    if (total_samples > capacity) total_samples = capacity;

    for (chip = 0u; chip < mseq_len && chip * samples_per_chip < total_samples; ++chip) {
        double phase_offset = mseq[chip] ? M_PI : 0.0;

        for (i = 0u; i < samples_per_chip; ++i) {
            size_t idx = chip * samples_per_chip + i;
            if (idx >= total_samples) break;
            double t = (double)idx / (double)EXP001_SAMPLE_RATE;
            out[idx] = (float)sin(2.0 * M_PI * f_center * t + phase_offset);
        }
    }

    return total_samples;
}

static size_t gen_freq_diverse(
    double duration_s, double f_start, double f_end,
    float *out, size_t capacity)
{
    /*
     * Frequency-diverse preamble: 4 equal-duration tone bursts at
     * different frequencies spread across [f_start, f_end], each
     * repeated twice (8 segments total).
     */
    const unsigned n_tones = 4u;
    const unsigned n_repeats = 2u;
    const unsigned n_segments = n_tones * n_repeats;
    size_t total_samples = (size_t)(duration_s * EXP001_SAMPLE_RATE);
    size_t seg_samples;
    unsigned seg;
    size_t i;

    if (total_samples > capacity) total_samples = capacity;
    if (total_samples == 0u) return 0u;

    seg_samples = total_samples / n_segments;
    total_samples = seg_samples * n_segments;
    if (total_samples > capacity) total_samples = capacity;

    for (seg = 0u; seg < n_segments; ++seg) {
        unsigned tone_idx = seg % n_tones;
        double freq = f_start + (double)tone_idx * (f_end - f_start) / (double)(n_tones - 1u);

        for (i = 0u; i < seg_samples; ++i) {
            size_t idx = (size_t)seg * seg_samples + i;
            if (idx >= total_samples) break;
            double t = (double)idx / (double)EXP001_SAMPLE_RATE;
            out[idx] = (float)sin(2.0 * M_PI * freq * t);
        }
    }

    return total_samples;
}

size_t exp001_generate_preamble(
    exp001_preamble_type_t type,
    double duration_s,
    double f_start_hz,
    double f_end_hz,
    float *out_samples,
    size_t out_capacity)
{
    if (out_samples == NULL || out_capacity == 0u || duration_s <= 0.0) {
        return 0u;
    }

    switch (type) {
    case EXP001_PREAMBLE_LFM_CHIRP:
        return gen_lfm_chirp(duration_s, f_start_hz, f_end_hz, out_samples, out_capacity);
    case EXP001_PREAMBLE_ZADOFF_CHU:
        return gen_zadoff_chu(duration_s, (f_start_hz + f_end_hz) / 2.0, out_samples, out_capacity);
    case EXP001_PREAMBLE_PN_MSEQ:
        return gen_pn_mseq(duration_s, (f_start_hz + f_end_hz) / 2.0, out_samples, out_capacity);
    case EXP001_PREAMBLE_FREQ_DIVERSE:
        return gen_freq_diverse(duration_s, f_start_hz, f_end_hz, out_samples, out_capacity);
    default:
        return 0u;
    }
}

/* ========== FSK modulation ========== */

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
            /* Keep phase in [0, 2*pi) to avoid float drift */
            if (phase >= 2.0 * M_PI) {
                phase -= 2.0 * M_PI;
            }
        }
    }

    return total_samples;
}

/* ========== FSK demodulation ========== */

size_t exp001_fsk_demodulate(
    const float *samples,
    size_t num_samples,
    uint8_t *out_payload,
    size_t out_capacity)
{
    /*
     * Goertzel-based FSK demodulator: for each bit period, compute
     * energy at freq_0 and freq_1, decide whichever is larger.
     */
    size_t samples_per_bit = EXP001_SAMPLE_RATE / EXP001_FSK_BAUD;
    size_t total_bits = num_samples / samples_per_bit;
    size_t total_bytes = total_bits / 8u;
    size_t bit_idx;

    if (samples == NULL || out_payload == NULL) {
        return 0u;
    }
    if (total_bytes > out_capacity) {
        total_bytes = out_capacity;
    }
    if (total_bytes == 0u) {
        return 0u;
    }

    memset(out_payload, 0, total_bytes);

    total_bits = total_bytes * 8u;

    for (bit_idx = 0u; bit_idx < total_bits; ++bit_idx) {
        /* Goertzel at freq_0 */
        double w0 = 2.0 * M_PI * EXP001_FSK_FREQ_0 / (double)EXP001_SAMPLE_RATE;
        double coeff0 = 2.0 * cos(w0);
        double s0_prev = 0.0, s0_prev2 = 0.0;

        /* Goertzel at freq_1 */
        double w1 = 2.0 * M_PI * EXP001_FSK_FREQ_1 / (double)EXP001_SAMPLE_RATE;
        double coeff1 = 2.0 * cos(w1);
        double s1_prev = 0.0, s1_prev2 = 0.0;

        double power0, power1;
        size_t s;
        size_t start = bit_idx * samples_per_bit;

        for (s = 0u; s < samples_per_bit; ++s) {
            double x = (double)samples[start + s];
            double temp;

            temp = coeff0 * s0_prev - s0_prev2 + x;
            s0_prev2 = s0_prev;
            s0_prev = temp;

            temp = coeff1 * s1_prev - s1_prev2 + x;
            s1_prev2 = s1_prev;
            s1_prev = temp;
        }

        power0 = s0_prev * s0_prev + s0_prev2 * s0_prev2 - coeff0 * s0_prev * s0_prev2;
        power1 = s1_prev * s1_prev + s1_prev2 * s1_prev2 - coeff1 * s1_prev * s1_prev2;

        if (power1 > power0) {
            size_t byte_idx = bit_idx / 8u;
            unsigned bit_pos = 7u - (unsigned)(bit_idx % 8u);
            out_payload[byte_idx] |= (uint8_t)(1u << bit_pos);
        }
    }

    return total_bytes;
}

/* ========== Frame encode ========== */

size_t exp001_frame_encode(
    const exp001_frame_config_t *config,
    const uint8_t *payload,
    size_t payload_len,
    float *out_samples,
    size_t out_capacity)
{
    size_t pos = 0u;
    size_t preamble_samples;
    uint8_t phy_header[3];
    uint16_t crc;
    size_t header_fsk_samples;
    size_t payload_fsk_samples;
    size_t silence_samples;

    if (config == NULL || payload == NULL || out_samples == NULL) {
        return 0u;
    }
    if (payload_len > EXP001_MAX_PAYLOAD_BYTES || payload_len > 255u) {
        return 0u;
    }

    /* 1. Preamble */
    preamble_samples = exp001_generate_preamble(
        config->preamble_type,
        config->preamble_duration_s,
        config->preamble_f_start_hz,
        config->preamble_f_end_hz,
        out_samples + pos,
        out_capacity - pos);
    pos += preamble_samples;

    /* 2. PHY header: [payload_len (1 byte)] [CRC16 (2 bytes)] as FSK */
    crc = exp001_crc16(payload, payload_len);
    phy_header[0] = (uint8_t)payload_len;
    phy_header[1] = (uint8_t)(crc >> 8u);
    phy_header[2] = (uint8_t)(crc & 0xFFu);

    header_fsk_samples = exp001_fsk_modulate(
        phy_header, 3u,
        out_samples + pos,
        out_capacity - pos);
    if (header_fsk_samples == 0u) return 0u;
    pos += header_fsk_samples;

    /* 3. Payload as FSK */
    payload_fsk_samples = exp001_fsk_modulate(
        payload, payload_len,
        out_samples + pos,
        out_capacity - pos);
    if (payload_fsk_samples == 0u) return 0u;
    pos += payload_fsk_samples;

    /* 4. Silence tail */
    silence_samples = (size_t)(config->silence_duration_s * (double)EXP001_SAMPLE_RATE);
    if (pos + silence_samples > out_capacity) {
        silence_samples = out_capacity - pos;
    }
    {
        size_t i;
        for (i = 0u; i < silence_samples; ++i) {
            out_samples[pos + i] = 0.0f;
        }
    }
    pos += silence_samples;

    return pos;
}

/* ========== Preamble detection ========== */

/*
 * Cross-correlation-based preamble detector.
 * Generate the expected preamble, slide over the signal, find the peak.
 */
static size_t detect_preamble(
    const exp001_frame_config_t *config,
    const float *samples,
    size_t num_samples,
    double *peak_correlation)
{
    float ref_buf[EXP001_PREAMBLE_MAX_SAMPLES];
    size_t ref_len;
    size_t best_offset = 0u;
    double best_corr = -1e30;
    size_t offset;
    double ref_energy = 0.0;
    size_t i;

    ref_len = exp001_generate_preamble(
        config->preamble_type,
        config->preamble_duration_s,
        config->preamble_f_start_hz,
        config->preamble_f_end_hz,
        ref_buf,
        EXP001_PREAMBLE_MAX_SAMPLES);

    if (ref_len == 0u || ref_len > num_samples) {
        *peak_correlation = 0.0;
        return 0u;
    }

    /* Reference energy for normalization */
    for (i = 0u; i < ref_len; ++i) {
        ref_energy += (double)ref_buf[i] * (double)ref_buf[i];
    }
    if (ref_energy == 0.0) ref_energy = 1.0;

    /* Slide and correlate */
    for (offset = 0u; offset <= num_samples - ref_len; ++offset) {
        double corr = 0.0;
        double sig_energy = 0.0;
        for (i = 0u; i < ref_len; ++i) {
            corr += (double)ref_buf[i] * (double)samples[offset + i];
            sig_energy += (double)samples[offset + i] * (double)samples[offset + i];
        }
        if (sig_energy > 0.0) {
            corr /= sqrt(ref_energy * sig_energy);
        }
        if (corr > best_corr) {
            best_corr = corr;
            best_offset = offset;
        }
    }

    *peak_correlation = best_corr;
    return best_offset;
}

/* ========== Frame decode ========== */

exp001_status_t exp001_frame_decode(
    const exp001_frame_config_t *config,
    const float *samples,
    size_t num_samples,
    uint8_t *out_payload,
    size_t out_capacity,
    exp001_decode_result_t *result)
{
    double peak_corr;
    size_t preamble_offset;
    size_t preamble_len;
    size_t fsk_start;
    uint8_t phy_header[3];
    size_t header_demod;
    uint8_t payload_len;
    size_t payload_fsk_samples;
    size_t payload_demod;
    uint16_t received_crc, computed_crc;
    size_t samples_per_bit;

    if (config == NULL || samples == NULL || out_payload == NULL || result == NULL) {
        return EXP001_ERR_INVALID_ARGUMENT;
    }

    memset(result, 0, sizeof(*result));

    /* Detect preamble */
    preamble_offset = detect_preamble(config, samples, num_samples, &peak_corr);
    preamble_len = (size_t)(config->preamble_duration_s * (double)EXP001_SAMPLE_RATE);

    if (peak_corr < 0.5) {
        return EXP001_ERR_PREAMBLE_NOT_FOUND;
    }

    result->preamble_end_sample = preamble_offset + preamble_len;

    /* FSK data starts right after preamble */
    fsk_start = result->preamble_end_sample;
    if (fsk_start >= num_samples) {
        return EXP001_ERR_SYNC_FAILURE;
    }

    samples_per_bit = EXP001_SAMPLE_RATE / EXP001_FSK_BAUD;

    /* Demodulate PHY header (3 bytes = 24 bits) */
    if (fsk_start + 24u * samples_per_bit > num_samples) {
        return EXP001_ERR_SYNC_FAILURE;
    }

    header_demod = exp001_fsk_demodulate(
        samples + fsk_start,
        24u * samples_per_bit,
        phy_header, 3u);

    if (header_demod != 3u) {
        return EXP001_ERR_SYNC_FAILURE;
    }

    payload_len = phy_header[0];
    received_crc = (uint16_t)((uint16_t)phy_header[1] << 8u) | (uint16_t)phy_header[2];
    result->received_crc = received_crc;
    result->payload_start_sample = fsk_start + 24u * samples_per_bit;

    if (payload_len == 0u || payload_len > out_capacity) {
        return EXP001_ERR_PAYLOAD_TOO_LARGE;
    }

    /* Demodulate payload */
    payload_fsk_samples = (size_t)payload_len * 8u * samples_per_bit;
    if (result->payload_start_sample + payload_fsk_samples > num_samples) {
        return EXP001_ERR_SYNC_FAILURE;
    }

    payload_demod = exp001_fsk_demodulate(
        samples + result->payload_start_sample,
        payload_fsk_samples,
        out_payload, (size_t)payload_len);

    result->payload_bytes = payload_demod;

    /* CRC check */
    computed_crc = exp001_crc16(out_payload, payload_demod);
    result->computed_crc = computed_crc;
    result->crc_valid = (received_crc == computed_crc) ? 1u : 0u;

    if (!result->crc_valid) {
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
        return EXP001_ERR_INVALID_ARGUMENT;  /* only 16-bit supported for now */
    }

    f = fopen(path, "wb");
    if (f == NULL) {
        return EXP001_ERR_FILE_IO;
    }

    data_size = (uint32_t)(num_samples * channels * (bits_per_sample / 8u));
    file_size = 36u + data_size;
    block_align = (uint16_t)(channels * (bits_per_sample / 8u));
    byte_rate = sample_rate * (uint32_t)block_align;

    /* RIFF header */
    fwrite("RIFF", 1, 4, f);
    wav_write_u32(f, file_size);
    fwrite("WAVE", 1, 4, f);

    /* fmt chunk */
    fwrite("fmt ", 1, 4, f);
    wav_write_u32(f, 16u);             /* chunk size */
    wav_write_u16(f, 1u);              /* PCM format */
    wav_write_u16(f, channels);
    wav_write_u32(f, sample_rate);
    wav_write_u32(f, byte_rate);
    wav_write_u16(f, block_align);
    wav_write_u16(f, bits_per_sample);

    /* data chunk */
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

    /* RIFF header */
    fread(chunk_id, 1, 4, f);
    if (memcmp(chunk_id, "RIFF", 4) != 0) {
        fclose(f);
        return EXP001_ERR_WAV_FORMAT;
    }
    wav_read_u32(f);  /* file size */
    fread(chunk_id, 1, 4, f);
    if (memcmp(chunk_id, "WAVE", 4) != 0) {
        fclose(f);
        return EXP001_ERR_WAV_FORMAT;
    }

    /* fmt chunk */
    fread(chunk_id, 1, 4, f);
    if (memcmp(chunk_id, "fmt ", 4) != 0) {
        fclose(f);
        return EXP001_ERR_WAV_FORMAT;
    }
    chunk_size = wav_read_u32(f);
    audio_format = wav_read_u16(f);
    if (audio_format != 1u) {
        fclose(f);
        return EXP001_ERR_WAV_FORMAT;  /* non-PCM */
    }
    *channels = wav_read_u16(f);
    *sample_rate = wav_read_u32(f);
    wav_read_u32(f);  /* byte rate */
    wav_read_u16(f);  /* block align */
    *bits_per_sample = wav_read_u16(f);

    /* Skip extra fmt bytes if any */
    if (chunk_size > 16u) {
        fseek(f, (long)(chunk_size - 16u), SEEK_CUR);
    }

    /* data chunk */
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

/* ========== Impairment harness ========== */

/* Simple deterministic xorshift32 PRNG */
static uint32_t xorshift32(uint32_t *state)
{
    uint32_t x = *state;
    x ^= x << 13u;
    x ^= x >> 17u;
    x ^= x << 5u;
    *state = x;
    return x;
}

/* Box-Muller for Gaussian noise from uniform RNG */
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
    float *samples,
    size_t num_samples)
{
    size_t i;
    uint32_t rng_state;

    if (config == NULL || samples == NULL) {
        return EXP001_ERR_INVALID_ARGUMENT;
    }

    rng_state = config->rng_seed;
    if (rng_state == 0u) rng_state = 1u;  /* xorshift can't start at 0 */

    /* AWGN */
    if (config->awgn_snr_db > 0.0) {
        /* Compute signal power */
        double sig_power = 0.0;
        double noise_power, noise_std;

        for (i = 0u; i < num_samples; ++i) {
            sig_power += (double)samples[i] * (double)samples[i];
        }
        sig_power /= (double)num_samples;

        noise_power = sig_power / pow(10.0, config->awgn_snr_db / 10.0);
        noise_std = sqrt(noise_power);

        for (i = 0u; i < num_samples; ++i) {
            samples[i] += (float)(noise_std * gaussian(&rng_state));
        }
    }

    /* Clipping */
    if (config->clipping_threshold > 0.0 && config->clipping_threshold < 1.0) {
        float thresh = (float)config->clipping_threshold;
        for (i = 0u; i < num_samples; ++i) {
            if (samples[i] > thresh) samples[i] = thresh;
            if (samples[i] < -thresh) samples[i] = -thresh;
        }
    }

    /* Sample rate offset (phase-based resampling approximation) */
    if (config->sample_rate_offset_ppm != 0.0) {
        /*
         * For small offsets, approximate by linearly interpolating from a
         * shifted timeline. This is a first-order approximation suitable
         * for the initial deterministic impairment set.
         */
        double ratio = 1.0 + config->sample_rate_offset_ppm * 1e-6;
        /* In-place: work backwards to avoid overwrite */
        if (ratio > 1.0) {
            /* Effective signal is shorter: some samples at end become silence */
            size_t j;
            for (j = num_samples; j > 0u; --j) {
                double src_idx = (double)(j - 1u) * ratio;
                size_t idx0 = (size_t)src_idx;
                double frac = src_idx - (double)idx0;
                if (idx0 + 1u < num_samples) {
                    samples[j - 1u] = (float)((1.0 - frac) * (double)samples[idx0] +
                                              frac * (double)samples[idx0 + 1u]);
                } else if (idx0 < num_samples) {
                    samples[j - 1u] = samples[idx0];
                } else {
                    samples[j - 1u] = 0.0f;
                }
            }
        } else {
            /* Effective signal is longer: iterate forward */
            size_t j;
            for (j = 0u; j < num_samples; ++j) {
                double src_idx = (double)j * ratio;
                size_t idx0 = (size_t)src_idx;
                double frac = src_idx - (double)idx0;
                if (idx0 + 1u < num_samples) {
                    samples[j] = (float)((1.0 - frac) * (double)samples[idx0] +
                                         frac * (double)samples[idx0 + 1u]);
                } else if (idx0 < num_samples) {
                    samples[j] = samples[idx0];
                } else {
                    samples[j] = 0.0f;
                }
            }
        }
    }

    return EXP001_OK;
}
