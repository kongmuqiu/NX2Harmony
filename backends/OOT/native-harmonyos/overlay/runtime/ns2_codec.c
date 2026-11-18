/* The Switch 2's nn::codec video decoders and sync point events, served on the host (GAME_NS2). Ported from seedds/nxnative
   dm/host/hle_codec_video.c (MIT; its current form with the SDK 22 entry points and AV1) with its SDK protocol notes: VP9
   through libvpx (VP8 too where the library has it), AV1 through dav1d (ns2_av1.c; Ocarina of Time's movies are AV1), not
   H.264 (nxnative: VideoToolbox), whose Initialize fails.
   The SDK sends every call to a decoder service the host does not serve. The protocol (sdk 20.5.19 Ounce):
   - the client makes one image pool per output picture (MakeDecodeImagePreference -> CalculateDecoderImagePoolBuffer ->
     InitializeDecoderImagePool) and registers each with the decoder under an index. Registering puts the index in the
     decoder's queue of acquirable buffers (8 deep), with a sync handle already signalled.
   - Acquire takes the oldest index and its sync handle (description 3604 when the queue is empty); Release gives the buffer
     to the decoder to fill and puts it back in the queue at once, with a new sync handle that signals when a picture is in
     it. The client polls nn::gs::TryWaitNativeSync (or waits on a sync point event), reads the picture's properties (3604
     until it is filled) and the picture (ReadDecoderOutputImageRgba / Nv12, the CPU route).
   - Submit takes one packet with its time and an end-of-stream flag; 3603 when the decoder cannot take more (the client
     retries after the submission-ready event).
   Pictures go into the released buffers in release order. Packets decode on the submitting thread; pictures with no
   released buffer wait in a short queue, and a full queue refuses packets. The SDK's opaque types hold a host pointer at
   +0; the decoder's event lives in its type at +0x100.
   SDK 22 reaches the same protocol through other names: ByTransferMemory / ByRemoteMemory initializers, a preference with
   an image tiling mode, a pool without its mode, and a submission event returned rather than written out. Its player can
   take pictures the GPU route: GetDecoderImagePoolNativeImageHandle's handle (nvnNativeImage, then a texture over the
   picture), so a pool whose handle was asked for keeps each picture as RGBA in the pool's own memory (dm_native_image
   serves the NVN side; on HarmonyOS that side is not there yet and logs once). Not served: encrypted input, alias pools.
   Entry points are declared with HLE_FORCE as in nxnative; here it registers the function in this file's table, which
   runtime_ns2_codec_override (services.c) searches. */
#include "runtime.h"
#include "thread_compat.h"
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include "vpx/vpx_decoder.h"
#include "vpx/vp8dx.h"
#include "ns2_av1.h"
/* The HarmonyOS libvpx (artifacts/native-vpx, Z-A) is built with the VP9 decoder only: a VP8 stream then fails to open
   (vpx_ok 0) instead of the library failing to link. */
__attribute__((weak)) vpx_codec_iface_t *vpx_codec_vp8_dx(void);

typedef uint32_t Result;
void hle_signal_system_event(void *event);   /* runtime/threads.c */
#undef LOG
#define LOG(...) do { fprintf(stderr, "[ns2-codec] " __VA_ARGS__); fputc('\n', stderr); } while (0)
#define die(msg) runtime_fail(msg)

/* nxnative's HLE_FORCE: here a function and its entry in g_cv_tab, made before main (constructor) */
typedef struct { const char *name; void *fn; } cv_entry;
static cv_entry g_cv_tab[192]; static int g_cv_ntab;
static void cv_add(const char *name, void *fn) { if (g_cv_ntab < (int)(sizeof g_cv_tab / sizeof *g_cv_tab)) g_cv_tab[g_cv_ntab++] = (cv_entry){name, fn}; else die("ns2_codec.c: entry table full"); }
#define HLE_FORCE(sym, why, ret, fn, ...) \
    static ret fn(__VA_ARGS__); __attribute__((constructor)) static void fn##_entry(void) { cv_add(sym, (void *)fn); } static ret fn(__VA_ARGS__)

static void hle_clear_system_event(void *e) {
    static void (*clear)(void *);
    if (!clear && !(clear = guest_sym("_ZN2nn2os16ClearSystemEventEPNS0_15SystemEventTypeE"))) die("nn::os::ClearSystemEvent missing");
    clear(e);
}
static void destroy_event(void *e) {
    static void (*destroy)(void *);
    if (!destroy && !(destroy = guest_sym("_ZN2nn2os18DestroySystemEventEPNS0_15SystemEventTypeE"))) die("nn::os::DestroySystemEvent missing");
    destroy(e);
}

/* ---------- YUV (libvpx's I420) to what the game reads; from nxnative hle_movie.c md_nv12 / md_rgba (MIT) ---------- */
static void md_nv12(const vpx_image_t *im, uint8_t *dst, int w, int h) {
    int cw = (int)im->d_w < w ? (int)im->d_w : w, ch = (int)im->d_h < h ? (int)im->d_h : h;
    if (cw < w || ch < h) { memset(dst, 16, (size_t)w * h); memset(dst + (size_t)w * h, 128, (size_t)w * h / 2); }
    for (int y = 0; y < ch; y++) memcpy(dst + (size_t)y * w, im->planes[0] + (size_t)y * im->stride[0], (size_t)cw);
    int cw2 = (cw + 1) / 2, ch2 = (ch + 1) / 2;
    for (int y = 0; y < ch2 && y < h / 2; y++) {
        const uint8_t *u = im->planes[1] + (size_t)y * im->stride[1], *v = im->planes[2] + (size_t)y * im->stride[2];
        uint8_t *o = dst + (size_t)w * h + (size_t)y * w;
        for (int x = 0; x < cw2 && 2 * x + 1 < w; x++) { o[2 * x] = u[x]; o[2 * x + 1] = v[x]; }
    }
}
/* By the colour space and range the picture declares (undeclared: BT.709 from 720 lines up, else BT.601). */
static void md_rgba(const vpx_image_t *im, uint8_t *dst, int w, int h) {
    int full = im->range == VPX_CR_FULL_RANGE, b709 = im->cs == VPX_CS_BT_709 || (im->cs == VPX_CS_UNKNOWN && h >= 720), b2020 = im->cs == VPX_CS_BT_2020;
    double ky = full ? 1.0 : 255.0 / 219.0, kc = full ? 1.0 : 255.0 / 224.0;
    double rv = b2020 ? 1.4746 : b709 ? 1.5748 : 1.402, gu = b2020 ? 0.16455 : b709 ? 0.1873 : 0.3441, gv = b2020 ? 0.57135 : b709 ? 0.4681 : 0.7141, bu = b2020 ? 1.8814 : b709 ? 1.8556 : 1.772;
    int Y = (int)(ky * 65536), RV = (int)(rv * kc * 65536), GU = (int)(gu * kc * 65536), GV = (int)(gv * kc * 65536), BU = (int)(bu * kc * 65536), y0 = full ? 0 : 16;
    int cw = (int)im->d_w < w ? (int)im->d_w : w, ch = (int)im->d_h < h ? (int)im->d_h : h;
    if (cw < w || ch < h) for (size_t i = 0; i < (size_t)w * h; i++) ((uint32_t *)(void *)dst)[i] = 0xff000000u;
#define CL(v) ((v) < 0 ? 0 : (v) > 255 ? 255 : (v))
    for (int y = 0; y < ch; y++) {
        const uint8_t *py = im->planes[0] + (size_t)y * im->stride[0], *pu = im->planes[1] + (size_t)(y >> 1) * im->stride[1], *pv = im->planes[2] + (size_t)(y >> 1) * im->stride[2];
        uint8_t *o = dst + (size_t)y * w * 4;
        for (int x = 0; x < cw; x++) {
            int l = (py[x] - y0) * Y, u = pu[x >> 1] - 128, v = pv[x >> 1] - 128;
            int r = (l + RV * v + 32768) >> 16, g = (l - GU * u - GV * v + 32768) >> 16, b = (l + BU * u + 32768) >> 16;
            o[4 * x] = (uint8_t)CL(r); o[4 * x + 1] = (uint8_t)CL(g); o[4 * x + 2] = (uint8_t)CL(b); o[4 * x + 3] = 255;
        }
    }
#undef CL
}

