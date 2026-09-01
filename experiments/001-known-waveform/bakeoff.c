/*
 * MCL-AP Experiment 001: Corrected Preamble Bakeoff
 *
 * Compares 4 candidate preambles under equalized resource budgets:
 *   - LFM_CHIRP
 *   - ZC_DERIVED
 *   - PN_MSEQ
 *   - FREQ_DIVERSE
 *
 * Methodology:
 *   1. Preamble exact length invariant: N = 4800 samples (0.100s at 48 kHz).
 *   2. Energy equalized: sum(x[n]^2) == 0.40 * N for all candidates.
 *   3. Empirical false-alarm calibration:
 *      - Run signal-absent noise trials over 9600-sample acquisition window.
 *      - Record maximum correlation magnitude per trial.
 *      - Establish threshold gamma for target empirical Pfa = 0.01 (1%).
 *      - Verify empirical Pfa on independent noise trials.
 *   4. Signal-present evaluation at calibrated threshold gamma:
 *      - Variable leading silence offsets (0, 17, 53, 101, 319, 1000 samples).
 *      - Evaluate detection probability (Pd), mean timing error, and max timing error.
 *      - Downstream FSK payload CRC evaluated as a separate downstream metric.
 *
 * Status: LAB / EXPERIMENTAL — NOT AP-B0.
 */

#include "exp001.h"
#include "mcl/wire.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define PCM_BUF_SIZE 960000u
static float g_pcm_src[PCM_BUF_SIZE];
static float g_pcm_imp[PCM_BUF_SIZE];

#define NOISE_CALIB_TRIALS 100u
#define NOISE_VERIFY_TRIALS 100u
#define TARGET_PFA 0.01

typedef struct {
    const char *name;
    double threshold_gamma;
    unsigned noise_verify_trials;
    unsigned observed_false_alarms;
    double empirical_pfa;
} calib_result_t;

typedef struct {
    const char *name;
    unsigned trials;
    unsigned detections;
    double pd;
    double total_timing_error;
    double max_timing_error;
    double total_corr;
    unsigned payload_crc_pass;
} eval_result_t;

static int compare_doubles(const void *a, const void *b)
{
    double da = *(const double *)a;
    double db = *(const double *)b;
    if (da < db) return -1;
    if (da > db) return 1;
    return 0;
}

/*
 * Step 1: Calibrate detection threshold gamma for empirical Pfa target.
 */
static void calibrate_candidate_pfa(exp001_preamble_type_t type,
                                   const char *name,
                                   calib_result_t *out_calib)
{
    const size_t search_window = 9600u;
    double peak_stats[NOISE_CALIB_TRIALS];
    uint32_t rng = 54321u + (uint32_t)type * 7919u;
    unsigned i;
    unsigned fa_count = 0u;

    out_calib->name = name;

    /* A. Signal-absent calibration trials */
    for (i = 0u; i < NOISE_CALIB_TRIALS; ++i) {
        size_t s;
        for (s = 0u; s < search_window; ++s) {
            /* Gaussian noise */
            uint32_t x = rng;
            x ^= x << 13u; x ^= x >> 17u; x ^= x << 5u;
            rng = x;
            double u1 = ((double)(x & 0x7FFFFFFFu) + 1.0) / (double)0x80000000;
            x ^= x << 13u; x ^= x >> 17u; x ^= x << 5u;
            rng = x;
            double u2 = (double)(x & 0x7FFFFFFFu) / (double)0x7FFFFFFF;
            double n = sqrt(-2.0 * log(u1)) * cos(2.0 * 3.1415926535 * u2);
            g_pcm_src[s] = (float)(n * 0.2); /* noise */
        }

        exp001_preamble_detect_t det = exp001_detect_preamble_iq(
            type, 0.1, 2000.0, 6000.0, 0.0, g_pcm_src, search_window);

        peak_stats[i] = det.peak_correlation;
    }

    qsort(peak_stats, NOISE_CALIB_TRIALS, sizeof(double), compare_doubles);

    /* Index for (1 - Pfa) percentile */
    size_t pfa_idx = (size_t)floor((1.0 - TARGET_PFA) * (double)NOISE_CALIB_TRIALS);
    if (pfa_idx >= NOISE_CALIB_TRIALS) pfa_idx = NOISE_CALIB_TRIALS - 1u;
    out_calib->threshold_gamma = peak_stats[pfa_idx];

    /* B. Independent verification trials */
    for (i = 0u; i < NOISE_VERIFY_TRIALS; ++i) {
        size_t s;
        for (s = 0u; s < search_window; ++s) {
            uint32_t x = rng;
            x ^= x << 13u; x ^= x >> 17u; x ^= x << 5u;
            rng = x;
            double u1 = ((double)(x & 0x7FFFFFFFu) + 1.0) / (double)0x80000000;
            x ^= x << 13u; x ^= x >> 17u; x ^= x << 5u;
            rng = x;
            double u2 = (double)(x & 0x7FFFFFFFu) / (double)0x7FFFFFFF;
            double n = sqrt(-2.0 * log(u1)) * cos(2.0 * 3.1415926535 * u2);
            g_pcm_src[s] = (float)(n * 0.2);
        }

        exp001_preamble_detect_t det = exp001_detect_preamble_iq(
            type, 0.1, 2000.0, 6000.0, out_calib->threshold_gamma, g_pcm_src, search_window);

        if (det.detected != 0u) {
            fa_count++;
        }
    }

    out_calib->noise_verify_trials = NOISE_VERIFY_TRIALS;
    out_calib->observed_false_alarms = fa_count;
    out_calib->empirical_pfa = (double)fa_count / (double)NOISE_VERIFY_TRIALS;
}

