/* (copy of prepared/oot/mesh-build/exp-gmem-verify/gmem_run.c for gmem_ways_test.py; added: GMEM_POOL_MASK, GMEM_FINE)
   exp-gmem: a Vulkan compute runner for lavapipe (mng-vk-host:lavapipe), after tools/mesh_selftest/vkrun.c, with
   - bufferDeviceAddress, and a memory pool table built as the replay builds it (overlay nvn_vk.c gmem_rebuild: 8192
     entries of 32 bytes {start, end, delta, hint, fine}, entry 0 empty, unused entries start = end = ~0, a hint for every
     2 MiB chunk an entry holds whole; with GMEM_FINE=1 also the fine hints of gmem_fine, oot-a111, else word 7 = 0) over
     one "pool memory" buffer whose word i is i * 2654435761 ^ 0x5bd1e995;
   - timings: module + pipeline creation, the first dispatch (lavapipe compiles there), the best of <repeat> more.
     gmem_run <module.spv> <groups> <repeat> [<binding>:<s|u>:<bytes>[:<input file>]] ... [T:<binding>:<spec file>] [S:<binding>:<n>]
   spec file: u64 pool memory bytes, then u64 triples {guest start, guest end, offset in the pool memory}, ascending.
   A buffer starts as its input file's bytes, else zero. Each storage buffer is written to out_<binding>.bin. */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <vulkan/vulkan.h>
#include <signal.h>
#include <unistd.h>
static unsigned long long g_pool_base, g_pool_size;
static void on_segv(int sig, siginfo_t *si, void *ctx) {   /* verifier: where did the shader's code fault? */
    (void)sig; (void)ctx; char b[200];
    int n = snprintf(b, sizeof b, "SIGSEGV at %#llx; pool memory %#llx .. %#llx\n", (unsigned long long)(size_t)si->si_addr, g_pool_base, g_pool_base + g_pool_size);
    if (write(2, b, (size_t)n) < 0) {}
    _exit(139);
}

#define CHECK(x) do { VkResult r_ = (x); if (r_) { fprintf(stderr, "%s: %d\n", #x, (int)r_); exit(1); } } while (0)
enum { MAXB = 24, GMEM_ENTRIES = 8192, GMEM_ENTRY = 32, GMEM_HINT_SHIFT = 21 };
typedef struct { uint32_t binding; int uniform; VkDeviceSize size; const char *in; VkBuffer buf; VkDeviceMemory mem; void *map; } bind_t;

static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
static void *read_file(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb"); if (!f) { perror(path); exit(1); }
    fseek(f, 0, SEEK_END); *n = (size_t)ftell(f); fseek(f, 0, SEEK_SET);
    void *p = malloc(*n ? *n : 1); if (fread(p, 1, *n, f) != *n) { perror(path); exit(1); } fclose(f); return p;
}
/* copied from overlay nvn_vk.c (oot-a111): keep the two alike */
/* the last entry in [0, n) whose start is at or below a (entry 0 starts at 0; starts ascend) */
static int gmem_at_or_below(const uint8_t *t, int n, uint64_t a) {
    int lo = 0, hi = n - 1;
    while (lo < hi) { int mid = (lo + hi + 1) / 2; uint64_t s; memcpy(&s, t + (size_t)mid * GMEM_ENTRY, 8); if (s <= a) lo = mid; else hi = mid - 1; }
    return lo;
}
/* oot-a111 fine hints (tools/gmemfast.py FIND_FINE; tools/mesh_selftest/gmem_run.c builds them alike): for each 2 MiB
   chunk below 2^40 that holds the start or the end of a range and has no whole-chunk hint in its slot, word 7 of the slot
   = first | (s + 1) << 16 and word 6 = the chunk's tag with entry 0, where first = the last entry starting at or below the
   chunk's start and 2^s >= (the last entry starting inside the chunk) - first + 1. The translator's helpers read words
   0..6: a tag with entry 0 sends them to their full search, as an empty slot does. A slot already holding a whole-chunk
   hint or another chunk's fine hint keeps it; a bound past the table is not written. TOTK_OHOS_GMEM_FINE=0: none. */
