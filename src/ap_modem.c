/*
 * MCL-AP candidate modem. See include/mcl/ap_modem.h for what this is and,
 * more importantly, what it is not.
 *
 * EXPERIMENTAL. Not AP-B0. Not a selected profile.
 *
 * MEMORY DISCIPLINE
 *
 * Nothing here allocates, and no function puts a buffer of consequence on the
 * stack. That is not style: this compiles for an ESP32-S3 whose default task
 * stack is a few kilobytes, and the experiment code it replaces places two
 * 96000-element float arrays in a single frame. The receive working set is
 * one caller-owned struct whose size is visible in the header.
 *
 * The signal is never copied. Correlation reads the caller's PCM buffer
 * directly, with a stride for the decimated pass, so the decimated "copy" of
 * the signal does not exist.
 */

#include "mcl/ap_modem.h"

#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define TWO_PI 6.283185307179586476925287f

/* Preamble energy target per sample, from Experiment 001. The receiver's
   correlation is normalized and therefore scale-invariant, so this sets the
   preamble's amplitude RELATIVE to the FSK section and nothing else. */
#define TARGET_ENERGY_PER_SAMPLE 0.40

/* Full-scale for PCM16. 32767 rather than 32768 so +1.0 and -1.0 are
   symmetric and neither saturates. */
#define PCM_SCALE 32767.0f

/* ------------------------------------------------------------------ util */

static size_t round_to_samples(float seconds)
{
    if (seconds <= 0.0f) {
        return 0u;
    }
    return (size_t)floorf(seconds * (float)MCL_AP_MODEM_SAMPLE_RATE_HZ + 0.5f);
}

static size_t samples_per_bit(void)
{
    return (size_t)(MCL_AP_MODEM_SAMPLE_RATE_HZ / MCL_AP_MODEM_BAUD);
}

static int16_t to_pcm(float value)
{
    float scaled = value * PCM_SCALE;
    if (scaled > PCM_SCALE) {
        scaled = PCM_SCALE;
    }
    if (scaled < -PCM_SCALE) {
        scaled = -PCM_SCALE;
    }
    return (int16_t)(scaled < 0.0f ? scaled - 0.5f : scaled + 0.5f);
}

uint16_t mcl_ap_modem_crc16(const uint8_t *data, size_t length)
{
    uint16_t crc = 0xFFFFu;
    size_t i;
    unsigned bit;

    if (data == NULL) {
        return 0u;
    }
    for (i = 0u; i < length; ++i) {
        crc ^= (uint16_t)((uint16_t)data[i] << 8u);
        for (bit = 0u; bit < 8u; ++bit) {
            if ((crc & 0x8000u) != 0u) {
                crc = (uint16_t)(((uint32_t)crc << 1u) ^ 0x1021u);
            } else {
                crc = (uint16_t)((uint32_t)crc << 1u);
            }
        }
    }
    return crc;
}

void mcl_ap_modem_default_config(mcl_ap_modem_config_t *config)
{
    if (config == NULL) {
        return;
    }
    config->fsk_freq_0_hz = 3000.0f;
    config->fsk_freq_1_hz = 6000.0f;
    config->preamble_f_start_hz = 2000.0f;
    config->preamble_f_end_hz = 6000.0f;
    config->preamble_duration_s = 0.2f;
    config->leading_silence_s = 0.1f;
    config->trailing_silence_s = 0.5f;
    config->detection_threshold = 0.40f;
    config->training_bits = (uint16_t)MCL_AP_MODEM_TRAINING_BITS;
    config->max_search_samples = 0u;   /* the whole buffer */
}

/* ---------------------------------------------------------- the preamble */

/*
 * LFM chirp phase at sample i.
 *
 * Accumulated in double even though the sample path is float. The quadratic
 * term reaches 2*pi*6000*0.2 radians; a float mantissa loses the fractional
 * part of an angle that size well before the chirp ends, and the result is a
 * reference that stops matching the transmitted waveform at its own tail --
 * exactly where a correlation peak is decided.
 */
