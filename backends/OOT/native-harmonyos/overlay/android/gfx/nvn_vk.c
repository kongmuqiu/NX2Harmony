/* The NVN replay interface (third_party/d2r_gfx/gfx/nvn/nvn_mtl.h) on Vulkan: Linux / Android. This file: the device,
   memory pools, submissions and their completion, and the command walker; textures, shaders and drawing are in
   vk_res.c, vk_shader.c and vk_draw.c (vk_int.h). android/VULKAN.md has the plan and the reasons.

   Every effect the game can observe of GPU completion (event values, counter reports, fences) happens only after the
   GPU has really finished the submission, as nvn_mtl.m does on Metal.

   Memory pools stay zero-copy: the game and the GPU use the same pages. Three ways, chosen at start
   (TOTK_VK_POOLS=host|export|heap forces one):
     host    import the guest's pages as device memory           (VK_EXT_external_memory_host; not on Adreno drivers)
     export  device memory exported as a dma-buf, mmap'd over the pool's guest address range (Turnip)
     heap    a dma-buf from /dev/dma_heap, imported, mmap'd the same way
     ohos    HarmonyOS: device memory exported as an OH_NativeBuffer (VK_OHOS_external_memory), its BufferHandle fd
             mmap'd over the pool's guest address range as with export (Maleoon has none of the three above)
   With export / heap, registering a pool moves its contents into the dma-buf and maps the dma-buf at the same address,
   so the game keeps its pointers; removing the pool moves the contents back into ordinary shared memory. Virtual pools
   (totk_vpool.c) alias physical pool pages with a second mmap of the dma-buf (mtl_pool_alias).

   The driver is the system one, or the file named by TOTK_VK_DRIVER (a Turnip build, android/gfx/vk_load.h). */
#include <malloc.h>
#include "vk_int.h"
#include "vk_record.h"
#include "vk_framegen.h"
#include "vk_framegen_qcom.h"
#include "vk_framegen_lsfg.h"
#include "framegen_dispatch_trace.h"
#include "framegen_adaptive_defaults.h"
#include "vk_timestamp.h"
#include "vk_gpu_time.h"
#include "vk_present_policy.h"
#include "nvn_dirty.h"
#include "totk_lifelog.h"
#include "../../runtime/frame_timeline.h"
#include "../../runtime/frame_fence.h"
#include "../../runtime/present_predict.h"
#include "../../runtime/fps_auto_runtime.h"
#include "../../runtime/android_perf.h"
#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <dlfcn.h>
#include <sched.h>
#include <sys/stat.h>
#ifdef TOTK_OHOS
#include <sys/syscall.h>
#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 1U
#endif
#include <vulkan/vulkan_ohos.h>
#include <native_buffer/native_buffer.h>
#include <native_window/external_window.h>
#endif
#include <time.h>
#include <unistd.h>
/* Implemented by the adaptive frame-generation backend. */
extern void vk_fg_snapshot_pressure_trace(unsigned owner_states,unsigned generation_states,
        unsigned cancelled,int append);

#define VK_DEF(name) PFN_##name name;
VK_DEVICE_FUNCS(VK_DEF)
#undef VK_DEF
vk_dev V;
#define g_dev V.dev
static _Atomic uint64_t g_memory_live[VK_MEMORY_PURPOSES], g_memory_created[VK_MEMORY_PURPOSES];
#ifdef TOTK_OHOS
/* HarmonyOS: bytes live per purpose (the system counts the driver's memory against a kill line, memmgr GPU_LEAK_KILL at
   about 3.2 GB on this phone): handle -> size in a small open-addressed table, under its own lock. */
static _Atomic uint64_t g_memory_bytes[VK_MEMORY_PURPOSES];
static struct { VkDeviceMemory mem; uint64_t size; } g_memory_sizes[16384];
static pthread_mutex_t g_memory_sizes_mu = PTHREAD_MUTEX_INITIALIZER;
static unsigned memory_slot(VkDeviceMemory m) { return (unsigned)(((uintptr_t)m >> 4) * 2654435761u) % 16384u; }
static void memory_size_put(VkDeviceMemory m, uint64_t size) {
    pthread_mutex_lock(&g_memory_sizes_mu);
    for (unsigned i = memory_slot(m), n = 0; n < 16384; i = (i + 1) % 16384u, n++) if (!g_memory_sizes[i].mem) { g_memory_sizes[i].mem = m; g_memory_sizes[i].size = size; break; }
    pthread_mutex_unlock(&g_memory_sizes_mu);
}
static uint64_t memory_size_take(VkDeviceMemory m) {
    uint64_t size = 0;
    pthread_mutex_lock(&g_memory_sizes_mu);
    for (unsigned i = memory_slot(m), n = 0; n < 16384; i = (i + 1) % 16384u, n++) {
        if (!g_memory_sizes[i].mem) break;
        if (g_memory_sizes[i].mem == m) { size = g_memory_sizes[i].size; g_memory_sizes[i].mem = VK_NULL_HANDLE; g_memory_sizes[i].size = 0;
            /* re-insert the rest of the run so later probes still find them */
            for (unsigned j = (i + 1) % 16384u; g_memory_sizes[j].mem; j = (j + 1) % 16384u) { VkDeviceMemory mm = g_memory_sizes[j].mem; uint64_t ss = g_memory_sizes[j].size; g_memory_sizes[j].mem = VK_NULL_HANDLE;
                for (unsigned k = memory_slot(mm);; k = (k + 1) % 16384u) if (!g_memory_sizes[k].mem) { g_memory_sizes[k].mem = mm; g_memory_sizes[k].size = ss; break; } }
            break; }
    }
    pthread_mutex_unlock(&g_memory_sizes_mu);
    return size;
}
void vk_memory_bytes(uint64_t bytes[VK_MEMORY_PURPOSES]) { for (int i = 0; i < VK_MEMORY_PURPOSES; i++) bytes[i] = atomic_load_explicit(&g_memory_bytes[i], memory_order_relaxed); }
#endif
VkResult vk_memory_allocate(const VkMemoryAllocateInfo *info, VkDeviceMemory *memory, vk_memory_purpose purpose) {
    /* Pool and arena buffers carry VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT when the device has bufferDeviceAddress
       (pool_usage): their memory must then be allocated with the device-address flag. */
    VkMemoryAllocateFlagsInfo flags = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, info->pNext, VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT, 0};
    VkMemoryAllocateInfo with = *info;
    if (V.has_bda && (purpose == VK_MEMORY_POOL || purpose == VK_MEMORY_SLOT || purpose == VK_MEMORY_SNAPSHOT)) { with.pNext = &flags; info = &with; }
    VkResult result = vkAllocateMemory(V.dev, info, NULL, memory);
    if (!result) {
        atomic_fetch_add_explicit(&g_memory_live[purpose], 1, memory_order_relaxed);
        atomic_fetch_add_explicit(&g_memory_created[purpose], 1, memory_order_relaxed);
#ifdef TOTK_OHOS
        atomic_fetch_add_explicit(&g_memory_bytes[purpose], info->allocationSize, memory_order_relaxed); memory_size_put(*memory, info->allocationSize);
#endif
    }
    return result;
}
void vk_memory_release(VkDeviceMemory memory, vk_memory_purpose purpose) {
    if (!memory) return;
    vkFreeMemory(V.dev, memory, NULL);
    atomic_fetch_sub_explicit(&g_memory_live[purpose], 1, memory_order_relaxed);
#ifdef TOTK_OHOS
    atomic_fetch_sub_explicit(&g_memory_bytes[purpose], memory_size_take(memory), memory_order_relaxed);
#endif
}
void vk_memory_counts(uint64_t live[VK_MEMORY_PURPOSES]) {
    for (int i = 0; i < VK_MEMORY_PURPOSES; i++) live[i] = atomic_load_explicit(&g_memory_live[i], memory_order_relaxed);
}
static void memory_objects_report(void) {
    uint64_t n[VK_MEMORY_PURPOSES], made = 0; vk_memory_counts(n);
    for (int i = 0; i < VK_MEMORY_PURPOSES; i++) made += atomic_exchange_explicit(&g_memory_created[i], 0, memory_order_relaxed);
    LOGV("memory Vulkan objects: live pool/slot/snapshot/image-block/image-dedicated/present/readback/zero/diagnostic/overlay %llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu; %llu allocations since report (driver-internal BOs excluded)",
        (unsigned long long)n[0], (unsigned long long)n[1], (unsigned long long)n[2], (unsigned long long)n[3],
        (unsigned long long)n[4], (unsigned long long)n[5], (unsigned long long)n[6], (unsigned long long)n[7],
        (unsigned long long)n[8], (unsigned long long)n[9], (unsigned long long)made);
#ifdef TOTK_OHOS
    uint64_t b[VK_MEMORY_PURPOSES]; vk_memory_bytes(b); uint64_t total = 0; for (int i = 0; i < VK_MEMORY_PURPOSES; i++) total += b[i];
    LOGV("memory Vulkan bytes (MiB): pool %llu slot %llu snapshot %llu image-block %llu image-dedicated %llu present %llu readback %llu zero %llu diagnostic %llu overlay %llu; total %llu (the driver's own command, pipeline and descriptor memory not included)",
        (unsigned long long)(b[0] >> 20), (unsigned long long)(b[1] >> 20), (unsigned long long)(b[2] >> 20), (unsigned long long)(b[3] >> 20), (unsigned long long)(b[4] >> 20),
        (unsigned long long)(b[5] >> 20), (unsigned long long)(b[6] >> 20), (unsigned long long)(b[7] >> 20), (unsigned long long)(b[8] >> 20), (unsigned long long)(b[9] >> 20), (unsigned long long)(total >> 20));
#endif
}

/* ---------- opt-in translation measurements; all mutable state is recording-lock owned ----------
   Hash tables store complete 128-bit keys + byte/op counts, never recording/handle pointers. Hash equality is
   probabilistic byte equality, not proof of equivalent resource contents or Vulkan command-buffer reusability. */
enum { XLATE_HASH_SLOTS = 16384, XLATE_HASH_LIMIT = 8192, XLATE_HASH_BYTES = 64 << 20, XLATE_HASH_OPS = 1 << 20 };
typedef struct { uint64_t h[2], bytes, ops; } xlate_hash;
typedef struct {
    uint64_t frames, frame, submissions, recordings, draws, repeated[2], repeated_draws[2];
    uint64_t covered[2], covered_draws[2], ops, bytes, unknown_ops, incomplete, overflow[2], previous_incomplete[2];
} xlate_record_counts;
static struct {
    xlate_hash table[2][2][XLATE_HASH_SLOTS]; /* frame bank, exact/abstract, open-addressed slot */
    unsigned count[2][2], current, depth;
    int previous_overflow[2], current_overflow[2], repeated[2], covered[2];
    unsigned char deferred[6][3][128];
    xlate_record_counts counts;
} *g_xlate;
int vk_xlate_stats_enabled;
/* Exit reads only a completed published counter snapshot, never mutable streams, uniforms or hash tables.
   This separate mutex cannot recurse into Vulkan or wait for the walker/recording lock during process teardown. */
static pthread_mutex_t g_xlate_publish_mu = PTHREAD_MUTEX_INITIALIZER;
static xlate_record_counts g_xlate_published;
static void xlate_publish(void) {
    pthread_mutex_lock(&g_xlate_publish_mu);
    g_xlate_published = g_xlate->counts;
    pthread_mutex_unlock(&g_xlate_publish_mu);
}
static void xlate_report(const xlate_record_counts *s, int partial) {
    LOGV("xlate recordings: frame=%llu frames=%llu partial=%d submissions=%llu recordings=%llu repeated_exact=%llu repeated_abstract=%llu draws=%llu repeated_draws_exact=%llu repeated_draws_abstract=%llu",
        (unsigned long long)s->frame, (unsigned long long)s->frames, partial, (unsigned long long)s->submissions,
        (unsigned long long)s->recordings, (unsigned long long)s->repeated[0], (unsigned long long)s->repeated[1],
        (unsigned long long)s->draws, (unsigned long long)s->repeated_draws[0], (unsigned long long)s->repeated_draws[1]);
    LOGV("xlate recording coverage: exact=%llu abstract=%llu draws_exact=%llu draws_abstract=%llu ops=%llu bytes=%llu unknown_ops=%llu incomplete=%llu overflow_exact=%llu overflow_abstract=%llu previous_incomplete_exact=%llu previous_incomplete_abstract=%llu; limit=8192 keys/lane/frame, hashes=128-bit+bytes+ops (collisions possible; address abstraction is not semantic equivalence)",
        (unsigned long long)s->covered[0], (unsigned long long)s->covered[1],
        (unsigned long long)s->covered_draws[0], (unsigned long long)s->covered_draws[1],
        (unsigned long long)s->ops, (unsigned long long)s->bytes, (unsigned long long)s->unknown_ops,
        (unsigned long long)s->incomplete, (unsigned long long)s->overflow[0], (unsigned long long)s->overflow[1],
        (unsigned long long)s->previous_incomplete[0], (unsigned long long)s->previous_incomplete[1]);
}
static void xlate_exit(void) {
    xlate_record_counts s;
    pthread_mutex_lock(&g_xlate_publish_mu); s = g_xlate_published; pthread_mutex_unlock(&g_xlate_publish_mu);
    if (s.frames || s.submissions || s.recordings) xlate_report(&s, 1);
}
int vk_xlate_stats_on(void) {
    static int initialized;
    if (!initialized) {
        const char *e = getenv("TOTK_VK_XLATE_STATS");
        vk_xlate_stats_enabled = e && !strcmp(e, "1"); initialized = 1;
        if (vk_xlate_stats_enabled) {
            g_xlate = VK_CALLOC(1, sizeof *g_xlate);
            if (atexit(xlate_exit)) LOGV("xlate recordings: exit summary registration failed");
            LOGV("xlate recordings: enabled; previous-present-frame membership, flattened submitted op bytes including resolved deferred handles; scan limits 64 MiB/1048576 ops per recording; native resource contents/inherited state are not hashed");
        }
    }
    return vk_xlate_stats_enabled;
}
void vk_xlate_recording_stats_frame(uint64_t frame) {
    if (!vk_xlate_stats_on()) return;
    xlate_record_counts *s = &g_xlate->counts;
    s->frame = frame; s->frames++;
    g_xlate->current ^= 1;
    memset(g_xlate->table[g_xlate->current], 0, sizeof g_xlate->table[0]);
    for (unsigned lane = 0; lane < 2; lane++) {
        g_xlate->count[g_xlate->current][lane] = 0;
        g_xlate->previous_overflow[lane] = g_xlate->current_overflow[lane];
        g_xlate->current_overflow[lane] = 0;
    }
    if (s->frames == 300) { xlate_report(s, 0); memset(s, 0, sizeof *s); s->frame = frame; }
    xlate_publish();
}
int vk_xlate_deferred_slot(unsigned stage, unsigned kind, unsigned slot) {
    return g_xlate && stage < 6 && kind < 3 && slot < (kind == 1 ? 32u : 128u) && g_xlate->deferred[stage][kind][slot];
}
static void xlate_deferred_bind(unsigned op, const uint8_t *a) {
    const p_bind_handles *b = (const void *)a;
    if ((unsigned)b->stage >= 6 || b->n < 0 || b->n > 32) return;
    unsigned kind = op == OP_BIND_TEX ? 0 : op == OP_BIND_IMG ? 1 : 2;
    for (int i = 0; i < b->n; i++)
        if ((unsigned)b->first + (unsigned)i < (kind == 1 ? 32u : 128u)) g_xlate->deferred[b->stage][kind][b->first + i] = b->src != 0;
}
static void xlate_hash_bytes(xlate_hash *h, const void *bytes, size_t n) {
    const uint8_t *p = bytes;
    for (size_t i = 0; i < n; i++) {
        h->h[0] = (h->h[0] ^ p[i]) * UINT64_C(1099511628211);
        h->h[1] = (h->h[1] ^ p[i]) * UINT64_C(0x9e3779b185ebca87);
        h->h[1] = (h->h[1] << 13) | (h->h[1] >> 51);
    }
}
typedef struct { size_t offset, size; } xlate_address_field;
/* Raw records have no named payload type in nvn_obj.h; these layouts follow their memcpy writers in hle_nvn.c.
   All other offsets/sizes come directly from the shared schema, including variable arrays (never address masks). */
typedef struct { uint64_t address; uint32_t mode, value; } xlate_signal;
typedef struct { uint64_t type, address; } xlate_counter;
typedef struct { uint64_t source, destination, size; } xlate_copy;
typedef struct { uint64_t destination, size; } xlate_update;
static int xlate_address_fields(unsigned op, const uint8_t *a, size_t size, xlate_address_field fields[17]) {
    unsigned n = 0;
#define XLATE_NEED(type) do { if (size < sizeof(type)) return -1; } while (0)
#define XLATE_FIELD(type, member) do { fields[n++] = (xlate_address_field){offsetof(type, member), sizeof(((type *)0)->member)}; } while (0)
#define XLATE_ARRAY(type, member, maximum) do { \
    if (size < offsetof(type, member)) return -1; \
    const type *b = (const void *)a; \
    if (b->n < 0 || b->n > (maximum) || (size_t)b->n > (size - offsetof(type, member)) / sizeof b->member[0]) return -1; \
} while (0)
    switch (op) {
        case OP_END: case OP_BEGIN: break;
        case OP_JUMP: case OP_CALL:
            XLATE_NEED(uint64_t); fields[n++] = (xlate_address_field){0, sizeof(uint64_t)}; break;
        case OP_SIGNAL_EVENT: XLATE_NEED(xlate_signal); XLATE_FIELD(xlate_signal, address); break;
        case OP_REPORT_COUNTER: XLATE_NEED(xlate_counter); XLATE_FIELD(xlate_counter, address); break;
        case OP_COPY_B2B: XLATE_NEED(xlate_copy); XLATE_FIELD(xlate_copy, source); XLATE_FIELD(xlate_copy, destination); break;
        case OP_UPDATE_UB: {
            XLATE_NEED(xlate_update); const xlate_update *u = (const void *)a;
            if (u->size > size - sizeof *u) return -1;
            XLATE_FIELD(xlate_update, destination); break;
        }
        case OP_SET_RT:
            XLATE_NEED(p_set_rt);
            for (unsigned i = 0; i < 8; i++) fields[n++] = (xlate_address_field){
                offsetof(p_set_rt, color) + i * sizeof(((p_set_rt *)0)->color[0]), sizeof(((p_set_rt *)0)->color[0])};
            XLATE_FIELD(p_set_rt, depth); break;
        case OP_BIND_PROGRAM: XLATE_NEED(p_bind_program); XLATE_FIELD(p_bind_program, prog); break;
        case OP_BIND_VBUF: {
            XLATE_ARRAY(p_bind_vbuf, r, 16); const p_bind_vbuf *b = (const void *)a;
            for (int i = 0; i < b->n; i++) fields[n++] = (xlate_address_field){
                offsetof(p_bind_vbuf, r) + (size_t)i * sizeof b->r[0] + offsetof(NVNbufferRange, address), sizeof b->r[0].address};
            break;
        }
        case OP_BIND_UBO: case OP_BIND_SSBO: XLATE_NEED(p_bind_buf); XLATE_FIELD(p_bind_buf, addr); break;
        case OP_BIND_TEX: case OP_BIND_IMG: case OP_BIND_SAMP:
            XLATE_ARRAY(p_bind_handles, h, 32); XLATE_FIELD(p_bind_handles, src); break;
        case OP_DRAW_IDX: XLATE_NEED(p_draw_idx); XLATE_FIELD(p_draw_idx, ibuf); XLATE_FIELD(p_draw_idx, ind); break;
        case OP_DISPATCH: XLATE_NEED(p_dispatch); XLATE_FIELD(p_dispatch, caller); break;
        case OP_SET_TEX_POOL: case OP_SET_SAMP_POOL: XLATE_NEED(p_set_pool); XLATE_FIELD(p_set_pool, pool); break;
        case OP_COPY_B2T: case OP_COPY_T2B:
            XLATE_NEED(p_copy_b2t); XLATE_FIELD(p_copy_b2t, src); XLATE_FIELD(p_copy_b2t, tex); break;
        case OP_COPY_T2T: XLATE_NEED(p_copy_t2t); XLATE_FIELD(p_copy_t2t, src); XLATE_FIELD(p_copy_t2t, dst); break;
        case OP_CLEAR_TEX: XLATE_NEED(p_clear_tex); XLATE_FIELD(p_clear_tex, tex); break;
        case OP_RENDER_COND: XLATE_NEED(p_render_cond); XLATE_FIELD(p_render_cond, addr); break;
        case OP_CLEAR_COLOR: XLATE_NEED(p_clear_color); break;
        case OP_CLEAR_DS: XLATE_NEED(p_clear_ds); break;
        case OP_ALPHA_REF: XLATE_NEED(p_alpha_ref); break;
        case OP_VIEWPORTS: XLATE_ARRAY(p_viewports, v, 16); break;
        case OP_SCISSOR: XLATE_NEED(p_scissor); break;
        case OP_DEPTH_RANGE: XLATE_NEED(p_depth_range); break;
        case OP_BIND_STATE: XLATE_NEED(p_bind_state); break;
        case OP_BIND_VATTR: XLATE_ARRAY(p_bind_vattr, st, 16); break;
        case OP_BIND_VSTREAM: XLATE_ARRAY(p_bind_vstream, st, 16); break;
        case OP_STENCIL: XLATE_NEED(p_stencil); break;
        case OP_POLY_OFFSET: XLATE_NEED(p_poly_offset); break;
        case OP_DRAW: XLATE_NEED(p_draw); break;
        case OP_BARRIER: XLATE_NEED(int32_t); break;
        case OP_COPY_STRIDE: XLATE_NEED(p_copy_stride); break;
        case OP_BLEND_COLOR: XLATE_NEED(p_blend_color); break;
        case OP_RESET_COUNTER: XLATE_NEED(uint64_t); break;
        case OP_DEPTH_CLAMP: case OP_RENDER_ENABLE: case OP_RASTER_DISCARD: XLATE_NEED(p_flag); break;
        case OP_TF_BIND: {
            XLATE_NEED(p_tf_bind); const p_tf_bind *b = (const void *)a;
            for (int i = 0; i < b->n && i < 4; i++) fields[n++] = (xlate_address_field){
                offsetof(p_tf_bind, r) + (size_t)i * sizeof b->r[0] + offsetof(NVNbufferRange, address), sizeof b->r[0].address};
            break;
        }
        case OP_TF_STATE: XLATE_NEED(p_tf_state); XLATE_FIELD(p_tf_state, addr); break;
        case OP_PUSH_CONST: XLATE_NEED(p_push_const); break;   /* NVN2 push constants: bytes, no address */
        case OP_FILL_B: XLATE_NEED(p_fill_b); XLATE_FIELD(p_fill_b, dst); break;
        default: return -1;
    }
#undef XLATE_ARRAY
#undef XLATE_FIELD
#undef XLATE_NEED
    return (int)n;
}
static int xlate_hash_equal(const xlate_hash *a, const xlate_hash *b) {
    return a->h[0] == b->h[0] && a->h[1] == b->h[1] && a->bytes == b->bytes && a->ops == b->ops;
}
static unsigned xlate_hash_slot(const xlate_hash *h) {
    uint64_t x = h->h[0] ^ h->h[1] ^ h->bytes ^ h->ops;
    x ^= x >> 33; x *= UINT64_C(0xff51afd7ed558ccd); x ^= x >> 33;
    return (unsigned)x & (XLATE_HASH_SLOTS - 1);
}
static int xlate_previous(const xlate_hash *key, unsigned lane) {
    const xlate_hash *table = g_xlate->table[g_xlate->current ^ 1][lane];
    unsigned at = xlate_hash_slot(key);
    for (unsigned i = 0; i < XLATE_HASH_SLOTS; i++, at = (at + 1) & (XLATE_HASH_SLOTS - 1)) {
        if (!table[at].bytes) return 0;
        if (xlate_hash_equal(&table[at], key)) return 1;
    }
    return 0;
}
static void xlate_remember(const xlate_hash *key, unsigned lane) {
    unsigned bank = g_xlate->current, at = xlate_hash_slot(key);
    xlate_hash *table = g_xlate->table[bank][lane];
    for (unsigned i = 0; i < XLATE_HASH_SLOTS; i++, at = (at + 1) & (XLATE_HASH_SLOTS - 1)) {
        if (table[at].bytes) { if (xlate_hash_equal(&table[at], key)) return; continue; }
        if (g_xlate->count[bank][lane] < XLATE_HASH_LIMIT) {
            table[at] = *key; g_xlate->count[bank][lane]++; return;
        }
        break;
    }
    g_xlate->counts.overflow[lane]++; g_xlate->current_overflow[lane] = 1;
}
static void xlate_recording(const uint8_t *p) {
    xlate_hash hashes[2] = {
        {.h = {UINT64_C(14695981039346656037), UINT64_C(0x6a09e667f3bcc909)}},
        {.h = {UINT64_C(14695981039346656037), UINT64_C(0x6a09e667f3bcc909)}}};
    int complete = 0, abstract = 1;
    xlate_record_counts *s = &g_xlate->counts; s->recordings++;
    for (;;) {
        op_hdr h; memcpy(&h, p, sizeof h);
        if (h.size < sizeof h || (h.size & 7) || h.size > XLATE_HASH_BYTES - hashes[0].bytes ||
            hashes[0].ops == XLATE_HASH_OPS || h.op >= OP_COUNT_) break;
        xlate_hash_bytes(&hashes[0], p, h.size);
        xlate_address_field fields[17]; int n = xlate_address_fields(h.op, p + sizeof h, h.size - sizeof h, fields);
        if (n < 0) { s->unknown_ops++; abstract = 0; }
        else if (abstract) {
            size_t cursor = 0;
            for (int i = 0; i < n; i++) {
                size_t at = sizeof h + fields[i].offset;
                xlate_hash_bytes(&hashes[1], p + cursor, at - cursor);
                uint64_t value = 0; memcpy(&value, p + at, fields[i].size);
                /* Nullness is semantic (notably immediate versus deferred); retain it, erase only the address. */
                value = value != 0; xlate_hash_bytes(&hashes[1], &value, fields[i].size);
                cursor = at + fields[i].size;
            }
            xlate_hash_bytes(&hashes[1], p + cursor, h.size - cursor);
        }
        for (unsigned lane = 0; lane < 2; lane++) { hashes[lane].bytes += h.size; hashes[lane].ops++; }
        if (h.op == OP_END) { complete = 1; break; }
        /* EndRecording flattens jumps and Call/CopyCommands inline their bodies. A residual control edge has
           unknown flattened coverage; replay still counts its actual draws, but neither hash is admitted. */
        if (h.op == OP_JUMP || h.op == OP_CALL) break;
        p += h.size;
    }
    s->ops += hashes[0].ops; s->bytes += hashes[0].bytes; s->incomplete += !complete;
    for (unsigned lane = 0; lane < 2; lane++) {
        g_xlate->covered[lane] = complete && (!lane || abstract);
        g_xlate->repeated[lane] = 0;
        if (!g_xlate->covered[lane]) continue;
        s->covered[lane]++;
        int repeat = xlate_previous(&hashes[lane], lane);
        g_xlate->repeated[lane] = repeat; s->repeated[lane] += repeat;
        s->previous_incomplete[lane] += !repeat && g_xlate->previous_overflow[lane];
        xlate_remember(&hashes[lane], lane);
    }
}

/* ---------- CPU recording split; all aggregates are protected by vk_rec_lock ----------
   CLOCK_MONOTONIC uses Android's vDSO when the kernel permits its counter fast path. Do not assume
   CNTVCT_EL0 is safe on every vendor kernel: arm_arch_timer may trap it for counter errata. */
int vk_rec_profile_on, vk_rec_timing_on;
static __thread struct {
    int active, category, weight, timeline;
    unsigned rate;
    uint64_t last, coarse_start;
} T_rec;
static unsigned g_rec_sample_rate = 8;
static uint64_t g_rec_sample_seq[OP_COUNT_];
static struct {
    uint64_t ns[VK_REC_N], count[VK_REC_N], reads, frames, draws, attempts, passes, hist[6], max_draws, ops, submissions, cpu_steps;
    uint64_t pass_ns, largest_ns, max_pass_ns;
    double max_pass_share;
    uint64_t ub_allocated, ub_reused;
    uint64_t coarse_ns, sampled_draws, weighted_reads;
    uint64_t snapshot_bindings, snapshot_unique, snapshot_direct, snapshot_staged;
    uint64_t touch_calls, touch_no_updates, touch_ranges, touch_resolve_avoided, touch_negative_hits, pointer_hits, pointer_misses;
} g_rec;
static struct { int open; uint64_t draws, ns; } g_rec_pass;
static uint64_t g_rec_frame_largest;
static double g_rec_probe_ns;
static uint64_t rec_now(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}
static int rec_sequential(int category) { return category == VK_REC_S || (category >= VK_REC_S_STREAM && category <= VK_REC_S_TOUCH); }
static int rec_other(int category) { return category == VK_REC_O || category >= VK_REC_O_SLOT; }
static uint64_t rec_category_ns(int other) {
    uint64_t ns = g_rec.ns[other ? VK_REC_O : VK_REC_S];
    for (int i = other ? VK_REC_O_SLOT : VK_REC_S_STREAM; i < (other ? VK_REC_N : VK_REC_O_SLOT); i++) ns += g_rec.ns[i];
    return ns;
}
static void rec_switch_weight(int category, int weight) {
    if (!T_rec.weight && !weight) { T_rec.category = category; return; }
    uint64_t now = rec_now();
    uint64_t ns = (now - T_rec.last) * (unsigned)T_rec.weight;
    if (vk_rec_profile_on) {
        g_rec.ns[T_rec.category] += ns;
        g_rec.weighted_reads += (unsigned)T_rec.weight;
        g_rec.reads++;
    }
    if (T_rec.timeline) {
        enum totk_ft_counter counter = rec_sequential(T_rec.category) ? FT_COUNT_REC_S_NS :
            rec_other(T_rec.category) ? FT_COUNT_REC_O_NS : FT_COUNT_REC_P_NS;
        totk_ft_count(counter, ns);
        (void)counter;
    }
    T_rec.last = now; T_rec.category = category; T_rec.weight = weight;
}
static void rec_switch(int category) { rec_switch_weight(category, T_rec.weight); }
int vk_rec_profile_push(int category) {
    if (!T_rec.active || (!T_rec.weight && (!vk_rec_profile_on || !rec_other(category)))) return -1;
    int previous = T_rec.category, weight = vk_rec_profile_on && rec_other(category) ? 1 : T_rec.weight;
    if (category == VK_REC_RESOLVE && (rec_sequential(previous) || rec_other(previous))) return -1;
    if (vk_rec_profile_on) g_rec.count[category] += (unsigned)weight;
    if (previous == category && T_rec.weight == weight) return -1;
    int token = previous | T_rec.weight << 8;
    rec_switch_weight(category, weight); return token;
}
void vk_rec_profile_pop(int previous) {
    if (previous >= 0) rec_switch_weight(previous & 255, previous >> 8);
}
static int rec_stream_begin(void) {
    if (!T_rec.active || T_rec.rate == 1) return -1;
    int token = T_rec.category | T_rec.weight << 8;
    rec_switch_weight(VK_REC_S_STREAM, 0); return token;
}
static void rec_sample_begin(unsigned op) {
    if (!T_rec.active) return;
    if (T_rec.rate == 1) { if (T_rec.timeline) totk_ft_count(FT_COUNT_REC_SAMPLES, 1); return; }
    if (op == OP_DRAW_IDX) op = OP_DRAW; /* sample graphics attempts together, not one index type's phase */
    uint64_t seq = ++g_rec_sample_seq[op], hash = seq * 0x9e3779b97f4a7c15ull;
    hash ^= hash >> 32;
    if (!(hash & (T_rec.rate - 1))) {
        rec_switch_weight(VK_REC_S_STREAM, (int)T_rec.rate);
        if (vk_rec_profile_on) g_rec.count[VK_REC_S_STREAM] += T_rec.rate;
        if (T_rec.timeline) totk_ft_count(FT_COUNT_REC_SAMPLES, 1);
    }
}
static void rec_sample_end(void) { if (T_rec.active && T_rec.rate != 1) rec_switch_weight(VK_REC_S_STREAM, 0); }
static uint64_t rec_parallel_ns(void) {
    uint64_t n = 0; for (int i = VK_REC_RESOLVE; i < VK_REC_O; i++) n += g_rec.ns[i]; return n;
}
void vk_rec_profile_begin(int category) {
    T_rec.timeline = 0; /* calibration is never walk work */
    if (!g_rec_probe_ns) {
        /* Calibrate the actual timer + exclusive-accounting path on this recording CPU, once.
           This is an estimate, not an overhead subtraction or a device benchmark. */
        T_rec.active = T_rec.weight = 1; T_rec.category = VK_REC_S; T_rec.last = rec_now();
        uint64_t t0 = rec_now();
        for (int i = 0; i < 4096; i++) { int prev = vk_rec_profile_push(VK_REC_PROG); vk_rec_profile_pop(prev); }
        g_rec_probe_ns = (double)(rec_now() - t0) / 8192.0;
        memset(&g_rec, 0, sizeof g_rec);
        LOGV("rec profile: CLOCK_MONOTONIC, %.1f ns/transition; opcode/draw sampling 1/%u (TOTK_VK_REC_PROFILE_RATE); O/lifecycle unsampled", g_rec_probe_ns, g_rec_sample_rate);
    }
    T_rec.active = T_rec.weight = 1; T_rec.category = category; T_rec.last = T_rec.coarse_start = rec_now();
    T_rec.rate = g_rec_sample_rate; T_rec.timeline = totk_ft_work_frame() != 0;
    g_rec.reads++; g_rec.count[category]++;
    if (rec_sequential(category)) g_rec.submissions++;
}
void vk_rec_profile_end(void) {
    if (!T_rec.active) return;
    rec_switch(T_rec.category);
    if (vk_rec_profile_on) g_rec.coarse_ns += T_rec.last - T_rec.coarse_start;
    T_rec.active = T_rec.weight = T_rec.timeline = 0;
}
/* Timeline-only mode reuses the exclusive scopes without enabling the full profiler, calibration or logs.
   Opcode S/P/O intervals are sampled at 1/64 and weighted 64x. Outside mtl_exec the lifecycle is exact,
   at weight one; unselected opcode scopes take no clock reads, including O. */
static void rec_timeline_begin(void) {
    T_rec.active = T_rec.weight = T_rec.timeline = 1;
    T_rec.rate = 64; T_rec.category = VK_REC_S_STREAM; T_rec.last = rec_now();
}
#ifdef TOTK_VK_TESTS
void vk_record_test_profile_ns(uint64_t *out, size_t count) {
    if (count > VK_REC_N) count = VK_REC_N;
    memcpy(out, g_rec.ns, count * sizeof *out);
}
static uint64_t g_lag_test_cache[VK_LAG_TEST_N];
void vk_record_test_lag_cache_stats(uint64_t *out, size_t count) {
    if (count > VK_LAG_TEST_N) count = VK_LAG_TEST_N;
    memcpy(out, g_lag_test_cache, count * sizeof *out);
}
#define LAG_TEST_INC(field) (g_lag_test_cache[field]++)
#else
#define LAG_TEST_INC(field) ((void)0)
#endif
uint64_t vk_rec_profile_draw_begin(void) {
    if (!T_rec.active) return UINT64_MAX;
    g_rec.attempts++;
    if (!T_rec.weight) return UINT64_MAX;
    g_rec.sampled_draws++; return rec_parallel_ns();
}
void vk_rec_profile_draw_end(uint64_t p0, int recorded) {
    if (!T_rec.active) return;
    if (recorded) g_rec.draws++;
    if (g_rec_pass.open) {
        g_rec_pass.draws += recorded;
        if (p0 != UINT64_MAX) g_rec_pass.ns += rec_parallel_ns() - p0;
    }
}
void vk_rec_profile_pass_begin(void) {
    if (T_rec.active) { g_rec_pass.open = 1; g_rec_pass.draws = g_rec_pass.ns = 0; }
}
static void rec_pass_finish(void) {
    if (!g_rec_pass.open) return;
    uint64_t d = g_rec_pass.draws, ns = g_rec_pass.ns;
    int bin = !d ? 0 : d < 16 ? 1 : d < 64 ? 2 : d < 256 ? 3 : d < 1024 ? 4 : 5;
    g_rec.passes++; g_rec.hist[bin]++; g_rec.pass_ns += ns;
    if (d > g_rec.max_draws) g_rec.max_draws = d;
    if (ns > g_rec_frame_largest) g_rec_frame_largest = ns;
    if (ns > g_rec.max_pass_ns) g_rec.max_pass_ns = ns;
    g_rec_pass.open = 0;
}
void vk_rec_profile_pass_end(void) { if (T_rec.active) rec_pass_finish(); }
void vk_rec_profile_present(uint64_t frame) {
    /* A rare pass crossing a present is accounted as frame-local slices, without changing rendering. */
    int open = g_rec_pass.open;
    rec_pass_finish();
    if (open) { g_rec_pass.open = 1; g_rec_pass.draws = g_rec_pass.ns = 0; }
    static uint64_t prev_p;
    uint64_t p = rec_parallel_ns(), frame_p = p - prev_p; prev_p = p;
    double share = frame_p ? (double)g_rec_frame_largest / frame_p : 0;
    if (share > g_rec.max_pass_share) g_rec.max_pass_share = share;
    g_rec.largest_ns += g_rec_frame_largest; g_rec_frame_largest = 0; g_rec.frames++;
    if (frame % 300) return;
    double f = (double)g_rec.frames, ms = 1e-6 / f, s = (double)rec_category_ns(0), o = (double)rec_category_ns(1);
    double total = s + p, estimate = g_rec.reads * g_rec_probe_ns;
    uint64_t sc = g_rec.count[VK_REC_S], oc = g_rec.count[VK_REC_O];
    for (int i = VK_REC_S_STREAM; i < VK_REC_O_SLOT; i++) sc += g_rec.count[i];
    for (int i = VK_REC_O_SLOT; i < VK_REC_N; i++) oc += g_rec.count[i];
    LOGV("rec profile frame %llu, ms/frame: S %.3f P %.3f [resolve %.3f bind/desc %.3f prog/variant %.3f pipeline %.3f vertex/index %.3f cmd %.3f] O %.3f; D=not-separated; draws %.1f/%.1f attempted, passes %.1f; ideal 2/3/4 %.2fx/%.2fx/%.2fx",
         (unsigned long long)frame, s * ms, p * ms, g_rec.ns[VK_REC_RESOLVE] * ms, g_rec.ns[VK_REC_BIND] * ms,
         g_rec.ns[VK_REC_PROG] * ms, g_rec.ns[VK_REC_PIPE] * ms, g_rec.ns[VK_REC_VERTEX] * ms, g_rec.ns[VK_REC_CMD] * ms,
         o * ms, g_rec.draws / f, g_rec.attempts / f, g_rec.passes / f,
         total ? total / (s + p / 2.0) : 1, total ? total / (s + p / 3.0) : 1, total ? total / (s + p / 4.0) : 1);
    LOGV("rec profile granularity: passes/frame draws=0/1-15/16-63/64-255/256-1023/1024+ %.2f/%.2f/%.2f/%.2f/%.2f/%.2f; max draws/pass %llu; largest pass P %.3f ms/frame (%.1f%% of P, max frame share %.1f%%), max pass P %.3f ms; pass-attributed %.1f%% of P",
         g_rec.hist[0] / f, g_rec.hist[1] / f, g_rec.hist[2] / f, g_rec.hist[3] / f, g_rec.hist[4] / f, g_rec.hist[5] / f,
         (unsigned long long)g_rec.max_draws, g_rec.largest_ns * ms, p ? 100.0 * g_rec.largest_ns / p : 0,
         100 * g_rec.max_pass_share, g_rec.max_pass_ns / 1e6, p ? 100.0 * g_rec.pass_ns / p : 0);
    LOGV("rec profile scopes/frame S/resolve/bind/prog/pipeline/vertex/cmd/O %.1f/%.1f/%.1f/%.1f/%.1f/%.1f/%.1f/%.1f; probes %.1f/draw, estimated overhead %.1f ns/draw (%.3f ms/frame, %.1f%%); O-fixed ideal 2/3/4 %.2fx/%.2fx/%.2fx",
         sc / f, g_rec.count[1] / f, g_rec.count[2] / f, g_rec.count[3] / f,
         g_rec.count[4] / f, g_rec.count[5] / f, g_rec.count[6] / f, oc / f,
         g_rec.attempts ? (double)g_rec.reads / g_rec.attempts : 0, g_rec.attempts ? estimate / g_rec.attempts : 0,
         estimate * ms, total + o ? 100 * estimate / (total + o) : 0,
         total + o ? (total + o) / (s + o + p / 2.0) : 1, total + o ? (total + o) / (s + o + p / 3.0) : 1,
         total + o ? (total + o) / (s + o + p / 4.0) : 1);
    LOGV("rec profile work/frame: %.1f ops, %.1f submissions, %.1f CPU steps",
         g_rec.ops / f, g_rec.submissions / f, g_rec.cpu_steps / f);
    LOGV("rec profile S ms/frame: stream %.3f state %.3f uniform-maintenance %.3f counters %.3f pass %.3f snapshots %.3f GPU-write %.3f resources %.3f draw-touch %.3f residual %.3f",
        g_rec.ns[VK_REC_S_STREAM] * ms, g_rec.ns[VK_REC_S_STATE] * ms, g_rec.ns[VK_REC_S_UNIFORM] * ms,
        g_rec.ns[VK_REC_S_COUNTER] * ms, g_rec.ns[VK_REC_S_PASS] * ms, g_rec.ns[VK_REC_S_SNAPSHOT] * ms,
        g_rec.ns[VK_REC_S_WRITE] * ms, g_rec.ns[VK_REC_S_RESOURCE] * ms, g_rec.ns[VK_REC_S_TOUCH] * ms, g_rec.ns[VK_REC_S] * ms);
    LOGV("rec profile O ms/frame: slot %.3f reset %.3f primary %.3f submit %.3f queue %.3f GPU-wait %.3f worker-wait %.3f execute %.3f assembly %.3f dispatch %.3f transfer %.3f upload %.3f present %.3f residual %.3f",
        g_rec.ns[VK_REC_O_SLOT] * ms, g_rec.ns[VK_REC_O_RESET] * ms, g_rec.ns[VK_REC_O_PRIMARY] * ms,
        g_rec.ns[VK_REC_O_SUBMIT] * ms, g_rec.ns[VK_REC_O_QUEUE] * ms, g_rec.ns[VK_REC_O_GPU_WAIT] * ms,
        g_rec.ns[VK_REC_O_WORKER_WAIT] * ms, g_rec.ns[VK_REC_O_EXECUTE] * ms, g_rec.ns[VK_REC_O_ASSEMBLE] * ms,
        g_rec.ns[VK_REC_O_DISPATCH] * ms, g_rec.ns[VK_REC_O_TRANSFER] * ms, g_rec.ns[VK_REC_O_UPLOAD] * ms,
        g_rec.ns[VK_REC_O_PRESENT] * ms, g_rec.ns[VK_REC_O] * ms);
    LOGV("rec profile uniform update payloads/frame: allocated %.1f reused %.1f", g_rec.ub_allocated / f, g_rec.ub_reused / f);
    LOGV("rec profile sampling: 1/%u ops/draws, %.1f/%.1f draws sampled; category ms are weighted estimates; coarse measured %.3f ms/frame; actual/weighted probe estimates %.3f/%.3f ms/frame",
        g_rec_sample_rate, g_rec.sampled_draws / f, g_rec.attempts / f, g_rec.coarse_ns * ms,
        estimate * ms, g_rec.weighted_reads * g_rec_probe_ns * ms);
    LOGV("rec profile snapshots/frame: binds %.1f unique candidates %.1f; direct/staged %.3f/%.3f KiB (async captures own eager bytes)",
        g_rec.snapshot_bindings / f, g_rec.snapshot_unique / f, g_rec.snapshot_direct / (1024 * f), g_rec.snapshot_staged / (1024 * f));
    LOGV("rec profile draw-touch/frame: calls/no-updates %.1f/%.1f, storage ranges/resolve-avoided %.1f/%.1f, negative-range hits %.1f, frozen pointer hit/miss %.1f/%.1f",
        g_rec.touch_calls / f, g_rec.touch_no_updates / f, g_rec.touch_ranges / f, g_rec.touch_resolve_avoided / f,
        g_rec.touch_negative_hits / f, g_rec.pointer_hits / f, g_rec.pointer_misses / f);
    memset(&g_rec, 0, sizeof g_rec); prev_p = 0;
}
/* ---------- device ---------- */
static PFN_vkGetInstanceProcAddr gipa;
static PFN_vkGetCalibratedTimestampsEXT g_calibrated_timestamps;
static int g_gpu_time_hardware;
#ifdef TOTK_OHOS
static _Thread_local int g_is_walker;   /* this thread walks the command streams (totk_thread_walk) */
#endif
static int g_gpu_time_fps_median;
static void ts_clock_init(void);
enum { POOLS_HOST = 1, POOLS_EXPORT, POOLS_HEAP, POOLS_OHOS };
static int g_pool_mode; static const char *g_pool_mode_name;
static VkDeviceSize g_host_align = 4096;
static int g_lookup_stats;   /* immutable after vk_init_once: one predictable branch per lookup when disabled */
static _Atomic uintptr_t g_lookup_thread;
static PFN_vkGetPhysicalDeviceExternalBufferProperties vkGetPhysicalDeviceExternalBufferProperties;

static VkExtensionProperties *g_ext; static uint32_t g_next;
static int has_ext(const char *n) { for (uint32_t i = 0; i < g_next; i++) if (!strcmp(g_ext[i].extensionName, n)) return 1; return 0; }

int vk_pick_type(uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < V.mp.memoryTypeCount; i++) if ((bits >> i & 1) && (V.mp.memoryTypes[i].propertyFlags & want) == want) return (int)i;
    return -1;
}
static int pick_host_type(uint32_t bits) {   /* CPU reads guest memory constantly: cached + coherent first */
    int t = vk_pick_type(bits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
    if (t < 0) t = vk_pick_type(bits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (t < 0) t = vk_pick_type(bits, 0);
    return t;
}

#define POOL_USAGE (POOL_USAGE_BASE | (V.has_bda ? VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT : 0))   /* Switch 2: device addresses (vk_gmem_map) */
static const VkBufferUsageFlags POOL_USAGE_BASE = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
    VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT;

static void *completion_thread(void *);
static void *gpu_guard_thread(void *);
static int lag_wait_on(void);
static int g_lag_wait = -1;   /* initialized on the walk before any recorder worker is dispatched */
_Atomic int vk_lag_capture_state = -1; /* -1 uninitialized, 0 off/cut over, 1 protected capture */
static void lag_pressure_fallback(void); /* walking owner, only between NVN streams */
static void lag_guard_mark(uint64_t addr, uint64_t size, unsigned reason);
/* In-flight check (TOTK_OHOS_INFLIGHT_CHECK=K, "in-flight check" below): K, immutable after vk_init_once. 0: each hook
   is one predictable branch and nothing is recorded. */
__attribute__((visibility("hidden"))) int g_ic_k;   /* vk_int.h: vk_draw.c and vk_res.c test it before their every-frame records; hidden: read directly, no GOT load */
__attribute__((visibility("hidden"))) int g_pt_on;   /* oot-a122 TOTK_OHOS_PROTECT (vk_int.h; "two frames in flight: bounded protection") */
static void pt_parse(void);
static void pt_gpu_wrote(uint64_t a, uint64_t n);
static _Atomic uint64_t g_ic_map_gen;   /* odd while lazy_back_units moves pages under a registered pool; hashes from before cannot be compared */
static void ic_parse(void);
static void ic_pool_barrier(void);
static void ic_census(void);
static void ic_gml_mark(void);
static void ic_slot_open(unsigned slot);
static void ic_slot_complete(unsigned slot);
static void ic_frame(uint64_t frame);

/* ---------- pipeline cache on disk ----------
   Driver/context-matched opaque blobs are loaded at device creation. vk_pcache.c owns shared validation,
   quarantine and coalesced, quiet-aware persistence with retained scratch; no save IO/cache snapshot runs
   on a game/recording/UI thread. TOTK_VK_PIPELINE_CACHE defaults to state/vk-pipeline-cache.bin; "0" disables it.
   TOTK_VK_PIPELINE_CACHE_IMPORT (default vk-pipeline-cache.bin in the game folder): a compatible context
   merged once at start and renamed .imported. Both paths accept the documented empty-debug legacy exception. */
/* The game's threads run with its FPCR (Ocarina of Time: 0x1000000, flush-to-zero), and every host call it makes, the
   replay included, ran with it; so did the threads made from them and the driver's shader compiler, which runs in this
   process. Swap2-Native (mamcjr/Swap2-Native, MIT; docs/reference/rendering.md "Pipelines compiled with the game's
   floating-point environment on the Vulkan replay") found its Qualcomm pipelines compiled so rendered Ocarina of Time's
   main pass wrong (overexposed) from the same SPIR-V; here the materials wrote NaN / 0 (a black world, pd-a47). Device
   creation, shader module and pipeline creation (vk_host_fp_wrap), and the replay (hle_nvn.c submit_exec, present) run
   with FPCR 0 and give the caller's back. The pipeline cache file is a new one: binaries compiled the old way stay out. */
uint64_t vk_fpcr_enter(void) { uint64_t v; __asm__ volatile("mrs %0, fpcr" : "=r"(v)); if (v) __asm__ volatile("msr fpcr, %0" :: "r"((uint64_t)0)); return v; }
void vk_fpcr_leave(uint64_t v) { if (v) __asm__ volatile("msr fpcr, %0" :: "r"(v)); }
static PFN_vkCreateShaderModule g_real_csm; static PFN_vkCreateGraphicsPipelines g_real_cgp; static PFN_vkCreateComputePipelines g_real_ccp;
static VKAPI_ATTR VkResult VKAPI_CALL fp_csm(VkDevice d, const VkShaderModuleCreateInfo *ci, const VkAllocationCallbacks *a, VkShaderModule *m) {
    uint64_t fp = vk_fpcr_enter(); VkResult r = g_real_csm(d, ci, a, m); vk_fpcr_leave(fp); return r;
}
static VKAPI_ATTR VkResult VKAPI_CALL fp_cgp(VkDevice d, VkPipelineCache c, uint32_t n, const VkGraphicsPipelineCreateInfo *ci, const VkAllocationCallbacks *a, VkPipeline *p) {
    uint64_t fp = vk_fpcr_enter(); VkResult r = g_real_cgp(d, c, n, ci, a, p); vk_fpcr_leave(fp); return r;
}
static VKAPI_ATTR VkResult VKAPI_CALL fp_ccp(VkDevice d, VkPipelineCache c, uint32_t n, const VkComputePipelineCreateInfo *ci, const VkAllocationCallbacks *a, VkPipeline *p) {
    uint64_t fp = vk_fpcr_enter(); VkResult r = g_real_ccp(d, c, n, ci, a, p); vk_fpcr_leave(fp); return r;
}
static void vk_host_fp_wrap(void) {
    if (getenv("TOTK_OHOS_HOST_FP") && !strcmp(getenv("TOTK_OHOS_HOST_FP"), "0")) { LOGV("TOTK_OHOS_HOST_FP=0: pipelines compile with the caller's FPCR"); return; }
    g_real_csm = vkCreateShaderModule; vkCreateShaderModule = fp_csm;
    g_real_cgp = vkCreateGraphicsPipelines; vkCreateGraphicsPipelines = fp_cgp;
    g_real_ccp = vkCreateComputePipelines; vkCreateComputePipelines = fp_ccp;
    uint64_t v; __asm__ volatile("mrs %0, fpcr" : "=r"(v));
    LOGV("shader modules and pipelines compile with FPCR 0 (this thread's FPCR %#llx; TOTK_OHOS_HOST_FP=0 keeps the caller's)", (unsigned long long)v);
}
static const char *pcache_path(void) {
    static char path[1100]; static const char *result; static int init;
    if (!init) {
        const char *p = getenv("TOTK_VK_PIPELINE_CACHE");
        result = vk_shader_cache_path(p && *p ? p : "state/vk-pipeline-cache-fp0.bin", path, sizeof path);
        init = 1;
    }
    return result;
}
static int export_ok(VkExternalMemoryHandleTypeFlagBits ht, int *dedicated) {
    VkPhysicalDeviceExternalBufferInfo q = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO, .usage = POOL_USAGE, .handleType = ht};
    VkExternalBufferProperties ep = {VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES};
    vkGetPhysicalDeviceExternalBufferProperties(V.pd, &q, &ep);
    VkExternalMemoryFeatureFlags f = ep.externalMemoryProperties.externalMemoryFeatures;
    if (dedicated) *dedicated = (f & VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT) != 0;
    return (f & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT) != 0;
}
static int g_export_dedicated;
static int heap_open(void);

/* Device features: everything the driver has, except robust buffer access (costly; NVN has none) -- the translated
   shaders use what Ryujinx's Vulkan output needs (storage image formats, clip distances, int64, ...). */
static VkPhysicalDeviceCustomBorderColorFeaturesEXT f_cbc = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT};
static VkPhysicalDeviceRobustness2FeaturesEXT f_rob2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT};
static VkPhysicalDeviceIndexTypeUint8FeaturesEXT f_u8 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INDEX_TYPE_UINT8_FEATURES_EXT};
static VkPhysicalDeviceDepthClipControlFeaturesEXT f_dcc = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_CONTROL_FEATURES_EXT};
static VkPhysicalDeviceVertexAttributeDivisorFeaturesEXT f_div = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_EXT};
static VkPhysicalDeviceVulkan13Features f13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
static VkPhysicalDeviceVulkan12Features f12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
static VkPhysicalDeviceVulkan11Features f11 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
static VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT f_gpl = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT};
static VkPhysicalDeviceFeatures2 f2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
static int g_occl_on;   /* fixed at device creation: precise occlusion + host query reset, explicitly opted in */
#ifdef TOTK_OHOS
#include "vk_selftest.inc"
#endif

static VkFormat depth_choice(VkFormat want, VkFormat fallback) {
    VkFormatProperties fp; V.get_format_props(V.pd, want, &fp);
    return (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) && (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) ? want : fallback;
}

static void vk_init_once(void) {
    vk_draw_switches_init();   /* before any compile worker or shader load reads them */
    vk_upload_threads_init();
    vk_tex_memo_init();
    (void)vk_shaders_ready();   /* headless startup too; the app already froze selection against its game folder */
    { const char *fps60 = getenv("TOTK_FPS60");
      LOGV("frame policy: FPS60=%d FRAME_LAG=%d CHUNK_DRAWS=%ld",
           fps60 && strcmp(fps60, "0") != 0, runtime_frame_lag_requested(), vk_frame_chunk_limit()); }
    { const char *e = getenv("TOTK_VK_HITCH"); g_hitch_on = e && atoi(e) > 0; }
    { const char *e = getenv("TOTK_VK_LOOKUP_STATS"); g_lookup_stats = e && atoi(e); }
    ic_parse();   /* TOTK_OHOS_INFLIGHT_CHECK, before the completion thread and any pool exist */
    pt_parse();   /* oot-a122 TOTK_OHOS_PROTECT, before any submission */
    { const char *e = getenv("TOTK_VK_REC_PROFILE"); vk_rec_profile_on = e && !strcmp(e, "1"); }
    vk_rec_timing_on = vk_rec_profile_on || totk_ft_on();
    const char *gpl_env = getenv("TOTK_VK_PIPELINE_LIBRARY");
    int gpl_requested = gpl_env && !strcmp(gpl_env, "1");
    V.has_pipeline_library = 0;
    V.pipeline_library_fast_linking = V.pipeline_library_independent_interpolation = 0;
    { const char *e = getenv("TOTK_VK_REC_PROFILE_RATE"); long requested = e ? strtol(e, NULL, 10) : 8;
      if (requested < 1) requested = 1; if (requested > 64) requested = 64;
      g_rec_sample_rate = 1; while (g_rec_sample_rate < requested) g_rec_sample_rate <<= 1; }
    const char *kind, *why, *drv = getenv("TOTK_VK_DRIVER");
    if (drv && (!*drv || !strcmp(drv, "system"))) drv = NULL;
    gipa = vk_load_driver(drv, &kind, &why);
    if (!gipa) DIE("Vulkan driver %s: %s", drv ? drv : "(system)", why);
    PFN_vkCreateInstance vkCreateInstance = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "totk-native", .apiVersion = VK_API_VERSION_1_3};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
    VKCHECK(vkCreateInstance(&ici, NULL, &V.inst));
#define IL(name) PFN_##name name = (PFN_##name)gipa(V.inst, #name)
    IL(vkEnumeratePhysicalDevices); IL(vkCreateDevice); IL(vkGetDeviceProcAddr); IL(vkEnumerateDeviceExtensionProperties);
    IL(vkGetPhysicalDeviceProperties2); IL(vkGetPhysicalDeviceFeatures2); IL(vkGetPhysicalDeviceMemoryProperties);
    IL(vkGetPhysicalDeviceQueueFamilyProperties);
#undef IL
    vkGetPhysicalDeviceExternalBufferProperties = (PFN_vkGetPhysicalDeviceExternalBufferProperties)gipa(V.inst, "vkGetPhysicalDeviceExternalBufferProperties");
    V.get_format_props = (PFN_vkGetPhysicalDeviceFormatProperties)gipa(V.inst, "vkGetPhysicalDeviceFormatProperties");
    V.get_image_format_props = (PFN_vkGetPhysicalDeviceImageFormatProperties)gipa(V.inst, "vkGetPhysicalDeviceImageFormatProperties");
    uint32_t n = 1; VkResult r = vkEnumeratePhysicalDevices(V.inst, &n, &V.pd);
    if ((r && r != VK_INCOMPLETE) || !n) DIE("no Vulkan device");
    VkPhysicalDeviceExternalMemoryHostPropertiesEXT hostp = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT};
    VkPhysicalDeviceDriverProperties drvp = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES, &hostp};
    V.subgroup = (VkPhysicalDeviceSubgroupProperties){VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
    V.subgroup_sizes = (VkPhysicalDeviceSubgroupSizeControlProperties){VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_PROPERTIES};
    hostp.pNext = &V.subgroup; V.subgroup.pNext = &V.subgroup_sizes;
    VkPhysicalDeviceProperties2 p2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &drvp};
    vkGetPhysicalDeviceProperties2(V.pd, &p2);
    V.props = p2.properties;
    if (V.props.apiVersion < VK_API_VERSION_1_3) DIE("%s offers Vulkan %u.%u; 1.3 is needed", V.props.deviceName, VK_API_VERSION_MAJOR(V.props.apiVersion), VK_API_VERSION_MINOR(V.props.apiVersion));
    LOGV("device %s, driver %s %s (%s%s%s)", V.props.deviceName, drvp.driverName, drvp.driverInfo, kind, drv ? " " : "", drv ? drv : "");
    vkGetPhysicalDeviceMemoryProperties(V.pd, &V.mp);
    vkEnumerateDeviceExtensionProperties(V.pd, NULL, &g_next, NULL);
    g_ext = VK_CALLOC(g_next, sizeof *g_ext);
    vkEnumerateDeviceExtensionProperties(V.pd, NULL, &g_next, g_ext);
#ifdef TOTK_OHOS
    {   /* the device's whole extension list, once (oot-a105): what the driver offers beyond what this file asks for */
        char line[900]; size_t o = 0; unsigned first = 0;
        LOGV("device API %u.%u.%u, %u device extensions:", VK_API_VERSION_MAJOR(V.props.apiVersion), VK_API_VERSION_MINOR(V.props.apiVersion), VK_API_VERSION_PATCH(V.props.apiVersion), g_next);
        for (uint32_t i = 0; i < g_next; i++) {
            size_t n = strlen(g_ext[i].extensionName);
            if (o + n + 8 > sizeof line) { LOGV("  ext %u-%u:%s", first, i - 1, line); o = 0; first = i; }
            o += (size_t)snprintf(line + o, sizeof line - o, " %s/%u", g_ext[i].extensionName, g_ext[i].specVersion);
        }
        if (o) LOGV("  ext %u-%u:%s", first, g_next - 1, line);
    }
#endif
    int gpl_extensions = has_ext("VK_EXT_graphics_pipeline_library") && has_ext("VK_KHR_pipeline_library");
    if (gpl_extensions) {
        VkPhysicalDeviceGraphicsPipelineLibraryPropertiesEXT gplp = {
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_PROPERTIES_EXT
        };
        VkPhysicalDeviceProperties2 gplp2 = {
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &gplp
        };
        vkGetPhysicalDeviceProperties2(V.pd, &gplp2);
        V.pipeline_library_fast_linking = gplp.graphicsPipelineLibraryFastLinking;
        V.pipeline_library_independent_interpolation = gplp.graphicsPipelineLibraryIndependentInterpolationDecoration;
    }

#if defined(__ANDROID__) && defined(TOTK_ANDROID_APP)
    /* Import the AHB's dma-buf with an explicit layout, not an AHB memory handle: Turnip's deferred AHB path
       discards caller tiling/modifier and replaces them with gralloc metadata. No loader swapchain is needed. */
    const char *present = getenv("TOTK_VK_PRESENT");
    int want_ahb = !present || !*present || !strcmp(present, "ahb");
    V.ahb_present = want_ahb && has_ext("VK_EXT_image_drm_format_modifier") &&
        has_ext("VK_KHR_external_memory_fd") && has_ext("VK_EXT_external_memory_dma_buf") && has_ext("VK_EXT_queue_family_foreign");
    if (want_ahb && !V.ahb_present) LOGV("window present: explicit AHB layout unavailable (modifier %d, fd %d, dma-buf %d, foreign queue %d); using copy",
        has_ext("VK_EXT_image_drm_format_modifier"), has_ext("VK_KHR_external_memory_fd"),
        has_ext("VK_EXT_external_memory_dma_buf"), has_ext("VK_EXT_queue_family_foreign"));
    else if (present && *present && strcmp(present, "copy") && !want_ahb) LOGV("window present: unknown TOTK_VK_PRESENT=%s; using copy", present);
    if (!V.ahb_present && !want_ahb) LOGV("window present: copy (Android default ahb; explicit copy override)");
#endif
    uint32_t qn = 0; vkGetPhysicalDeviceQueueFamilyProperties(V.pd, &qn, NULL);
    VkQueueFamilyProperties *qf = VK_CALLOC(qn, sizeof *qf); vkGetPhysicalDeviceQueueFamilyProperties(V.pd, &qn, qf);
    V.qfam = UINT32_MAX;
    for (uint32_t i = 0; i < qn; i++) if ((qf[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) == (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) { V.qfam = i; break; }
    V.timestamp_bits = V.qfam < qn ? qf[V.qfam].timestampValidBits : 0;
    free(qf);
    if (V.qfam == UINT32_MAX) DIE("no graphics + compute queue");
    if (vk_fg_qcom_requested()) {
        /* Report only. A float queue priority is device-relative, not KGSL
           global priority. Do not enable features/extensions or alter qci. */
        int khr=has_ext("VK_KHR_global_priority"), ext=has_ext("VK_EXT_global_priority");
        int query_ext=has_ext("VK_EXT_global_priority_query");
        VkPhysicalDeviceGlobalPriorityQueryFeaturesKHR priority_query={
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GLOBAL_PRIORITY_QUERY_FEATURES_KHR};
        if (khr || query_ext) {
            VkPhysicalDeviceFeatures2 query={VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,.pNext=&priority_query};
            vkGetPhysicalDeviceFeatures2(V.pd,&query);
        }
        int low=-1,medium=-1,high=-1,realtime=-1; uint32_t advertised=0;
        PFN_vkGetPhysicalDeviceQueueFamilyProperties2 properties2=
            (PFN_vkGetPhysicalDeviceQueueFamilyProperties2)gipa(V.inst,"vkGetPhysicalDeviceQueueFamilyProperties2");
        if (priority_query.globalPriorityQuery && properties2) {
            VkQueueFamilyGlobalPriorityPropertiesKHR priorities[qn];
            VkQueueFamilyProperties2 properties[qn];
            for (uint32_t i=0;i<qn;i++) {
                priorities[i]=(VkQueueFamilyGlobalPriorityPropertiesKHR){VK_STRUCTURE_TYPE_QUEUE_FAMILY_GLOBAL_PRIORITY_PROPERTIES_KHR};
                properties[i]=(VkQueueFamilyProperties2){VK_STRUCTURE_TYPE_QUEUE_FAMILY_PROPERTIES_2,.pNext=&priorities[i]};
            }
            uint32_t count=qn; properties2(V.pd,&count,properties);
            if (V.qfam<count) {
                const VkQueueFamilyGlobalPriorityPropertiesKHR *p=&priorities[V.qfam];
                advertised=p->priorityCount;
                if (advertised && advertised<=VK_MAX_GLOBAL_PRIORITY_SIZE_KHR) {
                    low=medium=high=realtime=0;
                    for (uint32_t i=0;i<advertised;i++) {
                        low|=p->priorities[i]==VK_QUEUE_GLOBAL_PRIORITY_LOW_KHR;
                        medium|=p->priorities[i]==VK_QUEUE_GLOBAL_PRIORITY_MEDIUM_KHR;
                        high|=p->priorities[i]==VK_QUEUE_GLOBAL_PRIORITY_HIGH_KHR;
                        realtime|=p->priorities[i]==VK_QUEUE_GLOBAL_PRIORITY_REALTIME_KHR;
                    }
                }
            }
        }
        LOGV("framegen qcom Vulkan priority: KHR_global_priority=%d EXT_global_priority=%d EXT_global_priority_query=%d query-feature=%u family=%u advertised-count=%u LOW/MEDIUM/HIGH/REALTIME=%d/%d/%d/%d (-1=unqueried/invalid); report-only, render queue unchanged: no global request (API default MEDIUM), device-relative priority=1.0; changing global priority requires queue/device creation and app permission is untested",
            khr,ext,query_ext,priority_query.globalPriorityQuery,V.qfam,advertised,low,medium,high,realtime);
    }

    /* Pool path. */
    const char *want = getenv("TOTK_VK_POOLS");
    int can_host = has_ext("VK_EXT_external_memory_host");
    int can_export = has_ext("VK_KHR_external_memory_fd") && has_ext("VK_EXT_external_memory_dma_buf") && export_ok(VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, &g_export_dedicated);
    int can_heap = has_ext("VK_KHR_external_memory_fd") && has_ext("VK_EXT_external_memory_dma_buf") && heap_open() >= 0;
    int can_ohos = 0;
#ifdef TOTK_OHOS
    can_ohos = has_ext("VK_OHOS_external_memory");
    if (can_ohos) { int dedicated = 0; int exportable = export_ok(VK_EXTERNAL_MEMORY_HANDLE_TYPE_OHOS_NATIVE_BUFFER_BIT_OHOS, &dedicated);
        LOGV("OH_NativeBuffer export: exportable %d, dedicated-only %d (allocations are dedicated either way)", exportable, dedicated); }
#endif
    if (want && !strcmp(want, "host")) { if (!can_host) DIE("TOTK_VK_POOLS=host: the driver has no VK_EXT_external_memory_host"); g_pool_mode = POOLS_HOST; }
    else if (want && !strcmp(want, "export")) { if (!can_export) DIE("TOTK_VK_POOLS=export: the driver cannot export dma-bufs"); g_pool_mode = POOLS_EXPORT; }
    else if (want && !strcmp(want, "heap")) { if (!can_heap) DIE("TOTK_VK_POOLS=heap: no dma-buf heap or no dma-buf import"); g_pool_mode = POOLS_HEAP; }
    else if (want && !strcmp(want, "ohos")) { if (!can_ohos) DIE("TOTK_VK_POOLS=ohos: no VK_OHOS_external_memory"); g_pool_mode = POOLS_OHOS; }
    else g_pool_mode = can_host ? POOLS_HOST : can_export ? POOLS_EXPORT : can_heap ? POOLS_HEAP : can_ohos ? POOLS_OHOS : 0;
    if (!g_pool_mode) DIE("this driver cannot share memory with the game (no host import, dma-buf export or dma-buf heap import): try a Turnip driver with TOTK_VK_DRIVER");
    g_pool_mode_name = g_pool_mode == POOLS_HOST ? "host-pointer import" : g_pool_mode == POOLS_EXPORT ? "dma-buf export mapped at the guest address" : g_pool_mode == POOLS_OHOS ? "OH_NativeBuffer export mapped at the guest address" : "dma-buf heap mapped at the guest address";
    if (g_pool_mode == POOLS_HOST) g_host_align = hostp.minImportedHostPointerAlignment ? hostp.minImportedHostPointerAlignment : 4096;

    /* Features: query the whole chain the backend knows, then enable what is there. */
    void *chain = NULL;
#define LINK(s, ext) if (!(ext) || has_ext(ext)) { (s).pNext = chain; chain = &(s); }
    LINK(f_cbc, "VK_EXT_custom_border_color") LINK(f_rob2, "VK_EXT_robustness2") LINK(f_u8, "VK_EXT_index_type_uint8")
    LINK(f_dcc, "VK_EXT_depth_clip_control") LINK(f_div, "VK_EXT_vertex_attribute_divisor")
    if (gpl_extensions) { f_gpl.pNext = chain; chain = &f_gpl; }
    LINK(f13, NULL) LINK(f12, NULL) LINK(f11, NULL)
#undef LINK
    f2.pNext = chain;
    vkGetPhysicalDeviceFeatures2(V.pd, &f2);
    LOGV("shader stores: vertex/geometry %d, fragment %d; draw parameters %d, geometry shader %d, point size in geometry %d; ranges uniform %u storage %u",
         f2.features.vertexPipelineStoresAndAtomics, f2.features.fragmentStoresAndAtomics, f11.shaderDrawParameters,
         f2.features.geometryShader, f2.features.shaderTessellationAndGeometryPointSize,
         V.props.limits.maxUniformBufferRange, V.props.limits.maxStorageBufferRange);
    const char *subgroup_fix = getenv("TOTK_VK_SUBGROUP_FIX");
    V.subgroup_fix = subgroup_fix && !strcmp(subgroup_fix, "1");
    V.subgroup_control = V.subgroup_fix && f13.subgroupSizeControl;
    f13.subgroupSizeControl = V.subgroup_control;
    f13.computeFullSubgroups = VK_FALSE;
    LOGV("subgroup fix: %d (startup-only), native %u, range %u-%u, control %d, required stages 0x%x",
        V.subgroup_fix, V.subgroup.subgroupSize, V.subgroup_sizes.minSubgroupSize,
        V.subgroup_sizes.maxSubgroupSize, V.subgroup_control, V.subgroup_sizes.requiredSubgroupSizeStages);
    int gpl_supported = gpl_extensions && f_gpl.graphicsPipelineLibrary;
    V.has_pipeline_library = gpl_requested && gpl_supported;
    f_gpl.graphicsPipelineLibrary = V.has_pipeline_library;
    if (gpl_extensions && !V.has_pipeline_library) f13.pNext = f_gpl.pNext; /* query only: no opt-in feature structure at device creation */
    LOGV("graphics pipeline library: requested %d, supported %d, enabled %d, fast-linking %u, independent-interpolation %u",
        gpl_requested, gpl_supported, V.has_pipeline_library,
        V.pipeline_library_fast_linking, V.pipeline_library_independent_interpolation);
    if (!f13.synchronization2 || !f13.dynamicRendering) DIE("the driver lacks synchronization2 or dynamic rendering");
#ifdef TOTK_OHOS
    /* Maleoon resets the GPU (device lost) within a second of the first real frames, where Adreno tolerates the same
       work; out-of-bounds reads are the usual cause on such GPUs. Keep the robustness the driver offers unless
       TOTK_VK_ROBUST=0 (the reference turns it all off for speed). */
    const char *robust_env = getenv("TOTK_VK_ROBUST");
    int robust = !(robust_env && !strcmp(robust_env, "0"));
    LOGV("robust access: %s (buffer %d, image %d, buffer2 %d, image2 %d offered)", robust ? "on" : "off (TOTK_VK_ROBUST=0)",
         f2.features.robustBufferAccess, f13.robustImageAccess, f_rob2.robustBufferAccess2, f_rob2.robustImageAccess2);
    /* TOTK_VK_ROBUST=1 (oot-a109, a measurement): the core robustness only (robustBufferAccess, robustImageAccess), not the
       stricter robustBufferAccess2 / robustImageAccess2 (exact-range zeros, null descriptors are a separate feature) */
    if (robust && robust_env && !strcmp(robust_env, "1")) {
        f_rob2.robustBufferAccess2 = VK_FALSE; f_rob2.robustImageAccess2 = VK_FALSE;
        LOGV("robust access: core only (TOTK_VK_ROBUST=1): robustBufferAccess2 and robustImageAccess2 off");
    }
    if (!robust)
#endif
    {
    f2.features.robustBufferAccess = VK_FALSE; f_rob2.robustBufferAccess2 = VK_FALSE; f_rob2.robustImageAccess2 = VK_FALSE;
    f13.robustImageAccess = VK_FALSE;
    }
    f12.bufferDeviceAddressCaptureReplay = VK_FALSE; f12.bufferDeviceAddressMultiDevice = VK_FALSE;
#ifdef GAME_NS2
    V.has_bda = f12.bufferDeviceAddress;
    /* A Switch 2 game's shaders reach memory through loaded addresses (the translator's memory pool table: physical storage
       buffer pointers) and draw with task / mesh shaders: what this GPU offers for them (docs/DESIGN-OOT.md). */
    LOGV("switch 2 needs: bufferDeviceAddress %d, VK_EXT_mesh_shader %d, VK_KHR_cooperative_matrix %d, shaderInt64 %d, shaderFloat16 %d, storageBuffer8BitAccess %d",
         f12.bufferDeviceAddress, has_ext("VK_EXT_mesh_shader"), has_ext("VK_KHR_cooperative_matrix"), f2.features.shaderInt64, f12.shaderFloat16, f12.storageBuffer8BitAccess);
    /* Pool-indexed (bindless) textures: the game's whole texture and sampler pools as descriptor arrays */
    LOGV("descriptor indexing: runtime arrays %d, partially bound %d, variable count %d, sampled image update after bind %d, sampled image array non-uniform indexing %d, update unused while pending %d; stores and atomics vertex %d fragment %d",
         f12.runtimeDescriptorArray, f12.descriptorBindingPartiallyBound, f12.descriptorBindingVariableDescriptorCount, f12.descriptorBindingSampledImageUpdateAfterBind,
         f12.shaderSampledImageArrayNonUniformIndexing, f12.descriptorBindingUpdateUnusedWhilePending, f2.features.vertexPipelineStoresAndAtomics, f2.features.fragmentStoresAndAtomics);
#endif
    f_cbc.customBorderColorWithoutFormat = f_cbc.customBorderColorWithoutFormat && f_cbc.customBorderColors;
    V.has_custom_border = has_ext("VK_EXT_custom_border_color") && f_cbc.customBorderColors && f_cbc.customBorderColorWithoutFormat;
    V.has_null_descriptor = has_ext("VK_EXT_robustness2") && f_rob2.nullDescriptor;
#ifdef TOTK_OHOS
    {   /* Maleoon advertises nullDescriptor, but a draw reading a null view logs "Null descriptor: ImageView or bufferView
           is null handle" and the GPU resets about a second later (device lost). Empty slots get the 1x1 zero
           resources instead (vk_res.c vk_dummy_view / vk_dummy_tbuf), the path for drivers without the feature.
           TOTK_VK_NULL_DESCRIPTOR=1 keeps the driver's null descriptors (diagnostics). */
        const char *e = getenv("TOTK_VK_NULL_DESCRIPTOR");
        if (!(e && !strcmp(e, "1"))) { V.has_null_descriptor = 0; f_rob2.nullDescriptor = VK_FALSE; }
    }
#endif
    V.has_index_u8 = has_ext("VK_EXT_index_type_uint8") && f_u8.indexTypeUint8;
    V.has_depth_clip_control = has_ext("VK_EXT_depth_clip_control") && f_dcc.depthClipControl;
    V.has_divisor = has_ext("VK_EXT_vertex_attribute_divisor") && f_div.vertexAttributeInstanceRateDivisor;
    V.has_bc = f2.features.textureCompressionBC; V.has_geometry = f2.features.geometryShader;
#ifdef TOTK_OHOS
    { extern void vk_fmt_set_bc(int has); vk_fmt_set_bc(V.has_bc); }   /* without BC: decoded on the CPU (vk_fmt.c) */
    {   extern void vk_fmt_set_e5(int has); VkFormatProperties fp; V.get_format_props(V.pd, VK_FORMAT_E5B9G9R9_UFLOAT_PACK32, &fp);
        VkFormatFeatureFlags need = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
        int has = (fp.optimalTilingFeatures & need) == need;
        vk_fmt_set_e5(has); LOGV("BC6H_UFLOAT textures: decoded to %s (TOTK_OHOS_BC6_E5)", has ? "E5B9G9R9, 4 bytes a texel" : "RGBA16F, 8 bytes a texel (E5B9G9R9 is not sampled and filtered here)"); }
    { extern void vk_fmt_set_etc(int has); vk_fmt_set_etc(f2.features.textureCompressionETC2);
      LOGV("BC textures: %s (BC %d, ETC2 %d)", V.has_bc ? "native" : f2.features.textureCompressionETC2 ? "colour formats transcoded to ETC2 at upload, BC4/BC5/BC6H decoded" : "decoded to uncompressed formats", V.has_bc, f2.features.textureCompressionETC2); }
#endif
    V.has_layer_output = f12.shaderOutputLayer; V.has_logic_op = f2.features.logicOp; V.has_dual_src = f2.features.dualSrcBlend;
    LOGV("layer output from vertex shaders: shaderOutputLayer %d, shaderOutputViewportIndex %d, VK_EXT_shader_viewport_index_layer %d, geometry shaders %d",
         f12.shaderOutputLayer, f12.shaderOutputViewportIndex, has_ext("VK_EXT_shader_viewport_index_layer"), f2.features.geometryShader);
    V.has_cache_control = f13.pipelineCreationCacheControl;
    V.has_pipeline_statistics = f2.features.pipelineStatisticsQuery;
    V.has_host_query_reset = f12.hostQueryReset;
    V.has_image_read_without_format = f2.features.shaderStorageImageReadWithoutFormat;
    V.has_image_write_without_format = f2.features.shaderStorageImageWriteWithoutFormat;
    V.samples = V.props.limits.framebufferColorSampleCounts & V.props.limits.framebufferDepthSampleCounts;

    static const char *want_ext[] = {"VK_EXT_external_memory_host", "VK_KHR_external_memory_fd", "VK_EXT_external_memory_dma_buf",
        "VK_EXT_custom_border_color", "VK_EXT_robustness2", "VK_EXT_index_type_uint8", "VK_EXT_depth_clip_control",
        "VK_EXT_vertex_attribute_divisor", "VK_EXT_shader_stencil_export", "VK_EXT_shader_viewport_index_layer",
        "VK_KHR_shader_float_controls", "VK_EXT_provoking_vertex", "VK_EXT_primitive_topology_list_restart"
#ifdef TOTK_OHOS
        , "VK_OHOS_external_memory"
#endif
        };
    const char *exts[32]; uint32_t ne = 0;
    for (unsigned i = 0; i < sizeof want_ext / sizeof *want_ext; i++) {
        if (!has_ext(want_ext[i])) continue;
        if (!strcmp(want_ext[i], "VK_EXT_external_memory_host") && g_pool_mode != POOLS_HOST) continue;
        exts[ne++] = want_ext[i];
    }
    if (V.has_pipeline_library) {
        exts[ne++] = "VK_EXT_graphics_pipeline_library";
        exts[ne++] = "VK_KHR_pipeline_library";
    }
    int calibrated = 0;
    int broken_device_clock = 0;
#ifdef __ANDROID__
    /* Turnip's KGSL device_get_gpu_timestamp is UNREACHABLE/return-success
       without writing the timestamp, despite advertising this extension. */
    broken_device_clock = drvp.driverID == VK_DRIVER_ID_MESA_TURNIP;
#endif
    if (broken_device_clock)
        LOGV("[fps-policy] GPU timestamp clock: bypassing unsupported Turnip/KGSL DEVICE calibration");
    if (!broken_device_clock && has_ext("VK_EXT_calibrated_timestamps")) {
        PFN_vkGetPhysicalDeviceCalibrateableTimeDomainsEXT domains =
            (PFN_vkGetPhysicalDeviceCalibrateableTimeDomainsEXT)gipa(V.inst, "vkGetPhysicalDeviceCalibrateableTimeDomainsEXT");
        uint32_t count = 0;
        if (domains && domains(V.pd, &count, NULL) == VK_SUCCESS && count) {
            VkTimeDomainEXT *available = VK_CALLOC(count, sizeof *available);
            if (domains(V.pd, &count, available) == VK_SUCCESS) {
                int device = 0, monotonic = 0;
                for (uint32_t i = 0; i < count; i++) {
                    device |= available[i] == VK_TIME_DOMAIN_DEVICE_EXT;
                    monotonic |= available[i] == VK_TIME_DOMAIN_CLOCK_MONOTONIC_EXT;
                }
                calibrated = device && monotonic;
            }
            free(available);
        }
        if (calibrated) exts[ne++] = "VK_EXT_calibrated_timestamps";
    }
#if defined(__ANDROID__) && defined(TOTK_ANDROID_APP)
    if (V.ahb_present) { exts[ne++] = "VK_EXT_image_drm_format_modifier"; exts[ne++] = "VK_EXT_queue_family_foreign"; }
    int qcom_interop=vk_fg_qcom_requested() &&
        has_ext("VK_ANDROID_external_memory_android_hardware_buffer") &&
        has_ext("VK_KHR_external_semaphore_fd") && has_ext("VK_EXT_queue_family_foreign");
    const char *ubwc=getenv("TOTK_PRESENT_UBWC");
    int ubwc_qualified=vk_present_ubwc_driver_known(drvp.driverInfo);
    int present_ahb_import=V.ahb_present && ubwc && !strcmp(ubwc,"1") && ubwc_qualified &&
        has_ext("VK_ANDROID_external_memory_android_hardware_buffer");
    if (present_ahb_import && !qcom_interop)
        exts[ne++]="VK_ANDROID_external_memory_android_hardware_buffer";
    if (qcom_interop) {
        exts[ne++]="VK_ANDROID_external_memory_android_hardware_buffer";
        exts[ne++]="VK_KHR_external_semaphore_fd";
        if (!V.ahb_present) exts[ne++]="VK_EXT_queue_family_foreign";
    }
#endif
#ifdef TOTK_OHOS
    /* Maleoon: enabling protectedMemory turns the driver's I/O coherency off ("InitEnabledFeatures protectedMemory is
       true, close io coherency"), which leaves no cached + coherent host memory type: the guest's pools (command
       memory, uniforms, everything the CPU reads) became uncached. Nothing here uses protected memory. */
    f11.protectedMemory = VK_FALSE;
#endif
#if defined(TOTK_OHOS) && defined(TOTK_GAME_OOT)
    {   /* oot-a121 TOTK_OHOS_PUSHDESC (vk_int.h): bits 1 / 2 need VK_KHR_push_descriptor and its maxPushDescriptors */
        const char *e = getenv("TOTK_OHOS_PUSHDESC"), *fg = getenv("TOTK_FRAMEGEN");
        /* default 1 since oot-a123 (fragment sets pushed: a122b, two pairs, +3.5% / +7.5% fps, walker -12-17% us a draw,
           0 bad frames in 1200 recorded frames); TOTK_OHOS_PUSHDESC=0 turns it off */
        int want = e && *e ? atoi(e) & 7 : 1;
        if (want && fg && *fg && strcmp(fg, "0")) {
            LOGV("descriptor push (oot-a121): TOTK_OHOS_PUSHDESC=%d ignored: frame generation (TOTK_FRAMEGEN=%s) replays recorded descriptor sets", want, fg);
            want = 0;
        }
        if ((want & 3) == 3) want &= ~2;
        if (want & 3) {
            VkPhysicalDevicePushDescriptorPropertiesKHR pdp = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PUSH_DESCRIPTOR_PROPERTIES_KHR};
            VkPhysicalDeviceProperties2 pp2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &pdp};
            if (has_ext("VK_KHR_push_descriptor") && ne < sizeof exts / sizeof *exts) {
                vkGetPhysicalDeviceProperties2(V.pd, &pp2); vk_push_max = pdp.maxPushDescriptors;
                if (vk_push_max) exts[ne++] = "VK_KHR_push_descriptor";
            }
            if (!vk_push_max) { LOGV("descriptor push (oot-a121): no VK_KHR_push_descriptor here; sets stay allocated and bound"); want &= 4; }
        }
        vk_push_bits = want;
    }
#endif
    float prio = 1;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = V.qfam, .queueCount = 1, .pQueuePriorities = &prio};
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &f2, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci, .enabledExtensionCount = ne, .ppEnabledExtensionNames = exts};
    { uint64_t fp = vk_fpcr_enter(); VKCHECK(vkCreateDevice(V.pd, &dci, NULL, &V.dev)); vk_fpcr_leave(fp); }   /* the driver's own threads start with FPCR 0 */
#ifdef TOTK_OHOS
    { char mt[256] = ""; size_t o = 0; for (uint32_t i = 0; i < V.mp.memoryTypeCount && o < sizeof mt - 16; i++) o += (size_t)snprintf(mt + o, sizeof mt - o, " %u:%#x", i, V.mp.memoryTypes[i].propertyFlags);
      LOGV("memory types (index:flags):%s", mt); }
#endif
#define DL(name) name = (PFN_##name)vkGetDeviceProcAddr(V.dev, #name);
    VK_DEVICE_FUNCS(DL)
    if (vk_push_bits & 3) {   /* oot-a121 TOTK_OHOS_PUSHDESC */
        vk_cmd_push_template = (PFN_vkCmdPushDescriptorSetWithTemplateKHR)vkGetDeviceProcAddr(V.dev, "vkCmdPushDescriptorSetWithTemplateKHR");
        if (vk_cmd_push_template) vk_push_stage = vk_push_bits & 1 ? 1 : 0;
        else { vk_push_bits &= 4; LOGV("descriptor push (oot-a121): no vkCmdPushDescriptorSetWithTemplateKHR; sets stay allocated and bound"); }
    }
    if (vk_push_bits)
        LOGV("descriptor push (oot-a121): TOTK_OHOS_PUSHDESC=%d: %s; the primary path %s; maxPushDescriptors %u", vk_push_bits,
             vk_push_stage == 1 ? "fragment shaders' sets (set 1) are pushed" : vk_push_stage == 0 ? "vertex shaders' sets (set 0) are pushed" : "no set is pushed",
             vk_push_bits & 4 ? "keeps sets bound under the same layout (bit 4)" : "binds as before", vk_push_max);
    vk_host_fp_wrap();   /* the calls that compile run with the host's floating-point environment */
#ifdef TOTK_OHOS
    { extern void vk_gpu_trace_install(void); vk_gpu_trace_install(); }   /* TOTK_GPU_TRACE=1: native-harmonyos/ohos/vk_gpu_trace.c */
#endif
#undef DL
#if defined(__ANDROID__) && defined(TOTK_ANDROID_APP)
    vk_fg_qcom_device(gipa,vkGetDeviceProcAddr,qcom_interop);
    vk_fg_lsfg_device(gipa,vkGetDeviceProcAddr,qcom_interop);
#else
    vk_fg_qcom_device(gipa,vkGetDeviceProcAddr,0);
    vk_fg_lsfg_device(gipa,vkGetDeviceProcAddr,0);
#endif
    if (calibrated) g_calibrated_timestamps =
        (PFN_vkGetCalibratedTimestampsEXT)vkGetDeviceProcAddr(V.dev, "vkGetCalibratedTimestampsEXT");
#if defined(__ANDROID__) && defined(TOTK_ANDROID_APP)
    if (V.ahb_present) {
        V.get_image_format_props2 = (PFN_vkGetPhysicalDeviceImageFormatProperties2)gipa(V.inst, "vkGetPhysicalDeviceImageFormatProperties2");
        V.get_image_modifier = (PFN_vkGetImageDrmFormatModifierPropertiesEXT)vkGetDeviceProcAddr(V.dev, "vkGetImageDrmFormatModifierPropertiesEXT");
        if (present_ahb_import) V.get_ahb_props=(PFN_vkGetAndroidHardwareBufferPropertiesANDROID)
            vkGetDeviceProcAddr(V.dev,"vkGetAndroidHardwareBufferPropertiesANDROID");
        if (!V.get_image_format_props2 || !V.get_image_modifier || !vkGetMemoryFdPropertiesKHR) {
            V.ahb_present = 0; LOGV("window present: missing explicit dma-buf import entry points; using copy");
        } else LOGV("window present: ahb (explicit LINEAR dma-buf import, modifier=0; device measurement pending)");
    }
    if (ubwc && !strcmp(ubwc,"1")) {
        LOGV("[present-ubwc] driverInfo=\"%s\" qualified=%d; AHB import API %s; nonidentity root hint required",
            drvp.driverInfo,ubwc_qualified,V.get_ahb_props ? "enabled" : "unavailable");
        if (!ubwc_qualified) LOGV("[present-ubwc] unknown driverInfo: retaining explicit LINEAR presentation");
        else if (!V.get_ahb_props) LOGV("[present-ubwc] import capabilities unavailable: retaining explicit LINEAR presentation");
    }
#endif
    { static int requested = -1; if (requested < 0) { const char *e = getenv("TOTK_VK_OCCLUSION"); requested = e && atoi(e) != 0; }
      g_occl_on = requested && f12.hostQueryReset && f2.features.occlusionQueryPrecise;
      if (requested) LOGV("[occl] TOTK_VK_OCCLUSION: %s (hostQueryReset %u, occlusionQueryPrecise %u); pending sample conditions stay visible",
                         g_occl_on ? "on" : "off: required features missing", f12.hostQueryReset, f2.features.occlusionQueryPrecise); }
    vkGetDeviceQueue(V.dev, V.qfam, 0, &V.queue);
#ifdef TOTK_OHOS
    vk_layer_selftest();   /* ZA_LAYER_TEST=1 (vk_selftest.inc) */
#endif
    int gpu_time_default_hardware = 0;
#if defined(__ANDROID__) && !defined(GAME_BOTW)
    const char *fps60 = getenv("TOTK_FPS60");
    gpu_time_default_hardware = !fps60 || !strcmp(fps60, "0");
#endif
    const char *gpu_time = getenv("TOTK_GPU_TIME");
    int gpu_time_explicit = gpu_time && *gpu_time;
    g_gpu_time_hardware = gpu_time_explicit ? !strcmp(gpu_time, "hardware") : gpu_time_default_hardware;
    LOGV("[fps-policy] GPU report time: %s (%s)",
         g_gpu_time_hardware ? "hardware" : "latency",
         !gpu_time_explicit ? (gpu_time_default_hardware ?
             "default Android 30-fps; TOTK_GPU_TIME=latency opts out" :
             "default latency; TOTK_GPU_TIME=hardware opts in") :
         g_gpu_time_hardware || !strcmp(gpu_time, "latency") ? "explicit" :
             "unknown value; retaining latency");
    g_gpu_time_fps_median = g_gpu_time_hardware &&
        vk_gpu_time_fps_median_enabled(getenv("TOTK_GPU_TIME_FPS_MEDIAN"));
    LOGV("[fps-policy] TOTK_GPU_TIME_FPS_MEDIAN=%d: FPS END consumer only, median of three unique valid reports; hardware required",
         g_gpu_time_fps_median);
    ts_clock_init();
    size_t pc_size; void *pc_data = vk_pcache_load(pcache_path(), &pc_size, 1);
    VkPipelineCacheCreateInfo pcc = {VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO, .initialDataSize = pc_size, .pInitialData = pc_data};
    VKCHECK(vkCreatePipelineCache(V.dev, &pcc, NULL, &V.pcache));
    free(pc_data);
    { const char *base = getenv("TOTK_VK_PIPELINE_CACHE_IMPORT"); if (!base || !*base) base = "vk-pipeline-cache.bin";
      char import_path[1100]; const char *ip = vk_shader_cache_path(base, import_path, sizeof import_path);
      size_t isz = 0; void *idata = vk_pcache_load(ip, &isz, 0); VkPipelineCache ic;
      VkPipelineCacheCreateInfo icc = {VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO, .initialDataSize = isz, .pInitialData = idata};
      if (idata && !vkCreatePipelineCache(V.dev, &icc, NULL, &ic)) {
          VkResult r = vkMergePipelineCaches(V.dev, V.pcache, 1, &ic); vkDestroyPipelineCache(V.dev, ic, NULL);
          if (!r) vk_pcache_changed();
          char done[1100]; snprintf(done, sizeof done, "%s.imported", ip);
          if (!r && !rename(ip, done)) LOGV("pipeline cache: merged %zu KiB from %s (now %s)", isz >> 10, ip, done);
          else RED(4, "pipeline cache: importing %s: merge %d, rename %s", ip, r, strerror(errno));
      }
      free(idata); }
    vk_pcache_start(pcache_path(), pc_size);
    vk_fmt_set_depth(depth_choice(VK_FORMAT_X8_D24_UNORM_PACK32, VK_FORMAT_D32_SFLOAT), depth_choice(VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT));
    vk_post_init();   /* optional internal pipelines are ready before game recording starts */
    if (lag_wait_on()) {
        nvn_dirty_lag_enable();
        pthread_t guard; if (pthread_create(&guard, NULL, gpu_guard_thread, NULL)) DIE("lag GPU-fence thread");
        pthread_detach(guard);
    }
    pthread_t t; if (pthread_create(&t, NULL, completion_thread, NULL)) DIE("completion thread");
    pthread_detach(t);
    vk_compute_start();
    vk_pipeline_prewarm();
    LOGV("memory pools: %s; %u extensions, BC %s, custom border colours %s, 8-bit indices %s, null descriptors %s", g_pool_mode_name, ne,
         V.has_bc ? "yes" : "no", V.has_custom_border ? "yes" : "no", V.has_index_u8 ? "yes" : "no", V.has_null_descriptor ? "yes" : "no");
    {   const VkPhysicalDeviceLimits *l = &V.props.limits;   /* the texture arrays of NVN2 shaders (pool-indexed, 32 elements) meet these */
        LOGV("descriptor limits: per stage samplers %u, sampled images %u, storage images %u, uniform %u, storage buffers %u, resources %u; per set samplers %u, sampled images %u, storage images %u; bound sets %u",
             l->maxPerStageDescriptorSamplers, l->maxPerStageDescriptorSampledImages, l->maxPerStageDescriptorStorageImages, l->maxPerStageDescriptorUniformBuffers,
             l->maxPerStageDescriptorStorageBuffers, l->maxPerStageResources, l->maxDescriptorSetSamplers, l->maxDescriptorSetSampledImages, l->maxDescriptorSetStorageImages, l->maxBoundDescriptorSets);
    }
}
static pthread_once_t g_once = PTHREAD_ONCE_INIT;
void mtl_init(void) { pthread_once(&g_once, vk_init_once); }

/* ---------- dma-buf heap ---------- */
struct dma_heap_alloc { uint64_t len; uint32_t fd, fd_flags; uint64_t heap_flags; };
#define DMA_HEAP_IOCTL_ALLOC _IOWR('H', 0x0, struct dma_heap_alloc)
static int g_heap_fd = -1;
static int heap_open(void) {
    static const char *heaps[] = {"/dev/dma_heap/system", "/dev/dma_heap/qcom,system"};
    for (unsigned i = 0; i < 2 && g_heap_fd < 0; i++) g_heap_fd = open(heaps[i], O_RDONLY | O_CLOEXEC);
    return g_heap_fd;
}

/* ---------- memory pools ----------
   kind OWN: the pool has its own device memory (and, for export / heap, the dma-buf mapped at its address).
   kind VIEW: the pool lies inside another pool's pages (pools may be carved from a bigger pool): same buffer, offset.
   kind VIRT: a virtual pool; addresses resolve through the alias table (physical pool pages mapped into it).
   kind CPU: no GPU backing (partial overlap with another pool): GPU-ordered work on it runs on the CPU in order.
   kind LAZY: a unit of a big pool that gets its device memory when the GPU first uses it (lazy_back, TOTK_OHOS_LAZY_MB). */
enum { K_OWN = 1, K_VIEW, K_VIRT, K_CPU, K_LAZY };
typedef struct pool_ent {
    uintptr_t lo, hi, base, end;    /* registered range; page-rounded range */
    int kind, fd;
    VkBuffer buf; VkDeviceMemory mem;
    uint64_t identity;             /* registration lifetime, not a guest pool object's reusable address */
    struct pool_ent *owner; uint64_t off;   /* VIEW: owner pool, offset of base in owner's buffer */
    struct pool_ent *chain;        /* the next chunk of a pool registered in parts (add_range: above POOL_CHUNK) */
    uintptr_t group_lo; uint64_t group_size;   /* a chunk's whole registered pool (0: not a chunk) */
#ifdef TOTK_OHOS
    void *nb;                      /* OH_NativeBuffer the pool memory was imported from (ohos_pool_import) */
    int join_next;                 /* LAZY: a game buffer runs from this unit into the next (mtl_buffer_init): both get GPU memory together */
    int flags, was_lazy;           /* the memory report: the pool's NVN flags (-1: not known), and whether its GPU memory came at first use */
#endif
} pool_ent;
typedef struct { uintptr_t va, end; pool_ent *owner; uint64_t off; } alias_ent;   /* virtual pool pages -> owner's buffer offset */
static pool_ent **g_pools; static int g_npool, g_cappool;
static alias_ent *g_alias; static int g_nalias, g_capalias;
static pthread_mutex_t g_pool_mu = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_pool_identity;
static uint64_t g_pool_bytes, g_pool_cpu;
/* Address index for resolve(): GPU-backed spans sorted by start, plus elementary segments naming the furthest-reaching
   span. An early large pool kept the old prefix-max backscan linear; resolve_local was 10.6% of recording at Lookout Landing. */
typedef struct { uintptr_t lo, end; VkBuffer buf; uint64_t off; } span_t;   /* off: buffer offset of lo */
typedef struct { uintptr_t lo; int best; } span_seg_t;   /* best covers [lo, next lo); the last breakpoint has no span */
static span_t *g_span; static span_seg_t *g_span_seg; static int *g_span_heap; static int g_nspan, g_nseg, g_capspan, g_span_dirty = 1;
static _Atomic uint64_t g_span_gen = 1;   /* bumped (under g_pool_mu) with every change of the pools or aliases */
static _Atomic uint64_t g_owner_gen = 1; /* physical buffer ownership, independent of virtual mapping churn */
static void span_changed(void) { g_span_dirty = 1; atomic_fetch_add_explicit(&g_span_gen, 1, memory_order_release); }

static uintptr_t page_sz(void) { static uintptr_t p; if (!p) p = (uintptr_t)getpagesize(); return p; }
static pool_ent *own_containing(uintptr_t a, uintptr_t b) {   /* caller holds g_pool_mu */
    for (int i = 0; i < g_npool; i++) { pool_ent *p = g_pools[i]; if (p->kind == K_OWN && a >= p->base && b <= p->end) return p; }
    return NULL;
}
static int own_overlapping(uintptr_t a, uintptr_t b) {
    for (int i = 0; i < g_npool; i++) { pool_ent *p = g_pools[i]; if (p->kind == K_OWN && a < p->end && b > p->base) return 1; }
    return 0;
}
static VkBuffer make_buffer(VkDeviceSize size, VkExternalMemoryHandleTypeFlags ht, VkMemoryRequirements *req) {
    VkExternalMemoryBufferCreateInfo eb = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO, .handleTypes = ht};
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, ht ? &eb : NULL, 0, size, POOL_USAGE, VK_SHARING_MODE_EXCLUSIVE};
    VkBuffer b; VKCHECK(vkCreateBuffer(g_dev, &bci, NULL, &b));
    vkGetBufferMemoryRequirements(g_dev, b, req);
    return b;
}
#ifdef TOTK_OHOS
/* Pool memory probe (once): the CPU reads the exported pool mappings at ~1.4 GB/s whatever the Vulkan memory type
   ([ohos-pool-bw]). Which native-buffer allocation does the CPU map cached, and does the driver import it?
   Each variant: OH_NativeBuffer_Alloc 4 MiB, mmap of its BufferHandle fd and OH_NativeBuffer_Map, read bandwidth of
   both, vkGetNativeBufferPropertiesOHOS memory types. Also lists every device extension (placed mapping?). */
static double ohos_read_gbps(const void *p, size_t n) {
    double best = 1e30; volatile uint64_t sink = 0;
    for (int it = 0; it < 3; it++) {
        uint64_t t0 = host_now_ns(), acc = 0; const volatile uint64_t *q = (const volatile uint64_t *)p;
        for (size_t i = 0; i < n / 8; i += 4) acc += q[i] ^ q[i + 1] ^ q[i + 2] ^ q[i + 3];
        uint64_t dt = host_now_ns() - t0; sink += acc; if (dt < best) best = (double)dt;
    }
    return n / best;
}
static void ohos_nb_probe(void) {
    static int done; if (done) return; done = 1;
    {   char line[8192]; size_t o = 0; line[0] = 0;   /* g_ext: the device's extensions (mtl_init) */
        for (uint32_t i = 0; i < g_next && o + 80 < sizeof line; i++) o += (size_t)snprintf(line + o, sizeof line - o, " %s", g_ext[i].extensionName);
        LOGV("[ohos-nb] %u device extensions:%s", g_next, line); }
    static PFN_vkGetNativeBufferPropertiesOHOS props_fn;
    if (!props_fn) props_fn = (PFN_vkGetNativeBufferPropertiesOHOS)gipa(V.inst, "vkGetNativeBufferPropertiesOHOS");
    const size_t n = 4u << 20;
    const struct { const char *name; uint64_t usage; } var[] = {
        {"dma", NATIVEBUFFER_USAGE_CPU_READ | NATIVEBUFFER_USAGE_CPU_WRITE | NATIVEBUFFER_USAGE_MEM_DMA | NATIVEBUFFER_USAGE_HW_RENDER | NATIVEBUFFER_USAGE_HW_TEXTURE},
        {"dma+cache", NATIVEBUFFER_USAGE_CPU_READ | NATIVEBUFFER_USAGE_CPU_WRITE | NATIVEBUFFER_USAGE_MEM_DMA | NATIVEBUFFER_USAGE_MEM_MMZ_CACHE | NATIVEBUFFER_USAGE_HW_RENDER | NATIVEBUFFER_USAGE_HW_TEXTURE},
        {"dma+often", NATIVEBUFFER_USAGE_CPU_READ | NATIVEBUFFER_USAGE_CPU_WRITE | NATIVEBUFFER_USAGE_MEM_DMA | NATIVEBUFFER_USAGE_CPU_READ_OFTEN | NATIVEBUFFER_USAGE_HW_RENDER | NATIVEBUFFER_USAGE_HW_TEXTURE},
        {"dma+cache+often", NATIVEBUFFER_USAGE_CPU_READ | NATIVEBUFFER_USAGE_CPU_WRITE | NATIVEBUFFER_USAGE_MEM_DMA | NATIVEBUFFER_USAGE_MEM_MMZ_CACHE | NATIVEBUFFER_USAGE_CPU_READ_OFTEN | NATIVEBUFFER_USAGE_HW_RENDER | NATIVEBUFFER_USAGE_HW_TEXTURE},
        {"cpu-only+cache", NATIVEBUFFER_USAGE_CPU_READ | NATIVEBUFFER_USAGE_CPU_WRITE | NATIVEBUFFER_USAGE_MEM_DMA | NATIVEBUFFER_USAGE_MEM_MMZ_CACHE},
    };
    for (unsigned v = 0; v < sizeof var / sizeof var[0]; v++) for (int fmt = 0; fmt < 2; fmt++) {
        OH_NativeBuffer_Config c = {.width = fmt ? 1024 : (int32_t)n, .height = fmt ? (int32_t)(n / 4096) : 1,
                                    .format = fmt ? NATIVEBUFFER_PIXEL_FMT_RGBA_8888 : NATIVEBUFFER_PIXEL_FMT_BLOB, .usage = (int32_t)var[v].usage};
        OH_NativeBuffer *nb = OH_NativeBuffer_Alloc(&c);
        if (!nb) { LOGV("[ohos-nb] %s %s: alloc failed", var[v].name, fmt ? "rgba" : "blob"); continue; }
        OHNativeWindowBuffer *wb = OH_NativeWindow_CreateNativeWindowBufferFromNativeBuffer(nb);
        BufferHandle *h = wb ? OH_NativeWindow_GetBufferHandleFromNative(wb) : NULL;
        double fd_gbps = -1, map_gbps = -1, w_gbps = -1; int hsize = h ? h->size : -1; uint64_t husage = h ? h->usage : 0;
        if (h && h->fd >= 0 && (size_t)h->size >= n) {
            void *m = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_SHARED, h->fd, 0);
            if (m != MAP_FAILED) {
                uint64_t t0 = host_now_ns(); memset(m, 1, n); w_gbps = n / (double)(host_now_ns() - t0 + 1);
                fd_gbps = ohos_read_gbps(m, n); munmap(m, n);
            }
        }
        void *va = NULL;
        if (!OH_NativeBuffer_Map(nb, &va) && va) { map_gbps = ohos_read_gbps(va, n); OH_NativeBuffer_Unmap(nb); }
        VkNativeBufferPropertiesOHOS pp = {VK_STRUCTURE_TYPE_NATIVE_BUFFER_PROPERTIES_OHOS};
        VkResult pr = props_fn ? props_fn(g_dev, nb, &pp) : VK_ERROR_EXTENSION_NOT_PRESENT;
        LOGV("[ohos-nb] %s %s: handle size %d usage %#llx; fd mmap read %.2f GB/s write %.2f GB/s; NativeBuffer_Map read %.2f GB/s; Vulkan import props %d: size %llu types %#x",
             var[v].name, fmt ? "rgba" : "blob", hsize, (unsigned long long)husage, fd_gbps, w_gbps, map_gbps, pr,
             (unsigned long long)pp.allocationSize, pp.memoryTypeBits);
        if (wb) OH_NativeWindow_DestroyNativeWindowBuffer(wb);
        OH_NativeBuffer_Unreference(nb);
    }
}
#endif
/* HarmonyOS pool memory: an OH_NativeBuffer allocated with MEM_MMZ_CACHE and imported into Vulkan. The export path
   (vkAllocateMemory exporting a native buffer) gets memory whose CPU mapping is uncached whatever the Vulkan memory type:
   the CPU reads it at ~1.4 GB/s, and the game and the command walker read pool memory constantly. A cached native
   buffer maps at ~36 GB/s ([ohos-nb] probe). The driver imports it as memory type 1 (HOST_CACHED, not HOST_COHERENT);
   with protectedMemory off it keeps I/O coherency (the GPU snoops the CPU caches; "close io coherency" otherwise), and
   pools are never vkMapMemory'd, so no flush/invalidate calls exist to make. The fd of the buffer's handle is mapped at
   the guest address as before. Returns that fd (duplicated), or -1 with nothing left allocated.
   TOTK_OHOS_POOL_ALLOC=export keeps the export path. */
static int ohos_pool_import(pool_ent *e, size_t size) {
    static PFN_vkGetNativeBufferPropertiesOHOS props_fn; static int said;
    if (!props_fn) props_fn = (PFN_vkGetNativeBufferPropertiesOHOS)gipa(V.inst, "vkGetNativeBufferPropertiesOHOS");
    if (!props_fn || size > 0x7fffffffu) return -1;
    OH_NativeBuffer_Config c = {.width = (int32_t)size, .height = 1, .format = NATIVEBUFFER_PIXEL_FMT_BLOB,
        .usage = (int32_t)(NATIVEBUFFER_USAGE_CPU_READ | NATIVEBUFFER_USAGE_CPU_WRITE | NATIVEBUFFER_USAGE_MEM_DMA |
                           NATIVEBUFFER_USAGE_MEM_MMZ_CACHE | NATIVEBUFFER_USAGE_HW_RENDER | NATIVEBUFFER_USAGE_HW_TEXTURE)};
    OH_NativeBuffer *nb = OH_NativeBuffer_Alloc(&c);
    if (!nb) { if (said++ < 4) LOGV("pool %#llx+%#zx: cached native buffer allocation failed", (unsigned long long)e->base, size); return -1; }
    VkNativeBufferPropertiesOHOS pp = {VK_STRUCTURE_TYPE_NATIVE_BUFFER_PROPERTIES_OHOS};
    VkMemoryRequirements req;
    e->buf = make_buffer(size, VK_EXTERNAL_MEMORY_HANDLE_TYPE_OHOS_NATIVE_BUFFER_BIT_OHOS, &req);
    int t = props_fn(g_dev, nb, &pp) || pp.allocationSize < size ? -1 : vk_pick_type(pp.memoryTypeBits & req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    VkImportNativeBufferInfoOHOS imp = {VK_STRUCTURE_TYPE_IMPORT_NATIVE_BUFFER_INFO_OHOS, .buffer = nb};
    VkMemoryDedicatedAllocateInfo ded = {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, &imp, .buffer = e->buf};
    VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &ded, pp.allocationSize, (uint32_t)t};
    VkResult r = t < 0 ? VK_ERROR_UNKNOWN : vk_memory_allocate(&mai, &e->mem, VK_MEMORY_POOL);
    int fd = -1;
    if (!r) {
        OHNativeWindowBuffer *wb = OH_NativeWindow_CreateNativeWindowBufferFromNativeBuffer(nb);
        BufferHandle *h = wb ? OH_NativeWindow_GetBufferHandleFromNative(wb) : NULL;
        if (h && h->fd >= 0 && (size_t)h->size >= size) fd = fcntl(h->fd, F_DUPFD_CLOEXEC, 0);
        if (wb) OH_NativeWindow_DestroyNativeWindowBuffer(wb);
        if (fd < 0) vk_memory_release(e->mem, VK_MEMORY_POOL);
    }
    if (fd < 0) {
        if (said++ < 4) LOGV("pool %#llx+%#zx: cached native buffer import failed (type %d, result %d, allocation %llu, types %#x)",
                              (unsigned long long)e->base, size, t, r, (unsigned long long)pp.allocationSize, pp.memoryTypeBits);
        vkDestroyBuffer(g_dev, e->buf, NULL); e->buf = VK_NULL_HANDLE; OH_NativeBuffer_Unreference(nb); return -1;
    }
    if (!said++) LOGV("pools: cached OH_NativeBuffer (MEM_MMZ_CACHE) imported as memory type %d (%#x)", t, V.mp.memoryTypes[t].propertyFlags);
    e->nb = nb;   /* our reference: released after the Vulkan memory (unback_pool) */
    VKCHECK(vkBindBufferMemory(g_dev, e->buf, e->mem, 0));
    return fd;
}
/* Give [base, end) its own device memory. 0 on failure (the pool then runs on the CPU path). */
static const void *g_back_src;   /* lazy_back_unit: back_pool copies the unit's contents from this snapshot (else from the pool) */
static int g_lazy_backing;   /* lazy_back_units is backing (g_pool_mu) */
static int back_pool(pool_ent *e) {
    size_t size = e->end - e->base;
    VkMemoryRequirements req;
    if (g_pool_mode == POOLS_HOST) {
        VkMemoryHostPointerPropertiesEXT hp = {VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
        if (vkGetMemoryHostPointerPropertiesEXT(g_dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, (void *)e->base, &hp) || !hp.memoryTypeBits) return 0;
        e->buf = make_buffer(size, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, &req);
        int t = pick_host_type(hp.memoryTypeBits & req.memoryTypeBits);
        VkImportMemoryHostPointerInfoEXT imp = {VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT, .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, .pHostPointer = (void *)e->base};
        VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &imp, size, (uint32_t)t};
        if (t < 0 || vk_memory_allocate(&mai, &e->mem, VK_MEMORY_POOL)) { vkDestroyBuffer(g_dev, e->buf, NULL); return 0; }
        VKCHECK(vkBindBufferMemory(g_dev, e->buf, e->mem, 0));
        e->fd = -1;
        return 1;
    }
    int fd = -1;
#ifdef TOTK_OHOS
    int imported = 0;
    if (g_pool_mode == POOLS_OHOS) {
        if (getenv("TOTK_OHOS_NB_PROBE")) ohos_nb_probe();
        const char *how = getenv("TOTK_OHOS_POOL_ALLOC");
        if (!how || strcmp(how, "export")) { fd = ohos_pool_import(e, size); imported = fd >= 0; }
    }
    if (g_pool_mode == POOLS_OHOS && !imported) {
        /* Dedicated export as an OH_NativeBuffer; its BufferHandle fd (a dma-buf) is then mapped as in export. The fd is
           duplicated, so the native buffer wrappers are released at once: the device memory and the fd keep it alive. */
        e->buf = make_buffer(size, VK_EXTERNAL_MEMORY_HANDLE_TYPE_OHOS_NATIVE_BUFFER_BIT_OHOS, &req);
        int t = pick_host_type(req.memoryTypeBits);
        /* Maleoon has no HOST_CACHED | HOST_COHERENT type, so pick_host_type takes the coherent uncached one, whose
           mapping the CPU reads at ~1.5 GB/s (ordinary memory ~38 GB/s; [ohos-pool-bw]). With protectedMemory off the
           driver keeps I/O coherency on (no "close io coherency"): the GPU snoops the CPU caches, so the cached type
           stays coherent at submission granularity. TOTK_OHOS_POOL_TYPE=<index> forces a type (diagnostic). */
        {   const char *pt = getenv("TOTK_OHOS_POOL_TYPE"); static int said;
            if (pt && *pt) { int want = atoi(pt); if (want >= 0 && want < 32 && (req.memoryTypeBits >> want & 1)) t = want; }
            if (!said++) LOGV("pools: memory type %d (allowed %#x, TOTK_OHOS_POOL_TYPE=%s)", t, req.memoryTypeBits, pt ? pt : "unset"); }
        VkMemoryDedicatedAllocateInfo ded = {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, .buffer = e->buf};
        VkExportMemoryAllocateInfo ex = {VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO, &ded, VK_EXTERNAL_MEMORY_HANDLE_TYPE_OHOS_NATIVE_BUFFER_BIT_OHOS};
        VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &ex, req.size, (uint32_t)t};
        VkResult r = t < 0 ? VK_ERROR_UNKNOWN : vk_memory_allocate(&mai, &e->mem, VK_MEMORY_POOL);
        if (r) { LOGV("pool %#llx+%#zx: OH_NativeBuffer export allocation failed: %d", (unsigned long long)e->base, size, r); vkDestroyBuffer(g_dev, e->buf, NULL); return 0; }
        static PFN_vkGetMemoryNativeBufferOHOS get_native;
        if (!get_native) get_native = (PFN_vkGetMemoryNativeBufferOHOS)gipa(V.inst, "vkGetMemoryNativeBufferOHOS");   /* instance-level trampoline */
        VkMemoryGetNativeBufferInfoOHOS gi = {VK_STRUCTURE_TYPE_MEMORY_GET_NATIVE_BUFFER_INFO_OHOS, .memory = e->mem};
        struct OH_NativeBuffer *nb = NULL;
        if (!get_native || get_native(g_dev, &gi, &nb) || !nb) { LOGV("pool %#llx+%#zx: vkGetMemoryNativeBufferOHOS failed", (unsigned long long)e->base, size); vk_memory_release(e->mem, VK_MEMORY_POOL); vkDestroyBuffer(g_dev, e->buf, NULL); return 0; }
        OHNativeWindowBuffer *wb = OH_NativeWindow_CreateNativeWindowBufferFromNativeBuffer((OH_NativeBuffer *)nb);
        BufferHandle *h = wb ? OH_NativeWindow_GetBufferHandleFromNative(wb) : NULL;
        if (h && h->fd >= 0 && (size_t)h->size >= size) fd = fcntl(h->fd, F_DUPFD_CLOEXEC, 0);
        else LOGV("pool %#llx+%#zx: native buffer handle %s (size %d)", (unsigned long long)e->base, size, h ? "too small or no fd" : "missing", h ? h->size : -1);
        if (wb) OH_NativeWindow_DestroyNativeWindowBuffer(wb);
        /* Whether vkGetMemoryNativeBufferOHOS hands the caller a reference (as Android's AHB query does) is not
           established for this driver; releasing one it did not give would free the pool under the GPU. Kept (one
           reference per pool at worst) unless TOTK_OHOS_NB_UNREF=1. */
        { const char *u = getenv("TOTK_OHOS_NB_UNREF"); if (u && !strcmp(u, "1")) OH_NativeBuffer_Unreference((OH_NativeBuffer *)nb); }
        if (fd < 0) { vk_memory_release(e->mem, VK_MEMORY_POOL); vkDestroyBuffer(g_dev, e->buf, NULL); return 0; }
    } else if (!imported)
#endif
    e->buf = make_buffer(size, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, &req);
    if (g_pool_mode == POOLS_OHOS) {
    } else if (g_pool_mode == POOLS_EXPORT) {
        int t = pick_host_type(req.memoryTypeBits);
        VkMemoryDedicatedAllocateInfo ded = {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, .buffer = e->buf};
        VkExportMemoryAllocateInfo ex = {VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO, g_export_dedicated ? &ded : NULL, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
        VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &ex, req.size, (uint32_t)t};
        VkResult r = t < 0 ? VK_ERROR_UNKNOWN : vk_memory_allocate(&mai, &e->mem, VK_MEMORY_POOL);
        if (r) { LOGV("pool %#llx+%#zx: export allocation failed: %d", (unsigned long long)e->base, size, r); vkDestroyBuffer(g_dev, e->buf, NULL); return 0; }
        VkMemoryGetFdInfoKHR gi = {VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR, .memory = e->mem, .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
        if (vkGetMemoryFdKHR(g_dev, &gi, &fd) || fd < 0) { vk_memory_release(e->mem, VK_MEMORY_POOL); vkDestroyBuffer(g_dev, e->buf, NULL); return 0; }
    } else {
        struct dma_heap_alloc a = {.len = req.size > size ? req.size : size, .fd_flags = O_RDWR | O_CLOEXEC};
        if (ioctl(g_heap_fd, DMA_HEAP_IOCTL_ALLOC, &a)) { LOGV("pool %#llx+%#zx: dma-buf heap allocation: %s", (unsigned long long)e->base, size, strerror(errno)); vkDestroyBuffer(g_dev, e->buf, NULL); return 0; }
        fd = (int)a.fd;
        VkMemoryFdPropertiesKHR fp = {VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
        int t = vkGetMemoryFdPropertiesKHR(g_dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, fd, &fp) ? -1 : pick_host_type(fp.memoryTypeBits & req.memoryTypeBits);
        VkImportMemoryFdInfoKHR imp = {VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR, .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, .fd = dup(fd)};
        VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &imp, a.len, (uint32_t)t};
        if (t < 0 || vk_memory_allocate(&mai, &e->mem, VK_MEMORY_POOL)) { close(imp.fd); close(fd); vkDestroyBuffer(g_dev, e->buf, NULL); return 0; }
    }
#ifdef TOTK_OHOS
    if (!imported)
#endif
    VKCHECK(vkBindBufferMemory(g_dev, e->buf, e->mem, 0));
    /* Move the contents in, then put the dma-buf where they were: one mmap replaces the pages at once. */
    void *tmp = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (tmp == MAP_FAILED) DIE("pool %#llx+%#zx: mapping the dma-buf: %s", (unsigned long long)e->base, size, strerror(errno));
#ifdef TOTK_OHOS
    if (!g_back_src) {
        /* Only the pages the guest has (mincore); a page it never touched is a hole of its shared memory, which reading
           would allocate first, so the dma-buf gets zeros there instead. The shared memory's pages are then punched out
           (MADV_REMOVE): replacing the mapping alone left them allocated in the shmem object, out of every RSS but in
           the phone's memory (oot-a46: memmgr at its lowest available buffer while the game loaded). */
        static unsigned char vec[8192];   /* 32 MiB of 4 KiB pages a call; the pool lock serializes back_pool */
        size_t copied = 0;
        for (size_t at = 0; at < size; at += sizeof vec * 4096) {
            size_t n = size - at < sizeof vec * 4096 ? size - at : sizeof vec * 4096, pages = (n + 4095) / 4096;
            if (mincore((void *)(e->base + at), n, vec)) { memcpy((uint8_t *)tmp + at, (const void *)(e->base + at), n); copied += n; continue; }
            for (size_t p = 0; p < pages; p++) {
                size_t len = p == pages - 1 ? n - p * 4096 : 4096;
                if (vec[p] & 1) { memcpy((uint8_t *)tmp + at + p * 4096, (const void *)(e->base + at + p * 4096), len); copied += len; }
                else memset((uint8_t *)tmp + at + p * 4096, 0, len);
            }
        }
        if (madvise((void *)e->base, size, MADV_REMOVE) && errno == EACCES && !mprotect((void *)e->base, size, PROT_READ | PROT_WRITE))
            madvise((void *)e->base, size, MADV_REMOVE);
        static uint64_t g_backed_total, g_backed_copied; g_backed_total += size; g_backed_copied += copied;
        static int said; if (said++ < 6) LOGV("pool %#llx+%#zx: native buffer filled from %zu resident bytes (all pools so far: %llu of %llu MiB were resident)", (unsigned long long)e->base, size,
                                             copied, (unsigned long long)(g_backed_copied >> 20), (unsigned long long)(g_backed_total >> 20));
    } else
#endif
    memcpy(tmp, g_back_src ? g_back_src : (const void *)e->base, size);
#ifdef TOTK_OHOS
    /* not atomic for other threads on this kernel: a game thread touching the range during the call faults (ohos_fault.c retries it) */
    { void ohos_fault_moving(uintptr_t, uintptr_t); ohos_fault_moving(e->base, e->end); }
#endif
    if (mmap((void *)e->base, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0) != (void *)e->base)
        DIE("pool %#llx+%#zx: mapping the dma-buf at the guest address: %s", (unsigned long long)e->base, size, strerror(errno));
#ifdef TOTK_OHOS
    { void ohos_fault_moved(void); ohos_fault_moved(); }
#endif
    munmap(tmp, size);
    e->fd = fd;
#ifdef TOTK_OHOS
    {   /* Write protection does not work on these mappings (mprotect EINVAL, even per-unit read-only remaps are not
           enforced: the dirty tracker leaves such pools untracked, nvn_dirty.c). How fast does the CPU read and write
           them compared with ordinary memory? Once per process on a pool of at least 4 MiB, before the game uses it:
           the bytes are saved and put back. */
        static int probed;
        if (!probed && size >= (4u << 20) && !g_lazy_backing) {   /* not a pool that gets its memory late: the game may be writing it */
            probed = 1;
            const size_t n = 4u << 20; uint8_t *anon = malloc(n), *save = malloc(n);
            if (anon && save) {
                memset(anon, 1, n); memcpy(save, (void *)e->base, n);
                double r_pool = 1e30, r_anon = 1e30, w_pool = 1e30, w_anon = 1e30; volatile uint64_t sink = 0;
                for (int it = 0; it < 3; it++) {
                    uint64_t t0 = host_now_ns(), acc = 0; const volatile uint64_t *q = (const volatile uint64_t *)e->base;
                    for (size_t i = 0; i < n / 8; i += 4) acc += q[i] ^ q[i + 1] ^ q[i + 2] ^ q[i + 3];
                    uint64_t t1 = host_now_ns(); const volatile uint64_t *qa = (const volatile uint64_t *)anon;
                    for (size_t i = 0; i < n / 8; i += 4) acc += qa[i] ^ qa[i + 1] ^ qa[i + 2] ^ qa[i + 3];
                    uint64_t t2 = host_now_ns(); memset((void *)e->base, it, n);
                    uint64_t t3 = host_now_ns(); memset(anon, it, n);
                    uint64_t t4 = host_now_ns(); sink += acc;
                    if (t1 - t0 < r_pool) r_pool = (double)(t1 - t0);
                    if (t2 - t1 < r_anon) r_anon = (double)(t2 - t1);
                    if (t3 - t2 < w_pool) w_pool = (double)(t3 - t2);
                    if (t4 - t3 < w_anon) w_anon = (double)(t4 - t3);
                }
                memcpy((void *)e->base, save, n);
                LOGV("[ohos-pool-bw] pool %#llx: CPU read %.2f GB/s (ordinary memory %.2f), write %.2f GB/s (ordinary %.2f)",
                     (unsigned long long)e->base, n / r_pool, n / r_anon, n / w_pool, n / w_anon);
            }
            free(anon); free(save);
        }
    }
#endif
    return 1;
}
static void unback_pool(pool_ent *e) {
    size_t size = e->end - e->base;
#ifdef TOTK_OHOS
    /* HarmonyOS: mremap onto the native-buffer mapping fails (EINVAL; every other mremap works, native-harmonyos probe).
       The contents go into a memfd that one mmap(MAP_FIXED) puts in place instead: still one step, still shared pages
       (totk_vpool.c's mremap(old_size 0) aliases need shared mappings). */
    if (e->fd >= 0 && g_pool_mode == POOLS_OHOS) {
        int mfd = (int)syscall(SYS_memfd_create, "totk-pool", MFD_CLOEXEC);
        if (mfd < 0 || ftruncate(mfd, (off_t)size)) DIE("pool %#llx: memfd: %s", (unsigned long long)e->base, strerror(errno));
        void *tmp = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, mfd, 0);
        if (tmp == MAP_FAILED) DIE("pool %#llx: memfd mapping: %s", (unsigned long long)e->base, strerror(errno));
        memcpy(tmp, (void *)e->base, size);
        { void ohos_fault_moving(uintptr_t, uintptr_t); ohos_fault_moving(e->base, e->end); }
        if (mmap((void *)e->base, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, mfd, 0) != (void *)e->base) {
            int err = errno;
            if (munmap((void *)e->base, size) || mmap((void *)e->base, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, mfd, 0) != (void *)e->base)
                DIE("pool %#llx+%#zx: restoring guest pages over the native buffer: %s (then %s)", (unsigned long long)e->base, size, strerror(err), strerror(errno));
            LOGV("pool %#llx+%#zx: native buffer unmapped first (MAP_FIXED over it: %s)", (unsigned long long)e->base, size, strerror(err));
        }
        { void ohos_fault_moved(void); ohos_fault_moved(); }
        munmap(tmp, size); close(mfd);
        close(e->fd); e->fd = -1;
    }
#endif
    if (e->fd >= 0) {   /* contents back into ordinary shared memory, moved into place in one step */
        void *fresh = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        if (fresh == MAP_FAILED) DIE("pool %#llx: fresh pages: %s", (unsigned long long)e->base, strerror(errno));
        memcpy(fresh, (void *)e->base, size);
        if (mremap(fresh, size, size, MREMAP_MAYMOVE | MREMAP_FIXED, (void *)e->base) != (void *)e->base)
            DIE("pool %#llx: restoring guest pages: %s", (unsigned long long)e->base, strerror(errno));
        close(e->fd); e->fd = -1;
    }
    vkDestroyBuffer(g_dev, e->buf, NULL); vk_memory_release(e->mem, VK_MEMORY_POOL);
#ifdef TOTK_OHOS
    if (e->nb) { OH_NativeBuffer_Unreference((OH_NativeBuffer *)e->nb); e->nb = NULL; }
#endif
}
static void pool_insert(pool_ent *e) {
    span_changed();
    if (e->kind == K_OWN) atomic_fetch_add_explicit(&g_owner_gen, 1, memory_order_release);
    e->identity = ++g_pool_identity;
    if (g_npool == g_cappool) { g_cappool = g_cappool ? 2 * g_cappool : 64; g_pools = VK_REALLOC(g_pools, (size_t)g_cappool * sizeof *g_pools); }
    g_pools[g_npool++] = e;
}
/* A pool larger than this is backed in chunks of this size, each its own device memory mapped at its own part of the guest
   range (Ocarina of Time's 2.4 GB pool: one OH_NativeBuffer cannot exceed 2 GB, and the export allocation failed, so its GPU
   work ran on the CPU). A resource that straddles two chunks resolves to neither (vk_resolve), which a game's allocator
   rarely makes; 1 GiB is also the device's storage buffer range. */
#define POOL_CHUNK (1ull << 30)
/* Big pools get GPU memory lazily (HarmonyOS): every pool's dma-buf counts against the app, and the system closes it
   (hiview StabilityCheckKill) near 5.6 GB of graphics memory. Ocarina of Time's 2.4 GB CPU pool (flags 0x24) is mostly
   texture and streaming storage the GPU never reads as memory (textures are Vulkan images uploaded from it). A pool of
   at least TOTK_OHOS_LAZY_MB (default 512; 0 = every pool at once) is registered as 32 MiB units without device memory;
   a unit is backed (back_pool) the first time a buffer lookup lands in it (lazy_back), together with the lazy units next
   to it that the same lookup covers. Global memory reached through the pool table (gmem) is not a lookup: an address in a
   unit not yet backed reads 0 there, as an address in no pool does.
   The resident-set line (RSS_THRESHOLD_KILLER, about 6 GiB of VmRSS: a dma-buf mapping is resident in full from the
   start) made this the rule for every CPU-visible pool of at least TOTK_OHOS_LAZY_MB (default 4 MiB): oot-a46 died
   loading the save with 1.58 GB of pool native buffers, 671 MB of them in 0x24 / 0x22 / 0x21 pools of 64-512 MB and
   347 MB in 48 0x24 pools of 4-32 MB. CPU-visible pools only (hle_nvn.c mtl_pool_flags_hint): the GPU-only ones
   (CPU_NO_ACCESS) are what shaders write through raw global memory, which a unit not yet backed would lose; they and
   pools of unknown flags keep the old rule, TOTK_OHOS_LAZY_BIG_MB (512). */
#define LAZY_UNIT (32ull << 20)
#define LAZY_GROUPS 8192   /* was 1024: the game makes about 4,600 pools, and the small ones are lazy too now (below) */
static uint64_t g_lazy_min = UINT64_MAX, g_lazy_big = UINT64_MAX;
static uint64_t g_lazy_units, g_lazy_backed;   /* units registered, units backed (g_pool_mu) */
static struct { uintptr_t lo, hi; } g_lazy_group[LAZY_GROUPS]; static _Atomic int g_nlazy_group;
static int lazy_on(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("TOTK_OHOS_LAZY_MB"); uint64_t mb = e && *e ? strtoull(e, NULL, 0) : 4; on = mb != 0 && g_pool_mode == POOLS_OHOS;
        e = getenv("TOTK_OHOS_LAZY_BIG_MB"); uint64_t big = e && *e ? strtoull(e, NULL, 0) : 512;
        if (on) { g_lazy_min = mb << 20; g_lazy_big = (big < mb ? mb : big) << 20; }
#ifdef TOTK_GAME_OOT
        /* Ocarina of Time (oot-a74, Kokiri Forest): the system kills the process when its graphics memory passes about 5 GB
           (hilog: PROCESS_KILL reason=GPU_THRESHOLD_KILLER; hidumper's GL column was 4.9 GB at each death), and 1.46 GB of it
           was native buffers of pools the GPU never reads as buffers: 914 CPU-visible pools of 64 KiB .. 4 MiB (526 MiB,
           one texture's storage each: textures are Vulkan images uploaded from them) and the GPU-only COMPRESSIBLE pools
           (flags 0xa1, 875 MiB: render target storage; the targets are images of their own). Both are lazy now: a pool
           gets its buffer when a buffer is made in it or a lookup lands in it, as the big CPU pools do.
           TOTK_OHOS_LAZY_KB=<n>: the smallest lazy pool in KiB (default 64; 4096 = the rule before). */
        if (on && !(getenv("TOTK_OHOS_LAZY_MB") && *getenv("TOTK_OHOS_LAZY_MB"))) {
            e = getenv("TOTK_OHOS_LAZY_KB"); uint64_t kb = e && *e ? strtoull(e, NULL, 0) : 64; if (kb < 4) kb = 4;
            g_lazy_min = kb << 10; if (g_lazy_big < g_lazy_min) g_lazy_big = g_lazy_min;
        }
#endif
    }
    return on;
}
static _Thread_local int g_add_flags = -1;   /* NVN pool flags of the mtl_pool_add that follows (hle_nvn.c pool_gpu_add) */
void mtl_pool_flags_hint(int flags) { g_add_flags = flags; }
/* nvn_dirty.c: a lazy pool is left untracked from the start, as dma-buf pools are (their pages cannot be write-protected
   once backed); its textures are checked by content. */
/* The lazy units that have no GPU memory yet, sorted by address, with the highest end so far beside each (units of a
   pool made inside another lazy pool overlap). vk_pool_lazy is asked at every nvnBufferGetAddress / nvnBufferInitialize:
   it scanned the table of every pool that was ever lazy (4,600 at Kokiri Forest) and said yes for pools backed long ago,
   and lazy_back then walked every pool twice under the pool lock: with mtl_buffer_fini's registry a third of all CPU
   samples of oot-a85 (hiperf), on the game's five worker threads and the replay thread. Writers hold g_pool_mu. */
typedef struct { uintptr_t lo, hi, top; } lz_t;
static lz_t *g_lz; static int g_nlz, g_caplz; static pthread_rwlock_t g_lz_rw = PTHREAD_RWLOCK_INITIALIZER; static _Atomic int g_lz_any;
static void lz_tops(int from) { for (int i = from; i < g_nlz; i++) g_lz[i].top = i && g_lz[i - 1].top > g_lz[i].hi ? g_lz[i - 1].top : g_lz[i].hi; }
static void lz_add(uintptr_t lo, uintptr_t hi) {
    pthread_rwlock_wrlock(&g_lz_rw);
    if (g_nlz == g_caplz) { int cap = g_caplz ? 2 * g_caplz : 1024; lz_t *n = realloc(g_lz, (size_t)cap * sizeof *n); if (n) { g_lz = n; g_caplz = cap; } }
    if (g_nlz < g_caplz) {
        int i = 0, j = g_nlz; while (i < j) { int m = (i + j) / 2; if (g_lz[m].lo < lo) i = m + 1; else j = m; }
        memmove(g_lz + i + 1, g_lz + i, (size_t)(g_nlz - i) * sizeof *g_lz);
        g_lz[i] = (lz_t){lo, hi, hi}; g_nlz++; lz_tops(i);
    }
    atomic_store_explicit(&g_lz_any, g_nlz, memory_order_release);
    pthread_rwlock_unlock(&g_lz_rw);
}
static void lz_del(uintptr_t lo, uintptr_t hi) {   /* the units that start in [lo, hi): they got GPU memory, or their pool is gone */
    pthread_rwlock_wrlock(&g_lz_rw);
    int i = 0, j = g_nlz; while (i < j) { int m = (i + j) / 2; if (g_lz[m].lo < lo) i = m + 1; else j = m; }
    int k = i; while (k < g_nlz && g_lz[k].lo < hi) k++;
    if (k > i) { memmove(g_lz + i, g_lz + k, (size_t)(g_nlz - k) * sizeof *g_lz); g_nlz -= k - i; lz_tops(i); }
    atomic_store_explicit(&g_lz_any, g_nlz, memory_order_release);
    pthread_rwlock_unlock(&g_lz_rw);
}
/* 1 when [mem, mem + size) lies in a pool whose GPU memory (a native buffer) is mapped over it: readable by the process,
   though process_vm_readv refuses such a mapping (vk_res.c guest_readable). Reads the table only. */
int vk_pool_mapped(const void *mem, size_t size) {
    uintptr_t a = (uintptr_t)mem, b = a + size; int hit = 0;
    pthread_mutex_lock(&g_pool_mu);
    pool_ent *p = own_containing(a, b);
#ifdef TOTK_OHOS
    hit = p && p->nb;
#else
    hit = p != NULL;
#endif
    pthread_mutex_unlock(&g_pool_mu);
    return hit;
}
int vk_pool_lazy(const void *mem, size_t size) {
    if (!atomic_load_explicit(&g_lz_any, memory_order_acquire)) return 0;
    uintptr_t a = (uintptr_t)mem, b = a + size; int hit = 0;
    pthread_rwlock_rdlock(&g_lz_rw);
    int i = 0, j = g_nlz; while (i < j) { int m = (i + j) / 2; if (g_lz[m].lo < b) i = m + 1; else j = m; }   /* units [0, i) start before b */
    for (int k = i - 1; k >= 0 && g_lz[k].top > a; k--) if (g_lz[k].hi > a) { hit = 1; break; }
    pthread_rwlock_unlock(&g_lz_rw);
    return hit;
}
static uintptr_t g_watch_lo, g_watch_len;   /* the first pool of at least 1 GiB (watch_hit, below) */
static void add_range(const void *mem, uint64_t size, int virt) {
    mtl_init();
    uintptr_t pg = page_sz(), lo = (uintptr_t)mem, hi = lo + size;
    if (g_pool_mode == POOLS_HOST && pg < g_host_align) pg = (uintptr_t)g_host_align;
    pool_ent *e = VK_CALLOC(1, sizeof *e);
    e->lo = lo; e->hi = hi; e->base = lo & ~(pg - 1); e->end = (hi + pg - 1) & ~(pg - 1); e->fd = -1;
    if (!virt && size >= (1ull << 30) && !g_watch_len) { g_watch_lo = lo; g_watch_len = size; }
#ifdef TOTK_OHOS
    int add_flags = g_add_flags; g_add_flags = -1;
    e->flags = add_flags;
    int cpu_visible = add_flags >= 0 && !(add_flags & 0x1) && (add_flags & 0x6);   /* NVN CPU_NO_ACCESS 1, CPU_UNCACHED 2, CPU_CACHED 4 */
    /* a shader code pool (NVN SHADER_CODE 0x40) the CPU can write is lazy at any size, in 32 MiB units: the shaders come from
       the seed, but the game keeps a few buffers there (hle_nvn.c pool_gpu_skip) */
    int shader_code = cpu_visible && (add_flags & 0x40);
#ifdef TOTK_GAME_OOT
    /* A GPU-only pool (CPU_NO_ACCESS 1) is lazy under the CPU-visible rule too (see lazy_on), as one unit. What the old
       rule feared, a shader writing it through raw global memory before any lookup lands there, needs an address, and the
       game has only two ways to one: a buffer made in the pool (mtl_buffer_init backs a GPU-only pool at once) and the
       pool's own address (nvnMemoryPoolGetBufferAddress: mtl_pool_address). Without either the pool holds textures only,
       which are Vulkan images of their own. oot-a81 at Kokiri Forest: 0x21 pools of 232, 45, 28 and 8 MiB and some 300
       smaller ones had native buffers and not one buffer in them (the textures the game streams in with copies).
       TOTK_OHOS_LAZY_GPUONLY=0: the rule before (lazy from TOTK_OHOS_LAZY_BIG_MB only). */
    static int gpu_only_lazy = -1; if (gpu_only_lazy < 0) { const char *g = getenv("TOTK_OHOS_LAZY_GPUONLY"); gpu_only_lazy = !(g && *g == '0'); }
    if (add_flags >= 0 && (add_flags & 0x1) && ((add_flags & 0x80) || (gpu_only_lazy && !(add_flags & 0x100)))) cpu_visible = 1;   /* not a PHYSICAL pool: virtual pools map its pages and find only pools with GPU memory */
#endif
    lazy_on();
    pthread_mutex_lock(&g_pool_mu);   /* own_containing / pool_insert below (the branch used to unlock a mutex it never took) */
    int lazy_slot = -1;   /* a slot of the lazy table: one a finalised pool gave back (del_range_ent), else the next */
    {   int used = atomic_load(&g_nlazy_group);
        for (int i = 0; i < used; i++) if (!g_lazy_group[i].hi) { lazy_slot = i; break; }
        if (lazy_slot < 0 && used < LAZY_GROUPS) lazy_slot = used;
        if (lazy_slot < 0) { static int said; if (!said++) LOGV("【红】lazy pools: the table of %d is full: pools get GPU memory when they are made from now on", LAZY_GROUPS); } }
    if (!virt && lazy_on() &&e->end - e->base >= (shader_code ? 1 : cpu_visible ? g_lazy_min : g_lazy_big) && lazy_slot >= 0 && !own_containing(e->base, e->end) && !own_overlapping(e->base, e->end)) {
        uintptr_t end = e->end; int n = 0; pool_ent *prev = NULL;
        /* A CPU-visible pool up to TOTK_OHOS_LAZY_WHOLE_MB (256) is one unit: it gets GPU memory whole when first used. In
           32 MiB units, a unit backed for a small buffer could never be bound with its neighbour for a buffer made later
           across them: Ocarina of Time's 150 MB pool 0x3af990000 got unit 0 for a buffer at its start, then made a 33.6 MB
           buffer at +0x680 ("crosses into a unit that has GPU memory already", oot-a59), never bound. Bigger pools (the
           2.4 GB texture / streaming pool) keep 32 MiB units. */
        static uint64_t whole = 0; if (!whole) { const char *w = getenv("TOTK_OHOS_LAZY_WHOLE_MB"); whole = (w && *w ? strtoull(w, NULL, 0) : 256) << 20; if (!whole) whole = 1; }
        uint64_t unit = cpu_visible && !shader_code && end - e->base <= whole ? end - e->base : LAZY_UNIT;
        for (uintptr_t u = e->base; u < end; u += unit, n++) {
            pool_ent *c = u == e->base ? e : VK_CALLOC(1, sizeof *c);
            c->base = u; c->end = end - u > unit ? u + unit : end; c->lo = lo > c->base ? lo : c->base; c->hi = hi < c->end ? hi : c->end; c->fd = -1;
            c->group_lo = lo; c->group_size = size; c->kind = K_LAZY; c->flags = add_flags;
            if (prev) prev->chain = c;
            prev = c;
            pool_insert(c);
            lz_add(c->base, c->end);
        }
        g_lazy_group[lazy_slot].lo = e->base; g_lazy_group[lazy_slot].hi = end;
        if (lazy_slot == atomic_load(&g_nlazy_group)) atomic_store_explicit(&g_nlazy_group, lazy_slot + 1, memory_order_release);
        g_lazy_units += (uint64_t)n;
        pthread_mutex_unlock(&g_pool_mu);
        LOGV("pool %p+%#llx: GPU memory in %d units of %llu MiB, each when the GPU first uses it (TOTK_OHOS_LAZY_MB=%llu)", mem, (unsigned long long)size, n,
             (unsigned long long)(unit >> 20), (unsigned long long)(g_lazy_min >> 20));
        return;
    }
#else
    pthread_mutex_lock(&g_pool_mu);
#endif
    pool_ent *owner = own_containing(e->base, e->end);
#ifdef TOTK_OHOS
    if (!virt && !owner && g_pool_mode == POOLS_OHOS && e->end - e->base > POOL_CHUNK && !own_overlapping(e->base, e->end)) {
        uintptr_t end = e->end; int n = 0, cpu = 0; pool_ent *prev = NULL;
        for (uintptr_t b = e->base; b < end; b += POOL_CHUNK, n++) {
            pool_ent *c = b == e->base ? e : VK_CALLOC(1, sizeof *c);
            c->base = b; c->end = end - b > POOL_CHUNK ? b + POOL_CHUNK : end; c->lo = lo > c->base ? lo : c->base; c->hi = hi < c->end ? hi : c->end; c->fd = -1;
            c->group_lo = lo; c->group_size = size; c->flags = add_flags;
            c->kind = back_pool(c) ? K_OWN : K_CPU;
            if (c->kind == K_OWN) g_pool_bytes += c->end - c->base; else { cpu++; g_pool_cpu++; }
            if (prev) prev->chain = c;
            prev = c;
            pool_insert(c);
        }
        pthread_mutex_unlock(&g_pool_mu);
        LOGV("%spool %p+%#llx backed in %d chunks of %llu MiB%s", cpu ? "【红】" : "", mem, (unsigned long long)size, n, (unsigned long long)(POOL_CHUNK >> 20), cpu ? ": some got no GPU memory" : "");
        return;
    }
#endif
    if (virt && g_pool_mode != POOLS_HOST) e->kind = K_VIRT;
    else if (owner) { e->kind = K_VIEW; e->owner = owner; e->off = e->base - owner->base; }
    else if (own_overlapping(e->base, e->end)) e->kind = K_CPU;
    else e->kind = back_pool(e) ? K_OWN : K_CPU;
    if (e->kind == K_CPU) { static int said; if (said++ < 8) LOGV("【红】pool %p+%#llx gets no GPU memory (overlaps another pool or allocation failed): its GPU work runs on the CPU", mem, (unsigned long long)size); g_pool_cpu++; }
    if (e->kind == K_OWN) g_pool_bytes += e->end - e->base;
    pool_insert(e);
    pthread_mutex_unlock(&g_pool_mu);
}
static int pool_gpu_quiet(void);
static void del_range_ent(const void *mem, int wait_gpu, const pool_ent *want);
static void del_range(const void *mem, int wait_gpu) { del_range_ent(mem, wait_gpu, NULL); }
static void del_range_ent(const void *mem, int wait_gpu, const pool_ent *want) {   /* want: this entry (a pool's later chunk), else the last registered at mem */
    pthread_mutex_lock(&g_pool_mu);
    int idx = -1;
    for (int i = g_npool - 1; i >= 0; i--) if (want ? g_pools[i] == want : g_pools[i]->lo == (uintptr_t)mem) { idx = i; break; }
    if (idx < 0) { pthread_mutex_unlock(&g_pool_mu); return; }
    /* Finalize owns recording and has waited outside the registry; no producer
       can reserve another ticket before destruction. MapVirtual keeps its old path. */
    if (!wait_gpu && !pool_gpu_quiet()) DIE("pool finalize lost recording exclusion");
    pool_ent *e = g_pools[idx]; g_pools[idx] = g_pools[--g_npool]; span_changed();
    if (__builtin_expect(g_ic_k, 0)) ic_pool_barrier();   /* the in-flight check has stopped reading the pool before its memory can go */
#ifdef TOTK_OHOS
    if (e->kind == K_LAZY) lz_del(e->base, e->end);
    if (!want && e->group_size) {   /* the pool's slot of the lazy table (add_range): free for the next pool (8192 for the whole session before) */
        int used = atomic_load(&g_nlazy_group);
        for (int i = 0; i < used; i++) if (g_lazy_group[i].lo == e->base && g_lazy_group[i].hi) { g_lazy_group[i].lo = g_lazy_group[i].hi = 0; break; }
    }
#endif
    if (e->kind == K_VIRT) {   /* its aliases go with it (MapVirtual re-adds the pool after remapping) */
        for (int i = 0; i < g_nalias; ) { if (g_alias[i].va >= e->base && g_alias[i].end <= e->end) g_alias[i] = g_alias[--g_nalias]; else i++; }
    }
    if (e->kind == K_OWN) {
        atomic_fetch_add_explicit(&g_owner_gen, 1, memory_order_release);
        /* Pools inside it and virtual pages mapped from it lose their GPU backing; the CPU still sees the same bytes. */
        for (int i = 0; i < g_npool; i++) if (g_pools[i]->owner == e) { g_pools[i]->kind = K_CPU; g_pools[i]->owner = NULL; }
        for (int i = 0; i < g_nalias; ) { if (g_alias[i].owner == e) g_alias[i] = g_alias[--g_nalias]; else i++; }
        if (wait_gpu) vk_gpu_idle();
        HK_ADD(idle_pool, 1);
        unback_pool(e);
        g_pool_bytes -= e->end - e->base;
    }
    pthread_mutex_unlock(&g_pool_mu);
    vk_wb_range_gone(e->base, e->end);
    if (e->kind == K_OWN || e->kind == K_LAZY) vk_tex_pool_gone(e->base, e->end);   /* after the pool lock: texture code takes the locks the other way round; a pool never backed has shared images over it too */
    pool_ent *next = e->chain;
    free(e);
    if (next) del_range_ent((const void *)next->lo, wait_gpu, next);   /* the pool's other chunks */
}
#ifdef TOTK_OHOS
/* Back the lazy units c .. (n of them, consecutive in the chain) as one: caller holds g_pool_mu. The game's CPU may write
   them while they move: their old pages stay reachable through a second mapping of the same shared memory (the guest's
   memory is shared anonymous memory, services.c reserve_fixed), the dma-buf is filled from a snapshot, and after the
   mapping is replaced every word the old pages changed since the snapshot is carried over, unless the new mapping
   changed it too. The old pages are then given back (MADV_REMOVE). */
static int lazy_back_units_now(pool_ent *c, int n);
static int lazy_back_units(pool_ent *c, int n) {
    if (__builtin_expect(g_ic_k, 0)) {
        /* In-flight check: back_pool replaces the mapping under a registered K_OWN pool around or inside these units (own_overlapping
           ignores K_LAZY at registration). Readers of it finish first, and none compares its hashes across the move: g_ic_map_gen is
           odd while it lasts and moved on after (ic_note_new, ic_rehash). */
        pool_ent *last = c; for (int i = 1; i < n; i++) last = last->chain;
        if (own_overlapping(c->base, last->end)) {
            atomic_fetch_add_explicit(&g_ic_map_gen, 1, memory_order_acq_rel); ic_pool_barrier();
            int r = lazy_back_units_now(c, n);
            atomic_fetch_add_explicit(&g_ic_map_gen, 1, memory_order_acq_rel);
            return r;
        }
    }
    return lazy_back_units_now(c, n);
}
static int lazy_back_units_now(pool_ent *c, int n) {
    pool_ent *last = c; for (int i = 1; i < n; i++) last = last->chain;
    uintptr_t base = c->base, end = last->end; size_t size = end - base;
    if (c->flags >= 0 && (c->flags & 0x1)) {
        /* A GPU-only pool (CPU_NO_ACCESS): nothing the CPU wrote is there, so no second mapping, no snapshot and nothing
           to carry over. Reading the range for a snapshot would allocate every page of it first (a 232 MiB pool: twice
           its size for a second, on a phone a few hundred MB from the memory kill) and the second mapping is wrong where
           the range is not one mapping of the guest's memory. back_pool copies what is resident, as at registration. */
        uintptr_t hi = last->hi; pool_ent *after = last->chain; int join = last->join_next;
        for (pool_ent *u = c->chain, *nx; n > 1 && u != after; u = nx) {
            nx = u->chain;
            for (int i = 0; i < g_npool; i++) if (g_pools[i] == u) { g_pools[i] = g_pools[--g_npool]; break; }
            free(u);
        }
        c->end = end; c->hi = hi; c->chain = after; c->join_next = join;
        lz_del(base, end);
        g_lazy_backing = 1; int ok = back_pool(c); g_lazy_backing = 0;
        if (!ok) { c->kind = K_CPU; g_pool_cpu++; span_changed(); LOGV("【红】lazy pool unit %#llx+%#zx: no GPU memory", (unsigned long long)base, size); return 0; }
        c->kind = K_OWN; c->was_lazy = 1; g_pool_bytes += size; g_lazy_backed += (size + LAZY_UNIT - 1) / LAZY_UNIT;
        atomic_fetch_add_explicit(&g_owner_gen, 1, memory_order_release);
        span_changed();
        static int said_gpu; if (said_gpu++ < 16) LOGV("lazy pool: GPU-only %#llx+%#zx got GPU memory (%llu of %llu units now)", (unsigned long long)base, size, (unsigned long long)g_lazy_backed, (unsigned long long)g_lazy_units);
        return 1;
    }
    void *alias = mremap((void *)base, 0, size, MREMAP_MAYMOVE);
    if (alias == MAP_FAILED) { static int said; if (said++ < 4) LOGV("【红】lazy pool unit %#llx+%#zx: second mapping: %s", (unsigned long long)base, size, strerror(errno)); return 0; }
    uint64_t *snap = malloc(size);
    if (!snap) { munmap(alias, size); return 0; }
    memcpy(snap, alias, size);
    /* the units become one entry: c spans them, the others leave the registry */
    uintptr_t hi = last->hi; pool_ent *after = last->chain; int join_after = last->join_next;   /* before last is freed below */
    for (pool_ent *u = c->chain, *nx; n > 1 && u != after; u = nx) {
        nx = u->chain;
        for (int i = 0; i < g_npool; i++) if (g_pools[i] == u) { g_pools[i] = g_pools[--g_npool]; break; }
        free(u);
    }
    c->end = end; c->hi = hi; c->chain = after; c->join_next = join_after;
    lz_del(base, end);
    g_back_src = snap; g_lazy_backing = 1;
    int ok = back_pool(c);
    g_back_src = NULL; g_lazy_backing = 0;
    if (!ok) { c->kind = K_CPU; g_pool_cpu++; free(snap); munmap(alias, size); span_changed(); LOGV("【红】lazy pool unit %#llx+%#zx: no GPU memory", (unsigned long long)base, size); return 0; }
    const uint64_t *o = alias; uint64_t *nw = (uint64_t *)base; size_t carried = 0;
    for (size_t pg = 0; pg < size; pg += 4096) {
        if (!memcmp((const uint8_t *)alias + pg, (const uint8_t *)snap + pg, 4096)) continue;
        for (size_t w = pg / 8; w < (pg + 4096) / 8; w++) if (o[w] != snap[w] && nw[w] == snap[w]) { nw[w] = o[w]; carried++; }
    }
    madvise(alias, size, MADV_REMOVE);
    munmap(alias, size); free(snap);
    c->kind = K_OWN; c->was_lazy = 1; g_pool_bytes += size; g_lazy_backed += (size + LAZY_UNIT - 1) / LAZY_UNIT;
    atomic_fetch_add_explicit(&g_owner_gen, 1, memory_order_release);
    span_changed();
    static int said; if (said++ < 40 || carried)
        LOGV("lazy pool: %#llx+%#zx got GPU memory (%llu of %llu units now)%s%zu words written during the move carried over", (unsigned long long)base, size,
             (unsigned long long)g_lazy_backed, (unsigned long long)g_lazy_units, carried ? "; " : "; ", carried);
    return 1;
}
/* A buffer lookup of [a, b) found no GPU memory: back the lazy units it covers (consecutive ones as one, so a buffer across
   a unit boundary resolves). Returns how many were backed. The registry lock is only tried: a caller may hold it. */
static _Thread_local int T_lazy_wait;   /* lazy_back_wait: the caller holds no pool lock and must not be turned away */
static int lazy_back(uintptr_t a, uintptr_t b) {
    if (!vk_pool_lazy((const void *)a, b - a)) return 0;
    if (T_lazy_wait) pthread_mutex_lock(&g_pool_mu);
    else for (int spin = 0; pthread_mutex_trylock(&g_pool_mu); spin++) {
        if (spin > 4000) { static _Atomic int said; if (atomic_fetch_add(&said, 1) < 8) LOGV("【红】lazy pool: lookup %#llx+%#llx gave up waiting for the pool lock: no GPU memory this time", (unsigned long long)a, (unsigned long long)(b - a)); return 0; }
        sched_yield();
    }
    int backed = 0;
    for (int i = 0; i < g_npool; i++) {
        pool_ent *c = g_pools[i];
        if (c->kind != K_LAZY || !(a < c->end && b > c->base) || c->base > a) continue;   /* the first unit of the run */
        int n = 1; pool_ent *u = c;
        /* units a game buffer joins (mtl_buffer_init) come along: before the run's first unit, and after its last */
        for (int moved = 1; moved; ) {
            moved = 0;
            for (int j = 0; j < g_npool; j++) {
                pool_ent *p = g_pools[j];
                if (p->chain == c && p->kind == K_LAZY && p->join_next && p->end == c->base) { c = p; n++; moved = 1; break; }
            }
        }
        while ((u->end < b || u->join_next) && u->chain && u->chain->kind == K_LAZY && u->chain->base == u->end) { u = u->chain; n++; }
        if (u->end < b) { static int said; if (said++ < 8) LOGV("【红】lazy pool: lookup %#llx+%#llx crosses a unit that has GPU memory already: not resolvable", (unsigned long long)a, (unsigned long long)(b - a)); }
        backed += lazy_back_units(c, n);
        break;
    }
    if (!backed) for (int i = 0; i < g_npool; i++) {   /* the lookup starts in a unit already backed and runs into lazy ones */
        pool_ent *c = g_pools[i];
        if (c->kind == K_LAZY && a < c->end && b > c->base) { static int said; if (said++ < 8) LOGV("【红】lazy pool: lookup %#llx+%#llx starts in a unit with GPU memory and runs into one without", (unsigned long long)a, (unsigned long long)(b - a)); break; }
    }
    pthread_mutex_unlock(&g_pool_mu);
    return backed;
}
/* Raw global memory (GMEM: shaders that load an address and read through the pool table) does not look buffers up, so a
   lazy unit it reads stayed without GPU memory and read 0. Ocarina of Time's lighting chain reads so (50d3050c, dc9bb120,
   9e6b00b0, 6e28359a): its analysis buffer kept its reset values (+inf / -inf bounds, NaN), every lit material came out
   NaN, the world black under the HUD (pd-a54 / a55, after the queue order and FPCR fixes). A buffer whose GPU address the
   game takes (nvnBufferGetAddress) may be read that way: its unit gets GPU memory then. TOTK_OHOS_LAZY_ADDR=0: not. */
void mtl_buffer_address(const void *mem, uint64_t size) {
    static int on = -1; if (on < 0) { const char *e = getenv("TOTK_OHOS_LAZY_ADDR"); on = !(e && *e == '0'); }
    if (!on || !mem || !size || !atomic_load_explicit(&g_nlazy_group, memory_order_relaxed) || !vk_pool_lazy(mem, (size_t)size)) return;
    static _Atomic uint64_t calls, backed;
    T_lazy_wait++; int n = lazy_back((uintptr_t)mem, (uintptr_t)mem + size); T_lazy_wait--;   /* a game thread: nothing looks this memory up later */
    atomic_fetch_add(&calls, 1); if (n) atomic_fetch_add(&backed, (uint64_t)n);
    uint64_t c = atomic_load(&calls); if (n && (c < 16 || (c & (c - 1)) == 0))
        LOGV("lazy pool: buffer %p+%#llx had its GPU address taken: its unit got GPU memory (%llu such buffers, %llu backings)", mem, (unsigned long long)size, (unsigned long long)c, (unsigned long long)atomic_load(&backed));
}
/* The game's buffers (nvnBufferInitialize .. nvnBufferFinalize), for the memory report: a pool's native buffer is as big
   as the pool, whatever part of it the game reads as buffers. */
typedef struct { uintptr_t a, b; const void *obj; } gbuf_t;   /* obj: the NVNbuffer (Finalize must not read its pool, which may be gone) */
static gbuf_t *g_gbuf; static int g_ngbuf, g_capgbuf; static pthread_mutex_t g_gbuf_mu = PTHREAD_MUTEX_INITIALIZER;
static int gbuf_cmp(const void *x, const void *y) { uintptr_t a = ((const gbuf_t *)x)->a, b = ((const gbuf_t *)y)->a; return a < b ? -1 : a > b; }
static _Thread_local const void *T_gbuf_obj;   /* mtl_buffer_object: the NVNbuffer of the mtl_buffer_init that follows */
void mtl_buffer_object(const void *obj) { T_gbuf_obj = obj; }
/* TOTK_OHOS_POOL_CENSUS=1 only: the registry is a list under a lock, searched at every nvnBufferFinalize (the game makes
   and finalises buffers all the time: 100,000 alive). On by default in oot-a81 .. a85 it was the first line of the CPU
   profile. */
static int gbuf_on(void) { static int on = -1; if (on < 0) { const char *e = getenv("TOTK_OHOS_POOL_CENSUS"); on = e && *e == '1'; } return on; }
static void gbuf_add(uintptr_t a, uintptr_t b) {
    pthread_mutex_lock(&g_gbuf_mu);
    if (g_ngbuf == g_capgbuf) { int cap = g_capgbuf ? g_capgbuf * 2 : 4096; gbuf_t *n = realloc(g_gbuf, (size_t)cap * sizeof *n); if (n) { g_gbuf = n; g_capgbuf = cap; } }
    if (g_ngbuf < g_capgbuf) g_gbuf[g_ngbuf++] = (gbuf_t){a, b, T_gbuf_obj};
    T_gbuf_obj = NULL;
    pthread_mutex_unlock(&g_gbuf_mu);
}
void mtl_buffer_fini(const void *obj) {
    if (!gbuf_on()) return;
    pthread_mutex_lock(&g_gbuf_mu);
    for (int i = g_ngbuf - 1; i >= 0; i--) if (g_gbuf[i].obj == obj) { g_gbuf[i] = g_gbuf[--g_ngbuf]; break; }
    pthread_mutex_unlock(&g_gbuf_mu);
}
/* A game buffer (nvnBufferInitialize) across a unit boundary of a lazy pool: its units must get GPU memory as one, or no
   Vulkan buffer holds it whole. Units backed one at a time could never be bound together: Ocarina of Time's 150 MB CPU pool
   0x3af990000 (flags 0x24) got its first unit for a small buffer, then its 33.6 MB buffer at +0x680 ran into the second
   ("starts in a unit with GPU memory and runs into one without", oot-a58) and was never bound. So the boundaries a buffer
   crosses are marked (join_next) when it is made, before the GPU uses it, and lazy_back takes the joined units along.
   A buffer over more than TOTK_OHOS_LAZY_JOIN_MB (256) is not joined (a whole-pool view would back the whole pool).
   A boundary next to a unit backed already cannot be joined any more: reported. */
void mtl_buffer_init(const void *mem, uint64_t size) {
    static uint64_t cap = 0; if (!cap) { const char *e = getenv("TOTK_OHOS_LAZY_JOIN_MB"); cap = (e && *e ? strtoull(e, NULL, 0) : 256) << 20; if (!cap) cap = 1; }
    if (mem && size && gbuf_on()) gbuf_add((uintptr_t)mem, (uintptr_t)mem + size);
    if (!mem || size < 2 || !atomic_load_explicit(&g_nlazy_group, memory_order_relaxed) || !vk_pool_lazy(mem, (size_t)size)) return;
    uintptr_t a = (uintptr_t)mem, b = a + size;
    if (size > cap) { static int said; if (said++ < 8) LOGV("【红】lazy pool: buffer %p+%#llx is over TOTK_OHOS_LAZY_JOIN_MB: its units are not joined", mem, (unsigned long long)size); return; }
    int joined = 0, stuck = 0, gpu_only = 0;
    pthread_mutex_lock(&g_pool_mu);
    for (int i = 0; i < g_npool; i++) {
        pool_ent *u = g_pools[i], *nx = u->chain;
        if (u->kind == K_LAZY && u->flags >= 0 && (u->flags & 0x1) && a < u->end && b > u->base) gpu_only = 1;
        if (!u->group_size || !(u->end > a && u->end < b) || !nx || nx->base != u->end) continue;   /* a boundary inside the buffer */
        if (u->kind == K_LAZY && nx->kind == K_LAZY) { if (!u->join_next) { u->join_next = 1; joined++; } }
        else if (u->kind == K_LAZY || nx->kind == K_LAZY) stuck++;
    }
    pthread_mutex_unlock(&g_pool_mu);
    if (gpu_only) {   /* a buffer in a GPU-only pool: only the GPU can fill it, perhaps through raw global memory, which looks nothing up */
        T_lazy_wait++; int n = lazy_back(a, b); T_lazy_wait--; static _Atomic uint64_t made; uint64_t c = atomic_fetch_add(&made, 1) + 1;
        if (n && (c <= 16 || (c & (c - 1)) == 0)) LOGV("lazy pool: buffer %p+%#llx made in a GPU-only pool: the pool got GPU memory (%llu such buffers)", mem, (unsigned long long)size, (unsigned long long)c);
    }
    static _Atomic uint64_t njoined; uint64_t t = joined ? atomic_fetch_add(&njoined, (uint64_t)joined) + (uint64_t)joined : 0;
    if (joined && (t < 16 || (t & (t - 1)) == 0))
        LOGV("lazy pool: buffer %p+%#llx crosses %d unit boundaries: those units get GPU memory together (%llu joins)", mem, (unsigned long long)size, joined, (unsigned long long)t);
    if (stuck) { static int said; if (said++ < 8) LOGV("【红】lazy pool: buffer %p+%#llx crosses into a unit that has GPU memory already: it cannot be bound whole", mem, (unsigned long long)size); }
}
#else
static int lazy_back(uintptr_t a, uintptr_t b) { (void)a; (void)b; return 0; }
#endif
#ifdef TOTK_OHOS
/* The game took a GPU-only pool's address (nvnMemoryPoolGetBufferAddress): it may reach the pool's memory without a buffer
   object, so a lazy pool gets its GPU memory now. */
void mtl_pool_address(const void *mem, uint64_t size) {
    if (!mem || !size || !atomic_load_explicit(&g_nlazy_group, memory_order_relaxed) || !vk_pool_lazy(mem, (size_t)size)) return;
    T_lazy_wait++; int n = lazy_back((uintptr_t)mem, (uintptr_t)mem + size); T_lazy_wait--;
    if (n) { static int said; if (said++ < 16) LOGV("lazy pool: GPU-only pool %p+%#llx had its address taken: it got GPU memory", mem, (unsigned long long)size); }
}
#endif
void mtl_pool_add(const void *mem, uint64_t size) { add_range(mem, size, 0); }
/* Pools left without GPU memory on purpose (hle_nvn.c pool_gpu_skip): a buffer lookup that lands in one is reported once
   per pool, which would show a game keeping buffers there. size 0 forgets the pool. */
typedef struct { uintptr_t lo, hi; int flags, said; } skipped_pool;
static skipped_pool g_skipped[512]; static int g_nskipped; static uint64_t g_skipped_bytes;
void mtl_pool_skipped(const void *mem, uint64_t size, int flags) {
    pthread_mutex_lock(&g_pool_mu);
    if (size) { if (g_nskipped < 512) g_skipped[g_nskipped++] = (skipped_pool){(uintptr_t)mem, (uintptr_t)mem + size, flags, 0}; g_skipped_bytes += size; }
    else for (int i = 0; i < g_nskipped; i++) if (g_skipped[i].lo == (uintptr_t)mem) { g_skipped_bytes -= g_skipped[i].hi - g_skipped[i].lo; g_skipped[i] = g_skipped[--g_nskipped]; break; }
    uint64_t total = g_skipped_bytes;
    pthread_mutex_unlock(&g_pool_mu);
    static int said; if (size && (said++ < 3 || size >= (64u << 20)))
        LOGV("pool %p+%#llx flags %#x: no GPU memory (shader code, or compressible and GPU-only; TOTK_OHOS_POOL_SKIP); %llu MiB of such pools", mem, (unsigned long long)size, flags, (unsigned long long)(total >> 20));
}
static void skipped_hit(uintptr_t a) {   /* racy read of a diagnostic table: at worst a missed or stale report */
    for (int i = 0; i < g_nskipped && i < 512; i++) if (a >= g_skipped[i].lo && a < g_skipped[i].hi) {
        if (!g_skipped[i].said++) LOGV("【红】a buffer at %#llx is in pool %#llx+%#llx (flags %#x), which has no GPU memory (TOTK_OHOS_POOL_SKIP=0 gives it some)",
            (unsigned long long)a, (unsigned long long)g_skipped[i].lo, (unsigned long long)(g_skipped[i].hi - g_skipped[i].lo), g_skipped[i].flags);
        return;
    }
}
/* Which 32 MiB units of the first pool of at least 1 GiB the GPU reads as buffers (resolve hits; global memory through the
   pool table is not seen): how much of Ocarina of Time's 2.4 GB CPU pool would need GPU memory if it were given lazily. */
static _Atomic uint64_t g_watch_bits[4];
static void watch_hit(uintptr_t a) { uintptr_t d = a - g_watch_lo; if (d < g_watch_len) { unsigned u = (unsigned)(d >> 25); if (u < 256) atomic_fetch_or_explicit(&g_watch_bits[u >> 6], 1ull << (u & 63), memory_order_relaxed); } }
void mtl_pool_del(const void *mem) { del_range(mem, 1); }
/* Write-back retains a registration identity, never a dereferenceable pool_t. Landing holds this lock through
   its last guest store, so deletion/remapping cannot race it; use exact bounds, not page-rounded GPU backing. */
uint64_t vk_wb_pool_identity(const pool_t *pool, const void *address, uint64_t size) {
    uint64_t identity = 0; uintptr_t a = (uintptr_t)address;
    if (!pool || !pool->mem || !size || size > UINTPTR_MAX - a) return 0;
    pthread_mutex_lock(&g_pool_mu);
    for (int i = g_npool - 1; i >= 0; i--) {
        pool_ent *p = g_pools[i];
        int same = (p->lo == (uintptr_t)pool->mem && p->hi - p->lo == pool->size) || (p->group_size && p->group_lo == (uintptr_t)pool->mem && p->group_size == pool->size);
        if (same && a >= p->lo && a + size <= p->hi) {
            identity = p->identity; break;
        }
    }
    pthread_mutex_unlock(&g_pool_mu);
    return identity;
}
int vk_wb_pool_lock(uint64_t identity, const void *address, uint64_t size) {
    uintptr_t a = (uintptr_t)address;
    if (!identity || !size || size > UINTPTR_MAX - a) return 0;
    pthread_mutex_lock(&g_pool_mu);
    for (int i = 0; i < g_npool; i++) {
        pool_ent *p = g_pools[i];
        if (p->identity == identity && a >= p->lo && a + size <= p->hi) return 1;
    }
    pthread_mutex_unlock(&g_pool_mu);
    return 0;
}
void vk_wb_pool_unlock(void) { pthread_mutex_unlock(&g_pool_mu); }
/* totk_vpool.c: a virtual pool's range, and its page aliases. */
void mtl_vpool_add(const void *mem, uint64_t size) { add_range(mem, size, 1); }
void mtl_vpool_del(const void *mem) { del_range(mem, 1); }
/* MapVirtual accepts overlapping requests in one batch: mmap replaces the old pages. Keep disjoint aliases matching
   that last-writer order, otherwise the alias-first scan and furthest-end index can select different (stale) mappings. */
static void alias_replace(uintptr_t va, uintptr_t end, pool_ent *owner, uint64_t off) {
    if (g_nalias + 2 > g_capalias) { g_capalias = g_capalias ? 2 * g_capalias : 256; g_alias = VK_REALLOC(g_alias, (size_t)g_capalias * sizeof *g_alias); }
    for (int i = 0; i < g_nalias; ) {
        alias_ent x = g_alias[i];
        if (va >= x.end || end <= x.va) { i++; continue; }
        if (va <= x.va && end >= x.end) { g_alias[i] = g_alias[--g_nalias]; continue; }
        if (x.va < va) {
            g_alias[i].end = va;
            if (x.end > end) g_alias[g_nalias++] = (alias_ent){end, x.end, x.owner, x.off + (end - x.va)};
        } else { g_alias[i].va = end; g_alias[i].off += end - x.va; }
        i++;
    }
    g_alias[g_nalias++] = (alias_ent){va, end, owner, off}; span_changed();
}
int mtl_pool_alias(void *dst, const void *src, uint64_t size) {
    mtl_init();
    if (g_pool_mode == POOLS_HOST) return 0;   /* plain shared pages: totk_vpool.c's mremap aliases them */
    pthread_mutex_lock(&g_pool_mu);
    pool_ent *o = own_containing((uintptr_t)src, (uintptr_t)src + size);
    int ok = 0;
    if (o && o->fd >= 0) {
        uint64_t off = (uintptr_t)src - o->base;
#ifdef TOTK_OHOS
        { void ohos_fault_moving(uintptr_t, uintptr_t); ohos_fault_moving((uintptr_t)dst, (uintptr_t)dst + size); }
#endif
        void *alias_at = mmap(dst, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, o->fd, (off_t)off);
#ifdef TOTK_OHOS
        { void ohos_fault_moved(void); ohos_fault_moved(); }
#endif
        if (alias_at == dst) {
            alias_replace((uintptr_t)dst, (uintptr_t)dst + size, o, off);
            ok = 1;
        } else LOGV("【红】virtual pool alias at %p (+%#llx): %s", dst, (unsigned long long)size, strerror(errno));
    }
    pthread_mutex_unlock(&g_pool_mu);
    return ok;
}
#ifdef TOTK_VK_TESTS
/* Replace lookup metadata without remapping memory: donor buffers stay alive throughout the delayed-worker test. */
void vk_record_test_alias(void *address, uint64_t source, uint64_t size) {
    pthread_mutex_lock(&g_pool_mu);
    pool_ent *owner = own_containing(source, source + size);
    if (!owner) DIE("recorder test alias has no donor");
    alias_replace((uintptr_t)address, (uintptr_t)address + size, owner, source - owner->base);
    pthread_mutex_unlock(&g_pool_mu);
}
#endif
static int span_cmp(const void *x, const void *y) { const span_t *a = x, *b = y; return a->lo < b->lo ? -1 : a->lo > b->lo; }
static int span_seg_cmp(const void *x, const void *y) { const span_seg_t *a = x, *b = y; return a->lo < b->lo ? -1 : a->lo > b->lo; }
static int span_further(int a, int b) { return g_span[a].end > g_span[b].end || (g_span[a].end == g_span[b].end && a > b); }
static void span_rebuild(void);
/* Switch 2's memory pool table (vk_int.h vk_gmem_map), as nvn_mtl.m gmem_map_gpu on the Mac: the GPU-backed spans as
   disjoint ranges sorted by start (a span starting inside an earlier one keeps only its part past that one's end: every
   span holding an address gives the same memory), each with its device address minus its guest address, and per 2 MiB
   chunk a hint naming the entry that holds the whole chunk. The CPU image is rebuilt when the pools change (g_span_gen);
   each submission that binds it gets its own copy in its transient memory, so a submission in flight keeps its table. */
/* The write log (offset 1 under the sentinel slot) is only zeroed here, never read. nxnative's translator writes a bitmap of
   table entries (8192 bits); Swap2-Native's a hash set of guest pages, 1 + (1 << 10) words: room for both */
enum { GMEM_ENTRIES = 8192, GMEM_ENTRY = 32, GMEM_HINT_SHIFT = 21, GMEM_LOG_BYTES = 8192 };
static uint8_t *g_gmem_img; static uint64_t g_gmem_img_gen, g_gmem_gen_seen;
static uint64_t g_gmem_sub = ~0ull; static VkDescriptorBufferInfo g_gmem_d, g_gmem_log_d;
uint64_t vk_gmem_generation(void) { return atomic_load_explicit(&g_span_gen, memory_order_acquire); }
/* the last entry in [0, n) whose start is at or below a (entry 0 starts at 0; starts ascend) */
static int gmem_at_or_below(const uint8_t *t, int n, uint64_t a) {
    int lo = 0, hi = n - 1;
    while (lo < hi) { int mid = (lo + hi + 1) / 2; uint64_t s; memcpy(&s, t + (size_t)mid * GMEM_ENTRY, 8); if (s <= a) lo = mid; else hi = mid - 1; }
    return lo;
}
/* oot-a111 fine hints (tools/gmemfast.py FIND_FINE; tools/mesh_selftest/gmem_run.c builds them alike): for each 2 MiB
   chunk below 2^40 that holds the start or the end of a range and has no whole-chunk hint in its slot, word 7 of the slot
   = first | (s + 1) << 16 and word 6 = the chunk's tag with entry 0, where first = the last entry starting at or below the
   chunk's start and 2^s >= (the last entry starting inside the chunk) - first + 1. The translator's helpers read words
   0..6: a tag with entry 0 sends them to their full search, as an empty slot does. A slot already holding a whole-chunk
   hint or another chunk's fine hint keeps it; a bound past the table is not written. TOTK_OHOS_GMEM_FINE=0: none. */
static void gmem_fine(uint8_t *t, int n, const uint8_t *whole, unsigned st[18]) {
    for (int k = 1; k < n; k++) {
        uint64_t r[2]; memcpy(r, t + (size_t)k * GMEM_ENTRY, 16);
        const uint64_t cs[2] = {r[0] >> GMEM_HINT_SHIFT, (r[1] - 1) >> GMEM_HINT_SHIFT};
        for (int j = 0; j < 2; j++) {
            uint64_t c = cs[j];
            if (j && c == cs[0]) continue;
            if (c >= (1ull << 19)) { st[16]++; continue; }
            uint8_t *slot = t + (size_t)(c & (GMEM_ENTRIES - 1)) * GMEM_ENTRY;
            uint32_t w6, w7; memcpy(&w6, slot + 24, 4); memcpy(&w7, slot + 28, 4);
            if (whole[(c & (GMEM_ENTRIES - 1)) >> 3] >> (c & 7) & 1) { if ((w6 >> 13) != (uint32_t)c) st[17]++; continue; }
            if (w7) { if ((w6 >> 13) != (uint32_t)c) st[17]++; continue; }
            int first = gmem_at_or_below(t, n, c << GMEM_HINT_SHIFT), last = gmem_at_or_below(t, n, ((c + 1) << GMEM_HINT_SHIFT) - 1);
            unsigned s = 0; while ((1 << s) < last - first + 1) s++;
            if (first + (1 << s) - 1 > GMEM_ENTRIES - 1) { st[16]++; continue; }
            w6 = (uint32_t)(c << 13); w7 = (uint32_t)first | (s + 1) << 16;
            memcpy(slot + 24, &w6, 4); memcpy(slot + 28, &w7, 4);
            st[s]++;
        }
    }
}
static void gmem_rebuild(void) {
    if (!g_gmem_img) g_gmem_img = malloc((size_t)GMEM_ENTRIES * GMEM_ENTRY);
    uint8_t *t = g_gmem_img;
    memset(t, 0, GMEM_ENTRY);
    for (int k = 1; k < GMEM_ENTRIES; k++) { uint8_t *e = t + (size_t)k * GMEM_ENTRY; memset(e, 0xff, 16); memset(e + 16, 0, 16); }
    int n = 1, over = 0; uintptr_t end = 0;
    static uint8_t whole[GMEM_ENTRIES / 8]; memset(whole, 0, sizeof whole);   /* slots holding a whole-chunk hint */
    unsigned nwhole = 0;
    pthread_mutex_lock(&g_pool_mu);
    if (g_span_dirty) span_rebuild();
    uint64_t gen = atomic_load_explicit(&g_span_gen, memory_order_acquire);
    for (int i = 0; i < g_nspan; i++) {
        const span_t *s = &g_span[i];
        uintptr_t lo = s->lo > end ? s->lo : end;
        if (lo >= s->end || !s->buf) continue;
        if (n == GMEM_ENTRIES) { over++; continue; }
        VkBufferDeviceAddressInfo ai = {VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, NULL, s->buf};
        static PFN_vkGetBufferDeviceAddress get_address;   /* not exported by the loader the port links: the device's entry */
        if (!get_address) { PFN_vkGetDeviceProcAddr gdpa = (PFN_vkGetDeviceProcAddr)gipa(V.inst, "vkGetDeviceProcAddr");
            if (gdpa) get_address = (PFN_vkGetBufferDeviceAddress)gdpa(V.dev, "vkGetBufferDeviceAddress");
            if (gdpa && !get_address) get_address = (PFN_vkGetBufferDeviceAddress)gdpa(V.dev, "vkGetBufferDeviceAddressKHR"); }
        if (!get_address) { RED(2, "raw global memory: the device gives no vkGetBufferDeviceAddress"); break; }
        uint64_t gpu = get_address(V.dev, &ai) + s->off + (lo - s->lo);
        uint64_t e[3] = {lo, s->end, gpu - lo}; memcpy(t + (size_t)n * GMEM_ENTRY, e, sizeof e);
        /* hints: chunks the entry holds whole, below 2^40 (the shader's tag), at entry (chunk & 8191) */
        for (uint64_t c = (lo >> GMEM_HINT_SHIFT) + ((lo & ((1ull << GMEM_HINT_SHIFT) - 1)) != 0); ((c + 1) << GMEM_HINT_SHIFT) <= s->end && c < (1ull << 19); c++) {
            uint32_t h = (uint32_t)(c << 13) | (uint32_t)n; memcpy(t + (size_t)(c & (GMEM_ENTRIES - 1)) * GMEM_ENTRY + 24, &h, 4);
            whole[(c & (GMEM_ENTRIES - 1)) >> 3] |= (uint8_t)(1u << (c & 7)); nwhole++;
        }
        n++; end = s->end;
    }
    pthread_mutex_unlock(&g_pool_mu);
    g_gmem_img_gen = gen;
    static int fine_on = -1; if (fine_on < 0) { const char *e = getenv("TOTK_OHOS_GMEM_FINE"); fine_on = !(e && *e == '0'); }
    unsigned st[18] = {0};
    if (fine_on) gmem_fine(t, n, whole, st);
    if (over) RED(4, "raw global memory: %d more ranges than the %d entries of the memory pool table; addresses in them are not reached", over, GMEM_ENTRIES - 1);
    { static int said; if (said++ < 6 || !(said & 1023)) {
        unsigned nf = 0; char h[160] = ""; size_t o = 0;
        for (int s = 0; s < 16; s++) if (st[s]) { nf += st[s]; o += (size_t)snprintf(h + o, o < sizeof h ? sizeof h - o : 0, " %d:%u", s, st[s]); if (o >= sizeof h) break; }
        LOGV("raw global memory: memory pool table of %d ranges (pool generation %llu); chunk hints: %u whole, %u fine (steps:count%s), %u not written, %u slots taken%s",
             n - 1, (unsigned long long)gen, nwhole, nf, nf ? h : " -", st[16], st[17], fine_on ? "" : " (TOTK_OHOS_GMEM_FINE=0)"); } }
}
int vk_gmem_map(VkDescriptorBufferInfo *out) {
    if (!V.has_bda) return 0;
    if (__builtin_expect(vk_inflight_on, 0)) ic_census();   /* this slot reads pool memory through the table: sample it */
    uint64_t gen = vk_gmem_generation(), sub = vk_submit_serial();
    if (!g_gmem_img || g_gmem_img_gen != gen) { gmem_rebuild(); g_gmem_sub = ~0ull; }
    if (g_gmem_sub != sub) {
        VkBuffer b; VkDeviceSize o;
        void *p = vk_transient((size_t)GMEM_ENTRIES * GMEM_ENTRY, V.props.limits.minStorageBufferOffsetAlignment, &b, &o);
        memcpy(p, g_gmem_img, (size_t)GMEM_ENTRIES * GMEM_ENTRY);
        g_gmem_d = (VkDescriptorBufferInfo){b, o, (VkDeviceSize)GMEM_ENTRIES * GMEM_ENTRY};
        p = vk_transient(GMEM_LOG_BYTES, V.props.limits.minStorageBufferOffsetAlignment, &b, &o);
        memset(p, 0, GMEM_LOG_BYTES);
        g_gmem_log_d = (VkDescriptorBufferInfo){b, o, GMEM_LOG_BYTES};
        g_gmem_sub = sub;
    }
    *out = g_gmem_d;
    return 1;
}
int vk_gmem_log(VkDescriptorBufferInfo *out) {
    VkDescriptorBufferInfo map;
    if (!vk_gmem_map(&map)) return 0;
    if (__builtin_expect(g_ic_k, 0)) ic_gml_mark();   /* in-flight check: this slot's shaders store through the pool table, which no GPU-write record shows */
    *out = g_gmem_log_d;
    return 1;
}
static void span_rebuild(void) {   /* caller holds g_pool_mu */
    int n = g_nalias + g_npool;
    if (n > g_capspan) { g_capspan = n + 256; g_span = VK_REALLOC(g_span, (size_t)g_capspan * sizeof *g_span); g_span_seg = VK_REALLOC(g_span_seg, (size_t)g_capspan * 2 * sizeof *g_span_seg); g_span_heap = VK_REALLOC(g_span_heap, (size_t)g_capspan * sizeof *g_span_heap); }
    g_nspan = 0;
    for (int i = 0; i < g_nalias; i++) { const alias_ent *x = &g_alias[i]; g_span[g_nspan++] = (span_t){x->va, x->end, x->owner->buf, x->off}; }
    for (int i = 0; i < g_npool; i++) {
        const pool_ent *p = g_pools[i];
        if (p->kind == K_OWN) g_span[g_nspan++] = (span_t){p->base, p->end, p->buf, 0};
        else if (p->kind == K_VIEW) g_span[g_nspan++] = (span_t){p->base, p->end, p->owner->buf, p->off};
    }
    qsort(g_span, (size_t)g_nspan, sizeof *g_span, span_cmp);
    for (int i = 0; i < g_nspan; i++) { g_span_seg[2 * i] = (span_seg_t){g_span[i].lo, -1}; g_span_seg[2 * i + 1] = (span_seg_t){g_span[i].end, -1}; }
    qsort(g_span_seg, (size_t)g_nspan * 2, sizeof *g_span_seg, span_seg_cmp);
    g_nseg = 0;
    for (int i = 0; i < 2 * g_nspan; i++) if (!g_nseg || g_span_seg[i].lo != g_span_seg[g_nseg - 1].lo) g_span_seg[g_nseg++].lo = g_span_seg[i].lo;
    /* Heap order matches the old backward scan exactly: largest end, then highest sorted span index. */
    int next = 0, nh = 0;
    for (int i = 0; i < g_nseg; i++) {
        uintptr_t x = g_span_seg[i].lo;
        while (next < g_nspan && g_span[next].lo <= x) {
            int v = next++, h = nh++;
            while (h && span_further(v, g_span_heap[(h - 1) / 2])) { g_span_heap[h] = g_span_heap[(h - 1) / 2]; h = (h - 1) / 2; }
            g_span_heap[h] = v;
        }
        while (nh && g_span[g_span_heap[0]].end <= x) {
            int v = g_span_heap[--nh], h = 0;
            while (2 * h + 1 < nh) {
                int c = 2 * h + 1;
                if (c + 1 < nh && span_further(g_span_heap[c + 1], g_span_heap[c])) c++;
                if (!span_further(g_span_heap[c], v)) break;
                g_span_heap[h] = g_span_heap[c]; h = c;
            }
            if (nh) g_span_heap[h] = v;
        }
        g_span_seg[i].best = nh ? g_span_heap[0] : -1;
    }
    /* Nested views often add endpoints without changing the furthest-reaching
       winner. Remove those non-boundaries: lookup and hint validation need only
       the maximal intervals over which that exact winning span stays the same. */
    int merged = 0;
    for (int i = 0; i < g_nseg; i++)
        if (!merged || g_span_seg[merged - 1].best != g_span_seg[i].best)
            g_span_seg[merged++] = g_span_seg[i];
    g_nseg = merged;
    g_span_dirty = 0;
}
/* A hint names an elementary segment, not the whole winning span: a nested span can win in only part of that pool. */
static int span_hint_matches(const span_seg_t *seg, int nseg, uintptr_t a, int hint) {
    return hint > 0 && hint < nseg && seg[hint - 1].lo <= a && a < seg[hint].lo;
}
static const span_t *span_best(const span_t *s, const span_seg_t *seg, int nseg, uintptr_t a, uintptr_t b, int *hint) {
    int lo = hint ? *hint : 0;
    if (!hint || !span_hint_matches(seg, nseg, a, lo)) {
        int hi = nseg; lo = 0; while (lo < hi) { int m = (lo + hi) >> 1; if (seg[m].lo <= a) lo = m + 1; else hi = m; }
        if (hint) *hint = lo;
    }
    if (!lo || seg[lo - 1].best < 0) return NULL;
    const span_t *best = &s[seg[lo - 1].best];
    return best->end >= b ? best : NULL;   /* if the furthest span containing a cannot reach b, none can */
}
static int span_linear(void) { static int linear = -1; if (linear < 0) linear = vk_off("span"); return linear; }
/* Which buffer holds [addr, addr + len), and how many bytes from addr it holds (*avail): caller holds g_pool_mu. */
static int resolve_span(uint64_t addr, uint64_t len, VkBuffer *buf, uint64_t *off, uint64_t *avail) {
    uintptr_t a = (uintptr_t)addr, b = a + len;
    if (b < a) return 0;
    if (span_linear()) {   /* the old scan (TOTK_VK_OFF=span) */
        for (int i = 0; i < g_nalias; i++) { const alias_ent *x = &g_alias[i]; if (a >= x->va && b <= x->end) { *buf = x->owner->buf; *off = x->off + (a - x->va); if (avail) *avail = x->end - a; return 1; } }
        for (int i = 0; i < g_npool; i++) {
            const pool_ent *p = g_pools[i];
            if (a < p->base || b > p->end) continue;
            if (p->kind == K_OWN) { *buf = p->buf; *off = a - p->base; if (avail) *avail = p->end - a; return 1; }
            if (p->kind == K_VIEW) { *buf = p->owner->buf; *off = p->off + (a - p->base); if (avail) *avail = p->end - a; return 1; }
        }
        return 0;
    }
    if (g_span_dirty) span_rebuild();
    const span_t *best = span_best(g_span, g_span_seg, g_nseg, a, b, NULL);
    if (!best) return 0;
    *buf = best->buf; *off = best->off + (a - best->lo); if (avail) *avail = best->end - a;
    return 1;
}
static int resolve(uint64_t addr, uint64_t len, VkBuffer *buf, uint64_t *off) { return resolve_span(addr, len, buf, off, NULL); }
/* The public lookups use the calling thread's own copy of the span index, refreshed when the pools change (rarely):
   the recording thread resolves several addresses per draw, and taking g_pool_mu for each (contended by the game's
   threads) was a tenth of its time. The lock never kept a buffer alive past the lookup, so a copy is as safe. */
enum { SPAN_HINTS = 64 };
typedef struct { uint64_t gen; span_t *s; span_seg_t *seg; int nseg, cap, hint[SPAN_HINTS]; } span_cache_t;
static __thread span_cache_t T_span;
/* Diagnostics count only the replay thread (including its snapshot collection), not concurrent game-thread lookups.
   Atomic counters allow present to report them even when async replay records on another thread. */
static _Atomic uint64_t g_lookup_calls[4], g_lookup_hits, g_lookup_tests;
static void lookup_count(int kind, const span_cache_t *t, uintptr_t a, int hint, int indexed) {
    if ((uintptr_t)pthread_self() != atomic_load_explicit(&g_lookup_thread, memory_order_relaxed)) return;
    atomic_fetch_add_explicit(&g_lookup_calls[kind], 1, memory_order_relaxed);
    if (indexed) {
        atomic_fetch_add_explicit(&g_lookup_tests, 1, memory_order_relaxed);
        if (span_hint_matches(t->seg, t->nseg, a, hint)) atomic_fetch_add_explicit(&g_lookup_hits, 1, memory_order_relaxed);
    }
}
void vk_lookup_stats(uint64_t frame) {
    if (!g_lookup_stats) return;
    static uint64_t last; uint64_t frames = frame - last; last = frame; if (!frames) return;
    uint64_t c[4]; for (int i = 0; i < 4; i++) c[i] = atomic_exchange_explicit(&g_lookup_calls[i], 0, memory_order_relaxed);
    uint64_t hit = atomic_exchange_explicit(&g_lookup_hits, 0, memory_order_relaxed), tests = atomic_exchange_explicit(&g_lookup_tests, 0, memory_order_relaxed);
    LOGV("frame %llu, pool lookups per frame: ubo/sbuf %.1f, vbuf/index %.1f, draw_touch %.1f, other %.1f; segment hints %.1f%% (%llu/%llu)",
         (unsigned long long)frame, (double)c[0] / frames, (double)c[1] / frames, (double)c[2] / frames, (double)c[3] / frames,
         tests ? 100.0 * hit / tests : 0, (unsigned long long)hit, (unsigned long long)tests);
}
static int resolve_local(uint64_t addr, uint64_t len, VkBuffer *buf, uint64_t *off, uint64_t *avail, int kind) {
    uintptr_t a = (uintptr_t)addr, b = a + len;
    if (b < a) return 0;
    if (span_linear()) { if (g_lookup_stats) lookup_count(kind, NULL, a, 0, 0); pthread_mutex_lock(&g_pool_mu); int ok = resolve_span(addr, len, buf, off, avail); pthread_mutex_unlock(&g_pool_mu); return ok; }
    span_cache_t *t = &T_span;   /* one dynamic TLS resolution per lookup */
    if (t->gen != atomic_load_explicit(&g_span_gen, memory_order_acquire)) {
        pthread_mutex_lock(&g_pool_mu);
        if (g_span_dirty) span_rebuild();
        if (g_nspan > t->cap) { t->cap = g_nspan + 256; t->s = VK_REALLOC(t->s, (size_t)t->cap * sizeof *t->s); t->seg = VK_REALLOC(t->seg, (size_t)t->cap * 2 * sizeof *t->seg); }
        memcpy(t->s, g_span, (size_t)g_nspan * sizeof *g_span); memcpy(t->seg, g_span_seg, (size_t)g_nseg * sizeof *g_span_seg);
        t->nseg = g_nseg; memset(t->hint, 0, sizeof t->hint); t->gen = atomic_load_explicit(&g_span_gen, memory_order_relaxed);
        pthread_mutex_unlock(&g_pool_mu);
    }
    /* One last-segment hint thrashes between a draw's uniform, vertex and index pools: 69% of resolve_local samples at
       Lookout Landing were still in the binary search. Keep page-hashed hints; collisions only fall back to that search. */
    unsigned h = (unsigned)((a >> 12) ^ (a >> 24)) & (SPAN_HINTS - 1);
    if (g_lookup_stats) lookup_count(kind, t, a, t->hint[h], 1);
    const span_t *best = span_best(t->s, t->seg, t->nseg, a, b, &t->hint[h]);
    if (!best) {
        if (atomic_load_explicit(&g_nlazy_group, memory_order_relaxed) && lazy_back(a, b)) return resolve_local(addr, len, buf, off, avail, kind);
        if (g_nskipped) skipped_hit(a);
        return 0;
    }
    if (g_watch_len) watch_hit(a);
    *buf = best->buf; *off = best->off + (a - best->lo); if (avail) *avail = best->end - a;
    return 1;
}
/* Recorder snapshots never refresh from live globals on a worker. Keep replaced versions until slot reuse. */
struct vk_record_spans {
    struct vk_record_spans *next; uint64_t generation, bias_bits, dirty_epoch;
    span_t *span; span_seg_t *segment; int count, segments, linear;
    uint32_t *buffer_heads, *buffer_next, buffer_mask;
};
uint64_t vk_record_spans_generation(const vk_record_spans *spans) { return spans ? spans->generation : 0; }
static vk_record_spans *g_record_spans[32];   /* by ring cell: RING_MAX (the assert is at its definition) */
static uint64_t g_record_index_ns, g_record_index_versions;
static unsigned record_buffer_hash(VkBuffer buffer) {
    uint64_t x = (uint64_t)(uintptr_t)buffer; x ^= x >> 33; x *= 0xff51afd7ed558ccdull; x ^= x >> 33;
    return (unsigned)x;
}
/* Keep all mappings, including lookup losers. O(n) construction matters when alias versions churn during a walk. */
static int record_spans_index(vk_record_spans *s) {
    if (!s->count) return 1;
    size_t cap = 1;
    while (cap < (size_t)s->count * 2) cap *= 2;
    if (cap > UINT32_MAX || cap > SIZE_MAX / sizeof *s->buffer_heads - (size_t)s->count) return 0;
    s->buffer_heads = malloc((cap + (size_t)s->count) * sizeof *s->buffer_heads);
    if (!s->buffer_heads) return 0;
    memset(s->buffer_heads, 0, cap * sizeof *s->buffer_heads);
    s->buffer_next = s->buffer_heads + cap; /* one allocation per immutable version; links need no zeroing */
    s->buffer_mask = (uint32_t)cap - 1;
    for (int i = s->count; i-- > 0; ) {
        unsigned h = record_buffer_hash(s->span[i].buf) & s->buffer_mask;
        s->buffer_next[i] = s->buffer_heads[h]; s->buffer_heads[h] = (uint32_t)i + 1;
    }
    return 1;
}
const vk_record_spans *vk_record_spans_capture(unsigned slot) {
    int lag = lag_wait_on(); /* initialize before the first frozen version can be reused by a lag packet */
    vk_record_spans *s = g_record_spans[slot];
    if (s && s->generation == atomic_load_explicit(&g_span_gen, memory_order_acquire)) return s;
    pthread_mutex_lock(&g_pool_mu);
    s = VK_CALLOC(1, sizeof *s); s->generation = atomic_load_explicit(&g_span_gen, memory_order_relaxed); s->linear = span_linear();
    if (lag) s->dirty_epoch = nvn_dirty_lag_epoch(); /* no per-span dirty-table lookups, and no arming */
    if (s->linear) {
        s->span = VK_MALLOC((size_t)(g_nalias + g_npool) * sizeof *s->span);
        for (int i = 0; i < g_nalias; i++) { const alias_ent *a = &g_alias[i]; s->span[s->count++] = (span_t){a->va, a->end, a->owner->buf, a->off}; }
        for (int i = 0; i < g_npool; i++) { const pool_ent *p = g_pools[i];
            if (p->kind == K_OWN) s->span[s->count++] = (span_t){p->base, p->end, p->buf, 0};
            else if (p->kind == K_VIEW) s->span[s->count++] = (span_t){p->base, p->end, p->owner->buf, p->off};
        }
    } else {
        if (g_span_dirty) span_rebuild();
        s->count = g_nspan; s->segments = g_nseg;
        s->span = VK_MALLOC((size_t)g_nspan * sizeof *s->span); s->segment = VK_MALLOC((size_t)g_nseg * sizeof *s->segment);
        if (g_nspan) memcpy(s->span, g_span, (size_t)g_nspan * sizeof *s->span);
        if (g_nseg) memcpy(s->segment, g_span_seg, (size_t)g_nseg * sizeof *s->segment);
    }
    for (int i = 0; i < s->count; i++) s->bias_bits |= s->span[i].off - s->span[i].lo;
    if (lag) {
        uint64_t start = host_now_ns();
        if (!record_spans_index(s)) {
            free(s->buffer_heads); s->buffer_heads = s->buffer_next = NULL;
        }   /* projection reports failure, never silently drops aliases or scans every span */
        g_record_index_ns += host_now_ns() - start; g_record_index_versions++;
    }
    pthread_mutex_unlock(&g_pool_mu);
    s->next = g_record_spans[slot]; g_record_spans[slot] = s; return s;
}
void vk_record_spans_reset(unsigned slot) {
    vk_record_spans *keep = g_record_spans[slot]; if (!keep) return;
    vk_record_spans *s = keep->next; keep->next = NULL;
    while (s) {
        vk_record_spans *next = s->next; free(s->span); free(s->segment);
        free(s->buffer_heads); free(s); s = next;
    }
}
int vk_record_storage_aligned(const vk_record_spans *s, uint64_t address) {
    uint64_t align = V.props.limits.minStorageBufferOffsetAlignment;
    return !(address % align) && !(s->bias_bits % align);
}
static int record_resolve_inner(const vk_record_spans *s, uint64_t a, uint64_t n, VkBuffer *b, uint64_t *o, uint64_t *available, int hints[64]) {
    if (a + n < a) return 0;
    const span_t *best = NULL;
    if (s->linear) { for (int i = 0; i < s->count; i++) if (s->span[i].lo <= a && s->span[i].end >= a + n) { best = &s->span[i]; break; } }
    else { unsigned h = (unsigned)((a >> 12) ^ (a >> 24)) & 63u; best = span_best(s->span, s->segment, s->segments, a, a + n, hints ? &hints[h] : NULL); }
    if (!best) {   /* a lazy pool unit: backed now, in the snapshots captured from here on (this draw misses it once) */
        if (atomic_load_explicit(&g_nlazy_group, memory_order_relaxed)) lazy_back((uintptr_t)a, (uintptr_t)(a + n));
        return 0;
    }
    *b = best->buf; *o = best->off + a - best->lo; if (available) *available = best->end - a; return 1;
}
int vk_record_resolve(const vk_record_spans *s, uint64_t a, uint64_t n, VkBuffer *b, uint64_t *o, uint64_t *available, int hints[64]) {
    return VK_REC_VALUE(VK_REC_RESOLVE, record_resolve_inner(s, a, n, b, o, available, hints));
}
int vk_resolve_for(uint64_t addr, uint64_t len, VkBuffer *buf, uint64_t *off, uint64_t *avail, int kind) { return VK_REC_VALUE(VK_REC_RESOLVE, resolve_local(addr, len, buf, off, avail, kind)); }
int vk_resolve_span(uint64_t addr, VkBuffer *buf, uint64_t *off, uint64_t *avail) { return vk_resolve_for(addr, 1, buf, off, avail, VK_LOOKUP_OTHER); }
int vk_resolve(uint64_t addr, uint64_t len, VkBuffer *buf, uint64_t *off) { return vk_resolve_for(addr, len, buf, off, NULL, VK_LOOKUP_OTHER); }
uint64_t vk_lag_bound(uint64_t addr, uint64_t limit) {
    enum { NB = 1024 };
    static struct { uint64_t addr, gen, bound; } cache[NB];
    uint64_t gen = atomic_load_explicit(&g_span_gen, memory_order_acquire);
    unsigned h = (unsigned)((addr >> 4) ^ (addr >> 20)) % NB;
    if (cache[h].addr == addr && cache[h].gen == gen) return limit < cache[h].bound ? limit : cache[h].bound;
    pthread_mutex_lock(&g_pool_mu);
    uint64_t physical = addr, bound = UINT64_MAX;
    for (int i = 0; i < g_nalias; i++) if (addr >= g_alias[i].va && addr < g_alias[i].end) {
        physical = g_alias[i].owner->base + g_alias[i].off + addr - g_alias[i].va;
        bound = g_alias[i].end - addr; break;
    }
    for (int i = 0; i < g_npool; i++) {
        const pool_ent *p = g_pools[i];
        if ((p->kind == K_OWN || p->kind == K_VIEW) && physical >= p->lo && physical < p->hi && p->hi - physical < bound) bound = p->hi - physical;
    }
    gen = atomic_load_explicit(&g_span_gen, memory_order_relaxed);
    pthread_mutex_unlock(&g_pool_mu);
    cache[h].addr = addr; cache[h].gen = gen; cache[h].bound = bound;
    return limit < bound ? limit : bound;
}
int mtl_addr(uint64_t addr, uint64_t len, void **buf, uint64_t *off) {
    VkBuffer b; int ok = resolve_local(addr, len, &b, off, NULL, VK_LOOKUP_OTHER);
    if (ok) *buf = (void *)(uintptr_t)b;
    return ok;
}
/* mtl_pool_invalidate: vk_res.c (GPU write-back of renderings into CPU-pool textures) */
void gpu_pool_totals(uint64_t *n, uint64_t *bytes) { pthread_mutex_lock(&g_pool_mu); *n = (uint64_t)g_npool; *bytes = g_pool_bytes; pthread_mutex_unlock(&g_pool_mu); }

/* ---------- submissions ----------
   A submission is recorded into the "pre" and "main" command buffers of one slot of a small ring (vk_int.h). Its
   CPU-visible results (post ops) are applied by the completion thread once the slot's VkFence signals, in submission
   order, and only then is its fence value done. A binary fence per slot, not a timeline semaphore: Turnip on KGSL emulates
   timeline semaphores (Mesa vk_sync_timeline), and signalling one in vkQueueSubmit2 garbage-collects its points by waiting
   for the GPU, which held the recording thread about 30 ms a frame (half its time) and serialised CPU and GPU. Work the GPU
   cannot do (an address outside every pool) is done on the CPU at its place in the order: what is recorded so far is
   submitted and waited for first ("CPU step"). A slot's transient memory and descriptor sets are recycled when it is begun
   again (after completion). */
enum { OQ = 256 };
typedef struct occl_node { struct occl_node *prev; uint64_t total; unsigned refs; int ready; } occl_node;
typedef struct occl_page { struct occl_page *next; VkQueryPool qp; uint32_t used; occl_node *node[OQ]; } occl_page;
typedef struct { int kind; volatile uint32_t *val; uint32_t mode, v; uint64_t addr, type, cval; uint32_t tsq1; occl_node *occl; uint64_t report_ticks, report_origin; int pair_end; } post_op;   /* 1 event, 2 counter; cval fixed at recording, tsq1: timestamp query + 1; report provenance staged until g_mu publication */
typedef struct { VkBuffer buf; VkDeviceMemory mem; uint8_t *ptr; VkDeviceSize size, used; } chunk_t;
typedef struct { chunk_t *chunks; int nchunk, capchunk; VkDeviceSize min, reserve; } arena_t;   /* host-visible bump allocator; min: chunk size (0: 8 MiB), reserve: compacted high-water */
static uint64_t arena_compact_ns[2], arena_compacts[2]; /* walking only: slot / snapshot */
/* The ring's length. 8 as in the Android port (a game of two or three submissions a frame). Ocarina of Time makes about 21
   slots a frame and its heaviest slot is sealed late, so with 8 the walker cannot record the next frame's scene while the
   GPU runs this one's. TOTK_VK_RING=<4..32> (oot-a102, an experiment: default 8); read once, before the first slot. */
enum { RING_MAX = 32 };   /* vk_record.c REC_RING and g_record_spans above are this long too */
_Static_assert(RING_MAX == 32, "g_record_spans and vk_record.c REC_RING are sized for 32 ring cells");
static unsigned g_ring_len = 8; static pthread_once_t g_ring_once = PTHREAD_ONCE_INIT;
static void ring_parse(void) {
    const char *e = getenv("TOTK_VK_RING"); long n = e && *e ? atol(e) : 8;
    g_ring_len = n < 4 ? 4u : n > RING_MAX ? (unsigned)RING_MAX : (unsigned)n;
    if (g_ring_len != 8) LOGV("submission ring: %u slots (TOTK_VK_RING; 8 by default)", g_ring_len);
}
static unsigned ring_len(void) { pthread_once(&g_ring_once, ring_parse); return g_ring_len; }
#define RING ring_len()
typedef struct {
    VkCommandPool pool; VkCommandBuffer cb, pre; int pre_open;
    VkCommandBuffer adopted; /* closed, externally owned; never reset/freed by this slot */
    VkCommandBuffer *primary; VkCommandBufferSubmitInfo *submit; uint32_t nprimary, capprimary, capsubmit;
    uint32_t primary_trim_uses, recent_primary_peak;
    uint64_t value; post_op *post; int npost, cap; int busy; VkFence fence;
    int publication, join_on_walk, native_lag, submit_busy;
    uint64_t ft_frame, ft_gpu_done; /* walking-captured owner; fence observation published by the GPU cursor */
    uint64_t guest_submit_ns, guest_origin_ticks, guest_frame, queue_ns;
    uint32_t tsq_first, tsq_count;
    nvn_dirty_lag_batch walking_guards; /* walking-built, immutable until this slot's queue publication */
    arena_t ar;   /* transient memory */
    VkDescriptorPool *dpools; int ndpool, capdpool, curdpool, curdsets;   /* walk every reset pool before allocating another */
    vk_desc_stock *dstock;   /* TOTK_VK_DESC_STOCK=1: per-layout sets kept across this slot's generations */
    int dstock_on;           /* this slot's stock policy, switched only in slot_descriptors_recycle */
    VkQueryPool ts; int ts_on;   /* GPU timestamps at the start and end of the main command buffer */
    occl_page *oq, *oq_cur;   /* reset/reused only after this slot's fence and occl_complete */
    vk_gpuprof_slot *gpuprof;   /* query pool and records, reused only after the slot fence */
} slot_t;
static void chunk_release(chunk_t *c, vk_memory_purpose purpose) {
    vk_res_buffer_released(c->buf);
    vkUnmapMemory(V.dev, c->mem);
    vkDestroyBuffer(V.dev, c->buf, NULL);
    vk_memory_release(c->mem, purpose);
}
static void arena_release(arena_t *a) {
    for (int i = 0; i < a->nchunk; i++) chunk_release(&a->chunks[i], a->min ? VK_MEMORY_SNAPSHOT : VK_MEMORY_SLOT);
    a->nchunk = 0; a->reserve = 0;
}
/* Only called after the arena's last GPU fence AND CPU recording ownership
   have retired. Retain byte capacity, but collapse fragmented BO high-water
   once; steady-state reuse never allocates/frees merely to shrink memory. */
static void arena_reset(arena_t *a, int max_bos) {
    if (a->nchunk > max_bos) {
        uint64_t start = host_now_ns();
        VkDeviceSize reserve = 0;
        for (int i = 0; i < a->nchunk; i++) reserve += a->chunks[i].size;
        arena_release(a);
        a->reserve = reserve;
        unsigned kind = a->min != 0;
        arena_compacts[kind]++; arena_compact_ns[kind] += host_now_ns() - start;
    } else for (int i = 0; i < a->nchunk; i++) a->chunks[i].used = 0;
}
/* TOTK_VK_STAGING_RETAIN_MB (default unset: no cap). A slot's transient arena holds the staging of its texture uploads
   (vk_res.c), and arena_reset keeps whatever a submission needed, compacted into one high-water chunk: a burst of
   uploads at an area transition stays allocated in that slot for the session. With N, each slot keeps at most N MiB
   after its reset (whole chunks, oldest first, and a high-water hint clipped to N); a later submission that needs more
   allocates it again. Live (totk.ctl) at each slot open, after the slot's fence. */
static int g_staging_retain_launch = -1;   /* MiB, -1: no cap; parsed once (pthread_once) */
static _Atomic uint64_t g_staging_trims, g_staging_trimmed;
static pthread_once_t g_staging_once = PTHREAD_ONCE_INIT;
static void staging_retain_parse(void) {
    const char *e = getenv("TOTK_VK_STAGING_RETAIN_MB");
    if (e && *e) { long n = atol(e); g_staging_retain_launch = n < 8 ? 8 : n > 4096 ? 4096 : (int)n; }
#ifdef TOTK_GAME_OOT
    /* Ocarina of Time: 16 MiB a slot by default. Its 8 slots kept 224 MiB of upload staging for the session, while the game
       plus the system left 1.1 GiB available on a 20 GiB phone and the system killed the process (oot-a72: with the cap the
       GPU memory in Kokiri Forest's gate view was 4.1 GiB instead of 4.3). TOTK_VK_STAGING_RETAIN_MB=4096: as before. */
    else g_staging_retain_launch = 16;
#endif
    /* a longer ring (TOTK_VK_RING) keeps the same total: the value in force is scaled, whether it came from the variable
       (the launcher sets 32) or from the default. Not below 8 MiB, a slot arena's smallest chunk: a smaller cap would
       release the slot's only chunk at every slot open (oot-a103). */
    if (RING > 8 && g_staging_retain_launch > 0) { int v = g_staging_retain_launch * 8 / (int)RING; g_staging_retain_launch = v < 8 ? 8 : v; }
    if (g_staging_retain_launch < 0) LOGV("staging retention: no cap (TOTK_VK_STAGING_RETAIN_MB unset: each slot keeps its upload high-water)");
    else LOGV("staging retention: %d MiB per slot (TOTK_VK_STAGING_RETAIN_MB; %d slots)", g_staging_retain_launch, (int)RING);
}
static int staging_retain_mb(void) {
    static totk_live_cache live; static _Atomic int reported = -2;
    pthread_once(&g_staging_once, staging_retain_parse);
    int mb = totk_live_cached(&live, "TOTK_VK_STAGING_RETAIN_MB", g_staging_retain_launch);
    if (atomic_load_explicit(&reported, memory_order_relaxed) != mb &&
        atomic_exchange_explicit(&reported, mb, memory_order_relaxed) != mb && totk_live_generation())
        LOGV("staging retention: %d MiB per slot (live; -1 = no cap; each slot applies it at its next open)", mb);
    return mb;
}
static void arena_retain_cap(arena_t *a) {   /* a slot arena just reset: no chunk is in use */
    int mb = staging_retain_mb(); if (mb < 0) return;
    VkDeviceSize cap = (VkDeviceSize)mb << 20, kept = 0, trimmed = 0;
    if (a->reserve > cap) { trimmed += a->reserve - cap; a->reserve = cap; }
    int n = 0;
    for (int i = 0; i < a->nchunk; i++) {
        if (kept + a->chunks[i].size <= cap) { kept += a->chunks[i].size; a->chunks[n++] = a->chunks[i]; continue; }
        trimmed += a->chunks[i].size; chunk_release(&a->chunks[i], VK_MEMORY_SLOT);
    }
    a->nchunk = n;
    if (trimmed) {
        atomic_fetch_add_explicit(&g_staging_trims, 1, memory_order_relaxed);
        atomic_fetch_add_explicit(&g_staging_trimmed, trimmed, memory_order_relaxed);
    }
}
#ifdef TOTK_OHOS
/* HarmonyOS: the same cap for the snapshot arenas of protected lag (TOTK_FRAME_LAG=1), which kept 1.09 GB of copies after
   a few area loads: the system kills the process at about 3.2 GB of GPU-driver memory (memmgr GPU_LEAK_KILL), whatever
   the phone's RAM. TOTK_OHOS_SNAPSHOT_RETAIN_MB per arena (4 arenas; default 64; 0 = no cap). Called after arena_reset. */
static void arena_cap_bytes(arena_t *a, VkDeviceSize cap, vk_memory_purpose purpose) {
    VkDeviceSize kept = 0, trimmed = 0; int n = 0;
    if (a->reserve > cap) { trimmed += a->reserve - cap; a->reserve = cap; }
    for (int i = 0; i < a->nchunk; i++) {
        if (kept + a->chunks[i].size <= cap) { kept += a->chunks[i].size; a->chunks[n++] = a->chunks[i]; continue; }
        trimmed += a->chunks[i].size; chunk_release(&a->chunks[i], purpose);
    }
    a->nchunk = n;
    if (trimmed) { atomic_fetch_add_explicit(&g_staging_trims, 1, memory_order_relaxed); atomic_fetch_add_explicit(&g_staging_trimmed, trimmed, memory_order_relaxed); }
}
static VkDeviceSize snapshot_retain_cap(void) {
    static VkDeviceSize cap = 1; static int parsed;
    if (!parsed) { const char *e = getenv("TOTK_OHOS_SNAPSHOT_RETAIN_MB"); long mb = e && *e ? atol(e) : 64; cap = mb <= 0 ? 0 : (VkDeviceSize)(mb > 4096 ? 4096 : mb) << 20; parsed = 1; }
    return cap;
}
#endif
static slot_t g_ring[RING_MAX]; static int g_ring_next;
#ifdef TOTK_VK_TESTS
uint64_t vk_test_slot_arena_bytes(void) {   /* walking owner: every slot arena's chunk bytes */
    uint64_t bytes = 0;
    for (int i = 0; i < RING; i++) for (int c = 0; c < g_ring[i].ar.nchunk; c++) bytes += g_ring[i].ar.chunks[c].size;
    return bytes;
}
#endif
/* Separate from the ordinary slot layout and never consulted before an
 * external engine has passed its runtime admission checks. */
static struct { VkSemaphore signal, wait; uint64_t until; } g_external[RING_MAX];
static _Atomic int g_external_enabled;
void vk_submit_external_enable(void) { atomic_store_explicit(&g_external_enabled,1,memory_order_release); }
static uint64_t g_primary_reclaimed;
static void primary_memory(uint64_t values[2]) {
    values[0] = 0; values[1] = g_primary_reclaimed;
    for (int i = 0; i < RING; i++) values[0] += g_ring[i].capprimary ? g_ring[i].capprimary : !!g_ring[i].pool;
}
#ifdef TOTK_VK_TESTS
void vk_record_test_primary_memory(uint64_t values[2]) { primary_memory(values); }
void vk_record_test_arena_costs(vk_record_test_costs *out) {
    memcpy(out->arena_ns, arena_compact_ns, sizeof arena_compact_ns);
    memcpy(out->arena_compacts, arena_compacts, sizeof arena_compacts);
}
#endif
static slot_t *g_cur;
int vk_slot_index(void) { return g_cur ? (int)(g_cur - g_ring) : -1; }
static slot_t *g_optional_next; /* recorder-owned preclaimed external continuation */
static post_op *g_carry; static int g_ncarry, g_capcarry;
static uint64_t g_fence, g_reserved_fence; /* g_mu: actual queue publication versus walking reservations */
static _Atomic uint64_t g_done;
static uint64_t g_ub_publication;   /* g_mu: final ticket whose CPU-known UBO bytes must publish before posts/done */
static uint64_t g_done_at;   /* g_mu: publication time, sampled only by a waiter which actually blocked */
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER, g_cv_work = PTHREAD_COND_INITIALIZER;
/* Predict/auto exact-ticket history. Sampled independently of presenter FIFO
 * residence, AFTER CPU posts/publication prerequisites, at g_done availability.
 * This is a CPU observation, not the GPU's hardware signal timestamp. */
enum { PRESENT_READY_HISTORY = 1024 };
static struct { uint64_t ticket, ready_ns; } g_present_ready[PRESENT_READY_HISTORY];
uint64_t vk_present_ready_ns(uint64_t ticket) {
    if (!ticket) return 0;
    pthread_mutex_lock(&g_mu);
    unsigned index = (unsigned)(ticket % PRESENT_READY_HISTORY);
    uint64_t ready = g_present_ready[index].ticket == ticket ? g_present_ready[index].ready_ns : 0;
    pthread_mutex_unlock(&g_mu);
    return ready; /* frontend feedback must happen only AFTER releasing g_mu */
}
static slot_t *g_queue_[RING_MAX]; static int g_qhead, g_qtail;
static int g_gpu_head;   /* guard thread's GPU-only completion cursor; CPU post ops may wait on later GPU reads */
/* CPU handoff and GPU completion have separate cursors: a reserved ticket is not yet queue-visible. */
static slot_t *g_sealed[RING_MAX]; static uint64_t g_sealed_head, g_sealed_tail, g_submit_done_value, g_fallback_value;
static int g_submit_started;
static pthread_cond_t g_sealed_cv = PTHREAD_COND_INITIALIZER;
static _Atomic unsigned g_replay_pins; /* g_mu: distinct owners, bounded by RING-1 */
static slot_t *g_replay_pending; /* walking owner; at most one unsealed continuation */
/* Independent of external-engine admission: a terminal draw may retain the
 * real recorder's bindings across several split/continuation pairs. */
static struct { uint64_t token, until; unsigned snapshots; int pending; } g_replay_owner[RING_MAX];
static uint64_t g_replay_token;
static int g_optional_immutable;
static unsigned g_replay_releases; /* walking owner: releases awaiting an ordinary seal */
static void snapshot_replay_pin(uint64_t until);
static unsigned snapshot_replay_mask(void);
static uint64_t replay_until(unsigned slot) { /* g_mu */
    uint64_t split=g_external[slot].until, owner=g_replay_owner[slot].until;
    return split>owner ? split : owner;
}
static void replay_retire(uint64_t done) { /* g_mu; also called after submit-owner retirement */
    if (!g_replay_pins) return;
    for (unsigned i=0;i<RING;i++) {
        if (g_ring[i].busy || g_ring[i].submit_busy || !replay_until(i)) continue;
        if (g_external[i].until && g_external[i].until<=done) g_external[i].until=0;
        if (g_replay_owner[i].until && g_replay_owner[i].until<=done) {
            g_replay_owner[i].until=0; g_replay_owner[i].snapshots=0;
        }
        if (!replay_until(i)) { g_replay_pins--; vk_record_slot_retire(i); }
    }
}
int vk_submit_external_disable(void) {
    if (!atomic_load_explicit(&g_external_enabled,memory_order_acquire)) return 1;
    pthread_mutex_lock(&g_mu);
    int idle=!g_replay_pins && !g_replay_pending;
    for (unsigned i=0;idle && i<RING;i++)
        idle=!g_external[i].until && !g_external[i].signal && !g_external[i].wait;
    if (idle) atomic_store_explicit(&g_external_enabled,0,memory_order_release);
    pthread_mutex_unlock(&g_mu);
    return idle; /* no GPU wait: retry after genuine continuation retirement */
}
#ifdef TOTK_VK_TESTS
static uint64_t g_test_replay_skips; /* g_mu */
static uint64_t g_test_completion_hold; /* g_mu; actual GPU fence precedes this gate */
static _Atomic uint64_t g_test_completion_held;
#endif
static int g_in_sub;
static __thread struct { uint64_t ns, origin_ticks, frame; } T_gpu_time_pending;
static uint64_t g_gpu_guest_ns, g_gpu_origin_ticks, g_gpu_guest_frame;
void vk_gpu_time_guest_submit(uint64_t ns, uint64_t origin_ticks, uint64_t frame) {
    T_gpu_time_pending.ns = ns; T_gpu_time_pending.origin_ticks = origin_ticks; T_gpu_time_pending.frame = frame;
}
static __thread uint64_t T_ft_walk_frame; /* this thread's NVN walk, including its final publication join */
static uint64_t g_slot_count, g_game_slot_count, g_handoff_count, g_submit_join_ns, g_fallback_consumed;
static _Atomic uint64_t g_queue_call_ns;
static uint64_t g_stat_sub, g_stat_draw, g_stat_gpu_copy, g_stat_cpu_step, g_stat_ub, g_stat_ub_renamed;
static _Atomic uint64_t g_seal_serial = 1; /* cache generation follows stream seals, never asynchronous publication */
uint64_t vk_submit_serial(void) { return atomic_load(&g_seal_serial); }
uint64_t vk_game_submit_serial(void) { return g_stat_sub + 1; }

/* Submission timing, recording-lock owned. Raw elapsed brackets the thread CPU clock pair; the legacy
   recording value subtracts explicit slot/GPU waits only. Worker/publication joins remain in elapsed.
   submit_exec calls begin/commit synchronously on the same walking thread, even with async replay. */
static uint64_t g_perf_replay_ns, g_perf_wait_ns, g_perf_sub0, g_perf_t0, g_perf_cpu_t0;
/* What the waits are for (oot-a103; the 300-frame slot line). 0: a ring slot (all RING in flight), 1: a uniform-snapshot
   arena (the game submission that used it last is not finished), 2: every other wait for a fence. ns: all waits; n: the
   ones that blocked. g_wait_kind: set around the snapshot arena's wait. */
static uint64_t g_waitk_ns[3], g_waitk_n[3], g_slot_small, g_slot_small_mid, g_slot_draw0, g_chunk_subs; static int g_wait_kind;
void vk_draw_geometry(uint64_t out[2]);   /* vk_draw.c: vertices (or indices) x instances of ordinary draws, index capacity of mesh draws; zeroed */
static uint64_t g_perf_cpu_ns, g_perf_elapsed_ns; static _Atomic uint64_t g_perf_gpu_ns; static uint64_t g_perf_subs;
static uint64_t walker_cpu_now(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t)) DIE("walker thread CPU clock");
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}
/* running totals for the long-frame log (vk_draw.c): recording ns, recording-thread waits for the GPU ns, the game's
   blocking sync waits ns (mtl_wait), CPU steps (a flush and wait so the CPU reads GPU results), blocking sync waits */
static _Atomic uint64_t g_cum[5];
void vk_perf_cum(uint64_t c[5]) { for (int i = 0; i < 5; i++) c[i] = atomic_load(&g_cum[i]); }
static int ts_ok(void) { static int ok = -1; if (ok < 0) ok = V.props.limits.timestampComputeAndGraphics && V.props.limits.timestampPeriod > 0 && !vk_off("ts"); return ok; }
void vk_perf_take(double *replay_ms, double *wait_ms, double *gpu_ms, double *cpu_ms, double *elapsed_ms, uint64_t *subs) {
    *replay_ms = g_perf_replay_ns / 1e6; *wait_ms = g_perf_wait_ns / 1e6; *gpu_ms = atomic_exchange(&g_perf_gpu_ns, 0) / 1e6; *subs = g_perf_subs;
    *cpu_ms = g_perf_cpu_ns / 1e6; *elapsed_ms = g_perf_elapsed_ns / 1e6;
    g_perf_replay_ns = g_perf_wait_ns = g_perf_cpu_ns = g_perf_elapsed_ns = 0; g_perf_subs = 0;
}
static uint64_t gpu_ticks(void) { return guest_clock_ns_to_ticks(runtime_now_ns()); }
/* Counter timestamps must share GetSystemTick's CLOCK_MONOTONIC epoch: the game
   subtracts its CPU frame origin from the GPU END report, not GPU end-start.
   Calibrate the clocks directly, never against a completed submission. */
enum { TSQ = 1024 };
static VkQueryPool g_tsq; static uint32_t g_tsq_next; static _Atomic uint8_t g_tsq_busy[TSQ]; static int g_ts_on = -1;
static int64_t g_ts_off; static int g_ts_cal;
static uint64_t g_ts_gpu_sample, g_ts_host_sample, g_ts_refresh_ns;
static int g_ts_clock_valid;
static vk_gpu_time_queue g_gpu_time_queue; /* single Vulkan queue, ordered completion owner */
static int g_gpu_time_uncertain;
static vk_gpu_time_samples g_gpu_time_samples; /* publication and FPS reads under g_mu */
enum { GC_SAMPLES, GC_REBASED, GC_INVALID, GC_WITHHELD, GC_COUNT };
static uint64_t g_gpu_controller_stats[GC_COUNT]; /* g_mu, not the completion-only stats */
static vk_gpu_time_fps_filter g_gpu_fps_filter; /* g_mu, optional FPS-only policy */
static uint64_t g_gpu_fps_filter_reads, g_gpu_fps_filter_changed;
static uint64_t g_gpu_fps_filter_before, g_gpu_fps_filter_after;
int vk_gpu_time_hardware(void) { return g_gpu_time_hardware; }
uint64_t vk_gpu_time_controller_end(uint64_t addr, uint64_t timestamp, uint64_t origin) {
    int valid, rebased;
    pthread_mutex_lock(&g_mu);
    uint64_t end = vk_gpu_time_sample_consume(&g_gpu_time_samples, addr, timestamp, origin, &valid, &rebased);
    g_gpu_controller_stats[GC_SAMPLES]++;
    g_gpu_controller_stats[GC_REBASED] += rebased;
    g_gpu_controller_stats[GC_INVALID] += !valid;
    g_gpu_controller_stats[GC_WITHHELD] += !valid;
    if (g_gpu_time_fps_median) {
        uint64_t work = end - origin;
        uint64_t filtered = vk_gpu_time_fps_median(&g_gpu_fps_filter, addr, timestamp, work, valid);
        g_gpu_fps_filter_reads++;
        g_gpu_fps_filter_changed += work != filtered;
        g_gpu_fps_filter_before += work;
        g_gpu_fps_filter_after += filtered;
        end = origin + filtered;
    }
    pthread_mutex_unlock(&g_mu);
    /* The listener converts ns back to ticks before subtracting. Round UP here
       so a held input, including initial zero, survives that round trip exactly. */
    return (uint64_t)(((__uint128_t)end * 625 + 11) / 12);
}
static void gpu_time_unavailable(uint64_t frame, const char *reason) {
    if (!g_gpu_time_hardware) return;
    if (!g_gpu_time_uncertain)
        LOGV("[fps-policy] GPU report time hardware unavailable at producer frame %llu: %s; latency reports until restart",
             (unsigned long long)frame, reason);
    g_gpu_time_uncertain = 1; /* Unknown GPU work must never disappear from later backlog estimates. */
}
static struct {
    vk_gpu_time_slot slot;
    uint64_t origin_ticks, frame, clock_gpu, clock_guest;
    int valid;
} g_tsq_time[TSQ]; /* retained until the carried post consumes this query, even after its slot is reused */
enum { GT_REPORTS, GT_REMOVED, GT_WORK_COUNT, GT_WORK_BEFORE, GT_WORK_AFTER, GT_FALLBACK,
       GT_SLOTS, GT_SUBMIT_DELAY, GT_MONOTONIC, GT_COUNT };
static uint64_t g_gpu_time_stats[GT_COUNT]; /* completion-owned */
static uint64_t g_gpu_time_published[GT_COUNT]; /* snapshot under existing g_mu */
static void gpu_time_stats(uint64_t frame) {
    uint64_t v[GT_COUNT], c[GC_COUNT]; static uint64_t previous[GT_COUNT], previous_controller[GC_COUNT];
    uint64_t filter[4]; static uint64_t previous_filter[4];
    pthread_mutex_lock(&g_mu);
    memcpy(v, g_gpu_time_published, sizeof v); memcpy(c, g_gpu_controller_stats, sizeof c);
    filter[0] = g_gpu_fps_filter_reads; filter[1] = g_gpu_fps_filter_changed;
    filter[2] = g_gpu_fps_filter_before; filter[3] = g_gpu_fps_filter_after;
    pthread_mutex_unlock(&g_mu);
    for (unsigned i = 0; i < GT_COUNT; i++) { uint64_t current = v[i]; v[i] -= previous[i]; previous[i] = current; }
    for (unsigned i = 0; i < GC_COUNT; i++) { uint64_t current = c[i]; c[i] -= previous_controller[i]; previous_controller[i] = current; }
    LOGV("[fps-policy] GPU report time frame %llu mode=%s: %llu reports, removed avg %.3f ms; guest-submit -> Vulkan-submit avg %.3f ms (%llu slots); controller GPU end-origin before/after %.3f/%.3f ms (%llu end-report samples, captured origin); fallback %llu monotonic-clamps %llu; controller samples %llu rebased %llu invalid %llu withheld %llu",
         (unsigned long long)frame, g_gpu_time_hardware ? "hardware" : "latency",
         (unsigned long long)v[GT_REPORTS], v[GT_REPORTS] ? v[GT_REMOVED] / (1e6 * v[GT_REPORTS]) : 0,
         v[GT_SLOTS] ? v[GT_SUBMIT_DELAY] / (1e6 * v[GT_SLOTS]) : 0, (unsigned long long)v[GT_SLOTS],
         v[GT_WORK_COUNT] ? v[GT_WORK_BEFORE] / (1e6 * v[GT_WORK_COUNT]) : 0,
         v[GT_WORK_COUNT] ? v[GT_WORK_AFTER] / (1e6 * v[GT_WORK_COUNT]) : 0,
         (unsigned long long)v[GT_WORK_COUNT], (unsigned long long)v[GT_FALLBACK],
         (unsigned long long)v[GT_MONOTONIC], (unsigned long long)c[GC_SAMPLES],
         (unsigned long long)c[GC_REBASED], (unsigned long long)c[GC_INVALID], (unsigned long long)c[GC_WITHHELD]);
    if (g_gpu_time_fps_median) {
        for (unsigned i = 0; i < 4; i++) {
            uint64_t current = filter[i]; filter[i] -= previous_filter[i]; previous_filter[i] = current;
        }
        LOGV("[fps-policy] GPU FPS median frame %llu: %llu reads, %llu changed; raw/filtered avg %.3f/%.3f ms",
             (unsigned long long)frame, (unsigned long long)filter[0], (unsigned long long)filter[1],
             filter[0] ? filter[2] / (19200.0 * filter[0]) : 0,
             filter[0] ? filter[3] / (19200.0 * filter[0]) : 0);
    }
}
/* Before any real submission or completion worker exists, bracket three tiny
   empty timestamp submissions with the SAME clock provider as the timer gates.
   The narrowest bracket supplies an honest midpoint and uncertainty, not the
   completion time of an arbitrary production submission. No hot-path wait or
   periodic queue drain is introduced on drivers without DEVICE calibration. */
static void ts_clock_init(void) {
    const char *enabled = getenv("TOTK_VK_GPU_TIMESTAMPS");
    if ((enabled && !strcmp(enabled, "0")) || !f12.hostQueryReset || !ts_ok()) return;
    VkQueryPool pool; VkCommandPool commands; VkCommandBuffer cb; VkFence fence;
    VkQueryPoolCreateInfo qi = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
        .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = 1};
    VkCommandPoolCreateInfo pi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .queueFamilyIndex = V.qfam};
    VkFenceCreateInfo fi = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VKCHECK(vkCreateQueryPool(V.dev, &qi, NULL, &pool));
    VKCHECK(vkCreateCommandPool(V.dev, &pi, NULL, &commands));
    VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = commands, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    VKCHECK(vkAllocateCommandBuffers(V.dev, &ai, &cb));
    VKCHECK(vkCreateFence(V.dev, &fi, NULL, &fence));
    uint64_t width = UINT64_MAX;
    for (int i = 0; i < 3; i++) {
        VKCHECK(vkResetCommandPool(V.dev, commands, 0));
        VKCHECK(vkResetFences(V.dev, 1, &fence));
        VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
        VKCHECK(vkBeginCommandBuffer(cb, &bi));
        vkCmdResetQueryPool(cb, pool, 0, 1);
        vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, pool, 0);
        VKCHECK(vkEndCommandBuffer(cb));
        VkCommandBufferSubmitInfo ci = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = cb};
        VkSubmitInfo2 si = {VK_STRUCTURE_TYPE_SUBMIT_INFO_2, .commandBufferInfoCount = 1, .pCommandBufferInfos = &ci};
        uint64_t before = runtime_now_ns();
        VKCHECK(vkQueueSubmit2(V.queue, 1, &si, fence));
        VKCHECK(vkWaitForFences(V.dev, 1, &fence, VK_TRUE, UINT64_MAX));
        uint64_t after = runtime_now_ns(), device;
        VKCHECK(vkGetQueryPoolResults(V.dev, pool, 0, 1, sizeof device, &device, sizeof device, VK_QUERY_RESULT_64_BIT));
        if (after - before < width) {
            width = after - before; g_ts_gpu_sample = device;
            g_ts_host_sample = before + width / 2;
        }
    }
    vkDestroyFence(V.dev, fence, NULL); vkDestroyCommandPool(V.dev, commands, NULL);
    vkDestroyQueryPool(V.dev, pool, NULL); g_ts_clock_valid = 1;
    LOGV("[fps-policy] GPU timestamp clock: bounded guest-clock query, uncertainty <= %llu ns; device %llu guest-ns %llu host-ns %llu period %.9g ns bits %u",
         (unsigned long long)((width + 1) / 2), (unsigned long long)g_ts_gpu_sample,
         (unsigned long long)g_ts_host_sample, (unsigned long long)host_now_ns(),
         V.props.limits.timestampPeriod, V.timestamp_bits);
}
static void ts_calibrate(void) {
    if (!g_calibrated_timestamps) return;
    uint64_t now = host_now_ns();
    if (g_ts_refresh_ns && now - g_ts_refresh_ns < UINT64_C(1000000000)) return;
    g_ts_refresh_ns = now;
    VkCalibratedTimestampInfoEXT info[2] = {
        {VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_EXT, NULL, VK_TIME_DOMAIN_DEVICE_EXT},
        {VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_EXT, NULL, VK_TIME_DOMAIN_CLOCK_MONOTONIC_EXT}
    };
    uint64_t best[2] = {0}, deviation = UINT64_MAX;
    for (int i = 0; i < 3; i++) {
        uint64_t sample[2], error, host_before = host_now_ns();
        uint64_t guest_before = runtime_now_ns();
        if (g_calibrated_timestamps(V.dev, 2, info, sample, &error) == VK_SUCCESS && error < deviation) {
            deviation = error; best[0] = sample[0];
            best[1] = guest_clock_rebase_ns(sample[1], host_before, guest_before);
        }
    }
    if (deviation == UINT64_MAX) {
        if (!g_ts_clock_valid) LOGV("[fps-policy] GPU timestamp clock calibration failed; using completion-anchored fallback");
        return;
    }
    static int reported;
    if (!reported) {
        LOGV("[fps-policy] GPU timestamp clock: calibrated CLOCK_MONOTONIC mapped to guest clock, maximum deviation %llu ns; device %llu guest-ns %llu period %.9g ns, valid bits %u",
             (unsigned long long)deviation, (unsigned long long)best[0], (unsigned long long)best[1],
             V.props.limits.timestampPeriod, V.timestamp_bits);
        reported = 1;
    }
    g_ts_gpu_sample = best[0]; g_ts_host_sample = best[1]; g_ts_clock_valid = 1;
}
static int ts_ok(void);
static int ts_reports_on(void) {
    if (g_ts_on < 0) {
        const char *e = getenv("TOTK_VK_GPU_TIMESTAMPS");
#ifdef TOTK_GAME_OOT
        /* Ocarina of Time's dynamic resolution takes the GPU's backlog from a pair of timestamp reports issued back to back
           (TIMESTAMP_TOP, then TIMESTAMP: Swap2-Native's game notes, the controller at main+0x5a8f0) and steps its scene size
           by it. With hardware queries the pair read the real backlog when the query ring had room and the same time when it
           was full (about 1500 counter reports a frame), so the scene jumped between 854x480 and 427x240 from frame to frame:
           the picture flickered sharp / soft (oot-a63). Off by default here, as Swap2-Native's gpu_timestamps submission: the
           reports of one submission read one time, the load is 0 and the game keeps its first level.
           TOTK_VK_GPU_TIMESTAMPS=1 turns the queries back on. */
        if (!e) e = "0";
#endif
        g_ts_on = !(e && !strcmp(e, "0")) && f12.hostQueryReset && ts_ok();
        if (g_ts_on) {
            VkQueryPoolCreateInfo qi = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = TSQ};
            if (vkCreateQueryPool(V.dev, &qi, NULL, &g_tsq)) g_ts_on = 0; else vkResetQueryPool(V.dev, g_tsq, 0, TSQ);
        }
        LOGV("GPU timestamps for the game's counter reports: %s", g_ts_on ? "on" : f12.hostQueryReset ? "off" : "off (no hostQueryReset)");
    }
    return g_ts_on;
}

/* ---------- cumulative samples-passed counter (TOTK_VK_OCCLUSION=1) ----------
   A Vulkan query cannot cross a dynamic-rendering instance. Close it at pass end, ResetCounter and ReportCounter;
   the next draw opens a new segment, without splitting the pass at a reset/report. Queries are precise, not boolean.
   Slot-local pools are host-reset only after completion (hostQueryReset is enabled in f12), so starting a segment in
   the middle of a pass needs no illegal vkCmdResetQueryPool there. New pool pages are host-reset before first use.

   Each segment owns a prefix node: its predecessor's cumulative total plus this query. The completion thread resolves
   nodes in queue/query order BEFORE applying posts or recycling the slot. Reports retain their exact prefix, not an
   address lookup or the live counter. Thus reports survive later resets, duplicate addresses, chunk/ring reuse, and
   submissions without a reset. Resolving a node detaches its predecessor; even a counter never reset keeps only its
   last resolved total plus in-flight nodes, not the whole history. Nodes/pool pages are recycled at their high-water mark. */
static pthread_mutex_t g_occl_mu = PTHREAD_MUTEX_INITIALIZER;
static occl_node *g_occl_tail, *g_occl_free;
static VkQueryPool g_occl_active; static uint32_t g_occl_q;
static uint64_t g_occl_reports, g_occl_zero, g_occl_segments;   /* g_occl_mu: keep zero/total snapshots consistent */
static uint64_t g_occl_unknown;   /* recording-thread pending sample lookups used by conditions */
static void occl_drop(occl_node *p) {   /* g_occl_mu held; each live node owns its predecessor */
    while (p && !--p->refs) { occl_node *prev = p->prev; p->prev = g_occl_free; g_occl_free = p; p = prev; }
}
void vk_occlusion_draw(VkCommandBuffer cb) {   /* immediately before a draw, inside rendering */
    if (!g_occl_on || g_occl_active) return;
    occl_page **at = g_cur->oq_cur ? &g_cur->oq_cur->next : &g_cur->oq;
    if (!g_cur->oq_cur || g_cur->oq_cur->used == OQ) {
        if (!*at) {
            *at = calloc(1, sizeof **at); if (!*at) DIE("occlusion query page allocation");
            VkQueryPoolCreateInfo qi = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_OCCLUSION, .queryCount = OQ};
            VKCHECK(vkCreateQueryPool(V.dev, &qi, NULL, &(*at)->qp));
            vkResetQueryPool(V.dev, (*at)->qp, 0, OQ);
        }
        g_cur->oq_cur = *at;
    }
    occl_page *p = g_cur->oq_cur;
    pthread_mutex_lock(&g_occl_mu);
    if (!g_occl_free) {
        occl_node *b = calloc(OQ, sizeof *b); if (!b) DIE("occlusion prefix allocation");
        for (int i = 0; i < OQ; i++) { b[i].prev = g_occl_free; g_occl_free = &b[i]; }
    }
    occl_node *n = g_occl_free; g_occl_free = n->prev;
    *n = (occl_node){.prev = g_occl_tail, .refs = 2};   /* slot completion + current prefix; transfer the old prefix's ref */
    g_occl_tail = n; p->node[p->used] = n;
    pthread_mutex_unlock(&g_occl_mu);
    g_occl_active = p->qp; g_occl_q = p->used++;
    vkCmdBeginQuery(cb, g_occl_active, g_occl_q, VK_QUERY_CONTROL_PRECISE_BIT);
}
void vk_occlusion_end(VkCommandBuffer cb) {   /* still inside the SAME rendering instance as BeginQuery */
    if (!g_occl_active) return;
    vkCmdEndQuery(cb, g_occl_active, g_occl_q); g_occl_active = VK_NULL_HANDLE;
}
void vk_occlusion_reset(void) {
    if (!g_occl_on) return;
    if (g_occl_active) vk_occlusion_end(vk_cb());
    pthread_mutex_lock(&g_occl_mu); occl_drop(g_occl_tail); g_occl_tail = NULL; pthread_mutex_unlock(&g_occl_mu);
}
static occl_node *occl_snapshot(void) {
    if (!g_occl_on) return NULL;
    if (g_occl_active) vk_occlusion_end(vk_cb());
    pthread_mutex_lock(&g_occl_mu);
    occl_node *n = g_occl_tail; if (n) n->refs++;
    pthread_mutex_unlock(&g_occl_mu);
    return n;
}
static void occl_complete(slot_t *s) {   /* fence signalled; every earlier slot has already been resolved */
    for (occl_page *p = s->oq; p && p->used; p = p->next) {
        uint64_t r[OQ][2];
        VKCHECK(vkGetQueryPoolResults(V.dev, p->qp, 0, p->used, (size_t)p->used * sizeof r[0], r, sizeof r[0],
                                      VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT));
        pthread_mutex_lock(&g_occl_mu);
        for (uint32_t i = 0; i < p->used; i++) {
            occl_node *n = p->node[i], *prev = n->prev;
            if (!r[i][1] || (prev && !prev->ready)) DIE("occlusion query/prefix unavailable after fence");
            n->total = r[i][0] + (prev ? prev->total : 0); n->ready = 1; n->prev = NULL;
            occl_drop(prev); occl_drop(n);   /* release predecessor and slot ownership; reports/current prefix retain totals */
        }
        g_occl_segments += p->used;
        pthread_mutex_unlock(&g_occl_mu);
    }
}
static uint64_t occl_result(occl_node *n) {   /* apply_posts, after all query segments up to the report have completed */
    if (!g_occl_on) return 1u << 20;
    pthread_mutex_lock(&g_occl_mu);
    if (n && !n->ready) DIE("occlusion report prefix unavailable after fence");
    uint64_t total = n ? n->total : 0; occl_drop(n);
    g_occl_reports++; g_occl_zero += total == 0;
    pthread_mutex_unlock(&g_occl_mu);
    return total;
}
void vk_occlusion_stats(uint64_t frame) {   /* present, recording lock held; completions never waited for here */
    if (!g_occl_on) return;
    pthread_mutex_lock(&g_occl_mu);
    uint64_t n = g_occl_reports, zero = g_occl_zero, seg = g_occl_segments; g_occl_reports = g_occl_zero = g_occl_segments = 0;
    pthread_mutex_unlock(&g_occl_mu);
    LOGV("[occl] frame %llu: %.1f queries/frame (%llu completed reports), %.1f%% reported 0; %.1f Vulkan segments/frame; %llu pending sample condition reads kept visible",
         (unsigned long long)frame, n / 300.0, (unsigned long long)n, n ? 100.0 * zero / n : 0, seg / 300.0, (unsigned long long)g_occl_unknown);
    g_occl_unknown = 0;
}
/* Counter reports recorded but not yet in memory (apply_posts writes them when their submission completes), oldest first.
   A render condition reads the newest pending report of an address instead of memory (nvn_mtl.m cpend): TOTK guards a
   clear with "the counters at a and a + 16 differ" and right after it reports a counter to a, so the clear runs once;
   reading memory, every guarded clear in the submission ran again (the linear depth the terrain tests against was wiped:
   no terrain, the sky layer showed through as a flat wedge). Non-sample values are known at recording. Pending real
   sample values are unknown: conditions involving them stay enabled, as on Metal, rather than comparing stale memory. */
enum { CPEND_N = 4096 };
static struct cpend_ent { uint64_t addr, val; int known; } *g_cpend;
static unsigned g_cpend_cap, g_cpend_head, g_cpend_tail;
static pthread_mutex_t g_cpend_mu = PTHREAD_MUTEX_INITIALIZER;
static void cpend_push(uint64_t addr, uint64_t val, int known) {
    pthread_mutex_lock(&g_cpend_mu);
    if (g_cpend_head - g_cpend_tail == g_cpend_cap) {   /* never discard an unknown report and accidentally read stale memory */
        unsigned cap = g_cpend_cap ? 2 * g_cpend_cap : CPEND_N;
        struct cpend_ent *p = malloc((size_t)cap * sizeof *p); if (!p) DIE("pending counter allocation");
        for (unsigned i = g_cpend_tail; i != g_cpend_head; i++) p[i % cap] = g_cpend[i % g_cpend_cap];
        free(g_cpend); g_cpend = p; g_cpend_cap = cap;
    }
    g_cpend[g_cpend_head++ % g_cpend_cap] = (struct cpend_ent){addr, val, known};
    pthread_mutex_unlock(&g_cpend_mu);
}
static void cpend_pop(uint64_t addr) {   /* the oldest pending report is now in memory */
    pthread_mutex_lock(&g_cpend_mu);
    if (g_cpend_tail != g_cpend_head && g_cpend[g_cpend_tail % g_cpend_cap].addr == addr) g_cpend_tail++;
    pthread_mutex_unlock(&g_cpend_mu);
}
int vk_counter_value(uint64_t addr, uint64_t *value) {   /* 0: pending real samples, whose GPU value is not known yet */
    static int ordered = -1; if (ordered < 0) ordered = !vk_off("condorder");
    if (!ordered) { memcpy(value, (const void *)(uintptr_t)addr, 8); return 1; }
    int found = 0, known = 1; *value = 0;
    pthread_mutex_lock(&g_cpend_mu);
    for (unsigned i = g_cpend_head; i != g_cpend_tail && !found; i--) if (g_cpend[(i - 1) % g_cpend_cap].addr == addr) {
        const struct cpend_ent *p = &g_cpend[(i - 1) % g_cpend_cap]; *value = p->val; known = p->known; found = 1;
    }
    if (!found) memcpy(value, (const void *)(uintptr_t)addr, 8);
    pthread_mutex_unlock(&g_cpend_mu);
    if (!known) g_occl_unknown++;
    return known;
}
static void apply_posts(slot_t *owner, const vk_gpu_time_slot *last_slot, int last_valid) {
    post_op *p = owner->post; int n = owner->npost;
    for (int i = 0; i < n; i++) {
        post_op *o = &p[i];
        if (o->kind == 1) {
            if (lag_wait_on()) { if (!nvn_dirty_lag_store32(o->val, o->v, o->mode == 1)) DIE("lag event store could not restore protection"); }
            else if (o->mode == 1) *o->val += o->v; else *o->val = o->v;
        }
        else {
            uint64_t ts = gpu_ticks();
            const vk_gpu_time_slot *timing = last_slot;
            uint64_t origin_ticks = owner->guest_origin_ticks;
            int valid = last_valid;
            if (o->tsq1) {
                unsigned q = o->tsq1 - 1;
                timing = &g_tsq_time[q].slot; origin_ticks = g_tsq_time[q].origin_ticks;
                valid = g_tsq_time[q].valid;
            }
            if (!o->tsq1) gpu_time_unavailable(owner->guest_frame, "point query disabled or ring full");
            if (g_gpu_time_uncertain) valid = 0;
            if (o->tsq1) {
                uint32_t q = o->tsq1 - 1; uint64_t v;
                if (!vkGetQueryPoolResults(V.dev, g_tsq, q, 1, sizeof v, &v, sizeof v, VK_QUERY_RESULT_64_BIT)) {
                    if (g_ts_clock_valid) {
                        unsigned q = o->tsq1 - 1;
                        uint64_t clock_gpu = g_gpu_time_hardware && valid ? g_tsq_time[q].clock_gpu : g_ts_gpu_sample;
                        uint64_t clock_guest = g_gpu_time_hardware && valid ? g_tsq_time[q].clock_guest : g_ts_host_sample;
                        ts = vk_timestamp_ticks(v, clock_gpu, clock_guest, V.props.limits.timestampPeriod, V.timestamp_bits);
                    } else {
                        uint64_t t = (uint64_t)((__uint128_t)(uint64_t)((double)v * V.props.limits.timestampPeriod) * 12 / 625);
                        if (!g_ts_cal) { g_ts_off = (int64_t)ts - (int64_t)t; g_ts_cal = 1; }
                        ts = (uint64_t)((int64_t)t + g_ts_off);
                    }
                } else {
                    valid = 0;
                    gpu_time_unavailable(g_tsq_time[q].frame, "point query read failed");
                }
                vkResetQueryPool(V.dev, g_tsq, q, 1);
            }
            uint64_t raw_ns = guest_clock_ticks_to_ns(ts), report_ns = raw_ns;
            if (g_gpu_time_hardware && valid) {
                uint64_t unclamped = timing->start_ns + (raw_ns > timing->raw_start_ns ? raw_ns - timing->raw_start_ns : 0);
                if (unclamped < g_gpu_time_queue.report_ns) g_gpu_time_stats[GT_MONOTONIC]++;
                report_ns = vk_gpu_time_report(&g_gpu_time_queue, timing, raw_ns, 0);
                ts = guest_clock_ns_to_ticks(report_ns);
            } else if (g_gpu_time_hardware) {
                g_gpu_time_stats[GT_FALLBACK]++;
                if (report_ns < timing->guest_ns) report_ns = timing->guest_ns;
                if (report_ns < g_gpu_time_queue.report_ns) report_ns = g_gpu_time_queue.report_ns;
                g_gpu_time_queue.report_ns = report_ns;
                ts = guest_clock_ns_to_ticks(report_ns);
            }
#ifdef TOTK_GAME_OOT
            {   /* the game's GPU load pair (mtl_sub_counter): the end report reads the begin report's time plus a fixed load */
                static struct { uint64_t addr, ticks; } tab[64]; static unsigned ntab, tnext; static int64_t add = -1; static uint64_t npair;
                /* default 40 ms: the game's middle level, 854x480. At 0 (1440x810) the phone gives 8 fps in Kokiri Forest with its
                   foliage drawn (oot-a71) and the process sits 0.3 GiB nearer to the system's memory limit (render targets) */
                if (add < 0) { const char *e = getenv("TOTK_OHOS_DYNRES_US"); add = (int64_t)(e && *e ? strtoull(e, NULL, 0) : 40000) * 1000; }
                if (o->pair_end) for (unsigned i = 0; i < ntab; i++) if (tab[i].addr == o->addr - 16) {
                    uint64_t was = ts; ts = guest_clock_ns_to_ticks(guest_clock_ticks_to_ns(tab[i].ticks) + (uint64_t)add);
                    if (!(npair & (npair - 1)) || npair % 3000 == 0)
                        LOGV("GPU load pair %llu at %#llx: the end report read %.3f ms after the begin report, now %.3f ms (TOTK_OHOS_DYNRES_US)", (unsigned long long)npair, (unsigned long long)o->addr,
                             ((double)guest_clock_ticks_to_ns(was) - (double)guest_clock_ticks_to_ns(tab[i].ticks)) / 1e6, (double)add / 1e6);
                    npair++; break;
                }
                if (o->type == NVN_COUNTER_TYPE_TIMESTAMP_TOP || o->type == NVN_COUNTER_TYPE_TIMESTAMP) {
                    unsigned i = 0; while (i < ntab && tab[i].addr != o->addr) i++;
                    if (i == ntab) { if (ntab < 64) ntab++; else { i = tnext; tnext = (tnext + 1) % 64; } }
                    tab[i].addr = o->addr; tab[i].ticks = ts;
                }
            }
#endif
            o->report_ticks = ts;
            o->report_origin = valid ? origin_ticks : 0;
            g_gpu_time_stats[GT_REPORTS]++;
            if (raw_ns > report_ns) g_gpu_time_stats[GT_REMOVED] += raw_ns - report_ns;
            if (origin_ticks && o->type == NVN_COUNTER_TYPE_TIMESTAMP) {
                uint64_t origin = guest_clock_ticks_to_ns(origin_ticks);
                if (raw_ns >= origin && report_ns >= origin) {
                    g_gpu_time_stats[GT_WORK_COUNT]++;
                    g_gpu_time_stats[GT_WORK_BEFORE] += raw_ns - origin;
                    g_gpu_time_stats[GT_WORK_AFTER] += report_ns - origin;
                }
            }
            if (o->tsq1) atomic_store(&g_tsq_busy[o->tsq1 - 1], 0);
            uint64_t rec[2] = {o->type == NVN_COUNTER_TYPE_SAMPLES_PASSED ? (g_occl_on ? occl_result(o->occl) : o->cval) : 0, ts};
            if (lag_wait_on()) { if (!nvn_dirty_lag_store((void *)(uintptr_t)o->addr, rec, sizeof rec)) DIE("lag counter store could not restore protection"); }
            else memcpy((void *)(uintptr_t)o->addr, rec, sizeof rec);
            cpend_pop(o->addr);
        }
    }
}
static void gpu_wait_slot(slot_t *s) {
    VkResult r = vkWaitForFences(V.dev, 1, &s->fence, VK_TRUE, 5000000000ull);
    for (int waited = 5; r == VK_TIMEOUT; waited += 5) {
        LOGV("【红】the GPU has not finished submission %llu after %d s (last finished: %llu)", (unsigned long long)s->value, waited, (unsigned long long)atomic_load(&g_done));
        r = vkWaitForFences(V.dev, 1, &s->fence, VK_TRUE, 5000000000ull);
    }
#ifdef TOTK_OHOS
    if (r == VK_ERROR_DEVICE_LOST) { extern void vk_gpu_trace_dump(void); vk_gpu_trace_dump(); }
#endif
    if (r) VK_DIE_RESULT(r, "waiting for the GPU (submission %llu): %d%s", (unsigned long long)s->value, r, r == VK_ERROR_DEVICE_LOST ? " (device lost)" : "");
}
/* Never writes guest memory. A post-op write may fault on a page a LATER submitted frame reads. The original
   completion thread cannot publish that later GPU fence while blocked in its own post op, so a GPU-only cursor is
   required; publishing before this submission's posts alone is insufficient. Slots stay busy until both cursors pass. */
static void *gpu_guard_thread(void *unused) {
    (void)unused;
    totk_thread_start("totk-gpufence", NULL);
    nvn_dirty_lag_thread_role("gpu-completion");
    for (;;) {
        pthread_mutex_lock(&g_mu);
        while (g_gpu_head == g_qtail) pthread_cond_wait(&g_cv_work, &g_mu);
        slot_t *s = g_queue_[g_gpu_head % RING];
        pthread_mutex_unlock(&g_mu);
        totk_live_slack_tick();   /* a live TOTK_TIMER_SLACK_NS: this thread applies it itself */
        gpu_wait_slot(s);
        if (s->ft_frame && totk_ft_on()) s->ft_gpu_done = totk_ft_now();
        nvn_dirty_lag_complete(s->value);
        pthread_mutex_lock(&g_mu);
        g_gpu_head++;
        pthread_cond_broadcast(&g_cv);
        pthread_mutex_unlock(&g_mu);
    }
    return NULL;
}
static void *completion_thread(void *arg) {
    (void)arg;
    totk_thread_start("totk-post", NULL);
    nvn_dirty_lag_thread_role("completion-post");
    for (;;) {
        pthread_mutex_lock(&g_mu);
        /* The GPU-only cursor's existence is immutable, even after capture is disabled at runtime.
           It must pass a slot before posts release that slot for reuse. */
        if (g_lag_wait == 1) { while (g_qhead == g_qtail || g_gpu_head == g_qhead) pthread_cond_wait(&g_cv, &g_mu); }
        else while (g_qhead == g_qtail) pthread_cond_wait(&g_cv_work, &g_mu);
        slot_t *s = g_queue_[g_qhead % RING];
        while (g_ub_publication == s->value) pthread_cond_wait(&g_cv, &g_mu);
        pthread_mutex_unlock(&g_mu);
        totk_live_slack_tick();
        if (g_lag_wait != 1) {
            gpu_wait_slot(s);
            if (s->ft_frame && totk_ft_on()) s->ft_gpu_done = totk_ft_now();
        }
        if (__builtin_expect(g_ic_k, 0)) ic_slot_complete((unsigned)(s - g_ring));   /* H2: before the posts and before g_done says the slot is finished */
        uint64_t t1 = g_hitch_on ? host_now_ns() : 0;   /* observed GPU completion handoff, not the driver's actual signal time */
        uint64_t gpu_ns = 0;
        int framegen=vk_fg_enabled() && vk_fg_interpolating(), framegen_valid=0;
        double framegen_ms=0;
        if (g_ts_on > 0 || g_gpu_time_hardware) ts_calibrate();
        uint64_t raw_start_ns = 0, raw_end_ns = 0; int timing_valid = 0;
        if (s->ts_on) {
            uint64_t t[2];
            if (!vkGetQueryPoolResults(V.dev, s->ts, 0, 2, sizeof t, t, 8, VK_QUERY_RESULT_64_BIT)) {
                if (t[1] > t[0]) {
                    gpu_ns = (uint64_t)((double)(t[1] - t[0]) * V.props.limits.timestampPeriod);
                    atomic_fetch_add(&g_perf_gpu_ns, gpu_ns);
                    vk_pipeline_keys_pressure(host_now_ns(), gpu_ns > 25000000);
                }
                /* Reuse this slot's existing bracket. Pre uploads execute before
                   all main primaries; include them in headroom, not host waits. */
                uint64_t pre=t[0];
                int pre_valid=1;
                if (s->pre_open && (g_gpu_time_hardware || framegen))
                    pre_valid=vkGetQueryPoolResults(V.dev,s->ts,2,1,sizeof pre,&pre,sizeof pre,VK_QUERY_RESULT_64_BIT) == VK_SUCCESS;
                if (framegen && pre_valid && V.timestamp_bits && V.props.limits.timestampPeriod > 0) {
                    uint64_t mask=V.timestamp_bits >= 64 ? UINT64_MAX : (UINT64_C(1) << V.timestamp_bits)-1;
                    framegen_ms=(double)((t[1]-pre) & mask)*V.props.limits.timestampPeriod/1e6;
                    framegen_valid=1;
                }
                if (g_gpu_time_hardware && g_ts_clock_valid) {
                    raw_start_ns = guest_clock_ticks_to_ns(vk_timestamp_ticks(t[0], g_ts_gpu_sample, g_ts_host_sample, V.props.limits.timestampPeriod, V.timestamp_bits));
                    raw_end_ns = guest_clock_ticks_to_ns(vk_timestamp_ticks(t[1], g_ts_gpu_sample, g_ts_host_sample, V.props.limits.timestampPeriod, V.timestamp_bits));
                    timing_valid = raw_end_ns >= raw_start_ns;
                    if (g_gpu_time_hardware && s->pre_open) {
                        if (pre_valid)
                            raw_start_ns = guest_clock_ticks_to_ns(vk_timestamp_ticks(pre, g_ts_gpu_sample, g_ts_host_sample, V.props.limits.timestampPeriod, V.timestamp_bits));
                        else timing_valid = 0;
                    }
                    timing_valid &= raw_end_ns >= raw_start_ns;
                }
            }
        }
        if (framegen) vk_fg_gpu_sample(s->value,framegen_ms,framegen_valid);
        totk_perf_gpu(s->guest_frame, gpu_ns);   /* TOTK_ADPF_GPU: per guest frame, in completion order */
        /* The guard cursor publishes its fence observation under g_mu before we can reach this slot.
           Report before CPU post ops: their completion is not GPU finish. gpu_ns covers main buffers, not pre. */
        if (s->ft_frame && totk_ft_on()) totk_ft_slot_gpu(s->ft_frame, s->value, s->ft_gpu_done, gpu_ns);
        occl_complete(s);
        if (vk_gpuprof_on) vk_gpuprof_slot_complete(&s->gpuprof);
        if (!timing_valid) gpu_time_unavailable(s->guest_frame, "slot GPU bracket unavailable");
        vk_gpu_time_slot timing = {.guest_ns = s->guest_submit_ns, .raw_start_ns = raw_start_ns, .raw_end_ns = raw_end_ns};
        if (g_gpu_time_hardware && timing_valid && !g_gpu_time_uncertain)
            timing = vk_gpu_time_begin(&g_gpu_time_queue, s->guest_submit_ns, raw_start_ns, raw_end_ns);
        for (uint32_t i = 0; i < s->tsq_count; i++) {
            unsigned q = (s->tsq_first + i) % TSQ;
            g_tsq_time[q].origin_ticks = s->guest_origin_ticks;
            if (g_gpu_time_hardware) {
                g_tsq_time[q].slot = timing; g_tsq_time[q].frame = s->guest_frame;
                g_tsq_time[q].valid = timing_valid && !g_gpu_time_uncertain;
                g_tsq_time[q].clock_gpu = g_ts_gpu_sample; g_tsq_time[q].clock_guest = g_ts_host_sample;
            }
        }
        if (s->guest_frame && s->queue_ns >= s->guest_submit_ns) {
            g_gpu_time_stats[GT_SLOTS]++;
            g_gpu_time_stats[GT_SUBMIT_DELAY] += s->queue_ns - s->guest_submit_ns;
        }
        apply_posts(s, &timing, timing_valid);
        int track_ready = runtime_android_predict_active() || runtime_android_fps_auto_active() ||
                          vk_fg_enabled() || totk_ft_on(); /* never enter frontend under g_mu */
        pthread_mutex_lock(&g_mu);
#ifdef TOTK_VK_TESTS
        if (g_test_completion_hold==s->value) {
            atomic_store_explicit(&g_test_completion_held,s->value,memory_order_release);
            pthread_cond_broadcast(&g_cv);
            while (g_test_completion_hold==s->value) pthread_cond_wait(&g_cv,&g_mu);
            atomic_store_explicit(&g_test_completion_held,0,memory_order_release);
        }
#endif
        if (g_gpu_time_hardware) for (int i = 0; i < s->npost; i++) {
            const post_op *p = &s->post[i];
            if (p->kind == 2 && p->type == NVN_COUNTER_TYPE_TIMESTAMP)
                vk_gpu_time_sample_publish(&g_gpu_time_samples, p->addr, p->report_ticks, p->report_origin);
        }
        memcpy(g_gpu_time_published, g_gpu_time_stats, sizeof g_gpu_time_published);
        /* Posts and the UBO-publication gate have passed. Keep slot ownership
           under g_mu until this pre-done-store anchor and g_done are published;
           neither timestamp is a manual NVN signal or a hardware signal time. */
        if (s->ft_frame && totk_ft_on()) totk_ff_ticket_publish(s->value, totk_ft_now(), (unsigned)s->npost);
        s->npost = 0; s->busy = 0; g_qhead++;
        if (!s->submit_busy && !replay_until((unsigned)(s-g_ring))) vk_record_slot_retire((unsigned)(s-g_ring));
        atomic_store(&g_done, s->value);
        replay_retire(s->value);
        if (track_ready) {
            unsigned index = (unsigned)(s->value % PRESENT_READY_HISTORY);
            g_present_ready[index].ticket = s->value;
            g_present_ready[index].ready_ns = host_now_ns();
        }
        if (g_hitch_on) { g_done_at = host_now_ns(); HK_ADD(comp_ns, g_done_at - t1); }
        pthread_cond_broadcast(&g_cv);
        pthread_mutex_unlock(&g_mu);
    }
    return NULL;
}
static void slot_open(slot_t *s);
/* Diagnostic facts are recorded only at the original short-circuit predicates.
 * -1 means unqueried; no allocator state is probed just for logging. */
typedef struct {
    int enabled,open_primary,optional_next,mutex_result,replay_pending,replay_pins;
    int busy,submit_busy,replay,scanned,free_slots;
    unsigned busy_checked,busy_skips,submit_busy_checked,submit_busy_skips,replay_checked,replay_skips;
} optional_begin_trace;
static void submit_optional_trace(const fg_dispatch_trace *dispatch,
        const optional_begin_trace *t,const char *reason) {
    if (!t) return;
    LOGV("[adaptive-dispatch-optional] source=%llu charge=%llu deadline-ns=%llu stage=optional-begin reason=%s open-primary=%d optional-next=%d mutex-result=%d replay-pending=%d replay-pins=%d pin-cap=%u scanned=%d free-slots=%d busy-checked=%u busy-skips=%u submit-busy-checked=%u submit-busy-skips=%u replay-checked=%u replay-skips=%u unqueried=-1 clock=host-monotonic",
        (unsigned long long)dispatch->source,(unsigned long long)dispatch->charge,
        (unsigned long long)dispatch->deadline,reason,t->open_primary,t->optional_next,
        t->mutex_result,t->replay_pending,t->replay_pins,(unsigned)(RING-1),
        t->scanned,t->free_slots,t->busy_checked,t->busy_skips,
        t->submit_busy_checked,t->submit_busy_skips,t->replay_checked,t->replay_skips);
}
/* Claim BOTH split and consumer before optional GL work. Busy or leased slots
 * are skipped, never waited on; tickets still come only from slot_submit. */
static int submit_optional_begin(int immutable) {
    fg_dispatch_trace *dispatch=vk_fg_dispatch_trace_current;
    optional_begin_trace facts,*t=NULL;
    if (dispatch && dispatch->enabled) {
        facts=(optional_begin_trace){.enabled=1,.open_primary=-1,.optional_next=-1,
            .mutex_result=-1,.replay_pending=-1,.replay_pins=-1,.scanned=-1,.free_slots=-1};
        t=&facts;
    }
    if (FG_DISPATCH_FACT(t,open_primary,g_cur!=NULL) ||
        FG_DISPATCH_FACT(t,optional_next,g_optional_next!=NULL)) {
        submit_optional_trace(dispatch,t,t && t->open_primary ? "open-primary" : "optional-next");
        return 0;
    }
    g_optional_immutable=0;
    if (FG_DISPATCH_FACT(t,mutex_result,pthread_mutex_trylock(&g_mu))) {
        submit_optional_trace(dispatch,t,"mutex-busy");
        return 0;
    }
    replay_retire(atomic_load(&g_done));
    slot_t *first=NULL,*second=NULL;
    if (!FG_DISPATCH_FACT(t,replay_pending,g_replay_pending!=NULL) &&
        FG_DISPATCH_FACT(t,replay_pins,g_replay_pins)<RING-1) {
        if (t) t->scanned=t->free_slots=0;
        for (unsigned n=0;n<RING;n++) {
            unsigned i=((unsigned)g_ring_next+n)%RING;
            slot_t *s=&g_ring[i];
            if (t) { t->scanned++; t->busy=t->submit_busy=t->replay=-1; }
            int skip=FG_DISPATCH_FACT(t,busy,s->busy) ||
                FG_DISPATCH_FACT(t,submit_busy,s->submit_busy) ||
                FG_DISPATCH_FACT(t,replay,replay_until(i)!=0);
            if (t) {
                t->busy_checked+=t->busy>=0; t->busy_skips+=t->busy>0;
                t->submit_busy_checked+=t->submit_busy>=0; t->submit_busy_skips+=t->submit_busy>0;
                t->replay_checked+=t->replay>=0; t->replay_skips+=t->replay>0;
            }
            if (skip) continue;
            if (t) t->free_slots++;
            if (!first) first=s;
            else { second=s; break; }
        }
    }
    if (second) {
        first->busy=second->busy=1;
        g_optional_next=second;
        g_ring_next=((unsigned)(second-g_ring)+1)%RING;
    }
    pthread_mutex_unlock(&g_mu);
    submit_optional_trace(dispatch,t,second ? "accepted" :
        t && t->replay_pending==1 ? "replay-pending" :
        t && t->replay_pins>=RING-1 ? "pin-cap" : "free-slots");
    if (!second) return 0;
    slot_open(first);
    g_optional_immutable=immutable;
    return 1;
}
int vk_submit_optional_begin(void) { return submit_optional_begin(0); }
int vk_submit_optional_begin_immutable(void) { return submit_optional_begin(1); }
void vk_submit_optional_cancel(void) {
    /* Only before signal/adoption: neither claimed slot has a submitted use. */
    if (!g_cur || !g_optional_next || g_replay_pending || g_cur->adopted)
        DIE("invalid optional pre-submit cancellation");
    VKCHECK(vkEndCommandBuffer(g_cur->cb));
    if (vk_gpuprof_on) vk_gpuprof_slot_cancel(&g_cur->gpuprof);
    pthread_mutex_lock(&g_mu);
    g_cur->busy=g_optional_next->busy=0;
    g_cur=g_optional_next=NULL;
    g_optional_immutable=0;
    pthread_cond_broadcast(&g_cv);
    pthread_mutex_unlock(&g_mu);
}
void vk_submit_adopt_primary(VkCommandBuffer closed) {
    if (!g_cur || !closed || g_cur->adopted) DIE("invalid prepared primary adoption");
    g_cur->adopted=closed;
    /* The closed primary precedes the ordinary main CB's timestamp start.
     * Reuse the existing pre-primary timestamp envelope so framegen's GPU
     * sample includes ALL adopted work, not just the empty continuation. */
    (void)vk_pre_cb();
    if (vk_gpuprof_on) {
        /* Optional adoption starts with an otherwise empty ordinary primary.
         * Queue order is pre, adopted, main: bracket the immutable primary
         * without adding queries to a reusable, pre-recorded command stream. */
        vk_gpuprof_scope_begin(g_cur->pre, "framegen");
        vk_gpuprof_scope_end(g_cur->cb);
    }
}
void vk_submit_optional_end(void) {
    /* Failure before the split leaves the spare slot unused. After the split,
     * sealing the consumer is mandatory even for a discarded picture. */
    if (!g_optional_next) { g_optional_immutable=0; return; }
    pthread_mutex_lock(&g_mu);
    g_optional_next->busy=0; g_optional_next=NULL;
    g_optional_immutable=0;
    pthread_cond_broadcast(&g_cv);
    pthread_mutex_unlock(&g_mu);
}
static void slot_begin_inner(void) {
    if (g_optional_next) {
        slot_t *s=g_optional_next; g_optional_next=NULL;
        slot_open(s); return;
    }
    slot_t *s = &g_ring[g_ring_next]; g_ring_next = (g_ring_next + 1) % RING;
    uint64_t w0 = host_now_ns(); int blocked;
    VK_REC_DO(VK_REC_O_GPU_WAIT,
        pthread_mutex_lock(&g_mu);
        if (g_replay_pins) {
            unsigned next=(unsigned)(s-g_ring);
            /* Admission always leaves one unpinned slot. Never wait on an
             * open lease: only its walking owner can seal the covering work. */
            while (replay_until(next)) {
#ifdef TOTK_VK_TESTS
                g_test_replay_skips++;
#endif
                next=(next+1)%RING;
            }
            s=&g_ring[next]; g_ring_next=(next+1)%RING;
        }
        blocked=s->busy || s->submit_busy;
        while (s->busy || s->submit_busy) pthread_cond_wait(&g_cv,&g_mu);
        s->busy=1; pthread_mutex_unlock(&g_mu));
    { uint64_t d = host_now_ns() - w0; g_perf_wait_ns += d; g_cum[1] += d; g_waitk_ns[0] += d; g_waitk_n[0] += (uint64_t)blocked;
      if (blocked && T_ft_walk_frame && totk_ft_on()) totk_ft_walk_wait(T_ft_walk_frame, d); }
    slot_open(s);
}
/* Primary-slot descriptors (immediate graphics, compute, post, presentation, frame generation) are rewound here,
   at the same point the pools used to be reset: busy and submit_busy have retired, replay pins have released the
   slot, or a cancelled optional pre-submit never reached the GPU. Every caller writes the set it gets.
   A live TOTK_VK_DESC_STOCK (vk_desc_stock_want) switches the slot here too; switched off, the slot frees its stock and
   destroys its pools (nothing of the retired generation is in use), and grows pools again as the off policy needs.
   With TOTK_VK_DESC_STOCK_MAX_SETS the stock is capped (vk_record.h): a reset for the ceiling also destroys the pools
   past vk_desc_stock_pool_limit, at this same point. */
static void slot_descriptors_trim(slot_t *s, int keep) {
    if (keep >= s->ndpool) return;
    for (int i = keep; i < s->ndpool; i++) vkDestroyDescriptorPool(V.dev, s->dpools[i], NULL);
    vk_record_primary_pools(-(int64_t)(s->ndpool - keep));
    s->ndpool = keep;
}
#ifdef TOTK_VK_TESTS
/* Both descriptor owners count, right before they reset or destroy a slot's pools, whether every submission that may
   reference those sets has retired. The primary slot checks at its open: the previous incarnation's ticket is done,
   its submit owner returned and no replay pin holds it; that ticket is kept for the recorder contexts of the slot,
   which recycle at their first job of the new incarnation, when the slot may already hold its next (unretired)
   ticket: theirs is the kept one. */
static _Atomic uint64_t g_test_pool_checks, g_test_pool_violations;
static uint64_t g_test_pool_retired[RING_MAX]; /* g_mu */
static void test_descriptor_pools_open(unsigned slot) {
    pthread_mutex_lock(&g_mu);
    const slot_t *s = &g_ring[slot];
    int idle = s->value <= atomic_load(&g_done) && !s->submit_busy && !replay_until(slot);
    g_test_pool_retired[slot] = s->value;
    pthread_mutex_unlock(&g_mu);
    atomic_fetch_add(&g_test_pool_checks, 1);
    if (!idle) atomic_fetch_add(&g_test_pool_violations, 1);
}
void vk_test_descriptor_pools_check(unsigned slot) {
    pthread_mutex_lock(&g_mu);
    int idle = g_test_pool_retired[slot] <= atomic_load(&g_done);
    pthread_mutex_unlock(&g_mu);
    atomic_fetch_add(&g_test_pool_checks, 1);
    if (!idle) atomic_fetch_add(&g_test_pool_violations, 1);
}
void vk_test_descriptor_pools_stats(uint64_t *checks, uint64_t *violations) {
    *checks = atomic_load(&g_test_pool_checks); *violations = atomic_load(&g_test_pool_violations);
}
#endif
static void slot_descriptors_recycle(slot_t *s) {
#ifdef TOTK_VK_TESTS
    test_descriptor_pools_open((unsigned)(s - g_ring));
#endif
    int want = vk_desc_stock_want();
    if (s->dstock_on && !want) {
        vk_desc_stock_free(&s->dstock);
        vk_record_descriptor_stock_event(VK_DESC_STOCK_OFF, (uint64_t)s->ndpool);
        slot_descriptors_trim(s, 0);
        s->curdpool = s->curdsets = 0; s->dstock_on = 0;
        return;
    }
    uint32_t ceiling = want ? vk_desc_stock_ceiling() : 0, limit = vk_desc_stock_pool_limit(ceiling, VK_DESC_PRIMARY_POOL_SETS);
    int keep = s->ndpool, reason = 0;
    if (s->dstock_on) {
        reason = vk_desc_stock_retire(s->dstock, ceiling, (uint32_t)s->ndpool, VK_DESC_PRIMARY_POOL_SETS);
        if (!reason) return;
    } else if (want && (uint32_t)s->ndpool > limit) reason = VK_DESC_STOCK_POOL_LIMIT;   /* switched on live over the limit */
    if (reason && reason != VK_DESC_STOCK_PRESSURE && (uint32_t)keep > limit) keep = (int)limit;
    int dirty = s->dstock_on ? (s->ndpool ? s->curdpool + 1 : 0) : s->ndpool;   /* a stock rebuild: pools used since the last */
    if (dirty > keep) dirty = keep;
    for (int i = 0; i < dirty; i++) VKCHECK(vkResetDescriptorPool(V.dev, s->dpools[i], 0));
    vk_record_descriptor_primary(0, 0, (uint64_t)dirty);
    if (reason) vk_record_descriptor_stock_event((unsigned)reason, (uint64_t)(s->ndpool - keep));
    slot_descriptors_trim(s, keep);
    s->curdpool = 0; s->curdsets = 0;
    s->dstock_on = want;   /* switched on: the stock starts empty on reset pools */
}
static void slot_open(slot_t *s) {
    VK_REC_DO(VK_REC_O_RESET,
    if (!s->pool) {
        VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | (vk_record_enabled() ? VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT : 0), .queueFamilyIndex = V.qfam};
        VKCHECK(vkCreateCommandPool(V.dev, &pci, NULL, &s->pool));
        VkCommandBuffer cbs[2];
        VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = s->pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 2};
        VKCHECK(vkAllocateCommandBuffers(V.dev, &cai, cbs)); s->cb = cbs[0]; s->pre = cbs[1];
    } else if (!vk_record_enabled()) VKCHECK(vkResetCommandPool(V.dev, s->pool, 0));
    if (!s->fence) { VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; VKCHECK(vkCreateFence(V.dev, &fci, NULL, &s->fence)); }
    else VKCHECK(vkResetFences(V.dev, 1, &s->fence));   /* the completion thread has waited for it (busy was cleared) */
    if (s->nprimary > s->recent_primary_peak) s->recent_primary_peak = s->nprimary;
    if (++s->primary_trim_uses == 32) {
        uint32_t keep = 256;
#ifdef TOTK_OHOS
        /* HarmonyOS experiment: the warm floor of primaries kept per slot (8 slots x 256 = 2048 retained, each with its
           driver-private memory, which the system counts as the process's GPU memory: GPU_LEAK_KILL above ~32% of RAM). */
        { static int floor_ = -1; if (floor_ < 0) { const char *e = getenv("TOTK_OHOS_PRIMARY_FLOOR");
#ifdef TOTK_GAME_OOT
            int v = e && *e ? atoi(e) : 16;   /* Ocarina of Time: 16 also when the variable is there but empty (a cleared test switch): 256 left 0.87 GB on the phone where 16 leaves 0.99 (oot-a90) */
#else
            int v = e && *e ? atoi(e) : 256;
#endif
            floor_ = v >= 8 && v <= 4096 ? v : 256; LOGV("primary command buffers: warm floor %d per slot (TOTK_OHOS_PRIMARY_FLOOR)", floor_); }
          keep = (uint32_t)floor_; }
#endif
        while (keep < (uint64_t)s->recent_primary_peak * 2 && keep <= UINT32_MAX / 2) keep *= 2;
        if (s->capprimary >= (uint64_t)keep * 2) {
            uint32_t freed = s->capprimary - keep;
            vkFreeCommandBuffers(V.dev, s->pool, freed, s->primary + keep);
            vkTrimCommandPool(V.dev, s->pool, 0); s->capprimary = keep;
            g_primary_reclaimed += freed;
        }
        s->primary_trim_uses = s->recent_primary_peak = 0;
    }
    arena_reset(&s->ar, 4); arena_retain_cap(&s->ar);
    slot_descriptors_recycle(s);
    );
    s->oq_cur = NULL;
    s->tsq_first = s->tsq_count = 0;
    for (occl_page *p = s->oq; p; p = p->next) { vkResetQueryPool(V.dev, p->qp, 0, OQ); p->used = 0; }
    if (s->primary) s->cb = s->primary[0];
    s->nprimary = 1;
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    VK_REC_DO(VK_REC_O_PRIMARY, VKCHECK(vkBeginCommandBuffer(s->cb, &bi)));
    s->pre_open = 0; s->ts_on = 0;
    s->adopted=VK_NULL_HANDLE;
    if (ts_ok()) {
        if (!s->ts) { VkQueryPoolCreateInfo qi = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = g_gpu_time_hardware || (vk_fg_enabled() && vk_fg_interpolating()) ? 3 : 2}; VKCHECK(vkCreateQueryPool(V.dev, &qi, NULL, &s->ts)); }
        vkCmdResetQueryPool(s->cb, s->ts, 0, 2);
        vkCmdWriteTimestamp2(s->cb, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, s->ts, 0);
        s->ts_on = 1;
    }
    g_cur = s;
    if (__builtin_expect(g_ic_k, 0)) ic_slot_open((unsigned)(s - g_ring));
    vk_record_slot_reset((unsigned)(s - g_ring));
    vk_gpuprof_slot_begin(&s->gpuprof, s->cb); /* also reads the launch-time switch once */
}
static void slot_begin(void) { VK_REC_DO(VK_REC_O_SLOT, slot_begin_inner()); }
void vk_barrier_all(VkCommandBuffer cb) {
    VkMemoryBarrier2 mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
                           .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
    VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vkCmdPipelineBarrier2(cb, &di);
}
static void barrier_host(VkCommandBuffer cb) {   /* GPU writes become visible to the game */
    VkMemoryBarrier2 mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
                           .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT, .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT | VK_ACCESS_2_HOST_WRITE_BIT};
    VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vkCmdPipelineBarrier2(cb, &di);
}
VkCommandBuffer vk_cb(void) { if (!g_cur) slot_begin(); return g_cur->cb; }
static VkCommandBuffer record_next_primary_inner(void) {
    slot_t *s = g_cur; if (!s) DIE("deferred rendering has no submission slot");
    if (s->nprimary >= s->capprimary) {
        uint32_t old = s->capprimary, count = old ? old * 2 : 16;
        s->primary = VK_REALLOC(s->primary, (size_t)count * sizeof *s->primary);
        if (!old) { s->primary[0] = s->cb; old = 1; }
        VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = s->pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = count - old};
        VKCHECK(vkAllocateCommandBuffers(V.dev, &ai, s->primary + old)); s->capprimary = count;
    }
    s->cb = s->primary[s->nprimary++];
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    VKCHECK(vkBeginCommandBuffer(s->cb, &bi)); return s->cb;
}
VkCommandBuffer vk_record_next_primary(void) { return VK_REC_VALUE(VK_REC_O_PRIMARY, record_next_primary_inner()); }
VkCommandBuffer vk_pre_cb(void) {
    if (!g_cur) slot_begin();
    if (!g_cur->pre_open) {
        VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
        VKCHECK(vkBeginCommandBuffer(g_cur->pre, &bi)); g_cur->pre_open = 1;
        if (vk_gpuprof_on) vk_gpuprof_slot_pre(g_cur->gpuprof, g_cur->pre);
        if ((g_gpu_time_hardware || (vk_fg_enabled() && vk_fg_interpolating())) && g_cur->ts_on) {
            vkCmdResetQueryPool(g_cur->pre, g_cur->ts, 2, 1);
            vkCmdWriteTimestamp2(g_cur->pre, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, g_cur->ts, 2);
        }
    }
    return g_cur->pre;
}
static void *arena_alloc(arena_t *a, size_t size, size_t align, VkBuffer *buf, VkDeviceSize *off) {
    if (!align) align = 16;
    chunk_t *c = a->nchunk ? &a->chunks[a->nchunk - 1] : NULL;
    VkDeviceSize at = c ? (c->used + align - 1) / align * align : 0;
    if (!c || at + size > c->size) {
        /* a free chunk (all are reset when the arena is) is used before a new one is made */
        int found = -1;
        for (int i = 0; i < a->nchunk - 1 && found < 0; i++) if (!a->chunks[i].used && a->chunks[i].size >= size) found = i;
        if (found >= 0) { chunk_t tmp = a->chunks[found]; a->chunks[found] = a->chunks[a->nchunk - 1]; a->chunks[a->nchunk - 1] = tmp; c = &a->chunks[a->nchunk - 1]; at = 0; }
        else {
            uint64_t compact_start = a->reserve ? host_now_ns() : 0;
            if (a->nchunk == a->capchunk) { a->capchunk = a->capchunk ? 2 * a->capchunk : 8; a->chunks = VK_REALLOC(a->chunks, (size_t)a->capchunk * sizeof *a->chunks); }
            c = &a->chunks[a->nchunk++]; memset(c, 0, sizeof *c);
            VkDeviceSize min = a->min ? a->min : (8u << 20);
            if (a->reserve > min) min = a->reserve;
            a->reserve = 0;
            c->size = size > min ? (size + 0xffff) & ~(VkDeviceSize)0xffff : min;
            VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = c->size, .usage = POOL_USAGE, .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
            VKCHECK(vkCreateBuffer(V.dev, &bci, NULL, &c->buf));
            HK_ADD(chunks, 1);
            VkMemoryRequirements req; vkGetBufferMemoryRequirements(V.dev, c->buf, &req);
            int t = pick_host_type(req.memoryTypeBits);
            VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size, .memoryTypeIndex = (uint32_t)t};
            VkResult result = vk_memory_allocate(&mai, &c->mem, a->min ? VK_MEMORY_SNAPSHOT : VK_MEMORY_SLOT);
            if (result) DIE("arena vkAllocateMemory failed: %d; buffer %llu, allocation %llu bytes, memory type %d, %s arena",
                result, (unsigned long long)c->size, (unsigned long long)req.size, t, a->min ? "snapshot" : "slot");
            VKCHECK(vkBindBufferMemory(V.dev, c->buf, c->mem, 0));
            result = vkMapMemory(V.dev, c->mem, 0, VK_WHOLE_SIZE, 0, (void **)&c->ptr);
            if (result) DIE("arena vkMapMemory failed: %d; allocation %llu bytes, memory type %d, %s arena",
                result, (unsigned long long)req.size, t, a->min ? "snapshot" : "slot");
            at = 0;
            if (compact_start) arena_compact_ns[a->min != 0] += host_now_ns() - compact_start;
        }
    }
    c->used = at + size;
    *buf = c->buf; *off = at;
    return c->ptr + at;
}
void *vk_transient(size_t size, size_t align, VkBuffer *buf, VkDeviceSize *off) {
    if (!g_cur) slot_begin();
    return arena_alloc(&g_cur->ar, size, align, buf, off);
}
#define DPOOL_SETS VK_DESC_PRIMARY_POOL_SETS
/* Allocates count (1..VK_DESC_STOCK_BATCH) sets from slot s, fewer near the end of a pool; never fewer than one. */
static uint32_t slot_alloc_sets(void *owner, VkDescriptorSetLayout layout, uint32_t count, VkDescriptorSet *out) {
    slot_t *s = owner;
    VkDescriptorSetLayout layouts[VK_DESC_STOCK_BATCH];
    for (uint32_t i = 0; i < count; i++) layouts[i] = layout;
    /* TOTK (2026-10-22): sets came only from the slot's last pool, so a submission needing more than one pool made a new
       pool every time and the reset ones before it were never used again: one more pool per frame in the world (~4,400
       sets a frame), each with its driver memory, until Android ran out (~45 MB/s). Now the slot's pools are used in
       turn and a new one is made only past the last. */
    int fresh = 0;
    for (;;) {
        if (s->curdpool < s->ndpool && s->curdsets >= DPOOL_SETS) {   /* counted here too: some drivers (lavapipe) never report a full pool */
            if (s->curdpool + 1 < s->ndpool) { s->curdpool++; s->curdsets = 0; continue; }
        } else if (s->curdpool < s->ndpool) {
            if (count > (uint32_t)(DPOOL_SETS - s->curdsets)) count = (uint32_t)(DPOOL_SETS - s->curdsets);
            VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = s->dpools[s->curdpool], .descriptorSetCount = count, .pSetLayouts = layouts};
            VkResult r = vkAllocateDescriptorSets(V.dev, &ai, out);
            vk_record_descriptor_primary(r ? 0 : count, 1, 0);
            if (!r) { s->curdsets += (int)count; totk_ft_count(FT_COUNT_DESC_CREATES, count); return count; }
            if (r != VK_ERROR_OUT_OF_POOL_MEMORY && r != VK_ERROR_FRAGMENTED_POOL) DIE("vkAllocateDescriptorSets: %d", r);
            if (count > 1) { count /= 2; continue; }   /* large layouts may fit fewer than a whole batch */
            if (s->curdpool + 1 < s->ndpool) { s->curdpool++; s->curdsets = 0; continue; }
        }
        VkDescriptorPoolCreateInfo pci = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = DPOOL_SETS, .poolSizeCount = VK_DESC_POOL_TYPES, .pPoolSizes = vk_desc_pool_sizes};
        if (s->ndpool == s->capdpool) { s->capdpool = s->capdpool ? 2 * s->capdpool : 4; s->dpools = VK_REALLOC(s->dpools, (size_t)s->capdpool * sizeof *s->dpools); }
        if (fresh) break;   /* a set that does not fit an empty pool */
        VKCHECK(vkCreateDescriptorPool(V.dev, &pci, NULL, &s->dpools[s->ndpool++]));
        HK_ADD(dpools, 1); vk_record_primary_pools(1);
        s->curdpool = s->ndpool - 1; s->curdsets = 0; fresh = 1;
    }
    DIE("descriptor set allocation failed in a fresh pool");
}
/* oot-a121 TOTK_OHOS_PUSHDESC (vk_int.h): set at device creation, read-only afterwards */
int vk_push_bits, vk_push_stage = -1; uint32_t vk_push_max; uint64_t vk_push_shader_n[2];
PFN_vkCmdPushDescriptorSetWithTemplateKHR vk_cmd_push_template;
VkDescriptorSet vk_alloc_set(VkDescriptorSetLayout layout) {
    if (!g_cur) slot_begin();
    slot_t *s = g_cur;
    if (s->dstock_on) return vk_desc_stock_take(&s->dstock, layout, slot_alloc_sets, s);
    VkDescriptorSet set; (void)slot_alloc_sets(s, layout, 1, &set); return set;
}
/* Submit what is recorded (pre, then main); with posts, the current submission's post ops ride along. */
static void ub_seal(uint64_t fence); static void ub_flush(void); static void gw_seal(uint64_t fence); static void gw_prune(void);
static int ub_private_on(void);
static void ub_publish(void);
static int lag_guard_submit(unsigned slot, uint64_t fence);
static void lag_guard_seal(unsigned slot, uint64_t fence);
static int lag_guard_submit_native(unsigned slot, uint64_t fence, int background);
static void wait_value(uint64_t fence);
#ifdef TOTK_VK_TESTS
static _Atomic(const char *) g_test_heap_failure;
void vk_test_fail_heap(const char *name) { atomic_store(&g_test_heap_failure, name); }
int vk_test_heap_failure(const char *name) {
    const char *fail = atomic_load(&g_test_heap_failure);
    return fail && !strcmp(fail, name);
}
static _Atomic uint64_t g_test_lag_fallbacks, g_test_wait_value;
static uint64_t g_test_submit_hold; /* g_mu */
static _Atomic uint64_t g_test_submit_held;
#endif
static int submit_async_on(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("TOTK_VK_REC_ASYNC_SUBMIT");
        on = vk_record_enabled() && (!e || atoi(e) != 0);
        if (on) LOGV("recorder slots: ordered asynchronous assembly/QueueSubmit (TOTK_VK_REC_ASYNC_SUBMIT=0 disables)");
    }
    /* Lavapipe producer exclusions still consume mutable walking logs; keep that diagnostic path synchronous. */
    if (on && lag_wait_on() && !vk_record_lag_native()) {
        static int logged;
        if (!logged) { logged = 1; LOGV("recorder slots: software-GPU lag producer exclusions require synchronous submission"); }
        return 0;
    }
    return on;
}
/* Exactly one queue caller per mode: the walking owner in inline mode, this FIFO owner in async mode. */
static void slot_queue(slot_t *s, int background) {
    VkCommandBufferSubmitInfo cbi[2] = {{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = s->pre}, {VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = s->cb}};
    VkSubmitInfo2 si = {VK_STRUCTURE_TYPE_SUBMIT_INFO_2, .commandBufferInfoCount = s->pre_open ? 2 : 1, .pCommandBufferInfos = s->pre_open ? cbi : cbi + 1};
    VkSemaphoreSubmitInfo signal, wait;
    int external=atomic_load_explicit(&g_external_enabled,memory_order_acquire);
    if (external) {
        signal=(VkSemaphoreSubmitInfo){VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
            .semaphore=g_external[s-g_ring].signal,.stageMask=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};
        wait=(VkSemaphoreSubmitInfo){VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
            .semaphore=g_external[s-g_ring].wait,.stageMask=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};
        if (signal.semaphore) { si.signalSemaphoreInfoCount=1; si.pSignalSemaphoreInfos=&signal; }
        if (wait.semaphore) { si.waitSemaphoreInfoCount=1; si.pWaitSemaphoreInfos=&wait; }
    }
    if (s->nprimary > 1 || s->adopted) {
        uint32_t count = s->nprimary + (s->pre_open != 0) + (s->adopted != VK_NULL_HANDLE);
        if (count > s->capsubmit) { s->capsubmit = count + 16; s->submit = VK_REALLOC(s->submit, (size_t)s->capsubmit * sizeof *s->submit); }
        uint32_t at = 0;
        if (s->pre_open) s->submit[at++] = (VkCommandBufferSubmitInfo){VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = s->pre};
        if (s->adopted) s->submit[at++] = (VkCommandBufferSubmitInfo){VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = s->adopted};
        for (uint32_t i = 0; i < s->nprimary; i++) s->submit[at++] = (VkCommandBufferSubmitInfo){VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = s->nprimary>1 ? s->primary[i] : s->cb};
        si.commandBufferInfoCount = count; si.pCommandBufferInfos = s->submit;
    }
    if (!background) pthread_mutex_lock(&g_mu);
    int ft_on = s->ft_frame && totk_ft_on();
    uint64_t start = ft_on ? totk_ft_now() : host_now_ns();
    s->queue_ns = runtime_now_ns();
    VkResult r = vkQueueSubmit2(V.queue, 1, &si, s->fence);
    uint64_t end = ft_on ? totk_ft_now() : host_now_ns();
    atomic_fetch_add_explicit(&g_queue_call_ns, end - start, memory_order_relaxed);
    if (ft_on) totk_ft_slot_queue(s->ft_frame, s->value, start, end);
#ifdef TOTK_OHOS
    if (r == VK_ERROR_DEVICE_LOST) { extern void vk_gpu_trace_dump(void); vk_gpu_trace_dump(); }
#endif
    if (r) VK_DIE_RESULT(r, "vkQueueSubmit2: %d%s", r, r == VK_ERROR_DEVICE_LOST ? " (device lost)" : "");
    if (external) g_external[s-g_ring].signal=g_external[s-g_ring].wait=VK_NULL_HANDLE;
    nvn_dirty_lag_submitted(s->value);
    if (background) pthread_mutex_lock(&g_mu);
    if (s->publication) g_ub_publication = s->value;
    g_queue_[g_qtail++ % RING] = s; g_fence = s->value;
    if (!background) g_submit_done_value = s->value;
    pthread_cond_signal(&g_cv_work); pthread_cond_broadcast(&g_cv);
    pthread_mutex_unlock(&g_mu);
    /* GPU completion may clear busy immediately; the async owner's submit_busy pin still prevents reuse. */
}
static void submit_failure(uint64_t value) {
#ifdef TOTK_VK_TESTS
    atomic_fetch_add_explicit(&g_test_lag_fallbacks, 1, memory_order_relaxed);
#endif
    pthread_mutex_lock(&g_mu); if (value > g_fallback_value) g_fallback_value = value; pthread_mutex_unlock(&g_mu);
}
static void *submit_worker(void *unused) {
    (void)unused; totk_thread_start("totk-submit", NULL);
    nvn_dirty_lag_thread_role("submit-assembly");
    for (;;) {
        pthread_mutex_lock(&g_mu);
        while (g_sealed_head == g_sealed_tail) pthread_cond_wait(&g_sealed_cv, &g_mu);
        slot_t *s = g_sealed[g_sealed_head % RING];
        pthread_mutex_unlock(&g_mu);
        totk_live_slack_tick();
        unsigned slot = (unsigned)(s - g_ring); uint64_t value = s->value; int join_on_walk = s->join_on_walk;
        vk_record_assemble(slot, 1);
        int failed = s->native_lag ? lag_guard_submit_native(slot, value, 1) : 0;
        slot_queue(s, 1);
        if (failed) {
            submit_failure(value);
            /* A slot pin prevents fence reset while this owner waits. Do not wait for guest post ops here:
               an older post may fault on a later ticket this same FIFO owner still needs to queue. */
            if (!join_on_walk || nvn_dirty_lag_vma_pressure()) gpu_wait_slot(s);
            if (nvn_dirty_lag_vma_pressure()) nvn_dirty_lag_vma_drain();
        }
#ifdef TOTK_VK_TESTS
        pthread_mutex_lock(&g_mu);
        if (g_test_submit_hold == value) {
            atomic_store_explicit(&g_test_submit_held, value, memory_order_release);
            pthread_cond_broadcast(&g_cv);
            while (g_test_submit_hold == value) pthread_cond_wait(&g_cv, &g_mu);
            atomic_store_explicit(&g_test_submit_held, 0, memory_order_release);
        }
        pthread_mutex_unlock(&g_mu);
#endif
        pthread_mutex_lock(&g_mu);
        s->submit_busy = 0; g_submit_done_value = value; g_sealed_head++;
        if (!s->busy && !replay_until(slot)) vk_record_slot_retire(slot);
        replay_retire(atomic_load(&g_done));
        pthread_cond_broadcast(&g_cv); pthread_mutex_unlock(&g_mu);
    }
    return NULL;
}
static void submit_worker_start(void) {
    pthread_t thread; if (pthread_create(&thread, NULL, submit_worker, NULL)) DIE("slot submit thread");
    pthread_detach(thread);
    g_submit_started = 1;
}
static void slot_handoff(slot_t *s) {
    static pthread_once_t once = PTHREAD_ONCE_INIT; pthread_once(&once, submit_worker_start);
    pthread_mutex_lock(&g_mu);
    while (g_sealed_tail - g_sealed_head == RING) pthread_cond_wait(&g_cv, &g_mu);
    g_sealed[g_sealed_tail++ % RING] = s; pthread_cond_signal(&g_sealed_cv);
    pthread_mutex_unlock(&g_mu); g_handoff_count++;
}
static void submit_join(uint64_t value) {
    uint64_t start = host_now_ns(); int blocked;
    VK_REC_DO(VK_REC_O_WORKER_WAIT,
        pthread_mutex_lock(&g_mu);
        blocked = g_submit_done_value < value;
        while (g_submit_done_value < value) pthread_cond_wait(&g_cv, &g_mu);
        pthread_mutex_unlock(&g_mu));
    uint64_t ns = host_now_ns() - start; g_submit_join_ns += ns; vk_record_walk_wait(ns);
    /* host_now_ns is CLOCK_MONOTONIC too; reuse this disjoint wait scope, not an enclosing CPU-step wait. */
    if (blocked && T_ft_walk_frame && totk_ft_on()) totk_ft_walk_wait(T_ft_walk_frame, ns);
}
static void rec_service_report(uint64_t frame);
unsigned vk_snap_arenas(void);
/* TOTK_OHOS_GW_LOG=<from frame>:<frames> (oot-a119): every GPU write into game memory the replay records (gpu_write_inner:
   writable bindings, copies into buffers, ...) in those frames, one line each with the frame, the submission within it
   (seal serials since the frame began), the range and its pool's NVN flags (CPU_UNCACHED / CPU_CACHED: the game can read
   it), to find what the game reads back after its frame-end wait (two frames in flight flash: DESIGN-OOT, oot-a118). */
static uint64_t g_gwl_from, g_gwl_to, g_gwl_frame, g_gwl_serial0; static int g_gwl_on = -1;
static int gwl_active(void) {
    if (__builtin_expect(g_gwl_on < 0, 0)) {
        const char *e = getenv("TOTK_OHOS_GW_LOG"); g_gwl_on = 0;
        if (e && *e) { g_gwl_from = strtoull(e, NULL, 10); const char *c = strchr(e, ':'); g_gwl_to = g_gwl_from + (c ? strtoull(c + 1, NULL, 10) : 2); g_gwl_on = 1;
            LOGV("GPU-write log: frames %llu .. %llu (TOTK_OHOS_GW_LOG)", (unsigned long long)g_gwl_from, (unsigned long long)g_gwl_to - 1); }
    }
    return g_gwl_on && g_gwl_frame >= g_gwl_from && g_gwl_frame < g_gwl_to;
}
static void gwl_note(uint64_t a, uint64_t n, const char *what);   /* after ic_span_of, below */
void vk_slot_stats(uint64_t frame) {
    if (__builtin_expect(g_gwl_on != 0, 0)) { g_gwl_frame = frame + 1; g_gwl_serial0 = vk_submit_serial(); if (gwl_active()) LOGV("[gwlog] frame %llu begins", (unsigned long long)g_gwl_frame); }
    if (__builtin_expect(g_ic_k, 0)) ic_frame(frame);   /* every present, recording lock held: the in-flight check's frame boundary */
    if (frame % 300) return;
    vk_wb_stats();
    vk_texel_stats(frame);
    rec_service_report(frame);
    gpu_time_stats(frame);
    static uint64_t previous_subs; uint64_t subs = g_stat_sub - previous_subs; previous_subs = g_stat_sub;
    LOGV("slot submission/frame: all/game/handed %.2f/%.2f/%.2f, game slots/NVN submit %.2f; walking publication join %.3f ms, driver QueueSubmit %.3f ms (background when async)",
        g_slot_count / 300.0, g_game_slot_count / 300.0, g_handoff_count / 300.0, subs ? (double)g_game_slot_count / subs : 0,
        g_submit_join_ns / 3e8, atomic_exchange_explicit(&g_queue_call_ns, 0, memory_order_relaxed) / 3e8);
    g_slot_count = g_game_slot_count = g_handoff_count = g_submit_join_ns = 0;
    LOGV("waits/frame by kind: ring slot %.2f ms (%.2f blocked), uniform-snapshot arena %.2f ms (%.2f blocked), other fences %.2f ms (%.2f blocked); "
         "slots sealed with fewer than 8 draws %.2f/frame (%.2f of them inside a game submission), chunk submissions %.2f/frame; ring %u, snapshot arenas %u",
         g_waitk_ns[0] / 3e8, g_waitk_n[0] / 300.0, g_waitk_ns[1] / 3e8, g_waitk_n[1] / 300.0, g_waitk_ns[2] / 3e8, g_waitk_n[2] / 300.0,
         g_slot_small / 300.0, g_slot_small_mid / 300.0, g_chunk_subs / 300.0, (unsigned)RING, vk_snap_arenas());
    memset(g_waitk_ns, 0, sizeof g_waitk_ns); memset(g_waitk_n, 0, sizeof g_waitk_n); g_slot_small = g_slot_small_mid = g_chunk_subs = 0;
    {   uint64_t geo[2]; vk_draw_geometry(geo);
        LOGV("geometry/frame: %.0f thousand vertices or indices (x instances) in ordinary draws, mesh draws' index capacity %.0f thousand",
             geo[0] / 300e3, geo[1] / 300e3); }
    static uint64_t previous_arena_ns[2], previous_arena_compacts[2];
    LOGV("recorder walking arena compaction ns/frame slot/snapshot: %.0f/%.0f; compactions/frame %.3f/%.3f (retired high-water consolidation only)",
        (arena_compact_ns[0] - previous_arena_ns[0]) / 300.0, (arena_compact_ns[1] - previous_arena_ns[1]) / 300.0,
        (arena_compacts[0] - previous_arena_compacts[0]) / 300.0, (arena_compacts[1] - previous_arena_compacts[1]) / 300.0);
    memcpy(previous_arena_ns, arena_compact_ns, sizeof arena_compact_ns);
    memcpy(previous_arena_compacts, arena_compacts, sizeof arena_compacts);
}
#ifdef TOTK_VK_TESTS
int vk_record_test_async_submit(void) { return submit_async_on(); }
void vk_record_test_submit_status(uint64_t *reserved, uint64_t *queued) {
    pthread_mutex_lock(&g_mu); *reserved = g_reserved_fence; *queued = g_fence; pthread_mutex_unlock(&g_mu);
}
void vk_record_test_hold_submit(uint64_t ticket) {
    pthread_mutex_lock(&g_mu); g_test_submit_hold = ticket; pthread_cond_broadcast(&g_cv); pthread_mutex_unlock(&g_mu);
}
uint64_t vk_record_test_submit_held(void) { return atomic_load_explicit(&g_test_submit_held, memory_order_acquire); }
void vk_record_test_hold_completion(uint64_t ticket) {
    pthread_mutex_lock(&g_mu); g_test_completion_hold=ticket;
    pthread_cond_broadcast(&g_cv); pthread_mutex_unlock(&g_mu);
}
uint64_t vk_record_test_completion_held(void) {
    return atomic_load_explicit(&g_test_completion_held,memory_order_acquire);
}
int vk_record_test_replay_pin(uint64_t source,uint64_t *until,uint64_t *skips) {
    int found=0;
    pthread_mutex_lock(&g_mu);
    for (unsigned i=0;i<RING;i++) if (g_ring[i].value==source) {
        *until=replay_until(i); found=1; break;
    }
    *skips=g_test_replay_skips;
    pthread_mutex_unlock(&g_mu);
    return found;
}
int vk_record_test_replay_snapshots(uint64_t source,unsigned *snapshots) {
    int found=0;
    pthread_mutex_lock(&g_mu);
    for (unsigned i=0;i<RING;i++) if (g_ring[i].value==source) {
        *snapshots=g_replay_owner[i].snapshots; found=1; break;
    }
    pthread_mutex_unlock(&g_mu);
    return found;
}
void vk_record_test_guest_context(uint64_t v[8]) {
    v[0]=g_in_sub; v[1]=g_gpu_guest_ns; v[2]=g_gpu_origin_ticks; v[3]=g_gpu_guest_frame;
    v[4]=(uint64_t)g_ncarry; v[5]=g_game_slot_count; v[6]=g_cur!=NULL; v[7]=g_slot_count;
}
int vk_record_test_ticket_context(uint64_t ticket,uint64_t v[5]) {
    int found=0;
    pthread_mutex_lock(&g_mu);
    if (ticket) for (unsigned i=0;i<RING;i++) if (g_ring[i].value==ticket) {
        const slot_t *s=&g_ring[i];
        v[0]=s->guest_submit_ns; v[1]=s->guest_origin_ticks; v[2]=s->guest_frame;
        v[3]=(uint64_t)s->npost; v[4]=s->join_on_walk; found=1; break;
    }
    pthread_mutex_unlock(&g_mu);
    return found;
}
#endif
static uint64_t slot_submit_inner(int with_posts, vk_pass_end_kind reason) {
    const char *previous_role = nvn_dirty_lag_thread_role_get();
    nvn_dirty_lag_thread_role("submit");
    if (!g_cur) slot_begin();
    int publish = with_posts && ub_private_on();
    if (!ub_private_on()) ub_flush();
    vk_end_rendering_at(reason, 0);
    vk_wb_record();   /* settled copies share this ordinary slot, after the last pass and before sealing */
    vk_record_slot_seal((unsigned)(g_cur - g_ring));
    totk_ft_count(FT_COUNT_CHUNKS, 1); /* one logical sealed slot, not each primary/secondary buffer */
    int asynchronous = submit_async_on();
    if (!asynchronous && g_submit_started) submit_join(g_reserved_fence);
    if (!asynchronous) vk_record_assemble((unsigned)(g_cur - g_ring), 0);
    VK_REC_DO(VK_REC_S_UNIFORM,
        ub_seal(g_reserved_fence + 1);
        gw_seal(g_reserved_fence + 1));
    slot_t *s = g_cur; g_cur = NULL;
    if (vk_gpuprof_on) vk_gpuprof_slot_seal(s->gpuprof);
    /* Bind a preceding external split to this actual resource consumer before
     * queue publication can complete either ticket. Its own GPU/CPU completion
     * remains independent; only physical resource retirement is extended. */
    if (atomic_load_explicit(&g_external_enabled,memory_order_acquire) && g_replay_pending && g_replay_pending!=s) {
        pthread_mutex_lock(&g_mu);
        g_external[g_replay_pending-g_ring].until=g_reserved_fence+1;
        g_replay_pending=NULL;
        pthread_mutex_unlock(&g_mu);
        if (!g_optional_immutable) snapshot_replay_pin(g_reserved_fence+1);
    }
    if (g_replay_releases) {
        pthread_mutex_lock(&g_mu);
        for (unsigned i=0;i<RING;i++) if (g_replay_owner[i].pending) {
            g_replay_owner[i].until=g_reserved_fence+1; g_replay_owner[i].pending=0;
        }
        g_replay_releases=0;
        pthread_mutex_unlock(&g_mu);
    }
    barrier_host(s->cb);
    if (s->ts_on) vkCmdWriteTimestamp2(s->cb, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, s->ts, 1);
    if (vk_gpuprof_on) vk_gpuprof_slot_end(s->gpuprof, s->cb);
#ifdef TOTK_OHOS
    vk_pp_ev[PPE_SUBMIT]++;
#endif
    VKCHECK(vkEndCommandBuffer(s->cb));
    if (s->pre_open) { barrier_host(s->pre); VKCHECK(vkEndCommandBuffer(s->pre)); }
    if (with_posts && g_ncarry) {
        if (s->cap < g_ncarry) { s->cap = g_ncarry; s->post = VK_REALLOC(s->post, (size_t)s->cap * sizeof *s->post); }
        memcpy(s->post, g_carry, (size_t)g_ncarry * sizeof *g_carry); s->npost = g_ncarry; g_ncarry = 0;
    }
    unsigned slot = (unsigned)(s - g_ring); uint64_t value = g_reserved_fence + 1;
    s->native_lag = lag_wait_on() && vk_record_lag_native();
    if (s->native_lag) lag_guard_seal(slot, value);
    int untracked_wait = !asynchronous && lag_wait_on() ? lag_guard_submit(slot, value) : 0;
    int join_required = with_posts || !g_in_sub || reason == VK_PASS_END_CPU_WAIT;
    s->publication = publish; s->join_on_walk = join_required;
    pthread_mutex_lock(&g_mu); s->value = ++g_reserved_fence; s->submit_busy = asynchronous; pthread_mutex_unlock(&g_mu);
    if (vk_fg_lsfg_probe_requested())
        vk_fg_lsfg_sealed(s->nprimary>1 ? s->primary : &s->cb,s->nprimary>1 ? s->nprimary : 1,s->value);
    s->ft_frame = 0; s->ft_gpu_done = 0;
    /* Backend-only work contributes GPU duration, never a late host-time floor. */
    s->guest_submit_ns = g_in_sub || with_posts ? g_gpu_guest_ns : 0;
    s->guest_origin_ticks = g_in_sub || with_posts ? g_gpu_origin_ticks : 0;
    s->guest_frame = g_in_sub || with_posts ? g_gpu_guest_frame : 0;
    if (totk_ft_on()) {
        s->ft_frame = totk_ft_current();
        totk_ft_slot_seal(s->ft_frame, s->value, totk_ft_now());
    }
    g_slot_count++; g_game_slot_count += with_posts || g_in_sub;
    {   /* all slots (the present's and the last slot of every small game submission among them), and those sealed
           inside a game submission, which is what the chunk counter's restart changes */
        int few = g_stat_draw - g_slot_draw0 < 8; g_slot_small += (uint64_t)few; g_slot_small_mid += (uint64_t)(few && g_in_sub && !with_posts);
        g_slot_draw0 = g_stat_draw;   /* (the next slot's draws start here) */
    }
    vk_draw_slot_sealed();
    if (asynchronous) slot_handoff(s);
    else {
        VK_REC_DO(VK_REC_O_QUEUE, slot_queue(s, 0));
        if (untracked_wait) submit_failure(value);
        if (untracked_wait && nvn_dirty_lag_vma_pressure()) { gpu_wait_slot(s); nvn_dirty_lag_vma_drain(); }
    }
    atomic_fetch_add(&g_seal_serial, 1);
    if (asynchronous && join_required) submit_join(value);
    if (publish) {
        ub_publish();   /* QueueSubmit and worker join precede every host store and genuine-alias wait */
        pthread_mutex_lock(&g_mu);
        g_ub_publication = 0; pthread_cond_broadcast(&g_cv);
        pthread_mutex_unlock(&g_mu);
    }
    if (!asynchronous || with_posts || !g_in_sub || reason == VK_PASS_END_CPU_WAIT) {
        /* Final publication precedes host stores; every failed earlier slot also retains its post-op frontier. */
        pthread_mutex_lock(&g_mu); uint64_t failed = g_fallback_value; pthread_mutex_unlock(&g_mu);
        if (failed > g_fallback_consumed && failed <= value) { wait_value(failed); g_fallback_consumed = failed; }
    }
    nvn_dirty_lag_thread_role(previous_role);
    return value;
}
static uint64_t slot_submit(int with_posts, vk_pass_end_kind reason) { return VK_REC_VALUE(VK_REC_O_SUBMIT, slot_submit_inner(with_posts, reason)); }
uint64_t vk_replay_lease_begin(void) {
    if (!g_cur) return 0;
    unsigned slot=(unsigned)(g_cur-g_ring), snapshots=snapshot_replay_mask();
    pthread_mutex_lock(&g_mu);
    uint64_t token=0;
    if (!g_replay_owner[slot].until && (replay_until(slot) || g_replay_pins<RING-1)) {
        if (!replay_until(slot)) g_replay_pins++;
        token=++g_replay_token;
        if (!token) token=++g_replay_token;
        g_replay_owner[slot].token=token; g_replay_owner[slot].until=UINT64_MAX;
        g_replay_owner[slot].snapshots=snapshots;
    }
    pthread_mutex_unlock(&g_mu);
    return token;
}
uint64_t vk_replay_lease_begin_immutable(void) {
    if (!g_cur) return 0;
    unsigned slot=(unsigned)(g_cur-g_ring);
    pthread_mutex_lock(&g_mu);
    uint64_t token=0;
    if (!g_replay_owner[slot].until && (replay_until(slot) || g_replay_pins<RING-1)) {
        if (!replay_until(slot)) g_replay_pins++;
        token=++g_replay_token;
        if (!token) token=++g_replay_token;
        g_replay_owner[slot].token=token; g_replay_owner[slot].until=UINT64_MAX;
        g_replay_owner[slot].snapshots=0;
    }
    pthread_mutex_unlock(&g_mu);
    return token;
}
int vk_replay_lease_end(uint64_t token,uint64_t covering_ticket) {
    if (!token) return 0;
    pthread_mutex_lock(&g_mu);
    int ended=0;
    if (covering_ticket<=g_reserved_fence) for (unsigned i=0;i<RING;i++) {
        if (g_replay_owner[i].token!=token) continue;
        /* Latest reservation also covers canceled output and split-only
         * failures. Open work must first seal; frame-end's ordinary submission
         * resolves this pending release before it can retire on the GPU. */
        g_replay_owner[i].token=0;
        g_replay_owner[i].pending=g_cur || g_replay_pending;
        g_replay_releases+=g_replay_owner[i].pending;
        g_replay_owner[i].until=g_replay_owner[i].pending ? UINT64_MAX : g_reserved_fence;
        replay_retire(atomic_load(&g_done));
        pthread_cond_broadcast(&g_cv);
        ended=1; break;
    }
    pthread_mutex_unlock(&g_mu);
    return ended;
}
uint64_t vk_submit_signal(VkSemaphore signal) {
    if (!atomic_load_explicit(&g_external_enabled,memory_order_acquire) || !signal || g_replay_pending) return 0;
    (void)vk_cb();
    pthread_mutex_lock(&g_mu);
    if (!replay_until((unsigned)(g_cur-g_ring)) && g_replay_pins>=RING-1) { pthread_mutex_unlock(&g_mu); return 0; }
    if (!replay_until((unsigned)(g_cur-g_ring))) g_replay_pins++;
    g_external[g_cur-g_ring].until=UINT64_MAX;
    g_replay_pending=g_cur;
    pthread_mutex_unlock(&g_mu);
    if (!g_optional_immutable) snapshot_replay_pin(UINT64_MAX);
    if (g_external[g_cur-g_ring].signal) DIE("duplicate external signal in recorder slot");
    g_external[g_cur-g_ring].signal=signal;
    uint64_t value=slot_submit(0,VK_PASS_END_PRESENT);
    /* Only CPU queue publication: exporting SYNC_FD requires a pending signal.
     * This never waits for the GPU or for the GL consumer. */
    if (g_submit_started) submit_join(value);
    return value;
}
void vk_submit_wait(VkSemaphore wait) {
    if (!atomic_load_explicit(&g_external_enabled,memory_order_acquire)) DIE("external wait before engine admission");
    (void)vk_cb();
    if (g_external[g_cur-g_ring].wait) DIE("duplicate external wait in recorder slot");
    g_external[g_cur-g_ring].wait=wait;
}
static void wait_value_inner(uint64_t v) {
    uint64_t w0 = host_now_ns();
    pthread_mutex_lock(&g_mu);
    int blocked = atomic_load(&g_done) < v;
#ifdef TOTK_VK_TESTS
    if (v==UINT64_MAX) DIE("test invariant: open replay owner is not a submitted wait ticket");
    if (blocked) atomic_store_explicit(&g_test_wait_value, v, memory_order_release);
#endif
    while (atomic_load(&g_done) < v) pthread_cond_wait(&g_cv, &g_mu);
    if (blocked && g_done_at) HK_ADD(wake_ns, host_now_ns() - g_done_at);
    pthread_mutex_unlock(&g_mu);
    { uint64_t d = host_now_ns() - w0; g_perf_wait_ns += d; g_cum[1] += d;
      { int k = g_wait_kind == 1 ? 1 : 2; g_waitk_ns[k] += d; g_waitk_n[k] += (uint64_t)blocked; }
      if (blocked && T_ft_walk_frame && totk_ft_on()) totk_ft_walk_wait(T_ft_walk_frame, d); }
}
static void wait_value(uint64_t v) { VK_REC_DO(VK_REC_O_GPU_WAIT, wait_value_inner(v)); }
void vk_wait_fence(uint64_t ticket) { wait_value(ticket); } /* exact ticket, never a latest-submission/global idle */
void vk_gpu_idle(void) { uint64_t v, t0 = g_hitch_on ? host_now_ns() : 0; pthread_mutex_lock(&g_mu); v = g_reserved_fence; pthread_mutex_unlock(&g_mu); wait_value(v); HK_ADD(idles, 1); HK_ADD(idle_ns, host_now_ns() - t0); }
static void cpu_step_inner(void) { HK_ADD(cpu_steps, 1); g_stat_cpu_step++; g_cum[3]++; totk_ft_count(FT_COUNT_CPU_STEPS, 1); if (vk_rec_profile_on && T_rec.active) g_rec.cpu_steps++; ub_flush(); if (g_cur) wait_value(slot_submit(0, VK_PASS_END_CPU_WAIT)); else vk_gpu_idle(); }
void vk_cpu_step(void) { VK_REC_DO(VK_REC_S, cpu_step_inner()); }

static pthread_mutex_t g_rec_mu;
static pthread_once_t g_rec_once = PTHREAD_ONCE_INIT;
static _Thread_local unsigned T_rec_depth;
static _Thread_local vk_pcache_span T_pcache_span;
static _Thread_local int T_rec_servicing;
static _Thread_local uint64_t T_rec_hold;
typedef struct rec_service_request {
    struct rec_service_request *next;
    vk_rec_service_fn function; void *arg;
    pthread_cond_t ready;
    uint64_t queued, started;
    int done, deferred, inline_call;
} rec_service_request;
/* This mutex only publishes stack requests/completions and optional statistics. Never hold it across callbacks,
   renderer work, GPU waits or logging. The recording owner alone touches renderer state in each callback. */
static pthread_mutex_t g_rec_service_mu = PTHREAD_MUTEX_INITIALIZER;
static rec_service_request *g_rec_service_head, *g_rec_service_tail;
_Atomic unsigned vk_rec_service_pending;
static int g_rec_service_stats_on;
enum { SERVICE_BINS = 512, SERVICE_BIN_NS = 100000 };
static struct {
    uint64_t calls, inline_calls, cooperative, deferred, delay_max, hold_max, hold_peak, samples;
    uint64_t histogram[SERVICE_BINS];
} g_rec_service_stats;
static void rec_mu_init(void) {
    pthread_mutexattr_t a; pthread_mutexattr_init(&a); pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&g_rec_mu, &a); pthread_mutexattr_destroy(&a);
    const char *e = getenv("TOTK_VK_WB_SERVICE_STATS"); g_rec_service_stats_on = e && atoi(e) > 0;
}
static void rec_service_append(rec_service_request *r) { /* g_rec_service_mu */
    r->next = NULL;
    if (g_rec_service_tail) g_rec_service_tail->next = r; else g_rec_service_head = r;
    g_rec_service_tail = r;
    atomic_fetch_or_explicit(&vk_rec_service_pending, r->deferred ? 2u : 1u, memory_order_relaxed);
}
void vk_rec_service_run(int boundary) {
    if (!T_rec_depth || T_rec_servicing) return;
    if (!atomic_load_explicit(&vk_rec_service_pending, memory_order_relaxed)) return;
    T_rec_servicing = 1;
    uint64_t seal = vk_submit_serial();
    pthread_mutex_lock(&g_rec_service_mu);
    rec_service_request *list = g_rec_service_head;
    g_rec_service_head = g_rec_service_tail = NULL;
    atomic_store_explicit(&vk_rec_service_pending, 0, memory_order_relaxed);
    pthread_mutex_unlock(&g_rec_service_mu);
    while (list) {
        rec_service_request *r = list; list = r->next;
        uint64_t started = host_now_ns();
        int done = (!r->deferred || boundary) && r->function(r->arg, boundary);
        pthread_mutex_lock(&g_rec_service_mu);
        if (!done) {
            if (!r->deferred) g_rec_service_stats.deferred++;
            r->deferred = 1; rec_service_append(r);
        } else {
            r->started = started; r->done = 1;
            g_rec_service_stats.inline_calls += r->inline_call;
            g_rec_service_stats.cooperative += !r->inline_call;
            if (g_rec_service_stats_on) {
                uint64_t delay = started - r->queued;
                unsigned bin = delay / SERVICE_BIN_NS;
                if (bin >= SERVICE_BINS) bin = SERVICE_BINS - 1;
                g_rec_service_stats.histogram[bin]++; g_rec_service_stats.samples++;
                if (delay > g_rec_service_stats.delay_max) g_rec_service_stats.delay_max = delay;
            }
            pthread_cond_signal(&r->ready);
        }
        pthread_mutex_unlock(&g_rec_service_mu);
    }
    T_rec_servicing = 0;
    /* Another ready readback may itself seal the deferred producer while this batch is being serviced.
       Drain those newly-ready requests now, not at the end of unrelated later walking work. */
    if (!boundary && seal != vk_submit_serial() &&
        (atomic_load_explicit(&vk_rec_service_pending, memory_order_relaxed) & 2u))
        vk_rec_service_run(1);
}
/* Standalone backend tests have no guest scheduler. App builds always link
 * threads.c; keep these optional only for backend-only executables. */
extern void runtime_host_wait_begin(runtime_host_wait *) __attribute__((weak));
extern void runtime_host_wait_acquired(runtime_host_wait *) __attribute__((weak));
extern void runtime_host_wait_end(runtime_host_wait *) __attribute__((weak));
static void rec_enter(void) {
    if (!T_rec_depth++ && g_rec_service_stats_on) T_rec_hold = host_now_ns();
    if (T_rec_depth == 1) vk_rec_service_run(1);
}
void vk_rec_lock(void) {
    if (!T_rec_depth) T_pcache_span = vk_pcache_span_begin();
    pthread_once(&g_rec_once, rec_mu_init); pthread_mutex_lock(&g_rec_mu);
    rec_enter();
}
void vk_rec_lock_texels(runtime_host_wait *wait) {
    if (!T_rec_depth) T_pcache_span = vk_pcache_span_begin();
    wait->active = 0;
    pthread_once(&g_rec_once, rec_mu_init);
    int rc = pthread_mutex_trylock(&g_rec_mu);
    if (rc) {
        if (rc != EBUSY) DIE("texel recording try-lock failed: %d", rc);
        if (runtime_host_wait_begin) runtime_host_wait_begin(wait);
        pthread_mutex_lock(&g_rec_mu);
        if (wait->active) runtime_host_wait_acquired(wait);
    }
    rec_enter();
}
void vk_rec_unlock_texels(runtime_host_wait *wait) {
    vk_rec_unlock();
    if (wait->active) runtime_host_wait_end(wait);
}
void vk_rec_unlock(void) {
    if (T_rec_depth > 1) { T_rec_depth--; pthread_mutex_unlock(&g_rec_mu); return; }
    for (;;) {
        vk_rec_service_run(1);
        pthread_mutex_lock(&g_rec_service_mu);
        if (g_rec_service_head) { pthread_mutex_unlock(&g_rec_service_mu); continue; }
        if (g_rec_service_stats_on) {
            uint64_t hold = host_now_ns() - T_rec_hold;
            if (hold > g_rec_service_stats.hold_max) g_rec_service_stats.hold_max = hold;
            if (hold > g_rec_service_stats.hold_peak) g_rec_service_stats.hold_peak = hold;
        }
        /* Serialize the final EMPTY check and actual unlock with enqueue+try-lock. A relaxed idle poll is
           only a hint: it must never be the authority that closes this last-owner/lost-wakeup race.
           This adds one short mutex scope per ownership release, never per idle opcode or draw. */
        T_rec_depth = 0; pthread_mutex_unlock(&g_rec_mu);
        pthread_mutex_unlock(&g_rec_service_mu);
        vk_pcache_span_end(T_pcache_span, "recording-scope");
        return;
    }
}
uint64_t vk_rec_service_call(vk_rec_service_fn function, void *arg) {
    pthread_once(&g_rec_once, rec_mu_init);
    if (T_rec_depth) {
        if (!function(arg, 1)) DIE("owner readback cannot defer itself");
        return 0;
    }
    rec_service_request request = {.function = function, .arg = arg, .ready = PTHREAD_COND_INITIALIZER,
        .queued = host_now_ns()};
    pthread_mutex_lock(&g_rec_service_mu);
    g_rec_service_stats.calls++; rec_service_append(&request);
    int owned = !pthread_mutex_trylock(&g_rec_mu); request.inline_call = owned;
    pthread_mutex_unlock(&g_rec_service_mu);
    if (owned) {
        T_pcache_span = vk_pcache_span_begin();
        T_rec_depth = 1; if (g_rec_service_stats_on) T_rec_hold = host_now_ns();
        vk_rec_service_run(1); vk_rec_unlock();
    }
    pthread_mutex_lock(&g_rec_service_mu);
    while (!request.done) pthread_cond_wait(&request.ready, &g_rec_service_mu);
    pthread_mutex_unlock(&g_rec_service_mu);
    pthread_cond_destroy(&request.ready);
    return request.started - request.queued;
}

/* Finalize never waits for GPU completion with registry or recording ownership.
   The recording lock closes the snapshot-to-destruction gap: it also seals any
   currently recorded work, and all asynchronous worker submissions have already
   reserved tickets before handing their immutable spans to those workers. */
static int pool_gpu_quiet(void) {
    pthread_mutex_lock(&g_mu);
    int quiet = atomic_load(&g_done) >= g_reserved_fence;
    pthread_mutex_unlock(&g_mu);
    return quiet;
}
static pthread_once_t g_pool_stats_once = PTHREAD_ONCE_INIT;
static int g_pool_stats;
static _Atomic uint64_t g_pool_finalizes, g_pool_wait_ns, g_pool_wait_max_ns, g_pool_released_ns;
static void pool_stats_init(void) {
    const char *s = getenv("TOTK_POOL_STATS"); g_pool_stats = s && !strcmp(s, "1");
}
static void pool_stats_report(void) {
    if (!g_pool_stats) return;
    LOGV("[pool-stats] finalize=%llu wait_ms=%.3f wait_max_ms=%.3f core_released_ms=%.3f (cumulative completed calls)",
        (unsigned long long)atomic_load(&g_pool_finalizes), atomic_load(&g_pool_wait_ns) / 1e6,
        atomic_load(&g_pool_wait_max_ns) / 1e6, atomic_load(&g_pool_released_ns) / 1e6);
}
#ifdef TOTK_VK_TESTS
static _Thread_local _Atomic int *T_pool_waited, *T_pool_retry;
static _Thread_local _Atomic uint64_t *T_pool_ticket;
void vk_pool_test_wait_ticket(_Atomic uint64_t *ticket) { T_pool_ticket = ticket; }
void vk_pool_test_pause_after_wait(_Atomic int *waited, _Atomic int *retry) {
    T_pool_waited = waited; T_pool_retry = retry;
}
void vk_pool_test_stats(uint64_t out[4]) {
    out[0] = atomic_load(&g_pool_finalizes); out[1] = atomic_load(&g_pool_wait_ns);
    out[2] = atomic_load(&g_pool_wait_max_ns); out[3] = atomic_load(&g_pool_released_ns);
}
#endif
static uint64_t pool_wait_ticket(uint64_t ticket) {
    uint64_t start = g_pool_stats ? host_now_ns() : 0;
    pthread_mutex_lock(&g_mu);
#ifdef TOTK_VK_TESTS
    atomic_store_explicit(&g_test_wait_value, ticket, memory_order_release);
    if (T_pool_ticket) atomic_store_explicit(T_pool_ticket, ticket, memory_order_release);
#endif
    while (atomic_load(&g_done) < ticket) pthread_cond_wait(&g_cv, &g_mu);
    pthread_mutex_unlock(&g_mu);
    uint64_t elapsed = g_pool_stats ? host_now_ns() - start : 0;
#ifdef TOTK_VK_TESTS
    if (T_pool_waited) {
        atomic_store(T_pool_waited, 1);
        while (!atomic_load(T_pool_retry)) sched_yield();
        T_pool_waited = T_pool_retry = NULL;
    }
#endif
    return elapsed;
}
void mtl_pool_finalize(const void *mem) {
    pthread_once(&g_pool_stats_once, pool_stats_init);
    runtime_host_wait wait = {0};
    uint64_t waited = 0;
    /* Dirty retirement can itself wait for an armed GPU reader. Release before
       entering it, preserving dirty-remove-before-unback ordering. */
    uint64_t initial = mtl_last_fence();
    int pending = atomic_load(&g_done) < initial;
    if (pending && runtime_host_wait_begin) runtime_host_wait_begin(&wait);
    if (pending) waited += pool_wait_ticket(initial);
    uint64_t dirty_start = g_pool_stats && pending ? host_now_ns() : 0;
    nvn_dirty_remove_range(mem);
    if (dirty_start) waited += host_now_ns() - dirty_start;
    for (;;) {
        /* Keep one token through retries: no core reacquisition under rec/pool.
           The first contended recording acquisition can itself release the core. */
        if (wait.active) vk_rec_lock(); else vk_rec_lock_texels(&wait);
        uint64_t ticket = vk_submit_now();
        if (pool_gpu_quiet()) {
            del_range(mem, 0);
            vk_rec_unlock();
            break;
        }
        vk_rec_unlock();
        if (!wait.active && runtime_host_wait_begin) runtime_host_wait_begin(&wait);
        waited += pool_wait_ticket(ticket);
        /* A different producer may have submitted after our snapshot. Reacquire
           recording and recheck, rather than destroying behind that producer. */
    }
    if (wait.active) runtime_host_wait_acquired(&wait);
    uint64_t released = g_pool_stats && wait.released_at_ns ? runtime_now_ns() - wait.released_at_ns : 0;
    if (wait.active) runtime_host_wait_end(&wait);
    if (g_pool_stats) {
        atomic_fetch_add(&g_pool_finalizes, 1);
        atomic_fetch_add(&g_pool_wait_ns, waited);
        atomic_fetch_add(&g_pool_released_ns, released);
        uint64_t maximum = atomic_load(&g_pool_wait_max_ns);
        while (maximum < waited && !atomic_compare_exchange_weak(&g_pool_wait_max_ns, &maximum, waited)) {}
        pool_stats_report();
    }
}
static uint64_t rec_service_percentile(unsigned percent) { /* g_rec_service_mu; upper bound, including overflow */
    uint64_t want = (g_rec_service_stats.samples * percent + 99) / 100, total = 0;
    if (!want) return 0;
    for (unsigned i = 0; i < SERVICE_BINS; i++) {
        total += g_rec_service_stats.histogram[i];
        if (total >= want) return i == SERVICE_BINS - 1 ? g_rec_service_stats.delay_max : (uint64_t)(i + 1) * SERVICE_BIN_NS;
    }
    return 0;
}
static void rec_service_report(uint64_t frame) {
    if (!g_rec_service_stats_on || frame % 300) return;
    pthread_mutex_lock(&g_rec_service_mu);
    uint64_t p50 = rec_service_percentile(50), p99 = rec_service_percentile(99);
    __typeof__(g_rec_service_stats) s = g_rec_service_stats;
    memset(&g_rec_service_stats, 0, sizeof g_rec_service_stats); g_rec_service_stats.hold_peak = s.hold_peak;
    pthread_mutex_unlock(&g_rec_service_mu);
    LOGV("write-back owner service: calls %llu inline/cooperative %llu/%llu deferred %llu; delay p50<=%.3f p99<=%.3f max %.3f ms; rec-lock hold max %.3f ms (peak %.3f); <=64-opcode chunks, same-producer defers to slot seal",
        (unsigned long long)s.calls, (unsigned long long)s.inline_calls, (unsigned long long)s.cooperative,
        (unsigned long long)s.deferred, p50 / 1e6, p99 / 1e6, s.delay_max / 1e6, s.hold_max / 1e6, s.hold_peak / 1e6);
}
#ifdef TOTK_VK_TESTS
void vk_rec_service_test_stats(uint64_t v[8]) {
    pthread_mutex_lock(&g_rec_service_mu);
    v[0] = g_rec_service_stats.calls; v[1] = g_rec_service_stats.inline_calls;
    v[2] = g_rec_service_stats.cooperative; v[3] = g_rec_service_stats.deferred;
    v[4] = g_rec_service_stats.delay_max; v[5] = g_rec_service_stats.hold_max;
    v[6] = rec_service_percentile(50); v[7] = rec_service_percentile(99);
    pthread_mutex_unlock(&g_rec_service_mu);
}
#endif
/* Chunked submission (vk_draw.c, at the start of a render pass): inside a game submission, what is recorded so far goes to
   the GPU, so it works while the rest of the frame is recorded. The game waits for its previous frame's GPU work before
   the next one, so with one Vulkan submission per frame the GPU idled during recording and the CPU during the GPU's work
   (frame = recording + GPU time). Post ops stay with the game submission's last part (mtl_sub_commit). */
static __thread const char *g_lag_guest_role;
#ifdef TOTK_VK_TESTS
uint64_t vk_record_test_lag_fallbacks(void) {
    return atomic_load_explicit(&g_test_lag_fallbacks, memory_order_relaxed);
}
uint64_t vk_record_test_wait_value(void) {
    return atomic_load_explicit(&g_test_wait_value, memory_order_acquire);
}
#endif
/* Recording-owner selector. Qualification must precede even its first getenv.
 * Scheduler default: absent means on; exact "0" restores the begin/commit-only
 * r6b path; an invalid spelling is off (logged once). */
static int g_fg_adaptive_chunk=-1;
static int fg_adaptive_chunk_on(void) {
    if (g_fg_adaptive_chunk<0) {
        const char *e=getenv("TOTK_FG_ADAPTIVE_CHUNK");
        int request=fga_default_on_request(e);
        if (request<0) LOGV("[adaptive-chunk] selector=invalid raw=\"%.32s\" enabled=0 (absent or exact \"1\" enables; \"0\" disables)",e);
        g_fg_adaptive_chunk=request==1;
    }
    return g_fg_adaptive_chunk;
}
#ifdef TOTK_VK_TESTS
void vk_record_test_chunk_selector_reset(void) { g_fg_adaptive_chunk=-1; }
#endif
int vk_submit_chunk_possible(void) { return g_in_sub && g_cur; }
void vk_submit_chunk(void) {
    if (!g_in_sub || !g_cur) return;
    g_chunk_subs++;
    int chunk=__builtin_expect(atomic_load_explicit(&vk_fg_slot_boundary_active,memory_order_relaxed),0) &&
        fg_adaptive_chunk_on();
    int slot_trace=chunk && vk_fg_job_trace_enabled();
    uint64_t slot_submit_begin=slot_trace ? host_now_ns() : 0;
    uint64_t v=slot_submit(0,VK_PASS_END_SUBMISSION);
    uint64_t slot_submit_return=slot_trace ? host_now_ns() : 0;
    vk_rec_service_run(1);
    uint64_t slot_service_return=slot_trace ? host_now_ns() : 0;
    if (!g_cur && __builtin_expect(atomic_load_explicit(&vk_fg_slot_boundary_active,memory_order_relaxed),0) &&
        fg_adaptive_chunk_on()) {
        uint64_t enter=slot_trace ? host_now_ns() : 0;
        if (slot_trace) LOGV("[adaptive-slot-boundary] event=enter kind=chunk boundary_ns=%llu submit_begin_ns=%llu submit_return_ns=%llu service_return_ns=%llu real_ticket=%llu open_primary=0 clock=CLOCK_MONOTONIC",
            (unsigned long long)enter,(unsigned long long)slot_submit_begin,
            (unsigned long long)slot_submit_return,(unsigned long long)slot_service_return,
            (unsigned long long)v);
        /* Optional children are backend-only. Leave immutable guest context and
         * pending posts with the interrupted walk; its original final seal owns
         * all posts, UBO publication and guest writeback. No allocator lock is held. */
        int in_sub=g_in_sub;
        g_in_sub=0;
        vk_fg_slot_boundary();
        g_in_sub=in_sub;
        if (slot_trace) LOGV("[adaptive-slot-boundary] event=exit kind=chunk enter_ns=%llu exit_ns=%llu real_ticket=%llu clock=CLOCK_MONOTONIC",
            (unsigned long long)enter,(unsigned long long)host_now_ns(),(unsigned long long)v);
    } else if (slot_trace) {
        LOGV("[adaptive-slot-boundary] event=skip kind=chunk now_ns=%llu submit_begin_ns=%llu submit_return_ns=%llu service_return_ns=%llu real_ticket=%llu open_primary=%u active=%u clock=CLOCK_MONOTONIC",
            (unsigned long long)host_now_ns(),(unsigned long long)slot_submit_begin,
            (unsigned long long)slot_submit_return,(unsigned long long)slot_service_return,
            (unsigned long long)v,g_cur!=NULL,
            atomic_load_explicit(&vk_fg_slot_boundary_active,memory_order_relaxed)!=0);
    }
}
/* ---------- in-flight check: TOTK_OHOS_INFLIGHT_CHECK=K (a diagnostic: the renderer does what it did) ----------
   With two frames in flight (hle_nvn.c TOTK_OHOS_SYNC_LAG=1, TOTK_VK_RING=16) the game rewrites memory that slots still
   on the GPU read, and the picture breaks. This shows what changes, before any protection is built
   (docs/design-2026-11-13-two-frames-in-flight.md, step 1).
   Every K-th frame is sampled. In a sampled frame each range of game-writable pool memory that the slot being recorded
   makes the GPU read is noted (vk_inflight_note, from where the backend resolves it: vertex streams, index ranges,
   indirect records, uniform and storage bindings of draws and dispatches, texel buffers, copy sources) with its pool,
   the pool's flags and a hash of each of its 4 KiB pages: H0. A slot that binds the memory pool table also notes a few
   64 KiB windows of all CPU-writable GPU-backed pool memory, a different few each time (ic_census): its shaders reach
   memory nobody bound. The pages are hashed again
     H1  when the game submission is committed (ic_sub_commit, the walker, after its last slot is queued): a difference
         from H0 means something wrote during the replay itself. H1 stops (counted) once the submission's last noted
         slot has begun H2: its posts may then have released the game, whose writes after that are legitimate.
         TOTK_OHOS_INFLIGHT_H1=0: no H1 (it lengthens the game's submit call in sampled frames);
     H2  when the slot's GPU work is done and before its completion is published (ic_slot_complete, the completion
         thread): a difference means the bytes changed under a slot in flight, which is the bug.
   Each changed page is decided on its own against the GPU writes recorded from the frame before the slot's on
   (ic_gpu_wrote: writable storage and texel bindings, copies and fills, inline uniform updates, counter reports and
   event values, write-back landings; kept from two frames before a sampled frame to two after it). A page under a GPU
   write of known size is the GPU's and counted apart. One under only an inline uniform update (the game also writes
   constant memory from the CPU) or only a size-less GPU write (one that runs to the end of its pool) is not excused:
   it stays with the changes and is shown apart. The changes are sorted by the pool's CPU_UNCACHED / CPU_CACHED flag
   jointly with whether the game flushed them (vk_inflight_stamp: nvnMemoryPoolFlushMappedRange,
   nvnBufferFlushMappedRange): before H0, between H0 and H2, both, not at all, or only after H2 (found again when the
   frame closes); and each frame's distinct changed pages are counted. Reads whose size came from neither the game nor
   the shader (to the end of their pool) have a row of their own and stay out of the per-kind MiB and the distinct
   count. Shaders that store through the pool table (they bind its write log) leave no GPU-write record: the slots that
   do are counted, and the changes of slots exposed to them (that slot, one before it still in flight when it opened,
   or one after it recorded by the time of the H1 or H2 that saw the change: it may run meanwhile) shown apart; the
   joint flag/flush table keeps those, and the pages under only an inline uniform update or a size-less GPU write,
   apart from the game's own. ic_report prints every 300 frames.
   Cost: a range up to 1 MiB is hashed whole; a larger one in 256 of its pages, other pages each time (the changed MiB
   is then extrapolated). TOTK_OHOS_INFLIGHT_MB (48) caps the MiB hashed at H0 in a frame, so H1 and H2 hash no more:
   each slot that notes adds its share (the cap over the slots of the last sampled frame) to what the frame may have
   hashed; past 3/4 of that a new range gets 16 pages, past all of it a new range is not hashed (counted). Census
   windows have their own cap, 16 times TOTK_OHOS_INFLIGHT_CENSUS_KB a frame.
   Threads: a slot's table is written by the recording-lock owner while the slot is open, only read once it is sealed
   (the walker at H1, the completion thread at H2), and reset when the slot is opened again, which is after its
   completion. Guest memory is read in pieces of at most 64 KiB, each between ic_read_begin and ic_read_end, and only
   while the pool generation shows the range is still the pool mapping it was at H0 (or its pool's registration is
   found again: ic_ent_live) and no lazy backing has moved pages under a registered pool since (g_ic_map_gen);
   del_range_ent and such a backing wait for the piece being read to end (ic_pool_barrier). The completion thread
   never waits for the pool lock: del_range_ent may hold it while it waits for the GPU. */
enum { IC_GMEM = VK_INFLIGHT_KINDS, IC_RW, IC_UNB, IC_NROW,   /* report rows: the kinds, the pool-table census, bindings the GPU writes, reads of unknown size */
       IC_PAGE = 4096, IC_FULL_PAGES = 256, IC_SPARSE_PAGES = 16, IC_WIN = 65536, IC_PIECE = 65536,
       IC_STAMPS = 16384, IC_GW = 1024, IC_OFF = 4096, IC_SET = 1 << 16, IC_BIG = 64, IC_SUB = 128, IC_LATE = 4096, IC_SW_TAIL = 512 };
enum { IC_SKIP_NOACCESS = 1, IC_SKIP_UNBACKED, IC_SKIP_COPY, IC_SKIP_RACED, IC_SKIP_MEMORY, IC_SKIP_BUDGET };
enum { IC_CH_GAME, IC_CH_AMB, IC_CH_UB, IC_CH_GPU };   /* a changed page: no GPU write over it; only a size-less one; an inline uniform update; a GPU write of known size */
enum { IC_ST_BEFORE, IC_ST_INFLIGHT, IC_ST_BOTH, IC_ST_NEVER, IC_ST_LATE, IC_ST_LOST, IC_ST_N };   /* the game's flushes over a changed page */
static const char *const ic_names[IC_NROW] = {"vertex", "index", "indirect", "uniform", "storage", "storage (compute)", "texel",
                                             "copy source", "pool-table census", "GPU-writable bindings", "reads of unknown size (to the end of their pool)"};
typedef struct {
    uint64_t addr, size, key_size;         /* the range, clamped to its pool; the size the site asked for (the table's key) */
    uint64_t span_gen, map_gen, identity, stamp0;   /* at H0: pool generation, lazy-move generation, registration of its K_OWN pool (0: through an alias), flush stamp generation */
    uint64_t pool_lo, pool_size;           /* the registered pool it lies in */
    uint32_t ph, np, stride, phase, uses;  /* page hashes in the table's ph[]: first, count; every stride-th 4 KiB page from page `phase` */
    int32_t flags;                         /* that pool's NVN flags, -1 unknown */
    uint8_t kind, rw, skip, unb, row;      /* rw: bound for the GPU to write; skip: IC_SKIP_*, no hashes; unb: size from neither the game nor the shader; row: report row */
} ic_ent;
typedef struct {
    ic_ent *n; uint32_t nn, capn;
    uint32_t *ph, nph, capph;
    uint32_t *idx, capidx;                 /* open addressing: (kind, rw, address, asked size) -> entry + 1 */
    uint64_t gen, frame, stamp_prev, seq;  /* resets so far; the frame of its notes; the stamp generation when the frame before that began; its place in queue order */
    int done, listed, census, gml, exposed;   /* H2 taken; in this game submission's H1 list; the census was noted; it binds the pool table's write log; it or a slot before it still in flight when it opened does (H1 and H2 also look for a later slot that does: g_ic_gml_last) */
    _Atomic int h2;                        /* H2 has begun (seq_cst): H1 of its submission stops */
} ic_tab;
typedef struct { uintptr_t lo, end; uint64_t pool_lo, pool_size, gen, map_gen, identity; VkBuffer buf; int flags; } ic_span;
typedef struct { uint64_t identity, gen; int ok; } ic_live;
typedef struct { uint64_t *set; uint32_t mask; uint64_t gen; int state; ic_live cache[16]; } ic_ids;   /* one H1 or H2 pass: the K_OWN registrations */
typedef struct { uint64_t lo, hi; uint32_t tag; } ic_gwr;
typedef struct { uint64_t ranges, bytes, uses, pages, hpages, h1n, h1b, h1ab, h1ub, h1gn, h1gb, h1xn, h1xb; } ic_wrow;   /* noted at H0; changed at H1 (ab: only a size-less GPU write, ub: an inline uniform update; g: GPU-written; x: in slots exposed to pool-table stores) */
typedef struct {   /* the walker's side of a report window; all uint64_t */
    uint64_t frames, slots, notes, paged, sparse, hashed0, ns0, hashed1, ns1, h1_skipped, h1_reused, h1_late;
    uint64_t no_access, no_access_bytes, unbacked, copies, raced, dropped, census_pop, census_n, census_ns, census_capped;
    uint64_t budget, budget_bytes, span_miss, gbuf_capped, gml_slots;
    ic_wrow k[IC_NROW];
} ic_wstat;
typedef struct { uint64_t n, b, cn, cb, an, ab, un, ub, xn, xb, gn, gb; } ic_crow;   /* hashed again at H2; changed, not GPU-written (of them: a only a size-less GPU write, u an inline uniform update, x in slots exposed to pool-table stores); GPU-written */
typedef struct {   /* the completion thread's side; all uint64_t (ic_add64) */
    uint64_t slots, slots_exposed, hashed, ns, skipped, skipped_bytes, stamp_lost, late_over;
    uint64_t pf[3][3][4][IC_ST_N];   /* [bound ranges, census, reads of unknown size][o: 0 the game's page, 1 the game's page in a slot exposed to pool-table stores, 2 a page under only an inline uniform update or a size-less GPU write][CPU_UNCACHED, CPU_CACHED, neither, unknown][IC_ST_*]: changed bytes not GPU-written */
    ic_crow k[IC_NROW];
} ic_cstat;
typedef struct { uint64_t addr, size, pool_lo, pool_size, uses, bytes, seen_frame, chg_frame; uint32_t seen, chg; int32_t flags; uint8_t kind, rw, game, used, unb; } ic_off;   /* game: 2 a change no GPU write was over, 1 only under inline uniform updates or size-less GPU writes, 0 GPU-written; unb: a read of unknown size */
typedef struct { uintptr_t lo, hi; uint64_t gen; } ic_stamp;

__attribute__((visibility("hidden"))) int vk_inflight_on;   /* vk_int.h */
static uint32_t g_ic_budget_mb = 48, g_ic_census_kb = 1024; static int g_ic_h1 = 1;
static uint64_t g_ic_frame;               /* the frame being recorded (presents so far + 1): written at present, read with __atomic (ic_gpu_wrote may run elsewhere) */
static uint64_t g_ic_frame_h0, g_ic_frame_census, g_ic_phase, g_ic_stamp_cur, g_ic_stamp_prev;   /* recording-lock owner */
static uint32_t g_ic_fslots, g_ic_fslots_prev;   /* recording-lock owner: slots that noted in this sampled frame, in the last one */
static ic_tab g_ic_tab[RING_MAX], g_ic_pend;   /* by ring cell; notes made while no slot is open */
static struct { uint32_t slot; uint64_t gen; } g_ic_sub[IC_SUB]; static int g_ic_nsub;   /* the tables of the game submission being walked */
static ic_wstat g_ic_w;                   /* recording-lock owner */
static uint64_t g_ic_seq; static int g_ic_pend_gml;   /* recording-lock owner: slots opened; the write log bound while no slot was open */
static _Atomic uint64_t g_ic_gml_last, g_ic_done_seq;   /* seq of the last slot that binds the write log (walker); of the last completed slot (completion thread) */
static _Atomic uint64_t g_ic_foreign, g_ic_gw_lost;
static pthread_mutex_t g_ic_mu = PTHREAD_MUTEX_INITIALIZER;   /* the completion thread's window and the offenders */
static struct { int open; uint64_t frame; ic_cstat s; } g_ic_cf;   /* completion thread: the frame whose slots are completing */
static struct { uint64_t frames, distinct, distinct_max, est, set_over; ic_cstat s; } g_ic_cw;   /* g_ic_mu: whole frames since the last report */
static ic_off g_ic_off[IC_OFF]; static uint64_t g_ic_off_lost;   /* g_ic_mu */
static uint64_t *g_ic_set; static uint32_t g_ic_set_n, g_ic_set_over;   /* completion thread: the frame's changed pages */
static struct { uint64_t addr, size, est; } g_ic_big[IC_BIG]; static int g_ic_nbig; static uint64_t g_ic_big_over;   /* its changed page-sampled ranges */
static struct { uint64_t lo, hi, w, gen; uint8_t c3, o, pc, st; } g_ic_late[IC_LATE]; static uint32_t g_ic_nlate;   /* completion thread: its changed pages not flushed by H2 */
static struct { uintptr_t lo, hi; } g_ic_st[IC_STAMPS]; static _Atomic uint64_t g_ic_st_gen, g_ic_st_n[2], g_ic_st_bytes[2];
static pthread_mutex_t g_ic_st_mu = PTHREAD_MUTEX_INITIALIZER;
static ic_stamp g_ic_sw[IC_STAMPS], g_ic_sw_t[IC_SW_TAIL]; static uintptr_t g_ic_sw_mx[IC_STAMPS];   /* completion thread: the flush ring's window */
static struct { uint32_t nbase, n; uint64_t since, start, end; int lost, valid; } g_ic_swi;
static struct { uint64_t lo, hi, frame; uint32_t tag; } g_ic_gw[IC_GW]; static pthread_mutex_t g_ic_gw_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_rwlock_t g_ic_rw = PTHREAD_RWLOCK_INITIALIZER; static _Atomic int g_ic_rw_want;

static void ic_parse(void) {
    const char *e = getenv("TOTK_OHOS_INFLIGHT_CHECK"); long k = e && *e ? atol(e) : 0;
    g_ic_k = k <= 0 ? 0 : k > 1000000 ? 1000000 : (int)k;
    if (!g_ic_k) return;
    e = getenv("TOTK_OHOS_INFLIGHT_MB"); if (e && *e) { long n = atol(e); g_ic_budget_mb = n < 1 ? 1u : n > 4096 ? 4096u : (uint32_t)n; }
    e = getenv("TOTK_OHOS_INFLIGHT_CENSUS_KB"); if (e && *e) { long n = atol(e); g_ic_census_kb = n < 0 ? 0u : n > 4096 ? 4096u : (uint32_t)n; }
    e = getenv("TOTK_OHOS_INFLIGHT_H1"); if (e && *e == '0') g_ic_h1 = 0;
    LOGV("inflight check: on, one frame in %d is sampled (TOTK_OHOS_INFLIGHT_CHECK); a range up to %d KiB is hashed whole, a larger one in %d of its 4 KiB pages; "
         "at most %u MiB hashed at H0 in a frame (TOTK_OHOS_INFLIGHT_MB; each slot adds its share; near the limit a range gets %d pages, past it none); "
         "pool-table census %u KiB a slot, at most %u MiB a frame (TOTK_OHOS_INFLIGHT_CENSUS_KB, 0 = off); H1 %s (TOTK_OHOS_INFLIGHT_H1)",
         g_ic_k, IC_FULL_PAGES * IC_PAGE / 1024, IC_FULL_PAGES, g_ic_budget_mb, IC_SPARSE_PAGES, g_ic_census_kb, g_ic_census_kb / 64, g_ic_h1 ? "on" : "off");
}
static void ic_add64(void *dst, const void *src, size_t bytes) { uint64_t *d = dst; const uint64_t *s = src; for (size_t i = 0; i < bytes / 8; i++) d[i] += s[i]; }
/* Four independent 64-bit lanes, one multiply a word: fast enough to be memory-bound, and a page's 32-bit result only has
   to differ when the page does. */
static inline uint64_t ic_rotl(uint64_t x, unsigned r) { return x << r | x >> (64 - r); }
static uint32_t ic_hash(const uint8_t *p, size_t n) {
    const uint64_t P = 0x9E3779B185EBCA87ull;
    uint64_t a = 0x243F6A8885A308D3ull ^ n, b = 0x13198A2E03707344ull, c = 0xA4093822299F31D0ull, d = 0x082EFA98EC4E6C89ull, w[4];
    for (; n >= 32; p += 32, n -= 32) {
        memcpy(w, p, 32);
        a = ic_rotl(a ^ w[0], 27) * P; b = ic_rotl(b ^ w[1], 27) * P; c = ic_rotl(c ^ w[2], 27) * P; d = ic_rotl(d ^ w[3], 27) * P;
    }
    for (; n >= 8; p += 8, n -= 8) { memcpy(w, p, 8); a = ic_rotl(a ^ w[0], 27) * P; uint64_t t = a; a = b; b = c; c = d; d = t; }
    if (n) { w[0] = 0; memcpy(w, p, n); a = ic_rotl(a ^ w[0], 27) * P; }
    uint64_t h = a ^ ic_rotl(b, 17) ^ ic_rotl(c, 31) ^ ic_rotl(d, 47);
    h ^= h >> 33; h *= 0xff51afd7ed558ccdull; h ^= h >> 29;
    return (uint32_t)(h ^ h >> 32);
}
/* Reading guest memory. A reader holds the lock shared for one piece of at most 64 KiB and waits for nothing meanwhile. */
static void ic_read_begin(void) {
    while (atomic_load_explicit(&g_ic_rw_want, memory_order_acquire)) sched_yield();   /* a pool is going: let its barrier through */
    pthread_rwlock_rdlock(&g_ic_rw);
}
static void ic_read_end(void) { pthread_rwlock_unlock(&g_ic_rw); }
/* del_range_ent (the pool out of the registry and the generation moved) or lazy_back_units (g_ic_map_gen odd), pool lock
   held: a piece being read sees it through; one that begins after finds a generation changed and reads nothing. */
static void ic_pool_barrier(void) {
    atomic_fetch_add_explicit(&g_ic_rw_want, 1, memory_order_release);
    pthread_rwlock_wrlock(&g_ic_rw); pthread_rwlock_unlock(&g_ic_rw);
    atomic_fetch_sub_explicit(&g_ic_rw_want, 1, memory_order_release);
}
/* The game's flushes, in their own ring: the textures' flush record (vk_res.c vk_pool_flush_hint) decides what is hashed
   and uploaded, and buffer flushes must not reach it. */
void vk_inflight_stamp(const void *addr, size_t n, int source) {
    if (__builtin_expect(g_pt_on, 0)) vk_pt_stamp(addr, n);   /* oot-a122: the game's flushes make bound ranges hot */
    if (!g_ic_k || !n) return;
    pthread_mutex_lock(&g_ic_st_mu);
    uint64_t g = atomic_load_explicit(&g_ic_st_gen, memory_order_relaxed);
    g_ic_st[g % IC_STAMPS].lo = (uintptr_t)addr; g_ic_st[g % IC_STAMPS].hi = (uintptr_t)addr + n;
    atomic_store_explicit(&g_ic_st_gen, g + 1, memory_order_release);
    pthread_mutex_unlock(&g_ic_st_mu);
    atomic_fetch_add_explicit(&g_ic_st_n[source != 0], 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_ic_st_bytes[source != 0], n, memory_order_relaxed);
}
/* The completion thread's copy of the flushes since generation `since` (the frame's stamp_prev), taken under the lock
   once per H2 pass and only extended by the next pass of the same frame: a part sorted by lo with the running maximum
   of hi, and a short unsorted tail. Matching is done with the lock released. lost: the ring no longer held `since`. */
static int ic_sw_cmp(const void *x, const void *y) { uintptr_t a = ((const ic_stamp *)x)->lo, b = ((const ic_stamp *)y)->lo; return a < b ? -1 : a > b; }
static void ic_sw_index(void) {   /* the running maximum of hi over the sorted part */
    uintptr_t m = 0;
    for (uint32_t k = 0; k < g_ic_swi.nbase; k++) { if (g_ic_sw[k].hi > m) m = g_ic_sw[k].hi; g_ic_sw_mx[k] = m; }
}
static void ic_sw_load(uint64_t since) {
    pthread_mutex_lock(&g_ic_st_mu);
    uint64_t cur = atomic_load_explicit(&g_ic_st_gen, memory_order_relaxed);
    int again = !g_ic_swi.valid || g_ic_swi.since != since || cur - g_ic_swi.start > IC_STAMPS;
    uint64_t from = again ? (cur - since > IC_STAMPS ? cur - IC_STAMPS : since) : g_ic_swi.end;
    if (again) { g_ic_swi.n = g_ic_swi.nbase = 0; g_ic_swi.since = since; g_ic_swi.start = from; g_ic_swi.lost = from > since; g_ic_swi.valid = 1; }
    for (uint64_t g = from; g < cur; g++) g_ic_sw[g_ic_swi.n++] = (ic_stamp){g_ic_st[g % IC_STAMPS].lo, g_ic_st[g % IC_STAMPS].hi, g};
    g_ic_swi.end = cur;
    pthread_mutex_unlock(&g_ic_st_mu);
    uint32_t nb = g_ic_swi.nbase, nt = g_ic_swi.n - nb;
    if (nt > IC_SW_TAIL) { qsort(g_ic_sw, g_ic_swi.n, sizeof *g_ic_sw, ic_sw_cmp); g_ic_swi.nbase = g_ic_swi.n; ic_sw_index(); }
    else if (nt > 64) {   /* the tail sorted and merged in from the top */
        memcpy(g_ic_sw_t, g_ic_sw + nb, nt * sizeof *g_ic_sw_t);
        qsort(g_ic_sw_t, nt, sizeof *g_ic_sw_t, ic_sw_cmp);
        for (uint32_t i = nb, j = nt, k = nb + nt; j > 0; ) {
            if (i > 0 && g_ic_sw[i - 1].lo > g_ic_sw_t[j - 1].lo) g_ic_sw[--k] = g_ic_sw[--i];
            else g_ic_sw[--k] = g_ic_sw_t[--j];
        }
        g_ic_swi.nbase = nb + nt; ic_sw_index();
    }
}
/* Whether flushes in the window cover any of [lo, hi): one with a generation below thr (before), one at or above (after). */
static void ic_sw_query(uintptr_t lo, uintptr_t hi, uint64_t thr, int *before, int *after) {
    int b = 0, a = 0;
    for (uint32_t i = g_ic_swi.nbase; i < g_ic_swi.n && !(a && b); i++)
        if (g_ic_sw[i].lo < hi && lo < g_ic_sw[i].hi) { if (g_ic_sw[i].gen < thr) b = 1; else a = 1; }
    uint32_t l = 0, r = g_ic_swi.nbase;   /* the first sorted flush that starts at or past hi */
    while (l < r) { uint32_t m = (l + r) / 2; if (g_ic_sw[m].lo < hi) l = m + 1; else r = m; }
    for (uint32_t i = l; i-- > 0 && !(a && b) && g_ic_sw_mx[i] > lo; )
        if (g_ic_sw[i].hi > lo) { if (g_ic_sw[i].gen < thr) b = 1; else a = 1; }
    *before = b; *after = a;
}
/* GPU writes, with the frame they were recorded in and a tag (vk_int.h VK_INFLIGHT_GW_*): gpu_write_inner (writable
   storage bindings, buffer copies and fills, texture-to-buffer copies), inline uniform updates, counter reports and
   event values, write-back landings, writable texel views, and the writable bindings noted here. Only frames within two
   of a sampled one are recorded (ic_gw_window): a record is read for the frame before a slot's and later ones. */
static inline int ic_gw_window(void) {   /* g_ic_k is set */
    uint64_t k = (uint64_t)g_ic_k, r = __atomic_load_n(&g_ic_frame, __ATOMIC_RELAXED) % k;
    return k <= 5 || r >= k - 2 || r <= 2;
}
static void ic_gpu_wrote(uint64_t a, uint64_t n, unsigned tag) {
    if (!n) return;
    uint64_t frame = __atomic_load_n(&g_ic_frame, __ATOMIC_RELAXED); unsigned h = (unsigned)(((a >> 6) ^ (a >> 22) ^ n) * 0x9E3779B1u) % IC_GW; int victim = -1;
    pthread_mutex_lock(&g_ic_gw_mu);
    for (unsigned p = 0; p < 8; p++) {
        int i = (int)((h + p) % IC_GW);
        if (g_ic_gw[i].lo == a && g_ic_gw[i].hi == a + n) {   /* a size-less binding notes UNBOUNDED before its gpu_write_inner PLAIN note: that note must not undo it */
            if (tag != VK_INFLIGHT_GW_PLAIN || g_ic_gw[i].frame != frame) g_ic_gw[i].tag = tag;
            g_ic_gw[i].frame = frame; victim = -2; break;
        }
        if (victim < 0 || g_ic_gw[i].frame < g_ic_gw[victim].frame) victim = i;
    }
    if (victim >= 0) {
        if (g_ic_gw[victim].hi && g_ic_gw[victim].frame + 1 >= frame) atomic_fetch_add_explicit(&g_ic_gw_lost, 1, memory_order_relaxed);
        g_ic_gw[victim].lo = a; g_ic_gw[victim].hi = a + n; g_ic_gw[victim].frame = frame; g_ic_gw[victim].tag = tag;
    }
    pthread_mutex_unlock(&g_ic_gw_mu);
}
void vk_inflight_gpu_wrote(uint64_t addr, uint64_t size, int tag) {   /* vk_int.h: callers test g_ic_k first */
    if (!g_ic_k || !ic_gw_window()) return;
    ic_gpu_wrote(addr, size, (unsigned)tag);
}
/* The records over [a, b) from the frame before `frame` on, copied in one pass under the lock (out: IC_GW long). */
static int ic_gw_collect(uint64_t a, uint64_t b, uint64_t frame, ic_gwr *out) {
    int n = 0;
    pthread_mutex_lock(&g_ic_gw_mu);
    for (int i = 0; i < IC_GW; i++)
        if (g_ic_gw[i].hi && g_ic_gw[i].lo < b && a < g_ic_gw[i].hi && g_ic_gw[i].frame + 1 >= frame) out[n++] = (ic_gwr){g_ic_gw[i].lo, g_ic_gw[i].hi, g_ic_gw[i].tag};
    pthread_mutex_unlock(&g_ic_gw_mu);
    return n;
}
static unsigned ic_gw_class(const ic_gwr *g, int ng, uint64_t lo, uint64_t hi) {   /* IC_CH_* of one changed page */
    unsigned c = IC_CH_GAME;
    for (int i = 0; i < ng; i++) if (g[i].lo < hi && lo < g[i].hi) {
        if (g[i].tag == VK_INFLIGHT_GW_PLAIN) return IC_CH_GPU;
        if (g[i].tag == VK_INFLIGHT_GW_UNIFORM) c = IC_CH_UB;
        else if (c == IC_CH_GAME) c = IC_CH_AMB;
    }
    return c;
}
/* The GPU-backed memory that holds an address: its pool's buffer, clipped to the smallest registered pool around it, with
   that pool's flags. Caller holds g_pool_mu. */
static void ic_span_pool(ic_span *s, const pool_ent *p) {
    s->pool_lo = p->group_size ? p->group_lo : p->lo; s->pool_size = p->group_size ? p->group_size : p->hi - p->lo;
#ifdef TOTK_OHOS
    s->flags = p->flags;
#endif
}
static void ic_clip(uintptr_t a, uintptr_t lo, uintptr_t hi, uintptr_t *vlo, uintptr_t *vhi) {   /* [lo, hi) does not hold a: keep it out of a's range */
    if (hi <= a) { if (hi > *vlo) *vlo = hi; }
    else if (lo > a) { if (lo < *vhi) *vhi = lo; }
}
/* [*vlo, *vhi): the addresses around a that get this same answer (the same alias or K_OWN pool, the same smallest
   registered pool), which is what ic_span_of may cache it for. */
static int ic_span_find(uintptr_t a, ic_span *out, uintptr_t *vlo, uintptr_t *vhi) {
    const pool_ent *own = NULL, *inner = NULL; const alias_ent *al = NULL;
    for (int i = 0; i < g_nalias && !al; i++) if (a >= g_alias[i].va && a < g_alias[i].end) al = &g_alias[i];
    for (int i = 0; i < g_npool; i++) {
        const pool_ent *p = g_pools[i];
        if (!al && !own && p->kind == K_OWN && a >= p->base && a < p->end) own = p;
        if (a >= p->lo && a < p->hi && (!inner || p->hi - p->lo < inner->hi - inner->lo)) inner = p;
    }
    if (!al && !own) return 0;
    memset(out, 0, sizeof *out);
    out->lo = al ? al->va : own->base; out->end = al ? al->end : own->end;
    out->buf = al ? al->owner->buf : own->buf; out->identity = al ? 0 : own->identity; out->flags = -1;
    if (inner) { ic_span_pool(out, inner); if (inner->lo > out->lo) out->lo = inner->lo; if (inner->hi < out->end) out->end = inner->hi; }
    uintptr_t l = out->lo, h = out->end;
    for (int i = 0; i < g_nalias; i++) { const alias_ent *x = &g_alias[i]; if (x != al && !(a >= x->va && a < x->end)) ic_clip(a, x->va, x->end, &l, &h); }
    for (int i = 0; i < g_npool; i++) {
        const pool_ent *p = g_pools[i];
        if (!al && p != own && p->kind == K_OWN && !(a >= p->base && a < p->end)) ic_clip(a, p->base, p->end, &l, &h);
        if (p != inner && !(a >= p->lo && a < p->hi) && (!inner || p->hi - p->lo <= inner->hi - inner->lo)) ic_clip(a, p->lo, p->hi, &l, &h);
    }
    *vlo = l; *vhi = h;
    return 1;
}
static int ic_span_of(uint64_t addr, ic_span *out) {   /* recording-lock owner */
    static struct { ic_span s; uintptr_t vlo, vhi; } cache[1024];   /* the last answer per 64 KiB of address, for the range it holds for, while the pools stay as they are */
    __typeof__(cache[0]) *c = &cache[(addr >> 16) & 1023]; uint64_t gen = atomic_load_explicit(&g_span_gen, memory_order_acquire);
    if (c->s.gen == gen && addr >= c->vlo && addr < c->vhi) { *out = c->s; return 1; }
    g_ic_w.span_miss++;
    uintptr_t vlo = 0, vhi = 0;
    pthread_mutex_lock(&g_pool_mu);
    int ok = ic_span_find((uintptr_t)addr, out, &vlo, &vhi); gen = atomic_load_explicit(&g_span_gen, memory_order_relaxed);
    pthread_mutex_unlock(&g_pool_mu);
    if (ok) { out->gen = gen; c->s = *out; c->vlo = vlo; c->vhi = vhi; }
    return ok;
}
static void gwl_note(uint64_t a, uint64_t n, const char *what) {   /* TOTK_OHOS_GW_LOG (above vk_slot_stats); recording-lock owner */
    ic_span sp; int ok = ic_span_of(a, &sp);
    LOGV("[gwlog] f=%llu sub=%llu %s %#llx+%#llx pool %#llx+%#llx flags %#x%s", (unsigned long long)g_gwl_frame,
         (unsigned long long)(vk_submit_serial() - g_gwl_serial0), what, (unsigned long long)a, (unsigned long long)n,
         ok ? (unsigned long long)sp.pool_lo : 0ull, ok ? (unsigned long long)sp.pool_size : 0ull, ok ? (unsigned)sp.flags : 0u,
         ok ? ((sp.flags & 0x2) ? " CPU_UNCACHED" : (sp.flags & 0x4) ? " CPU_CACHED" : (sp.flags & 0x1) ? " CPU_NO_ACCESS" : "") : " (no pool)");
}
#ifdef TOTK_OHOS
/* TOTK_OHOS_POOL_CENSUS=1: the bytes from addr to the end of the smallest game buffer that holds it, 0 when none does. */
static uint64_t ic_gbuf_rest(uint64_t addr) {
    uint64_t best = 0;
    pthread_mutex_lock(&g_gbuf_mu);
    for (int i = 0; i < g_ngbuf; i++) if (addr >= g_gbuf[i].a && addr < g_gbuf[i].b && (!best || g_gbuf[i].b - addr < best)) best = g_gbuf[i].b - addr;
    pthread_mutex_unlock(&g_gbuf_mu);
    return best;
}
#endif
/* A slot's table. */
static uint32_t ic_key_hash(unsigned kr, uint64_t addr, uint64_t size) {
    uint64_t h = (addr ^ (uint64_t)kr << 56) * 0x9E3779B97F4A7C15ull ^ size * 0xC2B2AE3D27D4EB4Full;
    return (uint32_t)(h >> 32 ^ h >> 7);
}
static ic_ent *ic_tab_find(const ic_tab *t, unsigned kr, uint64_t addr, uint64_t size) {
    if (!t->nn) return NULL;
    for (uint32_t i = ic_key_hash(kr, addr, size) & (t->capidx - 1);; i = (i + 1) & (t->capidx - 1)) {
        uint32_t v = t->idx[i]; if (!v) return NULL;
        ic_ent *n = &t->n[v - 1];
        if (n->addr == addr && n->key_size == size && (unsigned)(n->kind | n->rw << 7) == kr) return n;
    }
}
static ic_ent *ic_tab_add(ic_tab *t, unsigned kr, uint64_t addr, uint64_t size) {   /* NULL: no memory, the note is dropped */
    if (t->nn == t->capn) {
        uint32_t cap = t->capn ? t->capn * 2 : 256; ic_ent *p = realloc(t->n, (size_t)cap * sizeof *p);
        if (!p) return NULL;
        t->n = p; t->capn = cap;
    }
    if ((t->nn + 1) * 2 > t->capidx) {
        uint32_t cap = t->capidx ? t->capidx * 2 : 1024; uint32_t *p = calloc(cap, sizeof *p);
        if (!p) return NULL;
        for (uint32_t k = 0; k < t->nn; k++) {
            const ic_ent *o = &t->n[k]; uint32_t i = ic_key_hash((unsigned)(o->kind | o->rw << 7), o->addr, o->key_size) & (cap - 1);
            while (p[i]) i = (i + 1) & (cap - 1);
            p[i] = k + 1;
        }
        free(t->idx); t->idx = p; t->capidx = cap;
    }
    ic_ent *n = &t->n[t->nn]; memset(n, 0, sizeof *n);
    n->addr = addr; n->key_size = size; n->kind = (uint8_t)(kr & 0x7f); n->rw = (uint8_t)(kr >> 7); n->uses = 1; n->flags = -1;
    n->row = (uint8_t)(n->rw ? IC_RW : n->kind);
    uint32_t i = ic_key_hash(kr, addr, size) & (t->capidx - 1);
    while (t->idx[i]) i = (i + 1) & (t->capidx - 1);
    t->idx[i] = ++t->nn;
    return n;
}
static void ic_tab_reset(ic_tab *t) {   /* its arrays are kept for the slot's next use */
    if (t->nn && t->idx) memset(t->idx, 0, (size_t)t->capidx * sizeof *t->idx);
    t->nn = 0; t->nph = 0; t->done = t->listed = t->census = 0; t->gen++;
    atomic_store_explicit(&t->h2, 0, memory_order_relaxed);
}
static int ic_ph_room(ic_tab *t, uint32_t np) {
    if (t->nph + np <= t->capph) return 1;
    uint32_t cap = t->capph ? t->capph : 4096; while (cap < t->nph + np) cap *= 2;
    uint32_t *p = realloc(t->ph, (size_t)cap * sizeof *p);
    if (!p) return 0;
    t->ph = p; t->capph = cap; return 1;
}
static inline void ic_page(const ic_ent *n, uint32_t k, uint64_t *lo, uint64_t *len) {   /* the entry's k-th hashed page, clipped to the range */
    uint64_t a = ((n->addr >> 12) + n->phase + (uint64_t)k * n->stride) << 12, b = a + IC_PAGE;
    if (a < n->addr) a = n->addr;
    if (b > n->addr + n->size) b = n->addr + n->size;
    *lo = a; *len = b - a;
}
static void ic_sub_list(ic_tab *t) {   /* H1 visits it at the commit */
    t->listed = 1;
    if (g_ic_nsub < IC_SUB) { g_ic_sub[g_ic_nsub].slot = (uint32_t)(t - g_ic_tab); g_ic_sub[g_ic_nsub].gen = t->gen; g_ic_nsub++; }
    else g_ic_w.dropped++;
}
/* A new entry of ic_note: finds its pool, and unless the game cannot write there, the bytes are a backend copy or the
   frame's budget is spent, takes H0. */
static void ic_note_new(ic_tab *t, unsigned kind, unsigned rw, unsigned unb_site, uint64_t addr, uint64_t asked, VkBuffer bound, const ic_span *span) {
    unsigned kr = kind | rw << 7;
    int first = !t->nn;
    ic_ent *n = ic_tab_add(t, kr, addr, asked);
    if (!n) { g_ic_w.dropped++; return; }
    if (first) { t->frame = g_ic_frame; t->stamp_prev = g_ic_stamp_prev; if (t != &g_ic_pend) { g_ic_w.slots++; g_ic_fslots++; } }
    if (g_in_sub && !t->listed && t != &g_ic_pend) ic_sub_list(t);
    ic_span sp; uint64_t m0;
    if (span) { sp = *span; m0 = span->map_gen; }
    else {
        m0 = atomic_load_explicit(&g_ic_map_gen, memory_order_acquire);   /* before the lookup: a lazy move after it is seen at the read */
        if (!ic_span_of(addr, &sp)) { n->skip = IC_SKIP_UNBACKED; g_ic_w.unbacked++; return; }   /* the GPU reads no pool memory here */
    }
    if (bound && bound != sp.buf) { n->skip = IC_SKIP_COPY; g_ic_w.copies++; return; }   /* bound from a copy of the backend's */
    uint64_t rest = sp.end - addr, size = asked && asked < rest ? asked : rest;
    int census = kind == IC_GMEM;
    int unb = !census && (unb_site || !asked || ((kind == VK_INFLIGHT_STORAGE || kind == VK_INFLIGHT_STORAGE_CS) && asked >= rest));
#ifdef TOTK_OHOS
    if (unb && gbuf_on()) { uint64_t r = ic_gbuf_rest(addr); if (r) { if (r < size) size = r; unb = 0; g_ic_w.gbuf_capped++; } }   /* the game buffer it lies in bounds it */
#endif
    n->size = size; n->unb = (uint8_t)unb; if (unb && !rw) n->row = IC_UNB;
    n->flags = sp.flags; n->span_gen = sp.gen; n->map_gen = m0; n->identity = sp.identity; n->pool_lo = sp.pool_lo; n->pool_size = sp.pool_size;
    if (sp.flags >= 0 && (sp.flags & 0x1)) { n->skip = IC_SKIP_NOACCESS; g_ic_w.no_access++; g_ic_w.no_access_bytes += size; return; }   /* CPU_NO_ACCESS */
    if (rw) ic_gpu_wrote(addr, size, unb ? VK_INFLIGHT_GW_UNBOUNDED : VK_INFLIGHT_GW_PLAIN);
    uint64_t pages = ((addr + size - 1) >> 12) - (addr >> 12) + 1;
    int over = 0;
    if (!census) {   /* the frame's H0 budget, handed out by slot: later slots keep a share */
        uint64_t B = (uint64_t)g_ic_budget_mb << 20, est = g_ic_fslots_prev ? g_ic_fslots_prev : 24, i = g_ic_fslots ? g_ic_fslots : 1;
        uint64_t allowed = i >= est ? B : B / est * i;
        if (g_ic_frame_h0 >= allowed) { n->skip = IC_SKIP_BUDGET; g_ic_w.budget++; g_ic_w.budget_bytes += size; return; }
        over = g_ic_frame_h0 >= allowed / 4 * 3;
    }
    uint64_t maxp = over ? IC_SPARSE_PAGES : IC_FULL_PAGES;
    n->stride = pages > maxp ? (uint32_t)((pages + maxp - 1) / maxp) : 1;
    n->phase = n->stride > 1 ? (uint32_t)(g_ic_phase++ % n->stride) : 0;
    n->np = (uint32_t)((pages - n->phase + n->stride - 1) / n->stride);
    if (!ic_ph_room(t, n->np)) { n->skip = IC_SKIP_MEMORY; g_ic_w.dropped++; return; }
    n->ph = t->nph; n->stamp0 = atomic_load_explicit(&g_ic_st_gen, memory_order_acquire);
    uint64_t hashed = 0; int live = !(m0 & 1);   /* odd: a lazy backing is moving pages under a registered pool */
    for (uint32_t k = 0; k < n->np && live; ) {   /* pieces of at most 64 KiB, each under the read lock with the generations checked */
        ic_read_begin();
        live = atomic_load_explicit(&g_span_gen, memory_order_acquire) == sp.gen && atomic_load_explicit(&g_ic_map_gen, memory_order_acquire) == m0;
        for (uint64_t piece = 0; live && k < n->np && piece < IC_PIECE; k++) {
            uint64_t lo, len; ic_page(n, k, &lo, &len);
            t->ph[n->ph + k] = ic_hash((const uint8_t *)(uintptr_t)lo, (size_t)len); hashed += len; piece += len;
        }
        ic_read_end();
    }
    if (!live) { n->skip = IC_SKIP_RACED; g_ic_w.raced++; return; }   /* the pools changed between the lookup and the read */
    t->nph += n->np;
    if (census) g_ic_frame_census += hashed; else g_ic_frame_h0 += hashed;
    g_ic_w.hashed0 += hashed; g_ic_w.notes++;
    g_ic_w.paged += n->stride > 1 && !over; g_ic_w.sparse += n->stride > 1 && over;
    ic_wrow *r = &g_ic_w.k[n->row];
    r->ranges++; r->bytes += size; r->uses++; r->pages += pages; r->hpages += n->np;
}
/* One read of [addr, +asked) by the slot whose table t is: a known entry counts a use; a new one takes H0, and its time,
   pool lookup included, goes to the H0 time. span: the census knows it already. */
static void ic_note(ic_tab *t, unsigned kind, unsigned rw, unsigned unb_site, uint64_t addr, uint64_t asked, VkBuffer bound, const ic_span *span) {
    ic_ent *n = ic_tab_find(t, kind | rw << 7, addr, asked);
    if (n) { n->uses++; if (!n->skip) g_ic_w.k[n->row].uses++; return; }
    uint64_t t0 = host_now_ns();
    ic_note_new(t, kind, rw, unb_site, addr, asked, bound, span);
    g_ic_w.ns0 += host_now_ns() - t0;
}
void vk_inflight_note(int kind, uint64_t addr, uint64_t size, VkBuffer bound) {
    unsigned k = (unsigned)kind & 0x7f, rw = (kind & VK_INFLIGHT_GPUW) != 0, unb = (kind & VK_INFLIGHT_UNBOUNDED) != 0;
    if (!g_ic_k || !addr || k >= VK_INFLIGHT_KINDS) return;
    if (!T_rec_depth) { atomic_fetch_add_explicit(&g_ic_foreign, 1, memory_order_relaxed); return; }   /* the tables are the recording-lock owner's */
    ic_note(g_cur ? &g_ic_tab[g_cur - g_ring] : &g_ic_pend, k, rw, unb, addr, size, bound, NULL);
}
/* Memory reached through the pool table (RB_GMEM) is bound by nobody, so no site can note it. A slot that binds the table
   (vk_gmem_map) notes 64 KiB windows of all CPU-writable GPU-backed pool memory instead: TOTK_OHOS_INFLIGHT_CENSUS_KB
   of them, at golden-ratio steps through that memory, so every part of it comes up. What changes there under the slot is
   an upper bound on what its shaders could have read changed: nothing says which windows they read, so it is not zero
   with one frame in flight either (the game writes memory nobody reads at the time): compare it between runs. */
static int ic_census_pool(const pool_ent *p) {
    if (p->kind != K_OWN || p->hi <= p->lo) return 0;
#ifdef TOTK_OHOS
    if (p->flags >= 0 && (p->flags & 0x1)) return 0;   /* CPU_NO_ACCESS: the game cannot write it */
#endif
    return 1;
}
static void ic_census(void) {
    if (!g_ic_census_kb || !T_rec_depth) return;
    ic_tab *t = g_cur ? &g_ic_tab[g_cur - g_ring] : &g_ic_pend;
    if (t->census) return;
    t->census = 1;
    if (g_ic_frame_census >= (uint64_t)g_ic_census_kb << 14) { g_ic_w.census_capped++; return; }   /* its own cap: 16 slots' worth a frame */
    enum { MAXW = 64 }; static uint64_t counter;
    uint64_t pos[MAXW], at[MAXW], len[MAXW], pop = 0; ic_span win[MAXW];
    int want = (int)(((uint64_t)g_ic_census_kb << 10) / IC_WIN), nw = 0;
    if (want < 1) want = 1;
    if (want > MAXW) want = MAXW;
    uint64_t m0 = atomic_load_explicit(&g_ic_map_gen, memory_order_acquire), c0 = host_now_ns();
    pthread_mutex_lock(&g_pool_mu);
    uint64_t gen = atomic_load_explicit(&g_span_gen, memory_order_relaxed);
    for (int i = 0; i < g_npool; i++) if (ic_census_pool(g_pools[i])) pop += g_pools[i]->hi - g_pools[i]->lo;
    if (pop >= IC_WIN) {
        for (int k = 0; k < want; k++) {   /* window starts in the pools laid end to end, kept sorted */
            uint64_t r = (++counter * 0x9E3779B97F4A7C15ull) >> 11, p = (uint64_t)(((__uint128_t)r * pop) >> 53) & ~(uint64_t)(IC_WIN - 1);
            int j = k; while (j > 0 && pos[j - 1] > p) { pos[j] = pos[j - 1]; j--; }
            pos[j] = p;
        }
        uint64_t base = 0;
        for (int i = 0, k = 0; i < g_npool && k < want; i++) {
            const pool_ent *p = g_pools[i]; if (!ic_census_pool(p)) continue;
            uint64_t n = p->hi - p->lo;
            for (; k < want && pos[k] < base + n; k++, nw++) {
                at[nw] = p->lo + (pos[k] - base); len[nw] = p->hi - at[nw] < IC_WIN ? p->hi - at[nw] : IC_WIN;
                memset(&win[nw], 0, sizeof win[nw]);
                win[nw].lo = p->lo; win[nw].end = p->hi; win[nw].gen = gen; win[nw].map_gen = m0; win[nw].identity = p->identity; win[nw].buf = p->buf; win[nw].flags = -1;
                ic_span_pool(&win[nw], p);
            }
            base += n;
        }
    }
    pthread_mutex_unlock(&g_pool_mu);
    uint64_t dt = host_now_ns() - c0; g_ic_w.census_ns += dt; g_ic_w.ns0 += dt;   /* two passes over the registry under the pool lock */
    g_ic_w.census_pop += pop; g_ic_w.census_n++;
    for (int k = 0; k < nw; k++) ic_note(t, IC_GMEM, 0, 0, at[k], len[k], VK_NULL_HANDLE, &win[k]);
}
/* vk_gmem_log, every frame (recording-lock owner): the slot being recorded stores through the pool table. */
static void ic_gml_set(ic_tab *t) {
    if (t->gml) return;
    t->gml = t->exposed = 1; g_ic_w.gml_slots++;
    atomic_store_explicit(&g_ic_gml_last, t->seq, memory_order_release);
}
static void ic_gml_mark(void) {
    if (!T_rec_depth) return;
    if (!g_cur) { g_ic_pend_gml = 1; return; }   /* the slot that opens next */
    ic_gml_set(&g_ic_tab[g_cur - g_ring]);
}
static void ic_adopt(ic_tab *t) {   /* notes made while no slot was open belong to the slot that opens next */
    ic_tab *p = &g_ic_pend;
    for (uint32_t i = 0; i < p->nn; i++) {
        const ic_ent *o = &p->n[i]; ic_ent *n = ic_tab_add(t, (unsigned)(o->kind | o->rw << 7), o->addr, o->key_size);
        if (!n) { g_ic_w.dropped++; continue; }
        *n = *o;
        if (o->skip || !o->np) continue;
        if (!ic_ph_room(t, o->np)) { n->skip = IC_SKIP_MEMORY; g_ic_w.dropped++; continue; }
        memcpy(t->ph + t->nph, p->ph + o->ph, (size_t)o->np * sizeof *t->ph); n->ph = t->nph; t->nph += o->np;
    }
    t->frame = p->frame; t->stamp_prev = p->stamp_prev; t->census = p->census; g_ic_w.slots++; g_ic_fslots++;
    if (g_in_sub && t->nn) ic_sub_list(t);
    ic_tab_reset(p);
}
static void ic_slot_open(unsigned slot) {   /* slot_open, every frame: the slot's last use is complete, so nobody reads its table any more */
    ic_tab *t = &g_ic_tab[slot];
    ic_tab_reset(t); t->frame = g_ic_frame; t->seq = ++g_ic_seq; t->gml = 0;
    /* slots complete in queue order: the last one that binds the write log not complete yet means one still in flight */
    t->exposed = atomic_load_explicit(&g_ic_gml_last, memory_order_relaxed) > atomic_load_explicit(&g_ic_done_seq, memory_order_acquire);
    if (g_ic_pend_gml) { g_ic_pend_gml = 0; ic_gml_set(t); }
    if (g_ic_pend.nn) ic_adopt(t);
}
/* One H1 or H2 pass's view of the registry: a single try of the pool lock copies the K_OWN registrations into a hash set,
   at the generation they hold for (taken again once when the generation moves). When the try fails, each entry tries on
   its own as before (cache). */
static void ic_ids_begin(ic_ids *s) { s->state = 0; s->gen = 0; memset(s->cache, 0, sizeof s->cache); }
static int ic_ids_take(ic_ids *s) {
    if (pthread_mutex_trylock(&g_pool_mu)) return 0;
    uint32_t cap = 64; while (cap < (uint32_t)g_npool * 2u) cap *= 2;
    if (!s->set || cap - 1 > s->mask) {
        uint64_t *p = realloc(s->set, (size_t)cap * sizeof *p);
        if (!p) { pthread_mutex_unlock(&g_pool_mu); return 0; }
        s->set = p; s->mask = cap - 1;
    }
    memset(s->set, 0, ((size_t)s->mask + 1) * sizeof *s->set);
    for (int i = 0; i < g_npool; i++) {
        if (g_pools[i]->kind != K_OWN) continue;
        uint64_t id = g_pools[i]->identity;
        for (uint32_t h = (uint32_t)((id * 0x9E3779B97F4A7C15ull) >> 32) & s->mask;; h = (h + 1) & s->mask) {
            if (s->set[h] == id) break;
            if (!s->set[h]) { s->set[h] = id; break; }
        }
    }
    s->gen = atomic_load_explicit(&g_span_gen, memory_order_relaxed);
    pthread_mutex_unlock(&g_pool_mu);
    return 1;
}
static int ic_ids_has(const ic_ids *s, uint64_t id) {
    for (uint32_t h = (uint32_t)((id * 0x9E3779B97F4A7C15ull) >> 32) & s->mask;; h = (h + 1) & s->mask) {
        if (s->set[h] == id) return 1;
        if (!s->set[h]) return 0;
    }
}
/* The pool generation at which the entry's range is known to be the mapping it was at H0, 0 when that cannot be said:
   its pool is gone, it was reached through an alias and the pools have changed, or the pool lock is busy (it is only
   tried: del_range_ent may hold it while waiting for the very completion this runs in). */
static uint64_t ic_ent_live(const ic_ent *n, ic_ids *s) {
    uint64_t gen = atomic_load_explicit(&g_span_gen, memory_order_acquire);
    if (gen == n->span_gen) return gen;
    if (!n->identity) return 0;
    if (s->state == 1 && s->gen != gen) s->state = 0;   /* the pools moved since the copy: one more try */
    if (s->state == 0) s->state = ic_ids_take(s) ? 1 : 2;
    if (s->state == 1) return ic_ids_has(s, n->identity) ? s->gen : 0;
    ic_live *c = &s->cache[n->identity & 15];
    if (c->identity == n->identity && c->gen == gen) return c->ok ? gen : 0;
    if (pthread_mutex_trylock(&g_pool_mu)) return 0;
    int ok = 0;
    for (int i = 0; i < g_npool && !ok; i++) ok = g_pools[i]->identity == n->identity && g_pools[i]->kind == K_OWN;
    gen = atomic_load_explicit(&g_span_gen, memory_order_relaxed);
    pthread_mutex_unlock(&g_pool_mu);
    c->identity = n->identity; c->gen = gen; c->ok = ok;
    return ok ? gen : 0;
}
/* Hash the entry's pages again, in pieces of at most 64 KiB with the generations checked under each: the bytes of those
   that differ from H0, -1 when the range cannot be read now. changed (IC_FULL_PAGES long, or NULL) gets which of its
   hashed pages differ, *nchanged how many. */
static int64_t ic_rehash(const ic_tab *t, const ic_ent *n, ic_ids *ids, uint64_t *hashed, uint32_t *changed, uint32_t *nchanged) {
    uint64_t gen = ic_ent_live(n, ids); int64_t diff = 0; uint32_t nc = 0;
    if (!gen) return -1;
    for (uint32_t k = 0; k < n->np; ) {
        ic_read_begin();
        if (atomic_load_explicit(&g_span_gen, memory_order_acquire) != gen || atomic_load_explicit(&g_ic_map_gen, memory_order_acquire) != n->map_gen) { ic_read_end(); return -1; }
        for (uint64_t piece = 0; k < n->np && piece < IC_PIECE; k++) {
            uint64_t lo, len; ic_page(n, k, &lo, &len); *hashed += len; piece += len;
            if (ic_hash((const uint8_t *)(uintptr_t)lo, (size_t)len) == t->ph[n->ph + k]) continue;
            diff += (int64_t)len;
            if (changed && nc < IC_FULL_PAGES) changed[nc] = k;
            nc++;
        }
        ic_read_end();
    }
    if (nchanged) *nchanged = nc < IC_FULL_PAGES ? nc : IC_FULL_PAGES;
    return diff;
}
/* The changed pages of an entry, each decided on its own: wv[j] the bytes it stands for (len * stride, the entry's sum
   capped at its size), ch[j] its IC_CH_* class against the GPU writes over the range; sp[] the bytes by class. */
static void ic_split(const ic_ent *n, const uint32_t *changed, uint32_t nc, uint64_t frame, ic_gwr *gw, uint64_t *wv, uint8_t *ch, uint64_t sp[4]) {
    int own = n->rw && !n->unb;   /* a sized writable binding's range is the GPU's; a size-less one is decided by the records (its own is VK_INFLIGHT_GW_UNBOUNDED) */
    int ng = own ? 0 : ic_gw_collect(n->addr, n->addr + n->size, frame, gw);
    uint64_t room = n->size;
    for (uint32_t j = 0; j < nc; j++) {
        uint64_t lo, len; ic_page(n, changed[j], &lo, &len);
        uint64_t w = len * n->stride; if (w > room) w = room;
        room -= w;
        unsigned c = own ? IC_CH_GPU : ic_gw_class(gw, ng, lo, lo + len);
        wv[j] = w; ch[j] = (uint8_t)c; sp[c] += w;
    }
}
static void ic_sub_commit(void) {   /* H1, mtl_sub_commit after the submission's last slot is queued: the walker */
    if (!g_ic_nsub) return;
    if (!g_ic_h1) { g_ic_nsub = 0; return; }
    static ic_ids ids; static ic_gwr gw[IC_GW];
    uint64_t t0 = host_now_ns(), hashed = 0; uint32_t changed[IC_FULL_PAGES]; uint64_t wv[IC_FULL_PAGES]; uint8_t ch[IC_FULL_PAGES];
    ic_ids_begin(&ids);
    ic_tab *last = &g_ic_tab[g_ic_sub[g_ic_nsub - 1].slot]; uint64_t last_gen = g_ic_sub[g_ic_nsub - 1].gen; int late = 0;
    for (int s = 0; s < g_ic_nsub; s++) {
        const ic_tab *t = &g_ic_tab[g_ic_sub[s].slot];
        if (t->gen != g_ic_sub[s].gen) { g_ic_w.h1_reused++; continue; }   /* completed and opened again inside this submission */
        for (uint32_t i = 0; i < t->nn; i++) {
            const ic_ent *n = &t->n[i]; if (n->skip) continue;
            if (late) { g_ic_w.h1_late++; continue; }
            uint32_t nc = 0;
            int64_t d = ic_rehash(t, n, &ids, &hashed, changed, &nc);
            /* the submission's last noted slot has begun H2: its posts may have released the game since, whose writes are
               then legitimate. Read after the hash: a write the hash saw that came after the posts shows the flag set. */
            atomic_thread_fence(memory_order_seq_cst);
            if (last->gen != last_gen || atomic_load_explicit(&last->h2, memory_order_seq_cst)) { late = 1; g_ic_w.h1_late++; continue; }
            if (d < 0) { g_ic_w.h1_skipped++; continue; }
            if (!d) continue;
            uint64_t sp[4] = {0};
            ic_split(n, changed, nc, t->frame, gw, wv, ch, sp);
            ic_wrow *r = &g_ic_w.k[n->row]; uint64_t keep = sp[IC_CH_GAME] + sp[IC_CH_AMB] + sp[IC_CH_UB];
            if (sp[IC_CH_GPU]) { r->h1gn++; r->h1gb += sp[IC_CH_GPU]; }
            if (keep) {
                r->h1n++; r->h1b += keep; r->h1ab += sp[IC_CH_AMB]; r->h1ub += sp[IC_CH_UB];
                /* every slot after it recorded by now is queued and may be running: one that binds the write log exposes it too */
                if (t->exposed || atomic_load_explicit(&g_ic_gml_last, memory_order_relaxed) > t->seq) { r->h1xn++; r->h1xb += keep; }
            }
        }
    }
    g_ic_nsub = 0; g_ic_w.hashed1 += hashed; g_ic_w.ns1 += host_now_ns() - t0;
}
/* The completion thread's frame: distinct changed pages (ranges hashed whole) and page-sampled ranges that changed. */
static void ic_set_add(uint64_t page) {
    if (!g_ic_set && !(g_ic_set = calloc(IC_SET, sizeof *g_ic_set))) { g_ic_set_over++; return; }
    for (uint32_t i = (uint32_t)((page * 0x9E3779B97F4A7C15ull) >> 48) & (IC_SET - 1);; i = (i + 1) & (IC_SET - 1)) {
        if (g_ic_set[i] == page + 1) return;
        if (g_ic_set[i]) continue;
        if (g_ic_set_n >= IC_SET / 4 * 3) { g_ic_set_over++; return; }   /* full: counted, not told from a page already in */
        g_ic_set[i] = page + 1; g_ic_set_n++; return;
    }
}
static void ic_big_add(uint64_t addr, uint64_t size, uint64_t est) {
    for (int i = 0; i < g_ic_nbig; i++) if (g_ic_big[i].addr == addr && g_ic_big[i].size == size) { if (est > g_ic_big[i].est) g_ic_big[i].est = est; return; }
    if (g_ic_nbig < IC_BIG) { g_ic_big[g_ic_nbig].addr = addr; g_ic_big[g_ic_nbig].size = size; g_ic_big[g_ic_nbig].est = est; g_ic_nbig++; }
    else g_ic_big_over += est;
}
/* The ranges that changed, over the report window: found again in every slot that reads them, so their uses count too.
   game: 2 some of the change had no GPU write over it, 1 some had only an inline uniform update or a size-less GPU write
   over it, 0 a GPU write of known size was over all of it; the entry keeps the largest. */
static void ic_off_note(const ic_ent *n, uint64_t frame, uint64_t bytes, int game) {
    uint32_t h = ic_key_hash((unsigned)(n->kind | n->rw << 7), n->addr, n->size); ic_off *o = NULL; int full = 1;
    pthread_mutex_lock(&g_ic_mu);
    for (unsigned p = 0; p < 16; p++) {
        ic_off *e = &g_ic_off[(h + p) & (IC_OFF - 1)];
        if (e->used && e->addr == n->addr && e->size == n->size && e->kind == n->kind && e->rw == n->rw && e->unb == n->unb) { o = e; full = 0; break; }
        if (e->used) continue;
        full = 0;
        if (bytes) { o = e; memset(o, 0, sizeof *o); o->addr = n->addr; o->size = n->size; o->pool_lo = n->pool_lo; o->pool_size = n->pool_size;
                     o->flags = n->flags; o->kind = n->kind; o->rw = n->rw; o->unb = n->unb; o->used = 1; }
        break;
    }
    if (o) {
        o->uses += n->uses;
        if (o->seen_frame != frame) { o->seen_frame = frame; o->seen++; }
        if (bytes) { o->bytes += bytes; if (game > o->game) o->game = (uint8_t)game; if (o->chg_frame != frame) { o->chg_frame = frame; o->chg++; } }
    } else if (full && bytes) g_ic_off_lost++;
    pthread_mutex_unlock(&g_ic_mu);
}
/* A changed page's flush class from the window (ic_sw_load): flushed before H0, between H0 and H2, both, or not; when
   the ring had lost the window's start after H0, not knowing counts apart. A page not flushed by H2 is looked at again
   when the frame closes (IC_ST_LATE). */
static unsigned ic_st_class(const ic_ent *n, uint64_t lo, uint64_t len) {
    int b, a; ic_sw_query((uintptr_t)lo, (uintptr_t)(lo + len), n->stamp0, &b, &a);
    if (a) return b ? IC_ST_BOTH : IC_ST_INFLIGHT;
    if (b) return IC_ST_BEFORE;
    return g_ic_swi.lost && g_ic_swi.start > n->stamp0 ? IC_ST_LOST : IC_ST_NEVER;
}
/* The changed pages of a range that are not the GPU's: by origin (the game's; the game's in a slot exposed to pool-table
   stores, x; under only an inline uniform update or a size-less GPU write) and pool flag jointly with flush class, and
   into the frame's set (bound ranges of known size only, the last origin apart). */
static void ic_classify(const ic_ent *n, const uint32_t *changed, const uint64_t *wv, const uint8_t *ch, uint32_t nc, int x, ic_cstat *s) {
    unsigned c3 = n->kind == IC_GMEM ? 1 : n->unb ? 2 : 0, pc = n->flags < 0 ? 3 : (n->flags & 0x2) ? 0 : (n->flags & 0x4) ? 1 : 2;
    uint64_t est = 0;
    for (uint32_t j = 0; j < nc; j++) {
        if (ch[j] == IC_CH_GPU || !wv[j]) continue;
        uint64_t lo, len; ic_page(n, changed[j], &lo, &len);
        unsigned st = ic_st_class(n, lo, len), o = ch[j] != IC_CH_GAME ? 2 : x ? 1 : 0;
        s->pf[c3][o][pc][st] += wv[j]; if (o != 2) est += wv[j];
        if (st == IC_ST_BEFORE || st == IC_ST_NEVER) {
            if (g_ic_nlate < IC_LATE) { g_ic_late[g_ic_nlate].lo = lo; g_ic_late[g_ic_nlate].hi = lo + len; g_ic_late[g_ic_nlate].w = wv[j]; g_ic_late[g_ic_nlate].gen = g_ic_swi.end;
                                        g_ic_late[g_ic_nlate].c3 = (uint8_t)c3; g_ic_late[g_ic_nlate].o = (uint8_t)o; g_ic_late[g_ic_nlate].pc = (uint8_t)pc; g_ic_late[g_ic_nlate].st = (uint8_t)st; g_ic_nlate++; }
            else s->late_over++;
        }
        if (!c3 && n->stride == 1 && o != 2) ic_set_add(lo >> 12);
    }
    if (!c3 && n->stride > 1 && est) ic_big_add(n->addr, n->size, est);
}
static void ic_cframe_close(void) {   /* completion thread: every slot of the frame has been through H2 */
    ic_cstat *cs = &g_ic_cf.s;
    if (g_ic_nlate && g_ic_swi.valid) {   /* pages not flushed by H2: flushed since? */
        ic_sw_load(g_ic_swi.since);
        for (uint32_t i = 0; i < g_ic_nlate; i++) {
            int b, a; ic_sw_query((uintptr_t)g_ic_late[i].lo, (uintptr_t)g_ic_late[i].hi, g_ic_late[i].gen, &b, &a);
            if (!a) continue;
            cs->pf[g_ic_late[i].c3][g_ic_late[i].o][g_ic_late[i].pc][g_ic_late[i].st] -= g_ic_late[i].w; cs->pf[g_ic_late[i].c3][g_ic_late[i].o][g_ic_late[i].pc][IC_ST_LATE] += g_ic_late[i].w;
        }
    }
    g_ic_nlate = 0;
    uint64_t est = g_ic_big_over; for (int i = 0; i < g_ic_nbig; i++) est += g_ic_big[i].est;
    uint64_t distinct = ((uint64_t)g_ic_set_n + g_ic_set_over) * IC_PAGE + est;
    pthread_mutex_lock(&g_ic_mu);
    ic_add64(&g_ic_cw.s, cs, sizeof g_ic_cw.s);
    g_ic_cw.frames++; g_ic_cw.distinct += distinct; g_ic_cw.est += est; g_ic_cw.set_over += g_ic_set_over;
    if (distinct > g_ic_cw.distinct_max) g_ic_cw.distinct_max = distinct;
    pthread_mutex_unlock(&g_ic_mu);
    if (g_ic_set_n) memset(g_ic_set, 0, (size_t)IC_SET * sizeof *g_ic_set);
    g_ic_set_n = g_ic_set_over = 0; g_ic_nbig = 0; g_ic_big_over = 0;
    memset(&g_ic_cf, 0, sizeof g_ic_cf);
}
static void ic_slot_complete(unsigned slot) {   /* H2: completion thread, every slot, its GPU work done, its completion not yet published */
    ic_tab *t = &g_ic_tab[slot];
    if (t->seq > atomic_load_explicit(&g_ic_done_seq, memory_order_relaxed))   /* slots complete in queue order; only this thread stores it */
        atomic_store_explicit(&g_ic_done_seq, t->seq, memory_order_release);
    if (g_ic_cf.open && t->frame != g_ic_cf.frame) ic_cframe_close();
    if (!t->nn || t->done) return;
    t->done = 1;
    atomic_store_explicit(&t->h2, 1, memory_order_seq_cst);   /* before any hash and before the posts: H1 stops at it */
    if (!g_ic_cf.open) { g_ic_cf.open = 1; g_ic_cf.frame = t->frame; }
    ic_cstat *s = &g_ic_cf.s; static ic_ids ids; static ic_gwr gw[IC_GW];
    uint64_t t0 = host_now_ns(), hashed = 0; uint32_t changed[IC_FULL_PAGES]; uint64_t wv[IC_FULL_PAGES]; uint8_t ch[IC_FULL_PAGES]; int sw = 0;
    ic_ids_begin(&ids);
    s->slots++; int x = t->exposed;   /* a local: the table stays read-only after the seal */
    for (uint32_t i = 0; i < t->nn; i++) {
        const ic_ent *n = &t->n[i]; if (n->skip) continue;
        uint32_t nc = 0;
        int64_t d = ic_rehash(t, n, &ids, &hashed, changed, &nc);
        if (d < 0) { s->skipped++; s->skipped_bytes += n->size; continue; }
        ic_crow *r = &s->k[n->row]; r->n++; r->b += n->size;
        uint64_t sp[4] = {0}, keep = 0;
        if (d) {
            ic_split(n, changed, nc, t->frame, gw, wv, ch, sp);
            keep = sp[IC_CH_GAME] + sp[IC_CH_AMB] + sp[IC_CH_UB];
            if (sp[IC_CH_GPU]) { r->gn++; r->gb += sp[IC_CH_GPU]; }
            if (keep) {
                r->cn++; r->cb += keep;
                if (sp[IC_CH_AMB]) { r->an++; r->ab += sp[IC_CH_AMB]; }
                if (sp[IC_CH_UB]) { r->un++; r->ub += sp[IC_CH_UB]; }
                /* after this entry's rehash: a store the hash saw implies its slot's g_ic_gml_last store is visible */
                if (!x) x = atomic_load_explicit(&g_ic_gml_last, memory_order_acquire) > t->seq;   /* a later slot that binds the log has been recorded and may run during this H2 */
                if (x) { r->xn++; r->xb += keep; }
                if (!sw) { ic_sw_load(t->stamp_prev); sw = 1; s->stamp_lost += g_ic_swi.lost != 0; }   /* the flush ring, once a slot */
                ic_classify(n, changed, wv, ch, nc, x, s);
            }
        }
        if (n->kind != IC_GMEM) ic_off_note(n, t->frame, keep ? keep : sp[IC_CH_GPU], sp[IC_CH_GAME] ? 2 : keep ? 1 : 0);
    }
    if (!x) x = atomic_load_explicit(&g_ic_gml_last, memory_order_acquire) > t->seq;
    s->slots_exposed += x != 0;
    s->hashed += hashed; s->ns += host_now_ns() - t0;
}
static const char *ic_flag_name(int flags) {
    return flags < 0 ? " (unknown)" : (flags & 0x2) ? " (CPU_UNCACHED)" : (flags & 0x4) ? " (CPU_CACHED)" : (flags & 0x1) ? " (CPU_NO_ACCESS)" : "";
}
/* One line of the joint table: MiB a frame by pool class and flush class, and what the game had not flushed by H2, for
   the game's pages (o = 0); then the same not-flushed split for the pages of slots exposed to pool-table stores (o = 1)
   and for those under only an inline uniform update or a size-less GPU write (o = 2), which may be the GPU's own. */
static void ic_report_pf(const char *what, uint64_t pf[3][4][IC_ST_N], double cf) {
    static const char *const pcn[4] = {"CPU_UNCACHED", "CPU_CACHED", "neither", "flags unknown"};
    static const char *const on[3] = {NULL, "in slots exposed to pool-table stores", "under only an inline uniform update or a size-less GPU write; may be the GPU's own writes"};
    const double M = 1048576.0; char b[640]; size_t o = 0; uint64_t tot = 0, all[3] = {0}, nf[3][4] = {{0}}, st[IC_ST_N] = {0};
    for (int x = 0; x < 3; x++) for (int p = 0; p < 4; p++) {
        nf[x][p] = pf[x][p][IC_ST_BEFORE] + pf[x][p][IC_ST_NEVER] + pf[x][p][IC_ST_LATE];
        for (int q = 0; q < IC_ST_N; q++) { all[x] += pf[x][p][q]; if (!x) st[q] += pf[x][p][q]; }
    }
    tot = all[0] + all[1] + all[2];
    if (!tot) return;
    for (int p = 0; p < 4; p++) {
        o += (size_t)snprintf(b + o, o < sizeof b ? sizeof b - o : 0, "%s%s %.3f/%.3f/%.3f/%.3f/%.3f/%.3f", p ? "; " : "", pcn[p],
                              pf[0][p][IC_ST_BEFORE] / M / cf, pf[0][p][IC_ST_INFLIGHT] / M / cf, pf[0][p][IC_ST_BOTH] / M / cf, pf[0][p][IC_ST_NEVER] / M / cf,
                              pf[0][p][IC_ST_LATE] / M / cf, pf[0][p][IC_ST_LOST] / M / cf);
        if (o >= sizeof b) break;
    }
    LOGV("inflight check: H2 changes not GPU-written, %s, the game's pages (slots not exposed to pool-table stores, no GPU write over them), MiB a frame by pool, each as flushed by the game before the read only / in flight / both / not at all / only after H2 / "
         "unknown (the flush ring lost the in-flight history): %s; totals %.3f/%.3f/%.3f/%.3f/%.3f/%.3f; rewrite not flushed by H2 (before only + not at all + only after H2) %.3f MiB, "
         "of it CPU_CACHED %.3f, CPU_UNCACHED %.3f, neither %.3f, unknown %.3f",
         what, b, st[IC_ST_BEFORE] / M / cf, st[IC_ST_INFLIGHT] / M / cf, st[IC_ST_BOTH] / M / cf, st[IC_ST_NEVER] / M / cf, st[IC_ST_LATE] / M / cf, st[IC_ST_LOST] / M / cf,
         (nf[0][0] + nf[0][1] + nf[0][2] + nf[0][3]) / M / cf, nf[0][1] / M / cf, nf[0][0] / M / cf, nf[0][2] / M / cf, nf[0][3] / M / cf);
    for (int x = 1; x < 3; x++) {
        if (!all[x]) continue;
        LOGV("inflight check: H2 changes not GPU-written, %s, pages %s: %.3f MiB a frame; rewrite not flushed by H2 %.3f MiB, of it CPU_CACHED %.3f, CPU_UNCACHED %.3f, neither %.3f, unknown %.3f",
             what, on[x], all[x] / M / cf, (nf[x][0] + nf[x][1] + nf[x][2] + nf[x][3]) / M / cf, nf[x][1] / M / cf, nf[x][0] / M / cf, nf[x][2] / M / cf, nf[x][3] / M / cf);
    }
}
static void ic_report(uint64_t frame) {   /* recording-lock owner, every 300 presents */
    __typeof__(g_ic_cw) c; ic_off top[8]; int ntop = 0; uint64_t off_lost;
    pthread_mutex_lock(&g_ic_mu);
    c = g_ic_cw; memset(&g_ic_cw, 0, sizeof g_ic_cw);
    for (int i = 0; i < IC_OFF; i++) {   /* the eight worst: ranges with changes no GPU write explains first, then known-size ranges before reads of unknown size, then by changed bytes */
        const ic_off *o = &g_ic_off[i]; if (!o->used || !o->chg) continue;
        int at = ntop;
        while (at > 0 && (top[at - 1].game < o->game || (top[at - 1].game == o->game && (top[at - 1].unb > o->unb || (top[at - 1].unb == o->unb && top[at - 1].bytes < o->bytes))))) at--;
        if (at >= 8) continue;
        memmove(top + at + 1, top + at, (size_t)((ntop < 8 ? ntop : 7) - at) * sizeof *top);
        top[at] = *o; if (ntop < 8) ntop++;
    }
    off_lost = g_ic_off_lost; g_ic_off_lost = 0; memset(g_ic_off, 0, sizeof g_ic_off);
    pthread_mutex_unlock(&g_ic_mu);
    ic_wstat w = g_ic_w; memset(&g_ic_w, 0, sizeof g_ic_w);
    uint64_t foreign = atomic_exchange(&g_ic_foreign, 0), gw_lost = atomic_exchange(&g_ic_gw_lost, 0);
    uint64_t fn[2] = {atomic_exchange(&g_ic_st_n[0], 0), atomic_exchange(&g_ic_st_n[1], 0)};
    uint64_t fb[2] = {atomic_exchange(&g_ic_st_bytes[0], 0), atomic_exchange(&g_ic_st_bytes[1], 0)};
    if (!w.frames && !c.frames) return;   /* no sampled frame in this window */
    const double M = 1048576.0, wf = w.frames ? (double)w.frames : 1, cf = c.frames ? (double)c.frames : 1;
    LOGV("inflight check: frame %llu: %llu sampled frames (one in %d), a frame %.1f slots with %.0f ranges; hashed %.1f MiB at H0 in %.2f ms (pool lookups included, %.0f of them missed "
         "the lookup cache; %.2f ms of it the census holding the pool lock) and %.1f MiB at H1 in %.2f ms (walker%s), %.1f MiB at H2 in %.2f ms (completion thread; H2 done for %llu frames, %llu slots)",
         (unsigned long long)frame, (unsigned long long)w.frames, g_ic_k, w.slots / wf, w.notes / wf, w.hashed0 / M / wf, w.ns0 / 1e6 / wf, w.span_miss / wf, w.census_ns / 1e6 / wf,
         w.hashed1 / M / wf, w.ns1 / 1e6 / wf, g_ic_h1 ? "" : "; off: TOTK_OHOS_INFLIGHT_H1=0", c.s.hashed / M / cf, c.s.ns / 1e6 / cf, (unsigned long long)c.frames, (unsigned long long)c.s.slots);
    LOGV("inflight check: a frame %.1f ranges over %d KiB hashed in %d sampled 4 KiB pages (their changed MiB extrapolated), %.1f more thinned to %d pages near the frame's share of the %u MiB H0 budget, "
         "%.1f ranges (%.2f MiB) not hashed past it, census skipped in %.1f slots past its own %u MiB; "
         "not hashed: %.1f ranges (%.2f MiB) in CPU_NO_ACCESS pools, %.1f bound from backend copies, %.1f outside GPU-backed pools; %.1f reads of unknown size bounded by their game buffer; "
         "unreadable ranges (pools changed or moved, or pool lock busy) H0 %llu, H1 %llu, H2 %llu (%.2f MiB); H1 stopped before %llu ranges (the submission's last noted slot had reached H2); "
         "slots opened again before H1 %llu; notes dropped %llu, from other threads %llu; GPU-write records lost %llu; offender table full %llu times; changed pages not looked at again for a late flush %llu",
         w.paged / wf, IC_FULL_PAGES * IC_PAGE / 1024, IC_FULL_PAGES, w.sparse / wf, IC_SPARSE_PAGES, g_ic_budget_mb,
         w.budget / wf, w.budget_bytes / M / wf, w.census_capped / wf, g_ic_census_kb / 64,
         w.no_access / wf, w.no_access_bytes / M / wf, w.copies / wf, w.unbacked / wf, w.gbuf_capped / wf,
         (unsigned long long)w.raced, (unsigned long long)w.h1_skipped, (unsigned long long)c.s.skipped, c.s.skipped_bytes / M, (unsigned long long)w.h1_late,
         (unsigned long long)w.h1_reused, (unsigned long long)w.dropped, (unsigned long long)foreign, (unsigned long long)gw_lost, (unsigned long long)off_lost, (unsigned long long)c.s.late_over);
    for (int r = 0; r < IC_NROW; r++) {
        const ic_wrow *a = &w.k[r]; const ic_crow *b = &c.s.k[r];
        if (!a->ranges && !b->n) continue;
        LOGV("inflight check: %s: read %.1f ranges, %.2f MiB a frame summed over the slots that read them (%.1f uses; %.1f%% of their 4 KiB pages hashed, about how often a one-page rewrite is seen); "
             "changed at H1 %.2f ranges, %.3f MiB (of it only a size-less GPU write over it %.3f, an inline uniform update %.3f, in slots exposed to pool-table stores %.3f; GPU-written apart: %.2f ranges, %.3f MiB); "
             "changed at H2 %.2f ranges, %.3f MiB (of them only a size-less GPU write over them %.2f, %.3f MiB, an inline uniform update %.2f, %.3f MiB, in slots exposed to pool-table stores %.2f, %.3f MiB; "
             "GPU-written apart: %.2f ranges, %.3f MiB) of %.1f ranges, %.2f MiB hashed again, all summed over slots",
             ic_names[r], a->ranges / wf, a->bytes / M / wf, a->uses / wf, a->pages ? 100.0 * (double)a->hpages / (double)a->pages : 0.0,
             a->h1n / wf, a->h1b / M / wf, a->h1ab / M / wf, a->h1ub / M / wf, a->h1xb / M / wf, a->h1gn / wf, a->h1gb / M / wf,
             b->cn / cf, b->cb / M / cf, b->an / cf, b->ab / M / cf, b->un / cf, b->ub / M / cf, b->xn / cf, b->xb / M / cf, b->gn / cf, b->gb / M / cf, b->n / cf, b->b / M / cf);
    }
    ic_report_pf("bound ranges of known size", c.s.pf[0], cf);
    ic_report_pf("reads of unknown size", c.s.pf[2], cf);
    if (w.k[IC_GMEM].ranges || c.s.k[IC_GMEM].n) {
        double pop = w.census_n ? (double)w.census_pop / (double)w.census_n : 0, again = (double)c.s.k[IC_GMEM].b, share = again ? (double)c.s.k[IC_GMEM].cb / again : 0;
        LOGV("inflight check: pool-table census (an upper bound: the windows are not what the shaders read, so it is not zero with one frame in flight either; compare it between runs): "
             "64 KiB windows of the %.1f MiB of CPU-writable GPU-backed pool memory, %.2f MiB a frame in the slots that bind the pool table (%.2f ms a frame holding the pool lock); "
             "changed at H2 %.3f MiB a frame = %.3f%% of what was hashed again, so about %.2f MiB of all of it under one such slot",
             pop / M, w.k[IC_GMEM].bytes / M / wf, w.census_ns / 1e6 / wf, c.s.k[IC_GMEM].cb / M / cf, 100.0 * share, share * pop / M);
        ic_report_pf("pool-table census", c.s.pf[1], cf);
    }
    uint64_t xn = 0, xb = 0, cn = 0, cb = 0;
    for (int r = 0; r < IC_NROW; r++) { xn += c.s.k[r].xn; xb += c.s.k[r].xb; cn += c.s.k[r].cn; cb += c.s.k[r].cb; }
    LOGV("inflight check: pool-table stores (shaders that bind its write log; nothing records what they write): %.2f slots a frame bind it (all %d frames); "
         "of %llu sampled slots at H2, %llu were exposed (it, a slot before it still in flight when it opened, or a slot after it recorded by its H2, binds the log); their changes not GPU-written %.2f ranges, %.3f MiB a frame "
         "of the %.2f, %.3f MiB in all rows (part of the rows above, which the stores may explain)",
         w.gml_slots / 300.0, 300, (unsigned long long)c.s.slots, (unsigned long long)c.s.slots_exposed, xn / cf, xb / M / cf, cn / cf, cb / M / cf);
    LOGV("inflight check: distinct changed at H2 a frame (bound ranges of known size, 4 KiB pages; GPU-written ones, pages under only an inline uniform update or a size-less GPU write, "
         "the census and reads of unknown size apart (see each row's columns)): mean %.3f MiB, largest frame %.3f MiB; "
         "%.3f MiB of the mean is extrapolated from page-sampled ranges%s%s",
         c.distinct / M / cf, c.distinct_max / M, c.est / M / cf, c.set_over ? "; the page set overflowed (pages past it counted without deduplication)" : "",
         c.s.stamp_lost ? "; the flush ring had lost some of the frames' history" : "");
    LOGV("inflight check: game flushes in these 300 frames: nvnMemoryPoolFlushMappedRange %llu (%.1f MiB), nvnBufferFlushMappedRange %llu (%.1f MiB)",
         (unsigned long long)fn[0], fb[0] / M, (unsigned long long)fn[1], fb[1] / M);
    for (int i = 0; i < ntop; i++) {
        const ic_off *o = &top[i];
        LOGV("inflight check: worst %d: %s%s%s %#llx+%#llx in pool %#llx+%#llx flags %#x%s, %.1f uses a frame, changed in %u of %llu sampled frames (read in %u, counted from its first change), "
             "%.3f MiB summed over the slots of a changed frame%s",
             i + 1, ic_names[o->kind], o->rw ? " (GPU-writable)" : "", o->unb ? " (read of unknown size, to the end of its pool; MiB extrapolated over the tail)" : "",
             (unsigned long long)o->addr, (unsigned long long)o->size,
             (unsigned long long)o->pool_lo, (unsigned long long)o->pool_size, (unsigned)o->flags, ic_flag_name(o->flags),
             o->seen ? (double)o->uses / o->seen : 0.0, o->chg, (unsigned long long)c.frames, o->seen, o->chg ? o->bytes / M / o->chg : 0.0,
             o->game == 2 ? "" : o->game == 1 ? "; only inline uniform updates or size-less GPU writes over its changes" : "; a GPU write of known size was recorded over every change");
    }
}
static void ic_frame(uint64_t frame) {   /* vk_slot_stats: every present, recording lock held; `frame` has just been presented */
    if (frame % 300 == 0) ic_report(frame);
    uint64_t f = frame + 1;
    __atomic_store_n(&g_ic_frame, f, __ATOMIC_RELAXED);
    g_ic_stamp_prev = g_ic_stamp_cur; g_ic_stamp_cur = atomic_load_explicit(&g_ic_st_gen, memory_order_acquire);
    vk_inflight_on = f % (uint64_t)g_ic_k == 0;
    if (vk_inflight_on) {
        if (g_ic_fslots) g_ic_fslots_prev = g_ic_fslots;
        g_ic_fslots = 0; g_ic_frame_h0 = g_ic_frame_census = 0; g_ic_w.frames++;
        static int said;
        if (!said && vk_fg_enabled()) { said = 1; LOGV("inflight check: frame generation is on: H2 holds each slot of a sampled frame longer, so a generated frame can be refused then (no free slots); keep K large"); }
    }
}
static void snap_reset(void);
void mtl_sub_begin(void) {
    uint64_t guest_ns = T_gpu_time_pending.ns ? T_gpu_time_pending.ns : runtime_now_ns();
    uint64_t origin_ticks = T_gpu_time_pending.origin_ticks, frame = T_gpu_time_pending.frame;
    memset(&T_gpu_time_pending, 0, sizeof T_gpu_time_pending);
    totk_thread_walk();
#ifdef TOTK_OHOS
    g_is_walker = 1;
#endif
    g_lag_guest_role = nvn_dirty_lag_thread_role_get();
    nvn_dirty_lag_thread_role("walking");
    int slot_trace=__builtin_expect(atomic_load_explicit(&vk_fg_slot_boundary_active,memory_order_relaxed),0) &&
        vk_fg_job_trace_enabled();
    uint64_t slot_lock_request=slot_trace ? host_now_ns() : 0;
    vk_rec_lock(); mtl_init();
    if (!g_cur && __builtin_expect(atomic_load_explicit(&vk_fg_slot_boundary_active,memory_order_relaxed),0)) {
        uint64_t enter=slot_trace ? host_now_ns() : 0;
        if (slot_trace) LOGV("[adaptive-slot-boundary] event=enter kind=begin boundary_ns=%llu lock_request_ns=%llu open_primary=0 clock=CLOCK_MONOTONIC",
            (unsigned long long)enter,(unsigned long long)slot_lock_request);
        vk_fg_slot_boundary();
        if (slot_trace) LOGV("[adaptive-slot-boundary] event=exit kind=begin enter_ns=%llu exit_ns=%llu clock=CLOCK_MONOTONIC",
            (unsigned long long)enter,(unsigned long long)host_now_ns());
    } else if (slot_trace) {
        LOGV("[adaptive-slot-boundary] event=skip kind=begin now_ns=%llu lock_request_ns=%llu open_primary=%u active=%u clock=CLOCK_MONOTONIC",
            (unsigned long long)host_now_ns(),(unsigned long long)slot_lock_request,
            g_cur!=NULL,atomic_load_explicit(&vk_fg_slot_boundary_active,memory_order_relaxed)!=0);
    }
    if (vk_xlate_stats_on()) { g_xlate->counts.submissions++; xlate_publish(); }
    g_gpu_guest_ns = guest_ns; g_gpu_origin_ticks = origin_ticks; g_gpu_guest_frame = frame;
    lag_pressure_fallback();
    T_ft_walk_frame = totk_ft_on() ? totk_ft_current() : 0;
    totk_ft_work_begin(T_ft_walk_frame);
    if (__builtin_expect(vk_rec_profile_on, 0)) vk_rec_profile_begin(VK_REC_S_STREAM);
    else if (T_ft_walk_frame) rec_timeline_begin();
    if (g_lookup_stats) atomic_store_explicit(&g_lookup_thread, (uintptr_t)pthread_self(), memory_order_relaxed);
    g_ncarry = 0; snap_reset(); g_perf_t0 = host_now_ns(); g_perf_cpu_t0 = walker_cpu_now();
    g_perf_sub0 = g_perf_wait_ns; g_in_sub = 1;
}
void vk_gpu_write(uint64_t a, uint64_t n);
static void fill_cpu(uint64_t dst, uint64_t n, uint32_t value) { uint32_t *w = (uint32_t *)(uintptr_t)dst; for (uint64_t i = 0; i < n / 4; i++) w[i] = value; }
/* ClearBuffer (OP_FILL_B, NVN2 games): the value over whole words of [dst, dst + n), in command order: vkCmdFillBuffer in a
   pool, the CPU outside the pools (as mtl_sub_copy). */
void mtl_sub_fill(uint64_t dst, uint64_t n, uint32_t value) {
    n &= ~3ull;
    if (!n) return;
    VkBuffer db; uint64_t dof;
    int ok = vk_resolve(dst, n, &db, &dof) && !(dof & 3);
    if (ub_private_on()) vk_ub_touch(dst, n);
    else ub_flush();
    vk_gpu_write(dst, n);
    if (!ok) { vk_cpu_step(); if (__builtin_expect(g_pt_on, 0)) vk_pt_cpu_access(dst, n); VK_REC_DO(VK_REC_S, fill_cpu(dst, n, value)); return; }   /* oot-a122: copies over it land first */
    vk_end_rendering_at(VK_PASS_END_COPY, OP_FILL_B);   /* transfers happen outside rendering */
    if (__builtin_expect(g_pt_on, 0)) { VkDescriptorBufferInfo l; if (vk_pt_write(dst, n, 8, 4, &l)) { db = l.buffer; dof = l.offset; } }   /* oot-a122: a landed copy (else memory, after the copies it overlaps) */
    VkCommandBuffer cb = vk_cb();
    if (vk_gpuprof_on) vk_gpuprof_scope_begin(cb, "transfer");
    vk_barrier_all(cb);
    vkCmdFillBuffer(cb, db, dof, n, value);
    vk_barrier_all(cb);
    if (vk_gpuprof_on) vk_gpuprof_scope_end(cb);
    g_stat_gpu_copy++;
}
void mtl_sub_copy(uint64_t src, uint64_t dst, uint64_t n) {
    if (!n) return;
    tex_t trace_src, trace_dst;
    int trace = 0;
    if (totk_texcopy_on()) {
        memset(&trace_src, 0, sizeof trace_src); memset(&trace_dst, 0, sizeof trace_dst);
        trace = totk_texcopy_overlap(src, n, &trace_src) | totk_texcopy_overlap(dst, n, &trace_dst);
    }
    if (trace)
        totk_texcopy_log(vk_submit_serial(), "phase=replay op=CopyBufferToBuffer result=attempt src_ser=%llu dst_ser=%llu src_addr=%#llx dst_addr=%#llx bytes=%llu",
            (unsigned long long)trace_src.serial, (unsigned long long)trace_dst.serial,
            (unsigned long long)src, (unsigned long long)dst, (unsigned long long)n);
    vk_pd_write_note("buffer copy to", dst, n, NULL);
    { int vk_write_range_hit(uint64_t a, uint64_t n); extern uint64_t g_frame_pub __attribute__((weak)); static unsigned nb;
      if ((vk_write_range_hit(dst, n) || vk_write_range_hit(src, n)) && nb++ < 4000)
          LOGV("[b2b] fp=%llu src=%#llx dst=%#llx bytes=%#llx src_nonzero=%zu", (unsigned long long)(&g_frame_pub ? g_frame_pub : 0), (unsigned long long)src, (unsigned long long)dst, (unsigned long long)n,
               totk_texcopy_nonzero((const void *)(uintptr_t)src, (size_t)(n > (1u << 20) ? (1u << 20) : n))); }
    VkBuffer sb, db; uint64_t so, dof;
    int ok = vk_resolve(src, n, &sb, &so) && vk_resolve(dst, n, &db, &dof);
    if (ub_private_on()) { vk_ub_touch(src, n); vk_ub_touch(dst, n); }
    else ub_flush();
    vk_gpu_write(dst, n);
    if (!ok || (sb == db && so < dof + n && dof < so + n)) {   /* outside the pools, or overlapping in one buffer (Vulkan forbids it): the CPU does it in order */
        vk_cpu_step();
        if (__builtin_expect(g_pt_on, 0)) { vk_pt_cpu_access(src, n); vk_pt_cpu_access(dst, n); }   /* oot-a122: copies over either land first */
        VK_REC_DO(VK_REC_S, memmove((void *)(uintptr_t)dst, (const void *)(uintptr_t)src, (size_t)n));
        if (trace) {
            size_t counted = (size_t)(n > (1u << 20) ? (1u << 20) : n);
            totk_texcopy_log(vk_submit_serial(), "phase=replay op=CopyBufferToBuffer result=success path=cpu src_ser=%llu dst_ser=%llu counted=%zu nonzero=%zu truncated=%d",
                (unsigned long long)trace_src.serial, (unsigned long long)trace_dst.serial, counted,
                totk_texcopy_nonzero((const void *)(uintptr_t)dst, counted), n > counted);
        }
        return;
    }
    vk_end_rendering_at(VK_PASS_END_COPY, OP_COPY_B2B);   /* transfers happen outside rendering */
    VkDescriptorBufferInfo frozen;
    if (vk_lag_read(src, n, 16, &frozen, VK_LAG_COPY)) { sb = frozen.buffer; so = frozen.offset; }
    VK_INFLIGHT(VK_INFLIGHT_COPY, src, n, sb);
    if (__builtin_expect(g_pt_on, 0)) { VkDescriptorBufferInfo l; if (vk_pt_write(dst, n, 4, 1, &l)) { db = l.buffer; dof = l.offset; } }   /* oot-a122: a landed copy */
    VkCommandBuffer cb = vk_cb();
    if (vk_gpuprof_on) vk_gpuprof_scope_begin(cb, "transfer");
    vk_barrier_all(cb);
    VkBufferCopy c = {so, dof, n}; vkCmdCopyBuffer(cb, sb, db, 1, &c);
    vk_barrier_all(cb);
    if (vk_gpuprof_on) vk_gpuprof_scope_end(cb);
    if (trace)
        totk_texcopy_log(vk_submit_serial(), "phase=replay op=CopyBufferToBuffer result=success path=gpu src_ser=%llu dst_ser=%llu src_buffer=%#llx src_offset=%#llx dst_buffer=%#llx dst_offset=%#llx bytes=%llu content=not-sampled",
            (unsigned long long)trace_src.serial, (unsigned long long)trace_dst.serial,
            (unsigned long long)(uintptr_t)sb, (unsigned long long)so, (unsigned long long)(uintptr_t)db, (unsigned long long)dof, (unsigned long long)n);
    g_stat_gpu_copy++;
}
/* ---------- inline uniform updates without splitting the render pass (nvn_mtl.m "renaming") ----------
   The game updates small uniform ranges between draws (nvnCommandBufferUpdateUniformBuffer) over a hundred times per
   submission. A buffer write cannot happen inside a render pass, and on a tiled GPU every pass split stores and reloads
   all attachments. So an update is only logged: a later uniform binding that overlaps logged updates gets a private
   copy in the submission's transient memory (the pool's bytes with the logged updates laid over them in order), so draws
   before an update read the old bytes and draws after it the new ones. The logged updates reach the pool at the end of
   the submission, or earlier when anything other than a uniform binding may read or write an updated range
   (vk_ub_touch: vertex, index and storage buffers of a draw; copies, dispatches and other work outside passes).
   An entry stays logged, as an overlay for later copies, until the GPU has finished its submission (the CPU's view of
   the pool only has it from then). Renaming is opt-in (TOTK_VK_UB_RENAME=1); by default updates split the pass and write
   in place, except protected frame lag. There, UBO-only updates remain CPU-known logical shadows and bindings use
   immutable submission-arena copies. Only real aliases materialize GPU writes; remaining CPU-known bytes publish
   after the final QueueSubmit, before synchronous replay returns to the CPU producer. This is not global renaming. */
typedef struct { uint64_t lo, hi, seq, fence; int flushed; uint8_t *data; } ub_ent;   /* fence: submission it reached memory in (0: not yet) */
static uint64_t ub_address(uint64_t addr, uint64_t size);
static ub_ent *g_ub; static uint64_t *g_ub_maxhi; static int g_nub, g_capub, g_ub_unflushed; static uint64_t g_ub_seq, g_ub_lo = UINT64_MAX, g_ub_hi;
static uint64_t g_ub_maxlen, g_ub_pruned_at;   /* longest logged entry (bounds the overlap scan); g_done at the last prune */
static uint64_t g_ub_gen = 1;   /* rename memo: every log or snapshot change invalidates its answer */
uint64_t vk_record_uniform_revision(void) { return g_ub_gen; }
int vk_record_updates_pending(void) {
    if (vk_rec_profile_on && T_rec.active) { g_rec.touch_calls++; g_rec.touch_no_updates += g_ub_unflushed == 0; }
    return g_ub_unflushed != 0;
}
/* Exact range invalidations for recorder freezes. Missing/overwritten journal entries conservatively miss. */
enum { REC_UNIFORM_CHANGES = 256 };
static struct { uint64_t revision, lo, hi; } g_record_uniform_changes[REC_UNIFORM_CHANGES];
static void ub_changed(uint64_t address, uint64_t size) {
    g_ub_gen++;
    if (vk_record_enabled() || vk_lag_capture_on()) {
        uint64_t hi = address + size; if (hi < address) hi = UINT64_MAX;
        g_record_uniform_changes[g_ub_gen % REC_UNIFORM_CHANGES] = (__typeof__(*g_record_uniform_changes)){g_ub_gen, address, hi};
    }
}
static int ub_range_unchanged(uint64_t revision, uint64_t address, uint64_t size) {
    if (revision == g_ub_gen) return 1;
    if (!revision || revision > g_ub_gen || g_ub_gen - revision > REC_UNIFORM_CHANGES || address + size < address) return 0;
    for (uint64_t i = revision + 1; i <= g_ub_gen; i++) {
        const __typeof__(*g_record_uniform_changes) *c = &g_record_uniform_changes[i % REC_UNIFORM_CHANGES];
        if (c->revision != i || (address < c->hi && c->lo < address + size)) return 0;
    }
    return 1;
}
/* Pending-original authority is independent of snapshot resets and CPU-only logical overlays. Only an event
   introducing a queued original-memory writer advances this journal; completion can only remove producers.
   Keep repeated writes before seen-range deduplication. Exact history loss conservatively misses unless the
   spatial revision summary independently proves that no producer could have touched the requested range. */
enum { LAG_PRODUCER_CHANGES = 4096, LAG_PRODUCER_BUCKETS = 4096, LAG_PRODUCER_MAX_WINDOWS = 64 };
static uint64_t g_lag_producer_revision = 1;
static struct { uint64_t revision, lo, hi; } g_lag_producer_changes[LAG_PRODUCER_CHANGES];
/* Walk-owned monotonic revisions, not dirty/watch or byte authority. Hash collisions only make a query
   consult the exact journal. Large/overflowing writes advance the global floor instead of walking unbounded VAs. */
static uint64_t g_lag_producer_buckets[LAG_PRODUCER_BUCKETS], g_lag_producer_wide;
static unsigned lag_producer_bucket(uint64_t window) {
    return (unsigned)(window ^ (window >> 12) ^ (window >> 24)) & (LAG_PRODUCER_BUCKETS - 1);
}
static void lag_producer_changed(uint64_t address, uint64_t size) {
    if (!vk_lag_capture_on()) return;
    uint64_t revision = ++g_lag_producer_revision, hi = address + size;
    if (hi < address) hi = UINT64_MAX;
    g_lag_producer_changes[revision % LAG_PRODUCER_CHANGES] =
        (__typeof__(*g_lag_producer_changes)){revision, address, hi};
    uint64_t first = address >> 16, last = (hi - 1) >> 16;
    if (!size || hi <= address || last - first >= LAG_PRODUCER_MAX_WINDOWS) g_lag_producer_wide = revision;
    else for (uint64_t w = first; w <= last; w++) g_lag_producer_buckets[lag_producer_bucket(w)] = revision;
}
static int lag_producer_unchanged(uint64_t revision, uint64_t address, uint64_t size) {
    if (revision == g_lag_producer_revision) return 1;
    if (!revision || revision > g_lag_producer_revision || address + size < address) return 0;
    uint64_t first = address >> 16, last = (address + size - 1) >> 16;
    if (size && last - first < LAG_PRODUCER_MAX_WINDOWS && g_lag_producer_wide <= revision) {
        int unchanged = 1;
        for (uint64_t w = first; w <= last; w++)
            if (g_lag_producer_buckets[lag_producer_bucket(w)] > revision) { unchanged = 0; break; }
        if (unchanged) return 1;
    }
    if (g_lag_producer_revision - revision > LAG_PRODUCER_CHANGES) return 0;
    for (uint64_t i = revision + 1; i <= g_lag_producer_revision; i++) {
        const __typeof__(*g_lag_producer_changes) *c = &g_lag_producer_changes[i % LAG_PRODUCER_CHANGES];
        if (c->revision != i || (address < c->hi && c->lo < address + size)) return 0;
    }
    return 1;
}
int vk_record_uniform_unchanged(uint64_t revision, uint64_t address, uint64_t size) {
    return ub_range_unchanged(revision, ub_address(address, size), size);
}
int vk_off(const char *name) {
    const char *e = getenv("TOTK_VK_OFF"); if (!e || !*e) return 0;
    size_t n = strlen(name);
    for (const char *p = e; *p; ) {
        const char *q = strchr(p, ','); size_t l = q ? (size_t)(q - p) : strlen(p);
        if ((l == n && !strncmp(p, name, n)) || (l == 3 && !strncmp(p, "all", 3))) return 1;
        p += l; if (*p == ',') p++;
    }
    return 0;
}
static int ub_rename_on(void) { static int on = -1; if (on < 0) { const char *e = getenv("TOTK_VK_UB_RENAME"); on = e && atoi(e) && !vk_off("rename"); } return on; }
static int ub_private_on(void) { return vk_lag_capture_on() && !ub_rename_on(); }
int vk_lag_uniform_private(void) { return ub_private_on(); }
/* The replay lock gives these counters one walking owner; workers never increment them. Keep cumulative totals
   for host fixtures and take interval deltas only at the existing 300-frame report. Disabled lag never visits them. */
static vk_lag_kind_stats g_lag_kind[VK_LAG_KIND_COUNT];
static uint64_t lag_hot_limit(void);
static int lag_timing_on(void);
#ifdef TOTK_VK_TESTS
void vk_lag_test_stats_get(vk_lag_test_stats *out) { memcpy(out->kind, g_lag_kind, sizeof g_lag_kind); }
uint32_t vk_lag_test_features(void) { return VK_LAG_TEST_CROSS_FRAME_METADATA | VK_LAG_TEST_UNIFORM_COVER; }
#endif
/* Match the existing lag memo semantics, not the nested producer/watch/copy cache probes. */
static inline void ft_lag_count(vk_lag_kind kind, int miss) {
    if (__builtin_expect(totk_ft_work_frame() == 0, 1)) return;
    static const enum totk_ft_counter counters[] = {
        FT_COUNT_LAG_VERTEX, FT_COUNT_LAG_INDEX, FT_COUNT_LAG_STORAGE, FT_COUNT_LAG_UNIFORM
    };
    if ((unsigned)kind < sizeof counters / sizeof *counters) totk_ft_count(counters[kind] + miss, 1);
    (void)miss; /* host builds compile out the counter API */
}
static inline void ft_snapshot_allocation(uint64_t size) {
    totk_ft_count(FT_COUNT_SNAP_ALLOCS, 1);
    totk_ft_count(FT_COUNT_SNAP_BYTES, size);
    (void)size;
}
static uint64_t lag_decision_begin(vk_lag_kind kind, uint64_t size) {
    vk_lag_kind_stats *s = &g_lag_kind[kind];
    ft_lag_count(kind, 0);
    s->decisions++;
    if (size > s->max_size) s->max_size = size;
    unsigned bucket = size <= 256 ? 0 : size <= 4096 ? 1 : size <= 65536 ? 2 : size <= lag_hot_limit() ? 3 : 4;
    s->size_buckets[bucket]++;
    return (s->decisions & 63) == 1 && lag_timing_on() ? host_now_ns() : 0;
}
static void lag_decision_end(vk_lag_kind kind, uint64_t start) {
    if (start) { g_lag_kind[kind].sample_ns += host_now_ns() - start; g_lag_kind[kind].sample_count++; }
}
static void lag_snapshot_allocation(vk_lag_kind kind, uint64_t size) {
    g_lag_kind[kind].snapshot_allocations++; g_lag_kind[kind].snapshot_bytes += size;
    if (kind == VK_LAG_UNIFORM) ft_snapshot_allocation(size);
}
/* Fold every address lane before the table mask: equal-sized streams 64 KiB apart must not collide. */
static unsigned lag_hash(uint64_t address, uint64_t size, uint64_t align) {
    uint64_t x = (address >> 4) ^ (address >> 16) ^ (address >> 32) ^ size ^ (align << 7);
    x ^= x >> 23; x *= UINT64_C(0x9e3779b1); x ^= x >> 16;
    return (unsigned)x;
}
/* One direct-mapped collision used to trigger a scan of every physical pool on every alternating
   binding. Four ways retain those owners without changing mapping/window or byte-authority keys. */
static uint64_t lag_owner_base(VkBuffer buffer) {
    enum { NB = 4096, WAYS = 4 };
    static struct { VkBuffer buffer; uint64_t generation, base; } owners[NB][WAYS];
    static unsigned char next[NB];
    unsigned h = lag_hash((uint64_t)(uintptr_t)buffer, 0, 0) % NB;
    uint64_t generation = atomic_load_explicit(&g_owner_gen, memory_order_acquire);
    unsigned replace = next[h] % WAYS;
    for (unsigned i = 0; i < WAYS; i++) {
        if (owners[h][i].generation != generation) { replace = i; continue; }
        if (owners[h][i].buffer == buffer) return owners[h][i].base;
    }
    uint64_t base = 0;
    pthread_mutex_lock(&g_pool_mu);
    for (int i = 0; i < g_npool; i++)
        if (g_pools[i]->kind == K_OWN && g_pools[i]->buf == buffer) { base = g_pools[i]->base; break; }
    generation = atomic_load_explicit(&g_owner_gen, memory_order_relaxed);
    pthread_mutex_unlock(&g_pool_mu);
    owners[h][replace] = (__typeof__(owners[0][0])){buffer, generation, base};
    next[h] = (unsigned char)(replace + 1);
    return base;
}
#ifdef TOTK_VK_TESTS
static uint64_t lag_test_owner_direct(VkBuffer buffer) {
    static struct { VkBuffer buffer; uint64_t generation, base; } owners[4096];
    unsigned h = lag_hash((uint64_t)(uintptr_t)buffer, 0, 0) % 4096;
    uint64_t generation = atomic_load_explicit(&g_owner_gen, memory_order_acquire);
    if (owners[h].buffer == buffer && owners[h].generation == generation) return owners[h].base;
    uint64_t base = 0;
    pthread_mutex_lock(&g_pool_mu);
    for (int i = 0; i < g_npool; i++) if (g_pools[i]->kind == K_OWN && g_pools[i]->buf == buffer) { base = g_pools[i]->base; break; }
    generation = atomic_load_explicit(&g_owner_gen, memory_order_relaxed);
    pthread_mutex_unlock(&g_pool_mu);
    owners[h] = (__typeof__(owners[0])){buffer, generation, base};
    return base;
}
static int lag_test_owner_bench(void) {
    if (g_npool || g_pools) return 1; /* isolated pre-init query fixture only */
    enum { POOLS = 2048, CALLS = 20000 };
    pool_ent *pools = calloc(POOLS, sizeof *pools);
    g_pools = calloc(POOLS, sizeof *g_pools); g_npool = POOLS;
    if (!pools || !g_pools) abort();
    for (int i = 0; i < POOLS; i++) { g_pools[i] = &pools[i]; pools[i].kind = K_OWN; }
    VkBuffer collision[4]; unsigned count = 0;
    for (uintptr_t value = 16; count < 4; value += 16) if (lag_hash(value, 0, 0) % 4096 == 0)
        collision[count++] = (VkBuffer)value;
    for (int i = 0; i < 4; i++) { pools[POOLS - 4 + i].buf = collision[i]; pools[POOLS - 4 + i].base = (uint64_t)(i + 1) * 65536; }
    atomic_fetch_add_explicit(&g_owner_gen, 1, memory_order_release);
    uint64_t ns[2][3], checksum[2] = {0}; unsigned samples[2] = {0};
    for (unsigned pass = 0; pass < 6; pass++) {
        unsigned mode = (pass == 1 || pass == 2 || pass == 5);
        uint64_t sum = 0, start = host_now_ns();
        for (unsigned i = 0; i < CALLS; i++) sum += mode ? lag_owner_base(collision[i % 4]) : lag_test_owner_direct(collision[i % 4]);
        ns[mode][samples[mode]++] = host_now_ns() - start; checksum[mode] += sum;
    }
    unsigned errors = checksum[0] != checksum[1];
    for (unsigned mode = 0; mode < 2; mode++)
        for (unsigned i = 1; i < 3; i++) for (unsigned j = i; j && ns[mode][j] < ns[mode][j - 1]; j--) {
            uint64_t swap = ns[mode][j]; ns[mode][j] = ns[mode][j - 1]; ns[mode][j - 1] = swap;
        }
    printf("lag-query owner collision stress: %u pools, 4 colliding handles; direct %.2f four-way %.2f ns/query "
           "(interleaved medians, not an observed device collision rate); checksums %llu/%llu\n",
           POOLS, ns[0][1] / (double)CALLS, ns[1][1] / (double)CALLS,
           (unsigned long long)checksum[0], (unsigned long long)checksum[1]);
    pools[POOLS - 4].base += 16;
    atomic_fetch_add_explicit(&g_owner_gen, 1, memory_order_release);
    errors += lag_owner_base(collision[0]) != pools[POOLS - 4].base;
    pools[POOLS - 4].kind = K_CPU;
    atomic_fetch_add_explicit(&g_owner_gen, 1, memory_order_release);
    errors += lag_owner_base(collision[0]) != 0; /* negative answers are generation-scoped too */
    free(pools); free(g_pools); g_pools = NULL; g_npool = 0;
    atomic_fetch_add_explicit(&g_owner_gen, 1, memory_order_release);
    return errors != 0;
}
#endif
static uint64_t lag_physical(uint64_t addr, uint64_t size);
static uint64_t ub_address(uint64_t addr, uint64_t size) { return ub_private_on() ? lag_physical(addr, size) : addr; }
static void ub_maxhi_from(int i) { for (; i < g_nub; i++) { uint64_t prev = i ? g_ub_maxhi[i - 1] : 0; g_ub_maxhi[i] = g_ub[i].hi > prev ? g_ub[i].hi : prev; } }
static uint64_t g_prune_floor = UINT64_MAX;
static void ub_prune_inner(void) {   /* drop entries whose submission the GPU has finished, and that no later snapshot still needs */
    /* Only a finished submission or a changed snapshot floor can drop entries; vk_ub_rename calls this for every uniform
       binding, so without either it returns at once. */
    uint64_t done = atomic_load(&g_done); int j = 0;
    if (done > g_prune_floor) done = g_prune_floor;   /* a submission queued after the one replaying captured no earlier than it */
    if (done == g_ub_pruned_at) return;
    g_ub_pruned_at = done;
    g_ub_lo = UINT64_MAX; g_ub_hi = 0; g_ub_maxlen = 0;
    for (int i = 0; i < g_nub; i++) {
        if (g_ub[i].fence && g_ub[i].fence <= done) { free(g_ub[i].data); continue; }
        g_ub[j++] = g_ub[i];
        if (g_ub[i].lo < g_ub_lo) g_ub_lo = g_ub[i].lo; if (g_ub[i].hi > g_ub_hi) g_ub_hi = g_ub[i].hi;
        if (g_ub[i].hi - g_ub[i].lo > g_ub_maxlen) g_ub_maxlen = g_ub[i].hi - g_ub[i].lo;
    }
    if (j != g_nub) { g_nub = j; ub_maxhi_from(0); ub_changed(0, 0); }
}
static void ub_prune(void) { VK_REC_DO(VK_REC_S_UNIFORM, ub_prune_inner()); }
static void snap_note_update(uint64_t lo, uint64_t n);
static void ub_log(uint64_t lo, const uint8_t *data, uint64_t n, int flushed) {
    lo = ub_address(lo, n);
    /* Entries the new update covers entirely can never show through again: renames, flushes and overlays all apply the
       newer one after them, and bindings recorded before it already have their copies. The game rewrites the same
       constant blocks many times a frame; keeping only the live ones keeps every lookup short. */
    int a = 0, b = g_nub; while (a < b) { int m = (a + b) >> 1; if (g_ub[m].lo < lo) a = m + 1; else b = m; }
    uint8_t *reused = NULL; int recorder = vk_record_enabled();
    int first = a, j = a;
    for (int i = a; i < g_nub; i++) {
        if (g_ub[i].lo < lo + n && g_ub[i].hi <= lo + n) {
            if (!g_ub[i].flushed) g_ub_unflushed--;
            if (recorder && !reused && g_ub[i].hi - g_ub[i].lo == n) reused = g_ub[i].data;
            else free(g_ub[i].data);
            continue;
        }
        if (g_ub[i].lo >= lo + n) {   /* past the new range: the rest stays, moved down only if something was dropped */
            if (j != i) { int rest = g_nub - i; memmove(g_ub + j, g_ub + i, (size_t)rest * sizeof *g_ub); j += rest; } else j = g_nub;
            break;
        }
        g_ub[j++] = g_ub[i];
    }
    if (j != g_nub) { g_nub = j; ub_maxhi_from(first); }
    if (g_nub == g_capub) { g_capub = g_capub ? 2 * g_capub : 256; g_ub = VK_REALLOC(g_ub, (size_t)g_capub * sizeof *g_ub); g_ub_maxhi = VK_REALLOC(g_ub_maxhi, (size_t)g_capub * sizeof *g_ub_maxhi); }
    a = 0, b = g_nub; while (a < b) { int m = (a + b) >> 1; if (g_ub[m].lo <= lo) a = m + 1; else b = m; }   /* sorted by lo */
    memmove(g_ub + a + 1, g_ub + a, (size_t)(g_nub - a) * sizeof *g_ub);
    uint8_t *d = reused ? reused : VK_MALLOC((size_t)n); memcpy(d, data, (size_t)n);
    if (vk_rec_profile_on && T_rec.active) { if (reused) g_rec.ub_reused++; else g_rec.ub_allocated++; }
    g_ub[a] = (ub_ent){lo, lo + n, ++g_ub_seq, 0, flushed, d}; g_nub++; g_ub_unflushed += !flushed; ub_changed(lo, n);
    snap_note_update(lo, n);
    ub_maxhi_from(a);
    if (lo < g_ub_lo) g_ub_lo = lo; if (lo + n > g_ub_hi) g_ub_hi = lo + n; if (n > g_ub_maxlen) g_ub_maxlen = n;
}
/* the logged entries overlapping [lo, hi) (only_unflushed: not yet written), in update order; returns how many.
   Entries are sorted by start: walking back from the last one starting below hi, the walk ends where no earlier entry
   can reach lo, by the running maximum of ends or, tighter when one long update keeps that high, by the longest entry. */
static int ub_overlaps(uint64_t lo, uint64_t hi, int only_unflushed, int *idx, int cap) {
    if (!g_nub || hi <= g_ub_lo || lo >= g_ub_hi) return 0;
    int a = 0, b = g_nub; while (a < b) { int m = (a + b) >> 1; if (g_ub[m].lo < hi) a = m + 1; else b = m; }
    int n = 0; uint64_t floor = lo > g_ub_maxlen ? lo - g_ub_maxlen : 0;   /* an entry starting at or below floor ends by lo */
    for (int i = a - 1; i >= 0 && g_ub_maxhi[i] > lo && (g_ub[i].lo > floor || !floor); i--) if (g_ub[i].hi > lo && (!only_unflushed || !g_ub[i].flushed)) { if (n == cap) return -1; idx[n++] = i; }
    for (int i = 1; i < n; i++) for (int j = i; j > 0 && g_ub[idx[j - 1]].seq > g_ub[idx[j]].seq; j--) { int t = idx[j]; idx[j] = idx[j - 1]; idx[j - 1] = t; }
    return n;
}
static int ub_entry_cmp(const void *x, const void *y) {
    const ub_ent *a = x, *b = y;
    return a->lo < b->lo ? -1 : a->lo > b->lo;
}
static void ub_reindex(void) {
    g_ub_lo = UINT64_MAX; g_ub_hi = g_ub_maxlen = 0; g_ub_unflushed = 0;
    for (int i = 0; i < g_nub; i++) {
        ub_ent *e = &g_ub[i];
        if (e->lo < g_ub_lo) g_ub_lo = e->lo;
        if (e->hi > g_ub_hi) g_ub_hi = e->hi;
        if (e->hi - e->lo > g_ub_maxlen) g_ub_maxlen = e->hi - e->lo;
        g_ub_unflushed += !e->flushed;
    }
    ub_maxhi_from(0);
}
/* Revoke only bytes replaced by a real producer. A partial producer must not erase neighbouring private updates. */
static void ub_forget(uint64_t a, uint64_t n) {
    int changed = 0;
    for (int i = 0; i < g_nub; i++) {
        ub_ent *e = &g_ub[i];
        if (e->lo >= a + n || e->hi <= a) continue;
        changed = 1;
        if (a <= e->lo && e->hi <= a + n) {
            free(e->data); memmove(e, e + 1, (size_t)(g_nub - i - 1) * sizeof *e); g_nub--; i--;
        } else if (a <= e->lo) {
            uint64_t skip = a + n - e->lo;
            memmove(e->data, e->data + skip, (size_t)(e->hi - a - n)); e->lo = a + n;
        } else if (e->hi <= a + n) e->hi = a;
        else {
            ub_ent right = *e;
            right.lo = a + n; right.data = VK_MALLOC((size_t)(right.hi - right.lo));
            memcpy(right.data, e->data + right.lo - e->lo, (size_t)(right.hi - right.lo)); e->hi = a;
            if (g_nub == g_capub) {
                g_capub = g_capub ? 2 * g_capub : 256;
                g_ub = VK_REALLOC(g_ub, (size_t)g_capub * sizeof *g_ub);
                g_ub_maxhi = VK_REALLOC(g_ub_maxhi, (size_t)g_capub * sizeof *g_ub_maxhi);
            }
            g_ub[g_nub++] = right;
        }
    }
    if (changed) { qsort(g_ub, (size_t)g_nub, sizeof *g_ub, ub_entry_cmp); ub_reindex(); ub_changed(a, n); }
}
/* Protected lag already forces synchronous replay in hle_nvn.c. Publish CPU-known, UBO-only updates after ALL
   recorder jobs join and their final ticket is submitted, before the producer can rewrite the next frame.
   This is a host publication, not a queued original-buffer GPU write: old immutable descriptors cannot be clobbered.
   lag_store retains genuine non-UBO unit guards (including neighbouring aliases), and may wait only on submitted work.
   Never use dirty-unit changes to discard authority: an unrelated CPU write in the same unit is not an overwrite. */
static void ub_publish(void) {
    const char *previous_role = nvn_dirty_lag_thread_role_get();
    nvn_dirty_lag_thread_role("uniform-publication");
    if (!g_ub_unflushed) {
        nvn_dirty_lag_thread_role(previous_role);
        return;
    }
    static int *order, cap;
    if (g_ub_unflushed > cap) { cap = g_ub_unflushed; order = VK_REALLOC(order, (size_t)cap * sizeof *order); }
    int n = 0;
    for (int i = 0; i < g_nub; i++) if (!g_ub[i].flushed) order[n++] = i;
    for (int i = 1; i < n; i++) for (int j = i; j > 0 && g_ub[order[j - 1]].seq > g_ub[order[j]].seq; j--) {
        int t = order[j]; order[j] = order[j - 1]; order[j - 1] = t;
    }
    for (int k = 0; k < n; k++) {
        ub_ent *e = &g_ub[order[k]];
        if (!nvn_dirty_lag_store((void *)(uintptr_t)e->lo, e->data, (size_t)(e->hi - e->lo)))
            DIE("lag uniform publication could not restore protection");
    }
    int j = 0;
    for (int i = 0; i < g_nub; i++) {
        if (!g_ub[i].flushed) free(g_ub[i].data);
        else g_ub[j++] = g_ub[i];
    }
    g_nub = j; ub_reindex(); ub_changed(0, UINT64_MAX);
    nvn_dirty_lag_thread_role(previous_role);
}
/* Materialize only updates a real consumer can observe. Chunk seals are not consumers of private UBO bytes. */
static void ub_flush_inner(uint64_t lo, uint64_t hi) {
    if (!g_ub_unflushed) return;
    int cnt = 0;
    for (int i = 0; i < g_nub; i++) cnt += !g_ub[i].flushed && g_ub[i].lo < hi && lo < g_ub[i].hi;
    if (!cnt) return;
    vk_end_rendering_at(VK_PASS_END_UNIFORM, 0);
    VkCommandBuffer cb = vk_cb();
    vk_barrier_all(cb);
    static int *order, cap;
    if (cnt > cap) { cap = cnt; order = VK_REALLOC(order, (size_t)cap * sizeof *order); }
    int n = 0;
    for (int i = 0; i < g_nub; i++) if (!g_ub[i].flushed && g_ub[i].lo < hi && lo < g_ub[i].hi) order[n++] = i;
    for (int i = 1; i < n; i++) for (int j = i; j > 0 && g_ub[order[j - 1]].seq > g_ub[order[j]].seq; j--) { int t = order[j]; order[j] = order[j - 1]; order[j - 1] = t; }
    uint64_t prev_lo = UINT64_MAX, prev_hi = 0;
    for (int k = 0; k < n; k++) {
        ub_ent *e = &g_ub[order[k]]; VkBuffer db; uint64_t dof;
        if (vk_resolve(e->lo, e->hi - e->lo, &db, &dof)) {
            lag_guard_mark(e->lo, e->hi - e->lo, NVN_DIRTY_LAG_WRITABLE | NVN_DIRTY_LAG_GPU);
            if (e->lo < prev_hi && prev_lo < e->hi) {   /* overlapping writes in order */
                VkMemoryBarrier2 mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT, .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                       .dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT, .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT};
                VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
                vkCmdPipelineBarrier2(cb, &di);
                prev_lo = UINT64_MAX; prev_hi = 0;
            }
            vkCmdUpdateBuffer(cb, db, dof, e->hi - e->lo, e->data);
            if (__builtin_expect(g_ic_k, 0) && ic_gw_window()) ic_gpu_wrote(e->lo, e->hi - e->lo, VK_INFLIGHT_GW_UNIFORM);   /* in-flight check: an inline uniform update */
            if (e->lo < prev_lo) prev_lo = e->lo;
            if (e->hi > prev_hi) prev_hi = e->hi;
        }
        e->flushed = 1; g_ub_unflushed--;
        ub_changed(e->lo, e->hi - e->lo); /* a private overlay became a real original-memory producer */
        lag_producer_changed(e->lo, e->hi - e->lo);
    }
    vk_barrier_all(cb);
}
static void ub_flush(void) { VK_REC_DO(VK_REC_S_UNIFORM, ub_flush_inner(0, UINT64_MAX)); }
static int ub_pending_physical(uint64_t addr, uint64_t n);
void vk_ub_touch(uint64_t addr, uint64_t n) {   /* a non-uniform access: the updates it may see go to memory first */
    if (!g_ub_unflushed || !n) return;
    addr = ub_address(addr, n);
    if (ub_pending_physical(addr, n)) {
        if (ub_private_on()) VK_REC_DO(VK_REC_S_UNIFORM, ub_flush_inner(addr, addr + n));
        else ub_flush();
    }
}
void vk_ub_flush(void) { ub_flush(); }
int vk_ub_pending(uint64_t addr, uint64_t n) {
    if (!g_ub_unflushed || !n) return 0; /* no updates: no physical mapping can change the answer */
    return ub_pending_physical(ub_address(addr, n), n);
}
/* addr is already canonical (ub_address). Mapping is idempotent, so vk_ub_touch need not map a second time: that
   repeated lookup missed the window memo (keyed by the virtual address) and was ~18% of lag_physical_window. */
static int ub_pending_physical(uint64_t addr, uint64_t n) {
    if (!g_ub_unflushed || !n) return 0;
#ifdef TOTK_VK_TESTS
    if (ub_address(addr, n) != addr) DIE("ub_pending_physical: address mapping is not idempotent at 0x%llx+%llu",
                                         (unsigned long long)addr, (unsigned long long)n);
#endif
    if (!vk_record_enabled()) { int idx[1]; return ub_overlaps(addr, addr + n, 1, idx, 1) != 0; }
    enum { NC = 256 }; static struct { uint64_t address, size, revision; } negative[NC];
    __typeof__(negative[0]) *m = &negative[(unsigned)((addr >> 4) ^ (addr >> 18) ^ n) % NC];
    if (m->address == addr && m->size == n && m->revision &&
        (m->revision == g_ub_gen || vk_record_uniform_unchanged(m->revision, addr, n))) {
        m->revision = g_ub_gen;
        if (vk_rec_profile_on && T_rec.active) g_rec.touch_negative_hits++;
        return 0;
    }
    int idx[1], pending = ub_overlaps(addr, addr + n, 1, idx, 1) != 0;
    if (!pending) *m = (__typeof__(*m)){addr, n, g_ub_gen};
    return pending;
}
void vk_record_touch_buffer(uint64_t address, uint64_t size) {
    if (!address || !g_ub_unflushed) return;
    if (vk_rec_profile_on && T_rec.active) g_rec.touch_ranges++;
    if (size && address + size >= address && !vk_ub_pending(address, size)) {
        if (vk_rec_profile_on && T_rec.active) g_rec.touch_resolve_avoided++;
        return;
    }
    VkBuffer buffer; uint64_t offset, available;
    if (vk_resolve_for(address, 1, &buffer, &offset, &available, VK_LOOKUP_TOUCH))
        vk_ub_touch(address, size && size < available ? size : available);
}
/* ---------- uniform snapshots at submit (nvn_mtl.m usnap) ----------
   The GPU reads a submission's uniforms when it gets to them, which here can be a few frames after the submit call,
   while the game rewrites its constant pools right after submitting (the next frame's values): objects then drew with
   the wrong frame's uniforms (black flicker). So the uniform ranges a submission binds are copied when it is submitted
   (mtl_sub_capture_ubos on the game thread with TOTK_ASYNC_REPLAY, else at the start of the replay, which then runs inside
   the submit call), merged per pool, into the submission's transient memory; bindings read those copies. Logged inline
   updates of earlier submissions that were not in memory at the capture are laid over the copies; this submission's own
   updates go through renaming on top of them. A GPU copy into a range drops its snapshot (later bindings read memory on
   the GPU timeline). Ordinary snapshots require opt-in renaming; protected rename-OFF UBO captures are mandatory. */
typedef struct { uint64_t done; int n; uint64_t *lo, *hi; size_t *off; uint8_t *bytes; } ucap_t;
typedef struct { uint64_t lo, hi; VkBuffer buf; VkDeviceSize off; uint8_t *cpu; int dead, updated; } snap_t;
static snap_t *g_snap; static int g_nsnap, g_capsnap;
static uint64_t g_snap_seq, g_snap_generation;   /* overlay watermark and immutable game-submission base identity */
static void snap_reset(void) { g_nsnap = 0; g_snap_seq = g_ub_seq; g_snap_generation++; g_prune_floor = UINT64_MAX; ub_changed(0, UINT64_MAX); }
static uint64_t g_stat_snap_bytes, g_stat_snap_ranges, g_stat_snap_binds;
static int snap_on(void) { return (ub_rename_on() && !vk_off("snap")) || ub_private_on(); }
/* GPU writes (writable storage bindings, buffer copies, texture-to-buffer copies) with the submission they belong to
   (0 while it is being recorded): a capture taken while one of them may still be executing would miss its result, so
   ranges they overlap are not copied (bindings there read memory on the GPU timeline). */
/* Sorted by lo, with g_gw_maxhi[i] the largest hi among entries 0..i: an overlap query is a binary search plus a
   backward walk that stops once no earlier entry can reach the query (the uniform log's index, ub_original_pending).
   Captures query once per UBO range and lag reads once per memo miss, so the previous linear scan per query cost
   ~0.8 ms/frame on the Thor walker. Sealing only assigns fences and pruning is a stable filter: both keep the order. */
typedef struct { uint64_t lo, hi, fence; } gw_t;
static gw_t *g_gw; static uint64_t *g_gw_maxhi; static int g_ngw, g_capgw; static pthread_mutex_t g_gw_mu = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_stat_snap_skipped;
static void gw_reindex_from(int i) {   /* caller holds g_gw_mu */
    uint64_t m = i ? g_gw_maxhi[i - 1] : 0;
    for (; i < g_ngw; i++) { if (g_gw[i].hi > m) m = g_gw[i].hi; g_gw_maxhi[i] = m; }
}
static int gw_lower(uint64_t lo) {   /* first entry with lo >= the argument; caller holds g_gw_mu */
    int a = 0, b = g_ngw;
    while (a < b) { int m = (a + b) >> 1; if (g_gw[m].lo < lo) a = m + 1; else b = m; }
    return a;
}
static int gw_pending(uint64_t lo, uint64_t hi, uint64_t done) {   /* caller holds g_gw_mu */
    for (int i = gw_lower(hi) - 1; i >= 0 && g_gw_maxhi[i] > lo; i--)
        if (lo < g_gw[i].hi && (!g_gw[i].fence || g_gw[i].fence > done)) return 1;
    return 0;
}
/* An identical unsealed range is already pending and receives the same fence at the next seal, so it is not added
   again (one buffer for many dispatches). Caller holds g_gw_mu. */
static void gw_add(uint64_t lo, uint64_t hi) {
    int at = gw_lower(lo);
    for (int j = at; j < g_ngw && g_gw[j].lo == lo; j++) if (g_gw[j].hi == hi && !g_gw[j].fence) return;
    if (g_ngw == g_capgw) {
        g_capgw = g_capgw ? 2 * g_capgw : 1024;
        g_gw = VK_REALLOC(g_gw, (size_t)g_capgw * sizeof *g_gw);
        g_gw_maxhi = VK_REALLOC(g_gw_maxhi, (size_t)g_capgw * sizeof *g_gw_maxhi);
    }
    memmove(g_gw + at + 1, g_gw + at, (size_t)(g_ngw - at) * sizeof *g_gw);
    g_gw[at] = (gw_t){lo, hi, 0}; g_ngw++;
    gw_reindex_from(at);
}
static void gw_seal(uint64_t fence) { pthread_mutex_lock(&g_gw_mu); for (int i = 0; i < g_ngw; i++) if (!g_gw[i].fence) g_gw[i].fence = fence; pthread_mutex_unlock(&g_gw_mu); }
static void gw_prune(void) {
    uint64_t done = atomic_load(&g_done); int j = 0;
    pthread_mutex_lock(&g_gw_mu);
    for (int i = 0; i < g_ngw; i++) if (!g_gw[i].fence || g_gw[i].fence > done) g_gw[j++] = g_gw[i];
    g_ngw = j; gw_reindex_from(0);
    pthread_mutex_unlock(&g_gw_mu);
}
static void snap_drop(uint64_t a, uint64_t n);
static int ub_original_pending(uint64_t a, uint64_t n) {
    uint64_t hi = a + n;
    if (!g_nub || hi <= g_ub_lo || a >= g_ub_hi) return 0;
    uint64_t done = atomic_load(&g_done);
    /* The same sorted overlap index used by private overlays also bounds real producers. Do not
       scan every unrelated logical update for each UBO/vertex/index read after a memo collision. */
    int lo = 0, end = g_nub;
    while (lo < end) { int m = (lo + end) >> 1; if (g_ub[m].lo < hi) lo = m + 1; else end = m; }
    uint64_t floor = a > g_ub_maxlen ? a - g_ub_maxlen : 0;
    for (int i = lo - 1; i >= 0 && g_ub_maxhi[i] > a && (g_ub[i].lo > floor || !floor); i--)
        if (g_ub[i].flushed && a < g_ub[i].hi && (!g_ub[i].fence || g_ub[i].fence > done)) return 1;
    return 0;
}
/* Negative results survive actual submissions when mapping/lifetime and the relevant producer journal agree.
   A positive additionally expires whenever completion moves; sealing an unsubmitted producer does not add one. */
static int lag_original_pending(uint64_t address, uint64_t size) {
    enum { NP = 1024 };
    static struct { uint64_t address, size, revision, ticket, spans, lifetime, done; int pending; } memo[NP];
    __typeof__(memo[0]) *m = &memo[lag_hash(address, size, 0) % NP];
    uint64_t ticket = g_reserved_fence + 1, spans = atomic_load_explicit(&g_span_gen, memory_order_acquire);
    uint64_t lifetime = nvn_dirty_lag_epoch();
    if (m->address == address && m->size == size && m->spans == spans && m->lifetime == lifetime &&
        (!m->pending || m->done == atomic_load(&g_done)) && lag_producer_unchanged(m->revision, address, size)) {
        LAG_TEST_INC(VK_LAG_TEST_PRODUCER_HITS);
        if (!m->pending && m->ticket != ticket) LAG_TEST_INC(VK_LAG_TEST_PRODUCER_CROSS_TICKET);
        m->revision = g_lag_producer_revision; m->ticket = ticket;
        return m->pending;
    }
    LAG_TEST_INC(VK_LAG_TEST_PRODUCER_SCANS);
    uint64_t done = atomic_load(&g_done);
    pthread_mutex_lock(&g_gw_mu);
    int pending = gw_pending(address, address + size, done);
    pthread_mutex_unlock(&g_gw_mu);
    if (!pending) pending = ub_original_pending(address, size);
    *m = (__typeof__(*m)){address, size, g_lag_producer_revision, ticket, spans, lifetime, done, pending};
    return pending;
}
#ifdef TOTK_VK_TESTS
static int lag_test_collect_bench(void);
/* Standalone host fixture: exercise the production query against the pre-index scalar oracle,
   including private entries, completed/unsubmitted writers and exact half-open boundaries. */
static int lag_test_pending_linear(uint64_t a, uint64_t n) {
    uint64_t done = atomic_load(&g_done);
    for (int i = 0; i < g_nub; i++) if (g_ub[i].flushed && g_ub[i].lo < a + n && a < g_ub[i].hi &&
        (!g_ub[i].fence || g_ub[i].fence > done)) return 1;
    return 0;
}
static int lag_test_producer_linear(uint64_t revision, uint64_t address, uint64_t size) {
    if (revision == g_lag_producer_revision) return 1;
    if (!revision || revision > g_lag_producer_revision ||
        g_lag_producer_revision - revision > LAG_PRODUCER_CHANGES || address + size < address) return 0;
    for (uint64_t i = revision + 1; i <= g_lag_producer_revision; i++) {
        const __typeof__(*g_lag_producer_changes) *c = &g_lag_producer_changes[i % LAG_PRODUCER_CHANGES];
        if (c->revision != i || (address < c->hi && c->lo < address + size)) return 0;
    }
    return 1;
}
/* The sorted GPU-write index against the previous unsorted list and linear scan (including its last-8 duplicate
   rule), over random overlapping/duplicate writes, seals, completions and prunes; then per-query cost. */
static unsigned lag_test_gw_index(unsigned *checks) {
    enum { MAXW = 4096, OPS = 200000 };
    if (g_ngw) return 1;
    gw_t *shadow = calloc(MAXW, sizeof *shadow); int nshadow = 0;
    if (!shadow) abort();
    unsigned errors = 0; uint64_t rng = 0x9e3779b97f4a7c15u, fence = 0, done = 0;
    #define GW_RAND() (rng = rng * 6364136223846793005u + 1442695040888963407u, rng >> 33)
    atomic_store(&g_done, 0);
    for (unsigned op = 0; op < OPS; op++) {
        unsigned kind = GW_RAND() % 100;
        if (kind < 30 && nshadow < MAXW - 8) {   /* clustered starts give overlaps and exact duplicates */
            uint64_t lo = UINT64_C(0x340000000) + (GW_RAND() % 512) * 256, hi = lo + 64 + (GW_RAND() % 8) * 512;
            if (GW_RAND() % 16 == 0) hi = lo + 65536;   /* occasional long range keeps the running maximum high */
            int dup = 0;
            for (int j = nshadow - 1; j >= 0 && j >= nshadow - 8; j--) if (shadow[j].lo == lo && shadow[j].hi == hi && !shadow[j].fence) dup = 1;
            if (!dup) shadow[nshadow++] = (gw_t){lo, hi, 0};
            pthread_mutex_lock(&g_gw_mu); gw_add(lo, hi); pthread_mutex_unlock(&g_gw_mu);
        } else if (kind < 34) {
            fence++; gw_seal(fence);
            for (int j = 0; j < nshadow; j++) if (!shadow[j].fence) shadow[j].fence = fence;
        } else if (kind < 37 && done < fence) {
            done += 1 + GW_RAND() % (fence - done); atomic_store(&g_done, done);
        } else if (kind < 39) {
            gw_prune(); int k = 0;
            for (int j = 0; j < nshadow; j++) if (!shadow[j].fence || shadow[j].fence > done) shadow[k++] = shadow[j];
            nshadow = k;
        } else {
            uint64_t lo = UINT64_C(0x340000000) - 4096 + (GW_RAND() % 140000) * 1, hi = lo + 1 + GW_RAND() % 4096;
            uint64_t at = GW_RAND() % 4 ? done : (done ? done - 1 : 0);   /* captures may use an older completion */
            int oracle = 0;
            for (int j = 0; j < nshadow; j++) if (shadow[j].lo < hi && lo < shadow[j].hi && (!shadow[j].fence || shadow[j].fence > at)) { oracle = 1; break; }
            pthread_mutex_lock(&g_gw_mu); int indexed = gw_pending(lo, hi, at); pthread_mutex_unlock(&g_gw_mu);
            errors += indexed != oracle; (*checks)++;
        }
    }
    #undef GW_RAND
    /* Per-query cost at the route's size: ~100 pending writes, 1,308 capture ranges per submission. */
    for (int count = 32; count <= 512; count *= 4) {
        pthread_mutex_lock(&g_gw_mu); g_ngw = 0;
        for (int i = 0; i < count; i++) gw_add(UINT64_C(0x360000000) + (uint64_t)i * 65536 * 3, UINT64_C(0x360000000) + (uint64_t)i * 65536 * 3 + 4096);
        pthread_mutex_unlock(&g_gw_mu);
        uint64_t ns[2]; unsigned hits[2] = {0};
        for (unsigned indexed = 0; indexed < 2; indexed++) {
            uint64_t start = host_now_ns();
            for (unsigned i = 0; i < 200000; i++) {
                uint64_t lo = UINT64_C(0x360000000) + (uint64_t)(i % 4096) * 65536 + 1024;
                if (indexed) hits[1] += gw_pending(lo, lo + 256, 0);
                else { for (int j = 0; j < g_ngw; j++) if (g_gw[j].lo < lo + 256 && lo < g_gw[j].hi) { hits[0]++; break; } }
            }
            ns[indexed] = host_now_ns() - start;
        }
        errors += hits[0] != hits[1]; (*checks)++;
        printf("lag-query GPU writes %d pending: linear %.2f indexed %.2f ns/query; 6540 capture ranges/frame %.3f -> %.3f ms\n",
               count, ns[0] / 2e5, ns[1] / 2e5, ns[0] / 2e5 * 6540 / 1e6, ns[1] / 2e5 * 6540 / 1e6);
    }
    pthread_mutex_lock(&g_gw_mu); g_ngw = 0; pthread_mutex_unlock(&g_gw_mu);
    atomic_store(&g_done, 0);
    free(shadow);
    return errors;
}
static unsigned lag_test_rng_sort(unsigned *checks);
int vk_lag_test_query_bench(void) {
    if (g_nub || g_ub || g_ub_maxhi) return 1; /* run before Vulkan/runtime initialization */
    enum { CALLS = 200000 };
    unsigned checks = 0, errors = 0;
    for (int count = 256; count <= 4096; count *= 16) {
        g_nub = count; g_ub = VK_CALLOC((size_t)count, sizeof *g_ub);
        g_ub_maxhi = VK_CALLOC((size_t)count, sizeof *g_ub_maxhi);
        for (int i = 0; i < count; i++) g_ub[i] = (ub_ent){
            .lo = UINT64_C(0x330000000) + (uint64_t)i * 4096,
            .hi = UINT64_C(0x330000000) + (uint64_t)i * 4096 + (i % 7 ? 256 : 32768),
            .flushed = i % 3 == 0, .fence = i % 5 ? 101 : 0
        };
        ub_reindex();
        for (unsigned done = 100; done <= 101; done++) {
            atomic_store(&g_done, done);
            for (int i = -1; i <= count; i++) for (unsigned k = 0; k < 5; k++) {
                uint64_t a = UINT64_C(0x330000000) + (uint64_t)(i + 1) * 4096 - 4096 + k * 128;
                uint64_t n = k == 4 ? 65536 : 128;
                errors += ub_original_pending(a, n) != lag_test_pending_linear(a, n); checks++;
            }
        }
        atomic_store(&g_done, 100);
        uint64_t ns[2]; unsigned hits[2] = {0};
        for (unsigned indexed = 0; indexed < 2; indexed++) {
            uint64_t start = host_now_ns();
            for (unsigned i = 0; i < CALLS; i++) {
                uint64_t a = UINT64_C(0x330000000) + (uint64_t)((i * 509u) % (unsigned)count) * 4096 + 512;
                hits[indexed] += indexed ? ub_original_pending(a, 256) : lag_test_pending_linear(a, 256);
            }
            ns[indexed] = host_now_ns() - start;
        }
        errors += hits[0] != hits[1]; checks++;
        printf("lag-query pending %d entries: scalar %.2f indexed %.2f ns/query; 20000 queries/frame %.3f -> %.3f ms; hits %u/%u\n",
            count, ns[0] / (double)CALLS, ns[1] / (double)CALLS,
            ns[0] / (double)CALLS * .02, ns[1] / (double)CALLS * .02, hits[0], hits[1]);
        free(g_ub); free(g_ub_maxhi); g_ub = NULL; g_ub_maxhi = NULL; g_nub = 0;
    }
    setenv("TOTK_FRAME_LAG", "1", 1);
    for (unsigned gap = 64; gap <= LAG_PRODUCER_CHANGES; gap *= 8) {
        uint64_t revision = g_lag_producer_revision;
        for (unsigned i = 0; i < gap; i++)
            lag_producer_changed(UINT64_C(0x330000000) + (uint64_t)(i % 32) * 65536, 512);
        for (unsigned i = 0; i < 128; i++) for (unsigned k = 0; k < 4; k++) {
            uint64_t address = UINT64_C(0x330000000) + (uint64_t)i * 32768 + k * 256;
            uint64_t size = k == 3 ? UINT64_C(0x400001) : 256;
            errors += lag_producer_unchanged(revision, address, size) != lag_test_producer_linear(revision, address, size); checks++;
        }
        uint64_t ns[2]; unsigned hits[2] = {0};
        for (unsigned accelerated = 0; accelerated < 2; accelerated++) {
            uint64_t start = host_now_ns();
            for (unsigned i = 0; i < CALLS; i++) {
                uint64_t address = UINT64_C(0x350000000) + (uint64_t)(i % 1024) * 65536;
                hits[accelerated] += accelerated ? lag_producer_unchanged(revision, address, 256) :
                    lag_test_producer_linear(revision, address, 256);
            }
            ns[accelerated] = host_now_ns() - start;
        }
        errors += hits[0] != hits[1]; checks++;
        printf("lag-query journal %u unrelated writes: scalar %.2f spatial %.2f ns/query; 20000 queries/frame %.3f -> %.3f ms\n",
            gap, ns[0] / (double)CALLS, ns[1] / (double)CALLS,
            ns[0] / (double)CALLS * .02, ns[1] / (double)CALLS * .02);
    }
    uint64_t old = g_lag_producer_revision, target = UINT64_C(0x390000000);
    for (unsigned i = 0; i <= LAG_PRODUCER_CHANGES; i++) lag_producer_changed(UINT64_C(0x330000000), 512);
    errors += !lag_producer_unchanged(old, target, 256); checks++; /* independent proof survives exact history loss */
    lag_producer_changed(target + 128, 1);
    errors += lag_producer_unchanged(old, target, 256); checks++;
    old = g_lag_producer_revision;
    lag_producer_changed(target + 65535, 2);
    errors += lag_producer_unchanged(old, target + 65536, 1); checks++; /* both boundary buckets updated */
    old = g_lag_producer_revision;
    lag_producer_changed(target, UINT64_C(0x500000));
    errors += lag_producer_unchanged(old, target + UINT64_C(0x400000), 1); checks++;
    old = g_lag_producer_revision;
    lag_producer_changed(UINT64_MAX - 32, 64);
    errors += lag_producer_unchanged(old, UINT64_MAX - 16, 8); checks++;
    errors += lag_producer_unchanged(old, UINT64_MAX - 16, 32); checks++;
    errors += lag_test_owner_bench(); checks++;
    errors += lag_test_collect_bench(); checks++;
    errors += lag_test_gw_index(&checks);
    errors += lag_test_rng_sort(&checks);
    printf("%s: lag-query %u checks, %u failed\n", errors ? "FAILED" : "ok", checks, errors);
    return errors != 0;
}
#endif
static void gpu_write_inner(uint64_t a, uint64_t n, int guard) {
    if (!n) return;
    if (__builtin_expect(g_pt_on, 0)) pt_gpu_wrote(a, n);   /* oot-a122: GPU-produced bytes are never frozen */
    if (__builtin_expect(g_gwl_on != 0, 0) && gwl_active()) gwl_note(a, n, guard ? "write" : "write(rec)");
    if (__builtin_expect(g_ic_k, 0) && ic_gw_window()) ic_gpu_wrote(a, n, VK_INFLIGHT_GW_PLAIN);   /* the in-flight check tells these changes from the game's (frames near a sampled one) */
    vk_wb_gpu_wrote(a, n);
    int replay=vk_fg_enabled();
    if (ub_private_on() || replay) vk_ub_touch(a, n);
    uint64_t physical = ub_address(a, n);
    if (ub_private_on() || replay) ub_forget(physical, n);
    snap_drop(physical, n);
    if (vk_lag_capture_on() || replay) ub_changed(physical, n);
    lag_producer_changed(physical, n);
    /* the same range again in this submission (every material draw binds the per-tile flag buffer it writes):
       its snapshot copies are already dropped and its range already logged */
    { enum { NG = 256 }; static struct { uint64_t a, n, sub; int guarded; } seen[NG]; uint64_t sub = vk_submit_serial(); unsigned h = (unsigned)((a >> 6) ^ (a >> 20) ^ n) % NG;
      if (seen[h].a == a && seen[h].n == n && seen[h].sub == sub) {
          if (guard && !seen[h].guarded) { lag_guard_mark(a, n, NVN_DIRTY_LAG_WRITABLE | NVN_DIRTY_LAG_GPU); seen[h].guarded = 1; }
          return;
      }
      seen[h].a = a; seen[h].n = n; seen[h].sub = sub; seen[h].guarded = guard; }
    if (guard) lag_guard_mark(a, n, NVN_DIRTY_LAG_WRITABLE | NVN_DIRTY_LAG_GPU);
    a = physical;
    if (!snap_on() && !vk_lag_capture_on() && !replay) return;
    pthread_mutex_lock(&g_gw_mu);
    gw_add(a, a + n);
    pthread_mutex_unlock(&g_gw_mu);
}
void vk_gpu_write(uint64_t a, uint64_t n) { VK_REC_DO(VK_REC_S_WRITE, gpu_write_inner(a, n, 1)); }
void vk_gpu_write_record(uint64_t a, uint64_t n) { VK_REC_DO(VK_REC_S_WRITE, gpu_write_inner(a, n, 0)); }
typedef struct { uint64_t lo, hi; } rng_t;
enum { UBO_SEEN = 1024 }; /* shared across recursive command lists, allocated once per capture */
static int rng_cmp(const void *x, const void *y) { const rng_t *a = x, *b = y; return a->lo < b->lo ? -1 : a->lo > b->lo; }
/* Stable LSD radix sort by lo, 11-bit digits, only over digits whose bits differ between keys (guest addresses share
   their high bits). qsort's indirect comparator was ~0.36 ms/frame of the Thor walker for ~4,500 ranges per capture.
   Distinct lo come out in qsort's order; equal lo keep input order, which the capture merge does not depend on.
   Scratch is per call: asynchronous capture can run on a game thread while the walker captures. */
static void rng_sort(rng_t *r, int n) {
    if (n < 2) return;
    if (n <= 32) {
        for (int i = 1; i < n; i++) { rng_t x = r[i]; int j = i; for (; j > 0 && r[j - 1].lo > x.lo; j--) r[j] = r[j - 1]; r[j] = x; }
        return;
    }
    uint64_t any = 0, all = UINT64_MAX;
    for (int i = 0; i < n; i++) { any |= r[i].lo; all &= r[i].lo; }
    uint64_t vary = any ^ all;
    rng_t *tmp = VK_MALLOC((size_t)n * sizeof *tmp), *src = r, *dst = tmp;
    unsigned count[2048];
    for (unsigned shift = 0; shift < 64; shift += 11) {
        if (!((vary >> shift) & 2047)) continue;
        memset(count, 0, sizeof count);
        for (int i = 0; i < n; i++) count[(src[i].lo >> shift) & 2047]++;
        unsigned sum = 0;
        for (unsigned b = 0; b < 2048; b++) { unsigned c = count[b]; count[b] = sum; sum += c; }
        for (int i = 0; i < n; i++) dst[count[(src[i].lo >> shift) & 2047]++] = src[i];
        rng_t *swap = src; src = dst; dst = swap;
    }
    if (src != r) memcpy(r, src, (size_t)n * sizeof *r);
    free(tmp);
}
#ifdef TOTK_VK_TESTS
/* rng_sort against qsort: identical key order, and a permutation of the input (hi travels with its lo). */
static unsigned lag_test_rng_sort(unsigned *checks) {
    unsigned errors = 0; uint64_t rng = 0x243f6a8885a308d3u;
    rng_t *a = malloc(9000 * sizeof *a), *b = malloc(9000 * sizeof *b);
    if (!a || !b) abort();
    static const int sizes[] = {0, 1, 2, 31, 32, 33, 100, 1308, 4500, 9000};
    for (unsigned s = 0; s < sizeof sizes / sizeof *sizes; s++) for (unsigned shape = 0; shape < 4; shape++) {
        int n = sizes[s];
        for (int i = 0; i < n; i++) {
            rng = rng * 6364136223846793005u + 1442695040888963407u;
            uint64_t x = rng >> 20;
            uint64_t lo = shape == 0 ? UINT64_C(0x330000000) + (x % 65536) * 256        /* clustered guest UBOs */
                        : shape == 1 ? UINT64_C(0x330000000) + (x % 16) * 64            /* many equal keys */
                        : shape == 2 ? x << 20                                          /* only high bits vary */
                        : rng;                                                          /* all bits vary */
            a[i] = b[i] = (rng_t){lo, lo + 1 + (x & 0xfff)};
        }
        rng_sort(a, n); if (n) qsort(b, (size_t)n, sizeof *b, rng_cmp);
        uint64_t sum_a = 0, sum_b = 0;
        for (int i = 0; i < n; i++) {
            errors += a[i].lo != b[i].lo; sum_a += a[i].hi * 31 + a[i].lo; sum_b += b[i].hi * 31 + b[i].lo;
            if (i) errors += a[i - 1].lo > a[i].lo;
        }
        errors += sum_a != sum_b; (*checks)++;
    }
    free(a); free(b);
    return errors;
}
#endif
static void collect_ubos(rng_t **r, int *n, int *cap, const uint8_t *p, int depth, rng_t seen[UBO_SEEN]) {
    for (int guard = 0; guard < 1000000; guard++) {
        op_hdr h; memcpy(&h, p, sizeof h);
        if (h.size < 8 || (h.size & 7) || h.op >= OP_COUNT_) return;
        if (h.op == OP_END) return;
        if (h.op == OP_JUMP) { memcpy(&p, p + 8, 8); continue; }
        if (h.op == OP_CALL) { const uint8_t *t; memcpy(&t, p + 8, 8); if (depth < 16) collect_ubos(r, n, cap, t, depth + 1, seen); }
        else if (h.op == OP_BIND_UBO) {
            const p_bind_buf *b = (const p_bind_buf *)(p + 8);
            if (b->addr && b->size && b->size <= 65536) {
                uint64_t address = ub_address(b->addr, b->size);
                unsigned slot = lag_hash(address, b->size, 0) % UBO_SEEN;
                if (vk_rec_profile_on && T_rec.active) g_rec.snapshot_bindings++;
                if (seen[slot].lo == address && seen[slot].hi == address + b->size) { p += h.size; continue; }
                seen[slot] = (rng_t){address, address + b->size};
                if (vk_rec_profile_on && T_rec.active) g_rec.snapshot_unique++;
                if (*n == *cap) { *cap = *cap ? 2 * *cap : 4096; *r = VK_REALLOC(*r, (size_t)*cap * sizeof **r); }
                (*r)[(*n)++] = (rng_t){address, address + b->size};
            }
        }
        p += h.size;
    }
}
#ifdef TOTK_VK_TESTS
static int lag_test_collect_bench(void) {
    enum { BINDS = 2048, DISTINCT = 16, REPEATS = 16 };
    struct command { op_hdr h; p_bind_buf b; } *stream = calloc(BINDS + 1, sizeof *stream);
    if (!stream) abort();
    for (int i = 0; i < BINDS; i++) stream[i] = (struct command){
        {OP_BIND_UBO, sizeof *stream}, {i & 1, 0, UINT64_C(0x100000000) + (uint64_t)(i % DISTINCT) * 65536, 256}};
    stream[BINDS].h = (op_hdr){OP_END, sizeof *stream};
    rng_t *r = malloc(4096 * sizeof *r), seen[UBO_SEEN]; int capacity = 4096;
    if (!r) abort();
    uint64_t ns[2][3]; unsigned samples[2] = {0}, errors = 0; int candidates[2] = {0};
    for (unsigned pass = 0; pass < 6; pass++) {
        unsigned mode = pass == 1 || pass == 2 || pass == 5;
        int n = 0; uint64_t start = host_now_ns();
        for (unsigned repeat = 0; repeat < REPEATS; repeat++) {
            n = 0; memset(seen, 0, (mode ? UBO_SEEN : 64) * sizeof *seen);
            if (mode) collect_ubos(&r, &n, &capacity, (const uint8_t *)stream, 0, seen);
            else for (int i = 0; i < BINDS; i++) {
                uint64_t a = ub_address(stream[i].b.addr, stream[i].b.size);
                unsigned slot = (unsigned)((a >> 8) ^ (a >> 20) ^ stream[i].b.size) & 63;
                if (seen[slot].lo == a && seen[slot].hi == a + stream[i].b.size) continue;
                r[n++] = seen[slot] = (rng_t){a, a + stream[i].b.size};
            }
            qsort(r, (size_t)n, sizeof *r, rng_cmp);
        }
        ns[mode][samples[mode]++] = host_now_ns() - start; candidates[mode] = n;
        int distinct = 0;
        for (int i = 0; i < n; i++) {
            errors += r[i].hi != r[i].lo + 256;
            if (i && r[i].lo == r[i - 1].lo) continue;
            errors += r[i].lo != UINT64_C(0x100000000) + (uint64_t)distinct * 65536; distinct++;
        }
        errors += distinct != DISTINCT;
    }
    for (unsigned mode = 0; mode < 2; mode++)
        for (unsigned i = 1; i < 3; i++) for (unsigned j = i; j && ns[mode][j] < ns[mode][j - 1]; j--) {
            uint64_t swap = ns[mode][j]; ns[mode][j] = ns[mode][j - 1]; ns[mode][j - 1] = swap;
        }
    errors += candidates[1] >= candidates[0];
    printf("lag-query UBO collect/sort: %d repeated binds, %d distinct; candidates %d -> %d; "
           "old %.2f compact %.2f ns/bind (interleaved medians); identical canonical extents\n",
           BINDS, DISTINCT, candidates[0], candidates[1],
           ns[0][1] / (double)(BINDS * REPEATS), ns[1][1] / (double)(BINDS * REPEATS));
    free(r); free(stream); return errors != 0;
}
#endif
static ucap_t *ucap_build(const uint64_t *h, int n, int copy_bytes) {
    ucap_t *c = VK_CALLOC(1, sizeof *c);
    c->done = atomic_load(&g_done);   /* before the bytes: a write completing meanwhile only makes this conservative */
    rng_t *r = NULL, seen[UBO_SEEN] = {0}; int nr = 0, cap = 0;
    for (int i = 0; i < n; i++) collect_ubos(&r, &nr, &cap, (const uint8_t *)(uintptr_t)h[i], 0, seen);
    if (nr) {
        rng_sort(r, nr);
        int k = -1;   /* merge overlapping / touching ranges inside one pool; drop ranges outside every pool */
        for (int i = 0; i < nr; i++) {
            VkBuffer b; uint64_t o;
            if (k >= 0 && r[i].lo <= r[k].hi) { if (r[i].hi <= r[k].hi) continue; if (vk_resolve(r[k].lo, r[i].hi - r[k].lo, &b, &o)) { r[k].hi = r[i].hi; continue; } }
            if (!vk_resolve(r[i].lo, r[i].hi - r[i].lo, &b, &o)) continue;
            if (k >= 0 && r[i].lo < r[k].hi) r[i].lo = r[k].hi;   /* keep the copies disjoint */
            r[++k] = r[i];
        }
        /* ranges a GPU write still in flight overlaps are left to memory (the GPU timeline) */
        pthread_mutex_lock(&g_gw_mu);
        int m = 0; for (int i = 0; i <= k; i++) {
            if ((g_ngw && gw_pending(r[i].lo, r[i].hi, c->done)) ||
                (ub_private_on() && ub_original_pending(r[i].lo, r[i].hi - r[i].lo))) {
                g_stat_snap_skipped++; continue;
            }
            r[m++] = r[i];
        }
        pthread_mutex_unlock(&g_gw_mu);
        k = m - 1;
        c->n = k + 1;
        c->lo = VK_MALLOC(sizeof *c->lo * (size_t)(c->n + 1)); c->hi = VK_MALLOC(sizeof *c->hi * (size_t)(c->n + 1));
        if (copy_bytes) c->off = VK_MALLOC(sizeof *c->off * (size_t)(c->n + 1));
        size_t total = 0; for (int i = 0; i < c->n; i++) {
            c->lo[i] = r[i].lo; c->hi[i] = r[i].hi;
            if (copy_bytes) { c->off[i] = total; total += (size_t)(r[i].hi - r[i].lo); }
        }
        if (copy_bytes) {
            c->bytes = VK_MALLOC(total ? total : 1);
            for (int i = 0; i < c->n; i++) memcpy(c->bytes + c->off[i], (const void *)(uintptr_t)c->lo[i], (size_t)(c->hi[i] - c->lo[i]));
        }
    }
    free(r);
    return c;
}
static void ucap_free(ucap_t *c) { if (c) { free(c->lo); free(c->hi); free(c->off); free(c->bytes); free(c); } }
void *mtl_sub_capture_ubos(const uint64_t *h, int n) { return snap_on() ? ucap_build(h, n, 1) : NULL; } /* async: eagerly own the bytes */
static int snap_first(uint64_t a) { int lo = 0, hi = g_nsnap; while (lo < hi) { int m = (lo + hi) >> 1; if (g_snap[m].hi <= a) lo = m + 1; else hi = m; } return lo; }
static const snap_t *snap_find(uint64_t a, uint64_t n) {   /* the live copy holding all of [a, a+n) */
    int i = snap_first(a);
    return i < g_nsnap && !g_snap[i].dead && g_snap[i].lo <= a && a + n <= g_snap[i].hi ? &g_snap[i] : NULL;
}
/* A snapshot already contains all updates logged at its creation. Only a later overlapping update can make a binding
   need an overlay: mark once at the update instead of searching the whole log at every binding of untouched snapshots.
   The flag stays conservative after chunk submissions seal / prune those updates. */
static void snap_note_update(uint64_t lo, uint64_t n) {
    for (int i = snap_first(lo); i < g_nsnap && g_snap[i].lo < lo + n; i++) g_snap[i].updated = 1;
}
/* updated is only a cheap rejection hint. A merged snapshot can contain thousands of untouched bindings after
   one tiny update. The retained ordered log proves the requested subrange, including partial overlays; the
   snapshot prune floor keeps every post-base update available until that immutable incarnation is reset. */
static int snap_range_updated(const snap_t *sn, uint64_t address, uint64_t size) {
    uint64_t end = address + size;
    if (!sn->updated || !g_nub || end <= g_ub_lo || address >= g_ub_hi) return 0;
    int a = 0, b = g_nub;
    while (a < b) { int m = (a + b) >> 1; if (g_ub[m].lo < end) a = m + 1; else b = m; }
    uint64_t floor = address > g_ub_maxlen ? address - g_ub_maxlen : 0;
    for (int i = a - 1; i >= 0 && g_ub_maxhi[i] > address && (g_ub[i].lo > floor || !floor); i--)
        if (g_ub[i].hi > address && g_ub[i].seq > g_snap_seq) return 1;
    return 0;
}
static void snap_drop(uint64_t a, uint64_t n) {   /* a GPU / CPU-step write into [a, a+n): those copies no longer hold the truth */
    for (int i = snap_first(a); i < g_nsnap && g_snap[i].lo < a + n; i++) if (!g_snap[i].dead) {
        g_snap[i].dead = 1; ub_changed(g_snap[i].lo, g_snap[i].hi - g_snap[i].lo);
    }
}
/* The copies are bound for the whole game submission, which chunked submission (vk_submit_chunk) spreads over several ring
   slots, and the ring can wrap within one: a slot's transient memory would be reused while later parts still read it. So
   they live in an arena of their own, reused once the submission's last part has finished (one queue: all parts have). */
/* 4 with the ring of 8. A submission's replay cannot start before the submission that used its arena last has finished
   on the GPU, so with 4 arenas the walker was never more than 3 game submissions ahead, whatever TOTK_VK_RING said
   (oot-a103): with a longer ring there are as many arenas as slots (the 1u << arena masks hold 32). */
#define SNAP_ARENAS_MAX 32
unsigned vk_snap_arenas(void) { unsigned r = RING; return r <= 8 ? 4 : r > SNAP_ARENAS_MAX ? SNAP_ARENAS_MAX : r; }
#define SNAP_ARENAS vk_snap_arenas()
static struct { arena_t ar; uint64_t release; } g_sar[SNAP_ARENAS_MAX];
static uint64_t g_sar_replay_until[SNAP_ARENAS_MAX]; /* external split sidecar */
static int g_sar_cur = -1, g_sar_next;
static unsigned snapshot_replay_mask(void) {
    unsigned mask=0;
    for (unsigned i=0;i<SNAP_ARENAS;i++) if (g_sar[i].ar.nchunk) mask|=1u<<i;
    return mask;
}
static int arena_frozen_copy(arena_t *arena,VkBuffer buffer,VkDeviceSize offset,
        VkDeviceSize bytes,void *destination) {
    for (int i=0;i<arena->nchunk;i++) {
        const chunk_t *chunk=&arena->chunks[i];
        if (chunk->buf!=buffer || offset>chunk->used || bytes>chunk->used-offset) continue;
        memcpy(destination,chunk->ptr+offset,(size_t)bytes);
        return 1;
    }
    return 0;
}
int vk_replay_frozen_buffer_copy(VkBuffer buffer,VkDeviceSize offset,VkDeviceSize bytes,
        void *destination) {
    if (!buffer || !destination || !bytes || offset>UINT64_MAX-bytes || bytes>SIZE_MAX) return 0;
    for (unsigned i=0;i<RING;i++)
        if (arena_frozen_copy(&g_ring[i].ar,buffer,offset,bytes,destination)) return 1;
    for (unsigned i=0;i<SNAP_ARENAS;i++)
        if (arena_frozen_copy(&g_sar[i].ar,buffer,offset,bytes,destination)) return 1;
    return 0;
}
static uint64_t snapshot_owner_until(unsigned arena) {
    uint64_t until=0;
    if (!atomic_load_explicit(&g_replay_pins,memory_order_relaxed)) return 0;
    pthread_mutex_lock(&g_mu);
    for (unsigned i=0;i<RING;i++)
        if ((g_replay_owner[i].snapshots & (1u<<arena)) && g_replay_owner[i].until>until)
            until=g_replay_owner[i].until;
    pthread_mutex_unlock(&g_mu);
    return until;
}
static void snapshot_replay_pin(uint64_t until) {
    for (unsigned i=0;i<SNAP_ARENAS;i++) {
        if (until==UINT64_MAX) {
            if (g_sar[i].ar.nchunk) g_sar_replay_until[i]=until;
        } else if (g_sar_replay_until[i]==UINT64_MAX) g_sar_replay_until[i]=until;
    }
}
static arena_t *snap_arena(void) {   /* the current submission's arena (one more replay start in it appends) */
    if (g_sar_cur < 0) {
        int i = g_sar_next; g_sar_next = (i + 1) % SNAP_ARENAS;
        uint64_t release=g_sar[i].release, owner=snapshot_owner_until((unsigned)i);
        if (owner>release) release=owner;
        if (atomic_load_explicit(&g_external_enabled,memory_order_acquire) && g_sar_replay_until[i]>release)
            release=g_sar_replay_until[i];
        if (release==UINT64_MAX &&
                __builtin_expect(atomic_load_explicit(&vk_fg_generation_snapshot_retained,memory_order_relaxed),0)) {
            /* This recorder alone can resolve open/pending replay leases.
             * Cancel optional templates, never submitted/quarantined work.
             * Earlier cancellation may already have emptied every template;
             * quarantine may outlive the presenter queue/gate altogether. */
            unsigned cancelled=vk_fg_generation_cancel_prepared(),states=0;
            pthread_mutex_lock(&g_mu);
            for (unsigned n=0;n<RING;n++)
                if ((g_replay_owner[n].snapshots&(1u<<i)) && g_replay_owner[n].until==UINT64_MAX)
                    states|=g_replay_owner[n].pending?1u:2u;
            pthread_mutex_unlock(&g_mu);
            owner=snapshot_owner_until((unsigned)i);
            release=g_sar[i].release;
            if (owner>release) release=owner;
            if (atomic_load_explicit(&g_external_enabled,memory_order_acquire) && g_sar_replay_until[i]>release)
                release=g_sar_replay_until[i];
            vk_fg_snapshot_pressure_trace(states,vk_fg_generation_state_mask(),cancelled,
                release==UINT64_MAX);
            static int reported;
            if (!reported) {
                reported=1;
                LOGV("[adaptive-slot-dispatch] event=snapshot-open-owner wait-ns=%llu arena=%d owner-state-mask=%u generation-state-mask=%u cancelled=%u after-ns=%llu; pending=1 open=2 append-not-recycle-if-still-open",
                    (unsigned long long)UINT64_MAX,i,states,vk_fg_generation_state_mask(),
                    cancelled,(unsigned long long)release);
            }
            if (release==UINT64_MAX) {
                /* Do not split from descriptor capture: its caller can cache
                 * that primary. Append without reset/compaction until normal
                 * sealing resolves pending release. Quarantine keeps immutable
                 * prefixes indefinitely, even after scheduler disable. */
                g_sar_cur=i;
                return &g_sar[i].ar;
            }
        }
        g_wait_kind = 1; wait_value(release); g_wait_kind = 0; g_sar_replay_until[i]=0;
        g_sar[i].ar.min = 1u << 20;
        arena_reset(&g_sar[i].ar, 2);
#ifdef TOTK_OHOS
        if (snapshot_retain_cap()) {   /* per arena; with more than 4 arenas (TOTK_VK_RING > 8) the same total, not below one 1 MiB chunk */
            VkDeviceSize cap = snapshot_retain_cap() * 4 / SNAP_ARENAS;
            arena_cap_bytes(&g_sar[i].ar, cap < (1u << 20) ? (1u << 20) : cap, VK_MEMORY_SNAPSHOT);
        }
#endif
        g_sar_cur = i;
    }
    return &g_sar[g_sar_cur].ar;
}
#ifdef TOTK_VK_TESTS
uint64_t vk_record_test_snapshot_next_owner(void) {
    return snapshot_owner_until((unsigned)g_sar_next);
}
void vk_record_test_snapshot_allocate_range(uint32_t value,VkBuffer *buffer,VkDeviceSize *offset) {
    uint32_t *bytes=arena_alloc(snap_arena(),64,16,buffer,offset);
    for (unsigned i=0;i<16;i++) bytes[i]=value+i;
}
unsigned vk_record_test_snapshot_mask(void) { return snapshot_replay_mask(); }
uint64_t vk_record_test_snapshot_external_until(void) {
    uint64_t until=0;
    for (unsigned i=0;i<SNAP_ARENAS;i++)
        if (g_sar_replay_until[i]>until) until=g_sar_replay_until[i];
    return until;
}
unsigned vk_record_test_snapshot_next_index(void) { return (unsigned)g_sar_next; }
uint32_t *vk_record_test_snapshot_allocate(uint32_t value) {
    VkBuffer buffer; VkDeviceSize offset;
    uint32_t *bytes=arena_alloc(snap_arena(),64,16,&buffer,&offset);
    for (unsigned i=0;i<16;i++) bytes[i]=value+i;
    return bytes;
}
#endif
static void snapshot_ubos_inner(const uint64_t *h, int n, void *capv) {   /* the start of a replay */
    ucap_t *cap = capv;
    snap_reset();
    if (!snap_on()) { ucap_free(cap); return; }
    if (!cap) {
        /* Avoid a staging copy only if acquiring the replay-owned snapshot arena cannot wait for the GPU.
           Async capture and an unavailable arena retain eager capture before any wait. */
        int direct = vk_record_enabled() && (g_sar_cur >= 0 || atomic_load(&g_done) >= g_sar[g_sar_next].release);
        if (direct && g_sar_cur<0 && atomic_load_explicit(&g_external_enabled,memory_order_acquire))
            direct=atomic_load(&g_done)>=g_sar_replay_until[g_sar_next];
        if (direct && g_sar_cur<0)
            direct=atomic_load(&g_done)>=snapshot_owner_until((unsigned)g_sar_next);
        cap = ucap_build(h, n, !direct);
    }
    g_prune_floor = cap->done;
    if (cap->n > g_capsnap) { g_capsnap = cap->n + 256; g_snap = VK_REALLOC(g_snap, (size_t)g_capsnap * sizeof *g_snap); }
    VkDeviceSize align = V.props.limits.minUniformBufferOffsetAlignment; if (!align) align = 256;
    static int *idx; static int icap;
    for (int i = 0; i < cap->n; i++) {
        uint64_t lo = cap->lo[i], hi = cap->hi[i], sz = hi - lo, shift = lo % align;
        VkBuffer b; VkDeviceSize o; uint8_t *p = arena_alloc(snap_arena(), (size_t)(sz + shift), align, &b, &o);
        if (vk_lag_capture_on()) lag_snapshot_allocation(VK_LAG_UNIFORM, sz + shift);
        else ft_snapshot_allocation(sz + shift);
        p += shift; o += shift;   /* the copy keeps the guest address's offset within the alignment: aligned bindings stay aligned */
        memcpy(p, cap->bytes ? cap->bytes + cap->off[i] : (const void *)(uintptr_t)lo, (size_t)sz);
        if (vk_rec_profile_on && T_rec.active) {
            if (cap->bytes) g_rec.snapshot_staged += sz; else g_rec.snapshot_direct += sz;
        }
        /* inline updates of earlier submissions that were not in memory at the capture */
        int m; for (;;) { m = ub_overlaps(lo, hi, 0, idx, icap); if (m >= 0) break; icap = icap ? 2 * icap : 64; idx = VK_REALLOC(idx, (size_t)icap * sizeof *idx); }
        for (int k = 0; k < m; k++) {
            const ub_ent *e = &g_ub[idx[k]];
            if (e->fence && e->fence <= cap->done) continue;
            uint64_t a = e->lo > lo ? e->lo : lo, z = e->hi < hi ? e->hi : hi;
            memcpy(p + (a - lo), e->data + (a - e->lo), (size_t)(z - a));
        }
        g_snap[g_nsnap++] = (snap_t){lo, hi, b, o, p, 0, 0};
        g_stat_snap_bytes += sz;
    }
    g_stat_snap_ranges += (uint64_t)g_nsnap;
    ucap_free(cap);
}
void mtl_sub_snapshot_ubos_cap(const uint64_t *h, int n, void *capv) { VK_REC_DO(VK_REC_S_SNAPSHOT, snapshot_ubos_inner(h, n, capv)); }
static int ub_rename_slow(uint64_t addr, uint64_t size, const snap_t *sn, VkDescriptorBufferInfo *out);
int vk_ub_rename(uint64_t addr, uint64_t size, VkDescriptorBufferInfo *out) {
    if (!ub_rename_on()) return 0;   /* ordinary renaming stays opt-in; protected UBOs use vk_lag_uniform_read */
    const snap_t *sn = g_nsnap ? snap_find(addr, size) : NULL;
    /* Memo (TOTK, 2026-10-22: the overlap search was ~10% of the CPU, for ~15k uniform bindings a frame against ~100
       logged updates): with the same range, snapshot and submission and no change to the log since, the answer is the
       same as last time. */
    enum { NM = 1024 };
    static struct { uint64_t addr, size, gen, sub; const void *sn; int ret; VkDescriptorBufferInfo d; } memo[NM];
    uint64_t msub = vk_submit_serial(); unsigned mh = (unsigned)((addr >> 4) ^ (addr >> 14) ^ (size * 2654435761u)) % NM;
    if (memo[mh].addr == addr && memo[mh].size == size && memo[mh].gen == g_ub_gen && memo[mh].sub == msub && memo[mh].sn == (const void *)sn) { if (memo[mh].ret) *out = memo[mh].d; return memo[mh].ret; }
    int ret = ub_rename_slow(addr, size, sn, out);
    memo[mh].addr = addr; memo[mh].size = size; memo[mh].gen = g_ub_gen; memo[mh].sub = msub; memo[mh].sn = sn; memo[mh].ret = ret; if (ret) memo[mh].d = *out;
    return ret;
}
static int ub_rename_slow(uint64_t addr, uint64_t size, const snap_t *sn, VkDescriptorBufferInfo *out) {
    VkDeviceSize align = V.props.limits.minUniformBufferOffsetAlignment; if (!align) align = 256;
    if (sn && !snap_range_updated(sn, addr, size)) {
        VkDeviceSize o = sn->off + (addr - sn->lo);
        if (o % align == 0) { *out = (VkDescriptorBufferInfo){sn->buf, o, size}; g_stat_snap_binds++; return 1; }
    }
    int n = 0;
    static int *idx; static int cap;
    if (g_nub && addr + size > g_ub_lo && addr < g_ub_hi) {
        { static uint64_t last_done = UINT64_MAX; uint64_t d = atomic_load(&g_done); if (d != last_done) { last_done = d; ub_prune(); } }   /* prune only when the GPU moved on */
        for (;;) { n = ub_overlaps(addr, addr + size, 0, idx, cap); if (n >= 0) break; cap = cap ? 2 * cap : 64; idx = VK_REALLOC(idx, (size_t)cap * sizeof *idx); }
        /* A chunk split seals updates from this game submission too: fence != 0 does not mean the snapshot has them.
           Keep every update newer than its base, even after it has reached an earlier Vulkan submission. */
        if (sn) { int k = 0; for (int i = 0; i < n; i++) if (g_ub[idx[i]].seq > g_snap_seq) idx[k++] = idx[i]; n = k; }
    }
    if (!n) {
        if (!sn) return 0;
        VkDeviceSize o = sn->off + (addr - sn->lo);
        if (o % align == 0) { *out = (VkDescriptorBufferInfo){sn->buf, o, size}; g_stat_snap_binds++; return 1; }
    }
    /* the same range with the same newest update in this submission: the copy made for it is still right */
    enum { NC = 256 };
    static struct { uint64_t addr, size, seq, sub; const void *base; VkDescriptorBufferInfo d; } cache[NC];
    uint64_t newest = n ? g_ub[idx[n - 1]].seq : 0, sub = vk_submit_serial();
    unsigned h = (unsigned)((addr >> 4) ^ (addr >> 16) ^ size) % NC;
    if (cache[h].addr == addr && cache[h].size == size && cache[h].seq == newest && cache[h].sub == sub && cache[h].base == (const void *)sn) { *out = cache[h].d; return 1; }
    VkBuffer tb; VkDeviceSize to;
    uint8_t *p = vk_transient(size, align, &tb, &to);
    ft_snapshot_allocation(size);
    memcpy(p, sn ? (const void *)(sn->cpu + (addr - sn->lo)) : (const void *)(uintptr_t)addr, (size_t)size);
    for (int k = 0; k < n; k++) {
        const ub_ent *e = &g_ub[idx[k]];
        uint64_t lo = e->lo > addr ? e->lo : addr, hi = e->hi < addr + size ? e->hi : addr + size;
        memcpy(p + (lo - addr), e->data + (lo - e->lo), (size_t)(hi - lo));
    }
    *out = (VkDescriptorBufferInfo){tb, to, size};
    cache[h].addr = addr; cache[h].size = size; cache[h].seq = newest; cache[h].sub = sub; cache[h].base = sn; cache[h].d = *out;
    g_stat_ub_renamed++;
    return 1;
}
/* A covering submission snapshot proves the range exists: avoid re-resolving its guest pool on every descriptor.
   Only an overlapping post-base update requires the ordinary ordered overlay/rename memo. */
int vk_ub_snapshot_descriptor(uint64_t addr, uint64_t size, VkDescriptorBufferInfo *out) {
    const snap_t *sn = g_nsnap ? snap_find(addr, size) : NULL; if (!sn) return 0;
    VkDeviceSize offset = sn->off + addr - sn->lo, align = V.props.limits.minUniformBufferOffsetAlignment;
    if (!align) align = 256;
    if (!snap_range_updated(sn, addr, size) && !(offset % align)) { *out = (VkDescriptorBufferInfo){sn->buf, offset, size}; g_stat_snap_binds++; return 1; }
    return vk_ub_rename(addr, size, out);
}
/* The bytes a uniform binding at this point of the submission reads (the same base and overlay as vk_ub_rename's copy).
   For values the backend itself takes from a uniform block: a storage buffer's address (RB_SBUF_UBO) set by an inline
   update reaches memory only when the GPU runs the update, so memory still held an earlier draw's address (nvn_mtl.m
   resolves the same pointer with pend_read). */
static void ub_read_from(uint64_t addr, void *dst, size_t size, const snap_t *sn) {
    if (sn) {
        memcpy(dst, sn->cpu + (addr - sn->lo), size);
        if (!snap_range_updated(sn, addr, size)) return;
    }
    if (!g_nub || addr + size <= g_ub_lo || addr >= g_ub_hi) {
        if (!sn) memcpy(dst, (const void *)(uintptr_t)addr, size);
        return;
    }
    static int *idx; static int cap; int n;
    ub_prune();
    for (;;) { n = ub_overlaps(addr, addr + size, 0, idx, cap); if (n >= 0) break; cap = cap ? 2 * cap : 64; idx = VK_REALLOC(idx, (size_t)cap * sizeof *idx); }
    if (!sn) {
        /* Direct updates may still be writing the original pool on the GPU. CPU-known log bytes are
           authoritative: read only uncovered native gaps, not a racing base that is overwritten below. */
        uint64_t cursor = addr, end = addr + size;
        while (cursor < end) {
            uint64_t covered = cursor, next = end;
            for (int k = 0; k < n; k++) {
                const ub_ent *e = &g_ub[idx[k]];
                if (e->lo <= cursor && e->hi > covered) covered = e->hi;
                else if (e->lo > cursor && e->lo < next) next = e->lo;
            }
            if (covered > cursor) cursor = covered < end ? covered : end;
            else {
                memcpy((uint8_t *)dst + (cursor - addr), (const void *)(uintptr_t)cursor, (size_t)(next - cursor));
                cursor = next;
            }
        }
    }
    for (int k = 0; k < n; k++) {
        const ub_ent *e = &g_ub[idx[k]];
        if (sn && e->seq <= g_snap_seq) continue;   /* only updates already included in the snapshot's base */
        uint64_t lo = e->lo > addr ? e->lo : addr, hi = e->hi < addr + size ? e->hi : addr + size;
        memcpy((uint8_t *)dst + (lo - addr), e->data + (lo - e->lo), (size_t)(hi - lo));
    }
}
/* ---------- two frames in flight: bounded protection (oot-a122, TOTK_OHOS_PROTECT; off by default) ----------
   TOTK_OHOS_SYNC_LAG=1 moves the game's frame-end wait to the previous frame's fence: the game then writes the next
   frame's data while the GPU still runs this one, and reads GPU results before this frame has written them. Upstream's
   protection (TOTK_FRAME_LAG, lag_read_select below) decides by nvn_dirty's write tracking, which HarmonyOS refuses
   (dma-buf pools cannot be write-protected: nvn_dirty.c leaves every pool untracked). Untracked, every read decision
   copies its whole range into a snapshot arena (lag_classify returns before any hot test; m->tracked = 0), a copy is
   never reused (lag_watch_copy fails), and nothing bounds the sizes (size-less storage bindings run to the end of their
   pool): the process is killed for GPU memory entering the world (oot-a119f, oot-a121c). This replaces it, bounded,
   and only runs while TOTK_FRAME_LAG and TOTK_ASYNC_REPLAY are off:
   - Freeze (the game writes, the GPU reads). Read-only bindings of the kinds in TOTK_OHOS_PROTECT_KINDS (s storage,
     t texel, v vertex, i index, c copy source; default st) are candidates. A candidate the game flushes
     (nvnMemoryPoolFlushMappedRange / nvnBufferFlushMappedRange: vk_inflight_stamp) and no GPU write touches is hot until
     600 frames after its last flush; hot candidates merge into runs. At every game submit (nvnQueueSubmitCommands, on
     the game's thread, before a logical queue may hold the submission; again when a logical queue runs an item it held
     or resumes it after an event wait) each run not yet copied in this frame, or flushed since its last copy, is copied
     into this frame's arena; the submission's reads inside a run bind that copy (vk_pt_read). A texel view onto a copy
     is never served from vk_res.c's cached-view fast path. TOTK_OHOS_PROTECT_SMALL_KB / TOTK_OHOS_PROTECT_ALL=1: runs
     up to that size / all runs are also hot without a flush and compared by content.
   - Land (the GPU writes, the game reads). A GPU write of at most TOTK_OHOS_PROTECT_LAND_KB (writable storage binding,
     texture -> buffer copy, buffer copy, fill: TOTK_OHOS_PROTECT_LAND_OPS stbf) into a CPU_CACHED pool goes to a copy of
     its window (the write rounded to TOTK_OHOS_PROTECT_LAND_GRAIN_KB, default 4, inside its pool, not over another copy
     or a neighbour the GPU wrote in memory) for this frame (vk_pt_write); the frame's reads inside the window read the
     copy. The copy starts from the game's bytes at the walk, with the bytes earlier GPU writes put into memory (this
     frame or the last, not landed: mw below) filled in on the GPU in queue order; or, when the game has not written the
     window since we last read or wrote it, from the previous frame's open copy on the GPU (TOTK_OHOS_PROTECT_CHAIN,
     default 1). When the game's nvnSyncWait returns for a fence that covers the copy's last use (nvnQueueFinish: all
     of them; a CPU step of the replay over the window: at once), the bytes the GPU changed against the copy's start are
     written into the game's memory (mtl_protect_land). The game so reads whole results at the moment its wait says they
     are done (with two frames in flight: the previous frame's), never half-written ones, and a reset it writes after
     reading is not undone by a late GPU write. A copy chained from another starts, for its own landing, from that
     one's final bytes (set when that one lands).
   - An access no copy can serve while copies overlap it (it reaches past a window, cannot bind a copy's offset, gets
     no copy, or has no known extent) goes to memory, after every overlapping copy, oldest first, is landed at once (the
     GPU is done with it) or has the bytes the GPU wrote into it copied back to memory in queue order (outside a pass:
     the main command buffer; inside one: the pre-commands, when the copy was last written in an earlier submission).
     Inside a pass a copy written in the same submission cannot be ordered: a read then reads memory (stale), a write
     drops the copy (lost). A read of unknown extent over a copy only counts (stale). All of these count as unprotected.
   - Bounds: TOTK_OHOS_PROTECT_MB (32) of copies a frame for both, runs up to TOTK_OHOS_PROTECT_MAX_KB (16384),
     TOTK_OHOS_PROTECT_ARENAS (4) frame arenas, one reused only when nothing holds it and its last GPU use is complete.
     A hot read with no copy binds memory and counts as unprotected; TOTK_OHOS_PROTECT_STRICT=1 then makes the next
     frame-end wait the full one (hle_nvn.c frame_lag_target).
   - Not covered: memory reached through the memory pool table (raw global memory), write-back of rendered textures,
     counter reports, inline uniform updates over a landed window, submissions held by a logical queue for longer than
     the arenas; GPU state a window accumulates across frames is one frame old after the game writes the window while
     an older copy is open (a chain break, counted while the game leaves it alone).
   TOTK_OHOS_PROTECT_LOG=<frame>:<frames>: runs, landings and unprotected accesses of those frames; a summary every 300.
   Threads: the walker's state below is used under the recording lock (vk_pt_present too); g_pt_mu guards what the
   game's threads share (flushes, captures, landing at their waits) and is taken before g_pool_mu, after g_tex_mu. */
enum { PT_FREEZE = 1, PT_LAND = 2, PT_ARENAS_MAX = 8, PT_CAND = 16384, PT_STAMPS = 4096, PT_GW = 4096, PT_LANDS = 256,
       PT_HOT_FRAMES = 600, PT_UNPROT_LOG = 16, PT_MW = 4096, PT_FLUSH_MAX = 16, PT_GF = 4, PT_MEMO = 256 };
typedef struct { arena_t ar; uint64_t gen; _Atomic uint64_t ticket; int refs; } pt_arena;
typedef struct { uint64_t lo, hi, stamp, copy_seq, ar_gen, hash, hash_frame, copies; VkBuffer buf; VkDeviceSize off; int ar, hashed; } pt_run;
typedef struct { uint64_t lo, hi, seen, stamped, gpuw; uint32_t kinds; uint8_t used, hot, big; } pt_cand;
typedef struct { uint64_t lo, hi; VkBuffer buf; VkDeviceSize off; int ar, ok; } pt_ent;   /* ok: 1 the copy, 0 none (unprotected), 2 the GPU writes it (memory) */
typedef struct { uint64_t frame; uint32_t armask; int n; pt_ent e[]; } pt_cap;
/* A landing copy of the window [lo, hi) for frame `frame`. ticket: the last submission that used it; wticket, [wlo, whi):
   the last one that wrote it and the span of its writes; init: its starting bytes (the landing's baseline; gf: ranges the
   GPU filled in, their baseline in stage); id / from: the copy it was chained from; exp_hash: the game's bytes as we last
   read or wrote them; nochain: its start lacks bytes the game wrote or an older copy's GPU bytes (no chain from it);
   poisoned: no longer served, retired without landing (flushed: its GPU bytes were copied back to memory). */
typedef struct {
    uint64_t lo, hi, frame, ticket, wticket, wlo, whi, exp_hash, uses, id, from;
    uint64_t gf[PT_GF][2]; int ngf;
    VkBuffer buf; VkDeviceSize off; uint8_t *ptr, *init, *stage;
    int ar, chained, poisoned, flushed, nochain; unsigned op;
} pt_land;
typedef struct { VkBuffer src, dst; VkDeviceSize so, dof, n; } pt_xfer;
#define PT_STATS(X) X(caps) X(copies) X(copy_bytes) X(reused) X(hits) X(unprot) X(budget) X(noarena) X(gpuown) X(lands) \
    X(land_bytes) X(land_hits) X(landed) X(landed_bytes) X(chained) X(conflicts) X(dropped) X(promoted) X(demoted) X(big) X(hash_bytes) \
    X(rot_waits) X(early) X(flushed) X(lost) X(stale) X(refused) X(gpufill) X(chainbreak) X(walkcopy) X(walkcopy_bytes) X(walkcopy_gw)
typedef struct {
#define PT_FIELD(f) _Atomic uint64_t f;
    PT_STATS(PT_FIELD)
#undef PT_FIELD
} pt_stats;
static pt_stats g_pt_s;
#define PT_ADD(f, n) atomic_fetch_add_explicit(&g_pt_s.f, (uint64_t)(n), memory_order_relaxed)
static pthread_mutex_t g_pt_mu = PTHREAD_MUTEX_INITIALIZER;   /* after g_tex_mu, before g_pool_mu and g_mu */
static unsigned g_pt_kinds, g_pt_land_ops = 15; static int g_pt_narenas = 4, g_pt_all, g_pt_strict, g_pt_chain = 1, g_pt_land_any;
static uint64_t g_pt_budget = UINT64_C(32) << 20, g_pt_max = UINT64_C(16) << 20, g_pt_land_max = UINT64_C(64) << 10, g_pt_small;
static uint64_t g_pt_land_grain = 4096;
static uint64_t g_pt_copy_walk = UINT64_C(4096) << 10;   /* TOTK_OHOS_PROTECT_COPY_WALK_KB: one-shot copy sources up to this size are read at the walk */
static uint64_t g_pt_recheck = UINT64_C(64) << 10;       /* TOTK_OHOS_PROTECT_RECHECK_KB: content-checked runs up to this size are checked at every capture */
static uint64_t g_pt_log_from, g_pt_log_to, g_pt_only[32], g_pt_skip[32]; static int g_pt_nonly, g_pt_nskip;
static pt_arena g_pt_ar[PT_ARENAS_MAX]; static int g_pt_cur;                          /* g_pt_mu */
static uint64_t g_pt_frame = 1, g_pt_frame_bytes, g_pt_seq;                           /* g_pt_mu: the frame recorded, its copy bytes, flushes so far */
static pt_run *g_pt_run; static int g_pt_nrun;                         /* g_pt_mu: sorted, disjoint */
static struct { uint64_t lo, hi; } g_pt_st[PT_STAMPS]; static uint64_t g_pt_st_n, g_pt_st_done;   /* g_pt_mu: the flushes; present's position */
static pt_land g_pt_land[PT_LANDS]; static int g_pt_nland; static uint64_t g_pt_land_lo = UINT64_MAX, g_pt_land_hi, g_pt_land_id;   /* g_pt_mu */
static _Atomic int g_pt_nland_a;                                                       /* g_pt_nland, for a lock-free "none" */
static pt_cand g_pt_cand[PT_CAND]; static int g_pt_ncand;                              /* walker */
static uint64_t g_pt_gw[PT_GW][2], g_pt_gw_memo[PT_MEMO][2]; static int g_pt_ngw, g_pt_gw_lost;   /* walker: this frame's GPU writes */
static uint64_t g_pt_gwp[2][PT_GW][2]; static int g_pt_ngwp[2], g_pt_gwp_lost[2], g_pt_gwp_cur;   /* walker: the two frames before (oot-a129) */
static int pt_gw_recent(uint64_t lo, uint64_t hi) {   /* walker: did the GPU write [lo, hi) this frame or in the two before (or is it unknown)? */
    if (g_pt_gw_lost || g_pt_gwp_lost[0] || g_pt_gwp_lost[1]) return 1;
    for (int k = 0; k < g_pt_ngw; k++) if (g_pt_gw[k][0] < hi && g_pt_gw[k][1] > lo) return 1;
    for (int s = 0; s < 2; s++) for (int k = 0; k < g_pt_ngwp[s]; k++) if (g_pt_gwp[s][k][0] < hi && g_pt_gwp[s][k][1] > lo) return 1;
    return 0;
}
/* walker: GPU writes that went to memory (not to a copy), this frame [g_pt_mwc] and the last; a copy filled from memory
   at the walk misses them until they ran */
static struct { uint64_t lo, hi, ticket; } g_pt_mw[2][PT_MW]; static int g_pt_nmw[2], g_pt_mw_lost[2], g_pt_mwc;
static uint64_t g_pt_mw_lo[2] = {UINT64_MAX, UINT64_MAX}, g_pt_mw_hi[2];
static struct { uint64_t lo, hi; int idx; } g_pt_mw_memo[PT_MEMO];
static pt_cap *g_pt_curcap;                                                            /* walker: the submission being replayed */
static uint64_t g_pt_clamp_addr, g_pt_clamp_n, g_pt_wframe = 1;                        /* walker */
static struct { uint64_t addr, size; int kind; } g_pt_unlog[PT_UNPROT_LOG]; static int g_pt_nunlog;   /* walker */
static _Atomic uint64_t g_pt_unsafe, g_pt_gen;
static void pt_parse(void) {
    const char *e = getenv("TOTK_OHOS_PROTECT"), *a = getenv("TOTK_ASYNC_REPLAY");
    if (!e || !*e || !strcmp(e, "0")) return;
    if (runtime_frame_lag_requested()) { LOGV("protect: TOTK_OHOS_PROTECT=%s ignored: TOTK_FRAME_LAG's own protection is on", e); return; }
    if (a && atoi(a) > 0) {   /* hle_nvn.c async_replay(): no capture at the game's submit, and its waits are token waits (nothing lands) */
        LOGV("protect: TOTK_OHOS_PROTECT=%s ignored: TOTK_ASYNC_REPLAY=%s replays submissions on another thread", e, a); return;
    }
    g_pt_on = !strcmp(e, "freeze") ? PT_FREEZE : !strcmp(e, "land") ? PT_LAND : PT_FREEZE | PT_LAND;
    const char *k = getenv("TOTK_OHOS_PROTECT_KINDS"); if (!k || !*k) k = "st";
    const char *kinds = k;
    for (; *k; k++) g_pt_kinds |= *k == 's' ? 1u << VK_LAG_STORAGE : *k == 't' ? 1u << VK_LAG_TEXEL : *k == 'v' ? 1u << VK_LAG_VERTEX :
                                  *k == 'i' ? 1u << VK_LAG_INDEX : *k == 'c' ? 1u << VK_LAG_COPY : 0u;
    long v;
    if ((e = getenv("TOTK_OHOS_PROTECT_MB")) && *e) { v = atol(e); g_pt_budget = (uint64_t)(v < 1 ? 1 : v > 1024 ? 1024 : v) << 20; }
    if ((e = getenv("TOTK_OHOS_PROTECT_MAX_KB")) && *e) { v = atol(e); g_pt_max = (uint64_t)(v < 4 ? 4 : v > 65536 ? 65536 : v) << 10; }
    if ((e = getenv("TOTK_OHOS_PROTECT_LAND_KB")) && *e) { v = atol(e); g_pt_land_max = (uint64_t)(v < 0 ? 0 : v > 1024 ? 1024 : v) << 10; }
    if ((e = getenv("TOTK_OHOS_PROTECT_LAND_GRAIN_KB")) && *e) {   /* 0: the write's own range; else the power of two at or below it, 1 to 64 KiB */
        v = atol(e); if (v > 64) v = 64;
        g_pt_land_grain = 0;
        if (v > 0) { g_pt_land_grain = 1024; while ((g_pt_land_grain << 1) <= ((uint64_t)v << 10)) g_pt_land_grain <<= 1; }
    }
    if ((e = getenv("TOTK_OHOS_PROTECT_SMALL_KB")) && *e) { v = atol(e); g_pt_small = (uint64_t)(v < 0 ? 0 : v > 4096 ? 4096 : v) << 10; }
    if ((e = getenv("TOTK_OHOS_PROTECT_COPY_WALK_KB")) && *e) { v = atol(e); g_pt_copy_walk = (uint64_t)(v < 0 ? 0 : v > 65536 ? 65536 : v) << 10; }
    if ((e = getenv("TOTK_OHOS_PROTECT_RECHECK_KB")) && *e) { v = atol(e); g_pt_recheck = (uint64_t)(v < 0 ? 0 : v > 16384 ? 16384 : v) << 10; }
    if ((e = getenv("TOTK_OHOS_PROTECT_ARENAS")) && *e) { v = atol(e); g_pt_narenas = v < 2 ? 2 : v > PT_ARENAS_MAX ? PT_ARENAS_MAX : (int)v; }
    if ((e = getenv("TOTK_OHOS_PROTECT_LAND_OPS")) && *e) {
        g_pt_land_ops = 0;
        for (; *e; e++) g_pt_land_ops |= *e == 's' ? 1u : *e == 't' ? 2u : *e == 'b' ? 4u : *e == 'f' ? 8u : 0u;
    }
    /* LAND_ONLY: land only writes covering one of these addresses; LAND_SKIP: never land writes covering one of these */
    if ((e = getenv("TOTK_OHOS_PROTECT_LAND_ONLY")) && *e)
        for (const char *p = e; *p && g_pt_nonly < 32; ) {
            char *end; uint64_t ad = strtoull(p, &end, 0);
            if (end == p) break;
            g_pt_only[g_pt_nonly++] = ad; p = *end == ',' ? end + 1 : end;
        }
    if ((e = getenv("TOTK_OHOS_PROTECT_LAND_SKIP")) && *e)
        for (const char *p = e; *p && g_pt_nskip < 32; ) {
            char *end; uint64_t ad = strtoull(p, &end, 0);
            if (end == p) break;
            g_pt_skip[g_pt_nskip++] = ad; p = *end == ',' ? end + 1 : end;
        }
    g_pt_all = (e = getenv("TOTK_OHOS_PROTECT_ALL")) && *e == '1';
    g_pt_strict = (e = getenv("TOTK_OHOS_PROTECT_STRICT")) && *e == '1';
    if ((e = getenv("TOTK_OHOS_PROTECT_CHAIN")) && *e == '0') g_pt_chain = 0;
    g_pt_land_any = (e = getenv("TOTK_OHOS_PROTECT_LAND_POOLS")) && !strcmp(e, "any");
    if ((e = getenv("TOTK_OHOS_PROTECT_LOG")) && *e) {
        g_pt_log_from = strtoull(e, NULL, 10); const char *c = strchr(e, ':');
        g_pt_log_to = g_pt_log_from + (c ? strtoull(c + 1, NULL, 10) : 3);
    }
    if (!g_pt_land_max || !g_pt_land_ops) g_pt_on &= ~PT_LAND;
    if (!g_pt_on) return;
    for (int i = 0; i < PT_ARENAS_MAX; i++) { g_pt_ar[i].ar.min = 1u << 20; g_pt_ar[i].gen = 1; }
    LOGV("protect (two frames in flight, TOTK_OHOS_PROTECT): freeze %s (kinds %s, runs up to %llu KiB, content-checked %s), "
         "land %s (writes up to %llu KiB into %s pools, windows of %llu KiB, ops %#x, chain %d, %d listed addresses, %d skipped), %llu MiB of copies a frame, %d arenas, strict %d, log frames %llu..%llu",
         g_pt_on & PT_FREEZE ? "on" : "off", kinds, (unsigned long long)(g_pt_max >> 10),
         g_pt_all ? "all" : g_pt_small ? "small" : "none", g_pt_on & PT_LAND ? "on" : "off",
         (unsigned long long)(g_pt_land_max >> 10), g_pt_land_any ? "CPU-accessible" : "CPU_CACHED", (unsigned long long)(g_pt_land_grain >> 10),
         g_pt_land_ops, g_pt_chain, g_pt_nonly, g_pt_nskip, (unsigned long long)(g_pt_budget >> 20),
         g_pt_narenas, g_pt_strict, (unsigned long long)g_pt_log_from, (unsigned long long)g_pt_log_to);
}
int vk_pt_active(void) { return g_pt_on != 0; }   /* vk_res.c: protect-only texture safeguards (oot-a127) */
static uint64_t pt_hash(const uint8_t *p, size_t n) {
    uint64_t h = UINT64_C(0x9e3779b97f4a7c15) ^ n; size_t i = 0;
    for (; i + 8 <= n; i += 8) { uint64_t w; memcpy(&w, p + i, 8); h = (h ^ w) * UINT64_C(0x100000001b3); h ^= h >> 31; }
    for (; i < n; i++) h = (h ^ p[i]) * UINT64_C(0x100000001b3);
    return h;
}
static size_t pt_diff_copy(uint8_t *dst, const uint8_t *now, const uint8_t *was, size_t n) {   /* only the bytes the GPU changed */
    size_t changed = 0, i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t a, b; memcpy(&a, now + i, 8); memcpy(&b, was + i, 8);
        if (a == b) continue;
        for (size_t k = i; k < i + 8; k++) if (now[k] != was[k]) { dst[k] = now[k]; changed++; }
    }
    for (; i < n; i++) if (now[i] != was[i]) { dst[i] = now[i]; changed++; }
    return changed;
}
static int pt_run_first(uint64_t a) {   /* the first run ending after a; g_pt_mu */
    int lo = 0, hi = g_pt_nrun;
    while (lo < hi) { int m = (lo + hi) >> 1; if (g_pt_run[m].hi <= a) lo = m + 1; else hi = m; }
    return lo;
}
static int pt_ent_first(const pt_cap *c, uint64_t a) {
    int lo = 0, hi = c->n;
    while (lo < hi) { int m = (lo + hi) >> 1; if (c->e[m].hi <= a) lo = m + 1; else hi = m; }
    return lo;
}
static uint64_t pt_ticket(void) { return g_reserved_fence + 1; }   /* walker: the fence of the submission being recorded */
static void pt_arena_used(int ar) {   /* walker: the slot being recorded reads or writes arena ar */
    if (ar >= 0 && ar < PT_ARENAS_MAX) atomic_store_explicit(&g_pt_ar[ar].ticket, pt_ticket(), memory_order_relaxed);
}
/* g_pool_mu held: NVN flags (-1 unknown) and bounds of the smallest registered pool holding a */
static int pt_pool_find(uint64_t a, uint64_t *plo, uint64_t *phi) {
    static struct { uint64_t lo, hi, gen; int flags; } memo[64];
    uint64_t gen = atomic_load_explicit(&g_span_gen, memory_order_relaxed);
    unsigned h = (unsigned)((a >> 16) ^ (a >> 26)) & 63;
    if (!(memo[h].gen == gen && a >= memo[h].lo && a < memo[h].hi)) {
        const pool_ent *inner = NULL;
        for (int i = 0; i < g_npool; i++) {
            const pool_ent *p = g_pools[i];
            if (a >= p->lo && a < p->hi && (!inner || p->hi - p->lo < inner->hi - inner->lo)) inner = p;
        }
#ifdef TOTK_OHOS
        int flags = inner ? inner->flags : -1;
#else
        int flags = -1;
#endif
        memo[h].lo = inner ? inner->lo : a; memo[h].hi = inner ? inner->hi : a + 1; memo[h].gen = gen; memo[h].flags = flags;
    }
    *plo = memo[h].lo; *phi = memo[h].hi;
    return memo[h].flags;
}
/* any thread: the game flushed [addr, +n) */
void vk_pt_stamp(const void *addr, size_t n) {
    if (!(g_pt_on & PT_FREEZE) || !n) return;
    uint64_t lo = (uint64_t)(uintptr_t)addr, hi = lo + n;
    pthread_mutex_lock(&g_pt_mu);
    uint64_t s = ++g_pt_seq;
    g_pt_st[g_pt_st_n % PT_STAMPS].lo = lo; g_pt_st[g_pt_st_n % PT_STAMPS].hi = hi; g_pt_st_n++;
    for (int i = pt_run_first(lo); i < g_pt_nrun && g_pt_run[i].lo < hi; i++) g_pt_run[i].stamp = s;
    pthread_mutex_unlock(&g_pt_mu);
}
/* the game's thread, at nvnQueueSubmitCommands (or a logical queue running an item it held): every hot run as of now
   (copied when flushed since its last copy) */
void *mtl_protect_capture(void) {
    if (!(g_pt_on & PT_FREEZE)) return NULL;
    pthread_mutex_lock(&g_pt_mu);
    pt_cap *c = VK_MALLOC(sizeof *c + (size_t)(g_pt_nrun ? g_pt_nrun : 1) * sizeof c->e[0]);
    c->frame = g_pt_frame; c->armask = 0; c->n = 0;
    int cur = g_pt_cur;
    PT_ADD(caps, 1);
    if (g_pt_nrun) pthread_mutex_lock(&g_pool_mu);   /* the runs' memory stays registered (mapped) while it is read */
    for (int i = 0; i < g_pt_nrun; i++) {
        pt_run *r = &g_pt_run[i]; uint64_t size = r->hi - r->lo;
        VkBuffer pb; uint64_t po, avail;
        if (!resolve_span(r->lo, size, &pb, &po, &avail)) continue;   /* its pool is gone: nothing binds it */
        /* a copy serves the frame whose arena holds it (another frame reading it would hold that arena past its frame's
           fence and stall the rotation in vk_pt_present); without an arena an older copy still beats none */
        int valid = r->ar >= 0 && g_pt_ar[r->ar].gen == r->ar_gen && r->copy_seq >= r->stamp && (r->ar == cur || cur < 0);
        /* content-checked runs: at the frame's first submit that finds them, and small ones at every submit (oot-a133,
           TOTK_OHOS_PROTECT_RECHECK_KB, default 64; 0: once a frame). Before, every later submit of the frame reused the
           frame-start copy: data the game writes mid-frame without a flush and draws once (a button prompt's text, drawn
           into its label when the prompt appears) was read stale, and the label stayed blank for hundreds of frames
           (a127-skc 591, a131-skc5 552 frames; the texture never changed, so it was not the upload). */
        if (valid && r->hashed && (r->hash_frame != g_pt_frame || size <= g_pt_recheck)) {
            valid = pt_hash((const uint8_t *)(uintptr_t)r->lo, (size_t)size) == r->hash;
            r->hash_frame = g_pt_frame;
            PT_ADD(hash_bytes, size);
        }
        if (!valid) {
            uint64_t shift = r->lo & 255;
            if (cur < 0) PT_ADD(noarena, 1);
            else if (g_pt_frame_bytes + size + shift > g_pt_budget) PT_ADD(budget, 1);
            else {
                VkBuffer cb; VkDeviceSize co;
                uint8_t *p = arena_alloc(&g_pt_ar[cur].ar, (size_t)(size + shift), 256, &cb, &co);
                p += shift; co += shift;   /* the copy keeps the guest address's offset within 256 bytes: aligned bindings stay aligned */
                memcpy(p, (const void *)(uintptr_t)r->lo, (size_t)size);
                if (r->hashed) { r->hash = pt_hash(p, (size_t)size); r->hash_frame = g_pt_frame; }
                r->buf = cb; r->off = co; r->ar = cur; r->ar_gen = g_pt_ar[cur].gen; r->copies++;
                r->copy_seq = g_pt_seq > r->stamp ? g_pt_seq : r->stamp;
                g_pt_frame_bytes += size + shift;
                PT_ADD(copies, 1); PT_ADD(copy_bytes, size);
                valid = 1;
            }
        } else PT_ADD(reused, 1);
        pt_ent *e = &c->e[c->n++];
        e->lo = r->lo; e->hi = r->hi; e->ok = valid; e->buf = valid ? r->buf : VK_NULL_HANDLE; e->off = valid ? r->off : 0; e->ar = valid ? r->ar : -1;
        if (valid) c->armask |= 1u << r->ar;
    }
    if (g_pt_nrun) pthread_mutex_unlock(&g_pool_mu);
    for (int a = 0; a < PT_ARENAS_MAX; a++) if (c->armask >> a & 1) g_pt_ar[a].refs++;
    pthread_mutex_unlock(&g_pt_mu);
    return c;
}
void mtl_protect_begin(void *cap) { g_pt_curcap = cap; atomic_fetch_add_explicit(&g_pt_gen, 1, memory_order_relaxed); }   /* walker, under the recording lock */
void mtl_protect_end(void) { g_pt_curcap = NULL; }
void mtl_protect_release(void *capv) {   /* the submission's replay is over (its slots hold the arena by their tickets) */
    pt_cap *c = capv;
    if (!c) return;
    pthread_mutex_lock(&g_pt_mu);
    for (int a = 0; a < PT_ARENAS_MAX; a++) if (c->armask >> a & 1) g_pt_ar[a].refs--;
    pthread_mutex_unlock(&g_pt_mu);
    free(c);
}
uint64_t vk_pt_generation(void) { return atomic_load_explicit(&g_pt_gen, memory_order_relaxed); }
static void pt_cand_note(uint64_t lo, uint64_t hi, int kind) {   /* walker */
    unsigned h = (unsigned)(((lo >> 4) ^ (lo >> 20) ^ (hi * 0x9e3779b1u)) & (PT_CAND - 1));
    for (unsigned k = 0; k < 32; k++, h = (h + 1) & (PT_CAND - 1)) {
        pt_cand *c = &g_pt_cand[h];
        if (c->used && c->lo == lo && c->hi == hi) { c->seen = g_pt_wframe; c->kinds |= 1u << kind; return; }
        if (!c->used) {
            if (g_pt_ncand >= PT_CAND * 3 / 4) return;
            memset(c, 0, sizeof *c); c->lo = lo; c->hi = hi; c->seen = g_pt_wframe; c->kinds = 1u << kind; c->used = 1; g_pt_ncand++;
            return;
        }
    }
}
/* walker: a GPU access protection could not cover (TOTK_OHOS_PROTECT_STRICT counts it). code: a vk_lag_kind (a hot read
   without a copy), 100 + op an access no copy could serve while copies overlapped it, 200 + op a chain break, 300 + op
   a landable access that got no copy */
static void pt_unsafe_note(uint64_t addr, uint64_t size, int code) {
    atomic_fetch_add_explicit(&g_pt_unsafe, 1, memory_order_relaxed);
    if (g_pt_wframe >= g_pt_log_from && g_pt_wframe < g_pt_log_to && g_pt_nunlog < PT_UNPROT_LOG) {
        g_pt_unlog[g_pt_nunlog].addr = addr; g_pt_unlog[g_pt_nunlog].size = size; g_pt_unlog[g_pt_nunlog].kind = code; g_pt_nunlog++;
    }
}
static void pt_unprot(uint64_t addr, uint64_t size, int kind) { PT_ADD(unprot, 1); pt_unsafe_note(addr, size, kind); }   /* walker */
static void pt_land_bbox(void) {   /* g_pt_mu */
    g_pt_land_lo = UINT64_MAX; g_pt_land_hi = 0;
    for (int j = 0; j < g_pt_nland; j++) {
        if (g_pt_land[j].lo < g_pt_land_lo) g_pt_land_lo = g_pt_land[j].lo;
        if (g_pt_land[j].hi > g_pt_land_hi) g_pt_land_hi = g_pt_land[j].hi;
    }
    atomic_store_explicit(&g_pt_nland_a, g_pt_nland, memory_order_relaxed);
}
static int pt_landable(uint64_t lo, uint64_t hi, unsigned op) {
    if (!(g_pt_on & PT_LAND) || !(g_pt_land_ops & op) || hi - lo > g_pt_land_max) return 0;
    for (int i = 0; i < g_pt_nskip; i++) if (g_pt_skip[i] >= lo && g_pt_skip[i] < hi) return 0;
    if (!g_pt_nonly) return 1;
    for (int i = 0; i < g_pt_nonly; i++) if (g_pt_only[i] >= lo && g_pt_only[i] < hi) return 1;
    return 0;
}
/* ---- GPU writes to memory (mw), walker ---- */
static int pt_flags_landable(int f) { return g_pt_land_any ? f < 0 || !(f & 0x1) : f >= 0 && (f & 0x4); }
static int pt_pool_landable(uint64_t a) {   /* g_pool_mu not held: a's pool can hold a landing copy (memo per 64 KiB page) */
    static struct { uint64_t page, gen; int ok; } memo[256];
    uint64_t gen = atomic_load_explicit(&g_span_gen, memory_order_relaxed), page = a >> 16, plo, phi;
    unsigned h = (unsigned)(page ^ (page >> 8)) & 255;
    if (memo[h].gen == gen && memo[h].page == page) return memo[h].ok;
    pthread_mutex_lock(&g_pool_mu);
    int ok = pt_flags_landable(pt_pool_find(a, &plo, &phi));
    pthread_mutex_unlock(&g_pool_mu);
    if (plo <= page << 16 && phi >= (page + 1) << 16) { memo[h].page = page; memo[h].gen = gen; memo[h].ok = ok; }   /* the page is all that pool */
    return ok;
}
/* filter: keep it only when it starts or ends in a pool that can hold a landing copy (bulk writes to uncached pools would
   fill the list); callers holding g_pool_mu pass 0 */
static void pt_mw_add(uint64_t lo, uint64_t hi, int filter) {
    if (!(g_pt_on & PT_LAND) || hi <= lo) return;
    if (filter && !pt_pool_landable(lo) && !pt_pool_landable(hi - 1)) return;
    int c = g_pt_mwc; uint64_t t = pt_ticket();
    unsigned h = (unsigned)((lo >> 6) ^ (lo >> 20) ^ (hi * 0x9e3779b1u)) & (PT_MEMO - 1);
    int i = g_pt_mw_memo[h].idx;
    if (g_pt_mw_memo[h].lo == lo && g_pt_mw_memo[h].hi == hi && i < g_pt_nmw[c] && g_pt_mw[c][i].lo == lo && g_pt_mw[c][i].hi == hi) {
        g_pt_mw[c][i].ticket = t; return;   /* the same range again (every draw of a pass binds it): its last submission */
    }
    if (lo < g_pt_mw_lo[c]) g_pt_mw_lo[c] = lo;
    if (hi > g_pt_mw_hi[c]) g_pt_mw_hi[c] = hi;
    if (g_pt_nmw[c] == PT_MW) { g_pt_mw_lost[c] = 1; return; }   /* full: every window in the span counts as written */
    i = g_pt_nmw[c]++;
    g_pt_mw[c][i].lo = lo; g_pt_mw[c][i].hi = hi; g_pt_mw[c][i].ticket = t;
    g_pt_mw_memo[h].lo = lo; g_pt_mw_memo[h].hi = hi; g_pt_mw_memo[h].idx = i;
}
static int pt_mw_any(uint64_t lo, uint64_t hi) {   /* a GPU write to memory over [lo, hi) this frame or the last */
    if (hi <= lo) return 0;
    for (int c = 0; c < 2; c++) {
        if (!(lo < g_pt_mw_hi[c] && g_pt_mw_lo[c] < hi)) continue;
        if (g_pt_mw_lost[c]) return 1;
        for (int i = 0; i < g_pt_nmw[c]; i++) if (g_pt_mw[c][i].lo < hi && lo < g_pt_mw[c][i].hi) return 1;
    }
    return 0;
}
/* those writes clipped to [lo, hi), merged, sorted: at most max ranges (-1: more, or the list overflowed);
   *cur_slot: one of them is in the submission being recorded */
static int pt_mw_ranges(uint64_t lo, uint64_t hi, uint64_t t, uint64_t (*r)[2], int max, int *cur_slot) {
    int n = 0; *cur_slot = 0;
    for (int c = 0; c < 2; c++) {
        if (!(lo < g_pt_mw_hi[c] && g_pt_mw_lo[c] < hi)) continue;
        if (g_pt_mw_lost[c]) return -1;
        for (int i = 0; i < g_pt_nmw[c]; i++) {
            uint64_t a = g_pt_mw[c][i].lo, b = g_pt_mw[c][i].hi;
            if (!(a < hi && lo < b)) continue;
            if (a < lo) a = lo;
            if (b > hi) b = hi;
            if (g_pt_mw[c][i].ticket >= t) *cur_slot = 1;
            int k = 0;
            for (; k < n; k++) if (a <= r[k][1] && r[k][0] <= b) { if (a < r[k][0]) r[k][0] = a; if (b > r[k][1]) r[k][1] = b; break; }
            if (k == n) { if (n == max) return -1; r[n][0] = a; r[n][1] = b; n++; }
        }
    }
    for (int i = 1; i < n; i++) for (int j = i; j > 0 && r[j - 1][0] > r[j][0]; j--) {   /* sort, then merge what the growth made overlap */
        uint64_t a = r[j][0], b = r[j][1]; r[j][0] = r[j - 1][0]; r[j][1] = r[j - 1][1]; r[j - 1][0] = a; r[j - 1][1] = b;
    }
    int m = 0;
    for (int i = 0; i < n; i++) {
        if (m && r[i][0] <= r[m - 1][1]) { if (r[i][1] > r[m - 1][1]) r[m - 1][1] = r[i][1]; }
        else { r[m][0] = r[i][0]; r[m][1] = r[i][1]; m++; }
    }
    return m;
}
/* ---- landing copies, g_pt_mu ---- */
static void pt_sort_frames(int *idx, int k) {   /* oldest frame first (insertion: k is small) */
    for (int i = 1; i < k; i++) {
        int x = idx[i], j = i;
        while (j > 0 && g_pt_land[idx[j - 1]].frame > g_pt_land[x].frame) { idx[j] = idx[j - 1]; j--; }
        idx[j] = x;
    }
}
static void pt_succ_init(const pt_land *e) {   /* the copies chained from e started from its final bytes: their landing baseline */
    for (int j = 0; j < g_pt_nland; j++) {
        pt_land *y = &g_pt_land[j];
        if (y != e && y->init && y->chained && y->from == e->id) memcpy(y->init, e->ptr, (size_t)(e->hi - e->lo));
    }
}
/* + g_pool_mu, e done on the GPU: the bytes the GPU changed against e's start into the game's memory; 0: its pool is gone */
static int pt_land_one(pt_land *e) {
    size_t n = (size_t)(e->hi - e->lo);
    VkBuffer b; uint64_t o, avail;
    if (!resolve_span(e->lo, n, &b, &o, &avail)) return 0;
    for (int i = 0; i < e->ngf; i++) {   /* the ranges the GPU filled in: their baseline is what it filled */
        size_t d = (size_t)(e->gf[i][0] - e->lo); memcpy(e->init + d, e->stage + d, (size_t)(e->gf[i][1] - e->gf[i][0]));
    }
    uint8_t *h = (uint8_t *)(uintptr_t)e->lo;
    uint64_t pre = pt_hash(h, n);
    PT_ADD(landed_bytes, pt_diff_copy(h, e->ptr, e->init, n)); PT_ADD(landed, 1);
    uint64_t post = pt_hash(h, n);
    pt_succ_init(e);
    for (int j = 0; j < g_pt_nland; j++) {   /* the other open copies of the window */
        pt_land *y = &g_pt_land[j];
        if (y == e || !y->init || y->lo != e->lo || y->hi != e->hi) continue;
        if (y->exp_hash != pre) y->nochain = 1;   /* the game wrote the window since we last saw it: that copy lacks its bytes */
        y->exp_hash = post;
    }
    return 1;
}
static void pt_retire(pt_land *e) { free(e->init); e->init = NULL; e->stage = NULL; g_pt_ar[e->ar].refs--; e->hi = e->lo; }   /* compacted later */
static void pt_land_compact(void) {
    int m = 0;
    for (int j = 0; j < g_pt_nland; j++) if (g_pt_land[j].hi > g_pt_land[j].lo) g_pt_land[m++] = g_pt_land[j];
    g_pt_nland = m; pt_land_bbox();
}
/* walker, g_pt_mu: an access to [lo, hi) no copy serves goes to memory (op 0 a read, else a GPU write). Each overlapping
   copy, oldest first: landed now if the GPU is done with it; else the bytes the GPU wrote into it go back to memory in
   queue order (outside a pass: the main command buffer; inside one: the pre-commands, when it was last written in an
   earlier submission) and it is no longer served; else a read leaves it (and reads stale memory), a write drops it
   (its GPU bytes are lost). Counted as unprotected. */
static void pt_unserved_locked(uint64_t lo, uint64_t hi, unsigned op, int in_pass, pt_xfer *fl, int *nfl) {
    int idx[PT_LANDS], k = 0, removed = 0, stale = 0;
    uint64_t t = pt_ticket(), done = atomic_load(&g_done);
    if (g_pt_nland && lo < g_pt_land_hi && hi > g_pt_land_lo)
        for (int j = 0; j < g_pt_nland; j++) { const pt_land *e = &g_pt_land[j]; if (!e->poisoned && e->lo < hi && lo < e->hi) idx[k++] = j; }
    if (k) {
        pt_sort_frames(idx, k);
        pthread_mutex_lock(&g_pool_mu);
        for (int i = 0; i < k; i++) {
            pt_land *e = &g_pt_land[idx[i]];
            if (e->ticket <= done) { if (pt_land_one(e)) PT_ADD(early, 1); else PT_ADD(dropped, 1); pt_retire(e); removed = 1; continue; }
            int ordered = !in_pass || e->wticket < t;
            if (!op && !ordered) { stale = 1; continue; }
            if (e->whi > e->wlo) {
                VkBuffer pb; uint64_t po, avail;
                if (ordered && *nfl < PT_FLUSH_MAX && resolve_span(e->wlo, e->whi - e->wlo, &pb, &po, &avail)) {
                    fl[(*nfl)++] = (pt_xfer){e->buf, pb, e->off + (e->wlo - e->lo), po, e->whi - e->wlo};
                    pt_mw_add(e->wlo, e->whi, 0); e->flushed = 1; PT_ADD(flushed, 1);
                } else PT_ADD(lost, 1);
            }
            e->poisoned = 1; e->ticket = t; pt_arena_used(e->ar);   /* kept (and its arena) until the write-back ran */
        }
        pthread_mutex_unlock(&g_pool_mu);
        if (removed) pt_land_compact();
        atomic_fetch_add_explicit(&g_pt_gen, 1, memory_order_relaxed);
    }
    if (op) pt_mw_add(lo, hi, !k);   /* over a copy: kept whatever its pool */
    if (k) { PT_ADD(conflicts, 1); if (stale) PT_ADD(stale, 1); pt_unsafe_note(lo, hi - lo, 100 + (int)op); }
}
/* + g_pool_mu: the window a new copy for [lo, hi) covers: rounded to the grain, inside the pool [plo, phi) and one buffer,
   over no other open copy and no neighbour the GPU wrote in memory; else [lo, hi) */
static void pt_window(uint64_t lo, uint64_t hi, uint64_t plo, uint64_t phi, uint64_t *elo, uint64_t *ehi) {
    *elo = lo; *ehi = hi;
    uint64_t g = g_pt_land_grain;
    if (!g) return;
    uint64_t a = lo & ~(g - 1), b = hi + g - 1 < hi ? hi : (hi + g - 1) & ~(g - 1);
    if (a < plo) a = plo;
    if (b > phi) b = phi;
    if (a > lo || b < hi || (a == lo && b == hi)) return;
    VkBuffer pb; uint64_t po, avail;
    if (!resolve_span(a, b - a, &pb, &po, &avail)) return;
    for (int j = 0; j < g_pt_nland; j++) { const pt_land *e = &g_pt_land[j]; if (!e->poisoned && e->hi > e->lo && e->lo < b && a < e->hi) return; }
    if (pt_mw_any(a, lo) || pt_mw_any(hi, b)) return;
    *elo = a; *ehi = b;
}
/* g_pt_mu: a new copy for the access [lo, hi) (op 0 a read of prev's window); 1 made and bound (out). Refusals of a
   landable access are counted (unprotected). */
static int pt_land_new(uint64_t lo, uint64_t hi, unsigned op, pt_land *prev, uint64_t t, int in_pass, VkDescriptorBufferInfo *out,
                       pt_xfer *chain, pt_xfer *fill, int *nfill, int *fill_pre) {
    int why = g_pt_nland == PT_LANDS ? 1 : g_pt_cur < 0 ? 2 : 0;   /* 1 table full, 2 no arena, 3 budget, 4 memory written (see below), 5 not landable */
    if (!why) {
        pthread_mutex_lock(&g_pool_mu);
        uint64_t elo = lo, ehi = hi;
        if (prev) { elo = prev->lo; ehi = prev->hi; }
        else {   /* NVN pool flags: 0x1 CPU_NO_ACCESS (nothing to land), 0x4 CPU_CACHED (where the game keeps what it reads back) */
            uint64_t plo, phi; int f = pt_pool_find(lo, &plo, &phi);
            if (!pt_flags_landable(f)) why = 5;
            else pt_window(lo, hi, plo, phi, &elo, &ehi);
        }
        uint64_t size = ehi - elo, shift = elo & 255, hh = 0, gf[PT_GF][2];
        VkBuffer pb = VK_NULL_HANDLE; uint64_t po = 0, avail;
        int chained = 0, game_wrote = 0, ngf = 0, cur_slot = 0;
        if (!why && !resolve_span(elo, size, &pb, &po, &avail)) why = 5;
        const uint8_t *h = (const uint8_t *)(uintptr_t)elo;
        if (!why) {
            hh = pt_hash(h, (size_t)size);
            game_wrote = prev && hh != prev->exp_hash;
            chained = prev && g_pt_chain && !game_wrote && !prev->nochain;
            if (!chained) {   /* GPU writes in memory the walk's bytes lack: filled in on the GPU, in queue order */
                ngf = pt_mw_ranges(elo, ehi, t, gf, PT_GF, &cur_slot);
                if (ngf < 0 || (cur_slot && in_pass)) why = 4;   /* too scattered, or written earlier in this submission's pass */
            }
        }
        uint64_t need = shift + size * (ngf > 0 ? 2 : 1);
        if (!why && g_pt_frame_bytes + need > g_pt_budget) why = 3;
        if (!why) {
            VkBuffer b; VkDeviceSize o;
            uint8_t *p = arena_alloc(&g_pt_ar[g_pt_cur].ar, (size_t)need, 256, &b, &o);
            p += shift; o += shift;   /* the copy keeps the address's offset within 256 bytes: an offset valid in the pool is valid here */
            pt_land *e = &g_pt_land[g_pt_nland];
            memset(e, 0, sizeof *e);
            e->lo = elo; e->hi = ehi; e->frame = g_pt_frame; e->ticket = t; e->exp_hash = hh; e->uses = 1; e->id = ++g_pt_land_id;
            e->buf = b; e->off = o; e->ptr = p; e->ar = g_pt_cur; e->op = op; e->wlo = UINT64_MAX;
            e->init = VK_MALLOC((size_t)size);
            if (chained) {   /* the game left it alone: the previous frame's GPU bytes carry over, in queue order */
                memcpy(e->init, prev->init, (size_t)size);   /* placeholder: prev's final bytes once prev lands (pt_succ_init) */
                *chain = (pt_xfer){prev->buf, b, prev->off, o, size}; pt_arena_used(prev->ar);
                e->chained = 1; e->from = prev->id; PT_ADD(chained, 1);
            } else {
                memcpy(p, h, (size_t)size); memcpy(e->init, h, (size_t)size);
                if (prev) {   /* made from memory while prev's GPU bytes are not there yet */
                    e->nochain = 1;
                    if (g_pt_chain && !game_wrote) { PT_ADD(chainbreak, 1); pt_unsafe_note(lo, hi - lo, 200 + (int)op); }
                }
                if (ngf > 0) {
                    e->stage = p + size; e->ngf = ngf;
                    for (int i = 0; i < ngf; i++) {
                        uint64_t d = gf[i][0] - elo, n = gf[i][1] - gf[i][0];
                        e->gf[i][0] = gf[i][0]; e->gf[i][1] = gf[i][1];
                        fill[(*nfill)++] = (pt_xfer){pb, b, po + d, o + d, n};
                        fill[(*nfill)++] = (pt_xfer){pb, b, po + d, o + size + d, n};
                    }
                    *fill_pre = in_pass;   /* inside a pass only writes of earlier submissions (cur_slot refused) */
                    PT_ADD(gpufill, 1);
                }
            }
            for (int j = 0; j < g_pt_nland; j++) if (g_pt_land[j].lo == elo && g_pt_land[j].hi == ehi) g_pt_land[j].exp_hash = hh;
            if (op) { e->wticket = t; e->wlo = lo; e->whi = hi; }
            g_pt_ar[g_pt_cur].refs++; g_pt_frame_bytes += need; g_pt_nland++; pt_land_bbox();
            pt_arena_used(e->ar);
            PT_ADD(lands, 1); PT_ADD(land_bytes, size);
            atomic_fetch_add_explicit(&g_pt_gen, 1, memory_order_relaxed);
            *out = (VkDescriptorBufferInfo){b, o + (lo - elo), hi - lo};
        }
        pthread_mutex_unlock(&g_pool_mu);
    }
    if (why && why != 5) {
        PT_ADD(refused, 1);
        if (why == 2) PT_ADD(noarena, 1); else if (why == 3) PT_ADD(budget, 1);
        pt_unsafe_note(lo, hi - lo, 300 + (int)op);
    }
    return !why;
}
static void pt_xfer_record(const pt_xfer *x, int n, int pre, int ordered) {   /* walker, no lock: beginning a slot's command buffers may wait for a ring slot */
    if (n <= 0) return;
    VkCommandBuffer cb = pre ? vk_pre_cb() : vk_cb();
    vk_barrier_all(cb);
    for (int i = 0; i < n; i++) {
        VkBufferCopy c = {x[i].so, x[i].dof, x[i].n}; vkCmdCopyBuffer(cb, x[i].src, x[i].dst, 1, &c);
        if (ordered && i + 1 < n) vk_barrier_all(cb);   /* write-backs of one window, oldest first: the newer one wins */
    }
    vk_barrier_all(cb);
}
/* walker: the frame's copy for an access to [lo, hi). op 0: a read (served only when a copy exists or an older frame's
   copy has not landed yet); else a GPU write (1 storage, 2 texture -> buffer, 4 buffer copy, 8 fill). align: the offset
   the binding needs (0 any). 1 served (out); 0 memory; -1 memory, the window being a landed one (no freeze lookup). */
static int pt_land_access(uint64_t lo, uint64_t hi, unsigned op, VkDeviceSize align, VkDescriptorBufferInfo *out) {
    if (!op && !atomic_load_explicit(&g_pt_nland_a, memory_order_relaxed)) return 0;
    int r = 0, conflict = 0, mis = align > 1 && lo % align != 0, in_pass = vk_in_pass(), nfill = 0, fill_pre = 0, nfl = 0;
    pt_land *cur = NULL, *prev = NULL;
    pt_xfer chain = {0}, fill[2 * PT_GF], fl[PT_FLUSH_MAX];
    uint64_t t = pt_ticket();
    pthread_mutex_lock(&g_pt_mu);
    if (g_pt_nland && lo < g_pt_land_hi && hi > g_pt_land_lo)
        for (int j = 0; j < g_pt_nland; j++) {
            pt_land *e = &g_pt_land[j];
            if (e->poisoned || !(e->lo < hi && lo < e->hi)) continue;
            if (!(e->lo <= lo && hi <= e->hi)) conflict = 1;   /* windows never overlap: inside one means inside its family */
            else if (e->frame == g_pt_frame) cur = e;
            else if (!prev || e->frame > prev->frame) prev = e;
        }
    if (conflict || (mis && (cur || prev))) {
        pt_unserved_locked(lo, hi, op, in_pass, fl, &nfl); r = -1;
    } else if (cur) {
        cur->uses++; cur->ticket = t; pt_arena_used(cur->ar);
        if (op) { cur->wticket = t; if (lo < cur->wlo) cur->wlo = lo; if (hi > cur->whi) cur->whi = hi; }
        *out = (VkDescriptorBufferInfo){cur->buf, cur->off + (lo - cur->lo), hi - lo};
        PT_ADD(land_hits, 1); r = 1;
    } else if (mis) {
        if (op) pt_mw_add(lo, hi, 1);
    } else if (prev || (pt_landable(lo, hi, op) && pt_pool_landable(lo))) {   /* the pool test is memoized: most writes are in uncached pools */
        r = pt_land_new(lo, hi, op, prev, t, in_pass, out, &chain, fill, &nfill, &fill_pre);
        if (!r && prev) { pt_unserved_locked(lo, hi, op, in_pass, fl, &nfl); r = -1; }   /* no copy: memory, after prev's GPU bytes */
        else if (!r && op) pt_mw_add(lo, hi, 1);
    } else if (op) pt_mw_add(lo, hi, 1);
    pthread_mutex_unlock(&g_pt_mu);
    pt_xfer_record(&chain, chain.n ? 1 : 0, 1, 0);
    pt_xfer_record(fill, nfill, fill_pre, 0);
    pt_xfer_record(fl, nfl, in_pass, 1);
    return r;
}
static int pt_ub_logged(uint64_t a, uint64_t n) {   /* walker: an inline uniform update of this submission overlaps (a submit-time copy lacks it) */
    uint64_t end = a + n;
    if (!g_nub || end <= g_ub_lo || a >= g_ub_hi) return 0;
    int lo = 0, b = g_nub;
    while (lo < b) { int m = (lo + b) >> 1; if (g_ub[m].lo < end) lo = m + 1; else b = m; }
    for (int i = lo - 1; i >= 0 && g_ub_maxhi[i] > a; i--) if (g_ub[i].hi > a && g_ub[i].seq > g_snap_seq) return 1;
    return 0;
}
/* walker: a read of [addr, +size) the GPU makes; 1 with out = the landed or frozen copy */
static int pt_ub_live(uint64_t a, uint64_t hi) {   /* walker: a logged inline uniform update over [a, hi) the GPU may not have run yet */
    if (!g_nub || hi <= g_ub_lo || a >= g_ub_hi) return 0;
    uint64_t done = atomic_load(&g_done); int lo = 0, b = g_nub;
    while (lo < b) { int m = (lo + b) >> 1; if (g_ub[m].lo < hi) lo = m + 1; else b = m; }
    for (int i = lo - 1; i >= 0 && g_ub_maxhi[i] > a; i--)
        if (g_ub[i].hi > a && (!g_ub[i].fence || g_ub[i].fence > done)) return 1;
    return 0;
}
int vk_pt_read(uint64_t addr, uint64_t size, VkDeviceSize align, int kind, VkDescriptorBufferInfo *out) {
    if (!size || addr + size < addr) return 0;
    uint64_t hi = addr + size;
    if (kind == VK_LAG_COPY || kind == VK_LAG_VERTEX || kind == VK_LAG_INDEX) align = 0;   /* bound at the pool's offset, which a copy keeps */
    if (g_pt_on & PT_LAND) {
        int r = pt_land_access(addr, hi, 0, align, out);
        if (r) return r > 0;
    }
    if (!(g_pt_on & PT_FREEZE)) return 0;
    if (kind == VK_LAG_COPY && size <= g_pt_copy_walk) {
        /* (oot-a132, review of a129) not over a pool the CPU cannot access (nothing the CPU rewrites, and where kernels'
           pool-table stores land, which no GPU-write record shows) nor over an inline uniform update the GPU has not run */
        uint64_t plo, phi; pthread_mutex_lock(&g_pool_mu); int pf = pt_pool_find(addr, &plo, &phi); pthread_mutex_unlock(&g_pool_mu);
        if ((pf >= 0 && (pf & 0x1)) || pt_ub_live(addr, hi)) { PT_ADD(walkcopy_gw, 1); goto no_walk; }
        /* A one-shot copy source (a texture upload, buffer -> texture / buffer): read the game's bytes now, at the walk of
           the submission that copies them, into this slot's transient memory. With two frames in flight the GPU runs the
           copy later, by when the game may already have written the next frame's bytes into the same staging memory,
           and the one-shot destination keeps them (a123y-wlo5b / a127-skc: a button prompt drawn without its glyphs for
           hundreds of frames). oot-a125 did this only for sources inside a frozen run and it never fired: staging
           memory is not a hot run. A range the GPU wrote this frame or in the two before goes to memory in queue order
           (a snapshot would read it before that write ran). TOTK_OHOS_PROTECT_COPY_WALK_KB=0: off. */
        if (pt_gw_recent(addr, hi)) PT_ADD(walkcopy_gw, 1);
        else {
            VkBuffer tb; VkDeviceSize to; void *p = vk_transient((size_t)size, 16, &tb, &to);
            if (p) {
                memcpy(p, (const void *)(uintptr_t)addr, (size_t)size);
                PT_ADD(walkcopy, 1); PT_ADD(walkcopy_bytes, size);
                *out = (VkDescriptorBufferInfo){tb, to, size};
                return 1;
            }
        }
    }
no_walk:
    if (kind >= 0 && kind < 32 && (g_pt_kinds >> kind & 1) && size <= g_pt_max) pt_cand_note(addr, hi, kind);   /* bound this frame (keeps a hot one hot) */
    pt_cap *c = g_pt_curcap;
    if (c && c->n && addr < c->e[c->n - 1].hi && hi > c->e[0].lo) {
        int i = pt_ent_first(c, addr);
        if (i < c->n && c->e[i].lo <= addr && hi <= c->e[i].hi) {
            const pt_ent *e = &c->e[i];
            if (e->ok == 1) {
                VkDeviceSize off = e->off + (addr - e->lo);
                if ((align && off % align) || pt_ub_logged(addr, size)) return 0;
                pt_arena_used(e->ar); PT_ADD(hits, 1);
                *out = (VkDescriptorBufferInfo){e->buf, off, size};
                return 1;
            }
            if (e->ok == 0) pt_unprot(addr, size, kind);
            return 0;
        }
    }
    return 0;
}
/* walker: the copy a GPU write of [addr, +size) goes to this frame (op: 1 storage, 2 texture -> buffer, 4 buffer copy,
   8 fill; align: the offset the command needs). 0: it goes to memory, after the GPU bytes of the copies it overlaps
   (call it where the command is recorded: outside a pass for transfers) */
int vk_pt_write(uint64_t addr, uint64_t size, unsigned op, VkDeviceSize align, VkDescriptorBufferInfo *out) {
    if (!(g_pt_on & PT_LAND) || !size || addr + size < addr) return 0;
    return pt_land_access(addr, addr + size, op, align, out) > 0;
}
/* walker: a GPU access no copy serves (a storage binding of unknown extent or unaligned, a writable texel buffer). A write
   goes to memory after the GPU bytes of the copies it overlaps; a read over a copy only counts (stale). */
void vk_pt_unbound(uint64_t addr, uint64_t size, int write) {
    if (!(g_pt_on & PT_LAND) || !size) return;
    uint64_t hi = addr + size < addr ? UINT64_MAX : addr + size;
    if (!atomic_load_explicit(&g_pt_nland_a, memory_order_relaxed)) { if (write) pt_mw_add(addr, hi, 1); return; }
    pt_xfer fl[PT_FLUSH_MAX]; int nfl = 0, in_pass = vk_in_pass();
    pthread_mutex_lock(&g_pt_mu);
    if (write) pt_unserved_locked(addr, hi, 1, in_pass, fl, &nfl);
    else if (addr < g_pt_land_hi && hi > g_pt_land_lo) {
        int any = 0;
        for (int j = 0; j < g_pt_nland && !any; j++) any = !g_pt_land[j].poisoned && g_pt_land[j].lo < hi && addr < g_pt_land[j].hi;
        if (any) { PT_ADD(conflicts, 1); PT_ADD(stale, 1); pt_unsafe_note(addr, hi - addr, 100); }
    }
    pthread_mutex_unlock(&g_pt_mu);
    pt_xfer_record(fl, nfl, in_pass, 1);
}
/* walker, right after vk_cpu_step() (the GPU finished everything recorded): the CPU reads or writes [addr, +n) in
   command order, so the copies over it land now */
void vk_pt_cpu_access(uint64_t addr, uint64_t n) {
    if (!(g_pt_on & PT_LAND) || !n || !atomic_load_explicit(&g_pt_nland_a, memory_order_relaxed)) return;
    uint64_t lo = addr, hi = addr + n < addr ? UINT64_MAX : addr + n, done = atomic_load(&g_done);
    int idx[PT_LANDS], k = 0;
    pthread_mutex_lock(&g_pt_mu);
    if (lo < g_pt_land_hi && hi > g_pt_land_lo)
        for (int j = 0; j < g_pt_nland; j++) { const pt_land *e = &g_pt_land[j]; if (!e->poisoned && e->lo < hi && lo < e->hi) idx[k++] = j; }
    if (k) {
        pt_sort_frames(idx, k);
        pthread_mutex_lock(&g_pool_mu);
        for (int i = 0; i < k; i++) {
            pt_land *e = &g_pt_land[idx[i]];
            if (e->ticket <= done) { if (pt_land_one(e)) PT_ADD(early, 1); else PT_ADD(dropped, 1); pt_retire(e); }
            else { e->poisoned = 1; PT_ADD(lost, 1); pt_unsafe_note(lo, hi - lo, 104); }   /* not expected: vk_cpu_step waited for all of it */
        }
        pthread_mutex_unlock(&g_pool_mu);
        pt_land_compact();
        atomic_fetch_add_explicit(&g_pt_gen, 1, memory_order_relaxed);
    }
    pthread_mutex_unlock(&g_pt_mu);
}
static void pt_gpu_wrote(uint64_t a, uint64_t n) {   /* walker (gpu_write_inner): GPU-produced bytes are never frozen */
    if (a == g_pt_clamp_addr && g_pt_clamp_n && g_pt_clamp_n < n) n = g_pt_clamp_n;
    g_pt_clamp_addr = 0;
    uint64_t hi = a + n < a ? UINT64_MAX : a + n;
    unsigned h = (unsigned)((a >> 6) ^ (a >> 20) ^ (hi * 0x9e3779b1u)) & (PT_MEMO - 1);
    if (!(g_pt_gw_memo[h][0] == a && g_pt_gw_memo[h][1] == hi)) {   /* the same range again this frame is noted once */
        g_pt_gw_memo[h][0] = a; g_pt_gw_memo[h][1] = hi;
        if (g_pt_ngw < PT_GW) { g_pt_gw[g_pt_ngw][0] = a; g_pt_gw[g_pt_ngw][1] = hi; g_pt_ngw++; }
        else g_pt_gw_lost = 1;
    }
    pt_cap *c = g_pt_curcap;
    if (c) for (int i = pt_ent_first(c, a); i < c->n && c->e[i].lo < hi; i++) if (c->e[i].ok != 2) {
        c->e[i].ok = 2; PT_ADD(gpuown, 1);   /* later reads in this submission read what the GPU wrote */
        atomic_fetch_add_explicit(&g_pt_gen, 1, memory_order_relaxed);
    }
}
void vk_pt_gpu_clamp(uint64_t addr, uint64_t n) { g_pt_clamp_addr = addr; g_pt_clamp_n = n; }
int vk_pt_texel(uint64_t addr, uint64_t size, int writable) {   /* walker: 1 when a cached texel-buffer view must be made again */
    if (!size) return 0;
    if (writable) { pt_gpu_wrote(addr, size); vk_pt_unbound(addr, size, 1); return 0; }   /* memory, after the copies it overlaps */
    uint64_t hi = addr + size;
    if (atomic_load_explicit(&g_pt_nland_a, memory_order_relaxed)) {
        pthread_mutex_lock(&g_pt_mu);
        int hit = g_pt_nland && addr < g_pt_land_hi && hi > g_pt_land_lo;
        pthread_mutex_unlock(&g_pt_mu);
        if (hit) return 1;
    }
    pt_cap *c = g_pt_curcap;
    if (c && c->n) { int i = pt_ent_first(c, addr); if (i < c->n && c->e[i].lo < hi) return 1; }
    if ((g_pt_on & PT_FREEZE) && (g_pt_kinds >> VK_LAG_TEXEL & 1) && size <= g_pt_max) pt_cand_note(addr, hi, VK_LAG_TEXEL);
    return 0;
}
/* g_pt_mu: retire the entries whose last use is complete (ticket <= fence), oldest frame first; entries of frames from
   `below` on stay. An open one lands (the bytes the GPU changed go into the game's memory); a written-back one sets its
   successors' baseline; a dropped one is counted. */
static int pt_land_done(uint64_t fence, uint64_t below) {
    int idx[PT_LANDS], k = 0;
    for (int j = 0; j < g_pt_nland; j++) if (g_pt_land[j].ticket <= fence && g_pt_land[j].frame < below) idx[k++] = j;
    if (!k) return 0;
    pt_sort_frames(idx, k);
    pthread_mutex_lock(&g_pool_mu);
    for (int i = 0; i < k; i++) {
        pt_land *e = &g_pt_land[idx[i]];
        if (!e->poisoned) { if (!pt_land_one(e)) PT_ADD(dropped, 1); }
        else if (e->flushed) pt_succ_init(e);   /* its GPU bytes reached memory on the GPU */
        else PT_ADD(dropped, 1);
        pt_retire(e);
    }
    pthread_mutex_unlock(&g_pool_mu);
    pt_land_compact();
    return k;
}
/* the game's thread, after its nvnSyncWait returned for fence (or nvnQueueFinish: UINT64_MAX): the GPU results it waited for */
void mtl_protect_land(uint64_t fence) {
    if (!(g_pt_on & PT_LAND) || !fence || !atomic_load_explicit(&g_pt_nland_a, memory_order_relaxed)) return;
    uint64_t done = atomic_load(&g_done);
    pthread_mutex_lock(&g_pt_mu);
    pt_land_done(fence < done ? fence : done, UINT64_MAX);
    pthread_mutex_unlock(&g_pt_mu);
}
int mtl_protect_unsafe_since(uint64_t *seen) {   /* TOTK_OHOS_PROTECT_STRICT: an unprotected access since the last call */
    if (!g_pt_strict) return 0;
    uint64_t u = atomic_load_explicit(&g_pt_unsafe, memory_order_relaxed);
    int r = u != *seen; *seen = u;
    return r;
}
static int pt_u64x2_cmp(const void *x, const void *y) { const uint64_t *a = x, *b = y; return a[0] < b[0] ? -1 : a[0] > b[0]; }
static int pt_overlaps(uint64_t (*r)[2], const uint64_t *mx, int n, uint64_t lo, uint64_t hi) {   /* r sorted by lo, mx: running max of hi */
    int a = 0, b = n;
    while (a < b) { int m = (a + b) >> 1; if (r[m][0] < hi) a = m + 1; else b = m; }
    return a > 0 && mx[a - 1] > lo;
}
static void pt_runs_rebuild(void) {   /* walker, g_pt_mu: the hot candidates as runs, keeping each unchanged run's copy */
    static uint64_t (*h)[3]; static int caph;
    int n = 0;
    for (int i = 0; i < PT_CAND; i++) {
        const pt_cand *c = &g_pt_cand[i];
        if (!c->used || !c->hot) continue;
        if (n == caph) { caph = caph ? 2 * caph : 1024; h = VK_REALLOC(h, (size_t)caph * sizeof *h); }
        h[n][0] = c->lo; h[n][1] = c->hi; h[n][2] = g_pt_all || !c->stamped || g_pt_wframe - c->stamped > PT_HOT_FRAMES; n++;
    }
    if (n > 1) qsort(h, (size_t)n, sizeof *h, pt_u64x2_cmp);
    int m = 0;
    for (int i = 0; i < n; i++) {
        if (m && h[i][0] < h[m - 1][1]) { if (h[i][1] > h[m - 1][1]) h[m - 1][1] = h[i][1]; h[m - 1][2] |= h[i][2]; }
        else { h[m][0] = h[i][0]; h[m][1] = h[i][1]; h[m][2] = h[i][2]; m++; }
    }
    pt_run *old = g_pt_run; int nold = g_pt_nrun;
    pt_run *nr = VK_MALLOC((size_t)(m ? m : 1) * sizeof *nr); int k = 0, at = 0;
    for (int i = 0; i < m; i++) {
        if (h[i][1] - h[i][0] > g_pt_max) { PT_ADD(big, 1); continue; }
        while (at < nold && old[at].lo < h[i][0]) at++;
        if (at < nold && old[at].lo == h[i][0] && old[at].hi == h[i][1]) { nr[k] = old[at]; nr[k].hashed = (int)h[i][2]; }
        else { memset(&nr[k], 0, sizeof nr[k]); nr[k].lo = h[i][0]; nr[k].hi = h[i][1]; nr[k].stamp = 1; nr[k].ar = -1; nr[k].hashed = (int)h[i][2]; }
        k++;
    }
    free(old); g_pt_run = nr; g_pt_nrun = k;
}
static void pt_cand_compact(void) {   /* walker: the table is 3/4 full: keep the hot and recently bound candidates */
    static pt_cand keep[PT_CAND]; int n = 0;
    for (int i = 0; i < PT_CAND; i++) {
        const pt_cand *c = &g_pt_cand[i];
        if (c->used && (c->hot || g_pt_wframe - c->seen < 60)) keep[n++] = *c;
    }
    memset(g_pt_cand, 0, sizeof g_pt_cand); g_pt_ncand = 0;
    for (int i = 0; i < n; i++) {
        unsigned h = (unsigned)(((keep[i].lo >> 4) ^ (keep[i].lo >> 20) ^ (keep[i].hi * 0x9e3779b1u)) & (PT_CAND - 1));
        for (unsigned k = 0; k < 32; k++, h = (h + 1) & (PT_CAND - 1)) if (!g_pt_cand[h].used) { g_pt_cand[h] = keep[i]; g_pt_ncand++; break; }
    }
}
static void pt_report(uint64_t n) {   /* walker, every 300 presents */
    static pt_stats last; static uint64_t last_n;
    uint64_t f = n - last_n; if (!f) return;
#define PT_D(fl) (atomic_load_explicit(&g_pt_s.fl, memory_order_relaxed) - atomic_load_explicit(&last.fl, memory_order_relaxed))
    uint64_t run_bytes = 0, arena_bytes = 0;
    pthread_mutex_lock(&g_pt_mu);
    for (int i = 0; i < g_pt_nrun; i++) run_bytes += g_pt_run[i].hi - g_pt_run[i].lo;
    for (int a = 0; a < g_pt_narenas; a++) for (int c = 0; c < g_pt_ar[a].ar.nchunk; c++) arena_bytes += g_pt_ar[a].ar.chunks[c].size;
    int nrun = g_pt_nrun, nland = g_pt_nland;
    pthread_mutex_unlock(&g_pt_mu);
    double d = (double)f;
    LOGV("[protect] frames %llu-%llu, per frame: freeze %.1f captures, %.2f copies %.3f MiB, %.1f reused, %.1f frozen reads, "
         "%.2f unprotected (budget %.2f, no arena %.2f), %.2f GPU-written since capture, %.1f KiB content-checked; "
         "land %.2f new copies %.1f KiB (%.2f chained, %.2f GPU-filled), %.1f reads/writes of copies, %.2f landed %.2f KiB changed, "
         "%.2f conflicts (%.2f landed early, %.2f written back, %.2f lost, %.2f stale reads), %.2f refused, %.2f chain breaks, %.2f dropped; "
         "hot runs %d (%.2f MiB), candidates %d (promoted %llu, demoted %llu, too big %llu), open landings %d; arenas %.1f MiB, %llu arena waits; "
         "copy sources read at the walk %.2f (%.1f KiB), %.2f to memory (GPU-written in the last 3 frames); so far %llu read at the walk, %llu to memory",
         (unsigned long long)last_n + 1, (unsigned long long)n,
         PT_D(caps) / d, PT_D(copies) / d, PT_D(copy_bytes) / 1048576.0 / d, PT_D(reused) / d, PT_D(hits) / d,
         PT_D(unprot) / d, PT_D(budget) / d, PT_D(noarena) / d, PT_D(gpuown) / d, PT_D(hash_bytes) / 1024.0 / d,
         PT_D(lands) / d, PT_D(land_bytes) / 1024.0 / d, PT_D(chained) / d, PT_D(gpufill) / d, PT_D(land_hits) / d, PT_D(landed) / d,
         PT_D(landed_bytes) / 1024.0 / d, PT_D(conflicts) / d, PT_D(early) / d, PT_D(flushed) / d, PT_D(lost) / d, PT_D(stale) / d,
         PT_D(refused) / d, PT_D(chainbreak) / d, PT_D(dropped) / d,
         nrun, run_bytes / 1048576.0, g_pt_ncand, (unsigned long long)PT_D(promoted), (unsigned long long)PT_D(demoted),
         (unsigned long long)PT_D(big), nland, arena_bytes / 1048576.0, (unsigned long long)PT_D(rot_waits),
         PT_D(walkcopy) / d, PT_D(walkcopy_bytes) / 1024.0 / d, PT_D(walkcopy_gw) / d,
         (unsigned long long)atomic_load_explicit(&g_pt_s.walkcopy, memory_order_relaxed),
         (unsigned long long)atomic_load_explicit(&g_pt_s.walkcopy_gw, memory_order_relaxed));
#undef PT_D
#define PT_COPY(fl) atomic_store_explicit(&last.fl, atomic_load_explicit(&g_pt_s.fl, memory_order_relaxed), memory_order_relaxed);
    PT_STATS(PT_COPY)
#undef PT_COPY
    last_n = n;
}
static void pt_log_frame(uint64_t frame) {   /* walker, TOTK_OHOS_PROTECT_LOG: what protected frame `frame` */
    pthread_mutex_lock(&g_pt_mu);
    LOGV("[protect] frame %llu: %d hot runs, %d open landings, %llu copy bytes, arena %d, memory writes %d (+%d last frame)%s",
         (unsigned long long)frame, g_pt_nrun, g_pt_nland, (unsigned long long)g_pt_frame_bytes, g_pt_cur,
         g_pt_nmw[g_pt_mwc], g_pt_nmw[g_pt_mwc ^ 1], g_pt_mw_lost[0] || g_pt_mw_lost[1] ? " (list overflowed)" : "");
    for (int i = 0; i < g_pt_nrun && i < 48; i++) {
        const pt_run *r = &g_pt_run[i];
        LOGV("[protect]   run %#llx+%#llx: copies so far %llu, flushed since its copy %d, content-checked %d, copy in arena %d",
             (unsigned long long)r->lo, (unsigned long long)(r->hi - r->lo), (unsigned long long)r->copies,
             r->stamp > r->copy_seq, r->hashed, r->ar);
    }
    for (int j = 0; j < g_pt_nland && j < 48; j++) {
        const pt_land *e = &g_pt_land[j];
        LOGV("[protect]   landing #%llu %#llx+%#llx of frame %llu: op %u, %llu uses, last ticket %llu, written %#llx+%#llx (ticket %llu), "
             "chained %d (from #%llu), no chain %d, GPU-filled ranges %d, poisoned %d, written back %d",
             (unsigned long long)e->id, (unsigned long long)e->lo, (unsigned long long)(e->hi - e->lo), (unsigned long long)e->frame, e->op,
             (unsigned long long)e->uses, (unsigned long long)e->ticket, (unsigned long long)(e->whi > e->wlo ? e->wlo : 0),
             (unsigned long long)(e->whi > e->wlo ? e->whi - e->wlo : 0), (unsigned long long)e->wticket,
             e->chained, (unsigned long long)e->from, e->nochain, e->ngf, e->poisoned, e->flushed);
    }
    pthread_mutex_unlock(&g_pt_mu);
    static const char *const names[VK_LAG_KIND_COUNT] = {"vertex", "index", "storage", "uniform", "pointer", "texel", "copy", "other"};
    for (int i = 0; i < g_pt_nunlog; i++) {
        int k = g_pt_unlog[i].kind;
        const char *what = k >= 0 && k < VK_LAG_KIND_COUNT ? names[k] : k >= 300 ? "landable access refused a copy" :
                           k >= 200 ? "chain break" : k >= 100 ? "access no copy could serve" : "?";
        LOGV("[protect]   unprotected: %s%s (op %d) %#llx+%#llx", what, k >= 0 && k < VK_LAG_KIND_COUNT ? " read" : "", k >= 100 ? k % 100 : -1,
             (unsigned long long)g_pt_unlog[i].addr, (unsigned long long)g_pt_unlog[i].size);
    }
    g_pt_nunlog = 0;
}
/* walker (mtl_present, under the recording lock), at present n (frame n + 1 begins): hot set from this frame's flushes
   and GPU writes, the next arena, reports */
void vk_pt_present(uint64_t n) {
    static uint64_t st[PT_STAMPS][2], stmx[PT_STAMPS], gwmx[PT_GW];
    int nst = 0, lost = 0;
    pthread_mutex_lock(&g_pt_mu);
    uint64_t from = g_pt_st_done;
    if (g_pt_st_n - from > PT_STAMPS) { from = g_pt_st_n - PT_STAMPS; lost = 1; }
    for (uint64_t s = from; s < g_pt_st_n; s++) { st[nst][0] = g_pt_st[s % PT_STAMPS].lo; st[nst][1] = g_pt_st[s % PT_STAMPS].hi; nst++; }
    g_pt_st_done = g_pt_st_n;
    pthread_mutex_unlock(&g_pt_mu);
    int changed = 0;
    if (g_pt_on & PT_FREEZE) {
        if (nst > 1) qsort(st, (size_t)nst, sizeof *st, pt_u64x2_cmp);
        for (int i = 0; i < nst; i++) stmx[i] = i && stmx[i - 1] > st[i][1] ? stmx[i - 1] : st[i][1];
        if (g_pt_ngw > 1) qsort(g_pt_gw, (size_t)g_pt_ngw, sizeof *g_pt_gw, pt_u64x2_cmp);
        for (int i = 0; i < g_pt_ngw; i++) gwmx[i] = i && gwmx[i - 1] > g_pt_gw[i][1] ? gwmx[i - 1] : g_pt_gw[i][1];
        uint64_t f = g_pt_wframe;
        for (int i = 0; i < PT_CAND; i++) {
            pt_cand *c = &g_pt_cand[i];
            if (!c->used) continue;
            if (g_pt_ngw && pt_overlaps(g_pt_gw, gwmx, g_pt_ngw, c->lo, c->hi)) c->gpuw = f;
            if (nst && pt_overlaps(st, stmx, nst, c->lo, c->hi)) c->stamped = f;
            int hot = !c->big && f - c->seen <= PT_HOT_FRAMES && (!c->gpuw || f - c->gpuw > PT_HOT_FRAMES) &&
                      ((c->stamped && f - c->stamped <= PT_HOT_FRAMES) || g_pt_all || (g_pt_small && c->hi - c->lo <= g_pt_small));
            if (hot && !c->hot && (g_pt_gw_lost || lost)) hot = 0;   /* this frame's GPU writes or flushes were not all seen: promote nothing */
            if (hot != c->hot) { c->hot = (uint8_t)hot; changed = 1; if (hot) PT_ADD(promoted, 1); else PT_ADD(demoted, 1); }
        }
        if (g_pt_ncand >= PT_CAND * 3 / 4) pt_cand_compact();
    }
    {   /* keep this frame's GPU writes for two more frames (pt_gw_recent): with two frames in flight they may not have run yet */
        int s = g_pt_gwp_cur = (g_pt_gwp_cur + 1) % 2;
        memcpy(g_pt_gwp[s], g_pt_gw, (size_t)g_pt_ngw * sizeof g_pt_gw[0]); g_pt_ngwp[s] = g_pt_ngw; g_pt_gwp_lost[s] = g_pt_gw_lost;
    }
    g_pt_ngw = 0; g_pt_gw_lost = 0; memset(g_pt_gw_memo, 0, sizeof g_pt_gw_memo);
    g_pt_mwc ^= 1; g_pt_nmw[g_pt_mwc] = 0; g_pt_mw_lost[g_pt_mwc] = 0;   /* this frame's memory writes become the last frame's */
    g_pt_mw_lo[g_pt_mwc] = UINT64_MAX; g_pt_mw_hi[g_pt_mwc] = 0; memset(g_pt_mw_memo, 0, sizeof g_pt_mw_memo);
    uint64_t done = atomic_load(&g_done);
    pthread_mutex_lock(&g_pt_mu);
    if (changed) pt_runs_rebuild();
    if (g_pt_nland) {   /* safety valve: copies the game never waited for land three frames late; ones still running after 16 are dropped */
        pt_land_done(done, g_pt_frame > 3 ? g_pt_frame - 3 : 0);
        for (int j = 0; j < g_pt_nland; j++) if (g_pt_land[j].frame + 16 < g_pt_frame) g_pt_land[j].poisoned = 1;
        pt_land_done(UINT64_MAX, g_pt_frame > 16 ? g_pt_frame - 16 : 0);
    }
    /* the next frame's arena: one nothing holds, after its last GPU use */
    int next = -1;
    for (int pass = 0; pass < 2 && next < 0; pass++)   /* first one the GPU is already done with, else the oldest free one */
        for (int k = 1; k <= g_pt_narenas && next < 0; k++) {
            int a = ((g_pt_cur < 0 ? 0 : g_pt_cur) + k) % g_pt_narenas;
            uint64_t t = atomic_load_explicit(&g_pt_ar[a].ticket, memory_order_relaxed);
            if (a != g_pt_cur && !g_pt_ar[a].refs && (pass || t <= done)) next = a;
        }
    if (next >= 0) {
        pt_arena *a = &g_pt_ar[next];
        a->gen++;   /* runs stop reusing copies there */
        uint64_t t = atomic_load_explicit(&a->ticket, memory_order_relaxed);
        if (t > done) {   /* still on the GPU: wait for it (counted: a wait here costs the overlap) */
            PT_ADD(rot_waits, 1);
            pthread_mutex_unlock(&g_pt_mu);
            pthread_mutex_lock(&g_mu); uint64_t published = g_fence; pthread_mutex_unlock(&g_mu);
            int ok = t <= published && mtl_wait(t, UINT64_C(2000000000));
            pthread_mutex_lock(&g_pt_mu);
            if (!ok || a->refs) next = -1;
        }
        if (next >= 0) {
            /* without g_pt_mu: a released chunk takes g_tex_mu (vk_res_buffer_released), which is taken before g_pt_mu.
               Nothing else touches this arena meanwhile: no capture or landing holds it, it is not g_pt_cur, and its
               generation already changed (no run copy there is valid any more). */
            pthread_mutex_unlock(&g_pt_mu);
            arena_reset(&a->ar, 2);
#ifdef TOTK_OHOS
            arena_cap_bytes(&a->ar, g_pt_budget, VK_MEMORY_SNAPSHOT);
#endif
            pthread_mutex_lock(&g_pt_mu);
            atomic_store_explicit(&a->ticket, 0, memory_order_relaxed);
        }
    }
    g_pt_cur = next;
    g_pt_frame = n + 1; g_pt_frame_bytes = 0;
    pthread_mutex_unlock(&g_pt_mu);
    if (next < 0) { static unsigned said; if (said++ < 8) LOGV("[protect] frame %llu: no free arena (all held or still on the GPU): nothing is copied this frame", (unsigned long long)(n + 1)); }
    if (g_pt_wframe >= g_pt_log_from && g_pt_wframe < g_pt_log_to) pt_log_frame(g_pt_wframe);
    g_pt_wframe = n + 1;
    if (n % 300 == 0) pt_report(n);
}
/* Frame lag exposes every direct read of CPU-written pool memory, not just UBO descriptors. Freeze only the ranges
   actually consumed, on the walking thread before submit returns. GPU-produced ranges must not be replaced with stale
   CPU bytes: leave them on the queue timeline. Inline updates use exactly the uniform snapshot/overlay semantics.
   The submission arena (not the chunk/slot arena) keeps copies alive across chunk submissions and recorder jobs. */
int vk_lag_capture_init(void) {
    int current = atomic_load_explicit(&vk_lag_capture_state, memory_order_acquire);
    if (current >= 0) return current;
    const char *capture = getenv("TOTK_VK_LAG_CAPTURE");
    int on = runtime_frame_lag_requested() && !(capture && !atoi(capture)), expected = -1;
    /* A delayed initializer must not overwrite a completed pressure cutover or another initializer. */
    if (!atomic_compare_exchange_strong_explicit(&vk_lag_capture_state, &expected, on,
                                                 memory_order_acq_rel, memory_order_acquire)) return expected;
    if (on) LOGV("lag protection: read-only guest ranges protected before submit returns (TOTK_VK_LAG_CAPTURE=0 disables)");
    return on;
}
static int lag_wait_on(void) {
    if (!vk_lag_capture_on()) return 0;
    if (g_lag_wait < 0) {
        const char *mode = getenv("TOTK_VK_LAG_MODE");
        if (mode && strcmp(mode, "wait") && strcmp(mode, "copy")) DIE("TOTK_VK_LAG_MODE must be wait or copy");
        g_lag_wait = !(mode && !strcmp(mode, "copy"));
        LOGV("lag protection mode: %s (TOTK_VK_LAG_MODE)", g_lag_wait ? "wait on actual CPU reuse" : "eager copy");
    }
    return g_lag_wait;
}
static int lag_hybrid_on(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("TOTK_VK_LAG_HYBRID"); on = lag_wait_on() && !(e && !atoi(e)); }
    return on;
}
static uint64_t lag_hot_limit(void) {
    static uint64_t bytes;
    if (!bytes) { const char *e = getenv("TOTK_VK_LAG_HOT_MAX_KB"); unsigned long n = e ? strtoul(e, NULL, 10) : 256; bytes = n && n <= 1048576 ? (uint64_t)n * 1024 : 256u * 1024; }
    return bytes;
}
typedef struct { uint64_t lo, hi; unsigned reason; uint64_t max_range; } lag_pending;
static int lag_pending_cmp(const void *x, const void *y) {
    const lag_pending *a = x, *b = y;
    return a->lo < b->lo ? -1 : a->lo > b->lo;
}
static lag_pending *g_lag_pending; static int g_lag_npending, g_lag_cpending;
static uint64_t g_lag_epoch = 1;
static _Atomic uint64_t g_lag_guard_bytes, g_lag_guard_ns, g_lag_guard_ranges;
static uint64_t g_lag_bytes, g_lag_ns, g_lag_ranges, g_lag_gpu;
static _Atomic uint64_t g_lag_failed_ticket[RING_MAX], g_lag_record_bytes, g_lag_record_ranges;
/* At most RING ticket owners exist, including the current walking slot. A cell is not reused until that
   slot's jobs/registration and GPU fence retire. A later ticket cannot hide an earlier failed projection. */
static void lag_guard_failed(uint64_t ticket) {
    if (ticket) atomic_store_explicit(&g_lag_failed_ticket[ticket % RING], ticket, memory_order_relaxed);
}
static int lag_guard_has_failed(uint64_t ticket) {
    return atomic_load_explicit(&g_lag_failed_ticket[ticket % RING], memory_order_relaxed) == ticket;
}
static int lag_guard_take_failed(uint64_t ticket) {
    return atomic_compare_exchange_strong_explicit(&g_lag_failed_ticket[ticket % RING], &ticket, 0,
                                                    memory_order_relaxed, memory_order_relaxed);
}
#ifdef TOTK_VK_TESTS
int vk_record_test_take_failure(uint64_t ticket) { return lag_guard_take_failed(ticket); }
#endif
void vk_record_lag_fail(uint64_t ticket) { lag_guard_failed(ticket); }
uint64_t vk_record_lag_ticket(void) { return lag_wait_on() ? g_reserved_fence + 1 : 0; }
int vk_record_lag_enabled(void) { return g_lag_wait == 1 && vk_lag_capture_on(); }
/* Recording freezes resolved owner/view/alias VA intents without arming them. Only the separate submit
   registration phase may publish their exact ticket; it performs no span/map or snapshot lookup.
   Lavapipe still needs complete producer exclusions because it stores through guest host VAs. */
typedef struct { uint64_t lo, hi, ticket, max_range; uint32_t reason; } lag_record_pending;
static lag_record_pending *g_lag_record_pending;
static size_t g_lag_record_count, g_lag_record_capacity;
static pthread_mutex_t g_lag_record_mu = PTHREAD_MUTEX_INITIALIZER;
#ifdef TOTK_VK_TESTS
static int g_record_test_native_guards;
void vk_record_test_native_guards(int enabled) { g_record_test_native_guards = enabled; }
#endif
static int lag_software_gpu(void) {
#ifdef TOTK_VK_TESTS
    if (g_record_test_native_guards) return 0;
#endif
    return V.props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU;
}
int vk_record_lag_native(void) { return !lag_software_gpu(); }
static int lag_record_defer(const vk_record_lag_intent *intents, uint32_t count, uint64_t ticket) {
    int ok = 1;
    pthread_mutex_lock(&g_lag_record_mu);
    size_t end = g_lag_record_count + count;
    if (end < g_lag_record_count || end > SIZE_MAX / sizeof *g_lag_record_pending) ok = 0;
    if (ok && end > g_lag_record_capacity) {
        size_t cap = g_lag_record_capacity ? g_lag_record_capacity : 256;
        while (cap < end && cap <= SIZE_MAX / 2 / sizeof *g_lag_record_pending) cap *= 2;
        if (cap < end) cap = end;
        lag_record_pending *p = realloc(g_lag_record_pending, cap * sizeof *p);
        if (p) { g_lag_record_pending = p; g_lag_record_capacity = cap; } else ok = 0;
    }
    if (ok) for (uint32_t i = 0; i < count; i++) {
        const vk_record_lag_intent *r = &intents[i];
        g_lag_record_pending[g_lag_record_count++] = (lag_record_pending){r->lo, r->hi, ticket, r->max_range, r->reason};
    }
    pthread_mutex_unlock(&g_lag_record_mu);
    return ok;
}
typedef struct {
    nvn_dirty_lag_batch *batch;
    vk_record_lag_intent **out; uint32_t *count, *capacity;
    uint64_t projected, epoch;
} lag_projection;
static int lag_project_span(const span_t *s, VkBuffer buffer, uint64_t offset, uint64_t size,
                            uint32_t reason, lag_projection *p, int *covered) {
    if (s->buf != buffer || s->end <= s->lo || s->off + s->end - s->lo < s->off) return 1;
    uint64_t lo = offset > s->off ? offset : s->off, end = s->off + s->end - s->lo;
    uint64_t hi = offset + size < end ? offset + size : end;
    if (lo >= hi) return 1;
    if (lo == offset && hi == offset + size) *covered = 1;
    uint64_t address = s->lo + lo - s->off;
    p->projected++;
    if (p->batch)
        return nvn_dirty_lag_batch_add_before(p->batch, (const void *)(uintptr_t)address, hi - lo,
                                              reason | NVN_DIRTY_LAG_ALIAS, size, p->epoch);
    if (*p->count == *p->capacity) {
        size_t cap = *p->capacity ? (size_t)*p->capacity * 2 : 32;
        vk_record_lag_intent *v = cap <= UINT32_MAX && cap <= SIZE_MAX / sizeof *v ?
            realloc(*p->out, cap * sizeof *v) : NULL;
        if (!v) return 0;
        *p->out = v; *p->capacity = (uint32_t)cap;
    }
    (*p->out)[(*p->count)++] = (vk_record_lag_intent){address, address + hi - lo, size, reason | NVN_DIRTY_LAG_ALIAS};
    return 1;
}
static int lag_project_indexed(const vk_record_spans *spans, VkBuffer buffer, uint64_t offset, uint64_t size,
                               uint32_t reason, lag_projection *p) {
    if (!size) return 1;
    if (!spans || !buffer || offset + size < offset || (spans->count && !spans->buffer_heads)) return 0;
    int covered = 0;
    p->epoch = spans->dirty_epoch; /* a recycled alias cannot become the identity of an older frozen packet */
    /* Each bucket retains every physical owner, view and alias, independently of VA lookup winners and OFF=span. */
    uint32_t at = spans->count ? spans->buffer_heads[record_buffer_hash(buffer) & spans->buffer_mask] : 0;
    while (at) {
        uint32_t i = at - 1; at = spans->buffer_next[i];
        if (!lag_project_span(&spans->span[i], buffer, offset, size, reason, p, &covered)) return 0;
    }
    return covered;   /* union-only coverage was never sufficient: a resolved source span must cover the whole input */
}
int vk_record_lag_collect(const vk_record_spans *spans, VkBuffer buffer, uint64_t offset, uint64_t size,
                          uint64_t ticket, uint32_t reason, nvn_dirty_lag_batch *batch,
                          vk_record_lag_intent **out, uint32_t *count, uint32_t *capacity, uint64_t *projected) {
    if (!vk_record_lag_enabled() || !size) return 1;
    lag_projection p = {.batch = batch, .out = out, .count = count, .capacity = capacity};
    int ok = ticket && lag_project_indexed(spans, buffer, offset, size, reason, &p);
    if (projected) *projected += p.projected;
    if (!ok) lag_guard_failed(ticket);
    return ok;
}
int vk_record_lag_project(const vk_record_spans *spans, VkBuffer buffer, uint64_t offset, uint64_t size,
                          uint64_t ticket, uint32_t reason, vk_record_lag_intent **out, uint32_t *count, uint32_t *capacity) {
    return vk_record_lag_collect(spans, buffer, offset, size, ticket, reason, NULL, out, count, capacity, NULL);
}
void vk_record_lag_register(const vk_record_lag_intent *intents, uint32_t count, uint64_t ticket) {
    /* Only software-GPU callers retain VA vectors. Native callers collect and globally merge units before arming. */
    if (!lag_record_defer(intents, count, ticket)) lag_guard_failed(ticket);
}
void vk_record_lag_batch_register(const nvn_dirty_lag_batch *batch, uint64_t ticket, unsigned shard, unsigned nshards) {
    if (!nvn_dirty_lag_batch_register(batch, ticket, shard, nshards)) lag_guard_failed(ticket);
    if (!shard) {   /* aggregate once per immutable ticket batch, never atomics or clocks per unit */
        atomic_fetch_add_explicit(&g_lag_record_bytes, nvn_dirty_lag_batch_bytes(batch), memory_order_relaxed);
        atomic_fetch_add_explicit(&g_lag_record_ranges, nvn_dirty_lag_batch_count(batch), memory_order_relaxed);
    }
}
#ifdef TOTK_VK_TESTS
/* Synthetic host-only projection fixtures need neither a Vulkan device nor a dirty-tracking mapping. */
void vk_record_test_spans_destroy(vk_record_spans *s) {
    if (!s) return;
    free(s->span); free(s->segment); free(s->buffer_heads); free(s);
}
vk_record_spans *vk_record_test_spans_create(const vk_record_test_span *input, uint32_t count, int linear) {
    if (count > INT32_MAX) return NULL;
    vk_record_spans *s = calloc(1, sizeof *s); if (!s) return NULL;
    s->count = (int)count; s->linear = linear;
    if (count) {
        s->span = malloc((size_t)count * sizeof *s->span);
        if (!s->span) { vk_record_test_spans_destroy(s); return NULL; }
        for (uint32_t i = 0; i < count; i++)
            s->span[i] = (span_t){input[i].lo, input[i].hi, input[i].buffer, input[i].offset};
    }
    if (!record_spans_index(s)) { vk_record_test_spans_destroy(s); return NULL; }
    return s;
}
int vk_record_test_project(const vk_record_spans *spans, VkBuffer buffer, uint64_t offset, uint64_t size,
                            uint32_t reason, int reference, vk_record_lag_intent **out, uint32_t *count, uint32_t *capacity) {
    lag_projection p = {.out = out, .count = count, .capacity = capacity};
    if (!reference) return lag_project_indexed(spans, buffer, offset, size, reason, &p);
    /* The old complete scan exists only in tests. Keep its exact coverage and partial-overlap semantics as an oracle. */
    if (!size) return 1;
    if (!spans || !buffer || offset + size < offset) return 0;
    int covered = 0;
    for (int i = 0; i < spans->count; i++)
        if (!lag_project_span(&spans->span[i], buffer, offset, size, reason, &p, &covered)) return 0;
    return covered;
}
#endif
/* Per-binding clocks, including copy and sampled classification timing, are opt-in.
   Chunk registration remains timed; byte/count diagnostics never require a clock. */
static int lag_timing_on(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("TOTK_VK_LAG_TIMING"); on = e && atoi(e); }
    return on;
}
static void lag_guard_mark(uint64_t addr, uint64_t size, unsigned reason) {
    if (!lag_wait_on() || !addr || !size || addr + size < addr) return;
    if (!lag_software_gpu() && vk_record_enabled()) {
        VkBuffer buffer; uint64_t offset;
        if (vk_resolve(addr, size, &buffer, &offset)) vk_record_lag_guard(buffer, offset, size, reason);
        return;   /* frozen guard-only jobs also cover primary copies, uploads and direct uniform writes */
    }
    if (!nvn_dirty_tracked((const void *)(uintptr_t)addr, (size_t)size)) {
        VkBuffer b; uint64_t off;
        if (vk_resolve(addr, size, &b, &off)) { lag_guard_failed(g_reserved_fence + 1); RED(4, "lag guard: untracked writable GPU range forces its submission to complete"); }
        return;
    }
    enum { NG = 4096 };
    static struct { uint64_t addr, size, epoch; unsigned reason; } marked[NG];
    unsigned h = lag_hash(addr, size, 0) % NG;
    if (marked[h].addr == addr && marked[h].size == size && marked[h].epoch == g_lag_epoch &&
        (marked[h].reason & reason) == reason) return;
    uint64_t start = lag_timing_on() ? host_now_ns() : 0;
    if (g_lag_npending == g_lag_cpending) {
        g_lag_cpending = g_lag_cpending ? g_lag_cpending * 2 : 256;
        g_lag_pending = VK_REALLOC(g_lag_pending, (size_t)g_lag_cpending * sizeof *g_lag_pending);
    }
    g_lag_pending[g_lag_npending++] = (lag_pending){addr, addr + size, reason, size};
    if (marked[h].addr == addr && marked[h].size == size && marked[h].epoch == g_lag_epoch) marked[h].reason |= reason;
    else { marked[h].addr = addr; marked[h].size = size; marked[h].epoch = g_lag_epoch; marked[h].reason = reason; }
    if (start) g_lag_guard_ns += host_now_ns() - start;
}
/* The alias graph changes only when pool mappings change. Reuse its exact range projection instead of scanning every
   virtual mapping for every frame's merged guards. Cached arrays are walking-thread-owned, like the guard list. */
typedef struct { uint64_t lo, hi, gen; rng_t *r; int n, cap; } lag_alias_cache;
static const rng_t *lag_alias_ranges(rng_t r, int *count) {
    enum { NA = 2048 };
    static lag_alias_cache cache[NA];
    unsigned h = (unsigned)((r.lo >> 12) ^ (r.lo >> 28) ^ (r.hi >> 6)) % NA;
    lag_alias_cache *c = &cache[h];
    uint64_t gen = atomic_load_explicit(&g_span_gen, memory_order_acquire);
    if (c->lo == r.lo && c->hi == r.hi && c->gen == gen) { *count = c->n; return c->r; }
    pthread_mutex_lock(&g_pool_mu);
    c->n = 0;
    for (uint64_t addr = r.lo; addr < r.hi; ) {
        VkBuffer b; uint64_t off, avail;
        if (!resolve_span(addr, 1, &b, &off, &avail)) { addr += page_sz(); continue; }
        uint64_t len = r.hi - addr; if (len > avail) len = avail;
        for (int i = 0; i < g_nalias; i++) {
            const alias_ent *a = &g_alias[i];
            if (a->owner->buf != b) continue;
            uint64_t lo = off > a->off ? off : a->off, end = a->off + a->end - a->va;
            uint64_t hi = off + len < end ? off + len : end;
            if (lo < hi) {
                if (c->n + 2 > c->cap) { c->cap = c->cap ? c->cap * 2 : 64; c->r = VK_REALLOC(c->r, (size_t)c->cap * sizeof *c->r); }
                c->r[c->n++] = (rng_t){a->va + lo - a->off, a->va + hi - a->off};
                if (addr >= a->va && addr < a->end && a->off + addr - a->va == off)
                    c->r[c->n++] = (rng_t){a->owner->base + off, a->owner->base + off + len};
            }
        }
        addr += len;
    }
    c->lo = r.lo; c->hi = r.hi; c->gen = atomic_load_explicit(&g_span_gen, memory_order_relaxed);
    pthread_mutex_unlock(&g_pool_mu);
    *count = c->n; return c->r;
}
static int lag_software_excluded(uint64_t lo, uint64_t hi);
static void lag_guard_aliases(rng_t r, uint64_t fence, unsigned reason, uint64_t max_range) {
    int n; const rng_t *aliases = lag_alias_ranges(r, &n);
    for (int i = 0; i < n; i++) {
        if (lag_software_gpu()) {
            uintptr_t lo, hi;
            if (nvn_dirty_lag_bounds((void *)(uintptr_t)aliases[i].lo, aliases[i].hi - aliases[i].lo, &lo, &hi) &&
                lag_software_excluded(lo, hi)) continue;
        }
        if (nvn_dirty_lag_registered((void *)(uintptr_t)aliases[i].lo, aliases[i].hi - aliases[i].lo, fence)) continue;
        totk_ft_count(FT_COUNT_GUARD_REGS, 1);
        if (!nvn_dirty_lag_register((void *)(uintptr_t)aliases[i].lo, aliases[i].hi - aliases[i].lo,
                                   fence, reason | NVN_DIRTY_LAG_ALIAS, max_range)) {
            lag_guard_failed(fence);
            RED(4, "lag guard: pool alias untracked/busy or re-arm failed; its submission must complete before reuse");
        }
    }
}
/* Most bindings already name their physical pool. A virtual-pool binding learns from the physical CPU writer too;
   cache buffer->owner instead of scanning all virtual mappings for every cold, read-only range each frame. */
static uint64_t lag_physical_window(uint64_t addr, uint64_t size, uint64_t *lo, uint64_t *hi) {
    enum { NP = 4096 };
    static struct { uint64_t lo, hi, gen, physical; } ranges[NP];
    /* Fixed windows are hash/containment boundaries, never dirty-unit identities. Elementary segments clip
       them: a containing pool can lose to another mapping partway through its address span. */
    unsigned slot = lag_hash(addr & ~UINT64_C(65535), 0, 0) % NP;
    __typeof__(ranges[0]) *r = &ranges[slot];
    uint64_t spans = atomic_load_explicit(&g_span_gen, memory_order_acquire);
    if (r->gen == spans && addr >= r->lo && addr < r->hi && size <= r->hi - addr) {
        *lo = r->physical; *hi = r->physical + r->hi - r->lo;
        return r->physical + addr - r->lo;
    }
    *lo = *hi = 0;
    VkBuffer b; uint64_t off, avail, physical = addr;
    if (vk_resolve_span(addr, &b, &off, &avail) && size <= avail) {
        uint64_t base = lag_owner_base(b);
        if (base) {
            physical = base + off;
            *lo = physical; *hi = physical + size; /* the full request resolved even across a breakpoint */
            if (!span_linear() && T_span.gen == spans) {
                unsigned hint = (unsigned)((addr >> 12) ^ (addr >> 24)) & (SPAN_HINTS - 1);
                int at = T_span.hint[hint];
                if (span_hint_matches(T_span.seg, T_span.nseg, addr, at)) {
                    uint64_t a = addr & ~UINT64_C(65535), z = a + 65536;
                    if (a < T_span.seg[at - 1].lo) a = T_span.seg[at - 1].lo;
                    if (z > T_span.seg[at].lo) z = T_span.seg[at].lo;
                    *r = (__typeof__(*r)){a, z, spans, physical - (addr - a)};
                    if (size <= z - addr) { *lo = r->physical; *hi = r->physical + z - a; }
                }
            }
        }
    }
    return physical;
}
static uint64_t lag_physical(uint64_t addr, uint64_t size) {
    uint64_t lo, hi;
    return lag_physical_window(addr, size, &lo, &hi);
}
static int lag_range_hot(uint64_t addr, uint64_t size) {
    if (!lag_hybrid_on() || size > lag_hot_limit()) return 0;
    int hot = nvn_dirty_lag_hot((const void *)(uintptr_t)addr, size);
    if (!hot) {
        uint64_t physical = lag_physical(addr, size);
        hot = physical != addr && nvn_dirty_lag_hot((const void *)(uintptr_t)physical, size);
    }
    return hot;
}
/* Protect every known CPU writer, not only the descriptor VA. The global dirty token is a content token only
   while all writer aliases remain watched. Failure/untracked input permits a copy, but never reusable authority.
   Ticket zero is the existing write-watch operation, not registration of a future GPU read. */
static int lag_watch_copy(uint64_t addr, uint64_t size) {
    int watched = nvn_dirty_lag_protect((const void *)(uintptr_t)addr, size, 0);
    int n; const rng_t *aliases = lag_alias_ranges((rng_t){addr, addr + size}, &n);
    for (int i = 0; i < n; i++)
        watched &= nvn_dirty_lag_protect((const void *)(uintptr_t)aliases[i].lo, aliases[i].hi - aliases[i].lo, 0);
    return watched;
}
static uint64_t g_lag_copy_unwatched;
uint64_t vk_lag_copy_unwatched(void) { return g_lag_copy_unwatched; }
/* Walk-only software exclusions, assembled after joining workers. A producer discovered after an earlier packet
   must still exclude the whole dirty unit, including every frozen projected VA, before any future ticket is armed. */
static rng_t *g_lag_software_exclusions;
static size_t g_lag_software_count, g_lag_software_capacity;
static int lag_software_exclude(uint64_t lo, uint64_t hi) {
    uintptr_t a, b;
    if (!nvn_dirty_lag_bounds((const void *)(uintptr_t)lo, hi - lo, &a, &b)) return 1;
    if (g_lag_software_count == g_lag_software_capacity) {
        size_t cap = g_lag_software_capacity ? 2 * g_lag_software_capacity : 256;
        rng_t *p = cap > g_lag_software_capacity && cap <= SIZE_MAX / sizeof *p ?
            realloc(g_lag_software_exclusions, cap * sizeof *p) : NULL;
        if (!p) return 0;
        g_lag_software_exclusions = p; g_lag_software_capacity = cap;
    }
    g_lag_software_exclusions[g_lag_software_count++] = (rng_t){a, b};
    return 1;
}
static int lag_software_exclude_aliases(uint64_t lo, uint64_t hi) {
    if (!lag_software_exclude(lo, hi)) return 0;
    int count; const rng_t *aliases = lag_alias_ranges((rng_t){lo, hi}, &count);
    for (int i = 0; i < count; i++) if (!lag_software_exclude(aliases[i].lo, aliases[i].hi)) return 0;
    return 1;
}
static int lag_software_build_exclusions(uint64_t done) {
    g_lag_software_count = 0;
    int ok = 1;
    pthread_mutex_lock(&g_gw_mu);
    for (int i = 0; ok && i < g_ngw; i++)
        if (!g_gw[i].fence || g_gw[i].fence > done) ok = lag_software_exclude_aliases(g_gw[i].lo, g_gw[i].hi);
    pthread_mutex_unlock(&g_gw_mu);
    for (int i = 0; ok && i < g_nub; i++)
        if (g_ub[i].flushed && (!g_ub[i].fence || g_ub[i].fence > done)) ok = lag_software_exclude_aliases(g_ub[i].lo, g_ub[i].hi);
    for (size_t i = 0; ok && i < g_lag_record_count; i++) {
        const lag_record_pending *r = &g_lag_record_pending[i];
        if (r->reason & (NVN_DIRTY_LAG_GPU | NVN_DIRTY_LAG_WRITABLE)) ok = lag_software_exclude(r->lo, r->hi);
    }
    if (!ok) return 0;   /* no software registration may proceed with incomplete exclusions */
    if (g_lag_software_count > 1)
        qsort(g_lag_software_exclusions, g_lag_software_count, sizeof *g_lag_software_exclusions, rng_cmp);
    size_t count = 0;
    for (size_t i = 0; i < g_lag_software_count; i++) {
        rng_t r = g_lag_software_exclusions[i];
        if (count && r.lo <= g_lag_software_exclusions[count - 1].hi) {
            if (r.hi > g_lag_software_exclusions[count - 1].hi) g_lag_software_exclusions[count - 1].hi = r.hi;
        } else g_lag_software_exclusions[count++] = r;
    }
    g_lag_software_count = count;
    return 1;
}
static int lag_software_excluded(uint64_t lo, uint64_t hi) {
    size_t a = 0, b = g_lag_software_count;
    while (a < b) { size_t m = a + (b - a) / 2; if (g_lag_software_exclusions[m].hi <= lo) a = m + 1; else b = m; }
    return a < g_lag_software_count && g_lag_software_exclusions[a].lo < hi;
}
static uint64_t g_lag_hot_bytes, g_lag_hot_ranges;
/* Walking-only projection; no protection/ticket is armed until the sealed owner joins all recording jobs. */
static void lag_guard_seal(unsigned slot, uint64_t fence) {
    uint64_t start = host_now_ns(); nvn_dirty_lag_batch *walking = &g_ring[slot].walking_guards;
    static uint64_t sampled_spans;
    uint64_t generation = atomic_load_explicit(&g_span_gen, memory_order_acquire);
    if (generation != sampled_spans) { nvn_dirty_lag_vma_request(0); sampled_spans = generation; }
    nvn_dirty_lag_batch_reset(walking);
    for (int i = 0; i < g_lag_npending; i++) {
        const lag_pending *r = &g_lag_pending[i]; uintptr_t lo, hi;
        if (!nvn_dirty_lag_bounds((const void *)(uintptr_t)r->lo, r->hi - r->lo, &lo, &hi)) continue;
        if (!nvn_dirty_lag_batch_add(walking, (const void *)(uintptr_t)lo, hi - lo, r->reason, r->max_range))
            lag_guard_failed(fence);
        int count; const rng_t *aliases = lag_alias_ranges((rng_t){lo, hi}, &count);
        for (int a = 0; a < count; a++)
            if (!nvn_dirty_lag_batch_add(walking, (const void *)(uintptr_t)aliases[a].lo,
                                        aliases[a].hi - aliases[a].lo, r->reason | NVN_DIRTY_LAG_ALIAS, r->max_range))
                lag_guard_failed(fence);
    }
    g_lag_npending = 0; g_lag_epoch++;
    g_lag_guard_ns += host_now_ns() - start;
}
/* Sole sealed-slot owner: explicit identities, immutable units, and no walking lock/map/log accesses. */
static int lag_guard_submit_native(unsigned slot, uint64_t fence, int background) {
    const char *previous_role = nvn_dirty_lag_thread_role_get();
    nvn_dirty_lag_thread_role("submit-registration"); uint64_t start = host_now_ns();
    nvn_dirty_lag_batch *walking = &g_ring[slot].walking_guards;
    vk_record_lag_submit(slot, fence, walking, background);
    if (!vk_record_enabled()) {
        if (!nvn_dirty_lag_batch_seal(walking)) lag_guard_failed(fence);
        vk_record_lag_batch_register(walking, fence, 0, 1);
        if (background) {
            if (g_ring[slot].ft_frame)
                totk_ft_count_frame(g_ring[slot].ft_frame, FT_COUNT_GUARD_REGS, nvn_dirty_lag_batch_count(walking));
        } else if (totk_ft_work_frame())
            totk_ft_count(FT_COUNT_GUARD_REGS, nvn_dirty_lag_batch_count(walking));
    }
    g_lag_guard_bytes += atomic_exchange_explicit(&g_lag_record_bytes, 0, memory_order_relaxed);
    g_lag_guard_ranges += atomic_exchange_explicit(&g_lag_record_ranges, 0, memory_order_relaxed);
    g_lag_guard_ns += host_now_ns() - start;
    if (nvn_dirty_lag_vma_pressure()) lag_guard_failed(fence);
    int failed = lag_guard_take_failed(fence);
    nvn_dirty_lag_thread_role(previous_role); return failed;
}
/* Software Vulkan can write through host VAs. Its producer exclusions still require the complete live walk;
   this path deliberately remains synchronous rather than arming a CPU driver's own destination pages. */
static int lag_guard_submit(unsigned slot, uint64_t fence) {
    if (vk_record_lag_native()) return lag_guard_submit_native(slot, fence, 0);
    const char *previous_role = nvn_dirty_lag_thread_role_get();
    nvn_dirty_lag_thread_role("submit-registration");
    uint64_t start = host_now_ns(), done = atomic_load(&g_done); int nr = 0;
    vk_record_lag_submit(slot, fence, NULL, 0);
    if (nvn_dirty_lag_vma_pressure()) lag_guard_failed(fence);
    int protect = !lag_guard_has_failed(fence) && lag_software_build_exclusions(done);
    if (!protect) lag_guard_failed(fence);
    if (g_lag_npending > 1) qsort(g_lag_pending, (size_t)g_lag_npending, sizeof *g_lag_pending, lag_pending_cmp);
    for (int i = 0; protect && i < g_lag_npending; i++) {
        lag_pending r = g_lag_pending[i]; uintptr_t lo, hi;
        if (!nvn_dirty_lag_bounds((const void *)(uintptr_t)r.lo, r.hi - r.lo, &lo, &hi)) continue;
        r.lo = lo; r.hi = hi;
        if (lag_software_excluded(r.lo, r.hi)) { g_lag_gpu++; continue; }
        if (nr && r.lo <= g_lag_pending[nr - 1].hi &&
            nvn_dirty_tracked((void *)(uintptr_t)g_lag_pending[nr - 1].lo,
                             (r.hi > g_lag_pending[nr - 1].hi ? r.hi : g_lag_pending[nr - 1].hi) - g_lag_pending[nr - 1].lo)) {
            lag_pending *prev = &g_lag_pending[nr - 1];
            if (r.hi > prev->hi) prev->hi = r.hi;
            prev->reason |= r.reason;
            if (r.max_range > prev->max_range) prev->max_range = r.max_range;
        } else g_lag_pending[nr++] = r;
    }
    for (int i = 0; i < nr; i++) {
        lag_pending r = g_lag_pending[i];
        totk_ft_count(FT_COUNT_GUARD_REGS, 1);
        if (nvn_dirty_lag_register((void *)(uintptr_t)r.lo, r.hi - r.lo, fence, r.reason, r.max_range)) {
            lag_guard_aliases((rng_t){r.lo, r.hi}, fence, r.reason, r.max_range);
            g_lag_guard_bytes += r.hi - r.lo; g_lag_guard_ranges++;
        } else {
            lag_guard_failed(fence);
            RED(4, "lag guard: unit busy or re-arm failed; its submission must complete before reuse");
        }
    }
    for (size_t i = 0; protect && i < g_lag_record_count; i++) {
        const lag_record_pending *r = &g_lag_record_pending[i]; uintptr_t lo, hi;
        if (nvn_dirty_lag_bounds((const void *)(uintptr_t)r->lo, r->hi - r->lo, &lo, &hi) &&
            lag_software_excluded(lo, hi)) { g_lag_gpu++; continue; }
        totk_ft_count(FT_COUNT_GUARD_REGS, 1);
        if (!nvn_dirty_lag_register((const void *)(uintptr_t)r->lo, r->hi - r->lo, r->ticket, r->reason, r->max_range))
            lag_guard_failed(r->ticket);
        else { g_lag_guard_bytes += r->hi - r->lo; g_lag_guard_ranges++; }
    }
    g_lag_record_count = 0; g_lag_npending = 0; g_lag_epoch++;
    g_lag_guard_bytes += atomic_exchange_explicit(&g_lag_record_bytes, 0, memory_order_relaxed);
    g_lag_guard_ranges += atomic_exchange_explicit(&g_lag_record_ranges, 0, memory_order_relaxed);
    g_lag_guard_ns += host_now_ns() - start;
    int failed = lag_guard_take_failed(fence);
    nvn_dirty_lag_thread_role(previous_role); return failed;
}
/* Mapping/tracking metadata is independent of queue tickets and snapshot arenas. Byte authority is not:
   every actual ticket or snapshot incarnation change revokes captures, even when the metadata remains a hit.
   Native captures additionally require every writer watch and dirty token; owned snapshots ignore native CPU reuse. */
typedef struct {
    uint64_t raw, size, align, submission, ticket, spans, lifetime, snapshot, physical, revision, dirty;
    uint64_t map_lo, map_hi, unit_lo, unit_hi, hot_frame, hot_dirty;
    VkDescriptorBufferInfo descriptor;
    int tracked, hot, captured, immutable;
} lag_binding_memo;
static int lag_binding_key(lag_binding_memo *m, uint64_t addr, uint64_t size, uint64_t align, vk_lag_kind_stats *s) {
    uint64_t submission = vk_submit_serial(), ticket = g_reserved_fence + 1;
    uint64_t spans = atomic_load_explicit(&g_span_gen, memory_order_acquire), lifetime = nvn_dirty_lag_epoch();
    int range = m->raw == addr && m->size == size && m->align == align;
    int chunk = m->submission == submission && m->ticket == ticket;
    int mapping = m->spans == spans && m->lifetime == lifetime;
    int snapshot = m->snapshot == g_snap_generation;
    /* Exact warm keys change neither metadata nor byte authority. Avoid zero-increment counter stores
       and rewriting three unchanged lifetime fields; still count every observable binding hit. */
    if (range && mapping && chunk && snapshot) { s->binding_hits++; return 1; }
    s->key_ticket += !chunk; s->key_snapshot += !snapshot;
    if (range && mapping) {
        s->binding_hits++; s->metadata_cross_ticket += !chunk; s->metadata_cross_snapshot += !snapshot;
        s->copy_ticket_misses += m->captured && !chunk && m->immutable != 1;
        s->copy_snapshot_misses += m->captured && !snapshot;
        /* Owned snapshots and their overlay copies live through the GAME submission's final fence, not
           an intermediate Vulkan chunk. Native captures retain the stricter actual-ticket lifetime. */
        if (!snapshot || (!chunk && m->immutable != 1)) { m->captured = 0; m->immutable = 0; }
        m->submission = submission; m->ticket = ticket; m->snapshot = g_snap_generation;
        return 1;
    }
    s->binding_misses++; s->key_range += !range; s->key_mapping += !mapping;
    *m = (lag_binding_memo){.raw = addr, .size = size, .align = align, .submission = submission,
        .ticket = ticket, .spans = spans, .lifetime = lifetime, .snapshot = g_snap_generation, .physical = addr};
    if (ub_private_on()) m->physical = lag_physical_window(addr, size, &m->map_lo, &m->map_hi);
    return 0;
}
static int lag_binding_copy(lag_binding_memo *m, VkDescriptorBufferInfo *out, vk_lag_kind_stats *s) {
    if (!m->captured) { s->copy_misses++; return 0; }
    if ((!m->immutable && m->dirty != nvn_dirty_now()) || !ub_range_unchanged(m->revision, m->physical, m->size)) {
        s->copy_misses++; s->copy_content_misses++; return 0;
    }
    s->copy_hits++; m->revision = g_ub_gen; *out = m->descriptor;
    return 1;
}
/* Units retain geometry across tickets, never watch authority or bytes. Hot history is refreshed against both
   real present frames (quiet demotion) and CPU dirty history (promotion), independently of queue submissions. */
typedef struct {
    uint64_t lo, hi, unit_lo, unit_hi, spans, lifetime, ticket, hot_frame, hot_dirty;
    int hot;
} lag_unit_memo;
static lag_unit_memo *lag_classify(lag_binding_memo *m, int same, vk_lag_kind_stats *s, vk_lag_kind kind) {
    enum { NU = 4096 };
    static lag_unit_memo units[NU];
    uint64_t addr = m->physical, size = m->size, window = addr & ~UINT64_C(65535);
    uint64_t frame = nvn_dirty_lag_frame_now(), dirty = nvn_dirty_now();
    /* The common exact cold binding already owns valid tracking metadata. With unchanged frame/history,
       avoid a second hash/table lookup just to recover the same conservative guarded decision. */
    if (same && !m->hot && m->hot_frame == frame && m->hot_dirty == dirty) return NULL;
    lag_unit_memo *u = &units[lag_hash(window, 0, 0) % NU];
    int mapped = m->map_hi > m->map_lo;
    if (mapped && u->spans == m->spans && u->lifetime == m->lifetime &&
        addr >= u->lo && addr < u->hi && size <= u->hi - addr &&
        m->map_lo <= u->lo && m->map_hi >= u->hi) {
        if (!same) { s->class_hits++; s->memo_hits++; }
        s->unit_cross_ticket += u->ticket != m->ticket; u->ticket = m->ticket;
        if (u->hot_frame != frame || u->hot_dirty != dirty) {
            u->hot = lag_hybrid_on() && nvn_dirty_lag_hot((const void *)(uintptr_t)addr, 1);
            u->hot_frame = frame; u->hot_dirty = dirty; s->hot_refreshes++;
        }
        m->tracked = 1; m->hot = u->hot && size <= lag_hot_limit();
        m->unit_lo = u->unit_lo; m->unit_hi = u->unit_hi; m->hot_frame = frame; m->hot_dirty = dirty;
        return u;
    }
    if (same) {
        if (m->hot_frame != frame || m->hot_dirty != dirty) {
            m->hot = m->tracked && lag_range_hot(addr, size);
            m->hot_frame = frame; m->hot_dirty = dirty; s->hot_refreshes++;
        }
        return NULL; /* retained exact tracking survives unit eviction; no watch may be inferred from it */
    }
    s->class_misses++; s->memo_misses++;
    ft_lag_count(kind, 1);
    if (!lag_wait_on()) return NULL;
    uintptr_t lo, hi;
    m->hot_frame = frame; m->hot_dirty = dirty; s->hot_refreshes++;
    int bounds = nvn_dirty_lag_bounds((const void *)(uintptr_t)addr, 1, &lo, &hi);
    if (bounds && size <= hi - addr) {
        m->tracked = 1; m->unit_lo = lo; m->unit_hi = hi;
        if (mapped) {
            uint64_t a = lo > m->map_lo ? lo : m->map_lo, z = hi < m->map_hi ? hi : m->map_hi;
            if (a < window) a = window;
            if (z > window + 65536) z = window + 65536;
            if (addr >= a && addr < z && size <= z - addr) {
                *u = (lag_unit_memo){.lo = a, .hi = z, .unit_lo = lo, .unit_hi = hi,
                    .spans = m->spans, .lifetime = m->lifetime, .ticket = m->ticket,
                    .hot_frame = frame, .hot_dirty = dirty,
                    .hot = lag_hybrid_on() && nvn_dirty_lag_hot((const void *)(uintptr_t)addr, 1)};
                m->hot = u->hot && size <= lag_hot_limit();
                return u;
            }
        }
        m->hot = lag_range_hot(addr, size);
    } else {
        m->tracked = nvn_dirty_tracked((const void *)(uintptr_t)addr, (size_t)size);
        m->hot = m->tracked && lag_range_hot(addr, size);
        if (m->tracked && nvn_dirty_lag_bounds((const void *)(uintptr_t)addr, size, &lo, &hi)) {
            m->unit_lo = lo; m->unit_hi = hi;
        }
    }
    return NULL;
}
static int lag_binding_pending(const lag_binding_memo *m, int whole_unit) {
    if (m->unit_hi > m->unit_lo) {
        int pending = lag_original_pending(m->unit_lo, m->unit_hi - m->unit_lo);
        if (!pending || whole_unit) return pending;
    }
    return lag_original_pending(m->physical, m->size);
}
static uint32_t lag_record_read_reason(const lag_binding_memo *m, uint32_t reason) {
    return reason | (lag_binding_pending(m, lag_software_gpu()) ? NVN_DIRTY_LAG_GPU : 0);
}
static int lag_watch_unit(const lag_unit_memo *u, uint64_t addr, uint64_t size) {
    /* Reuse only projection geometry. Every new native capture still acquires ALL writer gates, including BUSY
       synchronization; dirty_now alone cannot certify a watch across intentional GPU completion stores. */
    return u ? lag_watch_copy(u->lo, u->hi - u->lo) : lag_watch_copy(addr, size);
}
static int lag_read_select(uint64_t addr, uint64_t size, VkDeviceSize align, VkDescriptorBufferInfo *out,
                           uint32_t *record_reason, vk_lag_kind kind) {
    if (!align) align = 16;
    vk_lag_kind_stats *s = &g_lag_kind[kind];
    enum { NL = 4096 };
    static lag_binding_memo memo[NL];
    lag_binding_memo *m = &memo[lag_hash(addr, size, align) % NL];
    int same = lag_binding_key(m, addr, size, align, s);
    if (same) s->memo_hits++;
    lag_unit_memo *u = lag_classify(m, same, s, kind);
    addr = m->physical;
    s->hot_decisions += m->hot;
    if (m->tracked && !m->hot) {
        unsigned reason = NVN_DIRTY_LAG_READONLY | (size > lag_hot_limit() ? NVN_DIRTY_LAG_OVER_CAP : NVN_DIRTY_LAG_COLD);
        s->cold_guards++;
        if (same) LAG_TEST_INC(VK_LAG_TEST_COLD_HITS);
        if (record_reason) *record_reason = lag_record_read_reason(m, reason);
        else lag_guard_mark(addr, size, reason);
        return 0; /* every live descriptor still registers its full actual range/aliases at its actual ticket */
    }
    if (lag_binding_copy(m, out, s)) { LAG_TEST_INC(VK_LAG_TEST_COPY_HITS); return 1; }
    uint64_t t = lag_timing_on() ? host_now_ns() : 0;
    int gpu = lag_binding_pending(m, 0), captured = 0;
    VkBuffer b; uint64_t o;
    m->captured = 0;
    if (!gpu && (m->map_hi > m->map_lo || vk_resolve(addr, size, &b, &o))) {
        /* Sample before arming any writer projection: an early alias may fault while later aliases are armed. */
        uint64_t dirty = nvn_dirty_now();
        int watched = lag_watch_unit(u, addr, size);
        uint8_t *p = arena_alloc(snap_arena(), (size_t)size, align, &b, &o);
        lag_snapshot_allocation(kind, size);
        vk_ub_read(addr, p, (size_t)size);
        watched &= dirty == nvn_dirty_now();
        *out = (VkDescriptorBufferInfo){b, o, size};
        m->captured = watched; m->dirty = dirty; m->descriptor = *out; m->revision = g_ub_gen;
        if (!watched) g_lag_copy_unwatched++;
        g_lag_bytes += size; g_lag_ranges++; captured = 1;
    } else if (gpu) g_lag_gpu++;
    uint64_t elapsed = t ? host_now_ns() - t : 0;
    if (captured && m->hot) {
        g_lag_hot_bytes += size; g_lag_hot_ranges++;
        s->hot_copies++; s->hot_bytes += size; s->hot_ns += elapsed;
        totk_ft_count(FT_COUNT_HOT_COPIES, 1);
        totk_ft_count(FT_COUNT_HOT_BYTES, size);
    } else if (!captured) {
        if (record_reason) *record_reason = lag_record_read_reason(m, NVN_DIRTY_LAG_READONLY | NVN_DIRTY_LAG_GPU);
        else if (m->tracked) lag_guard_mark(addr, size, NVN_DIRTY_LAG_READONLY | NVN_DIRTY_LAG_GPU);
    }
    g_lag_ns += elapsed;
    return captured;
}
/* ---------- adaptive lag protection (TOTK_VK_LAG_ADAPTIVE=K, K >= 2; unset/0 = full protection) ----------
   Full protection decides every binding (~26k/frame on the Thor route, ~7 ms of walking), yet CPU writes that race
   in-flight GPU reads concentrate in a few stable units. Vertex/index/storage bindings outside the learned danger set
   (nvn_dirty_lag_danger_*, expanded to every pool alias) skip the decision and bind original memory, exactly as with
   capture off. Each frame still fully protects a rotating 1/K of 64 KiB windows (no periodic slow frame), so every
   window is guarded at least every K frames and the sticky guards keep reporting writes; a newly dangerous unit is
   learned within about K frames, but its racing writes before that are unprotected. Uniform bindings stay fully
   protected (capture also changes where inline uniform updates live). A set larger than the arrays falls back to
   full protection. Wait mode only. Walking thread only. */
static unsigned lag_adaptive_period(void) {
    static int period = -1;
    if (period < 0) {
        const char *e = getenv("TOTK_VK_LAG_ADAPTIVE");
        int requested = e ? atoi(e) : 0;
        period = requested >= 2 && lag_wait_on() ? requested : 0;
        if (e) LOGV("lag adaptive protection: %s (TOTK_VK_LAG_ADAPTIVE=%s)", period ? "on, rotating 1/K windows" :
                    requested >= 2 ? "off: requires TOTK_VK_LAG_MODE=wait" : "off: K must be at least 2", e);
    }
    return (unsigned)period;
}
enum { LAG_DANGER_RUNS = 2048, LAG_DANGER_RANGES = 8192 };
static struct {
    rng_t r[LAG_DANGER_RANGES];
    int n, runs, overflow;
    uint64_t marks, spans, bytes, rebuilds, rebuild_ns;
    uint64_t skips, set_hits, rotation, overflowed;   /* decisions, cumulative */
} g_ad;
static int lag_rng_cmp(const void *x, const void *y) {
    uint64_t a = ((const rng_t *)x)->lo, b = ((const rng_t *)y)->lo;
    return (a > b) - (a < b);
}
static void lag_danger_rebuild(uint64_t marks, uint64_t spans) {
    static uintptr_t lo[LAG_DANGER_RUNS], hi[LAG_DANGER_RUNS];
    uint64_t start = host_now_ns(), bytes = 0;
    int runs = nvn_dirty_lag_danger_collect(lo, hi, LAG_DANGER_RUNS), n = 0, overflow = runs > LAG_DANGER_RUNS;
    for (int i = 0; !overflow && i < runs; i++) {
        bytes += hi[i] - lo[i];
        int count; const rng_t *aliases = lag_alias_ranges((rng_t){lo[i], hi[i]}, &count);
        if (n + 1 + count > LAG_DANGER_RANGES) { overflow = 1; break; }
        g_ad.r[n++] = (rng_t){lo[i], hi[i]};
        for (int k = 0; k < count; k++) g_ad.r[n++] = aliases[k];
    }
    if (!overflow) {
        qsort(g_ad.r, (size_t)n, sizeof *g_ad.r, lag_rng_cmp);
        int m = 0;
        for (int i = 0; i < n; i++) {
            if (m && g_ad.r[i].lo <= g_ad.r[m - 1].hi) { if (g_ad.r[i].hi > g_ad.r[m - 1].hi) g_ad.r[m - 1].hi = g_ad.r[i].hi; }
            else g_ad.r[m++] = g_ad.r[i];
        }
        n = m;
    }
    g_ad.n = overflow ? 0 : n; g_ad.overflow = overflow; g_ad.runs = runs; g_ad.bytes = bytes;
    g_ad.marks = marks; g_ad.spans = spans; g_ad.rebuilds++; g_ad.rebuild_ns += host_now_ns() - start;
}
static int lag_adaptive_skip(uint64_t addr, uint64_t size) {
    unsigned period = lag_adaptive_period();
    if (!period) return 0;
    if (lag_hash(addr & ~UINT64_C(65535), 0, 0) % period == nvn_dirty_lag_frame_now() % period) { g_ad.rotation++; return 0; }
    uint64_t marks = nvn_dirty_lag_danger_marks(), spans = atomic_load_explicit(&g_span_gen, memory_order_acquire);
    if (!g_ad.rebuilds || marks != g_ad.marks || spans != g_ad.spans) lag_danger_rebuild(marks, spans);
    if (g_ad.overflow) { g_ad.overflowed++; return 0; }
    int lo = 0, hi = g_ad.n;   /* first range ending after addr */
    while (lo < hi) { int mid = (lo + hi) >> 1; if (g_ad.r[mid].hi <= addr) lo = mid + 1; else hi = mid; }
    if (lo < g_ad.n && g_ad.r[lo].lo < addr + size) { g_ad.set_hits++; return 0; }
    g_ad.skips++;
    return 1;
}
static int lag_read_inner(uint64_t addr, uint64_t size, VkDeviceSize align, VkDescriptorBufferInfo *out,
                          uint32_t *record_reason, vk_lag_kind kind) {
    if (record_reason) *record_reason = 0;
    if (__builtin_expect(g_pt_on, 0) && kind != VK_LAG_STORAGE && !vk_lag_capture_on())   /* oot-a122 (storage: vk_draw.c pt_storage_desc) */
        return addr && size && addr + size >= addr ? vk_pt_read(addr, size, align, (int)kind, out) : 0;
    if (!vk_lag_capture_on() || !addr || !size || addr + size < addr) return 0;
    if ((kind == VK_LAG_VERTEX || kind == VK_LAG_INDEX || kind == VK_LAG_STORAGE) && lag_adaptive_skip(addr, size)) return 0;
    uint64_t start = lag_decision_begin(kind, size);
    int captured = lag_read_select(addr, size, align, out, record_reason, kind);
    lag_decision_end(kind, start);
    return captured;
}
int vk_lag_read(uint64_t addr, uint64_t size, VkDeviceSize align, VkDescriptorBufferInfo *out, vk_lag_kind kind) {
    return lag_read_inner(addr, size, align, out, NULL, kind);
}
int vk_lag_read_record(uint64_t addr, uint64_t size, VkDeviceSize align, VkDescriptorBufferInfo *out,
                       uint32_t *reason, vk_lag_kind kind) {
    if (!vk_record_lag_enabled()) { *reason = 0; return lag_read_inner(addr, size, align, out, NULL, kind); }
    return lag_read_inner(addr, size, align, out, reason, kind);
}
void vk_lag_capture_take(uint64_t *bytes, uint64_t *ns, uint64_t *ranges, uint64_t *gpu_owned) {
    *bytes = g_lag_bytes; *ns = g_lag_ns; *ranges = g_lag_ranges; *gpu_owned = g_lag_gpu;
    g_lag_bytes = g_lag_ns = g_lag_ranges = g_lag_gpu = 0;
}
void vk_lag_stats(uint64_t frame) {
    if (!vk_lag_capture_on()) return;
    vk_rec_lock();
    static uint64_t last;
    uint64_t n = frame - last, bytes, ns, ranges, gpu; last = frame;
    vk_lag_capture_take(&bytes, &ns, &ranges, &gpu);
    uint64_t guard_bytes = atomic_exchange_explicit(&g_lag_guard_bytes, 0, memory_order_relaxed);
    uint64_t guard_ns = atomic_exchange_explicit(&g_lag_guard_ns, 0, memory_order_relaxed);
    uint64_t guard_ranges = atomic_exchange_explicit(&g_lag_guard_ranges, 0, memory_order_relaxed);
    static uint64_t last_faults, last_host, last_ns;
    uint64_t faults, hosts, wait_ns; nvn_dirty_lag_stats(&faults, &hosts, &wait_ns);
    uint64_t df = faults - last_faults, dh = hosts - last_host, dn = wait_ns - last_ns;
    last_faults = faults; last_host = hosts; last_ns = wait_ns;
    uint64_t hot_bytes = g_lag_hot_bytes, hot_ranges = g_lag_hot_ranges;
    g_lag_hot_bytes = g_lag_hot_ranges = 0;
    static uint64_t old_rearms, old_updates, old_promoted, old_demoted;
    uint64_t rearms, updates, promoted, demoted;
    nvn_dirty_lag_sticky_stats(&rearms, &updates, &promoted, &demoted);
    uint64_t dr = rearms - old_rearms, du = updates - old_updates, dp = promoted - old_promoted, dd = demoted - old_demoted;
    old_rearms = rearms; old_updates = updates; old_promoted = promoted; old_demoted = demoted;
    uint64_t index_ns = g_record_index_ns, index_versions = g_record_index_versions;
    g_record_index_ns = g_record_index_versions = 0;
    static vk_lag_kind_stats last_counts[VK_LAG_KIND_COUNT];
    vk_lag_kind_stats counts[VK_LAG_KIND_COUNT], old_counts[VK_LAG_KIND_COUNT];
    memcpy(counts, g_lag_kind, sizeof counts); memcpy(old_counts, last_counts, sizeof old_counts);
    memcpy(last_counts, counts, sizeof counts);
    static uint64_t ad_old[4], ad_old_rebuilds, ad_old_rebuild_ns, ad_old_waits, ad_old_hots;
    uint64_t ad_now[4] = {g_ad.skips, g_ad.set_hits, g_ad.rotation, g_ad.overflowed}, ad_d[4];
    for (int i = 0; i < 4; i++) { ad_d[i] = ad_now[i] - ad_old[i]; ad_old[i] = ad_now[i]; }
    uint64_t ad_rebuilds = g_ad.rebuilds - ad_old_rebuilds, ad_rebuild_ns = g_ad.rebuild_ns - ad_old_rebuild_ns;
    ad_old_rebuilds = g_ad.rebuilds; ad_old_rebuild_ns = g_ad.rebuild_ns;
    int ad_ranges = g_ad.n, ad_runs = g_ad.runs, ad_overflow = g_ad.overflow; uint64_t ad_bytes = g_ad.bytes;
    uint64_t danger_waits, danger_hots; nvn_dirty_lag_danger_stats(&danger_waits, &danger_hots);
    uint64_t ad_waits = danger_waits - ad_old_waits, ad_hots = danger_hots - ad_old_hots;
    ad_old_waits = danger_waits; ad_old_hots = danger_hots;
    vk_rec_unlock();
    if (n && lag_adaptive_period())
        LOGV("lag adaptive: K=%u; decisions skipped %.1f, danger set %.1f, rotation %.1f, overflow %.1f/frame; set %d unit runs "
             "(%.3f MiB) as %d alias-expanded ranges%s; new units %llu waited, %llu hot this window; rebuilds %.2f/frame, %.3f ms/frame",
             lag_adaptive_period(), (double)ad_d[0] / n, (double)ad_d[1] / n, (double)ad_d[2] / n, (double)ad_d[3] / n,
             ad_runs, (double)ad_bytes / 1048576.0, ad_ranges, ad_overflow ? " OVERFLOW: full protection" : "",
             (unsigned long long)ad_waits, (unsigned long long)ad_hots, (double)ad_rebuilds / n, (double)ad_rebuild_ns / 1e6 / n);
    static const char *const names[VK_LAG_KIND_COUNT] = {"vertex", "index", "storage", "uniform", "pointer", "texel", "copy", "other"};
    for (unsigned k = 0; n && k < VK_LAG_KIND_COUNT; k++) {
        const vk_lag_kind_stats *s = &counts[k], *p = &old_counts[k];
        if (s->decisions == p->decisions && s->snapshot_allocations == p->snapshot_allocations) continue;
#define LAG_DELTA(field) (s->field - p->field)
        uint64_t samples = LAG_DELTA(sample_count);
        LOGV("lag decisions %s: %.1f/frame; binding hit/miss %.1f/%.1f, unit hit/miss %.1f/%.1f, memo hit/miss %.1f/%.1f per frame; cold %.1f, hot %.1f/frame",
             names[k], (double)LAG_DELTA(decisions) / n, (double)LAG_DELTA(binding_hits) / n,
             (double)LAG_DELTA(binding_misses) / n, (double)LAG_DELTA(class_hits) / n, (double)LAG_DELTA(class_misses) / n,
             (double)LAG_DELTA(memo_hits) / n, (double)LAG_DELTA(memo_misses) / n,
             (double)LAG_DELTA(cold_guards) / n, (double)LAG_DELTA(hot_decisions) / n);
        LOGV("lag keys %s: range/align %.1f, mapping/lifetime %.1f metadata misses; submission/ticket %.1f, snapshot %.1f byte-key changes/frame (nonexclusive)",
             names[k], (double)LAG_DELTA(key_range) / n, (double)LAG_DELTA(key_mapping) / n,
             (double)LAG_DELTA(key_ticket) / n, (double)LAG_DELTA(key_snapshot) / n);
        LOGV("lag reuse %s: cross-ticket/cross-snapshot metadata %.1f/%.1f, cross-ticket units %.1f, hot-history refreshes %.1f/frame; copy authority hit/miss %.1f/%.1f, ticket/snapshot/content invalidations %.1f/%.1f/%.1f/frame",
             names[k], (double)LAG_DELTA(metadata_cross_ticket) / n, (double)LAG_DELTA(metadata_cross_snapshot) / n,
             (double)LAG_DELTA(unit_cross_ticket) / n, (double)LAG_DELTA(hot_refreshes) / n,
             (double)LAG_DELTA(copy_hits) / n, (double)LAG_DELTA(copy_misses) / n,
             (double)LAG_DELTA(copy_ticket_misses) / n, (double)LAG_DELTA(copy_snapshot_misses) / n,
             (double)LAG_DELTA(copy_content_misses) / n);
        if (k == VK_LAG_UNIFORM)
            LOGV("lag uniform sharing: direct snapshot binds %.1f, covering hit/miss %.1f/%.1f per frame (exact copy hits above; allocation bytes below)",
                 (double)LAG_DELTA(uniform_snapshot_binds) / n, (double)LAG_DELTA(uniform_cover_hits) / n,
                 (double)LAG_DELTA(uniform_cover_misses) / n);
        LOGV("lag cost %s: timing=%s %.1f samples/frame, %.1f ns/sample (1/64 decisions); hot copies %.1f, %.3f KiB, %.3f ms/frame; snapshot allocations %.1f, %.3f KiB/frame",
             names[k], lag_timing_on() ? "on" : "off", (double)samples / n, samples ? (double)LAG_DELTA(sample_ns) / samples : 0,
             (double)LAG_DELTA(hot_copies) / n, (double)LAG_DELTA(hot_bytes) / 1024.0 / n,
             (double)LAG_DELTA(hot_ns) / 1e6 / n, (double)LAG_DELTA(snapshot_allocations) / n,
             (double)LAG_DELTA(snapshot_bytes) / 1024.0 / n);
        LOGV("lag sizes %s: <=256B %.1f, <=4KiB %.1f, <=64KiB %.1f, <=hotcap %.1f, >hotcap %.1f/frame; cumulative max %llu B",
             names[k], (double)LAG_DELTA(size_buckets[0]) / n, (double)LAG_DELTA(size_buckets[1]) / n,
             (double)LAG_DELTA(size_buckets[2]) / n, (double)LAG_DELTA(size_buckets[3]) / n,
             (double)LAG_DELTA(size_buckets[4]) / n, (unsigned long long)s->max_size);
#undef LAG_DELTA
    }
    if (n) LOGV("lag capture: %.3f MiB, %.3f ms, %.1f ranges/frame; %.1f GPU-owned ranges/frame kept on queue timeline",
                (double)bytes / 1048576.0 / n, (double)ns / 1e6 / n, (double)ranges / n, (double)gpu / n);
    if (n && lag_wait_on()) LOGV("lag guard: %.3f MiB, %.3f ms, %.1f range registrations/frame; reuse waits fault %.3f, host/contended %.3f/frame, %.3f ms/frame",
        (double)guard_bytes / 1048576.0 / n, (double)guard_ns / 1e6 / n, (double)guard_ranges / n,
        (double)df / n, (double)dh / n, (double)dn / 1e6 / n);
    if (n && lag_wait_on()) LOGV("lag projection index: %.1f frozen versions/frame, %.3f ms/frame build (separate from recorder prep/service)",
        (double)index_versions / n, (double)index_ns / 1e6 / n);
    if (n && lag_wait_on()) {
        LOGV("lag sticky: %.2f re-arms/frame, %.1f armed-ticket updates/frame; hot copies %.2f ranges, %.3f KiB/frame; units promoted %llu, demoted %llu",
             (double)dr / n, (double)du / n, (double)hot_ranges / n, (double)hot_bytes / 1024.0 / n,
             (unsigned long long)dp, (unsigned long long)dd);
        nvn_dirty_lag_unit top[8]; int ntop = nvn_dirty_lag_top(top, 8);
        for (int i = 0; i < ntop; i++) LOGV("lag fault unit %p+%zu: %llu writes, %llu waits, last frame %llu, hot %d; guard reasons 0x%x [readonly=%d cold=%d over-cap=%d GPU=%d writable=%d alias=%d], max range %zu",
            (void *)top[i].addr, top[i].len, (unsigned long long)top[i].faults, (unsigned long long)top[i].waits,
            (unsigned long long)top[i].last_frame, top[i].hot, top[i].reasons,
            !!(top[i].reasons & NVN_DIRTY_LAG_READONLY), !!(top[i].reasons & NVN_DIRTY_LAG_COLD),
            !!(top[i].reasons & NVN_DIRTY_LAG_OVER_CAP), !!(top[i].reasons & NVN_DIRTY_LAG_GPU),
            !!(top[i].reasons & NVN_DIRTY_LAG_WRITABLE), !!(top[i].reasons & NVN_DIRTY_LAG_ALIAS), top[i].max_range);
    }
}
/* Unlike ordinary optional renaming this protects every CPU-known UBO, even a cold range. Both inline drawing and
   recorder freezes call it on the walking thread. The native base is freshly captured per game submission; sequence
   (g_snap_seq), not a Vulkan fence, says which direct CPU overlays that immutable base already contains. */
void vk_lag_uniform_prepare(uint64_t addr, uint64_t size) {
    if (!g_ub_unflushed || !ub_private_on() || !addr || !size || addr + size < addr) return;
    addr = ub_address(addr, size);
    if (lag_original_pending(addr, size)) vk_ub_touch(addr, size);
}
/* TOTK_VK_PREPARE_MASK (vk_draw.c). Preparation only materializes private inline overlays when an original-memory
   producer overlaps. It does not select bytes, capture memory, or acquire a writer watch. Completion/CPU writes can
   never introduce such a producer. All new overlays, snapshot resets and original producers advance these walk-owned
   revisions; mapping changes revoke the physical identity independently. */
int vk_lag_uniform_prepare_changed(void) {
    static uint64_t uniform, producer, spans;
    uint64_t generation = atomic_load_explicit(&g_span_gen, memory_order_acquire);
    if (uniform == g_ub_gen && producer == g_lag_producer_revision && spans == generation) return 0;
    uniform = g_ub_gen; producer = g_lag_producer_revision; spans = generation;
    return 1;
}
void vk_lag_uniform_prepare_check(uint64_t addr, uint64_t size) {
    uint64_t revision = g_ub_gen;
    vk_lag_uniform_prepare(addr, size);
    if (revision != g_ub_gen) DIE("lag prepare oracle: skipped required materialization at %#llx+%#llx",
        (unsigned long long)addr, (unsigned long long)size);
}
/* A tiny fixed-way memo shares only owned snapshot bytes. Expansion is at most 2x the requested extent and
   4 KiB, clipped to the captured span: never read unowned neighbouring bytes or blindly copy a dirty unit.
   Entries describe immutable allocations; a new partial overlay appends a new version, never patches an old one.
   The two candidate pages cover every <=4 KiB window; large requests retain exact-range memoization only. */
static void lag_uniform_snapshot_read(const snap_t *sn, const lag_binding_memo *m,
                                      VkDescriptorBufferInfo *out, vk_lag_kind_stats *s) {
    uint64_t address = m->physical, size = m->size, align = m->align, offset = sn->off + address - sn->lo;
    if (!(offset % align) && !snap_range_updated(sn, address, size)) {
        *out = (VkDescriptorBufferInfo){sn->buf, offset, size};
        g_stat_snap_binds++; s->uniform_snapshot_binds++;
        return;
    }
    enum { NB = 256, WAYS = 4 };
    static struct {
        uint64_t lo, hi, spans, lifetime, snapshot, revision;
        VkDescriptorBufferInfo descriptor;
    } cover[NB][WAYS];
    static unsigned next[NB];
    uint64_t page = address & ~UINT64_C(4095);
    if (size <= 4096) for (unsigned lane = 0; lane < 2 && (!lane || page >= 4096); lane++) {
        unsigned bucket = lag_hash(page - lane * 4096, 0, 0) % NB;
        for (unsigned i = 0; i < WAYS; i++) {
            const __typeof__(cover[0][0]) *c = &cover[bucket][i];
            if (c->snapshot != m->snapshot || c->spans != m->spans || c->lifetime != m->lifetime || address < c->lo ||
                address >= c->hi || size > c->hi - address) continue;
            uint64_t at = c->descriptor.offset + address - c->lo;
            if (at % align || !ub_range_unchanged(c->revision, address, size)) continue;
            *out = (VkDescriptorBufferInfo){c->descriptor.buffer, at, size};
            s->uniform_cover_hits++;
            return; /* never advance the whole window's revision after proving only this requested subrange */
        }
    }
    s->uniform_cover_misses++;
    uint64_t lo = address, hi = address + size;
    if (size <= 4096) {
        uint64_t width = 4096;
        while (width > size * 2) width /= 2;
        uint64_t step = width / 2, start = address - address % width, end = start + width;
        if (hi > end) { start = address - address % step; end = start + width; }
        if (end >= start && hi <= end) {
            lo = start > sn->lo ? start : sn->lo; hi = end < sn->hi ? end : sn->hi;
            /* The returned binding, not the guest span's beginning, must satisfy the requested alignment. */
            lo = address - (address - lo) / align * align;
        }
    }
    VkBuffer buffer; VkDeviceSize at;
    uint8_t *bytes = arena_alloc(snap_arena(), (size_t)(hi - lo), align, &buffer, &at);
    lag_snapshot_allocation(VK_LAG_UNIFORM, hi - lo);
    ub_read_from(lo, bytes, (size_t)(hi - lo), sn);
    *out = (VkDescriptorBufferInfo){buffer, at + address - lo, size};
    if (hi - lo <= 4096) {
        unsigned bucket = lag_hash(lo & ~UINT64_C(4095), 0, 0) % NB;
        __typeof__(cover[0][0]) *c = &cover[bucket][next[bucket]++ % WAYS];
        *c = (__typeof__(*c)){lo, hi, m->spans, m->lifetime, m->snapshot,
            g_ub_gen, {buffer, at, hi - lo}};
    }
    g_stat_ub_renamed++;
}
enum { XLATE_UNIFORM_MEMOS = 1024 };
static lag_binding_memo g_lag_uniform_memo[XLATE_UNIFORM_MEMOS];
static int lag_uniform_select(uint64_t addr, uint64_t size, VkDeviceSize align, VkDescriptorBufferInfo *out,
                              int replay) {
    if (!align) align = 16;
    /* Shared with the diagnostic observer; selection remains the sole writer. */
    vk_lag_kind_stats *s = &g_lag_kind[VK_LAG_UNIFORM];
    lag_binding_memo *m = &g_lag_uniform_memo[lag_hash(addr, size, align) % XLATE_UNIFORM_MEMOS];
    lag_binding_key(m, addr, size, align, s);
    if (lag_binding_copy(m, out, s)) { s->memo_hits++; LAG_TEST_INC(VK_LAG_TEST_UNIFORM_HITS); return 1; }
    s->memo_misses++;
    totk_ft_count(FT_COUNT_LAG_UNIFORM_MISS, 1);
    addr = m->physical; m->captured = 0;
    if (!replay && lag_original_pending(addr, size)) { vk_ub_touch(addr, size); return 0; }
    const snap_t *sn = g_nsnap ? snap_find(addr, size) : NULL;
    VkBuffer b; uint64_t offset;
    if (!sn && !vk_resolve(addr, size, &b, &offset)) return 0;
    uint64_t dirty = nvn_dirty_now();
    int reusable;
    if (sn) {
        lag_uniform_snapshot_read(sn, m, out, s);
        reusable = 1; /* CPU rewrites cannot replace an owned eager snapshot's old-frame base. */
    } else {
        /* No expansion or shared watch booleans for mutable native bases. Every actual copy reacquires ALL
           writer gates, even after a producer completed without a CPU dirty-generation transition. */
        reusable = lag_watch_copy(addr, size);
        uint8_t *p = arena_alloc(snap_arena(), (size_t)size, align, &b, &offset);
        lag_snapshot_allocation(VK_LAG_UNIFORM, size);
        ub_read_from(addr, p, (size_t)size, NULL);
        *out = (VkDescriptorBufferInfo){b, offset, size};
        g_stat_ub_renamed++;
        reusable &= dirty == nvn_dirty_now();
    }
    m->revision = g_ub_gen; m->dirty = dirty; m->descriptor = *out; m->captured = reusable; m->immutable = sn != NULL;
    if (!reusable) g_lag_copy_unwatched++;
    return 1;
}
int vk_lag_uniform_read(uint64_t addr, uint64_t size, VkDeviceSize align, VkDescriptorBufferInfo *out) {
    if (!ub_private_on() || !addr || !size || addr + size < addr) return 0;
    uint64_t start = lag_decision_begin(VK_LAG_UNIFORM, size);
    int captured = lag_uniform_select(addr, size, align, out, 0);
    lag_decision_end(VK_LAG_UNIFORM, start);
    return captured;
}
/* A frozen packet can execute after later inline updates to its original
   buffer. Descriptor handles alone do not freeze those bytes. Reuse the
   existing submission-owned immutable selector even when frame lag is off.
   Logged CPU updates are authoritative overlays; a pending GPU-only writer
   is not CPU-readable and cannot be guessed from the guest mapping. */
int vk_ub_replay_read(uint64_t addr, uint64_t size, VkDeviceSize align, VkDescriptorBufferInfo *out) {
    if (!addr || !size || addr+size<addr) return 0;
    addr=ub_address(addr,size);
    if (!addr || addr+size<addr) return 0;
    pthread_mutex_lock(&g_gw_mu);
    int pending=gw_pending(addr,addr+size,atomic_load_explicit(&g_done,memory_order_acquire));
    pthread_mutex_unlock(&g_gw_mu);
    if (pending) return 0;
    return lag_uniform_select(addr,size,align,out,1);
}
void vk_xlate_uniform_key(uint64_t address, uint64_t size, uint64_t out[4]) {
    out[0] = XLATE_UNIFORM_UNKNOWN; out[1] = out[2] = out[3] = UINT64_MAX;
    if (!address || !size || address + size < address) return;
    uint64_t align = V.props.limits.minUniformBufferOffsetAlignment; if (!align) align = 16;
    const lag_binding_memo *m = &g_lag_uniform_memo[lag_hash(address, size, align) % XLATE_UNIFORM_MEMOS];
    int memo = m->raw == address && m->size == size && m->align == align &&
        m->submission == vk_submit_serial() && m->snapshot == g_snap_generation &&
        m->spans == atomic_load_explicit(&g_span_gen, memory_order_acquire) && m->lifetime == nvn_dirty_lag_epoch();
    uint64_t physical = address;
    if (ub_private_on()) {
        /* Only an existing mapping decision can canonicalize aliases here. Never resolve a new live resource. */
        if (!memo) return;
        physical = m->physical;
    }
    if (physical + size < physical) return;
    if (memo && m->descriptor.buffer) {
        out[0] |= XLATE_UNIFORM_DIRTY; out[2] = m->dirty;
        if (m->captured && (m->immutable || m->ticket == g_reserved_fence + 1) &&
            (m->immutable || m->dirty == nvn_dirty_now()) && ub_range_unchanged(m->revision, physical, size))
            out[0] |= XLATE_UNIFORM_CAPTURE;
    }
    const snap_t *sn = g_nsnap ? snap_find(physical, size) : NULL;
    uint64_t end = physical + size, newest = 0, done = atomic_load_explicit(&g_done, memory_order_acquire);
    uint64_t cursor = end;
    if (g_nub && end > g_ub_lo && physical < g_ub_hi) {
        int a = 0, b = g_nub;
        while (a < b) { int mid = (a + b) >> 1; if (g_ub[mid].lo < end) a = mid + 1; else b = mid; }
        uint64_t floor = physical > g_ub_maxlen ? physical - g_ub_maxlen : 0;
        for (int i = a - 1; i >= 0 && g_ub_maxhi[i] > physical && (g_ub[i].lo > floor || !floor); i--) {
            const ub_ent *e = &g_ub[i];
            if (e->hi <= physical || (sn ? e->seq <= g_snap_seq : e->fence && e->fence <= done)) continue;
            if (e->seq > newest) newest = e->seq;
            if (e->hi >= cursor) cursor = e->lo; /* sorted-by-start union proves complete log coverage */
        }
    }
    if (newest) out[0] |= XLATE_UNIFORM_LOGGED;
    /* Private mode removes superseded log slices on every GPU write. Other modes can retain old log entries
       alongside a live descriptor: without an owned base, their apparent coverage is not selected-byte authority. */
    if (sn || (ub_private_on() && cursor <= physical)) {
        out[0] = (out[0] & ~XLATE_UNIFORM_UNKNOWN) | XLATE_UNIFORM_OWNED;
        if (sn) out[0] |= XLATE_UNIFORM_SNAPSHOT;
        out[1] = newest; out[3] = sn ? g_snap_generation : 0;
    }
}
void vk_ub_read(uint64_t addr, void *dst, size_t size) {
    addr = ub_address(addr, size);
    const snap_t *sn = g_nsnap ? snap_find(addr, size) : NULL;
    enum { NC = 128, BYTES = 32 };
    static struct { uint64_t address, size, revision, submission; const snap_t *snapshot; uint8_t bytes[BYTES]; } memo[NC];
    if (!vk_record_enabled() || !sn || !sn->updated || size > BYTES) { ub_read_from(addr, dst, size, sn); return; }
    __typeof__(memo[0]) *m = &memo[(unsigned)((addr >> 3) ^ (addr >> 16) ^ size) % NC];
    uint64_t submission = vk_submit_serial();
    if (m->address == addr && m->size == size && m->snapshot == sn && m->submission == submission &&
        (m->revision == g_ub_gen || vk_record_uniform_unchanged(m->revision, addr, size))) {
        memcpy(dst, m->bytes, size); m->revision = g_ub_gen;
        if (vk_rec_profile_on && T_rec.active) g_rec.pointer_hits++;
        return;
    }
    ub_read_from(addr, dst, size, sn);
    m->address = addr; m->size = size; m->snapshot = sn; m->submission = submission; m->revision = g_ub_gen; memcpy(m->bytes, dst, size);
    if (vk_rec_profile_on && T_rec.active) g_rec.pointer_misses++;
}
/* Return the authority that supplies every byte without a mutable native gap: explicit immutable snapshot (1),
   or complete CPU-known log coverage (2). Log authority expires on any log generation change, including pruning;
   a same-value publication is not permission to certify a now-native parent against future CPU writes. */
static int ub_read_owned(uint64_t address, uint64_t size) {
    if (g_nsnap && snap_find(address, size)) return 1;
    ub_prune();
    uint64_t end = address + size;
    if (!g_nub || end <= g_ub_lo || address >= g_ub_hi) return 0;
    int a = 0, b = g_nub;
    while (a < b) { int m = (a + b) >> 1; if (g_ub[m].lo < end) a = m + 1; else b = m; }
    uint64_t cursor = end, floor = address > g_ub_maxlen ? address - g_ub_maxlen : 0;
    for (int i = a - 1; i >= 0 && g_ub_maxhi[i] > address && (g_ub[i].lo > floor || !floor); i--) {
        if (g_ub[i].hi >= cursor) cursor = g_ub[i].lo;
        if (cursor <= address) return 2;
    }
    return 0;
}
void vk_ub_read_record(uint64_t addr, void *dst, size_t size) {
    if (!size) { vk_ub_read(addr, dst, size); return; }
    if (!vk_lag_capture_on()) {
        /* Without write watching, only owned bytes certify an indirect pointer for stage-freeze reuse.
           The selected child's tracking cannot make a native parent pointer immutable. */
        if (!addr || addr + size < addr || !ub_read_owned(addr, size)) g_lag_copy_unwatched++;
        vk_ub_read(addr, dst, size); return;
    }
    if (!addr || addr + size < addr) {
        g_lag_copy_unwatched++; vk_ub_read(addr, dst, size); return;
    }
    uint64_t start = lag_decision_begin(VK_LAG_POINTER, size);
    vk_lag_kind_stats *s = &g_lag_kind[VK_LAG_POINTER];
    enum { NP = 128 };
    static lag_binding_memo memo[NP];
    lag_binding_memo *m = &memo[lag_hash(addr, size, 0) % NP];
    lag_binding_key(m, addr, size, 0, s);
    addr = m->physical;
    uint64_t dirty = nvn_dirty_now(), revision = g_ub_gen;
    int authority = m->immutable;
    int stable = m->captured && (authority || m->dirty == dirty) &&
        (authority == 2 ? m->revision == revision : ub_range_unchanged(m->revision, addr, size));
    if (stable) s->memo_hits++; else s->memo_misses++;
    if (!stable) {
        authority = ub_read_owned(addr, size);
        /* Sample before any watch: an early writer projection can fault while later ones are being armed. */
        dirty = nvn_dirty_now();
        stable = authority || (!lag_original_pending(addr, size) && lag_watch_copy(addr, size));
        revision = g_ub_gen;
    }
    vk_ub_read(addr, dst, size); /* native/unwatchable parents never use vk_ub_read's snapshot-only byte memo */
    if (!authority) stable &= dirty == nvn_dirty_now();
    else if (authority == 2) stable &= revision == g_ub_gen;
    m->captured = stable; m->immutable = authority; m->dirty = dirty; m->revision = g_ub_gen;
    if (!stable) g_lag_copy_unwatched++;
    lag_decision_end(VK_LAG_POINTER, start);
}
#ifdef TOTK_VK_TESTS
uint64_t vk_record_test_pointer_hits(void) { return g_rec.pointer_hits; }
#endif
static void update_ub(uint64_t dst, const uint8_t *data, uint64_t n) {
    VkBuffer db; uint64_t dof;
    if (n <= 64) vk_pd_write_note("inline update", dst, n, data);
    int ok = vk_resolve(dst, n, &db, &dof);
    if (!ok || (dof & 3) || (n & 3) || n > 65536 || !n) { vk_cpu_step(); memcpy((void *)(uintptr_t)dst, data, (size_t)n); vk_gpu_write(dst, n); return; }
    g_stat_ub++;
    if (ub_rename_on()) { ub_log(dst, data, n, 0); return; }
    if (ub_private_on()) {
        uint64_t a = ub_address(dst, n);
        pthread_mutex_lock(&g_gw_mu);
        int gpu = gw_pending(a, a + n, atomic_load(&g_done));
        pthread_mutex_unlock(&g_gw_mu);
        if (!gpu && !ub_original_pending(a, n)) { ub_log(dst, data, n, 0); return; }
        vk_gpu_write(dst, n);   /* a true original-memory producer cannot be replaced by a stale CPU base */
    }
    vk_end_rendering_at(VK_PASS_END_UNIFORM, 0);
    VkCommandBuffer cb = vk_cb();
    vk_barrier_all(cb);
    if (__builtin_expect(g_ic_k, 0) && ic_gw_window()) ic_gpu_wrote(dst, n, VK_INFLIGHT_GW_UNIFORM);   /* in-flight check: an inline uniform update */
    vkCmdUpdateBuffer(cb, db, dof, n, data);
    vk_barrier_all(cb);
    /* CPU-resolved storage pointers / recorder captures need these bytes before the GPU executes the update too.
       This entry is already recorded: retain it through its fence, but never flush it again or rename a descriptor. */
    ub_log(dst, data, n, 1);
    lag_producer_changed(ub_address(dst, n), n);
}
static void post(post_op o) {
    if (g_ncarry == g_capcarry) { g_capcarry = g_capcarry ? 2 * g_capcarry : 64; g_carry = VK_REALLOC(g_carry, (size_t)g_capcarry * sizeof *g_carry); }
    g_carry[g_ncarry++] = o;
}
void mtl_sub_event(volatile uint32_t *val, uint32_t mode, uint32_t v) {
    if (__builtin_expect(g_ic_k, 0) && ic_gw_window()) ic_gpu_wrote((uint64_t)(uintptr_t)val, 4, VK_INFLIGHT_GW_PLAIN);   /* in-flight check: apply_posts writes it at completion */
    post((post_op){1, val, mode, v, 0, 0});
}
void mtl_sub_counter(uint64_t addr, uint32_t type) {
    if (__builtin_expect(g_ic_k, 0) && ic_gw_window()) ic_gpu_wrote(addr, 16, VK_INFLIGHT_GW_PLAIN);   /* in-flight check: apply_posts writes the report at completion */
    if (__builtin_expect(g_gwl_on != 0, 0) && gwl_active()) { char w[32]; snprintf(w, sizeof w, "counter(type %u)", type); gwl_note(addr, 16, w); }
    uint32_t q1 = 0;
    if ((g_gpu_time_hardware || type == NVN_COUNTER_TYPE_TIMESTAMP || type == NVN_COUNTER_TYPE_TIMESTAMP_TOP) && ts_reports_on() && !atomic_load(&g_tsq_busy[g_tsq_next])) {
        /* Hardware mode timestamps every counter type too: a samples report's
           timestamp field must not jump ahead to the final chunk's CPU delivery. */
        uint32_t q = g_tsq_next; g_tsq_next = (q + 1) % TSQ; atomic_store(&g_tsq_busy[q], 1);
        VkPipelineStageFlags2 stage = type == NVN_COUNTER_TYPE_TIMESTAMP_TOP ? VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT;
        if (!vk_record_counter_timestamp(g_tsq, q, stage)) vkCmdWriteTimestamp2(vk_cb(), stage, g_tsq, q);
        q1 = q + 1;
        if (!g_cur->tsq_count) g_cur->tsq_first = q;
        g_cur->tsq_count++;
        g_tsq_time[q].valid = 0;
    }
    occl_node *samples = type == NVN_COUNTER_TYPE_SAMPLES_PASSED ? occl_snapshot() : NULL;
    uint64_t val = type == NVN_COUNTER_TYPE_SAMPLES_PASSED ? vk_counter_samples() : 0;
    int pair_end = 0;
#ifdef TOTK_GAME_OOT
    /* The game's GPU load pair: a TIMESTAMP_TOP report at A followed at once by a TIMESTAMP report at A + 16 (the time the
       GPU needs for what is queued before them). Ocarina of Time's dynamic resolution steps its scene size by their
       difference (Swap2-Native's game notes: the controller at main+0x5a8f0, load = microseconds x 100 / 16666.67, the
       last 3 and 5 samples averaged). Here the two were stamped apart (hardware queries or completion clocks), the load
       moved, and the scene went from 1440x810 down to 854x480 and 427x240 and back: the picture changed sharpness from
       frame to frame (oot-a63, a64). The end report reads the begin report's time plus TOTK_OHOS_DYNRES_US microseconds
       (default 0: no load, the game keeps its first level; a large value holds the last level). */
    /* The pairs span the frame: a begin report at X early in the frame, its end report at X + 16 before the last pass (pd-a64:
       TOP 0x336f3a020 .. TIMESTAMP 0x336f3a030, and TOP 0x33d6c84b0, TIMESTAMP 0x33d6c84c0 .. TIMESTAMP 0x33d6c84d0), with
       some 36 other TIMESTAMP_TOP reports between them (agl's one-shot clears). So: a TIMESTAMP report at X is an end report
       when X - 16 has had a timestamp report. */
    if (type == NVN_COUNTER_TYPE_TIMESTAMP_TOP || type == NVN_COUNTER_TYPE_TIMESTAMP) {
        static uint64_t seen[64]; static unsigned nseen, next; int have = 0;
        for (unsigned i = 0; i < nseen; i++) { if (seen[i] == addr - 16 && type == NVN_COUNTER_TYPE_TIMESTAMP) pair_end = 1; if (seen[i] == addr) have = 1; }
        if (!have) { if (nseen < 64) seen[nseen++] = addr; else { seen[next] = addr; next = (next + 1) % 64; } }
    }
#endif
    post((post_op){.kind = 2, .addr = addr, .type = type, .cval = val, .tsq1 = q1, .occl = samples, .pair_end = pair_end});
    cpend_push(addr, val, type != NVN_COUNTER_TYPE_SAMPLES_PASSED || !g_occl_on);
}
void mtl_sub_index(const uint64_t *h, int n) { (void)h; (void)n; }
void mtl_exec_rest(const uint64_t *h, int n) { (void)h; (void)n; }
void vk_note_draw(void) { g_stat_draw++; }
static void ub_seal(uint64_t fence) { for (int i = 0; i < g_nub; i++) if (g_ub[i].flushed && !g_ub[i].fence) g_ub[i].fence = fence; ub_changed(0, 0); }

/* A bounded draw-side lookahead never escapes the current recording or follows calls. Restored on recursive return. */
static const uint8_t *g_exec_next; static uint32_t g_exec_op;
/* Logical queues (hle_nvn.c): a stream stops at an event wait the queue may not pass yet and is resumed there later.
   g_park_stack: where to go on, innermost first (the wait itself, then each caller's op after its OP_CALL). Signals
   tell hle_nvn.c their values as they are encoded: the one Vulkan queue runs in encoding order. */
int nvn_event_wait_ready(const uint8_t *op, volatile uint32_t *val, uint32_t mode, uint32_t value) __attribute__((weak));
void nvn_event_encoded(volatile uint32_t *val, uint32_t mode, uint32_t value) __attribute__((weak));
static int g_park, g_park_n; static const uint8_t *g_park_stack[18];
const uint8_t *vk_exec_draw_next(void) { return g_exec_op == OP_DRAW || g_exec_op == OP_DRAW_IDX ? g_exec_next : NULL; }
/* Constant specialization removes all diagnostic work from the disabled opcode loop, including bind checks. */
static __attribute__((always_inline)) inline void mtl_exec_impl(const uint8_t *p, int diagnostics) {
    const uint8_t *saved_next = g_exec_next; uint32_t saved_op = g_exec_op;
    int profile_stream = -1; if (__builtin_expect(vk_rec_timing_on, 0)) profile_stream = rec_stream_begin();
    /* Poll at bounded work chunks, not every binding opcode. Calls start a
       fresh chunk and jumps still consume its budget, so draw-free streams
       cannot starve an invalidate. The flag remains a relaxed atomic hint;
       the ownership-release handshake supplies the no-lost-wakeup guarantee. */
    unsigned service_left = 0;
    for (;;) {
        if (__builtin_expect(!service_left--, 0)) {
            vk_rec_service_check();
            service_left = 63;
        }
        if (__builtin_expect(vk_rec_timing_on, 0)) {
            uint32_t code; memcpy(&code, p, sizeof code);
            if (code < OP_COUNT_) rec_sample_begin(code);
        }
        op_hdr h; memcpy(&h, p, sizeof h);
        if (__builtin_expect(vk_rec_profile_on, 0) && T_rec.active) g_rec.ops++;
        if (h.size < 8 || (h.size & 7) || h.op >= OP_COUNT_) DIE("【红】corrupt command record at %p: op %u size %u", (const void *)p, h.op, h.size);
        const uint8_t *a = p + 8;
        const uint8_t *next = p + h.size;
        g_exec_next = next; g_exec_op = h.op;
        if (diagnostics) {
            if (h.op == OP_BIND_TEX || h.op == OP_BIND_IMG || h.op == OP_BIND_SAMP) xlate_deferred_bind(h.op, a);
            if (h.op == OP_DRAW || h.op == OP_DRAW_IDX) {
                g_xlate->counts.draws++;
                for (unsigned lane = 0; lane < 2; lane++) {
                    g_xlate->counts.repeated_draws[lane] += g_xlate->repeated[lane];
                    g_xlate->counts.covered_draws[lane] += g_xlate->covered[lane];
                }
            }
        }
        switch (h.op) {
            case OP_END:
                if (__builtin_expect(vk_rec_timing_on, 0)) { rec_sample_end(); vk_rec_profile_pop(profile_stream); }
                g_exec_next = saved_next; g_exec_op = saved_op; return;
            case OP_BEGIN: break;
            case OP_JUMP: memcpy(&next, a, sizeof next); break;
            case OP_CALL: {
                const uint8_t *t; memcpy(&t, a, 8); mtl_exec(t);
                if (g_park) {   /* the call stopped at a wait: this stream goes on after the call */
                    if (g_park_n < 18) g_park_stack[g_park_n++] = next;
                    if (__builtin_expect(vk_rec_timing_on, 0)) { rec_sample_end(); vk_rec_profile_pop(profile_stream); }
                    g_exec_next = saved_next; g_exec_op = saved_op; return;
                }
                break;
            }
            case OP_WAIT_EVENT: {
                volatile uint32_t *val; uint32_t m, v; memcpy(&val, a, 8); memcpy(&m, a + 8, 4); memcpy(&v, a + 12, 4);
                if (nvn_event_wait_ready && !nvn_event_wait_ready(p, val, m, v)) {
                    g_park = 1; g_park_n = 0; g_park_stack[g_park_n++] = p;
                    if (__builtin_expect(vk_rec_timing_on, 0)) { rec_sample_end(); vk_rec_profile_pop(profile_stream); }
                    g_exec_next = saved_next; g_exec_op = saved_op; return;
                }
                break;
            }
            case OP_SIGNAL_EVENT: { volatile uint32_t *val; uint32_t m, v; memcpy(&val, a, 8); memcpy(&m, a + 8, 4); memcpy(&v, a + 12, 4); if (vk_fg_extract_active) vk_fg_extract_memory_write((uint64_t)(uintptr_t)val,4); mtl_sub_event(val, m, v); if (nvn_event_encoded) nvn_event_encoded(val, m, v); break; }
            case OP_REPORT_COUNTER: { uint64_t t, ad; memcpy(&t, a, 8); memcpy(&ad, a + 8, 8); if (vk_fg_extract_active) vk_fg_extract_memory_write(ad,16); VK_REC_DO(VK_REC_S_COUNTER, vk_draw_exec(h.op, a); mtl_sub_counter(ad, (uint32_t)t)); break; }
            case OP_COPY_B2B: { uint64_t s, d, n; memcpy(&s, a, 8); memcpy(&d, a + 8, 8); memcpy(&n, a + 16, 8); if (vk_fg_extract_active) vk_fg_extract_memory_write(d,n); VK_REC_DO(VK_REC_O_TRANSFER, mtl_sub_copy(s, d, n)); break; }
            case OP_FILL_B: { p_fill_b f; memcpy(&f, a, sizeof f); if (vk_fg_extract_active) vk_fg_extract_memory_write(f.dst, f.size); VK_REC_DO(VK_REC_O_TRANSFER, mtl_sub_fill(f.dst, f.size, f.value)); break; }
            case OP_UPDATE_UB: { uint64_t d, n; memcpy(&d, a, 8); memcpy(&n, a + 8, 8); if (vk_fg_extract_active) vk_fg_extract_memory_write(d,n); VK_REC_DO(VK_REC_S_UNIFORM, update_ub(d, a + 16, n)); break; }
            default:
                if (__builtin_expect(vk_rec_timing_on, 0)) {
                    int category = h.op == OP_RESET_COUNTER || h.op == OP_RENDER_COND ? VK_REC_S_COUNTER :
                        h.op == OP_SET_RT || h.op == OP_CLEAR_COLOR || h.op == OP_CLEAR_DS || h.op == OP_BARRIER ? VK_REC_S_PASS :
                        h.op == OP_DRAW || h.op == OP_DRAW_IDX ? VK_REC_S_TOUCH : VK_REC_S_STATE;
                    VK_REC_DO(category, vk_draw_exec(h.op, a));
                } else vk_draw_exec(h.op, a);
                break;
        }
        if (__builtin_expect(vk_rec_timing_on, 0)) rec_sample_end();
        p = next;
    }
}
void mtl_exec(const uint8_t *p) {
    if (__builtin_expect(vk_xlate_stats_on(), 0)) {
        if (!g_xlate->depth++) xlate_recording(p);
        mtl_exec_impl(p, 1);
        if (!--g_xlate->depth) xlate_publish();
    } else mtl_exec_impl(p, 0);
}
/* Run the continuation c (pc[0] first, each to its stream's end). 1: stopped at a wait again, c now says where to go on;
   0: ran to the end. */
int mtl_exec_cont(const uint8_t **pc, int *n) {
    for (int i = 0; i < *n; i++) {
        g_park = 0; mtl_exec(pc[i]);
        if (g_park) {
            const uint8_t *nc[18]; int k = 0;
            for (int j = 0; j < g_park_n && k < 18; j++) nc[k++] = g_park_stack[j];
            for (int j = i + 1; j < *n && k < 18; j++) nc[k++] = pc[j];
            for (int j = 0; j < k; j++) pc[j] = nc[j];
            *n = k; g_park = 0; return 1;
        }
    }
    *n = 0; return 0;
}
uint64_t mtl_sub_commit(void) {
    mtl_init();
    int slot_trace=__builtin_expect(atomic_load_explicit(&vk_fg_slot_boundary_active,memory_order_relaxed),0) &&
        vk_fg_job_trace_enabled();
    uint64_t slot_submit_begin=slot_trace ? host_now_ns() : 0;
    uint64_t v = slot_submit(1, VK_PASS_END_SUBMISSION);
    if (__builtin_expect(g_ic_k, 0)) ic_sub_commit();   /* H1: every slot of this game submission is sealed */
    uint64_t slot_submit_return=slot_trace ? host_now_ns() : 0;
    vk_rec_service_run(1); /* producer complete: service before unrelated future submissions can acquire ownership */
    uint64_t slot_service_return=slot_trace ? host_now_ns() : 0;
    g_in_sub = 0;
    g_gpu_guest_ns = g_gpu_origin_ticks = g_gpu_guest_frame = 0;
    if (!g_cur && __builtin_expect(atomic_load_explicit(&vk_fg_slot_boundary_active,memory_order_relaxed),0)) {
        uint64_t enter=slot_trace ? host_now_ns() : 0;
        if (slot_trace) LOGV("[adaptive-slot-boundary] event=enter kind=commit boundary_ns=%llu submit_begin_ns=%llu submit_return_ns=%llu service_return_ns=%llu real_ticket=%llu open_primary=0 clock=CLOCK_MONOTONIC",
            (unsigned long long)enter,(unsigned long long)slot_submit_begin,
            (unsigned long long)slot_submit_return,(unsigned long long)slot_service_return,
            (unsigned long long)v);
        vk_fg_slot_boundary();
        if (slot_trace) LOGV("[adaptive-slot-boundary] event=exit kind=commit enter_ns=%llu exit_ns=%llu real_ticket=%llu clock=CLOCK_MONOTONIC",
            (unsigned long long)enter,(unsigned long long)host_now_ns(),(unsigned long long)v);
    } else if (slot_trace) {
        LOGV("[adaptive-slot-boundary] event=skip kind=commit now_ns=%llu submit_begin_ns=%llu submit_return_ns=%llu service_return_ns=%llu real_ticket=%llu open_primary=%u active=%u clock=CLOCK_MONOTONIC",
            (unsigned long long)host_now_ns(),(unsigned long long)slot_submit_begin,
            (unsigned long long)slot_submit_return,(unsigned long long)slot_service_return,
            (unsigned long long)v,g_cur!=NULL,
            atomic_load_explicit(&vk_fg_slot_boundary_active,memory_order_relaxed)!=0);
    }
    if (ub_private_on()) snap_reset();
    if (g_sar_cur >= 0) { g_sar[g_sar_cur].release = v; g_sar_cur = -1; }
    g_stat_sub++;
    /* TOTK_VK_SYNC=1 (diagnostic): every submission finishes on the GPU before the game goes on, as if the GPU were
       infinitely fast; whatever still differs between frames then is not a race with the GPU. */
    { static int sync = -1; if (sync < 0) { const char *e = getenv("TOTK_VK_SYNC"); sync = e && atoi(e) > 0; if (sync) LOGV("TOTK_VK_SYNC: each submission is waited for"); }
      if (sync) mtl_wait(v, 5000000000ull); }
    ub_prune(); gw_prune();
    if (g_perf_t0) {
        uint64_t cpu = walker_cpu_now() - g_perf_cpu_t0, dt = host_now_ns() - g_perf_t0;
        uint64_t w = g_perf_wait_ns - g_perf_sub0, r = dt > w ? dt - w : 0;
        g_perf_cpu_ns += cpu; g_perf_elapsed_ns += dt;
        g_perf_replay_ns += r; g_cum[0] += r; g_perf_t0 = 0; g_perf_subs++; vk_record_walk_time(r);
        HK_ADD(subs, 1); HK_ADD(replay_ns, r); HK_ADD(wait_ns, w);
    }
    if (__builtin_expect(vk_rec_timing_on, 0)) vk_rec_profile_end();
    vk_res_sweep();   /* landing also owns the recording lock: no new GPU guards while guest bytes are stored */
    vk_rec_unlock();   /* taken in mtl_sub_begin */
    if (g_stat_sub % 3000 == 0) {
        uint64_t n, b; gpu_pool_totals(&n, &b);
        LOGV("uniform snapshots: %.1f KiB in %.0f ranges per submission, %llu bindings read them, %llu ranges left to memory (GPU writes in flight)", g_stat_snap_bytes / 1024.0 / g_stat_sub, (double)g_stat_snap_ranges / g_stat_sub, (unsigned long long)g_stat_snap_binds, (unsigned long long)g_stat_snap_skipped);
        LOGV("%llu submissions: %llu GPU copies, %llu uniform updates (%llu renamed copies), %llu CPU steps; %llu pools (%llu MiB on the GPU, %llu CPU-only)",
             (unsigned long long)g_stat_sub, (unsigned long long)g_stat_gpu_copy, (unsigned long long)g_stat_ub, (unsigned long long)g_stat_ub_renamed, (unsigned long long)g_stat_cpu_step,
             (unsigned long long)n, (unsigned long long)(b >> 20), (unsigned long long)g_pool_cpu);
        vk_res_stats();
    }
    nvn_dirty_lag_thread_role(g_lag_guest_role);
    totk_ft_work_end();
    T_ft_walk_frame = 0;
    return v;
}
/* A submission of the backend's own (present, CPU reads): whatever is recorded goes to the GPU and is waited for. */
/* Memory the backend holds (TOTK, 2026-10-22: the app's memory grew ~45 MB/s in the world, native heap and GPU-mapped
   memory together, until Android ran out). Logged with the per-300-frame lines: whichever number climbs is the leak. */
void vk_res_mem(uint64_t v[8]);
int vk_dpool_count(void) { int n = 0; for (int i = 0; i < RING; i++) n += g_ring[i].ndpool; return n; }
/* Stop capture at a game-submission boundary, never in the middle of a stream
   whose later draws may still use private uniform/texel incarnations. The GPU
   cursor remains enabled for its original lifetime-pin contract. */
static void lag_pressure_fallback(void) {
    if (!nvn_dirty_lag_vma_pressure() || !vk_lag_capture_on()) return;
    vk_flush_wait();
    submit_join(mtl_last_fence());
    nvn_dirty_lag_vma_drain();
    ub_publish();
    snap_reset();
    for (int i = 0; i < SNAP_ARENAS; i++) {
        arena_release(&g_sar[i].ar);
        g_sar[i].release = 0;
    }
    g_sar_cur = -1; g_sar_next = 0;
    ub_prune(); gw_prune();
    atomic_store_explicit(&vk_lag_capture_state, 0, memory_order_release);
    uint64_t vma[5]; nvn_dirty_lag_vma_stats(vma);
    LOGV("lag pressure cutover complete: maps %llu/%llu, guarded runs %llu; capture off, ordinary submission and raw frame-end waits restored",
        (unsigned long long)vma[0], (unsigned long long)vma[1], (unsigned long long)vma[2]);
}
/* /proc/self/maps holds ~30-40k lines in-game. Both budget counts and opt-in classification
   (TOTK_VMA_CLASSES=1) run on the low-priority sampler; this present-path report only reads cached
   samples/telemetry and optionally requests background work, never waits for or performs a maps scan. */
#ifdef TOTK_OHOS
/* The shader seed pack (2.2 GB) is a read-only file mapping (vk_seed_io.c); every page a module was read from stays in the
   process's RSS, which the system's RSS_THRESHOLD_KILLER counts: 938 MB of it at Ocarina of Time's title. Every memory
   report (300 presents) drops those pages again (MADV_DONTNEED: clean file pages, read back from the file when needed).
   TOTK_OHOS_SEED_DROP=0 keeps them. */
/* Loading a scene runs at about a frame a second, so a report every 300 presents never came while the pipelines of the
   new scene read the pack (oot-a43m / a44: killed 14 s after loading the save). A thread drops the pages every
   TOTK_OHOS_SEED_DROP_S seconds (5) as well. */
/* Where the resident set is while it grows (the same thread, every period, once VmRSS passes TOTK_OHOS_RSS_LOG_MB, 4096):
   the kernel's split and, by mincore over /proc/self/maps, the guest's stack, heap and alias regions (runtime services.c
   layout), each split into GPU-pool native buffers (dma-buf mappings) and the guest's own memory. */
static void guest_rss_report(void) {
    /* Off unless TOTK_OHOS_RSS_LOG_MB is set. It was on from 4096 MiB: every period it ran mincore over all of the guest's
       mapped memory (a dozen GiB, page by page) on a thread that had the walker's name, priority and CPUs. hiperf on
       Ocarina of Time in play (oot-a66): 45% of the cycles under "totk-walk" were this loop, more than the replay itself,
       and its /proc/self/maps and mincore passes hold the address space lock the replay's page faults need. */
    static long floor_mb = -1; if (floor_mb < 0) { const char *e = getenv("TOTK_OHOS_RSS_LOG_MB"); floor_mb = e && *e ? atol(e) : 0; }
    if (floor_mb <= 0) return;
    char line[512]; unsigned long anon = 0, file = 0, shm = 0, rss = 0;
    FILE *st = fopen("/proc/self/status", "r"); if (!st) return;
    while (fgets(line, sizeof line, st)) { sscanf(line, "VmRSS: %lu", &rss); sscanf(line, "RssAnon: %lu", &anon); sscanf(line, "RssFile: %lu", &file); sscanf(line, "RssShmem: %lu", &shm); }
    fclose(st);
    if ((long)(rss >> 10) < floor_mb) return;
    static const struct { const char *name; uint64_t lo, hi; } win[3] = {{"stack", 0x310000000ull, 0x320000000ull}, {"heap", 0x330000000ull, 0x570000000ull}, {"alias", 0x570000000ull, 0x1570000000ull}};
    uint64_t res[3][2] = {{0}};   /* [window][0 guest memory, 1 dma-buf] pages */
    static unsigned char vec[16384];   /* 64 MiB of 4 KiB pages a call */
    FILE *f = fopen("/proc/self/maps", "r"); if (!f) return;
    while (fgets(line, sizeof line, f)) {
        unsigned long lo, hi; if (sscanf(line, "%lx-%lx", &lo, &hi) != 2) continue;
        int dma = strstr(line, "dmabuf") || strstr(line, "dma_buf") || strstr(line, "dma-buf");
        for (int w = 0; w < 3; w++) {
            uint64_t a = lo > win[w].lo ? lo : win[w].lo, b = hi < win[w].hi ? hi : win[w].hi;
            for (; a < b; a += 64ull << 20) {
                uint64_t n = b - a < (64ull << 20) ? b - a : (64ull << 20);
                if (mincore((void *)a, n, vec)) break;
                for (uint64_t i = 0; i < (n + 4095) / 4096; i++) res[w][dma] += vec[i] & 1;
            }
        }
    }
    fclose(f);
    LOGV("[rss] VmRSS %lu MiB (anon %lu, file %lu, shmem %lu); guest resident MiB: stack %llu, heap %llu, alias %llu (+ pool native buffers %llu)",
         rss >> 10, anon >> 10, file >> 10, shm >> 10, (unsigned long long)(res[0][0] >> 8), (unsigned long long)(res[1][0] >> 8), (unsigned long long)(res[2][0] >> 8),
         (unsigned long long)((res[0][1] + res[1][1] + res[2][1]) >> 8));
    void vk_res_pageout_report(void); vk_res_pageout_report();
}
static void seed_pages_drop_ranges(uint64_t *dropped) {
    FILE *f = fopen("/proc/self/maps", "r"); if (!f) return;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        if (!strstr(line, "seed.vkpack")) continue;
        unsigned long lo, hi; char perms[8];
        if (sscanf(line, "%lx-%lx %7s", &lo, &hi, perms) == 3 && perms[1] != 'w' && !madvise((void *)lo, hi - lo, MADV_DONTNEED)) *dropped += hi - lo;
    }
    fclose(f);
}
void vk_walker_keep(void) { if (g_is_walker) ohos_sched_self(1); }   /* vk_draw.c: from the walker's draws and dispatches */
static void *seed_drop_thread(void *arg) {
    long s = (long)(intptr_t)arg;
    pthread_setname_np(pthread_self(), "totk-seed-drop");   /* it was made by the walker and kept its name, nice -20 and CPUs */
    setpriority(PRIO_PROCESS, 0, 19);
    for (;;) { sleep((unsigned)s); uint64_t d = 0; seed_pages_drop_ranges(&d); guest_rss_report(); }
    return NULL;
}
static void seed_pages_drop(void) {
    static int on = -1; if (on < 0) { const char *e = getenv("TOTK_OHOS_SEED_DROP"); on = !(e && *e == '0'); }
    if (!on) return;
    uint64_t dropped = 0; seed_pages_drop_ranges(&dropped);
    char line[512];   /* the smaps walk below */
    static int started;
    if (!started) {
        started = 1; const char *e = getenv("TOTK_OHOS_SEED_DROP_S"); long s = e && *e ? atol(e) : 5;
        pthread_t th; if (s > 0 && !pthread_create(&th, NULL, seed_drop_thread, (void *)(intptr_t)s)) { pthread_detach(th); LOGV("seed pack pages also dropped every %ld s (TOTK_OHOS_SEED_DROP_S)", s); }
        /* the resident-set limit the system enforces on this app (RSS_THRESHOLD_KILLER): hidebug's
           OH_HiDebug_GetAppMemoryLimit (API 12), looked up at run time so the library links without libohhidebug */
        void *hd = dlopen("libohhidebug.so", RTLD_NOW | RTLD_LOCAL);
        void (*lim)(uint64_t *) = hd ? (void (*)(uint64_t *))dlsym(hd, "OH_HiDebug_GetAppMemoryLimit") : NULL;
        if (lim) { uint64_t l[2] = {0, 0}; lim(l); LOGV("app memory limit (hidebug): resident set %llu MiB, virtual %llu MiB", (unsigned long long)(l[0] >> 10), (unsigned long long)(l[1] >> 10)); }
        else LOGV("app memory limit (hidebug): not available (%s)", hd ? "no OH_HiDebug_GetAppMemoryLimit" : "no libohhidebug.so");
    }
    static int said; if (dropped && said++ < 2) LOGV("seed pack pages dropped from the resident set (%llu MiB mapped; TOTK_OHOS_SEED_DROP=0 keeps them)", (unsigned long long)(dropped >> 20));
    /* which mappings hold the resident set (the system's RSS_THRESHOLD_KILLER counts it): every tenth report, the six
       largest by name */
    static unsigned calls; if (calls++ % 10) return;
    FILE *sm = fopen("/proc/self/smaps", "r"); if (!sm) return;
    enum { NTOP = 64 }; struct { char name[96]; uint64_t kb; } top[NTOP]; int ntop = 0; char cur[96] = "[anon]";
    while (fgets(line, sizeof line, sm)) {
        unsigned long lo, hi; char perms[8]; int name_at = 0;
        if (sscanf(line, "%lx-%lx %7s %*s %*s %*s %n", &lo, &hi, perms, &name_at) >= 3 && name_at > 0) {
            const char *n = line + name_at; size_t l = strcspn(n, "\n");
            if (!l) snprintf(cur, sizeof cur, "[anon]"); else { const char *base = l > 90 ? n + l - 90 : n; snprintf(cur, sizeof cur, "%.*s", (int)(n + l - base), base); }
            continue;
        }
        unsigned long kb;
        if (sscanf(line, "Rss: %lu kB", &kb) == 1 && kb) {
            int k = 0; while (k < ntop && strcmp(top[k].name, cur)) k++;
            if (k == ntop && ntop < NTOP) { snprintf(top[ntop].name, sizeof top[ntop].name, "%s", cur); top[ntop++].kb = 0; }
            if (k < ntop) top[k].kb += kb;
        }
    }
    fclose(sm);
    for (int i = 0; i < ntop; i++) for (int j = i + 1; j < ntop; j++) if (top[j].kb > top[i].kb) { __typeof__(top[0]) t = top[i]; top[i] = top[j]; top[j] = t; }
    char out[900]; size_t o = 0;
    for (int i = 0; i < ntop && i < 6 && o + 120 < sizeof out; i++) o += (size_t)snprintf(out + o, sizeof out - o, " %s %llu MiB;", top[i].name, (unsigned long long)(top[i].kb >> 10));
    LOGV("resident by mapping:%s", out);
}
#endif
void vk_mem_report(void) {
#ifdef TOTK_OHOS
    seed_pages_drop();
#endif
    static int classes = -1;
    if (classes < 0) { const char *e = getenv("TOTK_VMA_CLASSES"); classes = e && !strcmp(e, "1"); }
    if (nvn_dirty_lag_enabled()) {
        uint64_t vma[5], scans[4]; nvn_dirty_lag_vma_stats(vma); nvn_dirty_lag_vma_scan_stats(scans, 1);
        LOGV("memory VMAs: %llu/%llu, guarded-run credit %llu/%llu, pressure fallback %llu; scans %llu (epoch-requested %llu), scan ms total/max %.3f/%.3f",
            (unsigned long long)vma[0], (unsigned long long)vma[1], (unsigned long long)vma[2],
            (unsigned long long)vma[3], (unsigned long long)vma[4],
            (unsigned long long)scans[0], (unsigned long long)scans[1], scans[2] / 1e6, scans[3] / 1e6);
    }
    if (classes) nvn_dirty_lag_vma_request(1);
    memory_objects_report();
    uint64_t chunk = 0, nchunk = 0, ndp = 0, posts = 0;
    for (int i = 0; i < RING; i++) { slot_t *s = &g_ring[i]; nchunk += (uint64_t)s->ar.nchunk; ndp += (uint64_t)s->ndpool; posts += (uint64_t)s->cap; for (int c = 0; c < s->ar.nchunk; c++) chunk += s->ar.chunks[c].size; }
    uint64_t slot_chunks = chunk;
    for (int i = 0; i < SNAP_ARENAS; i++) { nchunk += (uint64_t)g_sar[i].ar.nchunk; for (int c = 0; c < g_sar[i].ar.nchunk; c++) chunk += g_sar[i].ar.chunks[c].size; }
    uint64_t recorder[4]; vk_record_memory(recorder);
    LOGV("memory recording: slot/snapshot arenas retained %.1f/%.1f MiB; draw packet slabs %.1f MiB, descriptor input slabs %.1f MiB; worker secondary buffers %llu, descriptor pools %llu (driver bytes not counted)",
        slot_chunks / 1048576.0, (chunk - slot_chunks) / 1048576.0, recorder[0] / 1048576.0, recorder[1] / 1048576.0,
        (unsigned long long)recorder[2], (unsigned long long)recorder[3]);
    LOGV("memory staging retention: cap %d MiB per slot (-1: none), trims %llu, %.1f MiB not retained (TOTK_VK_STAGING_RETAIN_MB)",
        staging_retain_mb(), (unsigned long long)atomic_load_explicit(&g_staging_trims, memory_order_relaxed),
        atomic_load_explicit(&g_staging_trimmed, memory_order_relaxed) / 1048576.0);
    uint64_t primary[2]; primary_memory(primary);
    LOGV("memory primary command buffers: retained %llu, reclaimed %llu; retired-only trim after32 uses, warm floor256/slot (driver-private BO bytes not observable)",
        (unsigned long long)primary[0], (unsigned long long)primary[1]);
    uint64_t pn, pb; gpu_pool_totals(&pn, &pb);
    uint64_t r[8]; vk_res_mem(r);
    LOGV("memory image blocks: used/free %.3f/%.3f MiB (empty blocks retained for reuse)",
        r[7] / 1048576.0, (r[0] - r[7]) / 1048576.0);
#ifdef __GLIBC__
    struct mallinfo2 mi = mallinfo2();
#else
    struct mallinfo mi = mallinfo();
#endif
    LOGV("memory: malloc %zu MiB in use; image blocks %llu MiB + dedicated %llu MiB, %llu images retired waiting, %llu texture objects, %llu shared images, %llu write-back records, %llu retired list capacity; "
         "pools %llu (%llu MiB); transient chunks %llu (%llu MiB), descriptor pools %llu, post-op capacity %llu; uniform log %d (cap %d), snapshots %d (cap %d), GPU-write ranges %d (cap %d)",
         (size_t)mi.uordblks >> 20, (unsigned long long)(r[0] >> 20), (unsigned long long)(r[1] >> 20), (unsigned long long)r[2], (unsigned long long)r[3], (unsigned long long)r[4], (unsigned long long)r[5], (unsigned long long)r[6],
         (unsigned long long)pn, (unsigned long long)(pb >> 20), (unsigned long long)nchunk, (unsigned long long)(chunk >> 20), (unsigned long long)ndp, (unsigned long long)posts,
         g_nub, g_capub, g_nsnap, g_capsnap, g_ngw, g_capgw);
#ifdef TOTK_OHOS
    {   /* The pools that hold GPU memory (native buffers: the system counts them as graphics memory), by NVN flags and by
           how they got it, with how much of each the game's buffers cover (the page-rounded union; the rest is texture
           storage or free space, which the GPU never reads as memory), and the largest. */
        typedef struct { uintptr_t base, end; int flags, lazy, nbuf; uint64_t cover, hull; } census_t;
        typedef struct { int key, n, empty; uint64_t bytes, cover, hull, empty_bytes; } class_t;
        pthread_mutex_lock(&g_pool_mu);
        int np = 0; census_t *ps = malloc((size_t)(g_npool + 1) * sizeof *ps);
        for (int i = 0; ps && i < g_npool; i++) { pool_ent *q = g_pools[i]; if (q->kind != K_OWN) continue; ps[np++] = (census_t){q->base, q->end, q->flags, q->was_lazy, 0, 0, 0}; }
        pthread_mutex_unlock(&g_pool_mu);
        pthread_mutex_lock(&g_gbuf_mu);
        int nb = g_ngbuf; gbuf_t *bs = malloc((size_t)(nb + 1) * sizeof *bs); if (bs) memcpy(bs, g_gbuf, (size_t)nb * sizeof *bs); else nb = 0;
        pthread_mutex_unlock(&g_gbuf_mu);
        if (ps && np && !gbuf_on()) {
            uint64_t total = 0; for (int i = 0; i < np; i++) total += ps[i].end - ps[i].base;
            LOGV("memory: %d pools with GPU memory, %llu MiB (TOTK_OHOS_POOL_CENSUS=1: by flags, and what the game's buffers cover)", np, (unsigned long long)(total >> 20));
        } else if (ps && np) {
            if (nb) qsort(bs, (size_t)nb, sizeof *bs, gbuf_cmp);
            for (int i = 1; i < np; i++) { census_t t = ps[i]; int k = i; while (k > 0 && ps[k - 1].base > t.base) { ps[k] = ps[k - 1]; k--; } ps[k] = t; }   /* nearly sorted already */
            int j = 0; uint64_t total = 0, cover = 0, hull = 0; class_t cls[24]; int ncls = 0;
            for (int i = 0; i < np; i++) {
                census_t *p = &ps[i]; uintptr_t lo = 0, hi = 0, first = 0, last = 0;
                while (j < nb && bs[j].a < p->base) j++;
                for (; j < nb && bs[j].a < p->end; j++) {
                    uintptr_t a = bs[j].a & ~(uintptr_t)4095, b = (bs[j].b + 4095) & ~(uintptr_t)4095; if (b > p->end) b = p->end;
                    if (!p->nbuf++) { first = lo = a; hi = b; }
                    else if (a <= hi) { if (b > hi) hi = b; }
                    else { p->cover += hi - lo; lo = a; hi = b; }
                    if (b > last) last = b;
                }
                if (p->nbuf) { p->cover += hi - lo; p->hull = last - first; }
                uint64_t size = p->end - p->base; total += size; cover += p->cover; hull += p->hull;
                int key = (p->flags & 0xfff) | p->lazy << 12 | (size >= (4u << 20) ? 1 << 13 : 0), c = 0;
                while (c < ncls && cls[c].key != key) c++;
                if (c == ncls) { if (ncls == 24) c = 23; else cls[ncls++] = (class_t){key, 0, 0, 0, 0, 0, 0}; }
                cls[c].n++; cls[c].bytes += size; cls[c].cover += p->cover; cls[c].hull += p->hull; if (!p->nbuf) { cls[c].empty++; cls[c].empty_bytes += size; }
            }
            LOGV("memory: %d pools with GPU memory, %llu MiB; the game's %d buffers cover %llu MiB of them (whole pages; from each pool's first buffer to its last %llu MiB)",
                 np, (unsigned long long)(total >> 20), nb, (unsigned long long)(cover >> 20), (unsigned long long)(hull >> 20));
            for (int c = 0; c < ncls; c++)
                LOGV("memory: pools of NVN flags %#x, %s, %s: %d with %llu MiB of GPU memory, buffers cover %llu MiB (first to last %llu MiB); %d of them (%llu MiB) hold no buffer",
                     cls[c].key & 0xfff, cls[c].key >> 12 & 1 ? "GPU memory at first use" : "GPU memory from the start", cls[c].key >> 13 & 1 ? "4 MiB and up" : "under 4 MiB",
                     cls[c].n, (unsigned long long)(cls[c].bytes >> 20), (unsigned long long)(cls[c].cover >> 20), (unsigned long long)(cls[c].hull >> 20), cls[c].empty, (unsigned long long)(cls[c].empty_bytes >> 20));
            char line[900]; int o = snprintf(line, sizeof line, "memory: the largest pools with GPU memory (size / what buffers cover, MiB):");
            for (int r = 0; r < 14 && o < (int)sizeof line - 60; r++) {
                int best = -1; for (int i = 0; i < np; i++) if (ps[i].end && (best < 0 || ps[i].end - ps[i].base > ps[best].end - ps[best].base)) best = i;
                if (best < 0) break;
                o += snprintf(line + o, sizeof line - (size_t)o, " %#llx[%#x%s] %llu/%llu", (unsigned long long)ps[best].base, ps[best].flags & 0xfff, ps[best].lazy ? "L" : "",
                              (unsigned long long)((ps[best].end - ps[best].base) >> 20), (unsigned long long)(ps[best].cover >> 20));
                ps[best].end = 0;
            }
            LOGV("%s", line);
        }
        free(ps); free(bs);
    }
#endif
    if (g_watch_len) {
        unsigned units = (unsigned)((g_watch_len + (32u << 20) - 1) >> 25), used = 0; uint64_t w[4];
        for (int i = 0; i < 4; i++) { w[i] = atomic_load_explicit(&g_watch_bits[i], memory_order_relaxed); used += (unsigned)__builtin_popcountll(w[i]); }
        LOGV("memory: pool %#llx+%#llx, 32 MiB units the GPU read buffers from: %u of %u (%016llx %016llx %016llx); pools without GPU memory %llu MiB",
             (unsigned long long)g_watch_lo, (unsigned long long)g_watch_len, used, units, (unsigned long long)w[0], (unsigned long long)w[1], (unsigned long long)w[2], (unsigned long long)(g_skipped_bytes >> 20));
    }
}
uint64_t vk_submit_now(void) { return g_cur ? slot_submit(0, VK_PASS_END_SUBMISSION) : mtl_last_fence(); }
uint64_t vk_submit_readback(void) { return g_cur ? slot_submit(0, VK_PASS_END_CPU_WAIT) : mtl_last_fence(); }
void vk_flush_wait(void) { HK_ADD(idle_flush, 1); if (ub_private_on()) ub_flush(); if (g_cur) { uint64_t v = slot_submit(0, VK_PASS_END_CPU_WAIT); wait_value(v); } else vk_gpu_idle(); }
uint64_t mtl_last_fence(void) { pthread_mutex_lock(&g_mu); uint64_t f = g_reserved_fence; pthread_mutex_unlock(&g_mu); return f; }
int mtl_wait(uint64_t fence, uint64_t timeout_ns) {
    if (atomic_load(&g_done) >= fence) return 1;
    if (!timeout_ns) return 0;
    struct timespec dl; clock_gettime(CLOCK_REALTIME, &dl);
    uint64_t ns = (uint64_t)dl.tv_nsec + (timeout_ns > 3600000000000ull ? 3600000000000ull : timeout_ns);
    dl.tv_sec += (time_t)(ns / 1000000000u); dl.tv_nsec = (long)(ns % 1000000000u);
    uint64_t w0 = host_now_ns();
    pthread_mutex_lock(&g_mu);
    int blocked = atomic_load(&g_done) < fence;
    while (atomic_load(&g_done) < fence) if (pthread_cond_timedwait(&g_cv, &g_mu, &dl)) break;
    int ok = atomic_load(&g_done) >= fence;
    if (blocked && ok && g_done_at) HK_ADD(wake_ns, host_now_ns() - g_done_at);
    pthread_mutex_unlock(&g_mu);
    g_cum[2] += host_now_ns() - w0; g_cum[4]++;
    if (blocked) { HK_ADD(gwaits, 1); HK_ADD(gwait_ns, host_now_ns() - w0); }
    return ok;
}
int vk_gpuprof_renderer_drain(uint64_t timeout_ns) {
    uint64_t start = host_now_ns();
    struct timespec deadline; clock_gettime(CLOCK_REALTIME, &deadline);
    uint64_t ns = (uint64_t)deadline.tv_nsec + timeout_ns;
    deadline.tv_sec += (time_t)(ns / UINT64_C(1000000000));
    deadline.tv_nsec = (long)(ns % UINT64_C(1000000000));
    pthread_once(&g_rec_once, rec_mu_init);
    if (pthread_mutex_timedlock(&g_rec_mu, &deadline)) return 0;
    /* Enter the normal recursive owner bookkeeping after bounded admission. */
    vk_rec_lock();
    pthread_mutex_unlock(&g_rec_mu);
    uint64_t ticket = vk_submit_now();
    vk_rec_unlock();
    uint64_t elapsed = host_now_ns() - start;
    return mtl_wait(ticket, elapsed < timeout_ns ? timeout_ns - elapsed : 0);
}
void mtl_finish(void) { HK_ADD(idle_fin, 1); if (ub_private_on()) vk_flush_wait(); else vk_gpu_idle(); }
void mtl_drain(void) { if (ub_private_on()) vk_flush_wait(); else vk_gpu_idle(); }
uint64_t gpu_stat_submits(void) { return g_stat_sub; }
uint64_t gpu_stat_draws(void) { return g_stat_draw; }
