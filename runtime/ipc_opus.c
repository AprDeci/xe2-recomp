/* hwopus - the hardware Opus decoder service, natively.
 *
 * Packets arrive with an 8-byte header (big-endian size and final range) before the Opus
 * data; a decode reports the bytes consumed and the samples per channel produced, as
 * interleaved 16-bit PCM. The decoding itself is libopus (linked in when the build finds it,
 * SSBU_HAVE_OPUS); without it, or for a packet it rejects, the output is silence of the
 * packet's length, which follows from its TOC byte (RFC 6716, 3.1), so timing stays exact. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ipc.h"

#ifdef SSBU_HAVE_OPUS
typedef struct OpusDecoder OpusDecoder;
typedef struct OpusMSDecoder OpusMSDecoder;
OpusDecoder *opus_decoder_create(int rate, int channels, int *error);
int opus_decode(OpusDecoder *st, const unsigned char *data, int len, short *pcm, int frame_size, int decode_fec);
int opus_decoder_ctl(OpusDecoder *st, int request, ...);
OpusMSDecoder *opus_multistream_decoder_create(int rate, int channels, int streams, int coupled_streams,
                                               const unsigned char *mapping, int *error);
int opus_multistream_decode(OpusMSDecoder *st, const unsigned char *data, int len, short *pcm, int frame_size, int decode_fec);
int opus_multistream_decoder_ctl(OpusMSDecoder *st, int request, ...);
#define OPUS_RESET_STATE 4028
#endif

typedef struct Decoder {
    u32 rate, channels;
#ifdef SSBU_HAVE_OPUS
    OpusDecoder *one;
    OpusMSDecoder *multi;
#endif
} Decoder;

static u32 be32(const u8 *p) { return (u32)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

/* Samples per channel in one packet, at the given rate. */
static u32 packet_samples(const u8 *data, u32 size, u32 rate) {
    if (!size) return 0;
    u32 config = data[0] >> 3, code = data[0] & 3, frames = code == 0 ? 1 : code < 3 ? 2 : size > 1 ? data[1] & 0x3F : 0;
    u32 tenths_ms;   /* frame duration in units of 0.1 ms */
    if (config < 12) { static const u32 d[4] = {100, 200, 400, 600}; tenths_ms = d[config & 3]; }        /* SILK */
    else if (config < 16) tenths_ms = config & 1 ? 200 : 100;                                         /* hybrid */
    else { static const u32 d[4] = {25, 50, 100, 200}; tenths_ms = d[config & 3]; }                    /* CELT */
    return frames * tenths_ms * rate / 10000;
}

static void decode(Decoder *d, Ipc *ipc, int with_perf, int reset) {
    u64 in_size, out_size;
    u64 in = ipc_in_buffer(ipc, 0, &in_size), out = ipc_out_buffer(ipc, 0, &out_size);
    u32 consumed = 0, samples = 0;
#ifdef SSBU_HAVE_OPUS
    if (reset) {
        if (d->one) opus_decoder_ctl(d->one, OPUS_RESET_STATE);
        if (d->multi) opus_multistream_decoder_ctl(d->multi, OPUS_RESET_STATE);
    }
#else
    (void)reset;
#endif
    if (in && in_size >= 8) {
        const u8 *p = P(u8, in);
        u32 size = be32(p);
        if (size <= in_size - 8) {
            consumed = 8 + size;
            u32 room = (u32)(out_size / (d->channels * 2));
            int n = -1;
#ifdef SSBU_HAVE_OPUS
            if (out && room) {
                if (d->multi) n = opus_multistream_decode(d->multi, p + 8, (int)size, P(short, out), (int)room, 0);
                else if (d->one) n = opus_decode(d->one, p + 8, (int)size, P(short, out), (int)room, 0);
            }
#endif
            if (n >= 0) samples = (u32)n;
            else {   /* no decoder, or a packet it would not take: its length in silence */
                samples = packet_samples(p + 8, size, d->rate);
                if (samples > room) samples = room;
                if (out) memset(P(void, out), 0, (u64)samples * d->channels * 2);
            }
            if (out) ipc_wrote(ipc, 0, (u64)samples * d->channels * 2);
        }
    }
    IPC_PUSH(ipc, consumed);
    IPC_PUSH(ipc, samples);
    if (with_perf) IPC_PUSH(ipc, (u64)0);
}

