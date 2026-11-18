/* Pokémon Legends: Z-A: SDK functions its main imports that this runtime answers on the host (services.c runtime_override
   asks runtime_za_override first). The SDK's own versions would open services this runtime does not serve (csrng, aoc,
   nifm, ldn, bcat, ssl, bsd sockets, capsrv, the time service, hid shared memory for single-state reads) and stop with
   "IPC service not implemented", or read objects their Initialize never made.
   Behaviour and signatures follow nxnative's takeovers for this game (seedds/nxnative, MIT, 028a263: dm/host/hle_*.c,
   dm/games/0100F43008C44000/README.md), mapped onto this runtime's own pieces: hle_bind_system_event /
   hle_signal_system_event (threads.c), the npad readers of input.c and the six-axis / vibration takeovers of
   hid_feedback.c (through runtime_override, so their state stays one). Everything network is offline: initialisation
   succeeds where the game needs it to, every request fails, as on a console without a connection. */
#include "runtime.h"
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <time.h>
#include <unistd.h>

typedef uint32_t Result;
#define RES_OK 0u
#define RES_FAIL 0x2A2u   /* nxnative's generic failure for cut features */
enum { SK_ENETDOWN = 50, SK_EAI_FAIL = 4 };
void hle_bind_system_event(void *ev, int autoclear);
void hle_signal_system_event(void *ev);
void *runtime_override(const char *name);
void *runtime_fs_override(const char *name);   /* romfs.c */

static void lza_log(const char *what) { static unsigned n; if (n++ < 64) fprintf(stderr, "[lza-hle] %s\n", what); }

/* ---- csrng: nnMain's first call ---- */
static void za_random_bytes(void *p, size_t n) {
    uint8_t *b = p;
    while (n) {
        ssize_t r = getrandom(b, n > 256 ? 256 : n, 0);
        if (r <= 0) { for (size_t i = 0; i < n; i++) b[i] = (uint8_t)rand(); return; }
        b += r; n -= (size_t)r;
    }
}

/* ---- aoc / oe / album / err / web / ec ---- */
static void za_aoc_changed_event(void *ev) { hle_bind_system_event(ev, 0); }   /* never signalled; polled with TryWait */
static void za_noop(void) {}
static void za_pseudo_device_id(uint8_t *uuid) { static const uint8_t k[16] = {0x4c, 0x5a, 0x41, 0x00, 0x48, 0x41, 0x52, 0x4d, 0x4f, 0x4e, 0x59, 0x4f, 0x53, 0x31, 0x30, 0x31}; memcpy(uuid, k, 16); }
static unsigned za_false(void) { return 0; }
static Result za_res_ok(void) { return RES_OK; }
static Result za_res_fail(void) { return RES_FAIL; }
static Result za_album_save(int report) { (void)report; lza_log("album::SaveCurrentScreenshot (no album)"); return RES_FAIL; }
static void za_err_app(const void *arg) { (void)arg; lza_log("the game asks to show an application error (error applet)"); }
static void za_err_code(uint64_t code) { fprintf(stderr, "[lza-hle] the game asks to show error %04u-%04u\n", (unsigned)(code & 0xffffffff), (unsigned)(code >> 32)); }
static void za_err_variant(const void *v) { (void)v; lza_log("the game asks to show an error (ErrorResultVariant)"); }
static Result za_web_page(void *rv, const void *arg) { (void)rv; (void)arg; lza_log("web::ShowWebPage (offline)"); return RES_FAIL; }
static void za_shop(const void *user) { (void)user; lza_log("ec::ShowShopAddOnContentList (offline)"); }

/* ---- nifm / nsd / ssl / bcat / ldn / sockets: offline ---- */
static void za_flag(unsigned on) { (void)on; }
static Result za_ip_config(uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d, uint32_t *e) { uint32_t *p[5] = {a, b, c, d, e}; for (int i = 0; i < 5; i++) if (p[i]) *p[i] = 0; return RES_FAIL; }
static Result za_res_fail_p(void *p) { (void)p; return RES_FAIL; }
static void za_env_id(char *out) { memset(out, 0, 8); memcpy(out, "lp1", 3); }
static Result za_res_fail_pp(void *a, const void *b) { (void)a; (void)b; return RES_FAIL; }
static Result za_ssl_certs(void **out, uint8_t *buf, unsigned bn, void *ids, unsigned n) { (void)buf; (void)bn; (void)ids; (void)n; if (out) *out = NULL; return RES_FAIL; }
static Result za_ssl_import(void *self, uint64_t *id) { (void)self; if (id) *id = 0; return RES_FAIL; }
static Result za_ssl_ctx(void *self) { (void)self; return RES_OK; }
static Result za_ldn_scan(void *out, int *n) { (void)out; if (n) *n = 0; return RES_FAIL; }
static Result za_ldn_ipv4(uint32_t *a, uint32_t *m) { if (a) *a = 0; if (m) *m = 0; return RES_FAIL; }
static void za_ldn_state_event(void *ev) { hle_bind_system_event(ev, 0); }
static int za_zero_int(void) { return 0; }
static int za_gai(const char *h, const char *s, const void *hint, void **res) { (void)h; (void)s; (void)hint; if (res) *res = NULL; return SK_EAI_FAIL; }
static const char *za_gai_str(int e) { (void)e; return "network unavailable"; }
static void za_free_ai(void *ai) { (void)ai; }
/* EventFd is a local descriptor of the bsd service and works with no network (nxnative hle_net.c): host counters with
   Horizon's flags (bit 0 reads one at a time, bits 1 and 2 never block). Socket still fails, so a poll only ever has
   these to watch; any other descriptor has nothing to say. An endless wait comes back with 0 after 100 ms. */
