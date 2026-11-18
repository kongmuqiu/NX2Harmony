/* HarmonyOS entry of the Android-stack runtime (libtotk_ohos.so). ArkTS loads the library ("import totk from
   'libtotk_ohos.so'") and calls totk.start(filesDir). That sets the game paths the runtime reads from the environment,
   sends stdout / stderr to <filesDir>/android/totk-native.log and runs the runtime's main (renamed totk_host_main,
   -Dmain=totk_host_main as the Android app build does) on a thread of its own:
     argv = totk-native <unused rewritten dir> boot
   The modules come from the signed libtotk_guest.so (runtime/loader.c, TOTK_OHOS); game data (runtime-tables.bin,
   romfs-config.txt, shared-fonts.txt), the RomFS image and its index sit in <filesDir>/game (game-v143 for 1.4.3, below),
   the shared fonts in its fonts/ folder. <filesDir>/android/env.txt may hold more KEY=VALUE lines for the runtime (diagnostics).
   Until the window frontend exists the runtime uses runtime/frontend_headless.c: no picture, no input. */
#include <napi/native_api.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <dlfcn.h>
#include <link.h>
#include <bundle/native_interface_bundle.h>
#include "runtime.h"   /* RuntimeTextRequest */
extern int totk_host_main(int argc, char **argv);

/* The game version this library runs (build_ohos.py --version): its game data folder (<filesDir>/game for 1.0.0,
   <filesDir>/game-v143 for 1.4.3, so both can stay on the device) and shader seed folder (android/shaders/<version>,
   android/gfx/vk_shader.c). The shared fonts are the firmware's, in that game data folder's fonts/ (totk-ohos, 2026-11-15). */
#if defined(TOTK_GAME_V143)
#define GAME_DIR "v143"
#define GAME_DATA_DIR "game-v143"
#else
#define GAME_DIR "v0"
#define GAME_DATA_DIR "game"
#endif

static atomic_int started;
static char work_dir[1024];

/* Each loaded module's TLS block on this thread, relative to the thread pointer (musl fills dlpi_tls_data for the
   calling thread). */
static int log_tls_block(struct dl_phdr_info *info, size_t size, void *who) {
    (void)size;
    for (int i = 0; i < info->dlpi_phnum; i++) {
        if (info->dlpi_phdr[i].p_type != PT_TLS) continue;
        uintptr_t tp; __asm__ volatile("mrs %0, tpidr_el0" : "=r"(tp));
        intptr_t rel = info->dlpi_tls_data ? (intptr_t)((uintptr_t)info->dlpi_tls_data - tp) : 0;
        fprintf(stderr, "[totk-ohos] %s TLS module %zu %s: tp%+ld size %#lx align %#lx%s\n", (const char *)who,
                (size_t)info->dlpi_tls_modid, info->dlpi_name && *info->dlpi_name ? info->dlpi_name : "(main)", (long)rel,
                (unsigned long)info->dlpi_phdr[i].p_memsz, (unsigned long)info->dlpi_phdr[i].p_align,
                info->dlpi_tls_data ? "" : " (not allocated on this thread)");
    }
    return 0;
}

/* What sits around the thread pointer on this thread: the slot-block pointer must go into a word nothing else uses. */
static void log_tp_words(const char *who) {
    uintptr_t tp; __asm__ volatile("mrs %0, tpidr_el0" : "=r"(tp));
    for (int off = -16; off <= 24; off += 8) {
        uintptr_t v = *(volatile uintptr_t *)(tp + off);
        Dl_info di = {0};
        int known = v > 0x10000 && dladdr((void *)v, &di) && di.dli_fname;
        fprintf(stderr, "[totk-ohos] %s tp%+d = %#lx%s%s%s%s\n", who, off, (unsigned long)v, known ? " in " : "",
                known ? di.dli_fname : "", known && di.dli_sname ? " " : "", known && di.dli_sname ? di.dli_sname : "");
    }
    dl_iterate_phdr(log_tls_block, (void *)who);
}

/* What this kernel allows of the reference's page moves (android/gfx/nvn_vk.c unback_pool: mremap of fresh shared
   pages onto a pool; totk_vpool.c: mremap(old_size 0) aliases of shared anonymous memory). */
#include <sys/mman.h>
#include <sys/syscall.h>
static void probe_page_moves(void) {
    const size_t n = 2 * 4096;
    uint8_t *a = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    uint8_t *b = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    uint8_t *c = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    uint8_t *d = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    a[0] = 0x5a;
    void *r1 = mremap(a, 0, n, MREMAP_MAYMOVE | MREMAP_FIXED, b); int e1 = errno;
    int alias = r1 == b && b[0] == 0x5a && (a[1] = 7, b[1] == 7);
    c[0] = 0x33;
    void *r2 = mremap(c, n, n, MREMAP_MAYMOVE | MREMAP_FIXED, d); int e2 = errno;
    uint8_t *e = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0), *f = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    e[0] = 0x44;
    void *r3 = mremap(e, n, n, MREMAP_MAYMOVE, NULL); int e3 = errno;
    void *r4 = mremap(f, n, n, MREMAP_MAYMOVE | MREMAP_FIXED, r3 == MAP_FAILED ? (void *)e : r3); int e4 = errno;
    long mfd = syscall(SYS_memfd_create, "totk-probe", 0); int e5 = errno;
    fprintf(stderr, "[totk-ohos] page moves: alias(old 0) %s errno %d visible %d; move onto shared %s errno %d (data %s); "
                    "move anywhere %s errno %d; move onto moved %s errno %d; memfd %ld errno %d\n",
            r1 == MAP_FAILED ? "FAIL" : "ok", r1 == MAP_FAILED ? e1 : 0, alias, r2 == MAP_FAILED ? "FAIL" : "ok", r2 == MAP_FAILED ? e2 : 0,
            r2 != MAP_FAILED && ((uint8_t *)r2)[0] == 0x33 ? "kept" : "-", r3 == MAP_FAILED ? "FAIL" : "ok", r3 == MAP_FAILED ? e3 : 0,
            r4 == MAP_FAILED ? "FAIL" : "ok", r4 == MAP_FAILED ? e4 : 0, mfd, mfd < 0 ? e5 : 0);
    if (mfd >= 0) close((int)mfd);
}

void ohos_gameperf_start(const char *bundle, const char *version, int frame_rate);   /* ohos_gameperf.c */
static void *gameperf_thread(void *arg) {
    (void)arg; const char *c = getenv("TOTK_OHOS_FPS_CAP");
    OH_NativeBundle_ApplicationInfo self = OH_NativeBundle_GetCurrentApplicationInfo();   /* the installed name */
    ohos_gameperf_start(self.bundleName ? self.bundleName : "TotZ.NativeGames.harmonyos", "1.0.0", c && *c ? atoi(c) : 30);
    free(self.bundleName); free(self.fingerprint); return NULL; }
static void *game_thread(void *arg) {
    (void)arg;
    log_tp_words("runtime thread");
    probe_page_moves();
    {   pthread_t g; if (!pthread_create(&g, NULL, gameperf_thread, NULL)) pthread_detach(g); }   /* IPC: off the game's path */
    {   void ohos_sched_start(void); ohos_sched_start(); }   /* ohos_sched.c: critical threads to the fastest allowed cores */
    char *argv[] = {"totk-native", work_dir, "boot", NULL};
    int rc = totk_host_main(3, argv);
    fprintf(stderr, "[totk-ohos] totk_host_main returned %d\n", rc);
    return NULL;
}

/* KEY=VALUE lines; "KEY=" (nothing after the '=') clears KEY: the defaults below are set with setenv(..., 0), which an
   empty value would block (TOTK_VK_REC_THREADS= would read as 0 and turn the recorder workers off). */
