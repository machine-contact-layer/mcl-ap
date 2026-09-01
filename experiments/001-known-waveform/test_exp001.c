/*
 * MCL-AP Experiment 001: Unit and Pipeline Test Suite
 *
 * Tests:
 *   1. Preamble exact length invariant (all 4 candidates)
 *   2. PN m-sequence properties (all 127 states, period 127, non-zero)
 *   3. Preamble energy equalization (sample count, RMS, peak, energy)
 *   4. Polarity invariance (source vs -source)
 *   5. Variable leading silence timing error (0, 17, 53, 101, 319, 1000 samples)
 *   6. SRO out-of-place resampler analytical displacement (-500 to +500 ppm)
 *   7. Symbol timing acquisition under SRO (-500 to +500 ppm)
 *   8. Full clean-channel Wire -> Acoustic -> Wire pipeline (all 6 Tier-0 kinds)
 *   9. Expanded impairments: colored noise, 2-path, 5-path, band attenuation, combined
 *  10. E3 source WAV generator with training sequence
 */

#include "exp001.h"
#include "mcl/wire.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

static int tests_run = 0;
static int tests_passed = 0;

#define PCM_BUF_SIZE 960000u
static float g_pcm_a[PCM_BUF_SIZE];
static float g_pcm_b[PCM_BUF_SIZE];

#define TEST(name) do { \
    printf("  [TEST] %s ... ", #name); \
    ++tests_run; \
} while (0)

#define PASS() do { \
    printf("PASS\n"); \
    ++tests_passed; \
} while (0)

#define FAIL(msg) do { \
    printf("FAIL: %s\n", msg); \
    return; \
} while (0)

/* ========== Test 1: Preamble Length Invariant ========== */

static void test_preamble_length_invariant(void)
{
    const double durations[] = { 0.05, 0.10, 0.20, 0.25 };
    unsigned d, t;

    for (t = 0u; t < EXP001_PREAMBLE_TYPE_COUNT; ++t) {
        for (d = 0u; d < sizeof(durations)/sizeof(durations[0]); ++d) {
            double dur = durations[d];
            size_t expected = (size_t)floor(dur * (double)EXP001_SAMPLE_RATE + 0.5);
            size_t actual;

            TEST(preamble_length_invariant);
            printf("(type=%u, dur=%.2fs, exp=%zu) ... ", t, dur, expected);

            actual = exp001_generate_preamble(
                (exp001_preamble_type_t)t, dur, 2000.0, 6000.0,
                g_pcm_a, PCM_BUF_SIZE);

            if (actual != expected) {
                printf("FAIL: actual=%zu != expected=%zu\n", actual, expected);
                return;
            }
            PASS();
        }
    }
}

/* ========== Test 2: PN m-sequence Properties ========== */

static void test_mseq_properties(void)
{
    TEST(mseq_lfsr_properties);
    if (exp001_verify_mseq_properties() == 1u) {
        PASS();
    } else {
        FAIL("m-sequence failed 127-state maximal period test");
    }
}

/* ========== Test 3: Preamble Energy Equalization ========== */

static void test_energy_equalization(void)
{
    unsigned t;
    const double dur = 0.1;
    const size_t N = (size_t)floor(dur * (double)EXP001_SAMPLE_RATE + 0.5);
    const double target_e = (double)N * EXP001_TARGET_ENERGY_PER_SAMPLE;

    for (t = 0u; t < EXP001_PREAMBLE_TYPE_COUNT; ++t) {
        exp001_preamble_energy_t metrics;
        exp001_status_t st;
        size_t gen_n;

        TEST(energy_equalization);
        printf("(type=%u) ... ", t);

        gen_n = exp001_generate_preamble((exp001_preamble_type_t)t, dur, 2000.0, 6000.0,
                                         g_pcm_a, PCM_BUF_SIZE);
        if (gen_n != N) FAIL("preamble length mismatch");

        st = exp001_equalize_preamble_energy(g_pcm_a, gen_n, target_e, &metrics);
        if (st != EXP001_OK) FAIL("equalize failed");

        /* Tolerance check: relative energy error < 1e-4 */
        double rel_err = fabs(metrics.energy - target_e) / target_e;
        if (rel_err > 1e-4) FAIL("energy tolerance exceeded");
        if (metrics.peak > 1.05f) FAIL("peak amplitude exceeds limit");
        if (metrics.sample_count != N) FAIL("sample count mismatch in metrics");

        printf("[N=%zu RMS=%.4f Peak=%.4f E=%.1f] ",
               metrics.sample_count, metrics.rms, (double)metrics.peak, metrics.energy);
        PASS();
    }
}

