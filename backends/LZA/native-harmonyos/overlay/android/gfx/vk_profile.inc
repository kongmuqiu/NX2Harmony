/* TOTK_VK_PROFILE=1 | <file> (diagnostic): write the Vulkan driver's capabilities as a Vulkan Profiles JSON file (the
   profiles-0.8 schema `vulkaninfo --json` writes) to vk-profile.json or <file> in the app's work folder
   (files/android). HarmonyOS ships no vulkaninfo, and hdc shell does not run a self-built program (exec from
   /data/local/tmp is denied even when self-signed), so the export runs in the app's own process: at its start
   (ohos/totk_napi.c, once the env files are read), with an instance of its own, before the game, so the game data need
   not be on the phone (the launcher's 仍然开始 starts it without them). Pull it with
     hdc file recv -b <bundle> /data/storage/el2/base/haps/entry/files/android/vk-profile.json .
   What it holds, as vulkaninfo's export does: the device extensions; VkPhysicalDeviceFeatures and every struct
   extending VkPhysicalDeviceFeatures2 the device has (by its core version or an extension it reports), less the ones
   promoted into a VkPhysicalDeviceVulkanXYFeatures it also has; the same for properties; VkFormatProperties (and
   VkFormatProperties3, VkSubpassResolvePerformanceQueryEXT) of every format with any feature; the queue families.
   The printers and tables are generated from vk.xml (vk_profile_gen.inc, native-harmonyos/tools/gen_vk_profile.py).
   Beyond the instance it only queries the physical device. Checked on lavapipe against vulkaninfo's own export. */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct { uint64_t value; const char *name; } pf_name;
typedef struct pj { FILE *f; int depth; int first[64]; int after_key; const char *key; } pj;
typedef struct {
    int32_t stype; size_t size; uint32_t core; const char *name; const char *exts; int covered;
    void (*print)(pj *, const void *); int (*fill)(void *); void (*free_)(void *);
} pf_struct;
typedef struct { int32_t value; const char *name; uint32_t core; const char *exts; } pf_format;

/* ---- a small JSON writer (tab indented, as vulkaninfo's) ---- */
static void pj_nl(pj *j) { fputc('\n', j->f); for (int i = 0; i < j->depth; i++) fputc('\t', j->f); }
static void pj_sep(pj *j) {
    if (j->after_key) { j->after_key = 0; return; }
    if (j->depth > 0) { if (!j->first[j->depth]) fputc(',', j->f); j->first[j->depth] = 0; pj_nl(j); }
}
static void pj_str_raw(pj *j, const char *s, size_t n) {
    fputc('"', j->f);
    for (size_t i = 0; i < n && s[i]; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"' || c == '\\') { fputc('\\', j->f); fputc(c, j->f); }
        else if (c < 0x20) fprintf(j->f, "\\u%04x", c);
        else fputc(c, j->f);
    }
    fputc('"', j->f);
}
static void pj_key(pj *j, const char *k) { pj_sep(j); pj_str_raw(j, k, strlen(k)); fputs(": ", j->f); j->after_key = 1; j->key = k; }
static void pj_open(pj *j, int c) { pj_sep(j); fputc(c, j->f); if (j->depth < 63) j->depth++; j->first[j->depth] = 1; }
static void pj_close(pj *j, int c) { int empty = j->first[j->depth]; j->depth--; if (!empty) pj_nl(j); fputc(c, j->f); }
static void pj_obj(pj *j) { pj_open(j, '{'); }
static void pj_end_obj(pj *j) { pj_close(j, '}'); }
static void pj_arr(pj *j) { pj_open(j, '['); }
static void pj_end_arr(pj *j) { pj_close(j, ']'); }
static void pj_key_obj(pj *j, const char *k) { pj_key(j, k); pj_obj(j); }
static void pj_key_arr(pj *j, const char *k) { pj_key(j, k); pj_arr(j); }
static void pj_num_u(pj *j, uint64_t v) { pj_sep(j); fprintf(j->f, "%llu", (unsigned long long)v); }
static void pj_num_i(pj *j, int64_t v) { pj_sep(j); fprintf(j->f, "%lld", (long long)v); }
static void pj_num_f(pj *j, double v) { pj_sep(j); if (isfinite(v)) fprintf(j->f, "%.9g", v); else fputs("0", j->f); }
static void pj_num_b(pj *j, uint32_t v) { pj_sep(j); fputs(v ? "true" : "false", j->f); }
static void pj_str(pj *j, const char *s) { pj_sep(j); pj_str_raw(j, s, strlen(s)); }
static void pj_str_n(pj *j, const char *s, size_t n) { pj_sep(j); pj_str_raw(j, s, n); }
static void pj_enum(pj *j, const pf_name *t, int64_t v) {
    for (; t->name; t++) if (t->value == (uint64_t)v) { pj_str(j, t->name); return; }
    pj_num_i(j, v);   /* a value newer than the registry the tables came from */
}
static int g_pf_unknown_bits; static char g_pf_unknown_first[96];
static void pj_flags(pj *j, const pf_name *t, uint64_t v) {
    const char *key = j->key;
    pj_arr(j);
    for (; t->name; t++) if (v & t->value) { pj_str(j, t->name); v &= ~t->value; }
    if (v && !g_pf_unknown_bits++)   /* bits the registry has no name for: left out (the schema takes names only) */
        snprintf(g_pf_unknown_first, sizeof g_pf_unknown_first, "%s %#llx", key ? key : "?", (unsigned long long)v);
    pj_end_arr(j);
}

