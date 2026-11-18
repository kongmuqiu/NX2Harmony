/* The Legend of Zelda: Ocarina of Time (Switch 2, v131072): the game-code patch file of the shared runtime
   (runtime/game_patches.h), in place of Tears of the Kingdom's game_patches_v143.c and Z-A's game_patches_za.c. A Switch 2
   game (GAME_NS2), so the rest of what MacNativeGames' game_patches_ns2.c holds applies: no output-size override, no aspect
   patch yet.
   Patches this game needs on this host are added here, each checked against its original instructions first; on
   HarmonyOS they go into the signed guest library when main's text is exported (native-harmonyos/prepare_guest.sh
   oot131072, tools/export_text.c, compiled with TOTK_OHOS_EXPORT: CODE_AT_BUILD 0 writes them) and the device only checks
   them (TOTK_OHOS: CODE_AT_BUILD 1), as Tears of the Kingdom's game_patches_v143.c does.
     - the real-time frame step (patch_frame_step below, after seedds/nxnative's dm/games/0400486024DEC000/patches.py
       frame_step, MIT, and game_patches_v143.c patch_realtime / runtime_frame_step_update).
   Candidate not added: the dynamic-resolution level (main+0x805aa0 / 0x8065e0, nxnative's dynamic_resolution).
   Not added: MacNativeGames' acquire bit on exclusive loads (a Mac-only need: the phone's cores order memory as the
   Switch's do; Tears of the Kingdom and Z-A run without it here). */
#include "game_patches.h"
#include "code_guard.h"
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(TOTK_OHOS)
#define CODE_AT_BUILD 1   /* the phone: main's text comes from the signed libtotk_guest.so, the export wrote the words */
#else
#define CODE_AT_BUILD 0   /* the export (tools/export_text.c, TOTK_OHOS_EXPORT) writes them into main's text */
#endif

/* ---- Real-time frame step ----
   Once a frame the function at main+0x6e6be0 (0x9b4 bytes) divides its base rate (60.0, or 30.0 while bit 0 of
   [x21+0x24f] is set) by the int frame rate at [x21+0x2c8] (fdiv s8,s1,s0 at main+0x6e6cc4) and hands the ratio s8 to the
   game's time object, which the game steps by. The frame's real duration never enters it, so at the 13-16 fps this phone
   reaches the game plays in slow motion (rate 30: half speed or worse). The site branches to a veneer in the text padding
   after TEXT_END that hands the game the host's measured step instead:
       ratio = step x base / 60, step = the time between the game's presents in 60 Hz ticks (seconds x 60),
   which is the division's own value when frames really take 60 / rate ticks, so one second of real time is one second of
   game time at any frame rate. The step is a float on main's private page after the image (the 16 KiB tail of
   libtotk_guest.so, zero at load); 0 there (not measured yet, TOTK_OHOS_REALTIME=0) runs the game's own fdiv.
   Free registers (checked by disassembly of the whole function in the rewritten image, the words FN_SHA256 guards):
   no instruction of the function uses x16 / x17; on every path after the site s0 is written before it is read (the
   only calls before such a write, main+0x50b20 and +0x50ea0, take x0..x2 and reach nn::os::GetTlsValue / LockMutex,
   no float argument); s1 (the base) is only read; NZCV is untouched (no flag-setting instruction in the veneer).
   Data page (main+DATA): +0 the step in ticks (float bits, the host writes), +4 the ratio the veneer last handed the
   game (either path, for the log), +8 veneer runs (u32, for the log). */
#define FS_FN 0x6e6be0ULL
#define FS_FN_SIZE 0x9b4u
#define FS_SITE 0x6e6cc4ULL
#define FS_ORIG 0x1e201828u   /* fdiv s8,s1,s0 (little-endian bytes 28 18 20 1e) */
#define FS_ORIG_INDEX 12      /* the veneer word that replays it */
#define FS_WORDS 15
/* SHA-256 of main's 621 words at [FS_FN, FS_FN + FS_FN_SIZE) in the verified rewrite (pacibsp / autibsp already nop),
   the site holding FS_ORIG: the function the free-register analysis above was made on. */
