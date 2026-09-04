/*
 * MCL-AP node, host side.
 *
 * The laptop half of Experiment 004. It is the same program the DFR1154 runs,
 * minus the I2S peripherals: it builds a Tier-0 object with mcl-wire, wraps it
 * in a Link frame with mcl-link, and modulates or demodulates it with the
 * MCL-AP candidate modem -- the same three source files the firmware compiles.
 *
 *   mcl_ap_node gen-frame <out.wav>     a Link frame carrying PRESENCE
 *   mcl_ap_node gen-wire  <out.wav>     a bare Tier-0 PRESENCE
 *   mcl_ap_node decode-frame <in.wav>   demodulate, then decode as a frame
 *   mcl_ap_node decode-wire  <in.wav>   demodulate, then decode as an object
 *   mcl_ap_node selftest                encode and decode with no audio
 *
 * The two decode verbs are separate rather than one that sniffs. The
 * experimental AP profile carries raw Wire bytes, the node also sends complete
 * Link frames, and both are strict decoders -- so a receiver could try one and
 * fall back to the other. It does not. The transmitter always knows what it
 * sent, so the receiver is told, and no byte string is ever given two possible
 * meanings on the strength of which decoder happened to accept it first.
 *
 * Exit status: 0 recovered and decoded, 3 acquired but not recovered,
 * 4 not acquired, 2 usage or I/O. A trial script counts these.
 */

#include "mcl/ap_modem.h"
#include "mcl/wire.h"
#include "mcl/link.h"
#include "wav_io.h"

#include <stdio.h>
#include <string.h>

#define MAX_SAMPLES 600000u

static int16_t g_pcm[MAX_SAMPLES];
static mcl_ap_modem_scratch_t g_scratch;

/* The host peer's contact reference. Different from the board's, so a
   recovered source_ref says which peer sent the frame -- for correlation
   only, never as proof of anything. */
#define HOST_SOURCE_REF 0x0A11EDC0u

static size_t build_presence(uint8_t *out, size_t capacity)
{
    mcl_wire_tier0_t object;
    size_t written = 0u;

    memset(&object, 0, sizeof(object));
    object.kind = MCL_WIRE_KIND_PRESENCE;
    object.priority = 1u;
    object.source_ref = HOST_SOURCE_REF;
    object.body.presence.capability_tag = 0x000004u;   /* acoustic only */
    object.body.presence.ttl = 60u;

    if (mcl_wire_tier0_encode_at_major(MCL_WIRE_STABLE_MAJOR, &object,
                                       out, capacity, &written) != MCL_WIRE_OK) {
        return 0u;
    }
    return written;
}

static size_t build_frame(uint8_t *out, size_t capacity,
                          const uint8_t *payload, size_t payload_len)
{
    mcl_link_frame_t frame;
    size_t written = 0u;

    memset(&frame, 0, sizeof(frame));
    frame.frame_class = MCL_LINK_CLASS_CONTACT;
    frame.flags = MCL_LINK_FLAG_SEQUENCE | MCL_LINK_FLAG_FRAME_CHECK;
    frame.source_ref = HOST_SOURCE_REF;
    frame.sequence = 1u;
    frame.payload = payload;
    frame.payload_len = (uint16_t)payload_len;

    if (mcl_link_frame_encode_at_major(MCL_LINK_STABLE_MAJOR, &frame,
                                       out, capacity, &written) != MCL_LINK_OK) {
        return 0u;
    }
    return written;
}

static void print_hex(const uint8_t *bytes, size_t count)
{
    size_t i;
    for (i = 0u; i < count; ++i) {
        printf("%02X", bytes[i]);
    }
}

static void configure(mcl_ap_modem_config_t *config, int for_transmit)
{
    mcl_ap_modem_default_config(config);
    if (for_transmit) {
        /* The board trims trailing silence to fit its shared buffer. The host
           matches it so both directions carry the same waveform, and a
           difference in results is a difference in the path. */
        config->trailing_silence_s = 0.10f;
    }
}

static int do_gen(const char *path, int as_frame)
{
    uint8_t object[MCL_WIRE_TIER0_MAX_SIZE];
    uint8_t frame[64];
    const uint8_t *payload;
    size_t payload_len;
    mcl_ap_modem_config_t config;
    size_t samples = 0u;

    payload_len = build_presence(object, sizeof(object));
    if (payload_len == 0u) {
        printf("wire encode failed\n");
        return 2;
    }
    payload = object;

    if (as_frame) {
        payload_len = build_frame(frame, sizeof(frame), object, payload_len);
        if (payload_len == 0u) {
            printf("link encode failed\n");
            return 2;
        }
        payload = frame;
    }

    configure(&config, 1);
    if (mcl_ap_modem_encode(&config, payload, payload_len,
                            g_pcm, MAX_SAMPLES, &samples) != MCL_AP_MODEM_OK) {
        printf("modulate failed\n");
        return 2;
    }
    if (wav_write_pcm16(path, g_pcm, samples) != WAV_OK) {
        printf("cannot write %s\n", path);
        return 2;
    }

    printf("wrote %s: %u samples (%.3f s)\n", path, (unsigned)samples,
           (double)samples / (double)MCL_AP_MODEM_SAMPLE_RATE_HZ);
    printf("payload %s bytes=%u hex=", as_frame ? "FRAME" : "WIRE",
           (unsigned)payload_len);
    print_hex(payload, payload_len);
    printf("\n");
    return 0;
}

