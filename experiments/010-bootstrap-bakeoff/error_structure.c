/*
 * MCL-AP Experiment 010: bit-error structure over the retained corpus.
 *
 * `spec/ap-bootstrap-requirements-v0.1.md` §6 forbids choosing a coding scheme
 * before the error structure is measured, because the four plausible causes
 * call for four different and non-interchangeable remedies:
 *
 *   independent and symmetric  -> block FEC pays
 *   bursty                     -> interleaving first; repetition wastes airtime
 *   asymmetric                 -> threshold placement, before any coding
 *   growing with bit index     -> timing recovery, and no coding fixes it
 *
 * Adding FEC to a timing problem buys nothing and costs airtime, and airtime is
 * the scarcest thing this bearer has. So this tool measures, with exact ground
 * truth, and chooses nothing.
 *
 * WHY THIS INCLUDES THE IMPLEMENTATION
 *
 * The per-bit soft value is `log_ratio(...) - bias`, and both are internal to
 * the decoder. The alternative to including the translation unit is to
 * reimplement the acquisition, timing search and Goertzel demodulation here --
 * at which point the tool would be measuring a second modem that resembles the
 * first, and any difference between them would be indistinguishable from a
 * result. Including it keeps ONE implementation of the DSP and instruments it.
 *
 * Consequence: this target must NOT also link mcl-ap. Its CMake entry says so.
 *
 * Usage:
 *   error_structure [--payload <hex>] [--f0 <hz>] [--f1 <hz>] <capture.wav>...
 *
 * Ground truth defaults to the E3/E4 PRESENCE vector the campaigns recorded:
 *   00 02 00 00 00 01 01 00 00 01 3C
 */

#include "../../src/ap_modem.c"
#include "../../tools/wav_io.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_SAMPLES 960000u
#define MAX_BITS    ((MCL_AP_MODEM_HEADER_BYTES + MCL_AP_MODEM_MAX_PAYLOAD_BYTES) * 8u)

static int16_t g_pcm[MAX_SAMPLES];

/* Optional overrides so a campaign recorded at a different pair can be read
   with the pair it was recorded at, rather than silently at today's default. */
static float g_f0 = 0.0f;
static float g_f1 = 0.0f;

/*
 * --sweep re-decodes each capture over a fine grid of symbol rate and phase and
 * reports the best achievable error count. It is a DIAGNOSTIC, not a receiver:
 * it uses the known payload to score, which a real receiver does not have. Its
 * only question is whether the errors a real decode makes were recoverable at a
 * different timing estimate -- which separates "the signal did not carry it"
 * from "the receiver estimated the rate slightly wrong".
 */
static int g_sweep = 0;

/* The vector Experiment 003 records as "Expected payload". */
static uint8_t g_truth[MCL_AP_MODEM_MAX_PAYLOAD_BYTES] = {
    0x00u, 0x02u, 0x00u, 0x00u, 0x00u, 0x01u,
    0x01u, 0x00u, 0x00u, 0x01u, 0x3Cu
};
static size_t g_truth_len = 11u;

/* ---- aggregates, across every capture that acquired ---------------------- */

static unsigned long g_err_at_bit[MAX_BITS];   /* errors by bit position */
static unsigned long g_tot_at_bit[MAX_BITS];   /* bits observed at that position */
static unsigned long g_err_0_to_1;             /* sent 0, decided 1 */
static unsigned long g_err_1_to_0;
static unsigned long g_run_hist[33];           /* consecutive-error run lengths */
static double        g_margin_ok_sum, g_margin_err_sum;
static unsigned long g_margin_ok_n,   g_margin_err_n;
static unsigned long g_bits_total, g_errs_total;
static unsigned      g_caps_acquired, g_caps_total, g_caps_clean;
static unsigned long g_sweep_recovered;

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
        int v;
        if (*hex == ' ' || *hex == ',' || *hex == ':') continue;
        v = hexval((unsigned char)*hex);
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

