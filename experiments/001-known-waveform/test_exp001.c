/*
 * MCL-AP Experiment 001: Test Driver
 *
 * Tests the experimental acoustic modem using real MCL Wire-encoded bytes.
 * NOT a normative test. LAB / EXPERIMENTAL only.
 *
 * NOTE: Large PCM buffers are static to avoid stack overflow on Windows
 * default 1 MB stack. Tests are run sequentially so buffer reuse is safe.
 */

#include "exp001.h"
#include "mcl/wire.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

static int tests_run = 0;
static int tests_passed = 0;

/* Shared static PCM buffer — ~3.6 MB */
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

/* ========== CRC-16 test ========== */

static void test_crc16(void)
{
    const uint8_t data[] = { 0x01, 0x02, 0x03 };
    uint16_t crc;

    TEST(crc16_basic);
    crc = exp001_crc16(data, sizeof(data));
    if (crc == 0u) FAIL("CRC is zero");
    if (crc != exp001_crc16(data, sizeof(data))) FAIL("CRC not deterministic");
    PASS();
}

/* ========== FSK round-trip test ========== */

static void test_fsk_roundtrip(void)
{
    const uint8_t payload[] = { 0xDE, 0xAD, 0xBE, 0xEF, 0x42 };
    uint8_t recovered[5];
    size_t mod_samples, demod_bytes;

    TEST(fsk_roundtrip);

    mod_samples = exp001_fsk_modulate(payload, sizeof(payload), g_pcm_a, PCM_BUF_SIZE);
    if (mod_samples == 0u) FAIL("modulate returned 0");

    demod_bytes = exp001_fsk_demodulate(g_pcm_a, mod_samples, recovered, sizeof(recovered));
    if (demod_bytes != sizeof(payload)) FAIL("demod byte count mismatch");

    if (memcmp(payload, recovered, sizeof(payload)) != 0) FAIL("FSK round-trip data mismatch");
    PASS();
}

/* ========== Wire -> FSK -> Wire round-trip for each Tier-0 kind ========== */

