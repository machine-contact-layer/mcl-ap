/*
 * MCL-AP Experiment 001: Preamble Bakeoff
 *
 * Compare four candidate preamble types under controlled impairments.
 * LAB / EXPERIMENTAL only — NOT a normative AP profile selection.
 *
 * Metrics per preamble type:
 *   - Detection probability (Pd) at fixed false-alarm rate
 *   - Timing error (samples)
 *   - Robustness under: AWGN, clipping, sample-rate offset
 *
 * Build:
 *   cl /std:c11 /W4 /O2 /D_CRT_SECURE_NO_WARNINGS \
 *      /I../../include /I../../../mcl-wire/include \
 *      exp001.c bakeoff.c ../../../mcl-wire/src/wire.c \
 *      ../../../mcl-wire/src/extension.c /Fe:bakeoff.exe
 */

#include "exp001.h"
#include "mcl/wire.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

#define PCM_BUF_SIZE 960000u
static float g_pcm_source[PCM_BUF_SIZE];
static float g_pcm_impaired[PCM_BUF_SIZE];

typedef struct {
    const char *name;
    unsigned detections;
    unsigned crc_passes;
    unsigned bit_perfect;
    unsigned trials;
    double total_timing_error;
} bakeoff_result_t;

/*
 * Run a single preamble trial: encode PRESENCE, apply impairment, decode.
 */
static void run_trial(
    exp001_preamble_type_t preamble_type,
    const exp001_impairment_config_t *imp,
    bakeoff_result_t *result)
{
    mcl_wire_tier0_t obj;
    uint8_t wire_buf[MCL_WIRE_TIER0_MAX_SIZE];
    size_t wire_written = 0u;
    uint8_t recovered[MCL_WIRE_TIER0_MAX_SIZE];
    exp001_frame_config_t fconfig;
    exp001_decode_result_t decode_result;
    size_t frame_samples;
    exp001_status_t est;
    mcl_wire_status_t wst;

    /* Build PRESENCE frame */
    memset(&obj, 0, sizeof(obj));
    obj.kind = MCL_WIRE_KIND_PRESENCE;
    obj.priority = 1u;
    obj.source_ref = 0xBABECAFEu;
    obj.body.presence.machine_class = 3u;
    obj.body.presence.capability_digest = 0x00FFAAu;
    obj.body.presence.ttl = 45u;

    wst = mcl_wire_tier0_encode(&obj, wire_buf, sizeof(wire_buf), &wire_written);
    if (wst != MCL_WIRE_OK) return;

    fconfig.preamble_type = preamble_type;
    fconfig.preamble_duration_s = 0.1;
    fconfig.preamble_f_start_hz = 2000.0;
    fconfig.preamble_f_end_hz = 6000.0;
    fconfig.silence_duration_s = 0.05;

    frame_samples = exp001_frame_encode(
        &fconfig, wire_buf, wire_written,
        g_pcm_source, PCM_BUF_SIZE);
    if (frame_samples == 0u) return;

    /* Apply impairment */
    memcpy(g_pcm_impaired, g_pcm_source, frame_samples * sizeof(float));
    if (imp != NULL) {
        exp001_apply_impairments(imp, g_pcm_impaired, frame_samples);
    }

    /* Decode */
    est = exp001_frame_decode(
        &fconfig, g_pcm_impaired, frame_samples,
        recovered, sizeof(recovered), &decode_result);

    result->trials++;

    if (est != EXP001_ERR_PREAMBLE_NOT_FOUND &&
        est != EXP001_ERR_SYNC_FAILURE) {
        result->detections++;
        /* Approximate timing error using preamble_end_sample vs expected */
        {
            size_t expected_end = (size_t)(0.1 * EXP001_SAMPLE_RATE);
            double err = (double)decode_result.preamble_end_sample - (double)expected_end;
            result->total_timing_error += fabs(err);
        }
    }

    if (est == EXP001_OK && decode_result.crc_valid) {
        result->crc_passes++;
        if (decode_result.payload_bytes == wire_written &&
            memcmp(wire_buf, recovered, wire_written) == 0) {
            result->bit_perfect++;
        }
    }
}