static void load_env_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *eq = strchr(line, '=');
        if (!line[0] || line[0] == '#' || !eq) continue;
        *eq = 0;
        if (!eq[1]) { unsetenv(line); fprintf(stderr, "[totk-ohos] env %s cleared (empty value)\n", line); continue; }
        setenv(line, eq + 1, 1);
        fprintf(stderr, "[totk-ohos] env %s=%s\n", line, eq + 1);
    }
    fclose(f);
}

#include <dirent.h>
static int env_name_cmp(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }
/* After env.txt: <dir>/env-dev.txt, then every <dir>/env-dev-<anything>.txt in name order (a later one wins). Diagnostics
   sent with hdc; the settings page rewrites env.txt, never these. hdc rewriting a file the app has read before is not
   seen by the app at once (its sandbox view keeps the old content), so each set goes in a new file: the list has no
   limit (the Ocarina of Time port kept the first 64 readdir returned until oot-a106 and could drop the newest). */
static void load_env_dev_files(const char *dir) {
    char path[1400];
    snprintf(path, sizeof path, "%s/env-dev.txt", dir); load_env_file(path);
    char **names = NULL; size_t n = 0, cap = 0;
    DIR *d = opendir(dir);
    if (d) {
        struct dirent *de;
        while ((de = readdir(d))) {
            size_t len = strlen(de->d_name);
            if (len < 12 || strncmp(de->d_name, "env-dev-", 8) || strcmp(de->d_name + len - 4, ".txt")) continue;
            if (n == cap) {
                size_t c2 = cap ? 2 * cap : 64; char **g = realloc(names, sizeof *names * c2);
                if (!g) break;
                names = g; cap = c2;
            }
            if (!(names[n] = strdup(de->d_name))) break;
            n++;
        }
        closedir(d);
    }
    if (n > 1) qsort(names, n, sizeof *names, env_name_cmp);
    for (size_t i = 0; i < n; i++) { snprintf(path, sizeof path, "%s/%s", dir, names[i]); load_env_file(path); free(names[i]); }
    free(names);
    fprintf(stderr, "[totk-ohos] %zu env-dev-*.txt files read in name order (a later one wins)\n", n);
}

/* A RomFS overlay folder and its subfolders, from its manifest: hdc cannot create folders in the app's sandbox. */
static void overlay_dirs(const char *dir) {
    mkdir(dir, 0770);
    char m[1200]; snprintf(m, sizeof m, "%s/manifest.tsv", dir);
    FILE *f = fopen(m, "r"); char line[4096];
    while (f && fgets(line, sizeof line, f)) {
        char *tab = strchr(line, '\t');
        if (line[0] == '#' || !tab || strstr(line, "..")) continue;
        *tab = 0;
        char d[5200]; size_t base = strlen(dir);
        if (snprintf(d, sizeof d, "%s/%s", dir, line) >= (int)sizeof d) continue;
        for (char *p = d + base + 1; *p; p++) if (*p == '/') { *p = 0; mkdir(d, 0770); *p = '/'; }
    }
    if (f) fclose(f);
}

/* totk-ohos: the HUD on a screen wider than 16:9. With TOTK_ASPECT the camera draws the wider view into the game's
   16:9 canvas and the presenter stretches the canvas over the screen, so the game's 16:9 UI layouts would stretch too.
   Two ways to keep the HUD's shape: squeeze every HUD quad about the canvas centre (android/gfx/vk_ui_aspect.h,
   TOTK_UI_SQUEEZE=1, the default: the HUD stays in the middle 16:9, and full-screen panels such as the loading screen's
   and the cutscene fades fall short of the edges), or layouts made for the screen (scripts/ultrawide_layout.py through
   make_romfs_patches.py --version v143 --aspect <W>x<H> --hud corner: every RootPane narrowed to 16:9 except the
   full-screen ones, backgrounds widened back, the HUD parts moved out to the corners). Those come as a RomFS overlay of
   their own, extracted/<version>/romfs-patches-<W>x<H> beside the base overlay (its files plus the layouts), and need
   the squeeze off or the HUD is narrowed twice. TOTK_HUD=center keeps the squeeze.
   TOTK_ASPECT as runtime/aspect_patch.h reads it: the ratio (0 when unset, "0" or not a ratio between 0.5 and 8; the
   runtime refuses to start with a malformed value) and, for <w>:<h> or <w>x<h> in whole pixels, the sides. */
static double hud_aspect(const char *e, unsigned *w, unsigned *h) {
    *w = *h = 0;
    if (!e || !*e || !strcmp(e, "0")) return 0;
    double a = 0, x, y; char rest;
    if ((sscanf(e, "%lf:%lf%c", &x, &y, &rest) == 2 || sscanf(e, "%lfx%lf%c", &x, &y, &rest) == 2) && x > 0 && y > 0) {
        a = x / y;
        if (x < 65536 && y < 65536 && x == (double)(unsigned)x && y == (double)(unsigned)y) { *w = (unsigned)x; *h = (unsigned)y; }
    } else if (sscanf(e, "%lf%c", &x, &rest) == 1) a = x;
    return a >= 0.5 && a <= 8 ? a : 0;
}

/* totk-ohos: 1 when <root>/<rel>/manifest.tsv can be read, lists a file and every file it lists is in <root>/<rel> at
   its listed size; else 0 and why. The manifest goes to the phone first (nx2h, push_v143.ps1), so a push cut short
   leaves it without its files, and runtime/romfs.c stops the game on a missing or short one: such an overlay is not
   picked (the HUD keeps the squeeze). The lines are read as runtime/romfs.c reads them (path, size, two SHA-256s); the
   hashes are left to the runtime. */
static int overlay_whole(const char *root, const char *rel, char *why, size_t whyn) {
    char m[1600], file[4400], line[8192]; int n = 0; struct stat st;
    snprintf(m, sizeof m, "%s/%s/manifest.tsv", root, rel);
    FILE *f = fopen(m, "r");
    if (!f) { snprintf(why, whyn, "no %s/manifest.tsv", rel); return 0; }
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        char *tab = strchr(line, '\t'), *end = NULL; unsigned long long size = tab ? strtoull(tab + 1, &end, 10) : 0;
        if (!tab || tab == line || end == tab + 1 || *end != '\t') { snprintf(why, whyn, "%s/manifest.tsv has a malformed line", rel); fclose(f); return 0; }
        *tab = 0;
        if (snprintf(file, sizeof file, "%s/%s/%s", root, rel, line) >= (int)sizeof file || stat(file, &st) || !S_ISREG(st.st_mode) ||
            (unsigned long long)st.st_size != size) {
            snprintf(why, whyn, "%s/manifest.tsv lists %s (%llu bytes), not in that folder at that size", rel, line, size); fclose(f); return 0; }
        n++;
    }
    fclose(f);
    if (!n) { snprintf(why, whyn, "%s/manifest.tsv lists no file", rel); return 0; }
    return 1;
}

/* totk-ohos: the overlay with the edge layouts for TOTK_ASPECT (aspect), TOTK_HUD (hud) and TOTK_ROMFS_PATCHES
   (preset; a value already set wins). 1: dir = <ver>/romfs-patches-<w>x<h>, relative to root (the working folder, as
   the runtime's default overlay path is), and its sides in *sw, *sh. 0: why = the reason the HUD stays in the middle
   16:9. Only a ratio more than 1% wider than 16:9 picks one: the screen's own folder (<w>:<h> exactly) first, else the
   romfs-patches-<w>x<h> (w > h) closest in ratio within 0.5%; either only when it is whole (overlay_whole). */
