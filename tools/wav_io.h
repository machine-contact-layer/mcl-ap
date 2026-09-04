/*
 * Minimal PCM16 mono WAV reader/writer for the MCL-AP host tools.
 *
 * Host-only. It is deliberately NOT in mcl-ap/src: the acoustic binding runs
 * on machines with no filesystem, and a modem that needed a file format to
 * exist would not be portable to them. The board links ap_modem.c and nothing
 * from this header.
 *
 * The reader refuses rather than converts. A capture at the wrong rate, in
 * stereo, or in float is not a capture this project can compare against its
 * evidence, and silently resampling one into range would produce a number
 * that looks like a measurement and is not.
 */

#ifndef MCL_AP_WAV_IO_H
#define MCL_AP_WAV_IO_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* A header of static helpers: a translation unit that reads WAVs but never
   writes one must not be a build error over it. */
#if defined(__GNUC__) || defined(__clang__)
#define MCL_AP_WAV_MAYBE_UNUSED __attribute__((unused))
#else
#define MCL_AP_WAV_MAYBE_UNUSED
#endif

enum {
    WAV_OK = 0,
    WAV_ERR_IO = 1,
    WAV_ERR_FORMAT = 2,
    WAV_ERR_CAPACITY = 3
};

MCL_AP_WAV_MAYBE_UNUSED
static uint32_t wav_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

MCL_AP_WAV_MAYBE_UNUSED
static uint16_t wav_u16(const uint8_t *p)
{
    return (uint16_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8));
}

/*
 * Read a 48 kHz PCM16 mono WAV. Walks the chunk list rather than assuming the
 * canonical 44-byte header: recorders insert LIST/INFO chunks, and a fixed
 * offset reads those as samples.
 */
MCL_AP_WAV_MAYBE_UNUSED
static int wav_read_pcm16(const char *path, int16_t *out, size_t capacity,
                          size_t *out_count)
{
    FILE *f = fopen(path, "rb");
    uint8_t header[12];
    uint8_t chunk[8];
    uint16_t channels = 0u, bits = 0u, format = 0u;
    uint32_t rate = 0u;
    int have_fmt = 0;

    if (f == NULL) {
        return WAV_ERR_IO;
    }
    if (fread(header, 1u, sizeof(header), f) != sizeof(header) ||
        memcmp(header, "RIFF", 4) != 0 || memcmp(header + 8, "WAVE", 4) != 0) {
        fclose(f);
        return WAV_ERR_FORMAT;
    }

    while (fread(chunk, 1u, sizeof(chunk), f) == sizeof(chunk)) {
        uint32_t size = wav_u32(chunk + 4);

        if (memcmp(chunk, "fmt ", 4) == 0) {
            uint8_t fmt[16];
            if (size < sizeof(fmt) ||
                fread(fmt, 1u, sizeof(fmt), f) != sizeof(fmt)) {
                fclose(f);
                return WAV_ERR_FORMAT;
            }
            format = wav_u16(fmt);
            channels = wav_u16(fmt + 2);
            rate = wav_u32(fmt + 4);
            bits = wav_u16(fmt + 14);
            have_fmt = 1;
            if (size > sizeof(fmt)) {
                fseek(f, (long)(size - sizeof(fmt)), SEEK_CUR);
            }
        } else if (memcmp(chunk, "data", 4) == 0) {
            size_t count;
            if (!have_fmt) {
                fclose(f);
                return WAV_ERR_FORMAT;
            }
            if (format != 1u || channels != 1u || bits != 16u ||
                rate != 48000u) {
                fclose(f);
                return WAV_ERR_FORMAT;
            }
            count = (size_t)size / 2u;
            if (count > capacity) {
                fclose(f);
                return WAV_ERR_CAPACITY;
            }
            if (fread(out, 2u, count, f) != count) {
                fclose(f);
                return WAV_ERR_IO;
            }
            fclose(f);
            *out_count = count;
            return WAV_OK;
        } else {
            fseek(f, (long)size + (long)(size & 1u), SEEK_CUR);
        }
    }
    fclose(f);
    return WAV_ERR_FORMAT;
}

MCL_AP_WAV_MAYBE_UNUSED
static void wav_put_u32(FILE *f, uint32_t v)
{
    fputc((int)(v & 0xFFu), f);
    fputc((int)((v >> 8) & 0xFFu), f);
    fputc((int)((v >> 16) & 0xFFu), f);
    fputc((int)((v >> 24) & 0xFFu), f);
}

MCL_AP_WAV_MAYBE_UNUSED
static void wav_put_u16(FILE *f, uint16_t v)
{
    fputc((int)(v & 0xFFu), f);
    fputc((int)((v >> 8) & 0xFFu), f);
}

MCL_AP_WAV_MAYBE_UNUSED
static int wav_write_pcm16(const char *path, const int16_t *pcm, size_t count)
{
    FILE *f = fopen(path, "wb");
    uint32_t data_bytes = (uint32_t)(count * 2u);
    size_t i;

    if (f == NULL) {
        return WAV_ERR_IO;
    }
    fwrite("RIFF", 1u, 4u, f);
    wav_put_u32(f, 36u + data_bytes);
    fwrite("WAVE", 1u, 4u, f);
    fwrite("fmt ", 1u, 4u, f);
    wav_put_u32(f, 16u);
    wav_put_u16(f, 1u);          /* PCM */
    wav_put_u16(f, 1u);          /* mono */
    wav_put_u32(f, 48000u);
    wav_put_u32(f, 48000u * 2u); /* byte rate */
    wav_put_u16(f, 2u);          /* block align */
    wav_put_u16(f, 16u);
    fwrite("data", 1u, 4u, f);
    wav_put_u32(f, data_bytes);
    for (i = 0u; i < count; ++i) {
        wav_put_u16(f, (uint16_t)pcm[i]);
    }
    fclose(f);
    return WAV_OK;
}

#endif /* MCL_AP_WAV_IO_H */
