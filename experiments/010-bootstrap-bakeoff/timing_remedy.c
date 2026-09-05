/*
 * MCL-AP Experiment 010c: does a longer estimation baseline actually fix it?
 *
 * Experiment 010b diagnosed the symbol-rate estimator as a SCORING problem
 * rather than a resolution problem: the search covers +-0.5 around the true
 * 160.0 in 0.05 steps, the right answer is inside the grid, and it is not
 * chosen. Sixteen training bits -- 2560 samples -- is too short a baseline to
 * pin a rate.
 *
 * That is a hypothesis. This tests it on the retained captures before any
 * hardware campaign is spent on it.
 *
 * THE CANDIDATE ESTIMATOR IS BLIND
 *
 * It uses no ground truth, so it is something a receiver can actually run.
 * The metric is the mean decision margin over the WHOLE frame:
 *
 *     score(sps) = mean over bits of | log_ratio(bit, sps) - bias |
 *
 * At the correct rate every symbol is sampled near its centre and the two tone
 * energies separate cleanly, so the mean margin is maximal. At a wrong rate the
 * later symbols straddle boundaries, both tones leak, and the mean margin
 * falls. The estimate therefore uses ~160 bits of evidence instead of 16, which
 * is exactly the change 010b argued for.
 *
 * This is NOT the --sweep diagnostic in error_structure.c. That one scores
 * against the known payload and is an upper bound no receiver can reach. This
 * one scores against nothing but the signal.
 *
 * Usage: timing_remedy --payload <hex> <capture.wav>...
 */

#include "../../src/ap_modem.c"
#include "../../tools/wav_io.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_SAMPLES 960000u
#define TRUE_SPS 160.0f

static int16_t g_pcm[MAX_SAMPLES];
static uint8_t g_truth[MCL_AP_MODEM_MAX_PAYLOAD_BYTES];
static size_t  g_truth_len;

static unsigned g_n;
static double   g_err_before, g_err_after;
static unsigned g_crc_before, g_crc_after;
static unsigned g_improved, g_worsened;

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int parse_payload(const char *hex)
{
    size_t n = 0u;
    int hi = -1;
    for (; *hex != '\0'; ++hex) {
        int v = hexval((unsigned char)*hex);
        if (v < 0) return -1;
        if (hi < 0) { hi = v; continue; }
        if (n >= sizeof(g_truth)) return -1;
        g_truth[n++] = (uint8_t)((hi << 4) | v);
        hi = -1;
    }
    if (hi >= 0 || n == 0u) return -1;
    g_truth_len = n;
    return 0;
}

/* Mean |margin| over `bits` symbols starting at `start_t`, at rate `sps`. */
static float exp010c_mean_margin(const int16_t *fsk, size_t fsk_len,
                         const mcl_ap_modem_config_t *cfg,
                         float start_t, float sps, float bias, size_t bits)
{
    double total = 0.0;
    size_t counted = 0u, bit;

    for (bit = 0u; bit < bits; ++bit) {
        float t0 = start_t + (float)bit * sps;
        size_t st = window_index(t0);
        size_t ln = window_index(sps);
        float soft;

        if (t0 < 0.0f || st + ln > fsk_len) break;
        soft = log_ratio(fsk, st, ln, cfg->fsk_freq_0_hz, cfg->fsk_freq_1_hz)
               - bias;
        total += (soft < 0.0f) ? -(double)soft : (double)soft;
        counted++;
    }
    if (counted == 0u) return -1e30f;
    return (float)(total / (double)counted);
}

/* Decode at a given (phase, sps) and report whether the CRC passes. */
static int crc_passes(const int16_t *fsk, size_t fsk_len,
                      const mcl_ap_modem_config_t *cfg,
                      float start_t, float sps, float bias)
{
    uint8_t demod[MCL_AP_MODEM_HEADER_BYTES + MCL_AP_MODEM_MAX_PAYLOAD_BYTES];
    size_t bytes = MCL_AP_MODEM_HEADER_BYTES + g_truth_len;
    size_t bit;
    uint16_t received, computed;

    memset(demod, 0, sizeof(demod));
    for (bit = 0u; bit < bytes * 8u; ++bit) {
        float t0 = start_t + (float)bit * sps;
        size_t st = window_index(t0);
        size_t ln = window_index(sps);
        if (t0 < 0.0f || st + ln > fsk_len) return 0;
        if (log_ratio(fsk, st, ln, cfg->fsk_freq_0_hz, cfg->fsk_freq_1_hz)
            - bias > 0.0f) {
            demod[bit / 8u] |= (uint8_t)(1u << (7u - (unsigned)(bit % 8u)));
        }
    }
    if (demod[0] != (uint8_t)g_truth_len) return 0;
    received = (uint16_t)(((uint16_t)demod[1] << 8) | demod[2]);
    computed = mcl_ap_modem_crc16(demod + MCL_AP_MODEM_HEADER_BYTES,
                                  g_truth_len);
    return (received == computed) ? 1 : 0;
}

