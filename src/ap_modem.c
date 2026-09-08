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
    config->refinement_guard_samples = 0u;
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
static double chirp_rate(const mcl_ap_modem_config_t *config)
{
    return ((double)config->preamble_f_end_hz
            - (double)config->preamble_f_start_hz)
           / (double)config->preamble_duration_s;
}

/*
 * `k` is loop-invariant, and on a part without a double-precision FPU the
 * divide that produces it is a called routine rather than an instruction.
 * Computing it per sample cost one such call per sample for a value that
 * cannot change. chirp_phase_at takes it precomputed; chirp_phase keeps the
 * original signature for callers outside the generation loop, and both
 * produce the same double arithmetic in the same order as before.
 */
static float chirp_phase_at(const mcl_ap_modem_config_t *config, double k,
                            size_t i)
{
    double t = (double)i / (double)MCL_AP_MODEM_SAMPLE_RATE_HZ;
    return (float)(2.0 * M_PI
                   * ((double)config->preamble_f_start_hz * t + 0.5 * k * t * t));
}

static float chirp_phase(const mcl_ap_modem_config_t *config, size_t i)
{
    return chirp_phase_at(config, chirp_rate(config), i);
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
    double k = chirp_rate(config);

    for (i = 0u; i < n; ++i) {
        float phase = chirp_phase_at(config, k, i);
        out_i[i] = sinf(phase);
        if (out_q != NULL) {
            out_q[i] = cosf(phase);
        }
    }
    return n;
}

/* See the cache note on mcl_ap_modem_scratch_t. */
#define MCL_AP_REF_CACHE_MAGIC 0x41505246u   /* "APRF" */

/* Defined with the acquisition pass below; the cache fills its results in
   at the moment the reference they describe is built. */
static void reference_stats(const float *ref_i, const float *ref_q,
                            size_t count, size_t stride,
                            float *mean_i, float *mean_q, float *energy);

static size_t preamble_iq_cached(const mcl_ap_modem_config_t *config,
                                 mcl_ap_modem_scratch_t *scratch)
{
    size_t n;

    if (scratch->cache_magic == MCL_AP_REF_CACHE_MAGIC &&
        scratch->cached_ref_len != 0u &&
        scratch->cached_f_start_hz == config->preamble_f_start_hz &&
        scratch->cached_f_end_hz == config->preamble_f_end_hz &&
        scratch->cached_duration_s == config->preamble_duration_s &&
        (size_t)scratch->cached_ref_len == preamble_length(config)) {
        return (size_t)scratch->cached_ref_len;
    }

    n = generate_preamble_iq(config, scratch->ref_i, scratch->ref_q);
    if (n == 0u) {
        scratch->cache_magic = 0u;
        scratch->cached_ref_len = 0u;
        return 0u;
    }
    /*
     * The statistics belong to the reference that has just been replaced, so
     * they are recomputed here rather than left to be noticed later. There is
     * no separate validity flag for them: they are valid exactly when the
     * reference is, which is the only relationship worth maintaining.
     */
    reference_stats(scratch->ref_i, scratch->ref_q,
                    n / MCL_AP_MODEM_COARSE_TAP_STRIDE,
                    MCL_AP_MODEM_COARSE_TAP_STRIDE,
                    &scratch->cached_coarse_mean_i,
                    &scratch->cached_coarse_mean_q,
                    &scratch->cached_coarse_energy);
    reference_stats(scratch->ref_i, scratch->ref_q, n, 1u,
                    &scratch->cached_fine_mean_i,
                    &scratch->cached_fine_mean_q,
                    &scratch->cached_fine_energy);
    scratch->cache_magic = MCL_AP_REF_CACHE_MAGIC;
    scratch->cached_ref_len = (uint32_t)n;
    scratch->cached_f_start_hz = config->preamble_f_start_hz;
    scratch->cached_f_end_hz = config->preamble_f_end_hz;
    scratch->cached_duration_s = config->preamble_duration_s;
    return n;
}