enum { EFD_BASE = 0x4000, EFD_MAX = 64, SK_POLLIN = 1, SK_POLLOUT = 4, SK_POLLRDNORM = 0x40, SK_POLLWRNORM = 4, SK_EBADF = 9, SK_EAGAIN = 35 };
typedef struct { int fd; int16_t events, revents; } sk_pollfd;
static struct { uint64_t count; int used, flags; } g_efd[EFD_MAX];
static pthread_mutex_t g_efd_m = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_efd_c = PTHREAD_COND_INITIALIZER;
static _Thread_local int t_skerr = SK_ENETDOWN;
static int za_sk_fail(void) { t_skerr = SK_ENETDOWN; return -1; }
static int za_sk_errno(void) { return t_skerr; }
static int efd_used(int fd) { return fd >= EFD_BASE && fd < EFD_BASE + EFD_MAX && g_efd[fd - EFD_BASE].used; }
static uint64_t za_now_ns(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec; }
static void efd_wait(int64_t ns) {   /* with g_efd_m held */
    struct timespec dl; clock_gettime(CLOCK_REALTIME, &dl);
    uint64_t v = (uint64_t)dl.tv_nsec + (uint64_t)ns; dl.tv_sec += (time_t)(v / 1000000000ull); dl.tv_nsec = (long)(v % 1000000000ull);
    pthread_cond_timedwait(&g_efd_c, &g_efd_m, &dl);
}
static int za_efd_create(uint64_t v, int flags) {
    pthread_mutex_lock(&g_efd_m);
    for (int i = 0; i < EFD_MAX; i++) if (!g_efd[i].used) { g_efd[i].used = 1; g_efd[i].count = v; g_efd[i].flags = flags; pthread_mutex_unlock(&g_efd_m); return EFD_BASE + i; }
    pthread_mutex_unlock(&g_efd_m);
    lza_log("socket: more than 64 event descriptors"); t_skerr = 24; return -1;   /* EMFILE */
}
static int za_sk_close(int fd) {
    pthread_mutex_lock(&g_efd_m);
    int ok = efd_used(fd);
    if (ok) { g_efd[fd - EFD_BASE].used = 0; pthread_cond_broadcast(&g_efd_c); }
    pthread_mutex_unlock(&g_efd_m);
    if (!ok) { t_skerr = SK_EBADF; return -1; }
    return 0;
}
static int za_efd_read(int fd, uint64_t *out) {
    pthread_mutex_lock(&g_efd_m);
    int r = -1;
    for (;;) {
        if (!efd_used(fd)) { t_skerr = SK_EBADF; break; }
        uint64_t *c = &g_efd[fd - EFD_BASE].count; int f = g_efd[fd - EFD_BASE].flags;
        if (*c) { if (f & 1) { if (out) *out = 1; *c -= 1; } else { if (out) *out = *c; *c = 0; } r = 0; break; }
        if (f & 6) { t_skerr = SK_EAGAIN; break; }
        efd_wait(100000000);
    }
    pthread_mutex_unlock(&g_efd_m);
    return r;
}
static int za_efd_write(int fd, uint64_t v) {
    pthread_mutex_lock(&g_efd_m);
    int ok = efd_used(fd);
    if (ok) { g_efd[fd - EFD_BASE].count += v; pthread_cond_broadcast(&g_efd_c); }
    pthread_mutex_unlock(&g_efd_m);
    if (!ok) { t_skerr = SK_EBADF; return -1; }
    return 0;
}
static int efd_ready(sk_pollfd *p, uint64_t n) {   /* with g_efd_m held */
    int ready = 0;
    for (uint64_t i = 0; i < n; i++) {
        p[i].revents = 0;
        if (p[i].fd < EFD_BASE || p[i].fd >= EFD_BASE + EFD_MAX) continue;
        if (!efd_used(p[i].fd)) { p[i].revents = 0x20; ready++; continue; }   /* POLLNVAL: closed while waiting */
        if (g_efd[p[i].fd - EFD_BASE].count) p[i].revents |= p[i].events & (SK_POLLIN | SK_POLLRDNORM);
        p[i].revents |= p[i].events & (SK_POLLOUT | SK_POLLWRNORM);
        if (p[i].revents) ready++;
    }
    return ready;
}
static int za_sk_poll(void *fds, uint64_t n, int timeout) {
    sk_pollfd *p = fds;
    int64_t limit = (timeout < 0 ? 100 : (int64_t)timeout) * 1000000;
    uint64_t start = za_now_ns();
    pthread_mutex_lock(&g_efd_m);
    int ready;
    while (!(ready = efd_ready(p, n))) {
        int64_t left = limit - (int64_t)(za_now_ns() - start);
        if (left <= 0) break;
        efd_wait(left);
    }
    pthread_mutex_unlock(&g_efd_m);
    return ready;
}

/* ---- account: online requests finish at once and fail ---- */
static Result za_acc_event(void *self, void *ev) { (void)self; hle_bind_system_event(ev, 0); hle_signal_system_event(ev); return RES_OK; }
static Result za_acc_done(void *self, uint8_t *done) { (void)self; if (done) *done = 1; return RES_OK; }
static Result za_acc_self_ok(void *self) { (void)self; return RES_OK; }
static Result za_acc_self_fail(void *self) { (void)self; return RES_FAIL; }

/* ---- time ---- */
typedef struct { int16_t year; int8_t month, day, hour, minute, second; int8_t pad; } CalendarTime;
static Result za_to_posix(int *count, int64_t *out, int n, const CalendarTime *ct) {
    struct tm tm = {.tm_year = ct->year - 1900, .tm_mon = ct->month - 1, .tm_mday = ct->day, .tm_hour = ct->hour, .tm_min = ct->minute, .tm_sec = ct->second, .tm_isdst = -1};
    time_t t = mktime(&tm);
    if (count) *count = n > 0;
    if (n > 0 && out) out[0] = (int64_t)t;
    return RES_OK;
}
static Result za_to_posix_rule(int *count, int64_t *out, int n, const CalendarTime *ct, const void *rule) { (void)rule; return za_to_posix(count, out, n, ct); }
static Result za_load_rule(void *rule, const char *name) { (void)rule; (void)name; lza_log("time::LoadTimeZoneRule: the device's time zone is used instead"); return RES_OK; }
static Result za_location(char *out) { memset(out, 0, 0x24); tzset(); snprintf(out, 0x24, "%s", tzname[0] && *tzname[0] ? tzname[0] : "UTC"); return RES_OK; }
static void *g_netclock_event;   /* the task's finish event, bound when the game asks for it */
static Result za_netclock_init(void *self, int mode) { (void)self; (void)mode; return RES_OK; }
static void *za_netclock_finish_event(void *self) {
    (void)self;
    if (!g_netclock_event) { g_netclock_event = calloc(1, 0x40); hle_bind_system_event(g_netclock_event, 0); }
    hle_signal_system_event(g_netclock_event); return g_netclock_event;
}
static Result za_netclock_start(void *self) { (void)self; if (g_netclock_event) hle_signal_system_event(g_netclock_event); return RES_OK; }
static void za_self_void(void *self) { (void)self; }

