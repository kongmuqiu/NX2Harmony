/* AV1 decoding with dav1d (native-harmonyos/third_party/dav1d, built by build_dav1d_ohos.sh) for the Switch 2's nn::codec
   decoders (ns2_codec.c). The interface of seedds/nxnative's dm/host/hle_movie_av1.h (MIT), dav1d only. */
#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct mav1 mav1;
/* What one decode gave: an NV12 picture of w x h in the decoder's memory, valid until the next call, its matrix as
   vpx_color_space_t's values (BT.601 = 1, BT.709 = 2, BT.2020 = 5). */
typedef struct { const uint8_t *y, *uv; size_t ystride, uvstride; int w, h, matrix, full_range; } mav1_picture;

mav1 *mav1_new(void);
void mav1_free(mav1 *a);
void mav1_reset(mav1 *a);   /* a new stream starts at the next packet with a sequence header */
/* One temporal unit (OBUs with their sizes). 1 and *out set: a picture; 0: nothing to show; -1: the decoder refused it */
int mav1_decode(mav1 *a, const uint8_t *data, size_t n, mav1_picture *out);
const char *mav1_backend(const mav1 *a);
