/* A minimal Vulkan compute runner for offline checks on Mesa's lavapipe (mng-vk-host:lavapipe): one compute module, the
   buffers of its descriptor set 0, one dispatch, the buffers written back to files. Used by mesh_selftest.py to run a
   wrapped mesh kernel (tools/mesh_wrap_in_container.py) as the replay will dispatch it.
     vkrun <module.spv> <groups> <binding>:<s|u>:<bytes>[:<input file>] ...
   s: storage buffer, u: uniform buffer; a buffer starts as its input file's bytes, or 0xcd bytes (so words the shader does
   not write show). Each buffer is written to out_<binding>.bin in the current directory. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

#define CHECK(x) do { VkResult r_ = (x); if (r_) { fprintf(stderr, "%s: %d\n", #x, (int)r_); exit(1); } } while (0)
enum { MAXB = 16 };
typedef struct { uint32_t binding; int uniform; VkDeviceSize size; const char *in; VkBuffer buf; VkDeviceMemory mem; void *map; } bind_t;

static void *read_file(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb"); if (!f) { perror(path); exit(1); }
    fseek(f, 0, SEEK_END); *n = (size_t)ftell(f); fseek(f, 0, SEEK_SET);
    void *p = malloc(*n ? *n : 1); if (fread(p, 1, *n, f) != *n) { perror(path); exit(1); } fclose(f); return p;
}

int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "usage: vkrun <module.spv> <groups> <binding>:<s|u>:<bytes>[:<input file>] ...\n"); return 2; }
    uint32_t groups = (uint32_t)atoi(argv[2]); bind_t b[MAXB]; int nb = 0;
    for (int i = 3; i < argc && nb < MAXB; i++) {
        char *s = strdup(argv[i]), *t = strtok(s, ":"); b[nb].binding = (uint32_t)atoi(t);
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
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci};
    VkDevice dev; CHECK(vkCreateDevice(pd, &dci, NULL, &dev));
    VkQueue queue; vkGetDeviceQueue(dev, q, 0, &queue);
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    VkDescriptorSetLayoutBinding lb[MAXB]; VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 0}, {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 0}};
    for (int i = 0; i < nb; i++) {
        VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = b[i].size,
            .usage = b[i].uniform ? VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT : VK_BUFFER_USAGE_STORAGE_BUFFER_BIT};
        CHECK(vkCreateBuffer(dev, &bci, NULL, &b[i].buf));
        VkMemoryRequirements mr; vkGetBufferMemoryRequirements(dev, b[i].buf, &mr);
        uint32_t t = 0; const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        while (t < mp.memoryTypeCount && !((mr.memoryTypeBits >> t & 1) && (mp.memoryTypes[t].propertyFlags & want) == want)) t++;
        VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = t};
        CHECK(vkAllocateMemory(dev, &mai, NULL, &b[i].mem)); CHECK(vkBindBufferMemory(dev, b[i].buf, b[i].mem, 0));
        CHECK(vkMapMemory(dev, b[i].mem, 0, VK_WHOLE_SIZE, 0, &b[i].map));
        memset(b[i].map, 0xcd, (size_t)b[i].size);
        if (b[i].in) { size_t n; void *d = read_file(b[i].in, &n); memcpy(b[i].map, d, n < b[i].size ? n : (size_t)b[i].size); free(d); }
        lb[i] = (VkDescriptorSetLayoutBinding){b[i].binding, b[i].uniform ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL};
        ps[b[i].uniform].descriptorCount++;
    }
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
    VkDescriptorPoolSize used[2]; uint32_t nused = 0; for (int k = 0; k < 2; k++) if (ps[k].descriptorCount) used[nused++] = ps[k];
    VkDescriptorPoolCreateInfo dpci = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = nused, .pPoolSizes = used};
    VkDescriptorPool pool; CHECK(vkCreateDescriptorPool(dev, &dpci, NULL, &pool));
    VkDescriptorSetAllocateInfo dsai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = pool, .descriptorSetCount = 1, .pSetLayouts = &dsl};
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
    CHECK(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE)); CHECK(vkQueueWaitIdle(queue));
    for (int i = 0; i < nb; i++) {
        char name[64]; snprintf(name, sizeof name, "out_%u.bin", b[i].binding);
        FILE *f = fopen(name, "wb"); fwrite(b[i].map, 1, (size_t)b[i].size, f); fclose(f);
    }
    printf("ran %s on %s: %u groups\n", argv[1], props.deviceName, groups);
    return 0;
}