/* ========== Test 4: Polarity Invariance ========== */

static void test_polarity_invariance(void)
{
    unsigned t;
    const double dur = 0.1;
    const size_t N = (size_t)floor(dur * (double)EXP001_SAMPLE_RATE + 0.5);
    size_t i;

    for (t = 0u; t < EXP001_PREAMBLE_TYPE_COUNT; ++t) {
        exp001_preamble_detect_t det_pos, det_neg;

        TEST(polarity_invariance);
        printf("(type=%u) ... ", t);

        exp001_generate_preamble((exp001_preamble_type_t)t, dur, 2000.0, 6000.0,
                                 g_pcm_a, PCM_BUF_SIZE);
        for (i = 0u; i < N; ++i) {
            g_pcm_b[i] = -g_pcm_a[i];
        }

        det_pos = exp001_detect_preamble_iq((exp001_preamble_type_t)t, dur, 2000.0, 6000.0,
                                            0.5, g_pcm_a, N);
        det_neg = exp001_detect_preamble_iq((exp001_preamble_type_t)t, dur, 2000.0, 6000.0,
                                            0.5, g_pcm_b, N);

        if (det_pos.detected == 0u || det_neg.detected == 0u) {
            FAIL("detection failed on pos or neg");
        }
        if (det_pos.peak_sample_index != det_neg.peak_sample_index) {
            FAIL("timing offset differs under polarity inversion");
        }
        if (fabs(det_pos.peak_correlation - det_neg.peak_correlation) > 1e-4) {
            FAIL("correlation magnitude differs under polarity inversion");
        }

        PASS();
    }
}

/* ========== Test 5: Variable Leading Silence Timing Error ========== */

static void test_variable_leading_silence(void)
{
    const size_t offsets[] = { 0u, 17u, 53u, 101u, 319u, 1000u };
    unsigned t, o;
    const double dur = 0.1;
    const size_t N = (size_t)floor(dur * (double)EXP001_SAMPLE_RATE + 0.5);

    for (t = 0u; t < EXP001_PREAMBLE_TYPE_COUNT; ++t) {
        for (o = 0u; o < sizeof(offsets)/sizeof(offsets[0]); ++o) {
            size_t true_start = offsets[o];
            size_t total_len = true_start + N + 1000u;
            exp001_preamble_detect_t det;

            TEST(variable_leading_silence);
            printf("(type=%u, lead=%zu) ... ", t, true_start);

            memset(g_pcm_a, 0, total_len * sizeof(float));
            exp001_generate_preamble((exp001_preamble_type_t)t, dur, 2000.0, 6000.0,
                                     g_pcm_a + true_start, N);

            det = exp001_detect_preamble_iq((exp001_preamble_type_t)t, dur, 2000.0, 6000.0,
                                            0.5, g_pcm_a, total_len);

            if (det.detected == 0u) {
                FAIL("preamble not detected");
            }

            int timing_err = (int)det.peak_sample_index - (int)true_start;
            if (abs(timing_err) > 2) {
                printf("FAIL: timing error %d > 2 samples (est=%zu, true=%zu)\n",
                       timing_err, det.peak_sample_index, true_start);
                return;
            }
            PASS();
        }
    }
}

/* ========== Test 6: SRO Out-of-place Resampler Analytical Verification ========== */

