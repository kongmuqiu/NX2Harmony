/* test_told_fence.c (totk-ohos 2026-11-17): host test for the told-fence consistency of two frames in flight
   (third_party/d2r_gfx/gfx/nvn/totk_told.h; hle_nvn.c told_*, nvn_vk.c mtl_told_pending / g_told_pb).

   1. The header's pieces: the told fence (raise, open, clear with the lag off), the inconsistency test, post and slot
      ranges, the event post index (granules, collisions, coverage), the call-site table, the per-thread memo.
   2. A model of the backend in nvn_vk.c's shapes: a ring of slots sealed by one walking thread (posts copied in, the
      event post index kept, the value published under g_mu), a completion thread that applies posts, leaves the index
      and only then publishes g_done, mtl_wait copied from nvn_vk.c, and the frame-end wait redirected to the previous
      frame's fence as hle_nvn.c frame_lag_target does. After each frame-end wait a game-logic thread reads the frame's
      event the way hle_nvn.c told_addr does. With F1 off it must flag every stale read (no false negatives) and stale
      reads must occur (the freeze mechanism, design.md H1); with F1 on no read may be stale. The index is switched on
      mid-run from another thread while the walker seals (mtl_told_track's since = reserved + 2).
   3. The bounded wait: a stalled GPU makes F1's wait give up after its timeout (here 100 ms instead of 2 s) and the
      check goes on; once the GPU moves the check is consistent again.
   4. Cost, information only: one event read through the index (miss) against the ring scan it replaces, with two
      pending slots of 1004 posts each (the freeze logs' post-op capacity: 1592-2294 entries over the ring).

   Build and run (Linux or WSL; -fsanitize=thread also checks the publication order):
     cc -std=gnu11 -O2 -Wall -Wextra -pthread -I<overlay>/third_party/d2r_gfx/gfx/nvn -o test_told_fence test_told_fence.c && ./test_told_fence
   Not part of the device build (the Makefile's BACKEND_C list names the backend's sources). Exit code 0 = pass. */
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "totk_told.h"

static int g_fail;
#define CHECK(c, ...) do { if (!(c)) { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
static uint64_t now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }

/* ---------------- the backend model (nvn_vk.c shapes) ---------------- */
enum { RINGN = 16, MAXPOST = 4 };
typedef struct { int kind; _Atomic uint32_t *val; uint32_t v; uint64_t addr; } spost;
typedef struct { uint64_t value; spost post[MAXPOST]; int npost, busy, told_pb; } sslot;
static sslot g_ring[RINGN]; static unsigned g_next;                 /* walking thread */
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER, g_cv_work = PTHREAD_COND_INITIALIZER;
static _Atomic uint64_t g_done; static uint64_t g_reserved;          /* g_mu (g_done stored under it) */
static unsigned g_q[RINGN], g_qhead, g_qtail; static int g_stop;     /* g_mu */
static _Atomic uint32_t g_pb[TOLD_PB_N]; static _Atomic int g_track; static _Atomic uint64_t g_since;
static _Atomic int g_stall; static unsigned g_gpu_us = 250;