static float chirp_phase(const mcl_ap_modem_config_t *config, size_t i)
{
    double t = (double)i / (double)MCL_AP_MODEM_SAMPLE_RATE_HZ;
    double k = ((double)config->preamble_f_end_hz
                - (double)config->preamble_f_start_hz)
               / (double)config->preamble_duration_s;
    return (float)(2.0 * M_PI
                   * ((double)config->preamble_f_start_hz * t + 0.5 * k * t * t));
}

static size_t preamble_length(const mcl_ap_modem_config_t *config)
{
    size_t n = round_to_samples(config->preamble_duration_s);
    return (n > MCL_AP_MODEM_MAX_PREAMBLE_SAMPLES)
           ? MCL_AP_MODEM_MAX_PREAMBLE_SAMPLES : n;
}

static size_t generate_preamble_iq(const mcl_ap_modem_config_t *config,
                                   float *out_i, float *out_q)
{
    size_t n = preamble_length(config);
    size_t i;

    for (i = 0u; i < n; ++i) {
        float phase = chirp_phase(config, i);
        out_i[i] = sinf(phase);
        if (out_q != NULL) {
            out_q[i] = cosf(phase);
        }
    }
    return n;
}

/*
 * Energy-equalization scale, computed by generating the chirp once and
 * throwing it away.
 *
 * The alternative is to hold the preamble in a buffer so it can be scaled in
 * place, which is what the experiment code does. Regenerating costs one extra
 * pass of 9600 sinf calls and removes a 38 KB buffer from the transmit path,
 * which on the embedded side is the difference between fitting and not.
 */
static float preamble_scale(const mcl_ap_modem_config_t *config, size_t n)
{
    double energy = 0.0;
    size_t i;

    for (i = 0u; i < n; ++i) {
        double s = (double)sinf(chirp_phase(config, i));
        energy += s * s;
    }
    if (energy <= 0.0) {
        return 1.0f;
    }
    return (float)sqrt(((double)n * TARGET_ENERGY_PER_SAMPLE) / energy);
}

/* --------------------------------------------------------------- encode */

/*
 * FSK, MSB first, phase-continuous across symbol boundaries.
 *
 * Continuity is not decoration. A phase discontinuity at every bit edge
 * spreads energy across the band, and the receiver's Goertzel windows sit
 * exactly on those edges.
 */
static size_t modulate(const uint8_t *bytes, size_t count,
                       float f0, float f1,
                       int16_t *out, size_t capacity, double *phase_state)
{
    const size_t sps = samples_per_bit();
    size_t total = count * 8u * sps;
    size_t bit, s;
    double phase = *phase_state;

    if (total > capacity) {
        return 0u;
    }
    for (bit = 0u; bit < count * 8u; ++bit) {
        size_t byte_index = bit / 8u;
        unsigned bit_position = 7u - (unsigned)(bit % 8u);
        uint8_t value = (uint8_t)((bytes[byte_index] >> bit_position) & 1u);
        double freq = (value == 0u) ? (double)f0 : (double)f1;
        double increment = 2.0 * M_PI * freq
                           / (double)MCL_AP_MODEM_SAMPLE_RATE_HZ;

        for (s = 0u; s < sps; ++s) {
            out[bit * sps + s] = to_pcm((float)sin(phase));
            phase += increment;
            if (phase >= 2.0 * M_PI) {
                phase -= 2.0 * M_PI;
            }
        }
    }
    *phase_state = phase;
    return total;
}

size_t mcl_ap_modem_encoded_samples(const mcl_ap_modem_config_t *config,
                                    size_t payload_bytes)
{
    size_t total;

    if (config == NULL || payload_bytes == 0u ||
        payload_bytes > MCL_AP_MODEM_MAX_PAYLOAD_BYTES) {
        return 0u;
    }
    total = round_to_samples(config->leading_silence_s);
    total += preamble_length(config);
    total += ((size_t)config->training_bits / 8u) * 8u * samples_per_bit();
    total += MCL_AP_MODEM_HEADER_BYTES * 8u * samples_per_bit();
    total += payload_bytes * 8u * samples_per_bit();
    total += round_to_samples(config->trailing_silence_s);
    return total;
}