static void test_wire_acoustic_roundtrip(mcl_wire_kind_t kind, const char *name)
{
    mcl_wire_tier0_t src_obj, dst_obj;
    uint8_t wire_buf[MCL_WIRE_TIER0_MAX_SIZE];
    size_t wire_written = 0u;
    size_t wire_consumed = 0u;
    mcl_wire_status_t wst;
    uint8_t recovered_wire[MCL_WIRE_TIER0_MAX_SIZE];
    exp001_frame_config_t fconfig;
    size_t frame_samples;
    exp001_decode_result_t decode_result;
    exp001_status_t est;
    char msg_buf[128];

    TEST(wire_acoustic_roundtrip);
    printf("(%s) ... ", name);

    memset(&src_obj, 0, sizeof(src_obj));
    src_obj.kind = kind;
    src_obj.priority = 1u;
    src_obj.source_ref = 0xDEAD0001u;

    switch (kind) {
    case MCL_WIRE_KIND_PRESENCE:
        src_obj.body.presence.machine_class = 5u;
        src_obj.body.presence.capability_digest = 0x00ABCDu;
        src_obj.body.presence.ttl = 60u;
        break;
    case MCL_WIRE_KIND_HAZARD:
        src_obj.body.hazard.hazard_class = 2u;
        src_obj.body.hazard.severity = 5u;
        src_obj.body.hazard.confidence = 90u;
        src_obj.body.hazard.x = 100;
        src_obj.body.hazard.y = -200;
        src_obj.body.hazard.z = 50;
        src_obj.body.hazard.radius = 500u;
        src_obj.body.hazard.ttl = 30u;
        break;
    case MCL_WIRE_KIND_REQUEST:
        src_obj.body.request.request_class = 1u;
        src_obj.body.request.target_ref = 0x12345678u;
        src_obj.body.request.x = -50;
        src_obj.body.request.y = 75;
        src_obj.body.request.radius = 250u;
        src_obj.body.request.ttl = 120u;
        break;
    case MCL_WIRE_KIND_AUTHORITY_CLAIM:
        src_obj.body.authority_claim.authority_class = 7u;
        src_obj.body.authority_claim.jurisdiction = 1024u;
        src_obj.body.authority_claim.credential_ref = 0xFEDCBA98u;
        src_obj.body.authority_claim.validity = 255u;
        break;
    case MCL_WIRE_KIND_DEGRADED_STATE:
        src_obj.body.degraded_state.affected_capability = 3u;
        src_obj.body.degraded_state.health = 85u;
        src_obj.body.degraded_state.severity = 2u;
        src_obj.body.degraded_state.ttl = 45u;
        break;
    case MCL_WIRE_KIND_TRANSPORT_OFFER:
        src_obj.body.transport_offer.transport_id = 1u;
        src_obj.body.transport_offer.profile_id = 0u;
        src_obj.body.transport_offer.endpoint_token = 0x87654321u;
        src_obj.body.transport_offer.validity = 180u;
        break;
    default:
        FAIL("unknown kind");
        return;
    }

    wst = mcl_wire_tier0_encode(&src_obj, wire_buf, sizeof(wire_buf), &wire_written);
    if (wst != MCL_WIRE_OK) {
        sprintf(msg_buf, "wire encode failed: %d", (int)wst);
        FAIL(msg_buf);
    }

    fconfig.preamble_type = EXP001_PREAMBLE_LFM_CHIRP;
    fconfig.preamble_duration_s = 0.1;
    fconfig.preamble_f_start_hz = 2000.0;
    fconfig.preamble_f_end_hz = 6000.0;
    fconfig.silence_duration_s = 0.05;

    frame_samples = exp001_frame_encode(
        &fconfig, wire_buf, wire_written,
        g_pcm_a, PCM_BUF_SIZE);

    if (frame_samples == 0u) FAIL("frame encode returned 0");

    est = exp001_frame_decode(
        &fconfig, g_pcm_a, frame_samples,
        recovered_wire, sizeof(recovered_wire),
        &decode_result);

    if (est != EXP001_OK) {
        sprintf(msg_buf, "frame decode failed: %d", (int)est);
        FAIL(msg_buf);
    }

    if (!decode_result.crc_valid) FAIL("CRC mismatch");
    if (decode_result.payload_bytes != wire_written) FAIL("payload size mismatch");
    if (memcmp(wire_buf, recovered_wire, wire_written) != 0) FAIL("wire bytes mismatch");

    wst = mcl_wire_tier0_decode(recovered_wire, decode_result.payload_bytes, &dst_obj, &wire_consumed);
    if (wst != MCL_WIRE_OK) {
        sprintf(msg_buf, "wire decode of recovered bytes failed: %d", (int)wst);
        FAIL(msg_buf);
    }

    if (dst_obj.kind != src_obj.kind) FAIL("decoded kind mismatch");
    if (dst_obj.source_ref != src_obj.source_ref) FAIL("decoded source_ref mismatch");

    PASS();
}

/* ========== Preamble generation test ========== */

static void test_preamble_generation(void)
{
    size_t n;
    unsigned type;

    for (type = 0u; type < EXP001_PREAMBLE_TYPE_COUNT; ++type) {
        const char *names[] = { "LFM_CHIRP", "ZADOFF_CHU", "PN_MSEQ", "FREQ_DIVERSE" };
        TEST(preamble_generation);
        printf("(%s) ... ", names[type]);

        n = exp001_generate_preamble(
            (exp001_preamble_type_t)type,
            0.1,
            2000.0,
            6000.0,
            g_pcm_a,
            48000u);  /* 1 second max */

        if (n == 0u) FAIL("returned 0 samples");

        {
            size_t i;
            for (i = 0u; i < n; ++i) {
                if (g_pcm_a[i] < -1.01f || g_pcm_a[i] > 1.01f) FAIL("sample out of range");
            }
        }

        PASS();
    }
}

/* ========== WAV write/read round-trip ========== */

