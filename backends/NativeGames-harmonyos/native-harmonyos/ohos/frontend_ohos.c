/* HarmonyOS window frontend for the Android-stack runtime (built with TOTK_ANDROID_APP, in place of the Android app's
   runtime/frontend_android.c). Like that one it shares the boot log, nn::vi and the game thread of
   runtime/frontend_headless.c (included below) and adds what the app build expects from its frontend:
     the window   the ArkTS XComponent's surface (an OHNativeWindow, used as ANativeWindow through
                  native-harmonyos/ohos/anative_window_ohos.c) handed to the Vulkan presenter (vk_window_set);
     input        the controller state of the on-screen controls (totk_ohos_touch_*) merged with physical controllers
                  (frontend_gamepad_ohos.c), read by the game's hid;
     text entry   the game's keyboard requests (nn::swkbd), answered by a dialog on the game page;
     the rest     close requests, display vsync (the headless 60 Hz thread); rumble and motion are in
                  frontend_feedback_ohos.c.
   The window, pad and text calls come from ArkTS through native-harmonyos/ohos/totk_napi.c. */
/* The boot progress the game page shows while the game loads (Game.ets polls bootStatus(), totk_napi.c): the stage the
   runtime reports with its progress, and whether a frame was presented. The headless frontend's versions, renamed
   here, still write the boot log. */
#define runtime_boot_progress headless_boot_progress
#define runtime_boot_count headless_boot_count
#define runtime_boot_status headless_boot_status
#define runtime_warm_progress headless_warm_progress
#define runtime_warm_done headless_warm_done
#define front_frame_presented headless_frame_presented
#include "frontend_headless.c"
#undef runtime_boot_progress
#undef runtime_boot_count
#undef runtime_boot_status
#undef runtime_warm_progress
#undef runtime_warm_done
#undef front_frame_presented
void runtime_boot_progress(const char *stage, uint64_t done, uint64_t total);
void runtime_boot_count(const char *stage, uint64_t done, uint64_t total);
void runtime_boot_status(const char *stage, const char *detail);
void runtime_warm_progress(uint64_t done, uint64_t total, uint64_t shaders, uint64_t shader_total, uint64_t pipelines, uint64_t pipeline_total);
void runtime_warm_done(void);
void front_frame_presented(void);

static pthread_mutex_t boot_mu = PTHREAD_MUTEX_INITIALIZER;
static char boot_stage[192], boot_detail[256];
static uint64_t boot_done, boot_total;
static int boot_items;   /* 1: done / total count items (shader import), 0: bytes */
static void boot_set(const char *stage, const char *detail, uint64_t done, uint64_t total, int items) {
    pthread_mutex_lock(&boot_mu);
    if (stage) snprintf(boot_stage, sizeof boot_stage, "%s", stage);
    snprintf(boot_detail, sizeof boot_detail, "%s", detail ? detail : "");
    boot_done = done; boot_total = total; boot_items = items;
    pthread_mutex_unlock(&boot_mu);
}
void runtime_boot_progress(const char *stage, uint64_t done, uint64_t total) { boot_set(stage, NULL, done, total, 0); headless_boot_progress(stage, done, total); }
void runtime_boot_count(const char *stage, uint64_t done, uint64_t total) { boot_set(stage, NULL, done, total, 1); headless_boot_count(stage, done, total); }
void runtime_boot_status(const char *stage, const char *detail) { boot_set(stage, detail, 0, 0, 0); headless_boot_status(stage, detail); }
void runtime_warm_progress(uint64_t done, uint64_t total, uint64_t shaders, uint64_t shader_total, uint64_t pipelines, uint64_t pipeline_total) {
    boot_set("shader warm-up", NULL, done, total, 1);
    headless_warm_progress(done, total, shaders, shader_total, pipelines, pipeline_total);
}
void runtime_warm_done(void) { boot_set("shader warm-up done", NULL, 0, 0, 0); headless_warm_done(); }
void front_frame_presented(void) { headless_frame_presented(); }
/* For totk_napi.c bootStatus(): the stage, its detail, progress, frames presented so far and seconds since boot. */
__attribute__((visibility("hidden"))) void totk_ohos_boot_status(char *stage, size_t stage_cap, char *detail, size_t detail_cap,
                                                                 double *done, double *total, int *items, double *frames, double *elapsed) {
    pthread_mutex_lock(&boot_mu);
    snprintf(stage, stage_cap, "%s", boot_stage); snprintf(detail, detail_cap, "%s", boot_detail);
    *done = (double)boot_done; *total = (double)boot_total; *items = boot_items;
    pthread_mutex_unlock(&boot_mu);
    *frames = (double)atomic_load(&presented);
    *elapsed = boot_start_ns ? since_boot() : 0.0;
}
#include <android/native_window.h>
#include <native_window/external_window.h>