static int hud_overlay_pick(const char *root, const char *ver, const char *aspect, const char *hud, const char *preset,
                            char *dir, size_t dirn, unsigned *sw, unsigned *sh, char *why, size_t whyn) {
    unsigned w, h; double a = hud_aspect(aspect, &w, &h);
    char p[1600], miss[600] = "", near[600] = "", r[600];   /* why the screen's own folder / the closest other one is not used */
    *sw = *sh = 0; if (dirn) dir[0] = 0;
    if (!a) {
        if (!aspect || !*aspect || !strcmp(aspect, "0")) snprintf(why, whyn, "TOTK_ASPECT=%s, the game's 16:9", aspect && *aspect ? aspect : "(unset)");
        else snprintf(why, whyn, "TOTK_ASPECT=%s is not a ratio between 0.5 and 8", aspect);
        return 0;
    }
    if (!(a > 16.0 / 9 * 1.01)) { snprintf(why, whyn, "TOTK_ASPECT=%s (%.4f) is not wider than 16:9 by more than 1%%", aspect, a); return 0; }
    if (preset && *preset) { snprintf(why, whyn, "TOTK_ROMFS_PATCHES=%s is set", preset); return 0; }
    if (hud && !strcmp(hud, "center")) { snprintf(why, whyn, "TOTK_HUD=center"); return 0; }
    if (w && h) {
        snprintf(p, sizeof p, "%s/romfs-patches-%ux%u", ver, w, h);
        if (overlay_whole(root, p, miss, sizeof miss)) { snprintf(dir, dirn, "%s", p); *sw = w; *sh = h; return 1; }
    }
    double best = 0.005; unsigned bw = 0, bh = 0;
    snprintf(p, sizeof p, "%s/%s", root, ver);
    DIR *d = opendir(p);
    if (d) {
        struct dirent *de;
        while ((de = readdir(d))) {
            unsigned x, y; char canon[64];
            if (sscanf(de->d_name, "romfs-patches-%ux%u", &x, &y) != 2 || !y || x <= y || (x == w && y == h)) continue;
            snprintf(canon, sizeof canon, "romfs-patches-%ux%u", x, y);
            if (strcmp(canon, de->d_name)) continue;   /* exactly romfs-patches-<w>x<h> */
            double q = (double)x / y, off = (q > a ? q - a : a - q) / a;
            if (off > best || (off == best && bw && x <= bw)) continue;   /* a tie: the bigger screen */
            snprintf(p, sizeof p, "%s/%s", ver, de->d_name);
            if (!overlay_whole(root, p, r, sizeof r)) { if (!near[0]) snprintf(near, sizeof near, "%s", r); continue; }
            best = off; bw = x; bh = y;
        }
        closedir(d);
    }
    if (bw) { snprintf(dir, dirn, "%s/romfs-patches-%ux%u", ver, bw, bh); *sw = bw; *sh = bh; return 1; }
    snprintf(r, sizeof r, "no %s %s/romfs-patches-<w>x<h> within 0.5%% of %.4f", w && h ? "other whole" : "whole", ver, a);
    snprintf(why, whyn, "%s%s%s", miss, miss[0] ? "; " : "", near[0] ? near : r);
    return 0;
}

__attribute__((weak)) void ohos_gamepad_start(void);
__attribute__((weak)) void ohos_feedback_start(void);

/* 0 when the runtime thread was started, else a message. */
__attribute__((visibility("default"))) const char *totk_ohos_start(const char *files_dir) {
    if (atomic_exchange(&started, 1)) return "already started";
    char path[1200];
    if (snprintf(work_dir, sizeof work_dir, "%s/android", files_dir) >= (int)sizeof work_dir) return "files path too long";
    if (mkdir(work_dir, 0770) && errno != EEXIST) return "cannot create the android work folder";
    snprintf(path, sizeof path, "%s/totk-native.log", work_dir);
    int log = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0660);
    if (log < 0) return "cannot open totk-native.log";
    dup2(log, 1); dup2(log, 2); close(log);
    setvbuf(stdout, NULL, _IOLBF, 0); setvbuf(stderr, NULL, _IONBF, 0);
    snprintf(path, sizeof path, "%s/" GAME_DATA_DIR, files_dir); setenv("TOTK_GAME_DATA", path, 1); setenv("TOTK_ROMFS", path, 1);
    mkdir(path, 0770);   /* so hdc can send the game data into it */
    /* the RomFS overlay files (runtime/romfs.c: extracted/<version>/romfs-patches under the working folder) */
    snprintf(path, sizeof path, "%s/extracted", work_dir); mkdir(path, 0770);
    snprintf(path, sizeof path, "%s/extracted/" GAME_DIR, work_dir); mkdir(path, 0770);
    snprintf(path, sizeof path, "%s/extracted/" GAME_DIR "/romfs-patches", work_dir); overlay_dirs(path);
    /* totk-ohos: the shared fonts in <game data>/fonts (as the Z-A port's game-za/fonts), created here because hdc cannot
       make folders in the sandbox. runtime/shared_font.c opens all six the first time the game asks, which Chinese or
       Korean text does and English does not, and a missing file stops the game there ("open shared font"). Until
       2026-11-15 the path was <filesDir>/game/fonts, which nothing created or filled: choosing Chinese crashed at the title. */
    snprintf(path, sizeof path, "%s/" GAME_DATA_DIR "/fonts", files_dir); mkdir(path, 0770); setenv("TOTK_SHARED_FONTS", path, 1);
    {   int n = 0; DIR *fd = opendir(path); struct dirent *de;
        while (fd && (de = readdir(fd))) { size_t l = strlen(de->d_name); if (l > 6 && !strcmp(de->d_name + l - 6, ".bfttf")) n++; }
        if (fd) closedir(fd);
        fprintf(stderr, "[totk-ohos] shared fonts: %d .bfttf files in %s%s\n", n, path,
                n < 6 ? " (Chinese / Korean text needs all 6 and stops the game without them; nx2h pushes them)" : ""); }
    snprintf(path, sizeof path, "%s/slot", work_dir); mkdir(path, 0770); setenv("TOTK_GAME_SLOT_DIR", path, 1);   /* runtime/game_slot.c */
    /* the Vulkan shader seed (android/gfx/vk_shader.c: shaders/<version>/seed.vkpack): created here so hdc can send into it */
    snprintf(path, sizeof path, "%s/shaders", work_dir); mkdir(path, 0770);
    snprintf(path, sizeof path, "%s/shaders/" GAME_DIR, work_dir); mkdir(path, 0770);
    /* saves: TOTK_PROFILE_DIR defaults to user-data in the working folder (runtime/fs_host.c) */
    /* shader variants the seed lacks are recorded here for native-harmonyos/fill_shaders_ohos.py (android/gfx/vk_shader.c); absolute,
       so a chdir elsewhere in the runtime cannot move them */
    snprintf(path, sizeof path, "%s/shader-requests", work_dir); mkdir(path, 0770); setenv("TOTK_SHADER_REQ_DIR", path, 1);
    snprintf(path, sizeof path, "%s/env.txt", work_dir); load_env_file(path);
    load_env_dev_files(work_dir);   /* env-dev.txt and env-dev-*.txt: hdc diagnostics, after the settings page's lines */
    /* The Android app's defaults (runtime/frontend_android.c start_game), after env.txt so its lines win. The RomFS
       verification record stays next to the image (the default), where the first start wrote it. */
    snprintf(path, sizeof path, "%s/state", work_dir); mkdir(path, 0770);
    snprintf(path, sizeof path, "%s/user-data", work_dir); setenv("TOTK_PROFILE_DIR", path, 0);
    snprintf(path, sizeof path, "%s/state/save-generation", work_dir); setenv("TOTK_SAVE_GENERATION", path, 0);
    snprintf(path, sizeof path, "%s/state/save-busy", work_dir); setenv("TOTK_SAVE_BUSY", path, 0);
    unlink(getenv("TOTK_SAVE_BUSY"));   /* a previous process ended mid-save: nothing of it is in progress now */
    /* (1.4.3: android/gfx/vk_shader.c vk_shader_cache_path adds .v143 to this and the other learned state files) */
    snprintf(path, sizeof path, "%s/state/vk-pipeline-cache.bin", work_dir); setenv("TOTK_VK_PIPELINE_CACHE", path, 0);
    setenv("TOTK_OPERATION_MODE", "handheld", 0);
    setenv("TOTK_ROMFS_MMAP", "1", 0);