static const char fs_fn_sha256[] = "1086d05879af109d0775093f59a205a8102938548196523182187166a11212da";
#define FS_CAP_S 0.100        /* a present interval counts at most this long (see runtime_frame_step_update) */
#define FS_RESET_S 0.250      /* longer: a load, the background, a pause - the five-sample window starts over */
#define FS_LOG_PRESENTS 300

static uint32_t fs_adrp(unsigned reg, uint64_t pc, uint64_t target) {
    int64_t pages = (int64_t)(target >> 12) - (int64_t)(pc >> 12);
    if (pages < -(1 << 20) || pages >= (1 << 20)) runtime_fail("OoT frame step: data page outside ADRP range");
    uint32_t imm = (uint32_t)pages & 0x1fffff;
    return 0x90000000u | ((imm & 3) << 29) | ((imm >> 2) << 5) | reg;
}
static uint32_t fs_branch(uint64_t pc, uint64_t target) {
    int64_t delta = (int64_t)target - (int64_t)pc;
    if ((delta & 3) || delta < -(1 << 27) || delta >= (1 << 27)) runtime_fail("OoT frame step: branch outside range");
    return 0x14000000u | (((uint32_t)(delta >> 2)) & 0x3ffffff);
}
/* The veneer at stub (module offsets: ADRP and B are module-relative, the mapping is 4 KiB aligned). */
static void fs_veneer(uint32_t code[FS_WORDS], uint64_t stub, uint64_t data) {
    const uint32_t words[FS_WORDS] = {
        fs_adrp(16, stub, data),   /*  0 adrp x16, data page                                                      */
        0xb9400a11,                /*  1 ldr  w17,[x16,#8]                                                        */
        0x11000631,                /*  2 add  w17,w17,#1                                                          */
        0xb9000a11,                /*  3 str  w17,[x16,#8]      veneer runs                                       */
        0xb9400211,                /*  4 ldr  w17,[x16]         the step in 60 Hz ticks (float bits)              */
        0x340000f1,                /*  5 cbz  w17, 12           0: the game's own division                        */
        0x1e270220,                /*  6 fmov s0,w17            s0 = step (s0 is dead after the site)             */
        0x1e210808,                /*  7 fmul s8,s0,s1          step x base                                       */
        0x52a84e11,                /*  8 mov  w17,#0x42700000   60.0                                              */
        0x1e270220,                /*  9 fmov s0,w17                                                              */
        0x1e201908,                /* 10 fdiv s8,s8,s0          / 60                                              */
        0x14000002,                /* 11 b    13                                                                  */
        FS_ORIG,                   /* 12 fdiv s8,s1,s0          the game's own ratio (replayed)                   */
        0xbd000608,                /* 13 str  s8,[x16,#4]       the ratio handed to the game, for the log         */
        fs_branch(stub + 4 * 14, FS_SITE + 4),   /* 14 b main+0x6e6cc8                                          */
    };
    memcpy(code, words, sizeof words);
}

static _Atomic uint32_t *fs_step, *fs_ratio, *fs_runs;   /* the data page's words; NULL until installed */
static int fs_on;                                        /* TOTK_OHOS_REALTIME (default 1) */
static _Atomic uint32_t fs_pin_us;                       /* TOTK_OHOS_REALTIME_PIN_MS / "framestep <ms>"; 0 = measured */
static uint64_t fs_last_ns;
static double fs_samples[5];
static unsigned fs_nsamples, fs_next;
/* the log window (present thread only) */
static uint64_t fs_win_ns;
static unsigned fs_win_presents, fs_win_steps, fs_win_capped, fs_win_resets, fs_win_rn;
static double fs_win_step_sum, fs_win_rsum;
static float fs_win_rmin, fs_win_rmax;
static uint32_t fs_win_runs0;

static void fs_publish_seconds(double seconds) {
    float ticks = (float)(seconds * 60.0);
    uint32_t bits;
    memcpy(&bits, &ticks, 4);
    atomic_store_explicit(fs_step, bits, memory_order_release);
}
static float fs_word_float(_Atomic uint32_t *w) {
    uint32_t bits = atomic_load_explicit(w, memory_order_relaxed);
    float f;
    memcpy(&f, &bits, 4);
    return f;
}

