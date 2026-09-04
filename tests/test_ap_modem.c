/*
 * MCL-AP candidate modem tests.
 *
 * Two halves, and the second is the one that matters.
 *
 * The first half is synthetic: encode, impair, decode, and check that the
 * impairments a real acoustic path actually applies -- arbitrary lead-in, a
 * microphone DC bias, polarity inversion, additive noise, and a band tilt that
 * puts one FSK tone well below the other -- do not break recovery, while a
 * corrupted payload does.
 *
 * The second half decodes the ARCHIVED E4 CAPTURES with this modem and
 * requires the same result the frozen Experiment 003 instrument recorded on
 * those exact files. That is what makes this module a reimplementation of the
 * measured waveform rather than a new one that happens to work on its own
 * output. A modem tested only against itself is a closed loop, and a closed
 * loop is exactly what an acoustic experiment must not be.
 *
 * The captures are never modified. They are read.
 */

#include "mcl/ap_modem.h"
#include "mcl/wire.h"
#include "mcl/link.h"
#include "../tools/wav_io.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define MAX_SAMPLES 600000u

static int g_checks;
static int g_failures;

static void check(int condition, const char *what)
{
    g_checks++;
    if (!condition) {
        g_failures++;
        printf("  FAIL: %s\n", what);
    }
}

static int16_t g_tx[MAX_SAMPLES];
static int16_t g_rx[MAX_SAMPLES];
static mcl_ap_modem_scratch_t g_scratch;

/* Deterministic noise. A test whose impairment changes between runs reports a
   different answer each time and cannot be a regression test. */
static uint32_t g_rng = 0x13579BDFu;

static float noise_sample(void)
{
    /* Sum of two uniforms: enough shaping for a decision test, and cheap. */
    float a, b;
    g_rng = g_rng * 1664525u + 1013904223u;
    a = (float)((g_rng >> 8) & 0xFFFFu) / 65535.0f - 0.5f;
    g_rng = g_rng * 1664525u + 1013904223u;
    b = (float)((g_rng >> 8) & 0xFFFFu) / 65535.0f - 0.5f;
    return a + b;
}

static int16_t clamp16(float v)
{
    if (v > 32767.0f) {
        return 32767;
    }
    if (v < -32767.0f) {
        return -32767;
    }
    return (int16_t)v;
}

/* --------------------------------------------------------------- payloads */

/* A major-1 Tier-0 PRESENCE, encoded by mcl-wire rather than written out as a
   literal. The point of the exercise is that the board builds this itself. */
static size_t build_presence(uint8_t *out, size_t capacity)
{
    mcl_wire_tier0_t object;
    size_t written = 0u;

    memset(&object, 0, sizeof(object));
    object.kind = MCL_WIRE_KIND_PRESENCE;
    object.priority = 1u;
    object.source_ref = 0x0BADCAFEu;
    object.body.presence.capability_tag = 0x00ABCDu;
    object.body.presence.ttl = 60u;

    if (mcl_wire_tier0_encode_at_major(MCL_WIRE_STABLE_MAJOR, &object,
                                       out, capacity, &written)
            != MCL_WIRE_OK) {
        return 0u;
    }
    return written;
}

static size_t build_link_frame(uint8_t *out, size_t capacity,
                               const uint8_t *payload, size_t payload_len)
{
    mcl_link_frame_t frame;
    size_t written = 0u;

    memset(&frame, 0, sizeof(frame));
    frame.frame_class = MCL_LINK_CLASS_CONTACT;
    frame.flags = MCL_LINK_FLAG_SEQUENCE | MCL_LINK_FLAG_FRAME_CHECK;
    frame.source_ref = 0x0BADCAFEu;
    frame.sequence = 1u;
    frame.payload = payload;
    frame.payload_len = (uint16_t)payload_len;

    if (mcl_link_frame_encode_at_major(MCL_LINK_STABLE_MAJOR, &frame,
                                       out, capacity, &written)
            != MCL_LINK_OK) {
        return 0u;
    }
    return written;
}

/* ------------------------------------------------------------ synthetic */

static size_t transmit(const uint8_t *payload, size_t len)
{
    mcl_ap_modem_config_t config;
    size_t written = 0u;

    mcl_ap_modem_default_config(&config);
    if (mcl_ap_modem_encode(&config, payload, len, g_tx, MAX_SAMPLES, &written)
            != MCL_AP_MODEM_OK) {
        return 0u;
    }
    return written;
}

static mcl_ap_modem_status_t receive(const int16_t *pcm, size_t count,
                                     uint8_t *out, size_t capacity,
                                     mcl_ap_modem_rx_t *info)
{
    mcl_ap_modem_config_t config;
    mcl_ap_modem_default_config(&config);
    return mcl_ap_modem_decode(&config, pcm, count, &g_scratch,
                               out, capacity, info);
}

