/* Pokémon Legends: Z-A: every shader container in arc/data.trpfs, decoded with the game's own Oodle (ZA_SHADER_DUMP=<dir>).

   Trinity keeps almost everything in arc/data.trpfs: 'ONEPACK\0', the u64 offset of its root table {0 pack name hashes
   [u64], 1 pack offsets [u64]}, then the packs, each a FlatBuffers table {0 file hashes [u64], 1 files [{1 compression i8
   (3 = Oodle Mermaid, else stored), 2 level u8, 3 length u64, 4 data [u8]}]} (nxnative, MIT: dm/gfx/tools/shader_split.py
   trinity_shaders). Oodle is not open source; its decoder is linked into main (OodleLZ_Decompress, main+0x1a9c9e0 in
   2.0.2: the one function that takes the address of its own name; nxnative dm/host/hle_oodle.c). So the files are decoded
   here, in the game process, on the guest main thread right before nnMain (services.c observed_main), and the ones that
   hold BNSH shader files are written to <dir>/bnsh.pack: "ZABN", u32 1, then per file u64 hash, u64 length, the bytes.
   native-harmonyos/za/za_seed.py takes them from there to a full shader seed on the PC. The process exits afterwards. */
#include "runtime.h"
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

typedef int64_t (*oodle_fn)(const void *, int64_t, void *, int64_t, int64_t, int64_t, int64_t, void *, int64_t, void *, void *, void *, int64_t, int64_t);
#define ZA_OODLE_OFF 0x1a9c9e0u   /* main 2.0.2 (b1f12fd919eae86ab8a978317677e64b) */

static const uint8_t *g_d; static uint64_t g_n;
static uint32_t u32_(uint64_t at) { uint32_t v = 0; if (at + 4 <= g_n) memcpy(&v, g_d + at, 4); return v; }
static uint64_t u64_(uint64_t at) { uint64_t v = 0; if (at + 8 <= g_n) memcpy(&v, g_d + at, 8); return v; }
static uint64_t fb_root(uint64_t at) { return at + u32_(at); }
static int64_t fb_field(uint64_t t, int i) {   /* absolute offset of field i of the table at t, -1 when absent */
    int32_t so; memcpy(&so, g_d + t, 4); uint64_t vt = t - (int64_t)so; uint16_t vl, off;
    if (vt + 4 > g_n) return -1;
    memcpy(&vl, g_d + vt, 2); if (4 + 2 * (unsigned)i >= vl) return -1;
    memcpy(&off, g_d + vt + 4 + 2 * i, 2); return off ? (int64_t)(t + off) : -1;
}
static int fb_vec(uint64_t t, int i, uint64_t *start, uint32_t *count) {
    int64_t p = fb_field(t, i); if (p < 0) { *start = 0; *count = 0; return 0; }
    uint64_t v = (uint64_t)p + u32_((uint64_t)p); *count = u32_(v); *start = v + 4; return 1;
}

