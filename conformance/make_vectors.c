/*
 * AP-BOOTSTRAP-1 PCM conformance vectors.
 *
 * WHY VECTORS AND NOT JUST A SPECIFICATION
 *
 * `spec/ap-bootstrap-1.md` §11 says promotion needs a clean-room receiver
 * cross-tested against the reference. That test cannot exist without an agreed
 * corpus: two implementations that each decode their own output prove nothing
 * about each other. These are the bytes on which the two are compared.
 *
 * THE NEGATIVES ARE THE POINT
 *
 * A receiver that accepts everything passes every positive vector. Six of the
 * nine vectors here MUST be refused, and each one refuses for a different
 * reason, so a receiver that gets the right answer for the wrong reason is
 * still caught:
 *
 *   - a payload CRC that does not match          -> ERR_CRC
 *   - a declared length of zero                  -> ERR_PAYLOAD
 *   - a declared length above the 64-byte cap    -> ERR_PAYLOAD
 *   - a frame cut off inside the payload         -> ERR_SYNC
 *   - a frame with no preamble at all            -> ERR_NOT_ACQUIRED
 *   - silence                                    -> ERR_NOT_ACQUIRED
 *
 * The distinction between the last two matters more than it looks: "heard
 * nothing" and "heard something and could not read it" are different problems
 * with the same shape, and only the second means a link is nearly working.
 *
 * It includes src/ap_modem.c to reach modulate(), because a deliberately wrong
 * CRC cannot be produced through the public encode API -- which computes the
 * CRC correctly, as it should.
 *
 * Usage: make_vectors <output directory>
 */

/* MSVC deprecates fopen in favour of fopen_s, which is not C99 and is not
   available everywhere this has to build. wav_io.h defines the same macro for
   the same reason, but it is included after src/ap_modem.c, and by then the
   CRT headers that arm the deprecation have already been pulled in. It has to
   be first in the translation unit, not merely present in it. */
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "../src/ap_modem.c"
#include "../tools/wav_io.h"

#include "mcl/wire.h"

#include <stdio.h>
#include <string.h>

#define MAX_SAMPLES 600000u

static int16_t g_pcm[MAX_SAMPLES];
static char g_path[512];

/* The same references Experiment 011 transmitted, so a vector and a capture
   describe the same object. */
#define PEER_SOURCE_REF   0x0A11EDC0u
#define MIGRATION_REF     0x00C0FFEEu
#define ENDPOINT_TOKEN    0x5E1EC7EDu
#define SESSION_REF       0x0D0C0DE5u

static void configure(mcl_ap_modem_config_t *config)
{
    mcl_ap_modem_default_config(config);
    config->trailing_silence_s = 0.10f;
}

/*
 * Assemble a frame with FULL control of the PHY header, which is what makes the
 * negative vectors possible. `declared` and `crc` are written as given, so they
 * can disagree with the payload on purpose.
 */
static size_t build_frame(const mcl_ap_modem_config_t *config,
                          const uint8_t *payload, size_t payload_bytes,
                          uint8_t declared, uint16_t crc,
                          int include_preamble)
{
    uint8_t training[MCL_AP_MODEM_TRAINING_BITS / 8u];
    uint8_t header[MCL_AP_MODEM_HEADER_BYTES];
    size_t pos = 0u, lead, preamble_n, i;
    double phase = 0.0;
    float scale;

    lead = round_to_samples(config->leading_silence_s);
    for (i = 0u; i < lead; ++i) {
        g_pcm[pos++] = 0;
    }

    if (include_preamble) {
        preamble_n = preamble_length(config);
        scale = preamble_scale(config, preamble_n);
        for (i = 0u; i < preamble_n; ++i) {
            g_pcm[pos++] = to_pcm(sinf(chirp_phase(config, i)) * scale);
        }
    }

    for (i = 0u; i < sizeof(training); ++i) {
        training[i] = (uint8_t)MCL_AP_MODEM_TRAINING_BYTE;
    }
    pos += modulate(training, sizeof(training),
                    config->fsk_freq_0_hz, config->fsk_freq_1_hz,
                    g_pcm + pos, MAX_SAMPLES - pos, &phase);

    header[0] = declared;
    header[1] = (uint8_t)(crc >> 8u);
    header[2] = (uint8_t)(crc & 0xFFu);
    pos += modulate(header, MCL_AP_MODEM_HEADER_BYTES,
                    config->fsk_freq_0_hz, config->fsk_freq_1_hz,
                    g_pcm + pos, MAX_SAMPLES - pos, &phase);

    if (payload_bytes > 0u) {
        pos += modulate(payload, payload_bytes,
                        config->fsk_freq_0_hz, config->fsk_freq_1_hz,
                        g_pcm + pos, MAX_SAMPLES - pos, &phase);
    }

    for (i = 0u; i < round_to_samples(config->trailing_silence_s); ++i) {
        if (pos >= MAX_SAMPLES) break;
        g_pcm[pos++] = 0;
    }
    return pos;
}

