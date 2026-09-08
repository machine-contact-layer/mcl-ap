/*
 * MCL-AP continuous listener: hearing a call you were not waiting for.
 *
 * STATUS: EXPERIMENTAL, like everything else in this repository. It changes
 * nothing on the wire -- it is the same `ap_modem.c` receiver, driven
 * differently.
 *
 * THE PROBLEM
 *
 * Every acoustic result in this repository so far was a *scheduled* decode: a
 * recording was started, a frame was transmitted into it, the recording was
 * stopped and handed to `mcl_ap_modem_decode` whole. Both peers knew when the
 * exchange would happen.
 *
 * A machine doing its actual job does not know that. It is welding, or
 * carrying a pallet, or sitting in a charging dock, and a stranger walks up
 * and tries to open contact. Nobody arranged a window. If the receiver is
 * only listening during a window it chose, the call is simply not heard, and
 * "not heard" is indistinguishable from "not present" to everyone involved.
 *
 * This module is the difference between a receiver that can be *told* to
 * listen and one that is always listening.
 *
 * WHAT IT ACTUALLY IS, AND WHAT IT IS NOT
 *
 * The obvious framing is a rolling buffer of the last N seconds, which the
 * machine searches when it gets a moment. That framing is misleading in an
 * important way, and the difference decides the whole design:
 *
 *   - It is NOT memory of the past. Acquisition is incremental. Every sample
 *     is correlated against the preamble reference exactly once, when it
 *     first becomes searchable -- not once per poll. Re-scanning a 30-second
 *     window on every poll would be about 1.5 billion multiply-accumulates
 *     per pass, which no embedded target and no busy host is going to spend
 *     ten times a second. Incremental scanning makes the cost proportional to
 *     elapsed audio, so a listener costs the same whether its window holds
 *     two seconds or sixty.
 *
 *   - It IS scheduling slack. The window is how late the machine is allowed
 *     to be. A robot that finishes its motion and polls eight seconds after a
 *     stranger transmitted still has those samples, and still finds the
 *     frame, because they have not been overwritten yet. Size the window by
 *     the longest stretch the application can go without polling, not by how
 *     far back it wants to remember.
 *
 * WHAT IT COSTS, AND WHICH PARTS CAN AFFORD IT
 *
 * Proportional to elapsed audio is the shape of the cost. The constant decides
 * whether a part can listen continuously at all, so here it is, measured
 * rather than estimated.
 *
 * Acquisition is a coarse correlation at MCL_AP_MODEM_DECIMATION stride over
 * every new sample position, so with the default 0.2 s chirp the steady-state
 * work is (9600 / 3) complex multiply-accumulates per sample of audio -- about
 * 154 million per second of real time at 48 kHz. That is the number to compare
 * a candidate part against.
 *
 * On a DFR1154 (ESP32-S3, 240 MHz, single-precision FPU, no double in
 * hardware) the MCL autonomous node measured 0.0888 ms per searched sample
 * position, or about 11 000 samples per second. Real time needs 48 000. So
 * this part is roughly four times short of continuous listening and cannot do
 * it, however the application is arranged -- the gap is arithmetic, not
 * scheduling.
 *
 * That is not the end of acoustic bootstrap on such a part. It is the reason
 * AP-BOOTSTRAP-1 repeats: a receiver that captures a bounded window and then
 * decodes it, as Experiment 008 does at E4, is deaf while it decodes, and
 * repetition is what makes a duty-cycled receiver reachable. Choose that
 * shape when the arithmetic above does not fit; choose continuous listening
 * when it does.
 *
 * Two costs that are NOT proportional to audio were removed in the course of
 * that measurement, and a caller reusing one scratch across polls gets the
 * benefit automatically: the quadrature reference and its statistics are pure
 * functions of the configuration, and are now built once per scratch rather
 * than once per decode. See the cache note on mcl_ap_modem_scratch_t. Together
 * they were 4x on this part -- a one-shot decoder never noticed them, because
 * it pays them once per capture.
 *
 * There is no anomaly detector here, and there should not be. The preamble
 * correlator IS the detector: it is a matched filter for exactly the thing
 * being looked for, it already runs at 10/10 on real over-air captures in
 * Experiments 008 and 009, and it produces a normalized score with a
 * threshold rather than a probability that needs interpreting. A learned
 * detector on top of it would have to be trained, shipped, versioned and
 * explained, and would answer a vaguer question than the one already
 * answered exactly. Where a learned model would earn its place is in
 * deciding when it is *worth* transmitting -- and that is a transmitter-side
 * question about a channel, not a receiver-side question about a frame.
 *
 * MEMORY
 *
 * Freestanding C99, no allocation, no libc beyond <string.h>. The window is
 * caller-owned int16 PCM, so the application chooses the slack it can afford
 * and can see the cost in its own linker map. The receive working set is the
 * same `mcl_ap_modem_scratch_t` the one-shot decoder uses, and the caller may
 * share one between polls or between listeners -- it holds no state across
 * calls.
 *
 * WHAT A LISTENER DOES NOT DO
 *
 * It does not decide that a frame it recovered came from anyone in
 * particular, that the sender is who the payload says, or that the machine
 * owes the sender a reply. Hearing a call is reception. MCL's constitutional
 * rule stands unchanged and is enforced above this layer:
 *
 *   reception != identity != authenticity != authority != trust != obligation
 */

