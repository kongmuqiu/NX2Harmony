/* Transform feedback without VK_EXT_transform_feedback (included by vk_shader.c after vk_layer_gs.inc).

   Pokémon Legends: Z-A skins its characters on the GPU: a vertex-only program (28 such shaders, e.g. 681cc42e48c0bbc0)
   runs with rasterizer discard between Begin/EndTransformFeedback and writes positions (output location 0 xyz) and
   packed normal / tangent bits (locations 1 and 2, x) into three feedback buffers, which the character draws then read
   as vertex streams 0-2 (nxnative README, dm/gfx/tools/dmsh_build.py tf_layout / vs_tf). Without the capture those
   buffers kept garbage and the characters became screen-sized spikes.

   The layout is in the program's control section: four u16 strides at +0x6dc, per buffer a count at +0x6e8+b, the
   output attributes (0x20 + location) at +0xdc+0x80*b and their component counts at +0x4dc+0x80*b, captured from x on
   and interleaved in that order.

   The Maleoon 935 has no vertexPipelineStoresAndAtomics: storage writes from the vertex (or geometry) stage are not
   executed, so the capture cannot store from the vertex shader (a first version did, and every buffer stayed garbage).
   It is done in the fragment stage instead, which can store (fragmentStoresAndAtomics):
     - the vertex shader's capture variant (tf_vs_patch) writes, before every return, the bits of each captured output
       as a flat uvec4 output (from location *first_loc on, one per captured output in layout order), then the vertex's
       capture index (VertexIndex - BaseVertex) as a flat uint, and Position (0,0,0,1) with PointSize 1;
     - the capture draw (vk_draw.c) draws its points with rasterization on, a 1x1 viewport and scissor and no depth,
       stencil or colour writes, so each vertex is one fragment at pixel (0,0);
     - that fragment's shader (tf_fs_build, generated) stores the captured bits at index * stride/4 + component of the
       feedback buffers, bound as fragment storage slots 12+b (descriptor set 1, bindings 0.. in buffer order). Storage
       buffers are bound at the aligned address below theirs (TOTK_VK_SB_ALIGN, vk_draw.c ubo_desc), so it adds the
       rest, (guest address & mask) / 4 words, the address read from the fragment driver segment (c0 +0x510 + 16 *
       slot, the next binding, kind RB_DRV0) as the translated shaders do.
   The pipelines of a capture variant keep their rasterization; vk_draw.c turns off what would write attachments. */

typedef struct { uint16_t stride[4]; uint8_t n[4]; uint8_t loc[4][8], comps[4][8]; } za_tf_layout;

static int tf_layout_parse(const uint8_t *ctl, za_tf_layout *L) {
    memset(L, 0, sizeof *L);
    int any = 0;
    for (int b = 0; b < 4; b++) {
        uint16_t s; memcpy(&s, ctl + 0x6dc + 2 * b, 2);
        if (!s) continue;
        uint8_t n = ctl[0x6e8 + b];
        if (n < 1 || n > 8) return 0;
        unsigned sum = 0;
        for (int i = 0; i < n; i++) {
            uint8_t a = ctl[0xdc + 0x80 * b + i], c = ctl[0x4dc + 0x80 * b + i];
            if (a < 0x20 || a >= 0x40 || c < 1 || c > 4) return 0;
            L->loc[b][i] = (uint8_t)(a - 0x20); L->comps[b][i] = c; sum += c;
        }
        if (s != 4 * sum) return 0;
        L->stride[b] = s; L->n[b] = n; any = 1;
    }
    return any;
}

enum { SPV_OpShiftRightLogical = 194, SPV_OpBitwiseAnd = 199, SPV_DEC_BLOCK = 2, SPV_OpISub = 130, SPV_OpIMul = 132, SPV_OpIAdd = 128, SPV_OpCompositeExtract = 81, SPV_OpCompositeConstruct = 80,
       SPV_OpConstantComposite = 44, SPV_OpTypeRuntimeArray = 29, SPV_OpReturnOp = 253, SPV_OpFunctionEndOp = 56,
       SPV_OpEntryPointOp = 15, SPV_DEC_FLAT = 14, SPV_DEC_OFFSET = 35, SPV_DEC_ARRAY_STRIDE = 6, SPV_DEC_BLOCK_BUFFER = 3,
       SPV_DEC_SET = 34, SPV_BI_POINTSIZE = 1, SPV_BI_VERTEX_INDEX = 42, SPV_BI_BASE_VERTEX = 4424, SPV_CAP_DRAW_PARAMETERS = 4427,
       SPV_SC_INPUT = 1, SPV_SC_UNIFORM = 2, SPV_SC_OUTPUT = 3 };

