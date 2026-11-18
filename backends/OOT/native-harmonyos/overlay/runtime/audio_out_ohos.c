/* audout:u — nn::audio AudioOut on HarmonyOS. Pokémon Legends: Z-A's Wwise sends its final mix through AudioOut
   (OpenDefaultAudioOut, Append / GetReleased buffers, GetAudioOutBufferCount from its sink wakeup thread); Tears of the
   Kingdom only used the audio renderer (audren:u), so this runtime had no AudioOut server.
   From MacNativeGames games/xcx/runtime/audio_out.c (the same IPC framework; commands and layouts from switchbrew's
   "Audio services": IAudioOutManager, IAudioOut), with Core Audio's AudioQueue replaced by an OHAudio renderer:
   appended buffers wait in a queue; the renderer's write callback (the system's audio thread) copies their samples as
   stereo PCM16 (5.1 mixed down) and, when a buffer has been read to its end, moves its tag to the released list and
   signals the buffer event, so the game is paced by the real output. GetAudioOutBufferCount answers appended and not yet
   played (the one playing included), which Wwise's sink reads after every release (nxnative's Z-A note).
   LZA_AUDIO=0: the silent real-time sink (each buffer held for its duration), for runs without sound.
   Every 10 s a [lza-audio] line reports samples played, frames of underrun, the peak level and the queue it saw. */
#include "runtime.h"
#include "ipc.h"
#include <ohaudio/native_audiostreambuilder.h>
#include <ohaudio/native_audiorenderer.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum { STATE_STARTED = 0, STATE_STOPPED = 1, FORMAT_PCM16 = 2, DEFAULT_RATE = 48000, QUEUE_MAX = 32 };
typedef struct { uint64_t next, sample_buffer, capacity, data_size, data_offset; } AudioOutBuffer;   /* the client's descriptor */
_Static_assert(sizeof(AudioOutBuffer) == 0x28, "AudioOutBuffer");
typedef struct { uint64_t tag, bytes, samples, read; } Queued;   /* samples: guest address of the PCM16 data; read: bytes consumed */
typedef struct {
    pthread_mutex_t lock; pthread_t thread; int running, silent;
    uint32_t rate, channels, state, event; float volume;
    OH_AudioRenderer *unit; OH_AudioStreamBuilder *builder;
    Queued queue[QUEUE_MAX]; unsigned nqueue;          /* appended, not yet played (head first) */
    uint64_t released[QUEUE_MAX]; unsigned nreleased;  /* played, not yet collected by GetReleasedAudioOutBuffers */
    uint64_t played_samples, underrun_frames;
    uint64_t report_ns; int peak;                      /* the 10 s report of pull() */
    uint32_t last_bytes, max_queue, cb_frames;         /* and the queue it saw */
} AudioOut;

static uint64_t now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }
static void need(int ok, const char *what) { if (!ok) runtime_fail(what); }

/* The head of the queue has been played: hand its tag back. Called with the lock held. */
static void release_head(AudioOut *o) {
    if (!o->nqueue) return;
    const Queued head = o->queue[0];
    memmove(o->queue, o->queue + 1, (--o->nqueue) * sizeof *o->queue);
    if (o->nreleased < QUEUE_MAX) o->released[o->nreleased++] = head.tag;
    o->played_samples += head.bytes / (2ull * o->channels);
}
static int16_t clamp16(float v) { return (int16_t)(v > 32767.f ? 32767.f : v < -32768.f ? -32768.f : v); }
/* One frame of the client's layout -> stereo, with the AudioOut's volume. */
static void frame_to_stereo(const int16_t *s, uint32_t channels, float vol, int16_t *lr) {
    if (channels == 2) { lr[0] = clamp16(s[0] * vol); lr[1] = clamp16(s[1] * vol); return; }
    if (channels == 1) { lr[0] = lr[1] = clamp16(s[0] * vol); return; }
    float c = s[2] * 0.7071f, l = (s[0] + c + s[4] * 0.7071f) * 0.5f * vol, r = (s[1] + c + s[5] * 0.7071f) * 0.5f * vol;   /* 5.1 (FL FR FC LFE BL BR), LFE left out */
    lr[0] = clamp16(l); lr[1] = clamp16(r);
}
/* LZA_AUDIO_CAPTURE=<seconds>: what the speaker gets (stereo PCM16, 48 kHz, underruns as silence), from the output's
   start, kept in memory and written once to za-audio.pcm in the working folder (files/android) by a thread of its own. */
