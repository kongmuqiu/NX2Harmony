/* HarmonyOS thread placement, after single-core scheduling measurements on a Kirin 9030 Pro (SPEC CPU and Geekbench
   runs). The game is bound by single threads: totk-walk (command
   walker/recorder, ~3/4 of a core in the cave) and g-main; left alone they hop over the little or big cores at ~25%
   each, so no core looks busy and the clusters stay at 560-1000 MHz.

   What this phone does (user's settled practice, SPEC cpu-pinning analysis):
   - plain affinity works for CPUs 0-11; the Prime cores 12-13 sit outside the app's cpuset, a request for them is
     "accepted" but answered with the big set, and hmmac may shrink the cpuset in retaliation;
   - the Prime cores are reached by QoS USER_INTERACTIVE (5) together with uclamp min = max = 1024 (sched_setattr),
     kept up by re-asserting it (the system resets uclamp; SPEC's watchdog re-asserts every 10 ms), and with no narrow
     affinity on the thread (SOFT mode: never sched_setaffinity those threads to a subset).
   So:
   - critical threads (totk-walk, g-main): on the thread itself (ohos_sched_self; OH_QoS_* and sched_setattr(0) act on
     the caller), at most every 10 ms: QoS 5, SCHED_NORMAL nice -20 with uclamp 1024/1024 (KEEP_POLICY|KEEP_PARAMS with
     uclamp alone as the fallback); every second the mask is widened back to the process's allowed set if it was
     narrowed (inherited from a creator). The keeper repeats QoS 5 (libqos's SetQosForOtherThread, as GB does) and the
     sched attributes by tid every 100 ms, for g-main whose own calls stop after its start;
   - other game threads, recorder workers, totk-post/submit/compute and the driver's gpu-work-server: hard affinity to
     the big cores (middle cpu_capacity tier, 4-11), by tid, re-applied every 500 ms; never the little cores.
   Every 10 s the keeper logs where each critical thread ran (last CPU sampled every 100 ms) and its QoS / uclamp.
   TOTK_OHOS_PIN=0: no keeper; TOTK_OHOS_SELF=0: no self maintenance; TOTK_OHOS_CRIT_QOS=0: no QoS on critical threads. */
#define _GNU_SOURCE
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <qos/qos.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    uint32_t size, policy; uint64_t flags; int32_t nice; uint32_t priority;
    uint64_t runtime, deadline, period; uint32_t util_min, util_max;
} sattr;

static unsigned read_uint(const char *path) {
    FILE *f = fopen(path, "r"); unsigned v = 0;
    if (f) { if (fscanf(f, "%u", &v) != 1) v = 0; fclose(f); }
    return v;
}
static void mask_text(const cpu_set_t *s, char *out, size_t n) {
    size_t o = 0; out[0] = 0;
    for (int c = 0; c < 64 && o + 8 < n; c++) if (CPU_ISSET(c, s)) o += (size_t)snprintf(out + o, n - o, "%s%d", o ? "," : "", c);
    if (!o) snprintf(out, n, "none");
}
static uint64_t mono_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }
static int env_off(const char *name) { const char *e = getenv(name); return e && !strcmp(e, "0"); }