/* ---- hid: single-state reads through this runtime's npad readers (input.c), defaults as nxnative ---- */
typedef int (*States)(void *out, int max, const uint32_t *id);
static void one_state(const char *states, void *out, const uint32_t *id) {
    States f = (States)runtime_override(states);
    if (!f) runtime_fail("npad state reader missing");
    if (f(out, 1, id) < 1) memset(out, 0, 40);
}
static void za_npad_fullkey(void *s, const uint32_t *id) { one_state("_ZN2nn3hid13GetNpadStatesEPNS0_16NpadFullKeyStateEiRKj", s, id); }
static void za_npad_handheld(void *s, const uint32_t *id) { one_state("_ZN2nn3hid13GetNpadStatesEPNS0_17NpadHandheldStateEiRKj", s, id); }
static void za_npad_joydual(void *s, const uint32_t *id) { one_state("_ZN2nn3hid13GetNpadStatesEPNS0_16NpadJoyDualStateEiRKj", s, id); }
static void za_npad_joyleft(void *s, const uint32_t *id) { one_state("_ZN2nn3hid13GetNpadStatesEPNS0_16NpadJoyLeftStateEiRKj", s, id); }
static void za_npad_joyright(void *s, const uint32_t *id) { one_state("_ZN2nn3hid13GetNpadStatesEPNS0_17NpadJoyRightStateEiRKj", s, id); }
static unsigned g_hold_type = 1, g_comm_mode, g_hh_mode;   /* hold horizontal; the game sets them */
static int za_hold_type(void) { return (int)g_hold_type; }
static void za_ptr_void(const void *p) { (void)p; }
static void za_ptr2_void(const void *a, const void *b) { (void)a; (void)b; }
static uint8_t za_led(const uint32_t *id) { static const uint8_t led[8] = {0x1, 0x3, 0x7, 0xf, 0x9, 0x5, 0xd, 0x6}; return *id < 8 ? led[*id] : 1; }
static int za_joy_assignment(const uint32_t *id) { (void)id; return 0; }
static void za_touch_init(int a, int b) { (void)a; (void)b; }
static Result za_color1(uint32_t *c, const uint32_t *id) { (void)id; c[0] = 0xff2d2d2d; c[1] = 0xffe6e6e6; return RES_OK; }   /* main, buttons */
static Result za_color2(uint32_t *l, uint32_t *r, const uint32_t *id) { (void)id; za_color1(l, id); za_color1(r, id); return RES_OK; }
static Result za_merge(const void *a, const void *b) { (void)a; (void)b; return RES_OK; }
static int za_comm_mode(void) { return (int)g_comm_mode; }
static void za_set_comm_mode(int m) { g_comm_mode = (unsigned)m; }
static uint32_t za_supported_styles(void) { return 0x7f; }
static Result za_strap_guide(void) { lza_log("hid::ShowControllerStrapGuide (no applet)"); return RES_OK; }
static void za_fusion_enable(const uint32_t *h, uint32_t on) { (void)h; (void)on; }
static int za_zero_drift(const uint32_t *h) { (void)h; return 1; }
static void za_style_event(const uint32_t *id, void *ev, int clear) { (void)id; hle_bind_system_event(ev, clear); hle_signal_system_event(ev); }
static unsigned za_fusion_enabled(const uint32_t *h) { (void)h; return 0; }
static int za_hh_mode(void) { return (int)g_hh_mode; }
static void za_set_hh_mode(int m) { g_hh_mode = (unsigned)m; }
static int za_single_assign3(uint32_t *out, const uint32_t *id, int t) { (void)out; (void)id; (void)t; return 0; }
static void za_single_assign2(const uint32_t *id, int t) { (void)id; (void)t; }
static void za_fusion_params(float *a, float *b, const uint32_t *h) { (void)h; *a = 0; *b = 0; }
static void za_set_fusion_params(const uint32_t *h, float a, float b) { (void)h; (void)a; (void)b; }
static unsigned za_at_rest(const uint32_t *h) { (void)h; return 1; }
/* six-axis and vibration: the public wrappers forward to hid_feedback.c's nn::hid::detail takeovers */
typedef void (*SixOne)(void *, const uint32_t *);
typedef int (*SixMany)(int *, void *, int, const uint32_t *);
typedef void (*SixOnOff)(const uint32_t *);
typedef void (*VibSend)(const uint32_t *, const void *);
static void za_six_start(const uint32_t *h) { SixOnOff f = (SixOnOff)runtime_override("_ZN2nn3hid6detail18StartSixAxisSensorERKNS0_19SixAxisSensorHandleE"); if (f) f(h); }
static void za_six_stop(const uint32_t *h) { SixOnOff f = (SixOnOff)runtime_override("_ZN2nn3hid6detail17StopSixAxisSensorERKNS0_19SixAxisSensorHandleE"); if (f) f(h); }
static void za_six_state(void *s, const uint32_t *h) {
    SixOne f = (SixOne)runtime_override("_ZN2nn3hid6detail21GetSixAxisSensorStateEPNS0_18SixAxisSensorStateERKNS0_19SixAxisSensorHandleE");
    if (f) f(s, h); else memset(s, 0, 0x60);
}
static int za_six_states(void *out, int n, const uint32_t *h) {
    SixMany f = (SixMany)runtime_override("_ZN2nn3hid6detail22GetSixAxisSensorStatesEPiPNS0_18SixAxisSensorStateEiRKNS0_19SixAxisSensorHandleE");
    int got = 0; if (f) f(&got, out, n, h); return got;
}
static void za_vib_init(const uint32_t *h) { SixOnOff f = (SixOnOff)runtime_override("_ZN2nn3hid6detail25InitializeVibrationDeviceERKNS0_21VibrationDeviceHandleE"); if (f) f(h); }
static void za_vib_send(const uint32_t *h, const void *v) { VibSend f = (VibSend)runtime_override("_ZN2nn3hid6detail18SendVibrationValueERKNS0_21VibrationDeviceHandleERKNS0_14VibrationValueE"); if (f) f(h, v); }

/* ---- ZA_CAM_SCAN=<seconds> (diagnostic, for the wide-screen camera sites): that long after start, the guest's readable
   anonymous memory is searched for Trinity camera parameters (the field of view in radians at +0x1c, 16:9 at +0x20, a
   view matrix with unit rows at +0x50: the layout main+0x2a73a20 reads) and each candidate is logged; with
   TOTK_WATCH_READ=1 the first one's ratio is then watched for ~3 s (gfx_bridge.c, armed here: this backend has no
   present tick for it), which logs every instruction reading or writing it. */
void runtime_watch_set(uintptr_t addr, size_t len);
void runtime_watch_tick(uint64_t frame);
static int cam_rows_ok(const float *m) {
    for (int r = 0; r < 3; r++) { float l = m[4 * r] * m[4 * r] + m[4 * r + 1] * m[4 * r + 1] + m[4 * r + 2] * m[4 * r + 2]; if (!(l > 0.81f && l < 1.21f)) return 0; }
    return 1;
}
static void *za_cam_scan(void *arg) {
    sleep((unsigned)(uintptr_t)arg);
    FILE *f = fopen("/proc/self/maps", "r"); if (!f) { lza_log("ZA_CAM_SCAN: no /proc/self/maps"); return NULL; }
    char line[512]; uint64_t found[16]; int nfound = 0; uint64_t scanned = 0;
    while (fgets(line, sizeof line, f) && nfound < 16) {
        unsigned long long lo, hi; char perm[8] = {0}, path[256] = {0};
        if (sscanf(line, "%llx-%llx %7s %*s %*s %*s %255[^\n]", &lo, &hi, perm, path) < 3) continue;
        if (perm[0] != 'r' || perm[1] != 'w' || lo < 0x1600000000ull || hi > 0x2800000000ull) continue;
        if (strstr(path, "dmabuf") || strstr(path, "/dev/")) continue;
        scanned += hi - lo;
        for (uint64_t p = lo + 0x20; p + 0x90 <= hi && nfound < 16; p += 4) {
            if (*(const uint32_t *)(uintptr_t)p != 0x3fe38e39u) continue;   /* 16/9 */
            const float *c = (const float *)(uintptr_t)(p - 0x20);
            if (!(c[7] > 0.2f && c[7] < 2.2f) || !cam_rows_ok(c + 20)) continue;
            found[nfound++] = p - 0x20;
            fprintf(stderr, "[lza-cam] camera candidate %#llx: +0x10 %g %g %g %g +0x20 %g %g %g %g\n", (unsigned long long)(p - 0x20),
                    c[4], c[5], c[6], c[7], c[8], c[9], c[10], c[11]);
        }
    }
    fclose(f);
    fprintf(stderr, "[lza-cam] scanned %.0f MB: %d camera candidate(s)\n", scanned / 1048576.0, nfound);
    if (!nfound || !getenv("TOTK_WATCH_READ")) return NULL;
    runtime_watch_set((uintptr_t)found[0] + 0x20, 4);
    for (int i = 0; i < 60; i++) { runtime_watch_tick(1000000 + (uint64_t)i); usleep(50000); }
    fprintf(stderr, "[lza-cam] watch of %#llx done\n", (unsigned long long)found[0] + 0x20);
    return NULL;
}
/* started from runtime_za_override, which the loader asks while linking main: after totk_napi.c has read the env files */
static void za_cam_scan_maybe(void) {
    static int started; const char *e = getenv("ZA_CAM_SCAN"); int s = e && *e ? atoi(e) : 0;
    if (started || s <= 0) return;
    started = 1; pthread_t t; if (!pthread_create(&t, NULL, za_cam_scan, (void *)(uintptr_t)s)) pthread_detach(t);
}

