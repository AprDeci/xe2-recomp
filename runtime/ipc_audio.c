/* Audio output (audout:u), natively, played through the Windows waveOut API (SDL on Linux).
 *
 * The game mixes its own sound and streams 48 kHz stereo 16-bit PCM through the SDK's
 * AudioOut interface. Each buffer it appends is copied to the sound card; when the card has
 * played it, the buffer is reported back as released and the buffer event fires - so audio
 * paces itself exactly as on the console. */
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ipc.h"
#ifdef _WIN32
#include <mmsystem.h>   /* after windows.h */
#else
#include <SDL.h>
#endif

#define RATE 48000
#define CHANNELS 2
#define MAX_BUFFERS 64

#ifdef _WIN32

typedef struct Pending { WAVEHDR hdr; u64 tag; int used; } Pending;
static struct {
    HWAVEOUT out;
    Pending pending[MAX_BUFFERS];
    u64 released[MAX_BUFFERS * 2];
    u32 nreleased;
    u32 event;
    int started;
    u64 played_samples;
    f32 volume;
    CRITICAL_SECTION lock;
} audio = {.volume = 1.0f};

/* waveOut calls back on its own thread while holding its internal lock, and waveOutWrite
 * takes that lock - so taking audio.lock in the callback deadlocks against an append. The
 * callback only wakes the reaper, which collects the played buffers. */
static HANDLE played_event;

static void CALLBACK wave_done(HWAVEOUT h, UINT msg, DWORD_PTR user, DWORD_PTR p1, DWORD_PTR p2) {
    (void)h; (void)user; (void)p1; (void)p2;
    if (msg == WOM_DONE) SetEvent(played_event);
}

static DWORD WINAPI reaper(void *arg) {
    (void)arg;
    for (;;) {
        WaitForSingleObject(played_event, INFINITE);
        int any = 0;
        EnterCriticalSection(&audio.lock);
        for (u32 i = 0; i < MAX_BUFFERS; i++) {
            Pending *p = &audio.pending[i];
            if (p->used != 1 || !(p->hdr.dwFlags & WHDR_DONE)) continue;
            p->used = 2;   /* done; unprepared and recycled on the next append */
            if (audio.nreleased < MAX_BUFFERS * 2) audio.released[audio.nreleased++] = p->tag;
            audio.played_samples += p->hdr.dwBufferLength / (2 * CHANNELS);
            any = 1;
        }
        LeaveCriticalSection(&audio.lock);
        if (any) kernel_signal_event(audio.event);
    }
}

static void open_device(void) {
    static int opened;
    if (opened++) return;
    InitializeCriticalSection(&audio.lock);
    played_event = CreateEventA(NULL, FALSE, FALSE, NULL);
    CloseHandle(CreateThread(NULL, 0, reaper, NULL, 0, NULL));
    WAVEFORMATEX fmt = {WAVE_FORMAT_PCM, CHANNELS, RATE, RATE * 2 * CHANNELS, 2 * CHANNELS, 16, 0};
    if (waveOutOpen(&audio.out, WAVE_MAPPER, &fmt, (DWORD_PTR)wave_done, 0, CALLBACK_FUNCTION) != MMSYSERR_NOERROR) {
        fprintf(stderr, "[audio] no audio device; sound is dropped\n");
        audio.out = NULL;
    }
    audio.event = kernel_create_event();
}

/* Append { u64 next; u64 address; u64 capacity; u64 size; u64 offset } with a 64-bit tag. */
static void append(u64 desc, u64 tag) {
    u64 addr = ld64(desc + 8), size = ld64(desc + 24), offset = ld64(desc + 32);
    EnterCriticalSection(&audio.lock);
    Pending *p = NULL;
    for (u32 i = 0; i < MAX_BUFFERS && !p; i++) {
        if (audio.pending[i].used == 2) {
            waveOutUnprepareHeader(audio.out, &audio.pending[i].hdr, sizeof(WAVEHDR));
            free(audio.pending[i].hdr.lpData);
            audio.pending[i].used = 0;
        }
        if (!audio.pending[i].used) p = &audio.pending[i];
    }
    if (!p || !audio.out) {   /* no device (or saturated): hand the buffer straight back */
        if (audio.nreleased < MAX_BUFFERS * 2) audio.released[audio.nreleased++] = tag;
        LeaveCriticalSection(&audio.lock);
        kernel_signal_event(audio.event);
        return;
    }
    memset(&p->hdr, 0, sizeof p->hdr);
    p->hdr.lpData = malloc(size ? size : 1);
    memcpy(p->hdr.lpData, P(void, addr + offset), size);
    if (audio.volume != 1.0f) {
        i16 *s = (i16 *)p->hdr.lpData;
        for (u64 i = 0; i < size / 2; i++) s[i] = (i16)(s[i] * audio.volume);
    }
    p->hdr.dwBufferLength = (DWORD)size;
    p->tag = tag;
    p->used = 1;
    waveOutPrepareHeader(audio.out, &p->hdr, sizeof p->hdr);
    waveOutWrite(audio.out, &p->hdr, sizeof p->hdr);
    LeaveCriticalSection(&audio.lock);
}

