/* HarmonyOS GameServiceKit scene hints (libgame_performance.z.so, API 14): tell the system's game scheduler that this
   is a heavy Vulkan RPG scene at importance 5 with recommended CPU / GPU / DDR level HIGH, the frame rate it aims for,
   and its key threads (render = totk-walk, logic = g-main), so CPU and GPU frequencies are held instead of following
   the bursty load. Loaded with dlopen: a missing library or an authentication failure (self-signed builds may not
   pass HMS_GamePerformance_Init) only logs. TOTK_OHOS_GAMEPERF=0 skips it. Signatures as in
   hms/native/sysroot/usr/include/GameServiceKit/game_performance.h (opaque handles, enums as int).
   Also here, each behind its own switch: the scene ID (TOTK_OHOS_SCENE_ID, default 7 = gaming), a device-state line
   every 5 s (TOTK_OHOS_DEVSTATE=1, default off) and the OpenGTX HEAVY_LOAD scene report (TOTK_OHOS_OPENGTX=1, default
   off; TOTK_OHOS_OPENGTX_DELAY, default 30 s). */
#include <dirent.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
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
typedef int (*fn_query)(void *, void **);   /* QueryThermalInfo(parameters, &info) */
typedef int (*fn_get)(void *, int32_t *);   /* GpuInfo_Get* / ThermalInfo_Get* */

static void *g_lib;
static void *sym(const char *name) {
    void *p = g_lib ? dlsym(g_lib, name) : NULL;
    if (!p) fprintf(stderr, "[ohos-gameperf] missing %s\n", name);
    return p;
}
#define CALL(type, name, ...) ({ type f_ = (type)sym("HMS_GamePerformance_" name); int r_ = f_ ? f_(__VA_ARGS__) : -1; \
    if (r_) fprintf(stderr, "[ohos-gameperf] %s -> %d\n", name, r_); r_; })

static void *g_scene;

/* the switches that default to off: set and not "0" */
static int env_on(const char *name) { const char *e = getenv(name); return e && *e && strcmp(e, "0"); }

/* The thread id of the first thread named names[i] (/proc/self/task/<tid>/comm) into out[i], 0 when there is none;
   returns how many names were found. One pass over the task list. */
static int tids_named(const char *const *names, int32_t *out, int n) {
    char path[64], comm[32]; int found = 0;
    for (int i = 0; i < n; i++) out[i] = 0;
    DIR *d = opendir("/proc/self/task"); if (!d) return 0;
    struct dirent *de;
    while (found < n && (de = readdir(d))) {
        int tid = atoi(de->d_name); if (tid <= 0) continue;
        snprintf(path, sizeof path, "/proc/self/task/%d/comm", tid);
        FILE *f = fopen(path, "r"); if (!f) continue;
        if (fgets(comm, sizeof comm, f)) {
            comm[strcspn(comm, "\n")] = 0;
            for (int i = 0; i < n; i++) if (!out[i] && !strcmp(comm, names[i])) { out[i] = tid; found++; break; }
        }
        fclose(f);
    }
    closedir(d);
    return found;
}
/* how many threads are named name */
static int tids_count(const char *name) {
    char path[64], comm[32]; int k = 0;
    DIR *d = opendir("/proc/self/task"); if (!d) return 0;
    struct dirent *de;
    while ((de = readdir(d))) {
        int tid = atoi(de->d_name); if (tid <= 0) continue;
        snprintf(path, sizeof path, "/proc/self/task/%d/comm", tid);
        FILE *f = fopen(path, "r"); if (!f) continue;
        if (fgets(comm, sizeof comm, f)) { comm[strcspn(comm, "\n")] = 0; k += !strcmp(comm, name); }
        fclose(f);
    }
    closedir(d);
    return k;
}

/* OpenGTX (libopengtx.so, API 12; hms/native/sysroot/usr/include/graphics_game_sdk/opengtx_base.h), from the Ocarina of
   Time port (oot-a112 - a114: GPU at 893-903 MHz on average with it against 784-843, +1.5-2.5% fps in 4 pairs there):
   the docs say the system then adjusts the frame rate and the SoC / DDR frequencies to the scene; this port reports
   HEAVY_LOAD. Off by default here (TOTK_OHOS_OPENGTX=1 turns it on): no measurement on this game yet. It starts
   TOTK_OHOS_OPENGTX_DELAY seconds (30) after the scene report and then waits, checking every second for at most 120 s,
   until a thread named totk-walk exists (the walker is named on the game's first submission). The structs mirror the
   header field for field (enums are int); the asserts hold the header's sizes and offsets (aarch64). */
