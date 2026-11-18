/* BC -> ETC2 transcoding for GPUs without BC (Maleoon): the decoded RGBA8 pixels of a BC texture are encoded to ETC2 on
   the CPU at upload (android/gfx/vk_res.c upload_inner), so the image stays compressed on the GPU: 4-8 bpp instead of
   32 (memory: the system kills the game near 3.1 GB of GPU memory; bandwidth: compressed sampling). Encoder: etcpak
   (native-harmonyos/third_party/etcpak, BSD-3, Bartosz Taudul; NEON paths on aarch64).
   kind 0: ETC2 RGB8 (one 64-bit word a block; quality 0 writes ETC1 blocks, valid ETC2, about 3x faster)
   kind 1: ETC2 RGBA8 (two words: EAC alpha, then colour)
   w and h are multiples of 4 (the caller pads the edge); pixels are RGBA8 bytes, row pitch w. Returns the bytes written. */
#include <stdint.h>
#include <stddef.h>
#include "../third_party/etcpak/ProcessRGB.hpp"

extern "C" size_t totk_etc_encode(int kind, const uint32_t *rgba, uint32_t w, uint32_t h, uint64_t *dst, int quality) {
    if (!rgba || !dst || (w & 3) || (h & 3) || !w || !h) return 0;
    uint32_t blocks = (w / 4) * (h / 4);
    if (kind == 0) {
        if (quality > 0) CompressEtc2Rgb(rgba, dst, blocks, w, true);
        else CompressEtc1Rgb(rgba, dst, blocks, w);
        return (size_t)blocks * 8;
    }
    CompressEtc2Rgba(rgba, dst, blocks, w, quality > 0);
    return (size_t)blocks * 16;
}
