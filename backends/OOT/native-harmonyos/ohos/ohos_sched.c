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

/* ---------- the frequency keeper (oot-a98) ----------
   The game is a serial chain: g-main's logic (about 20 ms a frame), then the walker's replay (50 ms of CPU), with waits for
   the GPU between; no thread is busy all the time (the walker 49%, g-main 20%), so the governor leaves the middle cluster
   at 0.56 - 1.0 GHz: hiperf's cycle counts say the walker really ran at 745 MHz and g-main at 534 MHz. One core of the
   cluster kept busy takes the whole cluster to 2.0 GHz (and the Prime cores to 1.7 - 1.9): measured with a shell loop on
   cpu 9, the walker's CPU time a draw halved. So: one thread at the lowest priority, on a middle core nothing else of
   ours is pinned to, spins while the walker is submitting (its last call within 0.3 s) and sleeps otherwise (menus that
   stop drawing, the app in the background). It costs the power of one core. TOTK_OHOS_SPIN=0: off;
   TOTK_OHOS_SPIN_CPU=<cpu>: another core (default: two below g-main's). */
static _Atomic uint64_t g_active_ns;
static int g_spin_cpu = -1;
static void *spinner(void *arg) {
    (void)arg;
    pthread_setname_np(pthread_self(), "totk-spin");
    int tid = (int)syscall(SYS_gettid);
    /* nice 19 lifted the cluster only to 1.15 - 1.38 GHz (the governor discounts niced load); nice 0 as the shell loop: 2.0 GHz */
    const char *ne = getenv("TOTK_OHOS_SPIN_NICE"); int nice_v = ne && *ne ? atoi(ne) : 0;
    int pr = setpriority(PRIO_PROCESS, (id_t)tid, nice_v) ? errno : 0;
    cpu_set_t one; CPU_ZERO(&one); CPU_SET(g_spin_cpu, &one);
    int ar = sched_setaffinity(0, sizeof one, &one) ? errno : 0;
    cpu_set_t got; CPU_ZERO(&got); sched_getaffinity(0, sizeof got, &got); char gt[96]; mask_text(&got, gt, sizeof gt);
    fprintf(stderr, "[ohos-spin] frequency keeper %d on cpu %d (read back as core %s%s%s), nice %d%s%s: spins while the walker submits (TOTK_OHOS_SPIN=0 turns it off)\n",
            tid, g_spin_cpu, gt, ar ? ", " : "", ar ? strerror(ar) : "", nice_v, pr ? ", " : "", pr ? strerror(pr) : "");
    volatile uint64_t sink = 0;
    uint64_t spun_ns = 0, slept_ns = 0, next_log = mono_ns() + 30000000000ull, repin = 0;
    /* how long after the walker's last submission the keeper goes on spinning (TOTK_OHOS_SPIN_IDLE_MS, default 300: it
       sleeps through the stalls of a scene load; whether a longer limit shortens the load is not measured) */
    const char *ie = getenv("TOTK_OHOS_SPIN_IDLE_MS"); uint64_t idle_ns = (ie && *ie ? strtoull(ie, NULL, 10) : 300) * 1000000ull;
    for (;;) {
        uint64_t now = mono_ns(), last = atomic_load_explicit(&g_active_ns, memory_order_relaxed);
        if (!last || (now > last && now - last > idle_ns)) {   /* (a heartbeat stored after `now` was read is not "long ago") */
            struct timespec ts = {0, 50000000}; nanosleep(&ts, NULL); slept_ns += 50000000ull;
        } else {
            uint64_t until = now + 2000000ull;   /* 2 ms between looks at the walker */
            while (mono_ns() < until) for (int i = 0; i < 4000; i++) sink += (uint64_t)i;
            spun_ns += 2000000ull;
        }
        if (now >= next_log) {
            fprintf(stderr, "[ohos-spin] the last 30 s: spun %.1f s, slept %.1f s, on cpu %d now\n", spun_ns / 1e9, slept_ns / 1e9, sched_getcpu());
            spun_ns = slept_ns = 0; next_log = now + 30000000000ull;
        }
        if (now - repin > 1000000000ull) {   /* the system narrows masks under load: ask again */
            repin = now; cpu_set_t m; CPU_ZERO(&m);
            if (!sched_getaffinity(0, sizeof m, &m) && !CPU_EQUAL(&m, &one)) sched_setaffinity(0, sizeof one, &one);
        }
    }
    return NULL;
}