#ifndef MCL_AP_LISTEN_H
#define MCL_AP_LISTEN_H

#include <stddef.h>
#include <stdint.h>

#include "mcl/ap_modem.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------- status */

typedef int32_t mcl_ap_listen_status_t;
enum {
    MCL_AP_LISTEN_OK = 0,
    MCL_AP_LISTEN_ERR_INVALID_ARGUMENT = 1,
    MCL_AP_LISTEN_ERR_WINDOW_TOO_SMALL = 2
};

/*
 * What one poll found. These are four different situations and a caller that
 * collapses them loses the ability to tell a quiet room from a broken one:
 *
 *   QUIET    nothing correlated above the threshold in the audio searched.
 *   CONTACT  a frame was recovered and its CRC verified. `out_payload` holds
 *            it. This is the only result that carries data.
 *   HEARD    a preamble was found and the payload did not survive. Somebody
 *            transmitted; the channel was not good enough. This is the state
 *            Experiment 009 sat in 6 times out of 10, and reporting it as
 *            QUIET would have hidden the entire finding.
 *   WAITING  a preamble was found close enough to the end of the buffered
 *            audio that the rest of the frame may still be in flight. Push
 *            more samples and poll again; the position is remembered.
 */
typedef int32_t mcl_ap_listen_result_t;
enum {
    MCL_AP_LISTEN_QUIET = 0,
    MCL_AP_LISTEN_CONTACT = 1,
    MCL_AP_LISTEN_HEARD = 2,
    MCL_AP_LISTEN_WAITING = 3
};

/* ---------------------------------------------------------------- event */

/*
 * Where a poll's result came from. `stream_index` is an absolute sample
 * position counted from the first sample ever pushed, not an offset into the
 * window, because the window slides and an offset into it means nothing five
 * seconds later. At 48 kHz a uint64 runs for twelve million years.
 */
typedef struct {
    uint64_t              stream_index;   /* where the preamble was found */
    mcl_ap_modem_rx_t     rx;             /* the modem's own report */
    mcl_ap_modem_status_t modem_status;   /* why it failed, if it did */
    size_t                payload_bytes;  /* bytes written, CONTACT only */
} mcl_ap_listen_event_t;

/* --------------------------------------------------------------- config */

typedef struct {
    /* The waveform being listened for. Defaults to the Experiment 003
       candidate, which is what every capture in this repository carries. */
    mcl_ap_modem_config_t modem;

    /*
     * The largest payload the listener will wait for a body to arrive for.
     * It sets how close to the end of the buffer a preamble may be found
     * before the listener reports WAITING instead of a verdict, and so it
     * sets both the minimum window and the worst-case detection latency.
     * Declaring 64 when the application only ever sends 10-byte objects
     * costs about 1.4 s of extra patience on every failed frame.
     */
    uint8_t max_payload_bytes;
} mcl_ap_listen_config_t;

/* The Experiment 003 candidate waveform, 64-byte maximum payload. */
void mcl_ap_listen_default_config(mcl_ap_listen_config_t *config);

/*
 * Smallest window that can hold one complete maximum-length frame plus the
 * preamble reference. A window this size works but leaves no slack at all --
 * the caller must poll after every push. Real deployments want multiples of
 * it; the window is scheduling slack and one frame is not slack.
 *
 * Zero if the config is unusable.
 */
size_t mcl_ap_listen_min_window_samples(const mcl_ap_listen_config_t *config);

/* -------------------------------------------------------------- listener */

/*
 * All fields are owned by the listener and are readable but must not be
 * written after `mcl_ap_listen_init`. They are exposed rather than hidden
 * because an operator debugging a machine that "never hears anything" needs
 * to see `samples_unscanned` without a debugger.
 */
