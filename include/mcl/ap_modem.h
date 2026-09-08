/*
 * MCL-AP candidate modem: a portable acoustic transmit/receive path.
 *
 * STATUS: EXPERIMENTAL. This is the Experiment 003 candidate waveform made
 * reusable. It is NOT AP-B0, NOT a selected profile, and nothing here freezes
 * spectrum. `mcl-ap/experiments/003-band-informed-candidate/README.md` states
 * why: the 3000/6000 Hz pair was chosen from a measurement of one path in one
 * room, and MCL-AP is a path-convergence system in which profiles are selected
 * from measurement rather than assumed.
 *
 * WHY THIS EXISTS SEPARATELY FROM exp001.c
 *
 * The experiment code is an instrument and is frozen: it holds the E3 and E4
 * results and its value depends on not moving. It is also unrunnable on a
 * microcontroller. `exp001_detect_preamble_iq` alone puts two 96000-element
 * float arrays on the stack -- 768 KB -- and correlates in double precision
 * across the whole capture. An ESP32-S3 has 512 KB of SRAM and no
 * double-precision FPU.
 *
 * So this module is the same waveform written to run in both places:
 *
 *   - every buffer is caller-owned; there is no malloc and no large stack
 *     array, so the working set is visible in the type system
 *   - the sample path is float, which the target has hardware for
 *   - acquisition searches at a stride first and refines at full rate, which
 *     is what makes it finish in a fraction of a second instead of tens of
 *     seconds, and which needs no decimated copy of the signal
 *   - samples are int16 at the boundaries, because that is what an I2S
 *     peripheral and a WAV file both actually carry
 *
 * The host tools and the firmware link THE SAME source. That is deliberate:
 * it makes "the board and the laptop agree" a property of one implementation
 * compiled twice rather than a claim about two. What it demonstrates is
 * portability and end-to-end operation, NOT independent implementation --
 * `mcl-core/governance/V1_SCOPE.md` §5.9 draws that line and this file does
 * not blur it.
 *
 * Frame layout, identical to Experiment 001/003:
 *
 *   [leading silence] [LFM preamble, energy-equalized] [FSK training]
 *   [PHY header: len(1) crc16(2)] [FSK payload] [trailing silence]
 */

#ifndef MCL_AP_MODEM_H
#define MCL_AP_MODEM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------- limits */

#define MCL_AP_MODEM_SAMPLE_RATE_HZ     48000u
#define MCL_AP_MODEM_BAUD               300u
#define MCL_AP_MODEM_TRAINING_BITS      16u
#define MCL_AP_MODEM_TRAINING_BYTE      0x55u
#define MCL_AP_MODEM_HEADER_BYTES       3u
#define MCL_AP_MODEM_MAX_PAYLOAD_BYTES  64u

/* 0.2 s of preamble at 48 kHz. The reference is held twice, I and Q, so the
   detector is insensitive to carrier phase and to polarity inversion -- a
   microphone that inverts the signal must still acquire. */
#define MCL_AP_MODEM_MAX_PREAMBLE_SAMPLES 9600u

/*
 * Acquisition decimation. 3 keeps the Nyquist limit at 8 kHz, above the
 * 6 kHz top of the chirp, so the coarse search sees the whole preamble
 * rather than an aliased version of it. 4 would put Nyquist exactly at
 * 6 kHz, which is the wrong side of a boundary to sit on.
 */
#define MCL_AP_MODEM_DECIMATION 3u

/* A bound a constrained receiver can put on acquisition work: 2.0 s at
   48 kHz. It is NOT a default -- see `max_search_samples` in the config. */
#define MCL_AP_MODEM_SEARCH_BOUND_SAMPLES 96000u

/* ---------------------------------------------------------------- status */

typedef int32_t mcl_ap_modem_status_t;
enum {
    MCL_AP_MODEM_OK = 0,
    MCL_AP_MODEM_ERR_INVALID_ARGUMENT = 1,
    MCL_AP_MODEM_ERR_BUFFER_TOO_SMALL = 2,
    MCL_AP_MODEM_ERR_NOT_ACQUIRED = 3,
    MCL_AP_MODEM_ERR_SYNC = 4,
    MCL_AP_MODEM_ERR_CRC = 5,
    MCL_AP_MODEM_ERR_PAYLOAD = 6
};

/* --------------------------------------------------------------- config */

typedef struct {
    float fsk_freq_0_hz;        /* bit 0 */
    float fsk_freq_1_hz;        /* bit 1 */
    float preamble_f_start_hz;
    float preamble_f_end_hz;
    float preamble_duration_s;
    float leading_silence_s;
    float trailing_silence_s;
    float detection_threshold;  /* normalized correlation, 0..1 */
    uint16_t training_bits;
    /*
     * How far into the buffer acquisition searches. Zero means the whole
     * buffer, which is the default and the only correct general answer: a
     * host recording has arbitrary lead-in, and a receiver that stops looking
     * after a fixed prefix reports "not acquired" for a frame that is plainly
     * present later in the capture.
     *
     * This was found by measurement, not by reasoning. A bound of 1.0 s
     * decoded 9/10 of the board-recorded captures -- which begin after a
     * fixed 500 ms guard -- and 0/10 of the laptop recordings of the same
     * transmissions, where the frame lands after a second or more of ffmpeg
     * device start-up. The frozen instrument searches the whole capture, and
     * a receiver that does less is a different receiver.
     *
     * A constrained target that must bound the work sets it explicitly, and
     * MCL_AP_MODEM_SEARCH_BOUND_SAMPLES exists for that. Bounding it is a
     * decision about a specific rig, so it is made where the rig is known.
     */
    uint32_t max_search_samples;
} mcl_ap_modem_config_t;

