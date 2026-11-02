// Horizon AudioOut PCM queues, consumed by SDL's playback clock. The SDK client runs unchanged.
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <SDL3/SDL.h>
#include "cfg.h"
#include "host.h"
#include "ipc.h"
#include "platform/platform.h"

#define AUDIO_INVALID 0x499u
#define AUDIO_QUEUE_FULL 0x1099u
#define MAX_BUFFERS 64
#define MAX_PCM_BYTES (16u << 20)
typedef struct Packet { struct Packet *next; uint64_t tag; size_t bytes, pos; int16_t pcm[]; } Packet;
typedef struct {
    PlatMutex lock;
    SDL_AudioStream *stream;
    PlatThread *timer;
    Packet *head, *tail, *released, *released_tail;
    uint32_t event, count, rate, channels;
    int started, quit, sdl, logged;
    float volume;
    uint64_t frames, nonzero;
} AudioOut;
typedef struct { uint64_t next, data, capacity, size, offset; } AudioBuffer;
_Static_assert(sizeof(AudioBuffer) == 0x28, "AudioOutBuffer wire size");

static IpcBuffer output_buffer(const IpcRequest *rq, int *pointer) {
    *pointer = !(rq->nb && rq->b[0].size) && rq->nc && rq->c[0].size;
    return *pointer ? rq->c[0] : rq->nb ? rq->b[0] : (IpcBuffer){0};
}
static void pointer_reply(IpcResponse *rs, IpcBuffer b, int pointer) {
    if (pointer && b.size) rs->xout[rs->nxout++] = b;
}
static void name_reply(const IpcRequest *rq, IpcResponse *rs) {
    int pointer; IpcBuffer b = output_buffer(rq, &pointer);
    if (!b.size) return;
    char *dst = ipc_guest(b.addr, b.size);
    memset(dst, 0, b.size);
    const char name[] = "DeviceOut";
    memcpy(dst, name, b.size < sizeof name ? b.size : sizeof name);
    pointer_reply(rs, b, pointer);
}
static void release_head(AudioOut *a) {
    Packet *p = a->head;
    a->head = p->next;
    if (!a->head) a->tail = NULL;
    p->next = NULL;
    if (a->released_tail) a->released_tail->next = p; else a->released = p;
    a->released_tail = p;
}
// No allocation or logging on SDL's audio callback.
static void consume(AudioOut *a, int16_t *dst, size_t bytes) {
    memset(dst, 0, bytes);
    int signal = 0;
    plat_mutex_lock(&a->lock);
    if (a->started) {
        size_t done = 0;
        while (a->head && done < bytes) {
            Packet *p = a->head;
            size_t take = p->bytes - p->pos;
            if (take > bytes - done) take = bytes - done;
            memcpy((char *)dst + done, (char *)p->pcm + p->pos, take);
            const int16_t *src = (const int16_t *)((char *)p->pcm + p->pos);
            for (size_t i = 0; i < take / 2; i++) if (src[i]) a->nonzero++;
            p->pos += take; done += take;
            a->frames += take / (2 * a->channels);
            if (p->pos == p->bytes) { release_head(a); signal = 1; }
        }
    }
    plat_mutex_unlock(&a->lock);
    if (signal) kern_event_signal(a->event);
}
static void SDLCALL pull(void *user, SDL_AudioStream *stream, int additional, int total) {
    (void)total; AudioOut *a = user; int16_t pcm[6144];
    int frame_bytes = (int)a->channels * 2;
    while (additional >= frame_bytes) {
        int bytes = additional < (int)sizeof pcm ? additional : (int)sizeof pcm;
        bytes -= bytes % frame_bytes;
        consume(a, pcm, (size_t)bytes);
        if (!SDL_PutAudioStreamData(stream, pcm, bytes)) break;
        additional -= bytes;
    }
}
static void silent_clock(void *user) {
    AudioOut *a = user; int16_t pcm[1440];
    for (;;) {
        plat_sleep_ns(5000000);
        plat_mutex_lock(&a->lock); int quit = a->quit; plat_mutex_unlock(&a->lock);
        if (quit) return;
        consume(a, pcm, (size_t)(a->rate / 200) * a->channels * 2);
    }
}
static void gain(AudioOut *a) {
    double pct = cfg_double("TOTK_VOLUME");
    if (pct < 0) pct = 0; if (pct > 100) pct = 100;
    plat_mutex_lock(&a->lock); float volume = a->volume; plat_mutex_unlock(&a->lock);
    if (a->stream) SDL_SetAudioStreamGain(a->stream, volume * (float)(pct / 100));
}
static AudioOut *output_new(uint32_t rate, uint32_t channels) {
    AudioOut *a = calloc(1, sizeof *a);
    if (!a) rt_fatal("audout: allocation failed");
    a->rate = rate; a->channels = channels; a->volume = 1;
    a->event = kern_event_new();
    const char *mode = cfg_get("TOTK_AUDIO");
    if (!(mode && !strcmp(mode, "none")) && !cfg_get("TOTK_AUDIO_OFF")) {
        a->sdl = SDL_InitSubSystem(SDL_INIT_AUDIO);
        if (a->sdl) {
            SDL_AudioSpec spec = {SDL_AUDIO_S16, (int)channels, (int)rate};
            a->stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, pull, a);
        }
        if (!a->stream) rt_log("audout: SDL output unavailable (%s); using a silent clock", SDL_GetError());
    }
    if (!a->stream) {
        a->timer = plat_thread_new(silent_clock, a, "audout silent");
        if (!a->timer) rt_fatal("audout: cannot start silent clock");
    }
    rt_log("audout: opened %u Hz, %u channels, PCM16, %s", rate, channels, a->stream ? "SDL playback" : "silent clock");
    return a;
}
static void output_close(void *state) {
    AudioOut *a = state;
    if (a->stream) SDL_DestroyAudioStream(a->stream);
    plat_mutex_lock(&a->lock); a->quit = 1; plat_mutex_unlock(&a->lock);
    if (a->timer) plat_thread_join(a->timer);
    while (a->head) release_head(a);
    while (a->released) { Packet *p = a->released; a->released = p->next; free(p); }
    kern_handle_close(a->event);
    rt_log("audout: closed after %llu frames, %llu nonzero samples", (unsigned long long)a->frames, (unsigned long long)a->nonzero);
    if (a->sdl) SDL_QuitSubSystem(SDL_INIT_AUDIO);
    free(a);
}
static void output_handler(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    AudioOut *a = s->state;
    switch (rq->cmd) {
    case 0: { plat_mutex_lock(&a->lock); uint32_t state = a->started ? 0 : 1; plat_mutex_unlock(&a->lock); IPC_OUT(rs, state); return; }
    case 1: case 2: {
        plat_mutex_lock(&a->lock); a->started = rq->cmd == 1; plat_mutex_unlock(&a->lock);
        if (a->stream) {
            gain(a);
            if (rq->cmd == 1) { if (!SDL_ResumeAudioStreamDevice(a->stream)) rt_fatal("audout: start failed: %s", SDL_GetError()); }
            else { SDL_PauseAudioStreamDevice(a->stream); SDL_ClearAudioStream(a->stream); }
        }
        return;
    }
    case 3: case 7: {
        IpcBuffer b = rq->na && rq->a[0].size ? rq->a[0] : rq->nx ? rq->x[0] : (IpcBuffer){0};
        if (b.size < sizeof(AudioBuffer)) { rs->result = AUDIO_INVALID; return; }
        AudioBuffer wire; memcpy(&wire, ipc_guest(b.addr, sizeof wire), sizeof wire);
        uint64_t tag = IPC_ARG(rq, uint64_t, 0);
        if (wire.offset > wire.capacity || wire.size > wire.capacity - wire.offset || wire.size > MAX_PCM_BYTES ||
            wire.size % (a->channels * 2) || wire.data > UINT64_MAX - wire.offset) { rs->result = AUDIO_INVALID; return; }
        Packet *p = malloc(sizeof *p + (size_t)wire.size);
        if (!p) rt_fatal("audout: PCM queue allocation failed");
        *p = (Packet){.tag = tag, .bytes = (size_t)wire.size};
        memcpy(p->pcm, ipc_guest(wire.data + wire.offset, wire.size), (size_t)wire.size);
        plat_mutex_lock(&a->lock);
        int full = a->count == MAX_BUFFERS;
        for (Packet *q = a->head; q; q = q->next) full |= q->tag == tag;
        for (Packet *q = a->released; q; q = q->next) full |= q->tag == tag;
        if (!full) { if (a->tail) a->tail->next = p; else a->head = p; a->tail = p; a->count++; }
        plat_mutex_unlock(&a->lock);
        if (full) { free(p); rs->result = AUDIO_QUEUE_FULL; } else gain(a);
        return;
    }
    case 4: rs->copy[rs->ncopy++] = kern_event_readable(a->event); return;
    case 5: case 8: {
        int pointer; IpcBuffer b = output_buffer(rq, &pointer);
        uint64_t *tags = ipc_guest(b.addr, b.size); uint32_t n = 0;
        memset(tags, 0, b.size);
        plat_mutex_lock(&a->lock);
        while (a->released && n < b.size / 8) { Packet *p = a->released; tags[n++] = p->tag; a->released = p->next; a->count--; free(p); }
        if (!a->released) a->released_tail = NULL;
        int log = !a->logged && a->nonzero && (rt_trace & RT_TRACE_AUDIO);
        if (log) a->logged = 1;
        uint64_t frames = a->frames, nonzero = a->nonzero;
        plat_mutex_unlock(&a->lock);
        if (log) rt_log("audout: playback contains sound (%llu frames, %llu nonzero samples)", (unsigned long long)frames, (unsigned long long)nonzero);
        pointer_reply(rs, b, pointer); IPC_OUT(rs, n); return;
    }
    case 6: {
        uint64_t tag = IPC_ARG(rq, uint64_t, 0); uint8_t found = 0;
        plat_mutex_lock(&a->lock);
        for (Packet *p = a->head; p; p = p->next) found |= p->tag == tag;
        plat_mutex_unlock(&a->lock); IPC_OUT(rs, found); return;
    }
    case 9: { plat_mutex_lock(&a->lock); uint32_t n = a->count; plat_mutex_unlock(&a->lock); IPC_OUT(rs, n); return; }
    case 10: { plat_mutex_lock(&a->lock); uint64_t frames = a->frames; plat_mutex_unlock(&a->lock); IPC_OUT(rs, frames); return; }
    case 11: {
        plat_mutex_lock(&a->lock); uint8_t pending = a->head != NULL;
        while (a->head) release_head(a);
        plat_mutex_unlock(&a->lock);
        if (pending) kern_event_signal(a->event);
        IPC_OUT(rs, pending); return;
    }
    case 12: {
        float v = IPC_ARG(rq, float, 0);
        plat_mutex_lock(&a->lock); a->volume = isfinite(v) && v >= 0 ? v : 0; plat_mutex_unlock(&a->lock);
        gain(a); return;
    }
    case 13: { plat_mutex_lock(&a->lock); float v = a->volume; plat_mutex_unlock(&a->lock); IPC_OUT(rs, v); return; }
    default: ipc_unimplemented(s, rq);
    }
}
void audout_manager_handler(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    switch (rq->cmd) {
    case 0: case 2: {
        int pointer; IpcBuffer b = output_buffer(rq, &pointer);
        uint32_t count = b.size >= 0x100;
        if (count) name_reply(rq, rs);
        IPC_OUT(rs, count); return;
    }
    case 1: case 3: {
        uint32_t rate = IPC_ARG(rq, uint32_t, 0);
        uint16_t channels = IPC_ARG(rq, uint16_t, 4);
        if (!rate) rate = 48000;
        if (!channels) channels = 2;
        if (rate != 48000 || (channels != 1 && channels != 2 && channels != 6)) { rs->result = AUDIO_INVALID; return; }
        AudioOut *a = output_new(rate, channels);
        struct { uint32_t rate, channels, format, state; } info = {rate, channels, 2, 1};
        IPC_OUT(rs, info); name_reply(rq, rs);
        rs->move[rs->nmove++] = ipc_session_new("IAudioOut", output_handler, a, output_close); return;
    }
    default: ipc_unimplemented(s, rq);
    }
}