#define CV_MAGIC 0x56436d44u        /* 'DmCV' */
#define CV_SYNC 0x53436d44u         /* 'DmCS': a sync handle the host made (dec id, index, generation) */
#define CV_BUFS 32
#define CV_READY 4                  /* decoded pictures waiting for a released buffer at which the submission-ready event goes down */
#define CV_HOLD 12                  /* and at which packets are refused (a signal in flight when the event goes down must still find room) */
#define CV_EVENT_OFF 0x100          /* the submission-ready SystemEventType inside the decoder type */
#define CV_R(desc) ((Result)(0x6f | ((desc) << 9)))   /* nn::codec results (module 111) */
#define CV_BUSY CV_R(3603)          /* the decoder cannot take the packet now */
#define CV_NONE CV_R(3604)          /* no buffer to acquire / no picture in the buffer yet */

typedef struct { uint16_t w, h; uint32_t fmt, mode, magic; uint32_t pad[2]; } cv_pref;   /* VideoDecoderImagePreference as the host fills it (returned in x8) */
typedef struct { uint32_t magic; uint16_t w, h; uint32_t fmt, count; vpx_image_t **img; uint8_t *mem; uint64_t size; int slot; _Atomic int native; } cv_pool;
typedef struct { uint32_t magic, dec, idx, gen; } cv_sync;   /* nn::gs::NativeSyncHandle (16 bytes) */
enum { CV_VP8, CV_VP9, CV_H264, CV_AV1 };
static const char *const k_cv_name[] = {"VP8", "VP9", "H.264", "AV1"};
typedef struct cv_buf { cv_pool *pool; uint32_t pidx; int reg, filled; uint32_t gen, filled_gen; uint16_t w, h; int64_t ts; uint8_t eos; } cv_buf;
typedef struct {
    uint32_t magic, id; pthread_mutex_t mu; vpx_codec_ctx_t vpx; int vpx_ok; void *event; uint64_t submits, pictures;
    int codec; uint16_t w, h;
    mav1 *av1;   /* AV1: dav1d (ns2_av1.c) */
    cv_buf buf[CV_BUFS];
    uint32_t fifo[8], fhead, fcount;          /* acquirable indices */
    uint32_t pend[CV_BUFS], phead, pcount;    /* released, waiting for a picture */
    vpx_image_t *held[CV_HOLD]; int64_t held_ts[CV_HOLD]; uint8_t held_eos[CV_HOLD]; int nheld;
    int eos_pending;
} cv_dec;

static cv_dec *g_cv[16];
static pthread_mutex_t g_cv_mu = PTHREAD_MUTEX_INITIALIZER;   /* lock order: g_cv_mu, then a decoder's mu, then g_spt_mu */
static cv_dec *cv_of(void *t) { cv_dec *d = t ? *(cv_dec **)t : NULL; return d && d->magic == CV_MAGIC ? d : NULL; }
static cv_pool *pool_of(const void *t) { cv_pool *p = t ? *(cv_pool *const *)t : NULL; return p && p->magic == CV_MAGIC ? p : NULL; }

static vpx_image_t *img_copy(const vpx_image_t *im) {   /* an owned I420 copy of a libvpx picture (its buffers are reused by the next decode) */
    vpx_image_t *c = vpx_img_alloc(NULL, VPX_IMG_FMT_I420, im->d_w, im->d_h, 1);
    if (!c) return NULL;
    c->cs = im->cs; c->range = im->range;
    for (int p = 0; p < 3; p++) {
        unsigned w = p ? (im->d_w + 1) / 2 : im->d_w, h = p ? (im->d_h + 1) / 2 : im->d_h;
        for (unsigned y = 0; y < h; y++) memcpy(c->planes[p] + (size_t)y * c->stride[p], im->planes[p] + (size_t)y * im->stride[p], w);
    }
    return c;
}

/* Sync point events (nn::syncpt::sdk): the player's worker waits on one (in a MultiWait) for the sync handle of the picture it
   expects. Their library is set up by the graphics initialisation the host replaces, so CreateSyncPointEvent jumped through
   its null table. The event type holds a SystemEvent the host binds at +0 (0x30 bytes) and the awaited handle at +0x40. */
typedef struct { uint32_t magic, armed; cv_sync want; } cv_spt;
#define SPT(e) ((cv_spt *)(void *)((uint8_t *)(e) + 0x40))
static void *g_spt[64];
static pthread_mutex_t g_spt_mu = PTHREAD_MUTEX_INITIALIZER;
static bool sync_reached(const cv_sync *s);
static void spt_fire(cv_dec *d, uint32_t i) {   /* buffer i of d filled: signal the events waiting for it (decoder lock held) */
    pthread_mutex_lock(&g_spt_mu);
    for (int k = 0; k < 64; k++) { void *e = g_spt[k]; cv_spt *t = e ? SPT(e) : NULL;
        if (t && t->want.dec == d->id && t->want.idx == i && d->buf[i].filled_gen >= t->want.gen) { t->armed = 0; g_spt[k] = NULL; hle_signal_system_event(e); } }
    pthread_mutex_unlock(&g_spt_mu);
}
static void spt_unarm(void *e) { pthread_mutex_lock(&g_spt_mu); for (int k = 0; k < 64; k++) if (g_spt[k] == e) g_spt[k] = NULL; pthread_mutex_unlock(&g_spt_mu); SPT(e)->armed = 0; }

/* A native image: one picture's RGBA in the pool's memory, rows on the 64-byte stride NVN's linear textures take, each
   picture on a 16 KB page */