static void patch_frame_step(Module *m) {
    const uint64_t data = (m->file_size + 0x3fff) & ~0x3fffULL;   /* main's private page after the image */
    const uint64_t stub = (m->text_end + 15) & ~15ULL;              /* the text padding after TEXT_END */
    /* the veneer stays below the next 4 KiB page (read-only data may start there; it does in this build: RO_OFF
       0x3c47000), the page lies within the module's mapping, the function within the text */
    if (data + 0x4000 != m->size || data + 12 > m->size ||
        stub + 4 * FS_WORDS > ((m->text_end + 0xfff) & ~0xfffULL) ||
        FS_FN + FS_FN_SIZE > m->text_end)
        runtime_fail("OoT frame step: patch layout (data page / veneer / function outside the module)");
    uint32_t code[FS_WORDS];
    fs_veneer(code, stub, data);
    const uint32_t jump = fs_branch(FS_SITE, stub);
    uint32_t site;
    memcpy(&site, m->base + FS_SITE, 4);
    uint32_t fn[FS_FN_SIZE / 4];
    memcpy(fn, m->base + FS_FN, sizeof fn);
    const char *e = getenv("TOTK_OHOS_REALTIME");
    if (e && *e && strcmp(e, "0") && strcmp(e, "1")) runtime_fail("TOTK_OHOS_REALTIME must be 0 or 1");
    fs_on = !(e && !strcmp(e, "0"));
#if CODE_AT_BUILD
    if (site == FS_ORIG) {   /* a guest library exported before this patch */
        if (!fs_on) {
            fprintf(stderr, "[oot] real-time frame step: off (TOTK_OHOS_REALTIME=0); this libtotk_guest.so carries no frame-step veneer: the game's own ratio\n");
            return;
        }
        fprintf(stderr, "[oot] real-time frame step: main+%#llx holds the original fdiv: libtotk_guest.so was exported without the veneer "
                        "(re-run native-harmonyos/prepare_guest.sh oot131072, then build_ohos.py --guest), or set TOTK_OHOS_REALTIME=0\n",
                (unsigned long long)FS_SITE);
        runtime_fail("OoT frame step: embedded patch missing (main+0x6e6cc4)");
    }
    if (site != jump) {
        fprintf(stderr, "[oot] real-time frame step main+%#llx: found %08x, expected %08x (b veneer)\n", (unsigned long long)FS_SITE, site, jump);
        runtime_fail("OoT frame step: embedded patch mismatch (main+0x6e6cc4)");
    }
    if (memcmp(m->base + stub, code, sizeof code)) {
        for (unsigned i = 0; i < FS_WORDS; i++) {
            uint32_t got;
            memcpy(&got, m->base + stub + 4 * i, 4);
            if (got != code[i]) fprintf(stderr, "[oot] frame-step veneer main+%#llx: found %08x, expected %08x\n", (unsigned long long)(stub + 4 * i), got, code[i]);
        }
        runtime_fail("OoT frame step: embedded veneer mismatch");
    }
    fn[(FS_SITE - FS_FN) / 4] = code[FS_ORIG_INDEX];   /* the function as the export found it: the word the veneer replays */
    if (!code_guard_words(fn, FS_FN_SIZE / 4, fs_fn_sha256)) runtime_fail("OoT frame step: main+0x6e6be0 is not the function the veneer was made for");
    for (unsigned i = 0; i < 12; i++)
        if (m->base[data + i]) runtime_fail("OoT frame step: data page already in use");
#else
    /* the export: the original instruction, then the whole function the free-register analysis was made on */
    if (site != FS_ORIG) {
        fprintf(stderr, "[oot] real-time frame step main+%#llx: found %08x, expected %08x (fdiv s8,s1,s0)\n", (unsigned long long)FS_SITE, site, FS_ORIG);
        runtime_fail("OoT frame step: original instruction mismatch (main+0x6e6cc4)");
    }
    if (!code_guard_words(fn, FS_FN_SIZE / 4, fs_fn_sha256)) runtime_fail("OoT frame step: main+0x6e6be0 is not the function the veneer was made for");
    for (unsigned i = 0; i < sizeof code; i++)
        if (m->base[stub + i]) runtime_fail("OoT frame step: veneer padding not empty");
    for (unsigned i = 0; i < 12; i++)
        if (m->base[data + i]) runtime_fail("OoT frame step: data page already in use");
    memcpy(m->base + stub, code, sizeof code);
    memcpy(m->base + FS_SITE, &jump, 4);
#endif
    fs_step = (_Atomic uint32_t *)(void *)(m->base + data);
    fs_ratio = (_Atomic uint32_t *)(void *)(m->base + data + 4);
    fs_runs = (_Atomic uint32_t *)(void *)(m->base + data + 8);
    atomic_store_explicit(fs_step, 0, memory_order_release);   /* not decided: the game's division until the first measurement */
    char mode[96];
    if (!fs_on) snprintf(mode, sizeof mode, "OFF (TOTK_OHOS_REALTIME=0): the veneer runs the game's own division");
    else {
        const char *p = getenv("TOTK_OHOS_REALTIME_PIN_MS");
        if (p && *p && strcmp(p, "0")) {
            char *end;
            double ms = strtod(p, &end);
            if (end == p || *end || !(ms > 0 && ms <= 100)) runtime_fail("TOTK_OHOS_REALTIME_PIN_MS must be milliseconds in (0, 100]");
            uint32_t us = (uint32_t)(ms * 1000.0 + 0.5);
            atomic_store(&fs_pin_us, us);
            fs_publish_seconds(us / 1e6);
            snprintf(mode, sizeof mode, "PINNED %.3f ms = %.3f ticks (TOTK_OHOS_REALTIME_PIN_MS)", us / 1000.0, us * 60e-6);
        } else snprintf(mode, sizeof mode, "on (TOTK_OHOS_REALTIME=0 restores the game's ratio)");
    }
    fprintf(stderr, "[oot] real-time frame step installed: main+%#llx fdiv -> veneer main+%#llx (%u words), data main+%#llx (%p, module size %#zx); "
                    "ratio = step x base / 60, step = mean of 5 present intervals x 60, each capped at %.0f ms (over %.0f ms resets); %s%s\n",
            (unsigned long long)FS_SITE, (unsigned long long)stub, FS_WORDS, (unsigned long long)data, (void *)(m->base + data), m->size,
            FS_CAP_S * 1000, FS_RESET_S * 1000, mode, CODE_AT_BUILD ? "" : " [written into the exported text]");
}

