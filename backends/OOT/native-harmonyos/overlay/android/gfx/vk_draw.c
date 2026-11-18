/* Drawing on Vulkan (vk_int.h): the bound NVN state, render passes as dynamic rendering, graphics pipelines,
   descriptor sets, draws, compute dispatches, texture copies and clears, and present. The Metal counterpart is
   nvn_mtl.m state_op / draw_inner / encode_args / pso_desc / copy_* / clear_tex / dispatch; the rules are the same,
   the many Metal-only optimisations (tile memory, pass merging, extrapolation) are not carried over.

   Conventions shared with the translated shaders (tools/totk_xlate, Metal's): clip depth 0..1 (NEAR_IS_MINUS_W is
   converted in the vertex shader), framebuffer origin at the top left, clip-space y up. Vulkan's clip y points down,
   so the viewport is flipped (negative height), which places every pixel where Metal places it. */
#include "vk_int.h"
#include "vk_record.h"
#include "vk_framegen.h"
#include "vk_framegen_qcom.h"
#include "vk_framegen_lsfg.h"
#include "vk_overlay.h"
#include "framegen_math.h"
#include "framegen_dependencies.h"
#include "framegen_passes.h"
#include "vk_canvas.h"
#include "vk_ui_aspect.h"
#include "nvn_hmap.h"
#include "nvn_dirty.h"
#include "totk_lifelog.h"
#include "dm_wait_stats.h"
#include "nvn_scan_budget.h"
#include "../../runtime/frame_timeline.h"
#include "../../runtime/present_clock.h"
#include "../../runtime/present_predict.h"
#include "../../runtime/fps_auto_runtime.h"
#include "../../runtime/fps_hybrid_runtime.h"
#include "../../runtime/fps_adaptive_runtime.h"
#include "framegen_adaptive.h"
#include "framegen_adaptive_plan.h"
#include "framegen_adaptive_present.h"
#include "framegen_adaptive_slots.h"
#include "framegen_adaptive_publication.h"
#include "framegen_adaptive_latch.h"
#include "framegen_adaptive_defaults.h"
#include "framegen_job_trace.h"
static void fg_adaptive_present_cancel(void);
/* TOTK_ASPECT / key 4 (runtime/aspect_patch.h): the ratio the canvas is shown at, 0 = its own shape. Weak: GPU-only
   tests link no runtime. */
extern float runtime_aspect_now(void) __attribute__((weak));
static float pres_shown_aspect(void) { return runtime_aspect_now ? runtime_aspect_now() : 0; }
/* This draw's HUD scale for that ratio (vk_ui_aspect.h), set by draw_inner and applied by dynamic_state. */
static vk_ui_aspect g_ui_aspect;
static uint8_t g_push_block[2][NVN_PUSH_SIZE];   /* NVN2 push constants: graphics, compute (OP_PUSH_CONST) */
static uint64_t g_push_rev[2] = {1, 1};          /* bumped by every OP_PUSH_CONST of that block: descriptor keys */
/* The descriptor of a stage's push-constant block (RB_PUSH, c[24]): this draw's or dispatch's copy, made once per block
   revision and submission. */
static void push_desc(int stage, VkDescriptorBufferInfo *out) {
    static uint64_t rev[2], sub[2]; static VkDescriptorBufferInfo d[2];
    int k = stage == 5; uint64_t s = vk_submit_serial();
    if (rev[k] != g_push_rev[k] || sub[k] != s || !d[k].buffer) {
        VkBuffer b; VkDeviceSize o;
        void *p = vk_transient(NVN_PUSH_SIZE, V.props.limits.minUniformBufferOffsetAlignment, &b, &o);
        memcpy(p, g_push_block[k], NVN_PUSH_SIZE);
        d[k] = (VkDescriptorBufferInfo){b, o, NVN_PUSH_SIZE}; rev[k] = g_push_rev[k]; sub[k] = s;
    }
    *out = d[k];
}
static int g_ui_aspect_on;
#include "../../runtime/pgo_dump.h"
#include "../../runtime/android_perf.h"
#include <math.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <time.h>
#include <errno.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <pthread.h>
#include <stddef.h>
#include <unistd.h>
#include "pso_trace_key.h"
#include "vk_screen_classify.h"

#define MAXV(a, b) ((a) > (b) ? (a) : (b))
#define MINV(a, b) ((a) < (b) ? (a) : (b))
vk_state S;
static uint64_t g_stat_draws, g_stat_skips[16], g_stat_passes, g_stat_pipes, g_stat_dispatch;
vk_hitch g_hk;
int g_hitch_on;
/* Per 300 frames: graphics pipeline creation (count, time, longest) and frame times (longest, over 33 / 50 ms): stutter. */
static uint64_t g_win_pipes, g_win_pipe_ns, g_win_pipe_max_ns, g_win_frame_max_ns, g_win_over33, g_win_over50, g_last_present_ns;
static uint64_t g_win_sync, g_win_sync_ns, g_win_prewarm_wait, g_win_prewarm_wait_ns;
static _Atomic uint64_t g_cum_pipe_ns;   /* all graphics pipeline creation, for the long-frame log */
static uint64_t g_chunk_draws;   /* draws recorded in the open slot (since the last slot was sealed: vk_draw_slot_sealed) */
static uint64_t g_chunk_held;    /* pass starts whose chunk submission waited for a run of mesh draws to end */
/* What the GPU's vertex stage is asked for (oot-a104): vertices or indices times instances of the draws recorded, and
   apart the index capacity of the mesh draws (their real count is the kernels'). nvn_vk.c prints them every 300 frames. */
static uint64_t g_geo_verts, g_geo_mesh_cap;
void vk_draw_geometry(uint64_t out[2]) { out[0] = g_geo_verts; out[1] = g_geo_mesh_cap; g_geo_verts = g_geo_mesh_cap = 0; }
/* nvn_vk.c, when a slot is sealed (a chunk, the end of a game submission, a wait for the GPU): the next slot's chunk
   counts from its own first draw. Until oot-a103 only the two chunk sites zeroed the counter, so a submission that ended
   with 75 draws or more since its last chunk made the next one submit a slot holding nothing but its first uploads and
   dispatches. TOTK_VK_CHUNK_CARRY=1: as before. */
void vk_draw_slot_sealed(void) {
    static int carry = -1; if (carry < 0) { const char *e = getenv("TOTK_VK_CHUNK_CARRY"); carry = e && *e == '1'; }
    if (!carry) g_chunk_draws = 0;
}
static int g_pipe_dirty = 1;   /* key-feeding state ops and pass rebuilds; resolved shaders and topology are checked separately */
/* Exact inputs of the generated driver banks; advance only when the bytes they encode change. */
static uint64_t g_driver_revision[2][6];
static uint64_t g_record_bind_revision[6], g_record_freeze_hits, g_record_freeze_misses;
static struct { uint64_t tex[2]; uint32_t img; } g_native_bound[6];
static uint32_t g_native_bound_any;
static uint64_t g_native_bound_revision;
/* TOTK_VK_PREPARE_MASK=1 (default off): pre-render uniform preparation (vk_lag_uniform_prepare) runs only for consumed
   registers marked dirty here. A register is dirtied when its bound address or size changes; every register is dirtied
   when an overlay, original-memory producer or mapping revision advances (vk_lag_uniform_prepare_changed). Full bound
   ranges are prepared, so shader switches need no invalidation: newly consumed registers keep their dirty bits. This
   skips only pre-render materialization, never lag selection, snapshots or guards. TOTK_VK_OFF=prepare (or all) keeps
   it off; TOTK_VK_PREPARE_CHECK=1 runs every skipped preparation as an oracle and dies on a missed materialization.
   Live (runtime/live_ctl.h): read at each draw's preparation; a change dirties every register, so the first draw after
   it prepares in full. The masks and counters are walking-lock owned. */
static int g_prepare_mask_launch, g_prepare_off, g_prepare_check, g_prepare_used;
static totk_live_cache g_prepare_mask_live;
static int g_alpha_launch;
static totk_live_cache g_alpha_live;
static uint64_t g_alpha_enabled, g_alpha_ignored;
static int g_clear_read_launch;
static totk_live_cache g_clear_read_live;
static uint64_t g_clear_read_forced;
static uint16_t g_uniform_prepare_dirty[2] = {UINT16_MAX, UINT16_MAX};
static uint64_t g_prepare_calls, g_prepare_skips, g_prepare_checks;
static int prepare_mask_live(void) {
    return !g_prepare_off && totk_live_cached(&g_prepare_mask_live, "TOTK_VK_PREPARE_MASK", g_prepare_mask_launch);
}
#ifdef TOTK_VK_TESTS
static uint64_t g_test_widened;
void vk_record_test_counts_get(vk_record_test_counts *out) {
    *out = (vk_record_test_counts){g_stat_draws, g_stat_passes, g_stat_skips[8], g_test_widened,
        g_record_freeze_hits, vk_prog_record_test_hits()};
}
#endif

static void state_reset(void) {
    g_pipe_dirty = 1;
    vk_stage_sig_invalidate(0x3fu);
    for (int b = 0; b < 2; b++) for (int st = 0; st < 6; st++) g_driver_revision[b][st]++;
    memset(&S, 0, sizeof S);
    memset(g_native_bound,0,sizeof g_native_bound); g_native_bound_any=0; g_native_bound_revision=0;
    g_uniform_prepare_dirty[0] = g_uniform_prepare_dirty[1] = UINT16_MAX;
    for (int i = 0; i < 8; i++) S.blend[i] = (blend_st){i, NVN_BLEND_FUNC_ONE, NVN_BLEND_FUNC_ZERO, NVN_BLEND_FUNC_ONE, NVN_BLEND_FUNC_ZERO, NVN_BLEND_EQUATION_ADD, NVN_BLEND_EQUATION_ADD};
    S.color = (color_st){0, NVN_LOGIC_OP_COPY, NVN_ALPHA_FUNC_ALWAYS}; S.chmask.mask = 0xffffffffu;
    S.poly = (poly_st){NVN_FACE_NONE, NVN_FRONT_FACE_CCW, NVN_POLYGON_MODE_FILL, 0};
    S.ds = (ds_st){0, 1, NVN_DEPTH_FUNC_LESS, 0, NVN_STENCIL_FUNC_ALWAYS, NVN_STENCIL_FUNC_ALWAYS, 1, 1, 1, 1, 1, 1};
    S.st_vmask[0] = S.st_vmask[1] = S.st_wmask[0] = S.st_wmask[1] = 0xff; S.dr[1] = 1;
    for (int i = 0; i < 6; i++) S.tex_hi[i] = 1;
}
static void pso_life_init(void);
static void state_init(void) { static int done; if (!done) { done = 1; pso_life_init(); state_reset(); } }

/* ---------- render passes ---------- */
static struct {
    int open, secondary;                        /* primary rendering contents: inline or worker secondaries */
    int clear_c[8]; VkClearValue cc[8]; int clear_d, clear_s; float cd; uint32_t cs;
    uint32_t w, h;                              /* extent of the open pass */
    uint32_t host_w, host_h; int native, canvas; /* logical support-buffer size remains w/h */
    uint32_t layers;                            /* layers the open pass renders into (layered targets: render_target_array_index) */
    uint32_t native_w, native_h;                /* active coordinate basis, distinct from padded window allocation */
    VkFormat cfmt[8]; VkFormat dfmt, sfmt; uint32_t ncolor; int dclass[8];
    vk_tex *ctex[8]; vk_tex *dtex;
    const void *caddr[8], *daddr; uint32_t cleared;   /* the attachments' guest memory, and which were cleared at the start (bit 8: depth) (TOTK_VK_PASS_DUMP) */
    uint8_t clevel[8], dlevel; uint16_t clayer[8], dlayer;   /* the attachments' view level and layer (TOTK_VK_PASS_DUMP) */
    uint64_t c_serial[8], d_serial; int c_nvnfmt[8];
    unsigned c0_level, c0_layer; int c0_cleared; uint64_t c0_serial; int c0_fmt; uint64_t draws0;   /* pass probe */
    VkSampleCountFlagBits samples;
    uint32_t merge_pending, merge_armed, merge_storage, merge_continued;
    int store_discard, split_refused;           /* a storeOp DONT_CARE was chosen at begin; the split was refused once */
    uint32_t split_draws;                       /* TOTK_VK_SPLIT_PASS_DRAWS taken when this pass (part) opened */
    uint32_t feedback_pending, feedback_written, feedback_sampled, feedback_continued;
    int feedback_on, feedback_unknown, feedback_armed, feedback_restore;
    uint32_t pool_alias; int pool_alias_known;   /* TOTK_OHOS_BARRIER_ELIDE: the targets a sampled texture pool ID lies over */
} R;
static VkPipeline g_bound_pipe; static int g_dyn_dirty = 1;
static vk_record_draw *g_record_draw;  /* walking-only packet under construction, NULL on original path */
static int g_native_draw_1x;
static vk_tex *g_native_sample;                 /* only the verified terminal normalized transfer */
static void descriptor_unbind(void);
static void fg_extract_pass_begin(uint32_t cleared);
static void fg_extract_pass_end(void);
static void fg_extract_authority_span_end(VkCommandBuffer cb);
static void fg_extract_writer_end(VkCommandBuffer cb);
#ifdef TOTK_VK_TESTS
static int fg_test_writer_classification=-1;
#endif
static int fg_extract_inline(void);
static void fg_extract_refresh(int begin_pass);
static int fg_extract_ui_clear(const VkClearAttachment *attachment, const VkClearRect *rect);

/* Bounded, opt-in lifetime evidence. All event order is the recording owner's order, not GPU completion.
   No state is allocated and no descriptor walk is added with TOTK_PSO_LIFE unset. */
typedef struct {
    uint64_t image, incarnation;
    uint32_t mip, layer, aspect, stage, binding;
} pso_life_read_key;
static struct {
    int enabled;
    unsigned w, h, limit;
    uint64_t frame, start, reads, writes, unknown, total_unknown, memo_pass, memo_sub;
    uint64_t color_draws[8], depth_draws, stencil_draws;
    pso_life_read_key *memo;
    size_t nmemo;
    VkImageView color_view[8], depth_view, stencil_view;
    uint32_t color_clear, depth_clear;
} PL;
static uint64_t pso_life_number(const char *name, uint64_t fallback, uint64_t maximum) {
    const char *value = getenv(name); char *end = NULL;
    if (!value || !*value) return fallback;
    unsigned long long number = strtoull(value, &end, 10);
    return end && !*end && number && number <= maximum ? number : fallback;
}
static void pso_life_init(void) {
    const char *value = getenv("TOTK_PSO_LIFE"); char tail;
    if (!value || !*value || !strcmp(value, "0")) return;
    if (sscanf(value, "%ux%u%c", &PL.w, &PL.h, &tail) != 2 || !PL.w || !PL.h || PL.w > 16384 || PL.h > 16384) {
        LOGV("[pso-life] kind=unknown frame=1 reason=invalid-extent"); return;
    }
    PL.frame = 1; PL.start = pso_life_number("TOTK_PSO_LIFE_START", 1, UINT32_MAX);
    PL.limit = (unsigned)pso_life_number("TOTK_PSO_LIFE_FRAMES", 12000, 30000);
    PL.memo = calloc(2048, sizeof *PL.memo);
    LOGV("[pso-life] kind=begin w=%u h=%u start=%llu limit=%u", PL.w, PL.h, (unsigned long long)PL.start, PL.limit);
    if (!PL.memo) {
        LOGV("[pso-life] kind=unknown frame=%llu reason=allocation-failed", (unsigned long long)PL.start);
        LOGV("[pso-life] kind=end frame=%llu complete=0 unknown=1", (unsigned long long)PL.start);
        return;
    }
    PL.enabled = 1;
}
static int pso_life_active(void) {
    return PL.enabled && PL.frame >= PL.start && PL.frame - PL.start < PL.limit;
}
static void pso_life_unknown(const char *reason) {
    if (!pso_life_active()) return;
    PL.unknown++; PL.total_unknown++;
    LOGV("[pso-life] kind=unknown frame=%llu reason=%s", (unsigned long long)PL.frame, reason);
}
static int pso_life_match(const vk_canvas_image *image, unsigned mip) {
    return mip < 32 && MAXV(image->width >> mip, 1u) == PL.w && MAXV(image->height >> mip, 1u) == PL.h;
}
static void pso_life_write(vk_canvas_image image, unsigned mip, unsigned layer, unsigned aspect,
                           int cleared, uint64_t draws, const char *source) {
    if (!pso_life_active() || !pso_life_match(&image, mip)) return;
    PL.writes++;
    LOGV("[pso-life] kind=write frame=%llu sub=%llu pass=%llu image=%#llx incarnation=%llu mip=%u layer=%u aspect=%u "
         "w=%u h=%u format=%u serial=%llu storage=%#llx cleared=%d draws=%llu source=%s",
         (unsigned long long)PL.frame, (unsigned long long)vk_game_submit_serial(), (unsigned long long)g_stat_passes,
         (unsigned long long)image.image, (unsigned long long)image.incarnation, mip, layer, aspect, PL.w, PL.h,
         image.format, (unsigned long long)image.serial, (unsigned long long)image.storage, cleared,
         (unsigned long long)draws, source);
}
void vk_pso_life_external_write(vk_tex *x, unsigned mip, unsigned layer, unsigned layers, const char *source) {
    if (!pso_life_active() || !x) return;
    vk_canvas_image image = vk_tex_canvas_identity(x, 0, 0);
    if (!pso_life_match(&image, mip)) return;
    if (R.open) pso_life_unknown("external-write-inside-pass");
    if (layers > 256) { pso_life_unknown("write-layer-range"); return; }
    if (vk_tex_is3d(x)) { pso_life_unknown("3d-write-range"); return; }
    unsigned aspects = vk_tex_aspect(x);
    for (unsigned i = 0; i < layers; i++) for (unsigned bit = 1; bit <= VK_IMAGE_ASPECT_STENCIL_BIT; bit <<= 1)
        if (aspects & bit) pso_life_write(image, mip, layer + i, bit, 0, 0, source);
}
/* Non-descriptor consumers are deliberately not certified by this diagnostic's descriptor lifetime proof. */
void vk_pso_life_external_read(uint32_t width, uint32_t height, const char *source) {
    if (pso_life_active() && width == PL.w && height == PL.h) pso_life_unknown(source);
}
static void pso_life_view_write(vk_tex *object, VkImageView view, unsigned aspect,
                                int cleared, uint64_t draws, const char *source) {
    if (!pso_life_active() || !object) return;
    if ((R.w != PL.w || R.h != PL.h) && (R.host_w != PL.w || R.host_h != PL.h)) return;
    vk_canvas_image image; VkImageSubresourceRange range;
    if (!vk_tex_view_identity(object, view, &image, &range)) { pso_life_unknown("writer-view-identity"); return; }
    int full = cleared && R.host_w == MAXV(image.width >> range.baseMipLevel, 1u) &&
        R.host_h == MAXV(image.height >> range.baseMipLevel, 1u);
    pso_life_write(image, range.baseMipLevel, range.baseArrayLayer, aspect, full, draws, source);
}
static void pso_life_pass_write(int opening) {
    if (!pso_life_active()) return;
    for (unsigned i = 0; i < R.ncolor && i < 8; i++)
        if (opening ? (PL.color_clear & (1u << i)) != 0 : PL.color_draws[i] != 0)
            pso_life_view_write(R.ctex[i], PL.color_view[i], VK_IMAGE_ASPECT_COLOR_BIT,
                (PL.color_clear & (1u << i)) != 0, opening ? 0 : PL.color_draws[i], opening ? "load-clear" : "render");
    for (unsigned bit = VK_IMAGE_ASPECT_DEPTH_BIT; bit <= VK_IMAGE_ASPECT_STENCIL_BIT; bit <<= 1) {
        uint64_t draws = bit == VK_IMAGE_ASPECT_DEPTH_BIT ? PL.depth_draws : PL.stencil_draws;
        if (R.dtex && (vk_tex_aspect(R.dtex) & bit) && (opening ? (PL.depth_clear & bit) != 0 : draws != 0))
            pso_life_view_write(R.dtex, bit == VK_IMAGE_ASPECT_DEPTH_BIT ? PL.depth_view : PL.stencil_view,
                bit, (PL.depth_clear & bit) != 0, opening ? 0 : draws, opening ? "load-clear" : "render");
    }
}
static void pso_life_present(void) {
    if (!PL.enabled) return;
    /* Close the old frame before advancing even when a delayed window has not started yet. */
    vk_end_rendering_at(VK_PASS_END_PRESENT, 0);
    if (pso_life_active()) {
        LOGV("[pso-life] kind=frame-end frame=%llu reads=%llu writes=%llu unknown=%llu",
             (unsigned long long)PL.frame, (unsigned long long)PL.reads, (unsigned long long)PL.writes, (unsigned long long)PL.unknown);
        if (PL.frame - PL.start + 1 == PL.limit) {
            LOGV("[pso-life] kind=end frame=%llu complete=1 unknown=%llu", (unsigned long long)PL.frame,
                 (unsigned long long)PL.total_unknown);
            PL.enabled = 0; free(PL.memo); PL.memo = NULL;
        }
    }
    PL.frame++; PL.reads = PL.writes = PL.unknown = 0; PL.nmemo = 0;
}
/* Opt-in until measured on Turnip: splitting a full clear can recover LRZ but costs another attachment load/store. */
static int store_clear_on(void) { static int v = -1; if (v < 0) { const char *e = getenv("TOTK_VK_STORE_CLEAR"); v = e && atoi(e) == 1; } return v; }
static int clear_load_on(void) { static int v = -1; if (v < 0) { const char *e = getenv("TOTK_VK_CLEAR_LOAD"); v = (e && atoi(e) == 1) || store_clear_on(); } return v; }
static uint64_t g_clear_load_c, g_clear_load_ds, g_clear_inline_d, g_store_clear_c, g_store_clear_ds, g_store_clear_pass;
static void gpu_work_stats(uint64_t frame) {   /* recording lock held; same 300-frame window as the GPU time */
    if (!clear_load_on()) return;
    LOGV("frame %llu, GPU work per frame: full-clear splits color %.2f, depth/stencil %.2f; inline depth clears %.2f; stores skipped color %.2f, depth/stencil %.2f in %.2f passes (CLEAR_LOAD %d STORE_CLEAR %d)",
         (unsigned long long)frame, g_clear_load_c / 300.0, g_clear_load_ds / 300.0, g_clear_inline_d / 300.0,
         g_store_clear_c / 300.0, g_store_clear_ds / 300.0, g_store_clear_pass / 300.0, clear_load_on(), store_clear_on());
    g_clear_load_c = g_clear_load_ds = g_clear_inline_d = g_store_clear_c = g_store_clear_ds = g_store_clear_pass = 0;
}

/* ---------- GPU pass profile (TOTK_VK_PASS_PROFILE=<every n-th frame>, off by default) ----------
   On a profiled frame every render pass and compute dispatch gets a timestamp before and after it (passes and dispatches
   already have a full barrier on both sides, so the timestamps add no stall of their own). Once the frame's fence has
   signalled (checked at later presents, never waited for) the log gets the frame's GPU span, the time in passes and in
   dispatches, and the passes grouped by target size and formats, costliest first. */
enum { PP_Q = 4096, PP_REC = PP_Q / 2 };
typedef struct { uint32_t q0, w, h, draws, vpw, vph; VkFormat f0, df; uint8_t kind, ncolor, native_op;
#ifdef TOTK_OHOS
    uint16_t ev[PPE_N];   /* recorded between the scope before and this one */
#endif
} pp_rec; /* kind 0 pass, 1 dispatch, 2+ native operation; native_op is op+1 */
#ifdef TOTK_OHOS
uint32_t vk_pp_ev[PPE_N];
static uint32_t pp_ev_mark[PPE_N];
#endif
static struct { long every; VkQueryPool qp; uint32_t nq; pp_rec rec[PP_REC]; int nrec, open, active, pending; uint32_t dropped; uint64_t frame, fence, draws0; } PP = {.every = -1, .open = -1};
static const char *const native_op_name[VK_NUI_OPS] = {
    "canvas-seed", "window-seed", "shadow-resolve", "region-patch", "ui-pass",
    "window-pass", "present-ahb", "present-copy", "host-post"
};
static uint64_t native_count[VK_NUI_OPS], native_skipped[VK_NUI_OPS];
static uint64_t native_report_count[VK_NUI_OPS], native_report_skipped[VK_NUI_OPS];
static const vk_shader *pp_sh[PP_REC];   /* a dispatch record's compute shader: the report's time by shader */
static uint32_t pp_ran[PP_REC];          /* a mesh / task kernel dispatch: the workgroups the same draw ran in an earlier frame (its feedback) */
static void pp_ts(VkCommandBuffer cb, uint32_t q) { vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, PP.qp, q); }
static int pp_begin(VkCommandBuffer cb, int kind) {   /* a timestamp before the work; the record index, or -1 */
    if (!PP.active) return -1;
    if (PP.nq + 2 > PP_Q || PP.nrec >= PP_REC) { PP.dropped++; return -1; }
    pp_rec *r = &PP.rec[PP.nrec]; memset(r, 0, sizeof *r); r->q0 = PP.nq; r->kind = (uint8_t)kind; PP.nq += 2;
    pp_sh[PP.nrec] = NULL; pp_ran[PP.nrec] = 0;
#ifdef TOTK_OHOS
    for (int k = 0; k < PPE_N; k++) { uint32_t n = vk_pp_ev[k] - pp_ev_mark[k]; r->ev[k] = n > 65535 ? 65535 : (uint16_t)n; }
#endif
    pp_ts(cb, r->q0); return PP.nrec++;
}
static void pp_end(VkCommandBuffer cb, int i) {
    if (i >= 0) pp_ts(cb, PP.rec[i].q0 + 1);
#ifdef TOTK_OHOS
    if (i >= 0) memcpy(pp_ev_mark, vk_pp_ev, sizeof pp_ev_mark);
#endif
}
int vk_native_gpu_begin(VkCommandBuffer cb, unsigned op) {
    if (op >= VK_NUI_OPS) return -1;
    native_count[op]++;
    if (vk_gpuprof_on) {
        static const char *const labels[VK_NUI_OPS] = {
            "upscale", "upscale", "transfer", "transfer", "upscale",
            "upscale", "present", "present", "post"
        };
        vk_gpuprof_scope_begin(cb, labels[op]);
    }
    return pp_begin(cb,2+(int)op);
}
void vk_native_gpu_end(VkCommandBuffer cb, int scope) {
    pp_end(cb,scope);
    if (vk_gpuprof_on) vk_gpuprof_scope_end(cb);
}
void vk_native_gpu_skip(unsigned op) { if (op < VK_NUI_OPS) native_skipped[op]++; }
#ifdef TOTK_VK_TESTS
void vk_native_gpu_counts(uint64_t executed[VK_NUI_OPS], uint64_t skipped[VK_NUI_OPS]) {
    memcpy(executed,native_count,sizeof native_count); memcpy(skipped,native_skipped,sizeof native_skipped);
}
#endif
static void native_gpu_stats(uint64_t frame) {
    if (!vk_native_ui_enabled()) return;
    for (unsigned op=0;op<VK_NUI_OPS;op++) {
        uint64_t n=native_count[op]-native_report_count[op], skipped=native_skipped[op]-native_report_skipped[op];
        LOGV("frame %llu, native UI work per frame: %s %.3f, full-clear avoided %.3f",
            (unsigned long long)frame,native_op_name[op],n/300.0,skipped/300.0);
        native_report_count[op]=native_count[op]; native_report_skipped[op]=native_skipped[op];
    }
}
typedef struct { uint32_t w, h, n, draws, vpw, vph; VkFormat f0, df; uint8_t ncolor; double ms; } pp_group;
static int pp_group_cmp(const void *a, const void *b) { double x = ((const pp_group *)a)->ms, y = ((const pp_group *)b)->ms; return x < y ? 1 : x > y ? -1 : 0; }
static void pp_report(void) {
    PP.pending = 0;
    uint64_t *t = malloc((size_t)PP.nq * 8); if (!t) return;
    VkResult r = vkGetQueryPoolResults(V.dev, PP.qp, 0, PP.nq, (size_t)PP.nq * 8, t, 8, VK_QUERY_RESULT_64_BIT);
    if (r) { LOGV("pass profile frame %llu: query results not available (%d)", (unsigned long long)PP.frame, r); free(t); return; }
    double tick = V.props.limits.timestampPeriod / 1e6, span = (double)(t[1] - t[0]) * tick, pass_ms = 0, disp_ms = 0;
    double native_ms[VK_NUI_OPS]={0}, operation_ms=0; uint32_t native_n[VK_NUI_OPS]={0};
    static pp_group g[512]; int ng = 0, npass = 0, ndisp = 0;
    static struct { const vk_shader *sh; double ms; uint32_t n; uint64_t ran; } ds[256]; int nds = 0;
    double kind_ms[3] = {0}, size_ms[6] = {0}; uint32_t kind_n[3] = {0}, size_n[6] = {0};   /* oot-a97: by kind (mesh, task, compute) and by duration */
    for (int i = 0; i < PP.nrec; i++) {
        const pp_rec *e = &PP.rec[i]; double ms = t[e->q0 + 1] > t[e->q0] ? (double)(t[e->q0 + 1] - t[e->q0]) * tick : 0;
        if (e->kind>=2) {
            unsigned op=e->kind-2; native_ms[op]+=ms; native_n[op]++; operation_ms+=ms; continue;
        }
        if (e->native_op) { unsigned op=e->native_op-1; native_ms[op]+=ms; native_n[op]++; }
        if (e->kind==1) {
            disp_ms += ms; ndisp++;
            { int k = pp_sh[i] && pp_sh[i]->mesh_stage == 7 ? 0 : pp_sh[i] && pp_sh[i]->mesh_stage == 6 ? 1 : 2, z = ms < 0.05 ? 0 : ms < 0.1 ? 1 : ms < 0.2 ? 2 : ms < 0.5 ? 3 : ms < 1 ? 4 : 5;
              kind_ms[k] += ms; kind_n[k]++; size_ms[z] += ms; size_n[z]++; }
            if (pp_sh[i]) {   /* by shader: the costliest dispatches (the mesh / task kernels of the mesh draws among them) */
                int k = 0; while (k < nds && ds[k].sh != pp_sh[i]) k++;
                if (k == nds && nds < 256) { ds[nds].sh = pp_sh[i]; ds[nds].ms = 0; ds[nds].n = 0; ds[nds].ran = 0; nds++; }
                if (k < nds) { ds[k].ms += ms; ds[k].n++; ds[k].ran += pp_ran[i]; }
            }
            continue;
        }
        pass_ms += ms; npass++;
        int k = 0; while (k < ng && !(g[k].w == e->w && g[k].h == e->h && g[k].f0 == e->f0 && g[k].df == e->df && g[k].ncolor == e->ncolor)) k++;
        if (k == ng) { if (ng == 512) continue; g[ng++] = (pp_group){e->w, e->h, 0, 0, 0, 0, e->f0, e->df, e->ncolor, 0}; }
        g[k].n++; g[k].draws += e->draws; g[k].ms += ms; g[k].vpw = MAXV(g[k].vpw, e->vpw); g[k].vph = MAXV(g[k].vph, e->vph);
    }
    free(t);
    qsort(g, (size_t)ng, sizeof *g, pp_group_cmp);
    LOGV("pass profile frame %llu: GPU span %.2f ms; %d passes %.2f ms, %d dispatches %.2f ms, native/host transfers %.3f ms; unscoped (copies, barriers, idle) %.2f ms, dropped scopes %u",
         (unsigned long long)PP.frame,span,npass,pass_ms,ndisp,disp_ms,operation_ms,span-pass_ms-disp_ms-operation_ms,PP.dropped);
#ifdef TOTK_OHOS
    {   /* the time between scopes, by what was recorded there (a gap that holds several kinds counts under the first of:
           a submission boundary, an image copy / blit, a buffer copy / fill / update, a clear, barriers only) */
        static const char *const cls[6] = {"across a submission", "image copies / blits", "buffer copies / fills", "clears", "barriers only", "nothing recorded"};
        static const char *const evn[PPE_N] = {"barriers", "buffer copies", "image copies", "buffer-to-image", "image-to-buffer", "blits", "fills", "clears", "buffer updates", "submissions"};
        double gms[6] = {0}; uint32_t gn[6] = {0}, gev[6][PPE_N]; memset(gev, 0, sizeof gev);
        struct { double ms; int i, c; } top[6]; int ntop = 0;
        uint64_t *tt = malloc((size_t)PP.nq * 8);
        if (tt && !vkGetQueryPoolResults(V.dev, PP.qp, 0, PP.nq, (size_t)PP.nq * 8, tt, 8, VK_QUERY_RESULT_64_BIT)) {
            for (int i = 1; i < PP.nrec; i++) {
                const pp_rec *e = &PP.rec[i], *b = &PP.rec[i - 1];
                double ms = tt[e->q0] > tt[b->q0 + 1] ? (double)(tt[e->q0] - tt[b->q0 + 1]) * tick : 0;
                int c = e->ev[PPE_SUBMIT] ? 0 : (e->ev[PPE_COPYIMG] | e->ev[PPE_B2I] | e->ev[PPE_I2B] | e->ev[PPE_BLIT]) ? 1 :
                        (e->ev[PPE_COPYBUF] | e->ev[PPE_FILL] | e->ev[PPE_UPDATE]) ? 2 : e->ev[PPE_CLEAR] ? 3 : e->ev[PPE_BARRIER] ? 4 : 5;
                gms[c] += ms; gn[c]++;
                for (int k = 0; k < PPE_N; k++) gev[c][k] += e->ev[k];
                int at = ntop < 6 ? ntop++ : -1;
                if (at < 0) { at = 0; for (int k = 1; k < 6; k++) if (top[k].ms < top[at].ms) at = k; if (top[at].ms >= ms) at = -1; }
                if (at >= 0) { top[at].ms = ms; top[at].i = i; top[at].c = c; }
            }
            {   static int seq = -1; if (seq < 0) { const char *e = getenv("TOTK_VK_PASS_PROFILE_SEQ"); seq = e ? atoi(e) : 0; if (seq < 0 || seq > 2) seq = 0; }   /* 2 (oot-a106): every profiled heavy frame */
                static int nseq;
                if (seq && PP.nrec > 250 && (seq == 2 || nseq++ < 3)) {   /* the frame in order: P pass WxH colour-format/depth-format draws ms, D dispatch ms, N native operation */
                    char line[900]; size_t o = 0;
                    for (int i = 0; i < PP.nrec; i++) {
                        const pp_rec *e = &PP.rec[i]; double ms = tt[e->q0 + 1] > tt[e->q0] ? (double)(tt[e->q0 + 1] - tt[e->q0]) * tick : 0;
                        if (e->kind == 0) o += (size_t)snprintf(line + o, sizeof line - o, " P%ux%u,%d/%d,%u,%.2f", e->w, e->h, (int)e->f0, (int)e->df, e->draws, ms);
                        else o += (size_t)snprintf(line + o, sizeof line - o, " %c%.2f", e->kind == 1 ? 'D' : 'N', ms);
                        if (o > sizeof line - 60 || i == PP.nrec - 1) { LOGV("pass profile frame %llu seq:%s", (unsigned long long)PP.frame, line); o = 0; }
                    }
                    /* oot-a117: the frame by submission (a submission starts at a scope with a submit recorded before it):
                       GPU ms of its scopes, scope count, and its costliest scope; TOTK_FRAME_LAG_SUBS=m lets the last m run
                       on while the game builds the next frame */
                    double sms = 0, big = -1; unsigned sn = 0, nsub = 0; const pp_rec *bb = NULL; o = 0;
                    for (int i = 0; i <= PP.nrec; i++) {
                        const pp_rec *e = i < PP.nrec ? &PP.rec[i] : NULL;
                        if (i == PP.nrec || (i > 0 && e->ev[PPE_SUBMIT])) {
                            if (sn) {
                                nsub++;
                                if (bb && bb->kind == 0) o += (size_t)snprintf(line + o, sizeof line - o, " #%u %.2f/%u [P%ux%u,%u %.2f]", nsub, sms, sn, bb->w, bb->h, bb->draws, big);
                                else o += (size_t)snprintf(line + o, sizeof line - o, " #%u %.2f/%u [%c %.2f]", nsub, sms, sn, bb && bb->kind == 1 ? 'D' : 'N', big);
                                if (o > sizeof line - 80) { LOGV("pass profile frame %llu submissions (GPU ms/scopes [costliest]):%s", (unsigned long long)PP.frame, line); o = 0; }
                            }
                            sms = 0; sn = 0; big = -1; bb = NULL;
                            if (i == PP.nrec) break;
                        }
                        double ms = tt[e->q0 + 1] > tt[e->q0] ? (double)(tt[e->q0 + 1] - tt[e->q0]) * tick : 0;
                        sms += ms; sn++; if (ms > big) { big = ms; bb = e; }
                    }
                    if (o) LOGV("pass profile frame %llu submissions (GPU ms/scopes [costliest]):%s; %u in all", (unsigned long long)PP.frame, line, nsub);
                }
            }
            for (int c = 0; c < 6; c++) if (gn[c]) {
                char ev[256]; size_t o = 0; ev[0] = 0;
                for (int k = 0; k < PPE_N; k++) if (gev[c][k]) o += (size_t)snprintf(ev + o, sizeof ev - o, "%s%u %s", o ? ", " : "", gev[c][k], evn[k]);
                LOGV("pass profile frame %llu: between scopes, %s: %u gaps, %.2f ms (%s)", (unsigned long long)PP.frame, cls[c], gn[c], gms[c], o ? ev : "no commands of the counted kinds");
            }
            for (int n = 0; n < ntop; n++) {
                const pp_rec *e = &PP.rec[top[n].i], *b = &PP.rec[top[n].i - 1];
                LOGV("pass profile frame %llu: a gap of %.2f ms (%s) after a %s (%ux%u, %u draws) before a %s (%ux%u): %u barriers, %u buffer copies, %u image copies, %u fills, %u clears",
                     (unsigned long long)PP.frame, top[n].ms, cls[top[n].c], b->kind == 0 ? "pass" : b->kind == 1 ? "dispatch" : "native operation", b->w, b->h, b->draws,
                     e->kind == 0 ? "pass" : e->kind == 1 ? "dispatch" : "native operation", e->w, e->h, e->ev[PPE_BARRIER], e->ev[PPE_COPYBUF],
                     e->ev[PPE_COPYIMG] + e->ev[PPE_B2I] + e->ev[PPE_I2B] + e->ev[PPE_BLIT], e->ev[PPE_FILL], e->ev[PPE_CLEAR]);
            }
        }
        free(tt);
    }
#endif
    for (unsigned op=0;op<VK_NUI_OPS;op++) if (native_n[op])
        LOGV("pass profile frame %llu: native UI %s: %u operations, %.3f ms%s",
            (unsigned long long)PP.frame,native_op_name[op],native_n[op],native_ms[op],
            op==VK_NUI_UI_PASS || op==VK_NUI_WINDOW_PASS ? " (also included in render passes)" : "");
    for (int n = 0; n < 16 && n < nds; n++) {   /* selection by time, the 16 costliest */
        int best = n; for (int k = n + 1; k < nds; k++) if (ds[k].ms > ds[best].ms) best = k;
        __typeof__(ds[0]) tmp = ds[n]; ds[n] = ds[best]; ds[best] = tmp;
        LOGV("pass profile frame %llu: dispatches of %.16s (%s, %u KB of SPIR-V, %u vertices x %u words, %u primitives): %u, %.2f ms, about %llu workgroups (%.2f us each)",
             (unsigned long long)PP.frame, ds[n].sh->key, ds[n].sh->mesh_stage == 6 ? "task kernel" : ds[n].sh->mesh_stage == 7 ? "mesh kernel" : "compute",
             (unsigned)(ds[n].sh->spv_bytes >> 10), ds[n].sh->mesh_max_v, ds[n].sh->mesh_ncomp, ds[n].sh->mesh_max_p, ds[n].n, ds[n].ms,
             (unsigned long long)ds[n].ran, ds[n].ran ? ds[n].ms * 1000.0 / (double)ds[n].ran : 0.0);
    }
    if (ndisp)
        LOGV("pass profile frame %llu: dispatches by kind: mesh kernels %u, %.2f ms; task kernels %u, %.2f ms; compute %u, %.2f ms; by duration: under 0.05 ms %u, %.2f ms; "
             "0.05 - 0.1: %u, %.2f ms; 0.1 - 0.2: %u, %.2f ms; 0.2 - 0.5: %u, %.2f ms; 0.5 - 1: %u, %.2f ms; over 1 ms: %u, %.2f ms; %d shaders",
             (unsigned long long)PP.frame, kind_n[0], kind_ms[0], kind_n[1], kind_ms[1], kind_n[2], kind_ms[2], size_n[0], size_ms[0], size_n[1], size_ms[1],
             size_n[2], size_ms[2], size_n[3], size_ms[3], size_n[4], size_ms[4], size_n[5], size_ms[5], nds);
    for (int k = 0; k < ng && k < 16; k++)
        LOGV("pass profile frame %llu: %ux%u color %u (fmt %d) depth fmt %d: %u passes, %u draws, %.2f ms, viewport up to %ux%u",
             (unsigned long long)PP.frame, g[k].w, g[k].h, g[k].ncolor, (int)g[k].f0, (int)g[k].df, g[k].n, g[k].draws, g[k].ms, g[k].vpw, g[k].vph);
}
/* At each present (the game's frame boundary), with the recording lock held. */
static void pp_present(uint64_t n) {
    if (PP.every < 0) { const char *e = getenv("TOTK_VK_PASS_PROFILE"); PP.every = e && *e ? atol(e) : 0; if (PP.every > 0) LOGV("pass profile every %ld frames (TOTK_VK_PASS_PROFILE)", PP.every); }
    if (PP.every <= 0) return;
    if (PP.pending && mtl_wait(PP.fence, 0)) pp_report();
    if (PP.active) {   /* the profiled frame ends here */
        vk_end_rendering_at(VK_PASS_END_DIAGNOSTIC, 1); pp_ts(vk_cb(), 1);
        PP.active = 0; PP.fence = vk_submit_now(); PP.pending = 1;
    } else if (!PP.pending && n % (uint64_t)PP.every == 0) {
        if (!PP.qp) { VkQueryPoolCreateInfo qi = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = PP_Q}; VKCHECK(vkCreateQueryPool(V.dev, &qi, NULL, &PP.qp)); }
        vk_end_rendering_at(VK_PASS_END_DIAGNOSTIC, 1);
        VkCommandBuffer cb = vk_cb(); vkCmdResetQueryPool(cb, PP.qp, 0, PP_Q); pp_ts(cb, 0);
        PP.nq = 2; PP.nrec = 0; PP.dropped = 0; PP.open = -1; PP.active = 1; PP.frame = n;
    }
}
/* ---------- pass dump (TOTK_VK_PASS_DUMP=<frame>[-<last frame>][,<min width>], off by default) ----------
   For finding the pass that first draws something wrong: on that presented frame (or each frame of the range, passes
   numbered on across them, index.txt split by "== frame" lines) every pass at least <min width> (default
   640; a huge width keeps only the log) wide has its colour and depth attachments copied out when it ends. After the frame
   they are written to <TOTK_VK_SHOT_DIR>/passdump/p<pass>_<c<i>|d>_<VkFormat>_<w>x<h>.raw (tightly packed texels of the
   attachment's level and layer; the depth aspect of depth/stencil formats). index.txt lists, in recording order, every
   pass (size, attachments' guest memory, which were cleared and with what), every draw (state, vertex attributes and
   streams, textures of both stages, storage buffers), dispatch, colour clear, render condition (with the counter values
   it compared), counter report and small buffer write, and the frame's storage buffers are saved as sbuf_<address>.bin.
   android/passdump_png.py turns the attachments into PNGs. Further switches for the dumped frame:
     TOTK_VK_PASS_DUMP_SPLIT=<VkFormat of colour 0>[,<width>]  each draw of such passes ends its pass (dumped on its own)
     TOTK_VK_PASS_DUMP_BUF=<guest address>,<size>               that range copied on the GPU after every pass and dispatch
     TOTK_VK_PASS_DUMP_VSTEX=<vertex shader key prefix>         the first such draw's vertex textures and bound uniforms */
typedef struct { VkBuffer buf; vk_alloc mem; size_t n; char name[80]; } pd_ent;
static struct { long frame, last, minw; int active, npass; pd_ent *e; int ne, cap; uint64_t draws0; char *index; size_t nindex; } PD = {.frame = -2};
static void pd_note(const char *line, int n) {   /* a line of index.txt */
    if (n <= 0) return;
    PD.index = realloc(PD.index, PD.nindex + (size_t)n + 1); memcpy(PD.index + PD.nindex, line, (size_t)n + 1); PD.nindex += (size_t)n;
}
/* The vertex stage's uniform ranges as bound (renamed / snapshot copies included), recorded for TOTK_VK_PASS_DUMP_VSTEX's
   draw so the dump has the exact bytes its vertex shader read. */
static struct { int binding, kind, reg; VkDescriptorBufferInfo b; } g_pd_ubo[32]; static int g_pd_nubo, g_pd_cap, g_pd_vstex_done;
static uint32_t pd_texel(VkFormat f) {   /* bytes per texel of a copy (depth aspect for depth/stencil), 0: not dumped */
    switch (f) {
        case VK_FORMAT_R8_UNORM: case VK_FORMAT_R8_UINT: case VK_FORMAT_S8_UINT: return 1;
        case VK_FORMAT_R8G8_UNORM: case VK_FORMAT_R16_SFLOAT: case VK_FORMAT_R16_UNORM: case VK_FORMAT_R16_UINT: case VK_FORMAT_D16_UNORM: return 2;
        case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_SRGB: case VK_FORMAT_B8G8R8A8_UNORM: case VK_FORMAT_B8G8R8A8_SRGB: case VK_FORMAT_R8G8B8A8_UINT:
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32: case VK_FORMAT_B10G11R11_UFLOAT_PACK32: case VK_FORMAT_R16G16_SFLOAT: case VK_FORMAT_R16G16_UNORM:
        case VK_FORMAT_R32_SFLOAT: case VK_FORMAT_R32_UINT: case VK_FORMAT_D32_SFLOAT: case VK_FORMAT_D32_SFLOAT_S8_UINT: case VK_FORMAT_D24_UNORM_S8_UINT:
        case VK_FORMAT_X8_D24_UNORM_PACK32: return 4;
        case VK_FORMAT_R16G16B16A16_SFLOAT: case VK_FORMAT_R16G16B16A16_UNORM: case VK_FORMAT_R32G32_SFLOAT: return 8;
        case VK_FORMAT_R32G32B32A32_SFLOAT: case VK_FORMAT_R32G32B32A32_UINT: return 16;
        default: return 0;
    }
}
static void pd_copy(VkCommandBuffer cb, vk_tex *x, unsigned level, unsigned layer, int depth, const char *tag) {
    if (!R.open) vk_tex_native_read(x);
    VkFormat f = vk_tex_format(x); uint32_t bpt = pd_texel(f), w = vk_tex_width(x, level), h = vk_tex_height(x, level);
    if (!bpt) return;
    uint32_t dz = 1;
    if (vk_tex_is3d(x)) {   /* a volume (Z-A's 32x32x32 colour grading LUT): every slice, while it stays small */
        extern uint32_t vk_tex_depth(vk_tex *x, unsigned l);
        dz = vk_tex_depth(x, level); if (!dz || dz > 64) return;
    }
    pd_ent e = {.n = (size_t)w * h * dz * bpt};
    if (dz > 1) snprintf(e.name, sizeof e.name, "p%04d_%s_%d_%ux%ux%u.raw", PD.npass, tag, (int)f, w, h, dz);
    else snprintf(e.name, sizeof e.name, "p%04d_%s_%d_%ux%u.raw", PD.npass, tag, (int)f, w, h);
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = e.n, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VKCHECK(vkCreateBuffer(V.dev, &bci, NULL, &e.buf));
    VkMemoryRequirements req; vkGetBufferMemoryRequirements(V.dev, e.buf, &req);
    if (!vk_mem_alloc(&req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &e.mem)) { vkDestroyBuffer(V.dev, e.buf, NULL); LOGV("pass dump: no memory for %s", e.name); return; }
    VKCHECK(vkBindBufferMemory(V.dev, e.buf, e.mem.mem, e.mem.off));
    VkBufferImageCopy r = {0, 0, 0, {depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT, level, dz > 1 ? 0 : layer, 1}, {0, 0, 0}, {w, h, dz}};
    if (vk_gpuprof_on) vk_gpuprof_scope_begin(cb, "transfer");
    vkCmdCopyImageToBuffer(cb, vk_tex_image(x), VK_IMAGE_LAYOUT_GENERAL, e.buf, 1, &r);
    if (vk_gpuprof_on) vk_gpuprof_scope_end(cb);
    if (PD.ne == PD.cap) { PD.cap = PD.cap ? 2 * PD.cap : 64; PD.e = realloc(PD.e, (size_t)PD.cap * sizeof *PD.e); }
    PD.e[PD.ne++] = e;
}
static void pd_copy_range(VkCommandBuffer cb, VkBuffer src, uint64_t so, uint64_t size, const char *name) {   /* a GPU copy of a buffer range, written after the frame */
    pd_ent e = {.n = (size_t)size};
    snprintf(e.name, sizeof e.name, "%s", name);
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VKCHECK(vkCreateBuffer(V.dev, &bci, NULL, &e.buf));
    VkMemoryRequirements req; vkGetBufferMemoryRequirements(V.dev, e.buf, &req);
    if (!vk_mem_alloc(&req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &e.mem)) { vkDestroyBuffer(V.dev, e.buf, NULL); return; }
    VKCHECK(vkBindBufferMemory(V.dev, e.buf, e.mem.mem, e.mem.off));
    if (vk_gpuprof_on) vk_gpuprof_scope_begin(cb, "transfer");
    VkBufferCopy r = {so, 0, size}; vkCmdCopyBuffer(cb, src, e.buf, 1, &r);
    if (vk_gpuprof_on) vk_gpuprof_scope_end(cb);
    if (PD.ne == PD.cap) { PD.cap = PD.cap ? 2 * PD.cap : 64; PD.e = realloc(PD.e, (size_t)PD.cap * sizeof *PD.e); }
    PD.e[PD.ne++] = e;
}
/* TOTK_VK_PASS_DUMP_BUF=<guest address>,<size>: in the dumped frame that range is copied on the GPU after every pass and
   every dispatch (buf_<n>_<p|d><index>.bin, in order), to follow what writes it. */
static void pd_buf(VkCommandBuffer cb, char what, int index) {
    static uint64_t addr = 1, size; if (addr == 1) { const char *e = getenv("TOTK_VK_PASS_DUMP_BUF"); addr = e ? strtoull(e, NULL, 0) : 0; const char *c = e ? strchr(e, ',') : NULL; size = c ? strtoull(c + 1, NULL, 0) : 4096; }
    static int seq;
    if (!PD.active || !addr) return;
    VkBuffer src; uint64_t so; if (!vk_resolve(addr, size, &src, &so)) return;
    char name[80]; snprintf(name, sizeof name, "buf_%04d_%c%04d.bin", seq++, what, index);
    pd_copy_range(cb, src, so, size, name);
    vk_barrier_all(cb);
}
static void pd_pass_end(VkCommandBuffer cb) {   /* after vkCmdEndRendering and its barrier */
    if (!PD.active) return;
    if (R.w >= PD.minw) {
        char tag[8];
        for (uint32_t i = 0; i < R.ncolor; i++) if (R.ctex[i]) { snprintf(tag, sizeof tag, "c%u", i); pd_copy(cb, R.ctex[i], R.clevel[i], R.clayer[i], 0, tag); }
        if (R.dtex && (vk_tex_aspect(R.dtex) & VK_IMAGE_ASPECT_DEPTH_BIT)) pd_copy(cb, R.dtex, R.dlevel, R.dlayer, 1, "d");
        vk_barrier_all(cb);
    }
    char line[512]; int n = snprintf(line, sizeof line, "p%04d %ux%u colour %u depth fmt %d draws %llu cleared %#x", PD.npass, R.w, R.h, R.ncolor, (int)R.dfmt, (unsigned long long)(g_stat_draws - PD.draws0), R.cleared);
    for (uint32_t i = 0; i < R.ncolor && i < 8 && n < (int)sizeof line - 40; i++) if (R.caddr[i]) n += snprintf(line + n, sizeof line - (size_t)n, " c%u@%p", i, R.caddr[i]);
    if (R.daddr && n < (int)sizeof line - 40) n += snprintf(line + n, sizeof line - (size_t)n, " d@%p", R.daddr);
    for (uint32_t i = 0; i < 8 && n < (int)sizeof line - 60; i++) if (R.cleared >> i & 1)   /* the clear value's raw words (and its float reading) */
        n += snprintf(line + n, sizeof line - (size_t)n, " clear%u=%08x,%08x,%08x,%08x(%g)", i, R.cc[i].color.uint32[0], R.cc[i].color.uint32[1], R.cc[i].color.uint32[2], R.cc[i].color.uint32[3], R.cc[i].color.float32[0]);
    if (n < (int)sizeof line - 1) { line[n++] = '\n'; line[n] = 0; }
    pd_note(line, n);
    pd_buf(cb, 'p', PD.npass);
    PD.npass++;
}
/* TOTK_VK_PASS_DUMP_SPLIT=<VkFormat of colour 0>[,<width>]: in the dumped frame each draw of such passes ends its pass, so
   every draw's result is dumped on its own, with the draw's state in index.txt above its pass line. The first draw of such
   a pass finds colour 0 filled with magenta, so what it leaves magenta it did not write (and nothing earlier did). */
static int pd_split(void) {
    if (!PD.active) return 0;
    static long fmt = -2, width; if (fmt == -2) { const char *e = getenv("TOTK_VK_PASS_DUMP_SPLIT"); fmt = e && *e ? atol(e) : -1; const char *c = e ? strchr(e, ',') : NULL; width = c ? atol(c + 1) : 0; }
    return fmt >= 0 && (long)R.cfmt[0] == fmt && (!width || (long)R.w == width);
}
static int g_pd_split_seen;   /* a split pass has had its first draw this frame */
static void pd_pre_draw(VkCommandBuffer cb) {
    if (!pd_split() || g_pd_split_seen) return;
    g_pd_split_seen = 1;
    VkClearAttachment ca = {VK_IMAGE_ASPECT_COLOR_BIT, 0, {.color.float32 = {1, 0, 1, 1}}};
    VkClearRect cr = {{{0, 0}, {R.w, R.h}}, 0, 1};
    vkCmdClearAttachments(cb, 1, &ca, 1, &cr);
}
/* Storage buffers a stage binds (as draw_touch resolves them): noted in index.txt, and their bytes (up to 64 KiB each) are
   written as sbuf_<address>.bin after the frame, to see what a draw or dispatch read. */
enum { PD_NSB = 1024 };
static struct { uint64_t addr, size; } g_pd_sb[PD_NSB]; static int g_pd_nsb;
static void pd_sb_add(uint64_t addr, uint64_t size) {   /* a guest range written as sbuf_<address>.bin after the frame (up to 64 KiB) */
    if (!addr) return;
    int k = 0; while (k < g_pd_nsb && g_pd_sb[k].addr != addr) k++;
    if (k == g_pd_nsb) { if (g_pd_nsb == PD_NSB) return; g_pd_sb[g_pd_nsb++].addr = addr; }
    g_pd_sb[k].size = MAXV(g_pd_sb[k].size, MINV(size ? size : 65536, 65536));
}
static int pd_sbufs(int st, const vk_shader *s, char *out, size_t cap) {
    int n = 0;
    for (int i = 0; s && i < s->nb && (size_t)n < cap; i++) {
        const vk_bind *b = &s->b[i];
        if (b->dtype != VK_DESCRIPTOR_TYPE_STORAGE_BUFFER || b->reg < 0 || b->reg >= 16) continue;
        uint64_t addr = 0, size = 0;
        if (b->kind == RB_SBUF) { addr = S.ssbo[st][b->reg].addr; size = S.ssbo[st][b->reg].size; }
        else if (b->kind == RB_CBUF || b->kind == RB_UBO_PTR) { addr = S.ubo[st][b->reg].addr; size = S.ubo[st][b->reg].size; }
        else if (b->kind == RB_SBUF_UBO) { const p_bind_buf *u = &S.ubo[st][b->reg]; if (u->addr && u->size >= (uint64_t)b->off + 8) vk_ub_read(u->addr + b->off, &addr, 8); }
        n += snprintf(out + n, cap - (size_t)n, " s%d.%d@%#llx+%#llx", st, b->reg, (unsigned long long)addr, (unsigned long long)size);
        pd_sb_add(addr, size);
    }
    return n;
}
static void pd_draw(int prim, uint32_t count, uint32_t inst, int indexed, vk_shader *const *sh) {   /* every draw of the dumped frame */
    if (!PD.active) return;
    char line[2600], sb[2048] = "";
    int ns = pd_sbufs(0, sh[0], sb, sizeof sb); if (sh[1] && (size_t)ns < sizeof sb) ns += pd_sbufs(1, sh[1], sb + ns, sizeof sb - (size_t)ns);
    for (int a = 0; a < 16 && (size_t)ns < sizeof sb; a++) if ((sh[0]->vin_mask >> a & 1) && a < S.nattr && S.attr[a].fmt) {   /* vertex attributes: NVN format, stream, offset, stride */
        int st = S.attr[a].stream & 15;
        ns += snprintf(sb + ns, sizeof sb - (size_t)ns, " a%d=%#x/s%d+%u/%u", a, (unsigned)S.attr[a].fmt, st, (unsigned)S.attr[a].offset, (unsigned)S.stream[st].stride);
        VkBuffer vb_; uint64_t vo_, avail_ = 0; int res = S.vbuf[st].address && vk_resolve_span(S.vbuf[st].address, &vb_, &vo_, &avail_);
        if ((size_t)ns < sizeof sb) ns += snprintf(sb + ns, sizeof sb - (size_t)ns, "@%#llx+%#llx(pool %d, avail %#llx)", (unsigned long long)S.vbuf[st].address, (unsigned long long)S.vbuf[st].size, res, (unsigned long long)avail_);
    }
#ifdef TOTK_OHOS
    if (sh[0]->tf) {   /* a transform feedback capture: its vertex streams and vertex uniforms are saved too, to recompute it offline */
        for (int a = 0; a < 16; a++) if ((sh[0]->vin_mask >> a & 1) && a < S.nattr && S.attr[a].fmt) { int st = S.attr[a].stream & 15; pd_sb_add(S.vbuf[st].address, S.vbuf[st].size); }
        for (int i = 0; i < sh[0]->nb && (size_t)ns < sizeof sb; i++) {
            const vk_bind *b = &sh[0]->b[i];
            if (b->dtype != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || b->reg < 0 || b->reg >= 16) continue;
            pd_sb_add(S.ubo[0][b->reg].addr, S.ubo[0][b->reg].size);
            ns += snprintf(sb + ns, sizeof sb - (size_t)ns, " u0.%d@%#llx+%#llx", b->reg, (unsigned long long)S.ubo[0][b->reg].addr, (unsigned long long)S.ubo[0][b->reg].size);
        }
    }
#endif
    for (int st = 0; st < 2; st++) for (int i = 0; sh[st] && i < sh[st]->nb && (size_t)ns < sizeof sb; i++) {   /* textures: stage, slot, handle, and what it resolves to */
        const vk_bind *b = &sh[st]->b[i];
        if (b->dtype != VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER && b->dtype != VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE) continue;
        uint64_t h = b->reg >= 0 && b->reg < 128 ? S.tex[st][b->reg] : 0; vk_treg r = {0};
        int ok = h && vk_treg_get((int)(h & 0xfffff), &r);
        ns += snprintf(sb + ns, sizeof sb - (size_t)ns, " %ct%d=%#llx", st ? 'f' : 'v', b->reg, (unsigned long long)h);
        if (ok && (size_t)ns < sizeof sb) ns += snprintf(sb + ns, sizeof sb - (size_t)ns, "(fmt %#x %ux%ux%u target %d @%p)", r.t.d.format, r.t.d.width, r.t.d.height, r.t.d.depth, r.t.d.target, (const void *)r.t.mem);
    }
    int n = snprintf(line, sizeof line, "  draw prim %d count %u inst %u%s%.0d cull %d front %d depth test %d write %d func %d clamp %d fs depth %d blend %#x vp %.0f,%.0f %.0fx%.0f vs %s fs %s%s\n",
                     prim, count, inst, indexed ? " idx-bytes " : "", indexed == 1 ? 1 : indexed == 2 ? 2 : indexed ? 4 : 0, (int)S.poly.cull, (int)S.poly.front, S.ds.dt, S.ds.dw, (int)S.ds.df, S.depth_clamp, sh[1] ? sh[1]->writes_depth : 0,
                     (unsigned)S.color.blend_en, S.vp[0][0], S.vp[0][1], S.vp[0][2], S.vp[0][3], sh[0]->key, sh[1] ? sh[1]->key : "-", sb);
    pd_note(line, n < (int)sizeof line ? n : (int)sizeof line - 1);
    if (g_pd_cap) {   /* TOTK_VK_PASS_DUMP_VSTEX's draw: its vertex-stage textures (every layer of level 0) and its bound uniform ranges */
        g_pd_cap = 0; g_pd_vstex_done = 1; vk_end_rendering_at(VK_PASS_END_DIAGNOSTIC, 2); VkCommandBuffer cb = vk_cb();
        for (int i = 0; i < sh[0]->nb; i++) {
            const vk_bind *b = &sh[0]->b[i];
            if ((b->dtype != VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER && b->dtype != VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE) || b->reg < 0 || b->reg >= 128 || !S.tex[0][b->reg]) continue;
            int want = b->target; vk_tex *x = NULL; vk_tex_by_id((int)(S.tex[0][b->reg] & 0xfffff), &want, &x); if (!x) continue;
            uint32_t nl = vk_tex_layers(x); if (nl > 16) nl = 16;
            for (uint32_t l = 0; l < nl; l++) { char tag[24]; snprintf(tag, sizeof tag, "vt%d-%u", b->reg, l); pd_copy(cb, x, 0, l, 0, tag); }
        }
        for (int i = 0; i < g_pd_nubo; i++) {
            char name[80]; snprintf(name, sizeof name, "p%04d_ubo-binding%d-kind%d-reg%d.bin", PD.npass, g_pd_ubo[i].binding, g_pd_ubo[i].kind, g_pd_ubo[i].reg);
            if (g_pd_ubo[i].b.buffer && g_pd_ubo[i].b.range && g_pd_ubo[i].b.range != VK_WHOLE_SIZE) pd_copy_range(cb, g_pd_ubo[i].b.buffer, g_pd_ubo[i].b.offset, g_pd_ubo[i].b.range, name);
        }
        vk_barrier_all(cb);
    }
    if (pd_split()) vk_end_rendering_at(VK_PASS_END_DIAGNOSTIC, 2);
}
/* A dispatch's sampled textures (ct<slot>) and images (ci<slot>): the handle and what it resolves to. Without them the dump
   could not show where the scene goes after the last render pass (agl's upscale chain is all compute). */
static int pd_cs_res(const vk_shader *cs, char *out, size_t cap) {
    int ns = 0; out[0] = 0;
    for (int i = 0; i < cs->nb && (size_t)ns + 112 < cap; i++) {
        const vk_bind *b = &cs->b[i];
        int tex = b->dtype == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER || b->dtype == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, img = b->dtype == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        if ((!tex && !img) || b->reg < 0 || b->reg >= (tex ? 128 : 32)) continue;
        uint64_t h = tex ? S.tex[5][b->reg] : S.img[5][b->reg]; vk_treg r = {0};
        int ok = h && vk_treg_get((int)(h & 0xfffff), &r);
        ns += snprintf(out + ns, cap - (size_t)ns, " c%c%d=%#llx", tex ? 't' : 'i', b->reg, (unsigned long long)h);
        if (ok) ns += snprintf(out + ns, cap - (size_t)ns, "(fmt %#x %ux%ux%u @%p)", r.t.d.format, r.t.d.width, r.t.d.height, r.t.d.depth, (const void *)r.t.mem);
    }
    return ns;
}
static void pd_dispatch(const vk_shader *cs, const uint32_t grid[3]) {
    if (!PD.active) return;
    char line[1500], sb[384] = "", res[800] = ""; pd_sbufs(5, cs, sb, sizeof sb); pd_cs_res(cs, res, sizeof res);
    int n = snprintf(line, sizeof line, "  dispatch %ux%ux%u cs %s%s%s\n", grid[0], grid[1], grid[2], cs->key, sb, res);
    pd_note(line, n < (int)sizeof line ? n : (int)sizeof line - 1);
}
/* A dispatch that did not run, and why: a dump that lists only what ran hid that the last two steps of Ocarina of Time's
   picture (fa3033d0, d17eefaf) never did (pd-a62) */
static void pd_dispatch_skip(const vk_shader *cs, const char *why) {
    if (!PD.active) return;
    char line[1500], res[800] = ""; if (cs) pd_cs_res(cs, res, sizeof res);
    int n = snprintf(line, sizeof line, "  dispatch SKIPPED (%s) cs %s%s\n", why, cs ? cs->key : "-", res);
    pd_note(line, n < (int)sizeof line ? n : (int)sizeof line - 1);
}
/* An agl NDSR upscaler network by its interface (after Swap2-Native's vka_ndsr.c): a big compute module (tensor emulation)
   with exactly one sampled texture, one storage image, one storage buffer (its weights) and the push block, and nothing else
   but the support buffer, its own constants and the driver segments. The slots of its input texture and output image. */
static int ndsr_network(const vk_shader *cs, int *tex_reg, int *img_reg) {
    if (cs->spv_bytes < (200u << 10)) return 0;
    int nt = 0, ni = 0, nsb = 0, push = 0;
    for (int i = 0; i < cs->nb; i++) {
        const vk_bind *b = &cs->b[i];
        if (b->kind == RB_SUPPORT || b->kind == RB_C1 || b->kind == RB_DRV0 || b->kind == RB_DRV2) continue;
        if (b->kind == RB_PUSH) { push = 1; continue; }
        if (b->kind == RB_TEXSAMP && b->reg >= 0 && b->reg < 128) { nt++; *tex_reg = b->reg; continue; }
        if (b->kind == RB_IMG && b->reg >= 0 && b->reg < 32) { ni++; *img_reg = b->reg; continue; }
        if (b->kind == RB_SBUF) { nsb++; continue; }
        return 0;
    }
    return nt == 1 && ni == 1 && nsb == 1 && push;
}
static void pd_present(uint64_t n) {   /* at each present, with the recording lock held */
    if (PD.frame == -2) {
        const char *e = getenv("TOTK_VK_PASS_DUMP"); PD.frame = e && *e ? atol(e) : -1; PD.minw = 640;
        const char *d = e ? strchr(e, '-') : NULL, *c = e ? strchr(e, ',') : NULL;
        PD.last = d && (!c || d < c) ? atol(d + 1) : PD.frame; if (PD.last < PD.frame) PD.last = PD.frame;
        if (c) PD.minw = atol(c + 1);
        if (PD.frame >= 0) LOGV("pass dump of frames %ld-%ld, passes at least %ld wide (TOTK_VK_PASS_DUMP)", PD.frame, PD.last, PD.minw);
    }
    if (PD.frame < 0) return;
    if (PD.active && PD.frame < PD.last) {   /* a range: the next frame goes on in the same dump, its passes numbered on */
        PD.frame++; char l[48]; pd_note(l, snprintf(l, sizeof l, "== frame %ld\n", PD.frame));
    } else if (PD.active) {   /* the (last) dumped frame ends here */
        vk_end_rendering_at(VK_PASS_END_DIAGNOSTIC, 2); PD.active = 0;
        if (!mtl_wait(vk_submit_now(), 10000000000ull)) LOGV("pass dump: the GPU did not finish the frame");
        const char *dir = getenv("TOTK_VK_SHOT_DIR"); if (!dir || !*dir) dir = "shots";
        char path[1024]; snprintf(path, sizeof path, "%s/passdump", dir); mkdir(dir, 0755); mkdir(path, 0755);
        {   /* an earlier dump's files go first: its sbuf_*.bin of buffers this frame does not bind were pulled as this
               dump's (oot-a58 / a59 read a57's SH buffer), and the device shell may not delete the app's files */
            DIR *dd = opendir(path); struct dirent *de; char old[1024];
            while (dd && (de = readdir(dd))) if (de->d_name[0] != '.') { snprintf(old, sizeof old, "%s/%s", path, de->d_name); unlink(old); }
            if (dd) closedir(dd);
        }
        size_t bytes = 0;
        for (int i = 0; i < PD.ne; i++) {
            pd_ent *e = &PD.e[i]; void *p;
            snprintf(path, sizeof path, "%s/passdump/%s", dir, e->name);
            if (!vkMapMemory(V.dev, e->mem.mem, e->mem.off, e->n, 0, &p)) {
                FILE *f = fopen(path, "wb"); if (f) { fwrite(p, 1, e->n, f); fclose(f); bytes += e->n; }
                vkUnmapMemory(V.dev, e->mem.mem);
            }
            vkDestroyBuffer(V.dev, e->buf, NULL); vk_mem_free(&e->mem);
        }
        for (int i = 0; i < g_pd_nsb; i++) {   /* guest memory: the pools are mapped at the guest address */
            snprintf(path, sizeof path, "%s/passdump/sbuf_%llx.bin", dir, (unsigned long long)g_pd_sb[i].addr);
            VkBuffer b; uint64_t o;
            if (!vk_resolve(g_pd_sb[i].addr, g_pd_sb[i].size, &b, &o)) continue;
            FILE *f = fopen(path, "wb"); if (f) { fwrite((const void *)(uintptr_t)g_pd_sb[i].addr, 1, (size_t)g_pd_sb[i].size, f); fclose(f); }
        }
        g_pd_nsb = 0;
        snprintf(path, sizeof path, "%s/passdump/index.txt", dir);
        FILE *f = fopen(path, "w"); if (f) { if (PD.index) fputs(PD.index, f); fclose(f); }
        LOGV("pass dump of frame %ld: %d passes, %d attachments, %.1f MiB in %s/passdump", PD.frame, PD.npass, PD.ne, bytes / 1048576.0, dir);
        free(PD.e); PD.e = NULL; PD.ne = PD.cap = 0; free(PD.index); PD.index = NULL; PD.nindex = 0; PD.frame = -1;
    } else if (n + 1 == (uint64_t)PD.frame) {   /* the next frame is dumped */
        vk_end_rendering_at(VK_PASS_END_DIAGNOSTIC, 2);
        PD.active = 1; PD.npass = 0; g_pd_split_seen = 0;
        char l[48]; pd_note(l, snprintf(l, sizeof l, "== frame %ld\n", PD.frame));
    }
}
static int begin_rendering(int draw_tail);
int vk_in_pass(void) { return R.open; }
static void pd_pass(VkCommandBuffer cb);
static void pd_draw_note(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static int pd_split_match(const char *key);
static int pd_active(void);
static uint64_t g_samples_cum;   /* default synthetic SAMPLES_PASSED: Reset zeroes, each emitted draw adds */
uint64_t vk_counter_samples(void) {
    static int ordered = -1; if (ordered < 0) ordered = !vk_off("condorder");
    return ordered ? g_samples_cum : 1u << 20;
}
/* DEPTH16 EQUAL class markers land on rounding ties. Nudge rasterized AND fragment-written depth together:
   the raster-only experiment broke clothes, but the complete fix was confirmed on the Thor (DEBUGGING.md). */
#define D16_NUDGE_UNITS 0.2f
/* On by default (2026-10-22: the dark-water fix, confirmed on the Thor with fragment-written depth nudged too);
   TOTK_VK_D16TIE=0 or TOTK_VK_OFF=d16tie: off. */
static int d16tie_on(void) { static int on = -1; if (on < 0) { const char *e = getenv("TOTK_VK_D16TIE"); on = !(e && !atoi(e)) && !vk_off("d16tie"); } return on; }
static float d16_clear(float v) { double t = (double)v * 65535.0; return d16tie_on() && fabs(t - floor(t) - 0.5) < 1e-3 ? (float)(v + D16_NUDGE_UNITS / 65535.0) : v; }
/* what the open pass's draws read (pass probe report) and what could not be found: a texture id that resolves to nothing
   is bound as an all-zero dummy, a buffer outside the pools as zeros (both counted, and logged the first times) */
typedef struct { uint64_t img; uint32_t w, h; int fmt; vk_tex *x; } probe_in;
static probe_in g_pin[16]; static int g_npin, g_pdummy, g_pzero; static char g_pkey[24];
static uint64_t g_stat_dummy_tex, g_stat_zero_buf;
typedef struct { VkBuffer b; VkDeviceSize off, n; uint64_t addr; uint8_t stage, reg, storage; } probe_buf;
enum { PROBE_BUFS = 12, PROBE_BUF_BYTES = 256 };
static probe_buf g_pbuf[PROBE_BUFS]; static int g_npbuf; static uint8_t g_pbuf_data[PROBE_BUFS][PROBE_BUF_BYTES];
typedef struct { char vs[17]; int prim; uint32_t count, first, inst; float vp[4], dr[2], bias[3]; int clamp, cull, has_sc; int32_t sc[4]; float v0[8]; float c11_25[4], c4_147[4], c4_7[4]; } probe_draw;
static probe_draw g_pdraw[4]; static int g_npdraw;
typedef struct { int clears, clears_skipped, clears_inpass, setrt, cond, cond_off, disabled_at_start; float cc[4]; } probe_ops;   /* between the previous pass's end and this pass's draws */
static probe_ops g_pops;
static void pass_inputs_reset(void) { g_npin = 0; g_pdummy = g_pzero = 0; g_pkey[0] = 0; g_npbuf = 0; g_npdraw = 0; }
static void pass_buf_note(int stage, int reg, int storage, uint64_t addr, const VkDescriptorBufferInfo *d) {
    if (!R.open || stage > 1) return;
    for (int k = 0; k < g_npbuf; k++) if (g_pbuf[k].stage == stage && g_pbuf[k].reg == reg && g_pbuf[k].b == d->buffer && g_pbuf[k].off == d->offset) return;
    if (g_npbuf >= PROBE_BUFS || !addr) return;
    /* Uniform diagnostics use the ordered CPU-known updates/snapshot, not guest bytes that an earlier chunk's
       OP_UPDATE_UB may still be writing. Storage diagnostics retain their existing raw guest-memory semantics. */
    uint64_t n = MINV(d->range, (VkDeviceSize)PROBE_BUF_BYTES); VkBuffer pb; uint64_t po, avail;
    if (!vk_resolve_span(addr, &pb, &po, &avail)) return;
    n = MINV(n, avail);
    if (storage) memcpy(g_pbuf_data[g_npbuf], (const void *)(uintptr_t)addr, (size_t)n);
    else vk_ub_read(addr, g_pbuf_data[g_npbuf], (size_t)n);
    g_pbuf[g_npbuf++] = (probe_buf){d->buffer, d->offset, n, addr, (uint8_t)stage, (uint8_t)reg, (uint8_t)(storage | (vk_ub_pending(addr, n) ? 2 : 0))};
}
/* ---------- pass probe (TOTK_VK_PASSPROBE=1, diagnostic) ----------
   After every pass, colour target 0 is shrunk to 8x8 texels (blit, nearest) into a row of an RGBA16F atlas; the rows of
   the last PROBE_FRAMES frames are kept. When the flash detector (black_check) confirms a dark frame, the probes of that
   frame and the good frame after it (the same view, after a camera cut) are compared pass by pass and the passes whose output differs most are logged: the first
   pass that goes dark (or NaN) in the flash frame is where the picture is lost. */
enum { PROBE_FRAMES = 16, PROBE_SLOTS = 256 };
typedef struct { uint64_t serial, draws, img; uint32_t w, h; int fmt; probe_in in[16]; int nin, dummy, zero; char key[24]; probe_buf buf[12]; int nbuf; uint32_t dt, dw, df, st, sf, sref, svm, chmask, blend; probe_draw dr[4]; int ndr, cleared; uint64_t dimg; probe_ops ops; } probe_meta;
/* other writes into textures (depth passes, copies, clears, compute stores), in order: `at` is the number of colour
   passes probed before it, so "written before pass N" can be told */
enum { PROBE_WRITES = 384 };
typedef struct { uint64_t img, src; uint32_t w, h, sw, sh; int fmt, sfmt, at; char kind; uint32_t draws; int dsamp; VkFormat vf; probe_in in[16]; int nin; char key[24]; probe_draw d0; int nd; } probe_write;   /* dsamp: depth samples taken (D) */
typedef struct { _Atomic uint64_t frame; uint64_t fence; int n, nw; probe_meta m[PROBE_SLOTS]; probe_write wr[PROBE_WRITES]; } probe_frame;
static probe_frame g_probe[PROBE_FRAMES];
static VkImage g_probe_img; static vk_alloc g_probe_mem; static VkBuffer g_probe_buf; static VkDeviceMemory g_probe_bmem; static uint16_t *g_probe_ptr; static int g_probe_coherent;
#define PROBE_DEPTH_OFF ((VkDeviceSize)PROBE_FRAMES * PROBE_SLOTS * 64 * 8)   /* depth samples after the colour rows: 64 x 4 bytes per write */
#define PROBE_BUF_OFF (PROBE_DEPTH_OFF + (VkDeviceSize)PROBE_FRAMES * PROBE_WRITES * 64 * 4)   /* then the first 256 bytes of each pass's uniform / storage buffers */
static uint64_t g_probe_frame = 1; static int g_probe_n;
static int probe_on(void) { static int on = -1; if (on < 0) { const char *e = getenv("TOTK_VK_PASSPROBE"); on = e && atoi(e); if (on) LOGV("TOTK_VK_PASSPROBE: every pass's colour target is sampled (8x8) for the flash report"); } return on; }
static int probe_bufs_on(void) { return probe_on(); }   /* CPU-side reads: cheap (a GPU copy per buffer stalled the game for tens of seconds per frame) */
static int probe_init(void) {
    static int state; if (state) return state > 0; state = -1;
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R16G16B16A16_SFLOAT, .extent = {8, 8 * PROBE_SLOTS, 1}, .mipLevels = 1,
        .arrayLayers = PROBE_FRAMES, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
    if (vkCreateImage(V.dev, &ci, NULL, &g_probe_img)) return 0;
    VkMemoryRequirements req; vkGetImageMemoryRequirements(V.dev, g_probe_img, &req);
    if (!vk_mem_alloc(&req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &g_probe_mem)) return 0;
    vkBindImageMemory(V.dev, g_probe_img, g_probe_mem.mem, g_probe_mem.off);
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = PROBE_BUF_OFF + (VkDeviceSize)PROBE_FRAMES * PROBE_SLOTS * 12 * 256, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    if (vkCreateBuffer(V.dev, &bci, NULL, &g_probe_buf)) return 0;
    vkGetBufferMemoryRequirements(V.dev, g_probe_buf, &req);
    int t = vk_pick_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (t < 0) t = vk_pick_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size, .memoryTypeIndex = (uint32_t)t};
    if (t < 0 || vk_memory_allocate(&mai, &g_probe_bmem, VK_MEMORY_DIAGNOSTIC)) return 0;
    vkBindBufferMemory(V.dev, g_probe_buf, g_probe_bmem, 0);
    if (vkMapMemory(V.dev, g_probe_bmem, 0, VK_WHOLE_SIZE, 0, (void **)&g_probe_ptr)) return 0;
    g_probe_coherent = !!(V.mp.memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkImageMemoryBarrier2 ib = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_NONE, .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT, .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = g_probe_img, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, PROBE_FRAMES}};
    VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &ib};
    vkCmdPipelineBarrier2(vk_cb(), &di);
    state = 1; return 1;
}
static void probe_pass(VkCommandBuffer cb) {
    if (!probe_on() || !R.ctex[0] || g_probe_n >= PROBE_SLOTS || !probe_init()) return;
    vk_tex *x = R.ctex[0];
    vk_tex_native_read(x);
    if (vk_tex_is3d(x) || !(vk_tex_aspect(x) & VK_IMAGE_ASPECT_COLOR_BIT) || R.samples > 1) return;   /* multisampled images cannot be blitted */
    VkFormat f = vk_tex_format(x); VkFormatProperties fp; V.get_format_props(V.pd, f, &fp);
    if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT)) return;
    { int bw, bh; nvn_fmt_bpb(vk_tex_nvn_format(x), &bw, &bh); if (bw > 1 || bh > 1) return; }
    if ((f >= VK_FORMAT_R8_UINT && f <= VK_FORMAT_R8_SINT) || f == VK_FORMAT_R8G8_UINT || f == VK_FORMAT_R8G8_SINT || f == VK_FORMAT_R8G8B8A8_UINT || f == VK_FORMAT_R8G8B8A8_SINT ||
        f == VK_FORMAT_R16_UINT || f == VK_FORMAT_R16_SINT || f == VK_FORMAT_R16G16_UINT || f == VK_FORMAT_R16G16_SINT || f == VK_FORMAT_R16G16B16A16_UINT || f == VK_FORMAT_R16G16B16A16_SINT ||
        f == VK_FORMAT_R32_UINT || f == VK_FORMAT_R32_SINT || f == VK_FORMAT_R32G32_UINT || f == VK_FORMAT_R32G32_SINT || f == VK_FORMAT_R32G32B32A32_UINT || f == VK_FORMAT_R32G32B32A32_SINT ||
        f == VK_FORMAT_A2B10G10R10_UINT_PACK32) return;   /* integer formats cannot be blitted to float */
    if (R.c0_layer >= vk_tex_layers(x) || R.c0_level >= vk_tex_levels(x)) return;
    int slot = g_probe_n++, fr = (int)(g_probe_frame % PROBE_FRAMES);
    uint32_t w = vk_tex_width(x, R.c0_level), h = vk_tex_height(x, R.c0_level);
    VkImageBlit b = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, R.c0_level, R.c0_layer, 1}, .srcOffsets = {{0, 0, 0}, {(int32_t)w, (int32_t)h, 1}},
        .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, (uint32_t)fr, 1}, .dstOffsets = {{0, slot * 8, 0}, {8, slot * 8 + 8, 1}}};
    if (vk_gpuprof_on) vk_gpuprof_scope_begin(cb, "transfer");
    vkCmdBlitImage(cb, vk_tex_image(x), VK_IMAGE_LAYOUT_GENERAL, g_probe_img, VK_IMAGE_LAYOUT_GENERAL, 1, &b, VK_FILTER_NEAREST);
    vk_barrier_all(cb);
    if (vk_gpuprof_on) vk_gpuprof_scope_end(cb);
    probe_meta *pm = &g_probe[fr].m[slot];
    *pm = (probe_meta){.serial = R.c0_serial, .draws = g_stat_draws - R.draws0, .img = (uint64_t)(uintptr_t)vk_tex_image(x), .w = w, .h = h, .fmt = R.c0_fmt, .nin = g_npin, .dummy = g_pdummy, .zero = g_pzero};
    memcpy(pm->in, g_pin, sizeof g_pin); memcpy(pm->key, g_pkey, sizeof g_pkey);
    if (!probe_bufs_on() || g_stat_draws - R.draws0 > 8) g_npbuf = 0;   /* buffers: screen-space passes only */
    pm->nbuf = g_npbuf; memcpy(pm->buf, g_pbuf, sizeof g_pbuf);
    pm->ndr = g_npdraw; memcpy(pm->dr, g_pdraw, sizeof g_pdraw); pm->cleared = R.c0_cleared; pm->ops = g_pops; memset(&g_pops, 0, sizeof g_pops); pm->dimg = R.dtex ? (uint64_t)(uintptr_t)vk_tex_image(R.dtex) : 0;
    pm->dt = S.ds.dt; pm->dw = S.ds.dw; pm->df = S.ds.df; pm->st = S.ds.st; pm->sf = S.ds.sf_f; pm->sref = S.st_ref[0]; pm->svm = S.st_vmask[0]; pm->chmask = S.chmask.mask; pm->blend = S.color.blend_en;   /* as of its last draw */
    for (int k = 0; k < g_npbuf; k++)   /* into the probe buffer's host memory (no GPU work) */
        memcpy((uint8_t *)g_probe_ptr + PROBE_BUF_OFF + (((size_t)fr * PROBE_SLOTS + (size_t)slot) * 12 + (size_t)k) * 256, g_pbuf_data[k], (size_t)g_pbuf[k].n);
}
static void probe_frame_end(uint64_t frame_no) {   /* at present: copy this frame's rows out, start the next frame */
    if (!probe_on() || !g_probe_img) { g_probe_n = 0; return; }
    int fr = (int)(g_probe_frame % PROBE_FRAMES);
    probe_frame *p = &g_probe[fr];
    atomic_store(&p->frame, 0);
    if (g_probe_n) {
        VkCommandBuffer cb = vk_cb();
        VkBufferImageCopy c = {.bufferOffset = (VkDeviceSize)fr * PROBE_SLOTS * 64 * 8, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, (uint32_t)fr, 1}, .imageExtent = {8, 8 * (uint32_t)g_probe_n, 1}};
        if (vk_gpuprof_on) vk_gpuprof_scope_begin(cb, "transfer");
        vkCmdCopyImageToBuffer(cb, g_probe_img, VK_IMAGE_LAYOUT_GENERAL, g_probe_buf, 1, &c);
        vk_barrier_all(cb);
        if (vk_gpuprof_on) vk_gpuprof_scope_end(cb);
    }
    p->n = g_probe_n; p->fence = mtl_last_fence() + 1;
    atomic_store(&p->frame, frame_no);
    g_probe_frame++; g_probe_n = 0;
    probe_frame *q = &g_probe[g_probe_frame % PROBE_FRAMES]; atomic_store(&q->frame, 0); q->nw = 0;
}
static void probe_write_note(char kind, vk_tex *x, vk_tex *src, uint32_t draws) {   /* replay thread */
    if (!probe_on() || !x) return;
    probe_frame *p = &g_probe[g_probe_frame % PROBE_FRAMES];
    if (p->nw >= PROBE_WRITES) return;
    int wi = p->nw++;
    probe_write *w = &p->wr[wi];
    *w = (probe_write){(uint64_t)(uintptr_t)vk_tex_image(x), src ? (uint64_t)(uintptr_t)vk_tex_image(src) : 0, vk_tex_width(x, 0), vk_tex_height(x, 0),
        src ? vk_tex_width(src, 0) : 0, src ? vk_tex_height(src, 0) : 0, vk_tex_nvn_format(x), src ? vk_tex_nvn_format(src) : 0, g_probe_n, kind, draws};
    w->vf = vk_tex_format(x);
    if (kind == 'D') {   /* what the depth pass read, and its depth at 8x8 points */
        w->nin = g_npin; memcpy(w->in, g_pin, sizeof g_pin); memcpy(w->key, g_pkey, sizeof g_pkey); w->nd = g_npdraw; if (g_npdraw) w->d0 = g_pdraw[0];
        if (draws <= 8 && probe_init() && R.samples <= 1 && !vk_tex_is3d(x) && (vk_tex_aspect(x) & VK_IMAGE_ASPECT_DEPTH_BIT) && R.dlayer < vk_tex_layers(x) && R.dlevel < vk_tex_levels(x)) {
            uint32_t lw = vk_tex_width(x, R.dlevel), lh = vk_tex_height(x, R.dlevel);
            VkBufferImageCopy r[64]; VkDeviceSize base = PROBE_DEPTH_OFF + ((VkDeviceSize)(p - g_probe) * PROBE_WRITES + (VkDeviceSize)wi) * 64 * 4;
            for (int k = 0; k < 64; k++) r[k] = (VkBufferImageCopy){.bufferOffset = base + (VkDeviceSize)k * 4, .imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, R.dlevel, R.dlayer, 1},
                .imageOffset = {(int32_t)(lw * (2 * (k % 8) + 1) / 16), (int32_t)(lh * (2 * (k / 8) + 1) / 16), 0}, .imageExtent = {1, 1, 1}};
            VkCommandBuffer cb = vk_cb();
            if (vk_gpuprof_on) vk_gpuprof_scope_begin(cb, "transfer");
            vkCmdCopyImageToBuffer(cb, vk_tex_image(x), VK_IMAGE_LAYOUT_GENERAL, g_probe_buf, 64, r);
            vk_barrier_all(cb);
            if (vk_gpuprof_on) vk_gpuprof_scope_end(cb);
            w->dsamp = 1;
        }
    }
}
static uint32_t g_d16_lo, g_d16_hi;
static int depth_stats(const probe_frame *p, int wi, double *mn, double *mean, double *mx) {
    const probe_write *w = &p->wr[wi]; if (!w->dsamp) return 0;
    const uint8_t *q = (const uint8_t *)g_probe_ptr + PROBE_DEPTH_OFF + ((size_t)(p - g_probe) * PROBE_WRITES + (size_t)wi) * 64 * 4;
    double s = 0; *mn = 1e30; *mx = -1e30;
    for (int k = 0; k < 64; k++) {
        uint32_t u; memcpy(&u, q + k * 4, 4); double v;
        if (w->vf == VK_FORMAT_D16_UNORM || w->vf == VK_FORMAT_D16_UNORM_S8_UINT) { v = (u & 0xffff) / 65535.0; if (k == 0 || (u & 0xffff) < g_d16_lo) g_d16_lo = u & 0xffff; if (k == 0 || (u & 0xffff) > g_d16_hi) g_d16_hi = u & 0xffff; }
        else if (w->vf == VK_FORMAT_D32_SFLOAT || w->vf == VK_FORMAT_D32_SFLOAT_S8_UINT) { float f; memcpy(&f, &u, 4); v = f; }
        else v = (u & 0xffffff) / 16777215.0;
        s += v; if (v < *mn) *mn = v; if (v > *mx) *mx = v;
    }
    *mean = s / 64; return 1;
}
static float half_f(uint16_t h) {
    uint32_t s = (uint32_t)(h >> 15) << 31, e = (h >> 10) & 31, m = h & 1023, u;
    if (!e) { if (!m) u = s; else { e = 113; while (!(m & 1024)) { m <<= 1; e--; } m &= 1023; u = s | e << 23 | m << 13; } }
    else if (e == 31) u = s | 0x7f800000u | m << 13; else u = s | (e + 112) << 23 | m << 13;
    float f; memcpy(&f, &u, 4); return f;
}
static const probe_frame *probe_find(uint64_t frame_no) { for (int i = 0; i < PROBE_FRAMES; i++) if (atomic_load(&g_probe[i].frame) == frame_no) return &g_probe[i]; return NULL; }
static void probe_stats(const probe_frame *p, int slot, double *mean, int *nan) {
    const uint16_t *q = g_probe_ptr + ((size_t)(p - g_probe) * PROBE_SLOTS + (size_t)slot) * 64 * 4;
    double s = 0; int n = 0;
    for (int i = 0; i < 64; i++) { for (int c = 0; c < 3; c++) { float v = half_f(q[i * 4 + c]); if (v != v || v > 65504.f || v < -65504.f) n++; else s += v; } }
    *mean = s / 192; *nan = n;
}
static int probe_report_inner(uint64_t flash, uint64_t before) {   /* caller serializes report scratch */
    if (!probe_on() || !g_probe_ptr) return -1;
    const probe_frame *a = probe_find(flash), *b = probe_find(before);
    if (!a || !b) { LOGV("pass probe: frames %llu / %llu are no longer kept", (unsigned long long)flash, (unsigned long long)before); return -1; }
    mtl_wait(a->fence, 2000000000ull); mtl_wait(b->fence, 2000000000ull);
    if (!g_probe_coherent) { VkMappedMemoryRange r = {VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, .memory = g_probe_bmem, .size = VK_WHOLE_SIZE}; vkInvalidateMappedMemoryRanges(V.dev, 1, &r); }
    LOGV("pass probe, flash frame %llu (%d passes probed) vs the good frame %llu (%d): passes whose colour output differs (mean RGB, bad = NaN/inf texels of 192)",
         (unsigned long long)flash, a->n, (unsigned long long)before, b->n);
    /* the two frames need not have the same passes (shadow / cube faces take turns): pair them up by size and format
       (longest common subsequence), then compare the pairs */
    static uint16_t L[PROBE_SLOTS + 1][PROBE_SLOTS + 1];
    int na_ = a->n, nb_ = b->n;
    #define PSAME(i, j) (a->m[i].w == b->m[j].w && a->m[i].h == b->m[j].h && a->m[i].fmt == b->m[j].fmt)
    for (int i = 0; i <= na_; i++) L[i][nb_] = 0; for (int j = 0; j <= nb_; j++) L[na_][j] = 0;
    for (int i = na_ - 1; i >= 0; i--) for (int j = nb_ - 1; j >= 0; j--) L[i][j] = PSAME(i, j) ? (uint16_t)(L[i + 1][j + 1] + 1) : MAXV(L[i + 1][j], L[i][j + 1]);
    int shown = 0, paired = 0;
    for (int i = 0, j = 0; i < na_ && j < nb_; ) {
        if (PSAME(i, j)) {
            paired++;
            double ma, mb; int bad_a, bad_b; probe_stats(a, i, &ma, &bad_a); probe_stats(b, j, &mb, &bad_b);
            if ((bad_a != bad_b || fabs(ma - mb) > 0.1 * (fabs(ma) + fabs(mb)) + 0.01) && shown < 60) {
                shown++;
                const probe_meta *x = &a->m[i], *y = &b->m[j];
                LOGV("  pass %3d (good %3d): %ux%u fmt %#x, %llu draws (good %llu): mean %.4f bad %d, good %.4f bad %d",
                     i, j, x->w, x->h, x->fmt, (unsigned long long)x->draws, (unsigned long long)y->draws, ma, bad_a, mb, bad_b);
                for (int f = 0; f < 2; f++) {   /* what the pass read, in the flash frame and in the good one */
                    const probe_frame *pf = f ? b : a; const probe_meta *m = f ? y : x; int at = f ? j : i; const probe_write *dps[4]; int ndp = 0;
                    char line[2400]; size_t o = (size_t)snprintf(line, sizeof line, "      %s: fs %s, %d missing textures, %d buffers outside the pools; reads", f ? "good " : "flash", m->key[0] ? m->key : "?", m->dummy, m->zero);
                    for (int k = 0; k < m->nin && o + 80 < sizeof line; k++) {
                        char fb[160]; int from = -1; for (int q = at - 1; q >= 0; q--) if (pf->m[q].img == m->in[k].img) { from = q; break; }
                        const probe_write *wl = NULL; for (int q = pf->nw - 1; q >= 0; q--) if (pf->wr[q].img == m->in[k].img && pf->wr[q].at <= at && (from < 0 || pf->wr[q].at > from)) { wl = &pf->wr[q]; break; }
                        if (wl) snprintf(fb, sizeof fb, "%s before pass %d%s", wl->kind == 'D' ? "depth pass" : wl->kind == 'C' ? "copy" : wl->kind == 'K' ? "clear" : wl->kind == 'X' ? "image store" : "upload", wl->at,
                                         wl->kind == 'C' ? "" : "");
                        if (wl && wl->kind == 'C') snprintf(fb + strlen(fb), sizeof fb - strlen(fb), " from %ux%u %#x", wl->sw, wl->sh, wl->sfmt);
                        if (wl && wl->kind == 'D') { double dn, dm, dx; snprintf(fb + strlen(fb), sizeof fb - strlen(fb), ", %u draws", wl->draws);
                            if (depth_stats(pf, (int)(wl - pf->wr), &dn, &dm, &dx)) snprintf(fb + strlen(fb), sizeof fb - strlen(fb), ", depth %.4f..%.4f mean %.4f", dn, dx, dm);
                            if (ndp < 4) dps[ndp++] = wl; }
                        else if (from >= 0) snprintf(fb, sizeof fb, "from pass %d", from);
                        o += (size_t)snprintf(line + o, sizeof line - o, " [%ux%u %#x %s]", m->in[k].w, m->in[k].h, m->in[k].fmt, (wl || from >= 0) ? fb : "not written this frame");
                    }
                    if (!m->nin) snprintf(line + o, sizeof line - o, " no textures");
                    LOGV("%s", line);
                    LOGV("        state (last draw): depth test %u func %u write %u, stencil %u func %u ref %#x mask %#x, colour mask %#x, blend %#x",
                         m->dt, m->df, m->dw, m->st, m->sf, m->sref, m->svm, m->chmask, m->blend);
                    { int seen_c = 0, seen_d = 0, wrote_c = -1, wrote_d = -1; const probe_frame *o = f ? a : b;   /* the colour / depth images: also in the other frame? written earlier in this one? */
                      for (int q = 0; q < o->n; q++) { if (o->m[q].img == m->img) seen_c = 1; if (m->dimg && o->m[q].dimg == m->dimg) seen_d = 1; }
                      for (int q = 0; q < at; q++) { if (pf->m[q].img == m->img) wrote_c = q; if (m->dimg && pf->m[q].dimg == m->dimg) wrote_d = q; }
                      LOGV("        since the pass before: %d colour clears issued (%d skipped: render disabled, %d inside a pass), last clear colour %g %g %g %g; %d SetRenderTargets, %d conditional-render ops (%d disabled); render %s at pass start",
                           m->ops.clears, m->ops.clears_skipped, m->ops.clears_inpass, m->ops.cc[0], m->ops.cc[1], m->ops.cc[2], m->ops.cc[3], m->ops.setrt, m->ops.cond, m->ops.cond_off, m->ops.disabled_at_start ? "DISABLED" : "enabled");
                      LOGV("        colour target %#llx: %s, %s the other frame, last drawn earlier this frame at pass %d; depth target %#llx: %s the other frame, used earlier at pass %d",
                           (unsigned long long)m->img, m->cleared ? "cleared at the pass start" : "loaded (not cleared)", seen_c ? "also in" : "NOT in", wrote_c,
                           (unsigned long long)m->dimg, seen_d ? "also in" : "NOT in", wrote_d); }
                    for (int q = 0; q < m->ndr; q++) { const probe_draw *d = &m->dr[q];
                        LOGV("        draw %d: vs %s prim %d count %u first %u inst %u, viewport %g %g %g %g, depth range %g..%g, bias %g/%g on %g, clamp %d, cull %d, scissor %s%d %d %d %d, vertex 0: %g %g %g %g %g %g %g %g",
                             q, d->vs, d->prim, d->count, d->first, d->inst, d->vp[0], d->vp[1], d->vp[2], d->vp[3], d->dr[0], d->dr[1], d->bias[0], d->bias[1], d->bias[2], d->clamp, d->cull,
                             d->has_sc ? "" : "(off) ", d->sc[0], d->sc[1], d->sc[2], d->sc[3], d->v0[0], d->v0[1], d->v0[2], d->v0[3], d->v0[4], d->v0[5], d->v0[6], d->v0[7]);
                        LOGV("          vs slot 8 [25] = %g %g %g %g, slot 1 [147] = %g %g %g %g, slot 1 [7] = %g %g %g %g", d->c11_25[0], d->c11_25[1], d->c11_25[2], d->c11_25[3],
                             d->c4_147[0], d->c4_147[1], d->c4_147[2], d->c4_147[3], d->c4_7[0], d->c4_7[1], d->c4_7[2], d->c4_7[3]); }
                    for (int q = 0; q < pf->nw; q++) if (pf->wr[q].at == at) {   /* depth passes (its own depth attachment too), copies and clears since the colour pass before */
                        const probe_write *w = &pf->wr[q]; double dn, dm, dx; char ds[80] = "";
                        if (w->kind == 'D' && depth_stats(pf, q, &dn, &dm, &dx)) { if (w->vf == VK_FORMAT_D16_UNORM) snprintf(ds, sizeof ds, ", depth %u..%u of 65535", g_d16_lo, g_d16_hi); else snprintf(ds, sizeof ds, ", depth %.4f..%.4f mean %.4f", dn, dx, dm); }
                        if (w->kind == 'D' && w->nd) LOGV("          its first draw: vs %s, slot 8 [25] = %g %g %g %g, slot 1 [147] = %g %g %g %g", w->d0.vs, w->d0.c11_25[0], w->d0.c11_25[1], w->d0.c11_25[2], w->d0.c11_25[3], w->d0.c4_147[0], w->d0.c4_147[1], w->d0.c4_147[2], w->d0.c4_147[3]);
                        LOGV("        just before / with it: %s %ux%u %#x%s%s", w->kind == 'D' ? "depth pass on" : w->kind == 'C' ? "copy into" : w->kind == 'K' ? "clear of" : w->kind == 'X' ? "image store to" : "upload to",
                             w->w, w->h, w->fmt, w->kind == 'D' ? (snprintf(ds + strlen(ds), sizeof ds - strlen(ds), ", %u draws", w->draws), ds) : "", "");
                    }
                    if (f == 1) {   /* the buffers both frames' passes read, side by side, where their bytes differ */
                        for (int k = 0; k < x->nbuf; k++) {
                            const probe_buf *ba = &x->buf[k]; const probe_buf *bb = NULL; int kb = -1;
                            for (int q = 0; q < y->nbuf; q++) if (y->buf[q].stage == ba->stage && y->buf[q].reg == ba->reg) { bb = &y->buf[q]; kb = q; break; }
                            const uint8_t *pa = (const uint8_t *)g_probe_ptr + PROBE_BUF_OFF + (((size_t)(a - g_probe) * PROBE_SLOTS + (size_t)i) * 12 + (size_t)k) * 256;
                            const uint8_t *pb = bb ? (const uint8_t *)g_probe_ptr + PROBE_BUF_OFF + (((size_t)(b - g_probe) * PROBE_SLOTS + (size_t)j) * 12 + (size_t)kb) * 256 : NULL;
                            size_t nn = bb ? MINV(ba->n, bb->n) : 0;
                            if (bb && ba->n == bb->n && !memcmp(pa, pb, nn)) continue;
                            int first = -1; for (size_t q = 0; q < nn; q += 4) if (memcmp(pa + q, pb + q, 4)) { first = (int)q; break; }
                            char l3[900]; size_t o3 = (size_t)snprintf(l3, sizeof l3, "        %s %s %d (%#llx, %llu bytes probed%s) differs%s", ba->stage ? "fs" : "vs", ba->reg >= 16 ? "storage" : "uniform", ba->reg % 16,
                                (unsigned long long)ba->addr, (unsigned long long)ba->n, (ba->storage & 2) || (bb && (bb->storage & 2)) ? ", inline updates pending" : "", bb ? "" : " (not bound in the good frame)");
                            if (first >= 0) {
                                int st = first / 16 * 16; o3 += (size_t)snprintf(l3 + o3, sizeof l3 - o3, " from byte %d; flash/good floats at %d:", first, st);
                                for (int q = st; q < st + 32 && (size_t)q + 4 <= nn && o3 + 40 < sizeof l3; q += 4) { float fa, fb2; memcpy(&fa, pa + q, 4); memcpy(&fb2, pb + q, 4); o3 += (size_t)snprintf(l3 + o3, sizeof l3 - o3, " %g/%g", fa, fb2); }
                            }
                            LOGV("%s", l3);
                        }
                    }
                    for (int d = 0; d < ndp; d++) {   /* the depth passes it read: what they read */
                        const probe_write *w = dps[d]; char l2[600]; size_t o2 = (size_t)snprintf(l2, sizeof l2, "        that %ux%u %#x depth pass: fs %s, reads", w->w, w->h, w->fmt, w->key[0] ? w->key : "(none: depth only)");
                        for (int k = 0; k < w->nin && o2 + 60 < sizeof l2; k++) {
                            int from = -1; for (int q = 0; q < w->at && q < pf->n; q++) if (pf->m[q].img == w->in[k].img) from = q;
                            const probe_write *ww = NULL; for (int q = (int)(w - pf->wr) - 1; q >= 0; q--) if (pf->wr[q].img == w->in[k].img) { ww = &pf->wr[q]; break; }
                            o2 += (size_t)snprintf(l2 + o2, sizeof l2 - o2, " [%ux%u %#x%s%s]", w->in[k].w, w->in[k].h, w->in[k].fmt, ww ? " written by " : "", ww ? (ww->kind == 'D' ? "a depth pass" : ww->kind == 'C' ? "a copy" : ww->kind == 'K' ? "a clear" : ww->kind == 'X' ? "an image store" : "an upload") : (from >= 0 ? " a colour pass" : " not written this frame"));
                            if (ww && ww->kind == 'D') { double dn, dm, dx; if (depth_stats(pf, (int)(ww - pf->wr), &dn, &dm, &dx)) o2 += (size_t)snprintf(l2 + o2, sizeof l2 - o2, " (depth %.4f..%.4f)", dn, dx); }
                        }
                        if (!w->nin) snprintf(l2 + o2, sizeof l2 - o2, " no textures");
                        LOGV("%s", l2);
                    }
                    ndp = 0;
                }
            }
            i++; j++;
        } else if (L[i + 1][j] >= L[i][j + 1]) i++; else j++;
    }
    #undef PSAME
    LOGV("  %d passes paired, %d differ%s", paired, shown, shown >= 60 ? " (first 60 shown)" : "");
    return shown;
}
/* Presenter diagnostics and explicit test reports share LCS/depth scratch. This diagnostic-only
   lock never participates in recording or queue publication. */
static int probe_report(uint64_t flash, uint64_t before) {
    static pthread_mutex_t report_mu = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&report_mu);
    int shown = probe_report_inner(flash, before);
    pthread_mutex_unlock(&report_mu);
    return shown;
}
int vk_probe_report(uint64_t flash, uint64_t before) { return probe_report(flash, before); }   /* tests */
/* depth target sizes per frame: a change is logged with the frame number, to line up with the dark
   flashes (dynamic resolution re-creates the scene targets; their storage then holds last frame's content on hardware) */
static uint32_t g_fr[16], g_prev[16]; static int g_fr_n, g_prev_n; static uint64_t g_stat_rescg;
/* Overlay-only best effort: sample actual depth-backed MRT draws, never a clear
   that may have opened the pass with the preceding viewport still bound. */
static uint32_t g_overlay_scene_w, g_overlay_scene_h;
static int g_overlay_enabled, g_overlay_scene_dirty;
static void overlay_scene_draw(void) {
    if (!g_overlay_enabled || !g_overlay_scene_dirty) return;
    g_overlay_scene_dirty = 0;
    if (!R.dtex || R.ncolor < 2 || !S.has_vp || S.vp[0][0] != 0 || S.vp[0][1] != 0) return;
    float vw = fabsf(S.vp[0][2]), vh = fabsf(S.vp[0][3]);
    if (!isfinite(vw) || !isfinite(vh)) return;
    uint32_t sw = (uint32_t)(fminf(R.w, vw) + .5f), sh = (uint32_t)(fminf(R.h, vh) + .5f);
    /* DRS aligns axes independently, by up to one 16-texel quantum. Rounding
       each axis by at most 15 gives |9*w - 16*h| <= (9+16)*15, not exact 16:9. */
    const int alignment = 16, tolerance = (9 + 16) * (alignment - 1);
    int64_t error = (int64_t)sw * 9 - (int64_t)sh * 16;
    if (sw > sh && sh && error >= -tolerance && error <= tolerance &&
        (uint64_t)sw * sh > (uint64_t)g_overlay_scene_w * g_overlay_scene_h) {
        g_overlay_scene_w = sw; g_overlay_scene_h = sh;
    }
}
#ifdef TOTK_VK_TESTS
void vk_overlay_test_scene_reset(int enabled) {
    g_overlay_enabled = enabled; g_overlay_scene_w = g_overlay_scene_h = 0;
    g_overlay_scene_dirty = 1;
}
void vk_overlay_test_scene_size(uint32_t *w, uint32_t *h) {
    *w = g_overlay_scene_w; *h = g_overlay_scene_h;
}
#endif
static int u32cmp(const void *a, const void *b) { uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b; return x < y ? -1 : x > y; }
static void sizes_str(const uint32_t *v, int n, char *out, size_t cap) {
    size_t o = 0; out[0] = 0;
    for (int i = 0; i < n && o + 16 < cap; i++) o += (size_t)snprintf(out + o, cap - o, "%s%ux%u", i ? " " : "", v[i] >> 16, v[i] & 0xffff);
}
/* NVN barriers between draws on the same render targets (TOTK_VK_OFF=passbarrier: dropped, as before). The game orders
   fragment work and invalidates texture caches between draws that feed each other without changing targets: the
   material shaders store per-tile flags that the lighting quads read (nvn_mtl.m msl_stores_through). Inside one Vulkan
   pass draws have no such ordering (turnip in sysmem mode runs them overlapped), so the barrier ends the pass; the next
   draw reopens it with load. TOTK_VK_SERIAL=1 (diagnostic): end the pass after every draw. */
static uint64_t g_stat_inpass_barriers;
static int pass_barrier_on(void) { static int on = -1; if (on < 0) on = !vk_off("passbarrier"); return on; }
static int serial_draws(void) { static int on = -1; if (on < 0) { const char *e = getenv("TOTK_VK_SERIAL"); on = e && atoi(e); if (on) LOGV("TOTK_VK_SERIAL: every draw is its own pass"); } return on; }
static uint64_t g_merge_deferred, g_merge_retained, g_merge_hazards, g_merge_avoided;
static int pass_merge_on(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("TOTK_VK_PASS_MERGE");
        int requested = e && !strcmp(e, "1");
        on = requested && pass_barrier_on() && !store_clear_on();
        if (on) LOGV("[pass-merge] enabled: defer read-only barriers; retain storage, feedback, unknown, indirect and ZCULL dependencies");
        else if (requested) LOGV("[pass-merge] disabled: requires passbarrier enabled and STORE_CLEAR off (begin-time discard assumptions)");
    }
    return on;
}
void vk_pass_merge_counters(uint64_t out[4]) {
    out[0] = g_merge_deferred; out[1] = g_merge_retained; out[2] = g_merge_hazards; out[3] = g_merge_avoided;
}
/* No rw inference: even a nominally read-only storage descriptor is a hazard. This also
   protects readers before a deferred barrier from a later storage writer (WAR). */
static int pass_merge_draw_hazard(vk_shader *const sh[6]) {
    for (int st = 0; st < 2; st++) for (int i = 0; sh[st] && i < sh[st]->nb; i++) {
        const vk_bind *b = &sh[st]->b[i];
        if (!b->kind || b->kind == RB_IMG) return 1;
        if (b->dtype == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || b->dtype == VK_DESCRIPTOR_TYPE_SAMPLER) continue;
        if (b->dtype != VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER && b->dtype != VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE &&
            b->dtype != VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER) return 1;   /* includes dynamic storage and unknown types */
        if (b->reg < 0 || b->reg >= 128) return 1;
        uint64_t handle = S.tex[st][b->reg]; if (!handle) continue;   /* dummy texture */
        vk_treg t; if (!vk_treg_get((int)(handle & 0xfffff), &t) || !t.t.mem || !t.t.L.size) return 1;
        uint64_t lo = (uint64_t)(uintptr_t)t.t.mem, hi = lo + t.t.L.size; if (hi < lo) return 1;
        for (int a = 0; S.has_rt && a < 9; a++) {
            const tex_t *att = a < 8 ? (a < S.rt.n ? S.rt.color[a] : NULL) : S.rt.depth;
            if (!att) continue;
            uint64_t alo = (uint64_t)(uintptr_t)att->mem, ahi = alo + att->L.size;
            if (att->serial == t.t.serial || !att->L.size || ahi < alo || (lo < ahi && alo < hi)) return 1;
        }
    }
    return 0;
}
/* Texture-only invalidations can move to the existing full barrier at pass end
   only while all intervening reads refer to unchanged images. In particular,
   a loaded, read-only attachment is not an attachment write. Storage, unknown
   descriptors and memory aliases retain the original boundary. */
static int g_feedback_narrow_launch;
static totk_live_cache g_feedback_narrow_live;
static uint64_t g_feedback_deferred, g_feedback_avoided, g_feedback_hazards;
typedef struct { uint32_t read, written; int unknown; uint32_t pool; } feedback_usage;   /* pool: the part of read that is "a pool ID lies over that target" */
#ifdef TOTK_OHOS
/* TOTK_OHOS_BARRIER_ELIDE=1 (default 0): see feedback_narrow_barrier. The counts of the 300-present report: barriers
   that arrived inside a pass; those that waited; kept because a storage writer drew in the pass, for their bits, for
   another reason; waiting ones a draw ended: it reads a target the pass wrote, a pool ID lies over such a target, it
   writes storage or a target an earlier draw sampled. */
static int barrier_elide_on(void) { static int v = -1; if (v < 0) { const char *e = getenv("TOTK_OHOS_BARRIER_ELIDE"); v = e && atoi(e) == 1; if (v) LOGV("[barrier-elide] on: a barrier inside a pass ends it only for a draw that needs it (TOTK_OHOS_BARRIER_ELIDE)"); } return v; }
static uint64_t g_be_n[9];
static struct { uint32_t bits; uint64_t n; } g_be_bits[8];
static void be_note_bits(uint32_t bits) {
    for (int i = 0; i < 8; i++) { if (g_be_bits[i].n && g_be_bits[i].bits != bits) continue; g_be_bits[i].bits = bits; g_be_bits[i].n++; return; }
}
static uint32_t be_pool_alias(void) {   /* the open pass's targets under sampled pool IDs; kept for the last sets of targets */
    static struct { uint64_t serial[9], rev; uint32_t nlog, mask; int valid; } cache[8]; static unsigned next;
    const tex_t *att[9]; uint64_t serial[9];
    for (int i = 0; i < 9; i++) { att[i] = !S.has_rt ? NULL : i < 8 ? (i < S.rt.n ? S.rt.color[i] : NULL) : S.rt.depth; serial[i] = att[i] ? att[i]->serial : 0; }
    uint64_t rev = vk_treg_revision(); uint32_t nlog = vk_bindless_wanted();
    for (int i = 0; i < 8; i++) if (cache[i].valid && cache[i].rev == rev && cache[i].nlog == nlog && !memcmp(cache[i].serial, serial, sizeof serial)) return cache[i].mask;
    uint32_t m = vk_bindless_alias_mask(att); g_be_n[8]++;
    unsigned k = next++ % 8; memcpy(cache[k].serial, serial, sizeof serial); cache[k].rev = rev; cache[k].nlog = nlog; cache[k].mask = m; cache[k].valid = 1;
    return m;
}
#else
static int barrier_elide_on(void) { return 0; }
#endif
static int feedback_overlap(const tex_t *a, const tex_t *b) {
    uint64_t lo=(uintptr_t)a->mem, hi=lo+a->L.size, blo=(uintptr_t)b->mem, bhi=blo+b->L.size;
    return !lo || !blo || !a->L.size || !b->L.size || hi<lo || bhi<blo || (lo<bhi && blo<hi);
}
static feedback_usage feedback_draw_usage(vk_shader *const sh[6]) {
    feedback_usage use = {0};
    if (!S.has_rt) { use.unknown=1; return use; }
    for (unsigned i=0;i<9;i++) {
        const tex_t *a=i<8 ? (i<(unsigned)S.rt.n ? S.rt.color[i] : NULL) : S.rt.depth;
        if (!a) continue;
        for (unsigned j=0;j<i;j++) {
            const tex_t *b=j<8 ? (j<(unsigned)S.rt.n ? S.rt.color[j] : NULL) : S.rt.depth;
            if (b && feedback_overlap(a,b)) use.unknown=1;
        }
        if (i<8 && sh[1] && ((S.chmask.mask>>(4*i))&15) &&
            ((sh[1]->fout_mask|sh[1]->fout_span)==0 || ((sh[1]->fout_mask|sh[1]->fout_span)>>i&1)))
            use.written |= 1u<<i;
    }
    if (S.rt.depth) {
        if (S.ds.dt && S.ds.dw) use.written |= 1u<<8;
        if (S.ds.st && (S.st_wmask[0] || S.st_wmask[1])) use.written |= 1u<<9;
    }
    int elide=barrier_elide_on();
    for (unsigned st=0;st<2;st++)
        if (sh[st] && (sh[st]->has_storage_writes || (!elide && sh[st]->has_storage_reads))) use.unknown=1;
#ifdef TOTK_OHOS
    if (elide) for (unsigned st=0;st<2;st++) if (sh[st] && sh[st]->bindless) {   /* it may sample any pool ID: the targets one lies over */
        if (!R.pool_alias_known) { R.pool_alias=be_pool_alias(); R.pool_alias_known=1; }
        use.read|=R.pool_alias; use.pool|=R.pool_alias; break;
    }
#endif
    for (unsigned st=0;st<2;st++) for (int i=0;sh[st] && i<sh[st]->nb;i++) {
        const vk_bind *b=&sh[st]->b[i];
        if (b->dtype==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER) {
            if (!elide && b->kind!=RB_SUPPORT && b->kind!=RB_CBUF && b->kind!=RB_C1 && b->kind!=RB_DRV0 && b->kind!=RB_DRV2)
                use.unknown=1;
            continue;
        }
        if (b->dtype==VK_DESCRIPTOR_TYPE_SAMPLER) continue;
        if (elide && (b->dtype==VK_DESCRIPTOR_TYPE_STORAGE_BUFFER || b->dtype==VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC || b->dtype==VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER)) {
            if (b->rw) use.unknown=1;   /* read only: nothing a draw of this pass wrote unless a storage writer drew (feedback_unknown) */
            continue;
        }
        if ((b->dtype!=VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE && b->dtype!=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) ||
            (b->kind!=RB_TEX && b->kind!=RB_TEXSAMP) || b->count!=1 || b->reg<0 || b->reg>=128) {
            use.unknown=1; continue;
        }
        uint64_t handle=S.tex[st][b->reg]; if (!handle) continue;
        vk_treg t;
        if (!vk_treg_get((int)(handle&0xfffff),&t) || !t.t.mem || !t.t.L.size) { use.unknown=1; continue; }
        for (unsigned a=0;a<9;a++) {
            const tex_t *att=a<8 ? (a<(unsigned)S.rt.n ? S.rt.color[a] : NULL) : S.rt.depth;
            if (!att) continue;
            if (att->serial==t.t.serial && att->mem==t.t.mem && att->L.size==t.t.L.size)
                use.read |= a<8 ? 1u<<a : 3u<<8; /* all levels/aspects: no view/LOD guesses */
            else if (feedback_overlap(att,&t.t)) use.unknown=1;
        }
    }
    return use;
}
static int feedback_narrow_barrier(uint32_t bits) {
    const uint32_t allowed=NVN_BARRIER_INVALIDATE_TEXTURE_BIT|NVN_BARRIER_ORDER_PRIMITIVES_BIT;
#ifdef TOTK_OHOS
    g_be_n[0]++; be_note_bits(bits);
    if (barrier_elide_on()) {
        /* Order of primitives: Vulkan's. Order of fragments, texture / shader / descriptor / ZCULL invalidation: what a
           later draw reads of this pass's own writes, which the draw's check sees (feedback_draw_usage). Indirect data
           written by the GPU is not followed: kept. */
        if (!R.feedback_on || R.native || R.canvas || vk_fg_extract_active || PD.active || probe_on() || serial_draws() || store_clear_on() || pass_merge_on()) { g_be_n[4]++; return 0; }
        if (bits&NVN_BARRIER_ORDER_INDIRECT_DATA_BIT) { g_be_n[3]++; return 0; }
        if (R.feedback_unknown) { g_be_n[2]++; return 0; }
        R.feedback_pending |= bits|NVN_BARRIER_INVALIDATE_TEXTURE_BIT; R.feedback_armed=1; g_feedback_deferred++; g_be_n[1]++;
        return 1;
    }
#endif
    if (!R.feedback_on || R.feedback_unknown || !(bits&NVN_BARRIER_INVALIDATE_TEXTURE_BIT) ||
        (bits&~allowed) || R.native || R.canvas || vk_fg_extract_active || PD.active || PP.active ||
        probe_on() || serial_draws() || store_clear_on() || pass_merge_on()) return 0;
    R.feedback_pending |= bits; R.feedback_armed=1; g_feedback_deferred++;
    return 1;
}
#ifdef TOTK_VK_TESTS
void vk_draw_test_feedback_narrow_counts(uint64_t out[3]) {
    out[0]=g_feedback_deferred; out[1]=g_feedback_avoided; out[2]=g_feedback_hazards;
}
#endif
static int pass_merge_barrier(uint32_t bits) {
    const uint32_t allowed = NVN_BARRIER_ORDER_PRIMITIVES_BIT | NVN_BARRIER_ORDER_FRAGMENTS_BIT |
        NVN_BARRIER_ORDER_FRAGMENTS_TILED_BIT | NVN_BARRIER_INVALIDATE_TEXTURE_BIT |
        NVN_BARRIER_INVALIDATE_SHADER_BIT | NVN_BARRIER_INVALIDATE_TEXTURE_DESCRIPTOR_BIT;
    if (!pass_merge_on()) return 0;
    if (R.merge_storage || (bits & ~allowed) || PD.active || PP.active || probe_on() || serial_draws()) {
        g_merge_retained++; return 0;
    }
    R.merge_pending |= bits; R.merge_armed = 1; g_merge_deferred++;
    return 1;   /* actual barrier remains at the eventual pass end; never issue one inside dynamic rendering */
}
#ifdef TOTK_OHOS
/* Why passes end (HarmonyOS: the GPU's time is mostly pass count on this tiler): counts per reason, logged with the
   300-present report (vk_pass_end_report). */
static _Atomic uint64_t g_pass_end_count[16];
void vk_pass_end_report(uint64_t frame) {
    static const char *const names[16] = {"other", "nvn-barrier", "texture-barrier", "attachments", "clear", "copy", "query", "compute", "cpu-wait", "submission", "uniform", "present", "diagnostic", "merge-hazard", "split", "?"};
    char line[512]; size_t o = 0; uint64_t total = 0;
    for (int i = 0; i < 16; i++) { uint64_t c = atomic_exchange_explicit(&g_pass_end_count[i], 0, memory_order_relaxed); total += c; if (c && o + 40 < sizeof line) o += (size_t)snprintf(line + o, sizeof line - o, " %s %.1f", names[i], c / 300.0); }
    LOGV("frame %llu, pass ends per frame (%.1f):%s", (unsigned long long)frame, total / 300.0, line);
    {   char bl[200]; size_t bo = 0; bl[0] = 0;
        for (int i = 0; i < 8; i++) if (g_be_bits[i].n) bo += (size_t)snprintf(bl + bo, sizeof bl - bo, " %#x:%.1f", g_be_bits[i].bits, g_be_bits[i].n / 300.0);
        LOGV("frame %llu, barriers inside passes per frame: %.1f (bits%s); waited %.1f, kept: storage writer in the pass %.1f, indirect data %.1f, other %.1f; waiting ones ended by a draw that reads a target the pass wrote %.1f, pool ID over such a target %.1f, storage write or write after read %.1f; pool target masks made %.1f (TOTK_OHOS_BARRIER_ELIDE=%d)",
             (unsigned long long)frame, g_be_n[0] / 300.0, bl, g_be_n[1] / 300.0, g_be_n[2] / 300.0, g_be_n[3] / 300.0, g_be_n[4] / 300.0,
             g_be_n[5] / 300.0, g_be_n[6] / 300.0, g_be_n[7] / 300.0, g_be_n[8] / 300.0, barrier_elide_on());
        memset(g_be_n, 0, sizeof g_be_n); memset(g_be_bits, 0, sizeof g_be_bits); }
    extern void vk_pass_barrier_report(uint64_t frame);
    vk_pass_barrier_report(frame);
    extern void vk_flicker_report(uint64_t frame);
    vk_flicker_report(frame);
#ifdef TOTK_OHOS
    extern void vk_blob_report(uint64_t frame);
    vk_blob_report(frame);
#endif
}
/* TOTK_OHOS_PASS_BARRIER=narrow|full (default full until measured): the barriers around a render pass. The reference
   drains the GPU at both ends of every pass (ALL_COMMANDS -> ALL_COMMANDS). On this tiler that makes the next pass's
   binning (vertex) work wait for the last tile of the pass before it; with 150-290 passes a frame the GPU idles a third
   of the time (SmartPerf GPU load 59-67% in GPU-bound frames). Narrow:
   - pass end, when another pass follows (ATTACHMENTS, TEXTURE_BARRIER, NVN_BARRIER, CLEAR, MERGE_HAZARD, SPLIT, UNIFORM,
     SUBMISSION): fragment-stage writes -> fragment-stage reads and attachment loads. Every other reason (copies,
     dispatches, queries, readbacks, presents, diagnostics) keeps the full barrier, and so does a pass whose shaders could
     write storage (buffers or images that a vertex stage, index fetch or indirect draw of a later pass may consume).
   - pass begin: transfers and dispatches -> all graphics stages (the pre command buffer's uploads, copies and decodes
     between passes); this does not order the vertex work after the previous pass's fragments.
   - a draw whose vertex shader samples an image that a narrow-ended pass wrote since the last full barrier gets a full
     barrier first (ending its pass when one is open): vertex-stage reads are the one consumer a narrow end leaves out.
   vk_pass_barrier_report counts all of it with the 300-present report. */
extern void vk_tex_note_pass_write(vk_tex *x, uint64_t epoch);
extern void vk_tex_claim(vk_tex *x);   /* vk_res.c: before a command writes x, an image on shared memory takes the memory */
extern uint64_t vk_tex_pass_epoch(vk_tex *x);
static int pass_barrier_narrow_on(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("TOTK_OHOS_PASS_BARRIER");
        on = e && !strcmp(e, "narrow");
        LOGV("TOTK_OHOS_PASS_BARRIER=%s: pass barriers %s", e ? e : "(unset)",
             on ? "narrow (fragment-stage dependencies; the next pass's vertex work overlaps)" : "full (ALL_COMMANDS at both ends, the reference)");
    }
    return on;
}
static uint64_t g_pass_epoch = 1, g_full_epoch = 1;   /* passes ended; the epoch of the last full barrier */
static int g_pass_storage;                             /* a draw of the open (or opening) pass could write storage */
static int g_pass_force_full;                          /* the next pass end uses the full barrier */
static uint64_t g_pb_narrow, g_pb_full, g_pb_full_reason, g_pb_full_storage, g_pb_vs_checks, g_pb_vs_hazards;
void vk_pass_barrier_report(uint64_t frame) {
    if (!pass_barrier_narrow_on()) return;
    LOGV("frame %llu, pass barriers a frame: narrow %.1f, full %.1f (%.1f for the end reason, %.1f storage writes, %.1f vertex sampling); vertex-sampling checks %.1f",
         (unsigned long long)frame, g_pb_narrow / 300.0, g_pb_full / 300.0, g_pb_full_reason / 300.0, g_pb_full_storage / 300.0, g_pb_vs_hazards / 300.0, g_pb_vs_checks / 300.0);
    g_pb_narrow = g_pb_full = g_pb_full_reason = g_pb_full_storage = g_pb_vs_checks = g_pb_vs_hazards = 0;
}
static void pass_barrier_narrow(VkCommandBuffer cb) {
    VkMemoryBarrier2 mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                        VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        .srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_SHADER_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                        VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        .dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_INPUT_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT |
                         VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT};
    VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vkCmdPipelineBarrier2(cb, &di);
}
static void pass_begin_barrier(VkCommandBuffer cb) {
    if (!pass_barrier_narrow_on()) { vk_barrier_all(cb); return; }
    VkMemoryBarrier2 mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_SHADER_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT,
        .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
    VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vkCmdPipelineBarrier2(cb, &di);
}
/* The barrier that ends a pass; a narrow end marks the attachments it leaves unordered against vertex stages. */
static void pass_end_barrier(VkCommandBuffer cb, vk_pass_end_kind reason) {
    g_pass_epoch++;
    int narrow = pass_barrier_narrow_on() && !g_pass_force_full && !g_pass_storage &&
        (reason == VK_PASS_END_ATTACHMENTS || reason == VK_PASS_END_TEXTURE_BARRIER || reason == VK_PASS_END_NVN_BARRIER ||
         reason == VK_PASS_END_CLEAR || reason == VK_PASS_END_MERGE_HAZARD || reason == VK_PASS_END_SPLIT ||
         reason == VK_PASS_END_UNIFORM || reason == VK_PASS_END_SUBMISSION);
    if (narrow) {
        pass_barrier_narrow(cb); g_pb_narrow++;
        for (unsigned i = 0; i < R.ncolor && i < 8; i++) if (R.ctex[i]) vk_tex_note_pass_write(R.ctex[i], g_pass_epoch);
        if (R.dtex) vk_tex_note_pass_write(R.dtex, g_pass_epoch);
    } else {
        vk_barrier_all(cb); g_full_epoch = g_pass_epoch; g_pb_full++;
        if (pass_barrier_narrow_on()) { g_pb_full_storage += g_pass_storage != 0; g_pb_full_reason += !g_pass_storage && !g_pass_force_full; }
    }
    g_pass_storage = 0; g_pass_force_full = 0;
}
/* Per-shader facts for the narrow barriers (shader objects are immutable after creation): bit 0 = could write storage
   buffers or images, bit 1 = samples images (checked for the vertex stage). */
static uint8_t pass_barrier_shader_flags(const vk_shader *sh) {
    enum { N = 2048 };
    static struct { const vk_shader *sh; uint8_t flags; } cache[N];
    if (!sh) return 0;
    unsigned h = (unsigned)(((uintptr_t)sh >> 4) * 2654435761u) % N;
    if (cache[h].sh == sh) return cache[h].flags;
    uint8_t f = sh->has_storage_writes ? 1 : 0;
    for (int i = 0; i < sh->nb; i++) {
        const vk_bind *b = &sh->b[i];
        if (b->dtype == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) f |= 1;
        else if ((b->dtype == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER || b->dtype == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC ||
                  b->dtype == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER) && b->rw) f |= 1;
        else if ((b->dtype == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE || b->dtype == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) &&
                 b->reg >= 0 && b->reg < 128) f |= 2;
    }
    cache[h].sh = sh; cache[h].flags = f;
    return f;
}
/* Before a draw (walking thread): a vertex shader sampling an image a narrow-ended pass wrote since the last full
   barrier needs that barrier now; then note whether this draw's shaders could write storage. */
static void pass_barrier_draw(vk_shader *const sh[6]) {
    uint8_t f0 = pass_barrier_shader_flags(sh[0]), f1 = pass_barrier_shader_flags(sh[1]);
    if ((f0 & 2) && g_full_epoch != g_pass_epoch) {
        g_pb_vs_checks++;
        for (int i = 0; i < sh[0]->nb; i++) {
            const vk_bind *b = &sh[0]->b[i];
            if ((b->dtype != VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE && b->dtype != VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) || b->reg < 0 || b->reg >= 128) continue;
            uint64_t handle = S.tex[0][b->reg]; if (!handle) continue;
            int want = b->target; vk_tex *x = NULL;
            vk_tex_by_id((int)(handle & 0xfffff), &want, &x);
            if (!x || vk_tex_pass_epoch(x) <= g_full_epoch) continue;
            g_pb_vs_hazards++;
            if (R.open) { g_pass_force_full = 1; vk_end_rendering_at(VK_PASS_END_TEXTURE_BARRIER, 0); }
            else { VK_REC_DO(VK_REC_O, vk_barrier_all(vk_cb())); g_full_epoch = g_pass_epoch; }
            break;
        }
    }
    if ((f0 | f1) & 1) g_pass_storage = 1;
}
#endif
static void end_rendering_inner(vk_pass_end_kind reason, uint32_t detail) {
    if (!R.open && S.has_rt) {   /* clears still waiting for a draw become a pass of their own (load op CLEAR) */
        int pend = R.clear_d || R.clear_s; for (int i = 0; i < 8; i++) pend |= R.clear_c[i];
        if (pend) begin_rendering(0);
    }
    if (!R.open) return;
#ifdef TOTK_OHOS
    atomic_fetch_add_explicit(&g_pass_end_count[(unsigned)reason < 16 ? (unsigned)reason : 15], 1, memory_order_relaxed);
#endif
    if (pass_merge_on()) {
        int restored = (reason == VK_PASS_END_MERGE_HAZARD || reason == VK_PASS_END_QUERY) &&
            !R.merge_armed && R.merge_continued != 0;
        g_merge_avoided += R.merge_continued - (uint32_t)restored;
    }
    g_feedback_avoided += R.feedback_continued - (uint32_t)R.feedback_restore;
    VkCommandBuffer cb = vk_cb();
    if (R.secondary) {
        int parked;
        if (__builtin_expect(vk_gpuprof_on, 0)) {
            vk_gpuprof_end_token profile_end;
            vk_record_pass_flush(); /* final chunk still belongs to the profiler's current open pass */
            vk_gpuprof_end_deferred(reason, detail, &profile_end);
            parked = VK_REC_VALUE(VK_REC_O_ASSEMBLE, vk_record_pass_end_profiled(cb, &profile_end));
        } else {
            parked = VK_REC_VALUE(VK_REC_O_ASSEMBLE, vk_record_pass_end(cb));
        }
        if (parked) cb = vk_record_next_primary();
        descriptor_unbind();
    } else {
        vk_occlusion_end(cb);   /* precise queries remain on the original inline diagnostic path */
        vkCmdEndRendering(cb);
        if (vk_gpuprof_on) vk_gpuprof_end(cb, reason, detail);
    }
#ifdef TOTK_OHOS
    pass_end_barrier(cb, reason);
#else
    vk_barrier_all(cb);
#endif
#ifdef TOTK_OHOS
    {   /* BC targets on a GPU without BC: decode the blocks this pass rendered (vk_res.c bc_aux_view) */
        extern void vk_bc_aux_decode(VkCommandBuffer cb, vk_tex *x);
        for (unsigned i = 0; i < R.ncolor && i < 8; i++) if (R.ctex[i]) vk_bc_aux_decode(cb, R.ctex[i]);
    }
#endif
    /* This full barrier also issues every deferred texture invalidation before
       the next pass's reads. No deferred dependency survives an ownership end. */
    R.feedback_pending=0;
    if (PP.open >= 0) { PP.rec[PP.open].draws = (uint32_t)(g_stat_draws - PP.draws0); pp_end(cb, PP.open); PP.open = -1; }
    pso_life_pass_write(0);
    R.open = 0; g_bound_pipe = VK_NULL_HANDLE;
    if (vk_native_ui_enabled()) for (unsigned i = 0; i < R.ncolor; i++) if (R.ctex[i]) vk_tex_native_written(R.ctex[i], R.native);
    pd_pass_end(cb);
    if (vk_fg_extract_active) fg_extract_pass_end();
    fg_extract_authority_span_end(cb); /* rejected frames must consume/poison pending authority */
    fg_extract_writer_end(cb); /* rejected probes must also close/poison diagnostics */
    if (__builtin_expect(vk_rec_profile_on, 0)) vk_rec_profile_pass_end();
    if (R.dtex) probe_write_note('D', R.dtex, NULL, (uint32_t)(g_stat_draws - R.draws0));
    probe_pass(cb);
    pd_pass(cb);
}
void vk_end_rendering_at(vk_pass_end_kind reason, uint32_t detail) { VK_REC_DO(VK_REC_S_PASS, end_rendering_inner(reason, detail)); }
void vk_end_rendering(void) { vk_end_rendering_at(VK_PASS_END_OTHER, 0); }
/* A pending load-clear is already an NVN write, even before a draw attaches
 * the target. A descriptor read must not observe the preceding image version.
 * Resolve via the same attachment path (views, integer formats, native canvas,
 * MSAA and depth/stencil), rather than clearing an imprecise whole VkImage.
 * Walking-owner only; the guard excludes diagnostic reads during pass setup. */
void vk_draw_texture_read(int tid, const tex_t *texture) {
    static int resolving;
    if (R.open || resolving || !S.has_rt) return;
    int pending=R.clear_d || R.clear_s;
    for (unsigned i=0;i<8;i++) pending|=R.clear_c[i];
    if (!pending || !totk_live_cached(&g_clear_read_live,"TOTK_VK_CLEAR_ON_READ",g_clear_read_launch)) return;
    vk_treg registration;
    if (!texture) {
        if (!vk_treg_get(tid,&registration)) return;
        texture=&registration.t;
    }
    int overlaps=(R.clear_d || R.clear_s) && S.rt.depth && feedback_overlap(texture,S.rt.depth);
    for (unsigned i=0;i<8 && !overlaps;i++)
        overlaps=R.clear_c[i] && S.rt.color[i] && feedback_overlap(texture,S.rt.color[i]);
    if (!overlaps) return;
    resolving=1;
    vk_end_rendering_at(VK_PASS_END_TEXTURE_BARRIER,NVN_BARRIER_INVALIDATE_TEXTURE_BIT);
    resolving=0;
    g_clear_read_forced++;
}
#ifdef TOTK_VK_TESTS
void vk_draw_test_correctness_counts(uint64_t out[2]) {
    out[0]=g_alpha_enabled; out[1]=g_alpha_ignored;
}
uint64_t vk_draw_test_clear_read_count(void) { return g_clear_read_forced; }
#endif
int vk_record_counter_timestamp(VkQueryPool pool, uint32_t query, VkPipelineStageFlags2 stage) {
    if (!R.open || !R.secondary) return 0;
    vk_record_timestamp(pool, query, stage); return 1;
}
static int rt_same(const p_set_rt *a, const p_set_rt *b) {
    if (a->n != b->n || a->depth != b->depth || (a->depth && a->depth->serial != b->depth->serial)) return 0;
    if (a->has_dview != b->has_dview || (a->has_dview && memcmp(&a->dview, &b->dview, sizeof a->dview))) return 0;
    for (int i = 0; i < a->n && i < 8; i++) {
        if (!a->color[i] != !b->color[i] || (a->color[i] && a->color[i]->serial != b->color[i]->serial)) return 0;
        if (a->has_cview[i] != b->has_cview[i] || (a->has_cview[i] && memcmp(&a->cview[i], &b->cview[i], sizeof a->cview[i]))) return 0;
    }
    return 1;
}
/* Only a proven full clear kills an attachment. Treat all bound texture/image slots as reads, including aliases
   at the same guest address; uncertainty retains the store. This also excludes feedback in the opening draw. */
static uint32_t store_read_mask(uint64_t slots[2][160], uint32_t possible) {
    uint32_t read = 0;
    for (int st = 0; st < 2; st++) for (int i = 0; i < 160 && (possible & ~read); i++) {
        uint64_t handle = slots[st][i]; if (!handle) continue;
        vk_treg t; if (!vk_treg_get((int)(handle & 0xfffff), &t)) return possible;
        for (int k = 0; k < 9; k++) {
            uint32_t bit = k < 8 ? 1u << k : 3u << 8; if (!(possible & bit)) continue;
            const tex_t *att = k < 8 ? S.rt.color[k] : S.rt.depth;
            if (att && (att->serial == t.t.serial || att->mem == t.t.mem)) read |= bit;
        }
    }
    return read;
}
/* Dynamic rendering fixes storeOp at BEGIN, not END (unlike Metal). A future draw with this attachment would need
   its store if an implicit split occurs, so don't elide it even when it might stay in this pass. Unrelated draws
   may be crossed, but no copies/dispatches, calls, conditions, events, updates, submissions or unknown operations.
   CLEAR_LOAD splits at a full clear of the exact same target set: DONT_CARE never discards the clear or later writes.
   Bits 0..7: colors, 8: depth, 9: stencil. Pass dumps read the pre-clear pixels and always retain stores. */
static uint32_t store_clear_mask(void) {
    if (!store_clear_on() || PD.active || !S.has_rt || !vk_exec_draw_next()) return 0;
    uint32_t possible = 0;
    for (int i = 0; i < 8; i++) if (R.ctex[i]) possible |= 1u << i;
    if (R.dfmt != VK_FORMAT_UNDEFINED) possible |= 1u << 8;
    if (R.sfmt != VK_FORMAT_UNDEFINED) possible |= 1u << 9;
    /* Aliased slots have independent load/store operations; don't reason about their write order. */
    for (int i = 0; i < 9; i++) for (int j = 0; j < i; j++) {
        vk_tex *x = i < 8 ? R.ctex[i] : R.dtex, *y = j < 8 ? R.ctex[j] : R.dtex;
        if (x && x == y) return 0;
    }
    /* First scan just finds candidates. Only if one exists, copy bindings and resolve their identities on the
       second scan. Most passes reach another draw on their own attachments and return without registry lookups. */
    uint64_t slots[2][160];
    for (int check_reads = 0; check_reads < 2; check_reads++) {
        if (check_reads) {
            for (int st = 0; st < 2; st++) { memcpy(slots[st], S.tex[st], sizeof S.tex[st]); memcpy(slots[st] + 128, S.img[st], sizeof S.img[st]); }
            possible &= ~store_read_mask(slots, possible); if (!possible) return 0;
        }
        const uint8_t *p = vk_exec_draw_next();
        const p_set_rt *rt = &S.rt; p_scissor sc = S.sc; int has_sc = S.has_sc, disabled = S.render_disabled;
        uint32_t mask = 0, live = possible;
        for (int guard = 0; guard < 128 && (live & ~mask); guard++) {
            op_hdr h; memcpy(&h, p, sizeof h); const void *a = p + 8;
            if (h.size < 8 || (h.size & 7) || h.op >= OP_COUNT_) break;
            switch (h.op) {
                case OP_SET_RT: rt = a; break;
                case OP_SCISSOR: sc = *(const p_scissor *)a; has_sc = 1; break;
                case OP_RENDER_ENABLE: disabled = !((const p_flag *)a)->value; break;
                case OP_BIND_TEX: case OP_BIND_IMG: {
                    if (!check_reads) break;
                    const p_bind_handles *b = a; int n = h.op == OP_BIND_TEX ? 128 : 32, base = h.op == OP_BIND_TEX ? 0 : 128;
                    if (b->stage < 0 || b->stage >= 6 || b->first < 0 || b->n < 0 || b->first + b->n > n) goto scanned;
                    if (b->stage < 2) memcpy(&slots[b->stage][base + b->first], b->h, (size_t)b->n * 8);
                    break;
                }
                case OP_DRAW: case OP_DRAW_IDX: {
                    uint32_t need = 0;
                    for (int k = 0; k < 9; k++) {
                        uint32_t bit = k < 8 ? 1u << k : 3u << 8; if (!(live & ~mask & bit)) continue;
                        const tex_t *t = k < 8 ? S.rt.color[k] : S.rt.depth; if (!t) continue;
                        for (int j = 0; j < 9; j++) {
                            const tex_t *u = j < 8 ? (j < rt->n ? rt->color[j] : NULL) : rt->depth;
                            if (u && (t->serial == u->serial || t->mem == u->mem)) need |= bit;
                        }
                    }
                    if (check_reads) need |= store_read_mask(slots, live & ~mask & ~need);
                    live &= ~need; break;   /* even disabled draws are conservatively considered reads */
                }
                case OP_CLEAR_COLOR: case OP_CLEAR_DS: {
                    if (disabled || !rt_same(&S.rt, rt)) break;
                    if (has_sc && !(sc.x <= 0 && sc.y <= 0 && (int64_t)sc.x + sc.w >= R.w && (int64_t)sc.y + sc.h >= R.h)) break;
                    if (h.op == OP_CLEAR_COLOR) {
                        const p_clear_color *c = a;
                        if (c->index >= 0 && c->index < S.rt.n && c->index < 8 && (c->mask & 15) == 15) mask |= live & (1u << c->index);
                    } else {
                        const p_clear_ds *c = a; int sm = c->stencil_mask & 255;
                        if (sm && sm != 255) break;   /* clear_ds cannot move a masked stencil clear to loadOp */
                        if (c->depth_mask) mask |= live & (1u << 8);
                        if (sm == 255) mask |= live & (1u << 9);
                    }
                    break;
                }
                case OP_BEGIN: case OP_VIEWPORTS: case OP_DEPTH_RANGE: case OP_BIND_PROGRAM: case OP_BIND_STATE:
                case OP_BIND_VATTR: case OP_BIND_VSTREAM: case OP_BIND_VBUF: case OP_BIND_UBO: case OP_BIND_SSBO:
                case OP_BIND_SAMP: case OP_STENCIL: case OP_POLY_OFFSET: case OP_BLEND_COLOR: case OP_DEPTH_CLAMP:
                case OP_RASTER_DISCARD: case OP_COPY_STRIDE: case OP_BARRIER: case OP_TF_BIND: case OP_TF_STATE: case OP_PUSH_CONST: break;
                default: goto scanned;
            }
            p += h.size;
        }
scanned:
        if (!mask || check_reads) return mask;
        possible = mask;
    }
    return 0;
}
long vk_frame_chunk_limit(void) {
    static int initialized, explicit_limit, lag_requested;
    static long configured_limit;
    if (!initialized) {
        const char *limit = getenv("TOTK_VK_CHUNK_DRAWS");
        explicit_limit = limit && *limit;
        configured_limit = explicit_limit ? atol(limit) : 0;
        lag_requested = runtime_frame_lag_requested();
        initialized = 1;
    }
    if (explicit_limit) return configured_limit;
    return lag_requested && !nvn_dirty_lag_vma_pressure() ? 600 : 75;
}
/* TOTK_VK_SPLIT_PASS_DRAWS=<n> (default 0 = off): the chunk threshold above is only evaluated when a pass opens, so a
   long pass (the main scene pass, hundreds of draws) reaches the GPU only after its last draw is recorded. With n set,
   the n-th draw since the pass (or its previous part) opened ends the pass; the next pass start submits the recorded
   part through the same chunk submission and reopens the same targets with loadOp LOAD (the clears were consumed by
   the first part). Each split costs one extra store and load of every attachment on a tiler, so it stays opt-in.
   Refused (once per pass) where the open pass cannot be resumed as it was: a store chosen DONT_CARE at its start,
   native/canvas UI passes, framegen extraction, pass dumps/probes. Nothing is split while chunking is disabled
   (TOTK_VK_CHUNK_DRAWS=0) or outside a game submission, since there would be nothing to submit. */
static uint32_t g_split_draws = UINT32_MAX;
static int g_split_submit;   /* the next pass start submits the recorded part (and reopens the split targets) */
static uint64_t g_split_count, g_split_reopen, g_split_refused, g_split_touches, g_split_lrz_refused;
static int g_split_safe_launch = 1;
static totk_live_cache g_split_safe_live;
static int split_lrz_safe(void) { return totk_live_cached(&g_split_safe_live, "TOTK_VK_SPLIT_LRZ_SAFE", g_split_safe_launch); }
/* Walking-owned depth epochs. A cache collision loses only a touch elision, never safety. Track even in the
   control arm so a live enable repairs depth written before it. VkImage, not guest texture/view, is the identity. */
static struct { VkImage image; uint64_t game, seal; } g_split_depth[64];
static uint64_t g_split_game;
static int g_split_cut, g_split_unrepairable;
static int split_depth_touchable(vk_tex *x) {
    VkFormat f = vk_tex_format(x);
    return vk_tex_samples(x) == 1 && (f == VK_FORMAT_D16_UNORM || f == VK_FORMAT_D32_SFLOAT ||
        f == VK_FORMAT_X8_D24_UNORM_PACK32 || f == VK_FORMAT_D24_UNORM_S8_UINT || f == VK_FORMAT_D32_SFLOAT_S8_UINT);
}
static void split_depth_begin(VkCommandBuffer cb, VkAttachmentLoadOp load) {
    if (!R.dtex || R.dfmt == VK_FORMAT_UNDEFINED) return;
    VkImage image = vk_tex_image(R.dtex);
    unsigned i = (unsigned)(((uintptr_t)image >> 4) % 64);
    uint64_t game = vk_game_submit_serial(), seal = vk_submit_serial();
    int known = g_split_depth[i].image == image && g_split_depth[i].game == game;
    int crossed = known ? g_split_depth[i].seal != seal : g_split_cut;
    int touchable = split_depth_touchable(R.dtex);
    if (!touchable) g_split_unrepairable = 1;
    if ((R.split_draws || g_split_cut) && split_lrz_safe() && crossed && load == VK_ATTACHMENT_LOAD_OP_LOAD && touchable) {
        /* A transfer depth write invalidates LRZ on Turnip and the Qualcomm driver. Preserve the exact texel
           bits (including D24); no CPU readback, clear, fragment shader, or change to the guest's depth state.
           This is outside rendering, on the NEW primary, before its LOAD and any secondary draw executes. */
        VkBuffer buffer; VkDeviceSize offset;
        (void)vk_transient(4, 4, &buffer, &offset);
        VkBufferImageCopy copy = {.bufferOffset = offset,
            .imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, R.dlevel, R.dlayer, 1}, .imageExtent = {1, 1, 1}};
        if (vk_gpuprof_on) vk_gpuprof_scope_begin(cb, "transfer");
        vk_barrier_all(cb);
        vkCmdCopyImageToBuffer(cb, image, VK_IMAGE_LAYOUT_GENERAL, buffer, 1, &copy);
        vk_barrier_all(cb);
        vkCmdCopyBufferToImage(cb, buffer, image, VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
        vk_barrier_all(cb);
        if (vk_gpuprof_on) vk_gpuprof_scope_end(cb);
        g_split_touches++;
    }
    g_split_depth[i].image = image; g_split_depth[i].game = game; g_split_depth[i].seal = seal;
}
static uint32_t split_pass_draws(void) {
    if (g_split_draws == UINT32_MAX) {
        const char *e = getenv("TOTK_VK_SPLIT_PASS_DRAWS"); long n = e && *e ? atol(e) : 0;
        g_split_draws = n <= 0 ? 0 : n > 1000000 ? 1000000 : (uint32_t)n;
        LOGV("split pass draws: %u (TOTK_VK_SPLIT_PASS_DRAWS; 0 = off: long passes reach the GPU only when they end)", g_split_draws);
    }
    return g_split_draws;
}
/* Live (runtime/live_ctl.h): taken when a pass (or a split part) opens, so one part never changes its split point. */
static totk_live_cache g_split_live;
static uint32_t split_pass_live(void) { return (uint32_t)totk_live_cached(&g_split_live, "TOTK_VK_SPLIT_PASS_DRAWS", (int)split_pass_draws()); }
#ifdef TOTK_VK_TESTS
void vk_draw_test_split_counts(uint64_t out[5]) {
    out[0] = g_split_count; out[1] = g_split_reopen; out[2] = g_split_refused;
    out[3] = g_split_touches; out[4] = g_split_lrz_refused;
}
#endif
/* TOTK_VK_FEEDBACK_CUT (see feedback_cut): the launch value, read once in vk_draw_switches_init, and the value in use.
   Live (runtime/live_ctl.h): taken at each descriptor build (record_stage_impl); the freeze memo's key carries the open
   pass's serial only while it is on, so a change never reuses sets built under the other value. */
static int g_feedback_cut_launch;
static _Atomic int g_feedback_cut;
static totk_live_cache g_feedback_cut_live;
static inline int feedback_cut_refresh(void) {   /* at the start of each descriptor build */
    int on = totk_live_cached(&g_feedback_cut_live, "TOTK_VK_FEEDBACK_CUT", g_feedback_cut_launch);
    if (atomic_load_explicit(&g_feedback_cut, memory_order_relaxed) != on) atomic_store_explicit(&g_feedback_cut, on, memory_order_relaxed);
    return on;
}
static uint64_t g_feedback_cuts, g_feedback_uncut;   /* sampled views cut below an attached level; left whole (base attached) */
#ifdef TOTK_VK_TESTS
void vk_draw_test_feedback_counts(uint64_t out[2]) { out[0] = g_feedback_cuts; out[1] = g_feedback_uncut; }
#endif
static unsigned view_level(const view_t *v, int has) { return has && v->set_levels ? v->base_level : 0; }
/* Opt-in, next-pass-open coverage proof. A certificate describes actual SPIR-V,
   not a shader key. Geometry is read through ordered CPU/snapshot authority;
   GPU-only producers, unknown layouts and all destination dependencies retain LOAD. */
static int g_load_elide_launch;
static totk_live_cache g_load_elide_live;
static uint64_t g_load_proven, g_load_elided;
static const p_draw *g_load_draw;
static const p_draw_idx *g_load_indexed;
static vk_shader *const *g_load_sh;
static VkPipeline g_load_pipeline;
static VkPipelineLayout g_load_layout;
static uint8_t g_load_vertices[48];
static unsigned g_load_vertex_bytes;
static VkDescriptorBufferInfo g_load_geometry;
static unsigned g_load_stream;
static int g_load_override;
static VkPipeline pipe_get(vk_shader *vs, vk_shader *fs, int prim, VkPipelineLayout layout);
static uint32_t load_coverage_mask(void) {
    vk_shader *vs = g_load_sh ? g_load_sh[0] : NULL, *fs = g_load_sh ? g_load_sh[1] : NULL;
    if (!vs || !fs || !vs->load_position_attr || !fs->load_full_outputs ||
        vs->has_storage_writes || fs->has_storage_writes || S.ds.dt || S.ds.st ||
        S.color.blend_en || S.color.logic_op != NVN_LOGIC_OP_COPY || (S.ms.a2c & 1) ||
        S.color.alpha_func != NVN_ALPHA_FUNC_ALWAYS || S.poly.mode != NVN_POLYGON_MODE_FILL ||
        S.poly.cull != NVN_FACE_NONE || R.native || R.canvas || vk_fg_extract_active ||
        PD.active || PP.active || probe_on() || R.samples != VK_SAMPLE_COUNT_1_BIT ||
        !S.has_vp || S.vp[0][0] != 0 || S.vp[0][1] != 0 ||
        S.vp[0][2] != R.w || S.vp[0][3] != R.h ||
        (S.has_sc && (S.sc.x > 0 || S.sc.y > 0 || (int64_t)S.sc.x + S.sc.w < R.w ||
                     (int64_t)S.sc.y + S.sc.h < R.h))) return 0;
    unsigned attr = vs->load_position_attr - 1;
    if (vs->vin_mask != (1u << attr) || attr >= (unsigned)S.nattr || S.attr[attr].offset) return 0;
    VkFormat format = vk_vertex_fmt(S.attr[attr].fmt);
    if (format != VK_FORMAT_R32G32_SFLOAT && format != VK_FORMAT_R16G16_SFLOAT) return 0;
    unsigned vertex_bytes = format == VK_FORMAT_R32G32_SFLOAT ? 8 : 4;
    unsigned stream = S.attr[attr].stream;
    if (stream >= 16 || S.stream[stream].divisor) return 0;
    int prim = g_load_draw ? g_load_draw->prim : g_load_indexed->prim;
    unsigned count = g_load_draw ? g_load_draw->count : g_load_indexed->count;
    if (prim != 4 || (count != 3 && count != 6)) return 0; /* triangle list only */
    if (g_load_indexed && (g_load_indexed->itype < 0 || g_load_indexed->itype > 2)) return 0;
    float xy[6][2];
    for (unsigned i = 0; i < count; i++) {
        int64_t vertex = g_load_draw ? (int64_t)g_load_draw->first + i : 0;
        if (g_load_indexed) {
            unsigned bytes = 1u << g_load_indexed->itype; uint32_t index = 0;
            uint64_t address = g_load_indexed->ibuf + (uint64_t)i * bytes;
            VkDescriptorBufferInfo frozen;
            if (address < g_load_indexed->ibuf || !vk_ub_replay_read(address, bytes, 16, &frozen)) return 0;
            vk_ub_read(address, &index, bytes); vertex = (int64_t)index + g_load_indexed->base_vertex;
        }
        if (vertex < 0 || (S.stream[stream].stride && (uint64_t)vertex > UINT64_MAX / S.stream[stream].stride)) return 0;
        uint64_t offset = (uint64_t)vertex * S.stream[stream].stride + S.attr[attr].offset;
        const NVNbufferRange *b = &S.vbuf[stream]; VkDescriptorBufferInfo frozen;
        if (offset > b->size || b->size - offset < vertex_bytes ||
            b->address + offset < b->address || !vk_ub_replay_read(b->address + offset, vertex_bytes, 16, &frozen)) return 0;
        uint8_t *raw = g_load_vertices + i * vertex_bytes;
        vk_ub_read(b->address + offset, raw, vertex_bytes);
        if (vertex_bytes == 8) memcpy(xy[i], raw, 8);
        else for (unsigned k = 0; k < 2; k++) {
            uint16_t h; memcpy(&h, raw + 2*k, 2);
            unsigned exponent = h >> 10 & 31, mantissa = h & 1023;
            if (exponent == 31) return 0;
            xy[i][k] = ldexpf((float)(exponent ? mantissa + 1024 : mantissa), exponent ? (int)exponent - 25 : -24);
            if (h & 0x8000) xy[i][k] = -xy[i][k];
        }
        for (unsigned k = 0; k < 2; k++)
            if (!isfinite(xy[i][k]) || fabsf(xy[i][k]) > 16 || xy[i][k] != truncf(xy[i][k])) return 0;
    }
    if (count == 3) {
        double area = ((double)xy[1][0] - xy[0][0]) * ((double)xy[2][1] - xy[0][1]) -
                      ((double)xy[1][1] - xy[0][1]) * ((double)xy[2][0] - xy[0][0]);
        if (!area) return 0;
        for (unsigned corner = 0; corner < 4; corner++) for (unsigned edge = 0; edge < 3; edge++) {
            unsigned j = (edge + 1) % 3;
            double cross = ((double)xy[j][0] - xy[edge][0]) * ((corner & 2 ? 1.0 : -1.0) - xy[edge][1]) -
                           ((double)xy[j][1] - xy[edge][1]) * ((corner & 1 ? 1.0 : -1.0) - xy[edge][0]);
            if (area > 0 ? cross < 0 : cross > 0) return 0;
        }
    } else {
        unsigned corners[2] = {0}, shared;
        for (unsigned i = 0; i < 6; i++) {
            if ((xy[i][0] != -1 && xy[i][0] != 1) || (xy[i][1] != -1 && xy[i][1] != 1)) return 0;
            unsigned bit = 1u << ((xy[i][0] == 1) | ((xy[i][1] == 1) << 1));
            if (corners[i / 3] & bit) return 0;
            corners[i / 3] |= bit;
        }
        shared = corners[0] & corners[1];
        if ((corners[0] | corners[1]) != 15 || (shared != 6 && shared != 9)) return 0;
    }
    uint32_t mask = fs->load_full_outputs & 255u;
    for (unsigned i = 0; i < 8; i++) if ((S.chmask.mask >> (4 * i) & 15) != 15) mask &= ~(1u << i);
    /* Aliases and every bound sampled/storage view are MAY-reads. */
    for (unsigned st = 0; st < 2; st++) for (unsigned slot = 0; slot < 160; slot++) {
        uint64_t handle = slot < 128 ? S.tex[st][slot] : S.img[st][slot - 128];
        if (!handle) continue;
        vk_treg t; if (!vk_treg_get((int)(handle & 0xfffff), &t)) return 0;
        for (unsigned i = 0; i < 8; i++) {
            const tex_t *a = S.rt.color[i];
            if (a && (a->serial == t.t.serial || !a->L.size || !t.t.L.size ||
                ((uintptr_t)a->mem < (uintptr_t)t.t.mem + t.t.L.size &&
                 (uintptr_t)t.t.mem < (uintptr_t)a->mem + a->L.size))) mask &= ~(1u << i);
        }
    }
    for (unsigned i = 0; i < 8; i++) for (unsigned j = 0; j < 9; j++) {
        const tex_t *a = S.rt.color[i], *b = j < 8 ? S.rt.color[j] : S.rt.depth;
        if (i != j && a && b && (a->serial == b->serial || a->mem == b->mem)) mask &= ~(1u << i);
    }
    if (mask) { g_load_vertex_bytes = vertex_bytes; g_load_stream = stream; }
    return mask;
}
#ifdef TOTK_VK_TESTS
void vk_draw_test_load_counts(uint64_t out[2]) { out[0] = g_load_proven; out[1] = g_load_elided; }
#endif
static int mesh_run_pending(void);   /* a run of mesh draws has dispatched kernels whose draws are not all recorded yet */
/* Open the pass for the current targets with the pending clears as load operations. 0 when nothing can be attached. */
static int begin_rendering_inner(int draw_tail) {
    if (R.open) return 1;
    fg_extract_refresh(1);   /* configuration/late-state lookup at pass boundaries only */
    uint64_t game = vk_game_submit_serial();
    if (g_split_game != game) { g_split_game = game; g_split_cut = g_split_unrepairable = 0; }
    /* The default follows resolved lag, including the native-pressure cutover.
       An explicit threshold (including disabled=0) remains authoritative. */
    { long lim = vk_frame_chunk_limit();
      g_split_reopen += (uint64_t)g_split_submit;
      /* not while a run of mesh draws is pending (unless the pass itself was split): a submission between a run's kernels
         and its draws made every later draw of the run dispatch its kernels again (oot-a102); the chunk waits for the
         first pass after the run */
      if (lim > 0 && (g_split_submit || g_chunk_draws >= (uint64_t)lim)) {
          if (!g_split_submit && mesh_run_pending()) g_chunk_held++;
          else { g_chunk_draws = 0; vk_submit_chunk(); }
      }
      g_split_submit = 0; }
    VkRenderingAttachmentInfo ca[8], da = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO}, sa = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    uint32_t w = 16384, h = 16384, nc = 0; int have = 0;
    g_pipe_dirty = 1;
    memset(R.cfmt, 0, sizeof R.cfmt); R.dfmt = R.sfmt = VK_FORMAT_UNDEFINED; memset(R.ctex, 0, sizeof R.ctex); R.dtex = NULL; R.samples = VK_SAMPLE_COUNT_1_BIT;
    R.native = R.canvas = 0;
    memset(R.caddr, 0, sizeof R.caddr); R.daddr = NULL; R.cleared = 0;
    if (S.has_rt) for (int i = 0; i < S.rt.n && i < 8; i++) {
        ca[i] = (VkRenderingAttachmentInfo){VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
            .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
        nc = (uint32_t)i + 1;
        const tex_t *t = S.rt.color[i]; if (!t) continue;
        vk_tex *x = vk_tex_get(t); if (!x) continue;
        const view_t *view = S.rt.has_cview[i] ? &S.rt.cview[i] : NULL;
        int native = !g_native_draw_1x && S.rt.n == 1 && !S.rt.depth &&
            (!view || ((!view->set_levels || !view->base_level) && (!view->set_layers || (!view->min_layer && view->nlayers <= 1)))) &&
            vk_tex_native_active(x);
        if (vk_canvas_on && (vk_tex_native_backed(x) || vk_tex_canvas_window(x))) R.canvas = 1;
        /* A queued full load-clear replaces every pixel, so neither yesterday's
           native contents nor a seed is needed. Reject padded windows, MRT,
           depth, alias formats and non-base subresources: their untouched
           pixels must still be preserved by the ordinary resolve/seed path. */
        uint32_t bw=0,bh=0;
        int full_clear = R.clear_c[i] && S.rt.n==1 && !S.rt.depth &&
            (native || vk_tex_native_backed(x)) &&
            (!view || ((!view->set_levels || (!view->base_level && view->nlevels<=1)) &&
             (!view->set_layers || (!view->min_layer && view->nlayers<=1)) &&
             (!view->set_format || view->format==t->d.format))) &&
            vk_tex_levels(x)==1 && vk_tex_layers(x)==1 && vk_tex_samples(x)==1 &&
            vk_tex_aspect(x)==VK_IMAGE_ASPECT_COLOR_BIT;
        if (full_clear) {
            vk_tex_native_basis(x,&bw,&bh);
            if (bw!=vk_tex_width(x,0) || bh!=vk_tex_height(x,0)) {
                /* Native pixels represent only the active basis. A whole
                   guest clear must also define the allocation's padding. */
                full_clear=0; native=0;
            }
        }
        if (!native) { if (full_clear) vk_tex_native_discard(x); else vk_tex_native_read(x); }
        VkImageView v = native ? (full_clear ? vk_tex_native_clear_view(x,view) : vk_tex_native_view(x,view,1)) : VK_NULL_HANDLE;
        R.native = v != VK_NULL_HANDLE;
        if (!v) { vk_tex_native_read(x); v = vk_tex_view(x, t, view, 1); }
        if (!v) continue;
        unsigned l = view_level(&S.rt.cview[i], S.rt.has_cview[i]);
        R.clevel[i] = (uint8_t)l; R.clayer[i] = S.rt.has_cview[i] && S.rt.cview[i].set_layers ? S.rt.cview[i].min_layer : 0;
        int f = S.rt.has_cview[i] && S.rt.cview[i].set_format ? S.rt.cview[i].format : t->d.format;
        uint32_t tw = vk_tex_width(x, l), th = vk_tex_height(x, l);
        { int bw, bh, vbw, vbh; nvn_fmt_bpb(t->d.format, &bw, &bh); nvn_fmt_bpb(f, &vbw, &vbh);   /* a one-texel-per-block view of a BC texture is sized in blocks */
          if (bw > vbw && vbw == 1) tw = (tw + bw - 1) / bw; if (bh > vbh && vbh == 1) th = (th + bh - 1) / bh; }
        w = MINV(w, tw); h = MINV(h, th);
        VkFormat vf; vk_fmt(f, &vf, NULL);
        R.c_serial[i] = t->serial; R.c_nvnfmt[i] = f;
        if (i == 0) { R.c0_level = l; R.c0_layer = S.rt.has_cview[0] && S.rt.cview[0].set_layers ? S.rt.cview[0].min_layer : 0; R.c0_serial = t->serial; R.c0_fmt = f; }
        ca[i].imageView = v; R.cfmt[i] = vf; R.ctex[i] = x; R.dclass[i] = vk_fmt_class(f); vk_tex_claim(x); vk_tex_mark_gpu_written(x); have = 1;
        R.caddr[i] = t->mem; if (R.clear_c[i]) R.cleared |= 1u << i;
        if (t->d.samples > 1) R.samples = (VkSampleCountFlagBits)t->d.samples;
        if (i == 0) R.c0_cleared = R.clear_c[0] ? 1 : 0;
        if (R.clear_c[i]) { ca[i].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; ca[i].clearValue = R.cc[i]; R.clear_c[i] = 0; }
    }
    if (S.has_rt && S.rt.depth) {
        const tex_t *t = S.rt.depth; vk_tex *x = vk_tex_get(t);
        VkImageView v = x ? vk_tex_view(x, t, S.rt.has_dview ? &S.rt.dview : NULL, 1) : VK_NULL_HANDLE;
        if (v) {
            unsigned l = view_level(&S.rt.dview, S.rt.has_dview);
            R.dlevel = (uint8_t)l; R.dlayer = S.rt.has_dview && S.rt.dview.set_layers ? S.rt.dview.min_layer : 0;
            w = MINV(w, vk_tex_width(x, l)); h = MINV(h, vk_tex_height(x, l));
            { uint32_t key = vk_tex_width(x, l) << 16 | vk_tex_height(x, l); int k = 0;   /* the frame's depth target sizes */
              while (k < g_fr_n && g_fr[k] != key) k++;
              if (k == g_fr_n && g_fr_n < 16) g_fr[g_fr_n++] = key; }
            VkImageAspectFlags as = vk_tex_aspect(x); R.dtex = x; R.d_serial = t->serial; vk_tex_claim(x); vk_tex_mark_gpu_written(x); have = 1;
            R.daddr = t->mem; if (R.clear_d || R.clear_s) R.cleared |= 1u << 8;
            if (as & VK_IMAGE_ASPECT_DEPTH_BIT) { da.imageView = v; da.imageLayout = VK_IMAGE_LAYOUT_GENERAL; da.loadOp = R.clear_d ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD; da.storeOp = VK_ATTACHMENT_STORE_OP_STORE; R.dfmt = vk_tex_format(x); da.clearValue.depthStencil.depth = R.dfmt == VK_FORMAT_D16_UNORM ? d16_clear(R.cd) : R.cd; }
            if (as & VK_IMAGE_ASPECT_STENCIL_BIT) { sa.imageView = v; sa.imageLayout = VK_IMAGE_LAYOUT_GENERAL; sa.loadOp = R.clear_s ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD; sa.storeOp = VK_ATTACHMENT_STORE_OP_STORE; sa.clearValue.depthStencil.stencil = R.cs; R.sfmt = vk_tex_format(x); }
            if (t->d.samples > 1) R.samples = (VkSampleCountFlagBits)t->d.samples;
            R.clear_d = R.clear_s = 0;
        }
    }
    if (!have) {   /* no target: a pass of rasterisation only, sized by viewport 0 (or the scissor) */
        double x1 = S.has_vp ? ceil(fmax(S.vp[0][0], S.vp[0][0] + S.vp[0][2])) : 0, y1 = S.has_vp ? ceil(fmax(S.vp[0][1], S.vp[0][1] + S.vp[0][3])) : 0;
        if ((x1 < 1 || y1 < 1) && S.has_sc) { x1 = (double)S.sc.x + S.sc.w; y1 = (double)S.sc.y + S.sc.h; }
        w = x1 < 1 ? 1 : x1 > 16384 ? 16384 : (uint32_t)x1; h = y1 < 1 ? 1 : y1 > 16384 ? 16384 : (uint32_t)y1;
    }
    R.ncolor = nc; R.w = w; R.h = h;
    R.host_w = R.native_w = w; R.host_h = R.native_h = h;
    if (R.native) {
        vk_post_output host;
        if (!vk_tex_native_output(R.ctex[0], &host)) DIE("native UI: missing active attachment");
        R.host_w = host.w; R.host_h = host.h;
        vk_tex_native_basis(R.ctex[0], &R.native_w, &R.native_h);
    }
    /* Injected scene/depth reads are not in the guest lookahead's dependency proof. */
    uint32_t discard = draw_tail && !vk_fg_extract_active ? store_clear_mask() : 0; int discarded = 0;
    for (uint32_t i = 0; i < nc; i++) if (ca[i].imageView && (discard >> i & 1)) { ca[i].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE; g_store_clear_c++; discarded = 1; }
    if (da.imageView && (discard >> 8 & 1)) { da.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE; g_store_clear_ds++; discarded = 1; }
    if (sa.imageView && (discard >> 9 & 1)) { sa.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE; g_store_clear_ds++; discarded = 1; }
    g_store_clear_pass += discarded; R.store_discard = discarded; R.split_refused = 0; R.split_draws = split_pass_live();
    if (draw_tail && totk_live_cached(&g_load_elide_live, "TOTK_VK_LOAD_ELIDE", g_load_elide_launch)) {
        uint32_t mask = load_coverage_mask();
        for (unsigned i = 0; i < nc; i++) if (!ca[i].imageView || ca[i].loadOp != VK_ATTACHMENT_LOAD_OP_LOAD) mask &= ~(1u << i);
        if (mask) {
            g_load_proven++;
            /* A skipped asynchronous/failed pipeline must never discard old pixels. */
            g_load_layout = vk_layout_of(g_load_sh[0], g_load_sh[1]);
            g_load_pipeline = pipe_get(g_load_sh[0], g_load_sh[1], g_load_draw ? g_load_draw->prim : g_load_indexed->prim, g_load_layout);
            if (g_load_pipeline && g_load_pipeline != (VkPipeline)(uintptr_t)1) {
                unsigned count = g_load_draw ? g_load_draw->count : g_load_indexed->count;
                VkDeviceSize offset;
                void *vertices = vk_transient(count * g_load_vertex_bytes, 16, &g_load_geometry.buffer, &offset);
                memcpy(vertices, g_load_vertices, count * g_load_vertex_bytes);
                g_load_geometry.offset = offset; g_load_geometry.range = count * g_load_vertex_bytes;
                g_load_override = 1; /* the submitted geometry is exactly the certified immutable bytes */
                for (unsigned i = 0; i < nc; i++) if (mask >> i & 1) ca[i].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
                g_load_elided++;
            }
        }
    }
    VkCommandBuffer cb = vk_cb();
    split_depth_begin(cb, da.loadOp);
#ifdef TOTK_OHOS
    pass_begin_barrier(cb);
#else
    vk_barrier_all(cb);
#endif
    { extern uint32_t vk_tex_attach_layers(vk_tex *x, const tex_t *t, const view_t *v);
      uint32_t layers = 0;
      for (uint32_t i = 0; i < nc; i++) if (ca[i].imageView && R.ctex[i] && S.rt.color[i]) {
          uint32_t n = vk_tex_attach_layers(R.ctex[i], S.rt.color[i], S.rt.has_cview[i] ? &S.rt.cview[i] : NULL); layers = layers ? MINV(layers, n) : n; }
      if ((da.imageView || sa.imageView) && R.dtex && S.rt.depth) {
          uint32_t n = vk_tex_attach_layers(R.dtex, S.rt.depth, S.rt.has_dview ? &S.rt.dview : NULL); layers = layers ? MINV(layers, n) : n; }
      R.layers = layers ? layers : 1;
      if (R.layers > 1) { static _Atomic int said; if (atomic_fetch_add(&said, 1) < 4) LOGV("layered pass: %u layers (%ux%u, colour fmt %d)", R.layers, R.host_w, R.host_h, (int)R.cfmt[0]); } }
    VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, {R.host_w, R.host_h}}, .layerCount = R.layers, .colorAttachmentCount = nc, .pColorAttachments = ca,
        .pDepthAttachment = da.imageView ? &da : NULL, .pStencilAttachment = sa.imageView ? &sa : NULL};
    { int i = pp_begin(cb, 0); PP.open = i;
      if (i >= 0) { pp_rec *r = &PP.rec[i]; r->w = R.host_w; r->h = R.host_h; r->f0 = R.cfmt[0]; r->df = R.dfmt != VK_FORMAT_UNDEFINED ? R.dfmt : R.sfmt; r->ncolor = (uint8_t)nc; PP.draws0 = g_stat_draws;
          if (R.native) r->native_op=(uint8_t)(1+(vk_tex_native_window(R.ctex[0]) ? VK_NUI_WINDOW_PASS : VK_NUI_UI_PASS)); } }
    if (R.native) native_count[vk_tex_native_window(R.ctex[0]) ? VK_NUI_WINDOW_PASS : VK_NUI_UI_PASS]++;
    R.secondary = vk_record_enabled() && !PD.active && !PP.active && !probe_on() &&
        !(vk_fg_extract_active && fg_extract_inline());
    if (vk_gpuprof_on) {
        vk_gpuprof_secondary(R.secondary); /* stats stay inside secondaries; no inheritedQueries requirement */
        VkFormat fmt[10]; const tex_t *tex[10]; uint32_t widths[10], heights[10];
        for (int j = 0; j < 10; j++) {
            vk_tex *x = j < 8 ? R.ctex[j] : R.dtex;
            unsigned level = j < 8 ? R.clevel[j] : R.dlevel;
            fmt[j] = j < 8 ? R.cfmt[j] : j == 8 ? R.dfmt : R.sfmt;
            tex[j] = S.has_rt && x ? (j < 8 ? S.rt.color[j] : S.rt.depth) : NULL;
            widths[j] = x ? vk_tex_width(x, level) : 0; heights[j] = x ? vk_tex_height(x, level) : 0;
            if (j < 8 && tex[j]) {
                int bw, bh, vbw, vbh;
                int f = S.rt.has_cview[j] && S.rt.cview[j].set_format ? S.rt.cview[j].format : tex[j]->d.format;
                nvn_fmt_bpb(tex[j]->d.format, &bw, &bh); nvn_fmt_bpb(f, &vbw, &vbh);
                if (bw > vbw && vbw == 1) widths[j] = (widths[j] + bw - 1) / bw;
                if (bh > vbh && vbh == 1) heights[j] = (heights[j] + bh - 1) / bh;
            }
        }
        vk_gpuprof_begin(cb, &ri, fmt, tex, widths, heights, R.samples);
    }
    PD.draws0 = g_stat_draws;
    if (R.secondary) { ri.flags |= VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT; vk_record_pass_begin(nc, R.cfmt, R.dfmt, R.sfmt, R.samples); }
    R.merge_pending = R.merge_armed = R.merge_storage = R.merge_continued = 0;
    R.feedback_on=totk_live_cached(&g_feedback_narrow_live,"TOTK_VK_FEEDBACK_NARROW",g_feedback_narrow_launch) || barrier_elide_on();
    R.pool_alias_known=0;
    R.feedback_pending=R.feedback_sampled=R.feedback_continued=0;
    R.feedback_unknown=R.feedback_armed=R.feedback_restore=0;
    R.feedback_written=R.cleared&255u;
    if (da.imageView && da.loadOp==VK_ATTACHMENT_LOAD_OP_CLEAR) R.feedback_written|=1u<<8;
    if (sa.imageView && sa.loadOp==VK_ATTACHMENT_LOAD_OP_CLEAR) R.feedback_written|=1u<<9;
    if (pso_life_active()) {
        memset(PL.color_draws, 0, sizeof PL.color_draws); PL.depth_draws = PL.stencil_draws = 0;
        for (unsigned i = 0; i < 8; i++) PL.color_view[i] = i < nc ? ca[i].imageView : VK_NULL_HANDLE;
        PL.depth_view = da.imageView; PL.stencil_view = sa.imageView; PL.color_clear = R.cleared & 255u;
        PL.depth_clear = (da.imageView && da.loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) |
            (sa.imageView && sa.loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
    }
    vkCmdBeginRendering(cb, &ri);
    totk_ft_count(FT_COUNT_PASSES, 1);
    R.open = 1; g_bound_pipe = VK_NULL_HANDLE; g_dyn_dirty = 1; g_stat_passes++; R.draws0 = g_stat_draws; pass_inputs_reset(); g_pops.disabled_at_start = S.render_disabled;
    pso_life_pass_write(1);
    g_overlay_scene_dirty = 1;
    if (vk_fg_extract_active) fg_extract_pass_begin((R.cleared & 255u) |
        (da.imageView && da.loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR ? 1u << 8 : 0) |
        (sa.imageView && sa.loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR ? 1u << 9 : 0));
    if (__builtin_expect(vk_rec_profile_on, 0)) vk_rec_profile_pass_begin();
    return 1;
}
static int begin_rendering(int draw_tail) { return VK_REC_VALUE(VK_REC_S_PASS, begin_rendering_inner(draw_tail)); }

/* Walking-owned, current-real-frame image versions. Material textures are
   outside the scene root, not "missed contexts". Only accepted camera/depth
   roots and their actual sampled descendants participate in provenance. */
enum { FGX_WATCHES = 256 };
typedef struct {
    tex_t depth_desc;
    fg_dep_key depth_key;
    float camera[84];
    float admitted_viewport[4];
    int admitted_scissor[4];
    uint64_t signature;
    unsigned valid, group;
    struct {
        const char *reason, *vs, *fs;
        unsigned admission_pass, revoke_pass, writer, depth_func, camera_declared;
        uint64_t version, ubo_size;
        const char *camera_check;
        const char *unproven_vs, *unproven_fs, *unproven_check;
        uint64_t unproven_draws, unproven_depth_draws;
        unsigned unproven_depth_func, unproven_camera_declared;
        float viewport[4], range[2];
        int scissor[4]; unsigned has_viewport, has_scissor;
    } diag;
} fg_extract_scene;
typedef struct {
    fg_dep_key key;
    uint64_t address, size;
} fg_extract_watch;
typedef struct {
    vk_record_draw draw;
    VkDescriptorSet sets[2];
    unsigned width, height, final, clear;
    VkClearAttachment attachment;
    VkClearRect rect;
    VkFormat format;
    VkImage image; /* Actual attachment, not its guest-sized logical identity. */
    unsigned native;
} fg_extract_replay_draw;
int vk_fg_extract_active;
static struct {
    fg_dep_frame deps;
    fg_pass_frame passes;
    fg_pass_guard guard;
    fg_pass_probe probe;
    unsigned requested, snapshot_ok, probe_started;
    struct {
        unsigned probes, backoff, transfers, nominations, attempts, snapshots, roots;
        unsigned dummy, missing, bounds, storage, image_writes, writes;
        uint64_t cpu_ns[2], cpu_max[2];
        unsigned cpu_frames[2], physical, groups, contexts, missed;
        uint64_t binding_reflected[3], binding_view[3], binding_old_view[3], binding_calls[3];
        uint64_t binding_cpu_ns, binding_cpu_max, binding_top_frame, binding_top_signature;
        unsigned binding_overflows[3], binding_top_work, binding_top_width, binding_top_height;
        int binding_top_group;
        vk_canvas_image canvas;
        fg_dep_value value;
        unsigned root_count, result;
    } diag_window;
    unsigned binding_class, binding_group_work[FG_PASS_MAX_GROUPS + 1];
    struct {
        uint64_t draws, accepted, scissor_pixels, viewport_pixels;
        double max_scissor_fraction;
    } plane_coverage;
    fg_extract_scene scene[FG_DEP_MAX_CANDIDATES];
    fg_extract_watch watch[FGX_WATCHES];
    vk_canvas_image watch_image[FGX_WATCHES], scene_depth_image[FG_DEP_MAX_CANDIDATES];
    /* Existing dependency tags carry diagnostic cause bits through the exact
       sampled versions. They never participate in eligibility or taint tests. */
    struct {
        fg_dep_key key;
        vk_canvas_image image;
        fg_dep_value before;
        const char *kind;
        unsigned writer, pass;
    } unknown[64];
    unsigned nunknown, unknown_overflow;
    unsigned nscene, nwatch, operation, selected, captured, invalid, ui_seen;
    unsigned authority_ready, authority_gpu_ready, authority_root; /* CPU root / GPU span state */
    fg_dep_key authority_key;
    unsigned refusal;
    unsigned transfer, eligible, diag, nreplay, draw_kind;
    fg_extract_replay_draw replay_draw[1024], *record;
    vk_tex *ui_depth;
    unsigned ui_depth_level, ui_depth_layer;
    fg_dep_key canvas_key, ui_depth_key;
    uint64_t ui_depth_address, ui_depth_size;
    unsigned ui_width, ui_height;
    VkFormat ui_format;
    uint64_t frame, signature, stable_frames;
    uint64_t cpu_start;
    const char *last_snapshot_check;
    unsigned last_snapshot_result, last_snapshot_taint;
    struct {
        vk_tex *depth;
        unsigned open, level, layer, experimental;
        unsigned x, y, width, height;
    } authority_span;
    struct {
        vk_tex *depth;
        const char *vs, *fs, *check;
        unsigned open, level, layer, classification;
        uint64_t draw0;
    } writer;
    const char *reason, *last_reason;
    struct {
        fg_dep_key key[9];
        fg_dep_value old[9];
        fg_dep_sources reads, context;
        uint64_t span0, depth_draws, published_draws;
        unsigned outputs, context_outputs, depth_write, root_committed, camera_context, authority_context;
        unsigned unproven_depth;
        unsigned uncertified_context;
        const char *unproven_vs, *unproven_fs, *unproven_check;
        uint64_t unproven_draws, unproven_depth_draws;
        unsigned unproven_depth_func, unproven_camera_declared;
        uint64_t camera_addr;
        uint64_t camera_size, camera_aux_addr;
        uint64_t camera_fragment_addr;
        unsigned plane_context;
        uint64_t scissor_pixels, viewport_pixels;
        const char *vs, *fs; unsigned camera_declared;
        const char *camera_check;
        int group, root;
    } pass;
#ifdef TOTK_VK_TESTS
    unsigned test_authority_inline;
#endif
} FGX;
static int fg_extract_inline(void) {
#ifdef TOTK_VK_TESTS
    if (FGX.test_authority_inline) return 1;
#endif
    return FGX.captured && !FGX.invalid && vk_fg_canvas_view();
}

static fg_dep_key fg_extract_image_key(vk_tex *x, unsigned mip, unsigned layer, unsigned aspect) {
    return (fg_dep_key){.backing = x ? vk_tex_canvas_identity(x,mip,layer).incarnation : 0,
        .mip = mip, .layer = layer, .aspect = aspect};
}
static int fg_extract_same_key(fg_dep_key a, fg_dep_key b) {
    return a.backing && fg_dep_key_equal(a, b);
}
static int fg_extract_canvas(vk_tex *x, unsigned mip, unsigned layer) {
    return fg_extract_same_key(FGX.canvas_key,fg_extract_image_key(x,mip,layer,VK_IMAGE_ASPECT_COLOR_BIT));
}
/* Frozen dynamic state is already in host coordinates. Both UI and terminal
   replay must use the attachment extent that produced that state. */
static int fg_extract_replay_target(vk_post_output *out) {
    if (!R.open || R.ncolor != 1 || !R.ctex[0] || !R.host_w || !R.host_h) return 0;
    VkImage image = R.native ? vk_tex_native_image(R.ctex[0]) : vk_tex_image(R.ctex[0]);
    if (!image) return 0;
    *out = (vk_post_output){image,VK_NULL_HANDLE,R.host_w,R.host_h,R.cfmt[0]};
    return 1;
}
#ifdef TOTK_VK_TESTS
int vk_fg_test_replay_target(vk_post_output *out) {
    return out && fg_extract_replay_target(out);
}
#endif
static int fg_extract_ui_depth(vk_tex *x, unsigned mip, unsigned layer) {
    return !x ? !FGX.ui_depth_key.backing :
        fg_extract_same_key(FGX.ui_depth_key,fg_extract_image_key(x,mip,layer,vk_tex_aspect(x)));
}
static uint64_t fg_extract_cpu_ns(void) {
    struct timespec t;
    return clock_gettime(CLOCK_THREAD_CPUTIME_ID,&t) ? 0 : (uint64_t)t.tv_sec*1000000000u+t.tv_nsec;
}
static int fg_extract_requested_config(void) {
    static int requested=-1;
    if (requested<0) {
        const char *e=getenv("TOTK_FRAMEGEN");
        requested=e && (!strcmp(e,"1") || !strcmp(e,"interp") || !strcmp(e,"hybrid") ||
            !strcmp(e,"locked") || !strcmp(e,"adaptive"));
    }
    return requested;
}
static void fg_extract_refresh(int begin_pass) {
    static int requested = -1;
    if (requested < 0) {
        requested=fg_extract_requested_config(); FGX.requested=requested;
        const char *e=getenv("TOTK_FRAMEGEN_DIAG"); FGX.diag=e && !strcmp(e,"1");
    }
    /* No resource work, clocks, registry traversal or callback when unset. */
    int capture=requested && vk_fg_capturing();
    static int hybrid_capture=-1;
    if (requested && runtime_fps_hybrid_selected() && hybrid_capture!=capture) {
        vk_fg_history_cut(); FGX.probe=(fg_pass_probe){0};
        hybrid_capture=capture;
    }
    vk_fg_extract_active=capture && !FGX.invalid && fg_pass_probe_due(&FGX.probe);
    if (begin_pass && requested && FGX.diag && !FGX.cpu_start) FGX.cpu_start=fg_extract_cpu_ns();
}
static void fg_extract_reject(unsigned result, const char *reason) {
    if (!FGX.invalid) {
        FGX.reason = reason; FGX.refusal = result;
        if (!FGX.last_reason || strcmp(reason,FGX.last_reason)) {
            LOGV("[framegen-extract] real %llu %s",(unsigned long long)FGX.frame,reason);
            FGX.last_reason=reason;
        }
    }
    FGX.invalid = 1;
    vk_fg_extract_active = 0; /* failed proof must not keep scanning/serializing the rest of this probe */
    vk_fg_skip();
}
static int fg_extract_overlap(uint64_t a, uint64_t as, uint64_t b, uint64_t bs) {
    return a && b && as && bs && (a <= b ? b - a < as : a - b < bs);
}
static void fg_extract_revoke(fg_dep_key key, const char *reason, unsigned writer) {
    for (unsigned i = 0; i < FGX.nscene; i++) if (fg_extract_same_key(key, FGX.scene[i].depth_key)) {
        if (FGX.captured && FGX.selected == i) continue; /* immutable depth copy already retained */
        fg_extract_scene *c=&FGX.scene[i];
        if (FGX.diag && c->valid) {
            c->diag.reason=reason; c->diag.writer=writer;
            c->diag.revoke_pass=FGX.passes.physical_passes;
            c->diag.version=fg_dep_read(&FGX.deps,key).version;
            c->diag.vs=FGX.pass.vs; c->diag.fs=FGX.pass.fs;
            c->diag.depth_func=S.ds.df; c->diag.ubo_size=S.ubo[0][1].size;
            c->diag.camera_declared=FGX.pass.camera_declared;
            c->diag.camera_check=FGX.pass.camera_check;
            c->diag.unproven_vs=FGX.pass.unproven_vs;
            c->diag.unproven_fs=FGX.pass.unproven_fs;
            c->diag.unproven_check=FGX.pass.unproven_check;
            c->diag.unproven_draws=FGX.pass.unproven_draws;
            c->diag.unproven_depth_draws=FGX.pass.unproven_depth_draws;
            c->diag.unproven_depth_func=FGX.pass.unproven_depth_func;
            c->diag.unproven_camera_declared=FGX.pass.unproven_camera_declared;
            memcpy(c->diag.viewport,S.vp[0],sizeof c->diag.viewport);
            memcpy(c->diag.range,S.dr,sizeof c->diag.range);
            c->diag.has_viewport=S.has_vp; c->diag.has_scissor=S.has_sc;
            c->diag.scissor[0]=S.sc.x; c->diag.scissor[1]=S.sc.y;
            c->diag.scissor[2]=S.sc.w; c->diag.scissor[3]=S.sc.h;
        }
        FGX.scene[i].valid = 0;
        fg_dep_candidate_invalidate(&FGX.deps, UINT64_C(1) << i);
    }
}
static void fg_extract_invalidate(fg_dep_key key, unsigned writer,
                                  const vk_canvas_image *image, const char *kind) {
    fg_dep_value before = FGX.diag ? fg_dep_read(&FGX.deps,key) : (fg_dep_value){0};
    fg_dep_invalidate(&FGX.deps,key,writer);
    if (!FGX.diag) return;
    unsigned i=FGX.nunknown;
    if (i==64) {
        FGX.unknown_overflow++;
        LOGV("[framegen-unknown] real %llu overflow image=%#llx incarnation=%llu size=%ux%u format=%u mip=%u layer=%u aspect=%#x cause=%s writer=%#x at-pass=%u previous-known=%u previous-version=%llu previous-writer=%#x; causal list incomplete",
            (unsigned long long)FGX.frame,(unsigned long long)(image ? image->image : 0),(unsigned long long)key.backing,
            image ? image->width : 0,image ? image->height : 0,image ? image->format : 0,key.mip,key.layer,key.aspect,
            kind,writer,FGX.passes.physical_passes,before.known,(unsigned long long)before.version,before.writer);
        return;
    }
    FGX.nunknown++;
    FGX.unknown[i].key=key;
    FGX.unknown[i].image=image ? *image : (vk_canvas_image){0};
    FGX.unknown[i].before=before;
    FGX.unknown[i].kind=kind;
    FGX.unknown[i].writer=writer;
    FGX.unknown[i].pass=FGX.passes.physical_passes;
    fg_dep_resource *r=fg_dep_find(&FGX.deps,key,0);
    if (r) r->value.tags=UINT64_C(1)<<i;
}

static int fg_extract_camera_equal(const float a[84], const float b[84]);
static uint64_t fg_extract_camera_hash(const float camera[84]) {
    uint64_t hash=UINT64_C(14695981039346656037);
    for (unsigned i=0;i<84;i++) if (i<58 || i==62 || i==70 || i==72 || i==73) {
        uint32_t bits; memcpy(&bits,camera+i,sizeof bits);
        hash=(hash^bits)*UINT64_C(1099511628211);
    }
    return hash;
}
static void fg_extract_refusal_resources(vk_tex *canvas, unsigned level, unsigned layer,
                                         fg_dep_value value) {
    if (!FGX.diag) return;
    vk_canvas_image image=vk_tex_canvas_identity(canvas,level,layer);
    LOGV("[framegen-ancestry] real %llu canvas=%#llx incarnation=%llu size=%ux%u format=%u version=%llu writer=%#x cause-mask=%#llx causes=%u overflow=%u deps-failed=%u pass-flags=%#x",
        (unsigned long long)FGX.frame,(unsigned long long)image.image,(unsigned long long)image.incarnation,
        image.width,image.height,image.format,(unsigned long long)value.version,value.writer,
        (unsigned long long)value.tags,FGX.nunknown,FGX.unknown_overflow,FGX.deps.failed,FGX.passes.failed);
    for (unsigned i=0;i<FGX.nunknown;i++) if (value.tags & (UINT64_C(1)<<i)) {
        const vk_canvas_image *x=&FGX.unknown[i].image;
        const fg_dep_key *key=&FGX.unknown[i].key;
        const fg_dep_value *before=&FGX.unknown[i].before;
        LOGV("[framegen-unknown] real %llu cause-bit=%u image=%#llx incarnation=%llu size=%ux%u format=%u storage=%#llx bytes=%llu mip=%u layer=%u aspect=%#x cause=%s writer=%#x at-pass=%u previous-known=%u previous-version=%llu previous-writer=%#x previous-kind=%s previous-origins=%#llx previous-taint=%#x",
            (unsigned long long)FGX.frame,i,(unsigned long long)x->image,(unsigned long long)key->backing,
            x->width,x->height,x->format,(unsigned long long)x->storage,(unsigned long long)x->size,
            key->mip,key->layer,key->aspect,FGX.unknown[i].kind,FGX.unknown[i].writer,FGX.unknown[i].pass,
            before->known,(unsigned long long)before->version,before->writer,
            !before->known ? "untracked" : before->writer & FG_PASS_WRITER_OPERATION ? "operation" : "raster-pass",
            (unsigned long long)before->origins,before->tainted);
    }
    if (value.tainted && !value.tags)
        LOGV("[framegen-unknown] real %llu untagged-taint=%#x (%s)",
            (unsigned long long)FGX.frame,value.tainted,value.tainted & FG_DEP_TAINT_MAPPING ?
            "unsupported scene/canvas viewport or copy rectangle" : "merge/epoch/identity or diagnostic overflow, not an identified texture");
    for (unsigned i=0;i<FGX.nscene;i++) {
        const fg_extract_scene *c=&FGX.scene[i];
        const fg_pass_key *p=&FGX.passes.group[c->group].key;
        fg_dep_value depth=fg_dep_read(&FGX.deps,c->depth_key);
        LOGV("[framegen-root] real %llu bit=%u valid=%u in-origins=%u eligible=%u unresolved=%u revoked=%u depth-image=%#llx depth-incarnation=%llu mip=%u layer=%u size=%ux%u format=%u group=%u signature=%#llx depth-version=%llu depth-writer=%#x depth-writer-kind=%s depth-taint=%#x",
            (unsigned long long)FGX.frame,i,c->valid,(unsigned)(value.origins>>i&1),
            (unsigned)(value.eligible>>i&1),(unsigned)(value.unresolved>>i&1),
            (unsigned)(FGX.deps.revoked_candidates>>i&1),(unsigned long long)FGX.scene_depth_image[i].image,(unsigned long long)c->depth_key.backing,
            c->depth_key.mip,c->depth_key.layer,p->depth.width,p->depth.height,p->depth.vk_format,c->group,
            (unsigned long long)c->signature,(unsigned long long)depth.version,depth.writer,
            !depth.known ? "untracked" : depth.writer & FG_PASS_WRITER_OPERATION ? "operation" : "raster-pass",depth.tainted);
        int live=-1;
        for (unsigned j=0;j<FGX.nscene;j++) if (FGX.scene[j].valid &&
            fg_extract_same_key(c->depth_key,FGX.scene[j].depth_key)) { live=(int)j; break; }
        LOGV("[framegen-root-camera] real %llu bit=%u camera=%#llx pose=%.9g,%.9g,%.9g near/far=%.9g/%.9g aspect/fov=%.9g/%.9g projection=%.9g,%.9g,%.9g,%.9g camera-extent=%.0fx%.0f live-same-depth=%d exact-camera-match=%d admitted-pass=%u first-revoke=%s writer=%#x at-pass=%u depth-version-at-revoke=%llu depth-func-at-revoke=%u last-context-vs=%.80s last-context-fs=%.80s last-vs-camera-declared=%u vs-ubo1-bytes-at-revoke=%llu",
            (unsigned long long)FGX.frame,i,(unsigned long long)fg_extract_camera_hash(c->camera),
            (double)c->camera[47],(double)c->camera[51],(double)c->camera[55],
            (double)c->camera[56],(double)c->camera[57],(double)c->camera[62],(double)c->camera[70],
            (double)c->camera[28],(double)c->camera[33],(double)c->camera[30],(double)c->camera[34],
            (double)c->camera[72],(double)c->camera[73],live,
            live>=0 ? fg_extract_camera_equal(c->camera,FGX.scene[live].camera) : -1,
            c->diag.admission_pass,c->diag.reason ? c->diag.reason : "none",c->diag.writer,c->diag.revoke_pass,
            (unsigned long long)c->diag.version,c->diag.depth_func,c->diag.vs ? c->diag.vs : "-",
            c->diag.fs ? c->diag.fs : "-",c->diag.camera_declared,(unsigned long long)c->diag.ubo_size);
        if (!c->valid) LOGV("[framegen-root-viewport] real %llu bit=%u camera-check=%s has-viewport=%u viewport=%.9g,%.9g,%.9g,%.9g depth-range=%.9g,%.9g has-scissor=%u scissor=%d,%d,%d,%d (state-at-first-revoke)",
            (unsigned long long)FGX.frame,i,c->diag.camera_check ? c->diag.camera_check : "none",
            c->diag.has_viewport,(double)c->diag.viewport[0],(double)c->diag.viewport[1],
            (double)c->diag.viewport[2],(double)c->diag.viewport[3],
            (double)c->diag.range[0],(double)c->diag.range[1],c->diag.has_scissor,
            c->diag.scissor[0],c->diag.scissor[1],c->diag.scissor[2],c->diag.scissor[3]);
        if (c->diag.reason && !strcmp(c->diag.reason,"raster-depth-without-accepted-camera"))
            LOGV("[framegen-root-unproven] real %llu bit=%u span-draws=%llu span-depth-draws=%llu vs=%.80s fs=%.80s camera-check=%s camera-declared=%u depth-func=%u camera-context=0 (first depth-writing span without accepted camera)",
                (unsigned long long)FGX.frame,i,
                (unsigned long long)c->diag.unproven_draws,
                (unsigned long long)c->diag.unproven_depth_draws,
                c->diag.unproven_vs ? c->diag.unproven_vs : "-",
                c->diag.unproven_fs ? c->diag.unproven_fs : "-",
                c->diag.unproven_check ? c->diag.unproven_check : "none",
                c->diag.unproven_camera_declared,c->diag.unproven_depth_func);
    }
}
/* Image incarnation/subresource is the GPU alias authority. Independent images
   may overlap guest storage (different format/extent); their GPU content is
   deliberately independent in vk_res. Guest/buffer writes still watch ranges. */
static void fg_extract_publish(fg_dep_key key, const tex_t *t, const fg_dep_sources *sources,
                                unsigned writer, fg_dep_mask seed) {
    if (!key.backing || !t) return;
    uint64_t address = (uint64_t)(uintptr_t)t->mem;
    fg_dep_value value = fg_dep_write(&FGX.deps, key, sources, writer, seed, 0, 0, 0);
    if (!value.origins) return;
    unsigned i = 0;
    while (i < FGX.nwatch && !fg_extract_same_key(FGX.watch[i].key, key)) i++;
    if (i == FGX.nwatch) {
        if (i == FGX_WATCHES) { fg_dep_fail(&FGX.deps); return; }
        FGX.nwatch++;
    }
    FGX.watch[i] = (fg_extract_watch){key, address, t->L.size};
    if (FGX.diag) FGX.watch_image[i]=vk_tex_canvas_identity(vk_tex_get(t),key.mip,key.layer);
}
void vk_fg_extract_memory_write(uint64_t address, uint64_t size) {
    if (!vk_fg_extract_active || !address) return;
    if (!size) size = UINT64_MAX;
    if (FGX.captured && fg_extract_overlap(address,size,FGX.ui_depth_address,FGX.ui_depth_size))
        fg_extract_reject(VK_FG_STRUCTURE_CHANGED,"CPU/storage write to captured UI depth");
    if (R.open && ((FGX.pass.camera_addr &&
        fg_extract_overlap(address,size,FGX.pass.camera_addr,FGX.pass.camera_size)) ||
        (FGX.pass.camera_aux_addr &&
        fg_extract_overlap(address,size,FGX.pass.camera_aux_addr,sizeof(float))) ||
        (FGX.pass.camera_fragment_addr &&
        fg_extract_overlap(address,size,FGX.pass.camera_fragment_addr,84*sizeof(float))))) {
        fg_pass_guard_invalidate(&FGX.guard);
    }
    for (unsigned i = 0; i < FGX.nwatch; i++) {
        const fg_extract_watch *w = &FGX.watch[i];
        if (!fg_extract_overlap(address, size, w->address, w->size)) continue;
        FGX.diag_window.writes++;
        if (FGX.captured && fg_extract_same_key(w->key,FGX.canvas_key))
            fg_extract_reject(VK_FG_STRUCTURE_CHANGED,"CPU/storage write to captured canvas");
        fg_extract_invalidate(w->key,FG_PASS_WRITER_OPERATION | ++FGX.operation,&FGX.watch_image[i],"CPU/buffer-storage-write");
        fg_extract_revoke(w->key,"CPU/buffer-storage-write",FG_PASS_WRITER_OPERATION|FGX.operation);
    }
}
static void fg_extract_span(void) {
    uint64_t draws = g_stat_draws - FGX.pass.span0;
    if (draws) {
        if (FGX.diag && FGX.pass.plane_context && FGX.pass.depth_write) {
            FGX.plane_coverage.draws+=draws;
            FGX.plane_coverage.accepted+=FGX.pass.camera_context ? draws : 0;
            FGX.plane_coverage.scissor_pixels+=draws*FGX.pass.scissor_pixels;
            FGX.plane_coverage.viewport_pixels+=draws*FGX.pass.viewport_pixels;
            double fraction=FGX.pass.viewport_pixels ?
                (double)FGX.pass.scissor_pixels/FGX.pass.viewport_pixels : 0;
            if (fraction>FGX.plane_coverage.max_scissor_fraction)
                FGX.plane_coverage.max_scissor_fraction=fraction;
        }
        FGX.pass.outputs |= FGX.pass.context_outputs;
        if (FGX.pass.depth_write) {
            FGX.pass.depth_draws += draws;
            if (!FGX.pass.camera_context) {
                if (!FGX.pass.unproven_depth) {
                    FGX.pass.unproven_vs=FGX.pass.vs;
                    FGX.pass.unproven_fs=FGX.pass.fs;
                    FGX.pass.unproven_check=FGX.pass.camera_check;
                    FGX.pass.unproven_draws=draws;
                    FGX.pass.unproven_depth_draws=FGX.pass.depth_draws;
                    FGX.pass.unproven_depth_func=S.ds.df;
                    FGX.pass.unproven_camera_declared=FGX.pass.camera_declared;
                }
                FGX.pass.unproven_depth=1;
            }
            if (FGX.pass.camera_context || FGX.pass.authority_context) FGX.pass.root_committed = 1;
        }
        for (unsigned i = 0; i < FGX.pass.context.count; i++)
            fg_dep_sources_add(&FGX.deps,&FGX.pass.reads,FGX.pass.context.source[i]);
        FGX.pass.reads.value.tainted |= FGX.pass.context.value.tainted;
    }
    FGX.pass.span0 = g_stat_draws;
}
static void fg_extract_state_change(int inputs) {
    if (!vk_fg_extract_active || !R.open) return;
    fg_extract_span();
    if (inputs) {
        fg_pass_guard_invalidate(&FGX.guard);
    }
}
static void fg_extract_pass_begin(uint32_t cleared) {
    FGX.probe_started = 1;
    if (!FGX.deps.epoch) { fg_dep_reset(&FGX.deps); fg_pass_reset(&FGX.passes); }
    memset(FGX.pass.key,0,sizeof FGX.pass.key);
    FGX.pass.depth_draws = FGX.pass.camera_addr = 0;
    FGX.pass.unproven_depth=0;
    FGX.pass.camera_size=FGX.pass.camera_aux_addr=0;
    FGX.pass.camera_fragment_addr=0;
    FGX.pass.plane_context=0;
    FGX.pass.outputs = FGX.pass.context_outputs = FGX.pass.root_committed = FGX.pass.camera_context = 0;
    FGX.pass.authority_context=0;
    FGX.pass.uncertified_context=0;
    fg_dep_sources_init(&FGX.pass.reads); fg_dep_sources_init(&FGX.pass.context);
    if (FGX.diag) { FGX.pass.vs=FGX.pass.fs=NULL; FGX.pass.camera_declared=0; }
    FGX.pass.camera_check="no-camera-context";
    FGX.pass.root = -1;
    FGX.pass.span0 = FGX.pass.published_draws = g_stat_draws;
    FGX.pass.depth_write = R.dfmt != VK_FORMAT_UNDEFINED && S.ds.dt && S.ds.dw;
    fg_pass_key key = {.width = R.w, .height = R.h};
    for (unsigned i = 0; i < 9; i++) {
        vk_tex *x = i < 8 ? R.ctex[i] : R.dtex;
        if (!x) continue;
        unsigned mip = i < 8 ? R.clevel[i] : R.dlevel, layer = i < 8 ? R.clayer[i] : R.dlayer;
        unsigned aspect = i < 8 ? VK_IMAGE_ASPECT_COLOR_BIT : vk_tex_aspect(x);
        if (aspect & VK_IMAGE_ASPECT_DEPTH_BIT) aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
        fg_dep_key k = fg_extract_image_key(x, mip, layer, aspect);
        FGX.pass.key[i] = k;
        fg_pass_attachment *a = i < 8 ? &key.color[i] : &key.depth;
        *a = (fg_pass_attachment){.key = k, .vk_format = i < 8 ? R.cfmt[i] : R.dfmt,
            .width = vk_tex_width(x, mip), .height = vk_tex_height(x, mip), .samples = vk_tex_samples(x)};
        if (cleared >> i & 1) {
            fg_dep_clear(&FGX.deps, k, FGX.passes.physical_passes + 1);
            fg_extract_revoke(k,"attachment-load-clear",FGX.passes.physical_passes+1);
        }
        FGX.pass.old[i] = fg_dep_read(&FGX.deps, k);
    }
    FGX.pass.group = fg_pass_begin(&FGX.passes, &key);
    fg_pass_guard_reset(&FGX.guard, UINT_MAX);
    if (FGX.captured && !FGX.invalid) {
        VkClearRect rect = {{{0,0},{R.host_w,R.host_h}},0,1};
        if (cleared & 1) {
            VkClearAttachment attachment = {VK_IMAGE_ASPECT_COLOR_BIT,0,R.cc[0]};
            (void)fg_extract_ui_clear(&attachment,&rect);
        }
        VkImageAspectFlags aspect = (cleared & (1u << 8) ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) |
            (cleared & (1u << 9) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
        if (aspect) {
            VkClearAttachment attachment = {aspect,0,{.depthStencil = {R.dfmt == VK_FORMAT_D16_UNORM ? d16_clear(R.cd) : R.cd,R.cs}}};
            (void)fg_extract_ui_clear(&attachment,&rect);
        }
    }
}
static void fg_extract_publish_pass(void) {
    fg_extract_span();
    if (FGX.pass.published_draws == g_stat_draws) return;
    unsigned writer = FGX.passes.physical_passes;
    /* Ordered span observations certify a depth VALUE, not an untouched pixel.
       Same-value writes/restoration preserve value authority; proven geometry
       after an unproven run is not a snapshot-to-final unknown. Colour changes
       remain the already accepted non-depth-writing-pass concern. */
    int masked=FGX.authority_ready && FGX.pass.root==(int)FGX.authority_root &&
        fg_extract_same_key(FGX.pass.key[8],FGX.authority_key);
    fg_dep_mask seed = FGX.pass.root >= 0 && FGX.pass.root_committed && FGX.pass.depth_draws &&
        (!FGX.pass.unproven_depth || masked) ? UINT64_C(1) << FGX.pass.root : 0;
    for (unsigned i = 0; g_stat_draws != R.draws0 && i < 9; i++) {
        if (!FGX.pass.key[i].backing || !(i < 8 ? FGX.pass.outputs >> i & 1 : FGX.pass.depth_draws != 0)) continue;
        fg_dep_sources sources; fg_dep_sources_init(&sources);
        if (!seed) sources.value = FGX.pass.reads.value;
        fg_dep_sources_add(&FGX.deps, &sources, FGX.pass.old[i]);
        if (i == 8 && !seed) fg_extract_revoke(FGX.pass.key[i],"raster-depth-without-accepted-camera",writer);
        if (FGX.nscene)
            fg_extract_publish(FGX.pass.key[i], i < 8 ? S.rt.color[i] : S.rt.depth, &sources, writer, seed);
    }
    FGX.pass.published_draws = g_stat_draws;
}
static void fg_extract_pass_end(void) {
    fg_extract_publish_pass();
    fg_pass_end(&FGX.passes, FGX.pass.group, g_stat_draws - R.draws0, FGX.pass.depth_draws);
}
static int fg_extract_reads_camera(const vk_shader *sh) {
    for (int i = 0; sh && i < sh->nb; i++) {
        const vk_bind *b = &sh->b[i];
        if (b->reg == 1 && (b->kind == RB_CBUF || b->kind == RB_UBO_PTR) &&
            (b->dtype == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || b->dtype == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER)) return 1;
    }
    return 0;
}
static int fg_extract_camera_equal(const float a[84], const float b[84]) {
    /* Other banks in this block can change with a material. Only the camera
       fields consumed by prediction are camera identity. */
    return !memcmp(a,b,58*sizeof(float)) && a[62] == b[62] && a[70] == b[70] &&
        a[72] == b[72] && a[73] == b[73];
}
static void fg_extract_camera(vk_shader *const sh[6]) {
    if (FGX.captured || !FGX.pass.depth_write || FGX.pass.group < 0) return;
    const fg_pass_key *key = &FGX.passes.group[FGX.pass.group].key;
    const p_bind_buf *u = &S.ubo[0][1];
    FGX.pass.camera_check="no-declared-camera";
    if (!key->depth.key.backing) return;
    int standard=fg_extract_reads_camera(sh[0]) && u->addr && u->size==2368;
    unsigned layout=sh[0] ? sh[0]->framegen_camera_layout : 0;
    if (!standard && !layout) return;
    int proved_plane=sh[0] && sh[1] && sh[0]->framegen_plane && sh[1]->framegen_plane;
    int experimental_plane=sh[0] && sh[1] && sh[0]->framegen_uncertified_plane &&
        sh[1]->framegen_plane && vk_fg_uncertified_plane();
    int plane=proved_plane || experimental_plane;
    if (sh[1] && sh[1]->writes_depth && !sh[1]->framegen_raster_depth && !plane) {
        FGX.pass.camera_check="fragment-depth-export"; return;
    }
    if (key->depth.width != R.w || key->depth.height != R.h || key->depth.samples != 1 ||
        key->depth.vk_format != VK_FORMAT_D32_SFLOAT || !S.has_vp ||
        fabsf(S.vp[0][0]) > 0.5f || fabsf(S.vp[0][1]) > 0.5f ||
        fabsf(fabsf(S.vp[0][2]) - R.w) > 1 || fabsf(fabsf(S.vp[0][3]) - R.h) > 1) {
        FGX.pass.camera_check="unsupported-depth-viewport";
        if (FGX.pass.root >= 0) fg_extract_reject(VK_FG_STRUCTURE_CHANGED,"scene camera source or viewport no longer accepted");
        return;
    }
    if (S.dr[0]!=0 || S.dr[1]!=1) { FGX.pass.camera_check="unsupported-depth-range"; return; }
    FGX.pass.camera_fragment_addr=0;
    if (plane) {
        /* The audited e87/ecbe pair exports the farther of raster depth and
           the SAME camera's projection of the view ray's world-Y=4 intersection.
           A shader name nominates this proof; it never bypasses runtime checks. */
        float camera[84]; vk_ub_read(u->addr,camera,sizeof camera);
        float gate=NAN,fragment_a=NAN;
        const char *why="plane-camera-binding"; int root=-1;
        const p_bind_buf *fragment=&S.ubo[1][1], *control=&S.ubo[0][11];
        FGX.pass.camera_addr=u->addr; FGX.pass.camera_size=sizeof camera;
        FGX.pass.camera_aux_addr=0;
        do {
            if (!standard || !fragment->addr || fragment->size!=2368 ||
                !control->addr || control->size<2*sizeof(float)) break;
            FGX.pass.camera_fragment_addr=fragment->addr;
            FGX.pass.camera_aux_addr=control->addr+sizeof(float);
            vk_ub_read(FGX.pass.camera_aux_addr,&gate,sizeof gate);
            why="plane-clip-shrink-active";
            if (!isfinite(gate) || (gate!=0 && !experimental_plane)) break;
            if (gate==0 && !proved_plane) { why="uncertified-plane-inactive"; break; }
            why="plane-projection-coefficients";
            if (!fg_math_plane_projection(camera)) break;
            float storage[84]; const float *fragment_camera=camera;
            if (fragment->addr!=u->addr) {
                vk_ub_read(fragment->addr,storage,sizeof storage); fragment_camera=storage;
            }
            fragment_a=fragment_camera[65];
            why="plane-stage-camera-mismatch";
            if (!fg_extract_camera_equal(camera,fragment_camera) ||
                camera[60]!=fragment_camera[60] || camera[65]!=fragment_camera[65]) break;
            why="plane-live-camera-mismatch";
            for (unsigned i=0;i<FGX.nscene;i++) if (FGX.scene[i].valid &&
                fg_extract_same_key(FGX.scene[i].depth_key,key->depth.key) &&
                fg_extract_camera_equal(camera,FGX.scene[i].camera) &&
                camera[60]==FGX.scene[i].camera[60] && camera[65]==FGX.scene[i].camera[65]) {
                root=(int)i; break;
            }
            if (root<0) break; /* cannot introduce a root through a plane fill */
            if (FGX.pass.root>=0 && FGX.pass.root!=root) {
                fg_extract_reject(VK_FG_STRUCTURE_CHANGED,"plane camera changed within a physical pass");
                why="plane-pass-camera-mismatch"; break;
            }
            FGX.pass.root=root;
            if (gate!=0) {
                /* Opt-in assumption ONLY: keep the ordinary unknown-depth
                   baseline and require isolated before/after value attribution. */
                FGX.pass.uncertified_context=1;
                why="UNCERTIFIED-active-plane-candidate";
            } else {
                FGX.pass.camera_context=1;
                why="accepted-world-plane-camera";
            }
        } while (0);
        FGX.pass.camera_check=why;
        if (FGX.diag) {
            static uint64_t reported[2]={UINT64_MAX,UINT64_MAX};
            unsigned accepted=FGX.pass.camera_context!=0;
            if (reported[accepted]!=FGX.frame) {
                reported[accepted]=FGX.frame;
                LOGV("[framegen-plane] real %llu result=%s root=%d shrink-scale=%.9g A=%.9g FS-A=%.9g projection-z=%.9g,%.9g near=%.9g world-Y=4 vs=%.80s fs=%.80s",
                    (unsigned long long)FGX.frame,why,root,(double)gate,(double)camera[65],
                    (double)fragment_a,(double)camera[38],(double)camera[39],(double)camera[56],
                    sh[0]->key,sh[1]->key);
            }
        }
        return;
    }
    if (!standard) {
        const p_bind_buf *alternate=&S.ubo[0][5], *material=&S.ubo[0][6];
        FGX.pass.camera_check="alternate-camera-binding";
        if (!alternate->addr || alternate->size<32*sizeof(float)) return;
        FGX.pass.camera_addr=alternate->addr; FGX.pass.camera_size=32*sizeof(float);
        FGX.pass.camera_aux_addr=0;
        float matrices[32]; vk_ub_read(alternate->addr,matrices,sizeof matrices);
        if (layout==2) {
            if (!material->addr || material->size<68*sizeof(float)) return;
            FGX.pass.camera_aux_addr=material->addr+67*sizeof(float);
            float bias; vk_ub_read(material->addr+67*sizeof(float),&bias,sizeof bias);
            if (bias!=0) { FGX.pass.camera_check="alternate-camera-depth-bias"; return; }
        }
        FGX.pass.camera_check="alternate-camera-mismatch";
        for (unsigned i=0;i<FGX.nscene;i++) {
            const fg_extract_scene *scene=&FGX.scene[i];
            if (!scene->valid || !fg_extract_same_key(scene->depth_key,key->depth.key) ||
                memcmp(matrices,scene->camera,12*sizeof(float)) ||
                matrices[12]!=0 || matrices[13]!=0 || matrices[14]!=0 || matrices[15]!=1 ||
                memcmp(matrices+16,scene->camera+28,16*sizeof(float))) continue;
            if (FGX.pass.root>=0 && FGX.pass.root!=(int)i) {
                fg_extract_reject(VK_FG_STRUCTURE_CHANGED,"alternate scene camera changed within a physical pass");
                return;
            }
            FGX.pass.root=(int)i; FGX.pass.camera_context=1;
            FGX.pass.camera_check="accepted-alternate-view-projection";
            return;
        }
        if (FGX.pass.root>=0)
            fg_extract_reject(VK_FG_STRUCTURE_CHANGED,"alternate camera mismatch within a physical pass");
        return; /* alternate layout can continue a proven root, never invent one */
    }
    FGX.pass.camera_addr = u->addr;
    FGX.pass.camera_size=84*sizeof(float); FGX.pass.camera_aux_addr=0;
    float camera[84]; vk_ub_read(u->addr, camera, sizeof camera);
    for (unsigned i = 0; i < FGX.nscene; i++) if (FGX.scene[i].valid &&
        fg_extract_same_key(FGX.scene[i].depth_key, key->depth.key) &&
        fg_extract_camera_equal(camera, FGX.scene[i].camera)) {
        FGX.pass.root = (int)i; FGX.pass.camera_context = 1;
        FGX.pass.camera_check="accepted-standard-camera"; return;
    }
    if (FGX.pass.root >= 0) {
        fg_extract_reject(VK_FG_STRUCTURE_CHANGED,"accepted scene camera changed within a physical pass"); return;
    }
    if (fg_pass_color_count(key) < 2 || fg_math_camera(camera,R.w,R.h) != FG_MATH_OK) return;
    for (unsigned i = 0; i < 8; i++) if (key->color[i].key.backing &&
        (key->color[i].width != R.w || key->color[i].height != R.h || key->color[i].samples != 1)) return;
    fg_dep_mask bit = fg_dep_candidate(&FGX.deps, 1);
    if (!bit) return;
    unsigned index = FGX.nscene++;
    fg_extract_scene *c = &FGX.scene[index];
    *c = (fg_extract_scene){.depth_desc = *S.rt.depth, .depth_key = key->depth.key,
        .group = (unsigned)FGX.pass.group, .signature = fg_pass_scene_signature(key), .valid = 1};
    if (FGX.diag) c->diag.admission_pass=FGX.passes.physical_passes;
    if (FGX.diag) FGX.scene_depth_image[index]=vk_tex_canvas_identity(R.dtex,key->depth.key.mip,key->depth.key.layer);
    memcpy(c->camera, camera, sizeof c->camera);
    if (vk_fg_dump_requested()) {
        memcpy(c->admitted_viewport,S.vp[0],sizeof c->admitted_viewport);
        c->admitted_scissor[0]=S.sc.x; c->admitted_scissor[1]=S.sc.y;
        c->admitted_scissor[2]=S.sc.w; c->admitted_scissor[3]=S.sc.h;
    }
    FGX.pass.root = (int)index; FGX.pass.camera_context = 1;
}
static void fg_extract_sample(fg_dep_sources *sources, const vk_treg *r, int target) {
    vk_tex *x = vk_tex_get(&r->t);
    if (!x) { sources->value.tainted |= FG_DEP_TAINT_IDENTITY; FGX.diag_window.missing++; return; }
    unsigned mip = r->has_v && r->v.set_levels ? r->v.base_level : 0;
    unsigned layer = r->has_v && r->v.set_layers ? r->v.min_layer : 0;
    unsigned levels = r->has_v && r->v.set_levels && r->v.nlevels ? r->v.nlevels : vk_tex_levels(x)-MINV(mip,vk_tex_levels(x));
    unsigned layers = r->has_v && r->v.set_layers && r->v.nlayers ? r->v.nlayers : vk_tex_layers(x)-MINV(layer,vk_tex_layers(x));
    if (vk_tex_is3d(x)) { layer=0; layers=1; }
    else if (target == 1) layers = 1;
    else if (target == 8 && (r->has_v && r->v.set_target ? r->v.target : r->t.d.target) == 9) layers=6;
    if (!levels || !layers || mip >= vk_tex_levels(x) || levels > vk_tex_levels(x)-mip ||
        layer >= vk_tex_layers(x) || layers > vk_tex_layers(x)-layer) {
        sources->value.tainted |= FG_DEP_TAINT_VIEW; FGX.diag_window.bounds++; return;
    }
    unsigned aspect = vk_tex_aspect(x);
    if (aspect & VK_IMAGE_ASPECT_DEPTH_BIT) aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
    unsigned work=fg_dep_sample_work(&FGX.deps,levels,layers);
    if (FGX.diag) {
        FGX.diag_window.binding_view[FGX.binding_class]+=work;
        FGX.diag_window.binding_old_view[FGX.binding_class]+=
            levels == 1 && layers == 1 ? 1 : FGX.deps.count;
    }
    if (!fg_pass_take_bindings(&FGX.passes,work)) {
        if (FGX.diag) FGX.diag_window.binding_overflows[FGX.binding_class]++;
        sources->value.tainted |= FG_DEP_TAINT_UNKNOWN; return;
    }
    (void)fg_dep_sample_view(&FGX.deps,sources,fg_extract_image_key(x,mip,layer,aspect).backing,
        mip,levels,layer,layers,aspect);
}
static void fg_extract_storage_buffers(const vk_shader *shader, unsigned stage, fg_dep_sources *sources) {
    if (!FGX.nscene || !shader || !shader->has_storage_writes) return;
    for (int i = 0; i < shader->nb; i++) {
        const vk_bind *b = &shader->b[i];
        if (!b->rw) continue;
        if (b->dtype == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) {
            if (stage == 5) continue; /* compute outputs are versioned by fg_extract_compute */
            uint64_t handle = b->reg < 0 ? 0 : b->kind == RB_IMG ?
                (b->reg < 32 ? S.img[stage][b->reg] : 0) : (b->reg < 128 ? S.tex[stage][b->reg] : 0);
            vk_treg r;
            if (!handle || !vk_treg_get((int)(handle & 0xfffff),&r)) continue; /* actual dummy */
            vk_tex *x=vk_tex_get(&r.t);
            if (!x) continue;
            uint64_t backing=fg_extract_image_key(x,0,0,vk_tex_aspect(x)).backing;
            for (unsigned w=0;w<FGX.nwatch;w++) if (FGX.watch[w].key.backing == backing) {
                fg_extract_invalidate(FGX.watch[w].key,FG_PASS_WRITER_OPERATION | ++FGX.operation,
                    &FGX.watch_image[w],"graphics-storage-image-write");
                fg_extract_revoke(FGX.watch[w].key,"graphics-storage-image-write",FG_PASS_WRITER_OPERATION|FGX.operation);
                sources->value.tainted |= FG_DEP_TAINT_STORAGE;
                FGX.diag_window.image_writes++; FGX.diag_window.storage++;
                if (FGX.captured && (backing == FGX.canvas_key.backing || backing == FGX.ui_depth_key.backing))
                    fg_extract_reject(VK_FG_STRUCTURE_CHANGED,"graphics storage image modifies captured canvas/depth");
            }
            continue;
        }
        if (b->dtype != VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) continue;
        uint64_t address = 0, size = 0;
        if (b->reg < 0 || b->reg >= 16) { fg_dep_fail(&FGX.deps); continue; }
        if (b->kind == RB_SBUF) { address=S.ssbo[stage][b->reg].addr; size=S.ssbo[stage][b->reg].size; }
        else if (b->kind == RB_CBUF || b->kind == RB_UBO_PTR) { address=S.ubo[stage][b->reg].addr; size=S.ubo[stage][b->reg].size; }
        else if (b->kind == RB_SBUF_UBO) {
            const p_bind_buf *u=&S.ubo[stage][b->reg];
            if (u->addr && u->size >= (uint64_t)b->off+8) vk_ub_read(u->addr+b->off,&address,8);
            else fg_dep_fail(&FGX.deps);
        } else fg_dep_fail(&FGX.deps);
        unsigned before=FGX.operation;
        vk_fg_extract_memory_write(address,size);
        if (FGX.operation != before) { sources->value.tainted |= FG_DEP_TAINT_STORAGE; FGX.diag_window.storage++; }
    }
}
static void fg_extract_reads(vk_shader *const sh[6], unsigned first, unsigned end, fg_dep_sources *sources) {
    if (!FGX.nscene) return;
    unsigned before=FGX.passes.bindings_used;
    uint64_t start=0;
    if (FGX.diag) {
        FGX.binding_class=first == 5 ? 2 : FGX.pass.depth_write ? 1 : 0;
        FGX.diag_window.binding_calls[FGX.binding_class]++;
        start=fg_extract_cpu_ns();
    }
    for (unsigned st = first; st < end; st++) {
        if (!sh[st]) continue;
        if (FGX.diag && sh[st]->nb > 0)
            FGX.diag_window.binding_reflected[FGX.binding_class]+=(unsigned)sh[st]->nb;
        if (sh[st]->nb < 0 || !fg_pass_take_bindings(&FGX.passes, (unsigned)sh[st]->nb)) {
            if (FGX.diag) FGX.diag_window.binding_overflows[FGX.binding_class]++;
            sources->value.tainted = 1; goto done;
        }
        for (int i = 0; i < sh[st]->nb; i++) {
            const vk_bind *b = &sh[st]->b[i];
            if (b->dtype != VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER &&
                b->dtype != VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE && b->dtype != VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) continue;
            if (b->rw) continue;
            uint64_t handle = b->reg < 0 ? 0 : b->kind == RB_IMG ?
                (b->reg < 32 ? S.img[st][b->reg] : 0) : (b->reg < 128 ? S.tex[st][b->reg] : 0);
            vk_treg r;
            /* Match the real emitter's deterministic dummy for an unbound
               reflected slot; it cannot supply a scene dependency. */
            if (!handle) { FGX.diag_window.dummy++; continue; }
            if (!vk_treg_get((int)(handle & 0xfffff), &r)) {
                sources->value.tainted |= FG_DEP_TAINT_IDENTITY; FGX.diag_window.missing++; continue;
            }
            fg_extract_sample(sources, &r, b->target);
        }
    }
done:
    if (FGX.diag) {
        uint64_t end=fg_extract_cpu_ns(), elapsed=end >= start ? end-start : 0;
        FGX.diag_window.binding_cpu_ns+=elapsed;
        if (elapsed>FGX.diag_window.binding_cpu_max) FGX.diag_window.binding_cpu_max=elapsed;
        unsigned group=first == 5 || FGX.pass.group < 0 ?
            FG_PASS_MAX_GROUPS : (unsigned)FGX.pass.group;
        unsigned work=FGX.binding_group_work[group]+=FGX.passes.bindings_used-before;
        if (work>FGX.diag_window.binding_top_work) {
            FGX.diag_window.binding_top_work=work;
            FGX.diag_window.binding_top_frame=FGX.frame;
            FGX.diag_window.binding_top_group=group == FG_PASS_MAX_GROUPS ? -1 : (int)group;
            const fg_pass_key *key=group == FG_PASS_MAX_GROUPS ? NULL : &FGX.passes.group[group].key;
            FGX.diag_window.binding_top_signature=key ? fg_pass_scene_signature(key) : 0;
            FGX.diag_window.binding_top_width=key ? key->width : 0;
            FGX.diag_window.binding_top_height=key ? key->height : 0;
        }
    }
}
static void fg_extract_snapshot_state(const char *check, fg_dep_result result, fg_dep_value value) {
    if (!FGX.diag || (FGX.last_snapshot_check && !strcmp(check,FGX.last_snapshot_check) &&
        FGX.last_snapshot_result == (unsigned)result && FGX.last_snapshot_taint == value.tainted)) return;
    LOGV("[framegen-extract] real %llu snapshot check=%s result=%s roots=%u origins=%#llx eligible=%#llx unresolved=%#llx taint=%#x (unknown=1 identity=2 view=4 storage=8 clear=16 mapping=32) tracking-flags=%#x",
        (unsigned long long)FGX.frame,check,fg_dep_reason(result),FGX.nscene,
        (unsigned long long)value.origins,(unsigned long long)value.eligible,
        (unsigned long long)value.unresolved,value.tainted,FGX.passes.failed);
    FGX.last_snapshot_check=check; FGX.last_snapshot_result=result; FGX.last_snapshot_taint=value.tainted;
}
static int g_fg_job_trace; /* tentative; defined with the presenter state below */
static void fg_extract_capture(vk_tex *canvas, unsigned level, unsigned layer) {
    if (R.open) fg_extract_publish_pass(); /* logical checkpoint, not a GMEM store/reload */
    fg_dep_value before = fg_dep_read(&FGX.deps,
        fg_extract_image_key(canvas, level, layer, VK_IMAGE_ASPECT_COLOR_BIT));
    unsigned root;
    fg_dep_result result = fg_dep_select(&FGX.deps, before, &root);
    FGX.diag_window.value = before; FGX.diag_window.result = result; FGX.diag_window.root_count = FGX.nscene;
    if (result != FG_DEP_SELECTED || FGX.passes.failed) {
        fg_extract_refusal_resources(canvas,level,layer,before);
        fg_extract_snapshot_state("canvas-ancestry",result,before);
        fg_extract_reject(result == FG_DEP_FAILED || FGX.passes.failed ? VK_FG_TRACK_CAPACITY : VK_FG_CHAIN_BROKEN,
            fg_dep_reason(result)); return;
    }
    fg_extract_scene *c = &FGX.scene[root];
    vk_tex *depth = vk_tex_get(&c->depth_desc);
    if (!c->valid || !depth || !fg_extract_same_key(c->depth_key,
        fg_extract_image_key(depth,c->depth_key.mip,c->depth_key.layer,VK_IMAGE_ASPECT_DEPTH_BIT))) {
        fg_extract_snapshot_state("live-depth-incarnation",result,before);
        fg_extract_reject(VK_FG_DEPTH_LOST,"scene depth no longer live"); return;
    }
    if (FGX.authority_ready && root!=FGX.authority_root) {
        fg_extract_reject(VK_FG_DEPTH_LOST,"selected camera differs from depth certificate"); return;
    }
    vk_end_rendering_at(VK_PASS_END_COPY,0); /* proof succeeded; only now split for the actual snapshot */
    FGX.selected = root; FGX.captured = 1;
    FGX.canvas_key = fg_extract_image_key(canvas,level,layer,VK_IMAGE_ASPECT_COLOR_BIT);
    if (FGX.signature == c->signature) FGX.stable_frames++;
    else {
        if (g_fg_job_trace)
            LOGV("[adaptive-scene] event=history-cut reason=scene-signature extract_frame=%llu now_ns=%llu old_signature=0x%016llx new_signature=0x%016llx old_stable_frames=%llu",
                (unsigned long long)FGX.frame,(unsigned long long)host_now_ns(),
                (unsigned long long)FGX.signature,(unsigned long long)c->signature,(unsigned long long)FGX.stable_frames);
        FGX.signature = c->signature; FGX.stable_frames = 1; vk_fg_history_cut();
    }
    vk_fg_camera(c->camera);
    VkCommandBuffer cb = vk_cb();
    if (vk_gpuprof_on) vk_gpuprof_scope_begin(cb, "framegen");
    if (!vk_fg_capture(cb,canvas,level,layer,depth,c->depth_key.mip,c->depth_key.layer,
        vk_tex_width(canvas,level),vk_tex_height(canvas,level))) {
        fg_extract_snapshot_state("copy-format-ring-resources",result,before);
        fg_extract_reject(VK_FG_STRUCTURE_CHANGED,"generation source format, ring or GPU resources unavailable");
    } else {
        FGX.snapshot_ok = 1;
        vk_fg_capture_geometry(FGX.frame,c->admitted_viewport,c->admitted_scissor);
        fg_extract_snapshot_state("captured",result,before);
    }
    if (vk_gpuprof_on) vk_gpuprof_scope_end(cb);
    g_bound_pipe = VK_NULL_HANDLE; g_dyn_dirty = g_pipe_dirty = 1;
    descriptor_unbind(); vk_stage_sig_invalidate(0x3fu);
}
static void fg_extract_boundary(vk_tex *canvas, unsigned level, unsigned layer) {
    fg_extract_capture(canvas,level,layer);
    FGX.ui_seen = 1;
    if (!FGX.snapshot_ok) return;
    FGX.ui_depth = S.rt.depth ? vk_tex_get(S.rt.depth) : NULL;
    FGX.ui_depth_level = view_level(&S.rt.dview,S.rt.has_dview);
    FGX.ui_depth_layer = S.rt.has_dview && S.rt.dview.set_layers ? S.rt.dview.min_layer : 0;
    FGX.ui_depth_key = fg_extract_image_key(FGX.ui_depth,FGX.ui_depth_level,FGX.ui_depth_layer,
        FGX.ui_depth ? vk_tex_aspect(FGX.ui_depth) : 0);
    FGX.ui_depth_address = S.rt.depth ? (uint64_t)(uintptr_t)S.rt.depth->mem : 0;
    FGX.ui_depth_size = S.rt.depth ? S.rt.depth->L.size : 0;
}
#ifdef TOTK_VK_TESTS
/* Resolve only explicitly registered, fixture-owned programs. Real walker
 * geometry/binding/native attachment checks and emitted packet freezing remain
 * untouched; production builds contain neither this registry nor this hook. */
int vk_fg_test_canvas_programs(prog_t *ui,prog_t *terminal) {
    static vk_shader *vertex,*fragment;
    static uint8_t old_ui,old_transfer;
    if (vertex) vertex->canvas_ui=old_ui;
    if (fragment) fragment->canvas_transfer=old_transfer;
    vertex=fragment=NULL;
    (void)vk_canvas_test_shaders(NULL,NULL);
    if (!ui && !terminal) return 1;
    vk_shader *u[6]={0},*t[6]={0};
    if (!ui || !terminal || vk_prog_resolve(ui,u) || vk_prog_resolve(terminal,t) ||
        !u[0] || !t[1] || !vk_canvas_test_shaders(u[0]->key,t[1]->key)) return 0;
    vertex=u[0]; fragment=t[1];
    old_ui=vertex->canvas_ui; old_transfer=fragment->canvas_transfer;
    vertex->canvas_ui=2; fragment->canvas_transfer=1;
    return 1;
}
void vk_fg_test_alias_lineage(const tex_t *root, const tex_t *other, unsigned results[3]) {
    fg_extract_refresh(0);
    fg_dep_reset(&FGX.deps); FGX.nwatch = FGX.nscene = 0;
    FGX.nunknown=FGX.unknown_overflow=0;
    fg_dep_key a=fg_extract_image_key(vk_tex_get(root),0,0,VK_IMAGE_ASPECT_COLOR_BIT);
    fg_dep_key b=fg_extract_image_key(vk_tex_get(other),0,0,VK_IMAGE_ASPECT_COLOR_BIT);
    fg_extract_publish(a,root,NULL,1,fg_dep_candidate(&FGX.deps,1));
    results[0]=fg_dep_select(&FGX.deps,fg_dep_read(&FGX.deps,a),NULL);
    fg_extract_publish(b,other,NULL,2,0);
    results[1]=fg_dep_select(&FGX.deps,fg_dep_read(&FGX.deps,a),NULL);
    vk_fg_extract_memory_write((uint64_t)(uintptr_t)root->mem,root->L.size);
    results[2]=fg_dep_select(&FGX.deps,fg_dep_read(&FGX.deps,a),NULL);
    if (FGX.diag) fg_extract_refusal_resources(vk_tex_get(root),0,0,fg_dep_read(&FGX.deps,a));
    FGX.nwatch=0; fg_dep_reset(&FGX.deps);
}
unsigned vk_fg_test_nominate_canvas(vk_tex *canvas) {
    fg_extract_boundary(canvas,0,0);
    return !!FGX.captured | (!!FGX.invalid << 1) | (!!R.open << 2) |
        (!!fg_extract_inline() << 3) | (!!vk_fg_extract_active << 4);
}
void vk_fg_test_scene_begin(const tex_t *scene, const tex_t *depth, const float camera[84]) {
    vk_end_rendering();
    fg_dep_reset(&FGX.deps); fg_pass_reset(&FGX.passes);
    FGX.nscene=FGX.nwatch=FGX.operation=FGX.nunknown=FGX.unknown_overflow=0;
    FGX.invalid=FGX.captured=FGX.snapshot_ok=FGX.eligible=0; FGX.probe.wait=0;
    FGX.authority_ready=FGX.authority_gpu_ready=0; FGX.authority_key=(fg_dep_key){0};
    fg_extract_refresh(0);
    vk_tex *color=vk_tex_get(scene), *z=vk_tex_get(depth);
    fg_dep_key ck=fg_extract_image_key(color,0,0,VK_IMAGE_ASPECT_COLOR_BIT);
    fg_dep_key dk=fg_extract_image_key(z,0,0,VK_IMAGE_ASPECT_DEPTH_BIT);
    fg_pass_key key={.width=vk_tex_width(color,0),.height=vk_tex_height(color,0)};
    key.color[0]=(fg_pass_attachment){.key=ck,.vk_format=vk_tex_format(color),
        .width=key.width,.height=key.height,.samples=1};
    key.depth=(fg_pass_attachment){.key=dk,.vk_format=vk_tex_format(z),
        .width=vk_tex_width(z,0),.height=vk_tex_height(z,0),.samples=1};
    int group=fg_pass_begin(&FGX.passes,&key);
    FGX.scene[0]=(fg_extract_scene){.depth_desc=*depth,.depth_key=dk,.valid=1,
        .group=(unsigned)group,.signature=fg_pass_scene_signature(&key)};
    memcpy(FGX.scene[0].camera,camera,sizeof FGX.scene[0].camera);
    if (FGX.diag) FGX.scene_depth_image[0]=vk_tex_canvas_identity(z,0,0);
    FGX.nscene=1;
    fg_dep_mask bit=fg_dep_candidate(&FGX.deps,1);
    fg_extract_publish(ck,scene,NULL,1,bit);
    fg_extract_publish(dk,depth,NULL,1,bit);
    fg_pass_end(&FGX.passes,group,1,1);
}
int vk_fg_test_capture_reads(const tex_t *canvas, const vk_treg *inputs, unsigned count) {
    vk_end_rendering();
    vk_tex *x=vk_tex_get(canvas);
    fg_dep_key key=fg_extract_image_key(x,0,0,VK_IMAGE_ASPECT_COLOR_BIT);
    fg_dep_sources sources; fg_dep_sources_init(&sources);
    fg_dep_sources_add(&FGX.deps,&sources,fg_dep_read(&FGX.deps,key));
    for (unsigned i=0;i<count;i++) fg_extract_sample(&sources,&inputs[i],1);
    fg_extract_publish(key,canvas,&sources,FG_PASS_WRITER_OPERATION|++FGX.operation,0);
    fg_extract_capture(x,0,0);
    FGX.eligible=FGX.snapshot_ok && !FGX.invalid;
    return FGX.eligible;
}
#endif
static void fg_extract_draw_context(void *context, unsigned event) {
    (void)event;
    vk_shader *const *sh = context;
    fg_extract_span();
    fg_dep_sources_init(&FGX.pass.context);
    FGX.pass.camera_context = 0;
    FGX.pass.authority_context=0;
    FGX.pass.uncertified_context=0;
    if (FGX.diag) {
        FGX.pass.plane_context=sh[1] && sh[1]->framegen_plane;
        FGX.pass.viewport_pixels=(uint64_t)R.w*R.h;
        int64_t x0=0,y0=0,x1=R.w,y1=R.h;
        if (S.has_sc) {
            x0=MAXV(x0,(int64_t)S.sc.x); y0=MAXV(y0,(int64_t)S.sc.y);
            x1=MINV(x1,(int64_t)S.sc.x+S.sc.w); y1=MINV(y1,(int64_t)S.sc.y+S.sc.h);
        }
        FGX.pass.scissor_pixels=x1>x0 && y1>y0 ? (uint64_t)(x1-x0)*(uint64_t)(y1-y0) : 0;
    }
    /* Admission must use this context's depth state, not the previous shader
       context's cached flag. In-pass depth-off/on transitions are legal. */
    FGX.pass.depth_write = R.dfmt != VK_FORMAT_UNDEFINED && S.ds.dt && S.ds.dw;
    if (FGX.diag) {
        FGX.pass.vs=sh[0]->key; FGX.pass.fs=sh[1] ? sh[1]->key : NULL;
        FGX.pass.camera_declared=fg_extract_reads_camera(sh[0]);
    }
    fg_extract_camera(sh);
#ifdef TOTK_VK_TESTS
    if (fg_test_writer_classification>=0) {
        unsigned classification=(unsigned)fg_test_writer_classification;
        FGX.pass.camera_context=classification==VK_FG_WRITER_PROVEN;
        FGX.pass.uncertified_context=classification==VK_FG_WRITER_UNCERTIFIED && vk_fg_uncertified_plane();
        FGX.pass.camera_check=FGX.pass.camera_context ? "fixture-proven-camera" : "fixture-unproven-camera";
        unsigned root=FGX.authority_ready ? FGX.authority_root : 0;
        if (FGX.pass.camera_context && root<FGX.nscene && FGX.scene[root].valid &&
            fg_extract_same_key(FGX.scene[root].depth_key,FGX.pass.key[8]))
            FGX.pass.root=(int)root;
    }
#endif
    if (!FGX.pass.camera_context && FGX.pass.depth_write && FGX.authority_ready &&
        FGX.authority_root<FGX.nscene && FGX.scene[FGX.authority_root].valid &&
        fg_extract_same_key(FGX.pass.key[8],FGX.authority_key)) {
        FGX.pass.root=(int)FGX.authority_root;
        FGX.pass.authority_context=1;
    }
    if (FGX.invalid) return;
    int ui = fg_pass_ui_context(FGX.ui_seen,sh[0]->canvas_ui,
        fg_extract_canvas(R.ctex[0],R.clevel[0],R.clayer[0]));
    unsigned outputs = sh[1] ? sh[1]->fout_mask : 0;
    for (unsigned i = 0; i < 8; i++) if (!(S.chmask.mask >> (4 * i) & 15)) outputs &= ~(1u << i);
    FGX.pass.context_outputs = outputs;
    if (FGX.pass.root < 0 && !ui) fg_extract_reads(sh, 0, 2, &FGX.pass.context);
    /* Extent-derived normalized scaling only models full, axis-aligned
       composition. A contributing cropped/offset viewport is not that map.
       UI and terminal transfer have their own separately frozen coordinates. */
    if (!FGX.captured && FGX.pass.context.value.origins && outputs &&
        (!S.has_vp || fabsf(S.vp[0][0]) > 0.5f || fabsf(S.vp[0][1]) > 0.5f ||
         fabsf(S.vp[0][2]-R.w) > 0.5f || fabsf(S.vp[0][3]-R.h) > 0.5f))
        FGX.pass.context.value.tainted |= FG_DEP_TAINT_MAPPING;
    fg_extract_storage_buffers(sh[0],0,&FGX.pass.context);
    fg_extract_storage_buffers(sh[1],1,&FGX.pass.context);
    if (FGX.captured) {
        int canvas = 0;
        for (unsigned i = 0; i < R.ncolor; i++) if (fg_extract_canvas(R.ctex[i],R.clevel[i],R.clayer[i])) {
            canvas = 1;
            if (!ui || i) fg_extract_reject(VK_FG_STRUCTURE_CHANGED,"non-UI work after scene/UI boundary");
        }
        if (!canvas && FGX.pass.depth_write && R.dtex && fg_extract_ui_depth(R.dtex,R.dlevel,R.dlayer))
            fg_extract_reject(VK_FG_STRUCTURE_CHANGED,"unrecorded work modifies captured UI depth");
    }
}
/* Identify the unique existing camera root logically. GPU reservation and the
   first clipped before-copy share ONE later physical boundary; reserving here
   would manufacture an empty store/reload pass before the first unknown draw. */
static int fg_extract_authority_before_draw(vk_shader *const sh[6]) {
    if (!vk_fg_extract_active || FGX.invalid || FGX.captured ||
        !FGX.pass.depth_write || FGX.pass.camera_context || FGX.pass.authority_context ||
        !vk_fg_interpolating()) return 1;
    fg_dep_key key=FGX.pass.key[8];
    int root=-1;
    for (unsigned i=0;i<FGX.nscene;i++) if (FGX.scene[i].valid &&
        fg_extract_same_key(FGX.scene[i].depth_key,key)) {
        if (root>=0) return 1; /* ambiguous camera authority: old refusal applies */
        root=(int)i;
    }
    if (root<0 || FGX.authority_ready) return 1;
    FGX.authority_ready=1; FGX.authority_root=(unsigned)root; FGX.authority_key=key;
    fg_pass_guard_invalidate(&FGX.guard);
    (void)fg_pass_route_programs(1,&FGX.passes,&FGX.guard,
        (uintptr_t)sh[0],(uintptr_t)sh[1],fg_extract_draw_context,(void *)sh);
    return 1;
}
static void fg_extract_authority_span_end(VkCommandBuffer cb) {
    if (!FGX.authority_span.open) return;
    vk_tex *depth=FGX.authority_span.depth;
    unsigned level=FGX.authority_span.level,layer=FGX.authority_span.layer;
    FGX.authority_span.open=0;
    if (FGX.invalid) vk_fg_skip();
    if (!vk_fg_authority_span_end(cb,depth,level,layer))
        fg_extract_reject(VK_FG_DEPTH_LOST,"ordered depth authority after-copy unavailable");
}
static void fg_extract_writer_end(VkCommandBuffer cb) {
    if (!FGX.writer.open) return;
    FGX.writer.open=0;
    if (FGX.invalid || !vk_fg_extract_active) {
        vk_fg_writer_poison("extractor rejected with pending writer span");
        return;
    }
    (void)vk_fg_writer_end(cb,FGX.writer.depth,FGX.writer.level,FGX.writer.layer,
        g_stat_draws-FGX.writer.draw0);
}
static int fg_extract_depth_spans_before_draw(vk_shader *const sh[6]) {
    /* Track VALUE changes only inside unproven raster spans. A clipped scissor
       is an upper bound on writes, NEVER evidence every covered pixel was hit.
       Proven draws between spans are observed by the next before-copy or final
       copy, not compared against an obsolete first-unknown snapshot. */
    int writing=R.dfmt!=VK_FORMAT_UNDEFINED && S.ds.dt && S.ds.dw;
    int target=writing && vk_fg_extract_active && !FGX.invalid && !FGX.captured &&
        FGX.authority_ready && fg_extract_same_key(FGX.pass.key[8],FGX.authority_key);
    int unknown=target && !FGX.pass.camera_context && FGX.pass.authority_context;
    unsigned experimental=unknown && FGX.pass.uncertified_context;
    int64_t x0=0,y0=0,x1=R.w,y1=R.h;
    if (S.has_sc) {
        x0=MAXV(x0,(int64_t)S.sc.x); y0=MAXV(y0,(int64_t)S.sc.y);
        x1=MINV(x1,(int64_t)S.sc.x+S.sc.w); y1=MINV(y1,(int64_t)S.sc.y+S.sc.h);
    }
    /* No depth command can affect the pending span on an empty scissor or with
       writes disabled. Invalid extraction must still close/poison its state. */
    if (!FGX.invalid && (!writing || x1<=x0 || y1<=y0)) return 1;
    unsigned x=(unsigned)x0,y=(unsigned)y0,width=(unsigned)(x1-x0),height=(unsigned)(y1-y0);
    /* TOTK_FG_ADAPTIVE_LEAN (adaptive QCOM scheduler): the CPU authority
       classification above still admits this writer exactly as before, but no
       consumer reads the ordered VALUE result, so no GPU span or pass split. */
    if (unknown && vk_fg_adaptive_lean_span()) unknown=0, experimental=0;
    int same=unknown && FGX.authority_span.open && FGX.authority_span.depth==R.dtex &&
        FGX.authority_span.level==R.dlevel && FGX.authority_span.layer==R.dlayer &&
        FGX.authority_span.experimental==experimental &&
        x>=FGX.authority_span.x && y>=FGX.authority_span.y &&
        (uint64_t)x+width<=(uint64_t)FGX.authority_span.x+FGX.authority_span.width &&
        (uint64_t)y+height<=(uint64_t)FGX.authority_span.y+FGX.authority_span.height;
    unsigned classification=FGX.pass.camera_context ? VK_FG_WRITER_PROVEN :
        experimental ? VK_FG_WRITER_UNCERTIFIED : VK_FG_WRITER_UNPROVEN;
    const char *vs=sh[0] ? sh[0]->key : "-", *fs=sh[1] ? sh[1]->key : "-";
    const char *check=FGX.pass.camera_check ? FGX.pass.camera_check : "no-camera-check";
    int diagnostic=target && vk_fg_writer_diag() &&
        (FGX.authority_gpu_ready ? vk_fg_writer_available() : unknown);
    int same_writer=diagnostic && FGX.writer.open && FGX.writer.depth==R.dtex &&
        FGX.writer.level==R.dlevel && FGX.writer.layer==R.dlayer &&
        FGX.writer.classification==classification && !strcmp(FGX.writer.vs,vs) &&
        !strcmp(FGX.writer.fs,fs) && !strcmp(FGX.writer.check,check);
    int boundary=(FGX.authority_span.open && !same) || (unknown && !FGX.authority_span.open) ||
        (FGX.writer.open && !same_writer) || (diagnostic && !FGX.writer.open);
    if (!boundary) return 1;
    /* One physical boundary serves production and optional diagnostics together.
       Either hook splitting independently would create empty/stale spans. */
    vk_tex *depth=R.dtex; unsigned level=R.dlevel,layer=R.dlayer;
    vk_end_rendering_at(diagnostic ? VK_PASS_END_DIAGNOSTIC : VK_PASS_END_COPY,0);
    if (target && !FGX.invalid) {
        if (unknown) {
            if (!vk_fg_authority_span_begin(vk_cb(),depth,level,layer,x,y,width,height,experimental))
                fg_extract_reject(VK_FG_DEPTH_LOST,"ordered depth authority before-copy unavailable");
            else {
                FGX.authority_gpu_ready=1;
                FGX.authority_span.depth=depth;
                FGX.authority_span.level=level; FGX.authority_span.layer=layer;
                FGX.authority_span.x=x; FGX.authority_span.y=y;
                FGX.authority_span.width=width; FGX.authority_span.height=height;
                FGX.authority_span.experimental=experimental; FGX.authority_span.open=1;
            }
        }
        if (diagnostic && !FGX.invalid &&
            vk_fg_writer_begin(vk_cb(),depth,level,layer,vs,fs,check,classification)) {
            FGX.writer.depth=depth; FGX.writer.level=level; FGX.writer.layer=layer;
            FGX.writer.vs=vs; FGX.writer.fs=fs; FGX.writer.check=check;
            FGX.writer.classification=classification;
            FGX.writer.draw0=g_stat_draws; FGX.writer.open=1;
        }
    }
    if (!begin_rendering(1)) {
        vk_fg_writer_poison("real rendering failed to resume after ordered span copy");
        FGX.writer.open=0;
        return 0; /* real command recording failed, not a framegen-only refusal */
    }
    if (vk_fg_extract_active && !FGX.invalid)
        (void)fg_pass_route_programs(1,&FGX.passes,&FGX.guard,
            (uintptr_t)sh[0],(uintptr_t)sh[1],fg_extract_draw_context,(void *)sh);
    return 1;
}
#ifdef TOTK_VK_TESTS
int vk_fg_test_authority_draw_begin(const tex_t *scene, const tex_t *depth,
                                  const float camera[84], int seed, const VkRect2D *scissor) {
    if (seed) vk_fg_test_scene_begin(scene,depth,camera);
    FGX.test_authority_inline=1;
    p_set_rt rt={.n=1,.color={scene},.depth=depth};
    vk_draw_exec(OP_SET_RT,&rt);
    S.has_vp=1; S.vp[0][0]=S.vp[0][1]=0;
    S.vp[0][2]=vk_tex_width(vk_tex_get(depth),0);
    S.vp[0][3]=vk_tex_height(vk_tex_get(depth),0);
    S.dr[0]=0; S.dr[1]=1; S.has_sc=scissor!=NULL;
    if (scissor) { S.sc.x=scissor->offset.x; S.sc.y=scissor->offset.y;
        S.sc.w=scissor->extent.width; S.sc.h=scissor->extent.height; }
    S.ds.dt=S.ds.dw=1; S.ds.df=7; S.chmask.mask=15;
    if (!begin_rendering(1)) return 0;
    static vk_shader vertex={.key="host-unknown-camera"};
    static vk_shader fragment={.key="host-unknown-depth-export",.writes_depth=1,.fout_mask=1};
    vk_shader *sh[6]={&vertex,&fragment};
    fg_pass_guard_invalidate(&FGX.guard);
    (void)fg_pass_route_programs(1,&FGX.passes,&FGX.guard,
        (uintptr_t)sh[0],(uintptr_t)sh[1],fg_extract_draw_context,sh);
    return fg_extract_authority_before_draw(sh) && fg_extract_depth_spans_before_draw(sh) &&
        !FGX.invalid && FGX.pass.authority_context;
}
void vk_fg_test_authority_draw_end(void) {
    g_stat_draws++;
    vk_end_rendering();
    FGX.test_authority_inline=0;
}
int vk_fg_test_writer_draw_begin(const tex_t *scene, const tex_t *depth,
                                const float camera[84], int seed, unsigned identity,
                                unsigned classification, const VkRect2D *scissor) {
    if (seed) vk_fg_test_scene_begin(scene,depth,camera);
    FGX.test_authority_inline=1;
    p_set_rt rt={.n=1,.color={scene},.depth=depth}; vk_draw_exec(OP_SET_RT,&rt);
    S.has_vp=1; S.vp[0][0]=S.vp[0][1]=0;
    S.vp[0][2]=vk_tex_width(vk_tex_get(depth),0); S.vp[0][3]=vk_tex_height(vk_tex_get(depth),0);
    S.dr[0]=0; S.dr[1]=1; S.has_sc=scissor!=NULL;
    if (scissor) { S.sc.x=scissor->offset.x; S.sc.y=scissor->offset.y;
        S.sc.w=scissor->extent.width; S.sc.h=scissor->extent.height; }
    S.ds.dt=S.ds.dw=1; S.ds.df=7; S.chmask.mask=15;
    if (!begin_rendering(1)) return 0;
    static vk_shader vertex[2]={{.key="writer-fixture-vs-A"},{.key="writer-fixture-vs-B"}};
    static vk_shader fragment[2]={{.key="writer-fixture-fs-A",.writes_depth=1,.fout_mask=1},
        {.key="writer-fixture-fs-B",.writes_depth=1,.fout_mask=1}};
    vk_shader *sh[6]={&vertex[identity&1],&fragment[identity&1]};
    int saved_classification=fg_test_writer_classification;
    fg_test_writer_classification=(int)classification;
    fg_pass_guard_invalidate(&FGX.guard);
    (void)fg_pass_route_programs(1,&FGX.passes,&FGX.guard,
        (uintptr_t)sh[0],(uintptr_t)sh[1],fg_extract_draw_context,sh);
    int accepted=fg_extract_authority_before_draw(sh) && fg_extract_depth_spans_before_draw(sh) && !FGX.invalid;
    fg_test_writer_classification=saved_classification;
    return accepted;
}
void vk_fg_test_writer_draw_note(int close) {
    g_stat_draws++;
    if (close) { vk_end_rendering(); FGX.test_authority_inline=0; }
}
/* Metadata follows the exact experimental admission path; the test then emits
   a real explicit-depth Vulkan draw before note(). No guest shader is executed. */
unsigned vk_fg_test_uncertified_draw_begin(const tex_t *scene, const tex_t *depth,
                                         const float input[84], int seed, unsigned kind,
                                         const VkRect2D *scissor) {
    static float uniform[592],fragment_uniform[592],control[2];
    float camera[84]; memcpy(camera,input,sizeof camera);
    camera[65]=(1-camera[38])*0.5f;
    if (seed) vk_fg_test_scene_begin(scene,depth,camera);
    vk_fg_extract_memory_write((uint64_t)(uintptr_t)uniform,sizeof uniform);
    vk_fg_extract_memory_write((uint64_t)(uintptr_t)fragment_uniform,sizeof fragment_uniform);
    vk_fg_extract_memory_write((uint64_t)(uintptr_t)control,sizeof control);
    memset(uniform,0,sizeof uniform); memcpy(uniform,camera,sizeof camera);
    memset(fragment_uniform,0,sizeof fragment_uniform); memcpy(fragment_uniform,camera,sizeof camera);
    control[0]=0; control[1]=kind==3 ? NAN : 800;
    if (kind==2) fragment_uniform[47]+=1;
    if (kind==7) uniform[47]+=1;
    FGX.test_authority_inline=1;
    p_set_rt rt={.n=1,.color={scene},.depth=depth}; vk_draw_exec(OP_SET_RT,&rt);
    S.has_vp=1; S.vp[0][0]=S.vp[0][1]=0;
    S.vp[0][2]=vk_tex_width(vk_tex_get(depth),0); S.vp[0][3]=vk_tex_height(vk_tex_get(depth),0);
    p_depth_range range={kind==4 ? 0.2f : 0,1}; vk_draw_exec(OP_DEPTH_RANGE,&range);
    S.has_sc=scissor!=NULL;
    if (scissor) { S.sc.x=scissor->offset.x; S.sc.y=scissor->offset.y;
        S.sc.w=scissor->extent.width; S.sc.h=scissor->extent.height; }
    S.ds.dt=S.ds.dw=1; S.ds.df=7; S.chmask.mask=15;
    p_bind_buf u={.stage=0,.index=1,.addr=(uint64_t)(uintptr_t)uniform,.size=sizeof uniform};
    vk_draw_exec(OP_BIND_UBO,&u);
    u.stage=1; u.addr=(uint64_t)(uintptr_t)fragment_uniform; vk_draw_exec(OP_BIND_UBO,&u);
    u.stage=0; u.index=11; u.addr=kind==8 ? 0 : (uint64_t)(uintptr_t)control; u.size=sizeof control;
    vk_draw_exec(OP_BIND_UBO,&u);
    if (!begin_rendering(1)) return 0;
    static vk_bind camera_binding={.reg=1,.kind=RB_CBUF,.dtype=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER};
    static vk_shader vertex[3]={
        {.key="e87dbf67974d71e68f4ec4586d5c70ec6fccaf7515d54280c3a765f8b7cc8c79-b7530f698e76",.nb=1,.b=&camera_binding},
        {.key="6ec214111aa5b8bc691f63fd0214aa5d4ed08e6d433f7973c9535f4a0d00f620-b7530f698e76",.nb=1,.b=&camera_binding},
        {.key="host-unlisted-plane-vertex",.nb=1,.b=&camera_binding}};
    static vk_shader fragment[2]={
        {.key="ecbe4aa4417b3f806ef471833eac0165958c3416642d8a23987c1376dc2681ba-ef49c76e8bd9",.writes_depth=1,.fout_mask=1},
        {.key="host-unlisted-plane-fragment",.writes_depth=1,.fout_mask=1}};
    vk_shader *vs=&vertex[kind==5 ? 2 : kind==1 ? 1 : 0], *fs=&fragment[kind==6];
    vs->framegen_plane=(uint8_t)vk_fg_plane_shader(vs->key,0);
    vs->framegen_uncertified_plane=(uint8_t)vk_fg_plane_shader(vs->key,2);
    fs->framegen_plane=(uint8_t)vk_fg_plane_shader(fs->key,1);
    vk_shader *sh[6]={vs,fs};
    (void)fg_pass_route_programs(1,&FGX.passes,&FGX.guard,
        (uintptr_t)vs,(uintptr_t)fs,fg_extract_draw_context,sh);
    if (!fg_extract_authority_before_draw(sh) || !fg_extract_depth_spans_before_draw(sh)) return 0;
    return !FGX.invalid && FGX.pass.authority_context ?
        1u | ((FGX.pass.uncertified_context && FGX.authority_span.open &&
            FGX.authority_span.experimental) ? 2u : 0u) : 0;
}
void vk_fg_test_uncertified_draw_note(int close) {
    g_stat_draws++;
    if (close) { vk_end_rendering(); FGX.test_authority_inline=0; }
}
unsigned vk_fg_test_depth_contexts(const tex_t *scene, const tex_t *depth, const float camera[84]) {
    vk_fg_test_scene_begin(scene,depth,camera);
    vk_state saved=S; __typeof__(R) saved_render=R;
    memset(&R,0,sizeof R);
    R.w=vk_tex_width(vk_tex_get(depth),0); R.h=vk_tex_height(vk_tex_get(depth),0);
    R.dfmt=VK_FORMAT_D32_SFLOAT;
    S.rt.depth=depth; S.has_vp=1;
    S.vp[0][0]=S.vp[0][1]=0; S.vp[0][2]=R.w; S.vp[0][3]=R.h;
    float uniform[592]={0}; memcpy(uniform,camera,84*sizeof(float));
    S.ubo[0][1]=(p_bind_buf){.addr=(uint64_t)(uintptr_t)uniform,.size=sizeof uniform};
    vk_bind bind={.reg=1,.kind=RB_CBUF,.dtype=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER};
    vk_shader vertex={.nb=1,.b=&bind};
    vk_shader *sh[6]={&vertex};
    FGX.pass.group=(int)FGX.scene[0].group; FGX.pass.root=-1;
    FGX.pass.depth_write=0; FGX.pass.span0=g_stat_draws;
    fg_dep_sources_init(&FGX.pass.reads);
    S.ds.dt=S.ds.dw=1;
    S.dr[0]=0; S.dr[1]=1;
    fg_extract_draw_context(sh,0);
    unsigned result=FGX.pass.root==0 && FGX.pass.camera_context ? 1 : 0;
    S.ds.dw=0;
    fg_extract_draw_context(sh,0);
    if (!FGX.pass.camera_context && !FGX.pass.depth_write) result|=2;
    S=saved; R=saved_render;
    return result;
}
/* Synthetic draw spans drive the production context/pass/version tracker; no
   synthetic span is submitted to Vulkan or counted as a real renderer draw. */
int vk_fg_test_scene_continuations(const tex_t *scene, const tex_t *depth,
                                   const tex_t *canvas, const float camera[84], unsigned negative) {
    float plane_camera[84]; memcpy(plane_camera,camera,sizeof plane_camera);
    plane_camera[65]=(1.0f-plane_camera[38])*0.5f; camera=plane_camera;
    vk_fg_test_scene_begin(scene,depth,camera);
    if (negative==17) FGX.scene[0].camera[65]*=1.01f;
    vk_state saved=S; __typeof__(R) saved_render=R; uint64_t saved_draws=g_stat_draws;
    memset(&R,0,sizeof R);
    R.w=vk_tex_width(vk_tex_get(depth),0); R.h=vk_tex_height(vk_tex_get(depth),0);
    R.dfmt=VK_FORMAT_D32_SFLOAT; R.dtex=vk_tex_get(depth);
    S.rt=(p_set_rt){.n=2,.color={scene,scene},.depth=depth};
    S.has_vp=1; S.vp[0][0]=S.vp[0][1]=0; S.vp[0][2]=R.w; S.vp[0][3]=R.h;
    S.ds.dt=S.ds.dw=1; S.chmask.mask=UINT32_MAX;
    S.dr[0]=0; S.dr[1]=1;
    float uniform[592]={0}, fragment_uniform[592]={0}, matrices[32]={0}, material[68]={0}, control[2]={0};
    memcpy(uniform,camera,84*sizeof(float));
    memcpy(fragment_uniform,uniform,sizeof fragment_uniform);
    if (negative==12) control[1]=1;
    if (negative==19 || negative==20) control[1]=800; /* observed active displacement amplitude */
    if (negative==13) uniform[65]*=1.01f;
    if (negative==14) fragment_uniform[65]*=1.01f;
    memcpy(matrices,camera,12*sizeof(float)); matrices[15]=1;
    memcpy(matrices+16,camera+28,16*sizeof(float));
    if (negative==1) matrices[3]+=1;
    if (negative==2) material[67]=0.5f;
    S.ubo[0][1]=(p_bind_buf){.addr=(uint64_t)(uintptr_t)uniform,.size=sizeof uniform};
    S.ubo[0][5]=(p_bind_buf){.addr=(uint64_t)(uintptr_t)matrices,.size=sizeof matrices};
    S.ubo[0][6]=(p_bind_buf){.addr=(uint64_t)(uintptr_t)material,.size=sizeof material};
    S.ubo[0][11]=(p_bind_buf){.addr=(uint64_t)(uintptr_t)control,.size=sizeof control};
    S.ubo[1][1]=(p_bind_buf){.addr=(uint64_t)(uintptr_t)fragment_uniform,.size=sizeof fragment_uniform};
    if (negative==15) S.ubo[1][1].size=256;
    vk_bind bindings[2]={{.reg=5,.kind=RB_CBUF,.dtype=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER},
                         {.reg=6,.kind=RB_CBUF,.dtype=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER}};
    vk_bind standard_binding={.reg=1,.kind=RB_CBUF,.dtype=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER};
    vk_shader standard={.nb=1,.b=&standard_binding}, alternate={.nb=2,.b=bindings}, fragment={0};
    vk_shader plane_vertex={.nb=1,.b=&standard_binding};
    strcpy(plane_vertex.key,"e87dbf67974d71e68f4ec4586d5c70ec6fccaf7515d54280c3a765f8b7cc8c79-b7530f698e76");
    if (negative==20)
        strcpy(plane_vertex.key,"6ec214111aa5b8bc691f63fd0214aa5d4ed08e6d433f7973c9535f4a0d00f620-b7530f698e76");
    plane_vertex.framegen_plane=(uint8_t)vk_fg_plane_shader(plane_vertex.key,0);
    if (negative==16) plane_vertex.framegen_plane=0;
    strcpy(alternate.key,"3e9fe6db781f37eb22f1acb0bcd73731cb8ffadaddd45bedc5aeb8ebb353fa18-468e580cd620");
    if (negative==6) strcpy(alternate.key,"de3ac20c93c60cf782e8c55a40b89688aead03de7d67373bd4cd90c1963a95d9-466ef310f9bb");
    if (negative==7) strcpy(alternate.key,"c5b26871948ae177db2518a0204b6f718d09b9079ebb70ff865a7b5aa8688b56-1a4d008ae5b4");
    if (negative==3 || negative==8) strcpy(alternate.key,"unclassified-camera-layout");
    alternate.framegen_camera_layout=(uint8_t)vk_fg_camera_layout(alternate.key);
    for (unsigned stage=0;stage<6;stage++) {
        R.ncolor=stage==0 ? 0 : stage==1 ? 1 : stage==5 ? 8 : 2;
        S.rt.n=R.ncolor; R.draws0=g_stat_draws;
        S.has_sc=1; S.sc=(p_scissor){0,0,(int)R.w,(int)R.h};
        if (stage==1) S.sc=(p_scissor){(int)R.w/2,(int)R.h/2,16,17};
        for (unsigned i=0;i<8;i++) {
            int bound=stage==5 ? i==1 || i==3 || i==7 : i<R.ncolor;
            R.ctex[i]=bound ? vk_tex_get(scene) : NULL;
            R.cfmt[i]=bound ? vk_tex_format(R.ctex[i]) : VK_FORMAT_UNDEFINED;
            S.rt.color[i]=bound ? scene : NULL;
        }
        fragment.fout_mask=stage==0 ? 0 : stage==1 ? 1 : stage==5 ? 0x8a : 3;
        S.ds.df=stage<=1 ? NVN_DEPTH_FUNC_ALWAYS : stage==5 ? 4 : NVN_DEPTH_FUNC_GREATER;
        S.vp[0][0]=stage==3 && negative==4 ? 1 : 0;
        S.dr[0]=stage==3 && negative==9 ? 0.25f : 0;
        fragment.writes_depth=stage==1 || stage==5 || (stage==3 && negative==5);
        strcpy(fragment.key,stage==1 ?
            "c9fd30402df30801e2b9eaff4c9a444c8a67937f07b712e1c2c39955539f0e80-c32ab336b99a" :
            stage==5 ? (negative==18 ?
            "ecbe4aa4417b3f806ef471833eac0165958c3416642d8a23987c1376dc2681ba-b7530f698e76" :
            "ecbe4aa4417b3f806ef471833eac0165958c3416642d8a23987c1376dc2681ba-ef49c76e8bd9") : "unknown-fragment-depth");
        fragment.framegen_raster_depth=(uint8_t)vk_fg_raster_depth_shader(fragment.key);
        fragment.framegen_plane=(uint8_t)vk_fg_plane_shader(fragment.key,1);
        fg_extract_pass_begin(0);
        vk_shader *sh[6]={stage==3 ? &alternate : stage==5 ? &plane_vertex : &standard,&fragment};
        if (stage==3 && negative==8) {
            vk_shader *accepted[6]={&standard,&fragment};
            fg_extract_draw_context(accepted,0); g_stat_draws++;
        }
        if (stage==3 && negative==10) {
            /* An actual depth-writing draw with an unaudited FragDepth export
               precedes a later camera-accepted draw in this same physical pass. */
            fragment.writes_depth=1;
            strcpy(fragment.key,"ecbe4aa4417b3f806ef471833eac0165958c3416642d8a23987c1376dc2681ba-ef49c76e8bd9");
            fragment.framegen_raster_depth=(uint8_t)vk_fg_raster_depth_shader(fragment.key);
            fragment.framegen_plane=(uint8_t)vk_fg_plane_shader(fragment.key,1);
            vk_shader *unaccepted[6]={&standard,&fragment};
            fg_extract_draw_context(unaccepted,0); g_stat_draws++;
            fragment.writes_depth=0;
            strcpy(fragment.key,"c9fd30402df30801e2b9eaff4c9a444c8a67937f07b712e1c2c39955539f0e80-c32ab336b99a");
            fragment.framegen_raster_depth=(uint8_t)vk_fg_raster_depth_shader(fragment.key);
            fragment.framegen_plane=0;
            vk_shader *accepted[6]={&standard,&fragment};
            fg_extract_draw_context(accepted,0); g_stat_draws++;
        }
        fg_extract_draw_context(sh,0); g_stat_draws++;
        if (stage==5) {
            /* A later accepted context must not erase an earlier failed plane
               proof; conversely a proven plane span must not invent a failure. */
            strcpy(fragment.key,"c9fd30402df30801e2b9eaff4c9a444c8a67937f07b712e1c2c39955539f0e80-c32ab336b99a");
            fragment.framegen_raster_depth=1; fragment.framegen_plane=0;
            vk_shader *accepted[6]={&standard,&fragment};
            fg_extract_draw_context(accepted,0); g_stat_draws++;
        }
        fg_extract_pass_end();
    }
    fg_dep_key source=fg_extract_image_key(vk_tex_get(scene),0,0,VK_IMAGE_ASPECT_COLOR_BIT);
    fg_dep_key destination=fg_extract_image_key(vk_tex_get(canvas),0,0,VK_IMAGE_ASPECT_COLOR_BIT);
    fg_dep_sources sources; fg_dep_sources_init(&sources);
    fg_dep_sources_add(&FGX.deps,&sources,fg_dep_read(&FGX.deps,source));
    fg_extract_publish(destination,canvas,&sources,FG_PASS_WRITER_OPERATION|++FGX.operation,0);
    fg_dep_value value=fg_dep_read(&FGX.deps,destination);
    int selected=fg_dep_select(&FGX.deps,value,NULL)==FG_DEP_SELECTED && !FGX.invalid;
    if (!selected && FGX.diag) fg_extract_refusal_resources(vk_tex_get(canvas),0,0,value);
    g_stat_draws=saved_draws; FGX.pass.span0=FGX.pass.published_draws=saved_draws;
    S=saved; R=saved_render;
    return selected ? (FGX.nscene==1 ? 1 : 2) : 0;
}
#endif
/* Retain attachment clears in order with the frozen draws, without breaking
   the real tiled pass. A full color overwrite is not a scene composition. */
static int fg_extract_ui_clear(const VkClearAttachment *attachment, const VkClearRect *rect) {
    if (!FGX.captured || FGX.invalid) return 0;
    int color = attachment->aspectMask & VK_IMAGE_ASPECT_COLOR_BIT;
    if (color ? attachment->colorAttachment || !fg_extract_canvas(R.ctex[0],R.clevel[0],R.clayer[0]) :
        !R.dtex || !fg_extract_ui_depth(R.dtex,R.dlevel,R.dlayer)) return 0;
    if (rect->baseArrayLayer || rect->layerCount != 1 || R.host_w != FGX.ui_width || R.host_h != FGX.ui_height ||
        (color && rect->rect.offset.x == 0 && rect->rect.offset.y == 0 &&
         rect->rect.extent.width == R.host_w && rect->rect.extent.height == R.host_h)) {
        fg_extract_reject(VK_FG_STRUCTURE_CHANGED,"UI clear changes canvas structure"); return 0;
    }
    if (!vk_fg_canvas_view()) return 1; /* recognized warm-up operation, no private target yet */
    if (FGX.nreplay == sizeof FGX.replay_draw / sizeof FGX.replay_draw[0]) {
        fg_extract_reject(VK_FG_TRACK_CAPACITY,"UI replay packet capacity exhausted"); return 0;
    }
    fg_extract_replay_draw *packet = &FGX.replay_draw[FGX.nreplay++];
    packet->width = FGX.ui_width; packet->height = FGX.ui_height; packet->format = FGX.ui_format;
    packet->final = 0; packet->clear = 1; packet->attachment = *attachment; packet->rect = *rect;
    return 1;
}
static void fg_extract_texture_write(const tex_t *t, unsigned mip, unsigned layer, unsigned count,
                                     unsigned aspect, int clear, int partial, int replayed_ui_clear) {
    if (!FGX.nscene) return;
    vk_tex *x = vk_tex_get(t);
    if (FGX.captured && !replayed_ui_clear && (fg_extract_canvas(x,mip,layer) || fg_extract_ui_depth(x,mip,layer)))
        fg_extract_reject(VK_FG_STRUCTURE_CHANGED,"unrecorded canvas/UI depth clear or copy");
    /* Publish preceding draws before versioning the preserved remainder.
       This is a logical checkpoint, never an extra Vulkan render pass. */
    if (R.open) {
        if (clear && partial) fg_extract_publish_pass();
        else fg_extract_span();
    }
    unsigned writer = FG_PASS_WRITER_OPERATION | ++FGX.operation;
    for (unsigned a = 1; a <= VK_IMAGE_ASPECT_STENCIL_BIT; a <<= 1) if (aspect & a)
        for (unsigned i = 0; i < count; i++) {
            fg_dep_key key = fg_extract_image_key(x, mip, layer + i, a);
            if (clear && !partial) fg_dep_clear(&FGX.deps, key, writer);
            else if (clear) fg_dep_partial_clear(&FGX.deps,key,writer);
            else {
                vk_canvas_image image=FGX.diag ? vk_tex_canvas_identity(x,mip,layer+i) : (vk_canvas_image){0};
                fg_extract_invalidate(key,writer,&image,"buffer-to-texture-write");
            }
            fg_extract_revoke(key,clear ? "attachment-clear" : "buffer-to-texture-write",writer);
            if (R.open) for (unsigned j = 0; j < 9; j++) if (fg_extract_same_key(key,FGX.pass.key[j])) {
                FGX.pass.old[j] = fg_dep_read(&FGX.deps,key);
                if (g_stat_draws != R.draws0 && !(clear && partial)) FGX.pass.reads.value.tainted |= FG_DEP_TAINT_CLEAR;
            }
        }
    if (R.open) fg_pass_guard_invalidate(&FGX.guard);
}
static void fg_extract_compute(vk_shader *const sh[6]) {
    if (!FGX.nscene) return;
    fg_dep_sources sources; fg_dep_sources_init(&sources);
    fg_extract_reads(sh, 5, 6, &sources);
    fg_extract_storage_buffers(sh[5],5,&sources);
    unsigned writer = FG_PASS_WRITER_OPERATION | ++FGX.operation;
    const vk_shader *shader = sh[5];
    for (int i = 0; i < shader->nb; i++) {
        const vk_bind *b = &shader->b[i];
        if (!b->rw || b->dtype != VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) continue;
        uint64_t handle = b->reg < 0 ? 0 : b->kind == RB_IMG ?
            (b->reg < 32 ? S.img[5][b->reg] : 0) : (b->reg < 128 ? S.tex[5][b->reg] : 0);
        vk_treg r;
        if (!handle || !vk_treg_get((int)(handle & 0xfffff), &r)) continue;
        vk_tex *x = vk_tex_get(&r.t);
        unsigned mip = r.has_v && r.v.set_levels ? r.v.base_level : 0;
        unsigned layer = r.has_v && r.v.set_layers ? r.v.min_layer : 0;
        fg_dep_key key = fg_extract_image_key(x, mip, layer, vk_tex_aspect(x));
        if (FGX.captured && (fg_extract_canvas(x,mip,layer) || fg_extract_ui_depth(x,mip,layer)))
            fg_extract_reject(VK_FG_STRUCTURE_CHANGED,"compute modifies captured canvas/UI depth");
        fg_dep_sources output; fg_dep_sources_init(&output); output.value = sources.value;
        fg_dep_sources_add(&FGX.deps, &output, fg_dep_read(&FGX.deps, key));
        fg_extract_publish(key, &r.t, &output, writer, 0);
        fg_extract_revoke(key,"compute-image-write",writer);
    }
}
static void fg_extract_copy(const p_copy_t2t *c, vk_tex *s, vk_tex *d, unsigned layers) {
    if (!FGX.nscene) return;
    unsigned sl = view_level(&c->sv, c->has_sv), dl = view_level(&c->dv, c->has_dv);
    unsigned sa = (c->has_sv && c->sv.set_layers ? c->sv.min_layer : 0) + c->sr.zoffset;
    unsigned da = (c->has_dv && c->dv.set_layers ? c->dv.min_layer : 0) + c->dr.zoffset;
    int partial = c->dr.xoffset || c->dr.yoffset || (unsigned)c->dr.width != vk_tex_width(d, dl) ||
        (unsigned)c->dr.height != vk_tex_height(d, dl);
    unsigned writer = FG_PASS_WRITER_OPERATION | ++FGX.operation;
    for (unsigned i = 0; i < layers; i++) {
        fg_dep_key source = fg_extract_image_key(s, sl, sa + i, vk_tex_aspect(s));
        fg_dep_key destination = fg_extract_image_key(d, dl, da + i, vk_tex_aspect(d));
        if (FGX.captured && (fg_extract_canvas(d,dl,da+i) || fg_extract_ui_depth(d,dl,da+i)))
            fg_extract_reject(VK_FG_STRUCTURE_CHANGED,"copy modifies captured canvas/UI depth");
        fg_dep_sources reads; fg_dep_sources_init(&reads);
        fg_dep_sources_add(&FGX.deps, &reads, fg_dep_read(&FGX.deps, source));
        if (partial) fg_dep_sources_add(&FGX.deps, &reads, fg_dep_read(&FGX.deps, destination));
        if (reads.value.origins && (partial || c->sr.xoffset || c->sr.yoffset ||
            (unsigned)c->sr.width != vk_tex_width(s,sl) || (unsigned)c->sr.height != vk_tex_height(s,sl)))
            reads.value.tainted |= FG_DEP_TAINT_MAPPING;
        fg_extract_publish(destination, c->dst, &reads, writer, 0);
        fg_extract_revoke(destination,"image-copy",writer);
    }
}
static void fg_extract_replay_batch(void) {
    if (FGX.invalid || !vk_fg_canvas_view()) return;
    if (FGX.nreplay < 2 || FGX.replay_draw[FGX.nreplay - 1].final != 2) {
        fg_extract_reject(VK_FG_CHAIN_BROKEN,"no frozen terminal canvas-transfer descriptor"); return;
    }
    const fg_extract_replay_draw *first = &FGX.replay_draw[0];
    for (unsigned i = 0; i + 1 < FGX.nreplay; i++) {
        const fg_extract_replay_draw *p = &FGX.replay_draw[i];
        if (p->final || p->width != first->width || p->height != first->height || p->format != first->format) {
            fg_extract_reject(VK_FG_STRUCTURE_CHANGED,"UI replay target changed within canvas"); return;
        }
    }
    VkCommandBuffer cb = vk_cb();
    int interpolating = vk_fg_interpolating();
    for (unsigned pass = 0; pass < (interpolating ? 2u : 1u); pass++) {
        int final = pass ? 2 : 0; /* Capture UI's affine RGB blend over black/white. */
        if (vk_gpuprof_on) vk_gpuprof_scope_begin(cb, "framegen");
        if (!vk_fg_replay_begin(cb,first->width,first->height,first->format,final)) {
            if (vk_gpuprof_on) vk_gpuprof_scope_end(cb);
            fg_extract_reject(VK_FG_STRUCTURE_CHANGED,"private UI canvas unavailable"); return;
        }
        for (unsigned i = 0; i + 1 < FGX.nreplay; i++) {
            const fg_extract_replay_draw *packet = &FGX.replay_draw[i];
            if (packet->clear) vkCmdClearAttachments(cb,1,&packet->attachment,1,&packet->rect);
            else vk_fg_replay_draw(cb,&packet->draw,packet->sets);
        }
        vk_fg_replay_end(cb,final);
        if (vk_gpuprof_on) vk_gpuprof_scope_end(cb);
    }
    int terminal_capture=vk_fg_adaptive();
#ifdef TOTK_VK_TESTS
    /* The owned Vulkan fixture substitutes only unavailable AFME/GL work;
     * production binding/extraction still supplies this exact terminal. */
    terminal_capture=terminal_capture || vk_fg_test_generation_bridge_active();
#endif
    if (terminal_capture) {
        const fg_extract_replay_draw *last=&FGX.replay_draw[FGX.nreplay-1];
        vk_fg_adaptive_terminal(&last->draw,last->sets,last->width,last->height,last->format);
    }
    /* Warmup/refusal keeps today's UI capture for the next adjacent pair.
       A midpoint uses retained older UI, never the newer frame's live HUD. */
    if (!interpolating || vk_fg_interp_compose(cb)) {
        if (vk_fg_qcom_requested() || vk_fg_lsfg_requested()) cb=vk_cb(); /* External handoff can seal the primary. */
        const fg_extract_replay_draw *last = &FGX.replay_draw[FGX.nreplay - 1];
        if (vk_gpuprof_on) vk_gpuprof_scope_begin(cb, "framegen");
        if (!vk_fg_replay_begin(cb,last->width,last->height,last->format,1)) {
            if (vk_gpuprof_on) vk_gpuprof_scope_end(cb);
            fg_extract_reject(VK_FG_STRUCTURE_CHANGED,"private terminal image unavailable"); return;
        }
        vk_fg_replay_draw(cb,&last->draw,last->sets);
        vk_fg_replay_end(cb,1);
        if (vk_gpuprof_on) vk_gpuprof_scope_end(cb);
    }
    g_bound_pipe = VK_NULL_HANDLE; g_dyn_dirty = g_pipe_dirty = 1;
    descriptor_unbind(); vk_stage_sig_invalidate(0x3fu);
}
void vk_fg_extract_present(const tex_t *t) {
    if (!vk_fg_extract_active || !t) return;
    FGX.probe_started = 1;
    vk_end_rendering_at(VK_PASS_END_PRESENT, 0);
    const vk_canvas_frame *chain = vk_canvas_current();
    vk_tex *window = vk_tex_get(t);
    vk_canvas_image identity = vk_tex_canvas_identity(window, 0, 0);
    int presented = vk_canvas_present(&identity);
    if (!FGX.captured) fg_extract_reject(VK_FG_NO_BOUNDARY,"scene/canvas/UI boundary not captured");
    else if (!chain->ui_committed || !chain->transfer_committed || !presented || !FGX.transfer)
        fg_extract_reject(VK_FG_CHAIN_BROKEN,"canvas/final-transfer/present chain broken");
    else if (!FGX.scene[FGX.selected].valid) fg_extract_reject(VK_FG_DEPTH_LOST,"scene depth overwritten before present");
    else if (FGX.deps.failed || FGX.passes.failed) fg_extract_reject(VK_FG_TRACK_CAPACITY,"tracking capacity exhausted");
    if (!FGX.invalid && vk_fg_canvas_view() && vk_native_ui_enabled() && vk_tex_native_active(window)) {
        const fg_extract_replay_draw *last = FGX.nreplay ? &FGX.replay_draw[FGX.nreplay-1] : NULL;
        vk_post_output native={0};
        int native_ready=vk_tex_native_output(window,&native);
        if (!last || last->final != 2 || !last->native ||
            !native_ready || native.image != last->image ||
            native.w != last->width || native.h != last->height ||
            vk_fg_raw_format(native.format) != vk_fg_raw_format(last->format)) {
            LOGV("[framegen-extract] native terminal mismatch: frozen image=%p native=%u final=%u extent=%ux%u format=%u; presented ready=%d image=%p extent=%ux%u format=%u",
                last ? (void *)last->image : NULL,last ? last->native : 0,last ? last->final : 0,
                last ? last->width : 0,last ? last->height : 0,last ? last->format : 0,
                native_ready,(void *)native.image,native.w,native.h,native.format);
            fg_extract_reject(VK_FG_STRUCTURE_CHANGED,"native terminal attachment/extent/encoding does not match presented sibling");
        }
        else if (vk_fg_interpolating()) {
            VkCommandBuffer cb = vk_cb();
            if (vk_gpuprof_on) vk_gpuprof_scope_begin(cb, "framegen");
            int captured = vk_fg_capture_real(cb,window,0,0);
            if (vk_gpuprof_on) vk_gpuprof_scope_end(cb);
            if (!captured)
                fg_extract_reject(VK_FG_STRUCTURE_CHANGED,"native presented real image could not be retained exactly");
        }
    } else if (!FGX.invalid && vk_fg_interpolating()) {
        VkCommandBuffer cb = vk_cb();
        if (vk_gpuprof_on) vk_gpuprof_scope_begin(cb, "framegen");
        (void)vk_fg_capture_real(cb,window,0,0); /* masked pairs require this; known pairs need no fallback */
        if (vk_gpuprof_on) vk_gpuprof_scope_end(cb);
    }
    if (!FGX.invalid) fg_extract_replay_batch();
    FGX.eligible = !FGX.invalid;
    const char *reason = FGX.invalid ? FGX.reason : "eligible scene -> canvas -> final transfer -> present";
    if (!FGX.last_reason || strcmp(reason, FGX.last_reason)) {
        LOGV("[framegen-extract] real %llu %s", (unsigned long long)FGX.frame, reason);
        FGX.last_reason = reason;
    }
}
void vk_fg_extract_end(void) {
    if (!FGX.requested) return;
    if (FGX.writer.open) {
        vk_fg_writer_poison("extractor frame ended before physical writer closure");
        FGX.writer.open=0;
    }
    if (FGX.authority_span.open) {
        FGX.authority_span.open=0;
        fg_extract_reject(VK_FG_DEPTH_LOST,"ordered depth authority span left open at frame end");
    }
    int active=FGX.probe_started || vk_fg_extract_active, enabled=vk_fg_capturing();
    if (FGX.diag && FGX.cpu_start) {
        uint64_t end=fg_extract_cpu_ns();
        if (end >= FGX.cpu_start) {
            uint64_t elapsed=end-FGX.cpu_start; unsigned index=active != 0;
            FGX.diag_window.cpu_ns[index] += elapsed; FGX.diag_window.cpu_frames[index]++;
            if (elapsed > FGX.diag_window.cpu_max[index]) FGX.diag_window.cpu_max[index]=elapsed;
        }
    }
    if (active) {
        FGX.diag_window.physical=FGX.passes.physical_passes; FGX.diag_window.groups=FGX.passes.count;
        FGX.diag_window.contexts=FGX.passes.draw_events; FGX.diag_window.missed=FGX.passes.incomplete_contexts;
    }
    const vk_canvas_frame *chain=vk_canvas_current();
    FGX.diag_window.probes += active;
    FGX.diag_window.backoff += !active && enabled;
    FGX.diag_window.transfers += chain->transfer_committed != 0;
    FGX.diag_window.nominations += chain->first_ui != 0;
    FGX.diag_window.attempts += FGX.ui_seen != 0;
    FGX.diag_window.snapshots += FGX.snapshot_ok != 0;
    FGX.diag_window.roots += FGX.nscene;
    if (chain->canvas.image) FGX.diag_window.canvas=chain->canvas;
    if (!FGX.eligible) vk_fg_skip();
    unsigned reason = !active ? (enabled ? VK_FG_BACKOFF : VK_FG_SUSPENDED) :
        FGX.eligible ? VK_FG_ELIGIBLE : FGX.invalid ? FGX.refusal : VK_FG_NO_BOUNDARY;
    vk_fg_track_result(reason);
    if (enabled) fg_pass_probe_end(&FGX.probe,FGX.eligible,reason == VK_FG_TRACK_CAPACITY);
    if (FGX.diag && FGX.plane_coverage.draws)
        LOGV("[framegen-plane-coverage] real %llu depth-draws=%llu accepted=%llu scissor-sum-pixels=%llu viewport-sum-pixels=%llu max-scissor-fraction=%.6f (tracked probe prefix; overlapping upper bounds, NOT raster coverage)",
            (unsigned long long)FGX.frame,(unsigned long long)FGX.plane_coverage.draws,
            (unsigned long long)FGX.plane_coverage.accepted,(unsigned long long)FGX.plane_coverage.scissor_pixels,
            (unsigned long long)FGX.plane_coverage.viewport_pixels,FGX.plane_coverage.max_scissor_fraction);
    if (++FGX.frame % 300 == 0 && FGX.diag) {
        const vk_canvas_image *c=&FGX.diag_window.canvas;
        const fg_dep_value *v=&FGX.diag_window.value;
        LOGV("[framegen-diag] canvas last300 probes=%u backoff=%u id=%#llx incarnation=%llu size=%ux%u format=%u transfer-seen=%u first-ui=%u snapshot-attempt=%u snapshot-taken=%u roots=%u",
            FGX.diag_window.probes,FGX.diag_window.backoff,(unsigned long long)c->image,
            (unsigned long long)c->incarnation,c->width,c->height,c->format,FGX.diag_window.transfers,
            FGX.diag_window.nominations,FGX.diag_window.attempts,FGX.diag_window.snapshots,FGX.diag_window.roots);
        LOGV("[framegen-diag] canvas dependency last-attempt result=%s roots=%u known=%u version=%llu origins=%#llx eligible=%#llx unresolved=%#llx tainted=%#x; last300 dummy=%u missing=%u bounds=%u storage=%u image-writes=%u writes=%u",
            FGX.diag_window.attempts ? fg_dep_reason(FGX.diag_window.result) : "not attempted",FGX.diag_window.root_count,v->known,
            (unsigned long long)v->version,(unsigned long long)v->origins,(unsigned long long)v->eligible,
            (unsigned long long)v->unresolved,v->tainted,FGX.diag_window.dummy,FGX.diag_window.missing,
            FGX.diag_window.bounds,FGX.diag_window.storage,FGX.diag_window.image_writes,FGX.diag_window.writes);
        LOGV("[framegen-diag] scene-signature %#llx stable-frames %llu physical %u groups %u contexts %u contexts-missed %u",
            (unsigned long long)FGX.signature,(unsigned long long)FGX.stable_frames,
            FGX.diag_window.physical,FGX.diag_window.groups,FGX.diag_window.contexts,FGX.diag_window.missed);
        for (unsigned i=0;i<3;i++)
            LOGV("[framegen-binding] last 300 real class=%s contexts=%llu reflected=%llu view-work=%llu old-view-work=%llu overflow=%u (attempted work; fixed budget=%u)",
                i == 2 ? "compute" : i == 1 ? "raster-depth" : "raster-no-depth",
                (unsigned long long)FGX.diag_window.binding_calls[i],
                (unsigned long long)FGX.diag_window.binding_reflected[i],
                (unsigned long long)FGX.diag_window.binding_view[i],
                (unsigned long long)FGX.diag_window.binding_old_view[i],
                FGX.diag_window.binding_overflows[i],FG_PASS_MAX_BINDINGS);
        LOGV("[framegen-binding] last 300 real busiest-frame=%llu group=%d shape=%#llx extent=%ux%u reserved-work=%u; binding-walk thread CPU total/max-context=%.3f/%.3fms (not whole tracker; DIAG clock overhead included)",
            (unsigned long long)FGX.diag_window.binding_top_frame,FGX.diag_window.binding_top_group,
            (unsigned long long)FGX.diag_window.binding_top_signature,
            FGX.diag_window.binding_top_width,FGX.diag_window.binding_top_height,FGX.diag_window.binding_top_work,
            FGX.diag_window.binding_cpu_ns/1e6,FGX.diag_window.binding_cpu_max/1e6);
        LOGV("[framegen-diag] first-pass-to-end thread CPU (whole walking window, NOT tracking alone): probes n=%u mean/max=%.3f/%.3fms backoff n=%u mean/max=%.3f/%.3fms",
            FGX.diag_window.cpu_frames[1],FGX.diag_window.cpu_frames[1] ?
            FGX.diag_window.cpu_ns[1]/(1e6*FGX.diag_window.cpu_frames[1]) : 0,FGX.diag_window.cpu_max[1]/1e6,
            FGX.diag_window.cpu_frames[0],FGX.diag_window.cpu_frames[0] ?
            FGX.diag_window.cpu_ns[0]/(1e6*FGX.diag_window.cpu_frames[0]) : 0,FGX.diag_window.cpu_max[0]/1e6);
        vk_canvas_image last=FGX.diag_window.canvas;
        memset(&FGX.diag_window,0,sizeof FGX.diag_window);
        FGX.diag_window.canvas=last; FGX.diag_window.result=FG_DEP_NO_ORIGIN;
    }
    FGX.nscene = FGX.nwatch = FGX.operation = 0;
    FGX.nunknown = FGX.unknown_overflow = 0;
    FGX.captured = FGX.invalid = FGX.ui_seen = FGX.transfer = FGX.eligible = FGX.nreplay = FGX.snapshot_ok = FGX.probe_started = 0;
    FGX.authority_ready=FGX.authority_gpu_ready=0; FGX.authority_key=(fg_dep_key){0};
    FGX.reason = NULL;
    FGX.canvas_key = FGX.ui_depth_key = (fg_dep_key){0};
    FGX.ui_depth_address = FGX.ui_depth_size = 0;
    FGX.cpu_start=0;
    memset(&FGX.plane_coverage,0,sizeof FGX.plane_coverage);
    if (FGX.diag) memset(FGX.binding_group_work,0,sizeof FGX.binding_group_work);
    if (active) { fg_dep_reset(&FGX.deps); fg_pass_reset(&FGX.passes); }
    fg_extract_refresh(0);
}
void vk_fg_present_begin(const tex_t *t) {
    if (fg_extract_requested_config()) vk_fg_extract_present(t);
}
void vk_fg_present_end(uint64_t fence) {
    if (!fg_extract_requested_config()) return;
    vk_fg_extract_end(); vk_fg_frame_end(fence); vk_fg_stats();
}
#ifdef TOTK_VK_TESTS
void vk_fg_test_present_begin(const tex_t *t) { vk_fg_present_begin(t); }
void vk_fg_test_present_end(uint64_t fence) { vk_fg_present_end(fence); }
void vk_fg_test_lifecycle(uint64_t facts[8]) {
    facts[0]=(uint64_t)fg_extract_requested_config(); facts[1]=(uint64_t)vk_fg_extract_active;
    facts[2]=FGX.frame; facts[3]=FGX.snapshot_ok; facts[4]=FGX.eligible;
    vk_fg_test_source_lifecycle(facts+5);
}
#endif
static int scissor_full(void) {
    if (!S.has_sc) return 1;
    uint32_t w = 0, h = 0; const tex_t *t = S.has_rt && S.rt.n && S.rt.color[0] ? S.rt.color[0] : S.has_rt ? S.rt.depth : NULL;
    if (t) { w = t->d.width; h = t->d.height; }
    return S.sc.x <= 0 && S.sc.y <= 0 && (uint32_t)(S.sc.x + S.sc.w) >= w && (uint32_t)(S.sc.y + S.sc.h) >= h;
}
static VkRect2D native_rect(VkRect2D r) {
    if (!R.native) return r;
    int64_t x0 = r.offset.x, y0 = r.offset.y, x1 = x0 + r.extent.width, y1 = y0 + r.extent.height;
    x0 = MINV((x0 * R.host_w + R.native_w/2) / R.native_w,R.host_w); x1 = MINV((x1 * R.host_w + R.native_w/2) / R.native_w,R.host_w);
    y0 = MINV((y0 * R.host_h + R.native_h/2) / R.native_h,R.host_h); y1 = MINV((y1 * R.host_h + R.native_h/2) / R.native_h,R.host_h);
    return (VkRect2D){{(int32_t)x0,(int32_t)y0},{(uint32_t)(x1-x0),(uint32_t)(y1-y0)}};
}
static VkRect2D clear_rect(void) {
    int32_t x0 = 0, y0 = 0, x1 = (int32_t)R.w, y1 = (int32_t)R.h;
    if (S.has_sc) { x0 = MAXV(x0, S.sc.x); y0 = MAXV(y0, S.sc.y); x1 = MINV(x1, S.sc.x + S.sc.w); y1 = MINV(y1, S.sc.y + S.sc.h); }
    if (x1 < x0) x1 = x0; if (y1 < y0) y1 = y0;
    return native_rect((VkRect2D){{x0, y0}, {(uint32_t)(x1 - x0), (uint32_t)(y1 - y0)}});
}
static int clear_full(void) {   /* same render area as vkCmdClearAttachments, including mip views and unequal targets */
    if (!R.open) return scissor_full();
    VkRect2D r = clear_rect();
    return !r.offset.x && !r.offset.y && r.extent.width == R.host_w && r.extent.height == R.host_h;
}
static VkClearValue color_value(int fmt_class, const float rgba[4]) {
    VkClearValue v;
    if (fmt_class == 1) for (int i = 0; i < 4; i++) v.color.uint32[i] = (uint32_t)rgba[i];
    else if (fmt_class == 2) for (int i = 0; i < 4; i++) v.color.int32[i] = (int32_t)rgba[i];
    else memcpy(v.color.float32, rgba, 16);
    return v;
}
static void clear_color(const p_clear_color *c) {
    if (S.render_disabled) { g_pops.clears_skipped++; return; }
    g_pops.clears++; memcpy(g_pops.cc, c->rgba, 16);
    if (c->index < 0 || c->index >= S.rt.n || c->index >= 8 || !S.rt.color[c->index]) { RED(4, "colour clear of target %d, which is not set", c->index); return; }
    if (!(c->mask & 0xf)) return;
    int f = S.rt.has_cview[c->index] && S.rt.cview[c->index].set_format ? S.rt.cview[c->index].format : S.rt.color[c->index]->d.format;
    VkClearValue v = color_value(vk_fmt_class(f), c->rgba);
    int full = clear_full();
    if (R.open && (c->mask & 0xf) == 0xf && full && clear_load_on()) { vk_end_rendering_at(VK_PASS_END_CLEAR, 1u << c->index); g_clear_load_c++; }
    if (!R.open && (c->mask & 0xf) == 0xf && full) { R.clear_c[c->index] = 1; R.cc[c->index] = v; return; }
    if ((c->mask & 0xf) != 0xf) RED(4, "colour clear with channel mask %#x: cleared whole (masked clears not done yet)", c->mask);
    g_pops.clears_inpass++;
    begin_rendering(0);
    VkClearAttachment ca = {VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t)c->index, v};
    VkClearRect cr = {clear_rect(), 0, R.layers ? R.layers : 1};
    if (cr.rect.extent.width && cr.rect.extent.height) {
        if (R.secondary) vk_record_clear(&ca, &cr); else vkCmdClearAttachments(vk_cb(), 1, &ca, 1, &cr);
        if (pso_life_active() && full && (c->mask & 15) == 15) PL.color_clear |= 1u << c->index;
        pso_life_view_write(R.ctex[c->index], PL.color_view[c->index], VK_IMAGE_ASPECT_COLOR_BIT,
            full && (c->mask & 15) == 15, 0, "inline-clear");
        if (vk_fg_extract_active) fg_extract_texture_write(S.rt.color[c->index], R.clevel[c->index],
            R.clayer[c->index], 1, VK_IMAGE_ASPECT_COLOR_BIT, 1, !full, fg_extract_ui_clear(&ca,&cr));
    }
}
static void clear_ds(const p_clear_ds *c) {
    if (S.render_disabled || !S.has_rt || !S.rt.depth) return;
    int sm = c->stencil_mask & 255; if (!c->depth_mask && !sm) return;
    int fmt = S.rt.depth->d.format;
    VkImageAspectFlags as = (c->depth_mask && vk_fmt_has_depth(fmt) ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) | (sm && vk_fmt_has_stencil(fmt) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
    if (!as) return;
    if ((as&VK_IMAGE_ASPECT_DEPTH_BIT) && FGX.writer.open)
        vk_end_rendering_at(VK_PASS_END_CLEAR,(uint32_t)as); /* close BEFORE the actual depth clear */
    int full = clear_full();
    if (R.open && (!sm || sm == 255) && full && clear_load_on()) { vk_end_rendering_at(VK_PASS_END_CLEAR, (uint32_t)as); g_clear_load_ds++; }
    if (!R.open && (!sm || sm == 255) && full) {
        if (as & VK_IMAGE_ASPECT_DEPTH_BIT) { R.clear_d = 1; R.cd = c->depth; }
        if (as & VK_IMAGE_ASPECT_STENCIL_BIT) { R.clear_s = 1; R.cs = (uint32_t)c->stencil; }
        return;
    }
    if (sm && sm != 255) RED(4, "stencil clear with mask %#x: cleared whole", sm);
    begin_rendering(0);
    VkClearAttachment ca = {as, 0, {.depthStencil = {c->depth, (uint32_t)c->stencil}}};
    VkClearRect cr = {clear_rect(), 0, R.layers ? R.layers : 1};
    if (cr.rect.extent.width && cr.rect.extent.height) {
        if (as & VK_IMAGE_ASPECT_DEPTH_BIT) g_clear_inline_d++;
        if (R.secondary) vk_record_clear(&ca, &cr); else vkCmdClearAttachments(vk_cb(), 1, &ca, 1, &cr);
        if (pso_life_active() && full && (!sm || sm == 255)) PL.depth_clear |= as;
        for (unsigned bit = VK_IMAGE_ASPECT_DEPTH_BIT; bit <= VK_IMAGE_ASPECT_STENCIL_BIT; bit <<= 1)
            if (as & bit) pso_life_view_write(R.dtex, bit == VK_IMAGE_ASPECT_DEPTH_BIT ? PL.depth_view : PL.stencil_view,
                bit, full && (!sm || sm == 255), 0, "inline-clear");
        if (vk_fg_extract_active) fg_extract_texture_write(S.rt.depth,R.dlevel,R.dlayer,1,as,1,!full,fg_extract_ui_clear(&ca,&cr));
    }
}

/* ---------- graphics pipelines ---------- */
typedef struct {
    void *vs, *fs;
    VkFormat cfmt[8], dfmt, sfmt; uint32_t ncolor, samples;
    blend_st blend[8]; uint32_t blend_en, chmask; uint8_t a2c, poly_mode, topo_class, depth_clamp, logic_op, pad[3];
    struct { uint32_t fmt, offset, stream; } attr[16];
    struct { uint32_t instanced, divisor; } stream[16];
} pipe_key;
static pthread_once_t g_pso_trace_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t g_pso_trace_mu = PTHREAD_MUTEX_INITIALIZER;
static int g_pso_trace_enabled;
static void pso_trace_init(void) {
    const char *value = getenv("TOTK_PSO_TRACE");
    g_pso_trace_enabled = value && !strcmp(value, "1");
}
static int pso_trace_on(void) {
    pthread_once(&g_pso_trace_once, pso_trace_init);
    return g_pso_trace_enabled;
}
static TotkPsoTraceKey pso_trace_key(const pipe_key *k) {
    TotkPsoTraceKey key = {0};
    const vk_shader *vs = k->vs, *fs = k->fs;
    key.vs_id = vs ? vs->key : NULL; key.fs_id = fs ? fs->key : NULL;
    key.fs_early_tests = fs ? (uint32_t)fs->early_tests | fs->alpha_func<<1 : 0;
    for (unsigned i = 0; i < 8; i++) key.color[i] = k->cfmt[i];
    key.depth = k->dfmt; key.stencil = k->sfmt; key.color_count = k->ncolor; key.samples = k->samples;
    for (unsigned i = 0; i < 8; i++) {
        key.blend[i][0] = (uint32_t)k->blend[i].target;
        key.blend[i][1] = (uint32_t)k->blend[i].src_rgb;
        key.blend[i][2] = (uint32_t)k->blend[i].dst_rgb;
        key.blend[i][3] = (uint32_t)k->blend[i].src_a;
        key.blend[i][4] = (uint32_t)k->blend[i].dst_a;
        key.blend[i][5] = (uint32_t)k->blend[i].eq_rgb;
        key.blend[i][6] = (uint32_t)k->blend[i].eq_a;
    }
    key.blend_enabled = k->blend_en; key.channel_mask = k->chmask; key.a2c = k->a2c;
    key.polygon_mode = k->poly_mode; key.topology_class = k->topo_class;
    key.depth_clamp = k->depth_clamp; key.logic_op = k->logic_op;
    for (unsigned i = 0; i < 16; i++) {
        key.attributes[i][0] = k->attr[i].fmt; key.attributes[i][1] = k->attr[i].offset; key.attributes[i][2] = k->attr[i].stream;
        key.streams[i][0] = k->stream[i].instanced; key.streams[i][1] = k->stream[i].divisor;
    }
    return key;
}
static void pso_trace_log(const pipe_key *k, const char *phase, const char *reason, uint64_t frame,
                          VkResult result, uint64_t elapsed_ns, VkPipelineCreateFlags flags, int part) {
    if (!pso_trace_on()) return;
    TotkPsoTraceKey key = pso_trace_key(k);
    uint64_t id = totk_pso_trace_key_hash(&key);
    const vk_shader *vs = k->vs, *fs = k->fs;
    pthread_mutex_lock(&g_pso_trace_mu);
    LOGV("[pso-trace] key=%016llx phase=%s reason=%s frame=%llu result=%d ms=%.3f flags=%x part=%d vs=%s fs=%s early=%u",
         (unsigned long long)id, phase, reason, (unsigned long long)frame, result, elapsed_ns / 1e6,
         (unsigned)flags, part, vs ? vs->key : "-", fs ? fs->key : "-", fs ? (unsigned)fs->early_tests : 0);
    pthread_mutex_unlock(&g_pso_trace_mu);
}
/* Recording-owner memo: target changes, not shader changes, determine the screen extent class.
   Runtime resolution configuration is immutable after env loading; keep parsing out of the draw hot path. */
static int pipe_screen_class(void) {
    static unsigned configured_w, configured_h, last_w = UINT32_MAX, last_h;
    static int initialized, last_class;
    if (last_w == R.w && last_h == R.h) return last_class;
    if (!initialized) {
        const char *value = getenv("TOTK_RENDER_RES"); unsigned w, h; char tail;
        if (value && sscanf(value, "%ux%u%c", &w, &h, &tail) == 2 &&
            w >= 64 && h >= 64 && w <= 4096 && h <= 4096) {
            configured_w = w; configured_h = h;
        }
        initialized = 1;
    }
    last_w = R.w; last_h = R.h;
    last_class = totk_screen_classify(R.w, R.h, configured_w, configured_h);
    return last_class;
}
/* Only called by the recording owner. Workers must never inspect mutable R/S to describe their origin.
   Cold-demand rows associate later worker trace records with actual attachment identity without per-entry copies. */
static void pso_trace_target_log(const pipe_key *k, const char *phase, uint64_t frame, VkResult result, uint64_t elapsed_ns) {
    if (!pso_trace_on()) return;
    TotkPsoTraceKey key = pso_trace_key(k);
    uint64_t id = totk_pso_trace_key_hash(&key);
    int cls = pipe_screen_class();
    int64_t delta = (int64_t)R.w * 9 - (int64_t)R.h * 16;
    const char *reason = !R.ncolor ? "depth-only" : cls == 1 ? "screen-exact" :
        cls == 8 ? "screen-aligned8" : cls == 16 ? "screen-aligned16" : cls == 32 ? "screen-aligned32" : "capture-guard";
    pthread_mutex_lock(&g_pso_trace_mu);
    LOGV("[pso-target] key=%016llx phase=%s frame=%llu sub=%llu pass_cursor=%llu open=%d result=%d ms=%.3f "
         "w=%u h=%u class=%s old_screen=%d delta=%lld colors=%u samples=%u "
         "cfmt=%u,%u,%u,%u,%u,%u,%u,%u dfmt=%u sfmt=%u depth_serial=%llu depth_mem=%p depth_mip=%u depth_layer=%u",
         (unsigned long long)id, phase, (unsigned long long)frame, (unsigned long long)vk_game_submit_serial(),
         (unsigned long long)g_stat_passes, R.open, result, elapsed_ns / 1e6, R.w, R.h, reason,
         delta > -32 && delta < 32, (long long)delta, R.ncolor, R.samples,
         k->cfmt[0], k->cfmt[1], k->cfmt[2], k->cfmt[3], k->cfmt[4], k->cfmt[5], k->cfmt[6], k->cfmt[7],
         k->dfmt, k->sfmt, (unsigned long long)(R.daddr ? R.d_serial : 0), R.daddr,
         R.daddr ? R.dlevel : 0, R.daddr ? R.dlayer : 0);
    for (uint32_t i = 0; i < R.ncolor && i < 8; i++) if (R.caddr[i])
        LOGV("[pso-target-color] key=%016llx phase=%s pass_cursor=%llu index=%u serial=%llu mem=%p fmt=%u mip=%u layer=%u",
             (unsigned long long)id, phase, (unsigned long long)g_stat_passes, i,
             (unsigned long long)R.c_serial[i], R.caddr[i], k->cfmt[i], R.clevel[i], R.clayer[i]);
    pthread_mutex_unlock(&g_pso_trace_mu);
}
/* g_pipes: pipe_key -> pipe_ent. A pipeline the pipeline cache holds is made at its first draw; one the driver has to
   compile (VK_PIPELINE_COMPILE_REQUIRED: on Turnip 70-150 ms for each new shader, freezes of up to 3 s on a first visit)
   goes to TOTK_VK_COMPILE_THREADS worker threads (default 2; 0 compiles at the draw, as before) and its draws are skipped
   until it is ready, so new objects show a few frames late instead (offscreen captures excepted, see pipe_get). Only the
   recording thread (under vk_rec_lock) uses the map; a worker publishes through the entry's state. */
enum { PE_QUEUED, PE_BUILDING, PE_READY, PE_FAILED };
typedef struct pipe_ent {
    pipe_key k; VkPipelineLayout layout; VkPipeline p; _Atomic int state;
    unsigned failures;                 /* published before PE_FAILED/PE_READY, read with acquire */
    VkPipeline fast;                   /* walking-thread fallback, retained for in-flight command buffers */
    uint64_t optimized_ns;             /* published with PE_READY */
    int fast_attempted, replaced;      /* walking thread only */
    uint64_t wait_sub, retry_at; unsigned skipped, seen_failure;   /* walking thread only */
    uint64_t frame;                    /* originating request; immutable while queued/building */
    int predicted, capture; uint32_t list_index, collected; struct pipe_ent *next;
    vk_lend lend;                      /* g_pq_mu: the worker building it and any nice a waiter lent it */
} pipe_ent;
#define PIPE_FAILED ((VkPipeline)(uintptr_t)1)
static nvn_hmap g_pipes = {.klen = sizeof(pipe_key)};
/* Separate immutable subset caches. The mutex protects map/publication only, never driver compilation.
   Missing subsets compile on workers; the walker only links already-ready parts (no shader compilation). */
enum { PL_VERTEX_INPUT, PL_PRE_RASTER, PL_FRAGMENT, PL_OUTPUT, PL_COUNT };
typedef struct { pipe_key k; VkPipelineLayout layout; } pipe_library_key;
typedef struct { VkPipeline p; int building, failed; } pipe_library;
static nvn_hmap g_libraries[PL_COUNT] = {
    {.klen = sizeof(pipe_library_key)}, {.klen = sizeof(pipe_library_key)},
    {.klen = sizeof(pipe_library_key)}, {.klen = sizeof(pipe_library_key)}};
static pthread_mutex_t g_library_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_library_cv = PTHREAD_COND_INITIALIZER;
static struct {
    _Atomic uint64_t fast_links, fast_link_ns, fast_link_max_ns, fast_draws;
    _Atomic uint64_t fast_link_window_max_ns;
    _Atomic uint64_t optimized_replacements, optimized_ns, library_skips;
    _Atomic uint64_t parts[PL_COUNT], part_ns[PL_COUNT];
} g_library_stats;
static int g_draw_fast;                /* reset by pipe_get; actual issued draws counted below */
/* Android prediction is opt-in: keep the shipped compiler/draw/pacing defaults unchanged.
   Only actual synchronous graphics driver builds earn adaptive credit, never worker/cache waits. */
static nvn_scan_budget g_pp_budget;
static uint64_t g_pp_frame = 1, g_pp_submit, g_pp_pass;
static unsigned g_pp_scans_in_sub;
static int g_pp_more;
static _Atomic uint64_t g_pp_build_ns, g_pp_wait_ns, g_pp_scan_ns;
static int pipe_predict_on(void) {
    static int on = -1; if (on < 0) { const char *e = getenv("TOTK_PSO_PREDICT"); on = e && atoi(e) != 0; } return on;
}
static int pipe_predict_adapt(void) {
    static int on = -1; if (on < 0) { const char *e = getenv("TOTK_PSO_SCAN_ADAPT"); on = !e || atoi(e) != 0; } return on;
}
static void pipe_diag_add(_Atomic uint64_t *counter, uint64_t ns) {
    /* One recording-thread writer; readers may be on the guest presenting thread. */
    atomic_store_explicit(counter, nvn_scan_budget_add(atomic_load_explicit(counter, memory_order_relaxed), ns), memory_order_relaxed);
}
void mtl_create_ns(uint64_t *lib_ns, uint64_t *pso_ns) {
    *lib_ns = 0; *pso_ns = dm_wait_stats_enabled() ?
        nvn_scan_budget_add(atomic_load_explicit(&g_pp_build_ns, memory_order_relaxed), vk_compute_pipe_build_ns()) : 0;
}
void mtl_wait_stats(uint64_t *lib_ns, uint64_t *pso_ns) {
    *lib_ns = 0; *pso_ns = dm_wait_stats_enabled() ? atomic_load_explicit(&g_pp_wait_ns, memory_order_relaxed) : 0;
}
void mtl_scan_stats(uint64_t *shader_ns, uint64_t *pso_ns) {
    *shader_ns = 0; *pso_ns = dm_wait_stats_enabled() ? atomic_load_explicit(&g_pp_scan_ns, memory_order_relaxed) : 0;
}
#ifdef TOTK_VK_TESTS
static vk_pipeline_test_stats g_pp_test;
void vk_pipeline_test_stats_get(vk_pipeline_test_stats *out) {
    *out = g_pp_test; out->frame = g_pp_budget.frame; out->used_ns = g_pp_budget.used_ns; out->sync_ns = g_pp_budget.sync_ns;
    out->scans_in_sub = g_pp_scans_in_sub;
    out->fast_links = atomic_load(&g_library_stats.fast_links);
    out->fast_draws = atomic_load(&g_library_stats.fast_draws);
    out->fast_link_ns = atomic_load(&g_library_stats.fast_link_ns);
    out->fast_link_max_ns = atomic_load(&g_library_stats.fast_link_max_ns);
    out->optimized_replacements = atomic_load(&g_library_stats.optimized_replacements);
    out->optimized_ns = atomic_load(&g_library_stats.optimized_ns);
    out->library_skips = atomic_load(&g_library_stats.library_skips);
    for (int i = 0; i < PL_COUNT; i++) {
        out->parts[i] = atomic_load(&g_library_stats.parts[i]);
        out->part_ns[i] = atomic_load(&g_library_stats.part_ns[i]);
    }
}
#endif
static void pipe_predict_maybe(const uint8_t *next);
static pthread_mutex_t g_pq_mu = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_pq_cv = PTHREAD_COND_INITIALIZER;
static pipe_ent *g_pq_head, *g_pq_tail;
static _Atomic uint64_t g_win_async, g_win_async_ns;   /* compiled on worker threads, per stutter window */
/* Priority lending (TOTK_VK_PIPE_BOOST=1, default off). Workers run at VK_WORKER_NICE so background builds yield to the
   game. Once a recording thread joins a worker's in-flight build, that build is on the frame's critical path: the
   waiter lends the worker its own nice until the build is published, then the worker returns to VK_WORKER_NICE.
   Joins and their wall time are counted with the switch off too, so both arms report the same line.
   Live (runtime/live_ctl.h): each join reads the current value; a loan already made is still restored. */
static int g_pipe_boost;   /* the launch value */
static totk_live_cache g_pipe_boost_live;
static int pipe_boost_on(void) { return totk_live_cached(&g_pipe_boost_live, "TOTK_VK_PIPE_BOOST", g_pipe_boost); }
static _Atomic uint64_t g_boost_joins, g_boost_join_ns, g_boost_raised, g_boost_refused;
static _Atomic int g_boost_refusal_logged;
#ifdef TOTK_VK_TESTS
static int (*g_boost_test_nice)(void), (*g_boost_test_set)(int tid, int nice);
void vk_pipe_boost_test_hooks(int (*self_nice)(void), int (*set)(int tid, int nice)) {
    g_boost_test_nice = self_nice; g_boost_test_set = set;
}
void vk_pipe_boost_test_stats(uint64_t out[4]) {
    out[0] = atomic_load(&g_boost_joins); out[1] = atomic_load(&g_boost_join_ns);
    out[2] = atomic_load(&g_boost_raised); out[3] = atomic_load(&g_boost_refused);
}
#endif
static int boost_self_nice(void) {
#ifdef TOTK_VK_TESTS
    if (g_boost_test_nice) return g_boost_test_nice();
#endif
    int saved = errno; errno = 0;
    int nice = getpriority(PRIO_PROCESS, 0);   /* Linux nice is per thread */
    if (nice == -1 && errno) nice = VK_WORKER_NICE;
    errno = saved; return nice;
}
static int boost_set(int tid, int nice) {   /* 0 or the errno; tid 0 is the calling thread */
#ifdef TOTK_VK_TESTS
    if (g_boost_test_set) return g_boost_test_set(tid, nice);
#endif
    int saved = errno, error = setpriority(PRIO_PROCESS, (id_t)tid, nice) ? errno : 0;
    errno = saved; return error;
}
int vk_thread_tid(void) { return (int)syscall(SYS_gettid); }
int vk_pipe_boost_lend(vk_lend *l) {
    if (!l->tid || !pipe_boost_on()) return 0;
    int nice = boost_self_nice();
    if (nice >= (l->lent ? l->nice : VK_WORKER_NICE)) return 0;
    int error = boost_set(l->tid, nice);
    if (error) {
        atomic_fetch_add_explicit(&g_boost_refused, 1, memory_order_relaxed);
        if (!atomic_exchange_explicit(&g_boost_refusal_logged, 1, memory_order_relaxed))
            LOGV("pipe boost: raising worker %d to nice %d refused (%s); joins keep the worker's nice", l->tid, nice, strerror(error));
        return -1;
    }
    l->lent = 1; l->nice = nice;
    atomic_fetch_add_explicit(&g_boost_raised, 1, memory_order_relaxed);
    return 1;
}
void vk_pipe_boost_restore(const vk_lend *l) { if (l->lent) boost_set(0, VK_WORKER_NICE); }
void vk_pipe_boost_waited(uint64_t ns) {
    atomic_fetch_add_explicit(&g_boost_joins, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_boost_join_ns, ns, memory_order_relaxed);
}
/* TOTK_VK_UNWRITTEN_MASK=1 (default off): colour attachments the fragment shader has no output for keep their contents.
   NVN/Maxwell leaves such a bound target untouched; Vulkan leaves it undefined and Adreno writes black. The write mask
   derives from the key's fragment shader alone, so equal keys still build equal pipelines and the PSO key lists stay
   valid in both arms; only the output-interface library key carries the effective mask.
   Live (runtime/live_ctl.h): pipelines and output libraries built after a change use the new value; those already
   built keep their masks (the key does not carry the switch), so a live arm covers the pipelines it builds. The
   SPIR-V output span (vk_shader.fout_span) is kept for every fragment shader, so array outputs are never masked. */
static int g_unwritten_mask;   /* the launch value */
static totk_live_cache g_unwritten_live;
static _Atomic uint64_t g_unwritten_pipes, g_unwritten_attachments;
int vk_unwritten_mask_on(void) { return g_unwritten_mask; }
static int unwritten_mask_live(void) { return totk_live_cached(&g_unwritten_live, "TOTK_VK_UNWRITTEN_MASK", g_unwritten_mask); }
void vk_draw_switches_init(void) {
    const char *e = getenv("TOTK_VK_PIPE_BOOST"); g_pipe_boost = e && atoi(e) == 1;
    LOGV("TOTK_VK_PIPE_BOOST=%d: %s", g_pipe_boost, g_pipe_boost ?
         "a join on a compile worker's build lends the worker the waiter's nice until it publishes" :
         "compile workers keep nice 10 while joined (default)");
    e = getenv("TOTK_VK_UNWRITTEN_MASK"); g_unwritten_mask = e && atoi(e) == 1;
    LOGV("TOTK_VK_UNWRITTEN_MASK=%d: %s", g_unwritten_mask, g_unwritten_mask ?
         "colour attachments the fragment shader does not output get colorWriteMask 0" :
         "colour write masks follow the game's channel mask only (default)");
    (void)split_pass_draws();
    e = getenv("TOTK_VK_SPLIT_LRZ_SAFE"); g_split_safe_launch = !e || !*e || atoi(e) != 0;
    LOGV("TOTK_VK_SPLIT_LRZ_SAFE=%d: %s", g_split_safe_launch, g_split_safe_launch ?
         "depth epoch transfer touches and conservative split refusals (default)" : "unsafe split control arm");
    e = getenv("TOTK_VK_LOAD_ELIDE"); g_load_elide_launch = e && atoi(e) == 1;
    LOGV("[nvn-vk] TOTK_VK_LOAD_ELIDE=%d: conservative first-draw coverage proof, next pass open", g_load_elide_launch);
    e=getenv("TOTK_VK_FEEDBACK_NARROW"); g_feedback_narrow_launch=e && atoi(e)==1;
    LOGV("[nvn-vk] TOTK_VK_FEEDBACK_NARROW=%d: texture-only feedback boundaries, next pass open",g_feedback_narrow_launch);
    e=getenv("TOTK_VK_ALPHA_TEST"); g_alpha_launch=e && atoi(e)==1;
    LOGV("[nvn-vk] TOTK_VK_ALPHA_TEST=%d: next draw, fragment alpha rejection with immutable reference blocks",g_alpha_launch);
    e=getenv("TOTK_VK_CLEAR_ON_READ"); g_clear_read_launch=e && atoi(e)==1;
    LOGV("[nvn-vk] TOTK_VK_CLEAR_ON_READ=%d: next texture read, resolve pending attachment clears",g_clear_read_launch);
    e = getenv("TOTK_VK_FEEDBACK_CUT"); g_feedback_cut_launch = e && atoi(e) == 1;
    atomic_store_explicit(&g_feedback_cut, g_feedback_cut_launch, memory_order_relaxed);
    LOGV("TOTK_VK_FEEDBACK_CUT=%d: %s", g_feedback_cut_launch, g_feedback_cut_launch ?
         "sampled views of an image the pass renders to stop below the attached level" :
         "sampled views may include the attached level (default)");
    e = getenv("TOTK_VK_PREPARE_MASK"); g_prepare_off = vk_off("prepare"); g_prepare_mask_launch = e && atoi(e) == 1;
    e = getenv("TOTK_VK_PREPARE_CHECK"); g_prepare_check = e && atoi(e) == 1;
    LOGV("TOTK_VK_PREPARE_MASK=%d%s: %s%s", g_prepare_mask_launch, g_prepare_off ? " (TOTK_VK_OFF=prepare)" : "",
         g_prepare_mask_launch && !g_prepare_off ? "lagged uniform ranges are prepared only after a rebind or invalidation" :
         "every consumed lagged uniform range is prepared at each draw (default)",
         g_prepare_check ? "; TOTK_VK_PREPARE_CHECK=1 runs each skipped preparation as an oracle" : "");
}
static uint32_t pipe_write_mask(const pipe_key *k) {   /* the key's channel mask minus attachments without an output */
    if (!unwritten_mask_live()) return k->chmask;
    const vk_shader *fs = k->fs;
    uint32_t outputs = fs ? fs->fout_mask | fs->fout_span : 0, mask = k->chmask;
    for (unsigned i = 0; i < 8; i++) if (!(outputs >> i & 1)) mask &= ~(0xfu << (4 * i));
    return mask;
}
#include "vk_pso_runtime.inc"
static void pipe_enqueue(pipe_ent *e) {
    pthread_mutex_lock(&g_pq_mu);
    if (g_pq_tail) g_pq_tail->next = e; else g_pq_head = e;
    g_pq_tail = e; pthread_cond_signal(&g_pq_cv);
    pthread_mutex_unlock(&g_pq_mu);
}
static unsigned pipe_wait_max(void) {
    static int n = -1;
    if (n < 0) { const char *v = getenv("TOTK_VK_PIPE_WAIT_MAX"); n = v && *v ? atoi(v) : 90; n = n < 0 ? 0 : n > 10000 ? 10000 : n; }
    return (unsigned)n;   /* zero disables the bound; count distinct game submissions, not draws or GPU chunks */
}
static int topo_class(int prim) { return prim == 0 ? 0 : prim == 1 || prim == 2 || prim == 3 ? 1 : 2; }
static VkPrimitiveTopology topology(int prim) {
    switch (prim) {
        case 0: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST; case 1: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST; case 3: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
        case 5: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; case 6: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
    }
    return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;   /* triangles, quads (drawn as indexed triangles) */
}
/* No blending into integer targets; inactive targets must not distinguish pipelines (SavageWindRecomp's finding). */
static uint32_t pipeline_blend_enable_for(const vk_state *state) {
    uint32_t be = state->color.blend_en;
    for (uint32_t i = 0; i < R.ncolor && i < 8; i++) if (R.dclass[i]) be &= ~(1u << i);
    return be & (R.ncolor >= 8 ? 0xffu : (1u << R.ncolor) - 1);
}
static uint32_t pipeline_blend_enable(void) { return pipeline_blend_enable_for(&S); }
static void pipe_key_fill(pipe_key *k, const vk_state *state, vk_shader *vs, vk_shader *fs, int prim) {
    memset(k, 0, sizeof *k);
    k->vs = vs; k->fs = fs;
    memcpy(k->cfmt, R.cfmt, sizeof k->cfmt); k->dfmt = R.dfmt; k->sfmt = R.sfmt; k->ncolor = R.ncolor; k->samples = R.samples;
    k->blend_en = pipeline_blend_enable_for(state); k->chmask = state->chmask.mask; k->a2c = state->ms.a2c; k->poly_mode = (uint8_t)state->poly.mode; k->topo_class = (uint8_t)topo_class(prim);
    k->depth_clamp = (uint8_t)state->depth_clamp; k->logic_op = (uint8_t)state->color.logic_op;
    if (k->ncolor < 8) k->chmask &= (1u << (4 * k->ncolor)) - 1;
    for (uint32_t i = 0; i < k->ncolor && i < 8; i++) if (k->blend_en >> i & 1) k->blend[i] = state->blend[i];
    for (int a = 0; a < 16; a++) if (vs->vin_mask >> a & 1) {
        if (a < state->nattr && state->attr[a].fmt) {
            k->attr[a].fmt = state->attr[a].fmt; k->attr[a].offset = state->attr[a].offset; k->attr[a].stream = state->attr[a].stream;
            int st = state->attr[a].stream; if (st < 16) { k->stream[st].divisor = state->stream[st].divisor; k->stream[st].instanced = state->stream[st].divisor != 0 && state->stream[st].stride != 0; }
        }
    }
}
#define ZERO_BINDING 16   /* a zero vertex buffer for attributes the shader reads but the game left unset */
/* the pipeline of a key; flags VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT asks the pipeline cache alone */
static VkResult pipe_create_kind(const pipe_key *k, VkPipelineLayout layout, VkPipelineCreateFlags flags, VkPipeline *out, uint64_t frame, int synchronous, int part) {
    vk_shader *vs = k->vs, *fs = k->fs; VkPipeline p = VK_NULL_HANDLE;
    VkPipelineShaderStageCreateInfo st[3] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs->mod, .pName = "main"},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs ? fs->mod : VK_NULL_HANDLE, .pName = "main"}};
    VkPipelineShaderStageRequiredSubgroupSizeCreateInfo subgroup[2] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO, .requiredSubgroupSize = vs->required_subgroup_size},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO, .requiredSubgroupSize = fs ? fs->required_subgroup_size : 0}};
    if (vs->required_subgroup_size) st[0].pNext = &subgroup[0];
    if (fs && fs->required_subgroup_size) st[1].pNext = &subgroup[1];
    /* depth the fragment shader writes gets the same DEPTH16 nudge as rasterized depth (see d16tie_on) */
    float d16spec = D16_NUDGE_UNITS / 65535.0f; VkSpecializationMapEntry sme = {D16_SPEC_ID, 0, 4}; VkSpecializationInfo spi = {1, &sme, 4, &d16spec};
    if (fs && fs->depth_spec && k->dfmt == VK_FORMAT_D16_UNORM && d16tie_on()) st[1].pSpecializationInfo = &spi;
    VkVertexInputBindingDescription bd[17]; VkVertexInputAttributeDescription ad[32]; VkVertexInputBindingDivisorDescriptionEXT dd[16];
    uint32_t nb = 0, na = 0, nd = 0, used_streams = 0; int zero_used = 0;
    for (int a = 0; a < 32; a++) if (vs->vin_mask >> a & 1) {
        VkFormat vf = a < 16 && k->attr[a].fmt ? vk_vertex_fmt((int)k->attr[a].fmt) : VK_FORMAT_UNDEFINED;
        if (a < 16 && k->attr[a].fmt && !vf) RED(8, "vertex attribute format %#x has no Vulkan format", k->attr[a].fmt);
        if (!vf) {
            int cls = vs->vin_class[a];
            ad[na++] = (VkVertexInputAttributeDescription){(uint32_t)a, ZERO_BINDING, cls == 1 ? VK_FORMAT_R32G32B32A32_UINT : cls == 2 ? VK_FORMAT_R32G32B32A32_SINT : VK_FORMAT_R32G32B32A32_SFLOAT, 0};
            zero_used = 1; continue;
        }
        uint32_t s = k->attr[a].stream & 15;
        ad[na++] = (VkVertexInputAttributeDescription){(uint32_t)a, s, vf, k->attr[a].offset};
        used_streams |= 1u << s;
    }
    for (uint32_t s = 0; s < 16; s++) if (used_streams >> s & 1) {
        bd[nb++] = (VkVertexInputBindingDescription){s, 0, k->stream[s].instanced ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX};
        if (k->stream[s].instanced && k->stream[s].divisor > 1) {
            if (V.has_divisor) dd[nd++] = (VkVertexInputBindingDivisorDescriptionEXT){s, k->stream[s].divisor};
            else RED(4, "instance divisor %u needs VK_EXT_vertex_attribute_divisor", k->stream[s].divisor);
        }
    }
    if (zero_used) bd[nb++] = (VkVertexInputBindingDescription){ZERO_BINDING, 0, VK_VERTEX_INPUT_RATE_VERTEX};
    VkPipelineVertexInputDivisorStateCreateInfoEXT dsi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO_EXT, .vertexBindingDivisorCount = nd, .pVertexBindingDivisors = dd};
    VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, nd ? &dsi : NULL, 0, nb, bd, na, ad};
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = k->topo_class == 0 ? VK_PRIMITIVE_TOPOLOGY_POINT_LIST : k->topo_class == 1 ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .depthClampEnable = k->depth_clamp && V.props.limits.maxViewports, .polygonMode = k->poly_mode == NVN_POLYGON_MODE_LINE ? VK_POLYGON_MODE_LINE : k->poly_mode == NVN_POLYGON_MODE_POINT ? VK_POLYGON_MODE_POINT : VK_POLYGON_MODE_FILL, .lineWidth = 1};
    /* a transform feedback capture variant (vk_tf.inc) rasterizes its points (one fragment each at pixel (0,0), where its
       fragment shader stores the captured outputs) but writes no attachment: colour masks, blending and logic op off
       here, depth, stencil, bias and culling off in dynamic_state */
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = k->samples ? (VkSampleCountFlagBits)k->samples : VK_SAMPLE_COUNT_1_BIT, .alphaToCoverageEnable = k->a2c & 1};
    VkPipelineDepthStencilStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState cba[8];
    uint32_t chmask = pipe_write_mask(k), unwritten = 0;
    for (uint32_t i = 0; i < k->ncolor && i < 8; i++) {
        uint32_t m = chmask >> (4 * i) & 0xf;
        unwritten += k->cfmt[i] && (k->chmask >> (4 * i) & 0xf) && !m;
        cba[i] = (VkPipelineColorBlendAttachmentState){.colorWriteMask = (m & 1 ? VK_COLOR_COMPONENT_R_BIT : 0) | (m & 2 ? VK_COLOR_COMPONENT_G_BIT : 0) | (m & 4 ? VK_COLOR_COMPONENT_B_BIT : 0) | (m & 8 ? VK_COLOR_COMPONENT_A_BIT : 0)};
        if (k->blend_en >> i & 1 && k->cfmt[i]) {
            cba[i].blendEnable = VK_TRUE;
            cba[i].srcColorBlendFactor = vk_blend_factor(k->blend[i].src_rgb); cba[i].dstColorBlendFactor = vk_blend_factor(k->blend[i].dst_rgb); cba[i].colorBlendOp = vk_blend_op(k->blend[i].eq_rgb);
            cba[i].srcAlphaBlendFactor = vk_blend_factor(k->blend[i].src_a); cba[i].dstAlphaBlendFactor = vk_blend_factor(k->blend[i].dst_a); cba[i].alphaBlendOp = vk_blend_op(k->blend[i].eq_a);
        }
        if (!k->cfmt[i]) cba[i].colorWriteMask = 0;
    }
    VkPipelineColorBlendStateCreateInfo cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = k->ncolor, .pAttachments = cba};
    if (k->logic_op != NVN_LOGIC_OP_COPY && k->logic_op && V.has_logic_op) { cb.logicOpEnable = VK_TRUE; cb.logicOp = (VkLogicOp)(k->logic_op - 1 > 15 ? 3 : k->logic_op - 1); }
    if (vs->tf) {
        for (uint32_t i = 0; i < k->ncolor && i < 8; i++) { cba[i].colorWriteMask = 0; cba[i].blendEnable = VK_FALSE; }
        cb.logicOpEnable = VK_FALSE; ms.alphaToCoverageEnable = VK_FALSE; rs.depthClampEnable = VK_FALSE; rs.polygonMode = VK_POLYGON_MODE_FILL;
    }
    static const VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_DEPTH_BIAS, VK_DYNAMIC_STATE_BLEND_CONSTANTS,
        VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE, VK_DYNAMIC_STATE_CULL_MODE, VK_DYNAMIC_STATE_FRONT_FACE,
        VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY, VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE, VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE, VK_DYNAMIC_STATE_DEPTH_COMPARE_OP,
        VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE, VK_DYNAMIC_STATE_STENCIL_OP, VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE, VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE,
        VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE, VK_DYNAMIC_STATE_LINE_WIDTH};
    VkPipelineDynamicStateCreateInfo dy = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = sizeof dyn / sizeof *dyn, .pDynamicStates = dyn};
    VkPipelineRenderingCreateInfo pr = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = k->ncolor, .pColorAttachmentFormats = k->cfmt,
        .depthAttachmentFormat = k->dfmt, .stencilAttachmentFormat = k->sfmt};
    /* a vertex shader that writes Layer here has a pass-through geometry shader (vk_layer_gs.inc): VS, GS, FS */
    uint32_t ngs = vs->gs ? 1 : 0;
    if (ngs) { st[2] = st[1]; st[1] = (VkPipelineShaderStageCreateInfo){VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_GEOMETRY_BIT, .module = vs->gs, .pName = "main"}; }
    VkGraphicsPipelineCreateInfo ci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &pr, .flags = flags, .stageCount = (fs ? 2 : 1) + ngs, .pStages = st, .pVertexInputState = &vi,
        .pInputAssemblyState = &ia, .pViewportState = &vp, .pRasterizationState = &rs, .pMultisampleState = &ms, .pDepthStencilState = &ds, .pColorBlendState = &cb,
        .pDynamicState = &dy, .layout = layout};
    VkGraphicsPipelineLibraryCreateInfoEXT subset = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT};
    VkDynamicState subset_dyn[sizeof dyn / sizeof *dyn];
    if (part >= 0) {
        static const VkGraphicsPipelineLibraryFlagBitsEXT bits[PL_COUNT] = {
            VK_GRAPHICS_PIPELINE_LIBRARY_VERTEX_INPUT_INTERFACE_BIT_EXT,
            VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT,
            VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT,
            VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_OUTPUT_INTERFACE_BIT_EXT};
        subset.flags = bits[part]; subset.pNext = &pr; ci.pNext = &subset;
        ci.flags |= VK_PIPELINE_CREATE_LIBRARY_BIT_KHR | VK_PIPELINE_CREATE_RETAIN_LINK_TIME_OPTIMIZATION_INFO_BIT_EXT;
        ci.stageCount = part == PL_PRE_RASTER ? 1 + ngs : part == PL_FRAGMENT && fs ? 1 : 0;
        ci.pStages = part == PL_PRE_RASTER ? st : part == PL_FRAGMENT && fs ? st + 1 + ngs : NULL;
        ci.pVertexInputState = part == PL_VERTEX_INPUT ? &vi : NULL;
        ci.pInputAssemblyState = part == PL_VERTEX_INPUT ? &ia : NULL;
        ci.pViewportState = part == PL_PRE_RASTER ? &vp : NULL;
        ci.pRasterizationState = part == PL_PRE_RASTER ? &rs : NULL;
        ci.pMultisampleState = part == PL_FRAGMENT || part == PL_OUTPUT ? &ms : NULL;
        ci.pDepthStencilState = part == PL_FRAGMENT ? &ds : NULL;
        ci.pColorBlendState = part == PL_OUTPUT ? &cb : NULL;
        ci.layout = part == PL_PRE_RASTER || part == PL_FRAGMENT ? layout : VK_NULL_HANDLE;
        if (part != PL_OUTPUT) { pr.colorAttachmentCount = 0; pr.pColorAttachmentFormats = NULL; }
        if (part != PL_FRAGMENT && part != PL_OUTPUT) {
            pr.depthAttachmentFormat = VK_FORMAT_UNDEFINED; pr.stencilAttachmentFormat = VK_FORMAT_UNDEFINED;
        }
        dy.dynamicStateCount = 0; dy.pDynamicStates = subset_dyn;
        for (unsigned i = 0; i < sizeof dyn / sizeof *dyn; i++) {
            VkDynamicState d = dyn[i];
            int owner = d == VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY || d == VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE ||
                        d == VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE ? PL_VERTEX_INPUT :
                        d == VK_DYNAMIC_STATE_BLEND_CONSTANTS ? PL_OUTPUT :
                        d == VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK || d == VK_DYNAMIC_STATE_STENCIL_WRITE_MASK ||
                        d == VK_DYNAMIC_STATE_STENCIL_REFERENCE || d == VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE ||
                        d == VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE || d == VK_DYNAMIC_STATE_DEPTH_COMPARE_OP ||
                        d == VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE || d == VK_DYNAMIC_STATE_STENCIL_OP ? PL_FRAGMENT : PL_PRE_RASTER;
            if (owner == part) subset_dyn[dy.dynamicStateCount++] = d;
        }
    }
    uint64_t t0 = g_hitch_on ? host_now_ns() : 0;
    /* flags=0 cache: Vulkan internally synchronizes creation and concurrent snapshot extraction.
       Do not add a coarse backend lock across compiles. Opt-in pcache overlap telemetry measures interference. */
    int diag = synchronous && dm_wait_stats_enabled();
    int credit = synchronous && pipe_predict_on() && pipe_predict_adapt();
    int probe = (flags & VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT) != 0;
    int trace = synchronous && !probe && pso_trace_on();
    int measure = !probe && (diag || credit || trace);
    uint64_t build_start = measure ? host_now_ns() : 0;
    VkResult r = vk_pcache_graphics(&ci, &p);
    if (measure) {
        uint64_t elapsed = host_now_ns() - build_start;
        if (diag && !probe) pipe_diag_add(&g_pp_build_ns, elapsed);
        if (credit && !probe) nvn_scan_budget_build(&g_pp_budget, g_pp_frame, elapsed);
    }
    if (trace) pso_trace_target_log(k, "sync-build", frame, r, host_now_ns() - build_start);
    if (!r && (runtime_android_fps_auto_active() || runtime_fps_adaptive_selected()))
        runtime_android_fps_auto_pipeline(host_now_ns());
    if (!r && frame) {
        if (frame == totk_ft_work_frame()) totk_ft_count(FT_COUNT_PIPE_CREATES, 1);
        else totk_ft_count_frame(frame, FT_COUNT_PIPE_CREATES, 1);
    }
    if (r != VK_PIPELINE_COMPILE_REQUIRED) { HK_ADD(pipes, 1); HK_ADD(pipe_ns, host_now_ns() - t0); }
    if (part >= 0 && r && p) vkDestroyPipeline(V.dev, p, NULL);
    if (!r && part < 0 && unwritten) {
        atomic_fetch_add_explicit(&g_unwritten_pipes, 1, memory_order_relaxed);
        atomic_fetch_add_explicit(&g_unwritten_attachments, unwritten, memory_order_relaxed);
    }
    *out = r ? VK_NULL_HANDLE : p;
    return r;
}
static VkResult pipe_create(const pipe_key *k, VkPipelineLayout layout, VkPipelineCreateFlags flags, VkPipeline *out, uint64_t frame, int synchronous) {
    return pipe_create_kind(k, layout, flags, out, frame, synchronous, -1);
}
static pipe_library_key pipe_library_key_for(const pipe_key *k, VkPipelineLayout layout, int part) {
    pipe_library_key key = {0};
    if (part == PL_VERTEX_INPUT) {
        key.k.vs = k->vs; key.k.topo_class = k->topo_class;
        memcpy(key.k.attr, k->attr, sizeof k->attr); memcpy(key.k.stream, k->stream, sizeof k->stream);
    } else if (part == PL_PRE_RASTER) {
        key.k.vs = k->vs; key.layout = layout; key.k.poly_mode = k->poly_mode; key.k.depth_clamp = k->depth_clamp;
    } else if (part == PL_FRAGMENT) {
        key.k.fs = k->fs; key.layout = layout; key.k.dfmt = k->dfmt; key.k.sfmt = k->sfmt;
        key.k.samples = k->samples; key.k.a2c = k->a2c;
    } else {
        memcpy(key.k.cfmt, k->cfmt, sizeof k->cfmt); key.k.dfmt = k->dfmt; key.k.sfmt = k->sfmt;
        key.k.ncolor = k->ncolor; key.k.samples = k->samples; key.k.a2c = k->a2c;
        memcpy(key.k.blend, k->blend, sizeof k->blend);
        key.k.blend_en = k->blend_en; key.k.chmask = pipe_write_mask(k); key.k.logic_op = k->logic_op;
    }
    return key;
}
static VkResult pipe_library_build(pipe_ent *e) {
    for (int part = 0; part < PL_COUNT; part++) {
        pipe_library_key key = pipe_library_key_for(&e->k, e->layout, part);
        pthread_mutex_lock(&g_library_mu);
        pipe_library *lib = nvn_hm_get(&g_libraries[part], &key);
        if (!lib) {
            lib = calloc(1, sizeof *lib);
            if (!lib) { pthread_mutex_unlock(&g_library_mu); return VK_ERROR_OUT_OF_HOST_MEMORY; }
            nvn_hm_put(&g_libraries[part], &key, lib);
        }
        while (lib->building) pthread_cond_wait(&g_library_cv, &g_library_mu);
        if (lib->failed) { pthread_mutex_unlock(&g_library_mu); return VK_ERROR_UNKNOWN; }   /* no retry per key */
        if (lib->p) { pthread_mutex_unlock(&g_library_mu); continue; }
        lib->building = 1;
        pthread_mutex_unlock(&g_library_mu);
        VkPipeline p; uint64_t started = host_now_ns();
        VkResult r = pipe_create_kind(&e->k, e->layout, 0, &p, 0, 0, part);
        uint64_t elapsed = host_now_ns() - started;
        pso_trace_log(&e->k, "build", "library-build", e->frame, r, elapsed, 0, part);
        atomic_fetch_add(&g_library_stats.parts[part], 1);
        atomic_fetch_add(&g_library_stats.part_ns[part], elapsed);
        pthread_mutex_lock(&g_library_mu);
        lib->p = p; lib->building = 0; lib->failed = r != VK_SUCCESS;
        pthread_cond_broadcast(&g_library_cv); pthread_mutex_unlock(&g_library_mu);
        if (r) return r;
    }
    return VK_SUCCESS;
}
static VkPipeline pipe_fast_get(pipe_ent *e) {
    if (!V.has_pipeline_library) return VK_NULL_HANDLE;
    if (!e->fast && !e->fast_attempted) {
        VkPipeline parts[PL_COUNT]; int ready = 1;
        pthread_mutex_lock(&g_library_mu);
        for (int part = 0; part < PL_COUNT; part++) {
            pipe_library_key key = pipe_library_key_for(&e->k, e->layout, part);
            pipe_library *lib = nvn_hm_get(&g_libraries[part], &key);
            parts[part] = lib ? lib->p : VK_NULL_HANDLE;
            if (!parts[part]) ready = 0;
        }
        pthread_mutex_unlock(&g_library_mu);
        if (!ready) return VK_NULL_HANDLE;
        e->fast_attempted = 1;
        VkPipelineLibraryCreateInfoKHR libs = {VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR,
            .libraryCount = PL_COUNT, .pLibraries = parts};
        VkGraphicsPipelineCreateInfo ci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &libs, .layout = e->layout};
        uint64_t started = host_now_ns();
        VkPipeline p = VK_NULL_HANDLE;
        VkResult r = vk_pcache_graphics(&ci, &p);
        uint64_t elapsed = host_now_ns() - started;
        pso_trace_log(&e->k, "build", "library-build", e->frame, r, elapsed, 0, PL_COUNT);
        if (dm_wait_stats_enabled()) pipe_diag_add(&g_pp_build_ns, elapsed);
        atomic_fetch_add(&g_library_stats.fast_link_ns, elapsed);
        uint64_t max = atomic_load(&g_library_stats.fast_link_max_ns);
        while (elapsed > max && !atomic_compare_exchange_weak(&g_library_stats.fast_link_max_ns, &max, elapsed)) {}
        max = atomic_load(&g_library_stats.fast_link_window_max_ns);
        while (elapsed > max && !atomic_compare_exchange_weak(&g_library_stats.fast_link_window_max_ns, &max, elapsed)) {}
        if (!r) {
            e->fast = p; atomic_fetch_add(&g_library_stats.fast_links, 1);
            if (g_pso_collect) { pthread_mutex_lock(&g_pq_mu); pso_collect_success(e); pthread_mutex_unlock(&g_pq_mu); }
        } else {
            if (p) vkDestroyPipeline(V.dev, p, NULL);
            LOGV("[pipeline-library] fast link failed: %d; keeping ordinary worker/backoff path", r);
        }
    }
    g_draw_fast = e->fast != VK_NULL_HANDLE;
    return e->fast;
}
static VkPipeline pipe_ready_get(pipe_ent *e) {
    g_draw_fast = 0;
    if (e->fast && !e->replaced) {
        e->replaced = 1;
        atomic_fetch_add(&g_library_stats.optimized_replacements, 1);
        atomic_fetch_add(&g_library_stats.optimized_ns, e->optimized_ns);
        /* Keep the old handle with its cache entry, just like the optimized handle: submissions/secondary
           recorders can still reference it. No device idle or unsafe immediate destruction at replacement. */
    }
    return e->p;
}
/* Exactly one builder owns an entry. Publication and prewarm accounting are shared by workers and bounded takeover.
   Returns the nice a waiter lent the building worker; once published no waiter can lend it again. */
static vk_lend pipe_publish(pipe_ent *e, VkResult r, VkPipeline p, uint64_t dt) {
    pthread_mutex_lock(&g_pq_mu);
    if (r) {
        HK_ADD(pipes_fail, 1);
        if (!e->failures)
            LOGV("pipeline failure key %p (vs %s fs %s): %d; retry next submission, then back off 30..300 submissions (never permanent skip)",
                 (void *)e, ((vk_shader *)e->k.vs)->key, e->k.fs ? ((vk_shader *)e->k.fs)->key : "-", r);
        e->failures++;
    } else if (e->failures) LOGV("pipeline recovered key %p after %u failed attempts", (void *)e, e->failures);
    if (!r) pso_collect_success(e);
    pso_complete(e, r, dt);
    e->optimized_ns = dt;
    e->p = p;
    atomic_store_explicit(&e->state, r ? PE_FAILED : PE_READY, memory_order_release);
    vk_lend lend = e->lend; e->lend = (vk_lend){0};
    pthread_cond_broadcast(&g_pq_cv);
    pthread_mutex_unlock(&g_pq_mu);
    return lend;
}
static int pipe_pending(int state) { return state == PE_QUEUED || state == PE_BUILDING; }
/* Remove a queued entry to build it on the walk. An in-flight build is joined, never duplicated or overwritten. */
static int pipe_build_now(pipe_ent *e) {
    pthread_mutex_lock(&g_pq_mu);
    int state = atomic_load_explicit(&e->state, memory_order_acquire), build = state == PE_FAILED;
    if (build) e->frame = totk_ft_work_frame(); /* a retry is a new request; queued takeovers retain their origin */
    if (state == PE_QUEUED) {
        pipe_ent *prev = NULL, *q = g_pq_head;
        while (q && q != e) { prev = q; q = q->next; }
        if (q) {
            if (prev) prev->next = e->next; else g_pq_head = e->next;
            if (g_pq_tail == e) g_pq_tail = prev;
            e->next = NULL; build = 1;
        }
    }
    if (build) atomic_store_explicit(&e->state, PE_BUILDING, memory_order_release);
    else if (pipe_pending(state)) {
        int diag = dm_wait_stats_enabled(), refused = 0;
        uint64_t started = host_now_ns();
        while (pipe_pending(state)) {
            /* A queued entry gets its worker when one takes it; lend again after any wake while unlent. */
            if (!refused && vk_pipe_boost_lend(&e->lend) < 0) refused = 1;
            pthread_cond_wait(&g_pq_cv, &g_pq_mu);
            state = atomic_load_explicit(&e->state, memory_order_acquire);
        }
        uint64_t waited = host_now_ns() - started;
        vk_pipe_boost_waited(waited);
        if (diag) pipe_diag_add(&g_pp_wait_ns, waited);
    }
    pthread_mutex_unlock(&g_pq_mu);
    if (build) {
        uint64_t t0 = host_now_ns(); VkPipeline p;
        HK_ADD(pipes_sync, 1);
        int warming = pso_sync_begin();
        VkResult r = pipe_create(&e->k, e->layout, 0, &p, e->frame, 1);
        pso_sync_end(warming);
        uint64_t dt = host_now_ns() - t0;
        pso_trace_log(&e->k, "build", state == PE_FAILED ? "retry" : "takeover",
                      e->frame, r, dt, 0, -1);
        g_win_sync++; g_win_sync_ns += dt;
        vk_overlay_note_compile(t0 + dt, dt);
        pipe_publish(e, r, p, dt);
        state = r ? PE_FAILED : PE_READY;
    }
    return state;
}
static void *pipe_worker(void *arg) {
    char name[16]; snprintf(name, sizeof name, "totk-pipe%u", (unsigned)(uintptr_t)arg);
    totk_thread_start(name, NULL);
    setpriority(PRIO_PROCESS, 0, VK_WORKER_NICE);   /* this thread: below the game's threads */
    int self = vk_thread_tid();
    for (;;) {
        pthread_mutex_lock(&g_pq_mu);
        int background = 0;
        for (;;) {
            if (g_pq_head) break;
            if (g_pw_head && (uintptr_t)arg == 0 && pso_allow_background(g_pq_active != 0, g_pso_sync,
                    host_now_ns() < atomic_load_explicit(&g_pso_pressure_until, memory_order_relaxed),
                    g_pw_active, host_now_ns(), g_pw_pause_until)) { background = 1; break; }
            if (!g_pw_head) pthread_cond_wait(&g_pq_cv, &g_pq_mu);
            else {
                struct timespec wake; clock_gettime(CLOCK_REALTIME, &wake); wake.tv_nsec += 100000000;
                if (wake.tv_nsec >= 1000000000) { wake.tv_nsec -= 1000000000; wake.tv_sec++; }
                pthread_cond_timedwait(&g_pq_cv, &g_pq_mu, &wake);
            }
        }
        pipe_ent *e;
        if (background) {
            e = g_pw_head; g_pw_head = e->next; if (!g_pw_head) g_pw_tail = NULL;
            g_pw_active = 1;
#ifdef TOTK_VK_TESTS
            if (!e->capture) g_pso_test_screen_started = 1;
            else if (g_pso_test_screen_started) g_pso_test_order_bad = 1;
#endif
        } else { e = g_pq_head; g_pq_head = e->next; if (!g_pq_head) g_pq_tail = NULL; }
        int demand_active = !background && atomic_load_explicit(&g_pso_warming, memory_order_relaxed);
        g_pq_active += demand_active;
        e->next = NULL; e->lend = (vk_lend){.tid = self}; atomic_store_explicit(&e->state, PE_BUILDING, memory_order_release);
        pthread_mutex_unlock(&g_pq_mu);
        uint64_t t0 = host_now_ns(); VkPipeline p;
        /* Library parts only enable fast links for later draws; a part failure never fails the entry itself. */
        if (!e->list_index && V.has_pipeline_library && pipe_library_build(e)) RED(4, "[pipeline-library] part creation failed; ordinary pipeline build continues");
        uint64_t build_started = pso_trace_on() ? host_now_ns() : 0;
        VkResult r = e->list_index && V.has_cache_control ? pipe_create(&e->k, e->layout,
            VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT, &p, e->frame, 0) : VK_PIPELINE_COMPILE_REQUIRED;
        if (r == VK_PIPELINE_COMPILE_REQUIRED) r = pipe_create(&e->k, e->layout, 0, &p, e->frame, 0);
        if (build_started) pso_trace_log(&e->k, "build", e->list_index ? "prewarm-worker" : "async-worker",
                                        e->frame, r, host_now_ns() - build_started, 0, -1);
        uint64_t dt = host_now_ns() - t0; g_win_async++; g_win_async_ns += dt;
        vk_lend lent = pipe_publish(e, r, p, dt);
        vk_pipe_boost_restore(&lent);
        if (background || demand_active) {
            pthread_mutex_lock(&g_pq_mu);
            if (background) { g_pw_active = 0; g_pw_pause_until = host_now_ns()+10000000; }
            g_pq_active -= demand_active;
            if (!g_pw_head && !g_pw_active) atomic_store_explicit(&g_pso_warming, 0, memory_order_relaxed);
            pthread_cond_broadcast(&g_pq_cv); pthread_mutex_unlock(&g_pq_mu);
        }
    }
    return NULL;
}
static int compile_threads(void) {
    static int n = -1;
    if (n < 0) {
        const char *e = getenv("TOTK_VK_COMPILE_THREADS"); n = e && *e ? atoi(e) : 2; n = n < 0 ? 0 : n > 8 ? 8 : n;
        e = getenv("TOTK_PIPE_WORKERS");
        if (e && *e) { n = atoi(e); n = n < 1 ? 1 : n > 4 ? 4 : n; }
        if (n && !V.has_cache_control) { LOGV("pipelines compile at the draw: the driver lacks pipelineCreationCacheControl"); n = 0; }
        for (int i = 0; i < n; i++) { pthread_t t; if (pthread_create(&t, NULL, pipe_worker, (void *)(uintptr_t)i)) { n = i; break; } pthread_detach(t); }
        LOGV("pipelines the driver has to compile: %s", n ? "on worker threads, their draws skipped until ready (TOTK_VK_COMPILE_THREADS)" : "at the draw");
    }
    return n;
}
/* A Vulkan pass already has exact attachment formats. Predict only within that target set:
   crossing an attachment change would require speculative resource/view realization. Calls, jumps,
   conditions, transfers, updates and registry changes are boundaries, never executed by this scan.
   The shadow is private: no live state, shader selection or recorder/lag memo is changed. */
static int pipe_predict_limit(const char *name, int dflt, int lo, int hi) {
    const char *e = getenv(name); int n = e ? atoi(e) : dflt; return n < lo ? lo : n > hi ? hi : n;
}
static void pipe_predict_maybe(const uint8_t *next) {
    if (!pipe_predict_on() || !next || !R.open || !compile_threads()) return;
    static int max_ops, max_keys, max_us, frame_us, rescans, initialized;
    if (!initialized) {
        max_ops = pipe_predict_limit("TOTK_PSO_PREDICT_SCAN", 8192, 1, 1 << 20);
        max_keys = pipe_predict_limit("TOTK_PSO_PREDICT_KEYS", 32, 1, 160);
        max_us = pipe_predict_limit("TOTK_PSO_PREDICT_US", 2000, 1, 1000000);
        frame_us = pipe_predict_limit("TOTK_PSO_PREDICT_FRAME_US", 4000, 0, 1000000);
        rescans = pipe_predict_limit("TOTK_PSO_RESCANS", 32, 0, 1024);
        initialized = 1;
    }
    uint64_t sub = vk_game_submit_serial();
    if (g_pp_submit != sub) { g_pp_submit = sub; g_pp_scans_in_sub = 0; g_pp_more = 1; }
    if (g_pp_scans_in_sub && (!g_pp_more && g_pp_pass == g_stat_passes)) return;
    if (g_pp_scans_in_sub > (unsigned)rescans) return;   /* first scan + bounded rescans */
    int adapt = pipe_predict_adapt();
    uint64_t fixed = (uint64_t)frame_us * 1000;
    if (!nvn_scan_budget_left(&g_pp_budget, g_pp_frame, fixed, adapt)) {
        g_pp_more = adapt; return;   /* a later actual build may earn another scan */
    }
    uint64_t allowance = adapt && g_pp_budget.sync_ns > fixed ? g_pp_budget.sync_ns : fixed;
    uint64_t scan_ns = MINV((uint64_t)max_us * 1000, allowance - g_pp_budget.used_ns);
    uint64_t started = host_now_ns();
    vk_state predicted = S;
    unsigned ops = 0, keys = 0, stop = 0;
    g_pp_pass = g_stat_passes; g_pp_scans_in_sub++;
    for (;;) {
        if (ops >= (unsigned)max_ops) { stop = 1; break; }
        if (keys >= (unsigned)max_keys) { stop = 2; break; }
        if (host_now_ns() - started >= scan_ns) { stop = 3; break; }
        op_hdr h; memcpy(&h, next, sizeof h); const void *a = next + sizeof h;
        if (h.size < sizeof h || (h.size & 7) || h.op >= OP_COUNT_) break;
        ops++;
#define PP_NEED(type) do { if (h.size < sizeof h + sizeof(type)) goto predicted_done; } while (0)
        switch (h.op) {
            case OP_BIND_PROGRAM: PP_NEED(p_bind_program); predicted.prog = ((const p_bind_program *)a)->prog; break;
            case OP_BIND_STATE: {
                PP_NEED(p_bind_state); const p_bind_state *b = a; blend_st bs;
                switch (b->kind) {
                    case 0: memcpy(&bs, b->bytes, sizeof bs); predicted.blend[bs.target & 7] = bs; break;
                    case 1: memcpy(&predicted.color, b->bytes, sizeof predicted.color); break;
                    case 2: memcpy(&predicted.chmask, b->bytes, sizeof predicted.chmask); break;
                    case 3: memcpy(&predicted.ms, b->bytes, sizeof predicted.ms); break;
                    case 4: memcpy(&predicted.poly, b->bytes, sizeof predicted.poly); break;
                    case 5: break;   /* dynamic depth/stencil state is not a Vulkan pipeline key input */
                    default: goto predicted_done;
                }
                break;
            }
            case OP_BIND_VATTR: {
                PP_NEED(p_bind_vattr); const p_bind_vattr *b = a;
                if (b->n < 0 || b->n > 16) goto predicted_done;
                memcpy(predicted.attr, b->st, (size_t)b->n * sizeof *b->st); predicted.nattr = b->n; break;
            }
            case OP_BIND_VSTREAM: {
                PP_NEED(p_bind_vstream); const p_bind_vstream *b = a;
                if (b->n < 0 || b->n > 16) goto predicted_done;
                memcpy(predicted.stream, b->st, (size_t)b->n * sizeof *b->st); predicted.nstream = b->n; break;
            }
            case OP_BIND_TEX: case OP_BIND_IMG: {
                if (h.size < sizeof h + offsetof(p_bind_handles, h)) goto predicted_done;
                const p_bind_handles *b = a; int count = h.op == OP_BIND_TEX ? 128 : 32;
                if (b->stage < 0 || b->stage >= 6 || b->n < 0 || b->n > count ||
                    b->first < 0 || b->first > count - b->n ||
                    h.size < sizeof h + offsetof(p_bind_handles, h) + (size_t)b->n * 8) goto predicted_done;
                if (h.op == OP_BIND_TEX) {
                    memcpy(&predicted.tex[b->stage][b->first], b->h, (size_t)b->n * 8);
                    if (b->first + b->n > predicted.tex_hi[b->stage]) predicted.tex_hi[b->stage] = b->first + b->n;
                } else memcpy(&predicted.img[b->stage][b->first], b->h, (size_t)b->n * 8);
                break;
            }
            case OP_DEPTH_CLAMP: PP_NEED(p_flag); predicted.depth_clamp = ((const p_flag *)a)->value; break;
            case OP_RENDER_ENABLE: PP_NEED(p_flag); predicted.render_disabled = !((const p_flag *)a)->value; break;
            case OP_RASTER_DISCARD: PP_NEED(p_flag); predicted.raster_discard = ((const p_flag *)a)->value; break;
            case OP_ALPHA_REF: PP_NEED(p_alpha_ref); break; /* reference bytes are not a pipeline input */
            case OP_SET_RT:
                PP_NEED(p_set_rt);
                if (!rt_same(&predicted.rt, a)) goto predicted_done;
                break;
            case OP_DRAW: case OP_DRAW_IDX: {
                int prim, count;
                if (h.op == OP_DRAW) { PP_NEED(p_draw); prim = ((const p_draw *)a)->prim; count = ((const p_draw *)a)->count; }
                else { PP_NEED(p_draw_idx); prim = ((const p_draw_idx *)a)->prim; count = ((const p_draw_idx *)a)->count; }
                if (!count || prim < 0 || prim == 2 || prim >= 8 || (h.op == OP_DRAW_IDX && prim == 7) ||
                    predicted.render_disabled || predicted.raster_discard || predicted.poly.cull == NVN_FACE_FRONT_AND_BACK) break;
                /* Prediction never loads modules. Alpha variants are demand-created;
                 * do not speculate the base module's (wrong) pipeline for this draw. */
                if (predicted.color.alpha_func && predicted.color.alpha_func!=NVN_ALPHA_FUNC_ALWAYS &&
                    totk_live_cached(&g_alpha_live,"TOTK_VK_ALPHA_TEST",g_alpha_launch)) break;
                vk_shader *sh[6]; if (!vk_prog_predict(&predicted, sh)) break;
                pipe_key k; pipe_key_fill(&k, &predicted, sh[0], sh[1], prim);
                if (nvn_hm_get(&g_pipes, &k)) break;   /* ready and pending entries remain authoritative */
                pipe_ent *e = calloc(1, sizeof *e); if (!e) goto predicted_done;
                e->k = k; e->layout = vk_layout_of(sh[0], sh[1]); e->frame = totk_ft_work_frame(); e->predicted = 1;
                nvn_hm_put(&g_pipes, &k, e); g_stat_pipes++; keys++;
                pipe_enqueue(e);
                break;
            }
            case OP_BEGIN: case OP_CLEAR_COLOR: case OP_CLEAR_DS: case OP_SCISSOR: case OP_VIEWPORTS:
            case OP_DEPTH_RANGE: case OP_BIND_VBUF: case OP_BIND_UBO: case OP_BIND_SSBO: case OP_BIND_SAMP:
            case OP_STENCIL: case OP_POLY_OFFSET: case OP_BLEND_COLOR: case OP_COPY_STRIDE: case OP_TF_BIND: case OP_TF_STATE: case OP_PUSH_CONST:
                break;   /* these do not select Vulkan shader variants or fixed pipeline state */
            default: goto predicted_done;
        }
#undef PP_NEED
        next += h.size;
    }
predicted_done:;
    uint64_t elapsed = host_now_ns() - started;
    nvn_scan_budget_spend(&g_pp_budget, g_pp_frame, elapsed);
    if (dm_wait_stats_enabled()) pipe_diag_add(&g_pp_scan_ns, elapsed);
    g_pp_more = stop && keys;   /* only a productive bounded scan earns a rescan, not more time */
#ifdef TOTK_VK_TESTS
    g_pp_test.scans++; g_pp_test.ops = ops; g_pp_test.keys = keys; g_pp_test.stop = stop;
#endif
}
#ifdef TOTK_VK_TESTS
void vk_pipeline_test_predict(const uint8_t *ops) { pipe_predict_maybe(ops); }
#endif

void vk_pipeline_prewarm(void) {
#ifndef TOTK_PSO_COLLECT_DEFAULT
#define TOTK_PSO_COLLECT_DEFAULT 0
#endif
    const char *collect = getenv("TOTK_PSO_COLLECT");
    g_pso_collect = collect ? atoi(collect) != 0 : TOTK_PSO_COLLECT_DEFAULT;
    const char *current_cache = vk_pcache_current_path();
    if (g_pso_collect && !pso_collection_path(current_cache)) {
        RED(4, "[pso-collect] cache path too long; collection disabled without touching the cache"); g_pso_collect = 0;
    }
    if (g_pso_collect) {
        g_pso_records = pso_read(g_pso_path, &g_pso_count, NULL);
        g_pso_capacity = g_pso_count;
        for (uint32_t i = 0; i < g_pso_count; i++) nvn_hm_put(&g_pso_keys, g_pso_records[i].key, (void *)(uintptr_t)(i+1));
        LOGV("[pso-collect] enabled; retained %u successful graphics keys", g_pso_count);
    }
    const char *path = getenv("TOTK_PSO_KEYS"), *enable = getenv("TOTK_PSO_PREWARM");
    pso_record *records = path && (!enable || atoi(enable)) ? pso_read(path, &g_pw_total, g_pw_digest) : NULL;
    if (path && (!enable || atoi(enable)) && !records) LOGV("[pso-prewarm] rejected unreadable/incompatible/checksum-failed list: %s", path);
    const char *cache = vk_pcache_current_path();
    if (cache) snprintf(g_pw_path, sizeof g_pw_path, "%s.pso-state", cache);
    g_pw_started = host_now_ns();
    pso_restore();
    pipe_ent *head = NULL, *tail = NULL;
    if (records && g_pw_total > g_pw_completed && compile_threads()) {
        for (int priority = 1; priority >= 0; priority--) for (uint32_t i = 0; i < g_pw_total; i++) {
            if (records[i].capture != (uint32_t)priority) continue;
            if (g_pw_done[i/8] >> (i%8) & 1) continue;
            pso_record *r = records+i; pipe_key k; pso_decode(r, &k);
            k.vs = vk_shader_seed((const char *)r->key, 0, 0);
            k.fs = r->key[80] ? vk_shader_seed((const char *)r->key+80, 1, pso_u32(r->key+160)) : NULL;
            if (!k.vs || (r->key[80] && !k.fs)) { g_pw_missing++; continue; }
            if (nvn_hm_get(&g_pipes, &k)) continue;
            pipe_ent *e = calloc(1, sizeof *e);
            if (!e) { RED(4, "[pso-prewarm] entry allocation failed"); break; }
            e->k = k; e->layout = vk_layout_of(k.vs, k.fs); e->list_index = i+1;
            e->capture = r->capture; e->predicted = 1; /* first demand must build/join, never skip a formerly cached draw */
            nvn_hm_put(&g_pipes, &k, e);
            if (tail) tail->next = e; else head = e; tail = e;
        }
    }
    free(records);
    if (path) LOGV("[pso-prewarm] list=%s resumed=%u/%u missing=%u startup_ms=%.1f; background_worker_limit=1, demand first", path, g_pw_completed, g_pw_total, g_pw_missing, (host_now_ns()-g_pw_started)/1e6);
    pthread_mutex_lock(&g_pq_mu);
    g_pw_head = head; g_pw_tail = tail;
    atomic_store_explicit(&g_pso_warming, head != NULL, memory_order_relaxed);
    atomic_store_explicit(&g_pso_ready, g_pso_collect || g_pw_total, memory_order_release);
    pthread_cond_broadcast(&g_pq_cv); pthread_mutex_unlock(&g_pq_mu);
}
static VkPipeline pipe_get(vk_shader *vs, vk_shader *fs, int prim, VkPipelineLayout layout) {
    static pipe_ent *last;   /* entries never move or die; only the recording thread reads this memo */
    static pipe_ent *memo[64];   /* bounded exact-key front cache for alternating shaders/topologies */
    pipe_ent *e = last; pipe_key k;
    g_draw_fast = 0;
    int screen_aspect = pipe_screen_class() != 0;
    int capture = !screen_aspect && R.ncolor;
    if (g_pipe_dirty || !e || e->layout != layout || e->k.vs != vs || e->k.fs != fs || e->k.topo_class != topo_class(prim)) {
        pipe_key_fill(&k, &S, vs, fs, prim); g_pipe_dirty = 0;
        if (!e || e->layout != layout || memcmp(&e->k, &k, sizeof k)) {
            unsigned h = (unsigned)(((uintptr_t)vs >> 4) ^ ((uintptr_t)fs >> 8) ^ k.topo_class) & 63u;
            pipe_ent *candidate = memo[h];
            e = candidate && candidate->layout == layout && !memcmp(&candidate->k, &k, sizeof k) ?
                candidate : nvn_hm_get(&g_pipes, &k);
            if (e) memo[h] = e;
        }
    }
    /* A memo hit still observes a worker's publication, including queued and failed entries. */
    if (e) {
        pso_demand(e, capture);
        last = e; int s = atomic_load_explicit(&e->state, memory_order_acquire);
        if (s == PE_READY) {
            return pipe_ready_get(e);
        }
        VkPipeline fast = !e->predicted && !capture ? pipe_fast_get(e) : VK_NULL_HANDLE;
        if (fast && pipe_pending(s)) return fast;
        uint64_t sub = vk_game_submit_serial();
        unsigned bound = pipe_wait_max();
        /* Only opt-in prediction-origin entries join at their first demand (upstream pso_pend_take).
           Skipping one could hide a draw that the original cache-only probe would have satisfied.
           Ordinary screen misses retain async/skipped-submission bounds; captures always join/build. */
        if (pipe_pending(s) && (e->predicted || capture || (bound && e->wait_sub != sub && e->skipped >= bound))) {
            uint64_t t0 = host_now_ns();
            if (!capture && !e->predicted) { HK_ADD(pipe_bounds, 1); LOGV("pipeline key %p reached %u skipped submissions: build/join synchronously", (void *)e, e->skipped); }
            pipe_predict_maybe(vk_exec_draw_next());
            s = pipe_build_now(e);
            uint64_t dt = host_now_ns() - t0; g_win_prewarm_wait++; g_win_prewarm_wait_ns += dt; g_cum_pipe_ns += dt;
        }
        if (s == PE_FAILED) {
            if (e->seen_failure != e->failures) {
                e->seen_failure = e->failures;
                unsigned shift = e->failures > 5 ? 4 : e->failures > 1 ? e->failures - 2 : 0;
                unsigned delay = e->failures == 1 ? 1 : 30u << shift;
                e->retry_at = sub + (delay > 300 ? 300 : delay);
            }
            if (sub >= e->retry_at) {
                pipe_predict_maybe(vk_exec_draw_next());
                HK_ADD(pipe_retries, 1); uint64_t t0 = host_now_ns();
                s = pipe_build_now(e);   /* one retry per backoff, not per draw; never cache a permanent failure */
                g_cum_pipe_ns += host_now_ns() - t0;
            }
        }
        if (s != PE_READY && fast) return fast;
        if (pipe_pending(s)) {
            if (e->wait_sub != sub) { e->wait_sub = sub; e->skipped++; }
            HK_ADD(pipe_skips, 1);
            if (V.has_pipeline_library) atomic_fetch_add(&g_library_stats.library_skips, 1);
        }
        if (s == PE_FAILED && V.has_pipeline_library) atomic_fetch_add(&g_library_stats.library_skips, 1);
        return s == PE_READY ? pipe_ready_get(e) : s == PE_FAILED ? PIPE_FAILED : VK_NULL_HANDLE;
    }
    e = calloc(1, sizeof *e); e->k = k; e->layout = layout; e->frame = totk_ft_work_frame(); last = e;
#ifdef TOTK_OHOS
    if (vs && vs->vary_pruned) vk_shader_prune_pipe();   /* oot-a108: the statistics line of vk_shader_prune */
#endif
    e->capture = capture;
    nvn_hm_put(&g_pipes, &k, e);
    unsigned h = (unsigned)(((uintptr_t)vs >> 4) ^ ((uintptr_t)fs >> 8) ^ k.topo_class) & 63u;
    memo[h] = e;
    g_stat_pipes++;
    /* Non-screen colour targets retain the Gerudo capture guard: skipping 64x64 B10G11R11 + depth
       draws permanently changed plaza lighting (09a79bca6). Recognize aligned scene extents before this
       guard: e.g. requested960x540 renders960x536 and must not be mistaken for a persistent probe.
       Depth-only shadow maps retain the existing worker policy; no capture replay/deferral is added. */
    int async = compile_threads() > 0 && (screen_aspect || !R.ncolor);
    pso_trace_target_log(&e->k, "demand", e->frame, VK_SUCCESS, 0);
    uint64_t t0 = host_now_ns(); VkPipeline p;
    int cache_probe = V.has_cache_control;
    uint64_t probe_started = pso_trace_on() ? host_now_ns() : 0;
    VkResult r = pipe_create(&e->k, layout, cache_probe ? VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT : 0,
                             &p, e->frame, !V.has_cache_control);
    if (probe_started) pso_trace_log(&e->k, cache_probe ? "probe" : "build",
                                     cache_probe ? "cache-probe" : "workers-disabled-sync", e->frame,
                                     r, host_now_ns() - probe_started,
                                     cache_probe ? VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT : 0, -1);
    if (r == VK_PIPELINE_COMPILE_REQUIRED) {
        VkPipeline fast = async ? pipe_fast_get(e) : VK_NULL_HANDLE;
        /* Queue the actual draw before speculative jobs; a synchronous miss can overlap their builds. */
        if (async) { e->wait_sub = vk_game_submit_serial(); e->skipped = fast ? 0 : 1; pipe_enqueue(e); }
        pipe_predict_maybe(vk_exec_draw_next());
        if (fast) {
            uint64_t dt = host_now_ns() - t0;
            g_win_pipes++; g_win_pipe_ns += dt; g_cum_pipe_ns += dt;
            if (dt > g_win_pipe_max_ns) g_win_pipe_max_ns = dt;
            return fast;
        }
    }
    if (!async && (r == VK_PIPELINE_COMPILE_REQUIRED || !V.has_cache_control)) {
        uint64_t c0 = host_now_ns();
        int warming = pso_sync_begin();
        if (r == VK_PIPELINE_COMPILE_REQUIRED) {
            uint64_t build_started = pso_trace_on() ? host_now_ns() : 0;
            r = pipe_create(&e->k, layout, 0, &p, e->frame, 1);
            if (build_started) pso_trace_log(&e->k, "build", capture ? "capture-sync" : "workers-disabled-sync",
                                            e->frame, r, host_now_ns() - build_started, 0, -1);
        }
        pso_sync_end(warming);
        HK_ADD(pipes_sync, 1);
        uint64_t ended = host_now_ns(), elapsed = ended - (V.has_cache_control ? c0 : t0);
        g_win_sync++; g_win_sync_ns += elapsed;
        vk_overlay_note_compile(ended, elapsed);
    }
    uint64_t dt = host_now_ns() - t0;
    g_win_pipes++; g_win_pipe_ns += dt; g_cum_pipe_ns += dt; if (dt > g_win_pipe_max_ns) g_win_pipe_max_ns = dt;
    if (r == VK_PIPELINE_COMPILE_REQUIRED) {
        HK_ADD(pipe_skips, 1);
        if (V.has_pipeline_library) atomic_fetch_add(&g_library_stats.library_skips, 1);
        return VK_NULL_HANDLE;
    }
    pipe_publish(e, r, p, dt);
    if (r && V.has_pipeline_library) atomic_fetch_add(&g_library_stats.library_skips, 1);
    return r ? PIPE_FAILED : p;
}
#ifdef TOTK_VK_TESTS
int vk_draw_test_pipeline_bench(vk_shader *vs, vk_shader *fs) {
    enum { NDRAWS = 200000 };
    const int prim[3] = {0, 1, 4};
    VkPipelineLayout layout = vk_layout_of(vs, fs); VkPipeline expected[3];
    int ok = 1;
    for (int i = 0; i < 3; i++) { expected[i] = pipe_get(vs, fs, prim[i], layout); ok &= expected[i] && expected[i] != PIPE_FAILED; }
    for (int churn = 0; churn < 2; churn++) {
        uint64_t start = host_now_ns();
        for (int draw = 0; draw < NDRAWS; draw++) {
            int i = churn ? draw % 3 : 2;
            ok &= pipe_get(vs, fs, prim[i], layout) == expected[i];
        }
        printf("pipeline-resolve-bench %s: %.2f ns/draw\n", churn ? "alternating" : "stable",
            (double)(host_now_ns() - start) / NDRAWS);
    }
    printf("%s: pipeline lookup preserves exact ready handles for all topology classes\n", ok ? "ok" : "FAIL");
    return !ok;
}
#endif

/* ---------- descriptor sets ---------- */
extern int vk_nvn2;   /* vk_shader.c */
static void driver_bank_fill(uint8_t *d, int bank, int stage, const uint32_t grid[3]) {   /* nvn_mtl.m driver_bank_fill */
    memset(d, 0, 0xA00);
    if (bank == 0) {
        /* stages 3 / 4 hold NVN2's task / mesh bindings (hle_nvn.c st_host): their storage buffer tables are at 0x670 / 0x770
           (nxnative dmsh_abi.h DMSH_C0_SSBO_TS / DMSH_C0_SSBO_MS), the only driver words the 520 kernels read (oot/drv_reads.py) */
        int base = stage == 0 ? 0x110 : stage == 5 ? 0x310 : stage == 1 ? 0x510 : stage == 3 ? 0x670 : stage == 4 ? 0x770 : -1;
        if (base >= 0) for (int r = 0; r < 16; r++) { uint64_t a = S.ssbo[stage][r].addr; uint32_t n = (uint32_t)S.ssbo[stage][r].size; memcpy(d + base + 16 * r, &a, 8); memcpy(d + base + 16 * r + 8, &n, 4); }
        if (stage == 5) {
            if (grid) memcpy(d + 0x200, grid, 12);
            /* NVN2 compute keeps the dispatch's workgroup counts at c0 + 4 (x, y, z), below its per-shader layout at 0x10
               (nxnative dmsh_abi.h DMSH_C0_DISPATCH_NVN2, shaders.md "NVN2 compute reads its workgroup counts at c0 + 4").
               Left 0 here, Ocarina of Time's SH producer 21d9f83e took its sample count (32 x 32 x counts) as 0: weight
               1 / 0 = +inf, direction z = -inf, and its four coefficients came out +inf, NaN, -inf, NaN in the lighting
               buffer every material reads: the black 3D world (oot-a47 .. a61). 18 of its compute shaders read them
               (oot/drv_reads.py). */
            if (grid && vk_nvn2) memcpy(d + 4, grid, 12);
            for (int r = 0; r < 16; r++) { uint64_t a = S.ubo[5][r].addr; uint32_t n = (uint32_t)S.ubo[5][r].size; memcpy(d + 0x210 + 16 * r, &a, 8); memcpy(d + 0x210 + 16 * r + 8, &n, 4); }
        } else if (stage == 0) { const float ps_max = 2047.9375f; memcpy(d + 0x610, &ps_max, sizeof ps_max); }
    } else {
        for (int r = 0; r < 128; r++) { uint32_t th = (uint32_t)S.tex[stage][r], sh_ = (uint32_t)S.samp[stage][r]; memcpy(d + 0x168 + 8 * r, &th, 4); memcpy(d + 0x568 + 8 * r, &sh_, 4); }
        for (int r = 0; r < 32; r++) { uint32_t th = (uint32_t)S.tex[stage][r]; memcpy(d + 0x20 + 8 * r, &th, 4); }
    }
}
static void support_fill(uint8_t *d, uint32_t w, uint32_t h) {   /* Ryujinx SupportBuffer: render scale 1, no BGRA swap */
    memset(d, 0, 1392);
    float vi[4] = {2.0f / (w ? w : 1), 2.0f / (h ? h : 1), 1, 1}, vs[4] = {(float)w, (float)h, 1, 1};
    memcpy(d + 144, vi, 16); memcpy(d + 160, vs, 16);
    for (int i = 0; i < 73; i++) { float one = 1; memcpy(d + 192 + 16 * i, &one, 4); }
}
typedef struct { VkDescriptorBufferInfo b; VkDescriptorImageInfo i; VkBufferView t; } dinfo;
#ifdef TOTK_OHOS
/* TOTK_VK_SB_ALIGN=<n>: the seed's storage accesses are relative to (base & -n) (set by the frontend from the seed's
   sb-align.txt). Only used when n is the device's own alignment; anything else keeps the reference's copy. */
static int vk_sb_align_down(VkDeviceSize device_align) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("TOTK_VK_SB_ALIGN"); long n = e && *e ? strtol(e, NULL, 10) : 0;
        v = n > 0 && (VkDeviceSize)n == device_align;
        LOGV("storage buffers: %s (TOTK_VK_SB_ALIGN=%s, device alignment %llu)", v ? "bound at the aligned address below, the seed's shaders add the rest" :
             "unaligned ones copied (GPU writes to them are lost)", e ? e : "unset", (unsigned long long)device_align);
    }
    return v;
}
#endif
static int gw_drop(uint64_t a, uint64_t n);   /* TOTK_OHOS_GW_DROP, at copy_t2b below */
#ifdef TOTK_OHOS
/* oot-a122 TOTK_OHOS_PROTECT (nvn_vk.c "two frames in flight: bounded protection"): a storage binding's frozen (read) or
   landed (write) copy, bound the way ubo_desc binds the pool's bytes: from the aligned address below with the seed's
   TOTK_VK_SB_ALIGN. A binding of unknown extent (the shader may reach the end of the pool) or an unaligned one is left to
   memory (vk_pt_unbound: a write goes there after the GPU bytes of the copies it may reach; a read over one counts). */
static int pt_storage_desc(uint64_t addr, uint64_t size, uint64_t avail, uint64_t extent, int rw, VkDescriptorBufferInfo *out) {
    VkDeviceSize align = V.props.limits.minStorageBufferOffsetAlignment; if (!align) align = 16;
    uint64_t n = MINV(size ? size : avail, avail);
    if (extent) n = MINV(n, extent);
    else if (!size) { vk_pt_unbound(addr, avail, rw); return 0; }
    n = MINV(n, V.props.limits.maxStorageBufferRange);
    uint64_t mis = addr % align;
    if (mis && !vk_sb_align_down(align)) { vk_pt_unbound(addr, n, rw); return 0; }   /* ubo_desc copies an unaligned binding at record time */
    VkDescriptorBufferInfo c;
    if (!(rw ? vk_pt_write(addr - mis, n + mis, 1, align, &c) : vk_pt_read(addr - mis, n + mis, align, VK_LAG_STORAGE, &c))) return 0;
    *out = c; return 1;
}
#endif
static int ubo_desc(uint64_t addr, uint64_t size, VkDescriptorBufferInfo *out, int storage, int rw, uint64_t extent, vk_record_descriptor *record) {
    VkBuffer b; uint64_t off, avail; uint32_t reason = 0;
    if (!addr || !vk_resolve_for(addr, 1, &b, &off, &avail, VK_LOOKUP_UBO)) return 0;
    if (vk_fg_extract_active && FGX.record && !rw) {
        uint64_t limit=storage ? V.props.limits.maxStorageBufferRange : V.props.limits.maxUniformBufferRange;
        uint64_t n=MINV(size ? size : limit,MINV(avail,limit));
        if (extent) n=MINV(n,extent);
        VkDeviceSize align=storage ? V.props.limits.minStorageBufferOffsetAlignment :
            V.props.limits.minUniformBufferOffsetAlignment;
        if (vk_ub_replay_read(addr,n,align,out)) return 1;
        fg_extract_reject(VK_FG_STRUCTURE_CHANGED,"UI/transfer readonly buffer cannot be frozen before a pending GPU writer");
    }
    if (storage && vk_lag_capture_on()) {
        if (extent) size = size ? MINV(size, extent) : extent;
        else if (!size) size = vk_lag_bound(addr, avail);
    }
    if (!storage) {
        uint64_t n = MINV(size ? size : 0x10000, MINV(avail, V.props.limits.maxUniformBufferRange));
        if (vk_lag_uniform_read(addr, n, V.props.limits.minUniformBufferOffsetAlignment, out)) return 1;
        if (vk_ub_rename(addr, n, out) || (record ?
            vk_lag_read_record(addr, n, V.props.limits.minUniformBufferOffsetAlignment, out, &reason, VK_LAG_UNIFORM) :
            vk_lag_read(addr, n, V.props.limits.minUniformBufferOffsetAlignment, out, VK_LAG_UNIFORM))) return 1;
    }
    else {   /* a storage binding sees the logged updates in memory; draw_touch wrote them before the pass opened */
        uint64_t n = MINV(size ? size : avail, avail);
        if (!rw && (record ? vk_lag_read_record(addr, MINV(n, V.props.limits.maxStorageBufferRange),
            V.props.limits.minStorageBufferOffsetAlignment, out, &reason, VK_LAG_STORAGE) :
            vk_lag_read(addr, MINV(n, V.props.limits.maxStorageBufferRange), V.props.limits.minStorageBufferOffsetAlignment, out, VK_LAG_STORAGE))) return 1;
        if (rw && __builtin_expect(gw_drop(addr, MINV(n, 65536)), 0)) {   /* TOTK_OHOS_GW_DROP (measurement): the shader writes a scratch copy, the game's bytes stay */
            uint64_t m = MINV(n, 65536); VkBuffer tb; VkDeviceSize to;
            uint8_t *p = vk_transient((size_t)m, (size_t)V.props.limits.minStorageBufferOffsetAlignment, &tb, &to);
            if (p) { memcpy(p, (const void *)(uintptr_t)addr, (size_t)m); *out = (VkDescriptorBufferInfo){tb, to, m}; return 1; }
        }
        if (rw) {
            if (__builtin_expect(g_ic_k, 0) && (!size || size >= avail)) {   /* in-flight check: no size from the game: the record runs to the end of the pool; before the PLAIN note below, which keeps it */
                vk_inflight_gpu_wrote(addr, n, VK_INFLIGHT_GW_UNBOUNDED);
                if (extent && extent < n) vk_inflight_gpu_wrote(addr, extent, VK_INFLIGHT_GW_PLAIN);   /* the shader's reflected extent bounds it, as ic_note_new records it in sampled frames */
            }
            if (__builtin_expect(g_pt_on, 0) && extent) vk_pt_gpu_clamp(addr, extent);   /* oot-a122: the shader writes its extent, not the pool */
            if (record) { vk_gpu_write_record(addr, n); reason = NVN_DIRTY_LAG_GPU | NVN_DIRTY_LAG_WRITABLE; }
            else vk_gpu_write(addr, n);
        }
        if (!R.open) VK_REC_DO(VK_REC_S, vk_ub_touch(addr, n));
        else if (VK_REC_VALUE(VK_REC_S, vk_ub_pending(addr, n))) RED(4, "storage buffer %#llx overlaps a uniform update logged inside the pass: it reads the bytes from before", (unsigned long long)addr);
    }
#ifdef TOTK_OHOS
    if (storage && __builtin_expect(g_pt_on, 0) && pt_storage_desc(addr, size, avail, extent, rw, out)) {   /* oot-a122 TOTK_OHOS_PROTECT */
        if (record && reason) { record->lag_range = *out; record->lag_reason = reason; }
        return 1;
    }
#endif
    VkDeviceSize align = storage ? V.props.limits.minStorageBufferOffsetAlignment : V.props.limits.minUniformBufferOffsetAlignment;
    uint64_t lim = storage ? V.props.limits.maxStorageBufferRange : V.props.limits.maxUniformBufferRange;
    if (!size || size > lim) size = lim;
#ifdef TOTK_OHOS
    /* A seed translated with TOTK_XLATE_SB_ALIGN=<device alignment> (native-harmonyos/xlate) addresses every storage access
       relative to (guest base & -alignment): the buffer is bound at that aligned address, the range grown by the
       difference. The shader writes then land in the pool (the copy below loses them) and no CPU copy is made. */
    if (storage && off % align && vk_sb_align_down(align)) {
        uint64_t mis = off % align;
        VK_INFLIGHT((R.open ? VK_INFLIGHT_STORAGE : VK_INFLIGHT_STORAGE_CS) | (rw ? VK_INFLIGHT_GPUW : 0) | (!extent && size == lim ? VK_INFLIGHT_UNBOUNDED : 0), addr,
                    extent && extent < MINV(size, avail) ? extent : MINV(size, avail), b);
        *out = (VkDescriptorBufferInfo){b, off - mis, MINV(MINV(size, avail) + mis, lim)};
        if (record && reason) { record->lag_range = *out; record->lag_reason = reason; }
        return 1;
    }
#endif
    if (off % align) {   /* not aligned for the device: a copy in this submission's memory (uniforms only change at submission boundaries) */
        uint64_t n = MINV(size, avail);
        VkBuffer tb; VkDeviceSize to; uint8_t *p = vk_transient(n, align, &tb, &to);
        memcpy(p, (const void *)(uintptr_t)addr, n);
        if (storage) RED(4, "storage buffer at %#llx is not %llu-aligned: bound as a copy (GPU writes to it are lost)", (unsigned long long)addr, (unsigned long long)align);
#ifdef TOTK_OHOS
        {   /* How often (Maleoon wants 256-byte offsets): read-only / written storage, uniforms, in a pass or not. */
            static _Atomic uint64_t n_ro, n_rw, n_u, n_pass, bytes, last;
            atomic_fetch_add(storage ? (rw ? &n_rw : &n_ro) : &n_u, 1); if (R.open) atomic_fetch_add(&n_pass, 1); atomic_fetch_add(&bytes, n);
            uint64_t now = host_now_ns(), l = atomic_load(&last);
            if (now - l > 5000000000ull && atomic_compare_exchange_strong(&last, &l, now))
                LOGV("[unaligned] last 5 s: storage read-only %llu, storage written %llu, uniform %llu (%llu inside a pass), %.1f MiB copied by the CPU at record time",
                     (unsigned long long)atomic_exchange(&n_ro, 0), (unsigned long long)atomic_exchange(&n_rw, 0), (unsigned long long)atomic_exchange(&n_u, 0),
                     (unsigned long long)atomic_exchange(&n_pass, 0), atomic_exchange(&bytes, 0) / 1048576.0);
        }
#endif
        *out = (VkDescriptorBufferInfo){tb, to, n}; return 1;
    }
    VK_INFLIGHT((storage ? R.open ? VK_INFLIGHT_STORAGE : VK_INFLIGHT_STORAGE_CS : VK_INFLIGHT_UNIFORM) | (storage && rw ? VK_INFLIGHT_GPUW : 0) |
                (storage && !extent && size == lim ? VK_INFLIGHT_UNBOUNDED : 0), addr,
                storage && extent && extent < MINV(size, avail) ? extent : MINV(size, avail), b);   /* the pool's own bytes, read when the GPU gets there */
    *out = (VkDescriptorBufferInfo){b, off, MINV(size, avail)};   /* clamped to the pool's buffer */
    if (record && reason) { record->lag_range = *out; record->lag_reason = reason; }
    return 1;
}
static VkBuffer g_zero_ubo; static VkDeviceSize g_zero_ubo_off;
static void zero_buffer(VkDescriptorBufferInfo *out, size_t size) {
    VkBuffer b; VkDeviceSize o; void *p = vk_transient(size, V.props.limits.minUniformBufferOffsetAlignment, &b, &o); memset(p, 0, size);
    *out = (VkDescriptorBufferInfo){b, o, size};
}
/* Small uniform blocks the backend fills per draw (driver banks, support buffer): a block with the same bytes as the
   last one of its kind in this submission reuses that copy, so the descriptor sets of consecutive draws can match.
   The transient bytes are immutable until slot retirement; the seal serial expires this pointer before slot reuse. */
static void block_desc(int key, const void *data, size_t n, VkDescriptorBufferInfo *out) {
    static struct { uint64_t sub; size_t n; uint8_t *bytes; VkDescriptorBufferInfo d; } last[32];
    uint64_t sub = vk_submit_serial();
    static int off = -1; if (off < 0) off = vk_off("block");
    if (!off && key >= 0 && key < 32 && last[key].sub == sub && last[key].n == n && !memcmp(last[key].bytes, data, n)) { *out = last[key].d; return; }
    VkBuffer tb; VkDeviceSize to; uint8_t *p = vk_transient(n, V.props.limits.minUniformBufferOffsetAlignment, &tb, &to); memcpy(p, data, n);
    *out = (VkDescriptorBufferInfo){tb, to, n};
    if (key >= 0 && key < 32) { last[key].bytes = p; last[key].n = n; last[key].sub = sub; last[key].d = *out; }
}
/* Most draws use the same generated blocks. Check their semantic inputs before filling / comparing 1.4--2.5 KiB:
   bind hooks cover every bank input, while support dimensions and compute grid are checked here. On a changed input
   block_desc still compares the bytes, preserving reuse when several state changes cancel before the next draw. */
static int g_cs_indirect; static VkDescriptorBufferInfo g_cs_drv0;   /* an indirect dispatch's own c0 block (below) */
static void generated_desc(int kind, int stage, const uint32_t grid[3], VkDescriptorBufferInfo *out) {
    static struct { uint64_t sub, revision; uint32_t w, h, grid[3]; VkDescriptorBufferInfo d; } last[13];
    int support = kind == RB_SUPPORT, bank = kind == RB_DRV2, key = support ? 0 : 1 + bank * 6 + stage;
    uint64_t sub = vk_submit_serial(), revision = support ? 0 : g_driver_revision[bank][stage];
    int compute = !support && !bank && stage == 5;
    uint32_t g[3] = {0}; if (compute && grid) memcpy(g, grid, sizeof g);
    if (compute && g_cs_indirect) {
        /* DispatchComputeIndirect: the workgroup counts are on the GPU. The block is this dispatch's own (no earlier or later
           dispatch shares it), and the dispatch copies the counts into it (nvn_mtl.m: "an indirect dispatch copies its
           GPU-written counts there too") */
        uint8_t blk[0xA00]; driver_bank_fill(blk, 0, 5, grid);
        VkBuffer tb; VkDeviceSize to; uint8_t *p = vk_transient(sizeof blk, V.props.limits.minUniformBufferOffsetAlignment, &tb, &to); memcpy(p, blk, sizeof blk);
        *out = g_cs_drv0 = (VkDescriptorBufferInfo){tb, to, sizeof blk};
        return;
    }
    static int off = -1; if (off < 0) off = vk_off("block");
    if (!off && last[key].sub == sub && last[key].revision == revision &&
        (!support || (last[key].w == R.w && last[key].h == R.h)) && (!compute || !memcmp(last[key].grid, g, sizeof g))) { *out = last[key].d; return; }
    uint8_t blk[0xA00]; size_t n = support ? 1392 : sizeof blk;
    if (support) support_fill(blk, R.w, R.h); else driver_bank_fill(blk, bank ? 2 : 0, stage, grid);
    block_desc(key, blk, n, out);
    last[key].sub = sub; last[key].revision = revision; last[key].w = R.w; last[key].h = R.h; memcpy(last[key].grid, g, sizeof g); last[key].d = *out;
}
/* References are immutable in the submission's arena. Reuse up to 64 distinct
 * reference words, including non-consecutive draws and hash collisions. */
static void alpha_desc(VkDescriptorBufferInfo *out) {
    static struct { uint64_t sub; uint32_t bits; VkDescriptorBufferInfo d; } memo[64];
    uint32_t bits; memcpy(&bits,&S.alpha_ref,4);
    uint64_t sub=vk_submit_serial();
    unsigned index=(bits*2654435761u)>>26;
    for (unsigned probe=0;probe<63 && memo[index].sub==sub && memo[index].bits!=bits;probe++)
        index=(index+1)&63u;
    if (memo[index].sub!=sub || memo[index].bits!=bits) {
        VkBuffer buffer; VkDeviceSize offset;
        float block[4]={S.alpha_ref,0,0,0};
        void *bytes=vk_transient(sizeof block,V.props.limits.minUniformBufferOffsetAlignment,&buffer,&offset);
        memcpy(bytes,block,sizeof block);
        memo[index].sub=sub; memo[index].bits=bits;
        memo[index].d=(VkDescriptorBufferInfo){buffer,offset,sizeof block};
    }
    *out=memo[index].d;
}
/* NVN2 task / mesh draws (mesh_draw below): the buffers of the draw being replayed, for the bindings the wrapped kernels and
   the draw's vertex shader add (tools/mesh_wrap_in_container.py; vk_int.h RB_MESH_IN .. RB_MESH_PAR). The control block's
   words are the wrapper's C_*. */
enum { MESH_C_FIRST = 0, MESH_C_CAP = 1, MESH_C_TCAP = 2, MESH_C_TDISP = 4, MESH_C_DISP = 8, MESH_C_DRAW = 12, MESH_C_WORDS = 20 };
static struct { int on; vk_shader *vs; VkDescriptorBufferInfo buf[4]; } g_mesh;   /* buf: payload table, vertices, indices, control block */
static void mesh_desc(int kind, VkDescriptorBufferInfo *out) {
    *out = g_mesh.buf[kind - RB_MESH_IN];
    if (!out->buffer) zero_buffer(out, 256);   /* a kernel bound outside a mesh draw: nothing to read or write */
}
/* Descriptor sets already written in this submission, by their contents. Two independent 64-bit hashes, layout and
   count identify a set; no retained info arrays to compare, copy or reallocate. Pass dumps read the live infos below. */
enum { DS_CACHE = 2048 };
static struct { uint64_t sub, hash[2]; VkDescriptorSetLayout dsl; size_t n; VkDescriptorSet set; } g_dsc[DS_CACHE];
static struct { uint64_t sub; VkPipelineLayout layout; VkDescriptorSet set; } g_bound_set[2][6];   /* [graphics, compute][set index] */
static void descriptor_unbind(void) { memset(g_bound_set, 0, sizeof g_bound_set); }
static uint64_t g_stat_sets_new, g_stat_sets_reused;
/* Hash both identities in one word-wise pass over the zero-filled infos. The second uses add/multiply/rotate rather
   than the first's xor/multiply/shift, not just another seed of the same hash. */
static void hash_words(const void *p, size_t n, uint64_t h[2]) {
    const uint8_t *b = p; size_t i = 0; uint64_t a = h[0], c = h[1];
    for (; i + 8 <= n; i += 8) {
        uint64_t w; memcpy(&w, b + i, 8);
        a = (a ^ w) * 0x9e3779b97f4a7c15ull; a ^= a >> 32;
        c = (c + w) * 0xc2b2ae3d27d4eb4full; c = c << 31 | c >> 33;
    }
    for (; i < n; i++) { a = (a ^ b[i]) * 0x100000001b3ull; c = (c + b[i]) * 0xc2b2ae3d27d4eb4full; c = c << 31 | c >> 33; }
    h[0] = a; h[1] = c;
}
/* Exact shader-used binding values. Every kind uses at most two words; write
   directly to the destination instead of returning/copying a larger zero-padded key. */
typedef struct { uint64_t value[2]; } record_binding;
static void record_binding_of(const vk_bind *b, int stage, record_binding *r) {
    *r = (record_binding){{0}};
    if (b->kind == RB_SUPPORT || b->kind == RB_C1) return;
    if (b->kind == RB_ALPHA) { uint32_t bits; memcpy(&bits,&S.alpha_ref,4); r->value[0]=bits; return; }
    if (b->kind == RB_PUSH) { r->value[0] = g_push_rev[stage == 5]; return; }
    if (b->kind == RB_GMEM || b->kind == RB_GMEML) { r->value[0] = vk_gmem_generation(); r->value[1] = vk_submit_serial(); return; }
    if (b->dtype == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || b->dtype == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) {
        if (b->kind == RB_DRV0 || b->kind == RB_DRV2) r->value[0] = g_driver_revision[b->kind == RB_DRV2][stage];
        else if (b->reg >= 0 && b->reg < 16) {
            const p_bind_buf *u = b->kind == RB_SBUF ? &S.ssbo[stage][b->reg] : &S.ubo[stage][b->reg];
            r->value[0] = u->addr; r->value[1] = u->size;
        }
    } else {
        uint64_t texture = b->reg >= 0 ? (b->kind == RB_IMG ? (b->reg < 32 ? S.img[stage][b->reg] : 0) :
            (b->reg < 128 ? S.tex[stage][b->reg] : 0)) : 0;
        r->value[0] = texture;
        if (b->dtype == VK_DESCRIPTOR_TYPE_SAMPLER) r->value[0] = b->reg >= 0 && b->reg < 128 ?
            (b->sreg == -2 ? S.tex[stage][b->reg] : S.samp[stage][b->reg]) >> 20 : 0;
        else if (b->dtype == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER)
            r->value[1] = b->sreg >= 0 && b->sreg < 128 ? S.samp[stage][b->sreg] >> 20 : texture >> 20;
    }
}
#ifdef TOTK_VK_TESTS
int vk_draw_test_binding_bench(void) {
    enum { NB = 32, NDRAWS = 200000 };
    vk_bind bindings[NB] = {{0}}; record_binding saved[NB];
    g_driver_revision[0][1] = 17;
    int ok = 1;
    for (int i = 0; i < NB; i++) {
        S.ubo[1][i & 15] = (p_bind_buf){1, i & 15, 0x10000u + (unsigned)(i & 15) * 256u, 256};
        S.ssbo[1][i & 15] = (p_bind_buf){1, i & 15, 0x20000u + (unsigned)(i & 15) * 512u, 512};
        S.tex[1][i] = (uint64_t)(i + 1) | (uint64_t)(i + 3) << 20;
        S.samp[1][i] = (uint64_t)(i + 5) << 20;
        S.img[1][i] = (uint64_t)(i + 7);
    }
    for (int i = 0; i < NB; i++) {
        vk_bind *b = &bindings[i]; b->reg = (int16_t)i; b->count = 1;
        uint64_t first = 0, second = 0;
        switch (i & 7) {
            case 0: b->kind = RB_SUPPORT; b->dtype = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; break;
            case 1: b->kind = RB_C1; b->dtype = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; break;
            case 2: b->kind = RB_DRV0; b->dtype = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; first = 17; break;
            case 3: b->kind = RB_CBUF; b->dtype = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; b->reg &= 15; first = 0x10000u + b->reg * 256u; second = 256; break;
            case 4: b->kind = RB_SBUF; b->dtype = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; b->reg &= 15; first = 0x20000u + b->reg * 512u; second = 512; break;
            case 5: b->kind = RB_SAMP; b->dtype = VK_DESCRIPTOR_TYPE_SAMPLER; first = i + 5; break;
            case 6: b->kind = RB_TEX; b->dtype = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; b->sreg = (int16_t)i; first = S.tex[1][i]; second = i + 5; break;
            case 7: b->kind = RB_IMG; b->dtype = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; first = i + 7; break;
        }
        record_binding_of(b, 1, &saved[i]);
        ok &= saved[i].value[0] == first && saved[i].value[1] == second;
    }
    uint64_t same = 0, start = host_now_ns();
    for (int draw = 0; draw < NDRAWS; draw++) {
        /* NVN state may change between draws; prevent the fixture from hoisting immutable reads. */
        __asm__ volatile("" ::: "memory");
        for (int i = 0; i < NB; i++) {
            record_binding value; record_binding_of(&bindings[i], 1, &value);
            same += !memcmp(&saved[i], &value, sizeof value);
        }
    }
    printf("record-binding-bench: %.2f ns/draw (%d bindings), key_bytes=%zu, matches=%llu\n",
        (double)(host_now_ns() - start) / NDRAWS, NB, sizeof(record_binding), (unsigned long long)same);
    ok &= same == (uint64_t)NDRAWS * NB;
    printf("%s: exact generated/uniform/storage/texture/sampler/image binding values\n", ok ? "ok" : "FAIL");
    return !ok;
}
#endif
static int record_uniforms_unchanged(vk_shader *sh, int stage, uint64_t revision) {
    for (int i = 0; i < sh->nb; i++) {
        const vk_bind *b = &sh->b[i];
        /* Read-only storage can acquire additional frozen-content dependencies (frame-lag integration). */
        if (b->dtype == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER || b->dtype == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER ||
            b->dtype == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER) return 0;
        if (b->dtype != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || b->reg < 0 || b->reg >= 16 ||
            (b->kind != RB_CBUF && b->kind != RB_UBO_PTR)) continue;
        const p_bind_buf *u = &S.ubo[stage][b->reg];
        uint64_t range = MINV(u->size ? u->size : 0x10000, V.props.limits.maxUniformBufferRange);
        if (u->addr && !vk_record_uniform_unchanged(revision, u->addr, range)) return 0;
    }
    return 1;
}
/* TOTK_VK_FEEDBACK_CUT=1: a pass that samples the image it renders to (a mip chain generated level by level) gets a
   sampled view without the attached level. Feedback through a view that includes the attachment's level is undefined
   in Vulkan even if the shader only reads other levels; Adreno shows it as NaN blotches. Only views whose base level
   is below the attached level are cut, to [base, attached), keeping LOD numbering; a view based at the attached level
   is left alone (counted). Sampled views only; storage images are the game's own explicit read/write. */
static VkImageView feedback_cut(vk_tex *x, VkImageView view) {
    VkImage image = vk_tex_image(x); int attached = 0;
    for (unsigned a = 0; a < 9 && !attached; a++) {
        vk_tex *t = a < 8 ? (a < R.ncolor ? R.ctex[a] : NULL) : R.dtex;
        attached = t && vk_tex_image(t) == image;
    }
    if (!attached) return view;
    vk_canvas_image identity; VkImageSubresourceRange range;
    if (!vk_tex_view_identity(x, view, &identity, &range)) return view;
    uint32_t keep = range.levelCount; int is3d = vk_tex_is3d(x);
    for (unsigned a = 0; a < 9; a++) {
        vk_tex *t = a < 8 ? (a < R.ncolor ? R.ctex[a] : NULL) : R.dtex;
        if (!t || vk_tex_image(t) != image) continue;
        uint32_t level = a < 8 ? R.clevel[a] : R.dlevel, layer = a < 8 ? R.clayer[a] : R.dlayer;
        if (level < range.baseMipLevel || level >= range.baseMipLevel + range.levelCount) continue;
        if (!is3d && (layer < range.baseArrayLayer || layer >= range.baseArrayLayer + range.layerCount)) continue;
        if (level - range.baseMipLevel < keep) keep = level - range.baseMipLevel;
    }
    if (keep == range.levelCount) return view;
    if (!keep) { g_feedback_uncut++; return view; }
    VkImageView cut = vk_tex_view_levels(x, view, keep);
    if (cut != view) g_feedback_cuts++; else g_feedback_uncut++;
    return cut;
}
/* Freeze only stream-position decisions. Descriptor construction, range resolution and zero bindings are worker work. */
static VkImageView native_texture_by_id(int tid, int *want, vk_tex **object) {
    VkImageView view = vk_tex_by_id(tid, want, object);
    if (g_native_sample && object && *object == g_native_sample && *want != -2) {
        vk_treg reg;
        if (vk_treg_get(tid, &reg)) {
            VkImageView native = vk_tex_native_view(*object, reg.has_v ? &reg.v : NULL, 0);
            if (native) return native;
        }
    }
    if (__builtin_expect(atomic_load_explicit(&g_feedback_cut, memory_order_relaxed), 0) && R.open && view && object && *object && *want != -2)
        view = feedback_cut(*object, view);
    return view;
}
/* Enumerate descriptor MAY-reads before freeze-memo reuse. The actual view cache supplies exact ranges,
   so a changed registration/serial cannot be mistaken for a new lifetime or the wrong mip/layer. */
static void pso_life_reads(const vk_shader *sh, unsigned stage) {
    if (!pso_life_active()) return;
    uint64_t sub = vk_game_submit_serial();
    if (!R.open || PL.memo_sub != sub || PL.memo_pass != g_stat_passes) {
        PL.nmemo = 0; PL.memo_sub = sub; PL.memo_pass = g_stat_passes;
    }
    for (int i = 0; i < sh->nb; i++) {
        const vk_bind *b = &sh->b[i];
        if (b->dtype != VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE && b->dtype != VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER &&
            b->dtype != VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) continue;
        uint64_t handle = b->reg >= 0 ? (b->kind == RB_IMG ? (b->reg < 32 ? S.img[stage][b->reg] : 0) :
            (b->reg < 128 ? S.tex[stage][b->reg] : 0)) : 0;
        if (!handle) continue;
        int want = b->dtype == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE ? -2 : b->target;
        vk_tex *object = NULL;
        VkImageView view = native_texture_by_id((int)(handle & 0xfffff), &want, &object);
        if (!object) continue;
        vk_canvas_image image; VkImageSubresourceRange range;
        if (!view || !vk_tex_view_identity(object, view, &image, &range)) {
            vk_canvas_image base = vk_tex_canvas_identity(object, 0, 0);
            for (unsigned mip = 0; mip < vk_tex_levels(object) && mip < 32; mip++)
                if (pso_life_match(&base, mip)) { pso_life_unknown("sample-view-identity"); break; }
            continue;
        }
        if (range.levelCount > 32 || range.layerCount > 256 ||
            range.baseMipLevel >= 32 || range.levelCount > 32 - range.baseMipLevel) {
            pso_life_unknown("sample-view-range"); continue;
        }
        const char *access = b->dtype != VK_DESCRIPTOR_TYPE_STORAGE_IMAGE ? "sampled" : b->rw ? "storage-write" : "storage-read";
        for (unsigned mip = range.baseMipLevel; mip < range.baseMipLevel + range.levelCount; mip++) {
            if (!pso_life_match(&image, mip)) continue;
            if (vk_tex_is3d(object)) { pso_life_unknown("3d-sampled-range"); continue; }
            for (unsigned layer = range.baseArrayLayer; layer < range.baseArrayLayer + range.layerCount; layer++)
                for (unsigned aspect = 1; aspect <= VK_IMAGE_ASPECT_STENCIL_BIT; aspect <<= 1) if (range.aspectMask & aspect) {
                    pso_life_read_key key = {image.image, image.incarnation, mip, layer, aspect, stage, b->binding};
                    int duplicate = 0;
                    if (R.open && b->dtype != VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) {
                        for (size_t j = 0; j < PL.nmemo; j++) {
                            const pso_life_read_key *old = &PL.memo[j];
                            if (old->image == key.image && old->incarnation == key.incarnation && old->mip == key.mip &&
                                old->layer == key.layer && old->aspect == key.aspect && old->stage == key.stage &&
                                old->binding == key.binding) { duplicate = 1; break; }
                        }
                        if (!duplicate && PL.nmemo < 2048) PL.memo[PL.nmemo++] = key;
                        else if (!duplicate) pso_life_unknown("read-memo-capacity");
                    }
                    if (duplicate) continue;
                    PL.reads++;
                    LOGV("[pso-life] kind=read frame=%llu sub=%llu pass=%llu image=%#llx incarnation=%llu mip=%u layer=%u aspect=%u "
                         "w=%u h=%u format=%u serial=%llu storage=%#llx stage=%u binding=%u access=%s shader=%s consumer_w=%u consumer_h=%u",
                         (unsigned long long)PL.frame, (unsigned long long)sub, (unsigned long long)g_stat_passes,
                         (unsigned long long)image.image, (unsigned long long)image.incarnation, mip, layer, aspect,
                         PL.w, PL.h, image.format, (unsigned long long)image.serial, (unsigned long long)image.storage,
                         stage, b->binding, access, sh->key, R.open ? R.w : 0, R.open ? R.h : 0);
                }
        }
    }
}
#if defined(TOTK_OHOS) && defined(TOTK_GAME_OOT)
/* ---------- oot-a121: push descriptors (TOTK_OHOS_PUSHDESC; vk_int.h, nvn_vk.c) ----------
   The update template that pushes set `set` of a graphics pipeline layout whose set layout there is sh->dsl_push. Its
   entries walk sh's bindings in the order a stage's descriptors are laid out, binding by binding and element by element,
   one record each: kind 0 vk_record_descriptor (record_stage_impl; the recorder's workers push it, vk_record.c push_set),
   kind 1 dinfo (bind_stage_body; push_stage pushes it on the primary command buffer). A push template belongs to a
   pipeline layout (vkCmdPushDescriptorSetWithTemplateKHR wants a compatible one); layouts live as long as the process,
   so each template is made once. Walking owner only. */
static VkDescriptorUpdateTemplate push_template(VkPipelineLayout layout, uint32_t set, const vk_shader *sh, int kind) {
    static struct { VkPipelineLayout layout; VkDescriptorUpdateTemplate t; } last[2][2];
    if (last[kind][set & 1].layout == layout) return last[kind][set & 1].t;
    static nvn_hmap templates = {.klen = 16};
    uint64_t key[2] = {(uint64_t)(uintptr_t)layout, (uint64_t)set << 1 | (uint64_t)kind};
    VkDescriptorUpdateTemplate *found = nvn_hm_get(&templates, key);
    if (!found) {
        size_t stride = kind ? sizeof(dinfo) : sizeof(vk_record_descriptor);
        VkDescriptorUpdateTemplateEntry *e = calloc((size_t)sh->nb, sizeof *e);
        found = malloc(sizeof *found);
        if (!e || !found) DIE("descriptor push (oot-a121): out of memory");
        for (int i = 0, k = 0; i < sh->nb; k += sh->b[i].count, i++) {
            VkDescriptorType dt = (VkDescriptorType)sh->b[i].dtype;
            int buf = dt == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || dt == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            int tbuf = dt == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER || dt == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;
            size_t off = kind ? (buf ? offsetof(dinfo, b) : tbuf ? offsetof(dinfo, t) : offsetof(dinfo, i))
                              : (buf ? offsetof(vk_record_descriptor, b) : tbuf ? offsetof(vk_record_descriptor, t) : offsetof(vk_record_descriptor, i));
            e[i] = (VkDescriptorUpdateTemplateEntry){.dstBinding = sh->b[i].binding, .dstArrayElement = 0, .descriptorCount = sh->b[i].count,
                .descriptorType = dt, .offset = (size_t)k * stride + off, .stride = stride};
        }
        VkDescriptorUpdateTemplateCreateInfo ci = {VK_STRUCTURE_TYPE_DESCRIPTOR_UPDATE_TEMPLATE_CREATE_INFO,
            .descriptorUpdateEntryCount = (uint32_t)sh->nb, .pDescriptorUpdateEntries = e,
            .templateType = VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_PUSH_DESCRIPTORS_KHR, .descriptorSetLayout = sh->dsl_push,
            .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .pipelineLayout = layout, .set = set};
        VKCHECK(vkCreateDescriptorUpdateTemplate(V.dev, &ci, NULL, found));
        free(e);
        nvn_hm_put(&templates, key, found);
        static uint64_t made; made++;
        if (made <= 4 || !(made & (made - 1)))
            LOGV("descriptor push (oot-a121): %llu push templates so far (this one: shader %.16s, set %u, %d bindings, %s path)",
                 (unsigned long long)made, sh->key, set, sh->nb, kind ? "primary" : "recorder");
    }
    last[kind][set & 1].layout = layout; last[kind][set & 1].t = *found;
    return *found;
}
/* primary path, per 300 frames: sets pushed, pushes left out (the same infos still pushed under the layout), bindless
   binds left out and sets left bound above a rebound one (bit 4); the hash of the infos last pushed at set 0 / 1 */
static uint64_t g_push_n[4], g_push_hash[2][2];
#endif
struct freeze_key {
    uint64_t chunk, submission, uniforms, bindings, resources, samplers, cpu_dirty, dirty_epoch;
    const vk_record_spans *spans; vk_shader *shader; uint32_t width, height, stage; vk_tex *native_sample;
    uint64_t feedback_pass;   /* TOTK_VK_FEEDBACK_CUT: views depend on the open pass's attachments */
    VkImage native_image;
};
static struct { struct freeze_key key; __typeof__(g_record_draw->sets[0]) set; record_binding *bindings; int cap, valid; } g_freeze_memo[128];
#include "vk_xlate_draw_stats.h"
/* Constant-specialized entry points keep all diagnostic work out of the normal binding walk. */
static inline __attribute__((always_inline)) int record_stage_impl(vk_shader *sh, int stage, uint32_t set, int stats) {
    if (sh->bindless) g_record_draw->bindless = vk_bindless_set();   /* the pools' set of this slot (vk_res.c) */
    static int c1off = -1; if (c1off < 0) c1off = vk_off("block");
    struct freeze_key key;
    memset(&key, 0, sizeof key); key.chunk = vk_record_chunk_id(); key.submission = vk_submit_serial();
    key.uniforms = vk_record_uniform_revision(); key.bindings = g_record_bind_revision[stage];
    key.resources = vk_treg_revision(); key.samplers = vk_record_sampler_revision();
    /* A freeze hit bypasses the core copy memo. CPU writes through any physical alias must make it call
       the core again; a dirty-unit birth is only lifetime identity, never a content revision. */
    if (vk_lag_capture_on()) { key.cpu_dirty = nvn_dirty_now(); key.dirty_epoch = nvn_dirty_lag_epoch(); }
    else if (__builtin_expect(g_pt_on, 0)) key.cpu_dirty = vk_pt_generation();   /* oot-a122: a frozen / landed copy changed within the submission */
    key.spans = g_record_draw->spans; key.shader = sh; key.width = R.w; key.height = R.h; key.stage = (uint32_t)stage;
    key.native_sample = g_native_sample; if (feedback_cut_refresh()) key.feedback_pass = g_stat_passes;
    if (g_native_sample) key.native_image = vk_tex_native_image(g_native_sample);
    __typeof__(g_freeze_memo[0]) *last = &g_freeze_memo[((uintptr_t)sh >> 4) % 128];
    unsigned causes = 0;
    if (!c1off && last->valid && last->key.shader == sh && last->key.chunk == key.chunk &&
        last->key.submission == key.submission && last->key.stage == key.stage) {
        if (last->key.bindings != key.bindings) {
            int same = 1;
            for (int i = 0; i < sh->nb; i++) { record_binding value; record_binding_of(&sh->b[i], stage, &value);
                if (memcmp(&last->bindings[i], &value, sizeof value)) { same = 0; break; } }
            if (same) { last->key.bindings = key.bindings; vk_record_memo_saved(VK_REC_M_BINDING); }
        }
        if (last->key.uniforms != key.uniforms && record_uniforms_unchanged(sh, stage, last->key.uniforms)) {
            last->key.uniforms = key.uniforms; vk_record_memo_saved(VK_REC_M_UNIFORM);
        }
        if (!memcmp(&last->key, &key, sizeof key)) {
            g_record_draw->sets[set] = last->set; g_record_freeze_hits++;
#if defined(TOTK_OHOS) && defined(TOTK_GAME_OOT)
            if (!last->set.layout && last->set.count) g_record_draw->sets[set].update = push_template(g_record_draw->layout, set, sh, 0);   /* oot-a121: pushed; this draw's layout */
#endif
            if (stats) xlate_stage(sh, stage, &key, 1, 0, NULL);
            return 1;
        }
    }
    if (c1off) causes |= 1u << VK_REC_M_DISABLED;
    else if (!last->valid) causes |= 1u << VK_REC_M_COLD;
    else {
        if (last->key.uniforms != key.uniforms || last->key.cpu_dirty != key.cpu_dirty ||
            last->key.dirty_epoch != key.dirty_epoch) causes |= 1u << VK_REC_M_UNIFORM;
        if (last->key.bindings != key.bindings) causes |= 1u << VK_REC_M_BINDING;
        if (last->key.spans != key.spans) causes |= 1u << VK_REC_M_SPAN;
        if (last->key.chunk != key.chunk) causes |= 1u << VK_REC_M_CHUNK;
        if (last->key.submission != key.submission) causes |= 1u << VK_REC_M_SUBMISSION;
        if (last->key.shader != key.shader) causes |= 1u << VK_REC_M_SHADER;
        if (last->key.resources != key.resources) causes |= 1u << VK_REC_M_REGISTRY;
        if (last->key.samplers != key.samplers) causes |= 1u << VK_REC_M_SAMPLER;
        if (last->key.width != key.width || last->key.height != key.height) causes |= 1u << VK_REC_M_DIMENSIONS;
    }
    if (stats) xlate_stage(sh, stage, &key, 0, c1off, NULL);
    vk_record_memo_miss(0, causes);
    g_record_freeze_misses++;
    int cacheable = !c1off;
    uint32_t count = 0; for (int i = 0; i < sh->nb; i++) { count += sh->b[i].count; if (sh->b[i].rw) cacheable = 0; }
    uint64_t unwatched = vk_lag_copy_unwatched();
    vk_record_descriptor *input = vk_record_descriptors(g_record_draw, set, sh->dsl, count);
    for (int i = 0, k = 0; i < sh->nb; i++) for (int e = 0; e < sh->b[i].count; e++, k++) {
        const vk_bind *b = &sh->b[i]; vk_record_descriptor *d = &input[k];
        d->binding = b->binding; d->element = (uint32_t)e; d->type = (VkDescriptorType)b->dtype;
        int storage = b->dtype == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        if (storage || b->dtype == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER) {
            if (b->kind == RB_ALPHA) { alpha_desc(&d->b); continue; }
            if (b->kind == RB_SUPPORT || b->kind == RB_DRV0 || b->kind == RB_DRV2) { generated_desc(b->kind, stage, NULL, &d->b); continue; }
            if (b->kind == RB_PUSH) { push_desc(stage, &d->b); continue; }
            if (b->kind >= RB_MESH_IN && b->kind <= RB_MESH_PAR) { mesh_desc(b->kind, &d->b); cacheable = 0; continue; }   /* this draw's own buffers: never a frozen set */
            if (b->kind == RB_GMEM || b->kind == RB_GMEML) {
                if (!(b->kind == RB_GMEM ? vk_gmem_map(&d->b) : vk_gmem_log(&d->b))) zero_buffer(&d->b, 256);
                continue;
            }
            if (b->kind == RB_C1) {
                uint64_t sub = vk_submit_serial();
                if (c1off || sh->c1_sub != sub) {
                    size_t n = (sh->c1_len + 15) & ~(size_t)15; if (!n) n = 16;
                    VkBuffer buffer; VkDeviceSize offset; void *bytes = vk_transient(n, V.props.limits.minUniformBufferOffsetAlignment, &buffer, &offset);
                    memset(bytes, 0, n); if (sh->c1_len) memcpy(bytes, sh->c1, sh->c1_len);
                    sh->c1_d = (VkDescriptorBufferInfo){buffer, offset, n}; sh->c1_sub = sub;
                }
                d->b = sh->c1_d; continue;
            }
            uint64_t address = 0, size = 0;
            if (b->reg >= 0 && b->reg < 16) {
                if (b->kind == RB_CBUF || b->kind == RB_UBO_PTR) { address = S.ubo[stage][b->reg].addr; size = S.ubo[stage][b->reg].size; }
                else if (b->kind == RB_SBUF) { address = S.ssbo[stage][b->reg].addr; size = S.ssbo[stage][b->reg].size; }
                else if (b->kind == RB_SBUF_UBO) { const p_bind_buf *u = &S.ubo[stage][b->reg]; if (u->addr && u->size >= (uint64_t)b->off + 8) vk_ub_read_record(u->addr + b->off, &address, 8); }
            }
            if (!storage) {
                uint64_t range = MINV(size ? size : 0x10000, V.props.limits.maxUniformBufferRange);
                if (address && !vk_lag_uniform_read(address, range, V.props.limits.minUniformBufferOffsetAlignment, &d->b) &&
                    !vk_ub_snapshot_descriptor(address, range, &d->b)) ubo_desc(address, size, &d->b, 0, 0, b->extent, d);
            } else if (address) {
                /* Lag registration/capture belongs to the walk, once per actual submission (freeze key above).
                   Wait mode keeps a live descriptor; copy mode returns the immutable arena override. */
                if (vk_lag_capture_on() || __builtin_expect(g_pt_on, 0) || !vk_record_storage_aligned(g_record_draw->spans, address)) ubo_desc(address, size, &d->b, 1, b->rw, b->extent, d);   /* oot-a122: protected bindings are decided on the walker */
                else {
                    d->raw.address = address; d->raw.size = size;
                    VK_INFLIGHT(VK_INFLIGHT_STORAGE | (b->rw ? VK_INFLIGHT_GPUW : 0), address, b->extent && (!size || b->extent < size) ? b->extent : size, VK_NULL_HANDLE);   /* the worker binds the pool's buffer from this address (vk_record.c descriptor_set) */
                    if (b->rw) { VkBuffer buffer; uint64_t offset, available;
                        if (vk_record_resolve(g_record_draw->spans, address, 1, &buffer, &offset, &available, NULL)) {
                            uint64_t w = MINV(size ? size : available, available);
                            if (__builtin_expect(g_ic_k, 0) && (!size || size >= available)) {   /* in-flight check: no size; before the PLAIN note below, which keeps it */
                                vk_inflight_gpu_wrote(address, w, VK_INFLIGHT_GW_UNBOUNDED);
                                if (b->extent && b->extent < w) vk_inflight_gpu_wrote(address, b->extent, VK_INFLIGHT_GW_PLAIN);   /* the shader's reflected extent */
                            }
                            vk_gpu_write(address, w);
                        }
                    }
                }
            }
            continue;
        }
        uint64_t texture = b->reg >= 0 ? (b->kind == RB_IMG ? (b->reg < 32 ? S.img[stage][b->reg] : 0) : (b->reg < 128 ? S.tex[stage][b->reg] : 0)) : 0;
        if (b->dtype == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER || b->dtype == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER) {
            d->t = texture ? vk_tbuf_by_id_record((int)(texture & 0xfffff), b->rw, &d->lag_range, &d->lag_reason) : VK_NULL_HANDLE;
            if (!d->t) d->t = vk_dummy_tbuf(); continue;
        }
        if (b->dtype == VK_DESCRIPTOR_TYPE_SAMPLER) {
            int sid = b->kind == RB_SAMP && b->reg >= 0 && b->reg < 128 ? (int)((b->sreg == -2 ? S.tex[stage][b->reg] : S.samp[stage][b->reg]) >> 20) : 0;
            d->i.sampler = sid ? vk_samp_by_id(sid) : VK_NULL_HANDLE;
            if (!d->i.sampler) d->i.sampler = vk_dummy_sampler(); continue;
        }
        int want = b->dtype == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE ? -2 : b->target; vk_tex *object = NULL;
        d->i.imageView = texture ? native_texture_by_id((int)(texture & 0xfffff), &want, &object) : VK_NULL_HANDLE;
        if (stats) xlate_texture_view(texture, object, d->i.imageView);
        if (b->dtype == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE && object) vk_tex_mark_gpu_written(object);
        if (!d->i.imageView && texture) {   /* which shader and slot gets the dummy (the other path says so too) */
            static _Atomic int said; if (atomic_fetch_add(&said, 1) < 24)
                LOGV("texture id %d (stage %d slot %d kind %d target %d, shader %.16s) resolves to nothing while recording: a dummy is bound", (int)(texture & 0xfffff), stage, b->reg, b->kind, b->target, sh->key);
        }
        if (!d->i.imageView) d->i.imageView = vk_dummy_view(b->target, b->is_int);
        if (b->dtype == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) {
            int sid = b->reg >= 0 && b->reg < 128 ? (int)((b->sreg >= 0 && b->sreg < 128 ? S.samp[stage][b->sreg] : texture) >> 20) : 0;
            d->i.sampler = sid ? vk_samp_by_id(sid) : VK_NULL_HANDLE;
            if (!d->i.sampler) d->i.sampler = vk_dummy_sampler();
        }
    }
    if (stats) xlate_selected(sh, stage, input);
    /* A positive copy without complete CPU write-watch authority cannot be reused by a stage freeze,
       even when every observable revision is unchanged. The core reports those selections without
       requiring another alias scan here. */
    if (vk_lag_copy_unwatched() != unwatched) { cacheable = 0; last->valid = 0; }
    if (stats) xlate_uncacheable(sh, stage, cacheable, vk_lag_copy_unwatched() != unwatched);
#if defined(TOTK_OHOS) && defined(TOTK_GAME_OOT)
    if (R.open && count && sh->dsl_push && set == (uint32_t)(sh->stage == 1)) {   /* oot-a121 TOTK_OHOS_PUSHDESC: no set; the worker pushes these (vk_record.c push_set) */
        g_record_draw->sets[set].update = push_template(g_record_draw->layout, set, sh, 0); g_record_draw->sets[set].layout = VK_NULL_HANDLE;
    } else
#endif
    if (R.open && count) g_record_draw->sets[set].update = vk_record_template(sh->dsl, input, count);
    if (R.open && cacheable) {
        if (last->cap < sh->nb) { last->cap = sh->nb; last->bindings = realloc(last->bindings, (size_t)last->cap * sizeof *last->bindings); }
        for (int i = 0; i < sh->nb; i++) record_binding_of(&sh->b[i], stage, &last->bindings[i]);
        last->key = key; last->set = g_record_draw->sets[set]; last->valid = 1;
    } else if (!cacheable) vk_record_memo_miss(0, 1u << VK_REC_M_DISABLED);
    return R.open;
}
#if defined(TOTK_OHOS) && defined(TOTK_GAME_OOT)
/* ---------- oot-a108: the fixed cost of a draw (timing-only switches, all off by default; the picture is wrong) ----------
   TOTK_OHOS_DC_SETS=<1 vertex, 2 fragment, 3 both>  a stage keeps the descriptor set the same shader bound one draw earlier
   TOTK_OHOS_DC_PIPE=1                              no pipeline switch that only changes vertex attribute formats (same size and type) or lowers offsets
   TOTK_OHOS_DC_STATE=<1 no dynamic state, 2 buffer binds only on change>   (bit 2 keeps the picture right)
   Scope: TOTK_OHOS_DC_W (dc_scope, oot-a106). Read once; -1 until then. Measure on top of TOTK_OHOS_DC_TRI=1
   TOTK_OHOS_DC_SCISSOR=1 so that wrong uniforms change no vertex or pixel work. */
static int dc_scope(void);
static int g_dc2_sets = -1, g_dc2_pipe = -1, g_dc2_state = -1;
static uint64_t g_dc2_n[6];
static void dc2_init(void) {
    const char *a = getenv("TOTK_OHOS_DC_SETS"), *b = getenv("TOTK_OHOS_DC_PIPE"), *c = getenv("TOTK_OHOS_DC_STATE");
    g_dc2_sets = a && *a ? atoi(a) & 3 : 0; g_dc2_pipe = b && *b == '1'; g_dc2_state = c && *c ? atoi(c) & 3 : 0;
    if (g_dc2_sets || g_dc2_pipe || g_dc2_state)
        LOGV("draw cost experiment (oot-a108): keep descriptor sets %d (TOTK_OHOS_DC_SETS: 1 vertex, 2 fragment), reuse pipelines %d (TOTK_OHOS_DC_PIPE), state %d (TOTK_OHOS_DC_STATE: 1 no dynamic-state change inside a pass, 2 buffer binds only on change); scope TOTK_OHOS_DC_W; timing only, the picture is wrong",
             g_dc2_sets, g_dc2_pipe, g_dc2_state);
}
static void dc2_note(int which) {
    static const char *const what[6] = {"vertex descriptor sets kept", "fragment descriptor sets kept", "pipeline switches left out",
                                        "dynamic-state changes held", "vertex buffer binds left out", "index buffer binds left out"};
    uint64_t n = ++g_dc2_n[which];
    if (n <= 2 || !(n & (n - 1)))
        LOGV("draw cost experiment (oot-a108): %llu %s so far (%llu draws so far; this one into %ux%u)", (unsigned long long)n, what[which],
             (unsigned long long)g_stat_draws, (unsigned)R.w, (unsigned)R.h);
}
/* The shader whose set a graphics set index holds (bound by bind_stage_body in this command buffer; NULL: not one a
   draw may keep), and the set layout of the last set 0 bound. g_bound_set says whether the binding still stands. */
static vk_shader *g_dc_set_sh[2];
static VkDescriptorSetLayout g_dc_set0_dsl;
static int dc_sets_keep(vk_shader *sh, uint32_t set_index) {
    if (!(g_dc2_sets >> set_index & 1) || !R.open || g_mesh.on || sh->tf || sh->has_storage_writes || PD.active) return 0;
    if (g_dc_set_sh[set_index] != sh || g_bound_set[0][set_index].sub != vk_submit_serial() || !g_bound_set[0][set_index].set) return 0;
    if (vk_fg_extract_active && FGX.record) return 0;
    if (!dc_scope()) return 0;
    dc2_note((int)set_index);
    return 1;
}
/* After a graphics set 0 or 1 is bound: 1 when the sets above it stay bound (set 0 again with the same layout, and the
   fragment bit is on), so the caller does not mark them disturbed. */
static int dc_sets_bound(vk_shader *sh, uint32_t set_index) {
    int eligible = R.open && !g_mesh.on && !sh->tf && !sh->has_storage_writes && !(vk_fg_extract_active && FGX.record);
    g_dc_set_sh[set_index] = eligible ? sh : NULL;
    if (set_index) return 0;
    int same = (g_dc2_sets & 2) && g_dc_set0_dsl && g_dc_set0_dsl == sh->dsl;
    g_dc_set0_dsl = sh->dsl;
    if (!same) g_dc_set_sh[1] = NULL;
    return same;
}
#endif
#if defined(TOTK_OHOS) && defined(TOTK_GAME_OOT)
/* oot-a121 TOTK_OHOS_PUSHDESC on the primary command buffer (the pass profile's path; the recorder's is vk_record.c).
   a121_bound: what binding or pushing graphics set `set_index` under `layout` leaves bound. Vulkan: a lower set bound
   under a layout not compatible with this one for it is disturbed; the sets above stay bound when this set's previous
   binding was under a layout compatible for it (here: the same layout, `keep`, only with bit 4), else they are
   disturbed, which is what bind_stage_body always assumed. */
static void a121_bound(uint32_t set_index, VkPipelineLayout layout, VkDescriptorSet set, uint64_t sub, int keep) {
    for (uint32_t m = 0; m < set_index; m++) if (g_bound_set[0][m].layout != layout) g_bound_set[0][m].sub = 0;
    g_bound_set[0][set_index].sub = sub; g_bound_set[0][set_index].layout = layout; g_bound_set[0][set_index].set = set;
    if (keep) g_push_n[3]++;
    else for (uint32_t j = set_index + 1; j < 6; j++) g_bound_set[0][j].sub = 0;
}
/* The bindless set after a graphics set under `layout`. With bit 4 it is bound only when it is not bound under this
   layout in this submission already (the recorder binds it only when the layout changes, vk_record.c record_job). */
static void a121_bindless(VkCommandBuffer cb, VkPipelineLayout layout, uint64_t sub) {
    VkDescriptorSet bs = vk_bindless_set();
    int same = g_bound_set[0][VK_BINDLESS_SET].sub == sub && g_bound_set[0][VK_BINDLESS_SET].layout == layout;
    if ((vk_push_bits & 4) && same && g_bound_set[0][VK_BINDLESS_SET].set == bs) { g_push_n[2]++; return; }
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, VK_BINDLESS_SET, 1, &bs, 0, NULL);
    a121_bound(VK_BINDLESS_SET, layout, bs, sub, (vk_push_bits & 4) && same);
}
/* bit 4: a graphics stage set bound (bind_stage_body's tail) without taking the sets above it as disturbed */
static int a121_bind_keep(VkCommandBuffer cb, VkPipelineLayout layout, uint32_t set_index, vk_shader *sh, VkDescriptorSet set, uint64_t sub) {
    int keep = g_bound_set[0][set_index].sub == sub && g_bound_set[0][set_index].layout == layout;
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, set_index, 1, &set, 0, NULL);
    a121_bound(set_index, layout, set, sub, keep);
    if (sh->bindless) a121_bindless(cb, layout, sub);
    return 1;
}
/* A pushed stage set (sh->dsl_push; vk_layout_of put it in `layout`). The infos are bind_stage_body's, hashed in h; the
   same infos pushed last at this index under this layout in this submission, with that binding still standing (an
   entry with a null set), are not pushed again. No null handle reaches the driver (bind_stage_body: Maleoon
   dereferences them in descriptor writes): the written path's dummies, and the zero buffer for a buffer. */
static int push_stage(VkCommandBuffer cb, VkPipelineLayout layout, uint32_t set_index, vk_shader *sh, dinfo *info, uint64_t sub, const uint64_t h[2], int dsoff) {
    if (!dsoff && g_bound_set[0][set_index].sub == sub && g_bound_set[0][set_index].layout == layout && !g_bound_set[0][set_index].set &&
        g_push_hash[set_index][0] == h[0] && g_push_hash[set_index][1] == h[1]) { g_push_n[1]++; return 1; }
    for (int i = 0, k = 0; i < sh->nb; k += sh->b[i].count, i++) {
        const vk_bind *b = &sh->b[i];
        for (int e = 0; e < b->count; e++) {
            dinfo *in = &info[k + e];
            switch (b->dtype) {
                case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
                    if (!in->i.sampler) in->i.sampler = vk_dummy_sampler();
                    /* fall through */
                case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE: case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE: case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
                    if (!in->i.imageView) { in->i.imageView = vk_dummy_view_image(b->target, b->is_int); in->i.imageLayout = VK_IMAGE_LAYOUT_GENERAL; }
                    break;
                case VK_DESCRIPTOR_TYPE_SAMPLER:
                    if (!in->i.sampler) in->i.sampler = vk_dummy_sampler();
                    break;
                case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER: case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
                    if (!in->b.buffer) zero_buffer(&in->b, 256);
                    break;
                case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER: case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
                    if (!in->t) in->t = vk_dummy_tbuf();
                    break;
                default: break;
            }
        }
    }
    int keep = (vk_push_bits & 4) && g_bound_set[0][set_index].sub == sub && g_bound_set[0][set_index].layout == layout;
    vk_cmd_push_template(cb, push_template(layout, set_index, sh, 1), layout, set_index, info);
    a121_bound(set_index, layout, VK_NULL_HANDLE, sub, keep);
    g_push_hash[set_index][0] = h[0]; g_push_hash[set_index][1] = h[1]; g_push_n[0]++;
    if (sh->bindless) a121_bindless(cb, layout, sub);
    return 1;
}
#endif
/* Fill and bind one stage's descriptor set; stage: NVN stage. Returns 0 when a binding cannot be satisfied. */
static inline __attribute__((always_inline)) int bind_stage_body(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipelineLayout layout, uint32_t set_index, vk_shader *sh, int stage, const uint32_t grid[3], int stats) {
#if defined(TOTK_OHOS) && defined(TOTK_GAME_OOT)
    if (__builtin_expect(g_dc2_sets != 0, 0) && bp == VK_PIPELINE_BIND_POINT_GRAPHICS && !g_record_draw) {   /* oot-a108 (timing only) */
        if (g_dc2_sets < 0) dc2_init();
        if (!sh->nb) g_dc_set_sh[0] = g_dc_set_sh[1] = NULL;   /* a bindless bind below may disturb them */
        else if (g_dc2_sets > 0 && set_index < 2 && dc_sets_keep(sh, set_index)) return 1;
    }
#endif
    if (!sh->nb) {   /* nothing in the stage's set; the bindless pools may still be indexed */
        if (sh->bindless && g_record_draw) { if (R.open) g_record_draw->bindless = vk_bindless_set(); }
#if defined(TOTK_OHOS) && defined(TOTK_GAME_OOT)
        else if (sh->bindless && (vk_push_bits & 4) && bp == VK_PIPELINE_BIND_POINT_GRAPHICS) a121_bindless(cb, layout, vk_submit_serial());   /* oot-a121 bit 4 */
#endif
        else if (sh->bindless) { VkDescriptorSet bs = vk_bindless_set(); vkCmdBindDescriptorSets(cb, bp, layout, VK_BINDLESS_SET, 1, &bs, 0, NULL); }
        return 1;
    }
    (void)feedback_cut_refresh();   /* a live TOTK_VK_FEEDBACK_CUT takes effect at this descriptor build */
    if (__builtin_expect(PL.enabled, 0)) pso_life_reads(sh, (unsigned)stage);
    if (g_record_draw) return R.open ? record_stage_impl(sh, stage, set_index, stats) : 0;
    size_t total = 0; for (int i = 0; i < sh->nb; i++) total += sh->b[i].count;
    VkImageView replay_source = VK_NULL_HANDLE;
    static dinfo *info; static size_t capi;
    if (total > capi) { capi = total + 64; info = realloc(info, capi * sizeof *info); }
    memset(info, 0, total * sizeof *info);
    int k = 0;
    for (int i = 0; i < sh->nb; i++) {
        const vk_bind *b = &sh->b[i];
        dinfo *in = &info[k]; k += b->count;
        for (int e = 0; e < b->count; e++) {
            dinfo *d = &in[e];
            /* buffers */
            if (b->dtype == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || b->dtype == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) {
                int storage = b->dtype == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, ok = 0;
                if (b->kind == RB_ALPHA) { alpha_desc(&d->b); continue; }
                if (b->kind == RB_PUSH) { push_desc(stage, &d->b); continue; }
                if (b->kind >= RB_MESH_IN && b->kind <= RB_MESH_PAR) { mesh_desc(b->kind, &d->b); continue; }
                if (b->kind == RB_GMEM || b->kind == RB_GMEML) { if (!(b->kind == RB_GMEM ? vk_gmem_map(&d->b) : vk_gmem_log(&d->b))) zero_buffer(&d->b, 256); continue; }
                if (b->kind == RB_SUPPORT) { generated_desc(b->kind, stage, grid, &d->b); ok = 1; }
                else if (b->kind == RB_CBUF && b->reg >= 0 && b->reg < 16) ok = ubo_desc(S.ubo[stage][b->reg].addr, S.ubo[stage][b->reg].size, &d->b, storage, storage && b->rw, b->extent, NULL);
                else if (b->kind == RB_C1) {
                    uint64_t sub = vk_submit_serial();
                    static int c1off = -1; if (c1off < 0) c1off = vk_off("block");
                    if (c1off || sh->c1_sub != sub) { size_t n = (sh->c1_len + 15) & ~(size_t)15; if (!n) n = 16; VkBuffer tb; VkDeviceSize to; uint8_t *p = vk_transient(n, V.props.limits.minUniformBufferOffsetAlignment, &tb, &to); memset(p, 0, n); if (sh->c1_len) memcpy(p, sh->c1, sh->c1_len); sh->c1_d = (VkDescriptorBufferInfo){tb, to, n}; sh->c1_sub = sub; }
                    d->b = sh->c1_d; ok = 1;
                }
                else if (b->kind == RB_DRV0 || b->kind == RB_DRV2) { generated_desc(b->kind, stage, grid, &d->b); ok = 1; }
                else if (b->kind == RB_SBUF && b->reg >= 0 && b->reg < 16) ok = ubo_desc(S.ssbo[stage][b->reg].addr, S.ssbo[stage][b->reg].size, &d->b, storage, storage && b->rw, b->extent, NULL);
                else if (b->kind == RB_UBO_PTR && b->reg >= 0 && b->reg < 16) ok = ubo_desc(S.ubo[stage][b->reg].addr, S.ubo[stage][b->reg].size, &d->b, storage, storage && b->rw, b->extent, NULL);
                else if (b->kind == RB_SBUF_UBO && b->reg >= 0 && b->reg < 16) {
                    const p_bind_buf *u = &S.ubo[stage][b->reg]; uint64_t va = 0;
                    if (u->addr && u->size >= (uint64_t)b->off + 8) vk_ub_read(u->addr + b->off, &va, 8);   /* with this submission's inline updates */
                    ok = va && ubo_desc(va, 0, &d->b, storage, storage && b->rw, b->extent, NULL);
                    if (!ok) RED(8, "storage buffer pointer in uniform slot %d+%u (%#llx) is outside the pools", b->reg, b->off, (unsigned long long)va);
                }
                if (!ok) { zero_buffer(&d->b, storage ? 256 : 256); if (b->kind != RB_SBUF_UBO) { g_pzero++; g_stat_zero_buf++; } }
                else if (probe_on() && (b->kind == RB_CBUF || b->kind == RB_SBUF || b->kind == RB_UBO_PTR || b->kind == RB_SBUF_UBO) && b->reg >= 0 && b->reg < 16)
                    pass_buf_note(stage, b->reg + (b->kind == RB_SBUF ? 16 : 0), storage, b->kind == RB_SBUF ? S.ssbo[stage][b->reg].addr : S.ubo[stage][b->reg].addr, &d->b);
                continue;
            }
            /* texel buffers */
            if (b->dtype == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER || b->dtype == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER) {
                int slot = b->kind == RB_IMG ? b->reg : b->reg;
                uint64_t h = slot >= 0 ? (b->kind == RB_IMG ? (slot < 32 ? S.img[stage][slot] : 0) : (slot < 128 ? S.tex[stage][slot] : 0)) : 0;
                d->t = h ? vk_tbuf_by_id((int)(h & 0xfffff), b->rw) : VK_NULL_HANDLE;
                if (!d->t) d->t = vk_dummy_tbuf();
                continue;
            }
            /* images and samplers */
            int tslot = b->reg, target = b->target;
            if (b->dtype == VK_DESCRIPTOR_TYPE_SAMPLER) {
                int sid = 0;
                if (b->kind == RB_SAMP && tslot >= 0 && tslot < 128) sid = (int)((b->sreg == -2 ? S.tex[stage][tslot] : S.samp[stage][tslot]) >> 20);
                d->i.sampler = sid ? vk_samp_by_id(sid) : VK_NULL_HANDLE;
                if (!d->i.sampler) d->i.sampler = vk_dummy_sampler();
                continue;
            }
            VkImageView view = VK_NULL_HANDLE; vk_tex *x = NULL;
            if (b->dtype == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) {
                if (tslot >= 0 && tslot < 32 && S.img[stage][tslot]) { int want = -2; view = native_texture_by_id((int)(S.img[stage][tslot] & 0xfffff), &want, &x); }
                if (stats && tslot >= 0 && tslot < 32) xlate_texture_view(S.img[stage][tslot], x, view);
                if (x) { vk_tex_mark_gpu_written(x); probe_write_note('X', x, NULL, 0); }
                d->i.imageView = view ? view : b->count > 1 ? vk_dummy_view_image(target, b->is_int) : vk_dummy_view(target, b->is_int); d->i.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
                continue;
            }
            if (tslot >= 0 && tslot < 128 && S.tex[stage][tslot]) {
                int want = target; view = native_texture_by_id((int)(S.tex[stage][tslot] & 0xfffff), &want, &x);
                if (stats) xlate_texture_view(S.tex[stage][tslot], x, view);
                if (!view) { g_pdummy++; uint64_t n = ++g_stat_dummy_tex;
                    if (n <= 16) LOGV("texture id %d (stage %d slot %d, shader %.16s) resolves to nothing: an all-zero dummy is bound (%llu so far)", (int)(S.tex[stage][tslot] & 0xfffff), stage, tslot, sh->key, (unsigned long long)n); }
                else if (x && R.open) {
                    uint64_t im = (uint64_t)(uintptr_t)vk_tex_image(x); int k = 0; while (k < g_npin && g_pin[k].img != im) k++;
                    if (k == g_npin && g_npin < 16) g_pin[g_npin++] = (probe_in){im, vk_tex_width(x, 0), vk_tex_height(x, 0), vk_tex_nvn_format(x), x};
                }
            }
            if (stage == 1 && R.open && !g_pkey[0]) snprintf(g_pkey, sizeof g_pkey, "%.16s", sh->key);
            if (vk_fg_extract_active && FGX.record && FGX.record->final &&
                fg_extract_canvas(x,0,0))
                replay_source = view;
            d->i.imageView = view ? view : b->count > 1 ? vk_dummy_view_image(target, b->is_int) : vk_dummy_view(target, b->is_int); d->i.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            if (b->dtype == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) {
                int sid = 0;
                if (tslot >= 0 && tslot < 128) sid = (int)((b->sreg >= 0 && b->sreg < 128 ? S.samp[stage][b->sreg] : S.tex[stage][tslot]) >> 20);
                d->i.sampler = sid ? vk_samp_by_id(sid) : VK_NULL_HANDLE;
                if (!d->i.sampler) d->i.sampler = vk_dummy_sampler();
            }
            if (b->count > 1 && e == 0) RED(4, "texture array binding (%u elements, slot %d): every element shows the first texture", b->count, tslot);
        }
        (void)g_zero_ubo; (void)g_zero_ubo_off;
    }
    if (stats) xlate_immediate_stage(sh, stage, info);
    if (g_pd_cap && stage == 0) {
        g_pd_nubo = 0;
        for (int i = 0, kk = 0; i < sh->nb; kk += sh->b[i].count, i++)
            if (sh->b[i].dtype == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER && g_pd_nubo < 32) g_pd_ubo[g_pd_nubo++] = (__typeof__(g_pd_ubo[0])){sh->b[i].binding, sh->b[i].kind, sh->b[i].reg, info[kk].b};
    }
    uint64_t sub = vk_submit_serial(), h[2] = {0xcbf29ce484222325ull, 0x6a09e667f3bcc909ull};
    hash_words(&sh->dsl, sizeof sh->dsl, h); hash_words(info, total * sizeof *info, h);
    unsigned slot = (unsigned)(h[0] % DS_CACHE);
    VkDescriptorSet set;
    static int dsoff = -1; if (dsoff < 0) dsoff = vk_off("ds");
#if defined(TOTK_OHOS) && defined(TOTK_GAME_OOT)
    if (sh->dsl_push && bp == VK_PIPELINE_BIND_POINT_GRAPHICS && set_index == (uint32_t)(sh->stage == 1))   /* oot-a121 TOTK_OHOS_PUSHDESC */
        return push_stage(cb, layout, set_index, sh, info, sub, h, dsoff);
#endif
    if (!dsoff && g_dsc[slot].sub == sub && g_dsc[slot].hash[0] == h[0] && g_dsc[slot].hash[1] == h[1] && g_dsc[slot].dsl == sh->dsl && g_dsc[slot].n == total) {
        set = g_dsc[slot].set; g_stat_sets_reused++;
    } else {
        set = vk_alloc_set(sh->dsl);
#ifdef TOTK_OHOS
        {   /* Maleoon's vkUpdateDescriptorSets dereferences some null handles even with nullDescriptor (Ocarina of Time, loading
               a save: SIGSEGV at 0x29 in the driver). Every null image view, sampler, buffer and texel buffer view of the write is
               reported once per shader and binding and replaced by a real dummy. */
            for (int i = 0, kk = 0; i < sh->nb; kk += sh->b[i].count, i++) {
                const vk_bind *bb = &sh->b[i]; int bad = 0;
                for (int e = 0; e < bb->count; e++) {
                    dinfo *in = &info[kk + e];
                    switch (bb->dtype) {
                        case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
                            if (!in->i.sampler) { in->i.sampler = vk_dummy_sampler(); bad |= 1; }
                            /* fall through */
                        case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE: case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE: case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
                            if (!in->i.imageView) { in->i.imageView = vk_dummy_view_image(bb->target, bb->is_int); in->i.imageLayout = VK_IMAGE_LAYOUT_GENERAL; bad |= 2; }
                            break;
                        case VK_DESCRIPTOR_TYPE_SAMPLER:
                            if (!in->i.sampler) { in->i.sampler = vk_dummy_sampler(); bad |= 1; }
                            break;
                        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER: case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
                        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC: case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
                            if (!in->b.buffer) bad |= 4;
                            break;
                        case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER: case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
                            if (!in->t) bad |= 8;
                            break;
                        default: break;
                    }
                }
                if (bad) {
                    static struct { char key[17]; int binding; } said[64]; static int nsaid;
                    int k = 0; while (k < nsaid && !(said[k].binding == bb->binding && !strncmp(said[k].key, sh->key, 16))) k++;
                    if (k == nsaid && nsaid < 64) { snprintf(said[nsaid].key, sizeof said[nsaid].key, "%.16s", sh->key); said[nsaid].binding = bb->binding; nsaid++;
                        RED(64, "null descriptor (%s%s%s%s) in shader %.16s binding %u (kind %u, type %u, %u elements, slot %d): %s", bad & 1 ? "sampler " : "", bad & 2 ? "image view " : "",
                            bad & 4 ? "buffer " : "", bad & 8 ? "texel buffer " : "", sh->key, bb->binding, bb->kind, bb->dtype, bb->count, bb->reg, bad & 12 ? "left null" : "a dummy is written"); }
                }
            }
        }
#endif
        /* Writes are only consumed on a cache miss; hits need only the resolved infos (also used by pass dumps). */
        static VkWriteDescriptorSet *w; static size_t capw;
        if ((size_t)sh->nb > capw) { capw = (size_t)sh->nb + 16; w = realloc(w, capw * sizeof *w); }
        /* An array binding's elements must be consecutive VkDescriptorImageInfo / BufferInfo / VkBufferView: dinfo holds all
           three, so pImageInfo = &in->i read the second element from the middle of the first dinfo (Ocarina of Time's 32-element
           pool texture arrays: garbage handles, SIGSEGV in Maleoon's vkUpdateDescriptorSets). They are copied out here. */
        static VkDescriptorImageInfo *ai; static VkDescriptorBufferInfo *abuf; static VkBufferView *at; static size_t capa;
        if ((size_t)total > capa) { capa = (size_t)total + 64; ai = realloc(ai, capa * sizeof *ai); abuf = realloc(abuf, capa * sizeof *abuf); at = realloc(at, capa * sizeof *at); }
        for (int i = 0, kk = 0; i < sh->nb; i++) {
            const vk_bind *b = &sh->b[i]; dinfo *in = &info[kk];
            const VkDescriptorImageInfo *pi = &in->i; const VkDescriptorBufferInfo *pb = &in->b; const VkBufferView *pt = &in->t;
            if (b->count > 1) {
                for (int e = 0; e < b->count; e++) { ai[kk + e] = in[e].i; abuf[kk + e] = in[e].b; at[kk + e] = in[e].t; }
                pi = &ai[kk]; pb = &abuf[kk]; pt = &at[kk];
            }
            kk += b->count;
            w[i] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = b->binding, .descriptorCount = b->count, .descriptorType = (VkDescriptorType)b->dtype,
                .pBufferInfo = pb, .pImageInfo = pi, .pTexelBufferView = pt};
        }
        vkUpdateDescriptorSets(V.dev, (uint32_t)sh->nb, w, 0, NULL);
        g_dsc[slot].sub = sub; g_dsc[slot].hash[0] = h[0]; g_dsc[slot].hash[1] = h[1]; g_dsc[slot].dsl = sh->dsl; g_dsc[slot].n = total; g_dsc[slot].set = set;
        g_stat_sets_new++;
    }
    if (vk_fg_extract_active && FGX.record && set_index < 2) {
        VkDescriptorSet replay_set = set;
        if (replay_source) {
            replay_set = vk_alloc_set(sh->dsl);
            for (int i = 0, first = 0; i < sh->nb; i++) {
                const vk_bind *b = &sh->b[i]; dinfo *in = &info[first]; first += b->count;
                if (b->dtype == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER || b->dtype == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE)
                    for (int e = 0; e < b->count; e++)
                        if (in[e].i.imageView == replay_source) in[e].i.imageView = vk_fg_canvas_view();
                VkWriteDescriptorSet write = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                    .dstSet=replay_set, .dstBinding=b->binding, .descriptorCount=b->count,
                    .descriptorType=(VkDescriptorType)b->dtype, .pBufferInfo=&in->b,
                    .pImageInfo=&in->i, .pTexelBufferView=&in->t};
                vkUpdateDescriptorSets(V.dev,1,&write,0,NULL);
            }
            FGX.record->final = 2; /* exact canvas input was replaced, not a shader-name-only guess */
        }
        if (FGX.record->final &&
                atomic_load_explicit(&vk_fg_terminal_owned_active,memory_order_relaxed)) {
            for (int i=0,first=0;i<sh->nb;i++) {
                const vk_bind *b=&sh->b[i]; dinfo *in=&info[first]; first+=b->count;
                VkWriteDescriptorSet write={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                    .dstBinding=b->binding,.descriptorCount=b->count,
                    .descriptorType=(VkDescriptorType)b->dtype,.pBufferInfo=&in->b,
                    .pImageInfo=&in->i,.pTexelBufferView=&in->t};
                if (!vk_fg_terminal_owned_binding(set_index,sh->dsl,&write,b->rw))
                    vk_fg_terminal_owned_fail();
            }
        }
        FGX.record->sets[set_index] = replay_set;
    }
    int bi = bp == VK_PIPELINE_BIND_POINT_COMPUTE;
    if (!dsoff && set_index < 6 && g_bound_set[bi][set_index].sub == sub && g_bound_set[bi][set_index].set == set && g_bound_set[bi][set_index].layout == layout) return 1;
#if defined(TOTK_OHOS) && defined(TOTK_GAME_OOT)
    if (__builtin_expect(vk_push_bits & 4, 0) && !bi && set_index < 2 && g_dc2_sets <= 0) return a121_bind_keep(cb, layout, set_index, sh, set, sub);   /* oot-a121 bit 4 */
#endif
    vkCmdBindDescriptorSets(cb, bp, layout, set_index, 1, &set, 0, NULL);
    if (sh->bindless) { VkDescriptorSet bs = vk_bindless_set(); vkCmdBindDescriptorSets(cb, bp, layout, VK_BINDLESS_SET, 1, &bs, 0, NULL); }
    if (set_index < 6) { g_bound_set[bi][set_index].sub = sub; g_bound_set[bi][set_index].set = set; g_bound_set[bi][set_index].layout = layout; }
#if defined(TOTK_OHOS) && defined(TOTK_GAME_OOT)
    if (g_dc2_sets > 0 && !bi && set_index < 2 && dc_sets_bound(sh, set_index)) return 1;   /* oot-a108: the sets above stay bound */
#endif
    for (uint32_t j = set_index + 1; j < 6; j++) g_bound_set[bi][j].sub = 0;   /* a later set may have been disturbed */
    return 1;
}
/* Descriptor counters' walker-ns: complete nonempty stage selection/binding, only while timing is enabled. */
static inline __attribute__((always_inline)) int bind_stage_impl(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipelineLayout layout, uint32_t set_index, vk_shader *sh, int stage, const uint32_t grid[3], int stats) {
    int timed = __builtin_expect(vk_rec_timing_on, 0) && sh->nb;
    uint64_t start = timed ? host_now_ns() : 0;
    int ok = bind_stage_body(cb, bp, layout, set_index, sh, stage, grid, stats);
    if (timed) vk_record_descriptor_walk(host_now_ns() - start);
    return ok;
}
static int bind_stage(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipelineLayout layout, uint32_t set_index, vk_shader *sh, int stage, const uint32_t grid[3]) {
    return bind_stage_impl(cb, bp, layout, set_index, sh, stage, grid, 0);
}

/* ---------- draws ---------- */
static VkBuffer g_quad_ib; static uint32_t g_quad_max;
static void quad_indices(uint32_t quads) {
    if (g_quad_ib && quads <= g_quad_max) return;
    uint32_t n = quads < 65536 ? 65536 : quads;
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = (VkDeviceSize)n * 24, .usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT};
    VkBuffer b; VKCHECK(vkCreateBuffer(V.dev, &bci, NULL, &b));
    VkMemoryRequirements req; vkGetBufferMemoryRequirements(V.dev, b, &req);
    vk_alloc a; if (!vk_mem_alloc(&req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &a)) DIE("quad index buffer");
    VKCHECK(vkBindBufferMemory(V.dev, b, a.mem, a.off));
    uint32_t *ix; VKCHECK(vkMapMemory(V.dev, a.mem, a.off, (VkDeviceSize)n * 24, 0, (void **)&ix));
    for (uint32_t q = 0; q < n; q++) { uint32_t v = q * 4, t[6] = {v, v + 1, v + 2, v, v + 2, v + 3}; memcpy(ix + q * 6, t, 24); }
    vkUnmapMemory(V.dev, a.mem);
    g_quad_ib = b; g_quad_max = n;   /* the old buffer (if any) is left alive: it may be in flight */
}
#if defined(TOTK_GAME_ZA) || defined(TOTK_GAME_OOT)
static vk_shader *g_tf_vs;   /* the transform feedback capture variant of the draw being recorded (draw), else NULL */
#endif
#if defined(TOTK_OHOS) && defined(TOTK_GAME_OOT)
/* ---------- oot-a106: what a material draw costs the GPU (timing-only switches, all off by default) ----------
   The same 1511 draws cost 4.8 ms depth-only and 22 ms with their material shaders, and the 128x128 probe has cost
   11 - 40 us a draw, as much as the 854x480 scene or more. Each switch removes one part of a draw and leaves the rest;
   the picture is wrong while one is on. All are read once.
     TOTK_OHOS_DC_W=<width>   scope: only draws into a target that wide (128 the probe, 854 the scene, 1000 the shadow
                              maps); unset or 0: every target
     TOTK_OHOS_DC_TRI=1       an ordinary draw keeps three of its indices: pipeline, sets, uniforms and binds stay, the
                              vertices and pixels go; what is left is the fixed cost of a draw
     TOTK_OHOS_DC_SCISSOR=1   the scissor is one pixel: the vertex stage and binning stay, the fragments go
     TOTK_OHOS_DC_FSNULL=1, TOTK_OHOS_DC_FSVARY=1, TOTK_OHOS_DC_NOUSED=1   fragment twins (vk_shader.c vk_shader_dc)
   The log says once that a switch is on and counts the draws it changed (the first two, then powers of two). */
vk_shader *vk_shader_dc(vk_shader *fs, int writes_ds);   /* vk_shader.c */
static int g_dc_init, g_dc_tri, g_dc_scissor, g_dc_w;
static uint64_t g_dc_n[2];
static void dc_init(void) {
    const char *t = getenv("TOTK_OHOS_DC_TRI"), *s = getenv("TOTK_OHOS_DC_SCISSOR"), *w = getenv("TOTK_OHOS_DC_W");
    g_dc_tri = t && *t == '1'; g_dc_scissor = s && *s == '1'; g_dc_w = w && *w ? atoi(w) : 0; if (g_dc_w < 0) g_dc_w = 0;
    g_dc_init = 1;
    if (g_dc_tri || g_dc_scissor || g_dc_w)
        LOGV("draw cost experiment: three indices a draw %d (TOTK_OHOS_DC_TRI), one-pixel scissor %d (TOTK_OHOS_DC_SCISSOR), only targets %d wide (TOTK_OHOS_DC_W; 0: all); timing only, the picture is wrong",
             g_dc_tri, g_dc_scissor, g_dc_w);
}
static int dc_scope(void) {   /* the draw's target is one the experiment acts on */
    if (!g_dc_init) dc_init();
    if (!g_dc_w) return 1;
    /* (oot-a107) the pass's render area, as the pass profile groups passes: a probe's mip levels are other widths */
    if (R.open) return (int)R.w == g_dc_w;
    const tex_t *t = S.has_rt && S.rt.n && S.rt.color[0] ? S.rt.color[0] : S.has_rt ? S.rt.depth : NULL;
    return t && (int)t->d.width == g_dc_w;
}
static void dc_note(int which, uint32_t was) {
    uint64_t n = ++g_dc_n[which];
    if (n <= 2 || !(n & (n - 1)))
        LOGV("draw cost experiment: %llu draws %s so far (this one: %u indices or vertices, target %ux%u)", (unsigned long long)n,
             which ? "given a one-pixel scissor" : "cut to three indices", was, (unsigned)R.w, (unsigned)R.h);
}
#endif
#if defined(TOTK_OHOS) && defined(TOTK_GAME_OOT)
/* oot-a108 TOTK_OHOS_DC_PIPE: the key of the pipeline last bound by draw_inner (NULL: not known) */
static VkPipeline g_dc_pipe_last; static VkPipelineLayout g_dc_pipe_layout; static pipe_key g_dc_pipe_key;
/* An NVN vertex format (vk_fmt.c vk_vertex_fmt) as bytes << 2 | how the shader reads it (0 float, 1 uint, 2 sint); 0: not
   listed. The shaded pass's same-shader pipeline changes swap attribute 8 among R16G16 SFLOAT / UNORM / SNORM (pd-a64), so
   two formats of one size and type count as the same: the bound pipeline reads the same bytes as the same numeric type. */
static unsigned dc_vfmt_kind(uint32_t f) {
    switch (f) {
        case 0x01: case 0x02: return 1u << 2; case 0x03: return 1u << 2 | 1; case 0x04: return 1u << 2 | 2;
        case 0x05: case 0x06: case 0x07: case 0x0D: case 0x0E: return 2u << 2; case 0x08: case 0x0F: return 2u << 2 | 1; case 0x09: case 0x10: return 2u << 2 | 2;
        case 0x19: case 0x1A: return 3u << 2; case 0x1B: return 3u << 2 | 1; case 0x1C: return 3u << 2 | 2;
        case 0x0A: case 0x11: case 0x12: case 0x13: case 0x25: case 0x26: case 0x3D: case 0x3F: case 0x69: case 0x9A: return 4u << 2;
        case 0x0B: case 0x14: case 0x27: case 0x3E: return 4u << 2 | 1; case 0x0C: case 0x15: case 0x28: case 0x6A: return 4u << 2 | 2;
        case 0x1D: case 0x1E: case 0x1F: return 6u << 2; case 0x20: return 6u << 2 | 1; case 0x21: return 6u << 2 | 2;
        case 0x16: case 0x29: case 0x2A: case 0x2B: return 8u << 2; case 0x17: case 0x2C: return 8u << 2 | 1; case 0x18: case 0x2D: return 8u << 2 | 2;
        case 0x22: return 12u << 2; case 0x23: return 12u << 2 | 1; case 0x24: return 12u << 2 | 2;
        case 0x2E: return 16u << 2; case 0x2F: return 16u << 2 | 1; case 0x30: return 16u << 2 | 2;
    }
    return 0;
}
static int dc_vfmt_same(uint32_t a, uint32_t b) { return a == b || (dc_vfmt_kind(a) && dc_vfmt_kind(a) == dc_vfmt_kind(b)); }
static VkPipeline dc_pipe_pick(VkPipeline p, VkPipelineLayout layout, vk_shader *const *sh, int prim) {
    if (g_dc2_pipe < 0) dc2_init();
    if (g_dc2_pipe <= 0 || !p || p == PIPE_FAILED || !g_bound_pipe || p == g_bound_pipe || g_bound_pipe != g_dc_pipe_last || layout != g_dc_pipe_layout) return p;
    if (g_mesh.on || g_tf_vs || g_load_pipeline || g_load_override || (vk_fg_extract_active && FGX.record) || PD.active || !dc_scope()) return p;
    pipe_key k; pipe_key_fill(&k, &S, sh[0], sh[1], prim);
    const pipe_key *o = &g_dc_pipe_key;
    if (o->vs != k.vs || o->fs != k.fs || memcmp(o->cfmt, k.cfmt, sizeof k.cfmt) || o->dfmt != k.dfmt || o->sfmt != k.sfmt || o->ncolor != k.ncolor ||
        o->samples != k.samples || o->topo_class != k.topo_class || o->poly_mode != k.poly_mode || memcmp(o->stream, k.stream, sizeof k.stream)) return p;
    for (int a = 0; a < 16; a++)   /* the bound pipeline reads no attribute later in a vertex than the draw's own layout does */
        if (!dc_vfmt_same(o->attr[a].fmt, k.attr[a].fmt) || o->attr[a].stream != k.attr[a].stream || o->attr[a].offset > k.attr[a].offset) return p;
    dc2_note(2);
    return g_bound_pipe;
}
static void dc_pipe_after(VkPipeline p, VkPipelineLayout layout, vk_shader *const *sh, int prim) {
    if (g_dc2_pipe <= 0 || p != g_bound_pipe || p == g_dc_pipe_last) return;
    if (g_load_pipeline || g_load_override || g_mesh.on || g_tf_vs) { g_dc_pipe_last = VK_NULL_HANDLE; return; }
    pipe_key_fill(&g_dc_pipe_key, &S, sh[0], sh[1], prim); g_dc_pipe_last = p; g_dc_pipe_layout = layout;
}
/* oot-a108 TOTK_OHOS_DC_STATE bit 1: a change of dynamic state after the pass's first draw is not issued */
static int dc_state_hold(void) {
    if (g_dc2_state < 0) dc2_init();
    if (!(g_dc2_state & 1) || g_dyn_dirty || g_tf_vs || !R.open || (vk_fg_extract_active && FGX.record) || !dc_scope()) return 0;
    dc2_note(3);
    return 1;
}
/* oot-a108 TOTK_OHOS_DC_STATE bit 2: what draw_buffers bound in this command buffer, pass and submission (the only
   vertex / index binds of the primary path; the mesh, quad and indirect paths forget the index buffer) */
static struct { VkCommandBuffer cb; uint64_t pass, sub; uint32_t valid; VkBuffer b[17]; VkDeviceSize o[17], sz[17], st[17];
                int ib_valid; VkBuffer ib; VkIndexType it; VkBuffer zb; VkDeviceSize zo; uint64_t zsub; } g_dcb;
static int dc_buf_scope(VkCommandBuffer cb) {
    if (g_dc2_state < 0) dc2_init();
    if (!(g_dc2_state & 2) || !R.open || !dc_scope()) return 0;
    uint64_t sub = vk_submit_serial();
    if (g_dcb.cb != cb || g_dcb.pass != g_stat_passes || g_dcb.sub != sub) { g_dcb.cb = cb; g_dcb.pass = g_stat_passes; g_dcb.sub = sub; g_dcb.valid = 0; g_dcb.ib_valid = 0; }
    return 1;
}
static int dc_vb_same(VkCommandBuffer cb, uint32_t s, VkBuffer b, VkDeviceSize o, VkDeviceSize sz, VkDeviceSize st) {
    if (s > 16 || !dc_buf_scope(cb)) return 0;
    if ((g_dcb.valid >> s & 1) && g_dcb.b[s] == b && g_dcb.o[s] == o && g_dcb.sz[s] == sz && g_dcb.st[s] == st) { dc2_note(4); return 1; }
    g_dcb.valid |= 1u << s; g_dcb.b[s] = b; g_dcb.o[s] = o; g_dcb.sz[s] = sz; g_dcb.st[s] = st;
    return 0;
}
static int dc_zero_buffer(VkBuffer *b, VkDeviceSize *o) {   /* one zero stream per submission (transients live as long as it) */
    if (g_dc2_state < 0) dc2_init();
    if (!(g_dc2_state & 2) || !R.open || !dc_scope()) return 0;
    uint64_t sub = vk_submit_serial();
    if (!g_dcb.zb || g_dcb.zsub != sub) { void *z = vk_transient(64, 16, &g_dcb.zb, &g_dcb.zo); memset(z, 0, 64); g_dcb.zsub = sub; }
    *b = g_dcb.zb; *o = g_dcb.zo;
    return 1;
}
/* The index buffer at offset 0 and the mesh's offset as firstIndex: 1 when that binding already stands */
static int dc_ib_rebase(VkCommandBuffer cb, VkBuffer ib, VkDeviceSize *io, VkIndexType it, uint32_t *first) {
    *first = 0;
    if (!dc_buf_scope(cb)) return 0;
    VkDeviceSize es = it == VK_INDEX_TYPE_UINT16 ? 2 : it == VK_INDEX_TYPE_UINT8_EXT ? 1 : 4;
    if (*io % es || *io / es > 0x7fffffffu) { g_dcb.ib_valid = 0; return 0; }
    *first = (uint32_t)(*io / es); *io = 0;
    if (g_dcb.ib_valid && g_dcb.ib == ib && g_dcb.it == it) { dc2_note(5); return 1; }
    g_dcb.ib_valid = 1; g_dcb.ib = ib; g_dcb.it = it;
    return 0;
}
#define DC_VB_SAME(cb, s, b, o, sz, st) (g_dc2_state != 0 && dc_vb_same(cb, s, b, o, sz, st))
#define DC_ZERO(b, o) (g_dc2_state != 0 && dc_zero_buffer(b, o))
#define DC_IB_REBASE(cb, ib, io, it, first) (g_dc2_state != 0 && dc_ib_rebase(cb, ib, io, it, first))
#define DC_IB_FORGET() (g_dcb.ib_valid = 0)
#else
#define DC_VB_SAME(cb, s, b, o, sz, st) 0
#define DC_ZERO(b, o) 0
#define DC_IB_REBASE(cb, ib, io, it, first) 0
#define DC_IB_FORGET() ((void)0)
#endif
static void dynamic_state(VkCommandBuffer cb, int prim) {
    /* everything the draw sets dynamically, compared with what this command buffer last set (unchanged: nothing issued) */
    vk_record_dynamic D;
    memset(&D, 0, sizeof D);
    float vx = S.has_vp ? S.vp[0][0] : 0, vy = S.has_vp ? S.vp[0][1] : 0, vw = S.has_vp ? S.vp[0][2] : (float)R.w, vh = S.has_vp ? S.vp[0][3] : (float)R.h;
    if (g_ui_aspect_on) vk_ui_aspect_viewport(&g_ui_aspect, &vx, &vy, &vw, &vh);   /* TOTK_ASPECT: HUD quad about the canvas centre */
    D.vp = (VkViewport){vx, vy + vh, vw, -vh, S.dr[0], S.dr[1]};
    if (D.vp.minDepth < 0) D.vp.minDepth = 0; if (D.vp.maxDepth > 1) D.vp.maxDepth = 1;
    int32_t x0 = 0, y0 = 0, x1 = (int32_t)R.w, y1 = (int32_t)R.h;
    if (S.has_sc) { x0 = MAXV(0, S.sc.x); y0 = MAXV(0, S.sc.y); x1 = MINV(x1, S.sc.x + S.sc.w); y1 = MINV(y1, S.sc.y + S.sc.h); }
    if (g_ui_aspect_on && S.has_sc) {   /* the pane's clip follows its squeezed content */
        vk_ui_aspect_rect(&g_ui_aspect, &x0, &y0, &x1, &y1);
        x0 = MAXV(0, x0); y0 = MAXV(0, y0); x1 = MINV(x1, (int32_t)R.w); y1 = MINV(y1, (int32_t)R.h);
    }
    D.sc = (VkRect2D){{x0, y0}, {(uint32_t)MAXV(0, x1 - x0), (uint32_t)MAXV(0, y1 - y0)}};
    if (R.native) {
        float sx = (float)R.host_w/R.native_w, sy = (float)R.host_h/R.native_h;
        D.vp.x *= sx; D.vp.width *= sx; D.vp.y *= sy; D.vp.height *= sy;
        D.sc = native_rect(D.sc);
    }
    D.bias_en = (S.poly.offset_en & 4) != 0; D.bias[0] = S.po.units; D.bias[1] = S.po.clamp; D.bias[2] = S.po.factor;
    if (R.dfmt == VK_FORMAT_D16_UNORM && d16tie_on()) {   /* see d16tie_on */
        if (!D.bias_en) { D.bias[0] = D.bias[1] = D.bias[2] = 0; D.bias_en = 1; }
        D.bias[0] += D16_NUDGE_UNITS;
    }
    memcpy(D.blend, S.blend_color, sizeof D.blend);
    for (int f = 0; f < 2; f++) { D.cmask[f] = S.st_vmask[f]; D.wmask[f] = S.st_wmask[f]; D.ref[f] = S.st_ref[f]; }
    static int flip = -1; if (flip < 0) { const char *e = getenv("TOTK_VK_WINDING_FLIP"); flip = e && atoi(e); }
    D.cull = S.poly.cull == NVN_FACE_FRONT ? VK_CULL_MODE_FRONT_BIT : S.poly.cull == NVN_FACE_BACK ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
#if defined(TOTK_GAME_ZA) || defined(TOTK_GAME_OOT)
    {   /* ZA_CULL_OFF=1 (diagnostic): no face culling at all, to tell a culled one-sided wall from one never drawn */
        static int off = -1; if (off < 0) { const char *e = getenv("ZA_CULL_OFF"); off = e && atoi(e) != 0; if (off) LOGV("ZA_CULL_OFF: face culling off"); }
        if (off) D.cull = VK_CULL_MODE_NONE;
    }
#endif
    D.front = ((S.poly.front == NVN_FRONT_FACE_CCW) != (flip != 0)) ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
    D.topo = topology(prim);
    int has_d = R.dfmt != VK_FORMAT_UNDEFINED, has_s = R.sfmt != VK_FORMAT_UNDEFINED;
    D.dt = has_d && S.ds.dt; D.dw = has_d && S.ds.dw; D.dc = vk_compare(S.ds.df); D.st = has_s && S.ds.st;
    D.sop[0][0] = vk_stencil_op(S.ds.fail_f); D.sop[0][1] = vk_stencil_op(S.ds.zpass_f); D.sop[0][2] = vk_stencil_op(S.ds.zfail_f); D.sop[0][3] = vk_compare(S.ds.sf_f);
    D.sop[1][0] = vk_stencil_op(S.ds.fail_b); D.sop[1][1] = vk_stencil_op(S.ds.zpass_b); D.sop[1][2] = vk_stencil_op(S.ds.zfail_b); D.sop[1][3] = vk_compare(S.ds.sf_b);
    D.restart = prim == 3 || prim == 5;
#if defined(TOTK_GAME_ZA) || defined(TOTK_GAME_OOT)
    if (g_tf_vs) {   /* transform feedback capture (vk_tf.inc): each point one fragment at pixel (0,0), nothing tested, culled or biased */
        D.vp = (VkViewport){0, 0, 1, 1, 0, 1}; D.sc = (VkRect2D){{0, 0}, {1, 1}};
        D.bias_en = 0; D.bias[0] = D.bias[1] = D.bias[2] = 0; D.cull = VK_CULL_MODE_NONE; D.dt = D.dw = 0; D.st = 0; D.restart = 0;
    }
#endif
#if defined(TOTK_OHOS) && defined(TOTK_GAME_OOT)
    /* oot-a106 (timing only): a one-pixel scissor; the draw's vertex stage and binning stay, its fragments go */
    if ((g_dc_init || (dc_init(), 1)) && g_dc_scissor && !g_tf_vs && dc_scope()) { D.sc = (VkRect2D){{0, 0}, {1, 1}}; dc_note(1, 0); }
#endif
    if (g_record_draw) { g_record_draw->dynamic = D; return; }
    if (vk_fg_extract_active && FGX.record) FGX.record->draw.dynamic = D;
    static __typeof__(D) last;
    static int dynoff = -1; if (dynoff < 0) dynoff = vk_off("dyn");
    if (!dynoff && !g_dyn_dirty && !memcmp(&D, &last, sizeof D)) return;
#if defined(TOTK_OHOS) && defined(TOTK_GAME_OOT)
    /* oot-a108 (timing only): `last` stays what was issued; a topology or restart change is still issued (the dynamic
       topology must stay in the bound pipeline's topology class) */
    if (__builtin_expect(g_dc2_state != 0, 0) && !dynoff && D.topo == last.topo && D.restart == last.restart && dc_state_hold()) return;
#endif
    int all = g_dyn_dirty || dynoff; g_dyn_dirty = 0;
    vk_record_dynamic_emit(cb, &D, &last, all);
}
/* Every stage's writable aliases precede any read-only preparation/capture. Repeated bindings still publish
   producer invalidations: unchanged addresses do not mean unchanged GPU contents. Only mapping lookup is reusable. */
static void storage_touch(int st, const vk_shader *sh, int writable) {
    if (!sh || (writable ? !sh->has_storage_writes : !sh->has_storage_reads)) return;
    int lag = vk_lag_capture_on(), updates = vk_record_updates_pending();
    if (!updates && (!writable || !lag)) return;
    for (int i = 0; i < sh->nb; i++) {
        const vk_bind *b = &sh->b[i];
        if (!!b->rw != writable) continue;
        if (writable && b->dtype == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER && lag) {
            int reg = b->reg;
            uint64_t h = b->kind == RB_IMG ? (reg >= 0 && reg < 32 ? S.img[st][reg] : 0) : (reg >= 0 && reg < 128 ? S.tex[st][reg] : 0);
            if (h) {
                if (vk_record_enabled()) { VkDescriptorBufferInfo live; uint32_t reason; (void)vk_tbuf_by_id_record((int)(h & 0xfffff), 1, &live, &reason); }
                else (void)vk_tbuf_by_id((int)(h & 0xfffff), 1);
            }
        }
        if (b->dtype != VK_DESCRIPTOR_TYPE_STORAGE_BUFFER || b->reg < 0 || b->reg >= 16) continue;
        uint64_t addr = 0, size = 0;
        if (b->kind == RB_SBUF) { addr = S.ssbo[st][b->reg].addr; size = S.ssbo[st][b->reg].size; }
        else if (b->kind == RB_CBUF || b->kind == RB_UBO_PTR) { addr = S.ubo[st][b->reg].addr; size = S.ubo[st][b->reg].size; }
        else if (b->kind == RB_SBUF_UBO) {
            const p_bind_buf *u = &S.ubo[st][b->reg];
            if (u->addr && u->size >= (uint64_t)b->off + 8) {
                if (vk_record_enabled()) vk_ub_read_record(u->addr + b->off, &addr, 8);
                else vk_ub_read(u->addr + b->off, &addr, 8);
            }
        }
        /* A known readonly range with no ordered update needs neither a pool lookup nor materialization.
           Writers must still resolve and invalidate contents on every draw, even with unchanged bindings. */
        uint64_t known = size;
        if (lag && b->extent) known = known ? MINV(known, b->extent) : b->extent;
        if (!writable && addr && known && !vk_ub_pending(addr, known)) continue;
        VkBuffer vb; uint64_t vo, avail;
        if (addr && vk_resolve_for(addr, 1, &vb, &vo, &avail, VK_LOOKUP_TOUCH)) {
            uint64_t n = MINV(size ? size : avail, avail);
            if (lag) {
                if (b->extent) n = MINV(n, b->extent);
                else if (!size) n = vk_lag_bound(addr, n);
            }
            if (writable && lag) {
                if (vk_record_enabled()) vk_gpu_write_record(addr, n);
                else vk_gpu_write(addr, n);
            }
            if (updates) vk_ub_touch(addr, n);
        }
    }
}
static void native_bound_update(unsigned st, int image, unsigned first, unsigned n) {
    for (unsigned i=first;i<first+n;i++) {
        uint64_t handle=image?S.img[st][i]:S.tex[st][i], bit=1ull<<(i&63);
        int candidate=handle && vk_native_descriptor((int)(handle&0xfffff));
        if (image) {
            if (candidate) g_native_bound[st].img|=(uint32_t)bit;
            else g_native_bound[st].img&=~(uint32_t)bit;
        } else {
            if (candidate) g_native_bound[st].tex[i/64]|=bit;
            else g_native_bound[st].tex[i/64]&=~bit;
        }
    }
    if (g_native_bound[st].tex[0] || g_native_bound[st].tex[1] || g_native_bound[st].img) g_native_bound_any|=1u<<st;
    else g_native_bound_any&=~(1u<<st);
}
/* Registration/promotion changes rebuild these tiny masks once, not once per
   draw. Ordinary descriptor changes update only their changed handle slots. */
static void native_bound_sync(void) {
    uint64_t revision=vk_native_descriptor_revision();
    if (revision==g_native_bound_revision) return;
    for (unsigned st=0;st<6;st++) { native_bound_update(st,0,0,S.tex_hi[st]); native_bound_update(st,1,0,32); }
    g_native_bound_revision=revision;
}
/* Resolve reflected reads only when they can touch a promoted backing. In
   particular, ordinary 1x feedback must NOT acquire an extra pass boundary. */
static void native_reads(vk_shader *const sh[6], unsigned first, unsigned end) {
    native_bound_sync();
    if (!g_native_bound_any) return;
    for (unsigned st=first;st<end;st++) {
        if (!sh[st] || !(g_native_bound_any&(1u<<st))) continue;
        if (!(g_native_bound[st].tex[0]&sh[st]->native_tex_mask[0]) &&
            !(g_native_bound[st].tex[1]&sh[st]->native_tex_mask[1]) &&
            !(g_native_bound[st].img&sh[st]->native_img_mask)) continue;
        for (int i=0;i<sh[st]->nb;i++) {
            const vk_bind *b=&sh[st]->b[i];
            if (b->dtype!=VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE && b->dtype!=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE &&
                b->dtype!=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) continue;
            uint64_t handle=b->kind==RB_IMG ? (b->reg>=0 && b->reg<32 ? S.img[st][b->reg] : 0) :
                (b->reg>=0 && b->reg<128 ? S.tex[st][b->reg] : 0);
            if (!handle || !vk_native_descriptor((int)(handle&0xfffff))) continue;
            int want=b->dtype==VK_DESCRIPTOR_TYPE_STORAGE_IMAGE?-2:b->target; vk_tex *x=NULL;
            vk_tex_by_id((int)(handle&0xfffff),&want,&x);
            if (x==g_native_sample || !vk_tex_native_backed(x)) continue;
            if (R.open && R.native) for (unsigned a=0;a<R.ncolor;a++)
                if (R.ctex[a] && vk_tex_image(x)==vk_tex_image(R.ctex[a])) {
                    vk_end_rendering_at(VK_PASS_END_TEXTURE_BARRIER,0); break;
                }
            vk_tex_native_read(x);
            if (b->dtype==VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) vk_tex_native_written(x,0);
        }
    }
}
static int canvas_draw_prepare(vk_shader *const sh[6], vk_canvas_image *transfer_source, vk_canvas_image *transfer_window) {
    g_native_sample = NULL; int one = 0, transfer = 0;
    if (R.open && !R.native && !R.canvas && !sh[0]->canvas_ui && (!sh[1] || !sh[1]->canvas_transfer)) {
        g_native_draw_1x=0;
        if (vk_native_ui_enabled()) native_reads(sh,0,2);
        return 0;
    }
    vk_tex *target = S.has_rt && S.rt.n == 1 ?
        (R.open ? R.ctex[0] : vk_tex_get(S.rt.color[0])) : NULL;
    unsigned target_level=target?view_level(&S.rt.cview[0],S.rt.has_cview[0]):0;
    unsigned target_layer=target && S.rt.has_cview[0] && S.rt.cview[0].set_layers?S.rt.cview[0].min_layer:0;
    if (target && !S.rt.depth && !target_level && !target_layer && sh[1] && sh[1]->canvas_transfer && vk_tex_canvas_window(target)) {
        vk_end_rendering_at(VK_PASS_END_ATTACHMENTS,0);
        for (int i = 0; i < sh[1]->nb; i++) {
            const vk_bind *b = &sh[1]->b[i];
            if (b->dtype != VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER || b->reg < 0 || b->reg >= 128) continue;
            int want = b->target; vk_tex *source = NULL;
            vk_tex_by_id((int)(S.tex[1][b->reg] & 0xfffff), &want, &source);
            if (source && source != target && b->target == 1 &&
                S.has_vp && !S.vp[0][0] && !S.vp[0][1] &&
                S.vp[0][2] == vk_tex_width(source,0) && S.vp[0][3] == vk_tex_height(source,0) &&
                vk_tex_width(source,0) <= vk_tex_width(target,0) && vk_tex_height(source,0) <= vk_tex_height(target,0) &&
                vk_tex_width(source,0) >= 640 && vk_tex_levels(source) == 1 && vk_tex_layers(source) == 1 &&
                !vk_tex_is3d(source) && vk_tex_samples(source) == 1 && vk_tex_aspect(source) == VK_IMAGE_ASPECT_COLOR_BIT) {
                *transfer_source=vk_tex_canvas_identity(source,0,0);
                *transfer_window=vk_tex_canvas_identity(target,0,0);
                vk_canvas_transfer(transfer_source,transfer_window,0); transfer=1;
                if (vk_native_ui_enabled()) {
                    vk_tex_native_promote(source); g_native_sample = source;
                    vk_tex_native_transfer(target,vk_tex_width(source,0),vk_tex_height(source,0));
                }
            }
        }
    }
    if (target) {
        /* Keep composition and ALL pre-UI full-screen work at 1x. One linear
           upscale at the first UI draw is cheaper than rerunning post at 2.25x. */
        vk_canvas_image identity=vk_tex_canvas_identity(target,target_level,target_layer);
        int first_ui = sh[0]->canvas_ui == 2 && vk_canvas_first_ui(&identity,sh[0]->key);
        int snapshot = first_ui && vk_fg_extract_active && !FGX.ui_seen;
        if (snapshot) fg_extract_boundary(target,target_level,target_layer);
        if (first_ui && vk_native_ui_enabled() && !S.rt.depth) vk_tex_native_ui_begin(target);
        if (vk_native_ui_enabled() && sh[1] && vk_tex_native_active(target)) one=sh[1]->native_1x;
        if (snapshot && FGX.captured && !FGX.invalid) {
            uint32_t width=vk_tex_width(target,target_level), height=vk_tex_height(target,target_level);
            if (vk_native_ui_enabled() && !S.rt.depth && !one && !target_level && !target_layer &&
                vk_tex_native_active(target) && vk_tex_native_view(target,S.rt.has_cview[0]?&S.rt.cview[0]:NULL,1)) {
                vk_post_output native;
                if (vk_tex_native_output(target,&native)) { width=native.w; height=native.h; }
            }
            FGX.ui_width = width; FGX.ui_height = height; FGX.ui_format = vk_tex_format(target);
            VkCommandBuffer cb = vk_cb();
            if (vk_gpuprof_on) vk_gpuprof_scope_begin(cb, "framegen");
            (void)vk_fg_prepare_canvas(cb,width,height);
            (void)vk_fg_replay_depth(cb,FGX.ui_depth,FGX.ui_depth_level,FGX.ui_depth_layer);
            if (vk_gpuprof_on) vk_gpuprof_scope_end(cb);
        }
        if (one) RED(16,"native UI v2: FragCoord.xy/storage shader %.64s executes at 1x",sh[1]->key);
    }
    if (R.open && (one != g_native_draw_1x || (target && R.native != (!one && vk_tex_native_active(target)))))
        vk_end_rendering_at(VK_PASS_END_ATTACHMENTS, 0);
    g_native_draw_1x = one;
    if (g_native_sample) {
        vk_end_rendering_at(VK_PASS_END_COPY, 0);
        if (!vk_tex_native_view(g_native_sample, NULL, 0)) {
            vk_tex_native_read(g_native_sample); g_native_sample = NULL;
        }
    }
    if (vk_native_ui_enabled()) native_reads(sh,0,2);
    return transfer;
}
/* Reflection is immutable. Enumerate only consumed UBO registers, once each, rather than all of both shaders'
   image/sampler/driver-bank descriptors on every draw. Storage writers run before every preparation; with
   TOTK_VK_PREPARE_MASK the dirty masks above keep only "already prepared under these revisions", never contents. */
static void draw_uniform_prepare(vk_shader *const *sh) {
    int on = prepare_mask_live();
    if (on != g_prepare_used) { g_prepare_used = on; g_uniform_prepare_dirty[0] = g_uniform_prepare_dirty[1] = UINT16_MAX; }
    if (on && vk_lag_uniform_prepare_changed()) g_uniform_prepare_dirty[0] = g_uniform_prepare_dirty[1] = UINT16_MAX;
    for (int st = 0; st < 2; st++) {
        unsigned consumed = sh[st] ? sh[st]->uniform_prepare_mask : 0;
        unsigned pending = consumed & (on ? g_uniform_prepare_dirty[st] : UINT16_MAX);
        g_prepare_skips += (unsigned)__builtin_popcount(consumed & ~pending);
        for (unsigned mask = g_prepare_check ? consumed : pending; mask; mask &= mask - 1) {
            unsigned reg = (unsigned)__builtin_ctz(mask), bit = 1u << reg;
            const p_bind_buf *u = &S.ubo[st][reg];
            /* A broad UBO can alias a smaller true producer: materialize its untouched logical neighbours
               before opening rendering, rather than ending the pass later during descriptor setup. */
            uint64_t size = MINV(u->size ? u->size : 0x10000, V.props.limits.maxUniformBufferRange);
            if (pending & bit) {
                if (u->addr) vk_lag_uniform_prepare(u->addr, size);
                g_prepare_calls++;
                g_uniform_prepare_dirty[st] &= (uint16_t)~bit;
            } else {
                if (u->addr) vk_lag_uniform_prepare_check(u->addr, size);
                g_prepare_checks++;
            }
        }
    }
}
static void draw_touch(vk_shader *const *sh, const p_draw *dw, const p_draw_idx *di) {
    for (int st = 0; st < 2; st++) storage_touch(st, sh[st], 1);
    for (int st = 0; st < 2; st++) storage_touch(st, sh[st], 0);
    if (nvn_dirty_lag_vma_pressure()) for (int st = 0; st < 2; st++) for (int i = 0; sh[st] && i < sh[st]->nb; i++) {
        const vk_bind *b = &sh[st]->b[i];
        if (b->dtype != VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE && b->dtype != VK_DESCRIPTOR_TYPE_STORAGE_IMAGE &&
            b->dtype != VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) continue;
        uint64_t texture = b->kind == RB_IMG ? (b->reg >= 0 && b->reg < 32 ? S.img[st][b->reg] : 0) :
            (b->reg >= 0 && b->reg < 128 ? S.tex[st][b->reg] : 0);
        /* Baseline texture watches remain active during lag fallback. Realize any changed inherited
           bindings before rendering, not while a worker packet's descriptor array is being filled. */
        int target = b->dtype == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE ? -2 : b->target;
        if (texture) (void)vk_tex_by_id((int)(texture & 0xfffff), &target, NULL);
    }
    if (!vk_record_updates_pending()) return;
    if (vk_lag_uniform_private()) draw_uniform_prepare(sh);
    uint32_t touched = 0;
    for (int a = 0; a < 16; a++) if ((sh[0]->vin_mask >> a & 1) && a < S.nattr && S.attr[a].fmt) {
        unsigned stream = S.attr[a].stream & 15;
        if (touched >> stream & 1) continue;
        touched |= 1u << stream;
        const NVNbufferRange *v = &S.vbuf[stream];
        if (v->address) vk_ub_touch(v->address, v->size ? v->size : 16);
    }
    if (di) vk_ub_touch(di->ibuf, (uint64_t)di->count * (di->itype == 0 ? 1 : di->itype == 1 ? 2 : 4));
    (void)dw;
}
#ifdef TOTK_VK_TESTS
/* Isolate reflection enumeration from Vulkan/producer queries. Both sides select the same twelve
   UBO registers; scalar is the former per-draw scan, summary is the production bit enumeration. */
int vk_draw_test_touch_bench(void) {
    enum { N = 128, DRAWS = 200000 };
    vk_bind bindings[N]; memset(bindings, 0, sizeof bindings);
    for (unsigned i = 0; i < N; i++) {
        bindings[i].dtype = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[i].reg = (int16_t)(i % 128);
    }
    for (unsigned i = 0; i < 12; i++) {
        bindings[i * 9].dtype = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        bindings[i * 9].kind = RB_CBUF; bindings[i * 9].reg = (int16_t)i;
    }
    vk_shader sh = {.nb = N, .b = bindings, .uniform_prepare_mask = 0xfff};
    uint64_t ns[2], checksums[2] = {0};
    for (unsigned summary = 0; summary < 2; summary++) {
        uint64_t start = host_now_ns();
        for (unsigned draw = 0; draw < DRAWS; draw++) {
            __asm__ volatile("" : : "r"(&sh) : "memory"); /* identical inputs; forbid loop-invariant hoisting */
            for (unsigned st = 0; st < 2; st++) {
                if (summary) {
                    for (unsigned mask = sh.uniform_prepare_mask; mask; mask &= mask - 1)
                        checksums[summary] += 1u << __builtin_ctz(mask);
                } else for (int i = 0; i < sh.nb; i++) {
                    const vk_bind *b = &sh.b[i];
                    if (b->dtype == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER && b->reg >= 0 && b->reg < 16 &&
                        (b->kind == RB_CBUF || b->kind == RB_UBO_PTR)) checksums[summary] += 1u << b->reg;
                }
            }
        }
        ns[summary] = host_now_ns() - start;
    }
    int failed = checksums[0] != checksums[1] || checksums[1] != (uint64_t)DRAWS * 2 * 0xfff;
    printf("lag-touch reflection 2x128 bindings/12 consumed UBOs per stage: scalar %.2f summary %.2f ns/draw; 5600 draws %.3f -> %.3f ms (enumeration only)\n",
        ns[0] / (double)DRAWS, ns[1] / (double)DRAWS, ns[0] / (double)DRAWS * .0056, ns[1] / (double)DRAWS * .0056);
    printf("%s: lag-touch identical register checksums %llu/%llu\n", failed ? "FAILED" : "ok",
        (unsigned long long)checksums[0], (unsigned long long)checksums[1]);
    return failed;
}
/* Called with real registered bindings and an outstanding private inline update by --lag-walk-bench.
   Isolate the production preparation path from pipeline/descriptor/vertex packet work. */
int vk_draw_test_prepare_bench(void) {
    enum { DRAWS = 200000 };
    vk_shader shader = {.uniform_prepare_mask = 15}, *sh[2] = {&shader, &shader};
    uint64_t calls = g_prepare_calls, skips = g_prepare_skips, checks = g_prepare_checks;
    uint64_t start = host_now_ns();
    for (int draw = 0; draw < DRAWS; draw++) draw_uniform_prepare(sh);
    uint64_t elapsed = host_now_ns() - start;
    printf("lag-prepare micro: %.3f ns/draw, %llu calls, %llu skipped, %llu oracle checks (%d draws, eight bound ranges, TOTK_VK_PREPARE_MASK=%d)\n",
        elapsed / (double)DRAWS, (unsigned long long)(g_prepare_calls - calls),
        (unsigned long long)(g_prepare_skips - skips), (unsigned long long)(g_prepare_checks - checks), DRAWS, prepare_mask_live());
    uint64_t expected = prepare_mask_live() ? 8 : (uint64_t)DRAWS * 8;
    return g_prepare_calls - calls != expected || g_prepare_calls - calls + g_prepare_skips - skips != (uint64_t)DRAWS * 8;
}
void vk_draw_test_prepare_counts(uint64_t *calls, uint64_t *skips, uint64_t *checks) {
    *calls = g_prepare_calls; *skips = g_prepare_skips; *checks = g_prepare_checks;
}
#endif
/* The walk needs clamped vertex ranges and immediate full-index eligibility before hot/copy selection.
   Cache only the exact mapping answer, never contents or lag classification. The immutable span generation
   survives submissions but changes on pool/view/alias mutation, including removal and same-address reuse. */
static int record_buffer_resolve(const vk_record_spans *spans, uint64_t address, uint64_t size, int whole,
                                 VkBuffer *buffer, uint64_t *offset, uint64_t *available) {
    enum { NR = 256 };
    static struct {
        uint64_t generation, address, size, offset, available;
        VkBuffer buffer; int whole, found;
    } memo[NR];
    static uint64_t hint_generation;
    static int hints[64];
    uint64_t generation = vk_record_spans_generation(spans);
    if (hint_generation != generation) {
        memset(hints, 0, sizeof hints); hint_generation = generation;
    }
    uint64_t hash = (address >> 4) ^ (address >> 16) ^ (address >> 32) ^ size ^ (unsigned)whole;
    unsigned h = (unsigned)(hash ^ (hash >> 8)) % NR;
    __typeof__(memo[0]) *m = &memo[h];
    if (m->generation != generation || m->address != address || m->size != size || m->whole != whole) {
        VkBuffer live = VK_NULL_HANDLE; uint64_t off = 0, avail = 0;
        int found = address && vk_record_resolve(spans, address, whole ? size : 1, &live, &off, &avail, hints);
        *m = (__typeof__(*m)){generation, address, size, off, avail, live, whole, found};
    }
    if (!m->found) return 0;
    *buffer = m->buffer; *offset = m->offset; if (available) *available = m->available;
    return 1;
}
/* Command-call blocks are separate from CPU setup; D remains included, not separately timed. */
static int record_buffers(vk_shader *sh, const p_draw *dw, const p_draw_idx *di, int prim, uint32_t count, uint32_t instances, uint32_t base_instance) {
    vk_record_draw *d = g_record_draw;
    for (int a = 0; a < 16; a++) if ((sh->vin_mask >> a & 1) && a < S.nattr && S.attr[a].fmt) d->vertex_mask |= 1u << (S.attr[a].stream & 15);
    for (uint32_t s = 0; s < 16; s++) if (d->vertex_mask >> s & 1) {
        d->vertex[s].address = S.vbuf[s].address; d->vertex[s].size = S.vbuf[s].size ? S.vbuf[s].size : 16; d->vertex[s].stride = S.stream[s].stride;
        /* Packets survive slot reuse: a previous hot/copy draw may have overridden this stream.
           Clear that version before choosing between today's live range and a new immutable copy. */
        d->vertex[s].buffer = VK_NULL_HANDLE; d->vertex[s].offset = 0; d->vertex[s].lag_reason = 0;
        if (g_load_override && s == g_load_stream) {
            d->vertex[s].address = 0; d->vertex[s].buffer = g_load_geometry.buffer;
            d->vertex[s].offset = g_load_geometry.offset; d->vertex[s].size = g_load_geometry.range;
            d->vertex[s].stride = g_load_vertex_bytes; continue;
        }
        if ((vk_lag_capture_on() || __builtin_expect(g_pt_on, 0)) && d->vertex[s].address) {   /* oot-a122: TOTK_OHOS_PROTECT reads vertex ranges here too */
            VkBuffer buffer; uint64_t offset, available;
            if (record_buffer_resolve(d->spans, d->vertex[s].address, d->vertex[s].size, 0, &buffer, &offset, &available)) {
                d->vertex[s].size = MINV(d->vertex[s].size, available);
                d->vertex[s].buffer = buffer; d->vertex[s].offset = offset;
                VkDescriptorBufferInfo frozen;
                if (vk_lag_read_record(d->vertex[s].address, d->vertex[s].size, 16, &frozen, &d->vertex[s].lag_reason, VK_LAG_VERTEX)) {
                    d->vertex[s].buffer = frozen.buffer; d->vertex[s].offset = frozen.offset;
                }
            }
        }
        VK_INFLIGHT(VK_INFLIGHT_VERTEX, d->vertex[s].address, d->vertex[s].size, d->vertex[s].buffer);   /* no buffer yet: the worker binds the pool's from this address */
    }
    if (sh->vin_mask) {
        d->vertex_mask |= 1u << ZERO_BINDING; d->vertex[ZERO_BINDING].address = 0; d->vertex[ZERO_BINDING].size = 64; d->vertex[ZERO_BINDING].stride = 0;
        d->vertex[ZERO_BINDING].buffer = VK_NULL_HANDLE; d->vertex[ZERO_BINDING].offset = 0; d->vertex[ZERO_BINDING].lag_reason = 0;
    }
    d->count = count; d->instances = instances; d->base_instance = base_instance; d->indirect = VK_NULL_HANDLE; d->indirect_offset = 0;
    if (g_mesh.on) {   /* a task / mesh draw (mesh_draw): the indices its kernel wrote, the count in the control block's draw record */
        d->index = g_mesh.buf[2].buffer; d->index_offset = g_mesh.buf[2].offset; d->index_type = VK_INDEX_TYPE_UINT32; d->vertex_offset = 0;
        d->indirect = g_mesh.buf[3].buffer; d->indirect_offset = g_mesh.buf[3].offset + 4 * MESH_C_DRAW;
        return 1;
    }
    if (dw && prim == 7) {
        if (count < 4) d->count = 0;
        else { quad_indices(count / 4); d->index = g_quad_ib; d->index_type = VK_INDEX_TYPE_UINT32; d->count = count / 4 * 6; d->vertex_offset = dw->first; }
    } else if (dw) d->first = (uint32_t)dw->first;
    else {
        uint32_t stride = di->itype == 0 ? 1 : di->itype == 1 ? 2 : 4; VkBuffer buffer; uint64_t offset;
        /* Eligibility affects immediate samples/conditions. Freeze the validated live handle too, so workers
           never repeat the mapping lookup or choose a version after this stream position. */
        if (!record_buffer_resolve(d->spans, di->ibuf, (uint64_t)count * stride, 1, &buffer, &offset, NULL)) { g_stat_skips[8]++; return 0; }
        d->index_type = di->itype == 0 ? VK_INDEX_TYPE_UINT8_EXT : di->itype == 1 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
        d->index = buffer; d->index_offset = offset; d->vertex_offset = di->base_vertex;
        if (di->itype != 0 || V.has_index_u8) {
            VkDescriptorBufferInfo frozen;
            if (vk_lag_read_record(di->ibuf, (uint64_t)count * stride, 16, &frozen, &d->index_lag_reason, VK_LAG_INDEX)) {
                d->index = frozen.buffer; d->index_offset = offset = frozen.offset;
            }
            d->index_bytes = (uint64_t)count * stride;
            VK_INFLIGHT(VK_INFLIGHT_INDEX, di->ibuf, (uint64_t)count * stride, d->index);
        }
        if (di->ind) {   /* DrawElementsIndirect: the argument record, resolved now like the index range */
            VkBuffer ab; uint64_t ao;
            if (di->itype == 0 && !V.has_index_u8) { g_stat_skips[8]++; return 0; }
            if (!record_buffer_resolve(d->spans, di->ind, 20, 1, &ab, &ao, NULL) || ao % 4) { g_stat_skips[8]++; return 0; }
            if (__builtin_expect(g_pt_on, 0)) { VkDescriptorBufferInfo l; if (vk_pt_read(di->ind, 20, 4, VK_LAG_OTHER, &l)) { ab = l.buffer; ao = l.offset; } }   /* oot-a122: landed / frozen arguments */
            d->indirect = ab; d->indirect_offset = ao;
            VK_INFLIGHT(VK_INFLIGHT_INDIRECT, di->ind, 20, ab);
        }
        if (di->itype == 0 && !V.has_index_u8) {
            #ifdef TOTK_VK_TESTS
            g_test_widened++;
            #endif
            VkDeviceSize to; uint16_t *copy = vk_transient((size_t)count * 2 + 2, 16, &d->index, &to);
            const uint8_t *source = (const uint8_t *)(uintptr_t)di->ibuf;
            for (uint32_t i = 0; i < count; i++) copy[i] = source[i] == 0xff ? 0xffff : source[i];
            d->index_address = 0; d->index_offset = offset = to; d->index_type = VK_INDEX_TYPE_UINT16;
        }
        if (offset % stride) { g_stat_skips[8]++; return 0; }
    }
    return 1;
}
#ifdef TOTK_VK_TESTS
static int g_test_terminal_geometry_freeze=1;
void vk_fg_test_terminal_geometry_freeze(int enabled) { g_test_terminal_geometry_freeze=!!enabled; }
#endif
/* Only the scheduler-owned terminal packet gets this stronger geometry
   authority. Real bindings keep their existing lag/live selection. A guest
   range always goes through the original-stream CPU/inline-update selector,
   even if lag selected an arena handle: its pending-GPU-writer guard must not
   be bypassed. address=0 is reserved for known CPU-generated zero and quad
   payloads at the callsites below, never an unknown buffer or live u8 source. */
static int fg_terminal_geometry_read(unsigned role,unsigned stream,uint64_t address,
        VkDescriptorBufferInfo source,VkDescriptorBufferInfo *out) {
    vk_fg_terminal_input input={.role=role,.stage=0,.stream=stream,
        .buffer=source.buffer,.offset=source.offset,.bytes=source.range,.freeze=0};
    if (!source.buffer || !source.range || source.range>VK_FG_TERMINAL_BYTES ||
            source.offset>UINT64_MAX-source.range) {
        vk_fg_terminal_owned_input_refuse(&input,"geometry-range");
        return 0;
    }
    if (address) {
        if (address>UINT64_MAX-source.range ||
                !vk_ub_replay_read(address,source.range,16,out)) {
            vk_fg_terminal_owned_input_refuse(&input,"replay-read-refused");
            return 0;
        }
    } else *out=source;
    return 1;
}
static inline __attribute__((always_inline)) int draw_buffers(VkCommandBuffer cb, vk_shader *sh, const p_draw *dw, const p_draw_idx *di, int prim, uint32_t count, uint32_t inst, uint32_t base_inst) {
    if (g_record_draw) return record_buffers(sh, dw, di, prim, count, inst, base_inst);
    VkBuffer vb[17]; VkDeviceSize vo[17], vsz[17], vst[17]; uint32_t used = 0;
    vk_record_draw *replay = vk_fg_extract_active && FGX.record ? &FGX.record->draw : NULL;
    int owned_geometry=replay && FGX.record->final &&
        atomic_load_explicit(&vk_fg_terminal_owned_active,memory_order_relaxed);
#ifdef TOTK_VK_TESTS
    owned_geometry=owned_geometry && g_test_terminal_geometry_freeze;
#endif
    if (owned_geometry && di && di->ind) {   /* the argument record is GPU-read: no CPU copy of this draw's geometry */
        vk_fg_terminal_input input={.role=VK_FG_TERMINAL_INPUT_INDEX,.stage=0,.stream=0,.bytes=20,.freeze=0};
        vk_fg_terminal_owned_input_refuse(&input,"indirect-draw");
    }
    if (replay) {
        replay->count = dw && prim == 7 ? (count / 4) * 6 : count;
        replay->instances = inst; replay->base_instance = base_inst;
        replay->first = dw && prim != 7 ? (uint32_t)dw->first : 0;
        replay->vertex_offset = dw ? dw->first : di->base_vertex;
    }
    for (int a = 0; a < 16; a++) if ((sh->vin_mask >> a & 1) && a < S.nattr && S.attr[a].fmt) used |= 1u << (S.attr[a].stream & 15);
    for (uint32_t s = 0; s < 16; s++) if (used >> s & 1) {
        VkBuffer b; uint64_t o, avail; uint64_t sz = S.vbuf[s].size ? S.vbuf[s].size : 16;
        int pool_backed=S.vbuf[s].address && vk_resolve_for(S.vbuf[s].address, 1, &b, &o, &avail, VK_LOOKUP_VERTEX);
        if (!pool_backed) { VkDeviceSize to; void *z = vk_transient(64, 16, &b, &to); memset(z, 0, 64); o = to; sz = 64; RED(4, "vertex stream %u is not in a memory pool", s); }
        else sz = MINV(sz, avail);
        VkDescriptorBufferInfo frozen;
        int lag_frozen=S.vbuf[s].address && vk_lag_read(S.vbuf[s].address, sz, 16, &frozen, VK_LAG_VERTEX);
        if (lag_frozen) { b = frozen.buffer; o = frozen.offset; }
        VkDeviceSize stride = S.stream[s].stride;
        if (g_load_override && s == g_load_stream) {
            b = g_load_geometry.buffer; o = g_load_geometry.offset; sz = g_load_geometry.range;
            stride = g_load_vertex_bytes;
        }
        VK_INFLIGHT(VK_INFLIGHT_VERTEX, S.vbuf[s].address, sz, b);
        vb[0] = b; vo[0] = o; vsz[0] = sz; vst[0] = stride;
        if (!DC_VB_SAME(cb, s, b, o, sz, stride))   /* oot-a108 TOTK_OHOS_DC_STATE bit 2; always false without it */
            VK_REC_DO(VK_REC_CMD, vkCmdBindVertexBuffers2(cb, s, 1, vb, vo, vsz, vst));
        if (replay) {
            replay->vertex_mask |= 1u << s;
            replay->vertex[s].buffer = b; replay->vertex[s].offset = o;
            replay->vertex[s].size = sz; replay->vertex[s].stride = stride;
            if (owned_geometry && fg_terminal_geometry_read(VK_FG_TERMINAL_INPUT_VERTEX,s,
                    pool_backed || lag_frozen ? S.vbuf[s].address : 0,(VkDescriptorBufferInfo){b,o,sz},&frozen)) {
                replay->vertex[s].buffer=frozen.buffer; replay->vertex[s].offset=frozen.offset;
            }
        }
    }
    if (sh->vin_mask) {
        VkBuffer zb; VkDeviceSize zo;
        if (!DC_ZERO(&zb, &zo)) { void *z = vk_transient(64, 16, &zb, &zo); memset(z, 0, 64); }   /* oot-a108: one a submission with DC_STATE bit 2 */
        VkDeviceSize zs = 64, zst = 0;
        if (!DC_VB_SAME(cb, ZERO_BINDING, zb, zo, zs, zst))
            VK_REC_DO(VK_REC_CMD, vkCmdBindVertexBuffers2(cb, ZERO_BINDING, 1, &zb, &zo, &zs, &zst));
        if (replay) {
            replay->vertex_mask |= 1u << ZERO_BINDING;
            replay->vertex[ZERO_BINDING].buffer = zb; replay->vertex[ZERO_BINDING].offset = zo;
            replay->vertex[ZERO_BINDING].size = zs; replay->vertex[ZERO_BINDING].stride = zst;
        }
    }
    VK_REC_DO(VK_REC_S, vk_occlusion_draw(cb)); pd_pre_draw(cb);
    if (g_mesh.on) {   /* a task / mesh draw (mesh_draw): the indices its kernel wrote, the count in the control block's draw record */
        DC_IB_FORGET();
        VK_REC_DO(VK_REC_CMD, vkCmdBindIndexBuffer(cb, g_mesh.buf[2].buffer, g_mesh.buf[2].offset, VK_INDEX_TYPE_UINT32);
                  vkCmdDrawIndexedIndirect(cb, g_mesh.buf[3].buffer, g_mesh.buf[3].offset + 4 * MESH_C_DRAW, 1, 20));
        totk_ft_count(FT_COUNT_DRAWS, 1);
        return 1;
    }
    if (dw && prim == 7) {
        if (count >= 4) {
            quad_indices(count / 4); DC_IB_FORGET();
            VK_REC_DO(VK_REC_CMD, vkCmdBindIndexBuffer(cb, g_quad_ib, 0, VK_INDEX_TYPE_UINT32); vkCmdDrawIndexed(cb, (count / 4) * 6, inst, 0, dw->first, base_inst));
            if (replay) {
                replay->index = g_quad_ib; replay->index_type = VK_INDEX_TYPE_UINT32;
                if (owned_geometry) {
                    /* This static buffer is not an arena. Reproduce only its
                       bounded, known CPU-generated prefix at the original bind;
                       do not grant arbitrary host-visible buffers copy authority. */
                    VkDescriptorBufferInfo source={g_quad_ib,0,(uint64_t)(count/4)*24}, frozen;
                    if (fg_terminal_geometry_read(VK_FG_TERMINAL_INPUT_INDEX,0,0,source,&frozen)) {
                        uint32_t *ix=vk_transient((size_t)source.range,16,&replay->index,&replay->index_offset);
                        for (uint32_t q=0;q<count/4;q++) {
                            uint32_t v=q*4,t[6]={v,v+1,v+2,v,v+2,v+3}; memcpy(ix+q*6,t,24);
                        }
                    }
                }
            }
            totk_ft_count(FT_COUNT_DRAWS, 1);
        }
    } else if (dw) { VK_REC_DO(VK_REC_CMD, vkCmdDraw(cb, count, inst, (uint32_t)dw->first, base_inst)); totk_ft_count(FT_COUNT_DRAWS, 1); }
    else {
        VkBuffer ib; uint64_t io;
        uint32_t isz = di->itype == 0 ? 1 : di->itype == 1 ? 2 : 4;
        if (!vk_resolve_for(di->ibuf, (uint64_t)count * isz, &ib, &io, NULL, VK_LOOKUP_VERTEX)) {
            if (owned_geometry) {
                vk_fg_terminal_input input={.role=VK_FG_TERMINAL_INPUT_INDEX,.stage=0,.stream=0,
                    .bytes=(uint64_t)count*isz,.freeze=0};
                vk_fg_terminal_owned_input_refuse(&input,"index-buffer-not-in-pool");
            }
            RED(4, "index buffer is not in a memory pool"); g_stat_skips[8]++; return 0;
        }
        VkDescriptorBufferInfo frozen;
        int frozen_u8=0;
        if (owned_geometry && di->itype==0 && !V.has_index_u8) {
            /* Real u8 widening below retains its existing live selection.
               Optional replay instead needs authority for the ORIGINAL bytes,
               including inline updates and the pending-GPU-writer guard. */
            if ((uint64_t)count*2>VK_FG_TERMINAL_BYTES) {
                vk_fg_terminal_input input={.role=VK_FG_TERMINAL_INPUT_INDEX,.stage=0,.stream=0,
                    .buffer=ib,.offset=io,.bytes=count,.freeze=0};
                vk_fg_terminal_owned_input_refuse(&input,"widened-index-range");
            } else frozen_u8=fg_terminal_geometry_read(VK_FG_TERMINAL_INPUT_INDEX,0,di->ibuf,
                (VkDescriptorBufferInfo){ib,io,count},&frozen);
        }
        if ((di->itype != 0 || V.has_index_u8) && vk_lag_read(di->ibuf, (uint64_t)count * isz, 16, &frozen, VK_LAG_INDEX)) { ib = frozen.buffer; io = frozen.offset; }
        if (di->itype != 0 || V.has_index_u8) VK_INFLIGHT(VK_INFLIGHT_INDEX, di->ibuf, (uint64_t)count * isz, ib);
        VkIndexType it = di->itype == 1 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
        if (di->itype == 0 && di->ind && !V.has_index_u8) { RED(4, "indirect draw with 8-bit indices: not drawn (no uint8 index type)"); g_stat_skips[8]++; return 0; }
        if (di->itype == 0) {
            if (V.has_index_u8) it = VK_INDEX_TYPE_UINT8_EXT;
            else {
                #ifdef TOTK_VK_TESTS
                g_test_widened++;
                #endif
                VkDeviceSize to; uint16_t *w = vk_transient((size_t)count * 2 + 2, 16, &ib, &to); const uint8_t *src = (const uint8_t *)(uintptr_t)di->ibuf;
                for (uint32_t i = 0; i < count; i++) w[i] = src[i] == 0xff ? 0xffff : src[i];
                io = to; it = VK_INDEX_TYPE_UINT16;
            }
        }
        if (io % isz) { RED(4, "index buffer offset %#llx is not aligned", (unsigned long long)io); g_stat_skips[8]++; return 0; }
        if (replay) {
            replay->index = ib; replay->index_offset = io; replay->index_type = it;
            if (owned_geometry) {
                uint64_t bytes=(uint64_t)count*(it==VK_INDEX_TYPE_UINT8_EXT ? 1 : it==VK_INDEX_TYPE_UINT16 ? 2 : 4);
                if (di->itype==0 && !V.has_index_u8) {
                    if (frozen_u8) {
                        VkBuffer replay_index; VkDeviceSize replay_offset;
                        uint16_t *w=vk_transient((size_t)bytes,16,&replay_index,&replay_offset);
                        if (vk_replay_frozen_buffer_copy(frozen.buffer,frozen.offset,count,w)) {
                            /* Backwards expansion cannot overwrite unread raw bytes. */
                            const uint8_t *src=(const uint8_t *)w;
                            for (uint32_t i=count;i;i--) w[i-1]=src[i-1]==0xff ? 0xffff : src[i-1];
                            replay->index=replay_index; replay->index_offset=replay_offset;
                        } else {
                            vk_fg_terminal_input input={.role=VK_FG_TERMINAL_INPUT_INDEX,.stage=0,.stream=0,
                                .buffer=frozen.buffer,.offset=frozen.offset,.bytes=count,.freeze=1};
                            vk_fg_terminal_owned_input_refuse(&input,"frozen-index-copy-refused");
                        }
                    }
                } else if (fg_terminal_geometry_read(VK_FG_TERMINAL_INPUT_INDEX,0,di->ibuf,
                        (VkDescriptorBufferInfo){ib,io,bytes},&frozen)) {
                    replay->index=frozen.buffer; replay->index_offset=frozen.offset;
                }
            }
        }
        if (di->ind) {   /* DrawElementsIndirect (hle_nvn.c): the GPU reads {count, instances, first index, base vertex, base instance} */
            VkBuffer ab; uint64_t ao;
            if (!vk_resolve_for(di->ind, 20, &ab, &ao, NULL, VK_LOOKUP_VERTEX)) { RED(4, "indirect draw arguments are not in a memory pool"); g_stat_skips[8]++; return 0; }
            if (ao % 4) { RED(4, "indirect draw arguments at %#llx are not 4-byte aligned", (unsigned long long)ao); g_stat_skips[8]++; return 0; }
            if (__builtin_expect(g_pt_on, 0)) { VkDescriptorBufferInfo l; if (vk_pt_read(di->ind, 20, 4, VK_LAG_OTHER, &l)) { ab = l.buffer; ao = l.offset; } }   /* oot-a122 */
            VK_INFLIGHT(VK_INFLIGHT_INDIRECT, di->ind, 20, ab);
            DC_IB_FORGET();
            VK_REC_DO(VK_REC_CMD, vkCmdBindIndexBuffer(cb, ib, io, it); vkCmdDrawIndexedIndirect(cb, ab, ao, 1, 20));
        } else {
            uint32_t dc_first = 0;   /* oot-a108 TOTK_OHOS_DC_STATE bit 2: the buffer at offset 0, the offset as firstIndex */
            if (!DC_IB_REBASE(cb, ib, &io, it, &dc_first)) VK_REC_DO(VK_REC_CMD, vkCmdBindIndexBuffer(cb, ib, io, it));
            VK_REC_DO(VK_REC_CMD, vkCmdDrawIndexed(cb, count, inst, dc_first, di->base_vertex, base_inst));
        }
        totk_ft_count(FT_COUNT_DRAWS, 1);
    }
    return 1;
}
static void shader_xlate_first_command(vk_shader *sh, const char *kind) {
    if (!sh || !sh->xlate_miss_ns) return;
    LOGV("[shader-xlate] first-%s %s miss-to-command=%.3fms (recorded, not GPU completion)", kind, sh->key,
         (double)(host_now_ns() - sh->xlate_miss_ns) / 1e6);
    sh->xlate_miss_ns = 0;
}
/* TOTK_VK_SPLIT_PASS_DRAWS: end a long pass before its next draw so its recorded part reaches the GPU at the reopen
   (begin_rendering). Only at a draw on the same, still-open targets; never with a DONT_CARE store, UI/canvas passes,
   framegen extraction or pass diagnostics. Deferred merge barriers are emitted at this end like at a hazard split. */
static void split_pass_check(vk_shader *const sh[6], int canvas_transfer) {
    uint32_t n = R.split_draws;   /* taken at this pass (part)'s open */
    if (!n || !R.open || R.split_refused || g_stat_draws - R.draws0 < n) return;
    if (vk_frame_chunk_limit() <= 0 || !vk_submit_chunk_possible()) return;   /* nothing would be submitted */
    if (mesh_run_pending()) return;   /* not between a run's kernels and its draws: the first draw after the run splits (oot-a103) */
    if (R.store_discard || R.native || R.canvas || canvas_transfer || sh[0]->canvas_ui || vk_fg_extract_active ||
        FGX.writer.open || FGX.authority_span.open || PD.active || probe_on() || serial_draws()) {
        R.split_refused = 1; g_split_refused++; return;
    }
    if (split_lrz_safe()) {
        if (g_split_unrepairable || (R.dtex && !split_depth_touchable(R.dtex))) {
            R.split_refused = 1; g_split_refused++; g_split_lrz_refused++; return;
        }
        int color_write = 0;
        uint32_t outputs = sh[1] ? sh[1]->fout_mask | sh[1]->fout_span : 0;
        for (unsigned i = 0; i < R.ncolor; i++)
            color_write |= R.ctex[i] && (outputs >> i & 1) && (S.chmask.mask >> (4 * i) & 15);
        if (S.raster_discard || S.render_disabled || (R.dtex && !color_write)) {
            /* Defer this candidate, not the whole scene pass: a depth prepass can become a color-writing region. */
            g_split_refused++; g_split_lrz_refused++; return;
        }
    }
    vk_end_rendering_at(VK_PASS_END_SPLIT, n);
    g_split_submit = 1; g_split_cut = 1; g_split_count++;
}
#if defined(TOTK_GAME_ZA) || defined(TOTK_GAME_OOT)
const void *g_za_lastbind[4]; int g_za_lastmask[4]; unsigned g_za_bindn;
/* Z-A's UI is nn::ui2d, as Tears of the Kingdom's: its pane vertex shaders (the menu's ce7eacf8, c4072c2b, 22cc0011,
   d4734e0e, 47a9db5b, read in their translation) take a corner of the unit quad (one float2 attribute), place it by
   c3[33] (size .xy, offset .zw, y down), move it by the pane's rows c3[4..6] and project it by c3[0..3]; c3 is NVN
   uniform binding 0 of the vertex stage (their reflection: cbuf reg 0), the same formula as the Mac backend's
   ui2d_quad_covers_screen. Sets the quad's NDC x range in x[0..1] and returns which sides of the game's picture it
   reaches: 1 both (the whole width: a menu's backdrop, a dimming layer, a transition's curtain, a bar along the top or
   bottom), 2 the left only, 3 the right only (the menu's list panel -1..-0.2; the title's background pieces -1.09..0.91,
   -1.92..0.75, -0.16..2.48), 0 neither (widgets, the menu's panel, a speech bubble); -1 when this draw is not such a
   quad or its data cannot be read here. The text (c7ee515f: instanced, its own vertex positions) is never one. */
static int za_ui_side(const p_draw *dw, const p_draw_idx *di, const vk_shader *vs, float x[2]) {
    uint32_t m = vs->vin_mask; if (!m || (m & (m - 1))) return -1;
    unsigned a = (unsigned)__builtin_ctz(m);
    if (a >= (unsigned)S.nattr || vk_vertex_fmt(S.attr[a].fmt) != VK_FORMAT_R32G32_SFLOAT) return -1;
    unsigned st = S.attr[a].stream;
    if (st >= 16 || S.stream[st].divisor || (dw ? dw->instances : di->instances) > 1 || (di && (di->ind || di->itype < 0 || di->itype > 2))) return -1;
    const p_bind_buf *u = &S.ubo[0][0]; VkBuffer b; uint64_t o, avail;
    float c[34][4];
    if (!u->addr || u->size < sizeof c || !vk_resolve_span(u->addr, &b, &o, &avail) || avail < sizeof c) return -1;
    vk_ub_read(u->addr, c, sizeof c);
    float x0 = 9, x1 = -9;
    for (unsigned i = 0; i < 6; i++) {
        int64_t vertex = dw ? (int64_t)dw->first + i : 0;
        if (di) {
            unsigned bytes = 1u << di->itype; uint32_t index = 0; uint64_t at = di->ibuf + (uint64_t)i * bytes;
            if (!di->ibuf || !vk_resolve_span(at, &b, &o, &avail) || avail < bytes) return -1;
            vk_ub_read(at, &index, bytes); vertex = (int64_t)index + di->base_vertex;
        }
        if (vertex < 0 || vertex > 0xffff) return -1;
        uint64_t off = (uint64_t)vertex * S.stream[st].stride + S.attr[a].offset;
        const NVNbufferRange *vb = &S.vbuf[st]; float v[2];
        if (!vb->address || off + 8 > vb->size || !vk_resolve_span(vb->address + off, &b, &o, &avail) || avail < 8) return -1;
        vk_ub_read(vb->address + off, v, 8);
        if ((v[0] != 0 && v[0] != 1) || (v[1] != 0 && v[1] != 1)) return -1;   /* not the unit quad: not a pane */
        float px = v[0] * c[33][0] + c[33][2], py = -v[1] * c[33][1] + c[33][3], w[3], q[4];
        for (int r = 0; r < 3; r++) w[r] = c[4 + r][0] * px + c[4 + r][1] * py + c[4 + r][3];
        for (int r = 0; r < 4; r++) q[r] = c[r][0] * w[0] + c[r][1] * w[1] + c[r][2] * w[2] + c[r][3];
        if (!(fabsf(q[3]) > 1e-6f) || !isfinite(q[0] / q[3])) return -1;
        x0 = fminf(x0, q[0] / q[3]); x1 = fmaxf(x1, q[0] / q[3]);
    }
    x[0] = x0; x[1] = x1;
    return x0 <= -0.99f ? (x1 >= 0.99f ? 1 : 2) : x1 >= 0.99f ? 3 : 0;
}
#endif
static void draw_inner(const p_draw *dw, const p_draw_idx *di) {
    g_load_override = 0;
    int prim = dw ? dw->prim : di->prim;
    if (!S.prog) { g_stat_skips[0]++;
#if defined(TOTK_GAME_ZA) || defined(TOTK_GAME_OOT)
        { static int said; extern const void *g_za_lastbind[4]; extern int g_za_lastmask[4]; extern unsigned g_za_bindn; unsigned k = g_za_bindn;
          if (said++ < 6) LOGV("no program: draw count %d inst %d prim %d raster discard %d tf %d (cprog %p); last binds %p/%#x %p/%#x %p/%#x (%u binds)", dw ? dw->count : di->count, dw ? dw->instances : di->instances, prim, S.raster_discard, S.tf_on, (const void *)S.cprog,
              g_za_lastbind[(k + 3) % 4], g_za_lastmask[(k + 3) % 4], g_za_lastbind[(k + 2) % 4], g_za_lastmask[(k + 2) % 4], g_za_lastbind[(k + 1) % 4], g_za_lastmask[(k + 1) % 4], k); }
#endif
        return; }
#if defined(TOTK_GAME_ZA) || defined(TOTK_GAME_OOT)
    if (S.render_disabled || (S.raster_discard && !g_tf_vs)) { g_stat_skips[1]++; pd_draw_note("      draw skipped: %s", S.render_disabled ? "conditional rendering off" : "rasterizer discard"); return; }
#else
    if (S.render_disabled || S.raster_discard) { g_stat_skips[1]++; pd_draw_note("      draw skipped: %s", S.render_disabled ? "conditional rendering off" : "rasterizer discard"); return; }
#endif
#if defined(TOTK_GAME_ZA) || defined(TOTK_GAME_OOT)
    if (S.poly.cull == NVN_FACE_FRONT_AND_BACK && !g_tf_vs) { g_stat_skips[2]++; pd_draw_note("      draw skipped: cull front and back"); return; }
#else
    if (S.poly.cull == NVN_FACE_FRONT_AND_BACK) { g_stat_skips[2]++; pd_draw_note("      draw skipped: cull front and back"); return; }
#endif
    if (prim == 2 || prim >= 8) { RED(4, "primitive type %d not drawn", prim); g_stat_skips[3]++; return; }
    if (di && prim == 7) { RED(4, "indexed quads not drawn"); g_stat_skips[3]++; return; }
    uint32_t count = dw ? (uint32_t)dw->count : (uint32_t)di->count; if (!count) return;
    if (S.color.alpha_func && S.color.alpha_func!=NVN_ALPHA_FUNC_ALWAYS) {
        g_alpha_enabled++;
        if (!totk_live_cached(&g_alpha_live,"TOTK_VK_ALPHA_TEST",g_alpha_launch)) g_alpha_ignored++;
    }
    vk_shader *sh[6];
    int unresolved = VK_REC_VALUE(VK_REC_PROG, vk_record_enabled() ? vk_prog_resolve_record(S.prog, sh) : vk_prog_resolve(S.prog, sh));
    if (g_mesh.on && !unresolved) { sh[0] = g_mesh.vs; sh[3] = sh[4] = NULL; }   /* a task / mesh draw: its pass-through vertex shader (mesh_draw) */
    if (unresolved || !sh[0]) { g_stat_skips[4]++; return; }
#if defined(TOTK_GAME_ZA) || defined(TOTK_GAME_OOT)
    {   /* TOTK_OHOS_SKIP_FS (oot-a107, a measurement switch): draws of the listed pixel shaders are not drawn */
        static int n = -1; static char pre[8][24]; static uint64_t skipped;
        if (n < 0) { n = 0; const char *e = getenv("TOTK_OHOS_SKIP_FS");
            for (const char *q = e; q && *q && n < 8; ) { const char *c = strchr(q, ','); size_t l = c ? (size_t)(c - q) : strlen(q);
                if (l && l < sizeof pre[0]) { memcpy(pre[n], q, l); pre[n][l] = 0; n++; } q = c ? c + 1 : NULL; }
            if (n) LOGV("TOTK_OHOS_SKIP_FS: draws of %d pixel shader key prefix(es) are skipped (a measurement)", n); }
        if (n && sh[1] && !S.fs_unbound) for (int i = 0; i < n; i++) if (!strncmp(sh[1]->key, pre[i], strlen(pre[i]))) {
            if (++skipped % 3000 == 1) LOGV("TOTK_OHOS_SKIP_FS: %llu draws skipped so far", (unsigned long long)skipped);
            return; }
    }
    if (S.fs_unbound) sh[1] = NULL;
    if (g_tf_vs) { sh[0] = g_tf_vs; sh[1] = g_tf_vs->tf_fs; }
#endif
#if defined(TOTK_GAME_ZA) || defined(TOTK_GAME_OOT)
    /* ZA_UBO_DIAG=<fragment shader key prefix>,<slot>,<vec4 index>[,<count>]: the guest bytes of that fragment uniform slot
       at that vec4 when such a draw is replayed (every 60th frame), to see what a shader reads (e.g. an exposure). */
    { static int on = -1; static char pre[24]; static int slot, idx, cnt;
      if (on < 0) { const char *e = getenv("ZA_UBO_DIAG"); on = 0;
          if (e && *e) { const char *c = strchr(e, ','); size_t n = c ? (size_t)(c - e) : strlen(e); if (n >= sizeof pre) n = sizeof pre - 1;
              memcpy(pre, e, n); pre[n] = 0; cnt = 4; if (c && sscanf(c + 1, "%d,%d,%d", &slot, &idx, &cnt) >= 2) on = 1; } }
      if (on && sh[1] && !strncmp(sh[1]->key, pre, strlen(pre)) && slot >= 0 && slot < 16) {
          static uint64_t last; extern uint64_t vk_present_count(void) __attribute__((weak));
          uint64_t f = vk_present_count ? vk_present_count() : 0;
          if (f / 60 != last / 60 || !last) { last = f ? f : 1;
              const p_bind_buf *u = &S.ubo[1][slot]; char line[1024]; int n = snprintf(line, sizeof line, "[ubo-diag] fs %.16s c%d@%#llx size %llu:", sh[1]->key, slot, (unsigned long long)u->addr, (unsigned long long)u->size);
              for (int k = 0; k < cnt && k < 12 && u->addr && (uint64_t)(idx + k + 1) * 16 <= u->size; k++) { const float *v = (const float *)(uintptr_t)(u->addr + (uint64_t)(idx + k) * 16);
                  n += snprintf(line + n, sizeof line - (size_t)n, " [%d]=(%.9g %.9g %.9g %.9g)", idx + k, v[0], v[1], v[2], v[3]); }
              LOGV("%s", line); } } }
#endif
    if (sh[1] && !sh[0]->tf && S.color.alpha_func && S.color.alpha_func!=NVN_ALPHA_FUNC_ALWAYS &&
        totk_live_cached(&g_alpha_live,"TOTK_VK_ALPHA_TEST",g_alpha_launch)) {
        sh[1]=vk_shader_alpha(sh[1],S.color.alpha_func);
        if (!sh[1]) { g_stat_skips[4]++; return; }
    }
#ifdef TOTK_OHOS
    if (sh[1] && !sh[0]->tf) sh[1] = vk_shader_depth_tests(sh[1], S.ds.dt && R.dfmt != VK_FORMAT_UNDEFINED, S.ds.dw, S.ds.st, S.ms.a2c & 1);   /* oot-a93: early depth tests where the results are the same */
#ifdef TOTK_GAME_OOT
    if (sh[1] && !sh[0]->tf && dc_scope())   /* oot-a106 (timing only): the fragment twin of a draw-cost switch; the shader itself when none is on */
        sh[1] = vk_shader_dc(sh[1], (R.dfmt != VK_FORMAT_UNDEFINED && S.ds.dt && S.ds.dw) || (S.ds.st && (S.st_wmask[0] || S.st_wmask[1])));
#endif
    if (sh[1] && !sh[0]->tf) sh[0] = vk_shader_prune(sh[0], sh[1]);   /* oot-a108 (TOTK_OHOS_PRUNE_VARYINGS): without the output stores sh[1] does not load; sh[0] itself when off */
#endif
    vk_canvas_image transfer_source,transfer_window; int canvas_transfer=0;
    if (vk_canvas_on && (vk_native_ui_enabled() ||
        ((sh[0]->canvas_ui == 2 || (sh[1] && sh[1]->canvas_transfer)) &&
         (!R.open || !vk_canvas_current()->ui_committed || (sh[1] && sh[1]->canvas_transfer)))))
        canvas_transfer=canvas_draw_prepare(sh,&transfer_source,&transfer_window);
    int merge_hazard = pass_merge_on() ? pass_merge_draw_hazard(sh) : 0;
    if (R.open && R.merge_pending && merge_hazard) {
        uint32_t pending = R.merge_pending; g_merge_hazards++;
        vk_end_rendering_at(VK_PASS_END_MERGE_HAZARD, pending);
    }
    feedback_usage feedback={0}; int feedback_known=0;
    if (R.open && R.feedback_on) {
        feedback=feedback_draw_usage(sh); feedback_known=1;
        if (R.feedback_pending && (feedback.unknown ||
            (feedback.read&(R.feedback_written|feedback.written)) || (feedback.written&R.feedback_sampled))) {
            R.feedback_restore=!R.feedback_armed && R.feedback_continued!=0;
            g_feedback_hazards++;
#ifdef TOTK_OHOS
            {   uint32_t wr=R.feedback_written|feedback.written;
                g_be_n[feedback.unknown ? 7 : ((feedback.read&~feedback.pool)&wr) ? 5 : (feedback.pool&wr) ? 6 : 7]++; }
#endif
            vk_end_rendering_at(VK_PASS_END_TEXTURE_BARRIER,R.feedback_pending);
        }
    }
#ifdef TOTK_OHOS
    if (pass_barrier_narrow_on()) pass_barrier_draw(sh);
#endif
    split_pass_check(sh, canvas_transfer);
    VK_REC_DO(VK_REC_S_TOUCH, draw_touch(sh, dw, di));
    g_load_draw = dw; g_load_indexed = di; g_load_sh = sh; g_load_pipeline = VK_NULL_HANDLE;
    int opened = begin_rendering(1);
    g_load_sh = NULL; g_load_draw = NULL; g_load_indexed = NULL;
    if (!opened) { g_stat_skips[5]++; return; }
#if defined(TOTK_OHOS) && defined(TOTK_GAME_OOT)
    /* oot-a106 (timing only): three indices of an ordinary draw (not a mesh draw's or an indirect draw's count, not quads) */
    int dc_one_inst = 0;   /* (oot-a107) TRI keeps one instance too, and leaves transform-feedback captures alone */
    if ((g_dc_init || (dc_init(), 1)) && g_dc_tri && count > 3 && prim != 7 && !g_mesh.on && !g_tf_vs && !(di && di->ind) && dc_scope()) { dc_note(0, count); count = 3; dc_one_inst = 1; }
#endif
    if (R.feedback_on && !feedback_known) feedback=feedback_draw_usage(sh);
    p_draw certified;
    if (g_load_override) {
        certified = (p_draw){prim,0,(int32_t)count,dw ? dw->base_instance : di->base_instance,
            dw ? dw->instances : di->instances};
        dw = &certified; di = NULL; /* certificates exclude VertexIndex/InstanceIndex */
    }
    if (S.has_sc && !sh[0]->tf) { int32_t x1 = MINV((int32_t)R.w, S.sc.x + S.sc.w), y1 = MINV((int32_t)R.h, S.sc.y + S.sc.h); if (x1 <= MAXV(0, S.sc.x) || y1 <= MAXV(0, S.sc.y)) { g_stat_skips[6]++; pd_draw_note("      draw skipped: empty scissor"); return; } }
    VkPipelineLayout layout; VkPipeline p;
    static struct { vk_shader *vs, *fs; VkPipelineLayout layout; } last_layout;
    VK_REC_DO(VK_REC_PIPE,
        if (g_load_pipeline) {
            layout = g_load_layout; p = g_load_pipeline;
        } else {
            if (!last_layout.layout || last_layout.vs != sh[0] || last_layout.fs != sh[1]) {
                last_layout.layout = vk_layout_of(sh[0], sh[1]); last_layout.vs = sh[0]; last_layout.fs = sh[1];
            }
            layout = last_layout.layout;
            p = pipe_get(sh[0], sh[1], prim, layout);
        });
    if (!p) { g_stat_skips[10]++; pd_draw_note("      draw skipped: pipeline compiling (vs %.16s fs %.16s)", sh[0]->key, sh[1] ? sh[1]->key : "-"); return; }
    if (p == PIPE_FAILED) { g_stat_skips[7]++; pd_draw_note("      draw skipped: no pipeline (vs %.16s fs %.16s)", sh[0]->key, sh[1] ? sh[1]->key : "-"); return; }
    if (vk_fg_extract_active && (FGX.guard.first || FGX.guard.changed ||
        FGX.guard.program != (uintptr_t)sh[0] || FGX.guard.fragment != (uintptr_t)sh[1]))
        (void)fg_pass_route_programs(1,&FGX.passes,&FGX.guard,(uintptr_t)sh[0],(uintptr_t)sh[1],fg_extract_draw_context,sh);
    if (!fg_extract_authority_before_draw(sh)) { g_stat_skips[5]++; return; }
    if (!fg_extract_depth_spans_before_draw(sh)) { g_stat_skips[5]++; return; }
    if (vk_fg_extract_active) { FGX.record = NULL; FGX.draw_kind = 0; }
    if (vk_fg_extract_active && FGX.captured && !FGX.invalid) {
        if (fg_extract_canvas(R.ctex[0],R.clevel[0],R.clayer[0]) && vk_canvas_ui_shader(sh[0]->key,1)) FGX.draw_kind = 1;
        else if (canvas_transfer || (sh[1] && vk_canvas_transfer_shader(sh[1]->key) && vk_tex_canvas_window(R.ctex[0])))
            FGX.draw_kind = 2;
        if (FGX.draw_kind && vk_fg_canvas_view()) {
            if (R.ncolor != 1 || R.samples != VK_SAMPLE_COUNT_1_BIT ||
                (FGX.draw_kind == 1 && !fg_extract_ui_depth(R.dtex,R.dlevel,R.dlayer)) ||
                (FGX.draw_kind == 2 && R.dtex) || sh[0]->has_storage_writes || (sh[1] && sh[1]->has_storage_writes))
                fg_extract_reject(VK_FG_STRUCTURE_CHANGED,"UI/transfer replay attachment or shader structure changed");
            else if (FGX.nreplay == sizeof FGX.replay_draw / sizeof FGX.replay_draw[0])
                fg_extract_reject(VK_FG_TRACK_CAPACITY,"UI replay packet capacity exhausted");
            else {
                vk_post_output target;
                if (!fg_extract_replay_target(&target)) {
                    fg_extract_reject(VK_FG_STRUCTURE_CHANGED,"UI/transfer replay attachment geometry unavailable");
                } else {
                    FGX.record = &FGX.replay_draw[FGX.nreplay];
                    memset(FGX.record, 0, sizeof *FGX.record);
                    FGX.record->draw.pipeline = p; FGX.record->draw.layout = layout;
                    FGX.record->width = target.w; FGX.record->height = target.h;
                    FGX.record->format = target.format; FGX.record->image = target.image;
                    FGX.record->native = R.native; FGX.record->final = FGX.draw_kind == 2;
                    if (FGX.draw_kind==2 &&
                            atomic_load_explicit(&vk_fg_terminal_owned_active,memory_order_relaxed))
                        (void)vk_fg_terminal_owned_begin();
                }
            }
        }
    }
    VkCommandBuffer cb = vk_cb();
    if (pd_active()) {
        float c25[4] = {0}; const p_bind_buf *u8 = &S.ubo[0][8]; if (u8->addr && u8->size >= 26 * 16) vk_ub_read(u8->addr + 25 * 16, c25, 16);
        pd_draw_note("      draw: vs %.16s fs %.16s prim %d count %u inst %u first %u%s; depth test %d func %d write %d; stencil %d func %d ref %#x mask %#x; colour mask %#x blend %#x; scissor %s %d,%d %dx%d; vs c11[25] %g %g %g %g",
            sh[0]->key, sh[1] ? sh[1]->key : "-", prim, count, dw ? (uint32_t)dw->instances : (uint32_t)di->instances, dw ? (uint32_t)dw->first : (uint32_t)di->base_vertex, di ? " indexed" : "",
            S.ds.dt, S.ds.df, S.ds.dw, S.ds.st, S.ds.sf_f, S.st_ref[0], S.st_vmask[0], S.chmask.mask, S.color.blend_en, S.has_sc ? "on" : "off", S.sc.x, S.sc.y, S.sc.w, S.sc.h,
            (double)c25[0], (double)c25[1], (double)c25[2], (double)c25[3]);
        /* The driver's "Depth write + blending" class also includes masks/missing targets/logic ops. Keep raw NVN
           state beside effective Vulkan state so a dump can distinguish translation artifacts from real requests. */
        if (S.ds.dt && S.ds.dw && R.dfmt != VK_FORMAT_UNDEFINED) {
            uint32_t be = pipeline_blend_enable();
            for (uint32_t i = 0; i < R.ncolor && i < 8; i++) if (!R.cfmt[i]) be &= ~(1u << i); /* pipe_create skips holes */
            pd_draw_note("      lrz: pipeline %#llx NVN test/write %d/%d -> Vulkan test/write 1/1 compare %u; blend raw/effective %#x/%#x logic %d a2c %u; FS NVN-early %d depth %d outputs %#x; c%u depthfmt %u formats %u,%u,%u,%u,%u,%u,%u,%u",
                (unsigned long long)(uintptr_t)p, S.ds.dt, S.ds.dw, (unsigned)vk_compare(S.ds.df), S.color.blend_en, be, S.color.logic_op, (unsigned)(S.ms.a2c & 1),
                sh[1] ? sh[1]->early_tests : 0, sh[1] ? sh[1]->writes_depth : 0, sh[1] ? sh[1]->fout_mask : 0, R.ncolor, (unsigned)R.dfmt,
                (unsigned)R.cfmt[0], (unsigned)R.cfmt[1], (unsigned)R.cfmt[2], (unsigned)R.cfmt[3], (unsigned)R.cfmt[4], (unsigned)R.cfmt[5], (unsigned)R.cfmt[6], (unsigned)R.cfmt[7]);
            for (uint32_t i = 0; i < R.ncolor && i < 8; i++) {
                const blend_st *b = &S.blend[i];
                pd_draw_note("        lrz c%u fmt %u mask %#x blend %u RGB %d/%d eq %d alpha %d/%d eq %d",
                    i, (unsigned)R.cfmt[i], R.cfmt[i] ? (unsigned)(S.chmask.mask >> (4 * i) & 15) : 0u, (unsigned)(be >> i & 1),
                    b->src_rgb, b->dst_rgb, b->eq_rgb, b->src_a, b->dst_a, b->eq_a);
            }
        }
    }
#if defined(TOTK_OHOS) && defined(TOTK_GAME_OOT)
    if (__builtin_expect(g_dc2_pipe != 0, 0) && !R.secondary) p = dc_pipe_pick(p, layout, sh, prim);   /* oot-a108 (timing only): the bound pipeline instead */
#endif
    if (R.secondary) g_record_draw = vk_record_draw_begin(p, layout);
    else { VK_REC_DO(VK_REC_CMD, if (p != g_bound_pipe) { vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p); g_bound_pipe = p; }); }
#if defined(TOTK_OHOS) && defined(TOTK_GAME_OOT)
    if (__builtin_expect(g_dc2_pipe > 0, 0) && !R.secondary) dc_pipe_after(p, layout, sh, prim);
#endif
    { static const char *vt = (const char *)1; if (vt == (const char *)1) vt = getenv("TOTK_VK_PASS_DUMP_VSTEX");
      g_pd_cap = PD.active && !g_pd_vstex_done && vt && *vt && !strncmp(sh[0]->key, vt, strlen(vt)); }
    g_ui_aspect_on = 0;
#if defined(TOTK_GAME_ZA) || defined(TOTK_GAME_OOT)
    /* Z-A draws its UI straight into the window texture after the scene's copy into it (pass dump of the menu: one pass of
       6-index quads with a dozen UI vertex shaders, the text instanced; the copy is a 3-vertex triangle). So every 6-vertex
       or 6-index draw into a window texture is the UI, scaled about the centre of the part the window shows (the game's
       crop, 0,0 1280x720 handheld in its 1920x1080 textures; hle_nvn.c totk_window_crop) instead of the TOTK canvas rule.
       On a wider screen the panes that reach a side of the picture (za_ui_side) reach the screen's side too: one across the
       whole width (a backdrop, a dimming layer, a curtain) is not squeezed; one from a side, at least 0.35 wide (a list
       panel, a background piece), keeps its outer side at the screen's edge and its inner side where the squeeze puts it,
       as the Mac backend's edge +-2 does. Narrower strips at a side stay squeezed with the pieces next to them. */
    if (vk_canvas_on && count == 6 && R.ncolor && R.ctex[0] && vk_tex_canvas_window(R.ctex[0])) {
        float shown = pres_shown_aspect(); double f = vk_ui_aspect_factor(shown);
        int side = 0; float x[2] = {0, 0};
        if (shown > 0 && f > 1) side = za_ui_side(dw, di, sh[0], x);
        if (shown > 0 && f != 1 && side != 1) {
            extern int totk_window_crop[4];
            int c[4]; memcpy(c, totk_window_crop, sizeof c);
            vk_canvas_image target = vk_tex_canvas_identity(R.ctex[0], R.clevel[0], R.clayer[0]);
            float x0 = 0, y0 = 0, w = (float)target.width, h = (float)target.height;
            if (c[2] > 0 && c[3] > 0) { x0 = (float)c[0]; y0 = (float)c[1]; w = (float)c[2]; h = (float)c[3]; }
            g_ui_aspect = (vk_ui_aspect){f > 1 ? (float)(1 / f) : 1, f > 1 ? 1 : (float)f, x0 + w * 0.5f, y0 + h * 0.5f};
            if (side >= 2 && x[1] - x[0] >= 0.35f && S.has_vp) {   /* x -> ax + (x - ax) * k: the outer side stays */
                float X0 = S.vp[0][0] + (x[0] + 1) * 0.5f * S.vp[0][2], X1 = S.vp[0][0] + (x[1] + 1) * 0.5f * S.vp[0][2], cx = g_ui_aspect.cx, sx = g_ui_aspect.sx;
                if (X1 - X0 > 1) {
                    g_ui_aspect.sx = side == 2 ? (cx + (X1 - cx) * sx - X0) / (X1 - X0) : (X1 - cx - (X0 - cx) * sx) / (X1 - X0);
                    g_ui_aspect.cx = side == 2 ? X0 : X1;
                }
            }
            g_ui_aspect_on = vk_ui_aspect_on(&g_ui_aspect);
        }
        if (PD.active && side >= 0 && shown > 0 && f > 1) {
            char l[112]; int n = snprintf(l, sizeof l, "    ui2d %.8s x %.3f..%.3f side %d: scale %.3f about %.1f\n", sh[0]->key, (double)x[0], (double)x[1], side,
                                          g_ui_aspect_on ? (double)g_ui_aspect.sx : 1.0, g_ui_aspect_on ? (double)g_ui_aspect.cx : 0.0);
            pd_note(l, n < (int)sizeof l ? n : (int)sizeof l - 1);
        }
    }
#else
    if (vk_canvas_on && sh[0]->canvas_ui && count == 6 && R.ncolor && R.ctex[0]) {   /* vk_ui_aspect.h: a HUD quad */
        float shown = pres_shown_aspect();
        if (shown > 0) {
            vk_canvas_image target = vk_tex_canvas_identity(R.ctex[0], R.clevel[0], R.clayer[0]);
            g_ui_aspect = vk_ui_aspect_draw(shown, &target, sh[0]->canvas_ui, count);
            g_ui_aspect_on = vk_ui_aspect_on(&g_ui_aspect);
        }
    }
#endif
    VK_REC_DO(VK_REC_CMD, dynamic_state(cb, prim));
    VK_REC_DO(VK_REC_BIND,
        if (__builtin_expect(vk_xlate_stats_enabled, 0)) {
            xlate_begin(); g_xlate->draws++; xlate_buffers(sh[0], di);
            for (int stage = 0; stage < 2; stage++) if (sh[stage])
                bind_stage_impl(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, (uint32_t)stage, sh[stage], stage, NULL, 1);
            xlate_end();
        } else {
            bind_stage(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, sh[0], 0, NULL);
            if (sh[1]) bind_stage(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 1, sh[1], 1, NULL);
        });
    if (!R.open) { RED(4, "the render pass ended while a draw was being set up: draw skipped"); g_stat_skips[5]++; return; }
    if (probe_on() && g_npdraw < 4) {   /* pass probe: what each of the pass's first draws was */
        probe_draw *q = &g_pdraw[g_npdraw++]; memset(q, 0, sizeof *q);
        snprintf(q->vs, sizeof q->vs, "%.16s", sh[0]->key); q->prim = prim; q->count = dw ? (uint32_t)dw->count : (uint32_t)di->count; q->first = dw ? (uint32_t)dw->first : (uint32_t)di->base_vertex;
        q->inst = dw ? (uint32_t)dw->instances : (uint32_t)di->instances;
        if (S.has_vp) memcpy(q->vp, S.vp[0], 16); memcpy(q->dr, S.dr, 8); q->bias[0] = S.po.factor; q->bias[1] = S.po.units; q->bias[2] = (S.poly.offset_en & 4) ? 1.f : 0.f;
        q->clamp = S.depth_clamp; q->cull = (int)S.poly.cull; q->has_sc = S.has_sc; if (S.has_sc) { q->sc[0] = S.sc.x; q->sc[1] = S.sc.y; q->sc[2] = S.sc.w; q->sc[3] = S.sc.h; }
        /* the words the class-shading vertex shaders (345bb4abe6b6bbf4 / 97c2cb79ae8c75a5) turn into depth: vp_c11[25].x
           (the class, uniform slot 8) and vp_c4[147].z (slot 1); depth = k + (1 - k) * nibble_swap(class) / 255 */
        { const p_bind_buf *u8 = &S.ubo[0][8], *u1 = &S.ubo[0][1];
          if (u8->addr && u8->size >= 26 * 16) vk_ub_read(u8->addr + 25 * 16, q->c11_25, 16);
          if (u1->addr && u1->size >= 148 * 16) vk_ub_read(u1->addr + 147 * 16, q->c4_147, 16);
          if (u1->addr && u1->size >= 8 * 16) vk_ub_read(u1->addr + 7 * 16, q->c4_7, 16); }
        int a0 = -1; for (int a = 0; a < 16; a++) if ((sh[0]->vin_mask >> a & 1) && a < S.nattr && S.attr[a].fmt) { a0 = a; break; }
        if (a0 >= 0) { uint32_t st = S.attr[a0].stream & 15; uint64_t va = S.vbuf[st].address; VkBuffer bb; uint64_t oo, av;
            if (va && vk_resolve_span(va, &bb, &oo, &av) && av >= 32) memcpy(q->v0, (const void *)(uintptr_t)(va + (uint64_t)q->first * S.stream[st].stride), 32); }
    }
    uint32_t inst = dw ? (dw->instances > 1 ? (uint32_t)dw->instances : 1) : (di->instances > 1 ? (uint32_t)di->instances : 1);
    if (dc_one_inst) inst = 1;
    uint32_t base_inst = dw ? (uint32_t)dw->base_instance : (uint32_t)di->base_instance;
    if (!VK_REC_VALUE(VK_REC_VERTEX, draw_buffers(cb, sh[0], dw, di, prim, count, inst, base_inst))) return;
    if (g_record_draw) { vk_record_draw_commit(); g_record_draw = NULL; }
    overlay_scene_draw();

    if (vk_canvas_on) {
        if (canvas_transfer) vk_canvas_transfer(&transfer_source,&transfer_window,1);
        if (R.ctex[0] && sh[0]->canvas_ui && !vk_canvas_current()->ui_committed) {
            vk_canvas_image target=vk_tex_canvas_identity(R.ctex[0],R.clevel[0],R.clayer[0]);
            vk_canvas_ui_commit(&target);
        }
    }
    g_stat_draws++; g_chunk_draws++; vk_note_draw();
    if (g_mesh.on) g_geo_mesh_cap += (uint64_t)count * inst; else g_geo_verts += (uint64_t)count * inst;
    if (__builtin_expect(PL.enabled, 0) && pso_life_active()) {
        for (unsigned i = 0; i < R.ncolor && i < 8; i++)
            if (sh[1] && ((S.chmask.mask >> (4 * i)) & 15u)) PL.color_draws[i]++;
        if (R.dtex && S.ds.dt && S.ds.dw) PL.depth_draws++;
        if (R.dtex && S.ds.st && (S.st_wmask[0] || S.st_wmask[1])) PL.stencil_draws++;
    }
    if (g_draw_fast) atomic_fetch_add(&g_library_stats.fast_draws, 1);
    if (vk_fg_extract_active) {
        if (FGX.draw_kind == 2) FGX.transfer = 1;
        if (FGX.record) { FGX.nreplay++; FGX.record = NULL; }
    }
    shader_xlate_first_command(sh[0], "draw"); shader_xlate_first_command(sh[1], "draw");
    if (pass_merge_on()) {
        R.merge_storage |= (uint32_t)merge_hazard;
        if (R.merge_armed) { R.merge_continued++; R.merge_armed = 0; }
    }
    if (R.feedback_on) {
        R.feedback_written |= feedback.written; R.feedback_sampled |= feedback.read;
        R.feedback_unknown |= feedback.unknown;
        if (R.feedback_armed) { R.feedback_continued++; R.feedback_armed=0; }
    }
    if (vk_gpuprof_on) vk_gpuprof_draw(sh);
    g_samples_cum += (uint64_t)count * inst;
    pd_draw(prim, count, inst, di ? di->itype + 1 : 0, sh);
    if (PP.open >= 0 && S.has_vp) { pp_rec *pr = &PP.rec[PP.open]; uint32_t vw = (uint32_t)fabsf(S.vp[0][2]), vh = (uint32_t)fabsf(S.vp[0][3]); if (vw > pr->vpw) pr->vpw = vw; if (vh > pr->vph) pr->vph = vh; }
    if (sh[1] && pd_split_match(sh[1]->key)) vk_end_rendering_at(VK_PASS_END_DIAGNOSTIC, 2);
}
#if defined(TOTK_GAME_ZA) || defined(TOTK_GAME_OOT)
vk_shader *vk_shader_tf(const prog_t *p, vk_shader *vs, uint32_t stride[4]);
static void tf_ssbo(int stage, int index, uint64_t addr, uint64_t size) {   /* as OP_BIND_SSBO */
    p_bind_buf *old = &S.ssbo[stage][index];
    if (vk_record_enabled() && (old->addr != addr || old->size != size)) g_record_bind_revision[stage]++;
    if (old->addr != addr || (uint32_t)old->size != (uint32_t)size) g_driver_revision[0][stage]++;
    old->stage = stage; old->index = index; old->addr = addr; old->size = size;
}
#endif
static void draw_inner(const p_draw *dw, const p_draw_idx *di);
static void draw_body(const p_draw *dw, const p_draw_idx *di);
#ifdef TOTK_OHOS
void vk_walker_keep(void);   /* nvn_vk.c: on the walker, QoS 5 and uclamp 1024/1024 again (ohos_sched_self; nothing within 10 ms of its last call) */
#endif
static void mesh_draw(const p_draw_idx *md);
static void draw(const p_draw *dw, const p_draw_idx *di) {
    if (di && di->prim == DM_PRIM_MESH_TASKS) { mesh_draw(di); return; }   /* nvnCommandBufferDrawMeshTasks[Indirect] */
#ifdef TOTK_OHOS
    /* The walker keeps itself on the Prime cores by asserting its scheduling attributes again and again (the system resets
       uclamp within tens of milliseconds). That was done at each submission: enough for Tears of the Kingdom (about 400 a
       second), not for Ocarina of Time at 13 submissions a frame of 180 ms: the walker read uclamp 0 / 1024 and ran 85% of
       its time on the little cores 0-3 (capacity 245 of 1024), at 52 us a draw. Asserted from the draws and dispatches too. */
    vk_walker_keep();
#endif
#if defined(TOTK_GAME_ZA) || defined(TOTK_GAME_OOT)
    /* Transform feedback: a draw while capture is on runs the capture variant of its vertex shader and that variant's
       fragment shader (vk_tf.inc), with the bound feedback buffers (from the offset captured so far) in fragment storage
       slots 12-15; the offsets then advance by the vertices drawn times the strides. Plain array draws only (as nxnative). */
    uint32_t tfs[4] = {0}; p_bind_buf saved[4]; int tf = 0;
    if (S.tf_on) { static int said; if (said++ < 8) LOGV("transform feedback: draw while on: prog %p dw %p count %d inst %d", (const void *)S.prog, (const void *)dw, dw ? dw->count : di->count, dw ? dw->instances : di->instances); }
    if (S.tf_on && S.prog) {
        vk_shader *sh[6];
        int rr = vk_prog_resolve(S.prog, sh);
        { static int said; if (said++ < 8) LOGV("transform feedback: resolve %d vs %s", rr, sh[0] ? sh[0]->key : "-"); }
        if (!rr && sh[0]) {
            vk_shader *t = vk_shader_tf(S.prog, sh[0], tfs);
            { static int said; if (said++ < 8) LOGV("transform feedback: capture variant %p strides %u %u %u %u", (void *)t, tfs[0], tfs[1], tfs[2], tfs[3]); }
            if (t && (!dw || dw->instances > 1)) { RED(4, "transform feedback: an indexed or instanced draw is not captured"); }
            else if (t) {
                tf = 1; g_tf_vs = t;
                for (int b = 0; b < 4; b++) if (tfs[b]) {
                    saved[b] = S.ssbo[1][12 + b];
                    uint64_t off = S.tf_off[b], size = S.tf[b].size > off ? S.tf[b].size - off : 0;
                    tf_ssbo(1, 12 + b, S.tf[b].address ? S.tf[b].address + off : 0, size);
                }
                static int said; if (said++ < 3) LOGV("transform feedback: capturing %d vertices (strides %u %u %u %u)", dw->count, tfs[0], tfs[1], tfs[2], tfs[3]);
            }
        }
    }
    /* The capture writes no attachment (vk_tf.inc): it runs in a pass without any. A game pass open with attachments ends
       here once, not around every capture: each capture used to load and store the whole 1920x1080 scene targets (87
       times a frame in the opening). TF End / Pause ends the attachment-less pass with its barrier, as before; the next
       game draw opens the game's targets again (ZA_TF_NOATT=0: captures in the game's pass, as before). */
    static int noatt = -1; if (noatt < 0) { const char *e = getenv("ZA_TF_NOATT"); noatt = !(e && *e == '0'); }
    int has_rt = S.has_rt;
    if (tf && noatt) {
        if (R.open && (R.ncolor || R.dtex)) vk_end_rendering_at(VK_PASS_END_NVN_BARRIER, ~0u);
        S.has_rt = 0;
    } else if (!tf && R.open && !R.ncolor && !R.dtex && S.has_rt && (S.rt.n || S.rt.depth)) {
        vk_end_rendering_at(VK_PASS_END_NVN_BARRIER, ~0u);   /* a capture pass still open: the game's targets again */
    }
    draw_body(dw, di);
    S.has_rt = has_rt;
    if (tf) {
        g_tf_vs = NULL;
        for (int b = 0; b < 4; b++) if (tfs[b]) { tf_ssbo(1, 12 + b, saved[b].addr, saved[b].size); S.tf_off[b] += (uint64_t)dw->count * tfs[b]; }
    }
}
static void draw_body(const p_draw *dw, const p_draw_idx *di) {
#endif
    if (__builtin_expect(vk_rec_profile_on, 0)) {
        uint64_t p0 = vk_rec_profile_draw_begin(), before = g_stat_draws;
        draw_inner(dw, di);
        vk_rec_profile_draw_end(p0, g_stat_draws != before);
    } else draw_inner(dw, di);
    if (g_record_draw) vk_record_draw_abort();
    if (vk_fg_extract_active) FGX.record = NULL;
    g_record_draw = NULL;
}
static void dispatch_bound(const p_dispatch *dp);
#if defined(TOTK_GAME_ZA) || defined(TOTK_GAME_OOT)
static void dispatch(const p_dispatch *dp) {   /* the compute program the stage mask bound (OP_BIND_PROGRAM), for this dispatch */
    const prog_t *g = S.prog;
    if (S.cprog) { S.prog = S.cprog; vk_stage_sig_invalidate(0x3fu); }
    dispatch_bound(dp);
    if (S.prog != g) { S.prog = g; vk_stage_sig_invalidate(0x3fu); g_pipe_dirty = 1; }
}
#else
static void dispatch(const p_dispatch *dp) { dispatch_bound(dp); }
#endif
static void dispatch_bound(const p_dispatch *dp) {
#ifdef TOTK_OHOS
    vk_walker_keep();   /* as draw() */
#endif
    if (!vk_lag_uniform_private()) vk_ub_flush();   /* private UBOs materialize only for actual non-UBO consumers */
    if (!S.prog) return;
    vk_shader *sh[6];
    if (vk_prog_resolve(S.prog, sh) || !sh[5]) { g_stat_skips[9]++; pd_dispatch_skip(NULL, "no compute shader: its variant is not in the seed"); return; }
#ifdef TOTK_GAME_OOT
    {   /* Ocarina of Time's movie enhancement (Movie/Ndve/Ndve-Nvn2-vid_seq5.dat: NDVE and its NDSR pack, 22 compute shaders
           of tensor (HMMA) emulation): Maleoon's compiler refuses the three largest (650 / 460 / 290 KB of SPIR-V), so the
           networks' output is black, and the rest held the title movie at ~3 fps. Bypassed: 49dff010 (NDVE@0x1efe90) is the
           decoded picture's YUV -> RGB step (Y fetched, UV sampled, the matrix in c[24]; 2x2 pixels a thread into its image
           slot 0, the full picture) and runs; the networks are skipped; the final NDSR upscaler (4f41ccf2, NDSR@0x104fe0)
           becomes a blit of that RGB picture into its output image (slot 1), which the game's own draw then shows.
           TOTK_VK_MOVIE_NDVE=1 runs the networks instead. */
        static const char ndve[][17] = {"0a56002cdc7e1fce","329e4b0f0b3f2bc6","3f18f89630c05813","539ad3b353758de0",
            "55ee6dc89a2b64e2","5a7d69b067a7f011","7653fe5f6ef0b2f2","7f7ff0f36b54d90f","8e98d86c0e4a7b53","9442573ee4ace011","a8b4fb48fee0ccf8",
            "ab9a92b55950ec02","b6e34b46dea44278","ba079f68ae288dba","c2fc2ddd604c25e7","c41f451b88a050f6","d15370a382630d4e","d72077c4b6cc264f",
            "e2eebd7ba18fe35f","e6be82d835fbe4d3"};
        static int run = -1; if (run < 0) { const char *e = getenv("TOTK_VK_MOVIE_NDVE"); run = e && *e == '1'; LOGV("movie enhancement networks (NDVE): %s (TOTK_VK_MOVIE_NDVE)", run ? "run" : "bypassed: the decoded picture is shown"); }
        static uint64_t rgb_handle;   /* the image 49dff010 last wrote: the decoded picture as RGB */
        static uint64_t n_rgb, n_blit, n_noblit, n_skip, report_ns;   /* the movie path every 5 s */
        { uint64_t t = host_now_ns(); if (!report_ns) report_ns = t; else if (t - report_ns > 5000000000ull) { LOGV("movie: in 5 s, YUV->RGB %llu, upscaler outputs replaced %llu (not %llu), networks skipped %llu", (unsigned long long)n_rgb, (unsigned long long)n_blit, (unsigned long long)n_noblit, (unsigned long long)n_skip); n_rgb = n_blit = n_noblit = n_skip = 0; report_ns = t; } }
        if (!run) {
            {   /* the order the movie paths use these shaders, once per key */
                static char seen[24][17]; static int nseen;
                int known = 0; for (int i = 0; i < nseen; i++) if (!strncmp(seen[i], sh[5]->key, 16)) { known = 1; break; }
                if (!known) for (size_t i = 0; i < sizeof ndve / sizeof *ndve + 1 && nseen < 24; i++) {
                    const char *k = i < sizeof ndve / sizeof *ndve ? ndve[i] : "49dff010b9832251";
                    if (!strncmp(sh[5]->key, k, 16) || (!strncmp(sh[5]->key, "4f41ccf24f163e0d", 16) && i == 0)) {
                        memcpy(seen[nseen++], sh[5]->key, 16); LOGV("movie: NDVE shader %.16s first used (%d so far)", sh[5]->key, nseen); break; }
                }
            }
            if (!strncmp(sh[5]->key, "49dff010b9832251", 16)) { rgb_handle = S.img[5][0]; n_rgb++; }
            else if (!strncmp(sh[5]->key, "4f41ccf24f163e0d", 16) || !strncmp(sh[5]->key, "c2fc2ddd604c25e7", 16)) {
                /* the upscalers' outputs: 4f41ccf2 (NDSR@0x104fe0) writes its image slot 1, c2fc2ddd (NDSR@0x104f00) slot 0 */
                uint64_t out = !strncmp(sh[5]->key, "c2fc2ddd604c25e7", 16) ? S.img[5][0] : S.img[5][1]; vk_treg rs, rd;
                if (rgb_handle && out && vk_treg_get((int)(rgb_handle & 0xfffff), &rs) && vk_treg_get((int)(out & 0xfffff), &rd)) {
                    vk_tex *src = vk_tex_get(&rs.t), *dst = vk_tex_get(&rd.t);
                    if (src && dst && src != dst && vk_tex_aspect(src) == VK_IMAGE_ASPECT_COLOR_BIT && vk_tex_aspect(dst) == VK_IMAGE_ASPECT_COLOR_BIT) {
                        vk_end_rendering_at(VK_PASS_END_COMPUTE, 0);
                        vk_tex_native_read(src); vk_tex_native_read(dst);
                        VkCommandBuffer cb = vk_cb(); vk_barrier_all(cb);
                        VkImageBlit r = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .srcOffsets = {{0, 0, 0}, {(int32_t)vk_tex_width(src, 0), (int32_t)vk_tex_height(src, 0), 1}},
                                         .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstOffsets = {{0, 0, 0}, {(int32_t)vk_tex_width(dst, 0), (int32_t)vk_tex_height(dst, 0), 1}}};
                        vkCmdBlitImage(cb, vk_tex_image(src), VK_IMAGE_LAYOUT_GENERAL, vk_tex_image(dst), VK_IMAGE_LAYOUT_GENERAL, 1, &r, VK_FILTER_LINEAR);
                        vk_barrier_all(cb);
                        vk_tex_mark_gpu_written(dst); n_blit++;
                        { static int said; if (!said++) LOGV("movie: NDVE bypassed, the decoded picture %ux%u blitted into the upscaler's output %ux%u", vk_tex_width(src, 0), vk_tex_height(src, 0), vk_tex_width(dst, 0), vk_tex_height(dst, 0)); }
                    }
                }
                n_noblit++; g_stat_skips[9]++; return;
            } else for (size_t i = 0; i < sizeof ndve / sizeof *ndve; i++) if (!strncmp(sh[5]->key, ndve[i], 16)) { n_skip++; g_stat_skips[9]++; return; }
        }
    }
#endif
#ifdef TOTK_OHOS
    {   /* agl's NDSR upscaler networks (agl_ndsr*.pck; Ocarina of Time's gameplay runs fa3033d05c0a41d6 of agl_ndsr_vr_light.pck
           each frame: the scene, anti-aliased, to the output size). They are Ampere tensor (HMMA) programs the translator lowers
           to plain arithmetic, 290 KB and more of SPIR-V. Maleoon's compiler never finished one (fa3033d0: asked for in 15
           runs, built in none), the one worker stayed in it and every pipeline behind it was never built, so the scene never
           reached the 1920x1080 target: a black picture under the HUD with a correct 854x480 scene (pd-a62).
           The network is purely spatial: one sampled input, one storage output (Swap2-Native rendering.md, "agl's NDSR
           upscaler network replaced by FSR 1 EASU"). Here it is not compiled at all; in its dispatch slot the scene is
           blitted (linear) into its output image. The source is the scene texture that agl's blend before the network samples
           (a335c1db6270c355, texture slot 3: linear colour), not the network's own input, which holds sqrt(colour) packed as
           RGB9E5 words; without that blend (another game), the network's input. TOTK_VK_NDSR=net runs the networks. */
        static int net = -1; if (net < 0) { const char *e = getenv("TOTK_VK_NDSR"); net = e && !strcmp(e, "net"); }
        static uint64_t scene;
        if (!strncmp(sh[5]->key, "a335c1db6270c355", 16)) scene = S.tex[5][3];
        int tr = -1, ir = -1;
        if (!net && ndsr_network(sh[5], &tr, &ir)) {
            uint64_t in = S.tex[5][tr], out = S.img[5][ir]; vk_treg rs, rd; int done = 0;
            int from_scene = scene && vk_treg_get((int)(scene & 0xfffff), &rs);
            if ((from_scene || (in && vk_treg_get((int)(in & 0xfffff), &rs))) && out && vk_treg_get((int)(out & 0xfffff), &rd)) {
                vk_tex *src = vk_tex_get(&rs.t), *dst = vk_tex_get(&rd.t);
                if (src && dst && src != dst && vk_tex_aspect(src) == VK_IMAGE_ASPECT_COLOR_BIT && vk_tex_aspect(dst) == VK_IMAGE_ASPECT_COLOR_BIT) {
                    vk_end_rendering_at(VK_PASS_END_COMPUTE, 0);
                    vk_tex_native_read(src); vk_tex_native_read(dst);
                    VkCommandBuffer cb = vk_cb(); vk_barrier_all(cb);
                    VkImageBlit r = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .srcOffsets = {{0, 0, 0}, {(int32_t)vk_tex_width(src, 0), (int32_t)vk_tex_height(src, 0), 1}},
                                     .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstOffsets = {{0, 0, 0}, {(int32_t)vk_tex_width(dst, 0), (int32_t)vk_tex_height(dst, 0), 1}}};
                    vkCmdBlitImage(cb, vk_tex_image(src), VK_IMAGE_LAYOUT_GENERAL, vk_tex_image(dst), VK_IMAGE_LAYOUT_GENERAL, 1, &r, VK_FILTER_LINEAR);
                    vk_barrier_all(cb);
                    vk_tex_mark_gpu_written(dst); done = 1;
                    /* once per network and scene size (the game's dynamic resolution shows here) */
                    static struct { char key[17]; uint32_t w, h; } said[24]; static int nsaid; int known = 0;
                    for (int i = 0; i < nsaid; i++) if (!strncmp(said[i].key, sh[5]->key, 16) && said[i].w == vk_tex_width(src, 0) && said[i].h == vk_tex_height(src, 0)) known = 1;
                    if (!known && nsaid < 24) { memcpy(said[nsaid].key, sh[5]->key, 16); said[nsaid].w = vk_tex_width(src, 0); said[nsaid].h = vk_tex_height(src, 0); nsaid++;
                        LOGV("NDSR network %.16s (%zu KB of SPIR-V) replaced by a linear upscale: %s %ux%u (VkFormat %d) into its output %ux%u (VkFormat %d) (TOTK_VK_NDSR=net runs it)",
                             sh[5]->key, sh[5]->spv_bytes >> 10, from_scene ? "the scene" : "its input", vk_tex_width(src, 0), vk_tex_height(src, 0), (int)vk_tex_format(src),
                             vk_tex_width(dst, 0), vk_tex_height(dst, 0), (int)vk_tex_format(dst)); }
                }
            }
            if (!done) RED(4, "NDSR network %.16s not replaced: texture %#llx or image %#llx does not resolve; its output keeps its old content", sh[5]->key, (unsigned long long)(from_scene ? scene : in), (unsigned long long)out);
            pd_dispatch_skip(sh[5], done ? "NDSR network: replaced by a blit of the scene" : "NDSR network: NOT replaced");
            g_stat_skips[9]++; return;
        }
    }
#endif
    VkBuffer ind_buf = VK_NULL_HANDLE; uint64_t ind_off = 0;
    if (dp->x < 0) {   /* DispatchComputeIndirect (hle_nvn.c): the group counts are at dp->caller, read by the GPU */
        if (!vk_resolve_for(dp->caller, 12, &ind_buf, &ind_off, NULL, VK_LOOKUP_OTHER) || (ind_off & 3)) {
            RED(4, "indirect dispatch arguments at %#llx are not in a memory pool", (unsigned long long)dp->caller); g_stat_skips[9]++; pd_dispatch_skip(sh[5], "indirect arguments in no pool"); return;
        }
        static int said; if (!said++) LOGV("indirect dispatch: %.16s, group counts at %#llx", sh[5]->key, (unsigned long long)dp->caller);
        if (__builtin_expect(g_pt_on, 0)) { VkDescriptorBufferInfo l; if (vk_pt_read(dp->caller, 12, 4, VK_LAG_OTHER, &l)) { ind_buf = l.buffer; ind_off = l.offset; } }   /* oot-a122 */
    }
    VkPipelineLayout layout; VkPipeline p = vk_compute_pipeline(sh[5], &layout);
    if (!p) { pd_dispatch_skip(sh[5], "its pipeline is not built (building in the background, or failed)"); return; }
    if (ind_buf) VK_INFLIGHT(VK_INFLIGHT_INDIRECT, dp->caller, 12, ind_buf);   /* copied into the driver constants and read by vkCmdDispatchIndirect */
    if (vk_lag_capture_on()) { storage_touch(5, sh[5], 1); storage_touch(5, sh[5], 0); }
    vk_end_rendering_at(VK_PASS_END_COMPUTE, 0);
    g_native_sample = NULL;
    if (vk_native_ui_enabled()) native_reads(sh,5,6);
    VkCommandBuffer cb = vk_cb();
    vk_barrier_all(cb);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, p);
    uint32_t grid[3] = {(uint32_t)dp->x, (uint32_t)dp->y, (uint32_t)dp->z};
    if (ind_buf) grid[0] = grid[1] = grid[2] = 1;   /* the real counts are on the GPU: copied into the driver constants below */
    g_cs_indirect = ind_buf != VK_NULL_HANDLE; g_cs_drv0.buffer = VK_NULL_HANDLE;
    if (__builtin_expect(vk_xlate_stats_enabled, 0)) {
        xlate_begin();
        bind_stage_impl(cb, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, sh[5], 5, grid, 1);
        xlate_end();
    } else bind_stage(cb, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, sh[5], 5, grid);
    g_cs_indirect = 0;
    if (ind_buf && g_cs_drv0.buffer) {   /* the shader reads c0: the GPU's counts at Maxwell's place and at NVN2's */
        VkBufferCopy r[2] = {{ind_off, g_cs_drv0.offset + 0x200, 12}, {ind_off, g_cs_drv0.offset + 4, 12}};
        vkCmdCopyBuffer(cb, ind_buf, g_cs_drv0.buffer, vk_nvn2 ? 2 : 1, r);
        vk_barrier_all(cb);
    }
    pd_dispatch(sh[5], grid);
    int pi = pp_begin(cb, 1); if (pi >= 0) pp_sh[pi] = sh[5];
    if (vk_gpuprof_on) { vk_gpuprof_dispatch_begin(cb); vk_gpuprof_draw(sh); }
    if (ind_buf) vkCmdDispatchIndirect(cb, ind_buf, ind_off);
    else vkCmdDispatch(cb, grid[0], grid[1], grid[2]);
    shader_xlate_first_command(sh[5], "dispatch");
    if (vk_fg_extract_active && grid[0] && grid[1] && grid[2]) fg_extract_compute(sh);
    vk_barrier_all(cb);
    pp_end(cb, pi);
    if (vk_gpuprof_on) vk_gpuprof_end(cb, VK_PASS_END_COMPUTE, 0);
    pd_buf(cb, 'd', (int)(g_stat_dispatch % 10000));
    g_bound_pipe = VK_NULL_HANDLE; g_stat_dispatch++;
}

/* ---------- NVN2 task / mesh draws ---------- */
/* Maleoon has no VK_EXT_mesh_shader (docs/DESIGN-OOT.md). nvnCommandBufferDrawMeshTasks[Indirect] is replayed as compute
   dispatches and one indexed draw: the program's task and mesh shaders are compute kernels in the seed, wrapped so that
   they write what the hardware's mesh stage hands to the rasterizer into buffers (tools/mesh_wrap_in_container.py):
     1. the render pass ends (a dispatch cannot run inside one);
     2. with a task shader, its kernel runs for each task workgroup: it writes its payload and, for every mesh workgroup
        it launches, a table entry (which payload, which of its launches); the launches are counted in the control block
        word that is also the mesh dispatch's indirect count;
     3. the mesh kernel runs for each mesh workgroup: MAX_V vertices of NCOMP words and MAX_P triangles in fixed slots a
        workgroup; workgroup 0 writes the draw's index count into the control block;
     4. the pass opens again (load) and the draw is vkCmdDrawIndexedIndirect with the program's pixel shader and the
        kernel's pass-through vertex shader (<key>-meshvs), which reads the vertex words by gl_VertexIndex.
   An indirect draw's record {count, first} is written by the game's GPU culling: it is copied into the control block on
   the GPU, never read by the CPU. The buffers hold a capacity of workgroups: exact for a direct draw without a task;
   otherwise what the same draw (kernels, record address) ran in earlier frames, which the GPU copies into a small feedback
   buffer, with headroom. A workgroup past the capacity does nothing (the kernels check) and the next frame's grows.

   Runs. Ocarina of Time draws about 40 meshes a frame in Kokiri Forest, most of them inside one pass, and a pass split a
   mesh cost 2.8 fps of 10.5 (oot-a70: 139 pass ends a frame against 100). So when a mesh draw has to end the pass, the
   kernels of the mesh draws that follow it in the command list are dispatched there too (mesh_ahead), up to the first op
   that may change what a kernel reads (a dispatch, a copy, a uniform update, the list's end ...): the bindings of the task
   and mesh stages and the program are followed through the ops in between, nothing else is. Those draws then find their
   buffers in a queue and draw without ending the pass.
   TOTK_OHOS_MESH=0: mesh draws are dropped. TOTK_OHOS_MESH_CAP=<n>: a draw's first capacity (default 64 workgroups).
   TOTK_OHOS_MESH_MB=<n>: the most vertex and index memory one draw may take (default 48). TOTK_OHOS_MESH_AHEAD=0: no runs. */
enum { MESH_SITES = 512, MESH_Q = 96 };
typedef struct { uint64_t key; uint32_t cap, tcap, peak, tpeak, seen; } mesh_site;
typedef struct { const p_draw_idx *rec; uint64_t sub; int ok; int32_t indices; vk_shader *vs; VkDescriptorBufferInfo buf[4];
                 uint32_t fb_site; int fb_due; } mesh_pre;   /* indices: the most the draw can have; fb_*: its feedback copy waits for the run's end (narrow barriers) */
static mesh_site g_mesh_site[MESH_SITES];
static mesh_pre g_mesh_q[MESH_Q]; static int g_mesh_qn, g_mesh_qi;   /* the draws dispatched ahead, in the command list's order; qi: the next expected */
/* TOTK_OHOS_MESH_RECHUNK=1: as before oot-a102 (a chunk submission may fall between a run's kernels and its draws, and the
   rest of the run dispatches its kernels a second time), to compare on the phone. */
static int mesh_rechunk_old(void) { static int v = -1; if (v < 0) { const char *e = getenv("TOTK_OHOS_MESH_RECHUNK"); v = e && *e == '1'; } return v; }
/* g_mesh.on: the draw being recorded is a mesh draw. The run's last queued draw is popped before its pass opens, and its
   control block is a transient of the slot its kernels are in (oot-a103). */
static int mesh_run_pending(void) { return !mesh_rechunk_old() && (g_mesh.on || (g_mesh_qi < g_mesh_qn && g_mesh_q[g_mesh_qi].sub == vk_submit_serial())); }
static VkBuffer g_mesh_fb; static uint32_t *g_mesh_fb_map;   /* a site's two words: the mesh workgroups and the tasks of its last draw the GPU finished */
static struct { uint64_t draws, direct, indirect, task, runs, ahead, splits, skip_shader, skip_build, skip_state, over, clamped, bytes, groups, report_ns, chunked;
                uint32_t stop[OP_COUNT_ + 2]; } g_mesh_stat;
static uint32_t g_mesh_first_cap, g_mesh_max_mb; static int g_mesh_ahead_on;
static int g_mesh_compile_only;   /* TOTK_OHOS_MESH=2 (diagnostic): the kernels' pipelines are built, nothing is dispatched or drawn: what the pipelines alone cost in GPU memory */
static int g_mesh_no_draw;        /* TOTK_OHOS_MESH=3 (diagnostic): the kernels are dispatched, the indexed draws are not made: dispatches against draws in GPU memory and time */
/* The vertex, index and payload buffers of the mesh draws are in chunks of their own, not in the submission slot's transient
   arena. That arena keeps the high water of each of the 8 slots for the session, and a view with 20,000 mesh workgroups
   takes 130 MiB of these buffers a frame, nearly all in one submission: turning the camera to Kokiri Forest's gate grew
   the GPU memory by 0.9 GiB in a minute and the system killed the process (RSS_THRESHOLD_KILLER; oot-a71c: PSS 7.7 ->
   8.3 GiB). A chunk is used again once the last submission that used it has completed, so the chunks alive are what the
   frames in flight need; one unused for 10 s is freed. Device memory, never mapped: only the GPU reads and writes them
   (the control block, which the CPU fills, stays in the transient arena: 80 bytes a draw). */
enum { MESH_CHUNKS = 64, MESH_CHUNK_MIN = 32u << 20 };   /* 32 MiB: an allocation of its own (vk_mem_alloc: above a quarter of an image block) */
typedef struct { VkBuffer buf; vk_alloc mem; VkDeviceSize size, used; uint64_t after, used_ns; } mesh_chunk;
static mesh_chunk g_mesh_chunk[MESH_CHUNKS]; static int g_mesh_nchunk, g_mesh_cur = -1;
static uint64_t g_mesh_chunk_bytes, g_mesh_chunk_peak;
static int mesh_alloc(size_t size, VkDeviceSize align, VkBuffer *buf, VkDeviceSize *off) {
    uint64_t fence = mtl_last_fence() + 1, now = host_now_ns();   /* the submission being recorded */
    if (g_mesh_cur >= 0) {
        mesh_chunk *c = &g_mesh_chunk[g_mesh_cur]; VkDeviceSize at = (c->used + align - 1) / align * align;
        if (at + size <= c->size) { c->used = at + size; c->after = fence; c->used_ns = now; *buf = c->buf; *off = at; return 1; }
    }
    g_mesh_cur = -1;
    for (int i = 0; i < g_mesh_nchunk && g_mesh_cur < 0; i++) {   /* one whose last submission is done */
        mesh_chunk *c = &g_mesh_chunk[i];
        if (c->buf && c->size >= size && mtl_wait(c->after, 0)) g_mesh_cur = i;
    }
    if (g_mesh_cur < 0) {
        int slot = -1; for (int i = 0; i < g_mesh_nchunk && slot < 0; i++) if (!g_mesh_chunk[i].buf) slot = i;
        if (slot < 0 && g_mesh_nchunk < MESH_CHUNKS) slot = g_mesh_nchunk++;
        if (slot < 0) return 0;
        mesh_chunk *c = &g_mesh_chunk[slot]; memset(c, 0, sizeof *c);
        VkDeviceSize bytes = size > MESH_CHUNK_MIN ? ((VkDeviceSize)size + 0xfffff) & ~(VkDeviceSize)0xfffff : MESH_CHUNK_MIN;
        VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = bytes, .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT};
        if (vkCreateBuffer(V.dev, &bci, NULL, &c->buf)) { c->buf = VK_NULL_HANDLE; return 0; }
        VkMemoryRequirements req; vkGetBufferMemoryRequirements(V.dev, c->buf, &req);
        if (!vk_mem_alloc(&req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &c->mem) || vkBindBufferMemory(V.dev, c->buf, c->mem.mem, c->mem.off)) {
            vkDestroyBuffer(V.dev, c->buf, NULL); c->buf = VK_NULL_HANDLE; return 0;
        }
        c->size = bytes; g_mesh_cur = slot;
        g_mesh_chunk_bytes += bytes; if (g_mesh_chunk_bytes > g_mesh_chunk_peak) g_mesh_chunk_peak = g_mesh_chunk_bytes;
    }
    mesh_chunk *c = &g_mesh_chunk[g_mesh_cur];
    c->used = size; c->after = fence; c->used_ns = now; *buf = c->buf; *off = 0;
    return 1;
}
static void mesh_chunks_trim(uint64_t now) {   /* every report: the chunks no draw has used for 10 s */
    for (int i = 0; i < g_mesh_nchunk; i++) {
        mesh_chunk *c = &g_mesh_chunk[i];
        if (!c->buf || i == g_mesh_cur || now - c->used_ns < 10000000000ull || !mtl_wait(c->after, 0)) continue;
        vkDestroyBuffer(V.dev, c->buf, NULL); vk_mem_free(&c->mem); g_mesh_chunk_bytes -= c->size; memset(c, 0, sizeof *c);
    }
}
static int mesh_feedback_init(void) {
    if (g_mesh_fb) return 1;
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = MESH_SITES * 8, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VkBuffer b; if (vkCreateBuffer(V.dev, &bci, NULL, &b)) return 0;
    VkMemoryRequirements req; vkGetBufferMemoryRequirements(V.dev, b, &req);
    vk_alloc a; if (!vk_mem_alloc(&req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &a)) { vkDestroyBuffer(V.dev, b, NULL); return 0; }
    if (vkBindBufferMemory(V.dev, b, a.mem, a.off) || vkMapMemory(V.dev, a.mem, a.off, MESH_SITES * 8, 0, (void **)&g_mesh_fb_map)) return 0;
    memset(g_mesh_fb_map, 0, MESH_SITES * 8); g_mesh_fb = b;
    return 1;
}
static mesh_site *mesh_site_of(uint64_t key, unsigned *index) {
    unsigned h = (unsigned)((key * 0x9E3779B97F4A7C15ull) >> 55) % MESH_SITES;
    for (unsigned k = 0; k < MESH_SITES; k++) {
        unsigned i = (h + k) % MESH_SITES;
        if (g_mesh_site[i].key == key || !g_mesh_site[i].key) { g_mesh_site[i].key = key; *index = i; return &g_mesh_site[i]; }
    }
    *index = h; return &g_mesh_site[h];   /* a full table: two draws share a slot's capacities */
}
static void mesh_report(void) {
    uint64_t now = host_now_ns(); if (!g_mesh_stat.report_ns) g_mesh_stat.report_ns = now;
    if (now - g_mesh_stat.report_ns < 5000000000ull) return;
    unsigned sites = 0; for (unsigned i = 0; i < MESH_SITES; i++) sites += g_mesh_site[i].key != 0;
    char stops[160] = ""; size_t o = 0;   /* what ended the runs: the op (nvn_obj.h's enum; OP_COUNT_: the queue was full, + 1: a malformed record) */
    for (int k = 0; k < 5; k++) {
        unsigned best = 0; for (unsigned i = 1; i < OP_COUNT_ + 2; i++) if (g_mesh_stat.stop[i] > g_mesh_stat.stop[best]) best = i;
        if (!g_mesh_stat.stop[best]) break;
        o += (size_t)snprintf(stops + o, sizeof stops - o, " op %u x%u", best, g_mesh_stat.stop[best]); g_mesh_stat.stop[best] = 0;
        if (o >= sizeof stops - 24) break;
    }
    unsigned chunks = 0; for (int i = 0; i < g_mesh_nchunk; i++) chunks += g_mesh_chunk[i].buf != VK_NULL_HANDLE;
    mesh_chunks_trim(now);
    LOGV("mesh draws in 5 s: %llu replayed (%llu direct, %llu indirect, %llu with a task shader) in %llu runs (%llu dispatched ahead; %llu runs began with a chunk submission, %llu pass starts held their chunk for a run), render pass split %llu times, "
         "capacity %llu workgroups in %.1f MiB; buffers now %u chunks of %.0f MiB (most %.0f MiB); skipped: %llu kernels missing, %llu pipelines building, %llu by state; "
         "%llu draws over their capacity, %llu clamped by TOTK_OHOS_MESH_MB; %u draw sites; runs ended by:%s",
         (unsigned long long)g_mesh_stat.draws, (unsigned long long)g_mesh_stat.direct, (unsigned long long)g_mesh_stat.indirect, (unsigned long long)g_mesh_stat.task,
         (unsigned long long)g_mesh_stat.runs, (unsigned long long)g_mesh_stat.ahead, (unsigned long long)g_mesh_stat.chunked, (unsigned long long)g_chunk_held,
         (unsigned long long)g_mesh_stat.splits, (unsigned long long)g_mesh_stat.groups, g_mesh_stat.bytes / 1048576.0,
         chunks, g_mesh_chunk_bytes / 1048576.0, g_mesh_chunk_peak / 1048576.0,
         (unsigned long long)g_mesh_stat.skip_shader, (unsigned long long)g_mesh_stat.skip_build, (unsigned long long)g_mesh_stat.skip_state,
         (unsigned long long)g_mesh_stat.over, (unsigned long long)g_mesh_stat.clamped, sites, stops[0] ? stops : " -");
    memset(&g_mesh_stat, 0, sizeof g_mesh_stat); g_mesh_stat.report_ns = now; g_chunk_held = 0;
}
/* TOTK_OHOS_MESH_BARRIER=narrow (oot-a106; unset: full barriers, the reference). The kernels of a run were bracketed by
   ALL_COMMANDS -> ALL_COMMANDS barriers: after a draw's indirect record copy, after its task kernel, after its mesh
   kernel. What each one has to order is less (every buffer a draw's kernels write is the draw's own):
     copy   the control block's dispatch count and first word (transfer write) -> the dispatch that reads them as its
            indirect record and the kernel that reads them as storage;
     task   payloads, table, launch count (compute write) -> the mesh kernel (storage, and the count as its indirect record);
     mesh   the control block (compute write) -> the copy of what the draw ran into the feedback buffer. The draw's own
            reads (vertex words in the vertex stage, indices, the indirect draw record) are ordered by the barrier its
            pass opens with (pass_begin_barrier: no kernel is dispatched while a pass is open), and whatever else
            follows a run begins with a barrier of its own.
   Narrow: copy and task name those stages only; the feedback copies of a run are made once, after its last kernel,
   behind one compute -> transfer barrier, so nothing follows a mesh kernel. A kernel that can write storage of the
   game's (a writable storage buffer or image that is not one of the four mesh buffers: 22 mesh and 3 task modules) keeps
   the full barrier after it: a later kernel may read what it wrote. The bindless pool's usage bitmap is not counted: it
   only gains bits, by atomics. */
static int mesh_barrier_narrow(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("TOTK_OHOS_MESH_BARRIER"); v = e && !strcmp(e, "narrow");
        LOGV("TOTK_OHOS_MESH_BARRIER=%s: the barriers between a run's mesh kernels are %s", e ? e : "(unset)",
             v ? "narrow (transfer / compute -> compute, indirect; one feedback copy barrier a run)" : "full (ALL_COMMANDS after every copy and kernel, the reference)");
    }
    return v;
}
static int mesh_kernel_writes(const vk_shader *sh) {   /* could the kernel write storage of the game's? */
    for (int i = 0; sh && i < sh->nb; i++) {
        const vk_bind *b = &sh->b[i];
        if (b->kind >= RB_MESH_IN && b->kind <= RB_MESH_PAR) continue;
        if (b->dtype == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) return 1;
        if ((b->dtype == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER || b->dtype == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC ||
             b->dtype == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER) && b->rw) return 1;
    }
    return 0;
}
/* from: 0 a transfer's write, 1 a kernel's; to: bit 0 a kernel and its indirect record, bit 1 a transfer. full: the old barrier. */
static void mesh_barrier(VkCommandBuffer cb, int from, int to, int full) {
    if (full || !mesh_barrier_narrow()) { vk_barrier_all(cb); return; }
    VkMemoryBarrier2 mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .srcStageMask = from ? VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT : VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
        .srcAccessMask = from ? VK_ACCESS_2_SHADER_WRITE_BIT : VK_ACCESS_2_TRANSFER_WRITE_BIT};
    if (to & 1) {
        mb.dstStageMask |= VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT;
        mb.dstAccessMask |= VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT;
    }
    if (to & 2) {
        mb.dstStageMask |= VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
        mb.dstAccessMask |= VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT;
    }
    VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vkCmdPipelineBarrier2(cb, &di);
}
/* Narrow barriers: what each draw of the run ran, into the feedback buffer, after the run's last kernel: the run's head
   and the draws mesh_ahead queued (the queue holds this run's draws only: it is emptied where a run starts). */
static void mesh_feedback_flush(VkCommandBuffer cb, mesh_pre *head) {
    uint32_t done[MESH_Q + 1]; int n = 0;
    for (int i = -1; i < g_mesh_qn; i++) {
        mesh_pre *e = i < 0 ? head : &g_mesh_q[i];
        if (!e->ok || !e->fb_due) continue;
        int again = 0;
        for (int k = 0; k < n; k++) again |= done[k] == e->fb_site;
        if (!n) mesh_barrier(cb, 1, 2, 0);            /* the kernels' control blocks -> the copies */
        else if (again) mesh_barrier(cb, 0, 2, 0);    /* the same site twice in a run: its two words are written again */
        VkDeviceSize coff = e->buf[3].offset;
        VkBufferCopy r[2] = {{coff + 4 * MESH_C_DISP, 8 * e->fb_site, 4}, {coff + 4 * MESH_C_TDISP, 8 * e->fb_site + 4, 4}};
        vkCmdCopyBuffer(cb, e->buf[3].buffer, g_mesh_fb, 2, r);
        done[n++] = e->fb_site; e->fb_due = 0;
    }
}
/* The kernels of one mesh draw, with the bindings S holds now, into cb (no pass is open). out: the draw's buffers. */
static int mesh_dispatch(const p_draw_idx *md, VkCommandBuffer cb, mesh_pre *out) {
    memset(out, 0, sizeof *out); out->rec = md; out->sub = vk_submit_serial();
    vk_shader *sh[6];
    if (!S.prog || vk_prog_resolve_stages(S.prog, sh, 1u << 3 | 1u << 4) || !sh[4] || sh[4]->mesh_stage != 7) {
        g_mesh_stat.skip_shader++; g_stat_skips[4]++;
        return 0;
    }
    vk_shader *ms = sh[4], *ts = ms->mesh_task ? sh[3] : NULL, *vs = vk_shader_mesh_vs(ms);
    if ((ms->mesh_task && (!ts || ts->mesh_stage != 6)) || !vs || ms->mesh_verts != 3 || !ms->mesh_ncomp || !ms->mesh_max_v || !ms->mesh_max_p) {
        RED(8, "mesh draw skipped: mesh kernel %.16s %s", ms->key, !vs ? "has no vertex shader in the seed" : ms->mesh_task ? "needs a task shader the program does not have" : "is not a triangle mesh");
        g_mesh_stat.skip_shader++; g_stat_skips[4]++; return 0;
    }
    VkPipelineLayout mlayout, tlayout = VK_NULL_HANDLE;
    VkPipeline mp = vk_compute_pipeline(ms, &mlayout), tp = ts ? vk_compute_pipeline(ts, &tlayout) : VK_NULL_HANDLE;
    if (!mp || (ts && !tp)) { g_mesh_stat.skip_build++; g_stat_skips[10]++; return 0; }
    if (g_mesh_compile_only) { g_mesh_stat.skip_state++; return 0; }
    if (!mesh_feedback_init()) { RED(4, "mesh draw skipped: no feedback buffer"); return 0; }
    /* the indirect record, read by the GPU */
    VkBuffer ind_buf = VK_NULL_HANDLE; uint64_t ind_off = 0;
    if (md->ind && (!vk_resolve_for(md->ind, 8, &ind_buf, &ind_off, NULL, VK_LOOKUP_OTHER) || (ind_off & 3))) {
        RED(4, "mesh draw skipped: its indirect record at %#llx is in no memory pool", (unsigned long long)md->ind); g_stat_skips[8]++; return 0;
    }
    if (md->ind && __builtin_expect(g_pt_on, 0)) { VkDescriptorBufferInfo l; if (vk_pt_read(md->ind, 8, 4, VK_LAG_OTHER, &l)) { ind_buf = l.buffer; ind_off = l.offset; } }   /* oot-a122 */
    uint32_t count = md->ind ? 0 : (uint32_t)md->count, first = md->ind ? 0 : (uint32_t)md->base_vertex;
    /* the capacities: from what this draw ran before */
    uint64_t key = ((uint64_t)(uintptr_t)ms * 0x9E3779B97F4A7C15ull) ^ ((uint64_t)(uintptr_t)ts * 0xC2B2AE3D27D4EB4Full) ^
                   (md->ind ? md->ind * 0x165667B19E3779F9ull : ((uint64_t)first << 32 | count) * 0x27D4EB2F165667C5ull);
    unsigned si; mesh_site *site = mesh_site_of(key | 1, &si);
    uint32_t ran = g_mesh_fb_map[2 * si], tran = g_mesh_fb_map[2 * si + 1];
    if (site->seen && site->cap && ran > site->cap) g_mesh_stat.over++;
    site->peak = ran > site->peak ? ran : site->peak - site->peak / 64;
    site->tpeak = tran > site->tpeak ? tran : site->tpeak - site->tpeak / 64;
    uint32_t cap, tcap = 0;
    if (!ts && !md->ind) cap = count;
    else cap = site->seen ? MAXV(16u, site->peak + site->peak / 4 + 8) : g_mesh_first_cap;   /* a quarter over what it ran: a draw that grows faster loses workgroups for one frame */
    if (ts) tcap = md->ind ? (site->seen ? MAXV(16u, site->tpeak + site->tpeak / 4 + 8) : g_mesh_first_cap) : count;
    uint64_t group_bytes = (uint64_t)ms->mesh_max_v * ms->mesh_ncomp * 4 + (uint64_t)ms->mesh_max_p * 12;
    uint64_t max_groups = ((uint64_t)g_mesh_max_mb << 20) / group_bytes;
    if (cap > max_groups) { cap = (uint32_t)max_groups; g_mesh_stat.clamped++; }
    if (tcap > 65536) { tcap = 65536; g_mesh_stat.clamped++; }
    if (!cap || (ts && !tcap)) return 0;
    site->cap = cap; site->tcap = tcap; site->seen = 1;
    /* the buffers */
    VkDeviceSize sa = MAXV(V.props.limits.minStorageBufferOffsetAlignment, (VkDeviceSize)16);
    size_t vtx_bytes = (size_t)cap * ms->mesh_max_v * ms->mesh_ncomp * 4, idx_bytes = (size_t)cap * ms->mesh_max_p * 12;
    size_t in_bytes = ts ? ((size_t)2 * cap + (size_t)tcap * ts->mesh_pay + ms->mesh_pay + 16) * 4 : 0;
    memset(g_mesh.buf, 0, sizeof g_mesh.buf);
    VkBuffer b; VkDeviceSize o;
    if ((ts && !mesh_alloc(in_bytes, sa, &b, &o))) { RED(4, "mesh draw skipped: no memory for its payload buffer (%zu bytes)", in_bytes); return 0; }
    if (ts) g_mesh.buf[0] = (VkDescriptorBufferInfo){b, o, in_bytes};
    if (!mesh_alloc(vtx_bytes, sa, &b, &o)) { RED(4, "mesh draw skipped: no memory for its vertex buffer (%zu bytes)", vtx_bytes); return 0; }
    g_mesh.buf[1] = (VkDescriptorBufferInfo){b, o, vtx_bytes};
    if (!mesh_alloc(idx_bytes, sa, &b, &o)) { RED(4, "mesh draw skipped: no memory for its index buffer (%zu bytes)", idx_bytes); return 0; }
    g_mesh.buf[2] = (VkDescriptorBufferInfo){b, o, idx_bytes};
    uint32_t *c = vk_transient(MESH_C_WORDS * 4, sa, &b, &o); g_mesh.buf[3] = (VkDescriptorBufferInfo){b, o, MESH_C_WORDS * 4};
    memset(c, 0, MESH_C_WORDS * 4);
    c[MESH_C_FIRST] = first; c[MESH_C_CAP] = cap; c[MESH_C_TCAP] = tcap;
    c[MESH_C_TDISP] = ts ? count : 0; c[MESH_C_TDISP + 1] = c[MESH_C_TDISP + 2] = 1;
    c[MESH_C_DISP] = ts ? 0 : count; c[MESH_C_DISP + 1] = c[MESH_C_DISP + 2] = 1;   /* with a task shader: its kernel counts the launches here */
    c[MESH_C_DRAW] = 0; c[MESH_C_DRAW + 1] = 1;                                     /* mesh workgroup 0 writes the index count */
    if (vk_lag_capture_on()) {
        storage_touch(4, ms, 1); storage_touch(4, ms, 0);
        if (ts) { storage_touch(3, ts, 1); storage_touch(3, ts, 0); }
    }
    g_native_sample = NULL;
    if (vk_native_ui_enabled()) native_reads(sh, 3, 5);
    VkBuffer cbuf = g_mesh.buf[3].buffer; VkDeviceSize coff = g_mesh.buf[3].offset;
    /* narrow: a kernel that writes the game's storage waits for every earlier kernel of the run too, whose reads of that
       storage the full barrier after each kernel used to order (oot-a107, from the review: draw A's kernel reads a buffer
       that draw B's kernel then atomically updates) */
    if (mesh_barrier_narrow() && (mesh_kernel_writes(ms) || mesh_kernel_writes(ts))) vk_barrier_all(cb);
    if (ind_buf) {   /* {count, first}: the count into the first dispatch's record, first into the control block */
        VK_INFLIGHT(VK_INFLIGHT_INDIRECT, md->ind, 8, ind_buf);
        VkBufferCopy r[2] = {{ind_off, coff + 4 * (ts ? MESH_C_TDISP : MESH_C_DISP), 4}, {ind_off + 4, coff + 4 * MESH_C_FIRST, 4}};
        vkCmdCopyBuffer(cb, ind_buf, cbuf, 2, r);
        mesh_barrier(cb, 0, 1, 0);
    }
    g_cs_indirect = 0;
    if (ts) {
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, tp);
        bind_stage(cb, VK_PIPELINE_BIND_POINT_COMPUTE, tlayout, 0, ts, 3, NULL);
        { int pi = pp_begin(cb, 1); if (pi >= 0) { pp_sh[pi] = ts; pp_ran[pi] = tran; } vkCmdDispatchIndirect(cb, cbuf, coff + 4 * MESH_C_TDISP); pp_end(cb, pi); }   /* in the pass profile's dispatches */
        shader_xlate_first_command(ts, "dispatch");
        mesh_barrier(cb, 1, 1, mesh_kernel_writes(ts));
    }
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, mp);
    bind_stage(cb, VK_PIPELINE_BIND_POINT_COMPUTE, mlayout, 0, ms, 4, NULL);
    { int pi = pp_begin(cb, 1); if (pi >= 0) { pp_sh[pi] = ms; pp_ran[pi] = ran; } vkCmdDispatchIndirect(cb, cbuf, coff + 4 * MESH_C_DISP); pp_end(cb, pi); }
    shader_xlate_first_command(ms, "dispatch");
    if (!mesh_barrier_narrow()) {
        vk_barrier_all(cb);
        /* what it ran, for the next frame's capacities */
        VkBufferCopy r[2] = {{coff + 4 * MESH_C_DISP, 8 * si, 4}, {coff + 4 * MESH_C_TDISP, 8 * si + 4, 4}};
        vkCmdCopyBuffer(cb, cbuf, g_mesh_fb, 2, r);
    } else {
        /* narrow: no barrier (the draw's reads are behind its pass's opening barrier), the feedback copy at the run's end
           (mesh_feedback_flush); a kernel that can write the game's storage keeps the full barrier */
        if (mesh_kernel_writes(ms)) vk_barrier_all(cb);
        out->fb_site = si; out->fb_due = 1;
    }
    g_bound_pipe = VK_NULL_HANDLE; g_stat_dispatch += ts ? 2 : 1;
    g_mesh_stat.draws++; g_mesh_stat.groups += cap; g_mesh_stat.bytes += vtx_bytes + idx_bytes + in_bytes;
    if (md->ind) g_mesh_stat.indirect++; else g_mesh_stat.direct++;
    if (ts) g_mesh_stat.task++;
    {   /* the first draws, and each new pair of kernels */
        static uint64_t n; static const void *said[64][2]; static int nsaid; int known = 0;
        for (int i = 0; i < nsaid; i++) if (said[i][0] == (const void *)ms && said[i][1] == (const void *)ts) known = 1;
        if (!known && nsaid < 64) { said[nsaid][0] = ms; said[nsaid][1] = ts; nsaid++; }
        if (++n <= 4 || !known)
            LOGV("mesh draw %llu: mesh %.16s (%u KB) task %.16s; %s%s; capacity %u workgroups (%u vertices x %u words, %u triangles a workgroup: %.1f MiB); the draw ran %u before",
                 (unsigned long long)n, ms->key, (unsigned)(ms->spv_bytes >> 10), ts ? ts->key : "-", md->ind ? "indirect" : "direct", ts ? ", tasks launch the mesh workgroups" : "",
                 cap, ms->mesh_max_v, ms->mesh_ncomp, ms->mesh_max_p, (vtx_bytes + idx_bytes + in_bytes) / 1048576.0, ran);
    }
    pd_draw_note("      mesh kernels: mesh %.16s task %.16s %s capacity %u (ran %u before)", ms->key, ts ? ts->key : "-", md->ind ? "indirect" : "direct", cap, ran);
    out->ok = 1; out->vs = vs; memcpy(out->buf, g_mesh.buf, sizeof out->buf);
    out->indices = (int32_t)MINV((uint64_t)cap * ms->mesh_max_p * 3, (uint64_t)INT32_MAX);
    memset(g_mesh.buf, 0, sizeof g_mesh.buf);
    return 1;
}
/* The mesh draws after the one being replayed: their kernels now, in the same pass split (see "Runs" above). */
static void mesh_ahead(VkCommandBuffer cb) {
    const uint8_t *p = vk_exec_draw_next(); if (!p) return;
    struct { const prog_t *prog; p_bind_buf ubo[2][16], ssbo[2][16]; uint64_t tex[2][128], samp[2][128], img[2][32]; int tex_hi[2]; } keep;
    keep.prog = S.prog; memcpy(keep.ubo, S.ubo[3], sizeof keep.ubo); memcpy(keep.ssbo, S.ssbo[3], sizeof keep.ssbo);
    memcpy(keep.tex, S.tex[3], sizeof keep.tex); memcpy(keep.samp, S.samp[3], sizeof keep.samp); memcpy(keep.img, S.img[3], sizeof keep.img);
    keep.tex_hi[0] = S.tex_hi[3]; keep.tex_hi[1] = S.tex_hi[4];
    int changed = 0, stop = -1;   /* stop: the op that ended the run (OP_COUNT_: the queue is full, + 1: a malformed record) */
    for (int guard = 0; guard < 8192 && stop < 0; guard++) {
        op_hdr h; memcpy(&h, p, sizeof h); const void *a = p + 8;
        if (h.size < 8 || (h.size & 7) || h.op >= OP_COUNT_) { stop = OP_COUNT_ + 1; break; }
        switch (h.op) {
            case OP_BIND_PROGRAM: {   /* as vk_draw_exec, without its effects on the graphics state */
                const p_bind_program *b = a;
                if (b->stages && !(b->stages & ~NVN_SHADER_STAGE_COMPUTE_BIT)) break;
                if (!b->prog && b->stages && !(b->stages & NVN_SHADER_STAGE_VERTEX_BIT)) break;
                if (S.prog != b->prog) { S.prog = b->prog; changed = 1; }
                break; }
            case OP_BIND_UBO: case OP_BIND_SSBO: {
                const p_bind_buf *b = a;
                if ((b->stage != 3 && b->stage != 4) || b->index < 0 || b->index >= 16) break;
                p_bind_buf *slot = h.op == OP_BIND_UBO ? &S.ubo[b->stage][b->index] : &S.ssbo[b->stage][b->index];
                if (slot->addr != b->addr || slot->size != b->size) { *slot = *b; changed = 1; if (h.op == OP_BIND_SSBO) g_driver_revision[0][b->stage]++; }
                break; }
            case OP_BIND_TEX: case OP_BIND_SAMP: case OP_BIND_IMG: {
                const p_bind_handles *b = a; int limit = h.op == OP_BIND_IMG ? 32 : 128;
                if ((b->stage != 3 && b->stage != 4) || b->first < 0 || b->n < 0 || b->first + b->n > limit) break;
                uint64_t *slots = h.op == OP_BIND_TEX ? S.tex[b->stage] : h.op == OP_BIND_SAMP ? S.samp[b->stage] : S.img[b->stage];
                if (memcmp(slots + b->first, b->h, 8 * (size_t)b->n)) {
                    memcpy(slots + b->first, b->h, 8 * (size_t)b->n); changed = 1;
                    g_driver_revision[1][b->stage]++; vk_stage_sig_invalidate(1u << b->stage);
                    if (h.op == OP_BIND_TEX && b->first + b->n > S.tex_hi[b->stage]) S.tex_hi[b->stage] = b->first + b->n;
                }
                break; }
            case OP_DRAW_IDX: {
                const p_draw_idx *d = a;
                if (d->prim != DM_PRIM_MESH_TASKS) break;   /* another draw: it writes attachments, which no kernel reads */
                if (g_mesh_qn == MESH_Q) { stop = OP_COUNT_; break; }
                mesh_dispatch(d, cb, &g_mesh_q[g_mesh_qn++]); g_mesh_stat.ahead++;
                break; }
            /* state no kernel reads */
            case OP_DRAW: case OP_BEGIN: case OP_SET_RT: case OP_CLEAR_COLOR: case OP_CLEAR_DS: case OP_VIEWPORTS: case OP_SCISSOR: case OP_DEPTH_RANGE:
            case OP_BIND_STATE: case OP_BIND_VATTR: case OP_BIND_VSTREAM: case OP_BIND_VBUF: case OP_STENCIL: case OP_POLY_OFFSET: case OP_BLEND_COLOR:
            case OP_DEPTH_CLAMP: case OP_RENDER_ENABLE: case OP_RASTER_DISCARD: case OP_RENDER_COND: case OP_ALPHA_REF: case OP_PUSH_CONST: case OP_BARRIER:
            case OP_REPORT_COUNTER: case OP_RESET_COUNTER: case OP_SIGNAL_EVENT: case OP_COPY_STRIDE:
                break;
            default: stop = (int)h.op; break;   /* a dispatch, a copy, a uniform update, a fill, a pool change, a call, the list's end */
        }
        p += h.size;
    }
    if (stop >= 0) g_mesh_stat.stop[stop]++;
    S.prog = keep.prog; memcpy(S.ubo[3], keep.ubo, sizeof keep.ubo); memcpy(S.ssbo[3], keep.ssbo, sizeof keep.ssbo);
    memcpy(S.tex[3], keep.tex, sizeof keep.tex); memcpy(S.samp[3], keep.samp, sizeof keep.samp); memcpy(S.img[3], keep.img, sizeof keep.img);
    S.tex_hi[3] = keep.tex_hi[0]; S.tex_hi[4] = keep.tex_hi[1];
    if (changed) { for (int st = 3; st <= 4; st++) { g_driver_revision[0][st]++; g_driver_revision[1][st]++; } vk_stage_sig_invalidate(1u << 3 | 1u << 4); }
}
static void mesh_draw(const p_draw_idx *md) {
#ifdef TOTK_OHOS
    vk_walker_keep();   /* as draw() */
#endif
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("TOTK_OHOS_MESH"); on = !(e && *e == '0'); g_mesh_compile_only = e && *e == '2'; g_mesh_no_draw = e && *e == '3';
        e = getenv("TOTK_OHOS_MESH_CAP"); g_mesh_first_cap = e && atoi(e) > 0 ? (uint32_t)atoi(e) : 64;
        e = getenv("TOTK_OHOS_MESH_MB"); g_mesh_max_mb = e && atoi(e) > 0 ? (uint32_t)atoi(e) : 48;
        e = getenv("TOTK_OHOS_MESH_AHEAD"); g_mesh_ahead_on = !(e && *e == '0');
        LOGV("task / mesh draws: %s (TOTK_OHOS_MESH); first capacity %u workgroups (TOTK_OHOS_MESH_CAP), at most %u MiB a draw (TOTK_OHOS_MESH_MB); runs %s (TOTK_OHOS_MESH_AHEAD)",
             on ? "replayed as compute dispatches and an indexed draw" : "dropped", g_mesh_first_cap, g_mesh_max_mb, g_mesh_ahead_on ? "on" : "off");
    }
    mesh_report();
    if (!on) return;
    /* dispatched ahead by the run's first draw? */
    mesh_pre *e = NULL, head;
    if (g_mesh_qi < g_mesh_qn && g_mesh_q[g_mesh_qi].rec == md && g_mesh_q[g_mesh_qi].sub == vk_submit_serial()) e = &g_mesh_q[g_mesh_qi++];
    else g_mesh_qn = g_mesh_qi = 0;   /* not the draw the run expected (or no run): a new run starts here */
    if (!S.prog || S.render_disabled || S.raster_discard || S.poly.cull == NVN_FACE_FRONT_AND_BACK) { g_mesh_stat.skip_state++; return; }
    if (!e) {
        if (!vk_lag_uniform_private()) vk_ub_flush();   /* as dispatch_bound */
        if (R.open) g_mesh_stat.splits++;
        vk_end_rendering_at(VK_PASS_END_COMPUTE, 0);
        {   /* the chunk boundary that the pass of this draw would take goes here, before the run's kernels (oot-a102) */
            long lim = vk_frame_chunk_limit();
            if (lim > 0 && g_chunk_draws >= (uint64_t)lim && !mesh_rechunk_old()) {
                uint64_t s0 = vk_submit_serial(); g_chunk_draws = 0; vk_submit_chunk();
                g_mesh_stat.chunked += vk_submit_serial() != s0;   /* (nothing recorded yet: no submission) */
            }
        }
        VkCommandBuffer cb = vk_cb();
        vk_barrier_all(cb);
        g_mesh_stat.runs++;
        int ok = mesh_dispatch(md, cb, &head);
        if (g_mesh_ahead_on) mesh_ahead(cb);
        mesh_feedback_flush(cb, &head);   /* (narrow barriers; nothing is due otherwise) */
        if (!ok) { pd_draw_note("      mesh draw skipped: its kernels are missing in the seed or still building"); return; }
        e = &head;
    } else if (!e->ok) { pd_draw_note("      mesh draw skipped: its kernels are missing in the seed or still building"); return; }
    if (g_mesh_no_draw) return;
    /* the draw itself: the pass opens (or goes on) in draw_inner */
    g_mesh.on = 1; g_mesh.vs = e->vs; memcpy(g_mesh.buf, e->buf, sizeof g_mesh.buf);
    {   /* its chunks are in use by the submission being recorded now (the kernels may have run in the one before) */
        uint64_t fence = mtl_last_fence() + 1;
        for (int i = 0; i < g_mesh_nchunk; i++) for (int k = 0; k < 3; k++)
            if (g_mesh_chunk[i].buf && g_mesh_chunk[i].buf == g_mesh.buf[k].buffer && g_mesh_chunk[i].after < fence) g_mesh_chunk[i].after = fence;
    }
    p_draw synthetic = {4 /* triangles */, 0, e->indices, 0, 1};   /* the real count is the control block's draw record, written by the GPU */
    draw_body(&synthetic, NULL);
    g_mesh.on = 0; g_mesh.vs = NULL; memset(g_mesh.buf, 0, sizeof g_mesh.buf);
}

/* ---------- copies and clears of textures ---------- */
static unsigned vlevel(const view_t *v, int has) { return has && v->set_levels ? v->base_level : 0; }
static unsigned vlayer(const view_t *v, int has) { return has && v->set_layers ? v->min_layer : 0; }
static VkImageAspectFlags copy_aspect(vk_tex *x) { VkImageAspectFlags a = vk_tex_aspect(x); return a == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT) ? VK_IMAGE_ASPECT_DEPTH_BIT : a; }
/* Replay diagnostics only: no texture following, buffer substitution, or per-draw work. */
static void texcopy_region(const char *op, const char *side, const tex_t *t, const view_t *v, int has, const NVNcopyRegion *r) {
    totk_texcopy_log(vk_submit_serial(),
        "phase=replay op=%s side=%s serial=%llu mem=%p storage_size=%zu format=%#x level=%u layer=%u region=%d,%d,%d,%dx%dx%d",
        op, side, (unsigned long long)t->serial, t->mem, (size_t)t->L.size, t->d.format,
        vlevel(v, has), vlayer(v, has), r->xoffset, r->yoffset, r->zoffset, r->width, r->height, r->depth);
}
static int texcopy_buffer_attempt(const p_copy_b2t *c, const char *op, int upload) {
    size_t bytes = (size_t)totk_texcopy_buffer_size(c->tex, &c->r, S.copy_stride, 0);
    int texture_match = totk_texcopy_match(c->tex);
    totk_texcopy_track(c->tex);
    tex_t overlap;
    int buffer_match = totk_texcopy_overlap(c->src, bytes ? bytes : 1, &overlap);
    if (!texture_match && !buffer_match) return 0;
    totk_texcopy_log(vk_submit_serial(),
        "phase=replay op=%s result=attempt texture_side=%s serial=%llu buffer_side=%s buffer=%#llx bytes=%zu bytes_known=%d copy_stride=%lld flags=%#x match_texture=%d match_buffer=%d",
        op, upload ? "dst" : "src", (unsigned long long)c->tex->serial, upload ? "src" : "dst",
        (unsigned long long)c->src, bytes, bytes != 0, (long long)S.copy_stride, c->flags, texture_match, buffer_match);
    texcopy_region(op, upload ? "dst" : "src", c->tex, &c->view, c->has_view, &c->r);
    if (buffer_match) totk_texcopy_log(vk_submit_serial(),
        "phase=replay op=%s buffer_side=%s overlap_serial=%llu overlap_mem=%p overlap_size=%zu",
        op, upload ? "src" : "dst", (unsigned long long)overlap.serial, overlap.mem, (size_t)overlap.L.size);
    return 1;
}
static void texcopy_buffer_resolved(const char *op, const char *side, const p_copy_b2t *c, VkBuffer b, uint64_t o, size_t row, size_t rows, size_t img, int nz) {
    totk_texcopy_log(vk_submit_serial(),
        "phase=replay op=%s buffer_side=%s buffer=%#llx bytes=%zu vk_buffer=%#llx offset=%#llx row=%zu rows=%zu image_stride=%zu depth=%d",
        op, side, (unsigned long long)c->src, img * nz, (unsigned long long)(uintptr_t)b,
        (unsigned long long)o, row, rows, img, nz);
}
static void texcopy_guest_source(const p_copy_b2t *c, size_t row, size_t img) {
    static unsigned samples;
    if (samples >= 32) {
        totk_texcopy_log(vk_submit_serial(),
            "phase=replay op=CopyBufferToTexture buffer_side=src data=guest-source-snapshot status=skip reason=snapshot-budget gpu_authoritative=0");
        return;
    }
    samples++;
    size_t bytes = (size_t)totk_texcopy_buffer_size(c->tex, &c->r, (ptrdiff_t)row, (ptrdiff_t)img);
    if (bytes > (size_t)1024 * 1024) {
        totk_texcopy_log(vk_submit_serial(),
            "phase=replay op=CopyBufferToTexture buffer_side=src data=guest-source-snapshot status=skip reason=snapshot-size bytes=%zu gpu_authoritative=0", bytes);
        return;
    }
    totk_texcopy_stats s = totk_texcopy_payload(c->tex, &c->r, (const void *)(uintptr_t)c->src, (ptrdiff_t)row, (ptrdiff_t)img);
    totk_texcopy_log(vk_submit_serial(),
        "phase=replay op=CopyBufferToTexture buffer_side=src data=guest-source-snapshot status=%s buffer=%#llx bytes=%zu sample_bytes=%zu nonzero=%zu zero=%zu rg=%d r_min=%u r_max=%u g_min=%u g_max=%u gpu_authoritative=0",
        s.valid ? "sampled" : "invalid", (unsigned long long)c->src, bytes, s.bytes, s.nonzero, s.bytes - s.nonzero,
        s.rg, s.min[0], s.max[0], s.min[1], s.max[1]);
}
static void copy_b2t(const p_copy_b2t *c) {   /* buffer -> texture (row stride from SetCopyRowStride, 0 = packed) */
    int trace = totk_texcopy_on() && texcopy_buffer_attempt(c, "CopyBufferToTexture", 1);
    if (!vk_lag_uniform_private()) vk_ub_flush();
    else vk_ub_touch((uint64_t)(uintptr_t)c->tex->mem, c->tex->L.size);
    vk_tex *x = vk_tex_get(c->tex);
    if (trace && !x) vk_texcopy_image("CopyBufferToTexture", "dst", c->tex, x);
    if (!x) {
        if (trace) totk_texcopy_log(vk_submit_serial(), "phase=replay op=CopyBufferToTexture result=skip reason=texture-unavailable");
        return;
    }
    int bw, bh, bpb = nvn_fmt_bpb(c->tex->d.format, &bw, &bh);
    if (!bpb) {
        if (trace) totk_texcopy_log(vk_submit_serial(), "phase=replay op=CopyBufferToTexture result=skip reason=unsupported-format");
        return;
    }
    unsigned level = vlevel(&c->view, c->has_view), layer0 = vlayer(&c->view, c->has_view);
    size_t row = S.copy_stride ? (size_t)S.copy_stride : (size_t)((c->r.width + bw - 1) / bw) * bpb, rows = (c->r.height + bh - 1) / bh, img = row * rows;
    int nz = c->r.depth ? c->r.depth : 1, is3d = vk_tex_is3d(x);
    if (vk_lag_uniform_private()) vk_ub_touch(c->src, img * nz);
    VkBuffer b; uint64_t o;
    if (!vk_resolve(c->src, img * nz, &b, &o)) {
        if (trace) totk_texcopy_log(vk_submit_serial(), "phase=replay op=CopyBufferToTexture result=skip reason=buffer-unresolved buffer=%#llx bytes=%zu", (unsigned long long)c->src, img * nz);
        RED(4, "buffer-to-texture copy: the source is not in a memory pool"); return;
    }
    /* oot-a132: ended before the source read. With protection on, vk_lag_read copies the source into the open slot's
       transient memory, and ending rendering can flush pending clears and submit a chunk (a new slot): the copy below
       would then read another slot's memory, which is reset or freed when that slot is reused (review of a129). */
    vk_end_rendering_at(VK_PASS_END_COPY, OP_COPY_B2T);
    VkDescriptorBufferInfo frozen;
    if (vk_lag_read(c->src, img * nz, 16, &frozen, VK_LAG_COPY)) {
        b = frozen.buffer; o = frozen.offset;
        if (trace) totk_texcopy_log(vk_submit_serial(), "phase=replay op=CopyBufferToTexture buffer_side=src source=lag-snapshot");
    }
    VK_INFLIGHT(VK_INFLIGHT_COPY, c->src, img * nz, b);   /* the buffer the copy reads: a frozen copy is not noted */
    if (trace) texcopy_buffer_resolved("CopyBufferToTexture", "src", c, b, o, row, rows, img, nz);
    if (row % bpb || o % 4) {
        if (trace) totk_texcopy_log(vk_submit_serial(), "phase=replay op=CopyBufferToTexture result=skip reason=buffer-layout-unexpressible row=%zu offset=%#llx", row, (unsigned long long)o);
        RED(4, "buffer-to-texture copy with stride %zu / offset %#llx not expressible", row, (unsigned long long)o); return;
    }
    if (trace) texcopy_guest_source(c, row, img);
    vk_tex_native_read(x);
    if (trace) vk_texcopy_image("CopyBufferToTexture", "dst", c->tex, x);
    vk_tex_claim(x);
    VkCommandBuffer cb = vk_cb(); vk_barrier_all(cb);
    if (vk_gpuprof_on) vk_gpuprof_scope_begin(cb, "upload");
    VkBufferImageCopy r = {.bufferOffset = o, .bufferRowLength = (uint32_t)(row / bpb * bw), .bufferImageHeight = (uint32_t)(rows * bh),
        .imageSubresource = {copy_aspect(x), level, is3d ? 0 : layer0 + c->r.zoffset, is3d ? 1 : (uint32_t)nz},
        .imageOffset = {c->r.xoffset, c->r.yoffset, is3d ? c->r.zoffset : 0}, .imageExtent = {(uint32_t)c->r.width, (uint32_t)c->r.height, is3d ? (uint32_t)nz : 1}};
    vkCmdCopyBufferToImage(cb, b, vk_tex_image(x), VK_IMAGE_LAYOUT_GENERAL, 1, &r);
    totk_ft_count(FT_COUNT_TEX_UPLOADS, 1);
    vk_barrier_all(cb);
    if (vk_gpuprof_on) vk_gpuprof_scope_end(cb);
    vk_tex_native_patch(x, level, layer0, &c->r);
    if (vk_fg_extract_active) fg_extract_texture_write(c->tex, level, is3d ? 0 : layer0 + c->r.zoffset,
        is3d ? 1 : (unsigned)nz, copy_aspect(x),
        !c->r.xoffset && !c->r.yoffset && (unsigned)c->r.width == vk_tex_width(x, level) &&
        (unsigned)c->r.height == vk_tex_height(x, level), 0, 0);
    vk_tex_mark_gpu_written(x); probe_write_note('B', x, NULL, 0);
    vk_pso_life_external_write(x, level, r.imageSubresource.baseArrayLayer, r.imageSubresource.layerCount, "buffer-copy");
    if (trace) {
        totk_texcopy_log(vk_submit_serial(), "phase=replay op=CopyBufferToTexture result=success encoded=1 dst_serial=%llu bytes=%zu", (unsigned long long)c->tex->serial, img * nz);
        vk_texcopy_readback("CopyBufferToTexture", "dst", c->tex, x, level, layer0, &c->r);
    }
}
/* TOTK_OHOS_GW_DROP=<addr>[,<addr>...] (oot-a120, measurement only): texture -> buffer copies whose destination holds one
   of these guest addresses are skipped, so the game reads whatever was there before; to find which GPU result the game
   reads after its frame-end wait (two frames in flight flash, DESIGN-OOT oot-a118/a119) */
static int gw_drop(uint64_t a, uint64_t n) {
    static uint64_t list[16]; static int nl = -1;
    if (nl < 0) { nl = 0; const char *e = getenv("TOTK_OHOS_GW_DROP");
        for (const char *p = e; p && *p && nl < 16; ) { char *end; uint64_t v = strtoull(p, &end, 0); if (end == p) break; list[nl++] = v; p = *end == ',' ? end + 1 : end; }
        if (nl) LOGV("GPU-write drop: %d addresses (TOTK_OHOS_GW_DROP); texture-to-buffer copies over them are skipped, writable storage bindings get a scratch copy", nl); }
    for (int i = 0; i < nl; i++) if (list[i] >= a && list[i] < a + n) {
        static unsigned said; if (said++ < 8) LOGV("GPU-write drop: GPU write %#llx+%#llx kept from the game's memory", (unsigned long long)a, (unsigned long long)n);
        return 1;
    }
    return 0;
}
static void copy_t2b(const p_copy_b2t *c) {   /* texture -> buffer */
    int trace = totk_texcopy_on() && texcopy_buffer_attempt(c, "CopyTextureToBuffer", 0);
    if (!vk_lag_uniform_private()) vk_ub_flush();
    else vk_ub_touch((uint64_t)(uintptr_t)c->tex->mem, c->tex->L.size);
    vk_tex *x = vk_tex_get(c->tex);
    if (trace && !x) vk_texcopy_image("CopyTextureToBuffer", "src", c->tex, x);
    if (!x) {
        if (trace) totk_texcopy_log(vk_submit_serial(), "phase=replay op=CopyTextureToBuffer result=skip reason=texture-unavailable");
        return;
    }
    int bw, bh, bpb = nvn_fmt_bpb(c->tex->d.format, &bw, &bh);
    if (!bpb) {
        if (trace) totk_texcopy_log(vk_submit_serial(), "phase=replay op=CopyTextureToBuffer result=skip reason=unsupported-format");
        return;
    }
    unsigned level = vlevel(&c->view, c->has_view), layer0 = vlayer(&c->view, c->has_view);
    size_t row = S.copy_stride ? (size_t)S.copy_stride : (size_t)((c->r.width + bw - 1) / bw) * bpb, rows = (c->r.height + bh - 1) / bh, img = row * rows;
    int nz = c->r.depth ? c->r.depth : 1, is3d = vk_tex_is3d(x);
    VkBuffer b; uint64_t o;
    if (!vk_resolve(c->src, img * nz, &b, &o)) {
        if (trace) totk_texcopy_log(vk_submit_serial(), "phase=replay op=CopyTextureToBuffer result=skip reason=buffer-unresolved buffer=%#llx bytes=%zu", (unsigned long long)c->src, img * nz);
        RED(4, "texture-to-buffer copy: the destination is not in a memory pool"); return;
    }
    if (trace) texcopy_buffer_resolved("CopyTextureToBuffer", "dst", c, b, o, row, rows, img, nz);
    if (__builtin_expect(gw_drop(c->src, img * nz), 0)) return;
    vk_gpu_write(c->src, img * nz);
    if (row % bpb || o % 4) {
        if (trace) totk_texcopy_log(vk_submit_serial(), "phase=replay op=CopyTextureToBuffer result=skip reason=buffer-layout-unexpressible row=%zu offset=%#llx", row, (unsigned long long)o);
        RED(4, "texture-to-buffer copy with stride %zu / offset %#llx not expressible", row, (unsigned long long)o); return;
    }
    vk_end_rendering_at(VK_PASS_END_COPY, OP_COPY_T2B);
    if (__builtin_expect(g_pt_on, 0)) { VkDescriptorBufferInfo ptl; if (vk_pt_write(c->src, img * nz, 2, 4, &ptl)) { b = ptl.buffer; o = ptl.offset; } }   /* oot-a122: a landed copy (else memory, after the copies it overlaps) */
    vk_tex_native_read(x);
    if (trace) vk_texcopy_image("CopyTextureToBuffer", "src", c->tex, x);
    VkCommandBuffer cb = vk_cb(); vk_barrier_all(cb);
    if (vk_gpuprof_on) vk_gpuprof_scope_begin(cb, "transfer");
    VkBufferImageCopy r = {.bufferOffset = o, .bufferRowLength = (uint32_t)(row / bpb * bw), .bufferImageHeight = (uint32_t)(rows * bh),
        .imageSubresource = {copy_aspect(x), level, is3d ? 0 : layer0 + c->r.zoffset, is3d ? 1 : (uint32_t)nz},
        .imageOffset = {c->r.xoffset, c->r.yoffset, is3d ? c->r.zoffset : 0}, .imageExtent = {(uint32_t)c->r.width, (uint32_t)c->r.height, is3d ? (uint32_t)nz : 1}};
    vkCmdCopyImageToBuffer(cb, vk_tex_image(x), VK_IMAGE_LAYOUT_GENERAL, b, 1, &r);
    vk_pso_life_external_read(vk_tex_width(x, level), vk_tex_height(x, level), "texture-to-buffer-read");
    vk_fg_extract_memory_write(c->src, img * nz);
    vk_barrier_all(cb);
    if (vk_gpuprof_on) vk_gpuprof_scope_end(cb);
    if (trace) {
        totk_texcopy_log(vk_submit_serial(), "phase=replay op=CopyTextureToBuffer result=success encoded=1 src_serial=%llu bytes=%zu", (unsigned long long)c->tex->serial, img * nz);
        vk_texcopy_readback("CopyTextureToBuffer", "src", c->tex, x, level, layer0, &c->r);
    }
}
static void copy_t2t(const p_copy_t2t *c) {   /* texture -> texture: same size as a copy, scaled as a blit */
    int trace = 0;
    if (totk_texcopy_on()) {
        int sm = totk_texcopy_match(c->src), dm = totk_texcopy_match(c->dst);
        trace = sm || dm;
        if (trace) {
            totk_texcopy_track(c->src); totk_texcopy_track(c->dst);
            totk_texcopy_log(vk_submit_serial(),
                "phase=replay op=CopyTextureToTexture result=attempt src_serial=%llu dst_serial=%llu flags=%#x match_src=%d match_dst=%d",
                (unsigned long long)c->src->serial, (unsigned long long)c->dst->serial, c->flags, sm, dm);
            texcopy_region("CopyTextureToTexture", "src", c->src, &c->sv, c->has_sv, &c->sr);
            texcopy_region("CopyTextureToTexture", "dst", c->dst, &c->dv, c->has_dv, &c->dr);
        }
    }
    if (!vk_lag_uniform_private()) vk_ub_flush();
    else {
        vk_ub_touch((uint64_t)(uintptr_t)c->src->mem, c->src->L.size);
        vk_ub_touch((uint64_t)(uintptr_t)c->dst->mem, c->dst->L.size);
    }
    vk_tex *s = vk_tex_get(c->src), *d = vk_tex_get(c->dst);
    if (trace && (!s || !d)) {
        vk_texcopy_image("CopyTextureToTexture", "src", c->src, s);
        vk_texcopy_image("CopyTextureToTexture", "dst", c->dst, d);
    }
    if (!s || !d) {
        if (trace) totk_texcopy_log(vk_submit_serial(), "phase=replay op=CopyTextureToTexture result=skip reason=texture-unavailable src_resolved=%d dst_resolved=%d", s != NULL, d != NULL);
        return;
    }
    unsigned sl = vlevel(&c->sv, c->has_sv), dl = vlevel(&c->dv, c->has_dv), sz0 = vlayer(&c->sv, c->has_sv), dz0 = vlayer(&c->dv, c->has_dv);
    int s3 = vk_tex_is3d(s), d3 = vk_tex_is3d(d), nz = c->sr.depth ? c->sr.depth : 1;
#ifdef TOTK_OHOS
    p_copy_t2t halved;
    {   /* a BC texture kept from level 1 (vk_res.c TOTK_OHOS_BC_HALF_MB): its level 0 does not exist; a copy of a lower
           level is one level down, at half the offsets and size. Both sides must agree, else the copy is skipped. */
        extern int vk_tex_mipdrop(vk_tex *);
        int ds = vk_tex_mipdrop(s), dd = vk_tex_mipdrop(d);
        if (ds || dd) {
            static _Atomic int said;
            if (ds != dd || sl < (unsigned)ds || dl < (unsigned)dd) {
                if (atomic_fetch_add(&said, 1) < 8) LOGV("[bc-half] texture copy skipped: levels %u->%u, halved %d/%d (ser %llu -> %llu)", sl, dl, ds, dd, (unsigned long long)c->src->serial, (unsigned long long)c->dst->serial);
                return;
            }
            sl -= (unsigned)ds; dl -= (unsigned)dd;
            halved = *c; c = &halved;
            halved.sr.xoffset >>= 1; halved.sr.yoffset >>= 1; halved.sr.width = MAXV(halved.sr.width >> 1, 1); halved.sr.height = MAXV(halved.sr.height >> 1, 1);
            halved.dr.xoffset >>= 1; halved.dr.yoffset >>= 1; halved.dr.width = MAXV(halved.dr.width >> 1, 1); halved.dr.height = MAXV(halved.dr.height >> 1, 1);
        }
    }
#endif
    vk_end_rendering_at(VK_PASS_END_COPY, OP_COPY_T2T);
    vk_tex_native_read(s); vk_tex_native_read(d);
    if (trace) {
        vk_texcopy_image("CopyTextureToTexture", "src", c->src, s);
        vk_texcopy_image("CopyTextureToTexture", "dst", c->dst, d);
    }
    vk_tex_claim(d);
    VkCommandBuffer cb = vk_cb(); vk_barrier_all(cb);
    if (vk_gpuprof_on) vk_gpuprof_scope_begin(cb, "transfer");
    int same = c->sr.width == c->dr.width && c->sr.height == c->dr.height && (c->sr.depth ? c->sr.depth : 1) == (c->dr.depth ? c->dr.depth : 1) && s3 == d3;
    int bwa, bha, bwb, bhb, bpa = nvn_fmt_bpb(c->src->d.format, &bwa, &bha), bpbb = nvn_fmt_bpb(c->dst->d.format, &bwb, &bhb);
    unsigned copied_layers = 0;
    if (same && bpa == bpbb && bwa == bwb && bha == bhb) {
        VkImageCopy r = {.srcSubresource = {copy_aspect(s), sl, s3 ? 0 : sz0 + c->sr.zoffset, s3 ? 1 : (uint32_t)nz}, .srcOffset = {c->sr.xoffset, c->sr.yoffset, s3 ? c->sr.zoffset : 0},
            .dstSubresource = {copy_aspect(d), dl, d3 ? 0 : dz0 + c->dr.zoffset, d3 ? 1 : (uint32_t)nz}, .dstOffset = {c->dr.xoffset, c->dr.yoffset, d3 ? c->dr.zoffset : 0},
            .extent = {(uint32_t)c->sr.width, (uint32_t)c->sr.height, s3 ? (uint32_t)nz : 1}};
        vkCmdCopyImage(cb, vk_tex_image(s), VK_IMAGE_LAYOUT_GENERAL, vk_tex_image(d), VK_IMAGE_LAYOUT_GENERAL, 1, &r);
        copied_layers = (unsigned)nz;
        if (trace) totk_texcopy_log(vk_submit_serial(),
            "phase=replay op=CopyTextureToTexture result=success kind=copy encoded=1 src_serial=%llu dst_serial=%llu layers=%u",
            (unsigned long long)c->src->serial, (unsigned long long)c->dst->serial, copied_layers);
    } else if (!s3 && !d3 && copy_aspect(s) == copy_aspect(d)) {
        VkImageBlit r = {.srcSubresource = {copy_aspect(s), sl, sz0 + c->sr.zoffset, 1}, .srcOffsets = {{c->sr.xoffset, c->sr.yoffset, 0}, {c->sr.xoffset + c->sr.width, c->sr.yoffset + c->sr.height, 1}},
            .dstSubresource = {copy_aspect(d), dl, dz0 + c->dr.zoffset, 1}, .dstOffsets = {{c->dr.xoffset, c->dr.yoffset, 0}, {c->dr.xoffset + c->dr.width, c->dr.yoffset + c->dr.height, 1}}};
        int linear = (c->flags & NVN_COPY_FLAGS_LINEAR_FILTER_BIT) && copy_aspect(s) == VK_IMAGE_ASPECT_COLOR_BIT;
        vkCmdBlitImage(cb, vk_tex_image(s), VK_IMAGE_LAYOUT_GENERAL, vk_tex_image(d), VK_IMAGE_LAYOUT_GENERAL, 1, &r, linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
        copied_layers = 1;
        if (trace) totk_texcopy_log(vk_submit_serial(),
            "phase=replay op=CopyTextureToTexture result=success kind=blit encoded=1 src_serial=%llu dst_serial=%llu layers=%u",
            (unsigned long long)c->src->serial, (unsigned long long)c->dst->serial, copied_layers);
    } else {
        if (trace) totk_texcopy_log(vk_submit_serial(),
            "phase=replay op=CopyTextureToTexture result=skip reason=unsupported-copy src_3d=%d dst_3d=%d src_aspect=%#x dst_aspect=%#x",
            s3, d3, copy_aspect(s), copy_aspect(d));
        RED(4, "texture copy %dx%dx%d -> %dx%dx%d (3D %d -> %d) not done", c->sr.width, c->sr.height, c->sr.depth, c->dr.width, c->dr.height, c->dr.depth, s3, d3);
    }
    vk_barrier_all(cb);
    if (vk_gpuprof_on) vk_gpuprof_scope_end(cb);
    if (copied_layers) vk_tex_native_patch(d, dl, dz0, &c->dr);
    if (vk_fg_extract_active && copied_layers) fg_extract_copy(c, s, d, copied_layers);
    vk_tex_mark_gpu_written(d); probe_write_note('C', d, s, 0);
    if (copied_layers) vk_pso_life_external_write(d, dl, dz0, copied_layers, "image-copy");
    if (copied_layers) vk_pso_life_external_read(vk_tex_width(s, sl), vk_tex_height(s, sl), "texture-copy-read");
    if (trace && copied_layers) {
        NVNcopyRegion sr = c->sr, dr = c->dr;
        sr.depth = dr.depth = (int)copied_layers;
        vk_texcopy_readback("CopyTextureToTexture", "src", c->src, s, sl, sz0, &sr);
        vk_texcopy_readback("CopyTextureToTexture", "dst", c->dst, d, dl, dz0, &dr);
    }
}
/* NVN ClearTexture clears a region of one level (x/y/width/height, layers or 3D slices zoffset/depth) and only the
   channels in its mask. vkCmdClearColorImage/DepthStencilImage clear whole subresources and every channel, so a
   region or a colour channel mask is cleared in a temporary rendering pass on that level: vkCmdClearAttachments with
   the region as its rect, or, for a channel mask, one triangle drawing the value through a pipeline whose
   colorWriteMask is the mask, scissored to the region. Whole unmasked clears keep the image clears. */
static VkPipelineLayout g_clear_mask_layout;
static VkShaderModule g_clear_mask_module[4];   /* vertex; fragment float, uint, int */
static struct { VkFormat format; uint32_t mask; VkSampleCountFlagBits samples; VkPipeline pipeline; } g_clear_mask_pipe[32];
static int g_clear_mask_npipe;
static uint64_t g_clear_region, g_clear_masked;   /* ClearTexture calls that needed the region/mask path (tests) */
#include "vk_clear_mask_spv.h"
static VkPipeline clear_mask_pipeline(VkFormat format, uint32_t mask, VkSampleCountFlagBits samples, int type) {
    for (int i = 0; i < g_clear_mask_npipe; i++)
        if (g_clear_mask_pipe[i].format == format && g_clear_mask_pipe[i].mask == mask && g_clear_mask_pipe[i].samples == samples)
            return g_clear_mask_pipe[i].pipeline;
    if (g_clear_mask_npipe == (int)(sizeof g_clear_mask_pipe / sizeof *g_clear_mask_pipe)) return VK_NULL_HANDLE;
    if (!g_clear_mask_layout) {
        VkPushConstantRange range = {.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT, .size = 16};
        VkPipelineLayoutCreateInfo lci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .pushConstantRangeCount = 1, .pPushConstantRanges = &range};
        if (vkCreatePipelineLayout(V.dev, &lci, NULL, &g_clear_mask_layout)) { g_clear_mask_layout = VK_NULL_HANDLE; return VK_NULL_HANDLE; }
    }
    static const uint32_t *const code[4] = {clear_mask_vert_spv, clear_mask_frag0_spv, clear_mask_frag1_spv, clear_mask_frag2_spv};
    static const size_t size[4] = {sizeof clear_mask_vert_spv, sizeof clear_mask_frag0_spv, sizeof clear_mask_frag1_spv, sizeof clear_mask_frag2_spv};
    const int modules[2] = {0, 1 + type};   /* the vertex shader and this value type's fragment shader */
    for (int k = 0, i; k < 2; k++) if (!g_clear_mask_module[i = modules[k]]) {
        VkShaderModuleCreateInfo sm = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = size[i], .pCode = code[i]};
        if (vkCreateShaderModule(V.dev, &sm, NULL, &g_clear_mask_module[i])) { g_clear_mask_module[i] = VK_NULL_HANDLE; return VK_NULL_HANDLE; }
    }
    VkPipelineShaderStageCreateInfo stages[2] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = g_clear_mask_module[0], .pName = "main"},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = g_clear_mask_module[1 + type], .pName = "main"}};
    VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .polygonMode = VK_POLYGON_MODE_FILL, .lineWidth = 1};
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = samples};
    VkPipelineColorBlendAttachmentState att = {.colorWriteMask = mask};
    VkPipelineColorBlendStateCreateInfo blend = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1, .pAttachments = &att};
    const VkDynamicState dynamic[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 2, .pDynamicStates = dynamic};
    VkPipelineRenderingCreateInfo rendering = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = 1, .pColorAttachmentFormats = &format};
    VkGraphicsPipelineCreateInfo ci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &rendering, .stageCount = 2, .pStages = stages,
        .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vp, .pRasterizationState = &rs,
        .pMultisampleState = &ms, .pColorBlendState = &blend, .pDynamicState = &dy, .layout = g_clear_mask_layout};
    VkPipeline pipeline = VK_NULL_HANDLE;
    if (vk_pcache_graphics(&ci, &pipeline)) return VK_NULL_HANDLE;
    g_clear_mask_pipe[g_clear_mask_npipe++] = (__typeof__(g_clear_mask_pipe[0])){format, mask, samples, pipeline};
    return pipeline;
}
/* One rendering pass per layer/slice over level lv, LOAD/STORE, clearing only the region (and channels). 0 when an
   attachment view or pipeline is unavailable: the caller falls back to the whole-subresource clear. */
static int clear_tex_region(const p_clear_tex *c, vk_tex *x, VkCommandBuffer cb, unsigned lv, unsigned first, unsigned layers,
                            VkImageAspectFlags as, const VkClearValue *value, uint32_t mask) {
    uint32_t w = vk_tex_width(x, lv), h = vk_tex_height(x, lv);
    int f = c->has_view && c->view.set_format ? c->view.format : c->tex->d.format;
    { int bw, bh, vbw, vbh; nvn_fmt_bpb(c->tex->d.format, &bw, &bh); nvn_fmt_bpb(f, &vbw, &vbh); if (bw != vbw || bh != vbh) return 0; }
    int64_t x0 = MAXV(c->r.xoffset, 0), y0 = MAXV(c->r.yoffset, 0);
    int64_t x1 = MINV((int64_t)c->r.xoffset + c->r.width, (int64_t)w), y1 = MINV((int64_t)c->r.yoffset + c->r.height, (int64_t)h);
    if (x1 <= x0 || y1 <= y0) return 1;   /* nothing of the level is inside the region */
    VkRect2D rect = {{(int32_t)x0, (int32_t)y0}, {(uint32_t)(x1 - x0), (uint32_t)(y1 - y0)}};
    int color = !(as & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT));
    VkPipeline pipeline = VK_NULL_HANDLE;
    if (color && mask != 0xf) {
        VkFormat vf; if (!vk_fmt(f, &vf, NULL) || !vf) return 0;
        int cls = vk_fmt_class(f);
        pipeline = clear_mask_pipeline(vf, mask, (VkSampleCountFlagBits)vk_tex_samples(x), cls == 1 ? 1 : cls == 2 ? 2 : 0);
        if (!pipeline) return 0;
    }
    VkImageView views[64]; if (layers > 64) return 0;
    for (unsigned k = 0; k < layers; k++) {
        view_t v = c->has_view ? c->view : (view_t){0};
        v.set_levels = 1; v.base_level = lv; v.nlevels = 1; v.set_layers = 1; v.min_layer = first + k; v.nlayers = 1;
        if (!(views[k] = vk_tex_view(x, c->tex, &v, 1))) return 0;
    }
    for (unsigned k = 0; k < layers; k++) {
        VkRenderingAttachmentInfo a = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = views[k], .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
            .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
        VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, {w, h}}, .layerCount = 1,
            .colorAttachmentCount = color, .pColorAttachments = color ? &a : NULL,
            .pDepthAttachment = as & VK_IMAGE_ASPECT_DEPTH_BIT ? &a : NULL, .pStencilAttachment = as & VK_IMAGE_ASPECT_STENCIL_BIT ? &a : NULL};
        vkCmdBeginRendering(cb, &ri);
        if (pipeline) {
            VkViewport viewport = {0, 0, (float)w, (float)h, 0, 1};
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
            vkCmdSetViewport(cb, 0, 1, &viewport); vkCmdSetScissor(cb, 0, 1, &rect);
            vkCmdPushConstants(cb, g_clear_mask_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16, value->color.uint32);
            vkCmdDraw(cb, 3, 1, 0, 0);
        } else {
            VkClearAttachment ca = {as, 0, *value}; VkClearRect cr = {rect, 0, 1};
            vkCmdClearAttachments(cb, 1, &ca, 1, &cr);
        }
        vkCmdEndRendering(cb);
    }
    if (pipeline) { g_bound_pipe = VK_NULL_HANDLE; g_dyn_dirty = g_pipe_dirty = 1; descriptor_unbind(); }
    return 1;
}
#ifdef TOTK_VK_TESTS
void vk_draw_test_clear_counts(uint64_t out[2]) { out[0] = g_clear_region; out[1] = g_clear_masked; }
#endif
static void clear_tex(const p_clear_tex *c) {   /* clear a texture region with a value, outside any pass */
    if (!vk_lag_uniform_private()) vk_ub_flush();
    else vk_ub_touch((uint64_t)(uintptr_t)c->tex->mem, c->tex->L.size);
    vk_tex *x = vk_tex_get(c->tex); if (!x) return;
    unsigned lv = vlevel(&c->view, c->has_view), l0 = vlayer(&c->view, c->has_view);
    int nz = c->r.depth ? c->r.depth : 1;
    VkImageAspectFlags as = vk_tex_aspect(x);
    int color = !(as & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT));
    uint32_t mask = color ? (uint32_t)c->mask & 0xf : 0xf;
    if (!mask) return;   /* no channel is written */
    int partial = c->r.xoffset || c->r.yoffset || (uint32_t)c->r.width != vk_tex_width(x,lv) || (uint32_t)c->r.height != vk_tex_height(x,lv);
    int native_region = partial && mask == 0xf && vk_tex_native_active(x) && !lv && !l0 && !c->r.zoffset && nz == 1;
    vk_end_rendering_at(VK_PASS_END_CLEAR, OP_CLEAR_TEX);
    vk_tex_native_read(x);
    VkCommandBuffer cb = vk_cb(); vk_barrier_all(cb);
    if (vk_gpuprof_on) vk_gpuprof_scope_begin(cb, "transfer");
    VkImageSubresourceRange rr = {as, lv, 1, vk_tex_is3d(x) ? 0 : l0 + c->r.zoffset, vk_tex_is3d(x) ? 1 : (uint32_t)nz};
    VkClearValue value;
    if (!color) value.depthStencil = (VkClearDepthStencilValue){vk_tex_format(x) == VK_FORMAT_D16_UNORM ? d16_clear(c->c.f[0]) : c->c.f[0], (as & VK_IMAGE_ASPECT_DEPTH_BIT) ? c->c.u[1] : c->c.u[0]};
    else {
        memcpy(&value.color, c->c.u, 16);
        if (!c->is_uint) { int f = c->has_view && c->view.set_format ? c->view.format : c->tex->d.format; int cls = vk_fmt_class(f); if (cls) { for (int i = 0; i < 4; i++) value.color.uint32[i] = (uint32_t)c->c.f[i]; } }
    }
    int region = (partial || mask != 0xf) && !native_region;
    if (region) {
        /* 3D: slices zoffset.. of the level through 2D views; arrays: layers from the view's first layer */
        unsigned first = vk_tex_is3d(x) ? (unsigned)c->r.zoffset : l0 + (unsigned)c->r.zoffset;
        if (clear_tex_region(c, x, cb, lv, first, (unsigned)nz, as, &value, mask)) {
            g_clear_region += partial; g_clear_masked += mask != 0xf;
            rr.baseArrayLayer = first; rr.layerCount = (uint32_t)nz;
        } else {
            region = 0;
            RED(4, "texture clear of a region (%d) or channel mask %#x without a render-target view: cleared whole", partial, mask);
        }
    }
    if (!region) {
        if (!color) vkCmdClearDepthStencilImage(cb, vk_tex_image(x), VK_IMAGE_LAYOUT_GENERAL, &value.depthStencil, 1, &rr);
        else if (native_region) {
            VkRenderingAttachmentInfo a = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                .imageView=vk_tex_view(x,c->tex,c->has_view?&c->view:NULL,1),.imageLayout=VK_IMAGE_LAYOUT_GENERAL,
                .loadOp=VK_ATTACHMENT_LOAD_OP_LOAD,.storeOp=VK_ATTACHMENT_STORE_OP_STORE};
            VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO,
                .renderArea={{0,0},{vk_tex_width(x,0),vk_tex_height(x,0)}},.layerCount=1,
                .colorAttachmentCount=1,.pColorAttachments=&a};
            VkClearAttachment ca={VK_IMAGE_ASPECT_COLOR_BIT,0,value};
            VkClearRect rect={{{c->r.xoffset,c->r.yoffset},{c->r.width,c->r.height}},0,1};
            vkCmdBeginRendering(cb,&ri); vkCmdClearAttachments(cb,1,&ca,1,&rect); vkCmdEndRendering(cb);
        } else vkCmdClearColorImage(cb, vk_tex_image(x), VK_IMAGE_LAYOUT_GENERAL, &value.color, 1, &rr);
    }
    vk_barrier_all(cb);
    if (vk_gpuprof_on) vk_gpuprof_scope_end(cb);
    if (native_region) vk_tex_native_patch(x,lv,l0,&c->r);
    else vk_tex_native_written(x, 0);
    if (vk_fg_extract_active) fg_extract_texture_write(c->tex, lv, rr.baseArrayLayer, rr.layerCount, as, 1, region, 0);
    vk_tex_mark_gpu_written(x); probe_write_note('K', x, NULL, 0);
    vk_pso_life_external_write(x, lv, rr.baseArrayLayer, rr.layerCount, "texture-clear");
}

/* ---------- the walker's other ops ---------- */

void vk_draw_exec(uint32_t op, const void *a) {
    state_init();
    if (R.open && (op==OP_CLEAR_COLOR || op==OP_CLEAR_DS)) R.feedback_written|=0x3ffu;
    if (R.open && R.merge_pending && (op == OP_REPORT_COUNTER || op == OP_RESET_COUNTER || op == OP_RENDER_COND))
        vk_end_rendering_at(VK_PASS_END_QUERY, op);
    if (vk_fg_extract_active && R.open) switch (op) {
        case OP_BIND_STATE: fg_extract_state_change(0); break;
        default: break;
    }
    switch (op) {
        case OP_SET_RT: {
            const p_set_rt *r = a; g_pops.setrt++;
            vk_stage_sig_invalidate(1u << 1);   /* also covers the same-target branch's state copy */
            int pend = R.clear_d || R.clear_s; for (int i = 0; i < 8; i++) pend |= R.clear_c[i];
            if (R.open && !pend && S.has_rt && rt_same(&S.rt, r)) { S.rt = *r; break; }   /* the same targets again: keep the pass */
            vk_end_rendering_at(VK_PASS_END_ATTACHMENTS, 0);
            memset(R.clear_c, 0, sizeof R.clear_c); R.clear_d = R.clear_s = 0;
            S.rt = *r; S.has_rt = 1;
            break;
        }
        case OP_CLEAR_COLOR: { const p_clear_color *c = a;
            if (PD.active) { char l[160]; pd_note(l, snprintf(l, sizeof l, "  clear colour %d mask %#x value %g,%g,%g,%g%s\n", c->index, c->mask, c->rgba[0], c->rgba[1], c->rgba[2], c->rgba[3], S.render_disabled ? " (render disabled: skipped)" : "")); }
            clear_color(a); break; }
        case OP_CLEAR_DS: {
            const p_clear_ds *c=a;
            if (FGX.authority_span.open && c->depth_mask && !S.render_disabled)
                vk_end_rendering_at(VK_PASS_END_COPY,0); /* clear is not an unproven raster write */
            clear_ds(a); break;
        }
        case OP_DRAW: draw(a, NULL); if (serial_draws()) vk_end_rendering_at(VK_PASS_END_DIAGNOSTIC, 3); break;
        case OP_DRAW_IDX: draw(NULL, a); if (serial_draws()) vk_end_rendering_at(VK_PASS_END_DIAGNOSTIC, 3); break;
        case OP_DISPATCH: VK_REC_DO(VK_REC_O_DISPATCH, dispatch(a)); break;
        case OP_COPY_B2T: VK_REC_DO(VK_REC_O_TRANSFER, copy_b2t(a)); break;
        case OP_COPY_T2B: VK_REC_DO(VK_REC_O_TRANSFER, copy_t2b(a)); break;
        case OP_COPY_T2T: VK_REC_DO(VK_REC_O_TRANSFER, copy_t2t(a)); break;
        case OP_CLEAR_TEX: VK_REC_DO(VK_REC_O_TRANSFER, clear_tex(a)); break;
        case OP_BARRIER: {
            int bits; memcpy(&bits, a, 4);
            if (!R.open) { VK_REC_DO(VK_REC_O, vk_barrier_all(vk_cb())); }
            else if ((bits & ~NVN_BARRIER_ORDER_PRIMITIVES_BIT) && pass_barrier_on()) {
                if (feedback_narrow_barrier((uint32_t)bits)) break;
                if (pass_merge_barrier((uint32_t)bits)) break;
                vk_end_rendering_at(bits & NVN_BARRIER_INVALIDATE_TEXTURE_BIT ? VK_PASS_END_TEXTURE_BARRIER : VK_PASS_END_NVN_BARRIER, (uint32_t)bits);
                g_stat_inpass_barriers++;
            }   /* the next draw reopens the pass (load) */
            break; }
        case OP_SCISSOR: S.sc = *(const p_scissor *)a; S.has_sc = 1; break;
        case OP_TF_BIND: { const p_tf_bind *b = a; for (int i = 0; i < b->n && b->first + i < 4; i++) S.tf[b->first + i] = b->r[i]; break; }
        case OP_TF_STATE: { const p_tf_state *s = a;
            { static int said; if (said++ < 12) LOGV("transform feedback: state %d (buffers %#llx+%llu %#llx+%llu %#llx+%llu)", s->kind,
                  (unsigned long long)S.tf[0].address, (unsigned long long)S.tf[0].size, (unsigned long long)S.tf[1].address, (unsigned long long)S.tf[1].size,
                  (unsigned long long)S.tf[2].address, (unsigned long long)S.tf[2].size); }
            if (s->kind == 1) { S.tf_on = 1; memset(S.tf_off, 0, sizeof S.tf_off); }
            else if (s->kind == 4) S.tf_on = 1;
            else {   /* End / Pause: what was captured is read as vertex streams next: the pass ends (the next opens with a barrier) */
                S.tf_on = 0;
                if (R.open) vk_end_rendering_at(VK_PASS_END_NVN_BARRIER, ~0u); else { VK_REC_DO(VK_REC_O, vk_barrier_all(vk_cb())); }
            }
            break; }
        case OP_BIND_PROGRAM: {
            const p_bind_program *b = a;
#if defined(TOTK_GAME_ZA) || defined(TOTK_GAME_OOT)
            { extern const void *g_za_lastbind[4]; extern int g_za_lastmask[4]; extern unsigned g_za_bindn;
              g_za_lastbind[g_za_bindn % 4] = b->prog; g_za_lastmask[g_za_bindn % 4] = b->stages; g_za_bindn++; }
#endif
#if defined(TOTK_GAME_ZA) || defined(TOTK_GAME_OOT)
            /* NVN binds a program for the stages its mask names; the others keep theirs. Trinity binds its compute
               programs (and NULL for the compute stage) between draws without rebinding the graphics one, and the
               draws after were skipped as having no program. A compute-only bind leaves the graphics binding. */
            if (b->stages & NVN_SHADER_STAGE_COMPUTE_BIT) { S.cprog = b->prog; if (b->prog) vk_prog_prepare(b->prog); }
            if (b->stages && !(b->stages & ~NVN_SHADER_STAGE_COMPUTE_BIT)) break;
            /* Per stage, as NVN: Trinity binds its skinning program for the vertex stage alone (mask 1) and then NULL
               for the fragment stage (mask 2); that NULL bind took the whole program away and every transform feedback
               draw was skipped as having no program. A NULL bind without the vertex stage only unbinds the fragment
               stage; a program bound with the fragment stage binds it again. */
            if (!b->prog && b->stages && !(b->stages & NVN_SHADER_STAGE_VERTEX_BIT)) {
                if (b->stages & NVN_SHADER_STAGE_FRAGMENT_BIT) { if (!S.fs_unbound) g_pipe_dirty = 1; S.fs_unbound = 1; }
                break;
            }
            if (b->prog && (b->stages & NVN_SHADER_STAGE_FRAGMENT_BIT)) S.fs_unbound = 0;
#endif
            if (vk_fg_extract_active && R.open && S.prog != b->prog) {
                fg_extract_state_change(1);
            }
            S.prog = b->prog; g_pipe_dirty = 1;
            vk_prog_prepare(S.prog);
            break;
        }
        case OP_BIND_STATE: {
            const p_bind_state *b = a; blend_st bs;
            if (b->kind >= 0 && b->kind < 5) g_pipe_dirty = 1;
            if (vk_fg_extract_active && (b->kind == 2 || b->kind == 5)) fg_extract_state_change(1);
            switch (b->kind) {
                case 0: memcpy(&bs, b->bytes, sizeof bs); S.blend[bs.target & 7] = bs; break;
                case 1: memcpy(&S.color, b->bytes, sizeof S.color); break;
                case 2: memcpy(&S.chmask, b->bytes, sizeof S.chmask); break;
                case 3: memcpy(&S.ms, b->bytes, sizeof S.ms); break;
                case 4: memcpy(&S.poly, b->bytes, sizeof S.poly); break;
                case 5: memcpy(&S.ds, b->bytes, sizeof S.ds); break;
            }
            if (vk_fg_extract_active && R.open) {
                if (b->kind == 2 || FGX.pass.depth_write != (unsigned)(R.dfmt != VK_FORMAT_UNDEFINED && S.ds.dt && S.ds.dw))
                    fg_pass_guard_invalidate(&FGX.guard);
                FGX.pass.depth_write = R.dfmt != VK_FORMAT_UNDEFINED && S.ds.dt && S.ds.dw;
            }
            break;
        }
        case OP_BIND_VATTR: { const p_bind_vattr *b = a; if (b->n >= 0 && b->n <= 16) {
            int changed = !vk_record_enabled() || S.nattr != b->n || memcmp(S.attr, b->st, 4 * (size_t)b->n);
            memcpy(S.attr, b->st, 4 * (size_t)b->n); S.nattr = b->n; g_pipe_dirty = 1;
            if (changed) vk_stage_sig_invalidate(1u << 0);
        } break; }
        case OP_BIND_VSTREAM: { const p_bind_vstream *b = a; if (b->n >= 0 && b->n <= 16) { memcpy(S.stream, b->st, 8 * (size_t)b->n); S.nstream = b->n; g_pipe_dirty = 1; } break; }
        case OP_BIND_VBUF: { const p_bind_vbuf *b = a; if (b->first >= 0 && b->first + b->n <= 16) memcpy(&S.vbuf[b->first], b->r, sizeof *b->r * (size_t)b->n); break; }
        case OP_BIND_UBO: { const p_bind_buf *b = a; if (b->stage >= 0 && b->stage < 6 && b->index >= 0 && b->index < 16) {
            p_bind_buf *old = &S.ubo[b->stage][b->index];
            int camera_bank=(b->stage==0 && (b->index==1 || b->index==5 ||
                b->index==6 || b->index==11)) || (b->stage==1 && b->index==1);
            if (vk_fg_extract_active && R.open && camera_bank &&
                (old->addr != b->addr || old->size != b->size)) {
                fg_pass_guard_invalidate(&FGX.guard);
            }
            if (b->stage < 2 && (old->addr != b->addr || old->size != b->size))
                g_uniform_prepare_dirty[b->stage] |= (uint16_t)(1u << b->index);
            if (vk_record_enabled() && (old->addr != b->addr || old->size != b->size)) g_record_bind_revision[b->stage]++;
            if (b->stage == 5 && (old->addr != b->addr || (uint32_t)old->size != (uint32_t)b->size)) g_driver_revision[0][5]++;
            *old = *b;
        } break; }
        case OP_BIND_SSBO: { const p_bind_buf *b = a; if (b->stage >= 0 && b->stage < 6 && b->index >= 0 && b->index < 16) {
            p_bind_buf *old = &S.ssbo[b->stage][b->index];
            if (vk_record_enabled() && (old->addr != b->addr || old->size != b->size)) g_record_bind_revision[b->stage]++;
            if (old->addr != b->addr || (uint32_t)old->size != (uint32_t)b->size) g_driver_revision[0][b->stage]++;
            *old = *b;
        } break; }
        case OP_BIND_TEX: { const p_bind_handles *b = a; if (b->stage >= 0 && b->stage < 6 && b->first >= 0 && b->first + b->n <= 128) {
            int changed = memcmp(&S.tex[b->stage][b->first], b->h, 8 * (size_t)b->n) || b->first + b->n > S.tex_hi[b->stage];
            if (vk_fg_extract_active && R.open && changed) fg_extract_state_change(1);
            if (vk_record_enabled() && changed) g_record_bind_revision[b->stage]++;
            int signature_changed = !vk_record_enabled();
            for (int i = 0; !signature_changed && i < b->n; i++)
                signature_changed = (S.tex[b->stage][b->first + i] & 0xfffff) != (b->h[i] & 0xfffff);
            for (int i = 0; i < b->n; i++) if ((uint32_t)S.tex[b->stage][b->first + i] != (uint32_t)b->h[i]) { g_driver_revision[1][b->stage]++; break; }
            memcpy(&S.tex[b->stage][b->first], b->h, 8 * (size_t)b->n); if (b->first + b->n > S.tex_hi[b->stage]) S.tex_hi[b->stage] = b->first + b->n;
            if (vk_native_ui_enabled() && vk_native_descriptor_revision()) native_bound_update(b->stage,0,b->first,b->n);
            if (signature_changed) vk_stage_sig_invalidate(1u << b->stage);
        } break; }
        case OP_BIND_SAMP: { const p_bind_handles *b = a; if (b->stage >= 0 && b->stage < 6 && b->first >= 0 && b->first + b->n <= 128) {
            if (vk_record_enabled() && memcmp(&S.samp[b->stage][b->first], b->h, 8 * (size_t)b->n)) g_record_bind_revision[b->stage]++;
            for (int i = 0; i < b->n; i++) if ((uint32_t)S.samp[b->stage][b->first + i] != (uint32_t)b->h[i]) { g_driver_revision[1][b->stage]++; break; }
            memcpy(&S.samp[b->stage][b->first], b->h, 8 * (size_t)b->n);
        } break; }
        case OP_BIND_IMG: { const p_bind_handles *b = a; if (b->stage >= 0 && b->stage < 6 && b->first >= 0 && b->first + b->n <= 32) {
            if (vk_fg_extract_active && R.open && memcmp(&S.img[b->stage][b->first], b->h, 8 * (size_t)b->n))
                fg_extract_state_change(1);
            if (vk_record_enabled() && memcmp(&S.img[b->stage][b->first], b->h, 8 * (size_t)b->n)) g_record_bind_revision[b->stage]++;
            memcpy(&S.img[b->stage][b->first], b->h, 8 * (size_t)b->n);
            if (vk_native_ui_enabled() && vk_native_descriptor_revision()) native_bound_update(b->stage,1,b->first,b->n);
        } break; }
        case OP_STENCIL: { const p_stencil *b = a; for (int f = 0; f < 2; f++) if (b->face & (1 << f)) { if (b->which == 0) S.st_vmask[f] = (uint32_t)b->value_mask; else if (b->which == 1) S.st_wmask[f] = (uint32_t)b->mask; else S.st_ref[f] = (uint32_t)b->ref; } break; }
        case OP_POLY_OFFSET: S.po = *(const p_poly_offset *)a; break;
        case OP_VIEWPORTS: { const p_viewports *b = a;
            if (vk_fg_extract_active && R.open && FGX.pass.camera_addr && b->first == 0 && b->n > 0 &&
                memcmp(S.vp[0], b->v[0], 16)) {
                fg_pass_guard_invalidate(&FGX.guard);
            }
            if (g_overlay_enabled && b->first == 0 && b->n > 0 &&
                (!S.has_vp || memcmp(S.vp[0], b->v[0], 16))) g_overlay_scene_dirty = 1;
            for (int i = 0; i < b->n && b->first + i < 16; i++) memcpy(S.vp[b->first + i], b->v[i], 16);
            S.has_vp = 1; break;
        }
        case OP_DEPTH_RANGE: {
            const p_depth_range *b=a;
            if (vk_fg_extract_active && R.open && (S.dr[0]!=b->n || S.dr[1]!=b->f))
                fg_pass_guard_invalidate(&FGX.guard);
            S.dr[0]=b->n; S.dr[1]=b->f; break;
        }
        case OP_BLEND_COLOR: memcpy(S.blend_color, ((const p_blend_color *)a)->rgba, 16); break;
        case OP_ALPHA_REF: {
            float value=((const p_alpha_ref *)a)->value;
            value=fminf(1,fmaxf(0,value));
            if (memcmp(&S.alpha_ref,&value,4)) g_record_bind_revision[1]++;
            S.alpha_ref=value; break;
        }
        case OP_COPY_STRIDE: S.copy_stride = ((const p_copy_stride *)a)->stride; break;
        case OP_PUSH_CONST: {   /* NVN2 push constants (hle_nvn.c): kept per graphics / compute block, read by the shaders as c[NVN_PUSH_BANK] */
            const p_push_const *pc = a;
            if (pc->offset < NVN_PUSH_SIZE) { uint32_t n = pc->size < NVN_PUSH_SIZE - pc->offset ? pc->size : NVN_PUSH_SIZE - pc->offset; memcpy(g_push_block[pc->compute ? 1 : 0] + pc->offset, pc->data, n); g_push_rev[pc->compute ? 1 : 0]++; }
            { static int said; if (said++ < 4) LOGV("NVN2 push constants: %s block +%#x, %u bytes (bound as c[%d])", pc->compute ? "compute" : "graphics", pc->offset, pc->size, NVN_PUSH_BANK); }
            break;
        }
        case OP_DEPTH_CLAMP: S.depth_clamp = ((const p_flag *)a)->value; g_pipe_dirty = 1; break;
        case OP_RENDER_ENABLE: S.render_disabled = !((const p_flag *)a)->value;
            if (PD.active) { char l[64]; pd_note(l, snprintf(l, sizeof l, "  render enable %d\n", !S.render_disabled)); }
            break;
        case OP_RENDER_COND: {   /* render if the counters at addr / addr + 16 are equal (mode 0) or not (1), with reports still pending counted (nvn_vk.c) */
            const p_render_cond *c = a; uint64_t x = 0, y = 0; int known = 1;
            if (c->addr) { known = vk_counter_value(c->addr, &x); known &= vk_counter_value(c->addr + 16, &y); }
            S.render_disabled = known && (c->mode == 0 ? x != y : x == y);   /* pending real samples: visible, as Metal's cond_eval */
            g_pops.cond++; if (S.render_disabled) g_pops.cond_off++;
            if (PD.active) { char l[200]; pd_note(l, snprintf(l, sizeof l, "  render cond %#llx mode %d: %llu vs %llu -> %s%s\n", (unsigned long long)c->addr, c->mode, (unsigned long long)x, (unsigned long long)y, S.render_disabled ? "off" : "on", known ? "" : " (pending samples: visible)")); }
            break;
        }
        case OP_RASTER_DISCARD: S.raster_discard = ((const p_flag *)a)->value; break;
        case OP_REPORT_COUNTER:
            if (PD.active) { uint64_t t, ad; memcpy(&t, a, 8); memcpy(&ad, (const uint8_t *)a + 8, 8); char l[96]; pd_note(l, snprintf(l, sizeof l, "  report counter type %llu at %#llx\n", (unsigned long long)t, (unsigned long long)ad)); }
            break;
        case OP_RESET_COUNTER: { uint64_t t; memcpy(&t, a, 8); if (t == NVN_COUNTER_TYPE_SAMPLES_PASSED) { g_samples_cum = 0; vk_occlusion_reset(); } break; }
        case OP_SET_TEX_POOL: case OP_SET_SAMP_POOL: break;
        default: break;
    }
}
/* the dump's note of a buffer write made outside the draw stream (nvn_vk.c: inline updates, buffer copies) */
void vk_pd_write_note(const char *what, uint64_t dst, uint64_t n, const void *data) {
    if (!PD.active || !n) return;
    char l[200]; int k = snprintf(l, sizeof l, "  %s %#llx +%#llx", what, (unsigned long long)dst, (unsigned long long)n);
    if (data) for (uint64_t i = 0; i < n && i < 32 && k < (int)sizeof l - 12; i += 8) { uint64_t v = 0; memcpy(&v, (const uint8_t *)data + i, (size_t)MINV(8, n - i)); k += snprintf(l + k, sizeof l - (size_t)k, " %llx", (unsigned long long)v); }
    if (k < (int)sizeof l - 1) { l[k++] = '\n'; l[k] = 0; }
    pd_note(l, k);
}

/* ---------- present: count frames; TOTK_VK_SHOT=<n> saves every n-th presented image as PNG (headless check) ---------- */
static uint64_t g_present_n, g_present_t0, g_last_ticket; static double g_pres_t[64];
static uint32_t crc_table[256];
static uint32_t crc32_(const uint8_t *p, size_t n, uint32_t c) {
    if (!crc_table[1]) for (uint32_t i = 0; i < 256; i++) { uint32_t v = i; for (int k = 0; k < 8; k++) v = v & 1 ? 0xedb88320u ^ (v >> 1) : v >> 1; crc_table[i] = v; }
    c = ~c; for (size_t i = 0; i < n; i++) c = crc_table[(c ^ p[i]) & 0xff] ^ (c >> 8); return ~c;
}

/* ---------- pass dump (TOTK_INPUT_FILE "passdump <n>", n frames from the one after next) ----------
   Every pass's attachments (each colour target and the depth) as PNG files in passdump/f<frame>/, plus index.txt (pass
   number, targets with serial / size / NVN format, draws, first fragment shader). For finding the pass where a picture
   problem first appears. Rows are sampled on the GPU (216 rows per attachment, full width) into one host buffer and
   converted on the CPU at the frame's present; float formats are scaled from their own min..max per channel (so linear
   depth or HDR colour is visible), depth likewise. Slow; a diagnostic only. */
#define PD_ROWS 216
#define PD_COLS 384
#define PD_BUF (1024ull << 20)
typedef struct { int pass, att; VkFormat f; int nvnfmt; uint32_t w, h, rows; VkDeviceSize off, rowbytes; uint64_t serial; unsigned level, layer; uint32_t tb; } pd_img;
static long g_pd_first, g_pd_last;   /* frames (present numbers) to dump */
static char g_pdd[32][240]; static int g_npdd;   /* the open pass's draws (index.txt lists them for passes of up to 32) */
static int pd_active(void);
static void pd_draw_note(const char *fmt, ...) {
    if (!pd_active() || g_npdd >= 32) { if (pd_active()) g_npdd++; return; }
    va_list ap; va_start(ap, fmt); vsnprintf(g_pdd[g_npdd], sizeof g_pdd[0], fmt, ap); va_end(ap); g_npdd++;
}
/* TOTK_VK_PD_SPLIT=<fs prefix>[,...] or d16 (diagnostic, with passdump): during a dumped frame, the pass ends after every draw
   whose fragment shader key starts with one of these (d16: every draw in a pass with colour targets and a DEPTH16 depth),
   so each draw's result is dumped on its own. */
static int pd_split_match(const char *key) {
    static const char *e; static int init; if (!init) { init = 1; e = getenv("TOTK_VK_PD_SPLIT"); }
    if (!e || !*e || !key || !pd_active()) return 0;
    if (!strcmp(e, "d16")) return R.dfmt == VK_FORMAT_D16_UNORM && R.ncolor > 0;   /* every pass with colour targets and a DEPTH16 depth (the deferred lighting) */
    for (const char *p = e; *p; ) { const char *q = strchr(p, ','); size_t l = q ? (size_t)(q - p) : strlen(p); if (l && !strncmp(key, p, l)) return 1; p += l; if (*p == ',') p++; }
    return 0;
}
static VkBuffer g_pd_buf; static VkDeviceMemory g_pd_mem; static uint8_t *g_pd_ptr; static VkDeviceSize g_pd_used;
static pd_img *g_pd; static int g_npd, g_cappd, g_pd_pass; static char *g_pd_idx; static size_t g_pd_idxn, g_pd_idxcap; static int g_pd_full;
void dm_gfx_passdump_next(long n) {
    g_pd_first = (long)g_present_n + 2; g_pd_last = g_pd_first + n - 1;
    LOGV("passdump: frames %ld to %ld into passdump/", g_pd_first, g_pd_last);
}
static int pd_active(void) {
    long f = (long)g_present_n + 1; return g_pd_first && f >= g_pd_first && f <= g_pd_last; }
static void pd_idx(const char *fmt, ...) {
    char line[600]; va_list ap; va_start(ap, fmt); int n = vsnprintf(line, sizeof line, fmt, ap); va_end(ap); if (n < 0) return; if (n >= (int)sizeof line) n = sizeof line - 1;
    if (g_pd_idxn + (size_t)n + 1 > g_pd_idxcap) { g_pd_idxcap = (g_pd_idxcap + (size_t)n + 1) * 2; g_pd_idx = realloc(g_pd_idx, g_pd_idxcap); }
    memcpy(g_pd_idx + g_pd_idxn, line, (size_t)n); g_pd_idxn += (size_t)n; g_pd_idx[g_pd_idxn] = 0;
}
static uint32_t pd_texel_bytes(VkFormat f) {
    switch (f) {
        case VK_FORMAT_R8_UNORM: case VK_FORMAT_R8_SRGB: case VK_FORMAT_R8_UINT: case VK_FORMAT_R8_SINT: case VK_FORMAT_R8_SNORM: case VK_FORMAT_S8_UINT: return 1;
        case VK_FORMAT_R8G8_UNORM: case VK_FORMAT_R8G8_UINT: case VK_FORMAT_R8G8_SNORM: case VK_FORMAT_R16_SFLOAT: case VK_FORMAT_R16_UNORM: case VK_FORMAT_R16_UINT: case VK_FORMAT_D16_UNORM: case VK_FORMAT_R5G6B5_UNORM_PACK16: return 2;
        case VK_FORMAT_R16G16B16A16_SFLOAT: case VK_FORMAT_R16G16B16A16_UNORM: case VK_FORMAT_R16G16B16A16_UINT: case VK_FORMAT_R32G32_SFLOAT: case VK_FORMAT_R32G32_UINT: return 8;
        case VK_FORMAT_R32G32B32A32_SFLOAT: case VK_FORMAT_R32G32B32A32_UINT: return 16;
        default: return 4;
    }
}
static void pd_sample_copy(VkCommandBuffer cb, vk_tex *x, VkImageAspectFlags aspect, VkFormat f, int att, uint64_t serial, int nvnfmt, unsigned level, unsigned layer) {
    if (!x || vk_tex_samples(x) > 1) return;
    vk_tex_native_read(x);
    { int bw, bh; nvn_fmt_bpb(vk_tex_nvn_format(x), &bw, &bh); if (bw > 1 || bh > 1) { pd_idx("    att %d: block-compressed target, not dumped\n", att); return; } }
    if (level >= vk_tex_levels(x)) return;
    uint32_t w = vk_tex_width(x, level), h = vk_tex_height(x, level), rows = MINV(h, (uint32_t)PD_ROWS);
    uint32_t tb = aspect == VK_IMAGE_ASPECT_DEPTH_BIT ? (f == VK_FORMAT_D16_UNORM ? 2 : 4) : aspect == VK_IMAGE_ASPECT_STENCIL_BIT ? 1 : pd_texel_bytes(f);
    VkDeviceSize rb = ((VkDeviceSize)w * tb + 3) & ~(VkDeviceSize)3, need = rb * rows;
    if (!g_pd_buf) {
        VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = PD_BUF, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
        if (vkCreateBuffer(V.dev, &bci, NULL, &g_pd_buf)) { g_pd_full = 1; return; }
        VkMemoryRequirements req; vkGetBufferMemoryRequirements(V.dev, g_pd_buf, &req);
        int t = vk_pick_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
        if (t < 0) t = vk_pick_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
        VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size, .memoryTypeIndex = (uint32_t)t};
        if (t < 0 || vk_memory_allocate(&mai, &g_pd_mem, VK_MEMORY_DIAGNOSTIC)) { vkDestroyBuffer(V.dev, g_pd_buf, NULL); g_pd_buf = VK_NULL_HANDLE; g_pd_full = 1; return; }
        vkBindBufferMemory(V.dev, g_pd_buf, g_pd_mem, 0); vkMapMemory(V.dev, g_pd_mem, 0, VK_WHOLE_SIZE, 0, (void **)&g_pd_ptr); g_pd_used = 0;
    }
    if (g_pd_used + need > PD_BUF) { if (!g_pd_full) LOGV("passdump: buffer full at pass %d", g_pd_pass); g_pd_full = 1; return; }
    int is3d = vk_tex_is3d(x);
    VkBufferImageCopy *rg = calloc(rows, sizeof *rg);
    for (uint32_t r = 0; r < rows; r++) {
        uint32_t y = rows == h ? r : (uint32_t)(((uint64_t)r * h + h / 2) / rows); if (y >= h) y = h - 1;
        rg[r] = (VkBufferImageCopy){.bufferOffset = g_pd_used + r * rb, .bufferRowLength = 0, .imageSubresource = {aspect, level, is3d ? 0 : layer, 1},
            .imageOffset = {0, (int32_t)y, is3d ? (int32_t)layer : 0}, .imageExtent = {w, 1, 1}};
    }
    if (vk_gpuprof_on) vk_gpuprof_scope_begin(cb, "transfer");
    vkCmdCopyImageToBuffer(cb, vk_tex_image(x), VK_IMAGE_LAYOUT_GENERAL, g_pd_buf, rows, rg);
    if (vk_gpuprof_on) vk_gpuprof_scope_end(cb);
    free(rg);
    if (g_npd == g_cappd) { g_cappd = g_cappd ? 2 * g_cappd : 256; g_pd = realloc(g_pd, (size_t)g_cappd * sizeof *g_pd); }
    g_pd[g_npd++] = (pd_img){g_pd_pass, att, aspect == VK_IMAGE_ASPECT_STENCIL_BIT ? VK_FORMAT_S8_UINT : f, nvnfmt, w, h, rows, g_pd_used, rb, serial, level, layer, tb};
    g_pd_used += (need + 255) & ~(VkDeviceSize)255;
}
static void pd_pass(VkCommandBuffer cb) {
    if (!pd_active()) return;
    int k = g_pd_pass;
    pd_idx("f%llu pass %d: %llu draws, fs %s%s\n", (unsigned long long)(g_present_n + 1), k, (unsigned long long)(g_stat_draws - R.draws0), g_pkey[0] ? g_pkey : "-", R.samples > 1 ? ", multisampled (not dumped)" : "");
    for (uint32_t i = 0; i < R.ncolor && i < 8; i++) if (R.ctex[i]) {
        pd_idx("    c%u: ser %llu %ux%u nvnfmt %#x vk %d level %u layer %u\n", i, (unsigned long long)R.c_serial[i], vk_tex_width(R.ctex[i], R.clevel[i]), vk_tex_height(R.ctex[i], R.clevel[i]), R.c_nvnfmt[i], R.cfmt[i], R.clevel[i], R.clayer[i]);
        pd_sample_copy(cb, R.ctex[i], VK_IMAGE_ASPECT_COLOR_BIT, R.cfmt[i], (int)i, R.c_serial[i], R.c_nvnfmt[i], R.clevel[i], R.clayer[i]);
    }
    if (R.dtex) {
        VkImageAspectFlags as = vk_tex_aspect(R.dtex);
        pd_idx("    depth: ser %llu %ux%u vk %d level %u layer %u\n", (unsigned long long)R.d_serial, vk_tex_width(R.dtex, R.dlevel), vk_tex_height(R.dtex, R.dlevel), vk_tex_format(R.dtex), R.dlevel, R.dlayer);
        if (as & VK_IMAGE_ASPECT_DEPTH_BIT) pd_sample_copy(cb, R.dtex, VK_IMAGE_ASPECT_DEPTH_BIT, vk_tex_format(R.dtex), 8, R.d_serial, 0, R.dlevel, R.dlayer);
        if (as & VK_IMAGE_ASPECT_STENCIL_BIT) pd_sample_copy(cb, R.dtex, VK_IMAGE_ASPECT_STENCIL_BIT, vk_tex_format(R.dtex), 9, R.d_serial, 0, R.dlevel, R.dlayer);
    }
    /* what the pass's draws sampled; screen-space passes (few draws) also get their inputs dumped (level 0, layer 0) */
    uint64_t nd = g_stat_draws - R.draws0;
    if (g_npdd <= 32) for (int i = 0; i < g_npdd; i++) pd_idx("%s\n", g_pdd[i]);
    g_npdd = 0;
    for (int i = 0; i < g_npin; i++) {
        vk_tex *x = g_pin[i].x; if (!x) continue;
        pd_idx("    in%d: ser %llu %ux%u nvnfmt %#x vk %d%s\n", i, (unsigned long long)vk_tex_serial(x), g_pin[i].w, g_pin[i].h, g_pin[i].fmt, vk_tex_format(x), vk_tex_is3d(x) ? " 3D" : "");
        if (nd > 16 || vk_tex_is3d(x)) continue;
        VkImageAspectFlags as = vk_tex_aspect(x);
        pd_sample_copy(cb, x, as & VK_IMAGE_ASPECT_COLOR_BIT ? VK_IMAGE_ASPECT_COLOR_BIT : VK_IMAGE_ASPECT_DEPTH_BIT, vk_tex_format(x), 10 + i, vk_tex_serial(x), g_pin[i].fmt, 0, 0);
    }
    vk_barrier_all(cb);
    g_pd_pass++;
}
static float pd_h2f(uint16_t h) {
    uint32_t s = (uint32_t)(h >> 15) << 31, e = (h >> 10) & 31, m = h & 1023, v;
    if (!e) { if (!m) v = s; else { e = 113; while (!(m & 1024)) { m <<= 1; e--; } m &= 1023; v = s | e << 23 | m << 13; } }
    else if (e == 31) v = s | 0xff << 23 | m << 13; else v = s | (e + 112) << 23 | m << 13;
    float f; memcpy(&f, &v, 4); return f;
}
static float pd_uf(uint32_t bits, int mb) {   /* unsigned 11/10-bit float, 5-bit exponent */
    uint32_t e = bits >> mb, m = bits & ((1u << mb) - 1);
    if (!e) return ldexpf((float)m, -14 - mb); if (e == 31) return m ? NAN : INFINITY;
    return ldexpf(1.0f + (float)m / (float)(1u << mb), (int)e - 15);
}
/* texel -> up to 4 channels (float); returns the channel count */
static int pd_decode_texel(VkFormat f, const uint8_t *p, float c[4]) {
    uint32_t u; memcpy(&u, p, 4);
    switch (f) {
        case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_SRGB: case VK_FORMAT_A8B8G8R8_UNORM_PACK32: case VK_FORMAT_A8B8G8R8_SRGB_PACK32:
        case VK_FORMAT_R8G8B8A8_UINT: for (int i = 0; i < 4; i++) c[i] = p[i] / 255.0f; return 4;
        case VK_FORMAT_R8G8B8A8_SNORM: for (int i = 0; i < 4; i++) c[i] = (int8_t)p[i] / 127.0f; return 4;
        case VK_FORMAT_B8G8R8A8_UNORM: case VK_FORMAT_B8G8R8A8_SRGB: c[0] = p[2] / 255.0f; c[1] = p[1] / 255.0f; c[2] = p[0] / 255.0f; c[3] = p[3] / 255.0f; return 4;
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32: case VK_FORMAT_A2B10G10R10_UINT_PACK32: c[0] = (u & 1023) / 1023.0f; c[1] = (u >> 10 & 1023) / 1023.0f; c[2] = (u >> 20 & 1023) / 1023.0f; c[3] = (u >> 30) / 3.0f; return 4;
        case VK_FORMAT_B10G11R11_UFLOAT_PACK32: c[0] = pd_uf(u & 2047, 6); c[1] = pd_uf(u >> 11 & 2047, 6); c[2] = pd_uf(u >> 22, 5); return 3;
        case VK_FORMAT_R16G16B16A16_SFLOAT: for (int i = 0; i < 4; i++) { uint16_t h; memcpy(&h, p + 2 * i, 2); c[i] = pd_h2f(h); } return 4;
        case VK_FORMAT_R16G16_SFLOAT: for (int i = 0; i < 2; i++) { uint16_t h; memcpy(&h, p + 2 * i, 2); c[i] = pd_h2f(h); } return 2;
        case VK_FORMAT_R16_SFLOAT: { uint16_t h; memcpy(&h, p, 2); c[0] = pd_h2f(h); return 1; }
        case VK_FORMAT_R16G16B16A16_UNORM: case VK_FORMAT_R16G16B16A16_UINT: for (int i = 0; i < 4; i++) { uint16_t h; memcpy(&h, p + 2 * i, 2); c[i] = h; } return 4;
        case VK_FORMAT_R16G16_UNORM: case VK_FORMAT_R16G16_UINT: case VK_FORMAT_R16G16_SINT: case VK_FORMAT_R16G16_SNORM: for (int i = 0; i < 2; i++) { uint16_t h; memcpy(&h, p + 2 * i, 2); c[i] = h; } return 2;
        case VK_FORMAT_R16_UNORM: case VK_FORMAT_R16_UINT: case VK_FORMAT_D16_UNORM: { uint16_t h; memcpy(&h, p, 2); c[0] = h; return 1; }
        case VK_FORMAT_R32_SFLOAT: case VK_FORMAT_D32_SFLOAT: case VK_FORMAT_D32_SFLOAT_S8_UINT: memcpy(c, p, 4); return 1;
        case VK_FORMAT_X8_D24_UNORM_PACK32: case VK_FORMAT_D24_UNORM_S8_UINT: c[0] = (float)(u & 0xffffff); return 1;
        case VK_FORMAT_R32_UINT: case VK_FORMAT_R32_SINT: c[0] = (float)u; return 1;
        case VK_FORMAT_R32G32_SFLOAT: memcpy(c, p, 8); return 2;
        case VK_FORMAT_R32G32_UINT: { uint32_t v[2]; memcpy(v, p, 8); c[0] = (float)v[0]; c[1] = (float)v[1]; return 2; }
        case VK_FORMAT_R32G32B32A32_SFLOAT: memcpy(c, p, 16); return 4;
        case VK_FORMAT_R32G32B32A32_UINT: { uint32_t v[4]; memcpy(v, p, 16); for (int i = 0; i < 4; i++) c[i] = (float)v[i]; return 4; }
        case VK_FORMAT_R8_UNORM: case VK_FORMAT_R8_SRGB: case VK_FORMAT_R8_UINT: case VK_FORMAT_S8_UINT: c[0] = p[0] / 255.0f; return 1;
        case VK_FORMAT_R8G8_UNORM: case VK_FORMAT_R8G8_UINT: c[0] = p[0] / 255.0f; c[1] = p[1] / 255.0f; return 2;
        default: c[0] = (u & 255) / 255.0f; c[1] = (u >> 8 & 255) / 255.0f; c[2] = (u >> 16 & 255) / 255.0f; c[3] = (u >> 24) / 255.0f; return 4;
    }
}
static int pd_unorm8(VkFormat f) {
    return f == VK_FORMAT_R8G8B8A8_UNORM || f == VK_FORMAT_R8G8B8A8_SRGB || f == VK_FORMAT_A8B8G8R8_UNORM_PACK32 || f == VK_FORMAT_A8B8G8R8_SRGB_PACK32 ||
           f == VK_FORMAT_B8G8R8A8_UNORM || f == VK_FORMAT_B8G8R8A8_SRGB || f == VK_FORMAT_A2B10G10R10_UNORM_PACK32 || f == VK_FORMAT_R8_UNORM || f == VK_FORMAT_R8G8_UNORM;
}
static void write_png(const char *path, const uint8_t *rgba, uint32_t w, uint32_t h, size_t pitch);
static void pd_frame_end(uint64_t n) {
    if (!g_pd_first || (long)n < g_pd_first || (long)n > g_pd_last) return;
    vk_rec_lock(); vk_flush_wait();
    char dir[64]; mkdir("passdump", 0755); snprintf(dir, sizeof dir, "passdump/f%llu", (unsigned long long)n); mkdir(dir, 0755);
    for (int i = 0; i < g_npd; i++) {
        pd_img *d = &g_pd[i];
        uint32_t cols = MINV(d->w, (uint32_t)PD_COLS), tb = d->tb;
        float *v = malloc((size_t)cols * d->rows * 4 * sizeof *v); int nch = 1;
        float lo[4] = {INFINITY, INFINITY, INFINITY, INFINITY}, hi[4] = {-INFINITY, -INFINITY, -INFINITY, -INFINITY};
        VkFormat cf = d->att == 9 ? VK_FORMAT_S8_UINT : d->f;
        for (uint32_t r = 0; r < d->rows; r++) for (uint32_t x = 0; x < cols; x++) {
            uint32_t sx = cols == d->w ? x : (uint32_t)(((uint64_t)x * d->w + d->w / 2) / cols); if (sx >= d->w) sx = d->w - 1;
            float c[4] = {0, 0, 0, 1}; nch = pd_decode_texel(cf, g_pd_ptr + d->off + r * d->rowbytes + (size_t)sx * tb, c);
            for (int k = 0; k < 4; k++) { v[((size_t)r * cols + x) * 4 + k] = c[k]; if (k < nch && isfinite(c[k])) { if (c[k] < lo[k]) lo[k] = c[k]; if (c[k] > hi[k]) hi[k] = c[k]; } }
        }
        int fixed = pd_unorm8(cf);
        uint8_t *px = malloc((size_t)cols * d->rows * 4);
        for (size_t t = 0; t < (size_t)cols * d->rows; t++) {
            for (int k = 0; k < 4; k++) {
                float c = v[t * 4 + k]; uint8_t o;
                if (k >= nch) o = k == 3 ? 255 : (nch == 1 ? px[t * 4] : 0);
                else if (!isfinite(c)) o = k == 0 ? 255 : k == 1 ? 0 : 255;   /* NaN / inf: magenta */
                else if (fixed) o = (uint8_t)(c < 0 ? 0 : c > 1 ? 255 : c * 255.0f + 0.5f);
                else { float a = lo[k], b = hi[k]; o = b > a ? (uint8_t)((c - a) / (b - a) * 255.0f + 0.5f) : 128; }
                px[t * 4 + k] = o;
            }
            if (nch < 4) px[t * 4 + 3] = 255;
            if (nch == 1) px[t * 4 + 1] = px[t * 4 + 2] = px[t * 4];   /* grey */
            if (nch >= 4 && !fixed) px[t * 4 + 3] = 255;
            if (fixed) px[t * 4 + 3] = 255;   /* alpha shown opaque; its range is in index.txt */
        }
        char path[160]; const char *kind = d->att == 8 ? "depth" : d->att == 9 ? "stencil" : "c";
        if (d->att >= 10) snprintf(path, sizeof path, "%s/p%03d_in%d_ser%llu_%ux%u.png", dir, d->pass, d->att - 10, (unsigned long long)d->serial, d->w, d->h);
        else if (d->att < 8) snprintf(path, sizeof path, "%s/p%03d_c%d_%ux%u.png", dir, d->pass, d->att, d->w, d->h);
        else snprintf(path, sizeof path, "%s/p%03d_%s_%ux%u.png", dir, d->pass, kind, d->w, d->h);
        write_png(path, px, cols, d->rows, (size_t)cols * 4);
        pd_idx("p%03d %s%d: ranges", d->pass, d->att >= 10 ? "in" : d->att < 8 ? "c" : kind, d->att >= 10 ? d->att - 10 : d->att < 8 ? d->att : 0);
        for (int k = 0; k < nch; k++) pd_idx(" [%g, %g]", (double)lo[k], (double)hi[k]);
        pd_idx("%s\n", fixed ? " (unorm: shown as is)" : " (scaled to its range; magenta = NaN / inf)");
        free(v); free(px);
    }
    char ip[96]; snprintf(ip, sizeof ip, "%s/index.txt", dir);
    FILE *f = fopen(ip, "w"); if (f) { if (g_pd_idx) fwrite(g_pd_idx, 1, g_pd_idxn, f); fclose(f); }
    LOGV("passdump: frame %llu, %d passes, %d images in %s%s", (unsigned long long)n, g_pd_pass, g_npd, dir, g_pd_full ? " (buffer full: later passes missing)" : "");
    g_npd = 0; g_pd_pass = 0; g_pd_idxn = 0; if (g_pd_idx) g_pd_idx[0] = 0; g_pd_used = 0; g_pd_full = 0;
    if ((long)n >= g_pd_last) {
        g_pd_first = g_pd_last = 0;
        if (g_pd_buf) { vkUnmapMemory(V.dev, g_pd_mem); vkDestroyBuffer(V.dev, g_pd_buf, NULL); vk_memory_release(g_pd_mem, VK_MEMORY_DIAGNOSTIC); g_pd_buf = VK_NULL_HANDLE; g_pd_mem = VK_NULL_HANDLE; g_pd_ptr = NULL; }
    }
    vk_rec_unlock();
}
static void png_chunk(FILE *f, const char *type, const uint8_t *data, uint32_t n) {
    uint8_t len[4] = {(uint8_t)(n >> 24), (uint8_t)(n >> 16), (uint8_t)(n >> 8), (uint8_t)n}; fwrite(len, 1, 4, f); fwrite(type, 1, 4, f); if (n) fwrite(data, 1, n, f);
    uint32_t c = crc32_((const uint8_t *)type, 4, 0); c = crc32_(data, n, c);
    uint8_t cb[4] = {(uint8_t)(c >> 24), (uint8_t)(c >> 16), (uint8_t)(c >> 8), (uint8_t)c}; fwrite(cb, 1, 4, f);
}
static void write_png(const char *path, const uint8_t *rgba, uint32_t w, uint32_t h, size_t pitch) {   /* uncompressed (stored) deflate: no zlib needed */
    FILE *f = fopen(path, "wb"); if (!f) { RED(4, "cannot write %s", path); return; }
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 13, 10, 26, 10}; fwrite(sig, 1, 8, f);
    uint8_t ihdr[13] = {(uint8_t)(w >> 24), (uint8_t)(w >> 16), (uint8_t)(w >> 8), (uint8_t)w, (uint8_t)(h >> 24), (uint8_t)(h >> 16), (uint8_t)(h >> 8), (uint8_t)h, 8, 6, 0, 0, 0};
    png_chunk(f, "IHDR", ihdr, 13);
    size_t raw_n = (size_t)h * (1 + (size_t)w * 4), blocks = (raw_n + 65534) / 65535;
    uint8_t *z = malloc(2 + raw_n + blocks * 5 + 4), *raw = malloc(raw_n);
    for (uint32_t y = 0; y < h; y++) { raw[y * (1 + (size_t)w * 4)] = 0; memcpy(raw + y * (1 + (size_t)w * 4) + 1, rgba + y * pitch, (size_t)w * 4); }
    size_t o = 0; z[o++] = 0x78; z[o++] = 0x01;
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < raw_n; i++) { a = (a + raw[i]) % 65521; b = (b + a) % 65521; }
    for (size_t at = 0; at < raw_n; at += 65535) {
        size_t n = raw_n - at < 65535 ? raw_n - at : 65535;
        z[o++] = at + n >= raw_n; z[o++] = (uint8_t)n; z[o++] = (uint8_t)(n >> 8); z[o++] = (uint8_t)~n; z[o++] = (uint8_t)(~n >> 8);
        memcpy(z + o, raw + at, n); o += n;
    }
    uint32_t ad = b << 16 | a; z[o++] = (uint8_t)(ad >> 24); z[o++] = (uint8_t)(ad >> 16); z[o++] = (uint8_t)(ad >> 8); z[o++] = (uint8_t)ad;
    png_chunk(f, "IDAT", z, (uint32_t)o); png_chunk(f, "IEND", NULL, 0);
    fclose(f); free(z); free(raw);
}
static void shot(const tex_t *t, uint64_t n, const int crop[4]) {
    vk_tex *x = vk_tex_get(t); if (!x) return;
    uint32_t w = vk_tex_width(x, 0), h = vk_tex_height(x, 0); int32_t cx = 0, cy = 0;
    /* the window crop (top-left origin, nvn_mtl.m present_encode): the game renders below 1080p into a 1080p texture */
    if (crop && crop[2] > 0 && crop[3] > 0 && crop[0] >= 0 && crop[1] >= 0 && (uint32_t)(crop[0] + crop[2]) <= w && (uint32_t)(crop[1] + crop[3]) <= h) {
        cx = crop[0]; cy = crop[1]; w = (uint32_t)crop[2]; h = (uint32_t)crop[3];
    }
    vk_end_rendering_at(VK_PASS_END_DIAGNOSTIC, 4);
    VkCommandBuffer cb = vk_cb(); vk_barrier_all(cb);
    VkBuffer b; VkDeviceSize o; uint8_t *p = vk_transient((size_t)w * h * 4, 16, &b, &o);
    VkBufferImageCopy r = {.bufferOffset = o, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageOffset = {cx, cy, 0}, .imageExtent = {w, h, 1}};
    int bpb = nvn_fmt_bpb(t->d.format, NULL, NULL);
    if (bpb != 4) { RED(2, "screenshot of a format %#x image (not 4 bytes per pixel) skipped", t->d.format); return; }
    if (vk_gpuprof_on) vk_gpuprof_scope_begin(cb, "transfer");
    vkCmdCopyImageToBuffer(cb, vk_tex_image(x), VK_IMAGE_LAYOUT_GENERAL, b, 1, &r);
    if (vk_gpuprof_on) vk_gpuprof_scope_end(cb);
    vk_flush_wait();
    VkFormat f = vk_tex_format(x);
    if (f == VK_FORMAT_B8G8R8A8_UNORM || f == VK_FORMAT_B8G8R8A8_SRGB) for (size_t i = 0; i < (size_t)w * h; i++) { uint8_t c = p[4 * i]; p[4 * i] = p[4 * i + 2]; p[4 * i + 2] = c; }
    for (size_t i = 0; i < (size_t)w * h; i++) p[4 * i + 3] = 255;
    const char *dir = getenv("TOTK_VK_SHOT_DIR"); if (!dir || !*dir) dir = "shots";
    mkdir(dir, 0755);
    char path[1024]; snprintf(path, sizeof path, "%s/frame_%06llu.png", dir, (unsigned long long)n);
    write_png(path, p, w, h, (size_t)w * 4);
    LOGV("saved %s (%ux%u)", path, w, h);
}
#ifdef TOTK_ANDROID_APP
/* ---------- the app's window (frontend_android.c) ----------
   Android default: render the final scale into imported AHardwareBuffers and give them to a SurfaceControl child.
   TOTK_VK_PRESENT=copy or any unavailable AHB capability falls back to host-visible readback and ANativeWindow post.
   Both paths enqueue without waiting on the recording thread; an occupied ring drops display copies, not game work.
   Three presenter buffers are active by default; slack 2 auto-selects four unless explicitly overridden.
   TOTK_PRESENT_SLOTS=1 reserves fixed display slots; copy waits only on this thread, AHB supplies a guarded desired time.
   The AHB path still waits for both GPU completion and the compositor's previous-buffer release before reuse. */
#include <android/native_window.h>
#include <stdatomic.h>
#ifdef __ANDROID__
#include <android/api-level.h>
#include <android/hardware_buffer.h>
#include <android/surface_control.h>
#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <linux/sync_file.h>
#include <sys/ioctl.h>
#include <dlfcn.h>
#include "vk_present_spv.h"
#include "vk_present_rotated_spv.h"
#include "vk_present_fence.h"
#include "vk_present_retire.h"
#endif
#include "vk_present_slots.h"
#include "vk_present_policy.h"
#include "vk_present_job.h"
#include "framegen_slots.h"
void front_frame_presented(void);
enum { NPRES = VK_PRESENT_SLOT_CAPACITY };
static int g_pres_active = VK_PRESENT_SLOT_DEFAULT_BUFFERS;
static int g_pres_guest_output; /* immutable before presenter startup */
/* Explicit experiment arm: 0 retains live frame queries for matched diagnostic A/B. */
static int g_guest_extent_cached = 1, g_guest_extent_diag;
enum { GUEST_QUERY_PRODUCER, GUEST_QUERY_PRESENTER, GUEST_QUERY_ROLES };
static struct { _Atomic uint64_t pairs, total_ns, max_ns; } g_guest_queries[GUEST_QUERY_ROLES];
/* Cached mode queries only on lifecycle events; the explicit legacy A/B arm polls through the timed wrapper. */
static vk_present_extent pres_window_extent(ANativeWindow *window) {
    if (!window) return (vk_present_extent){0};
    int32_t w=ANativeWindow_getWidth(window), h=ANativeWindow_getHeight(window);
    return w>0 && h>0 ? (vk_present_extent){(uint32_t)w,(uint32_t)h} : (vk_present_extent){0};
}
static vk_present_extent pres_frame_window_extent(ANativeWindow *window, unsigned role) {
    if (!window) return (vk_present_extent){0};
    uint64_t start = g_guest_extent_diag ? host_now_ns() : 0;
    vk_present_extent extent = pres_window_extent(window);
    if (g_guest_extent_diag) {
        uint64_t ns = host_now_ns() - start;
        atomic_fetch_add_explicit(&g_guest_queries[role].pairs, 1, memory_order_relaxed);
        atomic_fetch_add_explicit(&g_guest_queries[role].total_ns, ns, memory_order_relaxed);
        uint64_t max = atomic_load_explicit(&g_guest_queries[role].max_ns, memory_order_relaxed);
        while (max < ns && !atomic_compare_exchange_weak_explicit(&g_guest_queries[role].max_ns, &max, ns,
                memory_order_relaxed, memory_order_relaxed)) {}
    }
    return extent;
}
static void pres_extent_report(uint64_t frame) {
    if (!g_pres_guest_output || !g_guest_extent_diag || frame % 300) return;
    static const char *const names[] = {"producer", "presenter"};
    for (unsigned role = 0; role < GUEST_QUERY_ROLES; role++) {
        uint64_t pairs = atomic_exchange_explicit(&g_guest_queries[role].pairs, 0, memory_order_relaxed);
        uint64_t ns = atomic_exchange_explicit(&g_guest_queries[role].total_ns, 0, memory_order_relaxed);
        uint64_t max = atomic_exchange_explicit(&g_guest_queries[role].max_ns, 0, memory_order_relaxed);
        LOGV("[guest-extent] frame=%llu cache=%d role=%s query_pairs=%llu native_calls=%llu total_ms=%.6f max_us=%.3f (approximate 300-frame window; query wall time only)",
            (unsigned long long)frame, g_guest_extent_cached, names[role], (unsigned long long)pairs,
            (unsigned long long)(pairs * 2), ns / 1e6, max / 1e3);
    }
}
typedef struct {
    VkBuffer buf; VkDeviceMemory mem; uint8_t *ptr; VkDeviceSize size; int coherent; _Atomic int busy, credited;
    uint64_t fence, generation, desired_ns, timeline_frame; uint32_t w, h; int swap, zero;
    vk_present_extent guest_window;
    vk_present_rect guest_fit; /* immutable cached geometry for this queued job */
    unsigned rotation; /* immutable clockwise quarter turns; w/h remain logical, AHB is native-oriented */
    int ubwc; /* independent compressed-allocation opt-in, admitted only by a nonidentity root hint */
    uint64_t refresh_ns, copy_open_ns, slot_skipped; uint32_t interval; /* immutable display-slot reservation; zero is legacy */
    uint64_t framegen_real; /* immutable real budget identity, independent of FT */
    runtime_present_timeline_entry display_timeline; /* immutable API tuple, or ID=-1 re-anchor */
    unsigned timeline_role; /* 0=legacy, 1=real, 2=generated; assigned with the reservation */
    int timeline_display; /* immutable window logical-display ID, -1 if attachment raced */
    uint64_t frame, ctr[10];   /* black_check: the frame and the counters it was drawn with */
    runtime_present_prediction prediction;
    runtime_fps_auto_frame auto_frame;
    uint64_t ready_ns; int prediction_done, auto_done, auto_cadence;
    uint64_t source_fence, source_slot; int generated, interpolated, discard;
    uint64_t adaptive_epoch;
    fgs_slot adaptive_slot;
    uint64_t adaptive_token;
    unsigned adaptive_slots, adaptive_eligible;
    unsigned adaptive_real_conflicts; /* an already-applied GEN cannot be retracted */
    uint64_t adaptive_release_ns; /* EARLY_APPLY=1 sticky earlier GEN hold end; zero is r7 */
    unsigned adaptive_latch_kind; /* fga_latch_kind of the last release evaluation */
    uint64_t adaptive_source_ready_ns, adaptive_output_ready_ns; /* GEN decision's CPU-ready observations; trace rows only */
    fgs_real_observation adaptive_real_observation; /* exact backend CPU publication, not GPU time */
    fgs_capacity_observation adaptive_capacity;
    unsigned adaptive_capacity_live_count;
    _Atomic int adaptive_suppressed;
    uint64_t record_done_ns, enqueue_ns, dequeue_ns, own_ready_ns, pair_ready_ns;
    unsigned queue_depth;
    vk_present_job job; /* reset for a NEW produced job, never a same-job YIELD */
    _Atomic uint64_t diag_apply_ns, diag_callback_ns, diag_previous_wait_ns;
} pres_buf;
static pres_buf g_pres[NPRES];
static unsigned char g_pres_qcom[NPRES]; /* immutable generated-picture provenance */
static unsigned char g_pres_lsfg[NPRES]; /* separate from current engine activation */
static int g_pres_trace; /* initialized before the presenter thread starts */
static int g_fg_job_trace; /* logging only; default off, same setting in both ABBA arms */
#ifdef __ANDROID__
static int g_pres_async_retire; /* exact opt-in, immutable before presenter startup */
#endif
static int g_pres_slot_diag; /* FRAMEGEN_DIAG only; no extra clocks/stores in ordinary play */
static int g_pres_timeline;
static unsigned g_pres_floor_ns;
static unsigned g_pres_guard_ns;
static int g_pres_distinct_refreshes;
static runtime_present_timeline_state g_pres_timeline_state;
static _Atomic uint64_t g_timeline_applied[2], g_timeline_deadline_late[2], g_timeline_ready_known[2];
static _Atomic int64_t g_timeline_ready_slack[2], g_timeline_apply_slack[2];
#ifdef __ANDROID__
static void (*g_pres_set_timeline)(ASurfaceTransaction *, int64_t);
#endif
static void pres_slot_diag_reset(pres_buf *b) {
    if (b->adaptive_slots && !b->generated)
        b->adaptive_real_observation=(fgs_real_observation){0};
    if (!g_pres_slot_diag) return;
    b->enqueue_ns=0;
    atomic_store_explicit(&b->diag_apply_ns,0,memory_order_relaxed);
    atomic_store_explicit(&b->diag_callback_ns,0,memory_order_relaxed);
    atomic_store_explicit(&b->diag_previous_wait_ns,0,memory_order_relaxed);
}
static vk_present_interp g_pres_interp = {.retained = -1};
static pthread_mutex_t g_win_mu = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_pres_cv = PTHREAD_COND_INITIALIZER;
static ANativeWindow *g_win; static uint32_t g_win_w, g_win_h, g_win_pw, g_win_ph;   /* buffer size set; the window's own size */
static int g_pres_q[NPRES * 2], g_pres_qh, g_pres_qt;
static uint64_t g_win_generation;
static int g_win_display_id = -1;
static uint64_t g_win_attachment_epoch;
static fg_adaptive_policy g_adaptive_policy;
static fg_adaptive_plan g_adaptive_plan;
static int g_adaptive_presenting=-1; /* protected by g_win_mu */
static fg_adaptive_present_state g_adaptive_publication; /* g_win_mu; committed handoffs only */
/* Scheduler metadata is protected by g_win_mu. Prepared GPU objects are only
 * recorded/dispatched/cancelled by the recorder owner at an empty boundary.
 * Publishing/cancelling metadata NEVER retires their command/source leases. */
static fgs_state g_slot_policy;
static uint64_t g_slot_clock_generation, g_slot_window_generation, g_slot_source_epoch;
typedef struct {
    vk_fg_generation *generation;
    fg_adaptive_policy phase;
    fgs_request request;
    uint64_t frame, epoch, window_generation, real_fence, real_target;
    int real_index, output_index, published, cancelled, dispatching;
} pres_slot_prepared;
static pres_slot_prepared g_slot_prepared[NPRES];
static unsigned g_slot_prepared_count; /* recorder owner only */
/* Recorder-owned pending-template gate. Lifecycle cancellation leaves it set
 * until the recorder has drained every private command/source lease. */
_Atomic int vk_fg_slot_boundary_active;
static int pres_slots_requested(void) {
    static _Atomic int request=-2;
    int value=atomic_load_explicit(&request,memory_order_relaxed);
    if (value==-2) {
        value=runtime_present_adaptive_slots_request(getenv("TOTK_FG_ADAPTIVE_SLOTS"));
        atomic_store_explicit(&request,value,memory_order_relaxed);
    }
    return value!=0 && vk_fg_adaptive();
}
static int pres_slots_desired(const pres_buf *b, uint64_t *desired_ns) {
    if (!desired_ns) return 0;
    if (b->adaptive_slots) {
        if (b->adaptive_slot.target_ns)
            return runtime_present_adaptive_slot_desired(b->desired_ns,b->refresh_ns,desired_ns);
        /* Only an explicitly staged REAL-only fallback may lack a slot. */
        if (b->generated || !b->desired_ns || b->desired_ns>(uint64_t)INT64_MAX) return 0;
    }
    *desired_ns=b->desired_ns;
    return 1;
}
static struct {
    uint64_t first_ns, boundaries, opportunities, dispatched, cancelled;
    uint64_t no_publication, no_request, late, phase, unavailable, gpu_refused;
} g_slot_dispatch_stats;
static _Atomic uint64_t g_slot_on_time_applies, g_slot_late_applies;
static _Atomic uint64_t g_slot_template_refused, g_slot_irrevocable_real_conflicts;
static fgs_publication_history g_slot_applied_generations; /* g_win_mu; actual apply returns */
static _Atomic uint64_t g_slot_late_real_counts[FGS_REAL_READY_TIME_UNKNOWN+1];
static _Atomic uint64_t g_slot_capacity_gen_waits, g_slot_capacity_gen_associations;
static _Atomic uint64_t g_slot_capacity_gen_spans, g_slot_capacity_all_real_waits;
static _Atomic uint64_t g_slot_capacity_snapshot_incomplete;
/* Scheduler selectors, each parsed once and only once the scheduler itself is
 * requested. LATTICE is default-on (absent=on, "0"=r7 path); EARLY_APPLY is
 * opt-in (absent/"0"=r7 path). Any other spelling is off and logged once. */
static int pres_latch_selector(_Atomic int *request, const char *name, const char *meaning,
        int (*parse)(const char *)) {
    if (!pres_slots_requested()) return 0;
    int value=atomic_load_explicit(request,memory_order_relaxed);
    if (value==-2) {
        const char *e=getenv(name);
        int expected=-2;
        value=parse(e);
        if (atomic_compare_exchange_strong_explicit(request,&expected,value,
                memory_order_relaxed,memory_order_relaxed)) {
            if (value==1)
                LOGV("[adaptive-latch] selector=%s value=1 enabled=1 source=%s lag_ns=%lld lag_min_ns=%lld lag_max_ns=%lld deadline_guard_ns=%llu (%s; r7 otherwise)",
                    name,e ? "explicit":"default",(long long)FGA_LATCH_LAG_NS,(long long)FGA_LATCH_LAG_MIN_NS,(long long)FGA_LATCH_LAG_MAX_NS,
                    (unsigned long long)FGA_LATCH_DEADLINE_GUARD_NS,meaning);
            else if (value<0)
                LOGV("[adaptive-latch] selector=%s value=invalid raw=\"%.32s\" enabled=0 (exact \"0\" or \"1\" required; r7 path)",name,e);
        } else value=expected;
    }
    return value==1;
}
/* TOTK_FG_ADAPTIVE_LATTICE: lattice phase re-anchor ONLY; scheduler default. */
static int pres_latch_enabled(void) {
    static _Atomic int request=-2;
    return pres_latch_selector(&request,"TOTK_FG_ADAPTIVE_LATTICE","phase re-anchor without warmup",
        fga_default_on_request);
}
/* TOTK_FG_ADAPTIVE_EARLY_APPLY: SF-deadline earlier GEN release ONLY. */
static int pres_early_enabled(void) {
    static _Atomic int request=-2;
    return pres_latch_selector(&request,"TOTK_FG_ADAPTIVE_EARLY_APPLY","SF-deadline GEN release",
        fga_latch_request);
}
static struct {
    uint64_t first_ns, last_report_ns, gen_applies, early_applies, kinds[FGA_LATCH_KINDS];
    uint64_t reanchors, reanchor_moved, reanchor_refused_pending, reanchor_refused_invalid;
    uint64_t reals, early_displaced_reals;
} g_latch_stats; /* g_win_mu; selector counters, plus the trace-only REAL count */
/* EARLY_APPLY=1 only, from GEN decisions. Sticky per job: once an early release
 * is found it is kept, so the final pre-apply re-check cannot fall back to a
 * later r7 hold (a WAIT there would cancel a staged transaction). Every other
 * guard is still evaluated at each decision by fgs_admit_gen_release. */
static uint64_t pres_early_release_locked(pres_buf *b, const runtime_present_timelines *clock,
        uint64_t now) {
    if (!pres_early_enabled()) return 0;
    if (b->adaptive_release_ns) return b->adaptive_release_ns;
    uint64_t desired;
    if (!runtime_present_adaptive_slot_desired(b->adaptive_slot.target_ns,b->refresh_ns,&desired)) {
        b->adaptive_latch_kind=FGA_LATCH_R7_INVALID;
        return 0;
    }
    fga_latch_release r=fga_latch_release_for(1,&g_slot_policy,b->adaptive_slot,desired,clock,now);
    b->adaptive_latch_kind=r.kind;
    b->adaptive_release_ns=r.release_ns;
    return r.release_ns;
}
/* LATTICE=1 only, while staging a REAL on an unchanged clock/window/epoch. */
static void pres_latch_reanchor_locked(const runtime_present_timelines *clock, const pres_buf *b) {
    const runtime_present_timeline_entry *anchor=&clock->entries[clock->preferred];
    fga_latch_phase phase=fga_latch_phase_check(&g_slot_policy,anchor->expected_ns);
    if (phase.valid && !phase.reanchor) return;
    fgs_reanchor_result r={0};
    fgs_status status=phase.valid ? fgs_reanchor(&g_slot_policy,anchor->expected_ns,
        FGA_LATCH_LAG_NS,clock->refresh_ns,&r) : FGS_INVALID;
    if (status==FGS_OK) {
        g_latch_stats.reanchors++;
        g_latch_stats.reanchor_moved+=r.moved;
        LOGV("[adaptive-latch-reanchor] event=applied frame=%llu anchor_vsync_id=%lld anchor_expected_ns=%llu old_lag_ns=%lld lag_ns=%lld lag_min_ns=%lld lag_max_ns=%lld ordinal=%llu old_generation=%llu generation=%llu old_base_ns=%llu base_ns=%llu period_ns=%llu live=%u moved=%u old_sealed_before=%llu sealed_before=%llu samples=%u ewma_ns=%llu mode=%u reanchors=%llu",
            (unsigned long long)b->frame,(long long)anchor->vsync_id,(unsigned long long)anchor->expected_ns,
            (long long)r.old_lag_ns,(long long)FGA_LATCH_LAG_NS,(long long)FGA_LATCH_LAG_MIN_NS,
            (long long)FGA_LATCH_LAG_MAX_NS,(unsigned long long)r.anchor_ordinal,
            (unsigned long long)r.old_generation,(unsigned long long)r.generation,
            (unsigned long long)r.old_base_ns,(unsigned long long)r.base_ns,(unsigned long long)r.period_ns,
            r.live,r.moved,(unsigned long long)r.old_sealed_before,(unsigned long long)r.sealed_before,
            g_slot_policy.samples,(unsigned long long)g_slot_policy.ewma_ns,(unsigned)g_slot_policy.mode,
            (unsigned long long)g_latch_stats.reanchors);
        return;
    }
    if (status==FGS_WAIT) g_latch_stats.reanchor_refused_pending++;
    else g_latch_stats.reanchor_refused_invalid++;
    if (g_fg_job_trace)
        LOGV("[adaptive-latch-reanchor] event=refused reason=%s frame=%llu anchor_vsync_id=%lld anchor_expected_ns=%llu lag_ns=%lld lag_valid=%u generation=%llu base_ns=%llu period_ns=%llu pending=%u status=%u",
            status==FGS_WAIT ? "gen-pending":"invalid",(unsigned long long)b->frame,
            (long long)anchor->vsync_id,(unsigned long long)anchor->expected_ns,
            (long long)phase.lag_ns,phase.valid,(unsigned long long)g_slot_policy.generation,
            (unsigned long long)g_slot_policy.base_ns,(unsigned long long)g_slot_policy.period_ns,
            g_slot_policy.pending_count,(unsigned)status);
}
/* EARLY_APPLY=1: a REAL that became ready after an EARLY GEN apply but before that
 * GEN's r7 hold would have taken the GEN's slot under r7 (it would have
 * yielded). Counts the conflict the early apply caused; never a decision. */
static void pres_latch_real_conflicts_locked(const pres_buf *b, uint64_t publication_ns) {
    const fgs_real_observation *real=&b->adaptive_real_observation;
    for (unsigned i=0;i<FGS_PUBLICATION_HISTORY;i++) {
        const fgs_applied_generation *g=&g_slot_applied_generations.generated[i];
        if (!g->id.sequence) continue;
        uint64_t hold=fga_latch_r7_hold(&g_slot_policy,g->slot.target_ns);
        if (g->apply_ns>=hold ||
                fgs_publication_late_real(g,*real)!=FGS_REAL_READY_AFTER_GEN_APPLY ||
                real->ready_ns>=hold) continue;
        g_latch_stats.early_displaced_reals++;
        if (g_fg_job_trace)
            LOGV("[adaptive-latch-early] event=real-displaced real_frame=%llu real_ready_ns=%llu real_target_ns=%llu real_apply_return_ns=%llu gen_source_frame=%llu gen_target_ns=%llu gen_apply_return_ns=%llu gen_r7_hold_ns=%llu total=%llu",
                (unsigned long long)b->frame,(unsigned long long)real->ready_ns,
                (unsigned long long)b->adaptive_slot.target_ns,(unsigned long long)publication_ns,
                (unsigned long long)g->source_frame,(unsigned long long)g->slot.target_ns,
                (unsigned long long)g->apply_ns,(unsigned long long)hold,
                (unsigned long long)g_latch_stats.early_displaced_reals);
    }
}
/* After a scheduler apply, still under g_win_mu. apply_ns was read immediately
 * before ASurfaceTransaction_apply. JOB_TRACE rows are passive: the snapshot
 * and period accessors copy values under the display mutex and never wait. */
static void pres_latch_applied_locked(pres_buf *b, uint64_t apply_ns, uint64_t publication_ns,
        uint64_t desired_ns) {
    int lattice=pres_latch_enabled(), early_apply=pres_early_enabled();
    uint64_t target=b->adaptive_slot.target_ns;
    uint64_t r7_hold=b->generated ? fga_latch_r7_hold(&g_slot_policy,target) : 0;
    int early=b->generated && b->adaptive_release_ns && apply_ns<r7_hold;
    if ((lattice || early_apply) && !g_latch_stats.first_ns)
        g_latch_stats.first_ns=g_latch_stats.last_report_ns=publication_ns;
    if (early_apply) {
        if (b->generated) {
            g_latch_stats.gen_applies++;
            g_latch_stats.early_applies+=(unsigned)early;
            g_latch_stats.kinds[b->adaptive_latch_kind<FGA_LATCH_KINDS ? b->adaptive_latch_kind:0]++;
        } else pres_latch_real_conflicts_locked(b,publication_ns);
    }
    if (!b->generated) g_latch_stats.reals++;
    /* CPU-ready values already observed by the decision path: GEN source/output
     * fences from its final pres_adaptive_decide_locked, REAL backend publication. */
    uint64_t source_ready=b->generated ? b->adaptive_source_ready_ns : 0;
    uint64_t output_ready=b->generated ? b->adaptive_output_ready_ns :
        b->adaptive_real_observation.ready_time_known ? b->adaptive_real_observation.ready_ns : 0;
    if (g_fg_job_trace) {
        runtime_present_timelines clock={0};
        runtime_display_period_observation period={0};
        (void)runtime_android_display_period_observe(&period);
        int fresh=runtime_android_adaptive_slot_timelines(&clock) &&
            runtime_present_adaptive_slot_clock_matches(&clock,g_slot_clock_generation,
                g_slot_policy.period_ns,runtime_android_adaptive_slots_display_id(),g_win_display_id);
        fga_latch_match match=fresh ? fga_latch_nearest(&clock,target) : (fga_latch_match){0};
        fga_latch_match latch=fresh ? fga_latch_eligible(&clock,desired_ns,target) : (fga_latch_match){0};
        int64_t sf_period=fresh && clock.count>1 ? (int64_t)((clock.entries[clock.count-1].expected_ns-
            clock.entries[0].expected_ns)/(clock.count-1)) : 0;
        LOGV("[adaptive-latch-phase] role=%s generated=%u frame=%llu token=%llu source_ready_ns=%llu output_ready_ns=%llu apply_ns=%llu publication_ns=%llu target_ns=%llu desired_ns=%llu lattice=%d early_apply=%d lattice_generation=%llu lattice_base_ns=%llu lattice_period_ns=%llu reanchors=%llu slot_generation=%llu ordinal=%llu snapshot=%s snapshot_generation=%llu snapshot_period_ns=%lld match=%s match_vsync_id=%lld match_expected_ns=%llu match_deadline_ns=%llu phase_ns=%lld phase_abs_ns=%llu apply_minus_match_deadline_ns=%lld latch=%s latch_vsync_id=%lld latch_expected_ns=%llu latch_deadline_ns=%llu latch_offset_ns=%lld apply_minus_latch_deadline_ns=%lld effective_period_ns=%llu fence_period_ns=%llu callback_period_ns=%llu fence_calibrated=%u callback_calibrated=%u release_ns=%llu r7_hold_ns=%llu release_kind=%s apply_decision=%s",
            b->generated ? "GEN":"REAL",(unsigned)(b->generated!=0),(unsigned long long)b->frame,
            (unsigned long long)b->adaptive_token,(unsigned long long)source_ready,
            (unsigned long long)output_ready,(unsigned long long)apply_ns,(unsigned long long)publication_ns,
            (unsigned long long)target,(unsigned long long)desired_ns,lattice,early_apply,
            (unsigned long long)g_slot_policy.generation,(unsigned long long)g_slot_policy.base_ns,
            (unsigned long long)g_slot_policy.period_ns,(unsigned long long)g_latch_stats.reanchors,
            (unsigned long long)b->adaptive_slot.generation,(unsigned long long)b->adaptive_slot.ordinal,
            fresh ? "fresh":"none",(unsigned long long)(fresh ? clock.generation : 0),(long long)sf_period,
            match.found ? "nearest":"none",(long long)(match.found ? match.entry.vsync_id : -1),
            (unsigned long long)match.entry.expected_ns,(unsigned long long)match.entry.deadline_ns,
            (long long)match.offset_ns,
            (unsigned long long)(match.offset_ns<0 ? 0-(uint64_t)match.offset_ns : (uint64_t)match.offset_ns),
            (long long)(match.found ? fga_latch_delta(apply_ns,match.entry.deadline_ns) : 0),
            latch.found ? "eligible":"none",(long long)(latch.found ? latch.entry.vsync_id : -1),
            (unsigned long long)latch.entry.expected_ns,(unsigned long long)latch.entry.deadline_ns,
            (long long)latch.offset_ns,
            (long long)(latch.found ? fga_latch_delta(apply_ns,latch.entry.deadline_ns) : 0),
            (unsigned long long)period.effective_ns,(unsigned long long)period.fence_measured_ns,
            (unsigned long long)period.callback_measured_ns,period.fence_calibrated,period.callback_calibrated,
            (unsigned long long)b->adaptive_release_ns,(unsigned long long)r7_hold,
            b->generated ? fga_latch_kind_name((fga_latch_kind)b->adaptive_latch_kind) : "real",
            !b->generated ? "real" : early ? "early":"r7");
        if (!b->generated && g_latch_stats.reals%300==1)
            LOGV("[adaptive-latch-clock] reals=%llu lattice=%d early_apply=%d effective_period_ns=%llu nominal_period_ns=%llu fence_period_ns=%llu fence_calibrated=%u callback_period_ns=%llu callback_calibrated=%u snapshot_period_ns=%lld lattice_period_ns=%llu lattice_base_ns=%llu lattice_generation=%llu reanchors=%llu",
                (unsigned long long)g_latch_stats.reals,lattice,early_apply,(unsigned long long)period.effective_ns,
                (unsigned long long)period.nominal_ns,(unsigned long long)period.fence_measured_ns,
                period.fence_calibrated,(unsigned long long)period.callback_measured_ns,
                period.callback_calibrated,(long long)sf_period,(unsigned long long)g_slot_policy.period_ns,
                (unsigned long long)g_slot_policy.base_ns,(unsigned long long)g_slot_policy.generation,
                (unsigned long long)g_latch_stats.reanchors);
    }
    if ((lattice || early_apply) && publication_ns-g_latch_stats.last_report_ns>=UINT64_C(1000000000)) {
        g_latch_stats.last_report_ns=publication_ns;
        LOGV("[adaptive-latch-summary] lattice=%d early_apply=%d elapsed_ns=%llu gen_applies=%llu early=%llu r7=%llu r7_no_match=%llu r7_late=%llu r7_not_earlier=%llu r7_invalid=%llu reanchors=%llu reanchor_moved=%llu reanchor_refused_pending=%llu reanchor_refused_invalid=%llu early_displaced_reals=%llu (cumulative app handoffs, not SF outcomes)",
            lattice,early_apply,
            (unsigned long long)(publication_ns-g_latch_stats.first_ns),(unsigned long long)g_latch_stats.gen_applies,
            (unsigned long long)g_latch_stats.early_applies,
            (unsigned long long)(g_latch_stats.gen_applies-g_latch_stats.early_applies),
            (unsigned long long)g_latch_stats.kinds[FGA_LATCH_R7_NO_MATCH],
            (unsigned long long)g_latch_stats.kinds[FGA_LATCH_R7_LATE],
            (unsigned long long)g_latch_stats.kinds[FGA_LATCH_R7_NOT_EARLIER],
            (unsigned long long)g_latch_stats.kinds[FGA_LATCH_R7_INVALID],
            (unsigned long long)g_latch_stats.reanchors,(unsigned long long)g_latch_stats.reanchor_moved,
            (unsigned long long)g_latch_stats.reanchor_refused_pending,
            (unsigned long long)g_latch_stats.reanchor_refused_invalid,
            (unsigned long long)g_latch_stats.early_displaced_reals);
    }
}
static void pres_slots_route_locked(const fgs_batch *batch) {
    for (unsigned n=0;n<batch->cancel_count;n++) {
        uint64_t token=batch->cancelled[n].request.token;
        for (unsigned k=0;k<NPRES;k++)
            if (g_slot_prepared[k].request.token==token) g_slot_prepared[k].cancelled=1;
        for (int q=g_pres_qh;q<g_pres_qt;q++) {
            pres_buf *b=&g_pres[g_pres_q[q%(NPRES*2)]];
            if (b->adaptive_token==token)
                atomic_store_explicit(&b->adaptive_suppressed,1,memory_order_release);
        }
        if (g_adaptive_presenting>=0 && g_pres[g_adaptive_presenting].adaptive_token==token)
            atomic_store_explicit(&g_pres[g_adaptive_presenting].adaptive_suppressed,1,memory_order_release);
    }
    for (unsigned n=0;n<batch->request_count;n++) {
        const fgs_request *request=&batch->requests[n];
        int matched=0,source_prepared=0;
        for (unsigned k=0;k<NPRES;k++) {
            pres_slot_prepared *p=&g_slot_prepared[k];
            if (!p->generation || p->cancelled || p->frame!=request->frame ||
                    p->epoch!=request->source_epoch) continue;
            source_prepared=1;
            if (p->request.token) continue;
            p->request=*request; matched=1; break;
        }
        if (!matched) {
            fgs_cancel unused;
            (void)fgs_cancel_gen(&g_slot_policy,request->token,FGS_CANCEL_OWNER,&unused);
            atomic_fetch_add_explicit(&g_slot_template_refused,1,memory_order_relaxed);
            if (g_fg_job_trace) LOGV("[adaptive-slot] event=request-refused frame=%llu token=%llu target_ns=%llu reason=%s",
                (unsigned long long)request->frame,(unsigned long long)request->token,
                (unsigned long long)request->slot.target_ns,
                source_prepared?"additional-child":"no-prepared-template");
        }
    }
}
static void pres_slots_end_locked(void) {
    fgs_batch batch;
    (void)fgs_end(&g_slot_policy,&batch);
    pres_slots_route_locked(&batch);
    for (unsigned k=0;k<NPRES;k++)
        if (g_slot_prepared[k].generation) g_slot_prepared[k].cancelled=1;
}
static pthread_cond_t g_adaptive_commit_cv;
static int g_adaptive_commit_ready; /* initialized once under g_win_mu, only by an adaptive wait */
/* Independent notification lock: never held over an Android/backend call,
 * callback wait, release poll or Vulkan wait. Lock order is window -> notify;
 * the worker takes notify alone, after dropping its retirement queue lock. */
static pthread_mutex_t g_pres_retire_notify_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_pres_retire_notify_cv;
static _Atomic int g_pres_retire_notify_ready;
static void pres_retire_notify(void) {
    if (!atomic_load_explicit(&g_pres_retire_notify_ready,memory_order_acquire)) return;
    pthread_mutex_lock(&g_pres_retire_notify_mu);
    pthread_cond_broadcast(&g_pres_retire_notify_cv);
    pthread_mutex_unlock(&g_pres_retire_notify_mu);
}
static void pres_adaptive_wake_locked(void) {
    if (g_adaptive_commit_ready) pthread_cond_broadcast(&g_adaptive_commit_cv);
    pres_retire_notify();
}
static fg_adaptive_present_job pres_adaptive_job(const pres_buf *b) {
    return (fg_adaptive_present_job){b->desired_ns,b->frame,b->adaptive_epoch,b->generation,b->generated!=0};
}
/* Called before the first capacity wait, and again if backend provenance was
 * initially unavailable. Never replace a missing timestamp with dequeue/now. */
static void pres_slots_observe_real_locked(pres_buf *b, uint64_t now) {
    if (!b->adaptive_slots || b->generated) return;
    fgs_real_observation *real=&b->adaptive_real_observation;
    if (!real->first_attempt_ns) {
        *real=(fgs_real_observation){.frame=b->frame,.source_epoch=b->adaptive_epoch,
            .window_epoch=b->generation,.first_attempt_ns=now};
    }
    if (!real->ready_time_known) {
        uint64_t ready=vk_present_ready_ns(b->fence);
        if (ready && ready<=now) {
            real->ready_ns=ready;
            real->ready_time_known=1;
        }
    }
}
#ifdef __ANDROID__
static unsigned pres_slots_live_transactions(
    fgs_transaction_id live[FGS_PUBLICATION_CONTEXTS]);
#endif
/* A producer initially has a legacy callback-lattice key. Before allowing a
 * scheduler GEN apply, replan every newer READY real on the admitted API-clock
 * lattice. These are provisional queue keys, never publication/retirement.
 * g_win_mu covers the re-key, stable re-sort and final generated admission. */
static int pres_slots_ready_real_locked(const pres_buf *child, uint64_t now,
        fgs_real_plan *known_real) {
    int index=fgs_pending_index(&g_slot_policy,child->adaptive_token);
    if (index<0) return 0;
    fgs_request request=g_slot_policy.pending[index];
    int have=0;
    for (int q=g_pres_qh;q<g_pres_qt;q++) {
        pres_buf *real=&g_pres[g_pres_q[q%(NPRES*2)]];
        if (!real->adaptive_slots || real->generated || real->generation!=child->generation ||
                real->adaptive_epoch!=child->adaptive_epoch || real->frame<=child->frame) continue;
        uint64_t ready=vk_present_ready_ns(real->fence);
        /* A retired ticket whose CPU publication timestamp aged out still
         * wins arbitration; observed-now is not a claimed GPU completion. */
        if (!ready && mtl_wait(real->fence,0)) ready=now;
        fgs_real_observation observation={.frame=real->frame,.source_epoch=real->adaptive_epoch,
            .window_epoch=real->generation,.ready_ns=ready};
        fgs_real_plan plan;
        if (!fgs_publication_real_plan(&g_slot_policy,&request,observation,now,&plan)) continue;
        real->adaptive_slot=plan.slot;
        real->desired_ns=real->copy_open_ns=plan.slot.target_ns;
        real->refresh_ns=g_slot_policy.period_ns; real->interval=1;
        if (!have || plan.slot.ordinal<known_real->slot.ordinal) *known_real=plan;
        have=1;
    }
    if (have) for (int q=g_pres_qh+1;q<g_pres_qt;q++) {
        int item=g_pres_q[q%(NPRES*2)],position=q;
        const pres_buf *real=&g_pres[item];
        while (position>g_pres_qh) {
            int prior=g_pres_q[(position-1)%(NPRES*2)];
            const pres_buf *before=&g_pres[prior];
            if (!fgs_publication_queue_before(real->desired_ns,real->generated,
                    before->desired_ns,before->generated)) break;
            g_pres_q[position%(NPRES*2)]=prior; position--;
        }
        g_pres_q[position%(NPRES*2)]=item;
    }
    return have;
}
static fg_adaptive_present_result pres_adaptive_decide_locked(pres_buf *b) {
    runtime_fps_adaptive_frame stamp=runtime_fps_adaptive_snapshot();
    fg_adaptive_present_context context={.now=host_now_ns(),.epoch=stamp.epoch,
        .generation=g_win_generation,.window=g_win!=NULL,
        .suppressed=atomic_load_explicit(&b->adaptive_suppressed,memory_order_acquire)};
    pres_slots_observe_real_locked(b,context.now);
    if (b->adaptive_slots && !b->generated)
        return context.window && b->generation==context.generation ?
            FG_ADAPTIVE_PRESENT_APPLY:FG_ADAPTIVE_PRESENT_WINDOW;
    if (b->adaptive_slots && b->generated) {
        runtime_present_timelines clock;
        if (!runtime_android_adaptive_slot_timelines(&clock) ||
                !runtime_present_adaptive_slot_clock_matches(&clock,g_slot_clock_generation,
                    b->refresh_ns,runtime_android_adaptive_slots_display_id(),g_win_display_id))
            return FG_ADAPTIVE_PRESENT_SUPPRESSED;
        uint64_t ready=vk_present_ready_ns(b->fence);
        uint64_t source_ready=vk_present_ready_ns(b->source_fence);
        if (!ready || !source_ready || !b->adaptive_token)
            return FG_ADAPTIVE_PRESENT_SUPPRESSED;
        b->adaptive_output_ready_ns=ready; b->adaptive_source_ready_ns=source_ready; /* diagnostics only */
        if (source_ready>ready) ready=source_ready;
        fgs_cancel cancelled;
        if (fgs_mark_ready(&g_slot_policy,b->adaptive_token,ready,&cancelled)!=FGS_OK)
            return FG_ADAPTIVE_PRESENT_SUPPRESSED;
        fgs_request admitted;
        fgs_real_plan known_real;
        int have_real=pres_slots_ready_real_locked(b,context.now,&known_real);
        uint64_t release=pres_early_release_locked(b,&clock,context.now);
        fgs_status status=release ? fgs_admit_gen_release(&g_slot_policy,b->adaptive_token,
            context.now,have_real?&known_real.slot:NULL,release,&admitted) :
            fgs_admit_gen(&g_slot_policy,b->adaptive_token,context.now,
            have_real?&known_real.slot:NULL,&admitted);
        context.release=release;
        if (status==FGS_REAL_COLLISION) return FG_ADAPTIVE_PRESENT_YIELD;
        if (status!=FGS_OK && status!=FGS_WAIT)
            return status==FGS_LATE?FG_ADAPTIVE_PRESENT_EXPIRED:FG_ADAPTIVE_PRESENT_SUPPRESSED;
    }
    for (int q=g_pres_qh;b->generated && !b->adaptive_slots && q<g_pres_qt;q++) {
        const pres_buf *r=&g_pres[g_pres_q[q%(NPRES*2)]];
        if (r->generated || r->generation!=b->generation || r->adaptive_epoch!=b->adaptive_epoch ||
                r->frame<=b->frame || r->desired_ns>b->desired_ns) continue;
        context.queued_real=1;
        if (fg_adaptive_supersedes(b->desired_ns,b->frame,b->adaptive_epoch,r->desired_ns,
                r->frame,r->adaptive_epoch,vk_present_ready_ns(r->fence))) context.ready_real=1;
    }
    return fg_adaptive_present_decide(&g_adaptive_publication,pres_adaptive_job(b),context);
}
static void pres_adaptive_cond_locked(void) {
    if (g_adaptive_commit_ready) return;
    pthread_condattr_t attr;
    int error=pthread_condattr_init(&attr);
    if (error) DIE("adaptive presenter condattr: %d",error);
    error=pthread_condattr_setclock(&attr,CLOCK_MONOTONIC);
    if (!error) error=pthread_cond_init(&g_adaptive_commit_cv,&attr);
    pthread_condattr_destroy(&attr);
    if (error) DIE("adaptive presenter monotonic condition: %d",error);
    g_adaptive_commit_ready=1;
}
static fg_adaptive_present_result pres_adaptive_wait_locked(pres_buf *b) {
    for (;;) {
        fg_adaptive_present_result decision=pres_adaptive_decide_locked(b);
        if (decision!=FG_ADAPTIVE_PRESENT_WAIT) return decision;
        pres_adaptive_cond_locked();
        uint64_t deadline=fg_adaptive_present_hold(b->desired_ns,b->adaptive_release_ns);
        struct timespec absolute={(time_t)(deadline/UINT64_C(1000000000)),
            (long)(deadline%UINT64_C(1000000000))};
        int error=pthread_cond_timedwait(&g_adaptive_commit_cv,&g_win_mu,&absolute);
        if (error && error!=ETIMEDOUT) DIE("adaptive presenter deadline wait: %d",error);
        /* Queue/lifecycle/suppression wakes and spurious wakes arbitrate again;
         * timeout is the pre-slot deadline, never a display callback. */
    }
}
static struct {
    uint64_t real, first_ns, last_ns, generated, generator_refused, phase[4], refused[FG_ADAPTIVE_REASONS];
    _Atomic uint64_t shown, missed, suppressed;
} g_adaptive_stats;
/* The queue remains bounded by presenter ownership. Adaptive inserts by slot,
 * with real before generated at equality; canceled jobs remain until their
 * covering GPU fence retires. Legacy FIFO is unchanged. g_win_mu held. */
static void pres_queue_locked(int index) {
    pres_buf *b=&g_pres[index];
    int position=g_pres_qt++;
    if (b->adaptive_epoch) {
        while (position>g_pres_qh) {
            int prior=g_pres_q[(position-1)%(NPRES*2)];
            pres_buf *p=&g_pres[prior];
            if (!fgs_publication_queue_before(b->desired_ns,b->generated,
                    p->desired_ns,p->generated)) break;
            g_pres_q[position%(NPRES*2)]=prior;
            position--;
        }
        if (!b->generated && g_adaptive_presenting>=0) {
            pres_buf *p=&g_pres[g_adaptive_presenting];
            /* Scheduler jobs re-arbitrate on the admitted API lattice. Their
             * producer callback keys cannot suppress a different-clock GEN. */
            if ((!b->adaptive_slots || !p->adaptive_slots) &&
                fg_adaptive_supersedes(p->desired_ns,p->frame,p->adaptive_epoch,b->desired_ns,
                    b->frame,b->adaptive_epoch,vk_present_ready_ns(b->fence)))
                atomic_store_explicit(&p->adaptive_suppressed,1,memory_order_release);
        }
    }
    g_pres_q[position%(NPRES*2)]=index;
    if (b->adaptive_epoch) pres_adaptive_wake_locked();
}
static int g_win_prerotate, g_win_ubwc, g_win_transform_hint = -1; /* g_win_mu; no hint means the existing identity path */
/* The existing window mutex serializes lifecycle/canvas changes and snapshots.
   Packed atomic publications keep extents allocation-free and independently readable.
   Explicit-width/native-UI paths retain their existing g_win_pw/g_win_ph policy. */
static _Atomic uint64_t g_guest_window_wh, g_guest_fit_wh, g_guest_fit_xy;
static uint32_t g_guest_canvas_w, g_guest_canvas_h;
#ifdef TOTK_VK_TESTS
static _Atomic unsigned g_guest_fit_rebuilds;
unsigned vk_window_test_fit_rebuilds(void) {
    return atomic_load_explicit(&g_guest_fit_rebuilds, memory_order_relaxed);
}
uint64_t vk_window_test_cached_extent(void) {
    return atomic_load_explicit(&g_guest_window_wh, memory_order_relaxed);
}
#endif
static uint64_t pres_pack_extent(uint32_t w, uint32_t h) { return (uint64_t)w << 32 | h; }
static vk_present_extent pres_guest_window(void) {
    uint64_t wh = atomic_load_explicit(&g_guest_window_wh, memory_order_relaxed);
    return (vk_present_extent){(uint32_t)(wh >> 32), (uint32_t)wh};
}
static float g_guest_aspect;   /* g_win_mu: the ratio the cached fit was made for */
/* Caller holds g_win_mu. Compute only after a lifecycle, canvas/crop or shown-ratio change. */
static void pres_guest_fit_refresh(void) {
    vk_present_extent window = pres_guest_window();
    g_guest_aspect = pres_shown_aspect();
    vk_present_rect fit = vk_present_guest_fit(vk_present_shown_width(g_guest_canvas_w, g_guest_canvas_h, g_guest_aspect),
                                               g_guest_canvas_h, window.w, window.h);
    atomic_store_explicit(&g_guest_fit_wh, pres_pack_extent(fit.w, fit.h), memory_order_relaxed);
    atomic_store_explicit(&g_guest_fit_xy, pres_pack_extent((uint32_t)fit.x, (uint32_t)fit.y), memory_order_relaxed);
#ifdef TOTK_VK_TESTS
    atomic_fetch_add_explicit(&g_guest_fit_rebuilds, 1, memory_order_relaxed);
#endif
}
static vk_present_rect pres_guest_fit_cached(uint32_t canvas_w, uint32_t canvas_h) {
    if (canvas_w != g_guest_canvas_w || canvas_h != g_guest_canvas_h || pres_shown_aspect() != g_guest_aspect) {
        g_guest_canvas_w = canvas_w; g_guest_canvas_h = canvas_h;
        pres_guest_fit_refresh();
    }
    uint64_t wh = atomic_load_explicit(&g_guest_fit_wh, memory_order_relaxed);
    uint64_t xy = atomic_load_explicit(&g_guest_fit_xy, memory_order_relaxed);
    return (vk_present_rect){(int32_t)(xy >> 32), (int32_t)xy, (uint32_t)(wh >> 32), (uint32_t)wh};
}
static _Atomic uint64_t g_pres_shown, g_pres_dropped, g_pres_cancelled;
static _Atomic uint64_t g_fg_handoffs, g_fg_expired;
static _Atomic uint64_t g_fg_ready, g_fg_generated, g_fg_accepted, g_fg_pending, g_fg_no_buffer;
static _Atomic uint64_t g_fg_expiry[FRAMEGEN_CHILD_REASONS];
static _Atomic uint64_t g_hybrid_bins[6], g_hybrid_missing;
static struct {
    uint64_t pair[VK_INTERP_REASONS];
    uint64_t no_buffer, source_shape, generator_refused, recorded;
} g_fg_admission;
/* Queued slot credit bounds producer lead, not GPU/compositor ownership. It
 * retires at copy post acknowledgement or CURRENT AHB transaction completion;
 * busy and the existing release fences retain their separate lifetime rules. */
static void pres_credit_release(pres_buf *b) {
    if (b && b->refresh_ns && atomic_exchange_explicit(&b->credited, 0, memory_order_relaxed))
        runtime_android_present_credit(-1);
}
static void pres_predict_cancel(pres_buf *b) {
    if (b->prediction.id && !b->prediction_done) {
        runtime_android_predict_cancel(&b->prediction); b->prediction_done = 1;
    }
}
static void pres_capacity_trace(const pres_buf *b,const char *event) {
    if (!g_fg_job_trace) return;
    LOGV("[framegen-capacity] source-frame=%llu generated=%d target-ns=%llu generation=%llu event=%s now-ns=%llu capacity-wait-start-ns=%llu capacity-wait-end-ns=%llu count=%u async=%u",
        (unsigned long long)b->frame,b->generated!=0,(unsigned long long)b->desired_ns,
        (unsigned long long)b->generation,event,(unsigned long long)host_now_ns(),
        (unsigned long long)b->job.capacity_wait_start_ns,(unsigned long long)b->job.capacity_wait_end_ns,
        b->job.capacity_wait_count,b->job.retire_async);
}
static void pres_capacity_begin_live(pres_buf *b, const fgs_transaction_id *live,
        unsigned live_count) {
    uint64_t now=host_now_ns();
    pres_slots_observe_real_locked(b,now);
    if (!b->job.capacity_wait_start_ns) b->job.capacity_wait_start_ns=now;
    b->job.capacity_wait_end_ns=0; b->job.capacity_wait_count++;
    if (b->adaptive_slots && !b->generated) {
        b->adaptive_capacity_live_count=live_count;
        fgs_publication_capacity_begin(&b->adaptive_capacity,&g_slot_applied_generations,
            live,live_count,now);
    }
    pres_capacity_trace(b,"start");
}
#ifdef __ANDROID__
static void pres_capacity_begin(pres_buf *b) {
    fgs_transaction_id live[FGS_PUBLICATION_CONTEXTS];
    unsigned count=b->adaptive_slots && !b->generated ? pres_slots_live_transactions(live):0;
    pres_capacity_begin_live(b,live,count);
}
#endif
static void pres_capacity_end(pres_buf *b) {
    uint64_t end=host_now_ns();
    b->job.capacity_wait_end_ns=end;
    pres_capacity_trace(b,"end");
    if (!b->adaptive_slots || b->generated) return;
    const fgs_capacity_observation *wait=&b->adaptive_capacity;
    unsigned full=b->adaptive_capacity_live_count==FGS_PUBLICATION_CONTEXTS;
    if (!full) atomic_fetch_add_explicit(&g_slot_capacity_snapshot_incomplete,1,memory_order_relaxed);
    else atomic_fetch_add_explicit(wait->count ? &g_slot_capacity_gen_waits :
        &g_slot_capacity_all_real_waits,1,memory_order_relaxed);
    for (unsigned i=0;i<wait->count;i++) {
        const fgs_applied_generation *g=&wait->generated[i];
        int spans=fgs_publication_capacity_spans_target(wait,i,end);
        atomic_fetch_add_explicit(&g_slot_capacity_gen_associations,1,memory_order_relaxed);
        if (spans) atomic_fetch_add_explicit(&g_slot_capacity_gen_spans,1,memory_order_relaxed);
        if (g_fg_job_trace)
            LOGV("[adaptive-slot-capacity] event=gen-association real_frame=%llu first_attempt_ns=%llu real_ready_ns=%llu ready_provenance=%s tx_index=%u tx_sequence=%llu tx_generation=%llu gen_source_frame=%llu gen_target_ns=%llu gen_apply_return_ns=%llu wait_start_ns=%llu wait_end_ns=%llu wait_span_ns=%llu spans_target=%u live_contexts=%u full_snapshot=%u sf_outcome=UNKNOWN",
                (unsigned long long)b->frame,
                (unsigned long long)b->adaptive_real_observation.first_attempt_ns,
                (unsigned long long)b->adaptive_real_observation.ready_ns,
                b->adaptive_real_observation.ready_time_known?"backend-cpu-fence-publication":"UNKNOWN",
                g->id.index,(unsigned long long)g->id.sequence,(unsigned long long)g->id.generation,
                (unsigned long long)g->source_frame,(unsigned long long)g->slot.target_ns,
                (unsigned long long)g->apply_ns,(unsigned long long)wait->start_ns,
                (unsigned long long)end,(unsigned long long)(end>=wait->start_ns?end-wait->start_ns:0),
                (unsigned)spans,b->adaptive_capacity_live_count,full);
    }
    if (g_fg_job_trace)
        LOGV("[adaptive-slot-capacity] event=wait-summary real_frame=%llu live_contexts=%u gen_associations=%u full_snapshot=%u gen_waits_total=%llu gen_associations_total=%llu spans_target_total=%llu all_real_waits_total=%llu incomplete_snapshots_total=%llu sf_outcome=UNKNOWN",
            (unsigned long long)b->frame,b->adaptive_capacity_live_count,wait->count,full,
            (unsigned long long)atomic_load_explicit(&g_slot_capacity_gen_waits,memory_order_relaxed),
            (unsigned long long)atomic_load_explicit(&g_slot_capacity_gen_associations,memory_order_relaxed),
            (unsigned long long)atomic_load_explicit(&g_slot_capacity_gen_spans,memory_order_relaxed),
            (unsigned long long)atomic_load_explicit(&g_slot_capacity_all_real_waits,memory_order_relaxed),
            (unsigned long long)atomic_load_explicit(&g_slot_capacity_snapshot_incomplete,memory_order_relaxed));
}
static void pres_job_trace(const pres_buf *b,const char *outcome) {
    if (!g_fg_job_trace || !b->generated) return;
    uint64_t decision_ns=host_now_ns();
    char line[640];
    fg_job_trace_format(line,sizeof line,(fg_job_trace){.frame=b->frame,
        .submit=b->record_done_ns,.pickup=b->dequeue_ns,
        .source_ready=b->pair_ready_ns,.output_ready=b->own_ready_ns,
        .target=b->desired_ns,.decision=decision_ns},
        b->adaptive_epoch ? "adaptive" : "locked",outcome);
    LOGV("%s",line);
    pres_capacity_trace(b,"terminal");
    LOGV("[adaptive-dispatch-stage] stage=presenter-terminal source=%llu output=%u source_ticket=%llu output_ticket=%llu record_done_ns=%llu enqueue_ns=%llu pickup_ns=%llu source_cpu_ready_ns=%llu output_cpu_ready_ns=%llu final_gpu_done_ns=0 final_gpu_done_authority=UNKNOWN-not-exported target_ns=%llu decision_ns=%llu outcome=%s clock=CLOCK_MONOTONIC ready_authority=CPU-publication-after-fence-and-posts",
        (unsigned long long)b->frame,(unsigned)(b-g_pres),
        (unsigned long long)b->source_fence,(unsigned long long)b->fence,
        (unsigned long long)b->record_done_ns,(unsigned long long)b->enqueue_ns,
        (unsigned long long)b->dequeue_ns,(unsigned long long)b->pair_ready_ns,
        (unsigned long long)b->own_ready_ns,(unsigned long long)b->desired_ns,
        (unsigned long long)decision_ns,outcome);
}
/* Every recorded child has exactly one terminal outcome. GPU completion
 * refusal (budget/authority) is distinct from missing the child's vsync. */
static void pres_child_expire(pres_buf *b, framegen_child_reason reason) {
    if (b->adaptive_epoch) {
        pthread_mutex_lock(&g_win_mu);
        if (g_adaptive_presenting==(int)(b-g_pres)) g_adaptive_presenting=-1;
        pthread_mutex_unlock(&g_win_mu);
        if (reason!=FRAMEGEN_CHILD_OUTPUT)
            atomic_fetch_add_explicit(&g_adaptive_stats.missed,1,memory_order_relaxed);
    }
    if (g_pres_trace) LOGV("[present-drop] source_frame=%llu kind=%s desired_ns=%llu record_done_ns=%llu enqueue_ns=%llu dequeue_ns=%llu own_ready_ns=%llu pair_ready_ns=%llu now_ns=%llu reason=%u queue_depth=%u",
        (unsigned long long)b->frame,b->generated ? (b->interpolated ? "interp" : "extrap") : "real",
        (unsigned long long)b->desired_ns,(unsigned long long)b->record_done_ns,
        (unsigned long long)b->enqueue_ns,(unsigned long long)b->dequeue_ns,
        (unsigned long long)b->own_ready_ns,(unsigned long long)b->pair_ready_ns,
        (unsigned long long)host_now_ns(),(unsigned)reason,b->queue_depth);
    pres_job_trace(b,reason==FRAMEGEN_CHILD_DEADLINE ? "deadline" :
        reason==FRAMEGEN_CHILD_COMPLETION ? "completion-refused" : "expired");
    if (b->generated) {
        (void)vk_fg_complete((unsigned)(b-g_pres)); /* idempotent; own/source fences already retired */
        g_fg_expired++; g_fg_expiry[reason]++; g_fg_pending--;
        if (reason == FRAMEGEN_CHILD_SOURCE_MISSING || reason == FRAMEGEN_CHILD_SOURCE_LATE ||
            (b->interpolated && reason == FRAMEGEN_CHILD_DEADLINE)) {
            vk_fg_late();
            if (g_pres_qcom[b-g_pres]) vk_fg_qcom_late();
            if (g_pres_lsfg[b-g_pres]) vk_fg_lsfg_late();
        }
    }
    g_pres_cancelled++; pres_predict_cancel(b); pres_credit_release(b);
    atomic_store(&b->busy, 0);
}
static void pres_adaptive_expire(pres_buf *b,fg_adaptive_present_result decision) {
    if (decision==FG_ADAPTIVE_PRESENT_SUPPRESSED)
        atomic_fetch_add_explicit(&g_adaptive_stats.suppressed,1,memory_order_relaxed);
    pres_child_expire(b,decision==FG_ADAPTIVE_PRESENT_WINDOW ? FRAMEGEN_CHILD_WINDOW :
        decision==FG_ADAPTIVE_PRESENT_EXPIRED ? FRAMEGEN_CHILD_DEADLINE : FRAMEGEN_CHILD_OUTPUT);
}
static void pres_child_handoff(void) {
    g_fg_handoffs++; g_fg_pending--;
}
static void pres_predict_feedback(pres_buf *b, unsigned path, uint64_t ready_ns,
        uint64_t preparation_ns, uint64_t allowance_ns) {
    if (b->auto_frame.id && !b->auto_done && b->ready_ns) {
        runtime_android_fps_auto_feedback(&b->auto_frame, b->ready_ns, preparation_ns, allowance_ns);
        b->auto_done = 1;
    }
    if (!b->prediction.id || b->prediction_done) return;
    /* C joins acquire release to independent GPU/publication readiness, then
     * appends only measured copy preparation. Presenter FIFO residence and its
     * explicit slot hold are deliberately absent, even if prep ran later. */
    uint64_t release = b->prediction.release_ns;
    if (!b->ready_ns || !release || b->ready_ns < release || ready_ns < b->ready_ns ||
        preparation_ns > UINT64_MAX - (b->ready_ns - release)) {
        static uint64_t missing; missing++;
        if (missing <= 8 || missing % 300 == 0)
            LOGV("window predict: invalid/missing independent readiness, ticket=%llu plan=%llu ready=%llu release=%llu; cancelled total=%llu (no presenter-dequeue fallback)",
                 (unsigned long long)b->fence, (unsigned long long)b->prediction.id,
                 (unsigned long long)b->ready_ns, (unsigned long long)release, (unsigned long long)missing);
        pres_predict_cancel(b); return;
    }
    runtime_android_predict_feedback(&b->prediction, path, ready_ns,
        b->ready_ns - release + preparation_ns, allowance_ns);
    b->prediction_done = 1;
}
/* This snapshot never waits for a callback. Reserve before enqueue so any
 * producer scheduling cost remains before timeline handoff, not presenter
 * delay. Only the presenter may block on the display clock. */
static void pres_slot_reserve(pres_buf *b, double requested_period) {
    static int on = -1, logged, unavailable;
    static vk_present_slots slots;
    static uint64_t count, late, skipped, worst_late;
    static int hybrid_was_interpolating;
    if (on < 0) { const char *e = getenv("TOTK_PRESENT_SLOTS"); on = e && !strcmp(e, "1"); }
    if (!on) return;
    uint64_t callback = 0, refresh = 0;
    if (!runtime_android_present_clock(&callback, &refresh)) refresh = 0;
    uint64_t now = host_now_ns();
    if (vk_fg_adaptive()) {
        static uint64_t last_real, generation;
        if (!callback || refresh<8000000 || refresh>18000000 || callback>now) return;
        if (generation!=b->generation) { last_real=0; generation=b->generation; }
        uint64_t target=fg_adaptive_first_slot(callback,refresh,now);
        if (target && target<=last_real) target=fg_adaptive_first_slot(last_real,refresh,now);
        if (!target) return;
        b->desired_ns=b->copy_open_ns=last_real=target;
        b->refresh_ns=refresh; b->interval=1;
        return;
    }
    int hybrid_interpolating=runtime_fps_hybrid_selected() && runtime_fps_hybrid_interpolating();
    /* A native60 lattice can be almost one native tick behind now. Real30
     * cannot catch that phase up while paced, so seed its first full-period
     * lead like fixed interpolation. Keep desired_ns as the queued-target
     * floor: no previously queued native/child picture is retimed/overtaken. */
    if (hybrid_interpolating && !hybrid_was_interpolating) slots.refresh_ns=0;
    hybrid_was_interpolating=hybrid_interpolating;
    vk_present_slot slot = vk_present_slots_reserve(&slots, 1, callback, refresh,
        vk_present_slots_interval(requested_period, refresh), now);
    if (slot.mode==VK_PRESENT_SLOTS_ACTIVE && runtime_fps_hybrid_selected() &&
        !runtime_fps_hybrid_interpolating() && g_pres_interp.retained>=0 &&
        slot.desired_ns<=vk_present_interp_child_floor(&g_pres_interp))
        slot=vk_present_slots_reserve(&slots,1,callback,refresh,slot.interval,now);
    if (slot.mode != VK_PRESENT_SLOTS_ACTIVE) {
        if (!unavailable) { LOGV("window present slots: display clock unavailable; using legacy presentation"); unavailable = 1; }
        return;
    }
    b->desired_ns = slot.desired_ns; b->copy_open_ns = slot.copy_open_ns;
    b->refresh_ns = slot.refresh_ns; b->interval = slot.interval;
    b->slot_skipped = slot.skipped;
    if (!logged) {
        LOGV("window present slots: requested %.3f ms, interval %u x refresh %.3f ms, initial lead %.3f ms; never deliberately early: copy post floor=slot, AHB desired=slot",
             requested_period * 1e3, slot.interval, slot.refresh_ns / 1e6,
             slot.interval * slot.refresh_ns / 1e6);
        logged = 1;
    }
    uint64_t delay = now > slot.desired_ns ? now - slot.desired_ns : 0;
    late += delay != 0; skipped += slot.skipped; if (delay > worst_late) worst_late = delay;
    if (++count % 300 == 0) {
        LOGV("window present slots, last 300 reservations: %llu late when reserved, worst %.3f ms; %llu whole phase slots skipped (not scanout measurements)",
             (unsigned long long)late, worst_late / 1e6, (unsigned long long)skipped);
        late = skipped = worst_late = 0;
    }
}
/* Only admitted locked AHB pairs use display-provided expected times and IDs.
 * Selection happens before enqueue; neither the presenter nor callbacks retime
 * an owned reservation. A transient short/stale snapshot may re-anchor to a
 * fresh legacy callback, but never invent an API vsync ID. */
static void pres_timeline_pair(vk_present_interp_pair *pair, const pres_buf *source,
        runtime_present_timeline_entry *real, runtime_present_timeline_entry *child) {
    if (!g_pres_timeline || !source->zero || pair->real < 0 ||
        (pair->reason != VK_INTERP_READY && pair->reason != VK_INTERP_LATE)) return;
    if (!runtime_present_timeline_display_allowed(runtime_android_present_timeline_display_id(),
            source->timeline_display)) {
        static uint64_t logged_generation=UINT64_MAX;
        if (logged_generation!=source->generation) {
            LOGV("[present-timeline] display guard: source=%d window=%d generation=%llu; legacy reservation/waits retained",
                runtime_android_present_timeline_display_id(),source->timeline_display,
                (unsigned long long)source->generation);
            logged_generation=source->generation;
        }
        return;
    }
    runtime_present_timelines snapshot;
    runtime_present_timeline_pair selected;
    uint64_t now=host_now_ns();
    int api=runtime_android_present_timelines(&snapshot) &&
        runtime_present_timeline_choose(&g_pres_timeline_state,&snapshot,now,
            vk_fg_lead_refreshes(),source->generation,g_pres_distinct_refreshes,&selected);
    int anchored=api;
    if (!anchored) {
        uint64_t callback=0,refresh=0;
        anchored=runtime_android_present_clock(&callback,&refresh) &&
            runtime_present_timeline_reanchor(&g_pres_timeline_state,callback,refresh,now,
                vk_fg_lead_refreshes(),source->generation,&selected);
    }
    static uint64_t pairs,chosen,fallback,unavailable,delay_sum,delay_min=UINT64_MAX,delay_max;
    pairs++; chosen+=api; fallback+=anchored && !api; unavailable+=!anchored;
    if (anchored) {
        *real=selected.real; *child=selected.child;
        pair->slot.desired_ns=pair->slot.copy_open_ns=selected.real.expected_ns;
        pair->slot.refresh_ns=selected.refresh_ns;
        pair->slot.interval=2; pair->slot.mode=VK_PRESENT_SLOTS_ACTIVE;
        pair->midpoint_ns=selected.child.expected_ns; pair->reason=VK_INTERP_READY;
        uint64_t delay=selected.real.expected_ns-now;
        delay_sum+=delay; if(delay<delay_min)delay_min=delay; if(delay>delay_max)delay_max=delay;
    } else {
        /* No fresh authority: publish only a real, beyond already reserved
         * targets in this surface generation. Never invent an API ID. */
        uint64_t floor=g_pres_timeline_state.window_generation==source->generation ?
            g_pres_timeline_state.last_child_ns:0;
        if (floor && pair->slot.refresh_ns<=INT64_MAX-floor && pair->slot.desired_ns<=floor)
            pair->slot.desired_ns=pair->slot.copy_open_ns=floor+pair->slot.refresh_ns;
        g_pres_timeline_state.window_generation=source->generation;
        g_pres_timeline_state.last_real_ns=g_pres_timeline_state.last_child_ns=pair->slot.desired_ns;
        pair->midpoint_ns=0; pair->reason=VK_INTERP_CLOCK;
    }
    if (pairs%300==0) {
        uint64_t n=chosen+fallback;
        LOGV("[present-timeline] last300 pairs API=%llu reanchor=%llu unavailable=%llu real-target-from-reserve mean/min/max=%.3f/%.3f/%.3fms; immutable real/child roles, not scanout",
            (unsigned long long)chosen,(unsigned long long)fallback,(unsigned long long)unavailable,
            n?delay_sum/(double)n/1e6:0,n?delay_min/1e6:0,delay_max/1e6);
        for(unsigned role=0;role<2;role++) {
            uint64_t count=atomic_exchange_explicit(&g_timeline_applied[role],0,memory_order_relaxed);
            uint64_t late=atomic_exchange_explicit(&g_timeline_deadline_late[role],0,memory_order_relaxed);
            int64_t ready=atomic_exchange_explicit(&g_timeline_ready_slack[role],0,memory_order_relaxed);
            int64_t apply=atomic_exchange_explicit(&g_timeline_apply_slack[role],0,memory_order_relaxed);
            uint64_t known=atomic_exchange_explicit(&g_timeline_ready_known[role],0,memory_order_relaxed);
            LOGV("[present-timeline] role=%s applied=%llu deadline-late=%llu ready-known=%llu deadline-minus-ready/apply mean=%.3f/%.3fms (async report; real late is retained, child late is omitted)",
                role?"generated":"real",(unsigned long long)count,(unsigned long long)late,(unsigned long long)known,
                known?ready/(double)known/1e6:0,count?apply/(double)count/1e6:0);
        }
        chosen=fallback=unavailable=delay_sum=delay_max=0; delay_min=UINT64_MAX;
    }
}
/* Producer-side reservations, not display samples. The interval and target
 * below are the exact values later handed to the presenter with these pixels. */
static void pres_auto_reservation(const pres_buf *b) {
    if (!b->auto_cadence) return;
    static uint64_t total, previous, refresh, generation;
    static uint64_t interval1, interval2, other, legacy, short_floor, gaps, irregular, boundaries;
    static uint64_t late, skipped, worst_late, ahb, copy;
    interval1 += b->interval == 1; interval2 += b->interval == 2;
    other += b->interval > 2; legacy += !b->interval;
    ahb += b->zero != 0; copy += !b->zero;
    short_floor += b->auto_frame.id && b->interval && b->interval < b->auto_frame.interval;
    if (previous && b->refresh_ns && refresh == b->refresh_ns && generation == b->generation) {
        uint64_t period = b->refresh_ns * b->interval;
        uint64_t delta = b->desired_ns > previous ? b->desired_ns - previous : 0;
        if (!period || !delta || delta % period) irregular++;
        else gaps += delta != period;
    } else boundaries++;
    previous = b->desired_ns; refresh = b->refresh_ns; generation = b->generation;
    uint64_t now = host_now_ns(), delay = now > b->desired_ns ? now - b->desired_ns : 0;
    late += delay != 0; skipped += b->slot_skipped;
    if (delay > worst_late) worst_late = delay;
    if (++total % 300) return;
    LOGV("window AUTO queued cadence, last 300: AHB/copy=%llu/%llu immutable interval 1/2/>2/legacy=%llu/%llu/%llu/%llu below-acquired-floor=%llu; desired delta gap/irregular/boundary=%llu/%llu/%llu; skipped=%llu enqueue-late=%llu worst=%.3fms (reservations, NOT display)",
         (unsigned long long)ahb, (unsigned long long)copy,
         (unsigned long long)interval1, (unsigned long long)interval2, (unsigned long long)other,
         (unsigned long long)legacy, (unsigned long long)short_floor, (unsigned long long)gaps,
         (unsigned long long)irregular, (unsigned long long)boundaries, (unsigned long long)skipped,
         (unsigned long long)late, worst_late / 1e6);
    interval1 = interval2 = other = legacy = short_floor = gaps = irregular = boundaries = 0;
    late = skipped = worst_late = ahb = copy = 0;
}
#ifdef __ANDROID__
typedef struct { AHardwareBuffer *buffer; VkImage image; VkDeviceMemory mem; VkImageView view[2]; uint32_t w, h; int foreign, ahb_import; } pres_ahb;
static pres_ahb g_ahb[NPRES];
static _Atomic int g_ahb_off;
/* NativeActivity's root producer is not parentable on some framework releases. A failed creation is scoped to
   this window generation: a UI-thread attachment callback can publish an explicit parent and recover from copy. */
static _Atomic int g_ahb_surface_off;
static ANativeWindow *g_win_parent;
static int pres_slots_real_fallback_locked(pres_buf *b, const char *reason) {
    pres_slots_end_locked();
    b->adaptive_slot=(fgs_slot){0};
    b->adaptive_token=0; b->adaptive_eligible=0; b->adaptive_real_conflicts=0;
    b->refresh_ns=0; b->interval=0; b->timeline_role=0;
    uint64_t now=host_now_ns();
    int valid=now && now<=(uint64_t)INT64_MAX;
    LOGV("[adaptive-slot] event=%s frame=%llu mode=REAL-only scheduler=0 host_now_ns=%llu reason=%s",
        valid ? "real-only-fallback":"real-only-refused",(unsigned long long)b->frame,
        (unsigned long long)now,reason);
    if (!valid) return 0;
    b->desired_ns=b->copy_open_ns=now;
    return 1;
}
static int pres_slots_stage_real_locked(pres_buf *b, int display_id) {
    runtime_present_timelines clock;
    if (!b->adaptive_slots || b->generated) return 1;
    b->adaptive_real_conflicts=0;
    if (!runtime_android_adaptive_slots_active() ||
            !runtime_present_timeline_display_allowed(runtime_android_adaptive_slots_display_id(),display_id) ||
            !runtime_android_adaptive_slot_timelines(&clock))
        return pres_slots_real_fallback_locked(b,"clock-unavailable");
    if (!g_slot_policy.configured || g_slot_clock_generation!=clock.generation ||
            g_slot_window_generation!=b->generation || g_slot_source_epoch!=b->adaptive_epoch ||
            g_slot_policy.period_ns!=clock.refresh_ns) {
        pres_slots_end_locked();
        fgs_batch batch;
        if (fgs_reset(&g_slot_policy,b->generation,clock.entries[clock.preferred].expected_ns,
                clock.refresh_ns,UINT64_C(2000000),&batch)!=FGS_OK)
            return pres_slots_real_fallback_locked(b,"clock-reset-invalid");
        g_slot_clock_generation=clock.generation;
        g_slot_window_generation=b->generation;
        g_slot_source_epoch=b->adaptive_epoch;
    }
    /* LATTICE=1 only: same-clock phase correction; a fresh reset is re-phased too. */
    if (pres_latch_enabled()) pres_latch_reanchor_locked(&clock,b);
    fgs_real_plan plan;
    if (fgs_peek_real(&g_slot_policy,host_now_ns(),&plan)!=FGS_OK)
        return pres_slots_real_fallback_locked(b,"real-plan-invalid");
    uint64_t desired;
    if (!runtime_present_adaptive_slot_desired(plan.slot.target_ns,clock.refresh_ns,&desired))
        return pres_slots_real_fallback_locked(b,"desired-encoding-invalid");
    b->adaptive_slot=plan.slot;
    b->adaptive_real_conflicts=plan.generated_conflicts;
    b->desired_ns=b->copy_open_ns=plan.slot.target_ns;
    b->refresh_ns=clock.refresh_ns; b->interval=1;
    return 1;
}
static int pres_transaction_set_desired_locked(ASurfaceTransaction *tx, pres_buf *b,
        uint64_t *admitted_desired) {
    if (b->adaptive_slots && !b->generated && !pres_slots_stage_real_locked(b,g_win_display_id)) return 0;
    uint64_t desired;
    if (!pres_slots_desired(b,&desired)) {
        LOGV("[adaptive-slot] event=desired-refused frame=%llu generated=%d target_ns=%llu refresh_ns=%llu",
            (unsigned long long)b->frame,b->generated!=0,
            (unsigned long long)b->desired_ns,(unsigned long long)b->refresh_ns);
        return 0;
    }
    /* Keep nominal legacy cadence and its existing optional eligibility floor.
     * Scheduler setters and cadence share the same admitted signed encoding. */
    ASurfaceTransaction_setDesiredPresentTime(tx,(int64_t)(b->adaptive_slots ? desired :
        vk_present_eligible_time(desired,g_pres_floor_ns,g_pres_guard_ns,b->refresh_ns)));
    *admitted_desired=desired;
    return 1;
}
static void pres_slots_published_locked(pres_buf *b, uint64_t publication_ns,
        vk_present_retire_token transaction) {
    if (!b->adaptive_slots) return;
    if (g_fg_job_trace) {
        uint64_t desired;
        if (pres_slots_desired(b,&desired))
            LOGV("[adaptive-slot] event=apply-metadata frame=%llu generated=%d target_ns=%llu desired_ns=%llu desired_admitted=1 refresh_ns=%llu publication_ns=%llu tx_index=%u tx_sequence=%llu tx_generation=%llu sf_outcome=UNKNOWN",
                (unsigned long long)b->frame,b->generated!=0,
                (unsigned long long)b->desired_ns,(unsigned long long)desired,
                (unsigned long long)b->refresh_ns,(unsigned long long)publication_ns,
                transaction.index,(unsigned long long)transaction.sequence,
                (unsigned long long)transaction.generation);
        else LOGV("[adaptive-slot] event=apply-metadata frame=%llu generated=%d target_ns=%llu desired_admitted=0 refresh_ns=%llu publication_ns=%llu tx_index=%u tx_sequence=%llu tx_generation=%llu sf_outcome=UNKNOWN",
            (unsigned long long)b->frame,b->generated!=0,(unsigned long long)b->desired_ns,
            (unsigned long long)b->refresh_ns,(unsigned long long)publication_ns,
            transaction.index,(unsigned long long)transaction.sequence,
            (unsigned long long)transaction.generation);
    }
    if (b->generated) {
        /* Even a failed scheduler commit cannot undo an actual Android apply.
         * Keep the immutable transaction identity, not a reusable image index. */
        fgs_publication_record(&g_slot_applied_generations,(fgs_applied_generation){
            .id={transaction.index,transaction.sequence,transaction.generation},
            .slot=b->adaptive_slot,.source_frame=b->frame,.source_epoch=b->adaptive_epoch,
            .apply_ns=publication_ns});
        fgs_gen_commit result;
        fgs_status status=fgs_commit_gen(&g_slot_policy,b->adaptive_token,publication_ns,&result);
        if (status==FGS_OK) atomic_fetch_add_explicit(result.target_missed?
            &g_slot_late_applies:&g_slot_on_time_applies,1,memory_order_relaxed);
        if (g_fg_job_trace) LOGV("[adaptive-slot] event=gen-commit frame=%llu token=%llu target_ns=%llu publication_ns=%llu status=%u deadline_missed=%u target_missed=%u",
            (unsigned long long)b->frame,(unsigned long long)b->adaptive_token,
            (unsigned long long)b->desired_ns,(unsigned long long)publication_ns,(unsigned)status,
            status==FGS_OK?result.deadline_missed:0,status==FGS_OK?result.target_missed:0);
        return;
    }
    pres_slots_observe_real_locked(b,publication_ns);
    for (unsigned i=0;i<FGS_PUBLICATION_HISTORY;i++) {
        const fgs_applied_generation *g=&g_slot_applied_generations.generated[i];
        fgs_late_real_kind kind=fgs_publication_late_real(g,b->adaptive_real_observation);
        if (kind==FGS_REAL_NOT_LATE) continue;
        atomic_fetch_add_explicit(&g_slot_late_real_counts[kind],1,memory_order_relaxed);
        if (g_fg_job_trace) {
            const char *reason=kind==FGS_REAL_READY_AFTER_GEN_APPLY ?
                "potential-late-real-after-gen" :
                kind==FGS_REAL_READY_BEFORE_GEN_APPLY_UNSERVICED ?
                "ready-before-gen-apply-unserviced" : "ready-provenance-unknown";
            LOGV("[adaptive-slot-residual] kind=%s real_frame=%llu real_ready_ns=%llu ready_provenance=%s first_attempt_ns=%llu real_apply_return_ns=%llu tx_index=%u tx_sequence=%llu tx_generation=%llu gen_source_frame=%llu gen_target_ns=%llu gen_apply_return_ns=%llu potential_late_real_total=%llu ready_unserviced_total=%llu unknown_ready_total=%llu sf_outcome=UNKNOWN",
                reason,(unsigned long long)b->frame,
                (unsigned long long)b->adaptive_real_observation.ready_ns,
                b->adaptive_real_observation.ready_time_known?"backend-cpu-fence-publication":"UNKNOWN",
                (unsigned long long)b->adaptive_real_observation.first_attempt_ns,
                (unsigned long long)publication_ns,g->id.index,
                (unsigned long long)g->id.sequence,(unsigned long long)g->id.generation,
                (unsigned long long)g->source_frame,(unsigned long long)g->slot.target_ns,
                (unsigned long long)g->apply_ns,
                (unsigned long long)atomic_load_explicit(&g_slot_late_real_counts[FGS_REAL_READY_AFTER_GEN_APPLY],memory_order_relaxed),
                (unsigned long long)atomic_load_explicit(&g_slot_late_real_counts[FGS_REAL_READY_BEFORE_GEN_APPLY_UNSERVICED],memory_order_relaxed),
                (unsigned long long)atomic_load_explicit(&g_slot_late_real_counts[FGS_REAL_READY_TIME_UNKNOWN],memory_order_relaxed));
        }
    }
    for (unsigned k=0;k<NPRES;k++) {
        pres_slot_prepared *p=&g_slot_prepared[k];
        if (p->generation && p->frame==b->frame && p->epoch==b->adaptive_epoch) {
            p->published=1; p->real_fence=b->fence; p->real_target=b->desired_ns;
            if (!b->adaptive_slot.target_ns) p->cancelled=1;
        }
    }
    if (!b->adaptive_slot.target_ns) return;
    fgs_batch batch;
    fgs_status status=fgs_commit_real(&g_slot_policy,b->adaptive_slot,publication_ns,
        b->frame,b->adaptive_epoch,(int)b->adaptive_eligible,&batch);
    if (status==FGS_OK) atomic_fetch_add_explicit(&g_slot_irrevocable_real_conflicts,
        b->adaptive_real_conflicts,memory_order_relaxed);
    if (status==FGS_OK) pres_slots_route_locked(&batch);
    else for (unsigned k=0;k<NPRES;k++)
        if (g_slot_prepared[k].frame==b->frame) g_slot_prepared[k].cancelled=1;
    if (g_fg_job_trace) LOGV("[adaptive-slot] event=real-commit frame=%llu target_ns=%llu publication_ns=%llu status=%u mode=%u requests=%u cancellations=%u target_missed=%u irrevocable_gen_conflicts=%u",
        (unsigned long long)b->frame,(unsigned long long)b->desired_ns,
        (unsigned long long)publication_ns,(unsigned)status,(unsigned)batch.mode,
        batch.request_count,batch.cancel_count,batch.target_missed,b->adaptive_real_conflicts);
}
static VkSampler g_pres_sampler, g_pres_pixel_sampler; static VkDescriptorSetLayout g_pres_dsl; static VkPipelineLayout g_pres_layout; static VkPipeline g_pres_pipe[2];
static VkPipelineLayout g_pres_rotated_layout; static VkPipeline g_pres_rotated_pipe[2];
/* Ordinary presentation remains serial. Locked interpolation and explicitly
 * selected adaptive request mode may have two distinct FUTURE transactions.
 * Contexts stay immutable through ordered completion and PREVIOUS release. */
typedef struct pres_complete {
    pthread_mutex_t mu; pthread_cond_t cv; ASurfaceControl *surface; int done, previous, release_fd;
    pres_buf *current;
    uint64_t timeline_frame, timeline_apply;
    struct pres_complete *observer;
    vk_present_fence_entry snapshot;
    uint64_t record_done_ns, enqueue_ns, dequeue_ns, own_ready_ns, pair_ready_ns;
    unsigned queue_depth, pending_transactions;
    vk_present_retire_token retire_token;
    unsigned retire_async;
    /* Shared observation state lives only in the first context. */
    vk_present_fences display_fences;
    unsigned display_diagnostics;
    uint64_t cadence_next;
    vk_present_cadence cadence;
} pres_complete;
/* Never borrow presenter-stack storage: a missing callback may outlive a
 * bounded drain. A quarantined owner is deliberately retained until process
 * death, including its contexts, old surface, transaction and window refs. */
static struct {
    vk_present_retire queue;
    pres_complete *contexts[VK_PRESENT_RETIRE_CAPACITY];
    _Atomic int initialized, running, exiting;
    ASurfaceControl *surface;
    ASurfaceTransaction *transaction;
    ANativeWindow *window, *parent;
} g_pres_retire;
_Static_assert(VK_PRESENT_RETIRE_CAPACITY==VK_PRESENT_FLIGHT_CAPACITY,"retirement must not add contexts");
_Static_assert(VK_PRESENT_RETIRE_CAPACITY==FGS_PUBLICATION_CONTEXTS,"publication snapshot must match live contexts");
static unsigned pres_slots_live_transactions(
        fgs_transaction_id live[FGS_PUBLICATION_CONTEXTS]) {
    if (!g_pres_retire.initialized || !g_pres_retire.running) return 0;
    vk_present_retire_token tokens[VK_PRESENT_RETIRE_CAPACITY];
    unsigned count=vk_present_retire_inflight(&g_pres_retire.queue,tokens);
    for (unsigned i=0;i<count;i++)
        live[i]=(fgs_transaction_id){tokens[i].index,tokens[i].sequence,tokens[i].generation};
    return count;
}
static pthread_mutex_t g_pres_retire_lifecycle = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_pres_retire_sequence, g_pres_sync_retires;
static void pres_retire_event(void *context,const vk_present_retire_event *event) {
    (void)context;
    if (!g_fg_job_trace) return;
    LOGV("[framegen-retire] event=%s sequence=%llu generation=%llu context=%u now-ns=%llu depth=%u worker-wakes=%llu worker-retires=%llu async=1",
        event->event,(unsigned long long)event->token.sequence,
        (unsigned long long)event->token.generation,event->token.index,
        (unsigned long long)event->now_ns,event->depth,
        (unsigned long long)event->wakes,(unsigned long long)event->retires);
}
static void pres_retire_sync_event(pres_complete *c,const char *event,unsigned depth) {
    if (!g_fg_job_trace) return;
    LOGV("[framegen-retire] event=%s sequence=%llu generation=%llu context=%u now-ns=%llu depth=%u worker-wakes=0 worker-retires=%llu async=0",
        event,(unsigned long long)c->retire_token.sequence,
        (unsigned long long)c->retire_token.generation,c->retire_token.index,
        (unsigned long long)host_now_ns(),depth,(unsigned long long)g_pres_sync_retires);
}
static void pres_retire_wake(void *context) {
    (void)context;
    pres_retire_notify();
}
/* OnCommit has NO present/release fences. OnComplete owns the CURRENT
 * transaction's present fd, which can still be pending when inspected. Keep
 * that fd and immutable frame/apply IDs, not the callback stats or pres_buf.
 * Latch, callback, and PREVIOUS release times never become display endpoints. */
static vk_present_fence_reason pres_display_query(int fd, uint64_t *display, int *error, void *context) {
    (void)context;
    struct sync_fence_info fences[VK_PRESENT_FENCE_CHILDREN];
    struct sync_file_info info = {.num_fences = VK_PRESENT_FENCE_CHILDREN,
        .sync_fence_info = (uint64_t)(uintptr_t)fences};
    if (ioctl(fd, SYNC_IOC_FILE_INFO, &info)) {
        *error = errno; *display = 0; return VK_PF_IOCTL_ERROR;
    }
    return vk_present_fence_decode(&info, fences, VK_PRESENT_FENCE_CHILDREN, display);
}
static void pres_display_close(int fd, void *context) { (void)context; close(fd); }
static void pres_display_result(const vk_present_fence_entry *entry, vk_present_fence_reason reason,
        uint64_t display, void *context) {
    pres_complete *c = context;
    if (entry->cadence.id) {
        vk_present_cadence_sample sample = entry->cadence;
        sample.display_ns = reason == VK_PF_SIGNALED ? display : 0;
        int trace=g_pres_trace;
        if (trace) LOGV("[present-trace] id=%llu generation=%llu source_frame=%llu kind=%s desired_ns=%llu apply_ns=%llu display_ns=%llu refresh_ns=%llu interval=%u reason=%u",
            (unsigned long long)sample.id, (unsigned long long)sample.generation,
            (unsigned long long)entry->source_frame,
            entry->generated ? (entry->interpolated ? "interp" : "extrap") : "real",
            (unsigned long long)sample.desired_ns, (unsigned long long)sample.apply_ns,
            (unsigned long long)sample.display_ns, (unsigned long long)sample.refresh_ns,
            sample.interval, (unsigned)reason);
        int hybrid=runtime_fps_hybrid_selected();
        uint64_t previous_bins[6];
        if (hybrid) memcpy(previous_bins,c->cadence.counts.bins,sizeof previous_bins);
        vk_present_cadence_result(&c->cadence, sample);
        vk_present_cadence_counts *n = &c->cadence.counts;
        if (hybrid) {
            for (unsigned i=0;i<6;i++) g_hybrid_bins[i]+=n->bins[i]-previous_bins[i];
            g_hybrid_missing+=!sample.display_ns;
        }
        if (n->results == 300) {
            vk_present_cadence_percentile p = vk_present_cadence_percentiles(n);
            LOGV("window present-fence cadence, last 300 results: immutable interval 1/2/>2/legacy=%llu/%llu/%llu/%llu signaled/missing=%llu/%llu pairs=%llu missing-pairs=%llu boundary=%llu history-lost=%llu repeated/reversed=%llu/%llu; interval min/mean/max=%.3f/%.3f/%.3fms p50/p95/p99=%.3f/%.3f/%.3fms samples=%u/%llu sample-overflow=%llu; apply-late=%llu worst=%.3fms; fence-before-desired=%llu >=refresh-late=%llu worst=%.3fms (framework present-fence signals, NOT guaranteed hardware scanout)",
                 (unsigned long long)n->interval1, (unsigned long long)n->interval2,
                 (unsigned long long)n->other, (unsigned long long)n->legacy,
                 (unsigned long long)n->signaled, (unsigned long long)n->missing, (unsigned long long)n->pairs,
                 (unsigned long long)n->missing_pairs, (unsigned long long)n->boundaries, (unsigned long long)n->history_lost,
                 (unsigned long long)n->repeated, (unsigned long long)n->reversed,
                 n->min_ns / 1e6, n->pairs ? n->sum_ns / (double)n->pairs / 1e6 : 0, n->max_ns / 1e6,
                 p.p50_ns / 1e6, p.p95_ns / 1e6, p.p99_ns / 1e6, n->sample_count,
                 (unsigned long long)n->pairs, (unsigned long long)n->sample_lost,
                 (unsigned long long)n->apply_late, n->worst_apply_late_ns / 1e6,
                 (unsigned long long)n->before_desired, (unsigned long long)n->late_refresh, n->worst_late_ns / 1e6);
            LOGV("window present-fence interval bins ms [<12.5,12.5-20.9,20.9-29,29-37.5,37.5-45.8,>=45.8]: all=%llu/%llu/%llu/%llu/%llu/%llu; adjacent immutable interval2=%llu/%llu/%llu/%llu/%llu/%llu (only adjacent transaction IDs, no callback-order or missing-fence substitution; always on for AHB, independent of AUTO/FT)",
                 (unsigned long long)n->bins[0], (unsigned long long)n->bins[1], (unsigned long long)n->bins[2],
                 (unsigned long long)n->bins[3], (unsigned long long)n->bins[4], (unsigned long long)n->bins[5],
                 (unsigned long long)n->interval2_bins[0], (unsigned long long)n->interval2_bins[1], (unsigned long long)n->interval2_bins[2],
                 (unsigned long long)n->interval2_bins[3], (unsigned long long)n->interval2_bins[4],
                 (unsigned long long)n->interval2_bins[5]);
            for (unsigned i = 0; i < n->tail_count; i++) {
                const vk_present_cadence_tail *t = &n->tails[i];
                LOGV("window present-fence interval2 tail, first %u resolved pairs at most: ids=%llu/%llu preceding-interval=%u (0=legacy/unknown) desired-delta=%.3fms native-delta=%.3fms (immutable endpoint intervals 2/2; no transition or scanout inference)",
                     VK_PRESENT_CADENCE_TAILS, (unsigned long long)t->first_id, (unsigned long long)t->second_id,
                     t->before_interval, t->desired_delta_ns / 1e6, t->display_delta_ns / 1e6);
            }
            *n = (vk_present_cadence_counts){0};
        }
    }
    if (reason == VK_PF_SIGNALED) {
        if (entry->framegen_real && entry->cadence.interval==2)
            vk_fg_real_feedback(entry->framegen_real,entry->cadence.desired_ns,
                display,entry->cadence.refresh_ns);
        runtime_android_display_presented(display);
        if (entry->prediction.id) runtime_android_predict_presented(&entry->prediction, display);
        if (totk_ft_on() && entry->frame) totk_ft_compositor(entry->frame, entry->apply_ns, display);
    } else if (c->display_diagnostics++ < 8) {
        static const char *const names[VK_PF_REASON_COUNT] = {
            "signaled", "pending", "absent", "ioctl-error", "fence-error", "too-many-children",
            "invalid-timestamp", "evicted-pending", "teardown-pending"};
        LOGV("window present fence: frame=%llu apply_ns=%llu generation=%llu reason=%s errno=%d latch_ns=%lld (queried framework diagnostic, NOT display)",
             (unsigned long long)entry->frame, (unsigned long long)entry->apply_ns,
             (unsigned long long)entry->generation, names[reason], entry->error, (long long)entry->latch_ns);
    }
}
static void pres_display_sweep(pres_complete *c, int clear) {
    pthread_mutex_lock(&c->mu);
    vk_present_fences_sweep(&c->display_fences, pres_display_query, pres_display_close, pres_display_result, c);
    if (clear) vk_present_fences_clear(&c->display_fences, pres_display_close, pres_display_result, c);
    pthread_mutex_unlock(&c->mu);
}
static void pres_complete_cb(void *context, ASurfaceTransactionStats *stats) {
    pres_complete *c = context, *observer=c->observer;
    uint64_t callback_ns=g_pres_trace || g_pres_slot_diag ? host_now_ns() : 0;
    if (g_pres_slot_diag && c->current)
        atomic_store_explicit(&c->current->diag_callback_ns,callback_ns,memory_order_release);
    pres_credit_release(c->current); /* CURRENT completion, not PREVIOUS release */
    pthread_mutex_lock(&observer->mu);
    vk_present_fences *f = &observer->display_fences;
    vk_present_fences_sweep(f, pres_display_query, pres_display_close, pres_display_result, observer);
    if (c->current) {
        vk_present_fence_entry entry=c->snapshot;
        entry.fd=ASurfaceTransactionStats_getPresentFenceFd(stats);
        entry.latch_ns=ASurfaceTransactionStats_getLatchTime(stats);
        vk_present_fences_add(f, entry, pres_display_query, pres_display_close, pres_display_result, observer);
        if (g_pres_trace) LOGV("[present-pipeline] id=%llu generation=%llu source_frame=%llu kind=%s desired_ns=%llu record_done_ns=%llu enqueue_ns=%llu dequeue_ns=%llu own_ready_ns=%llu pair_ready_ns=%llu apply_ns=%llu callback_ns=%llu queue_depth=%u pending_transactions=%u",
            (unsigned long long)entry.cadence.id,(unsigned long long)entry.generation,
            (unsigned long long)entry.source_frame,entry.generated ? (entry.interpolated ? "interp" : "extrap") : "real",
            (unsigned long long)entry.cadence.desired_ns,(unsigned long long)c->record_done_ns,
            (unsigned long long)c->enqueue_ns,(unsigned long long)c->dequeue_ns,
            (unsigned long long)c->own_ready_ns,(unsigned long long)c->pair_ready_ns,
            (unsigned long long)entry.cadence.apply_ns,(unsigned long long)callback_ns,
            c->queue_depth,c->pending_transactions);
        if (f->accepted <= 3 || f->accepted % 300 == 0)
            LOGV("window present fences: total=%llu signaled=%llu deferred=%llu pending=%u pending_queries=%llu absent=%llu ioctl_error=%llu fence_error=%llu oversized=%llu invalid_time=%llu evicted=%llu teardown=%llu last_latch_ns=%lld (diagnostic only; no guaranteed hardware timestamp)",
                 (unsigned long long)f->accepted, (unsigned long long)f->reason[VK_PF_SIGNALED],
                 (unsigned long long)f->deferred, f->pending, (unsigned long long)f->reason[VK_PF_PENDING],
                 (unsigned long long)f->reason[VK_PF_NO_FD], (unsigned long long)f->reason[VK_PF_IOCTL_ERROR],
                 (unsigned long long)f->reason[VK_PF_FENCE_ERROR], (unsigned long long)f->reason[VK_PF_TOO_MANY],
                 (unsigned long long)f->reason[VK_PF_INVALID_TIME], (unsigned long long)f->reason[VK_PF_EVICTED],
                 (unsigned long long)f->reason[VK_PF_TEARDOWN], (long long)entry.latch_ns);
    }
    pthread_mutex_unlock(&observer->mu);
    int fd = c->previous >= 0 ? ASurfaceTransactionStats_getPreviousReleaseFenceFd(stats, c->surface) : -1;
    pthread_mutex_lock(&c->mu); c->release_fd = fd; c->done = 1; pthread_cond_signal(&c->cv); pthread_mutex_unlock(&c->mu);
}
static void pres_transaction_finish(pres_complete *c);
static fg_adaptive_present_result pres_transaction(ASurfaceTransaction *tx, pres_complete *c, ASurfaceControl *surface,
        int previous, pres_buf *current, uint64_t setup_ns, int defer, unsigned pending, int adaptive_locked) {
    /* Adaptive callers acquire g_win_mu only after blocking drains and before
     * staging setters. This routine releases it on rejection or just after
     * apply, never across the completion wait below. */
    if ((current && current->adaptive_epoch)!=adaptive_locked) DIE("adaptive transaction guard ownership");
    uint64_t admitted_desired=0;
    if (current && !pres_transaction_set_desired_locked(tx,current,&admitted_desired)) {
        c->current=NULL; c->snapshot=(vk_present_fence_entry){0}; c->done=1;
        if (adaptive_locked) pthread_mutex_unlock(&g_win_mu);
        return FG_ADAPTIVE_PRESENT_SUPPRESSED;
    }
    uint64_t frame = current ? current->timeline_frame : 0;
    c->surface = surface; c->previous = previous; c->current = current; c->done = 0; c->release_fd = -1;
    c->timeline_frame = frame; c->timeline_apply = 0;
    c->pending_transactions=pending;
    c->snapshot=(vk_present_fence_entry){0};
    if (!c->retire_async) {
        c->retire_token=(vk_present_retire_token){
            c==g_pres_retire.contexts[1],++g_pres_retire_sequence,
            current ? current->generation : previous>=0 ? g_pres[previous].generation : 0};
        pres_retire_sync_event(c,"reserve",pending);
    }
    uint64_t apply_start = current && (current->prediction.id || current->auto_frame.id) ? host_now_ns() : 0;
    ASurfaceTransaction_setOnComplete(tx, c, pres_complete_cb);
    if (current) {
        uint64_t apply_ns=host_now_ns();
        if (g_pres_timeline && current->timeline_role) {
            unsigned role=current->timeline_role-1;
            uint64_t deadline=current->display_timeline.deadline_ns;
            uint64_t ready=current->timeline_role==RUNTIME_PRESENT_TIMELINE_REAL ? current->own_ready_ns :
                current->own_ready_ns>current->pair_ready_ns ? current->own_ready_ns:current->pair_ready_ns;
            atomic_fetch_add_explicit(&g_timeline_applied[role],1,memory_order_relaxed);
            atomic_fetch_add_explicit(&g_timeline_deadline_late[role],apply_ns>=deadline,memory_order_relaxed);
            if (ready) {
                atomic_fetch_add_explicit(&g_timeline_ready_known[role],1,memory_order_relaxed);
                atomic_fetch_add_explicit(&g_timeline_ready_slack[role],(int64_t)deadline-(int64_t)ready,memory_order_relaxed);
            }
            atomic_fetch_add_explicit(&g_timeline_apply_slack[role],(int64_t)deadline-(int64_t)apply_ns,memory_order_relaxed);
        }
        if (g_pres_slot_diag) atomic_store_explicit(&current->diag_apply_ns,apply_ns,memory_order_release);
        c->snapshot=(vk_present_fence_entry){.frame=frame,
            .framegen_real=current->framegen_real,.source_frame=current->frame,
            .generated=current->generated,.interpolated=current->interpolated,
            .generation=current->generation,.prediction=current->prediction,
            .cadence={.id=++c->observer->cadence_next,.generation=current->generation,
                .desired_ns=admitted_desired,.refresh_ns=current->refresh_ns,
                .interval=current->interval,.apply_ns=apply_ns}};
        c->record_done_ns=current->record_done_ns; c->enqueue_ns=current->enqueue_ns;
        c->dequeue_ns=current->dequeue_ns; c->own_ready_ns=current->own_ready_ns;
        c->pair_ready_ns=current->pair_ready_ns; c->queue_depth=current->queue_depth;
    }
    if (totk_ft_on() && frame) c->timeline_apply = totk_ft_now();
    c->snapshot.apply_ns=c->timeline_apply;
    if (current && current->adaptive_epoch) {
        /* The caller still holds g_win_mu through staging. Repeat the epoch,
         * window and ready-real check immediately before the actual handoff. */
        fg_adaptive_present_result decision=pres_adaptive_decide_locked(current);
        uint64_t final_desired;
        if (decision==FG_ADAPTIVE_PRESENT_APPLY && current->adaptive_slots &&
                (!pres_slots_desired(current,&final_desired) || final_desired!=admitted_desired))
            decision=FG_ADAPTIVE_PRESENT_SUPPRESSED;
        if (decision!=FG_ADAPTIVE_PRESENT_APPLY) {
            if (g_pres_slot_diag) atomic_store_explicit(&current->diag_apply_ns,0,memory_order_release);
            c->observer->cadence_next--; /* this staged context was never handed off */
            c->current=NULL; c->snapshot=(vk_present_fence_entry){0}; c->done=1;
            if (!c->retire_async) pres_retire_sync_event(c,"cancel",pending-1);
            pthread_mutex_unlock(&g_win_mu);
            return decision;
        }
    }
    /* JOB_TRACE rows / LATTICE or EARLY_APPLY counters only; no decision reads this clock. */
    uint64_t latch_apply_ns=current && current->adaptive_epoch && current->adaptive_slots &&
        (g_fg_job_trace || pres_latch_enabled() || pres_early_enabled()) ? host_now_ns() : 0;
    ASurfaceTransaction_apply(tx);
    if (current && current->adaptive_epoch) {
        uint64_t publication_ns=host_now_ns();
        pres_slots_published_locked(current,publication_ns,c->retire_token);
        if (g_fg_job_trace)
            LOGV("[adaptive-dispatch-stage] stage=transaction-published source=%llu generated=%u output=%u source_ticket=%llu output_ticket=%llu source_cpu_ready_ns=%llu output_cpu_ready_ns=%llu publication_ns=%llu target_ns=%llu tx_index=%u tx_sequence=%llu tx_generation=%llu final_gpu_done_ns=0 final_gpu_done_authority=UNKNOWN-not-exported clock=CLOCK_MONOTONIC publication_authority=ASurfaceTransaction-apply-return sf_outcome=UNKNOWN",
                (unsigned long long)current->frame,current->generated!=0,(unsigned)(current-g_pres),
                (unsigned long long)current->source_fence,(unsigned long long)current->fence,
                (unsigned long long)current->pair_ready_ns,(unsigned long long)current->own_ready_ns,
                (unsigned long long)publication_ns,(unsigned long long)current->desired_ns,
                c->retire_token.index,(unsigned long long)c->retire_token.sequence,
                (unsigned long long)c->retire_token.generation);
        if (latch_apply_ns) pres_latch_applied_locked(current,latch_apply_ns,publication_ns,admitted_desired);
    }
    if (current && current->adaptive_epoch) {
        fg_adaptive_present_committed(&g_adaptive_publication,pres_adaptive_job(current));
        if (g_adaptive_presenting==(int)(current-g_pres)) g_adaptive_presenting=-1;
        pthread_mutex_unlock(&g_win_mu); /* never hold the queue/lifecycle mutex through completion */
        vk_overlay_presented(host_now_ns(),current->generated);
        if (current->generated) pres_child_handoff();
    }
    if (current) pres_job_trace(current,"handed-off");
    if (apply_start) {
        uint64_t apply_ns = host_now_ns() - apply_start;
        pres_predict_feedback(current, PRESENT_PREDICT_AHB, current->ready_ns, 0, setup_ns + apply_ns);
    }
    if (totk_ft_on() && frame) {
        totk_ft_submission(frame, c->timeline_apply, FT_SUBMIT_AHB_APPLY);
        totk_ft_compositor(frame, c->timeline_apply, 0);
    }
    if (!c->retire_async) pres_retire_sync_event(c,"publish",pending);
    if (!defer) pres_transaction_finish(c);
    return FG_ADAPTIVE_PRESENT_APPLY;
}
static int pres_transaction_finish_until(pres_complete *c,uint64_t deadline,unsigned depth) {
    uint64_t callback_wait_begin=g_fg_job_trace ? host_now_ns() : 0;
    if (!c->retire_async) pres_retire_sync_event(c,"dequeue",depth);
    pthread_mutex_lock(&c->mu);
    int complete=1;
    while (!c->done) {
        int error=0;
        if (deadline) {
            struct timespec absolute=vk_present_retire_time(deadline);
            error=pthread_cond_timedwait(&c->cv,&c->mu,&absolute);
        } else pthread_cond_wait(&c->cv,&c->mu);
        if (error && !c->done) { complete=0; break; }
    }
    int fd=complete ? c->release_fd : -1;
    pthread_mutex_unlock(&c->mu);
    uint64_t callback_wait_end=g_fg_job_trace ? host_now_ns() : 0;
    uint64_t release_poll_begin=0,release_poll_end=0;
    uint64_t wait_start=g_pres_trace || g_pres_slot_diag ? host_now_ns() : 0;
    int released=complete;
    if (fd>=0) {
        if (g_pres_slot_diag && c->previous>=0)
            atomic_store_explicit(&g_pres[c->previous].diag_previous_wait_ns,wait_start,memory_order_release);
        struct pollfd p={.fd=fd,.events=POLLIN};
        int r;
        if (g_fg_job_trace) release_poll_begin=host_now_ns();
        do {
            int timeout=-1;
            if (deadline) {
                uint64_t now=host_now_ns(),remaining=deadline>now ? deadline-now : 0;
                uint64_t ms=remaining/UINT64_C(1000000)+(remaining%UINT64_C(1000000)!=0);
                timeout=ms>INT_MAX ? INT_MAX : (int)ms;
            }
            r=poll(&p,1,timeout);
        } while (r<0 && errno==EINTR && (!deadline || host_now_ns()<deadline));
        if (g_fg_job_trace) release_poll_end=host_now_ns();
        released=r>0 && (p.revents&POLLIN) &&
            (!deadline || !(p.revents&(POLLERR|POLLNVAL)));
        if (!r || (r<0 && errno==EINTR)) complete=0;
        close(fd);
        c->release_fd=-1; /* only the finish owner writes after done */
    }
    vk_present_retire_release_result(released,
        c->previous>=0 ? &g_pres[c->previous].busy : NULL,&g_ahb_off);
    if (!complete) {
        LOGV("window present: async retirement timeout sequence=%llu generation=%llu previous=%d callback=%d; quarantining contexts/buffers/surface, ownership retained (2s monotonic budget)",
            (unsigned long long)c->retire_token.sequence,(unsigned long long)c->retire_token.generation,
            c->previous,fd>=0);
    } else if (!released) {
        LOGV("window present: compositor release fence failed; using copy, previous retained");
    }
    if (g_pres_trace && c->current) LOGV("[present-release] id=%llu generation=%llu source_frame=%llu wait_start_ns=%llu released_ns=%llu previous=%d",
        (unsigned long long)c->snapshot.cadence.id,(unsigned long long)c->snapshot.generation,
        (unsigned long long)c->snapshot.source_frame,(unsigned long long)wait_start,
        (unsigned long long)(released ? host_now_ns() : 0),c->previous);
    if (g_fg_job_trace) LOGV("[framegen-wait] source-frame=%llu generated=%d target-ns=%llu finish-wait-start-ns=%llu finish-wait-end-ns=%llu previous-release-poll-start-ns=%llu previous-release-poll-end-ns=%llu finish-end-ns=%llu previous=%d released=%d sequence=%llu generation=%llu async=%u complete=%d; CLOCK_MONOTONIC blocking-intervals-not-cpu-scheduler-attribution",
        (unsigned long long)c->snapshot.source_frame,c->snapshot.generated,
        (unsigned long long)c->snapshot.cadence.desired_ns,
        (unsigned long long)callback_wait_begin,(unsigned long long)callback_wait_end,
        (unsigned long long)release_poll_begin,(unsigned long long)release_poll_end,
        (unsigned long long)host_now_ns(),c->previous,released,
        (unsigned long long)c->retire_token.sequence,(unsigned long long)c->retire_token.generation,
        c->retire_async,complete);
    if (!c->retire_async) {
        g_pres_sync_retires++;
        pres_retire_sync_event(c,"retire",depth-1);
    }
    return complete;
}
static void pres_transaction_finish(pres_complete *c) {
    (void)pres_transaction_finish_until(c,0,1); /* OFF retains its original unbounded ownership wait */
}
static int pres_retire_finish(void *context,unsigned index,uint64_t deadline) {
    (void)context;
    return pres_transaction_finish_until(g_pres_retire.contexts[index],deadline,0);
}
static void pres_retire_exit(void) {
    pthread_mutex_lock(&g_pres_retire_lifecycle);
    g_pres_retire.exiting=1;
    atomic_store(&g_ahb_off,1); /* stop admission before the bounded process-exit drain */
    if (g_pres_retire.initialized)
        (void)vk_present_retire_drain_join(&g_pres_retire.queue,vk_present_retire_deadline());
    pthread_mutex_unlock(&g_pres_retire_lifecycle);
    /* No context, surface, buffer or callback storage is freed at process exit. */
}
static int pres_retire_start(void) {
    pthread_mutex_lock(&g_pres_retire_lifecycle);
    if (g_pres_retire.running) { pthread_mutex_unlock(&g_pres_retire_lifecycle); return 1; }
    if (g_pres_retire.initialized || g_pres_retire.exiting) {
        pthread_mutex_unlock(&g_pres_retire_lifecycle); return 0; /* permanently quarantined/exiting */
    }
    int error=vk_present_retire_init(&g_pres_retire.queue,pres_retire_finish,
        pres_retire_wake,g_fg_job_trace ? pres_retire_event : NULL,NULL);
    if (error) { pthread_mutex_unlock(&g_pres_retire_lifecycle); return 0; }
    g_pres_retire.queue.sequence=g_pres_retire_sequence;
    error=vk_present_retire_start(&g_pres_retire.queue);
    if (error) {
        vk_present_retire_destroy(&g_pres_retire.queue);
        pthread_mutex_unlock(&g_pres_retire_lifecycle); return 0;
    }
    g_pres_retire.initialized=g_pres_retire.running=1;
    static int exit_registered;
    if (!exit_registered) { atexit(pres_retire_exit); exit_registered=1; }
    pthread_mutex_unlock(&g_pres_retire_lifecycle);
    return 1;
}
static int pres_retire_stop(uint64_t deadline) {
    pthread_mutex_lock(&g_pres_retire_lifecycle);
    if (!g_pres_retire.initialized) { pthread_mutex_unlock(&g_pres_retire_lifecycle); return 1; }
    int clean=vk_present_retire_drain_join(&g_pres_retire.queue,deadline);
    g_pres_retire.running=0;
    if (!clean) {
        atomic_store(&g_ahb_off,1);
        LOGV("window present: retirement drain quarantined; process-lifetime contexts/buffers/surface retained, copy fallback");
        pthread_mutex_unlock(&g_pres_retire_lifecycle);
        return 0;
    }
    g_pres_retire_sequence=g_pres_retire.queue.sequence;
    if (!vk_present_retire_destroy(&g_pres_retire.queue)) DIE("retirement destroy without clean join");
    g_pres_retire.initialized=0;
    pthread_mutex_unlock(&g_pres_retire_lifecycle);
    return 1;
}
/* Held job stays GPU-retired but presenter-owned. This is a queue/lifecycle
 * notification wait, never an Android callback invocation or release poll. */
static fg_adaptive_present_result pres_retire_reserve(pres_buf *b,
        vk_present_retire_token *token) {
    int waiting=0;
    fg_adaptive_present_result decision;
    pthread_mutex_lock(&g_win_mu);
    for (;;) {
        decision=pres_adaptive_decide_locked(b);
        if (decision==FG_ADAPTIVE_PRESENT_WAIT) decision=pres_adaptive_wait_locked(b);
        if (decision!=FG_ADAPTIVE_PRESENT_APPLY) break;
        if (atomic_load(&g_ahb_off) || atomic_load(&g_ahb_surface_off) ||
                vk_present_retire_quarantined(&g_pres_retire.queue)) {
            decision=FG_ADAPTIVE_PRESENT_WINDOW; break;
        }
        /* Taking notify while still holding window closes both retirement
         * and lifecycle lost-wakeup gaps. No window lock survives the wait. */
        pthread_mutex_lock(&g_pres_retire_notify_mu);
        if (vk_present_retire_reserve(&g_pres_retire.queue,b->generation,token)) {
            pthread_mutex_unlock(&g_pres_retire_notify_mu); break;
        }
        if (!waiting) { pres_capacity_begin(b); waiting=1; }
        pthread_mutex_unlock(&g_win_mu);
        int error=0;
        if (b->generated) {
            struct timespec absolute=vk_present_retire_time(b->desired_ns);
            error=pthread_cond_timedwait(&g_pres_retire_notify_cv,&g_pres_retire_notify_mu,&absolute);
        } else pthread_cond_wait(&g_pres_retire_notify_cv,&g_pres_retire_notify_mu);
        pthread_mutex_unlock(&g_pres_retire_notify_mu);
        pthread_mutex_lock(&g_win_mu);
        if (error && error!=ETIMEDOUT) DIE("adaptive capacity notification wait: %d",error);
    }
    if (waiting) pres_capacity_end(b);
    if (decision==FG_ADAPTIVE_PRESENT_YIELD) {
        if (g_adaptive_presenting==(int)(b-g_pres)) g_adaptive_presenting=-1;
        pres_queue_locked((int)(b-g_pres));
    }
    pthread_mutex_unlock(&g_win_mu);
    return decision;
}
static void pres_retire_release_windows(void) {
    if (g_pres_retire.window) ANativeWindow_release(g_pres_retire.window);
    if (g_pres_retire.parent) ANativeWindow_release(g_pres_retire.parent);
    g_pres_retire.window=g_pres_retire.parent=NULL;
}
static void pres_retire_quarantine_surface(ASurfaceTransaction *tx,
        ASurfaceControl **surface,int *current) {
    atomic_store(&g_ahb_off,1);
    g_pres_retire.surface=*surface; g_pres_retire.transaction=tx;
    *surface=NULL; *current=-1;
    LOGV("window present: quarantined surface/context/buffer/window ownership retained until process exit; no removal/reuse/free");
}
static void pres_transaction_drain(pres_complete *contexts[VK_PRESENT_FLIGHT_CAPACITY],
        vk_present_flight *flight, int all) {
    while (flight->count && (all || flight->count == VK_PRESENT_FLIGHT_CAPACITY)) {
        unsigned index=flight->head;
        (void)pres_transaction_finish_until(contexts[index],0,flight->count);
        if (!vk_present_flight_retire(flight,index)) DIE("present transaction retirement order");
    }
}
static void pres_surface_remove(ASurfaceTransaction *tx, pres_complete *c, ASurfaceControl **surface, int *current) {
    if (!*surface) return;
    if (g_pres_retire.initialized || g_pres_retire.window) {
        uint64_t deadline=vk_present_retire_deadline();
        /* A mode cutover may already have joined the old worker. Removal of
         * that same async-owned surface still has bounded callback lifetime. */
        if (!g_pres_retire.initialized && !pres_retire_start()) {
            pres_retire_quarantine_surface(tx,surface,current); return;
        }
        vk_present_retire_token token;
        if (!vk_present_retire_drain(&g_pres_retire.queue,deadline) ||
                !vk_present_retire_reserve(&g_pres_retire.queue,
                    *current>=0 ? g_pres[*current].generation : 0,&token)) {
            (void)pres_retire_stop(deadline);
            pres_retire_quarantine_surface(tx,surface,current); return;
        }
        pres_complete *removal=g_pres_retire.contexts[token.index];
        removal->retire_async=1; removal->retire_token=token;
        ASurfaceTransaction_reparent(tx,*surface,NULL);
        pres_transaction(tx,removal,*surface,*current,NULL,0,1,1,0);
        if (!vk_present_retire_publish(&g_pres_retire.queue,token)) DIE("retirement removal publication");
        if (!pres_retire_stop(deadline)) {
            pres_retire_quarantine_surface(tx,surface,current); return;
        }
        for (unsigned i=0;i<VK_PRESENT_RETIRE_CAPACITY;i++) g_pres_retire.contexts[i]->retire_async=0;
    } else {
        c->retire_async=0;
        ASurfaceTransaction_reparent(tx, *surface, NULL);
        pres_transaction(tx, c, *surface, *current, NULL, 0, 0, 1, 0); /* legacy drain before removal */
    }
    pres_display_sweep(c, 1); /* query once more, then close unresolved old-generation fds */
    ASurfaceControl_release(*surface); *surface = NULL; *current = -1;
    pres_retire_release_windows();
}
/* Optional API 31 setter: never introduce a load-time dependency on it.
 * This presenter is the sole caller; request again on every new surface.
 * The setter returns void, so this logs a request, not verified readback. */
static int pres_backpressure(ASurfaceTransaction *tx, ASurfaceControl *surface, uint64_t generation) {
    static int resolved;
    static void (*set_backpressure)(ASurfaceTransaction *, ASurfaceControl *, bool);
    int api=android_get_device_api_level();
    if (!resolved) {
        resolved=1;
        if (api >= 31) set_backpressure=(void (*)(ASurfaceTransaction *, ASurfaceControl *, bool))
            dlsym(RTLD_DEFAULT,"ASurfaceTransaction_setEnableBackPressure");
    }
    int requested=vk_fg_locked() || vk_present_adaptive_backpressure_requested(vk_fg_adaptive(),
        getenv("TOTK_FG_ADAPTIVE_BACKPRESSURE"));
    int policy=vk_present_backpressure_policy(requested,getenv("TOTK_FRAMEGEN_BACKPRESSURE"),
        api,set_backpressure != NULL);
    if (policy == VK_PRESENT_BP_REQUEST) set_backpressure(tx,surface,true);
    if (vk_fg_locked() || vk_fg_adaptive()) LOGV("[framegen-backpressure] generation=%llu api=%d symbol=%d mode=%s; setter-void request only",
        (unsigned long long)generation,api,set_backpressure != NULL,
        policy == VK_PRESENT_BP_REQUEST ? "requested" :
        policy == VK_PRESENT_BP_DIAGNOSTIC_OFF ? "diagnostic-off-async" : "unavailable-serial");
    return policy;
}
static ASurfaceControl *pres_surface_create(ANativeWindow *window, ANativeWindow *parent, int display_id, uint64_t generation) {
    Dl_info provider = {0};
    dladdr((const void *)ASurfaceControl_createFromWindow, &provider);
    vk_present_extent extent = pres_frame_window_extent(window,GUEST_QUERY_PRESENTER);
    LOGV("window SurfaceControl: api=%d display=%d generation=%llu window=%p %dx%d format=%d explicit_parent=%p; direct NDK createFromWindow=%p transaction_create=%p provider=%s (no dlsym)",
         android_get_device_api_level(), display_id, (unsigned long long)generation, (void *)window,
         (int)extent.w, (int)extent.h, ANativeWindow_getFormat(window),
         (void *)parent, (void *)ASurfaceControl_createFromWindow, (void *)ASurfaceTransaction_create,
         provider.dli_fname ? provider.dli_fname : "unknown");
    errno = 0;
    ASurfaceControl *surface = ASurfaceControl_createFromWindow(window, "TOTK zero-copy");
    int error = errno;   /* This API returns only a pointer; errno is diagnostic, NOT a specified failure status. */
    LOGV("window SurfaceControl: ASurfaceControl_createFromWindow(NativeActivity window=%p) result=%p errno=%d (%s; errno not API status)",
         (void *)window, (void *)surface, error, strerror(error));
    if (!surface && parent) {
        errno = 0;
        surface = ASurfaceControl_createFromWindow(parent, "TOTK zero-copy attached");
        error = errno;
        LOGV("window SurfaceControl: ASurfaceControl_createFromWindow(attached Java Surface parent=%p) result=%p errno=%d (%s; errno not API status)",
             (void *)parent, (void *)surface, error, strerror(error));
    }
    if (!surface) LOGV("window SurfaceControl: no parentable surface api=%d display=%d generation=%llu explicit_parent=%p; using copy until window attachment changes (capture SurfaceComposerClient/SurfaceFlinger logcat)",
         android_get_device_api_level(), display_id, (unsigned long long)generation, (void *)parent);
    return surface;
}
static void pres_ahb_free(pres_ahb *a) {   /* only after both GPU and compositor have released this idle slot */
    for (int i = 0; i < 2; i++) if (a->view[i]) vkDestroyImageView(V.dev, a->view[i], NULL);
    if (a->image) vkDestroyImage(V.dev, a->image, NULL);
    vk_memory_release(a->mem, VK_MEMORY_PRESENT);
    if (a->buffer) AHardwareBuffer_release(a->buffer);
    memset(a, 0, sizeof *a);
}
/* native_handle_t and AHardwareBuffer_getNativeHandle are VNDK, not public NDK APIs. Match only the stable
   native_handle header ABI; resolve the accessor at runtime so a linker namespace denial safely selects copy. */
typedef struct { int version, numFds, numInts; int data[]; } pres_native_handle;
static const pres_native_handle *pres_ahb_handle(AHardwareBuffer *buffer) {
    static int tried;
    static const pres_native_handle *(*get_handle)(const AHardwareBuffer *);
    if (!tried) {
        tried = 1;
        void *lib = dlopen("libnativewindow.so", RTLD_NOW | RTLD_LOCAL);
        get_handle = lib ? (const pres_native_handle *(*)(const AHardwareBuffer *))dlsym(lib, "AHardwareBuffer_getNativeHandle") : NULL;
        if (!get_handle) {
            if (lib) dlclose(lib);
            LOGV("window AHB: native handle accessor unavailable in app linker namespace; using copy");
        }
        /* Keep a successful library reference alive along with its function pointer. */
    }
    return get_handle ? get_handle(buffer) : NULL;
}
static int pres_ahb_alloc(pres_ahb *a, uint32_t w, uint32_t h) {
    if (a->buffer && a->w == w && a->h == h) return 1;
    pres_ahb_free(a);
    /* Qualcomm gralloc may otherwise choose a generation-specific UBWC layout which a separately loaded Turnip
       cannot reliably infer from legacy gralloc metadata. CPU-readable usage requests an uncompressed allocation;
       it does NOT map/read the buffer or add any CPU work to presentation. Import that allocation as linear too. */
    AHardwareBuffer_Desc desc = {.width = w, .height = h, .layers = 1, .format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM,
        .usage = AHARDWAREBUFFER_USAGE_GPU_FRAMEBUFFER | AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE |
                 AHARDWAREBUFFER_USAGE_COMPOSER_OVERLAY | AHARDWAREBUFFER_USAGE_CPU_READ_RARELY};
    if (AHardwareBuffer_allocate(&desc, &a->buffer)) goto fail;
    AHardwareBuffer_Desc actual; AHardwareBuffer_describe(a->buffer, &actual);
    LOGV("window AHB allocation: %ux%u layers=%u format=%#x stride=%u usage=%#llx (requested CPU-readable linear)",
         actual.width, actual.height, actual.layers, actual.format, actual.stride, (unsigned long long)actual.usage);
    if (actual.width != w || actual.height != h || actual.layers != 1 || actual.format != desc.format || actual.stride < w ||
        (actual.usage & AHARDWAREBUFFER_USAGE_CPU_READ_MASK) != AHARDWAREBUFFER_USAGE_CPU_READ_RARELY) goto fail;
    const pres_native_handle *handle = pres_ahb_handle(a->buffer);
    if (!handle || handle->version != sizeof(pres_native_handle) || handle->numFds < 1 || handle->numInts < 0) {
        LOGV("window AHB: invalid/unavailable native handle; using copy"); goto fail;
    }
    /* Turnip's own AHB importer also uses data[0]; additional fds belong to gralloc metadata, not RGBA planes.
       Only the CPU-readable, single-layer RGBA allocation above is supported here. Never parse vendor integers. */
    int fd = handle->data[0];
    off_t allocation_size = lseek(fd, 0, SEEK_END);
    VkMemoryFdPropertiesKHR fd_props = {VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
    if (allocation_size <= 0 || vkGetMemoryFdPropertiesKHR(V.dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, fd, &fd_props)) {
        LOGV("window AHB: dma-buf size/memory properties unavailable; using copy"); goto fail;
    }
    /* RGBA8 storage with compatible UNORM/sRGB attachment views preserves the source transfer function,
       avoiding the decode-without-encode of an sRGB -> UNORM blit. */
    const VkFormat formats[2] = {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SRGB};
    for (unsigned i = 0; i < 2; i++) {
        VkFormatProperties properties; V.get_format_props(V.pd, formats[i], &properties);
        if (!(properties.linearTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)) {
            LOGV("window AHB: format %d cannot render linear; using copy", formats[i]); goto fail;
        }
    }
    VkImageFormatListCreateInfo list = {VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO, .viewFormatCount = 2, .pViewFormats = formats};
    /* DRM_FORMAT_MOD_LINEAR is zero. AHB-handle import cannot preserve this explicit layout in Mesa f7c93a1352:
       vk_android_init_deferred_image replaces tiling, and tu_AllocateMemory recomputes it from gralloc metadata.
       DMA_BUF import bypasses that deferred path and tu_image_init consumes our exact pitch, with UBWC disabled. */
    VkSubresourceLayout plane = {.offset = 0, .rowPitch = (VkDeviceSize)actual.stride * 4};
    VkImageDrmFormatModifierExplicitCreateInfoEXT modifier = {VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT,
        &list, .drmFormatModifier = 0, .drmFormatModifierPlaneCount = 1, .pPlaneLayouts = &plane};
    VkExternalMemoryImageCreateInfo ext = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO, &modifier,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, &ext, .flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT,
        .imageType = VK_IMAGE_TYPE_2D, .format = formats[0], .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | (vk_overlay_available() ? VK_IMAGE_USAGE_TRANSFER_DST_BIT : 0)};
    VkPhysicalDeviceImageDrmFormatModifierInfoEXT modifier_info = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT,
        &list, .drmFormatModifier = 0, .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VkPhysicalDeviceExternalImageFormatInfo external_info = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO,
        &modifier_info, .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    VkPhysicalDeviceImageFormatInfo2 query = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2, &external_info,
        .format = ci.format, .type = ci.imageType, .tiling = ci.tiling, .usage = ci.usage, .flags = ci.flags};
    VkExternalImageFormatProperties external_props = {VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
    VkImageFormatProperties2 properties = {VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2, &external_props};
    if (V.get_image_format_props2(V.pd, &query, &properties) ||
        !(external_props.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) ||
        !(external_props.externalMemoryProperties.compatibleHandleTypes & VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT) ||
        properties.imageFormatProperties.maxExtent.width < w || properties.imageFormatProperties.maxExtent.height < h) {
        LOGV("window AHB: explicit LINEAR dma-buf color image unsupported; using copy"); goto fail;
    }
    if (vkCreateImage(V.dev, &ci, NULL, &a->image)) goto fail;
    VkMemoryRequirements req; vkGetImageMemoryRequirements(V.dev, a->image, &req);
    uint32_t memory_types = fd_props.memoryTypeBits & req.memoryTypeBits;
    int type = vk_pick_type(memory_types, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type < 0) type = vk_pick_type(memory_types, 0);
    if (type < 0 || req.size > (VkDeviceSize)allocation_size || plane.rowPitch * h > (VkDeviceSize)allocation_size) {
        LOGV("window AHB: dma-buf memory type/size incompatible (types=%#x need=%llu have=%llu); using copy",
             memory_types, (unsigned long long)req.size, (unsigned long long)allocation_size); goto fail;
    }
    VkImportMemoryFdInfoKHR imp = {VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, .fd = dup(fd)};
    if (imp.fd < 0) goto fail;
    VkMemoryDedicatedAllocateInfo dedicated = {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, &imp, .image = a->image};
    VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &dedicated,
        .allocationSize = (VkDeviceSize)allocation_size, .memoryTypeIndex = (uint32_t)type};
    VkResult result = vk_memory_allocate(&mai, &a->mem, VK_MEMORY_PRESENT);
    if (result) { close(imp.fd); goto fail; }   /* Vulkan owns the duplicate only after successful import. */
    if (vkBindImageMemory(V.dev, a->image, a->mem, 0)) goto fail;
    VkImageSubresource subresource = {VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT, 0, 0}; VkSubresourceLayout layout;
    vkGetImageSubresourceLayout(V.dev, a->image, &subresource, &layout);
    VkImageDrmFormatModifierPropertiesEXT imported_modifier = {VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_PROPERTIES_EXT};
    if (V.get_image_modifier(V.dev, a->image, &imported_modifier)) goto fail;
    LOGV("window AHB imported layout: path=dma-buf-explicit tiling=DRM_FORMAT_MODIFIER_EXT modifier=%#llx planes=1 fds=%d stride=%u rowPitch=%llu expected=%llu offset=%llu size=%llu allocation=%llu memoryType=%d memoryTypes=%#x",
         (unsigned long long)imported_modifier.drmFormatModifier, handle->numFds, actual.stride,
         (unsigned long long)layout.rowPitch, (unsigned long long)plane.rowPitch, (unsigned long long)layout.offset,
         (unsigned long long)layout.size, (unsigned long long)allocation_size, type, memory_types);
    if (imported_modifier.drmFormatModifier != 0 || layout.offset || layout.rowPitch != plane.rowPitch ||
        layout.size > (VkDeviceSize)allocation_size || layout.rowPitch * h > (VkDeviceSize)allocation_size) {
        LOGV("window AHB: explicit LINEAR layout disagrees with gralloc stride/allocation; using copy"); goto fail;
    }
    for (int i = 0; i < 2; i++) {
        VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = a->image, .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = formats[i], .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        if (vkCreateImageView(V.dev, &vi, NULL, &a->view[i])) goto fail;
    }
    a->w = w; a->h = h;
    LOGV("window AHB picture %ux%u (explicit LINEAR dma-buf, CPU-readable allocation, UNORM/sRGB views, no readback)", w, h);
    return 1;
fail:
    pres_ahb_free(a); LOGV("window present: AHB allocation/import %ux%u failed; using copy", w, h);
    atomic_store(&g_ahb_off, 1); return 0;
}
/* Separate opt-in gralloc-compressed path. Never reinterpret a compressed fd
   as LINEAR: import the AHB itself so the qualified driver's metadata parser
   owns the layout. Any refusal retires these temporary resources and retries
   the unchanged CPU-readable explicit-LINEAR path, not the copy presenter. */
static int pres_ahb_alloc_ubwc(pres_ahb *a, uint32_t w, uint32_t h) {
    const char *reason="AHB allocation";
    /* Qualcomm GRALLOC_USAGE_PRIVATE_ALLOC_UBWC; a request, not compression
       proof. V.get_ahb_props is admitted only for a qualified driver build. */
    const uint64_t compressed_usage=UINT64_C(1)<<28;
    AHardwareBuffer_Desc desc={.width=w,.height=h,.layers=1,.format=AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM,
        .usage=AHARDWAREBUFFER_USAGE_GPU_FRAMEBUFFER | AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE |
            AHARDWAREBUFFER_USAGE_COMPOSER_OVERLAY | compressed_usage};
    if (AHardwareBuffer_allocate(&desc,&a->buffer)) goto fail;
    AHardwareBuffer_Desc actual; AHardwareBuffer_describe(a->buffer,&actual);
    LOGV("[present-ubwc] allocation request=%ux%u usage=%#llx actual=%ux%u format=%#x stride=%u usage=%#llx (UBWC requested, not public layout readback)",
        w,h,(unsigned long long)desc.usage,actual.width,actual.height,actual.format,actual.stride,
        (unsigned long long)actual.usage);
    reason="AHB descriptor";
    uint64_t gpu_usage=AHARDWAREBUFFER_USAGE_GPU_FRAMEBUFFER | AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE |
        AHARDWAREBUFFER_USAGE_COMPOSER_OVERLAY;
    if (actual.width!=w || actual.height!=h || actual.layers!=1 || actual.format!=desc.format || actual.stride<w ||
        (actual.usage & gpu_usage)!=gpu_usage ||
        (actual.usage & (AHARDWAREBUFFER_USAGE_CPU_READ_MASK | AHARDWAREBUFFER_USAGE_CPU_WRITE_MASK))) goto fail;
    VkAndroidHardwareBufferFormatPropertiesANDROID fp={VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_PROPERTIES_ANDROID};
    VkAndroidHardwareBufferPropertiesANDROID ap={VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID,.pNext=&fp};
    reason="Vulkan AHB format/properties";
    VkFormatFeatureFlags features=VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
        (vk_overlay_available() ? VK_FORMAT_FEATURE_TRANSFER_DST_BIT : 0);
    if (V.get_ahb_props(V.dev,a->buffer,&ap) || fp.format!=VK_FORMAT_R8G8B8A8_UNORM ||
        !ap.allocationSize || !ap.memoryTypeBits || (fp.formatFeatures & features)!=features) goto fail;
    const VkFormat formats[2]={VK_FORMAT_R8G8B8A8_UNORM,VK_FORMAT_R8G8B8A8_SRGB};
    for (unsigned i=0;i<2;i++) {
        VkFormatProperties properties; V.get_format_props(V.pd,formats[i],&properties);
        if ((properties.optimalTilingFeatures & features)!=features) goto fail;
    }
    VkImageFormatListCreateInfo list={VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO,
        .viewFormatCount=2,.pViewFormats=formats};
    VkExternalMemoryImageCreateInfo ext={VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,.pNext=&list,
        .handleTypes=VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID};
    VkImageCreateInfo ci={VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,.pNext=&ext,.flags=VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT,
        .imageType=VK_IMAGE_TYPE_2D,.format=fp.format,.extent={w,h,1},.mipLevels=1,.arrayLayers=1,
        .samples=VK_SAMPLE_COUNT_1_BIT,.tiling=VK_IMAGE_TILING_OPTIMAL,
        .usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | (vk_overlay_available() ? VK_IMAGE_USAGE_TRANSFER_DST_BIT : 0)};
    VkPhysicalDeviceExternalImageFormatInfo external={VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO,
        .pNext=&list,.handleType=VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID};
    VkPhysicalDeviceImageFormatInfo2 query={VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,.pNext=&external,
        .format=ci.format,.type=ci.imageType,.tiling=ci.tiling,.usage=ci.usage,.flags=ci.flags};
    VkAndroidHardwareBufferUsageANDROID usage={VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_USAGE_ANDROID};
    VkExternalImageFormatProperties external_properties={VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES,.pNext=&usage};
    VkImageFormatProperties2 properties={VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2,.pNext=&external_properties};
    reason="external AHB image capabilities";
    if (V.get_image_format_props2(V.pd,&query,&properties) ||
        !(external_properties.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) ||
        !(external_properties.externalMemoryProperties.compatibleHandleTypes & ext.handleTypes) ||
        properties.imageFormatProperties.maxExtent.width<w || properties.imageFormatProperties.maxExtent.height<h ||
        !(properties.imageFormatProperties.sampleCounts & VK_SAMPLE_COUNT_1_BIT) ||
        (actual.usage & usage.androidHardwareBufferUsage)!=usage.androidHardwareBufferUsage) goto fail;
    reason="AHB image creation";
    if (vkCreateImage(V.dev,&ci,NULL,&a->image)) goto fail;
    /* AHB import size/type come from the actual allocation, never an unbound
       deferred image's requirements. Binding lets the driver decode layout. */
    int type=vk_pick_type(ap.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type<0) type=vk_pick_type(ap.memoryTypeBits,0);
    reason="AHB memory type";
    if (type<0) goto fail;
    VkImportAndroidHardwareBufferInfoANDROID import={VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID,.buffer=a->buffer};
    VkMemoryDedicatedAllocateInfo dedicated={VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,.pNext=&import,.image=a->image};
    VkMemoryAllocateInfo allocate={VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.pNext=&dedicated,
        .allocationSize=ap.allocationSize,.memoryTypeIndex=(uint32_t)type};
    reason="AHB allocation/import binding";
    if (vk_memory_allocate(&allocate,&a->mem,VK_MEMORY_PRESENT) || vkBindImageMemory(V.dev,a->image,a->mem,0)) goto fail;
    VkMemoryRequirements requirements; vkGetImageMemoryRequirements(V.dev,a->image,&requirements);
    reason="post-bind AHB layout bounds";
    /* This experiment rejects ambiguous padding too: a mistakenly LINEAR
       interpretation can be smaller than a compressed AHB, not just larger. */
    if (!vk_present_ahb_layout_matches(ap.allocationSize,requirements.size,requirements.memoryTypeBits,(unsigned)type)) {
        LOGV("[present-ubwc] refused: post-bind requirement=%llu allocation=%llu memoryType=%d allowed=%#x",
            (unsigned long long)requirements.size,(unsigned long long)ap.allocationSize,type,requirements.memoryTypeBits);
        goto fail;
    }
    reason="AHB UNORM/sRGB views";
    for (unsigned i=0;i<2;i++) {
        VkImageViewCreateInfo view={VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,.image=a->image,
            .viewType=VK_IMAGE_VIEW_TYPE_2D,.format=formats[i],.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
        if (vkCreateImageView(V.dev,&view,NULL,&a->view[i])) goto fail;
    }
    a->w=w; a->h=h; a->ahb_import=1;
    LOGV("[present-ubwc] imported=%ux%u format=%u allocation=%llu post-bind=%llu memoryType=%d usage=%#llx; qualified-driver AHB layout, UNORM/sRGB views; actual compression/HWC admission require device evidence",
        w,h,fp.format,(unsigned long long)ap.allocationSize,(unsigned long long)requirements.size,type,
        (unsigned long long)actual.usage);
    return 1;
fail:
    pres_ahb_free(a);
    LOGV("[present-ubwc] %s failed/refused for %ux%u; retaining explicit LINEAR presentation",reason,w,h);
    return 0;
}
static int pres_ahb_prepare(pres_ahb *a, uint32_t w, uint32_t h, int ubwc) {
    static int refused;
    int compressed=ubwc && V.get_ahb_props && !refused;
    if (compressed) {
        if (a->buffer && a->w==w && a->h==h && a->ahb_import) return 1;
        pres_ahb_free(a); /* the caller owns an idle GPU/compositor-released slot */
        if (pres_ahb_alloc_ubwc(a,w,h)) return 1;
        refused=1; /* no repeated compressed allocations after a compatibility refusal */
    } else if (a->ahb_import) pres_ahb_free(a);
    return pres_ahb_alloc(a,w,h);
}
static int pres_pipeline(unsigned rotation) {
    VkPipeline *pipe = rotation ? g_pres_rotated_pipe : g_pres_pipe;
    VkPipelineLayout *layout = rotation ? &g_pres_rotated_layout : &g_pres_layout;
    if (pipe[0] && pipe[1]) return 1;
    VkShaderModule modules[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkSamplerCreateInfo sci = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_LINEAR, .minFilter = VK_FILTER_LINEAR,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST, .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
    if (!g_pres_sampler && vkCreateSampler(V.dev, &sci, NULL, &g_pres_sampler)) goto fail;
    if (!g_pres_pixel_sampler && vk_native_ui_enabled()) {
        sci.magFilter = sci.minFilter = VK_FILTER_NEAREST;
        if (vkCreateSampler(V.dev, &sci, NULL, &g_pres_pixel_sampler)) goto fail;
    }
    VkDescriptorSetLayoutBinding binding = {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT};
    VkDescriptorSetLayoutCreateInfo dci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &binding};
    if (!g_pres_dsl && vkCreateDescriptorSetLayout(V.dev, &dci, NULL, &g_pres_dsl)) goto fail;
    VkPushConstantRange range = {.stageFlags = VK_SHADER_STAGE_VERTEX_BIT, .size = (rotation ? 8 : 4) * sizeof(float)};
    VkPipelineLayoutCreateInfo lci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &g_pres_dsl,
        .pushConstantRangeCount = 1, .pPushConstantRanges = &range};
    if (vkCreatePipelineLayout(V.dev, &lci, NULL, layout)) goto fail;
    for (int i = 0; i < 2; i++) {
        VkShaderModuleCreateInfo sm = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .codeSize = i ? sizeof pres_frag_spv : rotation ? sizeof pres_rotated_vert_spv : sizeof pres_vert_spv,
            .pCode = i ? pres_frag_spv : rotation ? pres_rotated_vert_spv : pres_vert_spv};
        if (vkCreateShaderModule(V.dev, &sm, NULL, &modules[i])) goto fail;
    }
    VkPipelineShaderStageCreateInfo stages[2] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = modules[0], .pName = "main"},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = modules[1], .pName = "main"}};
    VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .polygonMode = VK_POLYGON_MODE_FILL, .lineWidth = 1};
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState att = {.colorWriteMask = 0xf};
    VkPipelineColorBlendStateCreateInfo blend = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1, .pAttachments = &att};
    const VkDynamicState dynamic[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 2, .pDynamicStates = dynamic};
    for (int i = 0; i < 2; i++) {
        VkFormat format = i ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
        VkPipelineRenderingCreateInfo rendering = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = 1, .pColorAttachmentFormats = &format};
        VkGraphicsPipelineCreateInfo ci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &rendering, .stageCount = 2, .pStages = stages,
            .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vp, .pRasterizationState = &rs,
            .pMultisampleState = &ms, .pColorBlendState = &blend, .pDynamicState = &dy, .layout = *layout};
        if (vk_pcache_graphics(&ci, &pipe[i])) goto fail;
    }
    for (int i = 0; i < 2; i++) vkDestroyShaderModule(V.dev, modules[i], NULL);
    return 1;
fail:
    for (int i = 0; i < 2; i++) {
        if (modules[i]) vkDestroyShaderModule(V.dev, modules[i], NULL);
        if (pipe[i]) { vkDestroyPipeline(V.dev, pipe[i], NULL); pipe[i] = VK_NULL_HANDLE; }
    }
    if (*layout) { vkDestroyPipelineLayout(V.dev, *layout, NULL); *layout = VK_NULL_HANDLE; }
    if (!g_pres_layout && !g_pres_rotated_layout) {
        if (g_pres_dsl) { vkDestroyDescriptorSetLayout(V.dev, g_pres_dsl, NULL); g_pres_dsl = VK_NULL_HANDLE; }
        if (g_pres_sampler) { vkDestroySampler(V.dev, g_pres_sampler, NULL); g_pres_sampler = VK_NULL_HANDLE; }
        if (g_pres_pixel_sampler) { vkDestroySampler(V.dev, g_pres_pixel_sampler, NULL); g_pres_pixel_sampler = VK_NULL_HANDLE; }
    }
    LOGV("window present: AHB blit pipeline failed; using copy"); atomic_store(&g_ahb_off, 1); return 0;
}
static int pres_ahb_draw(int i, const tex_t *t, vk_tex *x, int srgb, uint32_t ow, uint32_t oh,
                         int32_t cx, int32_t cy, uint32_t w, uint32_t h, int32_t dx, int32_t dy, int32_t dw, int32_t dh,
                         const vk_post_output *post, unsigned rotation, int ubwc, VkCommandBuffer prepared_cb) {
    if (!V.ahb_present || atomic_load(&g_ahb_off) || atomic_load(&g_ahb_surface_off)) return 0;
    VkFormat f = vk_tex_format(x);
    if (vk_tex_is3d(x) || (f != VK_FORMAT_R8G8B8A8_UNORM && f != VK_FORMAT_R8G8B8A8_SRGB &&
        f != VK_FORMAT_B8G8R8A8_UNORM && f != VK_FORMAT_B8G8R8A8_SRGB &&
        f != VK_FORMAT_A8B8G8R8_UNORM_PACK32 && f != VK_FORMAT_A8B8G8R8_SRGB_PACK32)) goto fail;
    view_t view = {.set_levels = 1, .nlevels = 1, .set_layers = 1, .nlayers = 1, .set_target = 1, .target = 1,
        .set_swizzle = 1, .swz = {2, 3, 4, 5}};   /* raw RGBA, like the old image blit, not the game's sampling swizzle */
    VkImageView src = post ? post->view : vk_tex_view(x, t, &view, 0); if (!src) goto fail;
    pres_ahb *a = &g_ahb[i];
    vk_present_extent native = {ow,oh};
    vk_present_rect draw = {dx,dy,(uint32_t)dw,(uint32_t)dh};
    if (rotation) {
        native = vk_present_rotate_extent(ow,oh,rotation);
        draw = vk_present_rotate_rect(draw,ow,oh,rotation);
    }
    if (!pres_pipeline(rotation) || !pres_ahb_prepare(a, native.w, native.h, ubwc)) return 0;
    VkPipelineLayout layout = rotation ? g_pres_rotated_layout : g_pres_layout;
    VkPipeline pipeline = rotation ? g_pres_rotated_pipe[srgb] : g_pres_pipe[srgb];
    if (!prepared_cb) {
        if (vk_lag_uniform_private()) vk_ub_touch((uint64_t)(uintptr_t)t->mem, t->L.size); else vk_ub_flush();
        vk_end_rendering_at(VK_PASS_END_PRESENT, 1);
    }
    VkCommandBuffer cb = prepared_cb ? prepared_cb : vk_cb();
    int scope = prepared_cb ? -1 : vk_native_gpu_begin(cb,VK_NUI_PRESENT_AHB); vk_barrier_all(cb);
    VkImageMemoryBarrier2 ib = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_NONE,
        .dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, .dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
        .oldLayout = a->foreign ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT, .dstQueueFamilyIndex = V.qfam, .image = a->image,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &ib};
    vkCmdPipelineBarrier2(cb, &di);
    VkRenderingAttachmentInfo att = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = a->view[srgb],
        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .clearValue.color = {{0, 0, 0, 1}}};
    VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, {native.w, native.h}}, .layerCount = 1, .colorAttachmentCount = 1, .pColorAttachments = &att};
    VkDescriptorSet set = vk_alloc_set(g_pres_dsl);
    VkSampler sampler = g_pres_pixel_sampler && w == (uint32_t)dw && h == (uint32_t)dh ? g_pres_pixel_sampler : g_pres_sampler;
    VkDescriptorImageInfo image = {.sampler = sampler, .imageView = src, .imageLayout = VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet write = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 0, .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &image};
    vkUpdateDescriptorSets(V.dev, 1, &write, 0, NULL);
    vkCmdBeginRendering(cb, &ri);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0, NULL);
    float iw = post ? post->w : vk_tex_width(x, 0), ih = post ? post->h : vk_tex_height(x, 0);
    if (rotation) {
        float push[8] = {(float)cx / iw, (float)cy / ih, (float)w / iw, (float)h / ih};
        vk_present_rotation_matrix(rotation,push+4);
        vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof push, push);
    } else {
        float push[4] = {(float)cx / iw, (float)cy / ih, (float)w / iw, (float)h / ih};
        vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof push, push);
    }
    VkViewport vp = {(float)draw.x, (float)draw.y, (float)draw.w, (float)draw.h, 0, 1};
    VkRect2D sc = {{draw.x, draw.y}, {draw.w, draw.h}};
    vkCmdSetViewport(cb, 0, 1, &vp); vkCmdSetScissor(cb, 0, 1, &sc); vkCmdDraw(cb, 3, 1, 0, 0);
    vkCmdEndRendering(cb);
    vk_overlay_record(cb, a->image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, ow, oh, (unsigned)i, rotation);
    ib.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT; ib.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    ib.dstStageMask = VK_PIPELINE_STAGE_2_NONE; ib.dstAccessMask = 0;
    ib.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; ib.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    ib.srcQueueFamilyIndex = V.qfam; ib.dstQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT;
    vkCmdPipelineBarrier2(cb, &di);
    /* Ownership becomes foreign only after this command stream is submitted;
     * private prepare/cancel must retain the slot's actual prior GPU state. */
    if (!prepared_cb) vk_native_gpu_end(cb,scope);
    g_bound_pipe = VK_NULL_HANDLE; g_dyn_dirty = 1; memset(g_bound_set[0], 0, sizeof g_bound_set[0]);
    return 1;
fail:
    LOGV("window present: source cannot be sampled for AHB blit; using copy"); atomic_store(&g_ahb_off, 1); return 0;
}
#endif
uint64_t vk_window_set(ANativeWindow *w, ANativeWindow *surface_parent, int display_id) {
    pthread_mutex_lock(&g_win_mu);
    if (w) ANativeWindow_acquire(w);
    if (g_win) ANativeWindow_release(g_win);
#ifdef __ANDROID__
    if (surface_parent) ANativeWindow_acquire(surface_parent);
    if (g_win_parent) ANativeWindow_release(g_win_parent);
    g_win_parent = surface_parent; g_win_display_id = display_id;
    atomic_store(&g_ahb_surface_off, 0);
#endif
    g_win = w; g_win_w = g_win_h = 0; g_win_generation++; pthread_cond_signal(&g_pres_cv);
    if (pres_slots_requested()) pres_slots_end_locked();
    pres_adaptive_wake_locked();
    const char *prerotate = getenv("TOTK_PRESENT_PREROTATE");
    g_win_prerotate = prerotate && !strcmp(prerotate,"1"); g_win_transform_hint = -1;
    const char *ubwc = getenv("TOTK_PRESENT_UBWC");
    g_win_ubwc = ubwc && !strcmp(ubwc,"1");
    uint64_t epoch = ++g_win_attachment_epoch;
    vk_present_extent extent = pres_window_extent(w);
    atomic_store_explicit(&g_guest_window_wh, pres_pack_extent(extent.w, extent.h), memory_order_relaxed);
    pres_guest_fit_refresh();
    if (w) { g_win_pw = extent.w; g_win_ph = extent.h; }
#ifdef __ANDROID__
    if (w) totk_perf_window(w);
#endif
    if (w) vk_native_window_size(g_win_pw, g_win_ph);
    pthread_mutex_unlock(&g_win_mu);
    vk_fg_reset();   /* atomic request only; recording owner retires/invalidates history */
    fprintf(stderr, "[nvn-vk] window %s\n", w ? "attached" : "detached");
    return epoch;
}
/* UI-thread hint listener supplies the exact attached root's transform. A
   quarter-turn/180 change invalidates queued jobs even when width/height do not
   change; an old attachment's listener cannot rotate the replacement window. */
void vk_window_transform_hint(ANativeWindow *w, uint64_t epoch, int hint) {
    pthread_mutex_lock(&g_win_mu);
    if (w && w == g_win && epoch == g_win_attachment_epoch && (g_win_prerotate || g_win_ubwc) &&
        hint != g_win_transform_hint) {
        g_win_transform_hint = hint; g_win_generation++;
        if (pres_slots_requested()) pres_slots_end_locked();
        pres_adaptive_wake_locked();
        int turns = vk_present_transform_turns(hint);
        LOGV("[present-prerotate] generation=%llu hint=%d turns=%d rotate=%d inverse=%d ubwc=%d; %s (composer/HWC identity not verified)",
            (unsigned long long)g_win_generation,hint,turns,g_win_prerotate && turns>0,
            g_win_prerotate && turns>=0 ? vk_present_turns_transform((4-(unsigned)turns)&3) : 0,
            g_win_ubwc && turns>0,
            turns >= 0 ? "runtime root hint accepted" : "unavailable/unsupported hint: identity and LINEAR retained");
        pthread_cond_signal(&g_pres_cv);
    }
    pthread_mutex_unlock(&g_win_mu);
}
/* UI lifecycle notification, not frame polling. Ignore callbacks for retired
   windows; a changed guest extent invalidates queued jobs from the old epoch. */
void vk_window_resized(ANativeWindow *w, uint64_t epoch) {
    pthread_mutex_lock(&g_win_mu);
    if (w && w == g_win && epoch == g_win_attachment_epoch) {
        vk_present_extent old = pres_guest_window(), extent = pres_window_extent(w);
        if (old.w != extent.w || old.h != extent.h) {
            atomic_store_explicit(&g_guest_window_wh, pres_pack_extent(extent.w, extent.h), memory_order_relaxed);
            pres_guest_fit_refresh();
            if (g_win_prerotate || g_win_ubwc) {
                g_win_pw = extent.w; g_win_ph = extent.h;
                vk_native_window_size(extent.w,extent.h); /* native UI stays in logical-window coordinates */
            }
            if (g_pres_guest_output || g_win_prerotate || g_win_ubwc) {
                g_win_generation++;
                pthread_cond_signal(&g_pres_cv);
                pres_adaptive_wake_locked();
            }
        }
    }
    pthread_mutex_unlock(&g_win_mu);
}
/* Diagnostics: a frame whose picture is almost all black right after a normal one (the brief black flashes) is logged
   with what happened since the last frame shown, and saved as shots/black_<frame>.png. TOTK_VK_BLACK=0: off. */
#ifdef TOTK_OHOS
/* Local flicker (HarmonyOS, the user's reports): the shown picture split into 16x9 cells; a cell whose mean brightness
   jumps by more than TOTK_OHOS_FLICKER (default 20 of 255) against the previous frame and is back within the next frame
   is an isolated spike (the reference's "isolated tile spikes" metric, .scratch/flicker.py over camera recordings, done
   here on the present readback). A frame where more than half the cells spike together is a global event (a cut, the
   black flashes black_check logs) and is counted apart. Per 300 presents: spikes per 100 frames, frames with spikes,
   global events, the three cells with the most spikes; the first 6 spike events save the frame and its two neighbours
   (every 4th pixel) as shots/flicker_<frame>_{a,b,c}.png. Diagnostic, off by default: TOTK_OHOS_FLICKER=<threshold>
   (20 works) turns it on. Present thread; ~0.5 ms a frame. */
enum { FC_W = 16, FC_H = 9, FC_N = FC_W * FC_H };
static _Atomic uint64_t g_fl_spikes, g_fl_frames, g_fl_global, g_fl_checked, g_fl_cell[FC_N];
static void flicker_check(const pres_buf *b) {
    static int thr = -1; if (thr < 0) { const char *e = getenv("TOTK_OHOS_FLICKER"); thr = e && *e ? atoi(e) : 0; }   /* a diagnostic: off unless set (20 is a good threshold) */
    if (thr <= 0 || !b->ptr || b->w < 64 || b->h < 36) return;
    static float m[3][FC_N]; static uint64_t fr[3]; static int have; static uint32_t pw, ph; static uint8_t *small[3]; static unsigned saved;
    uint32_t sw = b->w / 4, sh = b->h / 4;
    if (sw != pw || sh != ph) { for (int i = 0; i < 3; i++) { free(small[i]); small[i] = calloc((size_t)sw * sh, 4); } pw = sw; ph = sh; have = 0; }
    /* shift the ring: [0] oldest, [2] this frame */
    { uint8_t *t = small[0]; small[0] = small[1]; small[1] = small[2]; small[2] = t; }
    memmove(m[0], m[1], sizeof m[0] * 2); fr[0] = fr[1]; fr[1] = fr[2]; fr[2] = b->frame;
    float sum[FC_N]; uint32_t cnt[FC_N]; memset(sum, 0, sizeof sum); memset(cnt, 0, sizeof cnt);
    uint8_t *dst = small[2];
    for (uint32_t y = 0; y < sh; y++) {
        const uint8_t *row = b->ptr + (size_t)(y * 4 + 2) * b->w * 4; unsigned cy = y * FC_H / sh;
        for (uint32_t x = 0; x < sw; x++) {
            const uint8_t *p = row + (size_t)(x * 4 + 2) * 4; unsigned c = cy * FC_W + x * FC_W / sw;
            sum[c] += (float)(p[0] + p[1] + p[2]); cnt[c]++;
            memcpy(dst + ((size_t)y * sw + x) * 4, p, 4);
        }
    }
    for (int c = 0; c < FC_N; c++) m[2][c] = cnt[c] ? sum[c] / (3.0f * cnt[c]) : 0.f;
    if (have < 2) { have++; return; }
    if (fr[1] != fr[0] + 1 || fr[2] != fr[1] + 1) return;   /* only consecutive presented frames */
    atomic_fetch_add_explicit(&g_fl_checked, 1, memory_order_relaxed);
    int spikes = 0; int which[FC_N];
    for (int c = 0; c < FC_N; c++) {
        float d1 = m[1][c] - m[0][c], d2 = m[2][c] - m[1][c], back = m[2][c] - m[0][c];
        if ((d1 > thr || d1 < -thr) && (d1 > 0) != (d2 > 0) && back < thr * 0.5f && back > -thr * 0.5f) which[spikes++] = c;
    }
    if (!spikes) return;
    if (spikes > FC_N / 2) { atomic_fetch_add_explicit(&g_fl_global, 1, memory_order_relaxed); return; }
    atomic_fetch_add_explicit(&g_fl_spikes, (uint64_t)spikes, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_fl_frames, 1, memory_order_relaxed);
    for (int i = 0; i < spikes; i++) atomic_fetch_add_explicit(&g_fl_cell[which[i]], 1, memory_order_relaxed);
    if (saved < 6) {
        saved++; mkdir("shots", 0755); char path[256]; const char *tag = "abc";
        for (int i = 0; i < 3; i++) { snprintf(path, sizeof path, "shots/flicker_%06llu_%c.png", (unsigned long long)fr[1], tag[i]); write_png(path, small[i], sw, sh, (size_t)sw * 4); }
        LOGV("[flicker] frame %llu: %d cell(s) spiked (first cell %d: %.0f -> %.0f -> %.0f of 255); saved shots/flicker_%06llu_{a,b,c}.png",
             (unsigned long long)fr[1], spikes, which[0], m[0][which[0]], m[1][which[0]], m[2][which[0]], (unsigned long long)fr[1]);
    }
}
/* White blobs (oot-a127): foliage or a doorway drawn white for one frame, mostly on the first frame after a camera cut
   (the walking route's cut to the treehouse: 6 of 7 baseline walks; oot/frame_rec.py --blob finds them in recordings).
   Near-white samples (all channels >= 235) on a grid of every 8th pixel of every 8th row, the overlay box left out;
   frame F is an event when its count is >= max(N, S/1000) and >= 4x both neighbours' + S/2000 (S: samples). Logged with
   the counters of F-1 / F / F+1, F saved (every 4th pixel) as shots/blob_<F>.png for the first 3, and with
   TOTK_VK_PASSPROBE=1 the passes of F are compared with F+1's (the same view after the cut) to name the first pass that
   goes white. Diagnostic, off by default: TOTK_OHOS_BLOB=1 (N = 48) or =N. Present thread; ~0.1 ms a frame. */
static _Atomic uint64_t g_blob_checked, g_blob_events, g_blob_gaps, g_blob_peak, g_blob_moved;
static int blob_nmin(void) { static int nmin = -1; if (nmin < 0) { const char *e = getenv("TOTK_OHOS_BLOB"); int v = e && *e ? atoi(e) : 0; nmin = v <= 0 ? 0 : v == 1 ? 48 : v; } return nmin; }
static uint32_t blob_count(const uint8_t *px, uint32_t w, uint32_t h, uint32_t *samples) {   /* near-white samples on a ~50k grid, the overlay box left out */
    uint32_t step = 1; while ((uint64_t)(w / (step + 1)) * (h / (step + 1)) >= 50000 && step < 16) step++;
    uint32_t cnt = 0, S = 0, ytop = (uint32_t)(h * 0.16), xov = (uint32_t)(w * 0.72);
    for (uint32_t y = step / 2; y < h; y += step) {
        const uint8_t *row = px + (size_t)y * w * 4;
        for (uint32_t x = step / 2; x < w; x += step) {
            if (y < ytop && x > xov) continue;
            const uint8_t *p = row + (size_t)x * 4; S++;
            if (p[0] >= 235 && p[1] >= 235 && p[2] >= 235) cnt++;
        }
    }
    *samples = S; return cnt;
}
/* Isolated white frames are not shown (oot-a131, default on; TOTK_OHOS_BLOB_DROP=0: off). The ladder-to-treehouse camera
   cut draws foliage and a doorway white for one frame in most walks (a126b, a130: the frame itself holds the white, it is
   not a presentation race), and the standing view does so about once in 1650 frames; the pass that makes it is not
   found yet. A frame whose near-white count jumps against the frame before (>= max(48, S/1000) and >= 4x + S/2000, the
   rule that finds them in recordings) is not posted, so the screen keeps the frame before for one more game frame; never
   two in a row and at most 3 in 32 frames, so a scene that really turns white shows from its second frame. The rule only sees the frame
   before, so the first frame of any sudden rise in white (a cut to a bright view) is held too. Present thread, ~0.5 ms. */
static _Atomic uint64_t g_blob_dropped;
static int blob_drop(const pres_buf *b) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("TOTK_OHOS_BLOB_DROP"); on = !(e && *e == '0'); LOGV("TOTK_OHOS_BLOB_DROP=%s: isolated white frames %s", e ? e : "(unset)", on ? "are not shown" : "are shown"); }
    if (!on || !b->ptr || b->w < 64 || b->h < 36) return 0;
    static uint32_t prev; static uint64_t prev_fr, drops[3]; static int last_dropped;
    uint32_t S, cnt = blob_count(b->ptr, b->w, b->h, &S);
    /* never two in a row, and at most 3 in 32 frames (oot-a134): the camera cut draws several separate white frames a
       few frames apart (a132-base1 598 and 601), which one drop in 32 (a132) let through; alternating white content
       loses at most 3 frames in 32 */
    int recent = 0; for (int k = 0; k < 3; k++) recent += drops[k] && b->frame <= drops[k] + 32;
    int drop = prev_fr && b->frame == prev_fr + 1 && !last_dropped && recent < 3 &&
               cnt >= MAXV(48u, S / 1000) && cnt >= 4 * prev + S / 2000;
    if (drop) {
        drops[0] = drops[1]; drops[1] = drops[2]; drops[2] = b->frame;
        uint64_t n = atomic_fetch_add_explicit(&g_blob_dropped, 1, memory_order_relaxed) + 1;
        if (n <= 20 || !(n & (n - 1))) LOGV("[blob] frame %llu not shown: near-white %u of %u samples after %u in the frame before (%llu so far)",
                                             (unsigned long long)b->frame, cnt, S, prev, (unsigned long long)n);
    }
    prev = cnt; prev_fr = b->frame; last_dropped = drop;
    return drop;
}
/* the count of the bytes the window copy read (taken just before it): a129 counted after the post and never saw the
   white the recording of the same frames shows, as if the presentation buffer still changed after the copy */
static uint64_t g_blob_pre_frame; static uint32_t g_blob_pre_cnt;
static void blob_pre_copy(const pres_buf *b) {
    if (!blob_nmin() || !b->ptr || b->w < 64 || b->h < 36) return;
    uint32_t S; g_blob_pre_cnt = blob_count(b->ptr, b->w, b->h, &S); g_blob_pre_frame = b->frame;
}
static void blob_check(const pres_buf *b) {
    int nmin = blob_nmin();
    if (!nmin || !b->ptr || b->w < 64 || b->h < 36) return;
    {   static uint32_t lw, lh; if (b->w != lw || b->h != lh) { lw = b->w; lh = b->h; LOGV("[blob] presented buffers now %ux%u", b->w, b->h); } }
    uint32_t S, after = blob_count(b->ptr, b->w, b->h, &S), cnt = after;
    if (g_blob_pre_frame == b->frame) {   /* what the window got */
        cnt = g_blob_pre_cnt;
        uint32_t lo = MINV(cnt, after), hi = MAXV(cnt, after);
        if (hi >= MAXV((uint32_t)nmin, S / 1000) && hi >= 4 * lo + S / 2000) {
            uint64_t m = atomic_fetch_add_explicit(&g_blob_moved, 1, memory_order_relaxed) + 1;
            if (m <= 20) LOGV("【红】[blob] frame %llu: the presentation buffer changed after the window copy read it: near-white %u at the copy, %u after the post (of %u)",
                              (unsigned long long)b->frame, cnt, after, S);
        }
    }
    static uint64_t fr[3], ctr[3][10]; static uint32_t c[3]; static int have, saved, keep_ok; static uint8_t *keep; static uint32_t kw, kh; static uint64_t keep_fr;
    fr[0] = fr[1]; fr[1] = fr[2]; fr[2] = b->frame; c[0] = c[1]; c[1] = c[2]; c[2] = cnt;
    memcpy(ctr[0], ctr[1], sizeof ctr[0]); memcpy(ctr[1], ctr[2], sizeof ctr[0]); memcpy(ctr[2], b->ctr, sizeof ctr[0]);
    uint32_t need = MAXV((uint32_t)nmin, S / 1000);
    {   uint64_t pk = atomic_load_explicit(&g_blob_peak, memory_order_relaxed);   /* the window's largest near-white count (per mille of S) */
        uint64_t pm = S ? (uint64_t)cnt * 1000 / S : 0; if (pm > pk) atomic_store_explicit(&g_blob_peak, pm, memory_order_relaxed); }
    if (saved < 3 && cnt >= need && cnt >= 4 * c[1] + S / 2000) {   /* a candidate against the frame before: keep it until the next frame decides */
        uint32_t sw = b->w / 4, sh = b->h / 4;
        if (!keep || kw != sw || kh != sh) { free(keep); keep = malloc((size_t)sw * sh * 4); kw = sw; kh = sh; }
        if (keep) { for (uint32_t y = 0; y < sh; y++) for (uint32_t x = 0; x < sw; x++) memcpy(keep + ((size_t)y * sw + x) * 4, b->ptr + ((size_t)(y * 4) * b->w + x * 4) * 4, 4); keep_ok = 1; keep_fr = b->frame; }
    }
    if (have < 2) { have++; return; }
    if (fr[1] != fr[0] + 1 || fr[2] != fr[1] + 1) { atomic_fetch_add_explicit(&g_blob_gaps, 1, memory_order_relaxed); return; }   /* only consecutive presented frames */
    atomic_fetch_add_explicit(&g_blob_checked, 1, memory_order_relaxed);
    if (!(c[1] >= need && c[1] >= 4 * MAXV(c[0], c[2]) + S / 2000)) return;
    uint64_t n = atomic_fetch_add_explicit(&g_blob_events, 1, memory_order_relaxed) + 1;
    if (n > 40) return;
    uint64_t d0[10], d1[10]; for (int i = 0; i < 10; i++) { d0[i] = ctr[1][i] - ctr[0][i]; d1[i] = ctr[2][i] - ctr[1][i]; }
    LOGV("【红】[blob] white blob at frame %llu: near-white samples %u -> %u -> %u of %u; frame %llu / the next: %llu / %llu draws, %llu / %llu passes, "
         "%llu / %llu skipped, %llu / %llu images made, %llu / %llu shared reuses, %llu / %llu uploads, %llu / %llu re-uploads, %llu / %llu swept",
         (unsigned long long)fr[1], c[0], c[1], c[2], S, (unsigned long long)fr[1], (unsigned long long)d0[0], (unsigned long long)d1[0], (unsigned long long)d0[1], (unsigned long long)d1[1],
         (unsigned long long)d0[2], (unsigned long long)d1[2], (unsigned long long)d0[3], (unsigned long long)d1[3], (unsigned long long)d0[4], (unsigned long long)d1[4],
         (unsigned long long)d0[5], (unsigned long long)d1[5], (unsigned long long)d0[6], (unsigned long long)d1[6], (unsigned long long)d0[7], (unsigned long long)d1[7]);
    if (keep_ok && keep_fr == fr[1] && saved < 3) {
        saved++; mkdir("shots", 0755); char path[256]; snprintf(path, sizeof path, "shots/blob_%06llu.png", (unsigned long long)fr[1]);
        write_png(path, keep, kw, kh, (size_t)kw * 4); LOGV("[blob] saved %s", path);
    }
    keep_ok = 0;
    if (n <= 6) probe_report(fr[1], fr[2]);   /* TOTK_VK_PASSPROBE=1: against the frame after it, the same view after a cut */
}
void vk_blob_report(uint64_t frame) {
    {   static uint64_t last; uint64_t d = atomic_load_explicit(&g_blob_dropped, memory_order_relaxed);
        if (d != last) { LOGV("frame %llu, isolated white frames not shown: %llu in the last 300, %llu so far (TOTK_OHOS_BLOB_DROP)", (unsigned long long)frame, (unsigned long long)(d - last), (unsigned long long)d); last = d; } }
    uint64_t checked = atomic_exchange_explicit(&g_blob_checked, 0, memory_order_relaxed);
    uint64_t gaps = atomic_exchange_explicit(&g_blob_gaps, 0, memory_order_relaxed), peak = atomic_exchange_explicit(&g_blob_peak, 0, memory_order_relaxed);
    if (checked || gaps) LOGV("frame %llu, white blobs over %llu shown frames (%llu skipped: not consecutive; most near-white %.1f%%): %llu so far, %llu frames whose buffer changed after the window copy (TOTK_OHOS_BLOB)",
                              (unsigned long long)frame, (unsigned long long)checked, (unsigned long long)gaps, peak / 10.0,
                              (unsigned long long)atomic_load_explicit(&g_blob_events, memory_order_relaxed),
                              (unsigned long long)atomic_load_explicit(&g_blob_moved, memory_order_relaxed));
}
void vk_flicker_report(uint64_t frame) {
    uint64_t checked = atomic_exchange_explicit(&g_fl_checked, 0, memory_order_relaxed);
    if (!checked) return;
    uint64_t spikes = atomic_exchange_explicit(&g_fl_spikes, 0, memory_order_relaxed), frames = atomic_exchange_explicit(&g_fl_frames, 0, memory_order_relaxed);
    uint64_t global = atomic_exchange_explicit(&g_fl_global, 0, memory_order_relaxed);
    uint64_t cells[FC_N]; int top[3] = {-1, -1, -1};
    for (int c = 0; c < FC_N; c++) {
        cells[c] = atomic_exchange_explicit(&g_fl_cell[c], 0, memory_order_relaxed);
        for (int k = 0; k < 3; k++) if (top[k] < 0 || cells[c] > cells[top[k]]) { for (int j = 2; j > k; j--) top[j] = top[j - 1]; top[k] = c; break; }
    }
    char cellinfo[160] = ""; size_t o = 0;
    for (int k = 0; k < 3 && top[k] >= 0 && cells[top[k]]; k++) o += (size_t)snprintf(cellinfo + o, sizeof cellinfo - o, " cell %d (col %d row %d) %llu", top[k], top[k] % FC_W, top[k] / FC_W, (unsigned long long)cells[top[k]]);
    LOGV("frame %llu, flicker over %llu shown frames: %.1f isolated cell spikes per 100 frames in %llu frames, %llu global changes;%s",
         (unsigned long long)frame, (unsigned long long)checked, spikes * 100.0 / (double)checked, (unsigned long long)frames, (unsigned long long)global, o ? cellinfo : " none");
}
#endif
static void black_check(const pres_buf *b) {
    /* A flash: a frame far darker than the last few, with the brightness back within five frames (a fade to black stays
       dark and is not one). The dark frame is kept until that is known; up to 40 are logged and 5 saved. */
    static int on = -1; if (on < 0) { const char *e = getenv("TOTK_VK_BLACK"); on = !(e && !strcmp(e, "0")); }
    static double avg = -1, dark_m, dark_avg; static uint64_t pctr[10], dctr[10], pframe, dframe, dprev; static int saved, logged, pending;
    static uint8_t *keep; static uint32_t kw, kh;
    if (!on) return;
    double sum = 0;   /* brightness at 49 points across the picture */
    for (uint32_t gy = 1; gy < 8; gy++) for (uint32_t gx = 1; gx < 8; gx++) { const uint8_t *p = b->ptr + ((size_t)(b->h * gy / 8) * b->w + b->w * gx / 8) * 4; sum += (p[0] + p[1] + p[2]) / 3.0; }
    double m = sum / 49;
    if (pending) {
        if (m >= dark_avg * 0.75) {   /* back: it was a flash */
            if (logged++ < 40) {
                uint64_t d[10]; for (int i = 0; i < 10; i++) d[i] = dctr[i] - pctr[i];
                LOGV("【红】dark flash at frame %llu (after frame %llu; brightness %.1f, around it %.1f; back at frame %llu): before it %llu draws, %llu passes, %llu skipped, %llu images made, %llu shared reuses, %llu uploads, %llu re-uploads, %llu swept, %llu CPU-pool images shared, %llu serials switched",
                     (unsigned long long)dframe, (unsigned long long)dprev, dark_m, dark_avg, (unsigned long long)b->frame, (unsigned long long)d[0], (unsigned long long)d[1], (unsigned long long)d[2],
                     (unsigned long long)d[3], (unsigned long long)d[4], (unsigned long long)d[5], (unsigned long long)d[6], (unsigned long long)d[7], (unsigned long long)d[8], (unsigned long long)d[9]);
            }
            if (logged <= 12) probe_report(dframe, b->frame);   /* against the frame after it: the same view after a camera cut */
            if (saved < 5 && keep) { saved++; mkdir("shots", 0755); char path[256]; snprintf(path, sizeof path, "shots/flash_%06llu.png", (unsigned long long)dframe); write_png(path, keep, kw, kh, (size_t)kw * 4); }
            pending = 0;
        } else if (b->frame - dframe > 5) { pending = 0; avg = m; }   /* stayed dark: a fade (the new brightness is the normal one now) */
    } else if (avg > 6 && m > avg * 1.6 && m - avg > 35) {
        /* A bright flash (HarmonyOS, Ocarina of Time: the user's "flashes white"): a frame far brighter than the last few.
           Logged at once with the pass comparison against the frame before it (TOTK_VK_PASSPROBE=1 keeps the probes); the
           average is left alone, so a cut to a bright scene logs a few lines and then becomes the normal brightness. */
        static int nbright; static uint64_t last;
        if (nbright < 40 && b->frame != last + 1) {
            nbright++;
            LOGV("【红】bright flash at frame %llu (after frame %llu; brightness %.1f, before it %.1f)", (unsigned long long)b->frame, (unsigned long long)pframe, m, avg);
            if (nbright <= 12) probe_report(b->frame, pframe);
            if (nbright <= 5) { mkdir("shots", 0755); char path[256]; snprintf(path, sizeof path, "shots/bright_%06llu.png", (unsigned long long)b->frame); write_png(path, b->ptr, b->w, b->h, (size_t)b->w * 4); }
        }
        last = b->frame;
        avg = avg * 0.9 + m * 0.1;
        return;   /* pctr / pframe stay the frame before it */
    } else if (avg > 6 && m < avg * 0.45) {
        pending = 1; dark_m = m; dark_avg = avg; dframe = b->frame; dprev = pframe; memcpy(dctr, b->ctr, sizeof dctr);
        if (saved < 5) { size_t n = (size_t)b->w * b->h * 4; if (!keep || kw * kh * 4 < n) keep = realloc(keep, n); memcpy(keep, b->ptr, n); kw = b->w; kh = b->h; }
        return;   /* pctr stays the frame before it */
    }
    if (!pending && m >= avg * 0.45) avg = avg < 0 ? m : avg * 0.75 + m * 0.25;
    if (!pending) { memcpy(pctr, b->ctr, sizeof pctr); pframe = b->frame; }
}
static void *presenter(void *arg) {
    (void)arg;
#ifdef TOTK_OHOS
    /* HarmonyOS: the Android branch below is compiled out, so this thread kept the name it inherits from the walker
       that creates it ("totk-walk"): native-harmonyos/ohos/ohos_sched.c then neither treated it as the walker (registered
       by tid) nor as a worker (skipped by that name), and the ~5 ms/frame window copy ran wherever it landed (seen in a CPU profile). */
    totk_thread_start("totk-present", NULL);
#endif
#ifdef __ANDROID__
    totk_thread_start("totk-present", NULL);   /* name, performance hint and frame-thread settings */
    ASurfaceControl *surface = NULL; ASurfaceTransaction *tx = NULL; uint64_t surface_generation = 0; int current = -1;
    int backpressure_policy=VK_PRESENT_BP_SERIAL;
    static pres_complete completion = {.mu = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER};
    static pres_complete second = {.mu = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER};
    pres_complete *contexts[VK_PRESENT_FLIGHT_CAPACITY]={&completion,&second};
    completion.observer=second.observer=&completion;
    g_pres_retire.contexts[0]=&completion; g_pres_retire.contexts[1]=&second;
    if (g_pres_async_retire) {
        pthread_condattr_t attr;
        int error=pthread_condattr_init(&attr);
        if (error) DIE("retirement condattr: %d",error);
        error=pthread_condattr_setclock(&attr,CLOCK_MONOTONIC);
        for (unsigned i=0;!error && i<VK_PRESENT_RETIRE_CAPACITY;i++) {
            pthread_cond_destroy(&contexts[i]->cv);
            error=pthread_cond_init(&contexts[i]->cv,&attr);
        }
        if (!error) error=pthread_cond_init(&g_pres_retire_notify_cv,&attr);
        pthread_condattr_destroy(&attr);
        if (error) DIE("retirement monotonic condition: %d",error);
        atomic_store_explicit(&g_pres_retire_notify_ready,1,memory_order_release);
    }
    vk_present_flight flight={0};
    uint64_t retire_logged_generation=UINT64_MAX;
    int retire_logged_active=-1,retire_logged_qualified=-1;
    int retire_qualified=0; /* existing Thor/AFME admission, not transient scene readiness */
    unsigned hinted_rate = 0;
#endif
    for (;;) {
#ifdef __ANDROID__
        totk_live_slack_tick();   /* a live TOTK_TIMER_SLACK_NS: the presenter applies it itself, before its deadline waits */
        if (!g_pres_retire.running) {
            /* OFF keeps its original finish-before-pickup policy. Attribute
             * genuine full-context waiting to the still-queued immutable job. */
            pres_buf *capacity=NULL;
            if (g_fg_job_trace && flight.count==VK_PRESENT_FLIGHT_CAPACITY) {
                pthread_mutex_lock(&g_win_mu);
                if (g_pres_qh!=g_pres_qt) capacity=&g_pres[g_pres_q[g_pres_qh%(NPRES*2)]];
                if (capacity) {
                    fgs_transaction_id live[FGS_PUBLICATION_CONTEXTS];
                    for (unsigned n=0;n<flight.count;n++) {
                        unsigned index=(flight.head+n)%VK_PRESENT_FLIGHT_CAPACITY;
                        vk_present_retire_token token=contexts[index]->retire_token;
                        live[n]=(fgs_transaction_id){token.index,token.sequence,token.generation};
                    }
                    pres_capacity_begin_live(capacity,live,flight.count);
                }
                pthread_mutex_unlock(&g_win_mu);
            }
            pres_transaction_drain(contexts,&flight,0);
            if (capacity) pres_capacity_end(capacity);
        }
#endif
        pthread_mutex_lock(&g_win_mu);
        for (;;) {
#ifdef __ANDROID__
            int notifying=g_pres_retire.running;
            if (notifying) pthread_mutex_lock(&g_pres_retire_notify_mu);
#endif
            int update = g_pres_qh != g_pres_qt;
#ifdef __ANDROID__
            update |= surface && (surface_generation != g_win_generation || atomic_load(&g_ahb_off));
#endif
            if (update) {
#ifdef __ANDROID__
                if (notifying) pthread_mutex_unlock(&g_pres_retire_notify_mu);
#endif
                break;
            }
#ifdef __ANDROID__
            if (notifying) {
                pthread_mutex_unlock(&g_win_mu);
                pthread_cond_wait(&g_pres_retire_notify_cv,&g_pres_retire_notify_mu);
                pthread_mutex_unlock(&g_pres_retire_notify_mu);
                pthread_mutex_lock(&g_win_mu);
            } else
#endif
                pthread_cond_wait(&g_pres_cv, &g_win_mu);
        }
#ifdef __ANDROID__
        if (surface && (surface_generation != g_win_generation || atomic_load(&g_ahb_off))) {
            pthread_mutex_unlock(&g_win_mu);
            pres_transaction_drain(contexts,&flight,1);
            pres_surface_remove(tx, &completion, &surface, &current);
            if (g_pres_retire.transaction==tx) tx=NULL; /* retained quarantined transaction */
            continue;
        }
#endif
        int i = g_pres_q[g_pres_qh++ % (NPRES * 2)];
        if (g_pres[i].adaptive_epoch && g_pres[i].generated) g_adaptive_presenting=i;
        if (g_pres_trace || g_pres_timeline || g_fg_job_trace) {
            g_pres[i].dequeue_ns=host_now_ns();
            g_pres[i].queue_depth=(unsigned)(g_pres_qt-g_pres_qh);
        }
        pthread_mutex_unlock(&g_win_mu);
#ifdef __ANDROID__
        pres_display_sweep(&completion, 0); /* prior CURRENT fences, never a blocking display wait */
#endif
        pres_buf *b = &g_pres[i];
        /* A timeout is not completion: neither CPU nor compositor may read pixels the GPU is still writing. */
        while (!mtl_wait(b->fence, 2000000000ull)) RED(2, "window present: waiting over two seconds for the GPU");
        /* N was retained until N+1 was recorded. In admitted timeline mode N's
         * own immutable pixels may meet their earlier API deadline while N+1
         * is still rendering; children retain all own/source completion gates. */
        if (runtime_present_pair_wait_required(g_pres_timeline ? b->timeline_role:0,
                b->generated!=0,b->source_fence!=0))
            while (!mtl_wait(b->source_fence, 2000000000ull)) RED(2, "window present: waiting over two seconds for interpolation pair");
#ifdef __ANDROID__
        if (!b->adaptive_epoch && g_pres_retire.running) {
            if (!pres_retire_stop(vk_present_retire_deadline())) {
                pres_child_expire(b,FRAMEGEN_CHILD_WINDOW); continue;
            }
            for (unsigned k=0;k<VK_PRESENT_RETIRE_CAPACITY;k++) contexts[k]->retire_async=0;
        }
#endif
        b->ready_ns = b->prediction.id || b->auto_frame.id || totk_ft_on() ? vk_present_ready_ns(b->fence) : 0;
        if (g_pres_trace || g_pres_timeline || g_fg_job_trace) {
            b->own_ready_ns=vk_present_ready_ns(b->fence);
            b->pair_ready_ns=b->source_fence ? vk_present_ready_ns(b->source_fence) : b->own_ready_ns;
        }
        if (!b->generated && b->timeline_frame && totk_ft_on())
            totk_ft_present_ready(b->timeline_frame, b->ready_ns);
        fg_adaptive_present_result adaptive_decision=FG_ADAPTIVE_PRESENT_APPLY;
        if (b->adaptive_epoch && !b->discard) {
            pthread_mutex_lock(&g_win_mu);
            adaptive_decision=pres_adaptive_wait_locked(b);
            if (adaptive_decision==FG_ADAPTIVE_PRESENT_YIELD) {
                if (g_adaptive_presenting==i) g_adaptive_presenting=-1;
                pres_queue_locked(i); /* ownership/fences remain intact; earlier real is next */
            }
            pthread_mutex_unlock(&g_win_mu);
            if (adaptive_decision==FG_ADAPTIVE_PRESENT_YIELD) continue;
        }
        vk_post_complete((unsigned)i);   /* timing readback piggybacks on the existing GPU fence */
        if (b->discard) { pres_child_expire(b,FRAMEGEN_CHILD_OUTPUT); continue; }
        if (adaptive_decision!=FG_ADAPTIVE_PRESENT_APPLY) {
            pres_adaptive_expire(b,adaptive_decision); continue;
        }
        if (b->generated) {
            vk_present_job_completion generated_completion=vk_present_job_complete(&b->job,(unsigned)i,vk_fg_complete);
            int output_ok=generated_completion.accepted;
            if (generated_completion.first && output_ok) g_fg_accepted++;
            uint64_t source_ready = vk_present_ready_ns(b->source_fence);
            framegen_child_reason reason = !output_ok ? FRAMEGEN_CHILD_COMPLETION :
                b->generated == 2 ? FRAMEGEN_CHILD_OUTPUT :
                b->adaptive_epoch ? fg_adaptive_child_status(source_ready,
                    vk_present_ready_ns(b->fence),b->desired_ns,host_now_ns()) :
                b->interpolated ? framegen_interp_child_status(source_ready, b->desired_ns, host_now_ns()) :
                framegen_child_ready(source_ready, b->source_slot, b->desired_ns, host_now_ns()) ?
                    FRAMEGEN_CHILD_READY : !source_ready ? FRAMEGEN_CHILD_SOURCE_MISSING :
                    source_ready > b->source_slot ? FRAMEGEN_CHILD_SOURCE_LATE : FRAMEGEN_CHILD_DEADLINE;
            if (reason != FRAMEGEN_CHILD_READY) { pres_child_expire(b, reason); continue; }
        }
        pthread_mutex_lock(&g_win_mu);
        ANativeWindow *w = b->generation == g_win_generation ? g_win : NULL; if (w) ANativeWindow_acquire(w);
        vk_present_extent guest_window=g_pres_guest_output ?
            (g_guest_extent_cached ? pres_guest_window() : pres_frame_window_extent(w,GUEST_QUERY_PRESENTER)) :
            (vk_present_extent){0};
        int stale_copy=g_pres_guest_output && !b->zero && w &&
            (guest_window.w!=b->w || guest_window.h!=b->h);
#ifdef __ANDROID__
        uint32_t pw = g_win_pw, ph = g_win_ph;
        if (g_pres_guest_output) { pw=guest_window.w; ph=guest_window.h; }
        ANativeWindow *parent = w ? g_win_parent : NULL; if (parent) ANativeWindow_acquire(parent);
        int display_id = g_win_display_id;
#endif
        int resize = !stale_copy && !b->zero && w && (g_win_w != b->w || g_win_h != b->h);
        if (resize) { g_win_w = b->w; g_win_h = b->h; }
        pthread_mutex_unlock(&g_win_mu);
#ifdef __ANDROID__
        if (b->generated) totk_perf_generated();   /* TOTK_RATE_HINT_HOLD: frame generation is running */
        unsigned requested_rate = totk_perf_rate();
        if (w && requested_rate != hinted_rate) {
            totk_perf_window(w);
            if (surface && tx) totk_perf_surface(tx, surface);
            hinted_rate = requested_rate;
        }
#endif
        /* A resize can race a queued copy. Drop that old-sized backing before
         * publishing geometry; the next producer uses the live window size. */
        if (stale_copy) {
#ifdef __ANDROID__
            if (parent) ANativeWindow_release(parent);
#endif
            ANativeWindow_release(w); pres_child_expire(b,FRAMEGEN_CHILD_WINDOW); continue;
        }
#ifdef __ANDROID__
        if (b->zero) {
            if (w && !atomic_load(&g_ahb_off) && !atomic_load(&g_ahb_surface_off)) {
                if (!tx) {
                    errno = 0; tx = ASurfaceTransaction_create(); int error = errno;
                    LOGV("window SurfaceControl: ASurfaceTransaction_create result=%p errno=%d (errno not API status)", (void *)tx, error);
                }
                int created = !surface;
                if (tx && !surface) {
                    surface = pres_surface_create(w, parent, display_id, b->generation);
                    surface_generation = b->generation;
                    if (surface) {
                        backpressure_policy=pres_backpressure(tx,surface,b->generation);
                        totk_perf_surface(tx, surface);
                        hinted_rate = totk_perf_rate();
                    }
                }
                if (tx && surface) {
                    if (g_pres_guest_output && (!pw || !ph)) {
                        pres_child_expire(b,FRAMEGEN_CHILD_WINDOW);
                        if (parent) ANativeWindow_release(parent);
                        ANativeWindow_release(w); continue;
                    }
                    /* One-time bridge/surface creation is not steady presenter
                     * allowance. Its wall delay remains visible in diagnostics. */
                    uint64_t now=host_now_ns();
                    if (b->adaptive_epoch && vk_fg_qcom_active()) retire_qualified=1;
                    int qualified=retire_qualified;
                    int common_request=vk_present_adaptive_backpressure_requested(b->adaptive_epoch!=0,
                        getenv("TOTK_FG_ADAPTIVE_BACKPRESSURE")) && backpressure_policy==VK_PRESENT_BP_REQUEST;
                    int use_async=vk_present_retire_admitted(g_pres_async_retire,b->adaptive_epoch!=0,
                        qualified,common_request);
                    if (use_async && !g_pres_retire.window) {
                        g_pres_retire.window=w; ANativeWindow_acquire(w);
                        g_pres_retire.parent=parent; if (parent) ANativeWindow_acquire(parent);
                    }
                    if (use_async && !g_pres_retire.running) {
                        pres_transaction_drain(contexts,&flight,1);
                        if (!pres_retire_start()) { atomic_store(&g_ahb_off,1); use_async=0; }
                        if (!use_async) {
                            LOGV("[framegen-async-retire] generation=%llu requested=on active=0 capacity=2 qualified=%d backpressure=%s budget-ns=2000000000 reason=worker-unavailable",
                                (unsigned long long)b->generation,qualified,
                                common_request ? "requested" : "unavailable");
                            pres_retire_quarantine_surface(tx,&surface,&current); tx=NULL;
                            pres_child_expire(b,FRAMEGEN_CHILD_WINDOW);
                            if (parent) ANativeWindow_release(parent);
                            ANativeWindow_release(w); continue;
                        }
                    }
                    if (b->adaptive_epoch && (retire_logged_generation!=b->generation ||
                            retire_logged_active!=use_async || retire_logged_qualified!=qualified)) {
                        LOGV("[framegen-async-retire] generation=%llu requested=%s active=%d capacity=2 qualified=%d backpressure=%s budget-ns=2000000000",
                            (unsigned long long)b->generation,g_pres_async_retire ? "on" : "off",
                            use_async,qualified,common_request ? "requested" : "unavailable");
                        retire_logged_generation=b->generation;
                        retire_logged_active=use_async; retire_logged_qualified=qualified;
                    }
                    b->job.retire_async=use_async;
                    int defer=use_async || vk_present_flight_defer(backpressure_policy,b->refresh_ns,b->desired_ns,now);
                    if (!defer) pres_transaction_drain(contexts,&flight,1);
                    uint64_t child_deadline=b->timeline_role ? b->display_timeline.deadline_ns:b->desired_ns;
                    if (b->interpolated && host_now_ns() >= child_deadline) {
                        if (g_pres_timeline && b->timeline_role) {
                            static uint64_t missed;
                            atomic_fetch_add_explicit(&g_timeline_deadline_late[1],1,memory_order_relaxed);
                            if (++missed<=8 || missed%300==0)
                                LOGV("[present-timeline-miss] role=generated frame=%llu id=%lld expected=%llu deadline=%llu record=%llu enqueue=%llu dequeue=%llu own-ready=%llu pair-ready=%llu now=%llu total=%llu",
                                    (unsigned long long)b->frame,(long long)b->display_timeline.vsync_id,
                                    (unsigned long long)b->desired_ns,(unsigned long long)child_deadline,
                                    (unsigned long long)b->record_done_ns,(unsigned long long)b->enqueue_ns,
                                    (unsigned long long)b->dequeue_ns,(unsigned long long)b->own_ready_ns,
                                    (unsigned long long)b->pair_ready_ns,(unsigned long long)host_now_ns(),
                                    (unsigned long long)missed);
                        }
                        pres_child_expire(b, FRAMEGEN_CHILD_DEADLINE);
                        if (parent) ANativeWindow_release(parent);
                        ANativeWindow_release(w); continue;
                    }
                    vk_present_retire_token token={0};
                    if (use_async) {
                        fg_adaptive_present_result decision=pres_retire_reserve(b,&token);
                        if (decision!=FG_ADAPTIVE_PRESENT_APPLY) {
                            if (decision!=FG_ADAPTIVE_PRESENT_YIELD) pres_adaptive_expire(b,decision);
                            if (parent) ANativeWindow_release(parent);
                            ANativeWindow_release(w); continue;
                        }
                    }
                    /* Cancel before staging any buffer in the reusable transaction;
                     * otherwise teardown could apply a canceled child's pixels. */
                    uint64_t setup_start = b->prediction.id || b->auto_frame.id ? host_now_ns() : 0;
                    if (vk_native_ui_enabled() && (!pw || !ph)) { pw = b->w; ph = b->h; }
                    /* Crop in the actual native AHB coordinates, then undo
                     * its rendering rotation into the parent's logical space.
                     * Display transform * inverse(hint) can then be identity. */
                    ARect src = {0, 0, (int32_t)g_ahb[i].w, (int32_t)g_ahb[i].h};
                    ARect dst = {0, 0, (int32_t)pw, (int32_t)ph};
                    if (g_pres_guest_output) {
                        vk_present_rect fit=g_guest_extent_cached ? b->guest_fit : vk_present_guest_fit(b->w,b->h,pw,ph);
                        dst=(ARect){fit.x,fit.y,fit.x+(int32_t)fit.w,fit.y+(int32_t)fit.h};
                    }
                    int use_timeline=g_pres_timeline && b->timeline_role && b->display_timeline.vsync_id>=0 &&
                        runtime_present_timeline_display_allowed(runtime_android_present_timeline_display_id(),display_id);
                    if (b->adaptive_epoch) {
                        pthread_mutex_lock(&g_win_mu);
                        fg_adaptive_present_result decision=pres_adaptive_decide_locked(b);
                        uint64_t desired;
                        if (b->adaptive_slots && b->generated && !pres_slots_desired(b,&desired))
                            decision=FG_ADAPTIVE_PRESENT_SUPPRESSED;
                        if (decision!=FG_ADAPTIVE_PRESENT_APPLY) {
                            pthread_mutex_unlock(&g_win_mu);
                            if (use_async && !vk_present_retire_cancel(&g_pres_retire.queue,token))
                                DIE("retirement pre-stage cancellation");
                            pres_adaptive_expire(b,decision);
                            if (parent) ANativeWindow_release(parent);
                            ANativeWindow_release(w); continue;
                        }
                        /* Normal queue/drain cancellation costs no new
                         * transaction: nothing has been staged yet. */
                    }
                    ASurfaceTransaction_setGeometry(tx, surface, &src, &dst,
                        vk_present_turns_transform((4-b->rotation)&3));
                    ASurfaceTransaction_setBufferTransparency(tx, surface, ASURFACE_TRANSACTION_TRANSPARENCY_OPAQUE);
                    ASurfaceTransaction_setBufferDataSpace(tx, surface, ADATASPACE_SRGB);
                    if (use_timeline && !b->adaptive_slots)
                        g_pres_set_timeline(tx,b->display_timeline.vsync_id);
                    ASurfaceTransaction_setBuffer(tx, surface, g_ahb[i].buffer, -1);   /* GPU fence was waited above */
                    uint64_t setup_ns = setup_start ? host_now_ns() - setup_start : 0;
                    if (created) LOGV("window SurfaceControl: transaction configured surface=%p logical=%ux%u buffer=%ux%u->%ux%u rotation=%u inverse=%d desired_ns=%llu; setters/apply return void; awaiting first completion",
                         (void *)surface, b->w, b->h, g_ahb[i].w, g_ahb[i].h, pw, ph, b->rotation,
                         vk_present_turns_transform((4-b->rotation)&3), (unsigned long long)b->desired_ns);
                    if (!b->adaptive_epoch) {
                        vk_overlay_presented(host_now_ns(), b->generated); /* legacy handoff timing */
                        if (b->generated) pres_child_handoff();
                    }
                    int context_index=use_async ? (int)token.index : defer ? vk_present_flight_next(&flight) : 0;
                    if (context_index < 0) DIE("present transaction capacity");
                    contexts[context_index]->retire_async=use_async;
                    if (use_async) contexts[context_index]->retire_token=token;
                    unsigned pending=use_async ? vk_present_retire_count(&g_pres_retire.queue) : defer ? flight.count+1 : 1;
                    fg_adaptive_present_result applied=pres_transaction(tx, contexts[context_index], surface, current, b, setup_ns,
                        defer,pending,b->adaptive_epoch!=0);
                    if (applied!=FG_ADAPTIVE_PRESENT_APPLY) {
                        /* Drop all staged buffer/geometry/callback state. A later
                         * teardown must never apply this canceled candidate. */
                        ASurfaceTransaction_delete(tx); tx=ASurfaceTransaction_create();
                        if (!tx) DIE("adaptive presenter cannot replace canceled transaction");
                        if (use_async && !vk_present_retire_cancel(&g_pres_retire.queue,token))
                            DIE("retirement staged cancellation");
                        pres_adaptive_expire(b,applied);
                        if (parent) ANativeWindow_release(parent);
                        ANativeWindow_release(w); continue;
                    }
                    if (use_async) {
                        if (!vk_present_retire_publish(&g_pres_retire.queue,token)) DIE("retirement publication order");
                    } else if (defer && !vk_present_flight_submit(&flight,(unsigned)context_index))
                        DIE("present transaction submission order");
                    current = i;
                    if (created) LOGV("window SurfaceControl: first transaction applied surface=%p api=%d display=%d generation=%llu",
                         (void *)surface, android_get_device_api_level(), display_id, (unsigned long long)b->generation);
                    g_pres_shown++;
                    if (b->adaptive_epoch) atomic_fetch_add_explicit(&g_adaptive_stats.shown,1,memory_order_relaxed);
                    if (!b->generated) front_frame_presented();
                    if (parent) ANativeWindow_release(parent);
                    ANativeWindow_release(w); continue;   /* still busy: this is the compositor's CURRENT buffer */
                }
                LOGV("window present: SurfaceControl setup failed (transaction=%p surface=%p); using copy for window generation %llu",
                     (void *)tx, (void *)surface, (unsigned long long)b->generation);
                pthread_mutex_lock(&g_win_mu);
                if (b->generation == g_win_generation) atomic_store(&g_ahb_surface_off, 1);
                pthread_mutex_unlock(&g_win_mu);
            }
            if (parent) ANativeWindow_release(parent);
            if (w) ANativeWindow_release(w);
            pres_child_expire(b, FRAMEGEN_CHILD_WINDOW); continue;
        }
        if (parent) ANativeWindow_release(parent);
#endif
        if (b->adaptive_epoch) {
            /* Public copy has no cancel-without-post operation or serialized
             * handoff. Adaptive requires the strict native-UI/AHB route. */
            if (w) ANativeWindow_release(w);
            pres_child_expire(b,FRAMEGEN_CHILD_WINDOW); continue;
        }
        uint64_t preparation_start = b->prediction.id || b->auto_frame.id ? host_now_ns() : 0;
        uint64_t preparation_ns = 0;
        int posted = 0;
        if (!b->coherent) { VkMappedMemoryRange r = {VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, .memory = b->mem, .size = VK_WHOLE_SIZE}; vkInvalidateMappedMemoryRanges(V.dev, 1, &r); }
        if (w) {
            /* Public ANativeWindow copy has no buffer timestamp API. Do not
             * post before the nominal slot; scanout may consequently be one
             * refresh later. No graphics/recorder lock is held while waiting. */
            if (b->refresh_ns && !b->prediction.id && !b->interpolated) {
                /* Auto measures real preparation without moving the legacy
                 * wait-before-lock order or counting its deliberate slot hold. */
                if (preparation_start) preparation_ns = host_now_ns() - preparation_start;
                runtime_android_present_wait(b->copy_open_ns);
                if (preparation_start) preparation_start = host_now_ns();
            }
            if (resize) ANativeWindow_setBuffersGeometry(w, (int32_t)b->w, (int32_t)b->h, WINDOW_FORMAT_RGBX_8888);
            /* Interpolated copy prepares before its physical tick, like the
               prediction path. Refuse expired work before locking: the public
               window API has no unlock-without-post operation. FIFO retains
               temporal order even if lock/copy itself stalls. */
            if (b->interpolated && host_now_ns() >= b->desired_ns) {
                ANativeWindow_release(w); pres_child_expire(b, FRAMEGEN_CHILD_DEADLINE); continue;
            }
            ANativeWindow_Buffer out;
            int blob_skip = 0;
#ifdef TOTK_OHOS
            blob_skip = !b->generated && blob_drop(b);   /* an isolated white frame: not posted (counted as cancelled) */
#endif
            if (!blob_skip && !ANativeWindow_lock(w, &out, NULL)) {
#ifdef TOTK_OHOS
                if (!b->generated) blob_pre_copy(b);   /* TOTK_OHOS_BLOB: the bytes this copy reads */
#endif
                uint32_t rw = MINV(b->w, (uint32_t)out.width), rh = MINV(b->h, (uint32_t)out.height);
                for (uint32_t y = 0; y < rh; y++) {
                    const uint8_t *src = b->ptr + (size_t)y * b->w * 4; uint8_t *dst = (uint8_t *)out.bits + (size_t)y * (size_t)out.stride * 4;
                    if (!b->swap) memcpy(dst, src, (size_t)rw * 4);
                    else for (uint32_t x = 0; x < rw; x++) { dst[4 * x] = src[4 * x + 2]; dst[4 * x + 1] = src[4 * x + 1]; dst[4 * x + 2] = src[4 * x]; dst[4 * x + 3] = 255; }
                }
                uint64_t ready_ns = preparation_start ? host_now_ns() : 0;
                if (preparation_start) preparation_ns += ready_ns - preparation_start;
                /* Predict prepares pixels before its intentional no-earlier-than
                 * post floor. Holding this locked buffer preserves ANativeWindow
                 * lifetime; legacy modes retain wait-before-lock ordering. */
                if ((b->prediction.id || b->interpolated) && b->refresh_ns) runtime_android_present_wait(b->copy_open_ns);
                uint64_t ft_post = totk_ft_on() && b->timeline_frame ? totk_ft_now() : 0;
                uint64_t post_start = b->prediction.id || b->auto_frame.id ? host_now_ns() : 0;
                if (!ANativeWindow_unlockAndPost(w)) {
                    if (post_start) pres_predict_feedback(b, PRESENT_PREDICT_COPY, ready_ns,
                        preparation_ns, host_now_ns() - post_start);
                    /* Entry to a successful post, separate from the historical ACK endpoint below. */
                    if (ft_post) totk_ft_submission(b->timeline_frame, ft_post, FT_SUBMIT_COPY_POST);
                    pres_credit_release(b);   /* successful post acknowledgement, not scanout */
                    /* Successful post acknowledgement only; this API exposes no actual display timestamp. */
                    if (totk_ft_on() && b->timeline_frame) totk_ft_compositor(b->timeline_frame, totk_ft_now(), 0);
                    vk_overlay_presented(host_now_ns(), b->generated); g_pres_shown++;
                    if (b->adaptive_epoch) atomic_fetch_add_explicit(&g_adaptive_stats.shown,1,memory_order_relaxed);
                    posted = 1;
                    if (b->generated) pres_child_handoff(); else front_frame_presented();
                }
                else if (!b->generated) g_pres_cancelled++;
            }
            else if (!b->generated) g_pres_cancelled++;
            ANativeWindow_release(w);
        }
        else if (!b->generated) g_pres_cancelled++;
        if (b->generated && !posted) {
            pres_child_expire(b, w ? FRAMEGEN_CHILD_POST : FRAMEGEN_CHILD_WINDOW); continue;
        }
        pres_credit_release(b);   /* window/lock/post failures; success was already retired above */
        pres_predict_cancel(b); /* failed window/lock/post, or no usable readiness */
        if (!b->generated) black_check(b);
#ifdef TOTK_OHOS
        if (!b->generated) flicker_check(b);
        if (!b->generated) blob_check(b);
#endif
        atomic_store(&b->busy, 0);
    }
    return NULL;
}
static int pres_alloc(pres_buf *b, VkDeviceSize size) {
    if (b->size >= size) return 1;
    if (b->buf) { vkDestroyBuffer(V.dev, b->buf, NULL); vk_memory_release(b->mem, VK_MEMORY_READBACK); b->buf = VK_NULL_HANDLE; b->size = 0; }
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    if (vkCreateBuffer(V.dev, &bci, NULL, &b->buf)) return 0;
    VkMemoryRequirements req; vkGetBufferMemoryRequirements(V.dev, b->buf, &req);
    /* the CPU reads every byte: cached memory first */
    int t = vk_pick_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (t < 0) t = vk_pick_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
    if (t < 0) t = vk_pick_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size, .memoryTypeIndex = (uint32_t)t};
    if (t < 0 || vk_memory_allocate(&mai, &b->mem, VK_MEMORY_READBACK)) { vkDestroyBuffer(V.dev, b->buf, NULL); b->buf = VK_NULL_HANDLE; return 0; }
    vkBindBufferMemory(V.dev, b->buf, b->mem, 0);
    if (vkMapMemory(V.dev, b->mem, 0, VK_WHOLE_SIZE, 0, (void **)&b->ptr)) return 0;
    b->coherent = !!(V.mp.memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    b->size = size;
    return 1;
}
static VkImage g_pimg; static vk_alloc g_pmem; static uint32_t g_pimg_w, g_pimg_h; static VkFormat g_pimg_f;
static int pres_image(uint32_t w, uint32_t h, VkFormat f) {   /* the window-sized image the frame is scaled into */
    if (g_pimg && g_pimg_w == w && g_pimg_h == h && g_pimg_f == f) return 1;
    if (g_pimg) { vk_flush_wait(); vkDestroyImage(V.dev, g_pimg, NULL); vk_mem_free(&g_pmem); g_pimg = VK_NULL_HANDLE; }
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = f, .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
    if (vkCreateImage(V.dev, &ci, NULL, &g_pimg)) { RED(2, "presentation image %ux%u could not be made", w, h); g_pimg = VK_NULL_HANDLE; return 0; }
    VkMemoryRequirements req; vkGetImageMemoryRequirements(V.dev, g_pimg, &req);
    if (!vk_mem_alloc(&req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &g_pmem)) { vkDestroyImage(V.dev, g_pimg, NULL); g_pimg = VK_NULL_HANDLE; return 0; }
    vkBindImageMemory(V.dev, g_pimg, g_pmem.mem, g_pmem.off);
    VkImageMemoryBarrier2 ib = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_NONE, .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT, .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = g_pimg, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &ib};
    vkCmdPipelineBarrier2(vk_cb(), &di);
    g_pimg_w = w; g_pimg_h = h; g_pimg_f = f;
    fprintf(stderr, "[nvn-vk] window picture %ux%u\n", w, h);
    return 1;
}
static void pres_enqueue(int i) {
    pres_buf *b = &g_pres[i];
    vk_present_job_begin(&b->job);
    pthread_mutex_lock(&g_win_mu);
    if (b->refresh_ns) {
        atomic_store_explicit(&b->credited, 1, memory_order_relaxed);
        runtime_android_present_credit(1);
    }
    if (b->timeline_frame) {
        totk_ft_handoff(b->timeline_frame, totk_ft_now());
        if (b->refresh_ns) totk_ft_present_slot(b->timeline_frame, b->desired_ns, b->slot_skipped);
    }
    if (g_pres_trace || g_pres_slot_diag || g_pres_timeline) b->enqueue_ns=host_now_ns();
    pres_queue_locked(i);
    pthread_cond_signal(&g_pres_cv); pthread_mutex_unlock(&g_win_mu);
}
/* Snapshot immutable producer metadata and atomic observer timestamps only on
 * exhaustion, once per 300-real reporting group. No waits, readback or heap
 * allocation; a slot may release during the snapshot, explicitly reported. */
static void pres_slot_exhaustion(uint64_t pair_real) {
    if (!g_pres_slot_diag) return;
    static uint64_t last_group=UINT64_MAX;
    uint64_t group=g_present_n/300;
    if (group==last_group) return;
    last_group=group;
    uint64_t now=host_now_ns();
    LOGV("[framegen-slot-exhaustion] producer=%llu pair_real=%llu now_ns=%llu buffers=%d",
        (unsigned long long)g_present_n,(unsigned long long)pair_real,(unsigned long long)now,g_pres_active);
    for (int i=0;i<g_pres_active;i++) {
        const pres_buf *b=&g_pres[i];
        if (!atomic_load_explicit(&b->busy,memory_order_acquire)) continue;
        uint64_t apply=atomic_load_explicit(&b->diag_apply_ns,memory_order_acquire);
        uint64_t callback=atomic_load_explicit(&b->diag_callback_ns,memory_order_acquire);
        uint64_t wait=atomic_load_explicit(&b->diag_previous_wait_ns,memory_order_acquire);
        const char *owner=wait ? "previous-release-wait" : apply ?
            (callback ? "applied-callback" : "applied-await-callback") :
            b->enqueue_ns ? "queued-or-presenter" : "pair-held";
        LOGV("[framegen-slot-owner] slot=%d frame=%llu kind=%s owner=%s desired_ns=%llu enqueue_ns=%llu apply_enter_ns=%llu callback_ns=%llu previous_wait_ns=%llu own_ready_ns=%llu pair_ready_ns=%llu busy_now=%d",
            i,(unsigned long long)b->frame,b->generated ? "generated" : "real",owner,
            (unsigned long long)b->desired_ns,(unsigned long long)b->enqueue_ns,
            (unsigned long long)apply,(unsigned long long)callback,(unsigned long long)wait,
            (unsigned long long)vk_present_ready_ns(b->fence),
            (unsigned long long)(b->source_fence ? vk_present_ready_ns(b->source_fence) : 0),
            atomic_load_explicit(&b->busy,memory_order_acquire));
    }
}

/* Synthetic output is a child of an immutable real reservation, never another
 * guest present/acquire or a second advancement of the real display lattice. */
static void present_generated(const tex_t *t, vk_tex *x, int srgb, const pres_buf *real,
        uint32_t ow, uint32_t oh, const int crop[4], uint64_t midpoint,
        const runtime_present_timeline_entry *timeline, float phase, int prepare_slots) {
    int interpolating = vk_fg_interpolating();
    int diagnose = interpolating && FGX.diag && FGX.eligible;
    if (!interpolating) midpoint = framegen_midpoint(real->desired_ns, real->refresh_ns, real->interval, host_now_ns());
    if (!midpoint && !prepare_slots) { if (!interpolating) vk_fg_skip(); return; }
    int i = -1;
    for (int k = 0; k < g_pres_active; k++) if (&g_pres[k]!=real && !atomic_load(&g_pres[k].busy)) { i = k; break; }
    int output_ok = 1;
    if (i < 0) {
        g_fg_no_buffer++;
        if (diagnose) { g_fg_admission.no_buffer++; pres_slot_exhaustion(real->frame); }
        if (!interpolating) vk_fg_skip();
        return;
    }
    uint32_t fw = vk_tex_width(x,0), fh = vk_tex_height(x,0);
    uint32_t native_guest_w=0,native_guest_h=0;
    if (vk_native_ui_enabled()) {
        vk_post_output native;
        if (!vk_tex_native_output(x,&native)) {
            LOGV("[framegen-present] refusing native generated image: presented sibling unavailable");
            if (diagnose) g_fg_admission.source_shape++;
            return;
        }
        fw=native.w; fh=native.h;
        vk_tex_native_basis(x,&native_guest_w,&native_guest_h);
    }
    if (vk_tex_samples(x) != VK_SAMPLE_COUNT_1_BIT || vk_tex_is3d(x)) {
        if (diagnose) g_fg_admission.source_shape++;
        if (!interpolating) vk_fg_skip();
        return;
    }
    vk_end_rendering_at(VK_PASS_END_PRESENT,3);
    VkCommandBuffer cb;
    vk_post_output generated;
    vk_fg_generation *prepared=NULL;
    if (prepare_slots) {
        vk_fg_generation_stamp stamp={real->framegen_real,real->adaptive_epoch};
        prepared=vk_fg_generation_prepare((unsigned)i,fw,fh,srgb,&stamp);
        if (!prepared || !vk_fg_generation_output(prepared,&generated)) {
            if (prepared) vk_fg_generation_cancel(prepared);
            return;
        }
        cb=vk_fg_generation_prepare_cb(prepared);
    } else {
        cb=vk_cb(); vk_barrier_all(cb);
        if (!vk_fg_generate_phase(cb,(unsigned)i,fw,fh,srgb,phase,&generated)) {
            if (diagnose) g_fg_admission.generator_refused++;
            return;
        }
        cb=vk_cb(); /* Legacy external handoff sealed the entry primary. */
        if (diagnose) g_fg_admission.recorded++;
        g_fg_generated++; g_fg_pending++;
    }
    uint32_t w = native_guest_w ? native_guest_w : fw, h = native_guest_h ? native_guest_h : fh;
    int32_t cx = 0, cy = 0;
    if (crop && crop[0]>=0 && crop[1]>=0 && crop[2]>0 && crop[3]>0 &&
        (uint32_t)(crop[0]+crop[2])<=w && (uint32_t)(crop[1]+crop[3])<=h) {
        cx=crop[0]; cy=crop[1]; w=(uint32_t)crop[2]; h=(uint32_t)crop[3];
    }
    if (native_guest_w && native_guest_h) {
        vk_present_rect mapped=vk_present_native_crop((vk_present_rect){cx,cy,w,h},
            native_guest_w,native_guest_h,fw,fh);
        cx=mapped.x; cy=mapped.y; w=mapped.w; h=mapped.h;
    }
    int32_t dw, dh, dx, dy;
    if (g_pres_guest_output) {
        dw=(int32_t)real->guest_fit.w; dh=(int32_t)real->guest_fit.h; dx=dy=0;
        if (ow==real->guest_window.w && oh==real->guest_window.h) {
            dx=real->guest_fit.x; dy=real->guest_fit.y;
        }
    } else {
        vk_present_rect fit=vk_present_fit_shown(w,h,pres_shown_aspect(),ow,oh);
        dw=(int32_t)fit.w; dh=(int32_t)fit.h; dx=fit.x; dy=fit.y;
    }
    vk_post_output post = generated;
    if (vk_post_enabled()) {
        if (vk_gpuprof_on && !prepare_slots) vk_gpuprof_scope_begin(cb, "post");
        if (vk_post_run(cb,(unsigned)i,vk_fg_raw_view((unsigned)i),
                fw,fh,cx,cy,w,h,(uint32_t)dw,(uint32_t)dh,srgb,&post)) {
            cx=cy=0; w=post.w; h=post.h;
        }
        if (vk_gpuprof_on && !prepare_slots) vk_gpuprof_scope_end(cb);
    }
    pres_buf *b=&g_pres[i]; b->zero=0;
    b->rotation=real->rotation;
    b->ubwc=real->ubwc;
    if (g_pres_guest_output) { b->guest_window=real->guest_window; b->guest_fit=real->guest_fit; }
    vk_fg_output_copy_begin(cb,(unsigned)i);
#ifdef __ANDROID__
    if (pres_ahb_draw(i,t,x,srgb,ow,oh,cx,cy,w,h,dx,dy,dw,dh,&post,b->rotation,b->ubwc,prepare_slots?cb:VK_NULL_HANDLE)) {
        b->zero=1; goto generated_enqueue;
    }
#endif
    if (prepare_slots) {
        /* The legacy copy path owns a shared scratch image. It cannot be
         * referenced by a delayed immutable job across later real frames. */
        vk_fg_generation_cancel(prepared);
        return;
    }
    if (g_pres_guest_output) {
        vk_present_extent extent=real->guest_window;
        if (!g_guest_extent_cached) {
            pthread_mutex_lock(&g_win_mu);
            extent=pres_frame_window_extent(g_win,GUEST_QUERY_PRODUCER);
            pthread_mutex_unlock(&g_win_mu);
        }
        if (!extent.w || !extent.h) { output_ok=0; goto generated_enqueue; }
        ow=extent.w; oh=extent.h;
        if (g_guest_extent_cached) {
            dw=(int32_t)real->guest_fit.w; dh=(int32_t)real->guest_fit.h;
            dx=real->guest_fit.x; dy=real->guest_fit.y;
        } else {
            uint32_t sw=vk_present_shown_width(w,h,pres_shown_aspect());
            double sc=fmin(1.0,fmin((double)ow/sw,(double)oh/h));
            dw=(int32_t)(sw*sc+0.5); dh=(int32_t)(h*sc+0.5);
            dx=((int32_t)ow-dw)/2; dy=((int32_t)oh-dh)/2;
        }
    }
    if (!pres_image(ow,oh,srgb?VK_FORMAT_R8G8B8A8_SRGB:VK_FORMAT_R8G8B8A8_UNORM) ||
        !pres_alloc(b,(VkDeviceSize)ow*oh*4)) {
        /* Queue discard-only retirement; never block game production to clean
         * up synthetic work that could not be handed to the window. */
        output_ok=0; goto generated_enqueue;
    }
    cb=vk_cb(); vk_barrier_all(cb);
    if (vk_gpuprof_on) vk_gpuprof_scope_begin(cb, "present");
    VkClearColorValue black={{0,0,0,1}};
    VkImageSubresourceRange rr={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    vkCmdClearColorImage(cb,g_pimg,VK_IMAGE_LAYOUT_GENERAL,&black,1,&rr); vk_barrier_all(cb);
    VkImageBlit bl={.srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},
        .srcOffsets={{cx,cy,0},{cx+(int32_t)w,cy+(int32_t)h,1}},
        .dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.dstOffsets={{dx,dy,0},{dx+dw,dy+dh,1}}};
    VkFilter filter = vk_native_ui_enabled() && w == (uint32_t)dw && h == (uint32_t)dh ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    vkCmdBlitImage(cb,post.image,VK_IMAGE_LAYOUT_GENERAL,g_pimg,VK_IMAGE_LAYOUT_GENERAL,1,&bl,filter);
    vk_barrier_all(cb); vk_overlay_record(cb,g_pimg,VK_IMAGE_LAYOUT_GENERAL,ow,oh,(unsigned)i,0);
    VkBufferImageCopy copy={.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.imageExtent={ow,oh,1}};
    vkCmdCopyImageToBuffer(cb,g_pimg,VK_IMAGE_LAYOUT_GENERAL,b->buf,1,&copy); vk_barrier_all(cb);
    if (vk_gpuprof_on) vk_gpuprof_scope_end(cb);
generated_enqueue:
    vk_present_job_begin(&b->job); /* generated jobs bypass pres_enqueue */
    b->generation=real->generation; b->desired_ns=b->copy_open_ns=midpoint;
    b->refresh_ns=real->refresh_ns; b->interval=1; b->source_fence=real->fence;
    if (g_pres_timeline) {
        b->timeline_role=timeline && timeline->expected_ns ? RUNTIME_PRESENT_TIMELINE_GENERATED:0;
        if (b->timeline_role) b->display_timeline=*timeline;
    }
    b->source_slot=real->desired_ns; b->generated=output_ok?1:2; b->prediction=(runtime_present_prediction){0};
    b->discard=0;
    b->adaptive_epoch=real->adaptive_epoch;
    b->adaptive_slots=(unsigned)prepare_slots;
    b->adaptive_slot=(fgs_slot){0}; b->adaptive_token=0;
    b->adaptive_release_ns=0; b->adaptive_latch_kind=0;
    atomic_store_explicit(&b->adaptive_suppressed,0,memory_order_relaxed);
    b->interpolated=interpolating;
    g_pres_qcom[i]=vk_fg_qcom_picture();
    g_pres_lsfg[i]=vk_fg_lsfg_picture();
    b->frame=real->frame;
    b->prediction_done=1; b->ready_ns=0; b->timeline_frame=0;
    /* A synthetic image owns no game-frame observation or real-slot skip credit. */
    b->framegen_real=0;
    b->auto_frame=(runtime_fps_auto_frame){0}; b->auto_done=1; b->auto_cadence=0; b->slot_skipped=0;
    b->w=ow; b->h=oh; b->swap=0; atomic_store(&b->credited,0);
    pres_slot_diag_reset(b);
    if (prepare_slots) {
        if (!output_ok || !b->zero || !vk_fg_generation_prepare_finish(prepared)) {
            vk_fg_generation_cancel(prepared); return;
        }
        atomic_store_explicit(&b->busy,1,memory_order_release);
        pthread_mutex_lock(&g_win_mu);
        g_slot_prepared[i]=(pres_slot_prepared){.generation=prepared,
            .phase=g_adaptive_policy,.frame=real->frame,.epoch=real->adaptive_epoch,
            .window_generation=real->generation,.real_index=(int)(real-g_pres),.output_index=i};
        g_slot_prepared_count++;
        atomic_store_explicit(&vk_fg_slot_boundary_active,1,memory_order_relaxed);
        pthread_mutex_unlock(&g_win_mu);
        return;
    }
    vk_fg_gpu_end(vk_cb(),(unsigned)i);
    vk_fg_diag_dump((unsigned)i);
    if (g_pres_trace || g_pres_slot_diag || g_pres_timeline || g_fg_job_trace) b->record_done_ns=host_now_ns();
    b->fence=vk_submit_now();
#ifdef __ANDROID__
    if (b->zero && b->fence) g_ahb[i].foreign=1;
#endif
    atomic_store(&b->busy,1);
    /* The real image was submitted before any generated commands. Classify
       this whole ticket range before the real-frame endpoint is sealed. */
    vk_fg_synthetic_range(real->fence+1,b->fence);
    g_bound_pipe=VK_NULL_HANDLE; g_dyn_dirty=1; memset(g_bound_set[0],0,sizeof g_bound_set[0]);
    pthread_mutex_lock(&g_win_mu);
    if (g_pres_trace || g_pres_slot_diag || g_pres_timeline) b->enqueue_ns=host_now_ns();
    pres_queue_locked(i); pthread_cond_signal(&g_pres_cv);
    pthread_mutex_unlock(&g_win_mu);
}
/* Called only by the recorder owner with no open guest primary. Optional
 * dispatch never waits for publication, a free recorder slot, or the GPU. */
void vk_fg_slot_boundary(void) {
    int requested=pres_slots_requested();
    if (!requested && !g_slot_prepared_count) return;
    uint64_t now=host_now_ns();
    g_slot_dispatch_stats.boundaries++;
    if (!g_slot_dispatch_stats.first_ns) g_slot_dispatch_stats.first_ns=now;
    for (unsigned k=0;k<NPRES;k++) {
        pthread_mutex_lock(&g_win_mu);
        pres_slot_prepared *p=&g_slot_prepared[k];
        if (!p->generation || p->dispatching) { pthread_mutex_unlock(&g_win_mu); continue; }
        runtime_fps_adaptive_frame current=runtime_fps_adaptive_snapshot();
        unsigned generation_state=vk_fg_generation_state(p->generation);
        unsigned backend_reason=g_fg_job_trace ? vk_fg_generation_cancel_reason(p->generation) : FG_SLOT_CANCEL_NONE;
        fg_slot_cancel_reason reason=!requested ? FG_SLOT_CANCEL_MODE :
            p->cancelled ? FG_SLOT_CANCEL_OWNER :
            (!g_win || p->window_generation!=g_win_generation) ? FG_SLOT_CANCEL_WINDOW :
            p->epoch!=current.epoch ? FG_SLOT_CANCEL_EPOCH :
            generation_state==VK_FG_GENERATION_EMPTY ?
                (backend_reason==FG_SLOT_CANCEL_SNAPSHOT_PRESSURE ?
                    FG_SLOT_CANCEL_SNAPSHOT_PRESSURE : FG_SLOT_CANCEL_EMPTY) : FG_SLOT_CANCEL_NONE;
        int cancel=reason!=FG_SLOT_CANCEL_NONE;
        if (!cancel && !p->published) {
            g_slot_dispatch_stats.no_publication++;
            pthread_mutex_unlock(&g_win_mu); continue;
        }
        if (!cancel && !p->request.token) {
            g_slot_dispatch_stats.no_request++; cancel=1; reason=FG_SLOT_CANCEL_NO_HOLE;
        }
        if (!cancel && (fgs_pending_index(&g_slot_policy,p->request.token)<0 ||
                now>=p->request.deadline_ns)) {
            g_slot_dispatch_stats.late++; cancel=1;
            reason=fgs_pending_index(&g_slot_policy,p->request.token)<0 ?
                FG_SLOT_CANCEL_POLICY : FG_SLOT_CANCEL_DEADLINE;
        }
        runtime_present_timelines clock;
        if (!cancel && (!runtime_android_adaptive_slot_timelines(&clock) ||
                !runtime_present_adaptive_slot_clock_matches(&clock,g_slot_clock_generation,
                    g_pres[p->output_index].refresh_ns,runtime_android_adaptive_slots_display_id(),g_win_display_id))) {
            g_slot_dispatch_stats.unavailable++; cancel=1; reason=FG_SLOT_CANCEL_CLOCK;
        }
        fg_adaptive_policy phase_policy=p->phase;
        phase_policy.display_anchor_ns=p->real_target;
        float phase=0;
        if (!cancel && !fg_adaptive_phase(&phase_policy,p->request.slot.target_ns,0,&phase)) {
            g_slot_dispatch_stats.phase++; cancel=1; reason=FG_SLOT_CANCEL_PHASE;
        }
        pres_slot_prepared job=*p;
        p->dispatching=1;
        if (cancel && p->request.token) {
            fgs_cancel dropped;
            (void)fgs_cancel_gen(&g_slot_policy,p->request.token,FGS_CANCEL_OWNER,&dropped);
        }
        pthread_mutex_unlock(&g_win_mu);
        int dispatched=0;
        uint64_t ticket=0;
        if (!cancel) {
            vk_post_output output;
            g_slot_dispatch_stats.opportunities++;
            dispatched=vk_fg_generation_dispatch(job.generation,phase,job.request.deadline_ns,&output);
            if (dispatched==1) ticket=vk_fg_generation_seal(job.generation);
            if (dispatched!=1 || !ticket) {
                g_slot_dispatch_stats.gpu_refused++; reason=FG_SLOT_CANCEL_DISPATCH_REFUSED;
            }
        }
        if (!ticket) vk_fg_generation_cancel(job.generation);
        pthread_mutex_lock(&g_win_mu);
        p=&g_slot_prepared[k];
        pres_buf *b=&g_pres[job.output_index];
        if (ticket) {
#ifdef __ANDROID__
            g_ahb[job.output_index].foreign=1;
#endif
            b->fence=ticket; b->source_fence=job.real_fence;
            b->source_slot=job.real_target;
            b->desired_ns=b->copy_open_ns=job.request.slot.target_ns;
            b->adaptive_slot=job.request.slot; b->adaptive_token=job.request.token;
            b->adaptive_release_ns=0; b->adaptive_latch_kind=0;
            b->record_done_ns=host_now_ns(); b->enqueue_ns=b->record_done_ns;
            b->timeline_role=0; b->timeline_frame=0;
            g_pres_qcom[job.output_index]=1; g_pres_lsfg[job.output_index]=0;
            if (p->cancelled || !g_win || job.window_generation!=g_win_generation ||
                    fgs_pending_index(&g_slot_policy,job.request.token)<0)
                atomic_store_explicit(&b->adaptive_suppressed,1,memory_order_release);
            g_fg_generated++; g_fg_pending++; g_adaptive_stats.generated++;
            unsigned bin=(unsigned)(phase*4); if (bin>3) bin=3;
            g_adaptive_stats.phase[bin]++;
            g_slot_dispatch_stats.dispatched++;
            pres_queue_locked(job.output_index); pthread_cond_signal(&g_pres_cv);
        } else {
            fgs_cancel dropped;
            (void)fgs_cancel_gen(&g_slot_policy,job.request.token,FGS_CANCEL_OWNER,&dropped);
            if (vk_fg_generation_state(job.generation)==VK_FG_GENERATION_EMPTY)
                atomic_store_explicit(&b->busy,0,memory_order_release);
            g_slot_dispatch_stats.cancelled++;
        }
        if (g_fg_job_trace) LOGV("[adaptive-slot-dispatch] event=job frame=%llu token=%llu target_ns=%llu boundary_ns=%llu phase=%.8f ticket=%llu cancel=%d submit=%d",
            (unsigned long long)job.frame,(unsigned long long)job.request.token,
            (unsigned long long)job.request.slot.target_ns,(unsigned long long)now,
            phase,(unsigned long long)ticket,cancel,dispatched);
        if (g_fg_job_trace) {
            vk_fg_snapshot_pressure_counts pressure=vk_fg_snapshot_pressure_get();
            char line[640];
            fg_slot_job_trace_format(line,sizeof line,(fg_slot_job_trace){
                .frame=job.frame,.token=job.request.token,.target=job.request.slot.target_ns,
                .boundary=now,.reason=reason,.generation_state=generation_state,
                .backend_reason=backend_reason,.pressure_observations=pressure.observations,
                .pressure_cancellations=pressure.cancellations,.pressure_appends=pressure.appends});
            LOGV("%s",line);
        }
        *p=(pres_slot_prepared){0};
        g_slot_prepared_count--;
        atomic_store_explicit(&vk_fg_slot_boundary_active,g_slot_prepared_count!=0,memory_order_relaxed);
        pthread_mutex_unlock(&g_win_mu);
        g_bound_pipe=VK_NULL_HANDLE; g_dyn_dirty=1; memset(g_bound_set[0],0,sizeof g_bound_set[0]);
    }
    uint64_t elapsed=host_now_ns()-g_slot_dispatch_stats.first_ns;
    if (elapsed>=UINT64_C(1000000000)) {
        LOGV("[adaptive-slot-dispatch] window_ns=%llu boundaries=%llu opportunities=%llu dispatched=%llu cancelled=%llu no-publication=%llu no-hole=%llu late-boundary=%llu phase-refused=%llu clock-refused=%llu gpu-refused=%llu on-time-gen-applies=%llu late-gen-applies=%llu template-refused=%llu irrevocable-real-conflicts=%llu (app handoffs, not SF slot hits)",
            (unsigned long long)elapsed,(unsigned long long)g_slot_dispatch_stats.boundaries,
            (unsigned long long)g_slot_dispatch_stats.opportunities,(unsigned long long)g_slot_dispatch_stats.dispatched,
            (unsigned long long)g_slot_dispatch_stats.cancelled,(unsigned long long)g_slot_dispatch_stats.no_publication,
            (unsigned long long)g_slot_dispatch_stats.no_request,(unsigned long long)g_slot_dispatch_stats.late,
            (unsigned long long)g_slot_dispatch_stats.phase,(unsigned long long)g_slot_dispatch_stats.unavailable,
            (unsigned long long)g_slot_dispatch_stats.gpu_refused,
            (unsigned long long)atomic_exchange_explicit(&g_slot_on_time_applies,0,memory_order_relaxed),
            (unsigned long long)atomic_exchange_explicit(&g_slot_late_applies,0,memory_order_relaxed),
            (unsigned long long)atomic_exchange_explicit(&g_slot_template_refused,0,memory_order_relaxed),
            (unsigned long long)atomic_exchange_explicit(&g_slot_irrevocable_real_conflicts,0,memory_order_relaxed));
        memset(&g_slot_dispatch_stats,0,sizeof g_slot_dispatch_stats);
    }
}
static void present_window(const tex_t *t, double period, const int crop[4]) {
    uint64_t adaptive_lease=0;
    unsigned slot_state=FG_ADAPTIVE_WARMUP;
    runtime_fps_adaptive_frame slot_stamp={0};
    runtime_fps_auto_frame auto_frame = {0};
    runtime_android_fps_auto_take((uintptr_t)t, &auto_frame);
    int auto_cadence = runtime_android_fps_auto_active(); /* never query frontend under g_mu */
    runtime_present_prediction prediction = {0};
    int predict = runtime_android_predict_active();
    unsigned expected_path = PRESENT_PREDICT_COPY;
#ifdef __ANDROID__
    if (V.ahb_present && !atomic_load(&g_ahb_off) && !atomic_load(&g_ahb_surface_off))
        expected_path = PRESENT_PREDICT_AHB;
#endif
    /* Consume by exact guest NVNtexture identity even when the frame will drop.
     * Recording may subsequently discover that this expected AHB path is copy. */
    runtime_android_predict_take((uintptr_t)t, expected_path, &prediction);
    if (predict && !prediction.id) {
        static uint64_t missing_plan; missing_plan++; g_pres_dropped++;
        if (missing_plan <= 8 || missing_plan % 300 == 0)
            LOGV("window predict: missing acquire plan for texture=%p; display copy dropped total=%llu (no independent reservation/fallback)",
                 (const void *)t, (unsigned long long)missing_plan);
        goto cancel;
    }
    g_overlay_enabled = vk_overlay_mode(); /* Per present (follows the live Minus+R toggle); draws read only this copy. */
    static int started;
    if (!started) {
        const char *e = getenv("TOTK_PRESENT_BUFFERS");
        int scene_fsr=vk_scene_scale_enabled() && vk_scene_scale_fsr();
        g_pres_active = (int)vk_present_slots_buffers(vk_fg_interpolating(),
            vk_fg_locked() ? vk_fg_lead_refreshes() : 0,
            vk_present_buffer_count(e, getenv("TOTK_ACQUIRE_SLACK")),scene_fsr);
        if (g_pres_active != VK_PRESENT_SLOT_DEFAULT_BUFFERS)
            LOGV("window present: %d presenter buffers (default %d; guest texture count unchanged)",
                 g_pres_active, VK_PRESENT_SLOT_DEFAULT_BUFFERS);
        if (vk_fg_interpolating())
            LOGV("[framegen-slots] buffers=%d real-pair=%d AHB-current/pending=%d queued-child=%d new-child=%d FSR-scene-phase=%d; sources=4; lazy bounded allocations, GPU and compositor release both required",
                g_pres_active, VK_PRESENT_INTERP_REALS, VK_PRESENT_INTERP_AHB,
                VK_PRESENT_INTERP_QUEUED_CHILD, VK_PRESENT_INTERP_NEW_CHILD,
                scene_fsr ? VK_PRESENT_INTERP_FSR_PHASE : 0);
        e=getenv("TOTK_PRESENT_CADENCE_TRACE"); g_pres_trace=e && !strcmp(e,"1");
        e=getenv("TOTK_FRAMEGEN_JOB_TRACE"); g_fg_job_trace=e && !strcmp(e,"1");
#ifdef __ANDROID__
        g_pres_async_retire=vk_present_retire_selected(getenv("TOTK_FG_ASYNC_RETIRE"));
#endif
        g_pres_floor_ns=vk_fg_locked() ? vk_present_floor_config(getenv("TOTK_PRESENT_FLOOR_NS")) : 0;
        g_pres_guard_ns=vk_fg_locked() ? vk_present_guard_config(getenv("TOTK_PRESENT_GUARD_NS")) : 0;
        e=getenv("TOTK_PRESENT_DEDUPE");
        g_pres_distinct_refreshes=vk_fg_locked() && e && !strcmp(e,"1");
        LOGV("[present-floor] ns=%u guard_ns=%u dedupe=%d; locked AHB eligibility only; API metadata unchanged",
            g_pres_floor_ns,g_pres_guard_ns,g_pres_distinct_refreshes);
        e=getenv("TOTK_APP_WIDTH");
        pthread_mutex_lock(&g_win_mu); /* one-time selection, shared with lifecycle callbacks */
        g_pres_guest_output=!vk_native_ui_enabled() && e && !strcmp(e,"guest");
        pthread_mutex_unlock(&g_win_mu);
        e=getenv("TOTK_FRAMEGEN_DIAG");
        g_pres_slot_diag=vk_fg_interpolating() && e && !strcmp(e,"1");
        g_guest_extent_diag=e && !strcmp(e,"1");
        e=getenv("TOTK_GUEST_EXTENT_CACHE"); g_guest_extent_cached=!(e && !strcmp(e,"0"));
#ifdef __ANDROID__
        if (vk_fg_locked() && runtime_android_present_timeline_active()) {
            g_pres_set_timeline=(void (*)(ASurfaceTransaction *,int64_t))
                dlsym(RTLD_DEFAULT,"ASurfaceTransaction_setFrameTimeline");
            g_pres_timeline=g_pres_set_timeline!=NULL;
            LOGV("[present-timeline] presenter=%s; locked AHB pairs only; lead=%u; existing buffer/flight bounds retained",
                g_pres_timeline?"admitted":"unavailable (legacy)",vk_fg_lead_refreshes());
        }
#endif
        if (vk_fg_locked()) LOGV("[framegen-pacing] locked%s: at most two future transactions with runtime backpressure guard, lead=%u physical refreshes; intentional image latency about%.3f ms at60Hz; %d bounded slots; defaults/other modes unchanged",
            vk_fg_qcom_active() ? " qcom extrapolation" : "",
            vk_fg_lead_refreshes(),(vk_fg_qcom_active() ? 0 : 1000.0/30.0)+vk_fg_lead_refreshes()*(1000.0/60.0),g_pres_active);
        pthread_t th; if (pthread_create(&th, NULL, presenter, NULL)) { RED(2, "window presenter thread could not start"); goto cancel; }
        pthread_detach(th); started = 1;
    }
    int have = 0; uint32_t pw = 0, ph = 0; uint64_t generation = 0;
    unsigned rotation = 0;
    int ubwc = 0;
    vk_present_rect guest_fit = {0};
    if (!g_pres_guest_output || !g_guest_extent_cached) {
        pthread_mutex_lock(&g_win_mu);
        have = g_win != NULL; pw = g_win_pw; ph = g_win_ph; generation = g_win_generation;
        int turns=vk_present_transform_turns(g_win_transform_hint);
        if (g_win_prerotate && turns>=0) rotation=(unsigned)turns;
        ubwc=g_win_ubwc && turns>0;
        if (g_pres_guest_output) {
            vk_present_extent extent=pres_frame_window_extent(g_win,GUEST_QUERY_PRODUCER); pw=extent.w; ph=extent.h;
        }
        pthread_mutex_unlock(&g_win_mu);
        if (!have || !t) goto cancel;
    } else {
        vk_present_extent extent = pres_guest_window();
        if (!t || !extent.w || !extent.h) goto cancel;
    }
    int i = -1; for (int k = 0; k < g_pres_active; k++) if (!atomic_load(&g_pres[k].busy)) { i = k; break; }
    if (i < 0) { g_pres_dropped++; goto cancel; }
    vk_tex *x = vk_tex_get(t); if (!x) goto cancel;
    VkFormat f = vk_tex_format(x);
    int srgb = f == VK_FORMAT_R8G8B8A8_SRGB || f == VK_FORMAT_B8G8R8A8_SRGB || f == VK_FORMAT_A8B8G8R8_SRGB_PACK32;
    uint32_t w = vk_tex_width(x, 0), h = vk_tex_height(x, 0); int32_t cx = 0, cy = 0;
    vk_pso_life_external_read(w, h, "presenter-read");
    if (crop && crop[2] > 0 && crop[3] > 0 && crop[0] >= 0 && crop[1] >= 0 && (uint32_t)(crop[0] + crop[2]) <= w && (uint32_t)(crop[1] + crop[3]) <= h) { cx = crop[0]; cy = crop[1]; w = (uint32_t)crop[2]; h = (uint32_t)crop[3]; }
    if (g_pres_guest_output && g_guest_extent_cached) {
        pthread_mutex_lock(&g_win_mu);
        have = g_win != NULL; generation = g_win_generation;
        int turns=vk_present_transform_turns(g_win_transform_hint);
        if (g_win_prerotate && turns>=0) rotation=(unsigned)turns;
        ubwc=g_win_ubwc && turns>0;
        vk_present_extent extent = pres_guest_window(); pw = extent.w; ph = extent.h;
        guest_fit = pres_guest_fit_cached(w, h);
        pthread_mutex_unlock(&g_win_mu);
        if (!have || !guest_fit.w || !guest_fit.h) goto cancel;
    }
    else if (g_pres_guest_output) {
        guest_fit=vk_present_guest_fit(vk_present_shown_width(w,h,pres_shown_aspect()),h,pw,ph);
        if (!guest_fit.w || !guest_fit.h) goto cancel;
    }
    /* Relaxed telemetry only: never query resources, clocks or GPU state here.
       Without a scene MRT (menus/video), the valid game crop is the best known size. */
    vk_overlay_set_resolution(g_overlay_scene_w ? g_overlay_scene_w : w,
                              g_overlay_scene_h ? g_overlay_scene_h : h, w, h);
    /* Both front-buffer paths consume the native display sibling directly. */
    static uint32_t want_w;
    if (!want_w) {
        const char *e = getenv("TOTK_APP_WIDTH");
        want_w = vk_native_ui_enabled() ? VK_NATIVE_UI_WIDTH : e && atoi(e) >= 16 ? (uint32_t)atoi(e) : 1280;
        if (vk_native_ui_enabled()) {
            if (e && *e && strcmp(e, "1920"))
                LOGV("native UI v2: ignoring TOTK_APP_WIDTH=%s (720p guest canvas; window-sized host UI)", e);
            LOGV("native UI v2: 1280x720 game canvas; native UI begins at first UI draw; window-sized front buffers");
        }
    }
    vk_present_extent output = vk_present_output_size(vk_native_ui_enabled(), want_w, vk_post_scale(), pw, ph);
    if (g_pres_guest_output) {
        if (!guest_fit.w || !guest_fit.h) goto cancel;
        /* Copy fallback paints bars into a full window-sized buffer. AHB can
         * retain only the fitted guest pixels and place that surface centrally. */
        output=(vk_present_extent){pw,ph};
#ifdef __ANDROID__
        if (V.ahb_present && !atomic_load(&g_ahb_off) && !atomic_load(&g_ahb_surface_off))
            output=(vk_present_extent){guest_fit.w,guest_fit.h};
#endif
    }
    uint32_t ow = output.w, oh = output.h;
    if (vk_fg_interpolating()) {
        static uint32_t last_w, last_h;
        if (ow != last_w || oh != last_h) {
            LOGV("[framegen-slots] presenter RGBA8 payload=%llu bytes (%d x %ux%u x 4); excludes AHB stride/alignment, source/output/post images; actual allocations in image-memory report",
                (unsigned long long)((uint64_t)g_pres_active * ow * oh * 4), g_pres_active, ow, oh);
            last_w = ow; last_h = oh;
        }
    }
    int32_t dw, dh, dx, dy;
    if (g_pres_guest_output) {
        dw=(int32_t)guest_fit.w; dh=(int32_t)guest_fit.h; dx=dy=0;
        if (ow==pw && oh==ph) { dx=guest_fit.x; dy=guest_fit.y; }
    } else {
        vk_present_rect fit = vk_present_fit_shown(w, h, pres_shown_aspect(), ow, oh);
        dw = (int32_t)fit.w; dh = (int32_t)fit.h; dx = fit.x; dy = fit.y;
    }
    pres_buf *b = &g_pres[i]; b->zero = 0;
    b->rotation=rotation;
    b->ubwc=ubwc;
    if (g_pres_guest_output) { b->guest_window=(vk_present_extent){pw,ph}; b->guest_fit=guest_fit; }
    vk_post_output post, *post_result = NULL;
    if (vk_native_ui_enabled()) {
        vk_end_rendering_at(VK_PASS_END_PRESENT, 3);
        if (vk_tex_native_output(x, &post)) {
            uint32_t gw,gh; vk_tex_native_basis(x,&gw,&gh);
            vk_present_rect mapped=vk_present_native_crop((vk_present_rect){cx,cy,w,h},
                gw,gh,post.w,post.h);
            cx=mapped.x; cy=mapped.y; w=mapped.w; h=mapped.h; post_result=&post;
        }
    }
    if (vk_post_enabled()) {
        /* Sample raw display-encoded RGB; cached texture views retain the image's native channel order. */
        int raw_format = (f == VK_FORMAT_B8G8R8A8_UNORM || f == VK_FORMAT_B8G8R8A8_SRGB) ? 0x9a : 0x25;
        view_t view = {.set_levels = 1, .nlevels = 1, .set_layers = 1, .nlayers = 1, .set_target = 1, .target = 1,
            .set_swizzle = 1, .swz = {2,3,4,5}, .set_format = 1, .format = raw_format};
        if (vk_tex_is3d(x) || vk_tex_samples(x) != VK_SAMPLE_COUNT_1_BIT ||
            (f != VK_FORMAT_R8G8B8A8_UNORM && f != VK_FORMAT_R8G8B8A8_SRGB &&
             f != VK_FORMAT_B8G8R8A8_UNORM && f != VK_FORMAT_B8G8R8A8_SRGB &&
             f != VK_FORMAT_A8B8G8R8_UNORM_PACK32 && f != VK_FORMAT_A8B8G8R8_SRGB_PACK32))
            DIE("post: unsupported final game image format %d", f);
        VkImageView source = post_result ? vk_tex_native_view(x,&view,0) : vk_tex_view(x,t,&view,0);
        if (!source) DIE("post: final game image cannot be sampled");
        if (vk_lag_uniform_private()) vk_ub_touch((uint64_t)(uintptr_t)t->mem,t->L.size); else vk_ub_flush();
        vk_end_rendering_at(VK_PASS_END_PRESENT,3);
        VkCommandBuffer post_cb = vk_cb(); int scope = vk_native_gpu_begin(post_cb,VK_NUI_POST); vk_barrier_all(post_cb);
        if (vk_post_run(post_cb,(unsigned)i,source,post_result ? post.w : vk_tex_width(x,0),post_result ? post.h : vk_tex_height(x,0),cx,cy,w,h,
                        (uint32_t)dw,(uint32_t)dh,srgb,&post)) {
            post_result = &post; cx = cy = 0; w = post.w; h = post.h;
            g_bound_pipe = VK_NULL_HANDLE; g_dyn_dirty = 1; memset(g_bound_set[0],0,sizeof g_bound_set[0]);
        }
        vk_native_gpu_end(post_cb,scope);
    }
#ifdef __ANDROID__
    if (pres_ahb_draw(i, t, x, srgb, ow, oh, cx, cy, w, h, dx, dy, dw, dh, post_result, rotation, ubwc,VK_NULL_HANDLE)) { b->zero = 1; goto enqueue; }
#endif
    if (g_pres_guest_output && (ow!=pw || oh!=ph)) {
        ow=pw; oh=ph;
        dw=(int32_t)guest_fit.w; dh=(int32_t)guest_fit.h;
        dx=guest_fit.x; dy=guest_fit.y;
    }
    VkFormat of = srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
    if (!pres_image(ow, oh, of)) goto cancel;
    if (!pres_alloc(b, (VkDeviceSize)ow * oh * 4)) { RED(2, "presentation buffer (%ux%u) could not be made", ow, oh); goto cancel; }
    if (vk_lag_uniform_private()) vk_ub_touch((uint64_t)(uintptr_t)t->mem, t->L.size); else vk_ub_flush();
    vk_end_rendering_at(VK_PASS_END_PRESENT, 2);
    VkCommandBuffer cb = vk_cb(); int scope = vk_native_gpu_begin(cb,VK_NUI_PRESENT_COPY); vk_barrier_all(cb);
    if (dw < (int32_t)ow || dh < (int32_t)oh) {
        VkClearColorValue black = {{0, 0, 0, 1}}; VkImageSubresourceRange rr = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cb, g_pimg, VK_IMAGE_LAYOUT_GENERAL, &black, 1, &rr); vk_barrier_all(cb);
    }
    VkImageBlit bl = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .srcOffsets = {{cx, cy, 0}, {cx + (int32_t)w, cy + (int32_t)h, 1}},
                      .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstOffsets = {{dx, dy, 0}, {dx + dw, dy + dh, 1}}};
    VkFilter filter = vk_native_ui_enabled() && w == (uint32_t)dw && h == (uint32_t)dh ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    vkCmdBlitImage(cb, post_result ? post.image : vk_tex_image(x), VK_IMAGE_LAYOUT_GENERAL, g_pimg, VK_IMAGE_LAYOUT_GENERAL, 1, &bl, filter);
    vk_barrier_all(cb);
    vk_overlay_record(cb, g_pimg, VK_IMAGE_LAYOUT_GENERAL, ow, oh, (unsigned)i, 0);
    VkBufferImageCopy r = {.bufferOffset = 0, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {ow, oh, 1}};
    vkCmdCopyImageToBuffer(cb, g_pimg, VK_IMAGE_LAYOUT_GENERAL, b->buf, 1, &r);
    vk_barrier_all(cb);
    vk_native_gpu_end(cb,scope);
#ifdef __ANDROID__
enqueue:
#endif
    b->generation = generation; b->desired_ns = host_now_ns();
    b->refresh_ns = b->copy_open_ns = b->slot_skipped = 0; b->interval = 0;
    b->prediction = prediction; b->prediction_done = 0; b->ready_ns = 0;
    b->auto_frame = auto_frame; b->auto_done = 0; b->auto_cadence = auto_cadence;
    b->generated = b->interpolated = 0; b->source_fence = b->source_slot = 0;
    if (g_pres_timeline) {
        b->timeline_role=0; b->timeline_display=-1;
#ifdef __ANDROID__
        pthread_mutex_lock(&g_win_mu);
        if (generation==g_win_generation) b->timeline_display=g_win_display_id;
        pthread_mutex_unlock(&g_win_mu);
#endif
    }
    g_pres_qcom[i]=0;
    g_pres_lsfg[i]=0;
    b->discard=0;
    b->adaptive_epoch=0;
    b->adaptive_slots=(unsigned)pres_slots_requested(); b->adaptive_eligible=0;
    b->adaptive_real_conflicts=0;
    b->adaptive_slot=(fgs_slot){0}; b->adaptive_token=0;
    b->adaptive_release_ns=0; b->adaptive_latch_kind=0;
    atomic_store_explicit(&b->adaptive_suppressed,0,memory_order_relaxed);
    b->framegen_real=vk_fg_real_frame_id();
    b->w = ow; b->h = oh; b->swap = 0; atomic_store(&b->busy, 1);
    { uint64_t skipped = 0; for (int k = 0; k < 16; k++) skipped += g_stat_skips[k];
      b->ctr[0] = g_stat_draws; b->ctr[1] = g_stat_passes; b->ctr[2] = skipped; vk_res_counters(b->ctr + 3); b->frame = g_present_n; }
    pres_slot_diag_reset(b);
    if (g_pres_trace || g_pres_slot_diag || g_pres_timeline) b->record_done_ns=host_now_ns();
    int lsfg_probe=vk_fg_lsfg_probe_requested();
    if (lsfg_probe) vk_fg_lsfg_submitting(vk_cb());
    if (b->adaptive_slots) {
        runtime_present_timelines clock;
        slot_stamp=runtime_fps_adaptive_snapshot();
        b->adaptive_epoch=slot_stamp.epoch;
        b->adaptive_eligible=(unsigned)slot_stamp.eligible;
        if (g_fg_job_trace) {
            static uint64_t tr_epoch; static int tr_eligible, tr_have;
            if (!tr_have || tr_epoch!=slot_stamp.epoch || tr_eligible!=slot_stamp.eligible)
                LOGV("[adaptive-scene] event=stamp reason=%s frame=%llu now_ns=%llu epoch=%llu prev_epoch=%llu eligible=%d anchor_ns=%llu sim_ns=%llu",
                    !tr_have?"first":tr_epoch!=slot_stamp.epoch?"epoch":"eligibility",
                    (unsigned long long)b->frame,(unsigned long long)host_now_ns(),
                    (unsigned long long)slot_stamp.epoch,(unsigned long long)tr_epoch,slot_stamp.eligible,
                    (unsigned long long)slot_stamp.anchor_ns,(unsigned long long)slot_stamp.sim_ns);
            tr_have=1; tr_epoch=slot_stamp.epoch; tr_eligible=slot_stamp.eligible;
        }
        int clock_ok=runtime_android_adaptive_slots_requested() &&
            runtime_android_adaptive_slot_timelines(&clock);
        pthread_mutex_lock(&g_win_mu);
        clock_ok=clock_ok && generation==g_win_generation &&
            runtime_present_timeline_display_allowed(runtime_android_adaptive_slots_display_id(),g_win_display_id);
        pthread_mutex_unlock(&g_win_mu);
        slot_state=fg_adaptive_observe(&g_adaptive_policy,
            (fg_adaptive_stamp){slot_stamp.sim_ns,slot_stamp.anchor_ns,slot_stamp.epoch,slot_stamp.eligible},
            clock_ok?clock.refresh_ns:0);
        if (clock_ok && slot_state==FG_ADAPTIVE_ACTIVE && vk_fg_qcom_active() && vk_fg_enabled()) {
            b->refresh_ns=clock.refresh_ns; b->interval=1;
            present_generated(t,x,srgb,b,ow,oh,crop,0,NULL,0,1);
        }
    } else if (vk_fg_adaptive()) adaptive_lease=vk_replay_lease_begin();
    b->fence = vk_submit_now();
#ifdef __ANDROID__
    if (b->zero && b->fence) g_ahb[i].foreign=1;
#endif
    if (lsfg_probe) vk_fg_lsfg_submitted(b->fence); /* observation only; frame-end retirement still owns reuse */
    if (prediction.id) {
        /* The acquire plan owns this reservation. Never independently reserve
         * or advance the legacy lattice, and never adjust its immutable S/D. */
        b->desired_ns = b->copy_open_ns = prediction.slot_ns;
        b->refresh_ns = prediction.refresh_ns; b->interval = prediction.interval;
    } else if (!predict) pres_slot_reserve(b, period);
    { static unsigned last_interval; static int have;
      if (!have || last_interval != b->interval)
          LOGV("[fps-policy] frame %llu presenter effective interval %u -> %u; requested %.3f ms refresh %.3f ms path=%s plan=%llu (0=legacy; immutable queued slot, no guest writer)",
               (unsigned long long)b->frame, last_interval, b->interval, period * 1e3, b->refresh_ns / 1e6,
               b->zero ? "AHB" : "copy", (unsigned long long)prediction.id);
      have = 1; last_interval = b->interval; }
    pres_auto_reservation(b);
    b->timeline_frame = totk_ft_on() ? totk_ft_current() : 0;
    /* Ordinary/camera settings take the original retained-pair path. QCOM's
       direct real release is selected only after app-context admission. */
    int qcom=vk_fg_qcom_picture(); /* immutable engine of this already-composed picture */
    if (vk_fg_adaptive()) {
        runtime_fps_adaptive_frame stamp=b->adaptive_slots?slot_stamp:runtime_fps_adaptive_snapshot();
        b->adaptive_epoch=stamp.epoch;
        /* The simulated image at t_last is displayed at this real slot; map
         * its simulation clock there, not to GPU/render completion time. */
        unsigned state=b->adaptive_slots?slot_state:fg_adaptive_observe(&g_adaptive_policy,
            (fg_adaptive_stamp){stamp.sim_ns,stamp.anchor_ns,stamp.epoch,stamp.eligible},
            b->refresh_ns);
        g_adaptive_policy.display_anchor_ns=b->desired_ns;
        unsigned planned=fg_adaptive_plan_outputs(&g_adaptive_plan,stamp.anchor_ns,b->refresh_ns,state);
        g_adaptive_stats.refused[state]++;
        g_adaptive_stats.real++;
        if (!g_adaptive_stats.first_ns) g_adaptive_stats.first_ns=stamp.anchor_ns;
        g_adaptive_stats.last_ns=stamp.anchor_ns;
        pres_enqueue(i);
        if (adaptive_lease && planned && state==FG_ADAPTIVE_ACTIVE && vk_fg_qcom_active() && vk_fg_enabled()) {
            uint64_t target=fg_adaptive_first_slot(b->desired_ns,b->refresh_ns,host_now_ns());
            for (unsigned n=0;n<planned && target;n++) {
                float phase;
                if (!fg_adaptive_phase(&g_adaptive_policy,target,0,&phase)) {
                    g_adaptive_stats.refused[FG_ADAPTIVE_HORIZON]++; break;
                }
                uint64_t before=atomic_load_explicit(&g_fg_generated,memory_order_relaxed);
                present_generated(t,x,srgb,b,ow,oh,crop,target,NULL,phase,0);
                if (atomic_load_explicit(&g_fg_generated,memory_order_relaxed)!=before) {
                    unsigned bin=(unsigned)(phase*4); if (bin>3) bin=3;
                    g_adaptive_stats.phase[bin]++; g_adaptive_stats.generated++;
                } else g_adaptive_stats.generator_refused++;
                if (target>UINT64_MAX-b->refresh_ns) break;
                target+=b->refresh_ns;
            }
        }
        if (g_adaptive_stats.real==300) {
            double seconds=(g_adaptive_stats.last_ns-g_adaptive_stats.first_ns)/1e9;
            LOGV("[adaptive] last300-real fps=%.3f generated=%llu fraction=%.4f phase=[%llu,%llu,%llu,%llu] states warmup=%llu active=%llu passthrough=%llu scene=%llu floor=%llu stale=%llu clock=%llu horizon=%llu shown=%llu missed=%llu real-won=%llu generator-refused=%llu (app slots, not SF scanout)",
                seconds>0?299/seconds:0,(unsigned long long)g_adaptive_stats.generated,
                (double)g_adaptive_stats.generated/(300+g_adaptive_stats.generated),
                (unsigned long long)g_adaptive_stats.phase[0],(unsigned long long)g_adaptive_stats.phase[1],
                (unsigned long long)g_adaptive_stats.phase[2],(unsigned long long)g_adaptive_stats.phase[3],
                (unsigned long long)g_adaptive_stats.refused[0],(unsigned long long)g_adaptive_stats.refused[1],
                (unsigned long long)g_adaptive_stats.refused[2],(unsigned long long)g_adaptive_stats.refused[3],
                (unsigned long long)g_adaptive_stats.refused[4],(unsigned long long)g_adaptive_stats.refused[5],
                (unsigned long long)g_adaptive_stats.refused[6],(unsigned long long)g_adaptive_stats.refused[7],
                (unsigned long long)atomic_exchange(&g_adaptive_stats.shown,0),
                (unsigned long long)atomic_exchange(&g_adaptive_stats.missed,0),
                (unsigned long long)atomic_exchange(&g_adaptive_stats.suppressed,0),
                (unsigned long long)g_adaptive_stats.generator_refused);
            g_adaptive_stats.real=g_adaptive_stats.first_ns=g_adaptive_stats.last_ns=g_adaptive_stats.generated=0;
            g_adaptive_stats.generator_refused=0;
            memset(g_adaptive_stats.phase,0,sizeof g_adaptive_stats.phase);
            memset(g_adaptive_stats.refused,0,sizeof g_adaptive_stats.refused);
        }
        if (adaptive_lease) (void)vk_replay_lease_end(adaptive_lease,0);
        return;
    }
    int interpolate=vk_fg_interpolating() &&
        (!runtime_fps_hybrid_selected() || runtime_fps_hybrid_interpolating());
    if (qcom) interpolate=0;
    if (interpolate) {
        vk_present_slot slot = {.desired_ns = b->desired_ns, .copy_open_ns = b->copy_open_ns,
            .refresh_ns = b->refresh_ns, .skipped = b->slot_skipped, .interval = b->interval,
            .mode = b->refresh_ns ? VK_PRESENT_SLOTS_ACTIVE : VK_PRESENT_SLOTS_NO_CLOCK};
        vk_present_interp_pair pair = vk_present_interp_push(&g_pres_interp,i,b->frame,generation,slot,host_now_ns());
        if (vk_fg_locked()) pair=vk_present_interp_buffer(pair,vk_fg_lead_refreshes(),host_now_ns());
        runtime_present_timeline_entry real_timeline,child_timeline;
        if (g_pres_timeline) {
            real_timeline=(runtime_present_timeline_entry){0};
            child_timeline=(runtime_present_timeline_entry){0};
            pres_timeline_pair(&pair,b,&real_timeline,&child_timeline);
        }
        if (pair.reason == VK_INTERP_READY) g_fg_ready++;
        if (FGX.diag && FGX.eligible) g_fg_admission.pair[pair.reason]++;
        if (pair.real >= 0) {
            pres_buf *older = &g_pres[pair.real];
            older->desired_ns = pair.slot.desired_ns; older->copy_open_ns = pair.slot.copy_open_ns;
            older->refresh_ns = pair.slot.refresh_ns; older->interval = pair.slot.interval;
            older->slot_skipped = pair.slot.skipped; older->source_fence = b->fence;
            if (g_pres_timeline) {
                older->timeline_role=real_timeline.expected_ns ? RUNTIME_PRESENT_TIMELINE_REAL:0;
                if (older->timeline_role) older->display_timeline=real_timeline;
            }
            pres_enqueue(pair.real);
            if (pair.midpoint_ns && vk_fg_enabled()) present_generated(t,x,srgb,b,ow,oh,crop,pair.midpoint_ns,g_pres_timeline?&child_timeline:NULL,0.5f,0);
        }
    } else {
        int retained=vk_present_interp_leave(&g_pres_interp);
        if (retained>=0) {
            /* No compositor ownership yet. FIFO waits the covering GPU fence
             * before clearing busy; queued children retain their deadlines. */
            g_pres[retained].discard=1; pres_enqueue(retained);
            LOGV("[hybrid-present] native return: retiring retained real; reserved past queued child; latency step removes one unshown real");
        }
        if (qcom) {
            vk_present_interp_pair direct={.real=i,.reason=VK_INTERP_READY,
                .slot={.desired_ns=b->desired_ns,.copy_open_ns=b->copy_open_ns,
                    .refresh_ns=b->refresh_ns,.interval=b->interval,
                    .mode=b->refresh_ns ? VK_PRESENT_SLOTS_ACTIVE : VK_PRESENT_SLOTS_NO_CLOCK}};
            direct=vk_present_interp_buffer(direct,vk_fg_lead_refreshes(),host_now_ns());
            b->desired_ns=direct.slot.desired_ns; b->copy_open_ns=direct.slot.copy_open_ns;
            pres_enqueue(i);
            if (b->interval==2 && direct.midpoint_ns && vk_fg_enabled())
                present_generated(t,x,srgb,b,ow,oh,crop,direct.midpoint_ns,NULL,0.5f,0);
            return;
        }
        pres_enqueue(i);
        if (vk_fg_enabled() && !vk_fg_interpolating()) present_generated(t,x,srgb,b,ow,oh,crop,0,NULL,0.5f,0);
    }

    return;
cancel:
    if (adaptive_lease) (void)vk_replay_lease_end(adaptive_lease,0);
    if (vk_fg_adaptive()) fg_adaptive_present_cancel();
    runtime_android_predict_cancel(&prediction);
}
#endif
#ifndef TOTK_ANDROID_APP
_Atomic int vk_fg_slot_boundary_active; /* no app presenter: disabled unless a TEST fixture owns work */
#ifdef TOTK_VK_TESTS
static void (*fg_test_slot_boundary_callback)(void *);
static void *fg_test_slot_boundary_arg;
void vk_fg_test_slot_boundary_callback(void (*callback)(void *),void *arg) {
    fg_test_slot_boundary_callback=callback; fg_test_slot_boundary_arg=arg;
}
void vk_fg_slot_boundary(void) {
    if (fg_test_slot_boundary_callback)
        fg_test_slot_boundary_callback(fg_test_slot_boundary_arg);
}
#else
void vk_fg_slot_boundary(void) {}
#endif
#endif /* !TOTK_ANDROID_APP */
/* The recording owner invokes this on every bypassed real observation. Source
 * history and simulation policy must restart together; frame-end still owns
 * GPU/output retirement and budget accounting. The host fixture uses this same
 * helper even when the app-only presenter is not compiled. */
static void fg_adaptive_present_cancel(void) {
#ifdef TOTK_ANDROID_APP
    pthread_mutex_lock(&g_win_mu);
    g_adaptive_policy=(fg_adaptive_policy){0};
    if (pres_slots_requested()) pres_slots_end_locked();
    fg_adaptive_plan_reset(&g_adaptive_plan);
    if (g_adaptive_presenting>=0)
        atomic_store_explicit(&g_pres[g_adaptive_presenting].adaptive_suppressed,1,memory_order_release);
#endif
    runtime_fps_adaptive_reset();
    vk_fg_skip();
#ifdef TOTK_ANDROID_APP
    pres_adaptive_wake_locked();
    pthread_mutex_unlock(&g_win_mu);
#endif
}
#ifdef TOTK_VK_TESTS
void vk_fg_test_present_cancel(void) { fg_adaptive_present_cancel(); }
#endif
/* Upstream hitch attribution, opt-in because per-frame median sorting and cross-thread atomics are not free.
   Drain every present, not only on hitches: detail lines describe one frame rather than everything since the last hitch. */
static void hitch_present(uint64_t now, uint64_t frame) {
    if (!g_hitch_on) return;
    typedef struct {
#define HK_VALUE(name) uint64_t name;
        VK_HITCH_FIELDS(HK_VALUE)
#undef HK_VALUE
    } hitch_values;
    hitch_values v;
    static hitch_values total;
#define HK_TAKE(name) v.name = atomic_exchange_explicit(&g_hk.name, 0, memory_order_relaxed); total.name += v.name;
    VK_HITCH_FIELDS(HK_TAKE)
#undef HK_TAKE
    static uint64_t last, recent[64], bins[5], worst, count, sum;
    static unsigned nr, next, logged;
    uint64_t dt = last ? now - last : 0; last = now;
    if (!dt) return;
    recent[next] = dt; next = (next + 1) % 64; if (nr < 64) nr++;
    uint64_t sorted[64]; memcpy(sorted, recent, nr * sizeof *sorted);
    for (unsigned i = 1; i < nr; i++) for (unsigned j = i; j && sorted[j - 1] > sorted[j]; j--) {
        uint64_t t = sorted[j]; sorted[j] = sorted[j - 1]; sorted[j - 1] = t;
    }
    double median = (nr & 1 ? sorted[nr / 2] : sorted[nr / 2 - 1] / 2 + sorted[nr / 2] / 2) / 1e6;
    static const uint64_t thresholds[] = {40000000, 60000000, 100000000, 200000000, 400000000};
    for (int i = 4; i >= 0; i--) if (dt >= thresholds[i]) { bins[i]++; break; }
    if (dt > worst) worst = dt;
    count++; sum += dt;
    if (dt >= 40000000 && dt / 1e6 >= 2.5 * median && logged < 24) {
        logged++;
        LOGV("hitch at frame %llu: +%.1f ms (median %.1f): replay %.1f ms in %llu submits (GPU wait %.1f ms); pipelines %llu (%.1f ms, %llu sync-built, %llu failed, %llu retry, %llu bounded, %llu skipped draws); shader loads %llu (%.0f KiB), variant lists %llu, requests %llu; descriptor pools %llu, chunks %llu, cpu steps %llu; gpu idles %llu (%.1f ms, %llu finishes, %llu flush waits, %llu pool drops); invalidates %llu (%.1f ms, %llu writebacks, %.1f MiB); game waits %llu (%.1f ms); completion %.1f ms, wake %.1f ms",
             (unsigned long long)frame, dt / 1e6, median, v.replay_ns / 1e6, (unsigned long long)v.subs, v.wait_ns / 1e6,
             (unsigned long long)v.pipes, v.pipe_ns / 1e6, (unsigned long long)v.pipes_sync, (unsigned long long)v.pipes_fail,
             (unsigned long long)v.pipe_retries, (unsigned long long)v.pipe_bounds, (unsigned long long)v.pipe_skips,
             (unsigned long long)v.shloads, v.shbytes / 1024.0, (unsigned long long)v.vlists, (unsigned long long)v.sreq,
             (unsigned long long)v.dpools, (unsigned long long)v.chunks, (unsigned long long)v.cpu_steps,
             (unsigned long long)v.idles, v.idle_ns / 1e6, (unsigned long long)v.idle_fin, (unsigned long long)v.idle_flush, (unsigned long long)v.idle_pool,
             (unsigned long long)v.inval, v.inval_ns / 1e6, (unsigned long long)v.wb, v.wb_bytes / 1048576.0,
             (unsigned long long)v.gwaits, v.gwait_ns / 1e6, v.comp_ns / 1e6, v.wake_ns / 1e6);
    }
    if (frame % 300 == 0) {
        LOGV("hitches, last %llu frames: 40-60 ms %llu, 60-100 %llu, 100-200 %llu, 200-400 %llu, >=400 %llu; worst %.1f ms, median %.1f ms, mean %.1f ms; pipelines %llu failed, %llu retries, %llu bounded, %llu skipped draws%s",
             (unsigned long long)count, (unsigned long long)bins[0], (unsigned long long)bins[1], (unsigned long long)bins[2],
             (unsigned long long)bins[3], (unsigned long long)bins[4], worst / 1e6, median, sum / (double)count / 1e6,
             (unsigned long long)total.pipes_fail, (unsigned long long)total.pipe_retries, (unsigned long long)total.pipe_bounds,
             (unsigned long long)total.pipe_skips, logged == 24 ? " (detail lines capped at 24)" : "");
        memset(&total, 0, sizeof total); memset(bins, 0, sizeof bins); worst = count = sum = 0; logged = 0;
    }
}
uint64_t mtl_present(const tex_t *t, double period, const int crop[4]) {
#ifndef TOTK_ANDROID_APP
    (void)period;
#endif
    uint64_t now = host_now_ns(), n = ++g_present_n;
    hitch_present(now, n);
    if (vk_lag_capture_on()) nvn_dirty_lag_frame(n + 1);
    probe_frame_end(n);
    pd_frame_end(n);
    if (g_fr_n) {
        qsort(g_fr, (size_t)g_fr_n, sizeof *g_fr, u32cmp);
        if (g_fr_n != g_prev_n || memcmp(g_fr, g_prev, (size_t)g_fr_n * sizeof *g_fr)) {
            uint64_t c = ++g_stat_rescg;
            if (g_prev_n && (c <= 64 || c % 100 == 0)) { char a[300], b[300]; sizes_str(g_prev, g_prev_n, a, sizeof a); sizes_str(g_fr, g_fr_n, b, sizeof b);
                LOGV("depth target sizes at frame %llu: %s (frame before: %s; %llu changes so far)", (unsigned long long)n, b, a, (unsigned long long)c); }
            memcpy(g_prev, g_fr, sizeof g_fr); g_prev_n = g_fr_n;
        }
    }
    g_fr_n = 0;
    g_pres_t[n % 64] = now / 1e9;
    if (!g_present_t0) g_present_t0 = now;
    if (g_last_present_ns) { uint64_t dt = now - g_last_present_ns; if (dt > g_win_frame_max_ns) g_win_frame_max_ns = dt; g_win_over33 += dt > 33333333; g_win_over50 += dt > 50000000; }
    { /* a frame over 100 ms: where its time went in the backend; the rest is the game's own work */
      static uint64_t prev[5], prev_pipe, prev_cp, prev_io; static int nlog;
      uint64_t c[5], pipe = g_cum_pipe_ns, cp = vk_compute_pipe_cum_ns(), io = vk_shader_io_cum_ns(); vk_perf_cum(c);
      if (g_last_present_ns && now - g_last_present_ns > 100000000 && nlog++ < 200)
          LOGV("long frame %llu: %.0f ms since the last present; recording %.1f ms (pipelines made at the draw %.1f, compute pipelines %.1f, seed files %.1f), "
               "GPU waits while recording %.1f ms, the game's sync waits %.1f ms (%llu), %llu CPU steps", (unsigned long long)n, (now - g_last_present_ns) / 1e6,
               (c[0] - prev[0]) / 1e6, (pipe - prev_pipe) / 1e6, (cp - prev_cp) / 1e6, (io - prev_io) / 1e6, (c[1] - prev[1]) / 1e6, (c[2] - prev[2]) / 1e6,
               (unsigned long long)(c[4] - prev[4]), (unsigned long long)(c[3] - prev[3]));
      vk_pipeline_keys_pressure(now, c[0]-prev[0] > 25000000 || c[1]-prev[1] > 4000000);
      memcpy(prev, c, sizeof prev); prev_pipe = pipe; prev_cp = cp; prev_io = io; }
    totk_perf_render(g_last_present_ns ? now - g_last_present_ns : 0);
    vk_pipeline_keys_pressure(now, g_last_present_ns && now-g_last_present_ns > 40000000);
    g_last_present_ns = now;
    static long every = -1; if (every < 0) { const char *e = getenv("TOTK_VK_SHOT"); every = e ? atol(e) : 0; if (every) LOGV("saving every %ld-th frame as PNG (TOTK_VK_SHOT)", every); }
    vk_rec_lock();
    if (__builtin_expect(g_pt_on, 0)) vk_pt_present(n);   /* oot-a122 TOTK_OHOS_PROTECT: hot set, next arena, reports (under the recording lock: walker state) */
    static uint64_t pcache_draws;
    vk_pcache_present(now, g_stat_draws - pcache_draws); pcache_draws = g_stat_draws;
    if (pipe_predict_on()) {
        nvn_scan_budget_frame(&g_pp_budget, ++g_pp_frame);
        g_pp_scans_in_sub = 0; g_pp_submit = g_pp_pass = 0; g_pp_more = 0;
    }
    if (__builtin_expect(vk_rec_profile_on, 0)) vk_rec_profile_begin(VK_REC_O_PRESENT);
    if (every > 0 && t && n % (uint64_t)every == 0) shot(t, n, crop);
    pp_present(n); pd_present(n);
    if (vk_canvas_on && t) {
        vk_canvas_image window=vk_tex_canvas_identity(vk_tex_get(t),0,0);
        vk_canvas_present(&window);
    }
    if (vk_gpuprof_on) vk_end_rendering_at(VK_PASS_END_PRESENT, 0);
#ifdef TOTK_ANDROID_APP
    int fg_requested=fg_extract_requested_config();
    vk_fg_present_begin(t);
    vk_overlay_target(period);
    present_window(t, period, crop);
    pres_extent_report(n);
    g_overlay_scene_w = g_overlay_scene_h = 0;
    if (fg_requested) {
        /* Include every source copy/read even when the window job was dropped.
         * A synthetic output never becomes a separate guest frame. */
        vk_fg_present_end(vk_submit_now());
    }
#endif
    /* The real-frame endpoint follows our post, window copy and synthetic GPU
     * recordings, including their slot submissions, rather than the game pass. */
    if (vk_gpuprof_on) vk_gpuprof_present();
    vk_native_frame_end();
    if (vk_canvas_on) vk_canvas_frame_end();
    if (__builtin_expect(vk_rec_profile_on, 0)) { vk_rec_profile_end(); vk_rec_profile_present(n); }
    vk_record_stats(n);
    xlate_draw_frame(n);
    vk_xlate_recording_stats_frame(n);
    if (n % 300 == 0 && vk_record_enabled()) {
        uint64_t hits, misses; vk_prog_record_cache_take(&hits, &misses);
        LOGV("recorder memo per frame: program hit/miss %.1f/%.1f, binding freeze hit/miss %.1f/%.1f",
            hits / 300.0, misses / 300.0, g_record_freeze_hits / 300.0, g_record_freeze_misses / 300.0);
        g_record_freeze_hits = g_record_freeze_misses = 0;
    }
    vk_rec_unlock();
    totk_pgo_frame(n);
    if (n % 300 == 0) {
        vk_rec_lock(); vk_occlusion_stats(n); gpu_work_stats(n); native_gpu_stats(n);
        LOGV("[load-elide] frame %llu: last 300 presents %llu proven, %llu elided passes",
             (unsigned long long)n, (unsigned long long)g_load_proven, (unsigned long long)g_load_elided);
        g_load_proven = g_load_elided = 0;
        LOGV("[feedback-narrow] frame %llu: last 300 presents %llu deferred, %llu avoided pass ends, %llu retained late hazards",
             (unsigned long long)n,(unsigned long long)g_feedback_deferred,
             (unsigned long long)g_feedback_avoided,(unsigned long long)g_feedback_hazards);
        g_feedback_deferred=g_feedback_avoided=g_feedback_hazards=0;
        LOGV("[alpha-test] frame %llu: last 300 presents enabled=%llu ignored=%llu",
             (unsigned long long)n,(unsigned long long)g_alpha_enabled,(unsigned long long)g_alpha_ignored);
        g_alpha_enabled=g_alpha_ignored=0;
        LOGV("[clear-on-read] frame %llu: last 300 presents forced=%llu reason=first-read",
             (unsigned long long)n,(unsigned long long)g_clear_read_forced);
        g_clear_read_forced=0;
#ifdef TOTK_ANDROID_APP
        if (g_guest_extent_diag && vk_overlay_mode()) {
            static vk_overlay_counters previous;
            vk_overlay_counters current=vk_overlay_get_counters(NULL);
            LOGV("[overlay] frame=%llu layout-rebuilds=%llu text-field-updates=%llu graph-appends=%llu (last 300 real; glyph fields <=1Hz; no per-frame layout/format/resampling)",
                (unsigned long long)n,(unsigned long long)(current.layout_rebuilds-previous.layout_rebuilds),
                (unsigned long long)(current.text_field_updates-previous.text_field_updates),
                (unsigned long long)(current.graph_appends-previous.graph_appends));
            previous=current;
        }
#endif
        if (pass_merge_on()) {
            static uint64_t previous[4]; uint64_t c[4]; vk_pass_merge_counters(c);
            LOGV("[pass-merge] frame %llu: deferred %.2f retained %.2f barriers/frame; late hazard splits %.2f/frame; avoided %.2f pass boundaries/frame (not a GMEM traffic counter)",
                 (unsigned long long)n, (c[0] - previous[0]) / 300.0, (c[1] - previous[1]) / 300.0,
                 (c[2] - previous[2]) / 300.0, (c[3] - previous[3]) / 300.0);
            memcpy(previous, c, sizeof previous);
        }
        if (split_pass_draws() || split_pass_live()) {
            static uint64_t previous[5];
            uint64_t c[5] = {g_split_count, g_split_reopen, g_split_refused, g_split_touches, g_split_lrz_refused};
            LOGV("[split-pass] frame %llu: last 300 presents %llu splits, %llu extra passes (reopened parts), %llu refused splits, %llu depth touches, %llu LRZ refusals; per frame %.2f/%.2f/%.2f (TOTK_VK_SPLIT_PASS_DRAWS=%u TOTK_VK_SPLIT_LRZ_SAFE=%d)",
                 (unsigned long long)n, (unsigned long long)(c[0] - previous[0]), (unsigned long long)(c[1] - previous[1]),
                 (unsigned long long)(c[2] - previous[2]), (unsigned long long)(c[3] - previous[3]),
                 (unsigned long long)(c[4] - previous[4]), (c[0] - previous[0]) / 300.0, (c[1] - previous[1]) / 300.0,
                 (c[2] - previous[2]) / 300.0, split_pass_live(), split_lrz_safe());
            memcpy(previous, c, sizeof previous);
        }
        if (atomic_load_explicit(&g_feedback_cut, memory_order_relaxed) || g_feedback_cut_launch) {
            static uint64_t previous[2]; uint64_t c[2] = {g_feedback_cuts, g_feedback_uncut};
            LOGV("[feedback-cut] frame %llu: last 300 presents %llu sampled views cut below an attached level, %llu left whole (based at it)",
                 (unsigned long long)n, (unsigned long long)(c[0] - previous[0]), (unsigned long long)(c[1] - previous[1]));
            memcpy(previous, c, sizeof previous);
        }
        if (g_prepare_mask_launch || prepare_mask_live() || g_prepare_check) {
            static uint64_t previous[3]; uint64_t c[3] = {g_prepare_calls, g_prepare_skips, g_prepare_checks};
            LOGV("[prepare-mask] frame %llu: last 300 presents %llu uniform preparations, %llu skipped, %llu oracle checks (TOTK_VK_PREPARE_MASK=%d)",
                 (unsigned long long)n, (unsigned long long)(c[0] - previous[0]), (unsigned long long)(c[1] - previous[1]),
                 (unsigned long long)(c[2] - previous[2]), prepare_mask_live());
            memcpy(previous, c, sizeof previous);
        }
        { static uint64_t previous[5]; static int shown; uint64_t c[5]; vk_tex_memo_counts(c);
          int on = vk_tex_memo_on();
          if (on || shown) {
              uint64_t hits = c[0] - previous[0], misses = c[1] - previous[1];
              LOGV("[tex-memo] frame %llu: per frame %.1f hits %.1f misses (%.1f%% hit), %.1f retirements; registrations %.1f, %.1f unchanged (kept every revision) (TOTK_VK_TEX_MEMO=%d)",
                   (unsigned long long)n, hits / 300.0, misses / 300.0, hits + misses ? 100.0 * hits / (hits + misses) : 0.0,
                   (c[2] - previous[2]) / 300.0, (c[3] - previous[3]) / 300.0, (c[4] - previous[4]) / 300.0, on);
              shown = on;
          }
          memcpy(previous, c, sizeof previous); }
        vk_rec_unlock();
#ifdef TOTK_OHOS
        { extern void vk_pass_end_report(uint64_t); vk_pass_end_report(n); }
#endif
        LOGV("frame %llu: %.1f fps over the last 300, %llu draws, %llu passes, %llu pipelines, %llu dispatches; skipped: no program %llu, disabled %llu, culled %llu, primitive %llu, shader missing %llu, scissor %llu, pipeline %llu, pipeline compiling %llu",
             (unsigned long long)n, 300e9 / (double)(now - g_present_t0), (unsigned long long)g_stat_draws, (unsigned long long)g_stat_passes, (unsigned long long)g_stat_pipes,
             (unsigned long long)g_stat_dispatch, (unsigned long long)g_stat_skips[0], (unsigned long long)g_stat_skips[1], (unsigned long long)g_stat_skips[2],
             (unsigned long long)g_stat_skips[3], (unsigned long long)g_stat_skips[4], (unsigned long long)g_stat_skips[6], (unsigned long long)g_stat_skips[7],
             (unsigned long long)g_stat_skips[10]);
        LOGV("frame %llu, stutter: longest frame %.1f ms, %llu frames over 33 ms, %llu over 50 ms; %llu new pipelines took %.1f ms (longest %.1f ms); %llu compiled on worker threads in %.1f ms; sync compiles %llu in %.1f ms; prewarm waits %llu in %.1f ms",
             (unsigned long long)n, g_win_frame_max_ns / 1e6, (unsigned long long)g_win_over33, (unsigned long long)g_win_over50,
             (unsigned long long)g_win_pipes, g_win_pipe_ns / 1e6, g_win_pipe_max_ns / 1e6, (unsigned long long)atomic_exchange(&g_win_async, 0), atomic_exchange(&g_win_async_ns, 0) / 1e6,
             (unsigned long long)g_win_sync, g_win_sync_ns / 1e6, (unsigned long long)g_win_prewarm_wait, g_win_prewarm_wait_ns / 1e6);
        LOGV("frame %llu, pipeline joins: %llu waits on worker builds (graphics and compute) in %.1f ms; boosts %llu, refused %llu (TOTK_VK_PIPE_BOOST=%d)",
             (unsigned long long)n, (unsigned long long)atomic_exchange(&g_boost_joins, 0), atomic_exchange(&g_boost_join_ns, 0) / 1e6,
             (unsigned long long)atomic_exchange(&g_boost_raised, 0), (unsigned long long)atomic_exchange(&g_boost_refused, 0), pipe_boost_on());
        if (g_unwritten_mask || unwritten_mask_live())
            LOGV("frame %llu, unwritten colour attachments: %llu new pipelines masked %llu attachments their fragment shader does not output (TOTK_VK_UNWRITTEN_MASK=%d)",
                 (unsigned long long)n, (unsigned long long)atomic_exchange(&g_unwritten_pipes, 0), (unsigned long long)atomic_exchange(&g_unwritten_attachments, 0),
                 unwritten_mask_live());
        if (V.has_pipeline_library) {
            static uint64_t previous[7], previous_parts[PL_COUNT], previous_ns[PL_COUNT];
            uint64_t current[7] = {atomic_load(&g_library_stats.fast_links), atomic_load(&g_library_stats.fast_link_ns),
                atomic_load(&g_library_stats.fast_draws), atomic_load(&g_library_stats.optimized_replacements),
                atomic_load(&g_library_stats.optimized_ns), atomic_load(&g_library_stats.library_skips),
                atomic_exchange(&g_library_stats.fast_link_window_max_ns, 0)};
            LOGV("[pipeline-library] fast links %llu in %.3f ms (max %.3f ms), fast draws %llu, optimized replacements %llu in %.3f ms, misses still skipped %llu",
                (unsigned long long)(current[0] - previous[0]), (current[1] - previous[1]) / 1e6, current[6] / 1e6,
                (unsigned long long)(current[2] - previous[2]), (unsigned long long)(current[3] - previous[3]),
                (current[4] - previous[4]) / 1e6, (unsigned long long)(current[5] - previous[5]));
            static const char *names[PL_COUNT] = {"vertex-input", "pre-raster", "fragment", "output"};
            for (int i = 0; i < PL_COUNT; i++) {
                uint64_t count = atomic_load(&g_library_stats.parts[i]), ns = atomic_load(&g_library_stats.part_ns[i]);
                LOGV("[pipeline-library] %s parts %llu in %.3f ms", names[i],
                    (unsigned long long)(count - previous_parts[i]), (ns - previous_ns[i]) / 1e6);
                previous_parts[i] = count; previous_ns[i] = ns;
            }
            memcpy(previous, current, sizeof current);
        }
        g_win_pipes = g_win_pipe_ns = g_win_pipe_max_ns = g_win_frame_max_ns = g_win_over33 = g_win_over50 = 0;
        g_win_sync = g_win_sync_ns = g_win_prewarm_wait = g_win_prewarm_wait_ns = 0;
        { uint64_t fn, fns, fmax; vk_shader_io_take(&fn, &fns, &fmax);
          LOGV("frame %llu, shader seed files read: %llu in %.1f ms (longest %.1f ms)", (unsigned long long)n, (unsigned long long)fn, fns / 1e6, fmax / 1e6); }
        LOGV("frame %llu, diagnostics: %llu passes split by barriers; missing textures %llu, buffers outside the pools %llu",
             (unsigned long long)n, (unsigned long long)g_stat_inpass_barriers, (unsigned long long)g_stat_dummy_tex, (unsigned long long)g_stat_zero_buf);
        double rp, wt, gp, cpu, elapsed; uint64_t ns;
        vk_rec_lock(); vk_perf_take(&rp, &wt, &gp, &cpu, &elapsed, &ns); vk_rec_unlock();
        LOGV("frame %llu, per frame: recording %.1f ms, waiting for the GPU %.1f ms, GPU %.1f ms, %.1f submissions; descriptor sets %llu written, %llu reused",
             (unsigned long long)n, rp / 300, wt / 300, gp / 300, ns / 300.0, (unsigned long long)g_stat_sets_new, (unsigned long long)g_stat_sets_reused);
#if defined(TOTK_OHOS) && defined(TOTK_GAME_OOT)
        {   /* oot-a121 TOTK_OHOS_PUSHDESC: what the stage sets cost, per frame */
            void vk_record_push_counts(uint64_t out[4]);   /* vk_record.c */
            static uint64_t previous[4]; uint64_t now[4]; vk_record_push_counts(now);
            LOGV("frame %llu, stage sets per frame (oot-a121, TOTK_OHOS_PUSHDESC=%d, %s pushed): recorder written %.1f reused %.1f pushed %.1f push-skipped %.1f; "
                 "primary written %.1f reused %.1f pushed %.1f push-skipped %.1f bindless-kept %.1f above-kept %.1f; shaders with a push layout %llu, over maxPushDescriptors %u: %llu",
                 (unsigned long long)n, vk_push_bits, vk_push_stage == 1 ? "fragment sets" : vk_push_stage == 0 ? "vertex sets" : "nothing",
                 (now[2] - previous[2]) / 300.0, (now[3] - previous[3]) / 300.0, (now[0] - previous[0]) / 300.0, (now[1] - previous[1]) / 300.0,
                 g_stat_sets_new / 300.0, g_stat_sets_reused / 300.0, g_push_n[0] / 300.0, g_push_n[1] / 300.0, g_push_n[2] / 300.0, g_push_n[3] / 300.0,
                 (unsigned long long)vk_push_shader_n[0], vk_push_max, (unsigned long long)vk_push_shader_n[1]);
            memcpy(previous, now, sizeof previous); memset(g_push_n, 0, sizeof g_push_n);
        }
#endif
        static uint64_t previous_draws;
        uint64_t draws = g_stat_draws - previous_draws; previous_draws = g_stat_draws;
        LOGV("walker cpu frame %llu: cpu %.3f ms/frame, wall %.3f ms/frame, draws %.3f/frame, cpu %.3f us/draw, offcpu %.3f ms/frame",
             (unsigned long long)n, cpu / 300, elapsed / 300, draws / 300.0,
             draws ? cpu * 1000 / (double)draws : 0, (elapsed - cpu) / 300);
        vk_upload_report(n);
        g_stat_sets_new = g_stat_sets_reused = 0;
        vk_lookup_stats(n);
        vk_lag_stats(n);
        { void vk_mem_report(void); vk_mem_report(); }
        nvn_dirty_mp_report(n);
#ifdef TOTK_ANDROID_APP
        LOGV("window: %llu frames shown, %llu not shown (all buffers busy)", (unsigned long long)g_pres_shown, (unsigned long long)g_pres_dropped);
        LOGV("window: %llu discarded (window changed or present failed)",
             (unsigned long long)g_pres_cancelled);
        if (runtime_fps_hybrid_selected())
            LOGV("[hybrid-display] last 300 real: mode=%s shown-total=%llu; framework-fence interval bins ms [<12.5,12.5-20.9,20.9-29,29-37.5,37.5-45.8,>=45.8]=%llu/%llu/%llu/%llu/%llu/%llu missing=%llu; AHB only, includes real+synthetic, asynchronous boundaries, NOT scanout",
                runtime_fps_hybrid_interpolating()?"real30+interp":"native60",(unsigned long long)g_pres_shown,
                (unsigned long long)atomic_exchange(&g_hybrid_bins[0],0),
                (unsigned long long)atomic_exchange(&g_hybrid_bins[1],0),
                (unsigned long long)atomic_exchange(&g_hybrid_bins[2],0),
                (unsigned long long)atomic_exchange(&g_hybrid_bins[3],0),
                (unsigned long long)atomic_exchange(&g_hybrid_bins[4],0),
                (unsigned long long)atomic_exchange(&g_hybrid_bins[5],0),
                (unsigned long long)atomic_exchange(&g_hybrid_missing,0));
        if (fg_requested) {
            LOGV("[framegen-display] last 300 real frames: ready=%llu generated=%llu completion-accepted=%llu handed-off=%llu expired=%llu no-buffer=%llu pending=%llu; generated=recorded outputs, handoff is NOT scanout; async window boundaries",
                (unsigned long long)atomic_exchange(&g_fg_ready,0),
                (unsigned long long)atomic_exchange(&g_fg_generated,0),
                (unsigned long long)atomic_exchange(&g_fg_accepted,0),
                (unsigned long long)atomic_exchange(&g_fg_handoffs,0),
                (unsigned long long)atomic_exchange(&g_fg_expired,0),
                (unsigned long long)atomic_exchange(&g_fg_no_buffer,0),
                (unsigned long long)atomic_load(&g_fg_pending));
            LOGV("[framegen-expiry] source-missing=%llu source-late=%llu child-deadline=%llu completion-refused=%llu output=%llu window=%llu post=%llu",
                (unsigned long long)atomic_exchange(&g_fg_expiry[FRAMEGEN_CHILD_SOURCE_MISSING],0),
                (unsigned long long)atomic_exchange(&g_fg_expiry[FRAMEGEN_CHILD_SOURCE_LATE],0),
                (unsigned long long)atomic_exchange(&g_fg_expiry[FRAMEGEN_CHILD_DEADLINE],0),
                (unsigned long long)atomic_exchange(&g_fg_expiry[FRAMEGEN_CHILD_COMPLETION],0),
                (unsigned long long)atomic_exchange(&g_fg_expiry[FRAMEGEN_CHILD_OUTPUT],0),
                (unsigned long long)atomic_exchange(&g_fg_expiry[FRAMEGEN_CHILD_WINDOW],0),
                (unsigned long long)atomic_exchange(&g_fg_expiry[FRAMEGEN_CHILD_POST],0));
        }
        if (fg_requested && FGX.diag) {
            LOGV("[framegen-admission] eligible-pairs ready=%llu no-midpoint(warmup/adjacency/clock/interval/refresh/late/slot)=%llu/%llu/%llu/%llu/%llu/%llu/%llu no-buffer=%llu source-shape=%llu generator-refused=%llu recorded=%llu",
                (unsigned long long)g_fg_admission.pair[VK_INTERP_READY],
                (unsigned long long)g_fg_admission.pair[VK_INTERP_WARMUP],
                (unsigned long long)g_fg_admission.pair[VK_INTERP_ADJACENCY],
                (unsigned long long)g_fg_admission.pair[VK_INTERP_CLOCK],
                (unsigned long long)g_fg_admission.pair[VK_INTERP_INTERVAL],
                (unsigned long long)g_fg_admission.pair[VK_INTERP_REFRESH],
                (unsigned long long)g_fg_admission.pair[VK_INTERP_LATE],
                (unsigned long long)g_fg_admission.pair[VK_INTERP_SLOT],
                (unsigned long long)g_fg_admission.no_buffer,(unsigned long long)g_fg_admission.source_shape,
                (unsigned long long)g_fg_admission.generator_refused,(unsigned long long)g_fg_admission.recorded);
            memset(&g_fg_admission,0,sizeof g_fg_admission);
        }
#endif
        g_present_t0 = now;
    }
    /* The ticket is the fence of the work this present follows (the frame's submissions and the window copy):
       acquiring this swapchain texture again waits for it (mtl_present_wait), as on the Mac. Without that the game ran
       several frames ahead of the GPU and rewrote per-frame data (lights, constants) the GPU had not read yet: the lit
       scene went dark for a frame now and then. */
    pso_life_present();
    g_last_ticket = mtl_last_fence() ? mtl_last_fence() : 1;
    return g_last_ticket;
}
/* TOTK_VK_FRAMES_AHEAD=1: acquiring any swapchain texture waits for the latest present (the game at most one frame
   ahead of the GPU, as on the Switch where the GPU is rarely behind); default 2 (the texture's own last present). */
void mtl_present_wait(uint64_t tk) {
    static int off = -1, ahead = 2; if (off < 0) { off = vk_off("present_wait"); const char *e = getenv("TOTK_VK_FRAMES_AHEAD"); if (e && atoi(e) == 1) ahead = 1; }
    if (off) return;
    if (ahead == 1 && g_last_ticket > tk) tk = g_last_ticket;
    if (tk) mtl_wait(tk, 1000000000ull);
}
double mtl_presented_time(uint64_t n) { return n && n + 64 > g_present_n ? g_pres_t[n % 64] : 0; }

uint64_t vk_present_count(void) { return g_present_n; }   /* tests */
