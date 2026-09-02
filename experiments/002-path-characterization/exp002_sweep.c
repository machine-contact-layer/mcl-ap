/*
 * MCL-AP Experiment 002 (prototype): real transducer path characterization.
 *
 * Measures the frequency response of an actual speaker -> air -> microphone
 * path using stepped tones of equal amplitude and duration, so that the
 * usable acoustic band of a real device pair is measured rather than assumed.
 *
 * gen  <out.wav>              write the stepped-tone probe
 * anal <capture.wav>          measure per-tone response of a capture
 *
 * LAB / EXPERIMENTAL. Not a normative AP profile.
 */

#include "exp001.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

#define SR            48000u
#define TONE_MS       90u
#define GAP_MS        10u
#define LEAD_MS       200u
#define TONE_SAMPLES  ((SR * TONE_MS) / 1000u)
#define GAP_SAMPLES   ((SR * GAP_MS) / 1000u)
#define LEAD_SAMPLES  ((SR * LEAD_MS) / 1000u)
#define MAXS          960000u
#define AMPLITUDE     0.45

/* Marker chirp so the analyzer can find where the probe actually starts in a
 * capture that has unknown playback latency. Same family as the E3 preamble. */
#define MARK_MS       200u
#define MARK_SAMPLES  ((SR * MARK_MS) / 1000u)
#define MARK_F0       2000.0
#define MARK_F1       6000.0

static const double g_tones[] = {
     300.0,  500.0,  700.0, 1000.0, 1300.0, 1600.0, 2000.0, 2400.0,
    2800.0, 3000.0, 3400.0, 3800.0, 4200.0, 4600.0, 5000.0, 5500.0,
    6000.0, 7000.0, 8000.0, 9000.0, 10000.0, 12000.0
};
#define NTONES (sizeof(g_tones) / sizeof(g_tones[0]))

static float g_buf[MAXS];
static float g_cap[MAXS];

static double goertzel_power(const float *x, size_t n, double f)
{
    double w = 2.0 * 3.14159265358979323846 * f / (double)SR;
    double c = 2.0 * cos(w);
    double s1 = 0.0, s2 = 0.0, s0;
    size_t i;
    for (i = 0u; i < n; ++i) { s0 = (double)x[i] + c * s1 - s2; s2 = s1; s1 = s0; }
    return s1 * s1 + s2 * s2 - c * s1 * s2;
}

static size_t write_probe(float *out)
{
    size_t n = 0u, i, t;

    for (i = 0u; i < LEAD_SAMPLES; ++i) out[n++] = 0.0f;

    /* marker chirp */
    for (i = 0u; i < MARK_SAMPLES; ++i) {
        double frac = (double)i / (double)MARK_SAMPLES;
        double f = MARK_F0 + (MARK_F1 - MARK_F0) * frac;
        double phase = 2.0 * 3.14159265358979323846 *
                       (MARK_F0 * (double)i / (double)SR +
                        0.5 * (MARK_F1 - MARK_F0) * frac * (double)i / (double)SR);
        (void)f;
        out[n++] = (float)(AMPLITUDE * sin(phase));
    }
    for (i = 0u; i < GAP_SAMPLES; ++i) out[n++] = 0.0f;

    for (t = 0u; t < NTONES; ++t) {
        for (i = 0u; i < TONE_SAMPLES; ++i) {
            double env = 1.0;
            /* short raised-cosine edges to avoid transmitting clicks */
            if (i < 240u) env = 0.5 * (1.0 - cos(3.14159265358979323846 * (double)i / 240.0));
            else if (i > TONE_SAMPLES - 240u)
                env = 0.5 * (1.0 - cos(3.14159265358979323846 *
                                       (double)(TONE_SAMPLES - i) / 240.0));
            out[n++] = (float)(AMPLITUDE * env *
                               sin(2.0 * 3.14159265358979323846 * g_tones[t] *
                                   (double)i / (double)SR));
        }
        for (i = 0u; i < GAP_SAMPLES; ++i) out[n++] = 0.0f;
    }
    return n;
}

