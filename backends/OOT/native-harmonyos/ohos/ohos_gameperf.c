/* HarmonyOS GameServiceKit scene hints (libgame_performance.z.so, API 14): tell the system's game scheduler that this
   is a heavy Vulkan RPG scene at importance 5 with recommended CPU / GPU / DDR level HIGH, the frame rate it aims for,
   and its key threads (render = totk-walk, logic = g-main), so CPU and GPU frequencies are held instead of following
   the bursty load. Loaded with dlopen: a missing library or an authentication failure (self-signed builds may not
   pass HMS_GamePerformance_Init) only logs. TOTK_OHOS_GAMEPERF=0 skips it. Signatures as in
   hms/native/sysroot/usr/include/GameServiceKit/game_performance.h (opaque handles, enums as int). */
#include <dirent.h>
#include <dlfcn.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef int (*fn_create)(void **);
typedef int (*fn_str)(void *, const char *);
typedef int (*fn_int)(void *, int);
typedef int (*fn_i64)(void *, int64_t);
typedef int (*fn_bool)(void *, bool);
typedef int (*fn_obj)(void *);

static void *g_lib;
static void *sym(const char *name) {
    void *p = g_lib ? dlsym(g_lib, name) : NULL;
    if (!p) fprintf(stderr, "[ohos-gameperf] missing %s\n", name);
    return p;
}
#define CALL(type, name, ...) ({ type f_ = (type)sym("HMS_GamePerformance_" name); int r_ = f_ ? f_(__VA_ARGS__) : -1; \
    if (r_) fprintf(stderr, "[ohos-gameperf] %s -> %d\n", name, r_); r_; })

static void *g_scene;

/* OpenGTX (libopengtx.so, API 12; hms/native/sysroot/usr/include/graphics_game_sdk/opengtx_base.h), oot-a112 experiment:
   the docs say the system then adjusts the frame rate and the SoC / DDR frequencies to the scene; this port reports
   HEAVY_LOAD. On by default (TOTK_OHOS_OPENGTX=0 turns it off), after TOTK_OHOS_OPENGTX_DELAY seconds (30:
   the game's threads exist by then and are named). The structs mirror the header field for field (enums are int). */
typedef struct { int32_t height, width; } gtx_res;
typedef struct {
    int mode; int32_t targetFPS; char *packageName; char *appVersion; int engineType; char *engineVersion; int gameType;
    int pictureQualityMaxLevel; gtx_res resolutionMaxValue; int32_t gameMainThreadId; int32_t gameRenderThreadId;
    int32_t gameKeyThreadIds[5]; bool vulkanSupport;
} gtx_config;
typedef struct { int sceneID; char *description; int32_t recommendFPS, minFPS, maxFPS; gtx_res resolutionCurValue; } gtx_scene;
_Static_assert(sizeof(gtx_config) == 88 && sizeof(gtx_scene) == 40, "OpenGTX struct layout");
static void gtx_temp(int level) { fprintf(stderr, "[ohos-opengtx] temperature level %d\n", level); }
/* the thread ids of the first n threads named name (/proc/self/task/<tid>/comm) */
static int tids_named(const char *name, int32_t *out, int n) {
    char path[64], comm[32]; int k = 0;
    DIR *d = opendir("/proc/self/task"); if (!d) return 0;
    struct dirent *de;
    while (k < n && (de = readdir(d))) {
        int tid = atoi(de->d_name); if (tid <= 0) continue;
        snprintf(path, sizeof path, "/proc/self/task/%d/comm", tid);
        FILE *f = fopen(path, "r"); if (!f) continue;
        if (fgets(comm, sizeof comm, f)) { comm[strcspn(comm, "\n")] = 0; if (!strcmp(comm, name)) out[k++] = tid; }
        fclose(f);
    }
    closedir(d);
    return k;
}
/* oot-a113 (standing view, 4 pairs): GPU at 893-903 MHz on average with it (8-10 of 12 samples at 933) against 784-843,
   +1.5-2.5% fps every pair (a GPU time that barely follows the clock: the frame is memory-bound). On by default since
   oot-a114; TOTK_OHOS_OPENGTX=0 turns it off. */