/* ---- audio device names ---- */
/* The first calls of each are logged with their caller: whether the game's sound start-up (Wwise's AudioOut sink,
   main+0xa975b4, opens audout:u) gets this far. */
void runtime_describe_address(uint64_t a, char *out, size_t n);
static void za_audio_seen(const char *what, void *lr) {
    static _Atomic unsigned n; if (n++ >= 12) return;
    char at[96]; runtime_describe_address((uint64_t)(uintptr_t)lr, at, sizeof at);
    fprintf(stderr, "[lza-audio] %s from %s\n", what, at);
}
static void za_devname(char *name) { za_audio_seen("active audio device name", __builtin_return_address(0)); memset(name, 0, 0x100); snprintf(name, 0x100, "AudioStereoJackOutput"); }
static int za_two(void) { za_audio_seen("active output channel count", __builtin_return_address(0)); return 2; }
/* InitializeAudioOutParameter: the SDK's own body (sdk+0x1e7290: rate 0, channels 0, reserved 0xcafe), plus a log of
   who calls it. Wwise's AudioOut sink calls it from Init (main+0xa97608), before its channel test and OpenDefaultAudioOut:
   its this (x20 there, untouched until here) holds the output's channel config (+0xc) and sample rate (+0x24). The
   stub hands x20, the caller's return address and frame on to the C half. */
__attribute__((used)) void za_aout_param_c(uint64_t *p, uint64_t self, uint64_t lr, uint64_t fp) {
    *p = 0xcafe000000000000ull;
    char at[96]; runtime_describe_address(lr, at, sizeof at);
    static _Atomic unsigned n; if (n++ >= 4) return;
    char chain[512]; size_t o = 0; chain[0] = 0;
    for (int i = 0; i < 6 && fp && !(fp & 7); i++) {
        uint64_t f[2]; memcpy(f, (const void *)(uintptr_t)fp, 16);
        char a[96]; runtime_describe_address(f[1], a, sizeof a);
        int k = snprintf(chain + o, sizeof chain - o, " <- %s", a); if (k < 0 || (size_t)k >= sizeof chain - o) break; o += (size_t)k;
        fp = f[0];
    }
    if (strstr(at, "main+0xa9760c")) {
        uint32_t cfg, rate; memcpy(&cfg, (const void *)(uintptr_t)(self + 0xc), 4); memcpy(&rate, (const void *)(uintptr_t)(self + 0x24), 4);
        fprintf(stderr, "[lza-audio] Wwise AudioOut sink Init: channel config %#x (channel mask %#x, %u channels), rate %u%s\n",
                cfg, cfg >> 12, cfg & 0xff, rate, chain);
    } else fprintf(stderr, "[lza-audio] AudioOut parameter from %s%s\n", at, chain);
}
__attribute__((naked)) static void za_aout_param(void) {   /* (AudioOutParameter *p) in x0 */
    __asm__ volatile("mov x1, x20\n\tmov x2, x30\n\tmov x3, x29\n\tb za_aout_param_c\n");
}
static int g_device_notify;
static Result za_device_notify(void *ev) {
    za_audio_seen(g_device_notify ? "audio device notification (again: 0x499)" : "audio device notification", __builtin_return_address(0));
    if (g_device_notify++) return 0x499; hle_bind_system_event(ev, 0); return RES_OK;
}

/* ---- more of main's imports that reach a service (second survey, prepared/za/hle-candidates.txt) ---- */
static Result za_mount_rom_name(const char *name) {   /* the overload without cache arguments */
    static Result (*mount)(const char *, void *, uint64_t);
    if (!mount) mount = (Result (*)(const char *, void *, uint64_t))runtime_fs_override("_ZN2nn2fs8MountRomEPKcPvm");
    return mount(name, NULL, 0);
}
static int za_backtrace(uint64_t *out, int n) { (void)out; (void)n; return 0; }
/* ngc profanity filter: the word list is a system save; nothing is profanity, nothing is masked. The class is called
   through its vtable too, so a constructed object gets a host vtable: the SDK's table (9 slots after the two header
   words in this sdk) with each slot matched to the class's methods by address, every other slot answering 0. */