/*
 * Step 2: Evaluate a candidate under a specific impairment condition.
 */
static void eval_candidate_scenario(
    exp001_preamble_type_t type,
    double threshold_gamma,
    const exp001_impairment_config_t *imp,
    unsigned num_trials,
    eval_result_t *res)
{
    const size_t lead_offsets[] = { 0u, 17u, 53u, 101u, 319u, 1000u };
    const double dur = 0.1;
    uint8_t wire_buf[MCL_WIRE_TIER0_MAX_SIZE];
    size_t wire_written = 0u;
    mcl_wire_tier0_t obj;
    unsigned tr;

    memset(res, 0, sizeof(*res));

    /* Build PRESENCE object */
    memset(&obj, 0, sizeof(obj));
    obj.kind = MCL_WIRE_KIND_PRESENCE;
    obj.priority = 1u;
    obj.source_ref = 0xAA550001u;
    obj.body.presence.machine_class = 2u;
    obj.body.presence.capability_digest = 0x00FF00u;
    obj.body.presence.ttl = 30u;
    mcl_wire_tier0_encode(&obj, wire_buf, sizeof(wire_buf), &wire_written);

    for (tr = 0u; tr < num_trials; ++tr) {
        size_t true_lead = lead_offsets[tr % (sizeof(lead_offsets)/sizeof(lead_offsets[0]))];
        exp001_frame_config_t fconfig;
        exp001_decode_result_t dec_res;
        size_t frame_samples, impaired_samples;
        uint8_t rx_payload[EXP001_MAX_PAYLOAD_BYTES];
        exp001_status_t st;

        fconfig.preamble_type = type;
        fconfig.preamble_duration_s = dur;
        fconfig.preamble_f_start_hz = 2000.0;
        fconfig.preamble_f_end_hz = 6000.0;
        fconfig.leading_silence_s = (double)true_lead / (double)EXP001_SAMPLE_RATE;
        fconfig.silence_duration_s = 0.05;
        fconfig.include_training = 1u;
        fconfig.detection_threshold = threshold_gamma;

        frame_samples = exp001_frame_encode(&fconfig, wire_buf, wire_written,
                                            g_pcm_src, PCM_BUF_SIZE, NULL);
        if (frame_samples == 0u) continue;

        /* Apply impairment */
        if (imp != NULL) {
            exp001_impairment_config_t trial_imp = *imp;
            trial_imp.rng_seed = 10000u + tr * 37u + (uint32_t)type * 101u;
            exp001_apply_impairments(&trial_imp, g_pcm_src, frame_samples,
                                     g_pcm_imp, PCM_BUF_SIZE, &impaired_samples);
        } else {
            memcpy(g_pcm_imp, g_pcm_src, frame_samples * sizeof(float));
            impaired_samples = frame_samples;
        }

        res->trials++;

        /* 1. Evaluate Preamble Acquisition (independent of payload) */
        size_t acq_window = 4800u + 2400u;
        if (acq_window > impaired_samples) acq_window = impaired_samples;
        exp001_preamble_detect_t det = exp001_detect_preamble_iq(
            type, dur, 2000.0, 6000.0, threshold_gamma, g_pcm_imp, acq_window);

        res->total_corr += det.peak_correlation;

        if (det.detected != 0u) {
            res->detections++;
            double err = fabs((double)det.peak_sample_index - (double)true_lead);
            res->total_timing_error += err;
            if (err > res->max_timing_error) {
                res->max_timing_error = err;
            }
        }

        /* 2. Downstream FSK payload decode */
        st = exp001_frame_decode(&fconfig, g_pcm_imp, impaired_samples,
                                 rx_payload, sizeof(rx_payload), &dec_res);
        if (st == EXP001_OK && dec_res.crc_valid != 0u) {
            res->payload_crc_pass++;
        }
    }

    res->pd = (res->trials > 0u) ? (double)res->detections / (double)res->trials : 0.0;
}

