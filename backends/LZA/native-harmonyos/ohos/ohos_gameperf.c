/* HarmonyOS GameServiceKit scene hints (libgame_performance.z.so, API 14): tell the system's game scheduler that this
   is a heavy Vulkan RPG scene at importance 5 with recommended CPU / GPU / DDR level HIGH, the frame rate it aims for,
   and its key threads (render = totk-walk, logic = g-main), so CPU and GPU frequencies are held instead of following
   the bursty load. Loaded with dlopen: a missing library or an authentication failure (self-signed builds may not
   pass HMS_GamePerformance_Init) only logs. TOTK_OHOS_GAMEPERF=0 skips it. Signatures as in
   hms/native/sysroot/usr/include/GameServiceKit/game_performance.h (opaque handles, enums as int). */
#include <dlfcn.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
/* Called once the game runs (frontend start); frame_rate: the rate the game aims for (30 handheld). */
void ohos_gameperf_start(const char *bundle, const char *version, int frame_rate) {
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
    CALL(fn_i64, "SceneInfo_SetSceneID", g_scene, 1);
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
    fprintf(stderr, "[ohos-gameperf] UpdateSceneInfo (importance 5, CPU/GPU/DDR high, %d fps, key threads render=totk-walk logic=g-main): %d\n",
            frame_rate, CALL(fn_obj, "UpdateSceneInfo", g_scene));
}
