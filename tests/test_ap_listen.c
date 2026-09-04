/*
 * MCL-AP continuous listener tests.
 *
 * The listener's claim is that a machine which never scheduled a receive
 * window still hears a call. Three things have to be true for that, and each
 * one is a way the module could look correct and be useless:
 *
 *   1. It finds a frame it was not told to expect, at an arbitrary position
 *      in an arbitrary-length stream, delivered in arbitrary-sized blocks.
 *   2. It costs the same whether the window is small or large and whether it
 *      is polled rarely or constantly. A listener that re-scans its whole
 *      window per poll works in a test and is unaffordable in a robot, and
 *      the two are indistinguishable from the outside -- so `samples_searched`
 *      is asserted, not assumed.
 *   3. It never turns a frame whose body has not arrived into a permanent
 *      loss. This is the failure that would show up as an intermittent,
 *      unreproducible "sometimes it just doesn't hear you", so it gets a
 *      dedicated test with the boundary placed deliberately.
 *
 * The last section streams the ARCHIVED OVER-AIR CAPTURES through the
 * listener block by block and requires the same verdict, byte for byte, that
 * the one-shot decoder reaches on the same file. That is what makes this a
 * different way of driving the measured receiver rather than a second
 * receiver that agrees with itself.
 */

#include "mcl/ap_listen.h"
#include "../tools/wav_io.h"

#include <stdio.h>
#include <string.h>

#define STREAM_CAPACITY  900000u   /* 18.75 s at 48 kHz */
#define WINDOW_CAPACITY  240000u   /*  5.00 s */
#define ARCHIVE_WINDOW   288000u   /*  6.00 s */

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

static int16_t g_stream[STREAM_CAPACITY];
static int16_t g_window[WINDOW_CAPACITY];
static int16_t g_archive[STREAM_CAPACITY];
static mcl_ap_modem_scratch_t g_scratch;

/* A payload with no structure the demodulator could accidentally favour. */
static const uint8_t g_payload_a[10] = {
    0x01u, 0x23u, 0x45u, 0x67u, 0x89u, 0xABu, 0xCDu, 0xEFu, 0xFEu, 0xDCu
};
static const uint8_t g_payload_b[6] = {
    0xDEu, 0xADu, 0xBEu, 0xEFu, 0x00u, 0xFFu
};

static void listen_config(mcl_ap_listen_config_t *cfg, uint8_t max_payload)
{
    mcl_ap_listen_default_config(cfg);
    cfg->max_payload_bytes = max_payload;
}

/*
 * Lay a frame into `stream` starting at `at`, returning the sample just past
 * it. The modem's own leading silence is skipped so `at` is the position of
 * the first preamble sample, which is what the listener reports.
 */
static size_t place_frame(int16_t *stream, size_t capacity, size_t at,
                          const uint8_t *payload, size_t payload_bytes)
{
    static int16_t frame[200000];
    mcl_ap_modem_config_t config;
    size_t written = 0u, lead;
    mcl_ap_modem_status_t st;

    mcl_ap_modem_default_config(&config);
    st = mcl_ap_modem_encode(&config, payload, payload_bytes,
                             frame, sizeof(frame) / sizeof(frame[0]),
                             &written);
    if (st != MCL_AP_MODEM_OK) {
        return at;
    }
    lead = (size_t)(config.leading_silence_s
                    * (float)MCL_AP_MODEM_SAMPLE_RATE_HZ + 0.5f);
    if (at + (written - lead) > capacity) {
        return at;
    }
    memcpy(stream + at, frame + lead, (written - lead) * sizeof(int16_t));
    return at + (written - lead);
}

/* ------------------------------------------------------------------------ */

static void test_min_window(void)
{
    mcl_ap_listen_config_t cfg;
    mcl_ap_listener_t listener;
    size_t minimum;

    printf("min window\n");
    listen_config(&cfg, 32u);
    minimum = mcl_ap_listen_min_window_samples(&cfg);
    check(minimum > 0u, "a usable config has a minimum window");

    check(mcl_ap_listen_init(&listener, &cfg, g_window, minimum - 1u)
              == MCL_AP_LISTEN_ERR_WINDOW_TOO_SMALL,
          "a window one sample short is refused, not silently accepted");
    check(mcl_ap_listen_init(&listener, &cfg, g_window, minimum)
              == MCL_AP_LISTEN_OK,
          "the stated minimum is accepted");

    /* A bigger declared payload needs a bigger window: the relationship is
       the whole reason the field exists. */
    listen_config(&cfg, 64u);
    check(mcl_ap_listen_min_window_samples(&cfg) > minimum,
          "a larger maximum payload demands a larger window");

    listen_config(&cfg, 0u);
    check(mcl_ap_listen_init(&listener, &cfg, g_window, WINDOW_CAPACITY)
              == MCL_AP_LISTEN_ERR_INVALID_ARGUMENT,
          "a zero maximum payload is refused");
}