typedef struct { int32_t height, width; } gtx_res;
typedef struct {
    int mode; int32_t targetFPS; char *packageName; char *appVersion; int engineType; char *engineVersion; int gameType;
    int pictureQualityMaxLevel; gtx_res resolutionMaxValue; int32_t gameMainThreadId; int32_t gameRenderThreadId;
    int32_t gameKeyThreadIds[5]; bool vulkanSupport;
} gtx_config;
typedef struct { int sceneID; char *description; int32_t recommendFPS, minFPS, maxFPS; gtx_res resolutionCurValue; } gtx_scene;
_Static_assert(sizeof(gtx_config) == 88 && sizeof(gtx_scene) == 40, "OpenGTX struct layout");
_Static_assert(offsetof(gtx_config, engineType) == 24 && offsetof(gtx_config, engineVersion) == 32 &&
               offsetof(gtx_config, resolutionMaxValue) == 48 && offsetof(gtx_config, gameMainThreadId) == 56 &&
               offsetof(gtx_config, gameKeyThreadIds) == 64 && offsetof(gtx_config, vulkanSupport) == 84 &&
               offsetof(gtx_scene, description) == 8 && offsetof(gtx_scene, recommendFPS) == 16 &&
               offsetof(gtx_scene, resolutionCurValue) == 28, "OpenGTX field offsets");
