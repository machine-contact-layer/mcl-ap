/*
 * MCL-AP Experiment 003: band-informed FSK candidate.
 *
 * LAB / EXPERIMENTAL. This is an AP-B0 *candidate under evaluation*.
 * It is NOT a selected profile and NOT a normative part of MCL-AP.
 *
 * Experiment 001 chose 3000/5000 Hz before any real path had been measured.
 * Experiment 002 part A then measured the path and found that the 5 kHz mark
 * tone lands 19-21 dB down inside a notch, while 6 kHz sits at or above the
 * 3 kHz reference on both receivers tested. This experiment carries the same
 * frame, preamble, training sequence, baud rate and receiver as Experiment 001
 * and changes exactly one variable: the mark tone moves out of the notch.
 *
 * Holding everything else constant is the point. Any difference in recovery
 * rate is then attributable to band choice rather than to a new modem.
 *
 *   gen    <out.wav>        write the candidate source frame
 *   decode <capture.wav>    acquire anywhere in the capture and decode
 */

#include "exp001.h"
#include "mcl/wire.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

/* Chosen from Experiment 002 part A, not from intuition. */
#define EXP003_FSK_FREQ_0   3000.0   /* strong on both receivers measured */
#define EXP003_FSK_FREQ_1   6000.0   /* clear of the 4200-5000 Hz notch    */

#define MAXS 4800000u
static float g_s[MAXS];

/* The same canonical Tier-0 PRESENCE object Experiment 001 transmits. */
static const uint8_t expected_wire[] = {
    0x00u, 0x02u, 0x00u, 0x00u, 0x00u, 0x01u, 0x01u, 0x00u, 0x00u, 0x01u, 0x3Cu
};

#define LEAD 4800u   /* leading silence the source itself carries */

static void configure(exp001_frame_config_t *fc)
{
    memset(fc, 0, sizeof(*fc));
    fc->preamble_type = EXP001_PREAMBLE_LFM_CHIRP;
    fc->preamble_duration_s = 0.2;
    fc->preamble_f_start_hz = 2000.0;
    fc->preamble_f_end_hz = 6000.0;
    fc->leading_silence_s = 0.1;
    fc->silence_duration_s = 0.5;
    fc->include_training = 1u;
    fc->detection_threshold = 0.40;
    fc->fsk_freq_0_hz = EXP003_FSK_FREQ_0;
    fc->fsk_freq_1_hz = EXP003_FSK_FREQ_1;
}

static int do_gen(const char *path)
{
    mcl_wire_tier0_t obj;
    uint8_t wire_buf[MCL_WIRE_TIER0_MAX_SIZE];
    size_t wire_written = 0u, frame_samples;
    exp001_frame_config_t fc;

    memset(&obj, 0, sizeof(obj));
    obj.kind = MCL_WIRE_KIND_PRESENCE;
    obj.priority = 1u;
    obj.source_ref = 0x00000001u;
    obj.body.presence.machine_class = 1u;
    obj.body.presence.capability_tag = 0x000001u;
    obj.body.presence.ttl = 60u;

    if (mcl_wire_tier0_encode(&obj, wire_buf, sizeof(wire_buf), &wire_written)
            != MCL_WIRE_OK) {
        printf("wire encode failed\n");
        return 1;
    }
    if (wire_written != sizeof(expected_wire) ||
        memcmp(wire_buf, expected_wire, wire_written) != 0) {
        printf("wire bytes differ from the canonical E3 vector\n");
        return 1;
    }

    configure(&fc);
    frame_samples = exp001_frame_encode(&fc, wire_buf, wire_written,
                                        g_s, MAXS, NULL);
    if (frame_samples == 0u) { printf("frame encode failed\n"); return 1; }

    if (exp001_wav_write(path, g_s, frame_samples,
                         EXP001_SAMPLE_RATE, 16u, 1u) != EXP001_OK) {
        printf("wav write failed\n");
        return 1;
    }
    printf("wrote %s: %zu samples (%.3f s), FSK %.0f/%.0f Hz\n",
           path, frame_samples, (double)frame_samples / (double)EXP001_SAMPLE_RATE,
           EXP003_FSK_FREQ_0, EXP003_FSK_FREQ_1);
    return 0;
}

static int do_decode(const char *path)
{
    size_t n = 0u, start, i;
    uint32_t sr = 0u; uint16_t bps = 0u, ch = 0u;
    exp001_frame_config_t fc;
    exp001_decode_result_t dr;
    exp001_preamble_detect_t det;
    uint8_t rec[EXP001_MAX_PAYLOAD_BYTES];

    if (exp001_wav_read(path, g_s, MAXS, &n, &sr, &bps, &ch) != EXP001_OK) {
        printf("WAV read failed (format rejected or unreadable)\n");
        return 2;
    }
    configure(&fc);

    /*
     * A host recording has arbitrary lead-in, so acquire across the whole
     * capture and then hand the decoder a window positioned the way it
     * expects. This changes where the receiver is pointed, never how it works.
     */
    det = exp001_detect_preamble_iq(fc.preamble_type, fc.preamble_duration_s,
                                    fc.preamble_f_start_hz, fc.preamble_f_end_hz,
                                    fc.detection_threshold, g_s, n);
    printf("acquisition index=%zu correlation=%.6f detected=%u\n",
           det.peak_sample_index, det.peak_correlation, det.detected);
    if (det.detected == 0u) { printf("RESULT: not acquired\n"); return 3; }

    start = (det.peak_sample_index > LEAD) ? det.peak_sample_index - LEAD : 0u;
    if (exp001_frame_decode(&fc, g_s + start, n - start,
                            rec, sizeof(rec), &dr) != EXP001_OK) {
        printf("decode len=%zu rx_crc=0x%04X calc_crc=0x%04X\n",
               dr.payload_bytes, dr.received_crc, dr.computed_crc);
        printf("RESULT: no exact recovery\n");
        return 3;
    }

    printf("bytes:");
    for (i = 0u; i < dr.payload_bytes; ++i) printf(" %02X", rec[i]);
    printf("\n");

    if (dr.payload_bytes == sizeof(expected_wire) &&
        memcmp(rec, expected_wire, sizeof(expected_wire)) == 0) {
        mcl_wire_tier0_t obj;
        size_t consumed = 0u;
        if (mcl_wire_tier0_decode(rec, dr.payload_bytes, &obj, &consumed)
                == MCL_WIRE_OK && obj.kind == MCL_WIRE_KIND_PRESENCE) {
            printf("RESULT: EXACT WIRE + EXACT PRESENCE SEMANTIC RECOVERY\n");
            return 0;
        }
    }
    printf("RESULT: CRC valid but bytes differ from the canonical vector\n");
    return 3;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        printf("usage: exp003 gen|decode <wav>\n");
        return 2;
    }
    if (strcmp(argv[1], "gen") == 0)    return do_gen(argv[2]);
    if (strcmp(argv[1], "decode") == 0) return do_decode(argv[2]);
    printf("unknown mode\n");
    return 2;
}