static int16_t *g_cap; static size_t g_cap_frames, g_cap_n; static int g_cap_state;   /* 0 unknown, 1 capturing, 2 done / off */
static void *cap_write(void *arg) {
    (void)arg; FILE *f = fopen("za-audio.pcm", "wb");
    size_t n = f ? fwrite(g_cap, 4, g_cap_n, f) : 0; if (f) fclose(f);
    fprintf(stderr, "[lza-audio] capture %s: %zu frames (%.1f s) in za-audio.pcm\n", n == g_cap_n ? "written" : "failed", g_cap_n, g_cap_n / 48000.0);
    free(g_cap); g_cap = NULL; return NULL;
}
static void capture(const int16_t *lr, uint32_t frames) {
    if (g_cap_state == 0) {
        const char *e = getenv("LZA_AUDIO_CAPTURE"); int s = e && *e ? atoi(e) : 0;
        g_cap_frames = s > 0 && s <= 600 ? (size_t)s * 48000 : 0;
        g_cap = g_cap_frames ? malloc(g_cap_frames * 4) : NULL; g_cap_state = g_cap ? 1 : 2;
    }
    if (g_cap_state != 1) return;
    size_t k = frames < g_cap_frames - g_cap_n ? frames : g_cap_frames - g_cap_n;
    memcpy(g_cap + 2 * g_cap_n, lr, k * 4); g_cap_n += k;
    if (g_cap_n == g_cap_frames) { g_cap_state = 2; pthread_t t; if (!pthread_create(&t, NULL, cap_write, NULL)) pthread_detach(t); }
}
/* OHAudio write callback: stereo PCM16 frames from the queue; buffers read to their end are released. */
static OH_AudioData_Callback_Result pull(OH_AudioRenderer *r, void *ctx, void *data, int32_t bytes) {
    (void)r; AudioOut *o = ctx; int16_t *out = data; uint32_t frames = (uint32_t)bytes / 4u, done = 0; int signal = 0;
    pthread_mutex_lock(&o->lock);
    const uint32_t fb = 2u * o->channels;
    while (done < frames && o->state == STATE_STARTED && o->nqueue) {
        Queued *q = &o->queue[0];
        while (done < frames && q->read + fb <= q->bytes) {
            frame_to_stereo((const int16_t *)(uintptr_t)(q->samples + q->read), o->channels, o->volume, out + 2 * done);
            q->read += fb; done++;
        }
        if (q->read + fb > q->bytes) { release_head(o); signal = 1; }
    }
    if (done < frames) o->underrun_frames += frames - done;
    o->cb_frames = frames;
    /* every 10 s: what reached the speaker (played samples, frames of underrun, the loudest sample since the last line) */
    int peak = 0; for (uint32_t i = 0; i < 2 * done; i++) { int v = out[i] < 0 ? -out[i] : out[i]; if (v > peak) peak = v; }
    if (peak > o->peak) o->peak = peak;
    uint64_t t = now_ns();
    if (!o->report_ns) o->report_ns = t;
    else if (t - o->report_ns >= 10000000000ull) {
        fprintf(stderr, "[lza-audio] played %llu samples, %llu frames of underrun; peak %d of 32767 in the last %.0f s; "
                "buffers of %u bytes, up to %u queued, callbacks of %u frames\n",
                (unsigned long long)o->played_samples, (unsigned long long)o->underrun_frames, o->peak, (t - o->report_ns) / 1e9,
                o->last_bytes, o->max_queue, o->cb_frames);
        o->report_ns = t; o->peak = 0; o->max_queue = 0;
    }
    pthread_mutex_unlock(&o->lock);
    if (done < frames) memset(out + 2 * done, 0, (size_t)(frames - done) * 4);
    if (o->rate == 48000) capture(out, frames);
    if (signal) runtime_event_signal(o->event);
    return AUDIO_DATA_CALLBACK_RESULT_VALID;
}
static void *silent_playback(void *arg) {
    AudioOut *o = arg; uint64_t due = 0;
    for (;;) {
        pthread_mutex_lock(&o->lock);
        if (!o->running) { pthread_mutex_unlock(&o->lock); return NULL; }
        uint64_t now = now_ns();
        if (o->state == STATE_STARTED && o->nqueue) {
            if (!due) due = now + o->queue[0].bytes / (2ull * o->channels) * 1000000000ull / o->rate;
            if (now >= due) { release_head(o); due = 0; pthread_mutex_unlock(&o->lock); runtime_event_signal(o->event); continue; }
        } else due = 0;
        pthread_mutex_unlock(&o->lock);
        struct timespec ts = {0, 1000000}; nanosleep(&ts, NULL);
    }
}
static void open_device(AudioOut *o) {
    const char *e = getenv("LZA_AUDIO");
    o->silent = e && !strcmp(e, "0");
    if (!o->silent) {
        int r = OH_AudioStreamBuilder_Create(&o->builder, AUDIOSTREAM_TYPE_RENDERER);
        if (!r) r = OH_AudioStreamBuilder_SetSamplingRate(o->builder, (int32_t)o->rate);
        if (!r) r = OH_AudioStreamBuilder_SetChannelCount(o->builder, 2);
        if (!r) r = OH_AudioStreamBuilder_SetSampleFormat(o->builder, AUDIOSTREAM_SAMPLE_S16LE);
        if (!r) r = OH_AudioStreamBuilder_SetEncodingType(o->builder, AUDIOSTREAM_ENCODING_TYPE_RAW);
        if (!r) r = OH_AudioStreamBuilder_SetLatencyMode(o->builder, AUDIOSTREAM_LATENCY_MODE_NORMAL);
        /* Callback size: the normal path asked for 4458 frames at a time (Maleoon phone, 2026-11-09), more than Wwise
           ever queues (4 buffers of 1024 frames), so every callback ran dry: 10-15 % of the output was silence. One
           device period (960 frames, 20 ms) is taken per callback instead; LZA_AUDIO_FRAMES overrides, 0 leaves it. */
        if (!r) {
            const char *fe = getenv("LZA_AUDIO_FRAMES"); int frames = fe && *fe ? atoi(fe) : 960;
            if (frames > 0) {
                int fr = OH_AudioStreamBuilder_SetFrameSizeInCallback(o->builder, frames);
                fprintf(stderr, "[lza-audio] callbacks of %d frames%s\n", frames, fr ? " refused: the system's size" : "");
            }
        }
        if (!r) r = OH_AudioStreamBuilder_SetRendererInfo(o->builder, AUDIOSTREAM_USAGE_GAME);
        if (!r) r = OH_AudioStreamBuilder_SetRendererWriteDataCallback(o->builder, pull, o);
        if (!r) r = OH_AudioStreamBuilder_GenerateRenderer(o->builder, &o->unit);
        if (r) {
            fprintf(stderr, "[lza-audio] 【红】AudioOut: OHAudio renderer setup failed (%d); continuing with the silent sink\n", r);
            if (o->builder) { OH_AudioStreamBuilder_Destroy(o->builder); o->builder = NULL; }
            o->unit = NULL; o->silent = 1;
        }
    }
    if (o->silent) need(pthread_create(&o->thread, NULL, silent_playback, o) == 0, "AudioOut playback thread");
}