#if defined(TOTK_GAME_V143)
    /* The embedded render-size patch (game_patches_v143.c patch_render_res, at build like iOS) always takes the scene size
       from the data page: TOTK_RENDER_RES, else 1280x720, the handheld size. Docked gets the game's docked 1600x900. */
    {   const char *r = getenv("TOTK_RENDER_RES"), *mode = getenv("TOTK_OPERATION_MODE");
        if (!(r && *r && strcmp(r, "0")) && mode && !strcmp(mode, "docked")) setenv("TOTK_RENDER_RES", "1600x900", 1); }
#else
    /* 1.0.0 has no ultrawide camera patch (runtime/aspect_patch.h): a TOTK_ASPECT from the settings page would only squeeze
       the HUD (android/gfx/vk_ui_aspect.h) of a picture still shown at 16:9. */
    if (getenv("TOTK_ASPECT")) { fprintf(stderr, "[totk-ohos] TOTK_ASPECT=%s ignored: 1.0.0 has no wide camera\n", getenv("TOTK_ASPECT")); unsetenv("TOTK_ASPECT"); }
#endif
    {   /* totk-ohos: the HUD at the screen edges (the screen's layouts overlay, no squeeze) or in the middle 16:9 */
        char dir[256], why[512]; unsigned w, h, sw, sh;
        const char *aspect = getenv("TOTK_ASPECT");
        double a = hud_aspect(aspect, &w, &h);
        if (a > 16.0 / 9 * 1.01 && h >= 240) {   /* the screen's own overlay folder (pixel sides, not 21:9), so hdc can send into it */
            snprintf(path, sizeof path, "%s/extracted/" GAME_DIR "/romfs-patches-%ux%u", work_dir, w, h); overlay_dirs(path); }
        int edge = hud_overlay_pick(work_dir, "extracted/" GAME_DIR, aspect, getenv("TOTK_HUD"), getenv("TOTK_ROMFS_PATCHES"),
                                    dir, sizeof dir, &sw, &sh, why, sizeof why);
        if (edge) {   /* a value already set wins; an empty one counts as unset, as in hud_overlay_pick (load_env_file: "KEY=" clears) */
            const char *pp = getenv("TOTK_ROMFS_PATCHES"), *pq = getenv("TOTK_UI_SQUEEZE");
            setenv("TOTK_ROMFS_PATCHES", dir, !(pp && *pp)); setenv("TOTK_UI_SQUEEZE", "0", !(pq && *pq));   /* android/gfx/vk_draw.c, vk_post.c */
            snprintf(path, sizeof path, "%s/%s", work_dir, dir); overlay_dirs(path);
        }
        const char *sq = getenv("TOTK_UI_SQUEEZE"); int squeeze = !(sq && !strcmp(sq, "0"));
        if (edge) fprintf(stderr, "[totk-ohos] HUD at the screen edges: RomFS overlay %s (layouts for %ux%u), UI squeeze %s\n",
                          dir, sw, sh, squeeze ? "on (TOTK_UI_SQUEEZE is set: the HUD is narrowed twice)" : "off");
        else fprintf(stderr, "[totk-ohos] HUD in the middle 16:9: %s (%s)\n", why,   /* vk_ui_aspect.h: within 0.1% of 16:9 is 16:9 */
                     !a || (a * 9 / 16 < 1.001 && a * 9 / 16 > 0.999) ? "nothing to squeeze" : squeeze ? "UI squeeze on" : "UI squeeze off");
    }
    /* Frame-rate cap TOTK_OHOS_FPS_CAP = 30 / 60 (default 30, the Android app's): 60 turns the 60 FPS patch on (its real-time
       frame step keeps game speed right at any rate); frontend_ohos.c drives the guest vsync at the cap's rate. */
    {   const char *c = getenv("TOTK_OHOS_FPS_CAP"); int cap = c && *c ? atoi(c) : 30;   /* the Android app's default */
        if (cap == 90 || cap == 120) cap = 60;   /* totk-ohos: 90 / 120 left the settings page (2026-11-15) */
        if (cap != 30 && cap != 60) cap = 30;
        char v[8]; snprintf(v, sizeof v, "%d", cap); setenv("TOTK_OHOS_FPS_CAP", v, 1);
#if defined(TOTK_GAME_V143)   /* the 60 FPS patch is in the embedded code (prepare_guest.sh v143): the runtime checks it */
        setenv("TOTK_FPS60", "1", 1);
#else
        setenv("TOTK_FPS60", cap >= 60 ? "1" : "0", 0);
#endif
        const char *dh = getenv("TOTK_OHOS_DISPLAY_HZ"); int panel = dh && *dh ? atoi(dh) : 0;   /* frontend_ohos.c: the panel's rate */
        char ph[8]; snprintf(ph, sizeof ph, "%d", panel);
        /* One frame in flight (TOTK_OHOS_SYNC_LAG=0), cap 30: the backend present clock at 90 Hz (the game's interval 3 becomes a 33.3 ms period, interval 2 22.2 ms, so
           the guest's own 30 Hz vsync paces). Measured 2026-11-15 without a frequency lock, alternating pairs: +1.4/+1.4 fps
           with no env.txt, +1.8/+3.7 fps with a player's settings (native output, fill), frames over 50 ms 58-91 -> 21-23
           per 300. Not for cap 60 (interval 2 would run up to 45 fps on a 60 Hz panel). */
        /* totk-ohos, two frames in flight by default (2026-11-15): hle_nvn.c TOTK_OHOS_SYNC_LAG=1 moves the game's frame-end
           wait to the frame before the last, TOTK_OHOS_INTERVAL_MAX=2 keeps its frame controller off the 20 fps tier, and
           the present clock follows the cap: 60 Hz at cap 30 (interval 2 = 33.3 ms), 90 Hz at cap 60 (interval 2 = 22.2 ms).
           Unlocked clocks, a player's settings, walking route: cap 30 29.6-29.9 fps (a 10-minute soak 29.8, every 5 s window
           29.2-30.2) against 25.4-27.8 with one frame; cap 60 39.0-39.9 fps with the GPU 90-92% busy (a 60 Hz clock: 30 fps,
           GPU 57-67%; 120 Hz: 37.7-38.6 fps and 4 C hotter). In-app recordings (TOTK_OHOS_FRAME_REC, standing and walking,
           both caps) showed no white blobs, flashes or HUD faults beyond the one-frame default's. TOTK_OHOS_SYNC_LAG=0 gives
           back one frame in flight and the clocks above it (90 Hz at cap 30, 60 Hz at cap 60). */
        /* totk-ohos 2026-11-17: at one sky-island spot with yellow trees the world froze twice behind the game's loading
           indicator while rendering went on (two frames in flight; never with one): a completion check the game makes after
           the early frame-end answer may still see the frame unfinished (from the analysis of that night's runs; the
           notes are not published). Two frames stay the default (player's choice, 30 fps
           against 19-20 with one frame standing by Hyrule Castle), now with told-fence consistency (hle_nvn.c F1,
           TOTK_OHOS_SYNC_CONSIST): its default kinds (sync polls / waits, event reads, buffer invalidates) never wait in
           healthy play (29.9 fps, 0 waits), while F1 on every kind cost 30 -> 19 fps. The freeze did not come back in 7
           two-frame loads that night, the spot included; which check gated it is still unknown. TOTK_OHOS_SYNC_LAG=0: one frame. */
        setenv("TOTK_OHOS_SYNC_LAG", "1", 0);
        const char *lg = getenv("TOTK_OHOS_SYNC_LAG"); int lag = lg && atoi(lg) > 0;
        if (lag) setenv("TOTK_OHOS_INTERVAL_MAX", "2", 0);
        setenv("DM_PRESENT_HZ", panel >= 60 && panel <= 144 ? ph : cap >= 90 ? v : cap == 30 ? (lag ? "60" : "90") : (lag ? "90" : "60"), 0); }
    setenv("TOTK_FPS_AUTO", "0", 0); setenv("TOTK_UNCAPPED", "0", 0); setenv("TOTK_PACE", "0", 0);
    setenv("TOTK_ACQUIRE_CLOCK", "0", 0); setenv("TOTK_ACQUIRE_SLACK", "1", 0); setenv("TOTK_PRESENT_SLOTS", "1", 0);
    /* A draw whose pipeline is still compiling is skipped (objects missing for a few frames: flicker / pop-in, ~2000
       draws on entering the world); after one skipped submission the pipeline is built synchronously instead (the
       reference waits 90). The pipeline cache keeps them, so the stall is once per pipeline. */
    setenv("TOTK_VK_PIPE_WAIT_MAX", "1", 0);
    /* Defaults the reference only turns on for __ANDROID__ (this target is not): parallel recording (vk_record.c, two
       recorder workers: the walker alone spent 70-85 ms a frame recording in the cave) and asynchronous preemption of
       the virtual cores (threads.c: without it a save loading into the overworld can hang for good). */
    setenv("TOTK_VK_REC_THREADS", "2", 0);
    /* Texture-upload staging comes from the 8 submission slots' arenas, and without a cap each slot keeps its high-water
       for the session (nvn_vk.c arena_retain_cap): 2.0-2.7 GB after a few area loads on this phone, on top of the pools
       and images, and the system killed the game (signal 9) while the GPU driver purged regions of a 5.8-6.0 GB total.
       32 MiB per slot keeps at most 256 MiB; a bigger upload burst still gets its memory and gives it back after. */
    setenv("TOTK_VK_STAGING_RETAIN_MB", "32", 0);
    setenv("TOTK_OHOS_PRIMARY_FLOOR", "64", 0);   /* nvn_vk.c: primaries kept warm per slot */
    /* Descriptor sets kept per layout instead of a pool reset per draw batch (vk_record.c TOTK_VK_DESC_STOCK): Maleoon takes
       ~145 us per vkResetDescriptorPool, 12.6 of the two recorder workers' 20.5 ms a frame (2026-11-08); with the stock
       4.6 ms and 372 allocations a frame instead of 6032. The ceiling bounds the pools (GL stayed at 2.5 GB with it). */
    setenv("TOTK_VK_DESC_STOCK", "1", 0);
    /* HarmonyOS defaults measured 2026-11-08 (a18, start platform, 90 s each, GPU 850-930 MHz): narrow pass barriers alone
       25.4 fps (the 112 inline uniform updates a frame still drain the GPU twice each), narrow + uniform renaming 29.9 fps
       (GPU 26.7 ms, walker 16.4 ms) against 26.7 for the reference's full barriers and direct updates. Both are the
       reference's own switches / this port's TOTK_OHOS_PASS_BARRIER (vk_draw.c); =full / =0 restore the reference. */
    setenv("TOTK_OHOS_PASS_BARRIER", "narrow", 0);
    setenv("TOTK_VK_UB_RENAME", "1", 0);
    /* The BC -> ETC2 transcode (vk_res.c bc_to_etc) and large deswizzles run on these helpers beside the walker. */
    setenv("TOTK_VK_UPLOAD_THREADS", "3", 0);
    setenv("TOTK_VK_DESC_STOCK_MAX_SETS", "8192", 0);
    /* Measured 2026-11-15 on the phone without a frequency lock (game mark on, save slot 2, adjacent alternating pairs):
       no descriptor-stock rebuild on pressure (vk_record.c) took the walker's waits from 4.6-5.0 to 2.4-3.0 ms and frames
       over 66.7 ms from 7-18 to 5 per 300, +0.7/+0.7 fps standing and +1.1/+2.5 fps on the moving route. Cost: the stock
       keeps more descriptor pools (~+48 pools, ~+100 MiB process memory; the "memory Vulkan bytes" line leaves descriptor
       memory out). A volatile texture whose hash matched 150 frames after its last change goes back to the audit
       (vk_res.c TOTK_OHOS_TEX_VOL_DECAY): 5 textures (~25 MiB) changed only while the save loaded were hashed every frame
       on the walker, 4.3 -> 1.6 ms a frame, +0.8/+1.0 fps standing. Cost: a later change of such a texture shows up to
       TOTK_OHOS_TEX_AUDIT (16) frames late instead of 1. */
    setenv("TOTK_OHOS_STOCK_PRESSURE_GENS", "0", 0);
    setenv("TOTK_OHOS_TEX_VOL_DECAY", "150", 0);
    setenv("TOTK_ASYNC_PREEMPT", "1", 0);
    {   const char *c = getenv("TOTK_OHOS_FPS_CAP");   /* the Android app's 60 FPS companions */
        if (c && atoi(c) >= 60) setenv("TOTK_VK_SPLIT_PASS_DRAWS", "512", 0);
        /* Cap 30 too (2026-11-15, no frequency lock, player's settings, alternating pairs): the ~2030-draw scene pass
           reached the GPU only when its last draw was recorded, leaving the GPU idle ~6 ms a frame; ending it every 1024
           draws feeds the GPU during recording: 21.9/23.4 -> 26.3/25.4 fps (round 2 already at thermal level 2), frames at
           the 20 fps tier 143-212 -> 29-66 per 300, GPU clock 570 -> 650-690 MHz as the governor follows the steadier load;
           flicker detector 0. */
        else setenv("TOTK_VK_SPLIT_PASS_DRAWS", "1024", 0); }
    /* The split's depth "touch" (TOTK_VK_SPLIT_LRZ_SAFE=1, upstream default) copies one depth texel out and back with
       three full barriers around every reopened depth pass, and refuses splits of depth-only regions: both exist only to
       invalidate Qualcomm's LRZ (Turnip / the Adreno driver). Maleoon has no LRZ; a depth attachment stored and loaded
       again is plain Vulkan. With the touch, 3 splits a frame cost 26.5 touches (~2 ms GPU) and the split gained little
       (23.5/24.8 fps); without it 26.0/24.5 at 512 and 26.3/25.4 at 1024 draws. */
    setenv("TOTK_VK_SPLIT_LRZ_SAFE", "0", 0);
    /* Two-level chunks (vk_draw.c chunk_limit_now): the first chunk of each game submission is cut after 32 draws so the
       GPU starts sooner, and after the frame's big scene pass chunks grow to 150 draws (fewer submission bubbles; the
       backlog is ~20 ms of GPU work there). 2026-11-15, alternating pairs, player's settings, no lock: +1.0/+2.4/+0.0/+1.4
       fps over four rounds, walker ring waits 1.2-2.1 -> ~0.1 ms every time. */
    setenv("TOTK_OHOS_CHUNK_FIRST", "32", 0);
    setenv("TOTK_OHOS_CHUNK_TAIL", "150", 0);
    /* The game's frame controller (60 FPS patch, always in 1.4.3's embedded code) picks its present interval from the GPU
       time it is told: the default "latency" reports overstate it by ~10 ms (reference VULKAN.md; here 35 ms reported
       for frames the GPU finished in ~25), so it sat at interval 3 = 20 fps on frames that fit 30. Hardware timestamps. */
    setenv("TOTK_GPU_TIME", "hardware", 0);
    /* Game threads start with the process's mask (no capacity-based narrowing at start: a critical thread created by
       a narrowed one could not reach the Prime cores); ohos/ohos_sched.c places them after. */
    setenv("TOTK_AFFINITY_GAME", "all", 0);
    snprintf(path, sizeof path, "%s/shaders/" GAME_DIR, work_dir); setenv("TOTK_SHADER_SEED_DIR", path, 0);
    {   /* sb-align.txt next to the seed: the storage buffer alignment its shaders were translated for (native-harmonyos/xlate,
           TOTK_XLATE_SB_ALIGN); the backend binds storage buffers at that alignment (vk_draw.c vk_sb_align_down). */
        char marker[1100]; snprintf(marker, sizeof marker, "%s/shaders/" GAME_DIR "/sb-align.txt", work_dir);
        FILE *f = fopen(marker, "r"); char v[32] = "";
        if (f) { if (fgets(v, sizeof v, f)) v[strcspn(v, "\r\n ")] = 0; fclose(f); }
        if (v[0]) setenv("TOTK_VK_SB_ALIGN", v, 0);
    }
    snprintf(path, sizeof path, "%s/shaders/" GAME_DIR "/seed.vkpack", work_dir);
    { struct stat st; if (!stat(path, &st) && S_ISREG(st.st_mode)) setenv("TOTK_VK_SEED_IO", "1", 0); }
    fprintf(stderr, "[totk-ohos] policy: MODE=%s FPS60=%s UNCAPPED=%s ACQUIRE_SLACK=%s PRESENT_SLOTS=%s SEED=%s%s\n",
            getenv("TOTK_OPERATION_MODE"), getenv("TOTK_FPS60"), getenv("TOTK_UNCAPPED"), getenv("TOTK_ACQUIRE_SLACK"),
            getenv("TOTK_PRESENT_SLOTS"), getenv("TOTK_SHADER_SEED_DIR"), getenv("TOTK_VK_SEED_IO") ? " (pack)" : "");
    if (chdir(work_dir)) return "cannot enter the android work folder";
    fprintf(stderr, "[totk-ohos] start: files %s\n", files_dir);
    /* physical controllers and the phone's sensors (frontend_gamepad_ohos.c, frontend_feedback_ohos.c; window build) */
    if (ohos_gamepad_start) ohos_gamepad_start();
    if (ohos_feedback_start) ohos_feedback_start();
    log_tp_words("ArkTS thread");
    /* totk-ohos: the TOTK_INPUT_SCRIPT / TOTK_INPUT_FILE diagnostics (runtime/input.c: "threads", "framestep <ms>", ...).
       The Android app starts them once its environment is final (frontend_android.c runtime_input_diag_start); this
       front end never did, so a command file set through env-dev was ignored (2026-11-17 freeze investigation). Nothing
       starts unless one of the two variables is set. */
    { extern void runtime_input_diag_start(void); runtime_input_diag_start(); }
    pthread_attr_t attr; pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 64u << 20);   /* musl's default thread stack is 128 KiB */
    pthread_t t;
    int rc = pthread_create(&t, &attr, game_thread, NULL);
    pthread_attr_destroy(&attr);
    if (rc) return "cannot create the runtime thread";
    pthread_detach(t);
    return 0;
}

