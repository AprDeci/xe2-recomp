/* Real audio output for audren (XC2 -- issue #3 "no sound").
 *
 * XC2 drives nn::audio's software renderer: it creates voices (AcquireVoiceSlot) and streams
 * source buffers into them (AppendWaveBuffer). On the console the DSP decodes and mixes those
 * voices; the runtime has no DSP, so this file is a small mixer: at append time it copies each
 * buffer out of guest memory, then the waveOut clock lazily decodes one frame per voice per
 * output frame and sums them into one 48 kHz stereo stream. Lazy decoding (rather than expanding
 * a 24 s music buffer up front) keeps memory bounded and lets a slow card simply throttle.
 *
 * Voice formats seen on this title:
 *   - SampleFormat::Adpcm (fmt 6): Nintendo DSP-ADPCM, 8-byte frames, 14 samples/frame, using the
 *     per-voice coefficient table the game passes to AcquireVoiceSlot (32-byte ptr = 8 coef pairs).
 *     This is where the music lives (two looped layers).
 *   - SampleFormat::PcmInt16 (fmt 2): linear PCM, played directly (UI / one-shot effects).
 * Any other format is passed through as silence for now.
 *
 * ABI: HLE_WRAP runs INSTEAD of the original, so every guest input is read from c->x BEFORE the
 * *_orig call (that call runs guest code and may clobber caller-saved slots); the return value is
 * written back afterwards. We never touch guest bookkeeping fields.
 *
 * Pacing: this host clock is the real consumer -- waveOut pulls from the mixer at 48 kHz; the
 * game itself is paced separately by ipc_audio.c's renderer_clock.
 *
 * Env: SSBU_AUDIO_SELFTEST=1 -> synthetic-tone + buffer-recycle self-test (proves the device
 * consumes). SSBU_AUDIO_WAV=<path> -> writes the played stream. SSBU_AUDIO_STATS=1 -> counters. */
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "ipc.h"
#ifdef _WIN32
#include <mmsystem.h>

#define ORATE       48000
#define NBUF        4
#define BUF_FRAMES  1200                        /* 25 ms per card buffer */
#define FMT_PCM16   2
#define FMT_ADPCM   6
#define MAXVOICES   16
#define MAXQBUF     8                            /* pending source buffers per voice */
#define MAXCOPY     (8u << 20)                   /* cap one copied buffer at 8 MiB */

typedef struct { u8 *data; u64 size; int loop; u32 start, end; } SrcBuf;  /* host copy + declared region */
typedef struct {
    u64 vt; int fmt, rate, ch;
    i16 coef[16][2]; int have_coef, pairs;
    SrcBuf q[MAXQBUF]; int qh, qn;               /* FIFO of pending source buffers */
    u64 spos;                                    /* sample index within q[qh], exclusive end = q[qh].end */
    u32 next_frame;                              /* next ADPCM frame index to decode in q[qh] */
    int h1, h2;                                  /* DSP-ADPCM history */
    i16 cache[14]; int ci, cachelen;             /* decoded ADPCM frame cache */
} VRec;
static VRec vrecs[MAXVOICES]; static int nvrecs;

static CRITICAL_SECTION rlock;
static HWAVEOUT wout; static WAVEHDR whdr[NBUF]; static i16 *wbuf[NBUF];
static volatile int wfree[NBUF];
static HANDLE wevent; static volatile LONG player_up, open_failed, shut;

static volatile LONG st_appends, st_submitted, st_frames_in, st_frames_out, st_underrun, st_voicenoise, st_womdone, st_reject;
static volatile long st_peak;

/* ---- WAV capture ---- */
typedef struct { FILE *f; unsigned bytes; } Wav;
static Wav gwav, twav;
static void wav_headers(Wav *w) {
    if (!w->f) return;
    unsigned riff = 36 + w->bytes, data = w->bytes; unsigned sz = 16;
    unsigned short pcm = 1, ch = 2, bits = 16, ba = 4; unsigned rate = ORATE, br = ORATE * 4;
    fseek(w->f, 0, SEEK_SET); fwrite("RIFF", 4, 1, w->f); fwrite(&riff, 4, 1, w->f); fwrite("WAVEfmt ", 8, 1, w->f);
    fwrite(&sz, 4, 1, w->f); fwrite(&pcm, 2, 1, w->f); fwrite(&ch, 2, 1, w->f); fwrite(&rate, 4, 1, w->f);
    fwrite(&br, 4, 1, w->f); fwrite(&ba, 2, 1, w->f); fwrite(&bits, 2, 1, w->f); fwrite("data", 4, 1, w->f);
    fwrite(&data, 4, 1, w->f); fseek(w->f, 0, SEEK_END);
}
static void wav_open(Wav *w, const char *path) {
    w->f = fopen(path, "wb"); w->bytes = 0;
    if (!w->f) { fprintf(stderr, "[audio] cannot open WAV %s\n", path); return; }
    unsigned char zero[44] = {0}; fwrite(zero, 1, 44, w->f); wav_headers(w);
    fprintf(stderr, "[audio] WAV capture -> %s\n", path);
}
static void wav_put(Wav *w, const void *data, unsigned n) {
    if (!w->f) return; fwrite(data, 1, n, w->f); w->bytes += n;
    if ((w->bytes / (BUF_FRAMES * 4)) % 8 == 0) wav_headers(w);
}