#else
/* Linux: SDL plays the buffers - its callback takes them in order, and a reaper reports each
 * one played (as waveOut's completion does on Windows), outside the audio thread. */
typedef struct Pending { u8 *data; u32 size, pos; u64 tag; int used; } Pending;   /* used: 1 queued, 2 played */
static struct {
    SDL_AudioDeviceID out;
    Pending pending[MAX_BUFFERS];
    u32 order[MAX_BUFFERS], head, count;   /* queued slots, oldest first */
    u64 released[MAX_BUFFERS * 2];
    u32 nreleased;
    u32 event;
    int started;
    u64 played_samples;
    f32 volume;
    CRITICAL_SECTION lock;
} audio = {.volume = 1.0f};
static HANDLE played_event;

static void SDLCALL feed(void *user, Uint8 *out, int len) {
    (void)user;
    int any = 0;
    EnterCriticalSection(&audio.lock);
    while (len > 0) {
        if (!audio.count) { memset(out, 0, (size_t)len); break; }   /* (nothing queued: silence) */
        Pending *p = &audio.pending[audio.order[audio.head]];
        u32 n = p->size - p->pos < (u32)len ? p->size - p->pos : (u32)len;
        memcpy(out, p->data + p->pos, n);
        out += n; len -= (int)n; p->pos += n;
        if (p->pos >= p->size) {
            p->used = 2;
            audio.head = (audio.head + 1) % MAX_BUFFERS;
            audio.count--;
            any = 1;
        }
    }
    LeaveCriticalSection(&audio.lock);
    if (any) SetEvent(played_event);
}

static DWORD WINAPI reaper(void *arg) {
    (void)arg;
    SetThreadDescription(GetCurrentThread(), L"audio reaper");
    for (;;) {
        WaitForSingleObject(played_event, INFINITE);
        int any = 0;
        EnterCriticalSection(&audio.lock);
        for (u32 i = 0; i < MAX_BUFFERS; i++) {
            Pending *p = &audio.pending[i];
            if (p->used != 2) continue;
            if (audio.nreleased < MAX_BUFFERS * 2) audio.released[audio.nreleased++] = p->tag;
            audio.played_samples += p->size / (2 * CHANNELS);
            free(p->data);
            memset(p, 0, sizeof *p);
            any = 1;
        }
        LeaveCriticalSection(&audio.lock);
        if (any) kernel_signal_event(audio.event);
    }
}

static void open_device(void) {
    static int opened;
    if (opened++) return;
    InitializeCriticalSection(&audio.lock);
    played_event = CreateEventA(NULL, FALSE, FALSE, NULL);
    CloseHandle(CreateThread(NULL, 0, reaper, NULL, 0, NULL));
    SDL_AudioSpec want, got;
    memset(&want, 0, sizeof want);
    want.freq = RATE;
    want.format = AUDIO_S16SYS;
    want.channels = CHANNELS;
    want.samples = 256;   /* (the game keeps a few short buffers queued: small pulls keep up with it) */
    want.callback = feed;
    if (!SDL_WasInit(SDL_INIT_AUDIO)) SDL_InitSubSystem(SDL_INIT_AUDIO);
    audio.out = SDL_OpenAudioDevice(NULL, 0, &want, &got, 0);
    if (!audio.out) fprintf(stderr, "[audio] no audio device (%s); sound is dropped\n", SDL_GetError());
    else {
        fprintf(stderr, "[audio] SDL %s output, %d Hz, %d-sample periods\n", SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver() : "?", got.freq, got.samples);
        SDL_PauseAudioDevice(audio.out, 0);
    }
    audio.event = kernel_create_event();
}

