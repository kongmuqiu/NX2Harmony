/* ANativeWindow (native-harmonyos/compat/android/native_window.h) on HarmonyOS's OHNativeWindow, for the presenter's copy
   path: request a buffer, wait for its release fence, map it, hand out its rows; unlockAndPost unmaps and flushes it.
   One surface at a time (the game's), and only the presenter thread locks it, as with the NDK window.
   The display controller scans the buffer out of DRAM without snooping CPU caches, while the GPU compositor (and so
   every screenshot) is I/O-coherent: CPU writes still in caches or write buffers when the buffer is flushed show up as
   flicker on the panel only. So the CPU access is bracketed with DMA_BUF_IOCTL_SYNC (cache maintenance by the buffer's
   exporter; nothing to do for an uncached buffer), the mapping is always our own and unmapped before the hand-off,
   and a full barrier precedes FlushBuffer, which then gets the whole buffer as its damage. */
#include <android/native_window.h>
#include <native_window/external_window.h>
#include <native_buffer/native_buffer.h>
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <linux/dma-buf.h>
#include <poll.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#define WIN(w) ((OHNativeWindow *)(void *)(w))

static struct {
    ANativeWindow *window;            /* the surface the size below belongs to */
    _Atomic int32_t width, height;    /* XComponent surface size */
    int32_t buf_w, buf_h, format;     /* last geometry set */
    OHNativeWindowBuffer *locked; void *map; size_t map_size; int mapped, fd, synced;
    int32_t lw, lh, lstride;          /* the locked buffer's size, its row pitch in pixels (totk-ohos: the frame recorder) */
} S;

void anw_ohos_set_size(ANativeWindow *w, int32_t width, int32_t height) {
    S.window = w; atomic_store(&S.width, width); atomic_store(&S.height, height);
}
void ANativeWindow_acquire(ANativeWindow *w) { if (w) OH_NativeWindow_NativeObjectReference(w); }
void ANativeWindow_release(ANativeWindow *w) { if (w) OH_NativeWindow_NativeObjectUnreference(w); }
int32_t ANativeWindow_getWidth(ANativeWindow *w) {
    if (w == S.window && atomic_load(&S.width) > 0) return atomic_load(&S.width);
    int32_t h = 0, wd = 0; OH_NativeWindow_NativeWindowHandleOpt(WIN(w), GET_BUFFER_GEOMETRY, &h, &wd); return wd;
}
int32_t ANativeWindow_getHeight(ANativeWindow *w) {
    if (w == S.window && atomic_load(&S.height) > 0) return atomic_load(&S.height);
    int32_t h = 0, wd = 0; OH_NativeWindow_NativeWindowHandleOpt(WIN(w), GET_BUFFER_GEOMETRY, &h, &wd); return h;
}
int32_t ANativeWindow_getFormat(ANativeWindow *w) { (void)w; return S.format ? S.format : WINDOW_FORMAT_RGBA_8888; }

/* totk-ohos TOTK_OHOS_PRESENT=source (overlay/android/gfx/vk_draw.c, pres_src_mode): the presenter hands window buffers
   of the game image's own size (16:9) when its copy would only upscale, and the compositor stretches them onto the
   surface (2.16:1 on the phone) in both directions: OH_SCALING_MODE_SCALE_TO_WINDOW_V2, set after every geometry change
   from then on (the queue's new buffers take it). OH_NativeWindow_NativeWindowSetScalingModeV2 (API 12) is looked up at
   run time, so a system without it only logs and the presenter keeps its copy; a failed call does the same from the
   next frame on. The default copy never calls anw_ohos_present_stretch, so nothing here runs for it. */