static const IpcBuffer *in_buffer(const IpcRequest *rq) { return rq->na && rq->a[0].size ? &rq->a[0] : rq->nx && rq->x[0].size ? &rq->x[0] : NULL; }
static const IpcBuffer *out_buffer(const IpcRequest *rq, int *pointer) {
    if (rq->nb && rq->b[0].size) { *pointer = 0; return &rq->b[0]; }
    if (rq->nc && rq->c[0].size) { *pointer = 1; return &rq->c[0]; }
    return NULL;
}
static void append(AudioOut *o, const IpcRequest *rq) {
    const IpcBuffer *b = in_buffer(rq); need(b && b->size >= sizeof(AudioOutBuffer), "AppendAudioOutBuffer descriptor");
    AudioOutBuffer d; memcpy(&d, ipc_guest(b->addr, sizeof d), sizeof d);
    uint64_t tag = IPC_ARG(rq, uint64_t, 0);
    need(d.data_offset + d.data_size <= d.capacity || !d.capacity, "AudioOut buffer bounds");
    if (d.data_size) (void)ipc_guest(d.sample_buffer + d.data_offset, d.data_size);
    pthread_mutex_lock(&o->lock);
    need(o->nqueue < QUEUE_MAX, "AudioOut queue full");
    o->queue[o->nqueue++] = (Queued){tag, d.data_size, d.sample_buffer + d.data_offset, 0};
    o->last_bytes = (uint32_t)d.data_size; if (o->nqueue > o->max_queue) o->max_queue = o->nqueue;
    int empty_now = !d.data_size && o->nqueue == 1 && !o->silent;   /* an empty buffer still has to come back */
    if (empty_now) release_head(o);
    pthread_mutex_unlock(&o->lock);
    if (empty_now) runtime_event_signal(o->event);
}
static void released(AudioOut *o, const IpcRequest *rq, IpcResponse *rs) {
    int pointer; const IpcBuffer *b = out_buffer(rq, &pointer); need(b != NULL, "GetReleasedAudioOutBuffers buffer");
    uint64_t *out = ipc_guest(b->addr, b->size); uint32_t n = 0;
    pthread_mutex_lock(&o->lock);
    while (n < o->nreleased && (n + 1) * 8 <= b->size) { out[n] = o->released[n]; n++; }
    memmove(o->released, o->released + n, (o->nreleased - n) * sizeof *o->released); o->nreleased -= n;
    pthread_mutex_unlock(&o->lock);
    if (n < b->size / 8) out[n] = 0;
    if (pointer) rs->xout[rs->nxout++] = (IpcBuffer){b->addr, (uint64_t)n * 8, 0};
    IPC_OUT(rs, n);
}
static void set_state(AudioOut *o, uint32_t state) {
    pthread_mutex_lock(&o->lock); o->state = state; pthread_mutex_unlock(&o->lock);
    if (o->unit) { if (state == STATE_STARTED) OH_AudioRenderer_Start(o->unit); else OH_AudioRenderer_Pause(o->unit); }
}
static void audio_out_handler(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    AudioOut *o = s->state;
    switch (rq->cmd) {
    case 0: { uint32_t v = o->state; IPC_OUT(rs, v); return; }                                   /* GetAudioOutState */
    case 1: set_state(o, STATE_STARTED); return;                                                /* Start */
    case 2: set_state(o, STATE_STOPPED); return;                                                /* Stop */
    case 3: case 7: append(o, rq); return;                                                     /* AppendAudioOutBuffer(Auto) */
    case 4: rs->copy[rs->ncopy++] = runtime_event_readable(o->event); return;                   /* RegisterBufferEvent */
    case 5: case 8: released(o, rq, rs); return;                                               /* GetReleasedAudioOutBuffers(Auto) */
    case 6: {                                                                                   /* ContainsAudioOutBuffer(tag) */
        uint64_t tag = IPC_ARG(rq, uint64_t, 0); uint8_t found = 0;
        pthread_mutex_lock(&o->lock);
        for (unsigned i = 0; i < o->nqueue; i++) found |= o->queue[i].tag == tag;
        for (unsigned i = 0; i < o->nreleased; i++) found |= o->released[i] == tag;
        pthread_mutex_unlock(&o->lock); IPC_OUT(rs, found); return;
    }
    case 9: { pthread_mutex_lock(&o->lock); uint32_t v = o->nqueue; pthread_mutex_unlock(&o->lock); IPC_OUT(rs, v); return; }   /* GetAudioOutBufferCount */
    case 10: { pthread_mutex_lock(&o->lock); uint64_t v = o->played_samples; pthread_mutex_unlock(&o->lock); IPC_OUT(rs, v); return; }   /* GetAudioOutPlayedSampleCount */
    case 11: {                                                                                  /* FlushAudioOutBuffers */
        pthread_mutex_lock(&o->lock); uint8_t any = o->nqueue != 0;
        while (o->nqueue) release_head(o);
        pthread_mutex_unlock(&o->lock);
        if (o->unit) OH_AudioRenderer_Flush(o->unit);
        if (any) runtime_event_signal(o->event);
        IPC_OUT(rs, any); return;
    }
    case 12: { float v = IPC_ARG(rq, float, 0); pthread_mutex_lock(&o->lock); o->volume = v; pthread_mutex_unlock(&o->lock); return; }   /* SetAudioOutVolume */
    case 13: { float v = o->volume; IPC_OUT(rs, v); return; }                                   /* GetAudioOutVolume */
    default: ipc_unimplemented(s, rq);
    }
}
static void audio_out_close(void *p) {
    AudioOut *o = p;
    pthread_mutex_lock(&o->lock); o->running = 0; pthread_mutex_unlock(&o->lock);
    if (o->unit) { OH_AudioRenderer_Stop(o->unit); OH_AudioRenderer_Release(o->unit); }
    if (o->builder) OH_AudioStreamBuilder_Destroy(o->builder);
    if (o->silent) pthread_join(o->thread, NULL);
    fprintf(stderr, "[lza-audio] AudioOut closed: %llu samples played, %llu frames of underrun\n", (unsigned long long)o->played_samples, (unsigned long long)o->underrun_frames);
    pthread_mutex_destroy(&o->lock); free(o);
}

