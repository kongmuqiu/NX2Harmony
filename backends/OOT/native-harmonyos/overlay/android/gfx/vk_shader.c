/* Shaders of the Vulkan backend (vk_int.h): the shader seed on the device, variants per bind state, SPIR-V modules,
   descriptor set layouts, pipeline layouts and compute pipelines.

   The seed (android/build_android.py --shaders) holds every shader of the game translated on the PC for its default
   bind state and for every bind state of the shared shader list, as on the iPhone:
     <variant>.spv   Vulkan SPIR-V from the Ryujinx translator (tools/totk_xlate)
     <variant>.vk    its Vulkan reflection (android/vk_reflect.py)
     <key>.vv        the shader's variants with the answers each was translated for
   A shader is identified as on the Mac: SHA-256 of its control section (ctl_key_len) and code. A variant is chosen by
   the same answers (texture slot types, image formats, vertex input and colour output classes) and the same per-stage
   signature as nvn_mtl.m prog_fit / stage_sig. A variant the seed lacks skips its draws and is recorded once under
   shader-requests/ for a later fill on the PC. If packaged, the low-priority on-device service also translates it;
   a publication epoch invalidates negative caches on the recording thread and the existing pipeline path takes over.

   Every stage of a translation uses descriptor set 0; the vertex (or compute) stage keeps it and the fragment stage's
   SPIR-V is patched to set 1, so one pipeline layout holds both. */
#include "vk_int.h"
#include "vk_alpha_test.h"
#include "vk_record.h"
#include "vk_framegen.h"
#include "vk_earlyz.h"
#include "nvn_hmap.h"
#include "vk_shader_xlate.h"
#include "vk_seed_io.h"
#include "vk_load_proof.h"
#include "vk_subgroup.h"
#include "vk_position.h"
#include <errno.h>
#include <inttypes.h>
#include <sys/stat.h>
#include <dirent.h>
#include <stdatomic.h>

#include "host_os.h"
int totk_depth_near_minus_w(void) __attribute__((weak));

static pthread_once_t g_position_once = PTHREAD_ONCE_INIT;
static unsigned g_position_policy;
static void position_init(void) {
    g_position_policy = vk_position_policy(getenv("TOTK_VK_POS_INVARIANT"), getenv("TOTK_VK_POS_NOCONTRACT"));
    LOGV("[nvn-vk] TOTK_VK_POS_INVARIANT=%u (startup-only): vertex Position Invariant", g_position_policy & 1);
    LOGV("[nvn-vk] TOTK_VK_POS_NOCONTRACT=%u (startup-only): position-feeding float arithmetic NoContraction", g_position_policy >> 1);
}

/* Folder selection is process-lifetime. Preserve legacy cache paths for the
   canonical game seed; only alternate folders use a folder/content namespace. */
static pthread_once_t g_seed_once = PTHREAD_ONCE_INIT;
static const char *g_seed_dir;
static char g_seed_tag[65];
static int g_seed_default;
static char g_seed_base[4096], g_seed_path[4096];
static void seed_init(void) {
    const char *selected = getenv("TOTK_SHADER_SEED_DIR");
    if (!selected || !*selected) { selected = "shaders"; g_seed_default = 1; }
    if (!g_seed_base[0] && !getcwd(g_seed_base, sizeof g_seed_base)) {
        g_seed_dir = "(game folder unavailable)"; return;
    }
    int n = selected[0] == '/' ? snprintf(g_seed_path, sizeof g_seed_path, "%s", selected) :
        snprintf(g_seed_path, sizeof g_seed_path, "%s/%s", g_seed_base, selected);
    if (n < 0 || (size_t)n >= sizeof g_seed_path) { g_seed_dir = "(shader seed path too long)"; return; }
    char canonical[4096], standard[4096], marker[4096], id[65] = {0};
    if (realpath(g_seed_path, canonical)) snprintf(g_seed_path, sizeof g_seed_path, "%s", canonical);
    g_seed_dir = g_seed_path;
#if defined(TOTK_GAME_V143)
    const char *versioned_default = "shaders/v143";
#else
    const char *versioned_default = "shaders/v0";
#endif
    const char *defaults[] = {"shaders", versioned_default};
    for (unsigned i = 0; i < sizeof defaults / sizeof *defaults; i++) {
        n = snprintf(standard, sizeof standard, "%s/%s", g_seed_base, defaults[i]);
        if (n >= 0 && (size_t)n < sizeof standard &&
            !strcmp(g_seed_dir, realpath(standard, canonical) ? canonical : standard)) g_seed_default = 1;
    }
    if (g_seed_default) return;
    if (snprintf(marker, sizeof marker, "%s/seed-id.txt", g_seed_dir) < (int)sizeof marker) {
        FILE *f = fopen(marker, "rb");
        if (f) { size_t size = fread(id, 1, 64, f); fclose(f); if (size != 64) memset(id, 0, sizeof id); }
    }
    host_sha256_ctx ctx; unsigned char digest[32];
    host_sha256_init(&ctx); host_sha256_update(&ctx, g_seed_dir, strlen(g_seed_dir) + 1);
    host_sha256_update(&ctx, id, sizeof id); host_sha256_final(&ctx, digest);
    for (int i = 0; i < 32; i++) snprintf(g_seed_tag + 2 * i, 3, "%02x", digest[i]);
}
static const char *seed_dir(void) { pthread_once(&g_seed_once, seed_init); return g_seed_dir; }
/* Alternate seeds and position precision arms namespace explicit cache/import
   paths too. Off/off with the canonical seed retains legacy paths. Caller owns out. */
const char *vk_shader_cache_path(const char *base, char *out, size_t size) {
    pthread_once(&g_seed_once, seed_init);
    pthread_once(&g_position_once, position_init);
    if (!base || !strcmp(base, "0")) return NULL;
    const char *suffix = vk_position_cache_suffix(g_position_policy);
#if defined(TOTK_OHOS) && defined(TOTK_GAME_V143)
    /* HarmonyOS keeps 1.0.0 and 1.4.3 in one working folder: the default seed's learned lists and caches
       (state/vk-seed-used.txt, vk-compute-used.bin, the pipeline cache, ...) get the version, so neither warms the other's keys. */
    int n = g_seed_default ? snprintf(out, size, "%s.v143%s", base, suffix) : snprintf(out, size, "%s.%s%s", base, g_seed_tag, suffix);
#else
    int n = g_seed_default ? snprintf(out, size, "%s%s", base, suffix) : snprintf(out, size, "%s.%s%s", base, g_seed_tag, suffix);
#endif
    return n >= 0 && (size_t)n < size ? out : NULL;
}
static void seed_io_start(void) {
    char path[1100];
    mkdir("state", 0700);
    vk_seed_io_start(seed_dir(), vk_shader_cache_path("state/vk-seed-used.txt", path, sizeof path));
}
static int g_seed_ok = -1;
int vk_shaders_ready(void) {
    if (g_seed_ok < 0) {
        if (vk_seed_io_enabled()) {
            seed_io_start();
            if (vk_seed_io_active()) {
                g_seed_ok = vk_seed_io_ready();
#if defined(TOTK_GAME_ZA) || defined(TOTK_GAME_OOT)
                /* Z-A starts from an empty seed: every shader it meets is requested for the PC fill
                   (fill_shaders_ohos.py --version za202), which is how the seed is built. */
                if (!g_seed_ok) { g_seed_ok = 1; LOGV("【红】shader seed: %s holds no .spv modules yet: every shader is requested for a PC fill; nothing is drawn until then", seed_dir()); return g_seed_ok; }
#endif
                if (!g_seed_ok) LOGV("【红】shader seed error: %s: indexed seed contains no .spv modules; nothing is drawn", seed_dir());
                else LOGV("shader seed: %s (indexed IO)", seed_dir());
                return g_seed_ok;
            }
        }
        const char *path = seed_dir(); DIR *dir = opendir(path); int opened = dir != NULL; g_seed_ok = 0;
        if (dir) {
            struct dirent *entry;
            while ((entry = readdir(dir))) {
                size_t n = strlen(entry->d_name); struct stat st;
                if (n > 4 && !strcmp(entry->d_name + n - 4, ".spv") &&
                    !fstatat(dirfd(dir), entry->d_name, &st, 0) && S_ISREG(st.st_mode)) { g_seed_ok = 1; break; }
            }
            closedir(dir);
        }
#if defined(TOTK_GAME_ZA) || defined(TOTK_GAME_OOT)
        if (!g_seed_ok && opened) { g_seed_ok = 1; LOGV("【红】shader seed: %s holds no .spv modules yet: every shader is requested for a PC fill", path); }
#endif
        if (!g_seed_ok) LOGV("【红】shader seed error: %s: %s; nothing is drawn", path,
            opened ? "no .spv modules" : "directory missing or inaccessible");
        else LOGV("shader seed: %s", path);
    }
    return g_seed_ok;
}
/* Frontend calls before renderer startup: relative selection must not depend on
   a later process-wide chdir by the guest/runtime. Keep this base process-owned. */
void vk_shader_seed_startup(const char *game_dir) {
    pthread_once(&g_position_once, position_init);
    if (!g_seed_dir && !realpath(game_dir, g_seed_base))
        snprintf(g_seed_base, sizeof g_seed_base, "%s", game_dir);
    if (vk_seed_io_enabled()) seed_io_start(); /* index/warming overlaps remaining boot work */
    else (void)vk_shaders_ready();
}
static uint64_t g_io_n, g_io_ns, g_io_max_ns;   /* seed files read since the last vk_shader_io_take (the stutter log) */
static _Atomic uint64_t g_io_cum_ns;            /* all seed reads (the long-frame log) */
void vk_shader_io_take(uint64_t *n, uint64_t *ns, uint64_t *max_ns) { *n = g_io_n; *ns = g_io_ns; *max_ns = g_io_max_ns; g_io_n = g_io_ns = g_io_max_ns = 0; vk_seed_io_report(); vk_compute_report(); }
uint64_t vk_shader_io_cum_ns(void) { return atomic_load(&g_io_cum_ns); }
static char *read_file_(const char *path, size_t *len);
static char *read_file(const char *path, size_t *len) {
    uint64_t t0 = host_now_ns();
    const char *name = strrchr(path, '/'); if (name) name++; else name = path;
    char *r = vk_seed_io_active() ? vk_seed_io_read(name, len) : read_file_(path, len);
    uint64_t dt = host_now_ns() - t0;
    g_io_n++; g_io_ns += dt; g_io_cum_ns += dt; if (dt > g_io_max_ns) g_io_max_ns = dt;
    return r;
}
static char *read_file_(const char *path, size_t *len) {
    return vk_seed_file_read(path, len);
}

/* ---------- answers (nvn_mtl.m rt_answers) as flat "kind:slot=value" items ---------- */
static const char *target_name(int t) {
    switch (t) { case 0: return "1d"; case 2: return "3d"; case 3: return "1darray"; case 4: return "2darray"; case 5: return "2dms";
                 case 6: return "2dmsarray"; case 8: return "cube"; case 9: return "cubearray"; case 10: return "buffer"; default: return "2d"; }
}
static const char *image_format(int f) {
    switch (f) {
        case 0x01: return "R8Unorm"; case 0x0D: return "R8G8Unorm"; case 0x25: return "R8G8B8A8Unorm"; case 0x27: return "R8G8B8A8Uint";
        case 0x05: return "R16Float"; case 0x11: return "R16G16Float"; case 0x29: return "R16G16B16A16Float"; case 0x08: return "R16Uint"; case 0x2C: return "R16G16B16A16Uint";
        case 0x0A: return "R32Float"; case 0x16: return "R32G32Float"; case 0x2E: return "R32G32B32A32Float"; case 0x0B: return "R32Uint"; case 0x0C: return "R32Sint";
        case 0x17: return "R32G32Uint"; case 0x2F: return "R32G32B32A32Uint"; case 0x3D: return "R10G10B10A2Unorm"; case 0x3F: return "R11G11B10Float";
    }
    return NULL;
}
static const char *class_name(int f) { int c = vk_fmt_class(f); return c == 1 ? "uint" : c == 2 ? "sint" : "float"; }
typedef struct { char tex[128][24]; char img[32][48]; char attr[16][8]; char fout[8][8]; uint8_t has_tex[128], has_img[32], has_attr[16], has_fout[8]; } answers_t;
static void answers_of(int st, answers_t *a) {
    memset(a, 0, sizeof *a);
    for (int r = 0; r < 128; r++) if (S.tex[st][r]) {
        int f, tg; if (!vk_treg_kind((int)(S.tex[st][r] & 0xfffff), &f, &tg)) continue;
        snprintf(a->tex[r], sizeof a->tex[r], "%s%s", target_name(tg), f >= 0x32 && f <= 0x36 ? "_depth" : ""); a->has_tex[r] = 1;
    }
    for (int r = 0; r < 32; r++) if (S.img[st][r]) {
        int f, tg; if (!vk_treg_kind((int)(S.img[st][r] & 0xfffff), &f, &tg)) continue;
        const char *fn = image_format(f);
        snprintf(a->img[r], sizeof a->img[r], "%s%s%s", target_name(tg), fn ? "/" : "", fn ? fn : ""); a->has_img[r] = 1;
    }
    if (st == 0) for (int i = 0; i < S.nattr && i < 16; i++) { snprintf(a->attr[i], sizeof a->attr[i], "%s", class_name(S.attr[i].fmt)); a->has_attr[i] = 1; }
    if (st == 1 && S.has_rt) for (int i = 0; i < S.rt.n && i < 8; i++) if (S.rt.color[i]) {
        int f = S.rt.has_cview[i] && S.rt.cview[i].set_format ? S.rt.cview[i].format : S.rt.color[i]->d.format;
        snprintf(a->fout[i], sizeof a->fout[i], "%s", class_name(f)); a->has_fout[i] = 1;
    }
}
/* one flat dependency "kind:slot=value" against the answers (deps_match: a missing answer is the default) */
static int dep_ok(const answers_t *a, const char *kind, int slot, const char *want) {
    const char *have;
    if (!strcmp(kind, "tex")) {
        have = slot >= 0 && slot < 128 && a->has_tex[slot] ? a->tex[slot] : "2d";
        /* Shared-list variants can bind a texture without querying its dimension.
           Those depend only on depth class, not on the concrete target kind. */
        if (!strcmp(want, "*") || !strcmp(want, "*_depth")) {
            size_t n = strlen(have);
            int depth = n >= 6 && !strcmp(have + n - 6, "_depth");
            return depth == !strcmp(want, "*_depth");
        }
    }
    else if (!strcmp(kind, "img")) have = slot >= 0 && slot < 32 && a->has_img[slot] ? a->img[slot] : "";
    else if (!strcmp(kind, "attr")) have = slot >= 0 && slot < 16 && a->has_attr[slot] ? a->attr[slot] : "float";
    else if (!strcmp(kind, "fout")) have = slot >= 0 && slot < 8 && a->has_fout[slot] ? a->fout[slot] : "float";
    else return 0;
    return !strcmp(have, want);
}
static int deps_match(const char *deps, const answers_t *a) {
    char buf[4096]; snprintf(buf, sizeof buf, "%s", deps);
    for (char *save = NULL, *it = strtok_r(buf, ";", &save); it; it = strtok_r(NULL, ";", &save)) {
        char *colon = strchr(it, ':'), *eq = colon ? strchr(colon, '=') : NULL;
        if (!colon || !eq) return 0;
        *colon = 0; *eq = 0;
        if (!dep_ok(a, it, atoi(colon + 1), eq + 1)) return 0;
    }
    return 1;
}