/* ---- per-voice lazy decode: produce one 48 kHz stereo frame.
 * Returns 1 if this voice contributed a sample, 0 if it has nothing queued (silence). */
/* Capacity in samples a buffer can yield, and its per-format validity.
 * ADPCM: 8-byte frames of 14 samples; a trailing partial frame (>=2 bytes: 1 header + data) yields
 *   2*(rem-1) more samples. PCM16: size/(2*ch) frames. Returns 0 if the buffer is unusable. */
static u32 buf_capacity(int fmt, int ch, u64 size) {
    if (fmt == FMT_PCM16) { if (!ch || size % (2u * ch)) return 0; return (u32)(size / (2u * (u64)ch)); }
    if (fmt == FMT_ADPCM) { u64 rem = size % 8; return (u32)((size / 8) * 14 + (rem >= 2 ? 2 * (rem - 1) : 0)); }
    return 0;
}
/* Decode-and-discard from the buffer start to sample index `target`, keeping h1/h2 continuous, so a
 * non-zero start begins from the correct predictor state (never a raw byte jump + history reset). */
static void adpcm_preroll(VRec *v, u64 target) {
    SrcBuf *b = &v->q[v->qh];
    v->h1 = v->h2 = 0; v->spos = 0; v->ci = v->cachelen = 0;
    u32 f = 0;
    while (v->spos < target) {
        u64 fo = (u64)f * 8;
        if (fo + 1 > b->size) break;
        u8 hd = b->data[fo];
        int sc = 1 << (hd & 0xF), pp = (hd >> 4) & 0xF;
        if (pp >= v->pairs) pp = 0;
        int a1 = v->coef[pp][0], a2 = v->coef[pp][1];
        u64 nd = b->size - fo - 1; if (nd > 7) nd = 7;
        int nn = (int)(nd * 2); if (nn > 14) nn = 14;
        for (int k = 0; k < nn && v->spos < target; k++) {
            int bi = k >> 1; u8 by = bi < 7 ? b->data[fo + 1 + bi] : 0;
            int nib = (k & 1) ? (by & 0xF) : (by >> 4);
            int s = nib >= 8 ? nib - 16 : nib;
            int sm = ((0x400 + a1 * v->h1 + a2 * v->h2) >> 11) + s * sc;
            if (sm > 32767) sm = 32767; if (sm < -32768) sm = -32768;
            v->h2 = v->h1; v->h1 = sm; v->spos++;
        }
        f++; v->next_frame = f;
    }
    v->spos = target;
}
/* Rewind (loop) or drop the head buffer. On loop we restart at the declared start and re-derive the
 * predictor state (h1/h2) by pre-roll, so the loop point is sample-exact; a new buffer resets. */