static size_t build_object(mcl_wire_kind_t kind, uint8_t *out, size_t capacity)
{
    mcl_wire_tier0_t object;
    size_t written = 0u;

    memset(&object, 0, sizeof(object));
    object.kind = kind;
    object.priority = 1u;
    object.source_ref = PEER_SOURCE_REF;

    if (kind == MCL_WIRE_KIND_PRESENCE) {
        object.body.presence.capability_tag = 0x000004u;
        object.body.presence.ttl = 60u;
    } else if (kind == MCL_WIRE_KIND_TRANSPORT_ACCEPT) {
        object.body.transport_accept.migration_ref = MIGRATION_REF;
        object.body.transport_accept.transport_id = 3u;
        object.body.transport_accept.profile_id = 1u;
        object.body.transport_accept.session_ref = SESSION_REF;
    } else {
        object.body.transport_offer.migration_ref = MIGRATION_REF;
        object.body.transport_offer.transport_id = 3u;
        object.body.transport_offer.profile_id = 1u;
        object.body.transport_offer.endpoint_token = ENDPOINT_TOKEN;
        object.body.transport_offer.validity = 60u;
    }

    if (mcl_wire_tier0_encode_at_major(MCL_WIRE_STABLE_MAJOR, &object,
                                       out, capacity, &written) != MCL_WIRE_OK) {
        return 0u;
    }
    return written;
}

static int write_vector(const char *dir, const char *name, size_t samples,
                        FILE *manifest, const char *expect,
                        const char *payload_hex, const char *why)
{
    int n = snprintf(g_path, sizeof(g_path), "%s/%s.wav", dir, name);
    if (n < 0 || (size_t)n >= sizeof(g_path)) {
        fprintf(stderr, "path too long: %s\n", name);
        return -1;
    }
    if (wav_write_pcm16(g_path, g_pcm, samples) != WAV_OK) {
        fprintf(stderr, "cannot write %s\n", g_path);
        return -1;
    }
    fprintf(manifest, "| `%s.wav` | %s | `%s` | %s |\n",
            name, expect, payload_hex, why);
    printf("  %-28s %-18s %s\n", name, expect, why);
    return 0;
}

static void hex_of(const uint8_t *b, size_t n, char *out, size_t cap)
{
    size_t i;
    out[0] = '\0';
    for (i = 0u; i < n && (2u * i + 1u) < cap; ++i) {
        snprintf(out + 2u * i, cap - 2u * i, "%02X", b[i]);
    }
}