/* nvn_vk.c mtl_wait, copied */
static int mtl_wait(uint64_t fence, uint64_t timeout_ns) {
    if (atomic_load(&g_done) >= fence) return 1;
    if (!timeout_ns) return 0;
    struct timespec dl; clock_gettime(CLOCK_REALTIME, &dl);
    uint64_t ns = (uint64_t)dl.tv_nsec + (timeout_ns > 3600000000000ull ? 3600000000000ull : timeout_ns);
    dl.tv_sec += (time_t)(ns / 1000000000u); dl.tv_nsec = (long)(ns % 1000000000u);
    pthread_mutex_lock(&g_mu);
    while (atomic_load(&g_done) < fence) if (pthread_cond_timedwait(&g_cv, &g_mu, &dl)) break;
    int ok = atomic_load(&g_done) >= fence;
    pthread_mutex_unlock(&g_mu);
    return ok;
}
/* nvn_vk.c mtl_told_track */
static void track_on(void) {
    if (atomic_exchange(&g_track, 1)) return;
    pthread_mutex_lock(&g_mu); uint64_t since = g_reserved + 2; pthread_mutex_unlock(&g_mu);
    atomic_store(&g_since, since);
}
/* nvn_vk.c slot_submit_inner: posts into the slot, told_pb_seal, the value published under g_mu, handed to the GPU */
static uint64_t seal(_Atomic uint32_t *ev, uint32_t v) {
    sslot *s = &g_ring[g_next++ % RINGN];
    pthread_mutex_lock(&g_mu); while (s->busy) pthread_cond_wait(&g_cv, &g_mu); s->busy = 1; pthread_mutex_unlock(&g_mu);
    s->npost = 0;
    if (ev) { s->post[0] = (spost){1, ev, v, (uint64_t)(uintptr_t)ev}; s->post[1] = (spost){2, NULL, 0, (uint64_t)(uintptr_t)ev + 64}; s->npost = 2; }
    s->told_pb = 0;
    if (atomic_load(&g_track) && s->npost) { for (int i = 0; i < s->npost; i++) if (s->post[i].kind == 1) told_pb_add(g_pb, s->post[i].addr); s->told_pb = 1; }
    pthread_mutex_lock(&g_mu);
    s->value = ++g_reserved; g_q[g_qtail++ % RINGN] = (unsigned)(s - g_ring);
    pthread_cond_signal(&g_cv_work); uint64_t value = s->value;
    pthread_mutex_unlock(&g_mu);
    return value;
}
/* nvn_vk.c completion_thread: GPU time, apply_posts, leave the index, then g_done under g_mu */
static void *gpu_thread(void *arg) {
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&g_mu);
        while (g_qhead == g_qtail && !g_stop) pthread_cond_wait(&g_cv_work, &g_mu);
        if (g_qhead == g_qtail) { pthread_mutex_unlock(&g_mu); return NULL; }
        sslot *s = &g_ring[g_q[g_qhead % RINGN]];
        pthread_mutex_unlock(&g_mu);
        while (atomic_load(&g_stall)) usleep(1000);
        usleep(g_gpu_us);
        for (int i = 0; i < s->npost; i++) if (s->post[i].kind == 1) atomic_store_explicit(s->post[i].val, s->post[i].v, memory_order_relaxed);
        if (s->told_pb) for (int i = 0; i < s->npost; i++) if (s->post[i].kind == 1) told_pb_sub(g_pb, s->post[i].addr);
        pthread_mutex_lock(&g_mu);
        s->npost = 0; s->busy = 0; g_qhead++; atomic_store(&g_done, s->value);
        pthread_cond_broadcast(&g_cv);
        pthread_mutex_unlock(&g_mu);
    }
}
/* nvn_vk.c mtl_told_pending, the event path (what == TOLD_Q_EVENT) */
static _Atomic uint64_t g_fast, g_scans;
static uint64_t pending_event(uint64_t lo, uint64_t hi, uint64_t upto) {
    if (told_pb_covers(atomic_load(&g_done), atomic_load_explicit(&g_since, memory_order_relaxed)) && !told_pb_maybe(g_pb, lo)) { atomic_fetch_add(&g_fast, 1); return 0; }
    atomic_fetch_add(&g_scans, 1);
    uint64_t best = 0;
    pthread_mutex_lock(&g_mu);
    uint64_t done = atomic_load(&g_done);
    for (unsigned i = 0; i < RINGN; i++) {
        const sslot *s = &g_ring[i];
        if (!told_slot_in(s->value, done, upto) || s->value <= best) continue;
        for (int j = 0; j < s->npost; j++) if (s->post[j].kind == 1 && told_post_hits(1, s->post[j].addr, lo, hi)) { best = s->value; break; }
    }
    pthread_mutex_unlock(&g_mu);
    return best;
}
static void backend_reset(void) {
    memset(g_ring, 0, sizeof g_ring); g_next = 0; atomic_store(&g_done, 0); g_reserved = 0; g_qhead = g_qtail = 0; g_stop = 0;
    for (int i = 0; i < TOLD_PB_N; i++) atomic_store(&g_pb[i], 0);
    atomic_store(&g_track, 0); atomic_store(&g_since, 0); atomic_store(&g_stall, 0); atomic_store(&g_fast, 0); atomic_store(&g_scans, 0);
}
static void backend_stop(pthread_t gpu) { pthread_mutex_lock(&g_mu); g_stop = 1; pthread_cond_signal(&g_cv_work); pthread_mutex_unlock(&g_mu); pthread_join(gpu, NULL); }

