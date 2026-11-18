/* A thread that touches a memory pool unit at the instant its mapping is replaced must not kill the game (oot-a100).

   Pools get their GPU memory late (nvn_vk.c lazy_back_units / back_pool): the dma-buf is mapped over the unit's guest
   address with one mmap(MAP_FIXED) while the game keeps running. On Linux that replacement is atomic for other threads.
   On this kernel it is not: a game thread copying into the unit during the call got SIGSEGV (code 2, the address inside
   the unit that was being backed; oot-a99, "Cpp Crash" on g-main while the world loaded, the log line of that unit's move
   right before it). A fault handler set with sigaction() never sees it: musl's signal chain calls the system's crash
   reporter first, and that one does not return.

   So the kernel's handler for SIGSEGV and SIGBUS is replaced directly (rt_sigaction, the old action kept): a fault whose
   address lies in a range being replaced now, or replaced within the last quarter of a second, sleeps 20 us and returns,
   so the instruction runs again on the new mapping; every other fault goes to the old handler unchanged (the crash
   reporter and its log as before).

   What ends the retries (oot-a103, after the review of a101):
   - while the replacement is in progress: a count (MAX_TRIES sleeps, several seconds), not wall time, so a process the
     system froze in the middle of a move does not give up when it is thawed;
   - after it: a quarter of a second from its end, plus four more faults per range whatever the time (a thread that
     faulted during the move and was held longer than that before its handler ran).
   The first retried fault of a range and every fault given up write one line to the log (write(2), no stdio).
   TOTK_OHOS_FAULT_RETRY=0: not installed. TOTK_OHOS_FAULT_TEST=1: a self-test at start (a page without access, made
   readable by another thread 3 ms later, is read: the read must come back with the value stored before). */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

typedef struct { void (*handler)(int, siginfo_t *, void *); unsigned long flags; void (*restorer)(void); unsigned long mask; } ksa_t;   /* the kernel's (and musl's k_sigaction) */

/* NRANGE: a thread held between its fault and its handler while NRANGE more moves end finds its entry overwritten and
   goes to the crash reporter; a scene load makes hundreds of moves in a burst, so 1024 (32 KB; the scan is about 1 us). */
enum { NRANGE = 1024, MAX_TRIES = 200000, LATE_TRIES = 4 };
/* done_ns 0: being replaced now. tries: retried faults in the range; late: those more than 250 ms after its end. */
static struct { _Atomic uintptr_t lo, hi; _Atomic uint64_t done_ns; _Atomic unsigned tries, late; } g_rng[NRANGE];
static _Atomic unsigned g_rng_next;
static _Atomic int g_cur = -1;   /* the entry of the replacement in progress (one at a time: the pool lock) */
static ksa_t g_old[2];           /* SIGSEGV, SIGBUS */
static _Atomic int g_installed;
static _Atomic uint64_t g_retries, g_faults, g_given_up;

static uint64_t now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }

void ohos_fault_moving(uintptr_t lo, uintptr_t hi) {
    if (!atomic_load_explicit(&g_installed, memory_order_relaxed)) return;
    unsigned i = atomic_fetch_add(&g_rng_next, 1) % NRANGE;
    atomic_store(&g_rng[i].hi, 0);   /* no address is inside while the entry is rewritten */
    atomic_store(&g_rng[i].lo, lo); atomic_store(&g_rng[i].done_ns, 0);
    atomic_store(&g_rng[i].tries, 0); atomic_store(&g_rng[i].late, 0);
    atomic_store(&g_rng[i].hi, hi);
    atomic_store(&g_cur, (int)i);
}
void ohos_fault_moved(void) {
    int i = atomic_exchange(&g_cur, -1);
    if (i >= 0) { uint64_t t = now_ns(); atomic_store(&g_rng[i].done_ns, t ? t : 1); }
}