typedef int32_t (*anw_scaling_v2_fn)(OHNativeWindow *, OHScalingModeV2);
static anw_scaling_v2_fn anw_scaling_v2;
static _Atomic int anw_stretch;   /* 0 off (default), 1 on, -1 missing or failed: the presenter copies as before */
int anw_ohos_present_stretch(void) {   /* the recording thread, once, before the first source-sized buffer */
    anw_scaling_v2 = (anw_scaling_v2_fn)dlsym(RTLD_DEFAULT, "OH_NativeWindow_NativeWindowSetScalingModeV2");
    atomic_store(&anw_stretch, anw_scaling_v2 ? 1 : -1);
    fprintf(stderr, "[totk-ohos] OH_NativeWindow_NativeWindowSetScalingModeV2 (TOTK_OHOS_PRESENT=source): %s\n",
            anw_scaling_v2 ? "found, the compositor stretches window buffers to the surface" : "missing, the presenter keeps its copy");
    return anw_scaling_v2 != NULL;
}
int anw_ohos_present_stretch_ok(void) { return atomic_load(&anw_stretch) > 0; }

int32_t ANativeWindow_setBuffersGeometry(ANativeWindow *w, int32_t width, int32_t height, int32_t format) {
    int32_t pixel = format == WINDOW_FORMAT_RGBX_8888 ? NATIVEBUFFER_PIXEL_FMT_RGBX_8888 : NATIVEBUFFER_PIXEL_FMT_RGBA_8888;
    uint64_t usage = NATIVEBUFFER_USAGE_CPU_READ | NATIVEBUFFER_USAGE_CPU_WRITE | NATIVEBUFFER_USAGE_MEM_DMA;
    int e = OH_NativeWindow_NativeWindowHandleOpt(WIN(w), SET_BUFFER_GEOMETRY, width, height);
    if (!e) e = OH_NativeWindow_NativeWindowHandleOpt(WIN(w), SET_FORMAT, pixel);
    if (!e) e = OH_NativeWindow_NativeWindowHandleOpt(WIN(w), SET_USAGE, usage);
    if (!e && atomic_load(&anw_stretch) > 0) {   /* totk-ohos TOTK_OHOS_PRESENT=source, above; the presenter thread */
        int r = anw_scaling_v2(WIN(w), OH_SCALING_MODE_SCALE_TO_WINDOW_V2);
        if (r) atomic_store(&anw_stretch, -1);
        static int said;
        if (r || said++ < 4) fprintf(stderr, "[totk-ohos] window scaling SCALE_TO_WINDOW for %dx%d buffers: %d%s\n", width, height, r,
                                     r ? " (failed: the presenter copies window-sized pictures from the next frame on)" : "");
    }
    S.buf_w = width; S.buf_h = height; S.format = format;
    fprintf(stderr, "[totk-ohos] window buffers %dx%d format %d: %d\n", width, height, format, e);
    return e ? -EINVAL : 0;
}

static void dmabuf_sync(int fd, uint64_t flags) {
    struct dma_buf_sync s = {.flags = flags};
    int r = ioctl(fd, DMA_BUF_IOCTL_SYNC, &s), err = errno;
    static int said;
    if (!said++) fprintf(stderr, "[totk-ohos] window buffer DMA_BUF_IOCTL_SYNC: %s\n", r ? strerror(err) : "ok");
}

/* totk-ohos frame recorder (TOTK_OHOS_FRAME_REC; the OoT port's oot-a118 recorder, its ohos/anative_window_ohos.c
   frame_rec, moved off the presenter's critical path): one-frame faults of the picture (white blobs on vegetation, HUD
   pieces displaced or missing, a few cells darker for one frame) are judged from every shown frame, not from
   screenshots; a PC-side script reads the files and another drives a recording (both kept outside this project).
   TOTK_OHOS_FRAME_REC=<presents>[:<div>] arms it; unset (the default) costs one test of a static a present.
   A recording starts when the file frame-rec-go appears in the work folder (files/android, made absolute when armed;
   a writer thread looks every 250 ms) and covers the next <presents> presents. Each present's window buffer (the
   window-sized copy of the default path, or the source-sized one of TOTK_OHOS_PRESENT=source), every <div>-th pixel of
   every <div>-th row (default 4) as RGB, goes to frame-rec-<n>.bin (n one above the highest number there) in OoT's
   FRM1 layout: per frame a 24-byte little-endian header {u32 0x314d5246 "FRM1", u32 present index since the start,
   u64 microseconds since the recording's first present, u32 width, u32 height}, then the pixels row by row. A gap in
   the index is a present the writer could not take (counted as dropped in the stop line).
   The presenter thread only samples the picture it has just copied (the copy's cached source, anw_ohos_rec_source, or
   else the still-mapped buffer) into a preallocated ring slot (paged in by the writer) and posts a semaphore; the
   file, the go-file test and the start/stop lines are the writer's. A full
   ring drops the frame instead of waiting, and when over the first 3 frames, or any 30, the sampling took more than
   twice the copies it rides on (lock to unlockAndPost) the recording stops as aborted, so it never holds the presenter
   more than twice its own copy for more than those frames (the cached source at div 4 took 1.45 ms against a 1.32 ms
   copy on this phone, 2026-11-15: the earlier one-copy bound stopped walking recordings).
   The go file is removed when the recording ends (or cannot start), so it runs once a request. */
