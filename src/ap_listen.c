/*
 * MCL-AP continuous listener. See include/mcl/ap_listen.h for what this is
 * for and, more importantly, for what it deliberately is not.
 *
 * The whole module is two ideas:
 *
 *   1. The window is linear and slides. It is not a circular buffer, and
 *      that is a decision rather than an oversight: `mcl_ap_modem_decode`
 *      takes a contiguous `const int16_t *`, so a circular buffer would need
 *      either a linearizing copy of the whole window on every poll -- which
 *      costs more than the correlation it feeds -- or a second copy of the
 *      demodulator that understands wrapping. Sliding compacts once per
 *      window-fill instead of once per poll, and there is exactly one
 *      demodulator.
 *
 *   2. Acquisition is incremental. `scan_pos` is the stream position the next
 *      search starts from, and it only ever moves forward. Each poll hands
 *      the modem a slice beginning at `scan_pos`, so the correlator sweeps
 *      each sample once in the listener's whole lifetime, no matter how often
 *      it is polled or how large the window is.
 *
 * Everything else follows from keeping those two invariants true when the
 * audio runs out mid-frame.
 */

#include "mcl/ap_listen.h"

#include <string.h>

/* Samples the modem places before the preamble and after the last symbol.
   They are part of an encoded frame but not of the span that starts at an
   acquisition, so they have to come back off. */
static size_t silence_samples(float seconds)
{
    if (seconds <= 0.0f) {
        return 0u;
    }
    return (size_t)(seconds * (float)MCL_AP_MODEM_SAMPLE_RATE_HZ + 0.5f);
}

/*
 * Samples from the first sample of the preamble to the last sample of the
 * payload, for a frame carrying `payload_bytes`. This is the distance a
 * listener must advance past a decoded frame, and the amount of audio that
 * must exist after an acquisition before a failure can be called a failure
 * rather than a truncation.
 */
static size_t frame_span(const mcl_ap_modem_config_t *config,
                         size_t payload_bytes)
{
    const size_t encoded = mcl_ap_modem_encoded_samples(config, payload_bytes);
    const size_t lead = silence_samples(config->leading_silence_s);
    const size_t trail = silence_samples(config->trailing_silence_s);

    if (encoded == 0u || encoded <= lead + trail) {
        return 0u;
    }
    return encoded - lead - trail;
}

static size_t preamble_samples(const mcl_ap_modem_config_t *config)
{
    size_t n = silence_samples(config->preamble_duration_s);

    if (n > MCL_AP_MODEM_MAX_PREAMBLE_SAMPLES) {
        n = MCL_AP_MODEM_MAX_PREAMBLE_SAMPLES;
    }
    return n;
}

void mcl_ap_listen_default_config(mcl_ap_listen_config_t *config)
{
    if (config == NULL) {
        return;
    }
    mcl_ap_modem_default_config(&config->modem);
    config->max_payload_bytes = (uint8_t)MCL_AP_MODEM_MAX_PAYLOAD_BYTES;
}

size_t mcl_ap_listen_min_window_samples(const mcl_ap_listen_config_t *config)
{
    size_t span, ref;

    if (config == NULL || config->max_payload_bytes == 0u) {
        return 0u;
    }
    span = frame_span(&config->modem, (size_t)config->max_payload_bytes);
    ref = preamble_samples(&config->modem);
    if (span == 0u || ref == 0u) {
        return 0u;
    }
    /*
     * One whole frame, plus one preamble length of room before it. The extra
     * preamble is what lets a frame that begins at the very start of the
     * window still be acquired: the search region is the slice minus one
     * reference length, so a window sized to the frame alone can only ever
     * find a preamble at offset zero.
     */
    return span + ref;
}