void za_shader_dump(void) {
    const char *rm = getenv("ZA_SHADER_DUMP_REMOVE");   /* =<dir>: the dump was pulled; its 7.5 GB go (the app sandbox is the app's own) */
    if (rm && *rm) { char p[4096]; snprintf(p, sizeof p, "%s/bnsh.pack", rm); fprintf(stderr, "[za-dump] removing %s: %s\n", p, unlink(p) ? strerror(errno) : "done"); rmdir(rm); }
    const char *dir = getenv("ZA_SHADER_DUMP");
    if (!dir || !*dir) return;
    char path[4096]; const char *game = getenv("TOTK_ROMFS");
    snprintf(path, sizeof path, "%s/romfs.bin", game ? game : ".");
    const char *o = getenv("ZA_TRPFS_OFFSET"), *s = getenv("ZA_TRPFS_SIZE");
    uint64_t off = o ? strtoull(o, NULL, 0) : 9877520ull, size = s ? strtoull(s, NULL, 0) : 4753821072ull;
    int fd = open(path, O_RDONLY);
    if (fd < 0) { fprintf(stderr, "[za-dump] 【红】%s: %s\n", path, strerror(errno)); _exit(3); }
    struct stat st; if (fstat(fd, &st) || (uint64_t)st.st_size < off + size) { fprintf(stderr, "[za-dump] 【红】%s is too small for data.trpfs\n", path); _exit(3); }
    const uint8_t *img = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (img == MAP_FAILED) { fprintf(stderr, "[za-dump] 【红】mmap %s: %s\n", path, strerror(errno)); _exit(3); }
    g_d = img + off; g_n = size;
    if (memcmp(g_d, "ONEPACK\0", 8)) { fprintf(stderr, "[za-dump] 【红】data.trpfs at %#llx is not ONEPACK\n", (unsigned long long)off); _exit(3); }
    const uint8_t *main_base = runtime_module_base("main");
    uint32_t first; memcpy(&first, main_base + ZA_OODLE_OFF, 4);
    if (first != 0xa9ba7bfdu) { fprintf(stderr, "[za-dump] 【红】main+%#x is not OodleLZ_Decompress's prologue (%#x)\n", ZA_OODLE_OFF, first); _exit(3); }
    oodle_fn oodle = (oodle_fn)(void *)(main_base + ZA_OODLE_OFF);
    mkdir(dir, 0770);
    snprintf(path, sizeof path, "%s/bnsh.pack", dir);
    FILE *out = fopen(path, "wb");
    if (!out) { fprintf(stderr, "[za-dump] 【红】%s: %s\n", path, strerror(errno)); _exit(3); }
    fwrite("ZABN", 1, 4, out); uint32_t ver = 1; fwrite(&ver, 4, 1, out);
    uint64_t root = fb_root(u64_(8)), packs; uint32_t npack;
    fb_vec(root, 1, &packs, &npack);
    size_t scratch_n = 16u << 20; void *scratch = malloc(scratch_n);
    uint8_t *raw = NULL; size_t raw_cap = 0;
    uint64_t files = 0, oodled = 0, stored = 0, failed = 0, kept = 0, kept_bytes = 0, decoded = 0;
    struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0);
    fprintf(stderr, "[za-dump] data.trpfs: %u packs; decoding with main+%#x into %s\n", npack, ZA_OODLE_OFF, path);
    for (uint32_t p = 0; p < npack; p++) {
        uint64_t pt = fb_root(u64_(packs + 8ull * p)), hashes, frefs; uint32_t nh, nf;
        fb_vec(pt, 0, &hashes, &nh); fb_vec(pt, 1, &frefs, &nf);
        for (uint32_t k = 0; k < nf; k++) {
            uint64_t ft = frefs + 4ull * k + u32_(frefs + 4ull * k);
            int64_t cf = fb_field(ft, 1), lf = fb_field(ft, 3); uint64_t dst; uint32_t dn;
            int8_t comp = cf >= 0 ? (int8_t)g_d[cf] : 0; uint64_t len = lf >= 0 ? u64_((uint64_t)lf) : 0;
            fb_vec(ft, 4, &dst, &dn); files++;
            const uint8_t *data;
            if (comp == 3) {
                if (len > raw_cap) { free(raw); raw_cap = len + (len >> 3); raw = malloc(raw_cap); if (!raw) { fprintf(stderr, "[za-dump] 【红】out of memory for %llu bytes\n", (unsigned long long)len); _exit(3); } }
                int64_t r = oodle(g_d + dst, dn, raw, (int64_t)len, 1, 0, 0, NULL, 0, NULL, NULL, scratch, (int64_t)scratch_n, 3);
                if (r != (int64_t)len) { if (failed++ < 8) fprintf(stderr, "[za-dump] 【红】pack %u file %u: Oodle gave %lld of %llu bytes\n", p, k, (long long)r, (unsigned long long)len); continue; }
                data = raw; oodled++;
            } else if (dn == len) { data = g_d + dst; stored++; }
            else { if (failed++ < 8) fprintf(stderr, "[za-dump] 【红】pack %u file %u: compression %d (%u -> %llu bytes) not known\n", p, k, comp, dn, (unsigned long long)len); continue; }
            decoded += len;
            if (len >= 8 && memmem(data, (size_t)len, "BNSH\0\0\0\0", 8)) {
                uint64_t h = k < nh ? u64_(hashes + 8ull * k) : 0;
                fwrite(&h, 8, 1, out); fwrite(&len, 8, 1, out); fwrite(data, 1, (size_t)len, out);
                kept++; kept_bytes += len;
            }
        }
        if ((p + 1) % 1000 == 0 || p + 1 == npack) {
            clock_gettime(CLOCK_MONOTONIC, &t1);
            fprintf(stderr, "[za-dump] %u/%u packs, %llu files (%llu Oodle, %llu stored, %llu failed), %.2f GB decoded, %llu with shaders (%.1f MB), %.1f s\n",
                    p + 1, npack, (unsigned long long)files, (unsigned long long)oodled, (unsigned long long)stored, (unsigned long long)failed, decoded / 1e9,
                    (unsigned long long)kept, kept_bytes / 1e6, (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9);
        }
    }
    fclose(out); free(raw); free(scratch);
    fprintf(stderr, "[za-dump] done: %llu files with shaders in %s\n", (unsigned long long)kept, path);
    fflush(stderr);
    _exit(0);
}