static void voice_pop_buf(VRec *v) {
    if (v->qn <= 0) return;
    if (v->q[v->qh].loop) {                          /* looped buffer: rewind to declared start */
        if (v->fmt == FMT_ADPCM && v->q[v->qh].start) { adpcm_preroll(v, v->q[v->qh].start); }
        else { v->spos = v->q[v->qh].start; v->next_frame = (u32)(v->spos / 14); v->ci = v->cachelen = 0;
               if (v->fmt == FMT_ADPCM) { v->h1 = v->h2 = 0; } }
        return;
    }
    free(v->q[v->qh].data); v->q[v->qh].data = NULL; v->q[v->qh].size = 0;
    v->qh = (v->qh + 1) % MAXQBUF; v->qn--; v->spos = 0; v->next_frame = 0;
    if (v->fmt == FMT_ADPCM) { v->h1 = v->h2 = 0; v->ci = v->cachelen = 0; }
}
/* Produce one frame of one voice. Returns 1 if it contributed a sample, 0 if nothing queued. */
static int voice_sample(VRec *v, float *l, float *r) {
    while (v->qn > 0 && v->spos >= v->q[v->qh].end) voice_pop_buf(v);   /* consume region; rewind if loop */
    if (v->qn <= 0) return 0;
    SrcBuf *b = &v->q[v->qh];
    if (v->fmt == FMT_PCM16) {
        u64 fb = 2u * (u64)v->ch;
        u64 o = (u64)v->spos * fb;
        if (o + fb > b->size) { voice_pop_buf(v); return 0; }
        if (v->ch >= 2) { *l = (float)(i16)(b->data[o] | b->data[o+1] << 8); *r = (float)(i16)(b->data[o+2] | b->data[o+3] << 8); }
        else            { *l = *r = (float)(i16)(b->data[o] | b->data[o+1] << 8); }
        v->spos++; return 1;
    }
    if (v->fmt == FMT_ADPCM && v->have_coef) {
        if (v->ci >= v->cachelen) {                            /* decode the frame holding spos */
            u64 fr_off = (u64)v->next_frame * 8;
            if (fr_off + 1 > b->size) { voice_pop_buf(v); return 0; }
            u8 head = b->data[fr_off];
            int scale = 1 << (head & 0xF), p = (head >> 4) & 0xF;
            if (p >= v->pairs) p = 0;
            int c1 = v->coef[p][0], c2 = v->coef[p][1];
            u64 ndata = b->size - fr_off - 1;                  /* available data bytes (7, or fewer in the tail) */
            if (ndata > 7) ndata = 7;
            int n = (int)(ndata * 2); if (n > 14) n = 14;
            for (int i = 0; i < n; i++) {
                int byteidx = i >> 1;
                u8 byte = byteidx < 7 ? b->data[fr_off + 1 + byteidx] : 0;
                int nib = (i & 1) ? (byte & 0xF) : (byte >> 4);   /* high nibble first */
                int s = nib >= 8 ? nib - 16 : nib;
                int samp = ((0x400 + c1 * v->h1 + c2 * v->h2) >> 11) + s * scale;   /* Dolphin: +0x400 round */
                if (samp > 32767) samp = 32767; if (samp < -32768) samp = -32768;
                v->cache[i] = (i16)samp; v->h2 = v->h1; v->h1 = samp;
            }
            v->cachelen = n; v->ci = 0; v->next_frame++;
        }
        if (v->ci >= v->cachelen) { voice_pop_buf(v); return 0; }   /* e.g. header-only tail */
        *l = *r = (float)v->cache[v->ci++];
        v->spos++; return 1;
    }
    voice_pop_buf(v);                                   /* unsupported -> silence for this voice */
    return 0;
}

/* ---- card buffer fill ---- */
static void fill_one(int i) {
    i16 *dst = wbuf[i]; static i16 out[2 * BUF_FRAMES];
    EnterCriticalSection(&rlock);
    for (u32 f = 0; f < BUF_FRAMES; f++) {
        float l = 0, r = 0; int any = 0;
        for (int vi = 0; vi < nvrecs; vi++) { float vl, vr; if (voice_sample(&vrecs[vi], &vl, &vr)) { l += vl; r += vr; any = 1; } }
        if (!any) InterlockedIncrement(&st_underrun);
        int il = (int)l, ir = (int)r;
        if (il > 32767) il = 32767; if (il < -32768) il = -32768;
        if (ir > 32767) ir = 32767; if (ir < -32768) ir = -32768;
        long al = il < 0 ? -(long)il : il, ar = ir < 0 ? -(long)ir : ir;
        if (al > st_peak) st_peak = al; if (ar > st_peak) st_peak = ar;
        out[2*f] = (i16)il; out[2*f+1] = (i16)ir;
    }
    LeaveCriticalSection(&rlock);
    memcpy(dst, out, sizeof out);
    wav_put(&gwav, dst, sizeof out);
}
static void CALLBACK wdone(HWAVEOUT h, UINT m, DWORD_PTR u, DWORD_PTR a, DWORD_PTR b) {
    (void)h; (void)u; (void)a; (void)b;
    if (m == WOM_DONE) { InterlockedIncrement(&st_womdone); SetEvent(wevent); }
}
static DWORD WINAPI player(void *x) {
    (void)x; int tick = 0;
    while (!shut) {
        WaitForSingleObject(wevent, 20);
        for (int i = 0; i < NBUF; i++)                          /* recycle completed buffers */
            if (!wfree[i] && (whdr[i].dwFlags & WHDR_DONE)) { wfree[i] = 1; whdr[i].dwFlags &= ~WHDR_DONE; }
        for (int i = 0; i < NBUF; i++) if (wfree[i]) {
            wfree[i] = 0; fill_one(i);
            whdr[i].dwBufferLength = BUF_FRAMES * 4;
            MMRESULT mr = waveOutWrite(wout, &whdr[i], sizeof whdr[i]);
            if (mr != MMSYSERR_NOERROR) { wfree[i] = 1; fprintf(stderr, "[audio] waveOutWrite failed %u\n", (unsigned)mr); }
            else InterlockedIncrement(&st_frames_out);
        }
        if (++tick % 50 == 0 && (rt_trace || getenv("SSBU_AUDIO_STATS")))
            fprintf(stderr, "[audio] stats: appends=%ld submitted=%ld reject=%ld frames_out=%ld wom_done=%ld underrun=%ld peak=%ld\n",
                    (long)st_appends, (long)st_submitted, (long)st_reject, (long)st_frames_out, (long)st_womdone, (long)st_underrun, st_peak);
    }
    return 0;
}
static void player_open(void) {
    if (InterlockedCompareExchange(&player_up, 1, 0)) return;
    InitializeCriticalSection(&rlock);
    wevent = CreateEventA(NULL, FALSE, FALSE, NULL);
    WAVEFORMATEX fmt = {WAVE_FORMAT_PCM, 2, ORATE, ORATE * 4, 4, 16, 0};
    MMRESULT mr = waveOutOpen(&wout, WAVE_MAPPER, &fmt, (DWORD_PTR)wdone, 0, CALLBACK_FUNCTION);
    if (mr != MMSYSERR_NOERROR) {
        fprintf(stderr, "[audio] waveOutOpen failed (%u); audren will be silent\n", (unsigned)mr);
        wout = NULL; open_failed = 1; return;
    }
    for (int i = 0; i < NBUF; i++) {
        wbuf[i] = malloc(BUF_FRAMES * 4);
        memset(&whdr[i], 0, sizeof whdr[i]);
        whdr[i].lpData = (LPSTR)wbuf[i]; whdr[i].dwBufferLength = BUF_FRAMES * 4;
        MMRESULT pr = waveOutPrepareHeader(wout, &whdr[i], sizeof whdr[i]);
        if (pr != MMSYSERR_NOERROR) fprintf(stderr, "[audio] prepare header %d failed %u\n", i, (unsigned)pr);
        wfree[i] = 1;
    }
    { const char *p = getenv("SSBU_AUDIO_WAV"); if (p && *p) wav_open(&gwav, p); }
    fprintf(stderr, "[audio] waveOut ready: %d Hz stereo, %d x %d-frame buffers\n", ORATE, NBUF, BUF_FRAMES);
    CloseHandle(CreateThread(NULL, 0, player, NULL, 0, NULL));
}