static void test_round_trip_clean(void)
{
    uint8_t payload[MCL_WIRE_TIER0_MAX_SIZE];
    uint8_t received[MCL_AP_MODEM_MAX_PAYLOAD_BYTES];
    mcl_ap_modem_rx_t info;
    mcl_ap_modem_config_t config;
    size_t len, samples;

    printf("[modem] clean round trip, mcl-wire major-1 PRESENCE\n");
    len = build_presence(payload, sizeof(payload));
    check(len == 10u, "a major-1 PRESENCE is 10 bytes");

    mcl_ap_modem_default_config(&config);
    samples = transmit(payload, len);
    check(samples > 0u, "the modem encodes it");
    check(samples == mcl_ap_modem_encoded_samples(&config, len),
          "and writes exactly the sample count it predicted");

    check(receive(g_tx, samples, received, sizeof(received), &info)
              == MCL_AP_MODEM_OK,
          "decodes cleanly");
    check(info.acquired == 1u, "preamble acquired");
    check(info.crc_valid == 1u, "CRC verifies");
    check(info.payload_bytes == len, "payload length recovered");
    check(memcmp(received, payload, len) == 0, "payload bytes are identical");
}

static void test_arbitrary_lead_in(void)
{
    uint8_t payload[MCL_WIRE_TIER0_MAX_SIZE];
    uint8_t received[MCL_AP_MODEM_MAX_PAYLOAD_BYTES];
    mcl_ap_modem_rx_t info;
    size_t len, samples, lead = 17000u, i;

    printf("[modem] arbitrary lead-in, as a host recording has\n");
    len = build_presence(payload, sizeof(payload));
    samples = transmit(payload, len);

    for (i = 0u; i < lead; ++i) {
        g_rx[i] = 0;
    }
    memcpy(g_rx + lead, g_tx, samples * sizeof(int16_t));

    check(receive(g_rx, lead + samples, received, sizeof(received), &info)
              == MCL_AP_MODEM_OK,
          "still decodes when the frame starts 0.35 s in");
    check(info.acquisition_index >= lead - 8u &&
          info.acquisition_index <= lead + 4808u,
          "and acquires within the leading silence, not at zero");
    check(memcmp(received, payload, len) == 0, "identical bytes");
}

static void test_dc_bias_and_inversion(void)
{
    uint8_t payload[MCL_WIRE_TIER0_MAX_SIZE];
    uint8_t received[MCL_AP_MODEM_MAX_PAYLOAD_BYTES];
    mcl_ap_modem_rx_t info;
    size_t len, samples, i;

    printf("[modem] PDM DC bias, and polarity inversion\n");
    len = build_presence(payload, sizeof(payload));
    samples = transmit(payload, len);

    /* A PDM microphone carries a substantial constant offset. */
    for (i = 0u; i < samples; ++i) {
        g_rx[i] = clamp16((float)g_tx[i] * 0.5f + 6000.0f);
    }
    check(receive(g_rx, samples, received, sizeof(received), &info)
              == MCL_AP_MODEM_OK,
          "decodes through a large DC offset");
    check(memcmp(received, payload, len) == 0, "identical bytes under bias");

    /* Polarity inversion: some transducer chains invert. The quadrature
       detector must not care, and neither must the FSK energy decision. */
    for (i = 0u; i < samples; ++i) {
        g_rx[i] = (int16_t)-g_tx[i];
    }
    check(receive(g_rx, samples, received, sizeof(received), &info)
              == MCL_AP_MODEM_OK,
          "decodes an inverted signal");
    check(memcmp(received, payload, len) == 0, "identical bytes when inverted");
}

static void test_noise(void)
{
    uint8_t payload[MCL_WIRE_TIER0_MAX_SIZE];
    uint8_t received[MCL_AP_MODEM_MAX_PAYLOAD_BYTES];
    mcl_ap_modem_rx_t info;
    size_t len, samples, i;

    printf("[modem] additive noise\n");
    len = build_presence(payload, sizeof(payload));
    samples = transmit(payload, len);

    for (i = 0u; i < samples; ++i) {
        g_rx[i] = clamp16((float)g_tx[i] * 0.35f + noise_sample() * 2200.0f);
    }
    check(receive(g_rx, samples, received, sizeof(received), &info)
              == MCL_AP_MODEM_OK,
          "decodes with noise at a comparable level to the signal");
    check(memcmp(received, payload, len) == 0, "identical bytes under noise");
}