/* ---------------- 2. frames with the redirected frame-end wait ---------------- */
enum { FRAMES = 400, TRACK_AT = 60, NEV = 3 };
static _Atomic uint64_t g_told;
static _Atomic uint32_t g_ev[NEV * 64];   /* event values 256 bytes apart: ev 0 is posted every frame, the others never */
static _Atomic uint64_t g_frame_told; static _Atomic uint64_t g_frame_checked;
static int g_f1;
static uint64_t g_stale, g_flagged, g_flagged_other, g_waits;
/* hle_nvn.c told_addr for one event read, with the census outcome; returns the value read */
static uint64_t g_polls, g_poll_fast;
/* use_memo 0: every read looks up again (a game polling one event until it changes), so reads also land while the
   completion thread applies the slot: the order "apply, then leave the index" and the index coverage are tested there */
static uint32_t read_event(int e, int *flag, int use_memo) {
    _Atomic uint32_t *ev = &g_ev[e * 64]; uint64_t a = (uint64_t)(uintptr_t)ev;
    static __thread told_memo memo;
    *flag = 0;
    uint64_t t = told_open(&g_told, mtl_wait, 1);
    if (t) {
        if (!use_memo) memo.told = 0;
        if (!told_memo_hit(&memo, a, t)) { memo.need = pending_event(a, a + 4, t); memo.a = a; memo.told = t; }
        if (memo.need && !mtl_wait(memo.need, 0)) { *flag = 1; if (g_f1) { g_waits++; mtl_wait(memo.need, TOLD_WAIT_NS); } }
    }
    return atomic_load_explicit(ev, memory_order_relaxed);
}
static void *logic_thread(void *arg) {
    (void)arg;
    for (uint64_t n = 1; n <= FRAMES; n++) {
        while (atomic_load(&g_frame_told) < n) usleep(20);
        int flag; uint32_t v = read_event(0, &flag, 1);
        int stale = v < n;
        g_stale += stale; g_flagged += flag;
        CHECK(!stale || flag, "frame %llu: stale event value %u not flagged", (unsigned long long)n, v);
        if (g_f1) CHECK(!stale, "frame %llu: F1 on, event value %u is stale", (unsigned long long)n, v);
        for (int e = 1; e < NEV; e++) { int f2; read_event(e, &f2, 1); g_flagged_other += f2; }
        while (v < n) {   /* a gate polling the event until frame n's value lands */
            uint64_t fast0 = atomic_load(&g_fast);
            v = read_event(0, &flag, 0); g_polls++; g_poll_fast += atomic_load(&g_fast) != fast0;
            CHECK(v >= n || flag, "frame %llu: a poll read the stale value %u unflagged", (unsigned long long)n, v);
            if (v < n) usleep(5);
        }
        atomic_store(&g_frame_checked, n);
    }
    return NULL;
}
static void *tracker_thread(void *arg) {   /* switches the index on at about frame TRACK_AT, at no particular point of a seal */
    (void)arg; while (atomic_load(&g_frame_checked) < TRACK_AT - 1) usleep(37);
    usleep(500);   /* the walker is sealing frame TRACK_AT now (the mutation check stretches its seal to put this inside one) */
    track_on(); return NULL;
}
static void run_frames(int f1) {
    backend_reset(); memset(g_ev, 0, sizeof g_ev); atomic_store(&g_told, 0); atomic_store(&g_frame_told, 0); atomic_store(&g_frame_checked, 0);
    g_f1 = f1; g_stale = g_flagged = g_flagged_other = g_waits = g_polls = g_poll_fast = 0;
    pthread_t gpu, logic, tracker; pthread_create(&gpu, NULL, gpu_thread, NULL); pthread_create(&logic, NULL, logic_thread, NULL);
    pthread_create(&tracker, NULL, tracker_thread, NULL);   /* mtl_told_track from another thread while the walker seals */
    uint64_t last = 0, prev = 0;
    for (uint64_t n = 1; n <= FRAMES; n++) {
        for (int c = 0; c < 3; c++) seal(NULL, 0);       /* the frame's chunks */
        seal(&g_ev[0], (uint32_t)n);                     /* the game submission's last slot: event SET n */
        uint64_t fence = seal(NULL, 0);                  /* the present slot; the frame-end sync is fenced here */
        if (fence != last) { prev = last; last = fence; }
        uint64_t want = prev < fence ? prev : fence;     /* hle_nvn.c frame_lag_target */
        int ok = mtl_wait(want, 5000000000ull);
        told_after_wait(&g_told, want, fence, ok);
        CHECK(ok && atomic_load(&g_told) == fence, "frame %llu: told %llu, fence %llu", (unsigned long long)n, (unsigned long long)atomic_load(&g_told), (unsigned long long)fence);
        atomic_store(&g_frame_told, n);
        while (atomic_load(&g_frame_checked) < n) usleep(20);   /* the game's logic consumes frame n before frame n+1 */
    }
    pthread_join(logic, NULL); pthread_join(tracker, NULL); backend_stop(gpu);
    CHECK(atomic_load(&g_since) > 1, "the index was switched on mid-run (since %llu)", (unsigned long long)atomic_load(&g_since));
    printf("frames, F1 %s: %d frames, stale reads %llu, flagged %llu (F1 waits %llu); other events flagged %llu; polls %llu (%llu answered by the index); index answered %llu reads, scans %llu\n",
           f1 ? "on " : "off", FRAMES, (unsigned long long)g_stale, (unsigned long long)g_flagged, (unsigned long long)g_waits,
           (unsigned long long)g_flagged_other, (unsigned long long)g_polls, (unsigned long long)g_poll_fast,
           (unsigned long long)atomic_load(&g_fast), (unsigned long long)atomic_load(&g_scans));
    CHECK(g_flagged_other == 0, "an event that is never posted was flagged %llu times", (unsigned long long)g_flagged_other);
    CHECK(atomic_load(&g_fast) > 0, "the event post index never answered a read");
    if (!f1) CHECK(g_stale > 0, "no stale read without F1: the model does not reproduce the lag (raise g_gpu_us)");
    if (f1) CHECK(g_stale == 0 && g_waits > 0, "F1: stale %llu, waits %llu", (unsigned long long)g_stale, (unsigned long long)g_waits);
}