mcl_ap_listen_status_t mcl_ap_listen_init(mcl_ap_listener_t *listener,
                                          const mcl_ap_listen_config_t *config,
                                          int16_t *window,
                                          size_t capacity)
{
    size_t minimum;

    if (listener == NULL || config == NULL || window == NULL) {
        return MCL_AP_LISTEN_ERR_INVALID_ARGUMENT;
    }
    if (config->max_payload_bytes == 0u ||
        config->max_payload_bytes > MCL_AP_MODEM_MAX_PAYLOAD_BYTES) {
        return MCL_AP_LISTEN_ERR_INVALID_ARGUMENT;
    }
    minimum = mcl_ap_listen_min_window_samples(config);
    if (minimum == 0u) {
        return MCL_AP_LISTEN_ERR_INVALID_ARGUMENT;
    }
    if (capacity < minimum) {
        return MCL_AP_LISTEN_ERR_WINDOW_TOO_SMALL;
    }

    memset(listener, 0, sizeof(*listener));
    listener->config = *config;
    listener->window = window;
    listener->capacity = capacity;
    listener->ref_len = preamble_samples(&config->modem);
    listener->frame_span_max =
        frame_span(&config->modem, (size_t)config->max_payload_bytes);
    return MCL_AP_LISTEN_OK;
}

/*
 * Make room for `needed` more samples by discarding the oldest.
 *
 * The rule that matters: audio ahead of `scan_pos` has never been correlated,
 * so discarding it destroys a call that may be sitting in it. The function
 * will always discard scanned audio first and will only cut into unscanned
 * audio when the caller has pushed more than a whole window without polling.
 * When it has to, it says so -- `samples_unscanned` and `overruns` are the
 * only honest account of a machine that was too busy to listen.
 */
static void make_room(mcl_ap_listener_t *l, size_t needed)
{
    size_t free_space = l->capacity - l->filled;
    size_t discard, scanned;

    if (needed <= free_space) {
        return;
    }
    discard = needed - free_space;

    scanned = (size_t)(l->scan_pos - l->window_start);
    if (scanned > l->filled) {
        scanned = l->filled;
    }
    if (discard > scanned) {
        l->samples_unscanned += (uint64_t)(discard - scanned);
        l->overruns += 1u;
    }
    if (discard > l->filled) {
        discard = l->filled;
    }

    memmove(l->window, l->window + discard,
            (l->filled - discard) * sizeof(int16_t));
    l->filled -= discard;
    l->window_start += (uint64_t)discard;
    if (l->scan_pos < l->window_start) {
        l->scan_pos = l->window_start;
    }
    /* A held preamble that has just been overwritten is gone. Saying so is
       better than re-demodulating whatever audio now sits at its address. */
    if (l->pending != 0u && l->pending_at < l->window_start) {
        l->pending = 0u;
    }
}

mcl_ap_listen_status_t mcl_ap_listen_push(mcl_ap_listener_t *listener,
                                          const int16_t *pcm,
                                          size_t count)
{
    if (listener == NULL || listener->window == NULL ||
        (pcm == NULL && count > 0u)) {
        return MCL_AP_LISTEN_ERR_INVALID_ARGUMENT;
    }
    if (count == 0u) {
        return MCL_AP_LISTEN_OK;
    }

    listener->total_pushed += (uint64_t)count;

    /*
     * A block bigger than the whole window keeps only its newest tail. Every
     * sample it displaces is unscanned by definition, including the ones from
     * this very block that are dropped without ever being stored.
     */
    if (count >= listener->capacity) {
        const size_t drop = count - listener->capacity;
        size_t scanned = (size_t)(listener->scan_pos - listener->window_start);

        if (scanned > listener->filled) {
            scanned = listener->filled;
        }
        listener->samples_unscanned +=
            (uint64_t)(listener->filled - scanned) + (uint64_t)drop;
        listener->overruns += 1u;

        memcpy(listener->window, pcm + drop,
               listener->capacity * sizeof(int16_t));
        listener->window_start += (uint64_t)(listener->filled + drop);
        listener->filled = listener->capacity;
        listener->scan_pos = listener->window_start;
        listener->pending = 0u;
        return MCL_AP_LISTEN_OK;
    }

    make_room(listener, count);
    memcpy(listener->window + listener->filled, pcm,
           count * sizeof(int16_t));
    listener->filled += count;
    return MCL_AP_LISTEN_OK;
}