/* The bits the transmitter actually emitted: PHY header then payload. */
static size_t build_expected(uint8_t *out)
{
    uint16_t crc = mcl_ap_modem_crc16(g_truth, g_truth_len);
    out[0] = (uint8_t)g_truth_len;
    out[1] = (uint8_t)(crc >> 8);
    out[2] = (uint8_t)(crc & 0xFFu);
    memcpy(out + 3, g_truth, g_truth_len);
    return MCL_AP_MODEM_HEADER_BYTES + g_truth_len;
}

static int analyse(const char *path)
{
    static mcl_ap_modem_scratch_t scratch;
    static uint8_t expected[MCL_AP_MODEM_HEADER_BYTES + MCL_AP_MODEM_MAX_PAYLOAD_BYTES];
    mcl_ap_modem_config_t config;
    size_t sample_count = 0u, ref_len, index = 0u, fsk_start, fsk_len;
    size_t exp_bytes, exp_bits, bit, run = 0u;
    const int16_t *fsk;
    float correlation = 0.0f, sps = 0.0f, bias = 0.0f, payload_start;
    int32_t phase = 0;
    unsigned long errs = 0u;
    int rc;

    mcl_ap_modem_default_config(&config);
    if (g_f0 > 0.0f) config.fsk_freq_0_hz = g_f0;
    if (g_f1 > 0.0f) config.fsk_freq_1_hz = g_f1;

    rc = wav_read_pcm16(path, g_pcm, MAX_SAMPLES, &sample_count);
    if (rc != WAV_OK) {
        fprintf(stderr, "  SKIP %s (wav status %d)\n", path, rc);
        return -1;
    }
    g_caps_total++;

    /*
     * Through the cache, not generate_preamble_iq directly: acquire() now
     * reads the reference statistics from the scratch, and those are filled in
     * where the reference is built. Calling the generator on its own would
     * leave them stale -- or, on a scratch this function has just zeroed,
     * empty -- and the correlation would be computed against the wrong mean.
     */
    ref_len = preamble_iq_cached(&config, &scratch);
    if (ref_len == 0u || ref_len > sample_count) return -1;

    acquire(&config, g_pcm, sample_count, &scratch,
            scratch.ref_i, scratch.ref_q, ref_len,
            &index, &correlation);
    if (correlation < config.detection_threshold) {
        printf("%-58s  NOT_ACQUIRED  corr=%.3f\n", path, (double)correlation);
        return 0;
    }
    g_caps_acquired++;

    fsk_start = index + ref_len;
    if (fsk_start >= sample_count) return -1;
    fsk = g_pcm + fsk_start;
    fsk_len = sample_count - fsk_start;

    estimate_timing(fsk, fsk_len, (size_t)config.training_bits,
                    config.fsk_freq_0_hz, config.fsk_freq_1_hz,
                    &phase, &sps, &bias);

    payload_start = (float)phase + (float)config.training_bits * sps;
    if (payload_start < 0.0f || window_index(payload_start) >= fsk_len) return -1;

    exp_bytes = build_expected(expected);
    exp_bits = exp_bytes * 8u;

    for (bit = 0u; bit < exp_bits && bit < MAX_BITS; ++bit) {
        float t0 = payload_start + (float)bit * sps;
        size_t start = window_index(t0);
        size_t len = window_index(sps);
        float soft, margin;
        unsigned want, got;

        if (start + len > fsk_len) break;

        soft = log_ratio(fsk, start, len,
                         config.fsk_freq_0_hz, config.fsk_freq_1_hz) - bias;
        got  = (soft > 0.0f) ? 1u : 0u;
        want = (unsigned)((expected[bit / 8u] >> (7u - (bit % 8u))) & 1u);
        margin = (soft < 0.0f) ? -soft : soft;

        g_tot_at_bit[bit]++;
        g_bits_total++;

        if (got != want) {
            errs++;
            g_errs_total++;
            g_err_at_bit[bit]++;
            if (want == 0u) g_err_0_to_1++; else g_err_1_to_0++;
            g_margin_err_sum += (double)margin;
            g_margin_err_n++;
            run++;
        } else {
            g_margin_ok_sum += (double)margin;
            g_margin_ok_n++;
            if (run > 0u) {
                g_run_hist[run < 32u ? run : 32u]++;
                run = 0u;
            }
        }
    }
    if (run > 0u) g_run_hist[run < 32u ? run : 32u]++;

    if (errs == 0u) g_caps_clean++;
    printf("%-58s  corr=%.3f sps=%.3f phase=%+3d  errors=%lu/%u",
           path, (double)correlation, (double)sps, (int)phase,
           errs, (unsigned)exp_bits);

    if (g_sweep) {
        float try_sps, best_sps = sps;
        float ratios[MAX_BITS];
        int32_t try_phase, best_phase = phase;
        unsigned long best = (unsigned long)-1;
        unsigned long best_with_bias = (unsigned long)-1;
        float best_bias = bias;

        for (try_phase = -80; try_phase <= 80; ++try_phase) {
            for (try_sps = 159.0f; try_sps <= 161.0f; try_sps += 0.005f) {
                float pstart = (float)try_phase + (float)config.training_bits * try_sps;
                unsigned long e = 0u;
                if (pstart < 0.0f) continue;
                for (bit = 0u; bit < exp_bits; ++bit) {
                    float t0b = pstart + (float)bit * try_sps;
                    size_t st = window_index(t0b);
                    size_t ln = window_index(try_sps);
                    unsigned want2, got2;
                    if (st + ln > fsk_len) { e = (unsigned long)-1; break; }
                    got2 = (log_ratio(fsk, st, ln, config.fsk_freq_0_hz,
                                      config.fsk_freq_1_hz) - bias > 0.0f) ? 1u : 0u;
                    want2 = (unsigned)((expected[bit / 8u] >> (7u - (bit % 8u))) & 1u);
                    if (got2 != want2) e++;
                }
                if (e < best) { best = e; best_sps = try_sps; best_phase = try_phase; }
            }
        }
        printf("   | best=%lu at sps=%.3f phase=%+3d  drift=%.1f samples",
               best, (double)best_sps, (int)best_phase,
               (double)((best_sps - sps) * (float)exp_bits));

        /*
         * One further oracle question, still deliberately unavailable to a
         * receiver: at the timing that minimised errors above, could a single
         * different decision threshold separate the transmitted classes?
         * This distinguishes a timing failure from a threshold failure on a
         * retained capture.  Candidate thresholds only need to sit at one of
         * the observed ratios because decisions change nowhere in between.
         */
        {
            float pstart = (float)best_phase
                           + (float)config.training_bits * best_sps;
            size_t ratio_count = 0u;
            size_t candidate;

            for (bit = 0u; bit < exp_bits && bit < MAX_BITS; ++bit) {
                float t0b = pstart + (float)bit * best_sps;
                size_t st = window_index(t0b);
                size_t ln = window_index(best_sps);
                if (t0b < 0.0f || st + ln > fsk_len) break;
                ratios[ratio_count++] = log_ratio(
                    fsk, st, ln, config.fsk_freq_0_hz,
                    config.fsk_freq_1_hz);
            }

            for (candidate = 0u; candidate < ratio_count; ++candidate) {
                unsigned long e = 0u;
                float threshold = ratios[candidate];
                for (bit = 0u; bit < ratio_count; ++bit) {
                    unsigned got2 = (ratios[bit] > threshold) ? 1u : 0u;
                    unsigned want2 = (unsigned)((expected[bit / 8u]
                                      >> (7u - (bit % 8u))) & 1u);
                    if (got2 != want2) e++;
                }
                if (e < best_with_bias) {
                    best_with_bias = e;
                    best_bias = threshold;
                }
            }
        }
        printf("  best+bias=%lu at bias=%.3f (training %.3f)",
               best_with_bias, (double)best_bias, (double)bias);
        if (best < errs) g_sweep_recovered += (errs - best);
    }
    printf("\n");
    return 0;
}