static void append(u64 desc, u64 tag) {
    u64 addr = ld64(desc + 8), size = ld64(desc + 24), offset = ld64(desc + 32);
    EnterCriticalSection(&audio.lock);
    Pending *p = NULL;
    u32 slot = 0;
    for (u32 i = 0; i < MAX_BUFFERS && !p; i++) if (!audio.pending[i].used) { p = &audio.pending[i]; slot = i; }
    if (!p || !audio.out || !size || audio.count == MAX_BUFFERS) {   /* no device (or saturated): hand the buffer straight back */
        if (audio.nreleased < MAX_BUFFERS * 2) audio.released[audio.nreleased++] = tag;
        LeaveCriticalSection(&audio.lock);
        kernel_signal_event(audio.event);
        return;
    }
    p->data = malloc(size);
    memcpy(p->data, P(void, addr + offset), size);
    if (audio.volume != 1.0f) {
        i16 *s = (i16 *)p->data;
        for (u64 i = 0; i < size / 2; i++) s[i] = (i16)(s[i] * audio.volume);
    }
    p->size = (u32)size;
    p->pos = 0;
    p->tag = tag;
    p->used = 1;
    audio.order[(audio.head + audio.count) % MAX_BUFFERS] = slot;
    audio.count++;
    LeaveCriticalSection(&audio.lock);
}
#endif

static void audio_out(IpcObject *self, Ipc *ipc) {
    u64 size, b;
    switch (ipc->command) {
    case 0: IPC_PUSH(ipc, (u32)(audio.started ? 0 : 1)); return;   /* GetAudioOutState */
    case 1: audio.started = 1; return;                             /* Start */
    case 2: audio.started = 0; return;                             /* Stop */
    case 3: case 7:                                                /* AppendAudioOutBuffer(Auto)(tag) */
        b = ipc_in_buffer(ipc, 0, &size);
        if (rt_trace) {
            fprintf(stderr, "[audio] append: %u raw bytes:", ipc->in_size);
            for (u32 i = 0; i < ipc->in_size && i < 32; i += 4) fprintf(stderr, " %08x", ipc_u32(ipc, i));
            fprintf(stderr, "; buffer %#llx (%llu bytes):", b, size);
            for (u64 i = 0; b && i < size && i < 40; i += 8) fprintf(stderr, " %llx", ld64(b + i));
            fprintf(stderr, " (a %u, x %u)\n", ipc->na, ipc->nx);
        }
        if (b && size >= 40) append(b, ipc_u64(ipc, 0));
        return;
    case 4: ipc_copy_handle(ipc, audio.event); return;            /* RegisterBufferEvent */
    case 5: case 8: {                                              /* GetReleasedAudioOutBuffers(Auto) */
        b = ipc_out_buffer(ipc, 0, &size);
        if (b) memset(P(void, b), 0, size);   /* the client reads the first entry whatever the count */
        EnterCriticalSection(&audio.lock);
        u32 n = 0;
        while (n < audio.nreleased && (n + 1) * 8 <= size) { st64(b + 8ull * n, audio.released[n]); n++; }
        memmove(audio.released, audio.released + n, (audio.nreleased - n) * 8);
        audio.nreleased -= n;
        LeaveCriticalSection(&audio.lock);
        if (rt_trace)
            fprintf(stderr, "[audio] released %u (buffer %#llx, %llu bytes; b %u (%#llx), c %u (%#llx))\n", n, b, size,
                    ipc->nb, ipc->nb ? ipc->b[0].addr : 0, ipc->nc, ipc->nc ? ipc->c[0].addr : 0);
        ipc_wrote(ipc, 0, size);
        IPC_PUSH(ipc, n);
        return;
    }
    case 6: IPC_PUSH(ipc, (u32)0); return;                         /* ContainsAudioOutBuffer */
    case 9: {                                                      /* GetAudioOutBufferCount */
        u32 n = 0;
        for (u32 i = 0; i < MAX_BUFFERS; i++) n += audio.pending[i].used == 1;
        IPC_PUSH(ipc, n);
        return;
    }
    case 10: IPC_PUSH(ipc, audio.played_samples); return;          /* GetAudioOutPlayedSampleCount */
    case 11: IPC_PUSH(ipc, (u8)0); return;                         /* FlushAudioOutBuffers */
    case 12: memcpy(&audio.volume, ipc->in, 4); return;            /* SetAudioOutVolume */
    case 13: ipc_push(ipc, &audio.volume, 4); return;              /* GetAudioOutVolume */
    }
    ipc_unknown(ipc, self);
}

