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
/*
 * One scan of the correlation surface, over offsets [lo, hi] at `stride`.
 *
 * WHY THIS IS ONE PASS AND NOT TWO
 *
 * The straightforward form computes the window's mean, then correlates the
 * mean-removed reference against the mean-removed signal -- two passes over
 * every candidate window. On the board that cost 20.9 s for a single decode,
 * which is not a receiver, it is a batch job.
 *
 * Two facts remove the second pass entirely:
 *
 *   1. The signal mean cancels out of the numerator. With a mean-removed
 *      reference, sum((ref - mean_ref) * (x - mean_x)) equals
 *      sum(ref * x) - mean_ref * sum(x), because sum(ref - mean_ref) is zero.
 *      So the mean is needed only through sum(x), which the scan already has.
 *
 *   2. Successive candidate windows differ by exactly one sample of the
 *      strided subsequence, so sum(x) and sum(x^2) slide: drop the sample
 *      leaving, add the sample entering. They are maintained in int64 over
 *      the raw PCM, which is exact -- a float running sum over 3200 squared
 *      samples drifts, and a normalization that drifts turns into a
 *      correlation that drifts.
 *
 * What is left in the inner loop is two multiply-accumulates per tap.
 */
static void scan(const float *ref_i, const float *ref_q,
                 float mean_i, float mean_q, float ref_energy,
                 const int16_t *pcm, size_t count, size_t stride,
                 size_t lo, size_t hi,
                 size_t *best_index, float *best_value)
{
    const float inv = 1.0f / PCM_SCALE;
    int64_t sum = 0;
    int64_t sumsq = 0;
    size_t span = count * stride;   /* samples the window covers */
    size_t i, j;

    *best_index = lo;
    *best_value = -1.0f;

    for (j = 0u; j < count; ++j) {
        int32_t v = pcm[lo + j * stride];
        sum += v;
        sumsq += (int64_t)v * (int64_t)v;
    }

    for (i = lo; ; i += stride) {
        float corr_i = 0.0f;
        float corr_q = 0.0f;
        float sum_scaled = (float)sum * inv;
        float energy;
        float norm;

        for (j = 0u; j < count; ++j) {
            float x = (float)pcm[i + j * stride] * inv;
            corr_i += ref_i[j * stride] * x;
            corr_q += ref_q[j * stride] * x;
        }
        corr_i -= mean_i * sum_scaled;
        corr_q -= mean_q * sum_scaled;

        /* Mean-removed signal energy, from the exact running sums. */
        energy = (float)sumsq * inv * inv
                 - (sum_scaled * sum_scaled) / (float)count;
        if (energy < 0.0f) {
            energy = 0.0f;
        }

        norm = sqrtf(ref_energy * energy);
        if (norm > 1e-12f) {
            float value = sqrtf(corr_i * corr_i + corr_q * corr_q) / norm;
            if (value > *best_value) {
                *best_value = value;
                *best_index = i;
            }
        }

        if (i + stride > hi) {
            break;
        }
        /* Slide by one strided sample. */
        {
            int32_t leaving = pcm[i];
            int32_t entering = pcm[i + span];
            sum += entering - leaving;
            sumsq += (int64_t)entering * (int64_t)entering
                   - (int64_t)leaving * (int64_t)leaving;
        }
    }
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
    size_t lo, hi;

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

    if (coarse_count >= 64u && search >= stride) {
        float coarse_best;
        size_t coarse_index;

        reference_stats(ref_i, ref_q, coarse_count, stride,
                        &mean_i, &mean_q, &energy);
        scan(ref_i, ref_q, mean_i, mean_q, energy,
             pcm, coarse_count, stride, 0u, search,
             &coarse_index, &coarse_best);

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
    scan(ref_i, ref_q, mean_i, mean_q, energy,
         pcm, ref_len, 1u, lo, hi, &best_index, &best);

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
static float margin_threshold(float worst_zero, float worst_one,
                              float mean_midpoint)
{
    if (worst_zero < worst_one) {
        return 0.5f * (worst_zero + worst_one);
    }
    return mean_midpoint;
}

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
        float worst0 = -1e30f;   /* the transmitted 0 that looked most like a 1 */
        float worst1 = 1e30f;    /* the transmitted 1 that looked most like a 0 */
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
                if (ratio > worst0) { worst0 = ratio; }
                count0++;
            } else {
                sum1 += ratio;
                if (ratio < worst1) { worst1 = ratio; }
                count1++;
            }
        }
        if (count0 > 0u && count1 > 0u) {
            const float midpoint =
                0.5f * (sum0 / (float)count0 + sum1 / (float)count1);
            *out_bias = margin_threshold(worst0, worst1, midpoint);
        }
    }
}