static void opengtx_start(const char *bundle, const char *version, int frame_rate) {
    const char *e = getenv("TOTK_OHOS_OPENGTX");
    if (e && !strcmp(e, "0")) { fprintf(stderr, "[ohos-opengtx] off (TOTK_OHOS_OPENGTX=0)\n"); return; }
    const char *ds = getenv("TOTK_OHOS_OPENGTX_DELAY"); int delay = ds && *ds ? atoi(ds) : 30;
    if (delay > 0) sleep((unsigned)delay);
    void *lib = dlopen("libopengtx.so", RTLD_NOW | RTLD_LOCAL);
    if (!lib) { fprintf(stderr, "[ohos-opengtx] libopengtx.so: %s\n", dlerror()); return; }
    void *(*create)(void (*)(int)) = (void *(*)(void (*)(int)))dlsym(lib, "HMS_OpenGTX_CreateContext");
    int (*config)(void *, const gtx_config *) = (int (*)(void *, const gtx_config *))dlsym(lib, "HMS_OpenGTX_SetConfiguration");
    int (*activate)(void *) = (int (*)(void *))dlsym(lib, "HMS_OpenGTX_Activate");
    int (*scene)(void *, const gtx_scene *) = (int (*)(void *, const gtx_scene *))dlsym(lib, "HMS_OpenGTX_DispatchGameSceneInfo");
    if (!create || !config || !activate || !scene) { fprintf(stderr, "[ohos-opengtx] a symbol is missing\n"); return; }
    void *ctx = create(gtx_temp);
    if (!ctx) { fprintf(stderr, "[ohos-opengtx] CreateContext: NULL\n"); return; }
    int32_t walk[7] = {0}; int nw = tids_named("totk-walk", walk, 7);
    gtx_config c; memset(&c, 0, sizeof c);
    c.mode = 0x0001; c.targetFPS = frame_rate; c.packageName = (char *)bundle; c.appVersion = (char *)version;
    c.engineType = 100; c.engineVersion = (char *)"1.0.0"; c.gameType = 2; c.pictureQualityMaxLevel = 3;
    c.resolutionMaxValue = (gtx_res){1080, 1920}; c.gameMainThreadId = walk[0]; c.gameRenderThreadId = nw > 1 ? walk[1] : walk[0];
    for (int i = 0; i < 5 && i + 2 < nw; i++) c.gameKeyThreadIds[i] = walk[i + 2];
    c.vulkanSupport = true;
    int rc = config(ctx, &c), ra = activate(ctx);
    /* HEAVY_LOAD; recommend / min / max fps: 0 (not in effect) or 30 .. targetFPS (a112: a min of 20 was refused, 401) */
    int fps = frame_rate >= 30 ? frame_rate : 30;
    gtx_scene s = {7, (char *)"open world", fps, 0, fps, {1080, 1920}};
    int rs = scene(ctx, &s);
    fprintf(stderr, "[ohos-opengtx] SetConfiguration %d, Activate %d, DispatchGameSceneInfo (HEAVY_LOAD, %d fps, %d totk-walk threads: %d %d) %d\n",
            rc, ra, frame_rate, nw, (int)walk[0], (int)(nw > 1 ? walk[1] : 0), rs);
}

static void ohos_gameperf_scene(const char *bundle, const char *version, int frame_rate);
/* Called once the game runs (frontend start), on its own thread; frame_rate: the rate the game aims for (30 handheld). */
void ohos_gameperf_start(const char *bundle, const char *version, int frame_rate) {
    ohos_gameperf_scene(bundle, version, frame_rate);
    opengtx_start(bundle, version, frame_rate);
}
static void ohos_gameperf_scene(const char *bundle, const char *version, int frame_rate) {
    const char *e = getenv("TOTK_OHOS_GAMEPERF");
    if (e && !strcmp(e, "0")) { fprintf(stderr, "[ohos-gameperf] off (TOTK_OHOS_GAMEPERF=0)\n"); return; }
    g_lib = dlopen("libgame_performance.z.so", RTLD_NOW | RTLD_LOCAL);
    if (!g_lib) { fprintf(stderr, "[ohos-gameperf] libgame_performance.z.so: %s\n", dlerror()); return; }
    void *init = NULL;
    if (CALL(fn_create, "CreateInitParameters", &init) || !init) return;
    CALL(fn_str, "InitParameters_SetBundleName", init, bundle);
    CALL(fn_str, "InitParameters_SetAppVersion", init, version);
    int r = CALL(fn_obj, "Init", init);
    fprintf(stderr, "[ohos-gameperf] Init: %d%s\n", r, r == 1010300002 ? " (authentication failed)" : "");
    if (r) return;
    void *pkg = NULL;
    if (!CALL(fn_create, "CreatePackageInfo", &pkg) && pkg) {
        CALL(fn_str, "PackageInfo_SetBundleName", pkg, bundle);
        CALL(fn_str, "PackageInfo_SetAppVersion", pkg, version);
        CALL(fn_int, "PackageInfo_SetEngineType", pkg, 200);          /* OTHERS */
        CALL(fn_str, "PackageInfo_SetEngineVersion", pkg, "1.0.0");
        CALL(fn_int, "PackageInfo_SetGameType", pkg, 2);              /* RPG */
        CALL(fn_bool, "PackageInfo_SetVulkanSupported", pkg, true);
        fprintf(stderr, "[ohos-gameperf] UpdatePackageInfo: %d\n", CALL(fn_obj, "UpdatePackageInfo", pkg));
    }
    if (CALL(fn_create, "CreateSceneInfo", &g_scene) || !g_scene) return;
    CALL(fn_i64, "SceneInfo_SetSceneID", g_scene, 7);   /* as the page (Game.ets reportGameScene): the two reports no longer differ, whichever lands last (oot-a103) */
    CALL(fn_str, "SceneInfo_SetDescription", g_scene, "open world");
    CALL(fn_int, "SceneInfo_SetImportanceLevel", g_scene, 5);
    CALL(fn_int, "SceneInfo_SetRecommendedCpuLevel", g_scene, 3);    /* HIGH */
    CALL(fn_int, "SceneInfo_SetRecommendedGpuLevel", g_scene, 3);
    CALL(fn_int, "SceneInfo_SetRecommendedDdrLevel", g_scene, 3);
    CALL(fn_i64, "SceneInfo_SetMaxFrameRate", g_scene, frame_rate);
    CALL(fn_i64, "SceneInfo_SetCurrentFrameRate", g_scene, frame_rate);
    /* game_performance.h: "render|xxx|logic|xxx" (role, thread name), at most 32 characters; a plain list of thread
       names was refused with 401 (PARAM_INVALID). The recorder workers and audio have no role in that format. */
    /* g-main is named totk-walk from its first walk on (it replays its own submissions), so that is the logic thread's name */
    CALL(fn_str, "SceneInfo_SetKeyThread", g_scene, "render|totk-walk|logic|totk-walk");   /* this port's hot threads (thread_place.h, vk_record.c) */
    fprintf(stderr, "[ohos-gameperf] UpdateSceneInfo (scene 7, importance 5, CPU/GPU/DDR high, %d fps, key threads render=totk-walk logic=totk-walk): %d\n",
            frame_rate, CALL(fn_obj, "UpdateSceneInfo", g_scene));
}