/* ---------------- 3. the bounded wait ---------------- */
static void test_bounded_wait(void) {
    backend_reset(); atomic_store(&g_told, 0);
    pthread_t gpu; pthread_create(&gpu, NULL, gpu_thread, NULL);
    atomic_store(&g_stall, 1);
    uint64_t f = seal(NULL, 0);
    told_raise(&g_told, f);
    uint64_t t = told_open(&g_told, mtl_wait, 1);
    CHECK(t == f && told_inconsistent(f, t, atomic_load(&g_done)), "stalled: told %llu, fence %llu", (unsigned long long)t, (unsigned long long)f);
    uint64_t t0 = now_ns(); int ok = mtl_wait(f, 100000000ull); uint64_t dt = now_ns() - t0, gave_up = dt;
    CHECK(!ok && dt >= 95000000ull && dt < 1000000000ull, "bounded wait: ok %d after %.1f ms (want 0 after ~100 ms)", ok, dt / 1e6);
    atomic_store(&g_stall, 0);
    t0 = now_ns(); ok = mtl_wait(f, 100000000ull); dt = now_ns() - t0;
    CHECK(ok && dt < 100000000ull, "after the stall: ok %d after %.1f ms", ok, dt / 1e6);
    CHECK(!told_open(&g_told, mtl_wait, 1) && atomic_load(&g_told) == f, "lag on: a reached told fence stays (%llu)", (unsigned long long)atomic_load(&g_told));
    CHECK(!told_open(&g_told, mtl_wait, 0) && atomic_load(&g_told) == 0, "lag off: a reached told fence is cleared (%llu)", (unsigned long long)atomic_load(&g_told));
    printf("bounded wait (100 ms here, 2 s on the device): gave up after %.1f ms with the GPU stalled, returned %.1f ms after it moved\n", gave_up / 1e6, dt / 1e6);
    backend_stop(gpu);
}

