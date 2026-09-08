/*
 * MCL-AP Experiment 010b: how much symbol-rate error a frame of length N
 * survives, and what that predicts for the bootstrap objects nobody has
 * measured over air.
 *
 * WHY THIS IS NOT A SIMULATION OF THE CHANNEL
 *
 * `spec/ap-bootstrap-requirements-v0.1.md` §7 says synthetic impairment is a
 * supplement and never the selection evidence, because the failures that
 * mattered in this project were not the ones that get simulated: a notch in one
 * laptop speaker took a candidate from 9/10 to 0/10.
 *
 * So this invents no noise model and no SNR. It measures ONE deterministic
 * property -- the timing tolerance of a frame as a function of its length --
 * and combines it with the symbol-rate estimates the receiver ACTUALLY PRODUCED
 * on the retained captures. Experiment 010 found residual symbol-rate error
 * accumulating across the frame to be the dominant recoverable mechanism; this
 * quantifies the consequence for lengths that were never transmitted.
 *
 * The prediction it yields is a prediction. It tells the physical campaign what
 * to expect and what to instrument. It does not stand in for the campaign.
 *
 * Encoded objects are the REAL ONES, produced by mcl-wire at Stable major 1:
 *
 *   PRESENCE           10 bytes   measured over air, 0.192% BER, 9/10
 *   TRANSPORT_ACCEPT   16 bytes   never transmitted
 *   TRANSPORT_OFFER    17 bytes   never transmitted, and the worst case
 *
 * Usage: timing_tolerance
 */

#include "../../src/ap_modem.c"

#include "mcl/wire.h"

#include <stdio.h>
#include <string.h>

#define MAX_PCM 480000u

static int16_t g_pcm[MAX_PCM];

/*
 * The magnitudes of (estimated - true) samples-per-bit that the shipped
 * receiver produced on the twenty retained board-to-host captures. True is
 * exactly 48000/300 = 160.0, so these are estimator errors, not assumptions.
 * Recorded in evidence/error-structure-20260905/.
 */
static const float g_observed_sps_error[] = {
    0.000f, 0.000f, 0.000f, 0.050f, 0.050f, 0.100f, 0.100f, 0.100f,
    0.100f, 0.150f, 0.150f, 0.150f, 0.200f, 0.250f, 0.400f, 0.400f,
    0.450f, 0.450f, 0.450f, 0.500f
};
#define OBSERVED_N ((int)(sizeof(g_observed_sps_error) / sizeof(float)))

static size_t encode_presence(uint8_t *out, size_t cap)
{
    mcl_wire_tier0_t o;
    size_t n = 0u;
    memset(&o, 0, sizeof(o));
    o.kind = MCL_WIRE_KIND_PRESENCE;
    o.priority = 1u;
    o.source_ref = 0x0DFB1154u;
    o.body.presence.capability_tag = 4u;
    o.body.presence.ttl = 60u;
    if (mcl_wire_tier0_encode_at_major(MCL_WIRE_STABLE_MAJOR, &o, out, cap, &n)
        != MCL_WIRE_OK) {
        return 0u;
    }
    return n;
}

static size_t encode_accept(uint8_t *out, size_t cap)
{
    mcl_wire_tier0_t o;
    size_t n = 0u;
    memset(&o, 0, sizeof(o));
    o.kind = MCL_WIRE_KIND_TRANSPORT_ACCEPT;
    o.priority = 1u;
    o.source_ref = 0x0DFB1154u;
    o.body.transport_accept.migration_ref = 0x00A1B2C3u;
    o.body.transport_accept.transport_id = 3u;   /* MCL_BLE */
    o.body.transport_accept.profile_id = 1u;     /* BLE-GATT */
    o.body.transport_accept.session_ref = 0x11223344u;
    if (mcl_wire_tier0_encode_at_major(MCL_WIRE_STABLE_MAJOR, &o, out, cap, &n)
        != MCL_WIRE_OK) {
        return 0u;
    }
    return n;
}

static size_t encode_offer(uint8_t *out, size_t cap)
{
    mcl_wire_tier0_t o;
    size_t n = 0u;
    memset(&o, 0, sizeof(o));
    o.kind = MCL_WIRE_KIND_TRANSPORT_OFFER;
    o.priority = 1u;
    o.source_ref = 0x0DFB1154u;
    o.body.transport_offer.migration_ref = 0x00A1B2C3u;
    o.body.transport_offer.transport_id = 3u;
    o.body.transport_offer.profile_id = 1u;
    o.body.transport_offer.endpoint_token = 0x55667788u;
    o.body.transport_offer.validity = 60u;
    if (mcl_wire_tier0_encode_at_major(MCL_WIRE_STABLE_MAJOR, &o, out, cap, &n)
        != MCL_WIRE_OK) {
        return 0u;
    }
    return n;
}

/*
 * Decode a clean encoding of `payload` with the symbol rate deliberately
 * displaced by `sps_error`, and report whether the CRC still passes.
 *
 * Acquisition and the training-based phase estimate are left alone; only the
 * rate is perturbed, because the rate is the term that accumulates with bit
 * index and phase is not.
 */
