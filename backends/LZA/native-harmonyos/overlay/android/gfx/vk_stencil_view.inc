/* Stencil-mode texture views read through a float sampler (included by vk_shader.c).

   NVN reads the stencil plane of a depth-stencil texture through a view whose DepthStencilMode is STENCIL; the shader
   gets the stencil value's integer bits in a float register. Pokémon Legends: Z-A's deferred lighting (ead6e2a32540b19a)
   texelFetches such a view, takes floatBitsToUint(value) bit 6 and discards: the translator declares the slot a float
   sampler2D, and Vulkan leaves sampling a stencil (UINT) view through a float sampler undefined (on the Maleoon every
   lit pixel was discarded and the world was black). A shader whose slot holds a stencil view at the draw is loaded as a
   variant where that binding's image is UINT and every image read from it is bitcast back to float: the same bits the
   shader would read on the console. vk_prog_resolve picks it (stage_sig tells stencil views from depth views). */

enum { SPV_OpTypeImage = 25, SPV_OpTypeSampledImage = 27, SPV_OpImage = 100, SPV_OpImageFetch = 95, SPV_OpImageSampleImplicitLod = 87,
       SPV_OpImageSampleExplicitLod = 88, SPV_OpImageGather = 96, SPV_OpImageRead = 98, SPV_OpFunctionFirst = 54, SPV_DEC_BINDING = 33 };