mcl_ap_modem_status_t mcl_ap_modem_encode(
    const mcl_ap_modem_config_t *config,
    const uint8_t *payload,
    size_t payload_bytes,
    int16_t *out_pcm,
    size_t out_capacity,
    size_t *out_written)
{
    size_t needed;
    size_t pos = 0u;
    size_t lead, preamble_n, i;
    uint8_t training[MCL_AP_MODEM_TRAINING_BITS / 8u];
    uint8_t header[MCL_AP_MODEM_HEADER_BYTES];
    uint16_t crc;
    float scale;
    double phase = 0.0;

    if (config == NULL || payload == NULL || out_pcm == NULL) {
        return MCL_AP_MODEM_ERR_INVALID_ARGUMENT;
    }
    needed = mcl_ap_modem_encoded_samples(config, payload_bytes);
    if (needed == 0u) {
        return MCL_AP_MODEM_ERR_INVALID_ARGUMENT;
    }
    if (needed > out_capacity) {
        return MCL_AP_MODEM_ERR_BUFFER_TOO_SMALL;
    }

    lead = round_to_samples(config->leading_silence_s);
    for (i = 0u; i < lead; ++i) {
        out_pcm[pos++] = 0;
    }

    preamble_n = preamble_length(config);
    if (preamble_n == 0u) {
        return MCL_AP_MODEM_ERR_INVALID_ARGUMENT;
    }
    scale = preamble_scale(config, preamble_n);
    for (i = 0u; i < preamble_n; ++i) {
        out_pcm[pos++] = to_pcm(sinf(chirp_phase(config, i)) * scale);
    }

    if (config->training_bits >= 8u) {
        size_t training_bytes = (size_t)config->training_bits / 8u;
        if (training_bytes > sizeof(training)) {
            training_bytes = sizeof(training);
        }
        for (i = 0u; i < training_bytes; ++i) {
            training[i] = (uint8_t)MCL_AP_MODEM_TRAINING_BYTE;
        }
        pos += modulate(training, training_bytes,
                        config->fsk_freq_0_hz, config->fsk_freq_1_hz,
                        out_pcm + pos, out_capacity - pos, &phase);
    }

    crc = mcl_ap_modem_crc16(payload, payload_bytes);
    header[0] = (uint8_t)payload_bytes;
    header[1] = (uint8_t)(crc >> 8u);
    header[2] = (uint8_t)(crc & 0xFFu);
    pos += modulate(header, MCL_AP_MODEM_HEADER_BYTES,
                    config->fsk_freq_0_hz, config->fsk_freq_1_hz,
                    out_pcm + pos, out_capacity - pos, &phase);

    pos += modulate(payload, payload_bytes,
                    config->fsk_freq_0_hz, config->fsk_freq_1_hz,
                    out_pcm + pos, out_capacity - pos, &phase);

    while (pos < needed) {
        out_pcm[pos++] = 0;
    }

    if (out_written != NULL) {
        *out_written = pos;
    }
    return MCL_AP_MODEM_OK;
}

/* --------------------------------------------------------------- decode */

/*
 * Mean and quadrature energy of the reference, at a stride.
 *
 * A stride of 1 gives the full-rate reference; a stride of DECIMATION gives
 * the decimated one without a second array existing.
 */
static void reference_stats(const float *ref_i, const float *ref_q,
                            size_t count, size_t stride,
                            float *mean_i, float *mean_q, float *energy)
{
    double sum_i = 0.0;
    double sum_q = 0.0;
    double e = 0.0;
    size_t i;

    for (i = 0u; i < count; ++i) {
        sum_i += (double)ref_i[i * stride];
        sum_q += (double)ref_q[i * stride];
    }
    *mean_i = (float)(sum_i / (double)count);
    *mean_q = (float)(sum_q / (double)count);

    for (i = 0u; i < count; ++i) {
        double ci = (double)ref_i[i * stride] - (double)*mean_i;
        double cq = (double)ref_q[i * stride] - (double)*mean_q;
        e += 0.5 * (ci * ci + cq * cq);
    }
    *energy = (e > 0.0) ? (float)e : 1.0f;
}