static void manager(IpcObject *self, Ipc *ipc) {
    u64 size, b;
    switch (ipc->command) {
    case 0: case 2:   /* ListAudioOuts(Auto): one device */
        b = ipc_out_buffer(ipc, 0, &size);
        if (b && size >= 0x100) { memset(P(void, b), 0, 0x100); memcpy(P(void, b), "DeviceOut", 10); ipc_wrote(ipc, 0, 0x100); }
        IPC_PUSH(ipc, (u32)1);
        return;
    case 1: case 3: { /* OpenAudioOut(Auto)(parameter, applet) -> rate, channels, format, state; IAudioOut */
        open_device();
        b = ipc_out_buffer(ipc, 0, &size);
        if (b && size >= 0x100) { memset(P(void, b), 0, 0x100); memcpy(P(void, b), "DeviceOut", 10); ipc_wrote(ipc, 0, 0x100); }
        u32 out[4] = {RATE, CHANNELS, 2 /* PCM int16 */, 1 /* stopped */};
        ipc_push(ipc, out, sizeof out);
        ipc_object(ipc, ipc_new_object("audout:IAudioOut", audio_out, NULL));
        return;
    }
    }
    ipc_unknown(ipc, self);
}

/* ---- audren:u: only its audio-device interface is used (device name, volume, events) -- */
#define DEVICE "AudioTvOutput"

static void put_name(Ipc *ipc, u32 index) {
    u64 size, b = ipc_out_buffer(ipc, index, &size);
    if (b && size >= 0x100) {
        memset(P(void, b), 0, 0x100);
        memcpy(P(void, b), DEVICE, sizeof DEVICE);
        ipc_wrote(ipc, index, 0x100);
    }
}

static void audio_device(IpcObject *self, Ipc *ipc) {
    static u32 event;
    if (!event) event = kernel_create_event();
    switch (ipc->command) {
    case 0: case 6: put_name(ipc, 0); IPC_PUSH(ipc, (u32)1); return;   /* ListAudioDeviceName(Auto) */
    case 1: case 7: return;                                            /* SetAudioDeviceOutputVolume(Auto) */
    case 2: case 8: { f32 v = 1.0f; ipc_push(ipc, &v, 4); return; }   /* GetAudioDeviceOutputVolume(Auto) */
    case 3: case 9: put_name(ipc, 0); return;                          /* GetActiveAudioDeviceName(Auto) */
    case 4: case 10: case 11: ipc_copy_handle(ipc, event); return;     /* system / input / output events */
    case 5: IPC_PUSH(ipc, (u32)CHANNELS); return;                      /* GetActiveChannelCount */
    }
    ipc_unknown(ipc, self);
}

/* ---- audren:IAudioRenderer: the renderer OpenAudioRenderer hands out --------------------
 * XC2 mixes in software and drives the renderer through RequestUpdate/Start. There is no real
 * mixing state on the host, so the renderer reports the parameter's sample rate / counts and
 * Start/Stop flip state; audio is discarded. The heavy lifting (the validated RequestUpdate
 * round trip) is short-circuited at the SDK export below, since reproducing nn::audio's whole
 * out-status ABI on the server side would mean reimplementing its memory-pool/voice/sink/
 * effect/behaviour managers. */
typedef struct Renderer {
    u32 sample_rate, sample_count, mix_buffer_count;
    int started;
    u32 event;
    volatile int alive;
} Renderer;

static Renderer *g_renderer;   /* the one renderer OpenAudioRenderer made, for the SDK exports */

/* On the console the renderer paces the game's audio loop: the game waits on the renderer's
 * system event, then appends more wave buffers and asks for the next update. There is no DSP
 * here, so the runtime supplies that pacing itself - a host thread signals the event once per
 * render frame (sample_count / sample_rate) - and the game's audio thread runs, handing us the
 * source PCM it would have mixed. */
static DWORD WINAPI renderer_clock(void *arg) {
    Renderer *r = arg;
    u32 frame_ms = r->sample_rate && r->sample_count ? (u32)(1000ull * r->sample_count / r->sample_rate) : 5;
    if (frame_ms < 1) frame_ms = 1;
    if (rt_trace) fprintf(stderr, "[audio] renderer clock started (event handle %#x, %u ms)\n", r->event, frame_ms);
    long n = 0;
    while (r->alive) {
        kernel_signal_event(r->event);
        if (rt_trace && ++n % 250 == 0) fprintf(stderr, "[audio] renderer clock signalled %ld times\n", n);
        Sleep(frame_ms);
    }
    return 0;
}

/* Short-circuit the SDK's RequestUpdateAudioRenderer/Start/Stop: the renderer is silent and
 * idle on the host, so the client-side config never needs to reach a server. This is the same
 * idea as the language fix - satisfy the SDK at its own export instead of faking a service.
 * The renderer clock (below) is what paces the game's audio loop, so it is started here (the
 * SDK export is reached before the IPC Start command, and the handle is a client proxy, so a
 * global pointer to the one renderer is the simplest way to reach it). */