static void gtx_temp(int level) { fprintf(stderr, "[ohos-opengtx] temperature level %d\n", level); }
typedef struct { char *bundle, *version; int frame_rate, delay; } gtx_args;
static void *opengtx_thread(void *arg) {
    gtx_args *a = arg;   /* kept for the process's life: the context may hold the strings */
    if (a->delay > 0) sleep((unsigned)a->delay);
    static const char *const walk_name[1] = {"totk-walk"};
    int32_t walk = 0; int waited = 0;
    while (!tids_named(walk_name, &walk, 1) && waited < 120) { sleep(1); waited++; }
    if (!walk) fprintf(stderr, "[ohos-opengtx] no totk-walk thread after %d s: reporting without it\n", waited);
    void *lib = dlopen("libopengtx.so", RTLD_NOW | RTLD_LOCAL);
    if (!lib) { fprintf(stderr, "[ohos-opengtx] libopengtx.so: %s\n", dlerror()); return NULL; }
    void *(*create)(void (*)(int)) = (void *(*)(void (*)(int)))dlsym(lib, "HMS_OpenGTX_CreateContext");
    int (*config)(void *, const gtx_config *) = (int (*)(void *, const gtx_config *))dlsym(lib, "HMS_OpenGTX_SetConfiguration");
    int (*activate)(void *) = (int (*)(void *))dlsym(lib, "HMS_OpenGTX_Activate");
    int (*scene)(void *, const gtx_scene *) = (int (*)(void *, const gtx_scene *))dlsym(lib, "HMS_OpenGTX_DispatchGameSceneInfo");
    if (!create || !config || !activate || !scene) { fprintf(stderr, "[ohos-opengtx] a symbol is missing\n"); return NULL; }
    void *ctx = create(gtx_temp);
    if (!ctx) { fprintf(stderr, "[ohos-opengtx] CreateContext: NULL\n"); return NULL; }
    /* main = g-main (the game's main thread), render = totk-walk (replays the command stream); key threads: the two
       recorder workers, the submitter, the presenter and the driver's gpu-work-server, in that order, missing ones left out */
    static const char *const names[7] = {"g-main", "totk-walk", "totk-recw0", "totk-recw1", "totk-submit", "totk-present",
                                         "gpu-work-server"};
    int32_t tid[7]; tids_named(names, tid, 7);
    int nwalk = tids_count("totk-walk");
    gtx_config c; memset(&c, 0, sizeof c);
    c.mode = 0x0001; c.targetFPS = a->frame_rate; c.packageName = a->bundle; c.appVersion = a->version;
    c.engineType = 100; c.engineVersion = (char *)"1.0.0"; c.gameType = 2; c.pictureQualityMaxLevel = 3;   /* OTHERS_ENGINE, RPG, FHD */
    c.resolutionMaxValue = (gtx_res){1080, 1920};
    c.gameMainThreadId = tid[0] ? tid[0] : tid[1]; c.gameRenderThreadId = tid[1];
    for (int i = 2, k = 0; i < 7; i++) if (tid[i]) c.gameKeyThreadIds[k++] = tid[i];
    c.vulkanSupport = true;
    int rc = config(ctx, &c), ra = activate(ctx);
    /* HEAVY_LOAD; recommend / min / max fps: 0 (not in effect) or 30 .. targetFPS (oot-a112: a min of 20 was refused, 401) */
    int fps = a->frame_rate >= 30 ? a->frame_rate : 30;
    gtx_scene s = {7, (char *)"open world", fps, 0, fps, {1080, 1920}};
    int rs = scene(ctx, &s);
    fprintf(stderr, "[ohos-opengtx] SetConfiguration %d, Activate %d, DispatchGameSceneInfo (HEAVY_LOAD, %d fps, after %d s, "
                    "%d totk-walk threads, main %d, render %d, key threads totk-recw0=%d totk-recw1=%d totk-submit=%d "
                    "totk-present=%d gpu-work-server=%d) %d\n",
            rc, ra, a->frame_rate, (a->delay > 0 ? a->delay : 0) + waited, nwalk, (int)c.gameMainThreadId,
            (int)c.gameRenderThreadId, (int)tid[2], (int)tid[3], (int)tid[4], (int)tid[5], (int)tid[6], rs);
    return NULL;
}
static void opengtx_start(const char *bundle, const char *version, int frame_rate) {
    if (!env_on("TOTK_OHOS_OPENGTX")) { fprintf(stderr, "[ohos-opengtx] off (TOTK_OHOS_OPENGTX=1 turns it on)\n"); return; }
    const char *ds = getenv("TOTK_OHOS_OPENGTX_DELAY");
    gtx_args *a = calloc(1, sizeof *a);
    if (!a || !(a->bundle = strdup(bundle)) || !(a->version = strdup(version))) { fprintf(stderr, "[ohos-opengtx] out of memory\n"); return; }
    a->frame_rate = frame_rate; a->delay = ds && *ds ? atoi(ds) : 30;
    fprintf(stderr, "[ohos-opengtx] on: HEAVY_LOAD after %d s and the first totk-walk thread (TOTK_OHOS_OPENGTX=1)\n", a->delay > 0 ? a->delay : 0);
    pthread_t t;
    if (pthread_create(&t, NULL, opengtx_thread, a)) { fprintf(stderr, "[ohos-opengtx] cannot start its thread\n"); return; }
    pthread_detach(t);
}

/* Device state (TOTK_OHOS_DEVSTATE=1; off by default: these native queries had not run on any port): every 5 s the
   system's view of the phone, one line in the wording of the Ocarina of Time port's Game.ets hilog line (its parsers
   read both): temperature control level 1 - 6, GPU load levels 1 - 10 by stage, GPU frequency in kHz, and the time
   margin (s) and trend (-100 .. 100) of the temperature towards the level above the current one. Runs on the caller's
   thread after a successful HMS_GamePerformance_Init; a missing symbol ends it at once, 5 failed polls in a row end it. */