/* Once per guest present (hle_nvn.c nvnQueuePresentTexture, before the frame that follows computes its ratio): the
   interval since the previous present, a five-sample mean (TOTK's runtime_frame_step_update), published as the step.
   Cap 100 ms, not TOTK's 1/15 s: TOTK runs at 30-60 fps where 1/15 s is two frames of headroom, but this phone runs
   OoT at 13-16 fps (62-77 ms a frame), so 1/15 s would clip ordinary frames and leave the game ~13 % slow at 13 fps;
   100 ms (a 10 fps floor, as nxnative's and BananaNative's caps) covers the phone's range with jitter to spare and still
   bounds one frame's step at 6 ticks (three of the game's own 30 fps steps) when a frame hitches (a shader compile, a
   stream-in): that time is dropped rather than replayed as one huge physics step. Over 250 ms (a load, the background)
   the window starts over and the last step stays. */
void runtime_frame_step_update(uint64_t now_ns) {
    if (!fs_step || !now_ns) return;
    uint32_t pin = atomic_load_explicit(&fs_pin_us, memory_order_relaxed);
    if (!fs_win_ns) fs_win_ns = now_ns;   /* the first window's runs count from install (the page was zero) */
    float r = fs_word_float(fs_ratio);   /* the ratio the frame just presented was stepped by */
    if (isfinite(r) && r > 0) {
        if (!fs_win_rn || r < fs_win_rmin) fs_win_rmin = r;
        if (!fs_win_rn || r > fs_win_rmax) fs_win_rmax = r;
        fs_win_rsum += r;
        fs_win_rn++;
    }
    if (fs_last_ns && now_ns > fs_last_ns) {
        double dt = (double)(now_ns - fs_last_ns) * 1e-9;
        if (dt > FS_RESET_S || dt < 0.001) {
            fs_nsamples = fs_next = 0;
            fs_win_resets++;
        } else {
            if (dt > FS_CAP_S) { dt = FS_CAP_S; fs_win_capped++; }
            fs_samples[fs_next] = dt;
            fs_next = (fs_next + 1) % 5;
            if (fs_nsamples < 5) fs_nsamples++;
            double sum = 0;
            for (unsigned i = 0; i < fs_nsamples; i++) sum += fs_samples[i];
            if (fs_on && !pin) fs_publish_seconds(sum / fs_nsamples);
        }
    }
    fs_last_ns = now_ns;
    /* "framestep <ms>" runs on the input thread: a pin stored after the load above would otherwise be overwritten by
       the measured step just published and never published again (pinned presents publish nothing); re-assert it */
    uint32_t pin_now = atomic_load_explicit(&fs_pin_us, memory_order_relaxed);
    if (fs_on && pin_now) fs_publish_seconds(pin_now / 1e6);
    float step = fs_word_float(fs_step);
    if (step > 0) { fs_win_step_sum += step; fs_win_steps++; }
    if (++fs_win_presents < FS_LOG_PRESENTS) return;
    double wall = (double)(now_ns - fs_win_ns) * 1e-9;
    uint32_t runs = atomic_load_explicit(fs_runs, memory_order_relaxed) - fs_win_runs0;
    char steps[64], ratios[96];
    if (fs_win_steps) snprintf(steps, sizeof steps, "%.1f ms = %.2f ticks", fs_win_step_sum / fs_win_steps / 0.06, fs_win_step_sum / fs_win_steps);
    else snprintf(steps, sizeof steps, "none published");
    if (fs_win_rn) snprintf(ratios, sizeof ratios, "%.3f..%.3f mean %.3f", fs_win_rmin, fs_win_rmax, fs_win_rsum / fs_win_rn);
    else snprintf(ratios, sizeof ratios, "not seen (veneer not run)");
    fprintf(stderr, "[oot] frame step: %u presents in %.2f s (%.1f fps); step mean %s (%u capped at %.0f ms, %u resets); "
                    "ratio %s, real time at base 60 = %.3f; veneer ran %u times; %s\n",
            fs_win_presents, wall, wall > 0 ? fs_win_presents / wall : 0.0, steps, fs_win_capped, FS_CAP_S * 1000, fs_win_resets,
            ratios, fs_win_presents ? 60.0 * wall / fs_win_presents : 0.0, runs,
            !fs_on ? "off (TOTK_OHOS_REALTIME=0): the game's ratio" : pin ? "pinned" : step > 0 ? "real time" : "not decided yet");
    fs_win_ns = now_ns;
    fs_win_runs0 += runs;
    fs_win_presents = fs_win_steps = fs_win_capped = fs_win_resets = fs_win_rn = 0;
    fs_win_step_sum = fs_win_rsum = 0;
}