static napi_value Start(napi_env env, napi_callback_info info) {
    size_t argc = 1; napi_value argv[1];
    napi_get_cb_info(env, info, &argc, argv, NULL, NULL);
    char dir[1024] = {0}; size_t len = 0;
    if (argc < 1 || napi_get_value_string_utf8(env, argv[0], dir, sizeof dir, &len) != napi_ok || !len) {
        napi_throw_error(env, NULL, "start(filesDir) needs the files directory");
        return NULL;
    }
    const char *err = totk_ohos_start(dir);
    napi_value result;
    napi_create_string_utf8(env, err ? err : "started", NAPI_AUTO_LENGTH, &result);
    return result;
}

/* Window and controller (native-harmonyos/ohos/frontend_ohos.c; absent from the headless build, hence weak). */
__attribute__((weak)) int totk_ohos_surface(uint64_t surface_id, int32_t width, int32_t height);
__attribute__((weak)) void totk_ohos_surface_changed(int32_t width, int32_t height);
__attribute__((weak)) void totk_ohos_surface_destroyed(void);
__attribute__((weak)) void totk_ohos_pad(uint64_t buttons, int32_t lx, int32_t ly, int32_t rx, int32_t ry);

static int32_t arg_i32(napi_env env, napi_value v) { int32_t x = 0; napi_get_value_int32(env, v, &x); return x; }
static napi_value undefined(napi_env env) { napi_value u; napi_get_undefined(env, &u); return u; }