static int tf_entries(const za_tf_layout *L) { int n = 0; for (int b = 0; b < 4; b++) if (L->stride[b]) n += L->n[b]; return n; }

/* 1: patched (see the top of this file); *first_loc gets the first location of the flat outputs */
static int tf_vs_patch(char **spv_p, size_t *len_p, const za_tf_layout *L, uint32_t *first_loc, const char *variant) {
    uint32_t *w = (uint32_t *)*spv_p; size_t nw = *len_p / 4;
    if (nw < 5 || w[0] != 0x07230203u) return 0;
    uint32_t version = w[1], bound = w[3];
    if (!bound || bound > (1u << 22)) return 0;
    int nent = tf_entries(L);
    if (nent < 1 || nent > 32) return 0;
    uint32_t T_f = 0, T_i = 0, T_u = 0, T_v4 = 0, T_uv4 = 0, VI = 0, BV = 0, VI_ty = 0, BV_ty = 0, entry = 0;
    uint32_t PV_struct = 0, PV_var = 0, POS_var = 0; int m_pos = -1, m_psz = -1, max_loc = -1;
    int has_dp_cap = 0, has_dp_ext = 0, ok = 0;
    uint32_t *out_loc = calloc(64, 4), *out_ty = calloc(64, 4);
    uint32_t *ptr_to = calloc(bound, 4), *ptr_sc = calloc(bound, 4), *loc_of = calloc(bound, 4), *cval = calloc(bound, 4), *arr_len = calloc(bound, 4);
    uint8_t *vec4f = calloc(bound, 1), *bi_pos = calloc(bound, 1);
    uint32_t *o = NULL, *src = NULL;
    if (!out_loc || !out_ty || !ptr_to || !ptr_sc || !loc_of || !cval || !arr_len || !vec4f || !bi_pos) goto done;
    size_t at_entry = 0, at_last_annot = 0, at_first_fn = 0;
    for (size_t i = 5; i < nw; ) {
        uint32_t op = w[i] & 0xffff, l = w[i] >> 16; if (!l || i + l > nw) goto done;
        const uint32_t *a = w + i + 1;
        switch (op) {
        case SPV_OpCapability: if (a[0] == SPV_CAP_DRAW_PARAMETERS) has_dp_cap = 1; break;
        case SPV_OpExtension: if (lgs_name_is(a, l - 1, "SPV_KHR_shader_draw_parameters")) has_dp_ext = 1; break;
        case SPV_OpEntryPointOp: if (a[0] == 0) { at_entry = i; entry = a[1]; } break;
        case SPV_OpDecorate: case SPV_OpMemberDecorate: at_last_annot = i;
            if (op == SPV_OpDecorate && l == 4 && a[0] < bound) {
                if (a[1] == SPV_LOCATION) loc_of[a[0]] = a[2] + 1;
                if (a[1] == SPV_BUILTIN && a[2] == SPV_BI_VERTEX_INDEX) VI = a[0];
                if (a[1] == SPV_BUILTIN && a[2] == SPV_BI_BASE_VERTEX) BV = a[0];
                if (a[1] == SPV_BUILTIN && a[2] == SPV_BI_POSITION) bi_pos[a[0]] = 1;
            }
            if (op == SPV_OpMemberDecorate && l == 5 && a[2] == SPV_BUILTIN) {
                if (a[3] == SPV_BI_POSITION) { PV_struct = a[0]; m_pos = (int)a[1]; }
                if (a[3] == SPV_BI_POINTSIZE) { PV_struct = a[0]; m_psz = (int)a[1]; }
            }
            break;
        case SPV_OpTypeFloat: if (a[1] == 32 && !T_f) T_f = a[0]; break;
        case SPV_OpTypeInt: if (a[1] == 32) { if (a[2]) { if (!T_i) T_i = a[0]; } else if (!T_u) T_u = a[0]; } break;
        case SPV_OpTypeVector:
            if (T_f && a[1] == T_f && a[2] == 4 && a[0] < bound) { vec4f[a[0]] = 1; if (!T_v4) T_v4 = a[0]; }
            if (T_u && a[1] == T_u && a[2] == 4 && !T_uv4) T_uv4 = a[0];
            break;
        case SPV_OpConstant: if (l >= 4 && a[1] < bound) cval[a[1]] = a[2]; break;
        case SPV_OpTypeArray: if (a[0] < bound && a[2] < bound) arr_len[a[0]] = cval[a[2]]; break;
        case SPV_OpTypePointer: if (a[0] < bound) { ptr_sc[a[0]] = a[1]; ptr_to[a[0]] = a[2]; } break;
        case SPV_OpVariable:
            if (a[1] >= bound || a[0] >= bound) break;
            if (a[2] == SPV_SC_OUTPUT) {
                uint32_t pointee = ptr_to[a[0]];
                if (loc_of[a[1]]) {
                    int loc = (int)loc_of[a[1]] - 1, span = pointee < bound && arr_len[pointee] ? (int)arr_len[pointee] : 1;
                    if (loc < 64) { out_loc[loc] = a[1]; out_ty[loc] = pointee; }
                    if (loc + span - 1 > max_loc) max_loc = loc + span - 1;
                }
                if (PV_struct && pointee == PV_struct) PV_var = a[1];
                if (bi_pos[a[1]]) POS_var = a[1];
            }
            if (a[2] == SPV_SC_INPUT) { if (a[1] == VI) VI_ty = ptr_to[a[0]]; if (a[1] == BV) BV_ty = ptr_to[a[0]]; }
            break;
        case SPV_OpFunction: if (!at_first_fn) at_first_fn = i; break;
        }
        i += l;
    }
    if (!at_entry || !at_first_fn || !at_last_annot || !T_f || !T_v4) goto done;
    if (!PV_var && !POS_var) goto done;
    if ((VI && VI_ty != T_i && VI_ty != T_u) || (BV && BV_ty != T_i && BV_ty != T_u) || (VI && !VI_ty) || (BV && !BV_ty)) goto done;
    uint32_t first = (uint32_t)(max_loc + 1);
    if (first + (uint32_t)nent + 1 > 31) goto done;   /* maxVertexOutputComponents 128 on the devices in question */
    /* new ids */
    uint32_t nT_u = T_u ? 0 : bound++, nT_i = T_i ? 0 : bound++;
    if (!T_u) T_u = nT_u; if (!T_i) T_i = nT_i;
    uint32_t nT_uv4 = T_uv4 ? 0 : bound++; if (!T_uv4) T_uv4 = nT_uv4;
    uint32_t P_in_i = bound++, nVI = VI ? 0 : bound++, nBV = BV ? 0 : bound++;
    if (!VI) { VI = nVI; VI_ty = T_i; } if (!BV) { BV = nBV; BV_ty = T_i; }
    uint32_t P_out_uv4 = bound++, P_out_u = bound++, P_out_v4 = bound++, P_out_f = bound++;
    uint32_t OV[32], OIDX = bound++; for (int j = 0; j < nent; j++) OV[j] = bound++;
    uint32_t C_u0 = bound++, C_i_pos = bound++, C_i_psz = bound++, C_f0 = bound++, C_f1 = bound++, C_pos = bound++;
    size_t cap = nw * 2 + 4096, n = 0; o = malloc(cap * 4);
    if (!o) goto done;
#define PUT(v) do { if (n >= cap) { cap *= 2; uint32_t *p_ = realloc(o, cap * 4); if (!p_) goto done; o = p_; } o[n++] = (v); } while (0)
#define OPW(op, ...) do { const uint32_t a_[] = {__VA_ARGS__}; PUT((uint32_t)((sizeof a_ / 4 + 1) << 16 | (op))); for (size_t k_ = 0; k_ < sizeof a_ / 4; k_++) PUT(a_[k_]); } while (0)
    for (int k = 0; k < 5; k++) PUT(w[k]);
    int added_caps = 0;
    for (size_t i = 5; i < nw; ) {
        uint32_t op = w[i] & 0xffff, l = w[i] >> 16;
        if (!added_caps && op != SPV_OpCapability) {   /* after the capabilities: DrawParameters and its extension for BaseVertex */
            if (!has_dp_cap) OPW(SPV_OpCapability, SPV_CAP_DRAW_PARAMETERS);
            added_caps = 1;
            if (!has_dp_ext && version < 0x10300) {
                const char *s = "SPV_KHR_shader_draw_parameters"; size_t sl = strlen(s) + 1, wl = (sl + 3) / 4;
                PUT((uint32_t)((wl + 1) << 16 | SPV_OpExtension)); uint32_t buf[16] = {0}; memcpy(buf, s, sl); for (size_t k = 0; k < wl; k++) PUT(buf[k]);
            }
        }
        if (i == at_entry) {   /* the entry point lists the new inputs and outputs */
            size_t hdr = n; for (uint32_t k = 0; k < l; k++) PUT(w[i + k]);
            uint32_t extra = 0;
            if (nVI) { PUT(VI); extra++; } if (nBV) { PUT(BV); extra++; }
            for (int j = 0; j < nent; j++) { PUT(OV[j]); extra++; }
            PUT(OIDX); extra++;
            o[hdr] = (uint32_t)((l + extra) << 16 | SPV_OpEntryPointOp);
            i += l; continue;
        }
        for (uint32_t k = 0; k < l; k++) PUT(w[i + k]);
        if (i == at_last_annot) {   /* decorations of the new objects */
            if (nVI) OPW(SPV_OpDecorate, VI, SPV_BUILTIN, SPV_BI_VERTEX_INDEX);
            if (nBV) OPW(SPV_OpDecorate, BV, SPV_BUILTIN, SPV_BI_BASE_VERTEX);
            for (int j = 0; j < nent; j++) { OPW(SPV_OpDecorate, OV[j], SPV_LOCATION, first + (uint32_t)j); OPW(SPV_OpDecorate, OV[j], SPV_DEC_FLAT); }
            OPW(SPV_OpDecorate, OIDX, SPV_LOCATION, first + (uint32_t)nent); OPW(SPV_OpDecorate, OIDX, SPV_DEC_FLAT);
        }
        i += l;
        if (i < nw && i == at_first_fn) {   /* types, constants and variables go before the first function */
            if (nT_u) OPW(SPV_OpTypeInt, nT_u, 32, 0);
            if (nT_i) OPW(SPV_OpTypeInt, nT_i, 32, 1);
            if (nT_uv4) OPW(SPV_OpTypeVector, nT_uv4, T_u, 4);
            OPW(SPV_OpTypePointer, P_in_i, SPV_SC_INPUT, T_i);
            if (nVI) OPW(SPV_OpVariable, P_in_i, nVI, SPV_SC_INPUT);
            if (nBV) OPW(SPV_OpVariable, P_in_i, nBV, SPV_SC_INPUT);
            OPW(SPV_OpTypePointer, P_out_uv4, SPV_SC_OUTPUT, T_uv4);
            OPW(SPV_OpTypePointer, P_out_u, SPV_SC_OUTPUT, T_u);
            OPW(SPV_OpTypePointer, P_out_v4, SPV_SC_OUTPUT, T_v4);
            OPW(SPV_OpTypePointer, P_out_f, SPV_SC_OUTPUT, T_f);
            OPW(SPV_OpConstant, T_u, C_u0, 0);
            OPW(SPV_OpConstant, T_i, C_i_pos, (uint32_t)(m_pos < 0 ? 0 : m_pos));
            OPW(SPV_OpConstant, T_i, C_i_psz, (uint32_t)(m_psz < 0 ? 0 : m_psz));
            OPW(SPV_OpConstant, T_f, C_f0, 0);
            OPW(SPV_OpConstant, T_f, C_f1, 0x3f800000u);
            OPW(SPV_OpConstantComposite, T_v4, C_pos, C_f0, C_f0, C_f0, C_f1);
            for (int j = 0; j < nent; j++) OPW(SPV_OpVariable, P_out_uv4, OV[j], SPV_SC_OUTPUT);
            OPW(SPV_OpVariable, P_out_u, OIDX, SPV_SC_OUTPUT);
        }
    }
    /* second pass: the flat outputs, Position and PointSize before every OpReturn of the entry function */
    src = o; size_t sn = n; cap = sn * 3 + 8192; n = 0; o = malloc(cap * 4);
    if (!o) goto done;
    {
        int inside = 0;
        for (size_t i = 0; i < 5; i++) PUT(src[i]);
        for (size_t i = 5; i < sn; ) {
            uint32_t op = src[i] & 0xffff, l = src[i] >> 16;
            if (op == SPV_OpFunction && src[i + 2] == entry) inside = 1;
            if (op == SPV_OpFunctionEndOp) inside = 0;
            if (inside && op == SPV_OpReturnOp) {
                uint32_t vi = bound++, bv = bound++, li = bound++;
                OPW(SPV_OpLoad, VI_ty, vi, VI); OPW(SPV_OpLoad, BV_ty, bv, BV);
                if (VI_ty != T_u) { uint32_t t = bound++; OPW(SPV_OpBitcast, T_u, t, vi); vi = t; }
                if (BV_ty != T_u) { uint32_t t = bound++; OPW(SPV_OpBitcast, T_u, t, bv); bv = t; }
                OPW(SPV_OpISub, T_u, li, vi, bv);
                OPW(SPV_OpStore, OIDX, li);
                int j = 0;
                for (int b = 0; b < 4; b++) if (L->stride[b]) for (int q = 0; q < L->n[b]; q++, j++) {
                    unsigned loc = L->loc[b][q], nc = L->comps[b][q];
                    uint32_t var = loc < 64 ? out_loc[loc] : 0, val = 0, c4[4];
                    if (var && out_ty[loc] < bound && vec4f[out_ty[loc]]) { val = bound++; OPW(SPV_OpLoad, T_v4, val, var); }
                    for (unsigned c = 0; c < 4; c++) {
                        if (val && c < nc) { uint32_t x = bound++; c4[c] = bound++; OPW(SPV_OpCompositeExtract, T_f, x, val, c); OPW(SPV_OpBitcast, T_u, c4[c], x); }
                        else c4[c] = C_u0;
                    }
                    uint32_t u4 = bound++;
                    OPW(SPV_OpCompositeConstruct, T_uv4, u4, c4[0], c4[1], c4[2], c4[3]);
                    OPW(SPV_OpStore, OV[j], u4);
                }
                if (PV_var) {
                    if (m_pos >= 0) { uint32_t p = bound++; OPW(SPV_OpAccessChain, P_out_v4, p, PV_var, C_i_pos); OPW(SPV_OpStore, p, C_pos); }
                    if (m_psz >= 0) { uint32_t p = bound++; OPW(SPV_OpAccessChain, P_out_f, p, PV_var, C_i_psz); OPW(SPV_OpStore, p, C_f1); }
                } else OPW(SPV_OpStore, POS_var, C_pos);
            }
            for (uint32_t k = 0; k < l; k++) PUT(src[i + k]);
            i += l;
        }
    }
#undef OPW
#undef PUT
    o[3] = bound;
    free(*spv_p); *spv_p = (char *)o; *len_p = n * 4; o = NULL;
    *first_loc = first; ok = 1;
    LOGV("transform feedback: %.16s captures %d output(s) into %d buffer(s) (strides %u %u %u %u), flat outputs from location %u",
         variant, nent, (L->stride[0] != 0) + (L->stride[1] != 0) + (L->stride[2] != 0) + (L->stride[3] != 0),
         L->stride[0], L->stride[1], L->stride[2], L->stride[3], first);
done:
    free(o); free(src);
    free(out_loc); free(out_ty); free(ptr_to); free(ptr_sc); free(loc_of); free(cval); free(arr_len); free(vec4f); free(bi_pos);
    return ok;
}