uint64_t vk_window_set(ANativeWindow *w, ANativeWindow *surface_parent, int display_id);
void vk_window_resized(ANativeWindow *w, uint64_t epoch);
int ohos_gamepad_read(RuntimeGamepadState *out);   /* frontend_gamepad_ohos.c */
void ohos_gamepad_clear(void);

int runtime_front_request_close(void) { fprintf(stderr, "[totk] close requested (ignored: the app closes the game)\n"); return 0; }

/* ---- text entry (nn::swkbd, runtime/swkbd_host.c) ----
   The guest thread that asks waits here (swkbd_host.c gives its virtual core up). The request goes to the game page
   through the listener totk_napi.c registers (a thread-safe call into ArkTS, which shows a dialog with the system
   keyboard); the page answers once with totk_ohos_text_done, the text as UTF-8 or NULL for a cancel. Without a page
   listening the request is cancelled, as before. One request at a time; its strings live on the waiting thread. */
enum { TEXT_IDLE, TEXT_ASKED, TEXT_DONE };
static pthread_mutex_t text_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t text_cv = PTHREAD_COND_INITIALIZER;
static int text_state, text_ok;
static unsigned text_serial;
static char *text_out;
static size_t text_cap;
static void (*text_listener)(unsigned serial, const RuntimeTextRequest *req);
__attribute__((visibility("hidden"))) void totk_ohos_text_listener(void (*fn)(unsigned serial, const RuntimeTextRequest *req)) {
    pthread_mutex_lock(&text_mu); text_listener = fn; pthread_mutex_unlock(&text_mu);
}
int runtime_front_text_input(const RuntimeTextRequest *req, char *out, size_t out_size) {
    if (out_size) out[0] = 0;
    pthread_mutex_lock(&text_mu);
    while (text_state != TEXT_IDLE) pthread_cond_wait(&text_cv, &text_mu);
    void (*listener)(unsigned, const RuntimeTextRequest *) = text_listener;
    if (!listener) {
        pthread_mutex_unlock(&text_mu);
        fprintf(stderr, "[totk-ohos] keyboard: no game page to ask; cancelled\n");
        return 0;
    }
    unsigned serial = ++text_serial;
    text_out = out; text_cap = out_size; text_ok = 0; text_state = TEXT_ASKED;
    pthread_mutex_unlock(&text_mu);
    fprintf(stderr, "[totk-ohos] keyboard: asking the game page (request %u, up to %d characters)\n", serial, req->max_units);
    listener(serial, req);
    pthread_mutex_lock(&text_mu);
    while (text_state != TEXT_DONE) pthread_cond_wait(&text_cv, &text_mu);
    int ok = text_ok;
    text_out = NULL; text_cap = 0; text_state = TEXT_IDLE;
    pthread_cond_broadcast(&text_cv);
    pthread_mutex_unlock(&text_mu);
    fprintf(stderr, "[totk-ohos] keyboard: request %u %s\n", serial, ok ? "confirmed" : "cancelled");
    return ok;
}
/* The page's answer to request serial (0: whichever is open): the text as UTF-8, or NULL to cancel. */
__attribute__((visibility("hidden"))) int totk_ohos_text_done(unsigned serial, const char *utf8) {
    pthread_mutex_lock(&text_mu);
    if (text_state != TEXT_ASKED || (serial && serial != text_serial)) { pthread_mutex_unlock(&text_mu); return 0; }
    if (utf8 && text_out && text_cap) {
        size_t n = strlen(utf8), k = n < text_cap - 1 ? n : text_cap - 1;
        while (k && k < n && ((unsigned char)utf8[k] & 0xc0) == 0x80) k--;   /* never half a character */
        memcpy(text_out, utf8, k); text_out[k] = 0;
    }
    text_ok = utf8 != NULL; text_state = TEXT_DONE;
    pthread_cond_broadcast(&text_cv);
    pthread_mutex_unlock(&text_mu);
    return 1;
}
/* Display vsync: the guest's nn::vi vsync event follows the panel's real vsync (OH_NativeVSync), divided down to the
   frame-rate cap TOTK_OHOS_FPS_CAP (30 / 60 / 90 / 120, default 30; totk_napi.c turns the 60 FPS patch on for 60 and up,
   whose real-time frame step keeps game speed right at any rate). The system is asked for the cap's refresh range
   (OH_NativeVSync_SetExpectedFrameRateRange, API 20, resolved at run time). Any failure: the headless 60 Hz thread. */