/* "framestep <ms>" (input.c): pin the step for a test, 0 = back to the measured step (from the next present). */
void runtime_frame_step_pin(double ms) {
    uint32_t us = ms > 0 ? (uint32_t)(ms * 1000.0 + 0.5) : 0;
    atomic_store(&fs_pin_us, us);
    if (fs_step && fs_on && us) fs_publish_seconds(us / 1e6);
    fprintf(stderr, "[oot] frame step %s (pin %.3f ms)%s\n", us ? "pinned" : "real time", us / 1000.0,
            fs_step && fs_on ? "" : " -- but the real-time frame step is off, nothing changes");
}

void runtime_game_patches(Module *m) {
    if (strcmp(m->name, "main")) return;
    patch_frame_step(m);
}

/* What the shared runtime asks the game-patch file (runtime/runtime.h): Tears of the Kingdom's output and scene overrides
   and the aspect switch. Ocarina of Time has none of them yet: the game's own sizes. Its real-time frame step is above
   (runtime_frame_step_update: hle_nvn.c imports it weakly and calls it at every guest present). */
#include "runtime.h"
int runtime_output_res(unsigned *w, unsigned *h) { (void)w; (void)h; return 0; }
uint64_t runtime_output_res_extra_bytes(void) { return 0; }
int runtime_render_res(unsigned *w, unsigned *h) { (void)w; (void)h; return 0; }
int runtime_output_mode_cycle(void) { return 0; }
float runtime_aspect_now(void) { return 0; }
int runtime_aspect_toggle(void) { return 0; }
