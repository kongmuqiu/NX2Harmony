/* test_gpu_time_own.c (totk-ohos, two frames in flight, stage 1b): host test for TOTK_OHOS_GPU_TIME_OWN.
   Plain C, no Vulkan: it drives the upstream virtual GPU queue (android/gfx/vk_gpu_time.h, the header nvn_vk.c
   includes) the way nvn_vk.c's completion thread does, with and without the own-frame clamp that nvn_vk.c's
   gpu_time_own_frame applies when a new guest frame begins (end_ns and raw_end_ns clamped down to that frame's guest
   submit).

   The frame controller reads END - origin: the frame's last timestamp counter (vk_gpu_time_report) minus the CPU frame
   origin. Shown here: with an 11 ms backlog (the previous frame's virtual tail still past this frame's submit), END -
   origin equals the no-backlog value within 0.1 ms when the clamp is applied (OWN=1), and differs by the backlog when
   it is not (OWN=0). Exit code 0 = pass.

   Also shown, information only (totk-ohos review): the game's own dynamic resolution reads TIMESTAMP pairs, END minus a
   frame-start report (VULKAN.md "The game's own dynamic resolution"). The frame-start report is lifted by the monotonic
   report floor to the previous frame's END, so with OWN=1 and a backlog that pair reads max(own work - backlog, 0).

   Build and run (host gcc or clang; vk_gpu_time.h sits next to this file in the staged tree, or point -I at upstream
   games/totk/android/gfx):
     gcc -std=c11 -Wall -Wextra -I<upstream>/games/totk/android/gfx -o test_gpu_time_own test_gpu_time_own.c && ./test_gpu_time_own
   Not part of the device build: the Makefile's BACKEND_C list names its sources. */
#include <stdio.h>
#include <stdint.h>
#include "vk_gpu_time.h"

static uint64_t ms_ns(double ms) { return (uint64_t)(ms * 1e6 + 0.5); }
static double ns_ms(uint64_t ns) { return (double)ns / 1e6; }
static double dabs(double x) { return x < 0 ? -x : x; }

/* nvn_vk.c gpu_time_own_frame, the clamp half (the caller decides that a new guest frame begins here) */
static void own_frame_clamp(vk_gpu_time_queue *q, uint64_t guest_submit_ns) {
    if (q->end_ns > guest_submit_ns) q->end_ns = guest_submit_ns;
    if (q->raw_end_ns > guest_submit_ns) q->raw_end_ns = guest_submit_ns;
}

/* One guest frame as the completion thread sees it: guest submit at guest_ns, its GPU work work_ns in `slots` equal
   chunks executing back to back on the GPU from gpu_start_ns (>= guest_ns: the GPU takes it when it is free), each
   slot a TOP/BOTTOM bracket (vk_gpu_time_begin), the frame's END timestamp counter at the last slot's BOTTOM
   (vk_gpu_time_report), a frame-start timestamp counter at the first slot's TOP (the game's own resolution pair).
   Returns END - origin in ns; *lifted counts this frame's END reports raised by the monotonic floor (nvn_vk.c's
   'monotonic-clamps'); *pair_ns (optional) receives END - frame-start report. */
static uint64_t frame_end_origin(vk_gpu_time_queue *q, uint64_t guest_ns, uint64_t origin_ns, uint64_t gpu_start_ns,
                                 uint64_t work_ns, int slots, int own, int *lifted, uint64_t *pair_ns) {
    uint64_t t = gpu_start_ns, per = work_ns / (uint64_t)slots, report = 0, begin = 0;
    if (own) own_frame_clamp(q, guest_ns);
    for (int i = 0; i < slots; i++) {
        vk_gpu_time_slot s = vk_gpu_time_begin(q, guest_ns, t, t + per);
        if (i == 0) begin = vk_gpu_time_report(q, &s, t, 0);
        if (s.start_ns + per < q->report_ns) (*lifted)++;
        report = vk_gpu_time_report(q, &s, t + per, 0);
        t += per;
    }
    if (pair_ns) *pair_ns = report - begin;
    return report - origin_ns;
}

/* Two frames at 30 Hz: frame N-1, then frame N (the one measured). Without a backlog N-1 is an ordinary WORK frame;
   with one, N-1's virtual tail ends backlog_ms after N's guest submit (N-1 carried PERIOD + backlog of GPU work: a slow
   frame, or the running tail of two frames in flight). Frame N is identical in both arms: WORK in SLOTS chunks, taken
   by the GPU when N-1's work is done (DELAY after its submit when the GPU is idle), origin ORIGIN_LEAD before its
   submit. */
