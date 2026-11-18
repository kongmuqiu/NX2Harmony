/* AV1 for the nn::codec decoders (ns2_codec.c) with dav1d: Ocarina of Time's movies (the title's background, the opening)
   are AV1. Ported from seedds/nxnative dm/host/hle_movie_av1.c (MIT), its dav1d backend only: VideoToolbox is Apple's,
   and HarmonyOS phones have no AV1 decoder in OH_AVCodec to rely on. A temporal unit goes in whole, its OBUs in the
   low-overhead format the SDK's demuxer keeps them in; decoding is synchronous and a temporal unit shows at most one
   frame, so there is no queue. dav1d runs two task threads per decoder (the submitting thread waits for the picture). */
#include "ns2_av1.h"
#include "dav1d/dav1d.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#define LOG(...) do { fprintf(stderr, "[ns2-codec] " __VA_ARGS__); fputc('\n', stderr); } while (0)

struct mav1 {
    Dav1dContext *dav1d; Dav1dPicture pic; int has_pic;
    uint8_t *y8, *uv; size_t ny8, nuv;   /* dav1d's picture as NV12: the chroma interleaved, a 10- or 12-bit luma shifted to 8 bits */
    int red;
};
mav1 *mav1_new(void) { return calloc(1, sizeof(mav1)); }
const char *mav1_backend(const mav1 *a) { (void)a; return "dav1d"; }
static void unlock_picture(mav1 *a) { if (a->has_pic) { dav1d_picture_unref(&a->pic); a->has_pic = 0; } }
void mav1_reset(mav1 *a) { unlock_picture(a); if (a->dav1d) dav1d_flush(a->dav1d); }
void mav1_free(mav1 *a) { if (a) { mav1_reset(a); if (a->dav1d) dav1d_close(&a->dav1d); free(a->y8); free(a->uv); free(a); } }

static void d_log(void *cookie, const char *fmt, va_list ap) {
    (void)cookie; char b[256]; vsnprintf(b, sizeof b, fmt, ap);
    size_t l = strlen(b); if (l && b[l - 1] == '\n') b[l - 1] = 0;
    LOG("dav1d: %s", b);
}
static int grow(uint8_t **p, size_t *have, size_t want) {
    if (*have >= want) return 1;
    uint8_t *q = realloc(*p, want); if (!q) return 0;
    *p = q; *have = want; return 1;
}
/* dav1d's planar picture as NV12: the luma in place for 8 bits, else shifted down into y8; the chroma interleaved into uv
   (subsampled from 4:2:2 and 4:4:4, grey for 4:0:0) */
static int dav1d_nv12(mav1 *a, const Dav1dPicture *p, mav1_picture *out) {
    int w = p->p.w, h = p->p.h, sh = p->p.bpc - 8, l = p->p.layout, cw = (w + 1) / 2, ch = (h + 1) / 2;
    int sx = l != DAV1D_PIXEL_LAYOUT_I444, sy = l == DAV1D_PIXEL_LAYOUT_I420;
    if (!grow(&a->uv, &a->nuv, (size_t)cw * 2 * ch) || (sh && !grow(&a->y8, &a->ny8, (size_t)w * h))) return 0;
    const uint8_t *y = p->data[0]; size_t ys = (size_t)p->stride[0];
    if (sh) {
        for (int r = 0; r < h; r++) { const uint16_t *s = (const uint16_t *)(const void *)((const uint8_t *)p->data[0] + (ptrdiff_t)r * p->stride[0]); uint8_t *o = a->y8 + (size_t)r * w; for (int x = 0; x < w; x++) o[x] = (uint8_t)(s[x] >> sh); }
        y = a->y8; ys = (size_t)w;
    }
    for (int r = 0; r < ch; r++) {
        uint8_t *o = a->uv + (size_t)r * cw * 2;
        if (l == DAV1D_PIXEL_LAYOUT_I400) { memset(o, 128, (size_t)cw * 2); continue; }
        const uint8_t *u = (const uint8_t *)p->data[1] + (ptrdiff_t)(sy ? r : 2 * r) * p->stride[1], *v = (const uint8_t *)p->data[2] + (ptrdiff_t)(sy ? r : 2 * r) * p->stride[1];
        for (int x = 0; x < cw; x++) {
            int c = sx ? x : 2 * x;
            o[2 * x] = sh ? (uint8_t)(((const uint16_t *)(const void *)u)[c] >> sh) : u[c]; o[2 * x + 1] = sh ? (uint8_t)(((const uint16_t *)(const void *)v)[c] >> sh) : v[c];
        }
    }
    unsigned mc = p->seq_hdr->mtrx;   /* unspecified: BT.601 below 720 lines, BT.709 from there */
    if (mc == DAV1D_MC_UNKNOWN) mc = h < 720 ? DAV1D_MC_BT601 : DAV1D_MC_BT709;
    *out = (mav1_picture){y, a->uv, ys, (size_t)cw * 2, w, h, mc == DAV1D_MC_BT709 ? 2 : mc == DAV1D_MC_BT2020_NCL || mc == DAV1D_MC_BT2020_CL ? 5 : 1, p->seq_hdr->color_range};
    return 1;
}
int mav1_decode(mav1 *a, const uint8_t *data, size_t n, mav1_picture *out) {
    unlock_picture(a);
    if (!a->dav1d) {
        Dav1dSettings st; dav1d_default_settings(&st);
        st.n_threads = 2; st.max_frame_delay = 1;   /* a temporal unit's picture before its send returns */
        st.logger.cookie = a; st.logger.callback = d_log;
        if (dav1d_open(&a->dav1d, &st)) { a->dav1d = NULL; if (!a->red++) LOG("[RED] AV1: dav1d could not be opened"); return -1; }
        static int once; if (!once++) LOG("AV1: dav1d %s decoding", dav1d_version());
    }
    Dav1dData d = {0}; uint8_t *buf = dav1d_data_create(&d, n);
    if (!buf) return -1;
    memcpy(buf, data, n);
    Dav1dPicture p = {0}; int got = 0;
    for (int tries = 0; d.sz && tries < 64; tries++) {
        int r = dav1d_send_data(a->dav1d, &d);
        if (r && r != DAV1D_ERR(EAGAIN)) { dav1d_data_unref(&d); if (!a->red++) LOG("[RED] AV1: dav1d refused a temporal unit (%d, first case)", r); return -1; }
        while (!dav1d_get_picture(a->dav1d, &p)) { if (a->has_pic) dav1d_picture_unref(&a->pic); a->pic = p; a->has_pic = 1; got = 1; }
    }
    if (d.sz) { dav1d_data_unref(&d); if (!a->red++) LOG("[RED] AV1: dav1d took no more data, a temporal unit dropped (first case)"); return -1; }
    if (!got) return 0;
    if (!dav1d_nv12(a, &a->pic, out)) { if (!a->red++) LOG("[RED] AV1: no memory for a %dx%d picture", a->pic.p.w, a->pic.p.h); return -1; }
    return 1;
}