static void analyse(const char *path)
{
    static mcl_ap_modem_scratch_t scratch;
    mcl_ap_modem_config_t cfg;
    size_t count = 0u, ref_len, index = 0u, fsk_start, fsk_len, frame_bits;
    const int16_t *fsk;
    float correlation = 0.0f, sps0 = 0.0f, bias = 0.0f, start0;
    float best_sps, best_score, cand, start_c;
    int32_t phase = 0;
    double e0, e1;
    int ok0, ok1;

    mcl_ap_modem_default_config(&cfg);
    if (wav_read_pcm16(path, g_pcm, MAX_SAMPLES, &count) != WAV_OK) return;

    ref_len = generate_preamble_iq(&cfg, scratch.ref_i, scratch.ref_q);
    acquire(&cfg, g_pcm, count, scratch.ref_i, scratch.ref_q, ref_len,
            &index, &correlation);
    if (correlation < cfg.detection_threshold) {
        printf("%-46s NOT_ACQUIRED\n", path);
        return;
    }
    fsk_start = index + ref_len;
    fsk = g_pcm + fsk_start;
    fsk_len = count - fsk_start;

    estimate_timing(fsk, fsk_len, (size_t)cfg.training_bits,
                    cfg.fsk_freq_0_hz, cfg.fsk_freq_1_hz, &phase, &sps0, &bias);

    frame_bits = (MCL_AP_MODEM_HEADER_BYTES + g_truth_len) * 8u;
    start0 = (float)phase + (float)cfg.training_bits * sps0;

    /*
     * Refine over the whole frame, blind. The phase offset is recomputed for
     * each candidate rate because the training bits sit before the payload:
     * changing the rate moves where the payload is judged to begin, and holding
     * the old start would measure a different frame rather than a better rate.
     */
    best_sps = sps0;
    best_score = -1e30f;
    for (cand = TRUE_SPS - 0.6f; cand <= TRUE_SPS + 0.6f; cand += 0.01f) {
        float score;
        start_c = (float)phase + (float)cfg.training_bits * cand;
        score = exp010c_mean_margin(fsk, fsk_len, &cfg, start_c, cand, bias, frame_bits);
        if (score > best_score) {
            best_score = score;
            best_sps = cand;
        }
    }

    e0 = (double)sps0 - (double)TRUE_SPS;
    if (e0 < 0.0) e0 = -e0;
    e1 = (double)best_sps - (double)TRUE_SPS;
    if (e1 < 0.0) e1 = -e1;

    ok0 = crc_passes(fsk, fsk_len, &cfg, start0, sps0, bias);
    start_c = (float)phase + (float)cfg.training_bits * best_sps;
    ok1 = crc_passes(fsk, fsk_len, &cfg, start_c, best_sps, bias);

    g_n++;
    g_err_before += e0;
    g_err_after += e1;
    g_crc_before += (unsigned)ok0;
    g_crc_after += (unsigned)ok1;
    if (e1 < e0 - 0.001) g_improved++;
    if (e1 > e0 + 0.001) g_worsened++;

    printf("%-46s  sps %7.3f -> %7.3f   |err| %.3f -> %.3f   crc %s -> %s\n",
           path, (double)sps0, (double)best_sps, e0, e1,
           ok0 ? "ok " : "BAD", ok1 ? "ok " : "BAD");
}

int main(int argc, char **argv)
{
    int i;

    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--payload") == 0 && i + 1 < argc) {
            if (parse_payload(argv[++i]) != 0) {
                fprintf(stderr, "bad --payload\n");
                return 2;
            }
        } else {
            analyse(argv[i]);
        }
    }

    if (g_n == 0u) {
        fprintf(stderr, "nothing analysed\n");
        return 1;
    }

    printf("\n=== whole-frame blind rate refinement, %u captures ===\n", g_n);
    printf("mean |sps error|   before %.4f   after %.4f\n",
           g_err_before / g_n, g_err_after / g_n);
    printf("CRC pass           before %u/%u    after %u/%u\n",
           g_crc_before, g_n, g_crc_after, g_n);
    printf("estimate improved  %u   worsened %u   unchanged %u\n",
           g_improved, g_worsened, g_n - g_improved - g_worsened);
    return 0;
}
