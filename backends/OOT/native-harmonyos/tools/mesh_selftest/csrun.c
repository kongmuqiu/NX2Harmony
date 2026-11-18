/* A Vulkan compute runner for offline checks of a game compute shader on Mesa's lavapipe (mng-vk-host:lavapipe), after
   vkrun.c, with sampled textures and storage images. Used by smem_private_test.py (oot-a106).
     csrun <module.spv> <gx> <gy> <gz> <seed> <spec> ...
   spec:  <binding>:u:<bytes>[:<file>]        uniform buffer
          <binding>:s:<bytes>[:<file>]        storage buffer, written to out_<binding>.bin
          <binding>:t:<2d|3d|2da|2das>        sampled texture 16 x 16 (x 16 deep, or 4 layers), RGBA8 of pseudo-random bytes
                                              (seed, binding); 2das: a D16 array with a comparing sampler
          <binding>:i:<2d|3d>:<w>x<h>x<d>     storage image RGBA32F, cleared to zero, written to out_<binding>.bin
   A buffer starts as its file's bytes, else 0xcd bytes. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

#define CHECK(x) do { VkResult r_ = (x); if (r_) { fprintf(stderr, "%s: %d\n", #x, (int)r_); exit(1); } } while (0)
enum { MAXB = 48 };
typedef struct {
    uint32_t binding; char kind; char sub[8]; VkDeviceSize size; const char *in; uint32_t w, h, d, layers;
    VkBuffer buf; VkDeviceMemory mem; void *map; VkImage img; VkImageView view; VkSampler samp; VkFormat fmt; VkImageAspectFlags aspect;
} bind_t;
static VkDevice dev; static VkPhysicalDeviceMemoryProperties mp;

static void *read_file(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb"); if (!f) { perror(path); exit(1); }
    fseek(f, 0, SEEK_END); *n = (size_t)ftell(f); fseek(f, 0, SEEK_SET);
    void *p = malloc(*n ? *n : 1); if (fread(p, 1, *n, f) != *n) { perror(path); exit(1); } fclose(f); return p;
}
static void make_buffer(VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer *buf, VkDeviceMemory *mem, void **map) {
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = usage};
    CHECK(vkCreateBuffer(dev, &bci, NULL, buf));
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(dev, *buf, &mr);
    uint32_t t = 0; const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    while (t < mp.memoryTypeCount && !((mr.memoryTypeBits >> t & 1) && (mp.memoryTypes[t].propertyFlags & want) == want)) t++;
    VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = t};
    CHECK(vkAllocateMemory(dev, &mai, NULL, mem)); CHECK(vkBindBufferMemory(dev, *buf, *mem, 0));
    CHECK(vkMapMemory(dev, *mem, 0, VK_WHOLE_SIZE, 0, map));
}
static void make_image(bind_t *b, VkImageUsageFlags usage) {
    int three = !strcmp(b->sub, "3d");
    VkImageCreateInfo ici = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = three ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D, .format = b->fmt,
        .extent = {b->w, b->h, three ? b->d : 1}, .mipLevels = 1, .arrayLayers = b->layers, .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
    CHECK(vkCreateImage(dev, &ici, NULL, &b->img));
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(dev, b->img, &mr);
    uint32_t t = 0; while (t < mp.memoryTypeCount && !(mr.memoryTypeBits >> t & 1)) t++;
    VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = t};
    CHECK(vkAllocateMemory(dev, &mai, NULL, &b->mem)); CHECK(vkBindImageMemory(dev, b->img, b->mem, 0));
    VkImageViewCreateInfo vci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = b->img, .format = b->fmt,
        .viewType = three ? VK_IMAGE_VIEW_TYPE_3D : (b->layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D),
        .subresourceRange = {b->aspect, 0, 1, 0, b->layers}};
    CHECK(vkCreateImageView(dev, &vci, NULL, &b->view));
}
static void transition(VkCommandBuffer cb, bind_t *b, VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier im = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = 0x7fffffff & (VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT),
        .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, .oldLayout = from, .newLayout = to,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = b->img,
        .subresourceRange = {b->aspect, 0, 1, 0, b->layers}};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &im);
}

int main(int argc, char **argv) {
    if (argc < 7) { fprintf(stderr, "usage: csrun <module.spv> <gx> <gy> <gz> <seed> <spec> ...\n"); return 2; }
    uint32_t g[3] = {(uint32_t)atoi(argv[2]), (uint32_t)atoi(argv[3]), (uint32_t)atoi(argv[4])}, seed = (uint32_t)strtoul(argv[5], NULL, 0);
    static bind_t b[MAXB]; int nb = 0;
    for (int i = 6; i < argc && nb < MAXB; i++, nb++) {
        char *s = strdup(argv[i]), *t = strtok(s, ":"); b[nb].binding = (uint32_t)atoi(t);
        t = strtok(NULL, ":"); b[nb].kind = t ? *t : 0;
        t = strtok(NULL, ":");
        if (b[nb].kind == 'u' || b[nb].kind == 's') { b[nb].size = (VkDeviceSize)strtoull(t ? t : "0", NULL, 0); b[nb].in = strtok(NULL, ":"); }
        else {
            snprintf(b[nb].sub, sizeof b[nb].sub, "%s", t ? t : "2d");
            b[nb].w = b[nb].h = 16; b[nb].d = 1; b[nb].layers = 1; b[nb].fmt = VK_FORMAT_R8G8B8A8_UNORM; b[nb].aspect = VK_IMAGE_ASPECT_COLOR_BIT;
            if (!strcmp(b[nb].sub, "3d")) b[nb].d = 16;
            if (!strncmp(b[nb].sub, "2da", 3)) b[nb].layers = 4;
            if (!strcmp(b[nb].sub, "2das")) { b[nb].fmt = VK_FORMAT_D16_UNORM; b[nb].aspect = VK_IMAGE_ASPECT_DEPTH_BIT; }
            if (b[nb].kind == 'i') {
                t = strtok(NULL, ":"); b[nb].fmt = VK_FORMAT_R32G32B32A32_SFLOAT;
                if (!t || sscanf(t, "%ux%ux%u", &b[nb].w, &b[nb].h, &b[nb].d) != 3) { fprintf(stderr, "bad image spec %s\n", argv[i]); return 2; }
            }
        }
    }
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_2};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
    VkInstance inst; CHECK(vkCreateInstance(&ici, NULL, &inst));
    uint32_t npd = 8; VkPhysicalDevice pds[8]; vkEnumeratePhysicalDevices(inst, &npd, pds);
    if (!npd) { fprintf(stderr, "no Vulkan device\n"); return 1; }
    VkPhysicalDevice pd = pds[0];
    VkPhysicalDeviceProperties props; vkGetPhysicalDeviceProperties(pd, &props);
    VkPhysicalDeviceFeatures feats; vkGetPhysicalDeviceFeatures(pd, &feats);
    uint32_t nq = 8; VkQueueFamilyProperties qf[8]; vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qf);
    uint32_t q = 0; while (q < nq && !(qf[q].queueFlags & VK_QUEUE_COMPUTE_BIT)) q++;
    float prio = 1; VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = q, .queueCount = 1, .pQueuePriorities = &prio};
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci, .pEnabledFeatures = &feats};
    CHECK(vkCreateDevice(pd, &dci, NULL, &dev));
    VkQueue queue; vkGetDeviceQueue(dev, q, 0, &queue);
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    VkCommandPoolCreateInfo cpi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = q};
    VkCommandPool cp; CHECK(vkCreateCommandPool(dev, &cpi, NULL, &cp));
    VkCommandBufferAllocateInfo cbai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = cp, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 2};
    VkCommandBuffer cbs[2]; CHECK(vkAllocateCommandBuffers(dev, &cbai, cbs));
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    CHECK(vkBeginCommandBuffer(cbs[0], &bi));
    VkDescriptorSetLayoutBinding lb[MAXB]; uint32_t count[4] = {0};
    static const VkDescriptorType dt[4] = {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE};
    for (int i = 0; i < nb; i++) {
        int k = b[i].kind == 'u' ? 0 : b[i].kind == 's' ? 1 : b[i].kind == 't' ? 2 : 3;
        if (k < 2) {
            make_buffer(b[i].size, k ? VK_BUFFER_USAGE_STORAGE_BUFFER_BIT : VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, &b[i].buf, &b[i].mem, &b[i].map);
            memset(b[i].map, 0xcd, (size_t)b[i].size);
            if (b[i].in) { size_t n; void *d = read_file(b[i].in, &n); memcpy(b[i].map, d, n < b[i].size ? n : (size_t)b[i].size); free(d); }
        } else if (k == 2) {
            make_image(&b[i], VK_IMAGE_USAGE_SAMPLED_BIT);
            uint32_t bpp = b[i].fmt == VK_FORMAT_D16_UNORM ? 2 : 4; VkDeviceSize n = (VkDeviceSize)b[i].w * b[i].h * b[i].d * b[i].layers * bpp;
            VkBuffer sb; VkDeviceMemory sm; void *map; make_buffer(n, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &sb, &sm, &map);
            uint32_t x = seed * 2654435761u + b[i].binding * 40503u + 12345u;
            for (VkDeviceSize j = 0; j < n; j++) { x = x * 1664525u + 1013904223u; ((unsigned char *)map)[j] = (unsigned char)(x >> 24); }
            transition(cbs[0], &b[i], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            VkBufferImageCopy r = {.imageSubresource = {b[i].aspect, 0, 0, b[i].layers}, .imageExtent = {b[i].w, b[i].h, b[i].d}};
            vkCmdCopyBufferToImage(cbs[0], sb, b[i].img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
            transition(cbs[0], &b[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            int shadow = b[i].fmt == VK_FORMAT_D16_UNORM;
            VkSamplerCreateInfo sci = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_LINEAR, .minFilter = VK_FILTER_LINEAR,
                .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST, .compareEnable = shadow, .compareOp = VK_COMPARE_OP_LESS_OR_EQUAL};
            CHECK(vkCreateSampler(dev, &sci, NULL, &b[i].samp));
        } else {
            make_image(&b[i], VK_IMAGE_USAGE_STORAGE_BIT);
            transition(cbs[0], &b[i], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
            VkClearColorValue zero = {{0, 0, 0, 0}}; VkImageSubresourceRange all = {b[i].aspect, 0, 1, 0, 1};
            vkCmdClearColorImage(cbs[0], b[i].img, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &all);
            b[i].size = (VkDeviceSize)b[i].w * b[i].h * b[i].d * 16;
            make_buffer(b[i].size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &b[i].buf, &b[i].mem, &b[i].map);   /* (the image's memory handle is not kept) */
        }
        lb[i] = (VkDescriptorSetLayoutBinding){b[i].binding, dt[k], 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL};
        count[k]++;
    }
    CHECK(vkEndCommandBuffer(cbs[0]));
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cbs[0]};
    CHECK(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE)); CHECK(vkQueueWaitIdle(queue));
    VkDescriptorSetLayoutCreateInfo lci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = (uint32_t)nb, .pBindings = lb};
    VkDescriptorSetLayout dsl; CHECK(vkCreateDescriptorSetLayout(dev, &lci, NULL, &dsl));
    VkPipelineLayoutCreateInfo plci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &dsl};
    VkPipelineLayout layout; CHECK(vkCreatePipelineLayout(dev, &plci, NULL, &layout));
    size_t n; uint32_t *code = read_file(argv[1], &n);
    VkShaderModuleCreateInfo smci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = n, .pCode = code};
    VkShaderModule mod; CHECK(vkCreateShaderModule(dev, &smci, NULL, &mod));
    VkComputePipelineCreateInfo cpci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = mod, .pName = "main"}, .layout = layout};
    VkPipeline pipe; CHECK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpci, NULL, &pipe));
    VkDescriptorPoolSize ps[4]; uint32_t nps = 0;
    for (int k = 0; k < 4; k++) if (count[k]) ps[nps++] = (VkDescriptorPoolSize){dt[k], count[k]};
    VkDescriptorPoolCreateInfo dpci = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = nps, .pPoolSizes = ps};
    VkDescriptorPool pool; CHECK(vkCreateDescriptorPool(dev, &dpci, NULL, &pool));
    VkDescriptorSetAllocateInfo dsai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = pool, .descriptorSetCount = 1, .pSetLayouts = &dsl};
    VkDescriptorSet set; CHECK(vkAllocateDescriptorSets(dev, &dsai, &set));
    for (int i = 0; i < nb; i++) {
        VkDescriptorBufferInfo dbi = {b[i].buf, 0, VK_WHOLE_SIZE};
        VkDescriptorImageInfo dii = {b[i].samp, b[i].view, b[i].kind == 't' ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_GENERAL};
        VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = b[i].binding, .descriptorCount = 1,
            .descriptorType = lb[i].descriptorType, .pBufferInfo = &dbi, .pImageInfo = &dii};
        vkUpdateDescriptorSets(dev, 1, &w, 0, NULL);
    }
    CHECK(vkBeginCommandBuffer(cbs[1], &bi));
    vkCmdBindPipeline(cbs[1], VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    vkCmdBindDescriptorSets(cbs[1], VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, NULL);
    vkCmdDispatch(cbs[1], g[0], g[1], g[2]);
    for (int i = 0; i < nb; i++) if (b[i].kind == 'i') {
        VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT};
        vkCmdPipelineBarrier(cbs[1], VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
        VkBufferImageCopy r = {.imageSubresource = {b[i].aspect, 0, 0, 1}, .imageExtent = {b[i].w, b[i].h, !strcmp(b[i].sub, "3d") ? b[i].d : 1}};
        vkCmdCopyImageToBuffer(cbs[1], b[i].img, VK_IMAGE_LAYOUT_GENERAL, b[i].buf, 1, &r);
    }
    CHECK(vkEndCommandBuffer(cbs[1]));
    si.pCommandBuffers = &cbs[1];
    CHECK(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE)); CHECK(vkQueueWaitIdle(queue));
    for (int i = 0; i < nb; i++) if (b[i].kind == 's' || b[i].kind == 'i') {
        char name[64]; snprintf(name, sizeof name, "out_%u.bin", b[i].binding);
        FILE *f = fopen(name, "wb"); fwrite(b[i].map, 1, (size_t)b[i].size, f); fclose(f);
    }
    printf("ran %s on %s: %u x %u x %u groups\n", argv[1], props.deviceName, g[0], g[1], g[2]);
    return 0;
}