static void test_sro_resampler_analytical(void)
{
    const double ppms[] = { -500.0, -100.0, -50.0, -10.0, 0.0, 10.0, 50.0, 100.0, 500.0 };
    unsigned p;
    const size_t N = 48000u; /* 1 second of audio */
    size_t i;

    /* Fill source with a ramp x[n] = n */
    for (i = 0u; i < N; ++i) {
        g_pcm_a[i] = (float)i;
    }

    for (p = 0u; p < sizeof(ppms)/sizeof(ppms[0]); ++p) {
        double ppm = ppms[p];
        size_t n_out = 0u;
        exp001_status_t st;

        TEST(sro_resampler_analytical);
        printf("(ppm=%+6.0f) ... ", ppm);

        st = exp001_resample_sro(g_pcm_a, N, ppm, g_pcm_b, PCM_BUF_SIZE, &n_out);
        if (st != EXP001_OK) FAIL("resample failed");

        /*
         * Analytically: at output sample k, src_idx = k * (1 + ppm * 1e-6).
         * For the ramp signal, dst[k] == src_idx exactly (linear interpolation is exact for a linear ramp!).
         * Check displacement at k = 40000:
         */
        size_t k = 10000u;
        if (k < n_out) {
            double expected_idx = (double)k * (1.0 + ppm * 1e-6);
            double actual_val = (double)g_pcm_b[k];
            double err = fabs(actual_val - expected_idx);
            if (err > 0.01) {
                printf("FAIL: displacement error %g (act=%g, exp=%g)\n", err, actual_val, expected_idx);
                return;
            }
        }

        PASS();
    }
}

/* ========== Test 7: Symbol Timing Acquisition Under SRO ========== */

static void test_sro_receiver_timing_acquisition(void)
{
    const double ppms[] = { -500.0, -100.0, -50.0, -10.0, 0.0, 10.0, 50.0, 100.0, 500.0 };
    unsigned p;
    mcl_wire_tier0_t src_obj, dst_obj;
    uint8_t wire_buf[MCL_WIRE_TIER0_MAX_SIZE];
    size_t wire_written = 0u, wire_consumed = 0u;
    uint8_t recovered[MCL_WIRE_TIER0_MAX_SIZE];
    exp001_frame_config_t fconfig;
    exp001_decode_result_t decode_res;
    size_t frame_samples, resampled_samples;
    exp001_status_t st;

    memset(&src_obj, 0, sizeof(src_obj));
    src_obj.kind = MCL_WIRE_KIND_PRESENCE;
    src_obj.priority = 1u;
    src_obj.source_ref = 0x12345678u;
    src_obj.body.presence.machine_class = 1u;
    src_obj.body.presence.capability_digest = 0x00AABBu;
    src_obj.body.presence.ttl = 60u;

    mcl_wire_tier0_encode(&src_obj, wire_buf, sizeof(wire_buf), &wire_written);

    fconfig.preamble_type = EXP001_PREAMBLE_LFM_CHIRP;
    fconfig.preamble_duration_s = 0.1;
    fconfig.preamble_f_start_hz = 2000.0;
    fconfig.preamble_f_end_hz = 6000.0;
    fconfig.leading_silence_s = 0.01;
    fconfig.silence_duration_s = 0.05;
    fconfig.include_training = 1u; /* Enable training sequence for symbol timing acquisition */
    fconfig.detection_threshold = 0.5;

    frame_samples = exp001_frame_encode(&fconfig, wire_buf, wire_written,
                                        g_pcm_a, PCM_BUF_SIZE, NULL);
    if (frame_samples == 0u) return;

    for (p = 0u; p < sizeof(ppms)/sizeof(ppms[0]); ++p) {
        double ppm = ppms[p];

        TEST(sro_receiver_timing_acquisition);
        printf("(ppm=%+6.0f) ... ", ppm);

        /* Out-of-place SRO resample */
        st = exp001_resample_sro(g_pcm_a, frame_samples, ppm,
                                g_pcm_b, PCM_BUF_SIZE, &resampled_samples);
        if (st != EXP001_OK) FAIL("resample failed");

        /* Frame decode with timing search */
        st = exp001_frame_decode(&fconfig, g_pcm_b, resampled_samples,
                                 recovered, sizeof(recovered), &decode_res);

        if (st != EXP001_OK || decode_res.crc_valid == 0u) {
            printf("FAIL (status=%d, crc_valid=%u)\n", (int)st, (unsigned)decode_res.crc_valid);
            return;
        }

        if (memcmp(wire_buf, recovered, wire_written) != 0) {
            FAIL("payload bytes mismatch");
        }

        mcl_wire_tier0_decode(recovered, decode_res.payload_bytes, &dst_obj, &wire_consumed);
        if (dst_obj.kind != src_obj.kind || dst_obj.source_ref != src_obj.source_ref) {
            FAIL("semantic object mismatch");
        }

        printf("[sps=%.2f phase=%.1f] ",
               decode_res.estimated_samples_per_symbol, decode_res.estimated_symbol_phase);
        PASS();
    }
}