/* ---------- shaders ---------- */
typedef struct { char variant[80]; uint32_t early_tests, position_policy; uint64_t stencil; uint32_t tf, pruned; uint32_t keep[4]; } shader_key;   /* pruned, keep (oot-a108): a vertex shader without the stores of output components outside keep (vk_shader_prune) */   /* tf: transform feedback capture variant (vk_tf.inc) */   /* stencil: bindings patched to read a stencil view (vk_stencil_view.inc) */
static nvn_hmap g_shaders = {.klen = sizeof(shader_key)};
static int earlyz_on(void) { static int v = -1; if (v < 0) v = vk_earlyz_enabled(getenv("TOTK_VK_EARLYZ")); return v; }
static inline uint32_t ctl_key_len(const uint8_t *control) { uint32_t n; memcpy(&n, control + 0x20, 4); return n >= 0x7cc && n < 0x880 ? n : 0x880; }
/* Switch 2 control sections (NVN2: magic 0x98761234, version 0xc9; layout as MacNativeGames main's d2r_gfx nvn_ctl.h, read
   from seedds/nxnative, MIT): +0x38 the instructions' length (the graphics stages' 0x80-byte SPH included), +0x40 / +0x44 the
   c1 segment in the code area, +0x44 the code area's length, +0x56 the stage byte (5 compute, 6 task, 7 mesh), +0x58 / +0x64
   compute's thread group and shared memory; a graphics stage's type is in the SPH at the code's start. The key hashes the
   first 0x48 bytes with +0x30 as zero, then the code area (native-harmonyos/oot/oot_seed.py nvn2_key, nxnative's rule). */
static inline int ctl_nvn2(const uint8_t *c) { uint32_t m, v; memcpy(&m, c, 4); memcpy(&v, c + 4, 4); return m == 0x98761234u && v == 0xc9; }
static inline uint32_t ctl_code_length(const uint8_t *c) { uint32_t n; memcpy(&n, c + (ctl_nvn2(c) ? 0x44 : 0x704), 4); return n; }
/* The NVN stage (0 vertex, 1 fragment, 2 geometry, 3 / 4 tessellation, 5 compute); -1: not a stage, -6 / -7: NVN2 task /
   mesh (no translation chain yet). */
int vk_nvn2;   /* an NVN2 (Switch 2) shader was seen: vk_draw.c driver_bank_fill writes NVN2's own driver words too */
static inline int ctl_nvn_stage(const uint8_t *c, const uint8_t *code) {
    if (!ctl_nvn2(c)) { uint32_t st; memcpy(&st, c + 0x714, 4); return st <= 5 ? (int)st : -1; }
    vk_nvn2 = 1;
    if (c[0x56] == 5) return 5;
    if (c[0x56] == 6 || c[0x56] == 7) return -(int)c[0x56];
    uint32_t w0; memcpy(&w0, code, 4);
    static const int from_sph[6] = {0, 0, 3, 4, 2, 1};
    unsigned type = (w0 >> 10) & 0xf;
    return (w0 & 0x1f) && type <= 5 ? from_sph[type] : -1;
}
static inline void ctl_key_hash(host_sha256_ctx *ctx, const uint8_t *c) {
    if (ctl_nvn2(c)) { uint8_t k[0x48]; memcpy(k, c, sizeof k); memset(k + 0x30, 0, 4); host_sha256_update(ctx, k, sizeof k); return; }
    host_sha256_update(ctx, c, ctl_key_len(c));
}
static VkShaderStageFlags stage_flags(int st) { return st == 0 ? VK_SHADER_STAGE_VERTEX_BIT : st == 1 ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_COMPUTE_BIT; }
static void patch_set(uint32_t *w, size_t nw, uint32_t set) {
    for (size_t i = 5; i < nw; ) {
        uint32_t op = w[i] & 0xffff, n = w[i] >> 16; if (!n) break;
        if (op == 71 && n == 4 && w[i + 2] == 34 && w[i + 3] == 0) w[i + 3] = set;   /* OpDecorate <id> DescriptorSet 0 -> <set> (the bindless set stays) */
        i += n;
    }
}
/* Storage bindings the shader writes (TOTK, 2026-10-22). The reflection's rw was never set for storage buffers (the
   translator tested the wrong flag bit; nvn_mtl.m msl_stores_through found the same on the Mac), so every storage
   buffer counted as read-only: no "GPU writes here" mark, and uniform snapshots taken at submit kept the CPU's older
   bytes of buffers the GPU writes. The material shaders store a per-tile class flag that the next frame's class marker
   reads to decide which lighting draws light a pixel. Here the SPIR-V itself is read: a binding is rw when an OpStore,
   OpCopyMemory target, atomic or function-call argument reaches it through access chains. */
static int spirv_mark_stores(vk_shader *sh, const uint32_t *w, size_t nw) {
    static int on = -1; if (on < 0) on = !vk_off("storagewrites"); if (!on) return 0;
    if (nw < 5 || w[0] != 0x07230203) return 0;
    uint32_t bound = w[3]; if (!bound || bound > (1u << 22)) return 0;
    int32_t *bind = malloc(bound * sizeof *bind); uint32_t *root = malloc(bound * sizeof *root); uint8_t *isbuf = calloc(bound, 1), *stored = calloc(bound, 1);
    if (!bind || !root || !isbuf || !stored) { free(bind); free(root); free(isbuf); free(stored); return 0; }
    for (uint32_t i = 0; i < bound; i++) { bind[i] = -1; root[i] = 0; }
    #define ID_OK(x) ((x) && (x) < bound)
    for (size_t i = 5; i < nw; ) {
        uint32_t op = w[i] & 0xffff, n = w[i] >> 16; if (!n || i + n > nw) break;
        const uint32_t *a = w + i + 1;
        switch (op) {
            case 71: if (n >= 4 && a[1] == 33 && ID_OK(a[0])) bind[a[0]] = (int32_t)a[2]; break;   /* OpDecorate Binding */
            case 59: if (n >= 4 && ID_OK(a[1]) && (a[2] == 12 || a[2] == 2)) { isbuf[a[1]] = 1; root[a[1]] = a[1]; } break;   /* OpVariable StorageBuffer / Uniform */
            case 65: case 66: case 67: case 70: case 83: case 124:   /* access chains, OpCopyObject, OpBitcast */
                if (n >= 4 && ID_OK(a[1]) && ID_OK(a[2]) && root[a[2]]) root[a[1]] = root[a[2]]; break;
            case 62: case 63: if (n >= 3 && ID_OK(a[0]) && root[a[0]]) stored[root[a[0]]] = 1; break;   /* OpStore, OpCopyMemory target */
            case 228: if (n >= 2 && ID_OK(a[0]) && root[a[0]]) stored[root[a[0]]] = 1; break;   /* OpAtomicStore */
            case 57: for (uint32_t k = 3; k + 1 < n; k++) if (ID_OK(a[k]) && root[a[k]]) stored[root[a[k]]] = 1; break;   /* OpFunctionCall: a pointer argument may be written */
            default:
                if (((op >= 229 && op <= 242) || op == 6035 || op == 5614 || op == 5615) && n >= 4 && ID_OK(a[2]) && root[a[2]]) stored[root[a[2]]] = 1;   /* atomics */
                break;
        }
        i += n;
    }
    #undef ID_OK
    int marked = 0;
    for (uint32_t v = 1; v < bound; v++) if (isbuf[v] && stored[v] && bind[v] >= 0)
        for (int k = 0; k < sh->nb; k++) if (sh->b[k].binding == (uint16_t)bind[v] && !sh->b[k].rw && (sh->b[k].dtype == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER || sh->b[k].dtype == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC)) { sh->b[k].rw = 1; marked++; }
    free(bind); free(root); free(isbuf); free(stored);
    return marked;
}
/* Colour locations of the fragment shader's Output variables, arrays included: the seed's fout line names each
   variable's first location only. TOTK_VK_UNWRITTEN_MASK masks attachments outside this set, so it must not miss one. */
static uint32_t spirv_fout_locations(const uint32_t *w, size_t nw) {
    if (nw < 5 || w[0] != 0x07230203) return 0;
    uint32_t bound = w[3]; if (!bound || bound > (1u << 22)) return 0;
    int32_t *location = malloc(bound * sizeof *location); uint32_t *value = calloc(bound, sizeof *value), *count = calloc(bound, sizeof *count), *pointee = calloc(bound, sizeof *pointee);
    if (!location || !value || !count || !pointee) { free(location); free(value); free(count); free(pointee); return 0; }
    for (uint32_t i = 0; i < bound; i++) location[i] = -1;
    uint32_t mask = 0;
    #define ID_OK(x) ((x) && (x) < bound)
    for (size_t i = 5; i < nw; ) {
        uint32_t op = w[i] & 0xffff, n = w[i] >> 16; if (!n || i + n > nw) break;
        const uint32_t *a = w + i + 1;
        switch (op) {
            case 71: if (n >= 4 && a[1] == 30 && ID_OK(a[0])) location[a[0]] = (int32_t)a[2]; break;   /* OpDecorate Location */
            case 43: if (n >= 4 && ID_OK(a[1])) value[a[1]] = a[2]; break;                              /* OpConstant (low word) */
            case 28: if (n >= 4 && ID_OK(a[0]) && ID_OK(a[2]))                                          /* OpTypeArray */
                         count[a[0]] = value[a[2]] * (ID_OK(a[1]) && count[a[1]] ? count[a[1]] : 1); break;
            case 32: if (n >= 4 && ID_OK(a[0])) pointee[a[0]] = a[2]; break;                            /* OpTypePointer */
            case 59:                                                                                    /* OpVariable Output */
                if (n >= 4 && a[2] == 3 && ID_OK(a[0]) && ID_OK(a[1]) && location[a[1]] >= 0) {
                    uint32_t t = pointee[a[0]], span = ID_OK(t) && count[t] ? count[t] : 1;
                    for (uint32_t k = 0; k < span && location[a[1]] + k < 8; k++) mask |= 1u << (location[a[1]] + k);
                }
                break;
        }
        i += n;
    }
    #undef ID_OK
    free(location); free(value); free(count); free(pointee);
    return mask;
}
/* DEPTH16 nudge for depth the fragment shader writes (TOTK, 2026-10-22; vk_draw.c d16tie_on). Every store to the
   FragDepth built-in becomes value + a float specialization constant (id D16_SPEC_ID, default 0), set to the nudge in
   pipelines that draw into a DEPTH16 target. Returns a new word array (caller frees) or NULL when nothing to patch. */
static uint32_t *spirv_depth_nudge(const uint32_t *w, size_t nw, size_t *out_nw) {
    if (nw < 5 || w[0] != 0x07230203) return NULL;
    uint32_t bound = w[3], fragdepth = 0, tfloat = 0; size_t after_float = 0, first_dec = 0; int stores = 0;
    for (size_t i = 5; i < nw; ) {
        uint32_t op = w[i] & 0xffff, n = w[i] >> 16; if (!n || i + n > nw) return NULL;
        if (op == 71 && !first_dec) first_dec = i;
        if (op == 71 && n >= 4 && w[i + 2] == 11 && w[i + 3] == 22) fragdepth = w[i + 1];   /* BuiltIn FragDepth */
        if (op == 22 && n >= 3 && w[i + 2] == 32 && !tfloat) { tfloat = w[i + 1]; after_float = i + n; }   /* OpTypeFloat 32 */
        i += n;
    }
    if (!fragdepth || !tfloat || !first_dec) return NULL;
    for (size_t i = 5; i < nw; ) { uint32_t op = w[i] & 0xffff, n = w[i] >> 16; if (op == 62 && n >= 3 && w[i + 1] == fragdepth) stores++; i += n; }
    if (!stores) return NULL;
    uint32_t *o = malloc((nw + 4 + 4 + (size_t)stores * 5) * sizeof *o); size_t k = 0;
    uint32_t spec = bound++;
    for (size_t i = 0; i < 5; i++) o[k++] = w[i];
    for (size_t i = 5; i < nw; ) {
        uint32_t op = w[i] & 0xffff, n = w[i] >> 16;
        if (i == first_dec) { o[k++] = 4u << 16 | 71; o[k++] = spec; o[k++] = 1; o[k++] = D16_SPEC_ID; }   /* OpDecorate spec SpecId */
        if (op == 62 && n >= 3 && w[i + 1] == fragdepth) {
            uint32_t t = bound++;
            o[k++] = 5u << 16 | 129; o[k++] = tfloat; o[k++] = t; o[k++] = w[i + 2]; o[k++] = spec;   /* OpFAdd */
            o[k++] = w[i]; o[k++] = w[i + 1]; o[k++] = t; for (uint32_t j = 3; j < n; j++) o[k++] = w[i + j];   /* OpStore FragDepth t */
        } else for (uint32_t j = 0; j < n; j++) o[k++] = w[i + j];
        i += n;
        if (i == after_float) { o[k++] = 4u << 16 | 50; o[k++] = tfloat; o[k++] = spec; o[k++] = 0; }   /* OpSpecConstant float 0.0 */
    }
    o[3] = bound; *out_nw = k;
    return o;
}
/* The translator declares FragCoord even for shaders that only read w for
   interpolation. Only xy (or an unclassified vector/alias use) requires 1x.
   Pointer provenance covers direct/access-chain/copy aliases; unknown uses of
   the builtin fail closed. No seed/reflection format change is required. */
static int spirv_fragcoord_xy(const uint32_t *w, size_t nw) {
    if (nw < 5 || w[0] != 0x07230203 || !w[3] || w[3] > (1u << 22)) return 1;
    uint32_t bound = w[3], root = 0;
    for (size_t i = 5; i < nw; ) {
        uint32_t n = w[i] >> 16, op = w[i] & 0xffff;
        if (!n || i + n > nw) return 1;
        if (op == 71 && n >= 4 && w[i+2] == 11 && w[i+3] == 15) root = w[i+1];
        if (op == 72 && n >= 5 && w[i+3] == 11 && w[i+4] == 15) return 1;
        i += n;
    }
    if (!root || root >= bound) return 0;
    uint8_t *ptr = calloc(bound, 1), *constant = calloc(bound, 1);
    if (!ptr || !constant) DIE("native UI: out of memory classifying FragCoord");
    ptr[root] = 15; int result = 0, function = 0;
    for (size_t i = 5; i < nw && !result; i += w[i] >> 16) {
        uint32_t n = w[i] >> 16, op = w[i] & 0xffff;
        if (op == 54) function = 1;
        if (op == 43 && n == 4 && w[i+2] < bound && w[i+3] < 4) constant[w[i+2]] = (uint8_t)(w[i+3] + 1);
        if ((op == 65 || op == 66) && n >= 5 && w[i+2] < bound && w[i+3] < bound && ptr[w[i+3]]) {
            uint8_t mask = ptr[w[i+3]], index = w[i+4] < bound ? constant[w[i+4]] : 0;
            ptr[w[i+2]] = mask == 15 ? (index ? 1u << (index-1) : 15) : mask;
        } else if (op == 83 && n == 4 && w[i+2] < bound && w[i+3] < bound) ptr[w[i+2]] = ptr[w[i+3]];
        else if (op == 61 && n >= 4 && w[i+3] < bound && (ptr[w[i+3]] & 3)) result = 1;
        else if (function) {
            for (uint32_t j = 1; j < n; j++) if (w[i+j] < bound && (ptr[w[i+j]] & 3)) result = 1;
        }
    }
    free(ptr); free(constant); return result;
}
static int spirv_image_features_ok(const uint32_t *w, size_t nw, int can_read, int can_write) {
    if (nw < 5 || w[0] != 0x07230203) return 0;
    for (size_t i = 5; i < nw; ) {
        uint32_t n = w[i] >> 16, op = w[i] & 0xffff;
        if (!n || n > nw - i) return 0;
        if (op == 17 && n == 2 && ((w[i + 1] == 55 && !can_read) || (w[i + 1] == 56 && !can_write))) return 0;
        i += n;
    }
    return 1;
}
#ifdef TOTK_OHOS
/* Compute shaders that return early in some invocations and then reach workgroup barriers ("if (!c) return; ...
   barrier()"): Maxwell (and Adreno) count exited invocations as arrived; Maleoon waits for them forever and the GPU is
   reset (device lost; first seen in the auto-exposure reduction, native-harmonyos/ohos/vk_gpu_trace.c). Where every
   barrier of the entry function sits at its top level (in no selection or loop construct), each live invocation
   executes the same barriers in the same order, so an early return is given those that follow it: the exiting
   invocation passes them, then returns. Returns 1 when the module was rewritten (*w replaced, *nw updated), 0 when it
   needed nothing, -1 when barriers are nested (left as is, logged). */
