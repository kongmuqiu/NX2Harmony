/* Layered rendering without a layer output from vertex shaders (included by vk_shader.c).

   NVN lets a vertex shader write render_target_array_index; Vulkan needs shaderOutputLayer (1.2) or
   VK_EXT_shader_viewport_index_layer for that, and the Maleoon 935 has neither (only geometry shaders). Pokémon Legends:
   Z-A builds its 32x32x32 colour grading LUT that way (b29f08619f379a83: one triangle per instance, gl_Layer = instance +
   c0[100].y + 1) and tone-maps every frame through it, so without layers the whole picture was black.

   On such a device a vertex shader that writes Layer is patched at load: the Layer output becomes an ordinary output at
   the first free location, and a pass-through geometry shader is made from the vertex shader's outputs: every location
   output and the per-vertex block are copied for the three vertices of each triangle, and gl_Layer is written from that
   location. A fragment shader that reads Layer gets the Geometry capability in place of ShaderViewportIndexLayerEXT.
   The pass renders into all the layers its attachments share (vk_draw.c begin_rendering, R.layers). */

enum { SPV_OpExtension = 10, SPV_OpCapability = 17, SPV_OpTypeVoid = 19, SPV_OpTypeBool = 20, SPV_OpTypeInt = 21, SPV_OpTypeFloat = 22,
       SPV_OpTypeVector = 23, SPV_OpTypeArray = 28, SPV_OpTypeStruct = 30, SPV_OpTypePointer = 32, SPV_OpTypeFunction = 33, SPV_OpConstant = 43,
       SPV_OpFunction = 54, SPV_OpFunctionEnd = 56, SPV_OpVariable = 59, SPV_OpLoad = 61, SPV_OpStore = 62, SPV_OpAccessChain = 65,
       SPV_OpDecorate = 71, SPV_OpMemberDecorate = 72, SPV_OpBitcast = 124, SPV_OpEmitVertex = 218, SPV_OpEndPrimitive = 219,
       SPV_OpLabel = 248, SPV_OpReturn = 253, SPV_OpEntryPoint = 15, SPV_OpExecutionMode = 16, SPV_OpMemoryModel = 14 };
enum { SPV_CAP_GEOMETRY = 2, SPV_CAP_VP_INDEX_LAYER = 5254, SPV_BUILTIN = 11, SPV_LOCATION = 30, SPV_BI_LAYER = 9, SPV_BI_POSITION = 0 };

typedef struct { uint32_t *w; size_t n, cap; int bad; } lgs_buf;
static void lgs_put(lgs_buf *b, uint32_t v) {
    if (b->n == b->cap) { size_t c = b->cap ? 2 * b->cap : 512; uint32_t *p = realloc(b->w, c * 4); if (!p) { b->bad = 1; return; } b->w = p; b->cap = c; }
    b->w[b->n++] = v;
}
static void lgs_op(lgs_buf *b, uint32_t op, int n, const uint32_t *a) { lgs_put(b, (uint32_t)(n + 1) << 16 | op); for (int i = 0; i < n; i++) lgs_put(b, a[i]); }
#define LGS(b, op, ...) do { const uint32_t a_[] = {__VA_ARGS__}; lgs_op(b, op, (int)(sizeof a_ / 4), a_); } while (0)

/* a VS type described structurally, so the geometry shader can declare its own */
typedef struct { uint8_t kind; uint8_t sign; uint8_t count; uint32_t comp; uint32_t len; } lgs_ty;   /* kind: 1 float32, 2 int32, 3 vector, 4 array (len), 5 struct, 6 other */

static int lgs_name_is(const uint32_t *w, uint32_t len, const char *s) { return len * 4 > strlen(s) && !strncmp((const char *)w, s, len * 4) && !strcmp((const char *)w, s); }