static Result ngc_v_zero(void) { return RES_OK; }
static void ngc_v_none(void *self) { (void)self; }
static Result ngc_v_check(void *self, uint32_t *masks, uint32_t pat, const void **words, uint64_t n) { (void)self; (void)pat; (void)words; for (uint64_t i = 0; masks && i < n; i++) masks[i] = 0; return RES_OK; }
static Result ngc_v_mask(void *self, int *masked, void *text, uint32_t region) { (void)self; (void)text; (void)region; if (masked) *masked = 0; return RES_OK; }
static void *ngc_vtable(void) {
    enum { NV = 9 }; static void *vt[NV + 2]; static int built;
    if (built) return built > 0 ? &vt[2] : NULL;
    void **sdk = runtime_symbol("_ZTVN2nn3ngc15ProfanityFilterE");
    if (!sdk) { built = -1; lza_log("【红】ProfanityFilter: no SDK vtable; objects keep none"); return NULL; }
    static const struct { const char *name; void *fn; } M[] = {
        {"_ZN2nn3ngc15ProfanityFilterD1Ev", (void *)ngc_v_none}, {"_ZN2nn3ngc15ProfanityFilterD0Ev", (void *)ngc_v_none}, {"_ZN2nn3ngc15ProfanityFilter8FinalizeEv", (void *)ngc_v_none},
        {"_ZN2nn3ngc15ProfanityFilter19CheckProfanityWordsEPjjPPKcm", (void *)ngc_v_check}, {"_ZN2nn3ngc15ProfanityFilter19CheckProfanityWordsEPjjPPKDsm", (void *)ngc_v_check},
        {"_ZN2nn3ngc15ProfanityFilter19CheckProfanityWordsEPjjPPKtm", (void *)ngc_v_check},
        {"_ZN2nn3ngc15ProfanityFilter24MaskProfanityWordsInTextEPiPcj", (void *)ngc_v_mask}, {"_ZN2nn3ngc15ProfanityFilter24MaskProfanityWordsInTextEPiPDsj", (void *)ngc_v_mask},
        {"_ZN2nn3ngc15ProfanityFilter24MaskProfanityWordsInTextEPiPtj", (void *)ngc_v_mask},
    };
    vt[0] = sdk[0]; vt[1] = sdk[1];
    int named = 0;
    for (int n = 0; n < NV; n++) {
        vt[2 + n] = (void *)ngc_v_zero;
        for (unsigned k = 0; k < sizeof M / sizeof *M; k++) { void *a = runtime_symbol(M[k].name); if (a && sdk[2 + n] == a) { vt[2 + n] = M[k].fn; named++; break; } }
    }
    built = 1; fprintf(stderr, "[lza-hle] ngc: ProfanityFilter host vtable of %d slots (%d matched)\n", NV, named);
    return &vt[2];
}
static void za_ngc_ctor(void *self) { void *vt = ngc_vtable(); if (vt && self) memcpy(self, &vt, sizeof vt); }
static Result za_ngc_init(void *self, void *work, uint64_t sz) { (void)self; (void)work; (void)sz; return RES_OK; }
static void za_ngc_fini(void *self) { (void)self; }
/* bcat delivery cache: there is none (no network). Objects hold no session. */
static void za_obj_void(void *self) { (void)self; }
static Result za_bcat_open(void *self, const void *name) { (void)self; (void)name; return RES_FAIL; }
static Result za_bcat_read(void *self, uint64_t *out, int64_t off, void *buf, uint64_t n) { (void)self; (void)off; (void)buf; (void)n; if (out) *out = 0; return RES_FAIL; }
static int64_t za_bcat_size(void *self) { (void)self; return 0; }
static Result za_bcat_dir_read(void *self, int *out, void *e, int n) { (void)self; (void)e; (void)n; if (out) *out = 0; return RES_FAIL; }
static Result za_bcat_enum(int *out, void *names, int n) { (void)names; (void)n; if (out) *out = 0; return RES_OK; }
static Result za_bcat_sync(void *progress) { (void)progress; return RES_FAIL; }
/* account: network service licence and account, out of scope (none) */
static Result za_acc_license_async(void *ctx, const void *user) { (void)ctx; (void)user; return RES_FAIL; }
static int za_acc_license_kind(void *ctx) { (void)ctx; return 0; }
static void *za_acc_move(void *self, void *other) { (void)other; return self; }
static Result za_acc_nsa_id(void *id, const void *h) { (void)id; (void)h; return RES_FAIL; }
static Result za_acc_nsa_avail(uint8_t *out, const void *h) { (void)h; if (out) *out = 0; return RES_OK; }
static Result za_acc_fail_h(const void *h) { (void)h; return RES_FAIL; }
static Result za_acc_token(uint64_t *n, char *buf, uint64_t sz, const void *h) { (void)buf; (void)sz; (void)h; if (n) *n = 0; return RES_FAIL; }
static Result za_acc_fail_ctx(void *ctx, const void *h) { (void)ctx; (void)h; return RES_FAIL; }
static int za_sk_ok(void) { return 0; }

static const struct { const char *name; void *fn; } za_more[] = {
    {"_ZN2nn2fs8MountRomEPKc", (void *)za_mount_rom_name},
    {"_ZN2nn4diag12GetBacktraceEPmi", (void *)za_backtrace},
    {"_ZN2nn3ngc15ProfanityFilterC1Ev", (void *)za_ngc_ctor}, {"_ZN2nn3ngc15ProfanityFilter10InitializeEPvm", (void *)za_ngc_init},
    {"_ZN2nn3ngc15ProfanityFilter8FinalizeEv", (void *)za_ngc_fini},
    {"_ZN2nn6socket7EventFdEmNS0_12EventFdFlagsE", (void *)za_efd_create}, {"_ZN2nn6socket11EventFdReadEiPm", (void *)za_efd_read},
    {"_ZN2nn6socket12EventFdWriteEim", (void *)za_efd_write}, {"_ZN2nn6socket5CloseEi", (void *)za_sk_close},
    {"_ZN2nn6socket6SocketENS0_6FamilyENS0_4TypeENS0_8ProtocolE", (void *)za_sk_fail}, {"_ZN2nn6socket4BindEiPKNS0_8SockAddrEj", (void *)za_sk_fail},
    {"_ZN2nn6socket10SetSockOptEiNS0_5LevelENS0_6OptionEPKvj", (void *)za_sk_ok},
    {"_ZN2nn4bcat17DeliveryCacheFileC1Ev", (void *)za_obj_void}, {"_ZN2nn4bcat17DeliveryCacheFileD1Ev", (void *)za_obj_void},
    {"_ZN2nn4bcat17DeliveryCacheFile4OpenEPKc", (void *)za_bcat_open}, {"_ZN2nn4bcat17DeliveryCacheFile4ReadEPmlPvm", (void *)za_bcat_read},
    {"_ZN2nn4bcat17DeliveryCacheFile5CloseEv", (void *)za_obj_void}, {"_ZN2nn4bcat17DeliveryCacheFile7GetSizeEv", (void *)za_bcat_size},
    {"_ZN2nn4bcat21DeliveryCacheProgressC1Ev", (void *)za_obj_void}, {"_ZN2nn4bcat21DeliveryCacheProgressD1Ev", (void *)za_obj_void},
    {"_ZN2nn4bcat21DeliveryCacheProgress6UpdateEv", (void *)za_obj_void},
    {"_ZN2nn4bcat22DeliveryCacheDirectoryC1Ev", (void *)za_obj_void}, {"_ZN2nn4bcat22DeliveryCacheDirectoryD1Ev", (void *)za_obj_void},
    {"_ZN2nn4bcat22DeliveryCacheDirectory4OpenEPKc", (void *)za_bcat_open}, {"_ZN2nn4bcat22DeliveryCacheDirectory4OpenERKNS0_13DirectoryNameE", (void *)za_bcat_open},
    {"_ZN2nn4bcat22DeliveryCacheDirectory4ReadEPiPNS0_27DeliveryCacheDirectoryEntryEi", (void *)za_bcat_dir_read},
    {"_ZN2nn4bcat22DeliveryCacheDirectory5CloseEv", (void *)za_obj_void},
    {"_ZN2nn4bcat24RequestSyncDeliveryCacheEPNS0_21DeliveryCacheProgressE", (void *)za_bcat_sync},
    {"_ZN2nn4bcat31EnumerateDeliveryCacheDirectoryEPiPNS0_13DirectoryNameEi", (void *)za_bcat_enum},
    {"_ZN2nn7account12AsyncContextD1Ev", (void *)za_obj_void}, {"_ZN2nn7account12AsyncContextD2Ev", (void *)za_obj_void},
    {"_ZN2nn7account12AsyncContextC1Ev", (void *)za_obj_void}, {"_ZN2nn7account12AsyncContextaSEOS1_", (void *)za_acc_move},
    {"_ZN2nn7account34LoadNetworkServiceLicenseInfoAsyncEPNS0_37AsyncNetworkServiceLicenseInfoContextERKNS0_10UserHandleE", (void *)za_acc_license_async},
    {"_ZN2nn7account37AsyncNetworkServiceLicenseInfoContext28GetNetworkServiceLicenseKindEv", (void *)za_acc_license_kind},
    {"_ZN2nn7account37AsyncNetworkServiceLicenseInfoContextC1Ev", (void *)za_obj_void},
    {"_ZN2nn7account37AsyncNetworkServiceLicenseInfoContextC1EOS1_", (void *)za_acc_move},
    {"_ZN2nn7account37AsyncNetworkServiceLicenseInfoContextaSEOS1_", (void *)za_acc_move},
    {"_ZN2nn7account26GetNetworkServiceAccountIdEPNS0_23NetworkServiceAccountIdERKNS0_10UserHandleE", (void *)za_acc_nsa_id},
    {"_ZN2nn7account32IsNetworkServiceAccountAvailableEPbRKNS0_10UserHandleE", (void *)za_acc_nsa_avail},
    {"_ZN2nn7account36EnsureNetworkServiceAccountAvailableERKNS0_10UserHandleE", (void *)za_acc_fail_h},
    {"_ZN2nn7account37LoadNetworkServiceAccountIdTokenCacheEPmPcmRKNS0_10UserHandleE", (void *)za_acc_token},
    {"_ZN2nn7account40ShowLicenseRequirementsForNetworkServiceERKNS0_10UserHandleE", (void *)za_acc_fail_h},
    {"_ZN2nn7account44EnsureNetworkServiceAccountIdTokenCacheAsyncEPNS0_12AsyncContextERKNS0_10UserHandleE", (void *)za_acc_fail_ctx},
};

