/* GPU fault locator for the Vulkan backend (TOTK_GPU_TRACE=1, diagnostics): wraps the backend's device function
   pointers (android/gfx/vk_int.h VK_DEVICE_FUNCS) to remember, per command buffer, the pipelines bound and the last
   draws / dispatches / copies recorded, which command buffers each vkQueueSubmit2 carried, and which shader variants
   (seed names, from vk_shader.c) each pipeline was made from. When the device is lost, vk_gpu_trace_dump prints the
   last submissions' contents: with TOTK_VK_SYNC=1 TOTK_VK_SERIAL=1 TOTK_VK_CHUNK_DRAWS=1 the last one holds the
   faulting command alone. */
#define VK_NO_PROTOTYPES   /* the backend calls Vulkan through its own function pointers */
#include <vulkan/vulkan.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define D(name) extern PFN_##name name;
D(vkCreateGraphicsPipelines) D(vkCreateComputePipelines) D(vkBeginCommandBuffer) D(vkQueueSubmit2)
D(vkCmdBindPipeline) D(vkCmdDraw) D(vkCmdDrawIndexed) D(vkCmdDispatch) D(vkCmdCopyBufferToImage) D(vkCmdCopyImage)
D(vkCmdBlitImage) D(vkCmdCopyBuffer) D(vkCmdCopyImageToBuffer) D(vkCmdBeginRendering) D(vkCmdClearColorImage)
#undef D

#define NMOD 65536
#define NPIPE 32768
#define NCB 512
#define NEV 24
#define NSUB 8
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static struct { VkShaderModule m; char name[80]; } mods[NMOD];
static struct { VkPipeline p; VkShaderModule s[2]; int compute; } pipes[NPIPE];
typedef struct { char what[96]; VkPipeline p; } ev_t;
static struct { VkCommandBuffer cb; VkPipeline gfx, comp; ev_t ev[NEV]; unsigned nev; } cbs[NCB];
static struct { unsigned long long serial; VkCommandBuffer cb[16]; unsigned n; } subs[NSUB];
static unsigned long long nsub;
static int on;