static mcl_ap_listen_result_t poll_impl(mcl_ap_listener_t *listener,
                                        mcl_ap_modem_scratch_t *scratch,
                                        uint8_t *out_payload,
                                        size_t out_capacity,
                                        mcl_ap_listen_event_t *event,
                                        uint8_t final)
{
    mcl_ap_modem_config_t config;
    mcl_ap_modem_rx_t rx;
    mcl_ap_modem_status_t status;
    uint64_t slice_start, stream_end, acquired_at;
    size_t offset, available;
    uint8_t was_pending;

    if (listener == NULL || listener->window == NULL || scratch == NULL ||
        out_payload == NULL || out_capacity == 0u) {
        return MCL_AP_LISTEN_QUIET;
    }
    if (event != NULL) {
        memset(event, 0, sizeof(*event));
    }

    config = listener->config.modem;
    stream_end = listener->window_start + (uint64_t)listener->filled;
    was_pending = listener->pending;

    /*
     * A held preamble is re-judged from exactly where it was found: the slice
     * begins at it and the search is pinned to a single start position, so no
     * correlation sweep happens at all while waiting for a body to arrive.
     * Otherwise the slice begins at scan_pos and runs to the end of the
     * buffered audio, and its length is what bounds the search -- `acquire`
     * looks over the slice minus one reference length -- so no
     * `max_search_samples` is set. Setting one there would cap how far into
     * NEW audio the search reaches, which is the opposite of what a listener
     * wants.
     */
    if (listener->pending != 0u) {
        slice_start = listener->pending_at;
        config.max_search_samples =
            2u * (uint32_t)MCL_AP_MODEM_COARSE_OFFSET_STEP + 1u;
        config.refinement_guard_samples = 0u;
    } else {
        slice_start = listener->scan_pos;
        if (final == 0u && listener->frame_span_max > listener->ref_len) {
            config.refinement_guard_samples =
                (uint32_t)(listener->frame_span_max - listener->ref_len
                           + MCL_AP_MODEM_COARSE_OFFSET_STEP);
        }
    }

    if (slice_start >= stream_end || slice_start < listener->window_start) {
        return MCL_AP_LISTEN_QUIET;
    }
    offset = (size_t)(slice_start - listener->window_start);
    available = listener->filled - offset;

    /* A sparse candidate is held one grid step before its approximate start.
       Do not invoke the expensive fine/timing path until a maximum frame can
       be wholly present. Capture remains live while these samples arrive. */
    if (listener->pending != 0u && final == 0u &&
        available < listener->frame_span_max
                        + 2u * MCL_AP_MODEM_COARSE_OFFSET_STEP) {
        if (event != NULL) {
            event->stream_index = listener->pending_at;
            event->modem_status = MCL_AP_MODEM_ERR_INCOMPLETE;
        }
        return MCL_AP_LISTEN_WAITING;
    }

    /* Below one reference length the correlator has nothing to slide. */
    if (available <= listener->ref_len) {
        return MCL_AP_LISTEN_QUIET;
    }

    listener->samples_searched +=
        (listener->pending != 0u)
            ? (uint64_t)(2u * MCL_AP_MODEM_COARSE_OFFSET_STEP + 1u)
            : (uint64_t)(available - listener->ref_len);

    status = mcl_ap_modem_decode(&config,
                                 listener->window + offset, available,
                                 scratch, out_payload, out_capacity, &rx);

    acquired_at = slice_start + (uint64_t)rx.acquisition_index;
    if (event != NULL) {
        event->stream_index = acquired_at;
        event->rx = rx;
        event->modem_status = status;
    }

    if (rx.refinement_deferred != 0u) {
        const uint64_t approximate = acquired_at;
        const uint64_t step = (uint64_t)MCL_AP_MODEM_COARSE_OFFSET_STEP;
        listener->pending_at = (approximate > slice_start + step)
                                   ? approximate - step
                                   : slice_start;
        listener->scan_pos = listener->pending_at;
        listener->pending = 1u;
        return MCL_AP_LISTEN_WAITING;
    }

    if (rx.acquired == 0u) {
        /*
         * Nothing correlated. Everything up to the last position the search
         * could have started from has now been examined; leave the final
         * reference length unscanned, because a preamble beginning there was
         * never searched and will be on the next poll.
         *
         * Reaching this while pending means the held preamble stopped
         * correlating, which the same samples cannot do. Release it rather
         * than hold a position that will never resolve.
         */
        listener->pending = 0u;
        if (was_pending != 0u) {
            /* The sparse pass may see a partial-preamble sidelobe before the
               actual chirp has fully arrived.  Rejecting that held candidate
               proves only that start position false; it does not prove the
               rest of the buffered audio quiet.  Resume one coarse grid step
               later so the real preamble remains searchable. */
            listener->scan_pos =
                slice_start + (uint64_t)MCL_AP_MODEM_COARSE_OFFSET_STEP;
            return MCL_AP_LISTEN_QUIET;
        }
        listener->scan_pos = stream_end - (uint64_t)listener->ref_len;
        if (listener->scan_pos < slice_start) {
            listener->scan_pos = slice_start;
        }
        return MCL_AP_LISTEN_QUIET;
    }

    if (status == MCL_AP_MODEM_OK) {
        const size_t span = frame_span(&listener->config.modem,
                                       rx.payload_bytes);

        listener->contacts += 1u;
        listener->pending = 0u;
        listener->scan_pos =
            acquired_at + (uint64_t)((span > listener->ref_len)
                                     ? span : listener->ref_len);
        if (listener->scan_pos > stream_end) {
            listener->scan_pos = stream_end;
        }
        if (event != NULL) {
            event->payload_bytes = rx.payload_bytes;
        }
        return MCL_AP_LISTEN_CONTACT;
    }

    /*
     * Acquired and not recovered. That is either a real channel failure or a
     * frame whose tail has not arrived yet, and those must not be confused:
     * calling a truncation a failure loses the frame permanently, because
     * scan_pos would step past a preamble whose payload was still in flight.
     *
     * The test is whether a maximum-length frame starting here would already
     * be complete. If it would, the audio was all present and the failure is
     * the channel's. If it would not, hold position and wait.
     */
    if (final == 0u &&
        acquired_at + (uint64_t)listener->frame_span_max > stream_end) {
        listener->scan_pos = acquired_at;
        listener->pending_at = acquired_at;
        listener->pending = 1u;
        return MCL_AP_LISTEN_WAITING;
    }

    listener->heard += 1u;
    listener->pending = 0u;
    /* Step past this preamble only -- far enough to guarantee progress, near
       enough that a second frame overlapping its tail is still found. */
    listener->scan_pos = acquired_at + (uint64_t)listener->ref_len;
    return MCL_AP_LISTEN_HEARD;
}

mcl_ap_listen_result_t mcl_ap_listen_poll(mcl_ap_listener_t *listener,
                                          mcl_ap_modem_scratch_t *scratch,
                                          uint8_t *out_payload,
                                          size_t out_capacity,
                                          mcl_ap_listen_event_t *event)
{
    return poll_impl(listener, scratch, out_payload, out_capacity, event, 0u);
}

mcl_ap_listen_result_t mcl_ap_listen_flush(mcl_ap_listener_t *listener,
                                           mcl_ap_modem_scratch_t *scratch,
                                           uint8_t *out_payload,
                                           size_t out_capacity,
                                           mcl_ap_listen_event_t *event)
{
    return poll_impl(listener, scratch, out_payload, out_capacity, event, 1u);
}
