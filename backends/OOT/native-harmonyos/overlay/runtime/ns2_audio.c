/* The Switch 2 SDK's audio device manager calls (SDK 21 / 22: virtual devices, OutputStream, InputStream), served on the host
   (GAME_NS2). The SDK reaches them through auddmg:u, which the host does not serve: Ocarina of Time lists the virtual input
   devices at start (sdk ListVirtualInputDevices -> devmgr::Initialize -> auddmg:u), asks the state of its two microphone
   streams, purges them when they are not started and reads their statistics each update; it opens them only when a virtual
   input device is listed. From seedds/nxnative dm/host/hle_audio.c (MIT):
   - no virtual input device (no microphone); every input stream call answers as for a stream never opened;
   - one virtual output device;
   - OutputStream: here a silent stream for now: it takes the frames a write offers while the frames still queued, drained at
     48 kHz in real time, leave room, so a game that paces itself by the free count keeps its pace. Its sound is not played
     (the game's sound goes through the audio renderer, aud:u, served by audio_renderer.c); logged once when opened.
   The entry points are declared as in nxnative (HLE_FORCE), registered in this file's table for runtime_ns2_audio_override. */
#include "runtime.h"
#include "thread_compat.h"
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef uint32_t Result;
#define RES_OK 0u
#define RES_FAIL ((Result)(0x99 | (1u << 9)))   /* nn::audio (module 153): the open fails */
#define RATE 48000
#undef LOG
#define LOG(...) do { fprintf(stderr, "[ns2-audio] " __VA_ARGS__); fputc('\n', stderr); } while (0)
void hle_signal_system_event(void *event);   /* runtime/threads.c */

typedef struct { const char *name; void *fn; } au_entry;
static au_entry g_au_tab[64]; static int g_au_ntab;
static void au_add(const char *name, void *fn) { if (g_au_ntab < (int)(sizeof g_au_tab / sizeof *g_au_tab)) g_au_tab[g_au_ntab++] = (au_entry){name, fn}; else runtime_fail("ns2_audio.c: entry table full"); }
#define HLE_FORCE(sym, why, ret, fn, ...) \
    static ret fn(__VA_ARGS__); __attribute__((constructor)) static void fn##_entry(void) { au_add(sym, (void *)fn); } static ret fn(__VA_ARGS__)

/* ---------- devices ---------- */
HLE_FORCE("_ZN2nn5audio23ListVirtualInputDevicesEPNS0_15AudioDeviceNameEi", "no microphone: no device", int, h_is_List, void *names, int n) { (void)names; (void)n; return 0; }
HLE_FORCE("_ZN2nn5audio31GetVirtualInputDevicePropertiesEPNS0_23VirtualDevicePropertiesERKNS0_15AudioDeviceNameE", "no such device", Result, h_is_Props, void *props, const char *name) { (void)props; (void)name; return RES_FAIL; }
HLE_FORCE("_ZN2nn5audio29GetDefaultVirtualOutputDeviceEPNS0_15AudioDeviceNameE", "the host's one output", void, h_os_DefaultDev, char *name) {
    memset(name, 0, 0x100); snprintf(name, 0x100, "%s", "AudioVirtualOutput");
}

