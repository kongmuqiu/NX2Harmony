/* HarmonyOS thread placement, after single-core scheduling measurements on a Kirin 9030 Pro (SPEC CPU and Geekbench
   runs). The game is bound by single threads: totk-walk (command
   walker/recorder, ~3/4 of a core in the cave) and g-main; left alone they hop over the little or big cores at ~25%
   each, so no core looks busy and the clusters stay at 560-1000 MHz.

   What this phone does (the SPEC cpu-pinning practice, corrected by this port's and Ocarina of Time's logs, 2026-11):
   - 14 logical CPUs: little 0-3 (cpu_capacity 245), middle 4-11 (723), Prime 12-13 (1024); the middle and Prime cores
     run two hardware threads each (thread_siblings_list: 4-5, 6-7, 8-9, 10-11, 12-13);
   - sched_setaffinity takes logical CPUs, but sched_getaffinity reads back other numbers (physical cores, as far as the
     logs go: the whole set reads "0-8", CPU 9 reads "6", CPUs 10 and 11 both read "7"), so a request is not judged by
     comparing it with its read-back;
   - the Prime CPUs are inside the app's set while the phone is cool (Geekbench 7's single-core thread: allowed 4-13,
     100% on cpu 12) and an unpinned critical thread gets there part of the time (the walker: 1-45% on cpu 12); the SPEC
     analysis's warning that hmmac shrinks the cpuset after narrow requests is not confirmed here;
   - uclamp.min does not stick (sched_setattr answers ok, the thread reads back 0/1024; OpenHarmony 7.0.0.111): QoS 5
     and nice -20 do. The SPEC practice (QoS USER_INTERACTIVE 5 with uclamp 1024/1024 re-asserted every 10 ms, no narrow
     affinity on the thread: SOFT mode) stays the default for the critical threads.
   So:
   - critical threads (totk-walk, g-main): on the thread itself (ohos_sched_self; OH_QoS_* and sched_setattr(0) act on
     the caller), at most every TOTK_OHOS_SELF_MS (10): QoS 5, SCHED_NORMAL nice -20 with uclamp 1024/1024
     (KEEP_POLICY|KEEP_PARAMS with uclamp alone as the fallback); every second the mask is widened back to the process's
     allowed set if it was narrowed (inherited from a creator). TOTK_OHOS_WALK_CORE=<cpu>: the walker asks for that CPU
     each second instead; TOTK_OHOS_CRIT_PIN=1: a critical thread whose uclamp did not stick asks for the middle and
     Prime CPUs. The keeper repeats QoS 5 (libqos's SetQosForOtherThread, as GB does) and the sched attributes by tid
     every 100 ms, for g-main whose own calls stop after its start, and holds g-main on a big CPU of its own (main_core);
   - other game threads, recorder workers, totk-post/submit/compute and the driver's gpu-work-server: hard affinity to
     the big cores (middle cpu_capacity tier, 4-11), by tid, looked at every 500 ms and asked again when the wanted set
     or the read-back changes, and every 5 s; never the little cores (recw_cpu, worker_want);
   - TOTK_OHOS_SPIN=1: a frequency keeper (totk-spin) spins on a middle core of its own while the walker submits.
   Every 10 s the keeper logs where each critical thread ran (last CPU sampled every 100 ms) and its QoS / uclamp.
   TOTK_OHOS_PIN=0: no keeper; TOTK_OHOS_SELF=0: no self maintenance; both (the settings page's switch): nothing of this
   file runs; TOTK_OHOS_CRIT_QOS=0: no QoS on critical threads. ohos_sched_start prints every switch's effective value
   in one line ("[ohos-pin] switches:"). */
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
static int env_on(const char *name) { const char *e = getenv(name); return e && *e && strcmp(e, "0"); }
static int env_int(const char *name, int dflt) { const char *e = getenv(name); return e && *e ? atoi(e) : dflt; }
static uint64_t mask_bits(const cpu_set_t *s) { uint64_t b = 0; for (int c = 0; c < 64; c++) if (CPU_ISSET(c, s)) b |= 1ull << c; return b; }
/* a CPU list ("10,8", "4-7", "4,6-7"): the logical CPUs in the order written (a range ascending), at most n of them; the
   count, or -1 when the text is not such a list */