/* ---------------- 1. the header's pieces ---------------- */
static _Atomic uint64_t g_race_told; static uint64_t g_race_hi[4];
static void *raiser(void *arg) {
    unsigned id = (unsigned)(uintptr_t)arg, x = id * 2654435761u + 1; uint64_t hi = 0;
    for (int i = 0; i < 200000; i++) { x = x * 1103515245u + 12345u; uint64_t f = x % 1000003; if (f > hi) hi = f; told_raise(&g_race_told, f); }
    g_race_hi[id] = hi; return NULL;
}
static int reach_none(uint64_t f, uint64_t t) { (void)f; (void)t; return 0; }
static int reach_all(uint64_t f, uint64_t t) { (void)f; (void)t; return 1; }
static int reach_and_raise(uint64_t f, uint64_t t) { (void)t; told_raise(&g_race_told, f + 10); return 1; }   /* a redirect lands between the load and the clear */
static void test_pieces(void) {
    _Atomic uint64_t told = 0;
    told_after_wait(&told, 5, 5, 1); CHECK(told == 0, "an unmoved wait tells nothing");
    told_after_wait(&told, 4, 9, 0); CHECK(told == 0, "a failed wait tells nothing");
    told_after_wait(&told, 4, 9, 1); CHECK(told == 9, "a redirected wait tells its fence");
    told_after_wait(&told, 3, 7, 1); CHECK(told == 9, "the told fence never moves back");
    pthread_t th[4]; for (unsigned i = 0; i < 4; i++) pthread_create(&th[i], NULL, raiser, (void *)(uintptr_t)i);
    uint64_t hi = 0; for (unsigned i = 0; i < 4; i++) { pthread_join(th[i], NULL); if (g_race_hi[i] > hi) hi = g_race_hi[i]; }
    CHECK(atomic_load(&g_race_told) == hi, "concurrent raises: %llu, want %llu", (unsigned long long)atomic_load(&g_race_told), (unsigned long long)hi);

    atomic_store(&told, 0); CHECK(told_open(&told, reach_none, 1) == 0, "nothing told");
    atomic_store(&told, 12); CHECK(told_open(&told, reach_none, 1) == 12, "told and pending");
    CHECK(told_open(&told, reach_all, 1) == 0 && told == 12, "reached, lag on: kept");
    CHECK(told_open(&told, reach_all, 0) == 0 && told == 0, "reached, lag off: cleared");
    atomic_store(&g_race_told, 20); CHECK(told_open(&g_race_told, reach_and_raise, 0) == 0 && atomic_load(&g_race_told) == 30, "a newer told fence survives the clear (%llu)", (unsigned long long)atomic_load(&g_race_told));

    CHECK(!told_inconsistent(0, 9, 0), "fence 0 is complete");
    CHECK(told_inconsistent(9, 9, 8) && told_inconsistent(5, 9, 4), "at or below told, GPU short of it");
    CHECK(!told_inconsistent(10, 9, 4), "above told: the game was not told");
    CHECK(!told_inconsistent(5, 9, 5), "the GPU reached it");

    CHECK(told_post_hits(1, 100, 100, 101) && told_post_hits(1, 100, 103, 104) && !told_post_hits(1, 100, 104, 108) && !told_post_hits(1, 100, 96, 100), "event post: 4 bytes, half-open");
    CHECK(told_post_hits(2, 100, 115, 116) && !told_post_hits(2, 100, 116, 120) && told_post_hits(2, 100, 0, 1u << 20), "counter post: 16 bytes; a whole-pool range covers it");
    CHECK(!told_slot_in(5, 5, 9) && told_slot_in(6, 5, 9) && told_slot_in(9, 5, 9) && !told_slot_in(10, 5, 9), "slot range (done, upto]");

    static _Atomic uint32_t tab[TOLD_PB_N];
    CHECK(told_pb_slot(0x10000) == told_pb_slot(0x1000c) && told_pb_slot(0x10000) != told_pb_slot(0x10010), "granules of 16 bytes");
    told_pb_add(tab, 0x10004); CHECK(told_pb_maybe(tab, 0x10004) && told_pb_maybe(tab, 0x10008), "added: maybe (same granule)");
    told_pb_add(tab, 0x10004); told_pb_sub(tab, 0x10004); CHECK(told_pb_maybe(tab, 0x10004), "counts, not bits");
    told_pb_sub(tab, 0x10004); CHECK(!told_pb_maybe(tab, 0x10004), "removed");
    uint64_t a = 0x2000000, b = a + 16; while (told_pb_slot(b) != told_pb_slot(a)) b += 16;
    told_pb_add(tab, a); CHECK(told_pb_maybe(tab, b), "a collision answers maybe (then the exact scan)"); told_pb_sub(tab, a);
    { static unsigned load[TOLD_PB_N]; unsigned mx = 0; uint64_t base = 0x7f12340000ull;
      for (int i = 0; i < 2048; i++) { unsigned s = told_pb_slot(base + (uint64_t)i * 256); if (++load[s] > mx) mx = load[s]; }
      CHECK(mx <= 6, "2048 events 256 bytes apart: at most %u per granule counter", mx); }
    CHECK(!told_pb_covers(100, 0) && !told_pb_covers(100, 102) && told_pb_covers(101, 102) && told_pb_covers(500, 102), "index coverage");

    told_site st[TOLD_SITES]; memset(st, 0, sizeof st);
    for (int i = 0; i < 1000; i++) { told_site_note(st, 0xa0); if (i % 2) told_site_note(st, 0xb0); if (i % 5 == 0) told_site_note(st, 0xc0); told_site_note(st, 0x1000 + (uint64_t)i); }
    told_site top[4]; int nt = told_site_top(st, top, 4);
    CHECK(nt == 4 && top[0].lr == 0xa0 && top[1].lr == 0xb0 && top[2].lr == 0xc0, "top sites: %d, %#llx %#llx %#llx", nt, (unsigned long long)top[0].lr, (unsigned long long)top[1].lr, (unsigned long long)top[2].lr);
    CHECK(top[0].n >= 1000 && top[1].n >= 500 && top[2].n >= 200, "space-saving counts are upper bounds");

    told_memo m = {0};
    CHECK(!told_memo_hit(&m, 0x40, 9), "empty memo"); m = (told_memo){0x40, 9, 7};
    CHECK(told_memo_hit(&m, 0x40, 9) && !told_memo_hit(&m, 0x40, 10) && !told_memo_hit(&m, 0x44, 9), "memo by address and told fence");
    printf("pieces: told fence, inconsistency, ranges, event post index, call sites, memo\n");
}