static int survives(const uint8_t *payload, size_t len, float sps_error)
{
    static mcl_ap_modem_scratch_t scratch;
    static uint8_t out[MCL_AP_MODEM_MAX_PAYLOAD_BYTES];
    static uint8_t expected[MCL_AP_MODEM_HEADER_BYTES
                            + MCL_AP_MODEM_MAX_PAYLOAD_BYTES];
    mcl_ap_modem_config_t config;
    size_t written = 0u, ref_len, index = 0u, fsk_start, fsk_len, bit, exp_bits;
    const int16_t *fsk;
    float correlation = 0.0f, sps = 0.0f, bias = 0.0f, payload_start;
    int32_t phase = 0;
    uint16_t crc;

    mcl_ap_modem_default_config(&config);

    if (mcl_ap_modem_encode(&config, payload, len, g_pcm, MAX_PCM, &written)
        != MCL_AP_MODEM_OK) {
        return -1;
    }

    /* Through the cache: acquire() reads the reference statistics from the
       scratch, and those are filled in where the reference is built. */
    ref_len = preamble_iq_cached(&config, &scratch);
    acquire(&config, g_pcm, written, &scratch, scratch.ref_i, scratch.ref_q,
            ref_len, &index, &correlation);
    if (correlation < config.detection_threshold) {
        return -1;
    }
    fsk_start = index + ref_len;
    fsk = g_pcm + fsk_start;
    fsk_len = written - fsk_start;

    estimate_timing(fsk, fsk_len, (size_t)config.training_bits,
                    config.fsk_freq_0_hz, config.fsk_freq_1_hz,
                    &phase, &sps, &bias);

    /* The perturbation under test. */
    sps += sps_error;

    payload_start = (float)phase + (float)config.training_bits * sps;
    if (payload_start < 0.0f) {
        return 0;
    }

    crc = mcl_ap_modem_crc16(payload, len);
    expected[0] = (uint8_t)len;
    expected[1] = (uint8_t)(crc >> 8);
    expected[2] = (uint8_t)(crc & 0xFFu);
    memcpy(expected + 3, payload, len);
    exp_bits = (MCL_AP_MODEM_HEADER_BYTES + len) * 8u;

    memset(out, 0, sizeof(out));
    for (bit = 0u; bit < exp_bits; ++bit) {
        float t0 = payload_start + (float)bit * sps;
        size_t st = window_index(t0);
        size_t ln = window_index(sps);
        unsigned got, want;

        if (st + ln > fsk_len) {
            return 0;   /* ran off the end: a failure, not an error */
        }
        got = (log_ratio(fsk, st, ln, config.fsk_freq_0_hz,
                         config.fsk_freq_1_hz) - bias > 0.0f) ? 1u : 0u;
        want = (unsigned)((expected[bit / 8u] >> (7u - (bit % 8u))) & 1u);
        if (got != want) {
            return 0;
        }
    }
    return 1;
}

static float tolerance(const uint8_t *payload, size_t len)
{
    float e;
    float last_ok = 0.0f;
    for (e = 0.0f; e <= 1.0f; e += 0.005f) {
        if (survives(payload, len, e) == 1 && survives(payload, len, -e) == 1) {
            last_ok = e;
        } else {
            break;
        }
    }
    return last_ok;
}

int main(void)
{
    static uint8_t buf[MCL_WIRE_TIER0_MAX_SIZE];
    struct { const char *name; size_t len; uint8_t bytes[MCL_WIRE_TIER0_MAX_SIZE]; }
        cases[3];
    size_t n;
    int i, j;

    printf("=== Experiment 010b: timing tolerance versus frame length ===\n\n");
    printf("Real major-1 objects from mcl-wire. Only the symbol rate is\n");
    printf("perturbed; no noise model is invented. See the file header.\n\n");

    n = encode_presence(buf, sizeof(buf));
    cases[0].name = "PRESENCE"; cases[0].len = n;
    memcpy(cases[0].bytes, buf, n);

    n = encode_accept(buf, sizeof(buf));
    cases[1].name = "TRANSPORT_ACCEPT"; cases[1].len = n;
    memcpy(cases[1].bytes, buf, n);

    n = encode_offer(buf, sizeof(buf));
    cases[2].name = "TRANSPORT_OFFER"; cases[2].len = n;
    memcpy(cases[2].bytes, buf, n);

    printf("%-18s %6s %6s %10s %11s %10s %9s\n",
           "object", "bytes", "bits", "|sps| tol", "drift@fail", "of symbol",
           "clean/20");
    printf("%-18s %6s %6s %10s %11s %10s %9s\n",
           "------", "-----", "----", "---------", "----------", "---------",
           "--------");

    for (i = 0; i < 3; ++i) {
        float tol = tolerance(cases[i].bytes, cases[i].len);
        unsigned bits = (unsigned)((MCL_AP_MODEM_HEADER_BYTES + cases[i].len) * 8u);
        double drift = (double)tol * (double)bits;
        int clean = 0;
        for (j = 0; j < OBSERVED_N; ++j) {
            if (g_observed_sps_error[j] <= tol) {
                clean++;
            }
        }
        printf("%-18s %6u %6u %10.3f %11.1f %10.3f %6d/%d\n",
               cases[i].name, (unsigned)cases[i].len, bits,
               (double)tol, drift, drift / (double)samples_per_bit(),
               clean, OBSERVED_N);
    }

    printf("\nDRIFT@FAIL is tolerance x bits: the accumulated timing error, in\n");
    printf("samples, when the frame stops decoding. It is near-constant across\n");
    printf("lengths, which states the mechanism as a law -- a frame fails when\n");
    printf("accumulated drift reaches about 0.37 of a symbol, so usable length\n");
    printf("is roughly 60 / |sps error| bits.\n");

    printf("\nThe last column applies the tolerance to the symbol-rate errors\n");
    printf("the shipped receiver actually produced on the retained captures.\n");
    printf("It is a PREDICTION for the physical campaign, not a measurement of\n");
    printf("one: it holds only the timing term constant-perfect otherwise, and\n");
    printf("Experiment 010 found roughly half the errors at 24 bytes were NOT\n");
    printf("timing-removable. Expect the campaign to do worse than this line.\n");
    return 0;
}