/* ---------- OutputStream: silent, drained in real time ---------- */
enum { OS_NONE = 0, OS_STOPPED = 1, OS_STARTED = 2, OS_FLOAT = 5, OS_PCM16 = 2 };
typedef struct { const void *g; void *ev; pthread_mutex_t m; int ch, fmt, cap, state; double queued; uint64_t last_ns; } ostream;
static ostream *g_os[16];
static pthread_mutex_t g_os_mu = PTHREAD_MUTEX_INITIALIZER;
static uint64_t os_clock_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }
static ostream *os_of(const void *g) {
    pthread_mutex_lock(&g_os_mu); ostream *o = NULL;
    for (int i = 0; i < 16; i++) if (g_os[i] && g_os[i]->g == g) { o = g_os[i]; break; }
    pthread_mutex_unlock(&g_os_mu); return o;
}
static void os_drain(ostream *o) {   /* under o->m: frames played since the last look */
    uint64_t t = os_clock_ns();
    if (o->state == OS_STARTED && o->last_ns) { o->queued -= (double)(t - o->last_ns) * RATE / 1e9; if (o->queued < 0) o->queued = 0; }
    o->last_ns = t;
    if (o->ev && o->queued < o->cap) hle_signal_system_event(o->ev);
}
static Result os_open(void *g, void *ev, const int32_t *fmt, int frames) {
    int f = fmt[0], ch = fmt[1], rate = fmt[2];
    if ((f != OS_FLOAT && f != OS_PCM16) || (rate && rate != RATE) || ch <= 0 || frames <= 0) { LOG("[RED] OutputStream format %d, %d channels, %d Hz, %d frames: not supported, the open fails", f, ch, rate, frames); return RES_FAIL; }
    ostream *o = calloc(1, sizeof *o); if (!o) runtime_fail("OutputStream memory");
    pthread_mutex_init(&o->m, NULL); o->g = g; o->ev = ev; o->ch = ch; o->fmt = f; o->cap = frames; o->state = OS_STOPPED;
    if (ev) { hle_bind_system_event(ev, 0); hle_signal_system_event(ev); }
    pthread_mutex_lock(&g_os_mu); int k = 0; while (k < 16 && g_os[k]) k++; if (k < 16) g_os[k] = o; pthread_mutex_unlock(&g_os_mu);
    if (k == 16) { free(o); LOG("[RED] more than 16 output streams"); return RES_FAIL; }
    LOG("[RED] OutputStream %p: %d channels, format %d, %d frames: silent on this host (drained in real time)", g, ch, f, frames);
    return RES_OK;
}
HLE_FORCE("_ZN2nn5audio16OpenOutputStreamEPNS0_16OutputStreamTypeEPNS_2os11SystemEventEPvmRKNS0_15AudioDeviceNameERKNS0_12StreamFormatEiiNS_4util10BitFlagSetILi32ENS0_16OutputStreamFlagEEE",
          "a silent stream", Result, h_os_Open, void *g, void *ev, void *buf, uint64_t size, const char *dev, const int32_t *fmt, int frames, int threshold, uint32_t flags) {
    (void)buf; (void)size; (void)dev; (void)threshold; (void)flags; return os_open(g, ev, fmt, frames);
}
HLE_FORCE("_ZN2nn5audio16OpenOutputStreamEPNS0_16OutputStreamTypeEPvmRKNS0_15AudioDeviceNameERKNS0_12StreamFormatEiNS_4util10BitFlagSetILi32ENS0_16OutputStreamFlagEEE",
          "a silent stream (no event)", Result, h_os_OpenNoEv, void *g, void *buf, uint64_t size, const char *dev, const int32_t *fmt, int frames, uint32_t flags) {
    (void)buf; (void)size; (void)dev; (void)flags; return os_open(g, NULL, fmt, frames);
}
HLE_FORCE("_ZN2nn5audio17StartOutputStreamEPNS0_16OutputStreamTypeE", "start", Result, h_os_Start, void *g) {
    ostream *o = os_of(g); if (!o) return RES_FAIL;
    pthread_mutex_lock(&o->m); os_drain(o); o->state = OS_STARTED; o->last_ns = os_clock_ns(); pthread_mutex_unlock(&o->m); return RES_OK;
}
HLE_FORCE("_ZN2nn5audio18StartOutputStreamsEPKPNS0_16OutputStreamTypeEi", "start several", Result, h_os_StartN, void *const *gs, int n) {
    for (int i = 0; i < n; i++) { Result r = h_os_Start(gs[i]); if (r) return r; }
    return RES_OK;
}
HLE_FORCE("_ZN2nn5audio16StopOutputStreamEPNS0_16OutputStreamTypeE", "stop", void, h_os_Stop, void *g) {
    ostream *o = os_of(g); if (!o) return;
    pthread_mutex_lock(&o->m); os_drain(o); o->state = OS_STOPPED; pthread_mutex_unlock(&o->m);
}
HLE_FORCE("_ZN2nn5audio17PurgeOutputStreamEPNS0_16OutputStreamTypeE", "drop the frames not yet played", void, h_os_Purge, void *g) {
    ostream *o = os_of(g); if (!o) return;
    pthread_mutex_lock(&o->m); o->queued = 0; os_drain(o); pthread_mutex_unlock(&o->m);
}
HLE_FORCE("_ZN2nn5audio17CloseOutputStreamEPNS0_16OutputStreamTypeE", "close", void, h_os_Close, void *g) {
    pthread_mutex_lock(&g_os_mu); ostream *o = NULL;
    for (int i = 0; i < 16; i++) if (g_os[i] && g_os[i]->g == g) { o = g_os[i]; g_os[i] = NULL; }
    pthread_mutex_unlock(&g_os_mu);
    if (o) { pthread_mutex_destroy(&o->m); free(o); }
}
HLE_FORCE("_ZN2nn5audio20GetOutputStreamStateEPKNS0_16OutputStreamTypeE", "0 not open, 1 stopped, 2 started", int, h_os_State, const void *g) { ostream *o = os_of(g); return o ? o->state : OS_NONE; }
HLE_FORCE("_ZN2nn5audio29GetOutputStreamFreeFrameCountEPKNS0_16OutputStreamTypeE", "the capacity less the frames queued", int, h_os_Free, const void *g) {
    ostream *o = os_of(g); if (!o) return 0;
    pthread_mutex_lock(&o->m); os_drain(o); int n = o->cap - (int)o->queued; pthread_mutex_unlock(&o->m);
    return n < 0 ? 0 : n;
}
HLE_FORCE("_ZN2nn5audio31GetOutputStreamFilledFrameCountEPKNS0_16OutputStreamTypeE", "frames written and not yet played", int, h_os_Filled, const void *g) {
    ostream *o = os_of(g); if (!o) return 0;
    pthread_mutex_lock(&o->m); os_drain(o); int n = (int)o->queued; pthread_mutex_unlock(&o->m); return n;
}
HLE_FORCE("_ZN2nn5audio17WriteOutputStreamEPNS0_16OutputStreamTypeEPKvi", "frames taken (not played)", int, h_os_Write, void *g, const void *data, int frames) {
    ostream *o = os_of(g); if (!o || !data || frames <= 0) return 0;
    pthread_mutex_lock(&o->m); os_drain(o);
    int room = o->cap - (int)o->queued; if (frames > room) frames = room < 0 ? 0 : room;
    o->queued += frames;
    pthread_mutex_unlock(&o->m); return frames;
}
HLE_FORCE("_ZN2nn5audio19SetOutputStreamGainEPNS0_16OutputStreamTypeEfNS_8TimeSpanE", "gain: silent", void, h_os_Gain, void *g, float gain, int64_t fade) { (void)g; (void)gain; (void)fade; }
typedef struct { int64_t a, b, c; } os_stats;   /* returned through x8, 24 bytes */
HLE_FORCE("_ZN2nn5audio25GetOutputStreamStatisticsEPNS0_16OutputStreamTypeEb", "none kept", os_stats, h_os_Stats, void *g, bool reset) { (void)g; (void)reset; os_stats s = {0}; return s; }