/* Fixed-vector self-test for the actual decode path (fails on the old truncation version and on
 * any tail/region bug). Runs under rlock (caller holds it) so it cannot race the player thread. */
static int audio_vectors(void) {
    /* Synthetic Q11 pairs: (3/4, -1/4) and (5/4, -1/2), scaled by 2048.
     * A/B/E expectations are independently rebuildable with Python's exact fractions;
     * C/D use the same oracle and must saturate on every sample:
     *
     * from fractions import Fraction as Q
     * from math import floor
     * def oracle(a, b, h1, h2, scale, ns):
     *     out = []
     *     for n in ns:
     *         s = max(-32768, min(32767, floor(a*h1 + b*h2 + Q(1,2)) + n*scale))
     *         out.append(s)
     *         h2, h1 = h1, s
     *     return out
     * a, b = Q(3,4), Q(-1,4)
     * print(oracle(a, b, 5, 0, 1, [0]*14))                 # A
     * print(oracle(a, b, -1, 0, 1, [0]*14))                # B
     * assert oracle(a, b, 30000, 30000, 4096, [7]*14) == [32767]*14
     * assert oracle(a, b, -30000, -30000, 4096, [-8]*14) == [-32768]*14
     * print(oracle(Q(5,4), Q(-1,2), 0, 0, 32, range(7,-7,-1))) # E
     */
    static const i16 CO0[2] = {1536, -512};
    static const i16 CO4[2] = {2560, -1024};
    int fails = 0;
    /* decode one ADPCM frame with given coef/history, Dolphin rule (+0x400 round, s16 clamp) */
    #define DECFRAME(HEAD, DATA, C1, C2, H1, H2, OUT) do { \
        int _h1 = (H1), _h2 = (H2), _sc = 1 << ((HEAD) & 0xF); \
        for (int _i = 0; _i < 14; _i++) { \
            int _bi = _i >> 1; u8 _by = (_bi < 7) ? (u8)(DATA)[_bi] : 0; \
            int _nib = (_i & 1) ? (_by & 0xF) : (_by >> 4); \
            int _s = _nib >= 8 ? _nib - 16 : _nib; \
            int _sm = ((0x400 + (C1) * _h1 + (C2) * _h2) >> 11) + _s * _sc; \
            if (_sm > 32767) _sm = 32767; if (_sm < -32768) _sm = -32768; \
            (OUT)[_i] = (i16)_sm; _h2 = _h1; _h1 = _sm; \
        } } while (0)
    /* Vector A/B: zero data, synthetic coef0; distinguish rounding from truncation. */
    {
        static const u8 Z[7] = {0,0,0,0,0,0,0};
        static const i16 EA[14] = {4,2,1,0,0,0,0,0,0,0,0,0,0,0};
        static const i16 EB[14] = {-1,0,0,0,0,0,0,0,0,0,0,0,0,0};
        i16 o[14];
        DECFRAME(0x00, Z, CO0[0], CO0[1], 5, 0, o);
        for (int i = 0; i < 14; i++) if (o[i] != EA[i]) { fails++; fprintf(stderr, "[audiotest] vecA[%d]=%d want %d\n", i, o[i], EA[i]); break; }
        DECFRAME(0x00, Z, CO0[0], CO0[1], -1, 0, o);
        for (int i = 0; i < 14; i++) if (o[i] != EB[i]) { fails++; fprintf(stderr, "[audiotest] vecB[%d]=%d want %d\n", i, o[i], EB[i]); break; }
    }
    /* Vector C/D: saturation clamps to full scale in both directions */
    {
        static const u8 D7[7] = {0x77,0x77,0x77,0x77,0x77,0x77,0x77};
        static const u8 D8[7] = {0x88,0x88,0x88,0x88,0x88,0x88,0x88};
        i16 o[14];
        DECFRAME(0x0C, D7, CO0[0], CO0[1], 30000, 30000, o);
        for (int i = 0; i < 14; i++) if (o[i] != 32767) { fails++; fprintf(stderr, "[audiotest] vecC[%d]=%d want 32767\n", i, o[i]); break; }
        DECFRAME(0x0C, D8, CO0[0], CO0[1], -30000, -30000, o);
        for (int i = 0; i < 14; i++) if (o[i] != -32768) { fails++; fprintf(stderr, "[audiotest] vecD[%d]=%d want -32768\n", i, o[i]); break; }
    }
    /* Vector E: synthetic head 0x45 (pair 4, scale 32), h=0; nibble k = (7-k)&15.
     * Pack high nibble first: bytes are 0x76, 0x54, ..., 0xBA. */
    {
        static const u8 FR[7] = {0x76,0x54,0x32,0x10,0xfe,0xdc,0xba};
        static const i16 G[14] = {224,472,638,690,640,519,361,192,28,-125,-266,-398,-524,-648};
        i16 o[14];
        DECFRAME(0x45, FR, CO4[0], CO4[1], 0, 0, o);
        for (int i = 0; i < 14; i++) if (o[i] != G[i]) { fails++; fprintf(stderr, "[audiotest] vecE[%d]=%d want %d\n", i, o[i], G[i]); break; }
    }
    #undef DECFRAME
    /* Region/frame-count self-checks on buf_capacity */
    if (buf_capacity(FMT_ADPCM, 1, 655543) != 1147200) { fails++; fprintf(stderr, "[audiotest] cap(655543)=%u want 1147200\n", buf_capacity(FMT_ADPCM,1,655543)); }
    if (buf_capacity(FMT_ADPCM, 1, 655544) != 1147202) { fails++; fprintf(stderr, "[audiotest] cap(655544)=%u want 1147202\n", buf_capacity(FMT_ADPCM,1,655544)); }
    if (buf_capacity(FMT_ADPCM, 1, 1) != 0)            { fails++; fprintf(stderr, "[audiotest] cap(1)=%u want 0\n", buf_capacity(FMT_ADPCM,1,1)); }
    if (buf_capacity(FMT_PCM16, 2, 23040) != 5760)     { fails++; fprintf(stderr, "[audiotest] cap pcm=%u want 5760\n", buf_capacity(FMT_PCM16,2,23040)); }
    if (buf_capacity(FMT_PCM16, 2, 23042) != 0)        { fails++; fprintf(stderr, "[audiotest] odd pcm must be 0\n"); }
    /* Region/loop decode checks through the real voice path (synthetic ADPCM buffer, no device). */
    {
        /* build 8 synthetic silent frames: coef0, head 0x00 (scale 1), data/history 0 */
        u32 nfr = 8; u32 bsz = nfr * 8; u8 *tmpl = malloc(bsz);
        for (u32 f = 0; f < nfr; f++) { tmpl[f*8] = 0x00; for (int i = 0; i < 7; i++) tmpl[f*8+1+i] = 0x00; }
        VRec *tv = &vrecs[MAXVOICES - 1]; int savev = nvrecs; nvrecs = MAXVOICES;   /* isolated test slot */
        EnterCriticalSection(&rlock);                           /* keep the player thread off vrecs */
        memset(tv, 0, sizeof *tv); tv->fmt = FMT_ADPCM; tv->ch = 1; tv->pairs = 1; tv->have_coef = 1;
        tv->coef[0][0] = CO0[0]; tv->coef[0][1] = CO0[1];
        #define TVRESET(END, START, LOOP, SPOS, NF) do { \
            if (tv->qn > 0 && tv->q[tv->qh].data) free(tv->q[tv->qh].data); \
            tv->qh = 0; tv->qn = 1; tv->q[0].data = malloc(bsz); memcpy(tv->q[0].data, tmpl, bsz); \
            tv->q[0].size = bsz; tv->q[0].loop = (LOOP); tv->q[0].start = (START); tv->q[0].end = (END); \
            tv->spos = (SPOS); tv->next_frame = (NF); tv->ci = tv->cachelen = 0; tv->h1 = tv->h2 = 0; \
        } while (0)
        /* (a) full region: consume exactly nfr*14 samples */
        long got = 0; float l, r;
        TVRESET(nfr * 14, 0, 0, 0, 0);
        for (int g = 0; g < (int)(nfr*14) + 5; g++) { if (!voice_sample(tv, &l, &r)) break; got++; }
        if (got != (long)(nfr*14)) { fails++; fprintf(stderr, "[audiotest] region full got=%ld want %u\n", got, nfr*14); }
        /* (b) end truncation: end=30 -> exactly 30 samples */
        TVRESET(30, 0, 0, 0, 0);
        got = 0; for (int g = 0; g < 60; g++) { if (!voice_sample(tv, &l, &r)) break; got++; }
        if (got != 30) { fails++; fprintf(stderr, "[audiotest] end-trunc got=%ld want 30\n", got); }
        /* (c) non-zero start: [start,end) count + identical to from-0 decode slice */
        TVRESET(nfr * 14, 20, 0, 0, 0); adpcm_preroll(tv, 20);
        i16 region[64]; long nr = 0; while (nr < 40 && voice_sample(tv, &l, &r)) region[nr++] = (i16)l;
        /* reference: decode from 0 and take [20,60) */
        i16 ref[64]; long k = 0;
        TVRESET(nfr * 14, 0, 0, 0, 0);
        while (k < 60 && voice_sample(tv, &l, &r)) ref[k++] = (i16)l;
        int preok = (nr == 40); for (long i = 0; i < nr && preok; i++) if (region[i] != ref[20 + i]) preok = 0;
        if (!preok) { fails++; fprintf(stderr, "[audiotest] non-zero start pre-roll mismatch (nr=%ld)\n", nr); }
        /* (d) loop exactness: loop buffer, 3 loops -> 3*(end-start) samples */
        TVRESET(28, 0, 1, 0, 0);
        long lgot = 0; for (long g = 0; g < 3 * 28; g++) { if (!voice_sample(tv, &l, &r)) break; lgot++; }
        if (lgot != 3 * 28) { fails++; fprintf(stderr, "[audiotest] loop got=%ld want %d\n", lgot, 3*28); }
        /* (e) partial tail frame: 1 full frame + 1 header-only+5 data bytes (13 bytes) -> 14 + 8 = 22
         * samples, and the tail must NOT be dropped (old code returned 14). */
        {
            u32 tsz = 8 + 5; u8 *tbuf = malloc(tsz);
            tbuf[0] = 0x00; for (int i = 0; i < 7; i++) tbuf[1+i] = 0x00;   /* full frame */
            tbuf[8] = 0x00; for (int i = 0; i < 4; i++) tbuf[9+i] = 0x00;  /* partial: header + 4 data bytes = 8 samples */
            if (buf_capacity(FMT_ADPCM, 1, tsz) != 22) { fails++; fprintf(stderr, "[audiotest] cap(tail13)=%u want 22\n", buf_capacity(FMT_ADPCM,1,tsz)); }
            if (tv->qn > 0 && tv->q[tv->qh].data) free(tv->q[tv->qh].data);
            tv->qh = 0; tv->qn = 1; tv->q[0].data = tbuf; tv->q[0].size = tsz; tv->q[0].loop = 0;
            tv->q[0].start = 0; tv->q[0].end = 22; tv->spos = 0; tv->next_frame = 0; tv->ci = tv->cachelen = 0; tv->h1 = tv->h2 = 0;
            long tg = 0; for (int g = 0; g < 40; g++) { if (!voice_sample(tv, &l, &r)) break; tg++; }
            if (tg != 22) { fails++; fprintf(stderr, "[audiotest] tail-frame got=%ld want 22\n", tg); }
            /* header-only tail (size=1) must yield 0 and not overrun */
            free(tv->q[0].data);
            tv->qh = 0; tv->qn = 1; tv->q[0].data = malloc(1); tv->q[0].data[0] = 0x00; tv->q[0].size = 1;
            tv->q[0].loop = 0; tv->q[0].start = 0; tv->q[0].end = buf_capacity(FMT_ADPCM,1,1);
            tv->spos = 0; tv->next_frame = 0; tv->ci = tv->cachelen = 0; tv->h1 = tv->h2 = 0;
            long zg = 0; for (int g = 0; g < 5; g++) { if (!voice_sample(tv, &l, &r)) break; zg++; }
            if (zg != 0) { fails++; fprintf(stderr, "[audiotest] header-only got=%ld want 0\n", zg); }
            free(tv->q[0].data);
        }
        #undef TVRESET
        if (tv->qn > 0 && tv->q[tv->qh].data) free(tv->q[tv->qh].data);   /* loop buffer was never freed */
        memset(tv, 0, sizeof *tv);                              /* leave no dangling buffer behind */
        LeaveCriticalSection(&rlock);
        nvrecs = savev; free(tmpl);
    }
    fprintf(stderr, "[audiotest] vectors: %s (%d failure(s))\n", fails ? "FAIL" : "PASS", fails);
    return fails;
}