/* setSurface(surfaceId: string, width, height): the XComponent's surface (XComponentController.getXComponentSurfaceId) */
static napi_value SetSurface(napi_env env, napi_callback_info info) {
    size_t argc = 3; napi_value argv[3];
    napi_get_cb_info(env, info, &argc, argv, NULL, NULL);
    char id[32] = {0}; size_t len = 0;
    if (argc < 3 || napi_get_value_string_utf8(env, argv[0], id, sizeof id, &len) != napi_ok || !len) {
        napi_throw_error(env, NULL, "setSurface(surfaceId, width, height)"); return NULL;
    }
    int rc = totk_ohos_surface ? totk_ohos_surface(strtoull(id, NULL, 10), arg_i32(env, argv[1]), arg_i32(env, argv[2])) : -2;
    napi_value result; napi_create_int32(env, rc, &result); return result;
}
static napi_value SurfaceChanged(napi_env env, napi_callback_info info) {
    size_t argc = 2; napi_value argv[2];
    napi_get_cb_info(env, info, &argc, argv, NULL, NULL);
    if (argc >= 2 && totk_ohos_surface_changed) totk_ohos_surface_changed(arg_i32(env, argv[0]), arg_i32(env, argv[1]));
    return undefined(env);
}
static napi_value SurfaceDestroyed(napi_env env, napi_callback_info info) {
    (void)info; if (totk_ohos_surface_destroyed) totk_ohos_surface_destroyed(); return undefined(env);
}
/* setPad(buttons, lx, ly, rx, ry): Switch button bits (low 32 bits are enough for the face, shoulder, stick and dpad
   buttons) and stick positions in the Switch's range (-32767..32767, up positive). */
static napi_value SetPad(napi_env env, napi_callback_info info) {
    size_t argc = 5; napi_value argv[5];
    napi_get_cb_info(env, info, &argc, argv, NULL, NULL);
    if (argc < 5) { napi_throw_error(env, NULL, "setPad(buttons, lx, ly, rx, ry)"); return NULL; }
    double buttons = 0; napi_get_value_double(env, argv[0], &buttons);
    if (totk_ohos_pad) totk_ohos_pad((uint64_t)buttons, arg_i32(env, argv[1]), arg_i32(env, argv[2]), arg_i32(env, argv[3]), arg_i32(env, argv[4]));
    return undefined(env);
}