#define CV_NATIVE_ALIGN 0x4000ull
static uint32_t native_stride(uint32_t w) { return (w * 4 + 63) & ~63u; }
static uint64_t native_size(uint32_t w, uint32_t h) { return ((uint64_t)native_stride(w) * h + CV_NATIVE_ALIGN - 1) & ~(CV_NATIVE_ALIGN - 1); }
static void pool_native_write(cv_pool *p, uint32_t i, const vpx_image_t *im) {
    if (!p->native || !p->mem || (i + 1) * native_size(p->w, p->h) > p->size) return;
    uint8_t *dst = p->mem + i * native_size(p->w, p->h);
    uint32_t stride = native_stride(p->w);
    if (stride == (uint32_t)p->w * 4) { md_rgba(im, dst, p->w, p->h); return; }
    uint8_t *tmp = malloc((size_t)p->w * p->h * 4);
    if (!tmp) return;
    md_rgba(im, tmp, p->w, p->h);
    for (uint32_t y = 0; y < p->h; y++) memcpy(dst + (size_t)y * stride, tmp + (size_t)y * p->w * 4, (size_t)p->w * 4);
    free(tmp);
}
/* Pictures and end of stream into the released buffers, in order (decoder lock held). The event says a packet can come: it
   rises when one was taken (took) or room came back, not at every release. */
static void drain(cv_dec *d, int took) {
    int before = d->nheld;
    while (d->pcount && (d->nheld || d->eos_pending)) {
        uint32_t i = d->pend[d->phead]; d->phead = (d->phead + 1) % CV_BUFS; d->pcount--;
        cv_buf *b = &d->buf[i];
        if (d->nheld) {
            vpx_image_t *im = d->held[0]; int64_t ts = d->held_ts[0]; uint8_t eos = d->held_eos[0];
            memmove(d->held, d->held + 1, sizeof d->held[0] * (CV_HOLD - 1)); memmove(d->held_ts, d->held_ts + 1, sizeof d->held_ts[0] * (CV_HOLD - 1));
            memmove(d->held_eos, d->held_eos + 1, CV_HOLD - 1); d->nheld--;
            b->w = (uint16_t)im->d_w; b->h = (uint16_t)im->d_h; b->ts = ts; b->eos = eos;
            if (b->pool && b->pidx < b->pool->count) { pool_native_write(b->pool, b->pidx, im); if (b->pool->img[b->pidx]) vpx_img_free(b->pool->img[b->pidx]); b->pool->img[b->pidx] = im; } else vpx_img_free(im);
            if (!d->pictures++) LOG("%s decoder %u: first picture %ux%u into buffer %u", k_cv_name[d->codec], d->id, b->w, b->h, i);
        } else { b->w = b->h = 0; b->ts = 0; b->eos = 1; d->eos_pending = 0; }
        if (b->eos) LOG("%s decoder %u: end of stream after %llu pictures", k_cv_name[d->codec], d->id, (unsigned long long)d->pictures);
        b->filled = 1; b->filled_gen = b->gen;
        spt_fire(d, i);
    }
    if (d->event) { if (d->nheld >= CV_READY) hle_clear_system_event(d->event); else if (took || before >= CV_READY) hle_signal_system_event(d->event); }
}

HLE_FORCE("_ZN2nn5codec5video10InitializeEv", "the decoder service is the host's", Result, h_cv_init, void) { return 0; }
HLE_FORCE("_ZN2nn5codec5video8FinalizeEv", "the decoder service is the host's", Result, h_cv_fin, void) { return 0; }

HLE_FORCE("_ZN2nn5codec25MakeDecodeImagePreferenceEttNS0_24VideoDecoderOutputFormatENS0_9CodecModeE", "the preference only carries the size and format to the host's pool",
          cv_pref, h_cv_pref, uint16_t w, uint16_t h, uint32_t fmt, uint32_t mode) {
    return (cv_pref){w, h, fmt, mode, CV_MAGIC, {0, 0}};
}
HLE_FORCE("_ZN2nn5codec25MakeDecodeImagePreferenceEttNS0_24VideoDecoderOutputFormatENS0_15ImageTilingModeE", "the preference only carries the size and format (SDK 22's name)",
          cv_pref, h_cv_pref22, uint64_t w, uint64_t h, uint32_t fmt, uint32_t tiling) {
    return (cv_pref){(uint16_t)w, (uint16_t)h, fmt, tiling, CV_MAGIC, {0, 0}};
}
HLE_FORCE("_ZN2nn5codec31CalculateDecoderImagePoolBufferEPmS1_RKNS0_27VideoDecoderImagePreferenceEj", "pool memory: room for RGBA pictures",
          Result, h_cv_poolsize, uint64_t *size, uint64_t *align, const cv_pref *p, uint32_t n) {
    *size = native_size(p->w, p->h) * (n ? n : 1); *align = CV_NATIVE_ALIGN;
    return 0;
}
static cv_pool *g_cv_pools[256];   /* what a native image handle names (g_cv_mu): (slot + 1) << 8 | picture */
static Result cv_pool_init(void *t, void *mem, uint64_t size, const cv_pref *p, uint32_t n) {
    cv_pool *o = calloc(1, sizeof *o); if (!o) die("nn::codec image pool memory");
    o->magic = CV_MAGIC; o->w = p->w; o->h = p->h; o->fmt = p->fmt; o->count = n ? n : 1; o->img = calloc(o->count, sizeof *o->img); o->mem = mem; o->size = size;
    if (!o->img) die("nn::codec image pool memory");
    *(cv_pool **)t = o;
    static int once; if (!once++) LOG("image pool %ux%u format %u, tiling or mode %u, %u picture(s): the host keeps the decoded pictures", p->w, p->h, p->fmt, p->mode, o->count);
    return 0;
}
HLE_FORCE("_ZN2nn5codec26InitializeDecoderImagePoolEPNS0_25VideoDecoderImagePoolTypeEPvmRKNS0_27VideoDecoderImagePreferenceEjNS0_9CodecModeE", "a host pool of decoded pictures",
          Result, h_cv_pool, void *t, void *mem, uint64_t size, const cv_pref *p, uint32_t n, uint32_t mode) { (void)mode; return cv_pool_init(t, mem, size, p, n); }
HLE_FORCE("_ZN2nn5codec26InitializeDecoderImagePoolEPNS0_25VideoDecoderImagePoolTypeEPvmRKNS0_27VideoDecoderImagePreferenceEj", "a host pool of decoded pictures (SDK 22's name)",
          Result, h_cv_pool22, void *t, void *mem, uint64_t size, const cv_pref *p, uint32_t n) { return cv_pool_init(t, mem, size, p, n); }