static int spirv_barrier_safe_returns(uint32_t **pw, size_t *pnw) {
    uint32_t *w = *pw; size_t nw = *pnw;
    if (nw < 5 || w[0] != 0x07230203u) return 0;
    uint32_t entry = 0; int compute = 0;
    for (size_t i = 5; i < nw; ) { uint32_t op = w[i] & 0xffffu, len = w[i] >> 16; if (!len || i + len > nw) return 0;
        if (op == 15 && len >= 3 && w[i + 1] == 5) { compute = 1; entry = w[i + 2]; }   /* OpEntryPoint GLCompute */
        if (op == 54) break;                                                         /* first OpFunction */
        i += len; }
    if (!compute) return 0;
    /* the entry function's range and its structure */
    size_t fbeg = 0, fend = 0;
    for (size_t i = 5; i < nw; ) { uint32_t op = w[i] & 0xffffu, len = w[i] >> 16;
        if (op == 54 && w[i + 2] == entry) fbeg = i;
        if (op == 56 && fbeg && !fend) { fend = i; break; }
        i += len; }
    if (!fbeg || !fend) return 0;
    enum { MAXB = 64, MAXR = 64, MAXD = 64 };
    size_t bar[MAXB]; int nbar = 0; size_t ret[MAXR]; int nret = 0; uint32_t stack[MAXD]; int depth = 0, nested = 0;
    for (size_t i = fbeg; i < fend; ) { uint32_t op = w[i] & 0xffffu, len = w[i] >> 16;
        if (op == 248) { while (depth && stack[depth - 1] == w[i + 1]) depth--; }      /* OpLabel: a construct ends at its merge */
        if ((op == 247 || op == 246) && depth < MAXD) stack[depth++] = w[i + 1];      /* OpSelectionMerge / OpLoopMerge */
        if (op == 224) { if (depth) nested = 1; if (nbar < MAXB) bar[nbar++] = i; else nested = 1; }   /* OpControlBarrier */
        if (op == 253 && depth && nret < MAXR) ret[nret++] = i;                       /* OpReturn inside a construct */
        i += len; }
    int early = 0; for (int r = 0; r < nret; r++) if (nbar && ret[r] < bar[nbar - 1]) early++;
    if (!early) return 0;
    if (nested) return -1;
    /* Preferred: one barrier after the early returns, and between the first return's construct and that barrier no
       write other than to Function variables: the barrier moves up in front of that construct, where every invocation
       still runs, so all of them meet at the same instruction (Maleoon apparently needs that: a barrier copy on the
       exit path still hung). The shared-memory writes it publishes all happen before the construct. */
    if (nbar == 1) {
        size_t hdr = 0; int hdr_depth = -1, d = 0; uint32_t st[MAXD];   /* the construct holding the first early return, and its depth */
        for (size_t i = fbeg; i < ret[0]; ) { uint32_t op = w[i] & 0xffffu, len = w[i] >> 16;
            if (op == 248) { while (d && st[d - 1] == w[i + 1]) d--; }
            if (op == 247) { hdr = i; hdr_depth = d; }
            if ((op == 247 || op == 246) && d < MAXD) st[d++] = w[i + 1];
            i += len; }
        if (hdr_depth != 0) hdr = 0;   /* only a top-level construct: the hoisted barrier must stay in uniform control flow */
        /* storage classes of pointers: OpVariable gives one, access chains inherit their base's */
        uint32_t bound = w[3]; uint8_t *sc = bound < (1u << 22) ? calloc(bound, 1) : NULL; int ok = hdr && sc;
        for (size_t i = 5; ok && i < nw; ) { uint32_t op = w[i] & 0xffffu, len = w[i] >> 16;
            if (op == 59 && len >= 4 && w[i + 2] < bound) sc[w[i + 2]] = (uint8_t)(w[i + 3] + 1);              /* OpVariable */
            if ((op == 65 || op == 66) && len >= 4 && w[i + 2] < bound && w[i + 3] < bound) sc[w[i + 2]] = sc[w[i + 3]];   /* access chains */
            i += len; }
        for (size_t i = hdr; ok && i < bar[0]; ) { uint32_t op = w[i] & 0xffffu, len = w[i] >> 16;
            if (op == 62 && !(w[i + 1] < bound && sc[w[i + 1]] == 7 + 1)) ok = 0;   /* OpStore to anything but Function */
            if (op == 99 || (op >= 227 && op <= 242) || op == 224 || op == 225) ok = 0;   /* image writes, atomics, other barriers */
            i += len; }
        free(sc);
        if (ok) {
            uint32_t *o = malloc(nw * 4); if (!o) return 0;
            size_t on = 0;
            memcpy(o + on, w, hdr * 4); on += hdr;
            memcpy(o + on, w + bar[0], 16); on += 4;                                  /* the barrier, hoisted */
            memcpy(o + on, w + hdr, (bar[0] - hdr) * 4); on += bar[0] - hdr;
            memcpy(o + on, w + bar[0] + 4, (nw - bar[0] - 4) * 4); on += nw - bar[0] - 4;
            free(w); *pw = o; *pnw = on;
            return 2;
        }
    }
    size_t add = 0;
    for (int r = 0; r < nret; r++) for (int b = 0; b < nbar; b++) if (bar[b] > ret[r]) add += 4;
    uint32_t *o = malloc((nw + add) * 4); if (!o) return 0;
    size_t on = 0, from = 0;
    for (int r = 0; r < nret; r++) {
        memcpy(o + on, w + from, (ret[r] - from) * 4); on += ret[r] - from; from = ret[r];
        for (int b = 0; b < nbar; b++) if (bar[b] > ret[r]) { memcpy(o + on, w + bar[b], 16); on += 4; }
    }
    memcpy(o + on, w + from, (nw - from) * 4); on += nw - from;
    free(w); *pw = o; *pnw = on;
    return 1;
}
#endif
#ifdef TOTK_OHOS
#include "vk_layer_gs.inc"
#include "vk_stencil_view.inc"
#ifdef TOTK_OHOS
#include "vk_usage_cond.h"
#endif
#include "vk_tf.inc"
static za_tf_layout g_tf_pending;   /* the layout shader_load_x applies (recording thread) */
/* The capture's fragment shader (vk_tf.inc tf_fs_build): storage buffers at bindings 0.. of set 1 for the feedback
   buffers in use, NVN fragment storage slots 12+b (vk_draw.c binds the feedback buffers there for the capture draw),
   then the fragment driver segment. Storage buffers bound at the aligned address below theirs (TOTK_VK_SB_ALIGN equal
   to the device's alignment, as vk_draw.c vk_sb_align_down) make the shader add the address's low bits. */
static vk_shader *tf_fs_make(const za_tf_layout *L, uint32_t first_loc, const char *variant) {
    VkDeviceSize align = V.props.limits.minStorageBufferOffsetAlignment;
    const char *e = getenv("TOTK_VK_SB_ALIGN"); long sba = e && *e ? strtol(e, NULL, 10) : 0;
    uint32_t sb_mask = sba > 0 && (VkDeviceSize)sba == align && align <= 4096 ? (uint32_t)align - 1u : 0;
    size_t nw = 0; uint32_t *w = tf_fs_build(L, first_loc, sb_mask, &nw);
    if (!w) return NULL;
    vk_shader *fs = calloc(1, sizeof *fs);
    if (!fs) { free(w); return NULL; }
    snprintf(fs->key, sizeof fs->key, "%.64s-tf", variant);
    fs->stage = 1; fs->spv_bytes = nw * 4; fs->local[0] = fs->local[1] = fs->local[2] = 1;
    fs->b = calloc(5, sizeof *fs->b);
    if (!fs->b) { free(w); free(fs); return NULL; }
    for (int b = 0, k = 0; b < 4; b++) if (L->stride[b])
        fs->b[fs->nb++] = (vk_bind){(uint16_t)k++, 1, RB_SBUF, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 0, 1, 0, (int16_t)(12 + b), -1, 0, 0};
    fs->b[fs->nb] = (vk_bind){(uint16_t)fs->nb, 1, RB_DRV0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 0, 0, 0, -1, -1, 0, 0}; fs->nb++;
    fs->has_storage_writes = 1;
    { static int said; if (!said++) LOGV("transform feedback: capture fragment shaders add guest address bits %#x (TOTK_VK_SB_ALIGN %ld, device alignment %llu)", sb_mask, sba, (unsigned long long)align); }
    VkShaderModuleCreateInfo mci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = nw * 4, .pCode = w};
    VkResult r = vkCreateShaderModule(V.dev, &mci, NULL, &fs->mod);
    free(w);
    if (r) { RED(8, "transform feedback: capture fragment shader of %s: %d", variant, r); free(fs->b); free(fs); return NULL; }
    VkDescriptorSetLayoutBinding lb[5];
    for (int i = 0; i < fs->nb; i++) lb[i] = (VkDescriptorSetLayoutBinding){fs->b[i].binding, (VkDescriptorType)fs->b[i].dtype, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL};
    VkDescriptorSetLayoutCreateInfo dci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = (uint32_t)fs->nb, .pBindings = lb};
    r = vkCreateDescriptorSetLayout(V.dev, &dci, NULL, &fs->dsl);
    if (r) { RED(8, "transform feedback: capture fragment set layout of %s: %d", variant, r); vkDestroyShaderModule(V.dev, fs->mod, NULL); free(fs->b); free(fs); return NULL; }
    return fs;
}
#endif
int vk_treg_stencil(int tid);   /* vk_res.c: the registered view reads the stencil plane */
static int hexval(int c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }
#ifdef TOTK_OHOS
/* What decides whether a fragment shader may take its depth and stencil tests early. 1: it can discard (OpKill,
   OpTerminateInvocation, OpDemoteToHelperInvocation); 2: it writes depth, the sample mask or the stencil reference;
   4: it has an atomic write (the pool usage bitmap of prepare_xlate_ns2.py: OpAtomicOr; the atomics are 228..242). */
enum { FT_KILL = 1, FT_DEPTH = 2, FT_ATOMIC = 4, FT_OWN = 8 };   /* 8: a side effect that is not the bitmap's OpAtomicOr: another atomic, an image write */
static int spirv_frag_traits(const uint32_t *w, size_t nw) {
    int t = 0;
    for (size_t i = 5; i < nw; ) {
        uint32_t op = w[i] & 0xffffu, n = w[i] >> 16;
        if (!n || n > nw - i) break;
        if (op == 252 || op == 4416 || op == 5380) t |= FT_KILL;
        else if (op == 71 && n >= 4 && w[i + 2] == 11 && (w[i + 3] == 22 || w[i + 3] == 20 || w[i + 3] == 5014)) t |= FT_DEPTH;
        else if (op == 16 && n >= 3 && (w[i + 2] == 12 || w[i + 2] == 5027)) t |= FT_DEPTH;
        else if (op == 241) t |= FT_ATOMIC;
        else if ((op >= 228 && op <= 242) || op == 99) t |= FT_OWN;
        i += n;
    }
    return t;
}
/* TOTK_OHOS_EARLYZ_AUTO (default 2). The translator makes a fragment shader that samples the texture pool OR a bit
   into the usage bitmap. With that side effect and no EarlyFragmentTests the shader must run for every fragment, also
   those the depth test rejects (the write is observable), so the driver can neither test depth early nor remove hidden
   surfaces: every layer of overdraw is shaded in full. On the console the same shader has no side effect and the driver
   tests early by itself. So such a shader gets the mode where the results are the same:
     1  a shader without discard and without a depth / sample mask output, always (vk_shader_depth_tests gives a draw
        with alpha to coverage that writes depth or stencil the twin without the mode: coverage is dropped after the
        shader);
     2  also a shader that can discard, in a draw that writes neither depth nor stencil (depth write off, no stencil
        test): what a late discard would have kept from being written is nothing. Its twin with the mode is chosen a
        draw (vk_shader_depth_tests), as the alpha-test variants are;
     3  both, also for shaders without the bitmap write (experiment).
   The bitmap then reports textures of visible fragments only. 0: off. */
static int earlyz_auto(void) { static int v = -1; if (v < 0) { const char *e = getenv("TOTK_OHOS_EARLYZ_AUTO"); v = e && *e ? atoi(e) : 2; } return v; }
static _Atomic uint64_t g_ez_n[6];   /* fragment shaders loaded: with the mode from the start; twins with it; twins without it; that write depth; the others; of those, the ones that write the bitmap and can discard */
#endif
#ifdef TOTK_OHOS
/* ---------- oot-a106: fragment twins that time what a material draw costs (the picture is wrong while one is on) ----------
   TOTK_OHOS_DC_FSNULL=1  shaders of DC_FS_MIN_BYTES or more return at once (no varying is read);
   TOTK_OHOS_DC_FSVARY=1  they add up their vec4 inputs into their vec4 outputs and return (the varyings live, the body
                          does not run);
   TOTK_OHOS_DC_NOUSED=1  shaders that sample the texture pool lose the usage bitmap's OpAtomicOr (no side effect left).
   Read once; the first that is set wins. vk_shader_dc picks the twin per draw (vk_draw.c draw_inner, TOTK_OHOS_DC_W). */
enum { DC_FS_NULL = 1, DC_FS_VARY = 2, DC_FS_NOUSED = 3, DC_FS_MIN_BYTES = 8192 };
static int dc_fs_mode(void) {
    static int v = -1;
    if (v < 0) {
        const char *a = getenv("TOTK_OHOS_DC_FSNULL"), *b = getenv("TOTK_OHOS_DC_FSVARY"), *c = getenv("TOTK_OHOS_DC_NOUSED");
        v = a && *a == '1' ? DC_FS_NULL : b && *b == '1' ? DC_FS_VARY : c && *c == '1' ? DC_FS_NOUSED : 0;
        /* (oot-a107) NOUSED only with a scope: the texture pool binds only textures the bitmap ever reported, so a texture
           sampled by twins alone would never be bound (with TOTK_OHOS_DC_W=128 the scene still reports what it samples;
           textures only the probe samples stay unbound even then) */
        { const char *w = getenv("TOTK_OHOS_DC_W"); if (v == DC_FS_NOUSED && !(w && atoi(w) > 0)) { LOGV("TOTK_OHOS_DC_NOUSED needs TOTK_OHOS_DC_W: off"); v = 0; } }
        if (v) LOGV("draw cost experiment: fragment shaders %s (TOTK_OHOS_DC_%s=1); timing only, the picture is wrong",
                    v == DC_FS_NULL ? "of 8 KiB or more return at once" : v == DC_FS_VARY ? "of 8 KiB or more only add up their inputs" : "that sample the texture pool lose the usage bitmap's OpAtomicOr",
                    v == DC_FS_NULL ? "FSNULL" : v == DC_FS_VARY ? "FSVARY" : "NOUSED");
    }
    return v;
}
/* The module of a twin: a new word array (the caller frees the old one), or NULL when there is nothing to rewrite or the
   module is not understood (the twin then is the shader as it was).
   NOUSED, in place: OpAtomicOr type result pointer scope semantics value -> OpCopyObject type result value + 3 OpNop.
   NULL / VARY: the fragment entry function keeps its body under a new id, as a function that nobody calls, and the
   entry point's id goes to a new function appended to the module: for VARY one OpLoad of every vec4 float input with a
   Location, their sum, and its OpStore to every vec4 float output with a Location; then OpReturn. (A first draft left
   the old body behind an OpReturn as unreachable blocks: valid for spirv-val, but SPIRV-Cross refused it, and a driver
   that refuses a twin skips its draws, which would read as a gain.) */