static int cpu_list_parse(const char *s, int *out, int n) {
    int k = 0;
    for (const char *q = s; q && *q; ) {
        char *end; long a = strtol(q, &end, 10), b = a;
        if (end == q || a < 0 || a > 63) return -1;
        if (*end == '-') { q = end + 1; b = strtol(q, &end, 10); if (end == q || b < a || b > 63) return -1; }
        for (long c = a; c <= b; c++) if (k < n) out[k++] = (int)c;
        if (*end == ',') q = end + 1;
        else if (!*end) break;
        else return -1;
    }
    return k;
}

/* ---------- switches: each read once (ohos_sched_start reads them all before the game's threads exist) ---------- */
/* TOTK_OHOS_SELF_MS: the critical threads' self maintenance at most this often (default 10, SPEC's watchdog period) */
static int self_ms(void) { static int v = -1; if (v < 0) { int m = env_int("TOTK_OHOS_SELF_MS", 10); v = m < 0 ? 0 : m; } return v; }
/* TOTK_OHOS_WALK_CORE=<cpu>: the walker asks for that one logical CPU (default -1: not asked) */
static int walk_core(void) { static int v = -2; if (v == -2) { int c = env_int("TOTK_OHOS_WALK_CORE", -1); v = c >= 0 && c < 64 ? c : -1; } return v; }
/* TOTK_OHOS_CRIT_PIN=1: a critical thread whose uclamp.min did not stick asks for the middle and Prime CPUs (default 0) */
static int crit_pin(void) { static int v = -1; if (v < 0) v = env_on("TOTK_OHOS_CRIT_PIN"); return v; }
/* TOTK_OHOS_PIN_EXTRA=1: the threads g-main makes after it was pinned are placed as workers too (default 0, is_worker) */
static int pin_extra(void) { static int v = -1; if (v < 0) v = env_on("TOTK_OHOS_PIN_EXTRA"); return v; }

/* ---------- topology ---------- */
static pthread_once_t g_topo_once = PTHREAD_ONCE_INIT;
static unsigned g_cap[64]; static int g_ncpu;
static cpu_set_t g_prime, g_big;
static const char *g_cap_source = "cpu_capacity";
static uint64_t g_sib[64];   /* the logical CPUs of each CPU's physical core (thread_siblings_list; itself alone if unknown) */
static char g_sib_text[64][24];
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
    for (int c = 0; c < g_ncpu; c++) {
        char p[112], t[64] = ""; snprintf(p, sizeof p, "/sys/devices/system/cpu/cpu%d/topology/thread_siblings_list", c);
        FILE *f = fopen(p, "r"); if (f) { if (!fgets(t, sizeof t, f)) t[0] = 0; fclose(f); t[strcspn(t, "\n")] = 0; }
        int l[64], n = t[0] ? cpu_list_parse(t, l, 64) : -1;
        g_sib[c] = 1ull << c; for (int k = 0; k < n; k++) g_sib[c] |= 1ull << l[k];
        snprintf(g_sib_text[c], sizeof g_sib_text[c], "%.23s", n > 0 ? t : "?");
    }
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

/* ---------- the frequency keeper (Ocarina of Time's oot-a98; off by default here) ----------
   The game is a serial chain of single threads (g-main's logic, then the walker's replay, with waits for the GPU
   between); none is busy all the time, so the governor leaves the middle cluster low (Ocarina of Time: hiperf's cycle
   counts had the walker at 745 MHz and g-main at 534). One core of the cluster kept busy takes the whole cluster to
   2.0 GHz (a shell loop on one middle CPU halved OoT's walker CPU time a draw). So: one thread at nice 0
   (TOTK_OHOS_SPIN_NICE) on a middle core nothing of ours is pinned to spins while the walker is submitting (its last
   call within TOTK_OHOS_SPIN_IDLE_MS, default 300) and sleeps otherwise (menus that stop drawing, the app in the
   background). It costs the power of one core and slows that core's other hardware thread, and its heat may lower the
   GPU's frequency, which TOTK's GPU time follows. TOTK_OHOS_SPIN=1: on (default off; OoT has it on);
   TOTK_OHOS_SPIN_CPU=<cpu>: its CPU (default spin_default_cpu: by physical core). */