/* ---------- topology ---------- */
static pthread_once_t g_topo_once = PTHREAD_ONCE_INIT;
static unsigned g_cap[64]; static int g_ncpu;
static cpu_set_t g_prime, g_big;
static const char *g_cap_source = "cpu_capacity";
static void topo_init(void) {
    g_ncpu = (int)sysconf(_SC_NPROCESSORS_CONF); if (g_ncpu > 64) g_ncpu = 64;
    int known = 1;
    for (int c = 0; c < g_ncpu; c++) {
        char p[96]; snprintf(p, sizeof p, "/sys/devices/system/cpu/cpu%d/cpu_capacity", c);
        g_cap[c] = read_uint(p); known &= g_cap[c] != 0;
    }
    if (!known) {
        g_cap_source = "cpuinfo_max_freq";
        for (int c = 0; c < g_ncpu; c++) {
            char p[96]; snprintf(p, sizeof p, "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", c); g_cap[c] = read_uint(p);
        }
    }
    unsigned hi = 0, lo = ~0u;
    for (int c = 0; c < g_ncpu; c++) if (g_cap[c]) { if (g_cap[c] > hi) hi = g_cap[c]; if (g_cap[c] < lo) lo = g_cap[c]; }
    CPU_ZERO(&g_prime); CPU_ZERO(&g_big);
    for (int c = 0; c < g_ncpu; c++) if (g_cap[c] && g_cap[c] == hi) CPU_SET(c, &g_prime);
    for (int c = 0; c < g_ncpu; c++) if (g_cap[c] && g_cap[c] > lo && g_cap[c] < hi) CPU_SET(c, &g_big);
    if (!CPU_COUNT(&g_big)) g_big = g_prime;
}

/* ---------- critical threads ---------- */
enum { NCRIT = 4 };
static struct { _Atomic int tid, role; } g_crit[NCRIT];
static void crit_register(int tid, int role) {
    for (int i = 0; i < NCRIT; i++) if (atomic_load(&g_crit[i].tid) == tid) return;
    for (int i = 0; i < NCRIT; i++) { int z = 0; if (atomic_compare_exchange_strong(&g_crit[i].tid, &z, tid)) { atomic_store(&g_crit[i].role, role); return; } }
}

/* QoS for other threads (libqos C++ entry points, as GB's runtime scheduling resolves them) */
typedef int (*qos_set_other)(int level, int tid);
typedef int (*qos_get_other)(int *level, int tid);
static qos_set_other g_qset; static qos_get_other g_qget;
static void qos_other_init(void) {
    void *h = dlopen("libqos.so", RTLD_NOW | RTLD_LOCAL);
    if (h) {
        g_qset = (qos_set_other)dlsym(h, "_ZN4OHOS3QOS20SetQosForOtherThreadENS0_8QosLevelEi");
        g_qget = (qos_get_other)dlsym(h, "_ZN4OHOS3QOS20GetQosForOtherThreadERNS0_8QosLevelEi");
    }
    fprintf(stderr, "[ohos-pin] QoS for other threads: set %s, get %s\n", g_qset ? "found" : "missing", g_qget ? "found" : "missing");
}
static int demand_attr(int tid) {   /* [FIFO 1 +] nice -20 + uclamp 1024/1024, then uclamp alone; 0 ok */
    /* TOTK_OHOS_FIFO=1: SPEC's first tier, SCHED_FIFO priority 1 with uclamp (an RT thread also gets the RT default
       uclamp.min, 1024 on Linux: maximum frequency wherever it runs); refused without RT permission, then the rest. */
    static int fifo = -1; if (fifo < 0) { const char *e = getenv("TOTK_OHOS_FIFO"); fifo = e && !strcmp(e, "1"); }
    if (fifo) {
        sattr r = {.size = sizeof r, .policy = 1 /* SCHED_FIFO */, .priority = 1, .flags = 0x20 | 0x40, .util_min = 1024, .util_max = 1024};
        if (!syscall(SYS_sched_setattr, tid, &r, 0)) return 0;
        static int said; if (!said++) fprintf(stderr, "[ohos-pin] SCHED_FIFO refused (%s): normal policy\n", strerror(errno));
        fifo = 0;
    }
    sattr a = {.size = sizeof a, .policy = 0, .flags = 0x20 | 0x40, .nice = -20, .util_min = 1024, .util_max = 1024};
    if (!syscall(SYS_sched_setattr, tid, &a, 0)) return 0;
    sattr b = {.size = sizeof b, .flags = 0x08 | 0x10 | 0x20 | 0x40, .util_min = 1024, .util_max = 1024};
    return syscall(SYS_sched_setattr, tid, &b, 0) ? errno : 0;
}
static int last_cpu_of(int tid) {   /* field 39 of /proc/self/task/<tid>/stat */
    char p[64], line[1024]; snprintf(p, sizeof p, "/proc/self/task/%d/stat", tid);
    FILE *f = fopen(p, "r"); if (!f) return -1;
    size_t n = fread(line, 1, sizeof line - 1, f); fclose(f); line[n] = 0;
    char *r = strrchr(line, ')'); if (!r) return -1;
    int field = 2; char *save = NULL, *tok = strtok_r(r + 1, " ", &save);
    while (tok && ++field < 39) tok = strtok_r(NULL, " ", &save);
    return tok ? atoi(tok) : -1;
}
static int crit_qos_on(void) { static int v = -1; if (v < 0) v = !env_off("TOTK_OHOS_CRIT_QOS"); return v; }