#include <dlfcn.h>
#include <native_vsync/native_vsync.h>
#include <math.h>
#include <time.h>
static OH_NativeVSync *g_vs;
static unsigned g_vs_div = 1, g_vs_count;
static void vs_frame(long long timestamp, void *data) {
    (void)timestamp; (void)data;
    if (++g_vs_count % g_vs_div == 0) signal_vsync();
    if (OH_NativeVSync_RequestFrame(g_vs, vs_frame, NULL)) {
        static int said; if (!said++) fprintf(stderr, "[totk-ohos] vsync: RequestFrame failed; guest vsync stops\n");
    }
}
static int android_display_vsync_start(void) {
    const char *e = getenv("TOTK_OHOS_FPS_CAP"); int cap = e && *e ? atoi(e) : 30;
    if (cap == 90 || cap == 120) cap = 60;   /* totk-ohos: 90 / 120 left the settings page (2026-11-15) */
    if (cap != 30 && cap != 60) cap = 30;
    /* 30 with the shipping frame controller: the game's own interval 2 at 60 Hz. With the 60 FPS patch (always in 1.4.3's
       embedded code, game_patches_v143.c) the game takes every vsync, so the guest vsync runs at the cap itself. */
    const char *f60 = getenv("TOTK_FPS60"); int fps60 = !(f60 && !strcmp(f60, "0"));
    int guest_hz = fps60 ? cap : 60;
    g_vs = OH_NativeVSync_Create("totk-vsync", 10);
    if (!g_vs) { fprintf(stderr, "[totk-ohos] vsync: OH_NativeVSync_Create failed: headless 60 Hz timer\n"); return 0; }
    typedef int (*set_range_fn)(OH_NativeVSync *, OH_NativeVSync_ExpectedRateRange *);
    set_range_fn set_range = (set_range_fn)dlsym(RTLD_DEFAULT, "OH_NativeVSync_SetExpectedFrameRateRange");
    /* TOTK_OHOS_DISPLAY_HZ (experiment): ask the panel for this refresh rate (e.g. 120) instead of the cap's own. The guest
       vsync still runs at the cap (divided down); a frame that misses its slot is then shown at the next 8.3 ms boundary
       instead of the next 16.7 ms one (a 40 ms frame: 41.7 ms instead of 50). */
    const char *dh = getenv("TOTK_OHOS_DISPLAY_HZ"); int panel_hz = dh && *dh ? atoi(dh) : 0;
    if (panel_hz < guest_hz || panel_hz > 144) panel_hz = guest_hz;
    OH_NativeVSync_ExpectedRateRange range = {30, panel_hz, panel_hz};
    int rr = set_range ? set_range(g_vs, &range) : -1;
    long long period = 0; int pr = OH_NativeVSync_GetPeriod(g_vs, &period);
    double display_hz = !pr && period > 0 ? 1e9 / (double)period : 60.0;
    g_vs_div = display_hz > guest_hz * 1.5 ? (unsigned)(display_hz / guest_hz + 0.5) : 1;
    if (OH_NativeVSync_RequestFrame(g_vs, vs_frame, NULL)) {
        fprintf(stderr, "[totk-ohos] vsync: RequestFrame failed: headless 60 Hz timer\n");
        OH_NativeVSync_Destroy(g_vs); g_vs = NULL; return 0;
    }
    fprintf(stderr, "[totk-ohos] vsync: display %.1f Hz, frame-rate cap %d (guest vsync %d Hz, every %u display vsyncs); expected range 30-%d %s\n",
            display_hz, cap, guest_hz, g_vs_div, panel_hz, set_range ? (rr ? "refused" : "set") : "unavailable (API < 20)");
    return 1;
}