HLE_FORCE("_ZN2nn5codec24FinalizeDecoderImagePoolEPNS0_25VideoDecoderImagePoolTypeE", "frees the host pool", void, h_cv_poolfin, void *t) {
    cv_pool *p = pool_of(t); if (!p) return;
    pthread_mutex_lock(&g_cv_mu);   /* a decoder still holding a buffer of this pool keeps it, with no pool to write into */
    for (int k = 0; k < 16; k++) if (g_cv[k]) { pthread_mutex_lock(&g_cv[k]->mu); for (int i = 0; i < CV_BUFS; i++) if (g_cv[k]->buf[i].pool == p) g_cv[k]->buf[i].pool = NULL; pthread_mutex_unlock(&g_cv[k]->mu); }
    if (p->slot) g_cv_pools[p->slot - 1] = NULL;
    pthread_mutex_unlock(&g_cv_mu);
    for (uint32_t i = 0; i < p->count; i++) if (p->img[i]) vpx_img_free(p->img[i]);
    free(p->img); p->magic = 0; free(p); *(cv_pool **)t = NULL;
}
HLE_FORCE("_ZN2nn5codec36GetDecoderImagePoolNativeImageHandleEPKNS0_25VideoDecoderImagePoolTypeEj", "a handle dm_native_image knows; the pool keeps its pictures as RGBA from now on",
          uint32_t, h_cv_native, const void *t, uint32_t i) {
    cv_pool *p = pool_of(t); if (!p || i >= p->count) return 0;
    pthread_mutex_lock(&g_cv_mu);
    if (!p->slot) for (int k = 0; k < 256; k++) if (!g_cv_pools[k]) { g_cv_pools[k] = p; p->slot = k + 1; break; }
    pthread_mutex_unlock(&g_cv_mu);
    if (!p->slot) { static int once; if (!once++) LOG("[RED] more than 256 pools with native images"); return 0; }
    if (!p->native) LOG("image pool %ux%u: native image handles asked for (the GPU route): its pictures are kept as RGBA in the pool's memory", p->w, p->h);
    p->native = 1;
    return (uint32_t)p->slot << 8 | i;
}
/* A native image handle's picture for the NVN layer: 1 and its memory, size and layout; 0 for a handle no pool holds */
int dm_native_image(uint32_t handle, uint8_t **mem, uint64_t *size, uint32_t *w, uint32_t *h, uint32_t *stride) {
    uint32_t slot = handle >> 8, i = handle & 0xff; int ok = 0;
    pthread_mutex_lock(&g_cv_mu);
    cv_pool *p = slot && slot <= 256 ? g_cv_pools[slot - 1] : NULL;
    if (p && p->mem && i < p->count && (i + 1) * native_size(p->w, p->h) <= p->size) {
        *mem = p->mem + i * native_size(p->w, p->h); *size = native_size(p->w, p->h); *w = p->w; *h = p->h; *stride = native_stride(p->w); ok = 1;
    }
    pthread_mutex_unlock(&g_cv_mu);
    return ok;
}
HLE_FORCE("_ZN2nn5codec29ReadDecoderOutputImageRgbaHdrEPKNS0_25VideoDecoderImagePoolTypeEjttttPvmm", "HDR pictures are not served: black",
          Result, h_cv_rgbahdr, const void *t, uint32_t i, uint64_t x, uint64_t y, uint64_t w, uint64_t h, void *dst, uint64_t stride, uint64_t size) {
    (void)t; (void)i; (void)x; (void)y; (void)w; (void)h; (void)stride;
    static int once; if (!once++) LOG("[RED] ReadDecoderOutputImageRgbaHdr: HDR pictures are not served, black");
    memset(dst, 0, size); return 0;
}
HLE_FORCE("_ZN2nn5codec26ReadDecoderOutputImageRgbaEPKNS0_25VideoDecoderImagePoolTypeEjttttPvmm", "converts the host's picture (md_rgba)",
          Result, h_cv_rgba, const void *t, uint32_t i, uint16_t x, uint16_t y, uint16_t w, uint16_t h, void *dst, uint64_t stride, uint64_t size) {
    cv_pool *p = pool_of(t); const vpx_image_t *im = p && i < p->count ? p->img[i] : NULL;
    if (!im) { memset(dst, 0, size); return 0; }
    if (x || y || stride != (uint64_t)w * 4 || size < (uint64_t)w * h * 4) { static int once; if (!once++) LOG("[RED] ReadDecoderOutputImageRgba: region %u,%u stride %llu: only whole tight pictures are served", x, y, (unsigned long long)stride); return 0; }
    md_rgba(im, dst, w, h);
    return 0;
}
HLE_FORCE("_ZN2nn5codec26ReadDecoderOutputImageNv12EPKNS0_25VideoDecoderImagePoolTypeEjttttPvmmS4_mm", "converts the host's picture (md_nv12)",
          Result, h_cv_nv12, const void *t, uint32_t i, uint16_t x, uint16_t y, uint16_t w, uint16_t h, void *ydst, uint64_t ystride, uint64_t ysize, void *uvdst, uint64_t uvstride, uint64_t uvsize) {
    cv_pool *p = pool_of(t); const vpx_image_t *im = p && i < p->count ? p->img[i] : NULL;
    if (x || y || ystride != w || uvstride != w || ysize < (uint64_t)w * h || uvsize < (uint64_t)w * h / 2) { static int once; if (!once++) LOG("[RED] ReadDecoderOutputImageNv12: region %u,%u strides %llu / %llu: only whole tight pictures are served", x, y, (unsigned long long)ystride, (unsigned long long)uvstride); return 0; }
    if (!im) { memset(ydst, 16, ysize); memset(uvdst, 128, uvsize); return 0; }
    if ((uint8_t *)uvdst == (uint8_t *)ydst + (size_t)w * h) { md_nv12(im, ydst, w, h); return 0; }
    uint8_t *tmp = malloc((size_t)w * h * 3 / 2); if (!tmp) die("nn::codec readback memory");
    md_nv12(im, tmp, w, h);
    memcpy(ydst, tmp, (size_t)w * h); memcpy(uvdst, tmp + (size_t)w * h, (size_t)w * h / 2); free(tmp);
    return 0;
}