/* --- text without stdio (the handler) --- */
static size_t put_s(char *b, size_t o, const char *s) { while (*s && o < 230) b[o++] = *s++; return o; }
static size_t put_u(char *b, size_t o, uint64_t v) { char t[24]; int n = 0; do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v); while (n && o < 230) b[o++] = t[--n]; return o; }
static size_t put_x(char *b, size_t o, uint64_t v) { char t[20]; int n = 0; do { t[n++] = "0123456789abcdef"[v & 15]; v >>= 4; } while (v); o = put_s(b, o, "0x"); while (n && o < 230) b[o++] = t[--n]; return o; }
static void say(uintptr_t a, int running, uint64_t ago_ns, unsigned tries, const char *what) {
    char b[240]; size_t o = put_s(b, 0, "[ohos-fault] fault at "); o = put_x(b, o, a);
    o = put_s(b, o, " inside a pool unit whose mapping ");
    if (running) o = put_s(b, o, "is being replaced");
    else { o = put_s(b, o, "was replaced "); o = put_u(b, o, ago_ns / 1000000); o = put_s(b, o, " ms ago"); }
    o = put_s(b, o, ": "); o = put_s(b, o, what);
    if (tries) { o = put_s(b, o, " after "); o = put_u(b, o, tries); o = put_s(b, o, " retries"); }
    b[o++] = '\n';
    (void)!write(2, b, o);
}

/* 1: retry. 0: not in a range of ours. -1: in one, but given up (the line is written).
   The same addresses can be in several entries (a unit backed, released and backed again). Decided first, reported
   after (oot-a104, the review of a103): an entry being replaced now wins, then one replaced less than 250 ms ago; only
   when there is neither does the NEWEST older entry spend its late budget, so a fault that no move explains gets
   LATE_TRIES retries and one line, however many old entries cover its address. */