static size_t find_marker(const float *cap, size_t n)
{
    /* Correlate the marker chirp; returns the sample index just past it. */
    static float ref[MARK_SAMPLES];
    size_t i, off, best = 0u;
    double best_m = -1.0, ref_e = 0.0;

    for (i = 0u; i < MARK_SAMPLES; ++i) {
        double frac = (double)i / (double)MARK_SAMPLES;
        double phase = 2.0 * 3.14159265358979323846 *
                       (MARK_F0 * (double)i / (double)SR +
                        0.5 * (MARK_F1 - MARK_F0) * frac * (double)i / (double)SR);
        ref[i] = (float)sin(phase);
        ref_e += (double)ref[i] * (double)ref[i];
    }
    if (n < MARK_SAMPLES) return 0u;

    for (off = 0u; off + MARK_SAMPLES <= n; off += 2u) {
        double c = 0.0, e = 0.0, mean = 0.0, m;
        for (i = 0u; i < MARK_SAMPLES; ++i) mean += (double)cap[off + i];
        mean /= (double)MARK_SAMPLES;
        for (i = 0u; i < MARK_SAMPLES; ++i) {
            double s = (double)cap[off + i] - mean;
            c += (double)ref[i] * s;
            e += s * s;
        }
        m = (e > 0.0) ? fabs(c) / sqrt(ref_e * e) : 0.0;
        if (m > best_m) { best_m = m; best = off; }
    }
    fprintf(stderr, "marker at %zu (metric %.4f)\n", best, best_m);
    return best + MARK_SAMPLES + GAP_SAMPLES;
}

int main(int argc, char **argv)
{
    if (argc < 3) { printf("usage: exp002_sweep gen|anal <wav>\n"); return 2; }

    if (strcmp(argv[1], "gen") == 0) {
        size_t n = write_probe(g_buf);
        if (exp001_wav_write(argv[2], g_buf, n, SR, 16u, 1u) != EXP001_OK) {
            printf("write failed\n"); return 1;
        }
        printf("wrote %s: %zu samples (%.3f s), %zu tones\n",
               argv[2], n, (double)n / (double)SR, NTONES);
        return 0;
    }

    if (strcmp(argv[1], "anal") == 0) {
        size_t n = 0u, base, t;
        uint32_t sr = 0u; uint16_t bps = 0u, ch = 0u;
        double ref_db = 0.0;
        double noise_floor;

        if (exp001_wav_read(argv[2], g_cap, MAXS, &n, &sr, &bps, &ch) != EXP001_OK) {
            printf("read failed\n"); return 1;
        }
        if (sr != SR) { printf("sample rate %u unsupported\n", sr); return 1; }

        /* PDM microphones carry a large DC component that would otherwise
         * dominate both the marker correlation and every level estimate. */
        {
            size_t i; double mean = 0.0;
            for (i = 0u; i < n; ++i) mean += (double)g_cap[i];
            mean /= (double)n;
            for (i = 0u; i < n; ++i) g_cap[i] = (float)((double)g_cap[i] - mean);
            printf("removed DC offset %.6f\n", mean);
        }

        base = find_marker(g_cap, n);

        /* noise estimate from the lead-in before the marker */
        {
            size_t i, lim = (base > MARK_SAMPLES + 2000u) ? base - MARK_SAMPLES - 2000u : 0u;
            double acc = 0.0; size_t cnt = 0u;
            for (i = 0u; i < lim; ++i) { acc += (double)g_cap[i] * (double)g_cap[i]; cnt++; }
            noise_floor = (cnt > 0u) ? sqrt(acc / (double)cnt) : 0.0;
        }

        printf("capture=%s samples=%zu base=%zu noise_rms=%.6f\n",
               argv[2], n, base, noise_floor);
        printf("\n   tone_Hz   level_dB   rel_3kHz_dB   snr_dB\n");

        {
            double levels[NTONES];
            for (t = 0u; t < NTONES; ++t) {
                size_t start = base + t * (TONE_SAMPLES + GAP_SAMPLES);
                size_t win = TONE_SAMPLES - 960u;   /* skip edges */
                double p;
                if (start + 480u + win > n) { levels[t] = 0.0; continue; }
                p = goertzel_power(g_cap + start + 480u, win, g_tones[t]);
                levels[t] = sqrt(p) / (double)win;
                if (fabs(g_tones[t] - 3000.0) < 1.0) ref_db = levels[t];
            }
            for (t = 0u; t < NTONES; ++t) {
                double l = levels[t];
                double db = (l > 0.0) ? 20.0 * log10(l) : -999.0;
                double rel = (l > 0.0 && ref_db > 0.0) ? 20.0 * log10(l / ref_db) : -999.0;
                double snr = (l > 0.0 && noise_floor > 0.0)
                           ? 20.0 * log10(l / noise_floor) : -999.0;
                printf("%10.0f %10.2f %13.2f %8.2f%s\n",
                       g_tones[t], db, rel, snr,
                       (fabs(g_tones[t] - 3000.0) < 1.0 ||
                        fabs(g_tones[t] - 5000.0) < 1.0) ? "   <== current FSK" : "");
            }
        }
        return 0;
    }

    printf("unknown mode\n");
    return 2;
}
