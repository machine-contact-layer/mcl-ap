/*
 * MCL-AP Experiment 011: the bootstrap objects over air, measured.
 *
 * WHAT 010 LEFT UNMEASURED
 *
 * Experiment 010b derived a timing law from the retained corpus and used it to
 * PREDICT that a 17-byte TRANSPORT_OFFER decodes cleanly in 14 of 20 attempts
 * from the timing term alone. 010c then showed, on those same retained
 * captures, that a blind whole-frame rate refinement recovers most of the loss.
 *
 * Neither result is an over-air measurement of the bootstrap objects. The
 * corpus contains 10-, 11- and 24-byte payloads and nothing at 16 or 17, and
 * 010's own README records that degradation with length is superlinear, so
 * interpolating into the gap is exactly the move that would make the number
 * wrong. This experiment transmits the actual objects.
 *
 * THE THREE CELLS ARE THE THREE OBJECTS
 *
 *   PRESENCE           10 bytes    what a stranger emits
 *   TRANSPORT_ACCEPT   16 bytes    the reply
 *   TRANSPORT_OFFER    17 bytes    the largest object in the sequence
 *
 * They are encoded here by mcl-wire at MCL_WIRE_STABLE_MAJOR rather than
 * hand-assembled, so the bytes on the air are the release's bytes.
 *
 * EVERY CAPTURE IS DECODED TWICE
 *
 * Once by mcl_ap_modem_decode -- the shipped receiver, exactly as a builder
 * gets it -- and once by the blind refinement 010c proposed. Reporting both
 * from one capture is what makes the remedy a measurement rather than a
 * before/after across two different rooms. The refinement is blind: it scores
 * only the signal, never the known payload, so it is something a receiver can
 * actually run.
 *
 * It includes src/ap_modem.c for the same reason 010 does -- the per-symbol
 * soft value is internal -- and therefore must not also link mcl_ap.
 *
 * Usage:
 *   bootstrap_air sizes
 *   bootstrap_air hex     <presence|accept|offer>
 *   bootstrap_air gen     <presence|accept|offer> <out.wav>
 *   bootstrap_air decode  <presence|accept|offer> <in.wav>
 *
 * Exit codes for decode, so a trial script can count without parsing:
 *   0 both decoders recovered the object
 *   1 only the refined decoder recovered it
 *   2 usage or I/O
 *   3 acquired, neither decoder recovered it
 *   4 not acquired
 */

#include "../../src/ap_modem.c"
#include "../../tools/wav_io.h"

#include "mcl/wire.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_SAMPLES 600000u
#define NOMINAL_SPS 160.0f

static int16_t g_pcm[MAX_SAMPLES];

/*
 * THE BAND IS A VARIABLE HERE, AND IT HAS TO BE
 *
 * mcl_ap_modem_default_config puts the two FSK tones at 3000 and 6000 Hz. That
 * pair was measured on the DFR1154's speaker, and the AP band registry already
 * records that it must not be standardised on the strength of one campaign.
 *
 * Experiment 002 then measured a deep notch around 3 kHz in THIS LAPTOP's
 * speaker -- a transmitter-side notch, not a room. A bootstrap profile whose
 * lower tone lands in a common laptop speaker's notch is not a bootstrap
 * profile, so AP-BOOTSTRAP-1 cannot inherit the default pair by default. The
 * band therefore has to be a cell of this experiment rather than a constant in
 * it, and the preamble chirp moves with it: a chirp that sweeps 2-6 kHz while
 * the payload sits at 6-9 kHz would acquire on energy the payload never uses.
 *
 * Zero means "leave the default alone".
 */
static float g_f0_hz;
static float g_f1_hz;

/*
 * One correlation reference per role. These are not secrets and are not
 * identity -- wire.h says so at both objects -- but distinct values make a
 * recovered object attributable to the peer that emitted it while reading a
 * run log.
 */
#define PEER_SOURCE_REF   0x0A11EDC0u
#define MIGRATION_REF     0x00C0FFEEu
#define ENDPOINT_TOKEN    0x5E1EC7EDu
#define SESSION_REF       0x0D0C0DE5u

typedef enum { KIND_PRESENCE, KIND_ACCEPT, KIND_OFFER } cell_t;

