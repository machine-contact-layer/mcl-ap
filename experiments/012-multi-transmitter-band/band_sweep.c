/*
 * Experiment 012: which FSK pair survives EVERY transmitter in the room.
 *
 * LAB INSTRUMENT. Not part of the MCL library and not conformance code.
 *
 * WHY THIS EXPERIMENT EXISTS
 *
 * AP-BOOTSTRAP-1 specifies 3000 Hz for bit 0 and 6000 Hz for bit 1. That pair
 * was chosen by measuring ONE transmitter -- the DFR1154's amplifier and
 * speaker -- and it is a good pair for that transmitter. The profile cannot
 * become Stable on one transmitter, and the reason is not procedural: the
 * laptop speaker in this same room has a measured notch around 3 kHz, and
 * 3 kHz is one of the two tones. A profile whose bit-0 tone lands in a common
 * loudspeaker's notch is a profile that will be reported as broken by its
 * second implementer.
 *
 * So the question this answers is not "what is the best band" but "what is the
 * best band FOR THE WORST PATH", which is a different optimisation and gives a
 * different answer.
 *
 * WHY MINIMAX AND NOT AVERAGE
 *
 * Averaging the response over transmitters rewards a band that is superb on
 * one path and inaudible on another, because a large number and a small number
 * average to an acceptable one. Bootstrap contact does not average: a stranger
 * arrives on ONE path, and if that path cannot carry the tone the contact does
 * not happen. The score is therefore the WEAKER of the two tones on the WORST
 * path, and the winner is the pair that maximises it.
 *
 * WHY THE CANDIDATES ARE MULTIPLES OF 300 Hz
 *
 * 300 baud at 48 kHz is 160 samples per symbol. Only a multiple of 300 Hz puts
 * a whole number of cycles in a symbol; anything else leaves a phase
 * discontinuity at every symbol boundary and spreads energy the correlator
 * then has to reject. Candidates that the modem could never adopt are not
 * measured.
 *
 * WHAT IT DOES
 *
 *   band_sweep slots  <capture.wav> <label> [options]   per-tone level, TSV
 *   band_sweep minimax <a.tsv> [b.tsv ...]              choose the pair
 *
 * `slots` locates the marker tone the board emits before the ladder, then
 * measures each tone during its own slot. It ALSO reports what that tone
 * measured in every other slot -- see the leakage note at own_vs_leak_db.
 *
 * Exit codes: 0 ok, 2 bad usage, 3 file error, 4 marker not found.
 */

#define _CRT_SECURE_NO_WARNINGS

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../tools/wav_io.h"

#define SAMPLE_RATE 48000.0
#define MAX_SAMPLES (48000 * 10)
#define MAX_TONES 64
#define MAX_PATHS 8

/* Envelope resolution for the alignment fit. 5 ms hops, 20 ms windows. */
#define HOP_SAMPLES 240
#define WIN_SAMPLES 960
#define MAX_HOPS (MAX_SAMPLES / HOP_SAMPLES)

static int16_t g_pcm[MAX_SAMPLES];
static double g_env[MAX_TONES][MAX_HOPS];

typedef struct {
    double hz;
    double own_db;   /* level during this tone's own slot         */
    double leak_db;  /* strongest level in any OTHER slot         */
    double floor_db; /* the room at this frequency, before the emission */
    double snr_db;   /* own_db - floor_db                          */
} tone_t;

typedef struct {
    char label[64];
    double cap_gain;  /* receiver input gain this path was recorded at */
    double emit_gain; /* transmitter emission gain, percent            */
    size_t count;
    tone_t tone[MAX_TONES];
} path_t;

/*
 * Goertzel magnitude over a window, normalised so that a full-scale sine at
 * exactly `hz` reads 1.0. Reported in dBFS.
 *
 * A DFT bin would do as well; Goertzel is used because the frequencies of
 * interest are known in advance and there are at most a few dozen of them, so
 * paying for a full transform buys nothing.
 */