static unsigned hp(const void *p, unsigned n) { unsigned long long x = (unsigned long long)(uintptr_t)p; x ^= x >> 29; x *= 0x9e3779b97f4a7c15ull; return (unsigned)(x >> 40) % n; }
static const char *mod_name(VkShaderModule m) {
    if (!m) return "-";
    for (unsigned i = hp(m, NMOD), k = 0; k < 64; k++, i = (i + 1) % NMOD) if (mods[i].m == m) return mods[i].name;
    return "?";
}
void vk_gpu_trace_module(VkShaderModule m, const char *variant) {
    if (!on || !m) return;
    pthread_mutex_lock(&mu);
    for (unsigned i = hp(m, NMOD), k = 0; k < 64; k++, i = (i + 1) % NMOD)
        if (!mods[i].m || mods[i].m == m) { mods[i].m = m; snprintf(mods[i].name, sizeof mods[i].name, "%s", variant ? variant : "?"); break; }
    pthread_mutex_unlock(&mu);
}
static void pipe_add(VkPipeline p, const VkPipelineShaderStageCreateInfo *st, uint32_t n, int compute) {
    for (unsigned i = hp(p, NPIPE), k = 0; k < 64; k++, i = (i + 1) % NPIPE)
        if (!pipes[i].p || pipes[i].p == p) {
            pipes[i].p = p; pipes[i].compute = compute; pipes[i].s[0] = pipes[i].s[1] = VK_NULL_HANDLE;
            for (uint32_t j = 0; j < n && j < 2; j++) pipes[i].s[j] = st[j].module;
            return;
        }
}
static int pipe_get(VkPipeline p) { for (unsigned i = hp(p, NPIPE), k = 0; k < 64; k++, i = (i + 1) % NPIPE) if (pipes[i].p == p) return (int)i; return -1; }
static int cb_slot(VkCommandBuffer cb) {
    for (unsigned i = hp(cb, NCB), k = 0; k < NCB; k++, i = (i + 1) % NCB) if (cbs[i].cb == cb || !cbs[i].cb) { cbs[i].cb = cb; return (int)i; }
    return (int)hp(cb, NCB);
}
static void ev(VkCommandBuffer cb, int compute, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
#include <stdarg.h>
static void ev(VkCommandBuffer cb, int compute, const char *fmt, ...) {
    pthread_mutex_lock(&mu);
    int s = cb_slot(cb); ev_t *e = &cbs[s].ev[cbs[s].nev++ % NEV];
    va_list a; va_start(a, fmt); vsnprintf(e->what, sizeof e->what, fmt, a); va_end(a);
    e->p = compute ? cbs[s].comp : cbs[s].gfx;
    pthread_mutex_unlock(&mu);
}

static PFN_vkCreateGraphicsPipelines o_cgp; static PFN_vkCreateComputePipelines o_ccp; static PFN_vkBeginCommandBuffer o_begin;
static PFN_vkQueueSubmit2 o_submit; static PFN_vkCmdBindPipeline o_bind; static PFN_vkCmdDraw o_draw; static PFN_vkCmdDrawIndexed o_drawi;
static PFN_vkCmdDispatch o_disp; static PFN_vkCmdCopyBufferToImage o_b2i; static PFN_vkCmdCopyImage o_i2i; static PFN_vkCmdBlitImage o_blit;
static PFN_vkCmdCopyBuffer o_b2b; static PFN_vkCmdCopyImageToBuffer o_i2b; static PFN_vkCmdBeginRendering o_rend; static PFN_vkCmdClearColorImage o_clear;

static VkResult VKAPI_CALL h_cgp(VkDevice d, VkPipelineCache c, uint32_t n, const VkGraphicsPipelineCreateInfo *ci, const VkAllocationCallbacks *a, VkPipeline *p) {
    VkResult r = o_cgp(d, c, n, ci, a, p);
    if (r == VK_SUCCESS) { pthread_mutex_lock(&mu); for (uint32_t i = 0; i < n; i++) pipe_add(p[i], ci[i].pStages, ci[i].stageCount, 0); pthread_mutex_unlock(&mu); }
    return r;
}
static VkResult VKAPI_CALL h_ccp(VkDevice d, VkPipelineCache c, uint32_t n, const VkComputePipelineCreateInfo *ci, const VkAllocationCallbacks *a, VkPipeline *p) {
    VkResult r = o_ccp(d, c, n, ci, a, p);
    if (r == VK_SUCCESS) { pthread_mutex_lock(&mu); for (uint32_t i = 0; i < n; i++) pipe_add(p[i], &ci[i].stage, 1, 1); pthread_mutex_unlock(&mu); }
    return r;
}
static VkResult VKAPI_CALL h_begin(VkCommandBuffer cb, const VkCommandBufferBeginInfo *bi) {
    pthread_mutex_lock(&mu); int s = cb_slot(cb); cbs[s].nev = 0; cbs[s].gfx = cbs[s].comp = VK_NULL_HANDLE; pthread_mutex_unlock(&mu);
    return o_begin(cb, bi);
}
static void VKAPI_CALL h_bind(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipeline p) {
    pthread_mutex_lock(&mu); int s = cb_slot(cb); if (bp == VK_PIPELINE_BIND_POINT_COMPUTE) cbs[s].comp = p; else cbs[s].gfx = p; pthread_mutex_unlock(&mu);
    o_bind(cb, bp, p);
}
static void VKAPI_CALL h_draw(VkCommandBuffer cb, uint32_t v, uint32_t i, uint32_t fv, uint32_t fi) { ev(cb, 0, "draw v%u i%u first%u/%u", v, i, fv, fi); o_draw(cb, v, i, fv, fi); }
static void VKAPI_CALL h_drawi(VkCommandBuffer cb, uint32_t ix, uint32_t i, uint32_t f, int32_t vo, uint32_t fi) { ev(cb, 0, "drawIndexed idx%u i%u first%u vo%d fi%u", ix, i, f, vo, fi); o_drawi(cb, ix, i, f, vo, fi); }
/* TOTK_SKIP_CS=<variant prefix>[,...] (diagnostics): dispatches of those compute shaders are not recorded. */
static int skip_dispatch(VkCommandBuffer cb) {
    const char *list = getenv("TOTK_SKIP_CS"); if (!list || !*list) return 0;
    pthread_mutex_lock(&mu); int s = cb_slot(cb), p = pipe_get(cbs[s].comp); const char *name = p < 0 ? "" : mod_name(pipes[p].s[0]); pthread_mutex_unlock(&mu);
    for (const char *q = list; *q; ) {
        const char *c = strchr(q, ','); size_t n = c ? (size_t)(c - q) : strlen(q);
        if (n && !strncmp(name, q, n)) { static int said; if (said++ < 4) fprintf(stderr, "[gpu-trace] skipping dispatch of %s (TOTK_SKIP_CS)\n", name); return 1; }
        q += n; if (*q == ',') q++;
    }
    return 0;
}
static void VKAPI_CALL h_disp(VkCommandBuffer cb, uint32_t x, uint32_t y, uint32_t z) {
    if (skip_dispatch(cb)) { ev(cb, 1, "SKIPPED dispatch %ux%ux%u", x, y, z); return; }
    ev(cb, 1, "dispatch %ux%ux%u", x, y, z); o_disp(cb, x, y, z);
}
static void VKAPI_CALL h_b2i(VkCommandBuffer cb, VkBuffer b, VkImage im, VkImageLayout l, uint32_t n, const VkBufferImageCopy *r) {
    ev(cb, 0, "copyBufferToImage n%u off%llu %ux%ux%u", n, n ? (unsigned long long)r[0].bufferOffset : 0, n ? r[0].imageExtent.width : 0, n ? r[0].imageExtent.height : 0, n ? r[0].imageExtent.depth : 0);
    o_b2i(cb, b, im, l, n, r);
}
static void VKAPI_CALL h_i2i(VkCommandBuffer cb, VkImage s, VkImageLayout sl, VkImage d, VkImageLayout dl, uint32_t n, const VkImageCopy *r) {
    ev(cb, 0, "copyImage n%u %ux%u", n, n ? r[0].extent.width : 0, n ? r[0].extent.height : 0); o_i2i(cb, s, sl, d, dl, n, r);
}
static void VKAPI_CALL h_blit(VkCommandBuffer cb, VkImage s, VkImageLayout sl, VkImage d, VkImageLayout dl, uint32_t n, const VkImageBlit *r, VkFilter f) {
    ev(cb, 0, "blit n%u", n); o_blit(cb, s, sl, d, dl, n, r, f);
}
static void VKAPI_CALL h_b2b(VkCommandBuffer cb, VkBuffer s, VkBuffer d, uint32_t n, const VkBufferCopy *r) {
    ev(cb, 0, "copyBuffer n%u size%llu", n, n ? (unsigned long long)r[0].size : 0); o_b2b(cb, s, d, n, r);
}
static void VKAPI_CALL h_i2b(VkCommandBuffer cb, VkImage s, VkImageLayout sl, VkBuffer d, uint32_t n, const VkBufferImageCopy *r) {
    ev(cb, 0, "copyImageToBuffer n%u", n); o_i2b(cb, s, sl, d, n, r);
}
static void VKAPI_CALL h_rend(VkCommandBuffer cb, const VkRenderingInfo *ri) {
    ev(cb, 0, "beginRendering %ux%u colors%u depth%d", ri->renderArea.extent.width, ri->renderArea.extent.height, ri->colorAttachmentCount, ri->pDepthAttachment != NULL);
    o_rend(cb, ri);
}
static void VKAPI_CALL h_clear(VkCommandBuffer cb, VkImage im, VkImageLayout l, const VkClearColorValue *c, uint32_t n, const VkImageSubresourceRange *r) {
    ev(cb, 0, "clearColorImage n%u", n); o_clear(cb, im, l, c, n, r);
}
static VkResult VKAPI_CALL h_submit(VkQueue q, uint32_t n, const VkSubmitInfo2 *si, VkFence f) {
    pthread_mutex_lock(&mu);
    unsigned long long serial = ++nsub; unsigned k = (unsigned)(serial % NSUB);
    subs[k].serial = serial; subs[k].n = 0;
    for (uint32_t i = 0; i < n; i++) for (uint32_t j = 0; j < si[i].commandBufferInfoCount && subs[k].n < 16; j++) subs[k].cb[subs[k].n++] = si[i].pCommandBufferInfos[j].commandBuffer;
    pthread_mutex_unlock(&mu);
    return o_submit(q, n, si, f);
}

void vk_gpu_trace_install(void) {
    const char *e = getenv("TOTK_GPU_TRACE"); if (!e || strcmp(e, "1")) return;
    on = 1;
#define H(var, orig, hook) orig = var; var = hook;
    H(vkCreateGraphicsPipelines, o_cgp, h_cgp) H(vkCreateComputePipelines, o_ccp, h_ccp) H(vkBeginCommandBuffer, o_begin, h_begin)
    H(vkQueueSubmit2, o_submit, h_submit) H(vkCmdBindPipeline, o_bind, h_bind) H(vkCmdDraw, o_draw, h_draw) H(vkCmdDrawIndexed, o_drawi, h_drawi)
    H(vkCmdDispatch, o_disp, h_disp) H(vkCmdCopyBufferToImage, o_b2i, h_b2i) H(vkCmdCopyImage, o_i2i, h_i2i) H(vkCmdBlitImage, o_blit, h_blit)
    H(vkCmdCopyBuffer, o_b2b, h_b2b) H(vkCmdCopyImageToBuffer, o_i2b, h_i2b) H(vkCmdBeginRendering, o_rend, h_rend) H(vkCmdClearColorImage, o_clear, h_clear)
#undef H
    fprintf(stderr, "[gpu-trace] on: last submissions are printed if the device is lost\n");
}

void vk_gpu_trace_dump(void) {
    if (!on) return;
    pthread_mutex_lock(&mu);
    fprintf(stderr, "[gpu-trace] device lost after %llu submissions; the last %d:\n", nsub, NSUB);
    for (unsigned long long s = nsub > NSUB ? nsub - NSUB + 1 : 1; s <= nsub; s++) {
        unsigned k = (unsigned)(s % NSUB);
        fprintf(stderr, "[gpu-trace] submission %llu: %u command buffers\n", subs[k].serial, subs[k].n);
        for (unsigned c = 0; c < subs[k].n; c++) {
            int i = cb_slot(subs[k].cb[c]); unsigned n = cbs[i].nev, first = n > NEV ? n - NEV : 0;
            fprintf(stderr, "[gpu-trace]   cb %p: %u commands%s\n", (void *)subs[k].cb[c], n, n > NEV ? " (last ones)" : "");
            for (unsigned j = first; j < n; j++) {
                ev_t *e = &cbs[i].ev[j % NEV]; int p = pipe_get(e->p);
                fprintf(stderr, "[gpu-trace]     %s | %s %s %s\n", e->what, p < 0 ? "-" : pipes[p].compute ? "cs" : "vs/fs",
                        p < 0 ? "" : mod_name(pipes[p].s[0]), p < 0 || pipes[p].compute ? "" : mod_name(pipes[p].s[1]));
            }
        }
    }
    pthread_mutex_unlock(&mu);
}