/* ========== Test 8: Full Clean-Channel Pipeline (All 6 Tier-0 Kinds) ========== */

static void test_full_clean_pipeline(void)
{
    mcl_wire_kind_t kinds[6] = {
        MCL_WIRE_KIND_PRESENCE,
        MCL_WIRE_KIND_HAZARD,
        MCL_WIRE_KIND_REQUEST,
        MCL_WIRE_KIND_AUTHORITY_CLAIM,
        MCL_WIRE_KIND_DEGRADED_STATE,
        MCL_WIRE_KIND_TRANSPORT_OFFER
    };
    const char *names[6] = {
        "PRESENCE", "HAZARD", "REQUEST", "AUTHORITY_CLAIM", "DEGRADED_STATE", "TRANSPORT_OFFER"
    };
    unsigned k;

    for (k = 0u; k < 6u; ++k) {
        mcl_wire_tier0_t src_obj, dst_obj;
        uint8_t wire_buf[MCL_WIRE_TIER0_MAX_SIZE];
        size_t wire_written = 0u, wire_consumed = 0u;
        uint8_t recovered[MCL_WIRE_TIER0_MAX_SIZE];
        exp001_frame_config_t fconfig;
        exp001_decode_result_t decode_res;
        size_t frame_samples;
        exp001_status_t st;

        TEST(clean_pipeline_tier0);
        printf("(%s) ... ", names[k]);

        memset(&src_obj, 0, sizeof(src_obj));
        src_obj.kind = kinds[k];
        src_obj.priority = 1u;
        src_obj.source_ref = 0xCAFE0001u + k;

        switch (kinds[k]) {
        case MCL_WIRE_KIND_PRESENCE:
            src_obj.body.presence.machine_class = 2u;
            src_obj.body.presence.capability_digest = 0x123456u;
            src_obj.body.presence.ttl = 45u;
            break;
        case MCL_WIRE_KIND_HAZARD:
            src_obj.body.hazard.hazard_class = 1u;
            src_obj.body.hazard.severity = 4u;
            src_obj.body.hazard.confidence = 80u;
            src_obj.body.hazard.x = 20;
            src_obj.body.hazard.y = -30;
            src_obj.body.hazard.z = 10;
            src_obj.body.hazard.radius = 100u;
            src_obj.body.hazard.ttl = 15u;
            break;
        case MCL_WIRE_KIND_REQUEST:
            src_obj.body.request.request_class = 3u;
            src_obj.body.request.target_ref = 0x99887766u;
            src_obj.body.request.x = -100;
            src_obj.body.request.y = 200;
            src_obj.body.request.radius = 50u;
            src_obj.body.request.ttl = 60u;
            break;
        case MCL_WIRE_KIND_AUTHORITY_CLAIM:
            src_obj.body.authority_claim.authority_class = 5u;
            src_obj.body.authority_claim.jurisdiction = 500u;
            src_obj.body.authority_claim.credential_ref = 0xAABBCCDDu;
            src_obj.body.authority_claim.validity = 120u;
            break;
        case MCL_WIRE_KIND_DEGRADED_STATE:
            src_obj.body.degraded_state.affected_capability = 2u;
            src_obj.body.degraded_state.health = 50u;
            src_obj.body.degraded_state.severity = 3u;
            src_obj.body.degraded_state.ttl = 30u;
            break;
        case MCL_WIRE_KIND_TRANSPORT_OFFER:
            src_obj.body.transport_offer.transport_id = 2u;
            src_obj.body.transport_offer.profile_id = 1u;
            src_obj.body.transport_offer.endpoint_token = 0x55667788u;
            src_obj.body.transport_offer.validity = 90u;
            break;
        default:
            break;
        }

        mcl_wire_tier0_encode(&src_obj, wire_buf, sizeof(wire_buf), &wire_written);

        fconfig.preamble_type = EXP001_PREAMBLE_LFM_CHIRP;
        fconfig.preamble_duration_s = 0.1;
        fconfig.preamble_f_start_hz = 2000.0;
        fconfig.preamble_f_end_hz = 6000.0;
        fconfig.leading_silence_s = 0.02;
        fconfig.silence_duration_s = 0.05;
        fconfig.include_training = 1u;
        fconfig.detection_threshold = 0.5;

        frame_samples = exp001_frame_encode(&fconfig, wire_buf, wire_written,
                                            g_pcm_a, PCM_BUF_SIZE, NULL);
        if (frame_samples == 0u) FAIL("encode returned 0");

        st = exp001_frame_decode(&fconfig, g_pcm_a, frame_samples,
                                 recovered, sizeof(recovered), &decode_res);
        if (st != EXP001_OK || decode_res.crc_valid == 0u) {
            FAIL("decode or CRC failed");
        }

        if (memcmp(wire_buf, recovered, wire_written) != 0) {
            FAIL("byte payload mismatch");
        }

        mcl_wire_tier0_decode(recovered, decode_res.payload_bytes, &dst_obj, &wire_consumed);
        if (dst_obj.kind != src_obj.kind || dst_obj.source_ref != src_obj.source_ref) {
            FAIL("semantic decode mismatch");
        }

        PASS();
    }
}