static void gmem_fine(uint8_t *t, int n, const uint8_t *whole, unsigned st[18]) {
    for (int k = 1; k < n; k++) {
        uint64_t r[2]; memcpy(r, t + (size_t)k * GMEM_ENTRY, 16);
        const uint64_t cs[2] = {r[0] >> GMEM_HINT_SHIFT, (r[1] - 1) >> GMEM_HINT_SHIFT};
        for (int j = 0; j < 2; j++) {
            uint64_t c = cs[j];
            if (j && c == cs[0]) continue;
            if (c >= (1ull << 19)) { st[16]++; continue; }
            uint8_t *slot = t + (size_t)(c & (GMEM_ENTRIES - 1)) * GMEM_ENTRY;
            uint32_t w6, w7; memcpy(&w6, slot + 24, 4); memcpy(&w7, slot + 28, 4);
            if (whole[(c & (GMEM_ENTRIES - 1)) >> 3] >> (c & 7) & 1) { if ((w6 >> 13) != (uint32_t)c) st[17]++; continue; }
            if (w7) { if ((w6 >> 13) != (uint32_t)c) st[17]++; continue; }
            int first = gmem_at_or_below(t, n, c << GMEM_HINT_SHIFT), last = gmem_at_or_below(t, n, ((c + 1) << GMEM_HINT_SHIFT) - 1);
            unsigned s = 0; while ((1 << s) < last - first + 1) s++;
            if (first + (1 << s) - 1 > GMEM_ENTRIES - 1) { st[16]++; continue; }
            w6 = (uint32_t)(c << 13); w7 = (uint32_t)first | (s + 1) << 16;
            memcpy(slot + 24, &w6, 4); memcpy(slot + 28, &w7, 4);
            st[s]++;
        }
    }
}
static VkPhysicalDeviceMemoryProperties mp;
static void make_buffer(VkDevice dev, bind_t *b, VkBufferUsageFlags usage, int address) {
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = b->size, .usage = usage};
    CHECK(vkCreateBuffer(dev, &bci, NULL, &b->buf));
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(dev, b->buf, &mr);
    uint32_t t = 0; const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    while (t < mp.memoryTypeCount && !((mr.memoryTypeBits >> t & 1) && (mp.memoryTypes[t].propertyFlags & want) == want)) t++;
    VkMemoryAllocateFlagsInfo fi = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT};
    VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = address ? &fi : NULL, .allocationSize = mr.size, .memoryTypeIndex = t};
    CHECK(vkAllocateMemory(dev, &mai, NULL, &b->mem)); CHECK(vkBindBufferMemory(dev, b->buf, b->mem, 0));
    CHECK(vkMapMemory(dev, b->mem, 0, VK_WHOLE_SIZE, 0, &b->map));
    { const char *fill = getenv("GMEM_FILL"); memset(b->map, fill ? (int)strtol(fill, NULL, 0) : 0, (size_t)b->size); }   /* verifier: 0xcd shows unwritten words */
    if (b->in) { size_t n; void *d = read_file(b->in, &n); memcpy(b->map, d, n < b->size ? n : (size_t)b->size); free(d); }
}