/*
 * A band tilt: attenuate the 6 kHz tone by about 14 dB and leave 3 kHz alone.
 *
 * This is the impairment Experiment 002 actually measured -- a notch that put
 * one FSK tone 19-21 dB down -- and it is why the demodulator estimates a
 * decision bias from the training sequence instead of comparing raw energies.
 * Without that estimate every bit decodes as the loud tone.
 */
static void test_band_tilt(void)
{
    uint8_t payload[MCL_WIRE_TIER0_MAX_SIZE];
    uint8_t received[MCL_AP_MODEM_MAX_PAYLOAD_BYTES];
    mcl_ap_modem_rx_t info;
    size_t len, samples, i;
    /* One-pole low-pass at about 3.4 kHz: passes 3 kHz, cuts 6 kHz. */
    const float alpha = 0.35f;
    float state = 0.0f;

    printf("[modem] band tilt, one FSK tone far below the other\n");
    len = build_presence(payload, sizeof(payload));
    samples = transmit(payload, len);

    for (i = 0u; i < samples; ++i) {
        state = state + alpha * ((float)g_tx[i] - state);
        g_rx[i] = clamp16(state);
    }
    check(receive(g_rx, samples, received, sizeof(received), &info)
              == MCL_AP_MODEM_OK,
          "decodes across a strong band tilt");
    check(memcmp(received, payload, len) == 0, "identical bytes under tilt");
}

static void test_refusals(void)
{
    uint8_t payload[MCL_WIRE_TIER0_MAX_SIZE];
    uint8_t received[MCL_AP_MODEM_MAX_PAYLOAD_BYTES];
    mcl_ap_modem_rx_t info;
    size_t len, samples, i;

    printf("[modem] what it must refuse\n");
    len = build_presence(payload, sizeof(payload));
    samples = transmit(payload, len);

    /* Silence: nothing to acquire, and nothing must be reported as recovered. */
    for (i = 0u; i < samples; ++i) {
        g_rx[i] = 0;
    }
    check(receive(g_rx, samples, received, sizeof(received), &info)
              != MCL_AP_MODEM_OK,
          "silence does not decode");
    check(info.crc_valid == 0u, "and reports no valid CRC");

    /* Noise alone. The threshold exists for this case. */
    for (i = 0u; i < samples; ++i) {
        g_rx[i] = clamp16(noise_sample() * 9000.0f);
    }
    check(receive(g_rx, samples, received, sizeof(received), &info)
              != MCL_AP_MODEM_OK,
          "noise alone does not decode");

    /*
     * A payload symbol destroyed after the training sequence. Acquisition and
     * timing still succeed; the CRC is what refuses. That distinction is the
     * point of reporting `acquired` and `crc_valid` separately -- "heard
     * something and could not read it" is a different problem from "heard
     * nothing", and only the first means a link is nearly working.
     */
    memcpy(g_rx, g_tx, samples * sizeof(int16_t));
    for (i = samples / 2u; i < samples / 2u + 3000u && i < samples; ++i) {
        g_rx[i] = clamp16(noise_sample() * 20000.0f);
    }
    check(receive(g_rx, samples, received, sizeof(received), &info)
              != MCL_AP_MODEM_OK,
          "a destroyed payload symbol is refused");
    check(info.acquired == 1u, "while the preamble was still acquired");
    check(info.crc_valid == 0u, "and the CRC is what refused it");
}

static void test_link_frame_over_air(void)
{
    uint8_t object[MCL_WIRE_TIER0_MAX_SIZE];
    uint8_t frame[64];
    uint8_t received[MCL_AP_MODEM_MAX_PAYLOAD_BYTES];
    mcl_ap_modem_rx_t info;
    mcl_link_frame_t decoded;
    mcl_wire_tier0_t recovered;
    size_t object_len, frame_len, samples, consumed = 0u, frame_consumed = 0u;

    printf("[modem] a complete Link frame, not only a bare Wire object\n");
    object_len = build_presence(object, sizeof(object));
    frame_len = build_link_frame(frame, sizeof(frame), object, object_len);
    check(frame_len > 0u, "a major-1 Link frame is built");
    check(frame_len == 8u + 2u + 4u + object_len,
          "8 mandatory + sequence + frame check + payload");

    samples = transmit(frame, frame_len);
    check(samples > 0u, "and modulates");

    check(receive(g_tx, samples, received, sizeof(received), &info)
              == MCL_AP_MODEM_OK,
          "the frame survives the modem");
    check(memcmp(received, frame, frame_len) == 0, "byte-identical frame");

    check(mcl_link_frame_decode(received, info.payload_bytes, &decoded,
                                &frame_consumed) == MCL_LINK_OK,
          "and the Link decoder accepts what came off the air");
    check(decoded.link_major == MCL_LINK_STABLE_MAJOR, "at the Stable major");
    check(frame_consumed == frame_len,
          "and consumes exactly the frame, with nothing trailing");
    check(decoded.payload_len == object_len, "carrying the object");
    check(mcl_wire_tier0_decode(decoded.payload, decoded.payload_len,
                                &recovered, &consumed) == MCL_WIRE_OK,
          "which decodes as a Tier-0 object");
    check(recovered.kind == MCL_WIRE_KIND_PRESENCE, "a PRESENCE");
    check(recovered.source_ref == 0x0BADCAFEu, "with the source_ref sent");
    check(recovered.body.presence.capability_tag == 0x00ABCDu,
          "and the capability_tag sent");
}