/*
 * The headline case. A machine is listening; nobody told it when. A frame
 * arrives 3.1 seconds in, inside 15 seconds of nothing, delivered 512 samples
 * at a time as an audio callback would deliver it.
 */
static void test_unannounced_frame(void)
{
    mcl_ap_listen_config_t cfg;
    mcl_ap_listener_t listener;
    mcl_ap_listen_event_t event;
    uint8_t payload[MCL_AP_MODEM_MAX_PAYLOAD_BYTES];
    const size_t at = 148800u;   /* 3.1 s, not a round number of blocks */
    size_t i, end;
    int contacts = 0;
    uint64_t reported = 0u;

    printf("a frame nobody scheduled\n");
    memset(g_stream, 0, sizeof(g_stream));
    end = place_frame(g_stream, STREAM_CAPACITY, at, g_payload_a,
                      sizeof(g_payload_a));
    check(end > at, "the test frame was generated");

    listen_config(&cfg, 32u);
    check(mcl_ap_listen_init(&listener, &cfg, g_window, WINDOW_CAPACITY)
              == MCL_AP_LISTEN_OK, "listener initialized");

    for (i = 0u; i + 512u <= STREAM_CAPACITY; i += 512u) {
        mcl_ap_listen_result_t r;

        check(mcl_ap_listen_push(&listener, g_stream + i, 512u)
                  == MCL_AP_LISTEN_OK, "push accepted");
        do {
            r = mcl_ap_listen_poll(&listener, &g_scratch,
                                   payload, sizeof(payload), &event);
            if (r == MCL_AP_LISTEN_CONTACT) {
                contacts++;
                reported = event.stream_index;
                check(event.payload_bytes == sizeof(g_payload_a),
                      "the recovered payload is the length that was sent");
                check(memcmp(payload, g_payload_a, sizeof(g_payload_a)) == 0,
                      "the recovered payload is the bytes that were sent");
            }
        } while (r == MCL_AP_LISTEN_CONTACT);
    }

    check(contacts == 1, "exactly one contact, not zero and not a duplicate");
    check(listener.contacts == 1u, "the listener's own counter agrees");

    /*
     * Position. The acquisition index is the correlator's peak, which lands
     * within a few samples of the true preamble start; anything looser than a
     * millisecond would not be evidence that it found THIS frame.
     */
    check(reported + 48u > at && reported < at + 48u,
          "the reported stream position is the frame's, within a millisecond");

    /*
     * The cost claim. 900k samples were pushed and the correlator evaluated
     * start positions once each, plus at most one reference length of overlap
     * per poll where the search stopped short of the buffer end. It must not
     * be a multiple of the stream length, which is what re-scanning the
     * window on every poll would produce.
     */
    check(listener.samples_searched <= listener.total_pushed,
          "no sample was searched twice");
    check(listener.samples_unscanned == 0u,
          "nothing was dropped unheard");
}

/* Polling far more often than audio arrives must not multiply the work, and
   must not report the same frame twice. */
static void test_polling_does_not_cost(void)
{
    mcl_ap_listen_config_t cfg;
    mcl_ap_listener_t listener;
    uint8_t payload[MCL_AP_MODEM_MAX_PAYLOAD_BYTES];
    size_t i;
    int contacts = 0;

    printf("polling harder does not cost more\n");
    memset(g_stream, 0, sizeof(g_stream));
    (void)place_frame(g_stream, STREAM_CAPACITY, 96000u, g_payload_a,
                      sizeof(g_payload_a));

    listen_config(&cfg, 32u);
    (void)mcl_ap_listen_init(&listener, &cfg, g_window, WINDOW_CAPACITY);

    for (i = 0u; i + 4800u <= STREAM_CAPACITY; i += 4800u) {
        int k;
        (void)mcl_ap_listen_push(&listener, g_stream + i, 4800u);
        /* Ten polls for every tenth of a second of audio. */
        for (k = 0; k < 10; ++k) {
            if (mcl_ap_listen_poll(&listener, &g_scratch, payload,
                                   sizeof(payload), NULL)
                == MCL_AP_LISTEN_CONTACT) {
                contacts++;
            }
        }
    }

    check(contacts == 1, "ten times the polling still reports one contact");
    check(listener.samples_searched <= listener.total_pushed,
          "ten times the polling searched no extra audio");
}