static void renderer_start(Renderer *r) {
    if (!r || r->started) return;
    r->started = 1;
    r->alive = 1;
    CloseHandle(CreateThread(NULL, 0, renderer_clock, r, 0, NULL));
}
HLE(_ZN2nn5audio26RequestUpdateAudioRendererENS0_19AudioRendererHandleEPKNS0_19AudioRendererConfigE) {
    static long n;
    if (rt_trace && (++n <= 8 || n % 200 == 0)) fprintf(stderr, "[audio] RequestUpdateAudioRenderer #%ld\n", n);
    renderer_start(g_renderer);
    c->x[0] = 0;
}
HLE(_ZN2nn5audio18StartAudioRendererENS0_19AudioRendererHandleE) {
    if (rt_trace) fprintf(stderr, "[audio] StartAudioRenderer\n");
    renderer_start(g_renderer);
    c->x[0] = 0;
}
HLE(_ZN2nn5audio17StopAudioRendererENS0_19AudioRendererHandleE) {
    if (rt_trace) fprintf(stderr, "[audio] StopAudioRenderer\n");
    if (g_renderer) g_renderer->alive = 0, g_renderer->started = 0;
    c->x[0] = 0;
}

static void audio_renderer(IpcObject *self, Ipc *ipc) {
    Renderer *r = self->state;
    switch (ipc->command) {
    case 0: IPC_PUSH(ipc, (i32)r->sample_rate); return;                 /* GetSampleRate */
    case 1: IPC_PUSH(ipc, (i32)r->sample_count); return;               /* GetSampleCount */
    case 2: IPC_PUSH(ipc, (i32)r->mix_buffer_count); return;           /* GetMixBufferCount */
    case 3: IPC_PUSH(ipc, (i32)(r->started ? 1 : 0)); return;          /* GetState */
    case 4: case 10: {                                                 /* RequestUpdate(Auto) */
        u64 outsz = 0, out = ipc_out_buffer(ipc, 0, &outsz);
        if (out && outsz) { memset(P(void, out), 0, (size_t)outsz); ipc_wrote(ipc, 0, outsz); }
        u64 o2sz = 0, o2 = ipc_out_buffer(ipc, 1, &o2sz);
        if (o2 && o2sz) ipc_wrote(ipc, 1, o2sz);
        return;
    }
    case 5:                                                            /* Start */
        if (!r->started) {
            r->started = 1;
            r->alive = 1;
            CloseHandle(CreateThread(NULL, 0, renderer_clock, r, 0, NULL));
        }
        return;
    case 6: r->started = 0; r->alive = 0; return;                       /* Stop */
    case 7: ipc_copy_handle(ipc, r->event); return;                    /* QuerySystemEvent */
    case 8: return;                                                    /* SetRenderingTimeLimit */
    case 9: IPC_PUSH(ipc, (i32)10); return;                            /* GetRenderingTimeLimit (10%) */
    case 11: return;                                                   /* ExecuteAudioRendererRendering */
    }
    ipc_unknown(ipc, self);
}

static void renderer_manager(IpcObject *self, Ipc *ipc) {
    switch (ipc->command) {
    case 0: case 3: {                                                  /* OpenAudioRenderer(ForManual) */
        u64 sz = 0, in = ipc_in_buffer(ipc, 0, &sz);
        Renderer *r = calloc(1, sizeof *r);
        r->sample_rate = 48000; r->sample_count = 0x100; r->mix_buffer_count = 6;
        if (in && sz >= 12) {                                          /* AudioRendererParameterInternal */
            u32 rate = ld32(in + 0), count = ld32(in + 4), mix = ld32(in + 8);
            if (rate >= 8000 && rate <= 192000) r->sample_rate = rate;
            if (count) r->sample_count = count;
            if (mix) r->mix_buffer_count = mix;
        }
        r->event = kernel_create_event();
        g_renderer = r;
        if (rt_trace)
            fprintf(stderr, "[audio] OpenAudioRenderer: rate=%u count=%u mix=%u\n",
                    r->sample_rate, r->sample_count, r->mix_buffer_count);        ipc_object(ipc, ipc_new_object("audren:IAudioRenderer", audio_renderer, r));
        return;
    }
    case 1: IPC_PUSH(ipc, (u64)0x10000); return;                       /* GetWorkBufferSize */
    case 2:                                                            /* GetAudioDeviceService */
        ipc_object(ipc, ipc_new_object("audren:IAudioDevice", audio_device, NULL));
        return;
    }
    ipc_unknown(ipc, self);
}

IpcObject *service_audio(const char *name) {
    if (!strcmp(name, "audout:u")) return ipc_new_object("audout:u", manager, NULL);
    if (!strcmp(name, "audren:u")) return ipc_new_object("audren:u", renderer_manager, NULL);
    return NULL;
}