enum { REC_IDLE, REC_ON, REC_STOP, REC_SLOTS_MAX = 16, REC_WINDOW = 30, REC_FIRST = 3 };
#define REC_RING_BYTES ((size_t)32 << 20)   /* totk-ohos: ring memory while recording (16 slots at div 4, 11 at div 2) */
static struct {
    int on, count, div;                 /* on: -1 not read yet, 0 off (the default), 1 armed */
    char dir[512];                      /* the work folder, absolute: a later chdir cannot move the files */
    _Atomic int state;                  /* IDLE -> ON (writer: file open, ring ready) -> STOP (presenter) -> IDLE (writer) */
    _Atomic int32_t lw, lh;             /* the last presented buffer's size, which the writer sizes the slots from */
    int nslots; size_t cap;             /* ring slots and each slot's bytes, set by the writer before ON */
    uint8_t *slot[REC_SLOTS_MAX]; uint32_t hdr[REC_SLOTS_MAX][6];
    _Atomic uint32_t head, tail;        /* frames queued by the presenter / taken by the writer */
    sem_t wake;
    /* the presenter's side of one recording: the writer resets it before ON and reads it after STOP */
    int seen, queued, dropped, skipped, aborted, win_n;
    uint64_t t0, t_last, lock_ns, samp_ns, samp_max_ns, copy_ns, win_samp_ns, win_copy_ns;
} R = {.on = -1};
/* totk-ohos frame recorder: the cached source of this present, handed over by the presenter just before unlockAndPost
   (vk_draw.c, the window-sized copy: its readback buffer, HOST_CACHED when the device has it). This phone maps the
   window buffer uncached: sampling it at div 4 took 4.96 ms a frame against the copy's 1.33 ms (ng-l11, 2026-11-15),
   so every recording stopped as aborted after 3 frames. The source holds the same pixels the copy wrote (RGBA, or BGRA
   when swap is set). Without a source (TOTK_OHOS_PRESENT=source, another caller) the mapped buffer is sampled as before.
   Cleared by every unlockAndPost; the presenter thread is the only one that sets or reads it. */