/* ---------------- 4. cost (information only) ---------------- */
static void bench(void) {
    enum { SLOTS = 16, POSTS = 1000, READS = 200000 };
    static spost posts[SLOTS][POSTS + 4]; static uint64_t values[SLOTS]; static int nposts[SLOTS];
    static _Atomic uint32_t tab[TOLD_PB_N]; static _Atomic uint32_t evs[64];
    for (int s = 0; s < SLOTS; s++) {
        values[s] = 100 + (uint64_t)s; nposts[s] = s >= 14 ? POSTS + 4 : 0;   /* the two newest slots pending, as with the lag */
        for (int j = 0; j < POSTS; j++) posts[s][j] = (spost){2, NULL, 0, 0x500000000ull + (uint64_t)(s * POSTS + j) * 16};
        for (int j = 0; j < 4; j++) { posts[s][POSTS + j] = (spost){1, &evs[j * 4], 1, (uint64_t)(uintptr_t)&evs[j * 4]}; if (nposts[s]) told_pb_add(tab, (uint64_t)(uintptr_t)&evs[j * 4]); }
    }
    uint64_t probe = (uint64_t)(uintptr_t)&evs[40], sink = 0;
    uint64_t t0 = now_ns();
    for (int r = 0; r < READS; r++) {
        uint64_t best = 0, done = 113, upto = 115;
        pthread_mutex_lock(&g_mu);
        for (int s = 0; s < SLOTS; s++) {
            if (!told_slot_in(values[s], done, upto) || values[s] <= best) continue;
            for (int j = 0; j < nposts[s]; j++) if (posts[s][j].kind == 1 && told_post_hits(1, posts[s][j].addr, probe, probe + 4)) { best = values[s]; break; }
        }
        pthread_mutex_unlock(&g_mu);
        sink += best;
    }
    uint64_t scan = now_ns() - t0; t0 = now_ns();
    for (int r = 0; r < READS; r++) sink += (uint64_t)told_pb_maybe(tab, probe);
    uint64_t idx = now_ns() - t0;
    printf("cost per event read with %d posts pending: ring scan under the lock %.0f ns, event post index %.1f ns (sink %llu)\n",
           2 * (POSTS + 4), (double)scan / READS, (double)idx / READS, (unsigned long long)sink);
}

int main(void) {
    test_pieces();
    test_bounded_wait();
    run_frames(0);
    run_frames(1);
    bench();
    printf(g_fail ? "FAILED: %d\n" : "all passed\n", g_fail);
    return g_fail != 0;
}