/* ---------- self maintenance, on the critical thread ---------- */
static int main_core(void);
void ohos_sched_self(int role) {
    static __thread uint64_t last, widened;
    static __thread int my_role;
    static __thread char logged[200];
    static int self_off = -1;
    if (self_off < 0) self_off = env_off("TOTK_OHOS_SELF");
    uint64_t now = mono_ns();
    if (last && now - last < 50000000ull) return;   /* (10 ms, SPEC's watchdog period, until oot-a102: uclamp never sticks here and each QoS query opens a file) */
    last = now;
    if (role == 1) atomic_store_explicit(&g_active_ns, now, memory_order_relaxed);   /* the walker is submitting: the frequency keeper spins */
    if (self_off) return;   /* (below the heartbeat: the frequency keeper does not depend on self maintenance, oot-a103) */
    if (!my_role) my_role = role;   /* the walker also presents: one thread, one role */
    role = my_role;
    int tid = (int)syscall(SYS_gettid);
    if (!widened) crit_register(tid, role);
    QoS_Level lv = QOS_DEFAULT; int qget = OH_QoS_GetThreadQoS(&lv), qset = 0;
    if (crit_qos_on() && (qget || lv != QOS_USER_INTERACTIVE)) { qset = OH_QoS_SetThreadQoS(QOS_USER_INTERACTIVE); qget = OH_QoS_GetThreadQoS(&lv); }
    int ar = demand_attr(0);   /* every time: the system resets uclamp */
    cpu_set_t m; CPU_ZERO(&m); sched_getaffinity(0, sizeof m, &m);
    int wr = 0;
    sattr cur = {.size = sizeof cur}; long gr = syscall(SYS_sched_getattr, 0, &cur, sizeof cur, 0);
    /* uclamp.min did not stick (OpenHarmony 7.0.0.111 on this phone: sched_setattr answers ok and the thread reads back
       uclamp 0 / 1024): the Prime cores are not reached, and a critical thread left without affinity ran 60-85% of its
       time on the little cores 0-3 (capacity 245), slower than the plain workers the keeper holds on the middle tier:
       Ocarina of Time's walker took 52 us a draw there (oot-a66). Then the thread keeps to the middle tier itself: its
       own mask without the little cores (narrowed only, never widened to CPUs it did not have). TOTK_OHOS_CRIT_PIN=0:
       the old soft mode. */
    static int crit_pin = -1; if (crit_pin < 0) crit_pin = !env_off("TOTK_OHOS_CRIT_PIN");
    pthread_once(&g_topo_once, topo_init);
    int hard = crit_pin && !gr && cur.util_min < 1024;
    /* TOTK_OHOS_WALK_CORE=<cpu>: the walking threads on one logical CPU (sched_setaffinity takes logical CPUs; what
       sched_getaffinity reads back on this phone is in physical cores, so it is not compared, only asked again each second) */
    static int walk_core = -2;
    if (walk_core == -2) {   /* default: the Prime CPU after g-main's (the other thread of the Prime core); -1: not asked */
        const char *e = getenv("TOTK_OHOS_WALK_CORE"); pthread_once(&g_topo_once, topo_init);
        int mc = main_core();
        walk_core = e && *e ? atoi(e) : (mc >= 0 && mc + 1 < 64 && CPU_ISSET(mc, &g_prime) && CPU_ISSET(mc + 1, &g_prime) && !CPU_EQUAL(&g_big, &g_prime) ? mc + 1 : -1);
    }
    if (role == 1 && walk_core >= 0 && walk_core < 64) {
        if (!widened || now - widened >= 1000000000ull) {
            widened = now;
            cpu_set_t one; CPU_ZERO(&one); CPU_SET(walk_core, &one);
            wr = sched_setaffinity(0, sizeof one, &one) ? errno : 0; CPU_ZERO(&m); sched_getaffinity(0, sizeof m, &m);
        }
    } else if (hard) {
        /* every core but the little ones. The Prime cores are inside the process's set on this build while the phone is cool
           (Geekbench 7's single-core thread: allowed 4-13, 100% on cpu 12, read from /proc on 2026-11-10) */
        cpu_set_t fast; CPU_OR(&fast, &g_big, &g_prime);
        cpu_set_t want; CPU_AND(&want, &m, &fast);
        if (!CPU_COUNT(&want)) {   /* a mask of little cores only (inherited from a creator): the middle tier of the process's set */
            cpu_set_t proc; CPU_ZERO(&proc);
            if (!sched_getaffinity(getpid(), sizeof proc, &proc)) CPU_AND(&want, &proc, &fast);
        }
        if (CPU_COUNT(&want) && !CPU_EQUAL(&want, &m)) { wr = sched_setaffinity(0, sizeof want, &want) ? errno : 0; CPU_ZERO(&m); sched_getaffinity(0, sizeof m, &m); }
    } else if (!widened || now - widened >= 1000000000ull) {   /* SOFT mode: never narrower than the process's allowed set */
        widened = now;
        cpu_set_t proc; CPU_ZERO(&proc);
        if (!sched_getaffinity(getpid(), sizeof proc, &proc) && !CPU_EQUAL(&m, &proc)) {
            wr = sched_setaffinity(0, sizeof proc, &proc) ? errno : 0; CPU_ZERO(&m); sched_getaffinity(0, sizeof m, &m);
        }
    }
    if (!widened) widened = now;   /* registered once (crit_register above) */
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
typedef struct { int tid; unsigned repins, scan; cpu_set_t set; uint64_t set_ns; } work_t;   /* set: the logical CPUs asked for last; scan: the scan that saw the thread last */
static work_t g_work[MAXT]; static unsigned g_work_scan;
static cpu_set_t g_recw_cpus;   /* the CPUs given to recorder workers so far */
static int g_nwork;
static int is_worker(const char *name) {
    if (!strcmp(name, "totk-walk") || !strcmp(name, "g-main")) return 0;
    return !strncmp(name, "g-", 2) || !strncmp(name, "totk-recw", 9) || !strcmp(name, "totk-submit") || !strcmp(name, "totk-post") ||
        !strcmp(name, "totk-compute") || !strcmp(name, "gpu-work-server") || !strcmp(name, "totk-present") || !strcmp(name, "totk-gpufence") ||
        /* made by g-main after it got its own core, they inherited that one core (Ocarina of Time: 21 threads on cpu 11, the
           audio thread at 12% of it beside the main thread) */
        !strcmp(name, "totk-audio") || !strncmp(name, "totk-upload", 11) || !strcmp(name, "totk-pcache") || !strcmp(name, "totk-seed-io") ||
        !strcmp(name, "totk-seed-drop");
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
        else {
            /* the first Prime CPU when there is a middle tier below it (oot-a100: the request is answered with the middle and
               Prime cores, and with the frequency keeper busy on a middle core g-main then runs on the Prime core half of
               the time); else the highest big CPU, as before */
            c = -1;
            if (!CPU_EQUAL(&g_big, &g_prime)) for (int k = 0; k < 64; k++) if (CPU_ISSET(k, &g_prime)) { c = k; break; }
            if (c < 0) for (int k = 63; k >= 0; k--) if (CPU_ISSET(k, &g_big)) { c = k; break; }
        }
    }
    return c;
}
static void keep_workers(void) {
    DIR *d = opendir("/proc/self/task"); if (!d) return;
    struct dirent *e;
    unsigned scan = ++g_work_scan; int seen = 0;
    while ((e = readdir(d))) {
        int tid = atoi(e->d_name); if (tid <= 0) continue;
        char p[64], name[32] = ""; snprintf(p, sizeof p, "/proc/self/task/%d/comm", tid);
        FILE *f = fopen(p, "r"); if (!f) continue;
        if (!fgets(name, sizeof name, f)) name[0] = 0;
        fclose(f); name[strcspn(name, "\n")] = 0;
        int crit = 0; for (int i = 0; i < NCRIT; i++) if (atomic_load(&g_crit[i].tid) == tid) crit = 1;
        if (crit || !is_worker(name)) continue;
        /* a full table (512 threads alive at once) still places the thread, without the memory of what was set */
        int i = work_slot(tid); work_t spare = {.tid = tid, .repins = 2}; work_t *w = i >= 0 ? &g_work[i] : &spare;
        w->scan = scan; seen++;
        /* The recorder workers (~40% of a core each) get a big core each too, below g-main's: one busy core per thread
           keeps the cluster's frequency up instead of four quarter-busy ones. The rest share what is left. */
        int mc = main_core(), own = -1;
        if (mc >= 0 && !strncmp(name, "totk-recw", 9) && name[9] >= '0' && name[9] <= '9') {
            int c = mc - 1 - (name[9] - '0');
            /* not the frequency keeper's CPU when that is one of the workers' (below g-main's): the second worker took it */
            if (g_spin_cpu >= 0 && g_spin_cpu < mc && c <= g_spin_cpu) c--;
            if (c >= 0 && CPU_ISSET(c, &g_big)) { own = c; CPU_SET(c, &g_recw_cpus); }
        }
        cpu_set_t want; CPU_ZERO(&want);
        if (own >= 0) CPU_SET(own, &want);
        else {
            /* TOTK_OHOS_WORKER_CPUS=<list of logical CPUs, e.g. 4-7 or 4,5,8>: the other game threads' CPUs (an experiment switch) */
            static cpu_set_t wset; static int wparsed;
            if (!wparsed) {
                wparsed = 1; CPU_ZERO(&wset);
                const char *e = getenv("TOTK_OHOS_WORKER_CPUS");
                for (const char *q = e; q && *q; ) {
                    char *end; long a = strtol(q, &end, 10), b2 = a;
                    if (end == q) break;
                    if (*end == '-') { q = end + 1; b2 = strtol(q, &end, 10); if (end == q) break; }
                    for (long c = a; c <= b2 && c >= 0 && c < 64; c++) CPU_SET((int)c, &wset);
                    q = *end == ',' ? end + 1 : end; if (*end && *end != ',') break;
                }
                if (CPU_COUNT(&wset)) { char wt[96]; mask_text(&wset, wt, sizeof wt); fprintf(stderr, "[ohos-pin] other game threads on logical CPUs %s (TOTK_OHOS_WORKER_CPUS)\n", wt); }
            }
            if (CPU_COUNT(&wset)) want = wset;
            else {
                want = g_big;
                if (mc >= 0) for (int c = mc; c >= mc - 2 && c >= 0; c--) if (CPU_COUNT(&want) > 1) CPU_CLR(c, &want);
                if (g_spin_cpu >= 0 && CPU_COUNT(&want) > 1) CPU_CLR(g_spin_cpu, &want);   /* (the keeper moved with TOTK_OHOS_SPIN_CPU) */
                /* and the recorder workers' own CPUs (one worker: CPU 11, removed above already) */
                for (int c = 0; c < 64; c++) if (CPU_ISSET(c, &g_recw_cpus) && CPU_COUNT(&want) > 1) CPU_CLR(c, &want);
            }
        }
        cpu_set_t now; CPU_ZERO(&now); if (sched_getaffinity(tid, sizeof now, &now)) continue;   /* (the thread ended) */
        /* What is read back is in PHYSICAL cores on this phone, what is set is in LOGICAL CPUs: the two never compare equal,
           so every worker was set again every half second (oot-a102). What was set last is remembered; it is set again when
           the wanted set changes, and every 5 s in case the system narrowed it. */
        uint64_t tnow = mono_ns();
        if (w->set_ns && CPU_EQUAL(&w->set, &want) && tnow - w->set_ns < 5000000000ull) continue;
        int r = sched_setaffinity(tid, sizeof want, &want) ? errno : 0;
        w->set = want; w->set_ns = tnow;
        if (w->repins++ < 2) {
            cpu_set_t got; CPU_ZERO(&got); sched_getaffinity(tid, sizeof got, &got);
            char wt[96], nt[96], gt[96]; mask_text(&want, wt, sizeof wt); mask_text(&now, nt, sizeof nt); mask_text(&got, gt, sizeof gt);
            fprintf(stderr, "[ohos-pin] worker %s %d: %s -> %s, read back %s%s%s\n", name, tid, nt, wt, gt, r ? ", " : "", r ? strerror(r) : "");
        }
    }
    closedir(d);
    /* threads that ended (or stopped being workers) give their slots back: the table only grew, and a thread id used
       again found the old thread's memory (oot-a103) */
    if (seen) for (int k = 0; k < g_nwork; ) { if (g_work[k].scan != scan) g_work[k] = g_work[--g_nwork]; else k++; }
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
        /* which logical CPUs share a core (a spinning thread costs its sibling throughput), and each one's top frequency */
        o = 0; line[0] = 0;
        for (int c = 0; c < g_ncpu && o + 48 < sizeof line; c++) {
            char p[112], sib[32] = "?"; snprintf(p, sizeof p, "/sys/devices/system/cpu/cpu%d/topology/thread_siblings_list", c);
            FILE *f = fopen(p, "r"); if (f) { if (fgets(sib, sizeof sib, f)) sib[strcspn(sib, "\n")] = 0; fclose(f); }
            snprintf(p, sizeof p, "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", c);
            o += (size_t)snprintf(line + o, sizeof line - o, " %d:[%s]%u", c, sib, read_uint(p) / 1000);
        }
        fprintf(stderr, "[ohos-pin] topology (cpu:[thread siblings]max MHz):%s\n", line);
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
            {   /* ohos_fault.c: faults retried while a pool unit's mapping was replaced */
                void ohos_fault_stats(uint64_t out[3]); static uint64_t was[3]; uint64_t f[3]; ohos_fault_stats(f);
                if (f[0] != was[0] || f[2] != was[2]) {
                    fprintf(stderr, "[ohos-fault] %llu faults inside a unit being replaced so far, %llu retries, %llu given up\n",
                            (unsigned long long)f[0], (unsigned long long)f[1], (unsigned long long)f[2]);
                    was[0] = f[0]; was[1] = f[1]; was[2] = f[2];
                }
            }
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
    /* The frequency keeper first: its CPU is known before the keeper thread places the workers, and TOTK_OHOS_PIN=0 turns
       off the pinning only (it turned the frequency keeper off too until oot-a103). */
    pthread_once(&g_topo_once, topo_init); (void)main_core();   /* before any thread can ask: the function holds -1 while it searches */
    /* the settings page's switch writes TOTK_OHOS_PIN=0 and TOTK_OHOS_SELF=0: nothing of this file runs then, as before a103 */
    int all_off = env_off("TOTK_OHOS_PIN") && env_off("TOTK_OHOS_SELF");
    if (!env_off("TOTK_OHOS_SPIN") && !all_off) {
        pthread_once(&g_topo_once, topo_init);
        const char *e = getenv("TOTK_OHOS_SPIN_CPU");
        int c = e && *e ? atoi(e) : main_core() - 2;   /* g-main on CPU 12: CPU 10 (the recorder worker keeps 11) */
        if (c >= 0 && c < 64 && (CPU_ISSET(c, &g_big) || (e && *e))) {
            g_spin_cpu = c;
            pthread_t sp; if (!pthread_create(&sp, NULL, spinner, NULL)) pthread_detach(sp);
        } else fprintf(stderr, "[ohos-spin] no core for the frequency keeper (cpu %d)\n", c);
    } else fprintf(stderr, "[ohos-spin] off (%s)\n", all_off ? "pinning and self maintenance are both off" : "TOTK_OHOS_SPIN=0");
    if (env_off("TOTK_OHOS_PIN")) { fprintf(stderr, "[ohos-pin] off (TOTK_OHOS_PIN=0)\n"); return; }
    pthread_t t; if (!pthread_create(&t, NULL, keeper, NULL)) pthread_detach(t);
}