int main(int argc, char **argv)
{
    int i;
    unsigned b;
    size_t half;
    unsigned long first_half = 0u, second_half = 0u, fh_tot = 0u, sh_tot = 0u;

    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--payload") == 0 && i + 1 < argc) {
            if (parse_payload(argv[++i]) != 0) {
                fprintf(stderr, "bad --payload\n");
                return 2;
            }
        } else if (strcmp(argv[i], "--sweep") == 0) {
            g_sweep = 1;
        } else if (strcmp(argv[i], "--f0") == 0 && i + 1 < argc) {
            g_f0 = (float)atof(argv[++i]);
        } else if (strcmp(argv[i], "--f1") == 0 && i + 1 < argc) {
            g_f1 = (float)atof(argv[++i]);
        } else {
            analyse(argv[i]);
        }
    }

    if (g_bits_total == 0u) {
        fprintf(stderr, "no bits analysed\n");
        return 1;
    }

    printf("\n=== corpus ===\n");
    printf("captures read      %u\n", g_caps_total);
    printf("acquired           %u\n", g_caps_acquired);
    printf("error-free         %u\n", g_caps_clean);
    printf("bits compared      %lu\n", g_bits_total);
    printf("bit errors         %lu  (%.3f%%)\n", g_errs_total,
           100.0 * (double)g_errs_total / (double)g_bits_total);

    printf("\n=== asymmetry ===\n");
    printf("sent 0, read 1     %lu\n", g_err_0_to_1);
    printf("sent 1, read 0     %lu\n", g_err_1_to_0);
    if (g_errs_total > 0u) {
        unsigned long dom = (g_err_0_to_1 > g_err_1_to_0) ? g_err_0_to_1 : g_err_1_to_0;
        printf("dominant direction %.1f%% of errors\n",
               100.0 * (double)dom / (double)g_errs_total);
    }

    printf("\n=== position ===\n");
    half = 0u;
    for (b = 0u; b < MAX_BITS; ++b) if (g_tot_at_bit[b] > 0u) half = b + 1u;
    for (b = 0u; b < half; ++b) {
        if (b < half / 2u) { first_half += g_err_at_bit[b]; fh_tot += g_tot_at_bit[b]; }
        else               { second_half += g_err_at_bit[b]; sh_tot += g_tot_at_bit[b]; }
    }
    printf("bit span observed  %u\n", (unsigned)half);
    if (fh_tot > 0u && sh_tot > 0u) {
        printf("first  half error  %.3f%%  (%lu/%lu)\n",
               100.0 * (double)first_half / (double)fh_tot, first_half, fh_tot);
        printf("second half error  %.3f%%  (%lu/%lu)\n",
               100.0 * (double)second_half / (double)sh_tot, second_half, sh_tot);
    }
    printf("per-bit errors (index:count, non-zero only)\n  ");
    for (b = 0u; b < half; ++b) {
        if (g_err_at_bit[b] > 0u) printf("%u:%lu ", b, g_err_at_bit[b]);
    }
    printf("\n");

    printf("\n=== burst structure ===\n");
    for (b = 1u; b <= 32u; ++b) {
        if (g_run_hist[b] > 0u) {
            printf("run of %-2u          %lu\n", b, g_run_hist[b]);
        }
    }

    if (g_sweep) {
        printf("\n=== timing sweep ===\n");
        printf("errors removable by a better timing estimate alone  %lu of %lu (%.1f%%)\n",
               g_sweep_recovered, g_errs_total,
               g_errs_total ? 100.0 * (double)g_sweep_recovered / (double)g_errs_total : 0.0);
    }

    printf("\n=== decision margin ===\n");
    if (g_margin_ok_n > 0u) {
        printf("mean |margin| correct  %.4f\n", g_margin_ok_sum / (double)g_margin_ok_n);
    }
    if (g_margin_err_n > 0u) {
        printf("mean |margin| errored  %.4f\n", g_margin_err_sum / (double)g_margin_err_n);
    }
    return 0;
}
