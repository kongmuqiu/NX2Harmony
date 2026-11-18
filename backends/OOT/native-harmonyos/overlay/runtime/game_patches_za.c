/* Pokémon Legends: Z-A 2.0.2: the game-code patch file of the shared runtime (runtime/game_patches.h), in place of
   Tears of the Kingdom's game_patches_v143.c. Patches the game needs on this host are added here, each checked against
   its original instructions first, as game_patches.c does. On HarmonyOS the text patches go into the signed guest library
   when main's text is exported (native-harmonyos/prepare_guest.sh za202, tools/export_text.c); the device only checks
   them and writes the data page. */
#include "game_patches.h"
#include "aspect_patch.h"
#include <stdio.h>
#include <string.h>

/* Wide screen (runtime/aspect_patch.h, TOTK_ASPECT from the settings page's 画面比例): Trinity's camera parameters are
   {.., near +0x14, far +0x18, vertical field of view +0x1c, ratio +0x20, -960 960 -540 540 +0x24} (the layout
   main+0x2a73a20 reads; found on the phone with ZA_CAM_SCAN, za_hle.c). Every camera starts from the defaults
   {0.32, 6000, 30 degrees, 16:9} at main+0x319fc70, which three constructors load in one ldr q and store at +0x14; the
   ratio is never set again (all seven cameras of a station scene still held 16:9 while their fields of view differed).
   Those three loads are the sites: the stub replays the ldr q and, while a ratio is on the data page, puts it in lane 3,
   so projection, frustum culling and level of detail all see the wide camera. The picture is still drawn into the game's
   16:9 targets; the present stretches it over the screen (android/gfx/vk_draw.c runtime_aspect_now). */
static const uint64_t ZA_CAMERA_SITES[] = {0x7a1518, 0x18ce114, 0x1d1defc};
static const uint32_t ZA_CAMERA_WORDS[] = {0x3dc31d20, 0x3dc31d00, 0x3dc31d00};   /* ldr q0, [x9|x8, #0xc70] */

void runtime_game_patches(Module *m) {
    if (strcmp(m->name, "main")) return;
#if !defined(TOTK_OHOS)
    for (unsigned i = 0; i < 3; i++) {   /* the export: the game's own instructions first */
        uint32_t w; memcpy(&w, m->base + ZA_CAMERA_SITES[i], 4);
        if (w != ZA_CAMERA_WORDS[i]) runtime_fail("Z-A wide screen: the camera constructors do not match 2.0.2");
    }
#endif
    aspect_install(m, "Z-A 2.0.2", ZA_CAMERA_SITES, 3, (m->text_end + 15) & ~15ULL, (m->file_size + 0x3fff) & ~0x3fffULL);
}

/* What the shared runtime asks the game-patch file (runtime/runtime.h): Tears of the Kingdom's output and scene overrides
   and its real-time frame step. Z-A has none of them: the game's own sizes. (The aspect ratio's runtime_aspect_now and
   runtime_aspect_toggle come from aspect_patch.h.) */
#include "runtime.h"
int runtime_output_res(unsigned *w, unsigned *h) { (void)w; (void)h; return 0; }
uint64_t runtime_output_res_extra_bytes(void) { return 0; }
int runtime_render_res(unsigned *w, unsigned *h) { (void)w; (void)h; return 0; }
int runtime_output_mode_cycle(void) { return 0; }
void runtime_frame_step_pin(double ms) { (void)ms; }

/* The SDK functions the host answers for Z-A are in za_hle.c (runtime_za_override). */
