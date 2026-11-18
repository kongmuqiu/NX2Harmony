/* totk_told.h (totk-ohos 2026-11-17): the told fence and its consistency checks, for two frames in flight
   (hle_nvn.c TOTK_OHOS_SYNC_LAG=1). The parts that need no NVN object or Vulkan state, so the host test
   (android/gfx/test_told_fence.c) runs the same code that hle_nvn.c and nvn_vk.c use.

   Terms (as that night's analysis notes define them, section 2; the notes are not published):
   - the told fence: the largest frame-end fence a redirected nvnSyncWait told the game is complete, while the GPU had
     only reached the previous frame's fence (hle_nvn.c frame_lag_target);
   - an inconsistent check: a later check of GPU completion for something at or below the told fence that still sees
     "not done": a sync poll or wait, an event read whose post is still pending, an invalidate over a pending GPU write or
     post, a counter timestamp conversion whose report is pending.
   Fences count sealed ring slots (nvn_vk.c slot_submit_inner); the GPU's progress is g_done, read through mtl_wait.

   Nothing here locks, allocates or logs. The told fence only moves while hle_nvn.c tracks it (TOTK_OHOS_SYNC_LAG=1 with
   F1 or the census on): until then every check below costs one load and one predictable branch. */
#pragma once
#include <stdatomic.h>
#include <stdint.h>

/* mtl_wait's shape: 1 once the GPU has completed fence (timeout 0 = poll, else wait at most timeout_ns). */
typedef int (*told_wait_fn)(uint64_t fence, uint64_t timeout_ns);

/* F1's wait for one inconsistent check: bounded (2 s), after which the check goes on as without F1. */
#define TOLD_WAIT_NS 2000000000ull

/* nvn_vk.c mtl_told_pending: what a check depends on. */
enum { TOLD_Q_EVENT = 1,   /* event posts at the address (an event read) */
       TOLD_Q_POSTS = 2,   /* event and counter posts in the range */
       TOLD_Q_GW = 4 };    /* GPU writes in nvn_vk.c's GPU-write index */

/* Raise the told fence to f (never lower it). The release pairs with told_open's acquire: whoever sees the new told
   fence also sees what happened before the redirected wait returned (the slots' post index entries, nvn_vk.c). */
static inline void told_raise(_Atomic uint64_t *told, uint64_t f) {
    uint64_t t = atomic_load_explicit(told, memory_order_relaxed);
    while (f > t && !atomic_compare_exchange_weak_explicit(told, &t, f, memory_order_release, memory_order_relaxed)) {}
}
/* A frame-end wait whose target was moved below its fence (want < fence) and that returned satisfied (ok) told the game
   the fence is complete. Any other wait tells nothing new. */
static inline void told_after_wait(_Atomic uint64_t *told, uint64_t want, uint64_t fence, int ok) {
    if (ok && want < fence) told_raise(told, fence);
}
/* The told fence while some completion at or below it is still outstanding, else 0. Once the GPU has reached it every
   check is consistent; with the lag off (live 0) the told fence is then cleared, so the callers' first test (told != 0)
   is again the only cost. A redirect racing with the clear raises it again (the compare-exchange keeps a newer value). */
static inline uint64_t told_open(_Atomic uint64_t *told, told_wait_fn reached, int live) {
    uint64_t t = atomic_load_explicit(told, memory_order_acquire);
    if (__builtin_expect(!t, 1)) return 0;
    if (reached(t, 0)) {
        if (!live) { uint64_t e = t; atomic_compare_exchange_strong_explicit(told, &e, 0, memory_order_relaxed, memory_order_relaxed); }
        return 0;
    }
    return t;
}
/* A completion check for fence f is inconsistent when the game was told f is complete (f <= told) and the GPU has not
   reached it (f > done). Fence 0 (a sync fenced before any submission) is always complete. */
static inline int told_inconsistent(uint64_t f, uint64_t told, uint64_t done) { return f && f <= told && f > done; }

/* ---- pending posts (nvn_vk.c post_op): kind 1 an event value (4 bytes at val), kind 2 a counter report (16 bytes at addr).
   A sealed slot's posts reach memory when the completion thread applies them, just before g_done passes the slot. ---- */