/* The on-screen controls' touch API (components/Controls). */
__attribute__((weak)) unsigned totk_ohos_touch_epoch(void);
__attribute__((weak)) int totk_ohos_touch_button(int id, uint64_t mask, int down, unsigned epoch);
__attribute__((weak)) int totk_ohos_touch_stick(int id, int side, float x, float y, unsigned epoch);
__attribute__((weak)) int totk_ohos_touch_release(int id, unsigned epoch, int cancel);
static double arg_f64(napi_env env, napi_value v) { double x = 0; napi_get_value_double(env, v, &x); return x; }
static napi_value boolean(napi_env env, int b) { napi_value r; napi_get_boolean(env, b != 0, &r); return r; }
static napi_value GetTouchInputEpoch(napi_env env, napi_callback_info info) {
    (void)info; napi_value r; napi_create_uint32(env, totk_ohos_touch_epoch ? totk_ohos_touch_epoch() : 0, &r); return r;
}
static napi_value SetTouchButton(napi_env env, napi_callback_info info) {
    size_t argc = 4; napi_value argv[4]; napi_get_cb_info(env, info, &argc, argv, NULL, NULL);
    if (argc < 4 || !totk_ohos_touch_button) return boolean(env, 0);
    bool down = false; napi_get_value_bool(env, argv[2], &down);
    return boolean(env, totk_ohos_touch_button(arg_i32(env, argv[0]), (uint64_t)arg_f64(env, argv[1]), down, (unsigned)arg_f64(env, argv[3])));
}
static napi_value SetTouchStick(napi_env env, napi_callback_info info) {
    size_t argc = 5; napi_value argv[5]; napi_get_cb_info(env, info, &argc, argv, NULL, NULL);
    if (argc < 5 || !totk_ohos_touch_stick) return boolean(env, 0);
    return boolean(env, totk_ohos_touch_stick(arg_i32(env, argv[0]), arg_i32(env, argv[1]), (float)arg_f64(env, argv[2]),
                                              (float)arg_f64(env, argv[3]), (unsigned)arg_f64(env, argv[4])));
}
/* addTouchLook(pointerId, dx, dy, epoch): a swipe-camera finger moved by (dx, dy) fractions of the swipe area's height
   (right / up positive); frontend_ohos.c turns the distance into right-stick input as the game reads it. */
__attribute__((weak)) int totk_ohos_touch_look(int id, float dx, float dy, unsigned epoch);
static napi_value AddTouchLook(napi_env env, napi_callback_info info) {
    size_t argc = 4; napi_value argv[4]; napi_get_cb_info(env, info, &argc, argv, NULL, NULL);
    if (argc < 4 || !totk_ohos_touch_look) return boolean(env, 0);
    return boolean(env, totk_ohos_touch_look(arg_i32(env, argv[0]), (float)arg_f64(env, argv[1]), (float)arg_f64(env, argv[2]),
                                             (unsigned)arg_f64(env, argv[3])));
}
static napi_value ReleaseTouch(napi_env env, napi_callback_info info, int cancel) {
    size_t argc = 2; napi_value argv[2]; napi_get_cb_info(env, info, &argc, argv, NULL, NULL);
    if (argc < 2 || !totk_ohos_touch_release) return boolean(env, 0);
    return boolean(env, totk_ohos_touch_release(arg_i32(env, argv[0]), (unsigned)arg_f64(env, argv[1]), cancel));
}
static napi_value ReleaseTouchPointer(napi_env env, napi_callback_info info) { return ReleaseTouch(env, info, 0); }
static napi_value CancelTouchPointer(napi_env env, napi_callback_info info) { return ReleaseTouch(env, info, 1); }

/* Physical controllers (frontend_gamepad_ohos.c) and the page's foreground state (frontend_feedback_ohos.c). */
__attribute__((weak)) void totk_ohos_pad_key(int bit, int down);
__attribute__((weak)) int totk_ohos_gamepad_status(char *name, size_t cap);
__attribute__((weak)) void ohos_feedback_foreground(int on);
__attribute__((weak)) void ohos_gamepad_clear(void);
/* setPadKey(bit, down): a controller key the game page receives itself (Select / View as Minus, bit 11). */
static napi_value SetPadKey(napi_env env, napi_callback_info info) {
    size_t argc = 2; napi_value argv[2]; napi_get_cb_info(env, info, &argc, argv, NULL, NULL);
    bool down = false;
    if (argc >= 2 && totk_ohos_pad_key && napi_get_value_bool(env, argv[1], &down) == napi_ok) totk_ohos_pad_key(arg_i32(env, argv[0]), down);
    return undefined(env);
}
/* gamepadStatus(): { count, name }: connected controllers (-1 without GameControllerKit) and the last one's name. */
static napi_value GamepadStatus(napi_env env, napi_callback_info info) {
    (void)info; char name[96] = ""; int n = totk_ohos_gamepad_status ? totk_ohos_gamepad_status(name, sizeof name) : -1;
    napi_value r, c, s;
    napi_create_object(env, &r); napi_create_int32(env, n, &c); napi_create_string_utf8(env, name, NAPI_AUTO_LENGTH, &s);
    napi_set_named_property(env, r, "count", c); napi_set_named_property(env, r, "name", s);
    return r;
}
/* bootStatus(): the loading screen's state (frontend_ohos.c totk_ohos_boot_status): { stage, detail, done, total, items,
   frames, elapsed }; frames > 0 once the game has presented a picture. */
__attribute__((weak)) void totk_ohos_boot_status(char *stage, size_t stage_cap, char *detail, size_t detail_cap,
                                                 double *done, double *total, int *items, double *frames, double *elapsed);
static napi_value BootStatus(napi_env env, napi_callback_info info) {
    (void)info; char stage[192] = "", detail[256] = ""; double done = 0, total = 0, frames = 0, elapsed = 0; int items = 0;
    if (totk_ohos_boot_status) totk_ohos_boot_status(stage, sizeof stage, detail, sizeof detail, &done, &total, &items, &frames, &elapsed);
    napi_value r, v;
    napi_create_object(env, &r);
    napi_create_string_utf8(env, stage, NAPI_AUTO_LENGTH, &v); napi_set_named_property(env, r, "stage", v);
    napi_create_string_utf8(env, detail, NAPI_AUTO_LENGTH, &v); napi_set_named_property(env, r, "detail", v);
    napi_create_double(env, done, &v); napi_set_named_property(env, r, "done", v);
    napi_create_double(env, total, &v); napi_set_named_property(env, r, "total", v);
    napi_get_boolean(env, items != 0, &v); napi_set_named_property(env, r, "items", v);
    napi_create_double(env, frames, &v); napi_set_named_property(env, r, "frames", v);
    napi_create_double(env, elapsed, &v); napi_set_named_property(env, r, "elapsed", v);
    return r;
}
/* setForeground(on): the game page went to the background (rumble stops, sensors pause, held buttons are released) or
   came back. */
static napi_value SetForeground(napi_env env, napi_callback_info info) {
    size_t argc = 1; napi_value argv[1]; napi_get_cb_info(env, info, &argc, argv, NULL, NULL);
    bool on = true; if (argc >= 1) napi_get_value_bool(env, argv[0], &on);
    if (!on && ohos_gamepad_clear) ohos_gamepad_clear();
    if (ohos_feedback_foreground) ohos_feedback_foreground(on);
    return undefined(env);
}
static napi_value IsStarted(napi_env env, napi_callback_info info) { (void)info; return boolean(env, atomic_load(&started)); }

/* The game's keyboard (frontend_ohos.c runtime_front_text_input): onTextInput(callback) registers the page's dialog;
   each request reaches it as { serial, prompt, initial, maxLength, minLength, digitsOnly, secure } on the ArkTS
   thread, and the page answers with textInputDone(serial, text) or textInputDone(serial, null) to cancel. */