static void print_scenario_table(const char *scenario_name, eval_result_t results[4])
{
    unsigned t;
    printf("\n=== %s ===\n", scenario_name);
    printf("%-14s | %6s | %6s | %6s | %10s | %10s | %8s | %10s\n",
           "Candidate", "Trials", "Detect", "Pd", "MeanTimErr", "MaxTimErr", "AvgCorr", "PayloadCRC");
    printf("%-14s-+-%6s-+-%6s-+-%6s-+-%10s-+-%10s-+-%8s-+-%10s\n",
           "--------------", "------", "------", "------", "----------", "----------", "--------", "----------");

    for (t = 0u; t < 4u; ++t) {
        double avg_err = (results[t].detections > 0u) ?
            results[t].total_timing_error / (double)results[t].detections : 0.0;
        double avg_corr = (results[t].trials > 0u) ?
            results[t].total_corr / (double)results[t].trials : 0.0;

        printf("%-14s | %6u | %6u | %6.3f | %10.2f | %10.1f | %8.3f | %6u/%u\n",
               results[t].name,
               results[t].trials,
               results[t].detections,
               results[t].pd,
               avg_err,
               results[t].max_timing_error,
               avg_corr,
               results[t].payload_crc_pass,
               results[t].trials);
    }
    fflush(stdout);
}