static void test_wav_roundtrip(void)
{
    size_t i, n_read;
    uint32_t sr;
    uint16_t bps, ch;
    exp001_status_t est;
    const char *path = "exp001_test.wav";

    TEST(wav_roundtrip);

    for (i = 0u; i < 4800u; ++i) {
        g_pcm_a[i] = (float)sin(2.0 * 3.14159265 * 1000.0 * (double)i / 48000.0);
    }

    est = exp001_wav_write(path, g_pcm_a, 4800u, 48000u, 16u, 1u);
    if (est != EXP001_OK) FAIL("wav write failed");

    est = exp001_wav_read(path, g_pcm_b, 4800u, &n_read, &sr, &bps, &ch);
    if (est != EXP001_OK) FAIL("wav read failed");
    if (n_read != 4800u) FAIL("sample count mismatch");
    if (sr != 48000u) FAIL("sample rate mismatch");
    if (bps != 16u) FAIL("bits per sample mismatch");
    if (ch != 1u) FAIL("channel count mismatch");

    for (i = 0u; i < 4800u; ++i) {
        if (fabs((double)(g_pcm_a[i] - g_pcm_b[i])) > 0.001) FAIL("sample data mismatch");
    }

    remove(path);
    PASS();
}

/* ========== Impairment: clean channel survival ========== */

static void test_impairment_clean(void)
{
    size_t i;
    exp001_impairment_config_t imp;

    TEST(impairment_clean_channel);

    for (i = 0u; i < 480u; ++i) {
        g_pcm_a[i] = (float)sin(2.0 * 3.14159265 * 3000.0 * (double)i / 48000.0);
        g_pcm_b[i] = g_pcm_a[i];
    }

    memset(&imp, 0, sizeof(imp));
    imp.rng_seed = 42u;
    imp.clipping_threshold = 1.0;

    exp001_apply_impairments(&imp, g_pcm_a, 480u);

    for (i = 0u; i < 480u; ++i) {
        if (g_pcm_a[i] != g_pcm_b[i]) FAIL("clean impairment altered signal");
    }
    PASS();
}

/* ========== Full pipeline test with all six Wire kinds ========== */

static void test_full_pipeline(void)
{
    test_wire_acoustic_roundtrip(MCL_WIRE_KIND_PRESENCE, "PRESENCE");
    test_wire_acoustic_roundtrip(MCL_WIRE_KIND_HAZARD, "HAZARD");
    test_wire_acoustic_roundtrip(MCL_WIRE_KIND_REQUEST, "REQUEST");
    test_wire_acoustic_roundtrip(MCL_WIRE_KIND_AUTHORITY_CLAIM, "AUTHORITY_CLAIM");
    test_wire_acoustic_roundtrip(MCL_WIRE_KIND_DEGRADED_STATE, "DEGRADED_STATE");
    test_wire_acoustic_roundtrip(MCL_WIRE_KIND_TRANSPORT_OFFER, "TRANSPORT_OFFER");
}

/* ========== Impaired channel tests ========== */