__attribute__((weak)) void totk_ohos_text_listener(void (*fn)(unsigned serial, const RuntimeTextRequest *req));
__attribute__((weak)) int totk_ohos_text_done(unsigned serial, const char *utf8);
typedef struct { unsigned serial; char *prompt, *initial; int max_units, min_units, secure, digits; } TextAsk;
static napi_threadsafe_function text_tsfn;
static void text_ask_free(TextAsk *a) { if (a) { free(a->prompt); free(a->initial); free(a); } }
static void text_call_js(napi_env env, napi_value callback, void *context, void *data) {
    (void)context; TextAsk *a = data;
    int delivered = 0;
    if (env && callback && a) {
        napi_value o, v, u; napi_create_object(env, &o);
        napi_create_uint32(env, a->serial, &v); napi_set_named_property(env, o, "serial", v);
        napi_create_string_utf8(env, a->prompt, NAPI_AUTO_LENGTH, &v); napi_set_named_property(env, o, "prompt", v);
        napi_create_string_utf8(env, a->initial, NAPI_AUTO_LENGTH, &v); napi_set_named_property(env, o, "initial", v);
        napi_create_int32(env, a->max_units, &v); napi_set_named_property(env, o, "maxLength", v);
        napi_create_int32(env, a->min_units, &v); napi_set_named_property(env, o, "minLength", v);
        napi_get_boolean(env, a->digits != 0, &v); napi_set_named_property(env, o, "digitsOnly", v);
        napi_get_boolean(env, a->secure != 0, &v); napi_set_named_property(env, o, "secure", v);
        napi_get_undefined(env, &u);
        delivered = napi_call_function(env, u, callback, 1, &o, NULL) == napi_ok;
    }
    if (!delivered && a && totk_ohos_text_done) totk_ohos_text_done(a->serial, NULL);   /* nobody to ask: cancel */
    text_ask_free(a);
}
static void text_notify(unsigned serial, const RuntimeTextRequest *r) {
    TextAsk *a = calloc(1, sizeof *a);
    if (a) {
        a->serial = serial; a->prompt = strdup(r->prompt ? r->prompt : ""); a->initial = strdup(r->initial ? r->initial : "");
        a->max_units = r->max_units; a->min_units = r->min_units; a->secure = r->secure; a->digits = r->digits_only;
    }
    if (!a || !a->prompt || !a->initial || !text_tsfn || napi_call_threadsafe_function(text_tsfn, a, napi_tsfn_nonblocking) != napi_ok) {
        text_ask_free(a);
        if (totk_ohos_text_done) totk_ohos_text_done(serial, NULL);
    }
}
static napi_value OnTextInput(napi_env env, napi_callback_info info) {
    size_t argc = 1; napi_value argv[1]; napi_get_cb_info(env, info, &argc, argv, NULL, NULL);
    if (argc < 1 || !totk_ohos_text_listener) return boolean(env, 0);
    napi_value name; napi_create_string_utf8(env, "totk-text-input", NAPI_AUTO_LENGTH, &name);
    napi_threadsafe_function f = NULL;
    if (napi_create_threadsafe_function(env, argv[0], NULL, name, 0, 1, NULL, NULL, NULL, text_call_js, &f) != napi_ok) return boolean(env, 0);
    napi_unref_threadsafe_function(env, f);   /* the page's callback must not keep the app alive */
    if (text_tsfn) napi_release_threadsafe_function(text_tsfn, napi_tsfn_release);
    text_tsfn = f;
    totk_ohos_text_listener(text_notify);
    return boolean(env, 1);
}
static napi_value TextInputDone(napi_env env, napi_callback_info info) {
    size_t argc = 2; napi_value argv[2]; napi_get_cb_info(env, info, &argc, argv, NULL, NULL);
    if (argc < 1 || !totk_ohos_text_done) return boolean(env, 0);
    uint32_t serial = 0; napi_get_value_uint32(env, argv[0], &serial);
    napi_valuetype t = napi_undefined; if (argc >= 2) napi_typeof(env, argv[1], &t);
    if (t != napi_string) return boolean(env, totk_ohos_text_done(serial, NULL));
    size_t len = 0; napi_get_value_string_utf8(env, argv[1], NULL, 0, &len);
    char *text = malloc(len + 1); if (!text) return boolean(env, totk_ohos_text_done(serial, NULL));
    napi_get_value_string_utf8(env, argv[1], text, len + 1, &len);
    int ok = totk_ohos_text_done(serial, text);
    free(text);
    return boolean(env, ok);
}

/* gameVersion(): the game version this library runs, "1.4.3" or "1.0.0" (the settings page shows that version's options). */
static napi_value GameVersion(napi_env env, napi_callback_info info) {
    (void)info; napi_value r;
    napi_create_string_utf8(env, strcmp(GAME_DIR, "v143") ? "1.0.0" : "1.4.3", NAPI_AUTO_LENGTH, &r); return r;
}

static napi_value Init(napi_env env, napi_value exports) {
    napi_property_descriptor desc[] = {{"start", NULL, Start, NULL, NULL, NULL, napi_default, NULL},
                                       {"gameVersion", NULL, GameVersion, NULL, NULL, NULL, napi_default, NULL},
                                       {"setSurface", NULL, SetSurface, NULL, NULL, NULL, napi_default, NULL},
                                       {"surfaceChanged", NULL, SurfaceChanged, NULL, NULL, NULL, napi_default, NULL},
                                       {"surfaceDestroyed", NULL, SurfaceDestroyed, NULL, NULL, NULL, napi_default, NULL},
                                       {"setPad", NULL, SetPad, NULL, NULL, NULL, napi_default, NULL},
                                       {"getTouchInputEpoch", NULL, GetTouchInputEpoch, NULL, NULL, NULL, napi_default, NULL},
                                       {"setTouchButton", NULL, SetTouchButton, NULL, NULL, NULL, napi_default, NULL},
                                       {"setTouchStick", NULL, SetTouchStick, NULL, NULL, NULL, napi_default, NULL},
                                       {"addTouchLook", NULL, AddTouchLook, NULL, NULL, NULL, napi_default, NULL},
                                       {"releaseTouchPointer", NULL, ReleaseTouchPointer, NULL, NULL, NULL, napi_default, NULL},
                                       {"cancelTouchPointer", NULL, CancelTouchPointer, NULL, NULL, NULL, napi_default, NULL},
                                       {"setPadKey", NULL, SetPadKey, NULL, NULL, NULL, napi_default, NULL},
                                       {"gamepadStatus", NULL, GamepadStatus, NULL, NULL, NULL, napi_default, NULL},
                                       {"bootStatus", NULL, BootStatus, NULL, NULL, NULL, napi_default, NULL},
                                       {"setForeground", NULL, SetForeground, NULL, NULL, NULL, napi_default, NULL},
                                       {"isStarted", NULL, IsStarted, NULL, NULL, NULL, napi_default, NULL},
                                       {"onTextInput", NULL, OnTextInput, NULL, NULL, NULL, napi_default, NULL},
                                       {"textInputDone", NULL, TextInputDone, NULL, NULL, NULL, napi_default, NULL}};
    napi_define_properties(env, exports, sizeof desc / sizeof *desc, desc);
    return exports;
}

static napi_module totk_module = {
    .nm_version = 1, .nm_flags = 0, .nm_filename = NULL, .nm_register_func = Init, .nm_modname = "totk_ohos",
    .nm_priv = NULL, .reserved = {0},
};
__attribute__((constructor)) static void register_module(void) { napi_module_register(&totk_module); }