static _Atomic uint64_t g_active_ns;
static int g_spin_cpu = -1, g_spin_nice, g_spin_idle_ms = 300;   /* set by ohos_sched_start; g_spin_cpu -1: not running */
static void *spinner(void *arg) {
    (void)arg;
    pthread_setname_np(pthread_self(), "totk-spin");
    int tid = (int)syscall(SYS_gettid);
    /* nice 19 lifted OoT's cluster only to 1.15 - 1.38 GHz (the governor discounts niced load); nice 0 as the shell loop: 2.0 GHz */
    int pr = setpriority(PRIO_PROCESS, (id_t)tid, g_spin_nice) ? errno : 0;
    cpu_set_t one; CPU_ZERO(&one); CPU_SET(g_spin_cpu, &one);
    int ar = sched_setaffinity(0, sizeof one, &one) ? errno : 0;
    cpu_set_t got; CPU_ZERO(&got); sched_getaffinity(0, sizeof got, &got); char gt[96]; mask_text(&got, gt, sizeof gt);
    fprintf(stderr, "[ohos-spin] frequency keeper %d on cpu %d (read back as core %s%s%s; core cpus %s), nice %d%s%s: spins while the walker submitted within %d ms (TOTK_OHOS_SPIN=1)\n",
            tid, g_spin_cpu, gt, ar ? ", " : "", ar ? strerror(ar) : "", g_sib_text[g_spin_cpu], g_spin_nice, pr ? ", " : "", pr ? strerror(pr) : "", g_spin_idle_ms);
    volatile uint64_t sink = 0;
    uint64_t spun_ns = 0, slept_ns = 0, next_log = mono_ns() + 30000000000ull, repin = 0;
    uint64_t idle_ns = (uint64_t)g_spin_idle_ms * 1000000ull;
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
        if (now - repin > 1000000000ull) {   /* the system narrows masks under load: ask again (the read-back is a core number) */
            repin = now; cpu_set_t m; CPU_ZERO(&m);
            if (!sched_getaffinity(0, sizeof m, &m) && !CPU_EQUAL(&m, &one)) sched_setaffinity(0, sizeof one, &one);
        }
    }
    return NULL;
}