/* ---------- self maintenance, on the critical thread ---------- */
void ohos_sched_self(int role) {
    static __thread uint64_t last, widened;
    static __thread int my_role;
    static __thread char logged[200];
    static int self_off = -1;
    if (self_off < 0) self_off = env_off("TOTK_OHOS_SELF");
    if (self_off) return;
    uint64_t now = mono_ns();
    if (last && now - last < 10000000ull) return;   /* SPEC's watchdog period */
    last = now;
    if (!my_role) my_role = role;   /* the walker also presents: one thread, one role */
    role = my_role;
    int tid = (int)syscall(SYS_gettid);
    if (!widened) crit_register(tid, role);
    QoS_Level lv = QOS_DEFAULT; int qget = OH_QoS_GetThreadQoS(&lv), qset = 0;
    if (crit_qos_on() && (qget || lv != QOS_USER_INTERACTIVE)) { qset = OH_QoS_SetThreadQoS(QOS_USER_INTERACTIVE); qget = OH_QoS_GetThreadQoS(&lv); }
    int ar = demand_attr(0);   /* every time: the system resets uclamp */
    cpu_set_t m; CPU_ZERO(&m); sched_getaffinity(0, sizeof m, &m);
    int wr = 0;
    if (!widened || now - widened >= 1000000000ull) {   /* SOFT mode: never narrower than the process's allowed set */
        widened = now;
        cpu_set_t proc; CPU_ZERO(&proc);
        if (!sched_getaffinity(getpid(), sizeof proc, &proc) && !CPU_EQUAL(&m, &proc)) {
            wr = sched_setaffinity(0, sizeof proc, &proc) ? errno : 0; CPU_ZERO(&m); sched_getaffinity(0, sizeof m, &m);
        }
    }
    sattr cur = {.size = sizeof cur}; long gr = syscall(SYS_sched_getattr, 0, &cur, sizeof cur, 0);
    char mt[96]; mask_text(&m, mt, sizeof mt);
    char state[200];
    snprintf(state, sizeof state, "qos %d (set %d), attr %s, policy %u prio %u nice %d uclamp %d/%d, mask %s%s%s", qget ? -1 : (int)lv, qset,
             ar ? strerror(ar) : "ok", gr ? 99u : cur.policy, gr ? 0u : cur.priority, gr ? 99 : cur.nice, gr ? -1 : (int)cur.util_min, gr ? -1 : (int)cur.util_max, mt,
             wr ? ", widen " : "", wr ? strerror(wr) : "");
    if (strcmp(state, logged)) {
        char name[32] = "?"; pthread_getname_np(pthread_self(), name, sizeof name);
        static _Atomic int said; if (atomic_fetch_add(&said, 1) < 40)
            fprintf(stderr, "[ohos-self] %s %d role %d on cpu %d: %s\n", name, tid, role, sched_getcpu(), state);
        snprintf(logged, sizeof logged, "%s", state);
    }
}