static struct { const uint8_t *p; int32_t w, h, stride; int swap; } RS;
void anw_ohos_rec_source(const void *p, int32_t w, int32_t h, int32_t stride_px, int swap) {
    RS.p = (const uint8_t *)p; RS.w = w; RS.h = h; RS.stride = stride_px; RS.swap = swap;
}
static uint64_t rec_now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec; }
static void rec_sleep_ms(long ms) { struct timespec t = {ms / 1000, (ms % 1000) * 1000000L}; nanosleep(&t, NULL); }
static void rec_free_slots(void) { for (int i = 0; i < REC_SLOTS_MAX; i++) { free(R.slot[i]); R.slot[i] = NULL; } }
static int rec_next_number(void) {   /* one above the highest frame-rec-<n>.bin in the work folder */
    int best = 0; DIR *d = opendir(R.dir); struct dirent *e;
    while (d && (e = readdir(d))) { int k = 0, end = 0; if (sscanf(e->d_name, "frame-rec-%d.bin%n", &k, &end) == 1 && end && !e->d_name[end] && k > best) best = k; }
    if (d) closedir(d);
    return best + 1;
}
static void *rec_writer(void *arg) {   /* totk-ohos: the recorder's thread; the presenter never waits for it */
    (void)arg;
    char go[600], name[600];
    snprintf(go, sizeof go, "%s/frame-rec-go", R.dir);
    for (;;) {
        rec_sleep_ms(250);
        if (access(go, F_OK)) continue;
        int32_t ow = atomic_load(&R.lw) / R.div, oh = atomic_load(&R.lh) / R.div;
        if (ow <= 0 || oh <= 0) continue;   /* nothing presented yet */
        size_t cap = (size_t)ow * (size_t)oh * 3;
        int ns = (int)(REC_RING_BYTES / cap), ok = 1;
        if (ns > REC_SLOTS_MAX) ns = REC_SLOTS_MAX;
        if (ns < 2) ns = 2;
        for (int i = 0; i < ns && ok; i++) { R.slot[i] = malloc(cap); if (R.slot[i]) memset(R.slot[i], 0, cap); else ok = 0; }
        snprintf(name, sizeof name, "%s/frame-rec-%d.bin", R.dir, rec_next_number());
        FILE *f = ok ? fopen(name, "wb") : NULL;
        if (!f) {
            if (ok) fprintf(stderr, "[totk-ohos] frame recorder: cannot open %s: %s; request dropped\n", name, strerror(errno));
            else fprintf(stderr, "[totk-ohos] frame recorder: no memory for %d slots of %zu KiB; request dropped\n", ns, cap >> 10);
            rec_free_slots(); unlink(go); continue;
        }
        while (!sem_trywait(&R.wake)) {}   /* posts left over from the last recording */
        R.nslots = ns; R.cap = cap;
        atomic_store_explicit(&R.head, 0, memory_order_relaxed); atomic_store_explicit(&R.tail, 0, memory_order_relaxed);
        R.seen = R.queued = R.dropped = R.skipped = R.aborted = R.win_n = 0;
        R.t0 = R.t_last = R.samp_ns = R.samp_max_ns = R.copy_ns = R.win_samp_ns = R.win_copy_ns = 0;
        fprintf(stderr, "[totk-ohos] frame recorder: started %s (%d presents of %dx%d, every %d pixel; %d ring slots of %zu KiB)\n",
                name, R.count, ow, oh, R.div, ns, cap >> 10);
        atomic_store_explicit(&R.state, REC_ON, memory_order_release);
        int written = 0, werr = 0; uint64_t bytes = 0; uint32_t fw = 0, fh = 0;
        for (;;) {
            int st = atomic_load_explicit(&R.state, memory_order_acquire);   /* before head: STOP covers every frame queued */
            uint32_t hd = atomic_load_explicit(&R.head, memory_order_acquire), tl = atomic_load_explicit(&R.tail, memory_order_relaxed);
            for (; tl != hd; tl++) {
                int s = (int)(tl % (uint32_t)ns); const uint32_t *hdr = R.hdr[s]; size_t px = (size_t)hdr[4] * hdr[5] * 3;
                if (werr) {}   /* after a failed write the file is cut short: later frames are only taken off the ring */
                else if (fwrite(hdr, 1, 24, f) != 24 || fwrite(R.slot[s], 1, px, f) != px) werr = errno ? errno : EIO;
                else { written++; bytes += 24 + px; fw = hdr[4]; fh = hdr[5]; }
                atomic_store_explicit(&R.tail, tl + 1, memory_order_release);
            }
            if (st == REC_STOP) break;
            struct timespec t; clock_gettime(CLOCK_REALTIME, &t);
            t.tv_nsec += 250000000L; if (t.tv_nsec >= 1000000000L) { t.tv_sec++; t.tv_nsec -= 1000000000L; }
            sem_timedwait(&R.wake, &t);
        }
        if (fclose(f) && !werr) werr = errno ? errno : EIO;
        double secs = R.seen > 1 ? (double)(R.t_last - R.t0) / 1e9 : 0, n = R.queued ? (double)R.queued : 1;
        fprintf(stderr, "[totk-ohos] frame recorder: stopped %s: %d of %d presents written (%ux%u, %.1f MiB), %d dropped (writer behind), "
                "%d skipped (size changed); %.2f s, %.1f presents/s; sampling %.2f ms a frame (max %.2f) against the copy's %.2f ms%s%s%s\n",
                name, written, R.seen, fw, fh, (double)bytes / 1048576.0, R.dropped, R.skipped, secs, secs > 0 ? (R.seen - 1) / secs : 0.0,
                (double)R.samp_ns / 1e6 / n, (double)R.samp_max_ns / 1e6, (double)R.copy_ns / 1e6 / n,
                R.aborted ? "; aborted: the sampling took more than twice the copy" : "", werr ? "; write failed, file cut short: " : "", werr ? strerror(werr) : "");
        unlink(go);   /* totk-ohos: after the stop line, so the PC-side pull script finds it once the go file is gone */
        rec_free_slots();
        atomic_store_explicit(&R.state, REC_IDLE, memory_order_release);
    }
    return NULL;
}
static void rec_arm(void) {   /* totk-ohos: the presenter thread, its first unlockAndPost */
    const char *e = getenv("TOTK_OHOS_FRAME_REC"); R.on = 0;
    if (!e || !*e) return;
    R.count = atoi(e); const char *c = strchr(e, ':'); R.div = c ? atoi(c + 1) : 4;
    if (R.div < 1) R.div = 1;
    if (R.count <= 0) return;
    if (!getcwd(R.dir, sizeof R.dir)) snprintf(R.dir, sizeof R.dir, ".");
    pthread_t th; pthread_attr_t at; pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED); pthread_attr_setstacksize(&at, 256u << 10);
    int r = sem_init(&R.wake, 0, 0) ? errno : pthread_create(&th, &at, rec_writer, NULL);
    pthread_attr_destroy(&at);
    if (r) { fprintf(stderr, "[totk-ohos] frame recorder: no writer thread (%s); off\n", strerror(r)); return; }
    pthread_setname_np(th, "frame-rec");
    R.on = 1;
    fprintf(stderr, "[totk-ohos] frame recorder armed (TOTK_OHOS_FRAME_REC=%s): %d presents, every %d pixel; starts when %s/frame-rec-go appears\n",
            e, R.count, R.div, R.dir);
}
static void rec_present(const uint8_t *bits, int32_t w, int32_t h, int32_t stride_px, int swap) {   /* totk-ohos: the presenter thread */
    if (R.on < 0) rec_arm();
    if (R.on <= 0) return;
    atomic_store_explicit(&R.lw, w, memory_order_relaxed); atomic_store_explicit(&R.lh, h, memory_order_relaxed);
    if (atomic_load_explicit(&R.state, memory_order_acquire) != REC_ON) return;
    uint64_t t_in = rec_now_ns();
    uint32_t k = (uint32_t)R.seen++;
    if (!k) R.t0 = t_in;
    R.t_last = t_in;
    int32_t ow = w / R.div, oh = h / R.div;
    uint32_t hd = atomic_load_explicit(&R.head, memory_order_relaxed);
    if (ow <= 0 || oh <= 0 || (size_t)ow * (size_t)oh * 3 > R.cap) R.skipped++;
    else if (hd - atomic_load_explicit(&R.tail, memory_order_acquire) >= (uint32_t)R.nslots) R.dropped++;
    else {
        int s = (int)(hd % (uint32_t)R.nslots), div = R.div; uint8_t *d = R.slot[s]; uint32_t *hdr = R.hdr[s];
        for (int32_t y = 0; y < oh; y++) {   /* totk-ohos: one 32-bit load a pixel, not three byte loads (the buffer may be uncached) */
            const uint8_t *src = bits + (size_t)(y * div) * (size_t)stride_px * 4;
            for (int32_t x = 0; x < ow; x++, d += 3) {
                uint32_t v; memcpy(&v, src + (size_t)(x * div) * 4, 4);
                if (!swap) { d[0] = (uint8_t)v; d[1] = (uint8_t)(v >> 8); d[2] = (uint8_t)(v >> 16); }
                else { d[0] = (uint8_t)(v >> 16); d[1] = (uint8_t)(v >> 8); d[2] = (uint8_t)v; }   /* BGRA source */
            }
        }
        uint64_t us = (t_in - R.t0) / 1000u;
        hdr[0] = 0x314d5246u; hdr[1] = k; memcpy(&hdr[2], &us, 8); hdr[4] = (uint32_t)ow; hdr[5] = (uint32_t)oh;
        atomic_store_explicit(&R.head, hd + 1, memory_order_release);
        sem_post(&R.wake);
        R.queued++;
        uint64_t ds = rec_now_ns() - t_in, dc = R.lock_ns && t_in > R.lock_ns ? t_in - R.lock_ns : 0;
        R.samp_ns += ds; R.copy_ns += dc; if (ds > R.samp_max_ns) R.samp_max_ns = ds;
        R.win_samp_ns += ds; R.win_copy_ns += dc;
        if (++R.win_n == REC_WINDOW) { if (R.win_samp_ns > 2 * R.win_copy_ns) R.aborted = 1; R.win_n = 0; R.win_samp_ns = R.win_copy_ns = 0; }
        if (R.queued == REC_FIRST && R.samp_ns > 2 * R.copy_ns) R.aborted = 1;   /* totk-ohos: a slow readback costs 3 frames, not 30 */
    }
    if (R.seen >= R.count || R.aborted) { atomic_store_explicit(&R.state, REC_STOP, memory_order_release); sem_post(&R.wake); }
}