typedef struct {
    mcl_ap_listen_config_t config;

    int16_t *window;          /* caller-owned PCM16 at 48 kHz, mono */
    size_t   capacity;        /* samples the window can hold */
    size_t   filled;          /* samples currently in it */

    uint64_t window_start;    /* stream index of window[0] */
    uint64_t scan_pos;        /* stream index the next search begins at */
    uint64_t total_pushed;    /* every sample ever offered */

    /*
     * Audio that was overwritten before it was ever correlated, because the
     * caller pushed more than the window could hold without polling. This is
     * the number that says a machine is too busy to hear, and it is counted
     * rather than silently absorbed for exactly that reason: a listener that
     * quietly drops audio is indistinguishable from a quiet room.
     */
    uint64_t samples_unscanned;
    uint32_t overruns;        /* pushes that had to discard unscanned audio */

    /*
     * Correlator start positions evaluated over this listener's lifetime.
     * This is the number the "incremental, not retrospective" claim in the
     * file header stands or falls on: if the listener were re-searching its
     * window on every poll it would climb as the product of window length and
     * poll rate. It should stay near `total_pushed` however often poll is
     * called, and `tests/test_ap_listen.c` asserts exactly that.
     */
    uint64_t samples_searched;

    uint32_t contacts;        /* frames recovered, CRC verified */
    uint32_t heard;           /* preambles found whose payload did not survive */

    /*
     * A preamble found too near the end of the buffered audio to judge. The
     * position is kept so the next poll re-demodulates from it directly
     * instead of searching for it again -- without this, a frame whose body
     * takes two seconds to arrive would be re-acquired on every poll in
     * between, and the cost of waiting would grow as the square of the wait.
     */
    uint64_t pending_at;
    uint8_t  pending;

    size_t   ref_len;         /* preamble samples */
    size_t   frame_span_max;  /* preamble start to frame end, max payload */
} mcl_ap_listener_t;

/*
 * Bind a window to a listener. `window` must hold at least
 * `mcl_ap_listen_min_window_samples(config)` samples and stays owned by the
 * caller for the listener's lifetime.
 */
mcl_ap_listen_status_t mcl_ap_listen_init(mcl_ap_listener_t *listener,
                                          const mcl_ap_listen_config_t *config,
                                          int16_t *window,
                                          size_t capacity);

/*
 * Offer captured audio. Blocks may be any size, including larger than the
 * window; the newest samples always survive. Oldest audio is discarded to
 * make room, and audio discarded before it was searched is counted in
 * `samples_unscanned` rather than forgotten.
 *
 * This is the only call on the capture path, and it does no correlation, so
 * it is safe from an I2S or audio-callback context.
 */
mcl_ap_listen_status_t mcl_ap_listen_push(mcl_ap_listener_t *listener,
                                          const int16_t *pcm,
                                          size_t count);

/*
 * Search whatever audio has arrived since the last poll and report one
 * result. Call it whenever the machine has a moment; there is no required
 * cadence, only the consequence of being late, which is `samples_unscanned`.
 *
 * A single poll reports at most one event, so a caller that wants to drain a
 * burst of several frames polls until it gets QUIET. `event` may be NULL if
 * the caller only wants the result code; on CONTACT the payload is written to
 * `out_payload` and its length is in `event->payload_bytes` (and in
 * `event->rx.payload_bytes`, which is filled in on failures too).
 *
 * `scratch` holds no state between calls and may be shared.
 */
mcl_ap_listen_result_t mcl_ap_listen_poll(mcl_ap_listener_t *listener,
                                          mcl_ap_modem_scratch_t *scratch,
                                          uint8_t *out_payload,
                                          size_t out_capacity,
                                          mcl_ap_listen_event_t *event);

/*
 * Judge a held preamble on the audio that already exists, instead of waiting
 * for a body that is not coming.
 *
 * A real listener never needs this: a microphone does not end, so WAITING
 * always resolves. A finite stream does end -- a WAV file, a capture that was
 * stopped, a recording handed over after the fact -- and a frame sitting in
 * its last two seconds would otherwise stay WAITING forever. Call this once
 * after the last push, then keep calling it until it returns QUIET.
 *
 * It cannot invent samples. A frame that really was cut off mid-payload comes
 * back HEARD, which is the truth about it.
 */
mcl_ap_listen_result_t mcl_ap_listen_flush(mcl_ap_listener_t *listener,
                                           mcl_ap_modem_scratch_t *scratch,
                                           uint8_t *out_payload,
                                           size_t out_capacity,
                                           mcl_ap_listen_event_t *event);

#ifdef __cplusplus
}
#endif

#endif /* MCL_AP_LISTEN_H */