/* ---------- keeper ---------- */
enum { MAXT = 512 };
static struct { int tid; unsigned repins; } g_work[MAXT];
static int g_nwork;
static int is_worker(const char *name) {
    if (!strcmp(name, "totk-walk") || !strcmp(name, "g-main")) return 0;
    return !strncmp(name, "g-", 2) || !strncmp(name, "totk-recw", 9) || !strcmp(name, "totk-submit") || !strcmp(name, "totk-post") ||
        !strcmp(name, "totk-compute") || !strcmp(name, "gpu-work-server") || !strcmp(name, "totk-present") || !strcmp(name, "totk-gpufence");
}
static int work_slot(int tid) {
    for (int i = 0; i < g_nwork; i++) if (g_work[i].tid == tid) return i;
    if (g_nwork < MAXT) { memset(&g_work[g_nwork], 0, sizeof g_work[0]); g_work[g_nwork].tid = tid; return g_nwork++; }
    return -1;
}
/* g-main is pinned to one big core of its own (affinity works on 0-11): spread over 4-11 at ~25% each it kept the
   big cluster at 558 MHz while the walker waited for it; concentrated, that core's load drives the cluster up.
   TOTK_OHOS_MAIN_CORE=<cpu> picks another, -1 leaves g-main unpinned. */