/* 1: patched (*spv_p / *len_p replaced); bindings: bit b = descriptor binding b of set 0 */
static int stencil_patch(char **spv_p, size_t *len_p, uint64_t bindings, const char *variant) {
    uint32_t *w = (uint32_t *)*spv_p; size_t nw = *len_p / 4;
    if (nw < 5 || w[0] != 0x07230203u || !bindings) return 0;
    uint32_t bound = w[3]; if (!bound || bound > (1u << 22)) return 0;
    uint32_t *ptr_to = calloc(bound, 4), *si_img = calloc(bound, 4), *var_ty = calloc(bound, 4), *bind = calloc(bound, 4);
    uint8_t *is_f32 = calloc(bound, 1), *img_ok = calloc(bound, 1), *var_hit = calloc(bound, 1);
    uint32_t *patched = calloc(bound, 4);   /* id -> 1 patched sampled image value, 2 patched image value */
    size_t *img_at = calloc(bound, sizeof *img_at);
    int ok = ptr_to && si_img && var_ty && bind && is_f32 && img_ok && var_hit && patched && img_at;
    uint32_t t_uint = 0, t_v4u = 0;
    for (size_t i = 5; ok && i < nw; ) {
        uint32_t op = w[i] & 0xffff, l = w[i] >> 16; if (!l || i + l > nw) { ok = 0; break; }
        const uint32_t *a = w + i + 1;
        if (op == SPV_OpTypeFloat && a[0] < bound && a[1] == 32) is_f32[a[0]] = 1;
        else if (op == SPV_OpTypeInt && a[1] == 32 && a[2] == 0) t_uint = a[0];
        else if (op == SPV_OpTypeVector && t_uint && a[1] == t_uint && a[2] == 4) t_v4u = a[0];
        else if (op == SPV_OpTypeImage && a[0] < bound && a[1] < bound && is_f32[a[1]]) { img_ok[a[0]] = 1; img_at[a[0]] = i; }
        else if (op == SPV_OpTypeSampledImage && a[0] < bound) si_img[a[0]] = a[1];
        else if (op == SPV_OpTypePointer && a[0] < bound) ptr_to[a[0]] = a[2];
        else if (op == SPV_OpVariable && a[1] < bound) var_ty[a[1]] = a[0];
        else if (op == SPV_OpDecorate && l == 4 && a[1] == SPV_DEC_BINDING && a[0] < bound) bind[a[0]] = a[2] + 1;
        i += l;
    }
    /* the variables of those bindings, their sampled image and image types */
    uint32_t vars[8], nv = 0, imgs[8], ni = 0;
    for (uint32_t v = 0; ok && v < bound; v++) if (bind[v] && bind[v] - 1 < 64 && (bindings >> (bind[v] - 1) & 1) && var_ty[v]) {
        uint32_t si = ptr_to[var_ty[v]], im = si < bound ? si_img[si] : 0;
        if (!si || !im || !img_ok[im] || nv >= 8) continue;
        vars[nv++] = v; var_hit[v] = 1;
        int seen = 0; for (uint32_t k = 0; k < ni; k++) seen |= imgs[k] == im; if (!seen && ni < 8) imgs[ni++] = im;
    }
    if (!ok || !nv) goto out;
    /* new types per float image: a UINT image, its sampled image, a pointer to it */
    uint32_t new_img[8], new_si[8], new_ptr[8];
    uint32_t U = t_uint ? t_uint : bound++, V4 = t_v4u ? t_v4u : bound++;
    for (uint32_t k = 0; k < ni; k++) { new_img[k] = bound++; new_si[k] = bound++; new_ptr[k] = bound++; }
    size_t cap = nw + 64 + 32 * nv + 16 * ni; uint32_t *o = malloc(cap * 4); size_t n = 0;
    if (!o) goto out;
    for (size_t i = 0; i < 5; i++) o[n++] = w[i];
    o[3] = bound;   /* fixed again below */
    int added_types = 0;
    for (size_t i = 5; i < nw; ) {
        uint32_t op = w[i] & 0xffff, l = w[i] >> 16; const uint32_t *a = w + i + 1;
        if (n + l + 16 > cap) { cap = cap * 2 + l + 16; uint32_t *p = realloc(o, cap * 4); if (!p) { free(o); goto out; } o = p; }
        if (op == SPV_OpVariable && a[1] < bound && var_hit[a[1]]) {   /* the variable now points at the UINT sampled image */
            uint32_t si = ptr_to[a[0]], im = si_img[si], k = 0; while (k < ni && imgs[k] != im) k++;
            for (uint32_t j = 0; j < l; j++) o[n++] = w[i + j];
            o[n - l + 1] = new_ptr[k];
            i += l; continue;
        }
        if (op == SPV_OpLoad && l >= 4 && a[2] < bound && var_hit[a[2]]) {
            uint32_t si = ptr_to[var_ty[a[2]]], im = si_img[si], k = 0; while (k < ni && imgs[k] != im) k++;
            for (uint32_t j = 0; j < l; j++) o[n++] = w[i + j];
            o[n - l + 1] = new_si[k]; if (a[1] < bound) patched[a[1]] = 1 + (k << 4);
            i += l; continue;
        }
        if (op == SPV_OpImage && l == 4 && a[2] < bound && (patched[a[2]] & 15) == 1) {
            uint32_t k = patched[a[2]] >> 4;
            o[n++] = w[i]; o[n++] = new_img[k]; o[n++] = a[1]; o[n++] = a[2]; if (a[1] < bound) patched[a[1]] = 2 + (k << 4);
            i += l; continue;
        }
        if ((op == SPV_OpImageFetch || op == SPV_OpImageSampleImplicitLod || op == SPV_OpImageSampleExplicitLod || op == SPV_OpImageGather || op == SPV_OpImageRead)
            && l >= 5 && a[2] < bound && patched[a[2]]) {   /* read as uvec4, bitcast back to the float vec4 the shader uses */
            uint32_t rt = a[0], rid = a[1], tmp = bound++;
            for (uint32_t j = 0; j < l; j++) o[n++] = w[i + j];
            o[n - l + 1] = V4; o[n - l + 2] = tmp;
            o[n++] = 4u << 16 | SPV_OpBitcast; o[n++] = rt; o[n++] = rid; o[n++] = tmp;
            i += l; continue;
        }
        for (uint32_t j = 0; j < l; j++) o[n++] = w[i + j];
        /* the new types go right after the last float image they copy (so every operand is declared before them) */
        if (op == SPV_OpTypeImage && a[0] < bound && !added_types) {
            int last = 1; for (uint32_t k = 0; k < ni; k++) if (img_at[imgs[k]] > i) last = 0;
            int mine = 0; for (uint32_t k = 0; k < ni; k++) mine |= imgs[k] == a[0];
            if (mine && last) {
                if (!t_uint) { o[n++] = 4u << 16 | SPV_OpTypeInt; o[n++] = U; o[n++] = 32; o[n++] = 0; }
                if (!t_v4u) { o[n++] = 4u << 16 | SPV_OpTypeVector; o[n++] = V4; o[n++] = U; o[n++] = 4; }
                for (uint32_t k = 0; k < ni; k++) {
                    size_t at = img_at[imgs[k]]; uint32_t il = w[at] >> 16;
                    for (uint32_t j = 0; j < il; j++) o[n++] = w[at + j];
                    o[n - il + 1] = new_img[k]; o[n - il + 2] = U;
                    o[n++] = 3u << 16 | SPV_OpTypeSampledImage; o[n++] = new_si[k]; o[n++] = new_img[k];
                    o[n++] = 4u << 16 | SPV_OpTypePointer; o[n++] = new_ptr[k]; o[n++] = 0 /* UniformConstant */; o[n++] = new_si[k];
                }
                added_types = 1;
            }
        }
        i += l;
    }
    if (!added_types) { free(o); goto out; }
    o[3] = bound;
    free(*spv_p); *spv_p = (char *)o; *len_p = n * 4;
    LOGV("stencil view: %.16s reads %u binding(s) as UINT (mask %#llx)", variant, nv, (unsigned long long)bindings);
    free(ptr_to); free(si_img); free(var_ty); free(bind); free(is_f32); free(img_ok); free(var_hit); free(patched); free(img_at);
    return 1;
out:
    free(ptr_to); free(si_img); free(var_ty); free(bind); free(is_f32); free(img_ok); free(var_hit); free(patched); free(img_at);
    return 0;
}