#include "vk_profile_gen.inc"

/* ---- the export ---- */
static VkExtensionProperties *g_pf_ext; static uint32_t g_pf_next;
static int pf_has_ext(const char *name, size_t n) {
    for (uint32_t i = 0; i < g_pf_next; i++) if (strlen(g_pf_ext[i].extensionName) == n && !strncmp(g_pf_ext[i].extensionName, name, n)) return 1;
    return 0;
}
/* Is a struct or format available, and under which name: core version <= api, else an extension the device reports
   ("VK_EXT_x=VkStructEXT|..." for structs, "VK_EXT_x|..." for formats). */
static const char *pf_avail(uint32_t api, uint32_t core, const char *canonical, const char *exts, char *buf, size_t bufn) {
    if (core && api >= core) return canonical;
    for (const char *p = exts; p && *p; ) {
        const char *bar = strchr(p, '|'), *end = bar ? bar : p + strlen(p), *eq = memchr(p, '=', (size_t)(end - p));
        if (pf_has_ext(p, (size_t)((eq ? eq : end) - p))) {
            if (!eq) return canonical;
            size_t n = (size_t)(end - eq - 1); if (n >= bufn) n = bufn - 1;
            memcpy(buf, eq + 1, n); buf[n] = 0; return buf;
        }
        p = bar ? bar + 1 : end;
    }
    return NULL;
}
static int pf_ext_cmp(const void *a, const void *b) { return strcmp(((const VkExtensionProperties *)a)->extensionName, ((const VkExtensionProperties *)b)->extensionName); }
static void pf_driver_version(const VkPhysicalDeviceProperties *p, char *out, size_t n) {
    uint32_t v = p->driverVersion;
    if (p->vendorID == 0x10DE) snprintf(out, n, "%u.%u.%u.%u", (v >> 22) & 0x3ff, (v >> 14) & 0xff, (v >> 6) & 0xff, v & 0x3f);
    else snprintf(out, n, "%u.%u.%u", VK_API_VERSION_MAJOR(v), VK_API_VERSION_MINOR(v), VK_API_VERSION_PATCH(v));
}

typedef struct { int feat, prop, fmt, qf; long bytes; } pf_counts;