static inline uint64_t told_post_len(int kind) { return kind == 1 ? 4 : 16; }
/* The post at a (of kind) touches [lo, hi). */
static inline int told_post_hits(int kind, uint64_t a, uint64_t lo, uint64_t hi) { return a < hi && lo < a + told_post_len(kind); }
/* A sealed slot with this value is pending at or below upto: the GPU has not completed it (value > done) and the game
   was told it is complete (value <= upto). */
static inline int told_slot_in(uint64_t value, uint64_t done, uint64_t upto) { return value > done && value <= upto; }

/* ---- the event post index: per 16-byte granule, how many sealed, unapplied event posts (kind 1) it holds. Kept by
   nvn_vk.c only while hle_nvn.c tracks the told fence: + at the seal (before the slot's value is published under g_mu),
   - after apply_posts wrote them (release), so a reader that finds 0 (acquire) sees every applied value. An event read
   whose granule reads 0 has no pending post: one load, no lock, no scan. A non-zero granule (a pending post there or a
   hash collision) sends the read to the exact ring scan. Only event posts are indexed: they are the reads a game may
   make many times a frame, and few posts a frame; counter reports (the bulk: the freeze logs' post-op capacity is
   1592-2294 entries over the ring) stay out of it and are scanned only for the rare timestamp conversions and
   invalidates. Measured on the host (test_told_fence.c) with 2008 posts pending: ~0.5 us per read for the scan under
   the lock, ~0.4 ns for the index. ---- */
enum { TOLD_PB_N = 4096 };   /* power of two; 16 KiB of counters */
static inline unsigned told_pb_slot(uint64_t a) {
    uint64_t g = a >> 4; g ^= g >> 17; g *= 0x9e3779b97f4a7c15ull;
    return (unsigned)(g >> 52) & (TOLD_PB_N - 1);
}
static inline void told_pb_add(_Atomic uint32_t *tab, uint64_t a) { atomic_fetch_add_explicit(&tab[told_pb_slot(a)], 1, memory_order_relaxed); }
static inline void told_pb_sub(_Atomic uint32_t *tab, uint64_t a) { atomic_fetch_sub_explicit(&tab[told_pb_slot(a)], 1, memory_order_release); }
/* 0: no event post pending at a; 1: maybe (scan). */
static inline int told_pb_maybe(_Atomic uint32_t *tab, uint64_t a) { return atomic_load_explicit(&tab[told_pb_slot(a)], memory_order_acquire) != 0; }
/* The index covers every pending slot once every slot above done was sealed while it was kept: since is the first value
   sealed with the index on (0: never on). */
static inline int told_pb_covers(uint64_t done, uint64_t since) { return since && done + 1 >= since; }

/* ---- census call sites: the most frequent guest return addresses of one kind of check, in a small table (space-saving
   heavy hitters: a new address takes the least-counted entry and inherits its count, so counts are upper bounds and a
   site seen more often than any table entry's count cannot be missing). Caller serialises. ---- */
enum { TOLD_SITES = 16 };
typedef struct { uint64_t lr, n; } told_site;
static inline void told_site_note(told_site *t, uint64_t lr) {
    int min = 0;
    for (int i = 0; i < TOLD_SITES; i++) {
        if (t[i].n && t[i].lr == lr) { t[i].n++; return; }
        if (t[i].n < t[min].n) min = i;
    }
    t[min].lr = lr; t[min].n++;
}
/* The k most frequent entries, most first, into out; returns how many. */
static inline int told_site_top(const told_site *t, told_site *out, int k) {
    told_site all[TOLD_SITES]; int m = 0;
    for (int i = 0; i < TOLD_SITES; i++) {
        if (!t[i].n) continue;
        int j = m++; all[j] = t[i];
        while (j > 0 && all[j - 1].n < all[j].n) { told_site x = all[j - 1]; all[j - 1] = all[j]; all[j] = x; j--; }
    }
    if (m > k) m = k;
    for (int i = 0; i < m; i++) out[i] = all[i];
    return m;
}

/* ---- per-thread memo of one address check: the slots at or below a given told fence are all sealed before the game
   was told it (the seal publishes a value before FenceSync can return it), so for a fixed told fence the newest pending
   slot that posts to an address never changes: the check is inconsistent exactly while that slot is not complete. A
   spin poll on one event or counter scans once per told fence, not once per read. ---- */
typedef struct { uint64_t a, told, need; } told_memo;
static inline int told_memo_hit(const told_memo *m, uint64_t a, uint64_t told) { return m->told == told && m->a == a; }