static uint32_t *spirv_dc_fs(const uint32_t *w, size_t nw, int mode, size_t *out_nw) {
    if (nw < 5 || w[0] != 0x07230203u) return NULL;
    uint32_t bound = w[3]; if (!bound || bound > (1u << 22)) return NULL;
    if (mode == DC_FS_NOUSED) {
        uint32_t *o = malloc(nw * sizeof *o); if (!o) return NULL;
        memcpy(o, w, nw * sizeof *o); int k = 0;
        for (size_t i = 5; i < nw; ) {
            uint32_t op = o[i] & 0xffffu, n = o[i] >> 16; if (!n || n > nw - i) { free(o); return NULL; }
            if (op == 241 && n == 7) { uint32_t value = o[i + 6]; o[i] = 4u << 16 | 83; o[i + 3] = value; o[i + 4] = o[i + 5] = o[i + 6] = 1u << 16; k++; }
            i += n;
        }
        if (!k) { free(o); return NULL; }
        *out_nw = nw; return o;
    }
    uint32_t entry = 0, tfloat = 0, tvec4 = 0, in[32], out[8]; int nin = 0, nout = 0;
    uint32_t *m = calloc((size_t)bound * 3, sizeof *m);   /* [id] a pointer type: its storage class + 1; [bound + id] its pointee; [2 * bound + id] Location + 1 */
    if (!m) return NULL;
    size_t fn = 0;
    for (size_t i = 5; i < nw; ) {
        uint32_t op = w[i] & 0xffffu, n = w[i] >> 16; if (!n || n > nw - i) { free(m); return NULL; }
        const uint32_t *a = w + i + 1;
        if (op == 15 && n >= 3 && a[0] == 4 && !entry) entry = a[1];                                     /* OpEntryPoint Fragment */
        else if (op == 71 && n >= 4 && a[1] == 30 && a[0] < bound) m[2 * (size_t)bound + a[0]] = a[2] + 1;   /* OpDecorate Location */
        else if (op == 22 && n >= 3 && a[1] == 32 && !tfloat) tfloat = a[0];                           /* OpTypeFloat 32 */
        else if (op == 23 && n >= 4 && tfloat && a[1] == tfloat && a[2] == 4 && !tvec4) tvec4 = a[0];   /* OpTypeVector float 4 */
        else if (op == 32 && n >= 4 && a[0] < bound) { m[a[0]] = a[1] + 1; m[(size_t)bound + a[0]] = a[2]; }   /* OpTypePointer */
        else if (op == 59 && n >= 4 && a[0] < bound && a[1] < bound && m[2 * (size_t)bound + a[1]] && tvec4 && m[(size_t)bound + a[0]] == tvec4) {
            if (a[2] == 1 && nin < 32) in[nin++] = a[1];          /* OpVariable Input, vec4 of float, with a Location */
            else if (a[2] == 3 && nout < 8) out[nout++] = a[1];   /* OpVariable Output */
        } else if (op == 54 && n >= 5 && entry && a[1] == entry) { fn = i; break; }                    /* the entry point's OpFunction */
        i += n;
    }
    free(m);
    if (!entry || !fn) return NULL;
    if ((w[fn] >> 16) != 5) return NULL;
    if (mode != DC_FS_VARY || !nout) nin = 0;
    uint32_t *o = malloc((nw + 16 + (size_t)nin * 9 + (size_t)nout * 3) * sizeof *o); if (!o) return NULL;
    memcpy(o, w, nw * sizeof *o);
    size_t k = nw;
    o[fn + 2] = bound++;                                                             /* the old entry function: now one that nobody calls */
    o[k++] = 5u << 16 | 54; o[k++] = w[fn + 1]; o[k++] = entry; o[k++] = 0; o[k++] = w[fn + 4];   /* OpFunction <void> entry None <its type> */
    o[k++] = 2u << 16 | 248; o[k++] = bound++;                                       /* OpLabel */
    uint32_t acc = 0;
    for (int v = 0; v < nin; v++) {
        uint32_t l = bound++;
        o[k++] = 4u << 16 | 61; o[k++] = tvec4; o[k++] = l; o[k++] = in[v];           /* OpLoad */
        if (acc) { uint32_t s = bound++; o[k++] = 5u << 16 | 129; o[k++] = tvec4; o[k++] = s; o[k++] = acc; o[k++] = l; acc = s; }   /* OpFAdd */
        else acc = l;
    }
    if (acc) for (int v = 0; v < nout; v++) { o[k++] = 3u << 16 | 62; o[k++] = out[v]; o[k++] = acc; }   /* OpStore */
    o[k++] = 1u << 16 | 253;                                                         /* OpReturn */
    o[k++] = 1u << 16 | 56;                                                          /* OpFunctionEnd */
    o[3] = bound; *out_nw = k;
    return o;
}
#endif
static vk_shader *shader_load_k(const char *variant, int early_tests, uint64_t stencil, int tf, const uint32_t *keep);
#ifdef TOTK_OHOS
/* ---------- oot-a108: vertex outputs the fragment shader does not read (TOTK_OHOS_PRUNE_VARYINGS) ----------
   A mask holds bit 4 * Location + Component, Locations 0 - 31. */
static void vary_bits(uint32_t m[4], unsigned first, unsigned n) { for (unsigned b = first; b < first + n && b < 128; b++) m[b >> 5] |= 1u << (b & 31); }
static unsigned vary_count(const uint32_t m[4]) { return (unsigned)(__builtin_popcount(m[0]) + __builtin_popcount(m[1]) + __builtin_popcount(m[2]) + __builtin_popcount(m[3])); }
/* Whether operand k (0: the word after the opcode word) of a function-body instruction can be an id. Known literal
   positions are left out; a literal that equals an interface id only makes the masks keep more. */
static int vary_is_id(uint32_t op, uint32_t k) {
    switch (op) {
    case 12: return k == 2 || k >= 4;                        /* OpExtInst: (instruction) */
    case 79: case 82: return k <= 3;                         /* OpVectorShuffle, OpCompositeInsert: (indexes) */
    case 81: return k <= 2;                                  /* OpCompositeExtract */
    case 61: return k <= 2;                                  /* OpLoad: (memory access) */
    case 62: return k <= 1;                                  /* OpStore */
    case 246: return k <= 1;                                 /* OpLoopMerge */
    case 247: return k == 0;                                 /* OpSelectionMerge */
    case 250: return k <= 2;                                 /* OpBranchConditional: (weights) */
    case 251: return k <= 1 || (k >= 3 && (k & 1));         /* OpSwitch: (literal) label pairs */
    case 87: case 88: case 91: case 92: case 95: case 98: return k != 4;   /* image instructions: (operands mask) */
    case 89: case 90: case 93: case 94: case 96: case 97: return k != 5;
    case 99: return k != 3;                                  /* OpImageWrite */
    case 342: return k != 3;                                 /* OpGroupNonUniformBallotBitCount: (group operation) */
    default: return op >= 349 && op <= 359 ? k != 3 : 1;     /* OpGroupNonUniform arithmetic */
    }
}
/* mode 0: the Location input components a fragment module loads -> mask. mode 1: the Location output components a vertex
   module stores -> mask, and the components of outputs it uses otherwise (loads, passes on, copies) -> keepall. mode 2:
   *out_w gets a copy of the module without the stores whose components all lie outside keep (NULL when there is none;
   the caller frees it), *removed their number, mask the components the copy still stores. A load or store through an
   OpAccessChain with one constant index
   into a vector reaches that component; any other use of an interface variable or of a chain into one reaches all of
   the variable (an array, matrix or struct: every component from its Location up). Returns 0 for a module it does not
   understand (malformed, a Location of 32 or more, a Location on a block member, decoration groups): then nothing may be
   pruned. */
static int spirv_vary(const uint32_t *w, size_t nw, int mode, const uint32_t keep[4], uint32_t mask[4], uint32_t keepall[4],
                      uint32_t **out_w, size_t *out_nw, unsigned *removed) {
    memset(mask, 0, 4 * sizeof *mask);
    if (keepall) memset(keepall, 0, 4 * sizeof *keepall);
    if (out_w) *out_w = NULL;
    if (removed) *removed = 0;
    if (nw < 5 || w[0] != 0x07230203u || (mode && !keepall) || (mode == 2 && (!keep || !out_w || !out_nw))) return 0;
    uint32_t bound = w[3]; if (!bound || bound > (1u << 22)) return 0;
    int32_t *loc = malloc(bound * sizeof *loc);
    uint32_t *pointee = calloc(bound, sizeof *pointee), *vty = calloc(bound, sizeof *vty), *cval = calloc(bound, sizeof *cval), *cvar = calloc(bound, sizeof *cvar);
    uint8_t *comp = calloc(bound, 1), *vn = calloc(bound, 1), *w32 = calloc(bound, 1), *isc = calloc(bound, 1), *iv = calloc(bound, 1);
    int8_t *cc = calloc(bound, 1);
    uint8_t *drop = mode == 2 ? calloc(nw, 1) : NULL;
    int ok = loc && pointee && vty && cval && cvar && comp && vn && w32 && isc && iv && cc && (mode != 2 || drop);
    size_t fn = 0; unsigned nrem = 0;
    const uint32_t sc = mode ? 3u : 1u;   /* Output : Input */
    if (ok) for (uint32_t i = 0; i < bound; i++) loc[i] = -1;
    for (size_t i = 5; ok && i < nw; ) {   /* the declarations, up to the first function */
        uint32_t op = w[i] & 0xffffu, n = w[i] >> 16;
        if (!n || n > nw - i) { ok = 0; break; }
        const uint32_t *a = w + i + 1;
        if (op == 54) { fn = i; break; }
        if (op == 71 && n >= 4 && a[0] < bound) { if (a[1] == 30) loc[a[0]] = (int32_t)a[2]; else if (a[1] == 31) comp[a[0]] = (uint8_t)(a[2] < 4 ? a[2] : 4); }
        else if ((op == 21 || op == 22) && n >= 3 && a[0] < bound) { vn[a[0]] = 1; w32[a[0]] = a[1] == 32; }
        else if (op == 23 && n >= 4 && a[0] < bound && a[1] < bound) vn[a[0]] = (uint8_t)(w32[a[1]] && a[2] >= 2 && a[2] <= 4 ? a[2] : 0);
        else if (op == 32 && n >= 4 && a[0] < bound) pointee[a[0]] = a[2];
        else if (op == 43 && n == 4 && a[1] < bound) { cval[a[1]] = a[2]; isc[a[1]] = 1; }
        else if (op == 59 && n >= 4 && a[0] < bound && a[1] < bound && a[2] == sc) { iv[a[1]] = 1; vty[a[1]] = pointee[a[0]] < bound ? pointee[a[0]] : 0; }
        else if ((op == 72 && n >= 5 && a[2] == 30) || (op >= 73 && op <= 75)) ok = 0;   /* (checker) a Location on a block member, decoration groups: such a variable has no Location of its own and would be skipped; none in the seed */
        i += n;
    }
    if (ok && !fn) ok = 0;
    for (uint32_t v = 0; ok && v < bound; v++) if (iv[v]) { if (loc[v] < 0) iv[v] = 0; else if (loc[v] >= 32) ok = 0; }
    /* the components of variable v: one (c >= 0) or all */
#define VARY_SHAPE(v, c, m) do { unsigned l_ = 4u * (unsigned)loc[v], n_ = vn[vty[v]]; \
        if (n_ && comp[v] + n_ <= 4) vary_bits((m), l_ + comp[v] + ((c) >= 0 ? (unsigned)(c) : 0u), (c) >= 0 ? 1u : n_); \
        else vary_bits((m), l_, 128u - l_); } while (0)
    for (size_t i = fn; ok && i < nw; ) {   /* the function bodies */
        uint32_t op = w[i] & 0xffffu, n = w[i] >> 16;
        if (!n || n > nw - i) { ok = 0; break; }
        const uint32_t *a = w + i + 1;
        if ((op == 65 || op == 66) && n >= 4 && a[1] < bound && a[2] < bound && iv[a[2]]) {   /* OpAccessChain into an interface variable */
            uint32_t nv = vn[vty[a[2]]];
            cvar[a[1]] = a[2];
            cc[a[1]] = (int8_t)(n == 5 && a[3] < bound && isc[a[3]] && nv >= 2 && cval[a[3]] < nv ? (int)cval[a[3]] : -1);
            i += n; continue;
        }
        uint32_t p = op == 61 && n >= 4 ? a[2] : op == 62 && n >= 3 ? a[0] : 0;   /* the pointer of an OpLoad / OpStore */
        if (p && p < bound && (iv[p] || cvar[p]) && (op == 61) == (mode == 0)) {
            uint32_t v = iv[p] ? p : cvar[p]; int c = iv[p] ? -1 : cc[p];
            uint32_t m[4] = {0, 0, 0, 0};
            VARY_SHAPE(v, c, m);
            if (mode == 2 && !((m[0] & keep[0]) | (m[1] & keep[1]) | (m[2] & keep[2]) | (m[3] & keep[3]))) { drop[i] = 1; nrem++; }
            else for (int k = 0; k < 4; k++) mask[k] |= m[k];
            if (op == 62 && a[1] < bound && (iv[a[1]] || cvar[a[1]])) { uint32_t u = iv[a[1]] ? a[1] : cvar[a[1]]; VARY_SHAPE(u, -1, keepall); }
            i += n; continue;
        }
        for (uint32_t k = 0; k + 1 < n; k++) if (vary_is_id(op, k) && a[k] < bound && (iv[a[k]] || cvar[a[k]])) {
            uint32_t v = iv[a[k]] ? a[k] : cvar[a[k]];
            if (mode) VARY_SHAPE(v, -1, keepall); else VARY_SHAPE(v, -1, mask);
        }
        i += n;
    }
#undef VARY_SHAPE
    if (ok && mode == 2 && nrem) {
        uint32_t *o = malloc(nw * sizeof *o);
        if (!o) ok = 0;
        else {
            size_t k = 5; memcpy(o, w, 5 * sizeof *o);
            for (size_t i = 5; i < nw; ) { uint32_t n = w[i] >> 16; if (!drop[i]) { memcpy(o + k, w + i, n * sizeof *o); k += n; } i += n; }
            *out_w = o; *out_nw = k; if (removed) *removed = nrem;
        }
    }
    free(loc); free(pointee); free(vty); free(cval); free(cvar); free(comp); free(vn); free(w32); free(isc); free(iv); free(cc); free(drop);
    return ok;
}
static int vary_on(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("TOTK_OHOS_PRUNE_VARYINGS"); v = e && *e == '1';
        if (v) LOGV("varyings: vertex shaders get a variant per fragment read mask, without the output stores it does not load (TOTK_OHOS_PRUNE_VARYINGS=1)"); }
    return v;
}
/* recording thread (vk_shader_prune, shader_load_k for a pruned key, vk_draw.c pipe_get) */
static uint64_t g_vary_variants, g_vary_variant_cut, g_vary_pairs, g_vary_pairs_cut, g_vary_pipes, g_vary_draws, g_vary_draws_cut, g_vary_draw_cut, g_vary_draw_written;
void vk_shader_prune_pipe(void) { g_vary_pipes++; }
/* The vertex shader to draw with fragment shader fs: with TOTK_OHOS_PRUNE_VARYINGS=1 the variant of vs without the stores
   of output components fs never loads, keyed by what it keeps (fragment shaders that load the same components share it);
   otherwise vs. Recording thread, after vk_shader_depth_tests / vk_shader_dc: the twin that is drawn is the one whose loads
   count. A pair is decided once; shader objects never move or die. */