static void decoder(IpcObject *self, Ipc *ipc) {
    Decoder *d = self->state;
    switch (ipc->command) {
    case 0: case 2: decode(d, ipc, 0, 0); return;            /* DecodeInterleaved(ForMultiStream)Old */
    case 1: case 3: return;                                 /* SetContext(ForMultiStream) */
    case 4: case 5: decode(d, ipc, 1, 0); return;            /* DecodeInterleaved(ForMultiStream)WithPerfOld */
    case 6: case 7: case 8: case 9:                         /* ...WithPerfAndResetOld, DecodeInterleaved(ForMultiStream): reset flag first */
        decode(d, ipc, 1, ipc_u32(ipc, 0) & 0xFF);
        return;
    }
    ipc_unknown(ipc, self);
}

/* The decoder for a stream of rate and channels; multistream ones have a stream layout too. */
static Decoder *open_decoder(u32 rate, u32 channels, const u8 *layout) {
    Decoder *d = calloc(1, sizeof *d);
    d->rate = rate == 8000 || rate == 12000 || rate == 16000 || rate == 24000 ? rate : 48000;
    d->channels = channels && channels <= 8 ? channels : 2;
#ifdef SSBU_HAVE_OPUS
    int err = 0;
    if (layout) {   /* total streams, stereo streams, then the channel mapping */
        u32 streams = *(const u32 *)layout, coupled = *(const u32 *)(layout + 4);
        d->multi = opus_multistream_decoder_create((int)d->rate, (int)d->channels, (int)streams, (int)coupled, layout + 8, &err);
        if (!d->multi) fprintf(stderr, "[opus] multistream decoder (%u Hz, %u channels, %u/%u streams) failed: %d\n", d->rate, d->channels,
                               streams, coupled, err);
    } else {
        d->one = opus_decoder_create((int)d->rate, (int)d->channels, &err);
        if (!d->one) fprintf(stderr, "[opus] decoder (%u Hz, %u channels) failed: %d\n", d->rate, d->channels, err);
    }
#else
    (void)layout;
#endif
    return d;
}

static void manager(IpcObject *self, Ipc *ipc) {
    switch (ipc->command) {
    case 0: case 4: {   /* OpenHardwareOpusDecoder(Ex): sample rate, channels first */
        Decoder *d = open_decoder(ipc_u32(ipc, 0), ipc_u32(ipc, 4), NULL);
        ipc_object(ipc, ipc_new_object("hwopus:IHardwareOpusDecoder", decoder, d));
        return;
    }
    case 2: case 6: {   /* OpenHardwareOpusDecoderForMultiStream(Ex): the parameters in a buffer */
        u64 size;
        u64 in = ipc_in_buffer(ipc, 0, &size);
        u8 params[16 + 8 + 256];
        memset(params, 0, sizeof params);
        if (in) memcpy(params, P(void, in), size < sizeof params ? size : sizeof params);
        u32 rate, channels;
        memcpy(&rate, params, 4);
        memcpy(&channels, params + 4, 4);
        u8 layout[8 + 256];
        memcpy(layout, params + 8, 8);   /* total streams, stereo streams */
        memcpy(layout + 8, params + (ipc->command == 6 ? 20 : 16), 256 - 8);   /* Ex has a large-frame flag (and padding) first */
        Decoder *d = open_decoder(rate, channels, layout);
        ipc_object(ipc, ipc_new_object("hwopus:IHardwareOpusDecoder", decoder, d));
        return;
    }
    case 1: case 3: case 5: case 7: case 8: case 9: IPC_PUSH(ipc, (u32)0x4000); return;   /* GetWorkBufferSize... */
    }
    ipc_unknown(ipc, self);
}

IpcObject *service_opus(const char *name) {
    return strcmp(name, "hwopus") ? NULL : ipc_new_object("hwopus", manager, NULL);
}