/* ========== Test 9: Expanded Deterministic Impairments ========== */

static void test_expanded_impairments(void)
{
    mcl_wire_tier0_t src_obj;
    uint8_t wire_buf[MCL_WIRE_TIER0_MAX_SIZE];
    size_t wire_written = 0u;
    uint8_t recovered[MCL_WIRE_TIER0_MAX_SIZE];
    exp001_frame_config_t fconfig;
    exp001_decode_result_t decode_res;
    exp001_impairment_config_t imp;
    size_t frame_samples, impaired_samples;
    exp001_status_t st;

    memset(&src_obj, 0, sizeof(src_obj));
    src_obj.kind = MCL_WIRE_KIND_PRESENCE;
    src_obj.priority = 1u;
    src_obj.source_ref = 0xFEEDFACEu;
    src_obj.body.presence.machine_class = 1u;
    src_obj.body.presence.capability_digest = 0x000102u;
    src_obj.body.presence.ttl = 30u;

    mcl_wire_tier0_encode(&src_obj, wire_buf, sizeof(wire_buf), &wire_written);

    fconfig.preamble_type = EXP001_PREAMBLE_LFM_CHIRP;
    fconfig.preamble_duration_s = 0.1;
    fconfig.preamble_f_start_hz = 2000.0;
    fconfig.preamble_f_end_hz = 6000.0;
    fconfig.leading_silence_s = 0.02;
    fconfig.silence_duration_s = 0.05;
    fconfig.include_training = 1u;
    fconfig.detection_threshold = 0.45;

    frame_samples = exp001_frame_encode(&fconfig, wire_buf, wire_written,
                                        g_pcm_a, PCM_BUF_SIZE, NULL);

    /* 1. Colored noise (25 dB SNR) */
    {
        TEST(impairment_colored_noise);
        memset(&imp, 0, sizeof(imp));
        imp.colored_noise_snr_db = 25.0;
        imp.rng_seed = 1001u;

        exp001_apply_impairments(&imp, g_pcm_a, frame_samples, g_pcm_b, PCM_BUF_SIZE, &impaired_samples);
        st = exp001_frame_decode(&fconfig, g_pcm_b, impaired_samples, recovered, sizeof(recovered), &decode_res);
        if (st == EXP001_OK && decode_res.crc_valid != 0u) {
            PASS();
        } else {
            FAIL("colored noise decode failed");
        }
    }

    /* 2. Simple 2-path multipath (delay 5 ms, gain 0.35) */
    {
        TEST(impairment_2path_multipath);
        memset(&imp, 0, sizeof(imp));
        imp.num_multipath_paths = 2u;
        imp.multipath_delays_ms[1] = 5.0;
        imp.multipath_gains[1] = 0.35;

        exp001_apply_impairments(&imp, g_pcm_a, frame_samples, g_pcm_b, PCM_BUF_SIZE, &impaired_samples);
        st = exp001_frame_decode(&fconfig, g_pcm_b, impaired_samples, recovered, sizeof(recovered), &decode_res);
        if (st == EXP001_OK && decode_res.crc_valid != 0u) {
            PASS();
        } else {
            FAIL("2-path multipath decode failed");
        }
    }

    /* 3. 5-path multipath with bounded 0-30 ms delay */
    {
        TEST(impairment_5path_multipath);
        memset(&imp, 0, sizeof(imp));
        imp.num_multipath_paths = 5u;
        imp.multipath_delays_ms[1] = 3.0;  imp.multipath_gains[1] = 0.25;
        imp.multipath_delays_ms[2] = 8.0;  imp.multipath_gains[2] = 0.20;
        imp.multipath_delays_ms[3] = 17.0; imp.multipath_gains[3] = 0.15;
        imp.multipath_delays_ms[4] = 27.0; imp.multipath_gains[4] = 0.10;

        exp001_apply_impairments(&imp, g_pcm_a, frame_samples, g_pcm_b, PCM_BUF_SIZE, &impaired_samples);
        st = exp001_frame_decode(&fconfig, g_pcm_b, impaired_samples, recovered, sizeof(recovered), &decode_res);
        if (st == EXP001_OK && decode_res.crc_valid != 0u) {
            PASS();
        } else {
            FAIL("5-path multipath decode failed");
        }
    }

    /* 4. Band attenuation (notch at 4000 Hz, 10 dB suppression) */
    {
        TEST(impairment_band_attenuation);
        memset(&imp, 0, sizeof(imp));
        imp.band_atten_f_low_hz = 3800.0;
        imp.band_atten_f_high_hz = 4200.0;
        imp.band_atten_factor = 0.316; /* -10 dB */

        exp001_apply_impairments(&imp, g_pcm_a, frame_samples, g_pcm_b, PCM_BUF_SIZE, &impaired_samples);
        st = exp001_frame_decode(&fconfig, g_pcm_b, impaired_samples, recovered, sizeof(recovered), &decode_res);
        if (st == EXP001_OK && decode_res.crc_valid != 0u) {
            PASS();
        } else {
            FAIL("band attenuation decode failed");
        }
    }

    /* 5. Combined mild multipath + SRO + noise */
    {
        TEST(impairment_combined_mild);
        memset(&imp, 0, sizeof(imp));
        imp.num_multipath_paths = 2u;
        imp.multipath_delays_ms[1] = 4.0;
        imp.multipath_gains[1] = 0.20;
        imp.sample_rate_offset_ppm = 25.0;
        imp.awgn_snr_db = 30.0;
        imp.rng_seed = 2026u;

        exp001_apply_impairments(&imp, g_pcm_a, frame_samples, g_pcm_b, PCM_BUF_SIZE, &impaired_samples);
        st = exp001_frame_decode(&fconfig, g_pcm_b, impaired_samples, recovered, sizeof(recovered), &decode_res);
        if (st == EXP001_OK && decode_res.crc_valid != 0u) {
            PASS();
        } else {
            FAIL("combined mild impairment decode failed");
        }
    }
}

