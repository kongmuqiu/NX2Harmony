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
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#define WIN(w) ((OHNativeWindow *)(void *)(w))

static struct {
    ANativeWindow *window;            /* the surface the size below belongs to */
    _Atomic int32_t width, height;    /* XComponent surface size */
    int32_t buf_w, buf_h, format;     /* last geometry set */
    OHNativeWindowBuffer *locked; void *map; size_t map_size; int mapped, fd, synced;
    int32_t lw, lh;                   /* the locked buffer's size */
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
    S.locked = b; S.lw = h->width; S.lh = h->height;
    memset(out, 0, sizeof *out);
    out->width = h->width; out->height = h->height; out->stride = h->stride / 4; out->format = S.format; out->bits = bits;
    return 0;
}

int32_t ANativeWindow_unlockAndPost(ANativeWindow *w) {
    if (!S.locked) return -EINVAL;
    __asm__ volatile("dsb sy" ::: "memory");   /* every row store reached the point of coherency */
    if (S.synced) { dmabuf_sync(S.fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE); S.synced = 0; }
    if (S.mapped) { munmap(S.map, S.map_size); S.mapped = 0; }
    struct Rect full = {0, 0, (uint32_t)S.lw, (uint32_t)S.lh};
    Region region = {&full, 1};
    int e = OH_NativeWindow_NativeWindowFlushBuffer(WIN(w), S.locked, -1, region);
    S.locked = NULL;
    return e ? -EIO : 0;
}