// Exercise IPC buffer ownership without opening a speaker or running a guest game.
int probe_audout(void) {
    int bad = 0;
#define CHECK(x) do { if (!(x)) { rt_log("audout probe failed: %s", #x); bad++; } } while (0)
    unsigned char *scratch = mem_kernel_alloc(4096, 64);
    AudioBuffer *wire = (AudioBuffer *)scratch;
    int16_t *pcm = (int16_t *)(scratch + 64);
    uint64_t *tags = (uint64_t *)(scratch + 128);
    for (int i = 0; i < 8; i++) pcm[i] = (int16_t)(100 + i);
    AudioOut *a = calloc(1, sizeof *a);
    a->rate = 48000; a->channels = 2; a->volume = 1; a->event = kern_event_new();
    IpcSession session = {.state = a};
    uint64_t tag = 123;
    IpcRequest rq = {.cmd = 7, .data = (const uint8_t *)&tag, .data_size = 8, .nx = 1,
                     .x = {{(uint64_t)(uintptr_t)wire, sizeof *wire, 0}}};
    *wire = (AudioBuffer){.data = (uint64_t)(uintptr_t)pcm, .capacity = 16, .size = 16};
    IpcResponse rs = {0}; output_handler(&session, &rq, &rs);
    CHECK(!rs.result && a->count == 1);
    memset(&rs, 0, sizeof rs); output_handler(&session, &rq, &rs);
    CHECK(rs.result == AUDIO_QUEUE_FULL && a->count == 1);
    int16_t out[8]; consume(a, out, sizeof out);
    CHECK(!out[0] && a->frames == 0 && a->head);
    rq.cmd = 1; memset(&rs, 0, sizeof rs); output_handler(&session, &rq, &rs);
    consume(a, out, 8); CHECK(out[0] == 100 && out[3] == 103 && a->frames == 2 && !a->released);
    consume(a, out, sizeof out); CHECK(out[0] == 104 && out[3] == 107 && !out[4] && a->frames == 4 && a->released);
    rq = (IpcRequest){.cmd = 8, .nc = 1, .c = {{(uint64_t)(uintptr_t)tags, 8, 0}}};
    tags[0] = 999; tags[1] = 456; memset(&rs, 0, sizeof rs); output_handler(&session, &rq, &rs);
    uint32_t count = 0; memcpy(&count, rs.data, 4);
    CHECK(count == 1 && tags[0] == 123 && tags[1] == 456 && rs.nxout == 1 && !a->count);
    tags[0] = 999; memset(&rs, 0, sizeof rs); output_handler(&session, &rq, &rs);
    memcpy(&count, rs.data, 4); CHECK(count == 0 && tags[0] == 0 && tags[1] == 456);
    rq = (IpcRequest){.cmd = 3, .data = (const uint8_t *)&tag, .data_size = 8, .na = 1,
                     .a = {{(uint64_t)(uintptr_t)wire, sizeof *wire, 0}}};
    wire->offset = 17; memset(&rs, 0, sizeof rs); output_handler(&session, &rq, &rs);
    CHECK(rs.result == AUDIO_INVALID && !a->count);
    wire->offset = 0; memset(&rs, 0, sizeof rs); output_handler(&session, &rq, &rs); CHECK(!rs.result);
    rq.cmd = 2; output_handler(&session, &rq, &rs); consume(a, out, sizeof out);
    CHECK(out[0] == 0 && a->head && a->frames == 4);
    rq.cmd = 11; memset(&rs, 0, sizeof rs); output_handler(&session, &rq, &rs);
    CHECK(rs.data[0] == 1 && !a->head && a->released);
    output_close(a); mem_kernel_free(scratch, 4096);
    rt_log("audout probe: %s", bad ? "FAILED" : "passed");
#undef CHECK
    return bad ? 1 : 0;
}