/* ========== Test 10: Generate E3 Source WAV ========== */

static void generate_e3_source_wav(void)
{
    mcl_wire_tier0_t obj;
    uint8_t wire_buf[MCL_WIRE_TIER0_MAX_SIZE];
    size_t wire_written = 0u, preamble_samples = 0u;
    exp001_frame_config_t fconfig;
    size_t frame_samples;
    const char *path = "exp001_e3_source.wav";

    printf("\n=== E3 Source WAV Generation ===\n");

    memset(&obj, 0, sizeof(obj));
    obj.kind = MCL_WIRE_KIND_PRESENCE;
    obj.priority = 1u;
    obj.source_ref = 0x00000001u;
    obj.body.presence.machine_class = 1u;
    obj.body.presence.capability_digest = 0x000001u;
    obj.body.presence.ttl = 60u;

    mcl_wire_tier0_encode(&obj, wire_buf, sizeof(wire_buf), &wire_written);

    fconfig.preamble_type = EXP001_PREAMBLE_LFM_CHIRP;
    fconfig.preamble_duration_s = 0.20;
    fconfig.preamble_f_start_hz = 2000.0;
    fconfig.preamble_f_end_hz = 6000.0;
    fconfig.leading_silence_s = 0.10;
    fconfig.silence_duration_s = 0.50;
    fconfig.include_training = 1u;
    fconfig.detection_threshold = 0.50;

    frame_samples = exp001_frame_encode(&fconfig, wire_buf, wire_written,
                                        g_pcm_a, PCM_BUF_SIZE, &preamble_samples);

    exp001_wav_write(path, g_pcm_a, frame_samples,
                     EXP001_SAMPLE_RATE, EXP001_BITS_PER_SAMPLE, EXP001_CHANNELS);

    printf("  Source WAV written: %s\n", path);
    printf("  Sample rate: %u Hz\n", EXP001_SAMPLE_RATE);
    printf("  Bit depth: %u bits\n", EXP001_BITS_PER_SAMPLE);
    printf("  Samples: %zu (%.3f s)\n", frame_samples, (double)frame_samples / (double)EXP001_SAMPLE_RATE);
    printf("  Preamble: LFM chirp 2000-6000 Hz, %zu samples (exact)\n", preamble_samples);
    printf("  Training: 16 alternating bits (symbol timing acquisition)\n");
    printf("  CRC-16: 0x%04X\n", (unsigned)exp001_crc16(wire_buf, wire_written));
}

int main(void)
{
    printf("MCL-AP Experiment 001: Corrected Test Suite\n");
    printf("===========================================\n");
    printf("Status: LAB / EXPERIMENTAL — NOT AP-B0\n\n");

    printf("--- Unit Tests: Core Synchronization & Invariants ---\n");
    test_preamble_length_invariant();
    test_mseq_properties();
    test_energy_equalization();
    test_polarity_invariance();
    test_variable_leading_silence();
    test_sro_resampler_analytical();

    printf("\n--- SRO Receiver Timing Acquisition Tests (-500 to +500 ppm) ---\n");
    test_sro_receiver_timing_acquisition();

    printf("\n--- Full Clean-Channel Pipeline (All 6 Tier-0 Kinds) ---\n");
    test_full_clean_pipeline();

    printf("\n--- Expanded Deterministic Impairments ---\n");
    test_expanded_impairments();

    generate_e3_source_wav();

    printf("\n===========================================\n");
    printf("Results: %d / %d tests passed\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