/*
 * Where to put the decision threshold between the two tones.
 *
 * The midpoint of the two class means is the obvious answer and it is the
 * wrong one whenever the path does not treat the two tones equally -- which is
 * the normal case, not the exceptional one. Experiment 002 measured 6 kHz
 * sitting at or above the 3 kHz reference on both receivers tested, and a
 * louder tone is also a tighter distribution: the two classes have different
 * spreads, so the midpoint of their means is not the point at which they are
 * equally likely. It sits too close to the noisier class, and that class's tail
 * crosses it.
 *
 * This is not a hypothesis. Re-decoding the failed trials of Experiment 009
 * showed 28 of 29 payload bit errors were a transmitted 0 read as 1 -- 97% in
 * one direction, which is a threshold placement and not noise.
 *
 * So the threshold is placed by MARGIN instead: halfway between the worst 0 and
 * the worst 1 observed in the training sequence, which is the point furthest
 * from both classes' nearest members. The training byte 0x55 is what makes this
 * possible at all -- eight of each symbol, known in advance, at the start of
 * every frame.
 *
 * When the two classes overlap in the training itself, the margin is negative,
 * there is no separating point, and the estimate falls back to the midpoint of
 * means. A frame whose own training does not separate is not a frame this
 * refinement can rescue, and pretending otherwise would place the threshold
 * using an outlier.
 */

/*
 * Demodulate and verify one frame at a GIVEN timing. Split out of
 * mcl_ap_modem_decode so the same code can be run at more than one candidate
 * symbol rate without a second copy of the bit loop -- two copies would make
 * any difference between the first attempt and the retry indistinguishable
 * from a result.
 *
 * Returns the same statuses mcl_ap_modem_decode does, and fills the parts of
 * `info` that depend on the timing it was given.
 */
