/*
 * Codec 2 plug-in for SEMS.
 *
 * Low-bitrate speech codec (8 kHz, mono) using libcodec2.
 * Fixed 2400 bps mode, matching the FreeSWITCH mod_codec2 default. Codec 2 has
 * no standardized SDP fmtp, so the mode is not signalled - both ends must be
 * configured to the same mode.
 *
 * This file is part of SEMS, a free SIP media server.
 *
 * SEMS is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version. This program is released under
 * the GPL with the additional exemption that compiling, linking,
 * and/or using OpenSSL is allowed.
 *
 * SEMS is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include "amci.h"
#include "codecs.h"
#include "../../log.h"

#include <codec2.h>

#include <stdlib.h>

/* per-connection codec state; the negotiated mode is kept here so that
   bytes<->samples conversions know the frame geometry */
struct Codec2State {
    struct CODEC2 *encoder;
    struct CODEC2 *decoder;
    int            samples_per_frame; /* PCM samples per codec2 frame (160 or 320) */
    int            bytes_per_frame;   /* encoded bytes per codec2 frame */
};

static int pcm16_2_codec2(unsigned char *out_buf, unsigned char *in_buf, unsigned int size, unsigned int channels,
                          unsigned int rate, long h_codec);
static int codec2_2_pcm16(unsigned char *out_buf, unsigned char *in_buf, unsigned int size, unsigned int channels,
                          unsigned int rate, long h_codec);

static long codec2_state_create(const char *format_parameters, amci_codec_fmt_info_t *format_description);
static void codec2_state_destroy(long h_codec);

static unsigned int codec2_bytes2samples(long h_codec, unsigned int num_bytes);
static unsigned int codec2_samples2bytes(long h_codec, unsigned int num_samples);

BEGIN_EXPORTS("codec2", AMCI_NO_MODULEINIT, AMCI_NO_MODULEDESTROY)

BEGIN_CODECS
CODEC(CODEC_CODEC2, pcm16_2_codec2, codec2_2_pcm16, AMCI_NO_CODEC_PLC, codec2_state_create, codec2_state_destroy,
      codec2_bytes2samples, codec2_samples2bytes)
END_CODECS

BEGIN_PAYLOADS
PAYLOAD(-1, "CODEC2", 8000, 8000, 1, CODEC_CODEC2, AMCI_PT_AUDIO_FRAME)
END_PAYLOADS

BEGIN_FILE_FORMATS
END_FILE_FORMATS

END_EXPORTS

static long codec2_state_create(const char *format_parameters, amci_codec_fmt_info_t *format_description)
{
    /* fixed 2400 bps; Codec 2 has no standardized SDP fmtp to negotiate the mode */
    const int mode = CODEC2_MODE_2400;

    Codec2State *st = static_cast<Codec2State *>(malloc(sizeof(Codec2State)));
    if (!st) {
        ERROR("codec2: could not allocate state");
        return -1;
    }

    st->encoder = codec2_create(mode);
    st->decoder = codec2_create(mode);
    if (!st->encoder || !st->decoder) {
        ERROR("codec2: codec2_create failed for mode %d", mode);
        if (st->encoder)
            codec2_destroy(st->encoder);
        if (st->decoder)
            codec2_destroy(st->decoder);
        free(st);
        return -1;
    }

    st->samples_per_frame = codec2_samples_per_frame(st->encoder);
    st->bytes_per_frame   = codec2_bytes_per_frame(st->encoder);

    format_description[0].id    = AMCI_FMT_FRAME_LENGTH;
    format_description[0].value = st->samples_per_frame / 8; /* 8 samples per ms @ 8 kHz -> 20 or 40 ms */
    format_description[1].id    = AMCI_FMT_FRAME_SIZE;
    format_description[1].value = st->samples_per_frame;
    format_description[2].id    = AMCI_FMT_ENCODED_FRAME_SIZE;
    format_description[2].value = st->bytes_per_frame;
    format_description[3].id    = 0;

    DBG("codec2: created mode %d, %d samples/frame, %d bytes/frame", mode, st->samples_per_frame, st->bytes_per_frame);

    return (long)st;
}

static void codec2_state_destroy(long h_codec)
{
    Codec2State *st = (Codec2State *)h_codec;

    if (!st)
        return;

    codec2_destroy(st->encoder);
    codec2_destroy(st->decoder);
    free(st);
}

static unsigned int codec2_bytes2samples(long h_codec, unsigned int num_bytes)
{
    Codec2State *st = (Codec2State *)h_codec;
    return st->samples_per_frame * (num_bytes / st->bytes_per_frame);
}

static unsigned int codec2_samples2bytes(long h_codec, unsigned int num_samples)
{
    Codec2State *st = (Codec2State *)h_codec;
    return st->bytes_per_frame * (num_samples / st->samples_per_frame);
}

static int pcm16_2_codec2(unsigned char *out_buf, unsigned char *in_buf, unsigned int size, unsigned int channels,
                          unsigned int rate, long h_codec)
{
    Codec2State *st = (Codec2State *)h_codec;

    if (!st || channels != 1)
        return -1;

    /* encode only whole frames; a trailing partial frame is dropped (resampling on
       rate change may yield unaligned sizes - better lose a few samples than the call) */
    unsigned int in_frame_bytes = st->samples_per_frame * sizeof(short);
    unsigned int frames         = size / in_frame_bytes;
    for (unsigned int i = 0; i < frames; i++)
        codec2_encode(st->encoder, out_buf + i * st->bytes_per_frame, (short *)(in_buf + i * in_frame_bytes));

    return frames * st->bytes_per_frame;
}

static int codec2_2_pcm16(unsigned char *out_buf, unsigned char *in_buf, unsigned int size, unsigned int channels,
                          unsigned int rate, long h_codec)
{
    Codec2State *st = (Codec2State *)h_codec;

    if (!st || channels != 1)
        return -1;

    /* decode only whole frames; a trailing partial frame is dropped */
    unsigned int out_frame_bytes = st->samples_per_frame * sizeof(short);
    unsigned int frames          = size / st->bytes_per_frame;
    for (unsigned int i = 0; i < frames; i++)
        codec2_decode(st->decoder, (short *)(out_buf + i * out_frame_bytes), in_buf + i * st->bytes_per_frame);

    return frames * out_frame_bytes;
}