int main(void)
{
    const char *names[4] = { "LFM_CHIRP", "ZC_DERIVED", "PN_MSEQ", "FREQ_DIVERSE" };
    calib_result_t calib[4];
    eval_result_t results[4];
    exp001_impairment_config_t imp;
    unsigned t;

    printf("MCL-AP Experiment 001: Corrected Preamble Bakeoff\n");
    printf("===================================================\n");
    printf("Status: LAB / EXPERIMENTAL — Resource-Equalized Benchmarking\n\n");

    /* Step 1: Calibration */
    printf("--- Empirical False-Alarm Calibration (Target Pfa = %.3f per 9600-sample window) ---\n", TARGET_PFA);
    printf("%-14s | %10s | %10s | %10s | %12s\n",
           "Candidate", "Threshold", "NoiseTr", "FalseAlarms", "EmpiricalPfa");
    printf("%-14s-+-%10s-+-%10s-+-%10s-+-%12s\n",
           "--------------", "----------", "----------", "----------", "------------");

    for (t = 0u; t < 4u; ++t) {
        calibrate_candidate_pfa((exp001_preamble_type_t)t, names[t], &calib[t]);
        printf("%-14s | %10.4f | %10u | %10u | %12.4f\n",
               calib[t].name,
               calib[t].threshold_gamma,
               calib[t].noise_verify_trials,
               calib[t].observed_false_alarms,
               calib[t].empirical_pfa);
        fflush(stdout);
    }

    /* Scenario 1: Clean Channel */
    for (t = 0u; t < 4u; ++t) {
        results[t].name = names[t];
        eval_candidate_scenario((exp001_preamble_type_t)t, calib[t].threshold_gamma, NULL, 12u, &results[t]);
        results[t].name = names[t];
    }
    print_scenario_table("Clean Channel Baseline", results);

    /* Scenario 2: AWGN Sweeps (30, 20, 10, 5, 0 dB) */
    {
        double snrs[] = { 30.0, 20.0, 10.0, 5.0, 0.0 };
        unsigned s;
        char title[64];
        for (s = 0u; s < sizeof(snrs)/sizeof(snrs[0]); ++s) {
            memset(&imp, 0, sizeof(imp));
            imp.enable_awgn = 1u;
            imp.awgn_snr_db = snrs[s];
            sprintf(title, "AWGN SNR = %g dB", snrs[s]);

            for (t = 0u; t < 4u; ++t) {
                eval_candidate_scenario((exp001_preamble_type_t)t, calib[t].threshold_gamma, &imp, 12u, &results[t]);
                results[t].name = names[t];
            }
            print_scenario_table(title, results);
        }
    }

    /* Scenario 3: Colored Noise (15 dB SNR) */
    {
        memset(&imp, 0, sizeof(imp));
        imp.enable_colored_noise = 1u;
        imp.colored_noise_snr_db = 15.0;
        for (t = 0u; t < 4u; ++t) {
            eval_candidate_scenario((exp001_preamble_type_t)t, calib[t].threshold_gamma, &imp, 12u, &results[t]);
            results[t].name = names[t];
        }
        print_scenario_table("Colored Noise (15 dB SNR)", results);
    }

    /* Scenario 4: 2-Path Multipath (5 ms delay, 0.35 gain) */
    {
        memset(&imp, 0, sizeof(imp));
        imp.num_multipath_paths = 2u;
        imp.multipath_delays_ms[1] = 5.0;
        imp.multipath_gains[1] = 0.35;

        for (t = 0u; t < 4u; ++t) {
            eval_candidate_scenario((exp001_preamble_type_t)t, calib[t].threshold_gamma, &imp, 12u, &results[t]);
            results[t].name = names[t];
        }
        print_scenario_table("2-Path Multipath (5 ms, gain 0.35)", results);
    }

    /* Scenario 5: 5-Path Multipath (0-30 ms bounded delay) */
    {
        memset(&imp, 0, sizeof(imp));
        imp.num_multipath_paths = 5u;
        imp.multipath_delays_ms[1] = 3.0;  imp.multipath_gains[1] = 0.25;
        imp.multipath_delays_ms[2] = 8.0;  imp.multipath_gains[2] = 0.20;
        imp.multipath_delays_ms[3] = 17.0; imp.multipath_gains[3] = 0.15;
        imp.multipath_delays_ms[4] = 27.0; imp.multipath_gains[4] = 0.10;

        for (t = 0u; t < 4u; ++t) {
            eval_candidate_scenario((exp001_preamble_type_t)t, calib[t].threshold_gamma, &imp, 12u, &results[t]);
            results[t].name = names[t];
        }
        print_scenario_table("5-Path Multipath (0-30 ms bounded)", results);
    }

    /* Scenario 6: Band Attenuation (-10 dB notch at 4000 Hz) */
    {
        memset(&imp, 0, sizeof(imp));
        imp.enable_band_atten = 1u;
        imp.band_atten_f_center_hz = 4000.0;
        imp.band_atten_bandwidth_hz = 400.0;
        imp.band_atten_gain_db = -10.0;

        for (t = 0u; t < 4u; ++t) {
            eval_candidate_scenario((exp001_preamble_type_t)t, calib[t].threshold_gamma, &imp, 12u, &results[t]);
            results[t].name = names[t];
        }
        print_scenario_table("Band Attenuation (-10 dB Notch @ 4000 Hz)", results);
    }

    /* Scenario 7: Clipping (threshold = 0.5) */
    {
        memset(&imp, 0, sizeof(imp));
        imp.enable_clipping = 1u;
        imp.clipping_threshold = 0.5;

        for (t = 0u; t < 4u; ++t) {
            eval_candidate_scenario((exp001_preamble_type_t)t, calib[t].threshold_gamma, &imp, 12u, &results[t]);
            results[t].name = names[t];
        }
        print_scenario_table("Clipping (threshold = 0.5)", results);
    }

    /* Scenario 8: Sample Rate Offset (+50 ppm) */
    {
        memset(&imp, 0, sizeof(imp));
        imp.enable_sro = 1u;
        imp.sample_rate_offset_ppm = 50.0;

        for (t = 0u; t < 4u; ++t) {
            eval_candidate_scenario((exp001_preamble_type_t)t, calib[t].threshold_gamma, &imp, 12u, &results[t]);
            results[t].name = names[t];
        }
        print_scenario_table("Sample Rate Offset (+50 ppm)", results);
    }

    /* Scenario 9: Combined Impairments (2-path + SRO + 30 dB AWGN) */
    {
        memset(&imp, 0, sizeof(imp));
        imp.num_multipath_paths = 2u;
        imp.multipath_delays_ms[1] = 4.0;
        imp.multipath_gains[1] = 0.25;
        imp.enable_sro = 1u;
        imp.sample_rate_offset_ppm = 25.0;
        imp.enable_awgn = 1u;
        imp.awgn_snr_db = 30.0;

        for (t = 0u; t < 4u; ++t) {
            eval_candidate_scenario((exp001_preamble_type_t)t, calib[t].threshold_gamma, &imp, 12u, &results[t]);
            results[t].name = names[t];
        }
        print_scenario_table("Combined Mild (2-path + SRO + 30dB AWGN)", results);
    }

    printf("\n===================================================\n");
    printf("Bakeoff complete. All metrics recorded under equalized resources.\n");
    printf("Status: LAB / EXPERIMENTAL — NOT AP-B0.\n");
    return 0;
}