/* ---- controller: the on-screen controls ---- */
static pthread_mutex_t pad_mu = PTHREAD_MUTEX_INITIALIZER;
static RuntimeGamepadState pad;
void runtime_gamepad_start(void) {}
void runtime_gamepad_clear(void) {
    pthread_mutex_lock(&pad_mu); uint64_t c = pad.connected; memset(&pad, 0, sizeof pad); pad.connected = (int)c; pthread_mutex_unlock(&pad_mu);
    ohos_gamepad_clear();
}
/* Swipe camera (components/Controls/SwipeLook.ets, addTouchLook). A stick is a turning speed, so mapping finger
   speed to deflection (the earlier controls) made the camera lag the finger: slow swipes stayed in the game's dead zone
   and its slow range, and a pause shorter than a game frame dropped the turn. Here the finger's distance is kept
   instead (pending, in stick-seconds: the area's height times TOTK_OHOS_LOOK_SPEED, default 1.2) and handed out as the
   game reads the pad: each read in a new game frame (4 ms after the last) gives the deflection that delivers the
   pending distance over the time since the last read, at most full deflection, the rest carried to the next frame (at
   most LOOK_MAX, so the camera never runs on far past the finger). It stops in the frame after the finger stops. The
   deflection is put past the game's radial dead zone (TOTK_OHOS_LOOK_DEADZONE, default 0.12) so small swipes turn too. */
static int32_t touch_axis(float v);
#define LOOK_MAX 0.35
static struct { int id; unsigned epoch; double pending[2]; float out[2]; uint64_t last_ns; } look = {.id = -1};
static float look_speed = -1, look_dz;
static uint64_t look_now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }
static void look_config(void) {
    if (look_speed >= 0) return;
    const char *e = getenv("TOTK_OHOS_LOOK_SPEED"); look_speed = e && *e ? strtof(e, NULL) : 1.2f;
    if (!(look_speed > 0.05f && look_speed < 20)) look_speed = 1.2f;
    e = getenv("TOTK_OHOS_LOOK_DEADZONE"); look_dz = e && *e ? strtof(e, NULL) : 0.12f;
    if (!(look_dz >= 0 && look_dz < 0.6f)) look_dz = 0.12f;
    fprintf(stderr, "[totk-ohos] swipe camera: %.2f stick-seconds per swipe-area height, dead zone %.2f\n", look_speed, look_dz);
}
static void look_read_locked(RuntimeGamepadState *out) {
    if (!look.pending[0] && !look.pending[1] && !look.out[0] && !look.out[1]) return;
    uint64_t t = look_now_ns();
    double dt = look.last_ns ? (double)(t - look.last_ns) / 1e9 : 1.0 / 30;
    if (dt >= 0.004) {   /* a new game frame: the next part of the swipe */
        if (dt > 0.1) dt = 0.1;
        for (int k = 0; k < 2; k++) {
            double s = look.pending[k] / dt; if (s > 1) s = 1; if (s < -1) s = -1;
            look.pending[k] -= s * dt; if (fabs(look.pending[k]) < 1e-4) look.pending[k] = 0;
            look.out[k] = (float)s;
        }
        look.last_ns = t;
    }
    float m = hypotf(look.out[0], look.out[1]);
    if (m <= 0) return;
    float scale = (look_dz + (1 - look_dz) * fminf(m, 1)) / m;
    int32_t rx = touch_axis(look.out[0] * scale), ry = touch_axis(look.out[1] * scale);
    if (abs(rx) > abs(out->rx) || abs(ry) > abs(out->ry)) { out->rx = rx; out->ry = ry; }
}
/* The on-screen controls and any physical controller together: buttons of both, each stick from whichever is pushed
   further, the swipe camera on top. */
