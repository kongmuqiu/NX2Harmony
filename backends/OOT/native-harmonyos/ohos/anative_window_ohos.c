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
#include <errno.h>
#include <linux/dma-buf.h>
#include <poll.h>
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
    int32_t lw, lh, lstride;          /* the locked buffer's size, its row pitch in pixels */
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

int32_t ANativeWindow_setBuffersGeometry(ANativeWindow *w, int32_t width, int32_t height, int32_t format) {
    int32_t pixel = format == WINDOW_FORMAT_RGBX_8888 ? NATIVEBUFFER_PIXEL_FMT_RGBX_8888 : NATIVEBUFFER_PIXEL_FMT_RGBA_8888;
    uint64_t usage = NATIVEBUFFER_USAGE_CPU_READ | NATIVEBUFFER_USAGE_CPU_WRITE | NATIVEBUFFER_USAGE_MEM_DMA;
    int e = OH_NativeWindow_NativeWindowHandleOpt(WIN(w), SET_BUFFER_GEOMETRY, width, height);
    if (!e) e = OH_NativeWindow_NativeWindowHandleOpt(WIN(w), SET_FORMAT, pixel);
    if (!e) e = OH_NativeWindow_NativeWindowHandleOpt(WIN(w), SET_USAGE, usage);
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
    return 0;
}

/* Frame recorder (oot-a118; the picture's flashes are seen frame by frame, not in single screenshots):
   TOTK_OHOS_FRAME_REC=<frames>[:<div>] arms it; it starts when the file "frame-rec-go" appears in the work folder
   (files/android; oot/frame_rec.ps1 sends it), checked every 15 presents, and then writes the next <frames> presented
   frames, every <div>-th pixel of every <div>-th row (default 4) as RGB, to frame-rec.raw: per frame a 24-byte header
   {"FRM1", frame index, microseconds since the first recorded frame (u64), width, height} and the pixels. The go file
   is removed when the recording ends, so it runs once a request. Off: one test of a static a present. */
static struct { int on, count, div, active, done, n, polls; FILE *f; uint8_t *row; uint64_t t0; } R = {-1};
static uint64_t rec_now_us(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000u + (uint64_t)t.tv_nsec / 1000u; }
static void frame_rec(const uint8_t *bits, int32_t w, int32_t h, int32_t stride_px) {
    if (R.on < 0) {
        const char *e = getenv("TOTK_OHOS_FRAME_REC"); R.on = 0;
        if (e && *e) { R.count = atoi(e); const char *c = strchr(e, ':'); R.div = c ? atoi(c + 1) : 4; if (R.div < 1) R.div = 1;
            R.on = R.count > 0; if (R.on) fprintf(stderr, "[totk-ohos] frame recorder armed: %d frames, every %d pixel (send frame-rec-go)\n", R.count, R.div); }
    }
    if (!R.on) return;
    if (!R.active) {
        if (++R.polls % 15) return;
        if (access("frame-rec-go", F_OK)) return;
        R.f = fopen("frame-rec.raw", "wb");
        if (!R.f) { fprintf(stderr, "[totk-ohos] frame recorder: cannot open frame-rec.raw: %s\n", strerror(errno)); unlink("frame-rec-go"); return; }
        R.active = 1; R.n = 0; R.t0 = rec_now_us();
        fprintf(stderr, "[totk-ohos] frame recorder: started (%d frames of %dx%d)\n", R.count, w / R.div, h / R.div);
    }
    int32_t ow = w / R.div, oh = h / R.div;
    if (!R.row) R.row = malloc((size_t)ow * 3 + 64);
    if (!R.row) return;
    uint32_t hdr[6]; uint64_t t = rec_now_us() - R.t0;
    hdr[0] = 0x314d5246u; hdr[1] = (uint32_t)R.n; memcpy(&hdr[2], &t, 8); hdr[4] = (uint32_t)ow; hdr[5] = (uint32_t)oh;
    fwrite(hdr, 1, sizeof hdr, R.f);
    for (int32_t y = 0; y < oh; y++) {
        const uint8_t *src = bits + (size_t)(y * R.div) * (size_t)stride_px * 4;
        uint8_t *d = R.row;
        for (int32_t x = 0; x < ow; x++) { const uint8_t *p = src + (size_t)(x * R.div) * 4; d[0] = p[0]; d[1] = p[1]; d[2] = p[2]; d += 3; }
        fwrite(R.row, 1, (size_t)ow * 3, R.f);
    }
    if (++R.n >= R.count) {
        fclose(R.f); R.f = NULL; R.active = 0; unlink("frame-rec-go");
        fprintf(stderr, "[totk-ohos] frame recorder: %d frames %dx%d written to frame-rec.raw in %.2f s\n", R.n, ow, oh, (rec_now_us() - R.t0) / 1e6);
    }
}

int32_t ANativeWindow_unlockAndPost(ANativeWindow *w) {
    if (!S.locked) return -EINVAL;
    if (__builtin_expect(R.on != 0, 0) && S.mapped) frame_rec((const uint8_t *)S.map, S.lw, S.lh, S.lstride);
    __asm__ volatile("dsb sy" ::: "memory");   /* every row store reached the point of coherency */
    if (S.synced) { dmabuf_sync(S.fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE); S.synced = 0; }
    if (S.mapped) { munmap(S.map, S.map_size); S.mapped = 0; }
    struct Rect full = {0, 0, (uint32_t)S.lw, (uint32_t)S.lh};
    Region region = {&full, 1};
    int e = OH_NativeWindow_NativeWindowFlushBuffer(WIN(w), S.locked, -1, region);
    S.locked = NULL;
    return e ? -EIO : 0;
}