static void test_impaired_channel(void)
{
    mcl_wire_tier0_t src_obj, dst_obj;
    uint8_t wire_buf[MCL_WIRE_TIER0_MAX_SIZE];
    size_t wire_written = 0u;
    size_t consumed;
    uint8_t recovered[MCL_WIRE_TIER0_MAX_SIZE];
    exp001_frame_config_t fconfig;
    exp001_decode_result_t decode_result;
    exp001_impairment_config_t imp;
    size_t frame_samples;
    exp001_status_t est;
    mcl_wire_status_t wst;

    /* Use a PRESENCE frame for impairment testing */
    memset(&src_obj, 0, sizeof(src_obj));
    src_obj.kind = MCL_WIRE_KIND_PRESENCE;
    src_obj.priority = 0u;
    src_obj.source_ref = 0x42424242u;
    src_obj.body.presence.machine_class = 1u;
    src_obj.body.presence.capability_digest = 0x000FFFu;
    src_obj.body.presence.ttl = 30u;

    wst = mcl_wire_tier0_encode(&src_obj, wire_buf, sizeof(wire_buf), &wire_written);
    if (wst != MCL_WIRE_OK) {
        printf("  [SKIP] wire encode failed for impairment test\n");
        return;
    }

    fconfig.preamble_type = EXP001_PREAMBLE_LFM_CHIRP;
    fconfig.preamble_duration_s = 0.1;
    fconfig.preamble_f_start_hz = 2000.0;
    fconfig.preamble_f_end_hz = 6000.0;
    fconfig.silence_duration_s = 0.05;

    frame_samples = exp001_frame_encode(
        &fconfig, wire_buf, wire_written,
        g_pcm_a, PCM_BUF_SIZE);
    if (frame_samples == 0u) {
        printf("  [SKIP] frame encode returned 0\n");
        return;
    }

    /* Test 1: mild AWGN (40 dB SNR — should survive) */
    {
        memcpy(g_pcm_b, g_pcm_a, frame_samples * sizeof(float));

        memset(&imp, 0, sizeof(imp));
        imp.awgn_snr_db = 40.0;
        imp.clipping_threshold = 1.0;
        imp.rng_seed = 12345u;

        exp001_apply_impairments(&imp, g_pcm_b, frame_samples);

        TEST(impaired_awgn_40db);
        est = exp001_frame_decode(&fconfig, g_pcm_b, frame_samples,
                                  recovered, sizeof(recovered), &decode_result);
        if (est == EXP001_OK && decode_result.crc_valid &&
            memcmp(wire_buf, recovered, wire_written) == 0) {
            wst = mcl_wire_tier0_decode(recovered, decode_result.payload_bytes, &dst_obj, &consumed);
            if (wst == MCL_WIRE_OK && dst_obj.kind == src_obj.kind) {
                PASS();
            } else {
                FAIL("wire decode failed on recovered impaired bytes");
            }
        } else {
            printf("CONDITIONAL_FAIL (SNR=40dB): status=%d, crc_valid=%u\n",
                   (int)est, (unsigned)decode_result.crc_valid);
        }
    }

    /* Test 2: moderate clipping (0.8 threshold) */
    {
        memcpy(g_pcm_b, g_pcm_a, frame_samples * sizeof(float));

        memset(&imp, 0, sizeof(imp));
        imp.clipping_threshold = 0.8;
        imp.rng_seed = 1u;

        exp001_apply_impairments(&imp, g_pcm_b, frame_samples);

        TEST(impaired_clipping_0_8);
        est = exp001_frame_decode(&fconfig, g_pcm_b, frame_samples,
                                  recovered, sizeof(recovered), &decode_result);
        if (est == EXP001_OK && decode_result.crc_valid &&
            memcmp(wire_buf, recovered, wire_written) == 0) {
            PASS();
        } else {
            printf("CONDITIONAL_FAIL (clip=0.8): status=%d, crc_valid=%u\n",
                   (int)est, (unsigned)decode_result.crc_valid);
        }
    }

    /* Test 3: small sample rate offset (+50 ppm) */
    {
        memcpy(g_pcm_b, g_pcm_a, frame_samples * sizeof(float));

        memset(&imp, 0, sizeof(imp));
        imp.sample_rate_offset_ppm = 50.0;
        imp.clipping_threshold = 1.0;
        imp.rng_seed = 99u;

        exp001_apply_impairments(&imp, g_pcm_b, frame_samples);

        TEST(impaired_sro_50ppm);
        est = exp001_frame_decode(&fconfig, g_pcm_b, frame_samples,
                                  recovered, sizeof(recovered), &decode_result);
        if (est == EXP001_OK && decode_result.crc_valid &&
            memcmp(wire_buf, recovered, wire_written) == 0) {
            PASS();
        } else {
            printf("CONDITIONAL_FAIL (SRO=50ppm): status=%d, crc_valid=%u\n",
                   (int)est, (unsigned)decode_result.crc_valid);
        }
    }
}

/* ========== E3 WAV generator ========== */