/* ---------- InputStream: never open ---------- */
HLE_FORCE("_ZN2nn5audio15OpenInputStreamEPNS0_15InputStreamTypeEPvmRKNS0_15AudioDeviceNameERKNS0_12StreamFormatEi", "no microphone: fails", Result, h_is_Open,
          void *g, void *buf, uint64_t size, const char *dev, const void *fmt, int frames) { (void)g; (void)buf; (void)size; (void)dev; (void)fmt; (void)frames; return RES_FAIL; }
HLE_FORCE("_ZN2nn5audio16CloseInputStreamEPNS0_15InputStreamTypeE", "never open", void, h_is_Close, void *g) { (void)g; }
HLE_FORCE("_ZN2nn5audio16StartInputStreamEPNS0_15InputStreamTypeE", "never open", Result, h_is_Start, void *g) { (void)g; return RES_FAIL; }
HLE_FORCE("_ZN2nn5audio15StopInputStreamEPNS0_15InputStreamTypeE", "never open", void, h_is_Stop, void *g) { (void)g; }
HLE_FORCE("_ZN2nn5audio15ReadInputStreamEPNS0_15InputStreamTypeEPvi", "never open: nothing read", int, h_is_Read, void *g, void *buf, int frames) { (void)g; (void)buf; (void)frames; return 0; }
HLE_FORCE("_ZN2nn5audio30GetInputStreamFilledFrameCountEPKNS0_15InputStreamTypeE", "never open: nothing captured", int, h_is_Filled, const void *g) { (void)g; return 0; }
typedef struct { int32_t format, channels, rate; } os_format;   /* nn::audio::StreamFormat, 12 bytes: returned in registers */
HLE_FORCE("_ZN2nn5audio20GetInputStreamFormatEPKNS0_15InputStreamTypeE", "never open: 48 kHz mono PCM16", os_format, h_is_Format, const void *g) { (void)g; return (os_format){OS_PCM16, 1, RATE}; }
HLE_FORCE("_ZN2nn5audio19GetInputStreamStateEPKNS0_15InputStreamTypeE", "never open: closed (0)", int, h_is_State, const void *g) { (void)g; return 0; }
HLE_FORCE("_ZN2nn5audio16PurgeInputStreamEPNS0_15InputStreamTypeE", "never open: nothing buffered", void, h_is_Purge, void *g) { (void)g; }
HLE_FORCE("_ZN2nn5audio24GetInputStreamStatisticsEPNS0_15InputStreamTypeEb", "never open: zero", os_stats, h_is_Stats, void *g, bool reset) { (void)g; (void)reset; os_stats s = {0}; return s; }
HLE_FORCE("_ZN2nn5audio28GetInputStreamFreeFrameCountEPKNS0_15InputStreamTypeE", "never open: none", int, h_is_Free, const void *g) { (void)g; return 0; }
HLE_FORCE("_ZN2nn5audio32GetInputStreamCapacityFrameCountEPKNS0_15InputStreamTypeE", "never open: none", int, h_is_Capacity, const void *g) { (void)g; return 0; }
HLE_FORCE("_ZN2nn5audio18GetInputStreamGainEPKNS0_15InputStreamTypeE", "never open: 1", float, h_is_GetGain, const void *g) { (void)g; return 1.0f; }
HLE_FORCE("_ZN2nn5audio18SetInputStreamGainEPNS0_15InputStreamTypeEfNS_8TimeSpanE", "never open", void, h_is_SetGain, void *g, float gain, int64_t ramp_ns) { (void)g; (void)gain; (void)ramp_ns; }

void *runtime_ns2_audio_override(const char *name) {
    for (int i = 0; i < g_au_ntab; i++) if (!strcmp(name, g_au_tab[i].name)) return g_au_tab[i].fn;
    return NULL;
}