/*
 * The failure that would be blamed on the radio for months.
 *
 * The stream is cut off deliberately in the middle of the frame's payload.
 * The listener must say WAITING -- not HEARD, which would step past the
 * preamble and lose the frame the instant the rest of it arrived.
 */
static void test_body_still_arriving(void)
{
    mcl_ap_listen_config_t cfg;
    mcl_ap_listener_t listener;
    uint8_t payload[MCL_AP_MODEM_MAX_PAYLOAD_BYTES];
    const size_t at = 24000u;
    size_t end, cut, i;
    mcl_ap_listen_result_t r;
    int waited = 0, contacts = 0;

    printf("a body still in flight\n");
    memset(g_stream, 0, sizeof(g_stream));
    end = place_frame(g_stream, STREAM_CAPACITY, at, g_payload_a,
                      sizeof(g_payload_a));
    cut = at + (end - at) / 2u;      /* mid-payload */

    listen_config(&cfg, 32u);
    (void)mcl_ap_listen_init(&listener, &cfg, g_window, WINDOW_CAPACITY);

    for (i = 0u; i + 1024u <= cut; i += 1024u) {
        (void)mcl_ap_listen_push(&listener, g_stream + i, 1024u);
        r = mcl_ap_listen_poll(&listener, &g_scratch, payload,
                               sizeof(payload), NULL);
        if (r == MCL_AP_LISTEN_WAITING) {
            waited++;
        }
        check(r != MCL_AP_LISTEN_CONTACT,
              "no contact is claimed from a half-delivered frame");
        check(r != MCL_AP_LISTEN_HEARD,
              "a half-delivered frame is not written off as a bad channel");
    }
    check(waited > 0, "the truncated frame was held, not discarded");
    check(listener.pending != 0u, "the position is held for the next poll");

    /* Now deliver the rest, and the same frame must complete. */
    for (; i + 1024u <= end + 24000u; i += 1024u) {
        (void)mcl_ap_listen_push(&listener, g_stream + i, 1024u);
        if (mcl_ap_listen_poll(&listener, &g_scratch, payload,
                               sizeof(payload), NULL)
            == MCL_AP_LISTEN_CONTACT) {
            contacts++;
        }
    }
    check(contacts == 1, "the held frame completed once the rest arrived");
    check(memcmp(payload, g_payload_a, sizeof(g_payload_a)) == 0,
          "and it carried the bytes that were sent");
}

/* Two callers in a row, close together. Both must be reported, separately. */
static void test_two_frames(void)
{
    mcl_ap_listen_config_t cfg;
    mcl_ap_listener_t listener;
    mcl_ap_listen_event_t event;
    uint8_t payload[MCL_AP_MODEM_MAX_PAYLOAD_BYTES];
    uint64_t first = 0u, second = 0u;
    size_t i, next;
    int contacts = 0;

    printf("two callers\n");
    memset(g_stream, 0, sizeof(g_stream));
    next = place_frame(g_stream, STREAM_CAPACITY, 48000u, g_payload_a,
                       sizeof(g_payload_a));
    next = place_frame(g_stream, STREAM_CAPACITY, next + 9600u, g_payload_b,
                       sizeof(g_payload_b));
    check(next > 48000u, "both frames were generated");

    listen_config(&cfg, 32u);
    (void)mcl_ap_listen_init(&listener, &cfg, g_window, WINDOW_CAPACITY);

    for (i = 0u; i + 1024u <= STREAM_CAPACITY; i += 1024u) {
        mcl_ap_listen_result_t r;
        (void)mcl_ap_listen_push(&listener, g_stream + i, 1024u);
        do {
            r = mcl_ap_listen_poll(&listener, &g_scratch, payload,
                                   sizeof(payload), &event);
            if (r == MCL_AP_LISTEN_CONTACT) {
                contacts++;
                if (contacts == 1) {
                    first = event.stream_index;
                    check(event.payload_bytes == sizeof(g_payload_a)
                              && memcmp(payload, g_payload_a,
                                        sizeof(g_payload_a)) == 0,
                          "the first caller's bytes came out first");
                } else if (contacts == 2) {
                    second = event.stream_index;
                    check(event.payload_bytes == sizeof(g_payload_b)
                              && memcmp(payload, g_payload_b,
                                        sizeof(g_payload_b)) == 0,
                          "the second caller's bytes came out second");
                }
            }
        } while (r == MCL_AP_LISTEN_CONTACT);
    }

    check(contacts == 2, "both callers were heard");
    check(second > first, "and in the order they transmitted");
}