static double goertzel_db(const int16_t *pcm, size_t start, size_t n, double hz)
{
    double w = 2.0 * 3.14159265358979323846 * hz / SAMPLE_RATE;
    double coeff = 2.0 * cos(w);
    double s0 = 0.0, s1 = 0.0, s2 = 0.0;
    double mag;
    size_t i;

    if (n == 0) return -200.0;
    for (i = 0; i < n; ++i) {
        s0 = (double)pcm[start + i] / 32768.0 + coeff * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    mag = sqrt(s1 * s1 + s2 * s2 - coeff * s1 * s2);
    mag = 2.0 * mag / (double)n;
    if (mag < 1e-10) return -200.0;
    return 20.0 * log10(mag);
}

/*
 * Find the marker's onset.
 *
 * The board emits a fixed tone for `marker_ms`, then silence, then the ladder.
 * Slots are located from the marker rather than from the start of the file
 * because the host recorder starts before the board does and the lead-in is
 * not constant.
 *
 * Onset is the first hop whose marker-frequency level is within 6 dB of the
 * strongest hop, not the strongest hop itself: the strongest is somewhere in
 * the middle of the tone, and the middle is not a boundary.
 */
static long find_marker(size_t count, double marker_hz, double marker_ms)
{
    const size_t hop = (size_t)(SAMPLE_RATE * 0.005);        /* 5 ms   */
    const size_t win = (size_t)(SAMPLE_RATE * 0.020);        /* 20 ms  */
    double best = -300.0;
    size_t best_at = 0;
    size_t i;

    (void)marker_ms;
    if (count < win) return -1;
    for (i = 0; i + win < count; i += hop) {
        double db = goertzel_db(g_pcm, i, win, marker_hz);
        if (db > best) { best = db; best_at = i; }
    }
    if (best < -60.0) return -1;
    for (i = 0; i + win < count; i += hop) {
        if (goertzel_db(g_pcm, i, win, marker_hz) >= best - 6.0) {
            return (long)i;
        }
        if (i >= best_at) break;
    }
    return (long)best_at;
}

static int cmd_slots(int argc, char **argv)
{
    const char *path = argv[2];
    const char *label = argv[3];
    double marker_hz = 6000.0, marker_ms = 300.0, gap_ms = 100.0, slot_ms = 100.0;
    double f_start = 1200.0, f_end = 9000.0, step = 300.0;
    double cap_gain = -1.0, emit_gain = -1.0;
    size_t count = 0, n_tones = 0, n_sane = 0, i, j;
    double hz[MAX_TONES];
    long onset;
    size_t slot0, slot_n, guard, floor_at = 0, floor_n = 0;
    double flr;
    int a;

    for (a = 4; a + 1 < argc; a += 2) {
        double v = atof(argv[a + 1]);
        if (!strcmp(argv[a], "--marker")) marker_hz = v;
        else if (!strcmp(argv[a], "--markerms")) marker_ms = v;
        else if (!strcmp(argv[a], "--gapms")) gap_ms = v;
        else if (!strcmp(argv[a], "--slotms")) slot_ms = v;
        else if (!strcmp(argv[a], "--start")) f_start = v;
        else if (!strcmp(argv[a], "--end")) f_end = v;
        else if (!strcmp(argv[a], "--step")) step = v;
        else if (!strcmp(argv[a], "--capgain")) cap_gain = v;
        else if (!strcmp(argv[a], "--emitgain")) emit_gain = v;
        else { fprintf(stderr, "unknown option %s\n", argv[a]); return 2; }
    }
    if (cap_gain < 0.0 || emit_gain < 0.0) {
        fprintf(stderr,
                "--capgain and --emitgain are required.\n"
                "\n"
                "This tool reports dBFS, and dBFS is only comparable between\n"
                "two transmitters if both were recorded through the same input\n"
                "gain and driven at the same emission level. Without those two\n"
                "numbers a curve cannot be placed alongside another one, and a\n"
                "minimax over curves that were not measured alike would choose\n"
                "a band on the strength of a volume knob.\n");
        return 2;
    }

    if (wav_read_pcm16(path, g_pcm, MAX_SAMPLES, &count) != WAV_OK) {
        fprintf(stderr, "cannot read %s\n", path);
        return 3;
    }

    for (double f = f_start; f <= f_end + 0.5 && n_tones < MAX_TONES; f += step) {
        hz[n_tones++] = f;
    }

    onset = find_marker(count, marker_hz, marker_ms);
    if (onset < 0) {
        fprintf(stderr, "marker %.0f Hz not found in %s -- "
                        "no ladder to measure\n", marker_hz, path);
        return 4;
    }

    /*
     * THE LADDER IS LOCATED BY FITTING IT, NOT BY TRUSTING THE TRANSMITTER.
     *
     * The first over-air run assumed the slot period the transmitter was asked
     * for. The DFR1154 emitted 125 ms slots when told 100, at exactly the right
     * pitch -- the frequencies were dead on and only the durations were long --
     * so every tone was measured in the wrong window and every level came back
     * below the leakage floor. The tool reported an impossibility rather than a
     * plausible curve, which is the only reason it was caught.
     *
     * The modem already learned this lesson and it is written into
     * ap_modem.c: the receiver does not assume the sender's symbol rate, it
     * refines it over the whole frame. The same rule applies here. A phone,
     * a laptop and a microcontroller will each buffer audio their own way,
     * and an instrument that believes the nominal timing measures the
     * buffering rather than the loudspeaker.
     *
     * So the start and the period are fitted: build a per-tone energy envelope
     * once, then grid-search the (start, period) pair whose slot centres
     * collect the most energy. Missing tones -- a notch deep enough that a
     * candidate never rises above the room -- cost the fit nothing, because
     * they contribute nothing to the sum either way.
     */
    {
        size_t hops = (count > WIN_SAMPLES) ? (count - WIN_SAMPLES) / HOP_SAMPLES : 0;
        double nominal_period = SAMPLE_RATE * slot_ms / 1000.0;
        double best_sum = -1e18, best_start = 0.0, best_period = nominal_period;
        double start_lo, start_hi, p;
        size_t h;

        if (hops < 4 || hops > MAX_HOPS) {
            fprintf(stderr, "capture is the wrong length to fit (%lu hops)\n",
                    (unsigned long)hops);
            return 4;
        }
        for (i = 0; i < n_tones; ++i) {
            for (h = 0; h < hops; ++h) {
                g_env[i][h] = goertzel_db(g_pcm, h * HOP_SAMPLES, WIN_SAMPLES, hz[i]);
            }
        }

        /* The ladder starts after the marker. Search generously either side of
           where the nominal marker and gap would put it. */
        start_lo = (double)onset + SAMPLE_RATE * (marker_ms + gap_ms) / 1000.0 * 0.6;
        start_hi = (double)onset + SAMPLE_RATE * (marker_ms + gap_ms) / 1000.0 * 2.2;

        /*
         * THE LADDER IS FITTED BY REGRESSION, NOT BY SEARCH.
         *
         * Each tone appears exactly once, so each one's own energy envelope
         * has a single peak, and the peak of tone i sits at
         *
         *     centre(i) = start + period * (i + 0.5)
         *
         * which is a straight line in i. Fitting that line by least squares
         * over the tones that are actually detectable gives start and period
         * directly, to far better precision than the 5 ms envelope resolution,
         * because twenty-odd peaks are averaged rather than one being trusted.
         *
         * A grid search was tried first and was worse in a way worth
         * recording: the envelope's quantisation makes the score a plateau, so
         * the coarse pass ties and lands anywhere on it, and a fine pass
         * centred on that winner cannot reach the true period. The fit above
         * has no plateau to get stuck on.
         *
         * A tone deep in a notch has no peak worth having. Those are excluded
         * on the second pass, by residual, so a loudspeaker null costs the fit
         * nothing instead of dragging the line towards the room.
         */
        {
            double px[MAX_TONES];
            int use[MAX_TONES];
            size_t h, n_used = 0;
            int pass;
            double a = 0.0, b = nominal_period;

            for (i = 0; i < n_tones; ++i) {
                double bestv = -1e9;
                size_t at = 0;
                for (h = 0; h < hops; ++h) {
                    /* Only after the marker: the marker tone is one of the
                       candidates, and its 300 ms would outrank the 100 ms
                       slot belonging to that same frequency. */
                    if ((double)(h * HOP_SAMPLES) < (double)onset
                        + SAMPLE_RATE * marker_ms / 1000.0 * 0.8) continue;
                    if (g_env[i][h] > bestv) { bestv = g_env[i][h]; at = h; }
                }
                /*
                 * THE PEAK IS A PLATEAU, AND ITS MIDDLE IS THE CENTRE.
                 *
                 * The analysis window is shorter than a slot, so it sits
                 * wholly inside the tone for a whole range of positions and
                 * every one of them reads the same level. Taking the first
                 * such hop -- which is what an argmax returns -- puts the
                 * estimate at the tone's LEADING EDGE plus half a window, and
                 * the fitted intercept then lands about 35 ms early on a
                 * 100 ms slot. The measurement window straddles the boundary
                 * and every tone reads 3 dB low, uniformly, which looks like
                 * a quiet transmitter rather than a misfit.
                 */
                {
                    size_t first = at, last = at;
                    for (h = 0; h < hops; ++h) {
                        if ((double)(h * HOP_SAMPLES) < (double)onset
                            + SAMPLE_RATE * marker_ms / 1000.0 * 0.8) continue;
                        if (g_env[i][h] >= bestv - 3.0) {
                            if (h < first) first = h;
                            last = h;
                        }
                    }
                    /* Contiguity is not enforced: a spurious far-away hop
                       within 3 dB would widen this, and the residual test
                       below is what removes such a tone from the fit. */
                    px[i] = ((double)first + (double)last) / 2.0 * HOP_SAMPLES
                            + WIN_SAMPLES / 2.0;
                }
                use[i] = 1;
                n_used++;
            }

            /*
             * SEED THE LINE WITH MEDIANS, NOT WITH LEAST SQUARES.
             *
             * Least squares has no resistance to outliers, and some tones DO
             * peak in the wrong place: 6000 Hz is also the marker frequency,
             * and a tone sitting in a notch peaks wherever the room happened
             * to be loudest. Seeded with least squares over all tones, the
             * first line was dragged far enough off that the outlier rejection
             * then discarded twenty-four of twenty-seven good tones and the
             * fit failed.
             *
             * The median of consecutive peak spacings is immune to that: it
             * takes a majority of the tones to move it.
             */
            {
                double d[MAX_TONES], t;
                size_t nd = 0, k, m;
                for (i = 0; i + 1 < n_tones; ++i) d[nd++] = px[i + 1] - px[i];
                for (k = 0; k < nd; ++k) {
                    for (m = k + 1; m < nd; ++m) {
                        if (d[m] < d[k]) { t = d[k]; d[k] = d[m]; d[m] = t; }
                    }
                }
                if (nd == 0) {
                    fprintf(stderr, "no tones to fit in %s\n", path);
                    return 4;
                }
                b = d[nd / 2];
                if (b < nominal_period * 0.5 || b > nominal_period * 2.5) {
                    b = nominal_period;
                }
                for (i = 0; i < n_tones; ++i) d[i] = px[i] - b * ((double)i + 0.5);
                for (k = 0; k < n_tones; ++k) {
                    for (m = k + 1; m < n_tones; ++m) {
                        if (d[m] < d[k]) { t = d[k]; d[k] = d[m]; d[m] = t; }
                    }
                }
                a = d[n_tones / 2];
                n_used = 0;
                for (i = 0; i < n_tones; ++i) {
                    double pred = a + b * ((double)i + 0.5);
                    use[i] = (fabs(px[i] - pred) < fabs(b) / 3.0) ? 1 : 0;
                    if (use[i]) n_used++;
                }
            }

            for (pass = 0; pass < 2; ++pass) {
                double sx = 0.0, sy = 0.0, sxx = 0.0, sxy = 0.0, den;
                size_t n = 0;
                for (i = 0; i < n_tones; ++i) {
                    double x;
                    if (!use[i]) continue;
                    x = (double)i + 0.5;
                    sx += x; sy += px[i]; sxx += x * x; sxy += x * px[i];
                    n++;
                }
                if (n < 4) break;
                den = (double)n * sxx - sx * sx;
                if (den == 0.0) break;
                b = ((double)n * sxy - sx * sy) / den;
                a = (sy - b * sx) / (double)n;
                if (pass == 0) {
                    /* Drop the tones the line does not explain: a peak more
                       than a third of a slot from where the ladder says it
                       should be is a notch, not a slot. */
                    n_used = 0;
                    for (i = 0; i < n_tones; ++i) {
                        double pred = a + b * ((double)i + 0.5);
                        use[i] = (fabs(px[i] - pred) < fabs(b) / 3.0) ? 1 : 0;
                        if (use[i]) n_used++;
                    }
                }
            }

            if (b < nominal_period * 0.5 || b > nominal_period * 2.5
                || a < 0.0 || n_used < 4) {
                fprintf(stderr,
                        "could not fit a ladder in %s "
                        "(period %.0f samples from %lu usable tones)\n",
                        path, b, (unsigned long)n_used);
                return 4;
            }

            /* The line is fitted as centre(i) = a + b*(i + 0.5), so slot i
               starts at a + b*i and the first slot starts at a itself. */
            best_start = a;
            best_period = b;
            printf("# fit_tones_used\t%lu\n", (unsigned long)n_used);
        }
        if (0) {
            (void)start_lo; (void)start_hi; (void)best_sum; (void)p;
        }

        slot0 = (size_t)best_start;
        slot_n = (size_t)best_period;

        printf("# nominal_slot_ms\t%.1f\n", slot_ms);
        printf("# fitted_slot_ms\t%.2f\n", best_period * 1000.0 / SAMPLE_RATE);
        printf("# fitted_stretch\t%.4f\n", best_period / nominal_period);
    }

    /* Measure the middle of each slot only. The transmitter ramps each tone in
       and out, and a window that includes a ramp measures the ramp. The guard
       is a fraction of the FITTED slot, not a constant, because the slot
       length is now something this tool discovers rather than assumes. */
    guard = (size_t)((double)slot_n * 0.2);
    if (guard < (size_t)(SAMPLE_RATE * 0.010)) guard = (size_t)(SAMPLE_RATE * 0.010);
    if (slot_n <= 2 * guard + HOP_SAMPLES) {
        fprintf(stderr, "fitted slot %lu samples is too short to measure\n",
                (unsigned long)slot_n);
        return 4;
    }

    printf("# band_sweep slots\n");
    printf("# file\t%s\n", path);
    printf("# label\t%s\n", label);
    printf("# samples\t%lu\n", (unsigned long)count);
    printf("# capgain\t%.6f\n", cap_gain);
    printf("# emitgain\t%.1f\n", emit_gain);
    printf("# marker_onset_sample\t%ld\n", onset);
    printf("# slot0_sample\t%lu\n", (unsigned long)slot0);
    printf("# slot_samples\t%lu\n", (unsigned long)slot_n);
    /*
     * THE ROOM, MEASURED AT EACH CANDIDATE FREQUENCY.
     *
     * Levels in dBFS are not comparable between two transmitters unless both
     * were recorded through the same input gain -- and matching a board's
     * digital emission gain to a laptop's render volume is not something a rig
     * can do exactly. Comparing them raw compares two volume knobs.
     *
     * Signal-to-noise ratio is the quantity that actually decides whether a
     * tone is received, and it has the property this campaign needs: capture
     * gain multiplies the tone and the room alike, so it cancels. Emission
     * gain does NOT cancel, which is correct -- a transmitter that cannot
     * drive its speaker is genuinely worse off against the same room.
     *
     * The floor is measured in the capture's own pre-roll, before the marker,
     * where the recorder is running and nothing has been emitted yet. It is
     * therefore this room, on this day, through this microphone, rather than
     * an assumed number.
     */
    {
        size_t pre = (size_t)onset;
        if (pre > (size_t)(SAMPLE_RATE * 0.020)) {
            floor_n = pre - (size_t)(SAMPLE_RATE * 0.010); /* stop short of the onset */
            floor_at = 0;
        } else {
            floor_n = 0;
        }
        if (floor_n < (size_t)(SAMPLE_RATE * 0.050)) {
            fprintf(stderr,
                    "WARNING: only %.0f ms of pre-roll before the marker; the\n"
                    "         noise floor is measured over a short window and\n"
                    "         SNR is correspondingly less certain.\n",
                    (double)floor_n * 1000.0 / SAMPLE_RATE);
        }
    }

    printf("# floor_window_ms\t%.1f\n", (double)floor_n * 1000.0 / SAMPLE_RATE);
    printf("hz\town_db\tleak_db\tfloor_db\tsnr_db\town_vs_leak_db\n");

    for (i = 0; i < n_tones; ++i) {
        double own, leak = -300.0;

        size_t s = slot0 + i * slot_n;
        if (s + slot_n > count) {
            fprintf(stderr, "capture ends inside slot %lu (%.0f Hz) -- "
                            "recording too short\n",
                    (unsigned long)i, hz[i]);
            return 4;
        }
        own = goertzel_db(g_pcm, s + guard, slot_n - 2 * guard, hz[i]);

        /*
         * LEAKAGE, AND WHY IT IS REPORTED RATHER THAN SUBTRACTED
         *
         * A 1200 Hz tone puts harmonic energy at 2400, 3600 and 4800 Hz, and
         * all three are candidates in this ladder. If each tone were measured
         * anywhere in the capture, a low tone's distortion would be credited
         * to a high tone and a band could be chosen on the strength of a
         * harmonic that only exists while some other tone is playing.
         *
         * Measuring inside the tone's own slot already avoids that. This
         * column is the check that it worked: it is the strongest this
         * frequency ever read while some OTHER tone was playing. If own_db
         * does not stand well clear of leak_db, the number above it is not a
         * measurement of this path at this frequency, and the run should be
         * discarded rather than corrected -- a correction here would be a
         * model of the amplifier, and this instrument does not have one.
         */
        for (j = 0; j < n_tones; ++j) {
            double db;
            size_t o = slot0 + j * slot_n;
            if (j == i) continue;
            if (o + slot_n > count) break;
            db = goertzel_db(g_pcm, o + guard, slot_n - 2 * guard, hz[i]);
            if (db > leak) leak = db;
        }
        flr = (floor_n > 0) ? goertzel_db(g_pcm, floor_at, floor_n, hz[i]) : -200.0;
        printf("%.0f\t%.2f\t%.2f\t%.2f\t%.2f\t%.2f\n",
               hz[i], own, leak, flr, own - flr, own - leak);
        if (own > leak) n_sane++;
    }

    /*
     * A tone that reads WEAKER inside its own slot than outside it is not a
     * quiet tone -- it is a misaligned measurement, and the number above it
     * describes some other part of the recording. One such tone can happen in
     * a deep notch, where the slot holds nothing but room. A majority of them
     * means the ladder is not where this tool thinks it is, and the whole
     * curve has to be thrown away rather than interpreted.
     *
     * This is the check that caught the transmitter emitting 125 ms slots
     * when it was asked for 100.
     */
    fprintf(stderr, "%lu of %lu tones measured stronger in their own slot\n",
            (unsigned long)n_sane, (unsigned long)n_tones);
    if (n_sane * 2 <= n_tones) {
        fprintf(stderr,
                "REJECTED: the ladder is not aligned. Most tones read stronger\n"
                "outside their own slot than inside it, which is impossible if\n"
                "the slots are where they are believed to be. Do not interpret\n"
                "these levels. Run `band_sweep timeline <wav> 5` to see what the\n"
                "capture actually contains.\n");
        return 4;
    }
    return 0;
}

static int load_path(const char *file, path_t *p)
{
    FILE *f = fopen(file, "r");
    char line[512];

    if (!f) { fprintf(stderr, "cannot read %s\n", file); return 3; }
    p->count = 0;
    p->cap_gain = -1.0;
    p->emit_gain = -1.0;
    snprintf(p->label, sizeof(p->label), "%s", file);
    while (fgets(line, sizeof(line), f)) {
        double hz, own, leak, flr, snr, margin;
        if (!strncmp(line, "# capgain\t", 10)) {
            p->cap_gain = atof(line + 10);
            continue;
        }
        if (!strncmp(line, "# emitgain\t", 11)) {
            p->emit_gain = atof(line + 11);
            continue;
        }
        if (!strncmp(line, "# label\t", 8)) {
            char *nl;
            snprintf(p->label, sizeof(p->label), "%.*s",
                     (int)sizeof(p->label) - 1, line + 8);
            nl = strchr(p->label, '\n');
            if (nl) *nl = '\0';
            continue;
        }
        if (line[0] == '#' || line[0] == 'h') continue;
        if (sscanf(line, "%lf\t%lf\t%lf\t%lf\t%lf\t%lf",
                   &hz, &own, &leak, &flr, &snr, &margin) != 6) {
            continue;
        }
        if (p->count >= MAX_TONES) break;
        p->tone[p->count].hz = hz;
        p->tone[p->count].own_db = own;
        p->tone[p->count].leak_db = leak;
        p->tone[p->count].floor_db = flr;
        p->tone[p->count].snr_db = snr;
        p->count++;
    }
    fclose(f);
    if (p->count == 0) {
        fprintf(stderr, "%s has no tone rows\n", file);
        return 3;
    }
    return 0;
}

/*
 * The quantity the selection is scored on is SIGNAL-TO-NOISE RATIO, not level.
 *
 * Two transmitters cannot be driven to the same absolute level by a rig -- a
 * board's digital emission gain and a laptop's render volume are different
 * knobs -- so raw dBFS would rank the knobs. SNR divides both the tone and the
 * room by the same capture gain, so that setting cancels exactly, and what is
 * left is how far this transmitter puts this tone above this room.
 */
static double level_at(const path_t *p, double hz, int *found)
{
    size_t i;
    for (i = 0; i < p->count; ++i) {
        if (fabs(p->tone[i].hz - hz) < 0.5) { *found = 1; return p->tone[i].snr_db; }
    }
    *found = 0;
    return -300.0;
}

static double abs_at(const path_t *p, double hz)
{
    size_t i;
    for (i = 0; i < p->count; ++i) {
        if (fabs(p->tone[i].hz - hz) < 0.5) return p->tone[i].own_db;
    }
    return -300.0;
}

/*
 * The score of one pair, over all paths. ONE function, because the incumbent
 * comparison must be computed exactly as the search computes its candidates.
 *
 * It was not: the search scored the chirp band and the incumbent line scored
 * only the two tones, so the "+11.38 dB improvement" printed underneath was a
 * difference between two different measures rather than between two bands.
 */
static double pair_score(const path_t *path, size_t n_paths,
                         double f0, double f1, int *admissible)
{
    double score = 1e9;
    size_t k, m;

    *admissible = 1;
    for (k = 0; k < n_paths; ++k) {
        int have0 = 0, have1 = 0;
        double a = level_at(&path[k], f0, &have0);
        double b = level_at(&path[k], f1, &have1);
        double weaker = (a < b) ? a : b;
        if (!have0 || !have1) { *admissible = 0; return -1e9; }
        /*
         * THE PREAMBLE IS PART OF THE BAND, AND IT COST A BURST TO LEARN IT.
         *
         * AP-BOOTSTRAP-1 derives the acquisition chirp from the pair: it
         * sweeps from f0 - 1000 Hz up to f1. Scoring only the two FSK tones
         * scores the DATA and ignores the thing that has to find the data.
         *
         * THE SUPPORTING RUN WAS INVALID AND THE RULE IS KEPT ON ARGUMENT.
         *
         * 1500/6300 was first tried over air, came back acquired=0 with
         * correlation 0.106 at a healthy -10.5 dB peak, and that was written
         * down here as evidence that its 500 Hz chirp start was unreachable.
         * It was not evidence of anything: the rig set the band on the HOST
         * tool only and never sent BAND to the board, so the board emitted at
         * the modem default and the host decoded a different band. 6000/7200
         * failed identically for the same reason. Correlation ~0.10 was a
         * transmit/receive mismatch, not a preamble.
         *
         * The rule stays because the ARGUMENT stands on its own: the receiver
         * correlates against the derived chirp, so a chirp sweeping where the
         * transmitter cannot drive costs acquisition whatever the two tones
         * do. It has NOT been demonstrated over air, and the README says so.
         *
         * So every measured tone the chirp sweeps through is scored too. A
         * pair whose preamble crosses a null is not a usable pair, however
         * clean its two tones are.
         */
        for (m = 0; m < path[k].count; ++m) {
            double hz = path[k].tone[m].hz;
            if (hz < f0 - 1000.0 || hz > f1) continue;
            if (path[k].tone[m].snr_db < weaker) {
                weaker = path[k].tone[m].snr_db;
            }
        }
        if (weaker < score) score = weaker;
    }
    return score;
}

static int cmd_minimax(int argc, char **argv)
{
    path_t path[MAX_PATHS];
    size_t n_paths = 0;
    size_t i, j, k;
    double best_score = -1e9, best_f0 = 0.0, best_f1 = 0.0;
    const path_t *ref;

    for (i = 2; i < (size_t)argc && n_paths < MAX_PATHS; ++i) {
        int rc = load_path(argv[i], &path[n_paths]);
        if (rc) return rc;
        n_paths++;
    }
    if (n_paths == 0) return 2;
    ref = &path[0];

    /*
     * THE PATHS MUST HAVE BEEN MEASURED ALIKE.
     *
     * Comparing dBFS between two transmitters recorded at different input
     * gains compares the two gain settings, not the two transmitters -- and it
     * does so invisibly, producing a plausible winner. Refusing is the only
     * safe answer, because there is no correction that recovers the missing
     * information after the fact.
     */
    for (i = 1; i < n_paths; ++i) {
        if (fabs(path[i].cap_gain - ref->cap_gain) > 1e-6 ||
            fabs(path[i].emit_gain - ref->emit_gain) > 1e-6) {
            fprintf(stderr,
                    "NOTE: paths were recorded at different settings.\n"
                    "  %-28s capgain %.6f  emitgain %.1f\n"
                    "  %-28s capgain %.6f  emitgain %.1f\n"
                    "Selection is scored on SNR, and a constant capture gain\n"
                    "cancels from it, so this is not fatal. It WOULD be fatal\n"
                    "for a comparison of raw levels, and the own_db column\n"
                    "printed below is exactly that -- read it per path, never\n"
                    "across them.\n\n",
                    ref->label, ref->cap_gain, ref->emit_gain,
                    path[i].label, path[i].cap_gain, path[i].emit_gain);
        }
    }

    printf("=== minimax band selection over %lu path(s) ===\n",
           (unsigned long)n_paths);
    for (i = 0; i < n_paths; ++i) {
        printf("  path %lu  %s  (%lu tones)\n",
               (unsigned long)i, path[i].label, (unsigned long)path[i].count);
    }
    printf("\nscore = weaker tone's SNR above the room, on the worst path\n\n");

    for (i = 0; i < ref->count; ++i) {
        for (j = 0; j < ref->count; ++j) {
            double f0 = ref->tone[i].hz;
            double f1 = ref->tone[j].hz;
            double score = 1e9;

            if (f1 <= f0) continue;

            /*
             * CONSTRAINTS THE MODEM IMPOSES ON A PAIR. A candidate that
             * violates one of these cannot be adopted, so scoring it would
             * only produce a winner that has to be thrown away.
             *
             *   f0 >= 1500  the preamble chirp starts 1000 Hz below f0 and the
             *               modem floors that at 500 Hz; below 1500 the chirp
             *               is compressed rather than shifted.
             *   f1 - f0 >= 900  the symbol is 3.333 ms, so adjacent-bin
             *               spacing is 300 Hz. Three bins is the smallest
             *               separation at which the two detectors are not
             *               reading each other's skirts.
             *   f1 <= 9000  above this the laptop and phone capture paths are
             *               no longer flat and the measurement stops being
             *               about the transmitter.
             */
            /*
             * The chirp starts 1000 Hz below f0 and the modem floors it at
             * 500 Hz. A pair whose chirp would be FLOORED rather than shifted
             * is refused outright: the emitted preamble then differs from the
             * one the derivation describes, and the receiver correlates
             * against the derivation.
             *
             * The floor is the lowest MEASURED tone, not 500 Hz. Scoring a
             * chirp through a region nothing was measured in is guessing, and
             * that guess is exactly what failed over air.
             */
            if (f0 - 1000.0 < ref->tone[0].hz) continue;
            if (f1 - f0 < 900.0) continue;
            if (f1 > 9000.0) continue;

            {
                int ok = 1;
                score = pair_score(path, n_paths, f0, f1, &ok);
                if (!ok) continue;
            }

            if (score > best_score ||
                (score == best_score && (f1 - f0) > (best_f1 - best_f0))) {
                best_score = score;
                best_f0 = f0;
                best_f1 = f1;
            }
        }
    }

    if (best_score <= -1e8) {
        printf("NO ADMISSIBLE PAIR. The paths do not share a candidate set.\n");
        return 4;
    }

    printf("WINNER  %.0f / %.0f Hz\n", best_f0, best_f1);
    printf("worst-path weaker-tone SNR: %.2f dB\n\n", best_score);
    printf("per-path detail for the winning pair:\n");
    printf("  %-28s %9s %9s %9s %9s\n", "path",
           "f0 SNR", "f1 SNR", "f0 dBFS", "f1 dBFS");
    for (k = 0; k < n_paths; ++k) {
        int h0 = 0, h1 = 0;
        printf("  %-28s %9.2f %9.2f %9.2f %9.2f\n", path[k].label,
               level_at(&path[k], best_f0, &h0),
               level_at(&path[k], best_f1, &h1),
               abs_at(&path[k], best_f0),
               abs_at(&path[k], best_f1));
    }

    /* The incumbent, scored the same way, so the comparison is like for like
       rather than a new number against a remembered one. */
    {
        int ok = 1;
        double inc = pair_score(path, n_paths, 3000.0, 6000.0, &ok);
        printf("\nincumbent AP-BOOTSTRAP-1 pair 3000/6000 Hz, scored the "
               "same way\n(preamble chirp included): ");
        if (!ok) {
            printf("not in the measured set\n");
        } else {
            printf("%.2f dB SNR\n", inc);
            printf("minimax improvement: %+.2f dB\n", best_score - inc);
        }
    }
    return 0;
}

/*
 * Generate the same ladder the firmware emits, as a WAV.
 *
 * The second transmitter in this campaign is the laptop's own speaker, and the
 * third will be a phone. Neither runs the firmware, so the ladder has to exist
 * as a file -- and it must be THE SAME ladder, or the two curves are of two
 * different stimuli and cannot be compared. The parameters below are the
 * constants in dfr1154_mcl_node.ino, and the marker/gap/slot structure the
 * analyser looks for is identical.
 */
static int cmd_ladder(int argc, char **argv)
{
    const char *out = argv[2];
    double marker_hz = 6000.0, marker_ms = 300.0, gap_ms = 100.0, slot_ms = 100.0;
    double f_start = 1200.0, f_end = 9000.0, step = 300.0, amp = 0.7;
    double ramp_ms = 5.0;
    /* Leading silence, so a capture of this file contains a window of the
       room with nothing emitted into it. That window is where the analyser
       measures the noise floor, and SNR is what the selection is scored on. */
    double preroll_ms = 250.0;
    size_t n = 0;
    int a;

    for (a = 3; a + 1 < argc; a += 2) {
        double v = atof(argv[a + 1]);
        if (!strcmp(argv[a], "--marker")) marker_hz = v;
        else if (!strcmp(argv[a], "--start")) f_start = v;
        else if (!strcmp(argv[a], "--end")) f_end = v;
        else if (!strcmp(argv[a], "--step")) step = v;
        else if (!strcmp(argv[a], "--amp")) amp = v;
        else if (!strcmp(argv[a], "--preroll")) preroll_ms = v;
        else { fprintf(stderr, "unknown option %s\n", argv[a]); return 2; }
    }
    if (amp <= 0.0 || amp > 1.0) { fprintf(stderr, "--amp out of range\n"); return 2; }

    {
        /* One tone, ramped at both ends -- a hard start is a step, and a step
           is broadband energy this measurement must not manufacture. */
        double f;
        size_t k;
        size_t marker_n = (size_t)(SAMPLE_RATE * marker_ms / 1000.0);
        size_t gap_n = (size_t)(SAMPLE_RATE * gap_ms / 1000.0);
        size_t slot_n = (size_t)(SAMPLE_RATE * slot_ms / 1000.0);
        size_t ramp = (size_t)(SAMPLE_RATE * ramp_ms / 1000.0);

        for (k = 0; k < (size_t)(SAMPLE_RATE * preroll_ms / 1000.0)
                    && n < MAX_SAMPLES; ++k, ++n) {
            g_pcm[n] = 0;
        }
        for (k = 0; k < marker_n && n < MAX_SAMPLES; ++k, ++n) {
            double s = amp;
            if (k < ramp) s *= 0.5 * (1.0 - cos(3.14159265358979323846 * (double)k / (double)ramp));
            else if (k + ramp > marker_n) s *= 0.5 * (1.0 - cos(3.14159265358979323846 * (double)(marker_n - k) / (double)ramp));
            g_pcm[n] = (int16_t)(s * 32767.0 * sin(2.0 * 3.14159265358979323846 * marker_hz * (double)k / SAMPLE_RATE));
        }
        for (k = 0; k < gap_n && n < MAX_SAMPLES; ++k, ++n) g_pcm[n] = 0;
        for (f = f_start; f <= f_end + 0.5; f += step) {
            for (k = 0; k < slot_n && n < MAX_SAMPLES; ++k, ++n) {
                double s = amp;
                if (k < ramp) s *= 0.5 * (1.0 - cos(3.14159265358979323846 * (double)k / (double)ramp));
                else if (k + ramp > slot_n) s *= 0.5 * (1.0 - cos(3.14159265358979323846 * (double)(slot_n - k) / (double)ramp));
                g_pcm[n] = (int16_t)(s * 32767.0 * sin(2.0 * 3.14159265358979323846 * f * (double)k / SAMPLE_RATE));
            }
        }
    }

    if (wav_write_pcm16(out, g_pcm, n) != WAV_OK) {
        fprintf(stderr, "cannot write %s\n", out);
        return 3;
    }
    printf("wrote %s, %lu samples, %.2f s\n", out, (unsigned long)n,
           (double)n / SAMPLE_RATE);
    return 0;
}

/*
 * What is actually in this capture, as a function of time.
 *
 * The first over-air run of `slots` reported every tone weaker inside its own
 * slot than outside it, which is impossible if the slots are where the tool
 * thinks they are. That is a statement about alignment, not about the room,
 * and no amount of staring at the level column answers it. This prints the
 * strongest candidate tone in each window so the ladder's real structure --
 * where it starts, how long each step lasts, and at what frequency -- can be
 * read off the capture rather than assumed from the transmitter's parameters.
 */
static int cmd_timeline(int argc, char **argv)
{
    const char *path = argv[2];
    double win_ms = (argc > 3) ? atof(argv[3]) : 25.0;
    double f_start = (argc > 4) ? atof(argv[4]) : 300.0;
    double f_end = (argc > 5) ? atof(argv[5]) : 12000.0;
    double step = (argc > 6) ? atof(argv[6]) : 300.0;
    size_t count = 0, win, i;

    if (wav_read_pcm16(path, g_pcm, MAX_SAMPLES, &count) != WAV_OK) {
        fprintf(stderr, "cannot read %s\n", path);
        return 3;
    }
    win = (size_t)(SAMPLE_RATE * win_ms / 1000.0);
    printf("ms\tpeak_hz\tpeak_db\tsecond_hz\tsecond_db\n");
    for (i = 0; i + win < count; i += win) {
        double f, b1 = -300.0, b2 = -300.0, h1 = 0.0, h2 = 0.0;
        for (f = f_start; f <= f_end + 0.5; f += step) {
            double db = goertzel_db(g_pcm, i, win, f);
            if (db > b1) { b2 = b1; h2 = h1; b1 = db; h1 = f; }
            else if (db > b2) { b2 = db; h2 = f; }
        }
        printf("%.0f\t%.0f\t%.2f\t%.0f\t%.2f\n",
               (double)i * 1000.0 / SAMPLE_RATE, h1, b1, h2, b2);
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc >= 3 && !strcmp(argv[1], "timeline")) return cmd_timeline(argc, argv);
    if (argc >= 3 && !strcmp(argv[1], "ladder")) return cmd_ladder(argc, argv);
    if (argc >= 4 && !strcmp(argv[1], "slots")) return cmd_slots(argc, argv);
    if (argc >= 3 && !strcmp(argv[1], "minimax")) return cmd_minimax(argc, argv);
    fprintf(stderr,
            "usage: band_sweep ladder  <out.wav> [--marker hz] [--start hz]\n"
            "                          [--end hz] [--step hz] [--amp 0..1]\n"
            "       band_sweep slots   <capture.wav> <label>\n"
            "                          --capgain <g> --emitgain <percent>\n"
            "                          [--marker hz]\n"
            "                          [--markerms ms] [--gapms ms] [--slotms ms]\n"
            "                          [--start hz] [--end hz] [--step hz]\n"
            "       band_sweep minimax <a.tsv> [b.tsv ...]\n");
    return 2;
}