/*
 * Normalized quadrature correlation at one offset, reading the caller's PCM
 * directly at a stride.
 *
 * Both sequences are mean-removed. A PDM microphone carries a large DC
 * component, and an un-centred correlation measures that offset far more
 * strongly than it measures the chirp -- which produces a confident
 * acquisition at whatever offset happens to be loudest.
 */
static float correlate_at(const float *ref_i, const float *ref_q,
                          float mean_i, float mean_q, float ref_energy,
                          const int16_t *pcm, size_t count, size_t stride)
{
    float corr_i = 0.0f;
    float corr_q = 0.0f;
    float sig_energy = 0.0f;
    float sig_mean = 0.0f;
    float norm;
    size_t i;

    for (i = 0u; i < count; ++i) {
        sig_mean += (float)pcm[i * stride];
    }
    sig_mean /= (float)count;

    for (i = 0u; i < count; ++i) {
        float s = ((float)pcm[i * stride] - sig_mean) / PCM_SCALE;
        corr_i += (ref_i[i * stride] - mean_i) * s;
        corr_q += (ref_q[i * stride] - mean_q) * s;
        sig_energy += s * s;
    }

    norm = sqrtf(ref_energy * sig_energy);
    if (norm <= 1e-12f) {
        return 0.0f;
    }
    return sqrtf(corr_i * corr_i + corr_q * corr_q) / norm;
}

/*
 * Acquisition: coarse search at a stride, then refine at full rate.
 *
 * An exhaustive full-rate search is ~9600 multiply-accumulates per candidate
 * offset across tens of thousands of offsets -- seconds of work on a 240 MHz
 * microcontroller. Striding by 3 divides that by 9.
 *
 * It costs nothing in generality. The refinement pass re-examines every
 * full-rate offset within one stride of the coarse peak, so the result is the
 * peak an exhaustive search would find -- unless the correlation surface has
 * a second peak the coarse pass ranks higher, which is a real ambiguity in
 * the signal rather than an artefact of the resolution.
 *
 * The stride is 3 and not 4 because 4 puts the effective Nyquist limit at
 * exactly 6 kHz, the top of the chirp. 3 puts it at 8 kHz, so the coarse pass
 * sees the whole preamble rather than an aliased version of its top octave.
 */
static void acquire(const mcl_ap_modem_config_t *config,
                    const int16_t *pcm, size_t sample_count,
                    const float *ref_i, const float *ref_q, size_t ref_len,
                    size_t *out_index, float *out_correlation)
{
    const size_t stride = MCL_AP_MODEM_DECIMATION;
    size_t search = sample_count;
    size_t coarse_count = ref_len / stride;
    float mean_i, mean_q, energy;
    float best = -1.0f;
    size_t best_index = 0u;
    size_t lo, hi, i;

    *out_index = 0u;
    *out_correlation = 0.0f;

    if (ref_len == 0u || ref_len > sample_count) {
        return;
    }
    if (config->max_search_samples != 0u &&
        search > (size_t)config->max_search_samples) {
        search = (size_t)config->max_search_samples;
    }
    if (search + ref_len > sample_count) {
        search = sample_count - ref_len;
    }

    if (coarse_count >= 64u) {
        float coarse_best = -1.0f;
        size_t coarse_index = 0u;

        reference_stats(ref_i, ref_q, coarse_count, stride,
                        &mean_i, &mean_q, &energy);
        for (i = 0u; i <= search; i += stride) {
            float value = correlate_at(ref_i, ref_q, mean_i, mean_q, energy,
                                       pcm + i, coarse_count, stride);
            if (value > coarse_best) {
                coarse_best = value;
                coarse_index = i;
            }
        }
        lo = (coarse_index > stride) ? coarse_index - stride : 0u;
        hi = coarse_index + stride;
        if (hi > search) {
            hi = search;
        }
    } else {
        lo = 0u;
        hi = search;
    }

    reference_stats(ref_i, ref_q, ref_len, 1u, &mean_i, &mean_q, &energy);
    for (i = lo; i <= hi; ++i) {
        float value = correlate_at(ref_i, ref_q, mean_i, mean_q, energy,
                                   pcm + i, ref_len, 1u);
        if (value > best) {
            best = value;
            best_index = i;
        }
    }

    *out_index = best_index;
    *out_correlation = (best > 0.0f) ? best : 0.0f;
}