static void generate_e3_source_wav(void)
{
    mcl_wire_tier0_t obj;
    uint8_t wire_buf[MCL_WIRE_TIER0_MAX_SIZE];
    size_t wire_written = 0u;
    exp001_frame_config_t fconfig;
    size_t frame_samples;
    mcl_wire_status_t wst;
    exp001_status_t est;
    const char *path = "exp001_e3_source.wav";

    printf("\n=== E3 Source WAV Generation ===\n");

    memset(&obj, 0, sizeof(obj));
    obj.kind = MCL_WIRE_KIND_PRESENCE;
    obj.priority = 1u;
    obj.source_ref = 0x00000001u;
    obj.body.presence.machine_class = 1u;
    obj.body.presence.capability_digest = 0x000001u;
    obj.body.presence.ttl = 60u;

    wst = mcl_wire_tier0_encode(&obj, wire_buf, sizeof(wire_buf), &wire_written);
    if (wst != MCL_WIRE_OK) {
        printf("  Wire encode failed: %d\n", (int)wst);
        return;
    }

    printf("  Wire bytes (%zu): ", wire_written);
    {
        size_t i;
        for (i = 0u; i < wire_written; ++i) {
            printf("%02X ", wire_buf[i]);
        }
        printf("\n");
    }

    fconfig.preamble_type = EXP001_PREAMBLE_LFM_CHIRP;
    fconfig.preamble_duration_s = 0.2;
    fconfig.preamble_f_start_hz = 2000.0;
    fconfig.preamble_f_end_hz = 6000.0;
    fconfig.silence_duration_s = 0.5;

    frame_samples = exp001_frame_encode(
        &fconfig, wire_buf, wire_written,
        g_pcm_a, PCM_BUF_SIZE);

    if (frame_samples == 0u) {
        printf("  Frame encode failed\n");
        return;
    }

    est = exp001_wav_write(path, g_pcm_a, frame_samples,
                           EXP001_SAMPLE_RATE, EXP001_BITS_PER_SAMPLE, EXP001_CHANNELS);

    if (est != EXP001_OK) {
        printf("  WAV write failed: %d\n", (int)est);
        return;
    }

    printf("  Source WAV: %s\n", path);
    printf("  Sample rate: %u Hz\n", EXP001_SAMPLE_RATE);
    printf("  Bit depth: %u\n", EXP001_BITS_PER_SAMPLE);
    printf("  Samples: %zu\n", frame_samples);
    printf("  Duration: %.3f s\n", (double)frame_samples / (double)EXP001_SAMPLE_RATE);
    printf("  Preamble: LFM chirp 2000-6000 Hz, 200 ms\n");
    printf("  Modulation: Binary FSK, f0=%g Hz, f1=%g Hz, %u baud\n",
           EXP001_FSK_FREQ_0, EXP001_FSK_FREQ_1, EXP001_FSK_BAUD);
    printf("  CRC-16: 0x%04X\n", (unsigned)exp001_crc16(wire_buf, wire_written));
    printf("\n  To complete E3:\n");
    printf("  1. Play this WAV through a speaker\n");
    printf("  2. Record microphone capture as 48 kHz 16-bit mono WAV\n");
    printf("  3. Run the offline decoder on the captured WAV\n");
    printf("  4. Verify bit-perfect MCL Wire bytes recovered\n");
}

/* ========== Main ========== */

int main(void)
{
    printf("MCL-AP Experiment 001: Known-Waveform Acoustic Modem\n");
    printf("====================================================\n");
    printf("Status: LAB / EXPERIMENTAL -- NOT a normative AP profile\n\n");

    printf("--- Unit Tests ---\n");
    test_crc16();
    test_fsk_roundtrip();
    test_preamble_generation();
    test_wav_roundtrip();
    test_impairment_clean();

    printf("\n--- Full Pipeline: Wire -> Acoustic -> Wire (Clean Channel) ---\n");
    test_full_pipeline();

    printf("\n--- Impaired Channel Tests ---\n");
    test_impaired_channel();

    generate_e3_source_wav();

    printf("\n====================================================\n");
    printf("Results: %d / %d tests passed\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
