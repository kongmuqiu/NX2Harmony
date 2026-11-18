#pragma once
#include <stdint.h>

/* The timer gates and all guest-visible GPU clocks must use this provider and the console's counter rate: the Switch's
   19.2 MHz, or the Switch 2's 31.25 MHz (GAME_NS2: 32 ns a tick; its nn::os::GetSystemTickFrequency answers 0x1dcd650).
   At 19.2 MHz a Switch 2 game measured every span 1.63 times too short and its frame-rate controller held 20 fps
   (seedds/nxnative c4b0618c, MIT; MacNativeGames 17f79ddc loader.c GUEST_TICKS). runtime_now_ns currently has
   CLOCK_MONOTONIC's epoch. */
uint64_t runtime_now_ns(void);
#ifdef GAME_NS2
static inline uint64_t guest_clock_ns_to_ticks(uint64_t ns) {
    return ns / 32;
}
static inline uint64_t guest_clock_ticks_to_ns(uint64_t ticks) {
    return ticks * 32;
}
#else
static inline uint64_t guest_clock_ns_to_ticks(uint64_t ns) {
    return (uint64_t)((__uint128_t)ns * 12 / 625);
}
static inline uint64_t guest_clock_ticks_to_ns(uint64_t ticks) {
    return ticks / 12 * 625 + ticks % 12 * 625 / 12;
}
#endif
/* Translate a calibrated host sample using a contemporaneous host/guest pair;
   valid even when the guest provider's epoch is hours away from the host's. */
static inline uint64_t guest_clock_rebase_ns(uint64_t ns, uint64_t host_anchor,
                                           uint64_t guest_anchor) {
    return guest_anchor + (ns - host_anchor);
}