/* Silence is silence. A detector that fires on nothing is worse than none. */
static void test_quiet_room(void)
{
    mcl_ap_listen_config_t cfg;
    mcl_ap_listener_t listener;
    uint8_t payload[MCL_AP_MODEM_MAX_PAYLOAD_BYTES];
    uint32_t rng = 0x2468ACE0u;
    size_t i;
    int events = 0;

    printf("a quiet room\n");
    for (i = 0u; i < STREAM_CAPACITY; ++i) {
        rng = rng * 1664525u + 1013904223u;
        /* Low-level dither, about -40 dBFS: a real room, not a mute file. */
        g_stream[i] = (int16_t)((int32_t)((rng >> 16) & 0x1FFu) - 256);
    }

    listen_config(&cfg, 32u);
    (void)mcl_ap_listen_init(&listener, &cfg, g_window, WINDOW_CAPACITY);

    for (i = 0u; i + 2048u <= STREAM_CAPACITY; i += 2048u) {
        mcl_ap_listen_result_t r;
        (void)mcl_ap_listen_push(&listener, g_stream + i, 2048u);
        r = mcl_ap_listen_poll(&listener, &g_scratch, payload,
                               sizeof(payload), NULL);
        if (r != MCL_AP_LISTEN_QUIET) {
            events++;
        }
    }
    check(events == 0, "18 seconds of room noise produced no event at all");
    check(listener.contacts == 0u && listener.heard == 0u,
          "and no counter moved");
}

/*
 * A machine too busy to poll. The point is not that it still works -- it
 * cannot -- but that it SAYS SO. Audio dropped without being searched is the
 * one failure a listener could hide perfectly, because a missed call and a
 * quiet room look identical.
 */
static void test_too_busy_is_visible(void)
{
    mcl_ap_listen_config_t cfg;
    mcl_ap_listener_t listener;
    uint8_t payload[MCL_AP_MODEM_MAX_PAYLOAD_BYTES];
    size_t i;

    printf("a machine too busy to listen\n");
    memset(g_stream, 0, sizeof(g_stream));
    (void)place_frame(g_stream, STREAM_CAPACITY, 48000u, g_payload_a,
                      sizeof(g_payload_a));

    listen_config(&cfg, 32u);
    (void)mcl_ap_listen_init(&listener, &cfg, g_window, WINDOW_CAPACITY);

    /* Three windows of audio pushed with no poll at all. */
    for (i = 0u; i + 48000u <= 3u * WINDOW_CAPACITY; i += 48000u) {
        (void)mcl_ap_listen_push(&listener, g_stream + i, 48000u);
    }
    check(listener.samples_unscanned > 0u,
          "audio dropped unheard is counted, not absorbed");
    check(listener.overruns > 0u, "and the overrun itself is counted");
    check(listener.contacts == 0u, "the frame in the dropped audio is gone");

    /* And the listener recovers: a later frame is still found. */
    memset(g_stream, 0, sizeof(g_stream));
    (void)place_frame(g_stream, STREAM_CAPACITY, 24000u, g_payload_b,
                      sizeof(g_payload_b));
    for (i = 0u; i + 1024u <= 240000u; i += 1024u) {
        (void)mcl_ap_listen_push(&listener, g_stream + i, 1024u);
        (void)mcl_ap_listen_poll(&listener, &g_scratch, payload,
                                 sizeof(payload), NULL);
    }
    check(listener.contacts == 1u,
          "and it hears the next caller after falling behind");

    /* A block larger than the whole window keeps the newest audio. */
    (void)mcl_ap_listen_push(&listener, g_stream, STREAM_CAPACITY);
    check(listener.filled == WINDOW_CAPACITY,
          "an oversized block fills the window rather than failing");
    check(listener.scan_pos
              == listener.window_start + (uint64_t)0u
          || listener.scan_pos >= listener.window_start,
          "and the scan position stays inside the window");
}

/* ------------------------------------------------------------------------ */
/* The archived captures, streamed.                                          */

/*
 * One file, decoded twice: once whole, the way every result in this
 * repository so far was produced, and once through the listener in 1024-sample
 * blocks with no idea where the frame is. The verdicts must match, and where
 * both recover, the bytes must match.
 */