static void print_table_header(void)
{
    printf("%-14s | %6s | %6s | %6s | %6s | %10s\n",
           "Preamble", "Trials", "Detect", "CRC_OK", "BitPfct", "AvgTimErr");
    printf("%-14s-+-%6s-+-%6s-+-%6s-+-%6s-+-%10s\n",
           "--------------", "------", "------", "------", "------", "----------");
}

static void print_result(const bakeoff_result_t *r)
{
    double avg_timing = (r->detections > 0) ?
        r->total_timing_error / (double)r->detections : 0.0;

    printf("%-14s | %6u | %6u | %6u | %6u | %10.1f\n",
           r->name, r->trials, r->detections,
           r->crc_passes, r->bit_perfect, avg_timing);
}

static void run_scenario(const char *scenario_name,
                         const exp001_impairment_config_t *imp,
                         unsigned n_seeds)
{
    const char *names[] = { "LFM_CHIRP", "ZADOFF_CHU", "PN_MSEQ", "FREQ_DIVERSE" };
    bakeoff_result_t results[4];
    unsigned p, s;

    printf("\n=== %s ===\n", scenario_name);

    memset(results, 0, sizeof(results));
    for (p = 0u; p < 4u; ++p) {
        results[p].name = names[p];
    }

    for (p = 0u; p < 4u; ++p) {
        for (s = 0u; s < n_seeds; ++s) {
            exp001_impairment_config_t trial_imp;
            if (imp != NULL) {
                trial_imp = *imp;
                trial_imp.rng_seed = 1000u + s * 7u + p * 31u;
            }
            run_trial((exp001_preamble_type_t)p,
                      imp != NULL ? &trial_imp : NULL,
                      &results[p]);
        }
    }

    print_table_header();
    for (p = 0u; p < 4u; ++p) {
        print_result(&results[p]);
    }
}

int main(void)
{
    exp001_impairment_config_t imp;

    printf("MCL-AP Experiment 001: Preamble Bakeoff\n");
    printf("=======================================\n");
    printf("Status: LAB / EXPERIMENTAL — NOT AP-B0 selection\n");

    /* Scenario 1: Clean channel baseline */
    run_scenario("Clean Channel", NULL, 1u);

    /* Scenario 2: AWGN sweep */
    {
        double snr_levels[] = { 40.0, 30.0, 20.0, 15.0, 10.0 };
        unsigned i;
        char label[64];

        for (i = 0u; i < sizeof(snr_levels)/sizeof(snr_levels[0]); ++i) {
            memset(&imp, 0, sizeof(imp));
            imp.awgn_snr_db = snr_levels[i];
            imp.clipping_threshold = 1.0;
            sprintf(label, "AWGN SNR=%g dB", snr_levels[i]);
            run_scenario(label, &imp, 5u);
        }
    }

    /* Scenario 3: Clipping sweep */
    {
        double clip_levels[] = { 0.9, 0.7, 0.5, 0.3 };
        unsigned i;
        char label[64];

        for (i = 0u; i < sizeof(clip_levels)/sizeof(clip_levels[0]); ++i) {
            memset(&imp, 0, sizeof(imp));
            imp.clipping_threshold = clip_levels[i];
            sprintf(label, "Clipping %.1f", clip_levels[i]);
            run_scenario(label, &imp, 5u);
        }
    }

    /* Scenario 4: Sample rate offset sweep */
    {
        double sro_levels[] = { 10.0, 50.0, 100.0, 500.0 };
        unsigned i;
        char label[64];

        for (i = 0u; i < sizeof(sro_levels)/sizeof(sro_levels[0]); ++i) {
            memset(&imp, 0, sizeof(imp));
            imp.sample_rate_offset_ppm = sro_levels[i];
            imp.clipping_threshold = 1.0;
            sprintf(label, "SRO +%g ppm", sro_levels[i]);
            run_scenario(label, &imp, 5u);
        }
    }

    printf("\n=======================================\n");
    printf("Bakeoff complete. Review results to inform AP-B0 selection.\n");
    printf("These results are EXPERIMENTAL and do NOT define MCL-AP.\n");

    return 0;
}