enum { SLOTS = 3 };
static const double PERIOD_MS = 1000.0 / 30, WORK_MS = 27.0, DELAY_MS = 2.0, ORIGIN_LEAD_MS = 4.0;

static double run(double backlog_ms, int own, int *lifted, double *pair_ms) {
    vk_gpu_time_queue q = {0, 0, 0};
    uint64_t g_prev = ms_ns(100.0), g = g_prev + ms_ns(PERIOD_MS);
    uint64_t prev_work = backlog_ms > 0 ? ms_ns(PERIOD_MS + backlog_ms) : ms_ns(WORK_MS);
    int lifted_prev = 0; uint64_t pair = 0;
    (void)frame_end_origin(&q, g_prev, g_prev - ms_ns(ORIGIN_LEAD_MS), g_prev + ms_ns(DELAY_MS), prev_work, SLOTS, own, &lifted_prev, NULL);
    uint64_t gpu_start = q.raw_end_ns > g + ms_ns(DELAY_MS) ? q.raw_end_ns : g + ms_ns(DELAY_MS);
    *lifted = 0;
    double end_origin = ns_ms(frame_end_origin(&q, g, g - ms_ns(ORIGIN_LEAD_MS), gpu_start, ms_ns(WORK_MS), SLOTS, own, lifted, &pair));
    if (pair_ms) *pair_ms = ns_ms(pair);
    return end_origin;
}

int main(void) {
    const double BACKLOG_MS = 11.0, TOL_MS = 0.1;
    int l00, l01, l10, l11, fail = 0;
    double p00, p01, p10, p11;
    double off_idle = run(0, 0, &l00, &p00), off_back = run(BACKLOG_MS, 0, &l01, &p01);
    double on_idle = run(0, 1, &l10, &p10), on_back = run(BACKLOG_MS, 1, &l11, &p11);
    printf("[gpu-time-own] frame N: %.3f ms of GPU work in %d slots, %.3f ms submits (30 Hz), origin %.3f ms before the submit, backlog %.3f ms\n",
           WORK_MS, SLOTS, PERIOD_MS, ORIGIN_LEAD_MS, BACKLOG_MS);
    printf("[gpu-time-own] OWN=0 no backlog: END-origin %.3f ms (reports lifted %d)\n", off_idle, l00);
    printf("[gpu-time-own] OWN=0 backlog:    END-origin %.3f ms (reports lifted %d): differs by %.3f ms, backlog %.3f ms  %s\n",
           off_back, l01, off_back - off_idle, BACKLOG_MS, dabs(off_back - off_idle - BACKLOG_MS) <= TOL_MS ? "PASS" : "FAIL");
    fail |= dabs(off_back - off_idle - BACKLOG_MS) > TOL_MS;
    printf("[gpu-time-own] OWN=1 no backlog: END-origin %.3f ms (reports lifted %d): same as OWN=0 within %.1f ms  %s\n",
           on_idle, l10, TOL_MS, dabs(on_idle - off_idle) <= TOL_MS ? "PASS" : "FAIL");
    fail |= dabs(on_idle - off_idle) > TOL_MS;
    printf("[gpu-time-own] OWN=1 backlog:    END-origin %.3f ms (reports lifted %d): differs by %.3f ms, within %.1f ms  %s\n",
           on_back, l11, on_back - on_idle, TOL_MS, dabs(on_back - on_idle) <= TOL_MS ? "PASS" : "FAIL");
    fail |= dabs(on_back - on_idle) > TOL_MS;
    {   /* information only: a backlog longer than the frame's own work. report_ns is not clamped, so END itself is
           lifted to the previous frame's last report and the benefit is capped there. */
        int l; double big = WORK_MS + 3.0, pbig, v = run(big, 1, &l, &pbig);
        printf("[gpu-time-own] info: OWN=1 backlog %.3f ms (> own work): END-origin %.3f ms (reports lifted %d): the monotonic report floor caps the benefit\n",
               big, v, l);
        /* information only: the game's own resolution pair, END minus the frame-start report (its first slot's TOP),
           which the floor lifts to the previous frame's END: max(own work - backlog, 0) with OWN=1 and a backlog */
        printf("[gpu-time-own] info: game TIMESTAMP pair END-start: OWN=0 %.3f/%.3f ms (no backlog/backlog), OWN=1 %.3f/%.3f/%.3f ms (no backlog/backlog/backlog %.3f ms): own work %.3f ms\n",
               p00, p01, p10, p11, pbig, big, WORK_MS);
    }
    printf("[gpu-time-own] %s\n", fail ? "FAIL" : "PASS");
    return fail;
}