static int main_core(void) {
    static int c = -2;
    if (c == -2) {
        const char *e = getenv("TOTK_OHOS_MAIN_CORE");
        if (e && *e) c = atoi(e);
        else { c = -1; for (int k = 63; k >= 0; k--) if (CPU_ISSET(k, &g_big)) { c = k; break; } }
    }
    return c;
}
static void keep_workers(void) {
    DIR *d = opendir("/proc/self/task"); if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        int tid = atoi(e->d_name); if (tid <= 0) continue;
        char p[64], name[32] = ""; snprintf(p, sizeof p, "/proc/self/task/%d/comm", tid);
        FILE *f = fopen(p, "r"); if (!f) continue;
        if (!fgets(name, sizeof name, f)) name[0] = 0;
        fclose(f); name[strcspn(name, "\n")] = 0;
        int crit = 0; for (int i = 0; i < NCRIT; i++) if (atomic_load(&g_crit[i].tid) == tid) crit = 1;
        if (crit || !is_worker(name)) continue;
        int i = work_slot(tid); if (i < 0) continue;
        /* The recorder workers (~40% of a core each) get a big core each too, below g-main's: one busy core per thread
           keeps the cluster's frequency up instead of four quarter-busy ones. The rest share what is left. */
        int mc = main_core(), own = -1;
        if (mc >= 0 && !strncmp(name, "totk-recw", 9) && name[9] >= '0' && name[9] <= '9') {
            int c = mc - 1 - (name[9] - '0');
            if (c >= 0 && CPU_ISSET(c, &g_big)) own = c;
        }
        cpu_set_t want; CPU_ZERO(&want);
        if (own >= 0) CPU_SET(own, &want);
        else {
            want = g_big;
            if (mc >= 0) for (int c = mc; c >= mc - 2 && c >= 0; c--) if (CPU_COUNT(&want) > 1) CPU_CLR(c, &want);
        }
        cpu_set_t now; CPU_ZERO(&now); if (sched_getaffinity(tid, sizeof now, &now)) continue;
        if (CPU_EQUAL(&now, &want)) continue;
        int r = sched_setaffinity(tid, sizeof want, &want) ? errno : 0;
        if (g_work[i].repins++ < 2) {
            cpu_set_t got; CPU_ZERO(&got); sched_getaffinity(tid, sizeof got, &got);
            char wt[96], nt[96], gt[96]; mask_text(&want, wt, sizeof wt); mask_text(&now, nt, sizeof nt); mask_text(&got, gt, sizeof gt);
            fprintf(stderr, "[ohos-pin] worker %s %d: %s -> %s, read back %s%s%s\n", name, tid, nt, wt, gt, r ? ", " : "", r ? strerror(r) : "");
        }
    }
    closedir(d);
}
static void *keeper(void *arg) {
    (void)arg;
    pthread_setname_np(pthread_self(), "totk-pin");
    pthread_once(&g_topo_once, topo_init);
    qos_other_init();
    {   char line[512]; size_t o = 0;
        for (int c = 0; c < g_ncpu; c++) o += (size_t)snprintf(line + o, sizeof line - o, " %d:%u", c, g_cap[c]);
        char pt[96], bt[96]; mask_text(&g_prime, pt, sizeof pt); mask_text(&g_big, bt, sizeof bt);
        fprintf(stderr, "[ohos-pin] keeper: %s%s; Prime %s reached by QoS 5 + uclamp 1024 (no narrow affinity), game workers -> %s\n",
                g_cap_source, line, pt, bt);
    }
    unsigned hits[NCRIT][64]; memset(hits, 0, sizeof hits);
    unsigned samples[NCRIT] = {0};
    uint64_t next_log = mono_ns() + 10000000000ull;
    for (unsigned tick = 0;; tick++) {
        for (int i = 0; i < NCRIT; i++) {
            int tid = atomic_load(&g_crit[i].tid); if (!tid) continue;
            int lc = last_cpu_of(tid);
            if (lc < 0) { atomic_store(&g_crit[i].tid, 0); continue; }   /* the thread ended */
            if (lc < 64) { hits[i][lc]++; samples[i]++; }
            if (atomic_load(&g_crit[i].role) == 2 && main_core() >= 0) {   /* g-main: its own big core */
                cpu_set_t one, m; CPU_ZERO(&one); CPU_SET(main_core(), &one); CPU_ZERO(&m); sched_getaffinity(tid, sizeof m, &m);
                if (!CPU_EQUAL(&m, &one)) {
                    int r = sched_setaffinity(tid, sizeof one, &one) ? errno : 0;
                    static int said; if (said++ < 4) fprintf(stderr, "[ohos-pin] g-main %d pinned to cpu %d%s%s\n", tid, main_core(), r ? ": " : "", r ? strerror(r) : "");
                }
            }
            demand_attr(tid);
            int lv = -1;
            if (crit_qos_on() && g_qget && g_qset && (g_qget(&lv, tid) || lv != 5)) g_qset(5, tid);
        }
        if (tick % 5 == 0) keep_workers();
        uint64_t now = mono_ns();
        if (now >= next_log) {
            next_log = now + 10000000000ull;
            for (int i = 0; i < NCRIT; i++) {
                int tid = atomic_load(&g_crit[i].tid); if (!tid || !samples[i]) continue;
                char line[256]; size_t o = 0;
                for (int c = 0; c < 64 && o + 16 < sizeof line; c++) if (hits[i][c]) o += (size_t)snprintf(line + o, sizeof line - o, " cpu%d:%u%%", c, hits[i][c] * 100 / samples[i]);
                int lv = -1; if (g_qget) g_qget(&lv, tid);
                sattr cur = {.size = sizeof cur}; long gr = syscall(SYS_sched_getattr, tid, &cur, sizeof cur, 0);
                cpu_set_t m; CPU_ZERO(&m); sched_getaffinity(tid, sizeof m, &m); char mt[96]; mask_text(&m, mt, sizeof mt);
                char name[32] = "?", p[64]; snprintf(p, sizeof p, "/proc/self/task/%d/comm", tid);
                FILE *f = fopen(p, "r"); if (f) { if (fgets(name, sizeof name, f)) name[strcspn(name, "\n")] = 0; fclose(f); }
                fprintf(stderr, "[ohos-pin] %s %d (role %d) ran on%s; QoS %d, nice %d, uclamp %d/%d, mask %s\n", name, tid,
                        atomic_load(&g_crit[i].role), line, lv, gr ? 99 : cur.nice, gr ? -1 : (int)cur.util_min, gr ? -1 : (int)cur.util_max, mt);
                memset(hits[i], 0, sizeof hits[i]); samples[i] = 0;
            }
        }
        struct timespec ts = {0, 100000000}; nanosleep(&ts, NULL);
    }
    return NULL;
}

void ohos_sched_start(void) {
    if (env_off("TOTK_OHOS_PIN")) { fprintf(stderr, "[ohos-pin] off (TOTK_OHOS_PIN=0)\n"); return; }
    pthread_t t; if (!pthread_create(&t, NULL, keeper, NULL)) pthread_detach(t);
}
