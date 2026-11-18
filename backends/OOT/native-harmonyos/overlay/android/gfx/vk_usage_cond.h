/* The texture pool's usage bitmap is written only where a bit is not set yet (HarmonyOS port, Ocarina of Time).
   The translator (prepare_xlate_ns2.py MarkBindlessUsed) emits, for every sample of a pool texture,
       OpAtomicOr %uint %word_pointer %scope %semantics %bit
   The bitmap only gains bits and is never cleared (vk_res.c), so after a texture's first frames every such instruction
   is an atomic read-modify-write that changes nothing: several a fragment, on words every fragment of a draw shares.
   Here each one becomes
       %old = OpLoad %uint %word_pointer
       %and = OpBitwiseAnd %uint %old %bit
       %new = OpIEqual %bool %and %zero
              OpSelectionMerge %after None
              OpBranchConditional %new %write %after
     %write = OpLabel
              (the OpAtomicOr)
              OpBranch %after
     %after = OpLabel
   so a set bit costs a load. The same bits end up set (a racing fragment that reads the old word still does the OR).
   Left as they are: an OpAtomicOr whose result is used, whose block ends in OpLoopMerge (the block has to stay the loop
   header), or anything not of this exact shape. Returns the number rewritten (0: the module is unchanged), -1 on a
   malformed module or no memory. The caller owns *words before and after (malloc). */
#ifndef VK_USAGE_COND_H
#define VK_USAGE_COND_H
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

static int vk_spirv_usage_cond(uint32_t **words, size_t *nw) {
    const uint32_t *w = *words; size_t n = *nw;
    if (n < 5 || w[0] != 0x07230203u) return -1;
    uint32_t bound = w[3], t_bool = 0, t_uint = 0, c_zero = 0;
    size_t first_fn = 0; int cand = 0;
    for (size_t i = 5; i < n; ) {
        uint32_t op = w[i] & 0xffffu, len = w[i] >> 16;
        if (!len || len > n - i) return -1;
        if (op == 20 && len == 2) t_bool = w[i + 1];
        else if (op == 21 && len == 4 && w[i + 2] == 32 && w[i + 3] == 0) t_uint = w[i + 1];
        else if (op == 54 && !first_fn) first_fn = i;
        else if (op == 241 && len == 7) cand++;
        i += len;
    }
    if (!cand || !first_fn || !t_uint) return 0;
    for (size_t i = 5; i < first_fn; ) {   /* a zero of that type: OpConstant 0 or OpConstantNull */
        uint32_t op = w[i] & 0xffffu, len = w[i] >> 16;
        if (!c_zero && ((op == 43 && len == 4 && w[i + 1] == t_uint && w[i + 3] == 0) || (op == 46 && len == 3 && w[i + 1] == t_uint))) c_zero = w[i + 2];
        i += len;
    }
    /* how often each id appears as a word after the header: a result that appears once is not used (a literal equal to
       the id only makes this say "used", which leaves that instruction alone) */
    uint32_t *seen = calloc(bound ? bound : 1, sizeof *seen); if (!seen) return -1;
    for (size_t i = 5; i < n; ) {
        uint32_t len = w[i] >> 16;
        for (uint32_t k = 1; k < len; k++) if (w[i + k] < bound) seen[w[i + k]]++;
        i += len;
    }
    size_t cap = n + (size_t)cand * 32 + 8;   /* 27 words more an instruction rewritten, 6 for the two declarations */ uint32_t *o = malloc(cap * sizeof *o), *tail = calloc(bound ? bound : 1, sizeof *tail);
    if (!o || !tail) { free(seen); free(o); free(tail); return -1; }
    size_t at = 0; memcpy(o, w, 5 * sizeof *o); at = 5;
    uint32_t next = bound, block = 0, block0 = 0; int done = 0;
    for (size_t i = 5; i < n; ) {
        uint32_t op = w[i] & 0xffffu, len = w[i] >> 16;
        if (i == first_fn) {   /* the declarations the rewrite needs, in front of the first function */
            if (!t_bool) { t_bool = next++; o[at++] = 2u << 16 | 20; o[at++] = t_bool; }
            if (!c_zero) { c_zero = next++; o[at++] = 4u << 16 | 43; o[at++] = t_uint; o[at++] = c_zero; o[at++] = 0; }
        }
        if (op == 248) { block = block0 = w[i + 1]; }
        if (op == 241 && len == 7 && block && w[i + 1] == t_uint && seen[w[i + 2]] == 1) {
            /* the block's end: no OpLoopMerge before its terminator */
            int loop = 0;
            for (size_t j = i + len; j < n; ) {
                uint32_t jop = w[j] & 0xffffu, jl = w[j] >> 16;
                if (jop == 246) loop = 1;
                if (jop == 248 || jop == 56 || (jop >= 249 && jop <= 255)) break;   /* the next label, OpFunctionEnd, or a terminator */
                j += jl;
            }
            if (!loop) {
                uint32_t ptr = w[i + 3], val = w[i + 6], old = next++, band = next++, isnew = next++, wr = next++, after = next++;
                o[at++] = 4u << 16 | 61;  o[at++] = t_uint; o[at++] = old; o[at++] = ptr;
                o[at++] = 5u << 16 | 199; o[at++] = t_uint; o[at++] = band; o[at++] = old; o[at++] = val;
                o[at++] = 5u << 16 | 170; o[at++] = t_bool; o[at++] = isnew; o[at++] = band; o[at++] = c_zero;
                o[at++] = 3u << 16 | 247; o[at++] = after; o[at++] = 0;
                o[at++] = 4u << 16 | 250; o[at++] = isnew; o[at++] = wr; o[at++] = after;
                o[at++] = 2u << 16 | 248; o[at++] = wr;
                memcpy(o + at, w + i, len * sizeof *o); at += len;
                o[at++] = 2u << 16 | 249; o[at++] = after;
                o[at++] = 2u << 16 | 248; o[at++] = after;
                block = after; tail[block0] = after; done++;
                i += len; continue;
            }
        }
        memcpy(o + at, w + i, len * sizeof *o); at += len; i += len;
    }
    if (done) {   /* a block that was split is left through its last part: the OpPhi parents that named it */
        for (size_t i = 5; i < at; ) {
            uint32_t op = o[i] & 0xffffu, len = o[i] >> 16;
            if (op == 245) for (uint32_t k = 4; k < len; k += 2) if (o[i + k] < bound && tail[o[i + k]]) o[i + k] = tail[o[i + k]];
            i += len;
        }
        o[3] = next;
        free(*words); *words = o; *nw = at;
    } else free(o);
    free(seen); free(tail);
    return done;
}
#endif