vk_shader *vk_shader_prune(vk_shader *vs, vk_shader *fs) {
    if (!vary_on() || !vs || !fs || vs->stage != 0 || fs->stage != 1 || vs->tf || vs->vary_pruned) return vs;
    static vk_shader *last_vs, *last_fs, *last;
    static nvn_hmap pairs = {.klen = 2 * sizeof(void *)};
    vk_shader *v;
    if (vs == last_vs && fs == last_fs) v = last;
    else {
        void *k[2] = {vs, fs};
        v = nvn_hm_get(&pairs, k);
        if (!v) {
            v = vs; g_vary_pairs++;
            if (vs->vary_ok && fs->vary_ok && !vs->gs) {
                uint32_t keep[4]; int cut = 0;
                for (int i = 0; i < 4; i++) { keep[i] = vs->vary_written[i] & (fs->vary_read[i] | vs->vary_keepall[i]); cut |= keep[i] != vs->vary_written[i]; }
                if (cut) {
                    vk_shader *p = shader_load_k(vs->key, (int)(vs->early_tests | vs->alpha_func << 1), vs->stencil_mask, 0, keep);
                    if (p && p->vary_pruned) { v = p; g_vary_pairs_cut++; }
                    else if (!p) RED(8, "varyings: vertex shader %.16s with fragment shader %.16s: pruned variant refused; drawn as it is", vs->key, fs->key);
                }
            }
            nvn_hm_put(&pairs, k, v);
        }
        last_vs = vs; last_fs = fs; last = v;
    }
    g_vary_draws++; g_vary_draw_written += vs->vary_ncomp;
    if (v != vs) { g_vary_draws_cut++; g_vary_draw_cut += v->vary_removed; }
    if (!(g_vary_draws & ((1u << 19) - 1))) {
        const double d = (double)(1u << 19);
        LOGV("varyings: of the last %u draws %llu drew with a pruned vertex shader; %.2f of %.2f output components a vertex removed (all of them counted); so far %llu variants (%.1f components removed each) for %llu of %llu vertex / fragment pairs, %llu pipelines made with one (TOTK_OHOS_PRUNE_VARYINGS)",
             1u << 19, (unsigned long long)g_vary_draws_cut, (double)g_vary_draw_cut / d, (double)g_vary_draw_written / d,
             (unsigned long long)g_vary_variants, g_vary_variants ? (double)g_vary_variant_cut / (double)g_vary_variants : 0.0,
             (unsigned long long)g_vary_pairs_cut, (unsigned long long)g_vary_pairs, (unsigned long long)g_vary_pipes);
        g_vary_draws_cut = g_vary_draw_cut = g_vary_draw_written = 0;
    }
    return v;
}
#endif
static vk_shader *shader_load_x(const char *variant, int early_tests, uint64_t stencil, int tf) { return shader_load_k(variant, early_tests, stencil, tf, NULL); }
static vk_shader *shader_load_m(const char *variant, int early_tests, uint64_t stencil) { return shader_load_x(variant, early_tests, stencil, 0); }
static vk_shader *shader_load(const char *variant, int early_tests) { return shader_load_x(variant, early_tests, 0, 0); }
static vk_shader *shader_load_k(const char *variant, int early_tests, uint64_t stencil, int tf, const uint32_t *keep) {
    pthread_once(&g_position_once, position_init);
    shader_key key = {0}; snprintf(key.variant, sizeof key.variant, "%s", variant);
    key.early_tests = (uint32_t)early_tests; key.position_policy = g_position_policy; key.stencil = stencil; key.tf = (uint32_t)tf;
    if (keep) { key.pruned = 1; memcpy(key.keep, keep, sizeof key.keep); }   /* oot-a108: vk_shader_prune */
    vk_shader *sh = nvn_hm_get(&g_shaders, &key);
    if (sh) return sh;
    char path[1024]; size_t spv_len = 0, vk_len = 0;
    snprintf(path, sizeof path, "%s/%s.spv", seed_dir(), variant); char *spv = read_file(path, &spv_len);
    snprintf(path, sizeof path, "%s/%s.vk", seed_dir(), variant); char *txt = read_file(path, &vk_len);
    if (!spv || !txt || spv_len % 4 || spv_len < 20) { RED(16, "shader %s: SPIR-V or reflection missing in the seed", variant); free(spv); free(txt); return NULL; }
    if (!spirv_image_features_ok((const uint32_t *)spv, spv_len / 4, V.has_image_read_without_format, V.has_image_write_without_format)) {
        RED(16, "shader %s: malformed SPIR-V or unsupported formatless storage image capability", variant);
        free(spv); free(txt); return NULL;
    }
    sh = calloc(1, sizeof *sh); memcpy(sh->key, key.variant, sizeof sh->key); sh->early_tests = key.early_tests & 1; sh->alpha_func = key.early_tests >> 1 & 7; sh->early_flip = key.early_tests >> 4 & 1; sh->stencil_mask = stencil;
    sh->spv_bytes = spv_len;
    sh->local[0] = sh->local[1] = sh->local[2] = 1;
    int capb = 0;
    for (char *save = NULL, *line = strtok_r(txt, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        if (!strncmp(line, "stage ", 6)) sh->stage = atoi(line + 6);
        else if (!strncmp(line, "local ", 6)) sscanf(line + 6, "%u %u %u", &sh->local[0], &sh->local[1], &sh->local[2]);
        else if (!strncmp(line, "depth ", 6)) sh->writes_depth = atoi(line + 6);
        else if (!strncmp(line, "c1 ", 3)) {
            const char *h = line + 3; size_t n = strlen(h) / 2;
            sh->c1 = calloc(1, n ? n : 1); sh->c1_len = (uint32_t)n;
            for (size_t i = 0; i < n; i++) { int a = hexval(h[2 * i]), b = hexval(h[2 * i + 1]); sh->c1[i] = (uint8_t)(a < 0 || b < 0 ? 0 : a << 4 | b); }
        } else if (!strncmp(line, "vin", 3)) {
            for (char *p = line + 3; *p; ) { while (*p == ' ') p++; if (!*p) break; int loc, cls; if (sscanf(p, "%d:%d", &loc, &cls) == 2 && loc >= 0 && loc < 32) { sh->vin_mask |= 1u << loc; sh->vin_class[loc] = (uint8_t)cls; } while (*p && *p != ' ') p++; }
        } else if (!strncmp(line, "fout", 4)) {
            for (char *p = line + 4; *p; ) { while (*p == ' ') p++; if (!*p) break; int i = atoi(p); if (i >= 0 && i < 8) sh->fout_mask |= 1u << i; while (*p && *p != ' ') p++; }
        } else if (!strncmp(line, "mesh ", 5)) {   /* a wrapped NVN2 task / mesh kernel (tools/mesh_wrap_in_container.py mesh_line) */
            unsigned stg = 0, task = 0, mv = 0, mp = 0, nc = 0, pay = 0, verts = 0;
            if (sscanf(line + 5, "%u %u %u %u %u %u %u", &stg, &task, &mv, &mp, &nc, &pay, &verts) == 7) {
                sh->mesh_stage = (uint8_t)stg; sh->mesh_task = (uint8_t)task; sh->mesh_verts = (uint8_t)verts;
                sh->mesh_max_v = (uint16_t)mv; sh->mesh_max_p = (uint16_t)mp; sh->mesh_ncomp = (uint16_t)nc; sh->mesh_pay = (uint16_t)pay;
            }
        } else if (line[0] == 'b' && line[1] == ' ') {
            int bnd, cnt, dt, kind, reg, sreg, off, rw, tgt, isint; uint64_t extent = 0;   /* old ten-field seeds */
            if (sscanf(line + 2, "%d %d %d %d %d %d %d %d %d %d %" SCNu64, &bnd, &cnt, &dt, &kind, &reg, &sreg, &off, &rw, &tgt, &isint, &extent) < 10) continue;
            if (sh->nb == capb) { capb = capb ? 2 * capb : 16; sh->b = realloc(sh->b, (size_t)capb * sizeof *sh->b); }
            sh->b[sh->nb++] = (vk_bind){(uint16_t)bnd, (uint16_t)(cnt > 0 ? cnt : 1), (uint8_t)kind, (uint8_t)dt, (uint8_t)tgt, (uint8_t)rw, (uint8_t)isint, (int16_t)reg, (int16_t)sreg, (uint16_t)off, extent};
        }
    }
    free(txt);
    {   /* bindless pool arrays live in their own set (vk_res.c), not in the stage's */
        int k = 0;
        for (int i = 0; i < sh->nb; i++) { if (sh->b[i].kind == RB_POOLTEX || sh->b[i].kind == RB_POOLSAMP || sh->b[i].kind == RB_POOLUSED) sh->bindless = 1; else sh->b[k++] = sh->b[i]; }
        sh->nb = k;
    }
    if (!V.has_bda) for (int i = 0; i < sh->nb; i++) if (sh->b[i].kind == RB_GMEM || sh->b[i].kind == RB_GMEML) {
        /* physical storage buffer pointers (Switch 2's memory pool table) need bufferDeviceAddress */
        RED(8, "shader %s reaches memory through loaded addresses (memory pool table); the device has no bufferDeviceAddress: its draws are skipped", variant);
        free(spv); free(sh->b); free(sh->c1); free(sh); return NULL;
    }
    { vk_load_certificate proof = vk_load_certify((const uint32_t *)spv, spv_len / 4, sh->stage == 1);
      sh->load_position_attr = proof.position_attr; sh->load_full_outputs = proof.full_outputs; }
    { static uint64_t nmark; int m = spirv_mark_stores(sh, (const uint32_t *)spv, spv_len / 4);
      if (m && ++nmark <= 8) LOGV("shader %s writes %d storage binding(s) (from its SPIR-V)", variant, m); }
    if (sh->stage == 1) {   /* kept for a live TOTK_VK_UNWRITTEN_MASK; the launch switch also extends fout_mask, as before */
        sh->fout_span = spirv_fout_locations((const uint32_t *)spv, spv_len / 4);
        if (vk_unwritten_mask_on()) sh->fout_mask |= sh->fout_span;
    }
    for (int i = 0; i < sh->nb; i++) if (sh->b[i].rw &&
        (sh->b[i].dtype == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER || sh->b[i].dtype == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC ||
         sh->b[i].dtype == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER)) { sh->has_storage_writes = 1; break; }
    if (sh->stage == 1 && vk_native_ui_enabled()) sh->fragcoord_xy = spirv_fragcoord_xy((const uint32_t *)spv, spv_len / 4);
    sh->canvas_ui = vk_canvas_ui_shader(sh->key,0) ? 2 : vk_canvas_ui_shader(sh->key,1);
    sh->canvas_transfer = vk_canvas_transfer_shader(sh->key);
    if (sh->stage == 0) sh->framegen_camera_layout = (uint8_t)vk_fg_camera_layout(sh->key);
    if (sh->stage == 1) sh->framegen_raster_depth = (uint8_t)vk_fg_raster_depth_shader(sh->key);
    sh->framegen_plane=(uint8_t)vk_fg_plane_shader(sh->key,(unsigned)sh->stage);
    if (sh->stage==0) sh->framegen_uncertified_plane=(uint8_t)vk_fg_plane_shader(sh->key,2);
    sh->native_1x = sh->fragcoord_xy || sh->has_storage_writes;
    for (int i = 0; i < sh->nb; i++) {
        const vk_bind *b = &sh->b[i];
        if (b->dtype == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER && !b->rw && b->reg >= 0 && b->reg < 16)
            sh->has_storage_reads = 1;
        if (b->dtype == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER && b->reg >= 0 && b->reg < 16 &&
            (b->kind == RB_CBUF || b->kind == RB_UBO_PTR)) sh->uniform_prepare_mask |= 1u << b->reg;
        if (b->dtype == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) sh->native_1x = 1;
        if (b->dtype != VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE && b->dtype != VK_DESCRIPTOR_TYPE_STORAGE_IMAGE &&
            b->dtype != VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) continue;
        if (b->kind == RB_IMG) { if (b->reg >= 0 && b->reg < 32) sh->native_img_mask |= 1u << b->reg; }
        else if (b->reg >= 0 && b->reg < 128) sh->native_tex_mask[b->reg/64] |= 1ull << (b->reg&63);
    }
    if (sh->stage == 0 && g_position_policy) {
        size_t nw = spv_len / 4; uint32_t *words = (uint32_t *)spv;
        if (vk_spirv_position(&words, &nw, g_position_policy) < 0) {
            RED(16, "shader %s: cannot preserve requested vertex position precision; refused", variant);
            free(spv); free(sh->b); free(sh->c1); free(sh); return NULL;
        }
        spv = (char *)words; spv_len = nw * 4;
    }
    if (sh->stage == 1) {
        size_t nw = spv_len / 4; uint32_t *words = (uint32_t *)spv;
#ifdef TOTK_OHOS
        {   /* oot-a106 (timing only): a draw-cost twin; vk_shader_dc put its mode in bits 5 and 6 of the key */
            int dc = (int)(key.early_tests >> 5 & 3);
            /* NOUSED takes away the port's own side effect only (as earlyz_auto's "ours") */
            if (dc == DC_FS_NOUSED && (!sh->bindless || sh->has_storage_writes || (spirv_frag_traits(words, nw) & FT_OWN))) dc = 0;
            if (dc && !tf) {
                static _Atomic uint64_t made, kept;
                size_t dn = 0; uint32_t *dwords = spirv_dc_fs(words, nw, dc, &dn);
                if (dwords) { free(words); words = dwords; nw = dn; spv = (char *)words; spv_len = nw * 4; }
                uint64_t n = atomic_fetch_add(dwords ? &made : &kept, 1) + 1;
                if (n <= 3 || !(n & (n - 1)))
                    LOGV("draw cost experiment: fragment twin of %.16s %s (mode %d); so far %llu rewritten, %llu left as they were", variant, dwords ? "rewritten" : "left as it was", dc,
                         (unsigned long long)atomic_load(&made), (unsigned long long)atomic_load(&kept));
            }
        }
#endif
        int want_early = sh->early_tests && earlyz_on() && !sh->alpha_func;
#ifdef TOTK_OHOS
        sh->frag_traits = spirv_frag_traits(words, nw) | (sh->alpha_func ? FT_KILL : 0);   /* the alpha-test variant gets its discard below */
        if (!want_early && !tf && earlyz_auto()) {
            int t = sh->frag_traits, mode = earlyz_auto();
            /* the side effect has to be the port's alone: the pool's usage bitmap (a shader with atomics, storage or image
               writes of its own keeps its late tests: it may count on writing for hidden fragments) */
            int ours = (t & FT_ATOMIC) && sh->bindless && !(t & FT_OWN) && !sh->has_storage_writes;
            int able = !(t & FT_DEPTH) && (ours || (mode >= 3 && !(t & (FT_ATOMIC | FT_OWN)) && !sh->has_storage_writes)), kill = !!(t & FT_KILL);
            if (!sh->early_flip) { sh->auto_early = able && !kill; sh->early_candidate = able && kill && mode >= 2 && !sh->alpha_func; want_early = sh->auto_early; }
            else want_early = able && kill;   /* the twin: of a shader that can discard, with the mode; of one that has it, without */
            int slot = sh->early_flip ? (want_early ? 1 : 2) : want_early ? 0 : (t & FT_DEPTH) ? 3 : 4;
            uint64_t n = atomic_fetch_add(&g_ez_n[slot], 1) + 1;
            if (slot == 4 && able && kill) atomic_fetch_add(&g_ez_n[5], 1);
            if (slot < 3 && (n <= 3 || !(n & (n - 1))))
                LOGV("early depth tests: fragment shader %.16s %s (discard %d, pool usage write %d, storage writes of its own %d); so far %llu with the mode, %llu twins with it for draws that write no depth, %llu twins without it, %llu that write depth, %llu others (%llu of them write the bitmap and can discard) (TOTK_OHOS_EARLYZ_AUTO=%d)",
                     variant, slot == 0 ? "gets the mode" : slot == 1 ? "twin with the mode" : "twin without the mode", kill, !!(t & FT_ATOMIC), sh->has_storage_writes,
                     (unsigned long long)atomic_load(&g_ez_n[0]), (unsigned long long)atomic_load(&g_ez_n[1]), (unsigned long long)atomic_load(&g_ez_n[2]),
                     (unsigned long long)atomic_load(&g_ez_n[3]), (unsigned long long)atomic_load(&g_ez_n[4]), (unsigned long long)atomic_load(&g_ez_n[5]), mode);
        }
#endif
        if (vk_spirv_early_tests(&words, &nw, want_early) < 0) {
            RED(16, "shader %s: invalid fragment execution modes", variant); free(spv); free(sh->b); free(sh->c1); free(sh); return NULL;
        }
        spv = (char *)words; spv_len = nw * 4;
        size_t nnw; uint32_t *pw = spirv_depth_nudge((const uint32_t *)spv, spv_len / 4, &nnw);
        if (pw) { free(spv); spv = (char *)pw; spv_len = nnw * 4; sh->depth_spec = 1; }
        patch_set((uint32_t *)spv, spv_len / 4, 1);
        if (sh->alpha_func) {
            unsigned binding=0;
            for (int i=0;i<sh->nb;i++) if (sh->b[i].binding>=binding) binding=sh->b[i].binding+1;
            pw=vk_spirv_alpha_test((const uint32_t *)spv,spv_len/4,sh->alpha_func,binding,&nnw);
            if (!pw || binding>UINT16_MAX) {
                RED(16,"shader %s: cannot build alpha-test comparison %u",variant,sh->alpha_func);
                free(pw); free(spv); free(sh->b); free(sh->c1); free(sh); return NULL;
            }
            free(spv); spv=(char *)pw; spv_len=nnw*4;
            sh->b=realloc(sh->b,(size_t)(sh->nb+1)*sizeof *sh->b);
            sh->b[sh->nb++]=(vk_bind){.binding=binding,.count=1,.kind=RB_ALPHA,
                .dtype=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,.reg=-1,.sreg=-1,.extent=16};
            sh->load_full_outputs=0;
        }
    }
    if (V.subgroup_fix) {
        size_t nw = spv_len / 4; uint32_t *words = (uint32_t *)spv;
        int fixed = vk_spirv_subgroup32(&words, &nw);
        spv = (char *)words; spv_len = nw * 4;
        if (fixed > 0) {
            sh->required_subgroup_size = vk_subgroup_required(stage_flags(sh->stage), V.subgroup_control,
                V.subgroup_sizes.requiredSubgroupSizeStages, V.subgroup_sizes.minSubgroupSize, V.subgroup_sizes.maxSubgroupSize);
            uint32_t physical = sh->required_subgroup_size ? sh->required_subgroup_size : V.subgroup.subgroupSize;
            if (physical < 32 || physical > 128 || (physical & (physical - 1)) ||
                !(V.subgroup.supportedStages & stage_flags(sh->stage)) ||
                !(V.subgroup.supportedOperations & VK_SUBGROUP_FEATURE_BALLOT_BIT)) fixed = -1;
            else LOGV("subgroup shader %s: logical 32, physical %u%s", variant, physical,
                sh->required_subgroup_size ? " (pinned)" : " (fixed default)");
        }
        if (fixed < 0) {
            RED(32, "subgroup shader %s: cannot preserve 32-lane semantics on this device/module; refused", variant);
            free(spv); free(sh->b); free(sh->c1); free(sh); return NULL;
        }
    }
#ifdef TOTK_OHOS
    {   size_t nw = spv_len / 4; uint32_t *w = (uint32_t *)spv;
        int fixed = spirv_barrier_safe_returns(&w, &nw);
        spv = (char *)w; spv_len = nw * 4;
        if (fixed > 0) { static _Atomic int said; if (atomic_fetch_add(&said, 1) < 32) LOGV("compute shader %s: %s (Maleoon)", variant,
            fixed == 2 ? "workgroup barrier moved in front of the early return" : "early returns given the barriers that follow them"); }
        if (fixed < 0) RED(16, "compute shader %s: barriers inside branches or loops after an early return; left as is (may hang this GPU)", variant);
    }
#endif
#ifdef TOTK_OHOS
    {   /* Maleoon (no shaderFloat64) rejects OpCapability Float64 ("SPV01: Unsupported capability (10)"); the translator
           declares it in every module, also where no 64-bit float type exists. Drop it there; a module that really
           uses doubles keeps it and fails as before, visibly. */
        size_t nw = spv_len / 4; uint32_t *w = (uint32_t *)spv;
        if (nw >= 5 && w[0] == 0x07230203u) {
            size_t i = 5, cap = 0; int uses = 0, ok = 1;
            while (i < nw) {
                uint32_t op = w[i] & 0xffffu, len = w[i] >> 16;
                if (!len || i + len > nw) { ok = 0; break; }
                if (op == 17 && len == 2 && w[i + 1] == 10) cap = i;          /* OpCapability Float64 */
                if (op == 22 && len >= 3 && w[i + 2] == 64) uses = 1;        /* OpTypeFloat 64 */
                i += len;
            }
            if (ok && cap && !uses) { memmove(w + cap, w + cap + 2, (nw - cap - 2) * 4); spv_len -= 8; }
            else if (ok && cap && uses) RED(16, "shader %s really uses 64-bit floats, which this GPU lacks", variant);
        }
    }
#endif
#ifdef TOTK_OHOS
    {   /* the pool usage bitmap: an OpAtomicOr a sample becomes "read the word, OR only a bit that is not set" */
        /* Off unless TOTK_OHOS_USAGE_COND=1 (oot-a103): the translator (prepare_xlate_ns2.py MarkBindlessUsed) already tests
           the bit before every OpAtomicOr of the bitmap, so this rewrite only nested a second identical test in every
           bindless module (the seed: 1195 of 1195 sampled OpAtomicOr guarded). Kept for modules of an older translator. */
        static int on = -1; if (on < 0) { const char *e = getenv("TOTK_OHOS_USAGE_COND"); on = e && *e == '1'; }
        if (on && sh->bindless && !tf) {
            size_t nw = spv_len / 4; uint32_t *w = (uint32_t *)spv;
            int k = vk_spirv_usage_cond(&w, &nw);
            if (k > 0) {
                spv = (char *)w; spv_len = nw * 4;
                static _Atomic uint64_t mods, insts; uint64_t m = atomic_fetch_add(&mods, 1) + 1, t = atomic_fetch_add(&insts, (uint64_t)k) + (uint64_t)k;
                if (m <= 2 || !(m & (m - 1))) LOGV("pool usage bitmap: %llu modules write it only for bits not set yet (%llu instructions rewritten; this one %s stage %d: %d) (TOTK_OHOS_USAGE_COND)",
                                                   (unsigned long long)m, (unsigned long long)t, variant, sh->stage, k);
            } else if (k < 0) RED(8, "pool usage bitmap: %s could not be rewritten (left as it is)", variant);
        }
    }
#endif
#ifdef TOTK_OHOS
    uint32_t *gs_words = NULL; size_t gs_n = 0;
    if (!V.has_layer_output) (void)layer_patch(&spv, &spv_len, sh->stage, V.has_geometry ? &gs_words : NULL, &gs_n, variant);
    if (stencil && !stencil_patch(&spv, &spv_len, stencil, variant)) RED(8, "stencil view: %s could not be patched (mask %#llx)", variant, (unsigned long long)stencil);
    if (tf) {   /* the capture variant: flat outputs that its fragment shader (tf_fs) stores into the feedback buffers (vk_tf.inc) */
        uint32_t first_loc = 0;
        if (!tf_vs_patch(&spv, &spv_len, &g_tf_pending, &first_loc, variant)) { RED(8, "transform feedback: %s could not be patched", variant); free(spv); free(sh->b); free(sh->c1); free(sh); return NULL; }
        sh->tf_fs = tf_fs_make(&g_tf_pending, first_loc, variant);
        if (!sh->tf_fs) { free(spv); free(sh->b); free(sh->c1); free(sh); return NULL; }
        sh->tf = 1;
    }
#else
    (void)stencil;
#endif
#ifdef TOTK_OHOS
    if ((sh->stage == 0 || sh->stage == 1) && !tf && vary_on()) {   /* oot-a108: the masks of vk_shader_prune; a pruned variant loses its stores here */
        size_t nw = spv_len / 4, pnw = 0; uint32_t *pw = NULL; unsigned nrem = 0;
        if (sh->stage == 1) sh->vary_ok = (uint8_t)spirv_vary((const uint32_t *)spv, nw, 0, NULL, sh->vary_read, NULL, NULL, NULL, NULL);
        else {
            sh->vary_ok = (uint8_t)spirv_vary((const uint32_t *)spv, nw, 1, NULL, sh->vary_written, sh->vary_keepall, NULL, NULL, NULL);
            sh->vary_ncomp = (uint16_t)vary_count(sh->vary_written);
            uint32_t still[4], unused[4];
            if (key.pruned && sh->vary_ok && spirv_vary((const uint32_t *)spv, nw, 2, key.keep, still, unused, &pw, &pnw, &nrem)) {
                uint32_t cut[4]; for (int i = 0; i < 4; i++) cut[i] = sh->vary_written[i] & ~still[i];
                sh->vary_removed = (uint16_t)vary_count(cut); sh->vary_pruned = sh->vary_removed > 0;   /* whole-vec4 stores (mesh pass-through) go only with all of theirs */
                if (pw) { free(spv); spv = (char *)pw; spv_len = pnw * 4; pw = NULL; }
                uint64_t n = ++g_vary_variants; g_vary_variant_cut += sh->vary_removed;
                if (n <= 4 || !(n & (n - 1)))
                    LOGV("varyings: vertex shader %.16s keeps %u of %u output components (%u stores removed); so far %llu variants, %.1f components removed each (TOTK_OHOS_PRUNE_VARYINGS)",
                         variant, (unsigned)(sh->vary_ncomp - sh->vary_removed), (unsigned)sh->vary_ncomp, nrem, (unsigned long long)n, (double)g_vary_variant_cut / (double)n);
            }
            free(pw);
        }
    }
#endif
    VkShaderModuleCreateInfo mci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = spv_len, .pCode = (const uint32_t *)spv};
    VkResult r = vkCreateShaderModule(V.dev, &mci, NULL, &sh->mod);
    free(spv);
    if (r) { RED(16, "vkCreateShaderModule %s: %d", variant, r); free(sh->b); free(sh->c1); free(sh); return NULL; }
#ifdef TOTK_OHOS
    if (gs_words) {
        VkShaderModuleCreateInfo gci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = gs_n * 4, .pCode = gs_words};
        VkResult gr = vkCreateShaderModule(V.dev, &gci, NULL, &sh->gs);
        if (gr) { RED(16, "layer: geometry shader for %s: %d", variant, gr); sh->gs = VK_NULL_HANDLE; }
        free(gs_words);
    }