int32_t ANativeWindow_lock(ANativeWindow *w, ANativeWindow_Buffer *out, ARect *dirty) {
    (void)dirty;
    if (S.locked) return -EBUSY;
    OHNativeWindowBuffer *b = NULL; int fence = -1;
    int e = OH_NativeWindow_NativeWindowRequestBuffer(WIN(w), &b, &fence);
    if (e || !b) { static int reported; if (!reported++) fprintf(stderr, "[totk-ohos] RequestBuffer failed: %d\n", e); return -EAGAIN; }
    if (fence >= 0) { struct pollfd p = {fence, POLLIN, 0}; poll(&p, 1, 3000); close(fence); }
    BufferHandle *h = OH_NativeWindow_GetBufferHandleFromNative(b);
    if (!h || h->fd < 0) { OH_NativeWindow_NativeWindowAbortBuffer(WIN(w), b); return -EINVAL; }
    void *bits = mmap(NULL, (size_t)h->size, PROT_READ | PROT_WRITE, MAP_SHARED, h->fd, 0);
    if (bits == MAP_FAILED) { OH_NativeWindow_NativeWindowAbortBuffer(WIN(w), b); return -errno; }
    S.mapped = 1; S.map = bits; S.map_size = (size_t)h->size; S.fd = h->fd;
    dmabuf_sync(S.fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE); S.synced = 1;
    S.locked = b; S.lw = h->width; S.lh = h->height; S.lstride = h->stride / 4;
    memset(out, 0, sizeof *out);
    out->width = h->width; out->height = h->height; out->stride = h->stride / 4; out->format = S.format; out->bits = bits;
    if (__builtin_expect(R.on > 0, 0)) R.lock_ns = rec_now_ns();   /* totk-ohos frame recorder: the copy starts */
    return 0;
}

int32_t ANativeWindow_unlockAndPost(ANativeWindow *w) {
    if (!S.locked) return -EINVAL;
    if (__builtin_expect(R.on != 0, 0) && S.mapped) {   /* totk-ohos frame recorder: the cached source when the presenter gave one */
        if (RS.p) rec_present(RS.p, RS.w, RS.h, RS.stride, RS.swap);
        else rec_present((const uint8_t *)S.map, S.lw, S.lh, S.lstride, 0);
    }
    RS.p = NULL;
    __asm__ volatile("dsb sy" ::: "memory");   /* every row store reached the point of coherency */
    if (S.synced) { dmabuf_sync(S.fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE); S.synced = 0; }
    if (S.mapped) { munmap(S.map, S.map_size); S.mapped = 0; }
    struct Rect full = {0, 0, (uint32_t)S.lw, (uint32_t)S.lh};
    Region region = {&full, 1};
    int e = OH_NativeWindow_NativeWindowFlushBuffer(WIN(w), S.locked, -1, region);
    S.locked = NULL;
    return e ? -EIO : 0;
}