mcl_ap_modem_status_t mcl_ap_modem_prepare(
    const mcl_ap_modem_config_t *config,
    mcl_ap_modem_scratch_t *scratch)
{
    if (config == NULL || scratch == NULL) {
        return MCL_AP_MODEM_ERR_INVALID_ARGUMENT;
    }
    return preamble_iq_cached(config, scratch) != 0u
               ? MCL_AP_MODEM_OK
               : MCL_AP_MODEM_ERR_INVALID_ARGUMENT;
}

void mcl_ap_modem_scratch_invalidate(mcl_ap_modem_scratch_t *scratch)
{
    if (scratch == NULL) {
        return;
    }
    scratch->cache_magic = 0u;
    scratch->cached_ref_len = 0u;
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
 * One scan of the correlation surface, over offsets [lo, hi]. `tap_stride`
 * controls how many reference/input samples score one candidate;
 * `offset_step` controls how densely candidate starts are tested.
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
 * The original implementation coupled those two strides so window energy
 * could slide by one tap. That forced a receiver to spend 1/9 of the full
 * reference on every 1/9-position candidate. Decoupling them makes the
 * candidate grid fine enough to catch the chirp while scoring it with 1/18
 * of the reference.
 *
 * Energy still slides rather than being recomputed. With an 18-sample tap
 * stride and a nine-sample candidate step there are two interleaved lanes:
 * candidates 0,18,36... share one strided window, and 9,27,45... share the
 * other. Each lane drops and adds one sample when it advances. This matters
 * on a 32-bit target: recomputing sum-of-squares put a 64-bit multiply back
 * inside every tap and was measured to be slower than the denser search.
 */
#define MCL_AP_SCAN_MAX_LANES 16u
/* The retained 74-file corpus puts every real preamble at >=0.479970 in the
   sparse pass and every no-preamble refusal at <=0.199474. At the default
   0.40 final threshold, 5/8 is 0.25: inside that measured gap, with room on
   both sides. Scaling with the caller's threshold preserves the meaning of a
   deliberately stricter or looser detector. */
#define MCL_AP_COARSE_GATE_RATIO 0.625f

static void scan(const float *ref_i, const float *ref_q,
                 float mean_i, float mean_q, float ref_energy,
                 const int16_t *pcm, size_t count, size_t tap_stride,
                 size_t offset_step,
                 size_t lo, size_t hi,
                 size_t *best_index, float *best_value)
{
    const float inv = 1.0f / PCM_SCALE;
    int64_t lane_sum[MCL_AP_SCAN_MAX_LANES] = {0};
    int64_t lane_sumsq[MCL_AP_SCAN_MAX_LANES] = {0};
    uint8_t lane_ready[MCL_AP_SCAN_MAX_LANES] = {0};
    size_t lane_count = 0u;
    size_t i, j;

    if (offset_step != 0u && tap_stride % offset_step == 0u) {
        lane_count = tap_stride / offset_step;
        if (lane_count > MCL_AP_SCAN_MAX_LANES) {
            lane_count = 0u;
        }
    }

    *best_index = lo;
    *best_value = -1.0f;

    for (i = lo; ; i += offset_step) {
        size_t lane = (lane_count != 0u)
                          ? ((i - lo) / offset_step) % lane_count
                          : 0u;
        int64_t sum = 0;
        int64_t sumsq = 0;
        float corr_i = 0.0f;
        float corr_q = 0.0f;
        float sum_scaled;
        float energy;
        float norm;

        if (lane_count != 0u && lane_ready[lane]) {
            int32_t leaving = pcm[i - tap_stride];
            int32_t entering = pcm[i + (count - 1u) * tap_stride];
            lane_sum[lane] += entering - leaving;
            lane_sumsq[lane] += (int64_t)entering * (int64_t)entering
                                - (int64_t)leaving * (int64_t)leaving;
            sum = lane_sum[lane];
            sumsq = lane_sumsq[lane];
        } else {
            for (j = 0u; j < count; ++j) {
                int32_t raw = pcm[i + j * tap_stride];
                sum += raw;
                sumsq += (int64_t)raw * (int64_t)raw;
            }
            if (lane_count != 0u) {
                lane_sum[lane] = sum;
                lane_sumsq[lane] = sumsq;
                lane_ready[lane] = 1u;
            }
        }

        for (j = 0u; j < count; ++j) {
            int32_t raw = pcm[i + j * tap_stride];
            float x = (float)raw * inv;
            corr_i += ref_i[j * tap_stride] * x;
            corr_q += ref_q[j * tap_stride] * x;
        }
        sum_scaled = (float)sum * inv;
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

        if (i + offset_step > hi) {
            break;
        }
    }
}

/*
 * Acquisition: sparse search followed by a three-point full-rate check.
 *
 * An exhaustive full-rate search is ~9600 multiply-accumulates per candidate
 * offset across tens of thousands of offsets -- seconds of work on a 240 MHz
 * microcontroller. An 18-sample tap stride and nine-sample candidate step use
 * 1/162 of that coarse multiply-accumulate count.
 *
 * The candidate grid is nine samples (0.19 ms) and downstream symbol timing
 * is estimated independently. Three full-rate scores around the winning grid
 * point reject sparse aliases and locate the local peak closely enough;
 * scanning all nineteen neighbouring sample positions used to cost seconds
 * on the ESP32-S3 without changing any retained verdict. The grid/stride are
 * tested implementation parameters, not protocol constants.
 */
static void acquire(const mcl_ap_modem_config_t *config,
                    const int16_t *pcm, size_t sample_count,
                    const mcl_ap_modem_scratch_t *scratch,
                    const float *ref_i, const float *ref_q, size_t ref_len,
                    size_t *out_index, float *out_coarse_correlation,
                    float *out_correlation, uint8_t *out_refined)
{
    const size_t tap_stride = MCL_AP_MODEM_COARSE_TAP_STRIDE;
    const size_t offset_step = MCL_AP_MODEM_COARSE_OFFSET_STEP;
    size_t search = sample_count;
    size_t coarse_count = ref_len / tap_stride;
    float mean_i, mean_q, energy;
    float best = -1.0f;
    size_t best_index = 0u;
    size_t lo, hi;

    *out_index = 0u;
    *out_coarse_correlation = 0.0f;
    *out_correlation = 0.0f;
    *out_refined = 1u;

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

    if (coarse_count >= 64u && search >= offset_step) {
        float coarse_best;
        float verified_best;
        float verify_left;
        float verify_center;
        float verify_right;
        size_t coarse_index;
        size_t verified_index;
        const size_t verify_radius = 6u;
        size_t verify_lo;
        size_t verify_hi;
        size_t score_index;

        mean_i = scratch->cached_coarse_mean_i;
        mean_q = scratch->cached_coarse_mean_q;
        energy = scratch->cached_coarse_energy;
        scan(ref_i, ref_q, mean_i, mean_q, energy,
             pcm, coarse_count, tap_stride, offset_step, 0u, search,
             &coarse_index, &coarse_best);
        *out_coarse_correlation = (coarse_best > 0.0f) ? coarse_best : 0.0f;

        /* Even the three-point full-rate check is unnecessary in a quiet poll.
           This early refusal can only run below both the sparse gate and the
           final threshold. */
        if (coarse_best < config->detection_threshold
                              * MCL_AP_COARSE_GATE_RATIO) {
            *out_index = coarse_index;
            *out_correlation = *out_coarse_correlation;
            return;
        }

        /* A truncated chirp can have a high sparse score: the missing taps
           are occupied by silence, and the sparse normalization can rank
           that partial overlap above the eventual full preamble.  Holding
           such a sidelobe until a maximum body arrives creates a long deaf
           interval and can step past the real preamble when it is rejected.

           Verify three full-rate positions around the coarse start before
           declaring it a candidate.  Six-sample spacing puts the selected
           start within three samples of the local peak while costing three
           9 600-tap scores, not the former nineteen-position sweep.  On
           rejection the listener may safely advance to the end of the
           searched audio, while a credible preamble can be held until its
           body arrives. */
        verify_lo = (coarse_index > verify_radius)
                        ? coarse_index - verify_radius : 0u;
        verify_hi = coarse_index + verify_radius;
        if (verify_hi > search) { verify_hi = search; }
        scan(ref_i, ref_q,
             scratch->cached_fine_mean_i,
             scratch->cached_fine_mean_q,
             scratch->cached_fine_energy,
             pcm, ref_len, 1u, 1u,
             coarse_index, coarse_index,
             &score_index, &verify_center);
        verified_index = coarse_index;
        verified_best = verify_center;
        verify_left = verify_center;
        verify_right = verify_center;
        if (verify_lo < coarse_index) {
            scan(ref_i, ref_q,
                 scratch->cached_fine_mean_i,
                 scratch->cached_fine_mean_q,
                 scratch->cached_fine_energy,
                 pcm, ref_len, 1u, 1u,
                 verify_lo, verify_lo, &score_index, &verify_left);
            if (verify_left > verified_best) {
                verified_best = verify_left;
                verified_index = verify_lo;
            }
        }
        if (verify_hi > coarse_index) {
            scan(ref_i, ref_q,
                 scratch->cached_fine_mean_i,
                 scratch->cached_fine_mean_q,
                 scratch->cached_fine_energy,
                 pcm, ref_len, 1u, 1u,
                 verify_hi, verify_hi, &score_index, &verify_right);
            if (verify_right > verified_best) {
                verified_best = verify_right;
                verified_index = verify_hi;
            }
        }
        if (verify_lo + verify_radius == coarse_index &&
            verify_hi == coarse_index + verify_radius) {
            const float denominator =
                verify_left - 2.0f * verify_center + verify_right;
            if (denominator < -1e-6f) {
                float delta =
                    0.5f * (verify_left - verify_right) / denominator;
                int32_t shift;
                if (delta < -1.0f) { delta = -1.0f; }
                if (delta > 1.0f) { delta = 1.0f; }
                shift = (int32_t)(delta * (float)verify_radius
                                  + ((delta >= 0.0f) ? 0.5f : -0.5f));
                verified_index =
                    (size_t)((int64_t)coarse_index + (int64_t)shift);
            }
        }
        if (verified_best < config->detection_threshold) {
            *out_index = verified_index;
            *out_correlation = (verified_best > 0.0f)
                                   ? verified_best : 0.0f;
            return;
        }

        if (config->refinement_guard_samples != 0u &&
            (verified_index + ref_len > sample_count ||
             sample_count - (verified_index + ref_len)
                 < (size_t)config->refinement_guard_samples)) {
            *out_index = verified_index;
            *out_correlation = *out_coarse_correlation;
            *out_refined = 0u;
            return;
        }

        /* The interpolated three-point estimate is precise enough for the
           timing estimator that follows. Reusing it avoids a 19-position
           full-rate sweep on every real frame. */
        *out_index = verified_index;
        *out_correlation = verified_best;
        return;
    } else {
        lo = 0u;
        hi = search;
    }

    mean_i = scratch->cached_fine_mean_i;
    mean_q = scratch->cached_fine_mean_q;
    energy = scratch->cached_fine_energy;
    scan(ref_i, ref_q, mean_i, mean_q, energy,
         pcm, ref_len, 1u, 1u, lo, hi, &best_index, &best);

    *out_index = best_index;
    *out_correlation = (best > 0.0f) ? best : 0.0f;
    if (*out_coarse_correlation == 0.0f) {
        *out_coarse_correlation = *out_correlation;
    }
}

/* Goertzel power at one frequency over one symbol window. */
static float goertzel_coeff(const int16_t *pcm, size_t n, float coeff)
{
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

static float tone_coefficient(float frequency)
{
    const float w = TWO_PI * frequency
                    / (float)MCL_AP_MODEM_SAMPLE_RATE_HZ;
    return 2.0f * cosf(w);
}

static float log_ratio_coeff(const int16_t *pcm, size_t start, size_t len,
                             float coeff0, float coeff1)
{
    float p0 = goertzel_coeff(pcm + start, len, coeff0);
    float p1 = goertzel_coeff(pcm + start, len, coeff1);
    return logf(p1 + 1e-30f) - logf(p0 + 1e-30f);
}

#if defined(MCL_AP_MODEM_DIAGNOSTICS)
/* Diagnostic-instrument entry point used by Experiments 010 and 011. The
   production loops call log_ratio_coeff() so the invariant cosine work is
   paid once per decode, not once per symbol. */
static float log_ratio(const int16_t *pcm, size_t start, size_t len,
                       float f0, float f1)
{
    return log_ratio_coeff(pcm, start, len,
                           tone_coefficient(f0), tone_coefficient(f1));
}
#endif

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
    const float coeff0 = tone_coefficient(f0);
    const float coeff1 = tone_coefficient(f1);
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
                    float ratio = log_ratio_coeff(pcm, start, len,
                                                  coeff0, coeff1);
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
            ratio = log_ratio_coeff(pcm, start, len, coeff0, coeff1);
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
    const float coeff0 = tone_coefficient(config->fsk_freq_0_hz);
    const float coeff1 = tone_coefficient(config->fsk_freq_1_hz);

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
        if (log_ratio_coeff(fsk, start, len, coeff0, coeff1) > bias) {
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
static float mean_margin(const int16_t *fsk, size_t fsk_len,
                         float start_t, float sps, float bias, size_t bits,
                         float coeff0, float coeff1)
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
        soft = log_ratio_coeff(fsk, start, len, coeff0, coeff1) - bias;
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
    float coarse_correlation = 0.0f;
    float correlation = 0.0f;
    uint8_t refined = 1u;
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

    ref_len = preamble_iq_cached(config, scratch);
    if (ref_len == 0u || ref_len > sample_count) {
        return MCL_AP_MODEM_ERR_INVALID_ARGUMENT;
    }

    acquire(config, pcm, sample_count, scratch,
            scratch->ref_i, scratch->ref_q, ref_len,
            &index, &coarse_correlation, &correlation, &refined);
    info->acquisition_index = index;
    info->coarse_correlation = coarse_correlation;
    info->correlation = correlation;
    if (refined == 0u) {
        info->refinement_deferred = 1u;
        return MCL_AP_MODEM_ERR_INCOMPLETE;
    }
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
        const float coeff0 = tone_coefficient(config->fsk_freq_0_hz);
        const float coeff1 = tone_coefficient(config->fsk_freq_1_hz);
        int candidate_index;
        int coarse_best_index = 0;
        int fine_first;
        int fine_last;

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

        /* The whole-frame margin is locally smooth in symbol rate. Search the
           original 0.01 grid in two stages: every tenth point first, then the
           21 points within +/-0.10 sample/symbol of the winner. This retains
           the original candidate values and resolution while reducing the
           expensive full-frame scores from about 121 to at most 34. */
        candidate_index = 0;
        for (candidate = nominal - 0.6f;
             candidate <= nominal + 0.6f;
             candidate += 0.01f, ++candidate_index) {
            if (candidate_index % 10 == 0) {
                const float start_c =
                    (float)phase
                    + (float)config->training_bits * candidate;
                const float score =
                    mean_margin(fsk, fsk_len, start_c, candidate,
                                bias, score_bits, coeff0, coeff1);
                if (score > best_score) {
                    best_score = score;
                    best_sps = candidate;
                    coarse_best_index = candidate_index;
                }
            }
        }

        fine_first = coarse_best_index - 10;
        if (fine_first < 0) { fine_first = 0; }
        fine_last = coarse_best_index + 10;
        if (fine_last > 120) { fine_last = 120; }
        candidate_index = 0;
        for (candidate = nominal - 0.6f;
             candidate <= nominal + 0.6f;
             candidate += 0.01f, ++candidate_index) {
            if (candidate_index >= fine_first &&
                candidate_index <= fine_last) {
                const float start_c =
                    (float)phase
                    + (float)config->training_bits * candidate;
                const float score =
                    mean_margin(fsk, fsk_len, start_c, candidate,
                                bias, score_bits, coeff0, coeff1);
                if (score > best_score) {
                    best_score = score;
                    best_sps = candidate;
                }
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
