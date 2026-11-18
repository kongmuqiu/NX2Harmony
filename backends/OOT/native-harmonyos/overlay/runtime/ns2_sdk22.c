/* SDK 22 calls the host answers itself (GAME_NS2), small ones the Switch 2 SDK added or reaches through services the host
   does not serve. As seedds/nxnative answers them (dm/host, MIT): hle_hid.c (FinalizeSensorFusion, IsOunceVibrationDevice-
   Mounted), hle_pctl.c (CheckAgcPermissionUnderMinimumAge), hle_prepo.c (PlayReport::Send), hle_pl.c (RequestLoadNxFont).
   Registered in this file's table for runtime_ns2_sdk22_override (services.c). */
#include "runtime.h"
#include <stdint.h>
#include <string.h>

typedef struct { const char *name; void *fn; } s22_entry;
static s22_entry g_tab[32]; static int g_ntab;
static void s22_add(const char *name, void *fn) { if (g_ntab < (int)(sizeof g_tab / sizeof *g_tab)) g_tab[g_ntab++] = (s22_entry){name, fn}; else runtime_fail("ns2_sdk22.c: entry table full"); }
#define HLE_FORCE(sym, why, ret, fn, ...) \
    static ret fn(__VA_ARGS__); __attribute__((constructor)) static void fn##_entry(void) { s22_add(sym, (void *)fn); } static ret fn(__VA_ARGS__)

HLE_FORCE("_ZN2nn3hid20FinalizeSensorFusionEv", "the host keeps no IMU fusion state to disable", void, h_hid_FusionFin, void) {}
HLE_FORCE("_ZN2nn3hid29IsOunceVibrationDeviceMountedERKNS0_21VibrationDeviceHandleE", "no Switch 2 vibration device on the host", uint32_t, h_hid_VibMountedOunce, const uint32_t *h) { (void)h; return 0; }
HLE_FORCE("_ZN2nn4pctl33CheckAgcPermissionUnderMinimumAgeERKNS_7account3UidE", "networking is out of scope: never", uint32_t, h_pctl_CheckAgc, const void *uid) { (void)uid; return 0; }
HLE_FORCE("_ZN2nn5prepo10PlayReport4SendERKNS_7account3UidE", "telemetry: nothing is sent", uint32_t, h_prepo_Send, void *self, const void *uid) { (void)self; (void)uid; return 0; }
HLE_FORCE("_ZN2nn2pl17RequestLoadNxFontEPvm", "the host's shared fonts need none of the buffer", void, h_pl_ReqNxFont, void *buf, uint64_t size) { (void)buf; (void)size; }

void *runtime_ns2_sdk22_override(const char *name) {
    for (int i = 0; i < g_ntab; i++) if (!strcmp(name, g_tab[i].name)) return g_tab[i].fn;
    return NULL;
}