static const char device_name[] = "DeviceOut";
static void write_name(const IpcRequest *rq, IpcResponse *rs) {
    int pointer; const IpcBuffer *b = out_buffer(rq, &pointer);
    if (!b) return;
    char *out = ipc_guest(b->addr, b->size); size_t n = b->size < sizeof device_name ? b->size : sizeof device_name;
    memcpy(out, device_name, n); if (n) out[n - 1] = 0;
    if (pointer) rs->xout[rs->nxout++] = (IpcBuffer){b->addr, n, 0};
}
static void manager_handler(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    switch (rq->cmd) {
    case 0: case 2: { write_name(rq, rs); uint32_t n = 1; IPC_OUT(rs, n); return; }            /* ListAudioOuts(Auto) */
    case 1: case 3: {                                                                           /* OpenAudioOut(Auto) */
        uint32_t rate = IPC_ARG(rq, uint32_t, 0); uint16_t channels = IPC_ARG(rq, uint16_t, 4);
        if (!rate) rate = DEFAULT_RATE;
        if (!channels) channels = 2;
        if (rate != DEFAULT_RATE || (channels != 1 && channels != 2 && channels != 6)) {
            fprintf(stderr, "[lza-audio] OpenAudioOut %u Hz x%u\n", rate, channels); runtime_fail("AudioOut format not supported");
        }
        AudioOut *o = calloc(1, sizeof *o); need(o != NULL, "AudioOut allocation");
        pthread_mutex_init(&o->lock, NULL); o->rate = rate; o->channels = channels; o->state = STATE_STOPPED; o->volume = 1.0f;
        o->event = runtime_event_new(); o->running = 1;
        open_device(o);
        write_name(rq, rs);
        uint32_t out[4] = {rate, channels, FORMAT_PCM16, STATE_STOPPED}; ipc_out(rs, out, sizeof out);
        rs->move[rs->nmove++] = runtime_ipc_new_session("IAudioOut", audio_out_handler, o, audio_out_close);
        fprintf(stderr, "[lza-audio] AudioOut open %u Hz x%u PCM16 -> %s\n", rate, channels, o->silent ? "silent sink" : "OHAudio (stereo)");
        return;
    }
    default: ipc_unimplemented(s, rq);
    }
}

__attribute__((constructor)) static void register_audio_out(void) { runtime_ipc_register("audout:u", manager_handler, NULL, NULL); }