/* The capture's fragment shader (SPIR-V 1.0): the flat inputs tf_vs_patch writes, stored as bits into the feedback
   buffers (Uniform + BufferBlock, descriptor set 1, binding k for the k-th buffer in use) at index * stride/4 +
   component + (guest address & sb_mask) / 4, the address from the driver segment (binding: the number of buffers).
   NULL on failure; *nwords gets its length. */
static uint32_t *tf_fs_build(const za_tf_layout *L, uint32_t first_loc, uint32_t sb_mask, size_t *nwords) {
    int nent = tf_entries(L);
    if (nent < 1 || nent > 32) return NULL;
    uint32_t bound = 1;
    uint32_t T_void = bound++, T_fn = bound++, T_u = bound++, T_uv4 = bound++, P_in_uv4 = bound++, P_in_u = bound++;
    uint32_t RTA = bound++, BLK = bound++, P_blk = bound++, P_u = bound++, MAIN = bound++, LBL = bound++;
    uint32_t IN[32], INIDX = bound++, SB[4] = {0}, C[34], nsb = 0;
    for (int j = 0; j < nent; j++) IN[j] = bound++;
    for (int b = 0; b < 4; b++) if (L->stride[b]) SB[b] = bound++, nsb++;
    for (int k = 0; k < 34; k++) C[k] = bound++;   /* uint constants 0..33 (strides / 4 up to 32, offsets) */
    /* the driver segment: uvec4[160] (0xa00 bytes), the slot's address at uvec4 0x51 + slot, x the low half */
    uint32_t DARR = bound++, DBLK = bound++, P_dblk = bound++, DRV = bound++, C_160 = bound++, C_mask = bound++, C_dslot[4];
    for (int b = 0; b < 4; b++) C_dslot[b] = bound++;
    size_t cap = 4096, n = 0; uint32_t *o = malloc(cap * 4);
    if (!o) return NULL;
#define PUT(v) do { if (n >= cap) { cap *= 2; uint32_t *p_ = realloc(o, cap * 4); if (!p_) { free(o); return NULL; } o = p_; } o[n++] = (v); } while (0)
#define OPW(op, ...) do { const uint32_t a_[] = {__VA_ARGS__}; PUT((uint32_t)((sizeof a_ / 4 + 1) << 16 | (op))); for (size_t k_ = 0; k_ < sizeof a_ / 4; k_++) PUT(a_[k_]); } while (0)
    PUT(0x07230203u); PUT(0x00010000u); PUT(0); PUT(0 /* bound, below */); PUT(0);
    OPW(SPV_OpCapability, 1 /* Shader */);
    OPW(SPV_OpMemoryModel, 0 /* Logical */, 1 /* GLSL450 */);
    {   /* OpEntryPoint Fragment %main "main" <inputs> */
        PUT((uint32_t)((3 + 2 + (uint32_t)nent + 1) << 16 | SPV_OpEntryPointOp));
        PUT(4 /* Fragment */); PUT(MAIN); PUT(0x6e69616du /* "main" */); PUT(0);
        for (int j = 0; j < nent; j++) PUT(IN[j]);
        PUT(INIDX);
    }
    OPW(SPV_OpExecutionMode, MAIN, 7 /* OriginUpperLeft */);
    for (int j = 0; j < nent; j++) { OPW(SPV_OpDecorate, IN[j], SPV_LOCATION, first_loc + (uint32_t)j); OPW(SPV_OpDecorate, IN[j], SPV_DEC_FLAT); }
    OPW(SPV_OpDecorate, INIDX, SPV_LOCATION, first_loc + (uint32_t)nent); OPW(SPV_OpDecorate, INIDX, SPV_DEC_FLAT);
    OPW(SPV_OpDecorate, RTA, SPV_DEC_ARRAY_STRIDE, 4);
    OPW(SPV_OpDecorate, BLK, SPV_DEC_BLOCK_BUFFER);
    OPW(SPV_OpMemberDecorate, BLK, 0, SPV_DEC_OFFSET, 0);
    { uint32_t k = 0; for (int b = 0; b < 4; b++) if (SB[b]) { OPW(SPV_OpDecorate, SB[b], SPV_DEC_SET, 1); OPW(SPV_OpDecorate, SB[b], SPV_DEC_BINDING, k); k++; } }
    OPW(SPV_OpDecorate, DARR, SPV_DEC_ARRAY_STRIDE, 16);
    OPW(SPV_OpDecorate, DBLK, SPV_DEC_BLOCK);
    OPW(SPV_OpMemberDecorate, DBLK, 0, SPV_DEC_OFFSET, 0);
    OPW(SPV_OpDecorate, DRV, SPV_DEC_SET, 1); OPW(SPV_OpDecorate, DRV, SPV_DEC_BINDING, nsb);
    OPW(SPV_OpTypeVoid, T_void);
    OPW(SPV_OpTypeFunction, T_fn, T_void);
    OPW(SPV_OpTypeInt, T_u, 32, 0);
    OPW(SPV_OpTypeVector, T_uv4, T_u, 4);
    OPW(SPV_OpTypePointer, P_in_uv4, SPV_SC_INPUT, T_uv4);
    OPW(SPV_OpTypePointer, P_in_u, SPV_SC_INPUT, T_u);
    OPW(SPV_OpTypeRuntimeArray, RTA, T_u);
    OPW(SPV_OpTypeStruct, BLK, RTA);
    OPW(SPV_OpTypePointer, P_blk, SPV_SC_UNIFORM, BLK);
    OPW(SPV_OpTypePointer, P_u, SPV_SC_UNIFORM, T_u);
    for (int k = 0; k < 34; k++) OPW(SPV_OpConstant, T_u, C[k], (uint32_t)k);
    OPW(SPV_OpConstant, T_u, C_160, 160); OPW(SPV_OpConstant, T_u, C_mask, sb_mask);
    for (int b = 0; b < 4; b++) OPW(SPV_OpConstant, T_u, C_dslot[b], 0x51u + 12u + (uint32_t)b);
    OPW(SPV_OpTypeArray, DARR, T_uv4, C_160);
    OPW(SPV_OpTypeStruct, DBLK, DARR);
    OPW(SPV_OpTypePointer, P_dblk, SPV_SC_UNIFORM, DBLK);
    for (int j = 0; j < nent; j++) OPW(SPV_OpVariable, P_in_uv4, IN[j], SPV_SC_INPUT);
    OPW(SPV_OpVariable, P_in_u, INIDX, SPV_SC_INPUT);
    for (int b = 0; b < 4; b++) if (SB[b]) OPW(SPV_OpVariable, P_blk, SB[b], SPV_SC_UNIFORM);
    OPW(SPV_OpVariable, P_dblk, DRV, SPV_SC_UNIFORM);
    OPW(SPV_OpFunction, T_void, MAIN, 0, T_fn);
    OPW(SPV_OpLabel, LBL);
    uint32_t idx = bound++; OPW(SPV_OpLoad, T_u, idx, INIDX);
    int j = 0;
    for (int b = 0; b < 4; b++) if (SB[b]) {
        uint32_t words = L->stride[b] / 4u, scaled = bound++, ap = bound++, al = bound++, mis = bound++, misw = bound++, base = bound++;
        if (words > 32) { free(o); return NULL; }
        OPW(SPV_OpIMul, T_u, scaled, idx, C[words]);
        OPW(SPV_OpAccessChain, P_u, ap, DRV, C[0], C_dslot[b], C[0]);
        OPW(SPV_OpLoad, T_u, al, ap);
        OPW(SPV_OpBitwiseAnd, T_u, mis, al, C_mask);
        OPW(SPV_OpShiftRightLogical, T_u, misw, mis, C[2]);
        OPW(SPV_OpIAdd, T_u, base, scaled, misw);
        unsigned off = 0;
        for (int q = 0; q < L->n[b]; q++, j++) {
            uint32_t v = bound++; OPW(SPV_OpLoad, T_uv4, v, IN[j]);
            for (unsigned c = 0; c < L->comps[b][q]; c++, off++) {
                uint32_t x = bound++, at = bound++, ptr = bound++;
                OPW(SPV_OpCompositeExtract, T_u, x, v, c);
                OPW(SPV_OpIAdd, T_u, at, base, C[off < 33 ? off : 33]);
                OPW(SPV_OpAccessChain, P_u, ptr, SB[b], C[0], at);
                OPW(SPV_OpStore, ptr, x);
            }
        }
    }
    PUT(1u << 16 | SPV_OpReturn);
    PUT(1u << 16 | SPV_OpFunctionEnd);
#undef OPW
#undef PUT
    o[3] = bound; *nwords = n;
    return o;
}