static int parse_cell(const char *name, cell_t *out)
{
    if (strcmp(name, "presence") == 0) { *out = KIND_PRESENCE; return 0; }
    if (strcmp(name, "accept") == 0)   { *out = KIND_ACCEPT;   return 0; }
    if (strcmp(name, "offer") == 0)    { *out = KIND_OFFER;    return 0; }
    return -1;
}

static const char *cell_name(cell_t c)
{
    switch (c) {
    case KIND_PRESENCE: return "PRESENCE";
    case KIND_ACCEPT:   return "TRANSPORT_ACCEPT";
    default:            return "TRANSPORT_OFFER";
    }
}

/* Build one bootstrap object at the Stable major. Returns encoded length. */
static size_t build_object(cell_t cell, uint8_t *out, size_t capacity)
{
    mcl_wire_tier0_t object;
    size_t written = 0u;

    memset(&object, 0, sizeof(object));
    object.priority = 1u;
    object.source_ref = PEER_SOURCE_REF;

    switch (cell) {
    case KIND_PRESENCE:
        object.kind = MCL_WIRE_KIND_PRESENCE;
        object.body.presence.capability_tag = 0x000004u;   /* acoustic */
        object.body.presence.ttl = 60u;
        break;
    case KIND_ACCEPT:
        object.kind = MCL_WIRE_KIND_TRANSPORT_ACCEPT;
        object.body.transport_accept.migration_ref = MIGRATION_REF;
        object.body.transport_accept.transport_id = 3u;    /* BLE */
        object.body.transport_accept.profile_id = 1u;      /* BLE-GATT = 1 */
        object.body.transport_accept.session_ref = SESSION_REF;
        break;
    case KIND_OFFER:
    default:
        object.kind = MCL_WIRE_KIND_TRANSPORT_OFFER;
        object.body.transport_offer.migration_ref = MIGRATION_REF;
        object.body.transport_offer.transport_id = 3u;
        object.body.transport_offer.profile_id = 1u;
        object.body.transport_offer.endpoint_token = ENDPOINT_TOKEN;
        object.body.transport_offer.validity = 60u;
        break;
    }

    if (mcl_wire_tier0_encode_at_major(MCL_WIRE_STABLE_MAJOR, &object,
                                       out, capacity, &written) != MCL_WIRE_OK) {
        return 0u;
    }
    return written;
}

static void print_hex(const uint8_t *bytes, size_t count)
{
    size_t i;
    for (i = 0u; i < count; ++i) printf("%02X", bytes[i]);
}

static void configure(mcl_ap_modem_config_t *config, int for_transmit)
{
    mcl_ap_modem_default_config(config);
    /* Matches the board, so a difference between rigs is the path and not the
       waveform. Experiment 008 established this trim. */
    if (for_transmit) config->trailing_silence_s = 0.10f;

    if (g_f0_hz > 0.0f && g_f1_hz > 0.0f) {
        config->fsk_freq_0_hz = g_f0_hz;
        config->fsk_freq_1_hz = g_f1_hz;
        /* The chirp brackets the pair with the same margin the default keeps
           below its own lower tone (2 kHz under 3 kHz), so acquisition and
           payload occupy the same region of the response. */
        config->preamble_f_start_hz = g_f0_hz - 1000.0f;
        config->preamble_f_end_hz = g_f1_hz;
        if (config->preamble_f_start_hz < 500.0f) {
            config->preamble_f_start_hz = 500.0f;
        }
    }
}

/* Parse --band <f0>:<f1>. Returns 0 on success. */
static int parse_band(const char *arg)
{
    char *end = NULL;
    double f0 = strtod(arg, &end);
    double f1;

    if (end == NULL || *end != ':') return -1;
    f1 = strtod(end + 1, &end);
    if (end == NULL || *end != '\0') return -1;
    /* The modem samples at 48 kHz. A tone near Nyquist is not a band, it is an
       aliasing experiment nobody asked for. */
    if (f0 < 500.0 || f1 <= f0 || f1 > 20000.0) return -1;
    g_f0_hz = (float)f0;
    g_f1_hz = (float)f1;
    return 0;
}