static int judge(uintptr_t a, uint64_t now) {
    int running = -1, recent = -1, older = -1, spent = -1; uint64_t recent_d = 0, older_d = 0;
    for (int i = 0; i < NRANGE; i++) {
        uintptr_t lo = atomic_load_explicit(&g_rng[i].lo, memory_order_relaxed), hi = atomic_load_explicit(&g_rng[i].hi, memory_order_relaxed);
        if (a < lo || a >= hi) continue;
        uint64_t d = atomic_load_explicit(&g_rng[i].done_ns, memory_order_relaxed);
        unsigned n = atomic_load_explicit(&g_rng[i].tries, memory_order_relaxed);
        /* the entry was rewritten while it was read (ohos_fault_moving): its bounds and its state are not one entry's */
        if (atomic_load_explicit(&g_rng[i].lo, memory_order_relaxed) != lo || atomic_load_explicit(&g_rng[i].hi, memory_order_relaxed) != hi) continue;
        if (!d) { if (n < MAX_TRIES) running = i; else spent = i; }
        else if ((int64_t)(now - d) < 250000000) { if (recent < 0 || d > recent_d) { recent = i; recent_d = d; } }   /* signed: the move may have ended after `now` was read */
        else if (older < 0 || d > older_d) { older = i; older_d = d; }
    }
    int i = running >= 0 ? running : recent;
    if (i < 0 && older >= 0) {
        if (atomic_fetch_add_explicit(&g_rng[older].late, 1, memory_order_relaxed) < LATE_TRIES) i = older;
        else spent = older;
    }
    if (i >= 0) {
        if (!atomic_fetch_add_explicit(&g_rng[i].tries, 1, memory_order_relaxed)) {
            uint64_t d = atomic_load_explicit(&g_rng[i].done_ns, memory_order_relaxed); int64_t ago = d ? (int64_t)(now - d) : 0;
            atomic_fetch_add(&g_faults, 1);
            say(a, !d, ago > 0 ? (uint64_t)ago : 0, 0, "retried (the first fault of this range)");
        }
        return 1;
    }
    if (spent < 0) return 0;
    {   uint64_t d = atomic_load_explicit(&g_rng[spent].done_ns, memory_order_relaxed); int64_t ago = d ? (int64_t)(now - d) : 0;
        say(a, !d, ago > 0 ? (uint64_t)ago : 0, atomic_load_explicit(&g_rng[spent].tries, memory_order_relaxed), "given up, to the crash reporter"); }
    return -1;
}
static void on_fault(int sig, siginfo_t *si, void *uc) {
    int saved = errno;
    uintptr_t a = (uintptr_t)si->si_addr;
    int j = judge(a, now_ns());
    if (j > 0) {
        atomic_fetch_add_explicit(&g_retries, 1, memory_order_relaxed);
        struct timespec ts = {0, 20000}; nanosleep(&ts, NULL);
        errno = saved;
        return;   /* the faulting instruction runs again */
    }
    if (j < 0) atomic_fetch_add(&g_given_up, 1);
    errno = saved;
    const ksa_t *o = &g_old[sig == SIGBUS];
    if ((uintptr_t)o->handler > 1) { o->handler(sig, si, uc); return; }
    /* no handler before ours: the default action, at the instruction's next run */
    ksa_t dfl = {0}; syscall(SYS_rt_sigaction, sig, &dfl, NULL, 8);
}
static int install_one(int sig, ksa_t *old) {
    if (syscall(SYS_rt_sigaction, sig, NULL, old, 8)) return errno;
    ksa_t n = *old; n.handler = on_fault; n.flags |= SA_SIGINFO;
    if (!(old->flags & 0x04000000ul)) return ENOSYS;   /* no restorer in the old action: not the layout this file assumes */
    return syscall(SYS_rt_sigaction, sig, &n, NULL, 8) ? errno : 0;
}
static void *test_helper(void *p) {
    struct timespec ts = {0, 3000000}; nanosleep(&ts, NULL);
    mprotect(p, 4096, PROT_READ | PROT_WRITE);
    ohos_fault_moved();
    return NULL;
}
void ohos_fault_install(void) {
    const char *e = getenv("TOTK_OHOS_FAULT_RETRY");
    if (e && !strcmp(e, "0")) { fprintf(stderr, "[ohos-fault] off (TOTK_OHOS_FAULT_RETRY=0)\n"); return; }
    int a = install_one(SIGSEGV, &g_old[0]), b = a ? -1 : install_one(SIGBUS, &g_old[1]);
    if (a) { fprintf(stderr, "[ohos-fault] not installed: %s (SIGSEGV)\n", strerror(a)); return; }
    atomic_store(&g_installed, 1);
    fprintf(stderr, "[ohos-fault] a fault inside a pool unit whose mapping is being replaced is retried (SIGSEGV; SIGBUS %s); old handler %p, flags %#lx\n",
            b ? strerror(b) : "too", (void *)g_old[0].handler, g_old[0].flags);
    const char *t = getenv("TOTK_OHOS_FAULT_TEST");
    if (t && !strcmp(t, "1")) {
        /* the value is in the page before it loses its access, so whenever the read comes back it reads 0x600d */
        void *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) return;
        *(volatile int *)p = 0x600d;
        mprotect(p, 4096, PROT_NONE);
        ohos_fault_moving((uintptr_t)p, (uintptr_t)p + 4096);
        pthread_t th;
        if (pthread_create(&th, NULL, test_helper, p)) { ohos_fault_moved(); munmap(p, 4096); return; }
        uint64_t t0 = now_ns(); int v = *(volatile int *)p;   /* faults until the helper gives the page access */
        pthread_join(th, NULL);
        fprintf(stderr, "[ohos-fault] self-test: read %#x after %.2f ms and %llu retries (%s)\n", v, (now_ns() - t0) / 1e6,
                (unsigned long long)atomic_load(&g_retries), v == 0x600d ? "ok" : "WRONG");
        munmap(p, 4096);
        /* the test's own fault is not one of the game's: the counters the keeper prints start at zero */
        atomic_store(&g_faults, 0); atomic_store(&g_retries, 0); atomic_store(&g_given_up, 0);
    }
}
void ohos_fault_stats(uint64_t out[3]) { out[0] = atomic_load(&g_faults); out[1] = atomic_load(&g_retries); out[2] = atomic_load(&g_given_up); }