void runtime_gamepad_read(RuntimeGamepadState *out) {
    RuntimeGamepadState hw = {0};
    int have_hw = ohos_gamepad_read(&hw);
    pthread_mutex_lock(&pad_mu);
    *out = pad;
    if (have_hw) {
        out->buttons |= hw.buttons; out->connected = 1;
        if (abs(hw.lx) + abs(hw.ly) > abs(out->lx) + abs(out->ly)) { out->lx = hw.lx; out->ly = hw.ly; }
        if (abs(hw.rx) + abs(hw.ry) > abs(out->rx) + abs(out->ry)) { out->rx = hw.rx; out->ry = hw.ry; }
    }
    look_read_locked(out);
    pthread_mutex_unlock(&pad_mu);
}
__attribute__((visibility("hidden"))) void totk_ohos_pad(uint64_t buttons, int32_t lx, int32_t ly, int32_t rx, int32_t ry) {
    pthread_mutex_lock(&pad_mu);
    pad.buttons = buttons; pad.lx = lx; pad.ly = ly; pad.rx = rx; pad.ry = ry; pad.connected = 1;
    pthread_mutex_unlock(&pad_mu);
}

/* Touch pointers of the on-screen controls (the PhoneControls API: one entry per finger, a button mask or a
   stick side, valid for the epoch it was taken in). The pad is their union: buttons OR'd, each stick from its finger. */
#define TOUCH_MAX 16
static struct { int used, id, stick; unsigned epoch; uint64_t mask; float x, y; } touches[TOUCH_MAX];
static unsigned touch_epoch = 1;
static int32_t touch_axis(float v) { if (v > 1) v = 1; if (v < -1) v = -1; return (int32_t)(v * 32767.0f); }
static void touch_merge_locked(void) {
    uint64_t b = 0; float s[2][2] = {{0, 0}, {0, 0}};
    for (int i = 0; i < TOUCH_MAX; i++) {
        if (!touches[i].used) continue;
        if (touches[i].stick < 0) b |= touches[i].mask; else { s[touches[i].stick][0] = touches[i].x; s[touches[i].stick][1] = touches[i].y; }
    }
    pad.buttons = b; pad.lx = touch_axis(s[0][0]); pad.ly = touch_axis(s[0][1]); pad.rx = touch_axis(s[1][0]); pad.ry = touch_axis(s[1][1]);
    pad.connected = 1;
}
static int touch_slot_locked(int id, unsigned epoch, int create) {
    if (epoch != touch_epoch) return -1;
    for (int i = 0; i < TOUCH_MAX; i++) if (touches[i].used && touches[i].id == id) return i;
    if (!create) return -1;
    for (int i = 0; i < TOUCH_MAX; i++) if (!touches[i].used) { memset(&touches[i], 0, sizeof touches[i]); touches[i].used = 1; touches[i].id = id; touches[i].epoch = epoch; touches[i].stick = -1; return i; }
    return -1;
}
__attribute__((visibility("hidden"))) unsigned totk_ohos_touch_epoch(void) { pthread_mutex_lock(&pad_mu); unsigned e = touch_epoch; pthread_mutex_unlock(&pad_mu); return e; }
__attribute__((visibility("hidden"))) int totk_ohos_touch_button(int id, uint64_t mask, int down, unsigned epoch) {
    pthread_mutex_lock(&pad_mu);
    int i = touch_slot_locked(id, epoch, down);
    if (i >= 0) { if (down) { touches[i].stick = -1; touches[i].mask = mask; } else touches[i].used = 0; touch_merge_locked(); }
    pthread_mutex_unlock(&pad_mu);
    return i >= 0 || !down;
}
__attribute__((visibility("hidden"))) int totk_ohos_touch_stick(int id, int side, float x, float y, unsigned epoch) {
    pthread_mutex_lock(&pad_mu);
    int i = side == 0 || side == 1 ? touch_slot_locked(id, epoch, 1) : -1;
    if (i >= 0) { touches[i].stick = side; touches[i].mask = 0; touches[i].x = x; touches[i].y = y; touch_merge_locked(); }
    pthread_mutex_unlock(&pad_mu);
    return i >= 0;
}
__attribute__((visibility("hidden"))) int totk_ohos_touch_look(int id, float dx, float dy, unsigned epoch) {
    if (!isfinite(dx) || !isfinite(dy)) return 0;
    pthread_mutex_lock(&pad_mu);
    int ok = epoch == touch_epoch;
    if (ok) {
        look_config();
        if (look.id != id || look.epoch != epoch) { look.id = id; look.epoch = epoch; }
        if (!look.pending[0] && !look.pending[1] && !look.out[0] && !look.out[1]) look.last_ns = look_now_ns() - 33000000ull;
        look.pending[0] = fmax(-LOOK_MAX, fmin(LOOK_MAX, look.pending[0] + (double)dx * look_speed));
        look.pending[1] = fmax(-LOOK_MAX, fmin(LOOK_MAX, look.pending[1] + (double)dy * look_speed));
    }
    pthread_mutex_unlock(&pad_mu);
    return ok;
}
__attribute__((visibility("hidden"))) int totk_ohos_touch_release(int id, unsigned epoch, int cancel) {
    pthread_mutex_lock(&pad_mu);
    if (id == look.id && epoch == look.epoch) {   /* the swipe finger lifted: the last bit of its distance still turns */
        look.id = -1;
        if (cancel) { look.pending[0] = look.pending[1] = 0; look.out[0] = look.out[1] = 0; }
        pthread_mutex_unlock(&pad_mu);
        return 1;
    }
    int i = touch_slot_locked(id, epoch, 0);
    if (i >= 0) { touches[i].used = 0; touch_merge_locked(); }
    pthread_mutex_unlock(&pad_mu);
    return i >= 0;
}