/* One host decoder for every codec the SDK has a decoder type for (Vp8 / Vp9 / H264 / Av1DecoderType: the same layout) */
static void cv_open(cv_dec *d) {   /* (re)creates the codec state: a new stream starts at the next key frame */
    if (d->codec == CV_AV1) {
        if (!d->av1) d->av1 = mav1_new(); else mav1_reset(d->av1);
        d->vpx_ok = d->av1 != NULL;
        return;
    }
    if (d->codec == CV_H264) { d->vpx_ok = 0; return; }   /* no H.264 decoder on this host */
    if (d->vpx_ok) vpx_codec_destroy(&d->vpx);
    vpx_codec_dec_cfg_t cfg = {.threads = 2, .w = d->w, .h = d->h};
    vpx_codec_iface_t *iface = d->codec == CV_VP8 ? (vpx_codec_vp8_dx ? vpx_codec_vp8_dx() : NULL) : vpx_codec_vp9_dx();
    d->vpx_ok = iface && vpx_codec_dec_init(&d->vpx, iface, &cfg, 0) == VPX_CODEC_OK;
    if (!d->vpx_ok) LOG("[RED] %s decoder: libvpx init failed", k_cv_name[d->codec]);
}
static Result cv_dec_init(void *t, int codec, uint16_t w, uint16_t h, uint32_t in, uint32_t out) {
    if (codec == CV_H264) { static int once; if (!once++) LOG("[RED] H.264 decoder: no H.264 decoder on this host, Initialize fails"); return CV_R(2004); }
    cv_dec *d = calloc(1, sizeof *d); if (!d) die("nn::codec decoder memory");
    d->magic = CV_MAGIC; d->codec = codec; d->w = w; d->h = h; pthread_mutex_init(&d->mu, NULL);
    cv_open(d);
    pthread_mutex_lock(&g_cv_mu);
    for (int i = 0; i < 16; i++) if (!g_cv[i]) { g_cv[i] = d; d->id = (uint32_t)i + 1; break; }
    pthread_mutex_unlock(&g_cv_mu);
    if (!d->id) die("nn::codec: more than 16 video decoders");
    d->event = (uint8_t *)t + CV_EVENT_OFF; hle_bind_system_event(d->event, 0); hle_signal_system_event(d->event);
    *(cv_dec **)t = d;
    LOG("%s decoder %u: %ux%u, input format %u, output format %u (%s on the host)", k_cv_name[codec], d->id, w, h, in, out,
        codec == CV_AV1 ? mav1_backend(d->av1) : "libvpx");
    return 0;
}
static Result cv_dec_fin(void *t) {
    cv_dec *d = cv_of(t); if (!d) return 0;
    pthread_mutex_lock(&g_cv_mu); g_cv[d->id - 1] = NULL; pthread_mutex_lock(&d->mu); pthread_mutex_unlock(&d->mu); pthread_mutex_unlock(&g_cv_mu);
    if (d->codec != CV_AV1 && d->vpx_ok) vpx_codec_destroy(&d->vpx);
    if (d->av1) mav1_free(d->av1);
    for (int i = 0; i < d->nheld; i++) vpx_img_free(d->held[i]);
    d->magic = 0;
    /* The submission-ready event goes with the decoder, as in the SDK: kept, it stayed bound, and the next decoder made in the
       same (cleared) memory found it "bound" and got none; nn::os aborted on it (nxnative, Bananza's second movie). */
    destroy_event(d->event);
    pthread_mutex_destroy(&d->mu); free(d); *(cv_dec **)t = NULL;
    return 0;
}
static Result cv_dec_reg(void *t, uint32_t i, void *pool, uint32_t pidx) {
    cv_dec *d = cv_of(t); if (!d || i >= CV_BUFS) { LOG("[RED] Register...DecoderBuffer index %u", i); return CV_R(2004); }
    pthread_mutex_lock(&d->mu);
    d->buf[i] = (cv_buf){.pool = pool_of(pool), .pidx = pidx, .reg = 1};
    if (d->fcount < 8) { d->fifo[(d->fhead + d->fcount) % 8] = i; d->fcount++; } else LOG("[RED] acquirable queue full at register %u", i);
    pthread_mutex_unlock(&d->mu);
    return 0;
}
static void cv_drop_index(uint32_t *q, uint32_t *head, uint32_t *count, uint32_t cap, uint32_t i) {   /* removes i from a ring of indices, keeping the others' order */
    uint32_t n = 0, tmp[CV_BUFS];
    for (uint32_t k = 0; k < *count; k++) { uint32_t v = q[(*head + k) % cap]; if (v != i) tmp[n++] = v; }
    for (uint32_t k = 0; k < n; k++) q[k] = tmp[k];
    *head = 0; *count = n;
}
static Result cv_dec_unreg(void *t, uint32_t i) {   /* the buffer leaves the decoder: no longer acquirable, filled or waited for */
    cv_dec *d = cv_of(t); if (!d || i >= CV_BUFS) return CV_R(2004);
    pthread_mutex_lock(&d->mu);
    d->buf[i].reg = 0; d->buf[i].filled = 0; d->buf[i].pool = NULL; d->buf[i].gen++;
    cv_drop_index(d->fifo, &d->fhead, &d->fcount, 8, i); cv_drop_index(d->pend, &d->phead, &d->pcount, CV_BUFS, i);
    pthread_mutex_unlock(&d->mu);
    return 0;
}
static Result cv_dec_reset(void *t) {   /* Stop / Flush: undelivered pictures and a pending end of stream are dropped; next key frame starts over */
    cv_dec *d = cv_of(t); if (!d) return 0;
    pthread_mutex_lock(&d->mu);
    for (int i = 0; i < d->nheld; i++) vpx_img_free(d->held[i]);
    d->nheld = 0; d->eos_pending = 0;
    cv_open(d);
    if (d->event) hle_signal_system_event(d->event);
    pthread_mutex_unlock(&d->mu);
    return 0;
}
/* A started decoder that has room takes packets, whatever became of the signal Initialize gave (Mario Tennis Fever's player
   clears that one and counts only later signals). */
static Result cv_dec_start(void *t) {
    cv_dec *d = cv_of(t); if (!d) return CV_R(2004);
    pthread_mutex_lock(&d->mu);
    if (d->event && d->nheld < CV_READY) hle_signal_system_event(d->event);
    pthread_mutex_unlock(&d->mu);
    return 0;
}
static Result cv_dec_event(void **out, void *t) { cv_dec *d = cv_of(t); *out = d ? d->event : NULL; return d ? 0 : CV_R(2004); }
static int cv_hold(cv_dec *d, vpx_image_t *c, int64_t ts) {   /* a decoded picture waits for a released buffer (decoder lock held) */
    if (d->nheld >= CV_HOLD) { static int n; if (n++ < 4) LOG("[RED] %s: a packet gave more pictures than the host holds: one dropped", k_cv_name[d->codec]); vpx_img_free(c); return 0; }
    d->held[d->nheld] = c; d->held_ts[d->nheld] = ts; d->held_eos[d->nheld] = 0; d->nheld++; return 1;
}
static vpx_image_t *av1_to_i420(const mav1_picture *p) {   /* the decoder's NV12, rows at its strides, as the I420 the pool keeps */
    vpx_image_t *c = vpx_img_alloc(NULL, VPX_IMG_FMT_I420, (unsigned)p->w, (unsigned)p->h, 1); if (!c) return NULL;
    c->cs = p->matrix == 2 ? VPX_CS_BT_709 : p->matrix == 5 ? VPX_CS_BT_2020 : VPX_CS_BT_601; c->range = p->full_range ? VPX_CR_FULL_RANGE : VPX_CR_STUDIO_RANGE;
    for (int y = 0; y < p->h; y++) memcpy(c->planes[0] + (size_t)y * c->stride[0], p->y + (size_t)y * p->ystride, (size_t)p->w);
    for (int y = 0; y < (p->h + 1) / 2; y++) {
        const uint8_t *uv = p->uv + (size_t)y * p->uvstride; uint8_t *u = c->planes[1] + (size_t)y * c->stride[1], *v = c->planes[2] + (size_t)y * c->stride[2];
        for (int x = 0; x < (p->w + 1) / 2; x++) { u[x] = uv[2 * x]; v[x] = uv[2 * x + 1]; }
    }
    return c;
}
/* One packet: decoded at once; its pictures wait for released buffers. A refusal (CV_BUSY, 3603) is the decoder's defined
   answer: the movie library's SubmitDecoderInputBuffer turns exactly that into its own retry code and aborts on any other
   failure (Ocarina of Time's subsdk0 +0x10fd20, +0x103840, per nxnative). */