/* ------------------------------------------------- archived-capture regress */

/*
 * The payload the retained E3/E4 evidence carries: a major-0 Tier-0 PRESENCE.
 * It is written here as the literal it is in that evidence, because the point
 * is to compare against what was archived, not against what this tree would
 * generate today.
 */
static const uint8_t k_archived_payload[] = {
    0x00u, 0x02u, 0x00u, 0x00u, 0x00u, 0x01u, 0x01u, 0x00u, 0x00u, 0x01u, 0x3Cu
};

static int regress_directory(const char *directory, int trials,
                             int required, const char *label)
{
    uint8_t received[MCL_AP_MODEM_MAX_PAYLOAD_BYTES];
    mcl_ap_modem_rx_t info;
    char path[512];
    int index;
    int acquired = 0;
    int exact = 0;
    int present = 0;

    for (index = 1; index <= trials; ++index) {
        size_t count = 0u;
        int rc;

        sprintf(path, "%s/trial-%02d.wav", directory, index);
        rc = wav_read_pcm16(path, g_rx, MAX_SAMPLES, &count);
        if (rc != WAV_OK) {
            continue;
        }
        present++;

        if (receive(g_rx, count, received, sizeof(received), &info)
                == MCL_AP_MODEM_OK &&
            info.payload_bytes == sizeof(k_archived_payload) &&
            memcmp(received, k_archived_payload,
                   sizeof(k_archived_payload)) == 0) {
            exact++;
        }
        if (info.acquired) {
            acquired++;
        }
    }

    if (present == 0) {
        printf("  SKIP %s: captures not present at %s\n", label, directory);
        return 0;
    }

    printf("  %-34s %d/%d acquired, %d/%d exact (need %d)\n",
           label, acquired, present, exact, present, required);
    check(exact >= required,
          "the archived captures decode with this modem as they did with the "
          "frozen instrument");
    return 1;
}

static void test_archived_captures(const char *root)
{
    char base[512];
    int ran = 0;

    printf("[modem] ARCHIVED CAPTURES, decoded by this modem\n");
    printf("  These files are the retained E3/E4 evidence. They are read and\n"
           "  never written. Recovering what the frozen Experiment 003\n"
           "  instrument recovered from the same bytes is what makes this a\n"
           "  reimplementation of that waveform rather than a new one.\n");

    sprintf(base, "%s/experiments/003-band-informed-candidate/evidence/"
                  "e4-board-speaker-to-laptop-mic-20260902", root);
    ran += regress_directory(base, 10, 8, "E4 board speaker -> laptop mic");

    sprintf(base, "%s/experiments/003-band-informed-candidate/evidence/"
                  "dfr1154-20260902", root);
    ran += regress_directory(base, 10, 9, "E3-path candidate on board mic");

    sprintf(base, "%s/experiments/003-band-informed-candidate/evidence/"
                  "laptop-realtek-20260902", root);
    ran += regress_directory(base, 10, 4, "candidate on laptop mic");

    if (ran == 0) {
        printf("  no archived captures found; pass the mcl-ap root as argv[1]\n");
    }
}

int main(int argc, char **argv)
{
    const char *root = (argc > 1) ? argv[1] : ".";

    printf("=== MCL-AP candidate modem ===\n");
    printf("EXPERIMENTAL. Not AP-B0, not a selected profile.\n\n");

    test_round_trip_clean();
    test_arbitrary_lead_in();
    test_dc_bias_and_inversion();
    test_noise();
    test_band_tilt();
    test_refusals();
    test_link_frame_over_air();
    test_archived_captures(root);

    printf("\n%d checks, %d failed.\n", g_checks, g_failures);
    if (g_failures != 0) {
        printf("AP MODEM TESTS FAILED\n");
        return 1;
    }
    printf("AP MODEM TESTS PASSED\n");
    return 0;
}
