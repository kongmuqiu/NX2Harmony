/* Export main's patched text for the HarmonyOS embedding (host build, x86-64 Linux container).

   HarmonyOS, like iOS, executes code only from signed files, so the game patches the runtime applies to main's text on
   Android (runtime/game_patches.c) must already be in the text that goes into the signed guest library. The Mac does this
   for iOS by running the whole runtime in export mode (runtime/loader.c export_ios_text, TOTK_EXPORT_IOS). Here the same
   game_patches.c, compiled in unchanged, runs on the verified rewritten image in host memory: relocations never reach the
   text (they start at DATA_OFF), so the result differs from the image only at the patch sites, which is counted and
   printed. The patch code only computes module-relative words (ADRP / B between offsets of the same 4 KiB-aligned
   mapping), so the bytes do not depend on where host or device map the module.

   usage: export_text <rewritten dir> <out dir>    writes <out>/main.x0.bin = patched [0, page_up(TEXT_END)) */
#include "runtime.h"
#include "game_patches.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

_Noreturn void runtime_fail(const char *message) { fprintf(stderr, "export_text: FAIL: %s\n", message); exit(1); }
/* What game_patches.c calls outside patching (output-mode toggles at run time, the dev-only survey): never reached here. */
void runtime_survey_init(Module *m) { (void)m; if (getenv("TOTK_SURVEY")) runtime_fail("no survey in an export"); }
void runtime_oe_push_message(uint32_t message) { (void)message; runtime_fail("output toggle in an export"); }
int mfxup_output_enabled(void) { return 0; }
void dm_gfx_set_flag(const char *name, int value) { (void)name; (void)value; runtime_fail("graphics flag in an export"); }

/* SHA-256 for code_guard.h (host_os.c, whose other parts are arm64-only, has the runtime's copy). */
#define ROR(x, n) ((x) >> (n) | (x) << (32 - (n)))
static void sha_block(uint32_t h[8], const uint8_t *p) {
    static const uint32_t K[64] = {
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,
        0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,
        0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,
        0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
    uint32_t w[64];
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[4*i] << 24 | (uint32_t)p[4*i+1] << 16 | (uint32_t)p[4*i+2] << 8 | p[4*i+3];
    for (int i = 16; i < 64; i++)
        w[i] = w[i-16] + (ROR(w[i-15], 7) ^ ROR(w[i-15], 18) ^ (w[i-15] >> 3)) + w[i-7] + (ROR(w[i-2], 17) ^ ROR(w[i-2], 19) ^ (w[i-2] >> 10));
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], k = h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = k + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
        uint32_t t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        k = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += k;
}
void host_sha256(const void *data, size_t n, unsigned char out[32]) {
    uint32_t h[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    const uint8_t *p = data; size_t left = n;
    for (; left >= 64; p += 64, left -= 64) sha_block(h, p);
    uint8_t tail[128] = {0}; memcpy(tail, p, left); tail[left] = 0x80;
    size_t tn = left < 56 ? 64 : 128; uint64_t bits = (uint64_t)n * 8;
    for (int i = 0; i < 8; i++) tail[tn - 1 - i] = (uint8_t)(bits >> (8 * i));
    for (size_t o = 0; o < tn; o += 64) sha_block(h, tail + o);
    for (int i = 0; i < 8; i++) { out[4*i] = (uint8_t)(h[i] >> 24); out[4*i+1] = (uint8_t)(h[i] >> 16); out[4*i+2] = (uint8_t)(h[i] >> 8); out[4*i+3] = (uint8_t)h[i]; }
}
static void need(int c, const char *m) { if (!c) runtime_fail(m); }
static uint64_t page_up(uint64_t n) { return (n + 0x3fff) & ~0x3fffULL; }

static uint64_t fixup(const char *dir, const char *key) {
    char path[4096], line[256], k[64], v[64];
    snprintf(path, sizeof path, "%s/main.fixups.tsv", dir);
    FILE *f = fopen(path, "r"); need(f != NULL, "open main.fixups.tsv");
    while (fgets(line, sizeof line, f))
        if (sscanf(line, "%*s %63s %63s", k, v) == 2 && !strcmp(k, key)) { fclose(f); return strtoull(v, NULL, 16); }
    fclose(f); fprintf(stderr, "missing %s\n", key); runtime_fail("fixup key");
}

int main(int argc, char **argv) {
    need(argc == 3, "usage: export_text <rewritten dir> <out dir>");
    char path[4096]; snprintf(path, sizeof path, "%s/main.patched.bin", argv[1]);
    FILE *f = fopen(path, "rb"); need(f != NULL, "open main.patched.bin");
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET); need(n > 0, "image size");
    Module m; memset(&m, 0, sizeof m);
    m.name = "main"; m.file_size = (size_t)n; m.size = page_up(m.file_size) + 0x4000;   /* + the real-time frame-step page */
    m.text_end = fixup(argv[1], "TEXT_END"); m.patch_off = fixup(argv[1], "PATCH_OFF"); m.key_off = fixup(argv[1], "KEY_PAGE_OFF");
    m.base = mmap(NULL, m.size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    need(m.base != MAP_FAILED && !((uintptr_t)m.base & 0xfff), "image memory");
    need(fread(m.base, 1, m.file_size, f) == m.file_size, "read image"); fclose(f);
    size_t x0 = page_up(m.text_end);
    need(x0 <= m.key_off && m.key_off < m.patch_off && m.patch_off <= m.file_size, "layout");
    uint8_t *orig = malloc(x0); need(orig != NULL, "memory"); memcpy(orig, m.base, x0);
    runtime_game_patches(&m);
    unsigned words = 0;
    for (size_t k = 0; k < x0; k += 4) if (memcmp(orig + k, m.base + k, 4)) words++;
    /* the patches may write only the text (and main's private page after the image, which is not exported) */
    snprintf(path, sizeof path, "%s/main.patched.bin", argv[1]);
    f = fopen(path, "rb"); need(f != NULL, "reopen image");
    uint8_t *rest = malloc(m.file_size - x0); need(rest != NULL, "memory");
    fseek(f, (long)x0, SEEK_SET); need(fread(rest, 1, m.file_size - x0, f) == m.file_size - x0, "reread"); fclose(f);
    need(!memcmp(rest, m.base + x0, m.file_size - x0), "a game patch wrote outside main's text");
    snprintf(path, sizeof path, "%s/main.x0.bin", argv[2]);
    f = fopen(path, "wb"); need(f && fwrite(m.base, 1, x0, f) == x0 && fclose(f) == 0, "write main.x0.bin");
    printf("main text_bytes=%zu patched_words=%u\n", x0, words);
    return 0;
}