static Result cv_dec_submit(uint64_t *out, void *t, const void *data, uint64_t size, int64_t ts, bool eos) {
    cv_dec *d = cv_of(t); if (!d) return CV_R(2004);
    pthread_mutex_lock(&d->mu);
    if (d->nheld >= CV_HOLD) { static int n; if (n++ < 4) LOG("%s: busy (result 3603), a packet refused with %d pictures waiting for a released buffer", k_cv_name[d->codec], d->nheld); if (d->event) hle_clear_system_event(d->event); pthread_mutex_unlock(&d->mu); return CV_BUSY; }
    int got = 0;
    if (d->codec == CV_AV1) {
        if (size && d->av1) {
            mav1_picture pic;
            if (mav1_decode(d->av1, data, (size_t)size, &pic) == 1) { vpx_image_t *c = av1_to_i420(&pic); if (c) got += cv_hold(d, c, ts); }
        }
    } else if (size && d->vpx_ok) {
        if (vpx_codec_decode(&d->vpx, data, (unsigned)size, NULL, 0) != VPX_CODEC_OK) { static int n; if (n++ < 4) LOG("[RED] %s: %s", k_cv_name[d->codec], vpx_codec_error(&d->vpx)); }
        vpx_codec_iter_t it = NULL; const vpx_image_t *im;
        while ((im = vpx_codec_get_frame(&d->vpx, &it))) { vpx_image_t *c = img_copy(im); if (c) got += cv_hold(d, c, ts); }
    }
    if (eos) { if (got) d->held_eos[d->nheld - 1] = 1; else d->eos_pending = 1; }
    *out = ++d->submits;
    drain(d, 1);
    pthread_mutex_unlock(&d->mu);
    return 0;
}
static Result cv_dec_acq(uint32_t *idx, cv_sync *sync, void *t) {
    cv_dec *d = cv_of(t); if (!d) return CV_R(2004);
    pthread_mutex_lock(&d->mu);
    if (!d->fcount) { pthread_mutex_unlock(&d->mu); return CV_NONE; }
    uint32_t i = d->fifo[d->fhead]; d->fhead = (d->fhead + 1) % 8; d->fcount--;
    *idx = i; *sync = (cv_sync){CV_SYNC, d->id, i, d->buf[i].gen};
    pthread_mutex_unlock(&d->mu);
    return 0;
}
static void cv_dec_rel(void *t, uint32_t i, cv_sync sync) {
    (void)sync; cv_dec *d = cv_of(t); if (!d || i >= CV_BUFS || !d->buf[i].reg) { LOG("[RED] Release...DecoderOutputBuffer index %u", i); return; }
    pthread_mutex_lock(&d->mu);
    d->buf[i].gen++; d->buf[i].filled = 0;
    if (d->pcount < CV_BUFS) { d->pend[(d->phead + d->pcount) % CV_BUFS] = i; d->pcount++; }
    if (d->fcount < 8) { d->fifo[(d->fhead + d->fcount) % 8] = i; d->fcount++; } else LOG("[RED] acquirable queue full at release %u", i);
    drain(d, 0);
    pthread_mutex_unlock(&d->mu);
}
static Result cv_dec_props(uint16_t *w, uint16_t *h, uint16_t *cw, uint16_t *ch, int64_t *ts, bool *eos, void *t, uint32_t i) {
    cv_dec *d = cv_of(t); if (!d || i >= CV_BUFS) return CV_R(2004);
    pthread_mutex_lock(&d->mu);
    int ok = d->buf[i].filled;
    if (ok) { *w = *cw = d->buf[i].w; *h = *ch = d->buf[i].h; *ts = d->buf[i].ts; *eos = d->buf[i].eos != 0; }
    pthread_mutex_unlock(&d->mu);
    return ok ? 0 : CV_NONE;
}
static Result cv_dec_encrypted(void) { static int once; if (!once++) LOG("[RED] an encrypted input buffer: protected streams are not served"); return CV_R(2004); }
/* Each decoder type's entry points, by their names in the SDK */
#define CV_FAMILY(tag, codec, s_work, s_init, s_init2, s_fin, s_reg, s_unreg, s_start, s_stop, s_flush, s_ev, s_sub, s_enc, s_acq, s_rel, s_props) \
    HLE_FORCE(s_work, "no work memory", Result, h_cv_##tag##work, uint64_t *size, uint64_t *align, uint64_t bs, uint16_t w, uint16_t h) { (void)bs; (void)w; (void)h; *size = 0x1000; *align = 0x1000; return 0; } \
    HLE_FORCE(s_init, "decoded on the host", Result, h_cv_##tag##init, void *t, void *work, uint64_t wsize, uint64_t bs, uint16_t w, uint16_t h, uint32_t refs, uint32_t in, uint32_t out) { (void)work; (void)wsize; (void)bs; (void)refs; return cv_dec_init(t, codec, w, h, in, out); } \
    HLE_FORCE(s_init2, "decoded on the host", Result, h_cv_##tag##init2, void *t, uint64_t bs, uint16_t w, uint16_t h, uint32_t refs, uint32_t in, uint32_t out) { (void)bs; (void)refs; return cv_dec_init(t, codec, w, h, in, out); } \
    HLE_FORCE(s_fin, "frees the host decoder", Result, h_cv_##tag##fin, void *t) { return cv_dec_fin(t); } \
    HLE_FORCE(s_reg, "a buffer of the host decoder", Result, h_cv_##tag##reg, void *t, uint32_t i, void *pool, uint32_t pidx) { return cv_dec_reg(t, i, pool, pidx); } \
    HLE_FORCE(s_unreg, "the buffer leaves the host decoder", Result, h_cv_##tag##unreg, void *t, uint32_t i) { return cv_dec_unreg(t, i); } \
    HLE_FORCE(s_start, "ready for packets", Result, h_cv_##tag##start, void *t) { return cv_dec_start(t); } \
    HLE_FORCE(s_stop, "drops undelivered pictures", Result, h_cv_##tag##stop, void *t) { return cv_dec_reset(t); } \
    HLE_FORCE(s_flush, "drops undelivered pictures", Result, h_cv_##tag##flush, void *t) { return cv_dec_reset(t); } \
    HLE_FORCE(s_ev, "signalled while the host decoder takes packets", Result, h_cv_##tag##event, void **out, void *t) { return cv_dec_event(out, t); } \
    HLE_FORCE(s_sub, "decodes the packet on the submitting thread", Result, h_cv_##tag##submit, uint64_t *out, void *t, const void *data, uint64_t size, int64_t ts, bool eos) { return cv_dec_submit(out, t, data, size, ts, eos); } \
    HLE_FORCE(s_enc, "protected streams are not served", Result, h_cv_##tag##enc, void) { return cv_dec_encrypted(); } \
    HLE_FORCE(s_acq, "the oldest acquirable buffer", Result, h_cv_##tag##acq, uint32_t *idx, cv_sync *sync, void *t) { return cv_dec_acq(idx, sync, t); } \
    HLE_FORCE(s_rel, "the buffer waits for the next picture", void, h_cv_##tag##rel, void *t, uint32_t i, cv_sync sync) { cv_dec_rel(t, i, sync); } \
    HLE_FORCE(s_props, "the filled buffer's picture", Result, h_cv_##tag##props, uint16_t *w, uint16_t *h, uint16_t *cw, uint16_t *ch, int64_t *ts, bool *eos, void *t, uint32_t i) { return cv_dec_props(w, h, cw, ch, ts, eos, t, i); }
#define CV_NAMES(C, N) "_ZN2nn5codec29Calculate" C "DecoderWorkBufferEPmS1_mtt", \
    "_ZN2nn5codec20Initialize" C "DecoderEPNS0_14" C "DecoderTypeEPvmmttjNS0_23VideoDecoderInputFormatENS0_24VideoDecoderOutputFormatE", \
    "_ZN2nn5codec20Initialize" C "DecoderEPNS0_14" C "DecoderTypeEmttjNS0_23VideoDecoderInputFormatENS0_24VideoDecoderOutputFormatE", \
    "_ZN2nn5codec18Finalize" C "DecoderEPNS0_14" C "DecoderTypeE", "_ZN2nn5codec24Register" C "DecoderBufferEPNS0_14" C "DecoderTypeEjPNS0_25VideoDecoderImagePoolTypeEj", \
    "_ZN2nn5codec26Unregister" C "DecoderBufferEPNS0_14" C "DecoderTypeEj", "_ZN2nn5codec15Start" C "DecoderEPNS0_14" C "DecoderTypeE", "_ZN2nn5codec14Stop" C "DecoderEPNS0_14" C "DecoderTypeE", \
    "_ZN2nn5codec15Flush" C "DecoderEPNS0_14" C "DecoderTypeE", "_ZN2nn5codec33Get" C "DecoderSubmissionReadyEventEPPNS_2os15SystemEventTypeEPNS0_14" C "DecoderTypeE", \
    "_ZN2nn5codec27Submit" C "DecoderInputBufferEPmPNS0_14" C "DecoderTypeEPKvmNS_8TimeSpanEb", \
    "_ZN2nn5codec36SubmitEncrypted" C "DecoderInputBufferEPmPNS0_14" C "DecoderTypeEPKNS0_18EncryptionPropertyEPKvmNS_8TimeSpanEb", \
    "_ZN2nn5codec29Acquire" C "DecoderOutputBufferEPjPNS_2gs16NativeSyncHandleEPNS0_14" C "DecoderTypeE", "_ZN2nn5codec29Release" C "DecoderOutputBufferEPNS0_14" C "DecoderTypeEjNS_2gs16NativeSyncHandleE", \
    "_ZN2nn5codec35Get" C "DecoderOutputBufferPropertiesEPtS1_S1_S1_PNS_8TimeSpanEPbPNS0_14" C "DecoderTypeEj"
#define CV_FAMILY_(tag, codec, names) CV_FAMILY(tag, codec, names)
CV_FAMILY_(vp9, CV_VP9, CV_NAMES("Vp9", 3))
CV_FAMILY_(vp8, CV_VP8, CV_NAMES("Vp8", 3))
CV_FAMILY_(av1, CV_AV1, CV_NAMES("Av1", 3))
/* H.264's names are one character longer (H264DecoderType: NS0_15, and its own lengths); its Initialize also takes the
   parameter sets. No decoder here: Initialize fails; the rest answer as for the others. */
HLE_FORCE("_ZN2nn5codec19FinalizeH264DecoderEPNS0_15H264DecoderTypeE", "frees the host decoder", Result, h_cv_h264fin, void *t) { return cv_dec_fin(t); }
HLE_FORCE("_ZN2nn5codec16StartH264DecoderEPNS0_15H264DecoderTypeE", "ready for packets", Result, h_cv_h264start, void *t) { return cv_dec_start(t); }
HLE_FORCE("_ZN2nn5codec15StopH264DecoderEPNS0_15H264DecoderTypeE", "drops undelivered pictures", Result, h_cv_h264stop, void *t) { return cv_dec_reset(t); }
HLE_FORCE("_ZN2nn5codec16FlushH264DecoderEPNS0_15H264DecoderTypeE", "drops undelivered pictures", Result, h_cv_h264flush, void *t) { return cv_dec_reset(t); }
HLE_FORCE("_ZN2nn5codec37InitializeH264DecoderByTransferMemoryEPNS0_15H264DecoderTypeEPvmmttjNS0_23VideoDecoderInputFormatENS0_24VideoDecoderOutputFormatENS0_22VideoDecoderOutputModeEPKvm",
          "no H.264 decoder on this host", Result, h_cv_h264tm, void *t) { (void)t; return cv_dec_init(t, CV_H264, 0, 0, 0, 0); }
HLE_FORCE("_ZN2nn5codec35InitializeH264DecoderByRemoteMemoryEPNS0_15H264DecoderTypeEmttjNS0_23VideoDecoderInputFormatENS0_24VideoDecoderOutputFormatENS0_22VideoDecoderOutputModeEPKvm",
          "no H.264 decoder on this host", Result, h_cv_h264rm, void *t) { (void)t; return cv_dec_init(t, CV_H264, 0, 0, 0, 0); }
/* SDK 22's forms (the movie module's). ByTransferMemory has ten arguments and ByRemoteMemory eight: past the eight registers
   each has its own 8-byte stack slot (AAPCS64; hle_stack.S copies the guest's), so they are declared 64 bits wide; the
   16-bit sizes are masked, as the guest does not extend them */
#define CV_SDK22(tag, codec, s_tm, s_rm, s_ev) \
    HLE_FORCE(s_tm, "decoded on the host", Result, h_cv_##tag##tm, void *t, void *work, uint64_t wsize, uint64_t bs, uint64_t w, uint64_t h, uint32_t refs, uint32_t in, uint64_t out, uint64_t mode) { (void)work; (void)wsize; (void)bs; (void)refs; (void)mode; return cv_dec_init(t, codec, (uint16_t)w, (uint16_t)h, in, (uint32_t)out); } \
    HLE_FORCE(s_rm, "decoded on the host", Result, h_cv_##tag##rm, void *t, uint64_t bs, uint64_t w, uint64_t h, uint32_t refs, uint32_t in, uint32_t out, uint32_t mode) { (void)bs; (void)refs; (void)mode; return cv_dec_init(t, codec, (uint16_t)w, (uint16_t)h, in, out); } \
    HLE_FORCE(s_ev, "signalled while the host decoder takes packets", void *, h_cv_##tag##ev22, void *t) { cv_dec *d = cv_of(t); return d ? d->event : NULL; }
CV_SDK22(vp9, CV_VP9, "_ZN2nn5codec36InitializeVp9DecoderByTransferMemoryEPNS0_14Vp9DecoderTypeEPvmmttjNS0_23VideoDecoderInputFormatENS0_24VideoDecoderOutputFormatENS0_22VideoDecoderOutputModeE",
    "_ZN2nn5codec34InitializeVp9DecoderByRemoteMemoryEPNS0_14Vp9DecoderTypeEmttjNS0_23VideoDecoderInputFormatENS0_24VideoDecoderOutputFormatENS0_22VideoDecoderOutputModeE",
    "_ZN2nn5codec33GetVp9DecoderSubmissionReadyEventEPNS0_14Vp9DecoderTypeE")
CV_SDK22(vp8, CV_VP8, "_ZN2nn5codec36InitializeVp8DecoderByTransferMemoryEPNS0_14Vp8DecoderTypeEPvmmttjNS0_23VideoDecoderInputFormatENS0_24VideoDecoderOutputFormatENS0_22VideoDecoderOutputModeE",
    "_ZN2nn5codec34InitializeVp8DecoderByRemoteMemoryEPNS0_14Vp8DecoderTypeEmttjNS0_23VideoDecoderInputFormatENS0_24VideoDecoderOutputFormatENS0_22VideoDecoderOutputModeE",
    "_ZN2nn5codec33GetVp8DecoderSubmissionReadyEventEPNS0_14Vp8DecoderTypeE")
CV_SDK22(av1, CV_AV1, "_ZN2nn5codec36InitializeAv1DecoderByTransferMemoryEPNS0_14Av1DecoderTypeEPvmmttjNS0_23VideoDecoderInputFormatENS0_24VideoDecoderOutputFormatENS0_22VideoDecoderOutputModeE",
    "_ZN2nn5codec34InitializeAv1DecoderByRemoteMemoryEPNS0_14Av1DecoderTypeEmttjNS0_23VideoDecoderInputFormatENS0_24VideoDecoderOutputFormatENS0_22VideoDecoderOutputModeE",
    "_ZN2nn5codec33GetAv1DecoderSubmissionReadyEventEPNS0_14Av1DecoderTypeE")
HLE_FORCE("_ZN2nn5codec34GetH264DecoderSubmissionReadyEventEPNS0_15H264DecoderTypeE", "signalled while the host decoder takes packets", void *, h_cv_h264ev22, void *t) { cv_dec *d = cv_of(t); return d ? d->event : NULL; }

/* The host's sync handles signal when their buffer holds the picture of that release; a handle the host did not make reads
   as signalled */
static bool sync_reached(const cv_sync *s) {
    if (s->magic != CV_SYNC) { static int once; if (!once++ && (s->magic || s->dec)) LOG("[RED] a sync handle the host did not make (%#x): signalled", s->magic); return true; }
    if (!s->gen) return true;
    pthread_mutex_lock(&g_cv_mu);
    cv_dec *d = s->dec && s->dec <= 16 ? g_cv[s->dec - 1] : NULL;
    bool r = true;
    if (d && s->idx < CV_BUFS) { pthread_mutex_lock(&d->mu); r = d->buf[s->idx].filled && d->buf[s->idx].filled_gen >= s->gen; pthread_mutex_unlock(&d->mu); }
    pthread_mutex_unlock(&g_cv_mu);
    return r;
}
HLE_FORCE("_ZN2nn2gs17TryWaitNativeSyncERKNS0_16NativeSyncHandleE", "decoder sync handles are the host's", bool, h_gs_trywait, const cv_sync *s) { return sync_reached(s); }
HLE_FORCE("_ZN2nn6syncpt3sdk20CreateSyncPointEventEPNS1_18SyncPointEventTypeE", "a host-bound SystemEvent inside the event type", Result, h_spt_create, void *e) {
    memset(e, 0, 0x58); hle_bind_system_event(e, 0); SPT(e)->magic = CV_SYNC;
    return 0;
}
HLE_FORCE("_ZN2nn6syncpt3sdk25InitializeMultiWaitHolderEPNS_2os19MultiWaitHolderTypeEPNS1_18SyncPointEventTypeE", "links the event's SystemEvent", void, h_spt_holder, void *holder, void *e) {
    static void (*init)(void *, void *);
    if (!init && !(init = guest_sym("_ZN2nn2os25InitializeMultiWaitHolderEPNS0_19MultiWaitHolderTypeEPNS0_15SystemEventTypeE"))) die("nn::os::InitializeMultiWaitHolder missing");
    init(holder, e);
}
HLE_FORCE("_ZN2nn6syncpt3sdk21RequestSyncPointEventEPNS1_18SyncPointEventTypeERKNS1_15SyncPointHandleE", "signalled when the decoder fills the awaited buffer",
          Result, h_spt_request, void *e, const cv_sync *s) {
    spt_unarm(e); hle_clear_system_event(e);
    SPT(e)->want = *s; SPT(e)->armed = 1;
    pthread_mutex_lock(&g_spt_mu); int k = 0; while (k < 64 && g_spt[k]) k++; if (k < 64) g_spt[k] = e; pthread_mutex_unlock(&g_spt_mu);
    if (k == 64) { LOG("[RED] nn::syncpt: more than 64 armed events: signalled at once"); hle_signal_system_event(e); return 0; }
    if (sync_reached(s)) { spt_unarm(e); hle_signal_system_event(e); }
    return 0;
}
HLE_FORCE("_ZN2nn6syncpt3sdk20CancelSyncPointEventEPNS1_18SyncPointEventTypeE", "disarms the event", void, h_spt_cancel, void *e) { spt_unarm(e); hle_clear_system_event(e); }
HLE_FORCE("_ZN2nn6syncpt3sdk21TryWaitSyncPointEventEPKNS1_18SyncPointEventTypeE", "the event's SystemEvent is signalled", bool, h_spt_trywait, const void *e) {
    static uint32_t (*tw)(void *);
    if (!tw && !(tw = guest_sym("_ZN2nn2os18TryWaitSystemEventEPNS0_15SystemEventTypeE"))) die("nn::os::TryWaitSystemEvent missing");
    return (tw((void *)e) & 0xff) != 0;
}
HLE_FORCE("_ZN2nn6syncpt3sdk21DestroySyncPointEventEPNS1_18SyncPointEventTypeE", "destroys the event's SystemEvent", void, h_spt_destroy, void *e) {
    spt_unarm(e); destroy_event(e); SPT(e)->magic = 0;
}

void *runtime_ns2_codec_override(const char *name) {
    for (int i = 0; i < g_cv_ntab; i++) if (!strcmp(name, g_cv_tab[i].name)) return g_cv_tab[i].fn;
    return NULL;
}