/* inst_api: the instance's apiVersion; core structs are used up to the lower of it and the device's. */
static int vk_profile_write(PFN_vkGetInstanceProcAddr gipa, VkInstance inst, uint32_t inst_api, VkPhysicalDevice pd, const char *path, pf_counts *cnt) {
#define PF_FN(name) PFN_##name pf_##name = (PFN_##name)gipa(inst, #name); if (!pf_##name) return -1
    PF_FN(vkGetPhysicalDeviceProperties); PF_FN(vkGetPhysicalDeviceFeatures); PF_FN(vkEnumerateDeviceExtensionProperties);
    PF_FN(vkGetPhysicalDeviceProperties2); PF_FN(vkGetPhysicalDeviceFeatures2); PF_FN(vkGetPhysicalDeviceFormatProperties2);
    PF_FN(vkGetPhysicalDeviceQueueFamilyProperties2);
#undef PF_FN
    memset(cnt, 0, sizeof *cnt);
    VkPhysicalDeviceProperties props; pf_vkGetPhysicalDeviceProperties(pd, &props);
    uint32_t api = props.apiVersion < inst_api ? props.apiVersion : inst_api;
    pf_vkEnumerateDeviceExtensionProperties(pd, NULL, &g_pf_next, NULL);
    g_pf_ext = calloc(g_pf_next ? g_pf_next : 1, sizeof *g_pf_ext);
    pf_vkEnumerateDeviceExtensionProperties(pd, NULL, &g_pf_next, g_pf_ext);
    qsort(g_pf_ext, g_pf_next, sizeof *g_pf_ext, pf_ext_cmp);
    char tmp[1100]; snprintf(tmp, sizeof tmp, "%s.part", path);
    FILE *f = fopen(tmp, "w");
    if (!f) { free(g_pf_ext); g_pf_ext = NULL; return -2; }
    pj J = {.f = f, .first = {1}}, *j = &J; char nb[256];
    pj_obj(j);
    pj_key(j, "$schema"); pj_str(j, "https://schema.khronos.org/vulkan/profiles-0.8-latest.json");
    pj_key_obj(j, "capabilities"); pj_key_obj(j, "device");

    pj_key_obj(j, "extensions");
    for (uint32_t i = 0; i < g_pf_next; i++) { pj_key(j, g_pf_ext[i].extensionName); pj_num_u(j, g_pf_ext[i].specVersion); }
    pj_end_obj(j);

    pj_key_obj(j, "features");
    { VkPhysicalDeviceFeatures f0; pf_vkGetPhysicalDeviceFeatures(pd, &f0);
      pj_key_obj(j, "VkPhysicalDeviceFeatures"); pf_print_VkPhysicalDeviceFeatures(j, &f0); pj_end_obj(j); cnt->feat++; }
    for (const pf_struct *e = pf_feat; e->name; e++) {
        const char *name = pf_avail(api, e->core, e->name, e->exts, nb, sizeof nb);
        if (!name || (e->covered && api >= e->core)) continue;
        VkBaseOutStructure *s = calloc(1, e->size); s->sType = (VkStructureType)e->stype;
        VkPhysicalDeviceFeatures2 f2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = s};
        pf_vkGetPhysicalDeviceFeatures2(pd, &f2);
        pj_key_obj(j, name); e->print(j, s); pj_end_obj(j); cnt->feat++;
        free(s);
    }
    pj_end_obj(j);

    pj_key_obj(j, "properties");
    pj_key_obj(j, "VkPhysicalDeviceProperties"); pf_print_VkPhysicalDeviceProperties(j, &props); pj_end_obj(j); cnt->prop++;
    for (const pf_struct *e = pf_prop; e->name; e++) {
        const char *name = pf_avail(api, e->core, e->name, e->exts, nb, sizeof nb);
        if (!name || (e->covered && api >= e->core)) continue;
        VkBaseOutStructure *s = calloc(1, e->size); s->sType = (VkStructureType)e->stype;
        VkPhysicalDeviceProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = s};
        pf_vkGetPhysicalDeviceProperties2(pd, &p2);
        if (e->fill && e->fill(s)) { s->pNext = NULL; pf_vkGetPhysicalDeviceProperties2(pd, &p2); }   /* output arrays: counts first */
        pj_key_obj(j, name); e->print(j, s); pj_end_obj(j); cnt->prop++;
        if (e->free_) e->free_(s);
        free(s);
    }
    pj_end_obj(j);

    /* formats: the format structs the device has, chained once per format */
    const pf_struct *fs[8]; const char *fsn[8]; char fsb[8][128]; int nfs = 0;
    for (const pf_struct *e = pf_fmt; e->name && nfs < 8; e++) {
        const char *name = pf_avail(api, e->core, e->name, e->exts, fsb[nfs], sizeof fsb[nfs]);
        if (name) { fs[nfs] = e; fsn[nfs] = name; nfs++; }
    }
    pj_key_obj(j, "formats");
    for (const pf_format *e = pf_formats; e->name; e++) {
        if (!pf_avail(api, e->core, e->name, e->exts, nb, sizeof nb)) continue;
        VkBaseOutStructure *sub[8] = {0};
        VkFormatProperties2 fp2 = {.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2};
        void **link = &fp2.pNext;
        for (int k = 0; k < nfs; k++) { sub[k] = calloc(1, fs[k]->size); sub[k]->sType = (VkStructureType)fs[k]->stype; *link = sub[k]; link = (void **)&sub[k]->pNext; }
        pf_vkGetPhysicalDeviceFormatProperties2(pd, (VkFormat)e->value, &fp2);
        const VkFormatProperties *b = &fp2.formatProperties;
        if (b->linearTilingFeatures || b->optimalTilingFeatures || b->bufferFeatures) {
            pj_key_obj(j, e->name);
            pj_key_obj(j, "VkFormatProperties"); pf_print_VkFormatProperties(j, b); pj_end_obj(j);
            for (int k = 0; k < nfs; k++) { pj_key_obj(j, fsn[k]); fs[k]->print(j, sub[k]); pj_end_obj(j); }
            pj_end_obj(j); cnt->fmt++;
        }
        for (int k = 0; k < nfs; k++) free(sub[k]);
    }
    pj_end_obj(j);

    /* queue families */
    const pf_struct *qs[4]; const char *qsn[4]; char qsb[4][128]; int nqs = 0;
    for (const pf_struct *e = pf_qf; e->name && nqs < 4; e++) {
        const char *name = pf_avail(api, e->core, e->name, e->exts, qsb[nqs], sizeof qsb[nqs]);
        if (name) { qs[nqs] = e; qsn[nqs] = name; nqs++; }
    }
    uint32_t nq = 0; pf_vkGetPhysicalDeviceQueueFamilyProperties2(pd, &nq, NULL);
    VkQueueFamilyProperties2 *q = calloc(nq ? nq : 1, sizeof *q);
    VkBaseOutStructure **qsub = calloc((size_t)(nq ? nq : 1) * 4, sizeof *qsub);
    for (uint32_t i = 0; i < nq; i++) {
        q[i].sType = VK_STRUCTURE_TYPE_QUEUE_FAMILY_PROPERTIES_2; void **link = &q[i].pNext;
        for (int k = 0; k < nqs; k++) { VkBaseOutStructure *s = calloc(1, qs[k]->size); s->sType = (VkStructureType)qs[k]->stype; qsub[i * 4 + k] = s; *link = s; link = (void **)&s->pNext; }
    }
    pf_vkGetPhysicalDeviceQueueFamilyProperties2(pd, &nq, q);
    pj_key_arr(j, "queueFamiliesProperties");
    for (uint32_t i = 0; i < nq; i++) {
        pj_obj(j);
        pj_key_obj(j, "VkQueueFamilyProperties"); pf_print_VkQueueFamilyProperties(j, &q[i].queueFamilyProperties); pj_end_obj(j);
        for (int k = 0; k < nqs; k++) { pj_key_obj(j, qsn[k]); qs[k]->print(j, qsub[i * 4 + k]); pj_end_obj(j); }
        pj_end_obj(j); cnt->qf++;
    }
    pj_end_arr(j);
    for (uint32_t i = 0; i < nq * 4; i++) free(qsub[i]);
    free(qsub); free(q);
    pj_end_obj(j); pj_end_obj(j);   /* device, capabilities */

    /* the profile that names these capabilities */
    char drv[64], pname[512], label[512], apis[32], date[16];
    pf_driver_version(&props, drv, sizeof drv);
    snprintf(pname, sizeof pname, "VP_LZA_%s_%s", props.deviceName, drv);
    for (char *c = pname; *c; c++) if (!((*c >= 'A' && *c <= 'Z') || (*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9'))) *c = '_';
    snprintf(label, sizeof label, "%s driver %s", props.deviceName, drv);
    snprintf(apis, sizeof apis, "%u.%u.%u", VK_API_VERSION_MAJOR(props.apiVersion), VK_API_VERSION_MINOR(props.apiVersion), VK_API_VERSION_PATCH(props.apiVersion));
    { time_t t = time(NULL); struct tm tm; gmtime_r(&t, &tm); strftime(date, sizeof date, "%Y-%m-%d", &tm); }
    pj_key_obj(j, "profiles"); pj_key_obj(j, pname);
    pj_key(j, "version"); pj_num_u(j, 1);
    pj_key(j, "api-version"); pj_str(j, apis);
    pj_key(j, "label"); pj_str(j, label);
    pj_key(j, "description"); pj_str(j, "Exported from the LZA runtime (TOTK_VK_PROFILE), in vulkaninfo's format");
    pj_key_obj(j, "contributors"); pj_end_obj(j);
    pj_key_arr(j, "history"); pj_obj(j);
    pj_key(j, "revision"); pj_num_u(j, 1); pj_key(j, "date"); pj_str(j, date);
    pj_key(j, "author"); pj_str(j, "Automated export from the LZA runtime"); pj_key(j, "comment"); pj_str(j, "");
    pj_end_obj(j); pj_end_arr(j);
    pj_key_arr(j, "capabilities"); pj_str(j, "device"); pj_end_arr(j);
    pj_end_obj(j); pj_end_obj(j);   /* profile, profiles */
    pj_end_obj(j); fputc('\n', f);
    cnt->bytes = ftell(f);
    int bad = ferror(f); bad |= fclose(f);
    free(g_pf_ext); g_pf_ext = NULL; g_pf_next = 0;
    if (bad || rename(tmp, path)) { remove(tmp); return -2; }
    return 0;
}

static void vk_profile_dump_env(PFN_vkGetInstanceProcAddr gipa, VkInstance inst, uint32_t inst_api, VkPhysicalDevice pd) {
    const char *e = getenv("TOTK_VK_PROFILE");
    if (!e || !*e || !strcmp(e, "0")) return;
    const char *path = strcmp(e, "1") ? e : "vk-profile.json";
    pf_counts c; int r = vk_profile_write(gipa, inst, inst_api, pd, path, &c);
    if (r) LOGV("Vulkan profile not written to %s: %s (TOTK_VK_PROFILE)", path, r == -1 ? "a physical-device query is missing" : "cannot write the file");
    else LOGV("Vulkan profile written to %s: %ld bytes, %d feature and %d property structs, %d formats, %d queue families%s%s%s (TOTK_VK_PROFILE)",
              path, c.bytes, c.feat, c.prop, c.fmt, c.qf, g_pf_unknown_bits ? "; flag bits without a name in the registry left out, first: " : "",
              g_pf_unknown_bits ? g_pf_unknown_first : "", g_pf_unknown_bits > 1 ? " ..." : "");
}

#ifdef TOTK_OHOS
/* The app's start (ohos/totk_napi.c start, in the work folder, after the env files): an instance of its own at the
   loader's version (so a 1.4 device's core structs may be queried), the first device, the export, gone again before
   the game makes its own. */
void vk_profile_dump_at_start(void) {
    const char *e = getenv("TOTK_VK_PROFILE");
    if (!e || !*e || !strcmp(e, "0")) return;
    const char *kind, *why, *drv = getenv("TOTK_VK_DRIVER");
    if (drv && (!*drv || !strcmp(drv, "system"))) drv = NULL;
    PFN_vkGetInstanceProcAddr g = vk_load_driver(drv, &kind, &why);
    if (!g) { LOGV("Vulkan profile: no driver: %s (TOTK_VK_PROFILE)", why); return; }
    PFN_vkEnumerateInstanceVersion ev = (PFN_vkEnumerateInstanceVersion)g(NULL, "vkEnumerateInstanceVersion");
    uint32_t api = VK_API_VERSION_1_1; if (ev) ev(&api);
    PFN_vkCreateInstance ci = (PFN_vkCreateInstance)g(NULL, "vkCreateInstance");
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "totk-native-profile", .apiVersion = api};
    VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
    VkInstance inst;
    if (!ci || ci(&ici, NULL, &inst) != VK_SUCCESS) { LOGV("Vulkan profile: no instance (TOTK_VK_PROFILE)"); return; }
    PFN_vkEnumeratePhysicalDevices ep = (PFN_vkEnumeratePhysicalDevices)g(inst, "vkEnumeratePhysicalDevices");
    PFN_vkDestroyInstance di = (PFN_vkDestroyInstance)g(inst, "vkDestroyInstance");
    uint32_t n = 1; VkPhysicalDevice pd = VK_NULL_HANDLE; VkResult r = ep ? ep(inst, &n, &pd) : VK_ERROR_INITIALIZATION_FAILED;
    if ((r == VK_SUCCESS || r == VK_INCOMPLETE) && n) vk_profile_dump_env(g, inst, api, pd);
    else LOGV("Vulkan profile: no device (TOTK_VK_PROFILE)");
    if (di) di(inst, NULL);
}
#endif