#endif
#ifdef TOTK_OHOS
    { extern void vk_gpu_trace_module(VkShaderModule m, const char *variant); vk_gpu_trace_module(sh->mod, variant); }
#endif
    VkDescriptorSetLayoutBinding *lb = calloc((size_t)(sh->nb ? sh->nb : 1), sizeof *lb);
    for (int i = 0; i < sh->nb; i++) lb[i] = (VkDescriptorSetLayoutBinding){sh->b[i].binding, (VkDescriptorType)sh->b[i].dtype, sh->b[i].count, stage_flags(sh->stage), NULL};
    VkDescriptorSetLayoutCreateInfo dci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = (uint32_t)sh->nb, .pBindings = lb};
    char why[128];
    if (!vk_desc_layout_fits(lb, (uint32_t)sh->nb, why, sizeof why)) {   /* it could never be allocated: refuse it here */
        RED(16, "descriptor set layout of %s: %s", variant, why);
        free(lb); vkDestroyShaderModule(V.dev, sh->mod, NULL); free(sh->b); free(sh->c1); free(sh); return NULL;
    }
    r = vkCreateDescriptorSetLayout(V.dev, &dci, NULL, &sh->dsl);
    if (!r && vk_push_stage >= 0 && sh->stage == vk_push_stage && sh->nb && !tf && !sh->mesh_stage) {   /* oot-a121 TOTK_OHOS_PUSHDESC */
        uint32_t n = 0; for (int i = 0; i < sh->nb; i++) n += sh->b[i].count;
        VkDescriptorSetLayoutCreateInfo pci = dci; pci.flags |= VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
        if (n <= vk_push_max && !vkCreateDescriptorSetLayout(V.dev, &pci, NULL, &sh->dsl_push)) vk_push_shader_n[0]++;
        else {
            sh->dsl_push = VK_NULL_HANDLE; uint64_t k = ++vk_push_shader_n[1];
            if (k <= 8 || !(k & (k - 1)))
                LOGV("descriptor push (oot-a121): %s has %u descriptors (maxPushDescriptors %u): its set stays allocated and bound (%llu such so far)",
                     variant, n, vk_push_max, (unsigned long long)k);
        }
    }
    free(lb);
    if (r) { RED(16, "descriptor set layout of %s: %d", variant, r); vkDestroyShaderModule(V.dev, sh->mod, NULL); free(sh->b); free(sh->c1); free(sh); return NULL; }
    sh->xlate_miss_ns = vk_shader_xlate_take_miss(variant);
    nvn_hm_put(&g_shaders, &key, sh);
    HK_ADD(shloads, 1); HK_ADD(shbytes, spv_len + vk_len);
    if (sh->stage == 5) vk_compute_prepare(sh);
    return sh;
}
vk_shader *vk_shader_alpha(vk_shader *base, unsigned func) {
    if (!base || base->stage!=1 || !func || func==NVN_ALPHA_FUNC_ALWAYS) return base;
    if (func>NVN_ALPHA_FUNC_GEQUAL) return NULL;
    return shader_load_m(base->key,base->early_tests | (int)(func<<1),base->stencil_mask);
}
#ifdef TOTK_OHOS
/* The variant of a fragment shader for a draw's depth and stencil writes (earlyz_auto; recording thread, as
   vk_shader_alpha). A shader that has the port's early tests keeps them unless alpha to coverage could drop a fragment
   whose depth or stencil the tests already wrote; one that can discard takes them when the tests write nothing. */
vk_shader *vk_shader_depth_tests(vk_shader *fs, int depth_test, int depth_write, int stencil_test, int a2c) {
    if (!fs || fs->stage != 1 || fs->early_flip) return fs;
    int writes = (depth_test && depth_write) || stencil_test;
    if (fs->early_candidate && earlyz_auto() >= 9) writes = 0;   /* diagnostic: wrong depth where it discards; what do the late tests cost? */
    if (fs->auto_early ? !(a2c && writes) : !(fs->early_candidate && !writes && depth_test)) return fs;   /* no depth test: nothing for the tests to reject */
    vk_shader *twin = shader_load_m(fs->key, (int)(fs->early_tests | fs->alpha_func << 1) | 16, fs->stencil_mask);
    return twin ? twin : fs;
}
/* oot-a106 (timing only): the twin of a fragment shader for TOTK_OHOS_DC_FSNULL / _FSVARY / _NOUSED (dc_fs_mode), chosen
   after vk_shader_depth_tests on the recording thread. Alpha-test variants and the small depth-only shaders stay as they
   are (the latter so that the depth pre-pass, shadow maps and their discards are unchanged). The memo saves the map
   lookup of every draw; a shader object is never freed. */