/* ---------------------------------------------------------------- refined RX
 *
 * 010c estimator, unchanged in substance: score a candidate symbol rate by the
 * mean decision margin over the WHOLE frame instead of over the 16 training
 * bits. The phase is recomputed per candidate because the training bits precede
 * the payload -- holding the old start while changing the rate would measure a
 * different frame rather than a better rate.
 */
static float exp011_mean_margin(const int16_t *fsk, size_t fsk_len,
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

/* Demodulate `expect_len` payload bytes at a given timing; 1 if the PHY header
   length matches and the CRC passes. Recovered payload is copied out. */
static int demod_check(const int16_t *fsk, size_t fsk_len,
                       const mcl_ap_modem_config_t *cfg,
                       float start_t, float sps, float bias,
                       size_t expect_len, uint8_t *out)
{
    uint8_t demod[MCL_AP_MODEM_HEADER_BYTES + MCL_AP_MODEM_MAX_PAYLOAD_BYTES];
    size_t bytes = MCL_AP_MODEM_HEADER_BYTES + expect_len;
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
    if (demod[0] != (uint8_t)expect_len) return 0;
    received = (uint16_t)(((uint16_t)demod[1] << 8) | demod[2]);
    computed = mcl_ap_modem_crc16(demod + MCL_AP_MODEM_HEADER_BYTES, expect_len);
    if (received != computed) return 0;
    memcpy(out, demod + MCL_AP_MODEM_HEADER_BYTES, expect_len);
    return 1;
}

/* Does the recovered payload decode as the object we expect? A passing CRC is
   a modem result; this is the protocol result, and they are reported apart. */
static int object_matches(cell_t cell, const uint8_t *bytes, size_t count)
{
    mcl_wire_tier0_t object;
    size_t consumed = 0u;
    mcl_wire_kind_t want;

    if (mcl_wire_tier0_decode(bytes, count, &object, &consumed) != MCL_WIRE_OK) {
        return 0;
    }
    want = (cell == KIND_PRESENCE) ? MCL_WIRE_KIND_PRESENCE
         : (cell == KIND_ACCEPT)   ? MCL_WIRE_KIND_TRANSPORT_ACCEPT
                                   : MCL_WIRE_KIND_TRANSPORT_OFFER;
    if (object.kind != want) return 0;
    if (object.source_ref != PEER_SOURCE_REF) return 0;

    switch (cell) {
    case KIND_PRESENCE:
        return (object.body.presence.ttl == 60u) ? 1 : 0;
    case KIND_ACCEPT:
        return (object.body.transport_accept.migration_ref == MIGRATION_REF &&
                object.body.transport_accept.session_ref == SESSION_REF) ? 1 : 0;
    default:
        return (object.body.transport_offer.migration_ref == MIGRATION_REF &&
                object.body.transport_offer.endpoint_token == ENDPOINT_TOKEN)
               ? 1 : 0;
    }
}

static int do_sizes(void)
{
    static const cell_t cells[3] = { KIND_PRESENCE, KIND_ACCEPT, KIND_OFFER };
    uint8_t object[MCL_WIRE_TIER0_MAX_SIZE];
    int i;

    printf("major-1 bootstrap objects, encoded by mcl-wire\n");
    for (i = 0; i < 3; ++i) {
        size_t n = build_object(cells[i], object, sizeof(object));
        if (n == 0u) {
            printf("  %-18s ENCODE FAILED\n", cell_name(cells[i]));
            return 2;
        }
        printf("  %-18s %2u bytes  hex=", cell_name(cells[i]), (unsigned)n);
        print_hex(object, n);
        printf("\n");
    }
    return 0;
}

static int do_hex(cell_t cell)
{
    uint8_t object[MCL_WIRE_TIER0_MAX_SIZE];
    size_t n = build_object(cell, object, sizeof(object));
    if (n == 0u) return 2;
    print_hex(object, n);
    printf("\n");
    return 0;
}

static int do_gen(cell_t cell, const char *path)
{
    uint8_t object[MCL_WIRE_TIER0_MAX_SIZE];
    mcl_ap_modem_config_t cfg;
    size_t samples = 0u;
    size_t n = build_object(cell, object, sizeof(object));

    if (n == 0u) { printf("wire encode failed\n"); return 2; }
    configure(&cfg, 1);
    if (mcl_ap_modem_encode(&cfg, object, n, g_pcm, MAX_SAMPLES, &samples)
        != MCL_AP_MODEM_OK) {
        printf("modulate failed\n");
        return 2;
    }
    if (wav_write_pcm16(path, g_pcm, samples) != WAV_OK) {
        printf("cannot write %s\n", path);
        return 2;
    }
    printf("wrote %s: %u samples (%.3f s) band %.0f/%.0f chirp %.0f-%.0f\n",
           path, (unsigned)samples,
           (double)samples / (double)MCL_AP_MODEM_SAMPLE_RATE_HZ,
           (double)cfg.fsk_freq_0_hz, (double)cfg.fsk_freq_1_hz,
           (double)cfg.preamble_f_start_hz, (double)cfg.preamble_f_end_hz);
    printf("payload %s bytes=%u hex=", cell_name(cell), (unsigned)n);
    print_hex(object, n);
    printf("\n");
    return 0;
}

static int do_decode(cell_t cell, const char *path)
{
    static mcl_ap_modem_scratch_t scratch;
    uint8_t expect[MCL_WIRE_TIER0_MAX_SIZE];
    uint8_t got_stock[MCL_AP_MODEM_MAX_PAYLOAD_BYTES];
    uint8_t got_ref[MCL_AP_MODEM_MAX_PAYLOAD_BYTES];
    mcl_ap_modem_config_t cfg;
    mcl_ap_modem_rx_t info;
    mcl_ap_modem_status_t st;
    size_t count = 0u, expect_len, ref_len, index = 0u;
    size_t fsk_start, fsk_len, frame_bits;
    const int16_t *fsk;
    float correlation = 0.0f, sps0 = 0.0f, bias = 0.0f;
    float best_sps, best_score, cand, start_best;
    int32_t phase = 0;
    int stock_ok = 0, ref_ok = 0, stock_obj = 0, ref_obj = 0;

    expect_len = build_object(cell, expect, sizeof(expect));
    if (expect_len == 0u) { printf("wire encode failed\n"); return 2; }

    configure(&cfg, 0);
    if (wav_read_pcm16(path, g_pcm, MAX_SAMPLES, &count) != WAV_OK) {
        printf("cannot read %s\n", path);
        return 2;
    }

    /* ---- the shipped receiver, unmodified ---- */
    memset(&info, 0, sizeof(info));
    st = mcl_ap_modem_decode(&cfg, g_pcm, count, &scratch,
                             got_stock, sizeof(got_stock), &info);
    if (st == MCL_AP_MODEM_OK && info.payload_bytes == expect_len) {
        stock_ok = 1;
        stock_obj = object_matches(cell, got_stock, expect_len);
    }

    /* ---- the candidate: blind whole-frame rate refinement ---- */
    ref_len = generate_preamble_iq(&cfg, scratch.ref_i, scratch.ref_q);
    acquire(&cfg, g_pcm, count, scratch.ref_i, scratch.ref_q, ref_len,
            &index, &correlation);
    if (correlation < cfg.detection_threshold) {
        printf("%-34s acquired=0 corr=%.3f  stock=--  refined=--\n",
               path, (double)correlation);
        return 4;
    }
    fsk_start = index + ref_len;
    fsk = g_pcm + fsk_start;
    fsk_len = count - fsk_start;

    estimate_timing(fsk, fsk_len, (size_t)cfg.training_bits,
                    cfg.fsk_freq_0_hz, cfg.fsk_freq_1_hz, &phase, &sps0, &bias);
    frame_bits = (MCL_AP_MODEM_HEADER_BYTES + expect_len) * 8u;

    best_sps = sps0;
    best_score = -1e30f;
    for (cand = NOMINAL_SPS - 0.6f; cand <= NOMINAL_SPS + 0.6f; cand += 0.01f) {
        float start_c = (float)phase + (float)cfg.training_bits * cand;
        float score = exp011_mean_margin(fsk, fsk_len, &cfg, start_c, cand, bias,
                                  frame_bits);
        if (score > best_score) { best_score = score; best_sps = cand; }
    }
    start_best = (float)phase + (float)cfg.training_bits * best_sps;
    ref_ok = demod_check(fsk, fsk_len, &cfg, start_best, best_sps, bias,
                         expect_len, got_ref);
    if (ref_ok) ref_obj = object_matches(cell, got_ref, expect_len);

    printf("%-34s acquired=1 corr=%.3f sps %7.3f->%7.3f  stock=%s obj=%s  "
           "refined=%s obj=%s\n",
           path, (double)correlation, (double)sps0, (double)best_sps,
           stock_ok ? "ok " : "BAD", stock_obj ? "ok " : "-- ",
           ref_ok ? "ok " : "BAD", ref_obj ? "ok " : "-- ");

    if (stock_obj && ref_obj) return 0;
    if (ref_obj) return 1;
    return 3;
}

/*
 * Decode a file with the shipped receiver and report ONLY the outcome, with no
 * expectation of what the payload should be.
 *
 * This is what the conformance vectors need: a negative vector has no expected
 * payload, so a decoder that must be told one in advance cannot be run against
 * it. The reason word matches the vocabulary in conformance/vectors/VECTORS.md,
 * so the reference and the independent receiver are compared on the same terms
 * rather than on two descriptions of the same thing.
 */
static int do_raw(const char *path)
{
    static mcl_ap_modem_scratch_t scratch;
    uint8_t payload[MCL_AP_MODEM_MAX_PAYLOAD_BYTES];
    mcl_ap_modem_config_t cfg;
    mcl_ap_modem_rx_t info;
    mcl_ap_modem_status_t st;
    size_t count = 0u;
    const char *word;

    configure(&cfg, 0);
    if (wav_read_pcm16(path, g_pcm, MAX_SAMPLES, &count) != WAV_OK) {
        printf("io\n");
        return 2;
    }
    memset(&info, 0, sizeof(info));
    st = mcl_ap_modem_decode(&cfg, g_pcm, count, &scratch,
                             payload, sizeof(payload), &info);

    switch (st) {
    case MCL_AP_MODEM_OK:                 word = "accept";       break;
    case MCL_AP_MODEM_ERR_NOT_ACQUIRED:   word = "not acquired"; break;
    case MCL_AP_MODEM_ERR_CRC:            word = "crc";          break;
    case MCL_AP_MODEM_ERR_PAYLOAD:        word = "payload";      break;
    case MCL_AP_MODEM_ERR_SYNC:           word = "sync";         break;
    default:                              word = "other";        break;
    }
    printf("%s", word);
    if (st == MCL_AP_MODEM_OK) {
        printf(" ");
        print_hex(payload, info.payload_bytes);
    }
    printf("\n");
    return (st == MCL_AP_MODEM_OK) ? 0 : 3;
}

int main(int argc, char **argv)
{
    cell_t cell;
    char *positional[4];
    int i, n = 0;

    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--band") == 0 && i + 1 < argc) {
            if (parse_band(argv[++i]) != 0) {
                fprintf(stderr, "bad --band, expected <f0>:<f1> in Hz\n");
                return 2;
            }
        } else if (n < 4) {
            positional[n++] = argv[i];
        } else {
            fprintf(stderr, "too many arguments\n");
            return 2;
        }
    }
    for (i = 0; i < n; ++i) argv[i + 1] = positional[i];
    argc = n + 1;

    if (argc >= 2 && strcmp(argv[1], "sizes") == 0) return do_sizes();
    if (argc == 3 && strcmp(argv[1], "raw") == 0) return do_raw(argv[2]);
    if (argc == 3 && strcmp(argv[1], "hex") == 0) {
        if (parse_cell(argv[2], &cell) != 0) return 2;
        return do_hex(cell);
    }
    if (argc == 4 && strcmp(argv[1], "gen") == 0) {
        if (parse_cell(argv[2], &cell) != 0) return 2;
        return do_gen(cell, argv[3]);
    }
    if (argc == 4 && strcmp(argv[1], "decode") == 0) {
        if (parse_cell(argv[2], &cell) != 0) return 2;
        return do_decode(cell, argv[3]);
    }
    fprintf(stderr,
            "  --band <f0>:<f1>  override the FSK pair, in Hz\n"
            "usage: bootstrap_air sizes\n"
            "       bootstrap_air hex    <presence|accept|offer>\n"
            "       bootstrap_air gen    <presence|accept|offer> <out.wav>\n"
            "       bootstrap_air decode <presence|accept|offer> <in.wav>\n");
    return 2;
}