/* ---- the window ---- */
static pthread_mutex_t win_mu = PTHREAD_MUTEX_INITIALIZER;
static OHNativeWindow *window;
static uint64_t window_epoch;
static int32_t attached_w, attached_h;   /* the size the presenter was given the window at; 0 = not given yet */
/* The game window is landscape: a portrait size is the surface before the orientation change and is not given to the
   presenter (it would size its buffers for it). Any later size change gives the window again: a new attachment, so
   the presenter sizes everything from the new extent (vk_window_resized only refreshes the guest fit). */
static void attach_locked(int32_t width, int32_t height) {
    anw_ohos_set_size((ANativeWindow *)(void *)window, width, height);
    if (width < height) { fprintf(stderr, "[totk-ohos] surface %dx%d is portrait: waiting for the landscape size\n", width, height); return; }
    if (width == attached_w && height == attached_h) return;
    window_epoch = vk_window_set((ANativeWindow *)(void *)window, NULL, 0);   /* the presenter takes its own reference */
    attached_w = width; attached_h = height;
    fprintf(stderr, "[totk-ohos] window given to the presenter at %dx%d\n", width, height);
}
__attribute__((visibility("hidden"))) int totk_ohos_surface(uint64_t surface_id, int32_t width, int32_t height) {
    OHNativeWindow *w = NULL;
    int e = OH_NativeWindow_CreateNativeWindowFromSurfaceId(surface_id, &w);
    if (e || !w) { fprintf(stderr, "[totk-ohos] surface %llu: no native window (%d)\n", (unsigned long long)surface_id, e); return -1; }
    pthread_mutex_lock(&win_mu);
    if (window) { vk_window_set(NULL, NULL, 0); OH_NativeWindow_DestroyNativeWindow(window); }
    window = w; attached_w = attached_h = 0;
    fprintf(stderr, "[totk-ohos] surface %llu, %dx%d\n", (unsigned long long)surface_id, width, height);
    attach_locked(width, height);
    pthread_mutex_unlock(&win_mu);
    return 0;
}
__attribute__((visibility("hidden"))) void totk_ohos_surface_changed(int32_t width, int32_t height) {
    pthread_mutex_lock(&win_mu);
    fprintf(stderr, "[totk-ohos] surface resized %dx%d\n", width, height);
    if (window) attach_locked(width, height);
    pthread_mutex_unlock(&win_mu);
}
__attribute__((visibility("hidden"))) void totk_ohos_surface_destroyed(void) {
    pthread_mutex_lock(&win_mu);
    vk_window_set(NULL, NULL, 0);
    if (window) { OH_NativeWindow_DestroyNativeWindow(window); window = NULL; }
    pthread_mutex_unlock(&win_mu);
    fprintf(stderr, "[totk-ohos] surface detached\n");
}