static void devstate_loop(void) {
    static const char *const gpu_names[6] = {"HMS_GamePerformance_GpuInfo_GetGpuLoadLevel", "HMS_GamePerformance_GpuInfo_GetVertexLoadLevel",
        "HMS_GamePerformance_GpuInfo_GetFragmentLoadLevel", "HMS_GamePerformance_GpuInfo_GetTextureLoadLevel",
        "HMS_GamePerformance_GpuInfo_GetBandwidthLoadLevel", "HMS_GamePerformance_GpuInfo_GetCurrentFrequency"};
    static const char *const thermal_names[3] = {"HMS_GamePerformance_ThermalInfo_GetThermalLevel",
        "HMS_GamePerformance_ThermalInfo_GetThermalMargin", "HMS_GamePerformance_ThermalInfo_GetThermalTrend"};
    fn_get gpu_get[6], thermal_get[3];
    fn_create query_gpu = (fn_create)sym("HMS_GamePerformance_QueryGpuInfo");
    fn_create destroy_gpu = (fn_create)sym("HMS_GamePerformance_DestroyGpuInfo");
    fn_create create_params = (fn_create)sym("HMS_GamePerformance_CreateThermalInfoQueryParameters");
    fn_create destroy_params = (fn_create)sym("HMS_GamePerformance_DestroyThermalInfoQueryParameters");
    fn_bool set_prediction = (fn_bool)sym("HMS_GamePerformance_ThermalInfoQueryParameters_SetNeedsPrediction");
    fn_int set_target = (fn_int)sym("HMS_GamePerformance_ThermalInfoQueryParameters_SetTargetThermalLevel");
    fn_query query_thermal = (fn_query)sym("HMS_GamePerformance_QueryThermalInfo");
    fn_create destroy_thermal = (fn_create)sym("HMS_GamePerformance_DestroyThermalInfo");
    int missing = !query_gpu || !destroy_gpu || !create_params || !destroy_params || !set_prediction || !set_target ||
                  !query_thermal || !destroy_thermal;
    for (int i = 0; i < 6; i++) missing |= !(gpu_get[i] = (fn_get)sym(gpu_names[i]));
    for (int i = 0; i < 3; i++) missing |= !(thermal_get[i] = (fn_get)sym(thermal_names[i]));
    if (missing) { fprintf(stderr, "[ohos-gameperf] device state off: a query symbol is missing\n"); return; }
    void *params = NULL; int r = create_params(&params);
    if (r || !params) { fprintf(stderr, "[ohos-gameperf] device state off: CreateThermalInfoQueryParameters -> %d\n", r); return; }
    int target = 2;   /* the margin is the time to this level: the one above the current level, from the first answer on */
    int predict = !set_prediction(params, true) && !set_target(params, target);
    if (!predict) set_prediction(params, false);   /* no target level: the level alone, as the line then says */
    fprintf(stderr, "[ohos-gameperf] device state every 5 s (TOTK_OHOS_DEVSTATE=1), thermal prediction %s\n", predict ? "on" : "off");
    for (int fails = 0, predicted = 0;; sleep(5)) {
        int32_t g[6] = {-1, -1, -1, -1, -1, -1}, t[3] = {-1, -9, -999};   /* -1 / -9 / -999: not answered (as OoT) */
        void *gi = NULL, *ti = NULL;
        int rg = query_gpu(&gi), used = target, rt = query_thermal(params, &ti);
        int got_g = !rg && gi, got_t = !rt && ti;
        if (got_g) { for (int i = 0; i < 6; i++) gpu_get[i](gi, &g[i]); destroy_gpu(&gi); }
        if (got_t) { for (int i = 0; i < 3; i++) thermal_get[i](ti, &t[i]); destroy_thermal(&ti); predicted |= predict; }
        if (!got_t && predict && !predicted) {   /* never answered with the prediction: ask for the level alone from now on */
            predict = 0; set_prediction(params, false);
            fprintf(stderr, "[ohos-gameperf] device state: QueryThermalInfo -> %d with prediction, prediction off\n", rt);
        }
        if (!got_g && !got_t) {
            fprintf(stderr, "[ohos-gameperf] device state: QueryGpuInfo %d, QueryThermalInfo %d (%d failed polls in a row)\n", rg, rt, ++fails);
            if (fails < 5) continue;
            fprintf(stderr, "[ohos-gameperf] device state off after 5 failed polls in a row\n");
            break;
        }
        fails = 0;
        fprintf(stderr, "[ohos-gameperf] device state: temp level %d, GPU load %d (vertex %d, fragment %d, texture %d, bandwidth %d), "
                        "GPU %d kHz, thermal margin %d s, trend %d (margin to level %d)\n",
                (int)t[0], (int)g[0], (int)g[1], (int)g[2], (int)g[3], (int)g[4], (int)g[5], (int)t[1], (int)t[2], predict ? used : -1);
        /* the target only moves when the parameters took it, so 'margin to level' names the level the query used */
        if (predict && t[0] >= 1 && t[0] < 6 && t[0] + 1 != target && !set_target(params, t[0] + 1)) target = t[0] + 1;
    }
    destroy_params(&params);
}