static int report_object(const uint8_t *bytes, size_t count)
{
    mcl_wire_tier0_t object;
    size_t consumed = 0u;

    if (mcl_wire_tier0_decode(bytes, count, &object, &consumed) != MCL_WIRE_OK) {
        printf("OBJECT decode=refused\n");
        return 3;
    }
    if (object.kind != MCL_WIRE_KIND_PRESENCE) {
        printf("OBJECT kind=%u (not PRESENCE)\n", (unsigned)object.kind);
        return 3;
    }
    printf("OBJECT kind=PRESENCE consumed=%u priority=%u source_ref=%lu "
           "capability_tag=%lu ttl=%u\n",
           (unsigned)consumed, (unsigned)object.priority,
           (unsigned long)object.source_ref,
           (unsigned long)object.body.presence.capability_tag,
           (unsigned)object.body.presence.ttl);
    return 0;
}

static int do_decode(const char *path, int as_frame)
{
    size_t count = 0u;
    uint8_t recovered[MCL_AP_MODEM_MAX_PAYLOAD_BYTES];
    mcl_ap_modem_config_t config;
    mcl_ap_modem_rx_t info;
    mcl_ap_modem_status_t rc;

    if (wav_read_pcm16(path, g_pcm, MAX_SAMPLES, &count) != WAV_OK) {
        printf("cannot read %s as 48 kHz PCM16 mono\n", path);
        return 2;
    }

    configure(&config, 0);
    rc = mcl_ap_modem_decode(&config, g_pcm, count, &g_scratch,
                             recovered, sizeof(recovered), &info);

    printf("rc=%ld acquired=%u index=%u corr=%.6f crc_valid=%u "
           "rx_crc=%04X calc_crc=%04X bytes=%u\n",
           (long)rc, (unsigned)info.acquired,
           (unsigned)info.acquisition_index, (double)info.correlation,
           (unsigned)info.crc_valid, (unsigned)info.received_crc,
           (unsigned)info.computed_crc, (unsigned)info.payload_bytes);

    if (info.acquired == 0u) {
        printf("RESULT: not acquired\n");
        return 4;
    }
    if (rc != MCL_AP_MODEM_OK) {
        printf("RESULT: acquired, no recovery\n");
        return 3;
    }

    printf("payload=");
    print_hex(recovered, info.payload_bytes);
    printf("\n");

    if (as_frame) {
        mcl_link_frame_t frame;
        size_t consumed = 0u;
        if (mcl_link_frame_decode(recovered, info.payload_bytes, &frame,
                                  &consumed) != MCL_LINK_OK) {
            printf("FRAME decode=refused\n");
            return 3;
        }
        printf("FRAME link_major=%u class=%u flags=0x%02X source_ref=%lu "
               "sequence=%u payload_len=%u consumed=%u\n",
               (unsigned)frame.link_major, (unsigned)frame.frame_class,
               (unsigned)frame.flags, (unsigned long)frame.source_ref,
               (unsigned)frame.sequence, (unsigned)frame.payload_len,
               (unsigned)consumed);
        if (consumed != info.payload_bytes) {
            printf("FRAME trailing bytes: refused\n");
            return 3;
        }
        if (report_object(frame.payload, frame.payload_len) != 0) {
            return 3;
        }
    } else if (report_object(recovered, info.payload_bytes) != 0) {
        return 3;
    }

    printf("RESULT: RECOVERED\n");
    return 0;
}

static int do_selftest(void)
{
    uint8_t object[MCL_WIRE_TIER0_MAX_SIZE];
    uint8_t frame[64];
    uint8_t recovered[MCL_AP_MODEM_MAX_PAYLOAD_BYTES];
    mcl_ap_modem_config_t config;
    mcl_ap_modem_rx_t info;
    size_t object_len, frame_len, samples = 0u;

    object_len = build_presence(object, sizeof(object));
    printf("wire_encode bytes=%u hex=", (unsigned)object_len);
    print_hex(object, object_len);
    printf("\n");

    frame_len = build_frame(frame, sizeof(frame), object, object_len);
    printf("link_encode bytes=%u hex=", (unsigned)frame_len);
    print_hex(frame, frame_len);
    printf("\n");

    configure(&config, 1);
    if (mcl_ap_modem_encode(&config, frame, frame_len,
                            g_pcm, MAX_SAMPLES, &samples) != MCL_AP_MODEM_OK) {
        printf("SELFTEST FAIL modulate\n");
        return 1;
    }
    printf("modulate samples=%u\n", (unsigned)samples);

    if (mcl_ap_modem_decode(&config, g_pcm, samples, &g_scratch,
                            recovered, sizeof(recovered), &info)
            != MCL_AP_MODEM_OK ||
        info.payload_bytes != frame_len ||
        memcmp(recovered, frame, frame_len) != 0) {
        printf("SELFTEST FAIL round_trip\n");
        return 1;
    }
    printf("SELFTEST PASS corr=%.4f\n", (double)info.correlation);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: mcl_ap_node gen-frame|gen-wire|decode-frame|"
               "decode-wire <wav> | selftest\n");
        return 2;
    }
    if (strcmp(argv[1], "selftest") == 0) {
        return do_selftest();
    }
    if (argc < 3) {
        printf("this verb needs a WAV path\n");
        return 2;
    }
    if (strcmp(argv[1], "gen-frame") == 0)    return do_gen(argv[2], 1);
    if (strcmp(argv[1], "gen-wire") == 0)     return do_gen(argv[2], 0);
    if (strcmp(argv[1], "decode-frame") == 0) return do_decode(argv[2], 1);
    if (strcmp(argv[1], "decode-wire") == 0)  return do_decode(argv[2], 0);

    printf("unknown verb %s\n", argv[1]);
    return 2;
}