/* Synthetic-tone self-test through the same mixer+waveOut path; proves the device consumes and
 * recycles buffers (not just that it opened). Gated by env, runs once. */
static void audio_selftest(void) {
    static int done; if (done) return; done = 1;
    if (!player_up) player_open();
    EnterCriticalSection(&rlock);
    audio_vectors();                                        /* fixed-vector decode/region assertions */
    LeaveCriticalSection(&rlock);
    const char *p = getenv("SSBU_AUDIO_WAV");
    if (p && *p) { char t[300]; snprintf(t, sizeof t, "%s.tone.wav", p); wav_open(&twav, t); }
    if (!player_up) player_open();
    if (!wout) { fprintf(stderr, "[audiotest] FAIL: no audio device\n"); return; }
    long f0 = st_frames_out, w0 = st_womdone;
    /* Push a tone by temporarily faking a silent PCM16 voice so it goes through voice_sample. */
    EnterCriticalSection(&rlock);
    VRec *t = &vrecs[nvrecs]; int save = nvrecs;
    memset(t, 0, sizeof *t); t->fmt = FMT_PCM16; t->ch = 2;
    u32 nfr = 14400; t->q[0].data = malloc(nfr * 4); t->q[0].size = nfr * 4; t->qn = 1;
    for (u32 f = 0; f < nfr; f++) { i16 vv = (i16)(8000 * sinf(2.0f * 3.14159265f * 440.0f * f / ORATE));
        t->q[0].data[4*f] = vv & 0xFF; t->q[0].data[4*f+1] = (vv >> 8) & 0xFF; t->q[0].data[4*f+2] = vv & 0xFF; t->q[0].data[4*f+3] = (vv >> 8) & 0xFF; }
    nvrecs = save + 1;
    LeaveCriticalSection(&rlock);
    Sleep(700);
    EnterCriticalSection(&rlock); nvrecs = save; free(t->q[0].data); LeaveCriticalSection(&rlock);
    long out = st_frames_out - f0, wom = st_womdone - w0;
    int ok = out > NBUF && wom > 0;
    fprintf(stderr, "[audiotest] %s: frames_out+%ld wom_done+%ld underrun=%ld (buffers=%d) -> %s\n",
            ok ? "PASS" : "FAIL", out, wom, (long)st_underrun, NBUF,
            ok ? "device consumes & buffers recycle" : "buffers did NOT recycle");
}