/* 1 when HMS_GamePerformance_Init succeeded (the device-state queries need it) */
static int ohos_gameperf_scene(const char *bundle, const char *version, int frame_rate);
/* Called once the game runs (frontend start), on its own thread; frame_rate: the rate the game aims for (30 handheld).
   With TOTK_OHOS_DEVSTATE=1 it does not return: that thread then polls the device state. */
void ohos_gameperf_start(const char *bundle, const char *version, int frame_rate) {
    int devstate = env_on("TOTK_OHOS_DEVSTATE");
    int inited = ohos_gameperf_scene(bundle, version, frame_rate);
    opengtx_start(bundle, version, frame_rate);
    if (!devstate) fprintf(stderr, "[ohos-gameperf] device state off (TOTK_OHOS_DEVSTATE=1 turns it on)\n");
    else if (!inited) fprintf(stderr, "[ohos-gameperf] device state off: HMS_GamePerformance_Init did not succeed\n");
    else devstate_loop();
}
static int ohos_gameperf_scene(const char *bundle, const char *version, int frame_rate) {
    const char *e = getenv("TOTK_OHOS_GAMEPERF");
    if (e && !strcmp(e, "0")) { fprintf(stderr, "[ohos-gameperf] off (TOTK_OHOS_GAMEPERF=0)\n"); return 0; }
    g_lib = dlopen("libgame_performance.z.so", RTLD_NOW | RTLD_LOCAL);
    if (!g_lib) { fprintf(stderr, "[ohos-gameperf] libgame_performance.z.so: %s\n", dlerror()); return 0; }
    void *init = NULL;
    if (CALL(fn_create, "CreateInitParameters", &init) || !init) return 0;
    CALL(fn_str, "InitParameters_SetBundleName", init, bundle);
    CALL(fn_str, "InitParameters_SetAppVersion", init, version);
    int r = CALL(fn_obj, "Init", init);
    fprintf(stderr, "[ohos-gameperf] Init: %d%s\n", r, r == 1010300002 ? " (authentication failed)" : "");
    if (r) return 0;
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
    if (CALL(fn_create, "CreateSceneInfo", &g_scene) || !g_scene) return 1;
    /* game_performance.h SceneInfo_SetSceneID: 1 game startup, 5 game loading, 7 gaming. TOTK_OHOS_SCENE_ID, default 7
       (the Ocarina of Time port reports 7 on this phone and it is accepted); 1 is what this port reported until 2026-11-14. */
    const char *si = getenv("TOTK_OHOS_SCENE_ID");
    int64_t scene_id = si && *si ? strtoll(si, NULL, 10) : 7;
    CALL(fn_i64, "SceneInfo_SetSceneID", g_scene, scene_id);
    CALL(fn_str, "SceneInfo_SetDescription", g_scene, "open world");
    CALL(fn_int, "SceneInfo_SetImportanceLevel", g_scene, 5);
    CALL(fn_int, "SceneInfo_SetRecommendedCpuLevel", g_scene, 3);    /* HIGH */
    CALL(fn_int, "SceneInfo_SetRecommendedGpuLevel", g_scene, 3);
    CALL(fn_int, "SceneInfo_SetRecommendedDdrLevel", g_scene, 3);
    CALL(fn_i64, "SceneInfo_SetMaxFrameRate", g_scene, frame_rate);
    CALL(fn_i64, "SceneInfo_SetCurrentFrameRate", g_scene, frame_rate);
    /* game_performance.h: "render|xxx|logic|xxx" (role, thread name), at most 32 characters; a plain list of thread
       names was refused with 401 (PARAM_INVALID). The recorder workers and audio have no role in that format. */
    CALL(fn_str, "SceneInfo_SetKeyThread", g_scene, "render|totk-walk|logic|g-main");   /* this port's hot threads (thread_place.h, vk_record.c) */
    fprintf(stderr, "[ohos-gameperf] UpdateSceneInfo (scene %lld, importance 5, CPU/GPU/DDR high, %d fps, key threads render=totk-walk logic=g-main): %d\n",
            (long long)scene_id, frame_rate, CALL(fn_obj, "UpdateSceneInfo", g_scene));
    return 1;
}