/*
 * The Experiment 003 candidate: FSK 3000/6000 Hz, 300 baud, 0.2 s LFM chirp
 * from 2 to 6 kHz, 16 training bits, acquisition threshold 0.40.
 *
 * These are the parameters the retained E3/E4 evidence was measured with. A
 * caller that changes them is measuring something else, which is allowed and
 * is the point of the struct -- but it is no longer comparable to that
 * evidence.
 */
void mcl_ap_modem_default_config(mcl_ap_modem_config_t *config);

/* ------------------------------------------------------------- scratch */

/*
 * Receive working set, caller-owned: two 9600-element float arrays for the
 * quadrature reference, plus the demodulated bytes. About 77 KB, and the
 * caller can see it in the type rather than discovering it in a malloc
 * inside the decoder.
 *
 * There is no decimated copy of anything. The coarse acquisition pass reads
 * the reference and the caller's PCM at a stride, so the decimated signal
 * never exists as a buffer.
 */
typedef struct {
    float ref_i[MCL_AP_MODEM_MAX_PREAMBLE_SAMPLES];
    float ref_q[MCL_AP_MODEM_MAX_PREAMBLE_SAMPLES];
    uint8_t demod[MCL_AP_MODEM_HEADER_BYTES + MCL_AP_MODEM_MAX_PAYLOAD_BYTES];

    /*
     * THE REFERENCE CACHE. Zero this struct before its first use.
     *
     * The quadrature reference above is a pure function of three config
     * fields, and building it costs 9 600 chirp phases -- a double-precision
     * divide, a sinf and a cosf each. On a host that is invisible. On a part
     * with no double-precision FPU it was measured at about 283 ms, which a
     * one-shot decode pays once per capture and a continuous listener pays on
     * every poll: the DFR1154 spent 20 s of a 20 s run inside decode, four and
     * a half times the per-position cost the same chip showed when the same
     * audio was decoded in a single call.
     *
     * So the decoder rebuilds the reference only when one of those fields has
     * moved. These are cache bookkeeping, never protocol: no decoded result
     * depends on whether the reference was reused or regenerated, because it
     * is the same reference either way.
     *
     * `cache_magic` guards a scratch that was never initialised. A caller that
     * hands over uninitialised memory would otherwise risk stale-looking
     * fields matching by accident and a wrong reference being used, which
     * would present as a decode that quietly stops acquiring.
     */
    uint32_t cache_magic;
    uint32_t cached_ref_len;
    float    cached_f_start_hz;
    float    cached_f_end_hz;
    float    cached_duration_s;
    /*
     * The reference's own statistics, which the acquisition pass needs and
     * which are a pure function of the reference above. Two sets: the coarse
     * pass reads the reference at MCL_AP_MODEM_DECIMATION stride, the fine
     * pass reads all of it, and the mean and energy differ accordingly.
     */
    float    cached_coarse_mean_i;
    float    cached_coarse_mean_q;
    float    cached_coarse_energy;
    float    cached_fine_mean_i;
    float    cached_fine_mean_q;
    float    cached_fine_energy;
} mcl_ap_modem_scratch_t;

/* --------------------------------------------------------------- encode */

/* Exact sample count mcl_ap_modem_encode will write. Zero if the payload is
   out of range. */
size_t mcl_ap_modem_encoded_samples(const mcl_ap_modem_config_t *config,
                                    size_t payload_bytes);

/*
 * Modulate `payload` into PCM16 mono at 48 kHz.
 *
 * The output is what an I2S amplifier consumes and what a WAV file stores, so
 * the transmit path holds no float buffer at all: the preamble is generated
 * twice, once to measure its energy and once to write it scaled, which costs
 * a second pass of 9600 sine evaluations and saves 38 KB. Transmit therefore
 * needs no scratch struct.
 */
mcl_ap_modem_status_t mcl_ap_modem_encode(
    const mcl_ap_modem_config_t *config,
    const uint8_t *payload,
    size_t payload_bytes,
    int16_t *out_pcm,
    size_t out_capacity,
    size_t *out_written);

/* --------------------------------------------------------------- decode */

typedef struct {
    size_t   acquisition_index;      /* where the preamble was found */
    float    correlation;            /* normalized magnitude, 0..1 */
    float    samples_per_symbol;     /* after the timing search */
    int32_t  timing_phase;           /* samples, relative to preamble end */
    uint16_t received_crc;
    uint16_t computed_crc;
    size_t   payload_bytes;
    uint8_t  acquired;
    uint8_t  crc_valid;
} mcl_ap_modem_rx_t;

/*
 * Acquire, demodulate, and verify one frame.
 *
 * `info` is filled in as far as the decode got, even on failure: a caller
 * needs to distinguish "heard nothing" from "heard something and the CRC
 * failed", because those are different problems with the same return code
 * shape and only the second one means a link is nearly working.
 */
mcl_ap_modem_status_t mcl_ap_modem_decode(
    const mcl_ap_modem_config_t *config,
    const int16_t *pcm,
    size_t sample_count,
    mcl_ap_modem_scratch_t *scratch,
    uint8_t *out_payload,
    size_t out_capacity,
    mcl_ap_modem_rx_t *info);

/* CRC-16/CCITT-FALSE over the payload, as carried in the PHY header. */
uint16_t mcl_ap_modem_crc16(const uint8_t *data, size_t length);

#ifdef __cplusplus
}
#endif

#endif /* MCL_AP_MODEM_H */