vk_shader *vk_shader_dc(vk_shader *fs, int writes_ds) {
    int mode = dc_fs_mode();
    if (!mode || !fs || fs->stage != 1 || fs->alpha_func || fs->tf) return fs;
    /* (oot-a107, from the review) a draw that writes depth or stencil keeps a shader that discards or writes its depth:
       the twin would write depth over whole triangles and hide later draws, and the drop would be occlusion, not cost */
    if (writes_ds && (fs->frag_traits & (FT_KILL | FT_DEPTH))) return fs;
    if (mode == DC_FS_NOUSED ? !fs->bindless : fs->spv_bytes < DC_FS_MIN_BYTES) return fs;
    static struct { vk_shader *base, *twin; } memo[256];
    unsigned h = (unsigned)((((uintptr_t)fs >> 4) * 2654435761u) >> 12) & 255u;
    if (memo[h].base == fs) return memo[h].twin;
    vk_shader *twin = shader_load_m(fs->key, (int)(fs->early_tests | fs->alpha_func << 1 | fs->early_flip << 4) | mode << 5, fs->stencil_mask);
    memo[h].base = fs; memo[h].twin = twin ? twin : fs;
    return memo[h].twin;
}
#endif
/* Startup/replay only: exact persisted or program-known variant; shader/layout maps
   remain single-owner even though compute driver calls run on a worker. */
vk_shader *vk_shader_seed(const char *variant, int stage, int early_tests) {
    size_t n = strnlen(variant, 80);
    if (n != 64 && n != 77) return NULL;
    for (size_t i = 0; i < n; i++) if (i == 64 ? variant[i] != '-' : hexval(variant[i]) < 0) return NULL;
    if (early_tests<0 || early_tests>15 || (stage!=1 && early_tests)) return NULL;
    vk_shader *s = shader_load(variant,stage==1 ? early_tests : 0);
    return s && s->stage == stage ? s : NULL;
}

/* A wrapped mesh kernel's pass-through vertex shader, <key>-meshvs in the seed (tools/mesh_wrap_in_container.py): the
   vertex stage of the indexed draw a mesh draw becomes (vk_draw.c mesh_draw). Recording thread. */
vk_shader *vk_shader_mesh_vs(vk_shader *mesh) {
    if (!mesh || mesh->mesh_stage != 7) return NULL;
    if (!mesh->mesh_vs && !mesh->mesh_vs_missing) {
        char name[80]; snprintf(name, sizeof name, "%.64s-meshvs", mesh->key);
        mesh->mesh_vs = shader_load(name, 0);
        if (mesh->mesh_vs && mesh->mesh_vs->stage != 0) mesh->mesh_vs = NULL;
        if (!mesh->mesh_vs) mesh->mesh_vs_missing = 1;
    }
    return mesh->mesh_vs;
}