int main(int argc, char **argv)
{
    mcl_ap_modem_config_t cfg;
    uint8_t object[MCL_WIRE_TIER0_MAX_SIZE];
    char hex[2u * MCL_WIRE_TIER0_MAX_SIZE + 1u];
    size_t n, samples;
    FILE *manifest;
    const char *dir;
    int rc = 0;

    if (argc != 2) {
        fprintf(stderr, "usage: make_vectors <output directory>\n");
        return 2;
    }
    dir = argv[1];
    configure(&cfg);

    if (snprintf(g_path, sizeof(g_path), "%s/VECTORS.md", dir) < 0) {
        return 2;
    }
    /*
     * Binary mode, deliberately. On Windows a text-mode stream turns every
     * newline into CRLF, while .gitattributes stores the file as LF -- so the
     * generated manifest differs from the stored one on one platform and not
     * the other, and any digest taken over the working tree disagrees with a
     * fresh clone. That is the same defect the release bundle was repaired for
     * and that Experiment 011's evidence hit the same day.
     */
    manifest = fopen(g_path, "wb");
    if (manifest == NULL) {
        fprintf(stderr, "cannot write %s\n", g_path);
        return 2;
    }

    fprintf(manifest,
        "# AP-BOOTSTRAP-1 PCM conformance vectors\n\n"
        "**Generated by `make_vectors.c`. Do not edit by hand.**\n\n"
        "Every file is 48 kHz monaural signed 16-bit PCM, and every one is a\n"
        "complete `AP-BOOTSTRAP-1` transmission as `spec/ap-bootstrap-1.md`\n"
        "describes it -- 0.1 s of silence, a 0.2 s chirp, 16 training symbols,\n"
        "a 3-byte PHY header, the payload, and trailing silence.\n\n"
        "## How to use these\n\n"
        "A receiver is conformant against this corpus when it recovers the\n"
        "stated payload from every `accept` vector and produces the stated\n"
        "refusal for every `refuse` vector. **The refusals carry the weight.**\n"
        "A receiver that accepts everything passes all three positive vectors\n"
        "and is not a receiver.\n\n"
        "The expected refusals distinguish *heard nothing* from *heard\n"
        "something and could not read it*. Those are different problems with\n"
        "the same shape, and only the second means a link is nearly working, so\n"
        "an implementation that collapses them into one failure is not\n"
        "conformant even though it rejects the right files.\n\n"
        "| Vector | Expected | Payload | Why |\n"
        "|---|---|---|---|\n");

    printf("AP-BOOTSTRAP-1 vectors -> %s\n\n", dir);

    /* ---- positive: the three objects the profile exists to carry ---- */

    n = build_object(MCL_WIRE_KIND_PRESENCE, object, sizeof(object));
    hex_of(object, n, hex, sizeof(hex));
    samples = build_frame(&cfg, object, n, (uint8_t)n,
                          mcl_ap_modem_crc16(object, n), 1);
    rc |= write_vector(dir, "01-presence-10b", samples, manifest,
                       "accept", hex, "the 10-byte first-contact announcement");

    n = build_object(MCL_WIRE_KIND_TRANSPORT_ACCEPT, object, sizeof(object));
    hex_of(object, n, hex, sizeof(hex));
    samples = build_frame(&cfg, object, n, (uint8_t)n,
                          mcl_ap_modem_crc16(object, n), 1);
    rc |= write_vector(dir, "02-transport-accept-16b", samples, manifest,
                       "accept", hex, "the 16-byte reply");

    n = build_object(MCL_WIRE_KIND_TRANSPORT_OFFER, object, sizeof(object));
    hex_of(object, n, hex, sizeof(hex));
    samples = build_frame(&cfg, object, n, (uint8_t)n,
                          mcl_ap_modem_crc16(object, n), 1);
    rc |= write_vector(dir, "03-transport-offer-17b", samples, manifest,
                       "accept", hex,
                       "the 17-byte offer, the largest object in the sequence");

    /* ---- negative ---- */

    n = build_object(MCL_WIRE_KIND_PRESENCE, object, sizeof(object));
    samples = build_frame(&cfg, object, n, (uint8_t)n,
                          (uint16_t)(mcl_ap_modem_crc16(object, n) ^ 0xFFFFu), 1);
    rc |= write_vector(dir, "04-refuse-bad-crc", samples, manifest,
                       "refuse: CRC", "-",
                       "a valid PRESENCE whose header CRC is inverted");

    samples = build_frame(&cfg, object, n, 0u,
                          mcl_ap_modem_crc16(object, n), 1);
    rc |= write_vector(dir, "05-refuse-zero-length", samples, manifest,
                       "refuse: payload", "-",
                       "declared length 0, which names no payload");

    samples = build_frame(&cfg, object, n, 200u,
                          mcl_ap_modem_crc16(object, n), 1);
    rc |= write_vector(dir, "06-refuse-length-over-cap", samples, manifest,
                       "refuse: payload", "-",
                       "declared length 200, above the 64-byte cap; a receiver "
                       "must reject this **before** reading 200 bytes");

    /*
     * Truncated inside the payload. The frame is built normally and then cut
     * short of the declared length, which is what a real transmission
     * interrupted mid-frame looks like.
     */
    samples = build_frame(&cfg, object, n, (uint8_t)n,
                          mcl_ap_modem_crc16(object, n), 1);
    {
        /*
         * Cut the TRAILING SILENCE PLUS four payload symbols.
         *
         * The first version of this vector removed 640 samples, which is less
         * than the 4800 samples of trailing silence, so the payload arrived
         * complete and a correct receiver accepted it -- as it should have.
         * The independent receiver caught it: the vector was wrong, not the
         * decoder. A negative vector that is secretly positive is worse than
         * no vector, because it certifies a receiver for refusing something it
         * should accept.
         */
        size_t tail = round_to_samples(cfg.trailing_silence_s) + 4u * 160u;
        if (samples > tail) {
            samples -= tail;
        }
    }
    rc |= write_vector(dir, "07-refuse-truncated", samples, manifest,
                       "refuse: sync", "-",
                       "cut off inside the payload, as an interrupted "
                       "transmission is");

    samples = build_frame(&cfg, object, n, (uint8_t)n,
                          mcl_ap_modem_crc16(object, n), 0);
    rc |= write_vector(dir, "08-refuse-no-preamble", samples, manifest,
                       "refuse: not acquired", "-",
                       "a complete, correct frame with no chirp in front of it");

    memset(g_pcm, 0, sizeof(int16_t) * 96000u);
    rc |= write_vector(dir, "09-refuse-silence", 96000u, manifest,
                       "refuse: not acquired", "-",
                       "two seconds of digital silence");

    fprintf(manifest,
        "\n## What these vectors do not cover\n\n"
        "They are **noise-free**. Every one is the transmitter's own output,\n"
        "so they test that a receiver reads the format correctly and say\n"
        "nothing about whether it works over air. The over-air evidence is\n"
        "`experiments/011-bootstrap-over-air/`, and the two are not\n"
        "substitutes: a receiver can pass every vector here and still fail in\n"
        "a room, because the thing that fails in a room is timing recovery\n"
        "and these vectors have perfect timing.\n\n"
        "In particular **nothing here exercises the whole-frame rate\n"
        "refinement that §6.4 makes mandatory**, because these frames decode on\n"
        "the first attempt. A receiver that omitted §6.4 entirely would pass\n"
        "this corpus. That is a known limit of noise-free vectors and is the\n"
        "reason §11 requires a clean-room receiver tested against captures as\n"
        "well as against these.\n");
    fclose(manifest);

    printf("\nmanifest: %s/VECTORS.md\n", dir);
    return rc;
}