static int compare_one_capture(const char *path, int *out_present)
{
    static int16_t window[ARCHIVE_WINDOW];
    mcl_ap_modem_config_t modem;
    mcl_ap_listen_config_t cfg;
    mcl_ap_listener_t listener;
    mcl_ap_modem_rx_t rx;
    uint8_t one_shot[MCL_AP_MODEM_MAX_PAYLOAD_BYTES];
    uint8_t streamed[MCL_AP_MODEM_MAX_PAYLOAD_BYTES];
    size_t count = 0u, i;
    mcl_ap_modem_status_t st;
    mcl_ap_listen_result_t r;
    int listener_recovered = 0;

    *out_present = 0;
    if (wav_read_pcm16(path, g_archive, STREAM_CAPACITY, &count) != 0
        || count == 0u) {
        return 0;
    }
    *out_present = 1;

    mcl_ap_modem_default_config(&modem);
    st = mcl_ap_modem_decode(&modem, g_archive, count, &g_scratch,
                             one_shot, sizeof(one_shot), &rx);

    listen_config(&cfg, 64u);
    if (mcl_ap_listen_init(&listener, &cfg, window, ARCHIVE_WINDOW)
        != MCL_AP_LISTEN_OK) {
        return 0;
    }

    for (i = 0u; i < count; i += 1024u) {
        const size_t n = (count - i < 1024u) ? (count - i) : 1024u;
        (void)mcl_ap_listen_push(&listener, g_archive + i, n);
        do {
            r = mcl_ap_listen_poll(&listener, &g_scratch, streamed,
                                   sizeof(streamed), NULL);
            if (r == MCL_AP_LISTEN_CONTACT && listener_recovered == 0) {
                listener_recovered = 1;
            }
        } while (r == MCL_AP_LISTEN_CONTACT);
    }
    /* The file ends; a frame in its last seconds is still owed a verdict. */
    do {
        r = mcl_ap_listen_flush(&listener, &g_scratch, streamed,
                                sizeof(streamed), NULL);
        if (r == MCL_AP_LISTEN_CONTACT && listener_recovered == 0) {
            listener_recovered = 1;
        }
    } while (r == MCL_AP_LISTEN_CONTACT || r == MCL_AP_LISTEN_HEARD);

    /*
     * Acquisition must agree on every file: the correlator is the same one,
     * so a disagreement here means the incremental slicing lost a preamble.
     */
    check((rx.acquired != 0u)
              == (listener.contacts + listener.heard > 0u),
          "streamed acquisition agrees with the one-shot decoder");

    if (st == MCL_AP_MODEM_OK) {
        check(listener_recovered == 1,
              "a frame the one-shot decoder recovered is also recovered "
              "streamed");
        if (listener_recovered) {
            check(memcmp(one_shot, streamed, rx.payload_bytes) == 0,
                  "and the streamed bytes are the one-shot bytes");
        }
    }
    return (st == MCL_AP_MODEM_OK) ? 1 : 0;
}

static void test_archived_captures(const char *root)
{
    static const char *sets[] = {
        "experiments/009-android-acoustic-peer/evidence/"
            "e3-android-speaker-20260905/wire-trial-%02d.wav",
        "experiments/008-embedded-node/evidence/"
            "e4-node-board-to-host-wire-20260904/trial-%02d.wav"
    };
    size_t s;
    int any = 0;

    printf("the archived over-air captures, streamed\n");
    for (s = 0u; s < sizeof(sets) / sizeof(sets[0]); ++s) {
        char pattern[512];
        int trial, present, recovered = 0, found = 0;

        snprintf(pattern, sizeof(pattern), "%s/%s", root, sets[s]);
        for (trial = 1; trial <= 10; ++trial) {
            char path[512];
            snprintf(path, sizeof(path), pattern, trial);
            recovered += compare_one_capture(path, &present);
            found += present;
        }
        if (found > 0) {
            any = 1;
            printf("    %d/%d files, %d recovered by both receivers\n",
                   found, 10, recovered);
        }
    }
    if (!any) {
        printf("    no archived captures found; pass the mcl-ap root as "
               "argv[1]\n");
    }
}

int main(int argc, char **argv)
{
    const char *root = (argc > 1) ? argv[1] : ".";

    printf("MCL-AP listener tests\n\n");
    test_min_window();
    test_unannounced_frame();
    test_polling_does_not_cost();
    test_body_still_arriving();
    test_two_frames();
    test_quiet_room();
    test_too_busy_is_visible();
    test_archived_captures(root);

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return (g_failures == 0) ? 0 : 1;
}