HLE_WRAP(71091796f0) {   /* AcquireVoiceSlot(cfg, VoiceType* voice, id, node, SampleFormat, rate, ptr, size) */
    u64 voice = c->x[1], fmt = c->x[4], rate = c->x[5], ptr = c->x[6], vsz = c->x[7];   /* BEFORE _orig */
    f_71091796f0_orig(c);
    u64 impl = voice ? ld64(voice) : 0;
    int ch = impl ? (int)ld32(impl + 0x58) : 2; if (ch < 1 || ch > 2) ch = 2;
    int f = (int)fmt, rt = (int)rate; if (rt < 8000 || rt > 192000) rt = ORATE;
    if (voice && nvrecs < MAXVOICES - 1) {
        VRec *v = &vrecs[nvrecs++];
        memset(v, 0, sizeof *v); v->vt = voice; v->fmt = f; v->rate = rt; v->ch = ch;
        if (f == FMT_ADPCM && ptr && vsz >= 32 && vsz <= 4096) {
            v->pairs = (int)(vsz / 4); if (v->pairs > 16) v->pairs = 16;
            for (int i = 0; i < v->pairs; i++) { v->coef[i][0] = (i16)ld16(ptr + i * 4); v->coef[i][1] = (i16)ld16(ptr + i * 4 + 2); }
            v->have_coef = 1;
        }
    }
    if (rt_trace) fprintf(stderr, "[audio] voice slot fmt=%d rate=%d ch=%d%s\n", f, rt, ch,
                          f == FMT_PCM16 ? " (PCM16)" : f == FMT_ADPCM ? " (ADPCM+coef)" : " (unsupported)");
}
HLE_WRAP(7109179a6c) {   /* AppendWaveBuffer(VoiceType*, const WaveBuffer*) */
    u64 vt = c->x[0], wb = c->x[1];                             /* BEFORE _orig */
    u64 buf = wb ? ld64(wb) : 0, size = wb ? ld64(wb + 8) : 0;
    int loop = wb ? (int)ld8(wb + 0x18) : 0;                    /* WaveBuffer.loop */
    u32 start = wb ? ld32(wb + 0x10) : 0, end = wb ? ld32(wb + 0x14) : 0;   /* declared sample region */
    f_7109179a6c_orig(c);
    InterlockedIncrement(&st_appends);
    if (getenv("SSBU_AUDIO_SELFTEST")) audio_selftest();
    if (!buf || !size || size > MAXCOPY) return;
    if (!player_up) player_open();
    if (!wout) return;
    for (int i = 0; i < nvrecs; i++) if (vrecs[i].vt == vt) {
        VRec *v = &vrecs[i];
        u32 cap = buf_capacity(v->fmt, v->ch, size);
        /* endSampleOffset is the exclusive upper bound and is always set by the game (measured:
         * fmt6 size=655543 -> end=1147200 == capacity; fmt2 size=23040 -> end=5760 == capacity).
         * If it ever arrives as 0 (unset), treat it as "to end of buffer" but LOG it, so the
         * reinterpretation is observable rather than silent. */
        u32 eff_end = end;
        if (!end) {
            eff_end = cap;
            if (rt_trace || getenv("SSBU_AUDIO_STATS"))
                fprintf(stderr, "[audio] end=0 (unset) voice=%#llx fmt=%d size=%llu -> capacity %u\n", vt, v->fmt, size, cap);
        }
        /* Trust boundary: reject illegal/out-of-range regions observably; do NOT silently clamp. */
        if (!cap || start >= eff_end || eff_end > cap) {
            InterlockedIncrement(&st_reject);
            if (rt_trace || getenv("SSBU_AUDIO_STATS"))
                fprintf(stderr, "[audio] reject voice=%#llx fmt=%d size=%llu start=%u end=%u cap=%u\n",
                        vt, v->fmt, size, start, end, cap);
            return;
        }
        end = eff_end;
        EnterCriticalSection(&rlock);
        if (v->qn >= MAXQBUF) voice_pop_buf(v);                /* bounded: drop oldest if backed up */
        int slot = (v->qh + v->qn) % MAXQBUF;
        v->q[slot].data = malloc((size_t)size);
        if (v->q[slot].data) {
            memcpy(v->q[slot].data, P(void, buf), (size_t)size);
            v->q[slot].size = size; v->q[slot].loop = loop; v->q[slot].start = start; v->q[slot].end = end;
            if (v->qn == 0) {                                   /* head buffer: start at declared start */
                v->spos = start; v->next_frame = (u32)(start / 14); v->ci = v->cachelen = 0;
                if (v->fmt == FMT_ADPCM) {
                    v->h1 = v->h2 = 0;
                    if (start) adpcm_preroll(v, start);         /* non-zero start: correct predictor state */
                    else { v->spos = 0; v->next_frame = 0; }
                    v->spos = start;
                }
            }
            v->qn++;
        }
        LeaveCriticalSection(&rlock);
        if (v->q[slot].data) InterlockedIncrement(&st_submitted);
        return;
    }
}
HLE_WRAP(71091798fc) {   /* ReleaseVoiceSlot(cfg, VoiceType*) */
    u64 voice = c->x[1];
    f_71091798fc_orig(c);
    (void)voice;   /* leaving the record is harmless; its buffers drain then silence */
}

void audio_final_stats(void) {
    wav_headers(&gwav); wav_headers(&twav);
    if (gwav.f) { fclose(gwav.f); gwav.f = NULL; }
    if (twav.f) { fclose(twav.f); twav.f = NULL; }
    if (rt_trace || getenv("SSBU_AUDIO_STATS"))
        fprintf(stderr, "[audio] FINAL: appends=%ld submitted=%ld reject=%ld frames_out=%ld wom_done=%ld underrun=%ld peak=%ld\n",
                (long)st_appends, (long)st_submitted, (long)st_reject, (long)st_frames_out, (long)st_womdone, (long)st_underrun, st_peak);
}

#else
void audio_final_stats(void) {}
#endif