/* 1: patched (VS gets *gs / *gs_n when gs is given and the outputs are ones this copies; FS: capability only) */
static int layer_patch(char **spv_p, size_t *len_p, int stage, uint32_t **gs, size_t *gs_n, const char *variant) {
    uint32_t *w = (uint32_t *)*spv_p; size_t nw = *len_p / 4;
    if (nw < 5 || w[0] != 0x07230203u) return 0;
    uint32_t bound = w[3]; if (!bound || bound > (1u << 22)) return 0;
    /* pass 1: is Layer a built-in here at all */
    int has_layer = 0;
    for (size_t i = 5; i < nw; ) { uint32_t op = w[i] & 0xffff, l = w[i] >> 16; if (!l || i + l > nw) return 0;
        if (op == SPV_OpDecorate && l == 4 && w[i + 2] == SPV_BUILTIN && w[i + 3] == SPV_BI_LAYER) has_layer = 1;
        i += l; }
    if (!has_layer) return 0;
    /* drop the extension and capability Vulkan would need for it (the FS gets Geometry, which reads Layer) */
    size_t o = 5;
    for (size_t i = 5; i < nw; ) {
        uint32_t op = w[i] & 0xffff, l = w[i] >> 16;
        int drop = 0;
        if (op == SPV_OpExtension && (lgs_name_is(w + i + 1, l - 1, "SPV_EXT_shader_viewport_index_layer") || lgs_name_is(w + i + 1, l - 1, "SPV_NV_viewport_array2"))) drop = 1;
        if (op == SPV_OpCapability && l == 2 && w[i + 1] == SPV_CAP_VP_INDEX_LAYER) { if (stage == 1) { w[i + 1] = SPV_CAP_GEOMETRY; } else drop = 1; }
        if (!drop) { if (o != i) memmove(w + o, w + i, l * 4); o += l; }
        i += l;
    }
    nw = o; *len_p = nw * 4;
    if (stage != 0) { LOGV("layer: fragment shader %.16s reads Layer: Geometry capability", variant); return 1; }

    /* the vertex shader: types, output variables, decorations */
    lgs_ty *ty = calloc(bound, sizeof *ty); uint32_t *ptr_sc = calloc(bound, 4), *ptr_to = calloc(bound, 4), *cval = calloc(bound, 4);
    uint32_t *loc = calloc(bound, 4); uint8_t *hasloc = calloc(bound, 1), *interp = calloc(bound, 1);
    uint32_t *outs = calloc(bound, 4), nouts = 0;
    uint32_t layer_var = 0, layer_dec = 0, pv_struct = 0, pv_var = 0, version = w[1];
    int ok = ty && ptr_sc && ptr_to && cval && loc && hasloc && interp && outs;
    uint8_t *mbi_pos = calloc(bound, 1);   /* struct id -> 1 + member index of BuiltIn Position */
    uint32_t *struct_at = calloc(bound, 4);   /* struct id -> its OpTypeStruct's word index */
    struct { uint32_t s, m, bi; } mbi[16]; int nmbi = 0;   /* member built-ins (the per-vertex block's) */
    if (!mbi_pos || !struct_at) ok = 0;
    for (size_t i = 5; ok && i < nw; ) {
        uint32_t op = w[i] & 0xffff, l = w[i] >> 16; const uint32_t *a = w + i + 1;
        switch (op) {
        case SPV_OpTypeFloat: if (a[0] < bound) ty[a[0]] = (lgs_ty){a[1] == 32 ? 1 : 6, 0, 1, 0, 0}; break;
        case SPV_OpTypeInt: if (a[0] < bound) ty[a[0]] = (lgs_ty){a[1] == 32 ? 2 : 6, (uint8_t)a[2], 1, 0, 0}; break;
        case SPV_OpTypeVector: if (a[0] < bound) ty[a[0]] = (lgs_ty){3, 0, (uint8_t)a[2], a[1], 0}; break;
        case SPV_OpTypeArray: if (a[0] < bound) ty[a[0]] = (lgs_ty){4, 0, 0, a[1], a[2]}; break;
        case SPV_OpTypeStruct: if (a[0] < bound) { ty[a[0]] = (lgs_ty){5, 0, (uint8_t)(l - 2), 0, 0}; struct_at[a[0]] = (uint32_t)i; } break;
        case SPV_OpTypePointer: if (a[0] < bound) { ptr_sc[a[0]] = a[1]; ptr_to[a[0]] = a[2]; } break;
        case SPV_OpConstant: if (a[1] < bound && l == 4) cval[a[1]] = a[2]; break;
        case SPV_OpVariable: if (a[1] < bound && a[2] == 3 /* Output */) outs[nouts++] = a[1]; break;
        case SPV_OpDecorate:
            if (a[0] >= bound) break;
            if (a[1] == SPV_BUILTIN && l == 4 && a[2] == SPV_BI_LAYER) { layer_var = a[0]; layer_dec = (uint32_t)i; }
            else if (a[1] == SPV_LOCATION && l == 4) { loc[a[0]] = a[2]; hasloc[a[0]] = 1; }
            else if (a[1] == 14 || a[1] == 13 || a[1] == 16 || a[1] == 17) interp[a[0]] |= (uint8_t)(1u << (a[1] - 13));   /* NoPerspective 13, Flat 14, Centroid 16, Sample 17 */
            break;
        case SPV_OpMemberDecorate:
            if (a[0] < bound && l == 5 && a[2] == SPV_BUILTIN) {
                if (a[3] == SPV_BI_POSITION) mbi_pos[a[0]] = (uint8_t)(1 + a[1]);
                if (nmbi < 16) { mbi[nmbi].s = a[0]; mbi[nmbi].m = a[1]; mbi[nmbi].bi = a[3]; nmbi++; }
            }
            break;
        }
        i += l;
    }
    int lgs_ok = 0;
    uint32_t free_loc = 0;
    for (uint32_t k = 0; ok && k < nouts; k++) if (hasloc[outs[k]] && loc[outs[k]] + 1 > free_loc) free_loc = loc[outs[k]] + 1;
    /* each output variable's pointee type: the variable's result type is a pointer */
    uint32_t *var_ty = calloc(bound, 4); if (!var_ty) ok = 0;
    for (size_t i = 5; ok && i < nw; ) { uint32_t op = w[i] & 0xffff, l = w[i] >> 16; if (op == SPV_OpVariable && w[i + 2] < bound && w[i + 1] < bound) var_ty[w[i + 2]] = ptr_to[w[i + 1]]; i += l; }
    if (ok && layer_var && free_loc < 30) {
        /* the patch: Layer -> Location free_loc */
        w[layer_dec + 2] = SPV_LOCATION; w[layer_dec + 3] = free_loc;
        loc[layer_var] = free_loc; hasloc[layer_var] = 1;
        lgs_ok = 1;
        uint32_t lt = var_ty[layer_var];
        if (lt >= bound || ty[lt].kind != 2) lgs_ok = 0;
        /* the per-vertex block (struct with BuiltIn Position) */
        for (uint32_t k = 0; k < nouts; k++) { uint32_t t = var_ty[outs[k]]; if (t < bound && ty[t].kind == 5 && mbi_pos[t]) { pv_struct = t; pv_var = outs[k]; } }
        /* every other output must have a location and a scalar / vector type of 32-bit float or int */
        for (uint32_t k = 0; lgs_ok && k < nouts; k++) {
            uint32_t v = outs[k], t = var_ty[v];
            if (v == layer_var || v == pv_var || !hasloc[v]) continue;   /* built-ins outside the block (PointSize, ...) are not needed by a triangle */
            if (t >= bound) { lgs_ok = 0; break; }
            uint32_t c = ty[t].kind == 3 ? ty[t].comp : t;
            if (!(ty[t].kind == 1 || ty[t].kind == 2 || (ty[t].kind == 3 && c < bound && (ty[c].kind == 1 || ty[c].kind == 2)))) { lgs_ok = 0; break; }
        }
        if (!pv_struct) lgs_ok = 0;
        if (!lgs_ok) RED(16, "layer: vertex shader %s writes Layer but its outputs are not ones the pass-through geometry shader copies", variant);
    }
    if (lgs_ok && gs) {
        /* ---- the geometry shader ---- */
        lgs_buf h = {0}, d = {0}, t = {0}, f = {0};   /* header part, decorations, types/vars, function */
        uint32_t id = 1;
        uint32_t T_void = id++, T_fn = id++, T_f = id++, T_i = id++, T_u = id++, T_v4 = id++;
        uint32_t C_u3 = id++, C_i0 = id++, C_i1 = id++, C_i2 = id++;
        LGS(&t, SPV_OpTypeVoid, T_void); LGS(&t, SPV_OpTypeFunction, T_fn, T_void);
        LGS(&t, SPV_OpTypeFloat, T_f, 32); LGS(&t, SPV_OpTypeInt, T_i, 32, 1); LGS(&t, SPV_OpTypeInt, T_u, 32, 0); LGS(&t, SPV_OpTypeVector, T_v4, T_f, 4);
        LGS(&t, SPV_OpConstant, T_u, C_u3, 3); LGS(&t, SPV_OpConstant, T_i, C_i0, 0); LGS(&t, SPV_OpConstant, T_i, C_i1, 1); LGS(&t, SPV_OpConstant, T_i, C_i2, 2);
        /* GS type of a VS scalar / vector type (vectors deduplicated by component and count) */
        uint32_t vec_ids[3][5] = {{0}};   /* [float / int / uint][count]: one OpTypeVector each */
        vec_ids[0][4] = T_v4;
        #define GS_TYPE(vt) (ty[vt].kind == 1 ? T_f : ty[vt].kind == 2 ? (ty[vt].sign ? T_i : T_u) : 0)
        uint32_t iface[80]; int ni = 0;
        uint32_t main_id = id++;
        /* per location output: input array, output variable */
        typedef struct { uint32_t in, out, type, pin, pout; } gs_io;
        gs_io io[32]; int nio = 0;
        for (uint32_t k = 0; k < nouts && nio < 32; k++) {
            uint32_t v = outs[k], vt = var_ty[v];
            if (v == pv_var || !hasloc[v]) continue;
            uint32_t gt;
            if (ty[vt].kind == 3) {
                uint32_t c = ty[vt].comp, n = ty[vt].count > 4 ? 4 : ty[vt].count;
                uint32_t cls = ty[c].kind == 1 ? 0 : ty[c].sign ? 1 : 2, comp = cls == 0 ? T_f : cls == 1 ? T_i : T_u;
                if (vec_ids[cls][n]) gt = vec_ids[cls][n];
                else { gt = id++; LGS(&t, SPV_OpTypeVector, gt, comp, n); vec_ids[cls][n] = gt; }
            } else gt = GS_TYPE(vt);
            uint32_t arr = id++, p_in_arr = id++, p_in = id++, p_out = 0, vin = id++, vout = 0;
            LGS(&t, SPV_OpTypeArray, arr, gt, C_u3);
            LGS(&t, SPV_OpTypePointer, p_in_arr, 1, arr); LGS(&t, SPV_OpTypePointer, p_in, 1, gt);
            LGS(&t, SPV_OpVariable, p_in_arr, vin, 1);
            if (v != layer_var) { p_out = id++; vout = id++; LGS(&t, SPV_OpTypePointer, p_out, 3, gt); LGS(&t, SPV_OpVariable, p_out, vout, 3); }
            uint32_t L = v == layer_var ? free_loc : loc[v];
            LGS(&d, SPV_OpDecorate, vin, SPV_LOCATION, L);
            if (v != layer_var) {
                LGS(&d, SPV_OpDecorate, vout, SPV_LOCATION, L);
                for (int b = 0; b < 5; b++) if (interp[v] >> b & 1) LGS(&d, SPV_OpDecorate, vout, (uint32_t)(13 + b));
            }
            io[nio++] = (gs_io){vin, v == layer_var ? 0 : vout, gt, p_in, p_out};
            if (ni < 78) { iface[ni++] = vin; if (v != layer_var) iface[ni++] = vout; }
        }
        /* the layer input is one of them; find it */
        int li = -1; { int n = 0; for (uint32_t k = 0; k < nouts; k++) { uint32_t v = outs[k]; if (v == pv_var || !hasloc[v]) continue; if (v == layer_var) li = n; n++; if (n >= 32) break; } }
        /* per-vertex blocks. The input block mirrors the vertex shader's output block member for member (types and
           built-ins: Position, PointSize, ClipDistance[n], CullDistance[n]): built-in blocks must match across the
           interface, and the Maleoon 935 read nothing usable through a Position-only gl_in (both of Z-A's layered
           volumes stayed empty, while a glslang-built geometry shader with the full block filled them). The output block
           is Position alone: no clip or cull distance the geometry shader would leave unwritten. */
        uint32_t S_in = id++, A_in = id++, P_A_in = id++, S_out = id++, P_S_out = id++, P_v4_in = id++, P_v4_out = id++, V_gl_in = id++, V_gl_out = id++;
        uint32_t pv_members[8]; int npv = 0, pos_member = mbi_pos[pv_struct] ? mbi_pos[pv_struct] - 1 : 0;
        {   const uint32_t *sw = w + struct_at[pv_struct]; uint32_t sl = sw[0] >> 16;
            for (uint32_t m = 2; m < sl && npv < 8; m++) {
                uint32_t mt = sw[m], gt = 0;
                if (mt < bound && ty[mt].kind == 1) gt = T_f;
                else if (mt < bound && ty[mt].kind == 3 && ty[mt].count == 4 && ty[mt].comp < bound && ty[ty[mt].comp].kind == 1) gt = T_v4;
                else if (mt < bound && ty[mt].kind == 4 && ty[mt].comp < bound && ty[ty[mt].comp].kind == 1 && ty[mt].len < bound && cval[ty[mt].len]) {
                    uint32_t cn = id++; gt = id++; LGS(&t, SPV_OpConstant, T_u, cn, cval[ty[mt].len]); LGS(&t, SPV_OpTypeArray, gt, T_f, cn);
                }
                if (!gt) { npv = -1; break; }
                pv_members[npv++] = gt;
            }
        }
        if (npv <= pos_member) { npv = 1; pv_members[0] = T_v4; pos_member = 0; }   /* not a block this copies: Position alone, as before */
        { uint32_t sm[10]; sm[0] = S_in; for (int m = 0; m < npv; m++) sm[1 + m] = pv_members[m]; lgs_op(&t, SPV_OpTypeStruct, 1 + npv, sm); }
        LGS(&t, SPV_OpTypeArray, A_in, S_in, C_u3); LGS(&t, SPV_OpTypePointer, P_A_in, 1, A_in);
        LGS(&t, SPV_OpTypeStruct, S_out, T_v4); LGS(&t, SPV_OpTypePointer, P_S_out, 3, S_out);
        LGS(&t, SPV_OpTypePointer, P_v4_in, 1, T_v4); LGS(&t, SPV_OpTypePointer, P_v4_out, 3, T_v4);
        LGS(&t, SPV_OpVariable, P_A_in, V_gl_in, 1); LGS(&t, SPV_OpVariable, P_S_out, V_gl_out, 3);
        LGS(&d, SPV_OpDecorate, S_in, 2 /* Block */);
        if (npv > 1) { for (int k = 0; k < nmbi; k++) if (mbi[k].s == pv_struct && (int)mbi[k].m < npv) LGS(&d, SPV_OpMemberDecorate, S_in, mbi[k].m, SPV_BUILTIN, mbi[k].bi); }
        else LGS(&d, SPV_OpMemberDecorate, S_in, 0, SPV_BUILTIN, SPV_BI_POSITION);
        LGS(&d, SPV_OpDecorate, S_out, 2); LGS(&d, SPV_OpMemberDecorate, S_out, 0, SPV_BUILTIN, SPV_BI_POSITION);
        uint32_t C_pos = pos_member == 0 ? C_i0 : pos_member == 1 ? C_i1 : C_i2;
        if (pos_member > 2) { C_pos = id++; LGS(&t, SPV_OpConstant, T_i, C_pos, (uint32_t)pos_member); }
        uint32_t P_i_out = id++, V_layer = id++;
        LGS(&t, SPV_OpTypePointer, P_i_out, 3, T_i); LGS(&t, SPV_OpVariable, P_i_out, V_layer, 3);
        LGS(&d, SPV_OpDecorate, V_layer, SPV_BUILTIN, SPV_BI_LAYER);
        iface[ni++] = V_gl_in; iface[ni++] = V_gl_out; iface[ni++] = V_layer;
        /* main */
        uint32_t lbl = id++;
        LGS(&f, SPV_OpFunction, T_void, main_id, 0, T_fn); LGS(&f, SPV_OpLabel, lbl);
        uint32_t lay = 0;
        if (li >= 0) {
            uint32_t p = id++, x = id++; LGS(&f, SPV_OpAccessChain, io[li].pin, p, io[li].in, C_i0); LGS(&f, SPV_OpLoad, io[li].type, x, p);
            if (io[li].type != T_i) { uint32_t y = id++; LGS(&f, SPV_OpBitcast, T_i, y, x); x = y; }
            lay = x;
        }
        const uint32_t cv[3] = {C_i0, C_i1, C_i2};
        for (int v = 0; v < 3; v++) {
            for (int k = 0; k < nio; k++) if (io[k].out) {
                uint32_t p = id++, x = id++; LGS(&f, SPV_OpAccessChain, io[k].pin, p, io[k].in, cv[v]); LGS(&f, SPV_OpLoad, io[k].type, x, p); LGS(&f, SPV_OpStore, io[k].out, x);
            }
            uint32_t pi = id++, pos = id++, po = id++;
            LGS(&f, SPV_OpAccessChain, P_v4_in, pi, V_gl_in, cv[v], C_pos); LGS(&f, SPV_OpLoad, T_v4, pos, pi);
            LGS(&f, SPV_OpAccessChain, P_v4_out, po, V_gl_out, C_i0); LGS(&f, SPV_OpStore, po, pos);
            if (lay) LGS(&f, SPV_OpStore, V_layer, lay);
            LGS(&f, SPV_OpEmitVertex);
        }
        LGS(&f, SPV_OpEndPrimitive); LGS(&f, SPV_OpReturn); LGS(&f, SPV_OpFunctionEnd);
        /* header */
        lgs_put(&h, 0x07230203u); lgs_put(&h, version < 0x10000 ? 0x10000 : version); lgs_put(&h, 0); lgs_put(&h, id); lgs_put(&h, 0);
        LGS(&h, SPV_OpCapability, SPV_CAP_GEOMETRY);
        LGS(&h, SPV_OpMemoryModel, 0, 1);
        { uint32_t ep[96]; int n = 0; ep[n++] = 3 /* Geometry */; ep[n++] = main_id; ep[n++] = 0x6e69616du /* "main" */; ep[n++] = 0; for (int k = 0; k < ni; k++) ep[n++] = iface[k]; lgs_op(&h, SPV_OpEntryPoint, n, ep); }
        LGS(&h, SPV_OpExecutionMode, main_id, 22 /* Triangles */); LGS(&h, SPV_OpExecutionMode, main_id, 29 /* OutputTriangleStrip */);
        LGS(&h, SPV_OpExecutionMode, main_id, 26 /* OutputVertices */, 3); LGS(&h, SPV_OpExecutionMode, main_id, 0 /* Invocations */, 1);
        lgs_buf all = {0};
        for (size_t i = 0; i < h.n; i++) lgs_put(&all, h.w[i]);
        for (size_t i = 0; i < d.n; i++) lgs_put(&all, d.w[i]);
        for (size_t i = 0; i < t.n; i++) lgs_put(&all, t.w[i]);
        for (size_t i = 0; i < f.n; i++) lgs_put(&all, f.w[i]);
        all.w[3] = id;
        if (h.bad || d.bad || t.bad || f.bad || all.bad) { free(all.w); lgs_ok = 0; }
        else { *gs = all.w; *gs_n = all.n; }
        free(h.w); free(d.w); free(t.w); free(f.w);
        #undef GS_TYPE
        if (lgs_ok) LOGV("layer: vertex shader %.16s writes Layer: now at location %u, pass-through geometry shader of %d outputs", variant, free_loc, nio);
    }
    free(ty); free(ptr_sc); free(ptr_to); free(cval); free(loc); free(hasloc); free(interp); free(outs); free(mbi_pos); free(var_ty); free(struct_at);
    return lgs_ok;
}
#undef LGS