void *runtime_za_override(const char *name) {
    za_cam_scan_maybe();
    static const struct { const char *name; void *fn; } entries[] = {
        {"_ZN2nn6crypto36GenerateCryptographicallyRandomBytesEPvm", (void *)za_random_bytes},
        {"_ZN2nn3aoc31GetAddOnContentListChangedEventEPNS_2os11SystemEventE", (void *)za_aoc_changed_event},
        {"_ZN2nn2pl21RequestSharedFontLoadENS0_14SharedFontTypeE", (void *)za_noop},   /* shared_font.c loads them at the first read */
        {"_ZN2nn2oe16DisableRecordingEv", (void *)za_noop}, {"_ZN2nn2oe16EnableScreenShotEv", (void *)za_noop}, {"_ZN2nn2oe17DisableScreenShotEv", (void *)za_noop},
        /* The SDK's versions go through am::GetSelfController / GetApplicationFunctions, which this runtime never makes:
           the first save in Lumiose (the station, after the opening) entered the exit-request section and read through a
           NULL controller (SIGSEGV at sdk+0x3f7230). These three are all of main's imports that reach an am controller
           getter and had no takeover (an offline scan of the SDK's PLT calls); saving needs no exit protection here. */
        {"_ZN2nn2oe31EnterExitRequestHandlingSectionEv", (void *)za_noop}, {"_ZN2nn2oe31LeaveExitRequestHandlingSectionEv", (void *)za_noop},
        {"_ZN2nn2oe15EnableRecordingEv", (void *)za_noop},
        /* hid settings the SDK would send to the hid service; TOTK's runtime takes them over for Breath of the Wild only */
        {"_ZN2nn3hid25SetGyroscopeZeroDriftModeERKNS0_19SixAxisSensorHandleERKNS0_22GyroscopeZeroDriftModeE", (void *)za_noop},
        {"_ZN2nn3hid28SetNpadJoyAssignmentModeDualERKj", (void *)za_noop},
        {"_ZN2nn2oe17GetPseudoDeviceIdEPNS_4util4UuidE", (void *)za_pseudo_device_id},
        {"_ZN2nn2oe37IsUserInactivityDetectionTimeExtendedEv", (void *)za_false},
        {"_ZN2nn5album10InitializeEv", (void *)za_noop}, {"_ZN2nn5album8FinalizeEv", (void *)za_noop},
        {"_ZN2nn5album21SaveCurrentScreenshotENS0_17AlbumReportOptionE", (void *)za_album_save},
        {"_ZN2nn3err20ShowApplicationErrorERKNS0_19ApplicationErrorArgE", (void *)za_err_app},
        {"_ZN2nn3err9ShowErrorENS0_9ErrorCodeE", (void *)za_err_code}, {"_ZN2nn3err9ShowErrorERKNS0_18ErrorResultVariantE", (void *)za_err_variant},
        {"_ZN2nn3web11ShowWebPageEPNS0_18WebPageReturnValueERKNS0_14ShowWebPageArgE", (void *)za_web_page},
        {"_ZN2nn2ec24ShowShopAddOnContentListERKNS_7account10UserHandleE", (void *)za_shop},
        {"_ZN2nn4nifm10InitializeEv", (void *)za_res_ok}, {"_ZN2nn4nifm18IsNetworkAvailableEv", (void *)za_false},
        {"_ZN2nn4nifm19SetLocalNetworkModeEb", (void *)za_flag}, {"_ZN2nn4nifm27SubmitNetworkRequestAndWaitEv", (void *)za_noop},
        {"_ZN2nn4nifm22GetCurrentIpConfigInfoEP7in_addrS2_S2_S2_S2_", (void *)za_ip_config},
        {"_ZN2nn4nifm22GetCurrentProxySettingEPNS0_12ProxySettingE", (void *)za_res_fail_p},
        {"_ZN2nn4nifm31HandleNetworkRequestErrorResultEv", (void *)za_false},
        {"_ZN2nn3nsd24GetEnvironmentIdentifierEPNS0_21EnvironmentIdentifierE", (void *)za_env_id},
        {"_ZN2nn3nsd9ResolveExEPNS0_4FqdnERKS1_", (void *)za_res_fail_pp},
        {"_ZN2nn3ssl10InitializeEv", (void *)za_res_ok}, {"_ZN2nn3ssl8FinalizeEv", (void *)za_res_ok},
        {"_ZN2nn3ssl14BuiltInManager22GetBuiltInCertificatesEPPNS0_26BuiltInCertificateInfoBaseEPhjPNS0_15CaCertificateIdEj", (void *)za_ssl_certs},
        {"_ZN2nn3ssl7Context15ImportClientPkiEPmPKcS4_jj", (void *)za_ssl_import}, {"_ZN2nn3ssl7Context15ImportServerPkiEPmPKcjNS0_17CertificateFormatE", (void *)za_ssl_import},
        {"_ZN2nn3ssl7Context9ImportCrlEPmPKcj", (void *)za_ssl_import},
        {"_ZN2nn3ssl7Context6CreateENS1_10SslVersionE", (void *)za_ssl_ctx}, {"_ZN2nn3ssl7Context7DestroyEv", (void *)za_ssl_ctx},
        {"_ZN2nn4bcat10InitializeEv", (void *)za_noop}, {"_ZN2nn4bcat25MountDeliveryCacheStorageEv", (void *)za_res_fail},
        {"_ZN2nn4bcat27UnmountDeliveryCacheStorageEv", (void *)za_noop},
        {"_ZN2nn3ldn10InitializeEv", (void *)za_res_fail}, {"_ZN2nn3ldn8FinalizeEv", (void *)za_noop}, {"_ZN2nn3ldn8GetStateEv", (void *)za_zero_int},
        {"_ZN2nn3ldn10DisconnectEv", (void *)za_res_fail}, {"_ZN2nn3ldn11OpenStationEv", (void *)za_res_fail}, {"_ZN2nn3ldn12CloseStationEv", (void *)za_res_fail},
        {"_ZN2nn3ldn14DestroyNetworkEv", (void *)za_res_fail}, {"_ZN2nn3ldn15OpenAccessPointEv", (void *)za_res_fail}, {"_ZN2nn3ldn16CloseAccessPointEv", (void *)za_res_fail},
        {"_ZN2nn3ldn4ScanEPNS0_11NetworkInfoEPiiRKNS0_10ScanFilterEi", (void *)za_ldn_scan},
        {"_ZN2nn3ldn11ScanPrivateEPNS0_11NetworkInfoEPiiRKNS0_10ScanFilterEi", (void *)za_ldn_scan},
        {"_ZN2nn3ldn14GetIpv4AddressEPNS0_11Ipv4AddressEPNS0_10SubnetMaskE", (void *)za_ldn_ipv4},
        {"_ZN2nn3ldn14GetNetworkInfoEPNS0_11NetworkInfoE", (void *)za_res_fail_p}, {"_ZN2nn3ldn20GetSecurityParameterEPNS0_17SecurityParameterE", (void *)za_res_fail_p},
        {"_ZN2nn3ldn22AttachStateChangeEventEPNS_2os15SystemEventTypeE", (void *)za_ldn_state_event},
        {"_ZN2nn3ldn19GetDisconnectReasonEv", (void *)za_zero_int},
        {"_ZN2nn3ldn13CreateNetworkERKNS0_13NetworkConfigERKNS0_14SecurityConfigERKNS0_10UserConfigE", (void *)za_res_fail},
        {"_ZN2nn3ldn20CreateNetworkPrivateERKNS0_13NetworkConfigERKNS0_14SecurityConfigERKNS0_17SecurityParameterERKNS0_10UserConfigEiPKNS0_12AddressEntryE", (void *)za_res_fail},
        {"_ZN2nn3ldn7ConnectERKNS0_11NetworkInfoERKNS0_14SecurityConfigERKNS0_10UserConfigEiNS0_13ConnectOptionE", (void *)za_res_fail},
        {"_ZN2nn3ldn14ConnectPrivateERKNS0_13NetworkConfigERKNS0_14SecurityConfigERKNS0_17SecurityParameterERKNS0_10UserConfigEiNS0_13ConnectOptionE", (void *)za_res_fail},
        {"_ZN2nn3ldn16SetAdvertiseDataEPKvm", (void *)za_res_fail}, {"_ZN2nn3ldn20AddAcceptFilterEntryENS0_10MacAddressE", (void *)za_res_fail},
        {"_ZN2nn3ldn22SetStationAcceptPolicyENS0_12AcceptPolicyE", (void *)za_res_fail}, {"_ZN2nn3ldn6RejectENS0_11Ipv4AddressE", (void *)za_res_fail},
        {"_ZN2nn6socket10InitializeERKNS0_6ConfigE", (void *)za_res_ok},
        {"_ZN2nn6socket6SocketEiii", (void *)za_sk_fail}, {"_ZN2nn6socket4BindEiPK8sockaddrj", (void *)za_sk_fail}, {"_ZN2nn6socket8ShutdownEii", (void *)za_sk_fail},
        {"_ZN2nn6socket6SendToEiPKvmNS0_7MsgFlagEPKNS0_8SockAddrEj", (void *)za_sk_fail}, {"_ZN2nn6socket6SendToEiPKvmiPK8sockaddrj", (void *)za_sk_fail},
        {"_ZN2nn6socket8RecvFromEiPvmNS0_7MsgFlagEPNS0_8SockAddrEPj", (void *)za_sk_fail}, {"_ZN2nn6socket8RecvFromEiPvmiP8sockaddrPj", (void *)za_sk_fail},
        {"_ZN2nn6socket12GetLastErrnoEv", (void *)za_sk_errno}, {"_ZN2nn6socket12GetLastErrorEv", (void *)za_sk_errno},
        {"_ZN2nn6socket11GetAddrInfoEPKcS2_PK8addrinfoPPS3_", (void *)za_gai}, {"_ZN2nn6socket11GetAddrInfoEPKcS2_PKNS0_8AddrInfoEPPS3_", (void *)za_gai},
        {"_ZN2nn6socket11GAIStrErrorENS0_7AiErrnoE", (void *)za_gai_str},
        {"_ZN2nn6socket12FreeAddrInfoEP8addrinfo", (void *)za_free_ai}, {"_ZN2nn6socket12FreeAddrInfoEPNS0_8AddrInfoE", (void *)za_free_ai},
        {"_ZN2nn6socket4PollEP6pollfdmi", (void *)za_sk_poll},
        {"_ZN2nn7account12AsyncContext14GetSystemEventEPNS_2os11SystemEventE", (void *)za_acc_event},
        {"_ZN2nn7account12AsyncContext6CancelEv", (void *)za_acc_self_ok}, {"_ZN2nn7account12AsyncContext7HasDoneEPb", (void *)za_acc_done},
        {"_ZN2nn7account12AsyncContext9GetResultEv", (void *)za_acc_self_fail},
        {"_ZN2nn4time11ToPosixTimeEPiPNS0_9PosixTimeEiRKNS0_12CalendarTimeERKNS0_12TimeZoneRuleE", (void *)za_to_posix_rule},
        {"_ZN2nn4time11ToPosixTimeEPiPNS0_9PosixTimeEiRKNS0_12CalendarTimeE", (void *)za_to_posix},
        {"_ZN2nn4time16LoadTimeZoneRuleEPNS0_12TimeZoneRuleERKNS0_12LocationNameE", (void *)za_load_rule},
        {"_ZN2nn4time21GetDeviceLocationNameEPNS0_12LocationNameE", (void *)za_location},
        {"_ZN2nn4time39AsyncEnsureNetworkClockAvailabilityTask10InitializeENS_2os14EventClearModeE", (void *)za_netclock_init},
        {"_ZN2nn4time39AsyncEnsureNetworkClockAvailabilityTask14GetFinishEventEv", (void *)za_netclock_finish_event},
        {"_ZN2nn4time39AsyncEnsureNetworkClockAvailabilityTask9GetResultEv", (void *)za_acc_self_fail},
        {"_ZN2nn4time39AsyncEnsureNetworkClockAvailabilityTask9StartTaskEv", (void *)za_netclock_start},
        {"_ZN2nn4time39AsyncEnsureNetworkClockAvailabilityTaskC1Ev", (void *)za_self_void}, {"_ZN2nn4time39AsyncEnsureNetworkClockAvailabilityTaskD1Ev", (void *)za_self_void},
        {"_ZN2nn3hid12GetNpadStateEPNS0_16NpadFullKeyStateERKj", (void *)za_npad_fullkey},
        {"_ZN2nn3hid12GetNpadStateEPNS0_17NpadHandheldStateERKj", (void *)za_npad_handheld},
        {"_ZN2nn3hid12GetNpadStateEPNS0_16NpadJoyDualStateERKj", (void *)za_npad_joydual},
        {"_ZN2nn3hid12GetNpadStateEPNS0_16NpadJoyLeftStateERKj", (void *)za_npad_joyleft},
        {"_ZN2nn3hid12GetNpadStateEPNS0_17NpadJoyRightStateERKj", (void *)za_npad_joyright},
        {"_ZN2nn3hid18GetNpadJoyHoldTypeEv", (void *)za_hold_type},
        {"_ZN2nn3hid18StartSixAxisSensorERKNS0_19SixAxisSensorHandleE", (void *)za_six_start},
        {"_ZN2nn3hid17StopSixAxisSensorERKNS0_19SixAxisSensorHandleE", (void *)za_six_stop},
        {"_ZN2nn3hid21GetSixAxisSensorStateEPNS0_18SixAxisSensorStateERKNS0_19SixAxisSensorHandleE", (void *)za_six_state},
        {"_ZN2nn3hid22GetSixAxisSensorStatesEPNS0_18SixAxisSensorStateEiRKNS0_19SixAxisSensorHandleE", (void *)za_six_states},
        {"_ZN2nn3hid21IsSixAxisSensorAtRestERKNS0_19SixAxisSensorHandleE", (void *)za_at_rest},
        {"_ZN2nn3hid25InitializeVibrationDeviceERKNS0_21VibrationDeviceHandleE", (void *)za_vib_init},
        {"_ZN2nn3hid18SendVibrationValueERKNS0_21VibrationDeviceHandleERKNS0_14VibrationValueE", (void *)za_vib_send},
        {"_ZN2nn3hid18SwapNpadAssignmentERKjS2_", (void *)za_ptr2_void},
        {"_ZN2nn3hid19GetPlayerLedPatternERKj", (void *)za_led}, {"_ZN2nn3hid20GetNpadJoyAssignmentERKj", (void *)za_joy_assignment},
        {"_ZN2nn3hid20StopLrAssignmentModeEv", (void *)za_noop}, {"_ZN2nn3hid21StartLrAssignmentModeEv", (void *)za_noop},
        {"_ZN2nn3hid21InitializeTouchScreenEii", (void *)za_touch_init},
        {"_ZN2nn3hid22GetNpadControllerColorEPNS0_19NpadControllerColorERKj", (void *)za_color1},
        {"_ZN2nn3hid22GetNpadControllerColorEPNS0_19NpadControllerColorES2_RKj", (void *)za_color2},
        {"_ZN2nn3hid23MergeSingleJoyAsDualJoyERKjS2_", (void *)za_merge},
        {"_ZN2nn3hid24GetNpadCommunicationModeEv", (void *)za_comm_mode}, {"_ZN2nn3hid24SetNpadCommunicationModeENS0_21NpadCommunicationModeE", (void *)za_set_comm_mode},
        {"_ZN2nn3hid24GetSupportedNpadStyleSetEv", (void *)za_supported_styles},
        {"_ZN2nn3hid24ShowControllerStrapGuideEv", (void *)za_strap_guide},
        {"_ZN2nn3hid25EnableSixAxisSensorFusionERKNS0_19SixAxisSensorHandleEb", (void *)za_fusion_enable},
        {"_ZN2nn3hid25GetGyroscopeZeroDriftModeERKNS0_19SixAxisSensorHandleE", (void *)za_zero_drift},
        {"_ZN2nn3hid27BindNpadStyleSetUpdateEventERKjPNS_2os15SystemEventTypeENS3_14EventClearModeE", (void *)za_style_event},
        {"_ZN2nn3hid28IsSixAxisSensorFusionEnabledERKNS0_19SixAxisSensorHandleE", (void *)za_fusion_enabled},
        {"_ZN2nn3hid29GetNpadHandheldActivationModeEv", (void *)za_hh_mode}, {"_ZN2nn3hid29SetNpadHandheldActivationModeENS0_26NpadHandheldActivationModeE", (void *)za_set_hh_mode},
        {"_ZN2nn3hid30SetNpadJoyAssignmentModeSingleEPjRKjNS0_17NpadJoyDeviceTypeE", (void *)za_single_assign3},
        {"_ZN2nn3hid30SetNpadJoyAssignmentModeSingleERKj", (void *)za_ptr_void},
        {"_ZN2nn3hid30SetNpadJoyAssignmentModeSingleERKjNS0_17NpadJoyDeviceTypeE", (void *)za_single_assign2},
        {"_ZN2nn3hid32GetSixAxisSensorFusionParametersEPfS1_RKNS0_19SixAxisSensorHandleE", (void *)za_fusion_params},
        {"_ZN2nn3hid32SetSixAxisSensorFusionParametersERKNS0_19SixAxisSensorHandleEff", (void *)za_set_fusion_params},
        {"_ZN2nn5audio27InitializeAudioOutParameterEPNS0_17AudioOutParameterE", (void *)za_aout_param},
        {"_ZN2nn5audio24GetActiveAudioDeviceNameEPNS0_15AudioDeviceNameE", (void *)za_devname},
        {"_ZN2nn5audio30GetActiveAudioOutputDeviceNameEPNS0_15AudioDeviceNameE", (void *)za_devname},
        {"_ZN2nn5audio41GetActiveAudioDeviceChannelCountForOutputEv", (void *)za_two},
        {"_ZN2nn5audio39AcquireAudioDeviceNotificationForOutputEPNS_2os11SystemEventE", (void *)za_device_notify},
    };
    for (size_t i = 0; i < sizeof entries / sizeof *entries; i++) if (!strcmp(name, entries[i].name)) return entries[i].fn;
    for (size_t i = 0; i < sizeof za_more / sizeof *za_more; i++) if (!strcmp(name, za_more[i].name)) return za_more[i].fn;
    return NULL;
}