/* Goertzel power at one frequency over one symbol window. */
static float goertzel(const int16_t *pcm, size_t n, float frequency)
{
    float w = TWO_PI * frequency / (float)MCL_AP_MODEM_SAMPLE_RATE_HZ;
    float coeff = 2.0f * cosf(w);
    float s1 = 0.0f;
    float s2 = 0.0f;
    size_t i;

    for (i = 0u; i < n; ++i) {
        float x = (float)pcm[i] / PCM_SCALE;
        float t = coeff * s1 - s2 + x;
        s2 = s1;
        s1 = t;
    }
    return s1 * s1 + s2 * s2 - coeff * s1 * s2;
}

static size_t window_index(float t)
{
    /* A negative boundary is not a sample index, and converting a negative
       float to size_t is undefined. Every caller rejects t < 0 with its own
       test -- but that test runs after this conversion would have happened. */
    if (t < 0.0f) {
        return 0u;
    }
    return (size_t)floorf(t + 0.5f);
}

static float log_ratio(const int16_t *pcm, size_t start, size_t len,
                       float f0, float f1)
{
    float p0 = goertzel(pcm + start, len, f0);
    float p1 = goertzel(pcm + start, len, f1);
    return logf(p1 + 1e-30f) - logf(p0 + 1e-30f);
}

/*
 * Symbol timing and channel bias, both estimated from the training sequence.
 *
 * The bias matters more than it looks. Binary FSK decides by comparing energy
 * at two frequencies, and an acoustic path does not present those two
 * frequencies equally -- Experiment 002 measured one of them 19-21 dB down
 * inside a notch. Centring the decision in log-energy space using equal
 * counts of known 0s and 1s is what lets a receiver work on a tilted path
 * instead of deciding every bit the same way.
 */
static void estimate_timing(const int16_t *pcm, size_t n,
                            size_t training_bits, float f0, float f1,
                            int32_t *out_phase, float *out_sps, float *out_bias)
{
    const float nominal = (float)samples_per_bit();
    float best_score = -1e30f;
    int32_t best_phase = 0;
    float best_sps = nominal;
    int32_t phase;
    float sps;
    size_t b;

    *out_bias = 0.0f;

    if (training_bits == 0u) {
        *out_phase = 0;
        *out_sps = nominal;
        return;
    }

    for (phase = -16; phase <= 16; phase += 2) {
        for (sps = nominal - 0.5f; sps <= nominal + 0.5f; sps += 0.05f) {
            float score = 0.0f;
            for (b = 0u; b < training_bits; ++b) {
                float t0 = (float)phase + (float)b * sps;
                size_t start = window_index(t0);
                size_t len = window_index(sps);
                if (t0 < 0.0f || start + len > n) {
                    score -= 1e6f;
                    continue;
                }
                {
                    float ratio = log_ratio(pcm, start, len, f0, f1);
                    score += ((b & 1u) == 0u) ? -ratio : ratio;
                }
            }
            if (score > best_score) {
                best_score = score;
                best_phase = phase;
                best_sps = sps;
            }
        }
    }

    *out_phase = best_phase;
    *out_sps = best_sps;

    {
        float sum0 = 0.0f;
        float sum1 = 0.0f;
        size_t count0 = 0u;
        size_t count1 = 0u;
        for (b = 0u; b < training_bits; ++b) {
            float t0 = (float)best_phase + (float)b * best_sps;
            size_t start = window_index(t0);
            size_t len = window_index(best_sps);
            float ratio;
            if (t0 < 0.0f || start + len > n) {
                continue;
            }
            ratio = log_ratio(pcm, start, len, f0, f1);
            if ((b & 1u) == 0u) {
                sum0 += ratio;
                count0++;
            } else {
                sum1 += ratio;
                count1++;
            }
        }
        if (count0 > 0u && count1 > 0u) {
            *out_bias = 0.5f * (sum0 / (float)count0 + sum1 / (float)count1);
        }
    }
}