/* ---------- variants of a shader ---------- */
typedef struct { char *name, *deps; } variant_t;
typedef struct { variant_t *v; int n; uint64_t epoch; } variant_list;
static nvn_hmap g_variants = {.klen = 64};   /* shader key (hex) -> variant_list */
static variant_list *variants_of(const char *key) {
    char k[64]; memcpy(k, key, 64);
    variant_list *vl = nvn_hm_get(&g_variants, k);
    uint64_t epoch = vk_shader_xlate_epoch();
    if (vl && vl->epoch == epoch) return vl;
    if (!vl) vl = calloc(1, sizeof *vl);
    else { for (int i = 0; i < vl->n; i++) { free(vl->v[i].name); free(vl->v[i].deps); } free(vl->v); vl->v = NULL; vl->n = 0; }
    vl->epoch = epoch;
    char path[1024]; snprintf(path, sizeof path, "%s/%.64s.vv", seed_dir(), key);
    char *txt = read_file(path, NULL);
    int cap = 0;
    if (txt) HK_ADD(vlists, 1);
    if (txt) for (char *save = NULL, *line = strtok_r(txt, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char *tab = strchr(line, '\t'); if (tab) *tab = 0;
        if (vl->n == cap) { cap = cap ? 2 * cap : 4; vl->v = realloc(vl->v, (size_t)cap * sizeof *vl->v); }
        vl->v[vl->n].name = strdup(line); vl->v[vl->n].deps = strdup(tab ? tab + 1 : ""); vl->n++;
    }
    free(txt);
    nvn_hm_put(&g_variants, k, vl);
    return vl;
}
/* The request a PC-side fill needs (the iPhone's shader-requests/<variant>.json, ios/tools/fill_shaders.py). */
static void answers_json(const answers_t *a, char *out, size_t cap) {
    size_t o = 0;
#define PUT(...) do { int n_ = snprintf(out + o, o < cap ? cap - o : 0, __VA_ARGS__); if (n_ > 0) o += (size_t)n_; } while (0)
    /* NSJSONSerialization / json.dumps(sort_keys) order: keys sorted as strings ("10" before "2") */
    const char *kinds[4] = {"attr", "fout", "img", "tex"};
    PUT("{");
    for (int k = 0; k < 4; k++) {
        PUT("%s\"%s\":{", k ? "," : "", kinds[k]);
        int n = k == 0 ? 16 : k == 1 ? 8 : k == 2 ? 32 : 128, first = 1;
        char names[128][8]; int idx[128], m = 0;
        for (int i = 0; i < n; i++) { int has = k == 0 ? a->has_attr[i] : k == 1 ? a->has_fout[i] : k == 2 ? a->has_img[i] : a->has_tex[i]; if (has) { snprintf(names[m], 8, "%d", i); idx[m++] = i; } }
        for (int i = 1; i < m; i++) for (int j = i; j > 0 && strcmp(names[j - 1], names[j]) > 0; j--) { char t[8]; memcpy(t, names[j], 8); memcpy(names[j], names[j - 1], 8); memcpy(names[j - 1], t, 8); int ti = idx[j]; idx[j] = idx[j - 1]; idx[j - 1] = ti; }
        for (int j = 0; j < m; j++) {
            int i = idx[j];
            if (k == 2) { char ty[24] = {0}, fm[24] = {0}; const char *slash = strchr(a->img[i], '/');
                if (slash) { snprintf(ty, sizeof ty, "%.*s", (int)(slash - a->img[i]), a->img[i]); snprintf(fm, sizeof fm, "%s", slash + 1); } else snprintf(ty, sizeof ty, "%s", a->img[i]);
                if (fm[0]) PUT("%s\"%s\":{\"fmt\":\"%s\",\"type\":\"%s\"}", first ? "" : ",", names[j], fm, ty); else PUT("%s\"%s\":{\"type\":\"%s\"}", first ? "" : ",", names[j], ty); }
            else PUT("%s\"%s\":\"%s\"", first ? "" : ",", names[j], k == 0 ? a->attr[i] : k == 1 ? a->fout[i] : a->tex[i]);
            first = 0;
        }
        PUT("}");
    }
    PUT("}");
#undef PUT
}
static void request_miss(const char *key, int st, const uint8_t *control, const uint8_t *code, const answers_t *a) {
    char aj[16384]; answers_json(a, aj, sizeof aj);
    unsigned char md[32]; host_sha256(aj, strlen(aj), md);
    char variant[80]; snprintf(variant, sizeof variant, "%.64s-", key);
    for (int i = 0; i < 6; i++) snprintf(variant + 65 + 2 * i, 3, "%02x", md[i]);
    static nvn_hmap missed = {.klen = 80}; char k[80] = {0}; memcpy(k, variant, strlen(variant));
    if (nvn_hm_get(&missed, k)) { static int said; if (said++ < 4) LOGV("[shader-miss] %s: already requested", variant); return; }
    nvn_hm_put(&missed, k, (void *)1);
    const char *dir = getenv("TOTK_SHADER_REQ_DIR"); if (!dir || !*dir) dir = "shader-requests";
    int background = vk_seed_io_active();
#ifdef TOTK_OHOS
    /* HarmonyOS musl: fclose on an open_memstream FILE fails with EBADF (the request was never queued). A request is
       rare (once per missing variant), so it is written to the file directly; TOTK_SHADER_REQ_DIR is absolute (totk_napi.c). */
    background = 0;
#endif
    if (!background) mkdir(dir, 0755);
    int nvn2 = ctl_nvn2(control);
    uint32_t c1o, c1t; memcpy(&c1o, control + (nvn2 ? 0x40 : 0x700), 4); memcpy(&c1t, control + (nvn2 ? 0x44 : 0x704), 4);
    size_t skip = st == 5 || nvn2 ? 0 : 0x30, code_end = c1t;
    if (nvn2) { uint32_t inst; memcpy(&inst, control + 0x38, 4); code_end = inst <= c1t ? inst : c1t; }   /* oot_seed.py request */
    if (!c1t || c1t > (16u << 20) || c1o > c1t || c1t < skip) {
        static int said; if (said++ < 8) LOGV("[shader-miss] %s %s: no request (code section c1 %#x..%#x); its draws are skipped", st == 0 ? "vs" : st == 1 ? "ps" : "cs", variant, c1o, c1t);
        return;
    }
    char path[1024]; snprintf(path, sizeof path, "%s/%s.json", dir, variant);
    char *json = NULL; size_t json_size = 0;
    FILE *f = background ? open_memstream(&json, &json_size) : fopen(path, "w");
    if (!f) { static int said; if (said++ < 8) LOGV("[shader-miss] %s: cannot open the request (%s): %s", variant, background ? "memstream" : path, strerror(errno)); return; }
    HK_ADD(sreq, 1);
    fprintf(f, "{\"variant\":\"%s\",\"stage\":\"%s\",\"depth_minus_w\":%s", variant, st == 0 ? "vs" : st == 1 ? "ps" : "cs", totk_depth_near_minus_w && totk_depth_near_minus_w() ? "true" : "false");
    fprintf(f, ",\"early_fragment_tests\":%s", !nvn2 && vk_nvn_early_tests(control, ctl_key_len(control), st) ? "true" : "false");
    if (st == 5) { uint32_t l[3]; memcpy(l, control + (nvn2 ? 0x58 : 0x748), 12); fprintf(f, ",\"local\":[%u,%u,%u]", l[0], l[1], l[2]); }
    if (nvn2) {   /* Switch 2: Ampere, and compute's NVN2 layout (oot_seed.py request) */
        fputs(",\"arch\":\"sm86\"", f);
        if (st == 5) {
            uint16_t c0; memcpy(&c0, control + 0xba, 2); uint32_t shared; memcpy(&shared, control + 0x64, 4);
            fprintf(f, ",\"nvn2_cs\":[%u,%u,%u,%u,%u,%u,%u]", control[0xb1], control[0xb2], control[0xb3], c0, control[0xa9] * 4u, control[0xb4], control[0xb5]);
            if (shared) fprintf(f, ",\"shared\":%u", shared);
        }
    }
    fputs(",\"c1\":\"", f); for (uint32_t i = c1o; i < c1t; i++) fprintf(f, "%02x", code[i]);
    fputs("\",\"code_hex\":\"", f); for (size_t i = skip; i < code_end; i++) fprintf(f, "%02x", code[i]);
    fprintf(f, "\",%s\n", aj + 1);   /* the answers' own members (and their closing brace) after the request's */
    if (fclose(f)) { static int said; if (said++ < 4) LOGV("[shader-miss] %s: cannot finish the request: %s", variant, strerror(errno)); free(json); return; }
    if (background) {
        if (!vk_seed_io_request(dir, variant, json, json_size)) { free(json); RED(4, "shader request queue allocation failed: %s", variant); }
    } else vk_shader_xlate_enqueue(path, variant, seed_dir());
    static uint64_t n; n++;
    if (n <= 20 || n % 100 == 0) LOGV("[shader-miss] %s %s is not in the seed: its draws are skipped (request %llu %s in %s)", st == 0 ? "vs" : st == 1 ? "ps" : "cs", variant, (unsigned long long)n, background ? "queued" : "saved", dir);
}

/* ---------- programs ---------- */
/* Lookout Landing: vk_treg_kind was 8.6% of recording. Binding hooks and the registry revision cover every signature input. */
static uint64_t g_stage_sig[6], g_stage_sig_revision[6];
static unsigned g_stage_sig_valid;
void vk_stage_sig_invalidate(unsigned stages) { g_stage_sig_valid &= ~stages; }   /* S and this memo belong to the recording thread */
static uint64_t stage_sig_for(const vk_state *state, int st) {
    uint64_t sg = 0;
    if (st == 1 && state->has_rt) for (int i = 0; i < state->rt.n && i < 8; i++) if (state->rt.color[i])
        sg |= (uint64_t)(1 + vk_fmt_class(state->rt.has_cview[i] && state->rt.cview[i].set_format ? state->rt.cview[i].format : state->rt.color[i]->d.format)) << (3 * i);
    if (st == 0) for (int i = 0; i < state->nattr && i < 16; i++) sg |= (uint64_t)vk_fmt_class(state->attr[i].fmt) << (2 * i);
    uint64_t dm = 0; int hi = st >= 0 && st < 6 && state->tex_hi[st] ? state->tex_hi[st] : 128;
    for (int r = 0; r < hi && r < 128; r++) if (state->tex[st][r]) {
        int f, tg; if (!vk_treg_kind((int)(state->tex[st][r] & 0xfffff), &f, &tg)) continue;
        if (f >= 0x32 && f <= 0x36) {
            dm ^= 0x9E3779B97F4A7C15ull * (uint64_t)(r + 1);
            if (vk_treg_stencil((int)(state->tex[st][r] & 0xfffff))) dm ^= 0xC2B2AE3D27D4EB4Full * (uint64_t)(r + 1);
        }
    }
    if (dm) sg ^= dm | 1ull << 63;
    return sg;
}
static uint64_t stage_sig_at(int st, uint64_t revision) {   /* nvn_mtl.m stage_sig */
    if ((g_stage_sig_valid & (1u << st)) && g_stage_sig_revision[st] == revision) return g_stage_sig[st];
    uint64_t sg = stage_sig_for(&S, st);
    g_stage_sig[st] = sg; g_stage_sig_revision[st] = revision; g_stage_sig_valid |= 1u << st;   /* pre-scan revision: a concurrent registration invalidates the next lookup */
    return sg;
}
static uint64_t stage_sig(int st) { return stage_sig_at(st, vk_treg_revision()); }
typedef struct { uint64_t sig; vk_shader *sh; int missing; } alt_t;
struct vk_prog {
    char key[6][65]; const uint8_t *ctl[6], *code[6]; int has[6]; unsigned stages;
    uint64_t xlate_epoch;
    uint64_t prepare_epoch; int prepared;
    alt_t *alt[6]; int nalt[6], capalt[6];
    vk_shader *cur[6]; uint64_t cur_sig[6]; int cur_ok[6];
    struct { uint64_t registry, xlate_epoch, signature[6]; vk_shader *shader[6]; int ready, result; } record;
};
static nvn_hmap g_progs = {.klen = 8};   /* prog_t serial -> vk_prog */
/* The game releases programs on its own threads (nvnProgramSetShaders / Finalize) while the replay thread looks them
   up and adds them; the map's growth frees its arrays, so every access holds this. */
static pthread_mutex_t g_progs_mu = PTHREAD_MUTEX_INITIALIZER;
/* Recording thread only (vk_prog_resolve: draw / dispatch under vk_rec_lock). A small direct-mapped cache by serial in
   front of the locked map: locking g_progs_mu for every draw was ~4 % of recording at Lookout Landing. Safe because
   serials are never reused (nvn_new_serial; ProgramSetShaders takes a new one) and a vk_prog is never freed. */
enum { PROGC_N = 64 };
static struct { uint64_t serial; vk_prog *pr; } g_progc[PROGC_N];
static vk_prog *prog_of(const prog_t *p) {
    if (!p || !p->serial) return NULL;
    unsigned h = (unsigned)(p->serial % PROGC_N);
    if (g_progc[h].serial == p->serial) return g_progc[h].pr;
    pthread_mutex_lock(&g_progs_mu); vk_prog *pr = nvn_hm_get(&g_progs, &p->serial); pthread_mutex_unlock(&g_progs_mu);
    if (pr) { g_progc[h].serial = p->serial; g_progc[h].pr = pr; return pr; }
    pr = calloc(1, sizeof *pr);
    for (unsigned i = 0; i < p->nstage && i < 6; i++) {
        const uint8_t *control = p->sd[i].control; uint32_t length = 0, st = 0;
        if (!control || !p->sd[i].data) {
            static int said; if (said++ < 8) LOGV("[shader-miss] program %llu stage %u has no %s: its draws are skipped as \"shader missing\"", (unsigned long long)p->serial, i, control ? "data" : "control section");
            continue;
        }
        int sti = ctl_nvn_stage(control, (const uint8_t *)(uintptr_t)p->sd[i].data);
        length = ctl_code_length(control); st = sti < 0 ? 0xffu : (uint32_t)sti;
        /* NVN2 task / mesh shaders: compute kernels in the seed (tools/mesh_wrap_in_container.py), kept in the tessellation
           stages' places like their bindings (hle_nvn.c st_host); vk_draw.c mesh_draw runs them */
        int mesh = sti == -6 || sti == -7;
        if (mesh) st = sti == -6 ? 3 : 4;
        if (!length || length > (16u << 20) || (st != 0 && st != 1 && st != 5 && !mesh)) {
            RED(8, "shader stage %d (length %u%s) is not supported: its draws are skipped", sti, length, sti == -6 ? ", NVN2 task" : sti == -7 ? ", NVN2 mesh" : ctl_nvn2(control) ? ", NVN2" : "");
            continue;
        }
        host_sha256_ctx ctx; unsigned char md[32];
        host_sha256_init(&ctx); ctl_key_hash(&ctx, control); host_sha256_update(&ctx, (const void *)(uintptr_t)p->sd[i].data, length); host_sha256_final(&ctx, md);
        for (int j = 0; j < 32; j++) snprintf(pr->key[st] + 2 * j, 3, "%02x", md[j]);
        pr->ctl[st] = control; pr->code[st] = (const uint8_t *)(uintptr_t)p->sd[i].data; pr->has[st] = 1; pr->stages |= 1u << st;
    }
    pthread_mutex_lock(&g_progs_mu); nvn_hm_put(&g_progs, &p->serial, pr); pthread_mutex_unlock(&g_progs_mu);
    g_progc[h].serial = p->serial; g_progc[h].pr = pr;
    return pr;
}
void vk_prog_prepare(const prog_t *p) {
    if (!vk_shaders_ready()) return;
    vk_prog *pr = prog_of(p); if (!pr) return;
    uint64_t epoch = vk_shader_xlate_epoch();
    if (pr->prepared && pr->prepare_epoch == epoch) return;
    pr->prepared = 1; pr->prepare_epoch = epoch;
    for (unsigned stages = pr->stages; stages; stages &= stages - 1) {
        int st = __builtin_ctz(stages);
        /* Compute pipelines depend only on the exact shader, not dispatch-time
           resources. Compile all listed variants without choosing one using stale
           bind state. Dispatch still uses the authoritative ordinary resolver. */
        if (st == 5) {
            variant_list *vl = variants_of(pr->key[st]);
            for (int i = 0; i < vl->n; i++) (void)vk_shader_seed(vl->v[i].name, 5, 0);
        }
        if (st == 3 || st == 4) {   /* task / mesh kernels: their pipelines start building at the bind (a mesh draw is skipped while they build) */
            variant_list *vl = variants_of(pr->key[st]);
            for (int i = 0; i < vl->n; i++) {
                vk_shader *k = vk_shader_seed(vl->v[i].name, 5, 0);
                if (k && k->mesh_stage) { VkPipelineLayout layout; (void)vk_compute_pipeline(k, &layout); }
            }
        }
    }
}
void mtl_prog_release(prog_t *p) { if (!p->serial) return; pthread_mutex_lock(&g_progs_mu); nvn_hm_del(&g_progs, &p->serial); pthread_mutex_unlock(&g_progs_mu); }   /* the vk_prog leaks a little; programs are few */
#ifdef TOTK_OHOS
/* The transform feedback capture variant of the program's vertex shader, and the four buffer strides (0: unused);
   NULL when the program's control section names no feedback layout (vk_tf.inc). Recording thread. */
vk_shader *vk_shader_tf(const prog_t *p, vk_shader *vs, uint32_t stride[4]) {
    vk_prog *pr = prog_of(p);
    if (!pr || !vs || !pr->has[0] || !pr->ctl[0]) return NULL;
    za_tf_layout L; if (ctl_nvn2(pr->ctl[0]) || !tf_layout_parse(pr->ctl[0], &L)) return NULL;   /* NVN2 has no NVN-layout feedback table */
    for (int b = 0; b < 4; b++) stride[b] = L.stride[b];
    g_tf_pending = L;
    return shader_load_x(vs->key, (int)(vs->early_tests | vs->alpha_func << 1), 0, 1);
}
#endif
/* stages: the stages to resolve (bit = stage). A mesh draw resolves its task and mesh kernels alone, also for draws further
   down the command list whose pixel stage bindings are not bound yet: resolving that stage there would pick, load or
   request a variant for the wrong bind state. */
int vk_prog_resolve(const prog_t *p, vk_shader *out[6]) { return vk_prog_resolve_stages(p, out, 0x3fu); }
int vk_prog_resolve_stages(const prog_t *p, vk_shader *out[6], unsigned mask) {
    memset(out, 0, 6 * sizeof *out);
    if (!vk_shaders_ready()) return -1;
    vk_prog *pr = prog_of(p);
    if (!pr) return -1;
    uint64_t epoch = vk_shader_xlate_epoch();
    if (pr->xlate_epoch != epoch) {
        /* Shader maps and negative program caches are owned here, never by the child/worker.
           Ready variants remain valid; only misses must resolve again after publication. */
        for (int st = 0; st < 6; st++) {
            int n = 0;
            for (int i = 0; i < pr->nalt[st]; i++) if (!pr->alt[st][i].missing) pr->alt[st][n++] = pr->alt[st][i];
            pr->nalt[st] = n;
            if (!pr->cur[st]) pr->cur_ok[st] = 0;
        }
        pr->xlate_epoch = epoch;
    }
    int missing = 0;
    for (int st = 0; st < 6; st++) {
        if (!pr->has[st] || !(mask >> st & 1)) continue;
        uint64_t sg = stage_sig(st);
        if (pr->cur_ok[st] && pr->cur_sig[st] == sg) { out[st] = pr->cur[st]; if (!out[st]) missing = 1; continue; }
        alt_t *hit = NULL;
        for (int i = 0; i < pr->nalt[st]; i++) if (pr->alt[st][i].sig == sg) { hit = &pr->alt[st][i]; break; }
        if (!hit) {
            answers_t a; answers_of(st, &a);
            variant_list *vl = variants_of(pr->key[st]);
            vk_shader *sh = NULL;
            for (int i = 0; i < vl->n && !sh; i++) if (deps_match(vl->v[i].deps, &a)) sh = shader_load(vl->v[i].name, !ctl_nvn2(pr->ctl[st]) && vk_nvn_early_tests(pr->ctl[st], ctl_key_len(pr->ctl[st]), st));   /* NVN2: the seed's variants are translated without early tests (oot_seed.py) */
#ifdef TOTK_OHOS
            if (sh && st < 5) {   /* slots holding a stencil view: the variant that reads them as UINT */
                uint64_t m = 0;
                for (int i = 0; i < sh->nb; i++) { const vk_bind *b = &sh->b[i];
                    if (b->dtype != VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER || b->reg < 0 || b->reg >= 128 || b->binding >= 64 || !S.tex[st][b->reg]) continue;
                    if (vk_treg_stencil((int)(S.tex[st][b->reg] & 0xfffff))) m |= 1ull << b->binding; }
                if (m) { vk_shader *v = shader_load_m(sh->key, (int)(sh->early_tests | sh->alpha_func << 1), m); if (v) sh = v; }
            }
#endif
            if (sh && (st == 3 || st == 4) && sh->mesh_stage != (st == 3 ? 6 : 7)) {
                RED(8, "%s kernel %.16s in the seed is not wrapped for the replay (no \"mesh\" line): its draws are skipped", st == 3 ? "task" : "mesh", pr->key[st]);
                sh = NULL;
            } else if (!sh) {
                static int said; if (said++ < 20) LOGV("[shader-miss] %s %.16s: %s (%d variant(s) listed)", st == 0 ? "vs" : st == 1 ? "ps" : st == 3 ? "task" : st == 4 ? "mesh" : "cs", pr->key[st], vl->n ? "no variant matches this bind state" : "not in the seed", vl->n);
                if (st != 3 && st != 4) request_miss(pr->key[st], st, pr->ctl[st], pr->code[st], &a);   /* a task / mesh kernel is wrapped on the PC (oot_mesh.py), not filled */
            }
            if (pr->nalt[st] == pr->capalt[st]) { pr->capalt[st] = pr->capalt[st] ? 2 * pr->capalt[st] : 2; pr->alt[st] = realloc(pr->alt[st], (size_t)pr->capalt[st] * sizeof *pr->alt[st]); }
            pr->alt[st][pr->nalt[st]++] = (alt_t){sg, sh, !sh};
            hit = &pr->alt[st][pr->nalt[st] - 1];
        }
        pr->cur[st] = hit->sh; pr->cur_sig[st] = sg; pr->cur_ok[st] = 1;
        out[st] = hit->sh; if (!hit->sh) missing = 1;
    }
    return missing ? -1 : 0;
}
/* Prediction only borrows an already-resolved variant. It never loads a shader, caches a miss,
   or changes the active program/signature/recorder memo. Programs and shader objects never move or die. */
int vk_prog_predict(const vk_state *state, vk_shader *out[6]) {
    memset(out, 0, 6 * sizeof *out);
    const prog_t *p = state->prog; if (!p || !p->serial) return 0;
    unsigned h = (unsigned)(p->serial % PROGC_N);
    vk_prog *pr = g_progc[h].serial == p->serial ? g_progc[h].pr : NULL;
    if (!pr) {
        pthread_mutex_lock(&g_progs_mu); pr = nvn_hm_get(&g_progs, &p->serial); pthread_mutex_unlock(&g_progs_mu);
    }
    if (!pr || !pr->has[0] || pr->has[5]) return 0;
    uint64_t revision = vk_treg_revision();
    for (int st = 0; st < 2; st++) if (pr->has[st]) {
        uint64_t sig = stage_sig_for(state, st);
        if (pr->cur_ok[st] && pr->cur_sig[st] == sig) out[st] = pr->cur[st];
        else for (int i = 0; i < pr->nalt[st]; i++) if (pr->alt[st][i].sig == sig) { out[st] = pr->alt[st][i].sh; break; }
        if (!out[st]) return 0;
    }
    return revision == vk_treg_revision();
}
/* Recorder memo compares the active program's semantic signatures, not invalidation flags or unrelated stages.
   Recomputing a dirty signature can prove an NVN rebind did not change the variant selected by the ordinary resolver.
   Compiler workers publish pipeline handles, not shader/variant maps; no pipeline handle is cached here.
   The service's publication epoch also keys this memo, including its negative results. */
static uint64_t g_record_prog_hits, g_record_prog_misses;
int vk_prog_resolve_record(const prog_t *p, vk_shader *out[6]) {
    /* Owned by the program, so unrelated serials cannot evict a hot result. Programs are never freed. */
    if (!p || !p->serial || !vk_shaders_ready()) return vk_prog_resolve(p, out);
    vk_prog *program = prog_of(p); if (!program) return vk_prog_resolve(p, out);
    uint64_t registry = vk_treg_revision(), epoch = vk_shader_xlate_epoch(), signatures[6];
    int refreshed = 0, same = 1;
    __typeof__(program->record) *m = &program->record;
    /* A program's stage set is immutable. Visit only those entries, rather than
       clearing/comparing six signatures and probing all four absent graphics stages.
       The pre-scan registry revision still invalidates the next draw on publication. */
    for (unsigned stages = program->stages; stages; stages &= stages - 1) {
        int st = __builtin_ctz(stages);
        refreshed |= !(g_stage_sig_valid & (1u << st)) || g_stage_sig_revision[st] != registry;
        signatures[st] = stage_sig_at(st, registry);
        same &= m->signature[st] == signatures[st];
    }
    if (m->ready && m->xlate_epoch == epoch && same) {
        if (refreshed || m->registry != registry) vk_record_memo_saved(VK_REC_M_SIGNATURE);
        m->registry = registry;
        memcpy(out, m->shader, sizeof m->shader); g_record_prog_hits++; return m->result;
    }
    unsigned causes = !m->ready || m->xlate_epoch != epoch ? (1u << VK_REC_M_COLD) | (1u << VK_REC_M_PROGRAM) : 0;
    if (m->ready && !same) causes |= 1u << VK_REC_M_SIGNATURE;
    if (m->ready && m->registry != registry) causes |= 1u << VK_REC_M_REGISTRY;
    vk_record_memo_miss(1, causes);
    g_record_prog_misses++; int result = vk_prog_resolve(p, out);
    m->ready = 1; m->registry = registry; m->xlate_epoch = epoch; m->result = result;
    for (unsigned stages = program->stages; stages; stages &= stages - 1) {
        int st = __builtin_ctz(stages); m->signature[st] = signatures[st];
    }
    memcpy(m->shader, out, sizeof m->shader);
    return result;
}
void vk_prog_record_cache_take(uint64_t *hits, uint64_t *misses) {
    *hits = g_record_prog_hits; *misses = g_record_prog_misses; g_record_prog_hits = g_record_prog_misses = 0;
}
#ifdef TOTK_VK_TESTS
uint64_t vk_prog_record_test_hits(void) { return g_record_prog_hits; }
#endif

/* ---------- pipeline layouts: set 0 = vertex (or compute) stage, set 1 = fragment stage ---------- */
static VkDescriptorSetLayout empty_dsl(void) {
    static VkDescriptorSetLayout e;
    if (!e) { VkDescriptorSetLayoutCreateInfo ci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; VKCHECK(vkCreateDescriptorSetLayout(V.dev, &ci, NULL, &e)); }
    return e;
}
VkPipelineLayout vk_layout_of(vk_shader *vs, vk_shader *fs) {
    static nvn_hmap layouts = {.klen = 16};
    void *k[2] = {vs, fs};
    VkPipelineLayout l = (VkPipelineLayout)nvn_hm_get(&layouts, k);
    if (l) return l;
    VkDescriptorSetLayout sets[3] = {vs ? (vs->dsl_push ? vs->dsl_push : vs->dsl) : empty_dsl(), fs ? (fs->dsl_push ? fs->dsl_push : fs->dsl) : empty_dsl(), VK_NULL_HANDLE};
    if (vs && fs && vs->dsl_push && fs->dsl_push) DIE("descriptor push (oot-a121): two push sets in one layout (%.16s, %.16s)", vs->key, fs->key);   /* TOTK_OHOS_PUSHDESC picks one stage */
    uint32_t nsets = fs ? 2 : 1;
    if ((vs && vs->bindless) || (fs && fs->bindless)) { sets[2] = vk_bindless_layout(); nsets = 3; }   /* set 1 stays empty for compute */
    VkPipelineLayoutCreateInfo ci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = nsets, .pSetLayouts = sets};
    VKCHECK(vkCreatePipelineLayout(V.dev, &ci, NULL, &l));
    nvn_hm_put(&layouts, k, (void *)l);
    return l;
}