static mcl_ap_modem_status_t demod_at(
    const mcl_ap_modem_config_t *config,
    const int16_t *fsk,
    size_t fsk_len,
    mcl_ap_modem_scratch_t *scratch,
    uint8_t *out_payload,
    size_t out_capacity,
    float payload_start,
    float sps,
    float bias,
    mcl_ap_modem_rx_t *info)
{
    size_t available_bits, bytes, bit;
    uint8_t declared;
    uint16_t received, computed;

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

/*
 * Mean decision margin over `bits` symbols at a candidate rate. Blind: it
 * scores the SIGNAL and never the payload, so it is something a receiver can
 * run, unlike a sweep scored against known bytes.
 *
 * At the correct rate every symbol is sampled near its centre and the two tone
 * energies separate cleanly, so the mean margin is maximal; at a wrong rate the
 * later symbols straddle boundaries, both tones leak, and the mean falls.
 */
static float mean_margin(const mcl_ap_modem_config_t *config,
                         const int16_t *fsk, size_t fsk_len,
                         float start_t, float sps, float bias, size_t bits)
{
    double total = 0.0;
    size_t counted = 0u, bit;

    for (bit = 0u; bit < bits; ++bit) {
        float t0 = start_t + (float)bit * sps;
        size_t start = window_index(t0);
        size_t len = window_index(sps);
        float soft;

        if (t0 < 0.0f || start + len > fsk_len) {
            break;
        }
        soft = log_ratio(fsk, start, len,
                         config->fsk_freq_0_hz, config->fsk_freq_1_hz) - bias;
        total += (soft < 0.0f) ? -(double)soft : (double)soft;
        counted++;
    }
    if (counted == 0u) {
        return -1e30f;
    }
    return (float)(total / (double)counted);
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
    float payload_start;
    mcl_ap_modem_status_t status;

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
    status = demod_at(config, fsk, fsk_len, scratch, out_payload, out_capacity,
                      payload_start, sps, bias, info);
    if (status == MCL_AP_MODEM_OK) {
        return status;
    }

    /*
     * FIRST ATTEMPT FAILED. REFINE THE SYMBOL RATE OVER THE WHOLE FRAME.
     *
     * estimate_timing() fits the rate to config->training_bits symbols -- 16 of
     * them, 2560 samples at the default rate. Experiment 010b showed that is
     * too short a baseline: the search already covers the true rate in 0.05
     * steps and still returns a 0.4-0.45 sample error, so the right answer was
     * INSIDE THE GRID AND NOT CHOSEN. It was a scoring problem, not a
     * resolution problem.
     *
     * The fix is more evidence, not a finer grid: score a candidate rate by the
     * mean decision margin over the entire frame, ~160 symbols instead of 16.
     * Experiment 011 measured this over air, board to host, on the three
     * major-1 bootstrap objects:
     *
     *     PRESENCE          10 B    9/15  ->  15/15
     *     TRANSPORT_ACCEPT  16 B    8/15  ->  15/15
     *     TRANSPORT_OFFER   17 B    4/15  ->  14/15
     *
     * 21 of 45 to 44 of 45. That is why there is no forward error correction
     * here: the errors were not independent bit noise for a block code to mop
     * up, they were a timing estimate that drifted a frame out of alignment,
     * and 010's error structure said so before any code was written.
     *
     * WHY IT RUNS ONLY ON THE RETRY PATH
     *
     * A frame that already decodes costs nothing extra -- the search is ~120
     * candidates over the frame and is far more expensive than one demodulation
     * pass, which matters on the ESP32-S3 this also runs on. And placing it
     * here makes the change monotone: refinement can turn a failure into a
     * success and can never turn a success into a failure, because a success
     * has already returned.
     *
     * The search is centred on the NOMINAL rate rather than on the estimate,
     * because the estimate is the thing being distrusted.
     */
    {
        float nominal = (float)samples_per_bit();
        float best_sps = sps;
        float best_score = -1e30f;
        float candidate;
        size_t score_bits;

        /*
         * How many symbols to score over. The declared length is not
         * trustworthy here -- the first attempt just failed, and it may have
         * failed by misreading that very byte -- so a fixed budget is used:
         * the header plus a typical Tier-0 object. Scoring far past the end of
         * a short frame would average in silence, which has no margin and
         * flattens the peak the search is looking for.
         */
        score_bits = ((size_t)MCL_AP_MODEM_HEADER_BYTES + 16u) * 8u;
        if (info->payload_bytes > 0u &&
            info->payload_bytes <= MCL_AP_MODEM_MAX_PAYLOAD_BYTES) {
            score_bits = ((size_t)MCL_AP_MODEM_HEADER_BYTES
                          + (size_t)info->payload_bytes) * 8u;
        }

        for (candidate = nominal - 0.6f;
             candidate <= nominal + 0.6f;
             candidate += 0.01f) {
            float start_c = (float)phase
                            + (float)config->training_bits * candidate;
            float score = mean_margin(config, fsk, fsk_len, start_c, candidate,
                                      bias, score_bits);
            if (score > best_score) {
                best_score = score;
                best_sps = candidate;
            }
        }

        if (best_sps != sps) {
            mcl_ap_modem_status_t retry;
            float start_best = (float)phase
                               + (float)config->training_bits * best_sps;

            retry = demod_at(config, fsk, fsk_len, scratch, out_payload,
                             out_capacity, start_best, best_sps, bias, info);
            if (retry == MCL_AP_MODEM_OK) {
                info->samples_per_symbol = best_sps;
                return retry;
            }
            /*
             * The retry did not recover it either. `info` now describes the
             * refined attempt, which is the more informative of the two: it is
             * what a caller looking at received_crc and payload_bytes should
             * see, because it is the best reading the receiver could produce.
             */
            info->samples_per_symbol = best_sps;
            return retry;
        }
    }

    return status;
}