mcl_ap_modem_status_t mcl_ap_modem_decode(
    const mcl_ap_modem_config_t *config,
    const int16_t *pcm,
    size_t sample_count,
    mcl_ap_modem_scratch_t *scratch,
    uint8_t *out_payload,
    size_t out_capacity,
    mcl_ap_modem_rx_t *info)
{
    size_t ref_len;
    size_t index = 0u;
    float correlation = 0.0f;
    size_t fsk_start;
    const int16_t *fsk;
    size_t fsk_len;
    int32_t phase = 0;
    float sps = 0.0f;
    float bias = 0.0f;
    size_t available_bits, bytes, bit;
    uint8_t declared;
    uint16_t received, computed;
    float payload_start;

    if (config == NULL || pcm == NULL || scratch == NULL ||
        out_payload == NULL || info == NULL) {
        return MCL_AP_MODEM_ERR_INVALID_ARGUMENT;
    }
    memset(info, 0, sizeof(*info));

    ref_len = generate_preamble_iq(config, scratch->ref_i, scratch->ref_q);
    if (ref_len == 0u || ref_len > sample_count) {
        return MCL_AP_MODEM_ERR_INVALID_ARGUMENT;
    }

    acquire(config, pcm, sample_count, scratch->ref_i, scratch->ref_q, ref_len,
            &index, &correlation);
    info->acquisition_index = index;
    info->correlation = correlation;
    info->acquired = (correlation >= config->detection_threshold) ? 1u : 0u;
    if (info->acquired == 0u) {
        return MCL_AP_MODEM_ERR_NOT_ACQUIRED;
    }

    fsk_start = index + ref_len;
    if (fsk_start >= sample_count) {
        return MCL_AP_MODEM_ERR_SYNC;
    }
    fsk = pcm + fsk_start;
    fsk_len = sample_count - fsk_start;

    estimate_timing(fsk, fsk_len, (size_t)config->training_bits,
                    config->fsk_freq_0_hz, config->fsk_freq_1_hz,
                    &phase, &sps, &bias);
    info->timing_phase = phase;
    info->samples_per_symbol = sps;

    payload_start = (float)phase + (float)config->training_bits * sps;
    if (payload_start < 0.0f || window_index(payload_start) >= fsk_len) {
        return MCL_AP_MODEM_ERR_SYNC;
    }

    available_bits = (size_t)((float)(fsk_len - window_index(payload_start))
                              / sps);
    bytes = available_bits / 8u;
    if (bytes > sizeof(scratch->demod)) {
        bytes = sizeof(scratch->demod);
    }
    if (bytes < (size_t)MCL_AP_MODEM_HEADER_BYTES + 1u) {
        return MCL_AP_MODEM_ERR_SYNC;
    }

    memset(scratch->demod, 0, bytes);
    for (bit = 0u; bit < bytes * 8u; ++bit) {
        float t0 = payload_start + (float)bit * sps;
        size_t start = window_index(t0);
        size_t len = window_index(sps);

        if (start + len > fsk_len) {
            break;
        }
        if (log_ratio(fsk, start, len,
                      config->fsk_freq_0_hz, config->fsk_freq_1_hz) > bias) {
            scratch->demod[bit / 8u] |=
                (uint8_t)(1u << (7u - (unsigned)(bit % 8u)));
        }
    }

    declared = scratch->demod[0];
    received = (uint16_t)(((uint16_t)scratch->demod[1] << 8u)
                          | (uint16_t)scratch->demod[2]);
    info->received_crc = received;
    info->payload_bytes = declared;

    if (declared == 0u || declared > out_capacity ||
        declared > MCL_AP_MODEM_MAX_PAYLOAD_BYTES) {
        return MCL_AP_MODEM_ERR_PAYLOAD;
    }
    if (bytes < (size_t)MCL_AP_MODEM_HEADER_BYTES + (size_t)declared) {
        return MCL_AP_MODEM_ERR_SYNC;
    }

    memcpy(out_payload, scratch->demod + MCL_AP_MODEM_HEADER_BYTES, declared);
    computed = mcl_ap_modem_crc16(out_payload, declared);
    info->computed_crc = computed;
    info->crc_valid = (received == computed) ? 1u : 0u;

    if (info->crc_valid == 0u) {
        return MCL_AP_MODEM_ERR_CRC;
    }
    return MCL_AP_MODEM_OK;
}