int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "usage: gmem_run <module.spv> <groups> <repeat> <binding>:<s|u>:<bytes>[:<file>] ... [T:<binding>:<spec>]\n"); return 2; }
    { struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_sigaction = on_segv; sa.sa_flags = SA_SIGINFO; sigaction(SIGSEGV, &sa, NULL); }
    uint32_t groups = (uint32_t)atoi(argv[2]); int repeat = atoi(argv[3]); bind_t b[MAXB]; int nb = 0;
    int table = -1; const char *spec = NULL; int sweep_binding = -1, sweep_n = 0;
    for (int i = 4; i < argc && nb < MAXB; i++) {
        char *s = strdup(argv[i]), *t = strtok(s, ":");
        if (*t == 'S') { sweep_binding = atoi(strtok(NULL, ":")); sweep_n = atoi(strtok(NULL, ":")); continue; }
        if (*t == 'T') { t = strtok(NULL, ":"); b[nb].binding = (uint32_t)atoi(t); b[nb].uniform = 0; b[nb].size = GMEM_ENTRIES * GMEM_ENTRY; b[nb].in = NULL;
            spec = strtok(NULL, ":"); table = nb++; continue; }
        b[nb].binding = (uint32_t)atoi(t);
        t = strtok(NULL, ":"); b[nb].uniform = t && *t == 'u';
        t = strtok(NULL, ":"); b[nb].size = (VkDeviceSize)strtoull(t ? t : "0", NULL, 0);
        t = strtok(NULL, ":"); b[nb].in = t; nb++;
    }
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_2};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
    VkInstance inst; CHECK(vkCreateInstance(&ici, NULL, &inst));
    uint32_t npd = 8; VkPhysicalDevice pds[8]; vkEnumeratePhysicalDevices(inst, &npd, pds);
    if (!npd) { fprintf(stderr, "no Vulkan device\n"); return 1; }
    VkPhysicalDevice pd = pds[0];
    VkPhysicalDeviceProperties props; vkGetPhysicalDeviceProperties(pd, &props);
    uint32_t nq = 8; VkQueueFamilyProperties qf[8]; vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qf);
    uint32_t q = 0; while (q < nq && !(qf[q].queueFlags & VK_QUEUE_COMPUTE_BIT)) q++;
    float prio = 1; VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = q, .queueCount = 1, .pQueuePriorities = &prio};
    VkPhysicalDeviceVulkan12Features f12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .bufferDeviceAddress = VK_TRUE};
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &f12, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci};
    VkDevice dev; CHECK(vkCreateDevice(pd, &dci, NULL, &dev));
    VkQueue queue; vkGetDeviceQueue(dev, q, 0, &queue);
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    bind_t pool = {0};
    if (table >= 0) {   /* the pool memory and the table over it */
        size_t n; uint64_t *sp = read_file(spec, &n); int ne = (int)((n / 8 - 1) / 3);
        pool.size = sp[0]; make_buffer(dev, &pool, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, 1);
        /* gmem_ways_test.py: GMEM_POOL_MASK keeps the pool words small (indices read from pool memory stay in the pool) */
        const char *pm = getenv("GMEM_POOL_MASK"); uint32_t mask = pm ? (uint32_t)strtoul(pm, NULL, 0) : 0xffffffffu;
        uint32_t *w = pool.map; for (size_t i = 0; i < pool.size / 4; i++) w[i] = ((uint32_t)i * 2654435761u ^ 0x5bd1e995u) & mask;
        VkBufferDeviceAddressInfo ai = {VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, NULL, pool.buf};
        uint64_t base = vkGetBufferDeviceAddress(dev, &ai);
        g_pool_base = base; g_pool_size = pool.size;
        make_buffer(dev, &b[table], VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 0);
        uint8_t *t = b[table].map;
        memset(t, 0, GMEM_ENTRY);
        for (int k = 1; k < GMEM_ENTRIES; k++) { uint8_t *e = t + (size_t)k * GMEM_ENTRY; memset(e, 0xff, 16); memset(e + 16, 0, 16); }
        int k = 1; uint64_t end = 0; static uint8_t whole[GMEM_ENTRIES / 8];
        for (int i = 0; i < ne && k < GMEM_ENTRIES; i++) {
            uint64_t lo = sp[1 + 3 * i], hi = sp[2 + 3 * i], off = sp[3 + 3 * i];
            if (lo < end || lo >= hi) { fprintf(stderr, "spec entry %d not ascending\n", i); return 1; }
            uint64_t e[3] = {lo, hi, base + off - lo}; memcpy(t + (size_t)k * GMEM_ENTRY, e, sizeof e);
            /* the first chunk at or above lo, without the wrap of lo + 2 MiB - 1 for a range in the last 2 MiB of the space */
            for (uint64_t c = (lo >> GMEM_HINT_SHIFT) + ((lo & ((1ull << GMEM_HINT_SHIFT) - 1)) != 0); ((c + 1) << GMEM_HINT_SHIFT) <= hi && c < (1ull << 19); c++) {
                uint32_t h = (uint32_t)(c << 13) | (uint32_t)k; memcpy(t + (size_t)(c & (GMEM_ENTRIES - 1)) * GMEM_ENTRY + 24, &h, 4);
                whole[(c & (GMEM_ENTRIES - 1)) >> 3] |= (uint8_t)(1u << (c & 7));
            }
            k++; end = hi;
        }
        /* GMEM_FINE=1: the fine hints of overlay nvn_vk.c gmem_fine (oot-a111), else none (the a110 table) */
        { const char *fe = getenv("GMEM_FINE"); if (fe && *fe == '1') { unsigned st[18] = {0}; gmem_fine(t, k, whole, st);
            fprintf(stderr, "fine hints:"); for (int s = 0; s < 18; s++) if (st[s]) fprintf(stderr, " %d:%u", s, st[s]); fputc('\n', stderr); } }
    }
    VkDescriptorSetLayoutBinding lb[MAXB]; VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 0}, {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 0}};
    for (int i = 0; i < nb; i++) {
        if (i != table) make_buffer(dev, &b[i], b[i].uniform ? VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT : VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 0);
        lb[i] = (VkDescriptorSetLayoutBinding){b[i].binding, b[i].uniform ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL};
        ps[b[i].uniform].descriptorCount++;
    }
    VkDescriptorSetLayoutCreateInfo lci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = (uint32_t)nb, .pBindings = lb};
    VkDescriptorSetLayout dsl; CHECK(vkCreateDescriptorSetLayout(dev, &lci, NULL, &dsl));
    VkPipelineLayoutCreateInfo plci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &dsl};
    VkPipelineLayout layout; CHECK(vkCreatePipelineLayout(dev, &plci, NULL, &layout));
    size_t n; uint32_t *code = read_file(argv[1], &n);
    double t0 = now_ms();
    VkShaderModuleCreateInfo smci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = n, .pCode = code};
    VkShaderModule mod; CHECK(vkCreateShaderModule(dev, &smci, NULL, &mod));
    VkComputePipelineCreateInfo cpci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = mod, .pName = "main"}, .layout = layout};
    VkPipeline pipe; CHECK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpci, NULL, &pipe));
    double t_pipe = now_ms() - t0;
    VkDescriptorPoolSize used[2]; uint32_t nused = 0; for (int k = 0; k < 2; k++) if (ps[k].descriptorCount) used[nused++] = ps[k];
    VkDescriptorPoolCreateInfo dpci = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = nused, .pPoolSizes = used};
    VkDescriptorPool dpool; CHECK(vkCreateDescriptorPool(dev, &dpci, NULL, &dpool));
    VkDescriptorSetAllocateInfo dsai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = dpool, .descriptorSetCount = 1, .pSetLayouts = &dsl};
    VkDescriptorSet set; CHECK(vkAllocateDescriptorSets(dev, &dsai, &set));
    for (int i = 0; i < nb; i++) {
        VkDescriptorBufferInfo bi = {b[i].buf, 0, VK_WHOLE_SIZE};
        VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = b[i].binding, .descriptorCount = 1,
            .descriptorType = b[i].uniform ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &bi};
        vkUpdateDescriptorSets(dev, 1, &w, 0, NULL);
    }
    VkCommandPoolCreateInfo cpi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = q};
    VkCommandPool cp; CHECK(vkCreateCommandPool(dev, &cpi, NULL, &cp));
    VkCommandBufferAllocateInfo cbai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = cp, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    VkCommandBuffer cb; CHECK(vkAllocateCommandBuffers(dev, &cbai, &cb));
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    CHECK(vkBeginCommandBuffer(cb, &bi));
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, NULL);
    vkCmdDispatch(cb, groups, 1, 1);
    CHECK(vkEndCommandBuffer(cb));
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb};
    t0 = now_ms();
    CHECK(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE)); CHECK(vkQueueWaitIdle(queue));
    double t_first = now_ms() - t0, t_best = -1;
    for (int r = 0; r < repeat; r++) {
        t0 = now_ms();
        CHECK(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE)); CHECK(vkQueueWaitIdle(queue));
        double t = now_ms() - t0; if (t_best < 0 || t < t_best) t_best = t;
    }
    /* S:<binding>:<n>: n more dispatches, word 0 of that buffer (the control block's first workgroup) = 1 .. n, the buffer
       itself restored from its input file before each; every storage buffer is appended to its out file after each dispatch */
    for (int i = 0; i < nb; i++) {
        if (b[i].uniform || i == table) continue;
        char name[64]; snprintf(name, sizeof name, "out_%u.bin", b[i].binding);
        FILE *f = fopen(name, "wb"); fwrite(b[i].map, 1, (size_t)b[i].size, f); fclose(f);
    }
    for (int k = 1; k <= sweep_n; k++) {
        for (int i = 0; i < nb; i++) if ((int)b[i].binding == sweep_binding) {
            memset(b[i].map, 0, (size_t)b[i].size);
            if (b[i].in) { size_t n2; void *d = read_file(b[i].in, &n2); memcpy(b[i].map, d, n2 < b[i].size ? n2 : (size_t)b[i].size); free(d); }
            *(uint32_t *)b[i].map = (uint32_t)k;
        }
        CHECK(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE)); CHECK(vkQueueWaitIdle(queue));
        for (int i = 0; i < nb; i++) {
            if (b[i].uniform || i == table) continue;
            char name[64]; snprintf(name, sizeof name, "out_%u.bin", b[i].binding);
            FILE *f = fopen(name, "ab"); fwrite(b[i].map, 1, (size_t)b[i].size, f); fclose(f);
        }
    }
    printf("device %s; module %zu bytes; pipeline_ms %.2f first_dispatch_ms %.2f best_dispatch_ms %.3f groups %u\n", props.deviceName, n, t_pipe, t_first, t_best, groups);
    return 0;
}