/* ---------- self maintenance, on the critical thread ---------- */
void ohos_sched_self(int role) {
    static __thread uint64_t last, widened;
    static __thread int my_role;
    static __thread char logged[200];
    static int self_off = -1;
    if (self_off < 0) self_off = env_off("TOTK_OHOS_SELF");
    if (self_off && g_spin_cpu < 0) return;
    uint64_t now = mono_ns();
    if (last && now - last < (uint64_t)self_ms() * 1000000ull) return;   /* TOTK_OHOS_SELF_MS (10: SPEC's watchdog period) */
    last = now;
    if (role == 1) atomic_store_explicit(&g_active_ns, now, memory_order_relaxed);   /* the walker is submitting: the frequency keeper spins */
    if (self_off) return;   /* (below the heartbeat: the frequency keeper does not depend on self maintenance) */
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
    if (role == 1 && walk_core() >= 0) {
        /* TOTK_OHOS_WALK_CORE=<cpu>: the walker on one logical CPU (e.g. the Prime CPU 12). The read-back is not in
           logical CPUs, so it is not compared: the request is repeated each second */
        if (!widened || now - widened >= 1000000000ull) {
            widened = now;
            cpu_set_t one; CPU_ZERO(&one); CPU_SET(walk_core(), &one);
            wr = sched_setaffinity(0, sizeof one, &one) ? errno : 0; CPU_ZERO(&m); sched_getaffinity(0, sizeof m, &m);
        }
    } else if (crit_pin() && !gr && cur.util_min < 1024) {
        /* TOTK_OHOS_CRIT_PIN=1: uclamp.min did not stick, so it does not bring the Prime cores, and a critical thread left
           without affinity ran 60-85% of its time on the little cores 0-3 in Ocarina of Time (oot-a66). Then the thread
           asks for the middle and Prime CPUs itself, each second. (OoT narrows its read-back mask instead: read back in
           core numbers, "0-8" would ask for the logical CPUs 4-8 alone.) */
        if (!widened || now - widened >= 1000000000ull) {
            widened = now;
            pthread_once(&g_topo_once, topo_init);
            cpu_set_t fast; CPU_OR(&fast, &g_big, &g_prime);
            if (CPU_COUNT(&fast)) { wr = sched_setaffinity(0, sizeof fast, &fast) ? errno : 0; CPU_ZERO(&m); sched_getaffinity(0, sizeof m, &m); }
        }
    } else if (!widened || now - widened >= 1000000000ull) {   /* SOFT mode: never narrower than the process's allowed set */
        widened = now;
        cpu_set_t proc; CPU_ZERO(&proc);
        if (!sched_getaffinity(getpid(), sizeof proc, &proc) && !CPU_EQUAL(&m, &proc)) {
            wr = sched_setaffinity(0, sizeof proc, &proc) ? errno : 0; CPU_ZERO(&m); sched_getaffinity(0, sizeof m, &m);
        }
    }
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
/* set: the logical CPUs asked for last, back: what they read back as just after, set_ns: when; scan: the scan that saw
   the thread last */
typedef struct { int tid; unsigned repins, scan; uint64_t set, back, set_ns; } work_t;
static work_t g_work[MAXT]; static unsigned g_work_scan, g_work_sets;
static int g_nwork;
static int is_worker(const char *name) {
    if (!strcmp(name, "totk-walk") || !strcmp(name, "g-main")) return 0;
    if (!strncmp(name, "g-", 2) || !strncmp(name, "totk-recw", 9) || !strcmp(name, "totk-submit") || !strcmp(name, "totk-post") ||
        !strcmp(name, "totk-compute") || !strcmp(name, "gpu-work-server") || !strcmp(name, "totk-present") || !strcmp(name, "totk-gpufence"))
        return 1;
    /* TOTK_OHOS_PIN_EXTRA=1: also the threads g-main makes after it got its own core; they inherit that one core and stay
       on it beside g-main (audio, uploads, pipeline cache and compilers, seed I/O, translation) */
    return pin_extra() && (!strcmp(name, "totk-audio") || !strcmp(name, "totk-auddev") || !strcmp(name, "totk-audcap") ||
        !strncmp(name, "totk-upload", 11) || !strcmp(name, "totk-pcache") || !strcmp(name, "totk-seed-io") ||
        !strncmp(name, "totk-pipe", 9) || !strcmp(name, "totk-prewarm") || !strcmp(name, "totk-translate"));
}
static int work_slot(int tid) {
    for (int i = 0; i < g_nwork; i++) if (g_work[i].tid == tid) return i;
    if (g_nwork < MAXT) { memset(&g_work[g_nwork], 0, sizeof g_work[0]); g_work[g_nwork].tid = tid; return g_nwork++; }
    return -1;
}
/* g-main is pinned to one big core of its own (affinity works on 0-11): spread over 4-11 at ~25% each it kept the
   big cluster at 558 MHz while the walker waited for it; concentrated, that core's load drives the cluster up.
   TOTK_OHOS_MAIN_CORE=<cpu> picks another (a Prime CPU only when named, e.g. 12: Ocarina of Time's default), -1 leaves
   g-main unpinned. */
static int main_core(void) {
    static int c = -2;
    if (c == -2) {
        const char *e = getenv("TOTK_OHOS_MAIN_CORE"); int v = -1;
        if (e && *e) v = atoi(e);
        else for (int k = 63; k >= 0; k--) if (CPU_ISSET(k, &g_big)) { v = k; break; }
        c = v;   /* (stored once found: another thread never reads -1 while this one searches) */
    }
    return c;
}
/* TOTK_OHOS_RECW_CPUS=<list> (e.g. 10,8): recorder worker n on the n-th CPU of the list, a worker past its end placed as
   the other game threads; unset: main_core-1-n (10 and 9 below g-main's 11). TOTK_OHOS_WORKER_CPUS=<list>: the other
   game threads' CPUs as given; unset: worker_want. Both read by ohos_sched_start. */
static int g_recw_list[8], g_recw_n;
static cpu_set_t g_worker_set;
static void cpu_lists_init(void) {
    static int done; if (done) return; done = 1;
    const char *e = getenv("TOTK_OHOS_RECW_CPUS");
    if (e && *e) {
        int n = cpu_list_parse(e, g_recw_list, 8);
        if (n < 0) fprintf(stderr, "[ohos-pin] TOTK_OHOS_RECW_CPUS=%s is not a CPU list: ignored\n", e); else g_recw_n = n;
    }
    CPU_ZERO(&g_worker_set); e = getenv("TOTK_OHOS_WORKER_CPUS");
    if (e && *e) {
        int l[64], n = cpu_list_parse(e, l, 64);
        if (n < 0) fprintf(stderr, "[ohos-pin] TOTK_OHOS_WORKER_CPUS=%s is not a CPU list: ignored\n", e);
        else for (int k = 0; k < n; k++) CPU_SET(l[k], &g_worker_set);
    }
}
static int recw_cpu(int n) {   /* the CPU of its own for recorder worker n, -1: none */
    if (g_recw_n) return n < g_recw_n ? g_recw_list[n] : -1;
    int mc = main_core(); if (mc < 0) return -1;
    int c = mc - 1 - n;
    /* not the frequency keeper's CPU when that is one of the workers' (below g-main's): the next worker down took it */
    if (g_spin_cpu >= 0 && g_spin_cpu < mc && c <= g_spin_cpu) c--;
    return c >= 0 && CPU_ISSET(c, &g_big) ? c : -1;
}
static void worker_want(const char *name, cpu_set_t *want) {
    /* The recorder workers (~40% of a core each) get a big core each too, below g-main's: one busy core per thread
       keeps the cluster's frequency up instead of four quarter-busy ones. The rest share what is left. */
    int own = -1;
    if (!strncmp(name, "totk-recw", 9) && name[9] >= '0' && name[9] <= '9') own = recw_cpu(name[9] - '0');
    CPU_ZERO(want);
    if (own >= 0) { CPU_SET(own, want); return; }
    if (CPU_COUNT(&g_worker_set)) { *want = g_worker_set; return; }
    int mc = main_core();
    *want = g_big;
    if (g_recw_n) {   /* g-main's CPU and the CPUs TOTK_OHOS_RECW_CPUS names */
        if (mc >= 0 && mc < 64 && CPU_COUNT(want) > 1) CPU_CLR(mc, want);
        for (int n = 0; n < g_recw_n; n++) if (CPU_COUNT(want) > 1) CPU_CLR(g_recw_list[n], want);
    } else if (mc >= 0) for (int c = mc; c >= mc - 2 && c >= 0; c--) if (CPU_COUNT(want) > 1) CPU_CLR(c, want);   /* g-main's and two workers' */
    /* the frequency keeper's physical core, while it runs */
    if (g_spin_cpu >= 0) for (int c = 0; c < 64; c++) if ((g_sib[g_spin_cpu] >> c & 1) && CPU_COUNT(want) > 1) CPU_CLR(c, want);
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
        /* a full table (512 workers alive at once) still places the thread, without the memory of what was asked */
        int i = work_slot(tid); work_t spare = {.tid = tid, .repins = 2}; work_t *w = i >= 0 ? &g_work[i] : &spare;
        w->scan = scan; seen++;
        cpu_set_t want; worker_want(name, &want);
        cpu_set_t now; CPU_ZERO(&now); if (sched_getaffinity(tid, sizeof now, &now)) continue;   /* (the thread ended) */
        if (CPU_EQUAL(&now, &want)) continue;
        /* The read-back is not in logical CPUs here (CPUs 10 and 11 both read back as 7), so a single CPU never compared
           equal and every recorder worker was set again every half second. What was asked last and what it read back as
           are remembered: asked again when the wanted set changes, when the read-back moves (the system changed the
           mask), and every 5 s (a change the read-back cannot show). */
        uint64_t tnow = mono_ns(), wb = mask_bits(&want);
        if (w->set_ns && w->set == wb && w->back == mask_bits(&now) && tnow - w->set_ns < 5000000000ull) continue;
        int r = sched_setaffinity(tid, sizeof want, &want) ? errno : 0;
        cpu_set_t got; CPU_ZERO(&got); sched_getaffinity(tid, sizeof got, &got);
        w->set = wb; w->back = mask_bits(&got); w->set_ns = tnow; g_work_sets++;
        if (w->repins++ < 2) {
            char wt[96], nt[96], gt[96]; mask_text(&want, wt, sizeof wt); mask_text(&now, nt, sizeof nt); mask_text(&got, gt, sizeof gt);
            fprintf(stderr, "[ohos-pin] worker %s %d: %s -> %s, read back %s%s%s\n", name, tid, nt, wt, gt, r ? ", " : "", r ? strerror(r) : "");
        }
    }
    closedir(d);
    /* threads that ended (or stopped being workers) give their slots back: the table only grew, so after 512 worker ids
       no new worker was placed, and a thread id used again found the old thread's memory */
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
            char p[112]; snprintf(p, sizeof p, "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", c);
            o += (size_t)snprintf(line + o, sizeof line - o, " %d:[%s]%u", c, g_sib_text[c], read_uint(p) / 1000);
        }
        fprintf(stderr, "[ohos-pin] topology (cpu:[thread siblings]max MHz):%s\n", line);
    }
    unsigned hits[NCRIT][64]; memset(hits, 0, sizeof hits);
    unsigned samples[NCRIT] = {0};
    int pin_tid[NCRIT] = {0}; uint64_t pin_back[NCRIT] = {0}, pin_ns[NCRIT] = {0}; unsigned main_sets = 0;   /* g-main's pin, as keep_workers */
    uint64_t next_log = mono_ns() + 10000000000ull;
    for (unsigned tick = 0;; tick++) {
        uint64_t tnow = mono_ns();
        for (int i = 0; i < NCRIT; i++) {
            int tid = atomic_load(&g_crit[i].tid); if (!tid) continue;
            int lc = last_cpu_of(tid);
            if (lc < 0) { atomic_store(&g_crit[i].tid, 0); continue; }   /* the thread ended */
            if (lc < 64) { hits[i][lc]++; samples[i]++; }
            if (atomic_load(&g_crit[i].role) == 2 && main_core() >= 0) {   /* g-main: its own big core */
                cpu_set_t one, m; CPU_ZERO(&one); CPU_SET(main_core(), &one); CPU_ZERO(&m); sched_getaffinity(tid, sizeof m, &m);
                /* asked again when the read-back moves and every 5 s: compared with the request (CPU 11 reads back as 7) it
                   was set again every 100 ms */
                if (!CPU_EQUAL(&m, &one) && (pin_tid[i] != tid || pin_back[i] != mask_bits(&m) || tnow - pin_ns[i] >= 5000000000ull)) {
                    int r = sched_setaffinity(tid, sizeof one, &one) ? errno : 0;
                    cpu_set_t got; CPU_ZERO(&got); sched_getaffinity(tid, sizeof got, &got);
                    pin_tid[i] = tid; pin_back[i] = mask_bits(&got); pin_ns[i] = tnow; main_sets++;
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
            fprintf(stderr, "[ohos-pin] the last 10 s: %u affinity requests for workers (%d tracked), %u for g-main\n", g_work_sets, g_nwork, main_sets);
            g_work_sets = main_sets = 0;
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

/* The frequency keeper's default CPU, by physical core: the lowest middle CPU whose core (thread_siblings_list) holds
   none of the pinned threads: g-main (main_core), the walker (TOTK_OHOS_WALK_CORE) and the recorder's up to 4 workers
   (recw_cpu). Today's placement (g-main 11, workers 10, 9 and with more workers 8, 7): cores 10-11, 8-9, 6-7 taken,
   CPU 4; the other game threads then leave 4-5 (worker_want). OoT's main_core-2 (CPU 9 here) shares a core with a
   recorder worker. */
static int spin_default_cpu(void) {
    uint64_t pinned = 0; int mc = main_core(), wc = walk_core();
    if (mc >= 0 && mc < 64) pinned |= 1ull << mc;
    if (wc >= 0) pinned |= 1ull << wc;
    for (int n = 0; n < 4; n++) { int c = recw_cpu(n); if (c >= 0 && c < 64) pinned |= 1ull << c; }
    for (int c = 0; c < 64; c++) if (CPU_ISSET(c, &g_big) && !(g_sib[c] & pinned)) return c;
    return -1;
}

void ohos_sched_start(void) {
    /* the topology, g-main's CPU and every switch before any of the game's threads can ask */
    pthread_once(&g_topo_once, topo_init); (void)main_core();
    (void)self_ms(); (void)walk_core(); (void)crit_pin(); (void)pin_extra(); (void)crit_qos_on(); cpu_lists_init();
    int pin_off = env_off("TOTK_OHOS_PIN"), self_off = env_off("TOTK_OHOS_SELF"), all_off = pin_off && self_off;
    /* The frequency keeper first: its CPU is known before the keeper thread places the workers. The settings page's
       switch writes TOTK_OHOS_PIN=0 and TOTK_OHOS_SELF=0: nothing of this file runs then. */
    int spin = env_on("TOTK_OHOS_SPIN"); const char *se = getenv("TOTK_OHOS_SPIN_CPU");
    g_spin_nice = env_int("TOTK_OHOS_SPIN_NICE", 0);
    { int ms = env_int("TOTK_OHOS_SPIN_IDLE_MS", 300); g_spin_idle_ms = ms < 0 ? 0 : ms; }
    if (spin && !all_off) {
        int c = se && *se ? atoi(se) : spin_default_cpu();
        if (c >= 0 && c < g_ncpu && (CPU_ISSET(c, &g_big) || (se && *se))) {   /* (an explicit CPU must exist: else it would spin unpinned) */
            g_spin_cpu = c;
            pthread_t sp; int cr = pthread_create(&sp, NULL, spinner, NULL);
            if (!cr) pthread_detach(sp);
            else { g_spin_cpu = -1; fprintf(stderr, "[ohos-spin] no thread for the frequency keeper (%s)\n", strerror(cr)); }
        } else fprintf(stderr, "[ohos-spin] no core for the frequency keeper (cpu %d)\n", c);
    } else fprintf(stderr, "[ohos-spin] off (%s)\n", spin ? "pinning and self maintenance are both off" : "TOTK_OHOS_SPIN unset or 0");
    char rt[64] = "unset(main_core-1-n)", wt[96] = "unset";
    if (g_recw_n) { size_t o = 0; for (int n = 0; n < g_recw_n && o + 8 < sizeof rt; n++) o += (size_t)snprintf(rt + o, sizeof rt - o, "%s%d", n ? "," : "", g_recw_list[n]); }
    if (CPU_COUNT(&g_worker_set)) mask_text(&g_worker_set, wt, sizeof wt);
    fprintf(stderr, "[ohos-pin] switches: TOTK_OHOS_PIN=%d TOTK_OHOS_SELF=%d TOTK_OHOS_SELF_MS=%d TOTK_OHOS_CRIT_QOS=%d TOTK_OHOS_MAIN_CORE=%d"
            " TOTK_OHOS_WALK_CORE=%d TOTK_OHOS_CRIT_PIN=%d TOTK_OHOS_RECW_CPUS=%s TOTK_OHOS_WORKER_CPUS=%s TOTK_OHOS_PIN_EXTRA=%d"
            " TOTK_OHOS_SPIN=%d TOTK_OHOS_SPIN_CPU=%d TOTK_OHOS_SPIN_NICE=%d TOTK_OHOS_SPIN_IDLE_MS=%d\n",
            !pin_off, !self_off, self_ms(), crit_qos_on(), main_core(), walk_core(), crit_pin(), rt, wt, pin_extra(),
            g_spin_cpu >= 0, g_spin_cpu, g_spin_nice, g_spin_idle_ms);
    {   /* main_core-1-n puts the first two recorder workers on one physical core when g-main is on a Prime CPU (12: 11, 10) */
        int a = recw_cpu(0), b = recw_cpu(1);
        if (!pin_off && a >= 0 && b >= 0 && a < 64 && (g_sib[a] >> b & 1))
            fprintf(stderr, "[ohos-pin] recorder workers 0 and 1 share a core (cpus %d and %d): TOTK_OHOS_RECW_CPUS sets them apart\n", a, b);
    }
    if (pin_off) { fprintf(stderr, "[ohos-pin] off (TOTK_OHOS_PIN=0)\n"); return; }
    pthread_t t; if (!pthread_create(&t, NULL, keeper, NULL)) pthread_detach(t);
}
